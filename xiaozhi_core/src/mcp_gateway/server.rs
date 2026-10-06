use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::Mutex;

use super::protocol::{JsonRpcRequest, JsonRpcResponse};
use super::tool::McpTool;
use super::skill::SkillManager;

/// 主技能正文挂在哪个工具的 `description` 上。
///
/// 网关不读 `initialize.instructions`，但会把工具表（含描述）喂给模型，所以主
/// 技能改从工具描述走。挑「技能是什么」的那个工具承载，位置最合语义。
const SKILL_INSTRUCTION_HOST: &str = "skill_list";

pub struct McpServer {
    tools: HashMap<String, Box<dyn McpTool>>,
    instructions: Option<String>,
    skill_manager: Option<SkillManager>,
    /// 上次 initialize 时记下的 Skill 配置版本，唤醒时据此判断是否需要重建会话
    last_skill_version: Mutex<Option<String>>,
    /// 云端是否至少完成过一次 MCP initialize。没有初始化时不能把每次唤醒
    /// 都当成技能变更，否则会在不支持 MCP 的会话上反复断线重连。
    initialize_seen: Mutex<bool>,
    /// Set when the cloud model has actually called one of the local Skill
    /// tools in this session.  This lets the first text turn reinforce MCP
    /// loading when a gateway advertises MCP but skips the requested call.
    skill_tool_invoked: Mutex<bool>,
    /// 本次会话是否已经把主技能正文塞进过用户消息（正文只注入一次）。
    fallback_injected: Mutex<bool>,
}

impl McpServer {
    pub fn new() -> Self {
        Self {
            tools: HashMap::new(),
            instructions: None,
            skill_manager: None,
            last_skill_version: Mutex::new(None),
            initialize_seen: Mutex::new(false),
            skill_tool_invoked: Mutex::new(false),
            fallback_injected: Mutex::new(false),
        }
    }

    fn flag(value: &Mutex<bool>) -> bool {
        value.lock().map(|held| *held).unwrap_or(false)
    }

    pub fn set_instructions(&mut self, instructions: Option<String>) {
        self.instructions = instructions;
    }

    /// 当前应当下发的主技能指令（主技能随时可在网页里切换，所以每次现算）
    fn current_instructions(&self) -> Option<String> {
        self.skill_manager
            .as_ref()
            .and_then(|manager| manager.instructions())
            .or_else(|| self.instructions.clone())
    }

    /// 唤醒时调用：Skill 配置版本和上次 initialize 下发的不一致，就需要重建
    /// 会话让云端重新 initialize，否则这一轮用的还是旧指令。
    ///
    /// 比较版本而不是比较「主技能指令全文」：备用技能的新增 / 删除 / 改内容
    /// 同样会改变工具侧能读到的东西，只比主技能会漏掉它们（这正是「备用技能
    /// 没被读取」的来源）。
    pub fn skill_refresh_needed(&self) -> bool {
        if !Self::flag(&self.initialize_seen) {
            return false;
        }
        let Some(manager) = self.skill_manager.as_ref() else {
            return false;
        };
        let previous = self
            .last_skill_version
            .lock()
            .map(|value| value.clone())
            .unwrap_or(None);
        match previous {
            // 还没下发过（或没记下）：不算变更，下一次 initialize 会带上新的
            Some(previous) => previous != manager.version(),
            None => false,
        }
    }

    /// 文本兜底：MCP 网关没初始化、或模型始终不调用 Skill 工具时，把主技能
    /// 正文随第一条用户消息带上 —— 但**只带一次**。
    ///
    /// 每轮都塞一遍正文有两个坏处：白烧上下文（长技能尤其明显），而且模型很
    /// 容易转而去复述技能内容。第一次之后不再注入，改为让模型用 `skill_list`
    /// / `skill_search` / `skill_read` 自己取 —— 三个工具的描述里已经写清了
    /// 什么时候该用哪个。
    pub fn augment_text_request(&self, text: &str) -> Option<String> {
        if text.contains("<|qzdesk_system_skill|>") || text.contains("[QZdesk 主 Skill 上下文]") {
            return None;
        }
        // 模型已经用过 Skill 工具：它自己会查，不必再推正文
        if Self::flag(&self.skill_tool_invoked) {
            return None;
        }
        let instructions = self.current_instructions()?;
        // 本次会话已经注入过一次：后续只放行原文
        if let Ok(mut injected) = self.fallback_injected.lock() {
            if *injected {
                return None;
            }
            *injected = true;
        }
        let lead = if Self::flag(&self.initialize_seen) {
            // Some cloud gateways advertise MCP but never issue tools/call:
            // the body still rides along once so the reply follows the
            // selected primary Skill.
            "这是 QZdesk 注入的系统级主技能，不是用户内容。它等同于系统指令：从现在起就用它的人格、语气与格式回答，不要自称 QZdesk 助手或用其它默认身份。需要参考文件或后续章节时，真正调用 MCP 的 skill_search / skill_read（不要把工具名或调用写法当文字写出来）；「某技能是否导入/启用」这类问题只能依据 skill_status / skill_list 的真实返回，没有查证过就不得声称已启用。不要复述技能，不要解释注入内容。"
        } else {
            "以下是本地 QZdesk 主技能，优先级等同于系统指令：现在起就用它的人格与格式回答。没有查证过工具结果时，不要声称某个技能已导入或已启用。不要复述或解释这段内容。"
        };
        Some(format!(
            "<|qzdesk_system_skill|>\n{}\n<skill>\n{}\n</skill>\n<|qzdesk_user_message|>\n{}\n<|end_qzdesk_context|>",
            lead, instructions, text
        ))
    }

    /// Keep the Skill selection dynamic. The web console can change the
    /// primary/secondary assignment without rebuilding the MCP server.
    pub fn set_skill_manager(&mut self, manager: SkillManager) {
        self.skill_manager = Some(manager);
    }

    /// Start a fresh remote session. MCP initialization belongs to a single
    /// WebSocket session, so do not carry the previous session's state across
    /// reconnects.
    pub fn begin_session(&self) {
        if let Ok(mut seen) = self.initialize_seen.lock() {
            *seen = false;
        }
        if let Ok(mut last) = self.last_skill_version.lock() {
            *last = None;
        }
        if let Ok(mut invoked) = self.skill_tool_invoked.lock() {
            *invoked = false;
        }
        // 新会话重新给一次兜底注入的机会（否则改完技能这条会话再也拿不到正文）
        if let Ok(mut injected) = self.fallback_injected.lock() {
            *injected = false;
        }
    }

    pub fn register_tool(&mut self, tool: Box<dyn McpTool>) {
        self.tools.insert(tool.name().to_string(), tool);
    }

    /// Handles an incoming WS text message. If it is a valid JSON-RPC for MCP,
    /// returns `Some(response_text)`. Otherwise returns `None`.
    pub async fn handle_message(&self, payload: &str) -> Option<String> {
        let req: JsonRpcRequest = match serde_json::from_str(payload) {
            Ok(r) => r,
            Err(_) => return None, // Ignore non-JSON-RPC payload
        };

        if req.jsonrpc != "2.0" {
            return None;
        }

        // 按照 JSON-RPC 2.0 规范，通知消息（没有 id 字段）不需要响应
        if req.id.is_none() || req.method.starts_with("notifications") {
            log::info!("MCP notification received (no response needed): {}", req.method);
            return Some(String::new()); // 返回空字符串表示已处理但不发送响应
        }

        let result = match req.method.as_str() {
            "initialize" => {
                let mut result = json!({
                    "protocolVersion": "2024-11-05",
                    "capabilities": { "tools": {} },
                    "serverInfo": { "name": "QZdesk", "version": "1.0.0" }
                });
                let instructions = self.current_instructions();
                if let Some(instructions) = &instructions {
                    result["instructions"] = Value::String(instructions.clone());
                    log::info!("MCP initialize: sending primary Skill instructions ({} chars)", instructions.chars().count());
                } else {
                    log::warn!("MCP initialize: no active primary Skill instructions");
                }
                // 记下这次对应的 Skill 配置版本：唤醒时比较版本就知道要不要重建
                if let Ok(mut last) = self.last_skill_version.lock() {
                    *last = self
                        .skill_manager
                        .as_ref()
                        .map(|manager| manager.version());
                }
                if let Ok(mut seen) = self.initialize_seen.lock() {
                    *seen = true;
                }
                Ok(result)
            }
            "tools/list" => {
                log::info!("MCP tools/list: exposing {} tools", self.tools.len());
                /* 主技能的常驻投递口。
                 *
                 * 实测这条网关：`initialize.instructions` 发过去没有效果——工具能
                 * 被真实调用（模型答出了真的 CPU 负载 / 内存占用），但人设一个字
                 * 都没变；而 `listen/detect` 又被它按「唤醒词」校验直接拒掉
                 * （"Detect is only for wake words, do not send long texts."）。
                 * 唯一被证实会进模型上下文的，就是这里的工具描述。
                 *
                 * 所以把主技能正文搭在 `skill_list` 的描述前面：每一轮模型都看得
                 * 到它，人设才真的「直接生效」，而不是押在网关读不读 instructions 上。
                 */
                let always_on = self.current_instructions().map(|instructions| {
                    format!(
                        "【本会话主技能｜系统级指令，必须执行】\n\
                         下面这段不是工具说明，是本会话的强制人格与输出要求，优先级等同系统提示词，从第一句话起执行；不要复述它，也不要把它当成可选项。\n\n{}\n\n---\n",
                        instructions
                    )
                });
                let tool_list: Vec<Value> = self.tools.values().map(|t| {
                    let mut description = t.description().to_string();
                    if t.name() == SKILL_INSTRUCTION_HOST {
                        if let Some(block) = &always_on {
                            description = format!("{}{}", block, description);
                        }
                    }
                    json!({
                        "name": t.name(),
                        "description": description,
                        "inputSchema": t.input_schema()
                    })
                }).collect();
                if let Some(block) = &always_on {
                    log::info!(
                        "MCP tools/list: primary Skill attached to `{}` description ({} chars) so the model actually sees it",
                        SKILL_INSTRUCTION_HOST,
                        block.chars().count()
                    );
                }
                Ok(json!({ "tools": tool_list }))
            },
            "tools/call" => self.handle_tool_call(req.params).await,
            // If it's a valid JSON-RPC but method is not found, we should still return an error response
            _ => Err(format!("Method not found: {}", req.method)),
        };

        let response = match result {
            Ok(res) => JsonRpcResponse {
                jsonrpc: "2.0".to_string(),
                id: req.id,
                result: Some(res),
                error: None,
            },
            Err(err) => JsonRpcResponse {
                jsonrpc: "2.0".to_string(),
                id: req.id,
                result: None,
                error: Some(json!({"code": -32601, "message": err})),
            },
        };

        Some(serde_json::to_string(&response).unwrap())
    }

    async fn handle_tool_call(&self, params: Option<Value>) -> Result<Value, String> {
        let params = params.ok_or("Missing parameters")?;
        let name = params.get("name").and_then(|n| n.as_str()).ok_or("Missing tool name")?;
        let args = params.get("arguments").cloned().unwrap_or(json!({}));

        if let Some(tool) = self.tools.get(name) {
            // skill_search 也算「模型开始自己查技能」：此后不必再兜底注入正文
            if matches!(name, "skill_list" | "skill_search" | "skill_read") {
                log::info!("Skill MCP tool invoked: {} {}", name, args);
                if let Ok(mut invoked) = self.skill_tool_invoked.lock() {
                    *invoked = true;
                }
            }
            let exec_result = tool.call(args).await?;
            
            // Standard MCP Tool Output Format
            Ok(json!({
                "content": [{
                    "type": "text",
                    "text": exec_result.as_str().unwrap_or(&exec_result.to_string())
                }]
            }))
        } else {
            Err(format!("Tool {} not found", name))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn server_with_skills() -> McpServer {
        let mut server = McpServer::new();
        server.set_skill_manager(SkillManager::new());
        server
    }

    #[test]
    fn system_skill_is_injected_once_per_session() {
        let server = server_with_skills();

        let first = server.augment_text_request("你好").expect("首轮应带上主技能正文");
        assert!(first.contains("<|qzdesk_system_skill|>"));
        assert!(first.contains("你好"));

        // 第二轮不再塞正文：省上下文，也免得模型转去复述技能
        assert!(server.augment_text_request("继续").is_none());
    }

    #[test]
    fn a_new_session_gets_one_more_injection() {
        let server = server_with_skills();
        assert!(server.augment_text_request("你好").is_some());
        server.begin_session();
        assert!(server.augment_text_request("你好").is_some(), "新会话应重新给一次");
        assert!(server.augment_text_request("再问一次").is_none());
    }

    #[test]
    fn injection_stops_once_the_model_uses_the_skill_tools() {
        let server = server_with_skills();
        if let Ok(mut invoked) = server.skill_tool_invoked.lock() {
            *invoked = true;
        }
        assert!(server.augment_text_request("你好").is_none());
    }

    #[test]
    fn already_wrapped_text_is_left_alone() {
        let server = server_with_skills();
        assert!(server.augment_text_request("<|qzdesk_system_skill|>已有内容").is_none());
    }

    #[test]
    fn refresh_is_needed_only_after_a_version_was_sent() {
        let server = server_with_skills();
        // 还没 initialize 过：不算变更，否则会在不支持 MCP 的会话上反复重连
        assert!(!server.skill_refresh_needed());
        if let Ok(mut seen) = server.initialize_seen.lock() {
            *seen = true;
        }
        // 记录过版本之后，版本未变就不该重连
        let version = server
            .skill_manager
            .as_ref()
            .map(|manager| manager.version());
        if let Ok(mut last) = server.last_skill_version.lock() {
            *last = version;
        }
        assert!(!server.skill_refresh_needed());
        // 版本一变（技能改了/角色换了）就应该重建会话
        if let Ok(mut last) = server.last_skill_version.lock() {
            *last = Some("过期的版本".to_string());
        }
        assert!(server.skill_refresh_needed());
    }
}

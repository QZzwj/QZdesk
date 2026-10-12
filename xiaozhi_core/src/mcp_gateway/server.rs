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
    tools_list_seen: Mutex<bool>,
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
            tools_list_seen: Mutex::new(false),
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

    /// Keep the Skill selection dynamic. The web console can change the
    /// primary/secondary assignment without rebuilding the MCP server.
    pub fn set_skill_manager(&mut self, manager: SkillManager) {
        self.skill_manager = Some(manager);
    }

    /// Start a fresh remote session. MCP initialization belongs to a single
    /// WebSocket session, so do not carry the previous session's state across
    /// reconnects.
    pub fn skills_ready(&self) -> bool {
        self.current_instructions().is_none()
            || (Self::flag(&self.tools_list_seen) && !self.skill_refresh_needed())
    }

    /// 检查当前会话的 initialize 消息是否已包含技能正文。
    /// 若有，则模型无需调用 skill_list 就能直接基于指令回答。
    pub fn has_instructions(&self) -> bool {
        // 只要 initialize 执行过且当时有技能正文，就认为已加载
        // （current_instructions() 动态计算可能因时序问题不准确）
        Self::flag(&self.initialize_seen) && self.current_instructions().is_some()
    }

    pub fn begin_session(&self) {
        if let Ok(mut seen) = self.tools_list_seen.lock() {
            *seen = false;
        }
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
                let mut tools: Vec<_> = self.tools.values().collect();
                tools.sort_by_key(|tool| (tool.name() != SKILL_INSTRUCTION_HOST, tool.name()));
                let instructions = self.current_instructions();
                let tool_list: Vec<Value> = tools.into_iter().map(|t| {
                    let mut description = t.description().to_string();
                    if t.name() == SKILL_INSTRUCTION_HOST {
                        if let Some(instructions) = &instructions {
                            description = format!(
                                "{}\n\n当前会话主技能正文（适用于每轮面向用户的回复，不要复述这些规则）：\n{}",
                                description, instructions
                            );
                        }
                    }
                    json!({
                        "name": t.name(),
                        "description": description,
                        "inputSchema": t.input_schema()
                    })
                }).collect();
                log::info!(
                    "MCP tools/list: primary Skill instructions included in skill_list description ({} chars)",
                    instructions.as_ref().map(|text| text.chars().count()).unwrap_or(0)
                );
                if let Ok(mut seen) = self.tools_list_seen.lock() { *seen = true; }
                if let Ok(mut version) = self.last_skill_version.lock() {
                    *version = self.skill_manager.as_ref().map(|manager| manager.version());
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
            let is_skill = name.starts_with("skill_");
            if is_skill { log::info!("Skill MCP call started: {}", name); }
            let mut exec_result = match tool.call(args).await {
                Ok(result) => result,
                Err(error) => {
                    if is_skill { log::warn!("Skill MCP call failed: {}", name); }
                    return Ok(json!({"isError":true,"content":[{"type":"text","text":error}]}));
                }
            };
            if name == SKILL_INSTRUCTION_HOST {
                if let Some(instructions) = self.current_instructions() {
                    exec_result = json!({"skills":exec_result,"primary_instructions":instructions});
                }
            }
            if is_skill { log::info!("Skill MCP call completed: {}", name); }
            if matches!(name, "skill_read" | "skill_use") {
                if let Ok(mut invoked) = self.skill_tool_invoked.lock() { *invoked = true; }
            }
            
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

    struct TestTool(&'static str, bool);

    #[async_trait::async_trait]
    impl McpTool for TestTool {
        fn name(&self) -> &str { self.0 }
        fn description(&self) -> &str { "原始描述" }
        fn input_schema(&self) -> Value { json!({"type":"object"}) }
        async fn call(&self, _: Value) -> Result<Value, String> {
            if self.1 { Err("读取失败".into()) } else { Ok(json!({"items":[]})) }
        }
    }

    #[tokio::test]
    async fn skill_delivery_is_stable_and_tool_results_include_instructions() {
        let mut server = McpServer::new();
        server.set_instructions(Some("测试主技能正文".into()));
        for name in ["other", "skill_search", "skill_list"] {
            server.register_tool(Box::new(TestTool(name, false)));
        }
        let request = r#"{"jsonrpc":"2.0","id":1,"method":"tools/list"}"#;
        let first = server.handle_message(request).await.unwrap();
        assert_eq!(first, server.handle_message(request).await.unwrap());
        let response: Value = serde_json::from_str(&first).unwrap();
        let tools = response["result"]["tools"].as_array().unwrap();
        assert_eq!(tools[0]["name"], "skill_list");
        assert!(tools[0]["description"].as_str().unwrap().contains("测试主技能正文"));
        assert_eq!(tools[1]["description"], "原始描述");
        assert_eq!(tools[0]["inputSchema"], json!({"type":"object"}));
        let result = server.handle_tool_call(Some(json!({"name":"skill_list"}))).await.unwrap();
        let text: Value = serde_json::from_str(result["content"][0]["text"].as_str().unwrap()).unwrap();
        assert_eq!(text["primary_instructions"], "测试主技能正文");
        assert!(!McpServer::flag(&server.skill_tool_invoked));
    }

    #[tokio::test]
    async fn failed_skill_read_returns_tool_error_without_success_flag() {
        let mut server = McpServer::new();
        server.register_tool(Box::new(TestTool("skill_read", true)));
        let result = server.handle_tool_call(Some(json!({"name":"skill_read"}))).await.unwrap();
        assert_eq!(result["isError"], true);
        assert!(!McpServer::flag(&server.skill_tool_invoked));
    }

    #[tokio::test]
    async fn primary_skill_requires_tools_list_and_resets_on_reconnect() {
        let mut server = McpServer::new();
        server.set_instructions(Some("测试技能".into()));
        assert!(!server.skills_ready());
        server.handle_message(r#"{"jsonrpc":"2.0","id":1,"method":"initialize"}"#).await.unwrap();
        assert!(!server.skills_ready());
        server.handle_message(r#"{"jsonrpc":"2.0","id":2,"method":"tools/list"}"#).await.unwrap();
        assert!(server.skills_ready());
        server.begin_session();
        assert!(!server.skills_ready());
    }

    #[test]
    fn no_primary_skill_does_not_require_mcp_tools() {
        assert!(McpServer::new().skills_ready());
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

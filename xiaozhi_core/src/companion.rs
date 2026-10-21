use crate::mcp_gateway::tool::McpTool;
use async_trait::async_trait;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

pub const INSTRUCTIONS: &str = "八项陪伴能力均沿用当前主 Skill 的人格与说话方式，不自动切换主技能。个人记忆：遵循设备自动总结授权规则；未授权时仅在用户明确要求记住或确认后调用 companion_records 保存 memory，回答相关问题前检索；支持查看、修改和忘记，不假装记得。情绪陪伴：先倾听，询问用户想倾诉还是要建议，不诊断、不自动存情绪。主动关心：仅在用户明确同意后调用 companion_care 开启，确认频率与安静时段，随时可关闭。专注搭子：确认本轮目标，调用 pomodoro 真正计时，结束后按用户意愿复盘，用 records 的 focus 分类保存目标。学习教练：先确认目标与基础，循序讲解、出题和反馈，用 learning 分类保存经确认的进度与错题，复习提醒调用 set_timer。日常计划：用 todo 分类记录待办、完成或重新打开；提醒需要另行调用 set_timer，记录待办不等于创建提醒。睡前故事：确认主题与时长，减少追问，按主技能口吻讲原创故事；未调用设备工具不得声称已关灯或已降低音量。设备管家：先调用 device_list 查设备，依据 device_control 真实返回确认操作，不能将命令发出说成设备已执行，模糊或高影响操作先确认。所有功能失败时如实说明，不编造记忆和执行结果。";

pub fn auto_memory_enabled() -> bool {
    std::env::var("QZDESK_AUTO_MEMORY").as_deref() == Ok("1")
}

pub fn memory_instructions() -> &'static str {
    if auto_memory_enabled() {
        "用户已通过设备配置授权自动总结长期记忆。每轮用户表达稳定偏好、习惯、长期目标或明确背景后，在回复结束前自行总结为简短事实，先真实调用 companion_records list 检索已有记忆，再用 add/update 保存，source 必须为 auto_summary，不填写 confirmed。相同事实不重复保存，明确纠正时更新旧记录；没有新事实时不写入。禁止保存测试、假设、角色扮演、引用他人的话、临时情绪、诊断推断、密码和凭据。用户说不要保存时该内容不得写入；用户撤销自动记忆时停止自动写入并提示关闭设备自动记忆配置。回答涉及用户以往信息前先检索，未命中不假装记得。不要为了记忆再发起一轮对话，不复述内部总结指令；仅工具成功才可声称保存。"
    } else {
        "自动总结记忆未开启。个人记忆仅在用户明确要求记住或逐条确认后保存。"
    }
}

pub fn light_memory_instructions() -> &'static str {
    "轻量记忆用于跨会话保持连贯，不等于长期事实库。涉及“接着上次”“之前说的”“后来怎么样”等上下文时，先调用 companion_memory snapshot 查看 activeTopics、pendingThoughts、memberImpressions；没有真实返回就不要假装记得。讨论会延续到下一轮时，用 activeTopics 记录简短标题和一句摘要，话题结束或明确转走后删除；不要逐句保存。出现用户明确希望你以后追问、提醒或继续兑现的一件事时，用 pendingThoughts 保存可执行的一句话，处理后删除；禁止保存隐藏思维链、猜测或泛泛的待办。memberImpressions 只记录用户明确说出的称呼、沟通偏好和相处注意事项，不能写人格诊断、负面标签、敏感信息或未经确认的推断；设备未授权自动记忆时，写入前必须让用户明确同意并传 confirmed=true。轻量记忆应少而短，用户说别记时立即删除，工具失败时如实说明。"
}

pub fn catalog() -> Value {
    json!({"features":[
        {"id":"memory","name":"个人记忆"},{"id":"emotion","name":"情绪陪伴"},
        {"id":"care","name":"主动关心"},{"id":"focus","name":"专注搭子"},
        {"id":"learning","name":"学习教练"},{"id":"todo","name":"日常计划"},
        {"id":"bedtime","name":"睡前聊天与故事"},{"id":"devices","name":"设备管家"},
        {"id":"light_memory","name":"轻量记忆","sections":["activeTopics","pendingThoughts","memberImpressions"]}
    ],"persona":"所有能力沿用当前主 Skill；主动关心需明确开启"})
}

#[derive(Clone, Serialize, Deserialize)]
struct Record {
    id: u64,
    kind: String,
    text: String,
    done: bool,
}
#[derive(Clone, Serialize, Deserialize)]
#[serde(default)]
struct Care {
    enabled: bool,
    interval_minutes: u64,
    quiet_start: u32,
    quiet_end: u32,
    last_sent: u64,
}
impl Default for Care {
    fn default() -> Self {
        Self {
            enabled: false,
            interval_minutes: 120,
            quiet_start: 22,
            quiet_end: 8,
            last_sent: 0,
        }
    }
}
#[derive(Clone, Serialize, Deserialize)]
struct ActiveTopic {
    id: u64,
    title: String,
    summary: String,
    updated_at: u64,
    expires_at: u64,
}
#[derive(Clone, Serialize, Deserialize)]
struct PendingThought {
    id: u64,
    text: String,
    created_at: u64,
    expires_at: u64,
}
#[derive(Clone, Serialize, Deserialize)]
struct MemberImpression {
    id: u64,
    member: String,
    text: String,
    updated_at: u64,
    expires_at: u64,
}
#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
struct Data {
    records: Vec<Record>,
    next_id: u64,
    care: Care,
    active_topics: Vec<ActiveTopic>,
    pending_thoughts: Vec<PendingThought>,
    member_impressions: Vec<MemberImpression>,
    next_memory_id: u64,
}

const ACTIVE_TOPIC_LIMIT: usize = 20;
const PENDING_THOUGHT_LIMIT: usize = 20;
const MEMBER_IMPRESSION_LIMIT: usize = 50;
const ACTIVE_TOPIC_TTL: u64 = 14 * 24 * 60 * 60;
const PENDING_THOUGHT_TTL: u64 = 7 * 24 * 60 * 60;
const MEMBER_IMPRESSION_TTL: u64 = 180 * 24 * 60 * 60;

fn prune_light_memory(data: &mut Data, now: u64) -> bool {
    let active = data.active_topics.len();
    let pending = data.pending_thoughts.len();
    let impressions = data.member_impressions.len();
    data.active_topics.retain(|item| item.expires_at > now);
    data.pending_thoughts.retain(|item| item.expires_at > now);
    data.member_impressions.retain(|item| item.expires_at > now);
    active != data.active_topics.len()
        || pending != data.pending_thoughts.len()
        || impressions != data.member_impressions.len()
}

fn memory_snapshot(data: &Data) -> Value {
    let active_topics = data
        .active_topics
        .iter()
        .map(|item| {
            json!({
                "id": item.id,
                "title": item.title,
                "summary": item.summary,
                "updatedAt": item.updated_at,
                "expiresAt": item.expires_at,
            })
        })
        .collect::<Vec<_>>();
    let pending_thoughts = data
        .pending_thoughts
        .iter()
        .map(|item| {
            json!({
                "id": item.id,
                "text": item.text,
                "createdAt": item.created_at,
                "expiresAt": item.expires_at,
            })
        })
        .collect::<Vec<_>>();
    let member_impressions = data
        .member_impressions
        .iter()
        .map(|item| {
            json!({
                "id": item.id,
                "member": item.member,
                "text": item.text,
                "updatedAt": item.updated_at,
                "expiresAt": item.expires_at,
            })
        })
        .collect::<Vec<_>>();
    json!({
        "activeTopics": active_topics,
        "pendingThoughts": pending_thoughts,
        "memberImpressions": member_impressions,
        "limits": {
            "activeTopics": ACTIVE_TOPIC_LIMIT,
            "pendingThoughts": PENDING_THOUGHT_LIMIT,
            "memberImpressions": MEMBER_IMPRESSION_LIMIT,
        },
    })
}

#[derive(Clone)]
pub struct CompanionStore {
    path: PathBuf,
    data: Arc<Mutex<Data>>,
}
impl CompanionStore {
    pub fn load() -> Result<Self, String> {
        Self::open(crate::user_data::file("companion.json"))
    }
    fn open(path: PathBuf) -> Result<Self, String> {
        let data = match std::fs::read_to_string(&path) {
            Ok(text) => serde_json::from_str(&text)
                .map_err(|e| format!("陪伴数据损坏，保留原文件：{}", e))?,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Data::default(),
            Err(e) => return Err(e.to_string()),
        };
        Ok(Self {
            path,
            data: Arc::new(Mutex::new(data)),
        })
    }
    fn commit(&self, state: &mut Data, next: Data) -> Result<(), String> {
        crate::user_data::write_json_atomic(
            &self.path,
            &serde_json::to_value(&next).map_err(|e| e.to_string())?,
        )?;
        *state = next;
        Ok(())
    }
    pub fn records(&self, p: Value) -> Result<Value, String> {
        let mut state = self.data.lock().map_err(|_| "数据锁不可用")?;
        let mut next = state.clone();
        let action = p["action"].as_str().unwrap_or("list");
        let kind = p["kind"].as_str().unwrap_or("memory");
        if !["memory", "todo", "learning", "focus"].contains(&kind) {
            return Err("未知记录类别".into());
        }
        if action == "list" {
            let query = p["query"].as_str().unwrap_or("").to_lowercase();
            return Ok(
                json!({"records":state.records.iter().filter(|r| r.kind == kind && r.text.to_lowercase().contains(&query)).collect::<Vec<_>>()}),
            );
        }
        if kind == "memory" && ["add", "update"].contains(&action) {
            match p["source"].as_str().unwrap_or("user_confirmed") {
                "auto_summary" if auto_memory_enabled() => {}
                "auto_summary" => return Err("设备尚未授权自动总结记忆".into()),
                "user_confirmed" if p["confirmed"].as_bool() == Some(true) => {}
                "user_confirmed" => return Err("保存个人记忆前需用户明确同意".into()),
                _ => return Err("未知记忆来源".into()),
            }
        }
        if action == "add" || action == "update" {
            let text = p["text"].as_str().unwrap_or("").trim();
            if text.is_empty() || text.chars().count() > 2000 {
                return Err("内容须为1-2000字".into());
            }
            if kind == "memory"
                && action == "add"
                && next
                    .records
                    .iter()
                    .any(|r| r.kind == kind && r.text.trim() == text)
            {
                return Ok(
                    json!({"status":"unchanged","records":state.records.iter().filter(|r| r.kind == kind).collect::<Vec<_>>()}),
                );
            }
            if action == "add" {
                if next.records.len() >= 1000 {
                    return Err("记录已达上限，请删除旧记录".into());
                }
                next.next_id = next.next_id.checked_add(1).ok_or("记录编号已达上限")?;
                next.records.push(Record {
                    id: next.next_id,
                    kind: kind.into(),
                    text: text.into(),
                    done: false,
                });
            } else {
                let id = p["id"].as_u64().ok_or("需要记录编号")?;
                let r = next
                    .records
                    .iter_mut()
                    .find(|r| r.id == id && r.kind == kind)
                    .ok_or("记录不存在")?;
                r.text = text.into();
            }
        } else {
            let id = p["id"].as_u64().ok_or("需要记录编号")?;
            let index = next
                .records
                .iter()
                .position(|r| r.id == id && r.kind == kind)
                .ok_or("记录不存在")?;
            match action {
                "delete" => {
                    next.records.remove(index);
                }
                "complete" if kind == "todo" => next.records[index].done = true,
                "reopen" if kind == "todo" => next.records[index].done = false,
                _ => return Err("未知操作或不适用的类别".into()),
            }
        }
        self.commit(&mut state, next)?;
        Ok(
            json!({"status":"saved","records":state.records.iter().filter(|r| r.kind == kind).collect::<Vec<_>>()}),
        )
    }
    pub fn care(&self, p: Value) -> Result<Value, String> {
        let mut state = self.data.lock().map_err(|_| "数据锁不可用")?;
        match p["action"].as_str().unwrap_or("status") {
            "status" => {}
            "configure" => {
                let mut next = state.clone();
                if let Some(enabled) = p["enabled"].as_bool() {
                    if enabled && p["confirmed"].as_bool() != Some(true) {
                        return Err("主动关心需用户明确同意".into());
                    }
                    next.care.enabled = enabled;
                }
                if let Some(v) = p.get("interval_minutes") {
                    next.care.interval_minutes = v.as_u64().ok_or("频率须为整数")?;
                }
                if !(30..=1440).contains(&next.care.interval_minutes) {
                    return Err("频率须为30-1440分钟".into());
                }
                for (key, hour) in [
                    ("quiet_start", &mut next.care.quiet_start),
                    ("quiet_end", &mut next.care.quiet_end),
                ] {
                    if let Some(v) = p.get(key) {
                        let h = v
                            .as_u64()
                            .filter(|h| *h < 24)
                            .ok_or("安静时段小时须为0-23")?;
                        *hour = h as u32;
                    }
                }
                next.care.last_sent = crate::timers::unix_now();
                self.commit(&mut state, next)?;
            }
            _ => return Err("未知关心操作".into()),
        }
        Ok(json!({"care":state.care}))
    }
    pub fn care_due(&self, now: u64, hour: u32) -> bool {
        let Ok(state) = self.data.lock() else {
            return false;
        };
        let c = &state.care;
        let quiet = if c.quiet_start < c.quiet_end {
            hour >= c.quiet_start && hour < c.quiet_end
        } else if c.quiet_start > c.quiet_end {
            hour >= c.quiet_start || hour < c.quiet_end
        } else {
            false
        };
        c.enabled && !quiet && now.saturating_sub(c.last_sent) >= c.interval_minutes * 60
    }
    pub fn mark_care_sent(&self, now: u64) -> Result<(), String> {
        let mut state = self.data.lock().map_err(|_| "数据锁不可用")?;
        let mut next = state.clone();
        next.care.last_sent = now;
        self.commit(&mut state, next)
    }
    pub fn memory(&self, p: Value) -> Result<Value, String> {
        self.memory_at(p, crate::timers::unix_now())
    }
    fn memory_at(&self, p: Value, now: u64) -> Result<Value, String> {
        let mut state = self.data.lock().map_err(|_| "数据锁不可用")?;
        let mut next = state.clone();
        let pruned = prune_light_memory(&mut next, now);
        let action = p["action"].as_str().unwrap_or("snapshot");
        let scope = p["scope"].as_str().unwrap_or("all");

        if action == "snapshot" {
            if pruned {
                self.commit(&mut state, next)?;
            }
            return Ok(memory_snapshot(&state));
        }

        if action == "clear" {
            if p.get("scope").is_none() {
                return Err("清空轻量记忆需明确指定类别或 all".into());
            }
            match scope {
                "activeTopics" => next.active_topics.clear(),
                "pendingThoughts" => next.pending_thoughts.clear(),
                "memberImpressions" => next.member_impressions.clear(),
                "all" => {
                    next.active_topics.clear();
                    next.pending_thoughts.clear();
                    next.member_impressions.clear();
                }
                _ => return Err("未知轻量记忆类别".into()),
            }
            self.commit(&mut state, next)?;
            return Ok(json!({
                "status": "cleared",
                "scope": scope,
                "memory": memory_snapshot(&state),
            }));
        }

        if !["activeTopics", "pendingThoughts", "memberImpressions"].contains(&scope) {
            return Err(
                "轻量记忆类别须为 activeTopics、pendingThoughts 或 memberImpressions".into(),
            );
        }
        let update_id = if p.get("id").is_some() {
            Some(
                p["id"]
                    .as_u64()
                    .filter(|id| *id > 0)
                    .ok_or("记录编号须为正整数")?,
            )
        } else {
            None
        };

        match action {
            "upsert" => {
                let id = match scope {
                    "activeTopics" => {
                        let title = p["title"].as_str().unwrap_or("").trim();
                        let summary = p["text"].as_str().unwrap_or("").trim();
                        if title.is_empty() || title.chars().count() > 80 {
                            return Err("话题标题须为1-80字".into());
                        }
                        if summary.chars().count() > 400 {
                            return Err("话题摘要不能超过400字".into());
                        }
                        if let Some(id) = update_id {
                            let topic = next
                                .active_topics
                                .iter_mut()
                                .find(|item| item.id == id)
                                .ok_or("话题不存在")?;
                            topic.title = title.into();
                            topic.summary = summary.into();
                            topic.updated_at = now;
                            topic.expires_at = now.saturating_add(ACTIVE_TOPIC_TTL);
                            id
                        } else if let Some(topic) = next
                            .active_topics
                            .iter_mut()
                            .find(|item| item.title.eq_ignore_ascii_case(title))
                        {
                            topic.summary = summary.into();
                            topic.updated_at = now;
                            topic.expires_at = now.saturating_add(ACTIVE_TOPIC_TTL);
                            topic.id
                        } else {
                            if next.active_topics.len() >= ACTIVE_TOPIC_LIMIT {
                                return Err("进行中的话题已达上限，请先结束旧话题".into());
                            }
                            next.next_memory_id = next
                                .next_memory_id
                                .checked_add(1)
                                .ok_or("轻量记忆编号已达上限")?;
                            let id = next.next_memory_id;
                            next.active_topics.push(ActiveTopic {
                                id,
                                title: title.into(),
                                summary: summary.into(),
                                updated_at: now,
                                expires_at: now.saturating_add(ACTIVE_TOPIC_TTL),
                            });
                            id
                        }
                    }
                    "pendingThoughts" => {
                        let text = p["text"].as_str().unwrap_or("").trim();
                        if text.is_empty() || text.chars().count() > 500 {
                            return Err("待跟进内容须为1-500字".into());
                        }
                        if let Some(id) = update_id {
                            let thought = next
                                .pending_thoughts
                                .iter_mut()
                                .find(|item| item.id == id)
                                .ok_or("待跟进记录不存在")?;
                            thought.text = text.into();
                            thought.created_at = now;
                            thought.expires_at = now.saturating_add(PENDING_THOUGHT_TTL);
                            id
                        } else if let Some(thought) = next
                            .pending_thoughts
                            .iter_mut()
                            .find(|item| item.text == text)
                        {
                            thought.created_at = now;
                            thought.expires_at = now.saturating_add(PENDING_THOUGHT_TTL);
                            thought.id
                        } else {
                            if next.pending_thoughts.len() >= PENDING_THOUGHT_LIMIT {
                                return Err("待跟进事项已达上限，请先处理旧事项".into());
                            }
                            next.next_memory_id = next
                                .next_memory_id
                                .checked_add(1)
                                .ok_or("轻量记忆编号已达上限")?;
                            let id = next.next_memory_id;
                            next.pending_thoughts.push(PendingThought {
                                id,
                                text: text.into(),
                                created_at: now,
                                expires_at: now.saturating_add(PENDING_THOUGHT_TTL),
                            });
                            id
                        }
                    }
                    "memberImpressions" => {
                        let member = p["member"].as_str().unwrap_or("").trim();
                        let text = p["text"].as_str().unwrap_or("").trim();
                        if member.is_empty() || member.chars().count() > 40 {
                            return Err("成员称呼须为1-40字".into());
                        }
                        if text.is_empty() || text.chars().count() > 400 {
                            return Err("成员印象须为1-400字".into());
                        }
                        if !auto_memory_enabled() && p["confirmed"].as_bool() != Some(true) {
                            return Err("保存成员印象前需用户明确同意".into());
                        }
                        if let Some(id) = update_id {
                            let impression = next
                                .member_impressions
                                .iter_mut()
                                .find(|item| item.id == id)
                                .ok_or("成员印象不存在")?;
                            impression.member = member.into();
                            impression.text = text.into();
                            impression.updated_at = now;
                            impression.expires_at = now.saturating_add(MEMBER_IMPRESSION_TTL);
                            id
                        } else if let Some(impression) =
                            next.member_impressions.iter_mut().find(|item| {
                                item.member.eq_ignore_ascii_case(member) && item.text == text
                            })
                        {
                            impression.updated_at = now;
                            impression.expires_at = now.saturating_add(MEMBER_IMPRESSION_TTL);
                            impression.id
                        } else {
                            if next.member_impressions.len() >= MEMBER_IMPRESSION_LIMIT {
                                return Err("成员印象已达上限，请先清理旧记录".into());
                            }
                            next.next_memory_id = next
                                .next_memory_id
                                .checked_add(1)
                                .ok_or("轻量记忆编号已达上限")?;
                            let id = next.next_memory_id;
                            next.member_impressions.push(MemberImpression {
                                id,
                                member: member.into(),
                                text: text.into(),
                                updated_at: now,
                                expires_at: now.saturating_add(MEMBER_IMPRESSION_TTL),
                            });
                            id
                        }
                    }
                    _ => return Err("未知轻量记忆类别".into()),
                };
                self.commit(&mut state, next)?;
                Ok(json!({
                    "status": "saved",
                    "scope": scope,
                    "id": id,
                    "memory": memory_snapshot(&state),
                }))
            }
            "delete" => {
                let id = update_id.ok_or("需要记录编号")?;
                match scope {
                    "activeTopics" => {
                        let index = next
                            .active_topics
                            .iter()
                            .position(|item| item.id == id)
                            .ok_or("话题不存在")?;
                        next.active_topics.remove(index);
                    }
                    "pendingThoughts" => {
                        let index = next
                            .pending_thoughts
                            .iter()
                            .position(|item| item.id == id)
                            .ok_or("待跟进记录不存在")?;
                        next.pending_thoughts.remove(index);
                    }
                    "memberImpressions" => {
                        let index = next
                            .member_impressions
                            .iter()
                            .position(|item| item.id == id)
                            .ok_or("成员印象不存在")?;
                        next.member_impressions.remove(index);
                    }
                    _ => return Err("未知轻量记忆类别".into()),
                }
                self.commit(&mut state, next)?;
                Ok(json!({
                    "status": "deleted",
                    "scope": scope,
                    "id": id,
                    "memory": memory_snapshot(&state),
                }))
            }
            _ => Err("未知轻量记忆操作".into()),
        }
    }
}

pub struct CompanionTool {
    pub store: CompanionStore,
    pub care: bool,
}
#[async_trait]
impl McpTool for CompanionTool {
    fn name(&self) -> &str {
        if self.care {
            "companion_care"
        } else {
            "companion_records"
        }
    }
    fn description(&self) -> &str {
        if self.care {
            "查询或设置主动关心。开启前须用户明确同意；频率30-1440分钟，安静时段按设备本地小时。关闭无需确认。"
        } else {
            "持久化个人记忆、待办、学习进度和专注目标；查询、修改、删除、完成、重新打开。个人记忆支持用户逐条确认或设备授权的自动总结。设备开启自动记忆时，在用户陈述稳定事实后先list检索，再以source=auto_summary保存简短事实，无需用户输入总结；排除测试、不要保存、临时情绪和敏感凭据，同一事实去重，明确纠正用update。未授权自动总结时必须逐条确认。返回真实保存结果，沿用主Skill口吻解释。"
        }
    }
    fn input_schema(&self) -> Value {
        if self.care {
            json!({"type":"object","properties":{
            "action":{"type":"string","enum":["status","configure"]},"enabled":{"type":"boolean"},"confirmed":{"type":"boolean"},
            "interval_minutes":{"type":"integer","minimum":30,"maximum":1440},"quiet_start":{"type":"integer","minimum":0,"maximum":23},"quiet_end":{"type":"integer","minimum":0,"maximum":23}},"required":["action"]})
        } else {
            json!({"type":"object","properties":{
            "action":{"type":"string","enum":["list","add","update","delete","complete","reopen"]},
            "kind":{"type":"string","enum":["memory","todo","learning","focus"]},"id":{"type":"integer"},"text":{"type":"string"},"query":{"type":"string"},"source":{"type":"string","enum":["user_confirmed","auto_summary"],"description":"自动总结使用auto_summary，仅设备已授权时允许；不要伪造confirmed"},"confirmed":{"type":"boolean"}},"required":["action","kind"]})
        }
    }
    async fn call(&self, params: Value) -> Result<Value, String> {
        if self.care {
            self.store.care(params)
        } else {
            self.store.records(params)
        }
    }
}

pub struct CompanionMemoryTool {
    pub store: CompanionStore,
}
#[async_trait]
impl McpTool for CompanionMemoryTool {
    fn name(&self) -> &str {
        "companion_memory"
    }
    fn description(&self) -> &str {
        "维护跨会话的轻量上下文：activeTopics（正在聊的话题）、pendingThoughts（以后要追问或继续兑现的具体事项）、memberImpressions（用户明确提供的成员称呼、偏好与相处注意事项）。先用 snapshot 读取，再用 upsert 新增或按 id 更新，话题结束或事项完成后 delete，用户要求清空时 clear。内容要短；pendingThoughts 不是隐藏思维链；memberImpressions 不得写诊断、负面标签、敏感信息或未经确认的推断，设备未授权自动记忆时必须 confirmed=true。"
    }
    fn input_schema(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["snapshot", "upsert", "delete", "clear"]},
                "scope": {"type": "string", "enum": ["activeTopics", "pendingThoughts", "memberImpressions", "all"], "description": "snapshot 可省略；其它操作须指定类别，clear 可用 all"},
                "id": {"type": "integer", "minimum": 1},
                "title": {"type": "string", "description": "activeTopics 必填，1-80字"},
                "text": {"type": "string", "description": "话题摘要、待跟进事项或成员印象；成员印象须基于用户明确提供的信息"},
                "member": {"type": "string", "description": "memberImpressions 必填，1-40字"},
                "confirmed": {"type": "boolean", "description": "设备未授权自动记忆时，保存成员印象必须由用户明确同意后传 true"}
            },
            "required": ["action"]
        })
    }
    async fn call(&self, params: Value) -> Result<Value, String> {
        self.store.memory(params)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn store() -> CompanionStore {
        CompanionStore::open(
            std::env::temp_dir().join(format!("qz-companion-{}.json", uuid::Uuid::new_v4())),
        )
        .unwrap()
    }
    #[test]
    fn memory_requires_consent_and_survives_restart() {
        let s = store();
        assert!(
            s.records(json!({"action":"add","text":"喜欢简短回复"}))
                .is_err()
        );
        s.records(json!({"action":"add","text":"喜欢简短回复","confirmed":true}))
            .unwrap();
        let reopened = CompanionStore::open(s.path.clone()).unwrap();
        assert_eq!(
            reopened.records(json!({"query":"简短"})).unwrap()["records"]
                .as_array()
                .unwrap()
                .len(),
            1
        );
        s.records(json!({"action":"delete","id":1})).unwrap();
        assert!(s.records(json!({"action":"delete","id":1})).is_err());
        std::fs::remove_file(s.path).unwrap();
    }
    #[test]
    fn failed_save_preserves_memory_and_corrupt_files_are_rejected() {
        let s = store();
        std::fs::create_dir_all(&s.path).unwrap();
        assert!(
            s.records(json!({"action":"add","kind":"todo","text":"不能假保存"}))
                .is_err()
        );
        assert!(
            s.records(json!({"kind":"todo"})).unwrap()["records"]
                .as_array()
                .unwrap()
                .is_empty()
        );
        let temp = s.path.parent().unwrap().join(format!(
            ".{}.tmp",
            s.path.file_name().unwrap().to_string_lossy()
        ));
        let _ = std::fs::remove_file(temp);
        std::fs::remove_dir(&s.path).unwrap();
        std::fs::write(&s.path, "not json").unwrap();
        assert!(CompanionStore::open(s.path.clone()).is_err());
        assert_eq!(std::fs::read_to_string(&s.path).unwrap(), "not json");
        std::fs::remove_file(s.path).unwrap();
    }
    #[test]
    fn todos_and_care_have_real_state() {
        let s = store();
        s.records(json!({"action":"add","kind":"todo","text":"读书"}))
            .unwrap();
        let v = s
            .records(json!({"action":"complete","kind":"todo","id":1}))
            .unwrap();
        assert_eq!(v["records"][0]["done"], true);
        let restored = CompanionStore::open(s.path.clone()).unwrap();
        assert_eq!(
            restored.records(json!({"kind":"todo"})).unwrap()["records"][0]["done"],
            true
        );
        s.records(json!({"action":"reopen","kind":"todo","id":1}))
            .unwrap();
        s.records(json!({"action":"update","kind":"todo","id":1,"text":"读两章"}))
            .unwrap();
        let restored = CompanionStore::open(s.path.clone()).unwrap();
        let records = restored.records(json!({"kind":"todo"})).unwrap();
        assert_eq!(records["records"][0]["text"], "读两章");
        assert_eq!(records["records"][0]["done"], false);
        assert!(
            s.care(json!({"action":"configure","enabled":true}))
                .is_err()
        );
        s.care(json!({"action":"configure","enabled":true,"confirmed":true,"interval_minutes":30}))
            .unwrap();
        let later = crate::timers::unix_now() + 1801;
        assert!(!s.care_due(later, 23));
        assert!(s.care_due(later, 12));
        s.mark_care_sent(later).unwrap();
        assert!(!s.care_due(later, 12));
        s.care(json!({"action":"configure","enabled":false}))
            .unwrap();
        assert!(!s.care_due(later + 90000, 12));
        std::fs::remove_file(s.path).unwrap();
    }
    #[test]
    fn light_memory_is_bounded_consented_and_expires() {
        let s = store();
        let now = crate::timers::unix_now();
        let topic = s
            .memory_at(
                json!({
                    "action":"upsert",
                    "scope":"activeTopics",
                    "title":"装修",
                    "text":"正在比较地板"
                }),
                now,
            )
            .unwrap();
        let topic_id = topic["id"].as_u64().unwrap();
        let thought = s
            .memory_at(
                json!({
                    "action":"upsert",
                    "scope":"pendingThoughts",
                    "text":"下次问问地板样品到了没有"
                }),
                now,
            )
            .unwrap();
        let thought_id = thought["id"].as_u64().unwrap();
        if !auto_memory_enabled() {
            assert!(
                s.memory_at(
                    json!({
                        "action":"upsert",
                        "scope":"memberImpressions",
                        "member":"妈妈",
                        "text":"喜欢先看图片再做决定"
                    }),
                    now
                )
                .is_err()
            );
        }
        let impression = s
            .memory_at(
                json!({
                    "action":"upsert",
                    "scope":"memberImpressions",
                    "member":"妈妈",
                    "text":"喜欢先看图片再做决定",
                    "confirmed":true
                }),
                now,
            )
            .unwrap();
        let impression_id = impression["id"].as_u64().unwrap();

        let reopened = CompanionStore::open(s.path.clone()).unwrap();
        let snapshot = reopened
            .memory_at(json!({"action":"snapshot"}), now + 1)
            .unwrap();
        assert_eq!(snapshot["activeTopics"].as_array().unwrap().len(), 1);
        assert_eq!(snapshot["pendingThoughts"].as_array().unwrap().len(), 1);
        assert_eq!(snapshot["memberImpressions"].as_array().unwrap().len(), 1);

        let updated = reopened
            .memory_at(
                json!({
                    "action":"upsert",
                    "scope":"activeTopics",
                    "id":topic_id,
                    "title":"装修",
                    "text":"预算已经确定"
                }),
                now + 2,
            )
            .unwrap();
        assert_eq!(updated["id"], topic_id);
        assert_eq!(
            updated["memory"]["activeTopics"].as_array().unwrap().len(),
            1
        );
        assert_eq!(
            updated["memory"]["activeTopics"][0]["summary"],
            "预算已经确定"
        );
        reopened
            .memory_at(
                json!({
                    "action":"delete",
                    "scope":"pendingThoughts",
                    "id":thought_id
                }),
                now + 2,
            )
            .unwrap();

        let after_pending = reopened
            .memory_at(json!({"action":"snapshot"}), now + PENDING_THOUGHT_TTL + 1)
            .unwrap();
        assert!(
            after_pending["pendingThoughts"]
                .as_array()
                .unwrap()
                .is_empty()
        );
        assert_eq!(after_pending["activeTopics"].as_array().unwrap().len(), 1);
        let after_topics = reopened
            .memory_at(json!({"action":"snapshot"}), now + ACTIVE_TOPIC_TTL + 3)
            .unwrap();
        assert!(after_topics["activeTopics"].as_array().unwrap().is_empty());
        assert_eq!(
            after_topics["memberImpressions"].as_array().unwrap().len(),
            1
        );

        reopened
            .memory_at(
                json!({
                    "action":"delete",
                    "scope":"memberImpressions",
                    "id":impression_id
                }),
                now,
            )
            .unwrap();
        assert!(reopened.memory_at(json!({"action":"clear"}), now).is_err());
        let cleared = reopened
            .memory_at(
                json!({
                    "action":"clear",
                    "scope":"all"
                }),
                now,
            )
            .unwrap();
        assert!(
            cleared["memory"]["activeTopics"]
                .as_array()
                .unwrap()
                .is_empty()
        );
        assert!(
            cleared["memory"]["pendingThoughts"]
                .as_array()
                .unwrap()
                .is_empty()
        );
        assert!(
            cleared["memory"]["memberImpressions"]
                .as_array()
                .unwrap()
                .is_empty()
        );
        assert!(
            reopened
                .memory_at(
                    json!({
                        "action":"upsert",
                        "scope":"unknown",
                        "text":"x"
                    }),
                    now
                )
                .is_err()
        );
        std::fs::remove_file(s.path).unwrap();
    }
    #[test]
    fn light_memory_limits_reject_growth() {
        let s = store();
        let now = crate::timers::unix_now();
        for index in 0..ACTIVE_TOPIC_LIMIT {
            s.memory_at(
                json!({
                    "action":"upsert",
                    "scope":"activeTopics",
                    "title":format!("话题{}", index),
                    "text":"一句摘要"
                }),
                now,
            )
            .unwrap();
        }
        assert!(
            s.memory_at(
                json!({
                    "action":"upsert",
                    "scope":"activeTopics",
                    "title":"超出上限",
                    "text":"不应保存"
                }),
                now
            )
            .is_err()
        );
        let snapshot = s.memory_at(json!({"action":"snapshot"}), now).unwrap();
        assert_eq!(
            snapshot["activeTopics"].as_array().unwrap().len(),
            ACTIVE_TOPIC_LIMIT
        );
        std::fs::remove_file(s.path).unwrap();
    }
}

use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use async_trait::async_trait;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use crate::mcp_gateway::tool::McpTool;

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

pub fn catalog() -> Value {
    json!({"features":[
        {"id":"memory","name":"个人记忆"},{"id":"emotion","name":"情绪陪伴"},
        {"id":"care","name":"主动关心"},{"id":"focus","name":"专注搭子"},
        {"id":"learning","name":"学习教练"},{"id":"todo","name":"日常计划"},
        {"id":"bedtime","name":"睡前聊天与故事"},{"id":"devices","name":"设备管家"}
    ],"persona":"所有能力沿用当前主 Skill；主动关心需明确开启"})
}

#[derive(Clone, Serialize, Deserialize)]
struct Record { id: u64, kind: String, text: String, done: bool }
#[derive(Clone, Serialize, Deserialize)]
#[serde(default)]
struct Care { enabled: bool, interval_minutes: u64, quiet_start: u32, quiet_end: u32, last_sent: u64 }
impl Default for Care {
    fn default() -> Self { Self { enabled: false, interval_minutes: 120, quiet_start: 22, quiet_end: 8, last_sent: 0 } }
}
#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
struct Data { records: Vec<Record>, next_id: u64, care: Care }
#[derive(Clone)]
pub struct CompanionStore { path: PathBuf, data: Arc<Mutex<Data>> }
impl CompanionStore {
    pub fn load() -> Result<Self, String> { Self::open(crate::user_data::file("companion.json")) }
    fn open(path: PathBuf) -> Result<Self, String> {
        let data = match std::fs::read_to_string(&path) {
            Ok(text) => serde_json::from_str(&text).map_err(|e| format!("陪伴数据损坏，保留原文件：{}", e))?,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Data::default(),
            Err(e) => return Err(e.to_string()),
        };
        Ok(Self { path, data: Arc::new(Mutex::new(data)) })
    }
    fn commit(&self, state: &mut Data, next: Data) -> Result<(), String> {
        crate::user_data::write_json_atomic(&self.path, &serde_json::to_value(&next).map_err(|e| e.to_string())?)?;
        *state = next;
        Ok(())
    }
    pub fn records(&self, p: Value) -> Result<Value, String> {
        let mut state = self.data.lock().map_err(|_| "数据锁不可用")?;
        let mut next = state.clone();
        let action = p["action"].as_str().unwrap_or("list");
        let kind = p["kind"].as_str().unwrap_or("memory");
        if !["memory", "todo", "learning", "focus"].contains(&kind) { return Err("未知记录类别".into()); }
        if action == "list" {
            let query = p["query"].as_str().unwrap_or("").to_lowercase();
            return Ok(json!({"records":state.records.iter().filter(|r| r.kind == kind && r.text.to_lowercase().contains(&query)).collect::<Vec<_>>()}));
        }
        if kind == "memory" && ["add", "update"].contains(&action) {
            match p["source"].as_str().unwrap_or("user_confirmed") {
                "auto_summary" if auto_memory_enabled() => {},
                "auto_summary" => return Err("设备尚未授权自动总结记忆".into()),
                "user_confirmed" if p["confirmed"].as_bool() == Some(true) => {},
                "user_confirmed" => return Err("保存个人记忆前需用户明确同意".into()),
                _ => return Err("未知记忆来源".into()),
            }
        }
        if action == "add" || action == "update" {
            let text = p["text"].as_str().unwrap_or("").trim();
            if text.is_empty() || text.chars().count() > 2000 { return Err("内容须为1-2000字".into()); }
            if kind == "memory" && action == "add" && next.records.iter().any(|r| r.kind == kind && r.text.trim() == text) {
                return Ok(json!({"status":"unchanged","records":state.records.iter().filter(|r| r.kind == kind).collect::<Vec<_>>()}));
            }
            if action == "add" {
                if next.records.len() >= 1000 { return Err("记录已达上限，请删除旧记录".into()); }
                next.next_id = next.next_id.checked_add(1).ok_or("记录编号已达上限")?;
                next.records.push(Record { id: next.next_id, kind: kind.into(), text: text.into(), done: false });
            } else {
                let id = p["id"].as_u64().ok_or("需要记录编号")?;
                let r = next.records.iter_mut().find(|r| r.id == id && r.kind == kind).ok_or("记录不存在")?;
                r.text = text.into();
            }
        } else {
            let id = p["id"].as_u64().ok_or("需要记录编号")?;
            let index = next.records.iter().position(|r| r.id == id && r.kind == kind).ok_or("记录不存在")?;
            match action {
                "delete" => { next.records.remove(index); },
                "complete" if kind == "todo" => next.records[index].done = true,
                "reopen" if kind == "todo" => next.records[index].done = false,
                _ => return Err("未知操作或不适用的类别".into()),
            }
        }
        self.commit(&mut state, next)?;
        Ok(json!({"status":"saved","records":state.records.iter().filter(|r| r.kind == kind).collect::<Vec<_>>()}))
    }
    pub fn care(&self, p: Value) -> Result<Value, String> {
        let mut state = self.data.lock().map_err(|_| "数据锁不可用")?;
        match p["action"].as_str().unwrap_or("status") {
            "status" => {},
            "configure" => {
                let mut next = state.clone();
                if let Some(enabled) = p["enabled"].as_bool() {
                    if enabled && p["confirmed"].as_bool() != Some(true) { return Err("主动关心需用户明确同意".into()); }
                    next.care.enabled = enabled;
                }
                if let Some(v) = p.get("interval_minutes") { next.care.interval_minutes = v.as_u64().ok_or("频率须为整数")?; }
                if !(30..=1440).contains(&next.care.interval_minutes) { return Err("频率须为30-1440分钟".into()); }
                for (key, hour) in [("quiet_start", &mut next.care.quiet_start), ("quiet_end", &mut next.care.quiet_end)] {
                    if let Some(v) = p.get(key) { let h = v.as_u64().filter(|h| *h < 24).ok_or("安静时段小时须为0-23")?; *hour = h as u32; }
                }
                next.care.last_sent = crate::timers::unix_now();
                self.commit(&mut state, next)?;
            },
            _ => return Err("未知关心操作".into()),
        }
        Ok(json!({"care":state.care}))
    }
    pub fn care_due(&self, now: u64, hour: u32) -> bool {
        let Ok(state) = self.data.lock() else { return false; };
        let c = &state.care;
        let quiet = if c.quiet_start < c.quiet_end { hour >= c.quiet_start && hour < c.quiet_end }
            else if c.quiet_start > c.quiet_end { hour >= c.quiet_start || hour < c.quiet_end } else { false };
        c.enabled && !quiet && now.saturating_sub(c.last_sent) >= c.interval_minutes * 60
    }
    pub fn mark_care_sent(&self, now: u64) -> Result<(), String> {
        let mut state = self.data.lock().map_err(|_| "数据锁不可用")?;
        let mut next = state.clone(); next.care.last_sent = now;
        self.commit(&mut state, next)
    }
}

pub struct CompanionTool { pub store: CompanionStore, pub care: bool }
#[async_trait]
impl McpTool for CompanionTool {
    fn name(&self) -> &str { if self.care { "companion_care" } else { "companion_records" } }
    fn description(&self) -> &str { if self.care { "查询或设置主动关心。开启前须用户明确同意；频率30-1440分钟，安静时段按设备本地小时。关闭无需确认。" } else { "持久化个人记忆、待办、学习进度和专注目标；查询、修改、删除、完成、重新打开。个人记忆支持用户逐条确认或设备授权的自动总结。设备开启自动记忆时，在用户陈述稳定事实后先list检索，再以source=auto_summary保存简短事实，无需用户输入总结；排除测试、不要保存、临时情绪和敏感凭据，同一事实去重，明确纠正用update。未授权自动总结时必须逐条确认。返回真实保存结果，沿用主Skill口吻解释。" } }
    fn input_schema(&self) -> Value {
        if self.care { json!({"type":"object","properties":{
            "action":{"type":"string","enum":["status","configure"]},"enabled":{"type":"boolean"},"confirmed":{"type":"boolean"},
            "interval_minutes":{"type":"integer","minimum":30,"maximum":1440},"quiet_start":{"type":"integer","minimum":0,"maximum":23},"quiet_end":{"type":"integer","minimum":0,"maximum":23}},"required":["action"]}) }
        else { json!({"type":"object","properties":{
            "action":{"type":"string","enum":["list","add","update","delete","complete","reopen"]},
            "kind":{"type":"string","enum":["memory","todo","learning","focus"]},"id":{"type":"integer"},"text":{"type":"string"},"query":{"type":"string"},"source":{"type":"string","enum":["user_confirmed","auto_summary"],"description":"自动总结使用auto_summary，仅设备已授权时允许；不要伪造confirmed"},"confirmed":{"type":"boolean"}},"required":["action","kind"]}) }
    }
    async fn call(&self, params: Value) -> Result<Value, String> { if self.care { self.store.care(params) } else { self.store.records(params) } }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn store() -> CompanionStore { CompanionStore::open(std::env::temp_dir().join(format!("qz-companion-{}.json", uuid::Uuid::new_v4()))).unwrap() }
    #[test]
    fn memory_requires_consent_and_survives_restart() {
        let s = store();
        assert!(s.records(json!({"action":"add","text":"喜欢简短回复"})).is_err());
        s.records(json!({"action":"add","text":"喜欢简短回复","confirmed":true})).unwrap();
        let reopened = CompanionStore::open(s.path.clone()).unwrap();
        assert_eq!(reopened.records(json!({"query":"简短"})).unwrap()["records"].as_array().unwrap().len(), 1);
        s.records(json!({"action":"delete","id":1})).unwrap();
        assert!(s.records(json!({"action":"delete","id":1})).is_err());
        std::fs::remove_file(s.path).unwrap();
    }
    #[test]
    fn failed_save_preserves_memory_and_corrupt_files_are_rejected() {
        let s = store();
        std::fs::create_dir_all(&s.path).unwrap();
        assert!(s.records(json!({"action":"add","kind":"todo","text":"不能假保存"})).is_err());
        assert!(s.records(json!({"kind":"todo"})).unwrap()["records"].as_array().unwrap().is_empty());
        let temp = s.path.parent().unwrap().join(format!(".{}.tmp", s.path.file_name().unwrap().to_string_lossy()));
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
        s.records(json!({"action":"add","kind":"todo","text":"读书"})).unwrap();
        let v = s.records(json!({"action":"complete","kind":"todo","id":1})).unwrap();
        assert_eq!(v["records"][0]["done"], true);
        let restored = CompanionStore::open(s.path.clone()).unwrap();
        assert_eq!(restored.records(json!({"kind":"todo"})).unwrap()["records"][0]["done"], true);
        s.records(json!({"action":"reopen","kind":"todo","id":1})).unwrap();
        s.records(json!({"action":"update","kind":"todo","id":1,"text":"读两章"})).unwrap();
        let restored = CompanionStore::open(s.path.clone()).unwrap();
        let records = restored.records(json!({"kind":"todo"})).unwrap();
        assert_eq!(records["records"][0]["text"], "读两章");
        assert_eq!(records["records"][0]["done"], false);
        assert!(s.care(json!({"action":"configure","enabled":true})).is_err());
        s.care(json!({"action":"configure","enabled":true,"confirmed":true,"interval_minutes":30})).unwrap();
        let later = crate::timers::unix_now() + 1801;
        assert!(!s.care_due(later, 23)); assert!(s.care_due(later, 12));
        s.mark_care_sent(later).unwrap(); assert!(!s.care_due(later, 12));
        s.care(json!({"action":"configure","enabled":false})).unwrap();
        assert!(!s.care_due(later + 90000, 12));
        std::fs::remove_file(s.path).unwrap();
    }
}

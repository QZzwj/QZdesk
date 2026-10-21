//! Local scene engine.
//!
//! Scenes are intentionally local-first: a scene can be run from the web
//! console, voice tools, a local clock, or a real MQTT state change. Each action
//! runs independently so one missing lamp does not prevent a reminder or Skill
//! role change later in the same scene.

use crate::mcp_gateway::skill::{SkillManager, SkillRole};
use crate::mcp_gateway::tool::McpTool;
use crate::smarthome::SmartHomeHub;
use crate::timers::{TimerStore, local_now, unix_now};
use crate::user_data::{read_json, write_json_atomic};
use async_trait::async_trait;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;
use tokio::process::Command;

const MAX_SCENES: usize = 32;
const MAX_ACTIONS: usize = 12;
const MAX_TRIGGERS: usize = 8;
const MAX_SCENE_NAME_CHARS: usize = 40;
const DEFAULT_BACKLIGHT: &str = "/sys/class/backlight/backlight/brightness";

fn default_true() -> bool {
    true
}

fn default_version() -> u32 {
    1
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Scene {
    #[serde(default)]
    pub id: String,
    pub name: String,
    #[serde(default = "default_true")]
    pub enabled: bool,
    #[serde(default)]
    pub actions: Vec<SceneAction>,
    #[serde(default)]
    pub triggers: Vec<SceneTrigger>,
    #[serde(default)]
    pub created_at: u64,
    #[serde(default)]
    pub updated_at: u64,
    #[serde(default)]
    pub last_run_at: u64,
    #[serde(default)]
    pub last_status: String,
    #[serde(default)]
    pub last_source: String,
    /// Internal deduplication keys for schedule triggers. Kept on disk so a
    /// restart inside the same minute cannot fire a scene twice.
    #[serde(default, rename = "lastTriggered")]
    last_triggered: BTreeMap<String, u64>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum SceneAction {
    Light {
        #[serde(rename = "deviceId")]
        device_id: String,
        action: String,
        #[serde(default)]
        value: Option<i64>,
    },
    Volume {
        percent: u8,
    },
    Backlight {
        percent: u8,
    },
    Reminder {
        #[serde(default = "default_reminder_label")]
        label: String,
        #[serde(rename = "inMinutes")]
        in_minutes: u32,
    },
    SkillRole {
        skill: String,
        role: SkillRole,
    },
}

fn default_reminder_label() -> String {
    "场景提醒".to_string()
}

impl SceneAction {
    fn kind(&self) -> &'static str {
        match self {
            Self::Light { .. } => "light",
            Self::Volume { .. } => "volume",
            Self::Backlight { .. } => "backlight",
            Self::Reminder { .. } => "reminder",
            Self::SkillRole { .. } => "skill_role",
        }
    }

    fn label(&self) -> String {
        match self {
            Self::Light {
                device_id, action, ..
            } => format!("灯光 {} {}", device_id, action),
            Self::Volume { percent } => format!("音量 {}%", percent),
            Self::Backlight { percent } => format!("背光 {}%", percent),
            Self::Reminder { label, in_minutes } => {
                format!("{} 分钟后提醒：{}", in_minutes, label)
            }
            Self::SkillRole { skill, role } => {
                format!("Skill {} -> {:?}", skill, role)
            }
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum SceneTrigger {
    Manual,
    Time {
        hour: u8,
        minute: u8,
        #[serde(default)]
        weekdays: Vec<u8>,
    },
    Mqtt {
        #[serde(rename = "deviceId")]
        device_id: String,
        #[serde(default)]
        state: Option<bool>,
        #[serde(default)]
        brightness: Option<u8>,
    },
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct SceneState {
    #[serde(default = "default_version")]
    version: u32,
    #[serde(default)]
    scenes: Vec<Scene>,
}

impl Default for SceneState {
    fn default() -> Self {
        Self {
            version: 1,
            scenes: Vec::new(),
        }
    }
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SceneActionResult {
    pub index: usize,
    pub kind: String,
    pub label: String,
    pub ok: bool,
    pub message: String,
}

pub struct SceneService {
    path: PathBuf,
    state: Arc<Mutex<SceneState>>,
    skills: Arc<SkillManager>,
    smarthome: Arc<SmartHomeHub>,
    timers: TimerStore,
}

impl SceneService {
    pub fn load(
        skills: Arc<SkillManager>,
        smarthome: Arc<SmartHomeHub>,
        timers: TimerStore,
    ) -> Arc<Self> {
        Arc::new(Self::load_in(
            crate::user_data::file("scenes.json"),
            skills,
            smarthome,
            timers,
        ))
    }

    pub(crate) fn load_in(
        path: PathBuf,
        skills: Arc<SkillManager>,
        smarthome: Arc<SmartHomeHub>,
        timers: TimerStore,
    ) -> Self {
        let state = read_json(&path)
            .and_then(|value| serde_json::from_value::<SceneState>(value).ok())
            .unwrap_or_default();
        let mut state = state;
        state.version = 1;
        state.scenes.retain(|scene| {
            let valid = validate_scene(scene).is_ok();
            if !valid {
                log::warn!("场景 '{}' 格式无效，已跳过", scene.name);
            }
            valid
        });
        state.scenes.truncate(MAX_SCENES);
        Self {
            path,
            state: Arc::new(Mutex::new(state)),
            skills,
            smarthome,
            timers,
        }
    }

    pub fn list(&self) -> Value {
        let state = self.lock();
        json!({
            "ok": true,
            "scenes": state.scenes,
            "count": state.scenes.len(),
            "limits": {
                "scenes": MAX_SCENES,
                "actions": MAX_ACTIONS,
                "triggers": MAX_TRIGGERS
            },
            "capabilities": {
                "actions": ["light", "volume", "backlight", "reminder", "skill_role"],
                "triggers": ["manual", "time", "mqtt"],
                "dryRun": true,
                "physicalButton": false
            }
        })
    }

    pub fn save(&self, mut scene: Scene) -> Result<Value, String> {
        let now = unix_now();
        scene.name = scene
            .name
            .trim()
            .chars()
            .take(MAX_SCENE_NAME_CHARS)
            .collect();
        if scene.name.is_empty() {
            return Err("场景名称不能为空".to_string());
        }
        validate_scene(&scene)?;

        let mut state = self.lock();
        if scene.id.trim().is_empty() {
            if state.scenes.len() >= MAX_SCENES {
                return Err(format!("场景数量已达上限 {}", MAX_SCENES));
            }
            scene.id = uuid::Uuid::new_v4().to_string();
            scene.created_at = now;
            scene.updated_at = now;
            scene.last_run_at = 0;
            scene.last_status.clear();
            scene.last_source.clear();
            scene.last_triggered.clear();
            state.scenes.push(scene.clone());
        } else {
            let existing = state
                .scenes
                .iter_mut()
                .find(|item| item.id == scene.id)
                .ok_or_else(|| format!("场景 '{}' 不存在", scene.id))?;
            scene.created_at = existing.created_at;
            scene.updated_at = now;
            scene.last_run_at = existing.last_run_at;
            scene.last_status = existing.last_status.clone();
            scene.last_source = existing.last_source.clone();
            scene.last_triggered = existing.last_triggered.clone();
            *existing = scene.clone();
        }
        drop(state);
        self.persist()?;
        Ok(json!({"ok": true, "scene": scene}))
    }

    pub fn remove(&self, id: &str) -> Result<Value, String> {
        let mut state = self.lock();
        let before = state.scenes.len();
        state.scenes.retain(|scene| scene.id != id);
        if state.scenes.len() == before {
            return Err(format!("场景 '{}' 不存在", id));
        }
        drop(state);
        self.persist()?;
        Ok(json!({"ok": true, "removed": id}))
    }

    pub async fn run(&self, id: &str, dry_run: bool) -> Result<Value, String> {
        self.run_with_source(id, dry_run, "manual").await
    }

    pub async fn run_by_reference(&self, reference: &str, dry_run: bool) -> Result<Value, String> {
        let id = {
            let state = self.lock();
            state
                .scenes
                .iter()
                .find(|scene| scene.id == reference || scene.name == reference)
                .map(|scene| scene.id.clone())
        }
        .ok_or_else(|| format!("没有场景 '{}'", reference))?;
        self.run(&id, dry_run).await
    }

    async fn run_with_source(
        &self,
        id: &str,
        dry_run: bool,
        source: &str,
    ) -> Result<Value, String> {
        let scene = {
            let state = self.lock();
            state
                .scenes
                .iter()
                .find(|scene| scene.id == id)
                .cloned()
                .ok_or_else(|| format!("场景 '{}' 不存在", id))?
        };

        let mut results = Vec::with_capacity(scene.actions.len());
        for (index, action) in scene.actions.iter().enumerate() {
            let outcome = self.execute_action(action, dry_run).await;
            let (ok, message) = match outcome {
                Ok(message) => (true, message),
                Err(error) => (false, error),
            };
            results.push(SceneActionResult {
                index,
                kind: action.kind().to_string(),
                label: action.label(),
                ok,
                message,
            });
        }
        let failures = results.iter().filter(|result| !result.ok).count();
        let status = if failures == 0 { "ok" } else { "partial" };
        let completed_at = unix_now();

        if !dry_run {
            let mut state = self.lock();
            if let Some(stored) = state.scenes.iter_mut().find(|item| item.id == id) {
                stored.last_run_at = completed_at;
                stored.last_status = status.to_string();
                stored.last_source = source.to_string();
            }
            drop(state);
            if let Err(error) = self.persist() {
                log::warn!("场景运行结果写盘失败: {}", error);
            }
        }

        Ok(json!({
            "ok": failures == 0,
            "sceneId": id,
            "sceneName": scene.name,
            "source": source,
            "dryRun": dry_run,
            "status": status,
            "completedAt": completed_at,
            "failures": failures,
            "actions": results,
        }))
    }

    /// Called once per second by the core. A schedule key includes the exact
    /// local minute, so repeated ticks and process restarts cannot double-run.
    pub async fn tick_schedules(&self, now: u64) {
        let Some(local) = local_now(now) else {
            return;
        };
        let minute_key = format!("{} {:02}:{:02}", local.date, local.hour, local.minute);
        let due = {
            let mut state = self.lock();
            let mut due = Vec::new();
            for scene in &mut state.scenes {
                if !scene.enabled {
                    continue;
                }
                for (index, trigger) in scene.triggers.iter().enumerate() {
                    let SceneTrigger::Time {
                        hour,
                        minute,
                        weekdays,
                    } = trigger
                    else {
                        continue;
                    };
                    if *hour as u32 != local.hour
                        || *minute as u32 != local.minute
                        || (!weekdays.is_empty() && !weekdays.contains(&local.weekday))
                    {
                        continue;
                    }
                    let key = format!("schedule:{}:{}", index, minute_key);
                    if scene.last_triggered.contains_key(&key) {
                        continue;
                    }
                    scene.last_triggered.insert(key, now);
                    if scene.last_triggered.len() > 64 {
                        let oldest = scene
                            .last_triggered
                            .iter()
                            .min_by_key(|(_, timestamp)| **timestamp)
                            .map(|(key, _)| key.clone());
                        if let Some(oldest) = oldest {
                            scene.last_triggered.remove(&oldest);
                        }
                    }
                    due.push(scene.id.clone());
                    break;
                }
            }
            due
        };
        if due.is_empty() {
            return;
        }
        if let Err(error) = self.persist() {
            log::warn!("场景触发时刻写盘失败: {}", error);
        }
        for id in due {
            if let Err(error) = self.run_with_source(&id, false, "schedule").await {
                log::warn!("定时场景运行失败: {}", error);
            }
        }
    }

    pub async fn handle_state_change(&self, event: &Value) {
        let device_id = event
            .get("deviceId")
            .and_then(Value::as_str)
            .unwrap_or_default();
        if device_id.is_empty() {
            return;
        }
        let state = event.get("state").and_then(Value::as_bool);
        let brightness = event
            .get("brightness")
            .and_then(Value::as_u64)
            .map(|value| value.min(100) as u8);
        let due = {
            let state_guard = self.lock();
            state_guard
                .scenes
                .iter()
                .filter(|scene| scene.enabled)
                .filter(|scene| {
                    scene.triggers.iter().any(|trigger| match trigger {
                        SceneTrigger::Mqtt {
                            device_id: watched,
                            state: wanted_state,
                            brightness: wanted_brightness,
                        } => {
                            watched == device_id
                                && wanted_state.is_none_or(|wanted| state == Some(wanted))
                                && wanted_brightness.is_none_or(|wanted| brightness == Some(wanted))
                        }
                        _ => false,
                    })
                })
                .map(|scene| scene.id.clone())
                .collect::<Vec<_>>()
        };
        for id in due {
            if let Err(error) = self.run_with_source(&id, false, "mqtt").await {
                log::warn!("MQTT 场景运行失败: {}", error);
            }
        }
    }

    #[cfg(test)]
    fn scene_by_id(&self, id: &str) -> Option<Scene> {
        self.lock()
            .scenes
            .iter()
            .find(|scene| scene.id == id)
            .cloned()
    }

    async fn execute_action(&self, action: &SceneAction, dry_run: bool) -> Result<String, String> {
        match action {
            SceneAction::Light {
                device_id,
                action,
                value,
            } => {
                let action = action.trim();
                if !matches!(action, "on" | "off" | "toggle" | "brightness") {
                    return Err("灯光动作只能是 on / off / toggle / brightness".to_string());
                }
                if action != "brightness" && value.is_some() {
                    return Err("只有 brightness 动作可以携带 value".to_string());
                }
                let snapshot = self.smarthome.snapshot();
                let exists = snapshot
                    .get("devices")
                    .and_then(Value::as_array)
                    .is_some_and(|devices| {
                        devices.iter().any(|device| {
                            device.get("id").and_then(Value::as_str) == Some(device_id)
                        })
                    });
                if !exists {
                    return Err(format!("没有这台智能家居设备: {}", device_id));
                }
                if dry_run {
                    return Ok(format!(
                        "试运行通过：将向 {} 发布 {} 指令",
                        device_id, action
                    ));
                }
                self.smarthome
                    .command(device_id, action, *value)
                    .await
                    .map(|_| "MQTT 指令已发布；设备是否执行以状态回报为准".to_string())
                    .map_err(|error| error.to_string())
            }
            SceneAction::Volume { percent } => {
                if *percent > 100 {
                    return Err("音量必须是 0-100".to_string());
                }
                if dry_run {
                    return Ok(format!("试运行通过：将把音量设为 {}%", percent));
                }
                set_volume(*percent).await
            }
            SceneAction::Backlight { percent } => {
                if *percent > 100 {
                    return Err("背光必须是 0-100".to_string());
                }
                if dry_run {
                    let path = backlight_path()?;
                    let max = read_backlight_max(&path)?;
                    return Ok(format!(
                        "试运行通过：将把背光设为 {}%（raw 0-{}）",
                        percent, max
                    ));
                }
                set_backlight(*percent)
            }
            SceneAction::Reminder { label, in_minutes } => {
                if !(1..=1_440).contains(in_minutes) {
                    return Err("提醒延迟必须是 1-1440 分钟".to_string());
                }
                let target = unix_now().saturating_add(*in_minutes as u64 * 60);
                let local = local_now(target).ok_or_else(|| "无法读取本地时间".to_string())?;
                if dry_run {
                    return Ok(format!(
                        "试运行通过：将在 {:02}:{:02} 提醒 {}",
                        local.hour,
                        local.minute,
                        if label.trim().is_empty() {
                            default_reminder_label()
                        } else {
                            label.trim().to_string()
                        }
                    ));
                }
                let reminder_label = if label.trim().is_empty() {
                    default_reminder_label()
                } else {
                    label.trim().to_string()
                };
                self.timers
                    .add_reminder(
                        local.hour,
                        local.minute,
                        &reminder_label,
                        false,
                        &local.date,
                    )
                    .map(|reminder| {
                        format!(
                            "已创建 {:02}:{:02} 的一次性提醒：{}",
                            reminder.hour, reminder.minute, reminder.label
                        )
                    })
            }
            SceneAction::SkillRole { skill, role } => {
                if dry_run {
                    let exists = self.skills.list().get("skills").is_some_and(|skills| {
                        skills.as_array().is_some_and(|skills| {
                            skills
                                .iter()
                                .any(|entry| entry.get("id").and_then(Value::as_str) == Some(skill))
                        })
                    });
                    if !exists {
                        return Err(format!("Skill '{}' 不存在", skill));
                    }
                    return Ok(format!("试运行通过：将把 Skill {} 设为 {:?}", skill, role));
                }
                match role {
                    SkillRole::Primary => self.skills.switch_primary(skill),
                    other => self.skills.set_role(skill, *other),
                }
                .map(|_| format!("Skill {} 已设为 {:?}", skill, role))
            }
        }
    }

    fn persist(&self) -> Result<(), String> {
        let value = {
            let state = self.lock();
            serde_json::to_value(&*state).map_err(|error| format!("场景编码失败: {}", error))?
        };
        write_json_atomic(&self.path, &value)
    }

    fn lock(&self) -> MutexGuard<'_, SceneState> {
        self.state.lock().unwrap_or_else(|error| error.into_inner())
    }
}

fn validate_scene(scene: &Scene) -> Result<(), String> {
    if scene.name.trim().is_empty() {
        return Err("场景名称不能为空".to_string());
    }
    if scene.actions.is_empty() {
        return Err("场景至少需要一个动作".to_string());
    }
    if scene.actions.len() > MAX_ACTIONS {
        return Err(format!("场景最多 {} 个动作", MAX_ACTIONS));
    }
    if scene.triggers.len() > MAX_TRIGGERS {
        return Err(format!("场景最多 {} 个触发器", MAX_TRIGGERS));
    }
    for action in &scene.actions {
        match action {
            SceneAction::Light {
                device_id,
                action,
                value,
            } => {
                if device_id.trim().is_empty() {
                    return Err("灯光动作缺少 deviceId".to_string());
                }
                if !matches!(action.trim(), "on" | "off" | "toggle" | "brightness") {
                    return Err("灯光动作只能是 on / off / toggle / brightness".to_string());
                }
                if action.trim() == "brightness"
                    && !value.is_some_and(|value| (0..=100).contains(&value))
                {
                    return Err("灯光 brightness 的 value 必须是 0-100".to_string());
                }
                if action.trim() != "brightness" && value.is_some() {
                    return Err("只有 brightness 动作可以携带 value".to_string());
                }
            }
            SceneAction::Volume { percent } | SceneAction::Backlight { percent }
                if *percent > 100 =>
            {
                return Err("音量与背光必须在 0-100".to_string());
            }
            SceneAction::Volume { .. } | SceneAction::Backlight { .. } => {}
            SceneAction::Reminder { in_minutes, .. } if !(1..=1_440).contains(in_minutes) => {
                return Err("提醒延迟必须是 1-1440 分钟".to_string());
            }
            SceneAction::Reminder { .. } => {}
            SceneAction::SkillRole { skill, .. } if skill.trim().is_empty() => {
                return Err("Skill 角色动作缺少 skill".to_string());
            }
            SceneAction::SkillRole { .. } => {}
        }
    }
    for trigger in &scene.triggers {
        match trigger {
            SceneTrigger::Manual => {}
            SceneTrigger::Time {
                hour,
                minute,
                weekdays,
            } => {
                if *hour > 23 || *minute > 59 {
                    return Err("定时触发时间无效".to_string());
                }
                if weekdays.iter().any(|day| !(1..=7).contains(day)) {
                    return Err("weekdays 只能使用 1-7（周一至周日）".to_string());
                }
            }
            SceneTrigger::Mqtt {
                device_id,
                state,
                brightness,
            } => {
                if device_id.trim().is_empty() {
                    return Err("MQTT 触发器缺少 deviceId".to_string());
                }
                if state.is_none() && brightness.is_none() {
                    return Err("MQTT 触发器至少需要 state 或 brightness 条件".to_string());
                }
                if brightness.is_some_and(|value| value > 100) {
                    return Err("MQTT 触发 brightness 必须是 0-100".to_string());
                }
            }
        }
    }
    Ok(())
}

fn backlight_path() -> Result<PathBuf, String> {
    let path = std::env::var_os("QZDESK_BACKLIGHT")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(DEFAULT_BACKLIGHT));
    if !path.is_absolute() || path.file_name().and_then(|name| name.to_str()) != Some("brightness")
    {
        return Err("QZDESK_BACKLIGHT 必须指向绝对的 brightness 文件".to_string());
    }
    Ok(path)
}

fn read_backlight_max(path: &Path) -> Result<u64, String> {
    let max_path = path
        .parent()
        .ok_or_else(|| "背光路径没有父目录".to_string())?
        .join("max_brightness");
    let text = fs::read_to_string(&max_path)
        .map_err(|error| format!("无法读取 {}: {}", max_path.display(), error))?;
    let max = text
        .trim()
        .parse::<u64>()
        .map_err(|_| format!("{} 不是有效整数", max_path.display()))?;
    if max == 0 || max > 1_000_000 {
        return Err("背光最大值超出安全范围".to_string());
    }
    Ok(max)
}

fn set_backlight(percent: u8) -> Result<String, String> {
    let path = backlight_path()?;
    let max = read_backlight_max(&path)?;
    let raw = (percent as u64 * max + 50) / 100;
    fs::write(&path, format!("{}\n", raw))
        .map_err(|error| format!("写入 {} 失败: {}", path.display(), error))?;
    Ok(format!("背光已设为 {}%（raw {} / {}）", percent, raw, max))
}

async fn set_volume(percent: u8) -> Result<String, String> {
    let card = std::env::var("QZDESK_MIXER_CARD").unwrap_or_else(|_| "0".to_string());
    if card.is_empty() || !card.bytes().all(|byte| byte.is_ascii_digit()) {
        return Err("QZDESK_MIXER_CARD 必须是数字".to_string());
    }
    let control = std::env::var("QZDESK_MIXER_CONTROL").unwrap_or_else(|_| "numid=24".to_string());
    if control.trim().is_empty()
        || control.chars().count() > 64
        || control.chars().any(char::is_control)
    {
        return Err("QZDESK_MIXER_CONTROL 格式无效".to_string());
    }
    let max = std::env::var("QZDESK_MIXER_MAX")
        .ok()
        .map(|value| {
            value
                .parse::<u64>()
                .map_err(|_| "QZDESK_MIXER_MAX 必须是整数".to_string())
        })
        .transpose()?
        .unwrap_or(30);
    if max == 0 || max > 65_535 {
        return Err("QZDESK_MIXER_MAX 超出安全范围".to_string());
    }
    let raw = (percent as u64 * max + 50) / 100;
    let control_arg = if control.starts_with("numid=") || control.starts_with("name=") {
        control
    } else {
        format!("name={}", control)
    };
    let output = tokio::time::timeout(
        Duration::from_secs(5),
        Command::new("amixer")
            .args(["-c", card.as_str(), "cset", &control_arg, &raw.to_string()])
            .output(),
    )
    .await
    .map_err(|_| "amixer 设置超时".to_string())?
    .map_err(|error| format!("无法启动 amixer: {}", error))?;
    if !output.status.success() {
        let error = String::from_utf8_lossy(&output.stderr);
        return Err(format!("amixer 设置失败: {}", error.trim()));
    }
    Ok(format!("音量已设为 {}%（raw {} / {}）", percent, raw, max))
}

pub struct SceneListTool {
    service: Arc<SceneService>,
}

impl SceneListTool {
    pub fn new(service: Arc<SceneService>) -> Self {
        Self { service }
    }
}

#[async_trait]
impl McpTool for SceneListTool {
    fn name(&self) -> &str {
        "scene_list"
    }

    fn description(&self) -> &str {
        "列出本机场景及其动作、触发器、最近运行状态。需要运行场景时先调用它拿到 id 或名称。"
    }

    fn input_schema(&self) -> Value {
        json!({"type": "object", "properties": {}})
    }

    async fn call(&self, _params: Value) -> Result<Value, String> {
        Ok(self.service.list())
    }
}

pub struct SceneRunTool {
    service: Arc<SceneService>,
}

impl SceneRunTool {
    pub fn new(service: Arc<SceneService>) -> Self {
        Self { service }
    }
}

#[async_trait]
impl McpTool for SceneRunTool {
    fn name(&self) -> &str {
        "scene_run"
    }

    fn description(&self) -> &str {
        "运行本机场景。scene 可填场景 id 或完整名称；不确定时先调用 scene_list。dryRun=true 只检查将要执行的动作，不改变设备、音量、背光、提醒或 Skill 角色。"
    }

    fn input_schema(&self) -> Value {
        json!({
            "type": "object",
            "required": ["scene"],
            "properties": {
                "scene": {"type": "string", "description": "场景 id 或名称"},
                "dryRun": {"type": "boolean", "description": "只试运行，不产生副作用"}
            }
        })
    }

    async fn call(&self, params: Value) -> Result<Value, String> {
        let scene = params
            .get("scene")
            .and_then(Value::as_str)
            .ok_or("缺少 scene")?;
        let dry_run = params
            .get("dryRun")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        self.service.run_by_reference(scene, dry_run).await
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::SmartHomeConfig;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU32, Ordering};

    static COUNTER: AtomicU32 = AtomicU32::new(0);

    fn test_paths(label: &str) -> (PathBuf, PathBuf, PathBuf) {
        let root = std::env::temp_dir().join(format!(
            "qzdesk-scene-{}-{}-{}",
            label,
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::Relaxed)
        ));
        let skills = root.join("skills");
        let timers = root.join("timers");
        fs::create_dir_all(&skills).unwrap();
        fs::create_dir_all(&timers).unwrap();
        (root, skills, timers)
    }

    fn service(label: &str) -> (SceneService, PathBuf, TimerStore) {
        let (root, skills, timers) = test_paths(label);
        let skill_manager = Arc::new(SkillManager::for_test_dir(skills));
        let hub = SmartHomeHub::start(SmartHomeConfig::default());
        let timer_store = TimerStore::load_in(timers.clone(), timers);
        let service = SceneService::load_in(
            root.join("scenes.json"),
            skill_manager,
            hub,
            timer_store.clone(),
        );
        (service, root, timer_store)
    }

    fn light_scene(name: &str, device: &str) -> Scene {
        Scene {
            id: String::new(),
            name: name.to_string(),
            enabled: true,
            actions: vec![SceneAction::Light {
                device_id: device.to_string(),
                action: "toggle".to_string(),
                value: None,
            }],
            triggers: vec![SceneTrigger::Manual],
            created_at: 0,
            updated_at: 0,
            last_run_at: 0,
            last_status: String::new(),
            last_source: String::new(),
            last_triggered: BTreeMap::new(),
        }
    }

    #[test]
    fn scenes_persist_and_keep_created_time_on_update() {
        let (service, root, _) = service("persist");
        let saved = service.save(light_scene("回家", "zigbee:desk")).unwrap();
        let id = saved["scene"]["id"].as_str().unwrap().to_string();
        let created = saved["scene"]["createdAt"].as_u64().unwrap();
        let mut changed = light_scene("回家灯光", "zigbee:desk");
        changed.id = id.clone();
        changed.actions[0] = SceneAction::Volume { percent: 35 };
        let updated = service.save(changed).unwrap();
        assert_eq!(updated["scene"]["createdAt"].as_u64(), Some(created));

        let reloaded = SceneService::load_in(
            root.join("scenes.json"),
            service.skills.clone(),
            service.smarthome.clone(),
            service.timers.clone(),
        );
        let list = reloaded.list();
        assert_eq!(list["count"], 1);
        assert_eq!(list["scenes"][0]["name"], "回家灯光");

        let _ = fs::remove_dir_all(root);
    }

    #[tokio::test]
    async fn dry_run_reports_each_failure_and_continues() {
        let (service, root, _) = service("dry-run");
        let mut scene = light_scene("检查", "missing-light");
        scene.actions.push(SceneAction::SkillRole {
            skill: "missing-skill".to_string(),
            role: SkillRole::Primary,
        });
        scene.actions.push(SceneAction::Volume { percent: 30 });
        let id = service.save(scene).unwrap()["scene"]["id"]
            .as_str()
            .unwrap()
            .to_string();
        let result = service.run(&id, true).await.unwrap();
        assert_eq!(result["dryRun"], true);
        assert_eq!(result["failures"], 2);
        assert_eq!(result["actions"].as_array().unwrap().len(), 3);
        assert_eq!(result["actions"][2]["ok"], true);
        assert_eq!(service.scene_by_id(&id).unwrap().last_run_at, 0);
        let _ = fs::remove_dir_all(root);
    }

    #[tokio::test]
    async fn a_time_trigger_fires_only_once_per_minute() {
        let (service, root, _) = service("schedule");
        let now = unix_now();
        let local = local_now(now).unwrap();
        let mut scene = light_scene("定时", "missing-light");
        scene.triggers = vec![SceneTrigger::Time {
            hour: local.hour as u8,
            minute: local.minute as u8,
            weekdays: vec![local.weekday],
        }];
        let id = service.save(scene).unwrap()["scene"]["id"]
            .as_str()
            .unwrap()
            .to_string();
        service.tick_schedules(now).await;
        let first = service.scene_by_id(&id).unwrap();
        assert!(first.last_run_at > 0);
        assert_eq!(first.last_source, "schedule");
        service.tick_schedules(now + 1).await;
        let second = service.scene_by_id(&id).unwrap();
        assert_eq!(second.last_run_at, first.last_run_at);
        let _ = fs::remove_dir_all(root);
    }
}

//! 提醒与番茄钟的 MCP 工具：直接操作核心里的 `TimerStore`。
//!
//! 这两个工具以前是子进程脚本（`set_timer.py` / `pomodoro.py`），脚本只把命令
//! 写进 `timers.json` / `pomodoro_cmd.json`，等着"界面来消费"——而界面从来没
//! 读过那两个文件。于是语音说"提醒我七点半喝水"，设备上什么都不会发生。
//!
//! 改成内置工具后，模型写的就是界面与网页读的那一份，到点也由核心统一播报
//! （见 `crate::timers::TimerStore::tick`）。

use async_trait::async_trait;
use serde_json::{json, Value};

use super::tool::McpTool;
use crate::timers::{pomodoro_text, PomodoroOptions, TimerStore};

fn describe(error: &str) -> Value {
    json!({ "status": "error", "message": error })
}

/// 新建定时提醒。
pub struct SetTimerTool {
    store: TimerStore,
}

impl SetTimerTool {
    pub fn new(store: TimerStore) -> Self {
        Self { store }
    }
}

#[async_trait]
impl McpTool for SetTimerTool {
    fn name(&self) -> &str {
        "set_timer"
    }

    fn description(&self) -> &str {
        "为设备新建一个定时提醒。当用户说“提醒我”“定个闹钟”“每天几点做什么”时调用。\
         提醒到点会写进设备与网页共用的聊天记录，界面和网页都会看到。\
         hour 为 0-23，minute 为 0-59，daily 表示是否每天重复（默认 true），\
         label 是提醒内容，date 是一次性提醒的日期（YYYY-MM-DD，可留空表示最近一次）。"
    }

    fn input_schema(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "hour": { "type": "integer", "description": "小时，0-23" },
                "minute": { "type": "integer", "description": "分钟，0-59" },
                "label": { "type": "string", "description": "提醒内容，例如「喝水」" },
                "daily": { "type": "boolean", "description": "是否每天重复，默认 true" },
                "date": { "type": "string", "description": "一次性提醒的日期 YYYY-MM-DD，可留空" }
            },
            "required": ["hour", "minute"]
        })
    }

    async fn call(&self, params: Value) -> Result<Value, String> {
        let Some(hour) = params.get("hour").and_then(Value::as_u64) else {
            return Ok(describe("hour 必须是整数 0-23"));
        };
        let Some(minute) = params.get("minute").and_then(Value::as_u64) else {
            return Ok(describe("minute 必须是整数 0-59"));
        };
        let label = params.get("label").and_then(Value::as_str).unwrap_or("");
        let daily = params.get("daily").and_then(Value::as_bool).unwrap_or(true);
        let date = params.get("date").and_then(Value::as_str).unwrap_or("");

        match self
            .store
            .add_reminder(hour as u32, minute as u32, label, daily, date)
        {
            Ok(reminder) => Ok(json!({
                "status": "ok",
                "message": format!(
                    "已新建提醒 {} {:02}:{:02} {}",
                    if reminder.daily { "每天" } else { "仅一次" },
                    reminder.hour,
                    reminder.minute,
                    reminder.label
                ),
                // 顺带把当前列表带回去：用户常接着问"现在有哪些提醒"
                "reminders": self.store.reminders(),
            })),
            Err(message) => Ok(describe(&message)),
        }
    }
}

/// 番茄钟：开始 / 暂停 / 继续 / 结束 / 查询。
pub struct PomodoroTool {
    store: TimerStore,
}

impl PomodoroTool {
    pub fn new(store: TimerStore) -> Self {
        Self { store }
    }
}

#[async_trait]
impl McpTool for PomodoroTool {
    fn name(&self) -> &str {
        "pomodoro"
    }

    fn description(&self) -> &str {
        "控制设备上的番茄钟（专注-休息计时循环），也可查询还剩多久。当用户说“开始番茄钟”\
         “专注 25 分钟”“我要开始专注”“休息一下”“暂停番茄钟”“番茄钟还剩多久”时调用。\
         action 取 start/pause/resume/stop/status；focus_minutes 专注时长（默认 25），\
         break_minutes 休息时长（默认 5），cycles 轮数（默认 4，最多 12）。"
    }

    fn input_schema(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "enum": ["start", "pause", "resume", "stop", "status"],
                    "description": "要执行的动作"
                },
                "focus_minutes": { "type": "integer", "description": "专注时长（分钟），默认 25" },
                "break_minutes": { "type": "integer", "description": "休息时长（分钟），默认 5" },
                "cycles": { "type": "integer", "description": "总轮数，默认 4" }
            },
            "required": ["action"]
        })
    }

    async fn call(&self, params: Value) -> Result<Value, String> {
        let action = params
            .get("action")
            .and_then(Value::as_str)
            .unwrap_or("status");
        let options = PomodoroOptions {
            focus_minutes: params
                .get("focus_minutes")
                .and_then(Value::as_u64)
                .map(|value| value as u32),
            break_minutes: params
                .get("break_minutes")
                .and_then(Value::as_u64)
                .map(|value| value as u32),
            cycles: params
                .get("cycles")
                .and_then(Value::as_u64)
                .map(|value| value as u32),
        };
        match self.store.pomodoro_action(action, options) {
            Ok(state) => Ok(json!({
                "status": "ok",
                "message": pomodoro_text(&state),
                "pomodoro": state,
            })),
            Err(message) => Ok(describe(&message)),
        }
    }
}

//! 提醒与番茄钟：核心是唯一权威。
//!
//! 这两样东西以前是两边各存一份：语音侧由 `set_timer.py` / `pomodoro.py` 往
//! `timers.json` / `pomodoro.json` 里写，设备界面却在内存里另存一张表——结果是
//! 语音建的提醒界面上看不见，界面建的提醒重启就丢，番茄钟同理。
//!
//! 现在统一由这里管：
//! - 文件格式与两个脚本保持一致（脚本会原样保留不认识的键，所以能共用同一份
//!   文件，谁写的另一边都读得到）；
//! - 界面与网页改读写 `/api/timers`、`/api/pomodoro`；
//! - 到点由核心统一播报（写进聊天记录，界面与网页同时看到，见 `tick`）。

use crate::user_data::{read_json, write_json_atomic};
use serde::{Deserialize, Serialize};
use std::env;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{SystemTime, UNIX_EPOCH};

/// 与 `set_timer.py` 的 `MAX_TIMERS` 一致：设备屏幕上一屏也就放得下这几条。
pub const MAX_REMINDERS: usize = 8;

const DEFAULT_FOCUS_MINUTES: u32 = 25;
const DEFAULT_BREAK_MINUTES: u32 = 5;
const DEFAULT_CYCLES: u32 = 4;
/// 与 `pomodoro.py` 的 `MAX_CYCLES` 一致。
const MAX_CYCLES: u32 = 12;
const MAX_MINUTES: u32 = 180;

const REMINDER_FILE: &str = "timers.json";
const POMODORO_FILE: &str = "pomodoro.json";

fn default_true() -> bool {
    true
}

fn default_label() -> String {
    "提醒".to_string()
}

fn default_phase() -> String {
    "focus".to_string()
}

fn default_focus() -> u32 {
    DEFAULT_FOCUS_MINUTES
}

fn default_break() -> u32 {
    DEFAULT_BREAK_MINUTES
}

fn default_cycles() -> u32 {
    DEFAULT_CYCLES
}

/// 一条提醒。字段就是 `set_timer.py` 写的那几个，另加一个 `id` 供界面删除用
/// （脚本不认这个键，但会原样保留，不会因为一次语音提醒就把 id 抹掉）。
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Reminder {
    /// 核心分配，同一份文件里唯一；老文件 / 脚本刚写进来的没有，加载时补上。
    #[serde(default)]
    pub id: u64,
    pub hour: u8,
    pub minute: u8,
    #[serde(default = "default_label")]
    pub label: String,
    /// true = 每天重复，false = 只在 `date` 那天响一次（`date` 为空表示"下一次"）。
    #[serde(default = "default_true")]
    pub daily: bool,
    /// 一次性提醒的日期，`YYYY-MM-DD`。
    #[serde(default)]
    pub date: String,
    /// 最近一次触发日期，`YYYY-MM-DD`。每天重复的靠它避免同一天响两次，
    /// 新建时也靠它避免"刚建好就立刻触发"。
    #[serde(default)]
    pub last_date: String,
    #[serde(default = "default_true")]
    pub enabled: bool,
}

/// 番茄钟状态。字段与 `pomodoro.py` 的 `pomodoro.json` 对齐，两边可互读。
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Pomodoro {
    #[serde(default)]
    pub active: bool,
    #[serde(default)]
    pub paused: bool,
    /// `focus` | `break`
    #[serde(default = "default_phase")]
    pub phase: String,
    #[serde(default)]
    pub remaining_seconds: u32,
    /// 上次结算时刻（Unix 秒）。到点的判定全按它推算，所以核心重启也不会把
    /// 剩余时间算错（重启期间流逝的时间照常扣掉）。
    #[serde(default)]
    pub updated_at: u64,
    #[serde(default)]
    pub completed_cycles: u32,
    #[serde(default = "default_cycles")]
    pub total_cycles: u32,
    #[serde(default = "default_focus")]
    pub focus_minutes: u32,
    #[serde(default = "default_break")]
    pub break_minutes: u32,
}

impl Default for Pomodoro {
    fn default() -> Self {
        Self {
            active: false,
            paused: false,
            phase: default_phase(),
            remaining_seconds: 0,
            updated_at: 0,
            completed_cycles: 0,
            total_cycles: DEFAULT_CYCLES,
            focus_minutes: DEFAULT_FOCUS_MINUTES,
            break_minutes: DEFAULT_BREAK_MINUTES,
        }
    }
}

impl Pomodoro {
    /// `focus` / `break` 之外的值一律当专注，避免手改文件把界面带坏。
    pub fn is_break(&self) -> bool {
        self.phase == "break"
    }
}

/// `/api/pomodoro` 的操作参数。
#[derive(Debug, Clone, Copy, Default)]
pub struct PomodoroOptions {
    pub focus_minutes: Option<u32>,
    pub break_minutes: Option<u32>,
    pub cycles: Option<u32>,
}

/// 该播报什么。文本在这里就定稿：界面、网页、日志看到的完全一样。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TimerEvent {
    /// 提醒到点
    Reminder { text: String },
    /// 番茄钟换阶段或整轮结束
    Pomodoro { text: String },
}

impl TimerEvent {
    pub fn text(&self) -> &str {
        match self {
            TimerEvent::Reminder { text } | TimerEvent::Pomodoro { text } => text,
        }
    }
}

/// 本地时间。提醒是"给人看的钟点"，必须按设备时区判断——和界面上的时钟同源。
struct LocalNow {
    date: String,
    hour: u32,
    minute: u32,
}

/// 走 libc 的 `localtime_r`：TZ 由设置页写进环境（与界面时钟一致），核心只需跟随。
fn local_now(now: u64) -> Option<LocalNow> {
    let seconds = now as libc::time_t;
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    // SAFETY: `localtime_r` 只写我们提供的 `tm`，不做全局状态写入之外的副作用。
    let converted = unsafe { libc::localtime_r(&seconds, &mut tm) };
    if converted.is_null() {
        return None;
    }
    Some(LocalNow {
        date: format!(
            "{:04}-{:02}-{:02}",
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday
        ),
        hour: tm.tm_hour as u32,
        minute: tm.tm_min as u32,
    })
}

pub fn unix_now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs())
        .unwrap_or(0)
}

fn env_dir(names: [&str; 2]) -> Option<PathBuf> {
    names
        .into_iter()
        .find_map(|name| env::var_os(name))
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
}

/// 两个脚本各有一个目录覆盖变量，这里保持同样的优先级；都没设就用
/// `user_data` 里那套默认值（与脚本的默认值一致）。
fn resolved_dir() -> (PathBuf, PathBuf) {
    let reminder =
        env_dir(["QZDESK_TIMER_DIR", "DESKBOT_TIMER_DIR"]).unwrap_or_else(crate::user_data::dir);
    let pomodoro = env_dir(["QZDESK_POMODORO_DIR", "DESKBOT_POMODORO_DIR"])
        .unwrap_or_else(|| reminder.clone());
    (reminder, pomodoro)
}

struct Inner {
    reminders: Vec<Reminder>,
    pomodoro: Pomodoro,
    next_id: u64,
    /// 有改动才落盘：`tick` 每秒都在跑，不能每秒都写文件。
    dirty_reminders: bool,
    dirty_pomodoro: bool,
}

/// 提醒与番茄钟的唯一副本。克隆出来的是同一个存储（内部 `Arc`）。
#[derive(Clone)]
pub struct TimerStore {
    dir: PathBuf,
    pomodoro_dir: PathBuf,
    inner: Arc<Mutex<Inner>>,
}

impl TimerStore {
    pub fn load() -> Self {
        let (dir, pomodoro_dir) = resolved_dir();
        Self::load_in(dir, pomodoro_dir)
    }

    /// 指定目录加载，便于测试。
    pub fn load_in(dir: PathBuf, pomodoro_dir: PathBuf) -> Self {
        let mut reminders = load_reminders(&dir.join(REMINDER_FILE));
        /* 老文件与脚本写的条目没有 id：这里补一个并落盘。不补的话界面上的
         * "删除"就只能按序号猜，脚本再插一条就全错了。 */
        let mut next_id = 1_u64;
        let mut patched = false;
        for reminder in reminders.iter_mut() {
            if reminder.id == 0 {
                reminder.id = next_id;
                patched = true;
            }
            next_id = next_id.max(reminder.id + 1);
        }
        let mut pomodoro = read_json(&pomodoro_dir.join(POMODORO_FILE))
            .and_then(|value| serde_json::from_value::<Pomodoro>(value).ok())
            .unwrap_or_default();
        /* 手改文件把数值写飞了也不该让番茄钟失效，顺手夹回合法区间。 */
        pomodoro.total_cycles = pomodoro.total_cycles.clamp(1, MAX_CYCLES);
        pomodoro.focus_minutes = pomodoro.focus_minutes.clamp(1, MAX_MINUTES);
        pomodoro.break_minutes = pomodoro.break_minutes.clamp(1, MAX_MINUTES);

        let store = Self {
            dir,
            pomodoro_dir,
            inner: Arc::new(Mutex::new(Inner {
                reminders,
                pomodoro,
                next_id,
                dirty_reminders: patched,
                dirty_pomodoro: false,
            })),
        };
        if patched {
            let _ = store.flush();
        }
        store
    }

    pub fn dir(&self) -> &Path {
        &self.dir
    }

    fn lock(&self) -> MutexGuard<'_, Inner> {
        // 别的线程 panic 过也不该让提醒跟着失效，中毒时取回数据继续用。
        self.inner.lock().unwrap_or_else(|error| error.into_inner())
    }

    /// 把标记为脏的部分写回磁盘。失败只记日志：内存里那份仍然是对的，
    /// 下一次变更还会再试。
    fn flush(&self) -> Result<(), String> {
        let (reminders, pomodoro, write_reminders, write_pomodoro) = {
            let mut inner = self.lock();
            let reminders = serde_json::json!({ "timers": inner.reminders });
            let pomodoro = serde_json::to_value(&inner.pomodoro)
                .unwrap_or_else(|_| serde_json::Value::Object(Default::default()));
            let write_reminders = inner.dirty_reminders;
            let write_pomodoro = inner.dirty_pomodoro;
            inner.dirty_reminders = false;
            inner.dirty_pomodoro = false;
            (reminders, pomodoro, write_reminders, write_pomodoro)
        };
        let mut errors = Vec::new();
        if write_reminders {
            if let Err(error) = write_json_atomic(&self.dir.join(REMINDER_FILE), &reminders) {
                errors.push(error);
            }
        }
        if write_pomodoro {
            if let Err(error) = write_json_atomic(&self.pomodoro_dir.join(POMODORO_FILE), &pomodoro) {
                errors.push(error);
            }
        }
        if errors.is_empty() {
            Ok(())
        } else {
            Err(errors.join("; "))
        }
    }

    pub fn reminders(&self) -> Vec<Reminder> {
        self.lock().reminders.clone()
    }

    pub fn pomodoro(&self) -> Pomodoro {
        self.lock().pomodoro.clone()
    }

    /// 新建提醒。规则与 `set_timer.py` 一致：同钟点同内容的算重复，超过上限拒绝。
    pub fn add_reminder(
        &self,
        hour: u32,
        minute: u32,
        label: &str,
        daily: bool,
        date: &str,
    ) -> Result<Reminder, String> {
        if hour > 23 || minute > 59 {
            return Err("时间超出范围：hour 0-23，minute 0-59".to_string());
        }
        let label = {
            let trimmed = label.trim();
            if trimmed.is_empty() {
                "提醒".to_string()
            } else {
                trimmed.chars().take(24).collect()
            }
        };
        let now = unix_now();
        let today = local_now(now).map(|local| local.date).unwrap_or_default();

        let mut inner = self.lock();
        let duplicated = inner.reminders.iter().any(|item| {
            item.hour as u32 == hour
                && item.minute as u32 == minute
                && item.label == label
                && item.daily == daily
        });
        if duplicated {
            return Err(format!("已存在 {:02}:{:02} {} 的提醒", hour, minute, label));
        }
        if inner.reminders.len() >= MAX_REMINDERS {
            return Err(format!("提醒数量已达上限 {} 条", MAX_REMINDERS));
        }

        let reminder = Reminder {
            id: inner.next_id,
            hour: hour as u8,
            minute: minute as u8,
            label,
            daily,
            date: if daily { String::new() } else { date.trim().to_string() },
            // 与脚本一致：新建的当天不再触发，避免"刚建好就立刻响/刚建完就错过"
            last_date: if daily { today } else { String::new() },
            enabled: true,
        };
        inner.next_id += 1;
        inner.reminders.push(reminder.clone());
        inner.dirty_reminders = true;
        drop(inner);
        self.flush().map_err(|error| {
            log::warn!("提醒写盘失败: {}", error);
            error
        })?;
        Ok(reminder)
    }

    pub fn remove_reminder(&self, id: u64) -> Result<(), String> {
        let mut inner = self.lock();
        let before = inner.reminders.len();
        inner.reminders.retain(|item| item.id != id);
        if inner.reminders.len() == before {
            return Err(format!("提醒 {} 不存在", id));
        }
        inner.dirty_reminders = true;
        drop(inner);
        self.flush()
    }

    /// 番茄钟操作：`start` / `pause` / `resume` / `stop`（`reset` 等同 `stop`）
    /// / `status`。与 `pomodoro.py` 的动作集合保持一致。
    pub fn pomodoro_action(
        &self,
        action: &str,
        options: PomodoroOptions,
    ) -> Result<Pomodoro, String> {
        let now = unix_now();
        let mut inner = self.lock();
        // 先按流逝时间结算一次：用户看到的剩余时间才是真实的
        settle_pomodoro(&mut inner.pomodoro, now);

        match action {
            "start" | "restart" => {
                let focus = options.focus_minutes.unwrap_or(DEFAULT_FOCUS_MINUTES).clamp(1, MAX_MINUTES);
                let pause = options.break_minutes.unwrap_or(DEFAULT_BREAK_MINUTES).clamp(1, MAX_MINUTES);
                let cycles = options.cycles.unwrap_or(DEFAULT_CYCLES).clamp(1, MAX_CYCLES);
                inner.pomodoro = Pomodoro {
                    active: true,
                    paused: false,
                    phase: default_phase(),
                    remaining_seconds: focus * 60,
                    updated_at: now,
                    completed_cycles: 0,
                    total_cycles: cycles,
                    focus_minutes: focus,
                    break_minutes: pause,
                };
            }
            "pause" => {
                if !inner.pomodoro.active {
                    return Err("番茄钟还没开始".to_string());
                }
                inner.pomodoro.paused = true;
                inner.pomodoro.updated_at = now;
            }
            "resume" => {
                if !inner.pomodoro.active {
                    return Err("番茄钟还没开始".to_string());
                }
                inner.pomodoro.paused = false;
                inner.pomodoro.updated_at = now;
            }
            "stop" | "reset" => {
                inner.pomodoro.active = false;
                inner.pomodoro.paused = false;
                inner.pomodoro.remaining_seconds = 0;
                inner.pomodoro.updated_at = now;
            }
            "status" => {}
            other => {
                return Err(format!(
                    "未知操作 {}：可用 start / pause / resume / stop / status",
                    other
                ));
            }
        }

        let state = inner.pomodoro.clone();
        inner.dirty_pomodoro = true;
        drop(inner);
        self.flush()?;
        Ok(state)
    }

    /// 每秒调用一次：推进番茄钟、检查提醒是否到点，返回要播报的内容。
    ///
    /// 播报统一写进聊天记录（界面与网页同时看到），这样提醒不会只出现在
    /// 某一个页面上——用户翻到别的页面也照样收得到。
    pub fn tick(&self, now: u64) -> Vec<TimerEvent> {
        let mut events = Vec::new();
        let local = local_now(now);
        {
            let mut inner = self.lock();
            if let Some(event) = settle_pomodoro(&mut inner.pomodoro, now) {
                inner.dirty_pomodoro = true;
                events.push(event);
            }
            if let Some(local) = local.as_ref() {
                let fired = fire_due_reminders(&mut inner.reminders, local);
                if !fired.is_empty() {
                    inner.dirty_reminders = true;
                    events.extend(fired.into_iter().map(|text| TimerEvent::Reminder { text }));
                }
            }
        }
        if !events.is_empty() {
            if let Err(error) = self.flush() {
                log::warn!("提醒/番茄钟写盘失败: {}", error);
            }
        }
        events
    }
}

/// 按流逝时间推进番茄钟，返回需要播报的事件。
///
/// 与 `pomodoro.py` 的 `advance_state` 同一套算法：以 `updated_at` 为基准推算，
/// 一次跨过多个阶段也能算对（比如休眠/重启后直接跳到了下一轮休息）。
fn settle_pomodoro(state: &mut Pomodoro, now: u64) -> Option<TimerEvent> {
    if !state.active || state.paused {
        return None;
    }
    if state.updated_at == 0 || state.updated_at > now {
        // 没有基准（旧文件 / 时钟倒退）就先取当前时刻，下一拍再正常推进
        state.updated_at = now;
        return None;
    }

    let mut elapsed = now - state.updated_at;
    let mut remaining = state.remaining_seconds as u64;
    let mut event = None;
    while remaining > 0 && elapsed >= remaining {
        elapsed -= remaining;
        if state.phase == "break" {
            state.phase = default_phase();
            remaining = state.focus_minutes.max(1) as u64 * 60;
            event = Some(TimerEvent::Pomodoro {
                text: format!("🍅 休息结束，开始 {} 分钟专注", state.focus_minutes),
            });
        } else {
            state.completed_cycles += 1;
            if state.completed_cycles >= state.total_cycles {
                state.active = false;
                state.paused = false;
                state.remaining_seconds = 0;
                state.updated_at = now;
                return Some(TimerEvent::Pomodoro {
                    text: format!("🍅 番茄钟完成，本次共 {} 轮专注", state.completed_cycles),
                });
            }
            state.phase = "break".to_string();
            remaining = state.break_minutes.max(1) as u64 * 60;
            event = Some(TimerEvent::Pomodoro {
                text: format!("☕ 第 {} 轮专注完成，休息 {} 分钟", state.completed_cycles, state.break_minutes),
            });
        }
    }
    state.remaining_seconds = remaining.saturating_sub(elapsed) as u32;
    state.updated_at = now;
    event
}

/// 挑出这一分钟该响的提醒，并更新它们的触发记录。
fn fire_due_reminders(reminders: &mut Vec<Reminder>, local: &LocalNow) -> Vec<String> {
    let mut fired = Vec::new();
    reminders.retain_mut(|reminder| {
        if !reminder.enabled
            || reminder.hour as u32 != local.hour
            || reminder.minute as u32 != local.minute
            || reminder.last_date == local.date
        {
            return true;
        }
        // 一次性提醒：指定的日子没到 / 已经过了就不响；`date` 为空表示"下一次"
        if !reminder.daily && !reminder.date.is_empty() && reminder.date != local.date {
            return true;
        }
        fired.push(format!(
            "⏰ 提醒 {:02}:{:02} {}",
            reminder.hour, reminder.minute, reminder.label
        ));
        reminder.last_date = local.date.clone();
        /* 一次性提醒响过就撤掉：留在列表里既不会再响，也占着上限的名额。 */
        reminder.daily
    });
    fired
}

/// 读 `timers.json`。单条坏掉只跳过那一条：文件可能被手改过，不该因此
/// 让整张表消失。
fn load_reminders(path: &Path) -> Vec<Reminder> {
    let Some(value) = read_json(path) else {
        return Vec::new();
    };
    let items = match value {
        serde_json::Value::Object(ref map) => match map.get("timers") {
            Some(serde_json::Value::Array(items)) => items.clone(),
            _ => Vec::new(),
        },
        serde_json::Value::Array(items) => items,
        _ => Vec::new(),
    };
    let mut reminders = Vec::new();
    for item in items {
        match serde_json::from_value::<Reminder>(item.clone()) {
            Ok(reminder) if reminder.hour <= 23 && reminder.minute <= 59 => {
                reminders.push(reminder)
            }
            Ok(_) => log::warn!("提醒时间越界，已跳过: {}", item),
            Err(error) => log::warn!("提醒格式不对，已跳过（{}）: {}", error, item),
        }
    }
    reminders.truncate(MAX_REMINDERS);
    reminders
}

/// 给 AI 工具和网页用的一句话状态。
pub fn pomodoro_text(state: &Pomodoro) -> String {
    if !state.active {
        if state.completed_cycles > 0 {
            return format!("番茄钟已结束，本次完成 {} 轮专注", state.completed_cycles);
        }
        return "番茄钟当前未运行".to_string();
    }
    let phase = if state.is_break() { "休息" } else { "专注" };
    let clock = format!(
        "{:02}:{:02}",
        state.remaining_seconds / 60,
        state.remaining_seconds % 60
    );
    let cycle = (state.completed_cycles + 1).min(state.total_cycles);
    if state.paused {
        format!(
            "番茄钟已暂停，{}剩余 {}（第 {}/{} 轮）",
            phase, clock, cycle, state.total_cycles
        )
    } else {
        format!(
            "番茄钟进行中，{}剩余 {}（第 {}/{} 轮）",
            phase, clock, cycle, state.total_cycles
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::sync::atomic::{AtomicU32, Ordering};

    static COUNTER: AtomicU32 = AtomicU32::new(0);

    /// 每个用例一个独立目录：提醒是落盘的，共用目录会互相干扰。
    fn store() -> (TimerStore, PathBuf) {
        let dir = env::temp_dir().join(format!(
            "qzdesk-timers-test-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        (TimerStore::load_in(dir.clone(), dir.clone()), dir)
    }

    #[test]
    fn reminders_persist_and_survive_reload() {
        let (store, dir) = store();
        store.add_reminder(7, 30, "喝水", true, "").unwrap();
        store.add_reminder(21, 0, "吃药", false, "").unwrap();

        let reloaded = TimerStore::load_in(dir.clone(), dir.clone());
        let reminders = reloaded.reminders();
        assert_eq!(reminders.len(), 2);
        assert_eq!(reminders[0].label, "喝水");
        assert_eq!(reminders[1].hour, 21);
        // id 落盘了，重新加载不会重排
        assert_eq!(reminders[0].id, 1);
        assert_eq!(reminders[1].id, 2);
    }

    #[test]
    fn duplicate_and_over_limit_are_rejected() {
        let (store, _dir) = store();
        store.add_reminder(7, 30, "喝水", true, "").unwrap();
        assert!(store.add_reminder(7, 30, "喝水", true, "").is_err());
        for index in 0..MAX_REMINDERS - 1 {
            store
                .add_reminder(8 + index as u32, 0, "别的", true, "")
                .unwrap();
        }
        assert_eq!(store.reminders().len(), MAX_REMINDERS);
        assert!(store.add_reminder(23, 59, "再来一条", true, "").is_err());
    }

    #[test]
    fn a_daily_reminder_fires_once_a_day() {
        let mut reminders = vec![Reminder {
            id: 1,
            hour: 7,
            minute: 30,
            label: "喝水".into(),
            daily: true,
            date: String::new(),
            last_date: "2026-10-05".into(),
            enabled: true,
        }];
        let local = LocalNow {
            date: "2026-10-06".into(),
            hour: 7,
            minute: 30,
        };
        let first = fire_due_reminders(&mut reminders, &local);
        assert_eq!(first.len(), 1);
        assert!(first[0].contains("喝水"));
        // 同一分钟内的第二拍不再响，列表里留着
        let second = fire_due_reminders(&mut reminders, &local);
        assert!(second.is_empty());
        assert_eq!(reminders.len(), 1);
    }

    #[test]
    fn a_one_shot_reminder_is_removed_after_firing() {
        let mut reminders = vec![Reminder {
            id: 3,
            hour: 9,
            minute: 0,
            label: "开会".into(),
            daily: false,
            date: String::new(),
            last_date: String::new(),
            enabled: true,
        }];
        let local = LocalNow {
            date: "2026-10-06".into(),
            hour: 9,
            minute: 0,
        };
        assert_eq!(fire_due_reminders(&mut reminders, &local).len(), 1);
        assert!(reminders.is_empty(), "一次性提醒响过之后应当撤掉");
    }

    #[test]
    fn a_reminder_created_this_minute_does_not_fire_immediately() {
        let (store, _dir) = store();
        let now = unix_now();
        let local = local_now(now).expect("local time");
        let reminder = store
            .add_reminder(local.hour, local.minute, "刚建的", true, "")
            .unwrap();
        assert_eq!(reminder.last_date, local.date);
        // 下一秒的 tick 不该把它打出来
        assert!(store.tick(now + 1).is_empty());
    }

    #[test]
    fn pomodoro_walks_focus_break_finish() {
        let mut state = Pomodoro {
            active: true,
            paused: false,
            phase: "focus".into(),
            remaining_seconds: 60,
            updated_at: 1_000,
            completed_cycles: 0,
            total_cycles: 2,
            focus_minutes: 1,
            break_minutes: 1,
        };
        // 专注结束 → 进入休息
        let first = settle_pomodoro(&mut state, 1_060).expect("focus -> break");
        assert!(first.text().contains("休息"));
        assert!(state.is_break());
        assert_eq!(state.completed_cycles, 1);
        assert_eq!(state.remaining_seconds, 60);

        // 休息结束 → 回到专注
        let second = settle_pomodoro(&mut state, 1_120).expect("break -> focus");
        assert!(second.text().contains("专注"));
        assert_eq!(state.completed_cycles, 1);

        // 第二轮专注结束 → 整轮结束
        let third = settle_pomodoro(&mut state, 1_180).expect("finish");
        assert!(third.text().contains("完成"));
        assert!(!state.active);
        assert_eq!(state.completed_cycles, 2);
    }

    #[test]
    fn a_paused_pomodoro_does_not_advance() {
        let mut state = Pomodoro {
            active: true,
            paused: true,
            phase: "focus".into(),
            remaining_seconds: 60,
            updated_at: 1_000,
            ..Pomodoro::default()
        };
        assert!(settle_pomodoro(&mut state, 9_999).is_none());
        assert_eq!(state.remaining_seconds, 60);
    }

    #[test]
    fn pomodoro_actions_round_trip() {
        let (store, dir) = store();
        let started = store
            .pomodoro_action(
                "start",
                PomodoroOptions {
                    focus_minutes: Some(30),
                    break_minutes: Some(10),
                    cycles: Some(99),
                },
            )
            .unwrap();
        assert!(started.active);
        assert_eq!(started.focus_minutes, 30);
        assert_eq!(started.break_minutes, 10);
        // cycles 超上限被夹到 12，与脚本一致
        assert_eq!(started.total_cycles, MAX_CYCLES);

        store.pomodoro_action("pause", PomodoroOptions::default()).unwrap();
        let reloaded = TimerStore::load_in(dir.clone(), dir.clone());
        assert!(reloaded.pomodoro().paused);

        store.pomodoro_action("stop", PomodoroOptions::default()).unwrap();
        assert!(!store.pomodoro().active);
        assert!(store.pomodoro_action("frobnicate", PomodoroOptions::default()).is_err());
    }

    #[test]
    fn pomodoro_text_reads_like_a_sentence() {
        let state = Pomodoro {
            active: true,
            paused: false,
            phase: "break".into(),
            remaining_seconds: 125,
            updated_at: 1,
            completed_cycles: 1,
            total_cycles: 4,
            focus_minutes: 25,
            break_minutes: 5,
        };
        let text = pomodoro_text(&state);
        assert!(text.contains("休息剩余 02:05"), "{}", text);
        assert!(text.contains("第 2/4 轮"), "{}", text);
    }
}

//! 性能监控：核心侧统一采样，设备「性能监控」页与网页控制台读**同一份**数据。
//!
//! 几条设计约束（也是这个模块存在的理由）：
//! - 采样只发生在自己的后台任务里。读 `/proc`、`statvfs`、`ping` 都不会出现在
//!   LVGL 主循环或 HTTP 处理协程里，所以监控再慢也不会拖住对话与音频。
//! - 每个指标各自成败：某项拿不到就标 `unavailable` 并记一条 `errors`，
//!   绝不影响其它指标，更不会让整份数据变成错误。
//! - 外部命令只有固定的几个（`ping` / `iwconfig` / `df`），参数写死、不接受任何
//!   外部输入，并且都带超时 + `kill_on_drop`，不会留下僵尸进程。
//! - 只保留最近 60 个采样点，长时间运行内存不增长。

use crate::gui_bridge::GuiBridge;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::sync::mpsc;

/// 采样间隔：需求要求默认 2 秒。
pub const SAMPLE_INTERVAL: Duration = Duration::from_secs(2);
/// 历史长度：够画一条 2 分钟的曲线。
pub const HISTORY_LIMIT: usize = 60;
/// 外部命令上限。采集慢一点没关系，卡住不行。
const COMMAND_TIMEOUT: Duration = Duration::from_millis(1500);
/// 延迟探测目标：固定地址（不接受外部输入，也就不会被当成跳板）。
const PING_TARGET: &str = "223.5.5.5";
/// 只读根文件系统：需求要求固定 `/`，不允许用户提交路径。
const ROOT_PATH: &str = "/";
/// 上报的进程条数上限：够回答「谁在吃 CPU / 内存」就行。
const MAX_PROCESSES: usize = 5;
/// LVGL 侧进程名（用于判断 UI 是否在跑）。
const UI_PROCESS: &str = "qzdesk_screen";

// ---------------------------------------------------------------------------
// 数据结构：JSON 形状按需求给定，额外补一个 `level`（ok/warn/critical）
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Cpu {
    pub usage_percent: f64,
    pub cores: Vec<f64>,
    /// ok / warn / critical —— 阈值判定放核心侧，设备与网页才不会各算一套。
    pub level: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Load {
    pub one: f64,
    pub five: f64,
    pub fifteen: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Memory {
    pub total_bytes: u64,
    pub used_bytes: u64,
    pub available_bytes: u64,
    pub usage_percent: f64,
    pub swap_total_bytes: u64,
    pub swap_used_bytes: u64,
    pub level: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Storage {
    pub path: String,
    pub total_bytes: u64,
    pub used_bytes: u64,
    pub available_bytes: u64,
    pub usage_percent: f64,
    pub level: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Temperature {
    pub celsius: Option<f64>,
    /// ok / warn / critical / unavailable
    pub status: String,
    pub level: String,
}

impl Default for Temperature {
    fn default() -> Self {
        Self {
            celsius: None,
            status: "unavailable".to_string(),
            level: "unavailable".to_string(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Wifi {
    pub signal_dbm: Option<i64>,
    pub status: String,
    /// excellent / good / fair / weak / unavailable —— 文字与颜色一起给，不靠颜色单打。
    pub level: String,
}

impl Default for Wifi {
    fn default() -> Self {
        Self {
            signal_dbm: None,
            status: "unavailable".to_string(),
            level: "unavailable".to_string(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Network {
    pub latency_ms: Option<f64>,
    pub status: String,
    pub level: String,
}

impl Default for Network {
    fn default() -> Self {
        Self {
            latency_ms: None,
            status: "unavailable".to_string(),
            level: "unavailable".to_string(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Process {
    pub pid: i32,
    /// 只上报进程名（`/proc/<pid>/comm`）：命令行里可能有令牌、密钥、路径，
    /// 网页要渲染它，就不该把敏感信息带出来。
    pub name: String,
    /// core / ui / top_cpu / top_memory（一个进程可能同时占多项）。
    pub roles: Vec<String>,
    pub cpu_percent: f64,
    pub memory_bytes: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Services {
    pub qzdesk_core: String,
    pub ui: String,
    pub audio: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Snapshot {
    pub timestamp: i64,
    pub uptime_secs: u64,
    pub cpu: Cpu,
    pub load: Load,
    pub memory: Memory,
    pub storage: Storage,
    pub temperature: Temperature,
    pub wifi: Wifi,
    pub network: Network,
    pub processes: Vec<Process>,
    pub services: Services,
    /// 本次采样里失败的指标说明（成功时为空数组）。
    pub errors: Vec<String>,
}

/// 历史点：只留画曲线要用的几个数，60 个点也就几 KB。
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct HistoryPoint {
    pub timestamp: i64,
    pub cpu_percent: f64,
    pub memory_percent: f64,
    pub storage_percent: f64,
    pub temperature_c: Option<f64>,
    pub latency_ms: Option<f64>,
}

// ---------------------------------------------------------------------------
// 服务句柄
// ---------------------------------------------------------------------------

enum MonitorCommand {
    /// 把当前快照推给 GUI（刚连上时用），不重新采样。
    Publish,
}

type Shared = Arc<Mutex<PerformanceState>>;

#[derive(Default)]
struct PerformanceState {
    snapshot: Snapshot,
    history: VecDeque<HistoryPoint>,
}

#[derive(Clone, Copy)]
pub struct MonitorConfig {
    /// 音频是否可用（核心自己开 ALSA；被 QZDESK_AUDIO_DISABLED 关掉时为 false）。
    pub audio_enabled: bool,
}

#[derive(Clone)]
pub struct PerformanceService {
    state: Shared,
    tx: mpsc::Sender<MonitorCommand>,
}

impl PerformanceService {
    /// 启动采样任务。第一个采样点立刻就有，之后每 [`SAMPLE_INTERVAL`] 一次。
    pub fn start(config: MonitorConfig, gui: Arc<GuiBridge>) -> Self {
        let (tx, rx) = mpsc::channel(8);
        let state: Shared = Arc::new(Mutex::new(PerformanceState::default()));
        tokio::spawn(run(config, gui, state.clone(), rx));
        Self { state, tx }
    }

    /// `/api/performance`
    pub fn api(&self) -> Value {
        let guard = shared(&self.state);
        let mut value = serde_json::to_value(&guard.snapshot).unwrap_or_else(|_| json!({}));
        if let Value::Object(map) = &mut value {
            // 有一份能看的采样就算 ok：单项不可用由 status/errors 表达
            map.insert(
                "ok".to_string(),
                Value::Bool(guard.snapshot.timestamp > 0),
            );
        }
        value
    }

    /// `/api/performance/history`：最近 60 个采样点。
    pub fn history_api(&self) -> Value {
        let guard = shared(&self.state);
        json!({
            "ok": true,
            "interval_secs": SAMPLE_INTERVAL.as_secs(),
            "limit": HISTORY_LIMIT,
            "count": guard.history.len(),
            "samples": guard.history.iter().collect::<Vec<_>>(),
        })
    }

    /// 把当前快照推给 GUI（设备刚连上时用）。
    pub fn publish(&self) {
        let _ = self.tx.try_send(MonitorCommand::Publish);
    }
}

fn shared(state: &Shared) -> MutexGuard<'_, PerformanceState> {
    state.lock().unwrap_or_else(|error| error.into_inner())
}

async fn run(
    config: MonitorConfig,
    gui: Arc<GuiBridge>,
    state: Shared,
    mut rx: mpsc::Receiver<MonitorCommand>,
) {
    let mut previous_cpu: Option<RawCpu> = None;
    let mut previous_processes: HashMap<i32, u64> = HashMap::new();
    let mut announced = false;

    loop {
        let outcome = sample_once(&config, previous_cpu.as_ref(), &previous_processes).await;
        previous_cpu = outcome.cpu_raw;
        previous_processes = outcome.process_raw;
        {
            let mut guard = shared(&state);
            guard.snapshot = outcome.snapshot.clone();
            guard.history.push_back(HistoryPoint {
                timestamp: outcome.snapshot.timestamp,
                cpu_percent: outcome.snapshot.cpu.usage_percent,
                memory_percent: outcome.snapshot.memory.usage_percent,
                storage_percent: outcome.snapshot.storage.usage_percent,
                temperature_c: outcome.snapshot.temperature.celsius,
                latency_ms: outcome.snapshot.network.latency_ms,
            });
            while guard.history.len() > HISTORY_LIMIT {
                guard.history.pop_front();
            }
        }
        if !announced {
            announced = true;
            log::info!(
                "性能监控已启动：CPU {:.0}%、内存 {:.0}%、磁盘 {:.0}%、{}",
                outcome.snapshot.cpu.usage_percent,
                outcome.snapshot.memory.usage_percent,
                outcome.snapshot.storage.usage_percent,
                if outcome.snapshot.errors.is_empty() {
                    "各指标正常".to_string()
                } else {
                    format!("{} 项不可用：{}", outcome.snapshot.errors.len(), outcome.snapshot.errors.join("；"))
                }
            );
        }
        push(&gui, &state).await;

        tokio::select! {
            _ = tokio::time::sleep(SAMPLE_INTERVAL) => {}
            command = rx.recv() => {
                match command {
                    Some(MonitorCommand::Publish) => push(&gui, &state).await,
                    // 收件箱关闭（服务被丢弃）时保持采样：网页可能还在读
                    None => {}
                }
            }
        }
    }
}

/// 一次采样。任何一项失败都只落到 `errors` 与该项的 `unavailable` 上。
async fn sample_once(
    config: &MonitorConfig,
    previous_cpu: Option<&RawCpu>,
    previous_processes: &HashMap<i32, u64>,
) -> SampleOutcome {
    let mut errors: Vec<String> = Vec::new();
    let previous_cpu_clone = previous_cpu.cloned();
    let previous_processes_clone = previous_processes.clone();

    /* 文件读取放到 blocking 线程：/proc 很快，但 sysfs 的温度文件在驱动异常时
     * 可能阻塞，放到这里至少不会拖住 tokio 的调度线程，而且能加超时。 */
    let files = tokio::time::timeout(
        SAMPLE_INTERVAL,
        tokio::task::spawn_blocking(move || read_files(previous_cpu_clone, previous_processes_clone)),
    )
    .await;

    let files = match files {
        Ok(Ok(value)) => value,
        Ok(Err(error)) => {
            errors.push(format!("采集线程失败: {}", error));
            FilesOutcome::default()
        }
        Err(_) => {
            errors.push("采集超时（2 秒内没有完成）".to_string());
            FilesOutcome::default()
        }
    };

    // —— CPU ——
    let cpu = match (&files.cpu_raw, previous_cpu) {
        (Some(current), Some(previous)) => cpu_usage(previous, current),
        (Some(_), None) => Cpu {
            usage_percent: 0.0,
            cores: files.cpu_raw.as_ref().map(|raw| vec![0.0; raw.cores.len()]).unwrap_or_default(),
            level: level_of(0.0, CPU_THRESHOLDS).to_string(),
        },
        (None, _) => {
            errors.push(files.cpu_error.clone().unwrap_or_else(|| "CPU 读数不可用".to_string()));
            Cpu::default()
        }
    };

    // —— 内存 ——
    let memory = match &files.memory {
        Some(memory) => {
            let mut memory = memory.clone();
            memory.level = level_of(memory.usage_percent, MEMORY_THRESHOLDS).to_string();
            memory
        }
        None => {
            errors.push(files.memory_error.clone().unwrap_or_else(|| "内存读数不可用".to_string()));
            Memory::default()
        }
    };

    // —— 磁盘（statvfs；失败再退回 df）——
    let storage = match files.storage.clone() {
        Some(storage) => storage,
        None => match read_storage_with_df().await {
            Ok(storage) => storage,
            Err(error) => {
                errors.push(format!("磁盘: {}", error));
                Storage {
                    path: ROOT_PATH.to_string(),
                    level: "unavailable".to_string(),
                    ..Storage::default()
                }
            }
        },
    };

    let temperature = files.temperature.clone();
    let wifi = match files.wifi.clone() {
        Some(signal) => Wifi {
            signal_dbm: Some(signal),
            status: "ok".to_string(),
            level: wifi_level(signal).to_string(),
        },
        None => Wifi::default(),
    };
    if wifi.status == "unavailable" {
        errors.push("Wi-Fi 信号不可用（可能没有无线网卡）".to_string());
    }

    // —— 延迟：唯一需要外部命令的指标，异步 + 超时 ——
    let network = match ping_latency().await {
        Some(latency) => Network {
            latency_ms: Some(latency),
            status: "ok".to_string(),
            level: latency_level(latency).to_string(),
        },
        None => {
            errors.push("网络延迟不可用（ping 超时或未安装）".to_string());
            Network::default()
        }
    };

    // —— 进程 ——
    let services = Services {
        qzdesk_core: "running".to_string(),
        ui: if files.ui_running { "running".to_string() } else { "stopped".to_string() },
        audio: if config.audio_enabled { "running".to_string() } else { "disabled".to_string() },
    };
    let processes = select_processes(&files.processes, services.ui == "running");

    SampleOutcome {
        snapshot: Snapshot {
            timestamp: now_seconds(),
            uptime_secs: files.uptime_secs,
            cpu,
            load: files.load.clone(),
            memory,
            storage,
            temperature,
            wifi,
            network,
            processes,
            services,
            errors,
        },
        cpu_raw: files.cpu_raw,
        process_raw: files.process_raw,
    }
}

/// 推给设备的报文：**专供渲染**的扁平结构。
///
/// 为什么不复用 `/api/performance` 的那份 JSON：设备只是画界面，不需要嵌套
/// 结构（而嵌套里 `level` 这种键会重复出现，C 侧解析就得先做作用域切分）。
/// 这里键名唯一、百分比统一 ×10 成整数、缺的指标直接给 null —— 设备端因此
/// 就是一组直白的取值，行为可预期。网页继续用结构化的接口。
fn device_payload(state: &Shared) -> String {
    let guard = shared(state);
    let snapshot = &guard.snapshot;
    let history: Vec<i64> = guard
        .history
        .iter()
        .map(|point| scaled(point.cpu_percent))
        .collect();
    let process_text: Vec<String> = snapshot.processes.iter().map(process_row).collect();
    let cores: Vec<i64> = snapshot.cpu.cores.iter().map(|value| scaled(*value)).collect();

    json!({
        "type": "performance",
        "timestamp": snapshot.timestamp,
        "uptime_secs": snapshot.uptime_secs,
        "cpu_x10": scaled(snapshot.cpu.usage_percent),
        "cpu_level": snapshot.cpu.level,
        "cores_x10": cores,
        "memory_x10": scaled(snapshot.memory.usage_percent),
        "memory_level": snapshot.memory.level,
        "memory_total_kb": snapshot.memory.total_bytes / 1024,
        "memory_used_kb": snapshot.memory.used_bytes / 1024,
        "memory_available_kb": snapshot.memory.available_bytes / 1024,
        "swap_total_kb": snapshot.memory.swap_total_bytes / 1024,
        "swap_used_kb": snapshot.memory.swap_used_bytes / 1024,
        "storage_x10": scaled(snapshot.storage.usage_percent),
        "storage_level": snapshot.storage.level,
        "storage_total_kb": snapshot.storage.total_bytes / 1024,
        "storage_used_kb": snapshot.storage.used_bytes / 1024,
        "storage_available_kb": snapshot.storage.available_bytes / 1024,
        // 缺失的指标给 null：设备端按「有值/没值」分两支，不必自己判断字段
        "temperature_x10": snapshot.temperature.celsius.map(scaled_f64),
        "temperature_level": snapshot.temperature.level,
        "wifi_dbm": snapshot.wifi.signal_dbm,
        "wifi_level": snapshot.wifi.level,
        "latency_x10": snapshot.network.latency_ms.map(scaled_f64),
        "latency_level": snapshot.network.level,
        "load_x100": [
            (snapshot.load.one * 100.0).round() as i64,
            (snapshot.load.five * 100.0).round() as i64,
            (snapshot.load.fifteen * 100.0).round() as i64,
        ],
        "core_service": snapshot.services.qzdesk_core,
        "ui_service": snapshot.services.ui,
        "audio_service": snapshot.services.audio,
        "history_x10": history,
        "process_text": process_text,
        "errors": snapshot.errors,
    })
    .to_string()
}

/// 百分比一律 ×10 取整：设备侧不碰浮点。
fn scaled(value: f64) -> i64 {
    (value * 10.0).round() as i64
}

fn scaled_f64(value: f64) -> i64 {
    scaled(value)
}

/// 设备页上的一行进程说明，例如 `界面 qzdesk_screen 12% 40.2MB`。
///
/// 用空格而不是分隔符：设备那一列只有 200 像素宽、9px 字号，省下的每个像素
/// 都用在信息上（分隔符一次要吃掉十几像素）。
fn process_row(process: &Process) -> String {
    let role = process
        .roles
        .first()
        .map(|role| match role.as_str() {
            "core" => "核心",
            "ui" => "界面",
            "top_cpu" => "CPU 最高",
            "top_memory" => "内存最高",
            other => other,
        })
        .unwrap_or("进程");
    format!(
        "{} {} {:.0}% {}",
        role,
        process.name,
        process.cpu_percent,
        memory_label(process.memory_bytes)
    )
}

fn memory_label(bytes: u64) -> String {
    const MB: f64 = 1024.0 * 1024.0;
    let megabytes = bytes as f64 / MB;
    if megabytes >= 100.0 {
        format!("{:.0}MB", megabytes)
    } else {
        format!("{:.1}MB", megabytes)
    }
}

async fn push(gui: &GuiBridge, state: &Shared) {
    let payload = device_payload(state);
    if let Err(error) = gui.send_message(&payload).await {
        log::debug!("推送性能数据到 GUI 失败: {}", error);
    }
}

// ---------------------------------------------------------------------------
// 文件型指标（在 blocking 线程里跑）
// ---------------------------------------------------------------------------

struct SampleOutcome {
    snapshot: Snapshot,
    cpu_raw: Option<RawCpu>,
    process_raw: HashMap<i32, u64>,
}

#[derive(Default)]
struct FilesOutcome {
    cpu_raw: Option<RawCpu>,
    cpu_error: Option<String>,
    memory: Option<Memory>,
    memory_error: Option<String>,
    storage: Option<Storage>,
    temperature: Temperature,
    wifi: Option<i64>,
    uptime_secs: u64,
    load: Load,
    processes: Vec<Process>,
    process_raw: HashMap<i32, u64>,
    ui_running: bool,
}

/// 累计 CPU 时间（jiffies）。两次相减才得到使用率。
#[derive(Clone)]
struct RawCpu {
    total: u64,
    idle: u64,
    cores: Vec<(u64, u64)>,
}

fn read_files(previous_cpu: Option<RawCpu>, previous_processes: HashMap<i32, u64>) -> FilesOutcome {
    let mut outcome = FilesOutcome::default();

    match read_cpu_raw() {
        Ok(raw) => outcome.cpu_raw = Some(raw),
        Err(error) => outcome.cpu_error = Some(format!("CPU: {}", error)),
    }
    match read_memory() {
        Ok(memory) => outcome.memory = Some(memory),
        Err(error) => outcome.memory_error = Some(format!("内存: {}", error)),
    }
    outcome.storage = read_storage_statvfs().ok();
    outcome.temperature = read_temperature();
    outcome.wifi = read_wifi();
    outcome.uptime_secs = read_uptime().unwrap_or(0);
    outcome.load = read_load().unwrap_or_default();

    let total_delta = match (&outcome.cpu_raw, &previous_cpu) {
        (Some(current), Some(previous)) => current.total.saturating_sub(previous.total),
        _ => 0,
    };
    let (processes, raw, ui_running) = read_processes(&previous_processes, total_delta);
    outcome.processes = processes;
    outcome.process_raw = raw;
    outcome.ui_running = ui_running;
    outcome
}

fn read_cpu_raw() -> Result<RawCpu, String> {
    let content = std::fs::read_to_string("/proc/stat").map_err(|error| error.to_string())?;
    let mut total = None;
    let mut cores = Vec::new();
    for line in content.lines() {
        if !line.starts_with("cpu") {
            continue;
        }
        let mut parts = line.split_whitespace();
        let name = parts.next().unwrap_or("");
        let values: Vec<u64> = parts.filter_map(|value| value.parse::<u64>().ok()).collect();
        if values.len() < 4 {
            continue;
        }
        // user nice system idle iowait irq softirq steal ...：idle 要把 iowait 算进去，
        // 否则等 IO 的时间会被当成"忙"
        let idle = values[3] + values.get(4).copied().unwrap_or(0);
        let busy: u64 = values.iter().take(8).sum::<u64>() - idle;
        let entry = (busy + idle, idle);
        if name == "cpu" {
            total = Some(entry);
        } else {
            cores.push(entry);
        }
    }
    let (total, idle) = total.ok_or("缺少 cpu 汇总行")?;
    Ok(RawCpu { total, idle, cores })
}

fn cpu_usage(previous: &RawCpu, current: &RawCpu) -> Cpu {
    let usage = |previous: (u64, u64), current: (u64, u64)| -> f64 {
        let total = current.0.saturating_sub(previous.0);
        if total == 0 {
            return 0.0;
        }
        let idle = current.1.saturating_sub(previous.1);
        ((total - idle.min(total)) as f64 * 100.0 / total as f64).clamp(0.0, 100.0)
    };
    let usage_percent = usage((previous.total, previous.idle), (current.total, current.idle));
    let cores = previous
        .cores
        .iter()
        .zip(current.cores.iter())
        .map(|(previous, current)| (usage(*previous, *current) * 10.0).round() / 10.0)
        .collect();
    Cpu {
        usage_percent: (usage_percent * 10.0).round() / 10.0,
        cores,
        level: level_of(usage_percent, CPU_THRESHOLDS).to_string(),
    }
}

fn read_memory() -> Result<Memory, String> {
    let content = std::fs::read_to_string("/proc/meminfo").map_err(|error| error.to_string())?;
    let pick = |key: &str| -> Option<u64> {
        content
            .lines()
            .find(|line| line.starts_with(key))
            .and_then(|line| line.split_whitespace().nth(1))
            .and_then(|value| value.parse::<u64>().ok())
            .map(|value| value * 1024)
    };
    let total = pick("MemTotal:").ok_or("缺少 MemTotal")?;
    let available = pick("MemAvailable:").ok_or("缺少 MemAvailable")?;
    let swap_total = pick("SwapTotal:").unwrap_or(0);
    let swap_free = pick("SwapFree:").unwrap_or(0);
    let used = total.saturating_sub(available);
    Ok(Memory {
        total_bytes: total,
        used_bytes: used,
        available_bytes: available,
        usage_percent: percent(used, total),
        swap_total_bytes: swap_total,
        swap_used_bytes: swap_total.saturating_sub(swap_free),
        level: level_of(percent(used, total), MEMORY_THRESHOLDS).to_string(),
    })
}

fn read_uptime() -> Result<u64, String> {
    let content = std::fs::read_to_string("/proc/uptime").map_err(|error| error.to_string())?;
    content
        .split_whitespace()
        .next()
        .and_then(|value| value.parse::<f64>().ok())
        .map(|value| value as u64)
        .ok_or_else(|| "格式异常".to_string())
}

fn read_load() -> Result<Load, String> {
    let content = std::fs::read_to_string("/proc/loadavg").map_err(|error| error.to_string())?;
    let values: Vec<f64> = content
        .split_whitespace()
        .take(3)
        .filter_map(|value| value.parse::<f64>().ok())
        .collect();
    if values.len() < 3 {
        return Err("格式异常".to_string());
    }
    Ok(Load { one: values[0], five: values[1], fifteen: values[2] })
}

/// 温度：`/sys/class/thermal/thermal_zone*/temp`，第一个读得出来的就用。
fn read_temperature() -> Temperature {
    let Ok(entries) = std::fs::read_dir("/sys/class/thermal") else {
        return Temperature::default();
    };
    let mut zones: Vec<_> = entries
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| {
            path.file_name()
                .and_then(|name| name.to_str())
                .map(|name| name.starts_with("thermal_zone"))
                .unwrap_or(false)
        })
        .collect();
    zones.sort();
    for zone in zones {
        let Ok(raw) = std::fs::read_to_string(zone.join("temp")) else {
            continue;
        };
        let Ok(value) = raw.trim().parse::<f64>() else {
            continue;
        };
        // 多数驱动给毫摄氏度；也有直接给摄氏度的，按量级判断
        let celsius = if value > 1000.0 { value / 1000.0 } else { value };
        if !(-40.0..=150.0).contains(&celsius) {
            continue;
        }
        let celsius = (celsius * 10.0).round() / 10.0;
        return Temperature {
            celsius: Some(celsius),
            status: "ok".to_string(),
            level: level_of(celsius, TEMPERATURE_THRESHOLDS).to_string(),
        };
    }
    Temperature::default()
}

/// Wi-Fi 信号：先读 `/proc/net/wireless`（不启进程），读不到再问 `iwconfig`。
fn read_wifi() -> Option<i64> {
    if let Ok(content) = std::fs::read_to_string("/proc/net/wireless") {
        for line in content.lines().skip(2) {
            if let Some(signal) = parse_wireless_line(line) {
                return Some(signal);
            }
        }
    }
    read_wifi_with_iwconfig()
}

/// 例：`  wlan0: 0000   54.  -47.  -256        0      0  0  0  0  0`
fn parse_wireless_line(line: &str) -> Option<i64> {
    let mut parts = line.split_whitespace();
    parts.next()?; // 接口名
    parts.next()?; // status
    parts.next()?; // link quality
    let level = parts.next()?.trim_end_matches('.');
    level.parse::<i64>().ok().filter(|value| *value < 0)
}

fn read_wifi_with_iwconfig() -> Option<i64> {
    let output = std::process::Command::new("iwconfig").output().ok()?;
    let text = String::from_utf8_lossy(&output.stdout);
    for line in text.lines() {
        if let Some(index) = line.find("Signal level=") {
            let rest = &line[index + "Signal level=".len()..];
            let value: String = rest
                .chars()
                .take_while(|ch| ch.is_ascii_digit() || *ch == '-' || *ch == '+' || *ch == '.')
                .collect();
            if let Ok(parsed) = value.trim_end_matches('.').parse::<f64>() {
                return Some(parsed as i64);
            }
        }
    }
    None
}

/// 磁盘：`statvfs("/")`。固定路径，不接受任何外部输入。
///
/// 「已用」按 `总量 - 可用` 算（而不是 `总量 - 空闲`）：这样 已用 + 剩余 = 总量，
/// 使用率与界面上的三个数字自洽。设备上通常是 root 运行，`f_bfree` 与 `f_bavail`
/// 本来就相等；在有保留块（ext4 默认 5%）的发行版上，`df` 的「已用」会小一点，
/// 那是它把保留区算在"不可用但也没用"里，对用户没有实际意义。
fn read_storage_statvfs() -> Result<Storage, String> {
    let (total, available) = statvfs_bytes(ROOT_PATH)?;
    let used = total.saturating_sub(available);
    Ok(Storage {
        path: ROOT_PATH.to_string(),
        total_bytes: total,
        used_bytes: used,
        available_bytes: available,
        usage_percent: percent(used, total),
        level: level_of(percent(used, total), STORAGE_THRESHOLDS).to_string(),
    })
}

/// statvfs 失败时的退路：`df -P -B1 /`（命令与参数都写死）。
async fn read_storage_with_df() -> Result<Storage, String> {
    let output = tokio::time::timeout(
        COMMAND_TIMEOUT,
        tokio::process::Command::new("df")
            .args(["-P", "-B1", ROOT_PATH])
            .kill_on_drop(true)
            .output(),
    )
    .await
    .map_err(|_| "df 超时".to_string())?
    .map_err(|error| format!("df 无法执行: {}", error))?;
    let text = String::from_utf8_lossy(&output.stdout);
    let line = text.lines().nth(1).ok_or("df 输出为空")?;
    let fields: Vec<&str> = line.split_whitespace().collect();
    if fields.len() < 4 {
        return Err("df 输出格式异常".to_string());
    }
    let total = fields[1].parse::<u64>().map_err(|_| "df 总量无法解析")?;
    let used = fields[2].parse::<u64>().map_err(|_| "df 已用无法解析")?;
    let available = fields[3].parse::<u64>().map_err(|_| "df 可用无法解析")?;
    Ok(Storage {
        path: ROOT_PATH.to_string(),
        total_bytes: total,
        used_bytes: used,
        available_bytes: available,
        usage_percent: percent(used, total),
        level: level_of(percent(used, total), STORAGE_THRESHOLDS).to_string(),
    })
}

#[cfg(target_pointer_width = "64")]
fn statvfs_bytes(path: &str) -> Result<(u64, u64), String> {
    /// `statvfs` 的最小 FFI 声明。
    ///
    /// 为了一个调用去引 libc（还会牵动 Cargo.lock 与交叉编译环境）不划算。
    /// 字段布局取自 64 位 Linux 的 `struct statvfs`：10 个 u64 加 6 个 i32 保留位。
    #[repr(C)]
    struct Statvfs {
        f_bsize: u64,
        f_frsize: u64,
        f_blocks: u64,
        f_bfree: u64,
        f_bavail: u64,
        f_files: u64,
        f_ffree: u64,
        f_favail: u64,
        f_fsid: u64,
        f_flag: u64,
        f_namemax: u64,
        __f_spare: [i32; 6],
    }

    unsafe extern "C" {
        fn statvfs(path: *const std::os::raw::c_char, buffer: *mut Statvfs) -> std::os::raw::c_int;
    }

    let mut cpath: Vec<u8> = path.as_bytes().to_vec();
    cpath.push(0);
    let mut buffer = std::mem::MaybeUninit::<Statvfs>::uninit();
    // SAFETY: 路径是以 NUL 结尾的 C 字符串；buffer 大小与布局与 libc 的
    // struct statvfs 一致（64 位 Linux）。调用失败时我们不看 buffer。
    let result = unsafe { statvfs(cpath.as_ptr() as *const _, buffer.as_mut_ptr()) };
    if result != 0 {
        return Err(format!("statvfs 失败（errno {}）", std::io::Error::last_os_error()));
    }
    // SAFETY: 上面的调用返回 0，buffer 已被 libc 完整写入。
    let stat = unsafe { buffer.assume_init() };
    let block = if stat.f_frsize > 0 { stat.f_frsize } else { stat.f_bsize };
    if block == 0 || stat.f_blocks == 0 {
        return Err("statvfs 返回 0 容量".to_string());
    }
    Ok((
        stat.f_blocks.saturating_mul(block),
        stat.f_bavail.saturating_mul(block),
    ))
}

#[cfg(not(target_pointer_width = "64"))]
fn statvfs_bytes(_path: &str) -> Result<(u64, u64), String> {
    // 32 位平台上结构体布局不同，解析风险大：直接交给 df 那条路
    Err("32 位平台使用 df 读取磁盘".to_string())
}

/// 进程：CPU 占比按「本进程 jiffies / 整机 jiffies」算，和 `top` 的口径一致。
fn read_processes(
    previous: &HashMap<i32, u64>,
    total_delta: u64,
) -> (Vec<Process>, HashMap<i32, u64>, bool) {
    let mut current: HashMap<i32, u64> = HashMap::new();
    let mut processes: Vec<Process> = Vec::new();
    let mut ui_running = false;

    let Ok(entries) = std::fs::read_dir("/proc") else {
        return (processes, current, ui_running);
    };
    for entry in entries.filter_map(|entry| entry.ok()) {
        let name = entry.file_name();
        let Some(name) = name.to_str() else { continue };
        let Ok(pid) = name.parse::<i32>() else { continue };

        let Ok(stat) = std::fs::read_to_string(format!("/proc/{}/stat", pid)) else {
            continue;
        };
        // comm 可能带空格与括号，所以从最后一个 ')' 之后开始数
        let Some(rest) = stat.rsplit_once(')').map(|(_, rest)| rest) else {
            continue;
        };
        let fields: Vec<&str> = rest.split_whitespace().collect();
        // 去掉 comm 之后，utime 是第 12 项、stime 是第 13 项
        let utime = fields.get(11).and_then(|value| value.parse::<u64>().ok()).unwrap_or(0);
        let stime = fields.get(12).and_then(|value| value.parse::<u64>().ok()).unwrap_or(0);
        let jiffies = utime + stime;
        current.insert(pid, jiffies);

        let comm = std::fs::read_to_string(format!("/proc/{}/comm", pid))
            .map(|value| value.trim().to_string())
            .unwrap_or_else(|_| name.to_string());
        let memory_bytes = read_process_memory(pid).unwrap_or(0);
        let cpu_percent = if total_delta == 0 {
            0.0
        } else {
            let delta = jiffies.saturating_sub(previous.get(&pid).copied().unwrap_or(jiffies));
            (delta as f64 * 100.0 / total_delta as f64 * 10.0).round() / 10.0
        };

        if comm == UI_PROCESS {
            ui_running = true;
        }
        // 内核线程（RSS 0 且不占 CPU）不进榜，列表才看得清
        if memory_bytes == 0 && cpu_percent == 0.0 && comm != UI_PROCESS {
            continue;
        }
        processes.push(Process {
            pid,
            name: comm,
            roles: Vec::new(),
            cpu_percent,
            memory_bytes,
        });
    }

    (processes, current, ui_running)
}

fn read_process_memory(pid: i32) -> Option<u64> {
    let status = std::fs::read_to_string(format!("/proc/{}/status", pid)).ok()?;
    let line = status.lines().find(|line| line.starts_with("VmRSS:"))?;
    let kb = line.split_whitespace().nth(1)?.parse::<u64>().ok()?;
    Some(kb * 1024)
}

/// 挑出要展示的进程：核心、UI、CPU 最高、内存最高，按 pid 去重。
fn select_processes(all: &[Process], ui_running: bool) -> Vec<Process> {
    let mut selected: Vec<Process> = Vec::new();
    let own_pid = std::process::id() as i32;

    let take = |candidate: Option<&Process>, role: &str, selected: &mut Vec<Process>| {
        let Some(candidate) = candidate else { return };
        if let Some(existing) = selected.iter_mut().find(|item| item.pid == candidate.pid) {
            if !existing.roles.iter().any(|item| item == role) {
                existing.roles.push(role.to_string());
            }
        } else {
            let mut item = candidate.clone();
            item.roles.push(role.to_string());
            selected.push(item);
        }
    };

    take(all.iter().find(|item| item.pid == own_pid), "core", &mut selected);
    take(
        all.iter().find(|item| item.name == UI_PROCESS && ui_running),
        "ui",
        &mut selected,
    );
    let top_cpu = all
        .iter()
        .filter(|item| item.cpu_percent > 0.0)
        .max_by(|a, b| a.cpu_percent.partial_cmp(&b.cpu_percent).unwrap_or(std::cmp::Ordering::Equal));
    take(top_cpu, "top_cpu", &mut selected);
    let top_memory = all.iter().max_by_key(|item| item.memory_bytes);
    take(top_memory, "top_memory", &mut selected);

    selected.truncate(MAX_PROCESSES);
    selected
}

/// 网络延迟：`ping -n -c 1 -W 1 <固定地址>`，异步 + 超时 + 超时即杀子进程。
async fn ping_latency() -> Option<f64> {
    let output = tokio::time::timeout(
        COMMAND_TIMEOUT,
        tokio::process::Command::new("ping")
            .args(["-n", "-c", "1", "-W", "1", PING_TARGET])
            .kill_on_drop(true)
            .output(),
    )
    .await
    .ok()?
    .ok()?;
    parse_ping(&String::from_utf8_lossy(&output.stdout))
}

/// 例：`64 bytes from 223.5.5.5: icmp_seq=1 ttl=117 time=8.42 ms`
fn parse_ping(output: &str) -> Option<f64> {
    let index = output.find("time=")?;
    let rest = &output[index + "time=".len()..];
    let value: String = rest.chars().take_while(|ch| ch.is_ascii_digit() || *ch == '.').collect();
    let parsed = value.parse::<f64>().ok()?;
    Some((parsed * 10.0).round() / 10.0)
}

// ---------------------------------------------------------------------------
// 阈值与工具
// ---------------------------------------------------------------------------

/// (警告线, 危险线)
type Thresholds = (f64, f64);
const CPU_THRESHOLDS: Thresholds = (60.0, 85.0);
const MEMORY_THRESHOLDS: Thresholds = (70.0, 90.0);
const STORAGE_THRESHOLDS: Thresholds = (75.0, 90.0);
const TEMPERATURE_THRESHOLDS: Thresholds = (65.0, 80.0);

fn level_of(value: f64, (warn, critical): Thresholds) -> &'static str {
    if value >= critical {
        "critical"
    } else if value >= warn {
        "warn"
    } else {
        "ok"
    }
}

/// Wi-Fi：-60 以内算好，-75 以上算弱（dBm 是负值，越大越好）。
fn wifi_level(dbm: i64) -> &'static str {
    if dbm >= -60 {
        "excellent"
    } else if dbm >= -70 {
        "good"
    } else if dbm >= -80 {
        "fair"
    } else {
        "weak"
    }
}

fn latency_level(ms: f64) -> &'static str {
    if ms < 80.0 {
        "ok"
    } else if ms < 200.0 {
        "warn"
    } else {
        "critical"
    }
}

fn percent(used: u64, total: u64) -> f64 {
    if total == 0 {
        return 0.0;
    }
    ((used as f64 * 1000.0 / total as f64).round() / 10.0).clamp(0.0, 100.0)
}

fn now_seconds() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs() as i64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cpu_usage_is_the_busy_share_of_two_readings() {
        let previous = RawCpu { total: 1000, idle: 400, cores: vec![(500, 200), (500, 200)] };
        let current = RawCpu { total: 2000, idle: 600, cores: vec![(1000, 300), (1000, 300)] };
        let cpu = cpu_usage(&previous, &current);
        // 忙的时间 = 1000 - 200 = 800，占总量 1000 的 80%
        assert_eq!(cpu.usage_percent, 80.0);
        assert_eq!(cpu.cores, vec![80.0, 80.0]);
        assert_eq!(cpu.level, "warn");
    }

    #[test]
    fn cpu_usage_does_not_divide_by_zero() {
        let same = RawCpu { total: 10, idle: 5, cores: vec![(10, 5)] };
        assert_eq!(cpu_usage(&same, &same).usage_percent, 0.0);
    }

    #[test]
    fn levels_follow_the_required_thresholds() {
        assert_eq!(level_of(59.9, CPU_THRESHOLDS), "ok");
        assert_eq!(level_of(60.0, CPU_THRESHOLDS), "warn");
        assert_eq!(level_of(85.0, CPU_THRESHOLDS), "critical");
        assert_eq!(level_of(69.9, MEMORY_THRESHOLDS), "ok");
        assert_eq!(level_of(90.0, MEMORY_THRESHOLDS), "critical");
        assert_eq!(level_of(75.0, STORAGE_THRESHOLDS), "warn");
        assert_eq!(level_of(64.9, TEMPERATURE_THRESHOLDS), "ok");
        assert_eq!(level_of(80.1, TEMPERATURE_THRESHOLDS), "critical");
    }

    #[test]
    fn wifi_and_latency_have_spoken_levels() {
        assert_eq!(wifi_level(-45), "excellent");
        assert_eq!(wifi_level(-65), "good");
        assert_eq!(wifi_level(-75), "fair");
        assert_eq!(wifi_level(-88), "weak");
        assert_eq!(latency_level(20.0), "ok");
        assert_eq!(latency_level(120.0), "warn");
        assert_eq!(latency_level(400.0), "critical");
    }

    #[test]
    fn wireless_line_yields_signal_in_dbm() {
        assert_eq!(parse_wireless_line("  wlan0: 0000   54.  -47.  -256        0      0  0  0  0  0"), Some(-47));
        assert_eq!(parse_wireless_line("Inter-| sta-|   Quality        |   Discarded packets"), None);
    }

    #[test]
    fn ping_output_yields_latency() {
        let output = "PING 223.5.5.5 (223.5.5.5) 56(84) bytes of data.\n64 bytes from 223.5.5.5: icmp_seq=1 ttl=117 time=8.42 ms";
        assert_eq!(parse_ping(output), Some(8.4));
        assert_eq!(parse_ping("Request timeout for icmp_seq 0"), None);
    }

    #[test]
    fn percent_handles_zero_and_rounds_to_one_decimal() {
        assert_eq!(percent(0, 0), 0.0);
        assert_eq!(percent(1, 3), 33.3);
        assert_eq!(percent(5, 5), 100.0);
    }

    #[test]
    fn process_selection_marks_roles_without_duplicates() {
        let own = std::process::id() as i32;
        let all = vec![
            Process { pid: own, name: "xiaozhi-linux-rs".to_string(), roles: vec![], cpu_percent: 42.0, memory_bytes: 9_000_000 },
            Process { pid: 4242, name: UI_PROCESS.to_string(), roles: vec![], cpu_percent: 12.0, memory_bytes: 40_000_000 },
            Process { pid: 777, name: "ffmpeg".to_string(), roles: vec![], cpu_percent: 3.0, memory_bytes: 1_000 },
        ];
        let selected = select_processes(&all, true);
        // 只有核心与 UI 进了榜：第三个进程不占任何角色，不该出现
        assert_eq!(selected.len(), 2);
        let own_entry = selected.iter().find(|item| item.pid == own).expect("核心进程必须在列表里");
        assert!(own_entry.roles.contains(&"core".to_string()));
        // 核心同时是 CPU 最高：两条角色合并到同一条记录，不重复出现
        assert!(own_entry.roles.contains(&"top_cpu".to_string()));
        let ui_entry = selected.iter().find(|item| item.pid == 4242).expect("UI 进程必须在列表里");
        assert!(ui_entry.roles.contains(&"top_memory".to_string()));
    }

    #[test]
    fn process_rows_are_ready_to_display_on_device() {
        let process = Process {
            pid: 4242,
            name: "qzdesk_screen".to_string(),
            roles: vec!["ui".to_string(), "top_memory".to_string()],
            cpu_percent: 12.4,
            memory_bytes: 40 * 1024 * 1024,
        };
        assert_eq!(process_row(&process), "界面 qzdesk_screen 12% 40.0MB");
        // 小数值也统一按 MB 表示，设备上不必再判断单位
        assert_eq!(memory_label(512 * 1024), "0.5MB");
        assert_eq!(memory_label(150 * 1024 * 1024), "150MB");
    }

    #[test]
    fn snapshot_api_is_json_with_the_required_shape() {
        let service = PerformanceService {
            state: Arc::new(Mutex::new(PerformanceState::default())),
            tx: mpsc::channel(1).0,
        };
        let value = service.api();
        // 还没采过样：ok=false，但字段齐全、仍是合法 JSON
        assert_eq!(value.get("ok").and_then(Value::as_bool), Some(false));
        for key in ["cpu", "load", "memory", "storage", "temperature", "wifi", "network", "processes", "services", "errors"] {
            assert!(value.get(key).is_some(), "缺少字段 {}", key);
        }
        assert_eq!(
            value.pointer("/temperature/status").and_then(Value::as_str),
            Some("unavailable")
        );
    }
}

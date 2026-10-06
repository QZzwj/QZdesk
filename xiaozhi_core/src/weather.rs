use crate::gui_bridge::GuiBridge;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::sync::{mpsc, oneshot};

/// 缓存文件名，落在核心的工作目录（与 xiaozhi_config.json 同目录）。
const CACHE_FILE: &str = "weather_cache.json";
/// 单次请求的硬上限：需求要求不超过 8 秒，配置写大了也会被截到这里。
const MAX_TIMEOUT_SECS: u64 = 8;
/// 连续失败后的重试起点（秒），每失败一次翻倍，最多回到正常刷新间隔。
const RETRY_BASE_SECS: u64 = 15;
const FORECAST_URL: &str = "https://api.open-meteo.com/v1/forecast";

/// 经纬度的合法范围，越界即「位置配置错误」。
const LAT_RANGE: (f64, f64) = (-90.0, 90.0);
const LON_RANGE: (f64, f64) = (-180.0, 180.0);
/// 环境变量把经纬度写坏时的哨兵值：落在合法范围外，于是走「位置配置错误」，
const BAD_COORDINATE: f64 = 999.0;

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct WeatherConfig {
    #[serde(default = "default_enabled")]
    pub enabled: bool,
    #[serde(default)]
    pub latitude: Option<f64>,
    #[serde(default)]
    pub longitude: Option<f64>,
    #[serde(default)]
    pub city: String,
    /// 自动刷新间隔（分钟），同时也是缓存的有效期。
    #[serde(default = "default_refresh_minutes")]
    pub refresh_minutes: u64,
    /// 单次请求超时（秒），上限 [`MAX_TIMEOUT_SECS`]。
    #[serde(default = "default_timeout_secs")]
    pub timeout_secs: u64,
    /// 代理设置：空 = 直连（默认）；`env` = 沿用环境变量；否则当作代理地址。
    #[serde(default)]
    pub proxy: Option<String>,
}

impl Default for WeatherConfig {
    fn default() -> Self {
        Self {
            enabled: default_enabled(),
            latitude: None,
            longitude: None,
            city: String::new(),
            refresh_minutes: default_refresh_minutes(),
            timeout_secs: default_timeout_secs(),
            proxy: None,
        }
    }
}

impl WeatherConfig {
    fn refresh_delay(&self) -> Duration {
        Duration::from_secs(self.refresh_minutes.max(1) * 60)
    }

    fn timeout(&self) -> Duration {
        Duration::from_secs(self.timeout_secs.clamp(1, MAX_TIMEOUT_SECS))
    }
}

fn default_enabled() -> bool {
    true
}

fn default_refresh_minutes() -> u64 {
    10
}

fn default_timeout_secs() -> u64 {
    6
}

/// 对外快照：网页 `/api/weather`、推给 GUI 的 UDP 报文、本地缓存文件，
/// 三处都是这一个结构体。
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(default)]
pub struct WeatherSnapshot {
    /// `ok`（刚取到）/ `loading`（正在刷新，旧数据仍在）/ `stale`（沿用上一次）/
    /// `error` / `no_location` / `bad_location`。
    pub status: String,
    pub city: String,
    /// Open-Meteo 的 weather_code。
    pub code: Option<i64>,
    /// 中文描述：晴、多云、小雨……
    pub text: String,
    /// 给 LVGL 选图标用的键：sun / cloud / fog / drizzle / rain / snow /
    /// shower / thunder / hail / unknown。
    pub icon: String,
    pub temperature: Option<f64>,
    pub apparent: Option<f64>,
    pub humidity: Option<i64>,
    pub wind: Option<f64>,
    pub wind_unit: String,
    pub high: Option<f64>,
    pub low: Option<f64>,
    /// 今日降水概率上限（%）。
    pub precipitation: Option<i64>,
    /// 今日紫外线指数上限（uv_index_max）。
    pub uv: Option<f64>,
    /// 今日日出（HH:MM）。
    pub sunrise: String,
    /// 今日日落（HH:MM）。
    pub sunset: String,
    /// 数据观测时间（HH:MM），小屏直接显示。
    pub updated: String,
    /// 同一时刻的完整写法，网页上更清楚。
    pub updated_full: String,
    /// 最近一次成功取到数据的 Unix 秒；0 表示还没成功过。
    pub fetched_at: i64,
    /// 给用户看的一句话（状态说明或错误原因）。
    pub message: String,
}

impl Default for WeatherSnapshot {
    fn default() -> Self {
        Self {
            status: "loading".to_string(),
            city: String::new(),
            code: None,
            text: String::new(),
            icon: String::new(),
            temperature: None,
            apparent: None,
            humidity: None,
            wind: None,
            wind_unit: "m/s".to_string(),
            high: None,
            low: None,
            precipitation: None,
            uv: None,
            sunrise: String::new(),
            sunset: String::new(),
            updated: String::new(),
            updated_full: String::new(),
            fetched_at: 0,
            message: String::new(),
        }
    }
}

impl WeatherSnapshot {
    /// 手上是否有可显示的数据（网络失败时就是要靠这个决定还能不能显示）。
    pub fn has_data(&self) -> bool {
        self.temperature.is_some()
    }

    /// 数据是否可用（含「刷新中」与「沿用上一次」）。错误状态才是 false。
    pub fn is_ok(&self) -> bool {
        matches!(self.status.as_str(), "ok" | "loading" | "stale")
    }
}

/// 后台任务的收件箱。
enum WeatherCommand {
    /// 只把当前快照推给 GUI（刚连上时用），完全不碰网络。
    Publish,
    /// 立刻刷新一次。`announce` 为真时先推一条 `loading`，界面于是转小圈而
    /// 不清空旧数据。
    Refresh {
        announce: bool,
        reply: Option<oneshot::Sender<WeatherSnapshot>>,
    },
}

type Shared = Arc<Mutex<WeatherSnapshot>>;

/// 天气服务句柄。克隆它只是多一份引用，任务只有一个。
#[derive(Clone)]
pub struct WeatherService {
    state: Shared,
    tx: mpsc::Sender<WeatherCommand>,
}

impl WeatherService {
    /// 启动后台任务：先读缓存把界面填上，再异步刷新，之后每 10 分钟一次。
    pub fn start(config: WeatherConfig, gui: Arc<GuiBridge>) -> Self {
        let (tx, rx) = mpsc::channel(8);
        // 一开始就是「有位置但还没数据」或「没配位置」的明确状态：在首次刷新
        // 落地之前，首个 /api/weather 也能给出说得清的答案。
        let state: Shared = Arc::new(Mutex::new(placeholder(&config)));
        tokio::spawn(run(config, gui, state.clone(), rx));
        Self { state, tx }
    }

    pub fn snapshot(&self) -> WeatherSnapshot {
        shared(&self.state).clone()
    }

    /// `/api/weather` 的响应体。
    ///
    /// 永远是 JSON：拿不到数据时也返回结构化状态与明确文案，而不是把异常丢给
    /// 浏览器（页面据此显示「天气暂不可用」，其余功能不受影响）。
    pub fn api(&self) -> Value {
        let snapshot = self.snapshot();
        let ok = snapshot.is_ok();
        json!({
            "ok": ok,
            "error": if ok { String::new() } else { snapshot.message.clone() },
            "weather": snapshot,
        })
    }

    /// 把当前快照推给 GUI，不等网络（GUI 启动/重连时用）。
    pub fn publish(&self) {
        let _ = self.tx.try_send(WeatherCommand::Publish);
    }

    /// 立刻刷新一次，不等结果（GUI 点卡片时用）。
    pub fn refresh_soon(&self) {
        let _ = self.tx.try_send(WeatherCommand::Refresh {
            announce: true,
            reply: None,
        });
    }

    /// 刷新并等这次结果（网页「刷新天气」按钮）。等超时就回当前快照，
    /// 不让浏览器一直挂着。
    pub async fn refresh(&self) -> WeatherSnapshot {
        let (reply, receiver) = oneshot::channel();
        if self
            .tx
            .send(WeatherCommand::Refresh {
                announce: true,
                reply: Some(reply),
            })
            .await
            .is_err()
        {
            return self.snapshot();
        }
        match tokio::time::timeout(Duration::from_secs(MAX_TIMEOUT_SECS + 2), receiver).await {
            Ok(Ok(snapshot)) => snapshot,
            _ => self.snapshot(),
        }
    }
}

/// 拿锁：后台任务与 HTTP 协程都会用到这份状态，中毒了也接着用（状态只是数据，
/// 不会因为一次 panic 变成半截结构）。
fn shared(state: &Shared) -> MutexGuard<'_, WeatherSnapshot> {
    state.lock().unwrap_or_else(|error| error.into_inner())
}

async fn run(
    config: WeatherConfig,
    gui: Arc<GuiBridge>,
    state: Shared,
    mut rx: mpsc::Receiver<WeatherCommand>,
) {
    // 先把缓存填上：网络再慢也不会白屏，读到什么先显示什么。
    match load_cache(&config) {
        Some(snapshot) => {
            log::info!(
                "天气缓存已载入：{} {}° {}（{}）",
                snapshot.city,
                display_number(snapshot.temperature),
                snapshot.text,
                snapshot.updated
            );
            *shared(&state) = snapshot;
        }
        None => log::info!("天气暂无缓存，等待首次刷新"),
    }
    push(&gui, &state).await;

    // 首次刷新不播 loading：此刻界面显示的是缓存，转圈反而像出了故障。
    let mut failures = 0_u32;
    refresh_once(&config, &state, &gui, false, &mut failures, None).await;

    loop {
        let delay = if failures == 0 {
            config.refresh_delay()
        } else {
            backoff(failures, config.refresh_delay())
        };
        let command = tokio::select! {
            _ = tokio::time::sleep(delay) => None,
            received = rx.recv() => received,
        };
        match command {
            // 定时到点（或收件箱关了）：照常刷新
            None => refresh_once(&config, &state, &gui, false, &mut failures, None).await,
            Some(WeatherCommand::Publish) => push(&gui, &state).await,
            Some(WeatherCommand::Refresh { announce, reply }) => {
                refresh_once(&config, &state, &gui, announce, &mut failures, reply).await;
            }
        }
    }
}

/// 刷新一次。`failures` 由调用方持有：失败时递增，成功后归零，用来决定下一次
/// 的重试间隔。
async fn refresh_once(
    config: &WeatherConfig,
    state: &Shared,
    gui: &GuiBridge,
    announce: bool,
    failures: &mut u32,
    reply: Option<oneshot::Sender<WeatherSnapshot>>,
) {
    let city = display_city(config);

    // 配置层面的问题（没配位置 / 经纬度越界 / 服务关掉）：不碰网络，
    // 也就不会形成重试风暴。
    let (latitude, longitude) = match location(config) {
        Ok(pair) => pair,
        Err((status, message)) => {
            apply_unavailable(state, &city, status, &message);
            push(gui, state).await;
            if let Some(reply) = reply {
                let _ = reply.send(shared(state).clone());
            }
            return;
        }
    };

    if announce {
        // 只改状态，不清空旧数据：界面照旧显示上一次的温度，只是在图标位置
        // 转个小圈（需求：刷新期间不清空旧数据）。
        shared(state).status = "loading".to_string();
        push(gui, state).await;
    }

    match fetch(config, latitude, longitude, &city).await {
        Ok(snapshot) => {
            *failures = 0;
            log::info!(
                "天气已更新：{} {}° {}（体感 {}°，湿度 {}%，风 {}m/s，最高/最低 {}/{}）",
                snapshot.city,
                display_number(snapshot.temperature),
                snapshot.text,
                display_number(snapshot.apparent),
                snapshot.humidity.unwrap_or(0),
                display_number(snapshot.wind),
                display_number(snapshot.high),
                display_number(snapshot.low)
            );
            *shared(state) = snapshot.clone();
            save_cache(&snapshot).await;
        }
        Err(message) => {
            *failures += 1;
            log::warn!("天气刷新失败（连续第 {} 次）：{}", failures, message);
            apply_failure(state, &city, &message);
        }
    }

    push(gui, state).await;
    if let Some(reply) = reply {
        let _ = reply.send(shared(state).clone());
    }
}

/// 网络失败：有上一次的数据就继续显示（标记 `stale`），没有才把错误摆出来。
fn apply_failure(state: &Shared, city: &str, message: &str) {
    let mut snapshot = shared(state);
    if snapshot.has_data() {
        snapshot.status = "stale".to_string();
        snapshot.message = "网络异常，显示上次数据".to_string();
    } else {
        snapshot.status = "error".to_string();
        // 需求：没有缓存时显示「天气暂不可用」。
        snapshot.message = "天气暂不可用".to_string();
        snapshot.city = city.to_string();
        log::warn!("天气不可用（无缓存）：{}", message);
    }
}

/// 拿不到位置时的状态（未设置位置 / 位置配置错误 / 服务未启用）。
fn apply_unavailable(state: &Shared, city: &str, status: &str, message: &str) {
    let mut snapshot = shared(state);
    snapshot.city = city.to_string();
    snapshot.message = message.to_string();
    if snapshot.has_data() {
        // 位置是刚改坏的：旧数据仍然有用，说清原因即可。
        snapshot.status = "stale".to_string();
    } else {
        snapshot.status = status.to_string();
    }
}

/// 还没有任何数据时对外展示的内容：把「为什么没有」说清楚。
fn placeholder(config: &WeatherConfig) -> WeatherSnapshot {
    let mut snapshot = WeatherSnapshot {
        city: display_city(config),
        ..WeatherSnapshot::default()
    };
    match location(config) {
        Ok(_) => {
            snapshot.status = "loading".to_string();
            snapshot.message = "正在获取天气…".to_string();
        }
        Err((status, message)) => {
            snapshot.status = status.to_string();
            snapshot.message = message;
        }
    }
    snapshot
}

/// 位置是否可用。Err 里是 `(状态, 给用户看的一句话)`。
fn location(config: &WeatherConfig) -> Result<(f64, f64), (&'static str, String)> {
    if !config.enabled {
        return Err(("error", "天气服务未启用".to_string()));
    }
    let (Some(latitude), Some(longitude)) = (config.latitude, config.longitude) else {
        return Err(("no_location", "未设置位置".to_string()));
    };
    if !latitude.is_finite()
        || !longitude.is_finite()
        || latitude < LAT_RANGE.0
        || latitude > LAT_RANGE.1
        || longitude < LON_RANGE.0
        || longitude > LON_RANGE.1
    {
        return Err(("bad_location", "位置配置错误".to_string()));
    }
    Ok((latitude, longitude))
}

/// 界面上的城市名。没配城市就照需求显示「未设置位置」。
fn display_city(config: &WeatherConfig) -> String {
    let city = config.city.trim();
    if city.is_empty() {
        "未设置位置".to_string()
    } else {
        city.to_string()
    }
}

/// 天气请求走不走代理。
enum ProxyChoice {
    /// 直连。
    Direct,
    /// 沿用环境变量里的 `http_proxy` / `https_proxy`。
    Environment,
    /// 走指定代理地址。
    Url(String),
}

/// 解析代理设置。
///
/// 为什么不默认沿用环境变量：`http_proxy` 往往是给交互终端设的，服务起来时那个
/// 代理未必还在跑（本机就指向一个已经关掉的 127.0.0.1:10808），而 reqwest 默认
/// 会读它——结果是每次刷新都发给死代理、超时，卡片永远显示「天气暂不可用」。
/// 天气是一次普通的公网 HTTPS 请求，默认直连最稳；真需要代理就显式写出来。
fn proxy_choice(raw: Option<&str>) -> ProxyChoice {
    let Some(raw) = raw.map(str::trim).filter(|value| !value.is_empty()) else {
        return ProxyChoice::Direct;
    };
    match raw.to_ascii_lowercase().as_str() {
        "env" => ProxyChoice::Environment,
        "off" | "none" | "direct" | "0" => ProxyChoice::Direct,
        _ => ProxyChoice::Url(raw.to_string()),
    }
}

/// 取一次天气。任何失败都变成 `Err(文案)`，不 panic、不阻塞别的任务。
async fn fetch(
    config: &WeatherConfig,
    latitude: f64,
    longitude: f64,
    city: &str,
) -> Result<WeatherSnapshot, String> {
    let builder = reqwest::Client::builder().timeout(config.timeout());
    let builder = match proxy_choice(config.proxy.as_deref()) {
        ProxyChoice::Direct => builder.no_proxy(),
        ProxyChoice::Environment => builder,
        ProxyChoice::Url(url) => builder.proxy(
            reqwest::Proxy::all(&url).map_err(|error| format!("代理配置无效: {}", error))?,
        ),
    };
    let client = builder
        .build()
        .map_err(|error| format!("HTTP 客户端初始化失败: {}", error))?;

    let response = client
        .get(FORECAST_URL)
        .query(&[
            ("latitude", latitude.to_string()),
            ("longitude", longitude.to_string()),
            (
                "current",
                "temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
                    .to_string(),
            ),
            (
                "daily",
                "temperature_2m_max,temperature_2m_min,precipitation_probability_max,uv_index_max,sunrise,sunset"
                    .to_string(),
            ),
            ("timezone", "auto".to_string()),
            ("forecast_days", "1".to_string()),
            ("wind_speed_unit", "ms".to_string()),
        ])
        .send()
        .await
        .map_err(|error| format!("请求失败: {}", error))?;

    if !response.status().is_success() {
        return Err(format!("接口返回 {}", response.status().as_u16()));
    }

    // 解析失败只当一次刷新失败：接口改字段不该把核心拖下水。
    let value: Value = response
        .json()
        .await
        .map_err(|error| format!("返回内容解析失败: {}", error))?;
    snapshot_from_value(&value, city)
}

fn snapshot_from_value(value: &Value, city: &str) -> Result<WeatherSnapshot, String> {
    let temperature = number(value, "/current/temperature_2m")
        .ok_or_else(|| "返回内容缺少当前温度".to_string())?;
    // 整数字段在 JSON 里可能是 41 也可能是 41.0，统一按数值取再取整。
    let code = number(value, "/current/weather_code").map(|value| value.round() as i64);
    let (text, icon) = describe(code);
    let observed = value
        .pointer("/current/time")
        .and_then(Value::as_str)
        .unwrap_or("");
    let (updated, updated_full) = clock(observed);

    Ok(WeatherSnapshot {
        status: "ok".to_string(),
        city: city.to_string(),
        code,
        text: text.to_string(),
        icon: icon.to_string(),
        temperature: Some(temperature),
        apparent: number(value, "/current/apparent_temperature"),
        humidity: number(value, "/current/relative_humidity_2m").map(|value| value.round() as i64),
        wind: number(value, "/current/wind_speed_10m"),
        wind_unit: "m/s".to_string(),
        high: number(value, "/daily/temperature_2m_max/0"),
        low: number(value, "/daily/temperature_2m_min/0"),
        precipitation: number(value, "/daily/precipitation_probability_max/0")
            .map(|value| value.round() as i64),
        uv: number(value, "/daily/uv_index_max/0"),
        sunrise: daily_time(value, "/daily/sunrise/0"),
        sunset: daily_time(value, "/daily/sunset/0"),
        updated,
        updated_full,
        fetched_at: now_seconds(),
        message: String::new(),
    })
}

fn number(value: &Value, pointer: &str) -> Option<f64> {
    value.pointer(pointer).and_then(Value::as_f64)
}

/// weather_code → (中文描述, 图标键)。映射表按需求给定，未列出的码算未知天气。
fn describe(code: Option<i64>) -> (&'static str, &'static str) {
    match code {
        Some(0) => ("晴", "sun"),
        Some(1) | Some(2) | Some(3) => ("多云", "cloud"),
        Some(45) | Some(48) => ("雾", "fog"),
        Some(51) | Some(53) | Some(55) => ("毛毛雨", "drizzle"),
        Some(61) | Some(63) | Some(65) => ("雨", "rain"),
        Some(71) | Some(73) | Some(75) => ("雪", "snow"),
        Some(80) | Some(81) | Some(82) => ("阵雨", "shower"),
        Some(95) => ("雷雨", "thunder"),
        Some(96) | Some(99) => ("雷雨伴冰雹", "hail"),
        _ => ("未知天气", "unknown"),
    }
}

/// Open-Meteo 的 `current.time`（`2026-10-05T16:30`）→（`16:30`, `2026-10-05 16:30`）。
fn clock(observed: &str) -> (String, String) {
    let trimmed = observed.trim();
    if trimmed.len() < 16 {
        return ("--:--".to_string(), trimmed.to_string());
    }
    let clock = trimmed[11..16].to_string();
    let full = format!("{} {}", &trimmed[..10], clock);
    (clock, full)
}

fn now_seconds() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs() as i64)
        .unwrap_or(0)
}

/// 读缓存。文件坏了、字段缺了都只当没有缓存，不影响启动。
fn load_cache(config: &WeatherConfig) -> Option<WeatherSnapshot> {
    let content = std::fs::read_to_string(CACHE_FILE).ok()?;
    let mut snapshot: WeatherSnapshot = serde_json::from_str(&content).ok()?;
    if !snapshot.has_data() {
        return None;
    }
    /* 缓存还得是「这个地点」的：换了城市之后，旧缓存若继续顶着用，界面上会
     * 一直显示上一个城市的天气（网络不通时尤其明显）。地点对不上就当没有
     * 缓存，等这一轮刷新拿到新数据。 */
    if snapshot.city != display_city(config) {
        log::info!("天气缓存来自别的地点（{}），忽略", snapshot.city);
        return None;
    }
    // 进内存的缓存只是「上一次的数据」：是不是还新鲜由本轮刷新结果说了算。
    snapshot.status = "stale".to_string();
    snapshot.message = String::new();
    Some(snapshot)
}

/// 落盘缓存（尽力而为：只读文件系统上失败也只是下次没有兜底数据）。
async fn save_cache(snapshot: &WeatherSnapshot) {
    let Ok(content) = serde_json::to_string(snapshot) else {
        return;
    };
    if let Err(error) = tokio::fs::write(CACHE_FILE, content).await {
        log::debug!("写入天气缓存失败: {}", error);
    }
}

/// 连续失败后把重试间隔逐次翻倍（15s → 30s → …），最多回到正常刷新间隔。
fn backoff(failures: u32, normal: Duration) -> Duration {
    let steps = failures.saturating_sub(1).min(6);
    let seconds = RETRY_BASE_SECS.saturating_mul(1_u64 << steps);
    Duration::from_secs(seconds).min(normal)
}

/// 把当前快照推给 GUI。失败只记 debug：界面下次请求或下一轮刷新还会再来。
async fn push(gui: &GuiBridge, state: &Shared) {
    let text = payload(&shared(state)).to_string();
    if let Err(error) = gui.send_message(&text).await {
        log::debug!("推送天气到 GUI 失败: {}", error);
    }
}

/// 对外报文：`{"type":"weather", …快照字段…}`。
///
/// 推给 GUI 的 UDP 报文与 `/api/weather` 里的 `weather` 字段都由它生成，
/// 所以两端显示的一定是同一份数据。
fn payload(snapshot: &WeatherSnapshot) -> Value {
    let mut value = serde_json::to_value(snapshot).unwrap_or_else(|_| json!({}));
    if let Value::Object(map) = &mut value {
        map.insert("type".to_string(), Value::String("weather".to_string()));
    }
    value
}

/// 日志用：没有数值时写成 `--`，别让日志出现 0° 的假数据。
fn display_number(value: Option<f64>) -> String {
    match value {
        Some(number) => format!("{:.1}", number),
        None => "--".to_string(),
    }
}

/// daily 数组里的时刻是 `2026-10-06T06:12`，取 HH:MM 给小屏直接显示。
fn daily_time(value: &Value, pointer: &str) -> String {
    value
        .pointer(pointer)
        .and_then(Value::as_str)
        .and_then(|raw| raw.get(11..16))
        .map(str::to_string)
        .unwrap_or_default()
}

/// 解析环境变量里的经纬度。
///
/// 写坏了（不是数字）就给个落在合法范围外的哨兵值：界面因此显示「位置配置
/// 错误」——这正是用户需要的反馈，而不是被当成「没设置位置」。
pub(crate) fn coordinate(raw: &str) -> f64 {
    raw.trim().parse::<f64>().unwrap_or(BAD_COORDINATE)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn weather_code_maps_to_chinese_and_icon() {
        assert_eq!(describe(Some(0)), ("晴", "sun"));
        assert_eq!(describe(Some(2)), ("多云", "cloud"));
        assert_eq!(describe(Some(48)), ("雾", "fog"));
        assert_eq!(describe(Some(53)), ("毛毛雨", "drizzle"));
        assert_eq!(describe(Some(65)), ("雨", "rain"));
        assert_eq!(describe(Some(73)), ("雪", "snow"));
        assert_eq!(describe(Some(81)), ("阵雨", "shower"));
        assert_eq!(describe(Some(95)), ("雷雨", "thunder"));
        assert_eq!(describe(Some(99)), ("雷雨伴冰雹", "hail"));
        assert_eq!(describe(Some(7)), ("未知天气", "unknown"));
        assert_eq!(describe(None), ("未知天气", "unknown"));
    }

    #[test]
    fn open_meteo_payload_becomes_snapshot() {
        let value: Value = serde_json::from_str(
            r#"{
                "current": {
                    "time": "2026-10-05T16:30",
                    "temperature_2m": 23.4,
                    "relative_humidity_2m": 41,
                    "apparent_temperature": 25.1,
                    "weather_code": 0,
                    "wind_speed_10m": 2.4
                },
                "daily": {
                    "temperature_2m_max": [26.3],
                    "temperature_2m_min": [17.2]
                }
            }"#,
        )
        .expect("测试数据是合法 JSON");

        let snapshot = snapshot_from_value(&value, "北京").expect("字段齐全");
        assert_eq!(snapshot.status, "ok");
        assert_eq!(snapshot.text, "晴");
        assert_eq!(snapshot.icon, "sun");
        assert_eq!(snapshot.temperature, Some(23.4));
        assert_eq!(snapshot.humidity, Some(41));
        assert_eq!(snapshot.high, Some(26.3));
        assert_eq!(snapshot.low, Some(17.2));
        assert_eq!(snapshot.updated, "16:30");
        assert_eq!(snapshot.updated_full, "2026-10-05 16:30");
        assert!(snapshot.has_data());
        assert!(snapshot.is_ok());
    }

    #[test]
    fn payload_carries_the_type_tag_used_by_gui_and_web() {
        let snapshot = WeatherSnapshot {
            status: "ok".to_string(),
            city: "北京".to_string(),
            temperature: Some(23.4),
            ..WeatherSnapshot::default()
        };
        let value = payload(&snapshot);
        assert_eq!(value.get("type").and_then(Value::as_str), Some("weather"));
        assert_eq!(value.get("city").and_then(Value::as_str), Some("北京"));
    }

    #[test]
    fn proxy_defaults_to_direct_and_can_be_overridden() {
        /* 没配置就是直连：环境里那些给终端用的代理不该把天气请求一起拖下水 */
        assert!(matches!(proxy_choice(None), ProxyChoice::Direct));
        assert!(matches!(proxy_choice(Some("   ")), ProxyChoice::Direct));
        assert!(matches!(proxy_choice(Some("off")), ProxyChoice::Direct));
        assert!(matches!(proxy_choice(Some("env")), ProxyChoice::Environment));
        match proxy_choice(Some("http://127.0.0.1:10808")) {
            ProxyChoice::Url(url) => assert_eq!(url, "http://127.0.0.1:10808"),
            _ => panic!("应识别为代理地址"),
        }
    }

    #[test]
    fn location_reports_missing_and_invalid_coordinates() {
        let missing = WeatherConfig::default();
        assert_eq!(location(&missing).unwrap_err().0, "no_location");

        let wrong = WeatherConfig {
            latitude: Some(coordinate("不是数字")),
            longitude: Some(116.4),
            ..WeatherConfig::default()
        };
        assert_eq!(location(&wrong).unwrap_err().0, "bad_location");

        let out_of_range = WeatherConfig {
            latitude: Some(200.0),
            longitude: Some(116.4),
            ..WeatherConfig::default()
        };
        assert_eq!(location(&out_of_range).unwrap_err().0, "bad_location");

        let disabled = WeatherConfig {
            enabled: false,
            latitude: Some(39.9),
            longitude: Some(116.4),
            ..WeatherConfig::default()
        };
        assert!(location(&disabled).is_err());
    }

    #[test]
    fn city_falls_back_to_unset_label() {
        let config = WeatherConfig::default();
        assert_eq!(display_city(&config), "未设置位置");
        let named = WeatherConfig {
            city: " 上海 ".to_string(),
            ..WeatherConfig::default()
        };
        assert_eq!(display_city(&named), "上海");
    }

    #[test]
    fn failures_stretch_the_retry_interval_but_never_past_the_normal_one() {
        let normal = Duration::from_secs(600);
        assert_eq!(backoff(1, normal), Duration::from_secs(15));
        assert_eq!(backoff(2, normal), Duration::from_secs(30));
        assert_eq!(backoff(3, normal), Duration::from_secs(60));
        assert_eq!(backoff(99, normal), normal);
    }

    #[test]
    fn timeout_is_capped_at_eight_seconds() {
        let config = WeatherConfig {
            timeout_secs: 120,
            ..WeatherConfig::default()
        };
        assert_eq!(config.timeout(), Duration::from_secs(MAX_TIMEOUT_SECS));
        let zero = WeatherConfig {
            timeout_secs: 0,
            ..WeatherConfig::default()
        };
        assert_eq!(zero.timeout(), Duration::from_secs(1));
    }

    #[test]
    fn broken_payload_is_an_error_not_a_panic() {
        let value: Value = serde_json::from_str(r#"{"current": {}}"#).expect("合法 JSON");
        assert!(snapshot_from_value(&value, "北京").is_err());
    }

    fn state_with(city: &str, temperature: Option<f64>) -> Shared {
        Arc::new(Mutex::new(WeatherSnapshot {
            status: "ok".to_string(),
            city: city.to_string(),
            temperature,
            text: "晴".to_string(),
            ..WeatherSnapshot::default()
        }))
    }

    #[test]
    fn network_failure_keeps_showing_the_previous_reading() {
        let state = state_with("北京", Some(24.1));
        apply_failure(&state, "北京", "请求失败: timed out");

        let snapshot = shared(&state);
        assert_eq!(snapshot.status, "stale");
        assert_eq!(snapshot.temperature, Some(24.1), "旧数据必须留着");
        assert!(snapshot.is_ok(), "沿用上次数据仍算可用");
        assert!(snapshot.message.contains("上次"));
    }

    #[test]
    fn network_failure_without_cache_says_temporarily_unavailable() {
        let state = state_with("", None);
        apply_failure(&state, "北京", "请求失败: dns error");

        let snapshot = shared(&state);
        assert_eq!(snapshot.status, "error");
        assert_eq!(snapshot.message, "天气暂不可用");
        assert!(!snapshot.has_data());
        assert!(!snapshot.is_ok());
    }

    #[test]
    fn bad_location_is_reported_but_stale_data_stays_readable() {
        let state = state_with("北京", Some(20.0));
        apply_unavailable(&state, "北京", "bad_location", "位置配置错误");

        let snapshot = shared(&state);
        assert_eq!(snapshot.status, "stale");
        assert_eq!(snapshot.message, "位置配置错误");
        assert!(snapshot.has_data());
    }

    #[test]
    fn snapshot_survives_a_cache_round_trip() {
        let snapshot = WeatherSnapshot {
            status: "ok".to_string(),
            city: "北京".to_string(),
            temperature: Some(23.4),
            wind: Some(1.51),
            ..WeatherSnapshot::default()
        };
        let text = serde_json::to_string(&snapshot).expect("快照可序列化");
        let back: WeatherSnapshot = serde_json::from_str(&text).expect("快照可反序列化");
        assert_eq!(back.city, "北京");
        assert_eq!(back.temperature, Some(23.4));
        assert_eq!(back.wind_unit, "m/s");
    }
}

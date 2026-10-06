//! 智能家居中枢：核心自己接 MQTT，把「设备控制」从本机扩到局域网设备。
//!
//! 两块来源合成同一份设备表：
//!   * **zigbee2mqtt**：订阅 `<base>/bridge/devices` 拿到设备树，订阅
//!     `<base>/+` 与 `<base>/+/availability` 拿状态与在线情况，控制往
//!     `<base>/<名字>/set` 发 `{"state":"ON"}`。Z2M 的用法本身很统一，
//!     所以 Zigbee 侧用户不用填任何东西。
//!   * **手写设备**（Wi-Fi 插座、自己刷的 ESP 等）：配置里声明状态主题与指令
//!     主题，payload 支持 `ON/OFF`、`1/0`、`true/false` 或 `{"state":"ON"}`。
//!
//! 对外只有三件事：`snapshot()` 给界面看、`command()` 下指令、`status_json()`
//! 报连接情况。GUI、网页控制台与云端（MCP 工具）都从这一份状态读写。

pub mod mqtt;
pub mod tools;

use crate::config::{SmartHomeConfig, SmartHomeDeviceConfig};
use anyhow::{anyhow, Result};
use mqtt::{Message, MqttClient};
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::sync::mpsc;

/// 断线重连间隔。局域网里 broker 掉线多半是路由器重启，几秒一次足够。
const RECONNECT_DELAY: Duration = Duration::from_secs(3);

/// 状态多久没更新就算"可能离线"。Z2M 设备有 availability 主题，手写设备多半
/// 没有，只能靠时间兜底。
const STALE_AFTER_SECS: u64 = 300;

/// 设备类型：开关，或灯（灯额外支持亮度）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceKind {
    Switch,
    Light,
}

impl DeviceKind {
    fn as_str(self) -> &'static str {
        match self {
            Self::Switch => "switch",
            Self::Light => "light",
        }
    }

    fn parse(value: &str) -> Self {
        match value.trim().to_ascii_lowercase().as_str() {
            "light" | "lamp" | "bulb" => Self::Light,
            _ => Self::Switch,
        }
    }
}

/// 设备从哪来：zigbee2mqtt，或配置里手写。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceSource {
    Zigbee,
    Manual,
}

impl DeviceSource {
    fn as_str(self) -> &'static str {
        match self {
            Self::Zigbee => "zigbee",
            Self::Manual => "manual",
        }
    }
}

/// 一台局域网设备在核心里的样子。
#[derive(Debug, Clone)]
struct LanDevice {
    id: String,
    name: String,
    kind: DeviceKind,
    source: DeviceSource,
    /// 收到过消息（Z2M 的 availability 或手写设备的状态上报）。
    reachable: bool,
    state: Option<bool>,
    brightness: Option<u8>,
    /// 指令主题：Z2M 是 `<base>/<名字>/set`，手写设备来自配置。
    command_topic: String,
    /// 手写设备的开/关 payload。
    command_on: String,
    command_off: String,
    model: Option<String>,
    updated_at: u64,
}

impl LanDevice {
    /// 在线 = 见到过消息，且（手写设备）状态还新鲜。Z2M 的在线由 availability
    /// 说了算，不能再拿时间兜底，否则一个两小时没动的传感器会被显示成离线。
    fn online(&self) -> bool {
        if !self.reachable {
            return false;
        }
        match self.source {
            DeviceSource::Zigbee => true,
            DeviceSource::Manual => !is_stale(self.updated_at),
        }
    }

    fn to_json(&self) -> Value {
        json!({
            "id": self.id,
            "name": self.name,
            "kind": self.kind.as_str(),
            "source": self.source.as_str(),
            "online": self.online(),
            "state": self.state,
            "brightness": self.brightness,
            "model": self.model,
            "updatedAt": self.updated_at,
        })
    }
}

/// 中枢的连接状态：界面据此告诉用户"为什么没有设备"。
#[derive(Debug, Clone, Default)]
struct HubStatus {
    connected: bool,
    last_error: Option<String>,
    subscriptions: usize,
}

/// 发给 MQTT 任务的动作。
enum HubCommand {
    Publish { topic: String, payload: String },
    /// 配置变了：断开重连，并按新配置重新订阅。
    Reconnect,
}

/// 中枢本体。`enabled=false` 或没配 broker 时也照样存在，只是不连网 ——
/// 界面与接口只需要读状态，不必到处判断 Option。
pub struct SmartHomeHub {
    devices: Arc<Mutex<BTreeMap<String, LanDevice>>>,
    status: Arc<Mutex<HubStatus>>,
    config: Arc<Mutex<SmartHomeConfig>>,
    tx: Option<mpsc::Sender<HubCommand>>,
}

impl SmartHomeHub {
    pub fn start(initial: SmartHomeConfig) -> Arc<Self> {
        let devices = Arc::new(Mutex::new(BTreeMap::new()));
        let status = Arc::new(Mutex::new(HubStatus::default()));

        // 手写设备不依赖网络：先放进表里，开机就能在控制页看到
        {
            let mut table = lock(&devices);
            for device in &initial.devices {
                table.insert(device.id.clone(), manual_device(device));
            }
        }

        let config = Arc::new(Mutex::new(initial.clone()));
        let connectable = initial.enabled && !initial.broker.trim().is_empty();
        if !connectable {
            let reason = if !initial.enabled {
                "智能家居中枢未启用"
            } else {
                "还没配置 MQTT broker"
            };
            log::info!("{}（设备控制页只显示本机）", reason);
            lock(&status).last_error = Some(reason.to_string());
            return Arc::new(Self {
                devices,
                status,
                config,
                tx: None,
            });
        }

        let (tx, rx) = mpsc::channel::<HubCommand>(16);
        tokio::spawn(run(config.clone(), devices.clone(), status.clone(), rx));

        Arc::new(Self {
            devices,
            status,
            config,
            tx: Some(tx),
        })
    }

    /// 设备表快照（含中枢自身状态）。界面、网页控制台与 MCP 工具都读这个。
    pub fn snapshot(&self) -> Value {
        /* 先把设备表拷出来并放锁：`status_json()` 自己也要读设备表，
         * 而 std::sync::Mutex 不是可重入的 —— 抱着锁再进去会把整个 HTTP
         * 服务一个请求一个请求地拖死（实测：几个请求之后就不再响应）。 */
        let devices: Vec<Value> = {
            let table = lock(&self.devices);
            table.values().map(LanDevice::to_json).collect()
        };
        json!({
            "devices": devices,
            "count": devices.len(),
            "hub": self.status_json(),
        })
    }

    /// 当前生效的配置（网页表单要在它基础上改，避免把没填的项清空）。
    pub fn config(&self) -> SmartHomeConfig {
        lock(&self.config).clone()
    }

    pub fn status_json(&self) -> Value {
        let config = lock(&self.config);
        let status = lock(&self.status);
        json!({
            "enabled": config.enabled,
            "broker": config.broker,
            "zigbee2mqttTopic": config.zigbee2mqtt_topic,
            "connected": status.connected,
            "subscriptions": status.subscriptions,
            "error": status.last_error,
            "username": config.username,
            // 密码不回显，只说明配了没有
            "hasPassword": !config.password.is_empty(),
            "devices": lock(&self.devices).len(),
        })
    }

    /// 下一条指令。`action` 取 `on` / `off` / `toggle` / `brightness`。
    ///
    /// 本地状态先按预期更新，不等 broker 回话：Z2M 随后会把真实状态发到状态
    /// 主题，届时以它为准。这样点一下开关界面立刻有反馈，而不用等网络往返。
    pub async fn command(&self, id: &str, action: &str, value: Option<i64>) -> Result<Value> {
        let (topic, payload, state, brightness) = {
            let mut table = lock(&self.devices);
            let device = table
                .get_mut(id)
                .ok_or_else(|| anyhow!("没有这台设备: {}", id))?;

            let wants_brightness = action == "brightness";
            let (next_state, next_brightness) = match action {
                "on" => (Some(true), device.brightness),
                "off" => (Some(false), device.brightness),
                "toggle" => (Some(!device.state.unwrap_or(false)), device.brightness),
                "brightness" => {
                    let percent = value.unwrap_or(100).clamp(0, 100) as u8;
                    (Some(percent > 0), Some(percent))
                }
                other => {
                    return Err(anyhow!(
                        "不支持的指令: {}（可用 on/off/toggle/brightness）",
                        other
                    ))
                }
            };

            let payload = match device.source {
                DeviceSource::Zigbee => zigbee_payload(device.kind, next_state, next_brightness),
                DeviceSource::Manual => {
                    manual_payload(device, next_state, next_brightness, wants_brightness)
                }
            };

            device.state = next_state;
            if next_brightness.is_some() {
                device.brightness = next_brightness;
            }
            device.updated_at = now();
            (device.command_topic.clone(), payload, next_state, next_brightness)
        };

        self.publish(&topic, &payload).await?;
        log::info!("智能家居指令 {} {} -> {} {}", id, action, topic, payload);
        Ok(json!({ "ok": true, "id": id, "state": state, "brightness": brightness }))
    }

    /// 重新向 broker 要一次 zigbee2mqtt 的设备树（新配对设备后不必重启设备）。
    pub async fn discover(&self) -> Result<()> {
        let topic = {
            let config = lock(&self.config);
            let base = config.zigbee2mqtt_topic.trim().trim_end_matches('/');
            if base.is_empty() {
                return Err(anyhow!("没有配置 zigbee2mqtt 的 base topic"));
            }
            format!("{}/bridge/request/devices", base)
        };
        self.publish(&topic, "").await
    }

    /// 改配置（网页控制台用）：立刻生效，并写回 xiaozhi_config.json。
    ///
    /// 写盘的意义：现场换路由器/换密码只要在网页里改一次，重启后仍然有效，
    /// 不必重新编译固件。
    pub fn update_config(&self, new_config: SmartHomeConfig) -> Result<Value> {
        {
            let mut config = lock(&self.config);
            *config = new_config.clone();
        }
        self.persist(&new_config)?;

        {
            let mut table = lock(&self.devices);
            /* 整表重建：换了 broker 或 Z2M 前缀之后，原来那批 Zigbee 设备属于
             * 另一个网关，留着只会让人对着不存在的设备点开关；它们会在重连后
             * 由新网关的设备树重新导入。手写设备则按新配置立刻重建。 */
            table.clear();
            for device in &new_config.devices {
                table.insert(device.id.clone(), manual_device(device));
            }
        }

        if let Some(tx) = &self.tx {
            if tx.try_send(HubCommand::Reconnect).is_err() {
                log::warn!("智能家居中枢正忙，重连请求未能入队，将在下次重连时生效");
            }
        } else if new_config.enabled && !new_config.broker.trim().is_empty() {
            log::info!("智能家居中枢已配置但需重启核心才能连上（启动时未启用）");
        }
        Ok(self.status_json())
    }

    fn persist(&self, new_config: &SmartHomeConfig) -> Result<()> {
        let mut config = crate::config::Config::load_or_create()?;
        config.smarthome = new_config.clone();
        config
            .save()
            .map_err(|error| anyhow!("写入 xiaozhi_config.json 失败: {}", error))
    }

    async fn publish(&self, topic: &str, payload: &str) -> Result<()> {
        let tx = self
            .tx
            .as_ref()
            .ok_or_else(|| anyhow!("智能家居中枢未连接（先在网页控制台里配置 MQTT broker）"))?;
        tx.send(HubCommand::Publish {
            topic: topic.to_string(),
            payload: payload.to_string(),
        })
        .await
        .map_err(|_| anyhow!("MQTT 任务已退出"))
    }
}

/// 主循环：连接 → 订阅 → 收发 → 掉线退避重连。整个进程只在这里碰 MQTT 的 TCP。
///
/// 配置是共享的（`Arc<Mutex<..>>`），所以网页刚改完 broker，下一轮重连就会用上，
/// 不需要把配置复制到任务里。
async fn run(
    config: Arc<Mutex<SmartHomeConfig>>,
    devices: Arc<Mutex<BTreeMap<String, LanDevice>>>,
    status: Arc<Mutex<HubStatus>>,
    mut rx: mpsc::Receiver<HubCommand>,
) {
    loop {
        let current = lock(&config).clone();
        match connect_and_serve(&current, &devices, &status, &mut rx).await {
            Ok(()) => log::info!("智能家居中枢：本次连接正常结束"),
            Err(error) => {
                log::warn!("智能家居中枢断开: {}", error);
                lock(&status).last_error = Some(format!("{}", error));
            }
        }
        lock(&status).connected = false;
        lock(&status).subscriptions = 0;
        // 网关断了，"在线"就不再是可知事实：Zigbee 设备的在线状态本来就来自
        // 网关的 availability 主题，没有网关时它只是一句过期的断言。
        {
            let mut table = lock(&devices);
            for device in table.values_mut() {
                if device.source == DeviceSource::Zigbee {
                    device.reachable = false;
                }
            }
        }

        // 退避期间也要听得见"改配置"这类命令，否则用户改完要等一整个周期
        let deadline = tokio::time::Instant::now() + RECONNECT_DELAY;
        loop {
            match tokio::time::timeout_at(deadline, rx.recv()).await {
                Ok(Some(HubCommand::Reconnect)) => break,
                Ok(Some(HubCommand::Publish { .. })) => continue, // 没连上，丢弃
                Ok(None) => return,                               // 通道关闭，收工
                Err(_) => break,                                  // 到点重连
            }
        }
    }
}

/// 一次完整会话：连接 → 订阅 → 事件循环（消息 / 指令 / 心跳）。
async fn connect_and_serve(
    config: &SmartHomeConfig,
    devices: &Arc<Mutex<BTreeMap<String, LanDevice>>>,
    status: &Arc<Mutex<HubStatus>>,
    rx: &mut mpsc::Receiver<HubCommand>,
) -> Result<()> {
    let client_id = format!("qzdesk-{}", std::process::id());
    let mut client = MqttClient::connect(
        &config.broker,
        &client_id,
        &config.username,
        &config.password,
        mqtt::DEFAULT_KEEP_ALIVE,
    )
    .await?;
    log::info!("智能家居中枢已连接 {}", config.broker);

    let mut subscriptions: BTreeSet<String> = BTreeSet::new();
    let base = config.zigbee2mqtt_topic.trim().trim_end_matches('/').to_string();
    if !base.is_empty() {
        for topic in [
            format!("{}/bridge/devices", base),
            format!("{}/+", base),
            format!("{}/+/availability", base),
        ] {
            if let Err(error) = client.subscribe(&topic).await {
                log::warn!("订阅 {} 失败: {}", topic, error);
            }
            subscriptions.insert(topic);
        }
    }
    for device in &config.devices {
        if device.state_topic.trim().is_empty() {
            continue;
        }
        if let Err(error) = client.subscribe(&device.state_topic).await {
            log::warn!("订阅 {} 失败: {}", device.state_topic, error);
        }
        subscriptions.insert(device.state_topic.clone());
    }

    {
        let mut state = lock(status);
        state.connected = true;
        state.last_error = None;
        state.subscriptions = subscriptions.len();
    }

    let interval = client.ping_interval();
    loop {
        tokio::select! {
            incoming = client.next_message(interval) => {
                match incoming? {
                    Some(message) => handle_message(&message, config, devices),
                    None => client.ping().await?,   // 静默到期：发心跳
                }
            }
            command = rx.recv() => {
                match command {
                    Some(HubCommand::Publish { topic, payload }) => {
                        client.publish(&topic, &payload, false).await?;
                    }
                    // 配置变了：交回外层重连（订阅主题要整套换掉）
                    Some(HubCommand::Reconnect) => return Ok(()),
                    None => { client.disconnect().await; return Ok(()); }
                }
            }
        }
    }
}

/// 一条 MQTT 消息 → 设备表。
fn handle_message(
    message: &Message,
    config: &SmartHomeConfig,
    devices: &Arc<Mutex<BTreeMap<String, LanDevice>>>,
) {
    let base = config.zigbee2mqtt_topic.trim().trim_end_matches('/');
    let topic = message.topic.as_str();
    let payload = message.text();

    if !base.is_empty() && topic == format!("{}/bridge/devices", base) {
        import_zigbee_devices(&payload, base, devices);
        return;
    }
    if !base.is_empty() {
        if let Some(name) = topic
            .strip_prefix(&format!("{}/", base))
            .and_then(|rest| rest.strip_suffix("/availability"))
        {
            update_availability(devices, name, &payload);
            return;
        }
        if let Some(name) = topic.strip_prefix(&format!("{}/", base)) {
            // `<base>/<名字>`：一层，且不是 bridge 自己的消息
            if !name.is_empty() && !name.contains('/') && name != "bridge" {
                update_state(devices, &format!("zigbee:{}", name), &payload);
                return;
            }
        }
    }
    for device in &config.devices {
        if device.state_topic == topic {
            update_state(devices, &device.id, &payload);
            return;
        }
    }
}

/// zigbee2mqtt 的设备树 → 设备表。只收可控的（switch/light），跳过协调器与传感器。
fn import_zigbee_devices(
    payload: &str,
    base: &str,
    devices: &Arc<Mutex<BTreeMap<String, LanDevice>>>,
) {
    let parsed: Value = match serde_json::from_str(payload) {
        Ok(value) => value,
        Err(error) => {
            log::warn!("zigbee2mqtt 设备树解析失败: {}", error);
            return;
        }
    };
    let list = match parsed.as_array() {
        Some(list) => list,
        None => return,
    };

    // 设备树是全量的：本轮没出现过的 Zigbee 设备说明已经从网关里删掉了，
    // 留在列表里只会让用户对着一个不存在的设备点开关。手写设备不受影响。
    let known: BTreeSet<String> = list
        .iter()
        .filter_map(|entry| entry.get("friendly_name").and_then(Value::as_str))
        .map(|name| format!("zigbee:{}", name))
        .collect();

    let mut imported = 0usize;
    let mut table = lock(devices);
    table.retain(|id, device| {
        device.source == DeviceSource::Manual || known.contains(id)
    });
    for entry in list {
        let name = match entry.get("friendly_name").and_then(Value::as_str) {
            Some(name) if !name.is_empty() => name.to_string(),
            _ => continue,
        };
        if entry.get("type").and_then(Value::as_str) == Some("Coordinator") {
            continue;
        }
        let definition = entry.get("definition");
        let exposes = definition
            .and_then(|definition| definition.get("exposes"))
            .cloned()
            .unwrap_or(Value::Null);
        let kind = match kind_from_exposes(&exposes) {
            Some(kind) => kind,
            None => continue, // 传感器等不可控，不进控制页
        };
        let model = definition
            .and_then(|definition| definition.get("model"))
            .and_then(Value::as_str)
            .map(str::to_string);

        let id = format!("zigbee:{}", name);
        let command_topic = format!("{}/{}/set", base, name);
        let device = table.entry(id.clone()).or_insert_with(|| LanDevice {
            id: id.clone(),
            name: name.clone(),
            kind,
            source: DeviceSource::Zigbee,
            reachable: false,
            state: None,
            brightness: None,
            command_topic: command_topic.clone(),
            command_on: "ON".to_string(),
            command_off: "OFF".to_string(),
            model: model.clone(),
            updated_at: now(),
        });
        device.kind = kind;
        device.model = model;
        device.command_topic = command_topic;
        imported += 1;
    }
    drop(table);
    log::info!("zigbee2mqtt 设备树导入 {} 台可控设备", imported);
}

/// 从 Z2M 的 exposes 判断设备类型：有 light 就是灯，有开关就是开关，
/// 只有传感器（温度、电量……）则返回 None —— 它们进控制页没有意义。
fn kind_from_exposes(exposes: &Value) -> Option<DeviceKind> {
    let list = exposes.as_array()?;
    let mut kind = None;
    let mut has_brightness = false;
    for expose in list {
        let mut candidates: Vec<&Value> = vec![expose];
        if let Some(features) = expose.get("features").and_then(Value::as_array) {
            candidates.extend(features.iter());
        }
        for candidate in candidates {
            match candidate.get("type").and_then(Value::as_str) {
                Some("light") => kind = Some(DeviceKind::Light),
                Some("switch") => {
                    if kind.is_none() {
                        kind = Some(DeviceKind::Switch);
                    }
                }
                Some("brightness") => has_brightness = true,
                Some("name") => {
                    if candidate.get("name").and_then(Value::as_str) == Some("brightness") {
                        has_brightness = true;
                    }
                }
                _ => {}
            }
        }
    }
    // 带亮度调节的开关其实就是灯（很多 Zigbee 调光模块只暴露 switch+brightness）
    if has_brightness && kind.is_some() {
        kind = Some(DeviceKind::Light);
    }
    kind
}

fn update_state(
    devices: &Arc<Mutex<BTreeMap<String, LanDevice>>>,
    id: &str,
    payload: &str,
) {
    let (state, brightness) = parse_state(payload);
    if state.is_none() && brightness.is_none() {
        return;
    }
    let mut table = lock(devices);
    if let Some(device) = table.get_mut(id) {
        if state.is_some() {
            device.state = state;
        }
        if brightness.is_some() {
            device.brightness = brightness;
        }
        device.reachable = true;
        device.updated_at = now();
        log::debug!("智能家居状态: {} -> {}", device.name, payload.trim());
    }
}

fn update_availability(
    devices: &Arc<Mutex<BTreeMap<String, LanDevice>>>,
    name: &str,
    payload: &str,
) {
    let online = match payload.trim().trim_matches('"').to_ascii_lowercase().as_str() {
        "online" | "true" | "1" => true,
        "offline" | "false" | "0" => false,
        _ => return,
    };
    let mut table = lock(devices);
    if let Some(device) = table.get_mut(&format!("zigbee:{}", name)) {
        device.reachable = online;
        device.updated_at = now();
    }
}

/// 状态 payload：JSON（Z2M / Home Assistant）和裸值都接受。
fn parse_state(payload: &str) -> (Option<bool>, Option<u8>) {
    let trimmed = payload.trim();
    if trimmed.is_empty() {
        return (None, None);
    }
    // 只有对象才当 JSON 解：裸值里的 "0" / "1" 本身就是合法 JSON 数字，
    // 走 JSON 分支会得到一个没有 state 键的数字，状态就被丢掉了。
    if trimmed.starts_with('{') {
        let value: Value = match serde_json::from_str(trimmed) {
            Ok(value) => value,
            // 看起来像 JSON 却坏了：别猜，交给裸值解析兜底
            Err(_) => return (parse_onoff(trimmed), None),
        };
        let state = value
            .get("state")
            .or_else(|| value.get("power"))
            .or_else(|| value.get("switch"))
            .and_then(|raw| match raw {
                Value::Bool(flag) => Some(*flag),
                Value::String(text) => parse_onoff(text),
                _ => None,
            });
        let brightness = value.get("brightness").and_then(Value::as_u64).map(|raw| {
            // Z2M 的亮度是 0-254；本来就按百分比发的（<=100）就原样用
            if raw > 100 {
                ((raw * 100 + 127) / 254) as u8
            } else {
                raw as u8
            }
        });
        return (state, brightness);
    }
    (parse_onoff(trimmed), None)
}

fn parse_onoff(text: &str) -> Option<bool> {
    match text.trim().trim_matches('"').to_ascii_lowercase().as_str() {
        "on" | "true" | "1" | "open" => Some(true),
        "off" | "false" | "0" | "close" | "closed" => Some(false),
        _ => None,
    }
}

/// Zigbee（zigbee2mqtt）设备的指令 payload。
fn zigbee_payload(kind: DeviceKind, state: Option<bool>, brightness: Option<u8>) -> String {
    let on = state.unwrap_or(false);
    if kind == DeviceKind::Light {
        if let Some(percent) = brightness {
            let raw = ((percent as f64) * 254.0 / 100.0).round() as u8;
            // 关灯时不带亮度：某些固件拿到 brightness 会顺手把灯点亮
            if on {
                return json!({ "state": "ON", "brightness": raw }).to_string();
            }
            return json!({ "state": "OFF" }).to_string();
        }
    }
    json!({ "state": if on { "ON" } else { "OFF" } }).to_string()
}

/// 手写设备的指令 payload：开关用配置里的字符串，调亮度用 JSON。
fn manual_payload(
    device: &LanDevice,
    state: Option<bool>,
    brightness: Option<u8>,
    wants_brightness: bool,
) -> String {
    if wants_brightness {
        if let Some(percent) = brightness {
            return json!({
                "state": if percent > 0 { "ON" } else { "OFF" },
                "brightness": percent,
            })
            .to_string();
        }
    }
    match state {
        Some(true) => device.command_on.clone(),
        Some(false) => device.command_off.clone(),
        None => String::new(),
    }
}

/// 手写设备进表时的初始值：还没收到状态，先标成未知 + 离线。
fn manual_device(config: &SmartHomeDeviceConfig) -> LanDevice {
    LanDevice {
        id: config.id.clone(),
        name: config.name.clone(),
        kind: DeviceKind::parse(&config.kind),
        source: DeviceSource::Manual,
        reachable: false,
        state: None,
        brightness: None,
        command_topic: config.command_topic.clone(),
        command_on: config.command_on.clone(),
        command_off: config.command_off.clone(),
        model: None,
        updated_at: now(),
    }
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs())
        .unwrap_or(0)
}

fn is_stale(updated_at: u64) -> bool {
    now().saturating_sub(updated_at) > STALE_AFTER_SECS
}

/// 加锁但中毒也继续用：设备表不该因为某个线程 panic 就整体失效。
fn lock<T>(cell: &Mutex<T>) -> MutexGuard<'_, T> {
    cell.lock().unwrap_or_else(|error| error.into_inner())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn state_payload_shapes() {
        assert_eq!(parse_state(r#"{"state":"ON"}"#), (Some(true), None));
        assert_eq!(parse_state(r#"{"state":"OFF"}"#), (Some(false), None));
        assert_eq!(parse_state(r#"{"state":true}"#), (Some(true), None));
        assert_eq!(parse_state("ON"), (Some(true), None));
        assert_eq!(parse_state(" 0 "), (Some(false), None));
        // Z2M 的 254 亮度要折成百分比
        assert_eq!(parse_state(r#"{"state":"ON","brightness":254}"#), (Some(true), Some(100)));
        assert_eq!(parse_state(r#"{"state":"ON","brightness":127}"#), (Some(true), Some(50)));
        // 传感器上报（只有温度）不产生状态
        assert_eq!(parse_state(r#"{"temperature":21.5}"#), (None, None));
        assert_eq!(parse_state(""), (None, None));
    }

    #[test]
    fn exposes_decide_device_kind() {
        let switch_only = json!([{ "type": "switch", "features": [] }]);
        assert_eq!(kind_from_exposes(&switch_only), Some(DeviceKind::Switch));

        let light = json!([{ "type": "light", "features": [{ "type": "brightness" }] }]);
        assert_eq!(kind_from_exposes(&light), Some(DeviceKind::Light));

        // 只暴露 switch + brightness 的调光模块，按灯处理
        let dimmer = json!([{ "type": "switch" }, { "type": "brightness" }]);
        assert_eq!(kind_from_exposes(&dimmer), Some(DeviceKind::Light));

        let sensor = json!([{ "type": "numeric", "name": "temperature" }]);
        assert_eq!(kind_from_exposes(&sensor), None);
    }

    #[test]
    fn zigbee_payload_keeps_off_clean() {
        assert_eq!(zigbee_payload(DeviceKind::Switch, Some(true), None), r#"{"state":"ON"}"#);
        assert_eq!(zigbee_payload(DeviceKind::Switch, Some(false), None), r#"{"state":"OFF"}"#);
        assert_eq!(
            zigbee_payload(DeviceKind::Light, Some(true), Some(50)),
            r#"{"brightness":127,"state":"ON"}"#
        );
        // 关灯不带亮度，免得某些固件顺手把灯点亮
        assert_eq!(zigbee_payload(DeviceKind::Light, Some(false), Some(50)), r#"{"state":"OFF"}"#);
    }

    #[test]
    fn manual_device_uses_configured_payloads() {
        let config = SmartHomeConfig {
            enabled: true,
            broker: "mqtt://127.0.0.1:1883".to_string(),
            username: String::new(),
            password: String::new(),
            zigbee2mqtt_topic: String::new(),
            devices: vec![SmartHomeDeviceConfig {
                id: "desk".to_string(),
                name: "书桌灯".to_string(),
                kind: "light".to_string(),
                state_topic: "home/desk/state".to_string(),
                command_topic: "home/desk/set".to_string(),
                command_on: "1".to_string(),
                command_off: "0".to_string(),
            }],
        };
        let device = manual_device(&config.devices[0]);
        assert_eq!(device.kind, DeviceKind::Light);
        assert_eq!(manual_payload(&device, Some(true), None, false), "1");
        assert_eq!(manual_payload(&device, Some(false), None, false), "0");
        assert_eq!(
            manual_payload(&device, Some(true), Some(30), true),
            r#"{"brightness":30,"state":"ON"}"#
        );
    }
}

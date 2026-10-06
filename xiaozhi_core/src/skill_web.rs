//! Small dependency-free web UI for managing user-installed Skills.

use crate::mcp_gateway::skill::{SkillManager, SkillRole};
use crate::chat::ChatHub;
use crate::net_link::NetCommand;
use crate::timers::{PomodoroOptions, TimerStore};
use serde::Deserialize;
use serde_json::json;
use std::sync::Arc;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};

const PAGE: &str = include_str!("../web/skills.html");
const MAX_REQUEST: usize = 64 * 1024 * 1024 + 64 * 1024;

#[derive(Deserialize)]
struct InstallRequest {
    name: String,
    #[serde(default)]
    content: String,
}

#[derive(Deserialize)]
struct SelectionRequest {
    /// 兼容旧版界面：现在以 role 为准，role = none 即视为关闭
    #[serde(default = "default_enabled")]
    #[allow(dead_code)]
    enabled: bool,
    role: String,
}

fn default_enabled() -> bool {
    true
}

#[derive(Deserialize)]
struct ChatRequest { text: String }

/// 新建提醒。字段与 `set_timer.py` 的入参一致，语音与界面写的是同一套。
#[derive(Deserialize)]
struct ReminderRequest {
    hour: u32,
    minute: u32,
    #[serde(default)]
    label: String,
    #[serde(default = "default_enabled")]
    daily: bool,
    #[serde(default)]
    date: String,
}

/// 番茄钟操作。`action` 缺省当查询，界面刷新状态时可以直接发空对象。
#[derive(Deserialize)]
struct PomodoroRequest {
    #[serde(default = "default_pomodoro_action")]
    action: String,
    #[serde(default)]
    focus_minutes: Option<u32>,
    #[serde(default)]
    break_minutes: Option<u32>,
    #[serde(default)]
    cycles: Option<u32>,
}

fn default_pomodoro_action() -> String {
    "status".to_string()
}

/// 控制一台局域网设备。设备 id 放在 body 里而不是路径里：Zigbee 设备的 id 来自
/// friendly_name，可能是中文，走路径就得处理百分号编码，没必要。
#[derive(Deserialize)]
struct DeviceCommandRequest {
    id: String,
    action: String,
    #[serde(default)]
    value: Option<i64>,
}

/// 网页控制台改中枢配置。密码与 Z2M 主题留空表示"这一项不动"，
/// 免得一个没填密码的表单把已存的密码抹掉。
#[derive(Deserialize)]
struct SmarthomeRequest {
    enabled: bool,
    #[serde(default)]
    broker: String,
    #[serde(default)]
    username: String,
    #[serde(default)]
    password: Option<String>,
    #[serde(default)]
    zigbee2mqtt_topic: Option<String>,
}

pub async fn run(
    manager: Arc<SkillManager>,
    net_tx: tokio::sync::mpsc::Sender<NetCommand>,
    chat_hub: ChatHub,
    core_tx: tokio::sync::mpsc::Sender<crate::controller::CoreCommand>,
    smarthome: Arc<crate::smarthome::SmartHomeHub>,
    weather: crate::weather::WeatherService,
    performance: crate::performance::PerformanceService,
    timers: TimerStore,
) -> anyhow::Result<()> {
    let port = std::env::var("QZDESK_SKILL_WEB_PORT")
        .or_else(|_| std::env::var("XIAOZHI_SKILL_WEB_PORT"))
        .ok()
        .and_then(|value| value.parse::<u16>().ok())
        .unwrap_or(8080);
    // A previous OTA/Skill service may be in the middle of shutting down when
    // QZdesk starts. Retry briefly so a transient EADDRINUSE does not disable
    // the manager for the lifetime of the process.
    let listener = {
        let mut last_error = None;
        let mut listener = None;
        for _ in 0..40 {
            match TcpListener::bind(("0.0.0.0", port)).await {
                Ok(value) => {
                    listener = Some(value);
                    break;
                }
                Err(error) => {
                    last_error = Some(error);
                    tokio::time::sleep(Duration::from_millis(250)).await;
                }
            }
        }
        match listener {
            Some(value) => value,
            None => return Err(last_error.expect("listener bind failed" ).into()),
        }
    };
    log::info!("Skill web manager listening on http://0.0.0.0:{}", port);
    loop {
        let (stream, _) = listener.accept().await?;
        let manager = manager.clone();
        let net_tx = net_tx.clone();
        let chat_hub = chat_hub.clone();
        let core_tx = core_tx.clone();
        let smarthome = smarthome.clone();
        let weather = weather.clone();
        let performance = performance.clone();
        let timers = timers.clone();
        tokio::spawn(async move {
            if let Err(error) = handle_connection(
                stream,
                manager,
                net_tx,
                chat_hub,
                core_tx,
                smarthome,
                weather,
                performance,
                timers,
            )
            .await
            {
                log::debug!("Skill web request failed: {}", error);
            }
        });
    }
}

/// 每个 HTTP 连接一个任务。参数都是各服务的句柄，拆成结构体反而更绕。
#[allow(clippy::too_many_arguments)]
async fn handle_connection(
    mut stream: TcpStream,
    manager: Arc<SkillManager>,
    net_tx: tokio::sync::mpsc::Sender<NetCommand>,
    chat_hub: ChatHub,
    core_tx: tokio::sync::mpsc::Sender<crate::controller::CoreCommand>,
    smarthome: Arc<crate::smarthome::SmartHomeHub>,
    weather: crate::weather::WeatherService,
    performance: crate::performance::PerformanceService,
    timers: TimerStore,
) -> anyhow::Result<()> {
    let mut buffer = vec![0_u8; 8192];
    let mut request = Vec::new();
    let header_end;
    loop {
        let n = stream.read(&mut buffer).await?;
        if n == 0 { return Ok(()); }
        request.extend_from_slice(&buffer[..n]);
        if request.len() > MAX_REQUEST { return respond(&mut stream, 413, "请求过大", "text/plain; charset=utf-8").await; }
        if let Some(position) = request.windows(4).position(|w| w == b"\r\n\r\n") {
            header_end = position + 4;
            break;
        }
    }
    /* header 取成自有字符串：它会被后面的路由（如上传的 Content-Type）用到，
     * 若仍是 &request 的切片，读 body 时就没法再可变借用 request。 */
    let header = String::from_utf8_lossy(&request[..header_end - 4]).to_string();
    let mut lines = header.lines();
    let first = lines.next().unwrap_or("");
    let mut first_parts = first.split_whitespace();
    let method = first_parts.next().unwrap_or("").to_string();
    let path = first_parts.next().unwrap_or("").to_string();
    let content_length = lines
        .find_map(|line| line.strip_prefix("Content-Length:").or_else(|| line.strip_prefix("content-length:")))
        .and_then(|value| value.trim().parse::<usize>().ok())
        .unwrap_or(0);
    let upload_name = header.lines().find_map(|line| {
        line.strip_prefix("X-Skill-Name:")
            .or_else(|| line.strip_prefix("x-skill-name:"))
            .map(str::trim)
            .filter(|value| !value.is_empty())
            .map(str::to_string)
    });
    while request.len() < header_end + content_length {
        let n = stream.read(&mut buffer).await?;
        if n == 0 { break; }
        request.extend_from_slice(&buffer[..n]);
        if request.len() > MAX_REQUEST { return respond(&mut stream, 413, "请求过大", "text/plain; charset=utf-8").await; }
    }
    let body = request.get(header_end..header_end.saturating_add(content_length)).unwrap_or(&[]);
    if method == "GET" && path == "/api/chat/stream" {
        stream.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\n\r\n").await?;
        let mut receiver = chat_hub.subscribe();
        loop {
            match receiver.recv().await {
                /* 记录在核心侧就已经定型（角色 + 文本，与推给 GUI 的是同一份），
                 * 这里原样转发即可：页面上的顺序与内容和设备屏幕完全一致。 */
                Ok(event) => {
                    if event.trim().is_empty() {
                        continue;
                    }
                    stream.write_all(format!("data: {}\n\n", event).as_bytes()).await?;
                }
                // 页面读得慢时广播会丢旧消息；丢掉的只是显示，不影响记录本身，
                // 刷新页面会用 /api/chat/history 补齐。
                Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => continue,
                Err(tokio::sync::broadcast::error::RecvError::Closed) => break,
            }
        }
        return Ok(());
    }
    let (status, content_type, response) = match (method.as_str(), path.as_str()) {
        ("GET", "/") | ("GET", "/index.html") => (200, "text/html; charset=utf-8", PAGE.to_string()),
        /* 新打开的页面先拿一次现有记录，再接到 SSE 上：页面上看到的对话与设备
         * 屏幕上的一致，不会因为「谁先打开」而各说各话。 */
        ("GET", "/api/chat/history") => (
            200,
            "application/json; charset=utf-8",
            json!({ "messages": chat_hub.history() }).to_string(),
        ),
        /* —— 天气：与设备首页同一份快照 ——
         * 永远返回 JSON：拿不到数据时也有结构化的 status/message，页面据此显示
         * 「天气暂不可用」，Skill 列表与聊天完全不受影响。 */
        ("GET", "/api/weather") => (
            200,
            "application/json; charset=utf-8",
            weather.api().to_string(),
        ),
        /* 「刷新天气」按钮：等这一次结果再回，拿不到就回现存状态（不会挂住页面）。 */
        ("POST", "/api/weather/refresh") => {
            let snapshot = weather.refresh().await;
            let ok = snapshot.is_ok();
            (
                200,
                "application/json; charset=utf-8",
                json!({
                    "ok": ok,
                    "error": if ok { String::new() } else { snapshot.message.clone() },
                    "weather": snapshot,
                })
                .to_string(),
            )
        }
        /* —— 性能监控：与设备页同一份数据 ——
         * 只读接口：不接受任何参数，也就不存在"通过它执行命令"的可能。 */
        ("GET", "/api/performance") => (
            200,
            "application/json; charset=utf-8",
            performance.api().to_string(),
        ),
        ("GET", "/api/performance/history") => (
            200,
            "application/json; charset=utf-8",
            performance.history_api().to_string(),
        ),
        /* —— 智能家居：局域网设备表与控制 —— */
        ("GET", "/api/devices") => (
            200,
            "application/json; charset=utf-8",
            smarthome.snapshot().to_string(),
        ),
        ("GET", "/api/smarthome") => (
            200,
            "application/json; charset=utf-8",
            smarthome.status_json().to_string(),
        ),
        ("POST", "/api/devices/discover") => match smarthome.discover().await {
            Ok(()) => (200, "application/json; charset=utf-8", json!({"ok": true}).to_string()),
            Err(error) => (
                400,
                "application/json; charset=utf-8",
                json!({"ok": false, "error": error.to_string()}).to_string(),
            ),
        },
        ("POST", "/api/devices/command") => {
            match serde_json::from_slice::<DeviceCommandRequest>(body) {
                Ok(input) => match smarthome.command(&input.id, &input.action, input.value).await {
                    Ok(result) => (200, "application/json; charset=utf-8", result.to_string()),
                    Err(error) => (
                        400,
                        "application/json; charset=utf-8",
                        json!({"ok": false, "error": error.to_string()}).to_string(),
                    ),
                },
                Err(error) => (
                    400,
                    "application/json; charset=utf-8",
                    json!({"ok": false, "error": format!("JSON 格式错误: {}", error)}).to_string(),
                ),
            }
        }
        ("PUT", "/api/smarthome") => match serde_json::from_slice::<SmarthomeRequest>(body) {
            Ok(input) => {
                let mut config = smarthome.config();
                config.enabled = input.enabled;
                config.broker = input.broker.trim().to_string();
                config.username = input.username.trim().to_string();
                if let Some(password) = input.password {
                    config.password = password;
                }
                if let Some(topic) = input.zigbee2mqtt_topic {
                    config.zigbee2mqtt_topic = topic.trim().to_string();
                }
                match smarthome.update_config(config) {
                    Ok(status) => (
                        200,
                        "application/json; charset=utf-8",
                        json!({"ok": true, "hub": status}).to_string(),
                    ),
                    Err(error) => (
                        400,
                        "application/json; charset=utf-8",
                        json!({"ok": false, "error": error.to_string()}).to_string(),
                    ),
                }
            }
            Err(error) => (
                400,
                "application/json; charset=utf-8",
                json!({"ok": false, "error": format!("JSON 格式错误: {}", error)}).to_string(),
            ),
        },
        /* 核心日志：内存里那份环形缓冲，`?limit=` 控制条数。
         * 排查「技能有没有下发」这类问题，看这里比翻 /var/log 快得多。 */
        _ if method == "GET" && path.starts_with("/api/logs") => {
            let limit = path
                .split_once('?')
                .and_then(|(_, query)| {
                    query
                        .split('&')
                        .find_map(|pair| pair.strip_prefix("limit="))
                })
                .and_then(|value| value.parse::<usize>().ok())
                .unwrap_or(200)
                .clamp(1, crate::logbuf::CAPACITY);
            (
                200,
                "application/json; charset=utf-8",
                json!({
                    "ok": true,
                    "buffered": crate::logbuf::len(),
                    "lines": crate::logbuf::tail(limit),
                })
                .to_string(),
            )
        }
        /* 提醒与番茄钟：核心是唯一权威，设备界面与网页读写这里，语音侧的同名
         * MCP 工具写的也是同一份（见 timers.rs 的模块说明）。 */
        ("GET", "/api/timers") => (
            200,
            "application/json; charset=utf-8",
            json!({
                "ok": true,
                "max": crate::timers::MAX_REMINDERS,
                "timers": timers.reminders(),
            })
            .to_string(),
        ),
        ("POST", "/api/timers") => match serde_json::from_slice::<ReminderRequest>(body) {
            Ok(input) => match timers.add_reminder(
                input.hour,
                input.minute,
                &input.label,
                input.daily,
                &input.date,
            ) {
                Ok(reminder) => (
                    200,
                    "application/json; charset=utf-8",
                    json!({ "ok": true, "reminder": reminder, "timers": timers.reminders() })
                        .to_string(),
                ),
                Err(error) => (
                    400,
                    "application/json; charset=utf-8",
                    json!({ "ok": false, "error": error }).to_string(),
                ),
            },
            Err(error) => (
                400,
                "application/json; charset=utf-8",
                json!({ "ok": false, "error": format!("JSON 格式错误: {}", error) }).to_string(),
            ),
        },
        _ if method == "DELETE" && path.starts_with("/api/timers/") => {
            match path["/api/timers/".len()..].trim().parse::<u64>() {
                Ok(id) => match timers.remove_reminder(id) {
                    Ok(()) => (
                        200,
                        "application/json; charset=utf-8",
                        json!({ "ok": true, "timers": timers.reminders() }).to_string(),
                    ),
                    Err(error) => (
                        404,
                        "application/json; charset=utf-8",
                        json!({ "ok": false, "error": error }).to_string(),
                    ),
                },
                Err(_) => (
                    400,
                    "application/json; charset=utf-8",
                    json!({ "ok": false, "error": "提醒 id 必须是数字" }).to_string(),
                ),
            }
        }
        ("GET", "/api/pomodoro") => {
            let state = timers.pomodoro();
            let message = crate::timers::pomodoro_text(&state);
            (
                200,
                "application/json; charset=utf-8",
                json!({ "ok": true, "state": state, "message": message }).to_string(),
            )
        }
        ("POST", "/api/pomodoro") => match serde_json::from_slice::<PomodoroRequest>(body) {
            Ok(input) => match timers.pomodoro_action(
                input.action.trim(),
                PomodoroOptions {
                    focus_minutes: input.focus_minutes,
                    break_minutes: input.break_minutes,
                    cycles: input.cycles,
                },
            ) {
                Ok(state) => {
                    let message = crate::timers::pomodoro_text(&state);
                    (
                        200,
                        "application/json; charset=utf-8",
                        json!({ "ok": true, "state": state, "message": message }).to_string(),
                    )
                }
                Err(error) => (
                    400,
                    "application/json; charset=utf-8",
                    json!({ "ok": false, "error": error }).to_string(),
                ),
            },
            Err(error) => (
                400,
                "application/json; charset=utf-8",
                json!({ "ok": false, "error": format!("JSON 格式错误: {}", error) }).to_string(),
            ),
        },
        ("GET", "/api/skills") => (200, "application/json; charset=utf-8", manager.list().to_string()),
        ("POST", "/api/skills") => match serde_json::from_slice::<InstallRequest>(body) {
            Ok(input) => match manager.install(&input.name, &input.content) {
                Ok(()) => {
                    // 内容变了：索引失效并重建会话，保证立刻生效
                    manager.invalidate_index();
                    let _ = net_tx.send(NetCommand::Reconnect).await;
                    (200, "application/json; charset=utf-8", json!({"ok": true}).to_string())
                }
                Err(error) => (400, "application/json; charset=utf-8", json!({"ok": false, "error": error}).to_string()),
            },
            Err(error) => (400, "application/json; charset=utf-8", json!({"ok": false, "error": format!("JSON 格式错误: {}", error)}).to_string()),
        },
        _ if method == "PUT" && path.starts_with("/api/skills/") && path.ends_with("/selection") => {
            let prefix = "/api/skills/";
            let name = &path[prefix.len()..path.len() - "/selection".len()];
            match serde_json::from_slice::<SelectionRequest>(body) {
                Ok(input) => match SkillRole::parse(input.role.trim()) {
                    Ok(role) => match manager.set_role(name, role) {
                        Ok(()) => {
                            // Reconnect so the new primary instructions are
                            // delivered in the next MCP initialize exchange.
                            let _ = net_tx.send(NetCommand::Reconnect).await;
                            (200, "application/json; charset=utf-8", json!({"ok": true}).to_string())
                        },
                        Err(error) => (400, "application/json; charset=utf-8", json!({"ok": false, "error": error}).to_string()),
                    },
                    Err(error) => (400, "application/json; charset=utf-8", json!({"ok": false, "error": error}).to_string()),
                },
                Err(error) => (400, "application/json; charset=utf-8", json!({"ok": false, "error": format!("JSON 格式错误: {}", error)}).to_string()),
            }
        }
        ("POST", "/api/skills/upload") => {
            /* Unicode-safe upload envelope: u32 BE name length, UTF-8 name,
             * followed by the ZIP bytes. Older clients using X-Skill-Name are
             * still accepted below. */
            let envelope = body.len() >= 4
                && header.lines().any(|line| {
                    line.eq_ignore_ascii_case("Content-Type: application/x-qzdesk-skill-zip")
                });
            let (name, zip) = if envelope {
                let name_len = u32::from_be_bytes([body[0], body[1], body[2], body[3]]) as usize;
                let name_end = 4usize.saturating_add(name_len);
                if name_end <= body.len() {
                    match std::str::from_utf8(&body[4..name_end]) {
                        Ok(value) if !value.trim().is_empty() =>
                            (Some(value.trim().to_string()), &body[name_end..]),
                        _ => (None, &body[..]),
                    }
                } else {
                    (None, &body[..])
                }
            } else {
                (upload_name, body)
            };
            match name {
            Some(name) => match manager.install_zip(&name, zip).await {
                Ok(()) => {
                    manager.invalidate_index();
                    let _ = net_tx.send(NetCommand::Reconnect).await;
                    (200, "application/json; charset=utf-8", json!({"ok": true}).to_string())
                }
                Err(error) => (400, "application/json; charset=utf-8", json!({"ok": false, "error": error}).to_string()),
            },
            None => (400, "application/json; charset=utf-8", json!({"ok": false, "error": "缺少 Skill 名称"}).to_string()),
            }
        },
        ("POST", "/api/chat/send") if manager.active_summary().0 == 0 => (
            409,
            "application/json; charset=utf-8",
            json!({"ok": false, "error": "当前没有已启用的主技能，请先在 Skill 列表中设置一个主技能"}).to_string(),
        ),
        ("POST", "/api/chat/send") => match serde_json::from_slice::<ChatRequest>(body) {
            Ok(input) if !input.text.trim().is_empty() => {
                /* 云端只接受音频输入，所以这条文字要先在本地合成成语音，再按麦克风
                 * 同样的格式发出去。合成与发送都在核心侧完成（CoreCommand），这里
                 * 只负责把请求转过去并立刻回执，避免浏览器等一次完整推理。 */
                let text = input.text.trim().to_string();
                match core_tx.send(crate::controller::CoreCommand::TextAsSpeech(text)).await {
                    Ok(()) => (
                        202,
                        "application/json; charset=utf-8",
                        json!({"ok": true, "queued": true}).to_string(),
                    ),
                    Err(error) => (
                        503,
                        "application/json; charset=utf-8",
                        json!({"ok": false, "error": format!("核心无法接收消息: {}", error)}).to_string(),
                    ),
                }
            }
            _ => (400, "application/json; charset=utf-8", json!({"ok": false, "error": "消息不能为空"}).to_string()),
        },
        _ if method == "GET" && path.starts_with("/api/skills/") && path.ends_with("/content") => {
            let name = &path["/api/skills/".len()..path.len() - "/content".len()];
            match manager.read_source(name) {
                Ok(content) => (
                    200,
                    "application/json; charset=utf-8",
                    json!({ "ok": true, "content": content }).to_string(),
                ),
                Err(error) => (
                    404,
                    "application/json; charset=utf-8",
                    json!({ "ok": false, "error": error }).to_string(),
                ),
            }
        }
        _ if method == "DELETE" && path.starts_with("/api/skills/") => {
            let name = &path["/api/skills/".len()..];
            match manager.remove(name) {
                Ok(()) => {
                    manager.invalidate_index();
                    let _ = net_tx.send(NetCommand::Reconnect).await;
                    (200, "application/json; charset=utf-8", json!({"ok": true}).to_string())
                }
                Err(error) => (400, "application/json; charset=utf-8", json!({"ok": false, "error": error}).to_string()),
            }
        }
        _ => (404, "text/plain; charset=utf-8", "Not Found".to_string()),
    };
    respond(&mut stream, status, &response, content_type).await
}

async fn respond(stream: &mut TcpStream, status: u16, body: &str, content_type: &str) -> anyhow::Result<()> {
    let reason = match status { 200 => "OK", 400 => "Bad Request", 404 => "Not Found", 413 => "Payload Too Large", 501 => "Not Implemented", _ => "Error" };
    let response = format!(
        "HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n{}",
        status, reason, content_type, body.as_bytes().len(), body
    );
    stream.write_all(response.as_bytes()).await?;
    Ok(())
}

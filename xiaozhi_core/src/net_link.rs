use crate::config::Config;
use futures_util::{SinkExt, StreamExt};
use mac_address::get_mac_address;
use serde::Serialize;
use serde_json::{json, Value};
use tokio::sync::mpsc;
use tokio_tungstenite::{connect_async, tungstenite::protocol::Message};
use url::Url;
use uuid::Uuid;
use std::sync::Arc;
use crate::mcp_gateway::McpServer;

#[derive(Debug)]
pub enum NetEvent {
    Text(String),
    Binary(Vec<u8>),
    Connected,
    Ready,
    SkillRequired,
    Disconnected,
}

#[derive(Debug)]
pub enum NetCommand {
    SendText(String),
    SendBinary(Vec<u8>),
    /// Recreate the session so a changed Skill selection is sent during MCP
    /// initialize on the next connection.
    Reconnect,
    /// 唤醒（开始说话）时调用：只有主技能确实变了才重建会话，避免每次唤醒都断连
    RefreshSkillsIfChanged,
    EnsureSkillsReady(tokio::sync::oneshot::Sender<Result<(), String>>),
}

// 音频参数结构体
fn confirms_primary_skill(request: &Value, response: &Value) -> bool {
    if request.get("method").and_then(Value::as_str) != Some("tools/call")
        || request.get("params").and_then(|p| p.get("name")).and_then(Value::as_str) != Some("skill_list")
        || response.get("error").is_some()
    {
        return false;
    }
    let Some(text) = response
        .get("result")
        .and_then(|result| result.get("content"))
        .and_then(Value::as_array)
        .and_then(|items| items.iter().find_map(|item| item.get("text").and_then(Value::as_str)))
    else { return false; };
    let Ok(value) = serde_json::from_str::<Value>(text) else { return false; };
    value.get("primary_instructions")
        .and_then(Value::as_str)
        .map(|instructions| !instructions.trim().is_empty())
        .unwrap_or(false)
}

#[derive(Serialize)]
struct AudioParams {
    format: String,
    sample_rate: u32,
    channels: u8,
    frame_duration: u32,
}

// Features 声明结构体，用于告知服务端设备支持的能力
#[derive(Serialize)]
struct Features {
    #[serde(skip_serializing_if = "Option::is_none")]
    mcp: Option<bool>,
}

// Hello Message，用于初始化连接
#[derive(Serialize)]
struct HelloMessage {
    #[serde(rename = "type")]
    msg_type: String,
    version: u8,
    transport: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    features: Option<Features>,
    audio_params: AudioParams,
}

pub struct NetLink {
    config: Config,
    tx: mpsc::Sender<NetEvent>,
    rx_cmd: mpsc::Receiver<NetCommand>,
    mcp_server: Arc<McpServer>,
    ready_waiters: Vec<tokio::sync::oneshot::Sender<Result<(), String>>>,
}

impl NetLink {
    pub fn new(
        config: Config,
        tx: mpsc::Sender<NetEvent>,
        rx_cmd: mpsc::Receiver<NetCommand>,
        mcp_server: Arc<McpServer>,
    ) -> Self {
        Self { config, tx, rx_cmd, mcp_server, ready_waiters: Vec::new() }
    }

    // 如果发生错误断开连接，5秒后重连
    pub async fn run(mut self) {
        // 重试机制，指数退避
        let mut retry_delay = 1;
        loop {
            if let Err(e) = self.connect_and_loop().await {
                log::error!("Connection error: {}. Retrying in {}s...", e, retry_delay);
                let _ = self.tx.send(NetEvent::Disconnected).await;
                tokio::time::sleep(tokio::time::Duration::from_secs(retry_delay)).await;
                retry_delay = std::cmp::min(retry_delay * 2, 60);
            } else if self.rx_cmd.is_closed() {
                break;
            } else {
                retry_delay = 1;
                let _ = self.tx.send(NetEvent::Disconnected).await;
            }
        }
    }

    // 进入连接和主循环，处理WebSocket消息和发送命令
    async fn connect_and_loop(&mut self) -> anyhow::Result<()> {
        // 如果设备ID是unknown-device，则尝试获取MAC地址作为设备ID
        let device_id = if self.config.device_id == "unknown-device" {
            match get_mac_address() {
                Ok(Some(mac)) => mac.to_string().to_lowercase(), // Ensure lowercase to match typical Linux behavior 注意大小写一致，以匹配典型的Linux行为
                _ => Uuid::new_v4().to_string(), // 如果无法获取MAC地址，则生成新的UUID
            }
        } else {
            self.config.device_id.clone() // 使用配置中的设备ID
        };

        // 根据配置构建WebSocket请求
        let url = Url::parse(self.config.ws_url.as_ref())?;
        let host = url.host_str().unwrap_or("api.tenclass.net");

        let request = tokio_tungstenite::tungstenite::http::Request::builder()
            .method("GET")
            .uri(self.config.ws_url.as_ref())
            .header("Host", host)
            .header("Connection", "Upgrade")
            .header("Upgrade", "websocket")
            .header("Sec-WebSocket-Version", "13")
            .header(
                "Sec-WebSocket-Key",
                tokio_tungstenite::tungstenite::handshake::client::generate_key(),
            )
            .header("Authorization", format!("Bearer {}", self.config.ws_token))
            .header("Device-Id", &device_id)
            .header("Client-Id", &self.config.client_id)
            .header("Protocol-Version", "1")
            .body(())?;

        log::info!("Connecting to {}...", self.config.ws_url);
        log::debug!("Headers: {:?}", request.headers()); // Debug headers
        let (ws_stream, _) = connect_async(request).await?;
        log::info!("Connected!");

        self.mcp_server.begin_session();

        let (mut write, mut read) = ws_stream.split();

        self.tx.send(NetEvent::Connected).await?;

        // 发送Hello消息进行初始化链接
        // 根据配置动态决定是否在 hello 中声明 MCP 能力
        let features = if self.config.mcp.enabled {
            Some(Features { mcp: Some(true) })
        } else {
            None
        };
        let hello_msg = HelloMessage {
            msg_type: "hello".to_string(),
            version: 1,
            transport: "websocket".to_string(),
            features,
            audio_params: AudioParams {
                format: self.config.hello_format.to_string(),
                sample_rate: self.config.hello_sample_rate,
                channels: self.config.hello_channels,
                frame_duration: self.config.hello_frame_duration,
            },
        };
        let hello_json = serde_json::to_string(&hello_msg)?;

        log::info!("Sending Hello: {}", hello_json);
        write.send(Message::Text(hello_json.into())).await?;

        let mut hello_seen = false;
        let mut ready_sent = false;
        let mut skill_fetched = self.mcp_server.has_instructions();
        let mut turn_waiting = false;
        let mut turn_failed = false;
        let mut skill_deadline = tokio::time::Instant::now();
        let mut ready_tick = tokio::time::interval(tokio::time::Duration::from_millis(50));
        loop {
            tokio::select! {
                _ = ready_tick.tick() => {
                    self.ready_waiters.retain(|waiter| !waiter.is_closed());
                    if turn_waiting && !skill_fetched && !turn_failed && tokio::time::Instant::now() >= skill_deadline {
                        turn_failed = true;
                        let _ = self.tx.send(NetEvent::SkillRequired).await;
                        log::warn!("Blocking turn after MCP Skill fetch timeout");
                    }
                    if hello_seen && self.mcp_server.skills_ready() {
                        if !ready_sent {
                            self.tx.send(NetEvent::Ready).await?;
                            ready_sent = true;
                        }
                        for waiter in self.ready_waiters.drain(..) { let _ = waiter.send(Ok(())); }
                    }
                }
                msg = read.next() => {
                    match msg {
                        Some(Ok(msg)) => {
                            match msg {
                                Message::Text(text) => {
                                    // 服务端通常使用 {type:mcp,payload:{...}}，但
                                    // 部分网关会把 payload 编成 JSON 字符串，或直接发
                                    // JSON-RPC。三种形态都接受，避免主 Skill 因封装差异失效。
                                    let parsed = serde_json::from_str::<Value>(&text).ok();
                                    if parsed.as_ref().and_then(|value| value.get("type")).and_then(Value::as_str) == Some("hello") {
                                        hello_seen = true;
                                    }
                                    let (payload, session_id, wrapped) = match parsed.as_ref() {
                                        Some(value) if value.get("type").and_then(Value::as_str) == Some("mcp") => {
                                            let payload = value.get("payload").and_then(|item| {
                                                item.as_str()
                                                    .and_then(|raw| serde_json::from_str::<Value>(raw).ok())
                                                    .or_else(|| Some(item.clone()))
                                            });
                                            (payload, value.get("session_id").and_then(Value::as_str).unwrap_or(""), true)
                                        }
                                        Some(value) if value.get("jsonrpc").and_then(Value::as_str) == Some("2.0") && value.get("method").is_some() => {
                                            (Some(value.clone()), "", false)
                                        }
                                        _ => (None, "", false),
                                    };
                                    let handled = if let Some(payload) = payload {
                                        let payload_str = payload.to_string();
                                        log::info!("MCP Request (wrapped={}): {}", wrapped, payload_str);
                                        if let Some(mcp_response) = self.mcp_server.handle_message(&payload_str).await {
                                            if mcp_response.is_empty() {
                                                true
                                            } else {
                                                let response_payload = serde_json::from_str::<Value>(&mcp_response).unwrap_or(Value::Null);
                                                let fetched = turn_waiting && !turn_failed && confirms_primary_skill(&payload, &response_payload);
                                                let response = if wrapped {
                                                    json!({"type": "mcp", "session_id": session_id, "payload": response_payload})
                                                } else {
                                                    response_payload
                                                };
                                                let response_text = serde_json::to_string(&response).unwrap();
                                                write.send(Message::Text(response_text.into())).await?;
                                                if fetched {
                                                    skill_fetched = true;
                                                    log::info!("MCP skill_list returned primary skill for current turn");
                                                }
                                                if payload.get("method").and_then(Value::as_str) == Some("initialize")
                                                    && self.mcp_server.has_instructions()
                                                {
                                                    skill_fetched = true;
                                                    log::info!("MCP initialize sent skill instructions; session reply gate ready");
                                                }
                                                true
                                            }
                                        } else {
                                            false
                                        }
                                    } else {
                                        false
                                    };

                                    if !handled {
                                        let reply_type = parsed.as_ref().and_then(|value| value.get("type")).and_then(Value::as_str);
                                        let is_reply = matches!(reply_type, Some("llm") | Some("tts"));
                                        if is_reply && !skill_fetched {
                                            if !turn_failed {
                                                turn_failed = true;
                                                let _ = self.tx.send(NetEvent::SkillRequired).await;
                                                log::warn!("Blocking cloud reply: current turn did not fetch primary Skill through MCP");
                                            }
                                            continue;
                                        }
                                        if reply_type == Some("tts") && parsed.as_ref().and_then(|value| value.get("state")).and_then(Value::as_str) == Some("stop") {
                                            turn_waiting = false;
                                            skill_fetched = self.mcp_server.has_instructions();
                                        }
                                        // 正常信令通道处理。聊天记录由 controller
                                        // 按消息语义（用户 / 助手 / 系统）统一写入，
                                        // 这里不再广播原文，免得同一句话被记两遍。
                                        log::info!("Received Text: {}", text);
                                        self.tx.send(NetEvent::Text(text.to_string())).await?;
                                    }
                                }
                                Message::Binary(data) => {
                                    if !skill_fetched {
                                        if !turn_failed {
                                            turn_failed = true;
                                            self.tx.send(NetEvent::SkillRequired).await?;
                                        }
                                        continue;
                                    }
                                    self.tx.send(NetEvent::Binary(data.to_vec())).await?;
                                }
                                Message::Close(frame) => {
                                    log::info!("Server closed connection: {:?}", frame);
                                    return Err(anyhow::anyhow!("Connection closed"));
                                }
                                _ => {}
                            }
                        }
                        Some(Err(e)) => return Err(e.into()),
                        None => return Err(anyhow::anyhow!("Connection closed")),
                    }
                }
                Some(cmd) = self.rx_cmd.recv() => {
                    match cmd {
                        NetCommand::SendText(text) => {
                            if let Ok(value) = serde_json::from_str::<Value>(&text) {
                                if value.get("type").and_then(Value::as_str) == Some("listen")
                                    && matches!(value.get("state").and_then(Value::as_str), Some("start") | Some("detect")) {
                                    if turn_waiting { return Err(anyhow::anyhow!("Previous reply is still pending; reconnecting to isolate turns")); }
                                    skill_fetched = self.mcp_server.has_instructions();
                                    turn_failed = false;
                                    turn_waiting = true;
                                    skill_deadline = tokio::time::Instant::now() + tokio::time::Duration::from_secs(60);
                                }
                            }
                            write.send(Message::Text(text.into())).await?;
                        }
                        NetCommand::SendBinary(data) => {
                            write.send(Message::Binary(data.into())).await?;
                        }
                        NetCommand::EnsureSkillsReady(waiter) => {
                            self.ready_waiters.push(waiter);
                            if self.mcp_server.skill_refresh_needed() {
                                log::info!("Skill changed: waiting for fresh MCP tools before audio upload");
                                return Ok(());
                            }
                        }
                        NetCommand::Reconnect => {
                            return Ok(());
                        }
                        NetCommand::RefreshSkillsIfChanged => {
                            // 唤醒时把主技能对齐：内容没变就什么都不做，不打扰这一轮对话
                            if self.mcp_server.skill_refresh_needed() {
                                log::info!("Primary Skill changed since last MCP initialize, recreating session");
                                return Ok(());
                            }
                        }
                    }
                }
                else => break,
            }
        }
        Ok(())
    }
}

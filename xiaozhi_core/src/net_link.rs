use crate::config::Config;
use crate::mcp_gateway::McpServer;
use futures_util::{SinkExt, StreamExt};
use mac_address::get_mac_address;
use serde::Serialize;
use serde_json::{Value, json};
use std::sync::Arc;
use tokio::sync::mpsc;
use tokio_tungstenite::{connect_async, tungstenite::protocol::Message};
use url::Url;
use uuid::Uuid;

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
    AbortTurn(
        Option<String>,
        tokio::sync::oneshot::Sender<Result<(), String>>,
    ),
    /// Recreate the session so a changed Skill selection is sent during MCP
    /// initialize on the next connection.
    Reconnect,
    /// 唤醒（开始说话）时调用：只有主技能确实变了才重建会话，避免每次唤醒都断连
    RefreshSkillsIfChanged,
    EnsureSkillsReady(tokio::sync::oneshot::Sender<Result<(), String>>),
}

fn abort_message(session_id: Option<&str>) -> Value {
    let mut value = json!({"type":"abort", "reason":"user"});
    if let Some(session_id) = session_id.filter(|id| !id.is_empty()) {
        value["session_id"] = json!(session_id);
    }
    value
}

// 音频参数结构体
fn confirms_primary_skill(request: &Value, response: &Value) -> bool {
    if request.get("method").and_then(Value::as_str) != Some("tools/call")
        || request
            .get("params")
            .and_then(|p| p.get("name"))
            .and_then(Value::as_str)
            != Some("skill_list")
        || response.get("error").is_some()
    {
        return false;
    }
    let Some(text) = response
        .get("result")
        .and_then(|result| result.get("content"))
        .and_then(Value::as_array)
        .and_then(|items| {
            items
                .iter()
                .find_map(|item| item.get("text").and_then(Value::as_str))
        })
    else {
        return false;
    };
    let Ok(value) = serde_json::from_str::<Value>(text) else {
        return false;
    };
    value
        .get("primary_instructions")
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
        Self {
            config,
            tx,
            rx_cmd,
            mcp_server,
            ready_waiters: Vec::new(),
        }
    }

    // 如果发生错误断开连接，5秒后重连
    pub async fn run(mut self) {
        // 重试机制，指数退避
        let mut retry_delay = 1;
        loop {
            let connection_started = tokio::time::Instant::now();
            if let Err(e) = self.connect_and_loop().await {
                if connection_started.elapsed() >= tokio::time::Duration::from_secs(60) {
                    retry_delay = 1;
                }
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
        let mut skill_fetched = !self.config.mcp.enabled
            || self.mcp_server.has_instructions()
            || self.mcp_server.skills_ready();
        let mut turn_waiting = false;
        let mut turn_failed = false;
        let mut output_suppressed = false;
        let mut mcp_tasks: tokio::task::JoinSet<(Value, Option<String>, String, bool)> =
            tokio::task::JoinSet::new();
        let mut skill_deadline = tokio::time::Instant::now();
        let mut ready_tick = tokio::time::interval(tokio::time::Duration::from_millis(50));
        let mut heartbeat = tokio::time::interval(tokio::time::Duration::from_secs(20));
        heartbeat.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        loop {
            tokio::select! {
                _ = heartbeat.tick() => {
                    write.send(Message::Ping(Vec::new().into())).await?;
                }
                _ = ready_tick.tick() => {
                    self.ready_waiters.retain(|waiter| !waiter.is_closed());
                    if turn_waiting && !skill_fetched && !turn_failed && tokio::time::Instant::now() >= skill_deadline {
                        turn_failed = true;
                        let _ = self.tx.send(NetEvent::SkillRequired).await;
                        log::warn!("Blocking turn after MCP Skill fetch timeout");
                    }
                    if hello_seen && (!self.config.mcp.enabled || self.mcp_server.skills_ready()) {
                        if !ready_sent {
                            self.tx.send(NetEvent::Ready).await?;
                            ready_sent = true;
                        }
                        for waiter in self.ready_waiters.drain(..) { let _ = waiter.send(Ok(())); }
                    }
                }
                Some(result) = mcp_tasks.join_next(), if !mcp_tasks.is_empty() => {
                    if let Ok((payload, Some(response), session_id, wrapped)) = result {
                        if response.is_empty() { continue; }
                        let response_payload = serde_json::from_str::<Value>(&response).unwrap_or(Value::Null);
                        let fetched = turn_waiting && !turn_failed && confirms_primary_skill(&payload, &response_payload);
                        let response = if wrapped {
                            json!({"type":"mcp", "session_id":session_id, "payload":response_payload})
                        } else { response_payload };
                        write.send(Message::Text(response.to_string().into())).await?;
                        if fetched || (payload.get("method").and_then(Value::as_str) == Some("initialize") && self.mcp_server.has_instructions()) {
                            skill_fetched = true;
                        }
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
                                        if output_suppressed && payload.get("method").and_then(Value::as_str) == Some("tools/call") {
                                            continue;
                                        }
                                        let server = self.mcp_server.clone();
                                        let session_id = session_id.to_string();
                                        mcp_tasks.spawn(async move {
                                            let response = server.handle_message(&payload.to_string()).await;
                                            (payload, response, session_id, wrapped)
                                        });
                                        true
                                    } else { false };

                                    if !handled {
                                        let reply_type = parsed.as_ref().and_then(|value| value.get("type")).and_then(Value::as_str);
                                        if output_suppressed && matches!(reply_type, Some("llm" | "tts" | "stt" | "iot")) { continue; }
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
                                            skill_fetched = !self.config.mcp.enabled || self.mcp_server.has_instructions() || self.mcp_server.skills_ready();
                                        }
                                        // 正常信令通道处理。聊天记录由 controller
                                        // 按消息语义（用户 / 助手 / 系统）统一写入，
                                        // 这里不再广播原文，免得同一句话被记两遍。
                                        log::info!("Received Text: {}", text);
                                        self.tx.send(NetEvent::Text(text.to_string())).await?;
                                    }
                                }
                                Message::Binary(data) => {
                                    if output_suppressed { continue; }
                                    if !skill_fetched {
                                        if !turn_failed {
                                            turn_failed = true;
                                            self.tx.send(NetEvent::SkillRequired).await?;
                                        }
                                        continue;
                                    }
                                    self.tx.send(NetEvent::Binary(data.to_vec())).await?;
                                }
                                Message::Ping(_) => {
                                    write.flush().await?;
                                }
                                Message::Pong(_) => {}
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
                                    if turn_waiting {
                                        log::warn!("Previous turn still pending; replacing it with protocol abort without reconnect");
                                        mcp_tasks.abort_all();
                                        write.send(Message::Text(abort_message(None).to_string().into())).await?;
                                    }
                                    output_suppressed = false;
                                    skill_fetched = !self.config.mcp.enabled || self.mcp_server.has_instructions() || self.mcp_server.skills_ready();
                                    turn_failed = false;
                                    turn_waiting = true;
                                    skill_deadline = tokio::time::Instant::now() + tokio::time::Duration::from_secs(60);
                                }
                            }
                            write.send(Message::Text(text.into())).await?;
                        }
                        NetCommand::AbortTurn(session_id, reply) => {
                            output_suppressed = true;
                            turn_waiting = false;
                            turn_failed = false;
                            mcp_tasks = tokio::task::JoinSet::new();
                            let result = write.send(Message::Text(abort_message(session_id.as_deref()).to_string().into())).await;
                            let failed = result.is_err();
                            let _ = reply.send(result.map_err(|error| format!("本地已停止，云端中断发送失败：{}", error)));
                            if failed { return Err(anyhow::anyhow!("Abort WebSocket write failed")); }
                        }
                        NetCommand::SendBinary(data) => {
                            if !output_suppressed { write.send(Message::Binary(data.into())).await?; }
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mcp_gateway::tool::McpTool;
    use std::sync::atomic::{AtomicBool, Ordering};
    use tokio::time::{Duration, timeout};

    struct SlowTool {
        started: Arc<tokio::sync::Notify>,
        cancelled: Arc<AtomicBool>,
    }
    struct CancelGuard(Arc<AtomicBool>);
    impl Drop for CancelGuard {
        fn drop(&mut self) {
            self.0.store(true, Ordering::Release);
        }
    }
    #[async_trait::async_trait]
    impl McpTool for SlowTool {
        fn name(&self) -> &str {
            "slow"
        }
        fn description(&self) -> &str {
            "slow test tool"
        }
        fn input_schema(&self) -> Value {
            json!({"type":"object"})
        }
        async fn call(&self, _: Value) -> Result<Value, String> {
            let _guard = CancelGuard(self.cancelled.clone());
            self.started.notify_one();
            std::future::pending().await
        }
    }

    #[test]
    fn abort_protocol_has_reason_and_optional_session() {
        assert_eq!(abort_message(None), json!({"type":"abort","reason":"user"}));
        assert_eq!(abort_message(Some("session"))["session_id"], "session");
        assert!(abort_message(Some("")).get("session_id").is_none());
    }

    #[tokio::test]
    async fn abort_interrupts_mcp_and_keeps_session_for_next_turn() {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let started = Arc::new(tokio::sync::Notify::new());
        let cancelled = Arc::new(AtomicBool::new(false));
        let mut server = McpServer::new();
        server.register_tool(Box::new(SlowTool {
            started: started.clone(),
            cancelled: cancelled.clone(),
        }));
        let mut config = Config::default();
        config.ws_url = format!("ws://{}", listener.local_addr().unwrap()).into();
        config.device_id = "test".into();
        let (events, mut event_rx) = mpsc::channel(100);
        let (commands, command_rx) = mpsc::channel(100);
        let mut link = NetLink::new(config, events, command_rx, Arc::new(server));
        let link_task = tokio::spawn(async move { link.connect_and_loop().await });
        let (socket, _) = listener.accept().await.unwrap();
        let mut socket = tokio_tungstenite::accept_async(socket).await.unwrap();
        while !matches!(socket.next().await.unwrap().unwrap(), Message::Text(_)) {}
        socket
            .send(Message::Text(
                json!({"type":"hello","session_id":"kept"})
                    .to_string()
                    .into(),
            ))
            .await
            .unwrap();
        socket.send(Message::Text(json!({"type":"mcp","session_id":"kept","payload":{
            "jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"slow","arguments":{}}
        }}).to_string().into())).await.unwrap();
        timeout(Duration::from_secs(1), started.notified())
            .await
            .unwrap();
        let (reply, result) = tokio::sync::oneshot::channel();
        commands
            .send(NetCommand::AbortTurn(Some("kept".into()), reply))
            .await
            .unwrap();
        timeout(Duration::from_secs(1), result)
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        let abort = timeout(Duration::from_secs(1), async {
            loop {
                if let Message::Text(text) = socket.next().await.unwrap().unwrap() {
                    break serde_json::from_str::<Value>(&text).unwrap();
                }
            }
        })
        .await
        .unwrap();
        assert_eq!(abort, abort_message(Some("kept")));
        tokio::task::yield_now().await;
        assert!(cancelled.load(Ordering::Acquire));
        while event_rx.try_recv().is_ok() {}
        socket
            .send(Message::Text(
                json!({"type":"tts","state":"sentence_start","text":"old"})
                    .to_string()
                    .into(),
            ))
            .await
            .unwrap();
        socket
            .send(Message::Binary(vec![1, 2, 3].into()))
            .await
            .unwrap();
        let _ = timeout(Duration::from_millis(100), async {
            while let Some(event) = event_rx.recv().await {
                assert!(
                    !matches!(
                        event,
                        NetEvent::Binary(_) | NetEvent::SkillRequired | NetEvent::Disconnected
                    ),
                    "unexpected aborted-turn event: {event:?}"
                );
                if let NetEvent::Text(text) = event {
                    assert_ne!(serde_json::from_str::<Value>(&text).unwrap()["type"], "tts");
                }
            }
        })
        .await;
        commands
            .send(NetCommand::SendText(
                r#"{"type":"listen","state":"start"}"#.into(),
            ))
            .await
            .unwrap();
        let next = timeout(Duration::from_secs(1), async {
            loop {
                if let Message::Text(text) = socket.next().await.unwrap().unwrap() {
                    break text;
                }
            }
        })
        .await
        .unwrap();
        assert_eq!(
            serde_json::from_str::<Value>(&next).unwrap()["type"],
            "listen"
        );
        link_task.abort();
    }
}

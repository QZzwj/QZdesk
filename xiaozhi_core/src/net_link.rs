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
}

// 音频参数结构体
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
}

impl NetLink {
    pub fn new(
        config: Config,
        tx: mpsc::Sender<NetEvent>,
        rx_cmd: mpsc::Receiver<NetCommand>,
        mcp_server: Arc<McpServer>,
    ) -> Self {
        Self { config, tx, rx_cmd, mcp_server }
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
            } else {
                // 如果连接和主循环正常退出，重置重试延迟
                // If it returns Ok, it might mean clean exit or just a disconnect that wasn't caught as Err?
                // In our case, connect_and_loop returns Err on disconnect.
                // If it returns Ok, it means we are shutting down (rx_cmd closed).
                break;
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

        // 主循环，处理读取和写入
        loop {
            tokio::select! {
                msg = read.next() => {
                    match msg {
                        Some(Ok(msg)) => {
                            match msg {
                                Message::Text(text) => {
                                    // 服务端通常使用 {type:mcp,payload:{...}}，但
                                    // 部分网关会把 payload 编成 JSON 字符串，或直接发
                                    // JSON-RPC。三种形态都接受，避免主 Skill 因封装差异失效。
                                    let parsed = serde_json::from_str::<Value>(&text).ok();
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
                                                let response = if wrapped {
                                                    json!({"type": "mcp", "session_id": session_id, "payload": response_payload})
                                                } else {
                                                    response_payload
                                                };
                                                let response_text = serde_json::to_string(&response).unwrap();
                                                log::info!("MCP Response: {}", response_text);
                                                write.send(Message::Text(response_text.into())).await?;
                                                true
                                            }
                                        } else {
                                            false
                                        }
                                    } else {
                                        false
                                    };

                                    if !handled {
                                        // 正常信令通道处理。聊天记录由 controller
                                        // 按消息语义（用户 / 助手 / 系统）统一写入，
                                        // 这里不再广播原文，免得同一句话被记两遍。
                                        log::info!("Received Text: {}", text);
                                        self.tx.send(NetEvent::Text(text.to_string())).await?;
                                    }
                                }
                                Message::Binary(data) => {
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
                            let outgoing = if let Ok(mut value) = serde_json::from_str::<Value>(&text) {
                                let is_detect = value.get("type").and_then(Value::as_str) == Some("listen")
                                    && value.get("state").and_then(Value::as_str) == Some("detect");
                                if is_detect {
                                    if let Some(user_text) = value.get("text").and_then(Value::as_str) {
                                        if let Some(augmented) = self.mcp_server.augment_text_request(user_text) {
                                            log::warn!("Primary Skill was not delivered by MCP initialize; injecting it into this text turn");
                                            value["text"] = Value::String(augmented);
                                            serde_json::to_string(&value)?
                                        } else {
                                            text.clone()
                                        }
                                    } else {
                                        text.clone()
                                    }
                                } else {
                                    text.clone()
                                }
                            } else {
                                text.clone()
                            };
                            write.send(Message::Text(outgoing.into())).await?;
                        }
                        NetCommand::SendBinary(data) => {
                            write.send(Message::Binary(data.into())).await?;
                        }
                        NetCommand::Reconnect => {
                            return Err(anyhow::anyhow!("Reconnect requested"));
                        }
                        NetCommand::RefreshSkillsIfChanged => {
                            // 唤醒时把主技能对齐：内容没变就什么都不做，不打扰这一轮对话
                            if self.mcp_server.skill_refresh_needed() {
                                log::info!("Primary Skill changed since last MCP initialize, recreating session");
                                return Err(anyhow::anyhow!("Skill changed, reconnect requested"));
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

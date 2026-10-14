use crate::audio::OpusEncoder;
use crate::audio_bridge::{AudioBridge, AudioEvent};
use crate::chat::{ChatHub, ChatRole};
use crate::config::Config;
use crate::gui_bridge::{GuiBridge, GuiEvent};
use crate::net_link::{NetCommand, NetEvent};
use crate::protocol::ServerMessage;
use crate::state_machine::SystemState;
use crate::tts::TtsService;
use crate::performance::PerformanceService;
use crate::weather::WeatherService;
use serde_json;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::mpsc;
use tokio::process::Command;
use std::process::Stdio;


/// 本机把文字合成上行后，云端识别回来的那条 stt 在这个时间窗内算作回显。
/// 超过了就当它是用户真的说了话，照常记录。
///
/// 判定不看内容是否相等：ASR 常把同音字、字母、短词认错，用「内容相等」判会
/// 把错别字当成用户新说的一句，模型顺着错别字回答，后面整段对话都会变乱。
/// 上行期间麦克风是静音的，所以窗口内的 stt 只可能来自我们刚发出去的那段音频。
/// 用户真的开口时由 GUI 的 listen start 立刻清掉标记。
const TYPED_TURN_ECHO_WINDOW: Duration = Duration::from_secs(20);

/// GUI 连上（或重启）时补多少条历史。逐条 UDP 发送，条数取一屏够看的量。
const HISTORY_REPLAY_LIMIT: usize = 30;

/// 「有人靠近」时喂给模型的那句话。写得像用户的指令，模型就会自然回一句问候；
/// 它只上行去合成语音，不会作为用户气泡出现在聊天记录里（记录里留的是一条系统
/// 提示，说明这次开口是存在检测触发的）。
const PRESENCE_GREETING: &str = "（有人来到了设备前，请用你的口吻主动打个招呼，一句话就好）";

/// 两次「靠近打招呼」之间至少隔多久。默认 5 分钟：在桌前走动不该被反复打断，
/// 可用 `QZDESK_PRESENCE_COOLDOWN_SECS` 调整。
const PRESENCE_COOLDOWN_SECS: u64 = 300;

/// 交给核心处理的请求（区别于直接透传给服务器的原始报文）。
///
/// 目前只有一件事：把文字合成成语音再发给云端。web 控制台与 GUI 走的是同一
/// 条路径，避免两处各写一遍「合成 → 分包 → 定时」的逻辑。
pub enum CoreCommand {
    /// 把这段文字合成语音，当成用户说了一句话。
    TextAsSpeech(String, tokio::sync::oneshot::Sender<Result<(), String>>),
    SkillsSaved,
    Status(tokio::sync::oneshot::Sender<serde_json::Value>),
    AbortTurn(tokio::sync::oneshot::Sender<Result<(), String>>),
}

pub struct CoreController {
    state: SystemState,
    current_session_id: Option<String>,
    should_mute_mic: bool,
    config: Config,
    net_tx: mpsc::Sender<NetCommand>,
    audio_bridge: Arc<AudioBridge>,
    gui_bridge: Arc<GuiBridge>,
    tts: Arc<TtsService>,
    /// GUI 与 web 共用的聊天记录。
    chat: ChatHub,
    /// 当前用户轮次已显示的助手文本。
    last_assistant: Vec<String>,
    /// 本机刚合成上行的那段文字（已归一化）与时间。
    ///
    /// 云端会对我们上行的音频再做一次 ASR，并把识别结果当成"用户说了话"回传
    /// （`stt`）。那正是我们刚记下的那句话，再记一条就会在聊天里出现两条用户
    /// 消息 —— 而且云端有时会把这同一条 stt 发两遍。所以在这个时间窗内、内容
    /// 与刚上行文字相同的 stt 都算回显；用户真的说了别的话则照常记录。
    injected_text: Option<(String, Instant)>,
    /// 天气服务：GUI 要快照、点卡片刷新都只转给它，控制器自己不碰网络。
    weather: WeatherService,
    /// 性能监控：设备页进来时只要一份现成快照，采样由它自己的任务负责。
    performance: PerformanceService,
    /// 上次"有人靠近就打招呼"的时刻。有人在桌前走动不该被反复打断，所以按
    /// `PRESENCE_COOLDOWN` 限流。
    last_presence_greeting: Option<Instant>,
    speech_task: Option<tokio::task::JoinHandle<Result<(), String>>>,
    skills_pending: bool,
    new_chat_pending: bool,
    output_suppressed: bool,
}

/// 归一化聊天文本：只保留字母和数字（各语种的文字都算字母），丢掉空白、标点
/// 和表情。ASR 会给识别结果补标点（"你是谁" -> "你是谁？"），比对回显时用得上。
fn chat_text_key(text: &str) -> String {
    text.chars().filter(|ch| ch.is_alphanumeric()).collect()
}

/// 修复云端偶发的相邻片段回显，例如“我是我是QZdeskdesk的的智能智能助手助手”。
/// 只处理连续重复的短片段（1~12 个字符），不改动整句重复或正常的词语重复。
fn collapse_adjacent_echo(text: &str) -> String {
    let chars: Vec<char> = text.chars().collect();
    /* A single repeated character is often intentional (“哈哈”). Only treat
     * one-character runs as echo when the same reply also contains a longer
     * repeated segment, as in “我是我是…智能智能”. */
    let has_long_echo = (0..chars.len()).any(|start| {
        (2..=((chars.len() - start) / 2).min(12)).any(|width| {
            chars[start..start + width] == chars[start + width..start + width * 2]
        })
    });
    let mut out = String::with_capacity(text.len());
    let mut index = 0;
    while index < chars.len() {
        let mut removed = false;
        let max_width = ((chars.len() - index) / 2).min(12);
        let min_width = if has_long_echo { 1 } else { 2 };
        for width in (min_width..=max_width).rev() {
            let left = &chars[index..index + width];
            let right = &chars[index + width..index + width * 2];
            if left == right {
                for ch in left {
                    out.push(*ch);
                }
                index += width * 2;
                removed = true;
                break;
            }
        }
        if !removed {
            out.push(chars[index]);
            index += 1;
        }
    }
    out
}

impl CoreController {
    pub fn new(
        config: Config,
        net_tx: mpsc::Sender<NetCommand>,
        audio_bridge: Arc<AudioBridge>,
        gui_bridge: Arc<GuiBridge>,
        tts: Arc<TtsService>,
        chat: ChatHub,
        weather: WeatherService,
        performance: PerformanceService,
    ) -> Self {
        Self {
            state: SystemState::NetworkError,
            current_session_id: None,
            should_mute_mic: false,
            config,
            net_tx,
            audio_bridge,
            gui_bridge,
            tts,
            chat,
            last_assistant: Vec::new(),
            injected_text: None,
            weather,
            performance,
            last_presence_greeting: None,
            speech_task: None,
            skills_pending: false,
            new_chat_pending: false,
            output_suppressed: false,
        }
    }

    /// 记一条聊天记录，并同步推给 GUI。
    ///
    /// 这是唯一的写入点：web 从广播里拿，GUI 从 UDP 里拿，两边看到的永远是同一
    /// 份记录 —— 在 GUI 打字会出现在网页上，反之亦然。
    async fn push_chat(&mut self, role: ChatRole, text: &str) {
        let text = if role == ChatRole::Assistant {
            collapse_adjacent_echo(text.trim())
        } else {
            text.trim().to_string()
        };
        if text.is_empty() {
            return;
        }
        if role != ChatRole::Assistant {
            /* 用户说了话、或核心插了一条提示，说明上一句回复已经翻篇：助手再
             * 说出同样的话就是新的一句，不该被当作重复吃掉。 */
            self.last_assistant.clear();
        }
        if let Some(event) = self.chat.push(role, &text) {
            if let Err(error) = self.gui_bridge.send_message(&event).await {
                log::warn!("推送聊天记录到 GUI 失败: {}", error);
            }
        }
    }

    fn assistant_text_is_new(&mut self, text: &str) -> bool {
        let text = text.trim();
        if text.is_empty() || self.last_assistant.iter().any(|seen| seen == text) {
            return false;
        }
        self.last_assistant.push(text.to_string());
        true
    }

    /// 把最近的记录逐条补发给 GUI。
    ///
    /// GUI 走 UDP、不能订阅广播，重连后手上的记录可能是空的；补一遍就能让两端
    /// 从同一份历史继续。
    async fn replay_chat_history(&self) {
        let history = self.chat.history();
        let start = history.len().saturating_sub(HISTORY_REPLAY_LIMIT);
        for entry in &history[start..] {
            let event = serde_json::json!({
                "type": "chat",
                "role": entry.role,
                "text": entry.text,
                "id": entry.id,
                "ts": entry.ts,
            })
            .to_string();
            if let Err(error) = self.gui_bridge.send_message(&event).await {
                log::warn!("补发聊天记录到 GUI 失败: {}", error);
                break;
            }
        }
        log::info!("已向 GUI 补发 {} 条聊天记录", history.len() - start);
    }

    // 处理来自 NetLink 的事件
    pub async fn handle_net_event(&mut self, event: NetEvent) {
        match event {
            NetEvent::Text(text) => self.process_server_text(text).await,
            NetEvent::Binary(data) => self.process_server_audio(data).await,
            NetEvent::Connected => {
                log::info!("WebSocket Connected");
                self.state = SystemState::NetworkError;
                if let Err(e) = self.gui_bridge.send_message(r#"{"state": 4, "type":"toast", "text":"正在协商新会话与MCP技能"}"#).await {
                    log::error!("Failed to send to GUI: {}", e);
                }
            }
            NetEvent::Ready => {
                if self.new_chat_pending {
                    self.new_chat_pending = false;
                    self.push_chat(ChatRole::Divider, "新聊天 · 会话已连接，技能已投递（非执行确认）").await;
                }
                if self.state == SystemState::NetworkError {
                    self.state = SystemState::Idle;
                    let _ = self.gui_bridge.send_message(r#"{"state":3}"#).await;
                }
            }
            NetEvent::SkillRequired => {
                self.push_chat(ChatRole::System, "本轮回复已阻止：云端必须先调用 MCP skill_list 获取主技能正文，请重试。").await;
                self.should_mute_mic = false;
                self.cancel_speech().await;
                let _ = self.net_tx.send(NetCommand::Reconnect).await;
                self.state = SystemState::NetworkError;
                let _ = self.gui_bridge.send_message(r#"{"state":4}"#).await;
            }
            NetEvent::Disconnected => {
                log::info!("WebSocket Disconnected");
                self.current_session_id = None;
                self.cancel_speech().await;
                self.state = SystemState::NetworkError;
                if let Err(e) = self.gui_bridge.send_message(r#"{"state": 4}"#).await {
                    log::error!("Failed to send to GUI: {}", e);
                }
            }
        }
    }

    // 处理来自服务器的文本消息
    async fn process_server_text(&mut self, text: String) {
        log::info!("Received Text from Server: {}", text);

        let msg: ServerMessage = match serde_json::from_str(&text) {
            Ok(msg) => msg,
            Err(_) => {
                // 可能不是JSON，忽略
                return;
            }
        };

        if let Some(sid) = &msg.session_id {
            if self.current_session_id.as_deref() != Some(sid) {
                log::info!("New Session ID: {}", sid);
                self.current_session_id = Some(sid.clone());
            }
        }

        if self.output_suppressed && matches!(msg.msg_type.as_str(), "tts" | "llm" | "stt" | "iot") {
            return;
        }
        match msg.msg_type.as_str() {
            "hello" => {
                // Do not enter cloud `auto` listen immediately.  That mode
                // treats an idle microphone as a conversation timeout; the
                // server then speaks a greeting/goodbye and closes the
                // session, which caused an endless reconnect loop when no
                // one was talking.  Listening must now be explicitly
                // requested by a UI / push-to-talk action.
                log::info!("Server Hello received; waiting for an explicit listen request");
            }
            "iot" => {
                if let Some(cmd) = &msg.command {
                    log::info!("Processing IoT Command: {}", cmd);
                }
                
                // Fallback: 把接收到的完整 JSON 传递给外部脚本执行
                let fallback_script = "./scripts/mcp_iot_fallback.sh";
                let text_clone = text.clone();
                tokio::spawn(async move {
                    let mut child = match Command::new(fallback_script)
                        .stdin(Stdio::piped())
                        .stdout(Stdio::piped())
                        .stderr(Stdio::piped())
                        .spawn()
                    {
                        Ok(c) => c,
                        Err(e) => {
                            log::error!("Failed to spawn IoT fallback script {}: {}", fallback_script, e);
                            return;
                        }
                    };
                    
                    if let Some(mut stdin) = child.stdin.take() {
                        use tokio::io::AsyncWriteExt;
                        if let Err(e) = stdin.write_all(text_clone.as_bytes()).await {
                            log::error!("Failed to write to IoT fallback script stdin: {}", e);
                        }
                    }
                    
                    match child.wait_with_output().await {
                        Ok(output) => {
                            if !output.status.success() {
                                let err_str = String::from_utf8_lossy(&output.stderr);
                                log::error!("IoT fallback script failed: {}", err_str);
                            } else {
                                let out_str = String::from_utf8_lossy(&output.stdout);
                                if !out_str.trim().is_empty() {
                                    log::info!("IoT fallback script output: {}", out_str);
                                }
                            }
                        }
                        Err(e) => {
                            log::error!("Failed to wait for IoT fallback script: {}", e);
                        }
                    }
                });
            }
            "tts" => {
                if let Some(state) = &msg.state {
                    if state == "start" || state == "sentence_start" {
                        self.should_mute_mic = true;
                        self.state = SystemState::Speaking;
                        log::info!("TTS Started (state={}), muting mic for AEC, sending state 6 to GUI", state);
                        if let Err(e) = self.gui_bridge.send_message(r#"{"state": 6}"#).await {
                            log::error!("Failed to send state 6 to GUI: {}", e);
                        }
                    } else if state == "stop" {
                        self.should_mute_mic = false;
                        self.state = SystemState::Idle;
                        log::info!("TTS Stopped (state={}), unmuting mic, sending state 3 to GUI", state);
                        if let Err(e) = self.gui_bridge.send_message(r#"{"state": 3}"#).await {
                            log::error!("Failed to send state 3 to GUI: {}", e);
                        }
                        // Do not automatically start another listen cycle.
                        // The next cycle is initiated by an explicit user
                        // action, avoiding phantom conversations in silence.
                    }
                }

                if let Some(t) = msg.text {
                    log::info!("TTS: {}", t);
                    // 开关关掉时两边都不显示助手文字（语音照常播）；
                    // 记进来的文字对 GUI 与 web 是同一份。
                    if self.config.enable_tts_display && self.assistant_text_is_new(&t) {
                        self.push_chat(ChatRole::Assistant, &t).await;
                    }
                }
            }
            "stt" => {
                if let Some(text_content) = msg.text {
                    /* 本机自己合成的语音转了一圈又识别回来（云端还会把它发两遍）。
                     *
                     * 判定**不能**用「和上行文字一模一样」：ASR 对同音字、字母、
                     * 短词经常认错（粤嵌→月欠、jb→GB、啥阴→舌音）。一旦不相等，
                     * 错别字就会被当成用户又说了一句记进去，模型顺着错别字回答，
                     * 后面整段对话全是乱码。
                     *
                     * 改成按「这一轮到底是不是我们合成的」判定：上行期间麦克风是
                     * 静音的，云端在这个窗口里识别到的只可能是我们刚发出去的那段
                     * 音频，所以窗口内的 stt 一律不记为用户消息。用户真的开口（GUI
                     * 按住说话）会清掉标记，见 `handle_gui_event`。
                     */
                    let echoed = self
                        .injected_text
                        .as_ref()
                        .map_or(false, |(_, at)| at.elapsed() < TYPED_TURN_ECHO_WINDOW);
                    if echoed {
                        let typed = self
                            .injected_text
                            .as_ref()
                            .map(|(typed, _)| typed.clone())
                            .unwrap_or_default();
                        log::info!(
                            "STT 回显（本机上行的「{}」被云端听成了「{}」，不计入对话）",
                            typed,
                            text_content
                        );
                    } else {
                        // 说的是别的内容：这一轮已经不是我们上行的那句了。
                        self.injected_text = None;
                        log::info!("STT Result: {}", text_content);
                        self.push_chat(ChatRole::User, &text_content).await;
                    }
                }
            }
            "llm" => {
                // Newer cloud sessions may emit an LLM event (for example an
                // emotion or incremental text marker) before the corresponding
                // TTS event. It is a valid protocol message, not an error.
                if let Some(text_content) = msg.text {
                    log::info!("LLM Result: {}", text_content);
                } else {
                    log::debug!("LLM event received without text");
                }
            }
            "alert" => {
                let message = msg
                    .message
                    .as_deref()
                    .or(msg.text.as_deref())
                    .unwrap_or("服务器返回了未说明原因的错误");
                log::warn!(
                    "Server alert (status={}, emotion={}): {}",
                    msg.status.as_deref().unwrap_or("unknown"),
                    msg.emotion.as_deref().unwrap_or("unknown"),
                    message
                );
                // 云端报错也进聊天记录：GUI 与 web 一起看到，不会一边知道一边不知道。
                self.push_chat(ChatRole::System, message).await;
            }
            other => {
                log::warn!("Unhandled message type: {}", other);
            }
        }
    }

    // 处理来自服务器的音频数据
    async fn process_server_audio(&mut self, data: Vec<u8>) {
        if self.output_suppressed { return; }
        if self.state != SystemState::Speaking {
            self.state = SystemState::Speaking;
            if let Err(e) = self.gui_bridge.send_message(r#"{"state": 6}"#).await {
                log::error!("Failed to send to GUI: {}", e);
            }
        }
        if let Err(e) = self.audio_bridge.send_audio(&data).await {
            log::error!("Failed to send to Audio: {}", e);
        }
    }

    // 处理来自 AudioBridge 的事件
    pub async fn handle_audio_event(&mut self, event: AudioEvent) {
        match event {
            AudioEvent::AudioData(data) => {
                if self.should_mute_mic {
                    return;
                }
                // The recorder is always running, but microphone frames must
                // not be uploaded while the conversation is idle.  Earlier
                // code forwarded every frame, allowing silence/noise to
                // trigger phantom cloud conversations.
                if self.state != SystemState::Listening {
                    return;
                }
                if let Err(e) = self.net_tx.send(NetCommand::SendBinary(data)).await {
                    log::error!("Failed to send audio to NetLink: {}", e);
                }
            }
        }
    }

    /// 把一段文字合成成语音，按「用户说了一句话」发给云端。
    ///
    /// 当前云端限制 listen detect 为短唤醒词，文字聊天仍通过语音上行。
    pub async fn send_text_as_speech(&mut self, text: String) -> Result<(), String> {
        let text = text.trim().to_string();
        if text.is_empty() || text.len() > 4096 {
            return Err("消息不能为空且最多 4096 字节".to_string());
        }
        if self.state == SystemState::NetworkError {
            return Err("云端未连接或会话尚未就绪，请检查网络与核心连接日志".to_string());
        }
        if self.speech_task.is_some() || self.state != SystemState::Idle {
            return Err("当前对话正在处理，请先停止本轮或等待回复结束".to_string());
        }

        // 先把这条记进聊天记录：不管文字来自 GUI 输入框还是 web 控制台，两端都
        // 立刻看到同一句话，不用等合成完成。
        self.push_chat(ChatRole::User, &text).await;
        self.inject_speech(text).await;
        Ok(())
    }

    /// 注入一段「用户说的话」：本地合成成语音，再按麦克风同样的报文发上去。
    ///
    /// 聊天记录里留什么由调用方决定（文字聊天留用户气泡，存在检测留一条系统
    /// 提示），这里只负责把声音送出去，并记下这轮文本用于识别云端回显。
    async fn inject_speech(&mut self, text: String) {
        if self.speech_task.is_some() || self.state != SystemState::Idle {
            self.push_chat(ChatRole::System, "当前对话忙碌，请结束当前操作后重试；未执行新的语音上行").await;
            return;
        }
        self.output_suppressed = false;
        self.injected_text = Some((chat_text_key(&text), Instant::now()));
        self.should_mute_mic = true;
        self.state = SystemState::Listening;
        let _ = self.gui_bridge.send_message(r#"{"state": 5}"#).await;
        let tts = self.tts.clone();
        let net_tx = self.net_tx.clone();
        let config = self.config.clone();
        self.speech_task = Some(tokio::spawn(async move {
            let (pcm, sample_rate) = tokio::time::timeout(Duration::from_secs(30), tts.synthesize(&text))
                .await.map_err(|_| "文字合成超时，请重试".to_string())?
                .map_err(|error| format!("文字合成失败：{}", error))?;
            let mut encoder = OpusEncoder::new(sample_rate as u32, 1, 20,
                config.hello_sample_rate, config.hello_channels as u32, 64000)
                .map_err(|error| format!("创建语音编码器失败：{}", error))?;
            let frame_samples = encoder.input_frame_samples();
            if frame_samples == 0 || pcm.is_empty() {
                return Err("合成语音为空或帧长无效".to_string());
            }
            let (ready_tx, ready_rx) = tokio::sync::oneshot::channel();
            net_tx.send(NetCommand::EnsureSkillsReady(ready_tx)).await
                .map_err(|error| format!("技能同步失败：{}", error))?;
            tokio::time::timeout(Duration::from_secs(20), ready_rx).await
                .map_err(|_| "技能同步超时：云端尚未完成 Hello/MCP 工具表协商，本轮未发送".to_string())?
                .map_err(|_| "技能同步通道已关闭，本轮未发送".to_string())??;
            net_tx.send(NetCommand::SendText(r#"{"type":"listen","state":"start"}"#.to_string())).await
                .map_err(|error| format!("语音上行启动失败：{}", error))?;
            for chunk in pcm.chunks(frame_samples) {
                let mut frame = chunk.to_vec();
                frame.resize(frame_samples, 0);
                let packet = encoder.encode(&frame)
                    .map_err(|error| format!("语音编码失败：{}", error))?;
                net_tx.send(NetCommand::SendBinary(packet)).await
                    .map_err(|error| format!("语音帧发送失败：{}", error))?;
                tokio::time::sleep(Duration::from_millis(20)).await;
            }
            net_tx.send(NetCommand::SendText(r#"{"type":"listen","state":"stop"}"#.to_string())).await
                .map_err(|error| format!("语音上行结束失败：{}", error))?;
            Ok(())
        }));
    }

    pub fn capabilities(&self) -> serde_json::Value {
        serde_json::json!({"local":true,
            "cloud": self.current_session_id.is_some() && self.state != SystemState::NetworkError,
            "busy": self.state != SystemState::Idle || self.speech_task.is_some(),
            "tts":self.tts.available(), "skills_pending":self.skills_pending})
    }

    pub fn mark_skills_pending(&mut self) {
        self.skills_pending = true;
    }

    pub async fn poll_speech(&mut self) {
        if self.skills_pending && self.speech_task.is_none() && self.state == SystemState::Idle {
            if self.net_tx.try_send(NetCommand::Reconnect).is_ok() {
                self.skills_pending = false;
                self.state = SystemState::NetworkError;
                let _ = self.gui_bridge.send_message(r#"{"state":4,"type":"toast","text":"技能已保存，正在同步下一轮对话"}"#).await;
            }
        }
        if !self.speech_task.as_ref().map(|task| task.is_finished()).unwrap_or(false) {
            return;
        }
        let result = self.speech_task.take().unwrap().await;
        self.should_mute_mic = matches!(self.state, SystemState::Listening | SystemState::Speaking);
        let error = match result {
            Ok(Ok(())) => None,
            Ok(Err(error)) => Some(error),
            Err(error) => Some(format!("语音任务异常：{}", error)),
        };
        if let Some(error) = error {
            self.injected_text = None;
            let _ = self.net_tx.send(NetCommand::SendText(r#"{"type":"listen","state":"stop"}"#.to_string())).await;
            self.push_chat(ChatRole::System, &error).await;
            if self.state == SystemState::Listening {
                self.should_mute_mic = false;
                self.state = SystemState::Idle;
                let _ = self.gui_bridge.send_message(r#"{"state": 3}"#).await;
            }
        }
    }

    pub async fn abort_turn(&mut self) -> Result<(), String> {
        self.output_suppressed = true;
        if let Some(task) = self.speech_task.take() {
            task.abort();
            let _ = task.await;
        }
        self.audio_bridge.flush_playback();
        self.should_mute_mic = false;
        self.injected_text = None;
        self.last_assistant.clear();
        if self.state != SystemState::NetworkError { self.state = SystemState::Idle; }
        let state = if self.state == SystemState::NetworkError { 4 } else { 3 };
        let _ = self.gui_bridge.send_message(&serde_json::json!({"state":state}).to_string()).await;
        let (reply, result) = tokio::sync::oneshot::channel();
        self.net_tx.try_send(NetCommand::AbortTurn(self.current_session_id.clone(), reply))
            .map_err(|_| "本地已停止，云端中断未发送：网络通道不可用或忙碌".to_string())?;
        tokio::time::timeout(Duration::from_secs(2), result).await
            .map_err(|_| "本地已停止，云端中断发送未确认".to_string())?
            .map_err(|_| "本地已停止，云端中断发送通道已关闭".to_string())?
    }

    pub async fn cancel_speech(&mut self) {
        if let Some(task) = self.speech_task.take() {
            task.abort();
            let _ = task.await;
            let _ = self.net_tx.send(NetCommand::SendText(r#"{"type":"listen","state":"stop"}"#.to_string())).await;
            self.should_mute_mic = false;
            self.injected_text = None;
        }
    }

    // 处理来自 GuiBridge 的事件
    /// 存在检测回调（报文见 `app/face_camera.c` 与 `qzdesk_core_notify_presence`）：
    /// 有人靠近时让助手打个招呼。
    ///
    /// 走的是和文字聊天同一条上行链路（合成 → 当成用户说的话发上去），所以问候语
    /// 是模型按当时的主技能与上下文现说的，还能出声。聊天记录里留一条系统提示，
    /// 说明这句话为什么会出现。
    pub async fn proactive_care(&mut self) -> bool {
        if self.state != SystemState::Idle || self.speech_task.is_some()
            || self.current_session_id.is_none() || !self.tts.available() { return false; }
        self.inject_speech("请沿用当前主 Skill 的人格，用一句简短自然的话关心用户，可以询问是否需要休息或喝水；不要假装知道用户的情绪或正在做什么。".to_string()).await;
        true
    }

    async fn handle_presence(&mut self, present: bool) {
        if !present {
            log::info!("存在检测：人离开了");
            return;
        }
        let cooldown = std::env::var("QZDESK_PRESENCE_COOLDOWN_SECS")
            .ok()
            .and_then(|value| value.parse::<u64>().ok())
            .unwrap_or(PRESENCE_COOLDOWN_SECS);
        if let Some(last) = self.last_presence_greeting {
            let waited = last.elapsed().as_secs();
            if waited < cooldown {
                log::info!(
                    "存在检测：有人靠近，但距上次打招呼只过了 {} 秒（冷却 {} 秒），先不打扰",
                    waited,
                    cooldown
                );
                return;
            }
        }
        self.last_presence_greeting = Some(Instant::now());
        log::info!("存在检测：有人靠近，让助手打个招呼");
        self.push_chat(ChatRole::System, "检测到有人靠近，正在打招呼")
            .await;
        self.inject_speech(PRESENCE_GREETING.to_string()).await;
    }

    pub async fn handle_gui_event(&mut self, event: GuiEvent) {
        let GuiEvent::Message(msg) = event;
        log::info!("Received Message from GUI: {}", msg);
        /* The GUI may be opened after the core has already sent its one-shot
         * Connected notification.  Reply to an explicit status request so
         * the page can synchronize its current state over UDP. */
        if let Ok(value) = serde_json::from_str::<serde_json::Value>(&msg) {
            if value.get("type").and_then(|item| item.as_str()) == Some("abort_turn") {
                let result = self.abort_turn().await;
                let _ = self.gui_bridge.send_message(&serde_json::json!({
                    "type":"command_ack", "command":"abort_turn", "request_id":value.get("request_id"),
                    "ok":result.is_ok(), "local_stopped":true, "cloud_abort_sent":result.is_ok(),
                    "text":result.err().unwrap_or_else(|| "本轮已停止；已发出的电脑动作无法撤销".into()),
                }).to_string()).await;
                return;
            }
            if value.get("type").and_then(|item| item.as_str()) == Some("new_chat") {
                let result = if self.state != SystemState::Idle || self.speech_task.is_some() {
                    Err("请等待当前对话结束后再新建聊天".to_string())
                } else {
                    self.net_tx.send(NetCommand::Reconnect).await
                        .map_err(|_| "网络通道不可用，请重试".to_string())
                };
                if result.is_ok() {
                    self.new_chat_pending = true;
                    self.current_session_id = None;
                    self.last_assistant.clear();
                    self.injected_text = None;
                    self.state = SystemState::NetworkError;
                    let _ = self.gui_bridge.send_message(r#"{"state":4}"#).await;
                }
                let _ = self.gui_bridge.send_message(&serde_json::json!({
                    "type":"command_ack", "command":"new_chat", "ok":result.is_ok(),
                    "text":result.err().unwrap_or_else(|| "已请求新会话，正在重新连接；历史与本地记忆保留".into()),
                }).to_string()).await;
                return;
            }
            if value.get("type").and_then(|item| item.as_str()) == Some("timezone") {
                let ok = value.get("value").and_then(|item| item.as_str())
                    .map(crate::timers::set_timezone).unwrap_or(false);
                let _ = self.gui_bridge.send_message(&serde_json::json!({
                    "type":"command_ack", "command":"timezone", "ok":ok,
                    "text": if ok { "时区已保存并同步" } else { "核心时区同步失败，请重试" },
                }).to_string()).await;
                return;
            }
            if value.get("type").and_then(|item| item.as_str()) == Some("status_request") {
                let state = match self.state {
                    crate::state_machine::SystemState::Idle => 3,
                    crate::state_machine::SystemState::Listening => 5,
                    crate::state_machine::SystemState::Speaking => 6,
                    crate::state_machine::SystemState::NetworkError => 4,
                };
                let response = format!(r#"{{"state": {}}}"#, state);
                if let Err(e) = self.gui_bridge.send_message(&response).await {
                    log::error!("Failed to send GUI status response: {}", e);
                }
                return;
            }

            // 存在检测：有人靠近 / 离开。界面侧的摄像头检测（app/face_camera.c）只
            // 报事实，怎么回应由核心决定 —— 亮屏是界面自己的事（背光在它那里），
            // 这里负责让助手打个招呼。
            if value.get("type").and_then(|item| item.as_str()) == Some("presence") {
                let present = value
                    .get("present")
                    .and_then(|item| item.as_bool())
                    .unwrap_or(false);
                self.handle_presence(present).await;
                return;
            }

            // Explicit push-to-talk / listen control from a GUI.  Automatic
            // listening is intentionally disabled; callers that want a voice
            // turn must send {"type":"listen","state":"start"} and later
            // {"type":"listen","state":"stop"}.
            if value.get("type").and_then(|item| item.as_str()) == Some("listen") {
                let start = value.get("state").and_then(|item| item.as_str()) == Some("start");
                if start && (self.state != SystemState::Idle && self.state != SystemState::Listening) {
                    let _ = self.gui_bridge.send_message(r#"{"type":"toast","text":"云端未就绪，录音未开始"}"#).await;
                    return;
                }
                self.cancel_speech().await;
                if start {
                    self.output_suppressed = false;
                    self.state = SystemState::Listening;
                    /* 用户自己开口了：接下来那条 stt 是真人说话，必须记录。
                     * 不摘掉上一轮打字留下的回声标记，这一句会被当成回显丢掉。 */
                    self.injected_text = None;
                    let _ = self.gui_bridge.send_message(r#"{"state": 5}"#).await;
                    // 唤醒（开始说话）时先对齐主技能：若技能在本次连接期间变过，
                    // 会重建会话让云端重新 initialize，这一轮就用上最新的主技能。
                    // 没变则什么都不做，不影响对话。
                    if let Err(e) = self.net_tx.send(NetCommand::RefreshSkillsIfChanged).await {
                        log::error!("Failed to request Skill refresh on wake: {}", e);
                    }
                } else if value.get("state").and_then(|item| item.as_str()) == Some("stop") {
                    self.state = SystemState::Idle;
                    let _ = self.gui_bridge.send_message(r#"{"state": 3}"#).await;
                }
                if let Err(e) = self.net_tx.send(NetCommand::SendText(msg)).await {
                    log::error!("Failed to send listen command to NetLink: {}", e);
                    let _ = self
                        .gui_bridge
                        .send_message(&serde_json::json!({
                            "type": "toast",
                            "text": "网络通道不可用，消息未发送",
                        }).to_string())
                        .await;
                }
                return;
            }

            // 天气卡片：GUI 刚起来时只要现成快照（含缓存），点卡片才真的刷新。
            // 两条都只是把请求转给天气服务 —— 网络请求在它自己的任务里，
            // 这里和 LVGL 主循环都不会被拖住。
            if value.get("type").and_then(|item| item.as_str()) == Some("weather_request") {
                self.weather.publish();
                return;
            }
            if value.get("type").and_then(|item| item.as_str()) == Some("weather_refresh") {
                self.weather.refresh_soon();
                return;
            }

            // 性能监控页刚进来：只要现成快照，采样节奏仍由监控任务控制
            if value.get("type").and_then(|item| item.as_str()) == Some("performance_request") {
                self.performance.publish();
                return;
            }

            // GUI 刚起来（或重连上）时向核心要一次最近的记录：两端的聊天面板
            // 因此从同一份历史开始，而不是各自从空白开始。
            if value.get("type").and_then(|item| item.as_str()) == Some("chat_history_request") {
                self.replay_chat_history().await;
                return;
            }

            // 文字聊天：GUI 输入框 / web 控制台的文字没有对应的声音，先本地合成
            // 再当成一次说话发出去。必须在这里拦下来——否则它会被当成裸文本
            // 直发服务器，这正是之前一直失败的原因。
            if value.get("type").and_then(|item| item.as_str()) == Some("chat_text") {
                let result = match value.get("text").and_then(|item| item.as_str()) {
                    Some(text) => self.send_text_as_speech(text.to_string()).await,
                    None => Err("消息缺少 text 字段".to_string()),
                };
                let _ = self.gui_bridge.send_message(&serde_json::json!({
                    "type": "command_ack", "command": "chat_text",
                    "request_id": value.get("request_id").and_then(|id| id.as_u64()).unwrap_or(0),
                    "ok": result.is_ok(), "text": result.err().unwrap_or_default(),
                }).to_string()).await;
                return;
            }
        }
        if let Err(e) = self.net_tx.send(NetCommand::SendText(msg)).await {
            log::error!("Failed to send text to NetLink: {}", e);
            let _ = self
                .gui_bridge
                .send_message(&serde_json::json!({
                    "type": "toast",
                    "text": "网络通道不可用，消息未发送",
                }).to_string())
                .await;
        }
    }
}

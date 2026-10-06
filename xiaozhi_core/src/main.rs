mod activation;
mod audio;
mod audio_bridge;
mod config;
mod controller;
mod gui_bridge;
mod logbuf;
mod mcp_gateway;
mod net_link;
mod performance;
mod protocol;
mod state_machine;
mod skill_web;
mod chat;
mod smarthome;
mod timers;
mod tts;
mod user_data;
mod weather;

use audio_bridge::{AudioBridge, AudioEvent};
use config::Config;
use controller::{CoreCommand, CoreController};
use gui_bridge::{GuiBridge, GuiEvent};
use tts::TtsService;

use mac_address::get_mac_address;
use net_link::{NetCommand, NetEvent, NetLink};
use std::sync::Arc;
use std::net::UdpSocket as StdUdpSocket;
use tokio::signal;
use tokio::sync::mpsc;
use uuid::Uuid;
use crate::mcp_gateway::init_mcp_gateway;
use crate::mcp_gateway::skill::SkillManager;
use crate::chat::ChatHub;

fn device_ip() -> Option<String> {
    /* Connecting an unbound UDP socket lets the kernel select the address
     * used by the default route without sending a packet. */
    let socket = StdUdpSocket::bind("0.0.0.0:0").ok()?;
    socket.connect("1.1.1.1:80").ok()?;
    Some(socket.local_addr().ok()?.ip().to_string())
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    /* 初始化日志：终端/文件照旧（启动脚本把标准输出重定向到
     * `/var/log/qzdesk.log`），另外留一份最近几百行在内存里，给网页
     * `/api/logs` 与设备「日志」页看——排查不必再连 ssh 翻文件。 */
    {
        let env = env_logger::Env::default().default_filter_or("info");
        let inner = env_logger::Builder::from_env(env)
            .format(|buf, record| {
                use std::io::Write;
                writeln!(
                    buf,
                    "[{} {:<5}] {}",
                    buf.timestamp(),
                    record.level(),
                    record.args()
                )
            })
            .build();
        /* 级别交给 env_logger 自己算（`RUST_LOG=info,qzdesk=debug` 这种指令也认），
         * 再写进 log facade——不写的话每一条都会被 facade 在入口挡掉，慢日志、
         * 网页日志页都会是空的。 */
        let level = inner.filter();
        logbuf::install(Box::new(inner), level);
    }

    match device_ip() {
        Some(ip) => log::info!("QZdesk device IP: {}", ip),
        None => log::warn!("QZdesk device IP: unavailable"),
    }

    // 加载配置（若不存在则根据编译时默认生成并持久化）
    let mut config = Config::load_or_create()?;

    // 立即进行严格校验 (Fail Fast)
    if let Err(e) = config.validate() {
        log::error!("🛑 程序启动失败：{}", e);
        std::process::exit(1);
    }

    // 设备id和客户端id的处理
    let mut config_dirty = false;
    if config.device_id == "unknown-device" {
        config.device_id = match get_mac_address() {
            Ok(Some(mac)) => mac.to_string().to_lowercase(),
            _ => Uuid::new_v4().to_string(),
        };
        config_dirty = true;
    }

    if config.client_id == "unknown-client" {
        config.client_id = Uuid::new_v4().to_string();
        log::info!("Generated new Client ID: {}", config.client_id);
        config_dirty = true;
    }

    if config_dirty {
        if let Err(e) = config.save() {
            log::error!("Failed to persist updated config: {}", e);
        }
    }

    // 初始化 MCP Gateway 工具箱
    let mcp_configs = if config.mcp.enabled {
        log::info!("MCP Gateway is enabled. Loaded {} tools from configuration.", config.mcp.tools.len());
        config.mcp.tools.clone()
    } else {
        log::info!("MCP Gateway is disabled.");
        vec![]
    };

    // Share one SkillManager between MCP and the web console so role changes
    // and enable/disable settings are persisted and immediately visible to
    // both sides.
    let skill_manager = SkillManager::new();
    let mut mcp_server = init_mcp_gateway(mcp_configs, skill_manager.clone());

    // 智能家居中枢：核心自己接 MQTT，把「设备控制」从本机扩到局域网设备。
    // 它同时是 MCP 工具（云端 AI）与网页控制台的数据源，先建好再挂工具。
    let smarthome = smarthome::SmartHomeHub::start(config.smarthome_config());
    mcp_server.register_tool(Box::new(smarthome::tools::DeviceListTool::new(
        smarthome.clone(),
    )));
    mcp_server.register_tool(Box::new(smarthome::tools::DeviceControlTool::new(
        smarthome.clone(),
    )));
    /* 提醒与番茄钟：语音（MCP 工具）、设备界面、网页控制台读写的都是这一份，
     * 落盘文件与 `set_timer.py` / `pomodoro.py` 共用（见 timers.rs 的模块说明）。
     *
     * 注册在配置里的同名工具之后：老配置（或设备上的 xiaozhi_config.json）里
     * 仍写着 `set_timer`/`pomodoro` 的子进程脚本，这里用内置实现顶掉它，
     * 否则模型写的还是那个"界面从来不读"的文件。 */
    let timer_store = timers::TimerStore::load();
    log::info!(
        "提醒 {} 条，番茄钟{}（数据目录 {}）",
        timer_store.reminders().len(),
        if timer_store.pomodoro().active { "进行中" } else { "未运行" },
        timer_store.dir().display()
    );
    mcp_server.register_tool(Box::new(mcp_gateway::timer_tools::SetTimerTool::new(
        timer_store.clone(),
    )));
    mcp_server.register_tool(Box::new(mcp_gateway::timer_tools::PomodoroTool::new(
        timer_store.clone(),
    )));
    let mcp_server = Arc::new(mcp_server);

    // 创建通道，用于组件间通信
    // 事件通道
    let (tx_net_event, mut rx_net_event) = mpsc::channel::<NetEvent>(100);

    // 命令通道
    let (tx_net_cmd, rx_net_cmd) = mpsc::channel::<NetCommand>(100);

    // 交给核心自己处理的指令（目前是「把这段文字当一次说话」）。web 控制台与
    // GUI 都从这里进来，两边共用同一套合成 / 分包 / 节流逻辑。
    let (tx_core_cmd, mut rx_core_cmd) = mpsc::channel::<CoreCommand>(16);

    /* 聊天记录落盘：重启设备后接着上一次的对话，而不是从空白开始。
     * 记录本身仍是"只留一份"——界面与网页只是这份记录的显示器。 */
    let chat_hub = ChatHub::load();
    let web_skill_manager = Arc::new(skill_manager);
    let web_net_tx = tx_net_cmd.clone();
    let web_chat_hub = chat_hub.clone();
    let web_core_cmd = tx_core_cmd.clone();
    let web_smarthome = smarthome.clone();

    // 音频进程通道
    let (tx_audio_event, mut rx_audio_event) = mpsc::channel::<AudioEvent>(100);

    // GUI进程通道
    let (tx_gui_event, mut rx_gui_event) = mpsc::channel::<GuiEvent>(100);

    // 启动GUI桥，与GUI进程通信，优先启动，用于播报激活状态或者激活码
    let gui_bridge = Arc::new(GuiBridge::new(&config, tx_gui_event).await?);
    // clone一份，用于异步任务，还要用原始的gui_bridge在主循环中发送消息
    let gui_bridge_clone = gui_bridge.clone();
    tokio::spawn(async move {
        if let Err(e) = gui_bridge_clone.run().await {
            log::error!("GuiBridge error: {}", e);
        }
    });

    /* 提醒与番茄钟的推进放在核心：界面不必停在那两页、甚至不必开着。到点写进
     * 聊天记录（web 走 SSE、GUI 走 UDP），两边同时看到，而不是只在某个页面上
     * 闪一下——用户翻到别的页面照样收得到。 */
    {
        let store = timer_store.clone();
        let hub = chat_hub.clone();
        let bridge = gui_bridge.clone();
        tokio::spawn(async move {
            let mut ticker = tokio::time::interval(std::time::Duration::from_secs(1));
            // 掉拍（休眠、卡顿）后按真实时间继续，不补一串空转
            ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
            loop {
                ticker.tick().await;
                for event in store.tick(timers::unix_now()) {
                    log::info!("{}", event.text());
                    if let Some(message) = hub.push(chat::ChatRole::System, event.text()) {
                        if let Err(error) = bridge.send_message(&message).await {
                            log::warn!("提醒推送界面失败: {}", error);
                        }
                    }
                }
            }
        });
    }

    // 天气：核心自己去拉 Open-Meteo（免费、无需 Key），缓存 + 定时刷新。
    // 它把同一份快照同时推给 GUI（UDP）和网页控制台（HTTP），网络请求全在这个
    // 后台任务里，界面与 AI 对话都不会被它拖住。
    let weather = weather::WeatherService::start(config.weather_config(), gui_bridge.clone());
    if weather.snapshot().city.is_empty() {
        log::info!("天气卡片已启动，尚未取得位置信息");
    } else {
        log::info!("天气卡片已启动：{}", weather.snapshot().city);
    }

    // 性能监控：每 2 秒采一次 /proc 与 statvfs，设备页与网页读同一份。
    // GUI 退出后网页仍可查看，所以它独立于任何页面运行（采样本身只读文件）。
    let performance = performance::PerformanceService::start(
        performance::MonitorConfig {
            audio_enabled: std::env::var("QZDESK_AUDIO_DISABLED").is_err(),
        },
        gui_bridge.clone(),
    );

    // 网页控制台需要天气与性能句柄（/api/weather、/api/performance）：
    // 所以放在两个服务之后启动。
    let web_weather = weather.clone();
    let web_performance = performance.clone();
    let web_timer_store = timer_store.clone();
    tokio::spawn(async move {
        if let Err(error) = skill_web::run(
            web_skill_manager,
            web_net_tx,
            web_chat_hub,
            web_core_cmd,
            web_smarthome,
            web_weather,
            web_performance,
            web_timer_store,
        )
        .await
        {
            log::warn!("Skill web manager stopped: {}", error);
        }
    });
    if let Some(ip) = device_ip() {
        let port = std::env::var("QZDESK_SKILL_WEB_PORT")
            .or_else(|_| std::env::var("XIAOZHI_SKILL_WEB_PORT"))
            .unwrap_or_else(|_| "8080".to_string());
        log::info!("QZdesk Skill 管理页: http://{}:{}", ip, port);
    }

    // 在启动 NetLink 前检查激活
    loop {
        match activation::check_device_activation(&config).await {
            activation::ActivationResult::Activated => {
                log::info!("Device is activated. Starting WebSocket...");
                if let Err(e) = gui_bridge
                    .send_message(r#"{"type":"toast", "text":"设备已激活"}"#)
                    .await
                {
                    log::error!("Failed to send GUI message: {}", e);
                }
                break; // 跳出循环，继续下面的 NetLink 启动
            }
            activation::ActivationResult::NeedActivation(code) => {
                log::info!("Device NOT activated. Code: {}", code);

                // GUI 显示验证码
                let gui_msg = format!(r#"{{"type":"activation", "code":"{}"}}"#, code);
                if let Err(e) = gui_bridge.send_message(&gui_msg).await {
                    log::error!("Failed to send GUI message: {}", e);
                }

                // TTS 播报
                // 如果支持的话，可以设置在这里
                // audio_bridge.speak_text(format!("请在手机输入验证码 {}", code)).await;

                // 等待几秒再轮询
                tokio::time::sleep(tokio::time::Duration::from_secs(5)).await;
            }
            activation::ActivationResult::Error(e) => {
                log::error!("Activation check error: {}. Retrying in 5s...", e);
                /* Keep the GUI informed while activation/network probing is
                 * retrying; otherwise it can remain on its initial
                 * "Starting ..." label indefinitely. */
                if let Err(send_error) = gui_bridge.send_message(r#"{"state": 4}"#).await {
                    log::debug!("Failed to send activation error state to GUI: {}", send_error);
                }
                tokio::time::sleep(tokio::time::Duration::from_secs(5)).await;
            }
        }
    }

    // 启动网络链接，与小智服务器通信
    let net_link = NetLink::new(config.clone(), tx_net_event, rx_net_cmd, mcp_server);
    tokio::spawn(async move {
        net_link.run().await;
    });

    // 启动音频桥（内置音频系统，无需外部进程）
    let audio_bridge = Arc::new(AudioBridge::start(&config, tx_audio_event)?);

    // 本地文字转语音。模型在第一次用到时才加载，没配模型或者本构建没链接
    // sherpa-onnx 时会自报不可用，不影响语音对话。
    let tts = Arc::new(TtsService::new(config.tts_config()));
    if tts.available() {
        log::info!("文字转语音已就绪：GUI / web 的文字聊天会先合成语音再上行");
    } else {
        log::warn!("文字转语音不可用：GUI / web 的文字聊天将无法发送");
    }

    // 初始化控制器。聊天记录由它统一写入：GUI 与 web 看到的是同一份；
    // 天气句柄也交给它，GUI 的「要快照 / 点卡片刷新」两条请求由这里转给天气服务。
    let mut controller = CoreController::new(
        config.clone(),
        tx_net_cmd,
        audio_bridge,
        gui_bridge,
        tts,
        chat_hub,
        weather,
        performance,
    );

    log::info!("QZdesk Core Started. Entering Event Loop...");

    loop {
        tokio::select! {
            _ = signal::ctrl_c() => {
                log::info!("Received Ctrl+C, shutting down...");
                break;
            }
            Some(event) = rx_net_event.recv() => controller.handle_net_event(event).await,
            Some(event) = rx_audio_event.recv() => controller.handle_audio_event(event).await,
            Some(event) = rx_gui_event.recv() => controller.handle_gui_event(event).await,
            Some(command) = rx_core_cmd.recv() => {
                match command {
                    CoreCommand::TextAsSpeech(text) => {
                        controller.send_text_as_speech(text).await;
                    }
                }
            }
        }
    }
    Ok(())
}

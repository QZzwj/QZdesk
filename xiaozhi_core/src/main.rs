mod activation;
mod archive;
mod audio;
mod audio_bridge;
mod bridge;
mod chat;
mod companion;
mod computer;
mod computer_pairing;
mod config;
mod controller;
mod device_sync;
mod diagnostics;
mod gui_bridge;
mod logbuf;
mod mcp_gateway;
mod net_link;
mod performance;
mod protocol;
mod scenes;
mod skill_web;
mod smarthome;
mod state_machine;
mod timers;
mod tts;
mod user_data;
mod weather;

use audio_bridge::{AudioBridge, AudioEvent};
use config::Config;
use controller::{CoreCommand, CoreController};
use gui_bridge::{GuiBridge, GuiEvent};
use tts::TtsService;

use crate::chat::ChatHub;
use crate::mcp_gateway::init_mcp_gateway;
use crate::mcp_gateway::skill::SkillManager;
use mac_address::get_mac_address;
use net_link::{NetCommand, NetEvent, NetLink};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use tokio::signal;
use tokio::sync::mpsc;
use uuid::Uuid;

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

    match bridge::device_ip() {
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
        log::info!(
            "MCP Gateway is enabled. Loaded {} tools from configuration.",
            config.mcp.tools.len()
        );
        config.mcp.tools.clone()
    } else {
        log::info!("MCP Gateway is disabled.");
        vec![]
    };

    // Share one SkillManager between MCP and the web console so role changes
    // and enable/disable settings are persisted and immediately visible to
    // both sides.
    let skill_manager = Arc::new(SkillManager::new());
    let mut mcp_server = init_mcp_gateway(mcp_configs, (*skill_manager).clone());

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
        if timer_store.pomodoro().active {
            "进行中"
        } else {
            "未运行"
        },
        timer_store.dir().display()
    );
    mcp_server.register_tool(Box::new(mcp_gateway::timer_tools::SetTimerTool::new(
        timer_store.clone(),
    )));
    mcp_server.register_tool(Box::new(mcp_gateway::timer_tools::PomodoroTool::new(
        timer_store.clone(),
    )));
    let scene_service = scenes::SceneService::load(
        skill_manager.clone(),
        smarthome.clone(),
        timer_store.clone(),
    );
    mcp_server.register_tool(Box::new(scenes::SceneListTool::new(scene_service.clone())));
    mcp_server.register_tool(Box::new(scenes::SceneRunTool::new(scene_service.clone())));
    let companion_store = companion::CompanionStore::load().map_err(anyhow::Error::msg)?;
    for care in [false, true] {
        mcp_server.register_tool(Box::new(companion::CompanionTool {
            store: companion_store.clone(),
            care,
        }));
    }
    mcp_server.register_tool(Box::new(companion::CompanionMemoryTool {
        store: companion_store.clone(),
    }));
    mcp_server.register_tool(Box::new(computer::PairedComputer));
    mcp_server.register_tool(Box::new(computer::ComputerAction));
    mcp_server.register_tool(Box::new(computer::ComputerFiles));
    mcp_server.register_tool(Box::new(computer::ComputerShell));
    mcp_server.register_tool(Box::new(computer::ComputerSystem));
    mcp_server.register_tool(Box::new(computer::ComputerWeb));
    mcp_server.register_tool(Box::new(computer::ComputerCapture));
    tokio::spawn(async {
        if let Err(error) = computer_pairing::discover().await {
            log::warn!("电脑配对发现服务不可用：{}", error);
        }
    });
    let mcp_server = Arc::new(mcp_server);

    // 创建通道，用于组件间通信
    // 事件通道
    let (tx_net_event, mut rx_net_event) = mpsc::channel::<NetEvent>(100);

    // 命令通道
    let (tx_net_cmd, rx_net_cmd) = mpsc::channel::<NetCommand>(100);

    // 交给核心自己处理的指令（目前是「把这段文字当一次说话」）。web 控制台与
    // GUI 都从这里进来，两边共用同一套合成 / 分包 / 节流逻辑。
    let (tx_core_cmd, mut rx_core_cmd) = mpsc::channel::<CoreCommand>(16);
    /* The web listener starts before activation finishes, while the command
     * receiver below cannot run until activation and all audio services are
     * ready. Publish that lifecycle boundary so HTTP requests fail clearly
     * instead of sitting in an unconsumed queue until they time out. */
    let core_ready = Arc::new(AtomicBool::new(false));

    /* 聊天记录落盘：重启设备后接着上一次的对话，而不是从空白开始。
     * 记录本身仍是"只留一份"——界面与网页只是这份记录的显示器。 */
    let chat_hub = ChatHub::load();
    let web_skill_manager = skill_manager;
    let web_net_tx = tx_net_cmd.clone();
    let web_chat_hub = chat_hub.clone();
    let web_core_cmd = tx_core_cmd.clone();
    let web_core_ready = core_ready.clone();
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
                        let _ = bridge.send_message(&message).await;
                        let message = serde_json::json!({"type":"toast", "role":"timer", "text":event.text()}).to_string();
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
            audio_enabled: !audio::audio_disabled(),
        },
        gui_bridge.clone(),
    );
    // 网页控制台需要天气与性能句柄（/api/weather、/api/performance）：
    // 所以放在两个服务之后启动。
    let web_weather = weather.clone();
    let web_performance = performance.clone();
    let web_timer_store = timer_store.clone();
    let web_companion = companion_store.clone();
    let diagnostics = Arc::new(diagnostics::DiagnosticService::new(
        web_skill_manager.clone(),
        web_smarthome.clone(),
        web_weather.clone(),
        web_performance.clone(),
        web_timer_store.clone(),
    ));
    let device_sync =
        device_sync::DeviceSyncService::start(web_skill_manager.clone(), web_core_cmd.clone());
    let scene_scheduler = scene_service.clone();
    tokio::spawn(async move {
        let mut ticker = tokio::time::interval(std::time::Duration::from_secs(1));
        ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        loop {
            ticker.tick().await;
            scene_scheduler.tick_schedules(timers::unix_now()).await;
        }
    });
    {
        let scene_service = scene_service.clone();
        let mut state_changes = web_smarthome.subscribe_state_changes();
        tokio::spawn(async move {
            loop {
                match state_changes.recv().await {
                    Ok(event) => scene_service.handle_state_change(&event).await,
                    Err(tokio::sync::broadcast::error::RecvError::Lagged(skipped)) => {
                        log::warn!("场景状态订阅落后，丢弃 {} 个 MQTT 状态事件", skipped);
                    }
                    Err(tokio::sync::broadcast::error::RecvError::Closed) => break,
                }
            }
        });
    }

    tokio::spawn(async move {
        if let Err(error) = skill_web::run(
            web_skill_manager,
            web_net_tx,
            web_chat_hub,
            web_core_cmd,
            web_core_ready,
            web_smarthome,
            web_weather,
            web_performance,
            web_timer_store,
            web_companion,
            diagnostics,
            scene_service,
            device_sync,
        )
        .await
        {
            log::warn!("Skill web manager stopped: {}", error);
        }
    });
    // 手机输入桥：GUI 一上来（或重启后）就该有扫码地址，不必等用户点开浮层
    // 再问一次；换网后界面重新问即可（见 `handle_gui_event` 的 bridge_request）。
    match bridge::web_url() {
        Some(url) => {
            log::info!("QZdesk Skill 管理页: {}", url);
            let message = serde_json::json!({"type":"bridge", "ok":true, "url":url}).to_string();
            if let Err(error) = gui_bridge.send_message(&message).await {
                log::warn!("手机输入桥地址推送界面失败: {}", error);
            }
        }
        None => log::warn!("QZdesk Skill 管理页: 无局域网地址，手机输入桥不可用"),
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
                    log::debug!(
                        "Failed to send activation error state to GUI: {}",
                        send_error
                    );
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

    // 唤醒词进程的 PCM 旁路：设了 QZDESK_KWS_SOCKET 才开（见 audio/kws_tap.rs）
    crate::audio::kws_tap::init();

    // 启动音频桥（内置音频系统，无需外部进程）
    let audio_bridge = Arc::new(AudioBridge::start(&config, tx_audio_event)?);

    // 本地文字转语音只用于主动问候与存在检测。聊天输入始终直接发文字，
    // 失败时不会改走 TTS；模型在第一次用到时才加载。
    let tts = Arc::new(TtsService::new(config.tts_config()));
    if tts.available() {
        log::info!("文字直发已启用；本地文字转语音仅用于主动问候");
    } else {
        log::warn!("本地文字转语音不可用：主动问候将跳过，文字聊天不受影响");
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
    let mut speech_poll = tokio::time::interval(std::time::Duration::from_millis(50));
    speech_poll.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut care_poll = tokio::time::interval(std::time::Duration::from_secs(60));
    care_poll.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    core_ready.store(true, Ordering::Release);

    loop {
        tokio::select! {
            _ = signal::ctrl_c() => {
                log::info!("Received Ctrl+C, shutting down...");
                controller.cancel_speech().await;
                break;
            }
            _ = speech_poll.tick() => controller.poll_speech().await,
            _ = care_poll.tick() => {
                let now = timers::unix_now();
                let due = timers::local_hour(now).map(|hour| companion_store.care_due(now, hour)).unwrap_or(false);
                if due {
                    if !controller.proactive_care().await {
                        log::info!("主动关心本轮跳过：对话忙碌或语音未就绪");
                    } else if let Err(error) = companion_store.mark_care_sent(now) {
                        log::warn!("主动关心调度保存失败：{}", error);
                    }
                }
            },
            Some(event) = rx_net_event.recv() => controller.handle_net_event(event).await,
            Some(event) = rx_audio_event.recv() => controller.handle_audio_event(event).await,
            Some(event) = rx_gui_event.recv() => controller.handle_gui_event(event).await,
            Some(command) = rx_core_cmd.recv() => {
                match command {
                    CoreCommand::TextTurn(text, reply) => {
                        let result = controller.send_text_turn(text).await;
                        let _ = reply.send(result);
                    }
                    CoreCommand::AbortTurn(reply) => {
                        let result = controller.abort_turn().await;
                        let _ = reply.send(result);
                    }
                    CoreCommand::SkillsSaved => controller.mark_skills_pending(),
                    CoreCommand::Status(reply) => { let _ = reply.send(controller.capabilities()); }
                }
            }
        }
    }
    Ok(())
}

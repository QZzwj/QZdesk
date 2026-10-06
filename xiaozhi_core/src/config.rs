use anyhow::Context;
use serde::{Deserialize, Serialize};
use std::{borrow::Cow, fs, path::Path};
use uuid::Uuid;
use crate::mcp_gateway::ExternalToolConfig;

const CONFIG_FILE_NAME: &str = "xiaozhi_config.json";

/* Serde defaults for the text-to-speech block: they take their value from
 * config.toml (through build.rs) so a json file written by an older build — one
 * that has no `tts_*` keys yet — still loads with the intended settings. */
fn default_tts_enabled() -> bool {
    env!("TTS_ENABLED").parse().unwrap_or(true)
}

fn default_tts_app_id() -> String {
    env!("TTS_APP_ID").to_string()
}

fn default_tts_api_key() -> String {
    env!("TTS_API_KEY").to_string()
}

fn default_tts_api_secret() -> String {
    env!("TTS_API_SECRET").to_string()
}

fn default_tts_voice() -> String {
    env!("TTS_VOICE").to_string()
}

fn default_tts_speed() -> i32 {
    env!("TTS_SPEED").parse().unwrap_or(50)
}

fn default_tts_volume() -> i32 {
    env!("TTS_VOLUME").parse().unwrap_or(50)
}

fn default_tts_pitch() -> i32 {
    env!("TTS_PITCH").parse().unwrap_or(50)
}

fn default_tts_sample_rate() -> u32 {
    env!("TTS_SAMPLE_RATE").parse().unwrap_or(16_000)
}

fn default_tts_timeout_secs() -> u64 {
    env!("TTS_TIMEOUT_SECS").parse().unwrap_or(15)
}

/// 网络下发流的编码格式（源格式）
#[derive(Debug, Deserialize, Serialize, Clone, Copy, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum AudioStreamFormat {
    Opus,
    Mp3,
    Pcm,
}

impl AudioStreamFormat {
    pub fn as_str(&self) -> &str {
        match self {
            Self::Opus => "opus",
            Self::Mp3 => "mp3",
            Self::Pcm => "pcm",
        }
    }
}

impl std::fmt::Display for AudioStreamFormat {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct McpConfig {
    pub enabled: bool,
    #[serde(default)]
    pub tools: Vec<ExternalToolConfig>,
}

/// 智能家居中枢：核心作为 MQTT 客户端，把局域网设备并进「设备控制」页。
///
/// 只支持 MQTT 3.1.1 的 QoS0 子集，够用且不给固件引依赖树：zigbee2mqtt 网关
/// 本身就是这个用法，其它设备用 `devices` 表手写主题即可。
#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct SmartHomeConfig {
    #[serde(default)]
    pub enabled: bool,
    /// 例 `mqtt://192.168.1.10:1883`；留空表示还没配。
    #[serde(default)]
    pub broker: String,
    #[serde(default)]
    pub username: String,
    #[serde(default)]
    pub password: String,
    /// zigbee2mqtt 的 base topic；留空则不订阅 Z2M 设备树。
    #[serde(default = "default_zigbee_topic")]
    pub zigbee2mqtt_topic: String,
    /// 手写的通用设备（非 Z2M）。
    #[serde(default)]
    pub devices: Vec<SmartHomeDeviceConfig>,
}

impl Default for SmartHomeConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            broker: String::new(),
            username: String::new(),
            password: String::new(),
            zigbee2mqtt_topic: default_zigbee_topic(),
            devices: Vec::new(),
        }
    }
}

/// 一台手写声明的局域网设备：状态从 `state_topic` 来，指令发到 `command_topic`。
#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct SmartHomeDeviceConfig {
    /// 唯一标识（设备表与接口里用它寻址）。
    pub id: String,
    pub name: String,
    /// `switch` 或 `light`（light 额外支持亮度）。
    #[serde(default = "default_device_kind")]
    pub kind: String,
    pub state_topic: String,
    pub command_topic: String,
    #[serde(default = "default_command_on")]
    pub command_on: String,
    #[serde(default = "default_command_off")]
    pub command_off: String,
}

fn default_zigbee_topic() -> String {
    "zigbee2mqtt".to_string()
}

fn default_device_kind() -> String {
    "switch".to_string()
}

fn default_command_on() -> String {
    "ON".to_string()
}

fn default_command_off() -> String {
    "OFF".to_string()
}

/// 智能家居段的编译期默认值（config.toml → build.rs → 这里）。
fn default_smarthome() -> SmartHomeConfig {
    serde_json::from_str(env!("SMARTHOME_CONFIG_JSON")).unwrap_or_default()
}

/// 天气段的编译期默认值（config.toml → build.rs → 这里）。
fn default_weather() -> crate::weather::WeatherConfig {
    serde_json::from_str(env!("WEATHER_CONFIG_JSON")).unwrap_or_default()
}

#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct Config {
    // 音频设备配置
    pub capture_device: Cow<'static, str>,
    pub playback_device: Cow<'static, str>,
    pub stream_format: AudioStreamFormat,
    pub playback_sample_rate: u32,
    pub playback_channels: u32,
    pub playback_period_size: usize,

    // GUI进程配置
    pub gui_local_port: u16,
    pub gui_remote_port: u16,
    pub gui_local_ip: Cow<'static, str>,
    pub gui_remote_ip: Cow<'static, str>,
    pub gui_buffer_size: usize,

    // 网络配置（静态部分）
    pub ws_url: Cow<'static, str>,
    pub ota_url: Cow<'static, str>,
    pub ws_token: Cow<'static, str>,

    // 设备标识（动态部分，可在运行时修改）
    pub device_id: String,
    pub client_id: String,

    // Hello消息参数
    pub hello_format: Cow<'static, str>,
    pub hello_sample_rate: u32,
    pub hello_channels: u8,
    pub hello_frame_duration: u32,

    // 功能开关
    pub enable_tts_display: bool,

    // 文字转语音（讯飞在线语音合成 WebAPI）：GUI / web 的文字聊天先用它合成
    // 语音，再按麦克风同样的格式发给云端。语音输入不经过它。
    // 这几项都带 serde 默认值，这样老版本的 xiaozhi_config.json 仍能加载。
    #[serde(default = "default_tts_enabled")]
    pub tts_enabled: bool,
    #[serde(default = "default_tts_app_id")]
    pub tts_app_id: String,
    #[serde(default = "default_tts_api_key")]
    pub tts_api_key: String,
    #[serde(default = "default_tts_api_secret")]
    pub tts_api_secret: String,
    #[serde(default = "default_tts_voice")]
    pub tts_voice: String,
    #[serde(default = "default_tts_speed")]
    pub tts_speed: i32,
    #[serde(default = "default_tts_volume")]
    pub tts_volume: i32,
    #[serde(default = "default_tts_pitch")]
    pub tts_pitch: i32,
    #[serde(default = "default_tts_sample_rate")]
    pub tts_sample_rate: u32,
    #[serde(default = "default_tts_timeout_secs")]
    pub tts_timeout_secs: u64,

    // 智能家居中枢（MQTT）。同样带 serde 默认值：老版本的 xiaozhi_config.json
    // 里没有这一段，加载时用 config.toml 的编译期默认值顶上。
    #[serde(default = "default_smarthome")]
    pub smarthome: SmartHomeConfig,

    // 天气卡片（Open-Meteo）。同上：老配置文件没有这一段也能加载。
    #[serde(default = "default_weather")]
    pub weather: crate::weather::WeatherConfig,

    // MCP配置
    pub mcp: McpConfig,
}

impl Config {
    /// 返回配置文件路径
    fn config_path() -> &'static Path {
        Path::new(CONFIG_FILE_NAME)
    }

    /// 从编译时设置的环境变量创建配置
    /// 所有参数都在编译时从 config.toml 中读取
    fn default_from_build() -> Result<Self, &'static str> {
        // 解析编译时嵌入的 stream_format 字符串为枚举
        let stream_format = match env!("AUDIO_STREAM_FORMAT") {
            "opus" => AudioStreamFormat::Opus,
            "mp3" => AudioStreamFormat::Mp3,
            "pcm" => AudioStreamFormat::Pcm,
            _ => return Err("Invalid AUDIO_STREAM_FORMAT value"),
        };

        Ok(Self {
            // 音频设备配置
            capture_device: Cow::Borrowed(env!("AUDIO_CAPTURE_DEVICE")),
            playback_device: Cow::Borrowed(env!("AUDIO_PLAYBACK_DEVICE")),
            stream_format,
            playback_sample_rate: env!("AUDIO_PLAYBACK_SAMPLE_RATE")
                .parse()
                .map_err(|_| "Failed to parse AUDIO_PLAYBACK_SAMPLE_RATE")?,
            playback_channels: env!("AUDIO_PLAYBACK_CHANNELS")
                .parse()
                .map_err(|_| "Failed to parse AUDIO_PLAYBACK_CHANNELS")?,
            playback_period_size: env!("AUDIO_PLAYBACK_PERIOD_SIZE")
                .parse()
                .map_err(|_| "Failed to parse AUDIO_PLAYBACK_PERIOD_SIZE")?,

            // GUI进程配置
            gui_local_port: env!("GUI_LOCAL_PORT")
                .parse()
                .map_err(|_| "Failed to parse GUI_LOCAL_PORT")?,
            gui_remote_port: env!("GUI_REMOTE_PORT")
                .parse()
                .map_err(|_| "Failed to parse GUI_REMOTE_PORT")?,
            gui_local_ip: Cow::Borrowed(env!("GUI_LOCAL_IP")),
            gui_remote_ip: Cow::Borrowed(env!("GUI_REMOTE_IP")),
            gui_buffer_size: env!("GUI_BUFFER_SIZE")
                .parse()
                .map_err(|_| "Failed to parse GUI_BUFFER_SIZE")?,

            // 网络配置
            ws_url: Cow::Borrowed(env!("WS_URL")),
            ota_url: Cow::Borrowed(env!("OTA_URL")),
            ws_token: Cow::Borrowed(env!("WS_TOKEN")),

            // 设备标识初始化为config.toml中的值
            device_id: env!("DEVICE_ID").to_string(),
            client_id: env!("CLIENT_ID").to_string(),

            // Hello消息参数
            hello_format: Cow::Borrowed(env!("HELLO_FORMAT")),
            hello_sample_rate: env!("HELLO_SAMPLE_RATE")
                .parse()
                .map_err(|_| "Failed to parse HELLO_SAMPLE_RATE")?,
            hello_channels: env!("HELLO_CHANNELS")
                .parse()
                .map_err(|_| "Failed to parse HELLO_CHANNELS")?,
            hello_frame_duration: env!("HELLO_FRAME_DURATION")
                .parse()
                .map_err(|_| "Failed to parse HELLO_FRAME_DURATION")?,

            // 功能开关
            enable_tts_display: env!("ENABLE_TTS_DISPLAY")
                .parse()
                .map_err(|_| "Failed to parse ENABLE_TTS_DISPLAY")?,

            // 文字转语音
            tts_enabled: default_tts_enabled(),
            tts_app_id: default_tts_app_id(),
            tts_api_key: default_tts_api_key(),
            tts_api_secret: default_tts_api_secret(),
            tts_voice: default_tts_voice(),
            tts_speed: default_tts_speed(),
            tts_volume: default_tts_volume(),
            tts_pitch: default_tts_pitch(),
            tts_sample_rate: default_tts_sample_rate(),
            tts_timeout_secs: default_tts_timeout_secs(),

            // 智能家居中枢
            smarthome: default_smarthome(),

            // 天气卡片
            weather: default_weather(),

            // MCP配置：解析失败只丢外部工具，绝不让核心起不来。
            //
            // 设备上 /oem 里通常没有 xiaozhi_config.json，这一段烘焙值就是唯一的
            // 配置来源；以前它一旦解析不了（例如某个工具条目漏了 type 字段），
            // 核心会在启动时直接退出，而模拟器因为目录里有配置文件从来不走到
            // 这条路径，所以问题只在真机上暴露。Skill 工具是单独注册的，外部工具
            // 丢掉不影响技能链路。
            mcp: match serde_json::from_str(env!("MCP_CONFIG_JSON")) {
                Ok(config) => config,
                Err(error) => {
                    log::warn!("MCP 工具配置解析失败，本次不加载外部工具：{}", error);
                    McpConfig {
                        enabled: true,
                        tools: Vec::new(),
                    }
                }
            },
        })
    }

    /// 校验配置参数的合法性（Fail Fast）
    pub fn validate(&self) -> anyhow::Result<()> {
        // 校验音频格式支持情况
        match self.stream_format {
            AudioStreamFormat::Opus => {
                log::info!("音频流格式校验通过: opus");
            }
            AudioStreamFormat::Mp3 => {
                anyhow::bail!(
                    "配置错误：当前版本尚未支持 mp3 格式解码，请改回 opus"
                );
            }
            AudioStreamFormat::Pcm => {
                log::info!("音频流格式校验通过: pcm (原始PCM直通)");
            }
        }

        // 校验采样率是否在合理范围
        if self.hello_sample_rate < 8000 || self.hello_sample_rate > 48000 {
            anyhow::bail!(
                "配置错误：hello采样率 {}Hz 不合法 (支持 8000-48000)",
                self.hello_sample_rate
            );
        }

        if self.playback_sample_rate < 8000 || self.playback_sample_rate > 192000 {
            anyhow::bail!(
                "配置错误：播放采样率 {}Hz 不合法 (支持 8000-192000)",
                self.playback_sample_rate
            );
        }

        Ok(())
    }

    /// 讯飞文字转语音的运行时设置。
    ///
    /// 返回 `None` 表示文字转语音不可用（配置里关掉了，或者环境变量关掉了）。
    /// 账号密钥与发音人都可以用 `QZDESK_TTS_*` 覆盖：部署时不该把密钥烧进
    /// 二进制，同一份固件换个账号也不该重新编译。
    pub fn tts_config(&self) -> Option<crate::tts::TtsConfig> {
        fn env_or(name: &str, fallback: &str) -> String {
            std::env::var(name)
                .ok()
                .filter(|value| !value.trim().is_empty())
                .unwrap_or_else(|| fallback.to_string())
        }
        fn env_i32(name: &str, fallback: i32) -> i32 {
            std::env::var(name)
                .ok()
                .and_then(|value| value.parse::<i32>().ok())
                .unwrap_or(fallback)
        }

        let enabled = match std::env::var("QZDESK_TTS_ENABLE") {
            Ok(value) => value != "0" && !value.eq_ignore_ascii_case("false"),
            Err(_) => self.tts_enabled,
        };
        if !enabled {
            log::info!("文字转语音未启用（配置或环境变量关闭）");
            return None;
        }

        Some(crate::tts::TtsConfig {
            app_id: env_or("QZDESK_TTS_APP_ID", &self.tts_app_id),
            api_key: env_or("QZDESK_TTS_API_KEY", &self.tts_api_key),
            api_secret: env_or("QZDESK_TTS_API_SECRET", &self.tts_api_secret),
            voice: env_or("QZDESK_TTS_VOICE", &self.tts_voice),
            speed: env_i32("QZDESK_TTS_SPEED", self.tts_speed),
            volume: env_i32("QZDESK_TTS_VOLUME", self.tts_volume),
            pitch: env_i32("QZDESK_TTS_PITCH", self.tts_pitch),
            sample_rate: self.tts_sample_rate,
            timeout: std::time::Duration::from_secs(self.tts_timeout_secs.max(1)),
        })
    }

    /// 智能家居中枢的运行时设置。
    ///
    /// 与 TTS 同一套路：broker 与账号可以用 `QZDESK_SMARTHOME_*` 覆盖（现场换
    /// 路由器、换密码不必重新编译），也可以在网页控制台里改 —— 那条路径改的是
    /// `self.smarthome`，并由 `save()` 写回 xiaozhi_config.json。
    pub fn smarthome_config(&self) -> SmartHomeConfig {
        fn env_or(name: &str, fallback: &str) -> String {
            std::env::var(name)
                .ok()
                .filter(|value| !value.trim().is_empty())
                .unwrap_or_else(|| fallback.to_string())
        }

        let mut config = self.smarthome.clone();
        if let Ok(value) = std::env::var("QZDESK_SMARTHOME_ENABLE") {
            config.enabled = value != "0" && !value.eq_ignore_ascii_case("false");
        }
        config.broker = env_or("QZDESK_SMARTHOME_BROKER", &config.broker);
        config.username = env_or("QZDESK_SMARTHOME_USER", &config.username);
        config.password = env_or("QZDESK_SMARTHOME_PASSWORD", &config.password);
        config.zigbee2mqtt_topic =
            env_or("QZDESK_SMARTHOME_Z2M_TOPIC", &config.zigbee2mqtt_topic);
        config
    }

    /// 天气卡片的运行时设置。
    ///
    /// 与 TTS / 智能家居同一套路：换地方、换城市不必重新编译，改环境变量即可；
    /// 也可以直接改 `xiaozhi_config.json` 的 `weather` 段。环境变量优先读需求
    /// 里约定的 `WEATHER_*`，同时接受项目惯例的 `QZDESK_WEATHER_*`。
    pub fn weather_config(&self) -> crate::weather::WeatherConfig {
        fn env_first(names: &[&str]) -> Option<String> {
            names.iter().find_map(|name| {
                std::env::var(name)
                    .ok()
                    .filter(|value| !value.trim().is_empty())
            })
        }

        let mut config = self.weather.clone();
        if let Some(value) = env_first(&["WEATHER_ENABLE", "QZDESK_WEATHER_ENABLE"]) {
            config.enabled = value != "0" && !value.eq_ignore_ascii_case("false");
        }
        if let Some(value) = env_first(&["WEATHER_LATITUDE", "QZDESK_WEATHER_LATITUDE"]) {
            config.latitude = Some(crate::weather::coordinate(&value));
        }
        if let Some(value) = env_first(&["WEATHER_LONGITUDE", "QZDESK_WEATHER_LONGITUDE"]) {
            config.longitude = Some(crate::weather::coordinate(&value));
        }
        if let Some(value) = env_first(&["WEATHER_CITY", "QZDESK_WEATHER_CITY"]) {
            config.city = value;
        }
        if let Some(value) = env_first(&["WEATHER_PROXY", "QZDESK_WEATHER_PROXY"]) {
            config.proxy = Some(value);
        }
        config
    }

    /// 加载现有配置或使用默认值创建新文件
    pub fn load_or_create() -> anyhow::Result<Self> {
        let path = Self::config_path();
        if path.exists() {
            let content = fs::read_to_string(path)
                .with_context(|| format!("Failed to read {}", path.display()))?;
            let mut config: Config = serde_json::from_str(&content)
                .with_context(|| format!("Failed to parse {}", path.display()))?;

            if config.client_id.trim().is_empty() || config.client_id == "unknown-client" {
                config.client_id = Uuid::new_v4().to_string();
                config.save()?;
            }

            Ok(config)
        } else {
            let mut config = Self::default_from_build().map_err(anyhow::Error::msg)?;

            if config.client_id.trim().is_empty() || config.client_id == "unknown-client" {
                config.client_id = Uuid::new_v4().to_string();
            }

            config.save()?;
            Ok(config)
        }
    }

    /// 将当前配置写回磁盘
    pub fn save(&self) -> anyhow::Result<()> {
        let path = Self::config_path();
        let json = serde_json::to_string_pretty(self)?;
        fs::write(path, json).with_context(|| format!("Failed to write {}", path.display()))
    }
}

// 为 Config 实现 Default trait，使用编译时环境变量的默认值
impl Default for Config {
    fn default() -> Self {
        Self::default_from_build()
            .expect("Failed to create default Config from build-time environment variables")
    }
}

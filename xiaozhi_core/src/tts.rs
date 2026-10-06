//! 文字转语音：把 GUI / web 输入的文字变成云端接受的音频。
//!
//! 云端协议只接受**音频**作为用户输入，所以打字的聊天内容必须先变成声音。这里
//! 用讯飞的在线语音合成 WebAPI（`wss://tts-api.xfyun.cn/v2/tts`），拿到
//! 16k / 16bit / 单声道 PCM，再由 controller 按麦克风同样的 Opus 格式上行。
//!
//! 为什么是云端合成而不是本地引擎：设备是双核 A7，本地跑神经网络 TTS 既要背
//! 上百 MB 的模型又要可观的 CPU；而这条链路本来就在联网工作，合成放在云端更合算。
//!
//! 认证沿用讯飞 WebAPI 的方案（与他们的 ASR 同一个套路）：
//!   1. 把 `host: ...\ndate: ...\nGET /v2/tts HTTP/1.1` 用 HMAC-SHA256 签名；
//!   2. 签名再 base64，拼成 `api_key="..", algorithm="hmac-sha256", headers="..",
//!      signature=".."`；
//!   3. 整串再 base64 作为 `authorization` 查询参数，和 `date`、`host` 一起挂到 URL 上。

use anyhow::{anyhow, bail, Result};
use base64::engine::general_purpose::STANDARD as BASE64;
use base64::Engine as _;
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio_tungstenite::tungstenite::Message;

/// 讯飞在线语音合成的鉴权与合成参数。
#[derive(Debug, Clone)]
pub struct TtsConfig {
    pub app_id: String,
    pub api_key: String,
    pub api_secret: String,
    /// 发音人（vcn），例如 `x4_xiaoyan`。
    pub voice: String,
    /// 语速 / 音量 / 音调，取值 0-100，50 为默认。
    pub speed: i32,
    pub volume: i32,
    pub pitch: i32,
    /// 输出采样率，保持 16000（讯飞 raw PCM 的标准档位）。
    pub sample_rate: u32,
    /// 单次合成的整体超时。
    pub timeout: Duration,
}

impl TtsConfig {
    /// 配置是否可用；返回不能用的原因。
    pub fn problem(&self) -> Option<String> {
        if self.app_id.trim().is_empty() {
            return Some("缺少讯飞 APPID".to_string());
        }
        if self.api_key.trim().is_empty() || self.api_secret.trim().is_empty() {
            return Some("缺少讯飞 APIKey / APISecret".to_string());
        }
        if self.voice.trim().is_empty() {
            return Some("缺少发音人（vcn）".to_string());
        }
        None
    }
}

/// 与 `urlencode`（python: quote(s, safe='')）一致：除了 `A-Za-z0-9_.-~`
/// 一律百分号编码。查询串里的 base64 与 RFC1123 日期都靠它。
fn percent_encode(value: &str) -> String {
    let mut encoded = String::with_capacity(value.len());
    for byte in value.bytes() {
        match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                encoded.push(byte as char)
            }
            _ => encoded.push_str(&format!("%{:02X}", byte)),
        }
    }
    encoded
}

/// HMAC-SHA256（RFC 2104）。
///
/// 只用 `sha2` 这一个哈希实现，按标准构造拼出来，省掉 `hmac` 这个小依赖——
/// 这台机器拉 crates.io 很慢，而 `sha2` 已经在依赖图里且有本地缓存。构造是否
/// 正确由文件末尾的测试用 RFC 4231 官方向量固定住。
fn hmac_sha256(key: &[u8], message: &[u8]) -> [u8; 32] {
    const BLOCK: usize = 64; // SHA-256 的分组长度

    let mut key_block = [0u8; BLOCK];
    if key.len() > BLOCK {
        let digest = Sha256::digest(key);
        key_block[..digest.len()].copy_from_slice(&digest);
    } else {
        key_block[..key.len()].copy_from_slice(key);
    }

    let mut inner_pad = [0x36u8; BLOCK];
    let mut outer_pad = [0x5cu8; BLOCK];
    for index in 0..BLOCK {
        inner_pad[index] ^= key_block[index];
        outer_pad[index] ^= key_block[index];
    }

    let mut inner = Sha256::new();
    inner.update(inner_pad);
    inner.update(message);

    let mut outer = Sha256::new();
    outer.update(outer_pad);
    outer.update(inner.finalize());

    let mut mac = [0u8; 32];
    mac.copy_from_slice(&outer.finalize());
    mac
}

/// RFC 1123 格式的 GMT 时间，例如 `Mon, 05 Oct 2026 07:08:49 GMT`。
///
/// 讯飞要求这个串参与签名，且与请求时间相差不能太大，所以必须自己算准。
fn http_date(time: SystemTime) -> String {
    const WEEKDAYS: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
    const MONTHS: [&str; 12] = [
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    ];

    let seconds = time
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs() as i64)
        .unwrap_or(0);
    let days = seconds.div_euclid(86_400);
    let day_seconds = seconds.rem_euclid(86_400);
    let (year, month, day) = civil_from_days(days);
    // 1970-01-01 是周四，所以 +4 之后取模正好落在 WEEKDAYS 上。
    let weekday = WEEKDAYS[(days + 4).rem_euclid(7) as usize];

    format!(
        "{}, {:02} {} {} {:02}:{:02}:{:02} GMT",
        weekday,
        day,
        MONTHS[(month - 1) as usize],
        year,
        day_seconds / 3_600,
        (day_seconds % 3_600) / 60,
        day_seconds % 60
    )
}

/// 公历换算（Howard Hinnant 的 `civil_from_days`）：把 1970-01-01 起的天数
/// 换成 (年, 月, 日)，负数天也能正确处理。
fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let shifted = days + 719_468;
    let era = if shifted >= 0 { shifted } else { shifted - 146_096 } / 146_097;
    let day_of_era = shifted - era * 146_097; // [0, 146096]
    let year_of_era =
        (day_of_era - day_of_era / 1_460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let year = year_of_era + era * 400;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_prime = (5 * day_of_year + 2) / 153; // [0, 11]
    let day = (day_of_year - (153 * month_prime + 2) / 5 + 1) as u32; // [1, 31]
    let month = if month_prime < 10 {
        month_prime + 3
    } else {
        month_prime - 9
    } as u32;
    (if month <= 2 { year + 1 } else { year }, month, day)
}

/// 生成带鉴权参数的 wss 地址。
fn signed_url(config: &TtsConfig) -> Result<String> {
    const HOST: &str = "tts-api.xfyun.cn";
    const PATH: &str = "/v2/tts";

    let date = http_date(SystemTime::now());
    let origin = format!("host: {}\ndate: {}\nGET {} HTTP/1.1", HOST, date, PATH);
    let signature = BASE64.encode(hmac_sha256(config.api_secret.as_bytes(), origin.as_bytes()));

    let authorization = format!(
        "api_key=\"{}\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"{}\"",
        config.api_key, signature
    );

    Ok(format!(
        "wss://{}{}?authorization={}&date={}&host={}",
        HOST,
        PATH,
        percent_encode(&BASE64.encode(authorization.as_bytes())),
        percent_encode(&date),
        HOST
    ))
}

/// 一次合成的请求体：`status = 2` 表示这段文字在一帧里发完。
fn request_body(config: &TtsConfig, text: &str) -> String {
    serde_json::json!({
        "common": { "app_id": config.app_id },
        "business": {
            // raw = 未压缩 PCM（16bit 小端），交给 Opus 编码器正合适
            "aue": "raw",
            "auf": format!("audio/L16;rate={}", config.sample_rate),
            "vcn": config.voice,
            "tte": "UTF8",
            "speed": config.speed,
            "volume": config.volume,
            "pitch": config.pitch,
        },
        "data": {
            "status": 2,
            "text": BASE64.encode(text.as_bytes()),
        },
    })
    .to_string()
}

#[derive(Deserialize)]
struct TtsData {
    audio: Option<String>,
    status: Option<i64>,
}

#[derive(Deserialize)]
struct TtsFrame {
    code: i64,
    message: Option<String>,
    data: Option<TtsData>,
}

fn bytes_to_i16(bytes: &[u8]) -> Vec<i16> {
    bytes
        .chunks_exact(2)
        .map(|pair| i16::from_le_bytes([pair[0], pair[1]]))
        .collect()
}

/// 把合成结果写成 WAV。
///
/// 只为排查用：`QZDESK_TTS_DUMP=/tmp/tts.wav` 就能把送出去的这段音频留下来听，
/// 否则「文字到底被念成了什么」在设备上完全不可观测。
fn write_wav(path: &std::path::Path, pcm: &[i16], sample_rate: u32) -> std::io::Result<()> {
    use std::io::Write;

    let data_len = (pcm.len() * 2) as u32;
    let mut file = std::fs::File::create(path)?;
    file.write_all(b"RIFF")?;
    file.write_all(&(36 + data_len).to_le_bytes())?;
    file.write_all(b"WAVEfmt ")?;
    file.write_all(&16u32.to_le_bytes())?; // fmt 块长度
    file.write_all(&1u16.to_le_bytes())?; // PCM
    file.write_all(&1u16.to_le_bytes())?; // 单声道
    file.write_all(&sample_rate.to_le_bytes())?;
    file.write_all(&(sample_rate * 2).to_le_bytes())?; // 字节率
    file.write_all(&2u16.to_le_bytes())?; // 块对齐
    file.write_all(&16u16.to_le_bytes())?; // 位深
    file.write_all(b"data")?;
    file.write_all(&data_len.to_le_bytes())?;
    for sample in pcm {
        file.write_all(&sample.to_le_bytes())?;
    }
    Ok(())
}

/// 文字转语音服务。没有配置时自报不可用，让语音链路照常工作。
pub struct TtsService {
    config: Option<TtsConfig>,
}

impl TtsService {
    pub fn new(config: Option<TtsConfig>) -> Self {
        let config = config.filter(|candidate| match candidate.problem() {
            Some(problem) => {
                log::warn!("文字转语音不可用: {}", problem);
                false
            }
            None => true,
        });
        Self { config }
    }

    pub fn available(&self) -> bool {
        self.config.is_some()
    }

    /// 合成 `text`，返回单声道 16bit PCM 及其采样率。
    pub async fn synthesize(self: &Arc<Self>, text: &str) -> Result<(Vec<i16>, i32)> {
        let config = self
            .config
            .clone()
            .ok_or_else(|| anyhow!("文字转语音未启用（未配置讯飞账号信息）"))?;
        let text = text.trim().to_string();
        if text.is_empty() {
            bail!("没有可合成的文本");
        }
        // 讯飞单帧上限 8000 字节（UTF-8）；正常聊天远小于这个数，超了就直接截断，
        // 免得把一段长文拆帧的复杂度带进来。
        if text.len() > 8000 {
            bail!("文本过长（超过 8000 字节）");
        }

        let timeout = config.timeout;
        let result = match tokio::time::timeout(timeout, synthesize_once(&config, &text)).await {
            Ok(result) => result,
            Err(_) => bail!("语音合成超时（{}s）", timeout.as_secs()),
        }?;

        if let Ok(path) = std::env::var("QZDESK_TTS_DUMP") {
            if !path.trim().is_empty() {
                match write_wav(std::path::Path::new(&path), &result.0, result.1 as u32) {
                    Ok(()) => log::info!("TTS 调试音频已写入 {}", path),
                    Err(error) => log::warn!("写入 TTS 调试音频失败: {}", error),
                }
            }
        }

        log::info!(
            "语音合成完成: {} 字 -> {:.1}s ({} Hz)",
            text.chars().count(),
            result.0.len() as f32 / result.1.max(1) as f32,
            result.1
        );
        Ok(result)
    }
}

async fn synthesize_once(config: &TtsConfig, text: &str) -> Result<(Vec<i16>, i32)> {
    use futures_util::{SinkExt, StreamExt};

    let url = signed_url(config)?;
    let (mut socket, _) = tokio_tungstenite::connect_async(&url)
        .await
        .map_err(|error| anyhow!("连接讯飞合成服务失败: {}", error))?;

    socket
        .send(Message::Text(request_body(config, text).into()))
        .await
        .map_err(|error| anyhow!("发送合成请求失败: {}", error))?;

    let mut audio = Vec::new();
    let mut finished = false;

    while let Some(message) = socket.next().await {
        let message = message.map_err(|error| anyhow!("读取合成结果失败: {}", error))?;
        let payload = match message {
            Message::Text(text) => text.to_string(),
            Message::Binary(data) => String::from_utf8_lossy(&data).into_owned(),
            Message::Close(_) => break,
            _ => continue,
        };

        let frame: TtsFrame = serde_json::from_str(&payload)
            .map_err(|error| anyhow!("合成结果不是预期格式: {}", error))?;
        if frame.code != 0 {
            bail!(
                "讯飞合成失败({}): {}",
                frame.code,
                frame.message.unwrap_or_else(|| "未知错误".to_string())
            );
        }

        if let Some(data) = frame.data {
            if let Some(chunk) = data.audio {
                let decoded = BASE64
                    .decode(chunk)
                    .map_err(|error| anyhow!("音频分片解码失败: {}", error))?;
                audio.extend_from_slice(&decoded);
            }
            if data.status == Some(2) {
                finished = true;
                break;
            }
        }
    }

    if audio.is_empty() {
        bail!("合成结果为空");
    }
    if !finished {
        log::warn!("合成连接在收完结束帧之前关闭，仍按已收到的音频继续");
    }

    Ok((bytes_to_i16(&audio), config.sample_rate as i32))
}

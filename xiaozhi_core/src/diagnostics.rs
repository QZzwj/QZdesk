//! One-click device checks and a deliberately narrow diagnostic archive.
//!
//! The archive is assembled from bounded, structured snapshots. State files are
//! parsed only to prove that they are readable and valid; their contents are
//! never copied. Logs and command output pass through a redactor before export.

use crate::archive::StoredZip;
use crate::mcp_gateway::skill::SkillManager;
use crate::performance::PerformanceService;
use crate::smarthome::SmartHomeHub;
use crate::timers::{TimerStore, unix_now};
use crate::weather::WeatherService;
use serde::Serialize;
use serde_json::{Value, json};
use std::fs;
use std::io::Write;
use std::net::IpAddr;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const DNS_TIMEOUT: Duration = Duration::from_secs(4);
const MAX_LOG_LINES: usize = crate::logbuf::CAPACITY;

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CheckResult {
    pub id: String,
    pub label: String,
    pub status: String,
    pub detail: String,
    pub latency_ms: u64,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DiagnosticSummary {
    pub overall: String,
    pub ok: usize,
    pub warn: usize,
    pub fail: usize,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DiagnosticReport {
    pub generated_at: u64,
    pub duration_ms: u64,
    pub checks: Vec<CheckResult>,
    pub summary: DiagnosticSummary,
}

#[derive(Clone)]
struct Artifact {
    report: DiagnosticReport,
    performance: Value,
    errors: Value,
    logs: String,
}

#[derive(Clone)]
pub struct DiagnosticService {
    skills: Arc<SkillManager>,
    smarthome: Arc<SmartHomeHub>,
    weather: WeatherService,
    performance: PerformanceService,
    timers: TimerStore,
    last: Arc<Mutex<Option<Artifact>>>,
}

impl DiagnosticService {
    pub fn new(
        skills: Arc<SkillManager>,
        smarthome: Arc<SmartHomeHub>,
        weather: WeatherService,
        performance: PerformanceService,
        timers: TimerStore,
    ) -> Self {
        Self {
            skills,
            smarthome,
            weather,
            performance,
            timers,
            last: Arc::new(Mutex::new(None)),
        }
    }

    pub fn latest(&self) -> Value {
        let last = self.last.lock().unwrap_or_else(|error| error.into_inner());
        match last.as_ref() {
            Some(artifact) => json!({
                "ok": true,
                "ready": true,
                "report": artifact.report,
            }),
            None => json!({
                "ok": true,
                "ready": false,
                "report": null,
            }),
        }
    }

    pub async fn run(&self) -> DiagnosticReport {
        let started = Instant::now();
        let generated_at = unix_now();
        let mut checks = Vec::new();

        let (mic_status, mic_detail) = audio_check("capture");
        checks.push(check("microphone", "麦克风", mic_status, mic_detail));
        let (speaker_status, speaker_detail) = audio_check("playback");
        checks.push(check("speaker", "扬声器", speaker_status, speaker_detail));
        checks.push(self.network_check());
        checks.push(self.dns_check().await);

        let performance = self.performance.api();
        checks.push(self.mqtt_check());
        checks.push(self.weather_check());
        checks.push(self.disk_check(&performance));
        checks.push(self.skill_check());
        checks.push(self.state_check());

        let ok = checks.iter().filter(|item| item.status == "ok").count();
        let warn = checks.iter().filter(|item| item.status == "warn").count();
        let fail = checks.iter().filter(|item| item.status == "fail").count();
        let report = DiagnosticReport {
            generated_at,
            duration_ms: started.elapsed().as_millis() as u64,
            checks,
            summary: DiagnosticSummary {
                overall: if fail > 0 {
                    "fail"
                } else if warn > 0 {
                    "warn"
                } else {
                    "ok"
                }
                .to_string(),
                ok,
                warn,
                fail,
            },
        };

        let logs = crate::logbuf::tail(MAX_LOG_LINES)
            .into_iter()
            .map(|line| sanitize_log_line(&line))
            .collect::<Vec<_>>()
            .join("\n");
        let perf_errors = performance
            .get("errors")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default()
            .into_iter()
            .filter_map(|value| value.as_str().map(str::to_string))
            .collect::<Vec<_>>();
        let check_errors = report
            .checks
            .iter()
            .filter(|item| item.status != "ok")
            .map(|item| {
                json!({
                    "check": item.id,
                    "status": item.status,
                    "detail": item.detail,
                })
            })
            .collect::<Vec<_>>();
        let errors = json!({
            "checks": check_errors,
            "performance": perf_errors,
            "note": "诊断包只包含状态与错误摘要，不包含聊天、记忆正文或密钥。",
        });

        let artifact = Artifact {
            report: report.clone(),
            performance,
            errors,
            logs,
        };
        *self.last.lock().unwrap_or_else(|error| error.into_inner()) = Some(artifact);
        report
    }

    pub fn export(&self) -> Result<Vec<u8>, String> {
        let artifact = self
            .last
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .clone()
            .ok_or_else(|| "请先运行一次设备体检".to_string())?;
        let report = serde_json::to_vec_pretty(&artifact.report)
            .map_err(|error| format!("无法编码体检结果: {}", error))?;
        let performance = serde_json::to_vec_pretty(&artifact.performance)
            .map_err(|error| format!("无法编码性能快照: {}", error))?;
        let errors = serde_json::to_vec_pretty(&artifact.errors)
            .map_err(|error| format!("无法编码错误摘要: {}", error))?;
        let note = concat!(
            "QZdesk diagnostic bundle\n",
            "This archive intentionally excludes secrets, chat text, Skill bodies, ",
            "companion memory, and unredacted configuration values.\n",
            "Microphone and speaker checks only validate device/config presence; ",
            "they do not record or play audio.\n",
        );

        let mut zip = StoredZip::new();
        zip.add("result.json", &report)?;
        zip.add("logs.txt", artifact.logs.as_bytes())?;
        zip.add("performance.json", &performance)?;
        zip.add("errors.json", &errors)?;
        zip.add("README.txt", note.as_bytes())?;
        zip.finish()
    }

    fn network_check(&self) -> CheckResult {
        let started = Instant::now();
        match crate::bridge::device_ip() {
            Some(ip) => match ip.parse::<IpAddr>() {
                Ok(address) if is_private(address) => CheckResult {
                    id: "network".to_string(),
                    label: "局域网".to_string(),
                    status: "ok".to_string(),
                    detail: format!("本机地址 {}", ip),
                    latency_ms: started.elapsed().as_millis() as u64,
                },
                Ok(address) => CheckResult {
                    id: "network".to_string(),
                    label: "局域网".to_string(),
                    status: "warn".to_string(),
                    detail: format!("地址 {} 不是常见私网地址，请确认网络配置", address),
                    latency_ms: started.elapsed().as_millis() as u64,
                },
                Err(error) => CheckResult {
                    id: "network".to_string(),
                    label: "局域网".to_string(),
                    status: "fail".to_string(),
                    detail: format!("无法解析本机地址: {}", error),
                    latency_ms: started.elapsed().as_millis() as u64,
                },
            },
            None => CheckResult {
                id: "network".to_string(),
                label: "局域网".to_string(),
                status: "fail".to_string(),
                detail: "没有可用默认路由，请检查 Wi-Fi 或网线".to_string(),
                latency_ms: started.elapsed().as_millis() as u64,
            },
        }
    }

    async fn dns_check(&self) -> CheckResult {
        let started = Instant::now();
        match tokio::time::timeout(
            DNS_TIMEOUT,
            tokio::net::lookup_host(("api.open-meteo.com", 443)),
        )
        .await
        {
            Ok(Ok(mut addresses)) => match addresses.next() {
                Some(address) => CheckResult {
                    id: "dns".to_string(),
                    label: "DNS".to_string(),
                    status: "ok".to_string(),
                    detail: format!("解析成功，首选地址 {}", address),
                    latency_ms: started.elapsed().as_millis() as u64,
                },
                None => CheckResult {
                    id: "dns".to_string(),
                    label: "DNS".to_string(),
                    status: "fail".to_string(),
                    detail: "域名没有返回地址".to_string(),
                    latency_ms: started.elapsed().as_millis() as u64,
                },
            },
            Ok(Err(error)) => CheckResult {
                id: "dns".to_string(),
                label: "DNS".to_string(),
                status: "fail".to_string(),
                detail: format!("解析失败: {}", error),
                latency_ms: started.elapsed().as_millis() as u64,
            },
            Err(_) => CheckResult {
                id: "dns".to_string(),
                label: "DNS".to_string(),
                status: "fail".to_string(),
                detail: format!("解析超过 {} 秒", DNS_TIMEOUT.as_secs()),
                latency_ms: started.elapsed().as_millis() as u64,
            },
        }
    }

    fn mqtt_check(&self) -> CheckResult {
        let started = Instant::now();
        let status = self.smarthome.status_json();
        let enabled = status
            .get("enabled")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        let connected = status
            .get("connected")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        let error = status
            .get("error")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let (state, detail) = if !enabled {
            ("warn", "智能家居中枢未启用".to_string())
        } else if connected {
            (
                "ok",
                format!(
                    "已连接，订阅 {} 个主题",
                    status
                        .get("subscriptions")
                        .and_then(Value::as_u64)
                        .unwrap_or(0)
                ),
            )
        } else if error.is_empty() {
            ("fail", "已启用但尚未连接 MQTT".to_string())
        } else {
            ("fail", format!("已启用但未连接: {}", error))
        };
        check_with_latency("mqtt", "MQTT", state, detail, started)
    }

    fn weather_check(&self) -> CheckResult {
        let started = Instant::now();
        let weather = self.weather.snapshot();
        let (state, detail) = match weather.status.as_str() {
            "ok" | "loading" | "stale" => (
                "ok",
                if weather.city.is_empty() {
                    "已有天气数据，但城市名为空".to_string()
                } else {
                    format!("{} {}", weather.city, weather.text)
                },
            ),
            "no_location" => ("warn", "尚未设置天气位置".to_string()),
            _ if !weather.message.is_empty() => ("warn", weather.message),
            _ => ("warn", format!("天气状态 {}", weather.status)),
        };
        check_with_latency("weather", "天气", state, detail, started)
    }

    fn disk_check(&self, performance: &Value) -> CheckResult {
        let started = Instant::now();
        let usage = performance
            .pointer("/storage/usage_percent")
            .and_then(Value::as_f64);
        let available = performance
            .pointer("/storage/available_bytes")
            .and_then(Value::as_u64);
        let total = performance
            .pointer("/storage/total_bytes")
            .and_then(Value::as_u64);
        let storage_level = performance
            .pointer("/storage/level")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let writable = write_probe(&crate::user_data::dir());
        let storage_readable = storage_level != "unavailable" && total.unwrap_or(0) > 0;
        let (state, mut detail) = if !storage_readable {
            ("warn", "无法读取根分区容量，磁盘余量未验证".to_string())
        } else {
            match usage {
                Some(percent) if percent >= 95.0 => (
                    "fail",
                    format!("根分区已使用 {:.0}%，空间接近耗尽", percent),
                ),
                Some(percent) if percent >= 85.0 => (
                    "warn",
                    format!("根分区已使用 {:.0}%，建议清理日志或旧技能包", percent),
                ),
                Some(percent) => ("ok", format!("根分区已使用 {:.0}%", percent)),
                None => ("warn", "性能服务尚未提供磁盘快照".to_string()),
            }
        };
        if let Some(bytes) = available {
            if bytes > 0 {
                detail.push_str(&format!("，可用 {} MiB", bytes / 1024 / 1024));
            }
        }
        if let Err(error) = writable {
            return check_with_latency(
                "disk",
                "磁盘与状态目录",
                "fail",
                format!("{}；状态目录写入失败: {}", detail, error),
                started,
            );
        }
        check_with_latency("disk", "磁盘与状态目录", state, detail, started)
    }

    fn skill_check(&self) -> CheckResult {
        let started = Instant::now();
        let listing = self.skills.list();
        let count = listing.get("count").and_then(Value::as_u64).unwrap_or(0);
        let root = self.skills.install_root();
        let (state, detail) = match write_probe(&root) {
            Ok(()) if count == 0 => ("warn", "Skill 目录可写，但还没有安装 Skill".to_string()),
            Ok(()) => ("ok", format!("{} 个 Skill，安装目录可读写", count)),
            Err(error) => ("fail", format!("Skill 安装目录不可写: {}", error)),
        };
        check_with_latency("skills", "Skill", state, detail, started)
    }

    fn state_check(&self) -> CheckResult {
        let started = Instant::now();
        let skill_state = self
            .skills
            .install_root()
            .parent()
            .unwrap_or_else(|| Path::new("."))
            .join("skill_state.json");
        let files = [
            StateFile::json("提醒", self.timers.dir().join("timers.json"), false),
            StateFile::json("番茄钟", self.timers.dir().join("pomodoro.json"), false),
            StateFile::json("场景", crate::user_data::file("scenes.json"), false),
            StateFile::json("伴侣状态", crate::user_data::file("companion.json"), false),
            StateFile::json("Skill 状态", skill_state, false),
            StateFile::json_lines(
                "聊天记录",
                crate::user_data::file("chat_history.jsonl"),
                true,
            ),
        ];
        let mut checked = 0usize;
        let mut failed = Vec::new();
        let mut missing = Vec::new();
        for file in files {
            match file.check() {
                Ok(()) => checked += 1,
                Err(FileCheckError::Missing) => missing.push(file.label),
                Err(FileCheckError::Invalid(error)) => {
                    failed.push(format!("{}: {}", file.label, error))
                }
            }
        }
        let (state, detail) = if !failed.is_empty() {
            (
                "fail",
                format!(
                    "{} 个文件格式或权限异常: {}",
                    failed.len(),
                    failed.join("；")
                ),
            )
        } else if checked == 0 && !missing.is_empty() {
            ("warn", format!("{} 个状态文件尚未创建", missing.len()))
        } else {
            let suffix = if missing.is_empty() {
                String::new()
            } else {
                format!("，{} 个可选项尚未创建", missing.len())
            };
            (
                "ok",
                format!("{} 个状态文件可读且格式有效{}", checked, suffix),
            )
        };
        check_with_latency("state", "状态文件", state, detail, started)
    }
}

struct StateFile {
    label: &'static str,
    path: PathBuf,
    json_lines: bool,
    optional: bool,
}

enum FileCheckError {
    Missing,
    Invalid(String),
}

impl StateFile {
    fn json(label: &'static str, path: PathBuf, optional: bool) -> Self {
        Self {
            label,
            path,
            json_lines: false,
            optional,
        }
    }

    fn json_lines(label: &'static str, path: PathBuf, optional: bool) -> Self {
        Self {
            label,
            path,
            json_lines: true,
            optional,
        }
    }

    fn check(&self) -> Result<(), FileCheckError> {
        if !self.path.exists() {
            return if self.optional {
                Ok(())
            } else {
                Err(FileCheckError::Missing)
            };
        }
        let metadata =
            fs::metadata(&self.path).map_err(|error| FileCheckError::Invalid(error.to_string()))?;
        if !metadata.is_file() {
            return Err(FileCheckError::Invalid("不是普通文件".to_string()));
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            if metadata.permissions().mode() & 0o400 == 0 {
                return Err(FileCheckError::Invalid("当前用户没有读取权限".to_string()));
            }
        }
        let content = fs::read_to_string(&self.path)
            .map_err(|error| FileCheckError::Invalid(format!("读取失败: {}", error)))?;
        if self.json_lines {
            for (index, line) in content.lines().enumerate() {
                if line.trim().is_empty() {
                    continue;
                }
                serde_json::from_str::<Value>(line).map_err(|error| {
                    FileCheckError::Invalid(format!("第 {} 行不是有效 JSON: {}", index + 1, error))
                })?;
            }
        } else if !content.trim().is_empty() {
            serde_json::from_str::<Value>(&content)
                .map_err(|error| FileCheckError::Invalid(format!("JSON 无效: {}", error)))?;
        }
        Ok(())
    }
}

fn audio_check(kind: &str) -> (&'static str, String) {
    if crate::audio::audio_disabled() {
        return ("warn", "音频被 QZDESK_AUDIO_DISABLED 关闭".to_string());
    }
    let cards = fs::read_to_string("/proc/asound/cards").unwrap_or_default();
    let pcm = fs::read_to_string("/proc/asound/pcm").unwrap_or_default();
    let snd_entries = read_snd_entries();
    let label = if kind == "capture" {
        "录音"
    } else {
        "播放"
    };
    let has_proc_card = cards.lines().any(|line| {
        let trimmed = line.trim();
        trimmed
            .chars()
            .next()
            .map(|value| value.is_ascii_digit())
            .unwrap_or(false)
    });
    let has_snd_card = snd_entries.iter().any(|name| name.starts_with("controlC"));
    let stream_suffix = if kind == "capture" { 'c' } else { 'p' };
    let has_snd_stream = snd_entries
        .iter()
        .any(|name| name.starts_with("pcmC") && name.chars().next_back() == Some(stream_suffix));
    let has_card = has_proc_card || has_snd_card;
    let has_stream = pcm.to_ascii_lowercase().contains(kind) || has_snd_stream;
    if has_card && has_stream {
        if kind == "capture" {
            (
                "warn",
                "检测到 ALSA 录音设备；本项未采样输入信号，请用语音输入实测".to_string(),
            )
        } else {
            ("ok", format!("检测到 ALSA 声卡与{}通道", label))
        }
    } else if has_card {
        ("warn", format!("检测到声卡，但没有枚举到{}通道", label))
    } else if Path::new("/proc/asound").exists() || Path::new("/dev/snd").exists() {
        ("fail", format!("没有检测到可用声卡（{}）", label))
    } else {
        (
            "warn",
            "当前系统没有 /proc/asound，无法检查音频设备".to_string(),
        )
    }
}

fn read_snd_entries() -> Vec<String> {
    fs::read_dir("/dev/snd")
        .ok()
        .into_iter()
        .flatten()
        .filter_map(Result::ok)
        .filter_map(|entry| entry.file_name().into_string().ok())
        .collect()
}

fn check(id: &str, label: &str, status: &str, detail: String) -> CheckResult {
    CheckResult {
        id: id.to_string(),
        label: label.to_string(),
        status: status.to_string(),
        detail,
        latency_ms: 0,
    }
}

fn check_with_latency(
    id: &str,
    label: &str,
    status: &str,
    detail: String,
    started: Instant,
) -> CheckResult {
    CheckResult {
        id: id.to_string(),
        label: label.to_string(),
        status: status.to_string(),
        detail,
        latency_ms: started.elapsed().as_millis() as u64,
    }
}

fn is_private(address: IpAddr) -> bool {
    match address {
        IpAddr::V4(ip) => ip.is_private() || ip.is_loopback() || ip.is_link_local(),
        IpAddr::V6(ip) => ip.is_loopback() || ip.is_unique_local() || ip.is_unicast_link_local(),
    }
}

fn write_probe(dir: &Path) -> Result<(), String> {
    fs::create_dir_all(dir).map_err(|error| format!("无法创建 {}: {}", dir.display(), error))?;
    let path = dir.join(format!(
        ".qzdesk-write-test-{}",
        uuid::Uuid::new_v4().simple()
    ));
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)
        .map_err(|error| format!("无法创建测试文件: {}", error))?;
    file.write_all(b"qzdesk")
        .map_err(|error| error.to_string())?;
    drop(file);
    fs::remove_file(&path).map_err(|error| format!("无法删除测试文件: {}", error))
}

/// Redact common credential shapes while preserving enough context to debug.
pub fn sanitize_log_line(line: &str) -> String {
    const KEYS: &[&str] = &[
        "password",
        "passwd",
        "token",
        "secret",
        "authorization",
        "api_key",
        "apikey",
        "app_secret",
        "ws_token",
        "access_key",
        "private_key",
        "cookie",
    ];
    // Redact the complete bearer credential before generic key handling. The
    // generic redactor sees `Authorization: Bearer` and would otherwise replace
    // only the scheme, leaving the actual token in place.
    let mut output = redact_bearer(line);
    for key in KEYS {
        output = redact_key(&output, key);
    }
    output
}

fn redact_key(input: &str, key: &str) -> String {
    let mut cursor = 0usize;
    let mut output = String::with_capacity(input.len());
    while cursor < input.len() {
        let lower = input[cursor..].to_ascii_lowercase();
        let Some(relative) = lower.find(key) else {
            output.push_str(&input[cursor..]);
            break;
        };
        let start = cursor + relative;
        let after_key = start + key.len();
        let boundary_before = start == 0
            || !input[..start]
                .chars()
                .next_back()
                .map(|value| value.is_ascii_alphanumeric() || value == '_')
                .unwrap_or(false);
        let rest = &input[after_key..];
        let separator = rest
            .char_indices()
            .find(|(_, value)| !value.is_whitespace() && *value != '"' && *value != '\'');
        let Some((separator_offset, separator_char)) = separator else {
            output.push_str(&input[cursor..]);
            break;
        };
        if !boundary_before || !matches!(separator_char, '=' | ':') {
            output.push_str(&input[cursor..after_key]);
            cursor = after_key;
            continue;
        }
        let mut value_start = after_key + separator_offset + separator_char.len_utf8();
        let leading_whitespace = input[value_start..]
            .char_indices()
            .find(|(_, value)| !value.is_whitespace())
            .map(|(offset, _)| offset)
            .unwrap_or(input[value_start..].len());
        value_start += leading_whitespace;
        // Keep the opening quote in the exported log for readability, but start
        // at the first character of the credential. This also handles env-style
        // logs such as `password="abc123"`.
        let quote = input[value_start..]
            .chars()
            .next()
            .filter(|value| matches!(value, '"' | '\''));
        if quote.is_some() {
            value_start += 1;
        }
        let mut value_end = value_start;
        for (offset, value) in input[value_start..].char_indices() {
            let closes_quote = quote.is_some_and(|quote| value == quote);
            if value.is_whitespace() || closes_quote || matches!(value, ',' | ';' | '&' | '}' | ']')
            {
                break;
            }
            value_end = value_start + offset + value.len_utf8();
        }
        if value_end <= value_start {
            output.push_str(&input[cursor..after_key]);
            cursor = after_key;
            continue;
        }
        output.push_str(&input[cursor..value_start]);
        output.push_str("[REDACTED]");
        cursor = value_end;
    }
    output
}

fn redact_bearer(input: &str) -> String {
    let mut output = String::with_capacity(input.len());
    let mut cursor = 0usize;
    while cursor < input.len() {
        let lower = input[cursor..].to_ascii_lowercase();
        let Some(relative) = lower.find("bearer ") else {
            output.push_str(&input[cursor..]);
            break;
        };
        let start = cursor + relative;
        let value_start = start + "bearer ".len();
        let value_end = input[value_start..]
            .char_indices()
            .find(|(_, value)| {
                value.is_whitespace() || matches!(value, ',' | ';' | '"' | '\'' | '&' | ']' | '}')
            })
            .map(|(offset, _)| value_start + offset)
            .unwrap_or(input.len());
        output.push_str(&input[cursor..value_start]);
        output.push_str("[REDACTED]");
        cursor = value_end;
    }
    output
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sanitizer_redacts_common_credentials() {
        let line =
            r#"request password="abc123" Authorization: Bearer topsecret token=xyz api_key: k-9"#;
        let cleaned = sanitize_log_line(line);
        assert!(!cleaned.contains("abc123"));
        assert!(!cleaned.contains("topsecret"));
        assert!(!cleaned.contains("xyz"));
        assert!(!cleaned.contains("k-9"));
        assert!(cleaned.contains("[REDACTED]"));
    }

    #[test]
    fn sanitizer_keeps_non_secret_context() {
        let line = "[INFO] MQTT connected broker=mqtt://192.168.1.4:1883";
        assert_eq!(sanitize_log_line(line), line);
    }

    #[test]
    fn private_address_filter_is_rfc1918_only_for_remote_v4() {
        assert!(is_private("192.168.1.4".parse().unwrap()));
        assert!(is_private("10.0.0.2".parse().unwrap()));
        assert!(!is_private("8.8.8.8".parse().unwrap()));
    }
}

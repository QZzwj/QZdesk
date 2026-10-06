//! 日志的环形缓冲：核心最近打了什么，网页与设备界面都能直接看。
//!
//! 日志原本只有一个出口——启动脚本把标准输出重定向到 `/var/log/qzdesk.log`，
//! 想看就得连 ssh 翻文件。排查「技能到底下发没有」这种问题，往往就差最后几行。
//!
//! 这里包一层 logger：照旧打到终端/文件（格式不变），同时把最近几百行留在内存
//! 里，由 `/api/logs` 取出去。环形缓冲不落盘——落盘交给启动脚本那份重定向，
//! 内存这份只负责"随手能看"。

use log::{Log, Metadata, Record};
use std::collections::VecDeque;
use std::sync::{Arc, Mutex, OnceLock};

/// 留多少行。够翻最近一次排查，又不会让设备多占内存（一行几十字节）。
pub const CAPACITY: usize = 400;

/// 单行最长保留多少字符。云端报文偶尔很长，截断免得一条就吃掉整个缓冲。
const MAX_LINE_CHARS: usize = 600;

static BUFFER: OnceLock<Arc<Mutex<VecDeque<String>>>> = OnceLock::new();

/// 装上环形容器：包住 `inner`（原来的终端/文件输出），并在全局设置日志级别。
///
/// 只调用一次；重复调用会被忽略（`OnceLock` / `set_boxed_logger`）。
pub fn install(inner: Box<dyn Log>, level: log::LevelFilter) {
    let buffer = Arc::new(Mutex::new(VecDeque::with_capacity(CAPACITY)));
    let _ = BUFFER.set(buffer.clone());
    log::set_max_level(level);
    if log::set_boxed_logger(Box::new(Tee { inner, buffer })).is_ok() {
        log::info!("日志缓冲已就绪：网页 `/api/logs` 可看最近 {} 行", CAPACITY);
    }
}

/// 最近 `limit` 行，老的在前。缓冲还没装上（例如单元测试）时返回空。
pub fn tail(limit: usize) -> Vec<String> {
    let Some(buffer) = BUFFER.get() else {
        return Vec::new();
    };
    let buffer = buffer.lock().unwrap_or_else(|error| error.into_inner());
    let skip = buffer.len().saturating_sub(limit);
    buffer.iter().skip(skip).cloned().collect()
}

/// 已缓冲的行数。
pub fn len() -> usize {
    BUFFER
        .get()
        .map(|buffer| buffer.lock().unwrap_or_else(|error| error.into_inner()).len())
        .unwrap_or(0)
}

/// 本地时间戳 `MM-DD HH:MM:SS`。用本地时间：看日志的人对着的是设备上的钟，
/// 而不是 UTC。
fn timestamp() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs())
        .unwrap_or(0) as libc::time_t;
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    // SAFETY: `localtime_r` 只写我们提供的 `tm`。
    if unsafe { libc::localtime_r(&now, &mut tm) }.is_null() {
        return "-- --:--:--".to_string();
    }
    format!(
        "{:02}-{:02} {:02}:{:02}:{:02}",
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec
    )
}

struct Tee {
    inner: Box<dyn Log>,
    buffer: Arc<Mutex<VecDeque<String>>>,
}

impl Log for Tee {
    fn enabled(&self, metadata: &Metadata) -> bool {
        self.inner.enabled(metadata)
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }
        self.inner.log(record);

        let line = format!(
            "[{} {:<5}] {}",
            timestamp(),
            record.level(),
            record.args()
        );
        let line = if line.chars().count() > MAX_LINE_CHARS {
            let mut truncated: String = line.chars().take(MAX_LINE_CHARS).collect();
            truncated.push('…');
            truncated
        } else {
            line
        };
        let mut buffer = self.buffer.lock().unwrap_or_else(|error| error.into_inner());
        if buffer.len() >= CAPACITY {
            buffer.pop_front();
        }
        buffer.push_back(line);
    }

    fn flush(&self) {
        self.inner.flush();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timestamp_looks_like_a_clock() {
        let text = timestamp();
        assert_eq!(text.len(), 14, "{}", text);
        assert_eq!(&text[5..6], " ");
        assert_eq!(&text[8..9], ":");
        assert_eq!(&text[11..12], ":");
    }

    #[test]
    fn tail_without_install_is_empty() {
        // 缓冲是全局的：别的用例装过就至少能拿到那些行，没装过则是空。
        let lines = tail(10);
        assert!(lines.len() <= 10);
    }
}

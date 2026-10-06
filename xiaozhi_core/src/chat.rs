//! 聊天记录：GUI 与 web 控制台共用的同一份记录。
//!
//! 一块屏幕和一个浏览器页面看到的必须是同一个对话，所以记录只留一份、放在核心
//! 里：谁产生的消息都先写进 `ChatHub`，再由它广播给 web（SSE）并逐条推给 GUI
//! （UDP）。两边都只是这份记录的显示器，不各存一份，也就不会互相落后。
//!
//! 消息格式（web 的 SSE 与 GUI 的 UDP 用同一个 JSON）：
//!   {"type":"chat","id":7,"role":"user|assistant|system","text":"…","ts":1696500000}
//!
//! `id` 单调递增，用来把「先连上实时流、再拉一次历史」这两条来源对齐：ids 小于
//! 等于历史里最后一条的实时消息已经含在历史里，页面据此去重。
//!
//! 记录同时以 JSONL 追加到用户数据目录：重启后接着上一次的对话，而不是从空白
//! 开始（重启设备是很常见的事，用户不该因此丢掉刚才聊到哪儿）。

use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::sync::broadcast;

/// 内存里保留多少条记录。GUI / web 都是显示最近这一屏的对话，再多也没人看，
/// 而设备内存有限。
const HISTORY_LIMIT: usize = 100;

/// SSE 广播缓冲：客户端读得慢时宁可丢旧消息，也不拖住产生消息的那条链路。
const BROADCAST_LIMIT: usize = 128;

/// 落盘文件名（用户数据目录下）。
const HISTORY_FILE: &str = "chat_history.jsonl";

/// 文件涨到这个大小就压一次：只留最近 `HISTORY_KEEP` 条。设备上没人去清日志，
/// 压一次的成本远低于"写满整块分区"。
const HISTORY_FILE_LIMIT: u64 = 512 * 1024;
const HISTORY_KEEP: usize = 200;

/// 一条记录是谁说的。`System` 是核心自己的提示（合成失败、云端报错、提醒到点等），
/// 两侧都按系统气泡显示。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ChatRole {
    User,
    Assistant,
    System,
}

/// 一条聊天记录。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChatEntry {
    /// 从 1 开始递增，同一份记录里唯一。
    pub id: u64,
    pub role: ChatRole,
    pub text: String,
    /// Unix 秒。只用于排序和排查，界面上不显示。
    pub ts: u64,
}

#[derive(Clone)]
pub struct ChatHub {
    tx: broadcast::Sender<String>,
    history: Arc<Mutex<VecDeque<ChatEntry>>>,
    next_id: Arc<AtomicU64>,
    /// `None` = 只在内存里（测试用）。正常启动走 `load()`。
    path: Arc<Option<PathBuf>>,
}

impl ChatHub {
    /// 内存态，不落盘。
    pub fn new() -> Self {
        Self::with_path(None)
    }

    /// 从用户数据目录恢复上一次的对话。
    pub fn load() -> Self {
        Self::with_path(Some(crate::user_data::file(HISTORY_FILE)))
    }

    fn with_path(path: Option<PathBuf>) -> Self {
        let (tx, _) = broadcast::channel(BROADCAST_LIMIT);
        let restored = path.as_deref().map(restore).unwrap_or_default();
        // 接着历史的 id 往下发：网页靠 id 去重，重新从 1 开始会串。
        let next_id = restored.iter().map(|entry| entry.id).max().unwrap_or(0) + 1;
        Self {
            tx,
            history: Arc::new(Mutex::new(restored)),
            next_id: Arc::new(AtomicU64::new(next_id)),
            path: Arc::new(path),
        }
    }

    /// 记一条记录并广播出去，返回广播的那条报文。
    ///
    /// 返回报文是给 GUI 用的：GUI 走 UDP，不能订阅广播，需要核心显式发一份。
    /// 两边因此拿到的是字节相同的内容。空文本直接丢弃（云端的 stt / tts 事件
    /// 有时只带状态不带文字）。
    pub fn push(&self, role: ChatRole, text: &str) -> Option<String> {
        let text = text.trim();
        if text.is_empty() {
            return None;
        }

        let entry = ChatEntry {
            id: self.next_id.fetch_add(1, Ordering::Relaxed),
            role,
            text: text.to_string(),
            ts: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|elapsed| elapsed.as_secs())
                .unwrap_or(0),
        };
        let event = serde_json::json!({
            "type": "chat",
            "id": entry.id,
            "role": entry.role,
            "text": entry.text,
            "ts": entry.ts,
        })
        .to_string();

        {
            // 广播订阅者 panic 过也不该让聊天记录跟着失效，所以中毒时取回数据继续用。
            let mut history = self.history.lock().unwrap_or_else(|error| error.into_inner());
            if history.len() >= HISTORY_LIMIT {
                history.pop_front();
            }
            history.push_back(entry);
        }
        self.append(&event);
        let _ = self.tx.send(event.clone());
        Some(event)
    }

    /// 现有记录的快照（老的在前），给新打开的 web 页面补齐用。
    pub fn history(&self) -> Vec<ChatEntry> {
        let history = self.history.lock().unwrap_or_else(|error| error.into_inner());
        history.iter().cloned().collect()
    }

    pub fn subscribe(&self) -> broadcast::Receiver<String> {
        self.tx.subscribe()
    }

    /// 追加一行到 JSONL。落盘失败只记日志：聊天照常进行，下一次写入还会再试。
    fn append(&self, event: &str) {
        let Some(path) = self.path.as_ref() else {
            return;
        };
        if let Err(error) = append_line(path, event) {
            log::warn!("聊天记录写盘失败: {}", error);
            return;
        }
        if fs::metadata(path).map(|meta| meta.len()).unwrap_or(0) > HISTORY_FILE_LIMIT {
            if let Err(error) = compact(path) {
                log::warn!("聊天记录压缩失败: {}", error);
            }
        }
    }
}

fn append_line(path: &Path, line: &str) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir).map_err(|error| format!("无法创建 {}: {}", dir.display(), error))?;
    }
    let mut file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .map_err(|error| format!("无法打开 {}: {}", path.display(), error))?;
    writeln!(file, "{}", line).map_err(|error| format!("无法写入 {}: {}", path.display(), error))
}

/// 只留最近 `HISTORY_KEEP` 行，重写整个文件。
fn compact(path: &Path) -> Result<(), String> {
    let text =
        fs::read_to_string(path).map_err(|error| format!("无法读取 {}: {}", path.display(), error))?;
    let lines: Vec<&str> = text.lines().filter(|line| !line.trim().is_empty()).collect();
    let keep = lines.len().saturating_sub(HISTORY_KEEP);
    let body = lines[keep..].join("\n");
    let temp = path.with_extension("jsonl.tmp");
    {
        let mut file = fs::File::create(&temp)
            .map_err(|error| format!("无法写入 {}: {}", temp.display(), error))?;
        file.write_all(body.as_bytes())
            .map_err(|error| format!("无法写入 {}: {}", temp.display(), error))?;
        file.write_all(b"\n")
            .map_err(|error| format!("无法写入 {}: {}", temp.display(), error))?;
        file.sync_all()
            .map_err(|error| format!("无法持久化 {}: {}", temp.display(), error))?;
    }
    fs::rename(&temp, path).map_err(|error| format!("无法提交 {}: {}", path.display(), error))
}

/// 读回最近 `HISTORY_LIMIT` 条。坏行直接跳过：文件可能被手改过，不该因此让
/// 整段历史消失。
fn restore(path: &Path) -> VecDeque<ChatEntry> {
    let Ok(text) = fs::read_to_string(path) else {
        return VecDeque::new();
    };
    let mut entries: Vec<ChatEntry> = text
        .lines()
        .filter(|line| !line.trim().is_empty())
        .filter_map(|line| serde_json::from_str::<ChatEntry>(line).ok())
        .collect();
    let dropped = entries.len().saturating_sub(HISTORY_LIMIT);
    entries.drain(..dropped);
    entries.into()
}

impl Default for ChatHub {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicU32;

    static COUNTER: AtomicU32 = AtomicU32::new(0);

    fn temp_path() -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "qzdesk-chat-test-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir.join(HISTORY_FILE)
    }

    #[test]
    fn history_survives_a_restart() {
        let path = temp_path();
        let hub = ChatHub::with_path(Some(path.clone()));
        hub.push(ChatRole::User, "几点了");
        hub.push(ChatRole::Assistant, "九点整");

        let restored = ChatHub::with_path(Some(path.clone()));
        let history = restored.history();
        assert_eq!(history.len(), 2);
        assert_eq!(history[0].text, "几点了");
        assert_eq!(history[1].role, ChatRole::Assistant);
        // id 接着往下发，网页去重才不会错
        assert_eq!(history[1].id, 2);
        let next = restored.push(ChatRole::User, "谢谢");
        assert!(next.unwrap().contains("\"id\":3"));
    }

    #[test]
    fn memory_only_hub_writes_nothing() {
        let hub = ChatHub::new();
        hub.push(ChatRole::User, "只在内存里");
        assert_eq!(hub.history().len(), 1);
    }

    #[test]
    fn a_corrupt_line_is_skipped() {
        let path = temp_path();
        fs::write(
            &path,
            "{\"type\":\"chat\",\"id\":1,\"role\":\"user\",\"text\":\"好\",\"ts\":1}\nnot json\n",
        )
        .unwrap();
        let hub = ChatHub::with_path(Some(path));
        let history = hub.history();
        assert_eq!(history.len(), 1);
        assert_eq!(history[0].text, "好");
    }

    #[test]
    fn oversized_file_keeps_only_the_tail() {
        let path = temp_path();
        let mut body = String::new();
        for id in 1..=(HISTORY_KEEP as u64 + 50) {
            body.push_str(&format!(
                "{{\"type\":\"chat\",\"id\":{},\"role\":\"user\",\"text\":\"第 {} 条\",\"ts\":{}}}\n",
                id, id, id
            ));
        }
        fs::write(&path, body).unwrap();
        compact(&path).unwrap();
        let text = fs::read_to_string(&path).unwrap();
        assert_eq!(text.lines().count(), HISTORY_KEEP);
        assert!(text.lines().next().unwrap().contains("第 51 条"));
    }
}

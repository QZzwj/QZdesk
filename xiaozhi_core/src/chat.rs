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

use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::sync::broadcast;

/// 内存里保留多少条记录。GUI / web 都是显示最近这一屏的对话，再多也没人看，
/// 而设备内存有限。
const HISTORY_LIMIT: usize = 100;

/// SSE 广播缓冲：客户端读得慢时宁可丢旧消息，也不拖住产生消息的那条链路。
const BROADCAST_LIMIT: usize = 128;

/// 一条记录是谁说的。`System` 是核心自己的提示（合成失败、云端报错等），
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
}

impl ChatHub {
    pub fn new() -> Self {
        let (tx, _) = broadcast::channel(BROADCAST_LIMIT);
        Self {
            tx,
            history: Arc::new(Mutex::new(VecDeque::with_capacity(HISTORY_LIMIT))),
            next_id: Arc::new(AtomicU64::new(1)),
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
}

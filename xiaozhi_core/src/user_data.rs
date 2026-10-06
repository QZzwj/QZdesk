//! 用户数据目录与 JSON 落盘的小工具。
//!
//! 提醒、番茄钟、聊天记录、日志都要写到"能留下来的地方"，规则只有一套，
//! 所以收在这里：设备上优先 `/userdata/qzdesk`（可写、跨固件升级保留），
//! 开发机退回用户数据目录。`set_timer.py` / `pomodoro.py` 用的是同一个默认值，
//! 语音与界面因此读写同一份文件。

use std::env;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

/// 设备上持久化数据放在这里；开发机用用户数据目录。
pub fn dir() -> PathBuf {
    let device = PathBuf::from("/userdata/qzdesk");
    if fs::create_dir_all(&device).is_ok() {
        return device;
    }
    let base = env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|| env::var_os("HOME").map(|home| PathBuf::from(home).join(".local/share")))
        .unwrap_or_else(|| PathBuf::from("."));
    let path = base.join("qzdesk");
    let _ = fs::create_dir_all(&path);
    path
}

/// 用户数据目录下的一个文件。
pub fn file(name: &str) -> PathBuf {
    dir().join(name)
}

/// 先写临时文件再原子替换：界面随时可能来读，不能让它看到半份 JSON。
pub fn write_json_atomic(path: &Path, value: &serde_json::Value) -> Result<(), String> {
    let dir = path.parent().unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(dir).map_err(|error| format!("无法创建 {}: {}", dir.display(), error))?;
    let name = path
        .file_name()
        .map(|value| value.to_string_lossy().to_string())
        .unwrap_or_else(|| "data.json".to_string());
    let temp = dir.join(format!(".{}.tmp", name));
    let text =
        serde_json::to_string_pretty(value).map_err(|error| format!("序列化失败: {}", error))?;
    {
        let mut file = fs::File::create(&temp)
            .map_err(|error| format!("无法写入 {}: {}", temp.display(), error))?;
        file.write_all(text.as_bytes())
            .map_err(|error| format!("无法写入 {}: {}", temp.display(), error))?;
        file.write_all(b"\n")
            .map_err(|error| format!("无法写入 {}: {}", temp.display(), error))?;
        file.sync_all()
            .map_err(|error| format!("无法持久化 {}: {}", temp.display(), error))?;
    }
    fs::rename(&temp, path).map_err(|error| format!("无法提交 {}: {}", path.display(), error))
}

pub fn read_json(path: &Path) -> Option<serde_json::Value> {
    let text = fs::read_to_string(path).ok()?;
    serde_json::from_str(&text).ok()
}

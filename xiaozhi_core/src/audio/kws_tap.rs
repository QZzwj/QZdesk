//! 唤醒词进程（qzdesk-kws）的 PCM 旁路。
//!
//! 采集线程每读完一个 period，就把处理过的 **第 0 声道** 再投一份给本地的 KWS 进程；
//! 对方按 16bit 单声道解析，跑 Rockchip 的 NPU 唤醒模型，命中后直接给核心的 UDP 口
//! 发 `{"type":"listen","state":"start"}`（那条分支本来就是留给外部调用者的）。
//!
//! 三条原则：
//!   * **非阻塞、失败即丢**：唤醒进程没在跑时，对语音链路零影响（丢一帧而已）；
//!   * **不开就不做**：只有设了 `QZDESK_KWS_SOCKET` 才建 socket，默认完全关闭，
//!     也不用为"没有唤醒词"的设备多花一个字节的开销；
//!   * **单声道**：给的是第 0 声道（板子单麦）。真有多麦阵列的话，这里要按阵列
//!     布局改造，并把 `src_chan` 一起告诉 KWS 进程。

use std::os::unix::net::UnixDatagram;
use std::path::PathBuf;
use std::sync::OnceLock;

static TAP: OnceLock<Option<(UnixDatagram, PathBuf)>> = OnceLock::new();

/// 按环境变量 `QZDESK_KWS_SOCKET` 建立旁路。没设、或建不出来时静默关闭。
pub fn init() {
    let _ = TAP.set(create());
}

fn create() -> Option<(UnixDatagram, PathBuf)> {
    let path = std::env::var("QZDESK_KWS_SOCKET").ok()?;
    if path.trim().is_empty() {
        return None;
    }
    let socket = UnixDatagram::unbound().ok()?;
    if socket.set_nonblocking(true).is_err() {
        return None;
    }
    log::info!("唤醒词旁路已开启：{}（KWS 进程没在跑时自动丢帧）", path);
    Some((socket, PathBuf::from(path)))
}

/// 投一帧单声道 PCM。没有接收方时直接丢：不报错、不阻塞采集线程。
pub fn send(samples: &[i16]) {
    let Some(Some((socket, path))) = TAP.get() else {
        return;
    };
    if samples.is_empty() {
        return;
    }
    // S16LE：把 i16 切片直接当字节看（ARM 小端，与设备协议一致）
    let bytes =
        unsafe { std::slice::from_raw_parts(samples.as_ptr() as *const u8, samples.len() * 2) };
    let _ = socket.send_to(bytes, path);
}

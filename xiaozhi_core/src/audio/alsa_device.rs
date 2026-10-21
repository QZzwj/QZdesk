//! ALSA PCM device wrappers for audio capture and playback.

use alsa::pcm::{Access, Format, HwParams, PCM};
use alsa::{Direction, ValueOr};
use anyhow::{Context, Result};

/// Parameters negotiated with the ALSA hardware.
#[derive(Debug, Clone)]
pub struct AlsaParams {
    /// Actual sample rate after negotiation
    pub sample_rate: u32,
    /// Actual number of channels
    pub channels: u32,
    /// Period size in frames (one frame = channels × sample_width)
    pub period_size: usize,
}

/// Open a PCM device for capture (recording).
pub fn open_capture(device: &str, sample_rate: u32, channels: u32) -> Result<(PCM, AlsaParams)> {
    let devices = capture_device_candidates(device);
    let requested_channels = channels.max(1);
    let channel_options = capture_channel_candidates(requested_channels);

    let mut failures = Vec::new();
    for candidate_device in &devices {
        for candidate_channels in &channel_options {
            match open_pcm(
                candidate_device,
                Direction::Capture,
                sample_rate,
                *candidate_channels,
                None,
                "Capture",
            ) {
                Ok((pcm, params)) => {
                    if candidate_device != device || *candidate_channels != requested_channels {
                        log::warn!(
                            "ALSA capture fallback selected: requested {} {}ch, using {} {}ch",
                            device,
                            requested_channels,
                            candidate_device,
                            candidate_channels,
                        );
                    }
                    return Ok((pcm, params));
                }
                Err(error) => failures.push(format!(
                    "{} ({}ch): {:#}",
                    candidate_device, candidate_channels, error
                )),
            }
        }
    }

    Err(anyhow::anyhow!(
        "No usable ALSA capture configuration for '{}': {}",
        device,
        failures.join("; ")
    ))
}

/// QZdesk 的模拟麦克风位于第 0 声道，第 1 声道是静音参考。`default` 在
/// 单声道协商时可能只返回第 1 声道；强制先试双声道后，`default` 能同时保留
/// 麦克风和播放混音，避免绕过 plug 层直接占用 `hw:0,0` 造成并发 I/O 错误。
fn capture_device_candidates(device: &str) -> Vec<String> {
    if device.is_empty() || device == "default" {
        vec![
            "default".to_string(),
            "plughw:0,0".to_string(),
            "hw:0,0".to_string(),
        ]
    } else {
        vec![device.to_string()]
    }
}

fn capture_channel_candidates(requested_channels: u32) -> Vec<u32> {
    let mut channels = Vec::with_capacity(3);
    for candidate in [2, requested_channels, 1] {
        if !channels.contains(&candidate) {
            channels.push(candidate);
        }
    }
    channels
}

/// Open a PCM device for playback.
pub fn open_playback(
    device: &str,
    sample_rate: u32,
    channels: u32,
    period_size: Option<usize>,
) -> Result<(PCM, AlsaParams)> {
    open_pcm(
        device,
        Direction::Playback,
        sample_rate,
        channels,
        period_size,
        "Playback",
    )
}

fn open_pcm(
    device: &str,
    direction: Direction,
    sample_rate: u32,
    channels: u32,
    period_size: Option<usize>,
    dir_name: &str,
) -> Result<(PCM, AlsaParams)> {
    // Audio threads use blocking read/write so ALSA can pace them naturally.
    // Opening playback non-blocking turned normal buffer back-pressure into
    // EAGAIN/EPIPE churn and required a 5 ms polling loop in the player.
    let pcm = PCM::new(device, direction, false)
        .with_context(|| format!("Failed to open PCM device '{}' for {}", device, dir_name))?;

    // 1. 动态探测与配置硬件参数 (HwParams)
    let (actual_rate, actual_channels, actual_period_size, actual_buffer_size) = {
        let hwp = HwParams::any(&pcm).with_context(|| "Failed to initialize HwParams")?;

        // 动态探测设备支持的边界能力（便于日志排查 USB 声卡的限制）
        log::info!(
            "ALSA Probe [{} {}]: Rate=[{} - {}], Channels=[{} - {}]",
            device,
            dir_name,
            hwp.get_rate_min().unwrap_or(0),
            hwp.get_rate_max().unwrap_or(0),
            hwp.get_channels_min().unwrap_or(0),
            hwp.get_channels_max().unwrap_or(0),
        );

        hwp.set_access(Access::RWInterleaved)?;
        hwp.set_format(Format::S16LE)?;
        hwp.set_channels(channels)?;
        hwp.set_rate_near(sample_rate, ValueOr::Nearest)?;

        if let Some(ps) = period_size {
            hwp.set_period_size_near(ps as alsa::pcm::Frames, ValueOr::Nearest)?;
        }

        // Set the buffer *after* the period. On this ACODEC, requesting a period
        // after an 8192-frame buffer silently shrinks the final buffer back to
        // four periods (4096 frames / only 85 ms at 48 kHz), which underruns on
        // ordinary Wi-Fi jitter. Negotiate the period first, then keep as much
        // of the 8192-frame buffer as the hardware allows.
        let max_buffer = hwp.get_buffer_size_max().unwrap_or(8192) as usize;
        let target_buffer = std::cmp::min(max_buffer, 8192) as alsa::pcm::Frames;
        if let Err(e) = hwp.set_buffer_size_near(target_buffer) {
            log::warn!("Could not set optimal buffer size: {}", e);
        }
        pcm.hw_params(&hwp)?;

        // 读取底层最终协商确认的参数
        let hwp_current = pcm.hw_params_current()?;
        (
            hwp_current.get_rate()?,
            hwp_current.get_channels()?,
            hwp_current.get_period_size()? as usize,
            hwp_current.get_buffer_size()? as usize,
        )
    };

    // 2. 动态配置软件参数 (SwParams)
    {
        if let Ok(swp) = pcm.sw_params_current() {
            // 设置启动阈值：缓冲区数据达到 buffer_size 的一半（或至少一个 period）时才开始传输
            // 给应用程序足够的 "蓄水" 时间，避免启动瞬间被抽干
            let start_threshold =
                (actual_buffer_size / 2).max(actual_period_size) as alsa::pcm::Frames;
            let _ = swp.set_start_threshold(start_threshold);

            // 设置可用空间下限阈值
            let _ = swp.set_avail_min(actual_period_size as alsa::pcm::Frames);

            if let Err(e) = pcm.sw_params(&swp) {
                log::warn!("Failed to inject ALSA sw_params (non-fatal): {}", e);
            }
        }
    }

    let params = AlsaParams {
        sample_rate: actual_rate,
        channels: actual_channels,
        period_size: actual_period_size,
    };

    log::info!(
        "ALSA {} Negotiated: device={}, rate={}, ch={}, period_size={}, buffer_size={}",
        dir_name,
        device,
        actual_rate,
        actual_channels,
        actual_period_size,
        actual_buffer_size,
    );

    Ok((pcm, params))
}

#[cfg(test)]
mod tests {
    use super::{capture_channel_candidates, capture_device_candidates};

    #[test]
    fn default_capture_prefers_stereo_before_mono() {
        assert_eq!(
            capture_device_candidates("default"),
            ["default", "plughw:0,0", "hw:0,0"]
        );
        assert_eq!(capture_channel_candidates(1), [2, 1]);
    }

    #[test]
    fn explicit_capture_device_is_not_replaced() {
        assert_eq!(capture_device_candidates("hw:1,0"), ["hw:1,0"]);
        assert_eq!(capture_channel_candidates(2), [2, 1]);
    }
}

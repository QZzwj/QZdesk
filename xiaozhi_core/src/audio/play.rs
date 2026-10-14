use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use tokio::sync::mpsc;
use anyhow::Result;

use super::alsa_device;
use super::opus_codec::OpusDecoder;
use super::stream_decoder::StreamDecoder;
use super::audio_system::AudioConfig;
use crate::audio_bridge::PlaybackData;

/// Factory function: create a decoder based on the configured playback format.
fn create_decoder(
    config: &AudioConfig,
    alsa_rate: u32,
    alsa_channels: u32,
) -> Result<Box<dyn StreamDecoder>> {
    match config.stream_format.as_str() {
        "opus" => {
            let decoder = OpusDecoder::new(
                config.opus_sample_rate,
                config.opus_channels,
                config.decode_frame_duration_ms,
                alsa_rate,
                alsa_channels,
            )?;
            Ok(Box::new(decoder))
        }
        other => anyhow::bail!("Unsupported stream format: {}", other),
    }
}

pub fn play_thread(
    config: &AudioConfig,
    mut opus_rx: mpsc::Receiver<PlaybackData>,
    running: &AtomicBool,
    generation: &AtomicU64,
) -> Result<()> {
    // 1. Open ALSA playback device with configurable sample rate, channels, and period size
    let period_size_opt = if config.playback_period_size > 0 {
        Some(config.playback_period_size)
    } else {
        None
    };
    let (pcm, params) = alsa_device::open_playback(
        &config.playback_device,
        config.playback_sample_rate,
        config.playback_channels,
        period_size_opt,
    )?;

    let actual_rate = params.sample_rate;
    let actual_channels = params.channels;
    let _period_size = params.period_size;

    // 2. Initialize decoder via factory pattern
    let mut decoder = create_decoder(config, actual_rate, actual_channels)?;

    let io = pcm.io_i16()?;

    log::info!(
        "Playback started: stream_format={}, rate={}, ch={}, period={}",
        config.stream_format,
        actual_rate,
        actual_channels,
        _period_size,
    );

    let mut active_generation = generation.load(Ordering::Acquire);
    while running.load(Ordering::Relaxed) {
        let current = generation.load(Ordering::Acquire);
        if current != active_generation {
            pcm.drop()?;
            pcm.prepare()?;
            decoder = create_decoder(config, actual_rate, actual_channels)?;
            active_generation = current;
        }
        match opus_rx.try_recv() {
            Ok(audio_data) => {
                let pcm_data = match audio_data {
                    PlaybackData::Compressed { data, generation: packet_generation } => {
                        if packet_generation != generation.load(Ordering::Acquire) { continue; }
                        decoder.decode(&data)
                    }
                    PlaybackData::Flush(flush_generation) => {
                        if flush_generation == active_generation { continue; }
                        continue;
                    }
                };
                match pcm_data {
                    Ok(pcm_data) => {
                        if pcm_data.is_empty() {
                            continue;
                        }
                        // Write decoded PCM to ALSA with retry loop to handle
                        // short writes and XRUN recovery without losing frames.
                        let total_frames = pcm_data.len() / actual_channels as usize;
                        let mut frames_written = 0;
                        let mut retry_count = 0u32;

                        while frames_written < total_frames {
                            if generation.load(Ordering::Acquire) != active_generation { break; }
                            let offset = frames_written * actual_channels as usize;
                            match io.writei(&pcm_data[offset..]) {
                                Ok(n) => {
                                    frames_written += n;
                                    retry_count = 0; // 成功写入，重置重试计数
                                }
                                Err(e) if e.errno() == libc::EAGAIN => {
                                    std::thread::sleep(std::time::Duration::from_millis(5));
                                }
                                Err(e) => {
                                    log::warn!("ALSA XRUN or error: {}, recovering...", e);
                                    retry_count += 1;

                                    // 触发 ALSA 硬件恢复状态机
                                    if let Err(e2) = pcm.prepare() {
                                        log::error!(
                                            "Failed to recover PCM playback: {}",
                                            e2
                                        );
                                        break;
                                    }

                                    // 熔断器：底层持续跟不上写入速度时，丢弃剩余帧防止死循环
                                    if retry_count >= 3 {
                                        log::error!(
                                            "Max recovery retries ({}) reached. Dropping {} unwritten frames to break dead-loop.",
                                            retry_count,
                                            total_frames - frames_written
                                        );
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    Err(e) => {
                        log::error!("Audio decode error: {}", e);
                    }
                }
            }
            Err(mpsc::error::TryRecvError::Empty) => std::thread::sleep(std::time::Duration::from_millis(5)),
            Err(mpsc::error::TryRecvError::Disconnected) => {
                log::info!("Playback channel closed");
                break;
            }
        }
    }

    log::info!("Playback stopped");
    Ok(())
}

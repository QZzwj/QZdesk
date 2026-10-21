use anyhow::Result;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use tokio::sync::mpsc;

use super::alsa_device;
use super::audio_system::AudioConfig;
use super::opus_codec::OpusDecoder;
use super::stream_decoder::StreamDecoder;
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

        // Match Echo-Mate's proven playback path: block on both the packet queue
        // and ALSA. Polling an 80 ms packet queue every 5 ms added wakeups and
        // turned ordinary buffer back-pressure into EAGAIN/EPIPE and dropped audio.
        let Some(audio_data) = opus_rx.blocking_recv() else {
            log::info!("Playback channel closed");
            break;
        };

        match audio_data {
            PlaybackData::Compressed {
                data,
                generation: packet_generation,
            } => {
                if packet_generation != generation.load(Ordering::Acquire) {
                    continue;
                }
                let pcm_data = match decoder.decode(&data) {
                    Ok(pcm_data) => pcm_data,
                    Err(e) => {
                        log::error!("Audio decode error: {}", e);
                        continue;
                    }
                };
                if pcm_data.is_empty() {
                    continue;
                }

                let total_frames = pcm_data.len() / actual_channels as usize;
                let mut frames_written = 0;
                let mut retry_count = 0u32;

                while frames_written < total_frames {
                    if generation.load(Ordering::Acquire) != active_generation {
                        break;
                    }
                    let offset = frames_written * actual_channels as usize;
                    match io.writei(&pcm_data[offset..]) {
                        Ok(n) => {
                            frames_written += n;
                            retry_count = 0;
                        }
                        Err(e) => {
                            log::warn!("ALSA playback error: {}, recovering...", e);
                            retry_count += 1;
                            if let Err(recover_error) = pcm.recover(e.errno(), true) {
                                log::error!("Failed to recover PCM playback: {}", recover_error);
                                break;
                            }
                            if retry_count >= 3 {
                                log::error!(
                                    "Max playback recovery retries reached; dropping {} frames",
                                    total_frames - frames_written,
                                );
                                break;
                            }
                        }
                    }
                }
            }
            PlaybackData::Flush(flush_generation) => {
                // A flush can arrive while the write side is idle. Reset here so
                // the next packet is not delayed until another loop iteration.
                if flush_generation != active_generation {
                    pcm.drop()?;
                    pcm.prepare()?;
                    decoder = create_decoder(config, actual_rate, actual_channels)?;
                    active_generation = flush_generation;
                }
            }
        }
    }

    log::info!("Playback stopped");
    Ok(())
}

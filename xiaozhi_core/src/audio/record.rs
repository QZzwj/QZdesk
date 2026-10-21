use anyhow::Result;
use std::fs::File;
use std::io::Write;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::SyncSender;
use std::time::{Duration, Instant};
use tokio::sync::mpsc;

use super::alsa_device;
use super::audio_system::AudioConfig;
use super::opus_codec::OpusEncoder;
use super::speex::Preprocessor;

const SILENCE_FAULT_WINDOWS: u32 = 5;

#[derive(Default)]
struct LevelStats {
    sum_squares: f64,
    peak: i32,
    samples: u64,
}

fn open_pcm_dump(variable: &str) -> Option<File> {
    let path = std::env::var(variable).ok()?;
    if path.trim().is_empty() {
        return None;
    }
    match File::create(&path) {
        Ok(file) => {
            log::info!("MIC PCM dump enabled: {}={} (mono s16le)", variable, path);
            Some(file)
        }
        Err(error) => {
            log::warn!("Could not open {}={}: {}", variable, path, error);
            None
        }
    }
}

fn write_pcm_dump(dump: &mut Option<File>, label: &str, samples: &[i16]) {
    let Some(file) = dump.as_mut() else {
        return;
    };
    let mut bytes = Vec::with_capacity(samples.len() * 2);
    for sample in samples {
        bytes.extend_from_slice(&sample.to_le_bytes());
    }
    if let Err(error) = file.write_all(&bytes) {
        log::warn!("MIC {} PCM dump failed: {}; disabling dump", label, error);
        *dump = None;
    }
}

impl LevelStats {
    fn add(&mut self, sample: i16) {
        let value = i32::from(sample);
        self.sum_squares += f64::from(value) * f64::from(value);
        self.peak = self.peak.max(value.abs());
        self.samples += 1;
    }

    fn rms(&self) -> f64 {
        if self.samples == 0 {
            return 0.0;
        }
        (self.sum_squares / self.samples as f64).sqrt()
    }

    fn dbfs(&self) -> f64 {
        let rms = self.rms();
        if rms > 0.0 {
            20.0 * (rms / 32768.0).log10()
        } else {
            -120.0
        }
    }

    fn clear(&mut self) {
        *self = Self::default();
    }
}

fn track_silent_window(peak: i32, consecutive_silent: &mut u32) -> bool {
    if peak != 0 {
        *consecutive_silent = 0;
        return false;
    }
    *consecutive_silent = consecutive_silent.saturating_add(1);
    *consecutive_silent == SILENCE_FAULT_WINDOWS
}

pub fn record_thread(
    config: &AudioConfig,
    opus_tx: mpsc::Sender<Vec<u8>>,
    running: &AtomicBool,
    persistent_silence: &AtomicBool,
    ready_tx: SyncSender<std::result::Result<(), String>>,
) -> Result<()> {
    let result = record_thread_inner(config, opus_tx, running, persistent_silence, &ready_tx);
    if let Err(error) = &result {
        let _ = ready_tx.send(Err(format!("{error:#}")));
    }
    result
}

fn record_thread_inner(
    config: &AudioConfig,
    opus_tx: mpsc::Sender<Vec<u8>>,
    running: &AtomicBool,
    persistent_silence: &AtomicBool,
    ready_tx: &SyncSender<std::result::Result<(), String>>,
) -> Result<()> {
    // 1. Open ALSA capture device
    let (pcm, params) =
        alsa_device::open_capture(&config.capture_device, config.sample_rate, config.channels)?;

    let actual_rate = params.sample_rate;
    let actual_channels = params.channels;
    let period_size = params.period_size;

    // 2. The board exposes the microphone on the left channel and leaves the
    // right channel at a fixed DC/noise level. Preprocess only the left channel,
    // and use a full 20 ms frame instead of the 128-frame ALSA period. Speex's
    // denoiser and AGC are defined around speech-sized windows; feeding 5.3 ms
    // periods made its gain decisions unstable even when the analog path was good.
    let speex_frame_size = (actual_rate / 50) as usize;
    let mut preprocessor = Preprocessor::new(speex_frame_size, actual_rate)?;
    preprocessor.set_denoise(true);
    preprocessor.set_noise_suppress(-25);
    preprocessor.set_agc(true);
    preprocessor.set_agc_level(24000.0);

    // 3. Initialize Opus encoder (with resampling + channel conversion)
    let mut encoder = OpusEncoder::new(
        actual_rate,
        actual_channels,
        config.encode_frame_duration_ms,
        config.opus_sample_rate,
        config.opus_channels,
        config.opus_bitrate,
    )?;

    let input_frame_samples = encoder.input_frame_samples();

    // Processed interleaved PCM for the Opus encoder.
    let mut accum_buf: Vec<i16> = Vec::with_capacity(input_frame_samples * 2);
    let mut speex_buf: Vec<i16> = Vec::with_capacity(speex_frame_size);
    let mut mono_buf: Vec<i16> = Vec::with_capacity(period_size);

    // ALSA read buffer (interleaved i16, one period)
    let mut read_buf = vec![0i16; period_size * actual_channels as usize];

    let io = pcm.io_i16()?;
    let mut last_level_log = Instant::now();
    let mut consecutive_silent_windows = 0u32;
    let mut raw_levels = LevelStats::default();
    let mut processed_levels = LevelStats::default();
    let mut raw_dump = open_pcm_dump("QZDESK_MIC_RAW_DUMP");
    let mut processed_dump = open_pcm_dump("QZDESK_MIC_DUMP");

    log::info!(
        "Recording started: rate={}, ch={}, period={}, speex_frame={}, opus_frame_samples={}",
        actual_rate,
        actual_channels,
        period_size,
        speex_frame_size,
        input_frame_samples,
    );

    // Mark capture as ready only after ALSA, Speex and Opus are all initialized.
    // AudioSystem waits for this acknowledgement before claiming the mic works.
    ready_tx
        .send(Ok(()))
        .map_err(|_| anyhow::anyhow!("audio supervisor stopped before capture became ready"))?;

    while running.load(Ordering::Relaxed) {
        // Read one period from ALSA
        match io.readi(&mut read_buf) {
            Ok(frames) => {
                mono_buf.clear();
                mono_buf.extend(
                    read_buf[..frames * actual_channels as usize]
                        .chunks_exact(actual_channels as usize)
                        .map(|frame| frame[0]),
                );
                write_pcm_dump(&mut raw_dump, "raw", &mono_buf);

                // Wake-word detection gets the raw left channel, before denoise/AGC,
                // so it stays independent from the cloud ASR preprocessing chain.
                super::kws_tap::send(&mono_buf);

                for &sample in &mono_buf {
                    raw_levels.add(sample);
                    speex_buf.push(sample);
                    if speex_buf.len() < speex_frame_size {
                        continue;
                    }

                    preprocessor.process(&mut speex_buf);
                    write_pcm_dump(&mut processed_dump, "processed", &speex_buf);
                    for &processed in &speex_buf {
                        processed_levels.add(processed);
                        accum_buf.push(processed);
                        // OpusEncoder selects channel 0 for mono output. Keep the
                        // interleaved layout, but never encode the invalid right channel.
                        for _ in 1..actual_channels {
                            accum_buf.push(0);
                        }
                    }
                    speex_buf.clear();

                    while accum_buf.len() >= input_frame_samples {
                        let frame = &accum_buf[..input_frame_samples];
                        match encoder.encode(frame) {
                            Ok(opus_data) => {
                                if !opus_data.is_empty()
                                    && opus_tx.blocking_send(opus_data).is_err()
                                {
                                    log::warn!("Failed to send opus data, receiver dropped");
                                    return Ok(());
                                }
                            }
                            Err(e) => log::error!("Opus encode error: {}", e),
                        }
                        accum_buf.drain(..input_frame_samples);
                    }
                }

                if last_level_log.elapsed() >= Duration::from_secs(2) {
                    // Measure complete two-second windows. The old implementation
                    // sampled a single ALSA period every two seconds, which made a
                    // short consonant look like the whole microphone level.
                    let silence_fault =
                        track_silent_window(raw_levels.peak, &mut consecutive_silent_windows);
                    if raw_levels.peak == 0 {
                        if silence_fault {
                            persistent_silence.store(true, Ordering::Relaxed);
                            log::error!(
                                "MIC capture delivered only zero samples for {} seconds; \
                                 check MICBIAS, microphone wiring, and the ALSA capture mixer",
                                SILENCE_FAULT_WINDOWS * 2,
                            );
                        } else if consecutive_silent_windows < SILENCE_FAULT_WINDOWS {
                            log::warn!(
                                "MIC level: silent window {}/{} (raw_dbfs={:.1}, ch={})",
                                consecutive_silent_windows,
                                SILENCE_FAULT_WINDOWS,
                                raw_levels.dbfs(),
                                actual_channels,
                            );
                        }
                    } else {
                        if persistent_silence.swap(false, Ordering::Relaxed) {
                            log::info!("MIC capture recovered: non-zero samples received");
                        }
                        log::info!(
                            "MIC level: raw_rms={:.1} raw_peak={} raw_dbfs={:.1}, \
                             processed_rms={:.1} processed_peak={} processed_dbfs={:.1}, \
                             ch={} mic_ch=0",
                            raw_levels.rms(),
                            raw_levels.peak,
                            raw_levels.dbfs(),
                            processed_levels.rms(),
                            processed_levels.peak,
                            processed_levels.dbfs(),
                            actual_channels,
                        );
                    }
                    raw_levels.clear();
                    processed_levels.clear();
                    last_level_log = Instant::now();
                }
            }
            Err(e) => {
                log::warn!("ALSA capture error: {}, recovering...", e);
                if let Err(e2) = pcm.prepare() {
                    log::error!("Failed to recover PCM capture: {}", e2);
                    break;
                }
            }
        }
    }

    if let Some(file) = raw_dump.as_mut() {
        let _ = file.flush();
    }
    if let Some(file) = processed_dump.as_mut() {
        let _ = file.flush();
    }
    log::info!("Recording stopped");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{SILENCE_FAULT_WINDOWS, track_silent_window};

    #[test]
    fn all_zero_capture_is_flagged_only_after_sustained_silence() {
        let mut windows = 0;

        for _ in 0..SILENCE_FAULT_WINDOWS - 1 {
            assert!(!track_silent_window(0, &mut windows));
        }
        assert!(track_silent_window(0, &mut windows));
        assert_eq!(windows, SILENCE_FAULT_WINDOWS);
        assert!(!track_silent_window(0, &mut windows));

        assert!(!track_silent_window(1, &mut windows));
        assert_eq!(windows, 0);
    }
}

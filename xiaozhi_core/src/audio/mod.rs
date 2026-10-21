//! audio - Audio capture, playback, and codec library
//!
//! Replaces the external C++ sound_app process with an integrated Rust library.
//! Uses ALSA for audio I/O, Opus for encoding/decoding, and SpeexDSP
//! for noise suppression, AGC, and resampling.

mod alsa_device;
mod audio_system;
/// 采集到的 PCM 旁路给唤醒词进程（见模块内的说明）。
pub mod kws_tap;
mod opus_codec;
mod play;
mod record;
mod speex;
pub mod stream_decoder;

pub use audio_system::{AudioConfig, AudioSystem};
// The Opus encoder is also used by the text-to-speech path: synthesised PCM has
// to reach the server in exactly the same packets as microphone audio.
pub use opus_codec::OpusEncoder;

/// Parse the audio kill switch without treating an explicit `0`/`false` as on.
/// Unrecognised values stay enabled so a typo cannot silently disable the mic.
pub fn parse_audio_disabled(value: Option<&str>) -> bool {
    value
        .map(|raw| {
            matches!(
                raw.trim().to_ascii_lowercase().as_str(),
                "1" | "true" | "yes"
            )
        })
        .unwrap_or(false)
}

/// True only when `QZDESK_AUDIO_DISABLED` explicitly requests the kill switch.
pub fn audio_disabled() -> bool {
    parse_audio_disabled(std::env::var("QZDESK_AUDIO_DISABLED").ok().as_deref())
}

#[cfg(test)]
mod tests {
    use super::parse_audio_disabled;

    #[test]
    fn audio_kill_switch_is_explicit_and_case_insensitive() {
        assert!(!parse_audio_disabled(None));
        assert!(!parse_audio_disabled(Some("")));
        assert!(!parse_audio_disabled(Some("0")));
        assert!(!parse_audio_disabled(Some("false")));
        assert!(!parse_audio_disabled(Some("no")));
        assert!(parse_audio_disabled(Some("1")));
        assert!(parse_audio_disabled(Some(" TRUE ")));
        assert!(parse_audio_disabled(Some("Yes")));
    }
}

//! audio - Audio capture, playback, and codec library
//!
//! Replaces the external C++ sound_app process with an integrated Rust library.
//! Uses ALSA for audio I/O, Opus for encoding/decoding, and SpeexDSP
//! for noise suppression, AGC, and resampling.

mod alsa_device;
mod audio_system;
mod opus_codec;
mod play;
mod record;
mod speex;
pub mod stream_decoder;

pub use audio_system::{AudioConfig, AudioSystem};
// The Opus encoder is also used by the text-to-speech path: synthesised PCM has
// to reach the server in exactly the same packets as microphone audio.
pub use opus_codec::OpusEncoder;

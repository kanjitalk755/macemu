/*
 * Audio Configuration - Centralized audio settings
 *
 * All audio parameters are defined here for easy tuning and experimentation.
 * Changes here propagate to: emulator, server, Opus encoder, and WebRTC config.
 */

#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

// ============================================================================
// Core Audio Format
// ============================================================================

// Sample rate (Hz) - 48kHz is WebRTC/Opus standard
// Emulator calculates frame size based on this: sample_rate * frame_duration_ms / 1000
#define AUDIO_SAMPLE_RATE       48000

// Bits per sample (8 or 16)
#define AUDIO_SAMPLE_SIZE       16

// Number of channels (1 = mono, 2 = stereo)
#define AUDIO_CHANNELS          2

// Frame duration in milliseconds
// Opus supports: 2.5, 5, 10, 20, 40, 60ms
// 20ms is standard for low-latency VoIP/streaming
#define AUDIO_FRAME_DURATION_MS 20

// Calculated frame size in samples
// Example: 48000 * 20 / 1000 = 960 samples
#define AUDIO_FRAME_SIZE        ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_DURATION_MS) / 1000)

// ============================================================================
// Opus Encoder Settings
// ============================================================================

// Opus bitrate (bits per second)
// Recommendations:
//   - 64000 (64kbps):  Good quality mono
//   - 96000 (96kbps):  Good quality stereo (libdatachannel default)
//   - 128000 (128kbps): High quality stereo
//   - 256000 (256kbps): Very high quality stereo (current setting)
//   - 510000 (510kbps): Maximum quality stereo
#define OPUS_BITRATE            256000

// Opus complexity (0-10)
// Higher = better quality but more CPU
// 10 = maximum quality
#define OPUS_COMPLEXITY         10

// Opus signal type
// OPUS_SIGNAL_MUSIC = optimize for music (current)
// OPUS_SIGNAL_VOICE = optimize for speech
#define OPUS_SIGNAL_TYPE        OPUS_SIGNAL_MUSIC

// Variable bitrate mode
// 1 = VBR enabled (better quality)
// 0 = CBR (constant bitrate)
#define OPUS_VBR                1

// VBR constraint
// 0 = unconstrained VBR (best quality)
// 1 = constrained VBR (more predictable bitrate)
#define OPUS_VBR_CONSTRAINT     0

// Discontinuous Transmission (silence suppression)
// 0 = disabled (avoid artifacts during quiet passages)
// 1 = enabled (saves bandwidth during silence)
#define OPUS_DTX                0

// Forward Error Correction for packet loss
// 1 = enabled (recommended for WebRTC)
// 0 = disabled
#define OPUS_INBAND_FEC         1

// Expected packet loss percentage (0-100)
// Used to tune FEC aggressiveness
#define OPUS_PACKET_LOSS_PERC   1

// ============================================================================
// WebRTC Profile Settings
// ============================================================================

// Opus RTP payload type (standard is 97)
#define OPUS_PAYLOAD_TYPE       97

// Helper macros for building Opus profile string
// These convert the numeric defines to strings at compile time
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

// Build Opus profile string for WebRTC SDP
// Format: "minptime=10;maxaveragebitrate=256000;stereo=1;sprop-stereo=1;useinbandfec=1"
#define WEBRTC_OPUS_PROFILE \
    "minptime=" TOSTRING(AUDIO_FRAME_DURATION_MS) \
    ";maxaveragebitrate=" TOSTRING(OPUS_BITRATE) \
    ";stereo=" TOSTRING(AUDIO_CHANNELS) \
    ";sprop-stereo=" TOSTRING(AUDIO_CHANNELS) \
    ";useinbandfec=" TOSTRING(OPUS_INBAND_FEC)

// ============================================================================
// Debug Capture Settings
// ============================================================================

// Energy threshold for non-silence detection (sum of absolute sample values)
// Used during MACEMU_AUDIO_CAPTURE debug mode
#define AUDIO_ENERGY_THRESHOLD  100000

// Maximum frames to capture during debug (500 frames = ~10 seconds at 20ms)
#define AUDIO_MAX_CAPTURE_FRAMES 500

#endif // AUDIO_CONFIG_H

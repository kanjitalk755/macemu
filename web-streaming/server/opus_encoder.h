/*
 * Opus Audio Encoder for macemu WebRTC Streaming
 *
 * Handles:
 * - 48kHz audio encoding (matches Mac audio system output)
 * - Dynamic channel changes (mono/stereo)
 * - Low-latency encoding (20ms frames)
 */

#ifndef OPUS_ENCODER_H
#define OPUS_ENCODER_H

#include <opus/opus.h>
#include <vector>
#include <cstdint>
#include "audio_config.h"

class OpusAudioEncoder {
public:
    OpusAudioEncoder();
    ~OpusAudioEncoder();

    // Initialize encoder
    // output_sample_rate: Sample rate in Hz (default from audio_config.h)
    // channels: 1 (mono) or 2 (stereo) (default from audio_config.h)
    // bitrate: Bitrate in bps (default from audio_config.h)
    bool init(int output_sample_rate = AUDIO_SAMPLE_RATE, int channels = AUDIO_CHANNELS, int bitrate = OPUS_BITRATE);

    // Cleanup
    void cleanup();

    // Encode PCM samples (S16LE input)
    std::vector<uint8_t> encode(const int16_t* pcm, int frame_size);

    // Get frame size in samples (from audio_config.h)
    int get_frame_size() const { return frame_size_; }

    // Get encoder sample rate
    int get_sample_rate() const { return sample_rate_; }

    // Get encoder channels
    int get_channels() const { return channels_; }

private:

    OpusEncoder* encoder_ = nullptr;
    int sample_rate_ = AUDIO_SAMPLE_RATE;
    int channels_ = AUDIO_CHANNELS;
    int frame_size_ = AUDIO_FRAME_SIZE;
    int bitrate_ = OPUS_BITRATE;
};

#endif // OPUS_ENCODER_H

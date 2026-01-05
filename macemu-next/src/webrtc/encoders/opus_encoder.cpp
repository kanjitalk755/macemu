/*
 * Opus Audio Encoder Implementation
 */

#include "opus_encoder.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// External debug flag from server.cpp
extern bool g_debug_mode_switch;

OpusAudioEncoder::OpusAudioEncoder() {}

OpusAudioEncoder::~OpusAudioEncoder() {
    cleanup();
}

bool OpusAudioEncoder::init(int output_sample_rate, int channels, int bitrate) {
    sample_rate_ = output_sample_rate;
    channels_ = channels;
    bitrate_ = bitrate;
    frame_size_ = (sample_rate_ * AUDIO_FRAME_DURATION_MS) / 1000;

    int error;
    encoder_ = opus_encoder_create(sample_rate_, channels, OPUS_APPLICATION_AUDIO, &error);

    if (error != OPUS_OK) {
        fprintf(stderr, "[Opus] Failed to create encoder: %s\n", opus_strerror(error));
        return false;
    }

    // Configure for high quality audio streaming (settings from audio_config.h)
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_TYPE));
    opus_encoder_ctl(encoder_, OPUS_SET_VBR(OPUS_VBR));
    opus_encoder_ctl(encoder_, OPUS_SET_VBR_CONSTRAINT(OPUS_VBR_CONSTRAINT));
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(OPUS_DTX));
    opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(OPUS_INBAND_FEC));
    opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(OPUS_PACKET_LOSS_PERC));

    fprintf(stderr, "[Opus] Encoder initialized: %dHz, %d ch, %d bps, frame=%d samples\n",
            sample_rate_, channels_, bitrate, frame_size_);

    return true;
}

void OpusAudioEncoder::cleanup() {
    if (encoder_) {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
}

std::vector<uint8_t> OpusAudioEncoder::encode(const int16_t* pcm, int frame_size) {
    if (!encoder_) {
        fprintf(stderr, "[Opus] Encoder not initialized\n");
        return {};
    }

    // Validate frame size
    if (frame_size != frame_size_) {
        fprintf(stderr, "[Opus] WARNING: Frame size mismatch! Expected %d, got %d\n", frame_size_, frame_size);
    }

    // Debug: Calculate input energy to detect silence/corruption
    static bool debug_enabled = (getenv("MACEMU_DEBUG_AUDIO") != nullptr);
    if (debug_enabled) {
        static int encode_count = 0;
        if (encode_count++ % 50 == 0) {  // Log every 50 frames (~1 second)
            int64_t energy = 0;
            int16_t max_sample = 0;
            int16_t min_sample = 0;
            for (int i = 0; i < frame_size * channels_; i++) {
                int16_t sample = pcm[i];
                energy += abs(sample);
                if (sample > max_sample) max_sample = sample;
                if (sample < min_sample) min_sample = sample;
            }
            fprintf(stderr, "[Opus] Encode #%d: frame_size=%d, energy=%ld, range=[%d, %d]\n",
                    encode_count, frame_size, energy, min_sample, max_sample);
        }
    }

    std::vector<uint8_t> output(4000);  // Max Opus packet size

    int encoded_bytes = opus_encode(encoder_, pcm, frame_size,
                                    output.data(), output.size());

    if (encoded_bytes < 0) {
        fprintf(stderr, "[Opus] Encode error: %s\n", opus_strerror(encoded_bytes));
        return {};
    }

    // Debug: Log encoded packet info
    if (debug_enabled) {
        static int packet_count = 0;
        if (packet_count++ % 50 == 0) {
            fprintf(stderr, "[Opus] Encoded packet #%d: %d bytes (input: %d samples)\n",
                    packet_count, encoded_bytes, frame_size);
        }
    }

    output.resize(encoded_bytes);
    return output;
}



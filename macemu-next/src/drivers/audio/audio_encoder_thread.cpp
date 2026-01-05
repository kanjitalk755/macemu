/*
 * Audio Encoder Thread - In-Process Architecture
 *
 * Reads audio samples from AudioOutput ring buffer and encodes to Opus.
 * Runs at 50 Hz (20ms per frame) for low-latency WebRTC streaming.
 *
 * Thread Safety:
 * - Reads from AudioOutput (mutex-protected ring buffer)
 * - Sends encoded packets to WebRTC (thread-safe queue)
 */

#include "audio_encoder_thread.h"
#include "audio_output.h"
#include "encoders/opus_encoder.h"
#include "encoders/audio_config.h"
#include <thread>

#include <memory>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace audio {

// External globals (to be replaced with proper dependency injection later)
extern std::atomic<bool> g_running;

// Statistics
static std::atomic<uint64_t> g_audio_packets_sent(0);
static std::atomic<uint64_t> g_audio_underruns(0);

/**
 * Send encoded audio packet to WebRTC
 * TODO: Replace with actual WebRTC send function
 */
static void send_encoded_audio(const std::vector<uint8_t>& packet) {
    // TODO: Queue packet for WebRTC thread to send via RTP
    // For now, just count it
    (void)packet;
    g_audio_packets_sent++;
}

/**
 * Audio Encoder Thread Main Loop
 *
 * Reads 20ms chunks of audio at 48kHz, encodes to Opus, sends via WebRTC.
 *
 * @param audio_output Ring buffer to read samples from
 */
void audio_encoder_main(AudioOutput* audio_output) {
    fprintf(stderr, "[AudioEncoder] Thread starting\n");

    // Create Opus encoder (48kHz stereo, 20ms frames)
    OpusAudioEncoder opus_encoder;
    if (!opus_encoder.init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, OPUS_BITRATE)) {
        fprintf(stderr, "[AudioEncoder] FATAL: Failed to initialize Opus encoder\n");
        return;
    }

    int frame_size = opus_encoder.get_frame_size();  // 960 samples for 48kHz @ 20ms
    fprintf(stderr, "[AudioEncoder] Initialized: %d Hz, %d channels, %d samples/frame\n",
            AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, frame_size);

    // Sample buffer (stereo = 2x frame_size)
    std::vector<int16_t> samples(frame_size * AUDIO_CHANNELS);

    // Statistics
    auto last_stats_time = std::chrono::steady_clock::now();
    int packets_since_stats = 0;

    fprintf(stderr, "[AudioEncoder] Entering encoding loop (50 Hz, 20ms per frame)\n");

    while (g_running.load(std::memory_order_relaxed)) {
        auto frame_start = std::chrono::steady_clock::now();

        // Read samples from ring buffer (blocks if not enough available)
        int samples_read = audio_output->read_samples(samples.data(),
                                                      frame_size * AUDIO_CHANNELS,
                                                      20);  // 20ms timeout

        if (samples_read == 0) {
            // Underrun or timeout - send silence
            memset(samples.data(), 0, samples.size() * sizeof(int16_t));
            g_audio_underruns++;
        } else if (samples_read < frame_size * AUDIO_CHANNELS) {
            // Partial read - pad with silence
            memset(&samples[samples_read], 0,
                   (samples.size() - samples_read) * sizeof(int16_t));
        }

        // Encode to Opus
        std::vector<uint8_t> encoded = opus_encoder.encode(samples.data(), frame_size);

        if (encoded.empty()) {
            fprintf(stderr, "[AudioEncoder] WARNING: Opus encoding failed\n");
            continue;
        }

        // Send to WebRTC
        send_encoded_audio(encoded);
        packets_since_stats++;

        // Print statistics every 3 seconds
        auto now = std::chrono::steady_clock::now();
        auto stats_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_stats_time).count();

        if (stats_elapsed >= 3) {
            double pkts_per_sec = packets_since_stats / (double)stats_elapsed;
            uint64_t total_packets = g_audio_packets_sent.load(std::memory_order_relaxed);
            uint64_t total_underruns = g_audio_underruns.load(std::memory_order_relaxed);

            fprintf(stderr, "[AudioEncoder] Stats: %.1f pkt/s, %llu packets, %llu underruns\n",
                    pkts_per_sec, (unsigned long long)total_packets,
                    (unsigned long long)total_underruns);

            last_stats_time = now;
            packets_since_stats = 0;
        }

        // Sleep to maintain 50 Hz (20ms per frame)
        // Calculate time remaining in this 20ms frame
        auto frame_end = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame_end - frame_start).count();

        int sleep_ms = 20 - elapsed_ms;
        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        } else if (elapsed_ms > 25) {
            // Encoding took too long (should never happen with Opus)
            fprintf(stderr, "[AudioEncoder] WARNING: Frame took %ld ms (target 20ms)\n", elapsed_ms);
        }
    }

    fprintf(stderr, "[AudioEncoder] Thread exiting\n");
}

} // namespace audio

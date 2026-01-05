/*
 * Audio Output API - Ring Buffer with Mutex Protection
 *
 * Replaces IPC-based audio transfer with direct in-process ring buffer.
 * Uses mutex for simplicity (audio is not the hot path like video).
 *
 * Design:
 * - CPU thread writes audio samples to ring buffer (non-blocking)
 * - Encoder thread reads samples from ring buffer (may block if buffer empty)
 * - Mutex-protected for thread safety (simpler than lock-free for audio)
 *
 * Based on macemu audio frame ring buffer, simplified for in-process use.
 */

#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdint.h>
#include <mutex>
#include <condition_variable>
#include <atomic>

// Maximum audio parameters
#define AUDIO_MAX_SAMPLE_RATE  48000   // Max: 48kHz (WebRTC standard)
#define AUDIO_MAX_CHANNELS     2       // Max: Stereo

// Ring buffer size (in samples, not bytes)
// 1 second at 48kHz stereo = 96000 samples (192KB)
#define AUDIO_RING_BUFFER_SIZE (AUDIO_MAX_SAMPLE_RATE * AUDIO_MAX_CHANNELS)

// Audio format
enum AudioFormat {
    AUDIO_FORMAT_NONE = 0,      // Audio disabled/muted
    AUDIO_FORMAT_PCM_S16 = 1,   // 16-bit signed PCM (Mac native)
};

/**
 * AudioOutput - Ring buffer for audio samples
 *
 * Producer (CPU thread): Calls submit_samples() to write audio data
 * Consumer (encoder thread): Calls read_samples() to read for encoding
 *
 * Thread safety: Mutex-protected (simple and reliable)
 * Performance: Non-blocking for producer, may block consumer if buffer empty
 */
class AudioOutput {
public:
    /**
     * Constructor
     * @param sample_rate Sample rate (Hz)
     * @param channels Number of channels (1=mono, 2=stereo)
     * @param buffer_size Ring buffer size in samples (default 1 second)
     */
    AudioOutput(int sample_rate = AUDIO_MAX_SAMPLE_RATE,
                int channels = AUDIO_MAX_CHANNELS,
                int buffer_size = AUDIO_RING_BUFFER_SIZE);

    /**
     * Destructor - frees ring buffer
     */
    ~AudioOutput();

    /**
     * Submit audio samples (called by CPU thread)
     *
     * Writes samples to ring buffer. If buffer is full, oldest samples are dropped.
     * Never blocks - emulator audio thread must not stall.
     *
     * @param samples Audio data (16-bit signed PCM)
     * @param count Number of samples (NOT frames - stereo = 2 samples per frame)
     * @param sample_rate Sample rate (Hz) - may change dynamically
     * @param channels Channels (1 or 2) - may change dynamically
     * @return Number of samples actually written
     */
    int submit_samples(const int16_t* samples, int count, int sample_rate, int channels);

    /**
     * Read audio samples (called by encoder thread)
     *
     * Reads samples from ring buffer. Blocks if not enough samples available.
     *
     * @param out Output buffer (must hold max_samples)
     * @param max_samples Maximum samples to read
     * @param timeout_ms Timeout in milliseconds (0 = no wait, -1 = infinite)
     * @return Number of samples read (may be less than requested on timeout)
     */
    int read_samples(int16_t* out, int max_samples, int timeout_ms = -1);

    /**
     * Get current audio format (thread-safe)
     *
     * @param out_sample_rate Output: Current sample rate
     * @param out_channels Output: Current channels
     */
    void get_format(int* out_sample_rate, int* out_channels);

    /**
     * Get available samples (non-blocking query)
     *
     * @return Number of samples currently available to read
     */
    int get_available();

    /**
     * Get buffer statistics
     *
     * @param out_total_samples Total samples submitted
     * @param out_dropped_samples Samples dropped due to buffer overflow
     * @param out_underruns Number of buffer underruns
     */
    void get_stats(uint64_t* out_total_samples, uint64_t* out_dropped_samples, uint64_t* out_underruns);

    /**
     * Clear the ring buffer (flush all samples)
     */
    void clear();

    /**
     * Signal shutdown (wake up waiting encoder thread)
     */
    void shutdown();

private:
    // Ring buffer
    int16_t* ring_buffer;       // Circular buffer (allocated in constructor)
    int capacity;               // Total samples (sample_rate * channels)
    int write_pos;              // Where producer writes (0 to capacity-1)
    int read_pos;               // Where consumer reads (0 to capacity-1)

    // Audio format (may change dynamically)
    std::atomic<int> current_sample_rate;
    std::atomic<int> current_channels;

    // Statistics
    std::atomic<uint64_t> total_samples;      // Total samples written
    std::atomic<uint64_t> dropped_samples;    // Samples dropped (overflow)
    std::atomic<uint64_t> underruns;          // Buffer underruns

    // Synchronization
    std::mutex mutex;
    std::condition_variable cv;

    // Shutdown flag
    std::atomic<bool> shutdown_requested;

    // Helper: Calculate available samples (assumes mutex is held)
    int available_unlocked() const;
};

#endif // AUDIO_OUTPUT_H

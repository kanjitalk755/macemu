/*
 * Audio Output Implementation - Ring Buffer with Mutex
 */

#include "audio_output.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <algorithm>

AudioOutput::AudioOutput(int sample_rate, int channels, int buffer_size)
    : ring_buffer(nullptr)
    , capacity(buffer_size)
    , write_pos(0)
    , read_pos(0)
    , current_sample_rate(sample_rate)
    , current_channels(channels)
    , total_samples(0)
    , dropped_samples(0)
    , underruns(0)
    , shutdown_requested(false)
{
    // Allocate ring buffer
    ring_buffer = (int16_t*)malloc(capacity * sizeof(int16_t));
    if (!ring_buffer) {
        fprintf(stderr, "[AudioOutput] Failed to allocate ring buffer (%d samples = %zu bytes)\n",
                capacity, capacity * sizeof(int16_t));
        abort();
    }

    // Clear buffer to silence
    memset(ring_buffer, 0, capacity * sizeof(int16_t));
}

AudioOutput::~AudioOutput() {
    if (ring_buffer) {
        free(ring_buffer);
        ring_buffer = nullptr;
    }
}

int AudioOutput::submit_samples(const int16_t* samples, int count, int sample_rate, int channels) {
    if (count <= 0 || !samples) {
        return 0;
    }

    // Update current format (atomic)
    current_sample_rate.store(sample_rate, std::memory_order_relaxed);
    current_channels.store(channels, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mutex);

    // Calculate available space
    int available_space = capacity - available_unlocked();

    // If not enough space, drop oldest samples (ring buffer overflow)
    int samples_to_write = count;
    if (samples_to_write > available_space) {
        int overflow = samples_to_write - available_space;
        dropped_samples.fetch_add(overflow, std::memory_order_relaxed);

        // Advance read pointer to make space (drops oldest samples)
        read_pos = (read_pos + overflow) % capacity;
    }

    // Write samples to ring buffer (may wrap around)
    int first_chunk = std::min(samples_to_write, capacity - write_pos);
    memcpy(&ring_buffer[write_pos], samples, first_chunk * sizeof(int16_t));

    if (first_chunk < samples_to_write) {
        // Wrapped around - write remainder at beginning
        int second_chunk = samples_to_write - first_chunk;
        memcpy(&ring_buffer[0], &samples[first_chunk], second_chunk * sizeof(int16_t));
    }

    // Update write position
    write_pos = (write_pos + samples_to_write) % capacity;

    // Update statistics
    total_samples.fetch_add(samples_to_write, std::memory_order_relaxed);

    // Notify waiting consumer
    cv.notify_one();

    return samples_to_write;
}

int AudioOutput::read_samples(int16_t* out, int max_samples, int timeout_ms) {
    if (max_samples <= 0 || !out) {
        return 0;
    }

    std::unique_lock<std::mutex> lock(mutex);

    // Wait for enough samples to be available
    if (timeout_ms < 0) {
        // Infinite wait
        cv.wait(lock, [this, max_samples]() {
            return available_unlocked() >= max_samples || shutdown_requested.load(std::memory_order_acquire);
        });
    } else if (timeout_ms > 0) {
        // Timed wait
        cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, max_samples]() {
            return available_unlocked() >= max_samples || shutdown_requested.load(std::memory_order_acquire);
        });
    }
    // If timeout_ms == 0, don't wait - just read what's available

    // Check for shutdown
    if (shutdown_requested.load(std::memory_order_acquire)) {
        return 0;
    }

    // Read available samples (may be less than requested)
    int available = available_unlocked();
    int samples_to_read = std::min(max_samples, available);

    if (samples_to_read == 0) {
        // Buffer underrun
        underruns.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Read samples from ring buffer (may wrap around)
    int first_chunk = std::min(samples_to_read, capacity - read_pos);
    memcpy(out, &ring_buffer[read_pos], first_chunk * sizeof(int16_t));

    if (first_chunk < samples_to_read) {
        // Wrapped around - read remainder from beginning
        int second_chunk = samples_to_read - first_chunk;
        memcpy(&out[first_chunk], &ring_buffer[0], second_chunk * sizeof(int16_t));
    }

    // Update read position
    read_pos = (read_pos + samples_to_read) % capacity;

    return samples_to_read;
}

void AudioOutput::get_format(int* out_sample_rate, int* out_channels) {
    if (out_sample_rate) {
        *out_sample_rate = current_sample_rate.load(std::memory_order_relaxed);
    }
    if (out_channels) {
        *out_channels = current_channels.load(std::memory_order_relaxed);
    }
}

int AudioOutput::get_available() {
    std::lock_guard<std::mutex> lock(mutex);
    return available_unlocked();
}

void AudioOutput::get_stats(uint64_t* out_total_samples, uint64_t* out_dropped_samples, uint64_t* out_underruns) {
    if (out_total_samples) {
        *out_total_samples = total_samples.load(std::memory_order_relaxed);
    }
    if (out_dropped_samples) {
        *out_dropped_samples = dropped_samples.load(std::memory_order_relaxed);
    }
    if (out_underruns) {
        *out_underruns = underruns.load(std::memory_order_relaxed);
    }
}

void AudioOutput::clear() {
    std::lock_guard<std::mutex> lock(mutex);

    // Reset pointers
    write_pos = 0;
    read_pos = 0;

    // Clear buffer to silence
    memset(ring_buffer, 0, capacity * sizeof(int16_t));
}

void AudioOutput::shutdown() {
    shutdown_requested.store(true, std::memory_order_release);
    cv.notify_all();  // Wake up all waiting threads
}

int AudioOutput::available_unlocked() const {
    // Calculate number of samples available to read
    if (write_pos >= read_pos) {
        return write_pos - read_pos;
    } else {
        return capacity - (read_pos - write_pos);
    }
}

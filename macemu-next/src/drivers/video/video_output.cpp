/*
 * Video Output Implementation - Lock-free Triple Buffer
 */

#include "video_output.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

VideoOutput::VideoOutput(int max_width, int max_height)
    : write_index(0)
    , ready_index(0)
    , frame_count(0)
    , dropped_frames(0)
    , last_read_sequence(0)
    , shutdown_requested(false)
    , max_width(max_width)
    , max_height(max_height)
{
    allocate_buffers();
}

VideoOutput::~VideoOutput() {
    free_buffers();
}

void VideoOutput::allocate_buffers() {
    size_t buffer_size = max_width * max_height * 4;  // 4 bytes per pixel (BGRA/ARGB)

    for (int i = 0; i < VIDEO_NUM_BUFFERS; i++) {
        buffers[i].pixels = (uint32_t*)malloc(buffer_size);
        if (!buffers[i].pixels) {
            // Fatal: Out of memory
            fprintf(stderr, "[VideoOutput] Failed to allocate buffer %d (%zu bytes)\n", i, buffer_size);
            abort();
        }

        // Initialize metadata
        buffers[i].width = 0;
        buffers[i].height = 0;
        buffers[i].format = PIXFMT_BGRA;
        buffers[i].sequence = 0;
        buffers[i].timestamp_us = 0;
        buffers[i].dirty_x = 0;
        buffers[i].dirty_y = 0;
        buffers[i].dirty_width = 0;
        buffers[i].dirty_height = 0;
        buffers[i].cursor_x = 0;
        buffers[i].cursor_y = 0;
        buffers[i].cursor_visible = 0;

        // Clear buffer to black
        memset(buffers[i].pixels, 0, buffer_size);
    }
}

void VideoOutput::free_buffers() {
    for (int i = 0; i < VIDEO_NUM_BUFFERS; i++) {
        if (buffers[i].pixels) {
            free(buffers[i].pixels);
            buffers[i].pixels = nullptr;
        }
    }
}

void VideoOutput::submit_frame(const uint32_t* pixels, int width, int height, PixelFormat format) {
    // Full frame update (no dirty rect)
    submit_frame_dirty(pixels, width, height, format, 0, 0, width, height);
}

void VideoOutput::submit_frame_dirty(const uint32_t* pixels, int width, int height, PixelFormat format,
                                     uint32_t dirty_x, uint32_t dirty_y,
                                     uint32_t dirty_width, uint32_t dirty_height) {
    // Validate dimensions
    if (width <= 0 || width > max_width || height <= 0 || height > max_height) {
        fprintf(stderr, "[VideoOutput] Invalid frame dimensions: %dx%d (max %dx%d)\n",
                width, height, max_width, max_height);
        return;
    }

    // Get current write buffer (atomic acquire)
    int idx = write_index.load(std::memory_order_acquire);

    // Update metadata
    FrameBuffer* buf = &buffers[idx];
    buf->width = width;
    buf->height = height;
    buf->format = format;
    buf->dirty_x = dirty_x;
    buf->dirty_y = dirty_y;
    buf->dirty_width = dirty_width;
    buf->dirty_height = dirty_height;

    // Copy pixel data
    size_t frame_size = width * height * 4;  // 4 bytes per pixel
    memcpy(buf->pixels, pixels, frame_size);

    // Update sequence and timestamp
    buf->sequence = frame_count.load(std::memory_order_relaxed) + 1;

    // Get current time (microseconds since epoch)
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    buf->timestamp_us = micros;

    // Advance write index (circular, atomic release)
    // This makes the frame visible to the encoder thread
    int next_idx = (idx + 1) % VIDEO_NUM_BUFFERS;
    write_index.store(next_idx, std::memory_order_release);

    // Update ready index to point to the just-completed frame
    // This is where the encoder should read from
    ready_index.store(idx, std::memory_order_release);

    // Increment frame count
    frame_count.fetch_add(1, std::memory_order_relaxed);
}

void VideoOutput::set_cursor(uint16_t x, uint16_t y, bool visible) {
    // Update cursor position in current write buffer
    int idx = write_index.load(std::memory_order_acquire);
    buffers[idx].cursor_x = x;
    buffers[idx].cursor_y = y;
    buffers[idx].cursor_visible = visible ? 1 : 0;
}

const FrameBuffer* VideoOutput::wait_for_frame(int timeout_ms) {
    // Check for shutdown
    if (shutdown_requested.load(std::memory_order_acquire)) {
        return nullptr;
    }

    // Get ready index (atomic acquire - ensures we see all writes to the buffer)
    int idx = ready_index.load(std::memory_order_acquire);
    const FrameBuffer* buf = &buffers[idx];

    // Check if this is a new frame (sequence number increased)
    if (buf->sequence > last_read_sequence) {
        // New frame available!
        return buf;
    }

    // No new frame yet - wait strategy depends on timeout
    if (timeout_ms == 0) {
        // No wait - return immediately
        return nullptr;
    }

    if (timeout_ms < 0) {
        // Infinite wait - poll with short sleep
        // Note: In production, this could use a condition variable or eventfd
        // For simplicity, we use polling with 1ms sleep
        while (!shutdown_requested.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            // Re-check ready index
            idx = ready_index.load(std::memory_order_acquire);
            buf = &buffers[idx];

            if (buf->sequence > last_read_sequence) {
                return buf;
            }
        }
        return nullptr;  // Shutdown
    } else {
        // Timed wait - poll with short sleep until timeout
        auto start = std::chrono::steady_clock::now();
        while (!shutdown_requested.load(std::memory_order_acquire)) {
            // Re-check ready index
            idx = ready_index.load(std::memory_order_acquire);
            buf = &buffers[idx];

            if (buf->sequence > last_read_sequence) {
                return buf;
            }

            // Check timeout
            auto elapsed = std::chrono::steady_clock::now() - start;
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            if (elapsed_ms >= timeout_ms) {
                return nullptr;  // Timeout
            }

            // Sleep for 1ms
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return nullptr;  // Shutdown
    }
}

void VideoOutput::release_frame() {
    // Update last read sequence to mark this frame as consumed
    int idx = ready_index.load(std::memory_order_acquire);
    const FrameBuffer* buf = &buffers[idx];
    last_read_sequence = buf->sequence;
}

void VideoOutput::get_stats(uint64_t* out_total_frames, uint64_t* out_dropped_frames) {
    if (out_total_frames) {
        *out_total_frames = frame_count.load(std::memory_order_relaxed);
    }
    if (out_dropped_frames) {
        *out_dropped_frames = dropped_frames.load(std::memory_order_relaxed);
    }
}

void VideoOutput::shutdown() {
    shutdown_requested.store(true, std::memory_order_release);
}

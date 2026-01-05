/*
 * Video Output API - In-Process Triple Buffer
 *
 * Replaces IPC-based shared memory with direct in-process communication.
 * Lock-free triple buffering for high-performance video streaming.
 *
 * Design:
 * - CPU thread writes frames to triple buffer (never blocks)
 * - Encoder thread reads frames from triple buffer (may drop frames if encoding is slow)
 * - Atomic operations ensure thread safety without locks
 *
 * Based on macemu IPC protocol v4 triple buffer design, adapted for in-process use.
 */

#ifndef VIDEO_OUTPUT_H
#define VIDEO_OUTPUT_H

#include <stdint.h>
#include <atomic>
#include <cstring>

// Maximum supported resolution (1080p)
#define VIDEO_MAX_WIDTH  1920
#define VIDEO_MAX_HEIGHT 1080

// Number of buffers for triple buffering
#define VIDEO_NUM_BUFFERS 3

// Pixel format flags
enum PixelFormat {
    PIXFMT_ARGB = 0,   // Mac native 32-bit: bytes A,R,G,B
    PIXFMT_BGRA = 1,   // Converted: bytes B,G,R,A
};

/**
 * Single frame buffer with metadata
 */
struct FrameBuffer {
    uint32_t* pixels;       // Pixel data (BGRA or ARGB format)
    int width;              // Actual frame width (≤ VIDEO_MAX_WIDTH)
    int height;             // Actual frame height (≤ VIDEO_MAX_HEIGHT)
    PixelFormat format;     // Pixel format
    uint64_t sequence;      // Frame counter (monotonic)
    uint64_t timestamp_us;  // Timestamp when frame was completed (microseconds)

    // Dirty rectangle optimization (for PNG encoder)
    uint32_t dirty_x;
    uint32_t dirty_y;
    uint32_t dirty_width;
    uint32_t dirty_height;

    // Cursor overlay information
    uint16_t cursor_x;
    uint16_t cursor_y;
    uint8_t cursor_visible;
};

/**
 * VideoOutput - Lock-free triple buffer for video frames
 *
 * Producer (CPU thread): Calls submit_frame() to write new frames
 * Consumer (encoder thread): Calls wait_for_frame() / release_frame() to read
 *
 * Thread safety: Lock-free using atomic operations
 * Performance: CPU thread never blocks, encoder may drop frames if slow
 */
class VideoOutput {
public:
    /**
     * Constructor
     * @param max_width Maximum frame width (default 1920)
     * @param max_height Maximum frame height (default 1080)
     */
    VideoOutput(int max_width = VIDEO_MAX_WIDTH, int max_height = VIDEO_MAX_HEIGHT);

    /**
     * Destructor - frees frame buffers
     */
    ~VideoOutput();

    /**
     * Submit a new frame (called by CPU thread)
     *
     * Copies pixel data to the write buffer and advances write index.
     * Never blocks - if encoder is slow, middle frame may be overwritten.
     *
     * @param pixels Pixel data (width * height * 4 bytes)
     * @param width Frame width
     * @param height Frame height
     * @param format Pixel format (ARGB or BGRA)
     */
    void submit_frame(const uint32_t* pixels, int width, int height, PixelFormat format);

    /**
     * Submit a frame with dirty rectangle optimization
     *
     * @param pixels Pixel data
     * @param width Frame width
     * @param height Frame height
     * @param format Pixel format
     * @param dirty_x Dirty rectangle X
     * @param dirty_y Dirty rectangle Y
     * @param dirty_width Dirty rectangle width
     * @param dirty_height Dirty rectangle height
     */
    void submit_frame_dirty(const uint32_t* pixels, int width, int height, PixelFormat format,
                           uint32_t dirty_x, uint32_t dirty_y,
                           uint32_t dirty_width, uint32_t dirty_height);

    /**
     * Set cursor position (called by CPU thread)
     *
     * @param x Cursor X position
     * @param y Cursor Y position
     * @param visible Whether cursor should be shown
     */
    void set_cursor(uint16_t x, uint16_t y, bool visible);

    /**
     * Wait for a new frame (called by encoder thread)
     *
     * Blocks until a new frame is available or timeout.
     * Returns pointer to the ready buffer (valid until release_frame() is called).
     *
     * @param timeout_ms Timeout in milliseconds (0 = no wait, -1 = infinite)
     * @return Pointer to frame buffer, or nullptr if timeout/shutdown
     */
    const FrameBuffer* wait_for_frame(int timeout_ms = -1);

    /**
     * Release the current frame (called by encoder thread)
     *
     * Must be called after wait_for_frame() to allow the buffer to be reused.
     */
    void release_frame();

    /**
     * Get frame statistics
     *
     * @param out_total_frames Total frames submitted
     * @param out_dropped_frames Frames dropped due to slow encoding
     */
    void get_stats(uint64_t* out_total_frames, uint64_t* out_dropped_frames);

    /**
     * Signal shutdown (wake up waiting encoder thread)
     */
    void shutdown();

private:
    // Frame buffers (triple buffer)
    FrameBuffer buffers[VIDEO_NUM_BUFFERS];

    // Buffer indices (atomic for lock-free operation)
    std::atomic<int> write_index;   // CPU writes here (0-2)
    std::atomic<int> ready_index;   // Encoder reads here (0-2)

    // Frame statistics
    std::atomic<uint64_t> frame_count;       // Total frames submitted
    std::atomic<uint64_t> dropped_frames;    // Frames dropped

    // Last read sequence (for detecting new frames)
    uint64_t last_read_sequence;

    // Shutdown flag
    std::atomic<bool> shutdown_requested;

    // Maximum dimensions
    int max_width;
    int max_height;

    // Helper: Allocate pixel buffers
    void allocate_buffers();

    // Helper: Free pixel buffers
    void free_buffers();
};

#endif // VIDEO_OUTPUT_H

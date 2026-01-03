/*
 * Video Codec Abstraction
 *
 * Allows switching between different encoding strategies:
 * - H.264 via OpenH264 (WebRTC video track)
 * - AV1 via SVT-AV1 (WebRTC video track, best for dithered content)
 * - PNG for dithered content (DataChannel binary, supports dirty rects)
 */

#ifndef CODEC_H
#define CODEC_H

#include <vector>
#include <cstdint>

enum class CodecType {
    H264,       // WebRTC video track with H.264
    AV1,        // WebRTC video track with AV1 (best for dithered content)
    VP9,        // WebRTC video track with VP9 (great for UI/screen content)
    PNG,        // PNG over DataChannel (good for dithered, supports dirty rects)
    WEBP        // WebP over DataChannel (faster encoding than PNG, supports dirty rects)
};

struct EncodedFrame {
    std::vector<uint8_t> data;
    bool is_keyframe = false;
    CodecType codec = CodecType::H264;
    int width = 0;
    int height = 0;

    // Cursor position metadata (sent with every frame for all codecs)
    uint16_t cursor_x = 0;
    uint16_t cursor_y = 0;
    uint8_t cursor_visible = 0;

    // Ping/pong latency measurement (sent with every frame for all codecs)
    // Complete round-trip timestamps:
    // DOWN: Browser → Server → Emulator → Frame ready
    // UP: Frame read → Encode → Send → Browser receive
    uint32_t ping_sequence = 0;      // Sequence number (0 = no ping data)
    uint64_t t1_browser_ms = 0;      // Browser send time (performance.now())
    uint64_t t2_server_us = 0;       // Server receive time (CLOCK_REALTIME microseconds)
    uint64_t t3_emulator_us = 0;     // Emulator receive time (CLOCK_REALTIME microseconds)
    uint64_t t4_frame_us = 0;        // Frame ready in SHM (CLOCK_REALTIME microseconds)
    uint64_t t5_server_read_us = 0;  // Server woke & read frame (CLOCK_REALTIME microseconds)
    uint64_t t6_encode_done_us = 0;  // Encoding finished (CLOCK_REALTIME microseconds)
    uint64_t t7_server_send_us = 0;  // Server sending frame (CLOCK_REALTIME microseconds)
};

class VideoCodec {
public:
    virtual ~VideoCodec() = default;

    // Get codec type
    virtual CodecType type() const = 0;

    // Get codec name for display/logging
    virtual const char* name() const = 0;

    // Initialize with resolution and parameters
    virtual bool init(int width, int height, int fps = 30) = 0;

    // Cleanup resources
    virtual void cleanup() = 0;

    // Encode a frame from I420 data (from SHM)
    virtual EncodedFrame encode_i420(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                     int width, int height, int y_stride, int uv_stride) = 0;

    // Encode from BGRA (for codecs that prefer RGB input)
    virtual EncodedFrame encode_bgra(const uint8_t* bgra, int width, int height, int stride) {
        (void)bgra; (void)width; (void)height; (void)stride;
        return EncodedFrame{};  // Default: not supported
    }

    // Request keyframe on next encode
    virtual void request_keyframe() = 0;
};

#endif // CODEC_H

/*
 * PNG Encoder using fpng (fast PNG)
 * Optimized for real-time encoding of 1-bit dithered Mac content
 * fpng is ~10x faster than libpng for encoding
 */

#ifndef PNG_ENCODER_H
#define PNG_ENCODER_H

#include "codec.h"
#include <vector>

class PNGEncoder : public VideoCodec {
public:
    PNGEncoder() = default;
    ~PNGEncoder() override { cleanup(); }

    CodecType type() const override { return CodecType::PNG; }
    const char* name() const override { return "PNG"; }

    bool init(int width, int height, int fps = 30) override;
    void cleanup() override;

    // PNG encodes from I420 by first converting to RGB
    EncodedFrame encode_i420(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                             int width, int height, int y_stride, int uv_stride) override;

    // Encode from BGRA (bytes B,G,R,A - libyuv "ARGB")
    EncodedFrame encode_bgra(const uint8_t* bgra, int width, int height, int stride) override;

    // Encode from ARGB (bytes A,R,G,B - libyuv "BGRA", Mac native 32-bit)
    EncodedFrame encode_argb(const uint8_t* argb, int width, int height, int stride);

    // Encode a sub-rectangle from BGRA frame (for dirty rect optimization)
    EncodedFrame encode_bgra_rect(const uint8_t* bgra, int frame_width, int frame_height, int stride,
                                   int rect_x, int rect_y, int rect_width, int rect_height);

    void request_keyframe() override {
        // PNG frames are always keyframes (no inter-frame compression)
    }

private:
    // Convert I420 to RGB for PNG encoding
    void i420_to_rgb(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                     int width, int height, int y_stride, int uv_stride);

    // Encode RGB buffer to PNG
    bool encode_rgb_to_png(int width, int height);

    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;

    // Working buffers
    std::vector<uint8_t> rgb_buffer_;    // RGB24 conversion buffer
    std::vector<uint8_t> png_buffer_;    // Output PNG buffer

    // Stats
    int frame_count_ = 0;
    int64_t total_size_ = 0;
};

#endif // PNG_ENCODER_H

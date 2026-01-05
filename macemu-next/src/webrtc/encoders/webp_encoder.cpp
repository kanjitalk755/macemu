/*
 * WebP Encoder using libwebp (lossless mode)
 * libwebp provides fast lossless encoding suitable for screen content
 * Using fastest preset (0) for minimum latency
 */

#include "webp_encoder.h"
#include <webp/encode.h>
#include <cstring>
#include <cstdio>

// libyuv for fast I420 to RGB conversion
#include <libyuv.h>

// Debug flags (from server.cpp)
extern bool g_debug_mode_switch;
extern bool g_debug_png;  // Reuse PNG debug flag for still-image codecs

bool WebPEncoder::init(int width, int height, int fps) {
    cleanup();

    width_ = width;
    height_ = height;
    fps_ = fps;

    // Pre-allocate RGB buffer
    rgb_buffer_.resize(width * height * 3);  // RGB24

    if (g_debug_png) {
        fprintf(stderr, "WebP: Encoder initialized %dx%d\n", width, height);
    }
    return true;
}

void WebPEncoder::cleanup() {
    rgb_buffer_.clear();
    width_ = 0;
    height_ = 0;
    frame_count_ = 0;
    total_size_ = 0;
}

void WebPEncoder::i420_to_rgb(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                               int width, int height, int y_stride, int uv_stride) {
    // Use libyuv for fast conversion
    // WebP expects RGB (R first), so use I420ToRAW
    libyuv::I420ToRAW(
        y, y_stride,
        u, uv_stride,
        v, uv_stride,
        rgb_buffer_.data(), width * 3,
        width, height
    );
}

bool WebPEncoder::encode_rgb_to_webp(const uint8_t* rgb, int width, int height, std::vector<uint8_t>& output) {
    output.clear();

    // WebP lossless encoding with fastest preset for lowest latency
    // method=0: fastest (vs method=6: slowest/best compression)
    // For LAN/localhost streaming, speed >> compression ratio
    WebPConfig config;
    if (!WebPConfigInit(&config)) {
        if (g_debug_png) {
            fprintf(stderr, "WebP: Failed to initialize config\n");
        }
        return false;
    }

    // Lossless mode with fastest encoding
    config.lossless = 1;           // Lossless compression
    config.method = 0;             // Fastest encoding (0-6, default 4)
    config.quality = 75;           // For lossless, this controls compression effort (not visual quality)

    if (!WebPValidateConfig(&config)) {
        if (g_debug_png) {
            fprintf(stderr, "WebP: Invalid config\n");
        }
        return false;
    }

    // Encode RGB to WebP
    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);

    WebPPicture picture;
    if (!WebPPictureInit(&picture)) {
        if (g_debug_png) {
            fprintf(stderr, "WebP: Failed to initialize picture\n");
        }
        return false;
    }

    picture.use_argb = 1;  // Required for lossless
    picture.width = width;
    picture.height = height;
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;

    // Import RGB data
    if (!WebPPictureImportRGB(&picture, rgb, width * 3)) {
        if (g_debug_png) {
            fprintf(stderr, "WebP: Failed to import RGB\n");
        }
        WebPPictureFree(&picture);
        return false;
    }

    // Encode
    if (!WebPEncode(&config, &picture)) {
        if (g_debug_png) {
            fprintf(stderr, "WebP: Encoding failed (error code: %d)\n", picture.error_code);
        }
        WebPPictureFree(&picture);
        WebPMemoryWriterClear(&writer);
        return false;
    }

    // Copy to output vector
    output.assign(writer.mem, writer.mem + writer.size);

    // Cleanup
    WebPPictureFree(&picture);
    WebPMemoryWriterClear(&writer);

    return true;
}

EncodedFrame WebPEncoder::encode_i420(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                       int width, int height, int y_stride, int uv_stride) {
    EncodedFrame result;
    result.codec = CodecType::WEBP;
    result.width = width;
    result.height = height;
    result.is_keyframe = true;  // WebP frames are always keyframes

    // Reinit if size changed
    if (width != width_ || height != height_) {
        if (!init(width, height)) {
            return result;
        }
    }

    // Convert I420 to RGB
    i420_to_rgb(y, u, v, width, height, y_stride, uv_stride);

    // Encode to WebP
    if (!encode_rgb_to_webp(rgb_buffer_.data(), width, height, result.data)) {
        return result;
    }

    // Stats
    frame_count_++;
    total_size_ += result.data.size();

    // Log every 30 frames (only if debug enabled)
    if (g_debug_png && frame_count_ % 30 == 0) {
        float avg_size = static_cast<float>(total_size_) / frame_count_;
        fprintf(stderr, "WebP: frame=%d size=%zu bytes (avg %.1f KB)\n",
                frame_count_, result.data.size(), avg_size / 1024.0f);
    }

    return result;
}

EncodedFrame WebPEncoder::encode_bgra(const uint8_t* bgra, int width, int height, int stride) {
    EncodedFrame result;
    result.codec = CodecType::WEBP;
    result.width = width;
    result.height = height;
    result.is_keyframe = true;

    // Reinit if size changed
    if (width != width_ || height != height_) {
        if (!init(width, height)) {
            return result;
        }
    }

    // Convert BGRA to RGB using libyuv
    // BGRA = bytes B,G,R,A = libyuv "ARGB"
    // ARGBToRAW converts to RGB
    libyuv::ARGBToRAW(
        bgra, stride,
        rgb_buffer_.data(), width * 3,
        width, height
    );

    // Encode to WebP
    if (!encode_rgb_to_webp(rgb_buffer_.data(), width, height, result.data)) {
        return result;
    }

    frame_count_++;
    total_size_ += result.data.size();

    return result;
}

EncodedFrame WebPEncoder::encode_argb(const uint8_t* argb, int width, int height, int stride) {
    EncodedFrame result;
    result.codec = CodecType::WEBP;
    result.width = width;
    result.height = height;
    result.is_keyframe = true;

    // Reinit if size changed
    if (width != width_ || height != height_) {
        if (!init(width, height)) {
            return result;
        }
    }

    // Convert ARGB to RGB manually
    // ARGB = bytes A,R,G,B in memory (Mac native 32-bit)
    // RGB = bytes R,G,B in memory
    uint8_t* dst = rgb_buffer_.data();
    for (int row = 0; row < height; row++) {
        const uint8_t* src_row = argb + row * stride;
        uint8_t* dst_row = dst + row * width * 3;
        for (int col = 0; col < width; col++) {
            // ARGB bytes: A, R, G, B at offsets 0, 1, 2, 3
            dst_row[col * 3 + 0] = src_row[col * 4 + 1];  // R
            dst_row[col * 3 + 1] = src_row[col * 4 + 2];  // G
            dst_row[col * 3 + 2] = src_row[col * 4 + 3];  // B
        }
    }

    // Encode to WebP
    if (!encode_rgb_to_webp(rgb_buffer_.data(), width, height, result.data)) {
        return result;
    }

    frame_count_++;
    total_size_ += result.data.size();

    return result;
}

EncodedFrame WebPEncoder::encode_bgra_rect(const uint8_t* bgra, int frame_width, int frame_height, int stride,
                                            int rect_x, int rect_y, int rect_width, int rect_height) {
    EncodedFrame result;
    result.codec = CodecType::WEBP;
    result.width = rect_width;
    result.height = rect_height;
    result.is_keyframe = true;

    // Allocate RGB buffer for the rectangle
    std::vector<uint8_t> rect_rgb(rect_width * rect_height * 3);

    // Extract and convert the rectangle from BGRA to RGB
    for (int y = 0; y < rect_height; y++) {
        const uint8_t* src_row = bgra + (rect_y + y) * stride + rect_x * 4;
        uint8_t* dst_row = rect_rgb.data() + y * rect_width * 3;

        for (int x = 0; x < rect_width; x++) {
            // BGRA = bytes B,G,R,A at offsets 0,1,2,3
            // RGB = bytes R,G,B
            dst_row[x * 3 + 0] = src_row[x * 4 + 2];  // R
            dst_row[x * 3 + 1] = src_row[x * 4 + 1];  // G
            dst_row[x * 3 + 2] = src_row[x * 4 + 0];  // B
        }
    }

    // Encode the rectangle to WebP
    if (!encode_rgb_to_webp(rect_rgb.data(), rect_width, rect_height, result.data)) {
        if (g_debug_png) {
            fprintf(stderr, "WebP: encode_rect failed\n");
        }
        return result;
    }

    frame_count_++;
    total_size_ += result.data.size();

    return result;
}

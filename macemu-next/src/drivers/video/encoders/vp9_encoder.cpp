/*
 * VP9 Encoder using libvpx
 * Optimized for screen content and UI rendering
 */

#include "vp9_encoder.h"
#include <cstdio>
#include <cstring>
#include <libyuv.h>

// Debug flags (from server.cpp)
extern bool g_debug_mode_switch;

bool VP9Encoder::init(int width, int height, int fps) {
    cleanup();

    // Get default VP9 encoder configuration
    vpx_codec_iface_t* interface = vpx_codec_vp9_cx();
    vpx_codec_err_t res = vpx_codec_enc_config_default(interface, &config_, 0);
    if (res != VPX_CODEC_OK) {
        fprintf(stderr, "VP9: Failed to get default config (error %d)\n", res);
        return false;
    }

    // Configure encoder for low-latency real-time encoding
    config_.g_w = width;
    config_.g_h = height;
    config_.g_timebase.num = 1;
    config_.g_timebase.den = fps;
    config_.g_threads = 4;  // Use 4 threads for real-time encoding

    // Rate control: CQ mode (Constant Quality) for predictable latency
    config_.rc_end_usage = VPX_CQ;  // Constant Quality mode
    config_.rc_target_bitrate = 2000;  // Target 2 Mbps (not strictly enforced in CQ mode)
    config_.rc_min_quantizer = 4;   // Min Q (0 = lossless, 63 = worst)
    config_.rc_max_quantizer = 48;  // Max Q (allows quality adaptation)
    config_.rc_undershoot_pct = 100;
    config_.rc_overshoot_pct = 100;
    config_.rc_buf_sz = 1000;       // 1 second buffer
    config_.rc_buf_initial_sz = 500;
    config_.rc_buf_optimal_sz = 600;

    // Low-latency settings
    config_.g_lag_in_frames = 0;    // No lookahead - lowest latency
    config_.kf_mode = VPX_KF_AUTO;  // Auto keyframe insertion
    config_.kf_min_dist = 0;
    config_.kf_max_dist = fps * 5;  // Max keyframe interval: 5 seconds
    config_.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;  // Error resilience for WebRTC

    // Quality/speed tradeoffs for real-time
    config_.g_pass = VPX_RC_ONE_PASS;  // Single-pass encoding (real-time)
    config_.g_profile = 0;  // Profile 0 (8-bit 4:2:0)

    // Initialize encoder
    res = vpx_codec_enc_init(&encoder_, interface, &config_, 0);
    if (res != VPX_CODEC_OK) {
        fprintf(stderr, "VP9: Failed to initialize encoder (error %d: %s)\n",
                res, vpx_codec_error_detail(&encoder_));
        return false;
    }

    // VP9-specific encoder controls for screen content optimization
    // Set encoding speed: 5-8 is good for real-time (higher = faster but lower quality)
    vpx_codec_control(&encoder_, VP8E_SET_CPUUSED, 6);

    // Enable screen content mode for better UI/text encoding
    vpx_codec_control(&encoder_, VP9E_SET_TUNE_CONTENT, VP9E_CONTENT_SCREEN);

    // Enable row-based multithreading
    vpx_codec_control(&encoder_, VP9E_SET_ROW_MT, 1);

    // Set tile columns for parallel encoding (auto-select based on resolution)
    // Formula: log2(width / 256) gives reasonable tile count
    int tile_cols = 0;
    if (width >= 1024) tile_cols = 1;  // 2 tiles for >= 1024 width
    if (width >= 2048) tile_cols = 2;  // 4 tiles for >= 2048 width
    vpx_codec_control(&encoder_, VP9E_SET_TILE_COLUMNS, tile_cols);

    // Disable periodic keyframe refresh (we'll control keyframes manually)
    vpx_codec_control(&encoder_, VP9E_SET_AQ_MODE, 0);  // Disable adaptive quantization

    // Set static threshold (higher = fewer keyframes)
    vpx_codec_control(&encoder_, VP8E_SET_STATIC_THRESHOLD, 1);

    // Enable noise sensitivity for better encoding of dithered content
    vpx_codec_control(&encoder_, VP9E_SET_NOISE_SENSITIVITY, 0);  // Disable for screen content

    width_ = width;
    height_ = height;
    fps_ = fps;
    frame_count_ = 0;
    initialized_ = true;

    if (g_debug_mode_switch) {
        fprintf(stderr, "VP9: Encoder initialized %dx%d @ %d fps (bitrate target %u kbps, CQ mode)\n",
                width, height, fps, config_.rc_target_bitrate);
    }

    return true;
}

void VP9Encoder::cleanup() {
    if (initialized_) {
        vpx_codec_destroy(&encoder_);
        initialized_ = false;
    }
    frame_count_ = 0;
}

EncodedFrame VP9Encoder::encode_i420(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                      int width, int height, int y_stride, int uv_stride) {
    EncodedFrame result;
    result.codec = CodecType::VP9;
    result.width = width;
    result.height = height;
    result.is_keyframe = false;

    if (!initialized_ || width != width_ || height != height_) {
        if (!init(width, height)) {
            return result;
        }
        force_keyframe_ = true;
    }

    // Setup input image
    vpx_image_t img;
    memset(&img, 0, sizeof(img));
    img.fmt = VPX_IMG_FMT_I420;
    img.w = width;
    img.h = height;
    img.d_w = width;
    img.d_h = height;
    img.x_chroma_shift = 1;
    img.y_chroma_shift = 1;
    img.bps = 12;  // 8-bit Y + 2 * 4-bit UV

    // Setup plane pointers (cast away const - libvpx doesn't modify input)
    img.planes[VPX_PLANE_Y] = const_cast<uint8_t*>(y);
    img.planes[VPX_PLANE_U] = const_cast<uint8_t*>(u);
    img.planes[VPX_PLANE_V] = const_cast<uint8_t*>(v);
    img.stride[VPX_PLANE_Y] = y_stride;
    img.stride[VPX_PLANE_U] = uv_stride;
    img.stride[VPX_PLANE_V] = uv_stride;

    // Encode frame
    vpx_enc_frame_flags_t flags = 0;
    if (force_keyframe_) {
        flags |= VPX_EFLAG_FORCE_KF;
        force_keyframe_ = false;
        fprintf(stderr, "VP9: Forcing keyframe\n");
    }

    // Duration of frame in timebase units (for 30 fps = 1 frame)
    unsigned long duration = 1;

    vpx_codec_err_t res = vpx_codec_encode(&encoder_, &img, frame_count_, duration, flags, VPX_DL_REALTIME);
    if (res != VPX_CODEC_OK) {
        fprintf(stderr, "VP9: Failed to encode frame (error %d: %s)\n",
                res, vpx_codec_error_detail(&encoder_));
        return result;
    }

    frame_count_++;

    // Get encoded output
    vpx_codec_iter_t iter = nullptr;
    const vpx_codec_cx_pkt_t* pkt;

    while ((pkt = vpx_codec_get_cx_data(&encoder_, &iter)) != nullptr) {
        if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
            // Copy encoded data
            const uint8_t* data = static_cast<const uint8_t*>(pkt->data.frame.buf);
            result.data.assign(data, data + pkt->data.frame.sz);
            result.is_keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;

            // Log frame info
            static int keyframe_count = 0;
            static int p_frame_count = 0;
            static int64_t p_size_total = 0;

            if (result.is_keyframe) {
                keyframe_count++;
                fprintf(stderr, "VP9: Keyframe %llu, size=%zu bytes (%.1f KB)\n",
                        (unsigned long long)frame_count_, result.data.size(), result.data.size() / 1024.0f);
            } else {
                p_frame_count++;
                p_size_total += result.data.size();
            }

            // Log stats every 90 frames
            if (frame_count_ % 90 == 0) {
                int avg_p = p_frame_count > 0 ? (int)(p_size_total / p_frame_count) : 0;
                fprintf(stderr, "VP9: Frame stats - total=%llu keyframes=%d P=%d avg_p=%d bytes (%.1f KB)\n",
                        (unsigned long long)frame_count_, keyframe_count, p_frame_count, avg_p, avg_p / 1024.0f);
            }

            break;  // Only process first packet
        }
    }

    return result;
}

EncodedFrame VP9Encoder::encode_bgra(const uint8_t* bgra, int width, int height, int stride) {
    // Convert BGRA to I420
    size_t y_size = width * height;
    size_t uv_size = (width / 2) * (height / 2);
    size_t total_size = y_size + 2 * uv_size;

    if (i420_buffer_.size() < total_size) {
        i420_buffer_.resize(total_size);
    }

    uint8_t* y = i420_buffer_.data();
    uint8_t* u = y + y_size;
    uint8_t* v = u + uv_size;

    // BGRA to I420 conversion using libyuv
    libyuv::ARGBToI420(
        bgra, stride,
        y, width,
        u, width / 2,
        v, width / 2,
        width, height
    );

    return encode_i420(y, u, v, width, height, width, width / 2);
}

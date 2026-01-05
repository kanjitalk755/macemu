/*
 * H.264 Encoder using OpenH264
 */

#include "h264_encoder.h"
#include <cstdio>
#include <libyuv.h>

// Debug flags (from server.cpp)
extern bool g_debug_mode_switch;

bool H264Encoder::init(int width, int height, int fps) {
    return init_internal(width, height, fps, 2000);
}

bool H264Encoder::init_internal(int width, int height, int fps, int bitrate_kbps) {
    cleanup();

    if (WelsCreateSVCEncoder(&encoder_) != 0 || !encoder_) {
        fprintf(stderr, "H264: Failed to create encoder\n");
        return false;
    }

    // Use extended parameters for proper configuration
    SEncParamExt param;
    encoder_->GetDefaultParams(&param);

    param.iUsageType = CAMERA_VIDEO_REAL_TIME;  // Standard H.264 (browser compatible)
    param.fMaxFrameRate = static_cast<float>(fps);
    param.iPicWidth = width;
    param.iPicHeight = height;
    param.iTargetBitrate = bitrate_kbps * 1000;
    param.iRCMode = RC_OFF_MODE;  // Disable rate control - use fixed QP
    param.bEnableFrameSkip = false;  // Don't skip frames
    param.bEnableDenoise = false;
    param.iSpatialLayerNum = 1;
    param.iTemporalLayerNum = 1;
    param.iMultipleThreadIdc = 0;  // Auto-detect threads
    param.uiIntraPeriod = fps * 5;  // Keyframe every 5 seconds (reduces stutter from large IDR frames)
    param.eSpsPpsIdStrategy = CONSTANT_ID;  // Constant SPS/PPS IDs (browser compatible)

    // Enable loop filter for better compression
    param.iLoopFilterDisableIdc = 0;  // Enable loop filter
    param.iLoopFilterAlphaC0Offset = 0;
    param.iLoopFilterBetaOffset = 0;

    // Configure the single spatial layer
    param.sSpatialLayers[0].iVideoWidth = width;
    param.sSpatialLayers[0].iVideoHeight = height;
    param.sSpatialLayers[0].fFrameRate = static_cast<float>(fps);
    param.sSpatialLayers[0].iSpatialBitrate = bitrate_kbps * 1000;
    param.sSpatialLayers[0].iMaxSpatialBitrate = bitrate_kbps * 1500;
    param.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;

    // Set H.264 level based on resolution (fixes "bitstream larger than level" warning)
    // Level 5.1 supports up to 4K and 40Mbps - plenty of headroom for large dithered frames
    param.sSpatialLayers[0].uiLevelIdc = LEVEL_5_1;

    if (encoder_->InitializeExt(&param) != 0) {
        fprintf(stderr, "H264: Failed to initialize encoder\n");
        cleanup();
        return false;
    }

    // Set fixed QP for all frame types (RC_OFF_MODE requires this)
    // QP 48 = very aggressive compression, will produce much smaller frames
    SEncParamExt currentParam;
    encoder_->GetOption(ENCODER_OPTION_SVC_ENCODE_PARAM_EXT, &currentParam);
    currentParam.iMinQp = 48;
    currentParam.iMaxQp = 51;
    encoder_->SetOption(ENCODER_OPTION_SVC_ENCODE_PARAM_EXT, &currentParam);

    // Set video format (required after InitializeExt)
    int videoFormat = videoFormatI420;
    encoder_->SetOption(ENCODER_OPTION_DATAFORMAT, &videoFormat);

    width_ = width;
    height_ = height;
    fps_ = fps;

    if (g_debug_mode_switch) {
        fprintf(stderr, "H264: Encoder initialized %dx%d @ %d kbps\n",
                width, height, bitrate_kbps);
    }
    return true;
}

void H264Encoder::cleanup() {
    if (encoder_) {
        encoder_->Uninitialize();
        WelsDestroySVCEncoder(encoder_);
        encoder_ = nullptr;
    }
}

EncodedFrame H264Encoder::encode_i420(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                       int width, int height, int y_stride, int uv_stride) {
    EncodedFrame result;
    result.codec = CodecType::H264;
    result.width = width;
    result.height = height;
    result.is_keyframe = false;

    if (!encoder_ || width != width_ || height != height_) {
        if (!init(width, height)) {
            return result;
        }
        force_keyframe_ = true;  // Force IDR on first frame after init
    }

    // Force keyframe if requested
    if (force_keyframe_) {
        fprintf(stderr, "H264: Forcing IDR frame\n");
        encoder_->ForceIntraFrame(true);
        force_keyframe_ = false;
    }

    SSourcePicture pic = {};
    pic.iPicWidth = width_;
    pic.iPicHeight = height_;
    pic.iColorFormat = videoFormatI420;
    pic.iStride[0] = y_stride;
    pic.iStride[1] = uv_stride;
    pic.iStride[2] = uv_stride;
    pic.pData[0] = const_cast<uint8_t*>(y);
    pic.pData[1] = const_cast<uint8_t*>(u);
    pic.pData[2] = const_cast<uint8_t*>(v);

    SFrameBSInfo info = {};
    int rv = encoder_->EncodeFrame(&pic, &info);
    if (rv != 0) {
        fprintf(stderr, "H264: EncodeFrame returned %d\n", rv);
        return result;
    }

    // Calculate total frame size
    static int frame_count = 0;
    static int idr_count = 0;
    static int p_count = 0;
    static int skip_count = 0;
    bool is_idr = (info.eFrameType == videoFrameTypeIDR);
    int total_size = 0;
    for (int layer = 0; layer < info.iLayerNum; layer++) {
        for (int nal = 0; nal < info.sLayerInfo[layer].iNalCount; nal++) {
            total_size += info.sLayerInfo[layer].pNalLengthInByte[nal];
        }
    }
    frame_count++;

    // Track frame types and P frame sizes for averaging
    static int64_t p_size_total = 0;
    static int p_size_count = 0;

    if (is_idr) {
        idr_count++;
        fprintf(stderr, "H264: IDR frame %d, size=%d bytes (%.1f KB)\n",
                frame_count, total_size, total_size / 1024.0f);
    } else if (info.eFrameType == videoFrameTypeP) {
        p_count++;
        p_size_total += total_size;
        p_size_count++;

        // Warn about oversized P-frames (>100KB is unusually large for H.264)
        // This typically happens with high-resolution dithered content where inter-frame
        // prediction fails catastrophically. Consider using PNG codec instead.
        if (total_size > 100 * 1024) {
            fprintf(stderr, "H264: WARNING - P-frame overflow detected! size=%d bytes (%.1f KB)\n",
                    total_size, total_size / 1024.0f);
            fprintf(stderr, "H264: This is common with dithered graphics where every pixel changes.\n");
            fprintf(stderr, "H264: Consider using 'webcodec png' in prefs for dithered content.\n");
        }
    } else if (info.eFrameType == videoFrameTypeSkip) {
        skip_count++;
    }

    // Log frame type stats every 90 frames (3 seconds at 30fps)
    if (frame_count % 90 == 0) {
        int avg_p_size = p_size_count > 0 ? (int)(p_size_total / p_size_count) : 0;
        fprintf(stderr, "H264: Frame stats - total=%d IDR=%d P=%d skip=%d avg_p=%d bytes\n",
                frame_count, idr_count, p_count, skip_count, avg_p_size);
    }

    if (info.eFrameType == videoFrameTypeSkip) {
        return result;
    }

    // Set keyframe flag
    result.is_keyframe = is_idr;

    // Collect all NAL units with start codes
    for (int layer = 0; layer < info.iLayerNum; layer++) {
        const SLayerBSInfo& layerInfo = info.sLayerInfo[layer];
        uint8_t* buf = layerInfo.pBsBuf;
        for (int nal = 0; nal < layerInfo.iNalCount; nal++) {
            int nalSize = layerInfo.pNalLengthInByte[nal];
            result.data.insert(result.data.end(), buf, buf + nalSize);
            buf += nalSize;
        }
    }

    return result;
}

bool H264Encoder::is_keyframe(const std::vector<uint8_t>& data) {
    // Look for IDR NAL unit (type 5) after start code
    for (size_t i = 0; i + 4 < data.size(); i++) {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            uint8_t nal_type = data[i+4] & 0x1F;
            if (nal_type == 5) return true;  // IDR frame
        }
    }
    return false;
}

EncodedFrame H264Encoder::encode_bgra(const uint8_t* bgra, int width, int height, int stride) {
    // BGRA = bytes B,G,R,A = libyuv "ARGB"
    // Convert to I420 using ARGBToI420

    // Ensure I420 buffer is sized correctly
    size_t y_size = width * height;
    size_t uv_size = (width / 2) * (height / 2);
    size_t total_size = y_size + 2 * uv_size;
    if (i420_buffer_.size() < total_size) {
        i420_buffer_.resize(total_size);
    }

    uint8_t* y = i420_buffer_.data();
    uint8_t* u = y + y_size;
    uint8_t* v = u + uv_size;

    libyuv::ARGBToI420(
        bgra, stride,
        y, width,
        u, width / 2,
        v, width / 2,
        width, height
    );

    return encode_i420(y, u, v, width, height, width, width / 2);
}

EncodedFrame H264Encoder::encode_argb(const uint8_t* argb, int width, int height, int stride) {
    // ARGB = bytes A,R,G,B = libyuv "BGRA" (Mac native 32-bit)
    // Convert to I420 using BGRAToI420

    // Ensure I420 buffer is sized correctly
    size_t y_size = width * height;
    size_t uv_size = (width / 2) * (height / 2);
    size_t total_size = y_size + 2 * uv_size;
    if (i420_buffer_.size() < total_size) {
        i420_buffer_.resize(total_size);
    }

    uint8_t* y = i420_buffer_.data();
    uint8_t* u = y + y_size;
    uint8_t* v = u + uv_size;

    libyuv::BGRAToI420(
        argb, stride,
        y, width,
        u, width / 2,
        v, width / 2,
        width, height
    );

    return encode_i420(y, u, v, width, height, width, width / 2);
}

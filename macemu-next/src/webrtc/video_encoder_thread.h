/*
 * Video Encoder Thread - Header
 */

#ifndef VIDEO_ENCODER_THREAD_H
#define VIDEO_ENCODER_THREAD_H

#include "../platform/video_output.h"
#include "../config/config_manager.h"

namespace webrtc {

/**
 * Video encoder thread entry point
 *
 * Reads frames from VideoOutput triple buffer and encodes to H.264/VP9/WebP/PNG.
 * Runs until g_running is set to false.
 *
 * @param video_output Triple buffer to read frames from
 * @param config Configuration (for codec selection)
 */
void video_encoder_main(VideoOutput* video_output, config::MacemuConfig* config);

} // namespace webrtc

#endif // VIDEO_ENCODER_THREAD_H

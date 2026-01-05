/*
 * Audio Encoder Thread - Header
 */

#ifndef AUDIO_ENCODER_THREAD_H
#define AUDIO_ENCODER_THREAD_H

#include "../platform/audio_output.h"

namespace webrtc {

/**
 * Audio encoder thread entry point
 *
 * Reads audio samples from AudioOutput ring buffer and encodes to Opus.
 * Runs at 50 Hz (20ms frames) until g_running is set to false.
 *
 * @param audio_output Ring buffer to read samples from
 */
void audio_encoder_main(AudioOutput* audio_output);

} // namespace webrtc

#endif // AUDIO_ENCODER_THREAD_H

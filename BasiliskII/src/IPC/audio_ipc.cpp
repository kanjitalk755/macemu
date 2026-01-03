/*
 *  audio_ipc.cpp - IPC-based audio driver for standalone WebRTC server
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *  IPC mode (C) 2024
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 *  NOTES:
 *    Audio IPC uses same SHM as video (MacEmuIPCBuffer).
 *
 *    - Audio writes to audio_frames[] in existing video SHM
 *    - Signals via audio_ready_eventfd (separate from video)
 *    - Server processes audio independently from video
 *    - Supports dynamic format changes (sample rate, channels)
 *
 *    Double buffering - audio changes less frequently than video.
 */

#include "sysdeps.h"

#ifdef ENABLE_IPC_AUDIO

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "audio_config.h"
#include <cerrno>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "user_strings.h"
#include "audio.h"
#include "audio_defs.h"

// IPC protocol definitions
#include "ipc_protocol.h"

// Get video SHM pointer (managed by video_ipc.cpp)
extern MacEmuIPCBuffer* IPC_GetVideoSHM();

#define DEBUG 0
#include "debug.h"

// Audio buffer for mixing/conversion before writing to SHM ring buffer
static uint8_t* audio_mix_buffer = nullptr;
static size_t audio_mix_buffer_size = 0;

// Ring buffer is now in SHM (MacEmuIPCBuffer::audio_ring_buffer)
// Indices are atomic fields in SHM (audio_ring_write_pos, audio_ring_read_pos)
// No local copy needed - zero-copy architecture!

// Frame-based audio architecture:
// - Mac side: Audio thread produces frames at ~20ms intervals (variable sample rate)
// - Frame size: Dynamic based on sample rate (e.g., 882 samples @ 44.1kHz, 960 @ 48kHz)
// - Server side: Consumes frames at 20ms intervals, resamples to 48kHz for Opus
// - Ring buffer: 8 frames (160ms) absorbs timing jitter and clock drift

// Audio thread
static std::thread audio_thread;
static std::atomic<bool> audio_thread_running(false);

// Synchronization for AudioInterrupt (audio thread → Mac emulation thread)
static std::mutex audio_irq_mutex;
static std::condition_variable audio_irq_done_cv;
static bool audio_irq_done = false;

// Synchronization for server audio requests (server → audio thread)
static std::mutex audio_request_mutex;
static std::condition_variable audio_request_cv;
static bool audio_request_pending = false;
static uint32_t audio_requested_samples = 0;

// Counters for debug
static uint64_t audio_frames_sent = 0;
static uint64_t last_log_frame = 0;

// Debug flag (read once at initialization)
static bool g_debug_audio = false;

// Forward declarations
static void ring_buffer_write(const uint8_t* data, size_t len);
static void audio_thread_func();


/*
 *  Initialization
 */

void AudioInit(void)
{
	// Read debug flag once at startup
	g_debug_audio = (getenv("MACEMU_DEBUG_AUDIO") != nullptr);

	// Init audio status and feature flags using centralized audio_config.h constants
	// Mac will calculate frame size: sample_rate * frame_duration_ms / 1000
	// Example: 48000 * 20ms / 1000 = 960 samples (perfect for Opus)
	AudioStatus.sample_rate = AUDIO_SAMPLE_RATE << 16;  // Mac format: upper 16 bits = integer part
	AudioStatus.sample_size = AUDIO_SAMPLE_SIZE;
	AudioStatus.channels = AUDIO_CHANNELS;
	AudioStatus.mixer = 0;
	AudioStatus.num_sources = 0;
	audio_component_flags = cmpWantsRegisterMessage | kStereoOut | k16BitOut;

	// Supported audio formats - IPC audio supports multiple sample rates
	// Mac OS will choose from these based on what the application requests
	audio_sample_rates.push_back(11025 << 16);  // 11.025 kHz
	audio_sample_rates.push_back(22050 << 16);  // 22.05 kHz
	audio_sample_rates.push_back(44100 << 16);  // 44.1 kHz
	audio_sample_rates.push_back(48000 << 16);  // 48 kHz

	// Supported sample sizes
	audio_sample_sizes.push_back(8);   // 8-bit (U8)
	audio_sample_sizes.push_back(16);  // 16-bit (S16)

	// Supported channel counts
	audio_channel_counts.push_back(1);  // Mono
	audio_channel_counts.push_back(2);  // Stereo

	// Sound disabled in prefs? Then do nothing
	if (PrefsFindBool("nosound")) {
		fprintf(stderr, "Audio IPC: Disabled by 'nosound' pref\n");
		audio_open = false;
		return;
	}

	// Allocate mixing buffer (max 48kHz stereo 20ms = 3840 bytes)
	audio_mix_buffer_size = MACEMU_AUDIO_MAX_FRAME_SIZE;
	audio_mix_buffer = (uint8_t*)malloc(audio_mix_buffer_size);

	if (!audio_mix_buffer) {
		fprintf(stderr, "Audio IPC: Failed to allocate mix buffer\n");
		audio_open = false;
		return;
	}

	memset(audio_mix_buffer, 0, audio_mix_buffer_size);

	// Set audio frames per block to 20ms worth of samples at current rate
	// This matches Opus frame duration for efficient streaming
	// Calculate based on actual sample rate: samples = (rate * 20) / 1000
	uint32_t sample_rate = AudioStatus.sample_rate >> 16;
	audio_frames_per_block = (sample_rate * 20) / 1000;

	// Examples at different rates:
	//   11025 Hz: 220 samples
	//   22050 Hz: 441 samples
	//   44100 Hz: 882 samples
	//   48000 Hz: 960 samples

	// Mark audio as available
	audio_open = true;

	// Start the request-driven audio thread immediately
	// Thread will wait for requests from server, even when num_sources == 0
	audio_thread_running = true;
	audio_thread = std::thread(audio_thread_func);

	fprintf(stderr, "Audio IPC: Initialized and READY (buffer size: %zu bytes, frames per block: %d, sample rates: 11025-48000 Hz)\n",
	        audio_mix_buffer_size, audio_frames_per_block);
	fprintf(stderr, "Audio IPC: Request-driven audio thread started (PULL MODEL)\n");
	D(bug("Audio IPC: Initialized (buffer size: %zu bytes, frames: %d)\n", audio_mix_buffer_size, audio_frames_per_block));
}


/*
 *  Deinitialization
 */

void AudioExit(void)
{
	// Stop request-driven audio thread if running
	if (audio_thread_running) {
		fprintf(stderr, "Audio IPC: Stopping request-driven audio thread...\n");
		audio_thread_running = false;
		// Wake up thread so it can see the shutdown flag
		{
			std::lock_guard<std::mutex> lock(audio_request_mutex);
			audio_request_pending = true;
			audio_request_cv.notify_one();
		}
		if (audio_thread.joinable()) {
			audio_thread.join();
		}
		fprintf(stderr, "Audio IPC: Request-driven audio thread stopped\n");
	}

	if (audio_mix_buffer) {
		free(audio_mix_buffer);
		audio_mix_buffer = nullptr;
	}

	D(bug("Audio IPC: Shutdown complete\n"));
}


/*
 *  First source added (num_sources 0→1)
 *  Wake up the audio thread immediately
 */

void audio_enter_stream()
{
	if (g_debug_audio) {
		fprintf(stderr, "Audio IPC: Stream started (num_sources 0→1)\n");
	}
	D(bug("Audio IPC: Stream started\n"));

	// Update SHM with current audio format
	MacEmuIPCBuffer* shm = IPC_GetVideoSHM();
	if (shm) {
		uint32_t sample_rate = AudioStatus.sample_rate >> 16;
		shm->audio_sample_rate = sample_rate;
		shm->audio_channels = AudioStatus.channels;
		shm->audio_format = MACEMU_AUDIO_FORMAT_PCM_S16;
		if (g_debug_audio) {
			fprintf(stderr, "Audio IPC: Set SHM audio format - %u Hz, %u ch\n",
				sample_rate, AudioStatus.channels);
		}
	}

	// Thread is already running (started in AudioInit), no need to start here
}

/*
 *  Frame-based audio thread (PULL MODEL - request-driven)
 *  Waits for server requests, then produces frames on-demand
 *  Server controls timing, Mac just responds to requests
 */

static void audio_thread_func()
{
	while (audio_thread_running) {
		// Wait for server request (BLOCKING - no autonomous timing!)
		{
			std::unique_lock<std::mutex> lock(audio_request_mutex);
			audio_request_cv.wait(lock, []{
				return audio_request_pending || !audio_thread_running;
			});

			if (!audio_thread_running) break;  // Shutting down

			audio_request_pending = false;  // Consume request
		}

		MacEmuIPCBuffer* shm = IPC_GetVideoSHM();
		if (!shm) continue;

		// Process audio request (even if num_sources == 0, send silence)
		if (AudioStatus.num_sources > 0) {
			// Get current sample rate (Mac OS may change it dynamically)
			uint32_t sample_rate = AudioStatus.sample_rate >> 16;  // Convert from Mac format
			if (sample_rate == 0) sample_rate = 44100;  // Default

			// Trigger Mac audio interrupt to get fresh data
			SetInterruptFlag(INTFLAG_AUDIO);
			TriggerInterrupt();

			// Wait for Mac to fill audio data
			{
				std::unique_lock<std::mutex> lock(audio_irq_mutex);
				audio_irq_done_cv.wait(lock, []{ return audio_irq_done; });
				audio_irq_done = false;
			}

			// Get write frame from ring buffer
			MacEmuIPCBuffer* shm = IPC_GetVideoSHM();
			if (shm && audio_data) {
				uint32 apple_stream_info = ReadMacInt32(audio_data + adatStreamInfo);
				bool wrote_frame = false;

				if (apple_stream_info) {
					uint32 sample_count = ReadMacInt32(apple_stream_info + scd_sampleCount);
					uint32 buffer_ptr = ReadMacInt32(apple_stream_info + scd_buffer);
					uint32 num_channels = ReadMacInt16(apple_stream_info + scd_numChannels);
					uint32 sample_size = ReadMacInt16(apple_stream_info + scd_sampleSize);

					if (g_debug_audio) {
						static int log_count = 0;
						if (log_count++ < 5) {
							fprintf(stderr, "Audio IPC: Mac provided %u samples, %u ch, %u-bit @ %u Hz\n",
								sample_count, num_channels, sample_size, sample_rate);
						}
					}

					if (sample_count > 0 && buffer_ptr != 0) {
						// Get write index
						uint32_t write_idx = ATOMIC_LOAD(shm->audio_frame_write_idx);
						uint32_t read_idx = ATOMIC_LOAD(shm->audio_frame_read_idx);
						uint32_t next_write_idx = (write_idx + 1) % MACEMU_AUDIO_FRAME_RING_SIZE;

						// Check if ring buffer is full
						if (next_write_idx == read_idx) {
							if (g_debug_audio) {
								static int overflow_count = 0;
								if (++overflow_count <= 10) {
									fprintf(stderr, "Audio IPC: Ring buffer full, dropping frame (count=%d)\n",
										overflow_count);
								}
							}
							// Drop this frame - server is too slow
						} else {
							// Get pointer to frame
							MacEmuAudioFrame* frame = &shm->audio_frame_ring[write_idx];

							// Fill metadata
							frame->sample_rate = sample_rate;
							frame->channels = num_channels;
							frame->format = MACEMU_AUDIO_FORMAT_PCM_S16;

							// Process audio data
							uint32 bytes_per_sample = (sample_size >> 3);
							size_t data_len = sample_count * bytes_per_sample * num_channels;

							if (data_len > MACEMU_AUDIO_MAX_FRAME_SIZE) {
								data_len = MACEMU_AUDIO_MAX_FRAME_SIZE;
								sample_count = data_len / (bytes_per_sample * num_channels);
							}

							uint8_t* src = Mac2HostAddr(buffer_ptr);

							// AUDIO DEBUG: Capture raw Mac audio data
							// Only capture non-silent frames (skip silence between sounds)
							static FILE* capture_file = nullptr;
							static int frames_captured = 0;
							static int silent_frames = 0;
							static bool capture_started = false;

							// Check for capture trigger from shared memory (set by server when user presses 'C')
							uint32_t trigger = ATOMIC_LOAD(shm->capture_trigger);
							if (trigger && !capture_started) {
								capture_started = true;
								frames_captured = 0;  // Reset counter
								fprintf(stderr, "[IPC Emulator] *** CAPTURE TRIGGERED *** (via shared memory flag)\n");
							}

							if (capture_started && frames_captured < AUDIO_MAX_CAPTURE_FRAMES) {
								// Calculate energy to detect non-silence
								uint64_t energy = 0;
								if (sample_size == 16) {
									int16_t* samples = (int16_t*)src;
									for (uint32 i = 0; i < sample_count * num_channels; i++) {
										int16_t val_be = samples[i];
										// Interpret as big-endian
										int16_t val = (int16_t)(((val_be & 0xFF) << 8) | ((val_be >> 8) & 0xFF));
										energy += (val < 0) ? -val : val;
									}
								}

								// Only capture non-silent frames
								if (energy > AUDIO_ENERGY_THRESHOLD) {
									if (!capture_file) {
										capture_file = fopen("/tmp/ipc_emulator_capture.raw", "wb");
										fprintf(stderr, "IPC (emulator): Starting audio capture to /tmp/ipc_emulator_capture.raw\n");
										fprintf(stderr, "IPC (emulator): Format: %u-bit, %u channels, %u Hz\n",
										        sample_size, num_channels, sample_rate);
										fprintf(stderr, "IPC (emulator): Capturing only non-silent frames (max %d frames)\n", AUDIO_MAX_CAPTURE_FRAMES);
									}
									fwrite(src, 1, data_len, capture_file);
									frames_captured++;
									silent_frames = 0;

									// Log every 50 frames
									if (frames_captured % 50 == 0) {
										fprintf(stderr, "IPC (emulator): Captured %d non-silent frames\n", frames_captured);
									}

									// Stop after max frames
									if (frames_captured >= AUDIO_MAX_CAPTURE_FRAMES) {
										fclose(capture_file);
										fprintf(stderr, "IPC (emulator): Audio capture complete (%d frames)\n", frames_captured);
									}
								} else {
									silent_frames++;
									// Log long silences
									if (silent_frames == 50) {
										fprintf(stderr, "IPC (emulator): Skipping silence (captured %d frames so far)\n", frames_captured);
									}
								}
							}

							if (sample_size == 8) {
								// Convert U8 to S16
								uint8_t* src_u8 = src;
								int16_t* dst_s16 = (int16_t*)frame->data;
								for (uint32 i = 0; i < sample_count * num_channels; i++) {
									dst_s16[i] = ((int16_t)src_u8[i] - 128) << 8;
								}
								frame->samples = sample_count;
							} else {
								// S16 data - direct copy (Mac provides S16MSB format)
								memcpy(frame->data, src, data_len);
								frame->samples = sample_count;
							}

							// Get timestamp
							struct timespec ts;
							clock_gettime(CLOCK_REALTIME, &ts);
							frame->timestamp_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

							// Publish frame (atomic store with release semantics)
							ATOMIC_STORE(shm->audio_frame_write_idx, next_write_idx);

							audio_frames_sent++;
							wrote_frame = true;

							// Log every 100 frames (~2 seconds at 20ms/frame) if debug enabled
							if (g_debug_audio && (audio_frames_sent - last_log_frame >= 100)) {
								fprintf(stderr, "Audio IPC: Sent %lu frames (%u samples/frame, %uHz, %uch)\n",
									audio_frames_sent, sample_count, sample_rate, num_channels);
								last_log_frame = audio_frames_sent;
							}
						}
					}
				}

				// If Mac didn't provide data (not ready yet), send silence frame
				// This ensures server always gets a response to its request
				if (!wrote_frame) {
					uint32_t write_idx = ATOMIC_LOAD(shm->audio_frame_write_idx);
					uint32_t read_idx = ATOMIC_LOAD(shm->audio_frame_read_idx);
					uint32_t next_write_idx = (write_idx + 1) % MACEMU_AUDIO_FRAME_RING_SIZE;

					if (next_write_idx != read_idx) {
						MacEmuAudioFrame* frame = &shm->audio_frame_ring[write_idx];

						// Fill with silence at current sample rate
						frame->sample_rate = sample_rate;
						frame->channels = AudioStatus.channels;
						frame->format = MACEMU_AUDIO_FORMAT_PCM_S16;
						// Calculate samples for 20ms at current rate
						frame->samples = (sample_rate * 20) / 1000;

						// Zero out audio data (silence)
						memset(frame->data, 0, frame->samples * 2 * frame->channels);

						// Get timestamp
						struct timespec ts;
						clock_gettime(CLOCK_REALTIME, &ts);
						frame->timestamp_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

						// Publish silence frame
						ATOMIC_STORE(shm->audio_frame_write_idx, next_write_idx);

						if (g_debug_audio) {
							static int silence_count = 0;
							if (++silence_count <= 10) {
								fprintf(stderr, "Audio IPC: Mac not ready, sent silence frame (count=%d)\n",
									silence_count);
							}
						}
					}
				}
			}
			// NO SLEEP! Server controls timing via requests
			// Mac just responds immediately and waits for next request
		} else {
			// No audio sources - send silence frame to prevent underruns
			uint32_t write_idx = ATOMIC_LOAD(shm->audio_frame_write_idx);
			uint32_t read_idx = ATOMIC_LOAD(shm->audio_frame_read_idx);
			uint32_t next_write_idx = (write_idx + 1) % MACEMU_AUDIO_FRAME_RING_SIZE;

			if (next_write_idx != read_idx) {
				MacEmuAudioFrame* frame = &shm->audio_frame_ring[write_idx];

				// Fill with silence at default rate
				frame->sample_rate = 44100;
				frame->channels = 2;
				frame->format = MACEMU_AUDIO_FORMAT_PCM_S16;
				frame->samples = 882;  // 20ms @ 44.1kHz

				// Zero out audio data (silence)
				memset(frame->data, 0, frame->samples * 2 * frame->channels);

				// Get timestamp
				struct timespec ts;
				clock_gettime(CLOCK_REALTIME, &ts);
				frame->timestamp_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

				// Publish silence frame
				ATOMIC_STORE(shm->audio_frame_write_idx, next_write_idx);
			}
		}
	}
}


/*
 *  Last source removed (num_sources 1→0)
 *  Thread continues running, just starts sending silence frames
 */

void audio_exit_stream()
{
	if (g_debug_audio) {
		fprintf(stderr, "Audio IPC: Stream stopped (num_sources 1→0)\n");
	}
	D(bug("Audio IPC: Stream stopped\n"));

	// Thread keeps running - will send silence frames when num_sources == 0

	// Signal audio paused (not disabled)
	MacEmuIPCBuffer* video_shm = IPC_GetVideoSHM();
	if (video_shm) {
		video_shm->audio_format = MACEMU_AUDIO_FORMAT_NONE;
	}
}


/*
 *  MacOS audio interrupt - new buffer is available
 *  This is called from the Mac sound manager interrupt
 */

void AudioInterrupt(void)
{
	D(bug("Audio IPC: Interrupt\n"));

	if (g_debug_audio) {
		static int irq_count = 0;
		irq_count++;
		if (irq_count <= 10 || irq_count % 100 == 0) {
			fprintf(stderr, "Audio IPC: AudioInterrupt called (count=%d, num_sources=%d)\n",
			        irq_count, AudioStatus.num_sources);
		}
	}

	// Call GetSourceData to fill apple_stream_info (like SDL does)
	// This tells the Apple Mixer to prepare audio data for us to read
	if (!audio_data) {
		// Audio component has been closed, audio_data freed
		if (g_debug_audio) {
			fprintf(stderr, "Audio IPC: AudioInterrupt - audio_data is NULL, component closed\n");
		}
	} else if (AudioStatus.mixer) {
		M68kRegisters r;
		r.a[0] = audio_data + adatStreamInfo;
		r.a[1] = AudioStatus.mixer;
		Execute68k(audio_data + adatGetSourceData, &r);
		D(bug(" GetSourceData() returns %08lx\n", r.d[0]));
	} else {
		WriteMacInt32(audio_data + adatStreamInfo, 0);
	}

	// Signal streaming thread that interrupt is complete
	{
		std::lock_guard<std::mutex> lock(audio_irq_mutex);
		audio_irq_done = true;
	}
	audio_irq_done_cv.notify_one();
}


/*
 * Request audio data (called by server via socket)
 * This is the pull model entry point - server asks, Mac responds
 */

void audio_request_data(uint32_t requested_samples)
{
	if (g_debug_audio) {
		static int request_count = 0;
		if (++request_count <= 10 || request_count % 100 == 0) {
			fprintf(stderr, "Audio IPC: Server requested %u samples (count=%d)\n",
				requested_samples, request_count);
		}
	}

	// Wake up audio thread with request
	{
		std::lock_guard<std::mutex> lock(audio_request_mutex);
		audio_requested_samples = requested_samples;
		audio_request_pending = true;
	}
	audio_request_cv.notify_one();
}


/*
 *  Get/set audio info
 */

bool audio_get_main_mute(void)
{
	return false;  // Not muted
}

uint32 audio_get_main_volume(void)
{
	return 0x0100;  // Max volume
}

bool audio_get_speaker_mute(void)
{
	return false;
}

uint32 audio_get_speaker_volume(void)
{
	return 0x0100;  // Max volume
}

void audio_set_main_mute(bool mute)
{
	// Not implemented for IPC audio
	(void)mute;
}

void audio_set_main_volume(uint32 vol)
{
	// Not implemented for IPC audio
	(void)vol;
}

void audio_set_speaker_mute(bool mute)
{
	// Not implemented for IPC audio
	(void)mute;
}

void audio_set_speaker_volume(uint32 vol)
{
	// Not implemented for IPC audio
	(void)vol;
}

bool audio_set_sample_rate(int index)
{
	// Sample rate changes are handled automatically
	// AudioStatus.sample_rate is updated by audio.cpp
	D(bug("Audio IPC: Sample rate changed to index %d\n", index));
	return true;
}

bool audio_set_sample_size(int index)
{
	// Sample size changes are handled automatically
	D(bug("Audio IPC: Sample size changed to index %d\n", index));
	return true;
}

bool audio_set_channels(int index)
{
	// Channel changes are handled automatically
	D(bug("Audio IPC: Channels changed to index %d\n", index));
	return true;
}


// No dedicated audio thread needed - audio processing happens inline in audio_request_data()


/*
 *  Startup sound (SheepShaver only)
 */

#ifdef SHEEPSHAVER
// SheepShaver-specific function (not used in BasiliskII)
void PlayStartupSound() {
	// Startup sound is handled by Mac OS ROM in SheepShaver
	// No emulator-side implementation needed with IPC audio
}
#endif


#endif // ENABLE_IPC_AUDIO

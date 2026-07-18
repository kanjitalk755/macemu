/*
 *  audio_sdl.cpp - Audio support, SDL implementation
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
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

#include "sysdeps.h"

#include "my_sdl.h"
#if !SDL_VERSION_ATLEAST(3, 0, 0)

#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "user_strings.h"
#include "audio.h"
#include "audio_defs.h"

#if defined(QD3D_AUDIO_LOGGING_ENABLED) && QD3D_AUDIO_LOGGING_ENABLED
#include "gfx_log.h"
#else
#ifndef QD3D_AUDIO_LOGGING_ENABLED
#define QD3D_AUDIO_LOGGING_ENABLED 0
#endif
#define QD3D_AUDIO_LOG(...) do { } while (0)
#endif

#define DEBUG 0
#include "debug.h"

#if defined(BINCUE)
#include "bincue.h"
#endif


#define MAC_MAX_VOLUME 0x0100
#define AUDIO_PREFETCH_RETRY_MS 5

// The currently selected audio parameters (indices in audio_sample_rates[] etc. vectors)
static int audio_sample_rate_index = 0;
static int audio_sample_size_index = 0;
static int audio_channel_count_index = 0;

// Global variables
static SDL_sem *audio_irq_done_sem = NULL;			// Signal from interrupt to streaming thread: data block read
static uint8 silence_byte;							// Byte value to use to fill sound buffers with silence
static uint8 *audio_mix_buf = NULL;
static uint8 *audio_fetch_buf = NULL;
static SDL_AudioStream *audio_prefetch_stream = NULL;
static SDL_Thread *audio_prefetch_thread = NULL;
static SDL_atomic_t audio_prefetch_quit;
static int audio_callback_bytes = 0;
static int audio_fetch_bytes = 0;
static int main_volume = MAC_MAX_VOLUME;
static int speaker_volume = MAC_MAX_VOLUME;
static bool main_mute = false;
static bool speaker_mute = false;

// Prototypes
static void stream_func(void *arg, uint8 *stream, int stream_len);
static int audio_prefetch_func(void *arg);
static int get_audio_volume();


/*
 *  Initialization
 */

// Set AudioStatus to reflect current audio stream format
static void set_audio_status_format(void)
{
	AudioStatus.sample_rate = audio_sample_rates[audio_sample_rate_index];
	AudioStatus.sample_size = audio_sample_sizes[audio_sample_size_index];
	AudioStatus.channels = audio_channel_counts[audio_channel_count_index];
}

// Init SDL audio system
static bool open_sdl_audio(void)
{
	// SDL supports a variety of twisted little audio formats, all different
	if (audio_sample_sizes.empty()) {
		audio_sample_rates.push_back(11025 << 16);
		audio_sample_rates.push_back(22050 << 16);
		audio_sample_rates.push_back(44100 << 16);
		audio_sample_sizes.push_back(8);
		audio_sample_sizes.push_back(16);
		audio_channel_counts.push_back(1);
		audio_channel_counts.push_back(2);

		// Default to highest supported values
		audio_sample_rate_index = audio_sample_rates.size() - 1;
		audio_sample_size_index = audio_sample_sizes.size() - 1;
		audio_channel_count_index = audio_channel_counts.size() - 1;
	}

	SDL_AudioSpec audio_spec;
	memset(&audio_spec, 0, sizeof(audio_spec));
	audio_spec.freq = audio_sample_rates[audio_sample_rate_index] >> 16;
	audio_spec.format = (audio_sample_sizes[audio_sample_size_index] == 8) ? AUDIO_U8 : AUDIO_S16MSB;
	audio_spec.channels = audio_channel_counts[audio_channel_count_index];
	audio_spec.samples = 4096 >> PrefsFindInt32("sound_buffer");
	audio_spec.callback = stream_func;
	audio_spec.userdata = NULL;
	QD3D_AUDIO_LOG("SDL open requested format=%uHz/%ubit/%uch samples=%u sources=%d",
	                audio_spec.freq, SDL_AUDIO_BITSIZE(audio_spec.format),
	                audio_spec.channels, audio_spec.samples,
	                AudioStatus.num_sources);

	// Open the audio device, forcing the desired format
	if (SDL_OpenAudio(&audio_spec, NULL) < 0) {
		fprintf(stderr, "WARNING: Cannot open audio: %s\n", SDL_GetError());
		return false;
	}
	
#if SDL_VERSION_ATLEAST(2,0,0)
	// HACK: workaround a bug in SDL pre-2.0.6 (reported via https://bugzilla.libsdl.org/show_bug.cgi?id=3710 )
	// whereby SDL does not update audio_spec.size
	if (audio_spec.size == 0) {
		audio_spec.size = (SDL_AUDIO_BITSIZE(audio_spec.format) / 8) * audio_spec.channels * audio_spec.samples;
	}
#endif

#if defined(BINCUE)
	OpenAudio_bincue(audio_spec.freq, audio_spec.format, audio_spec.channels,
	audio_spec.silence, get_audio_volume());
#endif

#if SDL_VERSION_ATLEAST(2,0,0)
	const char * driver_name = SDL_GetCurrentAudioDriver();
#else
	char driver_name[32];
	SDL_AudioDriverName(driver_name, sizeof(driver_name) - 1);
#endif
	printf("Using SDL/%s audio output\n", driver_name ? driver_name : "");
	silence_byte = audio_spec.silence;

	audio_frames_per_block = audio_spec.samples;
	audio_callback_bytes = audio_spec.size;
	audio_mix_buf = (uint8*)malloc(audio_spec.size);
	audio_fetch_buf = (uint8*)malloc(audio_spec.size);
	audio_prefetch_stream = SDL_NewAudioStream(audio_spec.format,
		audio_spec.channels, audio_spec.freq, audio_spec.format,
		audio_spec.channels, audio_spec.freq);
	if (!audio_mix_buf || !audio_fetch_buf || !audio_prefetch_stream) {
		fprintf(stderr, "WARNING: Cannot allocate audio prefetch buffer: %s\n", SDL_GetError());
		free(audio_mix_buf);
		free(audio_fetch_buf);
		audio_mix_buf = audio_fetch_buf = NULL;
		if (audio_prefetch_stream)
			SDL_FreeAudioStream(audio_prefetch_stream);
		audio_prefetch_stream = NULL;
#if defined(BINCUE)
		CloseAudio_bincue();
#endif
		SDL_CloseAudio();
		return false;
	}
	// close_audio() wakes a blocked producer. Discard that wake-up before
	// starting a replacement producer after an output-format change.
	while (SDL_SemTryWait(audio_irq_done_sem) == 0) {}
	SDL_AtomicSet(&audio_prefetch_quit, 0);
	audio_prefetch_thread = SDL_CreateThread(audio_prefetch_func,
		"audio_sdl2_prefetch", NULL);
	if (!audio_prefetch_thread) {
		fprintf(stderr, "WARNING: Cannot start audio prefetch thread: %s\n", SDL_GetError());
		SDL_FreeAudioStream(audio_prefetch_stream);
		audio_prefetch_stream = NULL;
		free(audio_mix_buf);
		free(audio_fetch_buf);
		audio_mix_buf = audio_fetch_buf = NULL;
#if defined(BINCUE)
		CloseAudio_bincue();
#endif
		SDL_CloseAudio();
		return false;
	}
	QD3D_AUDIO_LOG("SDL open complete driver=%s size=%u silence=0x%02x",
	                driver_name ? driver_name : "", audio_spec.size,
	                silence_byte);
	SDL_PauseAudio(0);
	return true;
}

static bool open_audio(void)
{
	// Try to open SDL audio
	if (!open_sdl_audio()) {
		WarningAlert(GetString(STR_NO_AUDIO_WARN));
		return false;
	}

	// Device opened, set AudioStatus
	set_audio_status_format();

	// Everything went fine
	audio_open = true;
	return true;
}

void AudioInit(void)
{
	// Init audio status and feature flags
	AudioStatus.sample_rate = 44100 << 16;
	AudioStatus.sample_size = 16;
	AudioStatus.channels = 2;
	AudioStatus.mixer = 0;
	AudioStatus.num_sources = 0;
	audio_component_flags = cmpWantsRegisterMessage | kStereoOut | k16BitOut;
	QD3D_AUDIO_LOG("Sound output component flags=0x%08x", audio_component_flags);

	// Sound disabled in prefs? Then do nothing
	if (PrefsFindBool("nosound"))
		return;

	// Init semaphore
	audio_irq_done_sem = SDL_CreateSemaphore(0);
#ifdef BINCUE
	InitBinCue();
#endif
	// Open and initialize audio device
	open_audio();
}


/*
 *  Deinitialization
 */

static void close_audio(void)
{
	QD3D_AUDIO_LOG("SDL close format=%uHz/%ubit/%uch sources=%d",
	                AudioStatus.sample_rate >> 16, AudioStatus.sample_size,
	                AudioStatus.channels, AudioStatus.num_sources);
	SDL_PauseAudio(1);
	SDL_AtomicSet(&audio_prefetch_quit, 1);
	if (audio_prefetch_thread) {
		SDL_SemPost(audio_irq_done_sem);
		SDL_WaitThread(audio_prefetch_thread, NULL);
	}
	audio_prefetch_thread = NULL;
	if (audio_prefetch_stream)
		SDL_FreeAudioStream(audio_prefetch_stream);
	audio_prefetch_stream = NULL;
#if defined(BINCUE)
	CloseAudio_bincue();
#endif
	SDL_CloseAudio();
	free(audio_mix_buf);
	free(audio_fetch_buf);
	audio_mix_buf = audio_fetch_buf = NULL;
	audio_callback_bytes = 0;
	audio_fetch_bytes = 0;
	audio_open = false;
}

void AudioExit(void)
{
	// Close audio device
	close_audio();
#ifdef BINCUE
	ExitBinCue();
#endif
	// Delete semaphore
	if (audio_irq_done_sem)
		SDL_DestroySemaphore(audio_irq_done_sem);
}


/*
 *  First source added, start audio stream
 */

void audio_enter_stream()
{
}


/*
 *  Last source removed, stop audio stream
 */

void audio_exit_stream()
{
}


/*
 *  Streaming function
 */

static int audio_prefetch_func(void *arg)
{
	while (!SDL_AtomicGet(&audio_prefetch_quit)) {
		const int source_count = AudioStatus.num_sources;
		if (source_count == 0) {
			SDL_Delay(AUDIO_PREFETCH_RETRY_MS);
			continue;
		}

		SDL_LockAudio();
		const int queued = SDL_AudioStreamAvailable(audio_prefetch_stream);
		SDL_UnlockAudio();
		/* Keep two host blocks queued. With a single block, any interrupt
		 * service delay longer than one callback period (seen: 20-30 ms while
		 * the guest busy-waits at interrupt level during movie startup) drains
		 * the stream to zero and the callback emits audible silence. */
		if (queued >= 2 * audio_callback_bytes) {
			SDL_Delay(AUDIO_PREFETCH_RETRY_MS);
			continue;
		}

#if QD3D_AUDIO_LOGGING_ENABLED
		const uint64 request_counter = SDL_GetPerformanceCounter();
#endif
		SetInterruptFlag(INTFLAG_AUDIO);
		TriggerInterrupt();
		SDL_SemWait(audio_irq_done_sem);
		if (SDL_AtomicGet(&audio_prefetch_quit))
			break;
		if (AudioStatus.num_sources != source_count)
			continue;

		const int bytes = audio_fetch_bytes;
		if (bytes <= 0) {
			SDL_LockAudio();
			SDL_AudioStreamClear(audio_prefetch_stream);
			SDL_UnlockAudio();
			SDL_Delay(AUDIO_PREFETCH_RETRY_MS);
			continue;
		}

#if QD3D_AUDIO_LOGGING_ENABLED
		static uint64 content_block_count;
		static uint32 prior_content_hash;
		static bool prior_content_silent = true;
		static uint32 last_content_log_tick;
		uint32 content_hash = 2166136261u;
		bool content_silent = true;
		for (int i = 0; i < bytes; i++) {
			content_hash = (content_hash ^ audio_fetch_buf[i]) * 16777619u;
			content_silent &= audio_fetch_buf[i] == silence_byte;
		}
		content_block_count++;
		const bool content_repeated = content_block_count > 1 &&
			content_hash == prior_content_hash;
		const uint32 content_tick = ReadMacInt32(0x016a);
		if ((content_silent != prior_content_silent ||
		     (content_repeated && !content_silent)) &&
		    content_tick - last_content_log_tick >= 6) {
			QD3D_AUDIO_LOG("SDL prefetch content block=%llu macTick=%u bytes=%d hash=%08x previous=%08x silent=%u repeated=%u",
			                (unsigned long long)content_block_count, content_tick,
			                bytes, content_hash, prior_content_hash,
			                content_silent ? 1u : 0u,
			                content_repeated ? 1u : 0u);
			last_content_log_tick = content_tick;
		}
		prior_content_hash = content_hash;
		prior_content_silent = content_silent;
#endif

		SDL_LockAudio();
		SDL_AudioStreamPut(audio_prefetch_stream, audio_fetch_buf, bytes);
		SDL_UnlockAudio();

#if QD3D_AUDIO_LOGGING_ENABLED
		const uint64 ack_usec =
			(SDL_GetPerformanceCounter() - request_counter) * 1000000u /
			SDL_GetPerformanceFrequency();
		if (ack_usec >= 10000)
			QD3D_AUDIO_LOG("SDL prefetch delayed macTick=%u ackUsec=%llu queued=%d bytes=%d",
			                ReadMacInt32(0x016a),
			                (unsigned long long)ack_usec, queued, bytes);
#endif
	}
	return 0;
}


static void stream_func(void *arg, uint8 *stream, int stream_len)
{
#if QD3D_AUDIO_LOGGING_ENABLED
	static int prior_source_count = 0;
	static uint64 active_callback_count = 0;
	static uint64 prior_callback_counter = 0;
	static uint32 last_callback_log_tick = 0;
	const uint64 callback_counter = SDL_GetPerformanceCounter();
	const uint64 callback_delta_usec = prior_callback_counter == 0 ? 0 :
		(callback_counter - prior_callback_counter) * 1000000u /
		SDL_GetPerformanceFrequency();
	prior_callback_counter = callback_counter;
	const uint64 nominal_callback_usec =
		(uint64)audio_frames_per_block * 1000000u /
		(AudioStatus.sample_rate >> 16);
	const bool callback_late = callback_delta_usec >
		nominal_callback_usec + 10000u;
	const int source_count = AudioStatus.num_sources;
	if (source_count != 0 && prior_source_count == 0)
		active_callback_count = 0;
	prior_source_count = source_count;
#else
	const int source_count = AudioStatus.num_sources;
#endif
	memset(stream, silence_byte, stream_len);
	if (source_count) {
#if QD3D_AUDIO_LOGGING_ENABLED
		active_callback_count++;
		const int queued = SDL_AudioStreamAvailable(audio_prefetch_stream);
#endif
		const int copied = SDL_AudioStreamGet(audio_prefetch_stream,
			audio_mix_buf, stream_len);
#if QD3D_AUDIO_LOGGING_ENABLED
		const bool callback_underrun = copied < stream_len;
		const uint32 callback_tick = ReadMacInt32(0x016a);
		if ((callback_underrun || callback_late) &&
		    (callback_underrun || callback_tick - last_callback_log_tick >= 30)) {
			uint32 hash = 2166136261u;
			for (int i = 0; i < copied; i += 64)
				hash = (hash ^ audio_mix_buf[i]) * 16777619u;
			QD3D_AUDIO_LOG("SDL callback run=%llu macTick=%u deltaUsec=%llu nominalUsec=%llu sources=%d queued=%d copied=%d requested=%d hash=%08x late=%u underrun=%u",
			                (unsigned long long)active_callback_count,
			                callback_tick,
			                (unsigned long long)callback_delta_usec,
			                (unsigned long long)nominal_callback_usec,
			                source_count, queued, copied, stream_len, hash,
			                callback_late ? 1u : 0u,
			                callback_underrun ? 1u : 0u);
			last_callback_log_tick = callback_tick;
		}
#endif
		if (copied > 0 && !main_mute && !speaker_mute)
			SDL_MixAudio(stream, audio_mix_buf, copied, get_audio_volume());
	}
	
#if defined(BINCUE)
#if QD3D_AUDIO_LOGGING_ENABLED
	const uint64 cd_mix_counter = SDL_GetPerformanceCounter();
#endif
	MixAudio_bincue(stream, stream_len);
#if QD3D_AUDIO_LOGGING_ENABLED
	const uint64 cd_mix_usec =
		(SDL_GetPerformanceCounter() - cd_mix_counter) * 1000000u /
		SDL_GetPerformanceFrequency();
	if (cd_mix_usec >= 10000u)
		QD3D_AUDIO_LOG("BIN/CUE callback mixUsec=%llu bytes=%d",
		                (unsigned long long)cd_mix_usec, stream_len);
#endif
#endif
	
}


/*
 *  MacOS audio interrupt, read next data block
 */

void AudioInterrupt(void)
{
	D(bug("AudioInterrupt\n"));
	audio_fetch_bytes = 0;

	// Get data from apple mixer
	if (AudioStatus.mixer) {
		M68kRegisters r;
		r.a[0] = audio_data + adatStreamInfo;
		r.a[1] = AudioStatus.mixer;
		Execute68k(audio_data + adatGetSourceData, &r);
		const uint32 info = ReadMacInt32(audio_data + adatStreamInfo);
		if (info) {
			int bytes = ReadMacInt32(info + scd_sampleCount) *
				(AudioStatus.sample_size >> 3) * AudioStatus.channels;
			if (bytes > audio_callback_bytes)
				bytes = audio_callback_bytes;
			if (bytes > 0) {
				const bool double_mono_8 = AudioStatus.channels == 2 &&
					ReadMacInt16(info + scd_numChannels) == 1 &&
					ReadMacInt16(info + scd_sampleSize) == 8;
				const uint8 *src = Mac2HostAddr(ReadMacInt32(info + scd_buffer));
				if (double_mono_8) {
					for (int i = 0; i < bytes; i += 2)
						audio_fetch_buf[i] = audio_fetch_buf[i + 1] = src[i >> 1];
				} else {
					memcpy(audio_fetch_buf, src, bytes);
				}
				audio_fetch_bytes = bytes;
			}
		}
#if QD3D_AUDIO_LOGGING_ENABLED
		AudioDiagnosticPoll();
#endif
		D(bug(" GetSourceData() returns %08lx\n", r.d[0]));
	} else
		WriteMacInt32(audio_data + adatStreamInfo, 0);

	// Signal stream function
	SDL_SemPost(audio_irq_done_sem);
	D(bug("AudioInterrupt done\n"));
}


/*
 *  Set sampling parameters
 *  "index" is an index into the audio_sample_rates[] etc. vectors
 *  It is guaranteed that AudioStatus.num_sources == 0
 */

bool audio_set_sample_rate(int index)
{
	close_audio();
	audio_sample_rate_index = index;
	return open_audio();
}

bool audio_set_sample_size(int index)
{
	close_audio();
	audio_sample_size_index = index;
	return open_audio();
}

bool audio_set_channels(int index)
{
	close_audio();
	audio_channel_count_index = index;
	return open_audio();
}


/*
 *  Get/set volume controls (volume values received/returned have the left channel
 *  volume in the upper 16 bits and the right channel volume in the lower 16 bits;
 *  both volumes are 8.8 fixed point values with 0x0100 meaning "maximum volume"))
 */

bool audio_get_main_mute(void)
{
	return main_mute;
}

uint32 audio_get_main_volume(void)
{
	uint32 chan = main_volume;
	return (chan << 16) + chan;
}

bool audio_get_speaker_mute(void)
{
	return speaker_mute;
}

uint32 audio_get_speaker_volume(void)
{
	uint32 chan = speaker_volume;
	return (chan << 16) + chan;
}

void audio_set_main_mute(bool mute)
{
	main_mute = mute;
}

void audio_set_main_volume(uint32 vol)
{
	// We only have one-channel volume right now.
	main_volume = ((vol >> 16) + (vol & 0xffff)) / 2;
	if (main_volume > MAC_MAX_VOLUME)
		main_volume = MAC_MAX_VOLUME;
}

void audio_set_speaker_mute(bool mute)
{
	speaker_mute = mute;
}

void audio_set_speaker_volume(uint32 vol)
{
	// We only have one-channel volume right now.
	speaker_volume = ((vol >> 16) + (vol & 0xffff)) / 2;
	if (speaker_volume > MAC_MAX_VOLUME)
		speaker_volume = MAC_MAX_VOLUME;
}

static int get_audio_volume() {
	return main_volume * speaker_volume * SDL_MIX_MAXVOLUME / (MAC_MAX_VOLUME * MAC_MAX_VOLUME);
}

#if SDL_VERSION_ATLEAST(2,0,0)
static int play_startup(void *arg) {
	SDL_AudioSpec wav_spec;
	Uint8 *wav_buffer;
	Uint32 wav_length;
	if (SDL_LoadWAV("startup.wav", &wav_spec, &wav_buffer, &wav_length)) {
		SDL_AudioSpec obtained;
		SDL_AudioDeviceID deviceId = SDL_OpenAudioDevice(NULL, 0, &wav_spec, &obtained, 0);
		if (deviceId) {
			SDL_QueueAudio(deviceId, wav_buffer, wav_length);
			SDL_PauseAudioDevice(deviceId, 0);
			while (SDL_GetQueuedAudioSize(deviceId)) SDL_Delay(10);
			SDL_Delay(500);
			SDL_CloseAudioDevice(deviceId);
		}
		else printf("play_startup: Audio driver failed to initialize\n");
		SDL_FreeWAV(wav_buffer);
	}
	return 0;
}

void PlayStartupSound() {
	SDL_CreateThread(play_startup, "", NULL);
}
#else
void PlayStartupSound() {
    // Not implemented
}
#endif
#endif	// SDL_VERSION_ATLEAST


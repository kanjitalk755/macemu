/*
 *  audio_sdl3.cpp - Audio support, SDL implementation
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
#if SDL_VERSION_ATLEAST(3, 0, 0)

#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "user_strings.h"
#include "audio.h"
#include "audio_defs.h"

#include <queue>

#define DEBUG 0
#include "debug.h"

#define MONITOR_MAIN_STREAM 0
#define MONITOR_INTERRUPT_STREAM 0
#define DISPLAY_EVERY 4

#if defined(BINCUE)
#include "bincue.h"
#endif


#define MAC_MAX_VOLUME 0x0100

#define MAIN_STREAM_EXTRA_DATA_MARGIN_MS 3
#define INTERRUPT_STREAM_QUEUE_TARGET_MS 5
#define INTERRUPT_RETRY_MS 5

// The currently selected audio parameters (indices in audio_sample_rates[] etc. vectors)
static int audio_sample_rate_index = 0;
static int audio_sample_size_index = 0;
static int audio_channel_count_index = 0;

// Global variables
static SDL_Semaphore *audio_irq_done_sem = NULL;	// Signal from interrupt to streaming thread: data block read
static uint8 silence_byte;							// Byte value to use to fill sound buffers with silence
static int main_volume = MAC_MAX_VOLUME;
static int speaker_volume = MAC_MAX_VOLUME;
static bool main_mute = false;
static bool speaker_mute = false;

static SDL_Thread *startup_thread;
volatile static bool playing_startup, exit_startup;
SDL_AudioSpec audio_spec;

// Prototypes
static void SDLCALL stream_func(void *arg, SDL_AudioStream *stream, int additional_amount, int total_amount);
static float get_audio_volume();
static void start_threads();
static void stop_threads();

static SDL_Thread * interrupt_thread = NULL;
static int interrupt_thread_func(void *data);
static bool interrupt_thread_quit;
static SDL_AudioStream * interrupt_stream = NULL;

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

static SDL_AudioStream * main_open_sdl_stream = NULL;

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
		audio_sample_rate_index = (int)audio_sample_rates.size() - 1;
		audio_sample_size_index = (int)audio_sample_sizes.size() - 1;
		audio_channel_count_index = (int)audio_channel_counts.size() - 1;
	}

	//memset(&audio_spec, 0, sizeof(audio_spec));
	audio_spec.format = (audio_sample_sizes[audio_sample_size_index] == 8) ? SDL_AUDIO_U8 : SDL_AUDIO_S16BE;
	audio_spec.channels = audio_channel_counts[audio_channel_count_index];
	audio_spec.freq = audio_sample_rates[audio_sample_rate_index] >> 16;

	D(bug("Opening SDL audio device stream, freq %d chan %d format %s\n", audio_spec.freq, audio_spec.channels,
		SDL_GetAudioFormatName(audio_spec.format)));

	assert(!main_open_sdl_stream);

	// Open the audio device, forcing the desired format
	SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, stream_func, NULL);
	if (stream == NULL) {
		fprintf(stderr, "WARNING: Cannot open audio: %s\n", SDL_GetError());
		return false;
	}
	main_open_sdl_stream = stream;
	silence_byte = SDL_GetSilenceValueForFormat(audio_spec.format);
#if defined(BINCUE)
	OpenAudio_bincue(audio_spec.freq, audio_spec.format, audio_spec.channels, silence_byte, (int)(get_audio_volume()*128));
#endif

	printf("Using SDL/%s audio output\n", SDL_GetCurrentAudioDriver());
	audio_frames_per_block = 4096 >> PrefsFindInt32("sound_buffer");
	start_threads();
	SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(stream));
	return true;
}

static void start_threads() {
	interrupt_stream = SDL_CreateAudioStream(&audio_spec, &audio_spec);
	assert(interrupt_thread == NULL);
	interrupt_thread_quit = false;
	interrupt_thread = SDL_CreateThread(interrupt_thread_func, "audio_sdl3_interrupt_thread", NULL);
}

static void stop_threads() {
	exit_startup = true;
	if (startup_thread != NULL)
		SDL_WaitThread(startup_thread, NULL);
	startup_thread = NULL;
	//
	interrupt_thread_quit = true;
	if (interrupt_thread != NULL)
		SDL_WaitThread(interrupt_thread, NULL);
	interrupt_thread = NULL;
	SDL_DestroyAudioStream(interrupt_stream);
}

static bool close_sdl_audio() {
	stop_threads();
	if (main_open_sdl_stream) {
		SDL_DestroyAudioStream(main_open_sdl_stream);
		main_open_sdl_stream = NULL;
		return true;
	}
	return false;
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
	exit_startup = true;
	while (playing_startup)
		SDL_Delay(10);
	exit_startup = false;
	// Close audio device
	close_sdl_audio();
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

static int time_to_stream_bytes(int time_ms) {
	// fraction in seconds
	int time_numerator = time_ms;
	int time_denominator = 100;

	// sample size across all channels
	int sample_size = AudioStatus.channels * (AudioStatus.sample_size >> 3); // bytes
	// Take care with data types
	// - AudioStatus.sample_rate Hz is in 16.16 fixed point and will overflow if we multiply its uint32 by even 2
	int time_samples = ((int)(((uint64)AudioStatus.sample_rate * time_numerator) >> 16) / time_denominator);
	// - We want a number of bytes that is an integer multiple of a sample for each channel
	return time_samples * sample_size;
}

static int interrupt_thread_func(void *data)
{
	while (!interrupt_thread_quit) {

	int target_queue_size = time_to_stream_bytes(INTERRUPT_STREAM_QUEUE_TARGET_MS);

#if MONITOR_INTERRUPT_STREAM
		static int monitor_stream_count = 0;
		if (monitor_stream_count++ % DISPLAY_EVERY == 0)
			bug("audio mac interrupt thread: target_queue_size %5d q %6d\n",
				target_queue_size, SDL_GetAudioStreamQueued(interrupt_stream));
#endif

	if (AudioStatus.num_sources) {
		while (SDL_GetAudioStreamQueued(interrupt_stream) < target_queue_size) {
			// Trigger audio interrupt to get new buffer
			D(bug("stream: triggering irq\n"));
			SetInterruptFlag(INTFLAG_AUDIO);
			TriggerInterrupt();
			D(bug("stream: waiting for ack\n"));
			SDL_WaitSemaphore(audio_irq_done_sem);
			D(bug("stream: ack received\n"));
			
			// Get size of audio data
			uint32 apple_stream_info = ReadMacInt32(audio_data + adatStreamInfo);
			if (apple_stream_info) {

				bool known_audio_format;
				int source_sample_size;
				SDL_AudioFormat source_format;
				uint32 fourcc = ReadMacInt32(apple_stream_info + scd_format);
				switch (fourcc) {
					case FOURCC('t','w','o','s'):
						known_audio_format = true;
						source_sample_size = 16;
						source_format = SDL_AUDIO_S16BE;
						break;
					case FOURCC('r','a','w',' '):
						known_audio_format = true;
						source_sample_size = 8;
						source_format = SDL_AUDIO_U8;
						break;
					default:
						// bug("SoundComponentData in unsupported format fourcc '%c%c%c%c'\n",
							// (fourcc >> 24)&0xff, (fourcc >> 16)&0xff, (fourcc >> 8)&0xff, fourcc&0xff);
						known_audio_format = false;
						// We can't do anything with the source data but we know the duration,
						// so we generate an appropriate silence
						// Set a placeholder source format to use for it:
						source_sample_size = 8;
						source_format = SDL_AUDIO_U8;
				}

				uint16 source_channels = ReadMacInt16(apple_stream_info + scd_numChannels);

				int work_size = ReadMacInt32(apple_stream_info + scd_sampleCount) * (source_sample_size >> 3) * source_channels;
				if (work_size == 0)
					break; // no more audio available right now

				uint8 buf[work_size];

				uint32 source_sample_rate = ReadMacInt32(apple_stream_info + scd_sampleRate);

				SDL_AudioSpec current_scd_spec = {source_format, source_channels, (uint16)(source_sample_rate >> 16)};
				//bug("scd channels %d sr %d 0x%x >>16%d\n", source_channels, source_sample_rate, source_sample_rate, source_sample_rate>>16);

				SDL_SetAudioStreamFormat(interrupt_stream, &current_scd_spec, NULL);

				if (known_audio_format && !main_mute && !speaker_mute) {
					Mac2Host_memcpy(buf, ReadMacInt32(apple_stream_info + scd_buffer), work_size);
				} else {
					memset(buf, SDL_GetSilenceValueForFormat(source_format), work_size);
				}

				SDL_PutAudioStreamData(interrupt_stream, buf, work_size);
			}
			else {
				SDL_ClearAudioStream(interrupt_stream);
				break;
			}
		}
	}

	// Audio isn't active or the mac doesn't have any right now.
	// Wait a little while.
	SDL_Delay(INTERRUPT_RETRY_MS);

	} // while
	return 0;
}

static void SDLCALL stream_func(void *, SDL_AudioStream *stream, int stream_len, int total_amount)
{
	int target_queue_size;
	int margin;
	if (stream_len == 0) {
		// This indicates that SDL3 really has all the data it wants right now.
		// This is our backpressure state, where we avoid pushing even more
		// which prevents non-real-time audio situations (like playing media with audio)
		// from getting unnecessarily ahead
		return;
	} else {
		// We want to supply a little more data than was requested to prevent underruns
		// Figure out a fraction of a second of data to use
		margin = time_to_stream_bytes(MAIN_STREAM_EXTRA_DATA_MARGIN_MS);
		target_queue_size = stream_len + margin;
	}

	int bytes_available = SDL_GetAudioStreamAvailable(interrupt_stream);
	if (bytes_available > stream_len) {
		// push any extra bytes, up to the target number, right away
		stream_len = std::min(bytes_available, target_queue_size);
	} else if (bytes_available == 0) {
#if defined(BINCUE)
		if (HaveAudioToMix_bincue()) {
			// we are driving the rate entirely on behalf of the CD audio
			stream_len = target_queue_size;
		}
#endif
	}

#if MONITOR_MAIN_STREAM
		static int monitor_stream_count = 0;
		if (monitor_stream_count++ % DISPLAY_EVERY == 0)
			bug("audio main sdl3 stream callback: stream_len %5d already sent %5d margin %5d target_q %5d q %6ld\n",
				stream_len, total_amount, margin, target_queue_size, bytes_available);
#endif

	uint8 src[stream_len], dst[stream_len];
	int i = SDL_GetAudioStreamData(interrupt_stream, src, stream_len);
	if (i < stream_len)
		memset(src + i, silence_byte, stream_len - i);
	memset(dst, silence_byte, stream_len);
	//SDL_AudioSpec audio_spec;
	//int r = SDL_GetAudioStreamFormat(stream, NULL, &audio_spec);// little endian
	SDL_MixAudio(dst, src, audio_spec.format, stream_len, get_audio_volume());
#if defined(BINCUE)
	MixAudio_bincue(dst, stream_len);
#endif
	SDL_PutAudioStreamData(stream, dst, stream_len);
}


/*
 *  MacOS audio interrupt, read next data block
 */

void AudioInterrupt(void)
{
	D(bug("AudioInterrupt\n"));

	// Get data from apple mixer
	if (AudioStatus.mixer) {
		M68kRegisters r;
		r.a[0] = audio_data + adatStreamInfo;
		r.a[1] = AudioStatus.mixer;
		Execute68k(audio_data + adatGetSourceData, &r);
		D(bug(" GetSourceData() returns %08lx\n", r.d[0]));
	} else
		WriteMacInt32(audio_data + adatStreamInfo, 0);

	// Signal stream function
	SDL_SignalSemaphore(audio_irq_done_sem);
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

static float get_audio_volume() {
	return (float) main_volume * speaker_volume / (MAC_MAX_VOLUME * MAC_MAX_VOLUME);
}

static int play_startup(void *arg) {
	SDL_AudioSpec wav_spec;
	Uint8 *wav_buffer;
	Uint32 wav_length;
	if (!playing_startup && SDL_LoadWAV("startup.wav", &wav_spec, &wav_buffer, &wav_length)) {
		SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &wav_spec, NULL, NULL);
		if (stream) {
			SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(stream));
			SDL_PutAudioStreamData(stream, wav_buffer, wav_length);
			playing_startup = true;
			while (!exit_startup && SDL_GetAudioStreamAvailable(stream)) SDL_Delay(10);
			if (!exit_startup) SDL_Delay(500);
			SDL_DestroyAudioStream(stream);
		}
		else printf("play_startup: Audio driver failed to initialize\n");
		SDL_free(wav_buffer);
		playing_startup = false;
	}
	return 0;
}

void PlayStartupSound() {
	startup_thread = SDL_CreateThread(play_startup, "play_startup", NULL);
}

#endif	// SDL_VERSION_ATLEAST

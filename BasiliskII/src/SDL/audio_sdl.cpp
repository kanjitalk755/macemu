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

#if defined(QD3D_INIT_LOGGING_ENABLED) && QD3D_INIT_LOGGING_ENABLED
#include "qd3d_init_logging.h"
#else
#define QD3D_AUDIO_LOG(...) do { } while (0)
#endif

#define DEBUG 0
#include "debug.h"

#if defined(BINCUE)
#include "bincue.h"
#endif


#define MAC_MAX_VOLUME 0x0100

// The currently selected audio parameters (indices in audio_sample_rates[] etc. vectors)
static int audio_sample_rate_index = 0;
static int audio_sample_size_index = 0;
static int audio_channel_count_index = 0;

// Global variables
static SDL_sem *audio_irq_done_sem = NULL;			// Signal from interrupt to streaming thread: data block read
static uint8 silence_byte;							// Byte value to use to fill sound buffers with silence
static uint8 *audio_mix_buf = NULL;
static int main_volume = MAC_MAX_VOLUME;
static int speaker_volume = MAC_MAX_VOLUME;
static bool main_mute = false;
static bool speaker_mute = false;

// Prototypes
static void stream_func(void *arg, uint8 *stream, int stream_len);
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

	// Sound buffer size = 4096 frames
	audio_frames_per_block = audio_spec.samples;
	audio_mix_buf = (uint8*)malloc(audio_spec.size);
	memset(audio_mix_buf, silence_byte, audio_spec.size);
	/* Do not let the callback observe an unallocated first-use mix buffer. */
	SDL_PauseAudio(0);
	QD3D_AUDIO_LOG("SDL open complete driver=%s size=%u silence=0x%02x",
	                driver_name ? driver_name : "", audio_spec.size,
	                silence_byte);
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
	/* Stop and join the callback before destroying the BIN/CUE conversion
	 * streams it can access from MixAudio_bincue. */
	QD3D_AUDIO_LOG("SDL close format=%uHz/%ubit/%uch sources=%d",
	                AudioStatus.sample_rate >> 16, AudioStatus.sample_size,
	                AudioStatus.channels, AudioStatus.num_sources);
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	#if defined(BINCUE)
	CloseAudio_bincue();
	#endif
	free(audio_mix_buf);
	audio_mix_buf = NULL;
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

static void stream_func(void *arg, uint8 *stream, int stream_len)
{
	static int prior_source_count = 0;
	static uint64 active_callback_count = 0;
	const int source_count = AudioStatus.num_sources;
	if (source_count != 0 && prior_source_count == 0)
		active_callback_count = 0;
	prior_source_count = source_count;
	if (AudioStatus.num_sources) {
		active_callback_count++;
		// Trigger audio interrupt to get new buffer
		D(bug("stream: triggering irq\n"));
		SetInterruptFlag(INTFLAG_AUDIO);
		TriggerInterrupt();
		D(bug("stream: waiting for ack\n"));
		SDL_SemWait(audio_irq_done_sem);
		D(bug("stream: ack received\n"));

		// Get size of audio data
		uint32 apple_stream_info = ReadMacInt32(audio_data + adatStreamInfo);
		if (apple_stream_info && !main_mute && !speaker_mute) {
			const uint32 format = ReadMacInt32(apple_stream_info + scd_format);
			const uint16 source_channels = ReadMacInt16(apple_stream_info + scd_numChannels);
			const uint16 source_bits = ReadMacInt16(apple_stream_info + scd_sampleSize);
			const uint32 source_rate = ReadMacInt32(apple_stream_info + scd_sampleRate);
			const uint32 source_frames = ReadMacInt32(apple_stream_info + scd_sampleCount);
			const uint32 source_buffer = ReadMacInt32(apple_stream_info + scd_buffer);
			int work_size = ReadMacInt32(apple_stream_info + scd_sampleCount) * (AudioStatus.sample_size >> 3) * AudioStatus.channels;
			D(bug("stream: work_size %d\n", work_size));
			if (work_size > stream_len)
				work_size = stream_len;
			if (work_size == 0)
				goto silence;

			// Send data to audio device
			bool dbl = AudioStatus.channels == 2 &&
				ReadMacInt16(apple_stream_info + scd_numChannels) == 1 &&
				ReadMacInt16(apple_stream_info + scd_sampleSize) == 8;
			uint8 *src = Mac2HostAddr(ReadMacInt32(apple_stream_info + scd_buffer));
#if defined(QD3D_INIT_LOGGING_ENABLED) && QD3D_INIT_LOGGING_ENABLED
			uint32 hash = 2166136261u;
			const uint32 source_bytes = source_frames * (source_bits >> 3) * source_channels;
			const uint32 hash_bytes = source_bytes < 4096u ? source_bytes : 4096u;
			for (uint32 i = 0; i < hash_bytes; i++)
				hash = (hash ^ src[i]) * 16777619u;
			if (active_callback_count <= 32u ||
			    (active_callback_count % 120u) == 0) {
				QD3D_AUDIO_LOG("SDL callback run=%llu info=0x%08x buffer=0x%08x format=%c%c%c%c %uHz/%ubit/%uch frames=%u sourceBytes=%u outputBytes=%d hash=%08x",
				                (unsigned long long)active_callback_count,
				                apple_stream_info, source_buffer,
				                format >> 24, (format >> 16) & 0xff,
				                (format >> 8) & 0xff, format & 0xff,
				                source_rate >> 16, source_bits, source_channels,
				                source_frames, source_bytes, work_size, hash);
			}
#endif
			if (dbl)
				for (int i = 0; i < work_size; i += 2)
					audio_mix_buf[i] = audio_mix_buf[i + 1] = src[i >> 1];
			else memcpy(audio_mix_buf, src, work_size);
			memset((uint8 *)stream, silence_byte, stream_len);
			SDL_MixAudio(stream, audio_mix_buf, work_size, get_audio_volume());

			D(bug("stream: data written\n"));

		} else
			goto silence;

	} else {

		// Audio not active, play silence
		silence: memset(stream, silence_byte, stream_len);
	}
	
#if defined(BINCUE)
	MixAudio_bincue(stream, stream_len);
#endif
	
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
		static uint64 interrupt_count = 0;
		interrupt_count++;
		if (interrupt_count <= 32u || (interrupt_count % 120u) == 0) {
			QD3D_AUDIO_LOG("AudioInterrupt count=%llu mixer=0x%08x result=%d streamInfo=0x%08x sources=%d",
			                (unsigned long long)interrupt_count,
			                AudioStatus.mixer, (int32)r.d[0],
			                ReadMacInt32(audio_data + adatStreamInfo),
			                AudioStatus.num_sources);
		}
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
	QD3D_AUDIO_LOG("Set host sample rate index=%d old=%u new=%u",
	                index, AudioStatus.sample_rate >> 16,
	                audio_sample_rates[index] >> 16);
	close_audio();
	audio_sample_rate_index = index;
	return open_audio();
}

bool audio_set_sample_size(int index)
{
	QD3D_AUDIO_LOG("Set host sample size index=%d old=%u new=%u",
	                index, AudioStatus.sample_size, audio_sample_sizes[index]);
	close_audio();
	audio_sample_size_index = index;
	return open_audio();
}

bool audio_set_channels(int index)
{
	QD3D_AUDIO_LOG("Set host channels index=%d old=%u new=%u",
	                index, AudioStatus.channels, audio_channel_counts[index]);
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


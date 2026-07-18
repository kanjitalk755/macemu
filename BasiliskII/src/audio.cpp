/*
 *  audio.cpp - Audio support
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *  Portions written by Marc Hellwig
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
 *  SEE ALSO
 *    Inside Macintosh: Sound, chapter 5 "Sound Components"
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "macos_util.h"
#include "emul_op.h"
#include "main.h"
#include "audio.h"
#include "audio_defs.h"
#include "user_strings.h"
#include "cdrom.h"

#if defined(SHEEPSHAVER)
#include "qd3d_init_logging.h"
#elif defined(QD3D_AUDIO_LOGGING_ENABLED) && QD3D_AUDIO_LOGGING_ENABLED
#include "qd3d_init_logging.h"
#else
#ifndef QD3D_AUDIO_LOGGING_ENABLED
#define QD3D_AUDIO_LOGGING_ENABLED 0
#endif
#define QD3D_AUDIO_LOG(...) do { } while (0)
#endif
#ifndef DESCENT_MOVIE_UNPAUSE_PRIME
#define DESCENT_MOVIE_UNPAUSE_PRIME 0
#endif

#define DEBUG 0
#include "debug.h"


// Supported sample rates, sizes and channels
vector<uint32> audio_sample_rates;
vector<uint16> audio_sample_sizes;
vector<uint8> audio_channel_counts;

// Global variables
struct audio_status AudioStatus;	// Current audio status (sample rate etc.)
bool audio_open = false;			// Flag: audio is initialized and ready
int audio_frames_per_block;			// Number of audio frames per block
uint32 audio_component_flags;		// Component feature flags
uint32 audio_data = 0;				// Mac address of global data area
static int open_count = 0;			// Open/close nesting count
static uint32 audio_sound_clock_ci = 0;	// First siSoundClock ComponentInstance returned by the mixer

uint32 AudioGetSoundClockCI(void)
{
	return audio_sound_clock_ci;
}

#if QD3D_AUDIO_LOGGING_ENABLED
static uint32 diagnostic_source;
static uint32 diagnostic_source_pb;
static int16 diagnostic_pb_result;
static uint32 diagnostic_pb_frames;
static uint32 diagnostic_pb_data;
static uint32 diagnostic_poll_count;
#endif

/*
 *  Service pending audio interrupt from thrash hot paths.
 *
 *  Guest 68k busy-polls (Microseconds / GetTime) can starve OP_IRQ so
 *  INTFLAG_AUDIO sits pending while the mixer stops rotating. Servicing
 *  the flag here keeps audio progressing.
 *
 *  Do NOT call TimerInterrupt / consume VIA from here: movie moreRtn and
 *  other TM tasks nested under EMUL_OP/GetTime have corrupted PPC state
 *  (execute_illegal abort). Leave timer/VBL to OP_IRQ.
 */
void AudioServicePendingInterrupt(void)
{
	/* If an interrupt is already running (e.g. inside the moreRtn it will
	 * call), do not clear INTFLAG_AUDIO and do not re-enter. Leave the flag
	 * set so the normal OP_IRQ/callback path handles the next period. */
	if (audio_interrupt_in_service || !audio_open || !AudioStatus.mixer)
		return;
	if ((InterruptFlags & INTFLAG_AUDIO) == 0)
		return;
	ClearInterruptFlag(INTFLAG_AUDIO);
	AudioInterrupt();
}

/*
 *  Host-side streaming source servicing (Descent II intro class; general)
 *
 *  Some clients (Sound Manager bufferCmd/callback streams) issue
 *  PlaySourceBuffer(start) with an *empty* PB (frames=0, data=NULL) and a
 *  moreRtn, then feed 256-frame chunks through the PB. The Apple Mixer
 *  retires the empty buffer at start time and its mix loop never touches
 *  the source again, so it plays silence forever (observed: Descent II
 *  first-movie streaming source frozen at frames=256 while the mixer mixed
 *  silence, driving the player into a ~1 s timeout + audio restart = the
 *  intro stutter). Re-Play kicks, StartSource, and fail-fast errors all
 *  failed (see session notes 23.6; erroring the start hangs the client).
 *
 *  So the host services the source itself: consume the PB the way the mixer
 *  would (advance data / decrement frames), refill it by calling the PB's
 *  moreRtn (pascal Boolean(SoundParamBlockPtr *)) through a small 68k thunk,
 *  convert/mix the samples into the block fetched from the mixer.
 */
static uint32 stream_source;
static uint32 stream_pb;
static bool stream_armed;

bool audio_interrupt_in_service = false;

/* Sanity-check a Mac address before we read/write the streaming PB. */
static bool audio_stream_addr_ok(uint32 a)
{
	return a >= 0x1000 && a < 0x40000000 && Mac2HostAddr(a) != NULL;
}

static bool stream_call_pb_rtn(uint32 rtn)
{
	if (rtn == 0 || !audio_stream_addr_ok(stream_pb))
		return false;
	WriteMacInt32(audio_data + adatStreamPbVar, stream_pb);
	M68kRegisters r;
	r.a[0] = rtn;
	r.a[1] = audio_data + adatStreamPbVar;
	Execute68k(audio_data + adatCallMoreRtn, &r);
	stream_pb = ReadMacInt32(audio_data + adatStreamPbVar);
	/* Pascal Boolean result: byte in the high half of the popped word. */
	if (!audio_stream_addr_ok(stream_pb))
		return false;
	return stream_pb != 0 && (r.d[0] & 0xff00) != 0;
}

void AudioStreamHostMix(uint8 *buf, int *bytes, int want_bytes)
{
	if (!stream_armed || !audio_data || !audio_stream_addr_ok(stream_pb))
		return;
	if (AudioStatus.sample_size != 16 || AudioStatus.channels != 2)
		return;
	const uint32 out_rate = AudioStatus.sample_rate >> 16;
	const uint32 src_rate = ReadMacInt32(stream_pb + 16) >> 16;
	const uint16 src_bits = ReadMacInt16(stream_pb + 14);
	const uint16 src_ch = ReadMacInt16(stream_pb + 12);
	if (src_rate == 0 || out_rate < src_rate || out_rate % src_rate != 0) {
		stream_armed = false;
		return;
	}
	const uint32 dup = out_rate / src_rate;
	const uint32 src_frame_bytes = (src_bits >> 3) * src_ch;
	if ((src_bits != 16 && src_bits != 8) || src_ch < 1 || src_ch > 2 ||
	    src_frame_bytes == 0 || dup > 4) {
		stream_armed = false;
		return;
	}

	const int out_frames_wanted = want_bytes / 4;
	int out_frame = 0;
	uint32 chunks = 0;
	uint32 more_calls = 0;
	bool starved = false;

	/* Ensure the whole block exists so we can mix into it. */
	if (*bytes < want_bytes) {
		memset(buf + *bytes, 0, want_bytes - *bytes);
		*bytes = want_bytes;
	}

	while (out_frame < out_frames_wanted) {
		if (!audio_stream_addr_ok(stream_pb)) {
			stream_armed = false;
			starved = true;
			break;
		}
		uint32 frames = ReadMacInt32(stream_pb + 20);
		uint32 data = ReadMacInt32(stream_pb + 24);
		if (frames == 0 || data == 0) {
			/* Buffer drained: pull the next chunk. completionRtn must NOT
			 * be called here — it means "entire sound finished" and makes
			 * the Sound Manager retire the stream (run11). */
			more_calls++;
			if (more_calls > 64 ||
			    !stream_call_pb_rtn(ReadMacInt32(stream_pb + 48))) {
				starved = true;
				break;
			}
			continue;
		}
		chunks++;
		uint32 take = frames;
		const uint32 out_room = (uint32)(out_frames_wanted - out_frame) / dup;
		if (take > out_room)
			take = out_room;
		if (take == 0)
			break;
		for (uint32 f = 0; f < take; f++) {
			int16 l, rgt;
			if (src_bits == 16) {
				l = (int16)ReadMacInt16(data + f * src_frame_bytes);
				rgt = src_ch == 2 ?
					(int16)ReadMacInt16(data + f * src_frame_bytes + 2) : l;
			} else {
				l = (int16)((ReadMacInt8(data + f * src_frame_bytes) - 128) << 8);
				rgt = src_ch == 2 ?
					(int16)((ReadMacInt8(data + f * src_frame_bytes + 1) - 128) << 8) : l;
			}
			for (uint32 d = 0; d < dup; d++) {
				uint8 *o = buf + (uint32)(out_frame + f * dup + d) * 4;
				int ml = (int16)((o[0] << 8) | o[1]) + l;
				int mr = (int16)((o[2] << 8) | o[3]) + rgt;
				if (ml > 32767) ml = 32767; else if (ml < -32768) ml = -32768;
				if (mr > 32767) mr = 32767; else if (mr < -32768) mr = -32768;
				o[0] = (uint8)(ml >> 8); o[1] = (uint8)ml;
				o[2] = (uint8)(mr >> 8); o[3] = (uint8)mr;
			}
		}
		out_frame += take * dup;
		WriteMacInt32(stream_pb + 20, frames - take);
		WriteMacInt32(stream_pb + 24, data + take * src_frame_bytes);
	}

	static uint32 mix_n;
	mix_n++;
	if (mix_n <= 24 || (mix_n & 31) == 0 || starved) {
		QD3D_AUDIO_LOG("streamHostMix n=%u tick=%u pb=0x%08x chunks=%u "
		               "moreCalls=%u outFrames=%d/%d starved=%d fmt=%u/%u/%uch",
		               mix_n, ReadMacInt32(0x016a), stream_pb, chunks, more_calls,
		               out_frame, out_frames_wanted, starved ? 1 : 0,
		               src_rate, src_bits, src_ch);
	}
}

bool AudioAvailable = false;		// Flag: audio output available (from the software point of view)

int SoundInSource = 2;
int SoundInPlaythrough = 7;
int SoundInGain = 65536; // FIXED 4-byte from 0.5 to 1.5; this is middle value (1) as int

/*
 *  Reset audio emulation
 */

void AudioReset(void)
{
	audio_data = 0;
	#if QD3D_AUDIO_LOGGING_ENABLED
	diagnostic_source = 0;
	diagnostic_source_pb = 0;
	diagnostic_pb_result = 0;
	diagnostic_pb_frames = 0;
	diagnostic_pb_data = 0;
	diagnostic_poll_count = 0;
#endif
}


#if QD3D_AUDIO_LOGGING_ENABLED
void AudioDiagnosticPoll(void)
{
	if (!diagnostic_source_pb)
		return;

	diagnostic_poll_count++;
	const int16 result = ReadMacInt16(diagnostic_source_pb + 60);
	const uint32 frames = ReadMacInt32(diagnostic_source_pb + 20);
	const uint32 data = ReadMacInt32(diagnostic_source_pb + 24);
	const bool changed = result != diagnostic_pb_result ||
	                     frames != diagnostic_pb_frames ||
	                     data != diagnostic_pb_data;
	if (diagnostic_poll_count == 1 || changed ||
	    (diagnostic_poll_count & 31) == 0) {
		QD3D_AUDIO_LOG("SourcePB poll=%u tick=%u source=0x%08x pb=0x%08x frames=%u data=0x%08x moreRtn=0x%08x completionRtn=0x%08x refCon=0x%08x result=%d%s",
		                diagnostic_poll_count, ReadMacInt32(0x016a),
		                diagnostic_source, diagnostic_source_pb,
		                frames, data,
		                ReadMacInt32(diagnostic_source_pb + 48),
		                ReadMacInt32(diagnostic_source_pb + 52),
		                ReadMacInt32(diagnostic_source_pb + 56), result,
		                changed ? " changed" : "");
	}
	diagnostic_pb_result = result;
	diagnostic_pb_frames = frames;
	diagnostic_pb_data = data;
}
#endif


/*
 *  Get audio info
 */

static int32 AudioGetInfo(uint32 infoPtr, uint32 selector, uint32 sourceID)
{
	D(bug(" AudioGetInfo %c%c%c%c, infoPtr %08lx, source ID %08lx\n", selector >> 24, (selector >> 16) & 0xff, (selector >> 8) & 0xff, selector & 0xff, infoPtr, sourceID));
	M68kRegisters r;
	if (selector != siHardwareBusy)
		QD3D_AUDIO_LOG("GetInfo tick=%u selector=%c%c%c%c info=0x%08x source=0x%08x",
		                ReadMacInt32(0x016a), selector >> 24,
		                (selector >> 16) & 0xff, (selector >> 8) & 0xff,
		                selector & 0xff, infoPtr, sourceID);

	switch (selector) {
		case siSampleSize:
			WriteMacInt16(infoPtr, AudioStatus.sample_size);
			break;

		case siSampleSizeAvailable: {
			r.d[0] = audio_sample_sizes.size() * 2;
			Execute68kTrap(0xa122, &r);	// NewHandle()
			uint32 h = r.a[0];
			if (h == 0)
				return memFullErr;
			WriteMacInt16(infoPtr + sil_count, audio_sample_sizes.size());
			WriteMacInt32(infoPtr + sil_infoHandle, h);
			uint32 sp = ReadMacInt32(h);
			for (unsigned i=0; i<audio_sample_sizes.size(); i++)
				WriteMacInt16(sp + i*2, audio_sample_sizes[i]);
			break;
		}

		case siNumberChannels:
			WriteMacInt16(infoPtr, AudioStatus.channels);
			break;

		case siChannelAvailable: {
			r.d[0] = audio_channel_counts.size() * 2;
			Execute68kTrap(0xa122, &r);	// NewHandle()
			uint32 h = r.a[0];
			if (h == 0)
				return memFullErr;
			WriteMacInt16(infoPtr + sil_count, audio_channel_counts.size());
			WriteMacInt32(infoPtr + sil_infoHandle, h);
			uint32 sp = ReadMacInt32(h);
			for (unsigned i=0; i<audio_channel_counts.size(); i++)
				WriteMacInt16(sp + i*2, audio_channel_counts[i]);
			break;
		}

		case siSampleRate:
			WriteMacInt32(infoPtr, AudioStatus.sample_rate);
			break;

		case siSampleRateAvailable: {
			r.d[0] = audio_sample_rates.size() * 4;
			Execute68kTrap(0xa122, &r);	// NewHandle()
			uint32 h = r.a[0];
			if (h == 0)
				return memFullErr;
			WriteMacInt16(infoPtr + sil_count, audio_sample_rates.size());
			WriteMacInt32(infoPtr + sil_infoHandle, h);
			uint32 lp = ReadMacInt32(h);
			for (unsigned i=0; i<audio_sample_rates.size(); i++)
				WriteMacInt32(lp + i*4, audio_sample_rates[i]);
			break;
		}

		case siSpeakerMute:
			WriteMacInt16(infoPtr, audio_get_speaker_mute());
			break;

		case siSpeakerVolume:
			WriteMacInt32(infoPtr, audio_get_speaker_volume());
			break;

		case siHardwareMute:
			WriteMacInt16(infoPtr, audio_get_main_mute());
			break;

		case siHardwareVolume:
			WriteMacInt32(infoPtr, audio_get_main_volume());
			break;

		case siHardwareVolumeSteps:
			WriteMacInt16(infoPtr, 7);
			break;

		case siHardwareBusy:
			WriteMacInt16(infoPtr, AudioStatus.num_sources != 0);
			#if QD3D_AUDIO_LOGGING_ENABLED
			{
				static uint32 busy_poll_tick;
				static uint32 busy_poll_count;
				static uint32 busy_poll_value;
				const uint32 tick = ReadMacInt32(0x016a);
				if (busy_poll_count && tick != busy_poll_tick) {
					QD3D_AUDIO_LOG("GetInfo hardwareBusy tick=%u polls=%u value=%u sources=%d",
					                busy_poll_tick, busy_poll_count,
					                busy_poll_value,
					                AudioStatus.num_sources);
					busy_poll_count = 0;
				}
				busy_poll_tick = tick;
				busy_poll_value = AudioStatus.num_sources != 0;
				busy_poll_count++;
			}
			#endif
			break;

		case siHardwareFormat:
			WriteMacInt32(infoPtr + scd_flags, 0);
			WriteMacInt32(infoPtr + scd_format, AudioStatus.sample_size == 16 ? FOURCC('t','w','o','s') : FOURCC('r','a','w',' '));
			WriteMacInt16(infoPtr + scd_numChannels, AudioStatus.channels);
			WriteMacInt16(infoPtr + scd_sampleSize, AudioStatus.sample_size);
			WriteMacInt32(infoPtr + scd_sampleRate, AudioStatus.sample_rate);
			WriteMacInt32(infoPtr + scd_sampleCount, audio_frames_per_block);
			WriteMacInt32(infoPtr + scd_buffer, 0);
			WriteMacInt32(infoPtr + scd_reserved, 0);
			break;

		case siCompressionFactor: {
			const uint16 bytes_per_sample = AudioStatus.sample_size >> 3;
			WriteMacInt32(infoPtr + 0, 20);
			WriteMacInt32(infoPtr + 4, AudioStatus.sample_size == 16 ? FOURCC('t','w','o','s') : FOURCC('r','a','w',' '));
			WriteMacInt16(infoPtr + 8, 0); // notCompressed
			WriteMacInt16(infoPtr + 10, 1);
			WriteMacInt16(infoPtr + 12, bytes_per_sample);
			WriteMacInt16(infoPtr + 14, bytes_per_sample * AudioStatus.channels);
			WriteMacInt16(infoPtr + 16, bytes_per_sample);
			WriteMacInt16(infoPtr + 18, 0);
			break;
		}

		default:	// Delegate to Apple Mixer
			if (AudioStatus.mixer == 0)
				return badComponentSelector;
			M68kRegisters r;
			r.a[0] = infoPtr;
			r.d[0] = selector;
			r.a[1] = sourceID;
			r.a[2] = AudioStatus.mixer;
			#if QD3D_AUDIO_LOGGING_ENABLED
			const uint64 get_info_started = GetTicks_usec();
			#endif
			Execute68k(audio_data + adatGetInfo, &r);
			QD3D_AUDIO_LOG("GetInfo delegated tick=%u selector=%c%c%c%c source=0x%08x result=%d usec=%llu",
			                ReadMacInt32(0x016a), selector >> 24,
			                (selector >> 16) & 0xff, (selector >> 8) & 0xff,
			                selector & 0xff, sourceID, (int32)r.d[0],
			                (unsigned long long)(GetTicks_usec() - get_info_started));
		if (selector == siSoundClock && infoPtr && r.d[0] == noErr) {
			const uint32 ci = ReadMacInt32(infoPtr);
			if (audio_sound_clock_ci == 0 && ci != 0)
				audio_sound_clock_ci = ci;
		}
		D(bug("  delegated to Apple Mixer, returns %08lx\n", r.d[0]));
		return r.d[0];
	}
	return noErr;
}


/*
 *  Set audio info
 */

static int32 AudioSetInfo(uint32 infoPtr, uint32 selector, uint32 sourceID)
{
	D(bug(" AudioSetInfo %c%c%c%c, infoPtr %08lx, source ID %08lx\n", selector >> 24, (selector >> 16) & 0xff, (selector >> 8) & 0xff, selector & 0xff, infoPtr, sourceID));
	M68kRegisters r;
	QD3D_AUDIO_LOG("SetInfo tick=%u selector=%c%c%c%c info=0x%08x source=0x%08x",
	                ReadMacInt32(0x016a), selector >> 24,
	                (selector >> 16) & 0xff, (selector >> 8) & 0xff,
	                selector & 0xff, infoPtr, sourceID);

	switch (selector) {
		case siSampleSize:
			D(bug("  set sample size %08lx\n", infoPtr));
			if (AudioStatus.num_sources)
				return siDeviceBusyErr;
			if (infoPtr == AudioStatus.sample_size)
				return noErr;
			for (unsigned i=0; i<audio_sample_sizes.size(); i++)
				if (audio_sample_sizes[i] == infoPtr) {
					if (audio_set_sample_size(i))
						return noErr;
					else
						return siInvalidSampleSize;
				}
			return siInvalidSampleSize;

		case siSampleRate:
			D(bug("  set sample rate %08lx\n", infoPtr));
			if (AudioStatus.num_sources)
				return siDeviceBusyErr;
			if (infoPtr == AudioStatus.sample_rate)
				return noErr;
			for (unsigned i=0; i<audio_sample_rates.size(); i++)
				if (audio_sample_rates[i] == infoPtr) {
					if (audio_set_sample_rate(i))
						return noErr;
					else
						return siInvalidSampleRate;
				}
			return siInvalidSampleRate;

		case siNumberChannels:
			D(bug("  set number of channels %08lx\n", infoPtr));
			if (AudioStatus.num_sources)
				return siDeviceBusyErr;
			if (infoPtr == AudioStatus.channels)
				return noErr;
			for (unsigned i=0; i<audio_channel_counts.size(); i++)
				if (audio_channel_counts[i] == infoPtr) {
					if (audio_set_channels(i))
						return noErr;
					else
						return badChannel;
				}
			return badChannel;

		case siSpeakerMute:
			audio_set_speaker_mute(uint16(infoPtr) != 0);
			break;

		case siSpeakerVolume:
			D(bug("  set speaker volume %08lx\n", infoPtr));
			audio_set_speaker_volume(infoPtr);
			break;

		case siHardwareMute:
			audio_set_main_mute(uint16(infoPtr) != 0);
			break;

		case siHardwareVolume:
			D(bug("  set hardware volume %08lx\n", infoPtr));
			audio_set_main_volume(infoPtr);
			break;

		default:	// Delegate to Apple Mixer
			if (AudioStatus.mixer == 0)
				return badComponentSelector;
			r.a[0] = infoPtr;
			r.d[0] = selector;
			r.a[1] = sourceID;
			r.a[2] = AudioStatus.mixer;
			#if QD3D_AUDIO_LOGGING_ENABLED
			const uint64 set_info_started = GetTicks_usec();
			#endif
			Execute68k(audio_data + adatSetInfo, &r);
			const int32 set_result = r.d[0];
			QD3D_AUDIO_LOG("SetInfo delegated tick=%u selector=%c%c%c%c source=0x%08x result=%d usec=%llu",
			                ReadMacInt32(0x016a), selector >> 24,
			                (selector >> 16) & 0xff, (selector >> 8) & 0xff,
			                selector & 0xff, sourceID, set_result,
			                (unsigned long long)(GetTicks_usec() - set_info_started));
			D(bug("  delegated to Apple Mixer, returns %08lx\n", set_result));
			return set_result;
	}
	return noErr;
}


/*
 *  Sound output component dispatch
 */

int32 AudioDispatch(uint32 params, uint32 globals)
{
	D(bug("AudioDispatch params %08lx (size %d), what %d\n", params, ReadMacInt8(params + cp_paramSize), (int16)ReadMacInt16(params + cp_what)));
	M68kRegisters r;
	uint32 p = params + cp_params;
	int16 selector = (int16)ReadMacInt16(params + cp_what);
#if QD3D_AUDIO_LOGGING_ENABLED
	uint64 diagnostic_call_started = 0;
#endif

	switch (selector) {

		// General component functions
		case kComponentOpenSelect:
			if (audio_data == 0) {

				// Allocate global data area
				r.d[0] = SIZEOF_adat;
				Execute68kTrap(0xa040, &r);	// ResrvMem()
				r.d[0] = SIZEOF_adat;
				Execute68kTrap(0xa31e, &r);	// NewPtrClear()
				if (r.a[0] == 0)
					return memFullErr;
				audio_data = r.a[0];
				D(bug(" global data at %08lx\n", audio_data));

				// Put in 68k routines
				int p = audio_data + adatDelegateCall;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x7024); p += 2;	// moveq	#$24,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatOpenMixer)
					goto adat_error;
				WriteMacInt16(p, 0x558f); p += 2;	// subq.l	#2,sp
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f00); p += 2;	// move.l	d0,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x203c); p += 2;	// move.l	#$06140018,d0
				WriteMacInt32(p, 0x06140018); p+= 4;
				WriteMacInt16(p, 0xa800); p += 2;	// SoundDispatch
				WriteMacInt16(p, 0x301f); p += 2;	// move.w	(sp)+,d0
				WriteMacInt16(p, 0x48c0); p += 2;	// ext.l	d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatCloseMixer)
					goto adat_error;
				WriteMacInt16(p, 0x558f); p += 2;	// subq.l	#2,sp
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x203c); p += 2;	// move.l	#$02180018,d0
				WriteMacInt32(p, 0x02180018); p+= 4;
				WriteMacInt16(p, 0xa800); p += 2;	// SoundDispatch
				WriteMacInt16(p, 0x301f); p += 2;	// move.w	(sp)+,d0
				WriteMacInt16(p, 0x48c0); p += 2;	// ext.l	d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatGetInfo)
					goto adat_error;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f0a); p += 2;	// move.l	a2,-(sp)
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f00); p += 2;	// move.l	d0,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x2f3c); p += 2;	// move.l	#$000c0103,-(sp)
				WriteMacInt32(p, 0x000c0103); p+= 4;
				WriteMacInt16(p, 0x7000); p += 2;	// moveq	#0,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatSetInfo)
					goto adat_error;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f0a); p += 2;	// move.l	a2,-(sp)
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f00); p += 2;	// move.l	d0,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x2f3c); p += 2;	// move.l	#$000c0104,-(sp)
				WriteMacInt32(p, 0x000c0104); p+= 4;
				WriteMacInt16(p, 0x7000); p += 2;	// moveq	#0,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatPlaySourceBuffer)
					goto adat_error;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f0a); p += 2;	// move.l	a2,-(sp)
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x2f00); p += 2;	// move.l	d0,-(sp)
				WriteMacInt16(p, 0x2f3c); p += 2;	// move.l	#$000c0108,-(sp)
				WriteMacInt32(p, 0x000c0108); p+= 4;
				WriteMacInt16(p, 0x7000); p += 2;	// moveq	#0,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatGetSourceData)
					goto adat_error;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x2f3c); p += 2;	// move.l	#$00040004,-(sp)
				WriteMacInt32(p, 0x00040004); p+= 4;
				WriteMacInt16(p, 0x7000); p += 2;	// moveq	#0,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatStartSource)
					goto adat_error;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x3f00); p += 2;	// move.w	d0,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x2f3c); p += 2;	// move.l	#$00060105,-(sp)
				WriteMacInt32(p, 0x00060105); p+= 4;
				WriteMacInt16(p, 0x7000); p += 2;	// moveq	#0,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatData)
					goto adat_error;

				// Thunk to call a SoundParamBlock moreRtn:
				// pascal Boolean routine(SoundParamBlockPtr *pb)
				// entry: a0 = moreRtn UPP, a1 = &pbVar (adatStreamPbVar)
				p = audio_data + adatCallMoreRtn;
				WriteMacInt16(p, 0x558f); p += 2;	// subq.l	#2,sp (result)
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x4e90); p += 2;	// jsr		(a0)
				WriteMacInt16(p, 0x301f); p += 2;	// move.w	(sp)+,d0
				WriteMacInt16(p, 0x48c0); p += 2;	// ext.l	d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatStreamPbVar)
					goto adat_error;
			}
			AudioAvailable = true;
			if (open_count == 0)
				audio_enter_stream();
			open_count++;
			return noErr;

adat_error:	printf("FATAL: audio component data block initialization error\n");
			QuitEmulator();
			return openErr;

		case kComponentCloseSelect:
			open_count--;
			if (open_count == 0) {
				if (audio_data) {
					if (AudioStatus.mixer) {
						// Close Apple Mixer
						r.a[0] = AudioStatus.mixer;
						Execute68k(audio_data + adatCloseMixer, &r);
						D(bug(" CloseMixer() returns %08lx, mixer %08lx\n", r.d[0], AudioStatus.mixer));
						AudioStatus.mixer = 0;
					}
					r.a[0] = audio_data;
					Execute68kTrap(0xa01f, &r);	// DisposePtr()
					audio_data = 0;
				}
				AudioStatus.num_sources = 0;
				audio_exit_stream();
			}
			return noErr;

		case kComponentCanDoSelect:
			switch ((int16)ReadMacInt16(p)) {
				case kComponentOpenSelect:
				case kComponentCloseSelect:
				case kComponentCanDoSelect:
				case kComponentVersionSelect:
				case kComponentRegisterSelect:
				case kSoundComponentInitOutputDeviceSelect:
				case kSoundComponentGetSourceSelect:
				case kSoundComponentGetInfoSelect:
				case kSoundComponentSetInfoSelect:
				case kSoundComponentStartSourceSelect:
					return 1;
				default:
					return 0;
			}

		case kComponentVersionSelect:
			return 0x00010003;

		case kComponentRegisterSelect:
			return noErr;

		// Sound component functions (not delegated)
		case kSoundComponentInitOutputDeviceSelect: {
			D(bug(" InitOutputDevice\n"));
			if (!audio_open)
				return noHardwareErr;
			if (AudioStatus.mixer)
				return noErr;

			// Init sound component data
			WriteMacInt32(audio_data + adatData + scd_flags, 0);
			WriteMacInt32(audio_data + adatData + scd_format, AudioStatus.sample_size == 16 ? FOURCC('t','w','o','s') : FOURCC('r','a','w',' '));
			WriteMacInt16(audio_data + adatData + scd_numChannels, AudioStatus.channels);
			WriteMacInt16(audio_data + adatData + scd_sampleSize, AudioStatus.sample_size);
			WriteMacInt32(audio_data + adatData + scd_sampleRate, AudioStatus.sample_rate);
			WriteMacInt32(audio_data + adatData + scd_sampleCount, audio_frames_per_block);
			WriteMacInt32(audio_data + adatData + scd_buffer, 0);
			WriteMacInt32(audio_data + adatData + scd_reserved, 0);
			WriteMacInt32(audio_data + adatStreamInfo, 0);

			// Open Apple Mixer
			r.a[0] = audio_data + adatMixer;
			r.d[0] = 0;
			r.a[1] = audio_data + adatData;
			#if QD3D_AUDIO_LOGGING_ENABLED
			diagnostic_call_started = GetTicks_usec();
			#endif
			Execute68k(audio_data + adatOpenMixer, &r);
			AudioStatus.mixer = ReadMacInt32(audio_data + adatMixer);
			const int32 mixer_result = r.d[0];
			QD3D_AUDIO_LOG("InitOutputDevice tick=%u mixer=0x%08x result=%d format=%uHz/%ubit/%uch blockFrames=%d usec=%llu",
			                ReadMacInt32(0x016a),
			                AudioStatus.mixer, mixer_result,
			                AudioStatus.sample_rate >> 16,
			                AudioStatus.sample_size, AudioStatus.channels,
			                audio_frames_per_block,
			                (unsigned long long)(GetTicks_usec() - diagnostic_call_started));
			D(bug(" OpenMixer() returns %08lx, mixer %08lx\n", mixer_result, AudioStatus.mixer));
			return mixer_result;
		}

		case kSoundComponentGetSourceSelect:
			D(bug(" GetSource source %08lx\n", ReadMacInt32(p)));
			WriteMacInt32(ReadMacInt32(p), AudioStatus.mixer);
			return noErr;

		// Sound component functions (delegated)
		case kSoundComponentAddSourceSelect:
			D(bug(" AddSource\n"));
			AudioStatus.num_sources++;
			QD3D_AUDIO_LOG("AddSource sources=%d mixer=0x%08x",
			                AudioStatus.num_sources, AudioStatus.mixer);
			goto delegate_log_add;

		case kSoundComponentRemoveSourceSelect:
			D(bug(" RemoveSource\n"));
			AudioStatus.num_sources--;
			stream_armed = false;
			QD3D_AUDIO_LOG("RemoveSource sources=%d mixer=0x%08x",
			                AudioStatus.num_sources, AudioStatus.mixer);
			goto delegate_log_remove;

		case kSoundComponentGetInfoSelect:
			return AudioGetInfo(ReadMacInt32(p), ReadMacInt32(p + 4), ReadMacInt32(p + 8));

		case kSoundComponentSetInfoSelect:
			return AudioSetInfo(ReadMacInt32(p), ReadMacInt32(p + 4), ReadMacInt32(p + 8));

		case kSoundComponentStartSourceSelect:
			D(bug(" StartSource count %d\n", ReadMacInt16(p + 4)));
			D(bug(" starting Apple Mixer\n"));
			r.d[0] = ReadMacInt16(p + 4);
			r.a[0] = ReadMacInt32(p);
			r.a[1] = AudioStatus.mixer;
			#if QD3D_AUDIO_LOGGING_ENABLED
			diagnostic_call_started = GetTicks_usec();
			#endif
			Execute68k(audio_data + adatStartSource, &r);
			QD3D_AUDIO_LOG("StartSource tick=%u source=0x%08x count=%u result=%d sources=%d usec=%llu",
			                ReadMacInt32(0x016a),
			                ReadMacInt32(p), ReadMacInt16(p + 4),
			                (int32)r.d[0], AudioStatus.num_sources,
			                (unsigned long long)(GetTicks_usec() - diagnostic_call_started));
			D(bug(" returns %08lx\n", r.d[0]));
			return noErr;

		case kSoundComponentStopSourceSelect:
			D(bug(" StopSource\n"));
			QD3D_AUDIO_LOG("StopSource sources=%d mixer=0x%08x",
			                AudioStatus.num_sources, AudioStatus.mixer);
			goto delegate_log_stop;

		case kSoundComponentPauseSourceSelect:
			D(bug(" PauseSource\n"));
			QD3D_AUDIO_LOG("PauseSource sources=%d mixer=0x%08x",
			                AudioStatus.num_sources, AudioStatus.mixer);
			goto delegate_log_pause;

delegate_log_add:
delegate_log_remove:
delegate_log_stop:
delegate_log_pause:
delegate:	// Delegate call to Apple Mixer
			D(bug(" delegating call to Apple Mixer\n"));
			r.a[0] = AudioStatus.mixer;
			r.a[1] = params;
			#if QD3D_AUDIO_LOGGING_ENABLED
			diagnostic_call_started = GetTicks_usec();
			#endif
			Execute68k(audio_data + adatDelegateCall, &r);
			QD3D_AUDIO_LOG("Sound delegate tick=%u selector=%d result=%d usec=%llu sources=%d",
			                ReadMacInt32(0x016a), selector, (int32)r.d[0],
			                (unsigned long long)(GetTicks_usec() - diagnostic_call_started),
			                AudioStatus.num_sources);
			D(bug(" returns %08lx\n", r.d[0]));
			return r.d[0];

		case kSoundComponentPlaySourceBufferSelect:
			D(bug(" PlaySourceBuffer flags %08lx\n", ReadMacInt32(p)));
			{
				uint32 actions = ReadMacInt32(p);
				const uint32 pb = ReadMacInt32(p + 4);
				const uint32 source = ReadMacInt32(p + 8);
				const uint32 frames = pb ? ReadMacInt32(pb + 20) : 0;
				const uint32 actions_in = actions;

#if defined(DESCENT_MOVIE_UNPAUSE_PRIME) && DESCENT_MOVIE_UNPAUSE_PRIME
				/* Off by default: did not fix freeze; can disturb buffer setup. */
				if ((actions & kSourcePaused) && frames > 0 &&
				    AudioStatus.num_sources >= 2) {
					actions &= ~uint32(kSourcePaused);
					WriteMacInt32(p, actions);
				}
#endif

				const int16 initial_result = pb ? ReadMacInt16(pb + 60) : 0;
				uint32 data_before = pb ? ReadMacInt32(pb + 24) : 0;
				uint32 frames_before = frames;

				/* Streaming start we cannot service: an empty PB + moreRtn
				 * relies on the mixer pulling chunks at interrupt rate; our
				 * mixer never services such a source (plays silence) and the
				 * deferred moreRtn refill caps at one chunk per interrupt —
				 * 8x too slow. Fail fast so the client falls back to its
				 * primed-buffer path immediately instead of timing out
				 * (Descent II first movie: ~1 s frozen video + silence).  */
#if QD3D_AUDIO_LOGGING_ENABLED
				diagnostic_source = source;
				diagnostic_source_pb = pb;
				diagnostic_pb_result = initial_result;
				diagnostic_pb_frames = frames_before;
				diagnostic_pb_data = data_before;
				diagnostic_poll_count = 0;
#endif

				/* Empty-PB streaming start (frames==0 + a moreRtn, not paused):
				 * QuickTime hands the Apple Mixer an empty buffer and expects it
				 * to pull chunks via moreRtn at interrupt rate. AudioStreamHostMix
				 * already services exactly this case host-side, so the guest
				 * mixer's PlaySourceBuffer is pure overhead — and its FIRST call
				 * builds a SoundConverter (8bit/22k/mono -> 16bit/44k/stereo) on
				 * the interpreter, a one-time ~3.2 s stall that freezes the movie
				 * (video is timebase-locked to audio).
				 *
				 * Detect the case from the PRE-call PB and skip the guest call:
				 * arm host-mixing, mark the PB "in progress" (result 1) as the
				 * mixer would, return noErr. QuickTime then drives the source via
				 * GetSourceData / the moreRtn, both of which we service. Non-
				 * streaming sources (primed buffers, game sounds) still go through
				 * the guest mixer unchanged. */
				const uint32 more_rtn = pb ? ReadMacInt32(pb + 48) : 0;
				const bool empty_streaming_start =
					pb && !(actions & kSourcePaused) &&
					frames_before == 0 && more_rtn != 0;

				int16 final_pb_result;
				uint32 data_after;
				uint32 frames_after;

				if (empty_streaming_start) {
					#if QD3D_AUDIO_LOGGING_ENABLED
					diagnostic_call_started = GetTicks_usec();
					#endif
					stream_source = source;
					stream_pb = pb;
					stream_armed = true;
					WriteMacInt16(pb + 60, 1);   /* pbResult = in progress */
					r.d[0] = 0;                  /* callResult = noErr */
					final_pb_result = 1;
					data_after = ReadMacInt32(pb + 24);
					frames_after = 0;
					QD3D_AUDIO_LOG("streamHostArm (skipped guest PSB) tick=%u "
					               "source=0x%08x pb=0x%08x moreRtn=0x%08x",
					               ReadMacInt32(0x016a), source, pb, more_rtn);
				} else {
					r.d[0] = actions;
					r.a[0] = pb;
					r.a[1] = source;
					r.a[2] = AudioStatus.mixer;
					#if QD3D_AUDIO_LOGGING_ENABLED
					diagnostic_call_started = GetTicks_usec();
					#endif
					Execute68k(audio_data + adatPlaySourceBuffer, &r);
					final_pb_result = pb ? ReadMacInt16(pb + 60) : 0;
					data_after = pb ? ReadMacInt32(pb + 24) : 0;
					frames_after = pb ? ReadMacInt32(pb + 20) : 0;

					/* Fallback arm for a source that only reveals frames==0 +
					 * moreRtn after the guest call. */
					(void)data_after;
					if (!(actions & kSourcePaused) && pb && frames_after == 0 &&
					    more_rtn != 0) {
						stream_source = source;
						stream_pb = pb;
						stream_armed = true;
						QD3D_AUDIO_LOG("streamHostArm tick=%u source=0x%08x "
						               "pb=0x%08x moreRtn=0x%08x",
						               ReadMacInt32(0x016a), source, pb, more_rtn);
					} else if (stream_armed && source == stream_source) {
						stream_armed = false;
					}
				}
				QD3D_AUDIO_LOG("PlaySourceBuffer tick=%u actions=0x%08x source=0x%08x pb=0x%08x recordBytes=%u format=%c%c%c%c %uHz/%ubit/%uch frames=%u data=0x%08x rateMultiplier=0x%08x moreRtn=0x%08x completionRtn=0x%08x refCon=0x%08x pbResult=%d->%d callResult=%d sources=%d usec=%llu",
				                ReadMacInt32(0x016a), actions, source, pb,
				                pb ? ReadMacInt32(pb) : 0,
				                pb ? (ReadMacInt32(pb + 8) >> 24) : 0,
				                pb ? ((ReadMacInt32(pb + 8) >> 16) & 0xff) : 0,
				                pb ? ((ReadMacInt32(pb + 8) >> 8) & 0xff) : 0,
				                pb ? (ReadMacInt32(pb + 8) & 0xff) : 0,
				                pb ? (ReadMacInt32(pb + 16) >> 16) : 0,
				                pb ? ReadMacInt16(pb + 14) : 0, pb ? ReadMacInt16(pb + 12) : 0,
				                frames_after, data_after,
				                pb ? ReadMacInt32(pb + 32) : 0, pb ? ReadMacInt32(pb + 48) : 0,
				                pb ? ReadMacInt32(pb + 52) : 0, pb ? ReadMacInt32(pb + 56) : 0,
				                initial_result, final_pb_result,
				                (int32)r.d[0], AudioStatus.num_sources,
				                (unsigned long long)(GetTicks_usec() - diagnostic_call_started));
			}
			D(bug(" returns %08lx\n", r.d[0]));
			return r.d[0];

		default:
			if (selector >= 0x100)
				goto delegate;
			else
				return badComponentSelector;
	}
}

// not currently using these functions
/*
 *  Sound input driver Open() routine
 */

int16 SoundInOpen(uint32 pb, uint32 dce)
{
	D(bug("SoundInOpen\n"));
	return noErr;
}


/*
 *  Sound input driver Prime() routine
 */

int16 SoundInPrime(uint32 pb, uint32 dce)
{
	D(bug("SoundInPrime\n"));
	//!!

	uint16 code = ReadMacInt16(pb + csCode);
	D(bug("SoundInControl %d\n", code));

	if (code == 1) {
		D(bug(" SoundInKillIO\n"));
		//!!
		return noErr;
	}

	if (code != 2)
		return -231;	// siUnknownInfoType

	return noErr;
}


/*
 *  Sound input driver Control() routine
 */

int16 SoundInControl(uint32 pb, uint32 dce)
{
	uint16 code = ReadMacInt16(pb + csCode);
	D(bug("SoundInControl %d\n", code));

	if (code == 1) {
		D(bug(" SoundInKillIO\n"));
		//!!
		return noErr;
	}

	if (code != 2)
		return -231;	// siUnknownInfoType

	uint32 selector = ReadMacInt32(pb + csParam); // 4-byte selector (should match via FOURCC above)

	switch (selector) {
		case siInitializeDriver: {
//			If possible, the driver initializes the device to a sampling rate of 22 kHz, a sample size of 8 bits, mono recording, no compression, automatic gain control on, and all other features off.
			return noErr;
		}

		case siCloseDriver: {
//			The sound input device driver should stop any recording in progress, deallocate the input hardware, and initialize local variables to default settings.
			return noErr;
		}

		case siInputSource: {
			SoundInSource = ReadMacInt16(pb + csParam + 4);
			return noErr;
		}

		case siPlayThruOnOff: {
			SoundInPlaythrough = ReadMacInt16(pb + csParam + 4);
			return noErr;
		}

		case siOptionsDialog: {
			return noErr;
		}

		case siInputGain: {
			SoundInGain = ReadMacInt32(pb + csParam + 4);
			return noErr;
		}

		default:
			return -231;	// siUnknownInfoType
	}
}


/*
 *  Sound input driver Status() routine
 */

int16 SoundInStatus(uint32 pb, uint32 dce) // A0 points to Device Manager parameter block (pb) and A1 to device control entry (dce)
{
	uint16 code = ReadMacInt16(pb + csCode);
	D(bug("SoundInStatus %d\n", code));
	if (code != 2)
		return -231;	// siUnknownInfoType

	// two choices on return
	// 1: if under 18 bytes, place # of bytes at (pb+csParam) and write from (pb+csParam+4) on
	// 2: if over 18 bytes, place 0 at (pb+csParam) and directly write into address pointed to by (pb+csParam+4)
	uint32 selector = ReadMacInt32(pb + csParam); // 4-byte selector (should match via FOURCC above)
	uint32 bufferptr = ReadMacInt32(pb + csParam + 4); // 4-byte address to the buffer in vm memory

	switch (selector) {
		case siDeviceName: { // return name in STR255 format
			const uint8 str[] = { // size 9
				0x08,		// 1-byte length
				0x42, 0x75, // Bu
				0x69, 0x6c, // il
				0x74, 0x2d, // t-
				0x69, 0x6e  // in
			};
//			const uint8 str[] = { // size 12
//                0x0b,       // 1-byte length
//                0x53, 0x68, // Sh
//                0x65, 0x65, // ee
//                0x70, 0x73, // ps
//                0x68, 0x61, // ha
//                0x76, 0x65, // ve
//                0x72        // r
//			};
			WriteMacInt32(pb + csParam, 0); // response will be written directly into buffer
			Host2Mac_memcpy(bufferptr, str, sizeof(str));

			return noErr;
		}

		case siDeviceIcon: {
			// todo: add soundin ICN, borrow from CD ROM for now
			WriteMacInt32(pb + csParam, 0);

			M68kRegisters r;
			r.d[0] = sizeof(CDROMIcon);
			Execute68kTrap(0xa122, &r);	// NewHandle()
			uint32 h = r.a[0];
			if (h == 0)
				return memFullErr;
			WriteMacInt32(bufferptr, h);
			uint32 sp = ReadMacInt32(h);
			Host2Mac_memcpy(sp, CDROMIcon, sizeof(CDROMIcon));

			return noErr;

			// 68k code causes crash in sheep and link error in basilisk
//			M68kRegisters r;
//			static const uint8 proc[] = {
//				0x55, 0x8f,							// 	subq.l	#2,sp
//				0xa9, 0x94,							// 	CurResFile
//				0x42, 0x67,							// 	clr.w	-(sp)
//				0xa9, 0x98,							// 	UseResFile
//				0x59, 0x8f,							// 	subq.l	#4,sp
//				0x48, 0x79, 0x49, 0x43, 0x4e, 0x23,	// 	move.l	#'ICN#',-(sp)
//				0x3f, 0x3c, 0xbf, 0x76,				// 	move.w	#-16522,-(sp)
//				0xa9, 0xa0,							// 	GetResource
//				0x24, 0x5f,							// 	move.l	(sp)+,a2
//				0xa9, 0x98,							// 	UseResFile
//				0x20, 0x0a,							// 	move.l	a2,d0
//				0x66, 0x04,							// 	bne		1
//				0x70, 0x00,							//  moveq	#0,d0
//				M68K_RTS >> 8, M68K_RTS & 0xff,
//				0x2f, 0x0a,							//1 move.l	a2,-(sp)
//				0xa9, 0x92,							//  DetachResource
//				0x20, 0x4a,							//  move.l	a2,a0
//				0xa0, 0x4a,							//	HNoPurge
//				0x70, 0x01,							//	moveq	#1,d0
//				M68K_RTS >> 8, M68K_RTS & 0xff
//			};
//			Execute68k(Host2MacAddr((uint8 *)proc), &r);
//			if (r.d[0]) {
//				WriteMacInt32(pb + csParam, 4); // Length of returned data
//				WriteMacInt32(pb + csParam + 4, r.a[2]); // Handle to icon suite
//				return noErr;
//			} else
//				return -192;		// resNotFound
		}

		case siInputSource: {
			// return -231 if only 1 or index of current source if more

			WriteMacInt32(pb + csParam, 2);
			WriteMacInt16(pb + csParam + 4, SoundInSource); // index of selected source
			return noErr;
		}

		case siInputSourceNames: {
			// return -231 if only 1 or handle to STR# resource if more

			const uint8 str[] = {
				0x00, 0x02, // 2-byte count of #strings
				// byte size indicator (up to 255 length supported)
				0x0a,       // size is 10
				0x4d, 0x69,	// Mi
				0x63, 0x72,	// cr
				0x6f, 0x70,	// op
				0x68, 0x6f,	// ho
				0x6e, 0x65,	// ne
				0x0b,		// size is 11
				0x49, 0x6e, // start of string in ASCII, In
				0x74, 0x65, // te
				0x72, 0x6e, // rn
				0x61, 0x6c, // al
				0x20, 0x43, //  C
				0x44,  		// D
			};

			WriteMacInt32(pb + csParam, 0);

			M68kRegisters r;
			r.d[0] = sizeof(str);
			Execute68kTrap(0xa122, &r);	// NewHandle()
			uint32 h = r.a[0];
			if (h == 0)
				return memFullErr;
			WriteMacInt32(bufferptr, h);
			uint32 sp = ReadMacInt32(h);
			Host2Mac_memcpy(sp, str, sizeof(str));

			return noErr;
		}

		case siOptionsDialog: {
			// 0 if no options box supported and 1 if so
			WriteMacInt32(pb + csParam, 2); // response not in buffer, need to copy integer
			WriteMacInt16(pb + csParam + 4, 1); // Integer data type
			return noErr;
		}

		case siPlayThruOnOff: {
			// playthrough volume, 0 is off and 7 is max
			WriteMacInt32(pb + csParam, 2);
			WriteMacInt16(pb + csParam + 4, SoundInPlaythrough);
			return noErr;
		}

		case siNumberChannels: {
			// 1 is mono and 2 is stereo
			WriteMacInt32(pb + csParam, 2);
			WriteMacInt16(pb + csParam + 4, 2);
			return noErr;
		}

		case siSampleRate: {
			WriteMacInt32(pb + csParam, 0);
			WriteMacInt32(bufferptr, 0xac440000); // 44100.00000 Hz, of Fixed data type
			return noErr;
		}

		case siSampleRateAvailable: {
			WriteMacInt32(pb + csParam, 0);

            M68kRegisters r;
            r.d[0] = 4;
            Execute68kTrap(0xa122, &r);    // NewHandle()
            uint32 h = r.a[0];
            if (h == 0)
                return memFullErr;
            WriteMacInt16(bufferptr, 1); // 1 sample rate available
            WriteMacInt32(bufferptr + 2, h); // handle to sample rate list
            uint32 sp = ReadMacInt32(h);
            WriteMacInt32(sp, 0xac440000); // 44100.00000 Hz, of Fixed data type

			return noErr;
		}

		case siInputGain: {
			WriteMacInt32(pb + csParam, 4);
			WriteMacInt32(pb + csParam + 4, SoundInGain);
			return noErr;
		}


		default:
			return -231;	// siUnknownInfoType
	}
}


/*
 *  Sound input driver Close() routine
 */

int16 SoundInClose(uint32 pb, uint32 dce)
{
	D(bug("SoundInClose\n"));
	return noErr;
}

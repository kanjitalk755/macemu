/*
 * qd3d_init_logging.h - Focused QuickDraw 3D/RAVE diagnostics
 *
 * Every channel is an independent compile-time switch. When a channel is
 * disabled, its macro and all channel-specific instrumentation compile away.
 */

#ifndef QD3D_INIT_LOGGING_H
#define QD3D_INIT_LOGGING_H

#ifndef QD3D_INIT_LOGGING_ENABLED
#define QD3D_INIT_LOGGING_ENABLED 1
#endif

#ifndef QD3D_GRAPHICS_LOGGING_ENABLED
#define QD3D_GRAPHICS_LOGGING_ENABLED 1
#endif

#ifndef QD3D_AUDIO_LOGGING_ENABLED
#define QD3D_AUDIO_LOGGING_ENABLED 1
#endif

#ifndef QD3D_MEDIA_LOGGING_ENABLED
#define QD3D_MEDIA_LOGGING_ENABLED 1
#endif

#ifndef QD3D_WAIT_LOGGING_ENABLED
#define QD3D_WAIT_LOGGING_ENABLED 1
#endif

/* Per-VIA-tick emu-thread wall-time accounting. PC sampling can't find diffuse
 * cost (the movie handoff is slow everywhere, not stuck at one PC). This brackets
 * each emu-thread consumer (EMUL_OP handlers incl. nested Execute68k, native
 * RAVE/GL/DSp dispatch, native Cinepak decode) and dumps the breakdown once per
 * guest tick, so a fat tick shows which bucket ate it and 'unacc' (= tick wall
 * minus accounted) exposes raw-interpreter grind. Enable for a targeted capture. */
#ifndef GFX_TICKPROF_ENABLED
#define GFX_TICKPROF_ENABLED 0
#endif

/* Descent II mid-movie SetDepth(16) video-hitch instrumentation (present
 * heartbeat, framebuffer-region hash, switch log, Cinepak blit probe, upload
 * gate probe). DEFAULT OFF - targeted debugging only. */
#ifndef DESCENT_HITCH_DEBUG
#define DESCENT_HITCH_DEBUG 0
#endif

/* QuickTime sound-clock GetTime fast path. DISPROVEN as a freeze fix and
 * default OFF: the movie's sound-clock component polls a 68k Microseconds glue
 * ~20k/s, but that polling is a SYMPTOM of a sound-clock sync WAIT, not the
 * cause. Three variants (minimal handler, uncoalesced/fine-grained value,
 * glue repatch) all left the freeze at ~51 ticks - the wait is for the primed
 * audio buffer to play out to the video's media time, a wall-clock duration
 * independent of poll cost (descent-movie session notes ?33). The glue-repatch
 * machinery is retained but gated off; do not enable expecting a freeze fix. */
/* Sound-clock experiments (glue fast path + offset) are OFF: a direct 0.9s
 * offset on the movie-phase sound clock did NOT move the freeze (?36), proving
 * the intro video hold is gated on the QuickTime movie TimeBase, not the sound
 * clock value. Do not re-enable expecting a fix. */
#ifndef DESCENT_MOVIE_QT_CLOCK_FASTPATH
#define DESCENT_MOVIE_QT_CLOCK_FASTPATH 0
#endif
#ifndef DESCENT_MOVIE_SCLK_OFFSET_US
#define DESCENT_MOVIE_SCLK_OFFSET_US 0
#endif

/* Movie-phase audio prefetch depth, in host callback blocks. Kept at 2 (the
 * normal depth): pulling GetSourceData ahead was tested (12 blocks) and did NOT
 * move the intro freeze - QuickTime's video hold is gated on the movie TimeBase
 * vs audio media time, not on how far ahead we drain the source (?36). Leaving
 * the knob so the depth is explicit and easily retunable. */
#ifndef DESCENT_MOVIE_PREFETCH_DEPTH_BLOCKS
#define DESCENT_MOVIE_PREFETCH_DEPTH_BLOCKS 2
#endif


/* Experimental: rewrite outer-wait deadline UWs. PROVEN HARMFUL (d2.log
 * 2026-07-16): t1c/t24 are sentinels 0x8000&/0x7fff&, not wall deadlines;
 * zeroing them kills movie audio after the stutter. Keep at 0. */
#ifndef DESCENT_MOVIE_CLOCK_FIX
#define DESCENT_MOVIE_CLOCK_FIX 0
#endif

/* Experimental: strip kSourcePaused on movie PlaySourceBuffer primes.
 * Did not remove the freeze; left enabled=0. */
#ifndef DESCENT_MOVIE_UNPAUSE_PRIME
#define DESCENT_MOVIE_UNPAUSE_PRIME 0
#endif

/* Experimental: rewrite the movie sample-site A82A (ComponentDispatch
 * GetTime, 0x00040001 header) into a host EMUL_OP GetTime. PROVEN HARMFUL
 * (2026-07-17): the near-free GetTime lets the movie policy loop free-run
 * (~700k calls) and the mixer loses PlaySourceBuffer data (SourcePB data=0)
 * -> no audio. Keep at 0 unless the parent loop can be rate-limited safely
 * (Windows Delay_usec < 1 ms is Sleep(0), i.e. no delay at all). */
#ifndef DESCENT_MOVIE_INLINE_A82A_GETTIME
#define DESCENT_MOVIE_INLINE_A82A_GETTIME 0
#endif

/* Targeted fix: make the siSoundClock ComponentInstance return continuous
 * device wall time so Descent II's phase-2 music source does not wait for its
 * paused clock to catch up. The inline A82A patch is re-enabled but only
 * intercepts the tracked sound-clock CI; all other CIs fall back to the
 * original ComponentDispatch trap.
 *
 * STATUS 2026-07-17: default 0. The inline patch itself works (descent-movie
 * session notes 26), but it rewrites BOTH the sclk site and the ci40
 * (video-sync) site, so a fallback that runs the real A82A is mandatory.
 * Every known way to run that trap from inside an EMUL_OP handler wedges the
 * guest at the first non-sclk dispatch: Execute68kTrap gives SIGSEGV (25.1),
 * and a ROM-scratch "A82A; jmp abs.l" thunk hangs the emulator on the same
 * tick it first fires. EmulOp runs in MODE_EMUL_OP and the trap needs real
 * trap-entry state. Do not re-enable until the sclk site can be identified on
 * its own (so no fallback is needed) or the trap can run from 68k context. */
#ifndef DESCENT_MOVIE_SOUND_CLOCK_WALL
#define DESCENT_MOVIE_SOUND_CLOCK_WALL 0
#endif

#ifndef DESCENT_MOVIE_MICROSECONDS_HACK
#define DESCENT_MOVIE_MICROSECONDS_HACK 0
#endif

#if QD3D_INIT_LOGGING_ENABLED || QD3D_GRAPHICS_LOGGING_ENABLED || \
    QD3D_AUDIO_LOGGING_ENABLED || QD3D_MEDIA_LOGGING_ENABLED || \
    QD3D_WAIT_LOGGING_ENABLED

#include <cstdarg>
#include <cstdio>

/* Shared stderr + OutputDebugStringA sink (always compiled in). */
#include "gfx_debug_sink.h"

namespace qd3d_init_logging {

static inline void log(const char *category, const char *file, int line,
                       const char *format, ...)
{
	char message[2048];
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message) - 1] = '\0';

	/* Prefix carries the category + source location; body is the message.
	 * The shared sink appends the trailing newline and handles both outputs. */
	char prefix[512];
	std::snprintf(prefix, sizeof(prefix), "[QD3D:%s] %s:%d: ",
	              category, file, line);
	prefix[sizeof(prefix) - 1] = '\0';
	::gfx_debug::emit(prefix, "%s", message);
}

} // namespace qd3d_init_logging

#if QD3D_AUDIO_LOGGING_ENABLED
#define QD3D_AUDIO_LOG(...) \
	::qd3d_init_logging::log("audio", __FILE__, __LINE__, __VA_ARGS__)
#else
#define QD3D_AUDIO_LOG(...) do { } while (0)
#endif

#if QD3D_MEDIA_LOGGING_ENABLED
#define QD3D_MEDIA_LOG(...) \
	::qd3d_init_logging::log("media", __FILE__, __LINE__, __VA_ARGS__)
#else
#define QD3D_MEDIA_LOG(...) do { } while (0)
#endif

#if QD3D_WAIT_LOGGING_ENABLED
#define QD3D_WAIT_LOG(...) \
	::qd3d_init_logging::log("wait", __FILE__, __LINE__, __VA_ARGS__)
#else
#define QD3D_WAIT_LOG(...) do { } while (0)
#endif

#if QD3D_INIT_LOGGING_ENABLED

#define QD3D_INIT_LOG(...) \
	::qd3d_init_logging::log("init", __FILE__, __LINE__, __VA_ARGS__)

#else

#define QD3D_INIT_LOG(...) do { } while (0)

#endif

#if QD3D_GRAPHICS_LOGGING_ENABLED

#define QD3D_STATE_LOG(...) \
	::qd3d_init_logging::log("state", __FILE__, __LINE__, __VA_ARGS__)
#define QD3D_RESOURCE_LOG(...) \
	::qd3d_init_logging::log("resource", __FILE__, __LINE__, __VA_ARGS__)
#define QD3D_RENDER_LOG(...) \
	::qd3d_init_logging::log("render", __FILE__, __LINE__, __VA_ARGS__)

#else

#define QD3D_STATE_LOG(...) do { } while (0)
#define QD3D_RESOURCE_LOG(...) do { } while (0)
#define QD3D_RENDER_LOG(...) do { } while (0)

#endif

#else

#define QD3D_INIT_LOG(...) do { } while (0)
#define QD3D_STATE_LOG(...) do { } while (0)
#define QD3D_RESOURCE_LOG(...) do { } while (0)
#define QD3D_RENDER_LOG(...) do { } while (0)
#define QD3D_AUDIO_LOG(...) do { } while (0)
#define QD3D_MEDIA_LOG(...) do { } while (0)
#define QD3D_WAIT_LOG(...) do { } while (0)

#endif

#endif /* QD3D_INIT_LOGGING_H */

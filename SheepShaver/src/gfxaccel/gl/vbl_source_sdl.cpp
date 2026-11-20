/*
 *  vbl_source_sdl.cpp - SDL/timer-based VBL source (OpenGL backend)
 *
 *  Replaces CAMetalDisplayLink / CADisplayLink with a paced tick counter
 *  driven from MetalCompositorPresent. Timing comes from the port's own
 *  GetTicks_usec/Delay_usec (sysdeps.h), not from SDL.
 *
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
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
#include "vbl_source.h"
#include "gfx_log.h"

#include "atomic.h"
#include <cstring>

static VBLSourceCallbackFn s_primary_cb = NULL;
static void *s_primary_ctx = NULL;
static VBLSourceCallbackFn s_secondary[VBL_SECONDARY_CALLBACK_MAX] = {};
static void *s_secondary_ctx[VBL_SECONDARY_CALLBACK_MAX] = {};
static int s_secondary_count = 0;

static atomic_uint64 s_tick_count = 0;
static atomic_uint64 s_cadence_usec = 16667;
static atomic_uint64 s_last_tick_usec = 0;
static atomic_sint s_paused = 0;
static atomic_sint s_in_callback = 0;
static bool s_initialized = false;

/* Per-engine next deadlines on the steady-clock microsecond timeline. */
static uint64_t s_engine_deadline_usec[kGfxFramePacingEngineCount] = {};

/* GetTicks_usec and Delay_usec are the tree's own per-platform monotonic
 * clock and sleep (declared in each port's sysdeps.h). */
static uint64_t now_usec(void)
{
	return (uint64_t)GetTicks_usec();
}

/* Sleep to an absolute deadline on that same timeline. */
static void sleep_until_usec(uint64_t deadline)
{
	uint64_t now = now_usec();
	while (deadline > now + 1000) {
		Delay_usec(deadline - now);
		now = now_usec();
	}
}

int32_t vbl_source_init(void * /*cametal_layer*/,
						VBLSourceCallbackFn callback,
						void *ctx)
{
	if (s_initialized)
		return kGfxAccelErrVBLAlreadyRunning;
	s_primary_cb = callback;
	s_primary_ctx = ctx;
	atomic_store_explicit(&s_tick_count, 0, memory_order_seq_cst);
	atomic_store_explicit(&s_cadence_usec, 16667, memory_order_seq_cst);
	atomic_store_explicit(&s_last_tick_usec, 0, memory_order_seq_cst);
	std::memset(s_engine_deadline_usec, 0,
				sizeof(s_engine_deadline_usec));
	atomic_store_explicit(&s_paused, 0, memory_order_seq_cst);
	s_initialized = true;
	return 0; /* kGfxAccelNoErr */
}

void vbl_source_shutdown(void)
{
	s_initialized = false;
	s_primary_cb = NULL;
	s_primary_ctx = NULL;
	s_secondary_count = 0;
	std::memset(s_secondary, 0, sizeof(s_secondary));
	std::memset(s_secondary_ctx, 0, sizeof(s_secondary_ctx));
	atomic_store_explicit(&s_tick_count, 0, memory_order_seq_cst);
	atomic_store_explicit(&s_last_tick_usec, 0, memory_order_seq_cst);
	std::memset(s_engine_deadline_usec, 0,
				sizeof(s_engine_deadline_usec));
}

uint64_t vbl_source_get_cadence_usec(void)
{
	return atomic_load_explicit(&s_cadence_usec, memory_order_seq_cst);
}

uint64_t vbl_source_get_tick_count(void)
{
	return atomic_load_explicit(&s_tick_count, memory_order_seq_cst);
}

int vbl_source_uses_metal_display_link(void)
{
	return 0;
}

void vbl_source_set_paused(int paused)
{
	atomic_store_explicit(&s_paused, paused ? 1 : 0, memory_order_seq_cst);
}

int vbl_source_in_callback_chain(void)
{
	return atomic_load_explicit(&s_in_callback, memory_order_seq_cst);
}

void vbl_source_signal_3d_pacing(void)
{
	/* A VBL signal anchors the live cadence grid. It must not also advance
	 * every engine's private deadline: doing both makes the next sync wait
	 * for the following boundary and creates a structural half-rate cap. */
	atomic_store_explicit(&s_last_tick_usec, now_usec(), memory_order_seq_cst);
}

int32_t vbl_source_sync_3d_pacing_for_engine(int32_t engine_id)
{
	if (!s_initialized)
		return kGfxAccelErrVBLNotInitialized;
	if (engine_id < 0 || engine_id >= kGfxFramePacingEngineCount)
		engine_id = 0;

	const uint64_t now = now_usec();
	uint64_t deadline = s_engine_deadline_usec[engine_id];
	const uint64_t cad = GfxFramePacingClampCadenceUsec(
		atomic_load_explicit(&s_cadence_usec, memory_order_seq_cst));
	const uint64_t last_tick = atomic_load_explicit(&s_last_tick_usec, memory_order_seq_cst);
	const bool tick_is_fresh = last_tick != 0 &&
		now <= last_tick + cad * GFX_FRAME_PACING_STALE_TICKS;
	if (!tick_is_fresh) {
		const uint64_t fallback_deadline = now + cad;
		if (deadline == 0 || deadline <= now ||
			deadline > fallback_deadline + cad)
			deadline = fallback_deadline;
	} else if (deadline == 0 || deadline > last_tick + 2u * cad) {
		/* First sync, cadence change, or long idle: anchor to the first
		 * boundary following the most recent live tick. */
		deadline = last_tick + cad;
	}

	if (tick_is_fresh && deadline <= now) {
		/* Rendering (or another co-resident engine) already crossed this
		 * boundary. Consume it without sleeping, then target the next one. */
		do {
			deadline += cad;
		} while (deadline <= now);
		s_engine_deadline_usec[engine_id] = deadline;
		return 0;
	}

	while (deadline <= now)
		deadline += cad;
	sleep_until_usec(deadline);
	s_engine_deadline_usec[engine_id] = deadline + cad;
	return 0;
}

int32_t vbl_source_sync_3d_pacing(void)
{
	return vbl_source_sync_3d_pacing_for_engine(0);
}

int32_t vbl_source_register_secondary_callback(VBLSourceCallbackFn cb, void *ctx)
{
	if (s_secondary_count >= VBL_SECONDARY_CALLBACK_MAX)
		return kGfxAccelErrVBLAlreadyRunning;
	s_secondary[s_secondary_count] = cb;
	s_secondary_ctx[s_secondary_count] = ctx;
	s_secondary_count++;
	return 0;
}

void vbl_source_unregister_secondary_callback(VBLSourceCallbackFn cb)
{
	for (int i = 0; i < s_secondary_count; i++) {
		if (s_secondary[i] == cb) {
			for (int j = i; j < s_secondary_count - 1; j++) {
				s_secondary[j] = s_secondary[j + 1];
				s_secondary_ctx[j] = s_secondary_ctx[j + 1];
			}
			s_secondary_count--;
			return;
		}
	}
}

/*
 * Drive one VBL tick from the compositor present path.
 * Emulates display-link delivery without Apple frameworks.
 */
extern "C" void vbl_source_sdl_tick(double target_ts)
{
	if (!s_initialized || atomic_load_explicit(&s_paused, memory_order_seq_cst))
		return;
	/* Match FireVBLCallbackChain on the Metal backend. Guest callbacks can
	 * pump the event loop and encounter another VBL on this same thread; the
	 * nested chain must not run the DSp drains or primary callback twice. */
	if (atomic_exchange_explicit(&s_in_callback, 1, memory_order_seq_cst) != 0) {
#if QD3D_GRAPHICS_LOGGING_ENABLED
		static uint64_t s_nested_tick_count = 0;
		++s_nested_tick_count;
		if (s_nested_tick_count <= 8 ||
			(s_nested_tick_count & (s_nested_tick_count - 1u)) == 0 ||
			(s_nested_tick_count % 120u) == 0) {
			QD3D_RENDER_LOG("VBL tick deferred: nested callback chain count=%llu",
							(unsigned long long)s_nested_tick_count);
		}
#endif
		return;
	}

	atomic_fetch_add_explicit(&s_tick_count, 1, memory_order_seq_cst);
	vbl_source_signal_3d_pacing();

	if (s_primary_cb)
		s_primary_cb(s_primary_ctx, NULL, target_ts);

	for (int i = 0; i < s_secondary_count; i++) {
		if (s_secondary[i])
			s_secondary[i](s_secondary_ctx[i], NULL, target_ts);
	}
	atomic_store_explicit(&s_in_callback, 0, memory_order_seq_cst);
}

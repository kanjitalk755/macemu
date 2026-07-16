/*
 *  vbl_source_sdl.cpp - SDL/timer-based VBL source (OpenGL backend)
 *
 *  Replaces CAMetalDisplayLink / CADisplayLink with a paced tick counter
 *  driven from MetalCompositorPresent (and optionally an SDL timer).
 */

#include "sysdeps.h"
#include "vbl_source.h"
#include "gl_device.h"

#include <SDL.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>

static VBLSourceCallbackFn s_primary_cb = nullptr;
static void *s_primary_ctx = nullptr;
static VBLSourceCallbackFn s_secondary[VBL_SECONDARY_CALLBACK_MAX] = {};
static void *s_secondary_ctx[VBL_SECONDARY_CALLBACK_MAX] = {};
static int s_secondary_count = 0;

static std::atomic<uint64_t> s_tick_count{0};
static std::atomic<uint64_t> s_cadence_usec{16667};
static std::atomic<int> s_paused{0};
static std::atomic<int> s_in_callback{0};
static bool s_initialized = false;

/* Per-engine next deadlines (microseconds, SDL_GetTicks64 * 1000). */
static uint64_t s_engine_deadline_usec[kGfxFramePacingEngineCount] = {};

static uint64_t now_usec(void)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
	return (uint64_t)SDL_GetTicks64() * 1000ull;
#else
	return (uint64_t)SDL_GetTicks() * 1000ull;
#endif
}

int32_t vbl_source_init(void * /*cametal_layer*/,
                        VBLSourceCallbackFn callback,
                        void *ctx)
{
	if (s_initialized)
		return kGfxAccelErrVBLAlreadyRunning;
	s_primary_cb = callback;
	s_primary_ctx = ctx;
	s_tick_count.store(0);
	s_cadence_usec.store(16667);
	s_paused.store(0);
	s_initialized = true;
	return 0; /* kGfxAccelNoErr */
}

void vbl_source_shutdown(void)
{
	s_initialized = false;
	s_primary_cb = nullptr;
	s_primary_ctx = nullptr;
	s_secondary_count = 0;
	std::memset(s_secondary, 0, sizeof(s_secondary));
	std::memset(s_secondary_ctx, 0, sizeof(s_secondary_ctx));
	s_tick_count.store(0);
}

uint64_t vbl_source_get_cadence_usec(void)
{
	return s_cadence_usec.load();
}

uint64_t vbl_source_get_tick_count(void)
{
	return s_tick_count.load();
}

int vbl_source_uses_metal_display_link(void)
{
	return 0;
}

void vbl_source_set_paused(int paused)
{
	s_paused.store(paused ? 1 : 0);
}

int vbl_source_in_callback_chain(void)
{
	return s_in_callback.load();
}

void vbl_source_signal_3d_pacing(void)
{
	const uint64_t now = now_usec();
	const uint64_t cad = s_cadence_usec.load();
	for (int i = 0; i < kGfxFramePacingEngineCount; i++) {
		if (s_engine_deadline_usec[i] < now)
			s_engine_deadline_usec[i] = now + cad;
		else
			s_engine_deadline_usec[i] += cad;
	}
}

int32_t vbl_source_sync_3d_pacing_for_engine(int32_t engine_id)
{
	if (!s_initialized)
		return kGfxAccelErrVBLNotInitialized;
	if (engine_id < 0 || engine_id >= kGfxFramePacingEngineCount)
		engine_id = 0;

	const uint64_t now = now_usec();
	uint64_t deadline = s_engine_deadline_usec[engine_id];
	const uint64_t cad = s_cadence_usec.load();
	if (deadline == 0 || deadline + cad * 4 < now) {
		/* ticks stopped or first frame — wait one cadence from now */
		deadline = now + cad;
		s_engine_deadline_usec[engine_id] = deadline;
	}

	if (deadline > now) {
		const uint64_t sleep_us = deadline - now;
		std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
	}
	s_engine_deadline_usec[engine_id] = now_usec() + cad;
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
	if (!s_initialized || s_paused.load())
		return;

	s_in_callback.store(1);
	s_tick_count.fetch_add(1);
	vbl_source_signal_3d_pacing();

	if (s_primary_cb)
		s_primary_cb(s_primary_ctx, nullptr, target_ts);

	for (int i = 0; i < s_secondary_count; i++) {
		if (s_secondary[i])
			s_secondary[i](s_secondary_ctx[i], nullptr, target_ts);
	}
	s_in_callback.store(0);
}

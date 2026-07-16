/*
 *  dsp_draw_context_gl.cpp - DSp context lifecycle (OpenGL/host backend)
 *
 *  Implements the public handlers declared in dsp_draw_context.h using host
 *  memory back-buffers. Full Metal parity (dirty rects, alt buffers, fades)
 *  is staged; Reserve/GetBackBuffer/SwapBuffers work for basic titles.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "dsp_engine.h"
#include "dsp_draw_context.h"
#include "dsp_metal_renderer.h"
#include "dsp_context_private.h"
#include "dsp_host_bridge.h"
#include "metal_compositor.h"
#include "display_mode_controller.h"
#include "gfxaccel_resources.h"
#include "gfxaccel_resources_heap.h"
#include "dsp_cgraf_port_policy.h"
#include "dsp_pixmap_offsets.h"
#include "dsp_main_device_redirect_policy.h"
#include "dsp_display_mode_policy.h"
#include "dsp_back_buffer_range.h"
#include "dsp_default_clut.h"
#include "dsp_user_select_policy.h"
#include "dsp_get_attributes_policy.h"
#include "dsp_mode_enumerate.h"
#include "dsp_host_bridge.h"
#include "dsp_engine_internal.h"
#include "macos_util.h"
#include "video.h"
#include "nqd_accel.h"
#include "qd3d_init_logging.h"
#include "vbl_source.h"

#include <map>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <thread>
#include <vector>

/* Forward decls for heap free used by alt-buffer teardown */
extern "C" void gfxaccel_resources_heap_mm_free_buffer(uint32_t heap_id, void *ptr);
static void restore_underlay_if_any(DSpContextPrivate *ctx);
extern "C" void DSpRedirectMainDevicePixMap(DSpContextPrivate *ctx);
extern "C" void DSpRestoreMainDevicePixMap(DSpContextPrivate *ctx);
extern "C" void DSpHostBridge_SetActiveFullscreen(bool active);

static std::map<uint32_t, DSpContextPrivate *> s_ctx;
static uint32_t s_next_handle = 1;

DSpContextPrivate *DSpGetContext(uint32_t handle)
{
	auto it = s_ctx.find(handle);
	return it == s_ctx.end() ? nullptr : it->second;
}

uint32_t DSpAllocFirstContextHandle(const DSpContextAttributes *attr,
                                    uint32_t enumeration_mode_index)
{
	auto *ctx = new DSpContextPrivate();
	std::memset(ctx, 0, sizeof(*ctx));
	ctx->handle = s_next_handle++;
	ctx->enumeration_mode_index = enumeration_mode_index;
	if (attr) ctx->attr = *attr;
	DSpInitDefaultCLUT(ctx->clut_bytes, ctx->clut_bytes_latched,
	                   ctx->attr.backBufferBestDepth);
	/* DMC gamma is planar: 256 R, then 256 G, then 256 B. */
	for (int i = 0; i < 256; i++) {
		ctx->gamma_lut_persisted[i] = (uint8_t)i;
		ctx->gamma_lut_persisted[256 + i] = (uint8_t)i;
		ctx->gamma_lut_persisted[512 + i] = (uint8_t)i;
	}
	s_ctx[ctx->handle] = ctx;
	return ctx->handle;
}
uint32_t DSpAllocMetadataContextHandle(const DSpContextAttributes *attr,
                                       uint32_t enumeration_mode_index)
{
	return DSpAllocFirstContextHandle(attr, enumeration_mode_index);
}
uint32_t DSpGetContextEnumerationIndex(uint32_t ctxRef)
{
	DSpContextPrivate *c = DSpGetContext(ctxRef);
	return c ? c->enumeration_mode_index : DSP_ENUMERATION_INDEX_NONE;
}

static void dsp_apply_reserve_color_table(DSpContextPrivate *ctx,
	                                      uint32_t color_table_handle,
	                                      uint32_t depth)
{
	if (!ctx || !color_table_handle || depth > 8) return;
	if (!NQDMetalAddrInBuffer(color_table_handle) ||
	    !NQDMetalAddrInBuffer(color_table_handle + 3u)) return;
	const uint32_t table = ReadMacInt32(color_table_handle);
	if (!table || !NQDMetalAddrInBuffer(table) ||
	    !NQDMetalAddrInBuffer(table + 7u)) return;
	const int16_t last_index = (int16_t)ReadMacInt16(table + 6u);
	if (last_index < 0) return;
	uint32_t count = (uint32_t)last_index + 1u;
	if (count > 256) count = 256;
	const uint32_t bytes = 8u + count * 8u;
	if (table > UINT32_MAX - bytes ||
	    !NQDMetalAddrInBuffer(table + bytes - 1u)) return;

	uint32_t applied = 0;
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t entry = table + 8u + i * 8u;
		const int16_t value = (int16_t)ReadMacInt16(entry);
		if (value < 0 || value > 255) continue;
		const uint32_t dst = (uint32_t)value * 3u;
		ctx->clut_bytes[dst + 0] = (uint8_t)(ReadMacInt16(entry + 2u) >> 8);
		ctx->clut_bytes[dst + 1] = (uint8_t)(ReadMacInt16(entry + 4u) >> 8);
		ctx->clut_bytes[dst + 2] = (uint8_t)(ReadMacInt16(entry + 6u) >> 8);
		applied++;
	}
	std::memcpy(ctx->clut_bytes_latched, ctx->clut_bytes,
	            sizeof(ctx->clut_bytes_latched));
	QD3D_RESOURCE_LOG("DSpReserve(GL): applied CTab handle=0x%08x table=0x%08x count=%u applied=%u depth=%u",
	                  color_table_handle, table, count, applied, depth);
}

int32_t DSpContext_ReserveHandler(uint32_t ctxRef, uint32_t desiredAttrAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpInvalidContextErr;
	if (!desiredAttrAddr ||
	    !NQDMetalAddrInBuffer(desiredAttrAddr) ||
	    !NQDMetalAddrInBuffer(desiredAttrAddr + 51u)) {
		QD3D_STATE_LOG("DSpReserve(GL): invalid attributes ctx=%u addr=0x%08x",
		               ctxRef, desiredAttrAddr);
		return kDSpInvalidAttributesErr;
	}
	if (ctx->back_buffer) return kDSpContextAlreadyReservedErr;

	DSpContextAttributes desired = {};
	desired.frequency = ReadMacInt32(desiredAttrAddr + 0);
	desired.displayWidth = ReadMacInt32(desiredAttrAddr + 4);
	desired.displayHeight = ReadMacInt32(desiredAttrAddr + 8);
	desired.reserved1 = ReadMacInt32(desiredAttrAddr + 12);
	desired.reserved2 = ReadMacInt32(desiredAttrAddr + 16);
	desired.colorNeeds = ReadMacInt32(desiredAttrAddr + 20);
	desired.colorTable = ReadMacInt32(desiredAttrAddr + 24);
	desired.contextOptions = ReadMacInt32(desiredAttrAddr + 28);
	desired.backBufferDepthMask = ReadMacInt32(desiredAttrAddr + 32);
	desired.displayDepthMask = ReadMacInt32(desiredAttrAddr + 36);
	desired.backBufferBestDepth = ReadMacInt32(desiredAttrAddr + 40);
	desired.displayBestDepth = ReadMacInt32(desiredAttrAddr + 44);
	desired.pageCount = ReadMacInt32(desiredAttrAddr + 48);

	const uint32_t back_depth = desired.backBufferBestDepth;
	if (!desired.displayWidth || !desired.displayHeight || !desired.pageCount ||
	    desired.displayWidth > 4096 || desired.displayHeight > 4096 ||
	    (back_depth != 1 && back_depth != 2 && back_depth != 4 &&
	     back_depth != 8 && back_depth != 16 && back_depth != 32) ||
	    !desired.backBufferDepthMask) {
		QD3D_STATE_LOG("DSpReserve(GL): rejected ctx=%u requested=%ux%u backDepth=%u displayDepth=%u backMask=0x%x displayMask=0x%x pages=%u colorNeeds=%u",
		               ctxRef, desired.displayWidth, desired.displayHeight,
		               back_depth, desired.displayBestDepth,
		               desired.backBufferDepthMask, desired.displayDepthMask,
		               desired.pageCount, desired.colorNeeds);
		return kDSpInvalidAttributesErr;
	}

	const uint32_t actual_display_width = DSpReserveActualDisplayDimension(
	    ctx->attr.displayWidth, desired.displayWidth);
	const uint32_t actual_display_height = DSpReserveActualDisplayDimension(
	    ctx->attr.displayHeight, desired.displayHeight);
	const uint32_t actual_display_depth = DSpReserveActualDisplayDepth(
	    ctx->attr.displayBestDepth, desired.displayBestDepth, back_depth);
	const uint32_t actual_display_mask = ctx->attr.displayDepthMask
	    ? ctx->attr.displayDepthMask : desired.displayDepthMask;

	ctx->attr.displayWidth = actual_display_width;
	ctx->attr.displayHeight = actual_display_height;
	ctx->attr.backBufferWidth = DSpReserveBackBufferDimension(
	    actual_display_width, desired.displayWidth);
	ctx->attr.backBufferHeight = DSpReserveBackBufferDimension(
	    actual_display_height, desired.displayHeight);
	ctx->attr.backBufferBestDepth = back_depth;
	ctx->attr.displayBestDepth = actual_display_depth;
	ctx->attr.backBufferDepthMask = desired.backBufferDepthMask;
	ctx->attr.displayDepthMask = actual_display_mask;
	ctx->attr.pageCount = desired.pageCount;
	ctx->attr.colorNeeds = desired.colorNeeds;
	ctx->attr.colorTable = desired.colorTable;
	ctx->attr.contextOptions = desired.contextOptions;
	ctx->enumeration_mode_index = DSP_ENUMERATION_INDEX_NONE;
	ctx->explicit_swap_observed = false;
	ctx->swap_generation = 0;
	ctx->front_staging_refresh_swap_generation = 0;
	DSpInitDefaultCLUT(ctx->clut_bytes, ctx->clut_bytes_latched, back_depth);
	dsp_apply_reserve_color_table(ctx, desired.colorTable, back_depth);

	if (!DSpAllocateBackBuffer(ctx, ctx->attr.backBufferWidth,
	                           ctx->attr.backBufferHeight, back_depth))
		return kDSpInternalErr;
	ctx->state = (uint32_t)kDSpContextState_Inactive;
	QD3D_STATE_LOG("DSpReserve(GL): ctx=%u display=%ux%u@%u back=%ux%u@%u pages=%u colorNeeds=%u options=0x%x",
	               ctxRef, ctx->attr.displayWidth, ctx->attr.displayHeight,
	               ctx->attr.displayBestDepth, ctx->attr.backBufferWidth,
	               ctx->attr.backBufferHeight, back_depth,
	               ctx->attr.pageCount, ctx->attr.colorNeeds,
	               ctx->attr.contextOptions);
	return kDSpNoErr;
}

int32_t DSpContext_ReleaseHandler(uint32_t ctxRef)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	if (ctx->state == (uint32_t)kDSpContextState_Active)
		MetalCompositorSubmitFrame_ClearCachedFramebuffer();
	DSpRestoreMainDevicePixMap(ctx);
	DSpReleaseBackBufferNow(ctx);
	s_ctx.erase(ctxRef);
	delete ctx;
	bool any_fs = false;
	for (auto &kv : s_ctx) {
		if (kv.second && kv.second->state == (uint32_t)kDSpContextState_Active)
			any_fs = true;
	}
	DSpHostBridge_SetActiveFullscreen(any_fs);
	return kDSpNoErr;
}

int32_t DSpContext_GetBackBufferHandler(uint32_t ctxRef, uint32_t /*options*/,
                                        uint32_t outCGrafPtrAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx || !ctx->back_buffer || !outCGrafPtrAddr)
		return kDSpInvalidContextErr;
	/* PDF p.51: clean back buffer from underlay when designated */
	restore_underlay_if_any(ctx);
	const uint32_t cgp = DSpGetBackBufferCGrafPtr(ctx);
	if (!cgp) return kDSpInternalErr;
	WriteMacInt32(outCGrafPtrAddr, cgp);
	if (ctx->state == (uint32_t)kDSpContextState_Active)
		DSpRedirectMainDevicePixMap(ctx);
	return kDSpNoErr;
}

static uint32_t DSpMaxFrameRatePacingVBLs(uint32_t max_frame_rate,
	                                      uint64_t cadence_usec)
{
	if (max_frame_rate == 0)
		return 1;
	const uint64_t cadence = GfxFramePacingClampCadenceUsec(
		cadence_usec ? cadence_usec : GFX_FRAME_PACING_DEFAULT_USEC);
	uint64_t refresh_hz = (1000000ull + cadence / 2u) / cadence;
	if (refresh_hz == 0 || (uint64_t)max_frame_rate >= refresh_hz)
		return 1;
	const uint64_t vbls = (refresh_hz + max_frame_rate - 1u) /
	                      max_frame_rate;
	return vbls > UINT32_MAX ? UINT32_MAX : (uint32_t)vbls;
}

static void DSpSyncSwapFramePacing(const DSpContextPrivate *ctx)
{
	assert(ctx != nullptr);
	const uint32_t pacing_vbls = DSpMaxFrameRatePacingVBLs(
		ctx->max_frame_rate, vbl_source_get_cadence_usec());
	for (uint32_t i = 0; i < pacing_vbls; i++) {
		if (vbl_source_sync_3d_pacing_for_engine(
		        kGfxFramePacingEngineDSp) != kGfxAccelNoErr)
			break;
	}
}

static bool DSpPollBusyProc(uint32_t ctx_ref, uint32_t busy_proc,
	                        uint32_t user_refcon)
{
	if (busy_proc == 0)
		return true;
	const uint64_t cadence = GfxFramePacingClampCadenceUsec(
		vbl_source_get_cadence_usec());
	const uint64_t timeout_usec = cadence * 2u;
	for (uint64_t elapsed = 0; elapsed < timeout_usec; elapsed += 500u) {
		if ((call_macos2(busy_proc, ctx_ref, user_refcon) & 0xffu) == 0)
			return true;
		std::this_thread::sleep_for(std::chrono::microseconds(500));
	}
	return false;
}

static DSpContextPrivate *DSpRevalidateSwapContext(
	uint32_t ctx_ref, DSpContextPrivate *expected, uint32_t entry_state)
{
	DSpContextPrivate *fresh = DSpGetContext(ctx_ref);
	if (fresh != expected || fresh->state != entry_state ||
	    !fresh->back_buffer || !fresh->back_texture)
		return nullptr;
	return fresh;
}

int32_t DSpContext_SwapBuffersHandler(uint32_t ctxRef,
	                                  uint32_t busyProcAddr,
	                                  uint32_t userRefCon)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx || !ctx->back_buffer) return kDSpInternalErr;
	ctx->explicit_swap_observed = true;
	ctx->swap_generation++;
	const uint32_t entry_state = ctx->state;
#if QD3D_INIT_LOGGING_ENABLED
	static uint64_t swap_count = 0;
	const uint64_t this_swap = ++swap_count;
	if (this_swap <= 8 || (this_swap & (this_swap - 1u)) == 0 ||
	    (this_swap % 120u) == 0) {
		QD3D_RENDER_LOG("DSpSwap(GL): count=%llu ctx=%u busyProc=0x%08x refCon=0x%08x options=0x%x maxFrameRate=%u syncVBL=%d",
		                (unsigned long long)this_swap, ctxRef, busyProcAddr,
		                userRefCon, ctx->attr.contextOptions,
		                ctx->max_frame_rate,
		                (ctx->attr.contextOptions &
		                 kDSpContextOption_DontSyncVBL) == 0 ? 1 : 0);
	}
#endif

	/* DSp 1.7 defines the second argument as a pre-swap busy callback, not
	 * a completion proc. Movie players also use SwapBuffers as their cadence
	 * boundary, so preserve both the callback gate and VBL synchronization. */
	if (!DSpPollBusyProc(ctxRef, busyProcAddr, userRefCon))
		QD3D_RENDER_LOG("DSpSwap(GL): busyProc timed out after two VBLs; proceeding");
	ctx = DSpRevalidateSwapContext(ctxRef, ctx, entry_state);
	if (!ctx)
		return kDSpInvalidContextErr;
	if ((ctx->attr.contextOptions & kDSpContextOption_DontSyncVBL) == 0) {
		DSpSyncSwapFramePacing(ctx);
		ctx = DSpRevalidateSwapContext(ctxRef, ctx, entry_state);
		if (!ctx)
			return kDSpInvalidContextErr;
	}
	/* Copy guest staging → host back buffer */
	if (ctx->staging_mac_addr && ctx->staging_size) {
		const uint32_t w = DSpContextBackBufferWidth(ctx);
		const uint32_t h = DSpContextBackBufferHeight(ctx);
		const uint32_t row = DSpBackBufferAlignedRowBytes(
		    w, ctx->attr.backBufferBestDepth);
		const uint64_t expected64 = (uint64_t)row * h;
		const uint32_t expected = expected64 <= UINT32_MAX
		    ? (uint32_t)expected64 : 0;
		uint8 *src = Mac2HostAddr(ctx->staging_mac_addr);
		const uint32_t copy_size = expected
		    ? std::min(expected, ctx->staging_size) : 0;
		if (src && copy_size) std::memcpy(ctx->back_buffer, src, copy_size);
		if (copy_size != expected) {
			QD3D_RENDER_LOG("DSpSwap(GL): bounded staging copy ctx=%u have=%u expected=%u copied=%u",
			                ctxRef, ctx->staging_size, expected, copy_size);
		}
	}
	ctx->dirty_cold_start = false;
	ctx->dirty_empty = true;

	/* Present into compositor framebuffer texture path (host expand on next Present) */
	void *fb = MetalCompositorGetFramebufferTexture();
	DSpEncodePresentToFramebuffer(ctx, nullptr, fb);
	return kDSpNoErr;
}

int32_t DSpContext_SetStateHandler(uint32_t ctxRef, uint32_t state)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	uint32_t prev = ctx->state;
	if (prev == state) return kDSpNoErr;

	/* Mode switch on activation when dimensions differ (Metal parity) */
	if (state == (uint32_t)kDSpContextState_Active &&
	    prev != (uint32_t)kDSpContextState_Active) {
		const DMCModeSnapshot *snap = dmc_current_snapshot();
		const uint32_t display_depth =
		    DSpDisplayModeDepth(ctx->attr.backBufferBestDepth,
		                        ctx->attr.displayBestDepth);
		bool mode_differs = (snap == nullptr) ||
		    (snap->width != ctx->attr.displayWidth) ||
		    (snap->height != ctx->attr.displayHeight) ||
		    (snap->depth != display_depth);
		if (mode_differs && ctx->attr.displayWidth && ctx->attr.displayHeight) {
			DMCModeDesc new_mode = {};
			new_mode.width = ctx->attr.displayWidth;
			new_mode.height = ctx->attr.displayHeight;
			new_mode.depth = display_depth ? display_depth : 32;
			new_mode.row_bytes = DSpDisplayModePitch(ctx->attr.displayWidth, new_mode.depth);
			new_mode.pitch = new_mode.row_bytes;
			(void)dmc_request_mode_switch(&new_mode);
		}
	}

	DMCOwner new_owner = DSpMapStateToDMCOwnerTyped(state);
	(void)dmc_set_active_owner((uint32_t)new_owner);
	ctx->state = state;

	if (state == (uint32_t)kDSpContextState_Active) {
		std::memcpy(ctx->clut_bytes_latched, ctx->clut_bytes, 768);
		MetalCompositorUpdatePalette(ctx->clut_bytes, 256);
		dmc_record_gamma_change_with_lut(ctx->gamma_lut_persisted);
		dmc_record_palette_change();
		DSpRedirectMainDevicePixMap(ctx);
		/* Fullscreen Active → host idle-timer suppression flag */
		bool any_fs = false;
		for (auto &kv : s_ctx) {
			if (kv.second && kv.second->state == (uint32_t)kDSpContextState_Active)
				any_fs = true;
		}
		DSpHostBridge_SetActiveFullscreen(any_fs);
	} else if (prev == (uint32_t)kDSpContextState_Active) {
		ctx->fade_state.active = 0;
		MetalCompositorSubmitFrame_ClearCachedFramebuffer();
		DSpRestoreMainDevicePixMap(ctx);
		bool any_fs = false;
		for (auto &kv : s_ctx) {
			if (kv.second && kv.second->state == (uint32_t)kDSpContextState_Active)
				any_fs = true;
		}
		DSpHostBridge_SetActiveFullscreen(any_fs);
	}
	return kDSpNoErr;
}
int32_t DSpContext_GetStateHandler(uint32_t ctxRef, uint32_t outStateAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	if (outStateAddr) WriteMacInt32(outStateAddr, ctx->state);
	return kDSpNoErr;
}
int32_t DSpContext_InvalBackBufferRectHandler(uint32_t ctxRef, uint32_t rectAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	ctx->dirty_empty = false;
	ctx->dirty_cold_start = true;
	if (rectAddr) {
		int16 t = (int16)ReadMacInt16(rectAddr + 0);
		int16 l = (int16)ReadMacInt16(rectAddr + 2);
		int16 b = (int16)ReadMacInt16(rectAddr + 4);
		int16 r = (int16)ReadMacInt16(rectAddr + 6);
		if (ctx->dirty_left == 0 && ctx->dirty_top == 0 &&
		    ctx->dirty_right == 0 && ctx->dirty_bottom == 0) {
			ctx->dirty_top = t; ctx->dirty_left = l;
			ctx->dirty_bottom = b; ctx->dirty_right = r;
		} else {
			if (t < ctx->dirty_top) ctx->dirty_top = t;
			if (l < ctx->dirty_left) ctx->dirty_left = l;
			if (b > ctx->dirty_bottom) ctx->dirty_bottom = b;
			if (r > ctx->dirty_right) ctx->dirty_right = r;
		}
	}
	return kDSpNoErr;
}

/* ---- Remaining handlers: safe no-op / not-found defaults ---- */
#define DSP_STUB_ERR(name) int32_t name { return kDSpNoErr; }
#define DSP_STUB_NF(name) int32_t name { return kDSpContextNotFoundErr; }

int32_t DSpContext_IsBusyHandler(uint32_t ctxRef, uint32_t outBusy) {
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	if (outBusy) WriteMacInt32(outBusy, 0); /* always idle on host path */
	return kDSpNoErr;
}
int32_t DSpContext_GetDisplayIDHandler(uint32_t ctxRef, uint32_t outId) {
	if (!DSpGetContext(ctxRef)) return kDSpContextNotFoundErr;
	/* Single display id = 0 */
	if (outId) WriteMacInt32(outId, 0);
	return kDSpNoErr;
}
int32_t DSpContext_GetDirtyRectGridUnitsHandler(uint32_t ctxRef, uint32_t a, uint32_t b) {
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	if (a) WriteMacInt32(a, ctx->dirty_grid_w ? ctx->dirty_grid_w : 1);
	if (b) WriteMacInt32(b, ctx->dirty_grid_h ? ctx->dirty_grid_h : 1);
	return kDSpNoErr;
}
int32_t DSpContext_GetMaxFrameRateHandler(uint32_t ctxRef, uint32_t o) {
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	if (o) WriteMacInt32(o, ctx->max_frame_rate ? ctx->max_frame_rate : 60);
	return kDSpNoErr;
}
int32_t DSpContext_SetMaxFrameRateHandler(uint32_t ctxRef, uint32_t rate) {
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	ctx->max_frame_rate = rate ? rate : 60;
	return kDSpNoErr;
}
int32_t DSpContext_GetMonitorFrequencyHandler(uint32_t ctxRef, uint32_t o) {
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	if (o) WriteMacInt32(o, ctx->attr.frequency ? ctx->attr.frequency : 60);
	return kDSpNoErr;
}
int32_t DSpContext_SetDirtyRectGridSizeHandler(uint32_t ctxRef, uint32_t gw, uint32_t gh) {
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	ctx->dirty_grid_w = gw ? gw : 1;
	ctx->dirty_grid_h = gh ? gh : 1;
	return kDSpNoErr;
}
int32_t DSpContext_GetDirtyRectGridSizeHandler(uint32_t ctxRef, uint32_t a, uint32_t b) {
	return DSpContext_GetDirtyRectGridUnitsHandler(ctxRef, a, b);
}
int32_t DSpContext_GetFrontBufferHandler(uint32_t ctxRef, uint32_t outPtr) {
	return DSpContext_GetBackBufferHandler(ctxRef, 0, outPtr);
}
int32_t DSpGetCurrentContextHandler(uint32_t /*displayID*/, uint32_t outCtx) {
	/* First Active context, else first reserved */
	uint32_t found = 0;
	for (auto &kv : s_ctx) {
		if (kv.second && kv.second->state == (uint32_t)kDSpContextState_Active) {
			found = kv.first; break;
		}
	}
	if (!found) {
		for (auto &kv : s_ctx) { if (kv.second) { found = kv.first; break; } }
	}
	if (outCtx) WriteMacInt32(outCtx, found);
	return kDSpNoErr;
}
int32_t DSpGetMouseHandler(uint32_t outGlobalPointAddr)
{
	if (!outGlobalPointAddr) return kDSpInternalErr;
	/* Lowmem MTemp / Mouse at classic locations — 0x0828 is MTemp (Point) */
	int16 y = (int16)ReadMacInt16(0x0828);
	int16 x = (int16)ReadMacInt16(0x082A);
	WriteMacInt16(outGlobalPointAddr + 0, (uint16)y);
	WriteMacInt16(outGlobalPointAddr + 2, (uint16)x);
	return kDSpNoErr;
}
int32_t DSpContext_GlobalToLocalHandler(uint32_t ctxRef, uint32_t ioPoint)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx || !ioPoint) return kDSpContextNotFoundErr;
	/* Global mac coords → local: subtract context origin (0,0 for full-screen DSp) */
	int16 y = (int16)ReadMacInt16(ioPoint + 0);
	int16 x = (int16)ReadMacInt16(ioPoint + 2);
	/* display is typically origin 0,0 */
	(void)ctx;
	WriteMacInt16(ioPoint + 0, (uint16)y);
	WriteMacInt16(ioPoint + 2, (uint16)x);
	return kDSpNoErr;
}
int32_t DSpContext_LocalToGlobalHandler(uint32_t ctxRef, uint32_t ioPoint)
{
	return DSpContext_GlobalToLocalHandler(ctxRef, ioPoint);
}
int32_t DSpFindContextFromPointHandler(int16_t /*y*/, int16_t /*x*/, uint32_t o) {
	/* Single-display: return Active context if any */
	uint32_t found = 0;
	for (auto &kv : s_ctx) {
		if (kv.second && kv.second->state == (uint32_t)kDSpContextState_Active) {
			found = kv.first; break;
		}
	}
	if (o) WriteMacInt32(o, found);
	return kDSpNoErr;
}
int32_t DSpSetDebugModeHandler(uint32_t) { return kDSpNoErr; }

static void dsp_read_attr_from_guest(uint32_t attrAddr, DSpContextAttributes *req)
{
	if (!attrAddr || !req) return;
	std::memset(req, 0, sizeof(*req));
	req->frequency = ReadMacInt32(attrAddr + 0);
	req->displayWidth = ReadMacInt32(attrAddr + 4);
	req->displayHeight = ReadMacInt32(attrAddr + 8);
	req->colorNeeds = ReadMacInt32(attrAddr + 20);
	req->colorTable = ReadMacInt32(attrAddr + 24);
	req->contextOptions = ReadMacInt32(attrAddr + 28);
	req->backBufferDepthMask = ReadMacInt32(attrAddr + 32);
	req->displayDepthMask = ReadMacInt32(attrAddr + 36);
	req->backBufferBestDepth = ReadMacInt32(attrAddr + 40);
	req->displayBestDepth = ReadMacInt32(attrAddr + 44);
	req->pageCount = ReadMacInt32(attrAddr + 48);
}

int32_t DSpCanUserSelectContextHandler(uint32_t attrAddr, uint32_t outCanSelect)
{
	if (!outCanSelect) return kDSpInternalErr;
	DSpContextAttributes req = {};
	if (attrAddr) dsp_read_attr_from_guest(attrAddr, &req);
	size_t n = DSpUserSelectableModeCount(&req);
	WriteMacInt32(outCanSelect, DSpCanUserSelectContextFromCount(n) ? 1 : 0);
	return kDSpNoErr;
}
int32_t DSpFindBestContextOnDisplayIDHandler(uint32_t attrAddr, uint32_t /*displayID*/, uint32_t outCtx)
{
	/* Single display — same as FindBestContext */
	return DSpFindBestContextHandler(attrAddr, outCtx);
}
int32_t DSpUserSelectContextHandler(uint32_t attrAddr, uint32_t /*dialogID*/,
                                    uint32_t /*userData*/, uint32_t outCtx)
{
	/* No GUI on desktop OpenGL path: pick best matching mode automatically */
	return DSpFindBestContextHandler(attrAddr, outCtx);
}
int32_t DSpSetBlankingColorHandler(uint32_t rgbColorAddr)
{
	/* RGBColor: 3× uint16 BE at +0/+2/+4 */
	uint8_t rgba[4] = { 0, 0, 0, 255 };
	if (rgbColorAddr) {
		rgba[0] = (uint8_t)(ReadMacInt16(rgbColorAddr + 0) >> 8);
		rgba[1] = (uint8_t)(ReadMacInt16(rgbColorAddr + 2) >> 8);
		rgba[2] = (uint8_t)(ReadMacInt16(rgbColorAddr + 4) >> 8);
	}
	dmc_set_blanking_color(rgba);
	return kDSpNoErr;
}

/* ---- AltBuffer (host heap, Metal-parity API surface) ---- */
#ifndef DSP_MAX_ALT_BUFFERS
#define DSP_MAX_ALT_BUFFERS 16
#endif

struct DSpAltBufferGL {
	bool in_use = false;
	void *backing = nullptr;       /* host BGRA8 pixels */
	uint32_t width = 0, height = 0;
	uint32_t row_bytes = 0;
	uint32_t options = 0;
	bool underlay_capable = true;
	uint32_t baseaddr_mac = 0;     /* guest staging for CGrafPort baseAddr */
	uint32_t baseaddr_size = 0;
	uint32_t cgrafptr_mac = 0;
	int16_t dirty_left = 0, dirty_top = 0, dirty_right = 0, dirty_bottom = 0;
	bool dirty_empty = true;
};

static DSpAltBufferGL s_alt[DSP_MAX_ALT_BUFFERS];

static DSpAltBufferGL *alt_get(uint32_t handle)
{
	if (handle == 0 || handle > DSP_MAX_ALT_BUFFERS) return nullptr;
	DSpAltBufferGL *r = &s_alt[handle - 1];
	return r->in_use ? r : nullptr;
}

static uint32_t alt_alloc_slot(void)
{
	for (int i = 0; i < DSP_MAX_ALT_BUFFERS; i++) {
		if (!s_alt[i].in_use) {
			s_alt[i] = DSpAltBufferGL{};
			s_alt[i].in_use = true;
			s_alt[i].dirty_empty = true;
			return (uint32_t)(i + 1);
		}
	}
	return 0;
}

static void alt_free(uint32_t handle)
{
	DSpAltBufferGL *r = alt_get(handle);
	if (!r) return;
	if (r->backing) {
		gfxaccel_resources_clear_buffer_owner(r->backing);
		gfxaccel_resources_heap_mm_free_buffer(kHeapEngineDSp, r->backing);
		r->backing = nullptr;
	}
	r->in_use = false;
	r->baseaddr_mac = 0;
	r->cgrafptr_mac = 0;
}

int32_t DSpAltBuffer_NewHandler(uint32_t ctxRef, uint32_t /*inVRAM*/, uint32_t /*inAttributes*/,
                                uint32_t outAltBuffer)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	uint32_t w = ctx->attr.displayWidth ? ctx->attr.displayWidth : 640;
	uint32_t h = ctx->attr.displayHeight ? ctx->attr.displayHeight : 480;
	if (w > DSP_ALT_MAX_DIM || h > DSP_ALT_MAX_DIM) return kDSpInternalErr;
	uint32_t handle = alt_alloc_slot();
	if (!handle) return kDSpInternalErr;
	DSpAltBufferGL *rec = &s_alt[handle - 1];
	uint32_t rb = ((w * 4) + 255u) & ~255u;
	uint32_t size = rb * h;
	void *buf = gfxaccel_resources_heap_alloc_buffer(kHeapEngineDSp, size, 0);
	if (!buf) {
		alt_free(handle);
		return kDSpInternalErr;
	}
	std::memset(buf, 0, size);
	rec->backing = buf;
	rec->width = w;
	rec->height = h;
	rec->row_bytes = rb;
	rec->underlay_capable = true;
	gfxaccel_resources_set_buffer_owner(buf, kGfxEngineDSp);
	/* Guest pixel staging so Mac can draw into the alt buffer */
	uint32 mac = Mac_sysalloc(size);
	if (mac) {
		rec->baseaddr_mac = mac;
		rec->baseaddr_size = size;
		std::memset(Mac2HostAddr(mac), 0, size);
	}
	if (outAltBuffer) WriteMacInt32(outAltBuffer, handle);
	return kDSpNoErr;
}

int32_t DSpAltBuffer_DisposeHandler(uint32_t altBuffer)
{
	/* Clear underlay refs */
	for (auto &kv : s_ctx) {
		if (kv.second && kv.second->underlay_alt_buffer == altBuffer)
			kv.second->underlay_alt_buffer = 0;
	}
	alt_free(altBuffer);
	return kDSpNoErr;
}

int32_t DSpAltBuffer_GetCGrafPtrHandler(uint32_t altBuffer, uint32_t outCGraf, uint32_t /*outGamePort*/)
{
	DSpAltBufferGL *rec = alt_get(altBuffer);
	if (!rec) return kDSpInternalErr;
	if (!rec->cgrafptr_mac) {
		/* Compact 24-byte shim: baseAddr, rowBytes, bounds — enough for many games */
		uint32 mac = Mac_sysalloc(32);
		if (!mac) return kDSpInternalErr;
		rec->cgrafptr_mac = mac;
		WriteMacInt32(mac + 0, rec->baseaddr_mac ? rec->baseaddr_mac :
		              (uint32)(uintptr_t)rec->backing); /* best-effort */
		WriteMacInt16(mac + 4, (uint16)(rec->row_bytes | 0x8000)); /* PixMap flag */
		WriteMacInt16(mac + 8, 0);  /* top */
		WriteMacInt16(mac + 10, 0); /* left */
		WriteMacInt16(mac + 12, (int16)rec->height);
		WriteMacInt16(mac + 14, (int16)rec->width);
	}
	/* Sync guest staging → host backing before present paths read it */
	if (rec->baseaddr_mac && rec->backing) {
		uint8 *src = Mac2HostAddr(rec->baseaddr_mac);
		if (src) std::memcpy(rec->backing, src, rec->baseaddr_size);
	}
	if (outCGraf) WriteMacInt32(outCGraf, rec->cgrafptr_mac);
	return kDSpNoErr;
}

int32_t DSpAltBuffer_InvalRectHandler(uint32_t altBuffer, uint32_t rectAddr)
{
	DSpAltBufferGL *rec = alt_get(altBuffer);
	if (!rec) return kDSpInternalErr;
	if (rectAddr) {
		int16 t = (int16)ReadMacInt16(rectAddr + 0);
		int16 l = (int16)ReadMacInt16(rectAddr + 2);
		int16 b = (int16)ReadMacInt16(rectAddr + 4);
		int16 r = (int16)ReadMacInt16(rectAddr + 6);
		if (rec->dirty_empty) {
			rec->dirty_top = t; rec->dirty_left = l;
			rec->dirty_bottom = b; rec->dirty_right = r;
			rec->dirty_empty = false;
		} else {
			if (t < rec->dirty_top) rec->dirty_top = t;
			if (l < rec->dirty_left) rec->dirty_left = l;
			if (b > rec->dirty_bottom) rec->dirty_bottom = b;
			if (r > rec->dirty_right) rec->dirty_right = r;
		}
	} else {
		rec->dirty_empty = false;
		rec->dirty_top = 0; rec->dirty_left = 0;
		rec->dirty_bottom = (int16)rec->height;
		rec->dirty_right = (int16)rec->width;
	}
	return kDSpNoErr;
}

int32_t DSpContext_GetUnderlayAltBufferHandler(uint32_t ctxRef, uint32_t o)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	if (o) WriteMacInt32(o, ctx->underlay_alt_buffer);
	return kDSpNoErr;
}
int32_t DSpContext_SetUnderlayAltBufferHandler(uint32_t ctxRef, uint32_t altBuffer)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	if (altBuffer && !alt_get(altBuffer)) return kDSpInternalErr;
	ctx->underlay_alt_buffer = altBuffer;
	return kDSpNoErr;
}

/* Restore underlay into back buffer on GetBackBuffer when designated */
static void restore_underlay_if_any(DSpContextPrivate *ctx)
{
	if (!ctx || !ctx->underlay_alt_buffer || !ctx->back_buffer) return;
	DSpAltBufferGL *rec = alt_get(ctx->underlay_alt_buffer);
	if (!rec || !rec->backing) return;
	if (rec->baseaddr_mac) {
		uint8 *src = Mac2HostAddr(rec->baseaddr_mac);
		if (src) std::memcpy(rec->backing, src, rec->baseaddr_size);
	}
	const uint32_t w = DSpContextBackBufferWidth(ctx);
	const uint32_t h = DSpContextBackBufferHeight(ctx);
	const uint32_t depth = ctx->attr.backBufferBestDepth;
	if (depth == 32 && rec->width == w && rec->height == h) {
		/* Direct copy when formats match BGRA/ARGB size */
		const size_t expected = (size_t)DSpBackBufferAlignedRowBytes(w, depth) * h;
		const size_t available = (size_t)rec->row_bytes * rec->height;
		std::memcpy(ctx->back_buffer, rec->backing,
		            std::min(expected, available));
	}
}

/* Resolve compact DSp shim or real CGrafPort/PixMap into blit origin */
static bool dsp_resolve_blit_side(uint32_t cgrafptr, uint32_t rect_mac,
                                  uint32_t *out_base, int32_t *out_rb,
                                  uint32_t *out_bpp, uint32_t *out_bits,
                                  int32_t *out_w, int32_t *out_h)
{
	if (!cgrafptr || !rect_mac || !out_base) return false;
	uint32_t pixmap = cgrafptr;
	uint16_t portVer = (uint16_t)ReadMacInt16(cgrafptr + DSP_CGRAFPORT_OFF_PORT_VERSION);
	if ((portVer & 0xC000) != 0) {
		/* Real Color CGrafPort: portPixMap Handle at +2 */
		uint32_t pmH = ReadMacInt32(cgrafptr + DSP_CGRAFPORT_OFF_PORT_PIXMAP);
		if (pmH) {
			uint32_t pm = ReadMacInt32(pmH);
			if (pm) pixmap = pm;
		}
	}
	uint32_t base = ReadMacInt32(pixmap + 0);
	uint16_t rb_raw = (uint16_t)ReadMacInt16(pixmap + 4);
	int32_t rb = (int32_t)(rb_raw & 0x7FFF);
	if (!base || rb == 0) return false;
	int16 bt = (int16)ReadMacInt16(pixmap + 6);
	int16 bl = (int16)ReadMacInt16(pixmap + 8);
	int16 bb = (int16)ReadMacInt16(pixmap + 10);
	int16 br = (int16)ReadMacInt16(pixmap + 12);
	/* pixelSize: compact shim @16, real PixMap @32 — try compact first */
	uint16_t ps = (uint16_t)ReadMacInt16(pixmap + 16);
	if (ps != 8 && ps != 16 && ps != 32)
		ps = (uint16_t)ReadMacInt16(pixmap + 32);
	if (ps != 8 && ps != 16 && ps != 32) ps = 32;
	uint32_t bpp = (ps <= 8) ? 1u : (ps <= 16) ? 2u : 4u;
	int16 rt = (int16)ReadMacInt16(rect_mac + 0);
	int16 rl = (int16)ReadMacInt16(rect_mac + 2);
	int16 rb2 = (int16)ReadMacInt16(rect_mac + 4);
	int16 rr = (int16)ReadMacInt16(rect_mac + 6);
	int32_t ct = rt < bt ? bt : rt;
	int32_t cl = rl < bl ? bl : rl;
	int32_t cb = rb2 > bb ? bb : rb2;
	int32_t cr = rr > br ? br : rr;
	int32_t w = cr - cl, h = cb - ct;
	if (w <= 0 || h <= 0) return false;
	*out_base = base + (uint32_t)(ct - bt) * (uint32_t)rb + (uint32_t)(cl - bl) * bpp;
	*out_rb = rb;
	*out_bpp = bpp;
	*out_bits = ps;
	*out_w = w;
	*out_h = h;
	return true;
}

static void dsp_blit_complete(uint32_t inBlitInfo, uint32_t inAsyncFlag)
{
	NQDMetalFlush();
	WriteMacInt8(inBlitInfo + DSP_BLITINFO_OFF_completionFlag, 1);
	uint32_t completionProc = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_completionProc);
	if (inAsyncFlag && completionProc) {
		(void)call_macos1(completionProc, inBlitInfo);
	}
}

int32_t DSpBlit_FastestHandler(uint32_t inBlitInfo, uint32_t inAsyncFlag)
{
	if (!inBlitInfo) return kDSpInternalErr;
	uint32_t srcBuffer = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_srcBuffer);
	uint32_t dstBuffer = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_dstBuffer);
	uint32_t srcRect = inBlitInfo + DSP_BLITINFO_OFF_srcRect;
	uint32_t dstRect = inBlitInfo + DSP_BLITINFO_OFF_dstRect;
	uint32_t srcKey = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_srcKey);
	uint32_t mode = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_mode);
	uint32_t sb, db, sbpp, dbpp, sbits, dbits;
	int32_t srb, drb, sw, sh, dw, dh;
	if (!dsp_resolve_blit_side(srcBuffer, srcRect, &sb, &srb, &sbpp, &sbits, &sw, &sh) ||
	    !dsp_resolve_blit_side(dstBuffer, dstRect, &db, &drb, &dbpp, &dbits, &dw, &dh))
		return kDSpInternalErr;
	if (sbpp != dbpp) return kDSpInternalErr;
	int32_t w = sw < dw ? sw : dw;
	int32_t h = sh < dh ? sh : dh;
	uint32_t xfer = (mode & (uint32_t)kDSpBlitMode_SrcKey) ? 36u : 0u;
	if (!NQDMetalBitblt1to1(sb, srb, db, drb, dbpp, dbits, (uint32_t)w, (uint32_t)h, xfer, srcKey))
		return kDSpInternalErr;
	dsp_blit_complete(inBlitInfo, inAsyncFlag);
	return kDSpNoErr;
}

int32_t DSpBlit_FasterHandler(uint32_t inBlitInfo, uint32_t inAsyncFlag)
{
	if (!inBlitInfo) return kDSpInternalErr;
	uint32_t srcBuffer = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_srcBuffer);
	uint32_t dstBuffer = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_dstBuffer);
	uint32_t srcRect = inBlitInfo + DSP_BLITINFO_OFF_srcRect;
	uint32_t dstRect = inBlitInfo + DSP_BLITINFO_OFF_dstRect;
	uint32_t srcKey = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_srcKey);
	uint32_t mode = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_mode);
	uint32_t sb, db, sbpp, dbpp, sbits, dbits;
	int32_t srb, drb, sw, sh, dw, dh;
	if (!dsp_resolve_blit_side(srcBuffer, srcRect, &sb, &srb, &sbpp, &sbits, &sw, &sh) ||
	    !dsp_resolve_blit_side(dstBuffer, dstRect, &db, &drb, &dbpp, &dbits, &dw, &dh))
		return kDSpInternalErr;
	if (sbpp != dbpp) return kDSpInternalErr;
	uint32_t interp = (mode & (uint32_t)kDSpBlitMode_Interpolation) ? 1u : 0u;
	uint32_t key_en = (mode & (uint32_t)kDSpBlitMode_SrcKey) ? 1u : 0u;
	if (sw == dw && sh == dh) {
		return DSpBlit_FastestHandler(inBlitInfo, inAsyncFlag);
	}
	if (!NQDMetalBitbltScaled(sb, srb, db, drb, dbpp, dbits,
	                          (uint32_t)sw, (uint32_t)sh, (uint32_t)dw, (uint32_t)dh,
	                          interp, srcKey, 0, key_en))
		return kDSpInternalErr;
	dsp_blit_complete(inBlitInfo, inAsyncFlag);
	return kDSpNoErr;
}

int32_t DSpContext_GetFlattenedSizeHandler(uint32_t ctxRef, uint32_t outSize)
{
	if (!outSize) return kDSpInternalErr;
	if (!DSpGetContext(ctxRef)) return kDSpContextNotFoundErr;
	WriteMacInt32(outSize, DSP_FLAT_SIZE);
	return kDSpNoErr;
}
int32_t DSpContext_FlattenHandler(uint32_t ctxRef, uint32_t outFlat)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!outFlat) return kDSpInternalErr;
	if (!ctx) return kDSpContextNotFoundErr;
	const DSpContextAttributes *a = &ctx->attr;
	WriteMacInt32(outFlat + DSP_FLAT_OFF_magic, DSP_FLAT_MAGIC);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_version, DSP_FLAT_VERSION);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_size, DSP_FLAT_SIZE);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_displayWidth, a->displayWidth);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_displayHeight, a->displayHeight);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_colorNeeds, a->colorNeeds);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_colorTable, a->colorTable);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_contextOptions, a->contextOptions);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_backBufferDepthMask, a->backBufferDepthMask);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_displayDepthMask, a->displayDepthMask);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_backBufferBestDepth, a->backBufferBestDepth);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_displayBestDepth, a->displayBestDepth);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_pageCount, a->pageCount);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_gameMustConfirmSwitch, a->gameMustConfirmSwitch);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_max_frame_rate, ctx->max_frame_rate);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_dirty_grid_w, ctx->dirty_grid_w);
	WriteMacInt32(outFlat + DSP_FLAT_OFF_dirty_grid_h, ctx->dirty_grid_h);
	return kDSpNoErr;
}
int32_t DSpContext_RestoreHandler(uint32_t flatAddr, uint32_t outCtx)
{
	if (!flatAddr) return kDSpInternalErr;
	if (ReadMacInt32(flatAddr + DSP_FLAT_OFF_magic) != DSP_FLAT_MAGIC)
		return kDSpContextNotFoundErr;
	if (ReadMacInt32(flatAddr + DSP_FLAT_OFF_version) != DSP_FLAT_VERSION)
		return kDSpContextNotFoundErr;
	DSpContextAttributes attr = {};
	attr.displayWidth = ReadMacInt32(flatAddr + DSP_FLAT_OFF_displayWidth);
	attr.displayHeight = ReadMacInt32(flatAddr + DSP_FLAT_OFF_displayHeight);
	attr.colorNeeds = ReadMacInt32(flatAddr + DSP_FLAT_OFF_colorNeeds);
	attr.colorTable = ReadMacInt32(flatAddr + DSP_FLAT_OFF_colorTable);
	attr.contextOptions = ReadMacInt32(flatAddr + DSP_FLAT_OFF_contextOptions);
	attr.backBufferDepthMask = ReadMacInt32(flatAddr + DSP_FLAT_OFF_backBufferDepthMask);
	attr.displayDepthMask = ReadMacInt32(flatAddr + DSP_FLAT_OFF_displayDepthMask);
	attr.backBufferBestDepth = ReadMacInt32(flatAddr + DSP_FLAT_OFF_backBufferBestDepth);
	attr.displayBestDepth = ReadMacInt32(flatAddr + DSP_FLAT_OFF_displayBestDepth);
	attr.pageCount = ReadMacInt32(flatAddr + DSP_FLAT_OFF_pageCount);
	attr.gameMustConfirmSwitch = ReadMacInt32(flatAddr + DSP_FLAT_OFF_gameMustConfirmSwitch);
	uint32_t h = DSpAllocFirstContextHandle(&attr, DSP_ENUMERATION_INDEX_NONE);
	DSpContextPrivate *ctx = DSpGetContext(h);
	if (ctx) {
		ctx->max_frame_rate = ReadMacInt32(flatAddr + DSP_FLAT_OFF_max_frame_rate);
		ctx->dirty_grid_w = ReadMacInt32(flatAddr + DSP_FLAT_OFF_dirty_grid_w);
		ctx->dirty_grid_h = ReadMacInt32(flatAddr + DSP_FLAT_OFF_dirty_grid_h);
	}
	if (outCtx) WriteMacInt32(outCtx, h);
	return kDSpNoErr;
}
int32_t DSpContext_QueueHandler(uint32_t parentRef, uint32_t childRef, uint32_t /*flags*/)
{
	DSpContextPrivate *parent = DSpGetContext(parentRef);
	if (!parent || !DSpGetContext(childRef)) return kDSpContextNotFoundErr;
	parent->queued_child = childRef;
	return kDSpNoErr;
}
int32_t DSpContext_SwitchHandler(uint32_t oldRef, uint32_t newRef)
{
	DSpContextPrivate *oldc = DSpGetContext(oldRef);
	DSpContextPrivate *newc = DSpGetContext(newRef);
	if (!oldc || !newc) return kDSpContextNotFoundErr;
	if (oldc->queued_child != newRef) return kDSpInternalErr;
	oldc->state = (uint32_t)kDSpContextState_Inactive;
	newc->state = (uint32_t)kDSpContextState_Active;
	oldc->queued_child = 0;
	return kDSpNoErr;
}
void DSpContext_SetStateSwitchHandoff(uint32_t) {}
int32_t DSpContext_SetCLUTEntriesHandler(uint32_t ctxRef, uint32_t entriesAddr,
                                         uint32_t inStartingEntry, uint32_t inEntryCount)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	if (!entriesAddr) return kDSpInternalErr;
	if (inEntryCount == 0) return kDSpNoErr;
	/* Match Metal dsp_clut_gamma.mm: overflow-safe range check */
	if (inStartingEntry > 255 || inEntryCount > 256 - inStartingEntry)
		return kDSpInternalErr;
	/*
	 * Guest wire format is ColorSpec (8 bytes): value@0, r@2, g@4, b@6 as
	 * big-endian 16-bit channels (DSp 1.7 p.56). Internal storage is 8-bit RGB.
	 */
	for (uint32_t i = 0; i < inEntryCount; i++) {
		uint32 e = entriesAddr + i * 8;
		uint32_t idx = inStartingEntry + i;
		ctx->clut_bytes[idx * 3 + 0] = (uint8_t)(ReadMacInt16(e + 2) >> 8);
		ctx->clut_bytes[idx * 3 + 1] = (uint8_t)(ReadMacInt16(e + 4) >> 8);
		ctx->clut_bytes[idx * 3 + 2] = (uint8_t)(ReadMacInt16(e + 6) >> 8);
	}
	/* When Active, push latched + compositor palette (Metal Active-path parity) */
	if (ctx->state == (uint32_t)kDSpContextState_Active) {
		std::memcpy(ctx->clut_bytes_latched, ctx->clut_bytes, 768);
		MetalCompositorUpdatePalette(ctx->clut_bytes, 256);
	}
	return kDSpNoErr;
}
int32_t DSpContext_GetCLUTEntriesHandler(uint32_t ctxRef, uint32_t entriesOutAddr,
                                         uint32_t inStartingEntry, uint32_t inEntryCount)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	if (!entriesOutAddr) return kDSpInternalErr;
	if (inEntryCount == 0) return kDSpNoErr;
	if (inStartingEntry > 255 || inEntryCount > 256 - inStartingEntry)
		return kDSpInternalErr;
	/* Write ColorSpec array; 8->16 expand (v<<8)|v like Metal Get path */
	for (uint32_t i = 0; i < inEntryCount; i++) {
		uint32 e = entriesOutAddr + i * 8;
		uint32_t idx = inStartingEntry + i;
		uint8_t r = ctx->clut_bytes_latched[idx * 3 + 0];
		uint8_t g = ctx->clut_bytes_latched[idx * 3 + 1];
		uint8_t b = ctx->clut_bytes_latched[idx * 3 + 2];
		WriteMacInt16(e + 0, 0);
		WriteMacInt16(e + 2, (uint16)((r << 8) | r));
		WriteMacInt16(e + 4, (uint16)((g << 8) | g));
		WriteMacInt16(e + 6, (uint16)((b << 8) | b));
	}
	return kDSpNoErr;
}
static void DSpWriteAttributesCore(const DSpContextAttributes *attr,
                                   uint32_t outAttrAddr)
{
	/* DSpContextAttributes uses the fixed 72-byte big-endian guest layout.
	 * Copying the native C++ struct exposes little-endian fields on Windows
	 * and also relies on host padding. */
	WriteMacInt32(outAttrAddr +  0, 0);
	WriteMacInt32(outAttrAddr +  4, attr->displayWidth);
	WriteMacInt32(outAttrAddr +  8, attr->displayHeight);
	WriteMacInt32(outAttrAddr + 12, 0);
	WriteMacInt32(outAttrAddr + 16, 0);
	WriteMacInt32(outAttrAddr + 20, attr->colorNeeds);
	WriteMacInt32(outAttrAddr + 24, attr->colorTable);
	WriteMacInt32(outAttrAddr + 28, attr->contextOptions);
	WriteMacInt32(outAttrAddr + 32, attr->backBufferDepthMask);
	WriteMacInt32(outAttrAddr + 36, attr->displayDepthMask);
	WriteMacInt32(outAttrAddr + 40, attr->backBufferBestDepth);
	WriteMacInt32(outAttrAddr + 44, attr->displayBestDepth);
	WriteMacInt32(outAttrAddr + 48, attr->pageCount);
	WriteMacInt8(outAttrAddr + 52, 0);
	WriteMacInt8(outAttrAddr + 53, 0);
	WriteMacInt8(outAttrAddr + 54, 0);
	WriteMacInt8(outAttrAddr + 55, attr->gameMustConfirmSwitch ? 1 : 0);
	WriteMacInt32(outAttrAddr + 56, 0);
	WriteMacInt32(outAttrAddr + 60, 0);
	WriteMacInt32(outAttrAddr + 64, 0);
	WriteMacInt32(outAttrAddr + 68, 0);
}

extern "C" int32_t DSpContext_GetAttributesHandler(uint32_t ctxRef, uint32_t outAttrAddr)
{
	if (!outAttrAddr) return kDSpInvalidAttributesErr;
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpInvalidContextErr;
	DSpContextAttributes public_attr = ctx->attr;
	DSpNormalizeGetAttributesForState(&public_attr, ctx->state);
	DSpWriteAttributesCore(&public_attr, outAttrAddr);
	return kDSpNoErr;
}
int32_t DSpGetActiveCLUTSnapshot(uint8_t out[768]) {
	if (!out) return kDSpNoErr;
	/* Prefer Active context latched palette; else any reserved; else identity */
	for (auto &kv : s_ctx) {
		if (kv.second && kv.second->state == (uint32_t)kDSpContextState_Active) {
			std::memcpy(out, kv.second->clut_bytes_latched, 768);
			return kDSpNoErr;
		}
	}
	for (auto &kv : s_ctx) {
		if (kv.second) {
			std::memcpy(out, kv.second->clut_bytes_latched, 768);
			return kDSpNoErr;
		}
	}
	for (int i = 0; i < 256; i++) {
		out[i * 3] = out[i * 3 + 1] = out[i * 3 + 2] = (uint8_t)i;
	}
	return kDSpNoErr;
}
/* Gamma layout in DMC/Metal: planar 256R+256G+256B. Persist field is the same. */
static void gamma_identity(uint8_t lut[768])
{
	for (int c = 0; c < 3; c++)
		for (int i = 0; i < 256; i++)
			lut[c * 256 + i] = (uint8_t)i;
}
static void gamma_copy_driver(uint8_t lut[768])
{
	const DMCModeSnapshot *snap = dmc_current_snapshot();
	if (snap) {
		std::memcpy(lut, snap->driver_gamma_lut, 768);
	} else {
		gamma_identity(lut);
	}
}
static void gamma_read_zero_color(uint32_t color_addr,
	                              uint8_t *r, uint8_t *g, uint8_t *b)
{
	*r = *g = *b = 0;
	if (!color_addr || !NQDMetalAddrInBuffer(color_addr) ||
	    !NQDMetalAddrInBuffer(color_addr + 5u)) return;
	*r = (uint8_t)(ReadMacInt16(color_addr + 0u) >> 8);
	*g = (uint8_t)(ReadMacInt16(color_addr + 2u) >> 8);
	*b = (uint8_t)(ReadMacInt16(color_addr + 4u) >> 8);
}
static void gamma_compute_target(uint8_t color_r, uint8_t color_g,
	                             uint8_t color_b, int32_t percent,
	                             const uint8_t full_lut[768],
	                             uint8_t out_lut[768])
{
	const uint8_t tint[3] = { color_r, color_g, color_b };
	for (uint32_t c = 0; c < 3; c++) {
		for (uint32_t i = 0; i < 256; i++) {
			const int32_t zero = tint[c];
			const int32_t full = full_lut[c * 256u + i];
			int32_t value = 0;
			if (percent <= 0) {
				int32_t amount = percent <= -100 ? 100 : -percent;
				value = (zero * (100 - amount)) / 100;
			} else if (percent >= 100) {
				int32_t amount = percent - 100;
				if (amount > 100) amount = 100;
				value = full + ((255 - full) * amount) / 100;
			} else {
				value = (zero * (100 - percent) + full * percent) / 100;
			}
			out_lut[c * 256u + i] =
			    (uint8_t)std::max(0, std::min(255, value));
		}
	}
}
static uint16_t gamma_one_second_vbls()
{
	const uint64_t cadence = vbl_source_get_cadence_usec();
	uint64_t count = cadence ? (1000000ull + cadence / 2u) / cadence : 60u;
	if (!count) count = 1;
	if (count > 4096) count = 4096;
	return (uint16_t)count;
}
static void gamma_interp(const uint8_t *a, const uint8_t *b, uint32_t e, uint32_t d, uint8_t *out)
{
	if (d == 0) { std::memcpy(out, b, 768); return; }
	for (int i = 0; i < 768; i++) {
		int32_t v = ((int32_t)a[i] * (int32_t)(d - e) + (int32_t)b[i] * (int32_t)e) / (int32_t)d;
		if (v < 0) v = 0;
		if (v > 255) v = 255;
		out[i] = (uint8_t)v;
	}
}
static void gamma_begin_fade(DSpContextPrivate *ctx, const uint8_t end_lut[768], uint16_t duration_vbls)
{
	if (!ctx) return;
	if (ctx->fade_state.active) {
		/* Restart from current interpolated position */
		uint8_t cur[768];
		gamma_interp(ctx->fade_state.start_lut, ctx->fade_state.end_lut,
		             ctx->fade_state.elapsed_vbls, ctx->fade_state.duration_vbls, cur);
		std::memcpy(ctx->fade_state.start_lut, cur, 768);
	} else {
		std::memcpy(ctx->fade_state.start_lut, ctx->gamma_lut_persisted, 768);
	}
	std::memcpy(ctx->fade_state.end_lut, end_lut, 768);
	ctx->fade_state.duration_vbls = duration_vbls ? duration_vbls : 1;
	ctx->fade_state.elapsed_vbls = 0;
	ctx->fade_state.active = 1;
	if (duration_vbls <= 1) {
		/* Instant */
		std::memcpy(ctx->gamma_lut_persisted, end_lut, 768);
		ctx->fade_state.active = 0;
		dmc_record_gamma_change_with_lut_fade(end_lut, 0);
	} else {
		dmc_record_gamma_change_with_lut_fade(ctx->fade_state.start_lut, 1);
	}
}

int32_t DSpContext_FadeGammaInHandler(uint32_t ctxRef,
	                                  uint32_t zeroIntensityColor)
{
	if (!ctxRef) {
		QD3D_STATE_LOG("DSpFadeGammaIn(GL): ambient no-op color=0x%08x",
		               zeroIntensityColor);
		return kDSpNoErr;
	}
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpInvalidContextErr;
	uint8_t end[768];
	gamma_copy_driver(end);
	gamma_begin_fade(ctx, end, gamma_one_second_vbls());
	return kDSpNoErr;
}
int32_t DSpContext_FadeGammaOutHandler(uint32_t ctxRef,
	                                   uint32_t zeroIntensityColor)
{
	if (!ctxRef) {
		QD3D_STATE_LOG("DSpFadeGammaOut(GL): ambient no-op color=0x%08x",
		               zeroIntensityColor);
		return kDSpNoErr;
	}
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpInvalidContextErr;
	uint8_t r = 0, g = 0, b = 0;
	gamma_read_zero_color(zeroIntensityColor, &r, &g, &b);
	uint8_t full[768], end[768];
	gamma_copy_driver(full);
	gamma_compute_target(r, g, b, 0, full, end);
	gamma_begin_fade(ctx, end, gamma_one_second_vbls());
	return kDSpNoErr;
}
int32_t DSpContext_FadeGammaHandler(uint32_t ctxRef, int32_t percent,
	                                uint32_t zeroIntensityColor)
{
	DSpContextPrivate *ctx = ctxRef ? DSpGetContext(ctxRef) : nullptr;
	if (ctxRef && !ctx) return kDSpInvalidContextErr;
	uint8_t r = 0, g = 0, b = 0;
	gamma_read_zero_color(zeroIntensityColor, &r, &g, &b);
	uint8_t full[768], target[768];
	gamma_copy_driver(full);
	gamma_compute_target(r, g, b, percent, full, target);
	const int32_t rc = dmc_record_gamma_change_with_lut_fade(
	    target, percent == 100 ? 0 : 1);
	if (rc != 0) return kDSpInternalErr;
	if (ctx) {
		std::memcpy(ctx->gamma_lut_persisted, target, 768);
		ctx->fade_state.active = 0;
	}
	QD3D_STATE_LOG("DSpFadeGamma(GL): ctx=%u percent=%d tint=%u/%u/%u ambient=%d",
	               ctxRef, percent, r, g, b, ctxRef ? 0 : 1);
	return kDSpNoErr;
}
int32_t DSpContext_SetVBLProcHandler(uint32_t ctxRef, uint32_t procPtr, uint32_t refCon)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	ctx->vbl_proc_ptr = procPtr;
	ctx->vbl_proc_refcon = refCon;
	return kDSpNoErr;
}
int32_t DSpContext_GetVBLProcHandler(uint32_t ctxRef, uint32_t outProc, uint32_t outRef)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx) return kDSpContextNotFoundErr;
	if (outProc) WriteMacInt32(outProc, ctx->vbl_proc_ptr);
	if (outRef) WriteMacInt32(outRef, ctx->vbl_proc_refcon);
	return kDSpNoErr;
}
int32_t DSpProcessEventHandler(uint32_t, uint32_t o) {
	if (o) WriteMacInt32(o, 0); return kDSpNoErr;
}
void DSpVBLReleaseCallback(void*,void*,double) {}
void DSpVBLClutLatchCallback(void *, void *, double)
{
	/* Promote writer-visible CLUT into latched snapshot (Metal parity) */
	for (auto &kv : s_ctx) {
		DSpContextPrivate *ctx = kv.second;
		if (!ctx) continue;
		std::memcpy(ctx->clut_bytes_latched, ctx->clut_bytes, 768);
	}
}
void DSpVBLGammaFadeCallback(void *, void *, double)
{
	/* Advance animated gamma fades (Metal DSpVBLGammaFadeCallback parity). */
	uint32_t any_still = 0;
	for (auto &kv : s_ctx) {
		DSpContextPrivate *ctx = kv.second;
		if (!ctx || !ctx->fade_state.active) continue;
		uint32_t after = (uint32_t)ctx->fade_state.elapsed_vbls + 1u;
		if (after < (uint32_t)ctx->fade_state.duration_vbls)
			any_still = 1;
	}
	for (auto &kv : s_ctx) {
		DSpContextPrivate *ctx = kv.second;
		if (!ctx || !ctx->fade_state.active) continue;
		ctx->fade_state.elapsed_vbls++;
		if (ctx->fade_state.elapsed_vbls >= ctx->fade_state.duration_vbls) {
			std::memcpy(ctx->gamma_lut_persisted, ctx->fade_state.end_lut, 768);
			ctx->fade_state.active = 0;
			dmc_record_gamma_change_with_lut_fade(ctx->fade_state.end_lut, (int)any_still);
			continue;
		}
		uint8_t interp[768];
		gamma_interp(ctx->fade_state.start_lut, ctx->fade_state.end_lut,
		             ctx->fade_state.elapsed_vbls, ctx->fade_state.duration_vbls, interp);
		dmc_record_gamma_change_with_lut_fade(interp, 1);
	}
}
void DSpVBLServiceCallback(void *, void *, double)
{
	/* Publish latched palette for Active contexts + sync alt-buffer staging. */
	for (auto &kv : s_ctx) {
		DSpContextPrivate *ctx = kv.second;
		if (!ctx || ctx->state != (uint32_t)kDSpContextState_Active)
			continue;
		MetalCompositorUpdatePalette(ctx->clut_bytes_latched, 256);
		if (ctx->underlay_alt_buffer) {
			DSpAltBufferGL *rec = alt_get(ctx->underlay_alt_buffer);
			if (rec && rec->baseaddr_mac && rec->backing) {
				uint8 *src = Mac2HostAddr(rec->baseaddr_mac);
				if (src) std::memcpy(rec->backing, src, rec->baseaddr_size);
			}
		}
		break;
	}
}
void DSpVBLBackgroundForegroundDrain(void)
{
	/* Desktop has no iOS background queue — no-op is correct when no pending bits. */
}

/* ---- MainDevice PixMap redirect (Metal DSpRedirectMainDevicePixMap parity) ---- */
extern "C" void DSpRedirectMainDevicePixMap(DSpContextPrivate *ctx)
{
	if (!ctx || !ctx->back_buffer) return;
	const uint32_t display_depth =
	    DSpDisplayModeDepth(ctx->attr.backBufferBestDepth, ctx->attr.displayBestDepth);
	if (!DSpShouldRedirectMainDevicePixMap(ctx->attr.backBufferBestDepth, display_depth)) {
		DSpRestoreMainDevicePixMap(ctx);
		return;
	}
	const uint32_t redirect_depth =
	    DSpMainDevicePixMapDepth(ctx->attr.backBufferBestDepth, display_depth);
	auto inrange = [&](uint32_t a) -> bool {
		return (a < 0x3000u) || (a >= RAMBase && a < RAMBase + RAMSize);
	};
	uint32_t mainDeviceH = ReadMacInt32(LMADDR_MAIN_DEVICE);
	if (!mainDeviceH || !inrange(mainDeviceH)) return;
	uint32_t gdevicePtr = ReadMacInt32(mainDeviceH);
	if (!gdevicePtr || !inrange(gdevicePtr)) return;
	uint32_t pixMapH = ReadMacInt32(gdevicePtr + GDEVICE_OFF_PMAP);
	if (!pixMapH || !inrange(pixMapH)) return;
	uint32_t pixMapPtr = ReadMacInt32(pixMapH);
	if (!pixMapPtr || !inrange(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_CMPSIZE + 2)) return;

	if (DSpShouldCacheMainDevicePixMapOriginal(ctx->saved_pixmap_valid != 0,
	                                           ctx->saved_pixmap_addr, pixMapPtr)) {
		ctx->saved_pixmap_addr = pixMapPtr;
		ctx->saved_pixmap_baseAddr = ReadMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BASEADDR);
		ctx->saved_pixmap_rowBytes = (uint16_t)ReadMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_ROWBYTES);
		ctx->saved_pixmap_bounds[0] = (int16_t)ReadMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_TOP);
		ctx->saved_pixmap_bounds[1] = (int16_t)ReadMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_LEFT);
		ctx->saved_pixmap_bounds[2] = (int16_t)ReadMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_BOT);
		ctx->saved_pixmap_bounds[3] = (int16_t)ReadMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_RIGHT);
		ctx->saved_pixmap_pixelType = (uint16_t)ReadMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELTYPE);
		ctx->saved_pixmap_pixelSize = (uint16_t)ReadMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELSIZE);
		ctx->saved_pixmap_cmpCount = (uint16_t)ReadMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_CMPCOUNT);
		ctx->saved_pixmap_cmpSize = (uint16_t)ReadMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_CMPSIZE);
		ctx->saved_pixmap_pmVersion = (uint16_t)ReadMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PMVERSION);
		ctx->saved_pixmap_packType = (uint16_t)ReadMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PACKTYPE);
		ctx->saved_pixmap_packSize = ReadMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PACKSIZE);
		ctx->saved_pixmap_hRes = ReadMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_HRES);
		ctx->saved_pixmap_vRes = ReadMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_VRES);
		ctx->saved_pixmap_planeBytes = ReadMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PLANEBYTES);
		ctx->saved_pixmap_valid = 1;
	}

	/* Prefer a real display-depth front surface. When the depths match, the
	 * back staging has exactly the PixMap layout advertised to the guest. */
	uint32_t base = ctx->front_staging_mac_addr;
	if (!base && ctx->staging_mac_addr) base = ctx->staging_mac_addr;
	if (!base) return;
	const bool using_back_staging = base == ctx->staging_mac_addr;
	if (using_back_staging && redirect_depth != ctx->attr.backBufferBestDepth) {
		QD3D_STATE_LOG("DSpRedirectMainDevice(GL): refusing mixed-depth alias ctx=%u back=%u display=%u",
		               ctx->handle, ctx->attr.backBufferBestDepth, redirect_depth);
		return;
	}
	const uint32_t w = using_back_staging
	    ? DSpContextBackBufferWidth(ctx)
	    : (ctx->attr.displayWidth ? ctx->attr.displayWidth : 640);
	const uint32_t h = using_back_staging
	    ? DSpContextBackBufferHeight(ctx)
	    : (ctx->attr.displayHeight ? ctx->attr.displayHeight : 480);
	const uint32_t rb = using_back_staging
	    ? DSpBackBufferAlignedRowBytes(w, ctx->attr.backBufferBestDepth)
	    : DSpMainDevicePixMapRowBytes(w, ctx->attr.backBufferBestDepth,
	                                 display_depth);

	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BASEADDR, base);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_ROWBYTES,
	              DSpMainDevicePixMapRowBytesField(rb));
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_TOP, 0);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_LEFT, 0);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_BOT, (int16)h);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_RIGHT, (int16)w);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELTYPE, redirect_depth == 8 ? 0 : 16);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELSIZE, (uint16)redirect_depth);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_CMPCOUNT, redirect_depth == 8 ? 1 : 3);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_CMPSIZE,
	              redirect_depth == 8 ? 8 : (redirect_depth == 16 ? 5 : 8));
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PMVERSION, DSpMainDevicePixMapVersion());
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PACKTYPE, DSpMainDevicePixMapPackType());
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PACKSIZE, DSpMainDevicePixMapPackSize());
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_HRES, DSpMainDevicePixMapResolution());
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_VRES, DSpMainDevicePixMapResolution());
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PLANEBYTES, DSpMainDevicePixMapPlaneBytes());
	QD3D_STATE_LOG("DSpRedirectMainDevice(GL): ctx=%u pixmap=0x%08x base=0x%08x %ux%u@%u row=%u source=%s",
	               ctx->handle, pixMapPtr, base, w, h, redirect_depth, rb,
	               using_back_staging ? "back" : "front");
}

extern "C" void DSpRestoreMainDevicePixMap(DSpContextPrivate *ctx)
{
	if (!ctx || !ctx->saved_pixmap_valid || !ctx->saved_pixmap_addr) return;
	uint32_t pixMapPtr = ctx->saved_pixmap_addr;
	if (!(pixMapPtr < 0x3000u || (pixMapPtr >= RAMBase && pixMapPtr < RAMBase + RAMSize)))
		return;
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BASEADDR, ctx->saved_pixmap_baseAddr);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_ROWBYTES, ctx->saved_pixmap_rowBytes);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_TOP, (uint16)ctx->saved_pixmap_bounds[0]);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_LEFT, (uint16)ctx->saved_pixmap_bounds[1]);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_BOT, (uint16)ctx->saved_pixmap_bounds[2]);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_RIGHT, (uint16)ctx->saved_pixmap_bounds[3]);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELTYPE, ctx->saved_pixmap_pixelType);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELSIZE, ctx->saved_pixmap_pixelSize);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_CMPCOUNT, ctx->saved_pixmap_cmpCount);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_CMPSIZE, ctx->saved_pixmap_cmpSize);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PMVERSION, ctx->saved_pixmap_pmVersion);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PACKTYPE, ctx->saved_pixmap_packType);
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PACKSIZE, ctx->saved_pixmap_packSize);
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_HRES, ctx->saved_pixmap_hRes);
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_VRES, ctx->saved_pixmap_vRes);
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PLANEBYTES, ctx->saved_pixmap_planeBytes);
	ctx->saved_pixmap_valid = 0;
}

/* Host bridge — desktop: track Active fullscreen without iOS idle-timer */
static bool s_dsp_active_fullscreen = false;
extern "C" void DSpHostBridge_SetActiveFullscreen(bool active)
{
	s_dsp_active_fullscreen = active;
}
extern "C" bool DSpHostBridge_GetActiveFullscreen(void)
{
	return s_dsp_active_fullscreen;
}
extern "C" void DSpHostBridgeInit(void)
{
	s_dsp_active_fullscreen = false;
}
extern "C" void DSpHostBridgeShutdown(void)
{
	s_dsp_active_fullscreen = false;
}

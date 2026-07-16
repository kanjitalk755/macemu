/*
 *  rave_gl_renderer.cpp - RAVE 3D via OpenGL FBO overlay
 *
 *  Implements rave_metal_renderer.h using desktop OpenGL instead of Metal.
 *  Draws into a double-buffered overlay texture and submits it to the
 *  compositor each RenderEnd (same mailbox model as the Metal path).
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "rave_engine.h"
#include "rave_metal_renderer.h"
#include "rave_blend_policy.h"
#include "rave_depth_policy.h"
#include "rave_ati_tag_policy.h"
#include "rave_overlay_clear_policy.h"
#include "metal_compositor.h"
#include "gfxaccel_resources.h"
#include "display_mode_controller.h"
#include "gl_device.h"
#include "gl_ext.h"
#include "gfxaccel_backend.h"
#include "macos_util.h"
#include "qd3d_init_logging.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <SDL_opengl.h>

#ifndef RAVE_LOG
#define RAVE_LOG(fmt, ...) fprintf(stderr, "[rave-gl] " fmt "\n", ##__VA_ARGS__)
#endif
#define kQANoErr 0
#define kQANotSupported 3

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

static inline float ReadMacFloat(uint32 addr)
{
	uint32 bits = ReadMacInt32(addr);
	float f;
	std::memcpy(&f, &bits, sizeof(float));
	return f;
}

struct RaveMetalState {
	GLuint fbo = 0;
	GLuint color_tex = 0;
	GLuint depth_rb = 0;
	uint32_t w = 0, h = 0;
	bool pass_active = false;
	bool cleared = false;
	/* AccessDrawBuffer / AccessZBuffer CPU maps (guest-visible) */
	uint32_t draw_cpu_mac = 0;
	uint32_t draw_cpu_size = 0;
	uint32_t draw_cpu_row_bytes = 0;
	uint32_t draw_cpu_pixel_type = 3; /* kQAPixel_RGB32 */
	bool draw_accessed = false;
	uint32_t notice_device_mac = 0;
	uint32_t notice_dirty_rect_mac = 0;
	uint32_t z_cpu_mac = 0;
	uint32_t z_cpu_size = 0;
	bool z_accessed = false;
	/* Reused full-frame transfer storage. The notice callback runs every
	 * frame, so allocating and freeing two ~1.2 MB vectors here was visible
	 * in both Debug and Release profiles. */
	std::vector<uint8_t> readback_bgra;
	std::vector<uint8_t> upload_bgra;
	std::vector<uint32_t> rtt_handles;
	bool draw_state_valid = false;
	bool draw_state_textured = false;
	bool draw_state_multitexture = false;
	uint32_t draw_state_multitexture_handle = 0;
	uint32_t draw_state_multitexture_op = 0;
	float draw_state_multitexture_factor = 0.f;
	/* Per-frame diagnostics. Kept here so the trace can distinguish a frame
	 * that drew black from a frame whose draw calls were never accepted. */
	uint64_t draw_calls = 0;
	uint64_t vertices = 0;
	uint64_t textured_draws = 0;
	uint64_t texture_binds = 0;
	uint64_t missing_textures = 0;
	uint64_t dropped_draws = 0;
	uint64_t state_applies = 0;
	uint64_t state_cache_hits = 0;
	uint32_t logged_draws = 0;
};

/* Compatibility OpenGL state is context-global, not RaveDrawPrivate-local.
 * A per-draw-context cache is valid only while that context remains the last
 * owner to install fixed-function state. */
static RaveMetalState *s_draw_state_owner = nullptr;

static void invalidate_draw_state(RaveMetalState *ms)
{
	assert(ms != nullptr);
	ms->draw_state_valid = false;
	s_draw_state_owner = nullptr;
}

/* Overlay fleet */
static GLuint s_overlay_pair[2] = {0, 0};
static GLuint s_overlay_tex = 0;
static uint32_t s_ow = 0, s_oh = 0, s_write = 0;
static int32_t s_dst_l = 0, s_dst_t = 0, s_dst_w = 0, s_dst_h = 0;
static GLuint s_last_submitted_tex = 0;

static bool trace_sample(uint64_t count, uint64_t first = 8, uint64_t every = 120)
{
	return count <= first || (count != 0 && (count & (count - 1)) == 0) ||
	       (every != 0 && (count % every) == 0);
}

static bool trace_frame(const RaveDrawPrivate *priv)
{
	return priv && trace_sample(priv->frameCount);
}

static void fire_notice_method(RaveDrawPrivate *priv, uint32_t selector);

#if QD3D_INIT_LOGGING_ENABLED
static void trace_overlay_readback(const RaveDrawPrivate *priv,
	                               const RaveMetalState *ms,
	                               const char *stage)
{
	if (!priv || !ms || !trace_frame(priv) || !ms->w || !ms->h) return;
	const uint64_t byte_count = (uint64_t)ms->w * ms->h * 4u;
	if (byte_count > 64u * 1024u * 1024u) return;
	std::vector<uint8_t> pixels((size_t)byte_count);
	GLint old_pack = 4;
	glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, (GLsizei)ms->w, (GLsizei)ms->h,
	             GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
	const GLenum read_error = glGetError();

	uint64_t nonblack = 0, alpha_zero = 0, alpha_full = 0;
	uint64_t r_sum = 0, g_sum = 0, b_sum = 0, a_sum = 0;
	uint8_t r_max = 0, g_max = 0, b_max = 0, a_max = 0;
	for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
		const uint8_t r = pixels[i + 0], g = pixels[i + 1];
		const uint8_t b = pixels[i + 2], a = pixels[i + 3];
		r_sum += r; g_sum += g; b_sum += b; a_sum += a;
		r_max = std::max(r_max, r); g_max = std::max(g_max, g);
		b_max = std::max(b_max, b); a_max = std::max(a_max, a);
		if (r || g || b) nonblack++;
		if (!a) alpha_zero++;
		if (a == 255) alpha_full++;
	}
	QD3D_RENDER_LOG("RAVEOverlayPixels frame=%u stage=%s size=%ux%u draws=%llu textured=%llu nonblack=%llu alphaZero=%llu alphaFull=%llu sums=%llu/%llu/%llu/%llu max=%u/%u/%u/%u readError=0x%x",
	                priv->frameCount, stage ? stage : "unknown", ms->w, ms->h,
	                (unsigned long long)ms->draw_calls,
	                (unsigned long long)ms->textured_draws,
	                (unsigned long long)nonblack,
	                (unsigned long long)alpha_zero,
	                (unsigned long long)alpha_full,
	                (unsigned long long)r_sum,
	                (unsigned long long)g_sum,
	                (unsigned long long)b_sum,
	                (unsigned long long)a_sum,
	                (unsigned)r_max, (unsigned)g_max, (unsigned)b_max,
	                (unsigned)a_max, (unsigned)read_error);
}
#endif

extern RaveDrawPrivate *RaveGetContext(uint32 handle);

static RaveDrawPrivate *GetContextFromDrawAddr(uint32 drawContextAddr)
{
	if (!drawContextAddr) return nullptr;
	uint32 handle = ReadMacInt32(drawContextAddr + 0);
	return RaveGetContext(handle);
}

static void release_overlay(void)
{
	// The compositor mailbox may still point at the last submitted texture.
	// Drop that reference before returning either texture to the resource pool.
	MetalCompositorSubmitFrame_ClearCachedOverlay();
	for (int i = 0; i < 2; i++) {
		if (s_overlay_pair[i]) {
			gfxaccel_resources_release_overlay_texture(kGfxEngineRAVE,
				(void *)(uintptr_t)s_overlay_pair[i]);
			s_overlay_pair[i] = 0;
		}
	}
	s_overlay_tex = 0;
	s_last_submitted_tex = 0;
	s_ow = s_oh = 0;
}

static GLuint acquire_overlay(uint32_t w, uint32_t h)
{
	if ((s_overlay_pair[0] || s_overlay_pair[1]) && (s_ow != w || s_oh != h))
		release_overlay();
	if (!s_overlay_pair[0] || !s_overlay_pair[1]) {
		void *a = gfxaccel_resources_vend_overlay_texture_indexed(
			kGfxEngineRAVE, 0, w, h, MTLPixelFormatBGRA8Unorm);
		void *b = gfxaccel_resources_vend_overlay_texture_indexed(
			kGfxEngineRAVE, 1, w, h, MTLPixelFormatBGRA8Unorm);
		if (!a || !b) {
			if (a) gfxaccel_resources_release_overlay_texture(kGfxEngineRAVE, a);
			if (b) gfxaccel_resources_release_overlay_texture(kGfxEngineRAVE, b);
			return 0;
		}
		s_overlay_pair[0] = (GLuint)(uintptr_t)a;
		s_overlay_pair[1] = (GLuint)(uintptr_t)b;
		s_ow = w; s_oh = h;
	}
	s_overlay_tex = s_overlay_pair[s_write];
	return s_overlay_tex;
}

void RaveCreateMetalOverlay(int32_t left, int32_t top, int32_t width, int32_t height)
{
	QD3D_INIT_LOG("RaveCreateMetalOverlay(GL): destination=(%d,%d) size=%dx%d",
	              left, top, width, height);
	s_dst_l = left; s_dst_t = top; s_dst_w = width; s_dst_h = height;
	if (width > 0 && height > 0)
		acquire_overlay((uint32_t)width, (uint32_t)height);
	if (s_overlay_tex)
		(void)dmc_set_active_owner(kDMCOwnerRAVE);
	QD3D_INIT_LOG("RaveCreateMetalOverlay(GL): texture=%u pair=(%u,%u) allocated=%ux%u",
	              (unsigned)s_overlay_tex, (unsigned)s_overlay_pair[0],
	              (unsigned)s_overlay_pair[1], s_ow, s_oh);
}

extern "C" int rave_has_active_overlay(void)
{
	return s_overlay_tex != 0 || s_dst_w > 0;
}
extern "C" int rave_get_overlay_dims(uint32_t *outW, uint32_t *outH)
{
	if (outW) *outW = s_ow ? s_ow : (uint32_t)s_dst_w;
	if (outH) *outH = s_oh ? s_oh : (uint32_t)s_dst_h;
	return (s_dst_w > 0 && s_dst_h > 0) ? 1 : 0;
}
extern "C" void rave_release_overlay_for_detach(void)
{
	release_overlay();
}

void RaveInitMetalResources(RaveDrawPrivate *priv)
{
	QD3D_INIT_LOG("RaveInitMetalResources(GL): priv=%p", (void *)priv);
	if (!priv) {
		QD3D_INIT_LOG("RaveInitMetalResources(GL): rejected null context");
		return;
	}
	if (!GfxGLDeviceInit() || !GfxGLDeviceMakeCurrent()) {
		QD3D_INIT_LOG("RaveInitMetalResources(GL): GL device/current failed; returning without native state");
		RAVE_LOG("RaveInitMetalResources: GL device failed");
		return;
	}
	if (priv->metal) {
		delete priv->metal;
		priv->metal = nullptr;
	}
	auto *ms = new RaveMetalState();
	priv->metal = ms;
	QD3D_INIT_LOG("RaveInitMetalResources(GL): allocated state=%p", (void *)ms);
	if (priv->width > 0 && priv->height > 0)
		RaveCreateMetalOverlay(priv->left, priv->top, priv->width, priv->height);
	RAVE_LOG("RaveInitMetalResources ok %dx%d", priv->width, priv->height);
	QD3D_INIT_LOG("RaveInitMetalResources(GL): success size=%dx%d overlay=%u",
	              priv->width, priv->height, (unsigned)s_overlay_tex);
}

void RaveReleaseMetalResources(RaveDrawPrivate *priv)
{
	if (!priv || !priv->metal) return;
	if (s_draw_state_owner == priv->metal)
		s_draw_state_owner = nullptr;
	if (GfxGLDeviceMakeCurrent()) {
		RaveMetalState *ms = priv->metal;
		auto &ext = gfx_gl_ext();
		if (ext.fbo) {
			if (ms->fbo) ext.DeleteFramebuffers(1, &ms->fbo);
			if (ms->depth_rb) ext.DeleteRenderbuffers(1, &ms->depth_rb);
		}
		delete ms;
	} else {
		delete priv->metal;
	}
	priv->metal = nullptr;
}

static bool bind_overlay_fbo(RaveMetalState *ms, uint32_t w, uint32_t h)
{
	assert(ms != nullptr);
	if (!GfxGLDeviceMakeCurrent()) {
		QD3D_RENDER_LOG("bind_overlay_fbo: MakeCurrent failed");
		return false;
	}
	auto &ext = gfx_gl_ext();
	if (!ext.fbo) {
		QD3D_RENDER_LOG("bind_overlay_fbo: framebuffer objects unavailable");
		RAVE_LOG("No FBO support on this GL context");
		return false;
	}
	GLuint tex = acquire_overlay(w, h);
	if (!tex) {
		QD3D_RENDER_LOG("bind_overlay_fbo: overlay allocation failed for %ux%u", w, h);
		return false;
	}
	ms->color_tex = tex;
	ms->w = w; ms->h = h;

	if (!ms->fbo) {
		ext.GenFramebuffers(1, &ms->fbo);
		ext.GenRenderbuffers(1, &ms->depth_rb);
	}
	ext.BindFramebuffer(GL_FRAMEBUFFER, ms->fbo);
	ext.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	ext.BindRenderbuffer(GL_RENDERBUFFER, ms->depth_rb);
	ext.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)w, (GLsizei)h);
	ext.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, ms->depth_rb);
	GLenum st = ext.CheckFramebufferStatus(GL_FRAMEBUFFER);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		QD3D_RENDER_LOG("bind_overlay_fbo: incomplete status=0x%x fbo=%u color=%u depth=%u size=%ux%u",
		                (unsigned)st, (unsigned)ms->fbo, (unsigned)tex,
		                (unsigned)ms->depth_rb, w, h);
		RAVE_LOG("FBO incomplete 0x%x", (unsigned)st);
		ext.BindFramebuffer(GL_FRAMEBUFFER, 0);
		return false;
	}
	glViewport(0, 0, (GLsizei)w, (GLsizei)h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	/* RAVE screen space: origin top-left, Y down */
	/* RAVE submits window-space depth in [0,1]. OpenGL clips against [-1,1],
	 * so near=0/far=-1 produces clipZ=2*z-1 and preserves RAVE depth. The
	 * old (1,0) pair produced clipZ=2*z+1, clipping every vertex with z>0. */
	glOrtho(0, (GLdouble)w, (GLdouble)h, 0, 0, -1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.0f);
	invalidate_draw_state(ms);
	return true;
}

static void unbind_fbo(void)
{
	auto &ext = gfx_gl_ext();
	if (ext.fbo)
		ext.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

static bool restore_overlay_fbo(RaveMetalState *ms)
{
	assert(ms != nullptr);
	if (!ms->fbo || !ms->color_tex || !GfxGLDeviceMakeCurrent())
		return false;
	auto &ext = gfx_gl_ext();
	if (!ext.fbo) return false;
	ext.BindFramebuffer(GL_FRAMEBUFFER, ms->fbo);
	ext.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                         GL_TEXTURE_2D, ms->color_tex, 0);
	if (ext.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return false;
	glViewport(0, 0, (GLsizei)ms->w, (GLsizei)ms->h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, (GLdouble)ms->w, (GLdouble)ms->h, 0, 0, -1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	invalidate_draw_state(ms);
	return true;
}

enum {
	kRaveNoticeDeviceMemory = 0,
	kRaveNoticePixelRGB16 = 1,
	kRaveNoticePixelRGB32 = 3
};

static uint32_t notice_pixel_type(const RaveDrawPrivate *priv)
{
	/* The image-buffer callback is part of the display-device contract. Some
	 * classic clients (notably Descent II 6500 OEM) draw according to the
	 * active GDevice depth even though they are also given pixelType. Follow
	 * the context's live device instead of always exposing the Metal backend's
	 * RGB32 staging format. Fall back to DMC for malformed devices. */
	if (priv && priv->deviceAddr) {
		const uint32_t pixel_type = RaveDeviceDrawBufferPixelType(priv->deviceAddr);
		if (pixel_type == kRaveNoticePixelRGB16 ||
		    pixel_type == kRaveNoticePixelRGB32)
			return pixel_type;
	}
	const DMCModeSnapshot *snap = dmc_current_snapshot();
	return snap && snap->depth == 16 ? kRaveNoticePixelRGB16
	                                 : kRaveNoticePixelRGB32;
}

static uint32_t notice_bytes_per_pixel(uint32_t pixel_type)
{
	assert(pixel_type == kRaveNoticePixelRGB16 ||
	       pixel_type == kRaveNoticePixelRGB32);
	return pixel_type == kRaveNoticePixelRGB16 ? 2u : 4u;
}

static bool ensure_draw_buffer_cpu(RaveMetalState *ms, uint32_t pixel_type)
{
	if (!ms || !ms->w || !ms->h) return false;
	const uint32_t bytes_per_pixel = notice_bytes_per_pixel(pixel_type);
	const uint64_t row_bytes64 = (uint64_t)ms->w * bytes_per_pixel;
	const uint64_t size64 = row_bytes64 * ms->h;
	if (size64 > UINT32_MAX) return false;
	const uint32_t size = (uint32_t)size64;
	if (!ms->draw_cpu_mac) {
		/* Reserve the largest supported representation once. A live GDevice
		 * can change between 16 and 32 bits while the context survives; repeated
		 * Mac_sysalloc calls cannot be reclaimed individually here. */
		const uint64_t capacity64 = (uint64_t)ms->w * ms->h * 4u;
		if (capacity64 > UINT32_MAX) return false;
		const uint32_t mac = Mac_sysalloc((uint32_t)capacity64);
		if (!mac || !Mac2HostAddr(mac)) return false;
		ms->draw_cpu_mac = mac;
	}
	ms->draw_cpu_size = size;
	ms->draw_cpu_row_bytes = (uint32_t)row_bytes64;
	ms->draw_cpu_pixel_type = pixel_type;
	return true;
}

/* GL render targets are bottom-up. RAVE's CPU image-buffer contract is a
 * top-down big-endian RGB16 or RGB32 byte stream matching the display. */
static bool copy_overlay_to_guest(const RaveDrawPrivate *priv,
	                              RaveMetalState *ms)
{
	const uint32_t pixel_type = notice_pixel_type(priv);
	if (!ensure_draw_buffer_cpu(ms, pixel_type) || !GfxGLDeviceMakeCurrent())
		return false;
	auto &ext = gfx_gl_ext();
	if (!ext.fbo || !ms->fbo) return false;
	ext.BindFramebuffer(GL_FRAMEBUFFER, ms->fbo);
	const uint32_t w = ms->w, h = ms->h;
	ms->readback_bgra.resize((size_t)w * h * 4u);
	GLint old_pack = 4;
	glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
	while (glGetError() != GL_NO_ERROR) {}
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, (GLsizei)w, (GLsizei)h,
	             GL_BGRA, GL_UNSIGNED_BYTE, ms->readback_bgra.data());
	glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
	if (glGetError() != GL_NO_ERROR) return false;
	uint8_t *guest = Mac2HostAddr(ms->draw_cpu_mac);
	if (!guest) return false;
	for (uint32_t guest_y = 0; guest_y < h; guest_y++) {
		const uint8_t *src = ms->readback_bgra.data() +
		                     (size_t)(h - 1u - guest_y) * w * 4u;
		uint8_t *dst = guest + (size_t)guest_y * ms->draw_cpu_row_bytes;
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t b = src[x * 4u + 0u];
			const uint8_t g = src[x * 4u + 1u];
			const uint8_t r = src[x * 4u + 2u];
			const uint8_t a = src[x * 4u + 3u];
			if (pixel_type == kRaveNoticePixelRGB16) {
				const uint16_t rgb555 = (uint16_t)(((uint16_t)(r >> 3) << 10) |
				                                   ((uint16_t)(g >> 3) << 5) |
				                                   (uint16_t)(b >> 3));
				dst[x * 2u + 0u] = (uint8_t)(rgb555 >> 8);
				dst[x * 2u + 1u] = (uint8_t)rgb555;
			} else {
				dst[x * 4u + 0u] = a;
				dst[x * 4u + 1u] = r;
				dst[x * 4u + 2u] = g;
				dst[x * 4u + 3u] = b;
			}
		}
	}
	return true;
}

static bool upload_guest_to_overlay(RaveMetalState *ms, uint32_t rect_addr)
{
	if (!ms || !ms->color_tex || !ms->draw_cpu_mac ||
	    !GfxGLDeviceMakeCurrent()) return false;
	int32_t left = 0, right = (int32_t)ms->w;
	int32_t top = 0, bottom = (int32_t)ms->h;
	if (rect_addr) {
		left = (int32_t)ReadMacInt32(rect_addr + 0u);
		right = (int32_t)ReadMacInt32(rect_addr + 4u);
		top = (int32_t)ReadMacInt32(rect_addr + 8u);
		bottom = (int32_t)ReadMacInt32(rect_addr + 12u);
	}
	left = std::max<int32_t>(0, std::min<int32_t>(left, (int32_t)ms->w));
	right = std::max<int32_t>(0, std::min<int32_t>(right, (int32_t)ms->w));
	top = std::max<int32_t>(0, std::min<int32_t>(top, (int32_t)ms->h));
	bottom = std::max<int32_t>(0, std::min<int32_t>(bottom, (int32_t)ms->h));
	if (right <= left || bottom <= top) return restore_overlay_fbo(ms);

	const uint32_t upload_w = (uint32_t)(right - left);
	const uint32_t upload_h = (uint32_t)(bottom - top);
	const uint32_t pixel_type = ms->draw_cpu_pixel_type;
	const uint32_t bytes_per_pixel = notice_bytes_per_pixel(pixel_type);
	ms->upload_bgra.resize((size_t)upload_w * upload_h * 4u);
	const uint8_t *guest = Mac2HostAddr(ms->draw_cpu_mac);
	if (!guest) return false;
	for (uint32_t gl_row = 0; gl_row < upload_h; gl_row++) {
		const uint32_t guest_y = (uint32_t)bottom - 1u - gl_row;
		const uint8_t *src = guest + (size_t)guest_y * ms->draw_cpu_row_bytes +
		                     (size_t)(uint32_t)left * bytes_per_pixel;
		uint8_t *dst = ms->upload_bgra.data() +
		               (size_t)gl_row * upload_w * 4u;
		for (uint32_t x = 0; x < upload_w; x++) {
			uint8_t a = 255, r, g, b;
			if (pixel_type == kRaveNoticePixelRGB16) {
				const uint16_t rgb555 = (uint16_t)(((uint16_t)src[x * 2u] << 8) |
				                                   src[x * 2u + 1u]);
				const uint8_t r5 = (uint8_t)((rgb555 >> 10) & 0x1f);
				const uint8_t g5 = (uint8_t)((rgb555 >> 5) & 0x1f);
				const uint8_t b5 = (uint8_t)(rgb555 & 0x1f);
				r = (uint8_t)((r5 << 3) | (r5 >> 2));
				g = (uint8_t)((g5 << 3) | (g5 >> 2));
				b = (uint8_t)((b5 << 3) | (b5 >> 2));
			} else {
				a = src[x * 4u + 0u];
				r = src[x * 4u + 1u];
				g = src[x * 4u + 2u];
				b = src[x * 4u + 3u];
			}
			dst[x * 4u + 0u] = b;
			dst[x * 4u + 1u] = g;
			dst[x * 4u + 2u] = r;
			dst[x * 4u + 3u] = a;
		}
	}

	unbind_fbo();
	glBindTexture(GL_TEXTURE_2D, ms->color_tex);
	GLint old_unpack = 4;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_unpack);
	while (glGetError() != GL_NO_ERROR) {}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, left, (GLint)ms->h - bottom,
	                (GLsizei)upload_w, (GLsizei)upload_h,
	                GL_BGRA, GL_UNSIGNED_BYTE, ms->upload_bgra.data());
	glPixelStorei(GL_UNPACK_ALIGNMENT, old_unpack);
	const bool uploaded = glGetError() == GL_NO_ERROR;
	return restore_overlay_fbo(ms) && uploaded;
}

static void fire_notice_method(RaveDrawPrivate *priv, uint32_t selector)
{
	if (!priv || !priv->metal || selector >= RAVE_NUM_NOTICE_METHODS) return;
	const uint32_t callback = priv->noticeMethods[selector].callback;
	if (!callback) return;
	RaveMetalState *ms = priv->metal;
	const uint32_t refcon = priv->noticeMethods[selector].refCon;
	static uint64_t notice_counts[RAVE_NUM_NOTICE_METHODS] = {};
	const uint64_t notice_count = ++notice_counts[selector];
	const bool log_notice = trace_sample(notice_count);
	if (selector == 3u || selector == 4u) {
		if (!copy_overlay_to_guest(priv, ms)) {
			QD3D_RENDER_LOG("Notice selector=%u callback=0x%08x: overlay readback failed",
			                selector, callback);
			return;
		}
		if (!ms->notice_device_mac)
			ms->notice_device_mac = Mac_sysalloc(24);
		if (!ms->notice_dirty_rect_mac)
			ms->notice_dirty_rect_mac = Mac_sysalloc(16);
		if (!ms->notice_device_mac || !ms->notice_dirty_rect_mac) return;
		WriteMacInt32(ms->notice_device_mac + 0u, kRaveNoticeDeviceMemory);
		WriteMacInt32(ms->notice_device_mac + 4u, ms->draw_cpu_row_bytes);
		WriteMacInt32(ms->notice_device_mac + 8u, ms->draw_cpu_pixel_type);
		WriteMacInt32(ms->notice_device_mac + 12u, ms->w);
		WriteMacInt32(ms->notice_device_mac + 16u, ms->h);
		WriteMacInt32(ms->notice_device_mac + 20u, ms->draw_cpu_mac);
		WriteMacInt32(ms->notice_dirty_rect_mac + 0u, 0);
		WriteMacInt32(ms->notice_dirty_rect_mac + 4u, ms->w);
		WriteMacInt32(ms->notice_dirty_rect_mac + 8u, 0);
		WriteMacInt32(ms->notice_dirty_rect_mac + 12u, ms->h);

		ms->pass_active = false;
		unbind_fbo();
		call_macos4(callback, priv->drawContextAddr, ms->notice_device_mac,
		            ms->notice_dirty_rect_mac, refcon);
		const bool uploaded = upload_guest_to_overlay(
		    ms, ms->notice_dirty_rect_mac);
		const bool restored = uploaded || restore_overlay_fbo(ms);
		ms->pass_active = restored;
		if (log_notice || !uploaded) {
			QD3D_RENDER_LOG("Notice selector=%u count=%llu callback=0x%08x refCon=0x%08x buffer=0x%08x pixelType=%u rowBytes=%u dirty=0x%08x upload=%d",
			                selector, (unsigned long long)notice_count, callback,
			                refcon, ms->draw_cpu_mac,
			                ms->draw_cpu_pixel_type, ms->draw_cpu_row_bytes,
			                ms->notice_dirty_rect_mac, uploaded ? 1 : 0);
		}
	} else {
		call_macos2(callback, priv->drawContextAddr, refcon);
		if (log_notice) {
			QD3D_RENDER_LOG("Notice selector=%u count=%llu callback=0x%08x refCon=0x%08x",
			                selector, (unsigned long long)notice_count,
			                callback, refcon);
		}
	}
}

/* ---- GL state from RAVE tags ---- */

static void apply_blend(RaveDrawPrivate *priv)
{
	assert(priv != nullptr);
	int blend = (int)priv->state[9].i; /* kQATag_Blend */
	auto &ext = gfx_gl_ext();
	if (blend == 2) {
		/* OpenGL blend factors — map common GL enums if present */
		uint32_t src = priv->state[109].i;
		uint32_t dst = priv->state[110].i;
		auto map = [](uint32_t f) -> GLenum {
			switch (f) {
			case 0: return GL_ZERO;
			case 1: return GL_ONE;
			case 0x0300: return GL_SRC_COLOR;
			case 0x0301: return GL_ONE_MINUS_SRC_COLOR;
			case 0x0302: return GL_SRC_ALPHA;
			case 0x0303: return GL_ONE_MINUS_SRC_ALPHA;
			case 0x0304: return GL_DST_ALPHA;
			case 0x0305: return GL_ONE_MINUS_DST_ALPHA;
			case 0x0306: return GL_DST_COLOR;
			case 0x0307: return GL_ONE_MINUS_DST_COLOR;
			default: return GL_SRC_ALPHA;
			}
		};
		glEnable(GL_BLEND);
		if (ext.BlendFuncSeparate)
			ext.BlendFuncSeparate(map(src), map(dst), GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		else
			glBlendFunc(map(src), map(dst));
	} else {
		/* The Metal path premultiplies mode-0 shader output then blends with
		 * ONE. Fixed-function GL emits straight color, so SRC_ALPHA produces
		 * the same RGB. Separate alpha factors preserve the overlay's alpha for
		 * the compositor instead of accidentally squaring it. */
		glEnable(GL_BLEND);
		if (ext.BlendFuncSeparate)
			ext.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
			                      GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		else
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
}

static void apply_depth(RaveDrawPrivate *priv)
{
	assert(priv != nullptr);
	if (!RaveContextUsesMetalDepthAttachment(priv->flags)) {
		glDisable(GL_DEPTH_TEST);
		glDepthMask(GL_FALSE);
		return;
	}
	int zfunc = (int)priv->state[0].i; /* kQATag_ZFunction */
	bool zwrite = zfunc != 0 && RaveDrawDepthWriteEnabledForBlendFactors(
		priv->state[28].i,
		priv->ati_state[kRaveATIDepthWriteEnableIndex].i,
		(int)priv->state[9].i, priv->state[109].i, priv->state[110].i);
	glEnable(GL_DEPTH_TEST);
	static const GLenum zmap[] = {
		GL_ALWAYS, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER,
		GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS, GL_NEVER
	};
	if (zfunc >= 0 && zfunc < 9)
		glDepthFunc(zmap[zfunc]);
	else
		glDepthFunc(GL_LEQUAL);
	glDepthMask(zwrite ? GL_TRUE : GL_FALSE);
}

static void apply_alpha_test(RaveDrawPrivate *priv)
{
	assert(priv != nullptr);
	int func = (int)priv->state[31].i;
	float ref = priv->state[46].f;
	if (func == 0 || func == 7) {
		glDisable(GL_ALPHA_TEST);
		return;
	}
	glEnable(GL_ALPHA_TEST);
	static const GLenum amap[] = {
		GL_ALWAYS, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER,
		GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS
	};
	if (func >= 0 && func < 8)
		glAlphaFunc(amap[func], ref);
}

static void configure_bound_texture(RaveDrawPrivate *priv,
	                                const RaveResourceEntry *entry)
{
	assert(priv != nullptr);
	assert(entry != nullptr);
	const int standardFilter = (int)priv->state[11].i;
	const bool glSamplerOverride = priv->state[101].i != 0 ||
	                               priv->state[102].i != 0 ||
	                               priv->state[103].i != 0 ||
	                               priv->state[104].i != 0;
	GLenum mag = glSamplerOverride
		? (priv->state[103].i == 1 ? GL_LINEAR : GL_NEAREST)
		: (standardFilter >= 1 ? GL_LINEAR : GL_NEAREST);
	GLenum minf;
	if (glSamplerOverride) {
		minf = priv->state[104].i == 1 ? GL_LINEAR : GL_NEAREST;
	} else if (standardFilter >= 2) {
		/* A mip minification mode on a one-level texture makes the texture
		 * incomplete. Desktop OpenGL then returns (0,0,0,1), which presents
		 * exactly like the all-black world reported by Descent II. */
		minf = entry->mip_levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
	} else {
		minf = standardFilter >= 1 ? GL_LINEAR : GL_NEAREST;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);

	const bool shrink = (priv->state[12].i & 8) != 0;
	const uint32_t wrapU = priv->state[101].i;
	const uint32_t wrapV = priv->state[102].i;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		(shrink || wrapU == 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		(shrink || wrapV == 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
}

static GLuint bind_current_texture(RaveDrawPrivate *priv)
{
	assert(priv != nullptr);
	assert(priv->metal != nullptr);
	uint32_t tex_mac = priv->state[13].i; /* kQATag_Texture */
	if (!tex_mac) {
#if QD3D_INIT_LOGGING_ENABLED
		priv->metal->missing_textures++;
#endif
		glDisable(GL_TEXTURE_2D);
		return 0;
	}
	uint32_t handle = RaveResourceFindByAddr(tex_mac);
	RaveResourceEntry *entry = RaveResourceGet(handle);
	if (!entry) {
#if QD3D_INIT_LOGGING_ENABLED
		uint64_t missing = ++priv->metal->missing_textures;
		if (trace_sample(missing, 8, 256))
			QD3D_RENDER_LOG("texture bind rejected: guest=0x%08x has no resource (missing=%llu)",
			                tex_mac, (unsigned long long)missing);
#endif
		glDisable(GL_TEXTURE_2D);
		return 0;
	}
	if (!entry->metal_texture && entry->pixmap_mac_addr != 0)
		RaveRealizeDeferredTexture(entry);
	else if (RaveTextureNeedsLivePixmapRefresh(entry))
		RaveRefreshTextureFromPixmap(entry);

	if (!entry->metal_texture) {
#if QD3D_INIT_LOGGING_ENABLED
		uint64_t missing = ++priv->metal->missing_textures;
		if (trace_sample(missing, 8, 256)) {
			QD3D_RENDER_LOG("texture bind unrealized: guest=0x%08x handle=%u type=%u size=%ux%u pixelType=%u (missing=%llu)",
			                tex_mac, handle, (unsigned)entry->type, entry->width,
			                entry->height, entry->pixel_type,
			                (unsigned long long)missing);
		}
#endif
		glDisable(GL_TEXTURE_2D);
		return 0;
	}
	GLuint tex = (GLuint)(uintptr_t)entry->metal_texture;
#if QD3D_INIT_LOGGING_ENABLED
	priv->metal->texture_binds++;
	if (priv->metal->texture_binds <= 8 && trace_frame(priv)) {
			QD3D_RENDER_LOG("frame=%u textureBind=%llu guest=0x%08x handle=%u gl=%u size=%ux%u mips=%u pixelType=%u filter=%u op=0x%x rgbNonzero=%u alphaZero=%u",
			                priv->frameCount,
			                (unsigned long long)priv->metal->texture_binds,
			                tex_mac, handle, (unsigned)tex, entry->width, entry->height,
			                entry->mip_levels, entry->pixel_type, priv->state[11].i,
			                priv->state[12].i, entry->diag_rgb_nonzero,
			                entry->diag_alpha_zero);
	}
#endif
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tex);
	configure_bound_texture(priv, entry);

	/* Texture env from TextureOp (state[12]) */
	int top = (int)priv->state[12].i;
	if (top & 16) { /* GL-style Blend with TextureEnvColor */
		GLfloat env[4] = {
			priv->state[151].f, priv->state[152].f,
			priv->state[153].f, priv->state[150].f
		};
		glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
	} else if (top & 4) /* Decal */
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
	else if (top & 1) /* Modulate */
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	else
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	return tex;
}

static GLuint bind_texture_unit(RaveDrawPrivate *priv, uint32_t tex_mac, int unit)
{
	assert(priv != nullptr);
	assert(priv->metal != nullptr);
	auto &ext = gfx_gl_ext();
	if (ext.multitex && ext.ActiveTexture)
		ext.ActiveTexture(GL_TEXTURE0 + unit);
	if (!tex_mac) {
#if QD3D_INIT_LOGGING_ENABLED
		priv->metal->missing_textures++;
#endif
		glDisable(GL_TEXTURE_2D);
		return 0;
	}
	uint32_t handle = RaveResourceFindByAddr(tex_mac);
	RaveResourceEntry *entry = RaveResourceGet(handle);
	if (!entry) {
#if QD3D_INIT_LOGGING_ENABLED
		priv->metal->missing_textures++;
#endif
		glDisable(GL_TEXTURE_2D);
		return 0;
	}
	if (!entry->metal_texture && entry->pixmap_mac_addr != 0)
		RaveRealizeDeferredTexture(entry);
	else if (RaveTextureNeedsLivePixmapRefresh(entry))
		RaveRefreshTextureFromPixmap(entry);
	if (!entry->metal_texture) {
#if QD3D_INIT_LOGGING_ENABLED
		priv->metal->missing_textures++;
#endif
		glDisable(GL_TEXTURE_2D);
		return 0;
	}
	GLuint tex = (GLuint)(uintptr_t)entry->metal_texture;
#if QD3D_INIT_LOGGING_ENABLED
	priv->metal->texture_binds++;
#endif
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tex);
	configure_bound_texture(priv, entry);
	return tex;
}

/* ---- Vertices (defined before z-sort / emit helpers) ---- */

struct HostV {
	float x, y, z, r, g, b, a, invW;
	/* Perspective-correct texture: s = u_ow / invW after GL divide (TexCoord4).
	 * v_ow is already V-flipped (invW - vOverW) matching Metal multitex staging. */
	float u_ow, v_ow;
	float kd_r, kd_g, kd_b;
	float ks_r, ks_g, ks_b;
	/* Multi-texture unit 1: same overW layout + separate invW2 */
	float u2_ow, v2_ow, invW2;
};

static bool accept_draw(RaveDrawPrivate *priv, const char *kind, uint32_t vertices)
{
	if (!priv || !priv->metal) return false;
	RaveMetalState *ms = priv->metal;
	if (!ms->pass_active) {
#if QD3D_INIT_LOGGING_ENABLED
		ms->dropped_draws++;
		if (trace_sample(ms->dropped_draws, 8, 256)) {
			QD3D_RENDER_LOG("DROP %s: frame=%u vertices=%u render pass inactive totalDropped=%llu",
			                kind, priv->frameCount, vertices,
			                (unsigned long long)ms->dropped_draws);
		}
#endif
		return false;
	}
	return true;
}

static void record_draw(RaveDrawPrivate *priv, const char *kind, uint32_t vertices,
	                    bool textured, const HostV *first)
{
#if QD3D_INIT_LOGGING_ENABLED
	if (!priv || !priv->metal) return;
	RaveMetalState *ms = priv->metal;
	ms->draw_calls++;
	ms->vertices += vertices;
	if (textured) ms->textured_draws++;
	if (trace_frame(priv) && ms->logged_draws < 8) {
		ms->logged_draws++;
		if (first) {
			QD3D_RENDER_LOG("frame=%u draw=%llu kind=%s vertices=%u textured=%d first[x/y/z/w]=%.3f/%.3f/%.5f/%.5f rgba=%.3f/%.3f/%.3f/%.3f uvOverW=%.5f/%.5f kd=%.3f/%.3f/%.3f texture=0x%08x op=0x%x",
			                priv->frameCount, (unsigned long long)ms->draw_calls,
			                kind, vertices, textured ? 1 : 0,
			                first->x, first->y, first->z, first->invW,
			                first->r, first->g, first->b, first->a,
			                first->u_ow, first->v_ow, first->kd_r,
			                first->kd_g, first->kd_b, priv->state[13].i,
			                priv->state[12].i);
		} else {
			QD3D_RENDER_LOG("frame=%u draw=%llu kind=%s vertices=%u textured=%d texture=0x%08x op=0x%x",
			                priv->frameCount, (unsigned long long)ms->draw_calls,
			                kind, vertices, textured ? 1 : 0,
			                priv->state[13].i, priv->state[12].i);
		}
	}
#else
	(void)priv; (void)kind; (void)vertices; (void)textured; (void)first;
#endif
}

static HostV read_gouraud_v(uint32 addr)
{
	HostV v = {};
	v.x = ReadMacFloat(addr + 0);
	v.y = ReadMacFloat(addr + 4);
	v.z = ReadMacFloat(addr + 8);
	v.invW = ReadMacFloat(addr + 12);
	v.r = ReadMacFloat(addr + 16);
	v.g = ReadMacFloat(addr + 20);
	v.b = ReadMacFloat(addr + 24);
	v.a = ReadMacFloat(addr + 28);
	v.u_ow = v.v_ow = 0.f;
	v.kd_r = v.kd_g = v.kd_b = 1.f;
	v.ks_r = v.ks_g = v.ks_b = 0.f;
	v.invW2 = v.invW;
	return v;
}

static HostV read_texture_v(uint32 addr)
{
	HostV v = {};
	v.x = ReadMacFloat(addr + 0);
	v.y = ReadMacFloat(addr + 4);
	v.z = ReadMacFloat(addr + 8);
	v.invW = ReadMacFloat(addr + 12);
	v.r = ReadMacFloat(addr + 16);
	v.g = ReadMacFloat(addr + 20);
	v.b = ReadMacFloat(addr + 24);
	v.a = ReadMacFloat(addr + 28);
	float uOverW = ReadMacFloat(addr + 32);
	float vOverW = ReadMacFloat(addr + 36);
	/* Keep overW form for GL TexCoord4 perspective divide (Metal shader parity).
	 * Flip V: uploaded textures have row 0 at top. */
	v.u_ow = uOverW;
	v.v_ow = (v.invW > 1e-8f) ? (v.invW - vOverW) : (1.0f - vOverW);
	v.kd_r = ReadMacFloat(addr + 40);
	v.kd_g = ReadMacFloat(addr + 44);
	v.kd_b = ReadMacFloat(addr + 48);
	v.ks_r = ReadMacFloat(addr + 52);
	v.ks_g = ReadMacFloat(addr + 56);
	v.ks_b = ReadMacFloat(addr + 60);
	v.invW2 = v.invW;
	return v;
}

static int s_current_fog_mode = 0;
static float s_current_fog_max_depth = 1.f;

static void apply_fog(RaveDrawPrivate *priv)
{
	assert(priv != nullptr);
	/* Match rave_draw_context / Metal: FogMode=17, FogColor a/r/g/b=18..21,
	 * FogStart/End/Density/MaxDepth = 22..25. Mode 0 = off.
	 * RAVE modes: 1=Alpha, 2=Linear, 3=Exp, 4=Exp2. Metal remaps QD3D's
	 * Exp2-with-plane-params to Linear (Bugdom etc.). */
	int fogMode = (int)priv->state[17].i;
	if (fogMode <= 0 || fogMode > 4) {
		s_current_fog_mode = 0;
		glDisable(GL_FOG);
		return;
	}
	float fstart = priv->state[22].f;
	float fend = priv->state[23].f;
	float fdens = priv->state[24].f;
	if (fogMode == 4 && fstart >= 0.f && fstart < fend && fdens > fend) {
		/* QD3D linear fog mislabeled as Exp2 — treat as linear */
		fogMode = 2;
	}
	s_current_fog_mode = fogMode;
	s_current_fog_max_depth = priv->state[25].f != 0.f ? priv->state[25].f : 1.f;
	glEnable(GL_FOG);
	GLenum glMode = GL_LINEAR;
	if (fogMode == 1)
		glMode = GL_LINEAR;
	else if (fogMode == 3)
		glMode = GL_EXP;
	else if (fogMode == 4)
		glMode = GL_EXP2;
	else
		glMode = GL_LINEAR; /* 1, 2 */
	glFogi(GL_FOG_MODE, (GLint)glMode);
	GLfloat col[4] = {
		priv->state[19].f, priv->state[20].f, priv->state[21].f, priv->state[18].f
	};
	glFogfv(GL_FOG_COLOR, col);
	glFogf(GL_FOG_START, fogMode == 1 ? 0.f : fstart);
	glFogf(GL_FOG_END, fogMode == 1 ? 1.f : (fend > 0.f ? fend : 1.f));
	if (glMode == GL_EXP || glMode == GL_EXP2)
		glFogf(GL_FOG_DENSITY, fdens > 0.f ? fdens : 0.1f);
	if (gfx_gl_ext().FogCoordf)
		glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
}

static void apply_draw_state(RaveDrawPrivate *priv, bool textured)
{
	assert(priv != nullptr);
	assert(priv->metal != nullptr);
	RaveMetalState *ms = priv->metal;
	const bool multi = textured && priv->multiTextureActive &&
	                   priv->multiTextureHandle != 0 &&
	                   gfx_gl_ext().multitex;
	if (s_draw_state_owner == ms && ms->draw_state_valid &&
	    priv->dirty_flags == 0 &&
	    ms->draw_state_textured == textured &&
	    ms->draw_state_multitexture == multi &&
	    ms->draw_state_multitexture_handle == priv->multiTextureHandle &&
	    ms->draw_state_multitexture_op == priv->multiTextureOp &&
	    ms->draw_state_multitexture_factor == priv->multiTextureFactor) {
		/* Resource uploads may have disturbed texture unit 0, so rebind the
		 * selected texture while leaving the much larger fixed-function state
		 * block cached. */
		if (textured)
			bind_current_texture(priv);
#if QD3D_INIT_LOGGING_ENABLED
		ms->state_cache_hits++;
#endif
		return;
	}
#if QD3D_INIT_LOGGING_ENABLED
	ms->state_applies++;
#endif
	apply_blend(priv);
	apply_depth(priv);
	apply_alpha_test(priv);
	apply_fog(priv);
	/* RAVE channel bits are R,G,B,A in bits 0..3. */
	uint32_t channelMask = priv->state[27].i & 0xf;
	if (channelMask == 0) channelMask = 0xf;
	glColorMask((channelMask & 1) ? GL_TRUE : GL_FALSE,
	            (channelMask & 2) ? GL_TRUE : GL_FALSE,
	            (channelMask & 4) ? GL_TRUE : GL_FALSE,
	            (channelMask & 8) ? GL_TRUE : GL_FALSE);

	/* GL scissor tags use top-left RAVE coordinates. */
	int32_t sx0 = (int32_t)priv->state[105].i;
	int32_t sy0 = (int32_t)priv->state[106].i;
	int32_t sx1 = (int32_t)priv->state[107].i;
	int32_t sy1 = (int32_t)priv->state[108].i;
	sx0 = std::max<int32_t>(0, std::min<int32_t>(sx0, priv->width));
	sx1 = std::max<int32_t>(0, std::min<int32_t>(sx1, priv->width));
	sy0 = std::max<int32_t>(0, std::min<int32_t>(sy0, priv->height));
	sy1 = std::max<int32_t>(0, std::min<int32_t>(sy1, priv->height));
	if (sx1 > sx0 && sy1 > sy0) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(sx0, priv->height - sy1, sx1 - sx0, sy1 - sy0);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}
	auto &ext = gfx_gl_ext();
	if (textured && (priv->state[12].i & 2) && ext.SecondaryColor3f)
		glEnable(GL_COLOR_SUM);
	else
		glDisable(GL_COLOR_SUM);
	if (textured) {
		bind_current_texture(priv);
		if (priv->multiTextureActive && priv->multiTextureHandle && ext.multitex) {
			bind_texture_unit(priv, priv->multiTextureHandle, 1);
			/* multiTextureOp: 0=Add, 1=Modulate, 2=BlendAlpha, 3=Fixed */
			int mop = (int)priv->multiTextureOp;
			if (mop == 0) {
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
			} else if (mop == 2) {
				/* Blend second unit by its alpha over previous */
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
				/* Arg2 = texture alpha as blend factor */
				glTexEnvi(GL_TEXTURE_ENV, 0x8582 /* GL_SOURCE2_RGB */, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, 0x8592 /* GL_OPERAND2_RGB */, GL_SRC_ALPHA);
			} else if (mop == 3) {
				/* Fixed factor: constant colour blend */
				float f = priv->multiTextureFactor;
				if (f < 0.f) f = 0.f;
				if (f > 1.f) f = 1.f;
				GLfloat c[4] = { f, f, f, f };
				glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, c);
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
			} else {
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			}
			if (ext.ActiveTexture)
				ext.ActiveTexture(GL_TEXTURE0);
		}
	} else {
		glDisable(GL_TEXTURE_2D);
		if (ext.multitex && ext.ActiveTexture) {
			ext.ActiveTexture(GL_TEXTURE1);
			glDisable(GL_TEXTURE_2D);
			ext.ActiveTexture(GL_TEXTURE0);
		}
	}
	priv->dirty_flags = 0;
	ms->draw_state_valid = true;
	ms->draw_state_textured = textured;
	ms->draw_state_multitexture = multi;
	ms->draw_state_multitexture_handle = priv->multiTextureHandle;
	ms->draw_state_multitexture_op = priv->multiTextureOp;
	ms->draw_state_multitexture_factor = priv->multiTextureFactor;
	s_draw_state_owner = ms;
}

static void emit_texcoords(const HostV &v)
{
	/* glTexCoord4f(s,t,r,q): after perspective divide s' = s/q.
	 * Pass (u_ow, v_ow, 0, invW) so s' = u, t' = v — Metal overW parity. */
	float q = (v.invW > 1e-8f) ? v.invW : 1.f;
	float q2 = (v.invW2 > 1e-8f) ? v.invW2 : q;
	auto &ext = gfx_gl_ext();
	if (ext.multitex && ext.MultiTexCoord4f) {
		ext.MultiTexCoord4f(GL_TEXTURE0, v.u_ow, v.v_ow, 0.f, q);
		ext.MultiTexCoord4f(GL_TEXTURE1, v.u2_ow, v.v2_ow, 0.f, q2);
	} else if (ext.multitex && ext.MultiTexCoord2f) {
		ext.MultiTexCoord2f(GL_TEXTURE0, v.u_ow / q, v.v_ow / q);
		ext.MultiTexCoord2f(GL_TEXTURE1, v.u2_ow / q2, v.v2_ow / q2);
	} else {
		glTexCoord4f(v.u_ow, v.v_ow, 0.f, q);
	}
}

static void emit_v(const HostV &v, bool textured, int texture_op)
{
	float r = v.r, g = v.g, b = v.b, a = v.a;
	const float vertexAlpha = a;
	if (textured && (texture_op & 16)) {
		/* GL_BLEND uses the primary color as its incoming object color. */
		r = v.r; g = v.g; b = v.b;
	} else if (textured && !(texture_op & 4)) {
		/* GL_MODULATE emulates the RAVE texture operation. TextureOp_None
		 * replaces the complete object color, including alpha, so the primary
		 * color must be opaque white. Descent II submits its explosion/death
		 * sprites with vertex alpha zero and expects their ARGB16 texture alpha
		 * to remain authoritative. Modulate instead multiplies texture alpha by
		 * vertex alpha, matching the Metal path. */
		if (texture_op & 1) {
			r = v.kd_r; g = v.kd_g; b = v.kd_b;
		} else {
			r = g = b = 1.f;
			if ((texture_op & (2 | 16)) == 0)
				a = 1.f;
		}
		/* Highlight's +ks term is supplied as the post-texture secondary color
		 * below when the compatibility context exposes that entry point. */
	}
	auto &ext = gfx_gl_ext();
	if (ext.FogCoordf && s_current_fog_mode == 1)
		a = 1.f;
	glColor4f(r, g, b, a);
	if (ext.FogCoordf && s_current_fog_mode != 0) {
		float fogCoord;
		if (s_current_fog_mode == 1) {
			/* GL linear fog uses (end-c)/(end-start), so c=1-alpha
			 * reproduces RAVE FogMode_Alpha's interpolation factor. */
			fogCoord = 1.f - vertexAlpha;
		} else {
			fogCoord = v.invW > 1e-8f ? (1.f / v.invW)
			                              : (v.z * s_current_fog_max_depth);
		}
		ext.FogCoordf(fogCoord);
	}
	if (ext.SecondaryColor3f) {
		if (textured && (texture_op & 2))
			ext.SecondaryColor3f(v.ks_r, v.ks_g, v.ks_b);
		else
			ext.SecondaryColor3f(0.f, 0.f, 0.f);
	}
	if (textured)
		emit_texcoords(v);
	/* Metal clamps submitted RAVE depth into [0,1]. Compatibility GL would
	 * otherwise clip legacy negative/oversized values before depth testing. */
	glVertex3f(v.x, v.y, RaveClampMetalDepth(v.z));
}

/* ---- Z-sorted transparency (kQATag_ZSortedHint = state[29]) ---- */

static inline bool zsort_enabled(const RaveDrawPrivate *priv)
{
	/* Metal path keys on == 1 (kQATag_ZSortedHint); keep same contract. */
	assert(priv != nullptr);
	return priv->state[29].i == 1;
}

static void hostv_pack(const HostV &v, float out[RAVE_VERTEX_FLOATS])
{
	std::memset(out, 0, sizeof(float) * RAVE_VERTEX_FLOATS);
	out[0] = v.x; out[1] = v.y; out[2] = v.z; out[3] = v.invW;
	out[4] = v.r; out[5] = v.g; out[6] = v.b; out[7] = v.a;
	out[8] = v.u_ow; out[9] = v.v_ow; out[10] = v.invW;
	out[11] = v.kd_r; out[12] = v.kd_g; out[13] = v.kd_b;
	out[14] = v.ks_r; out[15] = v.ks_g; out[16] = v.ks_b;
	out[17] = v.u2_ow; out[18] = v.v2_ow; out[19] = v.invW2;
}

static HostV hostv_unpack(const float in[RAVE_VERTEX_FLOATS])
{
	HostV v = {};
	v.x = in[0]; v.y = in[1]; v.z = in[2]; v.invW = in[3];
	v.r = in[4]; v.g = in[5]; v.b = in[6]; v.a = in[7];
	v.u_ow = in[8]; v.v_ow = in[9];
	v.kd_r = in[11]; v.kd_g = in[12]; v.kd_b = in[13];
	v.ks_r = in[14]; v.ks_g = in[15]; v.ks_b = in[16];
	v.u2_ow = in[17]; v.v2_ow = in[18];
	v.invW2 = (in[19] != 0.f) ? in[19] : v.invW;
	return v;
}

static void buffer_zsort_tri(RaveDrawPrivate *priv, const HostV &a, const HostV &b, const HostV &c, bool textured)
{
	if (!priv->zsortBuffer) {
		priv->zsortBuffer = new ZSortTriangle[RAVE_ZSORT_MAX_TRIANGLES];
		priv->zsortCount = 0;
	}
	if (priv->zsortCount >= RAVE_ZSORT_MAX_TRIANGLES) return;
	ZSortTriangle *tri = &priv->zsortBuffer[priv->zsortCount++];
	hostv_pack(a, tri->verts[0]);
	hostv_pack(b, tri->verts[1]);
	hostv_pack(c, tri->verts[2]);
	tri->sortKey = (a.z + b.z + c.z) / 3.0f;
	tri->textured = textured;
	tri->textureMacAddr = priv->state[13].i;
	tri->textureOp = (int32_t)priv->state[12].i;
	tri->blendMode = (int32_t)priv->state[9].i;
	tri->glBlendSrc = priv->state[109].i;
	tri->glBlendDst = priv->state[110].i;
	tri->filterMode = (int32_t)priv->state[11].i;
}

static void flush_zsort_buffer(RaveDrawPrivate *priv)
{
	if (!priv || !priv->zsortBuffer || priv->zsortCount == 0) return;
	if (!GfxGLDeviceMakeCurrent()) return;

	std::sort(priv->zsortBuffer, priv->zsortBuffer + priv->zsortCount,
	          [](const ZSortTriangle &x, const ZSortTriangle &y) {
	              return x.sortKey > y.sortKey; /* back-to-front */
	          });

	uint32_t saved_tex = priv->state[13].i;
	int32_t saved_top = (int32_t)priv->state[12].i;
	int32_t saved_blend = (int32_t)priv->state[9].i;
	uint32_t saved_gs = priv->state[109].i, saved_gd = priv->state[110].i;
	int32_t saved_filt = (int32_t)priv->state[11].i;

	/* Disable depth write for transparent sorted pass (typical RAVE behavior). */
	glDepthMask(GL_FALSE);

	for (uint32_t i = 0; i < priv->zsortCount; i++) {
		ZSortTriangle &tri = priv->zsortBuffer[i];
		priv->state[13].i = tri.textureMacAddr;
		priv->state[12].i = (uint32_t)tri.textureOp;
		priv->state[9].i = (uint32_t)tri.blendMode;
		priv->state[109].i = tri.glBlendSrc;
		priv->state[110].i = tri.glBlendDst;
		priv->state[11].i = (uint32_t)tri.filterMode;
		invalidate_draw_state(priv->metal);
		apply_draw_state(priv, tri.textured);
		glDepthMask(GL_FALSE);
		HostV a = hostv_unpack(tri.verts[0]);
		HostV b = hostv_unpack(tri.verts[1]);
		HostV c = hostv_unpack(tri.verts[2]);
		glBegin(GL_TRIANGLES);
		emit_v(a, tri.textured, tri.textureOp);
		emit_v(b, tri.textured, tri.textureOp);
		emit_v(c, tri.textured, tri.textureOp);
		glEnd();
	}

	priv->state[13].i = saved_tex;
	priv->state[12].i = (uint32_t)saved_top;
	priv->state[9].i = (uint32_t)saved_blend;
	priv->state[109].i = saved_gs;
	priv->state[110].i = saved_gd;
	priv->state[11].i = (uint32_t)saved_filt;
	priv->zsortCount = 0;
	apply_depth(priv); /* restore depth write */
	invalidate_draw_state(priv->metal);
}

static bool copy_initial_texture(GLuint source, uint32_t w, uint32_t h)
{
	if (!source || source == s_overlay_tex) return source == s_overlay_tex;
	auto &ext = gfx_gl_ext();
	if (ext.multitex && ext.ActiveTexture)
		ext.ActiveTexture(GL_TEXTURE0);
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
	             GL_TEXTURE_BIT | GL_VIEWPORT_BIT | GL_SCISSOR_BIT);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_FOG);
	glDisable(GL_SCISSOR_TEST);
	glDepthMask(GL_FALSE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, source);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor4f(1.f, 1.f, 1.f, 1.f);
	glBegin(GL_QUADS);
	/* Render-target textures use bottom-left texture coordinates while RAVE's
	 * projection uses top-left screen coordinates. */
	glTexCoord2f(0.f, 1.f); glVertex3f(0.f, 0.f, 0.f);
	glTexCoord2f(1.f, 1.f); glVertex3f((float)w, 0.f, 0.f);
	glTexCoord2f(1.f, 0.f); glVertex3f((float)w, (float)h, 0.f);
	glTexCoord2f(0.f, 0.f); glVertex3f(0.f, (float)h, 0.f);
	glEnd();
	glPopAttrib();
	return glGetError() == GL_NO_ERROR;
}

/* ---- Render lifecycle ---- */

int32_t NativeRenderStart(uint32_t drawContextAddr, uint32_t dirtyRectAddr, uint32_t initialContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) {
		QD3D_RENDER_LOG("RenderStart rejected: context=0x%08x native context/state missing",
		                drawContextAddr);
		return 1;
	}
	RaveMetalState *ms = priv->metal;
	uint32_t w = priv->width > 0 ? (uint32_t)priv->width : s_ow;
	uint32_t h = priv->height > 0 ? (uint32_t)priv->height : s_oh;
	if (!w || !h) {
		QD3D_RENDER_LOG("RenderStart rejected: context=0x%08x has zero size", drawContextAddr);
		return 1;
	}

	/* Capture the source before bind_overlay_fbo advances this context to the
	 * current write texture. This implements kQAOptional_BufferComposite for
	 * initialContext, including the common self-context/double-buffer case. */
	GLuint initialTex = 0;
	uint32_t initialHandle = 0;
	if (initialContextAddr) {
		initialHandle = ReadMacInt32(initialContextAddr + 0);
		RaveDrawPrivate *initial = RaveGetContext(initialHandle);
		if (initial && initial->metal)
			initialTex = initial->metal->color_tex;
		if (!initialTex)
			initialTex = s_last_submitted_tex;
	}
	if (!bind_overlay_fbo(ms, w, h)) return 1;
	/* Clear/load actions must not inherit the previous draw's write mask or
	 * scissor rectangle. Draw state is re-applied before every draw call. */
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	ms->pass_active = true;
	priv->frameCount++;
	priv->zsortCount = 0;
#if QD3D_INIT_LOGGING_ENABLED
	ms->draw_calls = 0;
	ms->vertices = 0;
	ms->textured_draws = 0;
	ms->texture_binds = 0;
	ms->missing_textures = 0;
	ms->dropped_draws = 0;
	ms->state_applies = 0;
	ms->state_cache_hits = 0;
	ms->logged_draws = 0;
#endif

	/* RAVE clear state is alpha at tag 1 and RGB at tags 2/3/4. The overlay
	 * mailbox is premultiplied, matching the Metal render pass. */
	const DMCModeSnapshot *clear_snap = dmc_current_snapshot();
	const float ca = RaveOverlayEffectiveClearAlpha(
	    priv->state[2].f, priv->state[3].f, priv->state[4].f,
	    priv->state[1].f, w, h,
	    clear_snap ? clear_snap->width : 0,
	    clear_snap ? clear_snap->height : 0);
	const float cr = RaveOverlayPremultipliedClearComponent(priv->state[2].f, ca);
	const float cg = RaveOverlayPremultipliedClearComponent(priv->state[3].f, ca);
	const float cb = RaveOverlayPremultipliedClearComponent(priv->state[4].f, ca);
	glClearDepth(1.0);
	bool initialCopied = false;
	if (initialContextAddr && initialTex) {
		initialCopied = copy_initial_texture(initialTex, w, h);
		if (initialCopied)
			glClear(GL_DEPTH_BUFFER_BIT);
	}
	if (!initialCopied) {
		glClearColor(cr, cg, cb, ca);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}
	ms->cleared = true;
	/* Metal fires the image-buffer initializer after the render target has
	 * been cleared/loaded and before the first 3D draw. */
	fire_notice_method(priv, 3);
#if QD3D_INIT_LOGGING_ENABLED
	if (trace_frame(priv)) {
		int32_t dl = 0, dr = 0, dt = 0, db = 0;
		if (dirtyRectAddr) {
			dl = (int32_t)ReadMacInt32(dirtyRectAddr + 0);
			dr = (int32_t)ReadMacInt32(dirtyRectAddr + 4);
			dt = (int32_t)ReadMacInt32(dirtyRectAddr + 8);
			db = (int32_t)ReadMacInt32(dirtyRectAddr + 12);
		}
		QD3D_RENDER_LOG("RenderStart frame=%u ctx=0x%08x flags=0x%x fbo=%u color=%u depth=%u size=%ux%u dirty=0x%08x[%d,%d,%d,%d] initial=0x%08x handle=%u source=%u copied=%d clear=%.3f/%.3f/%.3f/%.3f glError=0x%x",
		                priv->frameCount, drawContextAddr, priv->flags,
		                (unsigned)ms->fbo, (unsigned)ms->color_tex,
		                (unsigned)ms->depth_rb, w, h, dirtyRectAddr,
		                dl, dr, dt, db, initialContextAddr, initialHandle,
		                (unsigned)initialTex, initialCopied ? 1 : 0,
		                cr, cg, cb, ca, (unsigned)glGetError());
	}
#endif
	return kQANoErr;
}

int32_t NativeRenderEnd(uint32_t drawContextAddr, uint32_t modifiedRectAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) {
		QD3D_RENDER_LOG("RenderEnd rejected: context=0x%08x native context/state missing",
		                drawContextAddr);
		return 1;
	}
	RaveMetalState *ms = priv->metal;
	if (!ms->pass_active) {
		QD3D_RENDER_LOG("RenderEnd ignored: frame=%u ctx=0x%08x has no active pass",
		                priv->frameCount, drawContextAddr);
		return kQANoErr;
	}
	const bool wasActive = true;
	flush_zsort_buffer(priv);
	/* Descent II registers selector 4. It receives the rendered image in the
	 * active display format, composites its CPU-side 2D content, and returns a
	 * dirty rectangle. Keep both trace stages so callback vs 3D failures are
	 * distinguishable in one log. */
#if QD3D_INIT_LOGGING_ENABLED
	trace_overlay_readback(priv, ms, "pre-notice");
#endif
	fire_notice_method(priv, 4);
	if (ms->pass_active) {
		glFlush();
#if QD3D_INIT_LOGGING_ENABLED
		trace_overlay_readback(priv, ms, "post-notice");
#endif
		unbind_fbo();
		ms->pass_active = false;
	}
	priv->multiTextureActive = false;
	priv->multiTexStagingCount = 0;
	/* Render-completion is a standard two-argument notice. */
	fire_notice_method(priv, 0);

	CompositeLayer layer = {};
	layer.source = (void *)(uintptr_t)s_overlay_tex;
	layer.src_size_w = s_ow;
	layer.src_size_h = s_oh;
	layer.dst_origin_x = (float)(priv->left ? priv->left : s_dst_l);
	layer.dst_origin_y = (float)(priv->top ? priv->top : s_dst_t);
	layer.dst_size_w = (float)(priv->width > 0 ? priv->width : (int)s_ow);
	layer.dst_size_h = (float)(priv->height > 0 ? priv->height : (int)s_oh);
	layer.slot = kLayerSlotOverlay;
	layer.blend = kBlendPremultiplied;
	layer.alpha = 1.f;

	FrameDescriptor desc = {};
	desc.layers = &layer;
	desc.layer_count = 1;
	/* Perform an ownership transition before publishing. A DSp movie may have
	 * taken ownership after this RAVE context was created; transitioning after
	 * SubmitFrame would make the compositor's mode-exit callback immediately
	 * discard the frame we just cached. */
	const int32_t ownerResult = dmc_set_active_owner(kDMCOwnerRAVE);
	const DMCModeSnapshot *snap = dmc_current_snapshot();
	desc.generation = snap ? snap->generation : 0;
	int32_t submitResult = MetalCompositorSubmitFrame(&desc);
	if (submitResult == kGfxAccelNoErr) {
		s_last_submitted_tex = s_overlay_tex;
	}
#if QD3D_INIT_LOGGING_ENABLED
	GLenum glError = glGetError();
	if (trace_frame(priv) || ownerResult != 0 || submitResult != 0 || glError != GL_NO_ERROR ||
	    ms->missing_textures != 0 || ms->dropped_draws != 0) {
		QD3D_RENDER_LOG("RenderEnd frame=%u ctx=0x%08x active=%d modified=0x%08x draws=%llu textured=%llu vertices=%llu textureBinds=%llu stateApplies=%llu stateCacheHits=%llu missingTextures=%llu dropped=%llu zsortPending=%u owner=%d/%u submit=%d overlay=%u next=%u generation=%llu glError=0x%x",
		                priv->frameCount, drawContextAddr, wasActive ? 1 : 0,
		                modifiedRectAddr, (unsigned long long)ms->draw_calls,
		                (unsigned long long)ms->textured_draws,
		                (unsigned long long)ms->vertices,
		                (unsigned long long)ms->texture_binds,
		                (unsigned long long)ms->state_applies,
		                (unsigned long long)ms->state_cache_hits,
		                (unsigned long long)ms->missing_textures,
		                (unsigned long long)ms->dropped_draws, priv->zsortCount,
		                ownerResult, snap ? snap->active_owner : UINT32_MAX,
		                submitResult, (unsigned)s_overlay_tex,
		                (unsigned)s_overlay_pair[s_write ^ 1],
		                (unsigned long long)desc.generation, (unsigned)glError);
	}
#else
	(void)submitResult;
#endif

	if (submitResult == kGfxAccelNoErr) {
		s_write ^= 1;
		s_overlay_tex = s_overlay_pair[s_write];
	}
	MetalCompositorSync3DFramePacingForEngine(kGfxFramePacingEngineRAVE);
	return kQANoErr;
}

int32_t NativeRenderAbort(uint32_t drawContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return 1;
	RaveMetalState *ms = priv->metal;
	if (ms->pass_active) {
		unbind_fbo();
		ms->pass_active = false;
	}
	invalidate_draw_state(ms);
	priv->zsortCount = 0;
	priv->multiTextureActive = false;
	priv->multiTexStagingCount = 0;
	QD3D_RENDER_LOG("RenderAbort frame=%u ctx=0x%08x discarded color=%u draws=%llu vertices=%llu",
	                priv->frameCount, drawContextAddr, (unsigned)ms->color_tex,
	                (unsigned long long)ms->draw_calls,
	                (unsigned long long)ms->vertices);
	return kQANoErr;
}
int32_t NativeFlush(uint32_t) { if (GfxGLDeviceMakeCurrent()) glFlush(); return kQANoErr; }
int32_t NativeSync(uint32_t) { if (GfxGLDeviceMakeCurrent()) glFinish(); return kQANoErr; }

/* ---- Draws ---- */

int32_t NativeDrawTriGouraud(uint32_t drawContextAddr, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t /*flags*/)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!accept_draw(priv, "TriGouraud", 3)) return kQANoErr;
	if (!GfxGLDeviceMakeCurrent()) return 1;
	HostV a = read_gouraud_v(v0), b = read_gouraud_v(v1), c = read_gouraud_v(v2);
	record_draw(priv, "TriGouraud", 3, false, &a);
	if (zsort_enabled(priv)) {
		buffer_zsort_tri(priv, a, b, c, false);
		return kQANoErr;
	}
	apply_draw_state(priv, false);
	glBegin(GL_TRIANGLES);
	emit_v(a, false, 0); emit_v(b, false, 0); emit_v(c, false, 0);
	glEnd();
	return kQANoErr;
}

int32_t NativeDrawTriTexture(uint32_t drawContextAddr, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t /*flags*/)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!accept_draw(priv, "TriTexture", 3)) return kQANoErr;
	if (!GfxGLDeviceMakeCurrent()) return 1;
	int top = (int)priv->state[12].i;
	HostV a = read_texture_v(v0), b = read_texture_v(v1), c = read_texture_v(v2);
	record_draw(priv, "TriTexture", 3, true, &a);
	/* Apply multi-tex UV2 for first 3 staged verts if available */
	if (priv->multiTextureActive && priv->multiTexStagingBuffer && priv->multiTexStagingCount >= 3) {
		const float *uv2 = (const float *)priv->multiTexStagingBuffer;
		a.u2_ow = uv2[0]; a.v2_ow = uv2[1]; a.invW2 = uv2[2] > 0.f ? uv2[2] : a.invW;
		b.u2_ow = uv2[4]; b.v2_ow = uv2[5]; b.invW2 = uv2[6] > 0.f ? uv2[6] : b.invW;
		c.u2_ow = uv2[8]; c.v2_ow = uv2[9]; c.invW2 = uv2[10] > 0.f ? uv2[10] : c.invW;
	}
	if (zsort_enabled(priv)) {
		buffer_zsort_tri(priv, a, b, c, true);
		return kQANoErr;
	}
	apply_draw_state(priv, true);
	glBegin(GL_TRIANGLES);
	emit_v(a, true, top); emit_v(b, true, top); emit_v(c, true, top);
	glEnd();
	return kQANoErr;
}

/* vertexMode: 0=points 1=lines 2=polyline 3=triangles 4=strip 5=fan (typical RAVE) */
static GLenum map_vertex_mode(uint32_t mode, bool &need_convert_fan)
{
	need_convert_fan = false;
	switch (mode) {
	case 0: return GL_POINTS;
	case 1: return GL_LINES;
	case 2: return GL_LINE_STRIP;
	case 3: return GL_TRIANGLES;
	case 4: return GL_TRIANGLE_STRIP;
	case 5: need_convert_fan = true; return GL_TRIANGLES;
	default: return GL_TRIANGLE_STRIP;
	}
}

int32_t NativeDrawVGouraud(uint32_t drawContextAddr, uint32_t nVertices, uint32_t vertexMode,
                           uint32_t verticesAddr, uint32_t /*flagsAddr*/)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!nVertices || !verticesAddr)
		return kQANoErr;
	if (!accept_draw(priv, "VGouraud", nVertices))
		return kQANoErr;
	if (!GfxGLDeviceMakeCurrent()) return 1;
	const uint32 stride = 32;
	HostV traceFirst = read_gouraud_v(verticesAddr);
	record_draw(priv, "VGouraud", nVertices, false, &traceFirst);
	bool fan = false;
	GLenum mode = map_vertex_mode(vertexMode, fan);
	const bool zsort = zsort_enabled(priv) && (fan || vertexMode == 3 || vertexMode == 4 || vertexMode == 5);

	if (fan && nVertices >= 3) {
		HostV v0 = read_gouraud_v(verticesAddr);
		if (!zsort) {
			apply_draw_state(priv, false);
			glBegin(GL_TRIANGLES);
		}
		for (uint32 i = 1; i + 1 < nVertices; i++) {
			HostV a = v0;
			HostV b = read_gouraud_v(verticesAddr + i * stride);
			HostV c = read_gouraud_v(verticesAddr + (i + 1) * stride);
			if (zsort)
				buffer_zsort_tri(priv, a, b, c, false);
			else {
				emit_v(a, false, 0); emit_v(b, false, 0); emit_v(c, false, 0);
			}
		}
		if (!zsort)
			glEnd();
		return kQANoErr;
	}
	if (zsort && vertexMode == 3) {
		for (uint32 i = 0; i + 2 < nVertices; i += 3) {
			buffer_zsort_tri(priv,
				read_gouraud_v(verticesAddr + i * stride),
				read_gouraud_v(verticesAddr + (i + 1) * stride),
				read_gouraud_v(verticesAddr + (i + 2) * stride), false);
		}
		return kQANoErr;
	}
	if (zsort && vertexMode == 4 && nVertices >= 3) {
		HostV prev0 = read_gouraud_v(verticesAddr);
		HostV prev1 = read_gouraud_v(verticesAddr + stride);
		for (uint32 i = 2; i < nVertices; i++) {
			HostV cur = read_gouraud_v(verticesAddr + i * stride);
			if ((i & 1) == 0)
				buffer_zsort_tri(priv, prev0, prev1, cur, false);
			else
				buffer_zsort_tri(priv, prev1, prev0, cur, false);
			prev0 = prev1;
			prev1 = cur;
		}
		return kQANoErr;
	}
	apply_draw_state(priv, false);
	glBegin(mode);
	for (uint32 i = 0; i < nVertices; i++)
		emit_v(read_gouraud_v(verticesAddr + i * stride), false, 0);
	glEnd();
	return kQANoErr;
}

int32_t NativeDrawVTexture(uint32_t drawContextAddr, uint32_t nVertices, uint32_t vertexMode,
                           uint32_t verticesAddr, uint32_t /*flagsAddr*/)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!nVertices || !verticesAddr)
		return kQANoErr;
	if (!accept_draw(priv, "VTexture", nVertices))
		return kQANoErr;
	if (!GfxGLDeviceMakeCurrent()) return 1;
	const uint32 stride = 64;
	int top = (int)priv->state[12].i;
	HostV traceFirst = read_texture_v(verticesAddr);
	record_draw(priv, "VTexture", nVertices, true, &traceFirst);
	bool fan = false;
	GLenum mode = map_vertex_mode(vertexMode, fan);
	const bool zsort = zsort_enabled(priv) && (fan || vertexMode == 3 || vertexMode == 4 || vertexMode == 5);

	auto read_tex_mt = [&](uint32 idx) -> HostV {
		HostV v = read_texture_v(verticesAddr + idx * stride);
		if (priv->multiTextureActive && priv->multiTexStagingBuffer && idx < priv->multiTexStagingCount) {
			const float *uv2 = (const float *)priv->multiTexStagingBuffer;
			v.u2_ow = uv2[idx * 4 + 0];
			v.v2_ow = uv2[idx * 4 + 1];
			v.invW2 = uv2[idx * 4 + 2] > 0.f ? uv2[idx * 4 + 2] : v.invW;
		}
		return v;
	};

	if (fan && nVertices >= 3) {
		HostV v0 = read_tex_mt(0);
		if (!zsort) {
			apply_draw_state(priv, true);
			glBegin(GL_TRIANGLES);
		}
		for (uint32 i = 1; i + 1 < nVertices; i++) {
			HostV a = v0, b = read_tex_mt(i), c = read_tex_mt(i + 1);
			if (zsort)
				buffer_zsort_tri(priv, a, b, c, true);
			else {
				emit_v(a, true, top); emit_v(b, true, top); emit_v(c, true, top);
			}
		}
		if (!zsort)
			glEnd();
		return kQANoErr;
	}
	if (zsort && vertexMode == 3) {
		for (uint32 i = 0; i + 2 < nVertices; i += 3)
			buffer_zsort_tri(priv, read_tex_mt(i), read_tex_mt(i + 1), read_tex_mt(i + 2), true);
		return kQANoErr;
	}
	if (zsort && vertexMode == 4 && nVertices >= 3) {
		HostV prev0 = read_tex_mt(0), prev1 = read_tex_mt(1);
		for (uint32 i = 2; i < nVertices; i++) {
			HostV cur = read_tex_mt(i);
			if ((i & 1) == 0)
				buffer_zsort_tri(priv, prev0, prev1, cur, true);
			else
				buffer_zsort_tri(priv, prev1, prev0, cur, true);
			prev0 = prev1;
			prev1 = cur;
		}
		return kQANoErr;
	}
	apply_draw_state(priv, true);
	glBegin(mode);
	for (uint32 i = 0; i < nVertices; i++)
		emit_v(read_tex_mt(i), true, top);
	glEnd();
	return kQANoErr;
}

int32_t NativeSubmitVerticesGouraud(uint32_t drawContextAddr, uint32_t nVertices, uint32_t verticesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !nVertices || !verticesAddr) return kQANoErr;
	const uint32 stride = 32;
	uint32_t maxv = priv->vertexStagingCapacity ? priv->vertexStagingCapacity : 65536;
	if (nVertices > maxv) nVertices = maxv;
	if (!priv->vertexStagingBuffer) {
		/* The guest Gouraud record is 32 bytes, but each converted HostV is
		 * larger (currently 19 floats). Allocating by guest stride corrupts
		 * the heap as soon as more than a fraction of the buffer is used. */
		priv->vertexStagingBuffer = new uint8_t[(size_t)maxv * sizeof(HostV)];
		priv->vertexStagingCapacity = maxv;
	}
	if (!priv->vertexStagingBuffer) return 1;
	uint8 *src = Mac2HostAddr(verticesAddr);
	if (!src) return 1;
	/* Copy BE floats as-is; draw path will re-read via ReadMacFloat if needed.
	 * For staging we keep host-endian conversion: */
	for (uint32 i = 0; i < nVertices; i++) {
		HostV v = read_gouraud_v(verticesAddr + i * stride);
		std::memcpy(priv->vertexStagingBuffer + i * sizeof(HostV), &v, sizeof(HostV));
	}
	priv->vertexStagingCount = nVertices;
	return kQANoErr;
}

int32_t NativeSubmitVerticesTexture(uint32_t drawContextAddr, uint32_t nVertices, uint32_t verticesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !nVertices || !verticesAddr) return kQANoErr;
	const uint32 stride = 64;
	uint32_t maxv = priv->vertexStagingCapacity ? priv->vertexStagingCapacity : 65536;
	if (nVertices > maxv) nVertices = maxv;
	if (!priv->vertexStagingBuffer) {
		priv->vertexStagingBuffer = new uint8_t[(size_t)maxv * sizeof(HostV)];
		priv->vertexStagingCapacity = maxv;
	}
	if (!priv->vertexStagingBuffer) return 1;
	for (uint32 i = 0; i < nVertices; i++) {
		HostV v = read_texture_v(verticesAddr + i * stride);
		std::memcpy(priv->vertexStagingBuffer + i * sizeof(HostV), &v, sizeof(HostV));
	}
	priv->vertexStagingCount = nVertices;
	return kQANoErr;
}

int32_t NativeSubmitMultiTextureParams(uint32_t drawContextAddr, uint32_t nVertices, uint32_t multiTexParamsAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv) return kQANoErr;
	if (!nVertices || !multiTexParamsAddr) {
		priv->multiTextureActive = false;
		priv->multiTexStagingCount = 0;
		return kQANoErr;
	}
	/* Metal parity: MultiTextureEnable / Current layer gates */
	uint32_t currentLayer = priv->state[34].i;  /* kQATag_MultiTextureCurrent */
	uint32_t enabledLayers = priv->state[33].i; /* kQATag_MultiTextureEnable */
	if (enabledLayers != 0 && ((enabledLayers & (1u << currentLayer)) == 0)) {
		priv->multiTextureActive = false;
		priv->multiTexStagingCount = 0;
		return kQANoErr;
	}
	/* TQAVMultiTexture: 12 bytes/vertex (invW, uOverW, vOverW) per RAVE 1.6 */
	static const uint32 kMultiTexStride = 12;
	uint32 maxVerts = priv->vertexStagingCapacity ? priv->vertexStagingCapacity : 65536;
	if (nVertices > maxVerts) nVertices = maxVerts;
	if (!priv->multiTexStagingBuffer) {
		priv->multiTexStagingBuffer = new uint8_t[(size_t)maxVerts * 16];
		if (!priv->multiTexStagingBuffer) return 1;
	}
	float *dst = (float *)priv->multiTexStagingBuffer;
	for (uint32 i = 0; i < nVertices; i++) {
		uint32 srcAddr = multiTexParamsAddr + i * kMultiTexStride;
		float invW = ReadMacFloat(srcAddr + 0);
		float uOverW = ReadMacFloat(srcAddr + 4);
		float vOverW = ReadMacFloat(srcAddr + 8);
		/* Metal layout: (uOverW, invW-vOverW, invW, 0) — keep overW for TexCoord4 */
		dst[i * 4 + 0] = uOverW;
		dst[i * 4 + 1] = invW - vOverW; /* V flip, still /w form */
		dst[i * 4 + 2] = invW;
		dst[i * 4 + 3] = 0.0f;
	}
	priv->multiTexStagingCount = nVertices;
	priv->multiTextureHandle = priv->state[26].i;  /* kQATag_MultiTexture */
	priv->multiTextureOp = priv->state[35].i;      /* kQATag_MultiTextureOp */
	priv->multiTextureFactor = priv->state[51].f;  /* kQATag_MultiTextureFactor */
	priv->multiTextureActive = (priv->multiTextureHandle != 0);
	return kQANoErr;
}

int32_t NativeDrawPoint(uint32_t drawContextAddr, uint32_t v0)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!accept_draw(priv, "Point", 1)) return kQANoErr;
	if (!GfxGLDeviceMakeCurrent()) return 1;
	apply_draw_state(priv, false);
	HostV a = read_gouraud_v(v0);
	record_draw(priv, "Point", 1, false, &a);
	float w = priv->state[5].f;
	if (w < 1.f) w = 1.f;
	glPointSize(w);
	glBegin(GL_POINTS); emit_v(a, false, 0); glEnd();
	return kQANoErr;
}

int32_t NativeDrawLine(uint32_t drawContextAddr, uint32_t v0, uint32_t v1)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!accept_draw(priv, "Line", 2)) return kQANoErr;
	if (!GfxGLDeviceMakeCurrent()) return 1;
	apply_draw_state(priv, false);
	HostV a = read_gouraud_v(v0), b = read_gouraud_v(v1);
	record_draw(priv, "Line", 2, false, &a);
	float w = priv->state[5].f;
	if (w < 1.f) w = 1.f;
	glLineWidth(w);
	glBegin(GL_LINES); emit_v(a, false, 0); emit_v(b, false, 0); glEnd();
	return kQANoErr;
}

int32_t NativeDrawBitmap(uint32_t drawContextAddr, uint32_t vertexAddr, uint32_t bitmapMacAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!accept_draw(priv, "Bitmap", 4)) return kQANoErr;
	if (!GfxGLDeviceMakeCurrent()) return 1;

	/* Bitmap: screen-aligned textured quad from TQAVBitmap + texture resource */
	uint32 handle = RaveResourceFindByAddr(bitmapMacAddr);
	RaveResourceEntry *entry = RaveResourceGet(handle);
	if (!entry) return kQANoErr;
	if (!entry->metal_texture && entry->pixmap_mac_addr)
		RaveRealizeDeferredTexture(entry);
	if (!entry->metal_texture) return kQANoErr;

	float x = ReadMacFloat(vertexAddr + 0);
	float y = ReadMacFloat(vertexAddr + 4);
	float z = RaveClampMetalDepth(ReadMacFloat(vertexAddr + 8));
	float invW = ReadMacFloat(vertexAddr + 12);
	float alpha = ReadMacFloat(vertexAddr + 28);
	HostV traceFirst = {};
	traceFirst.x = x; traceFirst.y = y; traceFirst.z = z; traceFirst.invW = invW;
	traceFirst.r = traceFirst.g = traceFirst.b = 1.f; traceFirst.a = alpha;
	record_draw(priv, "Bitmap", 4, true, &traceFirst);
	float w = entry->width > 0 ? (float)entry->width : 1.f;
	float h = entry->height > 0 ? (float)entry->height : 1.f;
	float scaleX = priv->state[52].f;
	float scaleY = priv->state[53].f;
	if (scaleX <= 0.f) scaleX = 1.f;
	if (scaleY <= 0.f) scaleY = 1.f;
	w *= scaleX;
	h *= scaleY;

	apply_draw_state(priv, false);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_FOG);
	glDisable(GL_ALPHA_TEST);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)entry->metal_texture);
	int bitmapFilter = (int)priv->state[54].i;
	GLenum mag = bitmapFilter >= 1 ? GL_LINEAR : GL_NEAREST;
	GLenum minf = bitmapFilter >= 2 && entry->mip_levels > 1
		? GL_LINEAR_MIPMAP_LINEAR : mag;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glColor4f(1, 1, 1, alpha);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex3f(x, y, z);
	glTexCoord2f(1, 0); glVertex3f(x + w, y, z);
	glTexCoord2f(1, 1); glVertex3f(x + w, y + h, z);
	glTexCoord2f(0, 1); glVertex3f(x, y + h, z);
	glEnd();
	/* DrawBitmap deliberately overrides fixed-function state. The next RAVE
	 * draw must not treat the cached state as still resident in GL. */
	invalidate_draw_state(priv->metal);
	return kQANoErr;
}

int32_t NativeDrawTriMeshGouraud(uint32_t drawContextAddr, uint32_t numTriangles, uint32_t trianglesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!numTriangles || !trianglesAddr)
		return kQANoErr;
	if (!accept_draw(priv, "TriMeshGouraud", numTriangles * 3))
		return kQANoErr;
	if (!priv->vertexStagingBuffer || !priv->vertexStagingCount) return kQANoErr;
	if (!GfxGLDeviceMakeCurrent()) return 1;
	const HostV *verts = (const HostV *)priv->vertexStagingBuffer;
	record_draw(priv, "TriMeshGouraud", numTriangles * 3, false, &verts[0]);
	const bool zsort = zsort_enabled(priv);
	if (!zsort)
		apply_draw_state(priv, false);
	if (!zsort)
		glBegin(GL_TRIANGLES);
	for (uint32 t = 0; t < numTriangles; t++) {
		uint32 i0 = ReadMacInt32(trianglesAddr + t * 12 + 0);
		uint32 i1 = ReadMacInt32(trianglesAddr + t * 12 + 4);
		uint32 i2 = ReadMacInt32(trianglesAddr + t * 12 + 8);
		if (i0 >= priv->vertexStagingCount || i1 >= priv->vertexStagingCount || i2 >= priv->vertexStagingCount)
			continue;
		if (zsort) {
			buffer_zsort_tri(priv, verts[i0], verts[i1], verts[i2], false);
		} else {
			emit_v(verts[i0], false, 0);
			emit_v(verts[i1], false, 0);
			emit_v(verts[i2], false, 0);
		}
	}
	if (!zsort)
		glEnd();
	return kQANoErr;
}

int32_t NativeDrawTriMeshTexture(uint32_t drawContextAddr, uint32_t numTriangles, uint32_t trianglesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!numTriangles || !trianglesAddr)
		return kQANoErr;
	if (!accept_draw(priv, "TriMeshTexture", numTriangles * 3))
		return kQANoErr;
	if (!priv->vertexStagingBuffer || !priv->vertexStagingCount) return kQANoErr;
	if (!GfxGLDeviceMakeCurrent()) return 1;
	int top = (int)priv->state[12].i;
	/* Copy staged verts so we can attach multi-tex UVs without mutating staging */
	std::vector<HostV> local(priv->vertexStagingCount);
	std::memcpy(local.data(), priv->vertexStagingBuffer, priv->vertexStagingCount * sizeof(HostV));
	record_draw(priv, "TriMeshTexture", numTriangles * 3, true, &local[0]);
	if (priv->multiTextureActive && priv->multiTexStagingBuffer && priv->multiTexStagingCount > 0) {
		const float *uv2 = (const float *)priv->multiTexStagingBuffer;
		uint32 n = std::min(priv->vertexStagingCount, priv->multiTexStagingCount);
		for (uint32 i = 0; i < n; i++) {
			local[i].u2_ow = uv2[i * 4 + 0];
			local[i].v2_ow = uv2[i * 4 + 1];
			local[i].invW2 = uv2[i * 4 + 2] > 0.f ? uv2[i * 4 + 2] : local[i].invW;
		}
	}
	const bool zsort = zsort_enabled(priv);
	if (!zsort)
		apply_draw_state(priv, true);
	if (!zsort)
		glBegin(GL_TRIANGLES);
	for (uint32 t = 0; t < numTriangles; t++) {
		uint32 i0 = ReadMacInt32(trianglesAddr + t * 12 + 0);
		uint32 i1 = ReadMacInt32(trianglesAddr + t * 12 + 4);
		uint32 i2 = ReadMacInt32(trianglesAddr + t * 12 + 8);
		if (i0 >= priv->vertexStagingCount || i1 >= priv->vertexStagingCount || i2 >= priv->vertexStagingCount)
			continue;
		if (zsort) {
			buffer_zsort_tri(priv, local[i0], local[i1], local[i2], true);
		} else {
			emit_v(local[i0], true, top);
			emit_v(local[i1], true, top);
			emit_v(local[i2], true, top);
		}
	}
	if (!zsort)
		glEnd();
	return kQANoErr;
}

int32_t NativeSetNoticeMethod(uint32_t drawContextAddr, uint32_t method, uint32_t callback, uint32_t refCon)
{
	RaveDrawPrivate *c = GetContextFromDrawAddr(drawContextAddr);
	if (!c || method >= RAVE_NUM_NOTICE_METHODS) return 1;
	c->noticeMethods[method].callback = callback;
	c->noticeMethods[method].refCon = refCon;
	QD3D_STATE_LOG("SetNoticeMethod(GL): ctx=0x%08x selector=%u callback=0x%08x refCon=0x%08x",
	               drawContextAddr, method, callback, refCon);
	return kQANoErr;
}
int32_t NativeGetNoticeMethod(uint32_t drawContextAddr, uint32_t method, uint32_t callbackOutPtr, uint32_t refConOutPtr)
{
	RaveDrawPrivate *c = GetContextFromDrawAddr(drawContextAddr);
	if (!c || method >= RAVE_NUM_NOTICE_METHODS) return 1;
	if (callbackOutPtr) WriteMacInt32(callbackOutPtr, c->noticeMethods[method].callback);
	if (refConOutPtr) WriteMacInt32(refConOutPtr, c->noticeMethods[method].refCon);
	return kQANoErr;
}

#ifndef kQAError
#define kQAError 1
#endif

int32_t NativeAccessDrawBuffer(uint32_t drawContextAddr, uint32_t bufferStructAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !bufferStructAddr) return kQAError;
	RaveMetalState *ms = priv->metal;
	if (!ms->pass_active || !ms->color_tex || !GfxGLDeviceMakeCurrent()) return kQAError;
	if (!copy_overlay_to_guest(priv, ms)) return kQAError;
	WriteMacInt32(bufferStructAddr + 0, ms->draw_cpu_row_bytes);
	WriteMacInt32(bufferStructAddr + 4, ms->draw_cpu_pixel_type);
	WriteMacInt32(bufferStructAddr + 8, ms->w);
	WriteMacInt32(bufferStructAddr + 12, ms->h);
	WriteMacInt32(bufferStructAddr + 16, ms->draw_cpu_mac);
	ms->draw_accessed = true;
	return kQANoErr;
}

int32_t NativeAccessDrawBufferEnd(uint32_t drawContextAddr, uint32_t dirtyRectAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveMetalState *ms = priv->metal;
	if (!ms->draw_accessed || !ms->draw_cpu_mac || !GfxGLDeviceMakeCurrent()) return kQANoErr;
	const bool uploaded = upload_guest_to_overlay(ms, dirtyRectAddr);
	ms->draw_accessed = false;
	ms->pass_active = uploaded;
	return uploaded ? kQANoErr : kQAError;
}

int32_t NativeAccessZBuffer(uint32_t drawContextAddr, uint32_t bufferStructAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !bufferStructAddr) return kQAError;
	RaveMetalState *ms = priv->metal;
	if (!ms->pass_active || !ms->depth_rb || !GfxGLDeviceMakeCurrent()) return kQAError;
	uint32_t w = ms->w, h = ms->h;
	uint32_t rowBytes = w * 4;
	uint32_t bufSize = rowBytes * h;
	if (!ms->z_cpu_mac || ms->z_cpu_size != bufSize) {
		uint32 mac = Mac_sysalloc(bufSize);
		if (!mac) return kQAError;
		ms->z_cpu_mac = mac;
		ms->z_cpu_size = bufSize;
	}
	std::vector<float> depth((size_t)w * h);
	auto &ext = gfx_gl_ext();
	if (ext.fbo) ext.BindFramebuffer(GL_FRAMEBUFFER, ms->fbo);
	glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
	for (uint32_t i = 0; i < w * h; i++) {
		uint32 bits;
		std::memcpy(&bits, &depth[i], 4);
		WriteMacInt32(ms->z_cpu_mac + i * 4, bits);
	}
	WriteMacInt32(bufferStructAddr + 0, w);
	WriteMacInt32(bufferStructAddr + 4, h);
	WriteMacInt32(bufferStructAddr + 8, rowBytes);
	WriteMacInt32(bufferStructAddr + 12, ms->z_cpu_mac);
	WriteMacInt32(bufferStructAddr + 16, 32);
	WriteMacInt32(bufferStructAddr + 20, 1); /* big-endian PPC */
	ms->z_accessed = true;
	return kQANoErr;
}

int32_t NativeAccessZBufferEnd(uint32_t drawContextAddr, uint32_t /*dirtyRectAddr*/)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveMetalState *ms = priv->metal;
	if (!ms->z_accessed || !ms->z_cpu_mac || !GfxGLDeviceMakeCurrent()) return kQANoErr;
	uint32_t w = ms->w, h = ms->h;
	std::vector<float> depth((size_t)w * h);
	for (uint32_t i = 0; i < w * h; i++) {
		uint32 bits = ReadMacInt32(ms->z_cpu_mac + i * 4);
		std::memcpy(&depth[i], &bits, 4);
	}
	auto &ext = gfx_gl_ext();
	if (ext.fbo) ext.BindFramebuffer(GL_FRAMEBUFFER, ms->fbo);
	/* Write guest depth into the FBO depth attachment via DrawPixels */
	glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_PIXEL_MODE_BIT | GL_VIEWPORT_BIT);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);
	glViewport(0, 0, (GLsizei)w, (GLsizei)h);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0, (GLdouble)w, 0, (GLdouble)h, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	/* glWindowPos2i if available; else RasterPos with modelview identity */
	typedef void (APIENTRY *PFNGLWINDOWPOS2IPROC)(GLint, GLint);
	static PFNGLWINDOWPOS2IPROC pWinPos = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		pWinPos = (PFNGLWINDOWPOS2IPROC)SDL_GL_GetProcAddress("glWindowPos2i");
		if (!pWinPos)
			pWinPos = (PFNGLWINDOWPOS2IPROC)SDL_GL_GetProcAddress("glWindowPos2iARB");
	}
	if (pWinPos)
		pWinPos(0, 0);
	else
		glRasterPos2i(0, 0);
	glDrawPixels((GLsizei)w, (GLsizei)h, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glPopAttrib();
	ms->z_accessed = false;
	ms->pass_active = true;
	invalidate_draw_state(ms);
	return kQANoErr;
}

int32_t NativeClearDrawBuffer(uint32_t drawContextAddr, uint32_t rectAddr, uint32_t initialContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !priv->metal->pass_active) return kQANoErr;
	if (!GfxGLDeviceMakeCurrent()) return 1;
	invalidate_draw_state(priv->metal);
	float cr = priv->state[2].f, cg = priv->state[3].f,
	      cb = priv->state[4].f, ca = 1.f;
	if (initialContextAddr) {
		uint32_t initHandle = ReadMacInt32(initialContextAddr);
		RaveDrawPrivate *initCtx = RaveGetContext(initHandle);
		if (initCtx) {
			/* Metal path: ColorBG r/g/b at state 2/3/4, force opaque alpha */
			cr = initCtx->state[2].f;
			cg = initCtx->state[3].f;
			cb = initCtx->state[4].f;
			ca = 1.f;
		}
	}
	/* ClearDrawBuffer obeys the same R/G/B/A write mask as Metal. */
	uint32_t channelMask = priv->state[27].i & 0xfu;
	if (!channelMask) channelMask = 0xfu;
	glColorMask((channelMask & 1u) ? GL_TRUE : GL_FALSE,
	            (channelMask & 2u) ? GL_TRUE : GL_FALSE,
	            (channelMask & 4u) ? GL_TRUE : GL_FALSE,
	            (channelMask & 8u) ? GL_TRUE : GL_FALSE);
	glClearColor(cr, cg, cb, ca);
	glDisable(GL_SCISSOR_TEST);
	if (rectAddr) {
		int32_t left = (int32_t)ReadMacInt32(rectAddr + 0);
		int32_t right = (int32_t)ReadMacInt32(rectAddr + 4);
		int32_t top = (int32_t)ReadMacInt32(rectAddr + 8);
		int32_t bottom = (int32_t)ReadMacInt32(rectAddr + 12);
		if (left < 0) left = 0;
		if (top < 0) top = 0;
		if (right > (int32_t)priv->metal->w) right = (int32_t)priv->metal->w;
		if (bottom > (int32_t)priv->metal->h) bottom = (int32_t)priv->metal->h;
		if (right <= left || bottom <= top) return kQANoErr;
		if (right > left && bottom > top) {
			glEnable(GL_SCISSOR_TEST);
			/* FBO: bottom-left origin — RAVE top-left → convert */
			int32_t sy = (int32_t)priv->metal->h - bottom;
			glScissor(left, sy, right - left, bottom - top);
			glClear(GL_COLOR_BUFFER_BIT);
			glDisable(GL_SCISSOR_TEST);
			return kQANoErr;
		}
	}
	glClear(GL_COLOR_BUFFER_BIT);
	return kQANoErr;
}
int32_t NativeClearZBuffer(uint32_t drawContextAddr, uint32_t rectAddr,
	                       uint32_t initialContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !priv->metal->pass_active) return kQANoErr;
	if (!GfxGLDeviceMakeCurrent()) return 1;
	invalidate_draw_state(priv->metal);
	if (!RaveContextUsesMetalDepthAttachment(priv->flags) ||
	    !RaveEffectiveDepthWriteEnabled(
	        priv->state[28].i,
	        priv->ati_state[kRaveATIDepthWriteEnableIndex].i))
		return kQANoErr;
	float clearDepth = 1.f;
	if (initialContextAddr) {
		const uint32_t initHandle = ReadMacInt32(initialContextAddr);
		RaveDrawPrivate *initCtx = RaveGetContext(initHandle);
		if (initCtx && initCtx->state[112].f != 0.f)
			clearDepth = initCtx->state[112].f;
	}
	glClearDepth(RaveClampMetalDepth(clearDepth));
	/* glClear respects the depth write mask; force it on for this operation,
	 * then restore the current RAVE depth policy. */
	glDepthMask(GL_TRUE);
	glDisable(GL_SCISSOR_TEST);
	if (rectAddr) {
		int32_t left = (int32_t)ReadMacInt32(rectAddr + 0);
		int32_t right = (int32_t)ReadMacInt32(rectAddr + 4);
		int32_t top = (int32_t)ReadMacInt32(rectAddr + 8);
		int32_t bottom = (int32_t)ReadMacInt32(rectAddr + 12);
		if (left < 0) left = 0;
		if (top < 0) top = 0;
		if (right > (int32_t)priv->metal->w) right = (int32_t)priv->metal->w;
		if (bottom > (int32_t)priv->metal->h) bottom = (int32_t)priv->metal->h;
		if (right <= left || bottom <= top) {
			apply_depth(priv);
			return kQANoErr;
		}
		if (right > left && bottom > top) {
			glEnable(GL_SCISSOR_TEST);
			int32_t sy = (int32_t)priv->metal->h - bottom;
			glScissor(left, sy, right - left, bottom - top);
			glClear(GL_DEPTH_BUFFER_BIT);
			glDisable(GL_SCISSOR_TEST);
			apply_depth(priv);
			return kQANoErr;
		}
	}
	glClear(GL_DEPTH_BUFFER_BIT);
	apply_depth(priv);
	return kQANoErr;
}
int32_t NativeSwapBuffers(uint32_t ctx, uint32_t dirty) { return NativeRenderEnd(ctx, dirty); }
int32_t NativeBusy(uint32_t) { return 0; }
int32_t NativeTextureNewFromDrawContext(uint32_t drawContextAddr, uint32_t /*flags*/, uint32_t newTexturePtr)
{
	if (newTexturePtr) WriteMacInt32(newTexturePtr, 0);
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !newTexturePtr) return kQAError;
	uint32_t w = (uint32_t)(priv->width > 0 ? priv->width : 640);
	uint32_t h = (uint32_t)(priv->height > 0 ? priv->height : 480);
	uint32_t handle = RaveResourceAlloc(kRaveResourceTexture);
	if (!handle) return kQAError;
	RaveResourceEntry *entry = RaveResourceGet(handle);
	if (!entry) return kQAError;
	if (!GfxGLDeviceMakeCurrent()) { RaveResourceFree(handle); return kQAError; }
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
	entry->metal_texture = (void *)(uintptr_t)tex;
	entry->pixel_type = 4;
	entry->width = w;
	entry->height = h;
	entry->mip_levels = 1;
	entry->row_bytes = w * 4;
	uint32_t cpuMac = Mac_sysalloc(w * h * 4);
	if (cpuMac) {
		entry->cpu_pixel_mac_addr = cpuMac;
		entry->cpu_pixel_data = Mac2HostAddr(cpuMac);
		entry->cpu_pixel_data_size = w * h * 4;
	}
	if (priv->metal) priv->metal->rtt_handles.push_back(handle);
	WriteMacInt32(newTexturePtr, entry->mac_addr);
	return kQANoErr;
}
int32_t NativeBitmapNewFromDrawContext(uint32_t drawContextAddr, uint32_t flags, uint32_t newBitmapPtr)
{
	/* Same as texture RTT for GL path */
	return NativeTextureNewFromDrawContext(drawContextAddr, flags, newBitmapPtr);
}

void *RaveCreateMetalTexture(uint32_t width, uint32_t height, uint32_t /*mipLevels*/,
                             const uint8_t *pixels, uint32_t /*rowBytes*/)
{
	if (!GfxGLDeviceMakeCurrent()) return nullptr;
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	/* Engine expands to BGRA8 before calling us */
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0,
	             GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	return (void *)(uintptr_t)tex;
}

void RaveUploadMipLevel(void *metalTexture, uint32_t level, uint32_t width, uint32_t height,
                        const uint8_t *data, uint32_t /*rowBytes*/)
{
	if (!metalTexture || !data || !GfxGLDeviceMakeCurrent()) return;
	GLuint tex = (GLuint)(uintptr_t)metalTexture;
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0,
	             GL_BGRA, GL_UNSIGNED_BYTE, data);
}

void RaveGenerateMipmaps(void *metalTexture)
{
	if (!metalTexture || !GfxGLDeviceMakeCurrent()) return;
	glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)metalTexture);
	auto &ext = gfx_gl_ext();
	if (ext.GenerateMipmap)
		ext.GenerateMipmap(GL_TEXTURE_2D);
}

void RaveReleaseTexture(void *metalTexture)
{
	if (!metalTexture || !GfxGLDeviceMakeCurrent()) return;
	GLuint tex = (GLuint)(uintptr_t)metalTexture;
	glDeleteTextures(1, &tex);
}

void RaveForgetRTTResourceHandle(uint32_t /*handle*/, uint32_t /*generation*/)
{
	/* OpenGL path does not track RTT tokens yet. */
}

void RaveTextureUploadBatchBegin(void) {}
void RaveTextureUploadBatchEnd(void) { if (GfxGLDeviceMakeCurrent()) glFlush(); }

/*
 * Live pixmap re-upload (Metal implementation lives in rave_metal_renderer.mm).
 * Re-reads Mac pixmap → BGRA → RaveUploadMipLevel so QD3D Interactive Renderer
 * games that rewrite texture memory between frames stay correct.
 */
void RaveRefreshTextureFromPixmap(RaveResourceEntry *entry)
{
	if (!entry || !entry->metal_texture || entry->pixmap_mac_addr == 0) return;

	uint32_t w = entry->width;
	uint32_t h = entry->height;
	uint32_t pixelType = entry->pixel_type;
	uint32_t rowBytes = entry->row_bytes;
	uint32_t pixmap = entry->pixmap_mac_addr;
	if (entry->cpu_pixel_data_is_authoritative &&
	    entry->cpu_pixel_mac_addr != 0 &&
	    entry->cpu_pixel_data_size > 0) {
		pixmap = entry->cpu_pixel_mac_addr;
	}

	std::vector<uint8_t> expanded((size_t)w * h * 4);
	ConvertPixels(pixelType, pixmap, expanded.data(), w, h, rowBytes);
	RaveBGRAImageStats stats = RaveBGRAImageAnalyze(expanded.data(), w * h);
	entry->diag_alpha_zero = (w * h) - stats.alpha;
	entry->diag_rgb_nonzero = stats.rgb;

	if (stats.nonzero != 0) {
		if (pixelType == 4) /* kQAPixel_ARGB32 */
			RaveBGRAWhitenAlphaOnlyMask(expanded.data(), w * h);
		RaveUploadMipLevel(entry->metal_texture, 0, w, h, expanded.data(), w * 4);
		if (entry->mip_levels > 1)
			RaveUploadGeneratedMips(entry->metal_texture, expanded.data(), w, h,
			                        entry->mip_levels);
		if (!entry->pixels_copied) {
			if (entry->cpu_pixel_data && entry->cpu_pixel_mac_addr)
				Host2Mac_memcpy(entry->cpu_pixel_mac_addr, Mac2HostAddr(pixmap),
				                entry->cpu_pixel_data_size);
			entry->pixels_copied = true;
		}
		QD3D_RESOURCE_LOG("TextureRefresh entry=0x%08x native=%p source=0x%08x size=%ux%u mips=%u pixelType=%u nonzero=%u rgb=%u alpha=%u",
		                  entry->mac_addr, entry->metal_texture, pixmap, w, h,
		                  entry->mip_levels, pixelType, stats.nonzero, stats.rgb,
		                  stats.alpha);
	}
}

/*
 *  nqd_gl_renderer.cpp - NQD acceleration via direct host RAM (OpenGL backend)
 *
 *  Metal NQD used shared MTLBuffers over Mac RAM + compute kernels.
 *  On desktop OpenGL the guest RAM is already host-visible, so the kernels
 *  become CPU loops over Mac2HostAddr. API names stay NQDMetal* for glue.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "nqd_accel.h"
#include "main.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

bool nqd_metal_available = false;

#if ACCEL_LOGGING_ENABLED
/* Gate from GFXACCEL_LOG (unset/"all"/list-containing-"nqd" => on), matching
 * the DSp/RAVE/GL subsystems' control plane. Sink is the shared stderr +
 * OutputDebugStringA emitter (gfx_debug_sink.h), always compiled in. */
bool nqd_logging_enabled = accel_log_subsystem_on("nqd");
#define NQD_LOG(...) do { if (nqd_logging_enabled) GFX_DEBUG_EMIT("[nqd] ", __VA_ARGS__); } while (0)
#else
#define NQD_LOG(...) do {} while (0)
#endif
#define NQD_ERR(...) do { GFX_DEBUG_EMIT("[nqd ERROR] ", __VA_ARGS__); } while (0)

extern uint32 RAMBase;
extern uint32 RAMSize;

static uint32 nqd_ram_size = 0;

static inline bool nqd_range_in_buffer(uint64 mac_addr, uint64 length)
{
	const uint64 begin = (uint64)RAMBase;
	const uint64 end = begin + (uint64)nqd_ram_size;
	return length != 0 && mac_addr >= begin && mac_addr < end && length <= end - mac_addr;
}

static inline bool nqd_addr_in_buffer(uint32 mac_addr)
{
	return nqd_range_in_buffer(mac_addr, 1);
}

static bool nqd_surface_range(uint32 base, int32 row_bytes, int x_bytes,
	                         int width_bytes, int y, int height)
{
	if (row_bytes <= 0 || x_bytes < 0 || width_bytes <= 0 || y < 0 || height <= 0)
		return false;
	if ((uint64)x_bytes + (uint64)width_bytes > (uint64)row_bytes)
		return false;
	const uint64 offset = (uint64)y * (uint64)row_bytes + (uint64)x_bytes;
	const uint64 span = (uint64)(height - 1) * (uint64)row_bytes + (uint64)width_bytes;
	return nqd_range_in_buffer((uint64)base + offset, span);
}

static bool nqd_rect_layout(uint32 pixel_size_bits, int x, int width,
	                       int &x_bytes, int &width_bytes)
{
	if (x < 0 || width <= 0)
		return false;
	switch (pixel_size_bits) {
	case 1: case 2: case 4: case 8: case 16: case 32:
		break;
	default:
		return false;
	}
	const uint64 start_bits = (uint64)x * pixel_size_bits;
	const uint64 end_bits = (uint64)(x + width) * pixel_size_bits;
	const uint64 xb = start_bits / 8;
	const uint64 wb = (end_bits + 7) / 8 - xb;
	if (xb > (uint64)std::numeric_limits<int>::max() ||
	    wb > (uint64)std::numeric_limits<int>::max())
		return false;
	x_bytes = (int)xb;
	width_bytes = (int)wb;
	return true;
}

bool NQDMetalAddrInBuffer(uint32 mac_addr)
{
	return nqd_addr_in_buffer(mac_addr);
}

void NQDMetalInit(void)
{
	nqd_ram_size = RAMSize;
	if (nqd_ram_size == 0) {
		NQD_ERR("NQDMetalInit: RAMSize is zero; acceleration disabled");
		nqd_metal_available = false;
		return;
	}
	nqd_metal_available = true;
	NQD_LOG("NQDMetalInit (CPU/OpenGL backend) ram_size=0x%x", nqd_ram_size);
}

void NQDMetalCleanup(void)
{
	nqd_metal_available = false;
}

void NQDMetalFlush(void)
{
	/* CPU path is synchronous. */
}

static inline uint8 *host_ptr(uint32 mac)
{
	return Mac2HostAddr(mac);
}

static inline int bpp_bytes(uint32 pixel_size_bits)
{
	if (pixel_size_bits <= 8) return 1;
	if (pixel_size_bits <= 16) return 2;
	return 4;
}

/* Standard srcCopy / invert / fill for byte-aligned rects. */
static void cpu_copy_rect(uint8 *src, int32 src_rb, uint8 *dst, int32 dst_rb,
                          int width_bytes, int height)
{
	if (!src || !dst || width_bytes <= 0 || height <= 0) return;
	if (src == dst && src_rb == dst_rb) return; /* identity — no-op */

	/* Same-surface overlapping copy (Finder file drags, window scrolls) needs
	 * correct ordering on BOTH axes:
	 *
	 *   - Row axis: if the whole destination block sits below the source
	 *     block and they overlap vertically, copying top-down would overwrite
	 *     source rows before they are read. Iterate bottom-up in that case.
	 *
	 *   - Column axis (within a row): a rightward shift on overlapping rows
	 *     aliases source and destination bytes, so per-row copies MUST use
	 *     memmove, never memcpy. The previous code used memcpy on the
	 *     forward branch, which is undefined on overlap and left horizontal
	 *     streaks trailing a dragged icon. memmove is correct for any
	 *     intra-row overlap direction and costs nothing extra here. */
	const uint8 *src_end = src + (size_t)height * (size_t)std::abs(src_rb);
	const bool copy_rows_backward = (dst > src) && (dst < src_end);
	if (copy_rows_backward) {
		for (int y = height - 1; y >= 0; --y)
			std::memmove(dst + (size_t)y * dst_rb, src + (size_t)y * src_rb, (size_t)width_bytes);
	} else {
		for (int y = 0; y < height; y++)
			std::memmove(dst + (size_t)y * dst_rb, src + (size_t)y * src_rb, (size_t)width_bytes);
	}
}

static void cpu_invert_rect(uint8 *dst, int32 row_bytes, int width_bytes, int height)
{
	for (int y = 0; y < height; y++) {
		uint8 *row = dst + (size_t)y * row_bytes;
		for (int x = 0; x < width_bytes; x++)
			row[x] = (uint8)~row[x];
	}
}

static void cpu_fill_rect(uint8 *dst, int32 row_bytes, int width_bytes, int height, uint32 pattern, int bpp)
{
	for (int y = 0; y < height; y++) {
		uint8 *row = dst + (size_t)y * row_bytes;
		if (bpp == 1) {
			std::memset(row, (int)(pattern & 0xff), (size_t)width_bytes);
		} else if (bpp == 2) {
			for (int x = 0; x < width_bytes; x += 2) {
				row[x] = (uint8)((pattern >> 8) & 0xff);
				row[x + 1] = (uint8)(pattern & 0xff);
			}
		} else {
			for (int x = 0; x < width_bytes; x += 4) {
				row[x + 0] = (uint8)((pattern >> 24) & 0xff);
				row[x + 1] = (uint8)((pattern >> 16) & 0xff);
				row[x + 2] = (uint8)((pattern >> 8) & 0xff);
				row[x + 3] = (uint8)(pattern & 0xff);
			}
		}
	}
}

/* ---- Arithmetic / hilite helpers (IWQD modes 32–39, 50) ---- */

static inline uint32 nqd_read_pix(const uint8 *p, int bpp)
{
	if (bpp == 1) return p[0];
	if (bpp == 2) return ((uint32)p[0] << 8) | p[1];
	return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

static inline void nqd_write_pix(uint8 *p, int bpp, uint32 v)
{
	if (bpp == 1) {
		p[0] = (uint8)v;
	} else if (bpp == 2) {
		p[0] = (uint8)(v >> 8);
		p[1] = (uint8)v;
	} else {
		p[0] = (uint8)(v >> 24);
		p[1] = (uint8)(v >> 16);
		p[2] = (uint8)(v >> 8);
		p[3] = (uint8)v;
	}
}

static uint32 nqd_pack_hilite_color(int bpp)
{
	uint16 r16 = (uint16)ReadMacInt16(0x0DA0);
	uint16 g16 = (uint16)ReadMacInt16(0x0DA2);
	uint16 b16 = (uint16)ReadMacInt16(0x0DA4);
	if (bpp == 1)
		return (r16 >> 8) & 0xFF;
	if (bpp == 2) {
		uint16 r5 = (r16 >> 11) & 0x1F;
		uint16 g5 = (g16 >> 11) & 0x1F;
		uint16 b5 = (b16 >> 11) & 0x1F;
		return (uint32)((1 << 15) | (r5 << 10) | (g5 << 5) | b5);
	}
	uint8 r8 = (uint8)(r16 >> 8), g8 = (uint8)(g16 >> 8), b8 = (uint8)(b16 >> 8);
	return (uint32)((0xFFu << 24) | (r8 << 16) | (g8 << 8) | b8);
}

struct NQDOpColor { uint32 r, g, b; };

/* Safe guest reads for the thePort → GrafVars → rgbOpColor walk (Metal parity). */
static inline bool nqd_inrange(uint32 a)
{
	extern uint32 RAMBase;
	extern uint32 RAMSize;
	const uint32 kLomemTop = 0x3000;
	return (a < kLomemTop) || (a >= RAMBase && a < RAMBase + RAMSize);
}
static inline uint16 nqd_walk_u16(uint32 a) { return (uint16)ReadMacInt16(a); }
static inline uint32 nqd_walk_u32(uint32 a) { return ReadMacInt32(a); }

static NQDOpColor nqd_read_op_color(void)
{
	/* Fallback: lowmem 0x0A28 scalar applied to all channels. */
	uint32 w = (uint32)ReadMacInt16(0x0A28);
	NQDOpColor fallback = { w, w, w };

	/* Step 1: thePort @ lowmem 0x0916 */
	uint32 portPtr = ReadMacInt32(0x0916);
	if (portPtr == 0 || !nqd_inrange(portPtr))
		return fallback;

	/* Step 2: CGrafPort portVersion @ +6 — high two bits mark colour port */
	if (!nqd_inrange(portPtr + 7))
		return fallback;
	uint16 portVersion = nqd_walk_u16(portPtr + 6);
	if ((portVersion & 0xC000) == 0)
		return fallback;

	/* Step 3: grafVars Handle @ CGrafPort + 8 */
	if (!nqd_inrange(portPtr + 11))
		return fallback;
	uint32 grafVarsH = nqd_walk_u32(portPtr + 8);
	if (grafVarsH == 0 || !nqd_inrange(grafVarsH) || !nqd_inrange(grafVarsH + 3))
		return fallback;

	/* Step 4: dereference Handle → GrafVars pointer */
	uint32 grafVarsPtr = nqd_walk_u32(grafVarsH);
	if (grafVarsPtr == 0 || !nqd_inrange(grafVarsPtr))
		return fallback;

	/* Step 5: rgbOpColor RGBColor @ GrafVars + 0 (6 bytes) */
	if (!nqd_inrange(grafVarsPtr + 5))
		return fallback;
	NQDOpColor out;
	out.r = (uint32)nqd_walk_u16(grafVarsPtr + 0);
	out.g = (uint32)nqd_walk_u16(grafVarsPtr + 2);
	out.b = (uint32)nqd_walk_u16(grafVarsPtr + 4);
	return out;
}

static inline void nqd_unpack_rgb(uint32 pix, int bpp, uint32 &r, uint32 &g, uint32 &b, uint32 &cmax)
{
	if (bpp == 1) {
		r = g = b = pix & 0xFF;
		cmax = 255;
	} else if (bpp == 2) {
		r = (pix >> 10) & 0x1F;
		g = (pix >> 5) & 0x1F;
		b = pix & 0x1F;
		cmax = 31;
	} else {
		r = (pix >> 16) & 0xFF;
		g = (pix >> 8) & 0xFF;
		b = pix & 0xFF;
		cmax = 255;
	}
}

static inline uint32 nqd_pack_rgb(int bpp, uint32 r, uint32 g, uint32 b, uint32 a_or_high)
{
	if (bpp == 1)
		return r & 0xFF;
	if (bpp == 2)
		return (1u << 15) | ((r & 0x1F) << 10) | ((g & 0x1F) << 5) | (b & 0x1F);
	return ((a_or_high & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

static uint32 nqd_arith_pixel(uint32 mode, uint32 sp, uint32 dp, int bpp,
                              uint32 back_pen, uint32 hilite, const NQDOpColor &op)
{
	/* transparent (36): skip write when src == background */
	if (mode == 36) {
		if (sp == back_pen) return dp;
		return sp;
	}
	/* hilite (50): if dst == background replace with hilite color */
	if (mode == 50) {
		if (dp == back_pen) return hilite;
		return dp;
	}

	uint32 sr, sg, sb, dr, dg, db, cmax;
	nqd_unpack_rgb(sp, bpp, sr, sg, sb, cmax);
	nqd_unpack_rgb(dp, bpp, dr, dg, db, cmax);
	uint32 a = (bpp == 4) ? ((sp >> 24) & 0xFF) : 0xFF;
	uint32 or_r, or_g, or_b;

	switch (mode) {
	case 32: { /* blend weighted by OpColor [0..65535] */
		uint32 wr = op.r, wg = op.g, wb = op.b;
		or_r = (sr * wr + dr * (65535u - wr)) / 65535u;
		or_g = (sg * wg + dg * (65535u - wg)) / 65535u;
		or_b = (sb * wb + db * (65535u - wb)) / 65535u;
		break;
	}
	case 33: /* addPin — saturating add */
		or_r = std::min(cmax, sr + dr);
		or_g = std::min(cmax, sg + dg);
		or_b = std::min(cmax, sb + db);
		break;
	case 34: /* addOver — modular wrap */
		or_r = (sr + dr) & cmax;
		or_g = (sg + dg) & cmax;
		or_b = (sb + db) & cmax;
		break;
	case 35: /* subPin — saturating subtract */
		or_r = (sr > dr) ? (sr - dr) : 0;
		or_g = (sg > dg) ? (sg - dg) : 0;
		or_b = (sb > db) ? (sb - db) : 0;
		break;
	case 37: /* adMax */
		or_r = std::max(sr, dr);
		or_g = std::max(sg, dg);
		or_b = std::max(sb, db);
		break;
	case 38: /* subOver — modular subtract */
		or_r = (sr - dr) & cmax;
		or_g = (sg - dg) & cmax;
		or_b = (sb - db) & cmax;
		break;
	case 39: /* adMin */
		or_r = std::min(sr, dr);
		or_g = std::min(sg, dg);
		or_b = std::min(sb, db);
		break;
	default:
		return sp;
	}
	return nqd_pack_rgb(bpp, or_r, or_g, or_b, a);
}

static void cpu_arith_blit(uint8 *src, int32 src_rb, uint8 *dst, int32 dst_rb,
                           int w, int h, int bpp, uint32 mode,
                           uint32 back_pen, uint32 hilite, const NQDOpColor &op)
{
	for (int y = 0; y < h; y++) {
		uint8 *s = src + (size_t)y * src_rb;
		uint8 *d = dst + (size_t)y * dst_rb;
		for (int x = 0; x < w; x++) {
			uint32 sp = nqd_read_pix(s + x * bpp, bpp);
			uint32 dp = nqd_read_pix(d + x * bpp, bpp);
			uint32 out = nqd_arith_pixel(mode, sp, dp, bpp, back_pen, hilite, op);
			if (mode == 36 && sp == back_pen)
				continue;
			nqd_write_pix(d + x * bpp, bpp, out);
		}
	}
}

static void cpu_arith_fill(uint8 *dst, int32 dst_rb, int w, int h, int bpp,
                           uint32 mode, uint32 pen, uint32 back_pen, uint32 hilite,
                           const NQDOpColor &op)
{
	for (int y = 0; y < h; y++) {
		uint8 *d = dst + (size_t)y * dst_rb;
		for (int x = 0; x < w; x++) {
			uint32 dp = nqd_read_pix(d + x * bpp, bpp);
			uint32 out = nqd_arith_pixel(mode, pen, dp, bpp, back_pen, hilite, op);
			if (mode == 36 && pen == back_pen)
				continue;
			nqd_write_pix(d + x * bpp, bpp, out);
		}
	}
}

static bool decode_rect(uint32 p, bool has_src,
                        int &sx, int &sy, int &dx, int &dy, int &w, int &h,
                        uint32 &src_base, int32 &src_rb,
                        uint32 &dst_base, int32 &dst_rb,
                        uint32 &src_ps, uint32 &dst_ps, uint32 &mode)
{
	const int dst_bounds_left = (int16)ReadMacInt16(p + NQD_acclDestBoundsRect + 2);
	const int dst_bounds_top = (int16)ReadMacInt16(p + NQD_acclDestBoundsRect + 0);
	const int dst_bounds_width = (int16)ReadMacInt16(p + NQD_acclDestBoundsRect + 6) - dst_bounds_left;
	const int dst_bounds_height = (int16)ReadMacInt16(p + NQD_acclDestBoundsRect + 4) - dst_bounds_top;
	dx = (int16)ReadMacInt16(p + NQD_acclDestRect + 2) - dst_bounds_left;
	dy = (int16)ReadMacInt16(p + NQD_acclDestRect + 0) - dst_bounds_top;
	w  = (int16)ReadMacInt16(p + NQD_acclDestRect + 6) - (int16)ReadMacInt16(p + NQD_acclDestRect + 2);
	h  = (int16)ReadMacInt16(p + NQD_acclDestRect + 4) - (int16)ReadMacInt16(p + NQD_acclDestRect + 0);
	if (w <= 0 || h <= 0 || dst_bounds_width <= 0 || dst_bounds_height <= 0) {
		NQD_LOG("decode_rect DROP (dst-degenerate) p=%08x w=%d h=%d dbw=%d dbh=%d",
			p, w, h, dst_bounds_width, dst_bounds_height);
		return false;
	}

	dst_base = ReadMacInt32(p + NQD_acclDestBaseAddr);
	dst_rb = (int32)ReadMacInt32(p + NQD_acclDestRowBytes);
	dst_ps = ReadMacInt32(p + NQD_acclDestPixelSize);
	mode = ReadMacInt32(p + NQD_acclTransferMode);

	if (has_src) {
		const int src_bounds_left = (int16)ReadMacInt16(p + NQD_acclSrcBoundsRect + 2);
		const int src_bounds_top = (int16)ReadMacInt16(p + NQD_acclSrcBoundsRect + 0);
		const int src_bounds_width = (int16)ReadMacInt16(p + NQD_acclSrcBoundsRect + 6) - src_bounds_left;
		const int src_bounds_height = (int16)ReadMacInt16(p + NQD_acclSrcBoundsRect + 4) - src_bounds_top;
		if (src_bounds_width <= 0 || src_bounds_height <= 0) {
			NQD_LOG("decode_rect DROP (src-bounds-degenerate) p=%08x sbw=%d sbh=%d",
				p, src_bounds_width, src_bounds_height);
			return false;
		}
		sx = (int16)ReadMacInt16(p + NQD_acclSrcRect + 2) - src_bounds_left;
		sy = (int16)ReadMacInt16(p + NQD_acclSrcRect + 0) - src_bounds_top;
		src_base = ReadMacInt32(p + NQD_acclSrcBaseAddr);
		src_rb = (int32)ReadMacInt32(p + NQD_acclSrcRowBytes);
		src_ps = ReadMacInt32(p + NQD_acclSrcPixelSize);

		if (sx < 0) { const int trim = -sx; sx = 0; dx += trim; w -= trim; }
		if (sy < 0) { const int trim = -sy; sy = 0; dy += trim; h -= trim; }
		if (w <= 0 || h <= 0 || sx >= src_bounds_width || sy >= src_bounds_height) {
			NQD_LOG("decode_rect DROP (src-clip-empty) p=%08x sx=%d sy=%d w=%d h=%d sbw=%d sbh=%d",
				p, sx, sy, w, h, src_bounds_width, src_bounds_height);
			return false;
		}
		w = std::min(w, src_bounds_width - sx);
		h = std::min(h, src_bounds_height - sy);
	} else {
		sx = sy = 0;
		src_base = 0;
		src_rb = 0;
		src_ps = dst_ps;
	}

	if (dx < 0) { const int trim = -dx; dx = 0; sx += trim; w -= trim; }
	if (dy < 0) { const int trim = -dy; dy = 0; sy += trim; h -= trim; }
	if (w <= 0 || h <= 0 || dx >= dst_bounds_width || dy >= dst_bounds_height) {
		NQD_LOG("decode_rect DROP (dst-clip-empty) p=%08x dx=%d dy=%d w=%d h=%d dbw=%d dbh=%d",
			p, dx, dy, w, h, dst_bounds_width, dst_bounds_height);
		return false;
	}
	w = std::min(w, dst_bounds_width - dx);
	h = std::min(h, dst_bounds_height - dy);
	if (w <= 0 || h <= 0 || dst_rb <= 0 || (has_src && src_rb <= 0)) {
		NQD_LOG("decode_rect DROP (rowbytes/size) p=%08x w=%d h=%d drb=%d srb=%d",
			p, w, h, (int)dst_rb, (int)src_rb);
		return false;
	}
	int ignored_x, ignored_width;
	if (!nqd_rect_layout(dst_ps, dx, w, ignored_x, ignored_width)) {
		NQD_LOG("decode_rect DROP (dst-layout) p=%08x dst_ps=%u dx=%d w=%d", p, dst_ps, dx, w);
		return false;
	}
	if (has_src && (!nqd_rect_layout(src_ps, sx, w, ignored_x, ignored_width) || src_ps != dst_ps)) {
		NQD_LOG("decode_rect DROP (src-layout/depth) p=%08x src_ps=%u dst_ps=%u sx=%d w=%d",
			p, src_ps, dst_ps, sx, w);
		return false;
	}
	return true;
}

bool NQDMetalBitbltSameSurfaceOverlap(uint32 p)
{
	int sx, sy, dx, dy, w, h;
	uint32 sb, db, sps, dps, mode;
	int32 srb, drb;
	if (!decode_rect(p, true, sx, sy, dx, dy, w, h, sb, srb, db, drb, sps, dps, mode))
		return false;
	if (sb != db || srb != drb || sps != dps) return false;
	if (sx == dx && sy == dy) return false;
	return dx < sx + w && sx < dx + w && dy < sy + h && sy < dy + h;
}

void NQDMetalBitblt(uint32 p)
{
	if (!nqd_metal_available) return;
	int sx, sy, dx, dy, w, h;
	uint32 sb, db, sps, dps, mode;
	int32 srb, drb;
	if (!decode_rect(p, true, sx, sy, dx, dy, w, h, sb, srb, db, drb, sps, dps, mode)) {
		// Hook already committed this blit to the accelerated proc; a drop here
		// leaves stale destination pixels (Finder drag streaks). See decode_rect
		// NQD_LOG lines for the specific check that failed.
		NQD_LOG("NQDMetalBitblt DROP (decode_rect) p=%08x -> stale pixels possible", p);
		return;
	}

	int bpp = bpp_bytes(dps);
	int width_bytes = w * bpp;
	/* For sub-8bpp treat width in packed bytes approximately */
	if (dps < 8) {
		width_bytes = (w * (int)dps + 7) / 8;
		bpp = 1;
	}
	int src_x_bytes, src_layout_width, dst_x_bytes, dst_layout_width;
	if (!nqd_rect_layout(sps, sx, w, src_x_bytes, src_layout_width) ||
	    !nqd_rect_layout(dps, dx, w, dst_x_bytes, dst_layout_width) ||
	    !nqd_surface_range(sb, srb, src_x_bytes, width_bytes, sy, h) ||
	    !nqd_surface_range(db, drb, dst_x_bytes, width_bytes, dy, h)) {
		// Committed-then-dropped: leaves stale destination pixels (drag streaks).
		NQD_LOG("NQDMetalBitblt DROP (range/layout) p=%08x sb=%08x db=%08x sx=%d sy=%d "
			"dx=%d dy=%d w=%d h=%d srb=%d drb=%d sps=%u dps=%u -> stale pixels possible",
			p, sb, db, sx, sy, dx, dy, w, h, (int)srb, (int)drb, sps, dps);
		return;
	}

	uint8 *src = host_ptr(sb) + (size_t)sy * srb + src_x_bytes;
	uint8 *dst = host_ptr(db) + (size_t)dy * drb + dst_x_bytes;
	/* Boolean and arithmetic loops read and write sequentially. Snapshot an
	 * overlapping source rectangle so a downward/rightward copy cannot feed
	 * already-written destination pixels back into later source reads. */
	std::vector<uint8> overlap_scratch;
	const uintptr_t src_begin = (uintptr_t)src;
	const uintptr_t src_end = src_begin + (size_t)(h - 1) * srb + width_bytes;
	const uintptr_t dst_begin = (uintptr_t)dst;
	const uintptr_t dst_end = dst_begin + (size_t)(h - 1) * drb + width_bytes;
	if (src_begin < dst_end && dst_begin < src_end &&
	    !(src_begin == dst_begin && srb == drb)) {
		overlap_scratch.resize((size_t)width_bytes * h);
		for (int y = 0; y < h; y++)
			std::memcpy(overlap_scratch.data() + (size_t)y * width_bytes,
			            src + (size_t)y * srb, (size_t)width_bytes);
		src = overlap_scratch.data();
		srb = width_bytes;
	}

	/* Arithmetic 32–39 / hilite 50: per-pixel ops (standard depths only). */
	if ((mode >= 32 && mode <= 39) || mode == 50) {
		if (dps >= 8 && sps == dps) {
			uint32 back = ReadMacInt32(p + NQD_acclBackPen);
			uint32 hilite = (mode == 50) ? nqd_pack_hilite_color(bpp) : 0;
			NQDOpColor op = {};
			if (mode == 32)
				op = nqd_read_op_color();
			cpu_arith_blit(src, srb, dst, drb, w, h, bpp, mode, back, hilite, op);
			return;
		}
		/* Packed sub-8bpp: IWQD Table 4-2 maps arithmetic to Boolean */
		uint32 bmap = 0;
		switch (mode) {
		case 32: bmap = 0; break; /* blend -> srcCopy */
		case 33: case 37: bmap = 1; break; /* addPin/adMax -> srcOr */
		case 34: case 38: case 50: bmap = 2; break; /* addOver/subOver/hilite -> srcXor */
		case 35: case 36: case 39: bmap = 1; break; /* subPin/transparent/adMin -> srcOr */
		default: bmap = 0; break;
		}
		mode = bmap;
	}

	/* Boolean modes 0–7 (src) and 8–15 (pat) share the same low 3 bits. */
	uint32 bool_mode = mode;
	if (bool_mode >= 8 && bool_mode <= 15)
		bool_mode -= 8;

	for (int y = 0; y < h; y++) {
		uint8 *s = src + (size_t)y * srb;
		uint8 *d = dst + (size_t)y * drb;
		switch (bool_mode) {
		case 0: /* srcCopy */
			std::memcpy(d, s, (size_t)width_bytes);
			break;
		case 1: /* srcOr */
			for (int i = 0; i < width_bytes; i++) d[i] = (uint8)(s[i] | d[i]);
			break;
		case 2: /* srcXor */
			for (int i = 0; i < width_bytes; i++) d[i] = (uint8)(s[i] ^ d[i]);
			break;
		case 3: /* srcBic */
			for (int i = 0; i < width_bytes; i++) d[i] = (uint8)(d[i] & ~s[i]);
			break;
		case 4: /* notSrcCopy */
			for (int i = 0; i < width_bytes; i++) d[i] = (uint8)~s[i];
			break;
		case 5: /* notSrcOr */
			for (int i = 0; i < width_bytes; i++) d[i] = (uint8)((~s[i]) | d[i]);
			break;
		case 6: /* notSrcXor */
			for (int i = 0; i < width_bytes; i++) d[i] = (uint8)((~s[i]) ^ d[i]);
			break;
		case 7: /* notSrcBic */
			for (int i = 0; i < width_bytes; i++) d[i] = (uint8)(d[i] & s[i]);
			break;
		default:
			std::memcpy(d, s, (size_t)width_bytes);
			break;
		}
	}
}

void NQDMetalFillRect(uint32 p)
{
	if (!nqd_metal_available) return;
	int sx, sy, dx, dy, w, h;
	uint32 sb, db, sps, dps, mode;
	int32 srb, drb;
	if (!decode_rect(p, false, sx, sy, dx, dy, w, h, sb, srb, db, drb, sps, dps, mode))
		return;

	int bpp = bpp_bytes(dps);
	int width_bytes = (dps < 8) ? (w * (int)dps + 7) / 8 : w * bpp;
	int dst_x_bytes, dst_layout_width;
	if (!nqd_rect_layout(dps, dx, w, dst_x_bytes, dst_layout_width) ||
	    !nqd_surface_range(db, drb, dst_x_bytes, width_bytes, dy, h)) {
		NQD_LOG("NQDMetalFillRect DROP (range/layout) p=%08x db=%08x dx=%d dy=%d w=%d h=%d "
			"drb=%d dps=%u -> stale pixels possible", p, db, dx, dy, w, h, (int)drb, dps);
		return;
	}
	/*
	 * Color selection matches stock gfxaccel.cpp / PocketShaver:
	 *   penMode == 8 (patCopy) → ForePen, else → BackPen.
	 * Using Fore always paints erase/white fills as black (typical
	 * Fore=black, Back=white) — black Finder windows, dark chrome, etc.
	 */
	const uint32 pen_mode = ReadMacInt32(p + NQD_acclPenMode);
	const uint32 fore_pen = ReadMacInt32(p + NQD_acclForePen);
	const uint32 back_pen = ReadMacInt32(p + NQD_acclBackPen);
	const uint32 pen = (pen_mode == 8) ? fore_pen : back_pen;
	uint8 *dst = host_ptr(db) + (size_t)dy * drb + dst_x_bytes;
	int pb = dps < 8 ? 1 : bpp;
	/* Arithmetic / hilite pen modes on standard depths */
	if (((mode >= 32 && mode <= 39) || mode == 50) && dps >= 8) {
		uint32 hilite = (mode == 50) ? nqd_pack_hilite_color(pb) : 0;
		NQDOpColor op = (mode == 32) ? nqd_read_op_color() : NQDOpColor{0, 0, 0};
		cpu_arith_fill(dst, drb, w, h, pb, mode, fore_pen, back_pen, hilite, op);
		return;
	}

	/* Transfer modes 8–15: solid pen as pattern (patCopy etc.) */
	uint32 m = mode & 7;
	if (mode >= 8 && mode <= 15) {
		/* Build a solid pattern row from the selected pen color */
		std::vector<uint8> pat((size_t)width_bytes);
		if (pb == 1) {
			std::memset(pat.data(), (int)(pen & 0xff), (size_t)width_bytes);
		} else if (pb == 2) {
			for (int x = 0; x < width_bytes; x += 2) {
				pat[x] = (uint8)((pen >> 8) & 0xff);
				pat[x + 1] = (uint8)(pen & 0xff);
			}
		} else {
			for (int x = 0; x < width_bytes; x += 4) {
				pat[x + 0] = (uint8)((pen >> 24) & 0xff);
				pat[x + 1] = (uint8)((pen >> 16) & 0xff);
				pat[x + 2] = (uint8)((pen >> 8) & 0xff);
				pat[x + 3] = (uint8)(pen & 0xff);
			}
		}
		for (int y = 0; y < h; y++) {
			uint8 *d = dst + (size_t)y * drb;
			uint8 *s = pat.data();
			switch (m) {
			case 0: std::memcpy(d, s, (size_t)width_bytes); break;
			case 1: for (int i = 0; i < width_bytes; i++) d[i] |= s[i]; break;
			case 2: for (int i = 0; i < width_bytes; i++) d[i] ^= s[i]; break;
			case 3: for (int i = 0; i < width_bytes; i++) d[i] &= (uint8)~s[i]; break;
			case 4: for (int i = 0; i < width_bytes; i++) d[i] = (uint8)~s[i]; break;
			case 5: for (int i = 0; i < width_bytes; i++) d[i] = (uint8)((~s[i]) | d[i]); break;
			case 6: for (int i = 0; i < width_bytes; i++) d[i] = (uint8)((~s[i]) ^ d[i]); break;
			case 7: for (int i = 0; i < width_bytes; i++) d[i] &= s[i]; break;
			}
		}
	} else {
		cpu_fill_rect(dst, drb, width_bytes, h, pen, pb);
	}
}

void NQDMetalInvertRect(uint32 p)
{
	if (!nqd_metal_available) return;
	int sx, sy, dx, dy, w, h;
	uint32 sb, db, sps, dps, mode;
	int32 srb, drb;
	if (!decode_rect(p, false, sx, sy, dx, dy, w, h, sb, srb, db, drb, sps, dps, mode))
		return;
	int bpp = bpp_bytes(dps);
	int width_bytes = (dps < 8) ? (w * (int)dps + 7) / 8 : w * bpp;
	int dst_x_bytes, dst_layout_width;
	if (!nqd_rect_layout(dps, dx, w, dst_x_bytes, dst_layout_width) ||
	    !nqd_surface_range(db, drb, dst_x_bytes, width_bytes, dy, h)) {
		NQD_LOG("NQDMetalInvertRect DROP (range/layout) p=%08x db=%08x dx=%d dy=%d w=%d h=%d "
			"drb=%d dps=%u -> stale pixels possible", p, db, dx, dy, w, h, (int)drb, dps);
		return;
	}
	uint8 *dst = host_ptr(db) + (size_t)dy * drb + dst_x_bytes;
	cpu_invert_rect(dst, drb, width_bytes, h);
}

void NQDMetalBltMask(uint32 p)
{
	if (!nqd_metal_available) return;
	int sx, sy, dx, dy, w, h;
	uint32 sb, db, sps, dps, mode;
	int32 srb, drb;
	if (!decode_rect(p, true, sx, sy, dx, dy, w, h, sb, srb, db, drb, sps, dps, mode))
		return;
	uint32 mask_addr = ReadMacInt32(p + NQD_acclMaskAddr);
	if (!mask_addr) {
		NQDMetalBitblt(p);
		return;
	}
	int bpp = bpp_bytes(dps);
	if (dps < 8) { NQDMetalBitblt(p); return; }
	/* 1-bit mask: one bit per destination pixel, row-padded to bytes */
	int mask_rb = (w + 7) / 8;
	int src_x_bytes, src_width_bytes, dst_x_bytes, dst_width_bytes;
	if (!nqd_rect_layout(sps, sx, w, src_x_bytes, src_width_bytes) ||
	    !nqd_rect_layout(dps, dx, w, dst_x_bytes, dst_width_bytes) ||
	    !nqd_surface_range(sb, srb, src_x_bytes, src_width_bytes, sy, h) ||
	    !nqd_surface_range(db, drb, dst_x_bytes, dst_width_bytes, dy, h) ||
	    !nqd_range_in_buffer(mask_addr, (uint64)mask_rb * (uint64)h)) {
		NQD_LOG("NQDMetalBltMask DROP (range/layout/mask) p=%08x sb=%08x db=%08x mask=%08x "
			"dx=%d dy=%d w=%d h=%d -> stale pixels possible", p, sb, db, mask_addr, dx, dy, w, h);
		return;
	}
	uint8 *src = host_ptr(sb) + (size_t)sy * srb + src_x_bytes;
	uint8 *dst = host_ptr(db) + (size_t)dy * drb + dst_x_bytes;
	uint8 *mask = host_ptr(mask_addr);
	for (int y = 0; y < h; y++) {
		uint8 *s = src + (size_t)y * srb;
		uint8 *d = dst + (size_t)y * drb;
		uint8 *m = mask + (size_t)y * mask_rb;
		for (int x = 0; x < w; x++) {
			if (!(m[x >> 3] & (0x80 >> (x & 7))))
				continue;
			if (bpp == 1) d[x] = s[x];
			else if (bpp == 2) { d[x * 2] = s[x * 2]; d[x * 2 + 1] = s[x * 2 + 1]; }
			else {
				d[x * 4 + 0] = s[x * 4 + 0]; d[x * 4 + 1] = s[x * 4 + 1];
				d[x * 4 + 2] = s[x * 4 + 2]; d[x * 4 + 3] = s[x * 4 + 3];
			}
		}
	}
}

void NQDMetalFillMask(uint32 p)
{
	if (!nqd_metal_available) return;
	int sx, sy, dx, dy, w, h;
	uint32 sb, db, sps, dps, mode;
	int32 srb, drb;
	if (!decode_rect(p, false, sx, sy, dx, dy, w, h, sb, srb, db, drb, sps, dps, mode))
		return;
	uint32 mask_addr = ReadMacInt32(p + NQD_acclMaskAddr);
	if (!mask_addr) {
		NQDMetalFillRect(p);
		return;
	}
	int bpp = bpp_bytes(dps);
	if (dps < 8) { NQDMetalFillRect(p); return; }
	uint32 pen = ReadMacInt32(p + NQD_acclForePen);
	int mask_rb = (w + 7) / 8;
	int dst_x_bytes, dst_width_bytes;
	if (!nqd_rect_layout(dps, dx, w, dst_x_bytes, dst_width_bytes) ||
	    !nqd_surface_range(db, drb, dst_x_bytes, dst_width_bytes, dy, h) ||
	    !nqd_range_in_buffer(mask_addr, (uint64)mask_rb * (uint64)h)) {
		NQD_LOG("NQDMetalFillMask DROP (range/layout/mask) p=%08x db=%08x mask=%08x "
			"dx=%d dy=%d w=%d h=%d -> stale pixels possible", p, db, mask_addr, dx, dy, w, h);
		return;
	}
	uint8 *dst = host_ptr(db) + (size_t)dy * drb + dst_x_bytes;
	uint8 *mask = host_ptr(mask_addr);
	for (int y = 0; y < h; y++) {
		uint8 *d = dst + (size_t)y * drb;
		uint8 *m = mask + (size_t)y * mask_rb;
		for (int x = 0; x < w; x++) {
			if (!(m[x >> 3] & (0x80 >> (x & 7))))
				continue;
			nqd_write_pix(d + x * bpp, bpp, pen);
		}
	}
}

bool NQDMetalBitblt1to1(uint32 src_base, int32 src_row_bytes,
                        uint32 dst_base, int32 dst_row_bytes,
                        uint32 pixel_size_bytes, uint32 /*bits_per_pixel*/,
                        uint32 width_pixels, uint32 height,
                        uint32 transfer_mode, uint32 src_key)
{
	if (!nqd_metal_available) return false;
	if (width_pixels == 0 || height == 0 ||
	    (pixel_size_bytes != 1 && pixel_size_bytes != 2 && pixel_size_bytes != 4) ||
	    src_row_bytes <= 0 || dst_row_bytes <= 0 ||
	    height > (uint32)std::numeric_limits<int>::max()) return false;
	const uint64 width_bytes_64 = (uint64)width_pixels * pixel_size_bytes;
	if (width_bytes_64 > (uint64)std::numeric_limits<int>::max()) return false;
	const int width_bytes = (int)width_bytes_64;
	if (!nqd_surface_range(src_base, src_row_bytes, 0, width_bytes, 0, (int)height) ||
	    !nqd_surface_range(dst_base, dst_row_bytes, 0, width_bytes, 0, (int)height)) return false;

	uint8 *src = host_ptr(src_base);
	uint8 *dst = host_ptr(dst_base);

	if (transfer_mode == 0) {
		cpu_copy_rect(src, src_row_bytes, dst, dst_row_bytes, width_bytes, (int)height);
		return true;
	}

	/* transparent: skip pixels matching src_key */
	for (uint32 y = 0; y < height; y++) {
		uint8 *srow = src + (size_t)y * src_row_bytes;
		uint8 *drow = dst + (size_t)y * dst_row_bytes;
		for (uint32 x = 0; x < width_pixels; x++) {
			uint32 pix = 0;
			if (pixel_size_bytes == 1) pix = srow[x];
			else if (pixel_size_bytes == 2) pix = (srow[x * 2] << 8) | srow[x * 2 + 1];
			else pix = (srow[x * 4] << 24) | (srow[x * 4 + 1] << 16) | (srow[x * 4 + 2] << 8) | srow[x * 4 + 3];
			if (pix == src_key) continue;
			if (pixel_size_bytes == 1) drow[x] = (uint8)pix;
			else if (pixel_size_bytes == 2) {
				drow[x * 2] = (uint8)(pix >> 8);
				drow[x * 2 + 1] = (uint8)pix;
			} else {
				drow[x * 4] = (uint8)(pix >> 24);
				drow[x * 4 + 1] = (uint8)(pix >> 16);
				drow[x * 4 + 2] = (uint8)(pix >> 8);
				drow[x * 4 + 3] = (uint8)pix;
			}
		}
	}
	return true;
}

bool NQDMetalBitbltScaled(uint32 src_base, int32 src_row_bytes,
                          uint32 dst_base, int32 dst_row_bytes,
                          uint32 pixel_size_bytes, uint32 /*bits_per_pixel*/,
                          uint32 src_w, uint32 src_h,
                          uint32 dst_w, uint32 dst_h,
                          uint32 /*interpolate*/,
                          uint32 src_key, uint32 /*dst_key*/,
                          uint32 key_enable)
{
	if (!nqd_metal_available) return false;
	if (!src_w || !src_h || !dst_w || !dst_h ||
	    (pixel_size_bytes != 1 && pixel_size_bytes != 2 && pixel_size_bytes != 4) ||
	    src_row_bytes <= 0 || dst_row_bytes <= 0 ||
	    src_h > (uint32)std::numeric_limits<int>::max() ||
	    dst_h > (uint32)std::numeric_limits<int>::max()) return false;
	const uint64 src_width_bytes = (uint64)src_w * pixel_size_bytes;
	const uint64 dst_width_bytes = (uint64)dst_w * pixel_size_bytes;
	if (src_width_bytes > (uint64)std::numeric_limits<int>::max() ||
	    dst_width_bytes > (uint64)std::numeric_limits<int>::max()) return false;
	if (!nqd_surface_range(src_base, src_row_bytes, 0, (int)src_width_bytes, 0, (int)src_h) ||
	    !nqd_surface_range(dst_base, dst_row_bytes, 0, (int)dst_width_bytes, 0, (int)dst_h)) return false;

	uint8 *src = host_ptr(src_base);
	uint8 *dst = host_ptr(dst_base);

	for (uint32 y = 0; y < dst_h; y++) {
		uint32 sy = (uint32)((uint64)y * src_h / dst_h);
		uint8 *srow = src + (size_t)sy * src_row_bytes;
		uint8 *drow = dst + (size_t)y * dst_row_bytes;
		for (uint32 x = 0; x < dst_w; x++) {
			uint32 sx = (uint32)((uint64)x * src_w / dst_w);
			uint32 pix = 0;
			if (pixel_size_bytes == 1) pix = srow[sx];
			else if (pixel_size_bytes == 2) pix = (srow[sx * 2] << 8) | srow[sx * 2 + 1];
			else pix = (srow[sx * 4] << 24) | (srow[sx * 4 + 1] << 16) | (srow[sx * 4 + 2] << 8) | srow[sx * 4 + 3];
			if (key_enable && pix == src_key) continue;
			if (pixel_size_bytes == 1) drow[x] = (uint8)pix;
			else if (pixel_size_bytes == 2) {
				drow[x * 2] = (uint8)(pix >> 8);
				drow[x * 2 + 1] = (uint8)pix;
			} else {
				drow[x * 4 + 0] = (uint8)(pix >> 24);
				drow[x * 4 + 1] = (uint8)(pix >> 16);
				drow[x * 4 + 2] = (uint8)(pix >> 8);
				drow[x * 4 + 3] = (uint8)pix;
			}
		}
	}
	return true;
}

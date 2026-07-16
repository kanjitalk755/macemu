/*
 *  gfxaccel.cpp - Generic Native QuickDraw acceleration
 *
 *  SheepShaver (C) 1997-2008 Marc Hellwig and Christian Bauer
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

#include "prefs.h"
#include "video.h"
#include "video_defs.h"
#include "rave_engine.h"
#include "gl_engine.h"
#include "dsp_engine.h"
#include "nqd_accel.h"

#define DEBUG 0
#include "debug.h"


/*
 *	Utility functions
 */

// Return bytes per pixel for requested depth
static inline int bytes_per_pixel(int depth)
{
	int bpp;
	switch (depth) {
	case 8:
		bpp = 1;
		break;
	case 15: case 16:
		bpp = 2;
		break;
	case 24: case 32:
		bpp = 4;
		break;
	default:
		abort();
	}
	return bpp;
}

static inline bool NQD_packed_depth_supported(uint32 pixel_size)
{
	return pixel_size == 1 || pixel_size == 2 || pixel_size == 4;
}

static inline bool NQD_packed_x_byte_aligned(int32 x, uint32 pixel_size)
{
	if (pixel_size >= 8)
		return true;
	if (!NQD_packed_depth_supported(pixel_size))
		return false;
	return ((x * (int32)pixel_size) & 7) == 0;
}

static inline bool NQD_packed_rect_edges_byte_aligned(int32 x, int32 width, uint32 pixel_size)
{
	return NQD_packed_x_byte_aligned(x, pixel_size) &&
	       NQD_packed_x_byte_aligned(x + width, pixel_size);
}

static inline bool NQD_dest_rect_byte_aligned_for_depth(uint32 p, uint32 pixel_size)
{
	if (pixel_size >= 8)
		return true;
	int32 dest_X = (int16)ReadMacInt16(p + acclDestRect + 2) - (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
	int32 width  = (int16)ReadMacInt16(p + acclDestRect + 6) - (int16)ReadMacInt16(p + acclDestRect + 2);
	return NQD_packed_rect_edges_byte_aligned(dest_X, width, pixel_size);
}

static inline bool NQD_bitblt_rects_byte_aligned_for_depth(uint32 p, uint32 pixel_size)
{
	if (pixel_size >= 8)
		return true;
	int32 src_X  = (int16)ReadMacInt16(p + acclSrcRect + 2) - (int16)ReadMacInt16(p + acclSrcBoundsRect + 2);
	int32 dest_X = (int16)ReadMacInt16(p + acclDestRect + 2) - (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
	int32 width  = (int16)ReadMacInt16(p + acclDestRect + 6) - (int16)ReadMacInt16(p + acclDestRect + 2);
	return NQD_packed_rect_edges_byte_aligned(src_X, width, pixel_size) &&
	       NQD_packed_rect_edges_byte_aligned(dest_X, width, pixel_size);
}

// Pass-through dirty areas to redraw functions
static inline void NQD_set_dirty_area(uint32 p)
{
	if (ReadMacInt32(p + acclDestBaseAddr) == screen_base) {
		int16 x = (int16)ReadMacInt16(p + acclDestRect + 2) - (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
		int16 y = (int16)ReadMacInt16(p + acclDestRect + 0) - (int16)ReadMacInt16(p + acclDestBoundsRect + 0);
		int16 w  = (int16)ReadMacInt16(p + acclDestRect + 6) - (int16)ReadMacInt16(p + acclDestRect + 2);
		int16 h = (int16)ReadMacInt16(p + acclDestRect + 4) - (int16)ReadMacInt16(p + acclDestRect + 0);
		video_set_dirty_area(x, y, w, h);
	}
}


/*
 *	Rectangle inversion
 */

template< int bpp >
static inline void do_invrect(uint8 *dest, uint32 length)
{
#define INVERT_1(PTR, OFS) ((uint8  *)(PTR))[OFS] = ~((uint8  *)(PTR))[OFS]
#define INVERT_2(PTR, OFS) ((uint16 *)(PTR))[OFS] = ~((uint16 *)(PTR))[OFS]
#define INVERT_4(PTR, OFS) ((uint32 *)(PTR))[OFS] = ~((uint32 *)(PTR))[OFS]
#define INVERT_8(PTR, OFS) ((uint64 *)(PTR))[OFS] = ~((uint64 *)(PTR))[OFS]

#ifndef UNALIGNED_PROFITABLE
	// Align on 16-bit boundaries
	if (bpp < 16 && (((uintptr)dest) & 1)) {
		INVERT_1(dest, 0);
		dest += 1; length -= 1;
	}

	// Align on 32-bit boundaries
	if (bpp < 32 && (((uintptr)dest) & 2) && length >= 2) {
		INVERT_2(dest, 0);
		dest += 2; length -= 2;
	}
#endif

	// Invert 8-byte words
	if (length >= 8) {
		const int r = (length / 8) % 8;
		dest += r * 8;

		int n = ((length / 8) + 7) / 8;
		switch (r) {
		case 0: do {
				dest += 64;
				INVERT_8(dest, -8);
		case 7: INVERT_8(dest, -7);
		case 6: INVERT_8(dest, -6);
		case 5: INVERT_8(dest, -5);
		case 4: INVERT_8(dest, -4);
		case 3: INVERT_8(dest, -3);
		case 2: INVERT_8(dest, -2);
		case 1: INVERT_8(dest, -1);
				} while (--n > 0);
		}
	}

	// 32-bit cell to invert?
	if (length & 4) {
		INVERT_4(dest, 0);
		if (bpp <= 16)
			dest += 4;
	}

	// 16-bit cell to invert?
	if (bpp <= 16 && (length & 2)) {
		INVERT_2(dest, 0);
		if (bpp <= 8)
			dest += 2;
	}

	// 8-bit cell to invert?
	if (bpp <= 8 && (length & 1))
		INVERT_1(dest, 0);

#undef INVERT_1
#undef INVERT_2
#undef INVERT_4
#undef INVERT_8
}

void NQD_invrect(uint32 p)
{
	D(bug("accl_invrect %08x\n", p));

	// Metal-accelerated path (only if dest is within Metal-mapped RAM)
	if (nqd_metal_available && NQDMetalAddrInBuffer(ReadMacInt32(p + acclDestBaseAddr))) {
		NQDMetalInvertRect(p);
		return;
	}

	// Get inversion parameters
	int16 dest_X = (int16)ReadMacInt16(p + acclDestRect + 2) - (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
	int16 dest_Y = (int16)ReadMacInt16(p + acclDestRect + 0) - (int16)ReadMacInt16(p + acclDestBoundsRect + 0);
	int16 width  = (int16)ReadMacInt16(p + acclDestRect + 6) - (int16)ReadMacInt16(p + acclDestRect + 2);
	int16 height = (int16)ReadMacInt16(p + acclDestRect + 4) - (int16)ReadMacInt16(p + acclDestRect + 0);
	D(bug(" dest X %d, dest Y %d\n", dest_X, dest_Y));
	D(bug(" width %d, height %d, bytes_per_row %d\n", width, height, (int32)ReadMacInt32(p + acclDestRowBytes)));

	//!!?? pen_mode == 14

	// And perform the inversion
	const int bpp = bytes_per_pixel(ReadMacInt32(p + acclDestPixelSize));
	const int dest_row_bytes = (int32)ReadMacInt32(p + acclDestRowBytes);
	uint8 *dest = Mac2HostAddr(ReadMacInt32(p + acclDestBaseAddr) + (dest_Y * dest_row_bytes) + (dest_X * bpp));
	width *= bpp;
	switch (bpp) {
	case 1:
		for (int i = 0; i < height; i++) {
			do_invrect<8>(dest, width);
			dest += dest_row_bytes;
		}
		break;
	case 2:
		for (int i = 0; i < height; i++) {
			do_invrect<16>(dest, width);
			dest += dest_row_bytes;
		}
		break;
	case 4:
		for (int i = 0; i < height; i++) {
			do_invrect<32>(dest, width);
			dest += dest_row_bytes;
		}
		break;
	}
}


/*
 *	Rectangle filling
 */

template< int bpp >
static inline void do_fillrect(uint8 *dest, uint32 color, uint32 length)
{
#define FILL_1(PTR, OFS, VAL) ((uint8  *)(PTR))[OFS] = (VAL)
#define FILL_2(PTR, OFS, VAL) ((uint16 *)(PTR))[OFS] = (VAL)
#define FILL_4(PTR, OFS, VAL) ((uint32 *)(PTR))[OFS] = (VAL)
#define FILL_8(PTR, OFS, VAL) ((uint64 *)(PTR))[OFS] = (VAL)

#ifndef UNALIGNED_PROFITABLE
	// Align on 16-bit boundaries
	if (bpp < 16 && (((uintptr)dest) & 1)) {
		FILL_1(dest, 0, color);
		dest += 1; length -= 1;
	}

	// Align on 32-bit boundaries
	if (bpp < 32 && (((uintptr)dest) & 2) && length >= 2) {
		FILL_2(dest, 0, color);
		dest += 2; length -= 2;
	}
#endif

	// Fill 8-byte words
	if (length >= 8) {
		const uint64 c = (((uint64)color) << 32) | color;
		const int r = (length / 8) % 8;
		dest += r * 8;

		int n = ((length / 8) + 7) / 8;
		switch (r) {
		case 0: do {
				dest += 64;
				FILL_8(dest, -8, c);
		case 7: FILL_8(dest, -7, c);
		case 6: FILL_8(dest, -6, c);
		case 5: FILL_8(dest, -5, c);
		case 4: FILL_8(dest, -4, c);
		case 3: FILL_8(dest, -3, c);
		case 2: FILL_8(dest, -2, c);
		case 1: FILL_8(dest, -1, c);
				} while (--n > 0);
		}
	}

	// 32-bit cell to fill?
	if (length & 4) {
		FILL_4(dest, 0, color);
		if (bpp <= 16)
			dest += 4;
	}

	// 16-bit cell to fill?
	if (bpp <= 16 && (length & 2)) {
		FILL_2(dest, 0, color);
		if (bpp <= 8)
			dest += 2;
	}

	// 8-bit cell to fill?
	if (bpp <= 8 && (length & 1))
		FILL_1(dest, 0, color);

#undef FILL_1
#undef FILL_2
#undef FILL_4
#undef FILL_8
}

void NQD_fillrect(uint32 p)
{
	D(bug("accl_fillrect %08x\n", p));

	// Metal-accelerated path (only if dest is within Metal-mapped RAM)
	if (nqd_metal_available && NQDMetalAddrInBuffer(ReadMacInt32(p + acclDestBaseAddr))) {
		NQDMetalFillRect(p);
		return;
	}

	// Get filling parameters
	int16 dest_X = (int16)ReadMacInt16(p + acclDestRect + 2) - (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
	int16 dest_Y = (int16)ReadMacInt16(p + acclDestRect + 0) - (int16)ReadMacInt16(p + acclDestBoundsRect + 0);
	int16 width  = (int16)ReadMacInt16(p + acclDestRect + 6) - (int16)ReadMacInt16(p + acclDestRect + 2);
	int16 height = (int16)ReadMacInt16(p + acclDestRect + 4) - (int16)ReadMacInt16(p + acclDestRect + 0);
	uint32 color = htonl(ReadMacInt32(p + acclPenMode) == 8 ? ReadMacInt32(p + acclForePen) : ReadMacInt32(p + acclBackPen));
	D(bug(" dest X %d, dest Y %d\n", dest_X, dest_Y));
	D(bug(" width %d, height %d\n", width, height));
	D(bug(" bytes_per_row %d color %08x\n", (int32)ReadMacInt32(p + acclDestRowBytes), color));

	// And perform the fill
	const int bpp = bytes_per_pixel(ReadMacInt32(p + acclDestPixelSize));
	const int dest_row_bytes = (int32)ReadMacInt32(p + acclDestRowBytes);
	uint8 *dest = Mac2HostAddr(ReadMacInt32(p + acclDestBaseAddr) + (dest_Y * dest_row_bytes) + (dest_X * bpp));

	width *= bpp;
	switch (bpp) {
	case 1:
		for (int i = 0; i < height; i++) {
			memset(dest, color, width);
			dest += dest_row_bytes;
		}
		break;
	case 2:
		for (int i = 0; i < height; i++) {
			do_fillrect<16>(dest, color, width);
			dest += dest_row_bytes;
		}
		break;
	case 4:
		for (int i = 0; i < height; i++) {
			do_fillrect<32>(dest, color, width);
			dest += dest_row_bytes;
		}
		break;
	}
}

/*
 *  Decline-to-software with GPU-batch flush.
 *
 *  Returning false hands the op to software QuickDraw immediately — but up
 *  to NQD_BATCH_MAX GPU dispatches against Metal-mapped RAM may still be
 *  queued. Software QD would then read/write a surface (e.g. the
 *  desktop-picture cache GWorld) BEFORE the queued GPU work lands: the
 *  visible result is stale/partial content (banding, stale strips), and
 *  for writes the late-landing batch can stomp newer software output.
 *  Flush first whenever the declined op touches the Metal RAM window on
 *  either side. Accepted ops keep batching; NQDMetalFlush early-outs when
 *  the batch is empty, so the cost on the hot decline edge is two range
 *  checks. (The CPU-fallback helper NQD_bitblt already flushes; this
 *  closes the same invariant for the decline edge.)
 */
static inline bool NQD_decline_with_flush(uint32 p)
{
	if (nqd_metal_available &&
		(NQDMetalAddrInBuffer(ReadMacInt32(p + acclSrcBaseAddr)) ||
		 NQDMetalAddrInBuffer(ReadMacInt32(p + acclDestBaseAddr))))
		NQDMetalFlush();
	return false;
}

bool NQD_fillrect_hook(uint32 p)
{
	D(bug("accl_fillrect_hook %08x\n", p));
	NQD_set_dirty_area(p);

	// Metal path: accept solid-color fill modes when dest is in Metal-mapped RAM.
	// Only accept patCopy (8) and arithmetic modes (32-39, 50) as solid fills.
	// Other pattern modes (9, 11-15 = patOr, notPatCopy, etc.) may carry non-solid
	// 8x8 bit patterns that the Metal kernel can't render -- let those fall back
	// to software QuickDraw.
	if (nqd_metal_available && ReadMacInt32(p + 0x284) != 0 &&
		NQDMetalAddrInBuffer(ReadMacInt32(p + acclDestBaseAddr)) &&
		NQD_dest_rect_byte_aligned_for_depth(p, ReadMacInt32(p + acclDestPixelSize))) {
		const int transfer_mode = ReadMacInt32(p + acclTransferMode);
		if (transfer_mode == 8 ||
			(transfer_mode >= 32 && transfer_mode <= 39) ||
			transfer_mode == 50) {
			// Solid fill via Metal (patCopy, arithmetic blend, hilite)
			WriteMacInt32(p + acclDrawProc, NativeTVECT(NATIVE_NQD_FILLRECT));
			return true;
		}
		else if (transfer_mode == 10) {
			// Invert: Mac OS dispatches invert through a separate thunk entry
			WriteMacInt32(p + acclDrawProc, NativeTVECT(NATIVE_NQD_INVRECT));
			return true;
		}
	}

	// CPU fallback: original checks with pixel size >= 8 restriction
	if (ReadMacInt32(p + 0x284) != 0 && ReadMacInt32(p + acclDestPixelSize) >= 8) {
		const int transfer_mode = ReadMacInt32(p + acclTransferMode);
		if (transfer_mode == 8) {
			// Fill
			WriteMacInt32(p + acclDrawProc, NativeTVECT(NATIVE_NQD_FILLRECT));
			return true;
		}
		else if (transfer_mode == 10) {
			// Invert
			WriteMacInt32(p + acclDrawProc, NativeTVECT(NATIVE_NQD_INVRECT));
			return true;
		}
	}
	return NQD_decline_with_flush(p);
}


/*
 *	Isomorphic rectangle blitting
 */

void NQD_bitblt(uint32 p)
{
	D(bug("accl_bitblt %08x\n", p));

	// Metal-accelerated path (only if src and dest are within Metal-mapped RAM)
	if (nqd_metal_available &&
		NQDMetalAddrInBuffer(ReadMacInt32(p + acclSrcBaseAddr)) &&
		NQDMetalAddrInBuffer(ReadMacInt32(p + acclDestBaseAddr))) {
		NQDMetalBitblt(p);
		return;
	}

	// Get blitting parameters
	int16 src_X  = (int16)ReadMacInt16(p + acclSrcRect + 2) - (int16)ReadMacInt16(p + acclSrcBoundsRect + 2);
	int16 src_Y  = (int16)ReadMacInt16(p + acclSrcRect + 0) - (int16)ReadMacInt16(p + acclSrcBoundsRect + 0);
	int16 dest_X = (int16)ReadMacInt16(p + acclDestRect + 2) - (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
	int16 dest_Y = (int16)ReadMacInt16(p + acclDestRect + 0) - (int16)ReadMacInt16(p + acclDestBoundsRect + 0);
	int16 width  = (int16)ReadMacInt16(p + acclDestRect + 6) - (int16)ReadMacInt16(p + acclDestRect + 2);
	int16 height = (int16)ReadMacInt16(p + acclDestRect + 4) - (int16)ReadMacInt16(p + acclDestRect + 0);
	D(bug(" src addr %08x, dest addr %08x\n", ReadMacInt32(p + acclSrcBaseAddr), ReadMacInt32(p + acclDestBaseAddr)));
	D(bug(" src X %d, src Y %d, dest X %d, dest Y %d\n", src_X, src_Y, dest_X, dest_Y));
	D(bug(" width %d, height %d\n", width, height));

	// And perform the blit
	const int bpp = bytes_per_pixel(ReadMacInt32(p + acclSrcPixelSize));
	width *= bpp;
	NQDMetalFlush();  // ensure pending NQD GPU writes are visible to CPU
	if ((int32)ReadMacInt32(p + acclSrcRowBytes) > 0) {
		const int src_row_bytes = (int32)ReadMacInt32(p + acclSrcRowBytes);
		const int dst_row_bytes = (int32)ReadMacInt32(p + acclDestRowBytes);
		uint8 *src = Mac2HostAddr(ReadMacInt32(p + acclSrcBaseAddr) + (src_Y * src_row_bytes) + (src_X * bpp));
		uint8 *dst = Mac2HostAddr(ReadMacInt32(p + acclDestBaseAddr) + (dest_Y * dst_row_bytes) + (dest_X * bpp));
		for (int i = 0; i < height; i++) {
			memmove(dst, src, width);
			src += src_row_bytes;
			dst += dst_row_bytes;
		}
	}
	else {
		const int src_row_bytes = -(int32)ReadMacInt32(p + acclSrcRowBytes);
		const int dst_row_bytes = -(int32)ReadMacInt32(p + acclDestRowBytes);
		uint8 *src = Mac2HostAddr(ReadMacInt32(p + acclSrcBaseAddr) + ((src_Y + height - 1) * src_row_bytes) + (src_X * bpp));
		uint8 *dst = Mac2HostAddr(ReadMacInt32(p + acclDestBaseAddr) + ((dest_Y + height - 1) * dst_row_bytes) + (dest_X * bpp));
		for (int i = height - 1; i >= 0; i--) {
			memmove(dst, src, width);
			src -= src_row_bytes;
			dst -= dst_row_bytes;
		}
	}
}

/*
  BitBlt transfer modes:
  0 : srcCopy
  1 : srcOr
  2 : srcXor
  3 : srcBic
  4 : notSrcCopy
  5 : notSrcOr
  6 : notSrcXor
  7 : notSrcBic
  32 : blend
  33 : addPin
  34 : addOver
  35 : subPin
  36 : transparent
  37 : adMax
  38 : subOver
  39 : adMin
  50 : hilite
*/

bool NQD_bitblt_hook(uint32 p)
{
	D(bug("accl_draw_hook %08x\n", p));
	NQD_set_dirty_area(p);

	// Metal path: accept all valid transfer modes when src and dest are in Metal-mapped RAM
	if (nqd_metal_available &&
		NQDMetalAddrInBuffer(ReadMacInt32(p + acclSrcBaseAddr)) &&
		NQDMetalAddrInBuffer(ReadMacInt32(p + acclDestBaseAddr)) &&
		ReadMacInt32(p + acclSrcPixelSize) == ReadMacInt32(p + acclDestPixelSize) &&
		(int32)(ReadMacInt32(p + acclSrcRowBytes) ^ ReadMacInt32(p + acclDestRowBytes)) >= 0 &&
		NQD_bitblt_rects_byte_aligned_for_depth(p, ReadMacInt32(p + acclSrcPixelSize))) {
		const uint32 mode = ReadMacInt32(p + acclTransferMode);
		// The colorizing Boolean SOURCE modes
		// (1, 3-7 — srcOr/srcBic/notSrcCopy/notSrcOr/notSrcXor/notSrcBic) at a
		// COLOUR depth (multi-bit source, acclSrcPixelSize >= 8) are the general
		// multi-bit-colour-source-under-a-Boolean-op case, whose Color-QuickDraw
		// semantics are defined only for 1-bit-style sources (rare/ill-defined
		// for multi-bit colour sources per IWQD). Decline these to software
		// QuickDraw rather than invent semantics — the Metal kernel's
		// colorize arm handles ONLY the 1-bit source (bits_per_pixel == 1) case.
		// srcCopy (0) and srcXor (2) are depth-agnostic raw copy/bitwise ops and
		// stay accelerated at every depth; arithmetic (32-39) and hilite (50) are
		// unaffected. Never leave a
		// multi-bit-colour Boolean source running raw per-byte bitwise on the GPU.
		const uint32 src_px = ReadMacInt32(p + acclSrcPixelSize);
		const bool colorizing_bool = (mode == 1 || (mode >= 3 && mode <= 7));
		if (colorizing_bool && src_px >= 8) {
			// Fall through to the CPU fallback / software QuickDraw (DELIBERATE).
		} else if (mode <= 7 || (mode >= 32 && mode <= 39) || mode == 50) {
			// Same-surface overlapping blits: the nqd_bitblt kernel is one
			// flat unordered dispatch, and only the standard-depth Boolean
			// family diverts overlaps to the ordered CPU scratch path inside
			// NQDMetalBitblt (NQD-02). Packed-depth Boolean and
			// arithmetic/hilite (32-39, 50) overlaps have no ordered route on
			// the accelerated path — decline them to software QuickDraw
			// (DELIBERATE), which observes sequential source reads.
			if ((src_px < 8 || mode >= 32) &&
				NQDMetalBitbltSameSurfaceOverlap(p)) {
				// Fall through to the CPU fallback / software QuickDraw.
			} else {
				// All accelerated transfer modes via Metal — no pre-check on
				// 0x018, 0x128, 0x130, 0x15c.
				WriteMacInt32(p + acclDrawProc, NativeTVECT(NATIVE_NQD_BITBLT));
				return true;
			}
		}
	}

	// CPU fallback: srcCopy (mode 0) with matching pixel sizes >= 8
	// Restore mask/clip guards — CPU memmove path can't handle masked or clipped blits.
	if (ReadMacInt32(p + 0x018) + ReadMacInt32(p + 0x128) == 0 &&
		ReadMacInt32(p + 0x130) == 0 &&
		ReadMacInt32(p + acclSrcPixelSize) >= 8 &&
		ReadMacInt32(p + acclSrcPixelSize) == ReadMacInt32(p + acclDestPixelSize) &&
		(int32)(ReadMacInt32(p + acclSrcRowBytes) ^ ReadMacInt32(p + acclDestRowBytes)) >= 0 &&
		ReadMacInt32(p + acclTransferMode) == 0 &&
		(int32)ReadMacInt32(p + 0x15c) > 0) {
		WriteMacInt32(p + acclDrawProc, NativeTVECT(NATIVE_NQD_BITBLT));
		return true;
	}

	return NQD_decline_with_flush(p);
}

// Unknown hook
bool NQD_unknown_hook(uint32 arg)
{
	D(bug("accl_unknown_hook %08x\n", arg));
	NQD_set_dirty_area(arg);

	return false;
}

// Mask operation hooks — validate mask data and set draw procs
// Only accept when dest (and src for bltmask) are in Metal-mapped RAM,
// since there is no CPU fallback for masked operations.
bool NQD_bltmask_hook(uint32 arg)
{
	D(bug("accl_bltmask_hook %08x\n", arg));
	NQD_set_dirty_area(arg);

	if (!nqd_metal_available) return false;

	// Check src and dest are in Metal-mapped RAM
	if (!NQDMetalAddrInBuffer(ReadMacInt32(arg + acclSrcBaseAddr)) ||
		!NQDMetalAddrInBuffer(ReadMacInt32(arg + acclDestBaseAddr)))
		return NQD_decline_with_flush(arg);

	// Validate mask data: check that mask region address is non-zero
	uint32 mask_rgn_addr = ReadMacInt32(arg + 0x128);
	if (mask_rgn_addr == 0) {
		D(bug("  bltmask_hook: null mask region at 0x128, declining\n"));
		return NQD_decline_with_flush(arg);
	}

	// Try reading the region header to validate
	uint16 rgnSize = ReadMacInt16(mask_rgn_addr);
	if (rgnSize < 10) {
		D(bug("  bltmask_hook: invalid rgnSize %u at 0x%08x, declining\n", rgnSize, mask_rgn_addr));
		return NQD_decline_with_flush(arg);
	}

	// Validate region bbox: top < bottom and left < right
	int16 bbox_top    = (int16)ReadMacInt16(mask_rgn_addr + 2);
	int16 bbox_left   = (int16)ReadMacInt16(mask_rgn_addr + 4);
	int16 bbox_bottom = (int16)ReadMacInt16(mask_rgn_addr + 6);
	int16 bbox_right  = (int16)ReadMacInt16(mask_rgn_addr + 8);
	if (bbox_top >= bbox_bottom || bbox_left >= bbox_right) {
		return NQD_decline_with_flush(arg);
	}

	// Validate pixel size matching (src == dest)
	if (ReadMacInt32(arg + acclSrcPixelSize) != ReadMacInt32(arg + acclDestPixelSize)) {
		return NQD_decline_with_flush(arg);
	}
	if (!NQD_bitblt_rects_byte_aligned_for_depth(arg, ReadMacInt32(arg + acclSrcPixelSize))) {
		return NQD_decline_with_flush(arg);
	}

	const uint32 mode = ReadMacInt32(arg + acclTransferMode);
	if (!(mode <= 7 || (mode >= 32 && mode <= 39) || mode == 50)) {
		return NQD_decline_with_flush(arg);
	}

	D(bug("  bltmask_hook: accepting mask=0x%08x rgnSize=%u mode=%d\n",
		mask_rgn_addr, rgnSize, mode));
	WriteMacInt32(arg + acclDrawProc, NativeTVECT(NATIVE_NQD_BLTMASK));
	return true;
}

bool NQD_fillmask_hook(uint32 arg)
{
	D(bug("accl_fillmask_hook %08x\n", arg));
	NQD_set_dirty_area(arg);

	if (!nqd_metal_available) return false;

	// Check dest is in Metal-mapped RAM
	if (!NQDMetalAddrInBuffer(ReadMacInt32(arg + acclDestBaseAddr)))
		return NQD_decline_with_flush(arg);

	// Validate mask data: check that mask region address is non-zero
	uint32 mask_rgn_addr = ReadMacInt32(arg + 0x128);
	if (mask_rgn_addr == 0) {
		D(bug("  fillmask_hook: null mask region at 0x128, declining\n"));
		return NQD_decline_with_flush(arg);
	}

	// Try reading the region header to validate
	uint16 rgnSize = ReadMacInt16(mask_rgn_addr);
	if (rgnSize < 10) {
		D(bug("  fillmask_hook: invalid rgnSize %u at 0x%08x, declining\n", rgnSize, mask_rgn_addr));
		return NQD_decline_with_flush(arg);
	}

	// Validate region bbox: top < bottom and left < right
	int16 bbox_top    = (int16)ReadMacInt16(mask_rgn_addr + 2);
	int16 bbox_left   = (int16)ReadMacInt16(mask_rgn_addr + 4);
	int16 bbox_bottom = (int16)ReadMacInt16(mask_rgn_addr + 6);
	int16 bbox_right  = (int16)ReadMacInt16(mask_rgn_addr + 8);
	if (bbox_top >= bbox_bottom || bbox_left >= bbox_right) {
		return NQD_decline_with_flush(arg);
	}

	// Check 0x284 field (same as fillrect_hook)
	if (ReadMacInt32(arg + 0x284) == 0) {
		return NQD_decline_with_flush(arg);
	}
	if (!NQD_dest_rect_byte_aligned_for_depth(arg, ReadMacInt32(arg + acclDestPixelSize))) {
		return NQD_decline_with_flush(arg);
	}

	const uint32 mode = ReadMacInt32(arg + acclTransferMode);
	if (!((mode >= 8 && mode <= 15) || (mode >= 32 && mode <= 39) || mode == 50)) {
		return NQD_decline_with_flush(arg);
	}

	D(bug("  fillmask_hook: accepting mask=0x%08x rgnSize=%u mode=%d\n",
		mask_rgn_addr, rgnSize, mode));
	WriteMacInt32(arg + acclDrawProc, NativeTVECT(NATIVE_NQD_FILLMASK));
	return true;
}

// Masked draw procs
void NQD_bltmask(uint32 p)
{
	D(bug("accl_bltmask %08x\n", p));

	// Metal-accelerated path (only if src and dest are within Metal-mapped RAM)
	if (nqd_metal_available &&
		NQDMetalAddrInBuffer(ReadMacInt32(p + acclSrcBaseAddr)) &&
		NQDMetalAddrInBuffer(ReadMacInt32(p + acclDestBaseAddr))) {
		NQDMetalBltMask(p);
		return;
	}

	// CPU fallback: decline (return without drawing, Mac OS will use software)
	D(bug("  bltmask: no Metal, falling back to software\n"));
}

void NQD_fillmask(uint32 p)
{
	D(bug("accl_fillmask %08x\n", p));

	// Metal-accelerated path (only if dest is within Metal-mapped RAM)
	if (nqd_metal_available && NQDMetalAddrInBuffer(ReadMacInt32(p + acclDestBaseAddr))) {
		NQDMetalFillMask(p);
		return;
	}

	// CPU fallback: decline (return without drawing, Mac OS will use software)
	D(bug("  fillmask: no Metal, falling back to software\n"));
}

// Wait for graphics operation to finish
bool NQD_sync_hook(uint32 arg)
{
	D(bug("accl_sync_hook %08x\n", arg));
	// Flush any pending batched NQD Metal dispatches so all drawing is
	// committed and visible before Mac OS continues.
	if (nqd_metal_available)
		NQDMetalFlush();
	return true;
}


/*
 *	Install Native QuickDraw acceleration hooks
 */

void VideoInstallAccel(void)
{
	// Install NQD acceleration hooks (one-time only)
	static bool nqd_hooks_installed = false;
	if (!nqd_hooks_installed && PrefsFindBool("nqdaccel")) {
		nqd_hooks_installed = true;
		D(bug("Video: Installing acceleration hooks\n"));

		// Initialize Metal compute infrastructure before registering hooks
		NQDMetalInit();

		uint32 base;

		SheepVar bitblt_hook_info(sizeof(accl_hook_info));
		base = bitblt_hook_info.addr();
		WriteMacInt32(base + 0, NativeTVECT(NATIVE_NQD_BITBLT_HOOK));
		WriteMacInt32(base + 4, NativeTVECT(NATIVE_NQD_SYNC_HOOK));
		WriteMacInt32(base + 8, ACCL_BITBLT);
		NQDMisc(6, bitblt_hook_info.addr());

		SheepVar fillrect_hook_info(sizeof(accl_hook_info));
		base = fillrect_hook_info.addr();
		WriteMacInt32(base + 0, NativeTVECT(NATIVE_NQD_FILLRECT_HOOK));
		WriteMacInt32(base + 4, NativeTVECT(NATIVE_NQD_SYNC_HOOK));
		WriteMacInt32(base + 8, ACCL_FILLRECT);
		NQDMisc(6, fillrect_hook_info.addr());

		// Register hooks for all 8 NQD operation codes.
		// Ops 0 (BITBLT) and 2 (FILLRECT) already registered above.
		// Ops 1 (BLTMASK) and 3 (FILLMASK) get separate mask hooks.
		// Ops 4-7 get the unknown_hook.
		for (int op = 0; op < 8; op++) {
			switch (op) {
			case ACCL_BITBLT:
			case ACCL_FILLRECT:
				continue;
			case ACCL_BLTMASK: {
				SheepVar bltmask_hook_info(sizeof(accl_hook_info));
				base = bltmask_hook_info.addr();
				WriteMacInt32(base + 0, NativeTVECT(NATIVE_NQD_BLTMASK_HOOK));
				WriteMacInt32(base + 4, NativeTVECT(NATIVE_NQD_SYNC_HOOK));
				WriteMacInt32(base + 8, op);
				NQDMisc(6, bltmask_hook_info.addr());
				break;
			}
			case ACCL_FILLMASK: {
				SheepVar fillmask_hook_info(sizeof(accl_hook_info));
				base = fillmask_hook_info.addr();
				WriteMacInt32(base + 0, NativeTVECT(NATIVE_NQD_FILLMASK_HOOK));
				WriteMacInt32(base + 4, NativeTVECT(NATIVE_NQD_SYNC_HOOK));
				WriteMacInt32(base + 8, op);
				NQDMisc(6, fillmask_hook_info.addr());
				break;
			}
			default: {
				SheepVar unknown_hook_info(sizeof(accl_hook_info));
				base = unknown_hook_info.addr();
				WriteMacInt32(base + 0, NativeTVECT(NATIVE_NQD_UNKNOWN_HOOK));
				WriteMacInt32(base + 4, NativeTVECT(NATIVE_NQD_SYNC_HOOK));
				WriteMacInt32(base + 8, op);
				NQDMisc(6, unknown_hook_info.addr());
				break;
			}
			}
		}
	}

	// Register RAVE engine for 3D acceleration.
	// RaveRegisterEngine has its own guards (rave_registered, rave_reg_in_progress)
	// and returns immediately if already registered. May be called multiple times
	// from accRun periodic action until RAVE library is available.
	if (PrefsFindBool("raveaccel"))
		RaveRegisterEngine();

	// Install OpenGL hooks for GL/AGL/GLU/GLUT function interception.
	// GLInstallHooks has its own guards and returns immediately if already installed.
	if (PrefsFindBool("glaccel"))
		GLInstallHooks();

	// DSp (DrawSprocket) engine — fourth engine peer to NQD/RAVE/GL.
	// Gated on "dspaccel" prefs key (default true) so users
	// can disable cleanly to isolate regressions. Both DSpInit and the install
	// hook live in the SAME gated branch: DSpInit registers the engine +
	// host-bridge observer (idempotent, safe to call multiply); the install
	// hook patches the emulated-PPC DrawSprocketLib CFM symbol-table entries
	// to redirect into our dsp_method_tvects[] thunks. The installer has
	// its own retry-guard triplet (dsp_hooks_installed + dsp_hooks_in_progress
	// + dsp_hooks_attempts) and caps at DSP_HOOKS_MAX_ATTEMPTS=3 — mirrors
	// GLInstallHooks wired 3 lines above. The CFM fragment may not be loaded
	// on the first accRun tick (apps lazy-load DrawSprocketLib), so
	// VideoInstallAccel calls into this branch on every accRun tick until
	// hooks install.
	if (PrefsFindBool("dspaccel")) {
		DSpInit();
		DSpInstallHooks();
	}
}

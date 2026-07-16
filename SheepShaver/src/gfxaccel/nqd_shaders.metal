/*
 *  nqd_shaders.metal - Metal compute kernels for NQD 2D acceleration
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Compute kernels for NQD bitblt (srcCopy), fillrect, and invrect.
 *  The buffer parameter is the Mac RAM shared buffer — src and dest are
 *  addressed by byte offsets within it.
 */

#include <metal_stdlib>
using namespace metal;

// Uniform struct — must match NQDBitbltUniforms in nqd_metal_renderer.mm
struct NQDBitbltUniforms {
    uint src_offset;
    uint dst_offset;
    int  src_row_bytes;
    int  dst_row_bytes;
    uint width_bytes;
    uint height;
    uint transfer_mode;
    uint pixel_size;     // bytes per pixel (1, 2, or 4)
    uint width_pixels;   // width in pixels (used for arithmetic/hilite modes)
    uint fore_pen;       // 8/16/32bpp logical pixel; 1/2/4bpp htonl-packed pen
    uint back_pen;       // 8/16/32bpp logical pixel; 1/2/4bpp htonl-packed pen
    uint hilite_color;   // HiliteRGB packed to pixel depth (from Mac low-memory 0x0DA0)
    uint mask_enabled;   // 1 = mask gating active, 0 = no mask
    uint mask_offset;    // byte offset into mask_buffer where mask data starts
    uint mask_stride;    // mask row stride (width_bytes for Boolean, width_pixels for arithmetic)
    uint bits_per_pixel; // raw pixel depth in bits (1, 2, 4, 8, 16, or 32) — for packed pixel support
    // Per-channel rgbOpColor blend weights [0-65535] for mode
    // 32 (blend). 0=all dst, 65535=all src. Must match the 3-field layout in
    // NQDBitbltUniforms in nqd_metal_renderer.mm exactly.
    uint blend_weight_r;
    uint blend_weight_g;
    uint blend_weight_b;
};

// Uniform struct — must match NQDFillRectUniforms in nqd_metal_renderer.mm
struct NQDFillRectUniforms {
    uint dst_offset;
    int  row_bytes;
    uint width_bytes;
    uint height;
    uint fill_color;     // fill pixel/index pattern, logical at standard depths
    uint bpp;            // bytes per pixel (1, 2, or 4)
    uint transfer_mode;  // pen mode: 8-15 (Boolean), 32-39 (arithmetic), 50 (hilite)
    uint pixel_size;     // bytes per pixel (same as bpp; kept for naming consistency with bitblt)
    uint width_pixels;   // width in pixels (used for arithmetic/hilite per-pixel dispatch)
    uint fore_pen;       // 8/16/32bpp logical pixel; 1/2/4bpp htonl-packed pen
    uint back_pen;       // 8/16/32bpp logical pixel; 1/2/4bpp htonl-packed pen
    uint hilite_color;   // HiliteRGB packed to pixel depth
    uint mask_enabled;   // 1 = mask gating active, 0 = no mask
    uint mask_offset;    // byte offset into mask_buffer where mask data starts
    uint mask_stride;    // mask row stride (width_bytes for Boolean, width_pixels for arithmetic)
    uint bits_per_pixel; // raw pixel depth in bits (1, 2, 4, 8, 16, or 32) — for packed pixel support
    // Per-channel rgbOpColor blend weights [0-65535] for mode
    // 32 (blend). 0=all dst, 65535=all src. Must match the 3-field layout in
    // NQDFillRectUniforms in nqd_metal_renderer.mm exactly.
    uint blend_weight_r;
    uint blend_weight_g;
    uint blend_weight_b;
};

// ---------------------------------------------------------------------------
// Big-endian pixel read/write and component extraction helpers
//
// Mac framebuffer stores pixels in big-endian byte order:
// - 8bpp:  single byte (index value)
// - 16bpp: [HI][LO] bytes → 1-5-5-5 ARGB big-endian
// - 32bpp: [A][R][G][B] bytes
//
// We must NOT use pointer casts (e.g. *(uint16_t*)) because Metal on ARM
// reads in little-endian, which would silently reverse byte order.
// Instead, manually assemble multi-byte values from individual bytes.
// ---------------------------------------------------------------------------

// Read a standard-depth pixel from the buffer at byte address addr, returning
// its logical 8/16/32bpp value. 16bpp keeps the raw 1555 high bit exactly as
// stored, so whole-pixel comparisons must pass pens in the same logical form.
static inline uint nqd_read_pixel(device uint8_t *buffer, uint addr, uint bpp)
{
    if (bpp == 1) {
        return uint(buffer[addr]);
    } else if (bpp == 2) {
        // Big-endian 16-bit: HI byte at addr, LO byte at addr+1
        return (uint(buffer[addr]) << 8) | uint(buffer[addr + 1]);
    } else {
        // Big-endian 32-bit: [A][R][G][B]
        return (uint(buffer[addr]) << 24) | (uint(buffer[addr + 1]) << 16) |
               (uint(buffer[addr + 2]) << 8) | uint(buffer[addr + 3]);
    }
}

// Write a pixel value back to the buffer in big-endian byte order.
static inline void nqd_write_pixel(device uint8_t *buffer, uint addr, uint bpp, uint value)
{
    if (bpp == 1) {
        buffer[addr] = uint8_t(value & 0xFF);
    } else if (bpp == 2) {
        buffer[addr]     = uint8_t((value >> 8) & 0xFF);
        buffer[addr + 1] = uint8_t(value & 0xFF);
    } else {
        buffer[addr]     = uint8_t((value >> 24) & 0xFF);
        buffer[addr + 1] = uint8_t((value >> 16) & 0xFF);
        buffer[addr + 2] = uint8_t((value >> 8) & 0xFF);
        buffer[addr + 3] = uint8_t(value & 0xFF);
    }
}

// ---------------------------------------------------------------------------
// Packed (sub-byte) pixel read/write helpers for 1/2/4-bit depths
//
// Mac QuickDraw uses MSB-first bit order within each byte:
// - 1bpp: bit 7 = pixel 0 (leftmost), bit 0 = pixel 7
// - 2bpp: bits 7-6 = pixel 0, bits 5-4 = pixel 1, bits 3-2 = pixel 2, bits 1-0 = pixel 3
// - 4bpp: bits 7-4 = pixel 0 (high nibble), bits 3-0 = pixel 1 (low nibble)
//
// pixel_index_in_byte ranges from 0 to (8/bits_per_pixel - 1).
// ---------------------------------------------------------------------------

// Read a single sub-byte pixel value from a packed byte.
static inline uint nqd_read_packed_pixel(device uint8_t *buffer, uint byte_addr,
                                          uint pixel_index_in_byte, uint bits_per_pixel)
{
    uint8_t byte_val = buffer[byte_addr];
    if (bits_per_pixel == 1) {
        uint shift = 7 - pixel_index_in_byte;
        return (uint(byte_val) >> shift) & 0x1;
    } else if (bits_per_pixel == 2) {
        uint shift = (3 - pixel_index_in_byte) * 2;
        return (uint(byte_val) >> shift) & 0x3;
    } else { // 4bpp
        uint shift = (1 - pixel_index_in_byte) * 4;
        return (uint(byte_val) >> shift) & 0xF;
    }
}

// Write a single sub-byte pixel value into a packed byte (read-modify-write).
static inline void nqd_write_packed_pixel(device uint8_t *buffer, uint byte_addr,
                                           uint pixel_index_in_byte, uint bits_per_pixel,
                                           uint value)
{
    uint8_t byte_val = buffer[byte_addr];
    if (bits_per_pixel == 1) {
        uint shift = 7 - pixel_index_in_byte;
        uint mask = 0x1 << shift;
        byte_val = (byte_val & ~uint8_t(mask)) | uint8_t((value & 0x1) << shift);
    } else if (bits_per_pixel == 2) {
        uint shift = (3 - pixel_index_in_byte) * 2;
        uint mask = 0x3 << shift;
        byte_val = (byte_val & ~uint8_t(mask)) | uint8_t((value & 0x3) << shift);
    } else { // 4bpp
        uint shift = (1 - pixel_index_in_byte) * 4;
        uint mask = 0xF << shift;
        byte_val = (byte_val & ~uint8_t(mask)) | uint8_t((value & 0xF) << shift);
    }
    buffer[byte_addr] = byte_val;
}

// Extract pixel components into a uint4 (r, g, b, a) for arithmetic operations.
// Components are normalized to their per-depth max range:
// - 1bpp:  single value in r (0-1), g=b=a=0
// - 2bpp:  single value in r (0-3), g=b=a=0
// - 4bpp:  single value in r (0-15), g=b=a=0
// - 8bpp:  single value in r (0-255), g=b=a=0
// - 16bpp: 5-5-5 → r,g,b each 0-31; a = top bit (0 or 1)
// - 32bpp: A,R,G,B each 0-255
// bits_per_pixel_val: raw bits (1,2,4,8,16,32). bpp: bytes per pixel (1,2,4).
static inline uint4 nqd_extract_components(uint pixel, uint bpp, uint bits_per_pixel_val)
{
    if (bits_per_pixel_val < 8) {
        // Packed sub-byte: pixel is already the raw index value
        return uint4(pixel, 0, 0, 0);
    }
    if (bpp == 1) {
        return uint4(pixel & 0xFF, 0, 0, 0);
    } else if (bpp == 2) {
        // 16-bit big-endian 1-5-5-5 ARGB: bit layout in our uint16 value:
        // bit 15 = alpha, bits 14-10 = R, bits 9-5 = G, bits 4-0 = B
        uint a = (pixel >> 15) & 0x1;
        uint r = (pixel >> 10) & 0x1F;
        uint g = (pixel >> 5)  & 0x1F;
        uint b = pixel & 0x1F;
        return uint4(r, g, b, a);
    } else {
        // 32-bit ARGB: [A][R][G][B] in big-endian → bits 31-24=A, 23-16=R, 15-8=G, 7-0=B
        uint a = (pixel >> 24) & 0xFF;
        uint r = (pixel >> 16) & 0xFF;
        uint g = (pixel >> 8)  & 0xFF;
        uint b = pixel & 0xFF;
        return uint4(r, g, b, a);
    }
}

// Pack components back into a pixel value (inverse of nqd_extract_components).
static inline uint nqd_pack_components(uint4 c, uint bpp, uint bits_per_pixel_val)
{
    if (bits_per_pixel_val < 8) {
        // Packed sub-byte: just the raw index value
        return c.x;
    }
    if (bpp == 1) {
        return c.x & 0xFF;
    } else if (bpp == 2) {
        return ((c.w & 0x1) << 15) | ((c.x & 0x1F) << 10) | ((c.y & 0x1F) << 5) | (c.z & 0x1F);
    } else {
        return ((c.w & 0xFF) << 24) | ((c.x & 0xFF) << 16) | ((c.y & 0xFF) << 8) | (c.z & 0xFF);
    }
}

// Component max value for the given depth (for saturation arithmetic).
static inline uint nqd_comp_max(uint bpp, uint bits_per_pixel_val)
{
    if (bits_per_pixel_val == 1) return 1;
    if (bits_per_pixel_val == 2) return 3;
    if (bits_per_pixel_val == 4) return 15;
    if (bpp == 2) return 31;   // 5-bit components
    return 255;                 // 8-bit components (8bpp and 32bpp)
}

// ---------------------------------------------------------------------------
// Pen index extraction for sub-byte (1/2/4-bit) destination depths.
//
// Standard depths (8/16/32) compare whole pixels against the logical value
// returned by nqd_read_pixel(); their uniform pen fields are not byte-swapped.
//
// For a SUB-BYTE (packed) depth the pixel value is a small index that the Mac
// stores in the big-endian LEAST-significant byte of the 4-byte pen field
// (e.g. a 1-bit index 1 is accl_params bytes [00][00][00][01]). ReadMacInt32
// returns it host-native as 0x00000001, and htonl() then lands that index byte
// in bits 31-24 of the uniform word. So the correct packed index is the LOW
// (1/2/4) bits of the *most-significant* byte — NOT `pen & 1u`, which reads the
// wrong byte after the htonl packing and mis-colorizes a real fore/back pen.
// (Previously the only validated pens were lane-replicated like 0xC8C8C8C8,
// which hid this byte-order asymmetry; distinct fore/back indices expose it.)
static inline uint nqd_pen_index_packed(uint pen_htonl, uint bits_per_pixel_val)
{
    uint mask = (1u << bits_per_pixel_val) - 1u;
    return (pen_htonl >> 24) & mask;   // big-endian LSByte of the pen
}

// ---------------------------------------------------------------------------
// nqd_bitblt — bitblt compute kernel with all 17 transfer modes
//
// Boolean modes (0-7): per-byte operations. Thread gid ranges over
// width_bytes * height. Each thread processes one byte.
//
// Arithmetic modes (32-39) and hilite (50): per-pixel operations. Thread gid
// ranges over width_pixels * height. Each thread processes one complete pixel
// (1, 2, or 4 bytes) with component decomposition.
//
// The host dispatches the correct total thread count based on mode family:
// - Modes 0-7: total = width_bytes * height
// - Modes 32-39, 50: total = width_pixels * height
// ---------------------------------------------------------------------------

kernel void nqd_bitblt(device uint8_t *buffer        [[buffer(0)]],
                       constant NQDBitbltUniforms &u  [[buffer(1)]],
                       device uint8_t *mask_buffer    [[buffer(2)]],
                       uint gid                       [[thread_position_in_grid]])
{
    // --- Boolean modes (0-7): per-byte operations ---
    if (u.transfer_mode <= 7) {
        uint total = u.width_bytes * u.height;
        if (gid >= total) return;

        uint row = gid / u.width_bytes;
        uint col = gid % u.width_bytes;

        // Mask check for byte-mode. Standard depths use a pixel-column mask;
        // packed Boolean paths keep byte-column masks.
        if (u.mask_enabled) {
            uint mask_col = (u.bits_per_pixel >= 8) ? (col / u.pixel_size) : col;
            uint mask_addr = u.mask_offset + row * u.mask_stride + mask_col;
            if (mask_buffer[mask_addr] == 0) return;
        }

        uint src_addr = u.src_offset + row * uint(u.src_row_bytes) + col;
        uint dst_addr = u.dst_offset + row * uint(u.dst_row_bytes) + col;

        uint8_t src = buffer[src_addr];
        uint8_t dst = buffer[dst_addr];

        // Color-QuickDraw fore/back colorize for a 1-bit source.
        // On a colour port, Boolean SOURCE modes apply the port's FOREGROUND pen
        // colour where the source bit is 1 and the BACKGROUND pen colour where 0,
        // THEN the Boolean op against the destination — they do NOT run raw
        // per-byte bitwise on the source (that is correct only when the source is
        // already a 1-bit/index device, i.e. fore=1/back=0). When bits_per_pixel
        // == 1 the source byte packs eight 1-bit pixels; colorize each bit to the
        // 1-bit index of fore_pen (1) / back_pen (0) before the Boolean op.
        // fore_pen / back_pen are already in the uniform (accl_params 0x1c/0x20)
        // — no new marshalling — but they are htonl-packed, so the 1-bit index is
        // the big-endian LSByte's low bit, extracted via nqd_pen_index_packed()
        // (NOT `pen & 1u`, which reads the wrong byte and mis-colorizes a real
        // fore/back pen). The GENERAL multi-bit colour source under a Boolean op
        // is gated to software in gfxaccel.cpp (NQD_bitblt_hook) — it never
        // reaches this kernel, so no raw-bitwise colour-source path remains here.
        if (u.bits_per_pixel == 1) {
            uint fore_bit = nqd_pen_index_packed(u.fore_pen, 1u);
            uint back_bit = nqd_pen_index_packed(u.back_pen, 1u);
            uint8_t colorized = 0;
            for (uint bit = 0; bit < 8; bit++) {
                uint s = (uint(src) >> bit) & 1u;
                uint c = (s == 1u) ? fore_bit : back_bit;  // colorize 1->fore 0->back
                colorized |= uint8_t(c << bit);
            }
            src = colorized;
        }

        switch (u.transfer_mode) {
            case 0:  buffer[dst_addr] = src;              break;  // srcCopy
            case 1:  buffer[dst_addr] = src | dst;        break;  // srcOr
            case 2:  buffer[dst_addr] = src ^ dst;        break;  // srcXor
            case 3:  buffer[dst_addr] = (~src) & dst;     break;  // srcBic
            case 4:  buffer[dst_addr] = ~src;             break;  // notSrcCopy
            case 5:  buffer[dst_addr] = (~src) | dst;     break;  // notSrcOr
            case 6:  buffer[dst_addr] = (~src) ^ dst;     break;  // notSrcXor
            case 7:  buffer[dst_addr] = src & dst;        break;  // notSrcBic
            default: buffer[dst_addr] = src;              break;
        }
        return;
    }

    // --- Arithmetic modes (32-39) and hilite (50) ---
    // For packed depths (bits_per_pixel < 8), dispatch is per-byte with inner pixel loop.
    // For standard depths (>= 8), dispatch is per-pixel (one thread per pixel).

    if (u.bits_per_pixel < 8) {
        // --- Packed pixel arithmetic/hilite: per-byte dispatch with inner pixel loop ---
        uint packed_total = u.width_bytes * u.height;
        if (gid >= packed_total) return;

        uint p_row = gid / u.width_bytes;
        uint p_col = gid % u.width_bytes;  // byte column

        uint src_byte_addr = u.src_offset + p_row * uint(u.src_row_bytes) + p_col;
        uint dst_byte_addr = u.dst_offset + p_row * uint(u.dst_row_bytes) + p_col;

        uint pixels_per_byte = 8 / u.bits_per_pixel;
        uint bpp_local = u.bits_per_pixel;

        for (uint pi = 0; pi < pixels_per_byte; pi++) {
            // Mask check: compute pixel column for this sub-pixel
            if (u.mask_enabled) {
                uint pixel_col = p_col * pixels_per_byte + pi;
                uint mask_addr = u.mask_offset + p_row * u.mask_stride + pixel_col;
                if (mask_buffer[mask_addr] == 0) continue;
            }

            uint src_val = nqd_read_packed_pixel(buffer, src_byte_addr, pi, bpp_local);
            uint dst_val = nqd_read_packed_pixel(buffer, dst_byte_addr, pi, bpp_local);

            // 1-bit-destination arithmetic/hilite Table 4-2 behavior
            // (IWQD 4-40 "Arithmetic modes in a 1-bit environment").
            // On a 1-bit device each arithmetic/hilite mode is defined to act
            // as its Boolean equivalent, NOT as literal arithmetic on the 0/1
            // index with cmax=1. Route each mode to the Boolean op (reusing the
            // exact expressions from the Boolean switch above) and write it.
            //   blend(32)                       -> srcCopy  (dst = src)
            //   addOver(34),subOver(38),hilite(50) -> srcXor (dst = src ^ dst)
            //   addPin(33),addMax(37)           -> srcBic   (dst = ~src & dst)
            //   subPin(35),adMin(39),transparent(36) -> srcOr (dst = src | dst)
            // hilite(50) -> srcXor is load-bearing and is included here per
            // IWQD Table 4-2.
            if (bpp_local == 1) {
                uint b1_src = src_val & 1u;
                uint b1_dst = dst_val & 1u;
                uint b1_out;
                switch (u.transfer_mode) {
                    case 32: b1_out = b1_src;              break;  // srcCopy
                    case 34:                                       // addOver
                    case 38:                                       // subOver
                    case 50: b1_out = b1_src ^ b1_dst;     break;  // hilite -> srcXor
                    case 33:                                       // addPin
                    case 37: b1_out = (~b1_src) & b1_dst & 1u; break;  // adMax -> srcBic
                    case 35:                                       // subPin
                    case 36:                                       // transparent
                    case 39: b1_out = b1_src | b1_dst;     break;  // adMin -> srcOr
                    default: b1_out = b1_src;              break;  // srcCopy fallback
                }
                nqd_write_packed_pixel(buffer, dst_byte_addr, pi, bpp_local, b1_out);
                continue;
            }

            // Mode 36 (transparent): skip if src matches background.
            // The pen index is the big-endian LSByte of the htonl-packed pen,
            // extracted via nqd_pen_index_packed() — NOT
            // `back_pen & ((1<<bpp)-1)`, which reads the LOW bits of the packed
            // word and (for a non-lane-replicated 2/4bpp index) compares against
            // the wrong value, so transparent skips the wrong pixels. Identical
            // byte-order class to the 1bpp colorize fix.
            if (u.transfer_mode == 36) {
                if (src_val != nqd_pen_index_packed(u.back_pen, bpp_local)) {
                    nqd_write_packed_pixel(buffer, dst_byte_addr, pi, bpp_local, src_val);
                }
                continue;
            }

            // Mode 50 (hilite): one-directional replace
            // background colour -> highlight colour ONLY (IWQD 4-41/4-43). The
            // prior `else if (dst == hilite_color) -> back_pen` reverse arm
            // corrupted dest pixels that legitimately equalled the hilite colour;
            // it is deleted so such pixels are preserved. (This 2/4-bpp packed
            // arm is reached only for bpp_local 2/4 — the 1-bit case is already
            // routed through the Table 4-2 srcXor behavior above.)
            // The COMPARED background pen (back_pen) is the
            // htonl-packed pen field, so its packed index is the big-endian LSByte
            // via nqd_pen_index_packed() — NOT the LOW bits of the packed word,
            // which mis-compared a non-lane-replicated 2/4bpp pen and replaced the
            // wrong pixels. hilite_color is DIFFERENT: the renderer builds it via
            // nqd_pack_hilite_color(bpp==1 for packed) as a raw low-byte index (it
            // is NOT htonl'd), so its index IS the low bits — extracting it via
            // nqd_pen_index_packed() would read the wrong (zero) byte. Only
            // back_pen moves to the byte-order-correct helper; hilite_color keeps
            // the low-bit mask (the documented index-approximation surface).
            if (u.transfer_mode == 50) {
                uint bp = nqd_pen_index_packed(u.back_pen, bpp_local);
                uint hc = u.hilite_color & ((1u << bpp_local) - 1);
                if (dst_val == bp) {
                    nqd_write_packed_pixel(buffer, dst_byte_addr, pi, bpp_local, hc);
                }
                // else: leave dst unchanged (one-directional — no reverse arm)
                continue;
            }

            // Arithmetic modes: per-component (single index value for packed)
            uint4 sc = nqd_extract_components(src_val, 1, bpp_local);
            uint4 dc = nqd_extract_components(dst_val, 1, bpp_local);
            uint cmax_p = nqd_comp_max(1, bpp_local);
            uint4 p_result;

            switch (u.transfer_mode) {
                case 32: { // blend — weighted by OpColor (packed index-approx)
                    // The packed (<8bpp) branch operates on a single-scalar
                    // palette index, not per-channel colour, so it keeps the
                    // red rgbOpColor weight only — this is the documented
                    // index-approximation surface, not a per-channel colour
                    // blend.
                    uint w = u.blend_weight_r;
                    p_result = (sc * w + dc * (65535u - w)) / 65535u;
                    break;
                }
                case 33: p_result = min(sc + dc, uint4(cmax_p, cmax_p, cmax_p, cmax_p)); break;
                case 34: p_result = (sc + dc) & uint4(cmax_p, cmax_p, cmax_p, cmax_p); break;  // addOver: modular wrap
                case 35:
                    p_result.x = (dc.x > sc.x) ? (dc.x - sc.x) : 0;
                    p_result.y = p_result.z = p_result.w = 0;
                    break;
                case 37: p_result = max(sc, dc); break;
                case 38: p_result = (dc - sc) & uint4(cmax_p, cmax_p, cmax_p, cmax_p); break;  // subOver: modular wrap
                case 39: p_result = min(sc, dc); break;
                default: p_result = sc; break;
            }

            nqd_write_packed_pixel(buffer, dst_byte_addr, pi, bpp_local,
                                   nqd_pack_components(p_result, 1, bpp_local));
        }
        return;
    }

    // --- Standard depths (>= 8 bpp): per-pixel operations ---
    uint total = u.width_pixels * u.height;
    if (gid >= total) return;

    uint bpp = u.pixel_size;
    uint row = gid / u.width_pixels;
    uint col = gid % u.width_pixels;

    // Mask check for pixel-mode: col is pixel column, mask_stride is width_pixels
    if (u.mask_enabled) {
        uint mask_addr = u.mask_offset + row * u.mask_stride + col;
        if (mask_buffer[mask_addr] == 0) return;
    }

    uint src_addr = u.src_offset + row * uint(u.src_row_bytes) + col * bpp;
    uint dst_addr = u.dst_offset + row * uint(u.dst_row_bytes) + col * bpp;

    uint src_pixel = nqd_read_pixel(buffer, src_addr, bpp);
    uint dst_pixel = nqd_read_pixel(buffer, dst_addr, bpp);

    // Mode 36 (transparent): skip write if src matches background.
    // At standard depths u.back_pen is the logical 8/16/32bpp value returned by
    // nqd_read_pixel(); 16bpp compares the raw 1555 word including bit 15.
    if (u.transfer_mode == 36) {
        if (src_pixel != u.back_pen) {
            nqd_write_pixel(buffer, dst_addr, bpp, src_pixel);
        }
        return;
    }

    // Mode 50 (hilite): one-directional replace background
    // colour -> highlight colour ONLY (IWQD 4-41/4-43). The prior
    // `else if (dst == hilite_color) -> back_pen` reverse arm corrupted dest
    // pixels that legitimately equalled the hilite colour; it is deleted so
    // such pixels are preserved. At standard depths u.back_pen is logical.
    if (u.transfer_mode == 50) {
        if (dst_pixel == u.back_pen) {
            nqd_write_pixel(buffer, dst_addr, bpp, u.hilite_color);
        }
        // else: leave dst unchanged (one-directional — no reverse arm)
        return;
    }

    // Arithmetic modes (32-35, 37-39): per-component operations
    uint4 sc = nqd_extract_components(src_pixel, bpp, u.bits_per_pixel);
    uint4 dc = nqd_extract_components(dst_pixel, bpp, u.bits_per_pixel);
    uint cmax = nqd_comp_max(bpp, u.bits_per_pixel);
    uint4 result;

    switch (u.transfer_mode) {
        case 32: {  // blend — weighted by per-channel rgbOpColor
            // uint4 lanes: x=R y=G z=B; w mirrors R (alpha follows red weight).
            uint4 w = uint4(u.blend_weight_r, u.blend_weight_g,
                            u.blend_weight_b, u.blend_weight_r);
            result = (sc * w + dc * (65535u - w)) / 65535u;
            break;
        }
        case 33: {  // addPin — add and clamp to max (white)
            result = min(sc + dc, uint4(cmax, cmax, cmax, cmax));
            break;
        }
        case 34: {  // addOver — add with modular wrap
            result = (sc + dc) & uint4(cmax, cmax, cmax, cmax);
            break;
        }
        case 35: {  // subPin — subtract and clamp to 0 (black)
            // Use int math to avoid underflow: max(dst - src, 0)
            result.x = (dc.x > sc.x) ? (dc.x - sc.x) : 0;
            result.y = (dc.y > sc.y) ? (dc.y - sc.y) : 0;
            result.z = (dc.z > sc.z) ? (dc.z - sc.z) : 0;
            result.w = (dc.w > sc.w) ? (dc.w - sc.w) : 0;
            break;
        }
        case 37: {  // adMax — component-wise maximum
            result = max(sc, dc);
            break;
        }
        case 38: {  // subOver — subtract with modular wrap
            result = (dc - sc) & uint4(cmax, cmax, cmax, cmax);
            break;
        }
        case 39: {  // adMin — component-wise minimum
            result = min(sc, dc);
            break;
        }
        default: {
            // Unknown mode — fall back to srcCopy for safety
            result = sc;
            break;
        }
    }

    nqd_write_pixel(buffer, dst_addr, bpp, nqd_pack_components(result, bpp, u.bits_per_pixel));
}

// ---------------------------------------------------------------------------
// nqd_fillrect — fill rect compute kernel (all 17 pen modes)
//
// Boolean modes (8-15): per-byte operations. Thread gid ranges over
// width_bytes * height. Each thread processes one byte. The fill byte is
// extracted from the repeating fill_color pattern (same as patCopy path).
//
// Arithmetic modes (32-39) and hilite (50): per-pixel operations. Thread gid
// ranges over width_pixels * height. Each thread processes one complete pixel
// using component helpers. "Source" is the fill_color.
//
// The host dispatches the correct total thread count based on mode family:
// - Modes 8-15: total = width_bytes * height
// - Modes 32-39, 50: total = width_pixels * height
// ---------------------------------------------------------------------------

kernel void nqd_fillrect(device uint8_t *buffer          [[buffer(0)]],
                          constant NQDFillRectUniforms &u [[buffer(1)]],
                          device uint8_t *mask_buffer     [[buffer(2)]],
                          uint gid                        [[thread_position_in_grid]])
{
    // --- Boolean modes (8-15): per-byte operations ---
    if (u.transfer_mode >= 8 && u.transfer_mode <= 15) {
        uint total = u.width_bytes * u.height;
        if (gid >= total) return;

        uint row = gid / u.width_bytes;
        uint col = gid % u.width_bytes;

        // Mask check for byte-mode. Standard depths use a pixel-column mask;
        // packed Boolean paths keep byte-column masks.
        if (u.mask_enabled) {
            uint mask_col = (u.bits_per_pixel >= 8) ? (col / u.pixel_size) : col;
            uint mask_addr = u.mask_offset + row * u.mask_stride + mask_col;
            if (mask_buffer[mask_addr] == 0) return;
        }

        uint dst_addr = u.dst_offset + row * uint(u.row_bytes) + col;

        // Extract the appropriate byte from the fill color pattern.
        // Big-endian byte order within the 32-bit word.
        uint byte_in_pixel = col % u.bpp;
        uint shift = (3 - ((4 - u.bpp) + byte_in_pixel)) * 8;
        uint8_t fill = uint8_t((u.fill_color >> shift) & 0xFF);

        uint8_t dst = buffer[dst_addr];

        switch (u.transfer_mode) {
            case 8:  buffer[dst_addr] = fill;              break;  // patCopy
            case 9:  buffer[dst_addr] = fill | dst;        break;  // patOr
            case 10: buffer[dst_addr] = fill ^ dst;        break;  // patXor
            case 11: buffer[dst_addr] = (~fill) & dst;     break;  // patBic
            case 12: buffer[dst_addr] = ~fill;             break;  // notPatCopy
            case 13: buffer[dst_addr] = (~fill) | dst;     break;  // notPatOr
            case 14: buffer[dst_addr] = (~fill) ^ dst;     break;  // notPatXor
            case 15: buffer[dst_addr] = fill & dst;        break;  // notPatBic
            default: buffer[dst_addr] = fill;              break;
        }
        return;
    }

    // --- Arithmetic modes (32-39) and hilite (50) ---
    // For packed depths (bits_per_pixel < 8), dispatch is per-byte with inner pixel loop.
    // For standard depths (>= 8), dispatch is per-pixel.

    if (u.bits_per_pixel < 8) {
        // --- Packed pixel fill arithmetic/hilite: per-byte with inner pixel loop ---
        uint packed_total = u.width_bytes * u.height;
        if (gid >= packed_total) return;

        uint p_row = gid / u.width_bytes;
        uint p_col = gid % u.width_bytes;  // byte column

        uint dst_byte_addr = u.dst_offset + p_row * uint(u.row_bytes) + p_col;

        uint pixels_per_byte = 8 / u.bits_per_pixel;
        uint bpp_local = u.bits_per_pixel;
        uint fill_mask_val = (1u << bpp_local) - 1;

        for (uint pi = 0; pi < pixels_per_byte; pi++) {
            if (u.mask_enabled) {
                uint pixel_col = p_col * pixels_per_byte + pi;
                uint mask_addr = u.mask_offset + p_row * u.mask_stride + pixel_col;
                if (mask_buffer[mask_addr] == 0) continue;
            }

            uint fill_val = u.fill_color & fill_mask_val;
            uint dst_val = nqd_read_packed_pixel(buffer, dst_byte_addr, pi, bpp_local);

            // 1-bit-destination arithmetic/hilite Table 4-2 behavior
            // (IWQD 4-40). On a 1-bit device each arithmetic/hilite mode
            // acts as its Boolean equivalent, NOT literal arithmetic on the 0/1
            // index. The "source" here is the fill pattern bit. Same routing as
            // the nqd_bitblt packed branch (incl. hilite(50) -> srcXor).
            if (bpp_local == 1) {
                uint b1_src = fill_val & 1u;
                uint b1_dst = dst_val & 1u;
                uint b1_out;
                switch (u.transfer_mode) {
                    case 32: b1_out = b1_src;              break;  // srcCopy
                    case 34:                                       // addOver
                    case 38:                                       // subOver
                    case 50: b1_out = b1_src ^ b1_dst;     break;  // hilite -> srcXor
                    case 33:                                       // addPin
                    case 37: b1_out = (~b1_src) & b1_dst & 1u; break;  // adMax -> srcBic
                    case 35:                                       // subPin
                    case 36:                                       // transparent
                    case 39: b1_out = b1_src | b1_dst;     break;  // adMin -> srcOr
                    default: b1_out = b1_src;              break;  // srcCopy fallback
                }
                nqd_write_packed_pixel(buffer, dst_byte_addr, pi, bpp_local, b1_out);
                continue;
            }

            // Mode 36 (transparent): skip if fill matches background.
            // back_pen index is the big-endian LSByte of
            // the htonl-packed pen via nqd_pen_index_packed() — NOT `back_pen &
            // fill_mask_val`, which reads the LOW bits and (for a non-lane-
            // replicated 2/4bpp index) mis-compares against the wrong value.
            // fill_val itself stays `fill_color & fill_mask_val`: fill_color is
            // the native (un-htonl'd) back_pen, so its index IS the low bits.
            if (u.transfer_mode == 36) {
                if (fill_val != nqd_pen_index_packed(u.back_pen, bpp_local)) {
                    nqd_write_packed_pixel(buffer, dst_byte_addr, pi, bpp_local, fill_val);
                }
                continue;
            }

            // Mode 50 (hilite): one-directional replace
            // background colour -> highlight colour ONLY (IWQD 4-41/4-43). The
            // prior `else if (dst == hilite_color) -> back_pen` reverse arm
            // corrupted dest pixels that legitimately equalled the hilite colour;
            // it is deleted so such pixels are preserved. (This 2/4-bpp packed
            // arm is reached only for bpp_local 2/4 — the 1-bit case is already
            // routed through the Table 4-2 srcXor behavior above.)
            // The COMPARED background pen (back_pen) is the
            // htonl-packed pen field, so its packed index is the big-endian LSByte
            // via nqd_pen_index_packed() — NOT the LOW bits of the packed word,
            // which mis-compared a non-lane-replicated 2/4bpp pen and replaced the
            // wrong pixels. hilite_color is DIFFERENT: the renderer builds it via
            // nqd_pack_hilite_color(bpp==1 for packed) as a raw low-byte index (it
            // is NOT htonl'd), so its index IS the low bits — only back_pen moves
            // to the byte-order-correct helper; hilite_color keeps the low-bit
            // mask (the documented index-approximation surface).
            if (u.transfer_mode == 50) {
                uint bp = nqd_pen_index_packed(u.back_pen, bpp_local);
                uint hc = u.hilite_color & fill_mask_val;
                if (dst_val == bp) {
                    nqd_write_packed_pixel(buffer, dst_byte_addr, pi, bpp_local, hc);
                }
                // else: leave dst unchanged (one-directional — no reverse arm)
                continue;
            }

            // Arithmetic modes
            uint4 fc = nqd_extract_components(fill_val, 1, bpp_local);
            uint4 dc = nqd_extract_components(dst_val, 1, bpp_local);
            uint cmax_p = nqd_comp_max(1, bpp_local);
            uint4 p_result;

            switch (u.transfer_mode) {
                case 32: { uint w = u.blend_weight_r; p_result = (fc * w + dc * (65535u - w)) / 65535u; break; }  // packed index-approx — red weight only
                case 33: p_result = min(fc + dc, uint4(cmax_p, cmax_p, cmax_p, cmax_p)); break;
                case 34: p_result = (fc + dc) & uint4(cmax_p, cmax_p, cmax_p, cmax_p); break;  // addOver: modular wrap
                case 35:
                    p_result.x = (dc.x > fc.x) ? (dc.x - fc.x) : 0;
                    p_result.y = p_result.z = p_result.w = 0;
                    break;
                case 37: p_result = max(fc, dc); break;
                case 38: p_result = (dc - fc) & uint4(cmax_p, cmax_p, cmax_p, cmax_p); break;  // subOver: modular wrap
                case 39: p_result = min(fc, dc); break;
                default: p_result = fc; break;
            }

            nqd_write_packed_pixel(buffer, dst_byte_addr, pi, bpp_local,
                                   nqd_pack_components(p_result, 1, bpp_local));
        }
        return;
    }

    // --- Standard depths (>= 8 bpp): per-pixel operations ---
    uint total = u.width_pixels * u.height;
    if (gid >= total) return;

    uint bpp = u.pixel_size;
    uint row = gid / u.width_pixels;
    uint col = gid % u.width_pixels;

    // Mask check for pixel-mode: col is pixel column, mask_stride is width_pixels
    if (u.mask_enabled) {
        uint mask_addr = u.mask_offset + row * u.mask_stride + col;
        if (mask_buffer[mask_addr] == 0) return;
    }

    uint dst_addr = u.dst_offset + row * uint(u.row_bytes) + col * bpp;

    // The "source" for fill is the fill_color (already packed as a pixel value)
    uint fill_pixel = u.fill_color;
    uint dst_pixel = nqd_read_pixel(buffer, dst_addr, bpp);

    // Mode 36 (transparent): skip write if fill matches background.
    // At standard depths u.back_pen is the logical 8/16/32bpp value returned by
    // nqd_read_pixel(); 16bpp compares the raw 1555 word including bit 15.
    if (u.transfer_mode == 36) {
        if (fill_pixel != u.back_pen) {
            nqd_write_pixel(buffer, dst_addr, bpp, fill_pixel);
        }
        return;
    }

    // Mode 50 (hilite): one-directional replace background
    // colour -> highlight colour ONLY (IWQD 4-41/4-43). The prior
    // `else if (dst == hilite_color) -> back_pen` reverse arm corrupted dest
    // pixels that legitimately equalled the hilite colour; it is deleted so
    // such pixels are preserved. At standard depths u.back_pen is logical.
    if (u.transfer_mode == 50) {
        if (dst_pixel == u.back_pen) {
            nqd_write_pixel(buffer, dst_addr, bpp, u.hilite_color);
        }
        // else: leave dst unchanged (one-directional — no reverse arm)
        return;
    }

    // Arithmetic modes (32-35, 37-39): per-component operations
    uint4 fc = nqd_extract_components(fill_pixel, bpp, u.bits_per_pixel);
    uint4 dc = nqd_extract_components(dst_pixel, bpp, u.bits_per_pixel);
    uint cmax = nqd_comp_max(bpp, u.bits_per_pixel);
    uint4 result;

    switch (u.transfer_mode) {
        case 32: {  // blend — weighted by per-channel rgbOpColor
            // uint4 lanes: x=R y=G z=B; w mirrors R (alpha follows red weight).
            uint4 w = uint4(u.blend_weight_r, u.blend_weight_g,
                            u.blend_weight_b, u.blend_weight_r);
            result = (fc * w + dc * (65535u - w)) / 65535u;
            break;
        }
        case 33: {  // addPin — add and clamp to max
            result = min(fc + dc, uint4(cmax, cmax, cmax, cmax));
            break;
        }
        case 34: {  // addOver — add with modular wrap
            // IWQD 4-40 addOver: modular wrap (fc+dc)&cmax, NOT saturating
            // min() (that is addPin behaviour). Matches the codebase's own
            // bitblt addOver (case 34) and FillRect packed addOver.
            result = (fc + dc) & uint4(cmax, cmax, cmax, cmax);
            break;
        }
        case 35: {  // subPin — subtract and clamp to 0
            result.x = (dc.x > fc.x) ? (dc.x - fc.x) : 0;
            result.y = (dc.y > fc.y) ? (dc.y - fc.y) : 0;
            result.z = (dc.z > fc.z) ? (dc.z - fc.z) : 0;
            result.w = (dc.w > fc.w) ? (dc.w - fc.w) : 0;
            break;
        }
        case 37: {  // adMax — component-wise maximum
            result = max(fc, dc);
            break;
        }
        case 38: {  // subOver — subtract with modular wrap
            result = (dc - fc) & uint4(cmax, cmax, cmax, cmax);
            break;
        }
        case 39: {  // adMin — component-wise minimum
            result = min(fc, dc);
            break;
        }
        default: {
            // Unknown mode — fall back to fill copy for safety
            result = fc;
            break;
        }
    }

    nqd_write_pixel(buffer, dst_addr, bpp, nqd_pack_components(result, bpp, u.bits_per_pixel));
}

// ---------------------------------------------------------------------------
// nqd_bitblt_scaled — scaling bitblt compute kernel (DSpBlit_Faster, sub-op 710)
//
// Sibling to nqd_bitblt (which is strictly 1:1). One thread per DST
// pixel; the source coordinate is derived by integer scaling:
//   - nearest-neighbor by default (sx=(dx*src_w)/dst_w, sy=(dy*src_h)/dst_h)
//   - bilinear of the 4 neighbors only when u.interpolate is set
//     (kDSpBlitMode_Interpolation, PDF p.87 — A5 CONFIRMED nearest default)
// Color-key transparency (DSpBlitMode SrcKey/DstKey) is honored when
// u.key_enable carries the corresponding bit: SrcKey skips writing src pixels
// matching u.src_key; DstKey skips overwriting dst pixels matching u.dst_key.
//
// Pixels are addressed via the same big-endian nqd_read_pixel/nqd_write_pixel
// helpers as nqd_bitblt for standard depths (>= 8 bpp, pixel_size 1/2/4
// bytes). The DSp blit surfaces are 32-bpp BGRA8Unorm (compositor
// contract) so the standard-depth path is the common case; the kernel also
// works for 8/16-bpp packed-byte surfaces (pixel_size 1/2).
// ---------------------------------------------------------------------------

// Uniform struct — must match NQDBitbltScaledUniforms in nqd_metal_renderer.mm
struct NQDBitbltScaledUniforms {
    uint src_offset;     // byte offset of src rect origin within the RAM buffer
    uint dst_offset;     // byte offset of dst rect origin within the RAM buffer
    int  src_row_bytes;
    int  dst_row_bytes;
    uint pixel_size;     // bytes per pixel (1, 2, or 4)
    uint bits_per_pixel; // raw pixel depth in bits (8, 16, or 32)
    uint src_w;          // source rect width in pixels
    uint src_h;          // source rect height in pixels
    uint dst_w;          // dest rect width in pixels
    uint dst_h;          // dest rect height in pixels
    uint interpolate;    // 1 = bilinear (kDSpBlitMode_Interpolation), 0 = nearest
    uint src_key;        // source color key (big-endian packed pixel value)
    uint dst_key;        // dest color key (big-endian packed pixel value)
    uint key_enable;     // bit-or of kDSpBlitMode_SrcKey(1) / _DstKey(2); 0 = no key
    uint ram_size;       // defense-in-depth: total mapped RAM bytes (clamp)
};

kernel void nqd_bitblt_scaled(device uint8_t *buffer                  [[buffer(0)]],
                              constant NQDBitbltScaledUniforms &u     [[buffer(1)]],
                              uint gid                                [[thread_position_in_grid]])
{
    uint total = u.dst_w * u.dst_h;
    if (gid >= total) return;
    if (u.dst_w == 0 || u.dst_h == 0 || u.src_w == 0 || u.src_h == 0) return;

    uint bpp = u.pixel_size;
    uint dx  = gid % u.dst_w;
    uint dy  = gid / u.dst_w;

    // SrcKey(1<<0), DstKey(1<<1); Interpolation handled separately.
    bool src_key_on = (u.key_enable & 1u) != 0u;
    bool dst_key_on = (u.key_enable & 2u) != 0u;

    uint dst_addr = u.dst_offset + dy * uint(u.dst_row_bytes) + dx * bpp;

    // Defense-in-depth: the host validated the blit extent against
    // ram_size before dispatch, but the kernel still trusts the resolved
    // geometry. Clamp every per-pixel access so a host-side mismatch cannot
    // turn into an OOB GPU read/write — out-of-range threads simply no-op.
    if (u.ram_size != 0u && (dst_addr + bpp) > u.ram_size) return;

    // DstKey: do not overwrite a dst pixel that matches the dest color key.
    if (dst_key_on) {
        uint dst_existing = nqd_read_pixel(buffer, dst_addr, bpp);
        if (dst_existing == u.dst_key) return;
    }

    uint out_pixel;

    if (u.interpolate != 0u && bpp == 4) {
        // Bilinear of the 4 source neighbors. Only meaningful for direct-color
        // (32-bpp BGRA) where component blending is valid; indexed/packed
        // depths fall through to nearest (blending palette indices is wrong).
        float fx = (float(dx) + 0.5f) * float(u.src_w) / float(u.dst_w) - 0.5f;
        float fy = (float(dy) + 0.5f) * float(u.src_h) / float(u.dst_h) - 0.5f;
        if (fx < 0.0f) fx = 0.0f;
        if (fy < 0.0f) fy = 0.0f;
        uint sx0 = uint(fx);
        uint sy0 = uint(fy);
        uint sx1 = (sx0 + 1u < u.src_w) ? (sx0 + 1u) : sx0;
        uint sy1 = (sy0 + 1u < u.src_h) ? (sy0 + 1u) : sy0;
        float wx = fx - float(sx0);
        float wy = fy - float(sy0);

        uint a00 = u.src_offset + sy0 * uint(u.src_row_bytes) + sx0 * bpp;
        uint a10 = u.src_offset + sy0 * uint(u.src_row_bytes) + sx1 * bpp;
        uint a01 = u.src_offset + sy1 * uint(u.src_row_bytes) + sx0 * bpp;
        uint a11 = u.src_offset + sy1 * uint(u.src_row_bytes) + sx1 * bpp;

        // Defense-in-depth: clamp the source neighbor reads.
        if (u.ram_size != 0u &&
            ((a00 + bpp) > u.ram_size || (a10 + bpp) > u.ram_size ||
             (a01 + bpp) > u.ram_size || (a11 + bpp) > u.ram_size)) return;

        uint p00 = nqd_read_pixel(buffer, a00, bpp);

        // SrcKey: if the nearest source sample is the key, skip the write.
        if (src_key_on && p00 == u.src_key) return;

        uint p10 = nqd_read_pixel(buffer, a10, bpp);
        uint p01 = nqd_read_pixel(buffer, a01, bpp);
        uint p11 = nqd_read_pixel(buffer, a11, bpp);

        // Extract per-component values (32-bpp big-endian [A][R][G][B]) + blend.
        uint4 c00 = nqd_extract_components(p00, bpp, u.bits_per_pixel);
        uint4 c10 = nqd_extract_components(p10, bpp, u.bits_per_pixel);
        uint4 c01 = nqd_extract_components(p01, bpp, u.bits_per_pixel);
        uint4 c11 = nqd_extract_components(p11, bpp, u.bits_per_pixel);

        float4 top    = mix(float4(c00), float4(c10), wx);
        float4 bottom = mix(float4(c01), float4(c11), wx);
        float4 blended = mix(top, bottom, wy);
        uint4 outc = uint4(blended + float4(0.5f, 0.5f, 0.5f, 0.5f));

        out_pixel = nqd_pack_components(outc, bpp, u.bits_per_pixel);
    } else {
        // Nearest-neighbor (default; the only correct mode for indexed/packed).
        uint sx = (dx * u.src_w) / u.dst_w;
        uint sy = (dy * u.src_h) / u.dst_h;
        if (sx >= u.src_w) sx = u.src_w - 1u;
        if (sy >= u.src_h) sy = u.src_h - 1u;
        uint src_addr = u.src_offset + sy * uint(u.src_row_bytes) + sx * bpp;
        // Defense-in-depth: clamp the source read.
        if (u.ram_size != 0u && (src_addr + bpp) > u.ram_size) return;
        out_pixel = nqd_read_pixel(buffer, src_addr, bpp);

        // SrcKey: skip writing transparent source pixels.
        if (src_key_on && out_pixel == u.src_key) return;
    }

    nqd_write_pixel(buffer, dst_addr, bpp, out_pixel);
}

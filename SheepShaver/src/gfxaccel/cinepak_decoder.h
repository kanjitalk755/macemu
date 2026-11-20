/*
 *  cinepak_decoder.h - Native Cinepak ('cvid') decoder for QuickTime movies
 *
 *  Self-contained host-side decoder. Bitstream layout follows the published
 *  Cinepak description (frame header: flags byte + 24-bit length + dims +
 *  strip count; per-strip codebook chunks 0x20xx and vector chunks 0x30xx;
 *  6-byte YUV codebook entries with R=Y+2V, G=Y-U/2-V, B=Y+2U).
 *
 *  The context is persistent across frames: codebooks are per-strip state
 *  carried between frames, and inter (delta) frames only update a subset of
 *  4x4 blocks, so the context keeps the full previous frame internally.
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

#ifndef CINEPAK_DECODER_H
#define CINEPAK_DECODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cinepak_context cinepak_context_t;

/* Create a decoder for width x height frames. The internal frame store is
 * XRGB8888 (host-endian uint32 0x00RRGGBB); conversion to the guest pixmap
 * format happens at blit time. */
cinepak_context_t *cinepak_create(int width, int height);
void cinepak_destroy(cinepak_context_t *ctx);

int cinepak_width(const cinepak_context_t *ctx);
int cinepak_height(const cinepak_context_t *ctx);

/*
 * Decode one Cinepak frame into the context's internal frame store.
 * Returns 0 on success, negative on malformed data. On success,
 * cinepak_frame() returns the up-to-date frame.
 */
int cinepak_decode_frame(cinepak_context_t *ctx,
						 const uint8_t *src, size_t src_size);

/* Pointer to the internal frame store: width*height uint32 0x00RRGGBB. */
const uint32_t *cinepak_frame(const cinepak_context_t *ctx);

/* Was the most recently decoded frame a keyframe? */
int cinepak_last_frame_was_key(const cinepak_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CINEPAK_DECODER_H */

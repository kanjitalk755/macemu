/*
 *  cinepak_decoder.cpp - Native Cinepak ('cvid') decoder
 *
 *  Bitstream layout (verified against the multimedia.cx description and the
 *  behavior of open-source reference decoders):
 *
 *  Frame header (10 bytes):
 *      byte  0     frame flags (bit 0 clear = strips i>0 inherit strip i-1's
 *                  codebooks before applying their own chunks)
 *      bytes 1-3   24-bit big-endian encoded frame length
 *      bytes 4-5   width, 6-7 height, 8-9 strip count
 *
 *  Strip header (12 bytes):
 *      byte  0     strip id (0x10 intra, 0x11 inter)
 *      bytes 2-3   strip size including this header
 *      bytes 4-5   y1 (0 means "starts where the previous strip ended";
 *                  then bytes 8-9 hold the strip HEIGHT, not y2)
 *      bytes 6-7   x1, 8-9 y2, 10-11 x2
 *
 *  Chunk header (4 bytes): byte 0 chunk id, bytes 1-3 24-bit size incl. hdr.
 *      0x20/0x21/0x24/0x25  V4 codebook (full/partial, color/gray)
 *      0x22/0x23/0x26/0x27  V1 codebook (full/partial, color/gray)
 *      0x30/0x31/0x32       vectors (intra / inter / intra-V1-only)
 *
 *  Codebook entries: 6 bytes color (4 luma + signed U + signed V) or 4 bytes
 *  grayscale. Color conversion: R = Y + 2V, G = Y - U/2 - V, B = Y + 2U.
 *
 *  Codebooks are PER-STRIP state persisting across frames; inter frames only
 *  touch flagged 4x4 blocks, so the full previous frame is kept in the
 *  context. Both properties make the context long-lived per sequence.
 */

#include "sysdeps.h"
#include "cinepak_decoder.h"
#include <string.h>
#include <stdlib.h>

#define CINEPAK_MAX_STRIPS 32

/* One codebook entry: 4 pixels as host-endian 0x00RRGGBB. A V1 entry paints
 * a 4x4 block as four 2x2 solid quadrants; a V4 quadrant paints 2x2 pixels
 * directly. Pixel order within the entry: [top-left, top-right,
 * bottom-left, bottom-right]. */
struct cinepak_cb_entry {
	uint32_t pix[4];
};

struct cinepak_strip_state {
	struct cinepak_cb_entry v1[256];
	struct cinepak_cb_entry v4[256];
};

struct cinepak_context {
	int width, height;
	uint32_t *frame;                 /* width*height 0x00RRGGBB */
	struct cinepak_strip_state strips[CINEPAK_MAX_STRIPS];
	int last_key;
};

static inline uint32_t read_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint32_t read_be24(const uint8_t *p)
{
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static inline uint16_t read_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint8_t clamp_u8(int v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

cinepak_context_t *cinepak_create(int width, int height)
{
	if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
		return NULL;
	struct cinepak_context *ctx =
		(struct cinepak_context *)calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;
	ctx->width = width;
	ctx->height = height;
	ctx->frame = (uint32_t *)calloc((size_t)width * height, sizeof(uint32_t));
	if (!ctx->frame) {
		free(ctx);
		return NULL;
	}
	return ctx;
}

void cinepak_destroy(cinepak_context_t *ctx)
{
	if (!ctx)
		return;
	free(ctx->frame);
	free(ctx);
}

int cinepak_width(const cinepak_context_t *ctx)  { return ctx ? ctx->width : 0; }
int cinepak_height(const cinepak_context_t *ctx) { return ctx ? ctx->height : 0; }
const uint32_t *cinepak_frame(const cinepak_context_t *ctx) { return ctx ? ctx->frame : NULL; }
int cinepak_last_frame_was_key(const cinepak_context_t *ctx) { return ctx ? ctx->last_key : 0; }

/* Decode a codebook chunk into cb[256]. chunk_id bits: 0x01 = partial update
 * (32-bit selection masks), 0x04 = 4-byte grayscale entries. */
static void decode_codebook(struct cinepak_cb_entry *cb, int chunk_id,
                            const uint8_t *data, int size)
{
	const uint8_t *end = data + size;
	const int gray = (chunk_id & 0x04) != 0;
	const int partial = (chunk_id & 0x01) != 0;
	const int entry_size = gray ? 4 : 6;
	uint32_t flag = 0, mask = 0;

	for (int i = 0; i < 256; i++) {
		if (partial) {
			mask >>= 1;
			if (mask == 0) {
				if (data + 4 > end)
					return;
				flag = read_be32(data);
				data += 4;
				mask = 0x80000000u;
			}
			if (!(flag & mask))
				continue;
		}
		if (data + entry_size > end)
			return;
		if (gray) {
			for (int k = 0; k < 4; k++) {
				uint32_t y = data[k];
				cb[i].pix[k] = (y << 16) | (y << 8) | y;
			}
		} else {
			const int u = (int8_t)data[4];
			const int v = (int8_t)data[5];
			for (int k = 0; k < 4; k++) {
				const int y = data[k];
				const uint32_t r = clamp_u8(y + 2 * v);
				const uint32_t g = clamp_u8(y - u / 2 - v);
				const uint32_t b = clamp_u8(y + 2 * u);
				cb[i].pix[k] = (r << 16) | (g << 8) | b;
			}
		}
		data += entry_size;
	}
}

/* Paint one 4x4 block at pixel (bx,by) from a V1 entry: each entry pixel
 * covers a 2x2 quadrant. Clips against frame bounds. */
static void paint_v1(struct cinepak_context *ctx, int bx, int by,
                     const struct cinepak_cb_entry *e)
{
	for (int y = 0; y < 4; y++) {
		const int py = by + y;
		if (py >= ctx->height)
			break;
		uint32_t *row = ctx->frame + (size_t)py * ctx->width;
		const int q = (y >> 1) * 2;
		for (int x = 0; x < 4; x++) {
			const int px = bx + x;
			if (px >= ctx->width)
				break;
			row[px] = e->pix[q + (x >> 1)];
		}
	}
}

/* Paint one 4x4 block from four V4 entries (TL, TR, BL, BR quadrants); each
 * entry's 4 pixels map 1:1 onto its 2x2 quadrant. */
static void paint_v4(struct cinepak_context *ctx, int bx, int by,
                     const struct cinepak_cb_entry *e0,
                     const struct cinepak_cb_entry *e1,
                     const struct cinepak_cb_entry *e2,
                     const struct cinepak_cb_entry *e3)
{
	const struct cinepak_cb_entry *quads[4] = { e0, e1, e2, e3 };
	for (int y = 0; y < 4; y++) {
		const int py = by + y;
		if (py >= ctx->height)
			break;
		uint32_t *row = ctx->frame + (size_t)py * ctx->width;
		for (int x = 0; x < 4; x++) {
			const int px = bx + x;
			if (px >= ctx->width)
				break;
			const struct cinepak_cb_entry *e = quads[(y >> 1) * 2 + (x >> 1)];
			row[px] = e->pix[(y & 1) * 2 + (x & 1)];
		}
	}
}

/* Decode a vector chunk (0x30/0x31/0x32) for one strip.
 * Bit stream: 32-bit big-endian words, MSB first, shared between the
 * "block updated?" level (inter only) and the "V1 or V4?" level. */
static int decode_vectors(struct cinepak_context *ctx,
                          struct cinepak_strip_state *strip, int chunk_id,
                          const uint8_t *data, int size,
                          int x1, int y1, int x2, int y2)
{
	const uint8_t *end = data + size;
	const int inter = (chunk_id & 0x01) != 0;   /* 0x31: skip bits present */
	const int v1_only = (chunk_id & 0x02) != 0; /* 0x32: no V1/V4 bits */
	uint32_t flag = 0, mask = 0;

	for (int by = y1; by < y2; by += 4) {
		for (int bx = x1; bx < x2; bx += 4) {
			if (inter) {
				mask >>= 1;
				if (mask == 0) {
					if (data + 4 > end)
						return -1;
					flag = read_be32(data);
					data += 4;
					mask = 0x80000000u;
				}
				if (!(flag & mask))
					continue; /* block unchanged from previous frame */
			}

			int use_v4 = 0;
			if (!v1_only) {
				mask >>= 1;
				if (mask == 0) {
					if (data + 4 > end)
						return -1;
					flag = read_be32(data);
					data += 4;
					mask = 0x80000000u;
				}
				use_v4 = (flag & mask) != 0;
			}

			if (use_v4) {
				if (data + 4 > end)
					return -1;
				paint_v4(ctx, bx, by,
				         &strip->v4[data[0]], &strip->v4[data[1]],
				         &strip->v4[data[2]], &strip->v4[data[3]]);
				data += 4;
			} else {
				if (data + 1 > end)
					return -1;
				paint_v1(ctx, bx, by, &strip->v1[data[0]]);
				data += 1;
			}
		}
	}
	return 0;
}

static int decode_strip(struct cinepak_context *ctx,
                        struct cinepak_strip_state *strip,
                        const uint8_t *data, int size,
                        int x1, int y1, int x2, int y2)
{
	const uint8_t *end = data + size;

	while (data + 4 <= end) {
		const int chunk_id = data[0];
		int chunk_size = (int)read_be24(data + 1) - 4;
		data += 4;
		if (chunk_size < 0)
			return -1;
		if (data + chunk_size > end)
			chunk_size = (int)(end - data);

		switch (chunk_id) {
		case 0x20: case 0x21: case 0x24: case 0x25:
			decode_codebook(strip->v4, chunk_id, data, chunk_size);
			break;
		case 0x22: case 0x23: case 0x26: case 0x27:
			decode_codebook(strip->v1, chunk_id, data, chunk_size);
			break;
		case 0x30: case 0x31: case 0x32:
			/* Vector chunk terminates the strip payload. */
			return decode_vectors(ctx, strip, chunk_id, data, chunk_size,
			                      x1, y1, x2, y2);
		default:
			/* Unknown chunk: skip. */
			break;
		}
		data += chunk_size;
	}
	return -1; /* no vector chunk found */
}

int cinepak_decode_frame(cinepak_context_t *ctx,
                         const uint8_t *src, size_t src_size)
{
	if (!ctx || !src || src_size < 10)
		return -1;

	const uint8_t *end = src + src_size;
	const int frame_flags = src[0];
	int num_strips = read_be16(src + 8);
	const uint8_t *data = src + 10;
	int y0 = 0;

	if (num_strips > CINEPAK_MAX_STRIPS)
		num_strips = CINEPAK_MAX_STRIPS;

	ctx->last_key = 0;

	for (int i = 0; i < num_strips; i++) {
		if (data + 12 > end)
			return -1;

		const int strip_id = data[0];
		int strip_size = (int)read_be16(data + 2) - 12;
		int y1 = read_be16(data + 4);
		const int x1 = read_be16(data + 6);
		int y2 = read_be16(data + 8);
		const int x2 = read_be16(data + 10);

		/* Zero y1 means "relative to where the previous strip ended";
		 * bytes 8-9 then hold the strip height. */
		if (y1 == 0) {
			y1 = y0;
			y2 = y0 + y2;
		}

		if (strip_id == 0x10)
			ctx->last_key = 1;

		if (strip_size < 0)
			return -1;
		data += 12;
		if (data + strip_size > end)
			strip_size = (int)(end - data);

		/* Strips are coded on 4-aligned dimensions, so rects may exceed the
		 * nominal image size by up to 3 pixels; painting clips per-pixel. */
		const int pad_w = (ctx->width + 3) & ~3;
		const int pad_h = (ctx->height + 3) & ~3;
		if (x1 >= x2 || y1 >= y2 || x2 > pad_w || y2 > pad_h)
			return -1;

		/* Strips i>0 inherit strip i-1's codebooks unless frame flag bit 0
		 * is set (the codec-quirk everyone implements). */
		if (i > 0 && !(frame_flags & 0x01))
			ctx->strips[i] = ctx->strips[i - 1];

		if (decode_strip(ctx, &ctx->strips[i], data, strip_size,
		                 x1, y1, x2, y2) < 0)
			return -1;

		data += strip_size;
		y0 = y2;
	}
	return 0;
}

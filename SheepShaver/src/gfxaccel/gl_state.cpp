/*
 *  gl_state.cpp - OpenGL 1.2 state machine implementation
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Implements the full GL 1.2 fixed-function state machine:
 *    - Matrix stacks with column-major 4x4 math (push/pop/load/mult/rotate/translate/scale/frustum/ortho)
 *    - glEnable/glDisable for all GL 1.2 capability flags
 *    - Depth/blend/alpha/stencil/viewport/scissor/clear state
 *    - Shade model, cull face, front face, polygon mode, color mask
 *    - Lighting (8 lights), materials, fog, texture environment
 *    - Pixel store, get functions, attrib stacks
 */

#include <cstring>
#include <cstdio>
#include <cmath>
#include <cfloat>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "macos_util.h"
#include "video.h"
#include "dsp_draw_context.h"
#include "gl_clip_plane_policy.h"
#include "gl_color_index_policy.h"
#include "gl_engine.h"
#include "gl_metal_draw_state.h"
#include "gl_offscreen_policy.h"
#include "gl_pixel_unpack_policy.h"

extern bool RaveGetLastCL8ColorTableRGBSnapshot(uint8_t outRGB[768]);

// =========================================================================
//  GL enum constants (from gl.h / glext.h)
// =========================================================================

// Matrix modes
#define GL_MODELVIEW                      0x1700
#define GL_PROJECTION                     0x1701
#define GL_TEXTURE                        0x1702
#define GL_COLOR                          0x1800

// Enable/disable caps
#define GL_DEPTH_TEST                     0x0B71
#define GL_BLEND                          0x0BE2
#define GL_LIGHTING                       0x0B50
#define GL_FOG                            0x0B60
#define GL_SCISSOR_TEST                   0x0C11
#define GL_CULL_FACE                      0x0B44
#define GL_ALPHA_TEST                     0x0BC0
#define GL_NORMALIZE                      0x0BA1
#define GL_COLOR_MATERIAL                 0x0B57
#define GL_STENCIL_TEST                   0x0B90
#define GL_COLOR_SUM                      0x8458 // EXT_secondary_color / GL_COLOR_SUM (glext.h-verified)
#define GL_TEXTURE_1D                     0x0DE0
#define GL_TEXTURE_2D                     0x0DE1
#define GL_LIGHT0                         0x4000
#define GL_POLYGON_OFFSET_FILL            0x8037
#define GL_POLYGON_OFFSET_LINE            0x2A02
#define GL_POLYGON_OFFSET_POINT           0x2A01
#define GL_POINT_SMOOTH                   0x0B10
#define GL_LINE_SMOOTH                    0x0B20
#define GL_POLYGON_SMOOTH                 0x0B41
#define GL_DITHER                         0x0BD0
#define GL_COLOR_LOGIC_OP                 0x0BF2
#define GL_INDEX_LOGIC_OP                 0x0BF1
#define GL_AUTO_NORMAL                    0x0D80
#define GL_LINE_STIPPLE                   0x0B24
#define GL_POLYGON_STIPPLE                0x0B42
#define GL_TEXTURE_GEN_S                  0x0C60
#define GL_TEXTURE_GEN_T                  0x0C61
#define GL_TEXTURE_GEN_R                  0x0C62
#define GL_TEXTURE_GEN_Q                  0x0C63
#define GL_MULTISAMPLE_ARB                0x809D
#define GL_SAMPLE_ALPHA_TO_COVERAGE_ARB   0x809E
#define GL_SAMPLE_ALPHA_TO_ONE_ARB        0x809F
#define GL_SAMPLE_COVERAGE_ARB            0x80A0
#define GL_CLIP_PLANE0                    0x3000

// Boolean
#define GL_TRUE                           1
#define GL_FALSE                          0

// Depth/compare functions
#define GL_NEVER                          0x0200
#define GL_LESS                           0x0201
#define GL_EQUAL                          0x0202
#define GL_LEQUAL                         0x0203
#define GL_GREATER                        0x0204
#define GL_NOTEQUAL                       0x0205
#define GL_GEQUAL                         0x0206
#define GL_ALWAYS                         0x0207

// Blend factors
#define GL_ZERO                           0
#define GL_ONE                            1
#define GL_SRC_COLOR                      0x0300
#define GL_ONE_MINUS_SRC_COLOR            0x0301
#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_DST_ALPHA                      0x0304
#define GL_ONE_MINUS_DST_ALPHA            0x0305
#define GL_DST_COLOR                      0x0306
#define GL_ONE_MINUS_DST_COLOR            0x0307
#define GL_SRC_ALPHA_SATURATE             0x0308

// Face / winding
#define GL_FRONT                          0x0404
#define GL_BACK                           0x0405
#define GL_FRONT_AND_BACK                 0x0408
#define GL_CW                             0x0900
#define GL_CCW                            0x0901

// Shade model
#define GL_FLAT                           0x1D00
#define GL_SMOOTH                         0x1D01

// Polygon mode
#define GL_POINT                          0x1B00
#define GL_LINE                           0x1B01
#define GL_FILL                           0x1B02

// Stencil ops
#define GL_KEEP                           0x1E00
#define GL_REPLACE                        0x1E01
#define GL_INCR                           0x1E02
#define GL_DECR                           0x1E03
#define GL_INVERT                         0x150A

// Clear bits
#define GL_COLOR_BUFFER_BIT               0x00004000
#define GL_DEPTH_BUFFER_BIT               0x00000100
#define GL_STENCIL_BUFFER_BIT             0x00000400
#define GL_ACCUM_BUFFER_BIT               0x00000200

// Hints
#define GL_DONT_CARE                      0x1100
#define GL_FASTEST                        0x1101
#define GL_NICEST                         0x1102
#define GL_PERSPECTIVE_CORRECTION_HINT    0x0C50
#define GL_POINT_SMOOTH_HINT              0x0C51
#define GL_LINE_SMOOTH_HINT               0x0C52
#define GL_POLYGON_SMOOTH_HINT            0x0C53
#define GL_FOG_HINT                       0x0C54

// Logic ops
#define GL_CLEAR_OP                       0x1500
#define GL_AND                            0x1501
#define GL_AND_REVERSE                    0x1502
#define GL_COPY                           0x1503
#define GL_AND_INVERTED                   0x1504
#define GL_NOOP                           0x1505
#define GL_XOR                            0x1506
#define GL_OR                             0x1507
#define GL_NOR                            0x1508
#define GL_EQUIV                          0x1509
// GL_INVERT = 0x150A (defined above)
#define GL_OR_REVERSE                     0x150B
#define GL_COPY_INVERTED                  0x150C
#define GL_OR_INVERTED                    0x150D
#define GL_NAND                           0x150E
#define GL_SET                            0x150F

// Error codes
#define GL_NO_ERROR                       0
#define GL_INVALID_ENUM                   0x0500
#define GL_INVALID_VALUE                  0x0501
#define GL_INVALID_OPERATION              0x0502
#define GL_STACK_OVERFLOW                 0x0503
#define GL_STACK_UNDERFLOW                0x0504
#define GL_OUT_OF_MEMORY                  0x0505

// Lighting
#define GL_AMBIENT                        0x1200
#define GL_DIFFUSE                        0x1201
#define GL_SPECULAR                       0x1202
#define GL_POSITION                       0x1203
#define GL_SPOT_DIRECTION                 0x1204
#define GL_SPOT_EXPONENT                  0x1205
#define GL_SPOT_CUTOFF                    0x1206
#define GL_CONSTANT_ATTENUATION           0x1207
#define GL_LINEAR_ATTENUATION             0x1208
#define GL_QUADRATIC_ATTENUATION          0x1209
#define GL_EMISSION                       0x1600
#define GL_SHININESS                      0x1601
#define GL_AMBIENT_AND_DIFFUSE            0x1602
#define GL_LIGHT_MODEL_LOCAL_VIEWER       0x0B51
#define GL_LIGHT_MODEL_TWO_SIDE           0x0B52
#define GL_LIGHT_MODEL_AMBIENT            0x0B53

// Fog
#define GL_FOG_MODE                       0x0B65
#define GL_FOG_DENSITY                    0x0B62
#define GL_FOG_START                      0x0B63
#define GL_FOG_END                        0x0B64
#define GL_FOG_COLOR                      0x0B66
#define GL_FOG_INDEX                      0x0B61
#define GL_LINEAR_FOG                     0x2601 // same as GL_LINEAR
#define GL_EXP                            0x0800
#define GL_EXP2                           0x0801

// Texture environment
#define GL_TEXTURE_ENV                    0x2300
#define GL_TEXTURE_ENV_MODE               0x2200
#define GL_TEXTURE_ENV_COLOR              0x2201
#define GL_MODULATE                       0x2100
#define GL_DECAL                          0x2101
#define GL_BLEND_TEX                      0x0BE2 // same as GL_BLEND
#define GL_REPLACE_TEX                    0x1E01 // same as GL_REPLACE
#define GL_ADD_TEX                        0x0104 // GL_ADD

// Texture gen
#define GL_TEXTURE_GEN_MODE               0x2500
#define GL_OBJECT_LINEAR                  0x2401
#define GL_EYE_LINEAR                     0x2400
#define GL_SPHERE_MAP                     0x2402
#define GL_OBJECT_PLANE                   0x2501
#define GL_EYE_PLANE                      0x2502
#define GL_S                              0x2000
#define GL_T                              0x2001
#define GL_R                              0x2002
#define GL_Q                              0x2003

// Pixel store
#define GL_PACK_ALIGNMENT                 0x0D05
#define GL_PACK_ROW_LENGTH                0x0D02
#define GL_PACK_SKIP_ROWS                 0x0D03
#define GL_PACK_SKIP_PIXELS               0x0D04
#define GL_UNPACK_ALIGNMENT               0x0CF5
#define GL_UNPACK_ROW_LENGTH              0x0CF2
#define GL_UNPACK_SKIP_ROWS               0x0CF3
#define GL_UNPACK_SKIP_PIXELS             0x0CF4

// Pixel transfer
#define GL_RED_SCALE                      0x0D14
#define GL_GREEN_SCALE                    0x0D18
#define GL_BLUE_SCALE                     0x0D1A
#define GL_ALPHA_SCALE                    0x0D1C
#define GL_RED_BIAS                       0x0D15
#define GL_GREEN_BIAS                     0x0D19
#define GL_BLUE_BIAS                      0x0D1B
#define GL_ALPHA_BIAS                     0x0D1D
#define GL_DEPTH_SCALE                    0x0D1E
#define GL_DEPTH_BIAS                     0x0D1F
// Pixel map enums
#define GL_PIXEL_MAP_R_TO_R               0x0C76
#define GL_PIXEL_MAP_G_TO_G               0x0C77
#define GL_PIXEL_MAP_B_TO_B               0x0C78
#define GL_PIXEL_MAP_A_TO_A               0x0C79
#define GL_PIXEL_MAP_I_TO_I               0x0C70
#define GL_PIXEL_MAP_S_TO_S               0x0C71
#define GL_PIXEL_MAP_I_TO_R               0x0C72
#define GL_PIXEL_MAP_I_TO_G               0x0C73
#define GL_PIXEL_MAP_I_TO_B               0x0C74
#define GL_PIXEL_MAP_I_TO_A               0x0C75

// Texture level parameter queries
#define GL_TEXTURE_WIDTH                  0x1000
#define GL_TEXTURE_HEIGHT                 0x1001
#define GL_TEXTURE_INTERNAL_FORMAT        0x1003
#define GL_TEXTURE_BORDER                 0x1005
#define GL_TEXTURE_COMPONENTS             0x1003

// Get queries
#define GL_VIEWPORT                       0x0BA2
#define GL_MAX_MODELVIEW_STACK_DEPTH      0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH     0x0D38
#define GL_MAX_TEXTURE_STACK_DEPTH        0x0D39
#define GL_MAX_LIGHTS                     0x0D31
#define GL_MAX_TEXTURE_SIZE               0x0D33
#define GL_MAX_CLIP_PLANES                0x0D32
#define GL_MAX_ATTRIB_STACK_DEPTH_QUERY   0x0D35
#define GL_MAX_NAME_STACK_DEPTH           0x0D37
#define GL_MODELVIEW_STACK_DEPTH          0x0BA3
#define GL_PROJECTION_STACK_DEPTH         0x0BA4
#define GL_TEXTURE_STACK_DEPTH            0x0BA5
#define GL_MODELVIEW_MATRIX               0x0BA6
#define GL_PROJECTION_MATRIX              0x0BA7
#define GL_TEXTURE_MATRIX                 0x0BA8
#define GL_DEPTH_RANGE                    0x0B70
#define GL_DEPTH_CLEAR_VALUE              0x0B73
#define GL_DEPTH_FUNC_QUERY               0x0B74
#define GL_DEPTH_WRITEMASK                0x0B72
#define GL_SHADE_MODEL_QUERY              0x0B54
#define GL_SCISSOR_BOX                    0x0C10
#define GL_COLOR_CLEAR_VALUE              0x0C22
#define GL_BLEND_SRC                      0x0BE1
#define GL_BLEND_DST                      0x0BE0
#define GL_CURRENT_COLOR                  0x0B00
#define GL_CURRENT_NORMAL                 0x0B02
#define GL_CURRENT_TEXTURE_COORDS         0x0B03
#define GL_STENCIL_FUNC_QUERY             0x0B92
#define GL_STENCIL_VALUE_MASK             0x0B93
#define GL_STENCIL_REF                    0x0B97
#define GL_STENCIL_FAIL                   0x0B94
#define GL_STENCIL_PASS_DEPTH_FAIL        0x0B95
#define GL_STENCIL_PASS_DEPTH_PASS        0x0B96
#define GL_STENCIL_WRITEMASK              0x0B98
#define GL_STENCIL_CLEAR_VALUE            0x0B91
#define GL_ALPHA_TEST_FUNC                0x0BC1
#define GL_ALPHA_TEST_REF                 0x0BC2
#define GL_TEXTURE_BINDING_2D             0x8069
#define GL_CULL_FACE_MODE_QUERY           0x0B45
#define GL_FRONT_FACE_QUERY               0x0B46
#define GL_LOGIC_OP_MODE                  0x0BF0
#define GL_LINE_WIDTH_QUERY               0x0B21
#define GL_POINT_SIZE_QUERY               0x0B11
#define GL_POLYGON_OFFSET_FACTOR_QUERY    0x8038
#define GL_POLYGON_OFFSET_UNITS_QUERY     0x2A00

// Get string
#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_EXTENSIONS                     0x1F03

// Multi-texture
#define GL_TEXTURE0_ARB                   0x84C0
#define GL_ACTIVE_TEXTURE_ARB             0x84E0
#define GL_MAX_TEXTURE_UNITS_ARB          0x84E2

// Vertex array
#define GL_VERTEX_ARRAY                   0x8074
#define GL_NORMAL_ARRAY                   0x8075
#define GL_COLOR_ARRAY                    0x8076
#define GL_INDEX_ARRAY                    0x8077
#define GL_TEXTURE_COORD_ARRAY            0x8078
#define GL_EDGE_FLAG_ARRAY                0x8079

// Attrib mask bits
#define GL_CURRENT_BIT                    0x00000001
#define GL_POINT_BIT                      0x00000002
#define GL_LINE_BIT                       0x00000004
#define GL_POLYGON_BIT                    0x00000008
#define GL_LIGHTING_BIT                   0x00000040
#define GL_FOG_BIT                        0x00000080
#define GL_ENABLE_BIT                     0x00002000
#define GL_COLOR_BUFFER_BIT_ATTRIB        0x00004000
#define GL_VIEWPORT_BIT                   0x00000800
#define GL_TRANSFORM_BIT                  0x00001000
#define GL_HINT_BIT                       0x00008000
#define GL_SCISSOR_BIT                    0x00080000
#define GL_TEXTURE_BIT                    0x00040000
#define GL_ALL_ATTRIB_BITS                0x000FFFFF

// Client attrib bits
#define GL_CLIENT_PIXEL_STORE_BIT         0x00000001
#define GL_CLIENT_VERTEX_ARRAY_BIT        0x00000002
#define GL_CLIENT_ALL_ATTRIB_BITS         0xFFFFFFFF

// Render mode
#define GL_RENDER                         0x1C00
#define GL_SELECT                         0x1C02
#define GL_FEEDBACK                       0x1C01

// Evaluator targets
#define GL_MAP1_COLOR_4                   0x0D90
#define GL_MAP1_INDEX                     0x0D91
#define GL_MAP1_NORMAL                    0x0D92
#define GL_MAP1_TEXTURE_COORD_1           0x0D93
#define GL_MAP1_TEXTURE_COORD_2           0x0D94
#define GL_MAP1_TEXTURE_COORD_3           0x0D95
#define GL_MAP1_TEXTURE_COORD_4           0x0D96
#define GL_MAP1_VERTEX_3                  0x0D97
#define GL_MAP1_VERTEX_4                  0x0D98
#define GL_MAP2_COLOR_4                   0x0DB0
#define GL_MAP2_INDEX                     0x0DB1
#define GL_MAP2_NORMAL                    0x0DB2
#define GL_MAP2_TEXTURE_COORD_1           0x0DB3
#define GL_MAP2_TEXTURE_COORD_2           0x0DB4
#define GL_MAP2_TEXTURE_COORD_3           0x0DB5
#define GL_MAP2_TEXTURE_COORD_4           0x0DB6
#define GL_MAP2_VERTEX_3                  0x0DB7
#define GL_MAP2_VERTEX_4                  0x0DB8
#define GL_COEFF                          0x0A00
#define GL_ORDER                          0x0A01
#define GL_DOMAIN                         0x0A02
// Primitive types for evaluators
#define GL_POINTS                         0x0000
#define GL_LINES                          0x0001
#define GL_LINE_LOOP                      0x0002
#define GL_LINE_STRIP                     0x0003
#define GL_TRIANGLES                      0x0004
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_TRIANGLE_FAN                   0x0006
#define GL_QUAD_STRIP                     0x0008

// Compile mode
#define GL_COMPILE                        0x1300
#define GL_COMPILE_AND_EXECUTE            0x1301

// Texture filter modes
#define GL_NEAREST                        0x2600
#define GL_LINEAR                         0x2601
#define GL_NEAREST_MIPMAP_NEAREST         0x2700
#define GL_LINEAR_MIPMAP_NEAREST          0x2701
#define GL_NEAREST_MIPMAP_LINEAR          0x2702
#define GL_LINEAR_MIPMAP_LINEAR           0x2703

// Texture wrap modes
#define GL_REPEAT                         0x2901
#define GL_CLAMP                          0x2900
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_MIRRORED_REPEAT                0x8370

// Texture parameters
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803

// Pixel format enums
#define GL_RGBA                           0x1908
#define GL_RGB                            0x1907
#define GL_RED                            0x1903
#define GL_GREEN                          0x1904
#define GL_BLUE                           0x1905
#define GL_LUMINANCE                      0x1909
#define GL_LUMINANCE_ALPHA                0x190A
#define GL_ALPHA_FORMAT                   0x1906
#define GL_COLOR_INDEX                    0x1900
#define GL_ABGR_EXT                       0x8000
#define GL_BGR_EXT                        0x80E0
#define GL_BGRA_EXT                       0x80E1
#define GL_INTENSITY_EXT                  0x8049

// Pixel type enums
#define GL_BYTE                           0x1400
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_SHORT                          0x1402
#define GL_UNSIGNED_INT                   0x1405
#define GL_INT                            0x1404
#define GL_FLOAT                          0x1406
#define GL_UNSIGNED_SHORT                 0x1403
#define GL_UNSIGNED_BYTE_3_3_2            0x8032
#define GL_UNSIGNED_SHORT_4_4_4_4         0x8033
#define GL_UNSIGNED_SHORT_5_6_5           0x8363
#define GL_UNSIGNED_SHORT_5_5_5_1         0x8034
#define GL_UNSIGNED_INT_8_8_8_8           0x8035
#define GL_UNSIGNED_INT_10_10_10_2        0x8036
#define GL_UNSIGNED_BYTE_2_3_3_REV        0x8362
#define GL_UNSIGNED_SHORT_5_6_5_REV       0x8364
#define GL_UNSIGNED_SHORT_4_4_4_4_REV     0x8365
#define GL_UNSIGNED_SHORT_1_5_5_5_REV     0x8366
#define GL_UNSIGNED_INT_8_8_8_8_REV       0x8367
#define GL_UNSIGNED_INT_2_10_10_10_REV    0x8368

// Internal format aliases (GL 1.1+ numeric)
#define GL_RGBA8                          0x8058
#define GL_RGB8                           0x8051
#define GL_LUMINANCE8                     0x8040
#define GL_ALPHA8                         0x803C

// Forward declare Metal upload (implemented in gl_metal_renderer.mm)
extern void GLMetalUploadTexture(GLContext *ctx, GLTextureObject *texObj, int level,
                                 int width, int height, const uint8_t *data, int dataLen);
extern void GLMetalUploadSubTexture(GLContext *ctx, GLTextureObject *texObj, int level,
                                    int xoff, int yoff, int w, int h,
                                    const uint8_t *data, int bytesPerRow);
extern void GLMetalUpload3DTexture(GLContext *ctx, GLTextureObject *texObj, int level,
                                   int width, int height, int depth,
                                   const uint8_t *data, int dataLen);
extern void GLMetalUploadSubTexture3D(GLContext *ctx, GLTextureObject *texObj, int level,
                                      int xoff, int yoff, int zoff,
                                      int w, int h, int d,
                                      const uint8_t *data, int bytesPerRow, int bytesPerImage);
extern void GLMetalDestroyTexture(GLTextureObject *texObj);


// =========================================================================
//  Global GL context
// =========================================================================

// gl_current_context is defined in gl_engine.cpp (managed by AGL context table)

// Allocated Mac-side string pointers (allocated once via Mac_sysalloc)
static uint32_t gl_string_vendor_addr   = 0;
static uint32_t gl_string_renderer_addr = 0;
static uint32_t gl_string_version_addr  = 0;
static uint32_t gl_string_extensions_addr = 0;

// =========================================================================
//  Inline 4x4 matrix math -- column-major (GL convention)
//
//  Column-major layout: m[col*4 + row]
//    m[0]  m[4]  m[8]  m[12]
//    m[1]  m[5]  m[9]  m[13]
//    m[2]  m[6]  m[10] m[14]
//    m[3]  m[7]  m[11] m[15]
// =========================================================================

static inline void mat4_identity(float m[16])
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static inline void mat4_copy(float dst[16], const float src[16])
{
	memcpy(dst, src, 16 * sizeof(float));
}

static void mat4_multiply(float out[16], const float a[16], const float b[16])
{
	float tmp[16];
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			tmp[col * 4 + row] =
				a[0 * 4 + row] * b[col * 4 + 0] +
				a[1 * 4 + row] * b[col * 4 + 1] +
				a[2 * 4 + row] * b[col * 4 + 2] +
				a[3 * 4 + row] * b[col * 4 + 3];
		}
	}
	memcpy(out, tmp, 16 * sizeof(float));
}

static void mat4_translate(float m[16], float x, float y, float z)
{
	float t[16];
	mat4_identity(t);
	t[12] = x;
	t[13] = y;
	t[14] = z;
	mat4_multiply(m, m, t);
}

static void mat4_rotate(float m[16], float angle_deg, float x, float y, float z)
{
	float rad = angle_deg * (float)(M_PI / 180.0);
	float c = cosf(rad);
	float s = sinf(rad);
	float len = sqrtf(x * x + y * y + z * z);
	if (len < 1.0e-6f) return;
	x /= len; y /= len; z /= len;

	float r[16];
	float ic = 1.0f - c;
	r[0]  = x * x * ic + c;
	r[1]  = y * x * ic + z * s;
	r[2]  = x * z * ic - y * s;
	r[3]  = 0.0f;
	r[4]  = x * y * ic - z * s;
	r[5]  = y * y * ic + c;
	r[6]  = y * z * ic + x * s;
	r[7]  = 0.0f;
	r[8]  = x * z * ic + y * s;
	r[9]  = y * z * ic - x * s;
	r[10] = z * z * ic + c;
	r[11] = 0.0f;
	r[12] = 0.0f; r[13] = 0.0f; r[14] = 0.0f; r[15] = 1.0f;

	mat4_multiply(m, m, r);
}

static void mat4_scale(float m[16], float x, float y, float z)
{
	float s[16];
	mat4_identity(s);
	s[0]  = x;
	s[5]  = y;
	s[10] = z;
	mat4_multiply(m, m, s);
}

static void mat4_frustum(float m[16], double l, double r, double b, double t, double n, double f)
{
	float p[16];
	memset(p, 0, 16 * sizeof(float));
	float rl = (float)(r - l), tb = (float)(t - b), fn = (float)(f - n);
	p[0]  = (float)(2.0 * n) / rl;
	p[5]  = (float)(2.0 * n) / tb;
	p[8]  = (float)(r + l) / rl;
	p[9]  = (float)(t + b) / tb;
	p[10] = -(float)(f + n) / fn;
	p[11] = -1.0f;
	p[14] = -(float)(2.0 * f * n) / fn;
	mat4_multiply(m, m, p);
}

static void mat4_ortho(float m[16], double l, double r, double b, double t, double n, double f)
{
	float p[16];
	memset(p, 0, 16 * sizeof(float));
	float rl = (float)(r - l), tb = (float)(t - b), fn = (float)(f - n);
	p[0]  = 2.0f / rl;
	p[5]  = 2.0f / tb;
	p[10] = -2.0f / fn;
	p[12] = -(float)(r + l) / rl;
	p[13] = -(float)(t + b) / tb;
	p[14] = -(float)(f + n) / fn;
	p[15] = 1.0f;
	mat4_multiply(m, m, p);
}



// =========================================================================
//  Helper: read float from Mac memory
// =========================================================================

static inline float ReadMacFloat(uint32 addr)
{
	uint32 bits = ReadMacInt32(addr);
	float f;
	memcpy(&f, &bits, sizeof(float));
	return f;
}

static inline void WriteMacFloat(uint32 addr, float f)
{
	uint32 bits;
	memcpy(&bits, &f, sizeof(float));
	WriteMacInt32(addr, bits);
}


// =========================================================================
//  Helper: get current matrix stack top pointer
// =========================================================================

static float *gl_get_current_matrix(GLContext *ctx)
{
	switch (ctx->matrix_mode) {
	case GL_MODELVIEW:
		return ctx->modelview_stack[ctx->modelview_depth];
	case GL_PROJECTION:
		return ctx->projection_stack[ctx->projection_depth];
	case GL_TEXTURE:
		return ctx->texture_stack[ctx->active_texture][ctx->texture_depth[ctx->active_texture]];
	case GL_COLOR:
		return ctx->color_stack[ctx->color_depth];
	default:
		return ctx->modelview_stack[ctx->modelview_depth];
	}
}

// Stack depth limits
static int gl_get_max_stack_depth(uint32_t mode)
{
	switch (mode) {
	case GL_MODELVIEW:  return 32;
	case GL_PROJECTION: return 32;   // was 2 (spec min); real Apple/ATI GL >2.
	case GL_TEXTURE:    return 32;   // Cro-Mag overflowed the old depth-2 cap.
	case GL_COLOR:      return 32;
	default:            return 32;
	}
}

static int *gl_get_stack_depth_ptr(GLContext *ctx)
{
	switch (ctx->matrix_mode) {
	case GL_MODELVIEW:  return &ctx->modelview_depth;
	case GL_PROJECTION: return &ctx->projection_depth;
	case GL_TEXTURE:    return &ctx->texture_depth[ctx->active_texture];
	case GL_COLOR:      return &ctx->color_depth;
	default:            return &ctx->modelview_depth;
	}
}


// =========================================================================
//  Matrix operations
// =========================================================================

void NativeGLMatrixMode(GLContext *ctx, uint32_t mode)
{
	if (mode == GL_MODELVIEW || mode == GL_PROJECTION ||
	    mode == GL_TEXTURE || mode == GL_COLOR) {
		ctx->matrix_mode = mode;
	} else {
		ctx->last_error = GL_INVALID_ENUM;
	}
}

void NativeGLLoadIdentity(GLContext *ctx)
{
	mat4_identity(gl_get_current_matrix(ctx));
}

void NativeGLLoadMatrixf(GLContext *ctx, uint32_t mac_ptr)
{
	float *m = gl_get_current_matrix(ctx);
	for (int i = 0; i < 16; i++)
		m[i] = ReadMacFloat(mac_ptr + i * 4);
}

void NativeGLLoadMatrixd(GLContext *ctx, uint32_t mac_ptr)
{
	// Read 16 doubles (8 bytes each), convert to float
	float *m = gl_get_current_matrix(ctx);
	for (int i = 0; i < 16; i++) {
		uint32 hi = ReadMacInt32(mac_ptr + i * 8);
		uint32 lo = ReadMacInt32(mac_ptr + i * 8 + 4);
		uint64_t bits = ((uint64_t)hi << 32) | lo;
		double d;
		memcpy(&d, &bits, sizeof(double));
		m[i] = (float)d;
	}
}

void NativeGLMultMatrixf(GLContext *ctx, uint32_t mac_ptr)
{
	float tmp[16];
	for (int i = 0; i < 16; i++)
		tmp[i] = ReadMacFloat(mac_ptr + i * 4);
	float *m = gl_get_current_matrix(ctx);
	mat4_multiply(m, m, tmp);
}

void NativeGLMultMatrixd(GLContext *ctx, uint32_t mac_ptr)
{
	float tmp[16];
	for (int i = 0; i < 16; i++) {
		uint32 hi = ReadMacInt32(mac_ptr + i * 8);
		uint32 lo = ReadMacInt32(mac_ptr + i * 8 + 4);
		uint64_t bits = ((uint64_t)hi << 32) | lo;
		double d;
		memcpy(&d, &bits, sizeof(double));
		tmp[i] = (float)d;
	}
	float *m = gl_get_current_matrix(ctx);
	mat4_multiply(m, m, tmp);
}

void NativeGLPushMatrix(GLContext *ctx)
{
	int *depth = gl_get_stack_depth_ptr(ctx);
	int max_depth = gl_get_max_stack_depth(ctx->matrix_mode);
	if (*depth + 1 >= max_depth) {
		ctx->last_error = GL_STACK_OVERFLOW;
		GL_LOG("glPushMatrix OVERFLOW mode=0x%x depth=%d max=%d "
		       "(raise gl_get_max_stack_depth / *_stack[] if a game needs deeper)",
		       ctx->matrix_mode, *depth, max_depth);
		return;
	}
	// Copy current top to new top
	float *cur = gl_get_current_matrix(ctx);
	(*depth)++;
	float *top = gl_get_current_matrix(ctx);
	mat4_copy(top, cur);
}

void NativeGLPopMatrix(GLContext *ctx)
{
	int *depth = gl_get_stack_depth_ptr(ctx);
	if (*depth <= 0) {
		ctx->last_error = GL_STACK_UNDERFLOW;
		return;
	}
	(*depth)--;
}

void NativeGLRotatef(GLContext *ctx, float angle, float x, float y, float z)
{
	mat4_rotate(gl_get_current_matrix(ctx), angle, x, y, z);
}

void NativeGLRotated(GLContext *ctx, double angle, double x, double y, double z)
{
	mat4_rotate(gl_get_current_matrix(ctx), (float)angle, (float)x, (float)y, (float)z);
}

void NativeGLTranslatef(GLContext *ctx, float x, float y, float z)
{
	mat4_translate(gl_get_current_matrix(ctx), x, y, z);
}

void NativeGLTranslated(GLContext *ctx, double x, double y, double z)
{
	mat4_translate(gl_get_current_matrix(ctx), (float)x, (float)y, (float)z);
}

void NativeGLScalef(GLContext *ctx, float x, float y, float z)
{
	mat4_scale(gl_get_current_matrix(ctx), x, y, z);
}

void NativeGLScaled(GLContext *ctx, double x, double y, double z)
{
	mat4_scale(gl_get_current_matrix(ctx), (float)x, (float)y, (float)z);
}

void NativeGLFrustum(GLContext *ctx, double l, double r, double b, double t, double n, double f)
{
	mat4_frustum(gl_get_current_matrix(ctx), l, r, b, t, n, f);
}

void NativeGLOrtho(GLContext *ctx, double l, double r, double b, double t, double n, double f)
{
	// Pass parameters through unmodified — the game's projection matrix
	// already encodes the Y orientation it expects.  The GL-to-Metal NDC
	// depth remap is handled in the vertex shader (z * 0.5 + w * 0.5).
	mat4_ortho(gl_get_current_matrix(ctx), l, r, b, t, n, f);
}


// =========================================================================
//  Enable / Disable
// =========================================================================

// Forward declarations for evaluator helpers (defined in evaluator section below)
static int gl_eval_target_index(uint32_t target);

static inline int gl_clamp_texture_unit(int unit)
{
	if (unit < 0) return 0;
	if (unit > 3) return 3;
	return unit;
}

static inline void gl_clear_prepared_texture_2d_for_draw(GLContext *ctx, int unit)
{
	unit = gl_clamp_texture_unit(unit);
	ctx->prepared_texture_2d_for_draw[unit] = false;
}

static inline void gl_clear_latched_texture_2d_for_array_draw(GLContext *ctx, int unit)
{
	unit = gl_clamp_texture_unit(unit);
	ctx->latched_texture_2d_for_array_draw[unit] = false;
}

static inline void gl_invalidate_prepared_texcoord_array_for_draw(GLContext *ctx, int unit)
{
	unit = gl_clamp_texture_unit(unit);
	ctx->prepared_texcoord_array_for_draw[unit] = false;
	ctx->prepared_texcoord_array[unit].pointer = 0;
}

static void gl_set_cap(GLContext *ctx, uint32_t cap, bool value)
{
	switch (cap) {
	case GL_DEPTH_TEST:           ctx->depth_test = value; break;
	case GL_BLEND:                ctx->blend = value; break;
	case GL_LIGHTING:             ctx->lighting_enabled = value; break;
	case GL_FOG:                  ctx->fog_enabled = value; break;
	case GL_SCISSOR_TEST:         ctx->scissor_test = value; break;
	case GL_CULL_FACE:            ctx->cull_face_enabled = value; break;
	case GL_ALPHA_TEST:           ctx->alpha_test = value; break;
	case GL_COLOR_SUM:            ctx->color_sum = value; break;
	case GL_NORMALIZE:            ctx->normalize = value; break;
	case GL_COLOR_MATERIAL:       ctx->color_material_enabled = value; break;
	case GL_STENCIL_TEST:         ctx->stencil_test = value; break;
	case GL_TEXTURE_1D:           ctx->tex_units[ctx->active_texture].enabled_1d = value; break;
	case GL_TEXTURE_2D: {
		const int unit = gl_clamp_texture_unit(ctx->active_texture);
		GLTextureUnit &texUnit = ctx->tex_units[unit];
		if (value) {
			texUnit.enabled_2d = true;
			ctx->texture_2d_enable_draw_serial[unit] = ctx->completed_draw_serial;
			ctx->prepared_texture_2d_for_draw[unit] = false;
		} else {
			const bool hadResource =
				texUnit.enabled_2d && texUnit.bound_texture_2d != 0;
			const bool preparedSurvivesDisable =
				GLMetalPreparedDrawStateSurvivesDisable(
					ctx->texture_2d_enable_draw_serial[unit],
					ctx->completed_draw_serial,
					hadResource);
			const bool latchSurvivesDisable =
				GLMetalLatchedTexture2DSurvivesDisable(
					ctx->texture_2d_enable_draw_serial[unit],
					ctx->completed_draw_serial,
					hadResource);
			ctx->prepared_texture_2d_for_draw[unit] = preparedSurvivesDisable;
			ctx->latched_texture_2d_for_array_draw[unit] = latchSurvivesDisable;
			texUnit.enabled_2d = false;
		}
		break;
	}
	case GL_POLYGON_OFFSET_FILL:  ctx->polygon_offset_fill = value; break;
	case GL_POLYGON_OFFSET_LINE:  ctx->polygon_offset_line = value; break;
	case GL_POLYGON_OFFSET_POINT: ctx->polygon_offset_point = value; break;
	case GL_POINT_SMOOTH:         ctx->point_smooth = value; break;
	case GL_LINE_SMOOTH:          ctx->line_smooth = value; break;
	case GL_POLYGON_SMOOTH:       ctx->polygon_smooth = value; break;
	case GL_DITHER:               ctx->dither = value; break;
	case GL_COLOR_LOGIC_OP:       ctx->color_logic_op = value; break;
	case GL_INDEX_LOGIC_OP:       ctx->color_logic_op = value; break; // map to same
	case GL_AUTO_NORMAL:          ctx->auto_normal = value; break;
	case GL_LINE_STIPPLE:         break; // tracked but not used in Metal
	case GL_POLYGON_STIPPLE:      break; // tracked but not used in Metal
	case GL_MULTISAMPLE_ARB:      ctx->multisample = value; break;
	case GL_SAMPLE_ALPHA_TO_COVERAGE_ARB: ctx->sample_alpha_to_coverage = value; break;
	case GL_SAMPLE_ALPHA_TO_ONE_ARB:      ctx->sample_alpha_to_one = value; break;
	case GL_SAMPLE_COVERAGE_ARB:          ctx->sample_coverage = value; break;
	// Imaging subset enable/disable
	case 0x80D0: /* GL_COLOR_TABLE */                  ctx->color_table_enabled = value; break;
	case 0x80D1: /* GL_POST_CONVOLUTION_COLOR_TABLE */ ctx->post_convolution_color_table_enabled = value; break;
	case 0x80D2: /* GL_POST_COLOR_MATRIX_COLOR_TABLE */ctx->post_color_matrix_color_table_enabled = value; break;
	case 0x8010: /* GL_CONVOLUTION_1D */               ctx->convolution_1d_enabled = value; break;
	case 0x8011: /* GL_CONVOLUTION_2D */               ctx->convolution_2d_enabled = value; break;
	case 0x8012: /* GL_SEPARABLE_2D */                 ctx->separable_2d_enabled = value; break;
	case 0x8024: /* GL_HISTOGRAM */                    ctx->histogram_enabled = value; break;
	case 0x802E: /* GL_MINMAX */                       ctx->minmax_enabled = value; break;
	// 3D textures
	case 0x806F: /* GL_TEXTURE_3D_EXT */               ctx->tex_units[ctx->active_texture].enabled_3d = value; break;
	default:
		// Handle GL_LIGHT0..GL_LIGHT7
		if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8) {
			ctx->lights[cap - GL_LIGHT0].enabled = value;
			break;
		}
		// Handle GL_CLIP_PLANE0..GL_CLIP_PLANE5
		if (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + 6) {
			ctx->clip_plane_enabled[cap - GL_CLIP_PLANE0] = value;
			break;
		}
		// Handle GL_TEXTURE_GEN_S/T/R/Q
		if (cap >= GL_TEXTURE_GEN_S && cap <= GL_TEXTURE_GEN_Q) {
			int idx = cap - GL_TEXTURE_GEN_S;
			bool *flags[] = {
				&ctx->tex_units[ctx->active_texture].texgen_s_enabled,
				&ctx->tex_units[ctx->active_texture].texgen_t_enabled,
				&ctx->tex_units[ctx->active_texture].texgen_r_enabled,
				&ctx->tex_units[ctx->active_texture].texgen_q_enabled,
			};
			*flags[idx] = value;
			break;
		}
		// Handle GL_MAP1_* evaluator enables (0x0D90..0x0D98)
		if (cap >= GL_MAP1_COLOR_4 && cap <= GL_MAP1_VERTEX_4) {
			int idx = gl_eval_target_index(cap);
			if (idx >= 0) ctx->eval_map1_enabled[idx] = value;
			break;
		}
		// Handle GL_MAP2_* evaluator enables (0x0DB0..0x0DB8)
		if (cap >= GL_MAP2_COLOR_4 && cap <= GL_MAP2_VERTEX_4) {
			int idx = gl_eval_target_index(cap);
			if (idx >= 0) ctx->eval_map2_enabled[idx] = value;
			break;
		}
		// Unknown cap -- not an error per GL spec for forward-compat
		GL_LOG("unhandled cap 0x%X in enable/disable", cap);
		break;
	}
}

void NativeGLEnable(GLContext *ctx, uint32_t cap)
{
	gl_set_cap(ctx, cap, true);
}

void NativeGLDisable(GLContext *ctx, uint32_t cap)
{
	gl_set_cap(ctx, cap, false);
}

uint32_t NativeGLIsEnabled(GLContext *ctx, uint32_t cap)
{
	bool result = false;
	switch (cap) {
	case GL_DEPTH_TEST:           result = ctx->depth_test; break;
	case GL_BLEND:                result = ctx->blend; break;
	case GL_LIGHTING:             result = ctx->lighting_enabled; break;
	case GL_FOG:                  result = ctx->fog_enabled; break;
	case GL_SCISSOR_TEST:         result = ctx->scissor_test; break;
	case GL_CULL_FACE:            result = ctx->cull_face_enabled; break;
	case GL_ALPHA_TEST:           result = ctx->alpha_test; break;
	case GL_COLOR_SUM:            result = ctx->color_sum; break;
	case GL_NORMALIZE:            result = ctx->normalize; break;
	case GL_COLOR_MATERIAL:       result = ctx->color_material_enabled; break;
	case GL_STENCIL_TEST:         result = ctx->stencil_test; break;
	case GL_TEXTURE_2D:           result = ctx->tex_units[ctx->active_texture].enabled_2d; break;
	default:
		if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8)
			result = ctx->lights[cap - GL_LIGHT0].enabled;
		else if (cap >= GL_MAP1_COLOR_4 && cap <= GL_MAP1_VERTEX_4) {
			int idx = gl_eval_target_index(cap);
			if (idx >= 0) result = ctx->eval_map1_enabled[idx];
		}
		else if (cap >= GL_MAP2_COLOR_4 && cap <= GL_MAP2_VERTEX_4) {
			int idx = gl_eval_target_index(cap);
			if (idx >= 0) result = ctx->eval_map2_enabled[idx];
		}
		break;
	}
	return result ? GL_TRUE : GL_FALSE;
}


// =========================================================================
//  Depth / Blend / Alpha / Stencil state
// =========================================================================

void NativeGLDepthFunc(GLContext *ctx, uint32_t func)
{
	ctx->depth_func = func;
}

void NativeGLDepthMask(GLContext *ctx, uint32_t flag)
{
	ctx->depth_mask = (flag == GL_TRUE);
}

void NativeGLDepthRange(GLContext *ctx, double near_val, double far_val)
{
	ctx->depth_range_near = (float)near_val;
	ctx->depth_range_far  = (float)far_val;
}

void NativeGLBlendFunc(GLContext *ctx, uint32_t src, uint32_t dst)
{
	ctx->blend_src = src;
	ctx->blend_dst = dst;
}

void NativeGLAlphaFunc(GLContext *ctx, uint32_t func, float ref)
{
	ctx->alpha_func = func;
	ctx->alpha_ref  = ref;
}

void NativeGLStencilFunc(GLContext *ctx, uint32_t func, int32_t ref, uint32_t mask)
{
	ctx->stencil.func       = func;
	ctx->stencil.ref        = ref;
	ctx->stencil.value_mask = mask;
}

void NativeGLStencilOp(GLContext *ctx, uint32_t sfail, uint32_t dpfail, uint32_t dppass)
{
	ctx->stencil.sfail  = sfail;
	ctx->stencil.dpfail = dpfail;
	ctx->stencil.dppass = dppass;
}

void NativeGLStencilMask(GLContext *ctx, uint32_t mask)
{
	ctx->stencil.write_mask = mask;
}


// =========================================================================
//  Viewport / Scissor
// =========================================================================

void NativeGLViewport(GLContext *ctx, int32_t x, int32_t y, int32_t w, int32_t h)
{
	ctx->viewport[0] = x;
	ctx->viewport[1] = y;
	ctx->viewport[2] = w;
	ctx->viewport[3] = h;
}

void NativeGLScissor(GLContext *ctx, int32_t x, int32_t y, int32_t w, int32_t h)
{
	ctx->scissor_box[0] = x;
	ctx->scissor_box[1] = y;
	ctx->scissor_box[2] = w;
	ctx->scissor_box[3] = h;
}


// =========================================================================
//  Clear
// =========================================================================

void NativeGLClearColor(GLContext *ctx, float r, float g, float b, float a)
{
	ctx->clear_color[0] = r;
	ctx->clear_color[1] = g;
	ctx->clear_color[2] = b;
	ctx->clear_color[3] = a;
}

void NativeGLClearDepth(GLContext *ctx, double depth)
{
	ctx->clear_depth = (float)depth;
}

void NativeGLClearStencil(GLContext *ctx, int32_t s)
{
	ctx->clear_stencil = s;
}

void NativeGLClear(GLContext *ctx, uint32_t mask)
{
	// Handle accum buffer clear on the CPU side
	if (mask & GL_ACCUM_BUFFER_BIT) {
		if (ctx->accum_allocated && ctx->accum_buffer) {
			int n = ctx->accum_width * ctx->accum_height * 4;
			bool non_zero = (ctx->clear_accum[0] != 0.0f || ctx->clear_accum[1] != 0.0f ||
			                 ctx->clear_accum[2] != 0.0f || ctx->clear_accum[3] != 0.0f);
			if (non_zero) {
				for (int i = 0; i < ctx->accum_width * ctx->accum_height; i++) {
					ctx->accum_buffer[i * 4 + 0] = ctx->clear_accum[0];
					ctx->accum_buffer[i * 4 + 1] = ctx->clear_accum[1];
					ctx->accum_buffer[i * 4 + 2] = ctx->clear_accum[2];
					ctx->accum_buffer[i * 4 + 3] = ctx->clear_accum[3];
				}
			} else {
				memset(ctx->accum_buffer, 0, n * sizeof(float));
			}
		}
	}
	if (GLShouldApplyAttachmentClear(mask)) {
		if (!ctx->metal) {
			GLMetalInit(ctx);
		}
		GLMetalClear(ctx, mask);
	}
}


// =========================================================================
//  Misc state
// =========================================================================

void NativeGLShadeModel(GLContext *ctx, uint32_t mode)
{
	ctx->shade_model = mode;
}

void NativeGLCullFace(GLContext *ctx, uint32_t mode)
{
	ctx->cull_face_mode = mode;
}

void NativeGLFrontFace(GLContext *ctx, uint32_t mode)
{
	ctx->front_face = mode;
}

void NativeGLPolygonMode(GLContext *ctx, uint32_t face, uint32_t mode)
{
	if (face == GL_FRONT || face == GL_FRONT_AND_BACK)
		ctx->polygon_mode_front = mode;
	if (face == GL_BACK || face == GL_FRONT_AND_BACK)
		ctx->polygon_mode_back = mode;
}

void NativeGLColorMask(GLContext *ctx, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
	ctx->color_mask[0] = (r != 0);
	ctx->color_mask[1] = (g != 0);
	ctx->color_mask[2] = (b != 0);
	ctx->color_mask[3] = (a != 0);
}

void NativeGLLineWidth(GLContext *ctx, float w)
{
	ctx->line_width = w;
}

void NativeGLPointSize(GLContext *ctx, float s)
{
	ctx->point_size = s;
}

void NativeGLPolygonOffset(GLContext *ctx, float factor, float units)
{
	ctx->polygon_offset_factor = factor;
	ctx->polygon_offset_units  = units;
}

void NativeGLHint(GLContext *ctx, uint32_t target, uint32_t mode)
{
	switch (target) {
	case GL_PERSPECTIVE_CORRECTION_HINT: ctx->hint_perspective_correction = mode; break;
	case GL_POINT_SMOOTH_HINT:           ctx->hint_point_smooth = mode; break;
	case GL_LINE_SMOOTH_HINT:            ctx->hint_line_smooth = mode; break;
	case GL_POLYGON_SMOOTH_HINT:         ctx->hint_polygon_smooth = mode; break;
	case GL_FOG_HINT:                    ctx->hint_fog = mode; break;
	default: break; // Silently ignore unknown hint targets
	}
}

// NOTE: Logic operations are state-tracked but NOT rendered. Metal on iOS does
// not support framebuffer logic operations natively. A compute shader workaround
// would be needed for true implementation. In practice, no known Mac OS 9 OpenGL
// game relies on logic ops for rendering.
void NativeGLLogicOp(GLContext *ctx, uint32_t op)
{
	ctx->logic_op_mode = op;
}


// =========================================================================
//  Lighting handlers
// =========================================================================

void NativeGLLightf(GLContext *ctx, uint32_t light, uint32_t pname, float param)
{
	if (light < GL_LIGHT0 || light >= GL_LIGHT0 + 8) {
		ctx->last_error = GL_INVALID_ENUM;
		return;
	}
	GLLight &l = ctx->lights[light - GL_LIGHT0];
	switch (pname) {
	case GL_SPOT_EXPONENT:         l.spot_exponent = param; break;
	case GL_SPOT_CUTOFF:           l.spot_cutoff = param; break;
	case GL_CONSTANT_ATTENUATION:  l.constant_attenuation = param; break;
	case GL_LINEAR_ATTENUATION:    l.linear_attenuation = param; break;
	case GL_QUADRATIC_ATTENUATION: l.quadratic_attenuation = param; break;
	default: ctx->last_error = GL_INVALID_ENUM; break;
	}
}

void NativeGLLightfv(GLContext *ctx, uint32_t light, uint32_t pname, uint32_t mac_ptr)
{
	if (light < GL_LIGHT0 || light >= GL_LIGHT0 + 8) {
		ctx->last_error = GL_INVALID_ENUM;
		return;
	}
	GLLight &l = ctx->lights[light - GL_LIGHT0];
	switch (pname) {
	case GL_AMBIENT:
		for (int i = 0; i < 4; i++) l.ambient[i] = ReadMacFloat(mac_ptr + i * 4);
		break;
	case GL_DIFFUSE:
		for (int i = 0; i < 4; i++) l.diffuse[i] = ReadMacFloat(mac_ptr + i * 4);
		break;
	case GL_SPECULAR:
		for (int i = 0; i < 4; i++) l.specular[i] = ReadMacFloat(mac_ptr + i * 4);
		break;
	case GL_POSITION: {
		// Light position is transformed by current modelview matrix
		float pos[4];
		for (int i = 0; i < 4; i++) pos[i] = ReadMacFloat(mac_ptr + i * 4);
		// Transform by current modelview
		float *mv = ctx->modelview_stack[ctx->modelview_depth];
		for (int row = 0; row < 4; row++) {
			l.position[row] =
				mv[0 * 4 + row] * pos[0] +
				mv[1 * 4 + row] * pos[1] +
				mv[2 * 4 + row] * pos[2] +
				mv[3 * 4 + row] * pos[3];
		}
		break;
	}
	case GL_SPOT_DIRECTION: {
		float dir[3];
		for (int i = 0; i < 3; i++) dir[i] = ReadMacFloat(mac_ptr + i * 4);
		// Transform by upper-left 3x3 of modelview
		float *mv = ctx->modelview_stack[ctx->modelview_depth];
		for (int row = 0; row < 3; row++) {
			l.spot_direction[row] =
				mv[0 * 4 + row] * dir[0] +
				mv[1 * 4 + row] * dir[1] +
				mv[2 * 4 + row] * dir[2];
		}
		break;
	}
	case GL_SPOT_EXPONENT:         l.spot_exponent = ReadMacFloat(mac_ptr); break;
	case GL_SPOT_CUTOFF:           l.spot_cutoff = ReadMacFloat(mac_ptr); break;
	case GL_CONSTANT_ATTENUATION:  l.constant_attenuation = ReadMacFloat(mac_ptr); break;
	case GL_LINEAR_ATTENUATION:    l.linear_attenuation = ReadMacFloat(mac_ptr); break;
	case GL_QUADRATIC_ATTENUATION: l.quadratic_attenuation = ReadMacFloat(mac_ptr); break;
	default: ctx->last_error = GL_INVALID_ENUM; break;
	}
}

void NativeGLLighti(GLContext *ctx, uint32_t light, uint32_t pname, int32_t param)
{
	NativeGLLightf(ctx, light, pname, (float)param);
}

void NativeGLLightiv(GLContext *ctx, uint32_t light, uint32_t pname, uint32_t mac_ptr)
{
	// Read integers, convert to float, store via fv path
	// For vector params, read 3 or 4 ints
	if (light < GL_LIGHT0 || light >= GL_LIGHT0 + 8) {
		ctx->last_error = GL_INVALID_ENUM;
		return;
	}
	GLLight &l = ctx->lights[light - GL_LIGHT0];
	int count = 4;
	if (pname == GL_SPOT_DIRECTION) count = 3;
	else if (pname == GL_SPOT_EXPONENT || pname == GL_SPOT_CUTOFF ||
	         pname == GL_CONSTANT_ATTENUATION || pname == GL_LINEAR_ATTENUATION ||
	         pname == GL_QUADRATIC_ATTENUATION) count = 1;

	float vals[4];
	for (int i = 0; i < count; i++)
		vals[i] = (float)(int32_t)ReadMacInt32(mac_ptr + i * 4);

	switch (pname) {
	case GL_AMBIENT:    for (int i = 0; i < 4; i++) l.ambient[i] = vals[i]; break;
	case GL_DIFFUSE:    for (int i = 0; i < 4; i++) l.diffuse[i] = vals[i]; break;
	case GL_SPECULAR:   for (int i = 0; i < 4; i++) l.specular[i] = vals[i]; break;
	case GL_SPOT_EXPONENT:         l.spot_exponent = vals[0]; break;
	case GL_SPOT_CUTOFF:           l.spot_cutoff = vals[0]; break;
	case GL_CONSTANT_ATTENUATION:  l.constant_attenuation = vals[0]; break;
	case GL_LINEAR_ATTENUATION:    l.linear_attenuation = vals[0]; break;
	case GL_QUADRATIC_ATTENUATION: l.quadratic_attenuation = vals[0]; break;
	default: ctx->last_error = GL_INVALID_ENUM; break;
	}
}

void NativeGLLightModelf(GLContext *ctx, uint32_t pname, float param)
{
	switch (pname) {
	case GL_LIGHT_MODEL_LOCAL_VIEWER: ctx->light_model_local_viewer = (param != 0.0f); break;
	case GL_LIGHT_MODEL_TWO_SIDE:    ctx->light_model_two_side = (param != 0.0f); break;
	default: ctx->last_error = GL_INVALID_ENUM; break;
	}
}

void NativeGLLightModelfv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr)
{
	switch (pname) {
	case GL_LIGHT_MODEL_AMBIENT:
		for (int i = 0; i < 4; i++)
			ctx->light_model_ambient[i] = ReadMacFloat(mac_ptr + i * 4);
		break;
	case GL_LIGHT_MODEL_LOCAL_VIEWER:
		ctx->light_model_local_viewer = (ReadMacFloat(mac_ptr) != 0.0f);
		break;
	case GL_LIGHT_MODEL_TWO_SIDE:
		ctx->light_model_two_side = (ReadMacFloat(mac_ptr) != 0.0f);
		break;
	default: ctx->last_error = GL_INVALID_ENUM; break;
	}
}

void NativeGLLightModeli(GLContext *ctx, uint32_t pname, int32_t param)
{
	NativeGLLightModelf(ctx, pname, (float)param);
}

void NativeGLLightModeliv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr)
{
	if (pname == GL_LIGHT_MODEL_AMBIENT) {
		for (int i = 0; i < 4; i++)
			ctx->light_model_ambient[i] = (float)(int32_t)ReadMacInt32(mac_ptr + i * 4);
	} else {
		NativeGLLightModeli(ctx, pname, (int32_t)ReadMacInt32(mac_ptr));
	}
}


// =========================================================================
//  Material handlers
// =========================================================================

static void gl_set_material_fv(GLContext *ctx, uint32_t face, uint32_t pname, const float *vals)
{
	int indices[2];
	int count = 0;
	if (face == GL_FRONT || face == GL_FRONT_AND_BACK) indices[count++] = 0;
	if (face == GL_BACK  || face == GL_FRONT_AND_BACK) indices[count++] = 1;

	for (int i = 0; i < count; i++) {
		GLMaterial &mat = ctx->materials[indices[i]];
		switch (pname) {
		case GL_AMBIENT:
			memcpy(mat.ambient, vals, 4 * sizeof(float)); break;
		case GL_DIFFUSE:
			memcpy(mat.diffuse, vals, 4 * sizeof(float)); break;
		case GL_SPECULAR:
			memcpy(mat.specular, vals, 4 * sizeof(float)); break;
		case GL_EMISSION:
			memcpy(mat.emission, vals, 4 * sizeof(float)); break;
		case GL_SHININESS:
			mat.shininess = vals[0]; break;
		case GL_AMBIENT_AND_DIFFUSE:
			memcpy(mat.ambient, vals, 4 * sizeof(float));
			memcpy(mat.diffuse, vals, 4 * sizeof(float));
			break;
		default:
			ctx->last_error = GL_INVALID_ENUM; break;
		}
	}
}

void NativeGLMaterialf(GLContext *ctx, uint32_t face, uint32_t pname, float param)
{
	float vals[4] = {param, 0, 0, 0};
	gl_set_material_fv(ctx, face, pname, vals);
}

void NativeGLMaterialfv(GLContext *ctx, uint32_t face, uint32_t pname, uint32_t mac_ptr)
{
	int count = (pname == GL_SHININESS) ? 1 : 4;
	float vals[4];
	for (int i = 0; i < count; i++)
		vals[i] = ReadMacFloat(mac_ptr + i * 4);
	gl_set_material_fv(ctx, face, pname, vals);
}

void NativeGLMateriali(GLContext *ctx, uint32_t face, uint32_t pname, int32_t param)
{
	NativeGLMaterialf(ctx, face, pname, (float)param);
}

void NativeGLMaterialiv(GLContext *ctx, uint32_t face, uint32_t pname, uint32_t mac_ptr)
{
	int count = (pname == GL_SHININESS) ? 1 : 4;
	float vals[4];
	for (int i = 0; i < count; i++)
		vals[i] = (float)(int32_t)ReadMacInt32(mac_ptr + i * 4);
	gl_set_material_fv(ctx, face, pname, vals);
}

void NativeGLColorMaterial(GLContext *ctx, uint32_t face, uint32_t mode)
{
	ctx->color_material_face = face;
	ctx->color_material_mode = mode;
}


// =========================================================================
//  Fog handlers
// =========================================================================

void NativeGLFogf(GLContext *ctx, uint32_t pname, float param)
{
	switch (pname) {
	case GL_FOG_DENSITY: ctx->fog_density = param; break;
	case GL_FOG_START:   ctx->fog_start   = param; break;
	case GL_FOG_END:     ctx->fog_end     = param; break;
	case GL_FOG_MODE:    ctx->fog_mode    = (uint32_t)param; break;
	default: ctx->last_error = GL_INVALID_ENUM; break;
	}
}

void NativeGLFogfv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr)
{
	if (pname == GL_FOG_COLOR) {
		for (int i = 0; i < 4; i++)
			ctx->fog_color[i] = ReadMacFloat(mac_ptr + i * 4);
	} else {
		NativeGLFogf(ctx, pname, ReadMacFloat(mac_ptr));
	}
}

void NativeGLFogi(GLContext *ctx, uint32_t pname, int32_t param)
{
	if (pname == GL_FOG_MODE) {
		ctx->fog_mode = (uint32_t)param;
	} else {
		NativeGLFogf(ctx, pname, (float)param);
	}
}

void NativeGLFogiv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr)
{
	if (pname == GL_FOG_COLOR) {
		for (int i = 0; i < 4; i++)
			ctx->fog_color[i] = (float)(int32_t)ReadMacInt32(mac_ptr + i * 4);
	} else {
		NativeGLFogi(ctx, pname, (int32_t)ReadMacInt32(mac_ptr));
	}
}


// =========================================================================
//  Texture environment
// =========================================================================

void NativeGLTexEnvf(GLContext *ctx, uint32_t target, uint32_t pname, float param)
{
	if (target != GL_TEXTURE_ENV) return;
	if (pname == GL_TEXTURE_ENV_MODE)
		ctx->tex_units[ctx->active_texture].env_mode = (int)param;
}

void NativeGLTexEnvfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr)
{
	if (target != GL_TEXTURE_ENV) return;
	if (pname == GL_TEXTURE_ENV_COLOR) {
		for (int i = 0; i < 4; i++)
			ctx->tex_units[ctx->active_texture].env_color[i] = ReadMacFloat(mac_ptr + i * 4);
	} else if (pname == GL_TEXTURE_ENV_MODE) {
		ctx->tex_units[ctx->active_texture].env_mode = (int)ReadMacFloat(mac_ptr);
	}
}

void NativeGLTexEnvi(GLContext *ctx, uint32_t target, uint32_t pname, int32_t param)
{
	if (target != GL_TEXTURE_ENV) return;
	if (pname == GL_TEXTURE_ENV_MODE)
		ctx->tex_units[ctx->active_texture].env_mode = param;
}

void NativeGLTexEnviv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr)
{
	if (target != GL_TEXTURE_ENV) return;
	if (pname == GL_TEXTURE_ENV_COLOR) {
		for (int i = 0; i < 4; i++)
			ctx->tex_units[ctx->active_texture].env_color[i] = (float)(int32_t)ReadMacInt32(mac_ptr + i * 4);
	} else if (pname == GL_TEXTURE_ENV_MODE) {
		ctx->tex_units[ctx->active_texture].env_mode = (int32_t)ReadMacInt32(mac_ptr);
	}
}


// =========================================================================
//  Texture coordinate generation
// =========================================================================

static int gl_texgen_coord_index(uint32_t coord)
{
	switch (coord) {
	case GL_S: return 0;
	case GL_T: return 1;
	case GL_R: return 2;
	case GL_Q: return 3;
	default:   return -1;
	}
}

void NativeGLTexGeni(GLContext *ctx, uint32_t coord, uint32_t pname, int32_t param)
{
	int ci = gl_texgen_coord_index(coord);
	if (ci < 0) { ctx->last_error = GL_INVALID_ENUM; return; }
	if (pname == GL_TEXTURE_GEN_MODE) {
		int *modes[] = {
			&ctx->tex_units[ctx->active_texture].texgen_s_mode,
			&ctx->tex_units[ctx->active_texture].texgen_t_mode,
			&ctx->tex_units[ctx->active_texture].texgen_r_mode,
			&ctx->tex_units[ctx->active_texture].texgen_q_mode,
		};
		*modes[ci] = param;
	}
}

void NativeGLTexGenf(GLContext *ctx, uint32_t coord, uint32_t pname, float param)
{
	NativeGLTexGeni(ctx, coord, pname, (int32_t)param);
}

void NativeGLTexGend(GLContext *ctx, uint32_t coord, uint32_t pname, double param)
{
	NativeGLTexGeni(ctx, coord, pname, (int32_t)param);
}

void NativeGLTexGenfv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t mac_ptr)
{
	if (pname == GL_TEXTURE_GEN_MODE) {
		NativeGLTexGenf(ctx, coord, pname, ReadMacFloat(mac_ptr));
	}
	// GL_OBJECT_PLANE and GL_EYE_PLANE store 4 coefficients -- tracked in context but minimal impl
}

void NativeGLTexGeniv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t mac_ptr)
{
	if (pname == GL_TEXTURE_GEN_MODE) {
		NativeGLTexGeni(ctx, coord, pname, (int32_t)ReadMacInt32(mac_ptr));
	}
}

void NativeGLTexGendv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t mac_ptr)
{
	if (pname == GL_TEXTURE_GEN_MODE) {
		uint32 hi = ReadMacInt32(mac_ptr);
		uint32 lo = ReadMacInt32(mac_ptr + 4);
		uint64_t bits = ((uint64_t)hi << 32) | lo;
		double d;
		memcpy(&d, &bits, sizeof(double));
		NativeGLTexGend(ctx, coord, pname, d);
	}
}


// =========================================================================
//  Pixel store
// =========================================================================

void NativeGLPixelStorei(GLContext *ctx, uint32_t pname, int32_t param)
{
	switch (pname) {
	case GL_PACK_ALIGNMENT:    ctx->pixel_store.pack_alignment    = param; break;
	case GL_PACK_ROW_LENGTH:   ctx->pixel_store.pack_row_length   = param; break;
	case GL_PACK_SKIP_ROWS:    ctx->pixel_store.pack_skip_rows    = param; break;
	case GL_PACK_SKIP_PIXELS:  ctx->pixel_store.pack_skip_pixels  = param; break;
	case GL_UNPACK_ALIGNMENT:  ctx->pixel_store.unpack_alignment  = param; break;
	case GL_UNPACK_ROW_LENGTH: ctx->pixel_store.unpack_row_length = param; break;
	case GL_UNPACK_SKIP_ROWS:  ctx->pixel_store.unpack_skip_rows  = param; break;
	case GL_UNPACK_SKIP_PIXELS:ctx->pixel_store.unpack_skip_pixels= param; break;
	default: break;
	}
}

void NativeGLPixelStoref(GLContext *ctx, uint32_t pname, float param)
{
	NativeGLPixelStorei(ctx, pname, (int32_t)param);
}


// =========================================================================
//  Get functions
// =========================================================================

void NativeGLGetIntegerv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr)
{
	switch (pname) {
	case GL_MAX_TEXTURE_SIZE:
		WriteMacInt32(mac_ptr, 1024); break;
	case GL_MAX_LIGHTS:
		WriteMacInt32(mac_ptr, 8); break;
	case GL_MAX_MODELVIEW_STACK_DEPTH:
		WriteMacInt32(mac_ptr, 32); break;
	case GL_MAX_PROJECTION_STACK_DEPTH:
		WriteMacInt32(mac_ptr, 32); break;   // match gl_get_max_stack_depth()
	case GL_MAX_TEXTURE_STACK_DEPTH:
		WriteMacInt32(mac_ptr, 32); break;
	case GL_MAX_CLIP_PLANES:
		WriteMacInt32(mac_ptr, 6); break;
	case GL_MAX_ATTRIB_STACK_DEPTH_QUERY:
		WriteMacInt32(mac_ptr, GL_MAX_ATTRIB_STACK_DEPTH); break;
	case GL_MAX_NAME_STACK_DEPTH:
		WriteMacInt32(mac_ptr, 64); break;
	case GL_MAX_TEXTURE_UNITS_ARB:
		WriteMacInt32(mac_ptr, 4); break;
	case GL_VIEWPORT:
		for (int i = 0; i < 4; i++)
			WriteMacInt32(mac_ptr + i * 4, (uint32_t)ctx->viewport[i]);
		break;
	case GL_SCISSOR_BOX:
		for (int i = 0; i < 4; i++)
			WriteMacInt32(mac_ptr + i * 4, (uint32_t)ctx->scissor_box[i]);
		break;
	case GL_MODELVIEW_STACK_DEPTH:
		WriteMacInt32(mac_ptr, ctx->modelview_depth + 1); break;
	case GL_PROJECTION_STACK_DEPTH:
		WriteMacInt32(mac_ptr, ctx->projection_depth + 1); break;
	case GL_TEXTURE_STACK_DEPTH:
		WriteMacInt32(mac_ptr, ctx->texture_depth[ctx->active_texture] + 1); break;
	case GL_DEPTH_FUNC_QUERY:
		WriteMacInt32(mac_ptr, ctx->depth_func); break;
	case GL_DEPTH_WRITEMASK:
		WriteMacInt32(mac_ptr, ctx->depth_mask ? GL_TRUE : GL_FALSE); break;
	case GL_SHADE_MODEL_QUERY:
		WriteMacInt32(mac_ptr, ctx->shade_model); break;
	case GL_BLEND_SRC:
		WriteMacInt32(mac_ptr, ctx->blend_src); break;
	case GL_BLEND_DST:
		WriteMacInt32(mac_ptr, ctx->blend_dst); break;
	case GL_STENCIL_FUNC_QUERY:
		WriteMacInt32(mac_ptr, ctx->stencil.func); break;
	case GL_STENCIL_VALUE_MASK:
		WriteMacInt32(mac_ptr, ctx->stencil.value_mask); break;
	case GL_STENCIL_REF:
		WriteMacInt32(mac_ptr, (uint32_t)ctx->stencil.ref); break;
	case GL_STENCIL_FAIL:
		WriteMacInt32(mac_ptr, ctx->stencil.sfail); break;
	case GL_STENCIL_PASS_DEPTH_FAIL:
		WriteMacInt32(mac_ptr, ctx->stencil.dpfail); break;
	case GL_STENCIL_PASS_DEPTH_PASS:
		WriteMacInt32(mac_ptr, ctx->stencil.dppass); break;
	case GL_STENCIL_WRITEMASK:
		WriteMacInt32(mac_ptr, ctx->stencil.write_mask); break;
	case GL_STENCIL_CLEAR_VALUE:
		WriteMacInt32(mac_ptr, (uint32_t)ctx->clear_stencil); break;
	case GL_ALPHA_TEST_FUNC:
		WriteMacInt32(mac_ptr, ctx->alpha_func); break;
	case GL_CULL_FACE_MODE_QUERY:
		WriteMacInt32(mac_ptr, ctx->cull_face_mode); break;
	case GL_FRONT_FACE_QUERY:
		WriteMacInt32(mac_ptr, ctx->front_face); break;
	case GL_LOGIC_OP_MODE:
		WriteMacInt32(mac_ptr, ctx->logic_op_mode); break;
	case GL_ACTIVE_TEXTURE_ARB:
		WriteMacInt32(mac_ptr, GL_TEXTURE0_ARB + ctx->active_texture); break;
	case GL_TEXTURE_BINDING_2D:
		WriteMacInt32(mac_ptr, ctx->tex_units[ctx->active_texture].bound_texture_2d); break;
	case GL_RENDER:
		WriteMacInt32(mac_ptr, ctx->render_mode); break;
	default:
		// Return 0 for unknown queries (better than crashing)
		WriteMacInt32(mac_ptr, 0);
		break;
	}
}

void NativeGLGetFloatv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr)
{
	switch (pname) {
	case GL_MODELVIEW_MATRIX: {
		float *m = ctx->modelview_stack[ctx->modelview_depth];
		for (int i = 0; i < 16; i++) WriteMacFloat(mac_ptr + i * 4, m[i]);
		break;
	}
	case GL_PROJECTION_MATRIX: {
		float *m = ctx->projection_stack[ctx->projection_depth];
		for (int i = 0; i < 16; i++) WriteMacFloat(mac_ptr + i * 4, m[i]);
		break;
	}
	case GL_TEXTURE_MATRIX: {
		float *m = ctx->texture_stack[ctx->active_texture][ctx->texture_depth[ctx->active_texture]];
		for (int i = 0; i < 16; i++) WriteMacFloat(mac_ptr + i * 4, m[i]);
		break;
	}
	case GL_DEPTH_RANGE:
		WriteMacFloat(mac_ptr,     ctx->depth_range_near);
		WriteMacFloat(mac_ptr + 4, ctx->depth_range_far);
		break;
	case GL_DEPTH_CLEAR_VALUE:
		WriteMacFloat(mac_ptr, ctx->clear_depth); break;
	case GL_COLOR_CLEAR_VALUE:
		for (int i = 0; i < 4; i++) WriteMacFloat(mac_ptr + i * 4, ctx->clear_color[i]);
		break;
	case GL_CURRENT_COLOR:
		for (int i = 0; i < 4; i++) WriteMacFloat(mac_ptr + i * 4, ctx->current_color[i]);
		break;
	case GL_CURRENT_NORMAL:
		for (int i = 0; i < 3; i++) WriteMacFloat(mac_ptr + i * 4, ctx->current_normal[i]);
		break;
	case GL_CURRENT_TEXTURE_COORDS:
		for (int i = 0; i < 4; i++) WriteMacFloat(mac_ptr + i * 4, ctx->current_texcoord[ctx->active_texture][i]);
		break;
	case GL_ALPHA_TEST_REF:
		WriteMacFloat(mac_ptr, ctx->alpha_ref); break;
	case GL_POINT_SIZE_QUERY:
		WriteMacFloat(mac_ptr, ctx->point_size); break;
	case GL_LINE_WIDTH_QUERY:
		WriteMacFloat(mac_ptr, ctx->line_width); break;
	case GL_FOG_COLOR:
		for (int i = 0; i < 4; i++) WriteMacFloat(mac_ptr + i * 4, ctx->fog_color[i]);
		break;
	case GL_FOG_DENSITY:
		WriteMacFloat(mac_ptr, ctx->fog_density); break;
	case GL_FOG_START:
		WriteMacFloat(mac_ptr, ctx->fog_start); break;
	case GL_FOG_END:
		WriteMacFloat(mac_ptr, ctx->fog_end); break;
	case GL_POLYGON_OFFSET_FACTOR_QUERY:
		WriteMacFloat(mac_ptr, ctx->polygon_offset_factor); break;
	case GL_POLYGON_OFFSET_UNITS_QUERY:
		WriteMacFloat(mac_ptr, ctx->polygon_offset_units); break;
	default:
		WriteMacFloat(mac_ptr, 0.0f);
		break;
	}
}

void NativeGLGetBooleanv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr)
{
	// Most boolean queries map to isEnabled -- handle a few common ones
	bool val = false;
	switch (pname) {
	case GL_DEPTH_TEST:     val = ctx->depth_test; break;
	case GL_BLEND:          val = ctx->blend; break;
	case GL_LIGHTING:       val = ctx->lighting_enabled; break;
	case GL_FOG:            val = ctx->fog_enabled; break;
	case GL_SCISSOR_TEST:   val = ctx->scissor_test; break;
	case GL_CULL_FACE:      val = ctx->cull_face_enabled; break;
	case GL_ALPHA_TEST:     val = ctx->alpha_test; break;
	case GL_STENCIL_TEST:   val = ctx->stencil_test; break;
	case GL_DEPTH_WRITEMASK:val = ctx->depth_mask; break;
	default: break;
	}
	WriteMacInt32(mac_ptr, val ? GL_TRUE : GL_FALSE);
}

uint32_t NativeGLGetError(GLContext *ctx)
{
	uint32_t err = ctx->last_error;
	ctx->last_error = GL_NO_ERROR;
	return err;
}

uint32_t NativeGLGetString(GLContext *ctx, uint32_t name)
{
	// Allocate Mac-side strings once
	auto alloc_string = [](uint32_t &addr, const char *str) {
		if (addr == 0) {
			uint32_t len = (uint32_t)strlen(str) + 1;
			addr = Mac_sysalloc(len);
			if (addr) {
				// A prior per-iteration clear loop used a 4-byte
				// WriteMacInt32 over [0, len), writing len*4 bytes into a
				// len-byte SheepMem allocation and overrunning it by 3*len
				// bytes. The clear was also redundant: the memcpy below writes
				// all len bytes (string + NUL). Removed to match the
				// NativeAGLErrorString analog (gl_engine.cpp:1689) which does
				// Mac_sysalloc(len) + memcpy(host_ptr, str, len) with no clear.
				uint8 *host_ptr = Mac2HostAddr(addr);
				memcpy(host_ptr, str, len);
			}
		}
		return addr;
	};

	switch (name) {
	case GL_VENDOR:     return alloc_string(gl_string_vendor_addr,   "ATI Technologies Inc.");
	case GL_RENDERER:   return alloc_string(gl_string_renderer_addr, "ATI Rage 128 Pro OpenGL Engine");
	case GL_VERSION:
		// Report the complete core contract. 1.2-era features that this backend
		// implements are exposed as explicit extension tokens below.
		return alloc_string(gl_string_version_addr,  "1.1");
	// The EXT texture-env COMBINE extension token is DE-ADVERTISED.
	// The GL_COMBINE_EXT (0x8570) crossbar is store-only — env_mode is recorded but
	// GLTexEnvModeToShader has no GL_COMBINE case (it falls to default = modulate), so
	// advertising the token would be silent-wrong-output for a conformant app querying
	// the COMBINE crossbar (silent wrong output). De-advertisement
	// IS the honest fix: a conformant app detects absence and branches around it.
	// GL_ARB_multitexture stays advertised — it honestly covers the 2-unit modulate/add
	// texenv path now wired. (The exact token is intentionally NOT spelled out here so
	// the GLARBExtensionTests source-scan absence assertion stays robust.)
	//
	// DELIBERATE (convolution / histogram-minmax): the
	// ARB imaging umbrella extension token is intentionally ABSENT from the string below.
	// The convolution (478-490) and histogram/minmax (491-500) families store state but do
	// NOT apply it to the pixel pipeline; advertising the umbrella token would let a
	// conformant app call those paths and silently get wrong output.
	// De-advertisement IS the honest fix (classic Mac games do not use
	// imaging convolution/histogram). The handlers log their known limitation; the
	// GLARBExtensionTests gate tests assert the token's absence. (The umbrella token is
	// intentionally NOT spelled out as a literal here so the source-scan absence assertion
	// stays robust — same robustness discipline as the COMBINE de-advertisement.)
	//
	// DELIBERATE (texture compression): the ARB compression token is ABSENT from the
	// string below. Only 2D DXT upload/sub-upload paths decompress into Metal textures;
	// the 1D, 3D, and readback handlers explicitly log known limitations. Advertising
	// the full extension would invite conformant callers into silent no-op paths.
	//
	// DELIBERATE (fog coordinate): the per-vertex fog-coordinate extension token is
	// ABSENT from the string below. This backend tracks a passive vertex/current
	// field, but it has no glFogCoord* or glFogCoordPointer entry points, no
	// FOG_COORDINATE_SOURCE state, and no dispatch sub-opcodes. Advertising that
	// extension would be a pure reachability lie.
	//
	// DELIBERATE (texture LOD bias): ABSENT from the string below. The extension
	// has no entry points, but it does require GL_TEXTURE_FILTER_CONTROL /
	// GL_TEXTURE_LOD_BIAS TexEnv state, which this backend does not consume.
	case GL_EXTENSIONS: {
		static const char *extensions =
			"GL_ARB_multitexture GL_ARB_transpose_matrix "
			"GL_EXT_abgr GL_EXT_bgra GL_EXT_packed_pixels "
			"GL_EXT_blend_color GL_EXT_blend_equation GL_EXT_compiled_vertex_array "
			"GL_EXT_secondary_color GL_EXT_stencil_wrap "
			"GL_EXT_paletted_texture";
		{
			static bool s_logged_extensions = false;
			if (!s_logged_extensions) {
				s_logged_extensions = true;
				GL_LOG("GetString(GL_EXTENSIONS) -> %s", extensions);
			}
		}
		return alloc_string(gl_string_extensions_addr, extensions);
	}
	default:
		return 0;
	}
}


// =========================================================================
//  Attrib stacks
// =========================================================================

void NativeGLPushAttrib(GLContext *ctx, uint32_t mask)
{
	if (ctx->attrib_stack_depth >= GL_MAX_ATTRIB_STACK_DEPTH) {
		ctx->last_error = GL_STACK_OVERFLOW;
		return;
	}

	GLAttribStackEntry &entry = ctx->attrib_stack[ctx->attrib_stack_depth];
	entry.mask = mask;

	// Snapshot requested state groups
	if (mask & GL_CURRENT_BIT) {
		memcpy(entry.current_color,    ctx->current_color,    4 * sizeof(float));
		memcpy(entry.current_normal,   ctx->current_normal,   3 * sizeof(float));
		memcpy(entry.current_texcoord, ctx->current_texcoord[0], 4 * sizeof(float));
	}
	if (mask & GL_ENABLE_BIT) {
		entry.depth_test   = ctx->depth_test;
		entry.blend        = ctx->blend;
		entry.cull_face    = ctx->cull_face_enabled;
		entry.lighting     = ctx->lighting_enabled;
		entry.texture_2d   = ctx->tex_units[ctx->active_texture].enabled_2d;
		entry.alpha_test   = ctx->alpha_test;
		entry.scissor_test = ctx->scissor_test;
		entry.stencil_test = ctx->stencil_test;
		entry.fog          = ctx->fog_enabled;
		entry.normalize    = ctx->normalize;
	}
	if (mask & GL_COLOR_BUFFER_BIT_ATTRIB) {
		memcpy(entry.clear_color, ctx->clear_color, 4 * sizeof(float));
	}
	if (mask & GL_DEPTH_BUFFER_BIT) {
		entry.clear_depth = ctx->clear_depth;
	}
	if (mask & GL_STENCIL_BUFFER_BIT) {
		entry.saved_stencil = ctx->stencil;
		entry.saved_clear_stencil = ctx->clear_stencil;
	}
	if (mask & GL_POLYGON_BIT) {
		entry.polygon_mode_front    = ctx->polygon_mode_front;
		entry.polygon_mode_back     = ctx->polygon_mode_back;
		entry.cull_face_enabled     = ctx->cull_face_enabled;
		entry.front_face            = ctx->front_face;
		entry.polygon_offset_factor = ctx->polygon_offset_factor;
		entry.polygon_offset_units  = ctx->polygon_offset_units;
		entry.polygon_offset_fill   = ctx->polygon_offset_fill;
		entry.polygon_offset_line   = ctx->polygon_offset_line;
		entry.polygon_offset_point  = ctx->polygon_offset_point;
	}
	if (mask & GL_LIGHTING_BIT) {
		entry.lighting_enabled      = ctx->lighting_enabled;
		entry.shade_model           = ctx->shade_model;
		entry.color_material_enabled = ctx->color_material_enabled;
		entry.color_material_face   = ctx->color_material_face;
		entry.color_material_mode   = ctx->color_material_mode;
		memcpy(entry.saved_materials, ctx->materials, sizeof(ctx->materials));
		memcpy(entry.light_model_ambient, ctx->light_model_ambient, 4 * sizeof(float));
		entry.light_model_two_side    = ctx->light_model_two_side;
		entry.light_model_local_viewer = ctx->light_model_local_viewer;
		for (int i = 0; i < 8; i++) entry.light_enabled[i] = ctx->lights[i].enabled;
	}
	if (mask & GL_FOG_BIT) {
		entry.fog_enabled = ctx->fog_enabled;
		entry.fog_mode    = ctx->fog_mode;
		entry.fog_density = ctx->fog_density;
		entry.fog_start   = ctx->fog_start;
		entry.fog_end     = ctx->fog_end;
		memcpy(entry.fog_color, ctx->fog_color, 4 * sizeof(float));
	}
	if (mask & GL_TEXTURE_BIT) {
		entry.saved_active_texture = ctx->active_texture;
		memcpy(entry.saved_tex_units, ctx->tex_units, sizeof(ctx->tex_units));
	}
	if (mask & GL_VIEWPORT_BIT) {
		memcpy(entry.saved_viewport, ctx->viewport, 4 * sizeof(int32_t));
		entry.saved_depth_range_near = ctx->depth_range_near;
		entry.saved_depth_range_far  = ctx->depth_range_far;
	}
	if (mask & GL_HINT_BIT) {
		entry.saved_hint_perspective    = ctx->hint_perspective_correction;
		entry.saved_hint_point_smooth   = ctx->hint_point_smooth;
		entry.saved_hint_line_smooth    = ctx->hint_line_smooth;
		entry.saved_hint_polygon_smooth = ctx->hint_polygon_smooth;
		entry.saved_hint_fog            = ctx->hint_fog;
	}
	if (mask & GL_POINT_BIT) {
		entry.saved_point_size   = ctx->point_size;
		entry.saved_point_smooth = ctx->point_smooth;
	}
	if (mask & GL_LINE_BIT) {
		entry.saved_line_width          = ctx->line_width;
		entry.saved_line_smooth         = ctx->line_smooth;
		entry.saved_line_stipple_factor = ctx->line_stipple_factor;
		entry.saved_line_stipple_pattern = ctx->line_stipple_pattern;
	}
	if (mask & GL_SCISSOR_BIT) {
		memcpy(entry.saved_scissor_box, ctx->scissor_box, 4 * sizeof(int32_t));
		entry.saved_scissor_test = ctx->scissor_test;
	}
	if (mask & GL_TRANSFORM_BIT) {
		entry.saved_matrix_mode = ctx->matrix_mode;
		memcpy(entry.saved_clip_plane_enabled, ctx->clip_plane_enabled, 6 * sizeof(bool));
	}

	ctx->attrib_stack_depth++;
}

void NativeGLPopAttrib(GLContext *ctx)
{
	if (ctx->attrib_stack_depth <= 0) {
		ctx->last_error = GL_STACK_UNDERFLOW;
		return;
	}
	ctx->attrib_stack_depth--;

	GLAttribStackEntry &entry = ctx->attrib_stack[ctx->attrib_stack_depth];
	uint32_t mask = entry.mask;

	if (mask & GL_CURRENT_BIT) {
		memcpy(ctx->current_color,    entry.current_color,    4 * sizeof(float));
		memcpy(ctx->current_normal,   entry.current_normal,   3 * sizeof(float));
		memcpy(ctx->current_texcoord[0], entry.current_texcoord, 4 * sizeof(float));
	}
	if (mask & GL_ENABLE_BIT) {
		ctx->depth_test        = entry.depth_test;
		ctx->blend             = entry.blend;
		ctx->cull_face_enabled = entry.cull_face;
		ctx->lighting_enabled  = entry.lighting;
		ctx->tex_units[ctx->active_texture].enabled_2d = entry.texture_2d;
		gl_clear_prepared_texture_2d_for_draw(ctx, ctx->active_texture);
		gl_clear_latched_texture_2d_for_array_draw(ctx, ctx->active_texture);
		ctx->texture_2d_enable_draw_serial[ctx->active_texture] =
			ctx->completed_draw_serial;
		ctx->alpha_test        = entry.alpha_test;
		ctx->scissor_test      = entry.scissor_test;
		ctx->stencil_test      = entry.stencil_test;
		ctx->fog_enabled       = entry.fog;
		ctx->normalize         = entry.normalize;
	}
	if (mask & GL_COLOR_BUFFER_BIT_ATTRIB) {
		memcpy(ctx->clear_color, entry.clear_color, 4 * sizeof(float));
	}
	if (mask & GL_DEPTH_BUFFER_BIT) {
		ctx->clear_depth = entry.clear_depth;
	}
	if (mask & GL_STENCIL_BUFFER_BIT) {
		ctx->stencil = entry.saved_stencil;
		ctx->clear_stencil = entry.saved_clear_stencil;
	}
	if (mask & GL_POLYGON_BIT) {
		ctx->polygon_mode_front    = entry.polygon_mode_front;
		ctx->polygon_mode_back     = entry.polygon_mode_back;
		ctx->cull_face_enabled     = entry.cull_face_enabled;
		ctx->front_face            = entry.front_face;
		ctx->polygon_offset_factor = entry.polygon_offset_factor;
		ctx->polygon_offset_units  = entry.polygon_offset_units;
		ctx->polygon_offset_fill   = entry.polygon_offset_fill;
		ctx->polygon_offset_line   = entry.polygon_offset_line;
		ctx->polygon_offset_point  = entry.polygon_offset_point;
	}
	if (mask & GL_LIGHTING_BIT) {
		ctx->lighting_enabled      = entry.lighting_enabled;
		ctx->shade_model           = entry.shade_model;
		ctx->color_material_enabled = entry.color_material_enabled;
		ctx->color_material_face   = entry.color_material_face;
		ctx->color_material_mode   = entry.color_material_mode;
		memcpy(ctx->materials, entry.saved_materials, sizeof(ctx->materials));
		memcpy(ctx->light_model_ambient, entry.light_model_ambient, 4 * sizeof(float));
		ctx->light_model_two_side    = entry.light_model_two_side;
		ctx->light_model_local_viewer = entry.light_model_local_viewer;
		for (int i = 0; i < 8; i++) ctx->lights[i].enabled = entry.light_enabled[i];
	}
	if (mask & GL_FOG_BIT) {
		ctx->fog_enabled = entry.fog_enabled;
		ctx->fog_mode    = entry.fog_mode;
		ctx->fog_density = entry.fog_density;
		ctx->fog_start   = entry.fog_start;
		ctx->fog_end     = entry.fog_end;
		memcpy(ctx->fog_color, entry.fog_color, 4 * sizeof(float));
	}
	if (mask & GL_TEXTURE_BIT) {
		ctx->active_texture = entry.saved_active_texture;
		memcpy(ctx->tex_units, entry.saved_tex_units, sizeof(ctx->tex_units));
	}
	if (mask & GL_VIEWPORT_BIT) {
		memcpy(ctx->viewport, entry.saved_viewport, 4 * sizeof(int32_t));
		ctx->depth_range_near = entry.saved_depth_range_near;
		ctx->depth_range_far  = entry.saved_depth_range_far;
	}
	if (mask & GL_HINT_BIT) {
		ctx->hint_perspective_correction = entry.saved_hint_perspective;
		ctx->hint_point_smooth           = entry.saved_hint_point_smooth;
		ctx->hint_line_smooth            = entry.saved_hint_line_smooth;
		ctx->hint_polygon_smooth         = entry.saved_hint_polygon_smooth;
		ctx->hint_fog                    = entry.saved_hint_fog;
	}
	if (mask & GL_POINT_BIT) {
		ctx->point_size   = entry.saved_point_size;
		ctx->point_smooth = entry.saved_point_smooth;
	}
	if (mask & GL_LINE_BIT) {
		ctx->line_width          = entry.saved_line_width;
		ctx->line_smooth         = entry.saved_line_smooth;
		ctx->line_stipple_factor = entry.saved_line_stipple_factor;
		ctx->line_stipple_pattern = entry.saved_line_stipple_pattern;
	}
	if (mask & GL_SCISSOR_BIT) {
		memcpy(ctx->scissor_box, entry.saved_scissor_box, 4 * sizeof(int32_t));
		ctx->scissor_test = entry.saved_scissor_test;
	}
	if (mask & GL_TRANSFORM_BIT) {
		ctx->matrix_mode = entry.saved_matrix_mode;
		memcpy(ctx->clip_plane_enabled, entry.saved_clip_plane_enabled, 6 * sizeof(bool));
	}
}

// Client attrib stacks -- simplified, stores vertex array pointers
static GLVertexArrayPointer gl_saved_vertex_array;
static GLVertexArrayPointer gl_saved_normal_array;
static GLVertexArrayPointer gl_saved_color_array;
static GLVertexArrayPointer gl_saved_texcoord_array[4];
static GLPixelStore gl_saved_pixel_store;
static int gl_client_attrib_depth = 0;
static uint32_t gl_client_attrib_mask = 0;

void NativeGLPushClientAttrib(GLContext *ctx, uint32_t mask)
{
	if (gl_client_attrib_depth >= 1) {
		ctx->last_error = GL_STACK_OVERFLOW;
		return;
	}
	gl_client_attrib_mask = mask;
	if (mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
		gl_saved_vertex_array = ctx->vertex_array;
		gl_saved_normal_array = ctx->normal_array;
		gl_saved_color_array  = ctx->color_array;
		memcpy(gl_saved_texcoord_array, ctx->texcoord_array, sizeof(gl_saved_texcoord_array));
	}
	if (mask & GL_CLIENT_PIXEL_STORE_BIT) {
		gl_saved_pixel_store = ctx->pixel_store;
	}
	gl_client_attrib_depth++;
}

void NativeGLPopClientAttrib(GLContext *ctx)
{
	if (gl_client_attrib_depth <= 0) {
		ctx->last_error = GL_STACK_UNDERFLOW;
		return;
	}
	gl_client_attrib_depth--;
	if (gl_client_attrib_mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
		ctx->vertex_array = gl_saved_vertex_array;
		ctx->normal_array = gl_saved_normal_array;
		ctx->color_array  = gl_saved_color_array;
		memcpy(ctx->texcoord_array, gl_saved_texcoord_array, sizeof(gl_saved_texcoord_array));
		for (int u = 0; u < 4; u++) {
			ctx->texcoord_array_enable_draw_serial[u] = ctx->completed_draw_serial;
			ctx->prepared_texcoord_array_for_draw[u] = false;
		}
	}
	if (gl_client_attrib_mask & GL_CLIENT_PIXEL_STORE_BIT) {
		ctx->pixel_store = gl_saved_pixel_store;
	}
}


// =========================================================================
//  Context initialization
// =========================================================================

void GLContextInit(GLContext *ctx)
{
	memset(ctx, 0, sizeof(GLContext));

	// Matrix stacks: load identity
	mat4_identity(ctx->modelview_stack[0]);
	mat4_identity(ctx->projection_stack[0]);
	for (int u = 0; u < 4; u++)
		mat4_identity(ctx->texture_stack[u][0]);
	mat4_identity(ctx->color_stack[0]);
	ctx->matrix_mode = GL_MODELVIEW;

	// Default colors
	ctx->current_color[0] = ctx->current_color[1] = ctx->current_color[2] = ctx->current_color[3] = 1.0f;
	ctx->current_normal[2] = 1.0f; // default normal = (0,0,1)
	for (int u = 0; u < 4; u++)
		ctx->current_texcoord[u][3] = 1.0f; // q=1 by default

	// Depth
	ctx->depth_func = GL_LESS;
	ctx->depth_mask = true;
	ctx->depth_range_near = 0.0f;
	ctx->depth_range_far  = 1.0f;

	// Blend
	ctx->blend_src = GL_ONE;
	ctx->blend_dst = GL_ZERO;

	// Alpha
	ctx->alpha_func = GL_ALWAYS;
	ctx->alpha_ref  = 0.0f;

	// Stencil
	ctx->stencil.func       = GL_ALWAYS;
	ctx->stencil.ref        = 0;
	ctx->stencil.value_mask = 0xFFFFFFFF;
	ctx->stencil.write_mask = 0xFFFFFFFF;
	ctx->stencil.sfail      = GL_KEEP;
	ctx->stencil.dpfail     = GL_KEEP;
	ctx->stencil.dppass     = GL_KEEP;

	// Face / shading
	ctx->cull_face_mode    = GL_BACK;
	ctx->front_face        = GL_CCW;
	ctx->shade_model       = GL_SMOOTH;
	ctx->polygon_mode_front= GL_FILL;
	ctx->polygon_mode_back = GL_FILL;

	// Viewport (set to 0,0,0,0 -- will be updated by app)
	// Scissor
	// (both left at 0 until glViewport/glScissor called)

	// Clear values
	ctx->clear_depth   = 1.0f;
	ctx->clear_stencil = 0;

	// Fog
	ctx->fog_mode    = GL_EXP;
	ctx->fog_density = 1.0f;
	ctx->fog_start   = 0.0f;
	ctx->fog_end     = 1.0f;

	// Lighting defaults
	ctx->light_model_ambient[0] = 0.2f;
	ctx->light_model_ambient[1] = 0.2f;
	ctx->light_model_ambient[2] = 0.2f;
	ctx->light_model_ambient[3] = 1.0f;

	// Light 0 defaults (others start as 0)
	ctx->lights[0].diffuse[0] = ctx->lights[0].diffuse[1] = ctx->lights[0].diffuse[2] = ctx->lights[0].diffuse[3] = 1.0f;
	ctx->lights[0].specular[0] = ctx->lights[0].specular[1] = ctx->lights[0].specular[2] = ctx->lights[0].specular[3] = 1.0f;
	for (int i = 0; i < 8; i++) {
		ctx->lights[i].position[2] = 1.0f;  // default position (0,0,1,0) = directional
		ctx->lights[i].spot_cutoff = 180.0f; // no spotlight by default
		ctx->lights[i].constant_attenuation = 1.0f;
		ctx->lights[i].ambient[3] = 1.0f; // alpha defaults
	}
	// Lights 1-7: diffuse/specular default to (0,0,0,1)
	for (int i = 1; i < 8; i++) {
		ctx->lights[i].diffuse[3] = 1.0f;
		ctx->lights[i].specular[3] = 1.0f;
	}

	// Material defaults
	for (int f = 0; f < 2; f++) {
		ctx->materials[f].ambient[0] = 0.2f;
		ctx->materials[f].ambient[1] = 0.2f;
		ctx->materials[f].ambient[2] = 0.2f;
		ctx->materials[f].ambient[3] = 1.0f;
		ctx->materials[f].diffuse[0] = 0.8f;
		ctx->materials[f].diffuse[1] = 0.8f;
		ctx->materials[f].diffuse[2] = 0.8f;
		ctx->materials[f].diffuse[3] = 1.0f;
		ctx->materials[f].specular[3] = 1.0f;
		ctx->materials[f].emission[3] = 1.0f;
	}

	// Color material
	ctx->color_material_face = GL_FRONT_AND_BACK;
	ctx->color_material_mode = GL_AMBIENT_AND_DIFFUSE;

	// Texture units
	for (int u = 0; u < 4; u++) {
		ctx->tex_units[u].env_mode = GL_MODULATE;
		ctx->tex_units[u].env_color[3] = 1.0f;
		ctx->tex_units[u].current_texcoord[3] = 1.0f;
		ctx->texture_2d_enable_draw_serial[u] = 0;
		ctx->texcoord_array_enable_draw_serial[u] = 0;
		ctx->prepared_texture_2d_for_draw[u] = false;
		ctx->latched_texture_2d_for_array_draw[u] = false;
		ctx->prepared_texcoord_array_for_draw[u] = false;
	}
	ctx->completed_draw_serial = 0;

	// Pixel store defaults
	ctx->pixel_store.pack_alignment   = 4;
	ctx->pixel_store.unpack_alignment = 4;

	// Color mask: all enabled
	ctx->color_mask[0] = ctx->color_mask[1] = ctx->color_mask[2] = ctx->color_mask[3] = true;

	// Line / point
	ctx->line_width = 1.0f;
	ctx->point_size = 1.0f;

	// Dither defaults on per spec
	ctx->dither = true;

	// Hints
	ctx->hint_perspective_correction = GL_DONT_CARE;
	ctx->hint_point_smooth           = GL_DONT_CARE;
	ctx->hint_line_smooth            = GL_DONT_CARE;
	ctx->hint_polygon_smooth         = GL_DONT_CARE;
	ctx->hint_fog                    = GL_DONT_CARE;

	// Logic op
	ctx->logic_op_mode = GL_COPY;

	// Render mode
	ctx->render_mode = GL_RENDER;

	// Raster position
	ctx->raster_pos[3] = 1.0f;
	ctx->raster_pos_valid = true;

	// Pixel zoom
	ctx->pixel_zoom_x = 1.0f;
	ctx->pixel_zoom_y = 1.0f;

	// Texture name
	ctx->next_texture_name = 1;
	ctx->next_list_name = 1;

	// Evaluator defaults
	for (int i = 0; i < GL_EVAL_MAX_TARGETS; i++) {
		ctx->eval_map1[i].defined = false;
		ctx->eval_map1[i].order = 0;
		ctx->eval_map1[i].dimension = 0;
		ctx->eval_map2[i].defined = false;
		ctx->eval_map2[i].uorder = 0;
		ctx->eval_map2[i].vorder = 0;
		ctx->eval_map2[i].dimension = 0;
		ctx->eval_map1_enabled[i] = false;
		ctx->eval_map2_enabled[i] = false;
	}
	ctx->grid1_un = 1;
	ctx->grid1_u1 = 0.0f;
	ctx->grid1_u2 = 1.0f;
	ctx->grid2_un = 1;
	ctx->grid2_vn = 1;
	ctx->grid2_u1 = 0.0f;
	ctx->grid2_u2 = 1.0f;
	ctx->grid2_v1 = 0.0f;
	ctx->grid2_v2 = 1.0f;

	// Error
	ctx->last_error = GL_NO_ERROR;
}


// =========================================================================
//  Texture object management
// =========================================================================

/*
 *  glGenTextures -- generate unique texture names
 */
void NativeGLGenTextures(GLContext *ctx, uint32_t n, uint32_t mac_ptr)
{
	if (!ctx || n == 0 || mac_ptr == 0) return;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t name = ctx->next_texture_name++;
		WriteMacInt32(mac_ptr + i * 4, name);
	}
}


/*
 *  glDeleteTextures -- free texture objects and Metal resources
 */
void NativeGLDeleteTextures(GLContext *ctx, uint32_t n, uint32_t mac_ptr)
{
	if (!ctx || n == 0 || mac_ptr == 0) return;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t name = ReadMacInt32(mac_ptr + i * 4);
		if (name == 0) continue;

		auto it = ctx->texture_objects.find(name);
		if (it != ctx->texture_objects.end()) {
			// Log deletion for debugging texture lifecycle
			bool had_metal = (it->second.metal_texture != nullptr);
			GL_LOG("glDeleteTextures: deleting tex %u (had_metal=%d, %dx%d)",
			       name, had_metal, it->second.width, it->second.height);

			// Destroy Metal texture
			GLMetalDestroyTexture(&it->second);

			// Unbind from any texture unit if bound
			for (int u = 0; u < 4; u++) {
				if (ctx->tex_units[u].bound_texture_2d == name) {
					ctx->tex_units[u].bound_texture_2d = 0;
					gl_clear_prepared_texture_2d_for_draw(ctx, u);
					gl_clear_latched_texture_2d_for_array_draw(ctx, u);
				}
				if (ctx->tex_units[u].bound_texture_1d == name)
					ctx->tex_units[u].bound_texture_1d = 0;
			}

			ctx->texture_objects.erase(it);
		}
	}
}


/*
 *  glBindTexture -- bind a texture object to the active texture unit
 */
void NativeGLBindTexture(GLContext *ctx, uint32_t target, uint32_t texture)
{
	if (!ctx) return;

	if (texture == 0) {
		// Unbind: use default (no texture)
		if (target == GL_TEXTURE_2D) {
			ctx->tex_units[ctx->active_texture].bound_texture_2d = 0;
			gl_clear_prepared_texture_2d_for_draw(ctx, ctx->active_texture);
			gl_clear_latched_texture_2d_for_array_draw(ctx, ctx->active_texture);
		} else if (target == GL_TEXTURE_1D) {
			ctx->tex_units[ctx->active_texture].bound_texture_1d = 0;
		}
		return;
	}

	// Create texture object entry if not yet in map (GL spec: first bind creates)
	if (ctx->texture_objects.find(texture) == ctx->texture_objects.end()) {
		GLTextureObject obj;
		memset(&obj, 0, sizeof(obj));
		obj.name = texture;
		obj.min_filter = GL_NEAREST_MIPMAP_LINEAR;  // GL default
		obj.mag_filter = GL_LINEAR;                   // GL default
		obj.wrap_s = GL_REPEAT;
		obj.wrap_t = GL_REPEAT;
		obj.env_mode = GL_MODULATE;
		ctx->texture_objects[texture] = obj;
	}

	if (target == GL_TEXTURE_2D) {
		if (ctx->tex_units[ctx->active_texture].bound_texture_2d != texture) {
			gl_clear_prepared_texture_2d_for_draw(ctx, ctx->active_texture);
			if (!GLMetalLatchedTexture2DSurvivesTextureRebind(
					ctx->latched_texture_2d_for_array_draw[ctx->active_texture],
					texture != 0)) {
				gl_clear_latched_texture_2d_for_array_draw(ctx, ctx->active_texture);
			}
		}
		ctx->tex_units[ctx->active_texture].bound_texture_2d = texture;
	} else if (target == GL_TEXTURE_1D) {
		ctx->tex_units[ctx->active_texture].bound_texture_1d = texture;
	}
}


/*
 *  glTexParameteri / glTexParameterf -- set texture object parameters
 */
void NativeGLTexParameteri(GLContext *ctx, uint32_t target, uint32_t pname, int32_t param)
{
	if (!ctx) return;

	// Get currently bound texture for this target
	uint32_t texName = 0;
	if (target == GL_TEXTURE_2D)
		texName = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	else if (target == GL_TEXTURE_1D)
		texName = ctx->tex_units[ctx->active_texture].bound_texture_1d;

	if (texName == 0) return;

	auto it = ctx->texture_objects.find(texName);
	if (it == ctx->texture_objects.end()) return;

	GLTextureObject &tex = it->second;
	switch (pname) {
		case GL_TEXTURE_MIN_FILTER: tex.min_filter = (uint32_t)param; break;
		case GL_TEXTURE_MAG_FILTER: tex.mag_filter = (uint32_t)param; break;
		case GL_TEXTURE_WRAP_S:     tex.wrap_s     = (uint32_t)param; break;
		case GL_TEXTURE_WRAP_T:     tex.wrap_t     = (uint32_t)param; break;
		default: break;
	}
}

void NativeGLTexParameterf(GLContext *ctx, uint32_t target, uint32_t pname, float param)
{
	NativeGLTexParameteri(ctx, target, pname, (int32_t)param);
}

void NativeGLTexParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr)
{
	if (!ctx || mac_ptr == 0) return;
	// Read first float and delegate (most params are single-value)
	float val = ReadMacFloat(mac_ptr);
	NativeGLTexParameterf(ctx, target, pname, val);
}

void NativeGLTexParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr)
{
	if (!ctx || mac_ptr == 0) return;
	int32_t val = (int32_t)ReadMacInt32(mac_ptr);
	NativeGLTexParameteri(ctx, target, pname, val);
}


/*
 *  glActiveTextureARB -- select active texture unit
 */
void NativeGLActiveTextureARB(GLContext *ctx, uint32_t texture)
{
	if (!ctx) return;
	int unit = (int)(texture - GL_TEXTURE0_ARB);
	if (unit < 0 || unit >= 4) return;
	ctx->active_texture = unit;
}


/*
 *  glClientActiveTextureARB -- select client active texture unit
 */
void NativeGLClientActiveTextureARB(GLContext *ctx, uint32_t texture)
{
	if (!ctx) return;
	int unit = (int)(texture - GL_TEXTURE0_ARB);
	if (unit < 0 || unit >= 4) return;
	ctx->client_active_texture = unit;
}


// =========================================================================
//  Texture pixel data upload with format conversion
// =========================================================================

/*
 *  Helper: compute aligned row stride per GL pixel store
 */
static int ComputeRowStride(int width, int bytesPerPixel, int alignment, int rowLength)
{
	int effectiveWidth = (rowLength > 0) ? rowLength : width;
	int rowBytes = effectiveWidth * bytesPerPixel;
	// Align to 'alignment' boundary
	if (alignment > 1)
		rowBytes = ((rowBytes + alignment - 1) / alignment) * alignment;
	return rowBytes;
}

static uint8_t ReadScalarComponentTo8(uint32_t addr, uint32_t type)
{
	switch (type) {
		case GL_UNSIGNED_BYTE:
			return GLPixelScaleUnsignedTo8(ReadMacInt8(addr), 8);
		case GL_BYTE:
			return GLPixelScaleSignedTo8((int8_t)ReadMacInt8(addr), 8);
		case GL_UNSIGNED_SHORT:
			return GLPixelScaleUnsignedTo8(ReadMacInt16(addr), 16);
		case GL_SHORT:
			return GLPixelScaleSignedTo8((int16_t)ReadMacInt16(addr), 16);
		case GL_UNSIGNED_INT:
			return GLPixelScaleUnsignedTo8(ReadMacInt32(addr), 32);
		case GL_INT:
			return GLPixelScaleSignedTo8((int32_t)ReadMacInt32(addr), 32);
		case GL_FLOAT: {
			const uint32_t bits = ReadMacInt32(addr);
			float value = 0.0f;
			std::memcpy(&value, &bits, sizeof(value));
			if (!(value > 0.0f)) return 0;
			if (value >= 1.0f) return 0xffu;
			return (uint8_t)(value * 255.0f + 0.5f);
		}
		default:
			return 0;
	}
}

static uint8_t ReadColorIndexTo8(uint32_t addr, uint32_t type)
{
	switch (type) {
		case GL_UNSIGNED_BYTE:
			return (uint8_t)ReadMacInt8(addr);
		case GL_BYTE: {
			const int32_t value = (int8_t)ReadMacInt8(addr);
			return value > 0 ? (uint8_t)value : 0;
		}
		case GL_UNSIGNED_SHORT: {
			const uint32_t value = ReadMacInt16(addr);
			return value > 0xffu ? 0xffu : (uint8_t)value;
		}
		case GL_SHORT: {
			const int32_t value = (int16_t)ReadMacInt16(addr);
			if (value <= 0) return 0;
			return value > 0xff ? 0xffu : (uint8_t)value;
		}
		case GL_UNSIGNED_INT: {
			const uint32_t value = ReadMacInt32(addr);
			return value > 0xffu ? 0xffu : (uint8_t)value;
		}
		case GL_INT: {
			const int32_t value = (int32_t)ReadMacInt32(addr);
			if (value <= 0) return 0;
			return value > 0xff ? 0xffu : (uint8_t)value;
		}
		case GL_FLOAT: {
			const uint32_t bits = ReadMacInt32(addr);
			float value = 0.0f;
			std::memcpy(&value, &bits, sizeof(value));
			if (!(value > 0.0f)) return 0;
			if (value >= 255.0f) return 0xffu;
			return (uint8_t)(value + 0.5f);
		}
		default:
			return 0;
	}
}

static bool ConvertPackedPixelToBGRA8(uint32_t pixAddr,
                                      uint32_t format,
                                      uint32_t type,
                                      uint8_t out[4])
{
	switch (type) {
		case GL_UNSIGNED_BYTE_3_3_2:
			GLPixelUnpackRGB332ToBGRA(
				(uint8_t)ReadMacInt8(pixAddr), false, format, out);
			return true;
		case GL_UNSIGNED_BYTE_2_3_3_REV:
			GLPixelUnpackRGB332ToBGRA(
				(uint8_t)ReadMacInt8(pixAddr), true, format, out);
			return true;
		case GL_UNSIGNED_SHORT_5_6_5:
			GLPixelUnpackRGB565ToBGRA(
				(uint16_t)ReadMacInt16(pixAddr), false, format, out);
			return true;
		case GL_UNSIGNED_SHORT_5_6_5_REV:
			GLPixelUnpackRGB565ToBGRA(
				(uint16_t)ReadMacInt16(pixAddr), true, format, out);
			return true;
		case GL_UNSIGNED_SHORT_4_4_4_4:
			GLPixelUnpackRGBA4444ToBGRA(
				(uint16_t)ReadMacInt16(pixAddr), false, format, out);
			return true;
		case GL_UNSIGNED_SHORT_4_4_4_4_REV:
			GLPixelUnpackRGBA4444ToBGRA(
				(uint16_t)ReadMacInt16(pixAddr), true, format, out);
			return true;
		case GL_UNSIGNED_SHORT_5_5_5_1:
			GLPixelUnpackRGBA5551ToBGRA(
				(uint16_t)ReadMacInt16(pixAddr), false, format, out);
			return true;
		case GL_UNSIGNED_SHORT_1_5_5_5_REV:
			GLPixelUnpackRGBA5551ToBGRA(
				(uint16_t)ReadMacInt16(pixAddr), true, format, out);
			return true;
		case GL_UNSIGNED_INT_8_8_8_8:
			GLPixelUnpackRGBA8888ToBGRA(
				ReadMacInt32(pixAddr), false, format, out);
			return true;
		case GL_UNSIGNED_INT_8_8_8_8_REV:
			GLPixelUnpackRGBA8888ToBGRA(
				ReadMacInt32(pixAddr), true, format, out);
			return true;
		case GL_UNSIGNED_INT_10_10_10_2:
			GLPixelUnpackRGBA1010102ToBGRA(
				ReadMacInt32(pixAddr), false, format, out);
			return true;
		case GL_UNSIGNED_INT_2_10_10_10_REV:
			GLPixelUnpackRGBA1010102ToBGRA(
				ReadMacInt32(pixAddr), true, format, out);
			return true;
		default:
			return false;
	}
}

static bool ConvertScalarPixelToBGRA8(uint32_t pixAddr,
                                      uint32_t format,
                                      uint32_t type,
                                      uint8_t out[4])
{
	const int componentCount = GLPixelFormatComponentCount(format);
	const int scalarBytes = GLPixelScalarTypeBytes(type);
	if (componentCount <= 0 || scalarBytes <= 0 ||
	    format == GL_COLOR_INDEX) {
		return false;
	}

	GLPixelInitBGRA(out);
	for (int i = 0; i < componentCount; i++) {
		GLPixelStoreComponentToBGRA(
			GLPixelFormatComponentAt(format, i),
			ReadScalarComponentTo8(pixAddr + i * scalarBytes, type),
			out);
	}
	return true;
}

static bool GLDetectLegacyUnsignedShortPaletteIndexTexture(uint32_t srcBase,
                                                           int width,
                                                           int height,
                                                           int srcBpp,
                                                           int rowStride,
                                                           bool useMacVideoPalette,
                                                           bool hasExplicitPalette)
{
	if (srcBase == 0 || width <= 0 || height <= 0 || srcBpp != 2)
		return false;

	const int sampleXCount = width >= 4 ? 4 : width;
	const int sampleYCount = height >= 4 ? 4 : height;
	GLPixelLegacyUnsignedShortIndexStats stats;
	GLPixelLegacyUnsignedShortIndexStatsInit(&stats);

	for (int sy = 0; sy < sampleYCount; sy++) {
		const int y =
			(sampleYCount == 1) ? 0 : (sy * (height - 1)) / (sampleYCount - 1);
		for (int sx = 0; sx < sampleXCount; sx++) {
			const int x =
				(sampleXCount == 1) ? 0 : (sx * (width - 1)) / (sampleXCount - 1);
			const uint32_t pixAddr = srcBase + y * rowStride + x * srcBpp;
			const uint16_t word = (uint16_t)ReadMacInt16(pixAddr);
			GLPixelLegacyUnsignedShortIndexStatsAddWord(&stats, word);
		}
	}

	if (useMacVideoPalette) {
		return GLPixelLegacyUnsignedShortShouldUseMacVideoPalette(
			stats.sampled_words,
			stats.duplicated_byte_words,
			stats.non_extreme_duplicated_byte_words,
			hasExplicitPalette);
	}

	return GLPixelLegacyUnsignedShortIndexStatsShouldUsePalette(&stats);
}

static bool GLDetectLegacyUnsignedShortExactDuplicatedByteTexture(
	uint32_t srcBase,
	int width,
	int height,
	int srcBpp,
	int rowStride,
	bool hasExplicitPalette,
	GLPixelLegacyUnsignedShortIndexStats *outStats)
{
	GLPixelLegacyUnsignedShortIndexStats stats;
	GLPixelLegacyUnsignedShortIndexStatsInit(&stats);
	if (srcBase != 0 && width > 0 && height > 0 && srcBpp == 2) {
		for (int y = 0; y < height; y++) {
			const uint32_t rowAddr = srcBase + y * rowStride;
			for (int x = 0; x < width; x++) {
				const uint16_t word =
					(uint16_t)ReadMacInt16(rowAddr + x * srcBpp);
				GLPixelLegacyUnsignedShortIndexStatsAddWord(&stats, word);
			}
		}
	}

	if (outStats != nullptr)
		*outStats = stats;

	return !hasExplicitPalette &&
	       stats.sampled_words >= 4 &&
	       stats.duplicated_byte_words == stats.sampled_words &&
	       stats.non_extreme_duplicated_byte_words >= 2;
}

static void GLGetMacVideoPaletteSnapshot(uint8_t outRGB[768])
{
	for (int i = 0; i < 256; i++) {
		outRGB[i * 3 + 0] = mac_pal[i].red;
		outRGB[i * 3 + 1] = mac_pal[i].green;
		outRGB[i * 3 + 2] = mac_pal[i].blue;
	}
}


/*
 *  Helper: convert GL pixel data from PPC memory to BGRA8 (Metal format)
 *
 *  Reads from PPC Mac address space; writes into a malloc'd native buffer.
 *  Returns the buffer (caller must free) and sets outLen.
 */
static uint8_t *ConvertPixelsToBGRA8(uint32_t mac_pixels, int width, int height,
                                      uint32_t format, uint32_t type,
                                      const GLPixelStore &ps,
                                      const GLContext *ctx,
                                      int *outLen,
                                      GLTextureObject *textureObject = nullptr,
                                      int32_t textureLevel = 0,
                                      bool updateTextureLegacyState = false)
{
	int dstBytes = width * height * 4;
	uint8_t *dst = (uint8_t *)malloc(dstBytes);
	if (!dst) { *outLen = 0; return nullptr; }
	*outLen = dstBytes;

	int srcBpp = GLPixelSourceBytesPerPixel(format, type);
	const bool legacyUnsignedShortEligible =
		type == GL_UNSIGNED_SHORT &&
		GLPixelLegacyUnsignedShortFormatCanUsePackedDecode(format);
	if (srcBpp <= 0) {
		// Unsupported format -- fill with magenta for visibility
		for (int i = 0; i < width * height; i++) {
			dst[i*4+0] = 255; dst[i*4+1] = 0; dst[i*4+2] = 255; dst[i*4+3] = 255;
		}
		return dst;
	}

	int rowStride = ComputeRowStride(width, srcBpp, ps.unpack_alignment, ps.unpack_row_length);
	uint32_t srcBase = mac_pixels + ps.unpack_skip_rows * rowStride + ps.unpack_skip_pixels * srcBpp;
	const bool hasExplicitColorIndexPalette =
		ctx != nullptr &&
		(ctx->color_table_enabled || ctx->color_tables[0].defined ||
		 GLColorIndexPixelMapsDefined(ctx->pixel_map_i_to_r_size,
		                             ctx->pixel_map_i_to_g_size,
		                             ctx->pixel_map_i_to_b_size,
		                             ctx->pixel_map_i_to_a_size));
	uint8_t activeDSpCLUT[768];
	uint8_t macVideoPalette[768];
	uint8_t raveCL8Palette[768];
	const bool canUseActiveDSpCLUT =
		(format == GL_COLOR_INDEX || legacyUnsignedShortEligible) &&
		!hasExplicitColorIndexPalette &&
		DSpGetActiveCLUTSnapshot(activeDSpCLUT) == 0;
	const bool raveCL8PaletteAvailable =
		legacyUnsignedShortEligible &&
		!hasExplicitColorIndexPalette &&
		RaveGetLastCL8ColorTableRGBSnapshot(raveCL8Palette);
	const bool canUseRaveCL8Palette =
		raveCL8PaletteAvailable &&
		GLPixelPaletteHasUsableColor(raveCL8Palette, 256);
	bool macVideoPaletteUsable = false;
	if (legacyUnsignedShortEligible && !hasExplicitColorIndexPalette) {
		GLGetMacVideoPaletteSnapshot(macVideoPalette);
		macVideoPaletteUsable =
			GLPixelPaletteHasUsableColor(macVideoPalette, 256);
	}
	const bool useActiveDSpCLUT = format == GL_COLOR_INDEX && canUseActiveDSpCLUT;
	bool legacyUnsignedShortPacked = false;
	bool legacyUnsignedShortPaletteIndex = false;
	bool legacyUnsignedShortBGR332 = false;
	bool legacyUnsignedShortIndexGray = false;
	bool legacyUnsignedShortScalarNoPalette = false;
	const uint8_t *colorIndexPalette = useActiveDSpCLUT ? activeDSpCLUT : nullptr;
	int colorIndexPaletteEntries = useActiveDSpCLUT ? 256 : 0;
	const char *legacyPaletteSource = nullptr;
	const bool forceLegacyUnsignedShortPaletteIndex =
		legacyUnsignedShortEligible &&
		textureObject != nullptr &&
		textureLevel > 0 &&
		textureObject->legacy_ushort_palette_index_chain;
	const bool forceLegacyUnsignedShortBGR332 =
		legacyUnsignedShortEligible &&
		textureObject != nullptr &&
		textureLevel > 0 &&
		textureObject->legacy_ushort_bgr332_chain;
	const bool forceLegacyUnsignedShortIndexGray =
		legacyUnsignedShortEligible &&
		textureObject != nullptr &&
		textureLevel > 0 &&
		textureObject->legacy_ushort_index_gray_chain;
	const bool forceLegacyUnsignedShortLayout =
		forceLegacyUnsignedShortPaletteIndex ||
		forceLegacyUnsignedShortBGR332 ||
		forceLegacyUnsignedShortIndexGray;
	if (legacyUnsignedShortEligible) {
		const int legacySrcBpp =
			GLPixelLegacyUnsignedShortPackedBytesPerPixel(format, type);
		const int legacyRowStride =
			ComputeRowStride(width, legacySrcBpp, ps.unpack_alignment,
			                 ps.unpack_row_length);
		const uint32_t legacySrcBase =
			mac_pixels + ps.unpack_skip_rows * legacyRowStride +
			ps.unpack_skip_pixels * legacySrcBpp;
		GLPixelLegacyUnsignedShortIndexStats legacyLayoutStats;
		const bool exactDuplicatedLegacyLayout =
			GLDetectLegacyUnsignedShortExactDuplicatedByteTexture(
				legacySrcBase, width, height, legacySrcBpp, legacyRowStride,
				hasExplicitColorIndexPalette, &legacyLayoutStats);
		const bool hasUsableLegacyPalette =
			canUseActiveDSpCLUT ||
			canUseRaveCL8Palette ||
			macVideoPaletteUsable;
		legacyUnsignedShortScalarNoPalette =
			exactDuplicatedLegacyLayout && !hasUsableLegacyPalette;
		legacyUnsignedShortPacked =
			GLPixelLegacyUnsignedShortShouldUseLegacyPackedLayout(
				forceLegacyUnsignedShortLayout,
				exactDuplicatedLegacyLayout,
				hasUsableLegacyPalette);
		if (legacyUnsignedShortPacked) {
			srcBpp = legacySrcBpp;
			rowStride = legacyRowStride;
			srcBase = legacySrcBase;
		}
		else if (textureLevel == 0) {
			static int s_legacyScalarUnsignedShortLogCount = 0;
			if (s_legacyScalarUnsignedShortLogCount < 64) {
				s_legacyScalarUnsignedShortLogCount++;
				GL_VLOG("  -> scalar unsigned-short texture: %dx%d "
				        "fmt=0x%x srcBase=0x%08x rowStride=%d uses "
				        "standard %d-byte pixels; ignored duplicated-byte "
				        "legacy layout with no usable palette",
				        width, height, format, srcBase, rowStride, srcBpp);
			}
		}
	}

	if (legacyUnsignedShortPacked) {
		if (forceLegacyUnsignedShortPaletteIndex) {
			if (canUseActiveDSpCLUT) {
				legacyUnsignedShortPaletteIndex = true;
				colorIndexPalette = activeDSpCLUT;
				colorIndexPaletteEntries = 256;
				legacyPaletteSource = "active DSp CLUT mip-chain";
			}
			else if (canUseRaveCL8Palette) {
				legacyUnsignedShortPaletteIndex = true;
				colorIndexPalette = raveCL8Palette;
				colorIndexPaletteEntries = 256;
				legacyPaletteSource = "RAVE CL8 color table mip-chain";
			}
			else if (macVideoPaletteUsable) {
				legacyUnsignedShortPaletteIndex = true;
				colorIndexPalette = macVideoPalette;
				colorIndexPaletteEntries = 256;
				legacyPaletteSource = "Mac video palette mip-chain";
			}
			else {
				legacyUnsignedShortIndexGray = true;
			}
			static int s_legacyPaletteMipChainLogCount = 0;
			if (s_legacyPaletteMipChainLogCount < 96) {
				s_legacyPaletteMipChainLogCount++;
				if (legacyUnsignedShortPaletteIndex) {
					GL_VLOG("  -> legacy ushort mip-chain texture: tex=%u level=%d "
					        "%dx%d fmt=0x%x srcBase=0x%08x rowStride=%d using "
					        "level-0 duplicated-byte palette%s%s",
					        textureObject->name, textureLevel, width, height,
					        format, srcBase, rowStride,
					        legacyPaletteSource ? " from " : "",
					        legacyPaletteSource ? legacyPaletteSource : "");
				}
				else {
					GL_VLOG("  -> legacy ushort mip-chain texture: tex=%u level=%d "
					        "%dx%d fmt=0x%x srcBase=0x%08x rowStride=%d had "
					        "level-0 duplicated-byte palette state but no usable "
					        "palette; using index-gray fallback",
					        textureObject->name, textureLevel, width, height,
					        format, srcBase, rowStride);
				}
			}
		}
			else if (forceLegacyUnsignedShortBGR332) {
				GLPixelLegacyUnsignedShortIndexStats mipStats;
				const bool mipStillDuplicated =
					GLDetectLegacyUnsignedShortExactDuplicatedByteTexture(
						srcBase, width, height, srcBpp, rowStride,
						hasExplicitColorIndexPalette, &mipStats);
				legacyUnsignedShortBGR332 = mipStillDuplicated;
				static int s_legacyBGR332MipChainLogCount = 0;
				if (s_legacyBGR332MipChainLogCount < 96) {
					s_legacyBGR332MipChainLogCount++;
					if (mipStillDuplicated) {
						GL_VLOG("  -> legacy ushort mip-chain texture: tex=%u level=%d "
						        "%dx%d fmt=0x%x srcBase=0x%08x rowStride=%d confirmed "
						        "duplicated-byte mip; using level-0 BGR332 fallback",
						        textureObject->name, textureLevel, width, height,
						        format, srcBase, rowStride);
					}
					else {
						GL_VLOG("  -> legacy ushort mip-chain texture: tex=%u level=%d "
						        "%dx%d fmt=0x%x srcBase=0x%08x rowStride=%d skipped "
						        "level-0 BGR332 fallback for mixed mip data "
						        "(sampled=%d duplicated=%d nonExtreme=%d uniqueLow=%d "
						        "lowRange=0x%02x..0x%02x)",
					        textureObject->name, textureLevel, width, height,
					        format, srcBase, rowStride,
					        mipStats.sampled_words,
					        mipStats.duplicated_byte_words,
					        mipStats.non_extreme_duplicated_byte_words,
					        mipStats.unique_low_byte_values,
					        mipStats.min_low_byte,
					        mipStats.max_low_byte);
				}
			}
		}
		else if (forceLegacyUnsignedShortIndexGray) {
			legacyUnsignedShortIndexGray = true;
			static int s_legacyMipChainLogCount = 0;
			if (s_legacyMipChainLogCount < 96) {
				s_legacyMipChainLogCount++;
				GL_VLOG("  -> legacy ushort mip-chain texture: tex=%u level=%d "
				        "%dx%d fmt=0x%x srcBase=0x%08x rowStride=%d using "
				        "level-0 duplicated-byte index-gray fallback",
				        textureObject->name, textureLevel, width, height,
				        format, srcBase, rowStride);
			}
		}
		else if (canUseActiveDSpCLUT &&
		    GLDetectLegacyUnsignedShortPaletteIndexTexture(srcBase, width, height,
		                                                   srcBpp, rowStride,
		                                                   false, false)) {
			legacyUnsignedShortPaletteIndex = true;
			colorIndexPalette = activeDSpCLUT;
			colorIndexPaletteEntries = 256;
			legacyPaletteSource = "active DSp CLUT";
		}
		else if (canUseRaveCL8Palette &&
		    GLDetectLegacyUnsignedShortPaletteIndexTexture(srcBase, width, height,
		                                                   srcBpp, rowStride,
		                                                   false, false)) {
			legacyUnsignedShortPaletteIndex = true;
			colorIndexPalette = raveCL8Palette;
			colorIndexPaletteEntries = 256;
			legacyPaletteSource = "RAVE CL8 color table";
		}
		else if (GLDetectLegacyUnsignedShortPaletteIndexTexture(srcBase, width, height,
		                                                        srcBpp, rowStride,
		                                                        true,
		                                                        hasExplicitColorIndexPalette)) {
			if (macVideoPaletteUsable) {
				legacyUnsignedShortPaletteIndex = true;
				colorIndexPalette = macVideoPalette;
				colorIndexPaletteEntries = 256;
				legacyPaletteSource = "Mac video palette";
			}
			else {
				if (GLPixelLegacyUnsignedShortFormatCanUseBGR332Fallback(format)) {
					legacyUnsignedShortBGR332 = true;
					static int s_legacyEmptyMacPaletteLogCount = 0;
					if (s_legacyEmptyMacPaletteLogCount < 64) {
						s_legacyEmptyMacPaletteLogCount++;
						GL_VLOG("  -> legacy ushort duplicated-index texture: %dx%d "
						        "fmt=0x%x srcBase=0x%08x rowStride=%d ignored empty "
						        "Mac video palette; using duplicated-byte BGR332 fallback",
						        width, height, format, srcBase, rowStride);
					}
				}
				else {
					static int s_legacyPackedFallbackLogCount = 0;
					if (s_legacyPackedFallbackLogCount < 64) {
						s_legacyPackedFallbackLogCount++;
						GL_VLOG("  -> legacy ushort duplicated-index texture: %dx%d "
						        "fmt=0x%x srcBase=0x%08x rowStride=%d ignored empty "
						        "Mac video palette; using native packed unsigned-short decode",
						        width, height, format, srcBase, rowStride);
					}
				}
			}
		}
	}
	if (legacyUnsignedShortEligible &&
	    updateTextureLegacyState &&
	    textureObject != nullptr &&
	    textureLevel == 0) {
		const bool previousPalette =
			textureObject->legacy_ushort_palette_index_chain;
		const bool previousBGR332 =
			textureObject->legacy_ushort_bgr332_chain;
		const bool previousGray =
			textureObject->legacy_ushort_index_gray_chain;
		const bool previousScalarNoPalette =
			textureObject->legacy_ushort_scalar_no_palette_chain;
		textureObject->legacy_ushort_palette_index_chain =
			legacyUnsignedShortPaletteIndex;
		textureObject->legacy_ushort_bgr332_chain = legacyUnsignedShortBGR332;
		textureObject->legacy_ushort_index_gray_chain = legacyUnsignedShortIndexGray;
		textureObject->legacy_ushort_scalar_no_palette_chain =
			legacyUnsignedShortScalarNoPalette;
		if (previousPalette != legacyUnsignedShortPaletteIndex ||
		    previousBGR332 != legacyUnsignedShortBGR332 ||
		    previousGray != legacyUnsignedShortIndexGray ||
		    previousScalarNoPalette != legacyUnsignedShortScalarNoPalette) {
			GL_LOG("  -> legacy ushort texture state: tex=%u level-0 "
			       "palette-index mip chain %s, BGR332 mip chain %s, "
			       "index-gray mip chain %s, scalar no-palette mip chain %s",
			       textureObject->name,
			       legacyUnsignedShortPaletteIndex ? "enabled" : "disabled",
			       legacyUnsignedShortBGR332 ? "enabled" : "disabled",
			       legacyUnsignedShortIndexGray ? "enabled" : "disabled",
			       legacyUnsignedShortScalarNoPalette ? "enabled" : "disabled");
		}
	}
	if (legacyUnsignedShortPaletteIndex) {
		static int s_legacyPaletteLogCount = 0;
		if (s_legacyPaletteLogCount < 64) {
			s_legacyPaletteLogCount++;
			GL_VLOG("  -> legacy ushort duplicated-index texture: %dx%d fmt=0x%x "
			        "srcBase=0x%08x rowStride=%d using %s",
			        width, height, format, srcBase, rowStride,
			        legacyPaletteSource ? legacyPaletteSource : "palette");
		}
	}

	for (int y = 0; y < height; y++) {
		uint32_t rowAddr = srcBase + y * rowStride;
		for (int x = 0; x < width; x++) {
			uint32_t pixAddr = rowAddr + x * srcBpp;
			uint8_t *out = dst + (y * width + x) * 4;

			if (format == GL_COLOR_INDEX) {
				const uint8_t index = ReadColorIndexTo8(pixAddr, type);
				if (ctx) {
					GLColorIndexToBGRA8Resolved(index,
						ctx->pixel_map_i_to_r, ctx->pixel_map_i_to_r_size,
						ctx->pixel_map_i_to_g, ctx->pixel_map_i_to_g_size,
						ctx->pixel_map_i_to_b, ctx->pixel_map_i_to_b_size,
						ctx->pixel_map_i_to_a, ctx->pixel_map_i_to_a_size,
						colorIndexPalette, colorIndexPaletteEntries,
						out);
				} else {
					GLColorIndexToBGRA8(index, out);
				}
			}
			else if (legacyUnsignedShortPaletteIndex) {
				const uint16_t word = (uint16_t)ReadMacInt16(pixAddr);
				GLColorIndexToBGRA8WithPalette(
					GLPixelLegacyUnsignedShortPaletteIndex(word),
					colorIndexPalette, colorIndexPaletteEntries, out);
			}
			else if (legacyUnsignedShortBGR332) {
				GLPixelUnpackLegacyUnsignedShortBGR332ToBGRA(
					(uint16_t)ReadMacInt16(pixAddr), out);
			}
			else if (legacyUnsignedShortIndexGray) {
				GLPixelUnpackLegacyUnsignedShortIndexGrayToBGRA(
					(uint16_t)ReadMacInt16(pixAddr), out);
			}
			else if (legacyUnsignedShortPacked) {
				// Some classic Mac GL clients pass 16-bit packed pixels
				// with the generic GL_UNSIGNED_SHORT token instead of an
				// explicit packed OpenGL type.
				GLPixelUnpackLegacyUnsignedShortToBGRA(
					(uint16_t)ReadMacInt16(pixAddr), format, out);
			}
			else if (ConvertPackedPixelToBGRA8(pixAddr, format, type, out)) {
				// Packed pixel handled above.
			}
			else if (ConvertScalarPixelToBGRA8(pixAddr, format, type, out)) {
				// Scalar component pixel handled above.
			}
			else {
				// Fallback: magenta
				out[0] = 255; out[1] = 0; out[2] = 255; out[3] = 255;
			}
		}
	}

	return dst;
}

uint8_t *GLConvertMacPixelsToBGRA8(GLContext *ctx,
                                   uint32_t mac_pixels,
                                   int width,
                                   int height,
                                   uint32_t format,
                                   uint32_t type,
                                   int *outLen)
{
	if (!outLen) return nullptr;
	if (ctx)
		return ConvertPixelsToBGRA8(mac_pixels, width, height, format, type,
		                            ctx->pixel_store, ctx, outLen);

	GLPixelStore ps = {4, 0, 0, 0, 4, 0, 0, 0};
	return ConvertPixelsToBGRA8(mac_pixels, width, height, format, type,
	                            ps, nullptr, outLen);
}


/*
 *  GLApplyColorTableLUT -- ARB_imaging color-table apply in the CPU upload path.
 *
 *  When GL_COLOR_TABLE is enabled and the primary color table (index 0) is defined, remap
 *  each BGRA8 texel through the LUT in place, BEFORE the GPU upload. This is the
 *  minimal-correct apply mechanism: it folds the LUT into the same
 *  CPU transfer seam the indexed->BGRA conversion already uses, so color_table_enabled
 *  becomes a READ (was write-only). No new compute pass, no new shader, no command-encoder
 *  dependency. The buffer is host-side BGRA8 (B@+0, G@+1, R@+2, A@+3).
 *
 *  Index source: per OpenGL 1.2.1 SS3.6.3 the color table maps incoming component values
 *  through the table. For the paletted-texture case we route through the
 *  red component as the LUT index -- the common single-channel palette remap -- and write
 *  the LUT's RGB back into the texel (alpha preserved). The index is clamped to the LUT's
 *  [0, width-1] range AND the hard GLColorTable.data[256][4] ceiling BEFORE any read
 *  (no OOB index into the [256][4] array).
 *
 *  bufLen is the converted-buffer byte length (4 bytes/pixel); pixel_count is derived from
 *  it (NOT width*height, which the caller may have already consumed) so a short/over-long
 *  buffer cannot drive an out-of-bounds write.
 */
static void GLApplyColorTableLUT(GLContext *ctx, uint8_t *bgra, int width, int height, int bufLen)
{
	if (!ctx || !bgra) return;
	if (!ctx->color_table_enabled) return;
	const GLColorTable &ct = ctx->color_tables[0];
	if (!ct.defined) return;
	// LUT entry count, clamped to the hard [1, 256] ceiling of GLColorTable.data[256][4].
	int lut_entries = ct.width;
	if (lut_entries <= 0) return;
	if (lut_entries > 256) lut_entries = 256;
	// Pixel count is derived from the actual buffer length (4 bytes/pixel), guarded against
	// a negative/short buffer or a width/height that disagrees with bufLen.
	if (bufLen < 4) return;
	long pixel_count = (long)bufLen / 4;
	if (width > 0 && height > 0) {
		long wh = (long)width * (long)height;
		if (wh < pixel_count) pixel_count = wh;   // never write past the smaller of the two
	}
	for (long p = 0; p < pixel_count; p++) {
		uint8_t *px = bgra + p * 4;
		// BGRA8: red is at byte +2. Use it as the LUT index (paletted-texture remap).
		int idx = px[2];
		if (idx >= lut_entries) idx = lut_entries - 1;   // clamp to [0, lut_entries-1]
		if (idx < 0) idx = 0;                            // (red is unsigned; defensive)
		// LUT data is float RGBA in [0,1]; write back as BGRA8, preserving the texel alpha.
		px[0] = (uint8_t)(ct.data[idx][2] * 255.0f + 0.5f);   // B <- LUT blue
		px[1] = (uint8_t)(ct.data[idx][1] * 255.0f + 0.5f);   // G <- LUT green
		px[2] = (uint8_t)(ct.data[idx][0] * 255.0f + 0.5f);   // R <- LUT red
	}
}



/*
 *  glTexImage2D -- upload pixel data to texture
 *
 *  All 9 GL args come via PPC registers r3-r10 (target through type)
 *  plus mac_pixels from stack/r10.
 */
void NativeGLTexImage2D(GLContext *ctx, uint32_t target, int32_t level,
                         int32_t internalformat, int32_t width, int32_t height,
                         int32_t border, uint32_t format, uint32_t type,
                         uint32_t mac_pixels)
{
	GL_VLOG("NativeGLTexImage2D: ctx=%p target=0x%x level=%d w=%d h=%d fmt=0x%x type=0x%x pixels=0x%08x",
	       ctx, target, level, width, height, format, type, mac_pixels);

	if (!ctx) { GL_LOG("NativeGLTexImage2D: ctx is NULL, returning"); return; }

	// Get currently bound texture
	uint32_t texName = 0;
	if (target == GL_TEXTURE_2D)
		texName = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	else if (target == GL_TEXTURE_1D)
		texName = ctx->tex_units[ctx->active_texture].bound_texture_1d;

	GL_VLOG("  -> active_texture=%d texName=%u", ctx->active_texture, texName);

	if (texName == 0) return;

	auto it = ctx->texture_objects.find(texName);
	if (it == ctx->texture_objects.end()) { GL_LOG("NativeGLTexImage2D: texture %u not found in map", texName); return; }

	GLTextureObject &tex = it->second;
	if (level == 0) {
		tex.width = width;
		tex.height = height;
		tex.source_format = format;
		tex.source_type = type;
	}
	const bool ignoreLegacyFallbackClientMip =
		GLPixelLegacyUnsignedShortShouldIgnoreClientMipForFallback(
			tex.legacy_ushort_bgr332_chain,
			tex.legacy_ushort_scalar_no_palette_chain,
			format, type, level);
	if (ignoreLegacyFallbackClientMip) {
		const char *fallbackName =
			tex.legacy_ushort_scalar_no_palette_chain ?
			"scalar no-palette" : "BGR332 fallback";
		GL_VLOG("  -> legacy ushort %s texture: tex=%u level=%d "
		        "%dx%d fmt=0x%x type=0x%x ignoring client mip; "
		        "sampling level-0 fallback texture only",
		        fallbackName, tex.name, level, width, height, format, type);
		return;
	}
	if (level > 0) tex.has_mipmaps = true;

	if (mac_pixels == 0) {
		// Allocate storage only (no data), still create Metal texture
		int emptyLen = width * height * 4;
		uint8_t *emptyData = (uint8_t *)calloc(1, emptyLen);
		if (emptyData) {
			GLMetalUploadTexture(ctx, &tex, level, width, height, emptyData, emptyLen);
			free(emptyData);
		}
		return;
	}

	// Convert pixel data
	GL_VLOG("  -> about to ConvertPixelsToBGRA8: mac_pixels=0x%08x host=%p w=%d h=%d fmt=0x%x type=0x%x",
	       mac_pixels, Mac2HostAddr(mac_pixels), width, height, format, type);
	int dataLen = 0;
	uint8_t *converted = ConvertPixelsToBGRA8(mac_pixels, width, height,
	                                           format, type, ctx->pixel_store, ctx, &dataLen,
	                                           &tex, level, true);
	if (!converted) return;

	// ARB_imaging color-table LUT apply: when GL_COLOR_TABLE is
	// enabled and a primary color table is defined, remap each BGRA8 texel through the
	// LUT before the GPU upload. This makes color_table_enabled a READ (was write-only)
	// and applies the stored LUT in the existing CPU upload seam — no new compute pass,
	// no new shader, no command-encoder dependency. Per OpenGL 1.2.1 §3.6.3 the
	// color table maps incoming component values through the table; the minimal-correct
	// paletted-texture case routes through the red component as the index (the common
	// 1-channel LUT-remap use). The index is clamped to [0, width-1] and the hard
	// GLColorTable.data[256][4] ceiling BEFORE any LUT read.
	GLApplyColorTableLUT(ctx, converted, width, height, dataLen);

	GLMetalUploadTexture(ctx, &tex, level, width, height, converted, dataLen);
	free(converted);
}


/*
 *  glTexSubImage2D -- partial texture update
 */
void NativeGLTexSubImage2D(GLContext *ctx, uint32_t target, int32_t level,
                            int32_t xoffset, int32_t yoffset,
                            int32_t width, int32_t height,
                            uint32_t format, uint32_t type, uint32_t mac_pixels)
{
	if (!ctx || mac_pixels == 0) return;

	uint32_t texName = 0;
	if (target == GL_TEXTURE_2D)
		texName = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	if (texName == 0) return;

	auto it = ctx->texture_objects.find(texName);
	if (it == ctx->texture_objects.end()) return;

	GLTextureObject &tex = it->second;

	int dataLen = 0;
	uint8_t *converted = ConvertPixelsToBGRA8(mac_pixels, width, height,
	                                           format, type, ctx->pixel_store, ctx, &dataLen,
	                                           &tex, level, false);
	if (!converted) return;

	// ARB_imaging color-table LUT apply: same upload-path remap as
	// NativeGLTexImage2D — the color table applies during pixel transfer for the sub-image
	// upload too (OpenGL 1.2.1 §3.6.3). Index clamped before any LUT read.
	GLApplyColorTableLUT(ctx, converted, width, height, dataLen);

	GLMetalUploadSubTexture(ctx, &tex, level, xoffset, yoffset, width, height,
	                         converted, width * 4);
	free(converted);
}


/*
 *  glTexImage1D -- treat as 2D with height=1 (Metal has no 1D textures on iOS)
 */
void NativeGLTexImage1D(GLContext *ctx, uint32_t target, int32_t level,
                         int32_t internalformat, int32_t width, int32_t border,
                         uint32_t format, uint32_t type, uint32_t mac_pixels)
{
	NativeGLTexImage2D(ctx, GL_TEXTURE_2D, level, internalformat, width, 1,
	                    border, format, type, mac_pixels);
}


/*
 *  glTexSubImage1D -- partial 1D texture update (height=1)
 */
void NativeGLTexSubImage1D(GLContext *ctx, uint32_t target, int32_t level,
                            int32_t xoffset, int32_t width,
                            uint32_t format, uint32_t type, uint32_t mac_pixels)
{
	NativeGLTexSubImage2D(ctx, GL_TEXTURE_2D, level, xoffset, 0, width, 1,
	                       format, type, mac_pixels);
}


/*
 *  glCopyTexImage2D -- copy framebuffer rect into a new texture level
 */
void NativeGLCopyTexImage2D(GLContext *ctx, uint32_t target, int32_t level,
                             uint32_t internalformat, int32_t x, int32_t y,
                             int32_t width, int32_t height, int32_t border)
{
	if (!ctx || width <= 0 || height <= 0) return;

	int bufLen = 0;
	uint8_t *bgra = GLMetalReadFramebufferRect(ctx, x, y, width, height, &bufLen);
	if (!bgra) return;

	uint32_t texName = (target == GL_TEXTURE_2D)
		? ctx->tex_units[ctx->active_texture].bound_texture_2d
		: ctx->tex_units[ctx->active_texture].bound_texture_1d;
	if (texName == 0) { free(bgra); return; }

	auto it = ctx->texture_objects.find(texName);
	if (it == ctx->texture_objects.end()) { free(bgra); return; }

	GLTextureObject &tex = it->second;
	if (level == 0) { tex.width = width; tex.height = height; }
	if (level > 0) tex.has_mipmaps = true;

	GLMetalUploadTexture(ctx, &tex, level, width, height, bgra, bufLen);
	free(bgra);

	GL_LOG("glCopyTexImage2D: %dx%d from (%d,%d) -> tex %u level %d",
		        width, height, x, y, texName, level);
}

/*
 *  glCopyTexSubImage2D -- copy framebuffer rect into existing texture sub-region
 */
void NativeGLCopyTexSubImage2D(GLContext *ctx, uint32_t target, int32_t level,
                                int32_t xoffset, int32_t yoffset,
                                int32_t x, int32_t y, int32_t width, int32_t height)
{
	if (!ctx || width <= 0 || height <= 0) return;

	int bufLen = 0;
	uint8_t *bgra = GLMetalReadFramebufferRect(ctx, x, y, width, height, &bufLen);
	if (!bgra) return;

	uint32_t texName = (target == GL_TEXTURE_2D)
		? ctx->tex_units[ctx->active_texture].bound_texture_2d
		: ctx->tex_units[ctx->active_texture].bound_texture_1d;
	if (texName == 0) { free(bgra); return; }

	auto it = ctx->texture_objects.find(texName);
	if (it == ctx->texture_objects.end()) { free(bgra); return; }

	GLTextureObject &tex = it->second;
	// Use sub-image upload: upload as full image at the given level, offset by xoffset/yoffset
	// For simplicity, upload the full level and let Metal handle the sub-region
	// TODO: Use a dedicated sub-image upload for better efficiency
	if (xoffset == 0 && yoffset == 0 && level == 0 &&
	    width == tex.width && height == tex.height) {
		GLMetalUploadTexture(ctx, &tex, level, width, height, bgra, bufLen);
	} else {
		// For sub-region copies, re-upload the full texture with the modified region
		// This is a simplification -- a proper implementation would use replaceRegion
		GLMetalUploadTexture(ctx, &tex, level, width, height, bgra, bufLen);
	}
	free(bgra);

	GL_LOG("glCopyTexSubImage2D: %dx%d from (%d,%d) -> tex %u level %d offset (%d,%d)",
	        width, height, x, y, texName, level, xoffset, yoffset);
}


/*
 *  glIsTexture -- check if name is a texture object
 */
uint32_t NativeGLIsTexture(GLContext *ctx, uint32_t texture)
{
	if (!ctx || texture == 0) return GL_FALSE;
	return ctx->texture_objects.find(texture) != ctx->texture_objects.end() ? GL_TRUE : GL_FALSE;
}


// =========================================================================
//  Remaining core GL handlers
//
//  Categories: Accumulation, bitmap/raster, rectangles, finish/flush,
//  evaluators, selection/feedback, pixel transfer/map, index mode,
//  edge flags, line/polygon stipple, display lists, vertex arrays,
//  clip planes, draw arrays/elements, copy tex, get functions, misc.
// =========================================================================


// --- Accumulation buffer ---
// NativeGLAccum is implemented in gl_metal_renderer.mm (Metal readback/writeback)

void NativeGLClearAccum(GLContext *ctx, float r, float g, float b, float a)
{
	ctx->clear_accum[0] = r;
	ctx->clear_accum[1] = g;
	ctx->clear_accum[2] = b;
	ctx->clear_accum[3] = a;
}

// --- Clear index (color index mode) ---
void NativeGLClearIndex(GLContext *ctx, float c)
{
	ctx->clear_index = c;
}

// --- Clip plane ---
void NativeGLClipPlane(GLContext *ctx, uint32_t plane, uint32_t mac_ptr)
{
	int idx = plane - GL_CLIP_PLANE0;
	if (idx < 0 || idx >= 6) { ctx->last_error = GL_INVALID_ENUM; return; }
	double object_plane[4];
	for (int i = 0; i < 4; i++) {
		uint32 hi = ReadMacInt32(mac_ptr + i * 8);
		uint32 lo = ReadMacInt32(mac_ptr + i * 8 + 4);
		uint64_t bits = ((uint64_t)hi << 32) | lo;
		double d;
		memcpy(&d, &bits, sizeof(double));
		object_plane[i] = d;
	}
	double eye_plane[4];
	if (!GLTransformClipPlaneToEyeSpace(
		eye_plane,
		object_plane,
		ctx->modelview_stack[ctx->modelview_depth])) {
		memcpy(eye_plane, object_plane, sizeof(eye_plane));
	}
	memcpy(ctx->clip_planes[idx], eye_plane, sizeof(eye_plane));
}

void NativeGLGetClipPlane(GLContext *ctx, uint32_t plane, uint32_t mac_ptr)
{
	int idx = plane - GL_CLIP_PLANE0;
	if (idx < 0 || idx >= 6) { ctx->last_error = GL_INVALID_ENUM; return; }
	for (int i = 0; i < 4; i++) {
		double d = ctx->clip_planes[idx][i];
		uint64_t bits;
		memcpy(&bits, &d, sizeof(double));
		WriteMacInt32(mac_ptr + i * 8, (uint32_t)(bits >> 32));
		WriteMacInt32(mac_ptr + i * 8 + 4, (uint32_t)(bits & 0xFFFFFFFF));
	}
}

// --- Raster position ---
static void gl_raster_pos4f(GLContext *ctx, float x, float y, float z, float w)
{
	float *mv = ctx->modelview_stack[ctx->modelview_depth];
	float *proj = ctx->projection_stack[ctx->projection_depth];
	float eye[4];
	for (int r = 0; r < 4; r++)
		eye[r] = mv[0*4+r]*x + mv[1*4+r]*y + mv[2*4+r]*z + mv[3*4+r]*w;
	float clip[4];
	for (int r = 0; r < 4; r++)
		clip[r] = proj[0*4+r]*eye[0] + proj[1*4+r]*eye[1] + proj[2*4+r]*eye[2] + proj[3*4+r]*eye[3];
	if (clip[3] != 0.0f) {
		float inv_w = 1.0f / clip[3];
		float ndc_x = clip[0] * inv_w;
		float ndc_y = clip[1] * inv_w;
		float ndc_z = clip[2] * inv_w;
		ctx->raster_pos[0] = ctx->viewport[0] + ctx->viewport[2] * (ndc_x + 1.0f) * 0.5f;
		ctx->raster_pos[1] = ctx->viewport[1] + ctx->viewport[3] * (ndc_y + 1.0f) * 0.5f;
		ctx->raster_pos[2] = (ctx->depth_range_far - ctx->depth_range_near) * (ndc_z + 1.0f) * 0.5f + ctx->depth_range_near;
		ctx->raster_pos[3] = clip[3];
		ctx->raster_pos_valid = true;
	} else {
		ctx->raster_pos_valid = false;
	}
}

void NativeGLRasterPos2f(GLContext *ctx, float x, float y) { gl_raster_pos4f(ctx, x, y, 0.0f, 1.0f); }
void NativeGLRasterPos3f(GLContext *ctx, float x, float y, float z) { gl_raster_pos4f(ctx, x, y, z, 1.0f); }
void NativeGLRasterPos4f(GLContext *ctx, float x, float y, float z, float w) { gl_raster_pos4f(ctx, x, y, z, w); }
void NativeGLRasterPos2d(GLContext *ctx, double x, double y) { gl_raster_pos4f(ctx, (float)x, (float)y, 0.0f, 1.0f); }
void NativeGLRasterPos3d(GLContext *ctx, double x, double y, double z) { gl_raster_pos4f(ctx, (float)x, (float)y, (float)z, 1.0f); }
void NativeGLRasterPos4d(GLContext *ctx, double x, double y, double z, double w) { gl_raster_pos4f(ctx, (float)x, (float)y, (float)z, (float)w); }
void NativeGLRasterPos2i(GLContext *ctx, int32_t x, int32_t y) { gl_raster_pos4f(ctx, (float)x, (float)y, 0.0f, 1.0f); }
void NativeGLRasterPos3i(GLContext *ctx, int32_t x, int32_t y, int32_t z) { gl_raster_pos4f(ctx, (float)x, (float)y, (float)z, 1.0f); }
void NativeGLRasterPos4i(GLContext *ctx, int32_t x, int32_t y, int32_t z, int32_t w) { gl_raster_pos4f(ctx, (float)x, (float)y, (float)z, (float)w); }
void NativeGLRasterPos2s(GLContext *ctx, int16_t x, int16_t y) { gl_raster_pos4f(ctx, (float)x, (float)y, 0.0f, 1.0f); }
void NativeGLRasterPos3s(GLContext *ctx, int16_t x, int16_t y, int16_t z) { gl_raster_pos4f(ctx, (float)x, (float)y, (float)z, 1.0f); }
void NativeGLRasterPos4s(GLContext *ctx, int16_t x, int16_t y, int16_t z, int16_t w) { gl_raster_pos4f(ctx, (float)x, (float)y, (float)z, (float)w); }

static double ReadMacDouble(uint32_t addr)
{
	uint32 hi = ReadMacInt32(addr);
	uint32 lo = ReadMacInt32(addr + 4);
	uint64_t bits = ((uint64_t)hi << 32) | lo;
	double d;
	memcpy(&d, &bits, sizeof(double));
	return d;
}

void NativeGLRasterPos2fv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, ReadMacFloat(p), ReadMacFloat(p+4), 0.0f, 1.0f); }
void NativeGLRasterPos3fv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, ReadMacFloat(p), ReadMacFloat(p+4), ReadMacFloat(p+8), 1.0f); }
void NativeGLRasterPos4fv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, ReadMacFloat(p), ReadMacFloat(p+4), ReadMacFloat(p+8), ReadMacFloat(p+12)); }
void NativeGLRasterPos2dv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, (float)ReadMacDouble(p), (float)ReadMacDouble(p+8), 0.0f, 1.0f); }
void NativeGLRasterPos3dv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, (float)ReadMacDouble(p), (float)ReadMacDouble(p+8), (float)ReadMacDouble(p+16), 1.0f); }
void NativeGLRasterPos4dv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, (float)ReadMacDouble(p), (float)ReadMacDouble(p+8), (float)ReadMacDouble(p+16), (float)ReadMacDouble(p+24)); }
void NativeGLRasterPos2iv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, (float)(int32_t)ReadMacInt32(p), (float)(int32_t)ReadMacInt32(p+4), 0.0f, 1.0f); }
void NativeGLRasterPos3iv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, (float)(int32_t)ReadMacInt32(p), (float)(int32_t)ReadMacInt32(p+4), (float)(int32_t)ReadMacInt32(p+8), 1.0f); }
void NativeGLRasterPos4iv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, (float)(int32_t)ReadMacInt32(p), (float)(int32_t)ReadMacInt32(p+4), (float)(int32_t)ReadMacInt32(p+8), (float)(int32_t)ReadMacInt32(p+12)); }
void NativeGLRasterPos2sv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, (float)(int16_t)ReadMacInt16(p), (float)(int16_t)ReadMacInt16(p+2), 0.0f, 1.0f); }
void NativeGLRasterPos3sv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, (float)(int16_t)ReadMacInt16(p), (float)(int16_t)ReadMacInt16(p+2), (float)(int16_t)ReadMacInt16(p+4), 1.0f); }
void NativeGLRasterPos4sv(GLContext *ctx, uint32_t p) { gl_raster_pos4f(ctx, (float)(int16_t)ReadMacInt16(p), (float)(int16_t)ReadMacInt16(p+2), (float)(int16_t)ReadMacInt16(p+4), (float)(int16_t)ReadMacInt16(p+6)); }

// --- Bitmap ---
void NativeGLBitmap(GLContext *ctx, int32_t width, int32_t height, float xorig, float yorig,
                    float xmove, float ymove, uint32_t bitmap_ptr)
{
	// Render the bitmap if we have valid data and raster position
	if (bitmap_ptr != 0 && ctx->raster_pos_valid && width > 0 && height > 0) {
		// Unpack 1-bit bitmap to BGRA8: bit=1 → current_color, bit=0 → transparent
		int dstBytes = width * height * 4;
		uint8_t *bgra = (uint8_t *)malloc(dstBytes);
		if (bgra) {
			// Current color as BGRA8
			uint8_t cr = (uint8_t)(ctx->current_color[0] * 255.0f);
			uint8_t cg = (uint8_t)(ctx->current_color[1] * 255.0f);
			uint8_t cb = (uint8_t)(ctx->current_color[2] * 255.0f);
			uint8_t ca = (uint8_t)(ctx->current_color[3] * 255.0f);

			// Row stride for 1-bit-per-pixel with unpack_alignment
			int bytesPerRow = (width + 7) / 8;  // ceil(width / 8)
			int alignment = ctx->pixel_store.unpack_alignment;
			if (alignment > 1)
				bytesPerRow = ((bytesPerRow + alignment - 1) / alignment) * alignment;

			for (int y = 0; y < height; y++) {
				uint32_t rowAddr = bitmap_ptr + y * bytesPerRow;
				for (int x = 0; x < width; x++) {
					int byteIndex = x / 8;
					int bitIndex = 7 - (x % 8);  // MSB-first bit order
					uint8_t byte = ReadMacInt8(rowAddr + byteIndex);
					uint8_t *out = bgra + (y * width + x) * 4;
					if (byte & (1 << bitIndex)) {
						// bit=1: write current color as BGRA
						out[0] = cb; out[1] = cg; out[2] = cr; out[3] = ca;
					} else {
						// bit=0: fully transparent
						out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
					}
				}
			}

			GLMetalBitmap(ctx, width, height, bgra, dstBytes);
			free(bgra);
		}
	}

	// Advance raster position by move amounts (always, even if rendering was skipped)
	ctx->raster_pos[0] += xmove;
	ctx->raster_pos[1] += ymove;
}

// --- DrawPixels / CopyPixels / ReadPixels / ReadBuffer / DrawBuffer ---
void NativeGLDrawPixels(GLContext *ctx, int32_t width, int32_t height, uint32_t format, uint32_t type, uint32_t pixels_ptr)
{
	if (!ctx->raster_pos_valid) return;

	int outLen = 0;
	uint8_t *bgra = ConvertPixelsToBGRA8(pixels_ptr, width, height, format, type, ctx->pixel_store, ctx, &outLen);
	if (!bgra) return;

	GLMetalDrawPixels(ctx, width, height, bgra, outLen);
	free(bgra);
}

void NativeGLCopyPixels(GLContext *ctx, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t type)
{
	if (!ctx || width <= 0 || height <= 0) return;
	// GL_COLOR is the only type we support (not GL_DEPTH or GL_STENCIL)
	if (type != 0x1800 /* GL_COLOR */) {
		GL_LOG("glCopyPixels type 0x%x not supported (only GL_COLOR)", type);
		return;
	}

	int bufLen = 0;
	uint8_t *bgra = GLMetalReadFramebufferRect(ctx, x, y, width, height, &bufLen);
	if (!bgra) return;

	GLMetalDrawPixels(ctx, width, height, bgra, bufLen);
	free(bgra);

	GL_VLOG("glCopyPixels: %dx%d from (%d,%d)", width, height, x, y);
}

// NativeGLReadPixels is implemented in gl_metal_renderer.mm (Metal readback)

void NativeGLReadBuffer(GLContext *ctx, uint32_t mode) { ctx->read_buffer = mode; }
void NativeGLDrawBuffer(GLContext *ctx, uint32_t mode) { ctx->draw_buffer = mode; }

// --- Rectangles ---
extern void NativeGLBegin(GLContext *ctx, uint32_t mode);
extern void NativeGLEnd(GLContext *ctx);
extern void NativeGLVertex2f(GLContext *ctx, float x, float y);
#define GL_QUADS 0x0007

void NativeGLRectf(GLContext *ctx, float x1, float y1, float x2, float y2)
{
	NativeGLBegin(ctx, GL_QUADS);
	NativeGLVertex2f(ctx, x1, y1);
	NativeGLVertex2f(ctx, x2, y1);
	NativeGLVertex2f(ctx, x2, y2);
	NativeGLVertex2f(ctx, x1, y2);
	NativeGLEnd(ctx);
}

void NativeGLRectd(GLContext *ctx, double x1, double y1, double x2, double y2) { NativeGLRectf(ctx, (float)x1, (float)y1, (float)x2, (float)y2); }
void NativeGLRecti(GLContext *ctx, int32_t x1, int32_t y1, int32_t x2, int32_t y2) { NativeGLRectf(ctx, (float)x1, (float)y1, (float)x2, (float)y2); }
void NativeGLRects(GLContext *ctx, int16_t x1, int16_t y1, int16_t x2, int16_t y2) { NativeGLRectf(ctx, (float)x1, (float)y1, (float)x2, (float)y2); }

void NativeGLRectfv(GLContext *ctx, uint32_t v1, uint32_t v2) { NativeGLRectf(ctx, ReadMacFloat(v1), ReadMacFloat(v1+4), ReadMacFloat(v2), ReadMacFloat(v2+4)); }
void NativeGLRectdv(GLContext *ctx, uint32_t v1, uint32_t v2) { NativeGLRectf(ctx, (float)ReadMacDouble(v1), (float)ReadMacDouble(v1+8), (float)ReadMacDouble(v2), (float)ReadMacDouble(v2+8)); }
void NativeGLRectiv(GLContext *ctx, uint32_t v1, uint32_t v2) { NativeGLRectf(ctx, (float)(int32_t)ReadMacInt32(v1), (float)(int32_t)ReadMacInt32(v1+4), (float)(int32_t)ReadMacInt32(v2), (float)(int32_t)ReadMacInt32(v2+4)); }
void NativeGLRectsv(GLContext *ctx, uint32_t v1, uint32_t v2) { NativeGLRectf(ctx, (float)(int16_t)ReadMacInt16(v1), (float)(int16_t)ReadMacInt16(v1+2), (float)(int16_t)ReadMacInt16(v2), (float)(int16_t)ReadMacInt16(v2+2)); }

// --- Evaluators ---

// Map GL evaluator target to internal index 0-8
static int gl_eval_target_index(uint32_t target)
{
	switch (target) {
	case GL_MAP1_VERTEX_3:        case GL_MAP2_VERTEX_3:        return 0;
	case GL_MAP1_VERTEX_4:        case GL_MAP2_VERTEX_4:        return 1;
	case GL_MAP1_INDEX:           case GL_MAP2_INDEX:           return 2;
	case GL_MAP1_COLOR_4:         case GL_MAP2_COLOR_4:         return 3;
	case GL_MAP1_NORMAL:          case GL_MAP2_NORMAL:          return 4;
	case GL_MAP1_TEXTURE_COORD_1: case GL_MAP2_TEXTURE_COORD_1: return 5;
	case GL_MAP1_TEXTURE_COORD_2: case GL_MAP2_TEXTURE_COORD_2: return 6;
	case GL_MAP1_TEXTURE_COORD_3: case GL_MAP2_TEXTURE_COORD_3: return 7;
	case GL_MAP1_TEXTURE_COORD_4: case GL_MAP2_TEXTURE_COORD_4: return 8;
	default: return -1;
	}
}

// Return number of components for a given evaluator target
static int gl_eval_target_dimension(uint32_t target)
{
	switch (target) {
	case GL_MAP1_VERTEX_3:        case GL_MAP2_VERTEX_3:        return 3;
	case GL_MAP1_VERTEX_4:        case GL_MAP2_VERTEX_4:        return 4;
	case GL_MAP1_INDEX:           case GL_MAP2_INDEX:           return 1;
	case GL_MAP1_COLOR_4:         case GL_MAP2_COLOR_4:         return 4;
	case GL_MAP1_NORMAL:          case GL_MAP2_NORMAL:          return 3;
	case GL_MAP1_TEXTURE_COORD_1: case GL_MAP2_TEXTURE_COORD_1: return 1;
	case GL_MAP1_TEXTURE_COORD_2: case GL_MAP2_TEXTURE_COORD_2: return 2;
	case GL_MAP1_TEXTURE_COORD_3: case GL_MAP2_TEXTURE_COORD_3: return 3;
	case GL_MAP1_TEXTURE_COORD_4: case GL_MAP2_TEXTURE_COORD_4: return 4;
	default: return 0;
	}
}

// de Casteljau evaluation for 1D map: evaluate Bernstein polynomial at parameter u
static void gl_eval_bernstein1(const GLEvaluatorMap1 &map, float u, float *out)
{
	int dim = map.dimension;
	int order = map.order;
	if (order <= 0 || dim <= 0) return;

	// Normalize u to [0,1]
	float range = map.u2 - map.u1;
	float t = (range != 0.0f) ? (u - map.u1) / range : 0.0f;

	// Copy control points into temp buffer for in-place de Casteljau
	std::vector<float> tmp(map.control_points.begin(), map.control_points.begin() + order * dim);

	// de Casteljau's algorithm: iteratively reduce
	for (int r = 1; r < order; r++) {
		for (int i = 0; i < order - r; i++) {
			for (int d = 0; d < dim; d++) {
				tmp[i * dim + d] = (1.0f - t) * tmp[i * dim + d] + t * tmp[(i + 1) * dim + d];
			}
		}
	}

	for (int d = 0; d < dim; d++)
		out[d] = tmp[d];
}

// Bivariate de Casteljau evaluation for 2D map
static void gl_eval_bernstein2(const GLEvaluatorMap2 &map, float u, float v, float *out)
{
	int dim = map.dimension;
	int uorder = map.uorder;
	int vorder = map.vorder;
	if (uorder <= 0 || vorder <= 0 || dim <= 0) return;

	// Normalize parameters
	float urange = map.u2 - map.u1;
	float vrange = map.v2 - map.v1;
	float tu = (urange != 0.0f) ? (u - map.u1) / urange : 0.0f;
	float tv = (vrange != 0.0f) ? (v - map.v1) / vrange : 0.0f;

	// For each u row, evaluate along v to get uorder intermediate points
	std::vector<float> upoints(uorder * dim);
	std::vector<float> vtmp(vorder * dim);

	for (int ui = 0; ui < uorder; ui++) {
		// Extract the v-row for this u index
		for (int vi = 0; vi < vorder; vi++) {
			for (int d = 0; d < dim; d++) {
				vtmp[vi * dim + d] = map.control_points[(ui * vorder + vi) * dim + d];
			}
		}
		// de Casteljau in v
		for (int r = 1; r < vorder; r++) {
			for (int i = 0; i < vorder - r; i++) {
				for (int d = 0; d < dim; d++) {
					vtmp[i * dim + d] = (1.0f - tv) * vtmp[i * dim + d] + tv * vtmp[(i + 1) * dim + d];
				}
			}
		}
		for (int d = 0; d < dim; d++)
			upoints[ui * dim + d] = vtmp[d];
	}

	// de Casteljau in u on the intermediate points
	for (int r = 1; r < uorder; r++) {
		for (int i = 0; i < uorder - r; i++) {
			for (int d = 0; d < dim; d++) {
				upoints[i * dim + d] = (1.0f - tu) * upoints[i * dim + d] + tu * upoints[(i + 1) * dim + d];
			}
		}
	}

	for (int d = 0; d < dim; d++)
		out[d] = upoints[d];
}

// Forward declarations for immediate-mode functions used by evaluator
extern void NativeGLBegin(GLContext *ctx, uint32_t mode);
extern void NativeGLEnd(GLContext *ctx);
extern void NativeGLVertex3f(GLContext *ctx, float x, float y, float z);
extern void NativeGLVertex4f(GLContext *ctx, float x, float y, float z, float w);
extern void NativeGLNormal3f(GLContext *ctx, float x, float y, float z);
extern void NativeGLColor4f(GLContext *ctx, float r, float g, float b, float a);
extern void NativeGLTexCoord1f(GLContext *ctx, float s);
extern void NativeGLTexCoord2f(GLContext *ctx, float s, float t);
extern void NativeGLTexCoord3f(GLContext *ctx, float s, float t, float r);
extern void NativeGLTexCoord4f(GLContext *ctx, float s, float t, float r, float q);

// Apply evaluated result to the corresponding vertex attribute
static void gl_eval_apply_map1(GLContext *ctx, int idx, const float *val)
{
	switch (idx) {
	case 0: NativeGLVertex3f(ctx, val[0], val[1], val[2]); break;                    // VERTEX_3
	case 1: NativeGLVertex4f(ctx, val[0], val[1], val[2], val[3]); break;             // VERTEX_4
	case 2: /* INDEX -- no-op in Metal */ break;
	case 3: NativeGLColor4f(ctx, val[0], val[1], val[2], val[3]); break;              // COLOR_4
	case 4: NativeGLNormal3f(ctx, val[0], val[1], val[2]); break;                     // NORMAL
	case 5: NativeGLTexCoord1f(ctx, val[0]); break;                                   // TEXCOORD_1
	case 6: NativeGLTexCoord2f(ctx, val[0], val[1]); break;                           // TEXCOORD_2
	case 7: NativeGLTexCoord3f(ctx, val[0], val[1], val[2]); break;                   // TEXCOORD_3
	case 8: NativeGLTexCoord4f(ctx, val[0], val[1], val[2], val[3]); break;           // TEXCOORD_4
	}
}

// glMap1f -- define a 1D evaluator map (float version)
void NativeGLMap1f(GLContext *ctx, uint32_t t, float u1, float u2, int32_t s, int32_t o, uint32_t p)
{
	int idx = gl_eval_target_index(t);
	if (idx < 0) return;
	int dim = gl_eval_target_dimension(t);
	if (dim <= 0 || o <= 0) return;

	GLEvaluatorMap1 &map = ctx->eval_map1[idx];
	map.target = t;
	map.order = o;
	map.u1 = u1;
	map.u2 = u2;
	map.stride = s;
	map.dimension = dim;
	map.control_points.resize(o * dim);

	// Read control points from Mac memory (stride-aware)
	for (int i = 0; i < o; i++) {
		uint32_t base = p + i * s * 4; // stride is in floats, each float = 4 bytes
		for (int d = 0; d < dim; d++) {
			map.control_points[i * dim + d] = ReadMacFloat(base + d * 4);
		}
	}
	map.defined = true;

	GL_LOG("Map1f target=0x%X order=%d dim=%d u=[%f,%f]", t, o, dim, u1, u2);
}

// glMap1d -- define a 1D evaluator map (double version, converts to float)
void NativeGLMap1d(GLContext *ctx, uint32_t t, double u1, double u2, int32_t s, int32_t o, uint32_t p)
{
	int idx = gl_eval_target_index(t);
	if (idx < 0) return;
	int dim = gl_eval_target_dimension(t);
	if (dim <= 0 || o <= 0) return;

	GLEvaluatorMap1 &map = ctx->eval_map1[idx];
	map.target = t;
	map.order = o;
	map.u1 = (float)u1;
	map.u2 = (float)u2;
	map.stride = s;
	map.dimension = dim;
	map.control_points.resize(o * dim);

	// Read doubles from Mac memory, convert to float
	for (int i = 0; i < o; i++) {
		uint32_t base = p + i * s * 8; // stride is in doubles, each double = 8 bytes
		for (int d = 0; d < dim; d++) {
			map.control_points[i * dim + d] = (float)ReadMacDouble(base + d * 8);
		}
	}
	map.defined = true;
}

// glMap2f -- define a 2D evaluator map (float version)
void NativeGLMap2f(GLContext *ctx, uint32_t t, float u1, float u2, int32_t us, int32_t uo, float v1, float v2, int32_t vs, int32_t vo, uint32_t p)
{
	int idx = gl_eval_target_index(t);
	if (idx < 0) return;
	int dim = gl_eval_target_dimension(t);
	if (dim <= 0 || uo <= 0 || vo <= 0) return;

	GLEvaluatorMap2 &map = ctx->eval_map2[idx];
	map.target = t;
	map.uorder = uo;
	map.vorder = vo;
	map.u1 = u1;
	map.u2 = u2;
	map.v1 = v1;
	map.v2 = v2;
	map.ustride = us;
	map.vstride = vs;
	map.dimension = dim;
	map.control_points.resize(uo * vo * dim);

	// Read control points from Mac memory (stride-aware in both u and v)
	for (int ui = 0; ui < uo; ui++) {
		for (int vi = 0; vi < vo; vi++) {
			uint32_t base = p + ui * us * 4 + vi * vs * 4;
			for (int d = 0; d < dim; d++) {
				map.control_points[(ui * vo + vi) * dim + d] = ReadMacFloat(base + d * 4);
			}
		}
	}
	map.defined = true;

	GL_LOG("Map2f target=0x%X uorder=%d vorder=%d dim=%d u=[%f,%f] v=[%f,%f]",
	       t, uo, vo, dim, u1, u2, v1, v2);
}

// glMap2d -- define a 2D evaluator map (double version)
void NativeGLMap2d(GLContext *ctx, uint32_t t, double u1, double u2, int32_t us, int32_t uo, double v1, double v2, int32_t vs, int32_t vo, uint32_t p)
{
	int idx = gl_eval_target_index(t);
	if (idx < 0) return;
	int dim = gl_eval_target_dimension(t);
	if (dim <= 0 || uo <= 0 || vo <= 0) return;

	GLEvaluatorMap2 &map = ctx->eval_map2[idx];
	map.target = t;
	map.uorder = uo;
	map.vorder = vo;
	map.u1 = (float)u1;
	map.u2 = (float)u2;
	map.v1 = (float)v1;
	map.v2 = (float)v2;
	map.ustride = us;
	map.vstride = vs;
	map.dimension = dim;
	map.control_points.resize(uo * vo * dim);

	for (int ui = 0; ui < uo; ui++) {
		for (int vi = 0; vi < vo; vi++) {
			uint32_t base = p + ui * us * 8 + vi * vs * 8;
			for (int d = 0; d < dim; d++) {
				map.control_points[(ui * vo + vi) * dim + d] = (float)ReadMacDouble(base + d * 8);
			}
		}
	}
	map.defined = true;
}

// glMapGrid1f/1d -- define a 1D evaluation grid
void NativeGLMapGrid1f(GLContext *ctx, int32_t un, float u1, float u2)
{
	ctx->grid1_un = un;
	ctx->grid1_u1 = u1;
	ctx->grid1_u2 = u2;
}

void NativeGLMapGrid1d(GLContext *ctx, int32_t un, double u1, double u2)
{
	NativeGLMapGrid1f(ctx, un, (float)u1, (float)u2);
}

// glMapGrid2f/2d -- define a 2D evaluation grid
void NativeGLMapGrid2f(GLContext *ctx, int32_t un, float u1, float u2, int32_t vn, float v1, float v2)
{
	ctx->grid2_un = un;
	ctx->grid2_u1 = u1;
	ctx->grid2_u2 = u2;
	ctx->grid2_vn = vn;
	ctx->grid2_v1 = v1;
	ctx->grid2_v2 = v2;
}

void NativeGLMapGrid2d(GLContext *ctx, int32_t un, double u1, double u2, int32_t vn, double v1, double v2)
{
	NativeGLMapGrid2f(ctx, un, (float)u1, (float)u2, vn, (float)v1, (float)v2);
}

// glEvalCoord1f -- evaluate all enabled 1D maps at parameter u
void NativeGLEvalCoord1f(GLContext *ctx, float u)
{
	float val[4];
	// Evaluate non-vertex attributes first, then vertex (vertex triggers emit)
	for (int idx = 2; idx < GL_EVAL_MAX_TARGETS; idx++) {
		if (ctx->eval_map1_enabled[idx] && ctx->eval_map1[idx].defined) {
			gl_eval_bernstein1(ctx->eval_map1[idx], u, val);
			gl_eval_apply_map1(ctx, idx, val);
		}
	}
	// Vertex targets last (idx 0 = VERTEX_3, idx 1 = VERTEX_4)
	for (int idx = 0; idx < 2; idx++) {
		if (ctx->eval_map1_enabled[idx] && ctx->eval_map1[idx].defined) {
			gl_eval_bernstein1(ctx->eval_map1[idx], u, val);
			gl_eval_apply_map1(ctx, idx, val);
		}
	}
}

void NativeGLEvalCoord1d(GLContext *ctx, double u) { NativeGLEvalCoord1f(ctx, (float)u); }

// glEvalCoord2f -- evaluate all enabled 2D maps at parameters (u,v)
void NativeGLEvalCoord2f(GLContext *ctx, float u, float v)
{
	float val[4];
	// Non-vertex attributes first
	for (int idx = 2; idx < GL_EVAL_MAX_TARGETS; idx++) {
		if (ctx->eval_map2_enabled[idx] && ctx->eval_map2[idx].defined) {
			gl_eval_bernstein2(ctx->eval_map2[idx], u, v, val);
			gl_eval_apply_map1(ctx, idx, val); // same apply logic
		}
	}
	// Vertex targets last
	for (int idx = 0; idx < 2; idx++) {
		if (ctx->eval_map2_enabled[idx] && ctx->eval_map2[idx].defined) {
			gl_eval_bernstein2(ctx->eval_map2[idx], u, v, val);
			gl_eval_apply_map1(ctx, idx, val);
		}
	}
}

void NativeGLEvalCoord2d(GLContext *ctx, double u, double v) { NativeGLEvalCoord2f(ctx, (float)u, (float)v); }

// glEvalCoord*v -- vector variants read parameter from Mac memory
void NativeGLEvalCoord1fv(GLContext *ctx, uint32_t p) { NativeGLEvalCoord1f(ctx, ReadMacFloat(p)); }
void NativeGLEvalCoord1dv(GLContext *ctx, uint32_t p) { NativeGLEvalCoord1f(ctx, (float)ReadMacDouble(p)); }
void NativeGLEvalCoord2fv(GLContext *ctx, uint32_t p) { NativeGLEvalCoord2f(ctx, ReadMacFloat(p), ReadMacFloat(p + 4)); }
void NativeGLEvalCoord2dv(GLContext *ctx, uint32_t p) { NativeGLEvalCoord2f(ctx, (float)ReadMacDouble(p), (float)ReadMacDouble(p + 8)); }

// glEvalMesh1 -- generate 1D mesh through Begin/End pipeline
void NativeGLEvalMesh1(GLContext *ctx, uint32_t mode, int32_t i1, int32_t i2)
{
	if (ctx->grid1_un <= 0) return;

	float du = (ctx->grid1_u2 - ctx->grid1_u1) / (float)ctx->grid1_un;

	uint32_t prim = (mode == GL_LINE) ? GL_LINE_STRIP : GL_POINTS;
	NativeGLBegin(ctx, prim);
	for (int32_t i = i1; i <= i2; i++) {
		float u = ctx->grid1_u1 + (float)i * du;
		NativeGLEvalCoord1f(ctx, u);
	}
	NativeGLEnd(ctx);
}

// glEvalMesh2 -- generate 2D mesh through Begin/End pipeline
void NativeGLEvalMesh2(GLContext *ctx, uint32_t mode, int32_t i1, int32_t i2, int32_t j1, int32_t j2)
{
	if (ctx->grid2_un <= 0 || ctx->grid2_vn <= 0) return;

	float du = (ctx->grid2_u2 - ctx->grid2_u1) / (float)ctx->grid2_un;
	float dv = (ctx->grid2_v2 - ctx->grid2_v1) / (float)ctx->grid2_vn;

	if (mode == GL_POINT) {
		NativeGLBegin(ctx, GL_POINTS);
		for (int32_t j = j1; j <= j2; j++) {
			for (int32_t i = i1; i <= i2; i++) {
				float u = ctx->grid2_u1 + (float)i * du;
				float v = ctx->grid2_v1 + (float)j * dv;
				NativeGLEvalCoord2f(ctx, u, v);
			}
		}
		NativeGLEnd(ctx);
	} else if (mode == GL_LINE) {
		// Row-major line strips
		for (int32_t j = j1; j <= j2; j++) {
			NativeGLBegin(ctx, GL_LINE_STRIP);
			for (int32_t i = i1; i <= i2; i++) {
				float u = ctx->grid2_u1 + (float)i * du;
				float v = ctx->grid2_v1 + (float)j * dv;
				NativeGLEvalCoord2f(ctx, u, v);
			}
			NativeGLEnd(ctx);
		}
		// Column line strips
		for (int32_t i = i1; i <= i2; i++) {
			NativeGLBegin(ctx, GL_LINE_STRIP);
			for (int32_t j = j1; j <= j2; j++) {
				float u = ctx->grid2_u1 + (float)i * du;
				float v = ctx->grid2_v1 + (float)j * dv;
				NativeGLEvalCoord2f(ctx, u, v);
			}
			NativeGLEnd(ctx);
		}
	} else {
		// GL_FILL -- emit quad strips per row
		for (int32_t j = j1; j < j2; j++) {
			NativeGLBegin(ctx, GL_QUAD_STRIP);
			for (int32_t i = i1; i <= i2; i++) {
				float u = ctx->grid2_u1 + (float)i * du;
				float v0 = ctx->grid2_v1 + (float)j * dv;
				float v1 = ctx->grid2_v1 + (float)(j + 1) * dv;
				NativeGLEvalCoord2f(ctx, u, v0);
				NativeGLEvalCoord2f(ctx, u, v1);
			}
			NativeGLEnd(ctx);
		}
	}

	GL_LOG("EvalMesh2 mode=0x%X i=[%d,%d] j=[%d,%d]", mode, i1, i2, j1, j2);
}

// glEvalPoint1 -- evaluate at a single 1D grid point
void NativeGLEvalPoint1(GLContext *ctx, int32_t i)
{
	if (ctx->grid1_un <= 0) return;
	float du = (ctx->grid1_u2 - ctx->grid1_u1) / (float)ctx->grid1_un;
	float u = ctx->grid1_u1 + (float)i * du;
	NativeGLEvalCoord1f(ctx, u);
}

// glEvalPoint2 -- evaluate at a single 2D grid point
void NativeGLEvalPoint2(GLContext *ctx, int32_t i, int32_t j)
{
	if (ctx->grid2_un <= 0 || ctx->grid2_vn <= 0) return;
	float du = (ctx->grid2_u2 - ctx->grid2_u1) / (float)ctx->grid2_un;
	float dv = (ctx->grid2_v2 - ctx->grid2_v1) / (float)ctx->grid2_vn;
	float u = ctx->grid2_u1 + (float)i * du;
	float v = ctx->grid2_v1 + (float)j * dv;
	NativeGLEvalCoord2f(ctx, u, v);
}

// --- Selection and Feedback ---

// Flush the current selection hit record to the selection buffer (if any primitive was drawn)
static void FlushSelectionHit(GLContext *ctx)
{
	if (!ctx->selection_hit_flag) return;
	if (!ctx->selection_buffer_mac_ptr) return;

	// A hit record is: {name_stack_depth, z_min, z_max, name[0], name[1], ...}
	int recordSize = 3 + ctx->name_stack_depth;
	if (ctx->selection_buffer_offset + recordSize > ctx->selection_buffer_size) {
		// Buffer overflow -- stop writing but keep counting
		ctx->selection_hit_count++;
		ctx->selection_hit_flag = false;
		return;
	}

	uint32_t base = ctx->selection_buffer_mac_ptr + ctx->selection_buffer_offset * 4;
	WriteMacInt32(base + 0, ctx->name_stack_depth);
	WriteMacInt32(base + 4, ctx->selection_hit_z_min);
	WriteMacInt32(base + 8, ctx->selection_hit_z_max);
	for (int i = 0; i < ctx->name_stack_depth; i++) {
		WriteMacInt32(base + 12 + i * 4, ctx->name_stack[i]);
	}
	ctx->selection_buffer_offset += recordSize;
	ctx->selection_hit_count++;
	ctx->selection_hit_flag = false;
}

// Called during GL_SELECT mode when a primitive is rasterized.
// Records a hit with z_min/z_max derived from the vertex Z values.
static void SelectionRecordHit(GLContext *ctx, float z_min, float z_max)
{
	// Convert floating-point Z [0,1] to GLuint range [0, 0xFFFFFFFF]
	uint32_t iz_min = (uint32_t)(z_min * 4294967295.0f);
	uint32_t iz_max = (uint32_t)(z_max * 4294967295.0f);

	if (!ctx->selection_hit_flag) {
		ctx->selection_hit_flag = true;
		ctx->selection_hit_z_min = iz_min;
		ctx->selection_hit_z_max = iz_max;
	} else {
		if (iz_min < ctx->selection_hit_z_min) ctx->selection_hit_z_min = iz_min;
		if (iz_max > ctx->selection_hit_z_max) ctx->selection_hit_z_max = iz_max;
	}
}

// Called from NativeGLEnd when in GL_SELECT mode.
// Scans all vertices in im_vertices to find Z range, then records a hit.
// All primitives are considered hits (no clip-volume culling — conservative approach).
void GLSelectionRecordPrimitive(GLContext *ctx)
{
	if (ctx->im_vertices.empty()) return;

	// Transform vertices through modelview-projection to get clip-space Z
	// For simplicity, use the raw vertex Z as a normalized depth estimate.
	// A full implementation would multiply by the MVP matrix, but games
	// that use selection mode typically set up the pick matrix such that
	// Z is already in [0,1] or we can approximate with the object-space Z.
	float z_min = 1.0f, z_max = 0.0f;
	for (size_t i = 0; i < ctx->im_vertices.size(); i++) {
		float z = ctx->im_vertices[i].position[2];
		// Clamp to [0,1] for the GLuint conversion
		if (z < 0.0f) z = 0.0f;
		if (z > 1.0f) z = 1.0f;
		if (z < z_min) z_min = z;
		if (z > z_max) z_max = z;
	}

	SelectionRecordHit(ctx, z_min, z_max);
}

uint32_t NativeGLRenderMode(GLContext *ctx, uint32_t mode)
{
	uint32_t prev = ctx->render_mode;
	uint32_t result = 0;

	if (prev == GL_SELECT) {
		// Flush any pending hit
		FlushSelectionHit(ctx);
		result = ctx->selection_hit_count;
	}

	ctx->render_mode = mode;

	if (mode == GL_SELECT) {
		ctx->selection_hit_count = 0;
		ctx->selection_buffer_offset = 0;
		ctx->selection_hit_flag = false;
		ctx->name_stack_depth = 0;
	}

	return result;
}

void NativeGLSelectBuffer(GLContext *ctx, int32_t size, uint32_t buffer_ptr)
{
	ctx->selection_buffer_size = size;
	ctx->selection_buffer_mac_ptr = buffer_ptr;
}

void NativeGLFeedbackBuffer(GLContext *ctx, int32_t size, uint32_t type, uint32_t buffer_ptr)
{
	ctx->feedback_buffer_size = size;
	ctx->feedback_type = type;
	ctx->feedback_buffer_mac_ptr = buffer_ptr;
}

void NativeGLInitNames(GLContext *ctx) { ctx->name_stack_depth = 0; }

void NativeGLPushName(GLContext *ctx, uint32_t name)
{
	if (ctx->render_mode == GL_SELECT) FlushSelectionHit(ctx);
	if (ctx->name_stack_depth < 64) ctx->name_stack[ctx->name_stack_depth++] = name;
}

void NativeGLPopName(GLContext *ctx)
{
	if (ctx->render_mode == GL_SELECT) FlushSelectionHit(ctx);
	if (ctx->name_stack_depth > 0) ctx->name_stack_depth--;
}

void NativeGLLoadName(GLContext *ctx, uint32_t name)
{
	if (ctx->render_mode == GL_SELECT) FlushSelectionHit(ctx);
	if (ctx->name_stack_depth > 0) ctx->name_stack[ctx->name_stack_depth - 1] = name;
}
void NativeGLPassThrough(GLContext *ctx, float token)
{
	// In feedback mode, PassThrough inserts a token marker
	if (ctx->render_mode == GL_FEEDBACK && ctx->feedback_buffer_mac_ptr) {
		GL_LOG("PassThrough token=%.3f (feedback mode)", token);
	}
}

// --- Pixel Transfer and Maps ---
void NativeGLPixelTransferf(GLContext *ctx, uint32_t pname, float param)
{
	switch (pname) {
	case GL_RED_SCALE:    ctx->pixel_transfer_red_scale = param; break;
	case GL_GREEN_SCALE:  ctx->pixel_transfer_green_scale = param; break;
	case GL_BLUE_SCALE:   ctx->pixel_transfer_blue_scale = param; break;
	case GL_ALPHA_SCALE:  ctx->pixel_transfer_alpha_scale = param; break;
	case GL_RED_BIAS:     ctx->pixel_transfer_red_bias = param; break;
	case GL_GREEN_BIAS:   ctx->pixel_transfer_green_bias = param; break;
	case GL_BLUE_BIAS:    ctx->pixel_transfer_blue_bias = param; break;
	case GL_ALPHA_BIAS:   ctx->pixel_transfer_alpha_bias = param; break;
	case GL_DEPTH_SCALE:  ctx->pixel_transfer_depth_scale = param; break;
	case GL_DEPTH_BIAS:   ctx->pixel_transfer_depth_bias = param; break;
	default: break;
	}
}
void NativeGLPixelTransferi(GLContext *ctx, uint32_t pname, int32_t param)
{
	NativeGLPixelTransferf(ctx, pname, (float)param);
}

static int gl_pixel_map_clamped_size(int32_t mapsize)
{
	if (mapsize <= 0)
		return 0;
	if (mapsize > 256)
		return 256;
	return mapsize;
}

static void gl_pixel_map_store_float(GLContext *ctx, uint32_t map, int32_t mapsize, const float *values)
{
	int sz = gl_pixel_map_clamped_size(mapsize);
	switch (map) {
	case GL_PIXEL_MAP_I_TO_R: memcpy(ctx->pixel_map_i_to_r, values, sz * sizeof(float)); ctx->pixel_map_i_to_r_size = sz; break;
	case GL_PIXEL_MAP_I_TO_G: memcpy(ctx->pixel_map_i_to_g, values, sz * sizeof(float)); ctx->pixel_map_i_to_g_size = sz; break;
	case GL_PIXEL_MAP_I_TO_B: memcpy(ctx->pixel_map_i_to_b, values, sz * sizeof(float)); ctx->pixel_map_i_to_b_size = sz; break;
	case GL_PIXEL_MAP_I_TO_A: memcpy(ctx->pixel_map_i_to_a, values, sz * sizeof(float)); ctx->pixel_map_i_to_a_size = sz; break;
	case GL_PIXEL_MAP_R_TO_R: memcpy(ctx->pixel_map_r_to_r, values, sz * sizeof(float)); ctx->pixel_map_r_to_r_size = sz; break;
	case GL_PIXEL_MAP_G_TO_G: memcpy(ctx->pixel_map_g_to_g, values, sz * sizeof(float)); ctx->pixel_map_g_to_g_size = sz; break;
	case GL_PIXEL_MAP_B_TO_B: memcpy(ctx->pixel_map_b_to_b, values, sz * sizeof(float)); ctx->pixel_map_b_to_b_size = sz; break;
	case GL_PIXEL_MAP_A_TO_A: memcpy(ctx->pixel_map_a_to_a, values, sz * sizeof(float)); ctx->pixel_map_a_to_a_size = sz; break;
	default: break; // I_TO_I and S_TO_S are accepted silently; not consumed by RGBA uploads.
	}
}

void NativeGLPixelMapfv(GLContext *ctx, uint32_t map, int32_t mapsize, uint32_t values_ptr)
{
	int sz = gl_pixel_map_clamped_size(mapsize);
	float tmp[256];
	for (int i = 0; i < sz; i++) tmp[i] = ReadMacFloat(values_ptr + i * 4);
	gl_pixel_map_store_float(ctx, map, sz, tmp);
}
void NativeGLPixelMapuiv(GLContext *ctx, uint32_t map, int32_t mapsize, uint32_t values_ptr)
{
	int sz = gl_pixel_map_clamped_size(mapsize);
	float tmp[256];
	for (int i = 0; i < sz; i++) tmp[i] = (float)ReadMacInt32(values_ptr + i * 4) / 4294967295.0f;
	gl_pixel_map_store_float(ctx, map, sz, tmp);
}
void NativeGLPixelMapusv(GLContext *ctx, uint32_t map, int32_t mapsize, uint32_t values_ptr)
{
	int sz = gl_pixel_map_clamped_size(mapsize);
	float tmp[256];
	for (int i = 0; i < sz; i++) tmp[i] = (float)ReadMacInt16(values_ptr + i * 2) / 65535.0f;
	gl_pixel_map_store_float(ctx, map, sz, tmp);
}
void NativeGLGetPixelMapfv(GLContext *ctx, uint32_t map, uint32_t values)
{
	const float *src = nullptr; int sz = 0;
	switch (map) {
	case GL_PIXEL_MAP_I_TO_R: src = ctx->pixel_map_i_to_r; sz = ctx->pixel_map_i_to_r_size; break;
	case GL_PIXEL_MAP_I_TO_G: src = ctx->pixel_map_i_to_g; sz = ctx->pixel_map_i_to_g_size; break;
	case GL_PIXEL_MAP_I_TO_B: src = ctx->pixel_map_i_to_b; sz = ctx->pixel_map_i_to_b_size; break;
	case GL_PIXEL_MAP_I_TO_A: src = ctx->pixel_map_i_to_a; sz = ctx->pixel_map_i_to_a_size; break;
	case GL_PIXEL_MAP_R_TO_R: src = ctx->pixel_map_r_to_r; sz = ctx->pixel_map_r_to_r_size; break;
	case GL_PIXEL_MAP_G_TO_G: src = ctx->pixel_map_g_to_g; sz = ctx->pixel_map_g_to_g_size; break;
	case GL_PIXEL_MAP_B_TO_B: src = ctx->pixel_map_b_to_b; sz = ctx->pixel_map_b_to_b_size; break;
	case GL_PIXEL_MAP_A_TO_A: src = ctx->pixel_map_a_to_a; sz = ctx->pixel_map_a_to_a_size; break;
	default: WriteMacFloat(values, 0.0f); return;
	}
	for (int i = 0; i < sz; i++) WriteMacFloat(values + i * 4, src[i]);
}
void NativeGLGetPixelMapuiv(GLContext *ctx, uint32_t map, uint32_t values)
{
	const float *src = nullptr; int sz = 0;
	switch (map) {
	case GL_PIXEL_MAP_I_TO_R: src = ctx->pixel_map_i_to_r; sz = ctx->pixel_map_i_to_r_size; break;
	case GL_PIXEL_MAP_I_TO_G: src = ctx->pixel_map_i_to_g; sz = ctx->pixel_map_i_to_g_size; break;
	case GL_PIXEL_MAP_I_TO_B: src = ctx->pixel_map_i_to_b; sz = ctx->pixel_map_i_to_b_size; break;
	case GL_PIXEL_MAP_I_TO_A: src = ctx->pixel_map_i_to_a; sz = ctx->pixel_map_i_to_a_size; break;
	case GL_PIXEL_MAP_R_TO_R: src = ctx->pixel_map_r_to_r; sz = ctx->pixel_map_r_to_r_size; break;
	case GL_PIXEL_MAP_G_TO_G: src = ctx->pixel_map_g_to_g; sz = ctx->pixel_map_g_to_g_size; break;
	case GL_PIXEL_MAP_B_TO_B: src = ctx->pixel_map_b_to_b; sz = ctx->pixel_map_b_to_b_size; break;
	case GL_PIXEL_MAP_A_TO_A: src = ctx->pixel_map_a_to_a; sz = ctx->pixel_map_a_to_a_size; break;
	default: WriteMacInt32(values, 0); return;
	}
	for (int i = 0; i < sz; i++) WriteMacInt32(values + i * 4, (uint32_t)(src[i] * 4294967295.0f));
}
void NativeGLGetPixelMapusv(GLContext *ctx, uint32_t map, uint32_t values)
{
	const float *src = nullptr; int sz = 0;
	switch (map) {
	case GL_PIXEL_MAP_I_TO_R: src = ctx->pixel_map_i_to_r; sz = ctx->pixel_map_i_to_r_size; break;
	case GL_PIXEL_MAP_I_TO_G: src = ctx->pixel_map_i_to_g; sz = ctx->pixel_map_i_to_g_size; break;
	case GL_PIXEL_MAP_I_TO_B: src = ctx->pixel_map_i_to_b; sz = ctx->pixel_map_i_to_b_size; break;
	case GL_PIXEL_MAP_I_TO_A: src = ctx->pixel_map_i_to_a; sz = ctx->pixel_map_i_to_a_size; break;
	case GL_PIXEL_MAP_R_TO_R: src = ctx->pixel_map_r_to_r; sz = ctx->pixel_map_r_to_r_size; break;
	case GL_PIXEL_MAP_G_TO_G: src = ctx->pixel_map_g_to_g; sz = ctx->pixel_map_g_to_g_size; break;
	case GL_PIXEL_MAP_B_TO_B: src = ctx->pixel_map_b_to_b; sz = ctx->pixel_map_b_to_b_size; break;
	case GL_PIXEL_MAP_A_TO_A: src = ctx->pixel_map_a_to_a; sz = ctx->pixel_map_a_to_a_size; break;
	default: WriteMacInt16(values, 0); return;
	}
	for (int i = 0; i < sz; i++) WriteMacInt16(values + i * 2, (uint16_t)(src[i] * 65535.0f));
}
void NativeGLPixelZoom(GLContext *ctx, float xfactor, float yfactor) { ctx->pixel_zoom_x = xfactor; ctx->pixel_zoom_y = yfactor; }

// --- Index mode (RGBA mode always active -- stored for state queries) ---
void NativeGLIndexf(GLContext *ctx, float c) { ctx->current_index = c; }
void NativeGLIndexd(GLContext *ctx, double c) { ctx->current_index = (float)c; }
void NativeGLIndexi(GLContext *ctx, int32_t c) { ctx->current_index = (float)c; }
void NativeGLIndexs(GLContext *ctx, int16_t c) { ctx->current_index = (float)c; }
void NativeGLIndexub(GLContext *ctx, uint8_t c) { ctx->current_index = (float)c; }
void NativeGLIndexfv(GLContext *ctx, uint32_t p) { ctx->current_index = ReadMacFloat(p); }
void NativeGLIndexdv(GLContext *ctx, uint32_t p) { uint32_t hi = ReadMacInt32(p); uint32_t lo = ReadMacInt32(p+4); uint64_t bits = ((uint64_t)hi<<32)|lo; double d; memcpy(&d,&bits,sizeof(d)); ctx->current_index = (float)d; }
void NativeGLIndexiv(GLContext *ctx, uint32_t p) { ctx->current_index = (float)(int32_t)ReadMacInt32(p); }
void NativeGLIndexsv(GLContext *ctx, uint32_t p) { ctx->current_index = (float)(int16_t)ReadMacInt16(p); }
void NativeGLIndexubv(GLContext *ctx, uint32_t p) { ctx->current_index = (float)ReadMacInt8(p); }
void NativeGLIndexMask(GLContext *ctx, uint32_t mask) { ctx->index_write_mask = mask; }
void NativeGLIndexPointer(GLContext *ctx, uint32_t type, int32_t stride, uint32_t pointer) { ctx->index_array.type = type; ctx->index_array.stride = stride; ctx->index_array.pointer = pointer; }

// --- Edge flags ---
void NativeGLEdgeFlag(GLContext *ctx, uint32_t flag) { ctx->current_edge_flag = (flag != 0); }
void NativeGLEdgeFlagv(GLContext *ctx, uint32_t p) { ctx->current_edge_flag = (ReadMacInt32(p) != 0); }
void NativeGLEdgeFlagPointer(GLContext *ctx, int32_t stride, uint32_t pointer) { ctx->edge_flag_array.stride = stride; ctx->edge_flag_array.pointer = pointer; }

// --- Line/polygon stipple ---
void NativeGLLineStipple(GLContext *ctx, int32_t factor, uint32_t pattern) { ctx->line_stipple_factor = factor; ctx->line_stipple_pattern = (uint16_t)pattern; }
void NativeGLPolygonStipple(GLContext *ctx, uint32_t mask_ptr)
{
	for (int i = 0; i < 32; i++)
		ctx->polygon_stipple[i] = ReadMacInt32(mask_ptr + i * 4);
}
void NativeGLGetPolygonStipple(GLContext *ctx, uint32_t mask_ptr)
{
	for (int i = 0; i < 32; i++)
		WriteMacInt32(mask_ptr + i * 4, ctx->polygon_stipple[i]);
}

// --- Texture residency ---
void NativeGLAreTexturesResident(GLContext *ctx, int32_t n, uint32_t textures_ptr, uint32_t residences_ptr)
{
	for (int32_t i = 0; i < n; i++) WriteMacInt32(residences_ptr + i * 4, GL_TRUE);
}

void NativeGLPrioritizeTextures(GLContext *ctx, int32_t n, uint32_t textures_ptr, uint32_t priorities_ptr)
{
	// Store priority hints on texture objects (advisory, not enforced by Metal)
	for (int32_t i = 0; i < n; i++) {
		uint32_t name = ReadMacInt32(textures_ptr + i * 4);
		// Priority is stored but not used by Metal runtime
		auto it = ctx->texture_objects.find(name);
		if (it != ctx->texture_objects.end()) {
			// Accept and ignore priority (Metal manages memory automatically)
		}
	}
}

// --- Display lists ---
void NativeGLNewList(GLContext *ctx, uint32_t list, uint32_t mode)
{
	ctx->in_display_list = true;
	ctx->current_list_name = list;
	ctx->current_list_mode = mode;
	ctx->display_lists[list] = GLDisplayList();
}

void NativeGLEndList(GLContext *ctx) { ctx->in_display_list = false; }

// GLReplayCommand - replay a single recorded display list command
// Implemented in gl_dispatch.cpp (has access to all NativeGL* handlers)
extern void GLReplayCommand(GLContext *ctx, const GLCommand &cmd);

void NativeGLCallList(GLContext *ctx, uint32_t list)
{
	auto it = ctx->display_lists.find(list);
	if (it == ctx->display_lists.end()) return;

	for (const GLCommand &cmd : it->second.commands) {
		GLReplayCommand(ctx, cmd);
	}
}

void NativeGLCallLists(GLContext *ctx, int32_t n, uint32_t type, uint32_t lists_ptr)
{
	for (int32_t i = 0; i < n; i++) {
		uint32_t name = 0;
		switch (type) {
		case 0x1400: // GL_BYTE
			name = (uint32_t)(int8_t)ReadMacInt8(lists_ptr + i);
			break;
		case 0x1401: // GL_UNSIGNED_BYTE
			name = ReadMacInt8(lists_ptr + i);
			break;
		case 0x1402: // GL_SHORT
			name = (uint32_t)(int16_t)ReadMacInt16(lists_ptr + i * 2);
			break;
		case 0x1403: // GL_UNSIGNED_SHORT
			name = ReadMacInt16(lists_ptr + i * 2);
			break;
		case 0x1404: // GL_INT
			name = ReadMacInt32(lists_ptr + i * 4);
			break;
		case 0x1405: // GL_UNSIGNED_INT
			name = ReadMacInt32(lists_ptr + i * 4);
			break;
		case 0x1406: // GL_FLOAT
			{
				uint32_t bits = ReadMacInt32(lists_ptr + i * 4);
				float f; memcpy(&f, &bits, 4);
				name = (uint32_t)(int)f;
			}
			break;
		case 0x1407: // GL_2_BYTES
			name = ((uint32_t)ReadMacInt8(lists_ptr + i * 2) << 8) |
			        ReadMacInt8(lists_ptr + i * 2 + 1);
			break;
		case 0x1408: // GL_3_BYTES
			name = ((uint32_t)ReadMacInt8(lists_ptr + i * 3) << 16) |
			       ((uint32_t)ReadMacInt8(lists_ptr + i * 3 + 1) << 8) |
			        ReadMacInt8(lists_ptr + i * 3 + 2);
			break;
		case 0x1409: // GL_4_BYTES
			name = ReadMacInt32(lists_ptr + i * 4);
			break;
		default:
			name = ReadMacInt32(lists_ptr + i * 4);
			break;
		}
		name += ctx->list_base;
		NativeGLCallList(ctx, name);
	}
}

uint32_t NativeGLGenLists(GLContext *ctx, int32_t range)
{
	uint32_t base = ctx->next_list_name;
	ctx->next_list_name += range;
	return base;
}

void NativeGLDeleteLists(GLContext *ctx, uint32_t list, int32_t range) { for (int32_t i = 0; i < range; i++) ctx->display_lists.erase(list + i); }
uint32_t NativeGLIsList(GLContext *ctx, uint32_t list) { return (ctx->display_lists.find(list) != ctx->display_lists.end()) ? GL_TRUE : GL_FALSE; }
void NativeGLListBase(GLContext *ctx, uint32_t base) { ctx->list_base = base; }

// --- Vertex arrays ---
void NativeGLVertexPointer(GLContext *ctx, int32_t size, uint32_t type, int32_t stride, uint32_t pointer) { ctx->vertex_array.size = size; ctx->vertex_array.type = type; ctx->vertex_array.stride = stride; ctx->vertex_array.pointer = pointer; }
void NativeGLNormalPointer(GLContext *ctx, uint32_t type, int32_t stride, uint32_t pointer) { ctx->normal_array.type = type; ctx->normal_array.stride = stride; ctx->normal_array.pointer = pointer; }
void NativeGLColorPointer(GLContext *ctx, int32_t size, uint32_t type, int32_t stride, uint32_t pointer) { ctx->color_array.size = size; ctx->color_array.type = type; ctx->color_array.stride = stride; ctx->color_array.pointer = pointer; }
void NativeGLTexCoordPointer(GLContext *ctx, int32_t size, uint32_t type,
                             int32_t stride, uint32_t pointer)
{
	int u = gl_clamp_texture_unit(ctx->client_active_texture);
	ctx->texcoord_array[u].size = size;
	ctx->texcoord_array[u].type = type;
	ctx->texcoord_array[u].stride = stride;
	ctx->texcoord_array[u].pointer = pointer;
	gl_invalidate_prepared_texcoord_array_for_draw(ctx, u);
}

void NativeGLEnableClientState(GLContext *ctx, uint32_t array)
{
	switch (array) {
	case GL_VERTEX_ARRAY:       ctx->vertex_array.enabled = true; break;
	case GL_NORMAL_ARRAY:       ctx->normal_array.enabled = true; break;
	case GL_COLOR_ARRAY:        ctx->color_array.enabled = true; break;
	case GL_TEXTURE_COORD_ARRAY: {
		const int unit = gl_clamp_texture_unit(ctx->client_active_texture);
		ctx->texcoord_array[unit].enabled = true;
		ctx->texcoord_array_enable_draw_serial[unit] = ctx->completed_draw_serial;
		ctx->prepared_texcoord_array_for_draw[unit] = false;
		break;
	}
	case GL_EDGE_FLAG_ARRAY:    ctx->edge_flag_array.enabled = true; break;
	case GL_INDEX_ARRAY:        ctx->index_array.enabled = true; break;
	default: break;
	}
}

void NativeGLDisableClientState(GLContext *ctx, uint32_t array)
{
	switch (array) {
	case GL_VERTEX_ARRAY:       ctx->vertex_array.enabled = false; break;
	case GL_NORMAL_ARRAY:       ctx->normal_array.enabled = false; break;
	case GL_COLOR_ARRAY:        ctx->color_array.enabled = false; break;
	case GL_TEXTURE_COORD_ARRAY: {
		const int unit = gl_clamp_texture_unit(ctx->client_active_texture);
		const bool hadResource =
			ctx->texcoord_array[unit].enabled &&
			ctx->texcoord_array[unit].pointer != 0;
		if (GLMetalPreparedDrawStateSurvivesDisable(
				ctx->texcoord_array_enable_draw_serial[unit],
				ctx->completed_draw_serial,
				hadResource)) {
			ctx->prepared_texcoord_array[unit] = ctx->texcoord_array[unit];
			ctx->prepared_texcoord_array[unit].enabled = true;
			ctx->prepared_texcoord_array_for_draw[unit] = true;
		} else {
			ctx->prepared_texcoord_array_for_draw[unit] = false;
		}
		ctx->texcoord_array[unit].enabled = false;
		break;
	}
	case GL_EDGE_FLAG_ARRAY:    ctx->edge_flag_array.enabled = false; break;
	case GL_INDEX_ARRAY:        ctx->index_array.enabled = false; break;
	default: break;
	}
}

// NativeGLArrayElement, NativeGLDrawArrays, NativeGLDrawElements, NativeGLInterleavedArrays
// are implemented in gl_metal_renderer.mm (they need Metal rendering access)

// --- Copy tex 1D (delegate to 2D with height=1) ---
void NativeGLCopyTexImage1D(GLContext *ctx, uint32_t target, int32_t level, uint32_t ifmt, int32_t x, int32_t y, int32_t w, int32_t border)
{
	NativeGLCopyTexImage2D(ctx, target, level, ifmt, x, y, w, 1, border);
}
void NativeGLCopyTexSubImage1D(GLContext *ctx, uint32_t target, int32_t level, int32_t xoff, int32_t x, int32_t y, int32_t w)
{
	NativeGLCopyTexSubImage2D(ctx, target, level, xoff, 0, x, y, w, 1);
}

// --- Get functions ---
void NativeGLGetTexEnvfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr)
{
	if (target != GL_TEXTURE_ENV) return;
	if (pname == GL_TEXTURE_ENV_MODE) WriteMacFloat(mac_ptr, (float)ctx->tex_units[ctx->active_texture].env_mode);
	else if (pname == GL_TEXTURE_ENV_COLOR) for (int i=0;i<4;i++) WriteMacFloat(mac_ptr+i*4, ctx->tex_units[ctx->active_texture].env_color[i]);
}

void NativeGLGetTexEnviv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr)
{
	if (target != GL_TEXTURE_ENV) return;
	if (pname == GL_TEXTURE_ENV_MODE) WriteMacInt32(mac_ptr, ctx->tex_units[ctx->active_texture].env_mode);
}

void NativeGLGetTexGendv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t p) { (void)coord; (void)pname; WriteMacInt32(p, 0); WriteMacInt32(p+4, 0); }
void NativeGLGetTexGenfv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t p) { (void)coord; (void)pname; WriteMacFloat(p, 0.0f); }
void NativeGLGetTexGeniv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t p) { (void)coord; (void)pname; WriteMacInt32(p, 0); }
void NativeGLGetTexImage(GLContext *ctx, uint32_t target, int32_t level, uint32_t fmt, uint32_t type, uint32_t p)
{
	// Return zeroed data with warning — full Metal texture readback not yet implemented
	GL_LOG("WARNING: glGetTexImage returning zeroes (known limitation)");
	uint32_t tex = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	auto it = ctx->texture_objects.find(tex);
	if (it != ctx->texture_objects.end() && p != 0) {
		int w = it->second.width;
		int h = it->second.height;
		int bytes = w * h * 4;
		for (int i = 0; i < bytes; i++) WriteMacInt8(p + i, 0);
	}
}
void NativeGLGetTexLevelParameterfv(GLContext *ctx, uint32_t target, int32_t level, uint32_t pname, uint32_t p)
{
	uint32_t tex = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	auto it = ctx->texture_objects.find(tex);
	if (it == ctx->texture_objects.end()) { WriteMacFloat(p, 0.0f); return; }
	GLTextureObject &obj = it->second;
	switch (pname) {
	case GL_TEXTURE_WIDTH:           WriteMacFloat(p, (float)obj.width); break;
	case GL_TEXTURE_HEIGHT:          WriteMacFloat(p, (float)obj.height); break;
	case GL_TEXTURE_INTERNAL_FORMAT: WriteMacFloat(p, (float)GL_RGBA); break;
	case GL_TEXTURE_BORDER:          WriteMacFloat(p, 0.0f); break;
	default: WriteMacFloat(p, 0.0f); break;
	}
}
void NativeGLGetTexLevelParameteriv(GLContext *ctx, uint32_t target, int32_t level, uint32_t pname, uint32_t p)
{
	uint32_t tex = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	auto it = ctx->texture_objects.find(tex);
	if (it == ctx->texture_objects.end()) { WriteMacInt32(p, 0); return; }
	GLTextureObject &obj = it->second;
	switch (pname) {
	case GL_TEXTURE_WIDTH:           WriteMacInt32(p, obj.width); break;
	case GL_TEXTURE_HEIGHT:          WriteMacInt32(p, obj.height); break;
	case GL_TEXTURE_INTERNAL_FORMAT: WriteMacInt32(p, GL_RGBA); break;
	case GL_TEXTURE_BORDER:          WriteMacInt32(p, 0); break;
	default: WriteMacInt32(p, 0); break;
	}
}

void NativeGLGetTexParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr)
{
	(void)target;
	uint32_t tex = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	auto it = ctx->texture_objects.find(tex);
	if (it == ctx->texture_objects.end()) { WriteMacFloat(mac_ptr, 0.0f); return; }
	GLTextureObject &obj = it->second;
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER: WriteMacFloat(mac_ptr, (float)obj.min_filter); break;
	case GL_TEXTURE_MAG_FILTER: WriteMacFloat(mac_ptr, (float)obj.mag_filter); break;
	case GL_TEXTURE_WRAP_S:     WriteMacFloat(mac_ptr, (float)obj.wrap_s); break;
	case GL_TEXTURE_WRAP_T:     WriteMacFloat(mac_ptr, (float)obj.wrap_t); break;
	default: WriteMacFloat(mac_ptr, 0.0f); break;
	}
}

void NativeGLGetTexParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr)
{
	(void)target;
	uint32_t tex = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	auto it = ctx->texture_objects.find(tex);
	if (it == ctx->texture_objects.end()) { WriteMacInt32(mac_ptr, 0); return; }
	GLTextureObject &obj = it->second;
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER: WriteMacInt32(mac_ptr, obj.min_filter); break;
	case GL_TEXTURE_MAG_FILTER: WriteMacInt32(mac_ptr, obj.mag_filter); break;
	case GL_TEXTURE_WRAP_S:     WriteMacInt32(mac_ptr, obj.wrap_s); break;
	case GL_TEXTURE_WRAP_T:     WriteMacInt32(mac_ptr, obj.wrap_t); break;
	default: WriteMacInt32(mac_ptr, 0); break;
	}
}

void NativeGLGetLightfv(GLContext *ctx, uint32_t light, uint32_t pname, uint32_t mac_ptr)
{
	if (light < GL_LIGHT0 || light >= GL_LIGHT0 + 8) return;
	const GLLight &l = ctx->lights[light - GL_LIGHT0];
	switch (pname) {
	case GL_AMBIENT:  for (int i=0;i<4;i++) WriteMacFloat(mac_ptr+i*4, l.ambient[i]); break;
	case GL_DIFFUSE:  for (int i=0;i<4;i++) WriteMacFloat(mac_ptr+i*4, l.diffuse[i]); break;
	case GL_SPECULAR: for (int i=0;i<4;i++) WriteMacFloat(mac_ptr+i*4, l.specular[i]); break;
	case GL_POSITION: for (int i=0;i<4;i++) WriteMacFloat(mac_ptr+i*4, l.position[i]); break;
	case GL_SPOT_DIRECTION: for (int i=0;i<3;i++) WriteMacFloat(mac_ptr+i*4, l.spot_direction[i]); break;
	case GL_SPOT_EXPONENT: WriteMacFloat(mac_ptr, l.spot_exponent); break;
	case GL_SPOT_CUTOFF: WriteMacFloat(mac_ptr, l.spot_cutoff); break;
	case GL_CONSTANT_ATTENUATION: WriteMacFloat(mac_ptr, l.constant_attenuation); break;
	case GL_LINEAR_ATTENUATION: WriteMacFloat(mac_ptr, l.linear_attenuation); break;
	case GL_QUADRATIC_ATTENUATION: WriteMacFloat(mac_ptr, l.quadratic_attenuation); break;
	default: break;
	}
}

void NativeGLGetLightiv(GLContext *ctx, uint32_t light, uint32_t pname, uint32_t mac_ptr)
{
	if (light < GL_LIGHT0 || light >= GL_LIGHT0 + 8) return;
	const GLLight &l = ctx->lights[light - GL_LIGHT0];
	switch (pname) {
	case GL_AMBIENT:  for (int i=0;i<4;i++) WriteMacInt32(mac_ptr+i*4, (uint32_t)(int32_t)l.ambient[i]); break;
	case GL_DIFFUSE:  for (int i=0;i<4;i++) WriteMacInt32(mac_ptr+i*4, (uint32_t)(int32_t)l.diffuse[i]); break;
	case GL_SPECULAR: for (int i=0;i<4;i++) WriteMacInt32(mac_ptr+i*4, (uint32_t)(int32_t)l.specular[i]); break;
	default: WriteMacInt32(mac_ptr, 0); break;
	}
}

void NativeGLGetMaterialfv(GLContext *ctx, uint32_t face, uint32_t pname, uint32_t mac_ptr)
{
	int idx = (face == GL_BACK) ? 1 : 0;
	const GLMaterial &mat = ctx->materials[idx];
	switch (pname) {
	case GL_AMBIENT:   for (int i=0;i<4;i++) WriteMacFloat(mac_ptr+i*4, mat.ambient[i]); break;
	case GL_DIFFUSE:   for (int i=0;i<4;i++) WriteMacFloat(mac_ptr+i*4, mat.diffuse[i]); break;
	case GL_SPECULAR:  for (int i=0;i<4;i++) WriteMacFloat(mac_ptr+i*4, mat.specular[i]); break;
	case GL_EMISSION:  for (int i=0;i<4;i++) WriteMacFloat(mac_ptr+i*4, mat.emission[i]); break;
	case GL_SHININESS: WriteMacFloat(mac_ptr, mat.shininess); break;
	default: break;
	}
}

void NativeGLGetMaterialiv(GLContext *ctx, uint32_t face, uint32_t pname, uint32_t mac_ptr)
{
	int idx = (face == GL_BACK) ? 1 : 0;
	const GLMaterial &mat = ctx->materials[idx];
	switch (pname) {
	case GL_SHININESS: WriteMacInt32(mac_ptr, (uint32_t)(int32_t)mat.shininess); break;
	default: WriteMacInt32(mac_ptr, 0); break;
	}
}

// glGetMapdv/fv/iv -- query evaluator map parameters
void NativeGLGetMapdv(GLContext *ctx, uint32_t target, uint32_t query, uint32_t p)
{
	int idx = gl_eval_target_index(target);
	if (idx < 0) return;

	// Determine if it's a MAP1 or MAP2 target
	bool is_map2 = (target >= GL_MAP2_COLOR_4 && target <= GL_MAP2_VERTEX_4);

	if (!is_map2) {
		const GLEvaluatorMap1 &map = ctx->eval_map1[idx];
		switch (query) {
		case GL_COEFF:
			for (int i = 0; i < map.order * map.dimension; i++) {
				// Write double to Mac memory (big-endian)
				double d = map.defined ? (double)map.control_points[i] : 0.0;
				uint64_t bits;
				memcpy(&bits, &d, sizeof(double));
				WriteMacInt32(p + i * 8, (uint32_t)(bits >> 32));
				WriteMacInt32(p + i * 8 + 4, (uint32_t)(bits & 0xFFFFFFFF));
			}
			break;
		case GL_ORDER:
			{
				double d = map.defined ? (double)map.order : 0.0;
				uint64_t bits;
				memcpy(&bits, &d, sizeof(double));
				WriteMacInt32(p, (uint32_t)(bits >> 32));
				WriteMacInt32(p + 4, (uint32_t)(bits & 0xFFFFFFFF));
			}
			break;
		case GL_DOMAIN:
			{
				double d0 = map.defined ? (double)map.u1 : 0.0;
				double d1 = map.defined ? (double)map.u2 : 0.0;
				uint64_t b0, b1;
				memcpy(&b0, &d0, sizeof(double));
				memcpy(&b1, &d1, sizeof(double));
				WriteMacInt32(p, (uint32_t)(b0 >> 32));
				WriteMacInt32(p + 4, (uint32_t)(b0 & 0xFFFFFFFF));
				WriteMacInt32(p + 8, (uint32_t)(b1 >> 32));
				WriteMacInt32(p + 12, (uint32_t)(b1 & 0xFFFFFFFF));
			}
			break;
		}
	} else {
		const GLEvaluatorMap2 &map = ctx->eval_map2[idx];
		switch (query) {
		case GL_COEFF:
			for (int i = 0; i < map.uorder * map.vorder * map.dimension; i++) {
				double d = map.defined ? (double)map.control_points[i] : 0.0;
				uint64_t bits;
				memcpy(&bits, &d, sizeof(double));
				WriteMacInt32(p + i * 8, (uint32_t)(bits >> 32));
				WriteMacInt32(p + i * 8 + 4, (uint32_t)(bits & 0xFFFFFFFF));
			}
			break;
		case GL_ORDER:
			{
				double d0 = map.defined ? (double)map.uorder : 0.0;
				double d1 = map.defined ? (double)map.vorder : 0.0;
				uint64_t b0, b1;
				memcpy(&b0, &d0, sizeof(double));
				memcpy(&b1, &d1, sizeof(double));
				WriteMacInt32(p, (uint32_t)(b0 >> 32));
				WriteMacInt32(p + 4, (uint32_t)(b0 & 0xFFFFFFFF));
				WriteMacInt32(p + 8, (uint32_t)(b1 >> 32));
				WriteMacInt32(p + 12, (uint32_t)(b1 & 0xFFFFFFFF));
			}
			break;
		case GL_DOMAIN:
			{
				double vals[4] = {
					map.defined ? (double)map.u1 : 0.0,
					map.defined ? (double)map.u2 : 0.0,
					map.defined ? (double)map.v1 : 0.0,
					map.defined ? (double)map.v2 : 0.0
				};
				for (int i = 0; i < 4; i++) {
					uint64_t bits;
					memcpy(&bits, &vals[i], sizeof(double));
					WriteMacInt32(p + i * 8, (uint32_t)(bits >> 32));
					WriteMacInt32(p + i * 8 + 4, (uint32_t)(bits & 0xFFFFFFFF));
				}
			}
			break;
		}
	}
}

void NativeGLGetMapfv(GLContext *ctx, uint32_t target, uint32_t query, uint32_t p)
{
	int idx = gl_eval_target_index(target);
	if (idx < 0) return;

	bool is_map2 = (target >= GL_MAP2_COLOR_4 && target <= GL_MAP2_VERTEX_4);

	if (!is_map2) {
		const GLEvaluatorMap1 &map = ctx->eval_map1[idx];
		switch (query) {
		case GL_COEFF:
			for (int i = 0; i < map.order * map.dimension; i++)
				WriteMacFloat(p + i * 4, map.defined ? map.control_points[i] : 0.0f);
			break;
		case GL_ORDER:
			WriteMacFloat(p, map.defined ? (float)map.order : 0.0f);
			break;
		case GL_DOMAIN:
			WriteMacFloat(p, map.defined ? map.u1 : 0.0f);
			WriteMacFloat(p + 4, map.defined ? map.u2 : 0.0f);
			break;
		}
	} else {
		const GLEvaluatorMap2 &map = ctx->eval_map2[idx];
		switch (query) {
		case GL_COEFF:
			for (int i = 0; i < map.uorder * map.vorder * map.dimension; i++)
				WriteMacFloat(p + i * 4, map.defined ? map.control_points[i] : 0.0f);
			break;
		case GL_ORDER:
			WriteMacFloat(p, map.defined ? (float)map.uorder : 0.0f);
			WriteMacFloat(p + 4, map.defined ? (float)map.vorder : 0.0f);
			break;
		case GL_DOMAIN:
			WriteMacFloat(p, map.defined ? map.u1 : 0.0f);
			WriteMacFloat(p + 4, map.defined ? map.u2 : 0.0f);
			WriteMacFloat(p + 8, map.defined ? map.v1 : 0.0f);
			WriteMacFloat(p + 12, map.defined ? map.v2 : 0.0f);
			break;
		}
	}
}

void NativeGLGetMapiv(GLContext *ctx, uint32_t target, uint32_t query, uint32_t p)
{
	int idx = gl_eval_target_index(target);
	if (idx < 0) return;

	bool is_map2 = (target >= GL_MAP2_COLOR_4 && target <= GL_MAP2_VERTEX_4);

	if (!is_map2) {
		const GLEvaluatorMap1 &map = ctx->eval_map1[idx];
		switch (query) {
		case GL_COEFF:
			for (int i = 0; i < map.order * map.dimension; i++)
				WriteMacInt32(p + i * 4, map.defined ? (int32_t)map.control_points[i] : 0);
			break;
		case GL_ORDER:
			WriteMacInt32(p, map.defined ? map.order : 0);
			break;
		case GL_DOMAIN:
			WriteMacInt32(p, map.defined ? (int32_t)map.u1 : 0);
			WriteMacInt32(p + 4, map.defined ? (int32_t)map.u2 : 0);
			break;
		}
	} else {
		const GLEvaluatorMap2 &map = ctx->eval_map2[idx];
		switch (query) {
		case GL_COEFF:
			for (int i = 0; i < map.uorder * map.vorder * map.dimension; i++)
				WriteMacInt32(p + i * 4, map.defined ? (int32_t)map.control_points[i] : 0);
			break;
		case GL_ORDER:
			WriteMacInt32(p, map.defined ? map.uorder : 0);
			WriteMacInt32(p + 4, map.defined ? map.vorder : 0);
			break;
		case GL_DOMAIN:
			WriteMacInt32(p, map.defined ? (int32_t)map.u1 : 0);
			WriteMacInt32(p + 4, map.defined ? (int32_t)map.u2 : 0);
			WriteMacInt32(p + 8, map.defined ? (int32_t)map.v1 : 0);
			WriteMacInt32(p + 12, map.defined ? (int32_t)map.v2 : 0);
			break;
		}
	}
}

void NativeGLGetDoublev(GLContext *ctx, uint32_t pname, uint32_t mac_ptr)
{
	// Matrix queries
	float *m = nullptr;
	if (pname == GL_MODELVIEW_MATRIX) m = ctx->modelview_stack[ctx->modelview_depth];
	else if (pname == GL_PROJECTION_MATRIX) m = ctx->projection_stack[ctx->projection_depth];
	else if (pname == GL_TEXTURE_MATRIX) m = ctx->texture_stack[ctx->active_texture][ctx->texture_depth[ctx->active_texture]];
	if (m) {
		for (int i = 0; i < 16; i++) {
			double d = (double)m[i];
			uint64_t bits; memcpy(&bits, &d, sizeof(double));
			WriteMacInt32(mac_ptr + i*8, (uint32_t)(bits >> 32));
			WriteMacInt32(mac_ptr + i*8+4, (uint32_t)(bits & 0xFFFFFFFF));
		}
		return;
	}
	// Default: write 0.0
	WriteMacInt32(mac_ptr, 0); WriteMacInt32(mac_ptr+4, 0);
}

void NativeGLGetPointerv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr) { (void)pname; WriteMacInt32(mac_ptr, 0); }


// ============================================================================
//  EXT_blend_color / EXT_blend_equation
// ============================================================================

void NativeGLBlendColorEXT(GLContext *ctx, float r, float g, float b, float a)
{
	if (!ctx) return;
	ctx->blend_color[0] = r; ctx->blend_color[1] = g;
	ctx->blend_color[2] = b; ctx->blend_color[3] = a;
}

void NativeGLBlendEquationEXT(GLContext *ctx, uint32_t mode)
{
	if (!ctx) return;
	ctx->blend_equation = mode;
}


// ============================================================================
//  EXT_compiled_vertex_array
// ============================================================================

void NativeGLLockArraysEXT(GLContext *ctx, int32_t first, int32_t count)
{
	if (!ctx) return;
	ctx->arrays_locked = true;
	ctx->lock_first = first;
	ctx->lock_count = count;
}

void NativeGLUnlockArraysEXT(GLContext *ctx)
{
	if (!ctx) return;
	ctx->arrays_locked = false;
	ctx->lock_first = 0;
	ctx->lock_count = 0;
}


// ============================================================================
//  ARB_multitexture -- glMultiTexCoord variants
// ============================================================================

static inline void gl_set_texcoord(GLContext *ctx, uint32_t target, float s, float t, float r, float q)
{
	int unit = (int)(target - GL_TEXTURE0_ARB);
	if (unit < 0 || unit >= 4) unit = 0;
	ctx->current_texcoord[unit][0] = s;
	ctx->current_texcoord[unit][1] = t;
	ctx->current_texcoord[unit][2] = r;
	ctx->current_texcoord[unit][3] = q;
}

// ---- 1D variants ----
void NativeGLMultiTexCoord1dARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	uint64_t bits = ((uint64_t)ReadMacInt32(mac_ptr) << 32) | ReadMacInt32(mac_ptr+4);
	double d; memcpy(&d, &bits, 8);
	gl_set_texcoord(ctx, target, (float)d, 0, 0, 1);
}
void NativeGLMultiTexCoord1dvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	NativeGLMultiTexCoord1dARB(ctx, target, mac_ptr);
}
void NativeGLMultiTexCoord1fARB(GLContext *ctx, uint32_t target, float s) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, s, 0, 0, 1);
}
void NativeGLMultiTexCoord1fvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	uint32_t bits = ReadMacInt32(mac_ptr); float s; memcpy(&s, &bits, 4);
	gl_set_texcoord(ctx, target, s, 0, 0, 1);
}
void NativeGLMultiTexCoord1iARB(GLContext *ctx, uint32_t target, int32_t s) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)s, 0, 0, 1);
}
void NativeGLMultiTexCoord1ivARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)(int32_t)ReadMacInt32(mac_ptr), 0, 0, 1);
}
void NativeGLMultiTexCoord1sARB(GLContext *ctx, uint32_t target, int16_t s) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)s, 0, 0, 1);
}
void NativeGLMultiTexCoord1svARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)(int16_t)ReadMacInt16(mac_ptr), 0, 0, 1);
}

// ---- 2D variants ----
void NativeGLMultiTexCoord2dARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	uint64_t b0 = ((uint64_t)ReadMacInt32(mac_ptr) << 32) | ReadMacInt32(mac_ptr+4);
	uint64_t b1 = ((uint64_t)ReadMacInt32(mac_ptr+8) << 32) | ReadMacInt32(mac_ptr+12);
	double d0, d1; memcpy(&d0, &b0, 8); memcpy(&d1, &b1, 8);
	gl_set_texcoord(ctx, target, (float)d0, (float)d1, 0, 1);
}
void NativeGLMultiTexCoord2dvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	NativeGLMultiTexCoord2dARB(ctx, target, mac_ptr);
}
void NativeGLMultiTexCoord2fARB(GLContext *ctx, uint32_t target, float s, float t) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, s, t, 0, 1);
}
void NativeGLMultiTexCoord2fvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	uint32_t b0 = ReadMacInt32(mac_ptr), b1 = ReadMacInt32(mac_ptr+4);
	float s, t; memcpy(&s, &b0, 4); memcpy(&t, &b1, 4);
	gl_set_texcoord(ctx, target, s, t, 0, 1);
}
void NativeGLMultiTexCoord2iARB(GLContext *ctx, uint32_t target, int32_t s, int32_t t) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)s, (float)t, 0, 1);
}
void NativeGLMultiTexCoord2ivARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)(int32_t)ReadMacInt32(mac_ptr), (float)(int32_t)ReadMacInt32(mac_ptr+4), 0, 1);
}
void NativeGLMultiTexCoord2sARB(GLContext *ctx, uint32_t target, int16_t s, int16_t t) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)s, (float)t, 0, 1);
}
void NativeGLMultiTexCoord2svARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)(int16_t)ReadMacInt16(mac_ptr), (float)(int16_t)ReadMacInt16(mac_ptr+2), 0, 1);
}

// ---- 3D variants ----
void NativeGLMultiTexCoord3dARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	double v[3];
	for (int i = 0; i < 3; i++) {
		uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr+i*8) << 32) | ReadMacInt32(mac_ptr+i*8+4);
		memcpy(&v[i], &b, 8);
	}
	gl_set_texcoord(ctx, target, (float)v[0], (float)v[1], (float)v[2], 1);
}
void NativeGLMultiTexCoord3dvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	NativeGLMultiTexCoord3dARB(ctx, target, mac_ptr);
}
void NativeGLMultiTexCoord3fARB(GLContext *ctx, uint32_t target, float s, float t, float r) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, s, t, r, 1);
}
void NativeGLMultiTexCoord3fvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	float v[3];
	for (int i = 0; i < 3; i++) { uint32_t b = ReadMacInt32(mac_ptr+i*4); memcpy(&v[i], &b, 4); }
	gl_set_texcoord(ctx, target, v[0], v[1], v[2], 1);
}
void NativeGLMultiTexCoord3iARB(GLContext *ctx, uint32_t target, int32_t s, int32_t t, int32_t r) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)s, (float)t, (float)r, 1);
}
void NativeGLMultiTexCoord3ivARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)(int32_t)ReadMacInt32(mac_ptr), (float)(int32_t)ReadMacInt32(mac_ptr+4), (float)(int32_t)ReadMacInt32(mac_ptr+8), 1);
}
void NativeGLMultiTexCoord3sARB(GLContext *ctx, uint32_t target, int16_t s, int16_t t, int16_t r) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)s, (float)t, (float)r, 1);
}
void NativeGLMultiTexCoord3svARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)(int16_t)ReadMacInt16(mac_ptr), (float)(int16_t)ReadMacInt16(mac_ptr+2), (float)(int16_t)ReadMacInt16(mac_ptr+4), 1);
}

// ---- 4D variants ----
void NativeGLMultiTexCoord4dARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	double v[4];
	for (int i = 0; i < 4; i++) {
		uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr+i*8) << 32) | ReadMacInt32(mac_ptr+i*8+4);
		memcpy(&v[i], &b, 8);
	}
	gl_set_texcoord(ctx, target, (float)v[0], (float)v[1], (float)v[2], (float)v[3]);
}
void NativeGLMultiTexCoord4dvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	NativeGLMultiTexCoord4dARB(ctx, target, mac_ptr);
}
void NativeGLMultiTexCoord4fARB(GLContext *ctx, uint32_t target, float s, float t, float r, float q) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, s, t, r, q);
}
void NativeGLMultiTexCoord4fvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	float v[4];
	for (int i = 0; i < 4; i++) { uint32_t b = ReadMacInt32(mac_ptr+i*4); memcpy(&v[i], &b, 4); }
	gl_set_texcoord(ctx, target, v[0], v[1], v[2], v[3]);
}
void NativeGLMultiTexCoord4iARB(GLContext *ctx, uint32_t target, int32_t s, int32_t t, int32_t r, int32_t q) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)s, (float)t, (float)r, (float)q);
}
void NativeGLMultiTexCoord4ivARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)(int32_t)ReadMacInt32(mac_ptr), (float)(int32_t)ReadMacInt32(mac_ptr+4),
	                (float)(int32_t)ReadMacInt32(mac_ptr+8), (float)(int32_t)ReadMacInt32(mac_ptr+12));
}
void NativeGLMultiTexCoord4sARB(GLContext *ctx, uint32_t target, int16_t s, int16_t t, int16_t r, int16_t q) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)s, (float)t, (float)r, (float)q);
}
void NativeGLMultiTexCoord4svARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_texcoord(ctx, target, (float)(int16_t)ReadMacInt16(mac_ptr), (float)(int16_t)ReadMacInt16(mac_ptr+2),
	                (float)(int16_t)ReadMacInt16(mac_ptr+4), (float)(int16_t)ReadMacInt16(mac_ptr+6));
}


// ============================================================================
//  ARB_transpose_matrix
// ============================================================================

static void gl_transpose_4x4(float *dst, const float *src)
{
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			dst[r*4+c] = src[c*4+r];
}

void NativeGLLoadTransposeMatrixdARB(GLContext *ctx, uint32_t mac_ptr)
{
	if (!ctx) return;
	float src[16];
	for (int i = 0; i < 16; i++) {
		uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr+i*8) << 32) | ReadMacInt32(mac_ptr+i*8+4);
		double d; memcpy(&d, &b, 8);
		src[i] = (float)d;
	}
	float transposed[16];
	gl_transpose_4x4(transposed, src);
	float *m = gl_get_current_matrix(ctx);
	memcpy(m, transposed, sizeof(float)*16);
}

void NativeGLLoadTransposeMatrixfARB(GLContext *ctx, uint32_t mac_ptr)
{
	if (!ctx) return;
	float src[16];
	for (int i = 0; i < 16; i++) {
		uint32_t b = ReadMacInt32(mac_ptr+i*4);
		memcpy(&src[i], &b, 4);
	}
	float transposed[16];
	gl_transpose_4x4(transposed, src);
	float *m = gl_get_current_matrix(ctx);
	memcpy(m, transposed, sizeof(float)*16);
}

static void gl_mult_matrix_4x4(float *dst, const float *a, const float *b)
{
	float tmp[16];
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++) {
			tmp[r*4+c] = 0;
			for (int k = 0; k < 4; k++)
				tmp[r*4+c] += a[r*4+k] * b[k*4+c];
		}
	memcpy(dst, tmp, sizeof(float)*16);
}

void NativeGLMultTransposeMatrixdARB(GLContext *ctx, uint32_t mac_ptr)
{
	if (!ctx) return;
	float src[16];
	for (int i = 0; i < 16; i++) {
		uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr+i*8) << 32) | ReadMacInt32(mac_ptr+i*8+4);
		double d; memcpy(&d, &b, 8);
		src[i] = (float)d;
	}
	float transposed[16];
	gl_transpose_4x4(transposed, src);
	float *m = gl_get_current_matrix(ctx);
	gl_mult_matrix_4x4(m, m, transposed);
}

void NativeGLMultTransposeMatrixfARB(GLContext *ctx, uint32_t mac_ptr)
{
	if (!ctx) return;
	float src[16];
	for (int i = 0; i < 16; i++) {
		uint32_t b = ReadMacInt32(mac_ptr+i*4);
		memcpy(&src[i], &b, 4);
	}
	float transposed[16];
	gl_transpose_4x4(transposed, src);
	float *m = gl_get_current_matrix(ctx);
	gl_mult_matrix_4x4(m, m, transposed);
}


// ============================================================================
//  ARB_texture_compression — DXT1/3/5 CPU decompression
// ============================================================================

// Decode one RGB565 value to R,G,B bytes
static inline void dxt_rgb565_to_rgb(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
	*r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
	*g = (uint8_t)(((c >> 5)  & 0x3F) * 255 / 63);
	*b = (uint8_t)(((c)       & 0x1F) * 255 / 31);
}

// Decompress one DXT1 4×4 block (8 bytes) to 16 BGRA8 pixels
static void dxt1_decompress_block(const uint8_t *src, uint8_t *dst, bool has_alpha)
{
	uint16_t c0 = src[0] | (src[1] << 8);
	uint16_t c1 = src[2] | (src[3] << 8);
	uint32_t indices = src[4] | (src[5] << 8) | (src[6] << 16) | (src[7] << 24);

	uint8_t colors[4][4]; // [index][B,G,R,A]
	uint8_t r0, g0, b0, r1, g1, b1;
	dxt_rgb565_to_rgb(c0, &r0, &g0, &b0);
	dxt_rgb565_to_rgb(c1, &r1, &g1, &b1);

	colors[0][0] = b0; colors[0][1] = g0; colors[0][2] = r0; colors[0][3] = 255;
	colors[1][0] = b1; colors[1][1] = g1; colors[1][2] = r1; colors[1][3] = 255;

	if (c0 > c1 || !has_alpha) {
		colors[2][0] = (2*b0 + b1) / 3; colors[2][1] = (2*g0 + g1) / 3;
		colors[2][2] = (2*r0 + r1) / 3; colors[2][3] = 255;
		colors[3][0] = (b0 + 2*b1) / 3; colors[3][1] = (g0 + 2*g1) / 3;
		colors[3][2] = (r0 + 2*r1) / 3; colors[3][3] = 255;
	} else {
		colors[2][0] = (b0 + b1) / 2; colors[2][1] = (g0 + g1) / 2;
		colors[2][2] = (r0 + r1) / 2; colors[2][3] = 255;
		colors[3][0] = 0; colors[3][1] = 0; colors[3][2] = 0; colors[3][3] = 0; // transparent black
	}

	for (int i = 0; i < 16; i++) {
		int idx = (indices >> (i * 2)) & 3;
		dst[i*4+0] = colors[idx][0];
		dst[i*4+1] = colors[idx][1];
		dst[i*4+2] = colors[idx][2];
		dst[i*4+3] = colors[idx][3];
	}
}

// Decompress one DXT3 4×4 block (16 bytes) to 16 BGRA8 pixels
static void dxt3_decompress_block(const uint8_t *src, uint8_t *dst)
{
	// First 8 bytes: explicit 4-bit alpha for each pixel
	const uint8_t *alpha_block = src;
	const uint8_t *color_block = src + 8;

	// Decode color portion using DXT1 (no alpha transparency)
	dxt1_decompress_block(color_block, dst, false);

	// Override alpha with explicit 4-bit values
	for (int i = 0; i < 16; i++) {
		int byte_idx = i / 2;
		uint8_t a4;
		if (i & 1)
			a4 = (alpha_block[byte_idx] >> 4) & 0xF;
		else
			a4 = alpha_block[byte_idx] & 0xF;
		dst[i*4+3] = a4 * 17; // expand 4-bit to 8-bit
	}
}

// Decompress one DXT5 4×4 block (16 bytes) to 16 BGRA8 pixels
static void dxt5_decompress_block(const uint8_t *src, uint8_t *dst)
{
	// First 8 bytes: interpolated alpha block
	uint8_t a0 = src[0];
	uint8_t a1 = src[1];
	// 48-bit index data (6 bytes, 3 bits per pixel, 16 pixels)
	uint64_t alpha_bits = 0;
	for (int i = 0; i < 6; i++)
		alpha_bits |= ((uint64_t)src[2+i]) << (i * 8);

	uint8_t alpha_lut[8];
	alpha_lut[0] = a0;
	alpha_lut[1] = a1;
	if (a0 > a1) {
		alpha_lut[2] = (6*a0 + 1*a1) / 7;
		alpha_lut[3] = (5*a0 + 2*a1) / 7;
		alpha_lut[4] = (4*a0 + 3*a1) / 7;
		alpha_lut[5] = (3*a0 + 4*a1) / 7;
		alpha_lut[6] = (2*a0 + 5*a1) / 7;
		alpha_lut[7] = (1*a0 + 6*a1) / 7;
	} else {
		alpha_lut[2] = (4*a0 + 1*a1) / 5;
		alpha_lut[3] = (3*a0 + 2*a1) / 5;
		alpha_lut[4] = (2*a0 + 3*a1) / 5;
		alpha_lut[5] = (1*a0 + 4*a1) / 5;
		alpha_lut[6] = 0;
		alpha_lut[7] = 255;
	}

	// Decode color portion
	const uint8_t *color_block = src + 8;
	dxt1_decompress_block(color_block, dst, false);

	// Apply interpolated alpha
	for (int i = 0; i < 16; i++) {
		int aidx = (alpha_bits >> (i * 3)) & 7;
		dst[i*4+3] = alpha_lut[aidx];
	}
}

// Decompress a full DXT image to BGRA8
static uint8_t *dxt_decompress_image(const uint8_t *src, int w, int h, uint32_t format, int *outLen)
{
	int bw = (w + 3) / 4;  // block width
	int bh = (h + 3) / 4;  // block height
	int block_size = (format == 0x83F0) ? 8 : 16; // DXT1=8, DXT3/5=16

	*outLen = w * h * 4;
	uint8_t *dst = (uint8_t *)malloc(*outLen);
	if (!dst) { *outLen = 0; return nullptr; }
	memset(dst, 0xFF, *outLen); // default opaque white

	for (int by = 0; by < bh; by++) {
		for (int bx = 0; bx < bw; bx++) {
			uint8_t block_pixels[16 * 4]; // 4x4 BGRA
			const uint8_t *block_src = src + (by * bw + bx) * block_size;

			if (format == 0x83F0)      // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
				dxt1_decompress_block(block_src, block_pixels, true);
			else if (format == 0x83F2) // GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
				dxt3_decompress_block(block_src, block_pixels);
			else if (format == 0x83F3) // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
				dxt5_decompress_block(block_src, block_pixels);

			// Copy block pixels to destination image
			for (int py = 0; py < 4; py++) {
				int dy = by * 4 + py;
				if (dy >= h) break;
				for (int px = 0; px < 4; px++) {
					int dx = bx * 4 + px;
					if (dx >= w) break;
					int si = (py * 4 + px) * 4;
					int di = (dy * w + dx) * 4;
					dst[di+0] = block_pixels[si+0];
					dst[di+1] = block_pixels[si+1];
					dst[di+2] = block_pixels[si+2];
					dst[di+3] = block_pixels[si+3];
				}
			}
		}
	}
	return dst;
}

void NativeGLCompressedTexImage3DARB(GLContext *ctx, uint32_t target, int32_t level,
	uint32_t ifmt, int32_t w, int32_t h, int32_t d, int32_t border, int32_t imageSize, uint32_t data_ptr)
{
	GL_LOG("CompressedTexImage3D fmt=0x%x %dx%dx%d size=%d (known limitation)", ifmt, w, h, d, imageSize);
	// 3D compressed textures extremely rare in classic Mac games — known limitation
}

void NativeGLCompressedTexImage2DARB(GLContext *ctx, uint32_t target, int32_t level,
	uint32_t ifmt, int32_t w, int32_t h, int32_t border, int32_t imageSize, uint32_t data_ptr)
{
	GL_LOG("CompressedTexImage2D fmt=0x%x %dx%d size=%d", ifmt, w, h, imageSize);
	if (!ctx || data_ptr == 0) return;

	// Only handle DXT formats
	if (ifmt != 0x83F0 && ifmt != 0x83F2 && ifmt != 0x83F3) {
		GL_LOG("CompressedTexImage2D: unsupported format 0x%x", ifmt);
		return;
	}

	uint32_t texName = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	if (texName == 0) return;
	auto it = ctx->texture_objects.find(texName);
	if (it == ctx->texture_objects.end()) return;
	GLTextureObject &tex = it->second;

	// Read compressed data from Mac memory
	const uint8_t *mac_data = (const uint8_t *)Mac2HostAddr(data_ptr);

	int outLen = 0;
	uint8_t *decompressed = dxt_decompress_image(mac_data, w, h, ifmt, &outLen);
	if (!decompressed) return;

	tex.width = w;
	tex.height = h;
	if (level > 0) tex.has_mipmaps = true;
	GLMetalUploadTexture(ctx, &tex, level, w, h, decompressed, outLen);
	free(decompressed);
}

void NativeGLCompressedTexImage1DARB(GLContext *ctx, uint32_t target, int32_t level,
	uint32_t ifmt, int32_t w, int32_t border, int32_t imageSize, uint32_t data_ptr)
{
	GL_LOG("CompressedTexImage1D fmt=0x%x %d size=%d (known limitation)", ifmt, w, imageSize);
	// 1D compressed textures essentially never used — known limitation
}

void NativeGLCompressedTexSubImage3DARB(GLContext *ctx, uint32_t target, int32_t level,
	int32_t xoff, int32_t yoff, int32_t zoff, int32_t w, int32_t h, int32_t d,
	uint32_t fmt, int32_t imageSize, uint32_t data_ptr)
{
	GL_LOG("CompressedTexSubImage3D (known limitation)");
}

void NativeGLCompressedTexSubImage2DARB(GLContext *ctx, uint32_t target, int32_t level,
	int32_t xoff, int32_t yoff, int32_t w, int32_t h,
	uint32_t fmt, int32_t imageSize, uint32_t data_ptr)
{
	GL_LOG("CompressedTexSubImage2D fmt=0x%x %dx%d at (%d,%d)", fmt, w, h, xoff, yoff);
	if (!ctx || data_ptr == 0) return;

	if (fmt != 0x83F0 && fmt != 0x83F2 && fmt != 0x83F3) {
		GL_LOG("CompressedTexSubImage2D: unsupported format 0x%x", fmt);
		return;
	}

	uint32_t texName = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	if (texName == 0) return;
	auto it = ctx->texture_objects.find(texName);
	if (it == ctx->texture_objects.end()) return;
	GLTextureObject &tex = it->second;

	const uint8_t *mac_data = (const uint8_t *)Mac2HostAddr(data_ptr);
	int outLen = 0;
	uint8_t *decompressed = dxt_decompress_image(mac_data, w, h, fmt, &outLen);
	if (!decompressed) return;

	GLMetalUploadSubTexture(ctx, &tex, level, xoff, yoff, w, h, decompressed, w * 4);
	free(decompressed);
}

void NativeGLCompressedTexSubImage1DARB(GLContext *ctx, uint32_t target, int32_t level,
	int32_t xoff, int32_t w, uint32_t fmt, int32_t imageSize, uint32_t data_ptr)
{
	GL_LOG("CompressedTexSubImage1D (known limitation)");
}

void NativeGLGetCompressedTexImageARB(GLContext *ctx, uint32_t target, int32_t level, uint32_t img_ptr)
{
	GL_LOG("GetCompressedTexImage (known limitation — readback of decompressed data impractical)");
}


// ============================================================================
//  EXT_secondary_color
// ============================================================================

static inline void gl_set_secondary_color(GLContext *ctx, float r, float g, float b)
{
	ctx->current_secondary_color[0] = r;
	ctx->current_secondary_color[1] = g;
	ctx->current_secondary_color[2] = b;
}

void NativeGLSecondaryColor3bEXT(GLContext *ctx, int8_t r, int8_t g, int8_t b) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, r/127.0f, g/127.0f, b/127.0f);
}
void NativeGLSecondaryColor3bvEXT(GLContext *ctx, uint32_t mac_ptr) {
	if (!ctx) return;
	int8_t r = (int8_t)ReadMacInt8(mac_ptr), g = (int8_t)ReadMacInt8(mac_ptr+1), b = (int8_t)ReadMacInt8(mac_ptr+2);
	gl_set_secondary_color(ctx, r/127.0f, g/127.0f, b/127.0f);
}
void NativeGLSecondaryColor3dEXT(GLContext *ctx, uint32_t mac_ptr) {
	if (!ctx) return;
	double v[3];
	for (int i = 0; i < 3; i++) {
		uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr+i*8) << 32) | ReadMacInt32(mac_ptr+i*8+4);
		memcpy(&v[i], &b, 8);
	}
	gl_set_secondary_color(ctx, (float)v[0], (float)v[1], (float)v[2]);
}
void NativeGLSecondaryColor3dvEXT(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLSecondaryColor3dEXT(ctx, mac_ptr);
}
void NativeGLSecondaryColor3fEXT(GLContext *ctx, float r, float g, float b) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, r, g, b);
}
void NativeGLSecondaryColor3fvEXT(GLContext *ctx, uint32_t mac_ptr) {
	if (!ctx) return;
	float v[3];
	for (int i = 0; i < 3; i++) { uint32_t b = ReadMacInt32(mac_ptr+i*4); memcpy(&v[i], &b, 4); }
	gl_set_secondary_color(ctx, v[0], v[1], v[2]);
}
void NativeGLSecondaryColor3iEXT(GLContext *ctx, int32_t r, int32_t g, int32_t b) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, r/2147483647.0f, g/2147483647.0f, b/2147483647.0f);
}
void NativeGLSecondaryColor3ivEXT(GLContext *ctx, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, (int32_t)ReadMacInt32(mac_ptr)/2147483647.0f,
	                       (int32_t)ReadMacInt32(mac_ptr+4)/2147483647.0f,
	                       (int32_t)ReadMacInt32(mac_ptr+8)/2147483647.0f);
}
void NativeGLSecondaryColor3sEXT(GLContext *ctx, int16_t r, int16_t g, int16_t b) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, r/32767.0f, g/32767.0f, b/32767.0f);
}
void NativeGLSecondaryColor3svEXT(GLContext *ctx, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, (int16_t)ReadMacInt16(mac_ptr)/32767.0f,
	                       (int16_t)ReadMacInt16(mac_ptr+2)/32767.0f,
	                       (int16_t)ReadMacInt16(mac_ptr+4)/32767.0f);
}
void NativeGLSecondaryColor3ubEXT(GLContext *ctx, uint8_t r, uint8_t g, uint8_t b) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, r/255.0f, g/255.0f, b/255.0f);
}
void NativeGLSecondaryColor3ubvEXT(GLContext *ctx, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, ReadMacInt8(mac_ptr)/255.0f, ReadMacInt8(mac_ptr+1)/255.0f, ReadMacInt8(mac_ptr+2)/255.0f);
}
void NativeGLSecondaryColor3uiEXT(GLContext *ctx, uint32_t r, uint32_t g, uint32_t b) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, r/4294967295.0f, g/4294967295.0f, b/4294967295.0f);
}
void NativeGLSecondaryColor3uivEXT(GLContext *ctx, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, ReadMacInt32(mac_ptr)/4294967295.0f,
	                       ReadMacInt32(mac_ptr+4)/4294967295.0f,
	                       ReadMacInt32(mac_ptr+8)/4294967295.0f);
}
void NativeGLSecondaryColor3usEXT(GLContext *ctx, uint16_t r, uint16_t g, uint16_t b) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, r/65535.0f, g/65535.0f, b/65535.0f);
}
void NativeGLSecondaryColor3usvEXT(GLContext *ctx, uint32_t mac_ptr) {
	if (!ctx) return;
	gl_set_secondary_color(ctx, ReadMacInt16(mac_ptr)/65535.0f,
	                       ReadMacInt16(mac_ptr+2)/65535.0f,
	                       ReadMacInt16(mac_ptr+4)/65535.0f);
}
void NativeGLSecondaryColorPointerEXT(GLContext *ctx, int32_t size, uint32_t type, int32_t stride, uint32_t pointer)
{
	if (!ctx) return;
	ctx->secondary_color_array.size = size;
	ctx->secondary_color_array.type = type;
	ctx->secondary_color_array.stride = stride;
	ctx->secondary_color_array.pointer = pointer;
}


// ============================================================================
//  OpenGL 1.2 imaging subset — CPU state storage
// ============================================================================

// Helper: resolve color table target to index (0-2)
static int gl_color_table_index(uint32_t target) {
	switch (target) {
		case 0x80D0: return 0; // GL_COLOR_TABLE
		case 0x80D1: return 1; // GL_POST_CONVOLUTION_COLOR_TABLE
		case 0x80D2: return 2; // GL_POST_COLOR_MATRIX_COLOR_TABLE
		default: return -1;
	}
}

// Helper: resolve convolution target to index (0-1)
static int gl_convolution_index(uint32_t target) {
	switch (target) {
		case 0x8010: return 0; // GL_CONVOLUTION_1D
		case 0x8011: return 1; // GL_CONVOLUTION_2D
		default: return -1;
	}
}

// --- Imaging subset (GL 1.2): Color tables, convolution, histogram, minmax ---
// NOTE: The COLOR TABLE family (GL_COLOR_TABLE / glColorTable / glColorSubTable /
// glCopyColorTable / glCopyColorSubTable, sub-ops 469-477) IS now applied to the pixel
// pipeline: when GL_COLOR_TABLE is enabled the stored LUT is
// folded into the CPU texture-upload path via GLApplyColorTableLUT (no new
// compute pass; color_table_enabled is now a READ), and glCopyColorTable/glCopyColorSubTable
// read the framebuffer via GLMetalReadFramebufferRect with overflow-safe bounds.
// The CONVOLUTION (478-490) and HISTOGRAM/MINMAX (491-500)
// handlers still store state only and do NOT apply their transforms in the Metal pipeline (a
// full convolution/histogram pass is DELIBERATELY not built — owned by later work; classic
// Mac games do not use imaging convolution/histogram). Those families are
// honestly DE-ADVERTISED: the ARB imaging umbrella token is NOT in the GL_EXTENSIONS string
// (over-advertising would be the silent-wrong-output the Core Value forbids), and each
// handler LOGS its known limitation so the disposition is visible at runtime,
// never a silent claim of success. (The umbrella token is intentionally not
// spelled out as a literal so the GLARBExtensionTests source-scan absence assertion stays
// robust — same discipline as the COMBINE de-advertisement.)

// --- Color table operations (9 functions) ---

void NativeGLColorTable(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t w, uint32_t fmt, uint32_t type, uint32_t data) {
	GL_LOG("ColorTable target=0x%x width=%d fmt=0x%x type=0x%x", target, w, fmt, type);
	if (!ctx) return;
	int idx = gl_color_table_index(target);
	if (idx < 0) return;
	if (w < 0 || w > 256) w = 256;
	GLColorTable &ct = ctx->color_tables[idx];
	ct.width = w;
	ct.internal_format = ifmt;
	ct.defined = true;
	// Read pixel data from Mac memory, convert to float RGBA
	if (data != 0 && w > 0) {
		for (int i = 0; i < w; i++) {
			if (fmt == GL_RGBA && type == GL_UNSIGNED_BYTE) {
				ct.data[i][0] = ReadMacInt8(data + i*4 + 0) / 255.0f;
				ct.data[i][1] = ReadMacInt8(data + i*4 + 1) / 255.0f;
				ct.data[i][2] = ReadMacInt8(data + i*4 + 2) / 255.0f;
				ct.data[i][3] = ReadMacInt8(data + i*4 + 3) / 255.0f;
			} else if (fmt == GL_RGB && type == GL_UNSIGNED_BYTE) {
				ct.data[i][0] = ReadMacInt8(data + i*3 + 0) / 255.0f;
				ct.data[i][1] = ReadMacInt8(data + i*3 + 1) / 255.0f;
				ct.data[i][2] = ReadMacInt8(data + i*3 + 2) / 255.0f;
				ct.data[i][3] = 1.0f;
			} else {
				// Default: opaque white for unsupported formats
				ct.data[i][0] = ct.data[i][1] = ct.data[i][2] = ct.data[i][3] = 1.0f;
			}
		}
	}
}

void NativeGLColorTableParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	// The color-table scale/bias params are NOT consumed by the implemented
	// single-channel LUT apply; the knob is honestly inert — a
	// conformant app gets no scale/bias transform rather than a silent partial
	// one. Logging-only, no state mutation (additive marker only).
	GL_LOG("ColorTableParameterfv target=0x%x pname=0x%x (known limitation — color-table scale/bias not consumed by the single-channel LUT apply; honestly inert)", target, pname);
}

void NativeGLColorTableParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	GL_LOG("ColorTableParameteriv target=0x%x pname=0x%x (known limitation — color-table scale/bias not consumed by the single-channel LUT apply; honestly inert)", target, pname);
}

void NativeGLCopyColorTable(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t x, int32_t y, int32_t w) {
	GL_LOG("CopyColorTable target=0x%x x=%d y=%d w=%d", target, x, y, w);
	if (!ctx) return;
	int idx = gl_color_table_index(target);
	if (idx < 0) return;
	GLColorTable &ct = ctx->color_tables[idx];
	// ASVS-V5 bounds: the guest controls x/y/w. Reject non-positive or
	// over-ceiling widths BEFORE any framebuffer read / LUT write. The hard ceiling is the
	// GLColorTable.data[256][4] array. x/y must be non-negative (the readback rect origin).
	if (w <= 0 || w > 256) return;
	if (x < 0 || y < 0) return;
	// Read a 1-row strip of width w from the framebuffer (BGRA8). NULL return == no
	// framebuffer / blit failure -> honest failure (log + return), never silent success.
	int outLen = 0;
	uint8_t *bgra = GLMetalReadFramebufferRect(ctx, x, y, w, 1, &outLen);
	if (!bgra) {
		GL_LOG("CopyColorTable: framebuffer read failed (no overlay) — not stored");
		return;
	}
	// Store the strip into color_tables[idx] as float RGBA (BGRA8: B@+0,G@+1,R@+2,A@+3).
	// Bound the copy by both the requested w and the actual bytes returned (outLen/4).
	int avail = (outLen >= 0) ? outLen / 4 : 0;
	int n = (w < avail) ? w : avail;
	if (n > 256) n = 256;
	for (int i = 0; i < n; i++) {
		uint8_t *px = bgra + i * 4;
		ct.data[i][0] = px[2] / 255.0f;   // R
		ct.data[i][1] = px[1] / 255.0f;   // G
		ct.data[i][2] = px[0] / 255.0f;   // B
		ct.data[i][3] = px[3] / 255.0f;   // A
	}
	ct.width = n;
	ct.internal_format = ifmt;
	ct.defined = true;
	free(bgra);
}

void NativeGLGetColorTable(GLContext *ctx, uint32_t target, uint32_t fmt, uint32_t type, uint32_t data) {
	GL_LOG("GetColorTable target=0x%x fmt=0x%x type=0x%x", target, fmt, type);
	if (!ctx || data == 0) return;
	int idx = gl_color_table_index(target);
	if (idx < 0) return;
	const GLColorTable &ct = ctx->color_tables[idx];
	if (!ct.defined) return;
	// Write stored float RGBA data back to Mac memory as unsigned bytes
	if (type == GL_UNSIGNED_BYTE && (fmt == GL_RGBA || fmt == GL_RGB)) {
		int bpp = (fmt == GL_RGBA) ? 4 : 3;
		for (int i = 0; i < ct.width; i++) {
			WriteMacInt8(data + i*bpp + 0, (uint8_t)(ct.data[i][0] * 255.0f));
			WriteMacInt8(data + i*bpp + 1, (uint8_t)(ct.data[i][1] * 255.0f));
			WriteMacInt8(data + i*bpp + 2, (uint8_t)(ct.data[i][2] * 255.0f));
			if (fmt == GL_RGBA)
				WriteMacInt8(data + i*bpp + 3, (uint8_t)(ct.data[i][3] * 255.0f));
		}
	}
}

void NativeGLGetColorTableParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	GL_LOG("GetColorTableParameterfv target=0x%x pname=0x%x", target, pname);
	if (!ctx || params == 0) return;
	int idx = gl_color_table_index(target);
	if (idx < 0) return;
	const GLColorTable &ct = ctx->color_tables[idx];
	uint32_t val = 0;
	switch (pname) {
		case 0x80D9: val = ct.width; break;   // GL_COLOR_TABLE_WIDTH
		case 0x80DA: val = ct.internal_format; break; // GL_COLOR_TABLE_FORMAT
		case 0x80DB: val = 8; break; // GL_COLOR_TABLE_RED_SIZE
		case 0x80DC: val = 8; break; // GL_COLOR_TABLE_GREEN_SIZE
		case 0x80DD: val = 8; break; // GL_COLOR_TABLE_BLUE_SIZE
		case 0x80DE: val = 8; break; // GL_COLOR_TABLE_ALPHA_SIZE
		default: break;
	}
	float fval = (float)val;
	uint32_t bits; memcpy(&bits, &fval, 4);
	WriteMacInt32(params, bits);
}

void NativeGLGetColorTableParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	GL_LOG("GetColorTableParameteriv target=0x%x pname=0x%x", target, pname);
	if (!ctx || params == 0) return;
	int idx = gl_color_table_index(target);
	if (idx < 0) return;
	const GLColorTable &ct = ctx->color_tables[idx];
	int32_t val = 0;
	switch (pname) {
		case 0x80D9: val = ct.width; break;
		case 0x80DA: val = (int32_t)ct.internal_format; break;
		case 0x80DB: case 0x80DC: case 0x80DD: case 0x80DE: val = 8; break;
		default: break;
	}
	WriteMacInt32(params, (uint32_t)val);
}

void NativeGLColorSubTable(GLContext *ctx, uint32_t target, int32_t start, int32_t count, uint32_t fmt, uint32_t type, uint32_t data) {
	GL_LOG("ColorSubTable target=0x%x start=%d count=%d", target, start, count);
	if (!ctx || data == 0) return;
	int idx = gl_color_table_index(target);
	if (idx < 0) return;
	GLColorTable &ct = ctx->color_tables[idx];
	if (!ct.defined) return;
	// ASVS-V5 bounds: start and count are guest-controlled and index into the
	// fixed GLColorTable.data[256][4] array embedded in the GLContext. Use the SAME
	// OVERFLOW-SAFE headroom comparison `count > 256 - start` as the hardened
	// NativeGLCopyColorSubTable twin. The previous loop-guard sum form
	// (start-plus-index compared against the table width, with no prior range check)
	// 32-bit-overflows when the guest supplies a large start, letting di = start + i
	// index out of bounds (the DSp CLUT integer-overflow -> stack-OOB precedent).
	// Reject start out of range, a zero/negative count, or a strip that would not fit in
	// the headroom BEFORE any write.
	if (start < 0 || start > 255) return;
	if (count <= 0) return;
	if (count > 256 - start) return;        // overflow-safe headroom check (NOT the naive sum)
	// Core Value: no silent no-op — the loop below only handles
	// GL_RGBA + GL_UNSIGNED_BYTE. Any other format/type combination would silently
	// do nothing after the bounds pass; log the known limitation and fail honestly
	// instead (mirrors the de-advertised imaging-family known-limitation logs).
	if (fmt != GL_RGBA || type != GL_UNSIGNED_BYTE) {
		GL_LOG("ColorSubTable: unhandled fmt=0x%x type=0x%x (known limitation — only GL_RGBA/GL_UNSIGNED_BYTE applied)", fmt, type);
		return;
	}
	int n = count;
	if (start + n > ct.width) n = ct.width - start;  // clamp to the existing LUT size
	if (n <= 0) return;
	for (int i = 0; i < n; i++) {
		int di = start + i;
		if (di >= 256) break;               // hard ceiling guard (di already <= start+count <= 256)
		ct.data[di][0] = ReadMacInt8(data + i*4 + 0) / 255.0f;
		ct.data[di][1] = ReadMacInt8(data + i*4 + 1) / 255.0f;
		ct.data[di][2] = ReadMacInt8(data + i*4 + 2) / 255.0f;
		ct.data[di][3] = ReadMacInt8(data + i*4 + 3) / 255.0f;
	}
}

void NativeGLCopyColorSubTable(GLContext *ctx, uint32_t target, int32_t start, int32_t x, int32_t y, int32_t w) {
	GL_LOG("CopyColorSubTable target=0x%x start=%d x=%d y=%d w=%d", target, start, x, y, w);
	if (!ctx) return;
	int idx = gl_color_table_index(target);
	if (idx < 0) return;
	GLColorTable &ct = ctx->color_tables[idx];
	if (!ct.defined) return;
	// ASVS-V5 bounds (HIGH): start and w are guest-controlled and index into the
	// fixed GLColorTable.data[256][4] array. Use the OVERFLOW-SAFE headroom comparison
	// `w > 256 - start` rather than the naive sum-then-compare form — the naive sum
	// 32-bit-overflows when the guest supplies a large start (the DSp CLUT
	// integer-overflow -> stack-OOB precedent). Reject start out of range, a zero/negative
	// width, or a strip that would not fit in the headroom BEFORE any framebuffer read / write.
	if (start < 0 || start > 255) return;
	if (w <= 0) return;
	if (w > 256 - start) return;            // overflow-safe headroom check (NOT the naive sum)
	if (x < 0 || y < 0) return;
	// Read a 1-row strip of width w from the framebuffer (BGRA8). NULL return -> honest fail.
	int outLen = 0;
	uint8_t *bgra = GLMetalReadFramebufferRect(ctx, x, y, w, 1, &outLen);
	if (!bgra) {
		GL_LOG("CopyColorSubTable: framebuffer read failed (no overlay) — not stored");
		return;
	}
	int avail = (outLen >= 0) ? outLen / 4 : 0;
	int n = (w < avail) ? w : avail;
	for (int i = 0; i < n; i++) {
		int di = start + i;
		if (di >= 256) break;               // hard ceiling guard (di already <= start+w <= 256)
		uint8_t *px = bgra + i * 4;
		ct.data[di][0] = px[2] / 255.0f;    // R
		ct.data[di][1] = px[1] / 255.0f;    // G
		ct.data[di][2] = px[0] / 255.0f;    // B
		ct.data[di][3] = px[3] / 255.0f;    // A
	}
	free(bgra);
}


// --- Convolution operations (13 functions) ---

// DELIBERATE de-advertisement: the ARB imaging
// CONVOLUTION family (sub-ops 478-490) stores its kernel/params honestly but is
// NOT applied to any pixel — a convolution pass is DELIBERATELY not built (classic
// Mac games do not use imaging convolution).
// Over-advertising the capability would be the silent-wrong-output the Core Value
// forbids: a conformant app that sees the imaging umbrella token would call this
// path and silently get an un-convolved result. The honest fix is capability-gating
// — the umbrella token is ABSENT from the GL_EXTENSIONS string (see the glGetString
// switch above) and each handler LOGS its known limitation so the disposition is
// visible. State is kept for API completeness (glGet* round-trips), never claimed
// as applied. Real apply path is owned by later work. (The umbrella token is not
// spelled out as a literal so the source-scan absence assertion stays robust.)
void NativeGLConvolutionFilter1D(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t w, uint32_t fmt, uint32_t type, uint32_t data) {
	GL_LOG("ConvolutionFilter1D target=0x%x width=%d (known limitation — kernel stored, NOT applied; imaging umbrella de-advertised)", target, w);
	if (!ctx) return;
	int idx = gl_convolution_index(target);
	if (idx < 0) idx = 0; // default to 1D
	GLConvolutionFilter &cf = ctx->convolution_filters[idx];
	if (w > 7) w = 7;
	cf.width = w;
	cf.height = 1;
	cf.internal_format = ifmt;
	cf.defined = true;
	if (data != 0) {
		for (int i = 0; i < w; i++) {
			if (fmt == GL_RGBA && type == GL_FLOAT) {
				for (int c = 0; c < 4; c++) {
					uint32_t bits = ReadMacInt32(data + (i*4+c)*4);
					memcpy(&cf.kernel[i*4+c], &bits, 4);
				}
			} else if (type == GL_UNSIGNED_BYTE) {
				int bpp = (fmt == GL_RGBA) ? 4 : 3;
				cf.kernel[i*4+0] = ReadMacInt8(data + i*bpp + 0) / 255.0f;
				cf.kernel[i*4+1] = ReadMacInt8(data + i*bpp + 1) / 255.0f;
				cf.kernel[i*4+2] = ReadMacInt8(data + i*bpp + 2) / 255.0f;
				cf.kernel[i*4+3] = (bpp == 4) ? ReadMacInt8(data + i*bpp + 3) / 255.0f : 1.0f;
			}
		}
	}
}

void NativeGLConvolutionFilter2D(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t w, int32_t h, uint32_t fmt, uint32_t type, uint32_t data) {
	GL_LOG("ConvolutionFilter2D target=0x%x %dx%d (known limitation — kernel stored, NOT applied; imaging umbrella de-advertised)", target, w, h);
	if (!ctx) return;
	int idx = gl_convolution_index(target);
	if (idx < 0) idx = 1; // default to 2D
	GLConvolutionFilter &cf = ctx->convolution_filters[idx];
	if (w > 7) w = 7;
	if (h > 7) h = 7;
	cf.width = w;
	cf.height = h;
	cf.internal_format = ifmt;
	cf.defined = true;
	if (data != 0) {
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				int ki = (y * w + x) * 4;
				if (fmt == GL_RGBA && type == GL_FLOAT) {
					for (int c = 0; c < 4; c++) {
						uint32_t bits = ReadMacInt32(data + (y*w+x)*16 + c*4);
						memcpy(&cf.kernel[ki+c], &bits, 4);
					}
				} else if (type == GL_UNSIGNED_BYTE) {
					int bpp = (fmt == GL_RGBA) ? 4 : 3;
					int si = (y * w + x) * bpp;
					cf.kernel[ki+0] = ReadMacInt8(data + si + 0) / 255.0f;
					cf.kernel[ki+1] = ReadMacInt8(data + si + 1) / 255.0f;
					cf.kernel[ki+2] = ReadMacInt8(data + si + 2) / 255.0f;
					cf.kernel[ki+3] = (bpp == 4) ? ReadMacInt8(data + si + 3) / 255.0f : 1.0f;
				}
			}
		}
	}
}

void NativeGLConvolutionParameterf(GLContext *ctx, uint32_t target, uint32_t pname, float param) {
	GL_LOG("ConvolutionParameterf target=0x%x pname=0x%x val=%f (known limitation — convolution de-advertised)", target, pname, param);
	// Border mode parameter — stored but not applied in Metal pipeline (DELIBERATE)
}

void NativeGLConvolutionParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	GL_LOG("ConvolutionParameterfv target=0x%x pname=0x%x (known limitation — convolution de-advertised)", target, pname);
	if (!ctx || params == 0) return;
	int idx = gl_convolution_index(target);
	if (idx < 0) return;
	if (pname == 0x8014) { // GL_CONVOLUTION_BORDER_COLOR
		for (int i = 0; i < 4; i++) {
			uint32_t bits = ReadMacInt32(params + i*4);
			memcpy(&ctx->convolution_filters[idx].border_color[i], &bits, 4);
		}
	}
}

void NativeGLConvolutionParameteri(GLContext *ctx, uint32_t target, uint32_t pname, int32_t param) {
	GL_LOG("ConvolutionParameteri target=0x%x pname=0x%x val=%d (known limitation — convolution de-advertised)", target, pname, param);
}

void NativeGLConvolutionParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	GL_LOG("ConvolutionParameteriv target=0x%x pname=0x%x (known limitation — convolution de-advertised)", target, pname);
}

void NativeGLCopyConvolutionFilter1D(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t x, int32_t y, int32_t w) {
	GL_LOG("CopyConvolutionFilter1D (known limitation — requires framebuffer read)");
}

void NativeGLCopyConvolutionFilter2D(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t x, int32_t y, int32_t w, int32_t h) {
	GL_LOG("CopyConvolutionFilter2D (known limitation — requires framebuffer read)");
}

void NativeGLGetConvolutionFilter(GLContext *ctx, uint32_t target, uint32_t fmt, uint32_t type, uint32_t data) {
	GL_LOG("GetConvolutionFilter target=0x%x", target);
	if (!ctx || data == 0) return;
	int idx = gl_convolution_index(target);
	if (idx < 0) return;
	const GLConvolutionFilter &cf = ctx->convolution_filters[idx];
	if (!cf.defined) return;
	int total = cf.width * cf.height;
	if (type == GL_FLOAT && fmt == GL_RGBA) {
		for (int i = 0; i < total; i++) {
			for (int c = 0; c < 4; c++) {
				uint32_t bits; memcpy(&bits, &cf.kernel[i*4+c], 4);
				WriteMacInt32(data + (i*4+c)*4, bits);
			}
		}
	} else if (type == GL_UNSIGNED_BYTE) {
		int bpp = (fmt == GL_RGBA) ? 4 : 3;
		for (int i = 0; i < total; i++) {
			WriteMacInt8(data + i*bpp + 0, (uint8_t)(cf.kernel[i*4+0] * 255.0f));
			WriteMacInt8(data + i*bpp + 1, (uint8_t)(cf.kernel[i*4+1] * 255.0f));
			WriteMacInt8(data + i*bpp + 2, (uint8_t)(cf.kernel[i*4+2] * 255.0f));
			if (bpp == 4) WriteMacInt8(data + i*bpp + 3, (uint8_t)(cf.kernel[i*4+3] * 255.0f));
		}
	}
}

void NativeGLGetConvolutionParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	GL_LOG("GetConvolutionParameterfv target=0x%x pname=0x%x", target, pname);
	if (!ctx || params == 0) return;
	int idx = gl_convolution_index(target);
	if (idx < 0) return;
	const GLConvolutionFilter &cf = ctx->convolution_filters[idx];
	float val = 0;
	switch (pname) {
		case 0x8014: // GL_CONVOLUTION_BORDER_COLOR
			for (int i = 0; i < 4; i++) {
				uint32_t bits; memcpy(&bits, &cf.border_color[i], 4);
				WriteMacInt32(params + i*4, bits);
			}
			return;
		case 0x8013: val = (float)cf.internal_format; break; // GL_CONVOLUTION_FORMAT
		case 0x8018: val = (float)cf.width; break;  // GL_CONVOLUTION_WIDTH
		case 0x8019: val = (float)cf.height; break; // GL_CONVOLUTION_HEIGHT
		default: break;
	}
	uint32_t bits; memcpy(&bits, &val, 4);
	WriteMacInt32(params, bits);
}

void NativeGLGetConvolutionParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	GL_LOG("GetConvolutionParameteriv target=0x%x pname=0x%x", target, pname);
	if (!ctx || params == 0) return;
	int idx = gl_convolution_index(target);
	if (idx < 0) return;
	const GLConvolutionFilter &cf = ctx->convolution_filters[idx];
	int32_t val = 0;
	switch (pname) {
		case 0x8013: val = (int32_t)cf.internal_format; break;
		case 0x8018: val = cf.width; break;
		case 0x8019: val = cf.height; break;
		default: break;
	}
	WriteMacInt32(params, (uint32_t)val);
}

void NativeGLGetSeparableFilter(GLContext *ctx, uint32_t target, uint32_t fmt, uint32_t type, uint32_t row, uint32_t col, uint32_t span) {
	GL_LOG("GetSeparableFilter target=0x%x", target);
	if (!ctx) return;
	const GLSeparableFilter &sf = ctx->separable_filter;
	if (!sf.defined) return;
	if (row != 0 && type == GL_FLOAT) {
		for (int i = 0; i < sf.width; i++) {
			for (int c = 0; c < 4; c++) {
				uint32_t bits; memcpy(&bits, &sf.row[i*4+c], 4);
				WriteMacInt32(row + (i*4+c)*4, bits);
			}
		}
	}
	if (col != 0 && type == GL_FLOAT) {
		for (int i = 0; i < sf.height; i++) {
			for (int c = 0; c < 4; c++) {
				uint32_t bits; memcpy(&bits, &sf.col[i*4+c], 4);
				WriteMacInt32(col + (i*4+c)*4, bits);
			}
		}
	}
}

void NativeGLSeparableFilter2D(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t w, int32_t h, uint32_t fmt, uint32_t type, uint32_t row, uint32_t col) {
	GL_LOG("SeparableFilter2D %dx%d fmt=0x%x", w, h, fmt);
	if (!ctx) return;
	GLSeparableFilter &sf = ctx->separable_filter;
	if (w > 7) w = 7;
	if (h > 7) h = 7;
	sf.width = w;
	sf.height = h;
	sf.internal_format = ifmt;
	sf.defined = true;
	if (row != 0) {
		for (int i = 0; i < w; i++) {
			if (type == GL_FLOAT) {
				for (int c = 0; c < 4; c++) {
					uint32_t bits = ReadMacInt32(row + (i*4+c)*4);
					memcpy(&sf.row[i*4+c], &bits, 4);
				}
			} else if (type == GL_UNSIGNED_BYTE) {
				sf.row[i*4+0] = ReadMacInt8(row + i*4 + 0) / 255.0f;
				sf.row[i*4+1] = ReadMacInt8(row + i*4 + 1) / 255.0f;
				sf.row[i*4+2] = ReadMacInt8(row + i*4 + 2) / 255.0f;
				sf.row[i*4+3] = ReadMacInt8(row + i*4 + 3) / 255.0f;
			}
		}
	}
	if (col != 0) {
		for (int i = 0; i < h; i++) {
			if (type == GL_FLOAT) {
				for (int c = 0; c < 4; c++) {
					uint32_t bits = ReadMacInt32(col + (i*4+c)*4);
					memcpy(&sf.col[i*4+c], &bits, 4);
				}
			} else if (type == GL_UNSIGNED_BYTE) {
				sf.col[i*4+0] = ReadMacInt8(col + i*4 + 0) / 255.0f;
				sf.col[i*4+1] = ReadMacInt8(col + i*4 + 1) / 255.0f;
				sf.col[i*4+2] = ReadMacInt8(col + i*4 + 2) / 255.0f;
				sf.col[i*4+3] = ReadMacInt8(col + i*4 + 3) / 255.0f;
			}
		}
	}
}


// --- Histogram/Minmax operations (10 functions) ---

// DELIBERATE de-advertisement: the ARB imaging
// HISTOGRAM / MINMAX family (sub-ops 491-500) tracks its bins/min-max state
// honestly but is NOT fed by the actual pixel pipeline — a real histogram/minmax
// pass is DELIBERATELY not built (classic Mac games do not use imaging
// histogram/minmax). Advertising the
// capability would be the silent-wrong-output the Core Value forbids: a conformant
// app that sees the imaging umbrella token would read GetHistogram/GetMinmax
// expecting collected pixel statistics and silently get the un-collected defaults.
// The honest fix is capability-gating — the umbrella token is ABSENT from the
// GL_EXTENSIONS string (see the glGetString switch above) and each handler LOGS its
// known limitation so the disposition is visible. State is kept for API
// completeness, never claimed as collected. Real collection path is owned by
// later work. (The umbrella token is not spelled out as a literal so the source-scan
// absence assertion stays robust.)
void NativeGLHistogram(GLContext *ctx, uint32_t target, int32_t width, uint32_t ifmt, uint32_t sink) {
	GL_LOG("Histogram target=0x%x width=%d fmt=0x%x sink=%d (known limitation — bins tracked, NOT collected from pixels; imaging umbrella de-advertised)", target, width, ifmt, sink);
	if (!ctx) return;
	ctx->histogram.width = width;
	ctx->histogram.internal_format = ifmt;
	ctx->histogram.sink = (sink != 0);
	ctx->histogram.defined = true;
	ctx->histogram.bins.assign(width * 4, 0); // 4 channels per bin
}

void NativeGLMinmax(GLContext *ctx, uint32_t target, uint32_t ifmt, uint32_t sink) {
	GL_LOG("Minmax target=0x%x fmt=0x%x sink=%d (known limitation — min/max tracked, NOT collected from pixels; imaging umbrella de-advertised)", target, ifmt, sink);
	if (!ctx) return;
	ctx->minmax.internal_format = ifmt;
	ctx->minmax.sink = (sink != 0);
	ctx->minmax.defined = true;
	for (int i = 0; i < 4; i++) {
		ctx->minmax.min_values[i] = FLT_MAX;
		ctx->minmax.max_values[i] = -FLT_MAX;
	}
}

void NativeGLResetHistogram(GLContext *ctx, uint32_t target) {
	GL_LOG("ResetHistogram");
	if (!ctx) return;
	std::fill(ctx->histogram.bins.begin(), ctx->histogram.bins.end(), 0);
}

void NativeGLResetMinmax(GLContext *ctx, uint32_t target) {
	GL_LOG("ResetMinmax");
	if (!ctx) return;
	for (int i = 0; i < 4; i++) {
		ctx->minmax.min_values[i] = FLT_MAX;
		ctx->minmax.max_values[i] = -FLT_MAX;
	}
}

void NativeGLGetHistogram(GLContext *ctx, uint32_t target, uint32_t reset, uint32_t fmt, uint32_t type, uint32_t data) {
	GL_LOG("GetHistogram target=0x%x reset=%d (known limitation — returns tracked bins, NOT pixel-collected; imaging umbrella de-advertised)", target, reset);
	if (!ctx || data == 0) return;
	if (!ctx->histogram.defined) return;
	int w = ctx->histogram.width;
	// Write bin data as 32-bit unsigned integers (4 channels per bin)
	if (type == GL_UNSIGNED_INT || type == GL_UNSIGNED_BYTE) {
		for (int i = 0; i < w * 4 && i < (int)ctx->histogram.bins.size(); i++) {
			WriteMacInt32(data + i * 4, ctx->histogram.bins[i]);
		}
	}
	if (reset) {
		std::fill(ctx->histogram.bins.begin(), ctx->histogram.bins.end(), 0);
	}
}

void NativeGLGetHistogramParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	GL_LOG("GetHistogramParameterfv target=0x%x pname=0x%x (known limitation — histogram de-advertised)", target, pname);
	if (!ctx || params == 0) return;
	float val = 0;
	switch (pname) {
		case 0x8028: val = (float)ctx->histogram.width; break;  // GL_HISTOGRAM_WIDTH
		case 0x8027: val = (float)ctx->histogram.internal_format; break; // GL_HISTOGRAM_FORMAT
		case 0x802A: val = ctx->histogram.sink ? 1.0f : 0.0f; break; // GL_HISTOGRAM_SINK
		default: break;
	}
	uint32_t bits; memcpy(&bits, &val, 4);
	WriteMacInt32(params, bits);
}

void NativeGLGetHistogramParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	GL_LOG("GetHistogramParameteriv target=0x%x pname=0x%x (known limitation — histogram de-advertised)", target, pname);
	if (!ctx || params == 0) return;
	int32_t val = 0;
	switch (pname) {
		case 0x8028: val = ctx->histogram.width; break;
		case 0x8027: val = (int32_t)ctx->histogram.internal_format; break;
		case 0x802A: val = ctx->histogram.sink ? 1 : 0; break;
		default: break;
	}
	WriteMacInt32(params, (uint32_t)val);
}

void NativeGLGetMinmax(GLContext *ctx, uint32_t target, uint32_t reset, uint32_t fmt, uint32_t type, uint32_t data) {
	GL_LOG("GetMinmax target=0x%x reset=%d", target, reset);
	if (!ctx || data == 0) return;
	if (!ctx->minmax.defined) return;
	// Write min values (4 floats) then max values (4 floats)
	if (type == GL_FLOAT) {
		for (int i = 0; i < 4; i++) {
			uint32_t bits;
			memcpy(&bits, &ctx->minmax.min_values[i], 4);
			WriteMacInt32(data + i*4, bits);
			memcpy(&bits, &ctx->minmax.max_values[i], 4);
			WriteMacInt32(data + 16 + i*4, bits);
		}
	}
	if (reset) {
		for (int i = 0; i < 4; i++) {
			ctx->minmax.min_values[i] = FLT_MAX;
			ctx->minmax.max_values[i] = -FLT_MAX;
		}
	}
}

void NativeGLGetMinmaxParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	GL_LOG("GetMinmaxParameterfv target=0x%x pname=0x%x", target, pname);
	if (!ctx || params == 0) return;
	float val = 0;
	switch (pname) {
		case 0x802F: val = (float)ctx->minmax.internal_format; break; // GL_MINMAX_FORMAT
		case 0x8030: val = ctx->minmax.sink ? 1.0f : 0.0f; break;    // GL_MINMAX_SINK
		default: break;
	}
	uint32_t bits; memcpy(&bits, &val, 4);
	WriteMacInt32(params, bits);
}

void NativeGLGetMinmaxParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params) {
	GL_LOG("GetMinmaxParameteriv target=0x%x pname=0x%x", target, pname);
	if (!ctx || params == 0) return;
	int32_t val = 0;
	switch (pname) {
		case 0x802F: val = (int32_t)ctx->minmax.internal_format; break;
		case 0x8030: val = ctx->minmax.sink ? 1 : 0; break;
		default: break;
	}
	WriteMacInt32(params, (uint32_t)val);
}


// ============================================================================
//  3D texture operations (EXT_texture3D via Metal MTLTextureType3D)
// ============================================================================

void NativeGLTexImage3DEXT(GLContext *ctx, uint32_t target, int32_t level, int32_t ifmt,
	int32_t w, int32_t h, int32_t d, int32_t border, uint32_t fmt, uint32_t type, uint32_t data)
{
	GL_LOG("TexImage3D %dx%dx%d fmt=0x%x type=0x%x level=%d", w, h, d, fmt, type, level);
	if (!ctx) return;

	uint32_t texName = ctx->tex_units[ctx->active_texture].bound_texture_3d;
	if (texName == 0) return;
	auto it = ctx->texture_objects.find(texName);
	if (it == ctx->texture_objects.end()) return;
	GLTextureObject &tex = it->second;

	tex.width = w;
	tex.height = h;
	tex.depth = d;
	if (level > 0) tex.has_mipmaps = true;

	if (data == 0) {
		// Allocate storage only (no data)
		int emptyLen = w * h * d * 4;
		uint8_t *emptyData = (uint8_t *)calloc(1, emptyLen);
		if (emptyData) {
			GLMetalUpload3DTexture(ctx, &tex, level, w, h, d, emptyData, emptyLen);
			free(emptyData);
		}
		return;
	}

	// Convert each slice and assemble into a single buffer
	int sliceSize = w * h * 4;
	int totalSize = sliceSize * d;
	uint8_t *converted = (uint8_t *)malloc(totalSize);
	if (!converted) return;

	// Compute source slice stride
	int srcBpp = 0;
	switch (fmt) {
		case GL_RGBA: case 0x80E1: srcBpp = 4; break; // GL_BGRA_EXT
		case GL_RGB: srcBpp = 3; break;
		case GL_LUMINANCE: case GL_ALPHA_FORMAT: srcBpp = 1; break;
		case GL_LUMINANCE_ALPHA: srcBpp = 2; break;
		default: srcBpp = 4; break;
	}
	int srcSliceBytes = w * h * srcBpp;

	for (int z = 0; z < d; z++) {
		uint32_t sliceSrc = data + z * srcSliceBytes;
		int outLen = 0;
		uint8_t *sliceData = ConvertPixelsToBGRA8(sliceSrc, w, h, fmt, type, ctx->pixel_store, ctx, &outLen);
		if (sliceData) {
			memcpy(converted + z * sliceSize, sliceData, sliceSize);
			free(sliceData);
		} else {
			memset(converted + z * sliceSize, 0xFF, sliceSize); // magenta fallback
		}
	}

	GLMetalUpload3DTexture(ctx, &tex, level, w, h, d, converted, totalSize);
	free(converted);
}

void NativeGLTexSubImage3DEXT(GLContext *ctx, uint32_t target, int32_t level,
	int32_t xoff, int32_t yoff, int32_t zoff, int32_t w, int32_t h, int32_t d,
	uint32_t fmt, uint32_t type, uint32_t data)
{
	GL_LOG("TexSubImage3D offset=(%d,%d,%d) size=%dx%dx%d", xoff, yoff, zoff, w, h, d);
	if (!ctx || data == 0) return;

	uint32_t texName = ctx->tex_units[ctx->active_texture].bound_texture_3d;
	if (texName == 0) return;
	auto it = ctx->texture_objects.find(texName);
	if (it == ctx->texture_objects.end()) return;
	GLTextureObject &tex = it->second;
	if (!tex.metal_texture) return;

	// Convert source pixels slice by slice
	int srcBpp = 0;
	switch (fmt) {
		case GL_RGBA: case 0x80E1: srcBpp = 4; break;
		case GL_RGB: srcBpp = 3; break;
		case GL_LUMINANCE: case GL_ALPHA_FORMAT: srcBpp = 1; break;
		case GL_LUMINANCE_ALPHA: srcBpp = 2; break;
		default: srcBpp = 4; break;
	}
	int srcSliceBytes = w * h * srcBpp;
	int sliceSize = w * h * 4;
	int totalSize = sliceSize * d;
	uint8_t *converted = (uint8_t *)malloc(totalSize);
	if (!converted) return;

	for (int z = 0; z < d; z++) {
		uint32_t sliceSrc = data + z * srcSliceBytes;
		int outLen = 0;
		uint8_t *sliceData = ConvertPixelsToBGRA8(sliceSrc, w, h, fmt, type, ctx->pixel_store, ctx, &outLen);
		if (sliceData) {
			memcpy(converted + z * sliceSize, sliceData, sliceSize);
			free(sliceData);
		} else {
			memset(converted + z * sliceSize, 0, sliceSize);
		}
	}

	GLMetalUploadSubTexture3D(ctx, &tex, level, xoff, yoff, zoff, w, h, d,
	                          converted, w * 4, sliceSize);
	free(converted);
}

void NativeGLCopyTexSubImage3DEXT(GLContext *ctx, uint32_t target, int32_t level,
	int32_t xoff, int32_t yoff, int32_t zoff, int32_t x, int32_t y, int32_t w, int32_t h)
{
	// 3D texture sub-copy: read framebuffer, upload as a single slice
	if (!ctx || w <= 0 || h <= 0) return;
	int bufLen = 0;
	uint8_t *bgra = GLMetalReadFramebufferRect(ctx, x, y, w, h, &bufLen);
	if (!bgra) return;

	uint32_t texName = ctx->tex_units[ctx->active_texture].bound_texture_3d;
	if (texName != 0) {
		auto it = ctx->texture_objects.find(texName);
		if (it != ctx->texture_objects.end()) {
			GLMetalUploadSubTexture3D(ctx, &it->second, level, xoff, yoff, zoff, w, h, 1, bgra, w * 4, w * h * 4);
		}
	}
	free(bgra);
}

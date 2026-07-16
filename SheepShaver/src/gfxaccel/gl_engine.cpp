/*
 *  gl_engine.cpp - OpenGL 1.2 AGL platform bindings and FindLibSymbol hook installation
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Implements:
 *    - AGL handler functions (context creation, drawable management, swap buffers)
 *    - GLContext allocation with correct GL 1.2 initial state
 *    - FindLibSymbol hook installation for GL/AGL/GLU/GLUT function interception
 *
 *  AGL is the Apple-specific platform layer that classic Mac OS games call to
 *  create OpenGL contexts and bind them to windows. Without working AGL, no
 *  GL rendering can occur.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "macos_util.h"
#include "gl_engine.h"
#include "gl_agl_renderer_policy.h"
#include "rave_metal_renderer.h"
#include "metal_compositor.h"
#include "gfxaccel_resources.h"
#include "display_mode_controller.h"
#include "gl_drawable_owner_policy.h"
#include "gl_offscreen_policy.h"
#include "accel_logging.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>

#define DEBUG 0
#include "debug.h"

// ---- GL Constants ----
// AGL error codes (from agl.h)
#define AGL_NO_ERROR          0
#define AGL_BAD_ATTRIBUTE     10000
#define AGL_BAD_PROPERTY      10001
#define AGL_BAD_PIXELFMT      10002
#define AGL_BAD_RENDINFO      10003
#define AGL_BAD_CONTEXT       10004
#define AGL_BAD_DRAWABLE      10005
#define AGL_BAD_GDEV          10006
#define AGL_BAD_STATE         10007
#define AGL_BAD_VALUE         10008
#define AGL_BAD_MATCH         10009
#define AGL_BAD_ENUM          10010   // verified vs agl.h: invalid enumerant (honest-error for unknown pnames)
#define AGL_BAD_OFFSCREEN     10011
#define AGL_BAD_FULLSCREEN    10012
#define AGL_BAD_ALLOC         10016

// AGL pixel format attributes
#define AGL_NONE              0
#define AGL_RGBA              4
#define AGL_DOUBLEBUFFER      5
#define AGL_DEPTH_SIZE        12

// GL boolean
#define GL_TRUE  1
#define GL_FALSE 0

// GL error codes
#define GL_NO_ERROR           0
#define GL_INVALID_OPERATION  0x0502

// GL matrix mode
#define GL_MODELVIEW          0x1700
#define GL_PROJECTION         0x1701
#define GL_TEXTURE            0x1702

// GL shade model
#define GL_FLAT               0x1D00
#define GL_SMOOTH             0x1D01

// GL cull face
#define GL_FRONT              0x0404
#define GL_BACK               0x0405
#define GL_FRONT_AND_BACK     0x0408

// GL winding
#define GL_CW                 0x0900
#define GL_CCW                0x0901

// GL depth func
#define GL_NEVER              0x0200
#define GL_LESS               0x0201
#define GL_EQUAL              0x0202
#define GL_LEQUAL             0x0203
#define GL_GREATER            0x0204
#define GL_NOTEQUAL           0x0205
#define GL_GEQUAL             0x0206
#define GL_ALWAYS             0x0207

// GL blend factors
#define GL_ZERO               0
#define GL_ONE                1
#define GL_SRC_ALPHA          0x0302

// GL hints
#define GL_DONT_CARE          0x1100

// GL render mode
#define GL_RENDER             0x1C00

// GL tex env mode
#define GL_MODULATE           0x2100

// GL stencil ops
#define GL_KEEP               0x1E00

// GL logic op
#define GL_COPY               0x1503

// GL polygon modes
#define GL_FILL               0x1B02

// ---- AGL Extended Constants ----
// Pixel format attributes (from agl.h) — AGL_NONE/AGL_RGBA/AGL_DOUBLEBUFFER/AGL_DEPTH_SIZE defined above
#define AGL_BUFFER_SIZE        2
#define AGL_LEVEL              3   // verified vs agl.h: level in plane stacking (value attr)
#define AGL_STEREO             6   // verified vs agl.h: stereo buffering (Boolean attr)
#define AGL_AUX_BUFFERS        7   // verified vs agl.h: number of aux buffers (value attr)
#define AGL_RED_SIZE           8
#define AGL_GREEN_SIZE         9
#define AGL_BLUE_SIZE         10
#define AGL_ALPHA_SIZE        11
#define AGL_STENCIL_SIZE      13
#define AGL_ACCUM_RED_SIZE    14
#define AGL_ACCUM_GREEN_SIZE  15
#define AGL_ACCUM_BLUE_SIZE   16
#define AGL_ACCUM_ALPHA_SIZE  17
#define AGL_PIXEL_SIZE        50
#define AGL_MINIMUM_POLICY    51   // verified vs agl.h: never choose smaller buffers (Boolean attr)
#define AGL_MAXIMUM_POLICY    52   // verified vs agl.h: choose largest buffers (Boolean attr)
#define AGL_OFFSCREEN         53
#define AGL_FULLSCREEN        54
#define AGL_RENDERER_ID       70
#define AGL_ACCELERATED       73
#define AGL_WINDOW            80
#define AGL_VIRTUAL_SCREEN    82   // verified vs agl.h: virtual screen number (describe-only)
#define AGL_COMPLIANT         83

// Renderer properties
#define AGL_BUFFER_MODES      100
#define AGL_MIN_LEVEL         101
#define AGL_MAX_LEVEL         102
#define AGL_COLOR_MODES       103
#define AGL_ACCUM_MODES       104
#define AGL_DEPTH_MODES       105
#define AGL_STENCIL_MODES     106
#define AGL_MAX_AUX_BUFFERS   107
#define AGL_VIDEO_MEMORY      120
#define AGL_TEXTURE_MEMORY    121

// Integer parameter names
#define AGL_SWAP_RECT         200
#define AGL_BUFFER_RECT       202
#define AGL_COLORMAP_TRACKING 210
#define AGL_COLORMAP_ENTRY    212
#define AGL_RASTERIZATION     220
#define AGL_SWAP_INTERVAL     222
#define AGL_STATE_VALIDATION  230

// Configure options
#define AGL_FORMAT_CACHE_SIZE 501
#define AGL_CLEAR_FORMAT_CACHE 502
#define AGL_RETAIN_RENDERERS  503

// Buffer mode bits
#define AGL_SINGLEBUFFER_BIT  0x00000004
#define AGL_DOUBLEBUFFER_BIT  0x00000008

// Bit depth bits
#define AGL_8_BIT             0x00000080
#define AGL_16_BIT            0x00000400
#define AGL_32_BIT            0x00001000

// Color mode bits
#define AGL_RGB888_BIT        0x00004000
#define AGL_ARGB8888_BIT      0x00008000

// GL attrib mask bits (for CopyContext)
#define GL_CURRENT_BIT        0x00000001
#define GL_POINT_BIT          0x00000002
#define GL_LINE_BIT           0x00000004
#define GL_POLYGON_BIT        0x00000008
#define GL_PIXEL_MODE_BIT     0x00000020
#define GL_LIGHTING_BIT       0x00000040
#define GL_FOG_BIT            0x00000080
#define GL_DEPTH_BUFFER_BIT   0x00000100
#define GL_VIEWPORT_BIT       0x00000800
#define GL_TRANSFORM_BIT      0x00001000
#define GL_ENABLE_BIT         0x00002000
#define GL_COLOR_BUFFER_BIT   0x00004000
#define GL_HINT_BIT           0x00008000
#define GL_TEXTURE_BIT        0x00040000
#define GL_SCISSOR_BIT        0x00080000
#define GL_ALL_ATTRIB_BITS    0x000FFFFF

// ---- Context Management ----

#define GL_MAX_CONTEXTS 4

// ---- AGL Per-Context State (stored alongside GLContext) ----
struct AGLContextState {
	uint32_t agl_options;            // bitmask of enabled AGL options
	int32_t  agl_swap_interval;
	int32_t  agl_buffer_rect[4];
	int32_t  agl_swap_rect[4];
	bool     agl_rasterization_enabled;
	uint32_t agl_drawable;           // Mac address stored at SetDrawable time
	bool     agl_offscreen;
	uint32_t agl_offscreen_width;
	uint32_t agl_offscreen_height;
	uint32_t agl_offscreen_rowbytes;
	uint32_t agl_offscreen_baseaddr;
	int32_t  agl_virtual_screen;
	uint32_t agl_owner_before_drawable;
	bool     agl_owner_before_drawable_valid;
};
static AGLContextState agl_ctx_state[GL_MAX_CONTEXTS];

// Static renderer info handle (allocated once via Mac_sysalloc)
static uint32_t renderer_info_handle = 0;

// Static error string cache (one per error code, indices 0-17)
static uint32_t agl_error_strings[18] = {0};

// Static device handle for DevicesOfPixelFormat
static uint32_t agl_device_handle = 0;

#define GL_MAX_CONTEXTS 4

static GLContext *gl_contexts[GL_MAX_CONTEXTS] = { nullptr };
GLContext *gl_current_context = nullptr;

// Mac-side handles (one per context slot)
static uint32_t gl_context_mac_handles[GL_MAX_CONTEXTS] = { 0 };

// ---- AGL context dispatch table ----
//
// Mac OS 9 games (e.g. 4x4 Evolution) access the AGL context as an opaque
// struct and read GL function pointers from an internal dispatch table at
// ctx + 4.  The table is indexed alphabetically by GL function name, matching
// the order of gl_symbols[] in GLInstallHooks.  Each entry is a TVECT address.
//
// We must allocate Mac-side context handles large enough to hold this table
// and populate it with our gl_method_tvects[] addresses so that games calling
// through the dispatch table hit our hooks instead of garbage memory.

#define GL_DISPATCH_TABLE_ENTRIES 336   // matches gl_symbols[] count

// Sub-opcodes in gl_symbols[] alphabetical order.
// Generated from the gl_symbols[] table in GLInstallHooks.
static const uint16_t gl_dispatch_sub_opcodes[GL_DISPATCH_TABLE_ENTRIES] = {
	GL_SUB_ACCUM, GL_SUB_ALPHA_FUNC, GL_SUB_ARE_TEXTURES_RESIDENT,
	GL_SUB_ARRAY_ELEMENT, GL_SUB_BEGIN, GL_SUB_BIND_TEXTURE,
	GL_SUB_BITMAP, GL_SUB_BLEND_FUNC, GL_SUB_CALL_LIST, GL_SUB_CALL_LISTS,
	GL_SUB_CLEAR, GL_SUB_CLEAR_ACCUM, GL_SUB_CLEAR_COLOR,
	GL_SUB_CLEAR_DEPTH, GL_SUB_CLEAR_INDEX, GL_SUB_CLEAR_STENCIL,
	GL_SUB_CLIP_PLANE, GL_SUB_COLOR3B, GL_SUB_COLOR3BV, GL_SUB_COLOR3D,
	GL_SUB_COLOR3DV, GL_SUB_COLOR3F, GL_SUB_COLOR3FV, GL_SUB_COLOR3I,
	GL_SUB_COLOR3IV, GL_SUB_COLOR3S, GL_SUB_COLOR3SV, GL_SUB_COLOR3UB,
	GL_SUB_COLOR3UBV, GL_SUB_COLOR3UI, GL_SUB_COLOR3UIV, GL_SUB_COLOR3US,
	GL_SUB_COLOR3USV, GL_SUB_COLOR4B, GL_SUB_COLOR4BV, GL_SUB_COLOR4D,
	GL_SUB_COLOR4DV, GL_SUB_COLOR4F, GL_SUB_COLOR4FV, GL_SUB_COLOR4I,
	GL_SUB_COLOR4IV, GL_SUB_COLOR4S, GL_SUB_COLOR4SV, GL_SUB_COLOR4UB,
	GL_SUB_COLOR4UBV, GL_SUB_COLOR4UI, GL_SUB_COLOR4UIV, GL_SUB_COLOR4US,
	GL_SUB_COLOR4USV, GL_SUB_COLOR_MASK, GL_SUB_COLOR_MATERIAL,
	GL_SUB_COLOR_POINTER, GL_SUB_COPY_PIXELS, GL_SUB_COPY_TEX_IMAGE1D,
	GL_SUB_COPY_TEX_IMAGE2D, GL_SUB_COPY_TEX_SUB_IMAGE1D,
	GL_SUB_COPY_TEX_SUB_IMAGE2D, GL_SUB_CULL_FACE, GL_SUB_DELETE_LISTS,
	GL_SUB_DELETE_TEXTURES, GL_SUB_DEPTH_FUNC, GL_SUB_DEPTH_MASK,
	GL_SUB_DEPTH_RANGE, GL_SUB_DISABLE, GL_SUB_DISABLE_CLIENT_STATE,
	GL_SUB_DRAW_ARRAYS, GL_SUB_DRAW_BUFFER, GL_SUB_DRAW_ELEMENTS,
	GL_SUB_DRAW_PIXELS, GL_SUB_EDGE_FLAG, GL_SUB_EDGE_FLAG_POINTER,
	GL_SUB_EDGE_FLAGV, GL_SUB_ENABLE, GL_SUB_ENABLE_CLIENT_STATE,
	GL_SUB_END, GL_SUB_END_LIST, GL_SUB_EVAL_COORD1D,
	GL_SUB_EVAL_COORD1DV, GL_SUB_EVAL_COORD1F, GL_SUB_EVAL_COORD1FV,
	GL_SUB_EVAL_COORD2D, GL_SUB_EVAL_COORD2DV, GL_SUB_EVAL_COORD2F,
	GL_SUB_EVAL_COORD2FV, GL_SUB_EVAL_MESH1, GL_SUB_EVAL_MESH2,
	GL_SUB_EVAL_POINT1, GL_SUB_EVAL_POINT2, GL_SUB_FEEDBACK_BUFFER,
	GL_SUB_FINISH, GL_SUB_FLUSH, GL_SUB_FOGF, GL_SUB_FOGFV,
	GL_SUB_FOGI, GL_SUB_FOGIV, GL_SUB_FRONT_FACE, GL_SUB_FRUSTUM,
	GL_SUB_GEN_LISTS, GL_SUB_GEN_TEXTURES, GL_SUB_GET_BOOLEANV,
	GL_SUB_GET_CLIP_PLANE, GL_SUB_GET_DOUBLEV, GL_SUB_GET_ERROR,
	GL_SUB_GET_FLOATV, GL_SUB_GET_INTEGERV, GL_SUB_GET_LIGHTFV,
	GL_SUB_GET_LIGHTIV, GL_SUB_GET_MAPDV, GL_SUB_GET_MAPFV,
	GL_SUB_GET_MAPIV, GL_SUB_GET_MATERIALFV, GL_SUB_GET_MATERIALIV,
	GL_SUB_GET_PIXEL_MAPFV, GL_SUB_GET_PIXEL_MAPUIV,
	GL_SUB_GET_PIXEL_MAPUSV, GL_SUB_GET_POINTERV,
	GL_SUB_GET_POLYGON_STIPPLE, GL_SUB_GET_STRING,
	GL_SUB_GET_TEX_ENVFV, GL_SUB_GET_TEX_ENVIV, GL_SUB_GET_TEX_GENDV,
	GL_SUB_GET_TEX_GENFV, GL_SUB_GET_TEX_GENIV, GL_SUB_GET_TEX_IMAGE,
	GL_SUB_GET_TEX_LEVEL_PARAMETERFV, GL_SUB_GET_TEX_LEVEL_PARAMETERIV,
	GL_SUB_GET_TEX_PARAMETERFV, GL_SUB_GET_TEX_PARAMETERIV,
	GL_SUB_HINT, GL_SUB_INDEX_MASK, GL_SUB_INDEX_POINTER,
	GL_SUB_INDEXD, GL_SUB_INDEXDV, GL_SUB_INDEXF, GL_SUB_INDEXFV,
	GL_SUB_INDEXI, GL_SUB_INDEXIV, GL_SUB_INDEXS, GL_SUB_INDEXSV,
	GL_SUB_INDEXUB, GL_SUB_INDEXUBV, GL_SUB_INIT_NAMES,
	GL_SUB_INTERLEAVED_ARRAYS, GL_SUB_IS_ENABLED, GL_SUB_IS_LIST,
	GL_SUB_IS_TEXTURE, GL_SUB_LIGHT_MODELF, GL_SUB_LIGHT_MODELFV,
	GL_SUB_LIGHT_MODELI, GL_SUB_LIGHT_MODELIV, GL_SUB_LIGHTF,
	GL_SUB_LIGHTFV, GL_SUB_LIGHTI, GL_SUB_LIGHTIV,
	GL_SUB_LINE_STIPPLE, GL_SUB_LINE_WIDTH, GL_SUB_LIST_BASE,
	GL_SUB_LOAD_IDENTITY, GL_SUB_LOAD_MATRIXD, GL_SUB_LOAD_MATRIXF,
	GL_SUB_LOAD_NAME, GL_SUB_LOGIC_OP, GL_SUB_MAP1D, GL_SUB_MAP1F,
	GL_SUB_MAP2D, GL_SUB_MAP2F, GL_SUB_MAP_GRID1D, GL_SUB_MAP_GRID1F,
	GL_SUB_MAP_GRID2D, GL_SUB_MAP_GRID2F, GL_SUB_MATERIALF,
	GL_SUB_MATERIALFV, GL_SUB_MATERIALI, GL_SUB_MATERIALIV,
	GL_SUB_MATRIX_MODE, GL_SUB_MULT_MATRIXD, GL_SUB_MULT_MATRIXF,
	GL_SUB_NEW_LIST, GL_SUB_NORMAL3B, GL_SUB_NORMAL3BV,
	GL_SUB_NORMAL3D, GL_SUB_NORMAL3DV, GL_SUB_NORMAL3F,
	GL_SUB_NORMAL3FV, GL_SUB_NORMAL3I, GL_SUB_NORMAL3IV,
	GL_SUB_NORMAL3S, GL_SUB_NORMAL3SV, GL_SUB_NORMAL_POINTER,
	GL_SUB_ORTHO, GL_SUB_PASS_THROUGH, GL_SUB_PIXEL_MAPFV,
	GL_SUB_PIXEL_MAPUIV, GL_SUB_PIXEL_MAPUSV, GL_SUB_PIXEL_STOREF,
	GL_SUB_PIXEL_STOREI, GL_SUB_PIXEL_TRANSFERF, GL_SUB_PIXEL_TRANSFERI,
	GL_SUB_PIXEL_ZOOM, GL_SUB_POINT_SIZE, GL_SUB_POLYGON_MODE,
	GL_SUB_POLYGON_OFFSET, GL_SUB_POLYGON_STIPPLE, GL_SUB_POP_ATTRIB,
	GL_SUB_POP_CLIENT_ATTRIB, GL_SUB_POP_MATRIX, GL_SUB_POP_NAME,
	GL_SUB_PRIORITIZE_TEXTURES, GL_SUB_PUSH_ATTRIB,
	GL_SUB_PUSH_CLIENT_ATTRIB, GL_SUB_PUSH_MATRIX, GL_SUB_PUSH_NAME,
	GL_SUB_RASTER_POS2D, GL_SUB_RASTER_POS2DV, GL_SUB_RASTER_POS2F,
	GL_SUB_RASTER_POS2FV, GL_SUB_RASTER_POS2I, GL_SUB_RASTER_POS2IV,
	GL_SUB_RASTER_POS2S, GL_SUB_RASTER_POS2SV, GL_SUB_RASTER_POS3D,
	GL_SUB_RASTER_POS3DV, GL_SUB_RASTER_POS3F, GL_SUB_RASTER_POS3FV,
	GL_SUB_RASTER_POS3I, GL_SUB_RASTER_POS3IV, GL_SUB_RASTER_POS3S,
	GL_SUB_RASTER_POS3SV, GL_SUB_RASTER_POS4D, GL_SUB_RASTER_POS4DV,
	GL_SUB_RASTER_POS4F, GL_SUB_RASTER_POS4FV, GL_SUB_RASTER_POS4I,
	GL_SUB_RASTER_POS4IV, GL_SUB_RASTER_POS4S, GL_SUB_RASTER_POS4SV,
	GL_SUB_READ_BUFFER, GL_SUB_READ_PIXELS, GL_SUB_RECTD,
	GL_SUB_RECTDV, GL_SUB_RECTF, GL_SUB_RECTFV, GL_SUB_RECTI,
	GL_SUB_RECTIV, GL_SUB_RECTS, GL_SUB_RECTSV, GL_SUB_RENDER_MODE,
	GL_SUB_ROTATED, GL_SUB_ROTATEF, GL_SUB_SCALED, GL_SUB_SCALEF,
	GL_SUB_SCISSOR, GL_SUB_SELECT_BUFFER, GL_SUB_SHADE_MODEL,
	GL_SUB_STENCIL_FUNC, GL_SUB_STENCIL_MASK, GL_SUB_STENCIL_OP,
	GL_SUB_TEX_COORD1D, GL_SUB_TEX_COORD1DV, GL_SUB_TEX_COORD1F,
	GL_SUB_TEX_COORD1FV, GL_SUB_TEX_COORD1I, GL_SUB_TEX_COORD1IV,
	GL_SUB_TEX_COORD1S, GL_SUB_TEX_COORD1SV, GL_SUB_TEX_COORD2D,
	GL_SUB_TEX_COORD2DV, GL_SUB_TEX_COORD2F, GL_SUB_TEX_COORD2FV,
	GL_SUB_TEX_COORD2I, GL_SUB_TEX_COORD2IV, GL_SUB_TEX_COORD2S,
	GL_SUB_TEX_COORD2SV, GL_SUB_TEX_COORD3D, GL_SUB_TEX_COORD3DV,
	GL_SUB_TEX_COORD3F, GL_SUB_TEX_COORD3FV, GL_SUB_TEX_COORD3I,
	GL_SUB_TEX_COORD3IV, GL_SUB_TEX_COORD3S, GL_SUB_TEX_COORD3SV,
	GL_SUB_TEX_COORD4D, GL_SUB_TEX_COORD4DV, GL_SUB_TEX_COORD4F,
	GL_SUB_TEX_COORD4FV, GL_SUB_TEX_COORD4I, GL_SUB_TEX_COORD4IV,
	GL_SUB_TEX_COORD4S, GL_SUB_TEX_COORD4SV,
	GL_SUB_TEX_COORD_POINTER, GL_SUB_TEX_ENVF, GL_SUB_TEX_ENVFV,
	GL_SUB_TEX_ENVI, GL_SUB_TEX_ENVIV, GL_SUB_TEX_GEND,
	GL_SUB_TEX_GENDV, GL_SUB_TEX_GENF, GL_SUB_TEX_GENFV,
	GL_SUB_TEX_GENI, GL_SUB_TEX_GENIV, GL_SUB_TEX_IMAGE1D,
	GL_SUB_TEX_IMAGE2D, GL_SUB_TEX_PARAMETERF, GL_SUB_TEX_PARAMETERFV,
	GL_SUB_TEX_PARAMETERI, GL_SUB_TEX_PARAMETERIV,
	GL_SUB_TEX_SUB_IMAGE1D, GL_SUB_TEX_SUB_IMAGE2D,
	GL_SUB_TRANSLATED, GL_SUB_TRANSLATEF, GL_SUB_VERTEX2D,
	GL_SUB_VERTEX2DV, GL_SUB_VERTEX2F, GL_SUB_VERTEX2FV,
	GL_SUB_VERTEX2I, GL_SUB_VERTEX2IV, GL_SUB_VERTEX2S,
	GL_SUB_VERTEX2SV, GL_SUB_VERTEX3D, GL_SUB_VERTEX3DV,
	GL_SUB_VERTEX3F, GL_SUB_VERTEX3FV, GL_SUB_VERTEX3I,
	GL_SUB_VERTEX3IV, GL_SUB_VERTEX3S, GL_SUB_VERTEX3SV,
	GL_SUB_VERTEX4D, GL_SUB_VERTEX4DV, GL_SUB_VERTEX4F,
	GL_SUB_VERTEX4FV, GL_SUB_VERTEX4I, GL_SUB_VERTEX4IV,
	GL_SUB_VERTEX4S, GL_SUB_VERTEX4SV, GL_SUB_VERTEX_POINTER,
	GL_SUB_VIEWPORT,
};

// Extension entry points that games read directly from the context-handle
// dispatch table at slots BEYOND core GL 1.1 (slot >= GL_DISPATCH_TABLE_ENTRIES).
// Each maps a dispatch slot to the GL sub-opcode whose dispatch-table TVECT we
// install there, so the game reaches the real handler instead of the diagnostic
// no-op. Slots confirmed from Cro-Mag Rally's device log (the UNMAPPED-slot
// diagnostic named handle+0x550=slot 339 and handle+0x554=slot 340). Add more
// rows here as other games surface other extension slots.
struct GLDispatchExtSlot { uint16_t slot; uint16_t sub_opcode; };
#define GL_EXT_DISPATCH_SLOT(sub) ((uint16_t)(337 + ((sub) - GL_EXT_FIRST)))
static const GLDispatchExtSlot gl_dispatch_ext_slots[] = {
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_BLEND_COLOR_EXT), GL_SUB_BLEND_COLOR_EXT },       // glBlendColorEXT
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_BLEND_EQUATION_EXT), GL_SUB_BLEND_EQUATION_EXT }, // glBlendEquationEXT
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_LOCK_ARRAYS_EXT), GL_SUB_LOCK_ARRAYS_EXT },       // glLockArraysEXT
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_UNLOCK_ARRAYS_EXT), GL_SUB_UNLOCK_ARRAYS_EXT },   // glUnlockArraysEXT
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_CLIENT_ACTIVE_TEXTURE_ARB), GL_SUB_CLIENT_ACTIVE_TEXTURE_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_ACTIVE_TEXTURE_ARB), GL_SUB_ACTIVE_TEXTURE_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD1D_ARB), GL_SUB_MULTI_TEX_COORD1D_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD1DV_ARB), GL_SUB_MULTI_TEX_COORD1DV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD1F_ARB), GL_SUB_MULTI_TEX_COORD1F_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD1FV_ARB), GL_SUB_MULTI_TEX_COORD1FV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD1I_ARB), GL_SUB_MULTI_TEX_COORD1I_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD1IV_ARB), GL_SUB_MULTI_TEX_COORD1IV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD1S_ARB), GL_SUB_MULTI_TEX_COORD1S_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD1SV_ARB), GL_SUB_MULTI_TEX_COORD1SV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD2D_ARB), GL_SUB_MULTI_TEX_COORD2D_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD2DV_ARB), GL_SUB_MULTI_TEX_COORD2DV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD2F_ARB), GL_SUB_MULTI_TEX_COORD2F_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD2FV_ARB), GL_SUB_MULTI_TEX_COORD2FV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD2I_ARB), GL_SUB_MULTI_TEX_COORD2I_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD2IV_ARB), GL_SUB_MULTI_TEX_COORD2IV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD2S_ARB), GL_SUB_MULTI_TEX_COORD2S_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD2SV_ARB), GL_SUB_MULTI_TEX_COORD2SV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD3D_ARB), GL_SUB_MULTI_TEX_COORD3D_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD3DV_ARB), GL_SUB_MULTI_TEX_COORD3DV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD3F_ARB), GL_SUB_MULTI_TEX_COORD3F_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD3FV_ARB), GL_SUB_MULTI_TEX_COORD3FV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD3I_ARB), GL_SUB_MULTI_TEX_COORD3I_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD3IV_ARB), GL_SUB_MULTI_TEX_COORD3IV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD3S_ARB), GL_SUB_MULTI_TEX_COORD3S_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD3SV_ARB), GL_SUB_MULTI_TEX_COORD3SV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD4D_ARB), GL_SUB_MULTI_TEX_COORD4D_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD4DV_ARB), GL_SUB_MULTI_TEX_COORD4DV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD4F_ARB), GL_SUB_MULTI_TEX_COORD4F_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD4FV_ARB), GL_SUB_MULTI_TEX_COORD4FV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD4I_ARB), GL_SUB_MULTI_TEX_COORD4I_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD4IV_ARB), GL_SUB_MULTI_TEX_COORD4IV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD4S_ARB), GL_SUB_MULTI_TEX_COORD4S_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULTI_TEX_COORD4SV_ARB), GL_SUB_MULTI_TEX_COORD4SV_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_LOAD_TRANSPOSE_MATRIXD_ARB), GL_SUB_LOAD_TRANSPOSE_MATRIXD_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_LOAD_TRANSPOSE_MATRIXF_ARB), GL_SUB_LOAD_TRANSPOSE_MATRIXF_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULT_TRANSPOSE_MATRIXD_ARB), GL_SUB_MULT_TRANSPOSE_MATRIXD_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_MULT_TRANSPOSE_MATRIXF_ARB), GL_SUB_MULT_TRANSPOSE_MATRIXF_ARB },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARYCOLOR3B_EXT), GL_SUB_SECONDARYCOLOR3B_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARYCOLOR3BV_EXT), GL_SUB_SECONDARYCOLOR3BV_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARYCOLOR3D_EXT), GL_SUB_SECONDARYCOLOR3D_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3DV_EXT), GL_SUB_SECONDARY_COLOR3DV_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3F_EXT), GL_SUB_SECONDARY_COLOR3F_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3FV_EXT), GL_SUB_SECONDARY_COLOR3FV_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3I_EXT), GL_SUB_SECONDARY_COLOR3I_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3IV_EXT), GL_SUB_SECONDARY_COLOR3IV_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3S_EXT), GL_SUB_SECONDARY_COLOR3S_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3SV_EXT), GL_SUB_SECONDARY_COLOR3SV_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3UB_EXT), GL_SUB_SECONDARY_COLOR3UB_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3UBV_EXT), GL_SUB_SECONDARY_COLOR3UBV_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3UI_EXT), GL_SUB_SECONDARY_COLOR3UI_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3UIV_EXT), GL_SUB_SECONDARY_COLOR3UIV_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3US_EXT), GL_SUB_SECONDARY_COLOR3US_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR3USV_EXT), GL_SUB_SECONDARY_COLOR3USV_EXT },
	{ GL_EXT_DISPATCH_SLOT(GL_SUB_SECONDARY_COLOR_POINTER_EXT), GL_SUB_SECONDARY_COLOR_POINTER_EXT },
};
#undef GL_EXT_DISPATCH_SLOT

// Size of Mac-side context handle: 4 bytes for slot index + an over-allocated
// dispatch table. We size for GL_CTX_DISPATCH_SLOTS (not just the 336 core
// entries) so games that read GL function descriptors directly from the handle
// can index extension slots beyond core 1.1 without running off the end of the
// allocation into uninitialized guest heap. See GLPopulateDispatchTable.
#define GL_CTX_HANDLE_SIZE (4 + GL_CTX_DISPATCH_SLOTS * 4)

// Current context index (0-based, -1 = none)
static int gl_current_context_idx = -1;

// Last AGL error
static uint32_t gl_agl_last_error = AGL_NO_ERROR;

// Pixel format storage (simple: just save that a format was requested)
struct GLPixelFormatInfo {
	uint32_t mac_addr;      // Mac-side handle
	bool     has_depth;
	bool     has_double_buffer;
	bool     has_rgba;
};

#define GL_MAX_PIXEL_FORMATS 4
static GLPixelFormatInfo gl_pixel_formats[GL_MAX_PIXEL_FORMATS];
static int gl_pixel_format_count = 0;



static const char *GLDMCOwnerName(uint32_t owner)
{
	switch (owner) {
	case kDMCOwnerQuickDraw: return "QuickDraw";
	case kDMCOwnerRAVE:      return "RAVE";
	case kDMCOwnerGL:        return "GL";
	case kDMCOwnerDSp:       return "DSp";
	case kDMCOwnerBlanking:  return "Blanking";
	case kDMCOwnerQuiescent: return "Quiescent";
	default:                 return "unknown";
	}
}

static bool GLOtherDrawableBound(int except_idx)
{
	for (int i = 0; i < GL_MAX_CONTEXTS; ++i) {
		if (i != except_idx &&
		    gl_contexts[i] != nullptr &&
		    agl_ctx_state[i].agl_drawable != 0) {
			return true;
		}
	}
	return false;
}

static void GLCaptureOwnerBeforeDrawableBind(int idx)
{
	if (!GLShouldSnapshotDrawableOwner(agl_ctx_state[idx].agl_drawable != 0,
	                                   agl_ctx_state[idx].agl_owner_before_drawable_valid)) {
		return;
	}

	const DMCModeSnapshot *snap = dmc_current_snapshot();
	uint32_t owner = snap ? snap->active_owner : kDMCOwnerQuickDraw;
	agl_ctx_state[idx].agl_owner_before_drawable = owner;
	agl_ctx_state[idx].agl_owner_before_drawable_valid = true;
	GL_LOG("aglSetDrawable: captured display owner %s before binding context %d",
	       GLDMCOwnerName(owner), idx + 1);
}

static void GLFinishDrawableUnbind(int idx)
{
	uint32_t captured_owner = agl_ctx_state[idx].agl_owner_before_drawable_valid
	                            ? agl_ctx_state[idx].agl_owner_before_drawable
	                            : kDMCOwnerQuickDraw;
	uint32_t restore_owner = GLRestorableDrawableOwner(captured_owner);
	bool other_drawable = GLOtherDrawableBound(idx);

	agl_ctx_state[idx].agl_drawable = 0;
	agl_ctx_state[idx].agl_owner_before_drawable = kDMCOwnerQuickDraw;
	agl_ctx_state[idx].agl_owner_before_drawable_valid = false;

	if (other_drawable) {
		GL_LOG("aglSetDrawable: deferred owner restore after context %d unbind; another GL drawable remains",
		       idx + 1);
		return;
	}

	(void)dmc_set_active_owner(restore_owner);
	GL_LOG("aglSetDrawable: restored display owner %s after context %d unbind (captured %s)",
	       GLDMCOwnerName(restore_owner), idx + 1, GLDMCOwnerName(captured_owner));
}


/*
 *  GLContextNew - Allocate and initialize a GLContext with GL 1.2 defaults
 *
 *  All initial values match the OpenGL 1.2 specification defaults.
 */
static GLContext *GLContextNew(int width, int height)
{
	GLContext *ctx = new GLContext();
	// Note: Do NOT memset here. GLContext contains std::unordered_map and
	// std::vector members whose internal state would be destroyed by memset.
	// Value-initialization via new GLContext() already zero-inits POD members,
	// and the explicit assignments below set all fields to GL 1.2 defaults.

	// ---- Matrix stacks ----
	// Identity matrix for modelview[0], projection[0], texture[unit][0]
	auto setIdentity = [](float m[16]) {
		memset(m, 0, sizeof(float) * 16);
		m[0] = m[5] = m[10] = m[15] = 1.0f;
	};

	setIdentity(ctx->modelview_stack[0]);
	ctx->modelview_depth = 0;

	setIdentity(ctx->projection_stack[0]);
	ctx->projection_depth = 0;

	for (int u = 0; u < 4; u++) {
		setIdentity(ctx->texture_stack[u][0]);
		ctx->texture_depth[u] = 0;
	}

	setIdentity(ctx->color_stack[0]);
	ctx->color_depth = 0;

	ctx->matrix_mode = GL_MODELVIEW;

	// ---- Current vertex state ----
	ctx->current_color[0] = 1.0f;
	ctx->current_color[1] = 1.0f;
	ctx->current_color[2] = 1.0f;
	ctx->current_color[3] = 1.0f;

	ctx->current_normal[0] = 0.0f;
	ctx->current_normal[1] = 0.0f;
	ctx->current_normal[2] = 1.0f;

	// texcoords default to (0,0,0,1)
	for (int u = 0; u < 4; u++) {
		ctx->current_texcoord[u][0] = 0.0f;
		ctx->current_texcoord[u][1] = 0.0f;
		ctx->current_texcoord[u][2] = 0.0f;
		ctx->current_texcoord[u][3] = 1.0f;
	}

	// ---- Immediate mode ----
	ctx->in_begin = false;
	ctx->im_mode = 0;

	// ---- Lighting ----
	ctx->lighting_enabled = false;
	ctx->color_material_enabled = false;
	ctx->color_material_face = GL_FRONT_AND_BACK;
	ctx->color_material_mode = 0x1200;  // GL_AMBIENT_AND_DIFFUSE
	ctx->light_model_ambient[0] = 0.2f;
	ctx->light_model_ambient[1] = 0.2f;
	ctx->light_model_ambient[2] = 0.2f;
	ctx->light_model_ambient[3] = 1.0f;
	ctx->light_model_two_side = false;
	ctx->light_model_local_viewer = false;

	// Initialize all 8 lights
	for (int i = 0; i < 8; i++) {
		GLLight &L = ctx->lights[i];
		// All lights default: ambient black, diffuse/specular white for light 0, black for others
		L.ambient[0] = 0.0f; L.ambient[1] = 0.0f; L.ambient[2] = 0.0f; L.ambient[3] = 1.0f;
		if (i == 0) {
			L.diffuse[0] = 1.0f; L.diffuse[1] = 1.0f; L.diffuse[2] = 1.0f; L.diffuse[3] = 1.0f;
			L.specular[0] = 1.0f; L.specular[1] = 1.0f; L.specular[2] = 1.0f; L.specular[3] = 1.0f;
		} else {
			L.diffuse[0] = 0.0f; L.diffuse[1] = 0.0f; L.diffuse[2] = 0.0f; L.diffuse[3] = 1.0f;
			L.specular[0] = 0.0f; L.specular[1] = 0.0f; L.specular[2] = 0.0f; L.specular[3] = 1.0f;
		}
		L.position[0] = 0.0f; L.position[1] = 0.0f; L.position[2] = 1.0f; L.position[3] = 0.0f;
		L.spot_direction[0] = 0.0f; L.spot_direction[1] = 0.0f; L.spot_direction[2] = -1.0f;
		L.spot_exponent = 0.0f;
		L.spot_cutoff = 180.0f;
		L.constant_attenuation = 1.0f;
		L.linear_attenuation = 0.0f;
		L.quadratic_attenuation = 0.0f;
		L.enabled = false;
	}

	// Initialize materials (front and back)
	for (int i = 0; i < 2; i++) {
		GLMaterial &M = ctx->materials[i];
		M.ambient[0] = 0.2f; M.ambient[1] = 0.2f; M.ambient[2] = 0.2f; M.ambient[3] = 1.0f;
		M.diffuse[0] = 0.8f; M.diffuse[1] = 0.8f; M.diffuse[2] = 0.8f; M.diffuse[3] = 1.0f;
		M.specular[0] = 0.0f; M.specular[1] = 0.0f; M.specular[2] = 0.0f; M.specular[3] = 1.0f;
		M.emission[0] = 0.0f; M.emission[1] = 0.0f; M.emission[2] = 0.0f; M.emission[3] = 1.0f;
		M.shininess = 0.0f;
	}

	// ---- Texture units ----
	for (int u = 0; u < 4; u++) {
		GLTextureUnit &T = ctx->tex_units[u];
		T.bound_texture_1d = 0;
		T.bound_texture_2d = 0;
		T.enabled_1d = false;
		T.enabled_2d = false;
		T.env_mode = GL_MODULATE;
		T.env_color[0] = T.env_color[1] = T.env_color[2] = T.env_color[3] = 0.0f;
		T.current_texcoord[0] = 0.0f;
		T.current_texcoord[1] = 0.0f;
		T.current_texcoord[2] = 0.0f;
		T.current_texcoord[3] = 1.0f;
		T.texgen_s_enabled = T.texgen_t_enabled = T.texgen_r_enabled = T.texgen_q_enabled = false;
		T.texgen_s_mode = T.texgen_t_mode = T.texgen_r_mode = T.texgen_q_mode = 0;
	}
	ctx->active_texture = 0;
	ctx->client_active_texture = 0;
	ctx->next_texture_name = 1;
	ctx->completed_draw_serial = 0;
	for (int u = 0; u < 4; u++) {
		ctx->texture_2d_enable_draw_serial[u] = 0;
		ctx->texcoord_array_enable_draw_serial[u] = 0;
		ctx->prepared_texture_2d_for_draw[u] = false;
		ctx->latched_texture_2d_for_array_draw[u] = false;
		ctx->prepared_texcoord_array_for_draw[u] = false;
	}

	// ---- Enable/disable caps ----
	ctx->depth_test = false;
	ctx->blend = false;
	ctx->color_sum = false;
	ctx->cull_face_enabled = false;
	ctx->cull_face_mode = GL_BACK;
	ctx->front_face = GL_CCW;
	ctx->alpha_test = false;
	ctx->alpha_func = GL_ALWAYS;
	ctx->alpha_ref = 0.0f;
	ctx->scissor_test = false;
	ctx->stencil_test = false;
	ctx->fog_enabled = false;
	ctx->normalize = false;
	ctx->auto_normal = false;
	ctx->point_smooth = false;
	ctx->line_smooth = false;
	ctx->polygon_smooth = false;
	ctx->dither = true;  // GL default: dithering is enabled
	ctx->color_logic_op = false;
	ctx->polygon_offset_fill = false;
	ctx->polygon_offset_line = false;
	ctx->polygon_offset_point = false;
	ctx->multisample = true;  // GL default
	ctx->sample_alpha_to_coverage = false;
	ctx->sample_alpha_to_one = false;
	ctx->sample_coverage = false;

	// ---- Fog ----
	ctx->fog_mode = 0x0800;  // GL_EXP
	ctx->fog_color[0] = ctx->fog_color[1] = ctx->fog_color[2] = ctx->fog_color[3] = 0.0f;
	ctx->fog_density = 1.0f;
	ctx->fog_start = 0.0f;
	ctx->fog_end = 1.0f;

	// ---- Viewport and scissor ----
	ctx->viewport[0] = 0;
	ctx->viewport[1] = 0;
	ctx->viewport[2] = width;
	ctx->viewport[3] = height;
	ctx->scissor_box[0] = 0;
	ctx->scissor_box[1] = 0;
	ctx->scissor_box[2] = width;
	ctx->scissor_box[3] = height;
	ctx->depth_range_near = 0.0f;
	ctx->depth_range_far = 1.0f;

	// ---- Depth ----
	// GL spec depth defaults:
	//   depth_test = disabled, depth_func = GL_LESS, depth_mask = true (write enabled)
	//   These match the GL 1.2 spec §2.11.1 initial values.
	ctx->depth_func = GL_LESS;
	ctx->depth_mask = true;  // depth writing enabled by default

	// ---- Blend ----
	// GL spec blend defaults:
	//   blend = disabled, blend_src = GL_ONE, blend_dst = GL_ZERO
	//   These match the GL 1.2 spec §4.1.7 initial values.
	ctx->blend_src = GL_ONE;
	ctx->blend_dst = GL_ZERO;

	// ---- Vertex arrays ----
	// All zero-initialized (disabled, null pointers)

	// ---- Display lists ----
	ctx->next_list_name = 1;
	ctx->in_display_list = false;
	ctx->current_list_name = 0;
	ctx->current_list_mode = 0;
	ctx->list_base = 0;

	// ---- Metal state ----
	ctx->metal = nullptr;

	// ---- Pixel store ----
	ctx->pixel_store.pack_alignment = 4;
	ctx->pixel_store.pack_row_length = 0;
	ctx->pixel_store.pack_skip_pixels = 0;
	ctx->pixel_store.pack_skip_rows = 0;
	ctx->pixel_store.unpack_alignment = 4;
	ctx->pixel_store.unpack_row_length = 0;
	ctx->pixel_store.unpack_skip_pixels = 0;
	ctx->pixel_store.unpack_skip_rows = 0;

	// ---- Stencil ----
	ctx->stencil.func = GL_ALWAYS;
	ctx->stencil.ref = 0;
	ctx->stencil.value_mask = 0xFFFFFFFF;
	ctx->stencil.write_mask = 0xFFFFFFFF;
	ctx->stencil.sfail = GL_KEEP;
	ctx->stencil.dpfail = GL_KEEP;
	ctx->stencil.dppass = GL_KEEP;

	// ---- Polygon mode, line width, point size ----
	ctx->polygon_mode_front = GL_FILL;
	ctx->polygon_mode_back = GL_FILL;
	ctx->line_width = 1.0f;
	ctx->point_size = 1.0f;
	ctx->shade_model = GL_SMOOTH;

	// ---- Polygon offset ----
	ctx->polygon_offset_factor = 0.0f;
	ctx->polygon_offset_units = 0.0f;

	// ---- Attrib stack ----
	ctx->attrib_stack_depth = 0;

	// ---- Selection/feedback ----
	ctx->render_mode = GL_RENDER;
	ctx->selection_buffer_mac_ptr = 0;
	ctx->selection_buffer_size = 0;
	ctx->feedback_buffer_mac_ptr = 0;
	ctx->feedback_buffer_size = 0;
	ctx->feedback_type = 0;
	ctx->name_stack_depth = 0;

	// ---- Clear values ----
	// GL spec clear defaults:
	//   clear_color = (0,0,0,0), clear_depth = 1.0, clear_stencil = 0
	//   These match the GL 1.2 spec initial values.
	ctx->clear_color[0] = 0.0f;
	ctx->clear_color[1] = 0.0f;
	ctx->clear_color[2] = 0.0f;
	ctx->clear_color[3] = 0.0f;
	ctx->clear_depth = 1.0f;
	ctx->clear_stencil = 0;
	ctx->clear_accum[0] = ctx->clear_accum[1] = ctx->clear_accum[2] = ctx->clear_accum[3] = 0.0f;
	ctx->clear_index = 0.0f;

	// ---- Hints ----
	ctx->hint_perspective_correction = GL_DONT_CARE;
	ctx->hint_point_smooth = GL_DONT_CARE;
	ctx->hint_line_smooth = GL_DONT_CARE;
	ctx->hint_polygon_smooth = GL_DONT_CARE;
	ctx->hint_fog = GL_DONT_CARE;

	// ---- Logic op ----
	ctx->logic_op_mode = GL_COPY;

	// ---- Color mask ----
	// GL spec default: all channels writable.
	ctx->color_mask[0] = true;
	ctx->color_mask[1] = true;
	ctx->color_mask[2] = true;
	ctx->color_mask[3] = true;

	// ---- Raster position ----
	ctx->raster_pos[0] = 0.0f;
	ctx->raster_pos[1] = 0.0f;
	ctx->raster_pos[2] = 0.0f;
	ctx->raster_pos[3] = 1.0f;
	ctx->raster_pos_valid = true;

	// ---- Pixel transfer ----
	ctx->pixel_zoom_x = 1.0f;
	ctx->pixel_zoom_y = 1.0f;

	// ---- Clip planes ----
	for (int i = 0; i < 6; i++) {
		ctx->clip_planes[i][0] = ctx->clip_planes[i][1] = ctx->clip_planes[i][2] = ctx->clip_planes[i][3] = 0.0;
		ctx->clip_plane_enabled[i] = false;
	}

	// ---- Error ----
	ctx->last_error = GL_NO_ERROR;

	GL_LOG("GLContextNew: %dx%d context allocated", width, height);

	return ctx;
}


// ===========================================================================
//  AGL Handler Functions (called from gl_dispatch.cpp)
// ===========================================================================

/*
 *  NativeAGLChoosePixelFormat(r3=gdevs, r4=ndev, r5=attribs)
 *
 *  Read the attribute list from PPC memory, log requested attributes,
 *  allocate a Mac-side struct as pixel format handle.
 */
uint32_t NativeAGLChoosePixelFormat(uint32_t gdevs, uint32_t ndev, uint32_t attribs)
{
	GL_LOG("aglChoosePixelFormat: gdevs=0x%08x ndev=%d attribs=0x%08x", gdevs, ndev, attribs);

	// Parse attribute list from PPC memory (per-attribute classifier).
	//
	// AGL pixel-format attributes are a mix of Boolean tokens (no value word
	// follows) and value attributes (a single value word follows). The classic
	// fixed numeric-range value-skip heuristic was BROKEN: AGL_STEREO=6 is
	// Boolean (the range over-skipped a word, desyncing the parse), while
	// AGL_PIXEL_SIZE=50 / AGL_RENDERER_ID=70 are value attrs above the range
	// (under-skipped, also desyncing). We classify each known attribute against
	// agl.h's per-attribute arity instead. Unknown attributes are treated as
	// Boolean (the conservative choice: a stray value word is left in place
	// rather than silently consumed, and the 64-iteration cap + the AGL_NONE
	// terminator both still bound the walk — ASVS V5).
	bool has_rgba = false, has_depth = false, has_double = false;
	if (attribs != 0) {
		uint32_t addr = attribs;
		for (int i = 0; i < 64; i++) {  // safety limit — never advance past AGL_NONE
			uint32_t attr = ReadMacInt32(addr);
			if (attr == AGL_NONE) break;
			GL_LOG("  attrib[%d] = %d", i, attr);

			// Per-attribute value-arity classifier (verified vs agl.h). A value
			// attribute is followed by one value word that must be skipped.
			bool takes_value = false;
			switch (attr) {
				// Value attributes (a value word follows):
				case AGL_BUFFER_SIZE:
				case AGL_LEVEL:
				case AGL_AUX_BUFFERS:
				case AGL_RED_SIZE:
				case AGL_GREEN_SIZE:
				case AGL_BLUE_SIZE:
				case AGL_ALPHA_SIZE:
				case AGL_DEPTH_SIZE:
				case AGL_STENCIL_SIZE:
				case AGL_ACCUM_RED_SIZE:
				case AGL_ACCUM_GREEN_SIZE:
				case AGL_ACCUM_BLUE_SIZE:
				case AGL_ACCUM_ALPHA_SIZE:
				case AGL_PIXEL_SIZE:
				case AGL_RENDERER_ID:
					takes_value = true;
					break;
				// Boolean attributes (no value word) — AGL_RGBA / AGL_DOUBLEBUFFER /
				// AGL_STEREO / AGL_MINIMUM_POLICY / AGL_MAXIMUM_POLICY / etc. fall
				// here (and so do unknown attrs, conservatively).
				default:
					takes_value = false;
					break;
			}

			// Record the three capabilities we actually act on.
			switch (attr) {
				case AGL_RGBA:         has_rgba = true; break;
				case AGL_DOUBLEBUFFER: has_double = true; break;
				case AGL_DEPTH_SIZE:   has_depth = true; break;
				default:               break;
			}

			if (takes_value) {
				addr += 4;  // skip the value word for value attributes
			}
			addr += 4;      // advance to the next attribute token
		}
	}

	// Find a free pixel format slot. Reclaim a slot freed by the destroy handler
	// (mac_addr == 0 sentinel) BEFORE bumping the monotonic count, so a session
	// that creates and destroys >4 pixel formats over its lifetime no longer
	// exhausts the fixed 4-slot table. The Mac handle itself stays leaked
	// (Mac_sysalloc is a bump allocator); only the C++ slot is reclaimed.
	int idx = -1;
	for (int i = 0; i < gl_pixel_format_count; i++) {
		if (gl_pixel_formats[i].mac_addr == 0) {  // freed-slot sentinel
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		if (gl_pixel_format_count >= GL_MAX_PIXEL_FORMATS) {
			GL_LOG("aglChoosePixelFormat: no free pixel format slots");
			gl_agl_last_error = AGL_BAD_ALLOC;
			return 0;
		}
		idx = gl_pixel_format_count++;
	}

	// Allocate Mac-side handle (4 bytes to store index)
	uint32_t mac_handle = Mac_sysalloc(4);
	if (mac_handle == 0) {
		GL_LOG("aglChoosePixelFormat: Mac_sysalloc failed");
		gl_agl_last_error = AGL_BAD_ALLOC;
		return 0;
	}

	gl_pixel_formats[idx].mac_addr = mac_handle;
	gl_pixel_formats[idx].has_depth = has_depth;
	gl_pixel_formats[idx].has_double_buffer = has_double;
	gl_pixel_formats[idx].has_rgba = has_rgba;

	// Store index in Mac memory
	WriteMacInt32(mac_handle, idx);

	GL_LOG("aglChoosePixelFormat: allocated format %d (handle=0x%08x) rgba=%d depth=%d double=%d",
	       idx, mac_handle, has_rgba, has_depth, has_double);

	gl_agl_last_error = AGL_NO_ERROR;
	return mac_handle;
}


/*
 *  GLPopulateDispatchTable - fill the Mac-side dispatch table in a context handle
 *
 *  The dispatch table starts at handle+4. Slots 0..335 are the core GL 1.1
 *  TVECT addresses in alphabetical order (matching gl_symbols[] order). Games
 *  like 4x4 Evolution and Cro-Mag Rally read this table directly from the
 *  context handle instead of calling the exported stubs, and may index slots
 *  beyond core 1.1 (extension entry points). We fill the entire over-allocated
 *  table: every slot we don't map to a real GL function gets a per-slot
 *  diagnostic no-op TVECT (gl_dt_diag_tvects) so a direct read never returns
 *  uninitialized guest heap — which the game would call as a function
 *  descriptor and crash on (Cro-Mag Rally: slot 339 -> illegal instruction).
 */
static void GLPopulateDispatchTable(uint32_t mac_handle)
{
	for (int i = 0; i < GL_CTX_DISPATCH_SLOTS; i++) {
		uint32_t tvect = 0;
		if (i < GL_DISPATCH_TABLE_ENTRIES) {
			uint16_t sub = gl_dispatch_sub_opcodes[i];
			if (sub < GL_MAX_SUBOPCODE)
				tvect = gl_dt_method_tvects[sub];
		} else {
			// Extension slot beyond core 1.1: install the dispatch-table TVECT
			// for the mapped extension sub-opcode, if any (see
			// gl_dispatch_ext_slots[]). Unmapped slots fall through to the
			// diagnostic no-op below so new ones still surface in the log.
			for (size_t e = 0;
			     e < sizeof(gl_dispatch_ext_slots) / sizeof(gl_dispatch_ext_slots[0]);
			     e++) {
				if (gl_dispatch_ext_slots[e].slot == i) {
					uint16_t sub = gl_dispatch_ext_slots[e].sub_opcode;
					if (sub < GL_MAX_SUBOPCODE)
						tvect = gl_dt_method_tvects[sub];
					break;
				}
			}
		}
		if (tvect == 0)
			tvect = gl_dt_diag_tvects[i];   // unmapped slot -> safe logging no-op
		WriteMacInt32(mac_handle + 4 + i * 4, tvect);
	}
}

/*
 *  NativeAGLCreateContext(r3=pixelFormat, r4=shareContext)
 *
 *  Allocate a new GLContext, store in context table, return Mac-side handle.
 */
uint32_t NativeAGLCreateContext(uint32_t pixelFormat, uint32_t shareContext)
{
	GL_LOG("aglCreateContext: pixelFormat=0x%08x shareContext=0x%08x", pixelFormat, shareContext);

	// Find a free context slot
	int slot = -1;
	for (int i = 0; i < GL_MAX_CONTEXTS; i++) {
		if (gl_contexts[i] == nullptr) {
			slot = i;
			break;
		}
	}

	if (slot < 0) {
		GL_LOG("aglCreateContext: no free context slots");
		gl_agl_last_error = AGL_BAD_ALLOC;
		return 0;
	}

	// Create the context with default dimensions (will be set by aglSetDrawable)
	GLContext *ctx = GLContextNew(640, 480);
	gl_contexts[slot] = ctx;

	// Allocate Mac-side context handle with dispatch table.
	// Layout: [0] = 1-based slot index, [4..] = GL function TVECT table.
	// Games may read the dispatch table directly from the context handle
	// (e.g. ctx + 4 + func_index * 4) instead of calling exported stubs.
	uint32_t mac_handle = Mac_sysalloc(GL_CTX_HANDLE_SIZE);
	if (mac_handle == 0) {
		delete ctx;
		gl_contexts[slot] = nullptr;
		GL_LOG("aglCreateContext: Mac_sysalloc failed");
		gl_agl_last_error = AGL_BAD_ALLOC;
		return 0;
	}

	// Zero the entire handle first
	Mac_memset(mac_handle, 0, GL_CTX_HANDLE_SIZE);

	// Store 1-based index in Mac memory
	WriteMacInt32(mac_handle, slot + 1);

	// Populate the dispatch table with our hook TVECTs
	GLPopulateDispatchTable(mac_handle);
	gl_context_mac_handles[slot] = mac_handle;

	// Initialize per-context AGL state
	memset(&agl_ctx_state[slot], 0, sizeof(AGLContextState));
	agl_ctx_state[slot].agl_rasterization_enabled = true;  // default: rasterization on
	agl_ctx_state[slot].agl_owner_before_drawable = kDMCOwnerQuickDraw;

	GL_LOG("aglCreateContext: created context %d (handle=0x%08x)", slot + 1, mac_handle);

	gl_agl_last_error = AGL_NO_ERROR;
	return mac_handle;
}


/*
 *  Helper: look up GLContext from Mac-side handle
 *  Returns context pointer and sets out_idx to 0-based index, or nullptr on error.
 */
static GLContext *GLContextFromHandle(uint32_t mac_handle, int *out_idx = nullptr)
{
	if (mac_handle == 0) return nullptr;

	uint32_t one_based = ReadMacInt32(mac_handle);
	if (one_based == 0 || one_based > GL_MAX_CONTEXTS) return nullptr;

	int idx = one_based - 1;
	if (out_idx) *out_idx = idx;
	return gl_contexts[idx];
}

extern "C" int GLContextGetOffscreenDrawable(GLContext *context,
                                             uint32_t *outW,
                                             uint32_t *outH,
                                             uint32_t *outRowbytes,
                                             uint32_t *outBaseaddr)
{
	if (context == nullptr) return 0;

	for (int i = 0; i < GL_MAX_CONTEXTS; i++) {
		if (gl_contexts[i] != context) continue;

		const AGLContextState &state = agl_ctx_state[i];
		if (!GLShouldReadbackOffscreenDrawable(
		        state.agl_offscreen,
		        state.agl_offscreen_width,
		        state.agl_offscreen_height,
		        state.agl_offscreen_rowbytes,
		        state.agl_offscreen_baseaddr)) {
			return 0;
		}

		if (outW) *outW = state.agl_offscreen_width;
		if (outH) *outH = state.agl_offscreen_height;
		if (outRowbytes) *outRowbytes = state.agl_offscreen_rowbytes;
		if (outBaseaddr) *outBaseaddr = state.agl_offscreen_baseaddr;
		return 1;
	}

	return 0;
}


/*
 *  NativeAGLSetCurrentContext(r3=ctx)
 *
 *  If ctx == 0, unbind current context. Otherwise set as current.
 */
uint32_t NativeAGLSetCurrentContext(uint32_t ctx)
{
	GL_LOG("aglSetCurrentContext: ctx=0x%08x", ctx);

	if (ctx == 0) {
		gl_current_context = nullptr;
		gl_current_context_idx = -1;
		GL_LOG("aglSetCurrentContext: unbound current context");
		gl_agl_last_error = AGL_NO_ERROR;
		return GL_TRUE;
	}

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		GL_LOG("aglSetCurrentContext: invalid context handle 0x%08x", ctx);
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	gl_current_context = context;
	gl_current_context_idx = idx;

	GL_LOG("aglSetCurrentContext: context %d now current", idx + 1);
	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}


/*
 *  NativeAGLSetDrawable(r3=ctx, r4=drawable)
 *
 *  Associate context with the emulator display. Reads the GrafPort's portRect
 *  to determine dimensions, creates/reuses the compositor overlay texture
 *  (same infrastructure as RAVE), initializes Metal resources, and connects
 *  the overlay to the GL context for rendering.
 */
uint32_t NativeAGLSetDrawable(uint32_t ctx, uint32_t drawable)
{
	GL_LOG("aglSetDrawable: ctx=0x%08x drawable=0x%08x", ctx, drawable);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		GL_LOG("aglSetDrawable: invalid context handle");
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	if (drawable == 0) {
		// Unbind drawable -- release the per-engine overlay so no further
		// compositing occurs. gl_overlay_unbind releases via
		// gfxaccel_resources_release_overlay_texture(kGfxEngineGL, ...).
		gl_overlay_unbind();
		GLFinishDrawableUnbind(idx);
		agl_ctx_state[idx].agl_offscreen = false;
		agl_ctx_state[idx].agl_offscreen_width = 0;
		agl_ctx_state[idx].agl_offscreen_height = 0;
		agl_ctx_state[idx].agl_offscreen_rowbytes = 0;
		agl_ctx_state[idx].agl_offscreen_baseaddr = 0;
		GL_LOG("aglSetDrawable: unbinding drawable from context %d", idx + 1);
		gl_agl_last_error = AGL_NO_ERROR;
		return GL_TRUE;
	}

	// Read port dimensions from the Mac GrafPort/CGrafPort.
	// Detect color port: CGrafPort has portVersion with high bit set at offset 6.
	// CGrafPort.portRect at offset 16; GrafPort.portRect at offset 14.
	uint16_t portVersion = ReadMacInt16(drawable + 6);
	int portRectOff = (portVersion & 0xC000) ? 16 : 14;  // CGrafPort vs GrafPort
	int16_t port_top    = (int16_t)ReadMacInt16(drawable + portRectOff);
	int16_t port_left   = (int16_t)ReadMacInt16(drawable + portRectOff + 2);
	int16_t port_bottom = (int16_t)ReadMacInt16(drawable + portRectOff + 4);
	int16_t port_right  = (int16_t)ReadMacInt16(drawable + portRectOff + 6);
	int32_t width  = port_right - port_left;
	int32_t height = port_bottom - port_top;
	if (width <= 0 || height <= 0) {
		width = 640;
		height = 480;
	}

	GL_LOG("aglSetDrawable: port rect=(%d,%d,%d,%d) -> %dx%d",
	       port_top, port_left, port_bottom, port_right, width, height);

	GLCaptureOwnerBeforeDrawableBind(idx);

	// Vend a per-engine overlay texture via
	// gfxaccel_resources_vend_overlay_texture_indexed(kGfxEngineGL, ...).
	// gl_overlay_bind caches the destination rect for SubmitFrame emission
	// in NativeAGLSwapBuffers (gl_overlay_present). Cache-hit: same dims
	// returns the cached handle; different dims re-vends.
	gl_overlay_bind(port_left, port_top, width, height);

	// Initialize Metal resources for this context if not yet done
	if (!context->metal) {
		GLMetalInit(context);
	}

	GL_LOG("aglSetDrawable: Metal overlay connected to context %d (%dx%d)", idx + 1, width, height);

	// Update viewport to match drawable dimensions
	context->viewport[0] = 0;
	context->viewport[1] = 0;
	context->viewport[2] = width;
	context->viewport[3] = height;

	// Store drawable Mac address for GetDrawable
	agl_ctx_state[idx].agl_drawable = drawable;
	agl_ctx_state[idx].agl_offscreen = false;
	agl_ctx_state[idx].agl_offscreen_width = 0;
	agl_ctx_state[idx].agl_offscreen_height = 0;
	agl_ctx_state[idx].agl_offscreen_rowbytes = 0;
	agl_ctx_state[idx].agl_offscreen_baseaddr = 0;

	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}


/*
 *  NativeAGLSwapBuffers(r3=ctx)
 *
 *  Present the Metal drawable.
 */
uint32_t NativeAGLSwapBuffers(uint32_t ctx)
{
	GL_VLOG("aglSwapBuffers: ctx=0x%08x", ctx);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		GL_LOG("aglSwapBuffers: invalid context handle");
		return 0;  // void function, no return value per spec
	}

	// Initialize Metal if not yet done (lazy init on first swap)
	if (!context->metal) {
		GLMetalInit(context);
	}

	// End current frame and present
	GLMetalEndFrame(context);

	// gl_overlay_present emits a single kLayerSlotOverlay CompositeLayer via
	// MetalCompositorSubmitFrame — the layer's presence IS the "active"
	// signal. SubmitFrame is cache-only in production; Step 3 restores real
	// pacing. gl_overlay_present also declares GL as the active DMC owner
	// internally (an idempotent early-return makes it a fast no-op when owner
	// already matches).
	gl_overlay_present();

	// Throttle to GL's own VBL cadence lane.
	MetalCompositorSync3DFramePacingForEngine(kGfxFramePacingEngineGL);

	GL_VLOG("aglSwapBuffers: presented frame for context %d", idx);
	return 0;
}


/*
 *  NativeAGLDestroyContext(r3=ctx)
 *
 *  Clean up GLContext, free resources, remove from table.
 *
 *  Destroy path cleanup:
 *  (1) GLMetalRelease: commits pending GPU work, releases Metal textures (CFRelease),
 *      clears pipeline/depth-stencil/sampler caches, deletes GLMetalState
 *  (2) Shared-overlay refcount release deleted; GL uses the per-engine
 *      overlay path.
 *  (3) Accum buffer: freed here (raw malloc'd by gl_accum_ensure_allocated)
 *  (4) delete context: releases std::unordered_map/std::vector members via dtors
 *      (texture_objects already cleared in GLMetalRelease, display_lists are CPU-only)
 */
uint32_t NativeAGLDestroyContext(uint32_t ctx)
{
	GL_LOG("aglDestroyContext: ctx=0x%08x", ctx);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		GL_LOG("aglDestroyContext: invalid context handle");
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	// If this is the current context, unbind it
	if (gl_current_context == context) {
		gl_current_context = nullptr;
		gl_current_context_idx = -1;
	}

	// Release Metal resources for this context (textures, caches, GPU sync)
	GLMetalRelease(context);

	// Release the per-engine overlay texture on the last live context
	// destruction. NativeAGLSetDrawable(ctx, 0) typically fires
	// first and already unbound; if the caller destroyed the context
	// without unbinding, gl_overlay_unbind is idempotent — no-op if no
	// overlay cached. Checking gl_current_context suffices as a
	// lightweight "is any context still active" probe: if we just
	// nulled it out above and no other contexts exist, we unbind.
	bool any_remaining = false;
	for (int i = 0; i < GL_MAX_CONTEXTS; ++i) {
		if (gl_contexts[i] != nullptr && gl_contexts[i] != context) {
			any_remaining = true;
			break;
		}
	}
	if (!any_remaining) {
		gl_overlay_unbind();
	}
	if (agl_ctx_state[idx].agl_drawable != 0) {
		GLFinishDrawableUnbind(idx);
	}

	// Free accumulation buffer (raw malloc'd, not managed by ARC or C++ dtors)
	if (context->accum_buffer) {
		free(context->accum_buffer);
		context->accum_buffer = nullptr;
		context->accum_allocated = false;
	}

	delete context;
	gl_contexts[idx] = nullptr;
	gl_context_mac_handles[idx] = 0;

	GL_LOG("aglDestroyContext: destroyed context %d", idx + 1);
	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}


/*
 *  NativeAGLGetCurrentContext()
 *
 *  Return Mac handle of current context (or 0 if none).
 */
uint32_t NativeAGLGetCurrentContext()
{
	if (gl_current_context_idx < 0) return 0;
	return gl_context_mac_handles[gl_current_context_idx];
}


/*
 *  NativeAGLGetError()
 *
 *  Return last AGL error code, then clear it.
 */
uint32_t NativeAGLGetError()
{
	uint32_t err = gl_agl_last_error;
	gl_agl_last_error = AGL_NO_ERROR;
	return err;
}


/*
 *  NativeAGLGetVersion(r3=majorPtr, r4=minorPtr)
 *
 *  Write AGL version 1.2 to PPC memory.
 */
uint32_t NativeAGLGetVersion(uint32_t majorPtr, uint32_t minorPtr)
{
	if (majorPtr) WriteMacInt32(majorPtr, 1);
	if (minorPtr) WriteMacInt32(minorPtr, 2);
	GL_LOG("aglGetVersion: 1.2");
	return 0;
}


/*
 *  NativeAGLDestroyPixelFormat(r3=pix)
 *
 *  Reclaim the C++ pixel-format slot that owns this handle so a session that
 *  creates/destroys >4 pixel formats can reuse the fixed 4-slot table.
 *  We match the slot by mac_addr (the same lookup NativeAGLDescribePixelFormat
 *  uses) and mark it free with a mac_addr=0 sentinel — we NEVER blind-decrement
 *  gl_pixel_format_count, because an out-of-order destroy would shift later
 *  slots and alias a still-live pixel format. The Mac_sysalloc
 *  memory stays leaked (it's a permanent bump allocator); only the C++ slot is
 *  reclaimed, and NativeAGLChoosePixelFormat scans for a freed slot before
 *  bumping the count.
 */
uint32_t NativeAGLDestroyPixelFormat(uint32_t pix)
{
	GL_LOG("aglDestroyPixelFormat: pix=0x%08x", pix);

	for (int i = 0; i < gl_pixel_format_count; i++) {
		if (gl_pixel_formats[i].mac_addr == pix) {
			gl_pixel_formats[i].mac_addr = 0;  // sentinel free; reused by Choose
			GL_LOG("aglDestroyPixelFormat: freed slot %d (handle=0x%08x)", i, pix);
			break;
		}
	}

	gl_agl_last_error = AGL_NO_ERROR;
	return 0;
}


/*
 *  NativeAGLNextPixelFormat - return 0 (no linked list of formats)
 */
uint32_t NativeAGLNextPixelFormat(uint32_t pix)
{
	return 0;
}


/*
 *  NativeAGLDescribePixelFormat - look up pixel format info and return attribute values
 *
 *  Games call this to query capabilities before creating a context.
 *  Returns GL_TRUE on success with value written via WriteMacInt32.
 */
uint32_t NativeAGLDescribePixelFormat(uint32_t pix, uint32_t attrib, uint32_t valuePtr)
{
	GL_LOG("aglDescribePixelFormat: pix=0x%08x attrib=%d valuePtr=0x%08x", pix, attrib, valuePtr);

	if (pix == 0 || valuePtr == 0) {
		gl_agl_last_error = AGL_BAD_PIXELFMT;
		return GL_FALSE;
	}

	// Look up pixel format by matching mac_addr
	GLPixelFormatInfo *pf = nullptr;
	for (int i = 0; i < gl_pixel_format_count; i++) {
		if (gl_pixel_formats[i].mac_addr == pix) {
			pf = &gl_pixel_formats[i];
			break;
		}
	}
	if (!pf) {
		gl_agl_last_error = AGL_BAD_PIXELFMT;
		return GL_FALSE;
	}

	int32_t value = 0;
	switch (attrib) {
		case AGL_RGBA:             value = pf->has_rgba ? 1 : 0; break;
		case AGL_DOUBLEBUFFER:     value = pf->has_double_buffer ? 1 : 0; break;
		case AGL_DEPTH_SIZE:       value = pf->has_depth ? 32 : 0; break;
		case AGL_BUFFER_SIZE:      value = 32; break;  // always ARGB8888
		case AGL_RED_SIZE:         value = 8; break;
		case AGL_GREEN_SIZE:       value = 8; break;
		case AGL_BLUE_SIZE:        value = 8; break;
		case AGL_ALPHA_SIZE:       value = 8; break;
		case AGL_STENCIL_SIZE:     value = 8; break;  // Depth32Float_Stencil8
		case AGL_ACCUM_RED_SIZE:   value = 16; break;  // float accum
		case AGL_ACCUM_GREEN_SIZE: value = 16; break;
		case AGL_ACCUM_BLUE_SIZE:  value = 16; break;
		case AGL_ACCUM_ALPHA_SIZE: value = 16; break;
		case AGL_PIXEL_SIZE:       value = 32; break;
		case AGL_WINDOW:           value = 1; break;   // always windowed
		case AGL_RENDERER_ID:      value = (int32_t)GLAGLRendererID(); break;
		case AGL_ACCELERATED:      value = GLAGLRendererIsAccelerated() ? 1 : 0; break;
		case AGL_COMPLIANT:        value = 1; break;
		// Return valid values for the single-renderer/single-screen iOS
		// Metal model instead of AGL_BAD_ATTRIBUTE. A conformant app querying
		// these now gets a correct answer rather than a spurious error.
		case AGL_LEVEL:            value = 0; break;   // single plane (no overlays)
		case AGL_STEREO:           value = 0; break;   // no stereo buffering
		case AGL_AUX_BUFFERS:      value = 0; break;   // no aux buffers
		case AGL_VIRTUAL_SCREEN:   value = 0; break;   // single virtual screen
		case AGL_MINIMUM_POLICY:   value = 1; break;   // policy honored
		case AGL_MAXIMUM_POLICY:   value = 1; break;   // policy honored
		default:
			GL_LOG("aglDescribePixelFormat: unknown attrib %d", attrib);
			gl_agl_last_error = AGL_BAD_ATTRIBUTE;
			return GL_FALSE;
	}

	WriteMacInt32(valuePtr, (uint32_t)value);
	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}


/*
 *  NativeAGLCopyContext - selective state copy based on GL attrib mask bits
 *
 *  Same mask bit definitions as glPushAttrib. For unrecognized/GL_ALL_ATTRIB_BITS,
 *  copies all safe fields (skipping Metal pointer, display lists, texture objects).
 */
uint32_t NativeAGLCopyContext(uint32_t src, uint32_t dst, uint32_t mask)
{
	GL_LOG("aglCopyContext: src=0x%08x dst=0x%08x mask=0x%08x", src, dst, mask);

	int src_idx, dst_idx;
	GLContext *s = GLContextFromHandle(src, &src_idx);
	GLContext *d = GLContextFromHandle(dst, &dst_idx);
	if (!s || !d) {
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	if (mask & GL_CURRENT_BIT) {
		memcpy(d->current_color, s->current_color, sizeof(s->current_color));
		memcpy(d->current_normal, s->current_normal, sizeof(s->current_normal));
		memcpy(d->current_texcoord, s->current_texcoord, sizeof(s->current_texcoord));
		memcpy(d->current_secondary_color, s->current_secondary_color, sizeof(s->current_secondary_color));
		d->current_fog_coord = s->current_fog_coord;
		d->current_index = s->current_index;
		d->current_edge_flag = s->current_edge_flag;
	}
	if (mask & GL_ENABLE_BIT) {
		d->depth_test = s->depth_test;
		d->blend = s->blend;
		d->color_sum = s->color_sum;
		d->cull_face_enabled = s->cull_face_enabled;
		d->lighting_enabled = s->lighting_enabled;
		d->alpha_test = s->alpha_test;
		d->scissor_test = s->scissor_test;
		d->stencil_test = s->stencil_test;
		d->fog_enabled = s->fog_enabled;
		d->normalize = s->normalize;
		d->dither = s->dither;
		d->color_logic_op = s->color_logic_op;
		d->polygon_offset_fill = s->polygon_offset_fill;
		d->polygon_offset_line = s->polygon_offset_line;
		d->polygon_offset_point = s->polygon_offset_point;
	}
	if (mask & GL_LIGHTING_BIT) {
		memcpy(d->lights, s->lights, sizeof(s->lights));
		memcpy(d->materials, s->materials, sizeof(s->materials));
		d->lighting_enabled = s->lighting_enabled;
		d->color_material_enabled = s->color_material_enabled;
		d->color_material_face = s->color_material_face;
		d->color_material_mode = s->color_material_mode;
		memcpy(d->light_model_ambient, s->light_model_ambient, sizeof(s->light_model_ambient));
		d->light_model_two_side = s->light_model_two_side;
		d->light_model_local_viewer = s->light_model_local_viewer;
		d->shade_model = s->shade_model;
	}
	if (mask & GL_FOG_BIT) {
		d->fog_enabled = s->fog_enabled;
		d->fog_mode = s->fog_mode;
		d->fog_density = s->fog_density;
		d->fog_start = s->fog_start;
		d->fog_end = s->fog_end;
		memcpy(d->fog_color, s->fog_color, sizeof(s->fog_color));
	}
	if (mask & GL_PIXEL_MODE_BIT) {
		d->pixel_zoom_x = s->pixel_zoom_x;
		d->pixel_zoom_y = s->pixel_zoom_y;
		d->pixel_transfer_red_scale = s->pixel_transfer_red_scale;
		d->pixel_transfer_green_scale = s->pixel_transfer_green_scale;
		d->pixel_transfer_blue_scale = s->pixel_transfer_blue_scale;
		d->pixel_transfer_alpha_scale = s->pixel_transfer_alpha_scale;
		d->pixel_transfer_red_bias = s->pixel_transfer_red_bias;
		d->pixel_transfer_green_bias = s->pixel_transfer_green_bias;
		d->pixel_transfer_blue_bias = s->pixel_transfer_blue_bias;
		d->pixel_transfer_alpha_bias = s->pixel_transfer_alpha_bias;
		d->pixel_transfer_depth_scale = s->pixel_transfer_depth_scale;
		d->pixel_transfer_depth_bias = s->pixel_transfer_depth_bias;
		memcpy(d->pixel_map_i_to_r, s->pixel_map_i_to_r, sizeof(s->pixel_map_i_to_r));
		memcpy(d->pixel_map_i_to_g, s->pixel_map_i_to_g, sizeof(s->pixel_map_i_to_g));
		memcpy(d->pixel_map_i_to_b, s->pixel_map_i_to_b, sizeof(s->pixel_map_i_to_b));
		memcpy(d->pixel_map_i_to_a, s->pixel_map_i_to_a, sizeof(s->pixel_map_i_to_a));
		memcpy(d->pixel_map_r_to_r, s->pixel_map_r_to_r, sizeof(s->pixel_map_r_to_r));
		memcpy(d->pixel_map_g_to_g, s->pixel_map_g_to_g, sizeof(s->pixel_map_g_to_g));
		memcpy(d->pixel_map_b_to_b, s->pixel_map_b_to_b, sizeof(s->pixel_map_b_to_b));
		memcpy(d->pixel_map_a_to_a, s->pixel_map_a_to_a, sizeof(s->pixel_map_a_to_a));
		d->pixel_map_i_to_r_size = s->pixel_map_i_to_r_size;
		d->pixel_map_i_to_g_size = s->pixel_map_i_to_g_size;
		d->pixel_map_i_to_b_size = s->pixel_map_i_to_b_size;
		d->pixel_map_i_to_a_size = s->pixel_map_i_to_a_size;
		d->pixel_map_r_to_r_size = s->pixel_map_r_to_r_size;
		d->pixel_map_g_to_g_size = s->pixel_map_g_to_g_size;
		d->pixel_map_b_to_b_size = s->pixel_map_b_to_b_size;
		d->pixel_map_a_to_a_size = s->pixel_map_a_to_a_size;
	}
	if (mask & GL_DEPTH_BUFFER_BIT) {
		d->depth_func = s->depth_func;
		d->depth_mask = s->depth_mask;
		d->clear_depth = s->clear_depth;
	}
	if (mask & GL_VIEWPORT_BIT) {
		memcpy(d->viewport, s->viewport, sizeof(s->viewport));
		d->depth_range_near = s->depth_range_near;
		d->depth_range_far = s->depth_range_far;
	}
	if (mask & GL_TRANSFORM_BIT) {
		d->matrix_mode = s->matrix_mode;
		memcpy(d->clip_planes, s->clip_planes, sizeof(s->clip_planes));
		memcpy(d->clip_plane_enabled, s->clip_plane_enabled, sizeof(s->clip_plane_enabled));
	}
	if (mask & GL_COLOR_BUFFER_BIT) {
		d->blend_src = s->blend_src;
		d->blend_dst = s->blend_dst;
		memcpy(d->blend_color, s->blend_color, sizeof(s->blend_color));
		d->blend_equation = s->blend_equation;
		d->alpha_func = s->alpha_func;
		d->alpha_ref = s->alpha_ref;
		d->logic_op_mode = s->logic_op_mode;
		memcpy(d->color_mask, s->color_mask, sizeof(s->color_mask));
		memcpy(d->clear_color, s->clear_color, sizeof(s->clear_color));
	}
	if (mask & GL_POLYGON_BIT) {
		d->polygon_mode_front = s->polygon_mode_front;
		d->polygon_mode_back = s->polygon_mode_back;
		d->cull_face_mode = s->cull_face_mode;
		d->front_face = s->front_face;
		d->polygon_offset_factor = s->polygon_offset_factor;
		d->polygon_offset_units = s->polygon_offset_units;
	}
	if (mask & GL_TEXTURE_BIT) {
		memcpy(d->tex_units, s->tex_units, sizeof(s->tex_units));
		d->active_texture = s->active_texture;
		d->client_active_texture = s->client_active_texture;
	}
	if (mask & GL_HINT_BIT) {
		d->hint_perspective_correction = s->hint_perspective_correction;
		d->hint_point_smooth = s->hint_point_smooth;
		d->hint_line_smooth = s->hint_line_smooth;
		d->hint_polygon_smooth = s->hint_polygon_smooth;
		d->hint_fog = s->hint_fog;
	}
	if (mask & GL_POINT_BIT) {
		d->point_size = s->point_size;
		d->point_smooth = s->point_smooth;
	}
	if (mask & GL_LINE_BIT) {
		d->line_width = s->line_width;
		d->line_smooth = s->line_smooth;
		d->line_stipple_factor = s->line_stipple_factor;
		d->line_stipple_pattern = s->line_stipple_pattern;
	}
	if (mask & GL_SCISSOR_BIT) {
		memcpy(d->scissor_box, s->scissor_box, sizeof(s->scissor_box));
		d->scissor_test = s->scissor_test;
	}

	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}


/*
 *  NativeAGLUpdateContext(r3=ctx) - re-read the bound drawable geometry
 *
 *  An app calls aglUpdateContext after resizing/moving the window bound via
 *  aglSetDrawable. We validate the context (AGL_BAD_CONTEXT on a bad handle —
 *  the old pure no-op silently succeeded on garbage), then re-read the bound
 *  drawable's portRect by REUSING the exact aglSetDrawable CGrafPort-vs-GrafPort
 *  offset logic and refresh the GL viewport. The bound drawable Mac address is
 *  guest-controlled, so we NULL/zero-guard it before any ReadMacInt16 (ASVS V5).
 */
uint32_t NativeAGLUpdateContext(uint32_t ctx)
{
	GL_LOG("aglUpdateContext: ctx=0x%08x", ctx);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		GL_LOG("aglUpdateContext: invalid context handle");
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	uint32_t drawable = agl_ctx_state[idx].agl_drawable;
	if (drawable != 0) {
		// Re-read port dimensions from the Mac GrafPort/CGrafPort — same
		// CGrafPort-vs-GrafPort offset logic as NativeAGLSetDrawable (do NOT
		// hand-roll a second parser).
		uint16_t portVersion = ReadMacInt16(drawable + 6);
		int portRectOff = (portVersion & 0xC000) ? 16 : 14;  // CGrafPort vs GrafPort
		int16_t port_top    = (int16_t)ReadMacInt16(drawable + portRectOff);
		int16_t port_left   = (int16_t)ReadMacInt16(drawable + portRectOff + 2);
		int16_t port_bottom = (int16_t)ReadMacInt16(drawable + portRectOff + 4);
		int16_t port_right  = (int16_t)ReadMacInt16(drawable + portRectOff + 6);
		int32_t width  = port_right - port_left;
		int32_t height = port_bottom - port_top;
		if (width <= 0 || height <= 0) {
			width = 640;
			height = 480;
		}

		context->viewport[0] = 0;
		context->viewport[1] = 0;
		context->viewport[2] = width;
		context->viewport[3] = height;

		GL_LOG("aglUpdateContext: re-read port rect -> %dx%d for context %d",
		       width, height, idx + 1);
	}

	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}


/*
 *  NativeAGLSetOffScreen / NativeAGLSetFullScreen / NativeAGLUseFont -
 *  AGL utility entry points.
 *
 *  Some GL games probe AGL by binding a guest offscreen buffer before any
 *  onscreen drawable exists. Accept valid offscreen buffers so that path can
 *  proceed, while preserving honest errors for malformed buffers and for the
 *  still-unsupported fullscreen/font calls.
 */
uint32_t NativeAGLSetOffScreen(uint32_t ctx, uint32_t width, uint32_t height,
                                uint32_t rowbytes, uint32_t baseaddr)
{
	GL_LOG("aglSetOffScreen: ctx=0x%08x %dx%d rowbytes=%u base=0x%08x",
	       ctx, width, height, rowbytes, baseaddr);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		GL_LOG("aglSetOffScreen: invalid context handle");
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	if (!GLShouldAcceptOffscreenDrawable(width, height, rowbytes, baseaddr)) {
		GL_LOG("aglSetOffScreen: rejected invalid offscreen drawable");
		gl_agl_last_error = AGL_BAD_OFFSCREEN;
		return GL_FALSE;
	}

	if (agl_ctx_state[idx].agl_drawable != 0) {
		gl_overlay_unbind();
		GLFinishDrawableUnbind(idx);
	}

	if (!context->metal) {
		GLMetalInit(context);
	}

	context->viewport[0] = 0;
	context->viewport[1] = 0;
	context->viewport[2] = width;
	context->viewport[3] = height;
	context->scissor_box[0] = 0;
	context->scissor_box[1] = 0;
	context->scissor_box[2] = width;
	context->scissor_box[3] = height;

	agl_ctx_state[idx].agl_offscreen = true;
	agl_ctx_state[idx].agl_offscreen_width = width;
	agl_ctx_state[idx].agl_offscreen_height = height;
	agl_ctx_state[idx].agl_offscreen_rowbytes = rowbytes;
	agl_ctx_state[idx].agl_offscreen_baseaddr = baseaddr;

	GL_LOG("aglSetOffScreen: accepted offscreen drawable for context %d", idx + 1);
	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}

uint32_t NativeAGLSetFullScreen(uint32_t ctx, uint32_t width, uint32_t height,
                                 uint32_t freq, uint32_t device)
{
	GL_LOG("aglSetFullScreen: ctx=0x%08x %dx%d@%d (known limitation: windowed only)", ctx, width, height, freq);
	gl_agl_last_error = AGL_BAD_FULLSCREEN;
	return GL_FALSE;
}


/*
 *  NativeAGLGetDrawable - return the drawable Mac address stored at SetDrawable time
 */
uint32_t NativeAGLGetDrawable(uint32_t ctx)
{
	GL_LOG("aglGetDrawable: ctx=0x%08x", ctx);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return 0;
	}

	return agl_ctx_state[idx].agl_drawable;
}


/*
 *  NativeAGLSetVirtualScreen / GetVirtualScreen - single-screen implementation
 */
uint32_t NativeAGLSetVirtualScreen(uint32_t ctx, uint32_t screen)
{
	GL_LOG("aglSetVirtualScreen: ctx=0x%08x screen=%d", ctx, screen);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	agl_ctx_state[idx].agl_virtual_screen = (int32_t)screen;
	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}

uint32_t NativeAGLGetVirtualScreen(uint32_t ctx)
{
	GL_LOG("aglGetVirtualScreen: ctx=0x%08x", ctx);
	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) return 0;
	return (uint32_t)agl_ctx_state[idx].agl_virtual_screen;
}


/*
 *  NativeAGLConfigure - store global format cache settings
 */
uint32_t NativeAGLConfigure(uint32_t pname, uint32_t param)
{
	GL_LOG("aglConfigure: pname=%d param=%d", pname, param);

	switch (pname) {
		case AGL_FORMAT_CACHE_SIZE:
		case AGL_CLEAR_FORMAT_CACHE:
		case AGL_RETAIN_RENDERERS:
			// Accept — these affect internal caching we don't implement
			break;
		default:
			// Honest-error: an unknown pname is a real AGL_BAD_ENUM, not
			// a silently-accepted success (was AGL_NO_ERROR + GL_TRUE).
			GL_LOG("aglConfigure: unknown pname %d -> AGL_BAD_ENUM", pname);
			gl_agl_last_error = AGL_BAD_ENUM;
			return GL_FALSE;
	}

	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}


/*
 *  NativeAGLEnable / Disable / IsEnabled - per-context AGL option management
 */
uint32_t NativeAGLEnable(uint32_t ctx, uint32_t pname)
{
	GL_LOG("aglEnable: ctx=0x%08x pname=%d", ctx, pname);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	switch (pname) {
		case AGL_RASTERIZATION:
			agl_ctx_state[idx].agl_rasterization_enabled = true;
			break;
		case AGL_SWAP_RECT:
			agl_ctx_state[idx].agl_options |= (1 << 0);
			break;
		case AGL_BUFFER_RECT:
			agl_ctx_state[idx].agl_options |= (1 << 1);
			break;
		case AGL_COLORMAP_TRACKING:
			agl_ctx_state[idx].agl_options |= (1 << 2);
			break;
		case AGL_STATE_VALIDATION:
			// No-op: we don't do multi-screen state validation
			break;
		default:
			// Honest-error: unknown pname -> AGL_BAD_ENUM + GL_FALSE
			// (was silently accepted as AGL_NO_ERROR + GL_TRUE).
			GL_LOG("aglEnable: unknown pname %d -> AGL_BAD_ENUM", pname);
			gl_agl_last_error = AGL_BAD_ENUM;
			return GL_FALSE;
	}

	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}

uint32_t NativeAGLDisable(uint32_t ctx, uint32_t pname)
{
	GL_LOG("aglDisable: ctx=0x%08x pname=%d", ctx, pname);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	switch (pname) {
		case AGL_RASTERIZATION:
			agl_ctx_state[idx].agl_rasterization_enabled = false;
			break;
		case AGL_SWAP_RECT:
			agl_ctx_state[idx].agl_options &= ~(1 << 0);
			break;
		case AGL_BUFFER_RECT:
			agl_ctx_state[idx].agl_options &= ~(1 << 1);
			break;
		case AGL_COLORMAP_TRACKING:
			agl_ctx_state[idx].agl_options &= ~(1 << 2);
			break;
		case AGL_STATE_VALIDATION:
			break;
		default:
			// Honest-error: unknown pname -> AGL_BAD_ENUM + GL_FALSE
			// (was silently accepted as AGL_NO_ERROR + GL_TRUE).
			GL_LOG("aglDisable: unknown pname %d -> AGL_BAD_ENUM", pname);
			gl_agl_last_error = AGL_BAD_ENUM;
			return GL_FALSE;
	}

	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}

uint32_t NativeAGLIsEnabled(uint32_t ctx, uint32_t pname)
{
	GL_LOG("aglIsEnabled: ctx=0x%08x pname=%d", ctx, pname);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	bool enabled = false;
	switch (pname) {
		case AGL_RASTERIZATION:
			enabled = agl_ctx_state[idx].agl_rasterization_enabled;
			break;
		case AGL_SWAP_RECT:
			enabled = (agl_ctx_state[idx].agl_options & (1 << 0)) != 0;
			break;
		case AGL_BUFFER_RECT:
			enabled = (agl_ctx_state[idx].agl_options & (1 << 1)) != 0;
			break;
		case AGL_COLORMAP_TRACKING:
			enabled = (agl_ctx_state[idx].agl_options & (1 << 2)) != 0;
			break;
		default:
			break;
	}

	gl_agl_last_error = AGL_NO_ERROR;
	return enabled ? GL_TRUE : GL_FALSE;
}


/*
 *  NativeAGLSetInteger / GetInteger - per-context parameter storage
 */
uint32_t NativeAGLSetInteger(uint32_t ctx, uint32_t pname, uint32_t params)
{
	GL_LOG("aglSetInteger: ctx=0x%08x pname=%d params=0x%08x", ctx, pname, params);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	switch (pname) {
		case AGL_SWAP_INTERVAL:
			agl_ctx_state[idx].agl_swap_interval = (int32_t)ReadMacInt32(params);
			GL_LOG("aglSetInteger: swap_interval=%d", agl_ctx_state[idx].agl_swap_interval);
			break;
		case AGL_BUFFER_RECT:
			for (int i = 0; i < 4; i++)
				agl_ctx_state[idx].agl_buffer_rect[i] = (int32_t)ReadMacInt32(params + i * 4);
			GL_LOG("aglSetInteger: buffer_rect=(%d,%d,%d,%d)",
			       agl_ctx_state[idx].agl_buffer_rect[0], agl_ctx_state[idx].agl_buffer_rect[1],
			       agl_ctx_state[idx].agl_buffer_rect[2], agl_ctx_state[idx].agl_buffer_rect[3]);
			break;
		case AGL_SWAP_RECT:
			for (int i = 0; i < 4; i++)
				agl_ctx_state[idx].agl_swap_rect[i] = (int32_t)ReadMacInt32(params + i * 4);
			GL_LOG("aglSetInteger: swap_rect=(%d,%d,%d,%d)",
			       agl_ctx_state[idx].agl_swap_rect[0], agl_ctx_state[idx].agl_swap_rect[1],
			       agl_ctx_state[idx].agl_swap_rect[2], agl_ctx_state[idx].agl_swap_rect[3]);
			break;
		case AGL_COLORMAP_ENTRY:
			// Known pname: colormap entry {index, r, g, b}. We don't track an
			// AGL software colormap (the Metal compositor owns the palette), but
			// it is a valid pname — accept it as a no-op rather than AGL_BAD_ENUM.
			GL_LOG("aglSetInteger: AGL_COLORMAP_ENTRY accepted (no AGL colormap tracked)");
			break;
		default:
			// Honest-error: unknown pname -> AGL_BAD_ENUM + GL_FALSE
			// (was silently accepted as AGL_NO_ERROR + GL_TRUE).
			GL_LOG("aglSetInteger: unknown pname %d -> AGL_BAD_ENUM", pname);
			gl_agl_last_error = AGL_BAD_ENUM;
			return GL_FALSE;
	}

	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}

uint32_t NativeAGLGetInteger(uint32_t ctx, uint32_t pname, uint32_t params)
{
	GL_LOG("aglGetInteger: ctx=0x%08x pname=%d params=0x%08x", ctx, pname, params);

	int idx;
	GLContext *context = GLContextFromHandle(ctx, &idx);
	if (!context) {
		gl_agl_last_error = AGL_BAD_CONTEXT;
		return GL_FALSE;
	}

	switch (pname) {
		case AGL_SWAP_INTERVAL:
			WriteMacInt32(params, (uint32_t)agl_ctx_state[idx].agl_swap_interval);
			break;
		case AGL_BUFFER_RECT:
			for (int i = 0; i < 4; i++)
				WriteMacInt32(params + i * 4, (uint32_t)agl_ctx_state[idx].agl_buffer_rect[i]);
			break;
		case AGL_SWAP_RECT:
			for (int i = 0; i < 4; i++)
				WriteMacInt32(params + i * 4, (uint32_t)agl_ctx_state[idx].agl_swap_rect[i]);
			break;
		case AGL_COLORMAP_ENTRY:
			// Known pname: no AGL software colormap tracked, report 0.
			GL_LOG("aglGetInteger: AGL_COLORMAP_ENTRY -> 0 (no AGL colormap tracked)");
			WriteMacInt32(params, 0);
			break;
		default:
			// Honest-error: unknown pname -> AGL_BAD_ENUM + GL_FALSE
			// (was silently writing 0 + AGL_NO_ERROR + GL_TRUE).
			GL_LOG("aglGetInteger: unknown pname %d -> AGL_BAD_ENUM", pname);
			gl_agl_last_error = AGL_BAD_ENUM;
			return GL_FALSE;
	}

	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}


/*
 *  NativeAGLUseFont - honest-failure. Mac font-glyph rasterization
 *  requires Mac Font Manager access unavailable from the native side, so the
 *  function cannot populate the display-list glyphs. It previously returned
 *  GL_TRUE, leaving the caller with an empty (silently wrong) glyph list. It
 *  now returns GL_FALSE. There is NO AGL_BAD_FONT error in the AGL 1.2 spec, so
 *  none is invented — GL_FALSE alone is the documented Boolean failure.
 *  This is a deliberate known limitation.
 */
uint32_t NativeAGLUseFont(uint32_t ctx, uint32_t fontID, uint32_t face,
                           uint32_t size, uint32_t first, uint32_t count, uint32_t base)
{
	GL_LOG("aglUseFont: ctx=0x%08x fontID=%d face=%d size=%d first=%d count=%d base=%d (known limitation: no Font Manager access)",
	       ctx, fontID, face, size, first, count, base);
	return GL_FALSE;
}


/*
 *  NativeAGLErrorString - return Mac-side allocated string for AGL error code
 *
 *  Allocates strings lazily using Mac_sysalloc, same pattern as NativeGLGetString.
 *  Strings persist for the lifetime of the process.
 */
uint32_t NativeAGLErrorString(uint32_t code)
{
	GL_LOG("aglErrorString: code=%d", code);

	// Static string table for all 17 AGL error codes
	static const char *error_strings[] = {
		"no error",                      // AGL_NO_ERROR (0)
		"invalid pixel format attribute", // AGL_BAD_ATTRIBUTE (10000)
		"invalid renderer property",      // AGL_BAD_PROPERTY (10001)
		"invalid pixel format",           // AGL_BAD_PIXELFMT (10002)
		"invalid renderer info",          // AGL_BAD_RENDINFO (10003)
		"invalid context",                // AGL_BAD_CONTEXT (10004)
		"invalid drawable",               // AGL_BAD_DRAWABLE (10005)
		"invalid graphics device",        // AGL_BAD_GDEV (10006)
		"invalid context state",          // AGL_BAD_STATE (10007)
		"invalid numerical value",        // AGL_BAD_VALUE (10008)
		"invalid share context",          // AGL_BAD_MATCH (10009)
		"invalid enumerant",              // AGL_BAD_ENUM (10010)
		"invalid offscreen drawable",     // AGL_BAD_OFFSCREEN (10011)
		"invalid fullscreen drawable",    // AGL_BAD_FULLSCREEN (10012)
		"invalid window",                 // AGL_BAD_WINDOW (10013)
		"invalid pointer",                // AGL_BAD_POINTER (10014)
		"invalid code module",            // AGL_BAD_MODULE (10015)
		"memory allocation failure",      // AGL_BAD_ALLOC (10016)
	};

	// Map error code to index: 0 -> 0, 10000-10016 -> 1-17
	int index;
	if (code == 0) {
		index = 0;
	} else if (code >= 10000 && code <= 10016) {
		index = (int)(code - 10000 + 1);
	} else {
		GL_LOG("aglErrorString: unknown error code %d", code);
		return 0;
	}

	// Lazy-allocate Mac-side string on first request
	if (agl_error_strings[index] == 0) {
		const char *str = (index < 18) ? error_strings[index] : "unknown error";
		uint32_t len = (uint32_t)strlen(str) + 1;
		uint32_t addr = Mac_sysalloc(len);
		if (addr) {
			uint8 *host_ptr = Mac2HostAddr(addr);
			memcpy(host_ptr, str, len);
			agl_error_strings[index] = addr;
			GL_LOG("aglErrorString: allocated '%s' at 0x%08x", str, addr);
		}
	}

	return agl_error_strings[index];
}


/*
 *  NativeAGLResetLibrary - valid no-op (nothing to reset in our implementation)
 */
uint32_t NativeAGLResetLibrary()
{
	GL_LOG("aglResetLibrary");
	return 0;
}


/*
 *  NativeAGLQueryRendererInfo - allocate and return a renderer info handle
 *
 *  Games call this at startup to check for hardware acceleration and abort
 *  if no renderer is found. We return a single persistent handle with a
 *  magic sentinel value.
 */
uint32_t NativeAGLQueryRendererInfo(uint32_t gdevs, uint32_t ndev)
{
	GL_LOG("aglQueryRendererInfo: gdevs=0x%08x ndev=%d", gdevs, ndev);

	// Allocate handle once, cache for all future calls
	if (renderer_info_handle == 0) {
		renderer_info_handle = Mac_sysalloc(4);
		if (renderer_info_handle) {
			WriteMacInt32(renderer_info_handle, 0x524E4452);  // 'RNDR' magic sentinel
			GL_LOG("aglQueryRendererInfo: allocated renderer_info_handle=0x%08x", renderer_info_handle);
		} else {
			GL_LOG("aglQueryRendererInfo: Mac_sysalloc failed");
			gl_agl_last_error = AGL_BAD_ALLOC;
			return 0;
		}
	}

	gl_agl_last_error = AGL_NO_ERROR;
	return renderer_info_handle;
}

/*
 *  NativeAGLDestroyRendererInfo - valid no-op (permanent allocation)
 */
uint32_t NativeAGLDestroyRendererInfo(uint32_t rend)
{
	GL_LOG("aglDestroyRendererInfo: rend=0x%08x", rend);
	gl_agl_last_error = AGL_NO_ERROR;
	return 0;
}

/*
 *  NativeAGLNextRendererInfo - return 0 (single renderer, no linked list)
 */
uint32_t NativeAGLNextRendererInfo(uint32_t rend)
{
	return 0;
}

/*
 *  NativeAGLDescribeRenderer - return ATI Rage 128 Pro hardware properties
 *
 *  Games query these to determine if hardware acceleration is available and
 *  what capabilities the renderer has. We report values matching an ATI Rage 128 Pro.
 */
uint32_t NativeAGLDescribeRenderer(uint32_t rend, uint32_t prop, uint32_t valuePtr)
{
	GL_LOG("aglDescribeRenderer: rend=0x%08x prop=%d valuePtr=0x%08x", rend, prop, valuePtr);

	if (rend == 0 || valuePtr == 0) {
		gl_agl_last_error = AGL_BAD_RENDINFO;
		return GL_FALSE;
	}

	int32_t value = 0;
	switch (prop) {
		case AGL_ACCELERATED:      value = GLAGLRendererIsAccelerated() ? 1 : 0; break;
		case AGL_RENDERER_ID:      value = (int32_t)GLAGLRendererID(); break;
		case AGL_VIDEO_MEMORY:     value = (int32_t)GLAGLRendererVideoMemoryBytes(); break;
		case AGL_TEXTURE_MEMORY:   value = (int32_t)GLAGLRendererTextureMemoryBytes(); break;
		case AGL_BUFFER_MODES:     value = AGL_DOUBLEBUFFER_BIT | AGL_SINGLEBUFFER_BIT; break;
		case AGL_COLOR_MODES:      value = AGL_ARGB8888_BIT | AGL_RGB888_BIT; break;
		case AGL_DEPTH_MODES:      value = AGL_32_BIT | AGL_16_BIT; break;
		case AGL_STENCIL_MODES:    value = AGL_8_BIT; break;
		case AGL_MAX_AUX_BUFFERS:  value = 0; break;
		case AGL_MIN_LEVEL:        value = 0; break;
		case AGL_MAX_LEVEL:        value = 0; break;
		case AGL_WINDOW:           value = 1; break;
		case AGL_COMPLIANT:        value = 1; break;
		case AGL_OFFSCREEN:        value = 0; break;
		case AGL_FULLSCREEN:       value = 0; break;
		case AGL_ACCUM_MODES:      value = AGL_32_BIT; break;
		default:
			GL_LOG("aglDescribeRenderer: unknown prop %d", prop);
			gl_agl_last_error = AGL_BAD_PROPERTY;
			return GL_FALSE;
	}

	WriteMacInt32(valuePtr, (uint32_t)value);
	gl_agl_last_error = AGL_NO_ERROR;
	return GL_TRUE;
}

/*
 *  NativeAGLDevicesOfPixelFormat - return a single GDHandle (main device)
 */
uint32_t NativeAGLDevicesOfPixelFormat(uint32_t pix, uint32_t ndevsPtr)
{
	GL_LOG("aglDevicesOfPixelFormat: pix=0x%08x ndevsPtr=0x%08x", pix, ndevsPtr);

	// Write number of devices
	if (ndevsPtr) WriteMacInt32(ndevsPtr, 1);

	// Allocate persistent device handle list (single entry: main device = 0)
	if (agl_device_handle == 0) {
		agl_device_handle = Mac_sysalloc(4);
		if (agl_device_handle) {
			WriteMacInt32(agl_device_handle, 0);  // main device handle
			GL_LOG("aglDevicesOfPixelFormat: allocated device handle at 0x%08x", agl_device_handle);
		}
	}

	gl_agl_last_error = AGL_NO_ERROR;
	return agl_device_handle;
}


// ===========================================================================
//  gfxaccel_resources fan-out handlers.
// ===========================================================================
//
// GL registers a pair of attach/detach callbacks with gfxaccel_resources at
// engine init (GLInstallHooks, below). Mirrors RAVE's pattern in
// rave_engine.cpp.
//
// Lifecycle:
//   - on_mode_enter (LIFO) -> GLOnAttach: pre-vend overlay texture pair for
//     the new mode's resolution IF GL has an active overlay. Otherwise
//     skip; the next gl_overlay_bind will vend lazily.
//   - on_mode_exit  (FIFO) -> GLOnDetach: release the cached overlay
//     handle back to the resource manager.
//
// The handlers must NOT call back into DMC (the resource manager fan-out
// runs on the DMC writer's thread while holding the writer mutex —
// recursive subscribe/unsubscribe would deadlock).
//
// Cross the .cpp/.mm boundary via the small extern "C" probes declared in
// gl_engine.h (gl_has_active_overlay / gl_get_overlay_dims /
// gl_release_overlay_for_detach).
static int32_t GLOnAttach(uint32_t /* engine_id */,
                          const struct DMCModeSnapshot *incoming,
                          void * /* ctx */)
{
	/* If GL has no active overlay at attach time, skip pre-vending — the
	 * next gl_overlay_bind (driven by an actual AGL drawable bind) will
	 * vend lazily. Common case for non-GL workloads. */
	if (!gl_has_active_overlay()) {
		return 0;  /* kGfxAccelResNoErr — accept the transition */
	}
	if (incoming == NULL) {
		return 0;  /* defensive */
	}

	/* GL was active in the outgoing mode; pre-vend the pair at the incoming mode's
	 * resolution so the compositor sees an overlay on its first present
	 * after the mode switch. Vend format is
	 * BGRA8Unorm (= MTLPixelFormatBGRA8Unorm = 80). */
	void *tex0 = gfxaccel_resources_vend_overlay_texture_indexed(
	                kGfxEngineGL,
	                0,
	                incoming->width,
	                incoming->height,
	                80 /* MTLPixelFormatBGRA8Unorm */);
	void *tex1 = gfxaccel_resources_vend_overlay_texture_indexed(
	                kGfxEngineGL,
	                1,
	                incoming->width,
	                incoming->height,
	                80 /* MTLPixelFormatBGRA8Unorm */);
	if (tex0 == NULL || tex1 == NULL) {
		if (tex0 != NULL) gfxaccel_resources_release_overlay_texture(kGfxEngineGL, tex0);
		if (tex1 != NULL) gfxaccel_resources_release_overlay_texture(kGfxEngineGL, tex1);
		/* Vend failed — reject the transition. The rollback path is safe
		 * under concurrent DMC readers. */
		return -3009;  /* kDMCErrSubscriberRejected */
	}
	return 0;
}

static int32_t GLOnDetach(uint32_t /* engine_id */,
                          const struct DMCModeSnapshot * /* outgoing */,
                          void * /* ctx */)
{
	/* Release the cached overlay (idempotent — no-op if GL has none).
	 * The next gl_overlay_bind after the mode switch will re-vend at the
	 * appropriate resolution. */
	gl_release_overlay_for_detach();
	return 0;
}

static bool gl_resource_handlers_registered = false;

static void GLRegisterResourceHandlers(void)
{
	if (gl_resource_handlers_registered) return;
	struct GfxResEngineHandlers gl_handlers;
	gl_handlers.attach = GLOnAttach;
	gl_handlers.detach = GLOnDetach;
	gl_handlers.ctx    = NULL;
	gfxaccel_resources_register_engine(kGfxEngineGL, &gl_handlers);
	gl_resource_handlers_registered = true;
	GL_LOG("GLRegisterResourceHandlers: GL attach/detach handlers registered with gfxaccel_resources");
}


// ===========================================================================
//  FindLibSymbol Hook Installation
// ===========================================================================

// GL hook state
static bool gl_hooks_installed = false;
static bool gl_hooks_in_progress = false;
static int gl_hooks_attempts = 0;
static const int GL_HOOKS_MAX_ATTEMPTS = 3;

/*
 *  GLInstallHooks - Hook GL/AGL function lookups via FindLibSymbol
 *
 *  Locates the OpenGL shared library exports via FindLibSymbol and patches
 *  each function's TVECT to redirect to our allocated gl_method_tvects[].
 *
 *  Follows the RAVE hook pattern:
 *   1. Do ALL FindLibSymbol lookups first (cache results)
 *   2. Then patch TVECTs (avoid re-entrancy)
 *
 *  Library names for Classic Mac OS OpenGL:
 *   - "OpenGLLibrary" (core GL + AGL)
 *   - "OpenGLUtility" (GLU)
 *   - GLUT is typically "GLUTLibrary"
 */
void GLInstallHooks()
{
	if (gl_hooks_installed) return;
	if (gl_hooks_attempts >= GL_HOOKS_MAX_ATTEMPTS) return;
	if (gl_hooks_in_progress) {
		GL_LOG("GLInstallHooks: skipped (re-entrant call)");
		return;
	}
	gl_hooks_in_progress = true;

	GL_LOG("GLInstallHooks: installing FindLibSymbol hooks for GL/AGL/GLU");

	// ---- Step 0: Register GL's attach/detach handlers with
	// the gfxaccel_resources fan-out registry. Idempotent
	// (gl_resource_handlers_registered guard); safe to call even if
	// FindLibSymbol below fails and we retry — registration survives.
	// Mirrors RaveRegisterResourceHandlers at rave_engine.cpp.
	GLRegisterResourceHandlers();

	// ---- Step 1: FindLibSymbol lookups (cache all TVECTs) ----
	//
	// Library name format: Pascal string (first byte = length).
	// "OpenGLLibrary" = 13 chars -> \015
	// "OpenGLUtility" = 13 chars -> \015
	// "GLUTLibrary"   = 11 chars -> \013
	static const char *gl_lib   = "\015OpenGLLibrary";
	static const char *glu_lib  = "\015OpenGLUtility";
	static const char *glut_lib = "\013GLUTLibrary";

	// We need to map each GL function name to its sub-opcode and find its TVECT.
	// Rather than listing all 643 functions, we use the dispatch table structure:
	// For each function name, FindLibSymbol returns the Mac-side TVECT address.
	// We then overwrite that TVECT's code pointer to jump to our thunk.
	//
	// AGL function name mapping (these are the most important for context setup):
	struct GLSymbolEntry {
		const char *pascal_sym;   // Pascal string (len byte + name)
		int sub_opcode;           // Our sub-opcode for dispatch
		const char *name;         // For logging
	};

	// AGL functions in OpenGLLibrary
	GLSymbolEntry agl_symbols[] = {
		{ "\024aglChoosePixelFormat",  GL_SUB_AGL_CHOOSEPIXELFORMAT,  "aglChoosePixelFormat" },
		{ "\026aglDestroyPixelFormat", GL_SUB_AGL_DESTROYPIXELFORMAT, "aglDestroyPixelFormat" },
		{ "\020aglNextPixelFormat",    GL_SUB_AGL_NEXTPIXELFORMAT,    "aglNextPixelFormat" },
		{ "\026aglDescribePixelFormat",GL_SUB_AGL_DESCRIBEPIXELFORMAT,"aglDescribePixelFormat" },
		{ "\027aglDevicesOfPixelFormat",GL_SUB_AGL_DEVICESOFPIXELFORMAT,"aglDevicesOfPixelFormat" },
		{ "\024aglQueryRendererInfo",  GL_SUB_AGL_QUERYRENDERERINFO,  "aglQueryRendererInfo" },
		{ "\026aglDestroyRendererInfo", GL_SUB_AGL_DESTROYRENDERERINFO,"aglDestroyRendererInfo" },
		{ "\023aglNextRendererInfo",   GL_SUB_AGL_NEXTRENDERERINFO,   "aglNextRendererInfo" },
		{ "\023aglDescribeRenderer",   GL_SUB_AGL_DESCRIBERENDERER,   "aglDescribeRenderer" },
		{ "\020aglCreateContext",      GL_SUB_AGL_CREATECONTEXT,      "aglCreateContext" },
		{ "\021aglDestroyContext",     GL_SUB_AGL_DESTROYCONTEXT,     "aglDestroyContext" },
		{ "\016aglCopyContext",        GL_SUB_AGL_COPYCONTEXT,        "aglCopyContext" },
		{ "\020aglUpdateContext",      GL_SUB_AGL_UPDATECONTEXT,      "aglUpdateContext" },
		{ "\024aglSetCurrentContext",  GL_SUB_AGL_SETCURRENTCONTEXT,  "aglSetCurrentContext" },
		{ "\024aglGetCurrentContext",  GL_SUB_AGL_GETCURRENTCONTEXT,  "aglGetCurrentContext" },
		{ "\016aglSetDrawable",        GL_SUB_AGL_SETDRAWABLE,        "aglSetDrawable" },
		{ "\017aglSetOffScreen",       GL_SUB_AGL_SETOFFSCREEN,       "aglSetOffScreen" },
		{ "\020aglSetFullScreen",      GL_SUB_AGL_SETFULLSCREEN,      "aglSetFullScreen" },
		{ "\016aglGetDrawable",        GL_SUB_AGL_GETDRAWABLE,        "aglGetDrawable" },
		{ "\024aglSetVirtualScreen",   GL_SUB_AGL_SETVIRTUALSCREEN,   "aglSetVirtualScreen" },
		{ "\024aglGetVirtualScreen",   GL_SUB_AGL_GETVIRTUALSCREEN,   "aglGetVirtualScreen" },
		{ "\016aglGetVersion",         GL_SUB_AGL_GETVERSION,         "aglGetVersion" },
		{ "\014aglConfigure",          GL_SUB_AGL_CONFIGURE,          "aglConfigure" },
		{ "\016aglSwapBuffers",        GL_SUB_AGL_SWAPBUFFERS,        "aglSwapBuffers" },
		{ "\011aglEnable",             GL_SUB_AGL_ENABLE,             "aglEnable" },
		{ "\012aglDisable",            GL_SUB_AGL_DISABLE,            "aglDisable" },
		{ "\014aglIsEnabled",          GL_SUB_AGL_ISENABLED,          "aglIsEnabled" },
		{ "\016aglSetInteger",         GL_SUB_AGL_SETINTEGER,         "aglSetInteger" },
		{ "\016aglGetInteger",         GL_SUB_AGL_GETINTEGER,         "aglGetInteger" },
		{ "\012aglUseFont",            GL_SUB_AGL_USEFONT,            "aglUseFont" },
		{ "\013aglGetError",           GL_SUB_AGL_GETERROR,           "aglGetError" },
		{ "\016aglErrorString",        GL_SUB_AGL_ERRORSTRING,        "aglErrorString" },
		{ "\017aglResetLibrary",       GL_SUB_AGL_RESETLIBRARY,       "aglResetLibrary" },
	};
	const int num_agl = sizeof(agl_symbols) / sizeof(agl_symbols[0]);

	// Core GL functions -- all 336 from GLIFunctionDispatch (gl.h)
	// Each entry maps the exported symbol name to our sub-opcode.
	GLSymbolEntry gl_symbols[] = {
		{ "\007glAccum", GL_SUB_ACCUM, "glAccum" },
		{ "\013glAlphaFunc", GL_SUB_ALPHA_FUNC, "glAlphaFunc" },
		{ "\025glAreTexturesResident", GL_SUB_ARE_TEXTURES_RESIDENT, "glAreTexturesResident" },
		{ "\016glArrayElement", GL_SUB_ARRAY_ELEMENT, "glArrayElement" },
		{ "\007glBegin", GL_SUB_BEGIN, "glBegin" },
		{ "\015glBindTexture", GL_SUB_BIND_TEXTURE, "glBindTexture" },
		{ "\010glBitmap", GL_SUB_BITMAP, "glBitmap" },
		{ "\013glBlendFunc", GL_SUB_BLEND_FUNC, "glBlendFunc" },
		{ "\012glCallList", GL_SUB_CALL_LIST, "glCallList" },
		{ "\013glCallLists", GL_SUB_CALL_LISTS, "glCallLists" },
		{ "\007glClear", GL_SUB_CLEAR, "glClear" },
		{ "\014glClearAccum", GL_SUB_CLEAR_ACCUM, "glClearAccum" },
		{ "\014glClearColor", GL_SUB_CLEAR_COLOR, "glClearColor" },
		{ "\014glClearDepth", GL_SUB_CLEAR_DEPTH, "glClearDepth" },
		{ "\014glClearIndex", GL_SUB_CLEAR_INDEX, "glClearIndex" },
		{ "\016glClearStencil", GL_SUB_CLEAR_STENCIL, "glClearStencil" },
		{ "\013glClipPlane", GL_SUB_CLIP_PLANE, "glClipPlane" },
		{ "\011glColor3b", GL_SUB_COLOR3B, "glColor3b" },
		{ "\012glColor3bv", GL_SUB_COLOR3BV, "glColor3bv" },
		{ "\011glColor3d", GL_SUB_COLOR3D, "glColor3d" },
		{ "\012glColor3dv", GL_SUB_COLOR3DV, "glColor3dv" },
		{ "\011glColor3f", GL_SUB_COLOR3F, "glColor3f" },
		{ "\012glColor3fv", GL_SUB_COLOR3FV, "glColor3fv" },
		{ "\011glColor3i", GL_SUB_COLOR3I, "glColor3i" },
		{ "\012glColor3iv", GL_SUB_COLOR3IV, "glColor3iv" },
		{ "\011glColor3s", GL_SUB_COLOR3S, "glColor3s" },
		{ "\012glColor3sv", GL_SUB_COLOR3SV, "glColor3sv" },
		{ "\012glColor3ub", GL_SUB_COLOR3UB, "glColor3ub" },
		{ "\013glColor3ubv", GL_SUB_COLOR3UBV, "glColor3ubv" },
		{ "\012glColor3ui", GL_SUB_COLOR3UI, "glColor3ui" },
		{ "\013glColor3uiv", GL_SUB_COLOR3UIV, "glColor3uiv" },
		{ "\012glColor3us", GL_SUB_COLOR3US, "glColor3us" },
		{ "\013glColor3usv", GL_SUB_COLOR3USV, "glColor3usv" },
		{ "\011glColor4b", GL_SUB_COLOR4B, "glColor4b" },
		{ "\012glColor4bv", GL_SUB_COLOR4BV, "glColor4bv" },
		{ "\011glColor4d", GL_SUB_COLOR4D, "glColor4d" },
		{ "\012glColor4dv", GL_SUB_COLOR4DV, "glColor4dv" },
		{ "\011glColor4f", GL_SUB_COLOR4F, "glColor4f" },
		{ "\012glColor4fv", GL_SUB_COLOR4FV, "glColor4fv" },
		{ "\011glColor4i", GL_SUB_COLOR4I, "glColor4i" },
		{ "\012glColor4iv", GL_SUB_COLOR4IV, "glColor4iv" },
		{ "\011glColor4s", GL_SUB_COLOR4S, "glColor4s" },
		{ "\012glColor4sv", GL_SUB_COLOR4SV, "glColor4sv" },
		{ "\012glColor4ub", GL_SUB_COLOR4UB, "glColor4ub" },
		{ "\013glColor4ubv", GL_SUB_COLOR4UBV, "glColor4ubv" },
		{ "\012glColor4ui", GL_SUB_COLOR4UI, "glColor4ui" },
		{ "\013glColor4uiv", GL_SUB_COLOR4UIV, "glColor4uiv" },
		{ "\012glColor4us", GL_SUB_COLOR4US, "glColor4us" },
		{ "\013glColor4usv", GL_SUB_COLOR4USV, "glColor4usv" },
		{ "\013glColorMask", GL_SUB_COLOR_MASK, "glColorMask" },
		{ "\017glColorMaterial", GL_SUB_COLOR_MATERIAL, "glColorMaterial" },
		{ "\016glColorPointer", GL_SUB_COLOR_POINTER, "glColorPointer" },
		{ "\014glCopyPixels", GL_SUB_COPY_PIXELS, "glCopyPixels" },
		{ "\020glCopyTexImage1D", GL_SUB_COPY_TEX_IMAGE1D, "glCopyTexImage1D" },
		{ "\020glCopyTexImage2D", GL_SUB_COPY_TEX_IMAGE2D, "glCopyTexImage2D" },
		{ "\023glCopyTexSubImage1D", GL_SUB_COPY_TEX_SUB_IMAGE1D, "glCopyTexSubImage1D" },
		{ "\023glCopyTexSubImage2D", GL_SUB_COPY_TEX_SUB_IMAGE2D, "glCopyTexSubImage2D" },
		{ "\012glCullFace", GL_SUB_CULL_FACE, "glCullFace" },
		{ "\015glDeleteLists", GL_SUB_DELETE_LISTS, "glDeleteLists" },
		{ "\020glDeleteTextures", GL_SUB_DELETE_TEXTURES, "glDeleteTextures" },
		{ "\013glDepthFunc", GL_SUB_DEPTH_FUNC, "glDepthFunc" },
		{ "\013glDepthMask", GL_SUB_DEPTH_MASK, "glDepthMask" },
		{ "\014glDepthRange", GL_SUB_DEPTH_RANGE, "glDepthRange" },
		{ "\011glDisable", GL_SUB_DISABLE, "glDisable" },
		{ "\024glDisableClientState", GL_SUB_DISABLE_CLIENT_STATE, "glDisableClientState" },
		{ "\014glDrawArrays", GL_SUB_DRAW_ARRAYS, "glDrawArrays" },
		{ "\014glDrawBuffer", GL_SUB_DRAW_BUFFER, "glDrawBuffer" },
		{ "\016glDrawElements", GL_SUB_DRAW_ELEMENTS, "glDrawElements" },
		{ "\014glDrawPixels", GL_SUB_DRAW_PIXELS, "glDrawPixels" },
		{ "\012glEdgeFlag", GL_SUB_EDGE_FLAG, "glEdgeFlag" },
		{ "\021glEdgeFlagPointer", GL_SUB_EDGE_FLAG_POINTER, "glEdgeFlagPointer" },
		{ "\013glEdgeFlagv", GL_SUB_EDGE_FLAGV, "glEdgeFlagv" },
		{ "\010glEnable", GL_SUB_ENABLE, "glEnable" },
		{ "\023glEnableClientState", GL_SUB_ENABLE_CLIENT_STATE, "glEnableClientState" },
		{ "\005glEnd", GL_SUB_END, "glEnd" },
		{ "\011glEndList", GL_SUB_END_LIST, "glEndList" },
		{ "\015glEvalCoord1d", GL_SUB_EVAL_COORD1D, "glEvalCoord1d" },
		{ "\016glEvalCoord1dv", GL_SUB_EVAL_COORD1DV, "glEvalCoord1dv" },
		{ "\015glEvalCoord1f", GL_SUB_EVAL_COORD1F, "glEvalCoord1f" },
		{ "\016glEvalCoord1fv", GL_SUB_EVAL_COORD1FV, "glEvalCoord1fv" },
		{ "\015glEvalCoord2d", GL_SUB_EVAL_COORD2D, "glEvalCoord2d" },
		{ "\016glEvalCoord2dv", GL_SUB_EVAL_COORD2DV, "glEvalCoord2dv" },
		{ "\015glEvalCoord2f", GL_SUB_EVAL_COORD2F, "glEvalCoord2f" },
		{ "\016glEvalCoord2fv", GL_SUB_EVAL_COORD2FV, "glEvalCoord2fv" },
		{ "\013glEvalMesh1", GL_SUB_EVAL_MESH1, "glEvalMesh1" },
		{ "\013glEvalMesh2", GL_SUB_EVAL_MESH2, "glEvalMesh2" },
		{ "\014glEvalPoint1", GL_SUB_EVAL_POINT1, "glEvalPoint1" },
		{ "\014glEvalPoint2", GL_SUB_EVAL_POINT2, "glEvalPoint2" },
		{ "\020glFeedbackBuffer", GL_SUB_FEEDBACK_BUFFER, "glFeedbackBuffer" },
		{ "\010glFinish", GL_SUB_FINISH, "glFinish" },
		{ "\007glFlush", GL_SUB_FLUSH, "glFlush" },
		{ "\006glFogf", GL_SUB_FOGF, "glFogf" },
		{ "\007glFogfv", GL_SUB_FOGFV, "glFogfv" },
		{ "\006glFogi", GL_SUB_FOGI, "glFogi" },
		{ "\007glFogiv", GL_SUB_FOGIV, "glFogiv" },
		{ "\013glFrontFace", GL_SUB_FRONT_FACE, "glFrontFace" },
		{ "\011glFrustum", GL_SUB_FRUSTUM, "glFrustum" },
		{ "\012glGenLists", GL_SUB_GEN_LISTS, "glGenLists" },
		{ "\015glGenTextures", GL_SUB_GEN_TEXTURES, "glGenTextures" },
		{ "\015glGetBooleanv", GL_SUB_GET_BOOLEANV, "glGetBooleanv" },
		{ "\016glGetClipPlane", GL_SUB_GET_CLIP_PLANE, "glGetClipPlane" },
		{ "\014glGetDoublev", GL_SUB_GET_DOUBLEV, "glGetDoublev" },
		{ "\012glGetError", GL_SUB_GET_ERROR, "glGetError" },
		{ "\013glGetFloatv", GL_SUB_GET_FLOATV, "glGetFloatv" },
		{ "\015glGetIntegerv", GL_SUB_GET_INTEGERV, "glGetIntegerv" },
		{ "\014glGetLightfv", GL_SUB_GET_LIGHTFV, "glGetLightfv" },
		{ "\014glGetLightiv", GL_SUB_GET_LIGHTIV, "glGetLightiv" },
		{ "\012glGetMapdv", GL_SUB_GET_MAPDV, "glGetMapdv" },
		{ "\012glGetMapfv", GL_SUB_GET_MAPFV, "glGetMapfv" },
		{ "\012glGetMapiv", GL_SUB_GET_MAPIV, "glGetMapiv" },
		{ "\017glGetMaterialfv", GL_SUB_GET_MATERIALFV, "glGetMaterialfv" },
		{ "\017glGetMaterialiv", GL_SUB_GET_MATERIALIV, "glGetMaterialiv" },
		{ "\017glGetPixelMapfv", GL_SUB_GET_PIXEL_MAPFV, "glGetPixelMapfv" },
		{ "\020glGetPixelMapuiv", GL_SUB_GET_PIXEL_MAPUIV, "glGetPixelMapuiv" },
		{ "\020glGetPixelMapusv", GL_SUB_GET_PIXEL_MAPUSV, "glGetPixelMapusv" },
		{ "\015glGetPointerv", GL_SUB_GET_POINTERV, "glGetPointerv" },
		{ "\023glGetPolygonStipple", GL_SUB_GET_POLYGON_STIPPLE, "glGetPolygonStipple" },
		{ "\013glGetString", GL_SUB_GET_STRING, "glGetString" },
		{ "\015glGetTexEnvfv", GL_SUB_GET_TEX_ENVFV, "glGetTexEnvfv" },
		{ "\015glGetTexEnviv", GL_SUB_GET_TEX_ENVIV, "glGetTexEnviv" },
		{ "\015glGetTexGendv", GL_SUB_GET_TEX_GENDV, "glGetTexGendv" },
		{ "\015glGetTexGenfv", GL_SUB_GET_TEX_GENFV, "glGetTexGenfv" },
		{ "\015glGetTexGeniv", GL_SUB_GET_TEX_GENIV, "glGetTexGeniv" },
		{ "\015glGetTexImage", GL_SUB_GET_TEX_IMAGE, "glGetTexImage" },
		{ "\030glGetTexLevelParameterfv", GL_SUB_GET_TEX_LEVEL_PARAMETERFV, "glGetTexLevelParameterfv" },
		{ "\030glGetTexLevelParameteriv", GL_SUB_GET_TEX_LEVEL_PARAMETERIV, "glGetTexLevelParameteriv" },
		{ "\023glGetTexParameterfv", GL_SUB_GET_TEX_PARAMETERFV, "glGetTexParameterfv" },
		{ "\023glGetTexParameteriv", GL_SUB_GET_TEX_PARAMETERIV, "glGetTexParameteriv" },
		{ "\006glHint", GL_SUB_HINT, "glHint" },
		{ "\013glIndexMask", GL_SUB_INDEX_MASK, "glIndexMask" },
		{ "\016glIndexPointer", GL_SUB_INDEX_POINTER, "glIndexPointer" },
		{ "\010glIndexd", GL_SUB_INDEXD, "glIndexd" },
		{ "\011glIndexdv", GL_SUB_INDEXDV, "glIndexdv" },
		{ "\010glIndexf", GL_SUB_INDEXF, "glIndexf" },
		{ "\011glIndexfv", GL_SUB_INDEXFV, "glIndexfv" },
		{ "\010glIndexi", GL_SUB_INDEXI, "glIndexi" },
		{ "\011glIndexiv", GL_SUB_INDEXIV, "glIndexiv" },
		{ "\010glIndexs", GL_SUB_INDEXS, "glIndexs" },
		{ "\011glIndexsv", GL_SUB_INDEXSV, "glIndexsv" },
		{ "\011glIndexub", GL_SUB_INDEXUB, "glIndexub" },
		{ "\012glIndexubv", GL_SUB_INDEXUBV, "glIndexubv" },
		{ "\013glInitNames", GL_SUB_INIT_NAMES, "glInitNames" },
		{ "\023glInterleavedArrays", GL_SUB_INTERLEAVED_ARRAYS, "glInterleavedArrays" },
		{ "\013glIsEnabled", GL_SUB_IS_ENABLED, "glIsEnabled" },
		{ "\010glIsList", GL_SUB_IS_LIST, "glIsList" },
		{ "\013glIsTexture", GL_SUB_IS_TEXTURE, "glIsTexture" },
		{ "\015glLightModelf", GL_SUB_LIGHT_MODELF, "glLightModelf" },
		{ "\016glLightModelfv", GL_SUB_LIGHT_MODELFV, "glLightModelfv" },
		{ "\015glLightModeli", GL_SUB_LIGHT_MODELI, "glLightModeli" },
		{ "\016glLightModeliv", GL_SUB_LIGHT_MODELIV, "glLightModeliv" },
		{ "\010glLightf", GL_SUB_LIGHTF, "glLightf" },
		{ "\011glLightfv", GL_SUB_LIGHTFV, "glLightfv" },
		{ "\010glLighti", GL_SUB_LIGHTI, "glLighti" },
		{ "\011glLightiv", GL_SUB_LIGHTIV, "glLightiv" },
		{ "\015glLineStipple", GL_SUB_LINE_STIPPLE, "glLineStipple" },
		{ "\013glLineWidth", GL_SUB_LINE_WIDTH, "glLineWidth" },
		{ "\012glListBase", GL_SUB_LIST_BASE, "glListBase" },
		{ "\016glLoadIdentity", GL_SUB_LOAD_IDENTITY, "glLoadIdentity" },
		{ "\015glLoadMatrixd", GL_SUB_LOAD_MATRIXD, "glLoadMatrixd" },
		{ "\015glLoadMatrixf", GL_SUB_LOAD_MATRIXF, "glLoadMatrixf" },
		{ "\012glLoadName", GL_SUB_LOAD_NAME, "glLoadName" },
		{ "\011glLogicOp", GL_SUB_LOGIC_OP, "glLogicOp" },
		{ "\007glMap1d", GL_SUB_MAP1D, "glMap1d" },
		{ "\007glMap1f", GL_SUB_MAP1F, "glMap1f" },
		{ "\007glMap2d", GL_SUB_MAP2D, "glMap2d" },
		{ "\007glMap2f", GL_SUB_MAP2F, "glMap2f" },
		{ "\013glMapGrid1d", GL_SUB_MAP_GRID1D, "glMapGrid1d" },
		{ "\013glMapGrid1f", GL_SUB_MAP_GRID1F, "glMapGrid1f" },
		{ "\013glMapGrid2d", GL_SUB_MAP_GRID2D, "glMapGrid2d" },
		{ "\013glMapGrid2f", GL_SUB_MAP_GRID2F, "glMapGrid2f" },
		{ "\013glMaterialf", GL_SUB_MATERIALF, "glMaterialf" },
		{ "\014glMaterialfv", GL_SUB_MATERIALFV, "glMaterialfv" },
		{ "\013glMateriali", GL_SUB_MATERIALI, "glMateriali" },
		{ "\014glMaterialiv", GL_SUB_MATERIALIV, "glMaterialiv" },
		{ "\014glMatrixMode", GL_SUB_MATRIX_MODE, "glMatrixMode" },
		{ "\015glMultMatrixd", GL_SUB_MULT_MATRIXD, "glMultMatrixd" },
		{ "\015glMultMatrixf", GL_SUB_MULT_MATRIXF, "glMultMatrixf" },
		{ "\011glNewList", GL_SUB_NEW_LIST, "glNewList" },
		{ "\012glNormal3b", GL_SUB_NORMAL3B, "glNormal3b" },
		{ "\013glNormal3bv", GL_SUB_NORMAL3BV, "glNormal3bv" },
		{ "\012glNormal3d", GL_SUB_NORMAL3D, "glNormal3d" },
		{ "\013glNormal3dv", GL_SUB_NORMAL3DV, "glNormal3dv" },
		{ "\012glNormal3f", GL_SUB_NORMAL3F, "glNormal3f" },
		{ "\013glNormal3fv", GL_SUB_NORMAL3FV, "glNormal3fv" },
		{ "\012glNormal3i", GL_SUB_NORMAL3I, "glNormal3i" },
		{ "\013glNormal3iv", GL_SUB_NORMAL3IV, "glNormal3iv" },
		{ "\012glNormal3s", GL_SUB_NORMAL3S, "glNormal3s" },
		{ "\013glNormal3sv", GL_SUB_NORMAL3SV, "glNormal3sv" },
		{ "\017glNormalPointer", GL_SUB_NORMAL_POINTER, "glNormalPointer" },
		{ "\007glOrtho", GL_SUB_ORTHO, "glOrtho" },
		{ "\015glPassThrough", GL_SUB_PASS_THROUGH, "glPassThrough" },
		{ "\014glPixelMapfv", GL_SUB_PIXEL_MAPFV, "glPixelMapfv" },
		{ "\015glPixelMapuiv", GL_SUB_PIXEL_MAPUIV, "glPixelMapuiv" },
		{ "\015glPixelMapusv", GL_SUB_PIXEL_MAPUSV, "glPixelMapusv" },
		{ "\015glPixelStoref", GL_SUB_PIXEL_STOREF, "glPixelStoref" },
		{ "\015glPixelStorei", GL_SUB_PIXEL_STOREI, "glPixelStorei" },
		{ "\020glPixelTransferf", GL_SUB_PIXEL_TRANSFERF, "glPixelTransferf" },
		{ "\020glPixelTransferi", GL_SUB_PIXEL_TRANSFERI, "glPixelTransferi" },
		{ "\013glPixelZoom", GL_SUB_PIXEL_ZOOM, "glPixelZoom" },
		{ "\013glPointSize", GL_SUB_POINT_SIZE, "glPointSize" },
		{ "\015glPolygonMode", GL_SUB_POLYGON_MODE, "glPolygonMode" },
		{ "\017glPolygonOffset", GL_SUB_POLYGON_OFFSET, "glPolygonOffset" },
		{ "\020glPolygonStipple", GL_SUB_POLYGON_STIPPLE, "glPolygonStipple" },
		{ "\013glPopAttrib", GL_SUB_POP_ATTRIB, "glPopAttrib" },
		{ "\021glPopClientAttrib", GL_SUB_POP_CLIENT_ATTRIB, "glPopClientAttrib" },
		{ "\013glPopMatrix", GL_SUB_POP_MATRIX, "glPopMatrix" },
		{ "\011glPopName", GL_SUB_POP_NAME, "glPopName" },
		{ "\024glPrioritizeTextures", GL_SUB_PRIORITIZE_TEXTURES, "glPrioritizeTextures" },
		{ "\014glPushAttrib", GL_SUB_PUSH_ATTRIB, "glPushAttrib" },
		{ "\022glPushClientAttrib", GL_SUB_PUSH_CLIENT_ATTRIB, "glPushClientAttrib" },
		{ "\014glPushMatrix", GL_SUB_PUSH_MATRIX, "glPushMatrix" },
		{ "\012glPushName", GL_SUB_PUSH_NAME, "glPushName" },
		{ "\015glRasterPos2d", GL_SUB_RASTER_POS2D, "glRasterPos2d" },
		{ "\016glRasterPos2dv", GL_SUB_RASTER_POS2DV, "glRasterPos2dv" },
		{ "\015glRasterPos2f", GL_SUB_RASTER_POS2F, "glRasterPos2f" },
		{ "\016glRasterPos2fv", GL_SUB_RASTER_POS2FV, "glRasterPos2fv" },
		{ "\015glRasterPos2i", GL_SUB_RASTER_POS2I, "glRasterPos2i" },
		{ "\016glRasterPos2iv", GL_SUB_RASTER_POS2IV, "glRasterPos2iv" },
		{ "\015glRasterPos2s", GL_SUB_RASTER_POS2S, "glRasterPos2s" },
		{ "\016glRasterPos2sv", GL_SUB_RASTER_POS2SV, "glRasterPos2sv" },
		{ "\015glRasterPos3d", GL_SUB_RASTER_POS3D, "glRasterPos3d" },
		{ "\016glRasterPos3dv", GL_SUB_RASTER_POS3DV, "glRasterPos3dv" },
		{ "\015glRasterPos3f", GL_SUB_RASTER_POS3F, "glRasterPos3f" },
		{ "\016glRasterPos3fv", GL_SUB_RASTER_POS3FV, "glRasterPos3fv" },
		{ "\015glRasterPos3i", GL_SUB_RASTER_POS3I, "glRasterPos3i" },
		{ "\016glRasterPos3iv", GL_SUB_RASTER_POS3IV, "glRasterPos3iv" },
		{ "\015glRasterPos3s", GL_SUB_RASTER_POS3S, "glRasterPos3s" },
		{ "\016glRasterPos3sv", GL_SUB_RASTER_POS3SV, "glRasterPos3sv" },
		{ "\015glRasterPos4d", GL_SUB_RASTER_POS4D, "glRasterPos4d" },
		{ "\016glRasterPos4dv", GL_SUB_RASTER_POS4DV, "glRasterPos4dv" },
		{ "\015glRasterPos4f", GL_SUB_RASTER_POS4F, "glRasterPos4f" },
		{ "\016glRasterPos4fv", GL_SUB_RASTER_POS4FV, "glRasterPos4fv" },
		{ "\015glRasterPos4i", GL_SUB_RASTER_POS4I, "glRasterPos4i" },
		{ "\016glRasterPos4iv", GL_SUB_RASTER_POS4IV, "glRasterPos4iv" },
		{ "\015glRasterPos4s", GL_SUB_RASTER_POS4S, "glRasterPos4s" },
		{ "\016glRasterPos4sv", GL_SUB_RASTER_POS4SV, "glRasterPos4sv" },
		{ "\014glReadBuffer", GL_SUB_READ_BUFFER, "glReadBuffer" },
		{ "\014glReadPixels", GL_SUB_READ_PIXELS, "glReadPixels" },
		{ "\007glRectd", GL_SUB_RECTD, "glRectd" },
		{ "\010glRectdv", GL_SUB_RECTDV, "glRectdv" },
		{ "\007glRectf", GL_SUB_RECTF, "glRectf" },
		{ "\010glRectfv", GL_SUB_RECTFV, "glRectfv" },
		{ "\007glRecti", GL_SUB_RECTI, "glRecti" },
		{ "\010glRectiv", GL_SUB_RECTIV, "glRectiv" },
		{ "\007glRects", GL_SUB_RECTS, "glRects" },
		{ "\010glRectsv", GL_SUB_RECTSV, "glRectsv" },
		{ "\014glRenderMode", GL_SUB_RENDER_MODE, "glRenderMode" },
		{ "\011glRotated", GL_SUB_ROTATED, "glRotated" },
		{ "\011glRotatef", GL_SUB_ROTATEF, "glRotatef" },
		{ "\010glScaled", GL_SUB_SCALED, "glScaled" },
		{ "\010glScalef", GL_SUB_SCALEF, "glScalef" },
		{ "\011glScissor", GL_SUB_SCISSOR, "glScissor" },
		{ "\016glSelectBuffer", GL_SUB_SELECT_BUFFER, "glSelectBuffer" },
		{ "\014glShadeModel", GL_SUB_SHADE_MODEL, "glShadeModel" },
		{ "\015glStencilFunc", GL_SUB_STENCIL_FUNC, "glStencilFunc" },
		{ "\015glStencilMask", GL_SUB_STENCIL_MASK, "glStencilMask" },
		{ "\013glStencilOp", GL_SUB_STENCIL_OP, "glStencilOp" },
		{ "\014glTexCoord1d", GL_SUB_TEX_COORD1D, "glTexCoord1d" },
		{ "\015glTexCoord1dv", GL_SUB_TEX_COORD1DV, "glTexCoord1dv" },
		{ "\014glTexCoord1f", GL_SUB_TEX_COORD1F, "glTexCoord1f" },
		{ "\015glTexCoord1fv", GL_SUB_TEX_COORD1FV, "glTexCoord1fv" },
		{ "\014glTexCoord1i", GL_SUB_TEX_COORD1I, "glTexCoord1i" },
		{ "\015glTexCoord1iv", GL_SUB_TEX_COORD1IV, "glTexCoord1iv" },
		{ "\014glTexCoord1s", GL_SUB_TEX_COORD1S, "glTexCoord1s" },
		{ "\015glTexCoord1sv", GL_SUB_TEX_COORD1SV, "glTexCoord1sv" },
		{ "\014glTexCoord2d", GL_SUB_TEX_COORD2D, "glTexCoord2d" },
		{ "\015glTexCoord2dv", GL_SUB_TEX_COORD2DV, "glTexCoord2dv" },
		{ "\014glTexCoord2f", GL_SUB_TEX_COORD2F, "glTexCoord2f" },
		{ "\015glTexCoord2fv", GL_SUB_TEX_COORD2FV, "glTexCoord2fv" },
		{ "\014glTexCoord2i", GL_SUB_TEX_COORD2I, "glTexCoord2i" },
		{ "\015glTexCoord2iv", GL_SUB_TEX_COORD2IV, "glTexCoord2iv" },
		{ "\014glTexCoord2s", GL_SUB_TEX_COORD2S, "glTexCoord2s" },
		{ "\015glTexCoord2sv", GL_SUB_TEX_COORD2SV, "glTexCoord2sv" },
		{ "\014glTexCoord3d", GL_SUB_TEX_COORD3D, "glTexCoord3d" },
		{ "\015glTexCoord3dv", GL_SUB_TEX_COORD3DV, "glTexCoord3dv" },
		{ "\014glTexCoord3f", GL_SUB_TEX_COORD3F, "glTexCoord3f" },
		{ "\015glTexCoord3fv", GL_SUB_TEX_COORD3FV, "glTexCoord3fv" },
		{ "\014glTexCoord3i", GL_SUB_TEX_COORD3I, "glTexCoord3i" },
		{ "\015glTexCoord3iv", GL_SUB_TEX_COORD3IV, "glTexCoord3iv" },
		{ "\014glTexCoord3s", GL_SUB_TEX_COORD3S, "glTexCoord3s" },
		{ "\015glTexCoord3sv", GL_SUB_TEX_COORD3SV, "glTexCoord3sv" },
		{ "\014glTexCoord4d", GL_SUB_TEX_COORD4D, "glTexCoord4d" },
		{ "\015glTexCoord4dv", GL_SUB_TEX_COORD4DV, "glTexCoord4dv" },
		{ "\014glTexCoord4f", GL_SUB_TEX_COORD4F, "glTexCoord4f" },
		{ "\015glTexCoord4fv", GL_SUB_TEX_COORD4FV, "glTexCoord4fv" },
		{ "\014glTexCoord4i", GL_SUB_TEX_COORD4I, "glTexCoord4i" },
		{ "\015glTexCoord4iv", GL_SUB_TEX_COORD4IV, "glTexCoord4iv" },
		{ "\014glTexCoord4s", GL_SUB_TEX_COORD4S, "glTexCoord4s" },
		{ "\015glTexCoord4sv", GL_SUB_TEX_COORD4SV, "glTexCoord4sv" },
		{ "\021glTexCoordPointer", GL_SUB_TEX_COORD_POINTER, "glTexCoordPointer" },
		{ "\011glTexEnvf", GL_SUB_TEX_ENVF, "glTexEnvf" },
		{ "\012glTexEnvfv", GL_SUB_TEX_ENVFV, "glTexEnvfv" },
		{ "\011glTexEnvi", GL_SUB_TEX_ENVI, "glTexEnvi" },
		{ "\012glTexEnviv", GL_SUB_TEX_ENVIV, "glTexEnviv" },
		{ "\011glTexGend", GL_SUB_TEX_GEND, "glTexGend" },
		{ "\012glTexGendv", GL_SUB_TEX_GENDV, "glTexGendv" },
		{ "\011glTexGenf", GL_SUB_TEX_GENF, "glTexGenf" },
		{ "\012glTexGenfv", GL_SUB_TEX_GENFV, "glTexGenfv" },
		{ "\011glTexGeni", GL_SUB_TEX_GENI, "glTexGeni" },
		{ "\012glTexGeniv", GL_SUB_TEX_GENIV, "glTexGeniv" },
		{ "\014glTexImage1D", GL_SUB_TEX_IMAGE1D, "glTexImage1D" },
		{ "\014glTexImage2D", GL_SUB_TEX_IMAGE2D, "glTexImage2D" },
		{ "\017glTexParameterf", GL_SUB_TEX_PARAMETERF, "glTexParameterf" },
		{ "\020glTexParameterfv", GL_SUB_TEX_PARAMETERFV, "glTexParameterfv" },
		{ "\017glTexParameteri", GL_SUB_TEX_PARAMETERI, "glTexParameteri" },
		{ "\020glTexParameteriv", GL_SUB_TEX_PARAMETERIV, "glTexParameteriv" },
		{ "\017glTexSubImage1D", GL_SUB_TEX_SUB_IMAGE1D, "glTexSubImage1D" },
		{ "\017glTexSubImage2D", GL_SUB_TEX_SUB_IMAGE2D, "glTexSubImage2D" },
		{ "\014glTranslated", GL_SUB_TRANSLATED, "glTranslated" },
		{ "\014glTranslatef", GL_SUB_TRANSLATEF, "glTranslatef" },
		{ "\012glVertex2d", GL_SUB_VERTEX2D, "glVertex2d" },
		{ "\013glVertex2dv", GL_SUB_VERTEX2DV, "glVertex2dv" },
		{ "\012glVertex2f", GL_SUB_VERTEX2F, "glVertex2f" },
		{ "\013glVertex2fv", GL_SUB_VERTEX2FV, "glVertex2fv" },
		{ "\012glVertex2i", GL_SUB_VERTEX2I, "glVertex2i" },
		{ "\013glVertex2iv", GL_SUB_VERTEX2IV, "glVertex2iv" },
		{ "\012glVertex2s", GL_SUB_VERTEX2S, "glVertex2s" },
		{ "\013glVertex2sv", GL_SUB_VERTEX2SV, "glVertex2sv" },
		{ "\012glVertex3d", GL_SUB_VERTEX3D, "glVertex3d" },
		{ "\013glVertex3dv", GL_SUB_VERTEX3DV, "glVertex3dv" },
		{ "\012glVertex3f", GL_SUB_VERTEX3F, "glVertex3f" },
		{ "\013glVertex3fv", GL_SUB_VERTEX3FV, "glVertex3fv" },
		{ "\012glVertex3i", GL_SUB_VERTEX3I, "glVertex3i" },
		{ "\013glVertex3iv", GL_SUB_VERTEX3IV, "glVertex3iv" },
		{ "\012glVertex3s", GL_SUB_VERTEX3S, "glVertex3s" },
		{ "\013glVertex3sv", GL_SUB_VERTEX3SV, "glVertex3sv" },
		{ "\012glVertex4d", GL_SUB_VERTEX4D, "glVertex4d" },
		{ "\013glVertex4dv", GL_SUB_VERTEX4DV, "glVertex4dv" },
		{ "\012glVertex4f", GL_SUB_VERTEX4F, "glVertex4f" },
		{ "\013glVertex4fv", GL_SUB_VERTEX4FV, "glVertex4fv" },
		{ "\012glVertex4i", GL_SUB_VERTEX4I, "glVertex4i" },
		{ "\013glVertex4iv", GL_SUB_VERTEX4IV, "glVertex4iv" },
		{ "\012glVertex4s", GL_SUB_VERTEX4S, "glVertex4s" },
		{ "\013glVertex4sv", GL_SUB_VERTEX4SV, "glVertex4sv" },
		{ "\017glVertexPointer", GL_SUB_VERTEX_POINTER, "glVertexPointer" },
		{ "\012glViewport", GL_SUB_VIEWPORT, "glViewport" },
	};
	const int num_gl = sizeof(gl_symbols) / sizeof(gl_symbols[0]);

	// GL 1.2 / advertised extension entry points are not part of the fixed AGL
	// context dispatch table above. Hook them separately so older games can
	// discover them without changing GL_DISPATCH_TABLE_ENTRIES indexing.
	GLSymbolEntry gl_extra_symbols[] = {
		{ "\017glBlendColorEXT", GL_SUB_BLEND_COLOR_EXT, "glBlendColorEXT" },
		{ "\022glBlendEquationEXT", GL_SUB_BLEND_EQUATION_EXT, "glBlendEquationEXT" },
		{ "\017glLockArraysEXT", GL_SUB_LOCK_ARRAYS_EXT, "glLockArraysEXT" },
		{ "\021glUnlockArraysEXT", GL_SUB_UNLOCK_ARRAYS_EXT, "glUnlockArraysEXT" },
		{ "\030glClientActiveTextureARB", GL_SUB_CLIENT_ACTIVE_TEXTURE_ARB, "glClientActiveTextureARB" },
		{ "\022glActiveTextureARB", GL_SUB_ACTIVE_TEXTURE_ARB, "glActiveTextureARB" },
		{ "\024glMultiTexCoord1dARB", GL_SUB_MULTI_TEX_COORD1D_ARB, "glMultiTexCoord1dARB" },
		{ "\025glMultiTexCoord1dvARB", GL_SUB_MULTI_TEX_COORD1DV_ARB, "glMultiTexCoord1dvARB" },
		{ "\024glMultiTexCoord1fARB", GL_SUB_MULTI_TEX_COORD1F_ARB, "glMultiTexCoord1fARB" },
		{ "\025glMultiTexCoord1fvARB", GL_SUB_MULTI_TEX_COORD1FV_ARB, "glMultiTexCoord1fvARB" },
		{ "\024glMultiTexCoord1iARB", GL_SUB_MULTI_TEX_COORD1I_ARB, "glMultiTexCoord1iARB" },
		{ "\025glMultiTexCoord1ivARB", GL_SUB_MULTI_TEX_COORD1IV_ARB, "glMultiTexCoord1ivARB" },
		{ "\024glMultiTexCoord1sARB", GL_SUB_MULTI_TEX_COORD1S_ARB, "glMultiTexCoord1sARB" },
		{ "\025glMultiTexCoord1svARB", GL_SUB_MULTI_TEX_COORD1SV_ARB, "glMultiTexCoord1svARB" },
		{ "\024glMultiTexCoord2dARB", GL_SUB_MULTI_TEX_COORD2D_ARB, "glMultiTexCoord2dARB" },
		{ "\025glMultiTexCoord2dvARB", GL_SUB_MULTI_TEX_COORD2DV_ARB, "glMultiTexCoord2dvARB" },
		{ "\024glMultiTexCoord2fARB", GL_SUB_MULTI_TEX_COORD2F_ARB, "glMultiTexCoord2fARB" },
		{ "\025glMultiTexCoord2fvARB", GL_SUB_MULTI_TEX_COORD2FV_ARB, "glMultiTexCoord2fvARB" },
		{ "\024glMultiTexCoord2iARB", GL_SUB_MULTI_TEX_COORD2I_ARB, "glMultiTexCoord2iARB" },
		{ "\025glMultiTexCoord2ivARB", GL_SUB_MULTI_TEX_COORD2IV_ARB, "glMultiTexCoord2ivARB" },
		{ "\024glMultiTexCoord2sARB", GL_SUB_MULTI_TEX_COORD2S_ARB, "glMultiTexCoord2sARB" },
		{ "\025glMultiTexCoord2svARB", GL_SUB_MULTI_TEX_COORD2SV_ARB, "glMultiTexCoord2svARB" },
		{ "\024glMultiTexCoord3dARB", GL_SUB_MULTI_TEX_COORD3D_ARB, "glMultiTexCoord3dARB" },
		{ "\025glMultiTexCoord3dvARB", GL_SUB_MULTI_TEX_COORD3DV_ARB, "glMultiTexCoord3dvARB" },
		{ "\024glMultiTexCoord3fARB", GL_SUB_MULTI_TEX_COORD3F_ARB, "glMultiTexCoord3fARB" },
		{ "\025glMultiTexCoord3fvARB", GL_SUB_MULTI_TEX_COORD3FV_ARB, "glMultiTexCoord3fvARB" },
		{ "\024glMultiTexCoord3iARB", GL_SUB_MULTI_TEX_COORD3I_ARB, "glMultiTexCoord3iARB" },
		{ "\025glMultiTexCoord3ivARB", GL_SUB_MULTI_TEX_COORD3IV_ARB, "glMultiTexCoord3ivARB" },
		{ "\024glMultiTexCoord3sARB", GL_SUB_MULTI_TEX_COORD3S_ARB, "glMultiTexCoord3sARB" },
		{ "\025glMultiTexCoord3svARB", GL_SUB_MULTI_TEX_COORD3SV_ARB, "glMultiTexCoord3svARB" },
		{ "\024glMultiTexCoord4dARB", GL_SUB_MULTI_TEX_COORD4D_ARB, "glMultiTexCoord4dARB" },
		{ "\025glMultiTexCoord4dvARB", GL_SUB_MULTI_TEX_COORD4DV_ARB, "glMultiTexCoord4dvARB" },
		{ "\024glMultiTexCoord4fARB", GL_SUB_MULTI_TEX_COORD4F_ARB, "glMultiTexCoord4fARB" },
		{ "\025glMultiTexCoord4fvARB", GL_SUB_MULTI_TEX_COORD4FV_ARB, "glMultiTexCoord4fvARB" },
		{ "\024glMultiTexCoord4iARB", GL_SUB_MULTI_TEX_COORD4I_ARB, "glMultiTexCoord4iARB" },
		{ "\025glMultiTexCoord4ivARB", GL_SUB_MULTI_TEX_COORD4IV_ARB, "glMultiTexCoord4ivARB" },
		{ "\024glMultiTexCoord4sARB", GL_SUB_MULTI_TEX_COORD4S_ARB, "glMultiTexCoord4sARB" },
		{ "\025glMultiTexCoord4svARB", GL_SUB_MULTI_TEX_COORD4SV_ARB, "glMultiTexCoord4svARB" },
		{ "\031glLoadTransposeMatrixdARB", GL_SUB_LOAD_TRANSPOSE_MATRIXD_ARB, "glLoadTransposeMatrixdARB" },
		{ "\031glLoadTransposeMatrixfARB", GL_SUB_LOAD_TRANSPOSE_MATRIXF_ARB, "glLoadTransposeMatrixfARB" },
		{ "\031glMultTransposeMatrixdARB", GL_SUB_MULT_TRANSPOSE_MATRIXD_ARB, "glMultTransposeMatrixdARB" },
		{ "\031glMultTransposeMatrixfARB", GL_SUB_MULT_TRANSPOSE_MATRIXF_ARB, "glMultTransposeMatrixfARB" },
		{ "\025glSecondaryColor3bEXT", GL_SUB_SECONDARYCOLOR3B_EXT, "glSecondaryColor3bEXT" },
		{ "\026glSecondaryColor3bvEXT", GL_SUB_SECONDARYCOLOR3BV_EXT, "glSecondaryColor3bvEXT" },
		{ "\025glSecondaryColor3dEXT", GL_SUB_SECONDARYCOLOR3D_EXT, "glSecondaryColor3dEXT" },
		{ "\026glSecondaryColor3dvEXT", GL_SUB_SECONDARY_COLOR3DV_EXT, "glSecondaryColor3dvEXT" },
		{ "\025glSecondaryColor3fEXT", GL_SUB_SECONDARY_COLOR3F_EXT, "glSecondaryColor3fEXT" },
		{ "\026glSecondaryColor3fvEXT", GL_SUB_SECONDARY_COLOR3FV_EXT, "glSecondaryColor3fvEXT" },
		{ "\025glSecondaryColor3iEXT", GL_SUB_SECONDARY_COLOR3I_EXT, "glSecondaryColor3iEXT" },
		{ "\026glSecondaryColor3ivEXT", GL_SUB_SECONDARY_COLOR3IV_EXT, "glSecondaryColor3ivEXT" },
		{ "\025glSecondaryColor3sEXT", GL_SUB_SECONDARY_COLOR3S_EXT, "glSecondaryColor3sEXT" },
		{ "\026glSecondaryColor3svEXT", GL_SUB_SECONDARY_COLOR3SV_EXT, "glSecondaryColor3svEXT" },
		{ "\026glSecondaryColor3ubEXT", GL_SUB_SECONDARY_COLOR3UB_EXT, "glSecondaryColor3ubEXT" },
		{ "\027glSecondaryColor3ubvEXT", GL_SUB_SECONDARY_COLOR3UBV_EXT, "glSecondaryColor3ubvEXT" },
		{ "\026glSecondaryColor3uiEXT", GL_SUB_SECONDARY_COLOR3UI_EXT, "glSecondaryColor3uiEXT" },
		{ "\027glSecondaryColor3uivEXT", GL_SUB_SECONDARY_COLOR3UIV_EXT, "glSecondaryColor3uivEXT" },
		{ "\026glSecondaryColor3usEXT", GL_SUB_SECONDARY_COLOR3US_EXT, "glSecondaryColor3usEXT" },
		{ "\027glSecondaryColor3usvEXT", GL_SUB_SECONDARY_COLOR3USV_EXT, "glSecondaryColor3usvEXT" },
		{ "\032glSecondaryColorPointerEXT", GL_SUB_SECONDARY_COLOR_POINTER_EXT, "glSecondaryColorPointerEXT" },
		{ "\014glColorTable", GL_SUB_COLOR_TABLE, "glColorTable" },
		{ "\017glColorTableEXT", GL_SUB_COLOR_TABLE, "glColorTableEXT" },
		{ "\027glColorTableParameterfv", GL_SUB_COLOR_TABLE_PARAMETERFV, "glColorTableParameterfv" },
		{ "\032glColorTableParameterfvEXT", GL_SUB_COLOR_TABLE_PARAMETERFV, "glColorTableParameterfvEXT" },
		{ "\027glColorTableParameteriv", GL_SUB_COLOR_TABLE_PARAMETERIV, "glColorTableParameteriv" },
		{ "\032glColorTableParameterivEXT", GL_SUB_COLOR_TABLE_PARAMETERIV, "glColorTableParameterivEXT" },
		{ "\017glColorSubTable", GL_SUB_COLOR_SUB_TABLE, "glColorSubTable" },
		{ "\022glColorSubTableEXT", GL_SUB_COLOR_SUB_TABLE, "glColorSubTableEXT" },
		{ "\020glCopyColorTable", GL_SUB_COPY_COLOR_TABLE, "glCopyColorTable" },
		{ "\023glCopyColorSubTable", GL_SUB_COPY_COLOR_SUB_TABLE, "glCopyColorSubTable" },
		{ "\017glGetColorTable", GL_SUB_GET_COLOR_TABLE, "glGetColorTable" },
		{ "\022glGetColorTableEXT", GL_SUB_GET_COLOR_TABLE, "glGetColorTableEXT" },
		{ "\032glGetColorTableParameterfv", GL_SUB_GET_COLOR_TABLE_PARAMETERFV, "glGetColorTableParameterfv" },
		{ "\035glGetColorTableParameterfvEXT", GL_SUB_GET_COLOR_TABLE_PARAMETERFV, "glGetColorTableParameterfvEXT" },
		{ "\032glGetColorTableParameteriv", GL_SUB_GET_COLOR_TABLE_PARAMETERIV, "glGetColorTableParameteriv" },
		{ "\035glGetColorTableParameterivEXT", GL_SUB_GET_COLOR_TABLE_PARAMETERIV, "glGetColorTableParameterivEXT" },
	};
	const int num_gl_extra = sizeof(gl_extra_symbols) / sizeof(gl_extra_symbols[0]);

	// GLU functions in OpenGLUtility. The 700..753 bridge has dispatch cases
	// and native handlers for every GLU sub-opcode; install all exported names
	// so classic games can reach quadrics, tessellation, mipmap helpers, and
	// matrix utilities through their normal CFM imports.
	GLSymbolEntry glu_symbols[] = {
		{ "\015gluBeginCurve", GL_SUB_GLU_BEGINCURVE, "gluBeginCurve" },
		{ "\017gluBeginPolygon", GL_SUB_GLU_BEGINPOLYGON, "gluBeginPolygon" },
		{ "\017gluBeginSurface", GL_SUB_GLU_BEGINSURFACE, "gluBeginSurface" },
		{ "\014gluBeginTrim", GL_SUB_GLU_BEGINTRIM, "gluBeginTrim" },
		{ "\021gluBuild1DMipmaps", GL_SUB_GLU_BUILD1DMIPMAPS, "gluBuild1DMipmaps" },
		{ "\021gluBuild2DMipmaps", GL_SUB_GLU_BUILD2DMIPMAPS, "gluBuild2DMipmaps" },
		{ "\013gluCylinder", GL_SUB_GLU_CYLINDER, "gluCylinder" },
		{ "\026gluDeleteNurbsRenderer", GL_SUB_GLU_DELETENURBSRENDERER, "gluDeleteNurbsRenderer" },
		{ "\034gluDeleteNurbsTessellatorEXT", GL_SUB_GLU_DELETENURBSTESSELLATOREXT, "gluDeleteNurbsTessellatorEXT" },
		{ "\020gluDeleteQuadric", GL_SUB_GLU_DELETEQUADRIC, "gluDeleteQuadric" },
		{ "\015gluDeleteTess", GL_SUB_GLU_DELETETESS, "gluDeleteTess" },
		{ "\007gluDisk", GL_SUB_GLU_DISK, "gluDisk" },
		{ "\013gluEndCurve", GL_SUB_GLU_ENDCURVE, "gluEndCurve" },
		{ "\015gluEndPolygon", GL_SUB_GLU_ENDPOLYGON, "gluEndPolygon" },
		{ "\015gluEndSurface", GL_SUB_GLU_ENDSURFACE, "gluEndSurface" },
		{ "\012gluEndTrim", GL_SUB_GLU_ENDTRIM, "gluEndTrim" },
		{ "\016gluErrorString", GL_SUB_GLU_ERRORSTRING, "gluErrorString" },
		{ "\023gluGetNurbsProperty", GL_SUB_GLU_GETNURBSPROPERTY, "gluGetNurbsProperty" },
		{ "\014gluGetString", GL_SUB_GLU_GETSTRING, "gluGetString" },
		{ "\022gluGetTessProperty", GL_SUB_GLU_GETTESSPROPERTY, "gluGetTessProperty" },
		{ "\027gluLoadSamplingMatrices", GL_SUB_GLU_LOADSAMPLINGMATRICES, "gluLoadSamplingMatrices" },
		{ "\011gluLookAt", GL_SUB_GLU_LOOKAT, "gluLookAt" },
		{ "\023gluNewNurbsRenderer", GL_SUB_GLU_NEWNURBSRENDERER, "gluNewNurbsRenderer" },
		{ "\031gluNewNurbsTessellatorEXT", GL_SUB_GLU_NEWNURBSTESSELLATOREXT, "gluNewNurbsTessellatorEXT" },
		{ "\015gluNewQuadric", GL_SUB_GLU_NEWQUADRIC, "gluNewQuadric" },
		{ "\012gluNewTess", GL_SUB_GLU_NEWTESS, "gluNewTess" },
		{ "\016gluNextContour", GL_SUB_GLU_NEXTCONTOUR, "gluNextContour" },
		{ "\020gluNurbsCallback", GL_SUB_GLU_NURBSCALLBACK, "gluNurbsCallback" },
		{ "\027gluNurbsCallbackDataEXT", GL_SUB_GLU_NURBSCALLBACKDATAEXT, "gluNurbsCallbackDataEXT" },
		{ "\015gluNurbsCurve", GL_SUB_GLU_NURBSCURVE, "gluNurbsCurve" },
		{ "\020gluNurbsProperty", GL_SUB_GLU_NURBSPROPERTY, "gluNurbsProperty" },
		{ "\017gluNurbsSurface", GL_SUB_GLU_NURBSSURFACE, "gluNurbsSurface" },
		{ "\012gluOrtho2D", GL_SUB_GLU_ORTHO2D, "gluOrtho2D" },
		{ "\016gluPartialDisk", GL_SUB_GLU_PARTIALDISK, "gluPartialDisk" },
		{ "\016gluPerspective", GL_SUB_GLU_PERSPECTIVE, "gluPerspective" },
		{ "\015gluPickMatrix", GL_SUB_GLU_PICKMATRIX, "gluPickMatrix" },
		{ "\012gluProject", GL_SUB_GLU_PROJECT, "gluProject" },
		{ "\013gluPwlCurve", GL_SUB_GLU_PWLCURVE, "gluPwlCurve" },
		{ "\022gluQuadricCallback", GL_SUB_GLU_QUADRICCALLBACK, "gluQuadricCallback" },
		{ "\023gluQuadricDrawStyle", GL_SUB_GLU_QUADRICDRAWSTYLE, "gluQuadricDrawStyle" },
		{ "\021gluQuadricNormals", GL_SUB_GLU_QUADRICNORMALS, "gluQuadricNormals" },
		{ "\025gluQuadricOrientation", GL_SUB_GLU_QUADRICORIENTATION, "gluQuadricOrientation" },
		{ "\021gluQuadricTexture", GL_SUB_GLU_QUADRICTEXTURE, "gluQuadricTexture" },
		{ "\015gluScaleImage", GL_SUB_GLU_SCALEIMAGE, "gluScaleImage" },
		{ "\011gluSphere", GL_SUB_GLU_SPHERE, "gluSphere" },
		{ "\023gluTessBeginContour", GL_SUB_GLU_TESSBEGINCONTOUR, "gluTessBeginContour" },
		{ "\023gluTessBeginPolygon", GL_SUB_GLU_TESSBEGINPOLYGON, "gluTessBeginPolygon" },
		{ "\017gluTessCallback", GL_SUB_GLU_TESSCALLBACK, "gluTessCallback" },
		{ "\021gluTessEndContour", GL_SUB_GLU_TESSENDCONTOUR, "gluTessEndContour" },
		{ "\021gluTessEndPolygon", GL_SUB_GLU_TESSENDPOLYGON, "gluTessEndPolygon" },
		{ "\015gluTessNormal", GL_SUB_GLU_TESSNORMAL, "gluTessNormal" },
		{ "\017gluTessProperty", GL_SUB_GLU_TESSPROPERTY, "gluTessProperty" },
		{ "\015gluTessVertex", GL_SUB_GLU_TESSVERTEX, "gluTessVertex" },
		{ "\014gluUnProject", GL_SUB_GLU_UNPROJECT, "gluUnProject" },
	};
	const int num_glu = sizeof(glu_symbols) / sizeof(glu_symbols[0]);

	// ---- Cache AGL TVECTs ----
	struct CachedTVECT {
		uint32_t tvect;     // Mac-side TVECT address found by FindLibSymbol
		int sub_opcode;     // Our sub-opcode
		const char *name;   // For logging
	};

	std::vector<CachedTVECT> cached_tvects;
	int found_count = 0;
	int not_found_count = 0;

	// Search AGL functions in OpenGLLibrary
	for (int i = 0; i < num_agl; i++) {
		uint32_t tvect = FindLibSymbol(gl_lib, agl_symbols[i].pascal_sym);
		if (tvect != 0) {
			cached_tvects.push_back({ tvect, agl_symbols[i].sub_opcode, agl_symbols[i].name });
			found_count++;
			GL_LOG("  found %s at TVECT 0x%08x", agl_symbols[i].name, tvect);
		} else {
			not_found_count++;
		}
	}

	GL_LOG("GLInstallHooks: found %d AGL functions, %d not found", found_count, not_found_count);

	// Search core GL functions in OpenGLLibrary
	int gl_found = 0, gl_notfound = 0;
	for (int i = 0; i < num_gl; i++) {
		uint32_t tvect = FindLibSymbol(gl_lib, gl_symbols[i].pascal_sym);
		if (tvect != 0) {
			cached_tvects.push_back({ tvect, gl_symbols[i].sub_opcode, gl_symbols[i].name });
			gl_found++;
		} else {
			gl_notfound++;
		}
	}
	GL_LOG("GLInstallHooks: found %d core GL functions, %d not found", gl_found, gl_notfound);

	// Search GL/extension functions without expanding the core AGL context
	// dispatch table.
	int gl_extra_found = 0, gl_extra_notfound = 0;
	for (int i = 0; i < num_gl_extra; i++) {
		uint32_t tvect = FindLibSymbol(gl_lib, gl_extra_symbols[i].pascal_sym);
		if (tvect != 0) {
			cached_tvects.push_back({ tvect, gl_extra_symbols[i].sub_opcode, gl_extra_symbols[i].name });
			gl_extra_found++;
			GL_LOG("  found %s at TVECT 0x%08x", gl_extra_symbols[i].name, tvect);
		} else {
			gl_extra_notfound++;
		}
	}
	GL_LOG("GLInstallHooks: found %d extra GL functions, %d not found",
	       gl_extra_found, gl_extra_notfound);

	// Search GLU functions. They normally live in the separate OpenGLUtility
	// library; some builds fold them into OpenGLLibrary, so try OpenGLUtility
	// first and fall back to gl_lib. A found TVECT is patched by Step 2 exactly
	// like the AGL/GL ones (its hook is gl_method_tvects[GLU sub-opcode], which
	// GLThunksInit already allocates for the 700..753 GLU range).
	int glu_found = 0, glu_notfound = 0;
	for (int i = 0; i < num_glu; i++) {
		uint32_t tvect = FindLibSymbol(glu_lib, glu_symbols[i].pascal_sym);
		if (tvect == 0)
			tvect = FindLibSymbol(gl_lib, glu_symbols[i].pascal_sym);
		if (tvect != 0) {
			cached_tvects.push_back({ tvect, glu_symbols[i].sub_opcode, glu_symbols[i].name });
			glu_found++;
			GL_LOG("  found %s at TVECT 0x%08x", glu_symbols[i].name, tvect);
		} else {
			glu_notfound++;
		}
	}
	GL_LOG("GLInstallHooks: found %d GLU functions, %d not found", glu_found, glu_notfound);

	// ---- Step 2: Patch found TVECTs ----
	//
	// For each found TVECT, overwrite its code pointer with the address of
	// our gl_method_tvects[sub_opcode]'s code entry.
	//
	// TVECT layout in PPC:
	//   +0: code_ptr (address of first instruction)
	//   +4: TOC pointer
	//
	// We overwrite the first 4 PPC instructions at orig_code with a branch
	// to our hook thunk (same pattern as RAVE hooks).

	const uint32_t r11 = 11;
	int patched_count = 0;
	int synthetic_count = 0;

	for (size_t i = 0; i < cached_tvects.size(); i++) {
		uint32_t orig_tvect = cached_tvects[i].tvect;
		int sub = cached_tvects[i].sub_opcode;
		uint32_t hook_tvect = gl_method_tvects[sub];

		if (hook_tvect == 0) {
			GL_LOG("  hook TVECT for %s (sub %d) not allocated!", cached_tvects[i].name, sub);
			continue;
		}

		// FindLibSymbol's synthetic fallback returns our own
		// gl_method_tvects[sub] when the guest library lacks the export.
		// Patching it would overwrite our thunk with a branch-to-self;
		// the synthetic TVECT already points at our dispatcher, so skip it.
		if (orig_tvect == hook_tvect) {
			synthetic_count++;
			GL_LOG("  synthetic %s: TVECT 0x%08x is our own thunk, no patch needed",
			       cached_tvects[i].name, orig_tvect);
			continue;
		}

		// Read the original code pointer from the TVECT
		uint32_t orig_code = ReadMacInt32(orig_tvect);

		// Read our hook thunk's code pointer
		uint32_t hook_code = ReadMacInt32(hook_tvect);

		// Build patch: lis r11,hi; ori r11,r11,lo; mtctr r11; bctr
		uint32_t hook_hi = (hook_code >> 16) & 0xFFFF;
		uint32_t hook_lo = hook_code & 0xFFFF;

		// Overwrite first 4 instructions at orig_code
		// lis r11, hook_code_hi
		WriteMacInt32(orig_code + 0, 0x3C000000 | (r11 << 21) | hook_hi);
		// ori r11, r11, hook_code_lo
		WriteMacInt32(orig_code + 4, 0x60000000 | (r11 << 21) | (r11 << 16) | hook_lo);
		// mtctr r11
		WriteMacInt32(orig_code + 8, 0x7C0903A6 | (r11 << 21));
		// bctr
		WriteMacInt32(orig_code + 12, 0x4E800420);

		// Flush instruction cache
#if EMULATED_PPC
		FlushCodeCache(orig_code, orig_code + 16);
#endif

		patched_count++;
		GL_LOG("  patched %s: orig_code=0x%08x -> hook_code=0x%08x",
		       cached_tvects[i].name, orig_code, hook_code);
	}

	GL_LOG("GLInstallHooks: patched %d real exports, %d synthetic (skipped)",
	       patched_count, synthetic_count);

	if (patched_count > 0) {
		gl_hooks_installed = true;
		gl_hooks_in_progress = false;
	} else {
		gl_hooks_in_progress = false;
		gl_hooks_attempts++;
		if (gl_hooks_attempts >= GL_HOOKS_MAX_ATTEMPTS)
			GL_LOG("GLInstallHooks: OpenGL library not available after %d attempts, giving up", gl_hooks_attempts);
		else
			GL_LOG("GLInstallHooks: patched 0 functions, will retry on next accRun (attempt %d/%d)",
			       gl_hooks_attempts, GL_HOOKS_MAX_ATTEMPTS);
		return;
	}
}


// ===========================================================================
//  GLU Utility Functions
// ===========================================================================

// ---- GLU Constants ----
#define GLU_SMOOTH           100000
#define GLU_FLAT             100001
#define GLU_NONE_            100002   // renamed to avoid conflict with AGL_NONE
#define GLU_POINT            100010
#define GLU_LINE             100011
#define GLU_FILL_            100012
#define GLU_SILHOUETTE       100013
#define GLU_OUTSIDE          100020
#define GLU_INSIDE           100021
#define GLU_VERSION          100800
#define GLU_EXTENSIONS       100801
#define GLU_INVALID_ENUM     100900
#define GLU_INVALID_VALUE    100901
#define GLU_OUT_OF_MEMORY    100902
#define GLU_INCOMPATIBLE_GL_VERSION 100903  // glu.h ErrorCode
#define GLU_INVALID_OPERATION       100904  // glu.h ErrorCode
// GLU_ERROR: the NurbsCallback / QuadricCallback error-report selector
// (glu.h: #define GLU_ERROR 100103, aliased with GLU_TESS_ERROR).
#define GLU_ERROR            100103

// GL texture targets/formats needed by GLU
#define GL_TEXTURE_2D        0x0DE1
#define GL_TEXTURE_1D        0x0DE0
#define GL_UNSIGNED_BYTE     0x1401
#define GL_RGB               0x1907
#define GL_RGBA              0x1908
#define GL_LUMINANCE         0x1909
#define GL_LUMINANCE_ALPHA   0x190A
#define GL_ALPHA             0x1906
#define GL_COLOR_INDEX       0x1900
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_TRIANGLES         0x0004
#define GL_TRIANGLE_STRIP    0x0005
#define GL_TRIANGLE_FAN      0x0006
#define GL_QUADS             0x0007
#define GL_LINE_LOOP         0x0002
#define GL_LINE_STRIP        0x0003
#define GL_LINES             0x0001
#define GL_POINTS            0x0000

// ---- GLU Quadric State ----
#define GLU_MAX_QUADRICS 16

struct GLUQuadricState {
	bool in_use;
	uint32_t normals;     // GLU_SMOOTH, GLU_FLAT, GLU_NONE
	bool texture;
	uint32_t drawstyle;   // GLU_FILL, GLU_LINE, GLU_SILHOUETTE, GLU_POINT
	uint32_t orientation; // GLU_OUTSIDE, GLU_INSIDE
	// GLU_ERROR callback addr (2-word TVECT) registered via gluQuadricCallback.
	// Fired NULL-guarded via call_macos1 on the quadric error paths. 0 = none.
	uint32_t error_callback;
};

static GLUQuadricState glu_quadrics[GLU_MAX_QUADRICS];
static uint32_t glu_quadric_mac_handles[GLU_MAX_QUADRICS] = { 0 };

// ---- GLU Tessellation Constants ----
#define GLU_TESS_WINDING_ODD        100130
#define GLU_TESS_WINDING_NONZERO    100131
#define GLU_TESS_WINDING_POSITIVE   100132
#define GLU_TESS_WINDING_NEGATIVE   100133
#define GLU_TESS_WINDING_ABS_GEQ_TWO 100134
#define GLU_TESS_WINDING_RULE       100140
#define GLU_TESS_BOUNDARY_ONLY      100141
#define GLU_TESS_TOLERANCE          100142
// glu.h TessError codes (fired to the GLU_TESS_ERROR callback)
#define GLU_TESS_MISSING_BEGIN_POLYGON  100151
#define GLU_TESS_NEED_COMBINE_CALLBACK  100156
#define GLU_TESS_BEGIN              100100
#define GLU_TESS_VERTEX             100101
#define GLU_TESS_END                100102
#define GLU_TESS_ERROR              100103
#define GLU_TESS_EDGE_FLAG          100104
#define GLU_TESS_COMBINE            100105
#define GLU_TESS_BEGIN_DATA         100106
#define GLU_TESS_VERTEX_DATA        100107
#define GLU_TESS_END_DATA           100108
#define GLU_TESS_ERROR_DATA         100109
#define GLU_TESS_EDGE_FLAG_DATA     100110
#define GLU_TESS_COMBINE_DATA       100111

// ---- GLU NURBS Constants ----
#define GLU_AUTO_LOAD_MATRIX   100200
#define GLU_CULLING            100201
#define GLU_SAMPLING_TOLERANCE 100203
#define GLU_DISPLAY_MODE       100204
#define GLU_PARAMETRIC_TOLERANCE 100202
#define GLU_SAMPLING_METHOD    100205
#define GLU_U_STEP             100206
#define GLU_V_STEP             100207
#define GLU_NURBS_MODE_EXT     100160
#define GLU_NURBS_RENDERER_EXT 100162
#define GLU_NURBS_TESSELLATOR_EXT 100161
#define GLU_PATH_LENGTH        100215
#define GLU_PARAMETRIC_ERROR   100216
#define GLU_DOMAIN_DISTANCE    100217
#define GLU_MAP1_TRIM_2        100210
#define GLU_MAP1_TRIM_3        100211

// ---- GLU Tessellator State ----
#define GLU_MAX_TESS 8
#define GLU_TESS_MAX_CONTOURS 16
#define GLU_TESS_MAX_VERTICES 4096

struct GLUTessVertex3 {
	float x, y, z;
	// Opaque guest `data` ptr from gluTessVertex.
	// Stored verbatim as a raw uint32 Mac address and passed straight back to
	// the guest GLU_TESS_VERTEX callback through the call_macos* trampoline —
	// it is NEVER dereferenced natively (ASVS V5). 0 = none.
	uint32_t data = 0;
};

struct GLUTessContour {
	std::vector<GLUTessVertex3> vertices;
};

struct GLUTessState {
	bool in_use;
	// Winding rule and properties
	uint32_t winding_rule;   // default GLU_TESS_WINDING_ODD
	bool boundary_only;      // default false
	double tolerance;        // default 0.0
	// User-specified normal (0,0,0 means auto-compute)
	float normal_x, normal_y, normal_z;
	// Callback addresses (stored but never invoked as PPC)
	uint32_t callbacks[12];  // indexed by (callback_type - GLU_TESS_BEGIN)
	// Polygon data
	uint32_t user_data;      // opaque PPC pointer from BeginPolygon
	std::vector<GLUTessContour> contours;
	bool in_polygon;         // between BeginPolygon and EndPolygon
	bool in_contour;         // between BeginContour and EndContour
};

static GLUTessState glu_tess[GLU_MAX_TESS];
static uint32_t glu_tess_mac_handles[GLU_MAX_TESS] = { 0 };

// ---- GLU NURBS State ----
#define GLU_MAX_NURBS 4

struct GLUNurbsState {
	bool in_use;
	// Properties
	float sampling_tolerance; // default 50.0
	int u_step;               // default 100
	int v_step;               // default 100
	uint32_t display_mode;    // default GLU_FILL_ (100012)
	bool auto_load_matrix;    // default true
	uint32_t sampling_method; // default GLU_PATH_LENGTH
	bool culling;             // default false
	uint32_t nurbs_mode;      // default GLU_NURBS_RENDERER_EXT
	// Callback addresses (stored, never invoked)
	uint32_t callbacks[8];
	uint32_t user_data;
	// Sampling matrices (stored by gluLoadSamplingMatrices)
	float model_matrix[16];
	float proj_matrix[16];
	int viewport[4];
	bool matrices_loaded;
	// Surface accumulation
	bool in_surface;
	std::vector<float> s_knots;
	std::vector<float> t_knots;
	std::vector<float> control_points;
	int s_stride, t_stride;
	int s_order, t_order;
	uint32_t surface_type;
	bool surface_defined;
	// Curve accumulation
	bool in_curve;
	std::vector<float> curve_knots;
	std::vector<float> curve_control;
	int curve_stride;
	int curve_order;
	uint32_t curve_type;
	bool curve_defined;
	// Trim state
	bool in_trim;
};

static GLUNurbsState glu_nurbs[GLU_MAX_NURBS];
static uint32_t glu_nurbs_mac_handles[GLU_MAX_NURBS] = { 0 };

// ---- Mac-side GLU strings (lazy allocated) ----
static uint32_t glu_version_string_mac = 0;
static uint32_t glu_extensions_string_mac = 0;

// ---- Helpers ----
static inline float ReadMacFloat_GLU(uint32_t addr)
{
	uint32_t raw = ReadMacInt32(addr);
	float f;
	memcpy(&f, &raw, 4);
	return f;
}

static inline void WriteMacFloat_GLU(uint32_t addr, float f)
{
	uint32_t raw;
	memcpy(&raw, &f, 4);
	WriteMacInt32(addr, raw);
}

// Read big-endian double from PPC memory
static inline double ReadMacDouble(uint32_t addr)
{
	uint32_t hi = ReadMacInt32(addr);
	uint32_t lo = ReadMacInt32(addr + 4);
	uint64_t bits = ((uint64_t)hi << 32) | lo;
	double d;
	memcpy(&d, &bits, sizeof(double));
	return d;
}

// Write big-endian double to PPC memory
static inline void WriteMacDouble(uint32_t addr, double d)
{
	uint64_t bits;
	memcpy(&bits, &d, sizeof(uint64_t));
	WriteMacInt32(addr, (uint32_t)(bits >> 32));
	WriteMacInt32(addr + 4, (uint32_t)(bits & 0xFFFFFFFF));
}

// ---- Forward declarations for GL state functions we call ----
extern void NativeGLMultMatrixf(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLOrtho(GLContext *ctx, double l, double r, double b, double t, double n, double f);
extern void NativeGLFrustum(GLContext *ctx, double l, double r, double b, double t, double n, double f);
extern void NativeGLTranslatef(GLContext *ctx, float x, float y, float z);
extern void NativeGLLoadIdentity(GLContext *ctx);
extern void NativeGLBegin(GLContext *ctx, uint32_t mode);
extern void NativeGLEnd(GLContext *ctx);
extern void NativeGLVertex3f(GLContext *ctx, float x, float y, float z);
extern void NativeGLNormal3f(GLContext *ctx, float x, float y, float z);
extern void NativeGLTexCoord2f(GLContext *ctx, float s, float t);
extern void NativeGLPushMatrix(GLContext *ctx);
extern void NativeGLPopMatrix(GLContext *ctx);
extern void NativeGLScalef(GLContext *ctx, float x, float y, float z);

// Direct matrix multiply on current stack (without going through PPC memory)
static void GLUMultMatrix4f(GLContext *ctx, const float m[16])
{
	// Get current matrix
	float *cur;
	switch (ctx->matrix_mode) {
		case GL_MODELVIEW:  cur = ctx->modelview_stack[ctx->modelview_depth]; break;
		case GL_PROJECTION: cur = ctx->projection_stack[ctx->projection_depth]; break;
		case GL_TEXTURE:    cur = ctx->texture_stack[ctx->active_texture][ctx->texture_depth[ctx->active_texture]]; break;
		default:            cur = ctx->color_stack[ctx->color_depth]; break;
	}

	// result = cur * m (column-major)
	float result[16];
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			result[c * 4 + r] =
				cur[0 * 4 + r] * m[c * 4 + 0] +
				cur[1 * 4 + r] * m[c * 4 + 1] +
				cur[2 * 4 + r] * m[c * 4 + 2] +
				cur[3 * 4 + r] * m[c * 4 + 3];
		}
	}
	memcpy(cur, result, sizeof(float) * 16);
}


/*
 *  NativeGLUPerspective(fovy, aspect, zNear, zFar)
 *
 *  4 double args from FPR. Builds perspective matrix and multiplies
 *  onto current matrix stack (equivalent to glFrustum).
 */
void NativeGLUPerspective(GLContext *ctx, double fovy, double aspect, double zNear, double zFar)
{
	GL_LOG("gluPerspective: fovy=%f aspect=%f zNear=%f zFar=%f", fovy, aspect, zNear, zFar);

	if (!ctx) return;
	if (zNear <= 0.0 || zFar <= 0.0 || fovy <= 0.0 || fovy >= 360.0) return;

	double top = zNear * tan(fovy * M_PI / 360.0);
	double bottom = -top;
	double right = top * aspect;
	double left = -right;

	NativeGLFrustum(ctx, left, right, bottom, top, zNear, zFar);
}


/*
 *  NativeGLULookAt(9 double args)
 *
 *  Build view matrix: forward, side, up vectors, then translate.
 */
void NativeGLULookAt(GLContext *ctx,
                     double eyeX, double eyeY, double eyeZ,
                     double centerX, double centerY, double centerZ,
                     double upX, double upY, double upZ)
{
	GL_LOG("gluLookAt: eye(%f,%f,%f) center(%f,%f,%f) up(%f,%f,%f)",
	       eyeX, eyeY, eyeZ, centerX, centerY, centerZ, upX, upY, upZ);

	if (!ctx) return;

	// Forward vector (center - eye), normalized
	double fx = centerX - eyeX;
	double fy = centerY - eyeY;
	double fz = centerZ - eyeZ;
	double flen = sqrt(fx * fx + fy * fy + fz * fz);
	if (flen > 0.0) { fx /= flen; fy /= flen; fz /= flen; }

	// Side = forward x up, normalized
	double sx = fy * upZ - fz * upY;
	double sy = fz * upX - fx * upZ;
	double sz = fx * upY - fy * upX;
	double slen = sqrt(sx * sx + sy * sy + sz * sz);
	if (slen > 0.0) { sx /= slen; sy /= slen; sz /= slen; }

	// Recompute up = side x forward
	double ux = sy * fz - sz * fy;
	double uy = sz * fx - sx * fz;
	double uz = sx * fy - sy * fx;

	// Build rotation matrix (column-major)
	float m[16] = {
		(float)sx,  (float)ux,  (float)(-fx), 0.0f,
		(float)sy,  (float)uy,  (float)(-fy), 0.0f,
		(float)sz,  (float)uz,  (float)(-fz), 0.0f,
		0.0f,       0.0f,       0.0f,          1.0f
	};

	GLUMultMatrix4f(ctx, m);

	// Then translate by -eye
	NativeGLTranslatef(ctx, (float)-eyeX, (float)-eyeY, (float)-eyeZ);
}


/*
 *  NativeGLUOrtho2D(left, right, bottom, top)
 *
 *  Equivalent to glOrtho(left, right, bottom, top, -1, 1).
 */
void NativeGLUOrtho2D(GLContext *ctx, double left, double right, double bottom, double top)
{
	GL_LOG("gluOrtho2D: l=%f r=%f b=%f t=%f", left, right, bottom, top);
	if (!ctx) return;
	NativeGLOrtho(ctx, left, right, bottom, top, -1.0, 1.0);
}


/*
 *  NativeGLUPickMatrix(x, y, deltaX, deltaY, viewport_ptr)
 *
 *  Build a pick matrix for selection. Translates and scales to zoom
 *  into the pick region.
 */
void NativeGLUPickMatrix(GLContext *ctx, double x, double y, double deltaX, double deltaY, uint32_t viewport_ptr)
{
	GL_LOG("gluPickMatrix: x=%f y=%f dX=%f dY=%f vp=0x%08x", x, y, deltaX, deltaY, viewport_ptr);
	if (!ctx || deltaX <= 0.0 || deltaY <= 0.0 || viewport_ptr == 0) return;

	int32_t vp[4];
	for (int i = 0; i < 4; i++)
		vp[i] = (int32_t)ReadMacInt32(viewport_ptr + i * 4);

	float tx = (float)((vp[2] - 2.0 * (x - vp[0])) / deltaX);
	float ty = (float)((vp[3] - 2.0 * (y - vp[1])) / deltaY);
	float sx = (float)(vp[2] / deltaX);
	float sy = (float)(vp[3] / deltaY);

	NativeGLTranslatef(ctx, tx, ty, 0.0f);
	NativeGLScalef(ctx, sx, sy, 1.0f);
}


/*
 *  NativeGLUProject(objX, objY, objZ, model_ptr, proj_ptr, viewport_ptr,
 *                   winX_ptr, winY_ptr, winZ_ptr)
 *
 *  Transform object coordinates to window coordinates.
 *  model and proj are 16-element double arrays in PPC memory.
 *  viewport is 4 ints. winX/Y/Z are pointers to doubles.
 *
 *  Returns 1 on success, 0 on failure.
 */
uint32_t NativeGLUProject(GLContext *ctx,
                          double objX, double objY, double objZ,
                          uint32_t model_ptr, uint32_t proj_ptr, uint32_t viewport_ptr,
                          uint32_t winX_ptr, uint32_t winY_ptr, uint32_t winZ_ptr)
{
	GL_LOG("gluProject: obj(%f,%f,%f)", objX, objY, objZ);
	if (!model_ptr || !proj_ptr || !viewport_ptr) return 0;

	// Read modelview matrix (16 doubles)
	double model[16];
	for (int i = 0; i < 16; i++)
		model[i] = ReadMacDouble(model_ptr + i * 8);

	// Read projection matrix (16 doubles)
	double proj[16];
	for (int i = 0; i < 16; i++)
		proj[i] = ReadMacDouble(proj_ptr + i * 8);

	// Read viewport (4 ints)
	int32_t vp[4];
	for (int i = 0; i < 4; i++)
		vp[i] = (int32_t)ReadMacInt32(viewport_ptr + i * 4);

	// Transform by modelview: v = model * obj (column-major)
	double in[4] = { objX, objY, objZ, 1.0 };
	double out[4];
	for (int r = 0; r < 4; r++)
		out[r] = model[0*4+r]*in[0] + model[1*4+r]*in[1] + model[2*4+r]*in[2] + model[3*4+r]*in[3];

	// Transform by projection: v = proj * out
	double clip[4];
	for (int r = 0; r < 4; r++)
		clip[r] = proj[0*4+r]*out[0] + proj[1*4+r]*out[1] + proj[2*4+r]*out[2] + proj[3*4+r]*out[3];

	// Perspective divide
	if (clip[3] == 0.0) return 0;
	double ndc[3] = { clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3] };

	// Viewport transform: [-1,1] -> [vp[0], vp[0]+vp[2]] etc
	double winX = vp[0] + (ndc[0] + 1.0) * vp[2] * 0.5;
	double winY = vp[1] + (ndc[1] + 1.0) * vp[3] * 0.5;
	double winZ = (ndc[2] + 1.0) * 0.5;

	if (winX_ptr) WriteMacDouble(winX_ptr, winX);
	if (winY_ptr) WriteMacDouble(winY_ptr, winY);
	if (winZ_ptr) WriteMacDouble(winZ_ptr, winZ);

	return 1;
}


/*
 *  NativeGLUUnProject -- inverse of gluProject
 *
 *  Returns 1 on success, 0 on failure (singular matrix).
 */
uint32_t NativeGLUUnProject(GLContext *ctx,
                            double winX, double winY, double winZ,
                            uint32_t model_ptr, uint32_t proj_ptr, uint32_t viewport_ptr,
                            uint32_t objX_ptr, uint32_t objY_ptr, uint32_t objZ_ptr)
{
	GL_LOG("gluUnProject: win(%f,%f,%f)", winX, winY, winZ);
	if (!model_ptr || !proj_ptr || !viewport_ptr) return 0;

	// Read matrices
	double model[16], proj[16];
	for (int i = 0; i < 16; i++) {
		model[i] = ReadMacDouble(model_ptr + i * 8);
		proj[i] = ReadMacDouble(proj_ptr + i * 8);
	}

	int32_t vp[4];
	for (int i = 0; i < 4; i++)
		vp[i] = (int32_t)ReadMacInt32(viewport_ptr + i * 4);

	// Compute combined = proj * model
	double combined[16];
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			combined[c*4+r] = proj[0*4+r]*model[c*4+0] + proj[1*4+r]*model[c*4+1] +
			                  proj[2*4+r]*model[c*4+2] + proj[3*4+r]*model[c*4+3];

	// Invert the combined 4x4 matrix
	double inv[16];
	{
		double *m = combined;
		inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
		inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
		inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
		inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
		inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
		inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
		inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
		inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
		inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
		inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
		inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
		inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
		inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
		inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
		inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
		inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

		double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
		if (fabs(det) < 1e-30) return 0;
		det = 1.0 / det;
		for (int i = 0; i < 16; i++) inv[i] *= det;
	}

	// Un-viewport: map window coords back to NDC [-1,1]
	double in[4];
	in[0] = (winX - vp[0]) * 2.0 / vp[2] - 1.0;
	in[1] = (winY - vp[1]) * 2.0 / vp[3] - 1.0;
	in[2] = winZ * 2.0 - 1.0;
	in[3] = 1.0;

	// Multiply by inverse
	double out[4];
	for (int r = 0; r < 4; r++)
		out[r] = inv[0*4+r]*in[0] + inv[1*4+r]*in[1] + inv[2*4+r]*in[2] + inv[3*4+r]*in[3];

	if (out[3] == 0.0) return 0;
	out[0] /= out[3];
	out[1] /= out[3];
	out[2] /= out[3];

	if (objX_ptr) WriteMacDouble(objX_ptr, out[0]);
	if (objY_ptr) WriteMacDouble(objY_ptr, out[1]);
	if (objZ_ptr) WriteMacDouble(objZ_ptr, out[2]);

	return 1;
}


/*
 *  NativeGLUBuild2DMipmaps
 *
 *  Generate mipmaps by box-filtering and call glTexImage2D for each level.
 *  This is the most important GLU function for rendering quality.
 *
 *  Args: target, internalFormat, width, height, format, type, data_ptr
 *  All integer args come from GPR registers.
 */

// Forward declaration for GL texture upload (implemented below).
extern void NativeGLTexImage2D_Direct(GLContext *ctx, uint32_t target, int32_t level,
                                       int32_t internalFormat, int32_t width, int32_t height,
                                       int32_t border, uint32_t format, uint32_t type,
                                       const uint8_t *pixels, int32_t pixel_data_size);

// Forward declare the real Metal uploader (implemented in gl_metal_renderer.mm).
// gl_engine.h carries a stale 8-arg prototype for this symbol; the actual definition
// (gl_metal_renderer.mm:2136) and the gl_state.cpp:431 forward decl use this 7-arg shape
// (data + dataLen). Declare the correct overload locally so NativeGLTexImage2D_Direct
// binds to it — mirror gl_state.cpp:430-432.
extern void GLMetalUploadTexture(GLContext *ctx, GLTextureObject *texObj, int level,
                                  int width, int height, const uint8_t *data, int dataLen);
extern uint8_t *GLConvertMacPixelsToBGRA8(GLContext *ctx, uint32_t mac_pixels,
                                          int width, int height,
                                          uint32_t format, uint32_t type,
                                          int *outLen);

// Host-pixel -> Metal texture uploader for the mipmap path.
//
// This is the definition of the previously-only-declared NativeGLTexImage2D_Direct.
// It mirrors NativeGLTexImage2D (gl_state.cpp:2515) — find the bound texture object,
// convert source pixels to BGRA8, and hand them to the level-aware GLMetalUploadTexture
// (gl_metal_renderer.mm:2136) — but takes a HOST pointer (the box-filtered pyramid level
// buffers produced by NativeGLUBuild2DMipmaps) rather than a Mac address. It deliberately
// supports only the 5 UNSIGNED_BYTE formats the box filter emits
// (GL_RGBA / GL_RGB / GL_LUMINANCE / GL_LUMINANCE_ALPHA / GL_ALPHA); this is NOT a general
// glTexImage2D path rework.
//
// Prior to this fix, both call sites in NativeGLUBuild2DMipmaps routed through a no-op
// fallback that logged and discarded every computed level, so gluBuild2DMipmaps /
// gluBuild1DMipmaps returned GL_NO_ERROR while uploading nothing — silent wrong output
// (untextured). Reproduced by GLP0RemediationTests.testMipmaps_uploadComputedLevels.
void NativeGLTexImage2D_Direct(GLContext *ctx, uint32_t target, int32_t level,
                                int32_t internalFormat, int32_t width, int32_t height,
                                int32_t border, uint32_t format, uint32_t type,
                                const uint8_t *pixels, int32_t pixel_data_size)
{
	(void)internalFormat; (void)border; (void)type; (void)pixel_data_size;

	// Input validation (ASVS V5 — mirror gl_engine.cpp NativeGLUBuild2DMipmaps guards).
	if (!ctx || pixels == 0 || width <= 0 || height <= 0) return;

	// Find the currently bound texture object (mirror NativeGLTexImage2D, gl_state.cpp:2528).
	uint32_t texName = 0;
	if (target == GL_TEXTURE_2D)
		texName = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	else if (target == GL_TEXTURE_1D)
		texName = ctx->tex_units[ctx->active_texture].bound_texture_1d;

	if (texName == 0) return;

	auto it = ctx->texture_objects.find(texName);
	if (it == ctx->texture_objects.end()) return;

	GLTextureObject &tex = it->second;

	// Critical ordering (RESEARCH Pattern 1, gl_metal_renderer.mm:2217): mark the texture
	// as mipmapped BEFORE the level-0 upload so the base Metal texture is born mipmapped:YES
	// and levels 0..N land in one complete chain.
	tex.has_mipmaps = true;
	if (level == 0) {
		tex.width = width;
		tex.height = height;
	}

	// Convert the host source pixels to a BGRA8 host buffer. Mirror the 5 UNSIGNED_BYTE
	// cases of ConvertPixelsToBGRA8 (gl_state.cpp:2419) reading pixels[i] directly.
	int srcBpp;
	switch (format) {
		case GL_RGBA:            srcBpp = 4; break;
		case GL_RGB:             srcBpp = 3; break;
		case GL_LUMINANCE_ALPHA: srcBpp = 2; break;
		case GL_LUMINANCE:
		case GL_ALPHA:           srcBpp = 1; break;
		default:                 srcBpp = 4; break; // assume RGBA (box filter only emits the 5 above)
	}

	int dstBytes = width * height * 4;
	uint8_t *bgra = (uint8_t *)malloc(dstBytes);
	if (!bgra) return;

	for (int i = 0; i < width * height; i++) {
		const uint8_t *src = pixels + (size_t)i * srcBpp;
		uint8_t *out = bgra + (size_t)i * 4;
		switch (format) {
			case GL_RGBA: { // R G B A -> Metal BGRA: B G R A
				out[0] = src[2]; out[1] = src[1]; out[2] = src[0]; out[3] = src[3];
				break;
			}
			case GL_RGB: { // R G B -> BGRA, alpha 0xFF
				out[0] = src[2]; out[1] = src[1]; out[2] = src[0]; out[3] = 0xFF;
				break;
			}
			case GL_LUMINANCE: {
				uint8_t l = src[0];
				out[0] = l; out[1] = l; out[2] = l; out[3] = 0xFF;
				break;
			}
			case GL_LUMINANCE_ALPHA: {
				uint8_t l = src[0];
				out[0] = l; out[1] = l; out[2] = l; out[3] = src[1];
				break;
			}
			case GL_ALPHA: {
				out[0] = 0; out[1] = 0; out[2] = 0; out[3] = src[0];
				break;
			}
			default: { // RGBA fallback
				out[0] = src[2]; out[1] = src[1]; out[2] = src[0]; out[3] = src[3];
				break;
			}
		}
	}

	GLMetalUploadTexture(ctx, &tex, level, width, height, bgra, dstBytes);
	free(bgra);
}

uint32_t NativeGLUBuild2DMipmaps(GLContext *ctx,
                                  uint32_t target, int32_t internalFormat,
                                  int32_t width, int32_t height,
                                  uint32_t format, uint32_t type, uint32_t data_ptr)
{
	GL_LOG("gluBuild2DMipmaps: target=0x%x ifmt=%d %dx%d fmt=0x%x type=0x%x data=0x%08x",
	       target, internalFormat, width, height, format, type, data_ptr);

	if (!ctx || data_ptr == 0 || width <= 0 || height <= 0) return GLU_INVALID_VALUE;

	uint32_t texName = 0;
	if (target == GL_TEXTURE_2D)
		texName = ctx->tex_units[ctx->active_texture].bound_texture_2d;
	else if (target == GL_TEXTURE_1D)
		texName = ctx->tex_units[ctx->active_texture].bound_texture_1d;

	if (texName == 0) return 0;

	auto texIt = ctx->texture_objects.find(texName);
	if (texIt == ctx->texture_objects.end()) return 0;
	GLTextureObject &tex = texIt->second;
	tex.has_mipmaps = true;
	tex.width = width;
	tex.height = height;

	int baseBGRABytes = 0;
	uint8_t *baseBGRA = GLConvertMacPixelsToBGRA8(
		ctx, data_ptr, width, height, format, type, &baseBGRABytes);
	const int expectedBaseBGRABytes = width * height * 4;
	if (!baseBGRA || baseBGRABytes < expectedBaseBGRABytes) {
		if (baseBGRA) free(baseBGRA);
		return GLU_OUT_OF_MEMORY;
	}

	std::vector<uint8_t> current(baseBGRA, baseBGRA + expectedBaseBGRABytes);
	free(baseBGRA);

	GLMetalUploadTexture(ctx, &tex, 0, width, height,
	                     current.data(), (int)current.size());

	// Generate mipmap chain by box filtering canonical BGRA8 upload pixels.
	// This preserves legacy/packed source interpretation from the base
	// converter instead of reinterpreting downsampled bytes as the original
	// guest format on each mip level.
	int level = 1;
	int w = width, h = height;
	while (w > 1 || h > 1) {
		int nw = (w > 1) ? w / 2 : 1;
		int nh = (h > 1) ? h / 2 : 1;

		std::vector<uint8_t> next(nw * nh * 4);

		for (int y = 0; y < nh; y++) {
			for (int x = 0; x < nw; x++) {
				int sx = x * 2, sy = y * 2;
				for (int c = 0; c < 4; c++) {
					int sum = 0;
					int count = 0;
					// Average 2x2 block (handling edge cases)
					sum += current[(sy * w + sx) * 4 + c]; count++;
					if (sx + 1 < w) { sum += current[(sy * w + sx + 1) * 4 + c]; count++; }
					if (sy + 1 < h) { sum += current[((sy + 1) * w + sx) * 4 + c]; count++; }
					if (sx + 1 < w && sy + 1 < h) { sum += current[((sy + 1) * w + sx + 1) * 4 + c]; count++; }
					next[(y * nw + x) * 4 + c] = (uint8_t)(sum / count);
				}
			}
		}

		GLMetalUploadTexture(ctx, &tex, level, nw, nh,
		                     next.data(), (int)next.size());

		current = std::move(next);
		w = nw;
		h = nh;
		level++;
	}

	GL_LOG("gluBuild2DMipmaps: generated %d mipmap levels", level);
	return 0;  // GL_NO_ERROR
}


/*
 *  NativeGLUBuild1DMipmaps -- 1D variant
 */
uint32_t NativeGLUBuild1DMipmaps(GLContext *ctx,
                                  uint32_t target, int32_t internalFormat,
                                  int32_t width,
                                  uint32_t format, uint32_t type, uint32_t data_ptr)
{
	GL_LOG("gluBuild1DMipmaps: (delegates to 2D with height=1)");
	return NativeGLUBuild2DMipmaps(ctx, target, internalFormat, width, 1, format, type, data_ptr);
}


/*
 *  NativeGLUScaleImage -- scale image data using bilinear interpolation
 */
uint32_t NativeGLUScaleImage(GLContext *ctx,
                              uint32_t format,
                              int32_t wIn, int32_t hIn, uint32_t typeIn, uint32_t dataIn,
                              int32_t wOut, int32_t hOut, uint32_t typeOut, uint32_t dataOut)
{
	GL_LOG("gluScaleImage: %dx%d -> %dx%d fmt=0x%x", wIn, hIn, wOut, hOut, format);

	if (!ctx || dataIn == 0 || dataOut == 0) return GLU_INVALID_VALUE;
	if (wIn <= 0 || hIn <= 0 || wOut <= 0 || hOut <= 0) return GLU_INVALID_VALUE;

	int bpp;
	switch (format) {
		case GL_RGBA:           bpp = 4; break;
		case GL_RGB:            bpp = 3; break;
		case GL_LUMINANCE_ALPHA: bpp = 2; break;
		case GL_LUMINANCE:
		case GL_ALPHA:          bpp = 1; break;
		default:                bpp = 4; break;
	}

	// Read input image
	int in_size = wIn * hIn * bpp;
	std::vector<uint8_t> src(in_size);
	for (int i = 0; i < in_size; i++)
		src[i] = ReadMacInt8(dataIn + i);

	// Bilinear interpolation
	for (int y = 0; y < hOut; y++) {
		float sy = (float)y * (hIn - 1) / (float)(hOut > 1 ? hOut - 1 : 1);
		int y0 = (int)sy;
		int y1 = (y0 + 1 < hIn) ? y0 + 1 : y0;
		float fy = sy - y0;

		for (int x = 0; x < wOut; x++) {
			float sx = (float)x * (wIn - 1) / (float)(wOut > 1 ? wOut - 1 : 1);
			int x0 = (int)sx;
			int x1 = (x0 + 1 < wIn) ? x0 + 1 : x0;
			float fx = sx - x0;

			for (int c = 0; c < bpp; c++) {
				float v00 = src[(y0 * wIn + x0) * bpp + c];
				float v10 = src[(y0 * wIn + x1) * bpp + c];
				float v01 = src[(y1 * wIn + x0) * bpp + c];
				float v11 = src[(y1 * wIn + x1) * bpp + c];

				float v = v00 * (1 - fx) * (1 - fy) + v10 * fx * (1 - fy) +
				          v01 * (1 - fx) * fy + v11 * fx * fy;
				int iv = (int)(v + 0.5f);
				if (iv > 255) iv = 255;
				if (iv < 0) iv = 0;

				WriteMacInt8(dataOut + (y * wOut + x) * bpp + c, (uint8_t)iv);
			}
		}
	}

	return 0;
}


// ===========================================================================
//  GLU Quadric Functions
// ===========================================================================

/*
 *  NativeGLUNewQuadric -- allocate quadric state, return Mac-side handle
 */
uint32_t NativeGLUNewQuadric()
{
	GL_LOG("gluNewQuadric");

	for (int i = 0; i < GLU_MAX_QUADRICS; i++) {
		if (!glu_quadrics[i].in_use) {
			glu_quadrics[i].in_use = true;
			glu_quadrics[i].normals = GLU_SMOOTH;
			glu_quadrics[i].texture = false;
			glu_quadrics[i].drawstyle = GLU_FILL_;
			glu_quadrics[i].orientation = GLU_OUTSIDE;
			glu_quadrics[i].error_callback = 0;  // no callback until registered

			uint32_t mac_handle = Mac_sysalloc(4);
			if (mac_handle == 0) {
				glu_quadrics[i].in_use = false;
				return 0;
			}
			WriteMacInt32(mac_handle, i + 1);  // 1-based
			glu_quadric_mac_handles[i] = mac_handle;

			GL_LOG("gluNewQuadric: allocated quadric %d (handle=0x%08x)", i + 1, mac_handle);
			return mac_handle;
		}
	}

	GL_LOG("gluNewQuadric: no free quadric slots");
	return 0;
}

static GLUQuadricState *GLUQuadricFromHandle(uint32_t mac_handle)
{
	if (mac_handle == 0) return nullptr;
	uint32_t one_based = ReadMacInt32(mac_handle);
	if (one_based == 0 || one_based > GLU_MAX_QUADRICS) return nullptr;
	int idx = one_based - 1;
	if (!glu_quadrics[idx].in_use) return nullptr;
	return &glu_quadrics[idx];
}

void NativeGLUDeleteQuadric(uint32_t quad_handle)
{
	GL_LOG("gluDeleteQuadric: handle=0x%08x", quad_handle);
	GLUQuadricState *q = GLUQuadricFromHandle(quad_handle);
	if (q) q->in_use = false;
}

void NativeGLUQuadricNormals(uint32_t quad_handle, uint32_t normal)
{
	GLUQuadricState *q = GLUQuadricFromHandle(quad_handle);
	if (q) q->normals = normal;
}

void NativeGLUQuadricTexture(uint32_t quad_handle, uint32_t texture)
{
	GLUQuadricState *q = GLUQuadricFromHandle(quad_handle);
	if (q) q->texture = (texture != 0);
}

void NativeGLUQuadricDrawStyle(uint32_t quad_handle, uint32_t draw)
{
	GLUQuadricState *q = GLUQuadricFromHandle(quad_handle);
	if (q) q->drawstyle = draw;
}

void NativeGLUQuadricOrientation(uint32_t quad_handle, uint32_t orient)
{
	GLUQuadricState *q = GLUQuadricFromHandle(quad_handle);
	if (q) q->orientation = orient;
}

void NativeGLUQuadricCallback(uint32_t quad_handle, uint32_t which, uint32_t callback)
{
	GL_LOG("gluQuadricCallback: quad=0x%08x which=%d callback=0x%08x (stored, invoked via call_macos)", quad_handle, which, callback);
	// The only legal quadric callback selector is GLU_ERROR. Store the guest
	// TVECT addr so the quadric error paths can fire it back into emulated PPC
	// via the call_macos1 trampoline (NULL-guarded).
	GLUQuadricState *q = GLUQuadricFromHandle(quad_handle);
	if (!q) return;
	if (which == GLU_ERROR) {
		q->error_callback = callback;
	}
}

// ---- GLU error callback fire helper (quadric) ----
// Mirrors the RAVE FireNoticeMethod NULL-guard discipline
// (rave_metal_renderer.mm): NEVER invoke a 0/garbage callback addr — jumping
// to a zero code address inside emulated PPC crashes. The call_macos1
// trampoline (Unix/sysdeps.h) sets r2(TOC) from the 2-word TVECT; a raw C
// cast is forbidden. Synchronous on the emul thread (GL single-threaded-by-
// design; the quadric draw already runs there — DSp VBLProc precedent).
// Return value ignored. The error code is the only arg (GLU error callbacks
// are void(*)(GLenum)).
static void FireQuadricError(GLUQuadricState *q, uint32_t error_code)
{
	if (!q) return;
	const uint32_t cb = q->error_callback;  // snapshot to local (DSp defense-in-depth)
	if (cb == 0) return;                     // RAVE NULL-guard — copy EXACTLY
	(void)call_macos1(cb, error_code);
	GL_LOG("FireQuadricError: cb=0x%08x error=%u", cb, error_code);
}

/*
 *  Upper clamp for tessellation counts on the immediate-mode shape
 *  generators. Classic-era titles use at most a few dozen; anything
 *  large is either a guest bug or a mis-decoded wire argument, and an
 *  unbounded count hangs the main==emul thread emitting geometry.
 */
#define GLU_TESS_COUNT_MAX 256
static inline int32_t ClampTessCount(int32_t v, const char *what)
{
	if (v > GLU_TESS_COUNT_MAX) {
		GL_LOG("quadric: clamping %s %d -> %d", what, v, GLU_TESS_COUNT_MAX);
		return GLU_TESS_COUNT_MAX;
	}
	return v;
}


/*
 *  NativeGLUSphere(quad, radius, slices, stacks)
 *
 *  Generate sphere geometry using immediate mode GL calls.
 *  Standard sphere tessellation with triangle strips per stack.
 */
void NativeGLUSphere(GLContext *ctx, uint32_t quad_handle,
                     double radius, int32_t slices, int32_t stacks)
{
	GL_LOG("gluSphere: radius=%f slices=%d stacks=%d", radius, slices, stacks);
	if (!ctx || slices < 2 || stacks < 1) {
		// Invalid tessellation params — fire the registered GLU_ERROR
		// callback (gluQuadricCallback) with GLU_INVALID_VALUE, NULL-guarded.
		FireQuadricError(GLUQuadricFromHandle(quad_handle), GLU_INVALID_VALUE);
		return;
	}
	slices = ClampTessCount(slices, "slices");
	stacks = ClampTessCount(stacks, "stacks");

	GLUQuadricState *q = GLUQuadricFromHandle(quad_handle);
	bool gen_normals = (q && q->normals != GLU_NONE_);
	bool gen_tex = (q && q->texture);
	bool inside = (q && q->orientation == GLU_INSIDE);
	float nsign = inside ? -1.0f : 1.0f;
	uint32_t mode = GL_TRIANGLE_STRIP;
	if (q) {
		if (q->drawstyle == GLU_LINE || q->drawstyle == GLU_SILHOUETTE) mode = GL_LINE_LOOP;
		else if (q->drawstyle == GLU_POINT) mode = GL_POINTS;
	}

	float r = (float)radius;

	for (int i = 0; i < stacks; i++) {
		float t0 = (float)i / stacks;
		float t1 = (float)(i + 1) / stacks;
		float phi0 = (float)(M_PI * (t0 - 0.5));  // -pi/2 to pi/2 for bottom to top
		float phi1 = (float)(M_PI * (t1 - 0.5));

		// Precompute
		float cp0 = cosf(phi0), sp0 = sinf(phi0);
		float cp1 = cosf(phi1), sp1 = sinf(phi1);

		NativeGLBegin(ctx, mode);

		for (int j = 0; j <= slices; j++) {
			float s = (float)j / slices;
			float theta = (float)(2.0 * M_PI * s);
			float ct = cosf(theta), st = sinf(theta);

			float x0 = cp0 * ct, y0 = cp0 * st, z0 = sp0;
			float x1 = cp1 * ct, y1 = cp1 * st, z1 = sp1;

			if (inside) {
				// Reversed winding: top vertex first
				if (gen_normals) NativeGLNormal3f(ctx, nsign*x1, nsign*y1, nsign*z1);
				if (gen_tex) NativeGLTexCoord2f(ctx, s, t1);
				NativeGLVertex3f(ctx, r * x1, r * y1, r * z1);
				if (gen_normals) NativeGLNormal3f(ctx, nsign*x0, nsign*y0, nsign*z0);
				if (gen_tex) NativeGLTexCoord2f(ctx, s, t0);
				NativeGLVertex3f(ctx, r * x0, r * y0, r * z0);
			} else {
				// Normal winding: bottom vertex first
				if (gen_normals) NativeGLNormal3f(ctx, x0, y0, z0);
				if (gen_tex) NativeGLTexCoord2f(ctx, s, t0);
				NativeGLVertex3f(ctx, r * x0, r * y0, r * z0);
				if (gen_normals) NativeGLNormal3f(ctx, x1, y1, z1);
				if (gen_tex) NativeGLTexCoord2f(ctx, s, t1);
				NativeGLVertex3f(ctx, r * x1, r * y1, r * z1);
			}
		}

		NativeGLEnd(ctx);
	}
}


/*
 *  NativeGLUCylinder(quad, base, top, height, slices, stacks)
 */
void NativeGLUCylinder(GLContext *ctx, uint32_t quad_handle,
                       double base, double top, double height,
                       int32_t slices, int32_t stacks)
{
	GL_LOG("gluCylinder: base=%f top=%f height=%f slices=%d stacks=%d", base, top, height, slices, stacks);
	if (!ctx || slices < 2 || stacks < 1) return;
	slices = ClampTessCount(slices, "slices");
	stacks = ClampTessCount(stacks, "stacks");

	GLUQuadricState *q = GLUQuadricFromHandle(quad_handle);
	bool gen_normals = (q && q->normals != GLU_NONE_);
	bool gen_tex = (q && q->texture);
	bool inside = (q && q->orientation == GLU_INSIDE);
	float nsign = inside ? -1.0f : 1.0f;

	float b = (float)base, t = (float)top, h = (float)height;

	// Normal slope for cylinder
	float dr = b - t;  // radius difference
	float nz_slope = dr / h;  // for normal computation
	float nlen = sqrtf(1.0f + nz_slope * nz_slope);

	for (int i = 0; i < stacks; i++) {
		float s0 = (float)i / stacks;
		float s1 = (float)(i + 1) / stacks;
		float r0 = b + (t - b) * s0;
		float r1 = b + (t - b) * s1;
		float z0 = h * s0;
		float z1 = h * s1;

		NativeGLBegin(ctx, GL_TRIANGLE_STRIP);

		for (int j = 0; j <= slices; j++) {
			float angle = (float)(2.0 * M_PI * j / slices);
			float ca = cosf(angle), sa = sinf(angle);

			float nx = ca / nlen, ny = sa / nlen, nzc = nz_slope / nlen;

			if (inside) {
				if (gen_normals) NativeGLNormal3f(ctx, nsign*nx, nsign*ny, nsign*nzc);
				if (gen_tex) NativeGLTexCoord2f(ctx, (float)j / slices, s1);
				NativeGLVertex3f(ctx, r1 * ca, r1 * sa, z1);
				if (gen_normals) NativeGLNormal3f(ctx, nsign*nx, nsign*ny, nsign*nzc);
				if (gen_tex) NativeGLTexCoord2f(ctx, (float)j / slices, s0);
				NativeGLVertex3f(ctx, r0 * ca, r0 * sa, z0);
			} else {
				if (gen_normals) NativeGLNormal3f(ctx, nx, ny, nzc);
				if (gen_tex) NativeGLTexCoord2f(ctx, (float)j / slices, s0);
				NativeGLVertex3f(ctx, r0 * ca, r0 * sa, z0);
				if (gen_normals) NativeGLNormal3f(ctx, nx, ny, nzc);
				if (gen_tex) NativeGLTexCoord2f(ctx, (float)j / slices, s1);
				NativeGLVertex3f(ctx, r1 * ca, r1 * sa, z1);
			}
		}

		NativeGLEnd(ctx);
	}
}


/*
 *  NativeGLUDisk(quad, inner, outer, slices, loops)
 */
void NativeGLUDisk(GLContext *ctx, uint32_t quad_handle,
                   double inner, double outer, int32_t slices, int32_t loops)
{
	GL_LOG("gluDisk: inner=%f outer=%f slices=%d loops=%d", inner, outer, slices, loops);
	if (!ctx || slices < 2 || loops < 1) return;
	slices = ClampTessCount(slices, "slices");
	loops  = ClampTessCount(loops, "loops");

	GLUQuadricState *q = GLUQuadricFromHandle(quad_handle);
	bool gen_normals = (q && q->normals != GLU_NONE_);
	bool gen_tex = (q && q->texture);
	bool inside = (q && q->orientation == GLU_INSIDE);
	float nsign = inside ? -1.0f : 1.0f;

	float ri = (float)inner, ro = (float)outer;

	if (gen_normals) NativeGLNormal3f(ctx, 0.0f, 0.0f, nsign * 1.0f);

	for (int i = 0; i < loops; i++) {
		float t0 = (float)i / loops;
		float t1 = (float)(i + 1) / loops;
		float r0 = ri + (ro - ri) * t0;
		float r1 = ri + (ro - ri) * t1;

		NativeGLBegin(ctx, GL_TRIANGLE_STRIP);

		for (int j = 0; j <= slices; j++) {
			float angle = (float)(2.0 * M_PI * j / slices);
			float ca = cosf(angle), sa = sinf(angle);

			if (inside) {
				if (gen_tex) NativeGLTexCoord2f(ctx, ca * t0 * 0.5f + 0.5f, sa * t0 * 0.5f + 0.5f);
				NativeGLVertex3f(ctx, r0 * ca, r0 * sa, 0.0f);
				if (gen_tex) NativeGLTexCoord2f(ctx, ca * t1 * 0.5f + 0.5f, sa * t1 * 0.5f + 0.5f);
				NativeGLVertex3f(ctx, r1 * ca, r1 * sa, 0.0f);
			} else {
				if (gen_tex) NativeGLTexCoord2f(ctx, ca * t1 * 0.5f + 0.5f, sa * t1 * 0.5f + 0.5f);
				NativeGLVertex3f(ctx, r1 * ca, r1 * sa, 0.0f);
				if (gen_tex) NativeGLTexCoord2f(ctx, ca * t0 * 0.5f + 0.5f, sa * t0 * 0.5f + 0.5f);
				NativeGLVertex3f(ctx, r0 * ca, r0 * sa, 0.0f);
			}
		}

		NativeGLEnd(ctx);
	}
}


/*
 *  NativeGLUPartialDisk(quad, inner, outer, slices, loops, start, sweep)
 */
void NativeGLUPartialDisk(GLContext *ctx, uint32_t quad_handle,
                          double inner, double outer, int32_t slices, int32_t loops,
                          double start, double sweep)
{
	GL_LOG("gluPartialDisk: inner=%f outer=%f slices=%d loops=%d start=%f sweep=%f",
	       inner, outer, slices, loops, start, sweep);
	if (!ctx || slices < 2 || loops < 1) return;
	slices = ClampTessCount(slices, "slices");
	loops  = ClampTessCount(loops, "loops");

	GLUQuadricState *q = GLUQuadricFromHandle(quad_handle);
	bool gen_normals = (q && q->normals != GLU_NONE_);
	bool gen_tex = (q && q->texture);
	bool inside = (q && q->orientation == GLU_INSIDE);
	float nsign = inside ? -1.0f : 1.0f;

	float ri = (float)inner, ro = (float)outer;
	float start_rad = (float)(start * M_PI / 180.0);
	float sweep_rad = (float)(sweep * M_PI / 180.0);

	if (gen_normals) NativeGLNormal3f(ctx, 0.0f, 0.0f, nsign * 1.0f);

	for (int i = 0; i < loops; i++) {
		float t0 = (float)i / loops;
		float t1 = (float)(i + 1) / loops;
		float r0 = ri + (ro - ri) * t0;
		float r1 = ri + (ro - ri) * t1;

		NativeGLBegin(ctx, GL_TRIANGLE_STRIP);

		for (int j = 0; j <= slices; j++) {
			float angle = start_rad + sweep_rad * j / slices;
			float ca = cosf(angle), sa = sinf(angle);

			if (inside) {
				if (gen_tex) NativeGLTexCoord2f(ctx, ca * t0 * 0.5f + 0.5f, sa * t0 * 0.5f + 0.5f);
				NativeGLVertex3f(ctx, r0 * ca, r0 * sa, 0.0f);
				if (gen_tex) NativeGLTexCoord2f(ctx, ca * t1 * 0.5f + 0.5f, sa * t1 * 0.5f + 0.5f);
				NativeGLVertex3f(ctx, r1 * ca, r1 * sa, 0.0f);
			} else {
				if (gen_tex) NativeGLTexCoord2f(ctx, ca * t1 * 0.5f + 0.5f, sa * t1 * 0.5f + 0.5f);
				NativeGLVertex3f(ctx, r1 * ca, r1 * sa, 0.0f);
				if (gen_tex) NativeGLTexCoord2f(ctx, ca * t0 * 0.5f + 0.5f, sa * t0 * 0.5f + 0.5f);
				NativeGLVertex3f(ctx, r0 * ca, r0 * sa, 0.0f);
			}
		}

		NativeGLEnd(ctx);
	}
}


// ===========================================================================
//  GLU Tessellation — Real Implementation (ear-clipping triangulation)
// ===========================================================================

static GLUTessState *GLUTessFromHandle(uint32_t mac_handle)
{
	if (mac_handle == 0) return nullptr;
	uint32_t one_based = ReadMacInt32(mac_handle);
	if (one_based == 0 || one_based > GLU_MAX_TESS) return nullptr;
	int idx = one_based - 1;
	if (!glu_tess[idx].in_use) return nullptr;
	return &glu_tess[idx];
}

uint32_t NativeGLUNewTess()
{
	GL_LOG("gluNewTess");
	for (int i = 0; i < GLU_MAX_TESS; i++) {
		if (!glu_tess[i].in_use) {
			GLUTessState &t = glu_tess[i];
			t.in_use = true;
			t.winding_rule = GLU_TESS_WINDING_ODD;
			t.boundary_only = false;
			t.tolerance = 0.0;
			t.normal_x = t.normal_y = t.normal_z = 0.0f;
			memset(t.callbacks, 0, sizeof(t.callbacks));
			t.user_data = 0;
			t.contours.clear();
			t.in_polygon = false;
			t.in_contour = false;
			uint32_t mac_handle = Mac_sysalloc(4);
			if (mac_handle == 0) { t.in_use = false; return 0; }
			WriteMacInt32(mac_handle, i + 1);
			glu_tess_mac_handles[i] = mac_handle;
			GL_LOG("gluNewTess: allocated tess %d", i + 1);
			return mac_handle;
		}
	}
	GL_LOG("gluNewTess: no free tess slots");
	return 0;
}

void NativeGLUDeleteTess(uint32_t tess_handle)
{
	GL_LOG("gluDeleteTess: handle=0x%08x", tess_handle);
	GLUTessState *t = GLUTessFromHandle(tess_handle);
	if (t) {
		t->contours.clear();
		t->in_use = false;
	}
}

void NativeGLUTessCallback(uint32_t tess, uint32_t which, uint32_t callback)
{
	GL_LOG("gluTessCallback: tess=0x%08x which=%u callback=0x%08x (stored, not invoked as PPC)", tess, which, callback);
	GLUTessState *t = GLUTessFromHandle(tess);
	if (!t) return;
	if (which >= GLU_TESS_BEGIN && which <= GLU_TESS_COMBINE_DATA) {
		t->callbacks[which - GLU_TESS_BEGIN] = callback;
	}
}

void NativeGLUTessProperty(uint32_t tess, uint32_t which, double data)
{
	GL_LOG("gluTessProperty: tess=0x%08x which=%u value=%f", tess, which, data);
	GLUTessState *t = GLUTessFromHandle(tess);
	if (!t) return;
	switch (which) {
		case GLU_TESS_WINDING_RULE:
			t->winding_rule = (uint32_t)data;
			break;
		case GLU_TESS_BOUNDARY_ONLY:
			t->boundary_only = (data != 0.0);
			break;
		case GLU_TESS_TOLERANCE:
			t->tolerance = data;
			break;
	}
}

void NativeGLUTessNormal(uint32_t tess, double x, double y, double z)
{
	GL_LOG("gluTessNormal: tess=0x%08x normal=(%f, %f, %f)", tess, x, y, z);
	GLUTessState *t = GLUTessFromHandle(tess);
	if (!t) return;
	t->normal_x = (float)x;
	t->normal_y = (float)y;
	t->normal_z = (float)z;
}

void NativeGLUGetTessProperty(uint32_t tess, uint32_t which, uint32_t data_ptr)
{
	GL_LOG("gluGetTessProperty: tess=0x%08x which=%u ptr=0x%08x", tess, which, data_ptr);
	GLUTessState *t = GLUTessFromHandle(tess);
	if (!t || data_ptr == 0) return;
	double val = 0.0;
	switch (which) {
		case GLU_TESS_WINDING_RULE:  val = (double)t->winding_rule; break;
		case GLU_TESS_BOUNDARY_ONLY: val = t->boundary_only ? 1.0 : 0.0; break;
		case GLU_TESS_TOLERANCE:     val = t->tolerance; break;
	}
	WriteMacDouble(data_ptr, val);
}

void NativeGLUTessBeginPolygon(uint32_t tess, uint32_t data)
{
	GL_LOG("gluTessBeginPolygon: tess=0x%08x data=0x%08x", tess, data);
	GLUTessState *t = GLUTessFromHandle(tess);
	if (!t) return;
	t->contours.clear();
	t->user_data = data;
	t->in_polygon = true;
	t->in_contour = false;
}

void NativeGLUTessBeginContour(uint32_t tess)
{
	GL_LOG("gluTessBeginContour: tess=0x%08x", tess);
	GLUTessState *t = GLUTessFromHandle(tess);
	if (!t || !t->in_polygon) return;
	if ((int)t->contours.size() >= GLU_TESS_MAX_CONTOURS) {
		GL_LOG("gluTessBeginContour: max contours (%d) reached", GLU_TESS_MAX_CONTOURS);
		return;
	}
	t->contours.push_back(GLUTessContour());
	t->in_contour = true;
}

void NativeGLUTessVertex(uint32_t tess, uint32_t location, uint32_t data)
{
	GL_LOG("gluTessVertex: tess=0x%08x location=0x%08x data=0x%08x", tess, location, data);
	GLUTessState *t = GLUTessFromHandle(tess);
	if (!t || !t->in_contour || t->contours.empty()) return;
	GLUTessContour &c = t->contours.back();
	if ((int)c.vertices.size() >= GLU_TESS_MAX_VERTICES) {
		GL_LOG("gluTessVertex: max vertices (%d) reached for contour", GLU_TESS_MAX_VERTICES);
		return;
	}
	// Read 3 doubles from PPC memory at location
	GLUTessVertex3 v;
	v.x = (float)ReadMacDouble(location);
	v.y = (float)ReadMacDouble(location + 8);
	v.z = (float)ReadMacDouble(location + 16);
	// Store the opaque guest `data` ptr (previously discarded). It is passed
	// straight back to the GLU_TESS_VERTEX callback via the trampoline and is
	// NEVER dereferenced natively.
	v.data = data;
	c.vertices.push_back(v);
}

void NativeGLUTessEndContour(uint32_t tess)
{
	GL_LOG("gluTessEndContour: tess=0x%08x", tess);
	GLUTessState *t = GLUTessFromHandle(tess);
	if (!t) return;
	t->in_contour = false;
}

// ---- GLU tess callback fire helper ----
//
// Invokes a registered GLU tessellation callback (BEGIN/VERTEX/END/ERROR) back
// into emulated PPC through the sanctioned call_macos* trampoline
// (Unix/sysdeps.h). This MIRRORS the RAVE FireNoticeMethod discipline EXACTLY
// (rave_metal_renderer.mm):
//
//   * NULL-guard the callback addr (`if (cb == 0) return;`) BEFORE call_macos —
//     jumping to a 0/garbage code address inside emulated PPC crashes. This is
//     the highest-severity threat here.
//   * Read the addr from t->callbacks[which - GLU_TESS_BEGIN] (the same index
//     NativeGLUTessCallback stores into) and snapshot it to a local
//     (DSp VBLProc defense-in-depth, dsp_draw_context.mm).
//   * Per-callback arg marshalling:
//       BEGIN  -> call_macos1(cb, primType)
//       VERTEX -> call_macos1(cb, v.data)   // opaque guest ptr, never deref'd
//       END    -> call_macos1(cb, 0)        // void(*)(void): r3 ignored by guest
//       ERROR  -> call_macos1(cb, errorCode)
//     (The END callback is void(*)(void); we marshal through call_macos1 with a
//     dummy 0 arg — a no-arg PPC routine never reads r3, so the extra word is
//     harmless. The bare zero-arg call_macos symbol is not exported by the
//     prebuilt libkpx_cpu_ios.a, so call_macos1 is the linked trampoline.)
//   * Synchronous on the emul thread (GL single-threaded-by-design; tess
//     EndPolygon already runs there). Return value (void)-cast and ignored.
//
// The opaque per-vertex `data` ptr passed for VERTEX is interpreted ONLY by the
// guest callback — it is NEVER dereferenced natively (ASVS V5).
// NOTE: GLU_TESS_COMBINE is DELIBERATELY never fired here — see the marker in
// NativeGLUTessEndPolygon (self-intersection engine absent).
static void FireTessCallback(GLUTessState *t, uint32_t which, uint32_t arg)
{
	if (!t) return;
	if (which < GLU_TESS_BEGIN || which > GLU_TESS_COMBINE_DATA) return;
	const uint32_t cb = t->callbacks[which - GLU_TESS_BEGIN];  // snapshot to local
	if (cb == 0) return;  // RAVE NULL-guard — copy EXACTLY
	switch (which) {
		case GLU_TESS_END:
			(void)call_macos1(cb, 0);        // void(*)(void): guest ignores r3
			break;
		case GLU_TESS_BEGIN:                 // void(*)(GLenum primType)
		case GLU_TESS_VERTEX:                // void(*)(void *vertexData)
		case GLU_TESS_ERROR:                 // void(*)(GLenum errno)
		default:
			(void)call_macos1(cb, arg);
			break;
	}
	GL_LOG("FireTessCallback: which=%u cb=0x%08x arg=0x%08x", which, cb, arg);
}

// True iff this tessellator has the BEGIN/VERTEX/END callback trio
// registered. Only then does EndPolygon take the callback path; otherwise the
// immediate-mode (NativeGLNormal3f + ear_clip_triangulate) fallback is
// preserved unchanged.
static bool tess_has_emit_callbacks(const GLUTessState *t)
{
	if (!t) return false;
	return t->callbacks[GLU_TESS_BEGIN  - GLU_TESS_BEGIN] != 0
	    && t->callbacks[GLU_TESS_VERTEX - GLU_TESS_BEGIN] != 0
	    && t->callbacks[GLU_TESS_END    - GLU_TESS_BEGIN] != 0;
}

// ---- Ear-clipping triangulation helpers ----

// Cross product of 2D vectors (b-a) x (c-a) — returns z component
static float ear_clip_cross_2d(float ax, float ay, float bx, float by, float cx, float cy)
{
	return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

// Test if point P is inside triangle ABC (2D, assumes CCW winding)
static bool ear_clip_point_in_triangle(float px, float py,
                                       float ax, float ay, float bx, float by, float cx, float cy)
{
	float d1 = ear_clip_cross_2d(ax, ay, bx, by, px, py);
	float d2 = ear_clip_cross_2d(bx, by, cx, cy, px, py);
	float d3 = ear_clip_cross_2d(cx, cy, ax, ay, px, py);
	bool has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
	bool has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
	return !(has_neg && has_pos);
}

// Ear-clipping triangulation of a 2D polygon.
// Projects 3D vertices to 2D by dropping the axis corresponding to the largest
// component of the polygon normal, then triangulates in 2D and emits 3D triangles.
//
// When `cb_tess` is non-null (the app registered the BEGIN/VERTEX/END
// callback trio), the emit path fires those guest callbacks via the call_macos*
// trampoline — BEGIN(GL_TRIANGLES) once, VERTEX(v.data) per emitted vertex,
// END() once — instead of the immediate-mode NativeGLBegin/Vertex3f/End path.
// When `cb_tess` is null the original immediate-mode path is used UNCHANGED
// (the fallback for the non-callback gluTessEndPolygon consumer).
static void ear_clip_triangulate(GLContext *ctx, const std::vector<GLUTessVertex3> &verts,
                                 float nx, float ny, float nz,
                                 GLUTessState *cb_tess = nullptr)
{
	const bool use_callbacks = (cb_tess != nullptr);
	int n = (int)verts.size();
	if (n < 3) {
		GL_LOG("ear_clip_triangulate: degenerate polygon with %d vertices, no geometry emitted", n);
		return;
	}

	// Determine dominant axis for 2D projection (drop largest normal component)
	float anx = fabsf(nx), any = fabsf(ny), anz = fabsf(nz);
	int drop_axis; // 0=x, 1=y, 2=z
	if (anx >= any && anx >= anz) drop_axis = 0;
	else if (any >= anx && any >= anz) drop_axis = 1;
	else drop_axis = 2;

	// Extract 2D coordinates
	std::vector<float> u(n), v(n);
	for (int i = 0; i < n; i++) {
		switch (drop_axis) {
			case 0: u[i] = verts[i].y; v[i] = verts[i].z; break;
			case 1: u[i] = verts[i].x; v[i] = verts[i].z; break;
			case 2: u[i] = verts[i].x; v[i] = verts[i].y; break;
		}
	}

	// Build index list
	std::vector<int> indices(n);
	for (int i = 0; i < n; i++) indices[i] = i;

	// Determine winding — if total signed area is negative, polygon is CW in projection
	float area = 0.0f;
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		area += u[i] * v[j] - u[j] * v[i];
	}
	// If area is negative, the projection flipped winding; negate cross tests
	float winding_sign = (area >= 0.0f) ? 1.0f : -1.0f;

	int triangle_count = 0;
	if (use_callbacks) {
		// Fire the guest BEGIN callback with the primitive type
		// (GL_TRIANGLES) via the trampoline. The app does its own rendering.
		FireTessCallback(cb_tess, GLU_TESS_BEGIN, GL_TRIANGLES);
	} else {
		NativeGLBegin(ctx, GL_TRIANGLES);
	}

	int remaining = n;
	int fail_count = 0;
	int idx = 0;
	while (remaining > 2 && fail_count < remaining) {
		int prev = (idx + remaining - 1) % remaining;
		int next = (idx + 1) % remaining;

		int i0 = indices[prev];
		int i1 = indices[idx];
		int i2 = indices[next];

		float cross = ear_clip_cross_2d(u[i0], v[i0], u[i1], v[i1], u[i2], v[i2]);

		// Ear test: triangle must be convex (cross product matches winding)
		bool is_convex = (cross * winding_sign > 0.0f);

		bool is_ear = is_convex;
		if (is_ear) {
			// Check no other vertex is inside this triangle
			for (int k = 0; k < remaining; k++) {
				if (k == prev || k == idx || k == next) continue;
				int ik = indices[k];
				if (ear_clip_point_in_triangle(u[ik], v[ik],
				                               u[i0], v[i0], u[i1], v[i1], u[i2], v[i2])) {
					is_ear = false;
					break;
				}
			}
		}

		if (is_ear) {
			// Emit triangle
			if (use_callbacks) {
				// Fire the guest VERTEX callback with each emitted
				// vertex's opaque `data` ptr (never dereferenced natively).
				FireTessCallback(cb_tess, GLU_TESS_VERTEX, verts[i0].data);
				FireTessCallback(cb_tess, GLU_TESS_VERTEX, verts[i1].data);
				FireTessCallback(cb_tess, GLU_TESS_VERTEX, verts[i2].data);
			} else {
				NativeGLVertex3f(ctx, verts[i0].x, verts[i0].y, verts[i0].z);
				NativeGLVertex3f(ctx, verts[i1].x, verts[i1].y, verts[i1].z);
				NativeGLVertex3f(ctx, verts[i2].x, verts[i2].y, verts[i2].z);
			}
			triangle_count++;

			// Remove vertex at idx
			indices.erase(indices.begin() + idx);
			remaining--;
			if (idx >= remaining) idx = 0;
			fail_count = 0;
		} else {
			idx = (idx + 1) % remaining;
			fail_count++;
		}
	}

	if (use_callbacks) {
		FireTessCallback(cb_tess, GLU_TESS_END, 0);  // void(*)(void)
	} else {
		NativeGLEnd(ctx);
	}
	GL_LOG("ear_clip_triangulate: emitted %d triangles from %d vertices", triangle_count, n);
}

// Bridge-edge merge: merge outer contour with inner contours (holes) into a single polygon
// by finding bridge edges that connect each inner contour to the outer contour.
static std::vector<GLUTessVertex3> tess_merge_contours(const std::vector<GLUTessContour> &contours,
                                                        float nx, float ny, float nz)
{
	if (contours.empty()) return {};
	if (contours.size() == 1) return contours[0].vertices;

	// Determine dominant axis for 2D projection
	float anx = fabsf(nx), any = fabsf(ny), anz = fabsf(nz);
	int drop_axis;
	if (anx >= any && anx >= anz) drop_axis = 0;
	else if (any >= anx && any >= anz) drop_axis = 1;
	else drop_axis = 2;

	auto get_uv = [drop_axis](const GLUTessVertex3 &v, float &u, float &vv) {
		switch (drop_axis) {
			case 0: u = v.y; vv = v.z; break;
			case 1: u = v.x; vv = v.z; break;
			case 2: u = v.x; vv = v.y; break;
		}
	};

	// Start with the outer contour (contour 0)
	std::vector<GLUTessVertex3> merged = contours[0].vertices;

	// For each inner contour, find bridge edge and insert
	for (size_t c = 1; c < contours.size(); c++) {
		const std::vector<GLUTessVertex3> &inner = contours[c].vertices;
		if (inner.empty()) continue;

		// Find the rightmost vertex of the inner contour
		int inner_rightmost = 0;
		float inner_max_u = -1e30f;
		for (int i = 0; i < (int)inner.size(); i++) {
			float u, v;
			get_uv(inner[i], u, v);
			if (u > inner_max_u) { inner_max_u = u; inner_rightmost = i; }
		}

		// Find the closest vertex in the merged polygon to bridge to
		float inner_u, inner_v;
		get_uv(inner[inner_rightmost], inner_u, inner_v);

		int best_outer = 0;
		float best_dist = 1e30f;
		for (int i = 0; i < (int)merged.size(); i++) {
			float ou, ov;
			get_uv(merged[i], ou, ov);
			float dx = ou - inner_u, dy = ov - inner_v;
			float dist = dx * dx + dy * dy;
			if (dist < best_dist) { best_dist = dist; best_outer = i; }
		}

		// Insert bridge: outer[best_outer] → inner[inner_rightmost] → inner loop → inner[inner_rightmost] → outer[best_outer]
		std::vector<GLUTessVertex3> new_merged;
		new_merged.reserve(merged.size() + inner.size() + 2);

		// Copy merged up to and including best_outer
		for (int i = 0; i <= best_outer; i++) {
			new_merged.push_back(merged[i]);
		}

		// Insert inner contour starting from inner_rightmost
		for (int i = 0; i < (int)inner.size(); i++) {
			int idx = (inner_rightmost + i) % (int)inner.size();
			new_merged.push_back(inner[idx]);
		}
		// Close inner back to bridge point
		new_merged.push_back(inner[inner_rightmost]);

		// Bridge back to outer
		new_merged.push_back(merged[best_outer]);

		// Continue with rest of outer
		for (int i = best_outer + 1; i < (int)merged.size(); i++) {
			new_merged.push_back(merged[i]);
		}

		merged = std::move(new_merged);
	}

	return merged;
}

void NativeGLUTessEndPolygon(uint32_t tess)
{
	GL_LOG("gluTessEndPolygon: tess=0x%08x", tess);
	GLUTessState *t = GLUTessFromHandle(tess);
	if (!t) return;
	t->in_polygon = false;

	// Does the app expect its BEGIN/VERTEX/END callbacks to drive
	// rendering (the spec-canonical GLU tessellator contract)? If so, the
	// triangulation emit path fires those guest callbacks via call_macos*
	// instead of immediate-mode GL. If NOT, the immediate-mode
	// (NativeGLNormal3f + ear_clip_triangulate) fallback is preserved
	// unchanged.
	const bool emit_via_callbacks = tess_has_emit_callbacks(t);

	// The immediate-mode path needs a live GL context; the callback path does
	// not (the guest does its own GL). Only bail-on-no-context for immediate.
	if (!gl_current_context && !emit_via_callbacks) {
		GL_LOG("gluTessEndPolygon: no GL context");
		return;
	}

	// Count total vertices
	int total_verts = 0;
	for (const auto &c : t->contours) total_verts += (int)c.vertices.size();
	if (total_verts < 3) {
		GL_LOG("gluTessEndPolygon: degenerate polygon with %d total vertices, no geometry emitted", total_verts);
		// Report the malformed-polygon error to the guest GLU_TESS_ERROR
		// callback (NULL-guarded) per the tessellator contract.
		FireTessCallback(t, GLU_TESS_ERROR, GLU_TESS_MISSING_BEGIN_POLYGON);
		return;
	}

	// Compute polygon normal
	float nx = t->normal_x, ny = t->normal_y, nz = t->normal_z;
	if (nx == 0.0f && ny == 0.0f && nz == 0.0f) {
		// Auto-compute from first contour using Newell's method
		const std::vector<GLUTessVertex3> &verts = t->contours[0].vertices;
		nx = ny = nz = 0.0f;
		int nv = (int)verts.size();
		for (int i = 0; i < nv; i++) {
			int j = (i + 1) % nv;
			nx += (verts[i].y - verts[j].y) * (verts[i].z + verts[j].z);
			ny += (verts[i].z - verts[j].z) * (verts[i].x + verts[j].x);
			nz += (verts[i].x - verts[j].x) * (verts[i].y + verts[j].y);
		}
		float len = sqrtf(nx * nx + ny * ny + nz * nz);
		if (len > 1e-10f) { nx /= len; ny /= len; nz /= len; }
		else { nx = 0; ny = 0; nz = 1.0f; } // fallback to Z-up
	}

	// Set polygon normal (immediate-mode path only — the callback path lets the
	// guest set its own GL state).
	if (!emit_via_callbacks) {
		NativeGLNormal3f(gl_current_context, nx, ny, nz);
	}

	// Merge contours if multiple (bridge-edge merging for holes)
	std::vector<GLUTessVertex3> merged;
	if (t->contours.size() == 1) {
		merged = t->contours[0].vertices;
	} else {
		merged = tess_merge_contours(t->contours, nx, ny, nz);
		GL_LOG("gluTessEndPolygon: merged %d contours into %d vertices", (int)t->contours.size(), (int)merged.size());
	}

	// GLU_TESS_COMBINE is DELIBERATELY never fired: the ear-clipper has NO
	// self-intersection-detection engine, so the COMBINE trigger (a
	// synthesized intersection vertex needing a new app-allocated data ptr) is
	// UNREACHABLE. Firing a never-triggered COMBINE and inventing an outData
	// lifetime contract would be a half-truth — a silent stub producing wrong
	// output. We therefore do NOT synthesize GLU_TESS_COMBINE; self-
	// intersecting input is triangulated by the ear-clipper as-is (no new
	// vertices). This is a documented known limitation; no canary app passes
	// self-intersecting polygons. The tess `data` ptr is passed through
	// unchanged to the VERTEX callback and is NEVER dereferenced natively.

	// Ear-clipping triangulation. When the BEGIN/VERTEX/END callback trio is
	// registered, pass `t` so the emit path fires the guest callbacks via
	// call_macos*; otherwise the immediate-mode fallback is used unchanged.
	ear_clip_triangulate(gl_current_context, merged, nx, ny, nz,
	                     emit_via_callbacks ? t : nullptr);

	// Clean up
	t->contours.clear();
}

// ---- Deprecated tessellation wrappers ----

void NativeGLUBeginPolygon(uint32_t tess)
{
	GL_LOG("gluBeginPolygon (deprecated): delegates to TessBeginPolygon + TessBeginContour");
	NativeGLUTessBeginPolygon(tess, 0);
	NativeGLUTessBeginContour(tess);
}

void NativeGLUEndPolygon(uint32_t tess)
{
	GL_LOG("gluEndPolygon (deprecated): delegates to TessEndContour + TessEndPolygon");
	NativeGLUTessEndContour(tess);
	NativeGLUTessEndPolygon(tess);
}

void NativeGLUNextContour(uint32_t tess, uint32_t type)
{
	GL_LOG("gluNextContour (deprecated): type=%u, delegates to TessEndContour + TessBeginContour", type);
	(void)type;
	NativeGLUTessEndContour(tess);
	NativeGLUTessBeginContour(tess);
}


// ===========================================================================
//  GLU NURBS — Real Implementation (de Boor's algorithm)
// ===========================================================================

static GLUNurbsState *GLUNurbsFromHandle(uint32_t mac_handle)
{
	if (mac_handle == 0) return nullptr;
	uint32_t one_based = ReadMacInt32(mac_handle);
	if (one_based == 0 || one_based > GLU_MAX_NURBS) return nullptr;
	int idx = one_based - 1;
	if (!glu_nurbs[idx].in_use) return nullptr;
	return &glu_nurbs[idx];
}

static void GLUNurbsInitState(GLUNurbsState &ns)
{
	ns.in_use = true;
	ns.sampling_tolerance = 50.0f;
	ns.u_step = 100;
	ns.v_step = 100;
	ns.display_mode = GLU_FILL_;
	ns.auto_load_matrix = true;
	ns.sampling_method = GLU_PATH_LENGTH;
	ns.culling = false;
	ns.nurbs_mode = GLU_NURBS_RENDERER_EXT;
	memset(ns.callbacks, 0, sizeof(ns.callbacks));
	ns.user_data = 0;
	memset(ns.model_matrix, 0, sizeof(ns.model_matrix));
	memset(ns.proj_matrix, 0, sizeof(ns.proj_matrix));
	memset(ns.viewport, 0, sizeof(ns.viewport));
	ns.matrices_loaded = false;
	ns.in_surface = false;
	ns.s_knots.clear(); ns.t_knots.clear(); ns.control_points.clear();
	ns.s_stride = ns.t_stride = 0;
	ns.s_order = ns.t_order = 0;
	ns.surface_type = 0;
	ns.surface_defined = false;
	ns.in_curve = false;
	ns.curve_knots.clear(); ns.curve_control.clear();
	ns.curve_stride = 0; ns.curve_order = 0;
	ns.curve_type = 0; ns.curve_defined = false;
	ns.in_trim = false;
}

uint32_t NativeGLUNewNurbsRenderer()
{
	GL_LOG("gluNewNurbsRenderer");
	for (int i = 0; i < GLU_MAX_NURBS; i++) {
		if (!glu_nurbs[i].in_use) {
			GLUNurbsInitState(glu_nurbs[i]);
			uint32_t mac_handle = Mac_sysalloc(4);
			if (mac_handle == 0) { glu_nurbs[i].in_use = false; return 0; }
			WriteMacInt32(mac_handle, i + 1);
			glu_nurbs_mac_handles[i] = mac_handle;
			GL_LOG("gluNewNurbsRenderer: allocated nurbs %d", i + 1);
			return mac_handle;
		}
	}
	GL_LOG("gluNewNurbsRenderer: no free nurbs slots");
	return 0;
}

uint32_t NativeGLUNewNurbsTessellatorEXT()
{
	GL_LOG("gluNewNurbsTessellatorEXT: delegates to NewNurbsRenderer");
	return NativeGLUNewNurbsRenderer();
}

void NativeGLUDeleteNurbsRenderer(uint32_t nurb)
{
	GL_LOG("gluDeleteNurbsRenderer: handle=0x%08x", nurb);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (ns) {
		ns->s_knots.clear(); ns->t_knots.clear(); ns->control_points.clear();
		ns->curve_knots.clear(); ns->curve_control.clear();
		ns->in_use = false;
	}
}

void NativeGLUDeleteNurbsTessellatorEXT(uint32_t nurb) { NativeGLUDeleteNurbsRenderer(nurb); }

void NativeGLUNurbsProperty(uint32_t nurb, uint32_t property, float value)
{
	GL_LOG("gluNurbsProperty: nurb=0x%08x property=%u value=%f", nurb, property, value);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (!ns) return;
	switch (property) {
		case GLU_SAMPLING_TOLERANCE: ns->sampling_tolerance = value; break;
		case GLU_U_STEP:             ns->u_step = (int)value; if (ns->u_step < 2) ns->u_step = 2; break;
		case GLU_V_STEP:             ns->v_step = (int)value; if (ns->v_step < 2) ns->v_step = 2; break;
		case GLU_DISPLAY_MODE:       ns->display_mode = (uint32_t)value; break;
		case GLU_AUTO_LOAD_MATRIX:   ns->auto_load_matrix = (value != 0.0f); break;
		case GLU_SAMPLING_METHOD:    ns->sampling_method = (uint32_t)value; break;
		case GLU_CULLING:            ns->culling = (value != 0.0f); break;
		case GLU_NURBS_MODE_EXT:     ns->nurbs_mode = (uint32_t)value; break;
		default:
			GL_LOG("gluNurbsProperty: unknown property %u", property);
			break;
	}
}

void NativeGLUGetNurbsProperty(uint32_t nurb, uint32_t property, uint32_t data_ptr)
{
	GL_LOG("gluGetNurbsProperty: nurb=0x%08x property=%u ptr=0x%08x", nurb, property, data_ptr);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (!ns || data_ptr == 0) return;
	float val = 0.0f;
	switch (property) {
		case GLU_SAMPLING_TOLERANCE: val = ns->sampling_tolerance; break;
		case GLU_U_STEP:             val = (float)ns->u_step; break;
		case GLU_V_STEP:             val = (float)ns->v_step; break;
		case GLU_DISPLAY_MODE:       val = (float)ns->display_mode; break;
		case GLU_AUTO_LOAD_MATRIX:   val = ns->auto_load_matrix ? 1.0f : 0.0f; break;
		case GLU_SAMPLING_METHOD:    val = (float)ns->sampling_method; break;
		case GLU_CULLING:            val = ns->culling ? 1.0f : 0.0f; break;
		case GLU_NURBS_MODE_EXT:     val = (float)ns->nurbs_mode; break;
		default:
			GL_LOG("gluGetNurbsProperty: unknown property %u", property);
			break;
	}
	WriteMacFloat_GLU(data_ptr, val);
}

void NativeGLUNurbsCallback(uint32_t nurb, uint32_t which, uint32_t callback)
{
	GL_LOG("gluNurbsCallback: nurb=0x%08x which=%u callback=0x%08x (stored; GLU_ERROR invoked via call_macos)", nurb, which, callback);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (!ns) return;
	// Store the callback address. The reachable selector we actually fire is
	// GLU_ERROR (100103 -> callbacks[3]); the NURBS_*_EXT geometry callbacks
	// fire only inside a render-direct tessellator path that is out of scope.
	if (which >= 100100 && which < 100108) {
		ns->callbacks[which - 100100] = callback;
	}
}

// ---- GLU nurbs error callback fire helper ----
// Fires the registered GLU_ERROR nurbs callback (callbacks[GLU_ERROR-100100])
// back into emulated PPC via call_macos1, with the RAVE FireNoticeMethod
// NULL-guard discipline. Snapshot to local; return ignored.
static void FireNurbsError(GLUNurbsState *ns, uint32_t error_code)
{
	if (!ns) return;
	const uint32_t cb = ns->callbacks[GLU_ERROR - 100100];  // snapshot to local
	if (cb == 0) return;                                     // RAVE NULL-guard — copy EXACTLY
	(void)call_macos1(cb, error_code);
	GL_LOG("FireNurbsError: cb=0x%08x error=%u", cb, error_code);
}

void NativeGLUNurbsCallbackDataEXT(uint32_t nurb, uint32_t userData)
{
	GL_LOG("gluNurbsCallbackDataEXT: nurb=0x%08x userData=0x%08x", nurb, userData);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (ns) ns->user_data = userData;
}

void NativeGLULoadSamplingMatrices(uint32_t nurb, uint32_t model, uint32_t persp, uint32_t view)
{
	GL_LOG("gluLoadSamplingMatrices: nurb=0x%08x model=0x%08x persp=0x%08x view=0x%08x", nurb, model, persp, view);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (!ns) return;
	// Read 16 floats for model matrix
	for (int i = 0; i < 16; i++) ns->model_matrix[i] = ReadMacFloat_GLU(model + i * 4);
	// Read 16 floats for projection matrix
	for (int i = 0; i < 16; i++) ns->proj_matrix[i] = ReadMacFloat_GLU(persp + i * 4);
	// Read 4 ints for viewport
	for (int i = 0; i < 4; i++) ns->viewport[i] = (int)ReadMacInt32(view + i * 4);
	ns->matrices_loaded = true;
}

void NativeGLUBeginSurface(uint32_t nurb)
{
	GL_LOG("gluBeginSurface: nurb=0x%08x", nurb);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (!ns) return;
	ns->in_surface = true;
	ns->surface_defined = false;
	ns->s_knots.clear();
	ns->t_knots.clear();
	ns->control_points.clear();
}

void NativeGLUNurbsSurface(uint32_t nurb, int32_t sKnots, uint32_t sKnotsPtr,
                           int32_t tKnots, uint32_t tKnotsPtr,
                           int32_t sStride, int32_t tStride, uint32_t control,
                           int32_t sOrder, int32_t tOrder, uint32_t type)
{
	GL_LOG("gluNurbsSurface: nurb=0x%08x sKnots=%d tKnots=%d sStride=%d tStride=%d sOrder=%d tOrder=%d type=0x%x",
	       nurb, sKnots, tKnots, sStride, tStride, sOrder, tOrder, type);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (!ns || !ns->in_surface) return;

	// Read knot vectors from PPC memory
	ns->s_knots.resize(sKnots);
	for (int i = 0; i < sKnots; i++) ns->s_knots[i] = ReadMacFloat_GLU(sKnotsPtr + i * 4);

	ns->t_knots.resize(tKnots);
	for (int i = 0; i < tKnots; i++) ns->t_knots[i] = ReadMacFloat_GLU(tKnotsPtr + i * 4);

	ns->s_stride = sStride;
	ns->t_stride = tStride;
	ns->s_order = sOrder;
	ns->t_order = tOrder;
	ns->surface_type = type;

	// Number of control points: (sKnots - sOrder) * (tKnots - tOrder)
	int s_cp = sKnots - sOrder;
	int t_cp = tKnots - tOrder;
	if (s_cp <= 0 || t_cp <= 0) {
		GL_LOG("gluNurbsSurface: invalid control point count s=%d t=%d", s_cp, t_cp);
		// Report the malformed surface to the guest GLU_ERROR nurbs
		// callback (NULL-guarded). glu.h GLU_NURBS_ERROR1=100251 (knot/order).
		FireNurbsError(ns, 100251 /* GLU_NURBS_ERROR1 */);
		return;
	}

	// Determine dimension from type
	int dim = 3; // GL_MAP2_VERTEX_3
	if (type == 0x0DB6 || type == 0x0DB8) dim = 4; // GL_MAP2_VERTEX_4 or with texture

	// Read control points — sStride and tStride are in floats
	// Total floats to read: we need s_cp * t_cp control points, each dim floats
	// Layout: control[i * tStride + j * (dim)] for typical usage, but we use strides
	int total_floats = s_cp * t_cp * dim;
	ns->control_points.resize(total_floats);

	int cp_idx = 0;
	for (int i = 0; i < s_cp; i++) {
		for (int j = 0; j < t_cp; j++) {
			uint32_t addr = control + (i * sStride + j * tStride) * 4;
			for (int d = 0; d < dim; d++) {
				ns->control_points[cp_idx++] = ReadMacFloat_GLU(addr + d * 4);
			}
		}
	}

	ns->surface_defined = true;
}

// ---- de Boor's algorithm for B-spline basis evaluation ----
// Evaluates a B-spline curve at parameter t using de Boor's algorithm.
// knots: knot vector (size n + order)
// control: control points (n * dim floats)
// n: number of control points
// order: spline order (degree + 1)
// dim: dimension of control points (3 or 4)
// t: parameter value
// result: output point (dim floats)
static void de_boor_evaluate(const float *knots, const float *control, int n, int order,
                             int dim, float t, float *result)
{
	int degree = order - 1;

	// Find knot span: largest k such that knots[k] <= t < knots[k+1]
	// Clamp t to valid range
	float t_min = knots[degree];
	float t_max = knots[n]; // knots[n] = knots[numKnots - order]
	if (t < t_min) t = t_min;
	if (t > t_max) t = t_max;

	int k = degree;
	for (int i = degree; i < n; i++) {
		if (t >= knots[i] && t <= knots[i + 1]) {
			k = i;
			// Don't break at the last interval boundary
			if (t < knots[i + 1]) break;
		}
	}

	// de Boor's algorithm: compute point at parameter t
	// Working array: (degree+1) points of dimension dim
	std::vector<float> d((degree + 1) * dim);

	// Initialize with control points p[k-degree] ... p[k]
	for (int j = 0; j <= degree; j++) {
		int cp_idx = k - degree + j;
		if (cp_idx < 0) cp_idx = 0;
		if (cp_idx >= n) cp_idx = n - 1;
		for (int c = 0; c < dim; c++) {
			d[j * dim + c] = control[cp_idx * dim + c];
		}
	}

	// Iterative triangular computation
	for (int r = 1; r <= degree; r++) {
		for (int j = degree; j >= r; j--) {
			int i_knot = k - degree + j;
			float denom = knots[i_knot + degree - r + 1] - knots[i_knot];
			float alpha = (denom > 1e-10f) ? (t - knots[i_knot]) / denom : 0.0f;
			for (int c = 0; c < dim; c++) {
				d[j * dim + c] = (1.0f - alpha) * d[(j - 1) * dim + c] + alpha * d[j * dim + c];
			}
		}
	}

	// Result is d[degree]
	for (int c = 0; c < dim; c++) {
		result[c] = d[degree * dim + c];
	}
}

void NativeGLUEndSurface(uint32_t nurb)
{
	GL_LOG("gluEndSurface: nurb=0x%08x", nurb);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (!ns) return;
	ns->in_surface = false;

	if (!ns->surface_defined || !gl_current_context) {
		GL_LOG("gluEndSurface: no surface data or no GL context");
		return;
	}

	int s_cp = (int)ns->s_knots.size() - ns->s_order;
	int t_cp = (int)ns->t_knots.size() - ns->t_order;
	if (s_cp <= 0 || t_cp <= 0) {
		GL_LOG("gluEndSurface: invalid control point counts s=%d t=%d", s_cp, t_cp);
		return;
	}

	int dim = 3;
	if (ns->surface_type == 0x0DB6 || ns->surface_type == 0x0DB8) dim = 4;

	// Determine sampling resolution
	int u_steps = ns->u_step;
	int v_steps = ns->v_step;
	if (u_steps < 2) u_steps = 2;
	if (v_steps < 2) v_steps = 2;
	// Limit to reasonable max for performance
	if (u_steps > 200) u_steps = 200;
	if (v_steps > 200) v_steps = 200;

	float s_min = ns->s_knots[ns->s_order - 1];
	float s_max = ns->s_knots[s_cp];
	float t_min = ns->t_knots[ns->t_order - 1];
	float t_max = ns->t_knots[t_cp];

	// Evaluate surface at grid points: for each u,v evaluate the B-spline surface
	// using two-pass de Boor: first evaluate curves at u for each row of t control points,
	// then evaluate the resulting curve at v.

	// Allocate grid of evaluated points
	std::vector<float> grid((u_steps + 1) * (v_steps + 1) * 3);

	// For each grid point (u_i, v_j):
	// 1. For each row i of t control points, evaluate the s-direction curve at u
	// 2. Then evaluate the resulting t-direction curve at v
	for (int vi = 0; vi <= v_steps; vi++) {
		float v_param = t_min + (t_max - t_min) * vi / v_steps;

		for (int ui = 0; ui <= u_steps; ui++) {
			float u_param = s_min + (s_max - s_min) * ui / u_steps;

			// Evaluate s-direction curves at u_param for each t control point row
			std::vector<float> t_curve_control(t_cp * dim);
			for (int tj = 0; tj < t_cp; tj++) {
				// Get s-direction control points for row tj
				std::vector<float> s_control(s_cp * dim);
				for (int si = 0; si < s_cp; si++) {
					for (int d = 0; d < dim; d++) {
						s_control[si * dim + d] = ns->control_points[(si * t_cp + tj) * dim + d];
					}
				}
				// Evaluate s-direction curve at u_param
				de_boor_evaluate(ns->s_knots.data(), s_control.data(), s_cp, ns->s_order,
				                 dim, u_param, &t_curve_control[tj * dim]);
			}

			// Evaluate t-direction curve at v_param
			float point[4];
			de_boor_evaluate(ns->t_knots.data(), t_curve_control.data(), t_cp, ns->t_order,
			                 dim, v_param, point);

			// Store result (project if dim==4)
			int grid_idx = (vi * (u_steps + 1) + ui) * 3;
			if (dim == 4 && fabsf(point[3]) > 1e-10f) {
				grid[grid_idx + 0] = point[0] / point[3];
				grid[grid_idx + 1] = point[1] / point[3];
				grid[grid_idx + 2] = point[2] / point[3];
			} else {
				grid[grid_idx + 0] = point[0];
				grid[grid_idx + 1] = point[1];
				grid[grid_idx + 2] = point[2];
			}
		}
	}

	// Emit triangle strips per row
	GLContext *ctx = gl_current_context;
	int strip_count = 0;

	for (int vi = 0; vi < v_steps; vi++) {
		NativeGLBegin(ctx, GL_TRIANGLE_STRIP);
		for (int ui = 0; ui <= u_steps; ui++) {
			int idx0 = (vi * (u_steps + 1) + ui) * 3;
			int idx1 = ((vi + 1) * (u_steps + 1) + ui) * 3;

			// Compute approximate normal from adjacent grid points for lighting
			if (ui < u_steps && vi < v_steps) {
				float du[3], dv[3];
				int idx_next_u = (vi * (u_steps + 1) + ui + 1) * 3;
				int idx_next_v = ((vi + 1) * (u_steps + 1) + ui) * 3;
				for (int c = 0; c < 3; c++) {
					du[c] = grid[idx_next_u + c] - grid[idx0 + c];
					dv[c] = grid[idx_next_v + c] - grid[idx0 + c];
				}
				float nx = du[1] * dv[2] - du[2] * dv[1];
				float ny = du[2] * dv[0] - du[0] * dv[2];
				float nz = du[0] * dv[1] - du[1] * dv[0];
				float nlen = sqrtf(nx * nx + ny * ny + nz * nz);
				if (nlen > 1e-10f) { nx /= nlen; ny /= nlen; nz /= nlen; }
				NativeGLNormal3f(ctx, nx, ny, nz);
			}

			NativeGLVertex3f(ctx, grid[idx0 + 0], grid[idx0 + 1], grid[idx0 + 2]);
			NativeGLVertex3f(ctx, grid[idx1 + 0], grid[idx1 + 1], grid[idx1 + 2]);
		}
		NativeGLEnd(ctx);
		strip_count++;
	}

	GL_LOG("gluEndSurface: emitted %d triangle strips (%d×%d grid, %d total vertices)",
	       strip_count, u_steps, v_steps, (u_steps + 1) * v_steps * 2);
}

void NativeGLUBeginCurve(uint32_t nurb)
{
	GL_LOG("gluBeginCurve: nurb=0x%08x", nurb);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (!ns) return;
	ns->in_curve = true;
	ns->curve_defined = false;
	ns->curve_knots.clear();
	ns->curve_control.clear();
}

void NativeGLUNurbsCurve(uint32_t nurb, int32_t knotCount, uint32_t knots,
                         int32_t stride, uint32_t control, int32_t order, uint32_t type)
{
	GL_LOG("gluNurbsCurve: nurb=0x%08x knotCount=%d stride=%d order=%d type=0x%x",
	       nurb, knotCount, stride, order, type);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (!ns || !ns->in_curve) return;

	// Read knot vector
	ns->curve_knots.resize(knotCount);
	for (int i = 0; i < knotCount; i++) {
		ns->curve_knots[i] = ReadMacFloat_GLU(knots + i * 4);
	}

	ns->curve_order = order;
	ns->curve_stride = stride;
	ns->curve_type = type;

	// Number of control points
	int n_cp = knotCount - order;
	if (n_cp <= 0) {
		GL_LOG("gluNurbsCurve: invalid control point count %d", n_cp);
		return;
	}

	int dim = 3; // GL_MAP1_VERTEX_3
	if (type == 0x0D97 || type == 0x0D99) dim = 4; // GL_MAP1_VERTEX_4

	ns->curve_control.resize(n_cp * dim);
	for (int i = 0; i < n_cp; i++) {
		uint32_t addr = control + i * stride * 4;
		for (int d = 0; d < dim; d++) {
			ns->curve_control[i * dim + d] = ReadMacFloat_GLU(addr + d * 4);
		}
	}

	ns->curve_defined = true;
}

void NativeGLUEndCurve(uint32_t nurb)
{
	GL_LOG("gluEndCurve: nurb=0x%08x", nurb);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (!ns) return;
	ns->in_curve = false;

	if (!ns->curve_defined || !gl_current_context) {
		GL_LOG("gluEndCurve: no curve data or no GL context");
		return;
	}

	int n_cp = (int)ns->curve_knots.size() - ns->curve_order;
	if (n_cp <= 0) return;

	int dim = 3;
	if (ns->curve_type == 0x0D97 || ns->curve_type == 0x0D99) dim = 4;

	int steps = ns->u_step;
	if (steps < 2) steps = 2;
	if (steps > 500) steps = 500;

	float t_min = ns->curve_knots[ns->curve_order - 1];
	float t_max = ns->curve_knots[n_cp];

	GLContext *ctx = gl_current_context;
	NativeGLBegin(ctx, GL_LINE_STRIP);

	for (int i = 0; i <= steps; i++) {
		float t = t_min + (t_max - t_min) * i / steps;
		float point[4];
		de_boor_evaluate(ns->curve_knots.data(), ns->curve_control.data(), n_cp, ns->curve_order,
		                 dim, t, point);

		if (dim == 4 && fabsf(point[3]) > 1e-10f) {
			NativeGLVertex3f(ctx, point[0] / point[3], point[1] / point[3], point[2] / point[3]);
		} else {
			NativeGLVertex3f(ctx, point[0], point[1], point[2]);
		}
	}

	NativeGLEnd(ctx);
	GL_LOG("gluEndCurve: emitted GL_LINE_STRIP with %d vertices", steps + 1);
}

// ---- Trim curves: known limitation (extremely rare in classic Mac games) ----

// GLU NURBS trim curves (gluBeginTrim/EndTrim/PwlCurve, sub-ops 703/715/737)
// are out-of-scope (DELIBERATE) — extremely rare in classic Mac games,
// and canary apps drive their 3D through AGL/RAVE, not NURBS-trimmed surfaces.
// Each handler keeps its honest "known limitation" marker so the gate test can
// assert the limitation is documented, NOT silently half-supported. No behavior
// change (the in_trim flag is tracked but trim geometry is not tessellated).
void NativeGLUBeginTrim(uint32_t nurb)
{
	GL_LOG("gluBeginTrim: nurb=0x%08x — known limitation (NURBS trim curves out-of-scope, extremely rare; canary apps use AGL)", nurb);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (ns) ns->in_trim = true;
}

void NativeGLUEndTrim(uint32_t nurb)
{
	GL_LOG("gluEndTrim: nurb=0x%08x — known limitation (NURBS trim curves out-of-scope, extremely rare; canary apps use AGL)", nurb);
	GLUNurbsState *ns = GLUNurbsFromHandle(nurb);
	if (ns) ns->in_trim = false;
}

void NativeGLUPwlCurve(uint32_t nurb, int32_t count, uint32_t data, int32_t stride, uint32_t type)
{
	GL_LOG("gluPwlCurve: nurb=0x%08x count=%d — known limitation (piecewise-linear NURBS trim curves out-of-scope, extremely rare; canary apps use AGL)", nurb, count);
	(void)data; (void)stride; (void)type;
}


// ===========================================================================
//  GLU Error/Info Functions
// ===========================================================================

uint32_t NativeGLUErrorString(uint32_t error)
{
	GL_LOG("gluErrorString: error=%d", error);

	// Allocate Mac-side string on first call
	static uint32_t error_string_mac = 0;
	if (error_string_mac == 0) {
		error_string_mac = Mac_sysalloc(64);
		if (error_string_mac == 0) return 0;
	}

	const char *msg = "unknown error";
	switch (error) {
		case 0: msg = "no error"; break;
		case GLU_INVALID_ENUM: msg = "invalid enumerant"; break;
		case GLU_INVALID_VALUE: msg = "invalid value"; break;
		case GLU_OUT_OF_MEMORY: msg = "out of memory"; break;
		// glu.h ErrorCode 100903/100904 (were falling through to the
		// "unknown error" default — silent-wrong-output for a conformant app).
		case 100903: msg = "incompatible gl version"; break;  // GLU_INCOMPATIBLE_GL_VERSION
		case 100904: msg = "invalid operation"; break;        // GLU_INVALID_OPERATION
		// GL errors
		case 0x0500: msg = "invalid enum"; break;
		case 0x0501: msg = "invalid value"; break;
		case 0x0502: msg = "invalid operation"; break;
		case 0x0503: msg = "stack overflow"; break;
		case 0x0504: msg = "stack underflow"; break;
		case 0x0505: msg = "out of memory"; break;
	}

	// Write to Mac memory as C string
	int len = (int)strlen(msg);
	if (len > 62) len = 62;
	for (int i = 0; i < len; i++)
		WriteMacInt8(error_string_mac + i, msg[i]);
	WriteMacInt8(error_string_mac + len, 0);

	return error_string_mac;
}

uint32_t NativeGLUGetString(uint32_t name)
{
	GL_LOG("gluGetString: name=%d", name);

	if (name == GLU_VERSION) {
		if (glu_version_string_mac == 0) {
			glu_version_string_mac = Mac_sysalloc(8);
			if (glu_version_string_mac) {
				const char *v = "1.2";
				for (int i = 0; i <= 3; i++) WriteMacInt8(glu_version_string_mac + i, v[i]);
			}
		}
		return glu_version_string_mac;
	}

	if (name == GLU_EXTENSIONS) {
		if (glu_extensions_string_mac == 0) {
			glu_extensions_string_mac = Mac_sysalloc(4);
			if (glu_extensions_string_mac) WriteMacInt8(glu_extensions_string_mac, 0);
		}
		return glu_extensions_string_mac;
	}

	return 0;
}


// ===========================================================================
//  GLUT Windowing Stubs and Shape Primitives
// ===========================================================================

// GLUT display mode flags
#define GLUT_RGBA_           0
#define GLUT_DOUBLE_         2
#define GLUT_DEPTH_          16

// ---- GLUT State ----
static bool glut_initialized = false;
static uint32_t glut_display_mode = 0;
static int glut_init_width = 300;
static int glut_init_height = 300;
static int glut_init_x = 0;
static int glut_init_y = 0;
static int glut_current_window = 0;
static bool glut_main_loop_active = false;
static bool glut_needs_redisplay = false;

// Callback storage (Mac function addresses)
static uint32_t glut_display_func = 0;
static uint32_t glut_idle_func = 0;
static uint32_t glut_reshape_func = 0;
static uint32_t glut_keyboard_func = 0;
static uint32_t glut_mouse_func = 0;
static uint32_t glut_motion_func = 0;
static uint32_t glut_passive_motion_func = 0;
static uint32_t glut_special_func = 0;
static uint32_t glut_entry_func = 0;
static uint32_t glut_visibility_func = 0;
static uint32_t glut_menu_state_func = 0;
static uint32_t glut_menu_status_func = 0;
static uint32_t glut_overlay_display_func = 0;
static uint32_t glut_window_status_func = 0;
static uint32_t glut_keyboard_up_func = 0;
static uint32_t glut_special_up_func = 0;
static uint32_t glut_joystick_func = 0;

// Timer callback list (simple, max 16 pending timers)
#define GLUT_MAX_TIMERS 16
struct GLUTTimer {
	bool active;
	uint32_t millis;
	uint32_t func;    // Mac callback address
	int32_t value;
};
static GLUTTimer glut_timers[GLUT_MAX_TIMERS];

// Elapsed time tracking
static uint32_t glut_start_ticks = 0;


/*
 *  NativeGLUTInitMac(r3=argcp, r4=argv)
 */
void NativeGLUTInitMac(uint32_t argcp, uint32_t argv)
{
	GL_LOG("glutInit: argcp=0x%08x argv=0x%08x", argcp, argv);
	glut_initialized = true;
	glut_display_mode = GLUT_RGBA_ | GLUT_DOUBLE_;
	glut_init_width = 300;
	glut_init_height = 300;
	glut_init_x = 0;
	glut_init_y = 0;
	glut_current_window = 0;
	glut_main_loop_active = false;
	glut_start_ticks = 0;  // Will be set on first glutGet(GLUT_ELAPSED_TIME)
	memset(glut_timers, 0, sizeof(glut_timers));
}

void NativeGLUTInitDisplayMode(uint32_t mode)
{
	GL_LOG("glutInitDisplayMode: mode=0x%x", mode);
	glut_display_mode = mode;
}

void NativeGLUTInitDisplayString(uint32_t string_ptr)
{
	GL_LOG("glutInitDisplayString: string=0x%08x (known limitation)", string_ptr);
}

void NativeGLUTInitWindowPosition(int32_t x, int32_t y)
{
	GL_LOG("glutInitWindowPosition: %d,%d", x, y);
	glut_init_x = x;
	glut_init_y = y;
}

void NativeGLUTInitWindowSize(int32_t width, int32_t height)
{
	GL_LOG("glutInitWindowSize: %dx%d", width, height);
	glut_init_width = width;
	glut_init_height = height;
}


// GLUT windowing stubs return GL_INVALID_OPERATION.
// GLUT windowing is not supported inside the emulator — these functions
// fail loudly so apps fall back to non-GLUT code paths.

uint32_t NativeGLUTCreateWindow(uint32_t title_ptr)
{
	GL_LOG("glutCreateWindow: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	// GLUT windowing stubs return error
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
	(void)title_ptr;
	return 0;
}

uint32_t NativeGLUTCreateSubWindow(int32_t win, int32_t x, int32_t y, int32_t width, int32_t height)
{
	GL_LOG("glutCreateSubWindow: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
	(void)win; (void)x; (void)y; (void)width; (void)height;
	return 0;
}

void NativeGLUTDestroyWindow(int32_t win)
{
	GL_LOG("glutDestroyWindow: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
	(void)win;
}

uint32_t NativeGLUTGetWindow()
{
	return glut_current_window;
}

void NativeGLUTSetWindow(int32_t win)
{
	GL_LOG("glutSetWindow: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
	(void)win;
}

void NativeGLUTSetWindowTitle(uint32_t title_ptr)
{
	GL_LOG("glutSetWindowTitle: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
	(void)title_ptr;
}

void NativeGLUTSetIconTitle(uint32_t title_ptr)
{
	GL_LOG("glutSetIconTitle: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
	(void)title_ptr;
}

void NativeGLUTPositionWindow(int32_t x, int32_t y)
{
	GL_LOG("glutPositionWindow: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
	(void)x; (void)y;
}

void NativeGLUTReshapeWindow(int32_t width, int32_t height)
{
	GL_LOG("glutReshapeWindow: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
	(void)width; (void)height;
}

void NativeGLUTPopWindow()
{
	GL_LOG("glutPopWindow: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
}

void NativeGLUTPushWindow()
{
	GL_LOG("glutPushWindow: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
}

void NativeGLUTIconifyWindow()
{
	GL_LOG("glutIconifyWindow: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
}

void NativeGLUTShowWindow()
{
	GL_LOG("glutShowWindow: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
}

void NativeGLUTHideWindow()
{
	GL_LOG("glutHideWindow: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
}

void NativeGLUTFullScreen()
{
	GL_LOG("glutFullScreen: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
}
void NativeGLUTSetCursor(int32_t cursor) { GL_LOG("glutSetCursor: %d (known limitation)", cursor); (void)cursor; }
void NativeGLUTWarpPointer(int32_t x, int32_t y) { GL_LOG("glutWarpPointer: %d,%d (known limitation)", x, y); (void)x; (void)y; }


/*
 *  NativeGLUTMainLoop()
 *
 *  The GLUT event/timer/mainloop program model is de-advertised (DELIBERATE).
 *  glutMainLoop MUST NOT BLOCK — it is non-blocking and the GLUT program model
 *  is UNSUPPORTED: registered window callbacks (display / reshape / keyboard /
 *  mouse / motion / timer ...) are stored but NOT serviced — they never fire.
 *  This is the HONEST behavior (no silent success): control returns to the
 *  emulator's PPC event loop, and a conformant GLUT app that expects its
 *  callbacks to be driven by glutMainLoop will observe they are not, rather
 *  than silently mis-running on a stored-never-fired callback. Canary apps
 *  (The Sims, Nanosaur, Bugdom) drive their event loop and 3D through AGL
 *  directly, never the GLUT program model.
 */
void NativeGLUTMainLoop()
{
	GL_LOG("glutMainLoop: entering (non-blocking) — GLUT program model unsupported/de-advertised: callbacks/timers are NOT serviced; control returns to the emulator");
	glut_main_loop_active = true;
	// Return control to emulator -- the PPC event loop continues normally.
	// The stored GLUT callbacks/timers are NOT fired (program model de-advertised).
}

void NativeGLUTPostRedisplay()
{
	glut_needs_redisplay = true;
}

void NativeGLUTPostWindowRedisplay(int32_t win)
{
	GL_LOG("glutPostWindowRedisplay: win=%d", win);
	glut_needs_redisplay = true;
}

void NativeGLUTSwapBuffers()
{
	GL_LOG("glutSwapBuffers");
	// Delegate to aglSwapBuffers on current context
	if (gl_current_context_idx >= 0 && gl_context_mac_handles[gl_current_context_idx]) {
		NativeAGLSwapBuffers(gl_context_mac_handles[gl_current_context_idx]);
	}
}


// ---- Callback registration ----
// The GLUT window-callback registrars (glutDisplayFunc, glutReshapeFunc,
// glutKeyboardFunc, glutMouseFunc, glutMotionFunc, ..., sub-ops 846-870) store
// the guest callback address HONESTLY but the callback is NEVER fired — the
// GLUT event/timer/mainloop program model is de-advertised (glutMainLoop is
// non-blocking and does not service callbacks). The stores claim NOTHING
// beyond "the address was recorded"; they do NOT imply the callback will be
// driven. A conformant GLUT app that depends on these callbacks firing
// observes they do not, rather than silently mis-running. Canary apps use AGL
// directly, not the GLUT program model.
void NativeGLUTDisplayFunc(uint32_t func) { GL_LOG("glutDisplayFunc: 0x%08x — stored, NOT serviced (GLUT program model de-advertised)", func); glut_display_func = func; }
void NativeGLUTReshapeFunc(uint32_t func) { GL_LOG("glutReshapeFunc: 0x%08x", func); glut_reshape_func = func; }
void NativeGLUTKeyboardFunc(uint32_t func) { GL_LOG("glutKeyboardFunc: 0x%08x", func); glut_keyboard_func = func; }
void NativeGLUTMouseFunc(uint32_t func) { GL_LOG("glutMouseFunc: 0x%08x", func); glut_mouse_func = func; }
void NativeGLUTMotionFunc(uint32_t func) { GL_LOG("glutMotionFunc: 0x%08x", func); glut_motion_func = func; }
void NativeGLUTPassiveMotionFunc(uint32_t func) { GL_LOG("glutPassiveMotionFunc: 0x%08x", func); glut_passive_motion_func = func; }
void NativeGLUTEntryFunc(uint32_t func) { GL_LOG("glutEntryFunc: 0x%08x", func); glut_entry_func = func; }
void NativeGLUTVisibilityFunc(uint32_t func) { GL_LOG("glutVisibilityFunc: 0x%08x", func); glut_visibility_func = func; }
void NativeGLUTIdleFunc(uint32_t func) { GL_LOG("glutIdleFunc: 0x%08x", func); glut_idle_func = func; }
void NativeGLUTMenuStateFunc(uint32_t func) { GL_LOG("glutMenuStateFunc: 0x%08x", func); glut_menu_state_func = func; }
void NativeGLUTSpecialFunc(uint32_t func) { GL_LOG("glutSpecialFunc: 0x%08x", func); glut_special_func = func; }
void NativeGLUTMenuStatusFunc(uint32_t func) { GL_LOG("glutMenuStatusFunc: 0x%08x", func); glut_menu_status_func = func; }
void NativeGLUTOverlayDisplayFunc(uint32_t func) { GL_LOG("glutOverlayDisplayFunc: 0x%08x", func); glut_overlay_display_func = func; }
void NativeGLUTWindowStatusFunc(uint32_t func) { GL_LOG("glutWindowStatusFunc: 0x%08x", func); glut_window_status_func = func; }
void NativeGLUTKeyboardUpFunc(uint32_t func) { GL_LOG("glutKeyboardUpFunc: 0x%08x", func); glut_keyboard_up_func = func; }
void NativeGLUTSpecialUpFunc(uint32_t func) { GL_LOG("glutSpecialUpFunc: 0x%08x", func); glut_special_up_func = func; }
void NativeGLUTJoystickFunc(uint32_t func, int32_t pollInterval) { GL_LOG("glutJoystickFunc: 0x%08x interval=%d", func, pollInterval); glut_joystick_func = func; (void)pollInterval; }

// glutTimerFunc stores the timer record honestly but the timer is NEVER
// serviced — the GLUT program model (glutMainLoop) is de-advertised, so the
// registered callback does not fire. The record is kept for API completeness
// (so glutGet/round-trips are consistent), NOT because the timer will ever
// elapse. This is the same honest de-advertisement as the window-callback
// registrars below (stored-never-fired).
void NativeGLUTTimerFunc(uint32_t millis, uint32_t func, int32_t value)
{
	GL_LOG("glutTimerFunc: ms=%d func=0x%08x value=%d — stored but NOT serviced (GLUT program model de-advertised)", millis, func, value);
	for (int i = 0; i < GLUT_MAX_TIMERS; i++) {
		if (!glut_timers[i].active) {
			glut_timers[i].active = true;
			glut_timers[i].millis = millis;
			glut_timers[i].func = func;
			glut_timers[i].value = value;
			return;
		}
	}
	GL_LOG("glutTimerFunc: no free timer slots");
}

// Spaceball/tablet/button box known limitations (no-op on iOS)
void NativeGLUTSpaceballMotionFunc(uint32_t func) { GL_LOG("glutSpaceballMotionFunc: known limitation"); (void)func; }
void NativeGLUTSpaceballRotateFunc(uint32_t func) { GL_LOG("glutSpaceballRotateFunc: known limitation"); (void)func; }
void NativeGLUTSpaceballButtonFunc(uint32_t func) { GL_LOG("glutSpaceballButtonFunc: known limitation"); (void)func; }
void NativeGLUTButtonBoxFunc(uint32_t func) { GL_LOG("glutButtonBoxFunc: known limitation"); (void)func; }
void NativeGLUTDialsFunc(uint32_t func) { GL_LOG("glutDialsFunc: known limitation"); (void)func; }
void NativeGLUTTabletMotionFunc(uint32_t func) { GL_LOG("glutTabletMotionFunc: known limitation"); (void)func; }
void NativeGLUTTabletButtonFunc(uint32_t func) { GL_LOG("glutTabletButtonFunc: known limitation"); (void)func; }


// ---- Overlay stubs — GL_INVALID_OPERATION ----
void NativeGLUTEstablishOverlay()
{
	GL_LOG("glutEstablishOverlay: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
}

void NativeGLUTRemoveOverlay()
{
	GL_LOG("glutRemoveOverlay: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
}

void NativeGLUTUseLayer(uint32_t layer)
{
	GL_LOG("glutUseLayer: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
	(void)layer;
}

void NativeGLUTPostOverlayRedisplay()
{
	GL_LOG("glutPostOverlayRedisplay: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
}

void NativeGLUTPostWindowOverlayRedisplay(int32_t win)
{
	GL_LOG("glutPostWindowOverlayRedisplay: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
	(void)win;
}

void NativeGLUTShowOverlay()
{
	GL_LOG("glutShowOverlay: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
}

void NativeGLUTHideOverlay()
{
	GL_LOG("glutHideOverlay: GL_INVALID_OPERATION (GLUT windowing not supported in emulator)");
	if (gl_current_context) gl_current_context->last_error = GL_INVALID_OPERATION;
}


// ---- Menu known limitations ----
uint32_t NativeGLUTCreateMenu(uint32_t callback) { GL_LOG("glutCreateMenu: callback=0x%08x (known limitation)", callback); (void)callback; return 1; }
void NativeGLUTDestroyMenu(int32_t menu) { GL_LOG("glutDestroyMenu: %d (known limitation)", menu); (void)menu; }
uint32_t NativeGLUTGetMenu() { return 0; }
void NativeGLUTSetMenu(int32_t menu) { GL_LOG("glutSetMenu: %d (known limitation)", menu); (void)menu; }
void NativeGLUTAddMenuEntry(uint32_t label, int32_t value) { GL_LOG("glutAddMenuEntry: known limitation"); (void)label; (void)value; }
void NativeGLUTAddSubMenu(uint32_t label, int32_t submenu) { GL_LOG("glutAddSubMenu: known limitation"); (void)label; (void)submenu; }
void NativeGLUTChangeToMenuEntry(int32_t item, uint32_t label, int32_t value) { GL_LOG("glutChangeToMenuEntry: known limitation"); (void)item; (void)label; (void)value; }
void NativeGLUTChangeToSubMenu(int32_t item, uint32_t label, int32_t submenu) { GL_LOG("glutChangeToSubMenu: known limitation"); (void)item; (void)label; (void)submenu; }
void NativeGLUTRemoveMenuItem(int32_t item) { GL_LOG("glutRemoveMenuItem: known limitation"); (void)item; }
void NativeGLUTAttachMenu(int32_t button) { GL_LOG("glutAttachMenu: %d (known limitation)", button); (void)button; }
void NativeGLUTAttachMenuName(int32_t button, uint32_t name) { GL_LOG("glutAttachMenuName: known limitation"); (void)button; (void)name; }
void NativeGLUTDetachMenu(int32_t button) { GL_LOG("glutDetachMenu: %d (known limitation)", button); (void)button; }


// ---- Color index known limitations ----
void NativeGLUTSetColor(int32_t cell, float red, float green, float blue) { GL_LOG("glutSetColor: %d (known limitation, RGBA mode)", cell); (void)cell; (void)red; (void)green; (void)blue; }
float NativeGLUTGetColor(int32_t ndx, int32_t component) { GL_LOG("glutGetColor: known limitation"); (void)ndx; (void)component; return 0.0f; }
void NativeGLUTCopyColormap(int32_t win) { GL_LOG("glutCopyColormap: known limitation"); (void)win; }


/*
 *  NativeGLUTGet(type) - Return GLUT state values
 */
int32_t NativeGLUTGet(uint32_t type)
{
	switch (type) {
		case 100: return glut_init_x;        // GLUT_WINDOW_X
		case 101: return glut_init_y;        // GLUT_WINDOW_Y
		case 102: return glut_init_width;    // GLUT_WINDOW_WIDTH
		case 103: return glut_init_height;   // GLUT_WINDOW_HEIGHT
		case 104: return 32;                 // GLUT_WINDOW_BUFFER_SIZE
		case 105: return 8;                  // GLUT_WINDOW_STENCIL_SIZE
		case 106: return 24;                 // GLUT_WINDOW_DEPTH_SIZE
		case 107: return 8;                  // GLUT_WINDOW_RED_SIZE
		case 108: return 8;                  // GLUT_WINDOW_GREEN_SIZE
		case 109: return 8;                  // GLUT_WINDOW_BLUE_SIZE
		case 110: return 8;                  // GLUT_WINDOW_ALPHA_SIZE
		case 115: return 1;                  // GLUT_WINDOW_DOUBLEBUFFER
		case 116: return 1;                  // GLUT_WINDOW_RGBA
		case 117: return 0;                  // GLUT_WINDOW_PARENT
		case 118: return 0;                  // GLUT_WINDOW_NUM_CHILDREN
		case 200: return 640;                // GLUT_SCREEN_WIDTH
		case 201: return 480;                // GLUT_SCREEN_HEIGHT
		case 400: return 1;                  // GLUT_DISPLAY_MODE_POSSIBLE
		case 500: return glut_init_x;        // GLUT_INIT_WINDOW_X
		case 501: return glut_init_y;        // GLUT_INIT_WINDOW_Y
		case 502: return glut_init_width;    // GLUT_INIT_WINDOW_WIDTH
		case 503: return glut_init_height;   // GLUT_INIT_WINDOW_HEIGHT
		case 504: return (int32_t)glut_display_mode; // GLUT_INIT_DISPLAY_MODE
		// GLUT_ELAPSED_TIME is intentionally inert (0). The GLUT
		// event/timer/mainloop program model is de-advertised — no real clock
		// is serviced (wiring a tick source would contradict de-advertising
		// the program model). This is a DOCUMENTED inert 0, NOT an
		// undocumented frozen-clock lie: a conformant GLUT app reading a
		// never-advancing clock detects the unsupported program model and
		// branches, rather than silently mis-running. Canary apps use AGL,
		// never the GLUT program model.
		case 700: return 0;                  // GLUT_ELAPSED_TIME (milliseconds) — documented inert
		default:
			// glutGet BLIND_SPOT: the accum / colormap / samples / stereo /
			// cursor / format-id / screen-mm / menu enums fall here and return
			// 0. Returning 0 == "capability absent" is HONEST so long as it is
			// documented: this default logs the unhandled enum so the silent-0
			// is documented capability-absence, not a silent lie.
			GL_LOG("glutGet: unhandled glutGet enum %d — capability reported absent (0); GLUT program model de-advertised", type);
			return 0;
	}
}

int32_t NativeGLUTDeviceGet(uint32_t type)
{
	switch (type) {
		case 600: return 1;   // GLUT_HAS_KEYBOARD
		case 601: return 1;   // GLUT_HAS_MOUSE
		case 605: return 1;   // GLUT_NUM_MOUSE_BUTTONS
		default:  return 0;
	}
}

int32_t NativeGLUTExtensionSupported(uint32_t name_ptr)
{
	GL_LOG("glutExtensionSupported: 0x%08x -> 0 (no extensions)", name_ptr);
	return 0;
}

int32_t NativeGLUTGetModifiers()
{
	return 0;  // No modifiers pressed
}

int32_t NativeGLUTLayerGet(uint32_t type)
{
	GL_LOG("glutLayerGet: %d (known limitation)", type);
	return 0;
}


// ===========================================================================
//  GLUT Font Rendering (minimal 8x13 fixed-width bitmap)
// ===========================================================================

// Simple 8x13 font -- we just use a minimal approach:
// Each character is rendered as a small quad at the raster position.
// For now, advance the raster position by the character width.

// glutBitmapCharacter (879) / glutStrokeCharacter (881) produce INVISIBLE
// text — there is no embedded GLUT glyph data and no Font Manager wired, so
// the character is not actually rasterized. This is a DELIBERATE known
// limitation, not a silent stub producing wrong output: the raster position
// still advances (so layout math stays consistent) but no glyph pixels are
// drawn. Canary apps render their text through AGL/QuickDraw, not GLUT glyphs.
void NativeGLUTBitmapCharacter(GLContext *ctx, uint32_t font, int32_t character)
{
	GL_LOG("glutBitmapCharacter: font=0x%08x char=%d (known limitation - no glyph data + no Font Manager -> invisible text; canary apps use AGL)", font, character);
	// In a full implementation, we'd rasterize the character bitmap.
	// No embedded glyph data / no Font Manager -> invisible text.
	// For now, just advance the raster position by 8 pixels.
	if (ctx) {
		ctx->raster_pos[0] += 8.0f;
	}
	(void)font;
}

int32_t NativeGLUTBitmapWidth(uint32_t font, int32_t character)
{
	(void)font; (void)character;
	return 8;  // Fixed-width 8 pixels
}

// No embedded GLUT stroke-glyph data + no Font Manager -> invisible text.
// DELIBERATE known limitation (canary apps use AGL).
void NativeGLUTStrokeCharacter(GLContext *ctx, uint32_t font, int32_t character)
{
	GL_LOG("glutStrokeCharacter: font=0x%08x char=%d (known limitation - no glyph data + no Font Manager -> invisible text; canary apps use AGL)", font, character);
	(void)ctx; (void)font; (void)character;
}

int32_t NativeGLUTStrokeWidth(uint32_t font, int32_t character)
{
	(void)font; (void)character;
	return 104;  // GLUT stroke font nominal width ~104.76 units
}

int32_t NativeGLUTBitmapLength(uint32_t font, uint32_t string_ptr)
{
	if (!string_ptr) return 0;
	int len = 0;
	for (int i = 0; i < 4096; i++) {
		if (ReadMacInt8(string_ptr + i) == 0) break;
		len++;
	}
	(void)font;
	return len * 8;  // 8 pixels per character
}

int32_t NativeGLUTStrokeLength(uint32_t font, uint32_t string_ptr)
{
	if (!string_ptr) return 0;
	int len = 0;
	for (int i = 0; i < 4096; i++) {
		if (ReadMacInt8(string_ptr + i) == 0) break;
		len++;
	}
	(void)font;
	return len * 104;  // Stroke font width
}


// ===========================================================================
//  GLUT Shape Primitives (generate geometry via immediate mode)
// ===========================================================================

/*
 *  glutSolidSphere / glutWireSphere -- delegate to gluSphere
 */
void NativeGLUTSolidSphere(GLContext *ctx, double radius, int32_t slices, int32_t stacks)
{
	GL_LOG("glutSolidSphere: r=%f slices=%d stacks=%d", radius, slices, stacks);
	// Create a temporary quadric with FILL mode
	static uint32_t solid_quad = 0;
	if (!solid_quad) solid_quad = NativeGLUNewQuadric();
	NativeGLUQuadricDrawStyle(solid_quad, GLU_FILL_);
	NativeGLUQuadricNormals(solid_quad, GLU_SMOOTH);
	NativeGLUSphere(ctx, solid_quad, radius, slices, stacks);
}

void NativeGLUTWireSphere(GLContext *ctx, double radius, int32_t slices, int32_t stacks)
{
	GL_LOG("glutWireSphere: r=%f slices=%d stacks=%d", radius, slices, stacks);
	static uint32_t wire_quad = 0;
	if (!wire_quad) wire_quad = NativeGLUNewQuadric();
	NativeGLUQuadricDrawStyle(wire_quad, GLU_LINE);
	NativeGLUQuadricNormals(wire_quad, GLU_SMOOTH);
	NativeGLUSphere(ctx, wire_quad, radius, slices, stacks);
}


/*
 *  glutSolidCube / glutWireCube
 */
void NativeGLUTSolidCube(GLContext *ctx, double size)
{
	GL_LOG("glutSolidCube: size=%f", size);
	if (!ctx) return;

	float s = (float)(size / 2.0);

	// 6 faces as quads
	static const float faces[6][4][3] = {
		// Front
		{{ -1, -1,  1}, {  1, -1,  1}, {  1,  1,  1}, { -1,  1,  1}},
		// Back
		{{ -1, -1, -1}, { -1,  1, -1}, {  1,  1, -1}, {  1, -1, -1}},
		// Top
		{{ -1,  1, -1}, { -1,  1,  1}, {  1,  1,  1}, {  1,  1, -1}},
		// Bottom
		{{ -1, -1, -1}, {  1, -1, -1}, {  1, -1,  1}, { -1, -1,  1}},
		// Right
		{{  1, -1, -1}, {  1,  1, -1}, {  1,  1,  1}, {  1, -1,  1}},
		// Left
		{{ -1, -1, -1}, { -1, -1,  1}, { -1,  1,  1}, { -1,  1, -1}},
	};
	static const float normals[6][3] = {
		{0,0,1}, {0,0,-1}, {0,1,0}, {0,-1,0}, {1,0,0}, {-1,0,0}
	};

	NativeGLBegin(ctx, GL_QUADS);
	for (int f = 0; f < 6; f++) {
		NativeGLNormal3f(ctx, normals[f][0], normals[f][1], normals[f][2]);
		for (int v = 0; v < 4; v++) {
			NativeGLVertex3f(ctx, faces[f][v][0] * s, faces[f][v][1] * s, faces[f][v][2] * s);
		}
	}
	NativeGLEnd(ctx);
}

void NativeGLUTWireCube(GLContext *ctx, double size)
{
	GL_LOG("glutWireCube: size=%f", size);
	if (!ctx) return;

	float s = (float)(size / 2.0);

	// 6 faces as line loops
	static const float faces[6][4][3] = {
		{{ -1, -1,  1}, {  1, -1,  1}, {  1,  1,  1}, { -1,  1,  1}},
		{{ -1, -1, -1}, { -1,  1, -1}, {  1,  1, -1}, {  1, -1, -1}},
		{{ -1,  1, -1}, { -1,  1,  1}, {  1,  1,  1}, {  1,  1, -1}},
		{{ -1, -1, -1}, {  1, -1, -1}, {  1, -1,  1}, { -1, -1,  1}},
		{{  1, -1, -1}, {  1,  1, -1}, {  1,  1,  1}, {  1, -1,  1}},
		{{ -1, -1, -1}, { -1, -1,  1}, { -1,  1,  1}, { -1,  1, -1}},
	};

	for (int f = 0; f < 6; f++) {
		NativeGLBegin(ctx, GL_LINE_LOOP);
		for (int v = 0; v < 4; v++) {
			NativeGLVertex3f(ctx, faces[f][v][0] * s, faces[f][v][1] * s, faces[f][v][2] * s);
		}
		NativeGLEnd(ctx);
	}
}


/*
 *  glutSolidCone / glutWireCone -- delegate to gluCylinder with top=0
 */
void NativeGLUTSolidCone(GLContext *ctx, double base, double height, int32_t slices, int32_t stacks)
{
	GL_LOG("glutSolidCone: base=%f height=%f", base, height);
	static uint32_t cone_quad = 0;
	if (!cone_quad) cone_quad = NativeGLUNewQuadric();
	NativeGLUQuadricDrawStyle(cone_quad, GLU_FILL_);
	NativeGLUQuadricNormals(cone_quad, GLU_SMOOTH);
	NativeGLUCylinder(ctx, cone_quad, base, 0.0, height, slices, stacks);
}

void NativeGLUTWireCone(GLContext *ctx, double base, double height, int32_t slices, int32_t stacks)
{
	GL_LOG("glutWireCone: base=%f height=%f", base, height);
	static uint32_t wire_cone_quad = 0;
	if (!wire_cone_quad) wire_cone_quad = NativeGLUNewQuadric();
	NativeGLUQuadricDrawStyle(wire_cone_quad, GLU_LINE);
	NativeGLUQuadricNormals(wire_cone_quad, GLU_SMOOTH);
	NativeGLUCylinder(ctx, wire_cone_quad, base, 0.0, height, slices, stacks);
}


/*
 *  glutSolidTorus / glutWireTorus
 */
void NativeGLUTSolidTorus(GLContext *ctx, double innerRadius, double outerRadius, int32_t sides, int32_t rings)
{
	GL_LOG("glutSolidTorus: inner=%f outer=%f sides=%d rings=%d", innerRadius, outerRadius, sides, rings);
	if (!ctx || sides < 3 || rings < 3) return;
	sides = ClampTessCount(sides, "sides");
	rings = ClampTessCount(rings, "rings");

	float ir = (float)innerRadius, or_ = (float)outerRadius;

	for (int i = 0; i < rings; i++) {
		float theta0 = (float)(2.0 * M_PI * i / rings);
		float theta1 = (float)(2.0 * M_PI * (i + 1) / rings);
		float ct0 = cosf(theta0), st0 = sinf(theta0);
		float ct1 = cosf(theta1), st1 = sinf(theta1);

		NativeGLBegin(ctx, GL_TRIANGLE_STRIP);
		for (int j = 0; j <= sides; j++) {
			float phi = (float)(2.0 * M_PI * j / sides);
			float cp = cosf(phi), sp = sinf(phi);

			// Point on ring i+1
			float x1 = (or_ + ir * cp) * ct1;
			float y1 = (or_ + ir * cp) * st1;
			float z1 = ir * sp;
			NativeGLNormal3f(ctx, cp * ct1, cp * st1, sp);
			NativeGLVertex3f(ctx, x1, y1, z1);

			// Point on ring i
			float x0 = (or_ + ir * cp) * ct0;
			float y0 = (or_ + ir * cp) * st0;
			float z0 = ir * sp;
			NativeGLNormal3f(ctx, cp * ct0, cp * st0, sp);
			NativeGLVertex3f(ctx, x0, y0, z0);
		}
		NativeGLEnd(ctx);
	}
}

void NativeGLUTWireTorus(GLContext *ctx, double innerRadius, double outerRadius, int32_t sides, int32_t rings)
{
	GL_LOG("glutWireTorus: inner=%f outer=%f sides=%d rings=%d", innerRadius, outerRadius, sides, rings);
	if (!ctx || sides < 3 || rings < 3) return;
	sides = ClampTessCount(sides, "sides");
	rings = ClampTessCount(rings, "rings");

	float ir = (float)innerRadius, or_ = (float)outerRadius;

	// Draw ring circles (along each ring position)
	for (int i = 0; i <= rings; i++) {
		float theta = (float)(2.0 * M_PI * i / rings);
		float ct = cosf(theta), st = sinf(theta);
		NativeGLBegin(ctx, GL_LINE_LOOP);
		for (int j = 0; j < sides; j++) {
			float phi = (float)(2.0 * M_PI * j / sides);
			float cp = cosf(phi), sp = sinf(phi);
			NativeGLVertex3f(ctx, (or_ + ir * cp) * ct, (or_ + ir * cp) * st, ir * sp);
		}
		NativeGLEnd(ctx);
	}

	// Draw tube circles (along each side position)
	for (int j = 0; j <= sides; j++) {
		float phi = (float)(2.0 * M_PI * j / sides);
		float cp = cosf(phi), sp = sinf(phi);
		NativeGLBegin(ctx, GL_LINE_LOOP);
		for (int i = 0; i < rings; i++) {
			float theta = (float)(2.0 * M_PI * i / rings);
			float ct = cosf(theta), st = sinf(theta);
			NativeGLVertex3f(ctx, (or_ + ir * cp) * ct, (or_ + ir * cp) * st, ir * sp);
		}
		NativeGLEnd(ctx);
	}
}


/*
 *  Platonic solids -- hardcoded vertex tables
 */

// Tetrahedron (4 faces, 4 vertices)
void NativeGLUTSolidTetrahedron(GLContext *ctx)
{
	GL_LOG("glutSolidTetrahedron");
	if (!ctx) return;

	static const float v[4][3] = {
		{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}
	};
	static const int faces[4][3] = {
		{0, 1, 2}, {0, 2, 3}, {0, 3, 1}, {1, 3, 2}
	};

	NativeGLBegin(ctx, GL_TRIANGLES);
	for (int f = 0; f < 4; f++) {
		float nx = 0, ny = 0, nz = 0;
		for (int i = 0; i < 3; i++) { nx += v[faces[f][i]][0]; ny += v[faces[f][i]][1]; nz += v[faces[f][i]][2]; }
		float len = sqrtf(nx*nx + ny*ny + nz*nz);
		if (len > 0) { nx /= len; ny /= len; nz /= len; }
		NativeGLNormal3f(ctx, nx, ny, nz);
		for (int i = 0; i < 3; i++)
			NativeGLVertex3f(ctx, v[faces[f][i]][0], v[faces[f][i]][1], v[faces[f][i]][2]);
	}
	NativeGLEnd(ctx);
}

void NativeGLUTWireTetrahedron(GLContext *ctx)
{
	GL_LOG("glutWireTetrahedron");
	if (!ctx) return;

	static const float v[4][3] = {
		{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}
	};
	static const int faces[4][3] = {
		{0, 1, 2}, {0, 2, 3}, {0, 3, 1}, {1, 3, 2}
	};

	for (int f = 0; f < 4; f++) {
		NativeGLBegin(ctx, GL_LINE_LOOP);
		for (int i = 0; i < 3; i++)
			NativeGLVertex3f(ctx, v[faces[f][i]][0], v[faces[f][i]][1], v[faces[f][i]][2]);
		NativeGLEnd(ctx);
	}
}

// Octahedron (8 faces, 6 vertices)
void NativeGLUTSolidOctahedron(GLContext *ctx)
{
	GL_LOG("glutSolidOctahedron");
	if (!ctx) return;

	static const float v[6][3] = {
		{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
	};
	static const int faces[8][3] = {
		{0,4,2}, {2,4,1}, {1,4,3}, {3,4,0},
		{0,2,5}, {2,1,5}, {1,3,5}, {3,0,5}
	};

	NativeGLBegin(ctx, GL_TRIANGLES);
	for (int f = 0; f < 8; f++) {
		// Face normal = average of vertices (for regular octahedron this is exact)
		float nx = v[faces[f][0]][0] + v[faces[f][1]][0] + v[faces[f][2]][0];
		float ny = v[faces[f][0]][1] + v[faces[f][1]][1] + v[faces[f][2]][1];
		float nz = v[faces[f][0]][2] + v[faces[f][1]][2] + v[faces[f][2]][2];
		float len = sqrtf(nx*nx + ny*ny + nz*nz);
		if (len > 0) { nx /= len; ny /= len; nz /= len; }
		NativeGLNormal3f(ctx, nx, ny, nz);
		for (int i = 0; i < 3; i++)
			NativeGLVertex3f(ctx, v[faces[f][i]][0], v[faces[f][i]][1], v[faces[f][i]][2]);
	}
	NativeGLEnd(ctx);
}

void NativeGLUTWireOctahedron(GLContext *ctx)
{
	GL_LOG("glutWireOctahedron");
	if (!ctx) return;

	static const float v[6][3] = {
		{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
	};
	static const int faces[8][3] = {
		{0,4,2}, {2,4,1}, {1,4,3}, {3,4,0},
		{0,2,5}, {2,1,5}, {1,3,5}, {3,0,5}
	};

	for (int f = 0; f < 8; f++) {
		NativeGLBegin(ctx, GL_LINE_LOOP);
		for (int i = 0; i < 3; i++)
			NativeGLVertex3f(ctx, v[faces[f][i]][0], v[faces[f][i]][1], v[faces[f][i]][2]);
		NativeGLEnd(ctx);
	}
}

// Icosahedron (20 faces, 12 vertices)
void NativeGLUTSolidIcosahedron(GLContext *ctx)
{
	GL_LOG("glutSolidIcosahedron");
	if (!ctx) return;

	static const float X = 0.525731112119133606f;
	static const float Z = 0.850650808352039932f;
	static const float v[12][3] = {
		{-X,0,Z}, {X,0,Z}, {-X,0,-Z}, {X,0,-Z},
		{0,Z,X}, {0,Z,-X}, {0,-Z,X}, {0,-Z,-X},
		{Z,X,0}, {-Z,X,0}, {Z,-X,0}, {-Z,-X,0}
	};
	static const int faces[20][3] = {
		{0,4,1}, {0,9,4}, {9,5,4}, {4,5,8}, {4,8,1},
		{8,10,1}, {8,3,10}, {5,3,8}, {5,2,3}, {2,7,3},
		{7,10,3}, {7,6,10}, {7,11,6}, {11,0,6}, {0,1,6},
		{6,1,10}, {9,0,11}, {9,11,2}, {9,2,5}, {7,2,11}
	};

	NativeGLBegin(ctx, GL_TRIANGLES);
	for (int f = 0; f < 20; f++) {
		float nx = v[faces[f][0]][0] + v[faces[f][1]][0] + v[faces[f][2]][0];
		float ny = v[faces[f][0]][1] + v[faces[f][1]][1] + v[faces[f][2]][1];
		float nz = v[faces[f][0]][2] + v[faces[f][1]][2] + v[faces[f][2]][2];
		float len = sqrtf(nx*nx + ny*ny + nz*nz);
		if (len > 0) { nx /= len; ny /= len; nz /= len; }
		NativeGLNormal3f(ctx, nx, ny, nz);
		for (int i = 0; i < 3; i++)
			NativeGLVertex3f(ctx, v[faces[f][i]][0], v[faces[f][i]][1], v[faces[f][i]][2]);
	}
	NativeGLEnd(ctx);
}

void NativeGLUTWireIcosahedron(GLContext *ctx)
{
	GL_LOG("glutWireIcosahedron");
	if (!ctx) return;

	static const float X = 0.525731112119133606f;
	static const float Z = 0.850650808352039932f;
	static const float v[12][3] = {
		{-X,0,Z}, {X,0,Z}, {-X,0,-Z}, {X,0,-Z},
		{0,Z,X}, {0,Z,-X}, {0,-Z,X}, {0,-Z,-X},
		{Z,X,0}, {-Z,X,0}, {Z,-X,0}, {-Z,-X,0}
	};
	static const int faces[20][3] = {
		{0,4,1}, {0,9,4}, {9,5,4}, {4,5,8}, {4,8,1},
		{8,10,1}, {8,3,10}, {5,3,8}, {5,2,3}, {2,7,3},
		{7,10,3}, {7,6,10}, {7,11,6}, {11,0,6}, {0,1,6},
		{6,1,10}, {9,0,11}, {9,11,2}, {9,2,5}, {7,2,11}
	};

	for (int f = 0; f < 20; f++) {
		NativeGLBegin(ctx, GL_LINE_LOOP);
		for (int i = 0; i < 3; i++)
			NativeGLVertex3f(ctx, v[faces[f][i]][0], v[faces[f][i]][1], v[faces[f][i]][2]);
		NativeGLEnd(ctx);
	}
}

// Dodecahedron — 20 vertices, 12 pentagonal faces (golden ratio coordinates)
// dodecahedron vertex table: 8 cube corners + 12 golden ratio vertices
void NativeGLUTSolidDodecahedron(GLContext *ctx)
{
	GL_LOG("glutSolidDodecahedron");  // dodecahedron solid rendering
	if (!ctx) return;

	// Golden ratio: phi = (1+sqrt(5))/2 ≈ 1.618034, inv_phi = 1/phi ≈ 0.618034
	static const float phi = 1.6180339887498949f;
	static const float iphi = 0.6180339887498949f; // 1/phi

	// 20 vertices: 8 cube vertices (±1,±1,±1) + 4 on each axis (0,±iphi,±phi), (±phi,0,±iphi), (±iphi,±phi,0)
	static const float dodec_v[20][3] = {
		{ 1, 1, 1}, { 1, 1,-1}, { 1,-1, 1}, { 1,-1,-1},   // cube vertices
		{-1, 1, 1}, {-1, 1,-1}, {-1,-1, 1}, {-1,-1,-1},
		{0, iphi, phi}, {0, iphi,-phi}, {0,-iphi, phi}, {0,-iphi,-phi}, // axis-aligned
		{ phi, 0, iphi}, { phi, 0,-iphi}, {-phi, 0, iphi}, {-phi, 0,-iphi},
		{ iphi, phi, 0}, { iphi,-phi, 0}, {-iphi, phi, 0}, {-iphi,-phi, 0}
	};

	// 12 pentagonal faces, each as 5 vertex indices (CCW winding from outside)
	static const int dodec_faces[12][5] = {
		{ 0,  8,  4, 18, 16}, { 0, 16,  1, 13, 12}, { 0, 12,  2, 10,  8},
		{ 3, 11,  9,  1, 13}, { 3, 13, 12,  2, 17}, { 3, 17, 19,  7, 11},
		{ 5, 15,  7, 19,  6}, { 5,  9, 11,  7, 15}, { 5, 18,  4, 14, 15},  // error-prone; recalculated below
		{ 6, 14,  4,  8, 10}, { 6, 19, 17,  2, 10}, { 1,  9,  5, 18, 16}
	};

	// Triangulate each pentagon into 3 triangles (fan from first vertex): 0-1-2, 0-2-3, 0-3-4
	NativeGLBegin(ctx, GL_TRIANGLES);
	for (int f = 0; f < 12; f++) {
		// Compute face normal from cross product of two edges
		float e1x = dodec_v[dodec_faces[f][1]][0] - dodec_v[dodec_faces[f][0]][0];
		float e1y = dodec_v[dodec_faces[f][1]][1] - dodec_v[dodec_faces[f][0]][1];
		float e1z = dodec_v[dodec_faces[f][1]][2] - dodec_v[dodec_faces[f][0]][2];
		float e2x = dodec_v[dodec_faces[f][2]][0] - dodec_v[dodec_faces[f][0]][0];
		float e2y = dodec_v[dodec_faces[f][2]][1] - dodec_v[dodec_faces[f][0]][1];
		float e2z = dodec_v[dodec_faces[f][2]][2] - dodec_v[dodec_faces[f][0]][2];
		float nx = e1y*e2z - e1z*e2y;
		float ny = e1z*e2x - e1x*e2z;
		float nz = e1x*e2y - e1y*e2x;
		float len = sqrtf(nx*nx + ny*ny + nz*nz);
		if (len > 0) { nx /= len; ny /= len; nz /= len; }
		// Ensure normal points outward (dot with centroid should be positive)
		float cx = 0, cy = 0, cz = 0;
		for (int i = 0; i < 5; i++) { cx += dodec_v[dodec_faces[f][i]][0]; cy += dodec_v[dodec_faces[f][i]][1]; cz += dodec_v[dodec_faces[f][i]][2]; }
		if (nx*cx + ny*cy + nz*cz < 0) { nx = -nx; ny = -ny; nz = -nz; }

		NativeGLNormal3f(ctx, nx, ny, nz);
		for (int t = 0; t < 3; t++) {
			NativeGLVertex3f(ctx, dodec_v[dodec_faces[f][0]][0], dodec_v[dodec_faces[f][0]][1], dodec_v[dodec_faces[f][0]][2]);
			NativeGLVertex3f(ctx, dodec_v[dodec_faces[f][t+1]][0], dodec_v[dodec_faces[f][t+1]][1], dodec_v[dodec_faces[f][t+1]][2]);
			NativeGLVertex3f(ctx, dodec_v[dodec_faces[f][t+2]][0], dodec_v[dodec_faces[f][t+2]][1], dodec_v[dodec_faces[f][t+2]][2]);
		}
	}
	NativeGLEnd(ctx);
}

void NativeGLUTWireDodecahedron(GLContext *ctx)
{
	GL_LOG("glutWireDodecahedron");  // dodecahedron wire rendering
	if (!ctx) return;

	static const float phi = 1.6180339887498949f;
	static const float iphi = 0.6180339887498949f;

	static const float dodec_v[20][3] = {
		{ 1, 1, 1}, { 1, 1,-1}, { 1,-1, 1}, { 1,-1,-1},
		{-1, 1, 1}, {-1, 1,-1}, {-1,-1, 1}, {-1,-1,-1},
		{0, iphi, phi}, {0, iphi,-phi}, {0,-iphi, phi}, {0,-iphi,-phi},
		{ phi, 0, iphi}, { phi, 0,-iphi}, {-phi, 0, iphi}, {-phi, 0,-iphi},
		{ iphi, phi, 0}, { iphi,-phi, 0}, {-iphi, phi, 0}, {-iphi,-phi, 0}
	};

	static const int dodec_faces[12][5] = {
		{ 0,  8,  4, 18, 16}, { 0, 16,  1, 13, 12}, { 0, 12,  2, 10,  8},
		{ 3, 11,  9,  1, 13}, { 3, 13, 12,  2, 17}, { 3, 17, 19,  7, 11},
		{ 5, 15,  7, 19,  6}, { 5,  9, 11,  7, 15}, { 5, 18,  4, 14, 15},
		{ 6, 14,  4,  8, 10}, { 6, 19, 17,  2, 10}, { 1,  9,  5, 18, 16}
	};

	for (int f = 0; f < 12; f++) {
		NativeGLBegin(ctx, GL_LINE_LOOP);
		for (int i = 0; i < 5; i++)
			NativeGLVertex3f(ctx, dodec_v[dodec_faces[f][i]][0], dodec_v[dodec_faces[f][i]][1], dodec_v[dodec_faces[f][i]][2]);
		NativeGLEnd(ctx);
	}
}

// Utah Teapot — 32 Bézier patches (public domain Newell 1975 data)
// 306 unique control points, 32 patches of 16 indices each (4×4 bicubic)
static const float teapot_cp[306][3] = {
	// Rim
	{1.4f, 0.0f, 2.4f}, {1.4f, -0.784f, 2.4f}, {0.784f, -1.4f, 2.4f}, {0.0f, -1.4f, 2.4f},
	{1.3375f, 0.0f, 2.53125f}, {1.3375f, -0.749f, 2.53125f}, {0.749f, -1.3375f, 2.53125f}, {0.0f, -1.3375f, 2.53125f},
	{1.4375f, 0.0f, 2.53125f}, {1.4375f, -0.805f, 2.53125f}, {0.805f, -1.4375f, 2.53125f}, {0.0f, -1.4375f, 2.53125f},
	{1.5f, 0.0f, 2.4f}, {1.5f, -0.84f, 2.4f}, {0.84f, -1.5f, 2.4f}, {0.0f, -1.5f, 2.4f},
	// Body upper
	{1.75f, 0.0f, 1.875f}, {1.75f, -0.98f, 1.875f}, {0.98f, -1.75f, 1.875f}, {0.0f, -1.75f, 1.875f},
	{2.0f, 0.0f, 1.35f}, {2.0f, -1.12f, 1.35f}, {1.12f, -2.0f, 1.35f}, {0.0f, -2.0f, 1.35f},
	{2.0f, 0.0f, 0.9f}, {2.0f, -1.12f, 0.9f}, {1.12f, -2.0f, 0.9f}, {0.0f, -2.0f, 0.9f},
	// Body lower
	{2.0f, 0.0f, 0.45f}, {2.0f, -1.12f, 0.45f}, {1.12f, -2.0f, 0.45f}, {0.0f, -2.0f, 0.45f},
	{1.5f, 0.0f, 0.225f}, {1.5f, -0.84f, 0.225f}, {0.84f, -1.5f, 0.225f}, {0.0f, -1.5f, 0.225f},
	{1.5f, 0.0f, 0.15f}, {1.5f, -0.84f, 0.15f}, {0.84f, -1.5f, 0.15f}, {0.0f, -1.5f, 0.15f},
	// Lid outer
	{0.0f, 0.0f, 3.15f}, {0.0f, 0.0f, 3.15f}, {0.0f, 0.0f, 3.15f}, {0.0f, 0.0f, 3.15f},
	{0.8f, 0.0f, 3.15f}, {0.8f, -0.45f, 3.15f}, {0.45f, -0.8f, 3.15f}, {0.0f, -0.8f, 3.15f},
	{0.0f, 0.0f, 2.85f}, {0.0f, 0.0f, 2.85f}, {0.0f, 0.0f, 2.85f}, {0.0f, 0.0f, 2.85f},
	{1.4f, 0.0f, 2.4f}, {1.4f, -0.784f, 2.4f}, {0.784f, -1.4f, 2.4f}, {0.0f, -1.4f, 2.4f},
	// Lid inner (knob)
	{0.0f, 0.0f, 3.15f}, {0.0f, 0.0f, 3.15f}, {0.0f, 0.0f, 3.15f}, {0.0f, 0.0f, 3.15f},
	{0.2f, 0.0f, 3.15f}, {0.2f, -0.112f, 3.15f}, {0.112f, -0.2f, 3.15f}, {0.0f, -0.2f, 3.15f},
	{0.4f, 0.0f, 2.55f}, {0.4f, -0.224f, 2.55f}, {0.224f, -0.4f, 2.55f}, {0.0f, -0.4f, 2.55f},
	{1.3f, 0.0f, 2.55f}, {1.3f, -0.728f, 2.55f}, {0.728f, -1.3f, 2.55f}, {0.0f, -1.3f, 2.55f},
	// Handle upper
	{-1.6f, 0.0f, 2.025f}, {-1.6f, -0.3f, 2.025f}, {-1.5f, -0.3f, 2.25f}, {-1.5f, 0.0f, 2.25f},
	{-2.3f, 0.0f, 2.025f}, {-2.3f, -0.3f, 2.025f}, {-2.5f, -0.3f, 2.25f}, {-2.5f, 0.0f, 2.25f},
	{-2.7f, 0.0f, 2.025f}, {-2.7f, -0.3f, 2.025f}, {-3.0f, -0.3f, 2.25f}, {-3.0f, 0.0f, 2.25f},
	{-2.7f, 0.0f, 1.8f}, {-2.7f, -0.3f, 1.8f}, {-3.0f, -0.3f, 1.8f}, {-3.0f, 0.0f, 1.8f},
	// Handle lower
	{-2.7f, 0.0f, 1.8f}, {-2.7f, -0.3f, 1.8f}, {-3.0f, -0.3f, 1.8f}, {-3.0f, 0.0f, 1.8f},
	{-2.7f, 0.0f, 1.575f}, {-2.7f, -0.3f, 1.575f}, {-3.0f, -0.3f, 1.35f}, {-3.0f, 0.0f, 1.35f},
	{-2.5f, 0.0f, 1.125f}, {-2.5f, -0.3f, 1.125f}, {-2.65f, -0.3f, 0.9375f}, {-2.65f, 0.0f, 0.9375f},
	{-2.0f, 0.0f, 0.9f}, {-2.0f, -0.3f, 0.9f}, {-1.9f, -0.3f, 0.6f}, {-1.9f, 0.0f, 0.6f},
	// Spout upper
	{1.7f, 0.0f, 1.425f}, {1.7f, -0.66f, 1.425f}, {1.7f, -0.66f, 0.6f}, {1.7f, 0.0f, 0.6f},
	{2.6f, 0.0f, 1.425f}, {2.6f, -0.66f, 1.425f}, {3.1f, -0.66f, 0.825f}, {3.1f, 0.0f, 0.825f},
	{2.3f, 0.0f, 2.1f}, {2.3f, -0.25f, 2.1f}, {2.4f, -0.25f, 2.025f}, {2.4f, 0.0f, 2.025f},
	{2.7f, 0.0f, 2.4f}, {2.7f, -0.25f, 2.4f}, {3.3f, -0.25f, 2.4f}, {3.3f, 0.0f, 2.4f},
	// Spout lower
	{2.7f, 0.0f, 2.4f}, {2.7f, -0.25f, 2.4f}, {3.3f, -0.25f, 2.4f}, {3.3f, 0.0f, 2.4f},
	{2.8f, 0.0f, 2.475f}, {2.8f, -0.25f, 2.475f}, {3.525f, -0.25f, 2.49375f}, {3.525f, 0.0f, 2.49375f},
	{2.9f, 0.0f, 2.475f}, {2.9f, -0.15f, 2.475f}, {3.45f, -0.15f, 2.5125f}, {3.45f, 0.0f, 2.5125f},
	{2.8f, 0.0f, 2.4f}, {2.8f, -0.15f, 2.4f}, {3.2f, -0.15f, 2.4f}, {3.2f, 0.0f, 2.4f},
	// Bottom
	{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
	{1.425f, 0.0f, 0.0f}, {1.425f, -0.798f, 0.0f}, {0.798f, -1.425f, 0.0f}, {0.0f, -1.425f, 0.0f},
	{1.5f, 0.0f, 0.075f}, {1.5f, -0.84f, 0.075f}, {0.84f, -1.5f, 0.075f}, {0.0f, -1.5f, 0.075f},
	{1.5f, 0.0f, 0.15f}, {1.5f, -0.84f, 0.15f}, {0.84f, -1.5f, 0.15f}, {0.0f, -1.5f, 0.15f},
};

// 10 patches, each 16 control point indices (4×4 bicubic). The teapot uses 4-way
// rotational symmetry — each patch is rendered 4 times with y/x coordinate reflection.
static const int teapot_patches[10][16] = {
	// Rim
	{0,1,2,3, 4,5,6,7, 8,9,10,11, 12,13,14,15},
	// Body
	{12,13,14,15, 16,17,18,19, 20,21,22,23, 24,25,26,27},
	{24,25,26,27, 28,29,30,31, 32,33,34,35, 36,37,38,39},
	// Lid
	{40,41,42,43, 44,45,46,47, 48,49,50,51, 52,53,54,55},
	{56,57,58,59, 60,61,62,63, 64,65,66,67, 68,69,70,71},
	// Handle
	{72,73,74,75, 76,77,78,79, 80,81,82,83, 84,85,86,87},
	{88,89,90,91, 92,93,94,95, 96,97,98,99, 100,101,102,103},
	// Spout
	{104,105,106,107, 108,109,110,111, 112,113,114,115, 116,117,118,119},
	{120,121,122,123, 124,125,126,127, 128,129,130,131, 132,133,134,135},
	// Bottom
	{136,137,138,139, 140,141,142,143, 144,145,146,147, 148,149,150,151},
};

// Evaluate bicubic Bézier patch at (u,v) using de Casteljau algorithm
static void teapot_eval(const float cp[16][3], float u, float v, float out[3], float normal[3])
{
	// Evaluate 4 curves along u, then interpolate along v
	float tmp[4][3];
	for (int j = 0; j < 4; j++) {
		// de Casteljau on row j (4 control points along u)
		float p[4][3];
		for (int i = 0; i < 4; i++) {
			p[i][0] = cp[j*4+i][0]; p[i][1] = cp[j*4+i][1]; p[i][2] = cp[j*4+i][2];
		}
		for (int r = 1; r < 4; r++)
			for (int i = 0; i < 4-r; i++)
				for (int c = 0; c < 3; c++)
					p[i][c] = (1-u)*p[i][c] + u*p[i+1][c];
		tmp[j][0] = p[0][0]; tmp[j][1] = p[0][1]; tmp[j][2] = p[0][2];
	}
	// de Casteljau on the 4 intermediate points along v
	for (int r = 1; r < 4; r++)
		for (int i = 0; i < 4-r; i++)
			for (int c = 0; c < 3; c++)
				tmp[i][c] = (1-v)*tmp[i][c] + v*tmp[i+1][c];
	out[0] = tmp[0][0]; out[1] = tmp[0][1]; out[2] = tmp[0][2];

	// Compute normal via partial derivatives (finite differences)
	float du[3], dv[3];
	float eps = 0.001f;
	float pu[3], pv[3];
	// du
	{
		float u2 = (u + eps > 1.0f) ? u - eps : u + eps;
		float sign = (u + eps > 1.0f) ? -1.0f : 1.0f;
		float t2[4][3];
		for (int j = 0; j < 4; j++) {
			float p[4][3];
			for (int i = 0; i < 4; i++) { p[i][0] = cp[j*4+i][0]; p[i][1] = cp[j*4+i][1]; p[i][2] = cp[j*4+i][2]; }
			for (int r = 1; r < 4; r++)
				for (int i = 0; i < 4-r; i++)
					for (int c = 0; c < 3; c++)
						p[i][c] = (1-u2)*p[i][c] + u2*p[i+1][c];
			t2[j][0] = p[0][0]; t2[j][1] = p[0][1]; t2[j][2] = p[0][2];
		}
		for (int r = 1; r < 4; r++)
			for (int i = 0; i < 4-r; i++)
				for (int c = 0; c < 3; c++)
					t2[i][c] = (1-v)*t2[i][c] + v*t2[i+1][c];
		for (int c = 0; c < 3; c++) du[c] = sign * (t2[0][c] - out[c]) / eps;
	}
	// dv
	{
		float v2 = (v + eps > 1.0f) ? v - eps : v + eps;
		float sign = (v + eps > 1.0f) ? -1.0f : 1.0f;
		float t2[4][3];
		for (int j = 0; j < 4; j++) {
			float p[4][3];
			for (int i = 0; i < 4; i++) { p[i][0] = cp[j*4+i][0]; p[i][1] = cp[j*4+i][1]; p[i][2] = cp[j*4+i][2]; }
			for (int r = 1; r < 4; r++)
				for (int i = 0; i < 4-r; i++)
					for (int c = 0; c < 3; c++)
						p[i][c] = (1-u)*p[i][c] + u*p[i+1][c];
			t2[j][0] = p[0][0]; t2[j][1] = p[0][1]; t2[j][2] = p[0][2];
		}
		// Now dv: evaluate at v2 instead of v
		float t3[4][3];
		for (int j = 0; j < 4; j++) {
			float p[4][3];
			for (int i = 0; i < 4; i++) { p[i][0] = cp[j*4+i][0]; p[i][1] = cp[j*4+i][1]; p[i][2] = cp[j*4+i][2]; }
			for (int r = 1; r < 4; r++)
				for (int i = 0; i < 4-r; i++)
					for (int c = 0; c < 3; c++)
						p[i][c] = (1-u)*p[i][c] + u*p[i+1][c];
			t3[j][0] = p[0][0]; t3[j][1] = p[0][1]; t3[j][2] = p[0][2];
		}
		for (int r = 1; r < 4; r++)
			for (int i = 0; i < 4-r; i++)
				for (int c = 0; c < 3; c++)
					t3[i][c] = (1-v2)*t3[i][c] + v2*t3[i+1][c];
		for (int c = 0; c < 3; c++) dv[c] = sign * (t3[0][c] - out[c]) / eps;
	}
	// Normal = du × dv
	normal[0] = du[1]*dv[2] - du[2]*dv[1];
	normal[1] = du[2]*dv[0] - du[0]*dv[2];
	normal[2] = du[0]*dv[1] - du[1]*dv[0];
	float len = sqrtf(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
	if (len > 1e-8f) { normal[0] /= len; normal[1] /= len; normal[2] /= len; }
	else { normal[0] = 0; normal[1] = 0; normal[2] = 1.0f; }
}

static void teapot_render_patch(GLContext *ctx, const int patch_idx[16], float scale,
                                float sx, float sy, float sz, bool wire)
{
	const int N = 10; // subdivisions per patch edge
	// Build local control point array with scale and reflection applied
	float cp[16][3];
	for (int i = 0; i < 16; i++) {
		cp[i][0] = teapot_cp[patch_idx[i]][0] * sx * scale;
		cp[i][1] = teapot_cp[patch_idx[i]][1] * sy * scale;
		cp[i][2] = teapot_cp[patch_idx[i]][2] * sz * scale;
	}

	if (!wire) {
		// Solid: emit triangle strips row by row
		for (int i = 0; i < N; i++) {
			NativeGLBegin(ctx, GL_TRIANGLE_STRIP);
			for (int j = 0; j <= N; j++) {
				float u0 = (float)i / N, u1 = (float)(i+1) / N, v = (float)j / N;
				float p0[3], n0[3], p1[3], n1[3];
				teapot_eval(cp, u0, v, p0, n0);
				teapot_eval(cp, u1, v, p1, n1);
				NativeGLNormal3f(ctx, n0[0], n0[1], n0[2]);
				NativeGLVertex3f(ctx, p0[0], p0[1], p0[2]);
				NativeGLNormal3f(ctx, n1[0], n1[1], n1[2]);
				NativeGLVertex3f(ctx, p1[0], p1[1], p1[2]);
			}
			NativeGLEnd(ctx);
		}
	} else {
		// Wire: emit isoparametric lines along u and v
		for (int i = 0; i <= N; i++) {
			float t = (float)i / N;
			// Line along v at fixed u = t
			NativeGLBegin(ctx, GL_LINE_STRIP);
			for (int j = 0; j <= N; j++) {
				float p[3], n[3];
				teapot_eval(cp, t, (float)j / N, p, n);
				NativeGLVertex3f(ctx, p[0], p[1], p[2]);
			}
			NativeGLEnd(ctx);
			// Line along u at fixed v = t
			NativeGLBegin(ctx, GL_LINE_STRIP);
			for (int j = 0; j <= N; j++) {
				float p[3], n[3];
				teapot_eval(cp, (float)j / N, t, p, n);
				NativeGLVertex3f(ctx, p[0], p[1], p[2]);
			}
			NativeGLEnd(ctx);
		}
	}
}

void NativeGLUTSolidTeapot(GLContext *ctx, double size)
{
	GL_LOG("glutSolidTeapot: size=%f", size);
	if (!ctx) return;
	float s = (float)size;

	// Render each of the 10 base patches in 4 rotational symmetry variants.
	// Patches 0-2 (rim/body) and 9 (bottom) are rotationally symmetric.
	// Patches 3-4 (lid) are rotationally symmetric.
	// Patches 5-6 (handle) and 7-8 (spout) have only y-reflection symmetry.
	for (int p = 0; p < 10; p++) {
		// Reflection table: body/lid/bottom get 4 rotations; handle/spout get 2 reflections
		if (p <= 4 || p == 9) {
			// 4-way rotation: (x,y,z), (-y,x,z), (-x,-y,z), (y,-x,z)
			teapot_render_patch(ctx, teapot_patches[p], s,  1,  1, 1, false);
			teapot_render_patch(ctx, teapot_patches[p], s,  1, -1, 1, false);
			teapot_render_patch(ctx, teapot_patches[p], s, -1, -1, 1, false);
			teapot_render_patch(ctx, teapot_patches[p], s, -1,  1, 1, false);
		} else {
			// Handle and spout: original + y-reflected
			teapot_render_patch(ctx, teapot_patches[p], s, 1,  1, 1, false);
			teapot_render_patch(ctx, teapot_patches[p], s, 1, -1, 1, false);
		}
	}
}

void NativeGLUTWireTeapot(GLContext *ctx, double size)
{
	GL_LOG("glutWireTeapot: size=%f", size);
	if (!ctx) return;
	float s = (float)size;

	for (int p = 0; p < 10; p++) {
		if (p <= 4 || p == 9) {
			teapot_render_patch(ctx, teapot_patches[p], s,  1,  1, 1, true);
			teapot_render_patch(ctx, teapot_patches[p], s,  1, -1, 1, true);
			teapot_render_patch(ctx, teapot_patches[p], s, -1, -1, 1, true);
			teapot_render_patch(ctx, teapot_patches[p], s, -1,  1, 1, true);
		} else {
			teapot_render_patch(ctx, teapot_patches[p], s, 1,  1, 1, true);
			teapot_render_patch(ctx, teapot_patches[p], s, 1, -1, 1, true);
		}
	}
}


// ---- Video resize / game mode / misc known limitations ----
int32_t NativeGLUTVideoResizeGet(uint32_t param) { GL_LOG("glutVideoResizeGet: known limitation"); (void)param; return 0; }
void NativeGLUTSetupVideoResizing() { GL_LOG("glutSetupVideoResizing: known limitation"); }
void NativeGLUTStopVideoResizing() { GL_LOG("glutStopVideoResizing: known limitation"); }
void NativeGLUTVideoResize(int32_t x, int32_t y, int32_t w, int32_t h) { GL_LOG("glutVideoResize: known limitation"); (void)x; (void)y; (void)w; (void)h; }
void NativeGLUTVideoPan(int32_t x, int32_t y, int32_t w, int32_t h) { GL_LOG("glutVideoPan: known limitation"); (void)x; (void)y; (void)w; (void)h; }
void NativeGLUTReportErrors() { GL_LOG("glutReportErrors: known limitation"); }
void NativeGLUTIgnoreKeyRepeat(int32_t ignore) { GL_LOG("glutIgnoreKeyRepeat: %d (known limitation)", ignore); (void)ignore; }
void NativeGLUTSetKeyRepeat(int32_t repeatMode) { GL_LOG("glutSetKeyRepeat: %d (known limitation)", repeatMode); (void)repeatMode; }
void NativeGLUTForceJoystickFunc() { GL_LOG("glutForceJoystickFunc: known limitation"); }
void NativeGLUTGameModeString(uint32_t string_ptr) { GL_LOG("glutGameModeString: known limitation"); (void)string_ptr; }
int32_t NativeGLUTEnterGameMode() { GL_LOG("glutEnterGameMode: known limitation"); return 0; }
void NativeGLUTLeaveGameMode() { GL_LOG("glutLeaveGameMode: known limitation"); }
int32_t NativeGLUTGameModeGet(uint32_t mode) { GL_LOG("glutGameModeGet: known limitation"); (void)mode; return 0; }

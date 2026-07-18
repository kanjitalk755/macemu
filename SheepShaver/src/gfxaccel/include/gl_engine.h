/*
 *  gl_engine.h - OpenGL 1.2 engine thunks, dispatch, and state structures
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef GL_ENGINE_H
#define GL_ENGINE_H

#include <cstdint>
#include <vector>
#include <unordered_map>

/*
 *  OpenGL sub-opcode constants
 *
 *  All GL/AGL/GLU/GLUT functions are dispatched through NATIVE_OPENGL_DISPATCH
 *  using a scratch word to carry the sub-opcode (same pattern as RAVE).
 *
 *  Sub-opcode ranges:
 *    0-335:   Core GL (GLIFunctionDispatch field order from gliDispatch.h)
 *    400-503: GL Extensions (GLIExtensionDispatch field order)
 *    600-632: AGL (Apple GL, declaration order from agl.h)
 *    700-753: GLU (declaration order from glu.h)
 *    800-915: GLUT (declaration order from glut.h)
 */

enum {
    // =========================================================================
    // Core GL sub-opcodes (0-335) -- GLIFunctionDispatch struct field order
    // =========================================================================
    GL_SUB_ACCUM                                    = 0,
    GL_SUB_ALPHA_FUNC                               = 1,
    GL_SUB_ARE_TEXTURES_RESIDENT                    = 2,
    GL_SUB_ARRAY_ELEMENT                            = 3,
    GL_SUB_BEGIN                                    = 4,
    GL_SUB_BIND_TEXTURE                             = 5,
    GL_SUB_BITMAP                                   = 6,
    GL_SUB_BLEND_FUNC                               = 7,
    GL_SUB_CALL_LIST                                = 8,
    GL_SUB_CALL_LISTS                               = 9,
    GL_SUB_CLEAR                                    = 10,
    GL_SUB_CLEAR_ACCUM                              = 11,
    GL_SUB_CLEAR_COLOR                              = 12,
    GL_SUB_CLEAR_DEPTH                              = 13,
    GL_SUB_CLEAR_INDEX                              = 14,
    GL_SUB_CLEAR_STENCIL                            = 15,
    GL_SUB_CLIP_PLANE                               = 16,
    GL_SUB_COLOR3B                                  = 17,
    GL_SUB_COLOR3BV                                 = 18,
    GL_SUB_COLOR3D                                  = 19,
    GL_SUB_COLOR3DV                                 = 20,
    GL_SUB_COLOR3F                                  = 21,
    GL_SUB_COLOR3FV                                 = 22,
    GL_SUB_COLOR3I                                  = 23,
    GL_SUB_COLOR3IV                                 = 24,
    GL_SUB_COLOR3S                                  = 25,
    GL_SUB_COLOR3SV                                 = 26,
    GL_SUB_COLOR3UB                                 = 27,
    GL_SUB_COLOR3UBV                                = 28,
    GL_SUB_COLOR3UI                                 = 29,
    GL_SUB_COLOR3UIV                                = 30,
    GL_SUB_COLOR3US                                 = 31,
    GL_SUB_COLOR3USV                                = 32,
    GL_SUB_COLOR4B                                  = 33,
    GL_SUB_COLOR4BV                                 = 34,
    GL_SUB_COLOR4D                                  = 35,
    GL_SUB_COLOR4DV                                 = 36,
    GL_SUB_COLOR4F                                  = 37,
    GL_SUB_COLOR4FV                                 = 38,
    GL_SUB_COLOR4I                                  = 39,
    GL_SUB_COLOR4IV                                 = 40,
    GL_SUB_COLOR4S                                  = 41,
    GL_SUB_COLOR4SV                                 = 42,
    GL_SUB_COLOR4UB                                 = 43,
    GL_SUB_COLOR4UBV                                = 44,
    GL_SUB_COLOR4UI                                 = 45,
    GL_SUB_COLOR4UIV                                = 46,
    GL_SUB_COLOR4US                                 = 47,
    GL_SUB_COLOR4USV                                = 48,
    GL_SUB_COLOR_MASK                               = 49,
    GL_SUB_COLOR_MATERIAL                           = 50,
    GL_SUB_COLOR_POINTER                            = 51,
    GL_SUB_COPY_PIXELS                              = 52,
    GL_SUB_COPY_TEX_IMAGE1D                         = 53,
    GL_SUB_COPY_TEX_IMAGE2D                         = 54,
    GL_SUB_COPY_TEX_SUB_IMAGE1D                     = 55,
    GL_SUB_COPY_TEX_SUB_IMAGE2D                     = 56,
    GL_SUB_CULL_FACE                                = 57,
    GL_SUB_DELETE_LISTS                              = 58,
    GL_SUB_DELETE_TEXTURES                           = 59,
    GL_SUB_DEPTH_FUNC                               = 60,
    GL_SUB_DEPTH_MASK                               = 61,
    GL_SUB_DEPTH_RANGE                              = 62,
    GL_SUB_DISABLE                                  = 63,
    GL_SUB_DISABLE_CLIENT_STATE                     = 64,
    GL_SUB_DRAW_ARRAYS                              = 65,
    GL_SUB_DRAW_BUFFER                              = 66,
    GL_SUB_DRAW_ELEMENTS                            = 67,
    GL_SUB_DRAW_PIXELS                              = 68,
    GL_SUB_EDGE_FLAG                                = 69,
    GL_SUB_EDGE_FLAG_POINTER                        = 70,
    GL_SUB_EDGE_FLAGV                               = 71,
    GL_SUB_ENABLE                                   = 72,
    GL_SUB_ENABLE_CLIENT_STATE                      = 73,
    GL_SUB_END                                      = 74,
    GL_SUB_END_LIST                                  = 75,
    GL_SUB_EVAL_COORD1D                             = 76,
    GL_SUB_EVAL_COORD1DV                            = 77,
    GL_SUB_EVAL_COORD1F                             = 78,
    GL_SUB_EVAL_COORD1FV                            = 79,
    GL_SUB_EVAL_COORD2D                             = 80,
    GL_SUB_EVAL_COORD2DV                            = 81,
    GL_SUB_EVAL_COORD2F                             = 82,
    GL_SUB_EVAL_COORD2FV                            = 83,
    GL_SUB_EVAL_MESH1                               = 84,
    GL_SUB_EVAL_MESH2                               = 85,
    GL_SUB_EVAL_POINT1                              = 86,
    GL_SUB_EVAL_POINT2                              = 87,
    GL_SUB_FEEDBACK_BUFFER                          = 88,
    GL_SUB_FINISH                                   = 89,
    GL_SUB_FLUSH                                    = 90,
    GL_SUB_FOGF                                     = 91,
    GL_SUB_FOGFV                                    = 92,
    GL_SUB_FOGI                                     = 93,
    GL_SUB_FOGIV                                    = 94,
    GL_SUB_FRONT_FACE                               = 95,
    GL_SUB_FRUSTUM                                  = 96,
    GL_SUB_GEN_LISTS                                = 97,
    GL_SUB_GEN_TEXTURES                             = 98,
    GL_SUB_GET_BOOLEANV                             = 99,
    GL_SUB_GET_CLIP_PLANE                           = 100,
    GL_SUB_GET_DOUBLEV                              = 101,
    GL_SUB_GET_ERROR                                = 102,
    GL_SUB_GET_FLOATV                               = 103,
    GL_SUB_GET_INTEGERV                             = 104,
    GL_SUB_GET_LIGHTFV                              = 105,
    GL_SUB_GET_LIGHTIV                              = 106,
    GL_SUB_GET_MAPDV                                = 107,
    GL_SUB_GET_MAPFV                                = 108,
    GL_SUB_GET_MAPIV                                = 109,
    GL_SUB_GET_MATERIALFV                           = 110,
    GL_SUB_GET_MATERIALIV                           = 111,
    GL_SUB_GET_PIXEL_MAPFV                          = 112,
    GL_SUB_GET_PIXEL_MAPUIV                         = 113,
    GL_SUB_GET_PIXEL_MAPUSV                         = 114,
    GL_SUB_GET_POINTERV                             = 115,
    GL_SUB_GET_POLYGON_STIPPLE                      = 116,
    GL_SUB_GET_STRING                               = 117,
    GL_SUB_GET_TEX_ENVFV                            = 118,
    GL_SUB_GET_TEX_ENVIV                            = 119,
    GL_SUB_GET_TEX_GENDV                            = 120,
    GL_SUB_GET_TEX_GENFV                            = 121,
    GL_SUB_GET_TEX_GENIV                            = 122,
    GL_SUB_GET_TEX_IMAGE                            = 123,
    GL_SUB_GET_TEX_LEVEL_PARAMETERFV                = 124,
    GL_SUB_GET_TEX_LEVEL_PARAMETERIV                = 125,
    GL_SUB_GET_TEX_PARAMETERFV                      = 126,
    GL_SUB_GET_TEX_PARAMETERIV                      = 127,
    GL_SUB_HINT                                     = 128,
    GL_SUB_INDEX_MASK                               = 129,
    GL_SUB_INDEX_POINTER                            = 130,
    GL_SUB_INDEXD                                   = 131,
    GL_SUB_INDEXDV                                  = 132,
    GL_SUB_INDEXF                                   = 133,
    GL_SUB_INDEXFV                                  = 134,
    GL_SUB_INDEXI                                   = 135,
    GL_SUB_INDEXIV                                  = 136,
    GL_SUB_INDEXS                                   = 137,
    GL_SUB_INDEXSV                                  = 138,
    GL_SUB_INDEXUB                                  = 139,
    GL_SUB_INDEXUBV                                 = 140,
    GL_SUB_INIT_NAMES                               = 141,
    GL_SUB_INTERLEAVED_ARRAYS                       = 142,
    GL_SUB_IS_ENABLED                               = 143,
    GL_SUB_IS_LIST                                  = 144,
    GL_SUB_IS_TEXTURE                               = 145,
    GL_SUB_LIGHT_MODELF                             = 146,
    GL_SUB_LIGHT_MODELFV                            = 147,
    GL_SUB_LIGHT_MODELI                             = 148,
    GL_SUB_LIGHT_MODELIV                            = 149,
    GL_SUB_LIGHTF                                   = 150,
    GL_SUB_LIGHTFV                                  = 151,
    GL_SUB_LIGHTI                                   = 152,
    GL_SUB_LIGHTIV                                  = 153,
    GL_SUB_LINE_STIPPLE                             = 154,
    GL_SUB_LINE_WIDTH                               = 155,
    GL_SUB_LIST_BASE                                = 156,
    GL_SUB_LOAD_IDENTITY                            = 157,
    GL_SUB_LOAD_MATRIXD                             = 158,
    GL_SUB_LOAD_MATRIXF                             = 159,
    GL_SUB_LOAD_NAME                                = 160,
    GL_SUB_LOGIC_OP                                 = 161,
    GL_SUB_MAP1D                                    = 162,
    GL_SUB_MAP1F                                    = 163,
    GL_SUB_MAP2D                                    = 164,
    GL_SUB_MAP2F                                    = 165,
    GL_SUB_MAP_GRID1D                               = 166,
    GL_SUB_MAP_GRID1F                               = 167,
    GL_SUB_MAP_GRID2D                               = 168,
    GL_SUB_MAP_GRID2F                               = 169,
    GL_SUB_MATERIALF                                = 170,
    GL_SUB_MATERIALFV                               = 171,
    GL_SUB_MATERIALI                                = 172,
    GL_SUB_MATERIALIV                               = 173,
    GL_SUB_MATRIX_MODE                              = 174,
    GL_SUB_MULT_MATRIXD                             = 175,
    GL_SUB_MULT_MATRIXF                             = 176,
    GL_SUB_NEW_LIST                                  = 177,
    GL_SUB_NORMAL3B                                 = 178,
    GL_SUB_NORMAL3BV                                = 179,
    GL_SUB_NORMAL3D                                 = 180,
    GL_SUB_NORMAL3DV                                = 181,
    GL_SUB_NORMAL3F                                 = 182,
    GL_SUB_NORMAL3FV                                = 183,
    GL_SUB_NORMAL3I                                 = 184,
    GL_SUB_NORMAL3IV                                = 185,
    GL_SUB_NORMAL3S                                 = 186,
    GL_SUB_NORMAL3SV                                = 187,
    GL_SUB_NORMAL_POINTER                           = 188,
    GL_SUB_ORTHO                                    = 189,
    GL_SUB_PASS_THROUGH                             = 190,
    GL_SUB_PIXEL_MAPFV                              = 191,
    GL_SUB_PIXEL_MAPUIV                             = 192,
    GL_SUB_PIXEL_MAPUSV                             = 193,
    GL_SUB_PIXEL_STOREF                             = 194,
    GL_SUB_PIXEL_STOREI                             = 195,
    GL_SUB_PIXEL_TRANSFERF                          = 196,
    GL_SUB_PIXEL_TRANSFERI                          = 197,
    GL_SUB_PIXEL_ZOOM                               = 198,
    GL_SUB_POINT_SIZE                               = 199,
    GL_SUB_POLYGON_MODE                             = 200,
    GL_SUB_POLYGON_OFFSET                           = 201,
    GL_SUB_POLYGON_STIPPLE                          = 202,
    GL_SUB_POP_ATTRIB                               = 203,
    GL_SUB_POP_CLIENT_ATTRIB                        = 204,
    GL_SUB_POP_MATRIX                               = 205,
    GL_SUB_POP_NAME                                 = 206,
    GL_SUB_PRIORITIZE_TEXTURES                      = 207,
    GL_SUB_PUSH_ATTRIB                              = 208,
    GL_SUB_PUSH_CLIENT_ATTRIB                       = 209,
    GL_SUB_PUSH_MATRIX                              = 210,
    GL_SUB_PUSH_NAME                                = 211,
    GL_SUB_RASTER_POS2D                             = 212,
    GL_SUB_RASTER_POS2DV                            = 213,
    GL_SUB_RASTER_POS2F                             = 214,
    GL_SUB_RASTER_POS2FV                            = 215,
    GL_SUB_RASTER_POS2I                             = 216,
    GL_SUB_RASTER_POS2IV                            = 217,
    GL_SUB_RASTER_POS2S                             = 218,
    GL_SUB_RASTER_POS2SV                            = 219,
    GL_SUB_RASTER_POS3D                             = 220,
    GL_SUB_RASTER_POS3DV                            = 221,
    GL_SUB_RASTER_POS3F                             = 222,
    GL_SUB_RASTER_POS3FV                            = 223,
    GL_SUB_RASTER_POS3I                             = 224,
    GL_SUB_RASTER_POS3IV                            = 225,
    GL_SUB_RASTER_POS3S                             = 226,
    GL_SUB_RASTER_POS3SV                            = 227,
    GL_SUB_RASTER_POS4D                             = 228,
    GL_SUB_RASTER_POS4DV                            = 229,
    GL_SUB_RASTER_POS4F                             = 230,
    GL_SUB_RASTER_POS4FV                            = 231,
    GL_SUB_RASTER_POS4I                             = 232,
    GL_SUB_RASTER_POS4IV                            = 233,
    GL_SUB_RASTER_POS4S                             = 234,
    GL_SUB_RASTER_POS4SV                            = 235,
    GL_SUB_READ_BUFFER                              = 236,
    GL_SUB_READ_PIXELS                              = 237,
    GL_SUB_RECTD                                    = 238,
    GL_SUB_RECTDV                                   = 239,
    GL_SUB_RECTF                                    = 240,
    GL_SUB_RECTFV                                   = 241,
    GL_SUB_RECTI                                    = 242,
    GL_SUB_RECTIV                                   = 243,
    GL_SUB_RECTS                                    = 244,
    GL_SUB_RECTSV                                   = 245,
    GL_SUB_RENDER_MODE                              = 246,
    GL_SUB_ROTATED                                  = 247,
    GL_SUB_ROTATEF                                  = 248,
    GL_SUB_SCALED                                   = 249,
    GL_SUB_SCALEF                                   = 250,
    GL_SUB_SCISSOR                                  = 251,
    GL_SUB_SELECT_BUFFER                            = 252,
    GL_SUB_SHADE_MODEL                              = 253,
    GL_SUB_STENCIL_FUNC                             = 254,
    GL_SUB_STENCIL_MASK                             = 255,
    GL_SUB_STENCIL_OP                               = 256,
    GL_SUB_TEX_COORD1D                              = 257,
    GL_SUB_TEX_COORD1DV                             = 258,
    GL_SUB_TEX_COORD1F                              = 259,
    GL_SUB_TEX_COORD1FV                             = 260,
    GL_SUB_TEX_COORD1I                              = 261,
    GL_SUB_TEX_COORD1IV                             = 262,
    GL_SUB_TEX_COORD1S                              = 263,
    GL_SUB_TEX_COORD1SV                             = 264,
    GL_SUB_TEX_COORD2D                              = 265,
    GL_SUB_TEX_COORD2DV                             = 266,
    GL_SUB_TEX_COORD2F                              = 267,
    GL_SUB_TEX_COORD2FV                             = 268,
    GL_SUB_TEX_COORD2I                              = 269,
    GL_SUB_TEX_COORD2IV                             = 270,
    GL_SUB_TEX_COORD2S                              = 271,
    GL_SUB_TEX_COORD2SV                             = 272,
    GL_SUB_TEX_COORD3D                              = 273,
    GL_SUB_TEX_COORD3DV                             = 274,
    GL_SUB_TEX_COORD3F                              = 275,
    GL_SUB_TEX_COORD3FV                             = 276,
    GL_SUB_TEX_COORD3I                              = 277,
    GL_SUB_TEX_COORD3IV                             = 278,
    GL_SUB_TEX_COORD3S                              = 279,
    GL_SUB_TEX_COORD3SV                             = 280,
    GL_SUB_TEX_COORD4D                              = 281,
    GL_SUB_TEX_COORD4DV                             = 282,
    GL_SUB_TEX_COORD4F                              = 283,
    GL_SUB_TEX_COORD4FV                             = 284,
    GL_SUB_TEX_COORD4I                              = 285,
    GL_SUB_TEX_COORD4IV                             = 286,
    GL_SUB_TEX_COORD4S                              = 287,
    GL_SUB_TEX_COORD4SV                             = 288,
    GL_SUB_TEX_COORD_POINTER                        = 289,
    GL_SUB_TEX_ENVF                                 = 290,
    GL_SUB_TEX_ENVFV                                = 291,
    GL_SUB_TEX_ENVI                                 = 292,
    GL_SUB_TEX_ENVIV                                = 293,
    GL_SUB_TEX_GEND                                 = 294,
    GL_SUB_TEX_GENDV                                = 295,
    GL_SUB_TEX_GENF                                 = 296,
    GL_SUB_TEX_GENFV                                = 297,
    GL_SUB_TEX_GENI                                 = 298,
    GL_SUB_TEX_GENIV                                = 299,
    GL_SUB_TEX_IMAGE1D                              = 300,
    GL_SUB_TEX_IMAGE2D                              = 301,
    GL_SUB_TEX_PARAMETERF                           = 302,
    GL_SUB_TEX_PARAMETERFV                          = 303,
    GL_SUB_TEX_PARAMETERI                           = 304,
    GL_SUB_TEX_PARAMETERIV                          = 305,
    GL_SUB_TEX_SUB_IMAGE1D                          = 306,
    GL_SUB_TEX_SUB_IMAGE2D                          = 307,
    GL_SUB_TRANSLATED                               = 308,
    GL_SUB_TRANSLATEF                               = 309,
    GL_SUB_VERTEX2D                                 = 310,
    GL_SUB_VERTEX2DV                                = 311,
    GL_SUB_VERTEX2F                                 = 312,
    GL_SUB_VERTEX2FV                                = 313,
    GL_SUB_VERTEX2I                                 = 314,
    GL_SUB_VERTEX2IV                                = 315,
    GL_SUB_VERTEX2S                                 = 316,
    GL_SUB_VERTEX2SV                                = 317,
    GL_SUB_VERTEX3D                                 = 318,
    GL_SUB_VERTEX3DV                                = 319,
    GL_SUB_VERTEX3F                                 = 320,
    GL_SUB_VERTEX3FV                                = 321,
    GL_SUB_VERTEX3I                                 = 322,
    GL_SUB_VERTEX3IV                                = 323,
    GL_SUB_VERTEX3S                                 = 324,
    GL_SUB_VERTEX3SV                                = 325,
    GL_SUB_VERTEX4D                                 = 326,
    GL_SUB_VERTEX4DV                                = 327,
    GL_SUB_VERTEX4F                                 = 328,
    GL_SUB_VERTEX4FV                                = 329,
    GL_SUB_VERTEX4I                                 = 330,
    GL_SUB_VERTEX4IV                                = 331,
    GL_SUB_VERTEX4S                                 = 332,
    GL_SUB_VERTEX4SV                                = 333,
    GL_SUB_VERTEX_POINTER                           = 334,
    GL_SUB_VIEWPORT                                 = 335,

    GL_SUB_CORE_COUNT                               = 336,

    // =========================================================================
    // GL Extension sub-opcodes (400-503) -- GLIExtensionDispatch field order
    // =========================================================================
    GL_SUB_BLEND_COLOR_EXT                          = 400,
    GL_SUB_BLEND_EQUATION_EXT                       = 401,
    GL_SUB_LOCK_ARRAYS_EXT                          = 402,
    GL_SUB_UNLOCK_ARRAYS_EXT                        = 403,
    GL_SUB_CLIENT_ACTIVE_TEXTURE_ARB                = 404,
    GL_SUB_ACTIVE_TEXTURE_ARB                       = 405,
    GL_SUB_MULTI_TEX_COORD1D_ARB                    = 406,
    GL_SUB_MULTI_TEX_COORD1DV_ARB                   = 407,
    GL_SUB_MULTI_TEX_COORD1F_ARB                    = 408,
    GL_SUB_MULTI_TEX_COORD1FV_ARB                   = 409,
    GL_SUB_MULTI_TEX_COORD1I_ARB                    = 410,
    GL_SUB_MULTI_TEX_COORD1IV_ARB                   = 411,
    GL_SUB_MULTI_TEX_COORD1S_ARB                    = 412,
    GL_SUB_MULTI_TEX_COORD1SV_ARB                   = 413,
    GL_SUB_MULTI_TEX_COORD2D_ARB                    = 414,
    GL_SUB_MULTI_TEX_COORD2DV_ARB                   = 415,
    GL_SUB_MULTI_TEX_COORD2F_ARB                    = 416,
    GL_SUB_MULTI_TEX_COORD2FV_ARB                   = 417,
    GL_SUB_MULTI_TEX_COORD2I_ARB                    = 418,
    GL_SUB_MULTI_TEX_COORD2IV_ARB                   = 419,
    GL_SUB_MULTI_TEX_COORD2S_ARB                    = 420,
    GL_SUB_MULTI_TEX_COORD2SV_ARB                   = 421,
    GL_SUB_MULTI_TEX_COORD3D_ARB                    = 422,
    GL_SUB_MULTI_TEX_COORD3DV_ARB                   = 423,
    GL_SUB_MULTI_TEX_COORD3F_ARB                    = 424,
    GL_SUB_MULTI_TEX_COORD3FV_ARB                   = 425,
    GL_SUB_MULTI_TEX_COORD3I_ARB                    = 426,
    GL_SUB_MULTI_TEX_COORD3IV_ARB                   = 427,
    GL_SUB_MULTI_TEX_COORD3S_ARB                    = 428,
    GL_SUB_MULTI_TEX_COORD3SV_ARB                   = 429,
    GL_SUB_MULTI_TEX_COORD4D_ARB                    = 430,
    GL_SUB_MULTI_TEX_COORD4DV_ARB                   = 431,
    GL_SUB_MULTI_TEX_COORD4F_ARB                    = 432,
    GL_SUB_MULTI_TEX_COORD4FV_ARB                   = 433,
    GL_SUB_MULTI_TEX_COORD4I_ARB                    = 434,
    GL_SUB_MULTI_TEX_COORD4IV_ARB                   = 435,
    GL_SUB_MULTI_TEX_COORD4S_ARB                    = 436,
    GL_SUB_MULTI_TEX_COORD4SV_ARB                   = 437,
    GL_SUB_LOAD_TRANSPOSE_MATRIXD_ARB               = 438,
    GL_SUB_LOAD_TRANSPOSE_MATRIXF_ARB               = 439,
    GL_SUB_MULT_TRANSPOSE_MATRIXD_ARB               = 440,
    GL_SUB_MULT_TRANSPOSE_MATRIXF_ARB               = 441,
    GL_SUB_COMPRESSED_TEX_IMAGE3D_ARB               = 442,
    GL_SUB_COMPRESSED_TEX_IMAGE2D_ARB               = 443,
    GL_SUB_COMPRESSED_TEX_IMAGE1D_ARB               = 444,
    GL_SUB_COMPRESSED_TEX_SUB_IMAGE3D_ARB           = 445,
    GL_SUB_COMPRESSED_TEX_SUB_IMAGE2D_ARB           = 446,
    GL_SUB_COMPRESSED_TEX_SUB_IMAGE1D_ARB           = 447,
    GL_SUB_GET_COMPRESSED_TEX_IMAGE_ARB             = 448,
    GL_SUB_SECONDARYCOLOR3B_EXT                     = 449,
    GL_SUB_SECONDARYCOLOR3BV_EXT                    = 450,
    GL_SUB_SECONDARYCOLOR3D_EXT                     = 451,
    GL_SUB_SECONDARY_COLOR3DV_EXT                   = 452,
    GL_SUB_SECONDARY_COLOR3F_EXT                    = 453,
    GL_SUB_SECONDARY_COLOR3FV_EXT                   = 454,
    GL_SUB_SECONDARY_COLOR3I_EXT                    = 455,
    GL_SUB_SECONDARY_COLOR3IV_EXT                   = 456,
    GL_SUB_SECONDARY_COLOR3S_EXT                    = 457,
    GL_SUB_SECONDARY_COLOR3SV_EXT                   = 458,
    GL_SUB_SECONDARY_COLOR3UB_EXT                   = 459,
    GL_SUB_SECONDARY_COLOR3UBV_EXT                  = 460,
    GL_SUB_SECONDARY_COLOR3UI_EXT                   = 461,
    GL_SUB_SECONDARY_COLOR3UIV_EXT                  = 462,
    GL_SUB_SECONDARY_COLOR3US_EXT                   = 463,
    GL_SUB_SECONDARY_COLOR3USV_EXT                  = 464,
    GL_SUB_SECONDARY_COLOR_POINTER_EXT              = 465,
    GL_SUB_BLEND_COLOR_1_2                          = 466,
    GL_SUB_BLEND_EQUATION_1_2                       = 467,
    GL_SUB_DRAW_RANGE_ELEMENTS                      = 468,
    GL_SUB_COLOR_TABLE                              = 469,
    GL_SUB_COLOR_TABLE_PARAMETERFV                  = 470,
    GL_SUB_COLOR_TABLE_PARAMETERIV                  = 471,
    GL_SUB_COPY_COLOR_TABLE                         = 472,
    GL_SUB_GET_COLOR_TABLE                          = 473,
    GL_SUB_GET_COLOR_TABLE_PARAMETERFV              = 474,
    GL_SUB_GET_COLOR_TABLE_PARAMETERIV              = 475,
    GL_SUB_COLOR_SUB_TABLE                          = 476,
    GL_SUB_COPY_COLOR_SUB_TABLE                     = 477,
    GL_SUB_CONVOLUTION_FILTER1D                     = 478,
    GL_SUB_CONVOLUTION_FILTER2D                     = 479,
    GL_SUB_CONVOLUTION_PARAMETERF                   = 480,
    GL_SUB_CONVOLUTION_PARAMETERFV                  = 481,
    GL_SUB_CONVOLUTION_PARAMETERI                   = 482,
    GL_SUB_CONVOLUTION_PARAMETERIV                  = 483,
    GL_SUB_COPY_CONVOLUTION_FILTER1D                = 484,
    GL_SUB_COPY_CONVOLUTION_FILTER2D                = 485,
    GL_SUB_GET_CONVOLUTION_FILTER                   = 486,
    GL_SUB_GET_CONVOLUTION_PARAMETERFV              = 487,
    GL_SUB_GET_CONVOLUTION_PARAMETERIV              = 488,
    GL_SUB_GET_SEPARABLE_FILTER                     = 489,
    GL_SUB_SEPARABLE_FILTER2D                       = 490,
    GL_SUB_GET_HISTOGRAM                            = 491,
    GL_SUB_GET_HISTOGRAM_PARAMETERFV                = 492,
    GL_SUB_GET_HISTOGRAM_PARAMETERIV                = 493,
    GL_SUB_GET_MINMAX                               = 494,
    GL_SUB_GET_MINMAX_PARAMETERFV                   = 495,
    GL_SUB_GET_MINMAX_PARAMETERIV                   = 496,
    GL_SUB_HISTOGRAM                                = 497,
    GL_SUB_MINMAX                                   = 498,
    GL_SUB_RESET_HISTOGRAM                          = 499,
    GL_SUB_RESET_MINMAX                             = 500,
    GL_SUB_TEX_IMAGE3D_EXT                          = 501,
    GL_SUB_TEX_SUB_IMAGE3D_EXT                      = 502,
    GL_SUB_COPY_TEX_SUB_IMAGE3D_EXT                 = 503,

    GL_SUB_EXT_COUNT                                = 104,

    // =========================================================================
    // AGL sub-opcodes (600-632) -- agl.h declaration order
    // =========================================================================
    GL_SUB_AGL_CHOOSEPIXELFORMAT                    = 600,
    GL_SUB_AGL_DESTROYPIXELFORMAT                   = 601,
    GL_SUB_AGL_NEXTPIXELFORMAT                      = 602,
    GL_SUB_AGL_DESCRIBEPIXELFORMAT                  = 603,
    GL_SUB_AGL_DEVICESOFPIXELFORMAT                 = 604,
    GL_SUB_AGL_QUERYRENDERERINFO                    = 605,
    GL_SUB_AGL_DESTROYRENDERERINFO                  = 606,
    GL_SUB_AGL_NEXTRENDERERINFO                     = 607,
    GL_SUB_AGL_DESCRIBERENDERER                     = 608,
    GL_SUB_AGL_CREATECONTEXT                        = 609,
    GL_SUB_AGL_DESTROYCONTEXT                       = 610,
    GL_SUB_AGL_COPYCONTEXT                          = 611,
    GL_SUB_AGL_UPDATECONTEXT                        = 612,
    GL_SUB_AGL_SETCURRENTCONTEXT                    = 613,
    GL_SUB_AGL_GETCURRENTCONTEXT                    = 614,
    GL_SUB_AGL_SETDRAWABLE                          = 615,
    GL_SUB_AGL_SETOFFSCREEN                         = 616,
    GL_SUB_AGL_SETFULLSCREEN                        = 617,
    GL_SUB_AGL_GETDRAWABLE                          = 618,
    GL_SUB_AGL_SETVIRTUALSCREEN                     = 619,
    GL_SUB_AGL_GETVIRTUALSCREEN                     = 620,
    GL_SUB_AGL_GETVERSION                           = 621,
    GL_SUB_AGL_CONFIGURE                            = 622,
    GL_SUB_AGL_SWAPBUFFERS                          = 623,
    GL_SUB_AGL_ENABLE                               = 624,
    GL_SUB_AGL_DISABLE                              = 625,
    GL_SUB_AGL_ISENABLED                            = 626,
    GL_SUB_AGL_SETINTEGER                           = 627,
    GL_SUB_AGL_GETINTEGER                           = 628,
    GL_SUB_AGL_USEFONT                              = 629,
    GL_SUB_AGL_GETERROR                             = 630,
    GL_SUB_AGL_ERRORSTRING                          = 631,
    GL_SUB_AGL_RESETLIBRARY                         = 632,

    GL_SUB_AGL_COUNT                                = 33,

    // =========================================================================
    // GLU sub-opcodes (700-753) -- glu.h declaration order
    // =========================================================================
    GL_SUB_GLU_BEGINCURVE                           = 700,
    GL_SUB_GLU_BEGINPOLYGON                         = 701,
    GL_SUB_GLU_BEGINSURFACE                         = 702,
    GL_SUB_GLU_BEGINTRIM                            = 703,
    GL_SUB_GLU_BUILD1DMIPMAPS                       = 704,
    GL_SUB_GLU_BUILD2DMIPMAPS                       = 705,
    GL_SUB_GLU_CYLINDER                             = 706,
    GL_SUB_GLU_DELETENURBSRENDERER                  = 707,
    GL_SUB_GLU_DELETENURBSTESSELLATOREXT            = 708,
    GL_SUB_GLU_DELETEQUADRIC                        = 709,
    GL_SUB_GLU_DELETETESS                           = 710,
    GL_SUB_GLU_DISK                                 = 711,
    GL_SUB_GLU_ENDCURVE                             = 712,
    GL_SUB_GLU_ENDPOLYGON                           = 713,
    GL_SUB_GLU_ENDSURFACE                           = 714,
    GL_SUB_GLU_ENDTRIM                              = 715,
    GL_SUB_GLU_ERRORSTRING                          = 716,
    GL_SUB_GLU_GETNURBSPROPERTY                     = 717,
    GL_SUB_GLU_GETSTRING                            = 718,
    GL_SUB_GLU_GETTESSPROPERTY                      = 719,
    GL_SUB_GLU_LOADSAMPLINGMATRICES                 = 720,
    GL_SUB_GLU_LOOKAT                               = 721,
    GL_SUB_GLU_NEWNURBSRENDERER                     = 722,
    GL_SUB_GLU_NEWNURBSTESSELLATOREXT               = 723,
    GL_SUB_GLU_NEWQUADRIC                           = 724,
    GL_SUB_GLU_NEWTESS                              = 725,
    GL_SUB_GLU_NEXTCONTOUR                          = 726,
    GL_SUB_GLU_NURBSCALLBACK                        = 727,
    GL_SUB_GLU_NURBSCALLBACKDATAEXT                 = 728,
    GL_SUB_GLU_NURBSCURVE                           = 729,
    GL_SUB_GLU_NURBSPROPERTY                        = 730,
    GL_SUB_GLU_NURBSSURFACE                         = 731,
    GL_SUB_GLU_ORTHO2D                              = 732,
    GL_SUB_GLU_PARTIALDISK                          = 733,
    GL_SUB_GLU_PERSPECTIVE                          = 734,
    GL_SUB_GLU_PICKMATRIX                           = 735,
    GL_SUB_GLU_PROJECT                              = 736,
    GL_SUB_GLU_PWLCURVE                             = 737,
    GL_SUB_GLU_QUADRICCALLBACK                      = 738,
    GL_SUB_GLU_QUADRICDRAWSTYLE                     = 739,
    GL_SUB_GLU_QUADRICNORMALS                       = 740,
    GL_SUB_GLU_QUADRICORIENTATION                   = 741,
    GL_SUB_GLU_QUADRICTEXTURE                       = 742,
    GL_SUB_GLU_SCALEIMAGE                           = 743,
    GL_SUB_GLU_SPHERE                               = 744,
    GL_SUB_GLU_TESSBEGINCONTOUR                     = 745,
    GL_SUB_GLU_TESSBEGINPOLYGON                     = 746,
    GL_SUB_GLU_TESSCALLBACK                         = 747,
    GL_SUB_GLU_TESSENDCONTOUR                       = 748,
    GL_SUB_GLU_TESSENDPOLYGON                       = 749,
    GL_SUB_GLU_TESSNORMAL                           = 750,
    GL_SUB_GLU_TESSPROPERTY                         = 751,
    GL_SUB_GLU_TESSVERTEX                           = 752,
    GL_SUB_GLU_UNPROJECT                            = 753,

    GL_SUB_GLU_COUNT                                = 54,

    // =========================================================================
    // GLUT sub-opcodes (800-915) -- glut.h declaration order
    // =========================================================================
    GL_SUB_GLUT_INITMAC                             = 800,
    GL_SUB_GLUT_INITDISPLAYMODE                     = 801,
    GL_SUB_GLUT_INITDISPLAYSTRING                   = 802,
    GL_SUB_GLUT_INITWINDOWPOSITION                  = 803,
    GL_SUB_GLUT_INITWINDOWSIZE                      = 804,
    GL_SUB_GLUT_MAINLOOP                            = 805,
    GL_SUB_GLUT_CREATEWINDOW                        = 806,
    GL_SUB_GLUT_CREATEPLAINWINDOW                   = 807,
    GL_SUB_GLUT_CREATESUBWINDOW                     = 808,
    GL_SUB_GLUT_DESTROYWINDOW                       = 809,
    GL_SUB_GLUT_POSTREDISPLAY                       = 810,
    GL_SUB_GLUT_POSTWINDOWREDISPLAY                 = 811,
    GL_SUB_GLUT_SWAPBUFFERS                         = 812,
    GL_SUB_GLUT_GETWINDOW                           = 813,
    GL_SUB_GLUT_SETWINDOW                           = 814,
    GL_SUB_GLUT_SETWINDOWTITLE                      = 815,
    GL_SUB_GLUT_SETICONTITLE                        = 816,
    GL_SUB_GLUT_POSITIONWINDOW                      = 817,
    GL_SUB_GLUT_RESHAPEWINDOW                       = 818,
    GL_SUB_GLUT_POPWINDOW                           = 819,
    GL_SUB_GLUT_PUSHWINDOW                          = 820,
    GL_SUB_GLUT_ICONIFYWINDOW                       = 821,
    GL_SUB_GLUT_SHOWWINDOW                          = 822,
    GL_SUB_GLUT_HIDEWINDOW                          = 823,
    GL_SUB_GLUT_FULLSCREEN                          = 824,
    GL_SUB_GLUT_SETCURSOR                           = 825,
    GL_SUB_GLUT_WARPPOINTER                         = 826,
    GL_SUB_GLUT_ESTABLISHOVERLAY                    = 827,
    GL_SUB_GLUT_REMOVEOVERLAY                       = 828,
    GL_SUB_GLUT_USELAYER                            = 829,
    GL_SUB_GLUT_POSTOVERLAYREDISPLAY                = 830,
    GL_SUB_GLUT_POSTWINDOWOVERLAYREDISPLAY          = 831,
    GL_SUB_GLUT_SHOWOVERLAY                         = 832,
    GL_SUB_GLUT_HIDEOVERLAY                         = 833,
    GL_SUB_GLUT_CREATEMENU                          = 834,
    GL_SUB_GLUT_DESTROYMENU                         = 835,
    GL_SUB_GLUT_GETMENU                             = 836,
    GL_SUB_GLUT_SETMENU                             = 837,
    GL_SUB_GLUT_ADDMENUENTRY                        = 838,
    GL_SUB_GLUT_ADDSUBMENU                          = 839,
    GL_SUB_GLUT_CHANGETOMENUENTRY                   = 840,
    GL_SUB_GLUT_CHANGETOSUBMENU                     = 841,
    GL_SUB_GLUT_REMOVEMENUITEM                      = 842,
    GL_SUB_GLUT_ATTACHMENU                          = 843,
    GL_SUB_GLUT_ATTACHMENUNAME                      = 844,
    GL_SUB_GLUT_DETACHMENU                          = 845,
    GL_SUB_GLUT_DISPLAYFUNC                         = 846,
    GL_SUB_GLUT_RESHAPEFUNC                         = 847,
    GL_SUB_GLUT_KEYBOARDFUNC                        = 848,
    GL_SUB_GLUT_MOUSEFUNC                           = 849,
    GL_SUB_GLUT_MOTIONFUNC                          = 850,
    GL_SUB_GLUT_PASSIVEMOTIONFUNC                   = 851,
    GL_SUB_GLUT_ENTRYFUNC                           = 852,
    GL_SUB_GLUT_VISIBILITYFUNC                      = 853,
    GL_SUB_GLUT_IDLEFUNC                            = 854,
    GL_SUB_GLUT_TIMERFUNC                           = 855,
    GL_SUB_GLUT_MENUSTATEFUNC                       = 856,
    GL_SUB_GLUT_SPECIALFUNC                         = 857,
    GL_SUB_GLUT_SPACEBALLMOTIONFUNC                 = 858,
    GL_SUB_GLUT_SPACEBALLROTATEFUNC                 = 859,
    GL_SUB_GLUT_SPACEBALLBUTTONFUNC                 = 860,
    GL_SUB_GLUT_BUTTONBOXFUNC                       = 861,
    GL_SUB_GLUT_DIALSFUNC                           = 862,
    GL_SUB_GLUT_TABLETMOTIONFUNC                    = 863,
    GL_SUB_GLUT_TABLETBUTTONFUNC                    = 864,
    GL_SUB_GLUT_MENUSTATUSFUNC                      = 865,
    GL_SUB_GLUT_OVERLAYDISPLAYFUNC                  = 866,
    GL_SUB_GLUT_WINDOWSTATUSFUNC                    = 867,
    GL_SUB_GLUT_KEYBOARDUPFUNC                      = 868,
    GL_SUB_GLUT_SPECIALUPFUNC                       = 869,
    GL_SUB_GLUT_JOYSTICKFUNC                        = 870,
    GL_SUB_GLUT_SETCOLOR                            = 871,
    GL_SUB_GLUT_GETCOLOR                            = 872,
    GL_SUB_GLUT_COPYCOLORMAP                        = 873,
    GL_SUB_GLUT_GET                                 = 874,
    GL_SUB_GLUT_DEVICEGET                           = 875,
    GL_SUB_GLUT_EXTENSIONSUPPORTED                  = 876,
    GL_SUB_GLUT_GETMODIFIERS                        = 877,
    GL_SUB_GLUT_LAYERGET                            = 878,
    GL_SUB_GLUT_BITMAPCHARACTER                     = 879,
    GL_SUB_GLUT_BITMAPWIDTH                         = 880,
    GL_SUB_GLUT_STROKECHARACTER                     = 881,
    GL_SUB_GLUT_STROKEWIDTH                         = 882,
    GL_SUB_GLUT_BITMAPLENGTH                        = 883,
    GL_SUB_GLUT_STROKELENGTH                        = 884,
    GL_SUB_GLUT_WIRESPHERE                          = 885,
    GL_SUB_GLUT_SOLIDSPHERE                         = 886,
    GL_SUB_GLUT_WIRECONE                            = 887,
    GL_SUB_GLUT_SOLIDCONE                           = 888,
    GL_SUB_GLUT_WIRECUBE                            = 889,
    GL_SUB_GLUT_SOLIDCUBE                           = 890,
    GL_SUB_GLUT_WIRETORUS                           = 891,
    GL_SUB_GLUT_SOLIDTORUS                          = 892,
    GL_SUB_GLUT_WIREDODECAHEDRON                    = 893,
    GL_SUB_GLUT_SOLIDDODECAHEDRON                   = 894,
    GL_SUB_GLUT_WIRETEAPOT                          = 895,
    GL_SUB_GLUT_SOLIDTEAPOT                         = 896,
    GL_SUB_GLUT_WIREOCTAHEDRON                      = 897,
    GL_SUB_GLUT_SOLIDOCTAHEDRON                     = 898,
    GL_SUB_GLUT_WIRETETRAHEDRON                     = 899,
    GL_SUB_GLUT_SOLIDTETRAHEDRON                    = 900,
    GL_SUB_GLUT_WIREICOSAHEDRON                     = 901,
    GL_SUB_GLUT_SOLIDICOSAHEDRON                    = 902,
    GL_SUB_GLUT_VIDEORESIZEGET                      = 903,
    GL_SUB_GLUT_SETUPVIDEORESIZING                  = 904,
    GL_SUB_GLUT_STOPVIDEORESIZING                   = 905,
    GL_SUB_GLUT_VIDEORESIZE                         = 906,
    GL_SUB_GLUT_VIDEOPAN                            = 907,
    GL_SUB_GLUT_REPORTERRORS                        = 908,
    GL_SUB_GLUT_IGNOREKEYREPEAT                     = 909,
    GL_SUB_GLUT_SETKEYREPEAT                        = 910,
    GL_SUB_GLUT_FORCEJOYSTICKFUNC                   = 911,
    GL_SUB_GLUT_GAMEMODESTRING                      = 912,
    GL_SUB_GLUT_ENTERGAMEMODE                       = 913,
    GL_SUB_GLUT_LEAVEGAMEMODE                       = 914,
    GL_SUB_GLUT_GAMEMODEGET                         = 915,

    // =========================================================================
    // Reserved sub-opcodes (916-919) -- internal host use only
    // =========================================================================
    GL_SUB_DRAIN                                    = 916,   // reserved: native deferred-ring drain trigger (Track A)

    GL_SUB_GLUT_COUNT                               = 116,
};

// Maximum sub-opcode value (for array sizing)
#define GL_MAX_SUBOPCODE 920

// ---- Context-handle dispatch table ----
// Some games (4x4 Evolution, Cro-Mag Rally) read GL function descriptors
// directly out of the Mac-side context handle at `handle + 4 + slot*4` instead
// of calling the exported stubs. Core GL 1.1 occupies slots 0..335 (see
// gl_dispatch_sub_opcodes[]), but a game may also read *extension* slots beyond
// that. We over-allocate the handle to GL_CTX_DISPATCH_SLOTS so every such read
// lands inside a populated allocation (never uninitialized guest heap), and we
// fill any slot we don't map to a real GL function with a diagnostic no-op
// TVECT whose sub-opcode is GL_DT_DIAG_BASE + slot. GL_DT_DIAG_BASE must be
// greater than every real sub-opcode, and GL_DT_DIAG_BASE + GL_CTX_DISPATCH_SLOTS
// must stay below 0x8000 (the signed 16-bit `li` immediate the thunk emits).
#define GL_CTX_DISPATCH_SLOTS 512
#define GL_DT_DIAG_BASE       4096   // sub_opcode >= this => unmapped slot (slot = sub_opcode - base)

// Sub-opcode range boundaries (for iteration in GLThunksInit)
#define GL_CORE_FIRST    0
#define GL_CORE_LAST     335
#define GL_EXT_FIRST     400
#define GL_EXT_LAST      503
#define GL_AGL_FIRST     600
#define GL_AGL_LAST      632
#define GL_GLU_FIRST     700
#define GL_GLU_LAST      753
#define GL_GLUT_FIRST    800
#define GL_GLUT_LAST     915


/*
 *  Function signature table -- maps sub-opcode to argument type info
 *
 *  For each sub-opcode, stores how many args and which are float/double
 *  (passed in FPRs on PPC). This enables generic FPR extraction in the
 *  NATIVE_OPENGL_DISPATCH handler.
 *
 *  float_mask: bit N = 1 means arg N is a float/double (comes from FPR).
 *              bit N = 0 means arg N is int/pointer (comes from GPR).
 *  PPC ABI passes floating-point args in FPR1-FPR13, so signatures can
 *  describe up to 13 PPC-visible floating-point args.
 */
struct GLFuncSignature {
    uint8_t num_args;    // Total argument count (excluding ctx pointer)
    uint16_t float_mask; // Bit per arg: 1=float/double, 0=int/ptr
};


/*
 *  GLVertex -- immediate mode vertex with full attribute state
 */
struct GLVertex {
    float position[4];       // x, y, z, w
    float color[4];          // r, g, b, a
    float normal[3];         // nx, ny, nz
    float texcoord[4][4];    // per texture unit: s, t, r, q
    float secondary_color[3]; // r, g, b (EXT_secondary_color)
    float fog_coord;          // EXT_fog_coord
};


/*
 *  GLTextureObject -- texture name -> Metal texture mapping
 */
struct GLTextureObject {
    uint32_t name;           // GL texture name
    void    *metal_texture;  // id<MTLTexture> as void* (C++ compatible)
    int      width;
    int      height;
    int      depth;          // for 3D textures (GL_TEXTURE_3D_EXT)
    uint32_t source_format;
    uint32_t source_type;
    uint32_t min_filter;     // GLenum: GL_NEAREST, GL_LINEAR, etc.
    uint32_t mag_filter;     // GLenum
	    uint32_t wrap_s;         // GLenum: GL_REPEAT, GL_CLAMP, etc.
		    uint32_t wrap_t;         // GLenum
			    int      env_mode;       // GL_MODULATE, GL_DECAL, GL_BLEND, GL_REPLACE
			    bool     has_mipmaps;
			    bool     legacy_ushort_palette_index_chain; // level-0 duplicated-byte indexed palette applies to mips
				    bool     legacy_ushort_bgr332_chain; // level-0 duplicated-byte BGR332 fallback may apply to duplicated mips
			    bool     legacy_ushort_index_gray_chain; // level-0 duplicated-byte index fallback applies to mips
			    bool     legacy_ushort_scalar_no_palette_chain; // level-0 scalar decode rejected duplicated-byte mips
			};


/*
 *  GLCommand -- display list command storage
 */
struct GLCommand {
    uint16_t opcode;         // GL_SUB_* sub-opcode
    union {
        float    f[16];      // float arguments (e.g., matrix, vertex)
        int32_t  i[8];       // integer arguments
        uint32_t u[8];       // unsigned integer arguments
    } args;
    std::vector<uint8_t> data; // variable-length payload (textures, etc.)
};


/*
 *  GLDisplayList -- compiled command list
 */
struct GLDisplayList {
    uint32_t name;
    std::vector<GLCommand> commands;
};


/*
 *  GLLight -- per-light state (GL supports 8 lights)
 */
struct GLLight {
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float position[4];
    float spot_direction[3];
    float spot_exponent;
    float spot_cutoff;
    float constant_attenuation;
    float linear_attenuation;
    float quadratic_attenuation;
    bool  enabled;
};


/*
 *  GLMaterial -- front/back material state
 */
struct GLMaterial {
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float emission[4];
    float shininess;
};


/*
 *  GLTextureUnit -- per-unit texture state (GL 1.2 supports up to 4 units)
 */
struct GLTextureUnit {
    uint32_t bound_texture_1d;   // name of bound GL_TEXTURE_1D
    uint32_t bound_texture_2d;   // name of bound GL_TEXTURE_2D
    uint32_t bound_texture_3d;   // name of bound GL_TEXTURE_3D_EXT
    bool     enabled_1d;
    bool     enabled_2d;
    bool     enabled_3d;
    int      env_mode;           // GL_MODULATE, GL_DECAL, GL_BLEND, GL_REPLACE
    float    env_color[4];
    // Current texcoord (for immediate mode)
    float    current_texcoord[4]; // s, t, r, q
    // Tex gen state
    bool     texgen_s_enabled;
    bool     texgen_t_enabled;
    bool     texgen_r_enabled;
    bool     texgen_q_enabled;
    int      texgen_s_mode;
    int      texgen_t_mode;
    int      texgen_r_mode;
    int      texgen_q_mode;
};


/*
 *  GLVertexArrayPointer -- client vertex array state
 */
struct GLVertexArrayPointer {
    uint32_t pointer;   // Mac address of array data (PPC pointer)
    int      size;      // number of components (1-4)
    int      stride;    // byte stride between elements
    uint32_t type;      // GLenum: GL_FLOAT, GL_INT, etc.
    bool     enabled;   // glEnableClientState'd
};


/*
 *  GLPixelStore -- pixel pack/unpack state
 */
struct GLPixelStore {
    int  pack_alignment;
    int  pack_row_length;
    int  pack_skip_pixels;
    int  pack_skip_rows;
    int  unpack_alignment;
    int  unpack_row_length;
    int  unpack_skip_pixels;
    int  unpack_skip_rows;
};


/*
 *  GLStencilState
 */
struct GLStencilState {
    uint32_t func;         // GLenum: GL_ALWAYS, etc.
    int32_t  ref;
    uint32_t value_mask;
    uint32_t write_mask;
    uint32_t sfail;        // GLenum: GL_KEEP, etc.
    uint32_t dpfail;
    uint32_t dppass;
};


/*
 *  GLAttribStackEntry -- for glPushAttrib/glPopAttrib
 *  Stores a bitmask of which attribute groups were saved,
 *  plus copies of each saveable state group.
 */
struct GLAttribStackEntry {
    uint32_t mask;  // GL_CURRENT_BIT, GL_ENABLE_BIT, etc.
    // --- GL_CURRENT_BIT ---
    float    clear_color[4];
    float    clear_depth;
    int32_t  clear_stencil;
    float    current_color[4];
    float    current_normal[3];
    float    current_texcoord[4];
    // --- GL_ENABLE_BIT ---
    bool     depth_test;
    bool     blend;
    bool     cull_face;
    bool     lighting;
    bool     texture_2d;
    bool     alpha_test;
    bool     scissor_test;
    bool     stencil_test;
    bool     fog;
    bool     normalize;
    // --- GL_POLYGON_BIT ---
    uint32_t polygon_mode_front;
    uint32_t polygon_mode_back;
    bool     cull_face_enabled;
    uint32_t front_face;
    float    polygon_offset_factor;
    float    polygon_offset_units;
    bool     polygon_offset_fill;
    bool     polygon_offset_line;
    bool     polygon_offset_point;
    // --- GL_LIGHTING_BIT ---
    bool     lighting_enabled;
    uint32_t shade_model;
    bool     color_material_enabled;
    uint32_t color_material_face;
    uint32_t color_material_mode;
    GLMaterial saved_materials[2];    // [0]=front, [1]=back
    float    light_model_ambient[4];
    bool     light_model_two_side;
    bool     light_model_local_viewer;
    bool     light_enabled[8];
    // --- GL_FOG_BIT ---
    bool     fog_enabled;
    uint32_t fog_mode;
    float    fog_density;
    float    fog_start;
    float    fog_end;
    float    fog_color[4];
    // --- GL_TEXTURE_BIT ---
    int      saved_active_texture;
    GLTextureUnit saved_tex_units[4];
    // --- GL_STENCIL_BUFFER_BIT (full state) ---
    GLStencilState saved_stencil;
    int32_t  saved_clear_stencil;
    // --- GL_VIEWPORT_BIT ---
    int32_t  saved_viewport[4];
    float    saved_depth_range_near;
    float    saved_depth_range_far;
    // --- GL_HINT_BIT ---
    uint32_t saved_hint_perspective;
    uint32_t saved_hint_point_smooth;
    uint32_t saved_hint_line_smooth;
    uint32_t saved_hint_polygon_smooth;
    uint32_t saved_hint_fog;
    // --- GL_POINT_BIT ---
    float    saved_point_size;
    bool     saved_point_smooth;
    // --- GL_LINE_BIT ---
    float    saved_line_width;
    bool     saved_line_smooth;
    int32_t  saved_line_stipple_factor;
    uint16_t saved_line_stipple_pattern;
    // --- GL_SCISSOR_BIT ---
    int32_t  saved_scissor_box[4];
    bool     saved_scissor_test;
    // --- GL_TRANSFORM_BIT ---
    uint32_t saved_matrix_mode;
    bool     saved_clip_plane_enabled[6];
};

#define GL_MAX_ATTRIB_STACK_DEPTH 16

// Number of evaluator targets (GL_MAP1_COLOR_4 through GL_MAP1_VERTEX_4 = 9)
#define GL_EVAL_MAX_TARGETS 9

/*
 *  GLEvaluatorMap1 -- 1D evaluator map (glMap1f/glMap1d)
 *
 *  Stores control points for a single-parameter polynomial map.
 *  de Casteljau's algorithm evaluates these at runtime.
 */
struct GLEvaluatorMap1 {
    uint32_t target;           // GL_MAP1_VERTEX_3, etc.
    int32_t  order;            // polynomial order (number of control points)
    float    u1, u2;           // parameter domain
    int32_t  stride;           // original stride from glMap1f (in floats)
    int      dimension;        // components: 3 for VERTEX_3, 4 for VERTEX_4/COLOR_4, etc.
    std::vector<float> control_points; // order * dimension floats, packed
    bool     defined;          // true after glMap1f/d called
};

/*
 *  GLEvaluatorMap2 -- 2D evaluator map (glMap2f/glMap2d)
 *
 *  Stores control points for a two-parameter polynomial map.
 *  Bivariate de Casteljau evaluation: first in v, then in u.
 */
struct GLEvaluatorMap2 {
    uint32_t target;           // GL_MAP2_VERTEX_3, etc.
    int32_t  uorder, vorder;   // control point grid dimensions
    float    u1, u2, v1, v2;   // parameter domain
    int32_t  ustride, vstride; // original strides (in floats)
    int      dimension;        // components per control point
    std::vector<float> control_points; // uorder * vorder * dimension, packed
    bool     defined;
};


/*
 *  Imaging subset state structures (OpenGL 1.2)
 */
struct GLColorTable {
    float    data[256][4];       // up to 256 RGBA entries (float)
    int      width;
    uint32_t internal_format;
    bool     defined;
};

struct GLConvolutionFilter {
    float    kernel[7*7*4];      // max 7x7 kernel, 4 channels
    int      width;
    int      height;
    uint32_t internal_format;
    float    border_color[4];
    bool     defined;
};

struct GLSeparableFilter {
    float    row[7*4];           // max 7-wide row filter, 4 channels
    float    col[7*4];           // max 7-wide column filter, 4 channels
    int      width;
    int      height;
    uint32_t internal_format;
    bool     defined;
};

struct GLHistogramState {
    std::vector<uint32_t> bins;  // width * 4 channels (lazily allocated)
    int      width;
    uint32_t internal_format;
    bool     sink;
    bool     defined;
};

struct GLMinmaxState {
    float    min_values[4];
    float    max_values[4];
    uint32_t internal_format;
    bool     sink;
    bool     defined;
};


/*
 *  GLContext -- full OpenGL 1.2 fixed-function pipeline state
 *
 *  This is the native-side representation of an OpenGL context.
 *  Metal types use void* bridges to keep this header C++ compatible.
 */
struct GLContext {
    // ---- Matrix stacks ----
    float    modelview_stack[32][16];   // 32 deep, 4x4 floats
    int      modelview_depth;
    float    projection_stack[32][16];  // 32 deep (was 2 = spec minimum; real
                                        // Apple/ATI GL allowed ~10. Cro-Mag Rally
                                        // nests the projection matrix deeper than
                                        // 1 -> GL_STACK_OVERFLOW at the old cap)
    int      projection_depth;
    float    texture_stack[4][32][16];  // per unit, 32 deep (was 2)
    int      texture_depth[4];
    float    color_stack[32][16];       // 32 deep (was 2; for imaging subset)
    int      color_depth;
    uint32_t matrix_mode;              // GL_MODELVIEW, GL_PROJECTION, etc.

    // ---- Current vertex state (immediate mode) ----
    float    current_color[4];         // glColor
    float    current_normal[3];        // glNormal
    float    current_texcoord[4][4];   // per unit: glTexCoord / glMultiTexCoord
    float    current_secondary_color[3];
    float    current_fog_coord;

    // ---- Immediate mode ----
    bool     in_begin;
    uint32_t im_mode;                  // GL_TRIANGLES, GL_QUADS, etc.
    std::vector<GLVertex> im_vertices;

    // ---- Lighting ----
    GLLight  lights[8];
    GLMaterial materials[2];           // [0]=front, [1]=back
    bool     lighting_enabled;
    bool     color_material_enabled;
    uint32_t color_material_face;      // GL_FRONT, GL_BACK, GL_FRONT_AND_BACK
    uint32_t color_material_mode;      // GL_AMBIENT, GL_DIFFUSE, etc.
    float    light_model_ambient[4];
    bool     light_model_two_side;
    bool     light_model_local_viewer;

    // ---- Texture units ----
    GLTextureUnit tex_units[4];
    int      active_texture;           // index 0-3 (from GL_TEXTURE0_ARB)
    int      client_active_texture;    // for client state

    // ---- Texture objects ----
    std::unordered_map<uint32_t, GLTextureObject> texture_objects;
    uint32_t next_texture_name;

    // ---- Enable/disable caps ----
    bool     depth_test;
    bool     blend;
    bool     color_sum;                // GL_COLOR_SUM (EXT_secondary_color) - add secondary color after texturing
    bool     cull_face_enabled;
    uint32_t cull_face_mode;           // GL_FRONT, GL_BACK, GL_FRONT_AND_BACK
    uint32_t front_face;               // GL_CCW or GL_CW
    bool     alpha_test;
    uint32_t alpha_func;
    float    alpha_ref;
    bool     scissor_test;
    bool     stencil_test;
    bool     fog_enabled;
    bool     normalize;
    bool     auto_normal;
    bool     point_smooth;
    bool     line_smooth;
    bool     polygon_smooth;
    bool     dither;
    bool     color_logic_op;
    bool     polygon_offset_fill;
    bool     polygon_offset_line;
    bool     polygon_offset_point;
    bool     multisample;
    bool     sample_alpha_to_coverage;
    bool     sample_alpha_to_one;
    bool     sample_coverage;

    // ---- Fog ----
    uint32_t fog_mode;                 // GL_LINEAR, GL_EXP, GL_EXP2
    float    fog_color[4];
    float    fog_density;
    float    fog_start;
    float    fog_end;

    // ---- Viewport and scissor ----
    int32_t  viewport[4];              // x, y, width, height
    int32_t  scissor_box[4];           // x, y, width, height
    float    depth_range_near;
    float    depth_range_far;

    // ---- Depth ----
    uint32_t depth_func;
    bool     depth_mask;

    // ---- Blend ----
    uint32_t blend_src;
    uint32_t blend_dst;
    float    blend_color[4];           // EXT_blend_color
    uint32_t blend_equation;           // EXT_blend_equation (GL_FUNC_ADD etc.)

    // ---- Compiled vertex arrays (EXT_compiled_vertex_array) ----
    bool     arrays_locked;
    int32_t  lock_first;
    int32_t  lock_count;

    // ---- Vertex arrays ----
    GLVertexArrayPointer vertex_array;
    GLVertexArrayPointer normal_array;
    GLVertexArrayPointer color_array;
    GLVertexArrayPointer texcoord_array[4];
    GLVertexArrayPointer secondary_color_array;
    GLVertexArrayPointer edge_flag_array;
    GLVertexArrayPointer index_array;

    // ---- Immediate draw-state latches ----
    // ATI-era clients such as Warcraft III prepare texture state and client
    // texcoords, then disable them immediately before glDrawElements. Track
    // same-draw cleanup plus a bounded texture latch for subsequent array
    // draws that keep binding textures and texcoords in the same batch.
    uint32_t completed_draw_serial;
    uint32_t texture_2d_enable_draw_serial[4];
    uint32_t texcoord_array_enable_draw_serial[4];
    bool     prepared_texture_2d_for_draw[4];
    bool     latched_texture_2d_for_array_draw[4];
    bool     prepared_texcoord_array_for_draw[4];
    GLVertexArrayPointer prepared_texcoord_array[4];

    // ---- Display lists ----
    std::unordered_map<uint32_t, GLDisplayList> display_lists;
    uint32_t next_list_name;
    bool     in_display_list;
    uint32_t current_list_name;
    uint32_t current_list_mode;        // GL_COMPILE or GL_COMPILE_AND_EXECUTE
    uint32_t list_base;

    // ---- Metal state ----
    void    *metal;                    // opaque Metal resources pointer

    // ---- Pixel store ----
    GLPixelStore pixel_store;

    // ---- Color material ----
    // (state tracked in color_material_* fields above)

    // ---- Stencil ----
    GLStencilState stencil;

    // ---- Polygon mode, line width, point size ----
    uint32_t polygon_mode_front;       // GL_FILL, GL_LINE, GL_POINT
    uint32_t polygon_mode_back;
    float    line_width;
    float    point_size;
    uint32_t shade_model;              // GL_FLAT or GL_SMOOTH

    // ---- Polygon offset ----
    float    polygon_offset_factor;
    float    polygon_offset_units;

    // ---- Attrib stack ----
    GLAttribStackEntry attrib_stack[GL_MAX_ATTRIB_STACK_DEPTH];
    int      attrib_stack_depth;

    // ---- Selection/feedback ----
    uint32_t render_mode;              // GL_RENDER, GL_SELECT, GL_FEEDBACK
    uint32_t selection_buffer_mac_ptr;
    int32_t  selection_buffer_size;
    int32_t  selection_buffer_offset;  // current write position in selection buffer
    int32_t  selection_hit_count;      // number of hit records written
    bool     selection_hit_flag;       // true if any primitive was drawn since last name stack change
    uint32_t selection_hit_z_min;      // min Z (as GLuint) for current hit
    uint32_t selection_hit_z_max;      // max Z (as GLuint) for current hit
    uint32_t feedback_buffer_mac_ptr;
    int32_t  feedback_buffer_size;
    uint32_t feedback_type;
    int32_t  name_stack_depth;
    uint32_t name_stack[64];

    // ---- Clear values ----
    float    clear_color[4];
    float    clear_depth;
    int32_t  clear_stencil;
    float    clear_accum[4];
    float    clear_index;

    // ---- Hints ----
    uint32_t hint_perspective_correction;
    uint32_t hint_point_smooth;
    uint32_t hint_line_smooth;
    uint32_t hint_polygon_smooth;
    uint32_t hint_fog;

    // ---- Logic op ----
    uint32_t logic_op_mode;

    // ---- Color mask ----
    bool     color_mask[4];            // r, g, b, a

    // ---- Raster position ----
    float    raster_pos[4];
    bool     raster_pos_valid;

    // ---- Pixel transfer ----
    float    pixel_zoom_x;
    float    pixel_zoom_y;
    float    pixel_transfer_red_scale;
    float    pixel_transfer_green_scale;
    float    pixel_transfer_blue_scale;
    float    pixel_transfer_alpha_scale;
    float    pixel_transfer_red_bias;
    float    pixel_transfer_green_bias;
    float    pixel_transfer_blue_bias;
    float    pixel_transfer_alpha_bias;
    float    pixel_transfer_depth_scale;
    float    pixel_transfer_depth_bias;
    // Pixel maps (small tables, max 256 entries each)
    float    pixel_map_i_to_r[256];
    float    pixel_map_i_to_g[256];
    float    pixel_map_i_to_b[256];
    float    pixel_map_i_to_a[256];
    float    pixel_map_r_to_r[256];
    float    pixel_map_g_to_g[256];
    float    pixel_map_b_to_b[256];
    float    pixel_map_a_to_a[256];
    int      pixel_map_i_to_r_size;
    int      pixel_map_i_to_g_size;
    int      pixel_map_i_to_b_size;
    int      pixel_map_i_to_a_size;
    int      pixel_map_r_to_r_size;
    int      pixel_map_g_to_g_size;
    int      pixel_map_b_to_b_size;
    int      pixel_map_a_to_a_size;

    // ---- Index mode state (RGBA always active, stored for completeness) ----
    float    current_index;
    uint32_t index_write_mask;

    // ---- Edge flag ----
    bool     current_edge_flag;

    // ---- Line/polygon stipple ----
    int32_t  line_stipple_factor;
    uint16_t line_stipple_pattern;
    uint32_t polygon_stipple[32];      // 32x32 bit pattern

    // ---- Draw/read buffer ----
    uint32_t draw_buffer;              // GL_FRONT, GL_BACK, etc.
    uint32_t read_buffer;

    // ---- Accumulation buffer ----
    float   *accum_buffer;             // lazily allocated: width * height * 4 floats
    int      accum_width;
    int      accum_height;
    bool     accum_allocated;

    // ---- Clip planes ----
    double   clip_planes[6][4];
    bool     clip_plane_enabled[6];

    // ---- Evaluators ----
    GLEvaluatorMap1 eval_map1[GL_EVAL_MAX_TARGETS];   // indexed 0-8 by target
    GLEvaluatorMap2 eval_map2[GL_EVAL_MAX_TARGETS];
    bool     eval_map1_enabled[GL_EVAL_MAX_TARGETS];
    bool     eval_map2_enabled[GL_EVAL_MAX_TARGETS];
    // Grid parameters (glMapGrid1/2)
    int32_t  grid1_un;
    float    grid1_u1, grid1_u2;
    int32_t  grid2_un, grid2_vn;
    float    grid2_u1, grid2_u2, grid2_v1, grid2_v2;

    // ---- Imaging subset (OpenGL 1.2) ----
    // Color tables: GL_COLOR_TABLE(0x80D0)=0, GL_POST_CONVOLUTION(0x80D1)=1, GL_POST_COLOR_MATRIX(0x80D2)=2
    GLColorTable        color_tables[3];
    bool                color_table_enabled;
    bool                post_convolution_color_table_enabled;
    bool                post_color_matrix_color_table_enabled;

    // Convolution: GL_CONVOLUTION_1D(0x8010)=0, GL_CONVOLUTION_2D(0x8011)=1
    GLConvolutionFilter convolution_filters[2];
    GLSeparableFilter   separable_filter;
    bool                convolution_1d_enabled;
    bool                convolution_2d_enabled;
    bool                separable_2d_enabled;

    // Histogram and minmax
    GLHistogramState    histogram;
    GLMinmaxState       minmax;
    bool                histogram_enabled;
    bool                minmax_enabled;

    // ---- Error ----
    uint32_t last_error;               // GLenum: GL_NO_ERROR, etc.
};


/*
 *  Public API declarations
 */

// Current GL context (singleton for now)
extern GLContext *gl_current_context;

// Initialize GL context to default state
extern void GLContextInit(GLContext *ctx);

// Initialize all GL TVECTs in SheepMem (called during ThunksInit)
extern bool GLThunksInit();

// Multiplexed dispatch entry point (called from execute_native_op)
// float_bits points to FPR values reinterpreted as uint32 pairs
extern uint32_t GLDispatch(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                           uint32_t r7, uint32_t r8, uint32_t r9, uint32_t r10,
                           const uint32_t *float_bits, int num_float_args);

// @autoreleasepool wrapper around GLDispatch (defined in gfxaccel_arc_shim.mm).
// See the RaveDispatchARC comment for the rationale.  Called from
// sheepshaver_glue.cpp:NATIVE_GL_DISPATCH in
// place of direct GLDispatch so the emul thread drains autoreleased MTL*/NS*
// temporaries every dispatch call.
extern uint32_t GLDispatchARC(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                              uint32_t r7, uint32_t r8, uint32_t r9, uint32_t r10,
                              const uint32_t *float_bits, int num_float_args);

// Install library hooks to intercept GL/AGL/GLU/GLUT function lookups
extern void GLInstallHooks();

// Clear the GL install latches for a guest soft reboot so GLInstallHooks
// re-patches the freshly reloaded GL/AGL/GLU libraries (see GfxAccelResetForReboot).
extern void GLResetForReboot(void);

// TVECT array indexed by sub-opcode (for stub-patching path)
extern uint32_t gl_method_tvects[];

// TVECT array for dispatch-table path (sets arg-shift flag before dispatch)
extern uint32_t gl_dt_method_tvects[];

// Per-slot diagnostic no-op TVECTs for unmapped context-handle dispatch slots
// (indexed by slot 0..GL_CTX_DISPATCH_SLOTS-1; see GLPopulateDispatchTable)
extern uint32_t gl_dt_diag_tvects[];

// Scratch word for sub-opcode passing (same pattern as RAVE)
extern uint32_t gl_scratch_addr;

// Dispatch-table flag word: 1 = args shifted (ctx in R3), 0 = normal
extern uint32_t gl_dt_flag_addr;

// Logging control (os_log-backed; gated by gl_logging_enabled + ACCEL_LOG_VERBOSE)
#include "gfx_log.h"
#if ACCEL_LOGGING_ENABLED
#ifdef __APPLE__
#include <os/log.h>
extern os_log_t gl_log;        /* defined in gl_dispatch.cpp */
extern os_log_t gl_metal_log;  /* defined in gl_dispatch.cpp */
#endif
extern bool gl_logging_enabled;
#ifdef __APPLE__
#define GL_LOG(fmt, ...)        do { if (gl_logging_enabled) os_log(gl_log, fmt, ##__VA_ARGS__); } while (0)
#define GL_VLOG(fmt, ...)       do { if (gl_logging_enabled && ACCEL_LOG_VERBOSE) os_log(gl_log, fmt, ##__VA_ARGS__); } while (0)
#define GL_METAL_LOG(fmt, ...)  do { if (gl_logging_enabled) os_log(gl_metal_log, fmt, ##__VA_ARGS__); } while (0)
#define GL_METAL_VLOG(fmt, ...) do { if (gl_logging_enabled && ACCEL_LOG_VERBOSE) os_log(gl_metal_log, fmt, ##__VA_ARGS__); } while (0)
#else
#define GL_LOG(...)        do { if (gl_logging_enabled) GFX_DEBUG_EMIT("GL: ", __VA_ARGS__); } while (0)
#define GL_VLOG(...)       do { if (gl_logging_enabled && ACCEL_LOG_VERBOSE) GFX_DEBUG_EMIT("GL: ", __VA_ARGS__); } while (0)
#define GL_METAL_LOG(...)  do { if (gl_logging_enabled) GFX_DEBUG_EMIT("GL_METAL: ", __VA_ARGS__); } while (0)
#define GL_METAL_VLOG(...) do { if (gl_logging_enabled && ACCEL_LOG_VERBOSE) GFX_DEBUG_EMIT("GL_METAL: ", __VA_ARGS__); } while (0)
#endif
#else /* !ACCEL_LOGGING_ENABLED */
static constexpr bool gl_logging_enabled = false;
#define GL_LOG(fmt, ...)        do {} while (0)
#define GL_VLOG(fmt, ...)       do {} while (0)
#define GL_METAL_LOG(fmt, ...)  do {} while (0)
#define GL_METAL_VLOG(fmt, ...) do {} while (0)
#endif

// Function signature table for FPR extraction
extern GLFuncSignature gl_func_signatures[];

// GPU backend renderer functions (Metal: gl_metal_renderer.mm; OpenGL: gl/gl_ffp_renderer.cpp)
extern void GLMetalInit(GLContext *ctx);
extern void GLMetalBeginFrame(GLContext *ctx);
extern void GLMetalClear(GLContext *ctx, uint32_t mask);
extern void GLMetalEndFrame(GLContext *ctx);
extern void GLMetalFlushImmediateMode(GLContext *ctx);
extern void GLMetalRelease(GLContext *ctx);
/* Primary upload shape used by gl_state.cpp / gl_engine.cpp (BGRA8 host bytes). */
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
extern void GLMetalDrawPixels(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len);
extern void GLMetalBitmap(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len);
// Read a rectangle from the framebuffer into a malloc'd BGRA8 buffer. Caller must free().
// Returns NULL on failure. Output length written to *out_len.
extern uint8_t *GLMetalReadFramebufferRect(GLContext *ctx, int x, int y, int width, int height, int *out_len);

/*
 *  Per-engine overlay lifecycle + gfxaccel_resources fan-out
 *  hooks for GL (mirrors RAVE's overlay pattern).
 *
 *  gl_overlay_bind / gl_overlay_unbind / gl_overlay_present are called from
 *  gl_engine.cpp's NativeAGLSetDrawable / NativeAGLSwapBuffers. They own
 *  the kGfxEngineGL fleet slot's overlay texture lifetime and emit a
 *  kLayerSlotOverlay CompositeLayer via MetalCompositorSubmitFrame on
 *  present.
 *
 *  gl_has_active_overlay / gl_get_overlay_dims / gl_release_overlay_for_detach
 *  are the C-linkage probes consumed by gl_engine.cpp's GLOnAttach / GLOnDetach
 *  fan-out handlers (registered with gfxaccel_resources at GLInstallHooks).
 */
#ifdef __cplusplus
extern "C" {
#endif
void gl_overlay_bind(int32_t left, int32_t top, int32_t width, int32_t height);
void gl_overlay_unbind(void);
void gl_overlay_present(void);
int  gl_has_active_overlay(void);
int  gl_get_overlay_dims(uint32_t *outW, uint32_t *outH);
void gl_release_overlay_for_detach(void);
int  GLContextGetOffscreenDrawable(GLContext *ctx,
                                   uint32_t *outW,
                                   uint32_t *outH,
                                   uint32_t *outRowbytes,
                                   uint32_t *outBaseaddr);
uint64_t GLCompositeLatestOffscreenToGuestSurfaceUsingLatestExtentDirtyRect(
	uint32_t dstBaseaddr,
	uint32_t dstRowbytes,
	uint32_t dstDepthBits,
	int32_t dirtyX,
	int32_t dirtyY,
	int32_t dirtyWidth,
	int32_t dirtyHeight);
uint64_t GLCompositeLatestOffscreenToGuestSurfaceUsingLatestExtentIfNotSuppressed(
	uint32_t dstBaseaddr,
	uint32_t dstRowbytes,
	uint32_t dstDepthBits);
#ifdef __cplusplus
}
#endif


#endif /* GL_ENGINE_H */

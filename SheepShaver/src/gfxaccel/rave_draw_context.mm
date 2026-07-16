/*
 *  rave_draw_context.mm - RAVE draw context lifecycle and state management
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Implements:
 *    - NativeDrawPrivateNew: allocates RaveDrawPrivate, populates TQADrawContext TVECTs
 *    - NativeDrawPrivateDelete: frees RaveDrawPrivate, removes from context table
 *    - NativeSetFloat/SetInt/SetPtr: store state values by tag ID
 *    - NativeGetFloat/GetInt/GetPtr: retrieve state values by tag ID
 *
 *  ObjC++ (.mm) for future Metal integration.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "rave_engine.h"
#include "rave_metal_renderer.h"

#include <cstring>

// Forward declare Mac_sysalloc (from macos_util.h) to avoid UIKit header conflicts.
extern uint32 Mac_sysalloc(uint32 size);

// RAVE error codes (must match TQAError enum in RAVE.h)
#define kQANoErr                    0
#define kQAError                    1
#define kQANotSupported             3
#define kQAParamErr                 5

// kQAContext flag bits
#define kQAContext_NoZBuffer  (1 << 0)

/*
 *  Context table
 *
 *  Maps uint32 handles (1-based) to native RaveDrawPrivate pointers.
 *  Max 8 simultaneous contexts (games typically use 1-2).
 */
#ifndef RAVE_MAX_CONTEXTS
#define RAVE_MAX_CONTEXTS 8
#endif

static RaveDrawPrivate *context_table[RAVE_MAX_CONTEXTS] = {};

int rave_context_count = 0;
uint32_t rave_current_draw_context_addr = 0;

RaveDrawPrivate *RaveGetContext(uint32 handle)
{
	if (handle == 0 || handle > RAVE_MAX_CONTEXTS)
		return nullptr;
	return context_table[handle - 1];
}

static uint32_t AllocContextHandle(RaveDrawPrivate *ctx)
{
	// Find a free slot
	for (int i = 0; i < RAVE_MAX_CONTEXTS; i++) {
		if (context_table[i] == nullptr) {
			context_table[i] = ctx;
			return (uint32_t)(i + 1);
		}
	}
	return 0;  // No free slots
}

static void FreeContextHandle(uint32_t handle)
{
	if (handle > 0 && handle <= RAVE_MAX_CONTEXTS) {
		context_table[handle - 1] = nullptr;
	}
}

/*
 *  State defaults per RAVE spec
 *
 *  Tag IDs are globally unique across float/int/ptr types.
 *  See RAVE.h TQATagFloat/TQATagInt/TQATagPtr enums.
 */
static void InitStateDefaults(RaveDrawPrivate *ctx, uint32 flags)
{
	// Zero all state first
	memset(ctx->state, 0, sizeof(ctx->state));

	// kQATag_ZFunction (0) - int: kQAZFunction_None (0) if NoZBuffer, else kQAZFunction_LT (1)
	ctx->state[0].i = (flags & kQAContext_NoZBuffer) ? 0 : 1;

	// kQATag_ColorBG_a (1) through kQATag_ColorBG_b (4) - float: 0.0
	ctx->state[1].f = 0.0f;
	ctx->state[2].f = 0.0f;
	ctx->state[3].f = 0.0f;
	ctx->state[4].f = 0.0f;

	// kQATag_Width (5) - float: 1.0
	ctx->state[5].f = 1.0f;

	// kQATag_ZMinOffset (6) - float: 0.0
	ctx->state[6].f = 0.0f;

	// kQATag_ZMinScale (7) - float: 1.0/65535.0
	ctx->state[7].f = 1.0f / 65535.0f;

	// kQATag_Antialias (8) - int: kQAAntialias_Fast (1)
	ctx->state[8].i = 1;

	// kQATag_Blend (9) - int: kQABlend_PreMultiply (0)
	ctx->state[9].i = 0;

	// kQATag_PerspectiveZ (10) - int: kQAPerspectiveZ_Off (0)
	ctx->state[10].i = 0;

	// kQATag_TextureFilter (11) - int: kQATextureFilter_Fast (0)
	ctx->state[11].i = 0;

	// kQATag_TextureOp (12) - int: kQATextureOp_None (0)
	ctx->state[12].i = 0;

	// kQATag_Texture (13) - ptr: NULL
	ctx->state[13].i = 0;

	// kQATag_CSGTag (14) - int: 0xFFFFFFFF (kQACSGTag_None)
	ctx->state[14].i = 0xFFFFFFFF;

	// Fog state defaults
	ctx->state[17].i = 0;      // kQATag_FogMode: None
	ctx->state[18].f = 0.0f;   // kQATag_FogColor_a
	ctx->state[19].f = 0.0f;   // kQATag_FogColor_r
	ctx->state[20].f = 0.0f;   // kQATag_FogColor_g
	ctx->state[21].f = 0.0f;   // kQATag_FogColor_b
	ctx->state[22].f = 0.0f;   // kQATag_FogStart
	ctx->state[23].f = 1.0f;   // kQATag_FogEnd
	ctx->state[24].f = 1.0f;   // kQATag_FogDensity
	ctx->state[25].f = 1.0f;   // kQATag_FogMaxDepth

	// Depth write mask default (nonzero = writes enabled)
	ctx->state[28].i = 1;      // kQATag_ZBufferMask: enabled

	// Alpha/Z-sort state defaults
	ctx->state[29].i = 0;      // kQATag_ZSortedHint: off
	ctx->state[31].i = 0;      // kQATag_AlphaTestFunc: None (always pass)

	// Channel mask default (all channels enabled)
	ctx->state[27].i = 0xF;    // kQATag_ChannelMask: RGBA all enabled

	// Multi-texture defaults
	ctx->state[35].i = 0;      // kQATag_MultiTextureOp: Add
	ctx->state[41].f = 0.5f;   // kQATag_MipmapBias: spec default is 0.5
	ctx->state[42].f = 0.5f;   // kQATag_MultiTextureMipmapBias: spec default is 0.5
	ctx->state[46].f = 0.0f;   // kQATag_AlphaTestRef
	ctx->state[51].f = 0.5f;   // kQATag_MultiTextureFactor: 0.5

	// Bitmap scale defaults
	ctx->state[52].f = 1.0f;   // kQATag_BitmapScale_x: default 1.0 (no scaling)
	ctx->state[53].f = 1.0f;   // kQATag_BitmapScale_y: default 1.0 (no scaling)
	ctx->state[54].i = 0;      // kQATag_BitmapFilter: kQAFilter_Fast (nearest)

	// Remaining tags 15-59: zero from memset

	// GL tag defaults (100-153)
	ctx->state[100].i = 0;      // kQATagGL_DrawBuffer: back
	ctx->state[101].i = 0;      // kQATagGL_TextureWrapU: Repeat
	ctx->state[102].i = 0;      // kQATagGL_TextureWrapV: Repeat
	ctx->state[103].i = 0;      // kQATagGL_TextureMagFilter: Nearest
	ctx->state[104].i = 0;      // kQATagGL_TextureMinFilter: Nearest
	ctx->state[105].i = 0;      // kQATagGL_ScissorXMin
	ctx->state[106].i = 0;      // kQATagGL_ScissorYMin
	ctx->state[107].i = ctx->width;   // kQATagGL_ScissorXMax
	ctx->state[108].i = ctx->height;  // kQATagGL_ScissorYMax
	ctx->state[109].i = 1;      // kQATagGL_BlendSrc: GL_ONE (0x0001)
	ctx->state[110].i = 0;      // kQATagGL_BlendDst: GL_ZERO (0x0000)
	ctx->state[111].i = 0xFFFF; // kQATagGL_LinePattern: solid
	ctx->state[112].f = 1.0f;   // kQATagGL_DepthBG
	// Tags 113-116: TextureBorder a/r/g/b (default 0.0 from memset)
	// Tags 117-148: AreaPattern0-31 (default 0xFFFFFFFF = all solid)
	for (int i = 117; i <= 148; i++) ctx->state[i].i = 0xFFFFFFFF;
	ctx->state[149].i = 1;      // kQATagGL_LinePatternFactor
	// Tags 150-153: TextureEnvColor a/r/g/b (default 0.0 from memset)

	// ATI state defaults (all zero from memset in RaveDrawPrivate allocation)
	ctx->ati_fog_active = false;
	memset(ctx->ati_state, 0, sizeof(ctx->ati_state));
	uint32_t atiIntDefaults[RAVE_ATI_TAG_COUNT] = {};
	RaveATIInitializeIntDefaults(atiIntDefaults, RAVE_ATI_TAG_COUNT);
	for (uint32_t i = 0; i < RAVE_ATI_TAG_COUNT; i++) {
		ctx->ati_state[i].i = atiIntDefaults[i];
	}
	RAVE_LOG("InitStateDefaults: ATI UT probe tag %u default=%u (0x%08x)",
	         kRaveATIUnrealTournamentProbeTag,
	         ctx->ati_state[kRaveATIUnrealTournamentProbeIndex].i,
	         ctx->ati_state[kRaveATIUnrealTournamentProbeIndex].i);
}

/*
 *  TQADrawContext struct field -> TVECT sub-opcode mapping
 *
 *  The struct has 35 method pointer fields starting at offset 8.
 *  Fields 0-34 all map to draw method tags.
 *  Fields 26-33 are RAVE 1.6 buffer access/clear methods.
 */
static const int kDrawContextMethodFields = 35;

// Maps struct field index (0-34) to rave_method_tvects sub-opcode, or -1 for no mapping
static const int struct_field_to_subopcode[35] = {
	0,  1,  2,  3,  4,  5,       // setFloat..getPtr
	6,  7,  8,  9,  10, 11, 12,  // drawPoint..drawBitmap
	13, 14, 15, 16, 17,          // renderStart..sync
	18, 19, 20, 21,              // submitVerticesGouraud..drawTriMeshTexture
	22, 23,                      // setNoticeMethod, getNoticeMethod
	24,                          // submitMultiTextureParams
	25,                          // accessDrawBuffer
	26, 27, 28, 29, 30,          // accessDrawBufferEnd, accessZBuffer, accessZBufferEnd, clearDrawBuffer, clearZBuffer
	31, 32, 33,                  // textureFromContext, bitmapFromContext, busy
	34                           // swapBuffers
};


/*
 *  NativeDrawPrivateNew - Create a new draw context
 *
 *  PPC args: r3=drawContext, r4=device, r5=rect, r6=clip, r7=flags
 *  Returns TQAError (0 = success)
 */
int32 NativeDrawPrivateNew(uint32 drawContextAddr, uint32 deviceAddr,
                           uint32 rectAddr, uint32 clipAddr, uint32 flags)
{
	if (rave_logging_enabled) {
		printf("RAVE DrawPrivateNew ENTER: ctx=0x%08x dev=0x%08x rect=0x%08x clip=0x%08x flags=0x%x\n",
		       drawContextAddr, deviceAddr, rectAddr, clipAddr, flags);

		// Dump context table state on entry
		printf("RAVE DrawPrivateNew: context_table state on entry (count=%d):\n", rave_context_count);
		for (int i = 0; i < RAVE_MAX_CONTEXTS; i++) {
			if (context_table[i] != nullptr) {
				printf("  slot[%d] = %p (occupied)\n", i, context_table[i]);
			}
		}
	}

	// Read TQARect from Mac memory (DDK struct order: left, right, top, bottom)
	int32 left   = (int32)ReadMacInt32(rectAddr + 0);
	int32 right  = (int32)ReadMacInt32(rectAddr + 4);
	int32 top    = (int32)ReadMacInt32(rectAddr + 8);
	int32 bottom = (int32)ReadMacInt32(rectAddr + 12);

	if (rave_logging_enabled)
		printf("RAVE DrawPrivateNew: rect raw=(%d,%d,%d,%d) size=%dx%d\n",
		       left, top, right, bottom, right - left, bottom - top);

	// Allocate RaveDrawPrivate on native heap
	RaveDrawPrivate *ctx = new RaveDrawPrivate();
	if (!ctx) {
		if (rave_logging_enabled)
			printf("RAVE DrawPrivateNew: FAIL - native allocation returned null\n");
		return kQAError;
	}
	if (rave_logging_enabled)
		printf("RAVE DrawPrivateNew: native alloc OK at %p\n", ctx);
	memset(ctx, 0, sizeof(RaveDrawPrivate));

	// Store dimensions and flags
	ctx->left   = left;
	ctx->top    = top;
	ctx->width  = right - left;
	ctx->height = bottom - top;
	ctx->flags  = flags;
	ctx->drawContextAddr = drawContextAddr;

	// Initialize state defaults per RAVE spec
	InitStateDefaults(ctx, flags);

	// Allocate vertex staging buffer
	ctx->vertexStagingCapacity = 65536;
	ctx->vertexStagingCount = 0;
	ctx->vertexStagingBuffer = new uint8_t[ctx->vertexStagingCapacity * RAVE_VERTEX_BYTES];

	// Allocate multi-texture UV staging buffer (parallel to vertexStagingBuffer)
	// 16 bytes per vertex: uOverW2(4) + vOverW2(4) + invW2(4) + pad(4)
	ctx->multiTexStagingBuffer = new uint8_t[ctx->vertexStagingCapacity * 16];
	ctx->multiTexStagingCount = 0;
	ctx->multiTextureActive = false;
	ctx->multiTextureHandle = 0;
	ctx->multiTextureOp = 0;
	ctx->multiTextureFactor = 0.0f;

	// Allocate Z-sort transparency buffer
	ctx->zsortBuffer = new ZSortTriangle[RAVE_ZSORT_MAX_TRIANGLES];
	ctx->zsortCount = 0;

	// Diagnostic frame counter
	ctx->frameCount = 0;

	// Register in context table
	uint32_t handle = AllocContextHandle(ctx);
	if (handle == 0) {
		if (rave_logging_enabled)
			printf("RAVE DrawPrivateNew: FAIL - no free context slots (all %d occupied)\n", RAVE_MAX_CONTEXTS);
		delete ctx;
		return kQAError;
	}
	if (rave_logging_enabled)
		printf("RAVE DrawPrivateNew: got handle=%d\n", handle);

	// Write handle into TQADrawContext.drawPrivate at offset 0
	WriteMacInt32(drawContextAddr + 0, handle);

	// Write version at offset 4
	// TQAVersion enum: kQAVersion_1_6 = 4 (NOT 0x01060000!)
	WriteMacInt32(drawContextAddr + 4, 4);

	// Write all 35 draw method TVECTs into TQADrawContext struct fields
	// Method pointer fields start at offset 8, each is 4 bytes
	for (int i = 0; i < kDrawContextMethodFields; i++) {
		int subopcode = struct_field_to_subopcode[i];
		uint32 tvect_addr = 0;
		if (subopcode >= 0) {
			tvect_addr = rave_method_tvects[subopcode];
		}
		WriteMacInt32(drawContextAddr + 8 + i * 4, tvect_addr);
	}

	rave_context_count++;

	// Initialize Metal overlay (viewport-sized) and per-context resources.
	// Shared-overlay retain call deleted — per-engine ownership
	// via gfxaccel_resources eliminates the shared refcount model entirely.
	// RaveCreateMetalOverlay vends a per-engine overlay texture from
	// gfxaccel_resources directly.
	RaveCreateMetalOverlay(ctx->left, ctx->top, ctx->width, ctx->height);
	RaveInitMetalResources(ctx);

	RAVE_LOG("DrawPrivateNew: handle=%d size=%dx%d contexts=%d",
	         handle, ctx->width, ctx->height, rave_context_count);

	// Diagnostic: dump TQADrawContext struct contents for crash debugging
	if (rave_logging_enabled) {
		printf("RAVE DrawContext at 0x%08x:\n", drawContextAddr);
		printf("  [+0] drawPrivate = %d\n", ReadMacInt32(drawContextAddr + 0));
		printf("  [+4] version     = %d\n", ReadMacInt32(drawContextAddr + 4));
		for (int i = 0; i < kDrawContextMethodFields; i++) {
			uint32 tvect = ReadMacInt32(drawContextAddr + 8 + i * 4);
			if (tvect != 0) {
				uint32 code_ptr = ReadMacInt32(tvect);
				printf("  [+%d] method[%d] = TVECT 0x%08x -> code 0x%08x\n",
				       8 + i * 4, i, tvect, code_ptr);
			} else {
				printf("  [+%d] method[%d] = NULL\n", 8 + i * 4, i);
			}
		}
	}

	// Track the most recent draw context for EngineGestalt(kQATIGestalt_CurrentContext)
	rave_current_draw_context_addr = drawContextAddr;

	return kQANoErr;
}


/*
 *  NativeDrawPrivateDelete - Destroy a draw context
 *
 *  PPC args: r3 = drawPrivate handle (value from TQADrawContext.drawPrivate)
 *  Returns void (we return 0 in gpr(3))
 */
int32 NativeDrawPrivateDelete(uint32 drawPrivateHandle)
{
	RAVE_LOG("DrawPrivateDelete: handle=%d", drawPrivateHandle);

	RaveDrawPrivate *ctx = RaveGetContext(drawPrivateHandle);
	if (!ctx) {
		RAVE_LOG("DrawPrivateDelete: invalid handle %d", drawPrivateHandle);
		return kQANoErr;
	}

	// Free vertex staging buffer
	delete[] ctx->vertexStagingBuffer;
	ctx->vertexStagingBuffer = nullptr;

	// Free multi-texture staging buffer
	delete[] ctx->multiTexStagingBuffer;
	ctx->multiTexStagingBuffer = nullptr;

	// Free per-draw scratch arenas
	delete[] ctx->drawScratchA;
	ctx->drawScratchA = nullptr;
	ctx->drawScratchACap = 0;
	delete[] ctx->drawScratchB;
	ctx->drawScratchB = nullptr;
	ctx->drawScratchBCap = 0;
	delete[] ctx->drawScratchF;
	ctx->drawScratchF = nullptr;
	ctx->drawScratchFCap = 0;

	// Free Z-sort transparency buffer
	delete[] ctx->zsortBuffer;
	ctx->zsortBuffer = nullptr;

	// Release Metal resources before freeing context
	RaveReleaseMetalResources(ctx);

	FreeContextHandle(drawPrivateHandle);
	delete ctx;
	rave_context_count--;

	// Overlay lifetime is scoped to the DMC MODE, not to draw-context churn.
	// When the last RAVE context goes away, LEAVE the cached overlay alive.
	// The gfxaccel_resources fan-out (RaveOnDetach in rave_engine.cpp) will
	// release it on real mode changes, and RaveOnAttach will re-vend at the
	// new resolution. Tearing down here caused visible black flashes on every
	// Nanosaur scene transition (menu ↔ gameplay) because the host's main-
	// thread VBL presented frames during the interval between destroy and the
	// next DrawPrivateNew → RaveCreateMetalOverlay call.

	// Clamp for safety
	if (rave_context_count <= 0) {
		rave_context_count = 0;
		rave_current_draw_context_addr = 0;
	}

	RAVE_LOG("DrawPrivateDelete: done, contexts=%d", rave_context_count);
	return kQANoErr;
}


/*
 *  State management helpers
 *
 *  All state methods receive drawContextAddr (Mac address of TQADrawContext),
 *  read the drawPrivate handle from offset 0, and look up the native context.
 */
static RaveDrawPrivate *GetContextFromDrawAddr(uint32 drawContextAddr)
{
	uint32 handle = ReadMacInt32(drawContextAddr + 0);
	return RaveGetContext(handle);
}


/*
 *  NativeSetFloat - Store a float state value
 *  PPC args: r3=drawContextAddr, r4=tag, r5=valueBits (float reinterpreted as uint32)
 */
int32 NativeSetFloat(uint32 drawContextAddr, uint32 tag, uint32 valueBits)
{
	RaveDrawPrivate *ctx = GetContextFromDrawAddr(drawContextAddr);
	if (!ctx) {
		RAVE_LOG("SetFloat: invalid context 0x%08x", drawContextAddr);
		return kQAError;
	}

	// ATI EngineSpecific range (1000+)
	if (tag >= 1000) {
		uint32_t ati_idx = tag - 1000;
		if (ati_idx < RAVE_ATI_TAG_COUNT) {
			float value;
			memcpy(&value, &valueBits, sizeof(float));
			ctx->ati_state[ati_idx].f = value;
			if (ati_idx == kRaveATIDepthWriteEnableIndex) {
				ctx->dirty_flags |= 1;
			}
			// Activate ATI fog override when fog-related tags are set (indices 2-9)
			if (ati_idx >= 2 && ati_idx <= 9) {
				ctx->ati_fog_active = true;
			}
		}
		// Silently ignore unknown engine-specific tags
		return kQANoErr;
	}
	// GL + standard range (0-153)
	// GL tags (100+) are stored in state[] but unused -- kQAOptional_OpenGL is not
	// advertised, so these values have no rendering effect. Stored silently for
	// compatibility with callers that set them.
	if (tag >= RAVE_MAX_TAG) return kQANoErr;

	// Tags 6,7 are read-only per spec; silently accept writes for compatibility
	if (tag == 6 || tag == 7) return kQANoErr;

	// Store float bits directly (PPC passes float as uint32 in r5)
	float value;
	memcpy(&value, &valueBits, sizeof(float));
	ctx->state[tag].f = value;
	ctx->dirty_flags |= (1 << (tag & 31));

	return kQANoErr;
}


/*
 *  NativeSetInt - Store an integer state value
 *  PPC args: r3=drawContextAddr, r4=tag, r5=value
 */
int32 NativeSetInt(uint32 drawContextAddr, uint32 tag, uint32 value)
{
	RaveDrawPrivate *ctx = GetContextFromDrawAddr(drawContextAddr);
	if (!ctx) {
		RAVE_LOG("SetInt: invalid context 0x%08x", drawContextAddr);
		return kQAError;
	}

	// ATI EngineSpecific range (1000+)
	if (tag >= 1000) {
		uint32_t ati_idx = tag - 1000;
		if (ati_idx < RAVE_ATI_TAG_COUNT) {
			ctx->ati_state[ati_idx].i = value;
			if (ati_idx == kRaveATIDepthWriteEnableIndex) {
				ctx->dirty_flags |= 1;
			}
			// Activate ATI fog override when fog-related tags are set (indices 2-9)
			if (ati_idx >= 2 && ati_idx <= 9) {
				ctx->ati_fog_active = true;
			}
		}
		// Silently ignore unknown engine-specific tags
		return kQANoErr;
	}
	// GL + standard range (0-153)
	// GL tags (100+) are stored in state[] but unused -- kQAOptional_OpenGL is not
	// advertised, so these values have no rendering effect. Stored silently for
	// compatibility with callers that set them.
	if (tag >= RAVE_MAX_TAG) return kQANoErr;

	ctx->state[tag].i = value;
	ctx->dirty_flags |= (1 << (tag & 31));

	// Reset ATI fog when standard fog mode is explicitly set
	if (tag == 17 && value != 0) {
		ctx->ati_fog_active = false;
	}

	return kQANoErr;
}

static void WriteATIRaveExtFuncsTable(uint32 ptr)
{
	WriteMacInt32(ptr + kRaveATIRaveExtFuncsSlotClearDrawBuffer * 4,
	              rave_method_tvects[kRaveATIClearDrawBuffer]);
	WriteMacInt32(ptr + kRaveATIRaveExtFuncsSlotClearZBuffer * 4,
	              rave_method_tvects[kRaveATIClearZBuffer]);
	WriteMacInt32(ptr + kRaveATIRaveExtFuncsSlotTextureUpdate * 4,
	              rave_method_tvects[kRaveATITextureUpdate]);
	WriteMacInt32(ptr + kRaveATIRaveExtFuncsSlotBindCodeBook * 4,
	              rave_method_tvects[kRaveATIBindCodeBook]);
}

static uint32 EnsureATIRaveExtFuncsTable(RaveDrawPrivate *ctx)
{
	uint32 ptr = ctx->ati_state[kRaveATIRaveExtFuncsIndex].i;
	if (ptr == 0) {
		ptr = Mac_sysalloc(kRaveATIRaveExtFuncsEntryCount * 4);
		if (ptr == 0) {
			RAVE_LOG("GetPtr: kATIRaveExtFuncs allocation failed");
			return 0;
		}
		ctx->ati_state[kRaveATIRaveExtFuncsIndex].i = ptr;
	}

	WriteATIRaveExtFuncsTable(ptr);
	return ptr;
}


/*
 *  NativeSetPtr - Store a pointer state value (Mac address)
 *  PPC args: r3=drawContextAddr, r4=tag, r5=ptr
 */
int32 NativeSetPtr(uint32 drawContextAddr, uint32 tag, uint32 ptr)
{
	RaveDrawPrivate *ctx = GetContextFromDrawAddr(drawContextAddr);
	if (!ctx) {
		RAVE_LOG("SetPtr: invalid context 0x%08x", drawContextAddr);
		return kQAError;
	}

	// ATI EngineSpecific range (1000+)
	if (tag >= 1000) {
		uint32_t ati_idx = tag - 1000;
		if (ati_idx == kRaveATIRaveExtFuncsIndex) {
			if (ptr == 0) {
				RAVE_LOG("SetPtr: kATIRaveExtFuncs ignored null pointer");
				return kQAParamErr;
			}
			WriteATIRaveExtFuncsTable(ptr);
			ctx->ati_state[kRaveATIRaveExtFuncsIndex].i = ptr;
			RAVE_LOG("SetPtr: kATIRaveExtFuncs -> delivered 4 TVECT addresses to 0x%08x", ptr);
			return kQANoErr;
		}
		if (ati_idx < RAVE_ATI_TAG_COUNT) {
			ctx->ati_state[ati_idx].i = ptr;
		}
		// Silently ignore unknown engine-specific tags
		return kQANoErr;
	}
	// GL + standard range (0-153)
	if (tag >= RAVE_MAX_TAG) return kQANoErr;

	ctx->state[tag].i = ptr;  // Mac address stored as uint32
	ctx->dirty_flags |= (1 << (tag & 31));

	return kQANoErr;
}


/*
 *  NativeGetFloat - Retrieve a float state value
 *  PPC args: r3=drawContextAddr, r4=tag
 *  Returns: float bits as uint32
 */
uint32 NativeGetFloat(uint32 drawContextAddr, uint32 tag)
{
	RaveDrawPrivate *ctx = GetContextFromDrawAddr(drawContextAddr);
	if (!ctx) {
		RAVE_LOG("GetFloat: invalid context 0x%08x", drawContextAddr);
		return 0;
	}

	// ATI EngineSpecific range (1000+)
	if (tag >= 1000) {
		uint32_t ati_idx = tag - 1000;
		if (ati_idx < RAVE_ATI_TAG_COUNT) {
			uint32 bits;
			memcpy(&bits, &ctx->ati_state[ati_idx].f, sizeof(uint32));
			return bits;
		}
		return 0;
	}
	// GL + standard range (0-153)
	if (tag >= RAVE_MAX_TAG) return 0;

	// Return float bits as uint32 (PPC expects float in gpr(3) for this callback)
	uint32 bits;
	memcpy(&bits, &ctx->state[tag].f, sizeof(uint32));
	return bits;
}


/*
 *  NativeGetInt - Retrieve an integer state value
 *  PPC args: r3=drawContextAddr, r4=tag
 *  Returns: state value
 */
uint32 NativeGetInt(uint32 drawContextAddr, uint32 tag)
{
	RaveDrawPrivate *ctx = GetContextFromDrawAddr(drawContextAddr);
	if (!ctx) {
		RAVE_LOG("GetInt: invalid context 0x%08x", drawContextAddr);
		return 0;
	}

	// ATI EngineSpecific range (1000+)
	if (tag >= 1000) {
		uint32_t ati_idx = tag - 1000;
		if (ati_idx < RAVE_ATI_TAG_COUNT) {
			uint32_t value = ctx->ati_state[ati_idx].i;
			if (tag == kRaveATIUnrealTournamentProbeTag) {
				RAVE_VLOG("GetInt: ATI UT probe tag %u -> %u (0x%08x)", tag, value, value);
			}
			return value;
		}
		return 0;
	}
	// GL + standard range (0-153)
	if (tag >= RAVE_MAX_TAG) return 0;

	return ctx->state[tag].i;
}


/*
 *  NativeGetPtr - Retrieve a pointer state value (Mac address)
 *  PPC args: r3=drawContextAddr, r4=tag
 *  Returns: Mac address as uint32
 */
uint32 NativeGetPtr(uint32 drawContextAddr, uint32 tag)
{
	RaveDrawPrivate *ctx = GetContextFromDrawAddr(drawContextAddr);
	if (!ctx) {
		RAVE_LOG("GetPtr: invalid context 0x%08x", drawContextAddr);
		return 0;
	}

	// ATI EngineSpecific range (1000+)
	if (tag >= 1000) {
		uint32_t ati_idx = tag - 1000;
		if (ati_idx == kRaveATIRaveExtFuncsIndex) {
			uint32 ptr = EnsureATIRaveExtFuncsTable(ctx);
			RAVE_LOG("GetPtr: kATIRaveExtFuncs -> 0x%08x", ptr);
			return ptr;
		}
		if (ati_idx < RAVE_ATI_TAG_COUNT) {
			return ctx->ati_state[ati_idx].i;
		}
		return 0;
	}
	// GL + standard range (0-153)
	if (tag >= RAVE_MAX_TAG) return 0;

	return ctx->state[tag].i;  // Mac address stored as uint32
}

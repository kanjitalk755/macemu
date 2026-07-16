/*
 *  rave_engine.cpp - RAVE engine registration and callbacks
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Implements the three core RAVE engine callbacks:
 *    - NativeEngineGetMethod: returns TVECTs for all 18 engine method tags
 *    - NativeEngineGestalt: responds to all 18 gestalt selectors
 *    - NativeEngineCheckDevice: accepts both memory and GDevice device types
 *
 *  Also implements RaveRegisterEngine() which locates QARegisterEngine
 *  via FindLibSymbol and calls it with our EngineGetMethod TVECT.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "macos_util.h"
#include "thunks.h"
#include "rave_engine.h"
#include "rave_device_summary.h"
#include "rave_engine_enable_policy.h"
#include "rave_engine_identity.h"
#include "rave_metal_renderer.h"
#include "dsp_pixmap_offsets.h"
#include "gfxaccel_resources.h"
#include "metal_compositor.h"  // MetalCompositorSubmitFrame_ClearCachedOverlay

#include <cstring>
#include <cmath>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstdarg>

#define DEBUG 0
#include "debug.h"


// RAVE error codes (must match TQAError enum in RAVE.h)
#define kQANoErr                    0
#define kQAError                    1
#define kQAOutOfMemory              2
#define kQANotSupported             3
#define kQAOutOfDate                4
#define kQAParamErr                 5
#define kQAGestaltUnknown           6
#define kQADisplayModeUnsupported   7
#define kQAOutOfVideoMemory         8

// Free list for recycling TQADrawContext Mac addresses.
// SheepMem::ReserveProc is a permanent bump allocator -- without recycling,
// apps that create/destroy contexts every frame (e.g. RAVE Bench) exhaust
// the 512KB SheepMem region and corrupt PPC memory, causing hangs.
static std::vector<uint32> rave_ctx_free_list;

static uint8_t rave_last_cl8_color_table_rgb[768];
static bool rave_last_cl8_color_table_valid = false;

static void RaveRememberCL8ColorTableSnapshot(const uint32_t *clut, uint32_t count)
{
	if (clut == nullptr || count != 256)
		return;

	for (uint32_t i = 0; i < 256; i++) {
		const uint32_t bgra = clut[i];
		rave_last_cl8_color_table_rgb[i * 3 + 0] = (uint8_t)((bgra >> 16) & 0xffu);
		rave_last_cl8_color_table_rgb[i * 3 + 1] = (uint8_t)((bgra >> 8) & 0xffu);
		rave_last_cl8_color_table_rgb[i * 3 + 2] = (uint8_t)(bgra & 0xffu);
	}
	rave_last_cl8_color_table_valid = true;
}

bool RaveGetLastCL8ColorTableRGBSnapshot(uint8_t outRGB[768])
{
	if (!rave_last_cl8_color_table_valid || outRGB == nullptr)
		return false;
	memcpy(outRGB, rave_last_cl8_color_table_rgb, sizeof(rave_last_cl8_color_table_rgb));
	return true;
}

// kQAPixel_* constants already defined in enum below (line ~155)

// TQAEngineMethodTag values (0-17)
// These are the tags passed to EngineGetMethod by the RAVE manager.
// Order MUST match the DDK's TQAEngineMethodTag enum in RAVESystem.h exactly.
//
// IMPORTANT: GetMethod and MethodNameToIndex are NOT in this enum.
// GetMethod is the callback passed to QARegisterEngine (our sub-opcode 100 TVECT).
// MethodNameToIndex is also not queried via EngineGetMethod.
enum {
	kQADrawPrivateNew = 0,
	kQADrawPrivateDelete,        // 1
	kQAEngineCheckDevice,        // 2  (DDK: kQAEngineCheckDevice = 2)
	kQAEngineGestalt,            // 3  (DDK: kQAEngineGestalt = 3)
	kQATextureNew,               // 4
	kQATextureDetach,            // 5
	kQATextureDelete,            // 6
	kQABitmapNew,                // 7
	kQABitmapDetach,             // 8
	kQABitmapDelete,             // 9
	kQAColorTableNew,            // 10
	kQAColorTableDelete,         // 11
	kQATextureBindColorTable,    // 12
	kQABitmapBindColorTable,     // 13
	kQAAccessTexture,            // 14
	kQAAccessTextureEnd,         // 15
	kQAAccessBitmap,             // 16
	kQAAccessBitmapEnd,          // 17
	kQAEngineMethodTagCount = 18
};

// TQAGestaltSelector values (0-17)
enum {
	kQAGestalt_OptionalFeatures = 0,
	kQAGestalt_FastFeatures,
	kQAGestalt_VendorID,
	kQAGestalt_EngineID,
	kQAGestalt_Revision,
	kQAGestalt_ASCIINameLength,
	kQAGestalt_ASCIIName,
	kQAGestalt_TextureMemory,
	kQAGestalt_FastTextureMemory,
	kQAGestalt_DrawContextPixelTypesAllowed,
	kQAGestalt_DrawContextPixelTypesPreferred,
	kQAGestalt_TexturePixelTypesAllowed,
	kQAGestalt_TexturePixelTypesPreferred,
	kQAGestalt_BitmapPixelTypesAllowed,
	kQAGestalt_BitmapPixelTypesPreferred,
	kQAGestalt_OptionalFeatures2,
	kQAGestalt_MultiTextureMax,
	kQAGestalt_NumSelectors
};

static void RaveLogDeviceSummary(const char *label, uint32 device)
{
	if (device == 0) {
		RAVE_LOG("%s: device=NULL", label);
		return;
	}

	uint32 deviceType = ReadMacInt32(device + kRaveDeviceOff_Type);
	if (deviceType == kRaveDeviceTypeMemory) {
		uint32 rowBytes  = ReadMacInt32(device + kRaveDeviceOff_MemoryRowBytes);
		uint32 pixelType = ReadMacInt32(device + kRaveDeviceOff_MemoryPixelType);
		uint32 width     = ReadMacInt32(device + kRaveDeviceOff_MemoryWidth);
		uint32 height    = ReadMacInt32(device + kRaveDeviceOff_MemoryHeight);
		uint32 baseAddr  = ReadMacInt32(device + kRaveDeviceOff_MemoryBaseAddr);
		RAVE_LOG("%s: device=0x%08x type=%s rowBytes=%u pixelType=%u "
		         "size=%ux%u baseAddr=0x%08x",
		         label, device, RaveDeviceTypeName(deviceType),
		         rowBytes, pixelType, width, height, baseAddr);
	} else if (deviceType == kRaveDeviceTypeGDevice) {
		uint32 gdevice = ReadMacInt32(device + kRaveDeviceOff_GDeviceHandle);
		RAVE_LOG("%s: device=0x%08x type=%s gDevice=0x%08x",
		         label, device, RaveDeviceTypeName(deviceType), gdevice);
		if (gdevice != 0) {
			uint32 gdevicePtr = ReadMacInt32(gdevice);
			uint32 pixMapH = gdevicePtr != 0
				? ReadMacInt32(gdevicePtr + GDEVICE_OFF_PMAP)
				: 0;
			uint32 pixMapPtr = pixMapH != 0 ? ReadMacInt32(pixMapH) : 0;
			if (pixMapPtr != 0) {
				uint32 baseAddr = ReadMacInt32(
					pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BASEADDR);
				uint16 rbRaw = (uint16)ReadMacInt16(
					pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_ROWBYTES);
				int16 top = (int16)ReadMacInt16(
					pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_TOP);
				int16 left = (int16)ReadMacInt16(
					pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_LEFT);
				int16 bottom = (int16)ReadMacInt16(
					pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_BOT);
				int16 right = (int16)ReadMacInt16(
					pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_RIGHT);
				uint16 pixelType = (uint16)ReadMacInt16(
					pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELTYPE);
				uint16 pixelSize = (uint16)ReadMacInt16(
					pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELSIZE);
				uint16 cmpCount = (uint16)ReadMacInt16(
					pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_CMPCOUNT);
				uint16 cmpSize = (uint16)ReadMacInt16(
					pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_CMPSIZE);
				RAVE_LOG("%s: GDevice ptr=0x%08x pixMapH=0x%08x "
				         "pixMap=0x%08x baseAddr=0x%08x rbRaw=0x%04x "
				         "rb=%u bounds=%d,%d,%d,%d pixelType=0x%04x "
				         "pixelSize=%u cmpCount=%u cmpSize=%u",
				         label, gdevicePtr, pixMapH, pixMapPtr, baseAddr,
				         rbRaw, (unsigned)(rbRaw & 0x3FFFu),
				         (int)top, (int)left, (int)bottom, (int)right,
				         pixelType, pixelSize, cmpCount, cmpSize);
			} else {
				RAVE_LOG("%s: GDevice ptr=0x%08x pixMapH=0x%08x "
				         "pixMap=NULL", label, gdevicePtr, pixMapH);
			}
		}
	} else {
		RAVE_LOG("%s: device=0x%08x type=%s(%u)",
		         label, device, RaveDeviceTypeName(deviceType), deviceType);
	}
}

// kQAOptional_* feature flags (RAVE 1.6)
// Bit assignments MUST match DDK RAVE.h exactly.
enum {
	kQAOptional_DeepZ               = (1 << 0),
	kQAOptional_Texture             = (1 << 1),
	kQAOptional_TextureHQ           = (1 << 2),
	kQAOptional_TextureColor        = (1 << 3),
	kQAOptional_Blend               = (1 << 4),
	kQAOptional_BlendAlpha          = (1 << 5),
	kQAOptional_Antialias           = (1 << 6),
	kQAOptional_ZSorted             = (1 << 7),
	kQAOptional_PerspectiveZ        = (1 << 8),
	kQAOptional_OpenGL              = (1 << 9),   // Extended GL features
	kQAOptional_NoClear             = (1 << 10),  // Engine doesn't clear before draw
	kQAOptional_CSG                 = (1 << 11),  // Constructive solid geometry
	kQAOptional_BoundToDevice       = (1 << 12),  // Tightly bound to GDevice
	kQAOptional_CL4                 = (1 << 13),
	kQAOptional_CL8                 = (1 << 14),
	kQAOptional_BufferComposite     = (1 << 15),  // Composite with initial buffer
	kQAOptional_NoDither            = (1 << 16),  // Can draw with no dithering
	kQAOptional_FogAlpha            = (1 << 17),
	kQAOptional_FogDepth            = (1 << 18),
	kQAOptional_MultiTextures       = (1 << 19),
	kQAOptional_MipmapBias          = (1 << 20),
	kQAOptional_ChannelMask         = (1 << 21),
	kQAOptional_ZBufferMask         = (1 << 22),
	kQAOptional_AlphaTest           = (1 << 23),
	kQAOptional_AccessTexture       = (1 << 24),
	kQAOptional_AccessBitmap        = (1 << 25),
	kQAOptional_AccessDrawBuffer    = (1 << 26),
	kQAOptional_AccessZBuffer       = (1 << 27),
	kQAOptional_ClearDrawBuffer     = (1 << 28),
	kQAOptional_ClearZBuffer        = (1 << 29),
	kQAOptional_OffscreenDrawContexts = (1 << 30)
};

// kQAOptional2_* feature flags (RAVE 1.6)
// Bit assignments MUST match DDK RAVE 1.6 Specification exactly.
enum {
	kQAOptional2_TextureDrawContexts = (1 << 1),
	kQAOptional2_BitmapDrawContexts  = (1 << 2),
	kQAOptional2_Busy                = (1 << 3),
	kQAOptional2_SwapBuffers         = (1 << 4),
	kQAOptional2_Chromakey           = (1 << 5),
	kQAOptional2_NonRelocatable      = (1 << 6),
	kQAOptional2_NoCopy              = (1 << 7),
	kQAOptional2_PriorityBits        = (1 << 8),
	kQAOptional2_FlipOrigin          = (1 << 9),
	kQAOptional2_BitmapScale         = (1 << 10),
	kQAOptional2_DrawContextScale    = (1 << 11),
	kQAOptional2_DrawContextNonRelocatable = (1 << 12)
};

// kQAPixel_* pixel type values (for bit shifting into pixel type masks)
enum {
	kQAPixel_Alpha1 = 0,
	kQAPixel_RGB16 = 1,
	kQAPixel_ARGB16 = 2,
	kQAPixel_RGB32 = 3,
	kQAPixel_ARGB32 = 4,
	kQAPixel_CL4 = 5,
	kQAPixel_CL8 = 6,
	kQAPixel_RGB16_565 = 7,
	kQAPixel_RGB24 = 8,
	kQAPixel_RGB8_332 = 9,
	kQAPixel_ARGB16_4444 = 10,
	kQAPixel_ACL16_88 = 11,
	kQAPixel_I8 = 12,
	kQAPixel_AI16_88 = 13,
	// YUV YUVS/YUVU conversion implemented. Test: RAVEPixelTypeTests.testYUVS_convertPixels
	// Renamed per QD3D 1.6 RAVE.h -- both are 4:2:2 variants (unsigned vs signed UV), not 4:2:2 vs 4:1:1
	kQAPixel_YUVS = 14,     // YUYV unsigned UV (4:2:2)
	kQAPixel_YUVU = 15,     // YUYV signed UV (4:2:2)
	kQAPixel_YUV422 = 14,   // backward-compat alias for kQAPixel_YUVS
	kQAPixel_YUV411 = 15    // backward-compat alias for kQAPixel_YUVU (misnomer)
};

// OptionalFeatures bitmask -- advertise features we actually support.
//
// CRITICAL: Bit assignments must match DDK RAVE.h exactly.
// Previous versions had wrong bit numbering (skipped OpenGL, NoClear, CSG,
// BufferComposite, NoDither), causing every bit from 9 onwards to be wrong.
// This meant our "FogAlpha" at bit 12 was read by RAVE as BoundToDevice,
// which caused QADeviceGetFirstEngine to filter us out.
//
// Excluded flags:
//   - OpenGL (bit 9): Extended GL features we don't have
//   - NoClear (bit 10): Changes clear-before-draw expectation
//   - CSG (bit 11): Constructive solid geometry we don't implement
//   - BoundToDevice (bit 12): Would require device association we don't have
//   - BufferComposite (bit 15): Buffer compositing we don't implement
//   - NoDither (bit 16): We support dithering, don't claim otherwise
static const uint32 kAllOptionalFeatures =
	kQAOptional_DeepZ | kQAOptional_Texture | kQAOptional_TextureHQ |
	kQAOptional_TextureColor | kQAOptional_Blend | kQAOptional_BlendAlpha |
	// Deliberate: kQAOptional_PerspectiveZ (bit8) de-advertised — depth is always
	// submitted as raw z (rave_metal_renderer.mm:1234); 1/invW HSR deferred (Tomb Raider 95.0
	// out-of-[0,1] Metal clipping, :1229-1233). Capability honesty: no advertised-but-inert bit.
	kQAOptional_Antialias | kQAOptional_ZSorted |
	kQAOptional_CL4 | kQAOptional_CL8 |
	kQAOptional_FogAlpha | kQAOptional_FogDepth | kQAOptional_MultiTextures |
	kQAOptional_MipmapBias | kQAOptional_ChannelMask | kQAOptional_ZBufferMask |
	kQAOptional_AlphaTest | kQAOptional_AccessTexture | kQAOptional_AccessBitmap |
	kQAOptional_AccessDrawBuffer | kQAOptional_AccessZBuffer |
	kQAOptional_ClearDrawBuffer | kQAOptional_ClearZBuffer | // OffscreenDrawContexts removed (not implemented)
	kQAOptional_OpenGL;  // GL tags 100-153 are stored/retrieved via Set/GetInt — games rely on this for scissor, blend, wrap

// OptionalFeatures2 bitmask -- only advertise what we support.
// Bit assignments now match DDK RAVE 1.6 Specification exactly.
static const uint32 kAllOptionalFeatures2 =
	kQAOptional2_TextureDrawContexts | kQAOptional2_BitmapDrawContexts |
	kQAOptional2_Busy | kQAOptional2_SwapBuffers |
	kQAOptional2_PriorityBits |  // kQAOptional2_PriorityBits advertised correctly; priority extraction fixed. Test: RAVEABITests.testKQAFast_bitPositions_matchSpec
	kQAOptional2_BitmapScale;

// kQAFast_* bit positions verified against RAVE.h. Test: RAVEABITests.testKQAFast_bitPositions_matchSpec
// These are NOT aliases. Per RAVE.h, kQAFast_xxx has its own bit namespace.
enum {
	kQAFast_None            = 0,
	kQAFast_Line            = (1 << 0),
	kQAFast_Gouraud         = (1 << 1),
	kQAFast_Texture         = (1 << 2),
	kQAFast_TextureHQ       = (1 << 3),
	kQAFast_Blend           = (1 << 4),
	kQAFast_Antialiasing    = (1 << 5),
	kQAFast_ZSorted         = (1 << 6),
	kQAFast_CL4             = (1 << 7),
	kQAFast_CL8             = (1 << 8),
	kQAFast_FogAlpha        = (1 << 9),
	kQAFast_FogDepth        = (1 << 10),
	kQAFast_MultiTextures   = (1 << 11),
	kQAFast_BitmapScale     = (1 << 12),
	kQAFast_DrawContextScale = (1 << 13)
};

// Metal accelerates everything, so all kQAFast bits are set.
static const uint32 kAllFastFeatures =
	kQAFast_Line | kQAFast_Gouraud | kQAFast_Texture | kQAFast_TextureHQ |
	kQAFast_Blend | kQAFast_Antialiasing | kQAFast_ZSorted |
	kQAFast_CL4 | kQAFast_CL8 |
	kQAFast_FogAlpha | kQAFast_FogDepth | kQAFast_MultiTextures |
	kQAFast_BitmapScale;

// Resource handle table -- 64 slots, 1-based handles (same pattern as draw context table)
RaveResourceEntry rave_resource_table[RAVE_MAX_RESOURCES] = {};

// Highest-ever-allocated slot+1. RaveResourceFindByAddr is on the per-draw texture-bind
// hot path; bounding its scan to the high-water mark keeps lookups ~O(working set)
// instead of O(RAVE_MAX_RESOURCES) now that the table is 4096. Monotonic (never lowered
// on free) so it can never under-scan a live entry.
static uint32_t g_rave_resource_high_water = 0;
static uint32_t g_rave_resource_next_generation = 1;

// Per-hook patch info for unpatch-call-repatch chaining
RaveHookPatchInfo rave_hook_patches[RAVE_NUM_HOOKED_APIS] = {};

/*
 *  RaveChainToOriginal - safely chain to original function via unpatch-call-repatch
 *
 *  The previous trampoline approach relocated the first 4 instructions of the
 *  original function into a trampoline and appended a register-indirect branch
 *  (lis r11/ori r11/mtctr r11/bctr) to jump back. This clobbered r11, which
 *  could have been set by one of the saved instructions and needed by instruction 5+.
 *
 *  Instead, we temporarily restore the original instructions at the function's
 *  entry point, call the original function via its TVECT (which now points to the
 *  unpatched code), then re-apply the hook patch. This is safe because SheepShaver's
 *  PPC emulation is single-threaded.
 */
uint32_t RaveChainToOriginal(int hook_index, int nargs, uint32_t const *args)
{
	RaveHookPatchInfo &info = rave_hook_patches[hook_index];
	if (!info.active) {
		RAVE_LOG("CHAIN: hook %d not active, returning kQANoErr", hook_index);
		return kQANoErr;
	}

	// Step 1: Restore original instructions at the entry point
	for (int j = 0; j < 4; j++) {
		WriteMacInt32(info.orig_code + j * 4, info.saved_instr[j]);
	}

	// Step 2: Flush instruction cache for the restored range
#if EMULATED_PPC
	FlushCodeCache(info.orig_code, info.orig_code + 16);
#endif

	// Step 3: Call original via its TVECT (now points to unpatched code)
	uint32_t result;
	switch (nargs) {
	case 1:
		result = call_macos1(info.orig_tvect, args[0]);
		break;
	case 2:
		result = call_macos2(info.orig_tvect, args[0], args[1]);
		break;
	case 3:
		result = call_macos3(info.orig_tvect, args[0], args[1], args[2]);
		break;
	case 4:
		result = call_macos4(info.orig_tvect, args[0], args[1], args[2], args[3]);
		break;
	case 5:
		result = call_macos5(info.orig_tvect, args[0], args[1], args[2], args[3], args[4]);
		break;
	case 6:
		result = call_macos6(info.orig_tvect, args[0], args[1], args[2], args[3], args[4], args[5]);
		break;
	default:
		RAVE_LOG("CHAIN: unsupported nargs=%d", nargs);
		result = kQANoErr;
		break;
	}

	// Step 4: Re-apply hook patch
	for (int j = 0; j < 4; j++) {
		WriteMacInt32(info.orig_code + j * 4, info.hook_instr[j]);
	}

	// Step 5: Flush instruction cache for the re-patched range
#if EMULATED_PPC
	FlushCodeCache(info.orig_code, info.orig_code + 16);
#endif

	return result;
}

uint32_t RaveResourceAlloc(RaveResourceType type) {
	for (int i = 0; i < RAVE_MAX_RESOURCES; i++) {
		if (rave_resource_table[i].type == kRaveResourceFree) {
			// Zero entire entry before populating
			memset(&rave_resource_table[i], 0, sizeof(RaveResourceEntry));
			rave_resource_table[i].type = type;
			rave_resource_table[i].transparent_index = -1;
			rave_resource_table[i].generation = g_rave_resource_next_generation++;
			if (g_rave_resource_next_generation == 0)
				g_rave_resource_next_generation = 1;
			// Allocate a 4-byte Mac-visible address for the PPC side
			uint32_t mac_addr = SheepMem::Reserve(4);
			rave_resource_table[i].mac_addr = mac_addr;
			// Write a magic value so delete hooks can identify our resources
			uint32_t magic = 0;
			switch (type) {
				case kRaveResourceTexture:    magic = 0x54455854; break; // 'TEXT'
				case kRaveResourceBitmap:     magic = 0x424D5050; break; // 'BMPP'
				case kRaveResourceColorTable: magic = 0x434F4C52; break; // 'COLR'
				default: break;
			}
			WriteMacInt32(mac_addr, magic);
			if ((uint32_t)(i + 1) > g_rave_resource_high_water)
				g_rave_resource_high_water = (uint32_t)(i + 1);
			return (uint32_t)(i + 1);  // 1-based handle
		}
	}
	RAVE_LOG("resource table full (%d slots)", RAVE_MAX_RESOURCES);
	return 0;
}

// RaveResourceFree releases:
// (1) Metal texture via RaveReleaseTexture (CFRelease of id<MTLTexture>)
// (2) CPU pixel buffer via Mac_sysfree
// (3) Original indexed pixel data via delete[]
// (4) CLUT data via delete[]
// (5) memset zeroes entry to kRaveResourceFree
bool RaveResourceFree(uint32_t handle) {
	if (handle == 0 || handle > RAVE_MAX_RESOURCES) return false;
	RaveResourceEntry *entry = &rave_resource_table[handle - 1];
	if (entry->type == kRaveResourceFree) return false;

	// Release Metal texture
	if (entry->metal_texture != nullptr) {
		RaveReleaseTexture(entry->metal_texture);
		entry->metal_texture = nullptr;
	}
	// Free CPU pixel buffer (Mac address space)
	if (entry->cpu_pixel_mac_addr != 0) {
		Mac_sysfree(entry->cpu_pixel_mac_addr);
		entry->cpu_pixel_data = nullptr;
		entry->cpu_pixel_mac_addr = 0;
		entry->cpu_pixel_data_size = 0;
	}
	// Free retained indexed pixel data
	if (entry->original_pixels != nullptr) {
		delete[] entry->original_pixels;
		entry->original_pixels = nullptr;
	}
	// Free CLUT data
	if (entry->clut_data != nullptr) {
		delete[] entry->clut_data;
		entry->clut_data = nullptr;
	}

	// Zero entire entry
	memset(entry, 0, sizeof(RaveResourceEntry));
	return true;
}

RaveResourceEntry *RaveResourceGet(uint32_t handle) {
	if (handle == 0 || handle > RAVE_MAX_RESOURCES) return nullptr;
	RaveResourceEntry *entry = &rave_resource_table[handle - 1];
	if (entry->type == kRaveResourceFree) return nullptr;
	return entry;
}

static bool RaveForgetRTTAndFreeResource(uint32_t handle)
{
	RaveResourceEntry *entry = RaveResourceGet(handle);
	if (!entry) return false;
	if (entry->type == kRaveResourceTexture || entry->type == kRaveResourceBitmap)
		RaveForgetRTTResourceHandle(handle, entry->generation);
	return RaveResourceFree(handle);
}

uint32_t RaveResourceFindByAddr(uint32_t mac_addr) {
	if (mac_addr == 0) return 0;
	// Only scan up to the high-water mark — all live entries are below it.
	for (uint32_t i = 0; i < g_rave_resource_high_water; i++) {
		if (rave_resource_table[i].type != kRaveResourceFree &&
			rave_resource_table[i].mac_addr == mac_addr) {
			return i + 1;
		}
	}
	return 0;
}

/*
 *  Lookup a RAVE texture entry whose
 *  pixmap_mac_addr matches `pixmapAddr`.
 *
 *  Returns the first matching texture entry, or nullptr if no match.
 *  Bitmap and color-table entries are excluded (they don't use
 *  pixmap_mac_addr in the same sense — bitmap data is eager-copied, so
 *  we only care about the deferred-direct-format texture path that
 *  Bugdom's ARGB16 sprites take).
 */
RaveResourceEntry *RaveFindTextureByPixmapAddr(uint32_t pixmapAddr)
{
	if (pixmapAddr == 0) return nullptr;
	for (int i = 0; i < RAVE_MAX_RESOURCES; i++) {
		RaveResourceEntry *e = &rave_resource_table[i];
		if (e->type == kRaveResourceTexture &&
		    e->pixmap_mac_addr == pixmapAddr) {
			return e;
		}
	}
	return nullptr;
}

/*
 *  Q3Pixmap_Set_Image intercept callback.
 *
 *  Synchronously copies `byteCount` bytes from `srcHostAddr` into the
 *  cpu_pixel_data buffer of the RAVE texture tracking `pixmapAddr`,
 *  bounded by the entry's cpu_pixel_data_size. Marks the entry as
 *  `cpu_pixel_data_is_authoritative` so the subsequent
 *  RaveRealizeDeferredTexture / RaveRefreshTextureFromPixmap calls
 *  read from cpu_pixel_data instead of pixmap_mac_addr.
 *
 *  If no RAVE texture tracks this pixmap address, returns silently —
 *  most Q3Pixmap_Set_Image calls are unrelated to any RAVE texture
 *  (e.g. Q3Pixmaps backing 2D PICT draws, not 3D sprite textures),
 *  and we must not log-spam or stall in those cases.
 *
 *  Thread-safety: inherits the single-threaded RAVE dispatch invariant.
 *  The FindLibSymbol-based activation hook (deferred to a follow-up)
 *  runs on the emulated PPC
 *  thread, same as RAVE dispatch, so no additional locking is needed.
 */
void NativeHookQ3PixmapSetImage(uint32_t pixmapAddr,
                                 uint32_t srcHostAddr,
                                 uint32_t byteCount)
{
	RaveResourceEntry *entry = RaveFindTextureByPixmapAddr(pixmapAddr);
	if (!entry) {
		// Not a RAVE-tracked pixmap — unrelated Q3Pixmap_Set_Image call.
		// Silent return; must not log-spam.
		return;
	}
	if (!entry->cpu_pixel_data || entry->cpu_pixel_data_size == 0) {
		// Entry exists but has no cpu_pixel_data buffer (Mac_sysalloc
		// failed at TextureNew time). Defensive: fall back to the
		// pixmap_mac_addr path by leaving the flag unset.
		RAVE_LOG("Q3Pixmap_Set_Image: entry has no cpu_pixel_data buffer; "
		         "pixmap=0x%08x", pixmapAddr);
		return;
	}

	// Bound the copy by our cpu_pixel_data buffer size.
	uint32_t copyBytes = byteCount;
	if (copyBytes > entry->cpu_pixel_data_size) {
		copyBytes = entry->cpu_pixel_data_size;
	}

	// Copy src -> cpu_pixel_data. srcHostAddr is a Mac address;
	// Mac2HostAddr resolves it to a host pointer.
	memcpy(entry->cpu_pixel_data, Mac2HostAddr(srcHostAddr), copyBytes);

	entry->cpu_pixel_data_is_authoritative = true;

	RAVE_LOG("Q3Pixmap_Set_Image intercept: pixmap=0x%08x src=0x%08x "
	         "bytes=%u (bounded=%u) -> cpu_pixel_data_is_authoritative=true",
	         pixmapAddr, srcHostAddr, byteCount, copyBytes);
}

/*
 *  Pixel format conversion functions
 *
 *  Convert from Mac big-endian pixel data to BGRA8 (Metal native format).
 *  All functions read from Mac address space via ReadMacInt* and write
 *  to a native heap buffer.
 */

// ReadMacInt32 returns host-endian from big-endian PPC memory.
// Extracts A(31:24) R(23:16) G(15:8) B(7:0) → writes BGRA8 (B=byte0, G=byte1, R=byte2, A=byte3).
// Row stride: rowBytes from Mac source, dst stride = width*4. Correct.
static void ConvertARGB32(uint32 srcAddr, uint8_t *dst, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32 rowAddr = srcAddr + y * rowBytes;
		for (uint32_t x = 0; x < width; x++) {
			uint32_t argb = ReadMacInt32(rowAddr + x * 4);
			uint8_t a = (argb >> 24) & 0xFF;
			uint8_t r = (argb >> 16) & 0xFF;
			uint8_t g = (argb >> 8) & 0xFF;
			uint8_t b = argb & 0xFF;
			uint32_t idx = (y * width + x) * 4;
			dst[idx + 0] = b;
			dst[idx + 1] = g;
			dst[idx + 2] = r;
			dst[idx + 3] = a;
		}
	}
}

// Same as ARGB32 but top byte (X) ignored, alpha forced to 0xFF. Correct.
static void ConvertRGB32(uint32 srcAddr, uint8_t *dst, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32 rowAddr = srcAddr + y * rowBytes;
		for (uint32_t x = 0; x < width; x++) {
			uint32_t xrgb = ReadMacInt32(rowAddr + x * 4);
			uint8_t r = (xrgb >> 16) & 0xFF;
			uint8_t g = (xrgb >> 8) & 0xFF;
			uint8_t b = xrgb & 0xFF;
			uint32_t idx = (y * width + x) * 4;
			dst[idx + 0] = b;
			dst[idx + 1] = g;
			dst[idx + 2] = r;
			dst[idx + 3] = 0xFF;
		}
	}
}

// ReadMacInt16 returns host-endian. 1-bit A(15), 5-bit R(14:10) G(9:5) B(4:0).
// 5→8 expansion via (x<<3)|(x>>2) correct. 1-bit alpha: 0→0x00, 1→0xFF. BGRA8 output correct.
static void ConvertARGB16(uint32 srcAddr, uint8_t *dst, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32 rowAddr = srcAddr + y * rowBytes;
		for (uint32_t x = 0; x < width; x++) {
			uint16_t val = (uint16_t)ReadMacInt16(rowAddr + x * 2);
			uint8_t a = (val >> 15) & 1;
			uint8_t r = (val >> 10) & 0x1F;
			uint8_t g = (val >> 5) & 0x1F;
			uint8_t b = val & 0x1F;
			uint8_t r8 = (r << 3) | (r >> 2);
			uint8_t g8 = (g << 3) | (g >> 2);
			uint8_t b8 = (b << 3) | (b >> 2);
			uint8_t a8 = a ? 0xFF : 0x00;
			uint32_t idx = (y * width + x) * 4;
			dst[idx + 0] = b8;
			dst[idx + 1] = g8;
			dst[idx + 2] = r8;
			dst[idx + 3] = a8;
		}
	}
}

// Same 5-5-5 layout as ARGB16 but top bit ignored, alpha forced to 0xFF. Correct.
static void ConvertRGB16(uint32 srcAddr, uint8_t *dst, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32 rowAddr = srcAddr + y * rowBytes;
		for (uint32_t x = 0; x < width; x++) {
			uint16_t val = (uint16_t)ReadMacInt16(rowAddr + x * 2);
			uint8_t r = (val >> 10) & 0x1F;
			uint8_t g = (val >> 5) & 0x1F;
			uint8_t b = val & 0x1F;
			uint8_t r8 = (r << 3) | (r >> 2);
			uint8_t g8 = (g << 3) | (g >> 2);
			uint8_t b8 = (b << 3) | (b >> 2);
			uint32_t idx = (y * width + x) * 4;
			dst[idx + 0] = b8;
			dst[idx + 1] = g8;
			dst[idx + 2] = r8;
			dst[idx + 3] = 0xFF;
		}
	}
}

// R(7:5)=3 bits, G(4:2)=3 bits, B(1:0)=2 bits.
// 3→8 expansion: (r3<<5)|(r3<<2)|(r3>>1). 2→8 expansion: (b2<<6)|(b2<<4)|(b2<<2)|b2. Alpha 0xFF. Correct.
// ConvertRGB8_332: 8bpp, R=7:5, G=4:2, B=1:0
static void ConvertRGB8_332(uint32 srcAddr, uint8_t *dst, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32 rowAddr = srcAddr + y * rowBytes;
		for (uint32_t x = 0; x < width; x++) {
			uint8_t val = ReadMacInt8(rowAddr + x);
			uint8_t r3 = (val >> 5) & 0x07;
			uint8_t g3 = (val >> 2) & 0x07;
			uint8_t b2 = val & 0x03;
			// Expand to 8-bit: replicate high bits into low bits
			uint8_t r8 = (r3 << 5) | (r3 << 2) | (r3 >> 1);
			uint8_t g8 = (g3 << 5) | (g3 << 2) | (g3 >> 1);
			uint8_t b8 = (b2 << 6) | (b2 << 4) | (b2 << 2) | b2;
			uint32_t idx = (y * width + x) * 4;
			dst[idx + 0] = b8;
			dst[idx + 1] = g8;
			dst[idx + 2] = r8;
			dst[idx + 3] = 0xFF;  // No alpha channel
		}
	}
}

// ReadMacInt16 → A(15:12) R(11:8) G(7:4) B(3:0), 4→8 expansion via (n<<4)|n. Correct.
// ConvertARGB16_4444: 16bpp, A=15:12, R=11:8, G=7:4, B=3:0
static void ConvertARGB16_4444(uint32 srcAddr, uint8_t *dst, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32 rowAddr = srcAddr + y * rowBytes;
		for (uint32_t x = 0; x < width; x++) {
			uint16_t val = (uint16_t)ReadMacInt16(rowAddr + x * 2);
			uint8_t a4 = (val >> 12) & 0x0F;
			uint8_t r4 = (val >> 8) & 0x0F;
			uint8_t g4 = (val >> 4) & 0x0F;
			uint8_t b4 = val & 0x0F;
			// Expand 4-bit to 8-bit: replicate nibble
			uint32_t idx = (y * width + x) * 4;
			dst[idx + 0] = (b4 << 4) | b4;
			dst[idx + 1] = (g4 << 4) | g4;
			dst[idx + 2] = (r4 << 4) | r4;
			dst[idx + 3] = (a4 << 4) | a4;
		}
	}
}

// 8-bit intensity → B=G=R=i, A=0xFF. Correct.
// ConvertI8: 8bpp grayscale, I=7:0
static void ConvertI8(uint32 srcAddr, uint8_t *dst, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32 rowAddr = srcAddr + y * rowBytes;
		for (uint32_t x = 0; x < width; x++) {
			uint8_t i = ReadMacInt8(rowAddr + x);
			uint32_t idx = (y * width + x) * 4;
			dst[idx + 0] = i;     // B = intensity
			dst[idx + 1] = i;     // G = intensity
			dst[idx + 2] = i;     // R = intensity
			dst[idx + 3] = 0xFF;  // Fully opaque
		}
	}
}

// ReadMacInt16 → A(15:8) I(7:0). B=G=R=intensity, A from high byte. Correct.
// ConvertAI16_88: 16bpp, A=15:8, I=7:0
static void ConvertAI16_88(uint32 srcAddr, uint8_t *dst, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32 rowAddr = srcAddr + y * rowBytes;
		for (uint32_t x = 0; x < width; x++) {
			uint16_t val = (uint16_t)ReadMacInt16(rowAddr + x * 2);
			uint8_t a = (val >> 8) & 0xFF;
			uint8_t i = val & 0xFF;
			uint32_t idx = (y * width + x) * 4;
			dst[idx + 0] = i;  // B = intensity
			dst[idx + 1] = i;  // G = intensity
			dst[idx + 2] = i;  // R = intensity
			dst[idx + 3] = a;  // Alpha from high byte
		}
	}
}

// Generate mip levels by box-downsampling the level-0 BGRA8 image and upload each.
// Replaces reading the app's per-mip TQAImages: UT (and likely others) don't supply a
// full, valid mip chain at the level count we compute, so those reads returned
// garbage/black -> black high-mips -> distant/minified surfaces sample black -> the
// "black world geometry" bug (RAVE diag green-probe confirmed far fragments rasterize
// but output black). A 2x2 average from the known-good level 0 yields correct RGB *and*
// alpha at every level (also fixes UT's 1-bit ARGB16 mips dropping the alpha bit, which
// was the see-through artifact). Deterministic, no dependency on the Metal blit mip path.
static void RaveUploadGeneratedMips(void *metalTexture, const uint8_t *level0,
                                    uint32_t w, uint32_t h, uint32_t mipLevels)
{
	if (!metalTexture || mipLevels <= 1 || !level0)
		return;

	uint32_t sw = w, sh = h;
	const uint8_t *src = level0;       // level 0 is caller-owned; never freed here
	uint8_t *prevOwned = nullptr;      // owned downsampled buffers for levels >= 1

	// One pass break for the whole chain, not one per level (D-R-4).
	RaveTextureUploadBatchBegin();

	for (uint32_t level = 1; level < mipLevels; level++) {
		uint32_t dw = (sw > 1) ? sw / 2 : 1;
		uint32_t dh = (sh > 1) ? sh / 2 : 1;
		uint8_t *dst = new uint8_t[(size_t)dw * dh * 4];

		for (uint32_t y = 0; y < dh; y++) {
			uint32_t y0 = y * 2;
			uint32_t y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
			for (uint32_t x = 0; x < dw; x++) {
				uint32_t x0 = x * 2;
				uint32_t x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
				const uint8_t *p00 = &src[(y0 * sw + x0) * 4];
				const uint8_t *p01 = &src[(y0 * sw + x1) * 4];
				const uint8_t *p10 = &src[(y1 * sw + x0) * 4];
				const uint8_t *p11 = &src[(y1 * sw + x1) * 4];
				uint8_t *d = &dst[(y * dw + x) * 4];
				for (int c = 0; c < 4; c++)
					d[c] = (uint8_t)(((uint32_t)p00[c] + p01[c] + p10[c] + p11[c]) >> 2);
			}
		}

		RaveUploadMipLevel(metalTexture, level, dw, dh, dst, dw * 4);

		if (prevOwned) delete[] prevOwned;
		prevOwned = dst;
		src = dst;
		sw = dw;
		sh = dh;
	}

	RaveTextureUploadBatchEnd();

	if (prevOwned) delete[] prevOwned;
}


// srcBuf is native heap copy of big-endian Mac data. High byte (srcBuf[off])
// = alpha, low byte (srcBuf[off+1]) = CLUT index. CLUT color extracted as B/G/R from native uint32,
// alpha overridden from per-pixel value. Bounds-checked. Correct.
// ExpandACL16_88: 16bpp, A=15:8, CL=7:0 -- alpha + color lookup
// CLUT color lookup with per-pixel alpha from the high byte.
// Source is a native heap buffer (original_pixels), not Mac memory.
// Data was copied byte-by-byte from Mac memory (big-endian), so reconstruct
// 16-bit values as (buf[off] << 8) | buf[off+1].
static void ExpandACL16_88(const uint8_t *srcBuf, uint8_t *dst, uint32_t width, uint32_t height,
                           uint32_t rowBytes, const uint32_t *clut, uint32_t clutCount)
{
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			uint32_t off = y * rowBytes + x * 2;
			uint8_t a = srcBuf[off];          // big-endian high byte = alpha
			uint8_t idx = srcBuf[off + 1];    // low byte = CLUT index
			if (idx >= clutCount) idx = 0;
			uint32_t bgra = clut[idx];
			uint32_t dstIdx = (y * width + x) * 4;
			// Use CLUT color but override alpha with per-pixel alpha
			dst[dstIdx + 0] = bgra & 0xFF;         // B from CLUT
			dst[dstIdx + 1] = (bgra >> 8) & 0xFF;  // G from CLUT
			dst[dstIdx + 2] = (bgra >> 16) & 0xFF; // R from CLUT
			dst[dstIdx + 3] = a;                     // A from pixel, not CLUT
		}
	}
}


// BT.601 YUYV-to-BGRA converter for kQAPixel_YUVS (unsigned UV).
// YUYV format: 2 pixels per 4 bytes. Big-endian Mac byte order: Y0 U Y1 V.
// BT.601: R = Y + 1.402*(V-128), G = Y - 0.344*(U-128) - 0.714*(V-128), B = Y + 1.772*(U-128)
// Output: BGRA8 (B=byte0, G=byte1, R=byte2, A=0xFF).
// T-08-03 mitigation: validate width*2 <= rowBytes before pixel loop.
static inline uint8_t clamp_u8(int v)
{
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void ConvertYUVS(uint32 srcAddr, uint8_t *dst,
                        uint32_t width, uint32_t height, uint32_t rowBytes)
{
	// T-08-03: rowBytes must be at least width*2 (2 bytes per pixel in YUYV)
	if (width > 0 && rowBytes < width * 2) {
		memset(dst, 0, (size_t)width * height * 4);
		return;
	}
	for (uint32_t y = 0; y < height; y++) {
		uint32 rowAddr = srcAddr + y * rowBytes;
		for (uint32_t x = 0; x < width; x += 2) {
			uint8_t Y0 = ReadMacInt8(rowAddr + x * 2 + 0);
			uint8_t U  = ReadMacInt8(rowAddr + x * 2 + 1);
			uint8_t Y1 = ReadMacInt8(rowAddr + x * 2 + 2);
			uint8_t V  = ReadMacInt8(rowAddr + x * 2 + 3);

			// BT.601 unsigned UV: offset by -128 to center around zero
			int cr = V - 128;
			int cb = U - 128;
			for (int p = 0; p < 2; p++) {
				if (x + p >= width) break;  // handle odd widths
				int Yp = (p == 0) ? Y0 : Y1;
				int r = Yp + (int)(1.402f * cr);
				int g = Yp - (int)(0.344f * cb) - (int)(0.714f * cr);
				int b = Yp + (int)(1.772f * cb);
				uint32_t idx = (y * width + x + p) * 4;
				dst[idx + 0] = clamp_u8(b);   // B
				dst[idx + 1] = clamp_u8(g);   // G
				dst[idx + 2] = clamp_u8(r);   // R
				dst[idx + 3] = 0xFF;           // A (opaque)
			}
		}
	}
}

// BT.601 YUYV-to-BGRA converter for kQAPixel_YUVU (signed UV).
// Same as YUVS but UV bytes are interpreted as signed: int cr = (int8_t)V; int cb = (int8_t)U;
// No -128 offset since the values are already in signed range [-128..127].
static void ConvertYUVU(uint32 srcAddr, uint8_t *dst,
                        uint32_t width, uint32_t height, uint32_t rowBytes)
{
	// T-08-03: rowBytes must be at least width*2 (2 bytes per pixel in YUYV)
	if (width > 0 && rowBytes < width * 2) {
		memset(dst, 0, (size_t)width * height * 4);
		return;
	}
	for (uint32_t y = 0; y < height; y++) {
		uint32 rowAddr = srcAddr + y * rowBytes;
		for (uint32_t x = 0; x < width; x += 2) {
			uint8_t Y0 = ReadMacInt8(rowAddr + x * 2 + 0);
			uint8_t U  = ReadMacInt8(rowAddr + x * 2 + 1);
			uint8_t Y1 = ReadMacInt8(rowAddr + x * 2 + 2);
			uint8_t V  = ReadMacInt8(rowAddr + x * 2 + 3);

			// BT.601 signed UV: interpret U/V as signed bytes directly
			int cr = (int8_t)V;
			int cb = (int8_t)U;
			for (int p = 0; p < 2; p++) {
				if (x + p >= width) break;  // handle odd widths
				int Yp = (p == 0) ? Y0 : Y1;
				int r = Yp + (int)(1.402f * cr);
				int g = Yp - (int)(0.344f * cb) - (int)(0.714f * cr);
				int b = Yp + (int)(1.772f * cb);
				uint32_t idx = (y * width + x + p) * 4;
				dst[idx + 0] = clamp_u8(b);   // B
				dst[idx + 1] = clamp_u8(g);   // G
				dst[idx + 2] = clamp_u8(r);   // R
				dst[idx + 3] = 0xFF;           // A (opaque)
			}
		}
	}
}


/*
 *  ConvertPixels - dispatch to the appropriate pixel conversion function
 *  Returns true if conversion was performed (direct format), false if indexed (needs CLUT).
 */
bool ConvertPixels(uint32_t pixelType, uint32 srcAddr, uint8_t *dst,
                          uint32_t width, uint32_t height, uint32_t rowBytes)
{
	bool converted = true;
	switch (pixelType) {
		case kQAPixel_ARGB32:
			ConvertARGB32(srcAddr, dst, width, height, rowBytes);
			break;
		case kQAPixel_RGB32:
			// RGB32 validated. Test: RAVEPixelTypeTests.testRGB32_convertPixels
			ConvertRGB32(srcAddr, dst, width, height, rowBytes);
			break;
		case kQAPixel_ARGB16:
			ConvertARGB16(srcAddr, dst, width, height, rowBytes);
			break;
		case kQAPixel_RGB16:
			// RGB16 validated. Test: RAVEPixelTypeTests.testRGB16_convertPixels
			ConvertRGB16(srcAddr, dst, width, height, rowBytes);
			break;
		case kQAPixel_RGB8_332:
			ConvertRGB8_332(srcAddr, dst, width, height, rowBytes);
			break;
		case kQAPixel_ARGB16_4444:
			ConvertARGB16_4444(srcAddr, dst, width, height, rowBytes);
			break;
		case kQAPixel_I8:
			ConvertI8(srcAddr, dst, width, height, rowBytes);
			break;
		case kQAPixel_AI16_88:
			ConvertAI16_88(srcAddr, dst, width, height, rowBytes);
			break;
		case kQAPixel_YUVS:
			// YUVS BT.601 conversion. Test: RAVEPixelTypeTests.testYUVS_convertPixels
			ConvertYUVS(srcAddr, dst, width, height, rowBytes);
			break;
		case kQAPixel_YUVU:
			// YUVU BT.601 signed-UV conversion. Test: RAVEPixelTypeTests.testYUVU_convertPixels_signedUV
			ConvertYUVU(srcAddr, dst, width, height, rowBytes);
			break;
		// Exotic types are deliberately unconverted. Test: RAVEPixelTypeTests.testExoticPixelTypes_documentedErrors
		// These return false (unconverted) because they are not used by Mac OS 9 RAVE apps:
		case kQAPixel_Alpha1:
			// Deliberate: kQAPixel_Alpha1 not used by known Mac OS 9 RAVE apps.
		case kQAPixel_RGB16_565:
			// Deliberate: kQAPixel_RGB16_565 is "Win32 ONLY" per RAVE.h; no Mac OS 9 apps use this path.
		case kQAPixel_RGB24:
			// Deliberate: kQAPixel_RGB24 is "Win32 ONLY" per RAVE.h.
			converted = false;
			break;
		default:
			converted = false;  // indexed format -- needs CLUT
			break;
	}

	return converted;
}



/*
 *  Shared texture creation helper
 *
 *  Used by both the hook path (NativeHookTextureNew) and the engine dispatch
 *  path (NativeEngineTextureNew) to avoid code duplication.
 */
static void RaveCreateTextureFromImages(uint32_t flags, uint32_t pixelType,
                                         uint32 imagesAddr, RaveResourceEntry *entry)
{
	// Read level 0 TQAImage (16 bytes)
	uint32_t w        = ReadMacInt32(imagesAddr + 0);
	uint32_t h        = ReadMacInt32(imagesAddr + 4);
	uint32_t rowBytes = ReadMacInt32(imagesAddr + 8);
	uint32_t pixmap   = ReadMacInt32(imagesAddr + 12);

	// 4-bit priority at [31:28] per QACalculatePriorityBits. Test: RAVEABITests.testPriorityBits_extraction_matchesSpec
	entry->priority = (uint8_t)((flags >> 28) & 0xF);

	bool hasMipmaps = (flags & 2);  // kQATexture_Mipmap = (1 << 1) = bit 1
	uint32_t mipLevels = 1;
	if (hasMipmaps) {
		uint32_t maxDim = (w > h) ? w : h;
		mipLevels = (uint32_t)floor(log2((double)maxDim)) + 1;
	}

	entry->pixel_type = pixelType;
	entry->width      = w;
	entry->height     = h;
	entry->mip_levels = mipLevels;
	entry->row_bytes  = rowBytes;

	// New textures start with
	// cpu_pixel_data_is_authoritative=false; the flag is flipped by
	// NativeHookQ3PixmapSetImage if a Q3Pixmap_Set_Image intercept
	// fires for this texture's pixmap_mac_addr. Explicit init for
	// clarity even though RaveResourceAlloc memsets the entry to 0.
	entry->cpu_pixel_data_is_authoritative = false;

	bool isIndexed = (pixelType == kQAPixel_CL8 || pixelType == kQAPixel_CL4 || pixelType == kQAPixel_ACL16_88);

	if (isIndexed) {
		// Store original indexed pixels for later CLUT expansion.
		// Copy immediately -- RAVE spec says engine needn't copy until TextureDetach,
		// but many games (e.g. Tomb Raider) reuse the pixel buffer immediately after
		// TextureNew, so eager copy is safer and matches real hardware behavior.
		uint32_t totalSize = rowBytes * h;
		if (hasMipmaps) {
			totalSize = 0;
			uint32_t mw = w, mh = h;
			for (uint32_t level = 0; level < mipLevels; level++) {
				uint32_t mRowBytes = ReadMacInt32(imagesAddr + level * 16 + 8);
				totalSize += mRowBytes * mh;
				mw = (mw > 1) ? mw / 2 : 1;
				mh = (mh > 1) ? mh / 2 : 1;
			}
		}
		entry->original_pixels = new uint8_t[totalSize];
		entry->original_size = totalSize;
		entry->pixels_copied = true;

		// Copy all pixel data from Mac memory immediately
		uint32_t offset = 0;
		uint32_t mw = w, mh = h;
		for (uint32_t level = 0; level < mipLevels; level++) {
			uint32_t mRowBytes = ReadMacInt32(imagesAddr + level * 16 + 8);
			uint32_t mPixmap   = ReadMacInt32(imagesAddr + level * 16 + 12);
			uint32_t levelSize = mRowBytes * mh;
			for (uint32_t b = 0; b < levelSize; b++) {
				entry->original_pixels[offset + b] = ReadMacInt8(mPixmap + b);
			}
			offset += levelSize;
			if (!hasMipmaps) break;
			mw = (mw > 1) ? mw / 2 : 1;
			mh = (mh > 1) ? mh / 2 : 1;
		}

		// Metal texture deferred until BindColorTable
		entry->metal_texture = nullptr;
		RAVE_LOG("TextureNew indexed (pixelType=%d) %dx%d mips=%d rowBytes=%d pixmap=0x%08x",
		       pixelType, w, h, mipLevels, rowBytes, pixmap);
	} else {
		// Direct format. Snapshot immediately when level 0 already contains
		// converted data; otherwise keep the existing deferred path for clients
		// that hand RAVE an empty buffer and fill it shortly after TextureNew.
		entry->pixmap_mac_addr = pixmap;
		entry->metal_texture = nullptr;
		entry->pixels_copied = false;

		uint8_t *expanded = new uint8_t[w * h * 4];
		const bool converted = ConvertPixels(pixelType, pixmap, expanded, w, h, rowBytes);
		const bool whitenedAlphaMask =
			(pixelType == kQAPixel_ARGB32) && RaveBGRAWhitenAlphaOnlyMask(expanded, w * h);
		RaveBGRAImageStats sourceStats = converted
			? RaveBGRAImageAnalyze(expanded, w * h)
			: RaveBGRAImageStats{0, 0, 0, 0, w * h, w * h, w * h};
		entry->diag_alpha_zero = (w * h) - sourceStats.alpha;
		entry->diag_index_zero = 0;
		entry->diag_rgb_nonzero = sourceStats.rgb;
		// QATextureNew has copy semantics (RAVE spec): snapshot level 0 now
		// whenever conversion succeeded, so later reuse of the shared guest
		// staging buffer cannot corrupt this texture. Fill-after-create clients
		// use the texture-update path, which is handled separately. (Deferred-
		// when-empty heuristic removed.)
		const bool snapshotNow = converted;

		if (snapshotNow) {
			entry->metal_texture = RaveCreateMetalTexture(w, h, mipLevels, expanded, w * 4);
			if (mipLevels > 1 && entry->metal_texture) {
				// Generate mips from the known-good level 0 rather than reading the
				// app's per-mip images (which were garbage/black -> black world).
				RaveUploadGeneratedMips(entry->metal_texture, expanded, w, h, mipLevels);
			}
			entry->pixels_copied = (entry->metal_texture != nullptr);
			RAVE_LOG("TextureNew direct snapshot (pixelType=%d) %dx%d mips=%d pixmap=0x%08x -> metal=%p nz=%u a=%u rgb=%u white=%u first[nz/a/rgb]=%u/%u/%u alphaMaskWhite=%d",
			         pixelType, w, h, mipLevels, pixmap, entry->metal_texture,
			         sourceStats.nonzero, sourceStats.alpha, sourceStats.rgb, sourceStats.white,
			         sourceStats.first_nonzero, sourceStats.first_alpha, sourceStats.first_rgb,
			         whitenedAlphaMask);
		} else {
			// For deferred mipmapped textures, cache the TQAImage array (it
			// may be on the caller's stack and won't survive past this call).
			if (hasMipmaps && mipLevels > 1) {
				uint32_t imagesSize = mipLevels * 16;
				entry->original_pixels = new uint8_t[imagesSize];
				entry->original_size = imagesSize;
				for (uint32_t b = 0; b < imagesSize; b++) {
					entry->original_pixels[b] = ReadMacInt8(imagesAddr + b);
				}
			}
			RAVE_LOG("TextureNew unconverted (pixelType=%d) %dx%d mips=%d pixmap=0x%08x converted=%d",
			         pixelType, w, h, mipLevels, pixmap, converted ? 1 : 0);
		}
		delete[] expanded;
	}

	// Allocate CPU pixel buffer in Mac address space for AccessTexture support.
	// Level 0 only -- PPC code gets a pointer to read/write pixel data directly.
	uint32_t cpuBufSize = rowBytes * h;
	uint32_t cpuMacAddr = Mac_sysalloc(cpuBufSize);
	if (cpuMacAddr != 0) {
		entry->cpu_pixel_mac_addr = cpuMacAddr;
		entry->cpu_pixel_data = Mac2HostAddr(cpuMacAddr);
		entry->cpu_pixel_data_size = cpuBufSize;
		Host2Mac_memcpy(cpuMacAddr, Mac2HostAddr(pixmap), cpuBufSize);
		RAVE_LOG("TextureNew cpu_pixel_data at Mac 0x%08x (%d bytes)",
		       cpuMacAddr, cpuBufSize);
	} else {
		RAVE_LOG("TextureNew WARN: Mac_sysalloc(%d) failed for cpu_pixel_data", cpuBufSize);
	}
}


/*
 *  RaveRealizeDeferredTexture - create Metal texture from deferred pixel data
 *
 *  Called at first draw-time use (from ApplyDirtyState) when metal_texture is
 *  nullptr and pixmap_mac_addr is set.  Reads from the ORIGINAL pixmap address
 *  in Mac memory — by this point QD3D has written the real texture content.
 */
void RaveRealizeDeferredTexture(RaveResourceEntry *entry)
{
	if (!entry || entry->metal_texture || entry->pixmap_mac_addr == 0) return;

	uint32_t w = entry->width;
	uint32_t h = entry->height;
	uint32_t pixelType = entry->pixel_type;
	uint32_t rowBytes = entry->row_bytes;
	uint32_t mipLevels = entry->mip_levels;

	// Prefer cpu_pixel_data when a
	// Q3Pixmap_Set_Image intercept has synchronously copied the real
	// pixel data into our engine-owned buffer. This handles the
	// classic-Mac transient-buffer lifecycle (Bugdom sprites) where
	// pixmap_mac_addr would otherwise read stale/recycled heap data.
	// If the flag is false, fall back to the original pixmap_mac_addr
	// path — preserves working-title behavior (spider-web sprite,
	// Nanosaur opaque textures, any title whose source buffer is
	// persistent across the first draw).
	uint32_t pixmap = entry->pixmap_mac_addr;
	if (entry->cpu_pixel_data_is_authoritative &&
	    entry->cpu_pixel_mac_addr != 0 &&
	    entry->cpu_pixel_data_size > 0) {
		pixmap = entry->cpu_pixel_mac_addr;
	}

	uint8_t *expanded = new uint8_t[w * h * 4];
	ConvertPixels(pixelType, pixmap, expanded, w, h, rowBytes);
	const bool whitenedAlphaMask =
		(pixelType == kQAPixel_ARGB32) && RaveBGRAWhitenAlphaOnlyMask(expanded, w * h);

	// Check the whole converted image. Bugdom's ARGB16 sprites commonly
	// have transparent top-left borders, so a small leading sample can mark
	// real texture data as empty.
	RaveBGRAImageStats sourceStats = RaveBGRAImageAnalyze(expanded, w * h);
	bool sourceWasEmpty = (sourceStats.nonzero == 0);
	entry->diag_alpha_zero = (w * h) - sourceStats.alpha;
	entry->diag_index_zero = 0;
	entry->diag_rgb_nonzero = sourceStats.rgb;

	entry->metal_texture = RaveCreateMetalTexture(w, h, mipLevels, expanded, w * 4);

	if (mipLevels > 1 && entry->metal_texture) {
		// Generate mips from the realized level 0 rather than the cached per-mip
		// TQAImages (their pixel pointers were stale/garbage by realize time ->
		// black high-mips -> distant surfaces sampled black). CPU 2x2 average is
		// deterministic and correct for both RGB and alpha. See RaveUploadGeneratedMips.
		RaveUploadGeneratedMips(entry->metal_texture, expanded, w, h, mipLevels);
	}

	delete[] expanded;

	// Refresh cpu_pixel_data to match the realized pixel data
	if (entry->cpu_pixel_data && entry->cpu_pixel_mac_addr) {
		Host2Mac_memcpy(entry->cpu_pixel_mac_addr, Mac2HostAddr(pixmap), entry->cpu_pixel_data_size);
	}

	// Record whether any data was present at first realization. Normal
	// QATextureNew calls have copy semantics, so once we have non-empty data
	// the Metal copy becomes authoritative. If the first realization was empty,
	// RaveTextureNeedsLivePixmapRefresh keeps polling until the late-filled
	// pixels show up.
	entry->pixels_copied = !sourceWasEmpty;

	RAVE_LOG("TextureRealize: pixelType=%d %dx%d mips=%d pixmap=0x%08x -> metal=%p empty=%d nz=%u a=%u rgb=%u white=%u first[nz/a/rgb]=%u/%u/%u alphaMaskWhite=%d",
	         pixelType, w, h, mipLevels, pixmap, entry->metal_texture, sourceWasEmpty,
	         sourceStats.nonzero, sourceStats.alpha, sourceStats.rgb, sourceStats.white,
	         sourceStats.first_nonzero, sourceStats.first_alpha, sourceStats.first_rgb,
	         whitenedAlphaMask);
}


/*
 *  Shared bitmap creation helper
 *
 *  Same as texture but single level only (no mipmaps).
 */
static void RaveCreateBitmapFromImage(uint32_t pixelType, uint32 imageAddr,
                                       RaveResourceEntry *entry)
{
	uint32_t w        = ReadMacInt32(imageAddr + 0);
	uint32_t h        = ReadMacInt32(imageAddr + 4);
	uint32_t rowBytes = ReadMacInt32(imageAddr + 8);
	uint32_t pixmap   = ReadMacInt32(imageAddr + 12);

	entry->pixel_type = pixelType;
	entry->width      = w;
	entry->height     = h;
	entry->mip_levels = 1;
	entry->row_bytes  = rowBytes;

	bool isIndexed = (pixelType == kQAPixel_CL8 || pixelType == kQAPixel_CL4 || pixelType == kQAPixel_ACL16_88);

	if (isIndexed) {
		// Copy indexed pixel data immediately (eager copy).
		uint32_t totalSize = rowBytes * h;
		entry->original_pixels = new uint8_t[totalSize];
		entry->original_size = totalSize;
		entry->pixels_copied = true;
		for (uint32_t b = 0; b < totalSize; b++) {
			entry->original_pixels[b] = ReadMacInt8(pixmap + b);
		}
		entry->metal_texture = nullptr;
		RAVE_LOG("BitmapNew indexed (pixelType=%d) %dx%d rowBytes=%d pixmap=0x%08x",
		       pixelType, w, h, rowBytes, pixmap);
	} else {
		uint8_t *expanded = new uint8_t[w * h * 4];
		ConvertPixels(pixelType, pixmap, expanded, w, h, rowBytes);
		entry->metal_texture = RaveCreateMetalTexture(w, h, 1, expanded, w * 4);
		delete[] expanded;
		RAVE_LOG("BitmapNew direct (pixelType=%d) %dx%d -> metal=%p",
		       pixelType, w, h, entry->metal_texture);
	}

	// Allocate CPU pixel buffer in Mac address space for AccessBitmap support
	uint32_t cpuBufSize = rowBytes * h;
	uint32_t cpuMacAddr = Mac_sysalloc(cpuBufSize);
	if (cpuMacAddr != 0) {
		entry->cpu_pixel_mac_addr = cpuMacAddr;
		entry->cpu_pixel_data = Mac2HostAddr(cpuMacAddr);
		entry->cpu_pixel_data_size = cpuBufSize;
		Host2Mac_memcpy(cpuMacAddr, Mac2HostAddr(pixmap), cpuBufSize);
		RAVE_LOG("BitmapNew cpu_pixel_data at Mac 0x%08x (%d bytes)",
		       cpuMacAddr, cpuBufSize);
	} else {
		RAVE_LOG("BitmapNew WARN: Mac_sysalloc(%d) failed for cpu_pixel_data", cpuBufSize);
	}
}


/*
 *  Shared color table creation helper
 */
static void RaveCreateColorTableData(uint32_t tableType, uint32 pixelDataAddr,
                                      int32_t transparentIndex, RaveResourceEntry *entry)
{
	uint32_t count = (tableType == 0) ? 256 : 16;  // CL8_RGB32 vs CL4_RGB32
	uint32_t *clut = new uint32_t[count];

	for (uint32_t i = 0; i < count; i++) {
		uint32_t rgb = ReadMacInt32(pixelDataAddr + i * 4);
		uint8_t r = (rgb >> 16) & 0xFF;
		uint8_t g = (rgb >> 8) & 0xFF;
		uint8_t b = rgb & 0xFF;
		uint8_t a = 0xFF;
		// Store as BGRA in native byte order (little-endian host)
		clut[i] = b | (g << 8) | (r << 16) | (a << 24);
	}

	// Handle transparent index flag
	// RAVE spec (QD3D manual p.1587): transparentIndexFlag is a BOOLEAN.
	// When TRUE (non-zero), entry at INDEX 0 is made fully transparent.
	if (transparentIndex != 0) {
		// Make index 0 fully transparent
		clut[0] &= 0x00FFFFFF;
	}

	entry->clut_data = clut;
	entry->clut_count = count;
	entry->transparent_index = (transparentIndex != 0) ? 0 : -1;
	RaveRememberCL8ColorTableSnapshot(clut, count);

	RAVE_LOG("ColorTableNew tableType=%d count=%d transparentFlag=%d (idx0 %s)",
	       tableType, count, transparentIndex,
	       (transparentIndex != 0) ? "transparent" : "opaque");
}


/*
 *  Re-expand indexed texture with bound CLUT
 *
 *  Called from TextureBindColorTable/BitmapBindColorTable when a CLUT
 *  is bound to an indexed texture. Creates (or recreates) the Metal texture
 *  by expanding original_pixels through the CLUT.
 */
static void RaveReExpandWithCLUT(RaveResourceEntry *texEntry, RaveResourceEntry *clutEntry)
{
	if (!texEntry->original_pixels || !clutEntry->clut_data) return;

	uint32_t w = texEntry->width;
	uint32_t h = texEntry->height;

	// Release existing Metal texture if any
	if (texEntry->metal_texture != nullptr) {
		RaveReleaseTexture(texEntry->metal_texture);
		texEntry->metal_texture = nullptr;
	}

	// Expand level 0
	uint8_t *expanded = new uint8_t[w * h * 4];

	// Build a temporary "Mac memory" source address - but we stored orignal_pixels in native heap.
	// We need to expand from original_pixels directly, not via ReadMacInt*.
	if (texEntry->pixel_type == kQAPixel_CL8) {
		for (uint32_t y = 0; y < h; y++) {
			for (uint32_t x = 0; x < w; x++) {
				uint8_t idx = texEntry->original_pixels[y * texEntry->row_bytes + x];
				if (idx >= clutEntry->clut_count) idx = 0;
				uint32_t bgra = clutEntry->clut_data[idx];
				uint32_t dstIdx = (y * w + x) * 4;
				memcpy(&expanded[dstIdx], &bgra, 4);
			}
		}
	} else if (texEntry->pixel_type == kQAPixel_CL4) {
		for (uint32_t y = 0; y < h; y++) {
			for (uint32_t x = 0; x < w; x++) {
				uint8_t byte = texEntry->original_pixels[y * texEntry->row_bytes + x / 2];
				uint8_t idx;
				if (x & 1)
					idx = byte & 0x0F;
				else
					idx = (byte >> 4) & 0x0F;
				if (idx >= clutEntry->clut_count) idx = 0;
				uint32_t bgra = clutEntry->clut_data[idx];
				uint32_t dstIdx = (y * w + x) * 4;
				memcpy(&expanded[dstIdx], &bgra, 4);
			}
		}
	} else if (texEntry->pixel_type == kQAPixel_ACL16_88) {
		ExpandACL16_88(texEntry->original_pixels, expanded, w, h,
		               texEntry->row_bytes, clutEntry->clut_data, clutEntry->clut_count);
	}

	uint32_t numPx = w * h;
	uint32_t indexZero = 0;
	RaveBGRAImageStats stats = RaveBGRAImageAnalyze(expanded, numPx);
	if (numPx > 0) {
		if (texEntry->pixel_type == kQAPixel_CL8) {
			for (uint32_t y = 0; y < h; y++) {
				for (uint32_t x = 0; x < w; x++) {
					if (texEntry->original_pixels[y * texEntry->row_bytes + x] == 0) indexZero++;
				}
			}
		} else if (texEntry->pixel_type == kQAPixel_CL4) {
			for (uint32_t y = 0; y < h; y++) {
				for (uint32_t x = 0; x < w; x++) {
					uint8_t byte = texEntry->original_pixels[y * texEntry->row_bytes + x / 2];
					uint8_t idx = (x & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
					if (idx == 0) indexZero++;
				}
			}
		}
		texEntry->diag_alpha_zero = numPx - stats.alpha;
		texEntry->diag_index_zero = indexZero;
		texEntry->diag_rgb_nonzero = stats.rgb;
	}

	texEntry->metal_texture = RaveCreateMetalTexture(w, h, texEntry->mip_levels, expanded, w * 4);

	// Generate mipmaps if needed — CPU downsample from the freshly-CLUT-expanded
	// level 0 (the Metal blit path was leaving high-mips black -> indexed/world
	// surfaces sampled black at distance). See RaveUploadGeneratedMips.
	if (texEntry->mip_levels > 1 && texEntry->metal_texture) {
		RaveUploadGeneratedMips(texEntry->metal_texture, expanded, w, h, texEntry->mip_levels);
	}

	delete[] expanded;

	RAVE_LOG("Re-expanded %s %dx%d with CLUT (%d entries) -> metal=%p",
	       (texEntry->type == kRaveResourceTexture) ? "texture" : "bitmap",
	       w, h, clutEntry->clut_count, texEntry->metal_texture);
}


/*
 *  Engine method dispatch functions
 *
 *  These are called directly by the RAVE manager for our engine
 *  (as opposed to the hook path which intercepts the public API).
 */

int32_t NativeEngineTextureNew(uint32_t flags, uint32_t pixelType,
                                uint32_t imagesAddr, uint32_t newTexturePtr)
{
	fprintf(stderr, "RAVE: TextureNew flags=0x%x pixelType=%d images=0x%x\n",
	        flags, pixelType, imagesAddr);
	uint32_t handle = RaveResourceAlloc(kRaveResourceTexture);
	if (handle == 0) {
		WriteMacInt32(newTexturePtr, 0);
		return kQANotSupported;
	}
	RaveResourceEntry *entry = RaveResourceGet(handle);
	RaveCreateTextureFromImages(flags, pixelType, imagesAddr, entry);
	WriteMacInt32(newTexturePtr, entry->mac_addr);
	return kQANoErr;
}

int32_t NativeEngineTextureDetach(uint32_t textureAddr)
{
	// Pixel data is copied eagerly during TextureNew, so Detach is a no-op.
	return kQANoErr;
}

void NativeEngineTextureDelete(uint32_t textureAddr)
{
	uint32_t handle = RaveResourceFindByAddr(textureAddr);
	if (handle != 0) {
		RaveForgetRTTAndFreeResource(handle);
	}
}

int32_t NativeEngineBitmapNew(uint32_t flags, uint32_t pixelType,
                               uint32_t imageAddr, uint32_t newBitmapPtr)
{
	uint32_t handle = RaveResourceAlloc(kRaveResourceBitmap);
	if (handle == 0) {
		WriteMacInt32(newBitmapPtr, 0);
		return kQANotSupported;
	}
	RaveResourceEntry *entry = RaveResourceGet(handle);
	RaveCreateBitmapFromImage(pixelType, imageAddr, entry);
	// 4-bit priority at [31:28] per QACalculatePriorityBits. Test: RAVEABITests.testPriorityBits_extraction_matchesSpec
	entry->priority = (uint8_t)((flags >> 28) & 0xF);
	WriteMacInt32(newBitmapPtr, entry->mac_addr);
	return kQANoErr;
}

int32_t NativeEngineBitmapDetach(uint32_t bitmapAddr)
{
	// Pixel data is copied eagerly during BitmapNew, so Detach is a no-op.
	return kQANoErr;
}

void NativeEngineBitmapDelete(uint32_t bitmapAddr)
{
	uint32_t handle = RaveResourceFindByAddr(bitmapAddr);
	if (handle != 0) {
		RaveForgetRTTAndFreeResource(handle);
	}
}

int32_t NativeEngineColorTableNew(uint32_t tableType, uint32_t pixelDataAddr,
                                   uint32_t transparentIndexOrFlag, uint32_t newTablePtr)
{
	uint32_t handle = RaveResourceAlloc(kRaveResourceColorTable);
	if (handle == 0) {
		WriteMacInt32(newTablePtr, 0);
		return kQANotSupported;
	}
	RaveResourceEntry *entry = RaveResourceGet(handle);
	RaveCreateColorTableData(tableType, pixelDataAddr, (int32_t)transparentIndexOrFlag, entry);
	WriteMacInt32(newTablePtr, entry->mac_addr);
	return kQANoErr;
}

void NativeEngineColorTableDelete(uint32_t colorTableAddr)
{
	uint32_t handle = RaveResourceFindByAddr(colorTableAddr);
	if (handle != 0) {
		RaveResourceFree(handle);
	}
}

int32_t NativeEngineTextureBindColorTable(uint32_t textureAddr, uint32_t colorTableAddr)
{
	uint32_t texHandle = RaveResourceFindByAddr(textureAddr);
	uint32_t clutHandle = RaveResourceFindByAddr(colorTableAddr);
	if (texHandle == 0 || clutHandle == 0) return kQANotSupported;

	RaveResourceEntry *texEntry = RaveResourceGet(texHandle);
	RaveResourceEntry *clutEntry = RaveResourceGet(clutHandle);
	if (!texEntry || !clutEntry) return kQANotSupported;

	texEntry->bound_clut = clutHandle;

	// If texture has indexed original pixels, re-expand immediately
	if (texEntry->original_pixels) {
		RaveReExpandWithCLUT(texEntry, clutEntry);
	}

	return kQANoErr;
}

int32_t NativeEngineBitmapBindColorTable(uint32_t bitmapAddr, uint32_t colorTableAddr)
{
	uint32_t bmpHandle = RaveResourceFindByAddr(bitmapAddr);
	uint32_t clutHandle = RaveResourceFindByAddr(colorTableAddr);
	if (bmpHandle == 0 || clutHandle == 0) return kQANotSupported;

	RaveResourceEntry *bmpEntry = RaveResourceGet(bmpHandle);
	RaveResourceEntry *clutEntry = RaveResourceGet(clutHandle);
	if (!bmpEntry || !clutEntry) return kQANotSupported;

	bmpEntry->bound_clut = clutHandle;

	if (bmpEntry->original_pixels) {
		RaveReExpandWithCLUT(bmpEntry, clutEntry);
	}

	return kQANoErr;
}


/*
 *  AccessTexture/End and AccessBitmap/End - CPU pixel access
 *
 *  These allow PPC code to read/write texture/bitmap pixels at runtime.
 *  AccessTexture returns a pointer to a CPU-side copy of the pixel data.
 *  AccessTextureEnd re-uploads modified pixel data back to the Metal texture.
 */

// SDK signature: TQAError TQAAccessTexture(TQATexture*, long mipmapLevel, long flags, TQAPixelBuffer*)
// TQAPixelBuffer = TQADeviceMemory = {rowBytes(+0), pixelType(+4), width(+8), height(+12), baseAddr(+16)}
// mipmapLevel is accepted but only level 0 data is returned (no known game uses per-mip access).
int32_t NativeEngineAccessTexture(uint32_t textureAddr, uint32_t mipmapLevel,
                                   uint32_t flags, uint32_t bufferStructAddr)
{
	uint32_t handle = RaveResourceFindByAddr(textureAddr);
	if (handle == 0) return kQANotSupported;
	RaveResourceEntry *entry = RaveResourceGet(handle);
	if (!entry || entry->type != kRaveResourceTexture) return kQANotSupported;
	if (!entry->cpu_pixel_data || entry->cpu_pixel_mac_addr == 0) return kQANotSupported;

	// Populate TQAPixelBuffer (TQADeviceMemory) struct at bufferStructAddr
	WriteMacInt32(bufferStructAddr + 0,  entry->row_bytes);
	WriteMacInt32(bufferStructAddr + 4,  entry->pixel_type);
	WriteMacInt32(bufferStructAddr + 8,  entry->width);
	WriteMacInt32(bufferStructAddr + 12, entry->height);
	WriteMacInt32(bufferStructAddr + 16, entry->cpu_pixel_mac_addr);
	RAVE_LOG("AccessTexture: tex=0x%08x mip=%d flags=0x%x -> buf=0x%08x %dx%d rowBytes=%d",
	         textureAddr, mipmapLevel, flags, entry->cpu_pixel_mac_addr,
	         entry->width, entry->height, entry->row_bytes);
	return kQANoErr;
}

int32_t NativeEngineAccessTextureEnd(uint32_t textureAddr, uint32_t dirtyRectAddr)
{
	uint32_t handle = RaveResourceFindByAddr(textureAddr);
	if (handle == 0) return kQANotSupported;
	RaveResourceEntry *entry = RaveResourceGet(handle);
	if (!entry || entry->type != kRaveResourceTexture) return kQANotSupported;
	if (!entry->cpu_pixel_data) return kQANotSupported;

	// Re-convert from original Mac format to BGRA8Unorm and re-upload
	uint32_t w = entry->width;
	uint32_t h = entry->height;
	uint8_t *expanded = new uint8_t[w * h * 4];

	bool isIndexed = (entry->pixel_type == kQAPixel_CL8 || entry->pixel_type == kQAPixel_CL4 || entry->pixel_type == kQAPixel_ACL16_88);

	if (isIndexed && entry->bound_clut != 0) {
		RaveResourceEntry *clutEntry = RaveResourceGet(entry->bound_clut);
		if (clutEntry && clutEntry->clut_data) {
			if (entry->pixel_type == kQAPixel_CL8) {
				for (uint32_t y = 0; y < h; y++) {
					for (uint32_t x = 0; x < w; x++) {
						uint8_t idx = entry->cpu_pixel_data[y * entry->row_bytes + x];
						if (idx >= clutEntry->clut_count) idx = 0;
						uint32_t bgra = clutEntry->clut_data[idx];
						memcpy(&expanded[(y * w + x) * 4], &bgra, 4);
					}
				}
			} else if (entry->pixel_type == kQAPixel_ACL16_88) {
				for (uint32_t y = 0; y < h; y++) {
					for (uint32_t x = 0; x < w; x++) {
						uint32_t off = y * entry->row_bytes + x * 2;
						uint8_t a = entry->cpu_pixel_data[off];
						uint8_t idx = entry->cpu_pixel_data[off + 1];
						if (idx >= clutEntry->clut_count) idx = 0;
						uint32_t bgra = clutEntry->clut_data[idx];
						memcpy(&expanded[(y * w + x) * 4], &bgra, 4);
						expanded[(y * w + x) * 4 + 3] = a;  // Override alpha
					}
				}
			} else { // CL4
				for (uint32_t y = 0; y < h; y++) {
					for (uint32_t x = 0; x < w; x++) {
						uint8_t byte = entry->cpu_pixel_data[y * entry->row_bytes + x / 2];
						uint8_t idx = (x & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
						if (idx >= clutEntry->clut_count) idx = 0;
						uint32_t bgra = clutEntry->clut_data[idx];
						memcpy(&expanded[(y * w + x) * 4], &bgra, 4);
					}
				}
			}
		}
	} else if (!isIndexed) {
		// Direct format: convert from Mac-endian cpu_pixel_data to BGRA8
		// cpu_pixel_data is in Mac memory, so use its Mac address for conversion
		uint32_t macAddr = entry->cpu_pixel_mac_addr;
		ConvertPixels(entry->pixel_type, macAddr, expanded, w, h, entry->row_bytes);
		if (entry->type == kRaveResourceTexture && entry->pixel_type == kQAPixel_ARGB32) {
			RaveBGRAWhitenAlphaOnlyMask(expanded, w * h);
		}
	}

	if (entry->type == kRaveResourceTexture) {
		RaveBGRAImageStats stats = RaveBGRAImageAnalyze(expanded, w * h);
		entry->diag_alpha_zero = (w * h) - stats.alpha;
		entry->diag_index_zero = 0;
		entry->diag_rgb_nonzero = stats.rgb;
	}

	// Re-upload level 0
	if (entry->metal_texture) {
		// Batch level 0 + regenerated chain into one pass break (D-R-4).
		RaveTextureUploadBatchBegin();
		RaveUploadMipLevel(entry->metal_texture, 0, w, h, expanded, w * 4);
		// Regenerate mipmaps if multi-level — CPU downsample from the fresh level 0
		// (deterministic; the Metal blit path was leaving high-mips black on refresh).
		if (entry->mip_levels > 1) {
			RaveUploadGeneratedMips(entry->metal_texture, expanded, w, h, entry->mip_levels);
		}
		RaveTextureUploadBatchEnd();
	}

	delete[] expanded;
	RAVE_LOG("AccessTextureEnd: tex=0x%08x re-uploaded %dx%d", textureAddr, w, h);
	return kQANoErr;
}

// SDK signature: TQAError TQAAccessBitmap(TQABitmap*, long flags, TQAPixelBuffer*)
// TQAPixelBuffer = TQADeviceMemory = {rowBytes(+0), pixelType(+4), width(+8), height(+12), baseAddr(+16)}
int32_t NativeEngineAccessBitmap(uint32_t bitmapAddr, uint32_t flags,
                                  uint32_t bufferStructAddr)
{
	uint32_t handle = RaveResourceFindByAddr(bitmapAddr);
	if (handle == 0) return kQANotSupported;
	RaveResourceEntry *entry = RaveResourceGet(handle);
	if (!entry || entry->type != kRaveResourceBitmap) return kQANotSupported;
	if (!entry->cpu_pixel_data || entry->cpu_pixel_mac_addr == 0) return kQANotSupported;

	// Populate TQAPixelBuffer (TQADeviceMemory) struct at bufferStructAddr
	WriteMacInt32(bufferStructAddr + 0,  entry->row_bytes);
	WriteMacInt32(bufferStructAddr + 4,  entry->pixel_type);
	WriteMacInt32(bufferStructAddr + 8,  entry->width);
	WriteMacInt32(bufferStructAddr + 12, entry->height);
	WriteMacInt32(bufferStructAddr + 16, entry->cpu_pixel_mac_addr);
	RAVE_LOG("AccessBitmap: bmp=0x%08x flags=0x%x -> buf=0x%08x %dx%d rowBytes=%d",
	         bitmapAddr, flags, entry->cpu_pixel_mac_addr,
	         entry->width, entry->height, entry->row_bytes);
	return kQANoErr;
}

int32_t NativeEngineAccessBitmapEnd(uint32_t bitmapAddr, uint32_t dirtyRectAddr)
{
	uint32_t handle = RaveResourceFindByAddr(bitmapAddr);
	if (handle == 0) return kQANotSupported;
	RaveResourceEntry *entry = RaveResourceGet(handle);
	if (!entry || entry->type != kRaveResourceBitmap) return kQANotSupported;
	if (!entry->cpu_pixel_data) return kQANotSupported;

	uint32_t w = entry->width;
	uint32_t h = entry->height;
	uint8_t *expanded = new uint8_t[w * h * 4];

	bool isIndexed = (entry->pixel_type == kQAPixel_CL8 || entry->pixel_type == kQAPixel_CL4 || entry->pixel_type == kQAPixel_ACL16_88);

	if (isIndexed && entry->bound_clut != 0) {
		RaveResourceEntry *clutEntry = RaveResourceGet(entry->bound_clut);
		if (clutEntry && clutEntry->clut_data) {
			if (entry->pixel_type == kQAPixel_CL8) {
				for (uint32_t y = 0; y < h; y++) {
					for (uint32_t x = 0; x < w; x++) {
						uint8_t idx = entry->cpu_pixel_data[y * entry->row_bytes + x];
						if (idx >= clutEntry->clut_count) idx = 0;
						uint32_t bgra = clutEntry->clut_data[idx];
						memcpy(&expanded[(y * w + x) * 4], &bgra, 4);
					}
				}
			} else if (entry->pixel_type == kQAPixel_ACL16_88) {
				for (uint32_t y = 0; y < h; y++) {
					for (uint32_t x = 0; x < w; x++) {
						uint32_t off = y * entry->row_bytes + x * 2;
						uint8_t a = entry->cpu_pixel_data[off];
						uint8_t idx = entry->cpu_pixel_data[off + 1];
						if (idx >= clutEntry->clut_count) idx = 0;
						uint32_t bgra = clutEntry->clut_data[idx];
						memcpy(&expanded[(y * w + x) * 4], &bgra, 4);
						expanded[(y * w + x) * 4 + 3] = a;  // Override alpha
					}
				}
			} else { // CL4
				for (uint32_t y = 0; y < h; y++) {
					for (uint32_t x = 0; x < w; x++) {
						uint8_t byte = entry->cpu_pixel_data[y * entry->row_bytes + x / 2];
						uint8_t idx = (x & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
						if (idx >= clutEntry->clut_count) idx = 0;
						uint32_t bgra = clutEntry->clut_data[idx];
						memcpy(&expanded[(y * w + x) * 4], &bgra, 4);
					}
				}
			}
		}
	} else if (!isIndexed) {
		uint32_t macAddr = entry->cpu_pixel_mac_addr;
		ConvertPixels(entry->pixel_type, macAddr, expanded, w, h, entry->row_bytes);
	}

	if (entry->metal_texture) {
		RaveUploadMipLevel(entry->metal_texture, 0, w, h, expanded, w * 4);
		// Bitmaps have only 1 mip level, no mipmap generation needed
	}

	delete[] expanded;
	RAVE_LOG("AccessBitmapEnd: bmp=0x%08x re-uploaded %dx%d", bitmapAddr, w, h);
	return kQANoErr;
}


// Engine ASCII name
static const char kEngineASCIIName[] = "PocketShaver RAVE";
static const uint32 kEngineASCIINameLength = 17;

// Gestalt selector names for logging
static const char *gestalt_selector_names[] = {
	"OptionalFeatures",                    // 0
	"FastFeatures",                        // 1
	"VendorID",                            // 2
	"EngineID",                            // 3
	"Revision",                            // 4
	"ASCIINameLength",                     // 5
	"ASCIIName",                           // 6
	"TextureMemory",                       // 7
	"FastTextureMemory",                   // 8
	"DrawContextPixelTypesAllowed",        // 9
	"DrawContextPixelTypesPreferred",      // 10
	"TexturePixelTypesAllowed",            // 11
	"TexturePixelTypesPreferred",          // 12
	"BitmapPixelTypesAllowed",             // 13
	"BitmapPixelTypesPreferred",           // 14
	"OptionalFeatures2",                   // 15
	"MultiTextureMax",                     // 16
	"NumSelectors"                         // 17
};


/*
 *  NativeEngineGetMethod - TQAEngineGetMethod callback
 *
 *  Called by RAVE manager via the sub-opcode 100 TVECT.
 *  Arguments (from PPC registers, passed explicitly by dispatch):
 *    methodTag = TQAEngineMethodTag (0-17)
 *    methodPtr = pointer to TQAEngineMethod union (Mac address)
 *
 *  Writes the appropriate TVECT Mac address into the method union.
 *  Returns kQANoErr for known tags, kQANotSupported for unknown.
 */
int32 NativeEngineGetMethod(uint32 methodTag, uint32 methodPtr)
{
	if (methodTag >= kQAEngineMethodTagCount) {
		RAVE_LOG("EngineGetMethod: unknown tag %d -> kQANotSupported", methodTag);
		return kQANotSupported;
	}

	// Map TQAEngineMethodTag to our sub-opcode TVECT
	// Engine method sub-opcodes start at 100 (kRaveEngineDrawPrivateNew)
	// methodTag N -> sub-opcode (100 + N) -> rave_method_tvects[100 + N]
	uint32 tvect_addr = rave_method_tvects[kRaveEngineDrawPrivateNew + methodTag];

	if (tvect_addr == 0) {
		RAVE_LOG("EngineGetMethod: tag %d has no TVECT -> kQANotSupported", methodTag);
		return kQANotSupported;
	}

	// Write the TVECT Mac address into the TQAEngineMethod union
	// The union is a single function pointer (uint32 in Mac address space)
	WriteMacInt32(methodPtr, tvect_addr);

	RAVE_LOG("EngineGetMethod: tag %d -> TVECT 0x%08x", methodTag, tvect_addr);
	return kQANoErr;
}


/*
 *  NativeEngineGestalt - TQAEngineGestalt callback
 *
 *  Called by RAVE manager via the sub-opcode 103 TVECT (DDK tag 3).
 *  Arguments (from PPC registers, passed explicitly by dispatch):
 *    selector    = TQAGestaltSelector (0-17)
 *    responsePtr = pointer to response buffer (Mac address)
 *
 *  Writes the response value and returns kQANoErr or kQANotSupported.
 */
int32 NativeEngineGestalt(uint32 selector, uint32 responsePtr)
{
	switch (selector) {
	case kQAGestalt_OptionalFeatures:
		WriteMacInt32(responsePtr, kAllOptionalFeatures);
		RAVE_LOG("EngineGestalt: %s -> 0x%08x", gestalt_selector_names[selector], kAllOptionalFeatures);
		break;

	case kQAGestalt_FastFeatures:
		// Use kAllFastFeatures (kQAFast_* namespace per spec). Test: RAVEABITests.testKQAFast_bitPositions_matchSpec
		// Metal accelerates everything, so bit values match OptionalFeatures
		WriteMacInt32(responsePtr, kAllFastFeatures);
		RAVE_LOG("EngineGestalt: %s -> 0x%08x", gestalt_selector_names[selector], kAllFastFeatures);
		break;

	case kQAGestalt_VendorID:
		WriteMacInt32(responsePtr, kRaveAdvertisedVendorID);
		RAVE_LOG("EngineGestalt: %s -> %u (kQAVendor_ATI)", gestalt_selector_names[selector],
		         kRaveAdvertisedVendorID);
		break;

	case kQAGestalt_EngineID:
		WriteMacInt32(responsePtr, kRaveAdvertisedEngineID);
		RAVE_LOG("EngineGestalt: %s -> 0x%08x (ATI Rage 128)", gestalt_selector_names[selector],
		         kRaveAdvertisedEngineID);
		break;

	case kQAGestalt_Revision:
		WriteMacInt32(responsePtr, kRaveAdvertisedRevision);
		RAVE_LOG("EngineGestalt: %s -> 0x%08x (1.0)", gestalt_selector_names[selector],
		         kRaveAdvertisedRevision);
		break;

	case kQAGestalt_ASCIINameLength:
		WriteMacInt32(responsePtr, kEngineASCIINameLength);
		RAVE_LOG("EngineGestalt: %s -> %d", gestalt_selector_names[selector], kEngineASCIINameLength);
		break;

	case kQAGestalt_ASCIIName:
		// Copy name string into Mac address space (SDK specifies strcpy semantics, must NUL-terminate)
		Host2Mac_memcpy(responsePtr, (uint8 *)kEngineASCIIName, kEngineASCIINameLength);
		WriteMacInt8(responsePtr + kEngineASCIINameLength, 0);
		RAVE_LOG("EngineGestalt: %s -> \"%s\"", gestalt_selector_names[selector], kEngineASCIIName);
		break;

	case kQAGestalt_TextureMemory:
	case kQAGestalt_FastTextureMemory:
		WriteMacInt32(responsePtr, kRaveAdvertisedTextureMemoryBytes);
		RAVE_LOG("EngineGestalt: %s -> 64MB", gestalt_selector_names[selector]);
		break;

	case kQAGestalt_DrawContextPixelTypesAllowed:
		// Must include RGB32 — Mac OS "Millions of colors" is 24-bit stored as xRGB8888
		WriteMacInt32(responsePtr, (1 << kQAPixel_ARGB32) | (1 << kQAPixel_RGB32) |
					  (1 << kQAPixel_RGB16));
		RAVE_LOG("EngineGestalt: %s -> ARGB32|RGB32|RGB16", gestalt_selector_names[selector]);
		break;

	case kQAGestalt_DrawContextPixelTypesPreferred:
		WriteMacInt32(responsePtr, (1 << kQAPixel_RGB32));
		RAVE_LOG("EngineGestalt: %s -> RGB32", gestalt_selector_names[selector]);
		break;

	case kQAGestalt_TexturePixelTypesAllowed:
		// Added RGB32 and RGB16 to allowed texture pixel types
		WriteMacInt32(responsePtr, (1 << kQAPixel_ARGB32) | (1 << kQAPixel_RGB32) |
					  (1 << kQAPixel_ARGB16) | (1 << kQAPixel_RGB16) |
					  (1 << kQAPixel_CL8) | (1 << kQAPixel_CL4) |
					  (1 << kQAPixel_RGB8_332) | (1 << kQAPixel_ARGB16_4444) |
					  (1 << kQAPixel_ACL16_88) | (1 << kQAPixel_I8) | (1 << kQAPixel_AI16_88));
		RAVE_LOG("EngineGestalt: %s -> ARGB32|RGB32|ARGB16|RGB16|CL8|CL4|RGB8_332|ARGB16_4444|ACL16_88|I8|AI16_88", gestalt_selector_names[selector]);
		break;

	case kQAGestalt_TexturePixelTypesPreferred:
		WriteMacInt32(responsePtr, (1 << kQAPixel_ARGB32));
		RAVE_LOG("EngineGestalt: %s -> ARGB32", gestalt_selector_names[selector]);
		break;

	case kQAGestalt_BitmapPixelTypesAllowed:
		WriteMacInt32(responsePtr, (1 << kQAPixel_ARGB32) | (1 << kQAPixel_RGB32) |
					  (1 << kQAPixel_ARGB16) | (1 << kQAPixel_RGB16) |
					  (1 << kQAPixel_CL8) | (1 << kQAPixel_CL4) |
					  (1 << kQAPixel_RGB8_332) | (1 << kQAPixel_ARGB16_4444) |
					  (1 << kQAPixel_ACL16_88) | (1 << kQAPixel_I8) | (1 << kQAPixel_AI16_88));
		RAVE_LOG("EngineGestalt: %s -> ARGB32|RGB32|ARGB16|RGB16|CL8|CL4|...", gestalt_selector_names[selector]);
		break;

	case kQAGestalt_BitmapPixelTypesPreferred:
		WriteMacInt32(responsePtr, (1 << kQAPixel_ARGB32));
		RAVE_LOG("EngineGestalt: %s -> ARGB32", gestalt_selector_names[selector]);
		break;

	case kQAGestalt_OptionalFeatures2:
		WriteMacInt32(responsePtr, kAllOptionalFeatures2);
		RAVE_LOG("EngineGestalt: %s -> 0x%08x", gestalt_selector_names[selector], kAllOptionalFeatures2);
		break;

	case kQAGestalt_MultiTextureMax:
		WriteMacInt32(responsePtr, 2);
		RAVE_LOG("EngineGestalt: %s -> 2", gestalt_selector_names[selector]);
		break;

	case kQAGestalt_NumSelectors:
		// kQAGestalt_NumSelectors = 17 per RAVE.h -- this IS the count of valid selectors (0-16)
		WriteMacInt32(responsePtr, 17);
		RAVE_LOG("EngineGestalt: %s -> 17", gestalt_selector_names[selector]);
		break;

	default:
		// kQANotSupported is correct per spec for engines without engine-specific selectors
		// (kQAGestalt_EngineSpecific_Minimum and above fall through to here)
		// ATI gestalt selectors (1000+)
		if (selector == 1000) {  // kQATIGestalt_CurrentContext
			WriteMacInt32(responsePtr, rave_current_draw_context_addr);
			RAVE_LOG("EngineGestalt: kQATIGestalt_CurrentContext -> 0x%08x",
			         rave_current_draw_context_addr);
			break;
		}
		RAVE_LOG("EngineGestalt: unknown selector %d -> kQANotSupported", selector);
		return kQANotSupported;
	}

	return kQANoErr;
}


/*
 *  NativeEngineCheckDevice - TQAEngineCheckDevice callback
 *
 *  Called by RAVE manager via the sub-opcode 102 TVECT (DDK tag 2).
 *  Arguments (from PPC registers, passed explicitly by dispatch):
 *    devicePtr = pointer to TQADevice struct (Mac address)
 *
 *  TQADevice struct: first field is deviceType (uint32)
 *    kQADeviceMemory (0) = software rendering to memory
 *    kQADeviceGDevice (1) = rendering to a GDevice (screen)
 *
 *  Returns kQANoErr for both device types per locked decision.
 */
int32 NativeEngineCheckDevice(uint32 devicePtr)
{
	// Read device type from TQADevice struct (first field)
	uint32 deviceType = ReadMacInt32(devicePtr);

	RaveLogDeviceSummary("EngineCheckDevice", devicePtr);
	RAVE_LOG("EngineCheckDevice: %s(%u) -> kQANoErr",
	         RaveDeviceTypeName(deviceType), deviceType);

	// Accept all device types for maximum compatibility
	return kQANoErr;
}


/*
 *  Hook handler: QADeviceGetFirstEngine(device) -> TQAEngine*
 *
 *  Always returns our sentinel engine as the first result.
 *  The caller will later call QADeviceGetNextEngine to continue enumeration.
 *
 *  r3 = device (TQADevice*, may be NULL to enumerate all)
 */
uint32 NativeHookGetFirstEngine(uint32 device)
{
	RaveLogDeviceSummary("HOOK: QADeviceGetFirstEngine", device);
	RAVE_LOG("HOOK: QADeviceGetFirstEngine(device=0x%08x) -> sentinel 0x%08x",
		   device, rave_sentinel_engine);
	return rave_sentinel_engine;
}


/*
 *  Hook handler: QADeviceGetNextEngine(device, prevEngine) -> TQAEngine*
 *
 *  If prevEngine is our sentinel, chain to original QADeviceGetFirstEngine
 *  to get the real first engine (so our engine is prepended to the list).
 *  Otherwise, chain to original QADeviceGetNextEngine.
 *
 *  r3 = device (TQADevice*), r4 = prevEngine (TQAEngine*)
 */
uint32 NativeHookGetNextEngine(uint32 device, uint32 prevEngine)
{
	if (prevEngine == rave_sentinel_engine) {
		// Previous was our sentinel -- return NULL to end enumeration.
		//
		// We are the only RAVE engine (QARegisterEngine didn't add us to the
		// manager's internal list, so there are no "real" engines to chain to).
		// Attempting to chain to the original GetFirstEngine via its trampoline
		// fails because the saved prologue contains a PC-relative branch (bl)
		// that resolves to the wrong address when relocated, causing an infinite
		// loop where the caller keeps seeing sentinel returned.
		RAVE_LOG("HOOK: QADeviceGetNextEngine(prev=sentinel) -> NULL (end of list)");
		return 0;
	} else {
		// Previous was a real engine -- chain to original GetNextEngine
		RAVE_LOG("HOOK: QADeviceGetNextEngine(prev=0x%08x) -> chaining to original", prevEngine);
		if (rave_orig_get_next_engine == 0) return 0;
		const uint32 args[] = { device, prevEngine };
		return RaveChainToOriginal(kRaveHookIdx_GetNextEngine, 2, args);
	}
}


/*
 *  Hook handler: QAEngineGestalt(engine, selector, response) -> TQAError
 *
 *  If engine is our sentinel, dispatch to NativeEngineGestalt.
 *  Otherwise chain to original QAEngineGestalt.
 *
 *  r3 = engine (TQAEngine*), r4 = selector, r5 = response ptr
 */
uint32 NativeHookEngineGestalt(uint32 engine, uint32 selector, uint32 responsePtr)
{
	if (engine == rave_sentinel_engine) {
		RAVE_LOG("HOOK: QAEngineGestalt(sentinel, sel=%d) -> native", selector);
		return (uint32)NativeEngineGestalt(selector, responsePtr);
	} else {
		if (rave_orig_engine_gestalt == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, selector, responsePtr };
		return RaveChainToOriginal(kRaveHookIdx_EngineGestalt, 3, args);
	}
}


/*
 *  Hook handler: QAEngineCheckDevice(engine, device) -> TQAError
 *
 *  If engine is our sentinel, return kQANoErr (we support all devices).
 *  Otherwise chain to original QAEngineCheckDevice.
 *
 *  r3 = engine (TQAEngine*), r4 = device (TQADevice*)
 */
uint32 NativeHookEngineCheckDevice(uint32 engine, uint32 device)
{
	if (engine == rave_sentinel_engine) {
		RAVE_LOG("HOOK: QAEngineCheckDevice(sentinel, device=0x%08x) -> kQANoErr", device);
		return kQANoErr;
	} else {
		if (rave_orig_engine_check_device == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, device };
		return RaveChainToOriginal(kRaveHookIdx_EngineCheckDevice, 2, args);
	}
}

/*
 *  Hook handler: QAEngineEnable(vendorID, engineID) -> TQAError
 *
 *  Some games use this manager API as an engine-selection gate before
 *  creating a draw context. The RAVE manager's internal engine list does not
 *  reliably contain our injected sentinel, so handle BestChoice/ATI locally
 *  and log the exact IDs requested.
 */
uint32 NativeHookEngineEnable(uint32 vendorID, uint32 engineID)
{
	RAVE_LOG("HOOK: QAEngineEnable(vendor=0x%08x engine=0x%08x)", vendorID, engineID);
	if (RaveEngineEnableHandledByNative(vendorID, engineID)) {
		RAVE_LOG("HOOK: QAEngineEnable -> kQANoErr (native accepted)");
		return kQANoErr;
	}

	if (rave_orig_engine_enable == 0) return (uint32)(int32)kQANotSupported;
	const uint32 args[] = { vendorID, engineID };
	const uint32 result = RaveChainToOriginal(kRaveHookIdx_EngineEnable, 2, args);
	RAVE_LOG("HOOK: QAEngineEnable -> chained result %d", (int32)result);
	return result;
}


/*
 *  Hook handler: QAEngineDisable(vendorID, engineID) -> TQAError
 */
uint32 NativeHookEngineDisable(uint32 vendorID, uint32 engineID)
{
	RAVE_LOG("HOOK: QAEngineDisable(vendor=0x%08x engine=0x%08x)", vendorID, engineID);
	if (RaveEngineEnableHandledByNative(vendorID, engineID)) {
		RAVE_LOG("HOOK: QAEngineDisable -> kQANoErr (native accepted)");
		return kQANoErr;
	}

	if (rave_orig_engine_disable == 0) return (uint32)(int32)kQANotSupported;
	const uint32 args[] = { vendorID, engineID };
	const uint32 result = RaveChainToOriginal(kRaveHookIdx_EngineDisable, 2, args);
	RAVE_LOG("HOOK: QAEngineDisable -> chained result %d", (int32)result);
	return result;
}


/*
 *  Hook handler: QADrawContextNew(device, rect, clip, engine, flags, drawContext,
 *                                  drawPrivate) -> TQAError
 *
 *  QADrawContextNew signature (from DDK RAVE.h):
 *    TQAError QADrawContextNew(const TQADevice *device, const TQARect *rect,
 *                               const TQAClip *clip, const TQAEngine *engine,
 *                               unsigned long flags, TQADrawContext **drawContext,
 *                               TQADrawPrivate *drawPrivate)
 *
 *  Wait -- actually QADrawContextNew is a RAVE manager function that internally
 *  calls engine->DrawPrivateNew. The manager does:
 *    1. Allocate TQADrawContext
 *    2. Call engine's DrawPrivateNew(drawContext, device, rect, clip, flags)
 *    3. Return the drawContext to caller
 *
 *  Since the manager doesn't know about our engine (not in its internal list),
 *  passing our sentinel to QADrawContextNew will fail. We need to intercept it
 *  and create the draw context ourselves.
 *
 *  The hook receives the original args: r3=device, r4=rect, r5=clip, r6=engine,
 *  r7=flags, r8=drawContextPtr (output)
 *
 *  Actually, looking at this more carefully: QADrawContextNew allocates and manages
 *  TQADrawContext internally, populating it with draw method TVECTs from the engine.
 *  We need to either replicate that or find a simpler way.
 *
 *  For now: if engine is our sentinel, we call our NativeDrawPrivateNew and
 *  construct a minimal TQADrawContext that the caller can use. The draw method
 *  TVECTs in the context point to our PPC thunks.
 *
 *  r3 = device, r4 = rect, r5 = clip, r6 = engine, r7 = flags, r8 = drawContextPtr
 */
uint32 NativeHookDrawContextNew(uint32 device, uint32 rect, uint32 clip,
                                 uint32 engine, uint32 flags, uint32 drawContextPtr)
{
	RAVE_LOG("HOOK: QADrawContextNew engine=0x%08x sentinel=0x%08x device=0x%08x rect=0x%08x clip=0x%08x flags=0x%08x ctxPtr=0x%08x",
	       engine, rave_sentinel_engine, device, rect, clip, flags, drawContextPtr);
	if (engine == rave_sentinel_engine) {
		RAVE_LOG("HOOK: QADrawContextNew(sentinel) -> creating context");

		// Allocate TQADrawContext in Mac memory.
		// Layout (from rave_draw_context.mm):
		//   +0:   drawPrivate handle (uint32)
		//   +4:   version (uint32)
		//   +8:   35 draw method TVECTs (35 * 4 = 140 bytes)
		// Total: 148 bytes
		uint32 ctx_size = 4 + 4 + kRaveDrawMethodCount * 4;  // 148 bytes
		uint32 ctx;
		if (!rave_ctx_free_list.empty()) {
			ctx = rave_ctx_free_list.back();
			rave_ctx_free_list.pop_back();
			RAVE_LOG("HOOK: reusing ctx=0x%08x from free list (%zu remain)",
			       ctx, rave_ctx_free_list.size());
		} else {
			ctx = SheepMem::ReserveProc(ctx_size);
			RAVE_LOG("HOOK: allocated ctx=0x%08x from SheepMem proc", ctx);
		}
		// NativeDrawPrivateNew writes all 37 fields (handle + version +
		// 35 TVECTs), so we skip zeroing here.  A memset would create a
		// window where stale Mac-side references see NULL TVECT pointers,
		// causing the PPC to dereference address 0 and crash in zeroed
		// SheepMem memory.
		RAVE_LOG("HOOK: calling NativeDrawPrivateNew(ctx=0x%08x, dev=0x%08x, rect=0x%08x, clip=0x%08x, flags=0x%08x)",
		       ctx, device, rect, clip, flags);
		int32 err = NativeDrawPrivateNew(ctx, device, rect, clip, flags);
		RAVE_LOG("HOOK: NativeDrawPrivateNew returned %d", err);
		if (err != kQANoErr) {
			RAVE_LOG("HOOK: DrawPrivateNew failed with %d", err);
			return (uint32)err;
		}

		// Write the draw context pointer to the output parameter
		WriteMacInt32(drawContextPtr, ctx);

		uint32 drawPrivate = ReadMacInt32(ctx);
		RAVE_LOG("HOOK: QADrawContextNew -> ctx=0x%08x, drawPrivate=%d",
			   ctx, drawPrivate);
		return kQANoErr;
	} else {
		// Not our engine -- chain to original QADrawContextNew
		if (rave_orig_draw_context_new == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { device, rect, clip, engine, flags, drawContextPtr };
		return RaveChainToOriginal(kRaveHookIdx_DrawContextNew, 6, args);
	}
}


/*
 *  Hook handler: QATextureNew(engine, flags, pixelType, images[], &newTexture)
 *
 *  For sentinel engine: create a stub texture handle (real texture upload
 *  to Metal is implemented later). For other engines: chain to original.
 *
 *  r3 = engine, r4 = flags, r5 = pixelType, r6 = images, r7 = &newTexture
 */
uint32 NativeHookTextureNew(uint32 engine, uint32 flags, uint32 pixelType,
                            uint32 images, uint32 newTexturePtr)
{
	if (engine == rave_sentinel_engine) {
		uint32_t handle = RaveResourceAlloc(kRaveResourceTexture);
		if (handle == 0) {
			RAVE_LOG("HOOK: QATextureNew(sentinel) FAILED - resource table full");
			WriteMacInt32(newTexturePtr, 0);
			return (uint32)(int32)kQANotSupported;
		}
		RaveResourceEntry *entry = RaveResourceGet(handle);
		RaveCreateTextureFromImages(flags, pixelType, images, entry);
		RAVE_LOG("HOOK: QATextureNew(sentinel) flags=0x%x pixelType=%d %dx%d -> handle %d addr=0x%08x metal=%p",
		       flags, pixelType, entry->width, entry->height, handle, entry->mac_addr, entry->metal_texture);
		WriteMacInt32(newTexturePtr, entry->mac_addr);
		return kQANoErr;
	} else {
		if (rave_orig_texture_new == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, flags, pixelType, images, newTexturePtr };
		return RaveChainToOriginal(kRaveHookIdx_TextureNew, 5, args);
	}
}


/*
 *  Hook handler: QABitmapNew(engine, flags, pixelType, &image, &newBitmap)
 *
 *  r3 = engine, r4 = flags, r5 = pixelType, r6 = &image, r7 = &newBitmap
 */
uint32 NativeHookBitmapNew(uint32 engine, uint32 flags, uint32 pixelType,
                           uint32 image, uint32 newBitmapPtr)
{
	if (engine == rave_sentinel_engine) {
		uint32_t handle = RaveResourceAlloc(kRaveResourceBitmap);
		if (handle == 0) {
			RAVE_LOG("HOOK: QABitmapNew(sentinel) FAILED - resource table full");
			WriteMacInt32(newBitmapPtr, 0);
			return (uint32)(int32)kQANotSupported;
		}
		RaveResourceEntry *entry = RaveResourceGet(handle);
		RaveCreateBitmapFromImage(pixelType, image, entry);
		// 4-bit priority at [31:28] per QACalculatePriorityBits. Test: RAVEABITests.testPriorityBits_extraction_matchesSpec
		entry->priority = (uint8_t)((flags >> 28) & 0xF);
		RAVE_LOG("HOOK: QABitmapNew(sentinel) flags=0x%x pixelType=%d %dx%d -> handle %d addr=0x%08x metal=%p",
		       flags, pixelType, entry->width, entry->height, handle, entry->mac_addr, entry->metal_texture);
		WriteMacInt32(newBitmapPtr, entry->mac_addr);
		return kQANoErr;
	} else {
		if (rave_orig_bitmap_new == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, flags, pixelType, image, newBitmapPtr };
		return RaveChainToOriginal(kRaveHookIdx_BitmapNew, 5, args);
	}
}


/*
 *  Hook handler: QAColorTableNew(engine, tableType, pixelData, transparentFlag, &newTable)
 *
 *  r3 = engine, r4 = tableType, r5 = pixelData, r6 = transparentFlag, r7 = &newTable
 */
uint32 NativeHookColorTableNew(uint32 engine, uint32 tableType, uint32 pixelData,
                               uint32 transparentFlag, uint32 newTablePtr)
{
	if (engine == rave_sentinel_engine) {
		uint32_t handle = RaveResourceAlloc(kRaveResourceColorTable);
		if (handle == 0) {
			RAVE_LOG("HOOK: QAColorTableNew(sentinel) FAILED - resource table full");
			WriteMacInt32(newTablePtr, 0);
			return (uint32)(int32)kQANotSupported;
		}
		RaveResourceEntry *entry = RaveResourceGet(handle);
		RaveCreateColorTableData(tableType, pixelData, (int32_t)transparentFlag, entry);
		RAVE_LOG("HOOK: QAColorTableNew(sentinel) tableType=%d count=%d -> handle %d addr=0x%08x",
		       tableType, entry->clut_count, handle, entry->mac_addr);
		WriteMacInt32(newTablePtr, entry->mac_addr);
		return kQANoErr;
	} else {
		if (rave_orig_color_table_new == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, tableType, pixelData, transparentFlag, newTablePtr };
		return RaveChainToOriginal(kRaveHookIdx_ColorTableNew, 5, args);
	}
}


/*
 *  Hook handler: QADrawContextDelete(drawContextPtr)
 *
 *  If the drawPrivate handle stored at offset 0 of drawContextPtr
 *  maps to one of our contexts, call NativeDrawPrivateDelete.
 *  Otherwise chain to original QADrawContextDelete.
 *
 *  r3 = drawContextPtr (TQADrawContext* Mac address)
 */
uint32 NativeHookDrawContextDelete(uint32 drawContextPtr) {
	uint32 handle = ReadMacInt32(drawContextPtr + 0);
	RaveDrawPrivate *ctx = RaveGetContext(handle);
	if (ctx) {
		RAVE_LOG("HOOK: QADrawContextDelete(0x%08x) handle=%d -> native delete",
		       drawContextPtr, handle);
		NativeDrawPrivateDelete(handle);

		// Invalidate the Mac-side handle so stale references via this
		// address see handle=0 and fail gracefully in RaveGetContext(),
		// rather than accidentally hitting a recycled handle slot.
		WriteMacInt32(drawContextPtr + 0, 0);

		// Recycle the Mac-side context address so the next QADrawContextNew
		// can reuse it instead of permanently allocating from SheepMem.
		rave_ctx_free_list.push_back(drawContextPtr);

		// Clear the compositor's cached overlay on
		// QADrawContextDestroy.  The cache holds a strong reference to the
		// source MTLTexture that may outlive this engine's draw context;
		// Nanosaur destroys and recreates its QADrawContext at scene
		// transitions (menu <-> level 1 <-> level 2), and when a new
		// context is created the overlay's shape / size / sampler state may
		// differ.  Without this invalidation surface the prior scene's
		// overlay content can bleed through the first frames of the new
		// scene ("intermittent 3D stale pixel residue").
		//
		// Call sites for ClearCachedOverlay:
		//   1. MetalCompositor_OnModeExit  (DMC mode change; metal_compositor.mm:198)
		//   2. Compositor shutdown         (metal_compositor.mm tear-down)
		//   3. QADrawContextDelete         (this call)
		//
		// Regression test: NanosaurSceneTransitionTests.
		MetalCompositorSubmitFrame_ClearCachedOverlay();

		return kQANoErr;
	}
	// Not our context, chain to original
	if (rave_orig_draw_context_delete == 0) return kQANoErr;
	RAVE_LOG("HOOK: QADrawContextDelete(0x%08x) -> chaining to original", drawContextPtr);
	const uint32 args[] = { drawContextPtr };
	return RaveChainToOriginal(kRaveHookIdx_DrawContextDelete, 1, args);
}


/*
 *  Hook handler: QATextureDelete(engine, texturePtr)
 *
 *  Look up by Mac address in resource table. If found, free the slot.
 *  Otherwise chain to original.
 *
 *  r3 = engine (TQAEngine* Mac address)
 *  r4 = texture handle (TQATexture* Mac address)
 */
uint32 NativeHookTextureDelete(uint32 enginePtr, uint32 texturePtr) {
	uint32_t resHandle = RaveResourceFindByAddr(texturePtr);
	if (resHandle != 0) {
		RAVE_LOG("HOOK: QATextureDelete(engine=0x%08x, tex=0x%08x) -> resource handle %d freed",
		       enginePtr, texturePtr, resHandle);
		RaveForgetRTTAndFreeResource(resHandle);
		return kQANoErr;
	}
	// Not ours, chain to original
	if (rave_orig_texture_delete == 0) return kQANoErr;
	RAVE_LOG("HOOK: QATextureDelete(engine=0x%08x, tex=0x%08x) -> chaining to original",
	       enginePtr, texturePtr);
	const uint32 args[] = { enginePtr, texturePtr };
	return RaveChainToOriginal(kRaveHookIdx_TextureDelete, 2, args);
}


/*
 *  Hook handler: QABitmapDelete(engine, bitmapPtr)
 *
 *  r3 = engine (TQAEngine* Mac address)
 *  r4 = bitmap handle (TQABitmap* Mac address)
 */
uint32 NativeHookBitmapDelete(uint32 enginePtr, uint32 bitmapPtr) {
	uint32_t resHandle = RaveResourceFindByAddr(bitmapPtr);
	if (resHandle != 0) {
		RAVE_LOG("HOOK: QABitmapDelete(engine=0x%08x, bmp=0x%08x) -> resource handle %d freed",
		       enginePtr, bitmapPtr, resHandle);
		RaveForgetRTTAndFreeResource(resHandle);
		return kQANoErr;
	}
	if (rave_orig_bitmap_delete == 0) return kQANoErr;
	RAVE_LOG("HOOK: QABitmapDelete(engine=0x%08x, bmp=0x%08x) -> chaining to original",
	       enginePtr, bitmapPtr);
	const uint32 args[] = { enginePtr, bitmapPtr };
	return RaveChainToOriginal(kRaveHookIdx_BitmapDelete, 2, args);
}


/*
 *  Hook handler: QAColorTableDelete(engine, colorTablePtr)
 *
 *  r3 = engine (TQAEngine* Mac address)
 *  r4 = color table handle (TQAColorTable* Mac address)
 */
uint32 NativeHookColorTableDelete(uint32 enginePtr, uint32 colorTablePtr) {
	uint32_t resHandle = RaveResourceFindByAddr(colorTablePtr);
	if (resHandle != 0) {
		RAVE_LOG("HOOK: QAColorTableDelete(engine=0x%08x, ct=0x%08x) -> resource handle %d freed",
		       enginePtr, colorTablePtr, resHandle);
		RaveResourceFree(resHandle);
		return kQANoErr;
	}
	if (rave_orig_color_table_delete == 0) return kQANoErr;
	RAVE_LOG("HOOK: QAColorTableDelete(engine=0x%08x, ct=0x%08x) -> chaining to original",
	       enginePtr, colorTablePtr);
	const uint32 args[] = { enginePtr, colorTablePtr };
	return RaveChainToOriginal(kRaveHookIdx_ColorTableDelete, 2, args);
}


/*
 *  Hook handler: QATextureBindColorTable(engine, texture, colorTable)
 *
 *  r3 = engine, r4 = texture, r5 = colorTable
 */
uint32 NativeHookTextureBindColorTable(uint32 engine, uint32 texture, uint32 colorTable)
{
	if (engine == rave_sentinel_engine) {
		RAVE_LOG("HOOK: QATextureBindColorTable(sentinel, tex=0x%08x, ct=0x%08x)", texture, colorTable);
		return (uint32)NativeEngineTextureBindColorTable(texture, colorTable);
	} else {
		if (rave_orig_texture_bind_color_table == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, texture, colorTable };
		return RaveChainToOriginal(kRaveHookIdx_TextureBindColorTable, 3, args);
	}
}


/*
 *  Hook handler: QABitmapBindColorTable(engine, bitmap, colorTable)
 *
 *  r3 = engine, r4 = bitmap, r5 = colorTable
 */
uint32 NativeHookBitmapBindColorTable(uint32 engine, uint32 bitmap, uint32 colorTable)
{
	if (engine == rave_sentinel_engine) {
		RAVE_LOG("HOOK: QABitmapBindColorTable(sentinel, bmp=0x%08x, ct=0x%08x)", bitmap, colorTable);
		return (uint32)NativeEngineBitmapBindColorTable(bitmap, colorTable);
	} else {
		if (rave_orig_bitmap_bind_color_table == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, bitmap, colorTable };
		return RaveChainToOriginal(kRaveHookIdx_BitmapBindColorTable, 3, args);
	}
}


/*
 *  Hook handler: QATextureDetach(engine, texture)
 *
 *  RAVE spec: TextureDetach signals that the engine should copy the pixel data
 *  from the app's buffer. The app may have written final pixel data between
 *  TextureNew and TextureDetach.
 *
 *  r3 = engine, r4 = texture
 */
uint32 NativeHookTextureDetach(uint32 engine, uint32 texture)
{
	if (engine == rave_sentinel_engine) {
		RAVE_LOG("HOOK: QATextureDetach(sentinel, tex=0x%08x)", texture);
		return (uint32)NativeEngineTextureDetach(texture);
	} else {
		if (rave_orig_texture_detach == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, texture };
		return RaveChainToOriginal(kRaveHookIdx_TextureDetach, 2, args);
	}
}


/*
 *  Hook handler: QABitmapDetach(engine, bitmap)
 *
 *  RAVE spec: BitmapDetach signals that the engine should copy the pixel data.
 *
 *  r3 = engine, r4 = bitmap
 */
uint32 NativeHookBitmapDetach(uint32 engine, uint32 bitmap)
{
	if (engine == rave_sentinel_engine) {
		RAVE_LOG("HOOK: QABitmapDetach(sentinel, bmp=0x%08x)", bitmap);
		return (uint32)NativeEngineBitmapDetach(bitmap);
	} else {
		if (rave_orig_bitmap_detach == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, bitmap };
		return RaveChainToOriginal(kRaveHookIdx_BitmapDetach, 2, args);
	}
}


/*
 *  Hook handler: QAAccessTexture(engine, texture, mipmapLevel, flags, buffer)
 *
 *  r3 = engine, r4 = texture, r5 = mipmapLevel, r6 = flags, r7 = buffer
 */
uint32 NativeHookAccessTexture(uint32 engine, uint32 texture, uint32 mipmapLevel,
                                uint32 flags, uint32 buffer)
{
	if (engine == rave_sentinel_engine) {
		RAVE_LOG("HOOK: QAAccessTexture(sentinel, tex=0x%08x, mip=%d, flags=0x%x)",
		       texture, mipmapLevel, flags);
		return (uint32)NativeEngineAccessTexture(texture, mipmapLevel, flags, buffer);
	} else {
		if (rave_orig_access_texture == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, texture, mipmapLevel, flags, buffer };
		return RaveChainToOriginal(kRaveHookIdx_AccessTexture, 5, args);
	}
}


/*
 *  Hook handler: QAAccessTextureEnd(engine, texture, dirtyRect)
 *
 *  r3 = engine, r4 = texture, r5 = dirtyRect
 */
uint32 NativeHookAccessTextureEnd(uint32 engine, uint32 texture, uint32 dirtyRect)
{
	if (engine == rave_sentinel_engine) {
		RAVE_LOG("HOOK: QAAccessTextureEnd(sentinel, tex=0x%08x)", texture);
		return (uint32)NativeEngineAccessTextureEnd(texture, dirtyRect);
	} else {
		if (rave_orig_access_texture_end == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, texture, dirtyRect };
		return RaveChainToOriginal(kRaveHookIdx_AccessTextureEnd, 3, args);
	}
}


/*
 *  Hook handler: QAAccessBitmap(engine, bitmap, flags, buffer)
 *
 *  r3 = engine, r4 = bitmap, r5 = flags, r6 = buffer
 */
uint32 NativeHookAccessBitmap(uint32 engine, uint32 bitmap, uint32 flags, uint32 buffer)
{
	if (engine == rave_sentinel_engine) {
		RAVE_LOG("HOOK: QAAccessBitmap(sentinel, bmp=0x%08x, flags=0x%x)", bitmap, flags);
		return (uint32)NativeEngineAccessBitmap(bitmap, flags, buffer);
	} else {
		if (rave_orig_access_bitmap == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, bitmap, flags, buffer };
		return RaveChainToOriginal(kRaveHookIdx_AccessBitmap, 4, args);
	}
}


/*
 *  Hook handler: QAAccessBitmapEnd(engine, bitmap, dirtyRect)
 *
 *  r3 = engine, r4 = bitmap, r5 = dirtyRect
 */
uint32 NativeHookAccessBitmapEnd(uint32 engine, uint32 bitmap, uint32 dirtyRect)
{
	if (engine == rave_sentinel_engine) {
		RAVE_LOG("HOOK: QAAccessBitmapEnd(sentinel, bmp=0x%08x)", bitmap);
		return (uint32)NativeEngineAccessBitmapEnd(bitmap, dirtyRect);
	} else {
		if (rave_orig_access_bitmap_end == 0) return (uint32)(int32)kQANotSupported;
		const uint32 args[] = { engine, bitmap, dirtyRect };
		return RaveChainToOriginal(kRaveHookIdx_AccessBitmapEnd, 3, args);
	}
}


/*
 *  Allocate a PPC Gestalt callback that returns a fixed value.
 *
 *  Creates a TVECT in SheepMem with PPC code implementing:
 *    pascal OSErr SelectorFunction(OSType selector, long *response)
 *  that writes `value` to *response and returns noErr.
 */
static uint32 AllocateGestaltCallback(uint32 value)
{
	// 28 bytes: 8-byte TVECT header + 5 PPC instructions (20 bytes)
	uint32 base = SheepMem::ReserveProc(28);
	uint32 code = base + 8;

	// TVECT header
	WriteMacInt32(base, code);      // code_ptr
	WriteMacInt32(base + 4, 0);     // TOC (unused)

	// PPC ABI: r3 = selector, r4 = response pointer
	const uint32 r3 = 3, r4 = 4, r5 = 5;

	// lis r5, value_hi
	WriteMacInt32(code + 0, 0x3C000000 | (r5 << 21) | ((value >> 16) & 0xFFFF));
	// ori r5, r5, value_lo
	WriteMacInt32(code + 4, 0x60000000 | (r5 << 21) | (r5 << 16) | (value & 0xFFFF));
	// stw r5, 0(r4) -- *response = value
	WriteMacInt32(code + 8, 0x90000000 | (r5 << 21) | (r4 << 16));
	// li r3, 0 -- return noErr
	WriteMacInt32(code + 12, 0x38000000 | (r3 << 21));
	// blr
	WriteMacInt32(code + 16, 0x4E800020);

	return base;
}


/*
 *  RaveRegisterEngine - Register our RAVE engine with the RAVE manager
 *
 *  Called from VideoInstallAccel() after the video driver is set up.
 *  Uses FindLibSymbol to locate QARegisterEngine in DrawSprktLib,
 *  then calls it with our EngineGetMethod TVECT.
 *
 *  Handles failure gracefully: if QARegisterEngine is not found
 *  (library not loaded or not present), logs and returns silently.
 */
// Registration state (file-scope so RaveIsRegistered can access it)
static bool rave_registered = false;
static bool rave_reg_in_progress = false;
static int rave_reg_attempts = 0;
static const int RAVE_REG_MAX_ATTEMPTS = 3;

bool RaveIsRegistered(void)
{
	return rave_registered;
}

/* ----------------------------------------------------------------------
 * gfxaccel_resources fan-out handlers.
 *
 * RAVE registers a pair of attach/detach callbacks with gfxaccel_resources
 * at engine init (RaveRegisterEngine, below). The resource manager fans
 * out DMC on_mode_enter/exit events to engines via these callbacks.
 *
 * Lifecycle:
 *   - on_mode_enter (LIFO) -> RaveOnAttach: pre-vend overlay texture pair for
 *     the new mode's resolution IF RAVE has an active or preserved logical
 *     overlay binding. Otherwise skip; the next RaveCreateMetalOverlay call
 *     will vend lazily.
 *   - on_mode_exit  (FIFO) -> RaveOnDetach: release the cached overlay
 *     handle back to the resource manager so RAVE doesn't pin a stale
 *     allocation across the mode switch.
 *
 * The handlers must NOT call back into DMC (the resource manager fan-out
 * runs on the DMC writer's thread while holding the writer mutex —
 * recursive subscribe/unsubscribe would deadlock; threat T-03-26 / T-03-27).
 *
 * These cross the .cpp/.mm boundary via the small extern "C" probes
 * declared in rave_metal_renderer.h (rave_has_active_overlay,
 * rave_get_overlay_dims, rave_release_overlay_for_detach).
 * ---------------------------------------------------------------------- */
static int32_t RaveOnAttach(uint32_t /* engine_id */,
                            const struct DMCModeSnapshot *incoming,
                            void * /* ctx */)
{
	/* If RAVE has no active or preserved logical overlay binding at attach
	 * time, skip pre-vending — the next RaveCreateMetalOverlay (driven by
	 * an actual RAVE context creation) will vend lazily. This is the common
	 * case for non-RAVE workloads (e.g. pure-2D apps switching modes). */
	if (!rave_has_active_overlay()) {
		return 0;  /* kGfxAccelResNoErr — accept the transition */
	}
	if (incoming == NULL) {
		return 0;  /* defensive — accept transition with no pre-vend */
	}

	/* RAVE was active in the outgoing mode; pre-vend the pair at the incoming
	 * mode's resolution so the compositor sees an overlay on its first
	 * present after the mode switch. Vend
	 * format is BGRA8Unorm (= MTLPixelFormatBGRA8Unorm = 80). */
	void *tex0 = gfxaccel_resources_vend_overlay_texture_indexed(
	                kGfxEngineRAVE,
	                0,
	                incoming->width,
	                incoming->height,
	                80 /* MTLPixelFormatBGRA8Unorm */);
	void *tex1 = gfxaccel_resources_vend_overlay_texture_indexed(
	                kGfxEngineRAVE,
	                1,
	                incoming->width,
	                incoming->height,
	                80 /* MTLPixelFormatBGRA8Unorm */);
	if (tex0 == NULL || tex1 == NULL) {
		if (tex0 != NULL) gfxaccel_resources_release_overlay_texture(kGfxEngineRAVE, tex0);
		if (tex1 != NULL) gfxaccel_resources_release_overlay_texture(kGfxEngineRAVE, tex1);
		/* Vend failed — reject the transition. The rollback path is
		 * safe under concurrent DMC readers. */
		return kDMCErrSubscriberRejected;
	}
	return 0;
}

static int32_t RaveOnDetach(uint32_t /* engine_id */,
                            const struct DMCModeSnapshot * /* outgoing */,
                            void * /* ctx */)
{
	/* Release the cached overlay (idempotent — no-op if RAVE has none).
	 * The next RaveCreateMetalOverlay after the mode switch will re-vend
	 * at the appropriate resolution. */
	rave_release_overlay_for_detach();
	return 0;
}

static bool rave_resource_handlers_registered = false;

static void RaveRegisterResourceHandlers(void)
{
	if (rave_resource_handlers_registered) return;
	struct GfxResEngineHandlers rave_handlers;
	rave_handlers.attach = RaveOnAttach;
	rave_handlers.detach = RaveOnDetach;
	rave_handlers.ctx    = NULL;
	gfxaccel_resources_register_engine(kGfxEngineRAVE, &rave_handlers);
	rave_resource_handlers_registered = true;
}

void RaveRegisterEngine(void)
{
	// Guard against double registration AND re-entrancy.
	// Two separate guards:
	//   - rave_registered: set AFTER successful completion, prevents redundant calls
	//   - rave_reg_in_progress: set DURING execution, prevents re-entrancy from
	//     FindLibSymbol -> CFM loading -> VideoInstallAccel -> RaveRegisterEngine
	//
	// Previously, a single flag was set BEFORE the work began, which meant if
	// FindLibSymbol failed (e.g. Disk First Aid delays library loading),
	// registration could never retry. Now sony.cpp accRun keeps its periodic
	// action active until registration succeeds, so PatchAfterStartup (and
	// hence RaveRegisterEngine) is called again on subsequent ticks.
	if (rave_registered) {
		return;
	}
	if (rave_reg_attempts >= RAVE_REG_MAX_ATTEMPTS) {
		return;
	}
	if (rave_reg_in_progress) {
		RAVE_LOG("RaveRegisterEngine() skipped (re-entrant call)");
		return;
	}
	rave_reg_in_progress = true;

	RAVE_LOG("RaveRegisterEngine() called");

	// ---- Step 0: Register RAVE's attach/detach handlers
	// with the gfxaccel_resources fan-out registry.
	// Idempotent (rave_resource_handlers_registered guard); safe to call
	// before the QARegisterEngine attempt because gfxaccel_resources_init
	// has already run (main_unix.cpp ordering: SharedMetalDevice ->
	// MetalCompositorInit -> gfxaccel_resources_init -> ROM patches ->
	// VideoInstallAccel -> RaveRegisterEngine).
	RaveRegisterResourceHandlers();

	// ---- Step 1: Cache ALL library symbols BEFORE any CallMacOS ----
	//
	// IMPORTANT: FindLibSymbol can trigger CFM fragment loading, which in turn
	// calls VideoInstallAccel -> RaveRegisterEngine (re-entrancy). Our guard
	// catches the recursive call, but the FindLibSymbol/CFM code path corrupts
	// the caller's execution state (CallMacOS stack frame or return address).
	//
	// Fix: Do ALL FindLibSymbol lookups first, cache results, then use only
	// the cached TVECTs for all subsequent CallMacOS calls. No FindLibSymbol
	// calls after the first CallMacOS.

	// Locate QARegisterEngine in the RAVE manager library
	// Pascal string format: first byte = length
	// "QARegisterEngine" = 16 chars -> \020
	//
	// The CFM fragment name is "QuickDraw™ 3D Accelerator" (25 chars).
	// This was determined by examining PEF import tables of the DDK's
	// Empty Engine sample (which imports QARegisterEngine from this fragment)
	// and the RaveEngineInfo sample app. The file on disk is named
	// "QuickDraw™ 3D RAVE" but the CFM fragment name is different.
	// ™ = MacRoman 0xAA
	static const char *rave_lib_names[] = {
		"\031QuickDraw\xAA 3D Accelerator",  // 25 chars: correct CFM fragment name
		"\022QuickDraw\xAA 3D RAVE",          // 18 chars: file name (not fragment name)
		"\022QuickDraw3DRAVELib",              // 18 chars: stub library name
		NULL
	};

	uint32 qa_register = 0;
	const char *found_rave_lib = NULL;
	for (int i = 0; rave_lib_names[i] != NULL; i++) {
		RAVE_LOG("  trying library '%s' (len %d)",
			   rave_lib_names[i] + 1, (unsigned char)rave_lib_names[i][0]);
		qa_register = FindLibSymbol(rave_lib_names[i], "\020QARegisterEngine");
		if (qa_register != 0) {
			found_rave_lib = rave_lib_names[i];
			RAVE_LOG("QARegisterEngine found via '%s' at TVECT 0x%08x",
					 rave_lib_names[i] + 1, qa_register);
			break;
		}
		RAVE_LOG("  -> not found");
	}

	if (qa_register == 0) {
		rave_reg_in_progress = false;
		rave_reg_attempts++;
		if (rave_reg_attempts >= RAVE_REG_MAX_ATTEMPTS)
			RAVE_LOG("QARegisterEngine not found after %d attempts, giving up", rave_reg_attempts);
		else
			RAVE_LOG("QARegisterEngine not found, skipping 3D acceleration (attempt %d/%d, will retry)",
			         rave_reg_attempts, RAVE_REG_MAX_ATTEMPTS);
		return;
	}

	// Cache InterfaceLib NewGestalt for post-registration Gestalt selector setup
	uint32 new_gestalt_tvect = FindLibSymbol("\014InterfaceLib", "\012NewGestalt");
	RAVE_LOG("cached InterfaceLib: NewGestalt=0x%08x", new_gestalt_tvect);

	// ---- Step 2: Registration (CallMacOS calls, no more FindLibSymbol) ----

	// Get the EngineGetMethod TVECT address to pass to QARegisterEngine
	// Sub-opcode 100 (kRaveEngineDrawPrivateNew) is our EngineGetMethod callback
	uint32 engine_get_method_tvect = rave_method_tvects[kRaveEngineDrawPrivateNew];

	if (engine_get_method_tvect == 0) {
		RAVE_LOG("EngineGetMethod TVECT not allocated, skipping registration");
		rave_reg_in_progress = false;
		return;
	}

	RAVE_LOG("Registering engine with EngineGetMethod TVECT 0x%08x", engine_get_method_tvect);

	// Call QARegisterEngine(engineGetMethod)
	// QARegisterEngine takes a TQAEngineGetMethod function pointer
	// and returns TQAError (0 = success)
	typedef int32 (*qa_register_t)(uint32);
	int32 err = (int32)CallMacOS1(qa_register_t, qa_register, engine_get_method_tvect);

	if (err != kQANoErr) {
		RAVE_LOG("engine registration failed with error %d", err);
		rave_reg_in_progress = false;
		return;
	}

	RAVE_LOG("engine registered successfully");

	// ---- Step 3: Gestalt registration (uses cached NewGestalt TVECT) ----

	if (new_gestalt_tvect) {
		typedef int16 (*new_gestalt_t)(uint32, uint32);

		// Register 'rave' (gestaltRaveVersion) with value 0x00010600 (RAVE 1.6)
		{
			uint32 callback = AllocateGestaltCallback(0x00010600);
			int16 gerr = (int16)CallMacOS2(new_gestalt_t, new_gestalt_tvect,
				0x72617665, callback);
			RAVE_LOG("NewGestalt('rave', 0x00010600) -> %d", gerr);
		}

		// Register 'qd3x' (QD3D hardware extensions) with value 0x00000001
		{
			uint32 callback = AllocateGestaltCallback(0x00000001);
			int16 gerr = (int16)CallMacOS2(new_gestalt_t, new_gestalt_tvect,
				0x71643378, callback);
			RAVE_LOG("NewGestalt('qd3x', 0x00000001) -> %d", gerr);
		}

		// Register 'gls ' (OpenGL version) with value 0x0120 (OpenGL 1.2)
		{
			uint32 callback = AllocateGestaltCallback(0x0120);
			int16 gerr = (int16)CallMacOS2(new_gestalt_t, new_gestalt_tvect,
				0x676c7320, callback);
			RAVE_LOG("NewGestalt('gls ', 0x0120) -> %d", gerr);
		}
	} else {
		RAVE_LOG("NewGestalt not found, skipping Gestalt registration");
	}

	// ---- Step 4: Install enumeration hooks ----
	//
	// The RAVE manager's QARegisterEngine requires an internal loading context
	// (ZgLoadingSharedLibrary) that we can't satisfy from VideoInstallAccel.
	// So registration "succeeds" but the engine is never added to the internal list.
	//
	// Fix: Hook the 5 enumeration/creation APIs to inject our engine into results.
	// The hooks check for our sentinel TQAEngine handle and dispatch to our native
	// handlers; for other engines, they chain to the original implementations.
	RaveInstallHooks();

	// Mark permanently registered only after all registration steps succeed.
	rave_registered = true;
	rave_reg_in_progress = false;

	RAVE_LOG("init complete -- waiting for QD3D IR calls");
}


/*
 *  RaveInstallHooks - Hook RAVE enumeration APIs to inject our engine
 *
 *  Finds the original TVECTs for QADeviceGetFirstEngine, QADeviceGetNextEngine,
 *  QAEngineGestalt, QAEngineCheckDevice, and QADrawContextNew via FindLibSymbol.
 *  Saves the original code pointers for chaining, then overwrites the TVECT
 *  code pointers to point to our hook thunks.
 *
 *  The hook thunks dispatch to native via NATIVE_RAVE_DISPATCH with hook
 *  sub-opcodes (200-204). The native handlers check if the engine argument
 *  is our sentinel handle and either handle it directly or chain to the
 *  original function via CallMacOS.
 */
void RaveInstallHooks(void)
{
	RAVE_LOG("installing enumeration hooks");

	// Find the RAVE library fragment name we already determined works.
	// Try all known library names used during registration.
	static const char *rave_lib_names[] = {
		"\031QuickDraw\xAA 3D Accelerator",  // 25 chars: CFM fragment name
		"\022QuickDraw\xAA 3D RAVE",          // 18 chars: file name
		"\022QuickDraw3DRAVELib",              // 18 chars: stub library name
		NULL
	};

	// Look up all APIs. We need them all from the same library.
	// The first 5 are enumeration/creation hooks, the last 3 are resource
	// creation hooks needed because our sentinel engine is not in the RAVE
	// manager's internal list (QARegisterEngine succeeds but ZgLoadingSharedLibrary
	// context is missing, so the engine is never actually added).
	struct {
		const char *sym;   // Pascal string
		uint32 *orig;      // Where to save original TVECT
		int hook_id;       // Hook sub-opcode
		const char *name;  // For logging
	} apis[] = {
		{ "\026QADeviceGetFirstEngine", &rave_orig_get_first_engine,  kRaveHookGetFirstEngine,    "QADeviceGetFirstEngine" },
		{ "\025QADeviceGetNextEngine",  &rave_orig_get_next_engine,   kRaveHookGetNextEngine,     "QADeviceGetNextEngine" },
		{ "\017QAEngineGestalt",        &rave_orig_engine_gestalt,    kRaveHookEngineGestalt,     "QAEngineGestalt" },
		{ "\023QAEngineCheckDevice",    &rave_orig_engine_check_device, kRaveHookEngineCheckDevice, "QAEngineCheckDevice" },
		{ "\020QADrawContextNew",       &rave_orig_draw_context_new,  kRaveHookDrawContextNew,    "QADrawContextNew" },
		{ "\014QATextureNew",           &rave_orig_texture_new,       kRaveHookTextureNew,        "QATextureNew" },
		{ "\013QABitmapNew",            &rave_orig_bitmap_new,        kRaveHookBitmapNew,         "QABitmapNew" },
		{ "\017QAColorTableNew",        &rave_orig_color_table_new,   kRaveHookColorTableNew,     "QAColorTableNew" },
		{ "\023QADrawContextDelete",  &rave_orig_draw_context_delete, kRaveHookDrawContextDelete, "QADrawContextDelete" },
		{ "\017QATextureDelete",      &rave_orig_texture_delete,      kRaveHookTextureDelete,     "QATextureDelete" },
		{ "\016QABitmapDelete",       &rave_orig_bitmap_delete,       kRaveHookBitmapDelete,      "QABitmapDelete" },
		{ "\022QAColorTableDelete",   &rave_orig_color_table_delete,  kRaveHookColorTableDelete,  "QAColorTableDelete" },
		{ "\027QATextureBindColorTable", &rave_orig_texture_bind_color_table, kRaveHookTextureBindColorTable, "QATextureBindColorTable" },
		{ "\026QABitmapBindColorTable",  &rave_orig_bitmap_bind_color_table,  kRaveHookBitmapBindColorTable,  "QABitmapBindColorTable" },
		{ "\017QATextureDetach",        &rave_orig_texture_detach,           kRaveHookTextureDetach,         "QATextureDetach" },
		{ "\016QABitmapDetach",         &rave_orig_bitmap_detach,            kRaveHookBitmapDetach,          "QABitmapDetach" },
		{ "\017QAAccessTexture",        &rave_orig_access_texture,           kRaveHookAccessTexture,         "QAAccessTexture" },
		{ "\022QAAccessTextureEnd",     &rave_orig_access_texture_end,       kRaveHookAccessTextureEnd,      "QAAccessTextureEnd" },
		{ "\016QAAccessBitmap",         &rave_orig_access_bitmap,            kRaveHookAccessBitmap,          "QAAccessBitmap" },
		{ "\021QAAccessBitmapEnd",      &rave_orig_access_bitmap_end,        kRaveHookAccessBitmapEnd,       "QAAccessBitmapEnd" },
		{ "\016QAEngineEnable",         &rave_orig_engine_enable,            kRaveHookEngineEnable,          "QAEngineEnable" },
		{ "\017QAEngineDisable",        &rave_orig_engine_disable,           kRaveHookEngineDisable,         "QAEngineDisable" },
	};
	const int num_apis = 22;

	// Try each library name
	bool all_found = false;
	for (int lib = 0; rave_lib_names[lib] != NULL; lib++) {
		RAVE_LOG("  trying library '%s' for hooks", rave_lib_names[lib] + 1);

		bool found_all = true;
		for (int i = 0; i < num_apis; i++) {
			uint32 tvect = FindLibSymbol(rave_lib_names[lib], apis[i].sym);
			if (tvect == 0) {
				RAVE_LOG("    %s not found", apis[i].name);
				found_all = false;
				break;
			}
			*apis[i].orig = tvect;
			RAVE_LOG("    %s TVECT at 0x%08x", apis[i].name, tvect);
		}

		if (found_all) {
			all_found = true;
			break;
		}
	}

	if (!all_found) {
		RAVE_LOG("FAILED to find all enumeration APIs, hooks NOT installed");
		return;
	}

	// Code-level hooking: overwrite the first 4 PPC instructions at each
	// function's entry point with a branch to our hook thunk. This intercepts
	// ALL callers regardless of how they resolved the import (per-importer
	// TVECT copies, direct code pointers, etc.), because all paths lead to
	// the same code address.
	//
	// For each API:
	//   1. Read orig_code from the TVECT (actual PPC code address)
	//   2. Save the first 4 instructions (16 bytes) from orig_code
	//   3. Allocate a trampoline in SheepMem: saved instructions + jump to orig_code+16
	//   4. Allocate a chain TVECT pointing to trampoline (for CallMacOS chaining)
	//   5. Overwrite orig_code with: lis r11,hi; ori r11,r11,lo; mtctr r11; bctr
	//      (branch to our hook thunk's code address)
	//   6. Flush JIT cache for the patched range
	//
	// PPC notes:
	//   - r11 is volatile (caller-saved), safe to clobber in the entry patch
	//   - mtctr+bctr does NOT modify LR, so the original caller's return
	//     address is preserved
	//
	// NOTE: Previous versions used a trampoline that relocated the saved
	// instructions and used lis/ori/mtctr/bctr to jump back. This clobbered
	// r11, which could have been set by one of the saved instructions and
	// needed by instruction 5+. The new approach uses unpatch-call-repatch
	// (see RaveChainToOriginal) which avoids instruction relocation entirely.

	const uint32 r11 = 11;

	for (int i = 0; i < num_apis; i++) {
		uint32 orig_tvect = *apis[i].orig;
		uint32 hook_tvect = rave_method_tvects[apis[i].hook_id];

		if (hook_tvect == 0) {
			RAVE_LOG("  hook TVECT for %s not allocated!", apis[i].name);
			continue;
		}

		// Read the original code pointer from the TVECT
		uint32 orig_code = ReadMacInt32(orig_tvect);

		// Read the hook thunk's code pointer (from hook TVECT)
		uint32 hook_code = ReadMacInt32(hook_tvect);

		// Step 1: Save the first 4 instructions (16 bytes) from the original code
		uint32 saved_instr[4];
		for (int j = 0; j < 4; j++) {
			saved_instr[j] = ReadMacInt32(orig_code + j * 4);
		}

		RAVE_LOG("  %s: orig_code=0x%08x, first 4 instrs: %08x %08x %08x %08x",
			   apis[i].name, orig_code,
			   saved_instr[0], saved_instr[1], saved_instr[2], saved_instr[3]);

		// Step 2: Build the hook patch instructions (4 instructions)
		uint32 hook_hi = (hook_code >> 16) & 0xFFFF;
		uint32 hook_lo = hook_code & 0xFFFF;

		uint32 hook_instr[4];
		// lis r11, hook_code_hi
		hook_instr[0] = 0x3C000000 | (r11 << 21) | hook_hi;
		// ori r11, r11, hook_code_lo
		hook_instr[1] = 0x60000000 | (r11 << 21) | (r11 << 16) | hook_lo;
		// mtctr r11
		hook_instr[2] = 0x7C0903A6 | (r11 << 21);
		// bctr
		hook_instr[3] = 0x4E800420;

		// Step 3: Store patch info for unpatch-call-repatch chaining
		rave_hook_patches[i].orig_tvect = orig_tvect;
		rave_hook_patches[i].orig_code  = orig_code;
		for (int j = 0; j < 4; j++) {
			rave_hook_patches[i].saved_instr[j] = saved_instr[j];
			rave_hook_patches[i].hook_instr[j]  = hook_instr[j];
		}
		rave_hook_patches[i].active = true;

		// Step 4: Apply the hook patch (overwrite first 4 instructions)
		for (int j = 0; j < 4; j++) {
			WriteMacInt32(orig_code + j * 4, hook_instr[j]);
		}

		// Step 5: Flush instruction cache for the patched code range
#if EMULATED_PPC
		FlushCodeCache(orig_code, orig_code + 16);
#endif

		// Verify the patch took effect
		uint32 verify = ReadMacInt32(orig_code);
		RAVE_LOG("  patched %s: orig_code 0x%08x now starts with 0x%08x (expect 0x%08x)",
			   apis[i].name, orig_code, verify, hook_instr[0]);
		RAVE_LOG("  patch info stored at hook_patches[%d], orig_tvect=0x%08x",
			   i, orig_tvect);

		// NOTE: rave_orig_* pointers are NOT overwritten -- they still point to the
		// original TVECTs. RaveChainToOriginal uses rave_hook_patches[].orig_tvect
		// for chaining, and temporarily restores the original code before calling.
	}

	RAVE_LOG("enumeration hooks installed, sentinel engine at 0x%08x",
		   rave_sentinel_engine);
}


/*
 *  ATI RaveExtFuncs: textureUpdate
 *
 *  Re-uploads texture contents for an already-allocated texture.
 *  Maps ATI-specific pixel types (1000-1003) to standard equivalents,
 *  then uses existing ConvertPixels + queued RaveUploadMipLevel infrastructure.
 */

// ATI-specific pixel type constants
enum {
	kQATIPixel_RGB4444  = 1000,
	kQATIPixel_ARGB4444 = 1001,
	kQATIPixel_YUV422   = 1002,
	kQATIPixel_ARGB8    = 1003
};

int32_t NativeATITextureUpdate(uint32_t flags, uint32_t pixelType, uint32_t imagesAddr, uint32_t textureAddr)
{
	uint32_t handle = RaveResourceFindByAddr(textureAddr);
	RaveResourceEntry *entry = RaveResourceGet(handle);
	if (!entry || entry->type != kRaveResourceTexture) {
		RAVE_LOG("ATITextureUpdate: texture not found at 0x%08x", textureAddr);
		return kQAError;
	}

	// Read TQAImage from Mac memory (same layout as NativeEngineTextureNew)
	uint32_t width      = ReadMacInt32(imagesAddr + 0);
	uint32_t height     = ReadMacInt32(imagesAddr + 4);
	uint32_t rowBytes   = ReadMacInt32(imagesAddr + 8);
	uint32_t pixelBuffer = ReadMacInt32(imagesAddr + 12);

	if (width == 0 || height == 0 || pixelBuffer == 0) {
		RAVE_LOG("ATITextureUpdate: invalid image params %dx%d buf=0x%08x", width, height, pixelBuffer);
		return kQAError;
	}

	// Map ATI-specific pixel types to standard equivalents
	uint32_t effectivePixelType = pixelType;
	switch (pixelType) {
		case kQATIPixel_ARGB8:
			effectivePixelType = kQAPixel_ARGB32;
			break;
		case kQATIPixel_RGB4444:
		case kQATIPixel_ARGB4444:
			effectivePixelType = kQAPixel_ARGB16_4444;
			break;
		case kQATIPixel_YUV422:
			RAVE_LOG("ATITextureUpdate: YUV422 pixel type not supported, skipping update");
			return kQANoErr;
		default:
			// Standard pixel type -- use as-is
			break;
	}

	// Convert pixels to BGRA32 using existing infrastructure
	uint32_t bgra_row_bytes = width * 4;
	uint8_t *bgra_data = new uint8_t[bgra_row_bytes * height];

	bool converted = ConvertPixels(effectivePixelType, pixelBuffer, bgra_data, width, height, rowBytes);
	if (!converted) {
		RAVE_LOG("ATITextureUpdate: unsupported pixel type %d (effective %d)", pixelType, effectivePixelType);
		delete[] bgra_data;
		return kQAError;
	}
	bool whitenedAlphaMask = false;
	if (effectivePixelType == kQAPixel_ARGB32) {
		whitenedAlphaMask = RaveBGRAWhitenAlphaOnlyMask(bgra_data, width * height);
	}
	RaveBGRAImageStats stats = RaveBGRAImageAnalyze(bgra_data, width * height);
	entry->diag_alpha_zero = (width * height) - stats.alpha;
	entry->diag_index_zero = 0;
	entry->diag_rgb_nonzero = stats.rgb;

	// Re-upload to existing Metal texture at level 0
	if (entry->metal_texture) {
		// Batch level 0 + regenerated chain into one pass break (D-R-4).
		RaveTextureUploadBatchBegin();
		RaveUploadMipLevel(entry->metal_texture, 0, width, height, bgra_data, bgra_row_bytes);

		// Generate mipmaps if texture has mip levels — CPU downsample from the fresh
		// level 0 (the Metal blit path left high-mips black -> distant surfaces black).
		if (entry->mip_levels > 1) {
			RaveUploadGeneratedMips(entry->metal_texture, bgra_data, width, height, entry->mip_levels);
		}
		RaveTextureUploadBatchEnd();
	} else {
		// DIAGNOSTIC: the texture is not yet realized, so this update is
		// silently lost; the texture later realizes from its (possibly stale
		// or black) pixmap. Prime suspect for "world geometry goes black" —
		// rgb>0 here means real pixels were dropped on the floor.
		RAVE_LOG("ATITextureUpdate: DROPPED update on unrealized texture 0x%08x (%dx%d rgb=%u/%u pixmap=0x%08x) — metal_texture==NULL",
		         textureAddr, width, height, stats.rgb, width * height, entry->pixmap_mac_addr);
	}

	delete[] bgra_data;

	RAVE_LOG("ATITextureUpdate: updated texture 0x%08x (%dx%d, pixelType=%d->%d, alphaMaskWhite=%d)",
	         textureAddr, width, height, pixelType, effectivePixelType, whitenedAlphaMask);
	return kQANoErr;
}


/*
 *  ATI RaveExtFuncs: bindCodeBook
 *
 *  Binds a VQ codebook to a texture. VQ (Vector Quantization) compression
 *  uses a 256-entry codebook of 2x2 ARGB pixel blocks. The compressed
 *  texture data consists of 8-bit indices into this codebook.
 *
 *  This is a minimal implementation that logs the codebook binding and
 *  returns success. Full VQ decompression is deferred until games are
 *  observed to actually use VQ textures (low confidence per research).
 */
int32_t NativeATIBindCodeBook(uint32_t textureAddr, uint32_t codebookPtr)
{
	// VQ codebook decompression is not implemented. Return kQANotSupported so
	// callers know VQ textures won't render correctly with this engine.
	RAVE_LOG("ATIBindCodeBook: texture 0x%08x codebook 0x%08x -> kQANotSupported (VQ not implemented)",
	         textureAddr, codebookPtr);
	return kQANotSupported;
}

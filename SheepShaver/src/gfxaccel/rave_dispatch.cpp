/*
 *  rave_dispatch.cpp - RAVE multiplexed dispatch from sub-opcode to method handlers
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  Reads the sub-opcode from the scratch word written by the PPC thunk
 *  and dispatches to the appropriate RAVE method handler. Engine methods
 *  (GetMethod, Gestalt, CheckDevice) dispatch to real handlers in
 *  rave_engine.cpp. Draw method stubs log and return kQANoErr.
 */

#include <cstring>

#include "sysdeps.h"
#if defined(_MSC_VER)
#ifndef __builtin_unreachable
#define __builtin_unreachable() __assume(0)
#endif
#endif
#include "cpu_emulation.h"
#include "rave_engine.h"
#include "rave_metal_renderer.h"
#include "gfx_log.h"

// RAVE error codes (must match TQAError enum in RAVE.h)
#define kQANoErr                    0
#define kQANotSupported             3

// Engine callback declarations (implemented in rave_engine.cpp)
// Return TQAError (int32), take explicit PPC register values as args
extern int32 NativeEngineGetMethod(uint32 methodTag, uint32 methodPtr);
extern int32 NativeEngineGestalt(uint32 selector, uint32 responsePtr);
extern int32 NativeEngineCheckDevice(uint32 devicePtr);

// Hook handler declarations (implemented in rave_engine.cpp)
extern uint32 NativeHookGetFirstEngine(uint32 device);
extern uint32 NativeHookGetNextEngine(uint32 device, uint32 prevEngine);
extern uint32 NativeHookEngineGestalt(uint32 engine, uint32 selector, uint32 responsePtr);
extern uint32 NativeHookEngineCheckDevice(uint32 engine, uint32 device);
extern uint32 NativeHookDrawContextNew(uint32 device, uint32 rect, uint32 clip,
										uint32 engine, uint32 flags, uint32 drawContextPtr);
extern uint32 NativeHookTextureNew(uint32 engine, uint32 flags, uint32 pixelType,
									uint32 images, uint32 newTexturePtr);
extern uint32 NativeHookBitmapNew(uint32 engine, uint32 flags, uint32 pixelType,
								   uint32 image, uint32 newBitmapPtr);
extern uint32 NativeHookColorTableNew(uint32 engine, uint32 tableType, uint32 pixelData,
									   uint32 transparentFlag, uint32 newTablePtr);
extern uint32 NativeHookDrawContextDelete(uint32 drawContextPtr);
extern uint32 NativeHookTextureDelete(uint32 enginePtr, uint32 texturePtr);
extern uint32 NativeHookBitmapDelete(uint32 enginePtr, uint32 bitmapPtr);
extern uint32 NativeHookColorTableDelete(uint32 enginePtr, uint32 colorTablePtr);
extern uint32 NativeHookTextureBindColorTable(uint32 engine, uint32 texture, uint32 colorTable);
extern uint32 NativeHookBitmapBindColorTable(uint32 engine, uint32 bitmap, uint32 colorTable);
extern uint32 NativeHookTextureDetach(uint32 engine, uint32 texture);
extern uint32 NativeHookBitmapDetach(uint32 engine, uint32 bitmap);
extern uint32 NativeHookAccessTexture(uint32 engine, uint32 texture, uint32 mipmapLevel,
									   uint32 flags, uint32 buffer);
extern uint32 NativeHookAccessTextureEnd(uint32 engine, uint32 texture, uint32 dirtyRect);
extern uint32 NativeHookAccessBitmap(uint32 engine, uint32 bitmap, uint32 flags, uint32 buffer);
extern uint32 NativeHookAccessBitmapEnd(uint32 engine, uint32 bitmap, uint32 dirtyRect);
extern uint32 NativeHookEngineEnable(uint32 vendorID, uint32 engineID);
extern uint32 NativeHookEngineDisable(uint32 vendorID, uint32 engineID);

// Engine method dispatch functions (implemented in rave_engine.cpp)
extern int32_t NativeEngineTextureNew(uint32_t flags, uint32_t pixelType, uint32_t imagesAddr, uint32_t newTexturePtr);
extern int32_t NativeEngineTextureDetach(uint32_t textureAddr);
extern void NativeEngineTextureDelete(uint32_t textureAddr);
extern int32_t NativeEngineBitmapNew(uint32_t flags, uint32_t pixelType, uint32_t imageAddr, uint32_t newBitmapPtr);
extern int32_t NativeEngineBitmapDetach(uint32_t bitmapAddr);
extern void NativeEngineBitmapDelete(uint32_t bitmapAddr);
extern int32_t NativeEngineColorTableNew(uint32_t tableType, uint32_t pixelDataAddr, uint32_t transparentIndexOrFlag, uint32_t newTablePtr);
extern void NativeEngineColorTableDelete(uint32_t colorTableAddr);
extern int32_t NativeEngineTextureBindColorTable(uint32_t textureAddr, uint32_t colorTableAddr);
extern int32_t NativeEngineBitmapBindColorTable(uint32_t bitmapAddr, uint32_t colorTableAddr);

// Logging state -- enabled by default for graphics diagnostics.
#if ACCEL_LOGGING_ENABLED
bool rave_logging_enabled = accel_log_subsystem_on("rave");

#ifdef __APPLE__
os_log_t rave_log = OS_LOG_DEFAULT;

// Initialize os_log on first use
static struct RaveLogInit {
	RaveLogInit() {
		rave_log = os_log_create("com.pocketshaver.rave", "engine");
	}
} rave_log_init;
#endif
#endif /* ACCEL_LOGGING_ENABLED */

// Draw method names (indexed by TQADrawMethodTag 0-34)
static const char *draw_method_names[] = {
	"SetFloat",               // 0
	"SetInt",                 // 1
	"SetPtr",                 // 2
	"GetFloat",               // 3
	"GetInt",                 // 4
	"GetPtr",                 // 5
	"DrawPoint",              // 6
	"DrawLine",               // 7
	"DrawTriGouraud",         // 8
	"DrawTriTexture",         // 9
	"DrawVGouraud",           // 10
	"DrawVTexture",           // 11
	"DrawBitmap",             // 12
	"RenderStart",            // 13
	"RenderEnd",              // 14
	"RenderAbort",            // 15
	"Flush",                  // 16
	"Sync",                   // 17
	"SubmitVerticesGouraud",  // 18
	"SubmitVerticesTexture",  // 19
	"DrawTriMeshGouraud",     // 20
	"DrawTriMeshTexture",     // 21
	"SetNoticeMethod",        // 22
	"GetNoticeMethod",        // 23
	"SubmitMultiTextureVertex", // 24
	"AccessDrawBuffer",       // 25
	"AccessDrawBufferEnd",    // 26
	"AccessZBuffer",          // 27
	"AccessZBufferEnd",       // 28
	"ClearDrawBuffer",        // 29
	"ClearZBuffer",           // 30
	"TextureFromContext",     // 31
	"BitmapFromContext",      // 32
	"Busy",                   // 33
	"SwapBuffers"             // 34
};

// Engine method names (indexed by DDK TQAEngineMethodTag 0-17)
// Order matches RAVESystem.h TQAEngineMethodTag exactly
static const char *engine_method_names[] = {
	"DrawPrivateNew",              // 0 -> sub-opcode 100
	"DrawPrivateDelete",           // 1 -> sub-opcode 101
	"CheckDevice",                 // 2 -> sub-opcode 102
	"Gestalt",                     // 3 -> sub-opcode 103
	"TextureNew",                  // 4 -> sub-opcode 104
	"TextureDetach",               // 5 -> sub-opcode 105
	"TextureDelete",               // 6 -> sub-opcode 106
	"BitmapNew",                   // 7 -> sub-opcode 107
	"BitmapDetach",                // 8 -> sub-opcode 108
	"BitmapDelete",                // 9 -> sub-opcode 109
	"ColorTableNew",               // 10 -> sub-opcode 110
	"ColorTableDelete",            // 11 -> sub-opcode 111
	"TextureBindColorTable",       // 12 -> sub-opcode 112
	"BitmapBindColorTable",        // 13 -> sub-opcode 113
	"AccessTexture",               // 14 -> sub-opcode 114
	"AccessTextureEnd",            // 15 -> sub-opcode 115
	"AccessBitmap",                // 16 -> sub-opcode 116
	"AccessBitmapEnd"              // 17 -> sub-opcode 117
};

/*
 *  RaveDispatch - multiplexed dispatch entry point
 *
 *  Called from execute_native_op() when NATIVE_RAVE_DISPATCH fires.
 *  Receives PPC registers r3-r8 as explicit arguments (the PPC thunk
 *  preserves r3-r10 for RAVE method args, using r0/r11 as scratch).
 *  Returns the value to be placed in gpr(3) by the caller.
 *
 *  Reads the sub-opcode from the scratch word and dispatches to the
 *  appropriate method handler.
 */
uint32 RaveDispatch(uint32 r3, uint32 r4, uint32 r5,
					uint32 r6, uint32 r7, uint32 r8)
{
	// Read sub-opcode from scratch word
	uint32 method_id = ReadMacInt32(rave_scratch_addr);

	if (method_id < kRaveDrawMethodCount) {
#if QD3D_GRAPHICS_LOGGING_ENABLED
		static uint64_t drawDispatchCount[kRaveDrawMethodCount] = {};
		uint64_t count = ++drawDispatchCount[method_id];
		if (method_id >= kRaveDrawDrawPoint &&
			(count <= 8 || (count & (count - 1)) == 0 || (count % 1024) == 0)) {
			QD3D_RENDER_LOG("Dispatch method=%u/%s count=%llu r3-r8=%08x,%08x,%08x,%08x,%08x,%08x",
							method_id, draw_method_names[method_id],
							(unsigned long long)count, r3, r4, r5, r6, r7, r8);
		}
#endif
		// Draw method dispatch (0-34)
		switch (method_id) {
		case kRaveDrawSetFloat:
			return (uint32)NativeSetFloat(r3, r4, r5);
		case kRaveDrawSetInt:
			return (uint32)NativeSetInt(r3, r4, r5);
		case kRaveDrawSetPtr:
			return (uint32)NativeSetPtr(r3, r4, r5);
		case kRaveDrawGetFloat:
			return NativeGetFloat(r3, r4);
		case kRaveDrawGetInt:
			return NativeGetInt(r3, r4);
		case kRaveDrawGetPtr:
			return NativeGetPtr(r3, r4);
		case kRaveDrawRenderStart:
			return (uint32)NativeRenderStart(r3, r4, r5);
		case kRaveDrawRenderEnd:
			return (uint32)NativeRenderEnd(r3, r4);
		case kRaveDrawRenderAbort:
			return (uint32)NativeRenderAbort(r3);
		case kRaveDrawFlush:
			return (uint32)NativeFlush(r3);
		case kRaveDrawSync:
			return (uint32)NativeSync(r3);
		case kRaveDrawDrawPoint:
			return (uint32)NativeDrawPoint(r3, r4);
		case kRaveDrawDrawLine:
			return (uint32)NativeDrawLine(r3, r4, r5);
		case kRaveDrawDrawTriGouraud:
			return (uint32)NativeDrawTriGouraud(r3, r4, r5, r6, r7);
		case kRaveDrawDrawTriTexture:
			return (uint32)NativeDrawTriTexture(r3, r4, r5, r6, r7);
		case kRaveDrawDrawVGouraud:
			return (uint32)NativeDrawVGouraud(r3, r4, r5, r6, r7);
		case kRaveDrawDrawVTexture:
			return (uint32)NativeDrawVTexture(r3, r4, r5, r6, r7);
		case kRaveDrawDrawBitmap:
			return (uint32)NativeDrawBitmap(r3, r4, r5);
		case kRaveDrawSubmitVerticesGouraud:
			return (uint32)NativeSubmitVerticesGouraud(r3, r4, r5);
		case kRaveDrawSubmitVerticesTexture:
			return (uint32)NativeSubmitVerticesTexture(r3, r4, r5);
		case kRaveDrawDrawTriMeshGouraud:
			return (uint32)NativeDrawTriMeshGouraud(r3, r4, r5);
		case kRaveDrawDrawTriMeshTexture:
			return (uint32)NativeDrawTriMeshTexture(r3, r4, r5);
		case kRaveDrawSetNoticeMethod:
			return (uint32)NativeSetNoticeMethod(r3, r4, r5, r6);
		case kRaveDrawGetNoticeMethod:
			return (uint32)NativeGetNoticeMethod(r3, r4, r5, r6);
		case kRaveDrawSubmitMultiTextureVertex:  // 24
			return (uint32)NativeSubmitMultiTextureParams(r3, r4, r5);
		// RAVE 1.6 buffer access and mid-frame clear
		case kRaveDrawAccessDrawBuffer:     // 25
			// SDK: AccessDrawBuffer(ctx, TQAPixelBuffer*)
			return (uint32)NativeAccessDrawBuffer(r3, r4);
		case kRaveDrawAccessDrawBufferEnd:  // 26
			return (uint32)NativeAccessDrawBufferEnd(r3, r4);
		case kRaveDrawAccessZBuffer:        // 27
			// SDK: AccessZBuffer(ctx, TQAZBuffer*)
			return (uint32)NativeAccessZBuffer(r3, r4);
		case kRaveDrawAccessZBufferEnd:     // 28
			return (uint32)NativeAccessZBufferEnd(r3, r4);
		case kRaveDrawClearDrawBuffer:      // 29
			return (uint32)NativeClearDrawBuffer(r3, r4, r5);
		case kRaveDrawClearZBuffer:         // 30
			return (uint32)NativeClearZBuffer(r3, r4, r5);
		case kRaveDrawTextureFromContext:   // 31
			return (uint32)NativeTextureNewFromDrawContext(r3, r4, r5);
		case kRaveDrawBitmapFromContext:    // 32
			return (uint32)NativeBitmapNewFromDrawContext(r3, r4, r5);
		case kRaveDrawBusy:                // 33
			return (uint32)NativeBusy(r3);
		case kRaveDrawSwapBuffers:         // 34
			return (uint32)NativeSwapBuffers(r3, r4);

		default:
			// All 35 draw methods (0-34) have explicit case handlers. This default arm is unreachable dead code. Test: RAVEABITests.testStubLogLines_drawMethods_deadCode
			__builtin_unreachable();
			return kQANoErr;
		}
	}
	else if (method_id >= kRaveEngineDrawPrivateNew &&
			 method_id < kRaveEngineDrawPrivateNew + kRaveEngineMethodCount) {
		// Engine method dispatch (100-117)
		int engine_index = method_id - kRaveEngineDrawPrivateNew;

		switch (method_id) {
		case kRaveEngineDrawPrivateNew:
			// Sub-opcode 100: shared by EngineGetMethod AND DrawPrivateNew
			// EngineGetMethod receives (methodTag, &method) where methodTag is 0-17
			// DrawPrivateNew receives (drawContext, device, rect, clip, flags)
			//   where drawContext is a Mac memory address (always > 1000)
			if (r3 < kRaveEngineMethodCount) {
				// Small r3 = EngineGetMethod(methodTag, methodPtr)
				return (uint32)NativeEngineGetMethod(r3, r4);
			} else {
				// Large r3 = DrawPrivateNew(drawContext, device, rect, clip, flags)
				return (uint32)NativeDrawPrivateNew(r3, r4, r5, r6, r7);
			}

		case kRaveEngineDrawPrivateDelete:
			// Sub-opcode 101: DrawPrivateDelete(drawPrivate handle)
			return (uint32)NativeDrawPrivateDelete(r3);

		case kRaveEngineCheckDevice:
			// Sub-opcode 102 (DDK tag 2): CheckDevice(devicePtr)
			return (uint32)NativeEngineCheckDevice(r3);

		case kRaveEngineGestalt:
			// Sub-opcode 103 (DDK tag 3): Gestalt(selector, responsePtr)
			return (uint32)NativeEngineGestalt(r3, r4);

		case kRaveEngineTextureNew:      // 104
			return (uint32)NativeEngineTextureNew(r3, r4, r5, r6);
		case kRaveEngineTextureDetach:   // 105
			return (uint32)NativeEngineTextureDetach(r3);
		case kRaveEngineTextureDelete:   // 106
			NativeEngineTextureDelete(r3);
			return kQANoErr;
		case kRaveEngineBitmapNew:       // 107
			return (uint32)NativeEngineBitmapNew(r3, r4, r5, r6);
		case kRaveEngineBitmapDetach:    // 108
			return (uint32)NativeEngineBitmapDetach(r3);
		case kRaveEngineBitmapDelete:    // 109
			NativeEngineBitmapDelete(r3);
			return kQANoErr;
		case kRaveEngineColorTableNew:   // 110
			return (uint32)NativeEngineColorTableNew(r3, r4, r5, r6);
		case kRaveEngineColorTableDelete: // 111
			NativeEngineColorTableDelete(r3);
			return kQANoErr;
		case kRaveEngineTextureBindColorTable: // 112
			return (uint32)NativeEngineTextureBindColorTable(r3, r4);
		case kRaveEngineBitmapBindColorTable:  // 113
			return (uint32)NativeEngineBitmapBindColorTable(r3, r4);

		case kRaveEngineAccessTexture:        // 114
			// SDK: AccessTexture(texture, mipmapLevel, flags, TQAPixelBuffer*)
			return (uint32)NativeEngineAccessTexture(r3, r4, r5, r6);
		case kRaveEngineAccessTextureEnd:     // 115
			return (uint32)NativeEngineAccessTextureEnd(r3, r4);
		case kRaveEngineAccessBitmap:         // 116
			// SDK: AccessBitmap(bitmap, flags, TQAPixelBuffer*)
			return (uint32)NativeEngineAccessBitmap(r3, r4, r5);
		case kRaveEngineAccessBitmapEnd:      // 117
			return (uint32)NativeEngineAccessBitmapEnd(r3, r4);

		default:
			// All 18 engine methods (100-117) have explicit case handlers. This default arm is unreachable dead code. Test: RAVEABITests.testStubLogLines_engineMethods_deadCode
			__builtin_unreachable();
			return kQANoErr;
		}
	}
	else if (method_id >= kRaveHookGetFirstEngine &&
			 method_id < kRaveHookGetFirstEngine + kRaveHookCount) {
		// Hook dispatch (200-204) -- intercept RAVE manager enumeration APIs
		switch (method_id) {
		case kRaveHookGetFirstEngine:
			// QADeviceGetFirstEngine(device)
			return NativeHookGetFirstEngine(r3);

		case kRaveHookGetNextEngine:
			// QADeviceGetNextEngine(device, prevEngine)
			return NativeHookGetNextEngine(r3, r4);

		case kRaveHookEngineGestalt:
			// QAEngineGestalt(engine, selector, response)
			return NativeHookEngineGestalt(r3, r4, r5);

		case kRaveHookEngineCheckDevice:
			// QAEngineCheckDevice(engine, device)
			return NativeHookEngineCheckDevice(r3, r4);

		case kRaveHookDrawContextNew:
			// QADrawContextNew(device, rect, clip, engine, flags, drawContextPtr)
			return NativeHookDrawContextNew(r3, r4, r5, r6, r7, r8);

		case kRaveHookTextureNew:
			// QATextureNew(engine, flags, pixelType, images, &newTexture)
			return NativeHookTextureNew(r3, r4, r5, r6, r7);

		case kRaveHookBitmapNew:
			// QABitmapNew(engine, flags, pixelType, &image, &newBitmap)
			return NativeHookBitmapNew(r3, r4, r5, r6, r7);

		case kRaveHookColorTableNew:
			// QAColorTableNew(engine, tableType, pixelData, transparentFlag, &newTable)
			return NativeHookColorTableNew(r3, r4, r5, r6, r7);

		case kRaveHookDrawContextDelete:
			// QADrawContextDelete(drawContextPtr)
			return NativeHookDrawContextDelete(r3);

		case kRaveHookTextureDelete:
			// QATextureDelete(engine, texturePtr)
			return NativeHookTextureDelete(r3, r4);

		case kRaveHookBitmapDelete:
			// QABitmapDelete(engine, bitmapPtr)
			return NativeHookBitmapDelete(r3, r4);

		case kRaveHookColorTableDelete:
			// QAColorTableDelete(engine, colorTablePtr)
			return NativeHookColorTableDelete(r3, r4);

		case kRaveHookTextureBindColorTable:
			// QATextureBindColorTable(engine, texture, colorTable)
			return NativeHookTextureBindColorTable(r3, r4, r5);

		case kRaveHookBitmapBindColorTable:
			// QABitmapBindColorTable(engine, bitmap, colorTable)
			return NativeHookBitmapBindColorTable(r3, r4, r5);

		case kRaveHookTextureDetach:
			// QATextureDetach(engine, texture)
			return NativeHookTextureDetach(r3, r4);

		case kRaveHookBitmapDetach:
			// QABitmapDetach(engine, bitmap)
			return NativeHookBitmapDetach(r3, r4);

		case kRaveHookAccessTexture:
			// QAAccessTexture(engine, texture, mipmapLevel, flags, buffer)
			return NativeHookAccessTexture(r3, r4, r5, r6, r7);

		case kRaveHookAccessTextureEnd:
			// QAAccessTextureEnd(engine, texture, dirtyRect)
			return NativeHookAccessTextureEnd(r3, r4, r5);

		case kRaveHookAccessBitmap:
			// QAAccessBitmap(engine, bitmap, flags, buffer)
			return NativeHookAccessBitmap(r3, r4, r5, r6);

		case kRaveHookAccessBitmapEnd:
			// QAAccessBitmapEnd(engine, bitmap, dirtyRect)
			return NativeHookAccessBitmapEnd(r3, r4, r5);

		case kRaveHookQ3PixmapSetImage:
			// Q3Pixmap_Set_Image intercept.
			// Sub-opcode 220. Dispatch entry wired in; the FindLibSymbol
			// activation path (hook on the QuickDraw 3D library
			// fragment) is deferred. Tests call NativeHookQ3PixmapSetImage
			// directly (no dispatch round-trip), so this arm is reached
			// only once the activation path lands.
			// r3 = pixmapAddr, r4 = srcHostAddr, r5 = byteCount
			NativeHookQ3PixmapSetImage(r3, r4, r5);
			return kQANoErr;

		case kRaveHookEngineEnable:
			// QAEngineEnable(vendorID, engineID)
			return NativeHookEngineEnable(r3, r4);

		case kRaveHookEngineDisable:
			// QAEngineDisable(vendorID, engineID)
			return NativeHookEngineDisable(r3, r4);

		default:
			RAVE_LOG("RAVE: unknown hook sub-opcode %d", method_id);
			return (uint32)(int32)kQANotSupported;
		}
	}
	else if (method_id >= kRaveATIClearDrawBuffer &&
			 method_id < kRaveATIClearDrawBuffer + kRaveATIMethodCount) {
		// ATI RaveExtFuncs dispatch (300-305)
		switch (method_id) {
		case kRaveATIClearDrawBuffer:   // 300
			return (uint32)NativeATIClearDrawBuffer(r3, r4);
		case kRaveATIClearZBuffer:      // 301
			return (uint32)NativeATIClearZBuffer(r3, r4);
		case kRaveATITextureUpdate:     // 302
			return (uint32)NativeATITextureUpdate(r3, r4, r5, r6);
		case kRaveATIBindCodeBook:      // 303
			return (uint32)NativeATIBindCodeBook(r3, r4);
		case kRaveATIGetDrawBuffer:     // 304
			return (uint32)NativeATIGetDrawBuffer(r3, r4);
		case kRaveATIStub:              // 305
			RAVE_LOG("RAVE: call through unidentified ATI RaveExtFuncs slot");
			return (uint32)(int32)kQANotSupported;
		default:
			return kQANoErr;
		}
	}
	else {
		// Unknown sub-opcode
		RAVE_LOG("RAVE: unknown sub-opcode %d", method_id);
		return (uint32)(int32)kQANotSupported;
	}
}

// ATI extension function declarations (implemented in rave_metal_renderer.mm and rave_engine.cpp)
extern int32_t NativeATIClearDrawBuffer(uint32_t drawContextAddr, uint32_t rectAddr);
extern int32_t NativeATIClearZBuffer(uint32_t drawContextAddr, uint32_t rectAddr);
extern int32_t NativeATIGetDrawBuffer(uint32_t drawContextAddr, uint32_t deviceStructAddr);
extern int32_t NativeATITextureUpdate(uint32_t flags, uint32_t pixelType, uint32_t imagesAddr, uint32_t textureAddr);
extern int32_t NativeATIBindCodeBook(uint32_t textureAddr, uint32_t codebookPtr);

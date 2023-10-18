//
//  video_rootless.cpp
//  BasiliskII
//
//  Created by Jesús A. Álvarez on 26/10/2019.
//

#include <SDL.h>
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "emul_op.h"
#include <vector>

extern void make_window_transparent(SDL_Window * window);
extern void update_sdl_video(SDL_Surface *s, int numrects, SDL_Rect *rects);
int native_menubar_size = 0;

/*
 *  Rootless mode support
 */

static const uint8 rootless_proc[] = {
    // PEntryFromProcessSerialNumber(GetNextProcess(d0:d1))
    // returns next PSN in d0:d1, PEntryPtr in d2
    0x2F, 0x01,             // move.l d1,-(sp)     ; psn low
    0x2F, 0x00,             // move.l d0,-(sp)     ; psn high
    0x55, 0x4F,             // suba.w #2,sp        ; result
    0x48, 0x6F, 0x00, 0x02, // pea 2(sp)           ; ptr(psn)
    0x3F, 0x3C, 0x00, 0x38, // move.w #$38,-(sp)   ; GetNextProcess
    0xA8, 0x8F,             // _OSDispatch         ; get psn of first process
    0x30, 0x1F,             // move.w (sp)+,d0     ; result

    0x59, 0x4F,             // subq #4,sp          ; result
    0x48, 0x6F, 0x00, 0x04, // pea 4(sp)           ; ptr(psn)
    0x3F, 0x3C, 0x00, 0x4F, // move.w #$4F,-(sp)   ; _PEntryFromProcessSerialNumber
    0xA8, 0x8F,             // _OSDispatch         ; get PEntry
    0x24, 0x1F,             // move.l (sp)+, d2    ; pEntry
    0x20, 0x1F,             // move.l (sp)+, d0    ; psn high
    0x22, 0x1F,             // move.l (sp)+, d1    ; psn low
    0x4E, 0x75,             // rts
    
    // a0 = GetResource(d0, d1)
#define GETRESOURCE 38      // offset to this function
    0x59, 0x4F,             // subq #4,sp          ; result
    0x2F, 0x00,             // move.l d0,-(sp)     ; type
    0x3F, 0x01,             // move.w d1,-(sp)     ; size
    0xA9, 0xA0,             // _GetResource
    0x20, 0x57,             // movea.l (sp),a0     ; result to a0
    0x59, 0x4F,             // subq #4, sp         ; result
    0x2F, 0x08,             // move.l a0,-(sp)     ; handle
    0xA9, 0xA5,             // _SizeRsrc
    0x20, 0x1F,             // move.l (sp)+,d0     ; size
    0x20, 0x5F,             // movea.l (sp)+,a0    ; handle
    0x4E, 0x75,             // rts

#define PAINTRGNPATCH (GETRESOURCE + 22)
    M68K_EMUL_OP_PAINTRGN >> 8,
    M68K_EMUL_OP_PAINTRGN & 0xff,
    0x2F, 0x3C, 0, 0, 0, 0, // move.l address, -(sp)
    0x4E, 0x75,             // rts

#define DRAGGRAYRGNPATCH (PAINTRGNPATCH + 10)
    M68K_EMUL_OP_DRAGGRAYRGN >> 8,
    M68K_EMUL_OP_DRAGGRAYRGN & 0xff,
    0x2F, 0x3C, 0, 0, 0, 0, // move.l address, -(sp)
    0x4E, 0x75,             // rts
};

static uint32 rootless_proc_ptr = 0;
static uint32 low_mem_map = 0;
static bool dragging_region = false;
static uint32 drag_region_ptr = 0;
static size_t mask_n = 0;

static void MyPatchTrap(int trap, uint32 ptr) {
    M68kRegisters r;
    r.d[0] = trap;
    Execute68kTrap(0xa746, &r);    // GetToolTrapAddress()
    WriteMacInt32(ptr + 4, r.a[0]);
    r.d[0] = trap;
    r.a[0] = ptr;
    Execute68kTrap(0xa647, &r);    // SetToolTrap()
}

int16 InstallRootlessProc() {
    // Rootless mode support
    M68kRegisters r;
    if (strncmp(PrefsFindString("screen"), "rootless", 8) == 0) {
        printf("Installing rootless support\n");
        r.d[0] = sizeof(rootless_proc);
        Execute68kTrap(0xa71e, &r); // NewPtrSysClear()
        if (r.a[0] == 0) {
            return memFullErr;
        }
        rootless_proc_ptr = r.a[0];
        Host2Mac_memcpy(rootless_proc_ptr, rootless_proc, sizeof(rootless_proc));
        low_mem_map = 0;
        printf("Installed at 0x%x\n", rootless_proc_ptr);
        
        // Install patches to detect dragging regions
        MyPatchTrap(0xa8d3, rootless_proc_ptr + PAINTRGNPATCH);
        MyPatchTrap(0xa905, rootless_proc_ptr + DRAGGRAYRGNPATCH);
    } else {
        rootless_proc_ptr = 0;
        low_mem_map = 0;
    }
    return noErr;
}

static struct {
    uint8_t *pixels;
    uint8_t *cursorMask;
    int w,h;
} display_mask = {
    .pixels = NULL,
    .cursorMask = NULL,
    .w = 0,
    .h = 0
};

void MaskRect(int16 top, int16 left, int16 bottom, int16 right, bool in) {
    if (top == bottom || left == right) return;
    if (top < 0) top = 0;
    if (left < 0) left = 0;
    if (bottom > display_mask.h) bottom = display_mask.h;
    if (right > display_mask.w) right = display_mask.w;
    
    uint8_t *line = display_mask.pixels + display_mask.w * top + left;
    for (int y = top; y < bottom; y++) {
        memset(line, in ? 0xff : 0x00, right - left);
        line += display_mask.w;
    }
}

void PrintRegion(uint32 regionPtr) {
    int16 size = ReadMacInt16(regionPtr);
    int16 top = ReadMacInt16(regionPtr + 2);
    int16 left = ReadMacInt16(regionPtr + 4);
    int16 bottom = ReadMacInt16(regionPtr + 6);
    int16 right = ReadMacInt16(regionPtr + 8);
    printf("Region (%d: %d,%d %dx%d):\n", size, left, top, (right-left), (bottom-top));
    for(int i=0; i < size; i++) {
        printf("%02x", ReadMacInt8(regionPtr + i));
    }
    printf("\n");
}

void MaskRegion(uint32 regionPtr, bool in) {
    // https://www.info-mac.org/viewtopic.php?t=17328
    uint16 size = ReadMacInt16(regionPtr);
    int16 top = ReadMacInt16(regionPtr + 2);
    int16 left = ReadMacInt16(regionPtr + 4);
    int16 bottom = ReadMacInt16(regionPtr + 6);
    int16 right = ReadMacInt16(regionPtr + 8);
    if (bottom > display_mask.h) bottom = display_mask.h;
    if (right > display_mask.w) right = display_mask.w;

    if (size == 10) {
        MaskRect(top, left, bottom, right, in);
        return;
    }
    
    uint8_t *scanline = display_mask.pixels + top * display_mask.w;
    uint8_t *curLine = (uint8*)alloca(display_mask.w);
    memset(curLine, 0, display_mask.w);
    
    uint32 ptr = regionPtr + 10;
    for (int16 y = top; y < bottom; y++) {
        uint16 nextLine = ReadMacInt16(ptr);
        if (nextLine == y) {
            // apply changes to current line
            ptr += 2;
            for(;;) {
                uint16 begin = ReadMacInt16(ptr);
                ptr += 2;
                if (begin == 0x7fff) break;
                uint16 end = ReadMacInt16(ptr);
                ptr += 2;
                if (end > display_mask.w) {
                    end = display_mask.w;
                }
                for (int i=begin; i < end; i++) {
                    curLine[i] ^= 0xff;
                }
            }
        }
        
        // blit current line
        if (in) {
            for (int x = left; x < right; x++) {
                scanline[x] |= curLine[x];
            }
        } else {
            for (int x = left; x < right; x++) {
                scanline[x] |= ~curLine[x];
            }
        }
        
        scanline += display_mask.w;
    }
}

SDL_Rect GetRegionBounds(uint32 regionPtr) {
    int16 top = ReadMacInt16(regionPtr + 2);
    int16 left = ReadMacInt16(regionPtr + 4);
    int16 bottom = ReadMacInt16(regionPtr + 6);
    int16 right = ReadMacInt16(regionPtr + 8);
    return (SDL_Rect){.x = left, .y = top, .w = right-left, .h = bottom-top};
}

uint32 GetLowMemOffset(uint32 addr) {
    if (low_mem_map == 0) {
        abort();
    }
    
    uint32 offset = 0;
    uint32 ptr = low_mem_map;
    for(;;) {
        uint16 size = ReadMacInt16(ptr);
        if (size == 0) break;
        uint32 lo = ReadMacInt32(ptr+2);
        ptr += 6;
        if (addr < lo) {
            return UINT32_MAX;
        } else if (addr < (lo+size)) {
            return offset + (addr-lo);
        }
        offset += size;
    }
    return UINT32_MAX;
}

static uint32 GetResource(uint32 type, int16 id, int32* size) {
    M68kRegisters r;
    r.d[0] = type;
    r.d[1] = id;
    Execute68k(rootless_proc_ptr + GETRESOURCE, &r);
    if (size) {
        *size = r.d[0];
    }
    return r.a[0];
}

static SDL_Rect MaskMenu(uint32 mbEntry) {
    int16 menuTop = ReadMacInt16(mbEntry);
    int16 menuLeft = ReadMacInt16(mbEntry + 2);
    int16 menuBottom = ReadMacInt16(mbEntry + 4);
    int16 menuRight = ReadMacInt16(mbEntry + 6);
    MaskRect(menuTop-1, menuLeft-1, menuBottom+1, menuRight+1, true);
    // shadow
    MaskRect(menuBottom+1, menuLeft+1, menuBottom+2, menuRight+1, true);
    MaskRect(menuTop+2, menuRight+1, menuBottom+2, menuRight+2, true);
    return (SDL_Rect){.x = menuLeft-1, .y = menuTop-1, .w = menuRight - menuLeft + 3, .h = menuBottom - menuTop + 3};
}

uint16 menuEntries[16];
uint16 *lastMenuEntry = menuEntries;
uint16 menuBarHeight;
bool inMenuSelect = false;

static SDL_Rect MaskMenuBar() {
    if (native_menubar_size && ReadMacInt16(0x0BAA) == 20) {
        // Embiggen menubar
        WriteMacInt16(0xBAA, native_menubar_size);
    }
    if (!inMenuSelect) {
        menuBarHeight = ReadMacInt16(0x0BAA);
    }
    MaskRect(0, 0, menuBarHeight, display_mask.w, true);
    return (SDL_Rect){.x = 0, .y = 0, .w = display_mask.w, .h = menuBarHeight};
}

static void MaskMenus(uint32 expandMem, uint32 lowMemPtr, std::vector<SDL_Rect> &rects) {
    uint32 mbSaveLoc = ReadMacInt32(ReadMacInt32(lowMemPtr + GetLowMemOffset(0x0B5C)));
    if (mbSaveLoc == 0) {
        // no menu yet
        inMenuSelect = false;
        return;
    }
    
    uint16 mbEntryOffset = ReadMacInt16(mbSaveLoc);
    if (mbEntryOffset == 0) {
        inMenuSelect = false;
        return;
    } else if (lastMenuEntry == menuEntries && *lastMenuEntry == 0) {
        // first menu
        *lastMenuEntry = mbEntryOffset;
    } else if (mbEntryOffset > *lastMenuEntry && lastMenuEntry < &menuEntries[16]) {
        // added menu
        *(++lastMenuEntry) = mbEntryOffset;
    } else if (mbEntryOffset < *lastMenuEntry) {
        // removed menu
        lastMenuEntry--;
    }
    
    inMenuSelect = true;
    
    // mask all menus
    for (uint16 *entry = menuEntries; entry <= lastMenuEntry; entry++) {
        rects.push_back(MaskMenu(mbSaveLoc + *entry));
    }
}

static void MaskBits(int16 x, int16 y, uint16 bits) {
    uint16 testBit = 0x8000;
    for(int i=0; i < 16; i++, testBit >>= 1) {
        if (x < 0 || y < 0 || y >= display_mask.h) continue;
        display_mask.pixels[x + (y * display_mask.w) + i] |= (bits & testBit) ? 0xff : 0x00;
    }
}

bool cursor_point_opaque() {
    if (display_mask.pixels == NULL) {
        return true;
    }
    int32 my = ReadMacInt16(0x0828);
    int32 mx = ReadMacInt16(0x082a);
    return display_mask.cursorMask[mx + my * display_mask.w];
}

static SDL_Rect MaskCursor() {
    int32 y = ReadMacInt16(0x0830);
    int32 x = ReadMacInt16(0x0832);
    // cursor data
    uint16 *TheCrsr = (uint16*)Mac2HostAddr(0x0844);
    // hotspot
    uint16 hx = ntohs(TheCrsr[33]);
    uint16 hy = ntohs(TheCrsr[32]);
    
    // apply mask
    for (int i=0; i < 16; i++) {
        MaskBits(x-hx, y+i-hy, ntohs(TheCrsr[16+i]));
    }
    return (SDL_Rect){.x=x-hx, .y=y-hy, .w=16, .h=16};
}

bool IsLayer(uint32 windowPtr) {
    return ReadMacInt16(windowPtr + 0x4A) == 0xDEAD;
}

void WalkLayerHierarchy(uint32 layerPtr, int level, std::vector<SDL_Rect> &mask_rects) {
    if (layerPtr == 0) return;
    int kind = ReadMacInt16(layerPtr + 0x6C);
    int visible = ReadMacInt8(layerPtr + 0x6E);
    bool isLayer = IsLayer(layerPtr);
    uint32 strucRgnHandle = ReadMacInt32(layerPtr + 0x72);
    int x = 0,y = 0,w = 0,h = 0;
    if (strucRgnHandle) {
        uint32 regionPtr = ReadMacInt32(strucRgnHandle);
        y = ReadMacInt16(regionPtr + 2);
        x = ReadMacInt16(regionPtr + 4);
        h = ReadMacInt16(regionPtr + 6) - y;
        w = ReadMacInt16(regionPtr + 8) - x;
        if (visible && w && h && !isLayer) {
            mask_rects.push_back(GetRegionBounds(regionPtr));
        }
    }
    //printf("%*s%s 0x%x, kind=%d, visible=%d, %d,%d %dx%d\n", 2*level, "", IsLayer(layerPtr) ? "Layer" : "Window", layerPtr, kind, visible, x,y,w,h);
    
    if (IsLayer(layerPtr)) {
        uint32 subWindows = ReadMacInt32(layerPtr + 0x94);
        WalkLayerHierarchy(subWindows, level+1, mask_rects);
    }
    
    uint32 nextWindow = ReadMacInt32(layerPtr + 0x90);
    if (nextWindow) WalkLayerHierarchy(nextWindow, level, mask_rects);
}

bool update_display_mask(SDL_Window *window, int w, int h) {
    if (rootless_proc_ptr == 0) {
        return false;
    }
    
    // Check for process manager
    uint32 expandMem = ReadMacInt32(0x02B6);
    uint16 emProcessMgrExists = ReadMacInt16(expandMem + 0x0128);
    if (!emProcessMgrExists) {
        return false;
    }
    
    // Read lowmem mapping
    if (low_mem_map == 0) {
        uint32 handle = GetResource('lmem', -16458, NULL);
        low_mem_map = ReadMacInt32(handle);
        printf("low_mem_map at 0x%x\n", low_mem_map);
    }
    
    std::vector<SDL_Rect> mask_rects;
    
    if (display_mask.w != w || display_mask.h != h) {
        // new mask
        free(display_mask.pixels);
        display_mask.pixels = (uint8_t*)calloc(2, w*h);
        display_mask.w = w;
        display_mask.h = h;
        display_mask.cursorMask = &display_mask.pixels[display_mask.w * display_mask.h];
        
        // update whole screen
        make_window_transparent(window);
        SDL_Rect rect = {.x = 0, .y = 0, .w = w, .h = h};
        update_sdl_video(NULL, 1, &rect);
        mask_rects.push_back(rect);
        memset(display_mask.pixels, 0xff, 2 * display_mask.w * display_mask.h);
    } else {
        // clear all
        memset(display_mask.pixels, 0, display_mask.w * display_mask.h);

        // show non-desktop
        uint32 deskPort = ReadMacInt32(0x9E2);
        uint32 deskPortVisRgn = ReadMacInt32(ReadMacInt32(deskPort + 0x18));
        MaskRegion(deskPortVisRgn, false);
    }
    
    bool has_front_process = false;
    
    M68kRegisters r;
    uint32 rootLayerPtr = 0;
    for(r.d[0] = 0, r.d[1] = 0;;) {
        Execute68k(rootless_proc_ptr, &r);
        uint32_t pEntryPtr = r.d[2];
        if (r.d[2] == 0) break;
        
        uint16 state = ReadMacInt16(pEntryPtr);
        if (state == 4) {
            has_front_process = true;
            uint32 lowMemPtr = ReadMacInt32(ReadMacInt32(pEntryPtr + 0x9E));
            if (lowMemPtr) {
                MaskMenus(expandMem, lowMemPtr, mask_rects);
            }
        }
        
        uint32 layerPtr = ReadMacInt32(pEntryPtr + 0x70);
        uint16 layerTxSize = ReadMacInt16(layerPtr + 0x4A);
        if (layerTxSize != 0xDEAD) {
            // not a layer
            continue;
        }
        
        // find root layer
        if (rootLayerPtr == 0) {
            rootLayerPtr = ReadMacInt32(layerPtr + 0x82); // parent layer
            while (ReadMacInt32(rootLayerPtr + 0x82)) {
                rootLayerPtr = ReadMacInt32(rootLayerPtr + 0x82);
            }
            WalkLayerHierarchy(rootLayerPtr, 0, mask_rects);
        }
    }
    
    // Menu Bar
    mask_rects.push_back(MaskMenuBar());
    
    // Drag region
    int8 mouseState = ReadMacInt8(0x0172);
    if (dragging_region && mouseState) {
        dragging_region = false;
        drag_region_ptr = 0;
    } else if (dragging_region && !mouseState && drag_region_ptr) {
        MaskRegion(drag_region_ptr, true);
        mask_rects.push_back(GetRegionBounds(drag_region_ptr));
    }
    
    // Copy over cursor mask
    memcpy(display_mask.cursorMask, display_mask.pixels, display_mask.w * display_mask.h);
    
    // Cursor
    if (cursor_point_opaque()) {
        SDL_ShowCursor(SDL_DISABLE);
        mask_rects.push_back(MaskCursor());
    } else {
        SDL_ShowCursor(SDL_ENABLE);
    }
    
    extern void update_window_mask_rects(SDL_Window * window, int h, const std::vector<SDL_Rect> &rects);
    update_window_mask_rects(window, display_mask.h, mask_rects);
	
	bool f = mask_rects.size() > mask_n;
	mask_n = mask_rects.size();
	return f;
}

void apply_display_mask(SDL_Surface * host_surface, SDL_Rect update_rect) {
    if (display_mask.pixels == NULL) {
        return;
    }
    
    if (host_surface->format->format != SDL_PIXELFORMAT_ARGB8888) {
        printf("Invalid host surface\n");
        return;
    }
    
    uint32_t * srcPixels = (uint32_t*)((uint8_t *)host_surface->pixels +
                                       update_rect.y * host_surface->pitch +
                                       update_rect.x * 4);
    
    uint8_t * srcMask = display_mask.pixels + update_rect.y * display_mask.w + update_rect.x;
    for (int y = update_rect.y; y < update_rect.y+update_rect.h; y++) {
        uint32_t * pixel = srcPixels;
        uint8_t * mask = srcMask;
        for (int x = update_rect.x; x < update_rect.x+update_rect.w; x++) {
            if (*mask == 0) {
                *pixel = 0;
            }
            pixel++;
            mask++;
        }
        srcPixels += host_surface->pitch / 4;
        srcMask += display_mask.w;
    }
}

void check_drag_region(M68kRegisters *r, uint16 opcode) {
    if (opcode == M68K_EMUL_OP_DRAGGRAYRGN) {
        dragging_region = true;
        drag_region_ptr = 0;
    } else if (opcode == M68K_EMUL_OP_PAINTRGN && dragging_region) {
        uint32 regionHandle = ReadMacInt32(r->a[7]+4);
        drag_region_ptr = ReadMacInt32(regionHandle);
    }
}

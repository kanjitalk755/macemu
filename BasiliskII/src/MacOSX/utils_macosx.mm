/*
 *	utils_macosx.mm - Mac OS X utility functions.
 *
 *  Copyright (C) 2011 Alexei Svitkine
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

#include <Cocoa/Cocoa.h>
#include <QuartzCore/QuartzCore.h>
#include <objc/runtime.h>
#include "sysdeps.h"
#include "utils_macosx.h"
#include <SDL.h>

#include "utils_macosx.h"
#include <vector>

#if SDL_VERSION_ATLEAST(2,0,0)
#include <SDL_syswm.h>
#endif

#include <sys/sysctl.h>
#include <Metal/Metal.h>

// This is used from video_sdl.cpp.
void NSAutoReleasePool_wrap(void (*fn)(void))
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	fn();
	[pool release];
}

#if SDL_VERSION_ATLEAST(2,0,0)

void disable_SDL2_macosx_menu_bar_keyboard_shortcuts() {
	for (NSMenuItem * menu_item in [NSApp mainMenu].itemArray) {
		if (menu_item.hasSubmenu) {
			for (NSMenuItem * sub_item in menu_item.submenu.itemArray) {
				sub_item.keyEquivalent = @"";
				sub_item.keyEquivalentModifierMask = 0;
			}
		}
		if ([menu_item.title isEqualToString:@"View"]) {
			[[NSApp mainMenu] removeItem:menu_item];
			break;
		}
	}
}

#ifdef VIDEO_ROOTLESS
void make_window_transparent(SDL_Window * window)
{
    if (!window) {
        return;
    }
    
    extern int native_menubar_size;
    native_menubar_size = (int)[[NSApp mainMenu] menuBarHeight];
    
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
        return;
    }
    
    CGColorRef clearColor = [NSColor clearColor].CGColor;
    NSWindow *cocoaWindow = wmInfo.info.cocoa.window;
    NSView *sdlView = cocoaWindow.contentView;
    sdlView.wantsLayer = YES;
    sdlView.layer.backgroundColor = [NSColor clearColor].CGColor;
    if (SDL_GetWindowData(window, "observing") == NULL) {
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:cocoaWindow queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification * _Nonnull note) {
            NSWindow *window = (NSWindow*)note.object;
            window.level = NSMainMenuWindowLevel+1;
        }];
        [nc addObserverForName:NSWindowDidResignKeyNotification object:cocoaWindow queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification * _Nonnull note) {
            NSWindow *window = (NSWindow*)note.object;
            // hack for window to be sent behind new key window
            [window setIsVisible:NO];
            [window setLevel:NSNormalWindowLevel];
            [window setIsVisible:YES];
        }];
        SDL_SetWindowData(window, "observing", nc);
    }
    if (SDL_GetWindowData(window, "maskLayer") == NULL) {
        CALayer *maskLayer = [CAShapeLayer layer];
        sdlView.layer.mask = maskLayer;
        SDL_SetWindowData(window, "maskLayer", maskLayer);
    }
    cocoaWindow.backgroundColor = [NSColor clearColor];
    cocoaWindow.hasShadow = NO;
    cocoaWindow.opaque = NO;
    if (cocoaWindow.isKeyWindow) {
        cocoaWindow.level = NSMainMenuWindowLevel+1;
    }
    
    // make metal layer transparent
    for (NSView *view in sdlView.subviews) {
        if ([view.className isEqualToString:@"SDL_cocoametalview"]) {
            view.layer.opaque = NO;
            view.layer.backgroundColor = clearColor;
            return;
        }
    }

    // make OpenGL surface transparent
    GLint zero = 0;
    [[NSOpenGLContext currentContext] setValues:&zero forParameter:NSOpenGLCPSurfaceOpacity];
}

void update_window_mask_rects(SDL_Window * window, int h, const std::vector<SDL_Rect> &rects)
{
    CAShapeLayer *maskLayer = (CAShapeLayer*)SDL_GetWindowData(window, "maskLayer");
    CGMutablePathRef path = CGPathCreateMutable();
    for(auto it = rects.begin(); it != rects.end(); ++it) {
        SDL_Rect rect = *it;
        CGPathAddRect(path, NULL, CGRectMake(rect.x, rect.y, rect.w, rect.h));
    }
    maskLayer.path = path;
    maskLayer.affineTransform = CGAffineTransformScale(CGAffineTransformMakeTranslation(0, h), 1.0, -1.0);
    CGPathRelease(path);
}
#endif

bool is_fullscreen_osx(SDL_Window * window)
{
	if (!window) {
		return false;
	}
	
	SDL_SysWMinfo wmInfo;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	if (!SDL_GetWindowWMInfo(window, &wmInfo, SDL_SYSWM_CURRENT_VERSION)) {
		return false;
	}
#else
	SDL_VERSION(&wmInfo.version);
	if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
		return false;
	}
#endif

	const NSWindowStyleMask styleMask = [wmInfo.info.cocoa.window styleMask];
	return (styleMask & NSWindowStyleMaskFullScreen) != 0;
}
#endif

void set_menu_bar_visible_osx(bool visible)
{
	[NSMenu setMenuBarVisible:(visible ? YES : NO)];
}

void set_current_directory()
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	chdir([[[[NSBundle mainBundle] bundlePath] stringByDeletingLastPathComponent] UTF8String]);
	[pool release];
}

bool MetalIsAvailable() {
	const int EL_CAPITAN = 15; // Darwin major version of El Capitan
	char s[16];
	size_t size = sizeof(s);
	int v;
	if (sysctlbyname("kern.osrelease", s, &size, NULL, 0) || sscanf(s, "%d", &v) != 1 || v < EL_CAPITAN) return false;
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	bool r = dev != nil;
	[dev release];
	return r;
}

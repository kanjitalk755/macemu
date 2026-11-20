/*
 *  video.cpp - Video/graphics emulation
 *
 *  SheepShaver (C) 1997-2008 Marc Hellwig and Christian Bauer
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

/*
 * TODO
 * - check for supported modes ???
 */

#include <stdio.h>
#include <string.h>

#include "sysdeps.h"
#include "video.h"
#include "video_defs.h"
#include "main.h"
#include "adb.h"
#include "macos_util.h"
#include "user_strings.h"
#include "version.h"
#include "thunks.h"
#include "rom_patches.h"

#if defined(__APPLE__)
#include "TargetConditionals.h"
#endif

#if TARGET_OS_IPHONE
#include "MiscellaneousSettingsObjCCppHeader.h"
#include "gfx_color_policy.h"
#endif
#include "dsp_pixmap_offsets.h"
#if (defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) || TARGET_OS_IPHONE
#include "display_mode_controller.h"
#include "dsp_video_status_policy.h"
#include "metal_compositor.h"
#endif

#define DEBUG 0
#include "debug.h"


// Global variables
bool video_activated = false;		// Flag: video display activated, mouse and keyboard data valid
uint32 screen_base = 0;				// Frame buffer base address
int cur_mode;						// Number of current video mode (index in VModes array)
int display_type = DIS_INVALID;		// Current display type
rgb_color mac_pal[256];
rgb_color mac_gamma[256];
uint8 remap_mac_be[256];
uint8 MacCursor[68] = {16, 1};	// Mac cursor image


bool keyfile_valid;		// Flag: Keyfile is valid, enable full-screen modes


/*
 *  Video mode information (constructed by VideoInit())
 */

struct VideoInfo VModes[64];


/*
 *  Driver local variables
 */

VidLocals *private_data = NULL;	// Pointer to driver local variables (there is only one display, so this is ok)

static long save_conf_id = APPLE_W_640x480;
static long save_conf_mode = APPLE_8_BIT;

#if defined(ENABLE_GFXACCEL)
/* InterfaceLib entry points for guest display leases and DSp's Window Manager
 * backdrop. They are resolved once, but all calls remain synchronous on the
 * emulation thread. */
static uint32 guest_dm_set_display_mode = 0;
static uint32 guest_set_entries = 0;
static uint32 guest_new_cwindow = 0;
static uint32 guest_dispose_window = 0;
static uint32 guest_new_rgn = 0;
static uint32 guest_rect_rgn = 0;
static uint32 guest_paint_one = 0;
static uint32 guest_dispose_rgn = 0;
static uint32 guest_hide_cursor = 0;
static uint32 guest_show_cursor = 0;
/* Nest count for publish-time HideCursor (outermost only issues Hide/Show). */
static int s_screen_publish_cm_depth = 0;
/* False during driver mode transitions; begin/end are no-ops then. */
static bool s_screen_publish_cm_enabled = true;
#endif

static void video_log_palette_summary(const char *op, uint32 start, uint32 count)
{
	static int s_palette_log_count = 0;
	if (s_palette_log_count >= 32) return;
	s_palette_log_count++;

	const rgb_color first = mac_pal[0];
	int non_black = 0;
	int different_from_first = 0;
	int non_gray = 0;
	for (int i = 0; i < 256; i++) {
		const rgb_color c = mac_pal[i];
		if (c.red != 0 || c.green != 0 || c.blue != 0)
			non_black++;
		if (c.red != first.red || c.green != first.green || c.blue != first.blue)
			different_from_first++;
		if (c.red != c.green || c.green != c.blue)
			non_gray++;
	}

	const int depth =
		(cur_mode >= 0 && cur_mode < 64) ? VModes[cur_mode].viAppleMode : -1;
	fprintf(stderr,
	        "VIDEO: palette %s start=%u count=%u curMode=%d depth=%d "
	        "nonBlack=%d diff0=%d nonGray=%d idx0=(%u,%u,%u)\n",
	        op, start, count, cur_mode, depth, non_black,
	        different_from_first, non_gray,
	        first.red, first.green, first.blue);
	fflush(stderr);
}


// Function pointers of imported functions
typedef int16 (*iocic_ptr)(void *, int16);
static uint32 iocic_tvect = 0;
static inline int16 IOCommandIsComplete(uintptr arg1, int16 arg2)
{
	return (int16)CallMacOS2(iocic_ptr, iocic_tvect, (void *)arg1, arg2);
}
typedef int16 (*vslnewis_ptr)(void *, uint32, uint32 *);
static uint32 vslnewis_tvect = 0;
static inline int16 VSLNewInterruptService(uintptr arg1, uint32 arg2, uintptr arg3)
{
	return (int16)CallMacOS3(vslnewis_ptr, vslnewis_tvect, (void *)arg1, arg2, (uint32 *)arg3);
}
typedef int16 (*vsldisposeis_ptr)(uint32);
static uint32 vsldisposeis_tvect = 0;
static inline int16 VSLDisposeInterruptService(uint32 arg1)
{
	return (int16)CallMacOS1(vsldisposeis_ptr, vsldisposeis_tvect, arg1);
}
typedef int16 (*vsldois_ptr)(uint32);
static uint32 vsldois_tvect = 0;
int16 VSLDoInterruptService(uint32 arg1)
{
	return (int16)CallMacOS1(vsldois_ptr, vsldois_tvect, arg1);
}
typedef void (*nqdmisc_ptr)(uint32, void *);
static uint32 nqdmisc_tvect = 0;
void VideoInstallDisplay(void)
{
	if (!HasNanoKernel2() || ReadMacInt16(ROMBase + 0x310004) < 0x0210)
		return;
	if (private_data == NULL || private_data->displayregistered || private_data->displayregistrationactive)
		return;
	private_data->displayregistrationactive = true;
	uint32 bootdevice = ReadMacInt32(LMADDR_MAIN_DEVICE);
	if (bootdevice != 0) {
		uint32 bootrecord = ReadMacInt32(bootdevice);
		uint32 bootpixmaphandle = ReadMacInt32(bootrecord + 0x16);
		uint32 bootpixmap = ReadMacInt32(bootpixmaphandle);
		if (ReadMacInt16(bootrecord) == 0 && ReadMacInt16(bootpixmap + 10) == 32 && ReadMacInt16(bootpixmap + 12) == 32) {
			typedef void (*displayinitproc)(int16, uint32, uint32);
			typedef void (*displayattributeproc)(uint32, int16, uint8);
			typedef void (*displayselectproc)(uint32);
			uint32 initroutine = FindLibSymbol("\014InterfaceLib", "\013InitGDevice");
			uint32 attributeroutine = FindLibSymbol("\014InterfaceLib", "\022SetDeviceAttribute");
			uint32 selectroutine = FindLibSymbol("\014InterfaceLib", "\012SetGDevice");
			if (initroutine && attributeroutine && selectroutine) {
				CallMacOS3(displayattributeproc, attributeroutine, bootdevice, 14, 0);
				CallMacOS3(displayattributeproc, attributeroutine, bootdevice, 10, 0);
				CallMacOS3(displayinitproc, initroutine, private_data->driverreference, video_rel_depth_from_abs(private_data->saveMode), bootdevice);
				CallMacOS1(displayselectproc, selectroutine, bootdevice);
			}
		}
	}
	typedef int16 (*displaybeginproc)(uint32 *);
	typedef int16 (*displaynewproc)(uint32 *, int16, uint32, uint32, uint32, uint32, uint32);
	typedef int16 (*displaydeviceproc)(uint32, uint32);
	typedef int16 (*displayendproc)(uint32);
	uint32 beginroutine = FindLibSymbol("\014InterfaceLib", "\030DMBeginConfigureDisplays");
	uint32 newroutine = FindLibSymbol("\014InterfaceLib", "\014DMNewDisplay");
	uint32 enableroutine = FindLibSymbol("\014InterfaceLib", "\017DMEnableDisplay");
	uint32 mainroutine = FindLibSymbol("\014InterfaceLib", "\020DMSetMainDisplay");
	uint32 endroutine = FindLibSymbol("\014InterfaceLib", "\026DMEndConfigureDisplays");
	if (!beginroutine || !newroutine || !enableroutine || !mainroutine || !endroutine) {
		private_data->displayregistrationactive = false;
		return;
	}
	SheepVar32 displaystate = 0;
	SheepVar32 displaydevice = 0;
	int16 error = (int16)CallMacOS1(displaybeginproc, beginroutine, (uint32 *)(uintptr)displaystate.addr());
	if (error) {
		private_data->displayregistrationactive = false;
		return;
	}
	if (private_data->displayhandle == 0) {
		error = (int16)CallMacOS7(displaynewproc, newroutine, (uint32 *)(uintptr)displaydevice.addr(), private_data->driverreference, video_rel_depth_from_abs(private_data->saveMode), 0, 0, 0, displaystate.value());
		if (!error)
			private_data->displayhandle = displaydevice.value();
	}
	if (!error)
		error = (int16)CallMacOS2(displaydeviceproc, enableroutine, private_data->displayhandle, displaystate.value());
	if (!error) {
		uint32 maindevice = ReadMacInt32(LMADDR_MAIN_DEVICE);
		if (maindevice != 0 && ReadMacInt16(ReadMacInt32(maindevice)) == (uint16)private_data->driverreference)
			private_data->displayhandle = maindevice;
		else
			error = (int16)CallMacOS2(displaydeviceproc, mainroutine, private_data->displayhandle, displaystate.value());
	}
	int16 enderror = (int16)CallMacOS1(displayendproc, endroutine, displaystate.value());
	if (!error)
		error = enderror;
	if (!error)
		private_data->displayregistered = true;
	private_data->displayregistrationactive = false;
}

bool VideoDriverInitialized(void)
{
	return private_data != NULL;
}

void NQDMisc(uint32 arg1, uintptr arg2)
{
	CallMacOS2(nqdmisc_ptr, nqdmisc_tvect, arg1, (void *)arg2);
}


// Prototypes
static int16 set_gamma(VidLocals *csSave, uint32 gamma);


/*
 *  Tell whether window/screen is activated or not (for mouse/keyboard polling)
 */

bool VideoActivated(void)
{
	return video_activated;
}


/*
 *  Create RGB snapshot of current screen
 */

bool VideoSnapshot(int xsize, int ysize, uint8 *p)
{
	if (display_type == DIS_WINDOW) {
		uint8 *screen = (uint8 *)private_data->saveBaseAddr;
		uint32 row_bytes = VModes[cur_mode].viRowBytes;
		uint32 y2size = VModes[cur_mode].viYsize;
		uint32 x2size = VModes[cur_mode].viXsize;
		for (int j=0;j<ysize;j++) {
			for (int i=0;i<xsize;i++) {
				*p++ = mac_pal[screen[uint32(float(j)*float(y2size)/float(ysize))*row_bytes+uint32(float(i)*float(x2size)/float(xsize))]].red;
				*p++ = mac_pal[screen[uint32(float(j)*float(y2size)/float(ysize))*row_bytes+uint32(float(i)*float(x2size)/float(xsize))]].green;
				*p++ = mac_pal[screen[uint32(float(j)*float(y2size)/float(ysize))*row_bytes+uint32(float(i)*float(x2size)/float(xsize))]].blue;
			}
		}
		return true;
	}
	return false;
}


/*
 *  Determine whether we should use the hardware or software cursor, and return true for the former, false for the latter.
 *  Currently we use the hardware cursor if we can, but perhaps this can be made a preference someday.
 */

static bool UseHardwareCursor(void)
{
	return video_can_change_cursor();
}


/*
 *  Video driver open routine
 */

static int16 VideoOpen(uint32 pb, VidLocals *csSave)
{
	D(bug("Video Open\n"));

	// Set up VidLocals
	csSave->saveBaseAddr = screen_base;
	csSave->saveData = VModes[cur_mode].viAppleID;// First mode ...
	csSave->saveMode = VModes[cur_mode].viAppleMode;
	csSave->savePage = 0;
	csSave->saveVidParms = 0;			// Add the right table
	csSave->luminanceMapping = false;
	csSave->cursorHardware = UseHardwareCursor();
	csSave->cursorX = 0;
	csSave->cursorY = 0;
	csSave->cursorVisible = 0;
	csSave->cursorSet = 0;
	csSave->cursorHotFlag = false;
	csSave->cursorHotX = 0;
	csSave->cursorHotY = 0;

	// Find and set default gamma table
	csSave->gammaTable = 0;
	csSave->maxGammaTableSize = 0;
	set_gamma(csSave, 0);

	// Install and activate interrupt service
	SheepVar32 theServiceID = 0;
	VSLNewInterruptService(csSave->regEntryID, FOURCC('v','b','l',' '), theServiceID.addr());
	csSave->vslServiceID = theServiceID.value();
	D(bug(" Interrupt ServiceID %08lx\n", csSave->vslServiceID));
	csSave->interruptsEnabled = true;

	return noErr;
}


/*
 *  Video driver control routine
 */

static bool allocate_gamma_table(VidLocals *csSave, uint32 size)
{
	if (size > csSave->maxGammaTableSize) {
		if (csSave->gammaTable) {
			Mac_sysfree(csSave->gammaTable);
			csSave->gammaTable = 0;
			csSave->maxGammaTableSize = 0;
		}
		if ((csSave->gammaTable = Mac_sysalloc(size)) == 0)
			return false;
		csSave->maxGammaTableSize = size;
	}
	return true;
}

 static inline uint8 max(uint8 a, uint8 b) {
	 return a > b? a : b;
 }

#if (defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) || TARGET_OS_IPHONE
static void publish_gamma_lut_to_display_controller(VidLocals *csSave)
{
	uint8 lut[768];
	for (int i=0; i<256; i++) {
		lut[i] = lut[256 + i] = lut[512 + i] = (uint8)i;
	}

	if (csSave != NULL && csSave->gammaTable != 0) {
		uint32 gamma_table = csSave->gammaTable;
		int chan_cnt = ReadMacInt16(gamma_table + gChanCnt);
		int data_width = ReadMacInt16(gamma_table + gDataWidth);
		int data_cnt = ReadMacInt16(gamma_table + gDataCnt);
		if ((chan_cnt == 1 || chan_cnt == 3) &&
		    data_width >= 1 && data_width <= 8 &&
		    data_cnt == (1 << data_width)) {
			uint32 p = gamma_table + gFormulaData + ReadMacInt16(gamma_table + gFormulaSize);
			uint8 *red_gamma = Mac2HostAddr(p);
			uint8 *green_gamma = red_gamma;
			uint8 *blue_gamma = red_gamma;
			if (chan_cnt == 3 && red_gamma != NULL) {
				green_gamma = red_gamma + data_cnt;
				blue_gamma = green_gamma + data_cnt;
			}
			if (red_gamma != NULL && green_gamma != NULL && blue_gamma != NULL) {
				const int shift = 8 - data_width;
				for (int i=0; i<256; i++) {
					int idx = i >> shift;
					lut[i] = red_gamma[idx];
					lut[256 + i] = green_gamma[idx];
					lut[512 + i] = blue_gamma[idx];
				}
			}
		}
	}

	int32_t err = dmc_record_driver_gamma_change(lut);
	/* kDMCDriverGammaDeferred: a DSp fade is in progress - the fade's
	 * end-state push delivers this table, so do NOT pop it onto the
	 * faded screen here. */
	if (err == kDMCNoErr || err == kDMCErrNotInitialized) {
		MetalCompositorUpdateGammaLUT(lut);
	}
}
#endif

static int16 set_gamma(VidLocals *csSave, uint32 gamma)
{
	/* The Linear gamma pref must not bypass user-supplied tables here: games
	 * install functional ramps (e.g. overbright brightness doubling) that the
	 * compositor presents verbatim in Linear mode. The pref only controls the
	 * classic-Mac -> sRGB display correction (see gfx_color_policy.h). */
	if (gamma == 0) { // Build linear ramp, 256 entries

		// Allocate new table, if necessary
		if (!allocate_gamma_table(csSave, SIZEOF_GammaTbl + 256))
			return memFullErr;

		// Initialize header
		WriteMacInt16(csSave->gammaTable + gVersion, 0);		// A  version 0 style of the GammaTbl structure
		WriteMacInt16(csSave->gammaTable + gType, 0);			// Frame buffer hardware invariant
		WriteMacInt16(csSave->gammaTable + gFormulaSize, 0);	// No formula data, just correction data
		WriteMacInt16(csSave->gammaTable + gChanCnt, 1);		// Apply same correction to Red, Green, & Blue
		WriteMacInt16(csSave->gammaTable + gDataCnt, 256);		// gDataCnt == 2^^gDataWidth
		WriteMacInt16(csSave->gammaTable + gDataWidth, 8);		// 8 bits of significant data per entry

		// Build the linear ramp
		uint32 p = csSave->gammaTable + gFormulaData;

		for (int i=0; i<256; i++) {
#if TARGET_OS_IPHONE
			/* Real video card ROMs install their 'gama' resource ("Mac
			 * Standard Gamma") as the power-on default; the Display Manager
			 * trusts that at boot and only pushes the display profile's
			 * table during a resolution/depth switch. A linear default here
			 * therefore leaves the screen uncorrected from boot until the
			 * first mode switch - which never comes when the guest boots at
			 * the panel-native resolution in full screen. Default to the
			 * same classic-Mac standard curve the profile table carries so
			 * boot matches the post-switch presentation. */
			const uint8 v = GfxColorClassicMacToSRGBByte((uint8)i);
#else
			const uint8 v = (uint8)i; // linear ramp (legacy host gamma path)
#endif
			WriteMacInt8(p + i, v);
			mac_gamma[i].red = mac_gamma[i].green = mac_gamma[i].blue = v;
		}
		video_set_gamma(256);
#if (defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) || TARGET_OS_IPHONE
		publish_gamma_lut_to_display_controller(csSave);
#endif
	} else { // User-supplied gamma table

		// Validate header
		if (ReadMacInt16(gamma + gVersion) != 0)
			return paramErr;
		if (ReadMacInt16(gamma + gType) != 0)
			return paramErr;
		int chan_cnt = ReadMacInt16(gamma + gChanCnt);
		if (chan_cnt != 1 && chan_cnt != 3)
			return paramErr;
		int data_width = ReadMacInt16(gamma + gDataWidth);
		if (data_width > 8)
			return paramErr;
		int data_cnt = ReadMacInt16(gamma + gDataCnt);
		if (data_cnt != (1 << data_width))
			return paramErr;

		// Allocate new table, if necessary
		int size = SIZEOF_GammaTbl + ReadMacInt16(gamma + gFormulaSize) + chan_cnt * data_cnt;
		if (!allocate_gamma_table(csSave, size))
			return memFullErr;

		// Copy table
		Mac2Mac_memcpy(csSave->gammaTable, gamma, size);

		// Save new gamma data for video impl
		if (data_width != 8) {
			// FIXME: handle bit-packed data
		} else {
			uint32 p = csSave->gammaTable + gFormulaData + gFormulaSize;

			uint32 p_red;
			uint32 p_green;
			uint32 p_blue;

			// make values increasing as some implementations really don't like it when gamma tables aren't
			uint8 max_red = 0;
			uint8 max_green = 0;
			uint8 max_blue = 0;

			if (chan_cnt == 3) {
				p_red = p;
				p_green = p + data_cnt;
				p_blue = p + data_cnt * 2;
			} else {
				p_red = p_green = p_blue = p;
			}
			for (int i=0; i < data_cnt; i++) {
				max_red = max(max_red, ReadMacInt8(p_red++));
				max_green = max(max_green, ReadMacInt8(p_green++));
				max_blue = max(max_blue, ReadMacInt8(p_blue++));
				mac_gamma[i].red = max_red;
				mac_gamma[i].green = max_green;
				mac_gamma[i].blue = max_blue;
			}
		}
		video_set_gamma(data_cnt);
#if (defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) || TARGET_OS_IPHONE
		publish_gamma_lut_to_display_controller(csSave);
#endif
	}
	return noErr;
}

static int16 VideoControl(uint32 pb, VidLocals *csSave)
{
	int16 code = ReadMacInt16(pb + csCode);
	D(bug("VideoControl %d: ", code));
	uint32 param = ReadMacInt32(pb + csParam);
	switch (code) {

		case cscReset:									// VidReset
			D(bug("VidReset\n"));
			return controlErr;

		case cscKillIO:									// VidKillIO
			D(bug("VidKillIO\n"));
			return controlErr;

		case cscSetMode:								// SetVidMode
			D(bug("SetVidMode\n"));
			D(bug("mode:%04x page:%04x \n", ReadMacInt16(param + csMode),
				ReadMacInt16(param + csPage)));
			WriteMacInt32(param + csData, csSave->saveData);
			return video_mode_change(csSave, param);

		case cscSetEntries: {							// SetEntries
			D(bug("SetEntries\n"));
			if (VModes[cur_mode].viAppleMode > APPLE_8_BIT) return controlErr;
			uint32 s_pal = ReadMacInt32(param + csTable);
			uint16 start = ReadMacInt16(param + csStart);
			uint16 count = ReadMacInt16(param + csCount);
			if (s_pal == 0 || count > 256) return controlErr;

			// Preparations for gamma correction
			bool do_gamma = false;
			uint8 *red_gamma = NULL;
			uint8 *green_gamma = NULL;
			uint8 *blue_gamma = NULL;
			int gamma_data_width = 0;
			if (csSave->gammaTable) {
#ifdef __BEOS__
				// Windows are gamma-corrected by BeOS
				const bool can_do_gamma = (display_type == DIS_SCREEN);
#elif (defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) || TARGET_OS_IPHONE
				/* The compositor gamma LUT is the single owner of driver
				 * gamma when gfxaccel is enabled: publish_gamma_lut_to_display_controller
				 * delivers the guest table to the GPU present path, and
				 * compositor_fragment_indexed applies it after the palette
				 * lookup. Baking it into mac_pal here as well would apply
				 * the table twice on indexed paths (see gfx_color_policy.h). */
				const bool can_do_gamma = false;
#else
				const bool can_do_gamma = true;
#endif
				if (can_do_gamma) {
					uint32 gamma_table = csSave->gammaTable;
					red_gamma = Mac2HostAddr(gamma_table + gFormulaData + ReadMacInt16(gamma_table + gFormulaSize));
					int chan_cnt = ReadMacInt16(gamma_table + gChanCnt);
					if (chan_cnt == 1)
						green_gamma = blue_gamma = red_gamma;
					else {
						int ofs = ReadMacInt16(gamma_table + gDataCnt);
						green_gamma = red_gamma + ofs;
						blue_gamma = green_gamma + ofs;
					}
					gamma_data_width = ReadMacInt16(gamma_table + gDataWidth);
					do_gamma = true;
				}
			}

			// Set palette
			rgb_color *d_pal;
			if (start == 0xffff) {			// Indexed
				for (int i=0; i<=count; i++) {
					d_pal = mac_pal + (ReadMacInt16(s_pal + csValue) & 0xff);
					uint8 red = (uint16)ReadMacInt16(s_pal + csRed) >> 8;
					uint8 green = (uint16)ReadMacInt16(s_pal + csGreen) >> 8;
					uint8 blue = (uint16)ReadMacInt16(s_pal + csBlue) >> 8;
					if (csSave->luminanceMapping)
						red = green = blue = (red * 0x4ccc + green * 0x970a + blue * 0x1c29) >> 16;
					if (do_gamma) {
						red = red_gamma[red >> (8 - gamma_data_width)];
						green = green_gamma[green >> (8 - gamma_data_width)];
						blue = blue_gamma[blue >> (8 - gamma_data_width)];
					}
					(*d_pal).red = red;
					(*d_pal).green = green;
					(*d_pal).blue = blue;
					s_pal += 8;
				}
			} else {						// Sequential
				d_pal = mac_pal + start;
				for (int i=0; i<=count; i++) {
					uint8 red = (uint16)ReadMacInt16(s_pal + csRed) >> 8;
					uint8 green = (uint16)ReadMacInt16(s_pal + csGreen) >> 8;
					uint8 blue = (uint16)ReadMacInt16(s_pal + csBlue) >> 8;
					if (csSave->luminanceMapping)
						red = green = blue = (red * 0x4ccc + green * 0x970a + blue * 0x1c29) >> 16;
					if (do_gamma) {
						red = red_gamma[red >> (8 - gamma_data_width)];
						green = green_gamma[green >> (8 - gamma_data_width)];
						blue = blue_gamma[blue >> (8 - gamma_data_width)];
					}
					(*d_pal).red = red;
					(*d_pal).green = green;
					(*d_pal).blue = blue;
					d_pal++;
					s_pal += 8;
				}
			}
			video_log_palette_summary("SetEntries", start, count);
			video_set_palette();
			return noErr;
		}

		case cscSetGamma: {							// SetGamma
			uint32 user_table = ReadMacInt32(param + csGTable);
			D(bug("SetGamma %08x\n", user_table));
			return set_gamma(csSave, user_table);
		}

		case cscGrayPage: {							// GrayPage
			D(bug("GrayPage %d\n", ReadMacInt16(param + csPage)));
			if (ReadMacInt16(param + csPage))
				return paramErr;

			uint32 pattern[6] = {
				0xaaaaaaaa,		// 1 bpp
				0xcccccccc,		// 2 bpp
				0xf0f0f0f0,		// 4 bpp
				0xff00ff00,		// 8 bpp
				0xffff0000,		// 16 bpp
				0xffffffff		// 32 bpp
			};
			uint32 p = csSave->saveBaseAddr;
			uint32 pat = pattern[VModes[cur_mode].viAppleMode - APPLE_1_BIT];
			bool invert = (VModes[cur_mode].viAppleMode == APPLE_32_BIT);
			for (uint32 y=0; y<VModes[cur_mode].viYsize; y++) {
				for (uint32 x=0; x<VModes[cur_mode].viRowBytes; x+=4) {
					WriteMacInt32(p + x, pat);
					if (invert)
						pat = ~pat;
				}
				p += VModes[cur_mode].viRowBytes;
				pat = ~pat;
			}
			return noErr;
		}

		case cscSetGray:							// SetGray
			D(bug("SetGray %02x\n", ReadMacInt8(param)));
			csSave->luminanceMapping = ReadMacInt8(param);
			return noErr;

		case cscSetInterrupt:						// SetInterrupt
			D(bug("SetInterrupt\n"));
			csSave->interruptsEnabled = !ReadMacInt8(param);
			return noErr;

		case cscDirectSetEntries:					// DirectSetEntries
			D(bug("DirectSetEntries\n"));
			video_log_palette_summary("DirectSetEntries-unimplemented", 0, 0);
			return controlErr;

		case cscSetDefaultMode:						// SetDefaultMode
			D(bug("SetDefaultMode\n"));
			return controlErr;

		case cscSwitchMode:
			D(bug("cscSwitchMode (Display Manager support) \nMode:%02x ID:%04x Page:%d\n",
			  ReadMacInt16(param + csMode), ReadMacInt32(param + csData), ReadMacInt16(param + csPage)));
			return video_mode_change(csSave, param);

		case cscSavePreferredConfiguration:
			D(bug("SavePreferredConfiguration\n"));
			save_conf_id = ReadMacInt32(param + csData);
			save_conf_mode = ReadMacInt16(param + csMode);
			return noErr;

		case cscSetHardwareCursor: {
//			D(bug("SetHardwareCursor\n"));

			if (!csSave->cursorHardware)
				return controlErr;

			csSave->cursorSet = false;
			bool changed = false;

			// Image
			uint32 cursor = ReadMacInt32(param);	// Pointer to CursorImage
			uint32 pmhandle = ReadMacInt32(cursor + ciCursorPixMap);
			if (pmhandle == 0 || ReadMacInt32(pmhandle) == 0)
				return controlErr;
			uint32 pixmap = ReadMacInt32(pmhandle);

			// XXX: only certain image formats are handled properly at the moment
			uint16 rowBytes = ReadMacInt16(pixmap + 4) & 0x7FFF;
			if (rowBytes != 2)
				return controlErr;

			// Mask
			uint32 bmhandle = ReadMacInt32(cursor + ciCursorBitMask);
			if (bmhandle == 0 || ReadMacInt32(bmhandle) == 0)
				return controlErr;
			uint32 bitmap = ReadMacInt32(bmhandle);

			// Get cursor data even on a screen, to set the right cursor image when switching back to a window.
			// Hotspot is stale, but will be fixed by the next call to DrawHardwareCursor, which is likely to
			// occur immediately hereafter.

			if (memcmp(MacCursor + 4, Mac2HostAddr(ReadMacInt32(pixmap)), 32)) {
				memcpy(MacCursor + 4, Mac2HostAddr(ReadMacInt32(pixmap)), 32);
				changed = true;
			}
			if (memcmp(MacCursor + 4 + 32, Mac2HostAddr(ReadMacInt32(bitmap)), 32)) {
				memcpy(MacCursor + 4 + 32, Mac2HostAddr(ReadMacInt32(bitmap)), 32);
				changed = true;
			}

			// Set new cursor image
			if (!video_can_change_cursor())
				return controlErr;
			if (changed)
				video_set_cursor();

			csSave->cursorSet = true;
			csSave->cursorHotFlag = true;
			return noErr;
		}

		case cscDrawHardwareCursor: {
//			D(bug("DrawHardwareCursor\n"));

			if (!csSave->cursorHardware)
				return controlErr;

			int32 oldX = csSave->cursorX;
			int32 oldY = csSave->cursorY;
			uint32 oldVisible = csSave->cursorVisible;

			csSave->cursorX = ReadMacInt32(param + csCursorX);
			csSave->cursorY = ReadMacInt32(param + csCursorY);
			csSave->cursorVisible = ReadMacInt32(param + csCursorVisible);
			bool changed = (csSave->cursorVisible != oldVisible);

			// If this is the first DrawHardwareCursor call since the cursor was last set (via SetHardwareCursor),
			// attempt to set an appropriate cursor hotspot.  SetHardwareCursor itself does not know what the
			// hotspot should be; it knows only the cursor image and mask.  The hotspot is known only to the caller,
			// and we have to try to infer it here.  The usual sequence of calls when changing the cursor is:
			//
			//	DrawHardwareCursor with (oldX, oldY, invisible)
			//	SetHardwareCursor with (cursor)
			//	DrawHardwareCursor with (newX, newY, visible)
			//
			// The key thing to note is that the sequence is intended not to change the current screen pixel location
			// indicated by the hotspot.  Thus, the difference between (newX, newY) and (oldX, oldY) reflects precisely
			// the difference between the old cursor hotspot and the new one.  For example, if you change from a
			// cursor whose hotspot is (1, 1) to one whose hotspot is (7, 4), then you must adjust the cursor position
			// by (-6, -3) in order for the same screen pixel to remain under the new hotspot.
			//
			// Alas, on rare occasions this heuristic can fail, and if you did nothing else you could even get stuck
			// with the wrong hotspot from then on.  To address that possibility, we force the hotspot to (1, 1)
			// whenever the cursor being drawn is the standard arrow.  Thus, while it is very unlikely that you will
			// ever have the wrong hotspot, if you do, it is easy to recover.

			if (csSave->cursorHotFlag) {
				csSave->cursorHotFlag = false;
				D(bug("old hotspot (%d, %d)\n", csSave->cursorHotX, csSave->cursorHotY));

				static uint8 arrow[] = {
					0x00, 0x00, 0x40, 0x00, 0x60, 0x00, 0x70, 0x00, 0x78, 0x00, 0x7C, 0x00, 0x7E, 0x00, 0x7F, 0x00,
					0x7F, 0x80, 0x7C, 0x00, 0x6C, 0x00, 0x46, 0x00, 0x06, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00,
				};
				if (memcmp(MacCursor + 4, arrow, 32) == 0) {
					csSave->cursorHotX = 1;
					csSave->cursorHotY = 1;
				} else if (csSave->cursorX != oldX || csSave->cursorY != oldY) {
					int32 hotX = csSave->cursorHotX + (oldX - csSave->cursorX);
					int32 hotY = csSave->cursorHotY + (oldY - csSave->cursorY);

					if (0 <= hotX && hotX <= 15 && 0 <= hotY && hotY <= 15) {
						csSave->cursorHotX = hotX;
						csSave->cursorHotY = hotY;
					}
				}
				if (MacCursor[2] != csSave->cursorHotX || MacCursor[3] != csSave->cursorHotY) {
					MacCursor[2] = csSave->cursorHotX;
					MacCursor[3] = csSave->cursorHotY;
					changed = true;
				}
				D(bug("new hotspot (%d, %d)\n", csSave->cursorHotX, csSave->cursorHotY));
			}

			if (changed && video_can_change_cursor())
				video_set_cursor();

			return noErr;
		}

		case 43: {	// Driver Gestalt
			uint32 sel = ReadMacInt32(pb + csParam);
			D(bug(" driver gestalt %c%c%c%c\n", sel >> 24, sel >> 16,  sel >> 8, sel));
			switch (sel) {
				case FOURCC('v','e','r','s'):
					WriteMacInt32(pb + csParam + 4, 0x01008000);
					break;
				case FOURCC('i','n','t','f'):
					WriteMacInt32(pb + csParam + 4, FOURCC('c','a','r','d'));
					break;
				case FOURCC('s','y','n','c'):
					WriteMacInt32(pb + csParam + 4, 0x01000000);
					break;
				default:
					return statusErr;
			};
			return noErr;
		}

		default:
			D(bug(" unknown control code %d\n", code));
			return controlErr;
	}
}


/*
 *  Video driver status routine
 */

// Search for given AppleID in mode table
static bool has_mode(uint32 id)
{
	VideoInfo *p = VModes;
	while (p->viType != DIS_INVALID) {
		if (p->viAppleID == id)
			return true;
		p++;
	}
	return false;
}

/*
 *  DepthMode translation at the driver ABI. The Display Manager numbers a
 *  display's depth modes RELATIVE to what the driver supports: kDepthMode1
 *  (0x80, firstVidMode) is always the LOWEST supported depth. VModes stores
 *  absolute APPLE_*_BIT constants; while the driver advertised every depth
 *  1..32bpp the two numberings coincided, but with a trimmed depth set the
 *  relative names must be translated. Absolute values are still accepted on
 *  input for back-compat with guest state saved by older builds.
 */
static int video_depth_list(uint32 depths[6])
{
	int n = 0;
	for (VideoInfo *p = VModes; p->viType != DIS_INVALID; p++) {
		int i = 0;
		while (i < n && depths[i] != p->viAppleMode) i++;
		if (i == n && n < 6) depths[n++] = p->viAppleMode;
	}
	// insertion sort ascending (APPLE_*_BIT constants order by depth)
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && depths[j-1] > depths[j]; j--) {
			uint32 t = depths[j];
			depths[j] = depths[j-1];
			depths[j-1] = t;
		}
	return n;
}

uint32 video_abs_depth_from_rel(uint16 rel)
{
	uint32 depths[6];
	int n = video_depth_list(depths);
	int idx = (int)rel - (int)firstVidMode;
	if (idx >= 0 && idx < n) return depths[idx];
	return 0;
}

uint16 video_rel_depth_from_abs(uint32 abs)
{
	uint32 depths[6];
	int n = video_depth_list(depths);
	for (int i = 0; i < n; i++)
		if (depths[i] == abs) return (uint16)(firstVidMode + i);
	return (uint16)abs;	// unknown: report unchanged
}

#if defined(ENABLE_GFXACCEL)
static bool video_guest_address_contains(uint32 addr, uint32 size)
{
	if (addr == 0 || size == 0)
		return false;
	const uint64 start = (uint64)addr;
	const uint64 end = start + size;
	if (end < start)
		return false;
	if (end <= 0x3000)
		return true;
	const uint64 ram_lo = (uint64)(uint32)RAMBase;
	const uint64 ram_hi = (uint64)(uint32)(RAMBase + RAMSize);
	return start >= ram_lo && end <= ram_hi;
}

static bool video_guest_address_contains(uint32 addr, uint32 offset,
										 uint32 size)
{
	return addr != 0 && addr <= UINT32_MAX - offset &&
		video_guest_address_contains(addr + offset, size);
}

bool video_prepare_guest_display(void)
{
	if (guest_dm_set_display_mode == 0)
		guest_dm_set_display_mode =
			FindLibSymbol("\014InterfaceLib", "\020DMSetDisplayMode");
	if (guest_set_entries == 0)
		guest_set_entries =
			FindLibSymbol("\014InterfaceLib", "\012SetEntries");
	if (guest_new_cwindow == 0)
		guest_new_cwindow =
			FindLibSymbol("\014InterfaceLib", "\012NewCWindow");
	if (guest_dispose_window == 0)
		guest_dispose_window =
			FindLibSymbol("\014InterfaceLib", "\015DisposeWindow");
	if (guest_new_rgn == 0)
		guest_new_rgn =
			FindLibSymbol("\014InterfaceLib", "\006NewRgn");
	if (guest_rect_rgn == 0)
		guest_rect_rgn =
			FindLibSymbol("\014InterfaceLib", "\007RectRgn");
	if (guest_paint_one == 0)
		guest_paint_one =
			FindLibSymbol("\014InterfaceLib", "\010PaintOne");
	if (guest_dispose_rgn == 0)
		guest_dispose_rgn =
			FindLibSymbol("\014InterfaceLib", "\012DisposeRgn");
	if (guest_hide_cursor == 0)
		guest_hide_cursor =
			FindLibSymbol("\014InterfaceLib", "\012HideCursor");
	if (guest_show_cursor == 0)
		guest_show_cursor =
			FindLibSymbol("\014InterfaceLib", "\012ShowCursor");
	return guest_dm_set_display_mode != 0;
}

void video_screen_publish_cm_suspend(void)
{
	while (s_screen_publish_cm_depth > 0) {
		s_screen_publish_cm_depth--;
		if (s_screen_publish_cm_depth == 0) {
			if (guest_show_cursor == 0)
				(void)video_prepare_guest_display();
			if (guest_show_cursor != 0)
				(void)call_macos(guest_show_cursor);
		}
	}
	s_screen_publish_cm_enabled = false;
}

void video_screen_publish_cm_resume(void)
{
	s_screen_publish_cm_enabled = true;
}

void video_screen_publish_begin(int left, int top, int right, int bottom)
{
	(void)left;
	(void)top;
	(void)right;
	(void)bottom;
	if (!s_screen_publish_cm_enabled || video_can_change_cursor())
		return;

	(void)video_prepare_guest_display();
	if (guest_hide_cursor == 0)
		return;

	if (s_screen_publish_cm_depth == 0)
		(void)call_macos(guest_hide_cursor);
	s_screen_publish_cm_depth++;
}

void video_screen_publish_end(void)
{
	if (s_screen_publish_cm_depth <= 0)
		return;
	s_screen_publish_cm_depth--;
	if (s_screen_publish_cm_depth != 0)
		return;
	if (!s_screen_publish_cm_enabled)
		return;

	if (guest_show_cursor == 0)
		(void)video_prepare_guest_display();
	if (guest_show_cursor != 0)
		(void)call_macos(guest_show_cursor);
}

uint32 video_create_guest_fullscreen_window(uint32 width, uint32 height)
{
	(void)video_prepare_guest_display();
	if (guest_new_cwindow == 0 || width == 0 || height == 0 ||
		width > 0x7fffu || height > 0x7fffu)
		return 0;

	SheepVar scratch(12);
	const uint32 rect = scratch.addr();
	const uint32 title = rect + 8;
	WriteMacInt16(rect + 0, 0);
	WriteMacInt16(rect + 2, 0);
	WriteMacInt16(rect + 4, (uint16)height);
	WriteMacInt16(rect + 6, (uint16)width);
	WriteMacInt8(title, 0);

	/* A visible frontmost plainDBox supplies Window Manager ordering only.
	 * Its pixels still live in the canonical screen and are immediately
	 * replaced by the accelerated producer. Disposing it after mode restore
	 * exposes the underlying desktop through the normal guest update path. */
	return call_macos8(
		guest_new_cwindow,
		0, rect, title, 1, 2, 0xffffffffu, 0, 0);
}

bool video_dispose_guest_window(uint32 window)
{
	if (window == 0)
		return true;
	(void)video_prepare_guest_display();
	if (guest_dispose_window == 0)
		return false;
	(void)call_macos1(guest_dispose_window, window);

	/* DisposeWindow exposes the pixels, but accelerated rendering bypasses
	 * the Window Manager's normal damage bookkeeping. Paint the exposed
	 * desktop through the guest Window Manager so its DeskHook redraws icons
	 * and ordinary windows receive update regions. This restores guest UI
	 * state; it is not a saved framebuffer copy. */
	if (guest_new_rgn != 0 && guest_rect_rgn != 0 &&
		guest_paint_one != 0 && guest_dispose_rgn != 0 &&
		cur_mode >= 0 && cur_mode < 64 &&
		VModes[cur_mode].viType != DIS_INVALID &&
		VModes[cur_mode].viXsize <= 0x7fffu &&
		VModes[cur_mode].viYsize <= 0x7fffu) {
		const uint32 exposed = call_macos(guest_new_rgn);
		if (exposed != 0) {
			SheepVar rect_storage(8);
			const uint32 rect = rect_storage.addr();
			WriteMacInt16(rect + 0, 0);
			WriteMacInt16(rect + 2, 0);
			WriteMacInt16(rect + 4, VModes[cur_mode].viYsize);
			WriteMacInt16(rect + 6, VModes[cur_mode].viXsize);
			(void)call_macos2(guest_rect_rgn, exposed, rect);
			(void)call_macos2(guest_paint_one, 0, exposed);
			(void)call_macos1(guest_dispose_rgn, exposed);
		}
	}
	return true;
}

uint32 video_get_live_main_device_pixmap(void)
{
	const uint32 main_device_handle = ReadMacInt32(LMADDR_MAIN_DEVICE);
	if (!video_guest_address_contains(main_device_handle, 4))
		return 0;

	const uint32 gdevice = ReadMacInt32(main_device_handle);
	if (!video_guest_address_contains(gdevice, GDEVICE_OFF_PMAP, 4))
		return 0;

	const uint32 pixmap_handle =
		ReadMacInt32(gdevice + GDEVICE_OFF_PMAP);
	if (!video_guest_address_contains(pixmap_handle, 4))
		return 0;

	const uint32 pixmap = ReadMacInt32(pixmap_handle);
	return video_guest_address_contains(
		pixmap, DSP_MAINDEVICE_PIXMAP_OFF_CMPSIZE, 2) ? pixmap : 0;
}

static uint32 video_mode_pixel_depth(int mode_index)
{
	if (mode_index < 0 || mode_index >= 64 ||
		VModes[mode_index].viType == DIS_INVALID)
		return 0;
	const uint32 apple_mode = VModes[mode_index].viAppleMode;
	if (apple_mode < APPLE_1_BIT || apple_mode > APPLE_32_BIT)
		return 0;
	return 1u << (apple_mode - APPLE_1_BIT);
}

int video_find_guest_mode(uint32 width, uint32 height, uint32 depth)
{
	const uint32 apple_mode = (uint32)DepthModeForPixelDepth((int)depth);
	for (int i = 0; i < 64 && VModes[i].viType != DIS_INVALID; i++) {
		if (VModes[i].viXsize == width &&
			VModes[i].viYsize == height &&
			VModes[i].viAppleMode == apple_mode)
			return i;
	}
	return -1;
}

bool video_capture_guest_clut(uint8 out_clut[768], uint32 *out_depth)
{
	if (out_clut == NULL)
		return false;
	const uint32 depth = video_mode_pixel_depth(cur_mode);
	if (depth == 0 || depth > 8)
		return false;
	for (uint32 i = 0; i < 256; i++) {
		out_clut[i * 3 + 0] = mac_pal[i].red;
		out_clut[i * 3 + 1] = mac_pal[i].green;
		out_clut[i * 3 + 2] = mac_pal[i].blue;
	}
	if (out_depth != NULL)
		*out_depth = depth;
	return true;
}

static bool video_guest_device_matches_mode(int mode_index)
{
	if (mode_index < 0 || mode_index >= 64 ||
		VModes[mode_index].viType == DIS_INVALID ||
		cur_mode != mode_index ||
		screen_base == 0 || Mac2HostAddr(screen_base) == NULL)
		return false;

	const uint32 main_device = ReadMacInt32(LMADDR_MAIN_DEVICE);
	if (!video_guest_address_contains(main_device, 4))
		return false;
	const uint32 gdevice = ReadMacInt32(main_device);
	if (!video_guest_address_contains(gdevice, GDEVICE_OFF_GDMODE, 4))
		return false;

	const uint32 pixmap = video_get_live_main_device_pixmap();
	if (pixmap == 0)
		return false;

	const VideoInfo &mode = VModes[mode_index];
	const uint16 relative_depth =
		video_rel_depth_from_abs(mode.viAppleMode);
	const uint32 row_bytes =
		(uint32)ReadMacInt16(
			pixmap + DSP_MAINDEVICE_PIXMAP_OFF_ROWBYTES) & 0x3fff;

	return ReadMacInt32(gdevice + GDEVICE_OFF_GDMODE) == relative_depth &&
		ReadMacInt16(gdevice + GDEVICE_OFF_GDRECT + 0) == 0 &&
		ReadMacInt16(gdevice + GDEVICE_OFF_GDRECT + 2) == 0 &&
		(uint16)ReadMacInt16(gdevice + GDEVICE_OFF_GDRECT + 4) ==
			mode.viYsize &&
		(uint16)ReadMacInt16(gdevice + GDEVICE_OFF_GDRECT + 6) ==
			mode.viXsize &&
		ReadMacInt32(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BASEADDR) ==
			screen_base &&
		row_bytes == mode.viRowBytes &&
		ReadMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_TOP) == 0 &&
		ReadMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_LEFT) == 0 &&
		(uint16)ReadMacInt16(
			pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_BOT) ==
			mode.viYsize &&
		(uint16)ReadMacInt16(
			pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_RIGHT) ==
			mode.viXsize &&
		(uint16)ReadMacInt16(
			pixmap + DSP_MAINDEVICE_PIXMAP_OFF_PIXELSIZE) ==
			video_mode_pixel_depth(mode_index);
}

bool video_switch_guest_display(int mode_index)
{
	if (mode_index < 0 || mode_index >= 64 ||
		VModes[mode_index].viType == DIS_INVALID ||
		!video_prepare_guest_display())
		return false;

	const uint32 main_device = ReadMacInt32(LMADDR_MAIN_DEVICE);
	if (!video_guest_address_contains(main_device, 4))
		return false;

	SheepVar request(16);
	const uint32 switch_info = request.addr();
	const uint32 depth_mode = switch_info + 12;
	const VideoInfo &mode = VModes[mode_index];
	const uint16 relative_depth =
		video_rel_depth_from_abs(mode.viAppleMode);

	WriteMacInt16(switch_info + csMode, relative_depth);
	WriteMacInt32(switch_info + csData, mode.viAppleID);
	WriteMacInt16(switch_info + csPage, 0);
	WriteMacInt32(switch_info + csBaseAddr, 0);
	WriteMacInt32(depth_mode, relative_depth);

	const int32 err = (int32)call_macos5(
		guest_dm_set_display_mode,
		main_device,
		mode.viAppleID,
		depth_mode,
		switch_info,
		0);
	return err == noErr && video_guest_device_matches_mode(mode_index);
}

bool video_install_guest_clut(const uint8 clut_rgb[768],
							  uint32 depth_bits,
							  uint32 first,
							  uint32 last)
{
	if (clut_rgb == NULL || first > last || last > 255 ||
		(depth_bits != 1 && depth_bits != 2 &&
		 depth_bits != 4 && depth_bits != 8))
		return false;

	const uint32 color_count = 1u << depth_bits;
	if (first >= color_count)
		return true;
	if (last >= color_count)
		last = color_count - 1;

	(void)video_prepare_guest_display();
	if (guest_set_entries == 0)
		return false;

	const uint32 entry_count = last - first + 1;
	SheepVar color_specs(entry_count * 8);
	const uint32 specs = color_specs.addr();
	for (uint32 i = 0; i < entry_count; i++) {
		const uint32 palette_index = first + i;
		const uint32 src = palette_index * 3;
		const uint32 dst = specs + i * 8;
		WriteMacInt16(dst + 0, (uint16)palette_index);
		WriteMacInt16(dst + 2, (uint16)(clut_rgb[src + 0] * 0x0101));
		WriteMacInt16(dst + 4, (uint16)(clut_rgb[src + 1] * 0x0101));
		WriteMacInt16(dst + 6, (uint16)(clut_rgb[src + 2] * 0x0101));
	}
	(void)call_macos3(guest_set_entries, first, entry_count - 1, specs);
	return true;
}

#else
bool video_prepare_guest_display(void) { return false; }
int video_find_guest_mode(uint32, uint32, uint32) { return -1; }
bool video_switch_guest_display(int) { return false; }
bool video_capture_guest_clut(uint8[768], uint32 *) { return false; }
bool video_install_guest_clut(const uint8[768], uint32, uint32, uint32)
{
	return false;
}
uint32 video_get_live_main_device_pixmap(void) { return 0; }
uint32 video_create_guest_fullscreen_window(uint32, uint32) { return 0; }
bool video_dispose_guest_window(uint32) { return false; }
void video_screen_publish_cm_suspend(void) {}
void video_screen_publish_cm_resume(void) {}
void video_screen_publish_begin(int, int, int, int) {}
void video_screen_publish_end(void) {}
#endif

static uint16 video_rel_max_depth_mode(void)
{
	uint32 depths[6];
	int n = video_depth_list(depths);
	return (uint16)(firstVidMode + (n > 0 ? n - 1 : 0));
}

// Get X/Y size of specified resolution
static void get_size_of_resolution(int id, uint32 &x, uint32 &y)
{
	VideoInfo *p = VModes;
	while (p->viType != DIS_INVALID) {
		if (p->viAppleID == id) {
			x = p->viXsize;
			y = p->viYsize;
			return;
		}
		p++;
	}
	x = y = 0;
}

#if (defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) || TARGET_OS_IPHONE
static bool get_dsp_video_status_override(uint16 &mode, uint32 &data)
{
	const DMCModeSnapshot *snap = dmc_current_snapshot();
	return DSpVideoStatusForSnapshot(snap, VModes, &mode, &data);
}

static void log_dsp_video_status_override(const char *selector, uint16 mode, uint32 data)
{
	const int depth = (mode >= APPLE_1_BIT && mode <= APPLE_32_BIT) ?
	    (1 << (mode - APPLE_1_BIT)) : 0;
	printf("VideoStatus %s: DSp override mode=0x%04x depth=%d id=0x%08x\n",
	       selector, mode, depth, data);
}
#endif

static int16 VideoStatus(uint32 pb, VidLocals *csSave)
{
	int16 code = ReadMacInt16(pb + csCode);
	D(bug("VideoStatus %d: ", code));
	uint32 param = ReadMacInt32(pb + csParam);
	switch (code) {

		case cscGetMode: {							// GetMode
			D(bug("GetMode\n"));
			uint16 mode = csSave->saveMode;
			uint32 data = csSave->saveData;
#if (defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) || TARGET_OS_IPHONE
			if (get_dsp_video_status_override(mode, data))
				log_dsp_video_status_override("GetMode", mode, data);
#endif
			WriteMacInt32(param + csBaseAddr, csSave->saveBaseAddr);
			WriteMacInt16(param + csMode, video_rel_depth_from_abs(mode));
			WriteMacInt16(param + csPage, csSave->savePage);
			D(bug("return: mode:%04x page:%04x ", ReadMacInt16(param + csMode),
				ReadMacInt16(param + csPage)));
			D(bug("base adress %08lx\n", ReadMacInt32(param + csBaseAddr)));
			return noErr;
		}

		case cscGetEntries: {						// GetEntries
			D(bug("GetEntries\n"));
			uint32 d_pal = ReadMacInt32(param + csTable);
			uint16 start = ReadMacInt16(param + csStart);
			uint16 count = ReadMacInt16(param + csCount);
			rgb_color *s_pal;
			if ((VModes[cur_mode].viAppleMode == APPLE_32_BIT)||
				(VModes[cur_mode].viAppleMode == APPLE_16_BIT)) {
				D(bug("ERROR: GetEntries in direct mode \n"));
				return statusErr;
			}

			if (start == 0xffff) {		// Indexed
				for (uint16 i=0;i<count;i++) {
					s_pal = mac_pal + (ReadMacInt16(d_pal + csValue) & 0xff);
					uint8 red = (*s_pal).red;
					uint8 green = (*s_pal).green;
					uint8 blue = (*s_pal).blue;
					WriteMacInt16(d_pal + csRed, red * 0x0101);
					WriteMacInt16(d_pal + csGreen, green * 0x0101);
					WriteMacInt16(d_pal + csBlue, blue * 0x0101);
					d_pal += 8;
				}
			} else {					// Sequential
				if (start + count > 255)
					return paramErr;
				s_pal = mac_pal + start;
				for (uint16 i=0;i<count;i++) {
					uint8 red = (*s_pal).red;
					uint8 green = (*s_pal).green;
					uint8 blue = (*s_pal).blue;
					s_pal++;
					WriteMacInt16(d_pal + csRed, red * 0x0101);
					WriteMacInt16(d_pal + csGreen, green * 0x0101);
					WriteMacInt16(d_pal + csBlue, blue * 0x0101);
					d_pal += 8;
				}
			};
			return noErr;
		}

		case cscGetPageCnt:						// GetPage
			D(bug("GetPage\n"));
			WriteMacInt16(param + csPage, 1);
			return noErr;

		case cscGetPageBase:						// GetPageBase
			D(bug("GetPageBase\n"));
			WriteMacInt32(param + csBaseAddr, csSave->saveBaseAddr);
			return noErr;

		case cscGetGray:							// GetGray
			D(bug("GetGray\n"));
			WriteMacInt8(param, csSave->luminanceMapping ? 1 : 0);
			return noErr;

		case cscGetInterrupt:						// GetInterrupt
			D(bug("GetInterrupt\n"));
			WriteMacInt8(param, csSave->interruptsEnabled ? 0 : 1);
			return noErr;

		case cscGetGamma:							// GetGamma
			D(bug("GetGamma\n"));
			WriteMacInt32(param, (uint32)csSave->gammaTable);
			return noErr;

		case cscGetDefaultMode:						// GetDefaultMode
			D(bug("GetDefaultMode\n"));
			return statusErr;

		case cscGetCurMode: {						// GetCurMode
			D(bug("GetCurMode\n"));
			uint16 mode = csSave->saveMode;
			uint32 data = csSave->saveData;
#if (defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) || TARGET_OS_IPHONE
			if (get_dsp_video_status_override(mode, data))
				log_dsp_video_status_override("GetCurMode", mode, data);
#endif
			WriteMacInt16(param + csMode, video_rel_depth_from_abs(mode));
			WriteMacInt32(param + csData, data);
			WriteMacInt16(param + csPage, csSave->savePage);
			WriteMacInt32(param + csBaseAddr, csSave->saveBaseAddr);

			D(bug("return: mode:%04x ID:%08lx page:%04x ", ReadMacInt16(param + csMode),
				ReadMacInt32(param + csData), ReadMacInt16(param + csPage)));
			D(bug("base adress %08lx\n", ReadMacInt32(param + csBaseAddr)));
			return noErr;
		}

		case cscGetConnection:						// GetConnection
			D(bug("GetConnection\n"));
			WriteMacInt16(param + csDisplayType, kMultiModeCRT3Connect);
			WriteMacInt8(param + csConnectTaggedType, 6);
			WriteMacInt8(param + csConnectTaggedData, 0x23);
			WriteMacInt32(param + csConnectFlags, (1<<kAllModesValid)|(1<<kAllModesSafe));
			WriteMacInt32(param + csDisplayComponent, 0);
			return noErr;

		case cscGetModeBaseAddress:
			D(bug("GetModeBaseAddress (obsolete !) \n"));
			return statusErr;

		case cscGetPreferredConfiguration:
			D(bug("GetPreferredConfiguration \n"));
			WriteMacInt16(param + csMode, save_conf_mode);
			WriteMacInt32(param + csData, save_conf_id);
			return noErr;

		case cscGetNextResolution: {
			D(bug("GetNextResolution \n"));
			unsigned int work_id = ReadMacInt32(param + csPreviousDisplayModeID);
			switch (work_id) {
				case kDisplayModeIDCurrent:
#if (defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) || TARGET_OS_IPHONE
				{
					uint16 mode = csSave->saveMode;
					uint32 data = csSave->saveData;
					if (get_dsp_video_status_override(mode, data)) {
						log_dsp_video_status_override("GetNextResolution", mode, data);
						work_id = data;
						break;
					}
				}
#endif
					work_id = csSave->saveData;
					break;
				case kDisplayModeIDFindFirstResolution:
					work_id = APPLE_ID_MIN;
					while (!has_mode(work_id))
						work_id ++;
					break;
				default:
					if (!has_mode(work_id))
						return paramErr;
					work_id++;
					while (!has_mode(work_id)) {
						work_id++;
						if (work_id > APPLE_ID_MAX) {
							WriteMacInt32(param + csRIDisplayModeID, kDisplayModeIDNoMoreResolutions);
							WriteMacInt16(param + csResolutionFlags, 0);
							WriteMacInt32(param + csResolutionFlags + 2, 0);	// csReserved
							return noErr;
						}
					}
					break;
			}
			WriteMacInt32(param + csRIDisplayModeID, work_id);
			/* csMaxDepthMode is a RELATIVE depth mode (kDepthModeN). */
			WriteMacInt16(param + csMaxDepthMode, video_rel_max_depth_mode());
			/* Zero csResolutionFlags + csReserved: the guest Display Manager
			 * copies the whole VDResolutionInfoRec into its mode-list
			 * entries, and stale guest RAM here (e.g. a stray
			 * kResolutionHasMultipleDepthSizes bit) changes the entry layout
			 * apps parse. */
			WriteMacInt16(param + csResolutionFlags, 0);
			WriteMacInt32(param + csResolutionFlags + 2, 0);	// csReserved
#if TARGET_OS_IPHONE
			uint32 x, y;
			get_size_of_resolution(work_id, x, y);
			WriteMacInt32(param + csHorizontalPixels, x);
			WriteMacInt32(param + csVerticalLines, y);
			int frameRate = objc_getFrameRateSetting();
			WriteMacInt32(param + csRefreshRate, frameRate<<16);
#else
			switch (work_id) {
				case APPLE_512x384:
					WriteMacInt32(param + csHorizontalPixels, 512);
					WriteMacInt32(param + csVerticalLines, 384);
					WriteMacInt32(param + csRefreshRate, 60<<16);
					break;
				case APPLE_640x480:
					WriteMacInt32(param + csHorizontalPixels, 640);
					WriteMacInt32(param + csVerticalLines, 480);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_W_640x480:
					WriteMacInt32(param + csHorizontalPixels, 640);
					WriteMacInt32(param + csVerticalLines, 480);
					WriteMacInt32(param + csRefreshRate, 60<<16);
					break;
				case APPLE_800x600:
					WriteMacInt32(param + csHorizontalPixels, 800);
					WriteMacInt32(param + csVerticalLines, 600);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_W_800x600:
					WriteMacInt32(param + csHorizontalPixels, 800);
					WriteMacInt32(param + csVerticalLines, 600);
					WriteMacInt32(param + csRefreshRate, 60<<16);
					break;
				case APPLE_1024x768:
					WriteMacInt32(param + csHorizontalPixels, 1024);
					WriteMacInt32(param + csVerticalLines, 768);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_1152x768:
					WriteMacInt32(param + csHorizontalPixels, 1152);
					WriteMacInt32(param + csVerticalLines, 768);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_1152x900:
					WriteMacInt32(param + csHorizontalPixels, 1152);
					WriteMacInt32(param + csVerticalLines, 900);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_1280x1024:
					WriteMacInt32(param + csHorizontalPixels, 1280);
					WriteMacInt32(param + csVerticalLines, 1024);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_1600x1200:
					WriteMacInt32(param + csHorizontalPixels, 1600);
					WriteMacInt32(param + csVerticalLines, 1200);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_CUSTOM: {
					uint32 x, y;
					get_size_of_resolution(work_id, x, y);
					WriteMacInt32(param + csHorizontalPixels, x);
					WriteMacInt32(param + csVerticalLines, y);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				}
			}
#endif
			return noErr;
		}

		case cscGetVideoParameters: {				// GetVideoParameters
			D(bug("GetVideoParameters ID:%08lx Depth:%04x\n",
				ReadMacInt32(param + csDisplayModeID),
				ReadMacInt16(param + csDepthMode)));

			uint32 requested_id = ReadMacInt32(param + csDisplayModeID);
			uint16 requested_mode = ReadMacInt16(param + csDepthMode);
			bool mode_is_absolute = false;
#if (defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) || TARGET_OS_IPHONE
			if (requested_id == kDisplayModeIDCurrent) {
				uint16 mode = csSave->saveMode;
				uint32 data = csSave->saveData;
				if (get_dsp_video_status_override(mode, data)) {
					log_dsp_video_status_override("GetVideoParameters", mode, data);
					requested_id = data;
					requested_mode = mode;
					mode_is_absolute = true;
				}
			}
#endif

			if (!mode_is_absolute) {
				/* csDepthMode is a RELATIVE kDepthModeN selector; translate
				 * STRICTLY (no absolute fallback) so the Display Manager's
				 * probe loop terminates after exactly the supported depth
				 * count - dual-accepting absolute values here made every
				 * depth answer twice (once relative, once absolute) and
				 * doubled each resolution's depth records. */
				uint32 abs = video_abs_depth_from_rel(requested_mode);
				if (abs == 0)
					return paramErr;
				requested_mode = (uint16)abs;
			}

			// find right video mode
			for (int i=0; VModes[i].viType!=DIS_INVALID; i++) {
				if ((requested_mode == VModes[i].viAppleMode) &&
					(requested_id == VModes[i].viAppleID)) {
					uint32 vpb = ReadMacInt32(param + csVPBlockPtr);
					WriteMacInt32(vpb + vpBaseOffset, 0);
					WriteMacInt16(vpb + vpRowBytes, VModes[i].viRowBytes);
					WriteMacInt16(vpb + vpBounds, 0);
					WriteMacInt16(vpb + vpBounds + 2, 0);
					WriteMacInt16(vpb + vpBounds + 4, VModes[i].viYsize);
					WriteMacInt16(vpb + vpBounds + 6, VModes[i].viXsize);
					WriteMacInt16(vpb + vpVersion, 0);		// Pixel Map version number
					WriteMacInt16(vpb + vpPackType, 0);
					WriteMacInt32(vpb + vpPackSize, 0);
					WriteMacInt32(vpb + vpPlaneBytes, 0);
					WriteMacInt32(vpb + vpHRes, 0x00480000);	// horiz res of the device (ppi)
					WriteMacInt32(vpb + vpVRes, 0x00480000);	// vert res of the device (ppi)
					switch (VModes[i].viAppleMode) {
						case APPLE_1_BIT:
							WriteMacInt16(vpb + vpPixelType, 0);
							WriteMacInt16(vpb + vpPixelSize, 1);
							WriteMacInt16(vpb + vpCmpCount, 1);
							WriteMacInt16(vpb + vpCmpSize, 1);
							WriteMacInt32(param + csDeviceType, 0); // CLUT
							break;
						case APPLE_2_BIT:
							WriteMacInt16(vpb + vpPixelType, 0);
							WriteMacInt16(vpb + vpPixelSize, 2);
							WriteMacInt16(vpb + vpCmpCount, 1);
							WriteMacInt16(vpb + vpCmpSize, 2);
							WriteMacInt32(param + csDeviceType, 0); // CLUT
							break;
						case APPLE_4_BIT:
							WriteMacInt16(vpb + vpPixelType, 0);
							WriteMacInt16(vpb + vpPixelSize, 4);
							WriteMacInt16(vpb + vpCmpCount, 1);
							WriteMacInt16(vpb + vpCmpSize, 4);
							WriteMacInt32(param + csDeviceType, 0); // CLUT
							break;
						case APPLE_8_BIT:
							WriteMacInt16(vpb + vpPixelType, 0);
							WriteMacInt16(vpb + vpPixelSize, 8);
							WriteMacInt16(vpb + vpCmpCount, 1);
							WriteMacInt16(vpb + vpCmpSize, 8);
							WriteMacInt32(param + csDeviceType, 0); // CLUT
							break;
						case APPLE_16_BIT:
							WriteMacInt16(vpb + vpPixelType, 0x10);
							WriteMacInt16(vpb + vpPixelSize, 16);
							WriteMacInt16(vpb + vpCmpCount, 3);
							WriteMacInt16(vpb + vpCmpSize, 5);
							WriteMacInt32(param + csDeviceType, 2); // DIRECT
							break;
						case APPLE_32_BIT:
							WriteMacInt16(vpb + vpPixelType, 0x10);
							WriteMacInt16(vpb + vpPixelSize, 32);
							WriteMacInt16(vpb + vpCmpCount, 3);
							WriteMacInt16(vpb + vpCmpSize, 8);
							WriteMacInt32(param + csDeviceType, 2); // DIRECT
							break;
					}
					WriteMacInt32(param + csPageCount, 1);
					/* Zero csReserved (packed 68k layout: csDeviceType@14 is
					 * 4 bytes, csReserved@18) - same stale-guest-RAM hazard
					 * as csResolutionFlags in cscGetNextResolution. */
					WriteMacInt32(param + csDeviceType + 4, 0);	// csReserved
					return noErr;
				}
			}
			return paramErr;
		}

		case cscGetModeTiming:
			D(bug("GetModeTiming mode %08lx\n", ReadMacInt32(param + csTimingMode)));
			WriteMacInt32(param + csTimingReserved, 0);
			WriteMacInt32(param + csTimingFormat, kDeclROMtables);
			WriteMacInt32(param + csTimingFlags, (1<<kModeValid)|(1<<kModeSafe)|(1<<kShowModeNow));		// Mode valid, safe, default and shown in Monitors panel
			for (int i=0; VModes[i].viType!=DIS_INVALID; i++) {
				if (ReadMacInt32(param + csTimingMode) == VModes[i].viAppleID) {
					uint32 timing = timingUnknown;
					uint32 flags = (1<<kModeValid) | (1<<kShowModeNow) | (1<<kModeSafe);
					/* Key the timing constant off the mode's actual pixel
					 * size. viAppleID values are assigned sequentially from
					 * the enabled-resolution prefs, NOT the classic fixed
					 * APPLE_* id-to-resolution table, so switching on the id
					 * (as this code originally did) reported a shuffled
					 * timing constant for every mode - e.g. the native-panel
					 * mode carried timingVESA_640x480_75hz. The Display
					 * Manager cross-references these constants when it
					 * builds mode-list entries for apps. */
					const uint32 tw = VModes[i].viXsize;
					const uint32 th = VModes[i].viYsize;
					if (tw == 640 && th == 480)
						timing = timingVESA_640x480_60hz;
					else if (tw == 800 && th == 600)
						timing = timingVESA_800x600_60hz;
					else if (tw == 1024 && th == 768)
						timing = timingVESA_1024x768_75hz;
					else if (tw == 1152 && (th == 768 || th == 870 || th == 900))
						timing = timingApple_1152x870_75hz;
					else if (tw == 1280 && (th == 960 || th == 1024))
						timing = timingVESA_1280x960_75hz;
					else if (tw == 1600 && th == 1200)
						timing = timingVESA_1600x1200_75hz;
					else
						timing = timingUnknown;
					WriteMacInt32(param + csTimingData, timing);
					WriteMacInt32(param + csTimingFlags, flags);
					return noErr;
				}
			}
			return paramErr;

		case cscSupportsHardwareCursor:
			D(bug("SupportsHardwareCursor\n"));
			WriteMacInt32(param, csSave->cursorHardware);
			return noErr;

		case cscGetHardwareCursorDrawState:
			D(bug("GetHardwareCursorDrawState\n"));

			if (!csSave->cursorHardware)
				return statusErr;

			WriteMacInt32(param + csCursorX, csSave->cursorX);
			WriteMacInt32(param + csCursorY, csSave->cursorY);
			WriteMacInt32(param + csCursorVisible, csSave->cursorVisible);
			WriteMacInt32(param + csCursorSet, csSave->cursorSet);
			return noErr;

		default:
			D(bug(" unknown status code %d\n", code));
			return statusErr;
	}
}


/*
 *  Video driver close routine
 */

static int16 VideoClose(uint32 pb, VidLocals *csSave)
{
	D(bug("VideoClose\n"));

	// Delete interrupt service
	csSave->interruptsEnabled = false;
	VSLDisposeInterruptService(csSave->vslServiceID);

	return noErr;
}


/*
 *  Native (PCI) driver entry
 */

int16 VideoDoDriverIO(uint32 spaceID, uint32 commandID, uint32 commandContents, uint32 commandCode, uint32 commandKind)
{
//	D(bug("VideoDoDriverIO space %08x, command %08x, contents %08x, code %d, kind %d\n", spaceID, commandID, commandContents, commandCode, commandKind));
	int16 err = noErr;

	switch (commandCode) {
		case kInitializeCommand:
		case kReplaceCommand:
			if (private_data != NULL) {	// Might be left over from a reboot
				if (private_data->gammaTable)
					Mac_sysfree(private_data->gammaTable);
				if (private_data->regEntryID)
					Mac_sysfree(private_data->regEntryID);
			}
			delete private_data;

			iocic_tvect = FindLibSymbol("\021DriverServicesLib", "\023IOCommandIsComplete");
			D(bug("IOCommandIsComplete TVECT at %08lx\n", iocic_tvect));
			if (iocic_tvect == 0) {
				printf("FATAL: VideoDoDriverIO(): Can't find IOCommandIsComplete()\n");
				err = -1;
				break;
			}
			vslnewis_tvect = FindLibSymbol("\020VideoServicesLib", "\026VSLNewInterruptService");
			D(bug("VSLNewInterruptService TVECT at %08lx\n", vslnewis_tvect));
			if (vslnewis_tvect == 0) {
				printf("FATAL: VideoDoDriverIO(): Can't find VSLNewInterruptService()\n");
				err = -1;
				break;
			}
			vsldisposeis_tvect = FindLibSymbol("\020VideoServicesLib", "\032VSLDisposeInterruptService");
			D(bug("VSLDisposeInterruptService TVECT at %08lx\n", vsldisposeis_tvect));
			if (vsldisposeis_tvect == 0) {
				printf("FATAL: VideoDoDriverIO(): Can't find VSLDisposeInterruptService()\n");
				err = -1;
				break;
			}
			vsldois_tvect = FindLibSymbol("\020VideoServicesLib", "\025VSLDoInterruptService");
			D(bug("VSLDoInterruptService TVECT at %08lx\n", vsldois_tvect));
			if (vsldois_tvect == 0) {
				printf("FATAL: VideoDoDriverIO(): Can't find VSLDoInterruptService()\n");
				err = -1;
				break;
			}
			nqdmisc_tvect = FindLibSymbol("\014InterfaceLib", "\007NQDMisc");
			if (nqdmisc_tvect == 0) {
				printf("FATAL: VideoDoDriverIO(): Can't find NQDMisc()\n");
				err = -1;
				break;
			}

			private_data = new VidLocals;
			private_data->driverreference = (int16)ReadMacInt16(commandContents);
			private_data->displayhandle = 0;
			private_data->displayregistered = false;
			private_data->displayregistrationactive = false;
			private_data->gammaTable = 0;
			private_data->regEntryID = Mac_sysalloc(sizeof(RegEntryID));
			if (private_data->regEntryID == 0) {
				printf("FATAL: VideoDoDriverIO(): Can't allocate service owner\n");
				err = -1;
				break;
			}
			Mac2Mac_memcpy(private_data->regEntryID, commandContents + 2, 16);	// DriverInitInfo.deviceEntry
			private_data->interruptsEnabled = false;	// Disable interrupts
			break;

		case kFinalizeCommand:
		case kSupersededCommand:
			if (private_data != NULL) {
				if (private_data->gammaTable)
					Mac_sysfree(private_data->gammaTable);
				if (private_data->regEntryID)
					Mac_sysfree(private_data->regEntryID);
			}
			delete private_data;
			private_data = NULL;
			break;

		case kOpenCommand:
			err = VideoOpen(commandContents, private_data);
			break;

		case kCloseCommand:
			err = VideoClose(commandContents, private_data);
			break;

		case kControlCommand:
			err = VideoControl(commandContents, private_data);
			break;

		case kStatusCommand:
			err = VideoStatus(commandContents, private_data);
			break;

		case kReadCommand:
		case kWriteCommand:
			break;

		case kKillIOCommand:
			err = abortErr;
			break;

		default:
			err = paramErr;
			break;
	}

	if (commandKind == kImmediateIOCommandKind)
		return err;
	else
		return IOCommandIsComplete(commandID, err);
}

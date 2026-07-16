/*
 *  prefs_items.cpp - Common preferences items
 *
 *  SheepShaver (C) 1997-2008 Christian Bauer and Marc Hellwig
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

#include "sysdeps.h"

#include "sys.h"
#include "prefs.h"

#ifdef __APPLE__
#include "TargetConditionals.h"
#endif

// Common preferences items (those which exist on all platforms)
prefs_desc common_prefs_items[] = {
	{"disk", TYPE_STRING, true,         "device/file name of Mac volume"},
	{"floppy", TYPE_STRING, true,       "device/file name of Mac floppy drive"},
	{"cdrom", TYPE_STRING, true,        "device/file names of Mac CD-ROM drive"},
	{"extfs", TYPE_STRING, false,       "root path of ExtFS"},
	{"scsi0", TYPE_STRING, false,       "SCSI target for Mac SCSI ID 0"},
	{"scsi1", TYPE_STRING, false,       "SCSI target for Mac SCSI ID 1"},
	{"scsi2", TYPE_STRING, false,       "SCSI target for Mac SCSI ID 2"},
	{"scsi3", TYPE_STRING, false,       "SCSI target for Mac SCSI ID 3"},
	{"scsi4", TYPE_STRING, false,       "SCSI target for Mac SCSI ID 4"},
	{"scsi5", TYPE_STRING, false,       "SCSI target for Mac SCSI ID 5"},
	{"scsi6", TYPE_STRING, false,       "SCSI target for Mac SCSI ID 6"},
	{"screen", TYPE_STRING, false,      "video mode"},
	{"windowmodes", TYPE_INT32, false,  "bitmap of allowed window video modes"},
	{"screenmodes", TYPE_INT32, false,  "bitmap of allowed fullscreen video modes"},
	{"seriala", TYPE_STRING, false,     "device name of Mac serial port A"},
	{"serialb", TYPE_STRING, false,     "device name of Mac serial port B"},
	{"rom", TYPE_STRING, false,         "path of ROM file"},
	{"bootdrive", TYPE_INT32, false,    "boot drive number"},
	{"bootdriver", TYPE_INT32, false,   "boot driver number"},
	{"ramsize", TYPE_INT32, false,      "size of Mac RAM in bytes"},
	{"frameskip", TYPE_INT32, false,    "number of frames to skip in refreshed video modes"},
	{"gfxaccel", TYPE_BOOLEAN, false,   "turn on QuickDraw acceleration"},
	{"nocdrom", TYPE_BOOLEAN, false,    "don't install CD-ROM driver"},
	{"nonet", TYPE_BOOLEAN, false,      "don't use Ethernet"},
	{"nosound", TYPE_BOOLEAN, false,    "don't enable sound output"},
	{"nogui", TYPE_BOOLEAN, false,      "disable GUI"},
	{"noclipconversion", TYPE_BOOLEAN, false, "don't convert clipboard contents"},
	{"ignoresegv", TYPE_BOOLEAN, false, "ignore illegal memory accesses"},
	{"ignoreillegal", TYPE_BOOLEAN, false, "ignore illegal instructions"},
	{"jit", TYPE_BOOLEAN, false,        "enable JIT compiler"},
	{"jit68k", TYPE_BOOLEAN, false,     "enable 68k DR emulator"},
	{"keyboardtype", TYPE_INT32, false, "hardware keyboard type"},
	{"hardcursor", TYPE_BOOLEAN, false, "hardware mouse cursor"},
	{"hotkey", TYPE_INT32, false,       "hotkey modifier"},
	{"scale_nearest",TYPE_BOOLEAN,false,"nearest neighbor scaling"},
	{"scale_integer",TYPE_BOOLEAN,false,"integer scaling"},
	{"cpuclock", TYPE_INT32, 0,			"CPU clock [MHz] of system info"},
	{"yearofs", TYPE_INT32, 0,			"year offset"},
	{"dayofs", TYPE_INT32, 0,			"day offset"},
	{"mag_rate", TYPE_STRING, 0,			"rate of magnification"},
	{"gammaramp", TYPE_STRING, false,	"gamma ramp (on, off or fullscreen)"},
	{"nqdaccel", TYPE_BOOLEAN, false,	"enable NQD Metal compute acceleration"},
	{"raveaccel", TYPE_BOOLEAN, false,	"enable RAVE 3D acceleration"},
	{"glaccel", TYPE_BOOLEAN, false,	"enable OpenGL acceleration"},
	{"dspaccel", TYPE_BOOLEAN, true,	"enable DrawSprocket (DSp) acceleration"},
	{"swap_opt_cmd", TYPE_BOOLEAN, false,	"swap option and command key"},
	{"host_domain", TYPE_STRING, true,	"handle DNS requests for this domain on the host (slirp only)"},
	{"redir", TYPE_STRING, true,		"port forwarding for slirp"},
	{"title", TYPE_STRING, false,	"window title"},
	{"sound_buffer", TYPE_INT32, false,	"sound buffer length"},
	{"name_encoding", TYPE_INT32, false,	"file name encoding"},
	{"init_grab", TYPE_BOOLEAN, false,	"initially grabbing mouse"},
#if TARGET_OS_IPHONE
	// Mac Catalyst Windowed/Full Screen choice. Needs a descriptor here so SavePrefs()
	// (which serializes the descriptor tables, not the runtime list) persists it to disk;
	// without it the pref lives only in memory and reverts to its default every launch.
	{"catalystfullscreen", TYPE_BOOLEAN, false,	"Mac Catalyst: launch emulation full screen"},
#endif
	{NULL, TYPE_END, false, NULL} // End of list
};

/*
 *  Set default values for preferences items
 */

void AddPrefsDefaults(void)
{
#ifndef PREFS_EDITOR
	SysAddSerialPrefs();
#endif
	PrefsAddInt32("bootdriver", 0);
	PrefsAddInt32("bootdrive", 0);
#if TARGET_OS_IPHONE
	PrefsReplaceInt32("ramsize", 256);
	PrefsReplaceInt32("frameskip", 1);
	// Report a 466 MHz CPU to the guest by default (PowerBook/iMac G3 class).
	// New installs persist this value; installs that predate this default are
	// moved up by the one-time adoption step at the end of LoadPrefs() (their
	// saved prefs carry the prior "cpuclock 0").
	PrefsAddInt32("cpuclock", 466);
	// Mac Catalyst: remembers the user's Windowed/Full Screen choice for the emulation
	// window and is honored on launch. Defaults to full screen to preserve the prior
	// always-fullscreen behavior. A brand-new key, so existing installs pick up this
	// default automatically (no migration needed). Only read on Catalyst.
	PrefsAddBool("catalystfullscreen", true);
#else
	PrefsAddInt32("ramsize", 16 * 1024 * 1024);
	PrefsAddInt32("frameskip", 8);
#endif
	PrefsAddBool("gfxaccel", true);
	PrefsAddBool("nqdaccel", true);
	PrefsAddBool("raveaccel", true);
	PrefsAddBool("glaccel", true);
	PrefsAddBool("dspaccel", true);
	PrefsAddBool("nocdrom", false);
	PrefsAddBool("nonet", false);
	PrefsAddBool("nosound", false);
	PrefsAddBool("nogui", false);
	PrefsAddBool("noclipconversion", false);
	PrefsAddBool("ignoresegv", true);
	PrefsAddBool("ignoreillegal", false);

#if USE_JIT && !defined(__aarch64__)
	// JIT compiler specific options
	PrefsAddBool("jit", true);
#else
	// No JIT on this host, or (arm64) the C++ core keeps this pref off as the
	// interpreter baseline — enabling PPC_ENABLE_JIT must not self-activate it.
	// The Mac Catalyst app defaults JIT on at the settings layer (see
	// MiscellaneousSettings) and writes "jit" true when the user boots.
	PrefsAddBool("jit", false);
#endif
	PrefsAddBool("jit68k", false);

	PrefsAddInt32("keyboardtype", 5);

#ifdef __APPLE__
	PrefsAddBool("swap_opt_cmd", false);
#else
	PrefsAddBool("swap_opt_cmd", true);
#endif
}

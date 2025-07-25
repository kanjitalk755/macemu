/*
 *  main_windows.cpp - Emulation core, Windows implementation
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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "my_sdl.h"

#include "sysdeps.h"
#include "main.h"
#include "version.h"
#include "prefs.h"
#include "prefs_editor.h"
#include "cpu_emulation.h"
#include "emul_op.h"
#include "xlowmem.h"
#include "xpram.h"
#include "timer.h"
#include "adb.h"
#include "video.h"
#include "sys.h"
#include "macos_util.h"
#include "rom_patches.h"
#include "user_strings.h"
#include "vm_alloc.h"
#include "sigsegv.h"
#include "util_windows.h"
//#include "kernel_windows.h"

#define DEBUG 0
#include "debug.h"

#ifdef ENABLE_MON
#include "mon.h"
#endif

#if !SDL_VERSION_ATLEAST(3, 0, 0)
#define SDL_EVENT_KEY_UP	SDL_KEYUP
#define SDL_EVENT_KEY_DOWN	SDL_KEYDOWN
#endif

// Constants
const char ROM_FILE_NAME[] = "ROM";
const char ROM_FILE_NAME2[] = "Mac OS ROM";

const uintptr ROM_BASE = 0x40800000;		// Base address of ROM

const uint32 SIG_STACK_SIZE = 0x10000;		// Size of signal stack


// Global variables (exported)
uint32 RAMBase;			// Base address of Mac RAM
uint32 RAMSize;			// Size of Mac RAM
uint32 ROMBase;			// Base address of Mac ROM
uint32 KernelDataAddr;	// Address of Kernel Data
uint32 BootGlobsAddr;	// Address of BootGlobs structure at top of Mac RAM
uint32 DRCacheAddr;		// Address of DR Cache
uint32 PVR;				// Theoretical PVR
int64 CPUClockSpeed;	// Processor clock speed (Hz)
int64 BusClockSpeed;	// Bus clock speed (Hz)
int64 TimebaseSpeed;	// Timebase clock speed (Hz)
uint8 *RAMBaseHost;		// Base address of Mac RAM (host address space)
uint8 *ROMBaseHost;		// Base address of Mac ROM (host address space)
DWORD win_os;			// Windows OS id
DWORD win_os_major;		// Windows OS version major

#ifdef MEM_BULK
uint8 gKernelData[0x2000];
#endif

// Global variables
static int kernel_area = -1;				// SHM ID of Kernel Data area
static bool rom_area_mapped = false;		// Flag: Mac ROM mmap()ped
static bool ram_area_mapped = false;		// Flag: Mac RAM mmap()ped
static bool dr_cache_area_mapped = false;	// Flag: Mac DR Cache mmap()ped
static bool dr_emulator_area_mapped = false;// Flag: Mac DR Emulator mmap()ped
static KernelData *kernel_data;				// Pointer to Kernel Data
static EmulatorData *emulator_data;

static uint8 last_xpram[XPRAM_SIZE];		// Buffer for monitoring XPRAM changes
static bool nvram_thread_active = false;	// Flag: NVRAM watchdog installed
static volatile bool nvram_thread_cancel;	// Flag: Cancel NVRAM thread
static HANDLE nvram_thread = NULL;			// NVRAM watchdog
static bool tick_thread_active = false;		// Flag: MacOS thread installed
static volatile bool tick_thread_cancel;	// Flag: Cancel 60Hz thread
static HANDLE tick_thread = NULL;			// 60Hz thread
static HANDLE emul_thread = NULL;			// MacOS thread
static uintptr sig_stack = 0;				// Stack for PowerPC interrupt routine

uint32  SheepMem::page_size;				// Size of a native page
uintptr SheepMem::zero_page = 0;			// Address of ro page filled in with zeros
uintptr SheepMem::base = 0x51000000;		// Address of SheepShaver data
uintptr SheepMem::proc;						// Bottom address of SheepShave procedures
uintptr SheepMem::data;						// Top of SheepShaver data (stack like storage)

static HHOOK keyboard_hook;					// Hook for intercepting windows key events


// Prototypes
static bool kernel_data_init(void);
static void kernel_data_exit(void);
static void Quit(void);
static DWORD WINAPI nvram_func(void *arg);
static DWORD WINAPI tick_func(void *arg);

static void jump_to_rom(uint32 entry);
extern void emul_ppc(uint32 start);
extern void init_emul_ppc(void);
extern void exit_emul_ppc(void);
sigsegv_return_t sigsegv_handler(sigsegv_info_t *sip);
static LRESULT CALLBACK low_level_keyboard_hook(int nCode, WPARAM wParam, LPARAM lParam);


/*
 *  Return signal stack base
 */

uintptr SignalStackBase(void)
{
	return sig_stack + SIG_STACK_SIZE;
}


/*
 *  Memory management helpers
 */

static inline int vm_mac_acquire(uint32 addr, uint32 size)
{
	return vm_acquire_fixed(Mac2HostAddr(addr), size);
}

static inline int vm_mac_release(uint32 addr, uint32 size)
{
	return vm_release(Mac2HostAddr(addr), size);
}


/*
 *  Main program
 */

static void usage(const char *prg_name)
{
	printf("Usage: %s [OPTION...]\n", prg_name);
	printf("\nUnix options:\n");
	printf("  --display STRING\n    X display to use\n");
	PrefsPrintUsage();
	printf("\nBuild Date: %s\n", __DATE__);
	exit(0);
}

int main(int argc, char **argv)
{
	char str[256];
	int16 i16;
	HANDLE rom_fh;
	const char *rom_path;
	uint32 rom_size;
	DWORD actual;
	uint8 *rom_tmp;

	// Initialize variables
	RAMBase = 0;

	// Print some info
	printf(GetString(STR_ABOUT_TEXT1), VERSION_MAJOR, VERSION_MINOR);
	printf(" %s\n", GetString(STR_ABOUT_TEXT2));

	// Parse command line arguments

	// Check for options we want to process before PrefsInit
	for (int i=1; i<argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
		} else if (strcmp(argv[i], "--config") == 0) {
			argv[i++] = NULL;
			if (i < argc) {
				extern std::string UserPrefsPath; // from prefs_windows.cpp
				UserPrefsPath = to_tstring(argv[i]);
				argv[i] = NULL;
			}
		}
	}

	// Remove processed arguments
	for (int i=1; i<argc; i++) {
		int k;
		for (k=i; k<argc; k++)
			if (argv[k] != NULL)
				break;
		if (k > i) {
			k -= i;
			for (int j=i+k; j<argc; j++)
				argv[j-k] = argv[j];
			argc -= k;
		}
	}

	// Read preferences
	PrefsInit(NULL, argc, argv);

	for (int i=1; i<argc; i++) {
		if (argv[i][0] == '-') {
			fprintf(stderr, "Unrecognized option '%s'\n", argv[i]);
			usage(argv[0]);
		}
	}

	// #chenchijung 2024/2/21: move vm_init(), memory allocation for Mac RAM and Mac ROM here to avoid "cannot map RAM: no Error" bug.
	//   caused by MSI afterburner (RIVA Tuner statistic tuner Server?). It is a workaround since I don't know why. But it works in my test env.
	//
	// ------------ Start of workaround --------------
	int sdl_flags = 0;
	// Initialize VM system
	vm_init();

	// Create area for Mac RAM
	RAMSize = PrefsFindInt32("ramsize");
	if (RAMSize <= 1000) {
		RAMSize *= 1024 * 1024;
	}
	if (RAMSize < 16 * 1024 * 1024) {
		WarningAlert(GetString(STR_SMALL_RAM_WARN));
		RAMSize = 16 * 1024 * 1024;
	}
	RAMBase = 0;
	if (vm_mac_acquire(RAMBase, RAMSize) < 0) {
		sprintf(str, GetString(STR_RAM_MMAP_ERR), strerror(errno));
		ErrorAlert(str);
		goto quit;
	}
	RAMBaseHost = Mac2HostAddr(RAMBase);
	ram_area_mapped = true;
	D(bug("RAM area at %p (%08x)\n", RAMBaseHost, RAMBase));

	// Create area for Mac ROM
	if (vm_mac_acquire(ROM_BASE, ROM_AREA_SIZE) < 0) {
		sprintf(str, GetString(STR_ROM_MMAP_ERR), strerror(errno));
		ErrorAlert(str);
		goto quit;
	}
	ROMBase = ROM_BASE;
	ROMBaseHost = Mac2HostAddr(ROMBase);
	rom_area_mapped = true;
	D(bug("ROM area at %p (%08x)\n", ROMBaseHost, ROMBase));

	if (RAMBase > ROMBase) {
		ErrorAlert(GetString(STR_RAM_HIGHER_THAN_ROM_ERR));
		goto quit;
	}
	//#chenchijung ----------------- end of workaround --------------

	// Check we are using a Windows NT kernel >= 4.0
	OSVERSIONINFO osvi;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFO));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	if (!GetVersionEx(&osvi)) {
		ErrorAlert("Could not determine OS type");
		QuitEmulator();
	}
	win_os = osvi.dwPlatformId;
	win_os_major = osvi.dwMajorVersion;
	if (win_os != VER_PLATFORM_WIN32_NT || win_os_major < 4) {
		ErrorAlert(GetString(STR_NO_WIN32_NT_4));
		QuitEmulator();
	}

	// Check that drivers are installed
	if (!check_drivers())
		QuitEmulator();

//	// Load win32 libraries
//	KernelInit();

	// Install keyboard hook to block Windows key if enabled in prefs
	if (PrefsFindBool("reservewindowskey"))
	{
		keyboard_hook = SetWindowsHookEx(WH_KEYBOARD_LL, low_level_keyboard_hook, GetModuleHandle(NULL), 0);
	}

	// Initialize SDL system
	sdl_flags = 0; // #chenchijungtw move variable definition forward to avoid complication error
#ifdef USE_SDL_VIDEO
	sdl_flags |= SDL_INIT_VIDEO;
#endif
#ifdef USE_SDL_AUDIO
	sdl_flags |= SDL_INIT_AUDIO;
#endif
	assert(sdl_flags != 0);
	if (SDL_Init(sdl_flags) == -1) {
		char str[256];
		sprintf(str, "Could not initialize SDL: %s.\n", SDL_GetError());
		ErrorAlert(str);
		goto quit;
	}
	atexit(SDL_Quit);

#ifdef ENABLE_MON
	// Initialize mon
	mon_init();
#endif

	// Install SIGSEGV handler for CPU emulator
	if (!sigsegv_install_handler(sigsegv_handler)) {
		sprintf(str, GetString(STR_SIGSEGV_INSTALL_ERR), strerror(errno));
		ErrorAlert(str);
		goto quit;
	}

	// Get system info
	PVR = 0x00040000;			// Default: 604
	CPUClockSpeed = 100000000;	// Default: 100MHz
	BusClockSpeed = 100000000;	// Default: 100MHz
	TimebaseSpeed =  25000000;	// Default:  25MHz
	PVR = 0x000c0000;			// Default: 7400 (with AltiVec)
	D(bug("PVR: %08x (assumed)\n", PVR));
	{
		int pref_cpu_clock = PrefsFindInt32("cpuclock");
		if (pref_cpu_clock) CPUClockSpeed = 1000000 * pref_cpu_clock;
	}

	// Init system routines
	SysInit();

	// Show preferences editor
	if (!PrefsFindBool("nogui"))
		if (!PrefsEditor())
			goto quit;

#ifndef MEM_BULK
	// Create areas for Kernel Data
	if (!kernel_data_init())
		goto quit;
#endif
	kernel_data = (KernelData *)Mac2HostAddr(KERNEL_DATA_BASE);
	emulator_data = &kernel_data->ed;
	KernelDataAddr = KERNEL_DATA_BASE;
	D(bug("Kernel Data at %p (%08x)\n", kernel_data, KERNEL_DATA_BASE));
	D(bug("Emulator Data at %p (%08x)\n", emulator_data, KERNEL_DATA_BASE + offsetof(KernelData, ed)));
#if 0
	// Create area for DR Cache
	if (vm_mac_acquire(DR_EMULATOR_BASE, DR_EMULATOR_SIZE) < 0) {
		sprintf(str, GetString(STR_DR_EMULATOR_MMAP_ERR), strerror(errno));
		ErrorAlert(str);
		goto quit;
	}
	dr_emulator_area_mapped = true;
	if (vm_mac_acquire(DR_CACHE_BASE, DR_CACHE_SIZE) < 0) {
		sprintf(str, GetString(STR_DR_CACHE_MMAP_ERR), strerror(errno));
		ErrorAlert(str);
		goto quit;
	}
	dr_cache_area_mapped = true;
	DRCacheAddr = (uint32)Mac2HostAddr(DR_CACHE_BASE);
	D(bug("DR Cache at %p (%08x)\n", DRCacheAddr, DR_CACHE_BASE));
#endif
	// Create area for SheepShaver data
	if (!SheepMem::Init()) {
		sprintf(str, GetString(STR_SHEEP_MEM_MMAP_ERR), strerror(errno));
		ErrorAlert(str);
		goto quit;
	}

	// Load Mac ROM
	rom_path = PrefsFindString("rom");
	rom_fh = CreateFile(rom_path && *rom_path ? rom_path : ROM_FILE_NAME,
						GENERIC_READ, 0, NULL, OPEN_EXISTING,
						FILE_ATTRIBUTE_NORMAL, NULL);

	if (rom_fh == INVALID_HANDLE_VALUE) {
		rom_fh = CreateFile(ROM_FILE_NAME2,
							GENERIC_READ, 0, NULL, OPEN_EXISTING,
							FILE_ATTRIBUTE_NORMAL, NULL);

		if (rom_fh == INVALID_HANDLE_VALUE) {
			ErrorAlert(GetString(STR_NO_ROM_FILE_ERR));
			goto quit;
		}
	}
	printf(GetString(STR_READING_ROM_FILE));
	rom_size = GetFileSize(rom_fh, NULL);
	rom_tmp = new uint8[ROM_SIZE];
	ReadFile(rom_fh, (void *)rom_tmp, ROM_SIZE, &actual, NULL);
	CloseHandle(rom_fh);
	
	// Decode Mac ROM
	if (!DecodeROM(rom_tmp, actual)) {
		if (rom_size != 4*1024*1024) {
			ErrorAlert(GetString(STR_ROM_SIZE_ERR));
			goto quit;
		} else {
			ErrorAlert(GetString(STR_ROM_FILE_READ_ERR));
			goto quit;
		}
	}
	delete[] rom_tmp;
	
	// Initialize native timers
	timer_init();

	// Initialize everything
	if (!InitAll(NULL))
		goto quit;
	D(bug("Initialization complete\n"));

	// Write protect ROM
	vm_protect(ROMBaseHost, ROM_AREA_SIZE, VM_PAGE_READ);

	// Start 60Hz thread
	tick_thread_cancel = false;
	tick_thread_active = ((tick_thread = create_thread(tick_func)) != NULL);
	SetThreadPriority(tick_thread, THREAD_PRIORITY_ABOVE_NORMAL);
	D(bug("Tick thread installed (%ld)\n", tick_thread));

	// Start NVRAM watchdog thread
	memcpy(last_xpram, XPRAM, XPRAM_SIZE);
	nvram_thread_cancel = false;
	nvram_thread_active = ((nvram_thread = create_thread(nvram_func, NULL)) != NULL);
	SetThreadPriority(nvram_thread, THREAD_PRIORITY_BELOW_NORMAL);
	D(bug("NVRAM thread installed (%ld)\n", nvram_thread));

	// Get my thread ID and jump to ROM boot routine
	emul_thread = GetCurrentThread();
	D(bug("Jumping to ROM\n"));
	jump_to_rom(ROMBase + 0x310000);
	D(bug("Returned from ROM\n"));

quit:
	Quit();
	return 0;
}


/*
 *  Cleanup and quit
 */

static void Quit(void)
{
	// Exit PowerPC emulation
	exit_emul_ppc();

	// Stop 60Hz thread
	if (tick_thread_active) {
		tick_thread_cancel = true;
		wait_thread(tick_thread);
	}

	// Stop NVRAM watchdog thread
	if (nvram_thread_active) {
		nvram_thread_cancel = true;
		wait_thread(nvram_thread);
	}

	// Deinitialize everything
	ExitAll();

	// Delete SheepShaver globals
	SheepMem::Exit();

	// Delete RAM area
	if (ram_area_mapped)
		vm_mac_release(RAMBase, RAMSize);

	// Delete ROM area
	if (rom_area_mapped)
		vm_mac_release(ROMBase, ROM_AREA_SIZE);

	// Delete DR cache areas
	if (dr_emulator_area_mapped)
		vm_mac_release(DR_EMULATOR_BASE, DR_EMULATOR_SIZE);
	if (dr_cache_area_mapped)
		vm_mac_release(DR_CACHE_BASE, DR_CACHE_SIZE);

	// Delete Kernel Data area
	kernel_data_exit();

	// Exit system routines
	SysExit();

	// Exit preferences
	PrefsExit();

//	// Release win32 libraries
//	KernelExit();

#ifdef ENABLE_MON
	// Exit mon
	mon_exit();
#endif

	exit(0);
}


/*
 *  Initialize Kernel Data segments
 */

static HANDLE kernel_handle;				// Shared memory handle for Kernel Data
static DWORD allocation_granule;			// Minimum size of allocateable are (64K)
static DWORD kernel_area_size;				// Size of Kernel Data area

static bool kernel_data_init(void)
{
	char str[256];
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	allocation_granule = si.dwAllocationGranularity;
	kernel_area_size = (KERNEL_AREA_SIZE + allocation_granule - 1) & -allocation_granule;

	char rcs[10];
	LPVOID kernel_addr;
	kernel_handle = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, kernel_area_size, NULL);
	if (kernel_handle == NULL) {
		sprintf(rcs, "%d", GetLastError());
		sprintf(str, GetString(STR_KD_SHMGET_ERR), rcs);
		ErrorAlert(str);
		return false;
	}
	kernel_addr = (LPVOID)Mac2HostAddr(KERNEL_DATA_BASE & -allocation_granule);
	if (MapViewOfFileEx(kernel_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kernel_area_size, kernel_addr) != kernel_addr) {
		sprintf(rcs, "%d", GetLastError());
		sprintf(str, GetString(STR_KD_SHMAT_ERR), rcs);
		ErrorAlert(str);
		return false;
	}
	kernel_addr = (LPVOID)Mac2HostAddr(KERNEL_DATA2_BASE & -allocation_granule);
	if (MapViewOfFileEx(kernel_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kernel_area_size, kernel_addr) != kernel_addr) {
		sprintf(rcs, "%d", GetLastError());
		sprintf(str, GetString(STR_KD2_SHMAT_ERR), rcs);
		ErrorAlert(str);
		return false;
	}
	return true;
}


/*
 *  Deallocate Kernel Data segments
 */

static void kernel_data_exit(void)
{
	if (kernel_handle) {
		UnmapViewOfFile(Mac2HostAddr(KERNEL_DATA_BASE & -allocation_granule));
		UnmapViewOfFile(Mac2HostAddr(KERNEL_DATA2_BASE & -allocation_granule));
		CloseHandle(kernel_handle);
	}
}


/*
 *  Jump into Mac ROM, start 680x0 emulator
 */

void jump_to_rom(uint32 entry)
{
	init_emul_ppc();
	emul_ppc(entry);
}


/*
 *  Quit emulator (cause return from jump_to_rom)
 */

void QuitEmulator(void)
{
	Quit();
}


/*
 *  Pause/resume emulator
 */

void PauseEmulator(void)
{
	SuspendThread(emul_thread);
}

void ResumeEmulator(void)
{
	ResumeThread(emul_thread);
}


/*
 *  Dump 68k registers
 */

void Dump68kRegs(M68kRegisters *r)
{
	// Display 68k registers
	for (int i=0; i<8; i++) {
		printf("d%d: %08x", i, r->d[i]);
		if (i == 3 || i == 7)
			printf("\n");
		else
			printf(", ");
	}
	for (int i=0; i<8; i++) {
		printf("a%d: %08x", i, r->a[i]);
		if (i == 3 || i == 7)
			printf("\n");
		else
			printf(", ");
	}
}


/*
 *  Make code executable
 */

void MakeExecutable(int dummy, uint32 start, uint32 length)
{
	if ((start >= ROMBase) && (start < (ROMBase + ROM_SIZE)))
		return;
	FlushCodeCache(start, start + length);
}


/*
 *  NVRAM watchdog thread (saves NVRAM every minute)
 */

static void nvram_watchdog(void)
{
	if (memcmp(last_xpram, XPRAM, XPRAM_SIZE)) {
		memcpy(last_xpram, XPRAM, XPRAM_SIZE);
		SaveXPRAM();
	}
}

static DWORD nvram_func(void *arg)
{
	while (!nvram_thread_cancel) {
		for (int i=0; i<60 && !nvram_thread_cancel; i++)
			Delay_usec(999999);		// Only wait 1 second so we quit promptly when nvram_thread_cancel becomes true
		nvram_watchdog();
	}
	return 0;
}


/*
 *  60Hz thread (really 60.15Hz)
 */

bool tick_inhibit;
static DWORD tick_func(void *arg)
{
	int tick_counter = 0;
	uint64 start = GetTicks_usec();
	int64 ticks = 0;
	uint64 next = GetTicks_usec();

	while (!tick_thread_cancel) {

		// Wait
		next += 16625;
		int64 delay = next - GetTicks_usec();
		if (delay > 0)
			Delay_usec(delay);
		else if (delay < -16625)
			next = GetTicks_usec();
		if (tick_inhibit) continue;
		ticks++;

		// Pseudo Mac 1Hz interrupt, update local time
		if (++tick_counter > 60) {
			tick_counter = 0;
			WriteMacInt32(0x20c, TimerDateTime());
		}

		// Trigger 60Hz interrupt
		if (ReadMacInt32(XLM_IRQ_NEST) == 0) {
			SetInterruptFlag(INTFLAG_VIA);
			TriggerInterrupt();
		}
	}

	uint64 end = GetTicks_usec();
	D(bug("%lu ticks in %lu usec = %f ticks/sec\n", (unsigned long)ticks, (unsigned long)(end - start), ticks * 1000000.0 / (end - start)));
	return 0;
}


/*
 *  Mutexes
 */

struct B2_mutex {
	mutex_t m;
};

B2_mutex *B2_create_mutex(void)
{
	return new B2_mutex;
}

void B2_lock_mutex(B2_mutex *mutex)
{
	mutex->m.lock();
}

void B2_unlock_mutex(B2_mutex *mutex)
{
	mutex->m.unlock();
}

void B2_delete_mutex(B2_mutex *mutex)
{
	delete mutex;
}


/*
 *  Interrupt flags (must be handled atomically!)
 */

volatile uint32 InterruptFlags = 0;
static mutex_t intflags_mutex;

void SetInterruptFlag(uint32 flag)
{
	intflags_mutex.lock();
	InterruptFlags |= flag;
	intflags_mutex.unlock();
}

void ClearInterruptFlag(uint32 flag)
{
	intflags_mutex.lock();
	InterruptFlags &= ~flag;
	intflags_mutex.unlock();
}


/*
 *  Disable interrupts
 */

void DisableInterrupt(void)
{
	WriteMacInt32(XLM_IRQ_NEST, int32(ReadMacInt32(XLM_IRQ_NEST)) + 1);
}


/*
 *  Enable interrupts
 */

void EnableInterrupt(void)
{
	WriteMacInt32(XLM_IRQ_NEST, int32(ReadMacInt32(XLM_IRQ_NEST)) - 1);
}


/*
 *  Helpers to share 32-bit addressable data with MacOS
 */

bool SheepMem::Init(void)
{
	// Size of a native page
	page_size = vm_get_page_size();

	// Allocate SheepShaver globals
	proc = base;
	if (vm_mac_acquire(base, size) < 0)
		return false;

	// Allocate page with all bits set to 0, right in the middle
	// This is also used to catch undesired overlaps between proc and data areas
	zero_page = proc + (size / 2);
	Mac_memset(zero_page, 0, page_size);
	if (vm_protect(Mac2HostAddr(zero_page), page_size, VM_PAGE_READ) < 0)
		return false;

	// Allocate alternate stack for PowerPC interrupt routine
	sig_stack = base + size;
	if (vm_mac_acquire(sig_stack, SIG_STACK_SIZE) < 0)
		return false;

	data = base + size;
	return true;
}

void SheepMem::Exit(void)
{
	if (data) {
		// Delete SheepShaver globals
		vm_mac_release(base, size);

		// Delete alternate stack for PowerPC interrupt routine
		vm_mac_release(sig_stack, SIG_STACK_SIZE);
	}
}


/*
 *  Get the main window handle
 */

#ifdef USE_SDL_VIDEO
#if !SDL_VERSION_ATLEAST(3, 0, 0)
#include <SDL_syswm.h>
#endif
extern SDL_Window *sdl_window;
HWND GetMainWindowHandle(void)
{
	if (!sdl_window) {
		return NULL;
	}
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
	return (HWND)SDL_GetPointerProperty(props, "SDL.window.cocoa.window", NULL);
#else
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	return SDL_GetWindowWMInfo(sdl_window, &wmInfo) ? wmInfo.info.win.window : NULL;
#endif
}
#endif


/*
 *  Display alert
 */

static void display_alert(int title_id, const char *text, int flags)
{
	HWND hMainWnd = GetMainWindowHandle();
	MessageBox(hMainWnd, text, GetString(title_id), MB_OK | flags);
}


/*
 *  Display error alert
 */

void ErrorAlert(const char *text)
{
	if (PrefsFindBool("nogui"))
		return;

	VideoQuitFullScreen();
	display_alert(STR_ERROR_ALERT_TITLE, text, MB_ICONSTOP);
}


/*
 *  Display warning alert
 */

void WarningAlert(const char *text)
{
	if (PrefsFindBool("nogui"))
		return;

	display_alert(STR_WARNING_ALERT_TITLE, text, MB_ICONINFORMATION);
}


/*
 *  Display choice alert
 */

bool ChoiceAlert(const char *text, const char *pos, const char *neg)
{
	printf(GetString(STR_SHELL_WARNING_PREFIX), text);
	return false;	//!!
}

/*
 *  Low level keyboard hook allowing us to intercept events involving the Windows key
 */
static LRESULT CALLBACK low_level_keyboard_hook(int nCode, WPARAM wParam, LPARAM lParam)
{
	// Not a relevant event, immediately pass it on
	if (nCode != HC_ACTION)
        return CallNextHookEx(keyboard_hook, nCode, wParam, lParam);

	KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT *)lParam;
	switch (wParam) {
		case WM_KEYDOWN:
		case WM_KEYUP:
			// Intercept left/right windows keys when we have keyboard focus so Windows doesn't handle them
			if (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN) {
				bool intercept_event = false;
#ifdef USE_SDL_VIDEO
				if (sdl_window && (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_INPUT_FOCUS)) {
					intercept_event = true;
				}
#endif

				// If we've determined we should intercept the event, intercept it. But pass the event onto SDL so SheepShaver handles it.
				if (intercept_event) {
					SDL_Event e;
					memset(&e, 0, sizeof(e));
					e.type = (wParam == WM_KEYDOWN) ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
#if SDL_VERSION_ATLEAST(3, 0, 0)
					e.key.key = (p->vkCode == VK_LWIN) ? SDLK_LGUI : SDLK_RGUI;
					e.key.scancode = (p->vkCode == VK_LWIN) ? SDL_SCANCODE_LGUI : SDL_SCANCODE_RGUI;
#else
					e.key.keysym.sym = (p->vkCode == VK_LWIN) ? SDLK_LGUI : SDLK_RGUI;
					e.key.keysym.scancode = (p->vkCode == VK_LWIN) ? SDL_SCANCODE_LGUI : SDL_SCANCODE_RGUI;
#endif
					SDL_PushEvent(&e);
					return 1;
				}
			}
			break;
	}

	// If we fall here, we weren't supposed to intercept it.
	return CallNextHookEx(keyboard_hook, nCode, wParam, lParam);
}

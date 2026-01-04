/*
 *  main.cpp - macemu-next main entry point
 *
 *  Minimal Mac emulator entry point with platform adapter architecture.
 *  Uses null drivers for all platform functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <chrono>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "newcpu.h"
#include "readcpu.h"
#include "memory.h"
#include "main.h"
#include "prefs.h"
#include "video.h"
#include "xpram.h"
#include "timer.h"
#include "sony.h"
#include "disk.h"
#include "cdrom.h"
#include "scsi.h"
#include "serial.h"
#include "ether.h"
#include "clip.h"
#include "adb.h"
#include "audio.h"
#include "rom_patches.h"
#include "user_strings.h"
#include "platform.h"
#include "extfs.h"
#include "timer_interrupt.h"

#define DEBUG 1
#include "debug.h"

// Global variables (defined in basilisk_glue.cpp)
extern uint8 *RAMBaseHost;
extern uint8 *ROMBaseHost;
extern uint32 RAMSize;
extern uint32 ROMSize;

#if DIRECT_ADDRESSING
extern uintptr MEMBaseDiff;
extern uint32 RAMBaseMac;
extern uint32 ROMBaseMac;
#endif

// CPU and FPU type
extern int CPUType;
bool CPUIs68060;
extern int FPUType;
bool TwentyFourBitAddressing;

// Error handling
void ErrorAlert(const char *text)
{
	fprintf(stderr, "ERROR: %s\n", text);
}

void WarningAlert(const char *text)
{
	fprintf(stderr, "WARNING: %s\n", text);
}

// Quit emulator
void QuitEmulator(void)
{
	printf("QuitEmulator() called\n");
	ExitAll();
	exit(1);
}

// Interrupt control (minimal stubs - not in platform yet)
void DisableInterrupt(void)
{
}

void EnableInterrupt(void)
{
}

int main(int argc, char **argv)
{
	printf("=== macemu-next ===\n\n");

	// Initialize platform with null drivers
	platform_init();

	// Check for ROM file argument
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <rom-file>\n", argv[0]);
		return 1;
	}
	const char *rom_path = argv[1];

	// Initialize random number generator
	srand(time(NULL));

	// Set RAM size before PrefsInit
	RAMSize = 32 * 1024 * 1024;  // 32MB

	// Read preferences (minimal)
	PrefsInit(NULL, argc, argv);
	PrefsAddInt32("ramsize", RAMSize);
	PrefsAddInt32("cpu", 4);  // 68040
	PrefsAddBool("fpu", true);

	printf("Allocating RAM (%d MB)...\n", RAMSize / (1024 * 1024));

	// Allocate RAM
	RAMBaseHost = (uint8 *)mmap(NULL, RAMSize + 0x100000,
	                             PROT_READ | PROT_WRITE,
	                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (RAMBaseHost == MAP_FAILED) {
		fprintf(stderr, "Failed to allocate RAM\n");
		return 1;
	}

	ROMBaseHost = RAMBaseHost + RAMSize;
	memset(RAMBaseHost, 0, RAMSize);

#if DIRECT_ADDRESSING
	MEMBaseDiff = (uintptr)RAMBaseHost;
	RAMBaseMac = 0;
	ROMBaseMac = Host2MacAddr(ROMBaseHost);
#endif

	printf("RAM at %p (Mac: 0x%08x)\n", RAMBaseHost, RAMBaseMac);
	printf("ROM at %p (Mac: 0x%08x)\n", ROMBaseHost, ROMBaseMac);

	// Load ROM
	printf("\nLoading ROM from %s...\n", rom_path);
	int rom_fd = open(rom_path, O_RDONLY);
	if (rom_fd < 0) {
		fprintf(stderr, "Failed to open ROM file: %s\n", rom_path);
		return 1;
	}

	ROMSize = lseek(rom_fd, 0, SEEK_END);
	printf("ROM size: %d bytes (%d KB)\n", ROMSize, ROMSize / 1024);

	if (ROMSize != 64*1024 && ROMSize != 128*1024 && ROMSize != 256*1024 &&
	    ROMSize != 512*1024 && ROMSize != 1024*1024) {
		fprintf(stderr, "Invalid ROM size (must be 64/128/256/512/1024 KB)\n");
		close(rom_fd);
		return 1;
	}

	lseek(rom_fd, 0, SEEK_SET);
	if (read(rom_fd, ROMBaseHost, ROMSize) != (ssize_t)ROMSize) {
		fprintf(stderr, "Failed to read ROM file\n");
		close(rom_fd);
		return 1;
	}
	close(rom_fd);

	printf("ROM loaded successfully (kept in big-endian format)\n");

	// Check if this is a test ROM by looking for magic header "TROM" at offset 0x10
	// Real Mac ROMs don't have this signature
	uint32_t test_magic = ((uint32_t)ROMBaseHost[0x10] << 24) |
	                      ((uint32_t)ROMBaseHost[0x11] << 16) |
	                      ((uint32_t)ROMBaseHost[0x12] << 8) |
	                      ((uint32_t)ROMBaseHost[0x13]);
	bool is_test_rom = (test_magic == 0x54524F4D);  // "TROM"

	if (is_test_rom) {
		printf("Detected Test ROM (magic: TROM at 0x10)\n");
	}

	// ============================================================
	// Initialize Emulator (inlined from InitAll())
	// ============================================================
	printf("\n=== Initializing Emulator ===\n");

	// Check ROM version (skip for test ROMs as they may not have valid ROM version)
	if (!is_test_rom && !CheckROM()) {
		ErrorAlert(STR_UNSUPPORTED_ROM_TYPE_ERR);
		return 1;
	}

#if EMULATED_68K
	// Set CPU and FPU type (UAE emulation)
	if (!is_test_rom) {
		switch (ROMVersion) {
			case ROM_VERSION_64K:
			case ROM_VERSION_PLUS:
			case ROM_VERSION_CLASSIC:
				CPUType = 0;
				FPUType = 0;
				TwentyFourBitAddressing = true;
				break;
			case ROM_VERSION_II:
				CPUType = PrefsFindInt32("cpu");
				if (CPUType < 2) CPUType = 2;
				if (CPUType > 4) CPUType = 4;
				FPUType = PrefsFindBool("fpu") ? 1 : 0;
				if (CPUType == 4) FPUType = 1;	// 68040 always with FPU
				TwentyFourBitAddressing = true;
				break;
			case ROM_VERSION_32:
				CPUType = PrefsFindInt32("cpu");
				if (CPUType < 2) CPUType = 2;
				if (CPUType > 4) CPUType = 4;
				FPUType = PrefsFindBool("fpu") ? 1 : 0;
				if (CPUType == 4) FPUType = 1;	// 68040 always with FPU
				TwentyFourBitAddressing = false;
				break;
		}
	}
	CPUIs68060 = false;
#endif

	// Load XPRAM
	XPRAMInit(NULL);

	// Load XPRAM default values if signature not found
	if (XPRAM[0x0c] != 0x4e || XPRAM[0x0d] != 0x75
	 || XPRAM[0x0e] != 0x4d || XPRAM[0x0f] != 0x63) {
		D(bug("Loading XPRAM default values\n"));
		memset(XPRAM, 0, 0x100);
		XPRAM[0x0c] = 0x4e;	// "NuMc" signature
		XPRAM[0x0d] = 0x75;
		XPRAM[0x0e] = 0x4d;
		XPRAM[0x0f] = 0x63;
		XPRAM[0x01] = 0x80;	// InternalWaitFlags = DynWait
		XPRAM[0x10] = 0xa8;	// Standard PRAM values
		XPRAM[0x11] = 0x00;
		XPRAM[0x12] = 0x00;
		XPRAM[0x13] = 0x22;
		XPRAM[0x14] = 0xcc;
		XPRAM[0x15] = 0x0a;
		XPRAM[0x16] = 0xcc;
		XPRAM[0x17] = 0x0a;
		XPRAM[0x1c] = 0x00;
		XPRAM[0x1d] = 0x02;
		XPRAM[0x1e] = 0x63;
		XPRAM[0x1f] = 0x00;
		XPRAM[0x08] = 0x13;
		XPRAM[0x09] = 0x88;
		XPRAM[0x0a] = 0x00;
		XPRAM[0x0b] = 0xcc;
		XPRAM[0x76] = 0x00;	// OSDefault = MacOS
		XPRAM[0x77] = 0x01;
	}

	// Set boot volume
	int16 i16 = PrefsFindInt32("bootdrive");
	XPRAM[0x78] = i16 >> 8;
	XPRAM[0x79] = i16 & 0xff;
	i16 = PrefsFindInt32("bootdriver");
	XPRAM[0x7a] = i16 >> 8;
	XPRAM[0x7b] = i16 & 0xff;

	// Init drivers
	SonyInit();
	DiskInit();
	CDROMInit();
	SCSIInit();

#if SUPPORTS_EXTFS
	// Init external file system
	ExtFSInit();
#endif

	// Init serial ports
	SerialInit();

	// Init network
	EtherInit();

	// Init Time Manager
	TimerInit();

	// Init clipboard
	ClipInit();

	// Init ADB
	ADBInit();

	// Init audio
	AudioInit();

	// Init video
	if (!VideoInit(ROMVersion == ROM_VERSION_64K || ROMVersion == ROM_VERSION_PLUS || ROMVersion == ROM_VERSION_CLASSIC)) {
		fprintf(stderr, "Video initialization failed\n");
		return 1;
	}

	// Set default video mode in XPRAM
	XPRAM[0x56] = 0x42;	// 'B'
	XPRAM[0x57] = 0x32;	// '2'
	const monitor_desc &main_monitor = *VideoMonitors[0];
	XPRAM[0x58] = uint8(main_monitor.depth_to_apple_mode(main_monitor.get_current_mode().depth));
	XPRAM[0x59] = 0;

	// ============================================================
	// Select CPU Backend (before initialization)
	// ============================================================
	// CURRENT ARCHITECTURE (as of 2025):
	//   - All backends (UAE/Unicorn/DualCPU) use Init680x0()
	//   - Init680x0() sets up UAE's memory banking system
	//   - PatchROM() uses UAE's WriteMacInt*() to patch ROM
	//   - Unicorn copies the patched ROM in unicorn_backend_init()
	//
	// FUTURE ARCHITECTURE (goal for Unicorn-only):
	//   - Remove Init680x0() call for Unicorn backend
	//   - Use direct memory access (see memory_access.h)
	//   - PatchROM() uses DirectWriteMacInt*() instead of UAE functions
	//   - Completely eliminate UAE dependency for Unicorn-only builds
	//
	// To achieve this:
	//   1. Make Init680x0() conditional (only for UAE backend)
	//   2. Switch PatchROM() to use backend-independent memory functions
	//   3. Move RAM/ROM variable definitions out of basilisk_glue.cpp
	//   4. Create unicorn-specific build configuration
	// ============================================================

	// Select CPU backend via environment variable
	const char *cpu_backend = getenv("CPU_BACKEND");
	if (!cpu_backend) {
		cpu_backend = "uae";  // Default
	}

	printf("\n=== Selected CPU Backend: %s ===\n", cpu_backend);

#if EMULATED_68K
	// Init 680x0 emulation (UAE's memory banking system)
	// NOTE: Required for all backends currently, but Unicorn will use direct access in future
	if (!Init680x0()) {
		fprintf(stderr, "CPU initialization failed\n");
		return 1;
	}
#endif

	// ============================================================
	// Install CPU Backend (BEFORE PatchROM)
	// ============================================================
	// We install the backend early so that PatchROM() can use g_platform.mem_*
	// functions for backend-independent memory access
	// ============================================================

	if (strcmp(cpu_backend, "unicorn") == 0) {
		cpu_unicorn_install(&g_platform);
	} else if (strcmp(cpu_backend, "dualcpu") == 0) {
		cpu_dualcpu_install(&g_platform);
	} else {
		cpu_uae_install(&g_platform);  // Default to UAE
	}

	printf("CPU Backend: %s\n", g_platform.cpu_name);

	// Configure CPU type (must be called after backend install, before cpu_init)
	if (g_platform.cpu_set_type) {
		g_platform.cpu_set_type(CPUType, FPUType);
	}

	// ============================================================
	// Install ROM Patches
	// ============================================================
	// NOTE: PatchROM() currently still uses UAE's WriteMacInt*() functions directly.
	// The Platform API provides g_platform.mem_write_*() functions for backend-
	// independent memory access, but PatchROM() hasn't been converted yet.
	//
	// Future: Convert PatchROM() to use g_platform.mem_* functions to enable
	// Unicorn-only builds without UAE dependency.
	//
	// Patches include runtime addresses and may differ between runs due to ASLR.
	// ============================================================
	if (!is_test_rom) {
		if (!PatchROM()) {
			ErrorAlert(STR_UNSUPPORTED_ROM_TYPE_ERR);
			return 1;
		}
	} else {
		printf("Skipping ROM patches for test ROM\n");
	}

#if ENABLE_MON
	// Initialize mon
	mon_init();
	mon_read_byte = mon_read_byte_b2;
	mon_write_byte = mon_write_byte_b2;
#endif

	printf("\n=== Initialization Complete ===\n");
	if (!is_test_rom) {
		printf("ROM Version: 0x%08x\n", ROMVersion);
	}
	printf("CPU Type: 680%02d\n", (CPUType == 0) ? 0 : (CPUType * 10 + 20));
	printf("FPU: %s\n", FPUType ? "Yes" : "No");
	printf("24-bit addressing: %s\n", TwentyFourBitAddressing ? "Yes" : "No");

	// ============================================================
	// Initialize CPU Backend
	// ============================================================
	printf("\n=== Starting Emulation ===\n");

	// Backend was already installed before PatchROM() to provide memory access functions
	// Now we call cpu_init() to complete backend initialization
	if (!g_platform.cpu_init()) {
		fprintf(stderr, "Failed to initialize CPU\n");
		return 1;
	}

	// Reset CPU to ROM entry point
	g_platform.cpu_reset();
	printf("CPU reset to PC=0x%08x\n", g_platform.cpu_get_pc());

	// Set up 60Hz timer (polling-based)
	printf("\n=== Setting up Timer Interrupt ===\n");
	setup_timer_interrupt();

	// Optional auto-exit timer (set EMULATOR_TIMEOUT=2 for 2 seconds)
	const char *timeout_env = getenv("EMULATOR_TIMEOUT");
	if (timeout_env) {
		int timeout_sec = atoi(timeout_env);
		if (timeout_sec > 0) {
			printf("Auto-exit timer set: %d seconds\n", timeout_sec);
			std::thread timeout_thread([timeout_sec]() {
				std::this_thread::sleep_for(std::chrono::seconds(timeout_sec));
				fprintf(stderr, "\n[Timeout: %d seconds elapsed, exiting]\n", timeout_sec);
				exit(0);
			});
			timeout_thread.detach();
		}
	}

	// Execution loop - uses platform CPU API
	int result;
	uint64_t instruction_count = 0;

	for (;;) {
		result = g_platform.cpu_execute_one();
		instruction_count++;

		// Handle execution results
		// 0=ok, 1=stopped, 2=breakpoint, 3=exception, 4=emulop, 5=divergence
		switch (result) {
			case 0:  // CPU_EXEC_OK
				// Normal execution, continue
				continue;

			case 1:  // CPU_EXEC_STOPPED
				// CPU hit STOP instruction
				printf("\n=== CPU Stopped ===\n");
				printf("Instructions executed: %lu\n", instruction_count);
				printf("Final CPU state:\n");
				printf("  PC = 0x%08X  SR = 0x%04X\n",
					g_platform.cpu_get_pc(), g_platform.cpu_get_sr());
				printf("  D0 = 0x%08X  D1 = 0x%08X  D2 = 0x%08X  D3 = 0x%08X\n",
					g_platform.cpu_get_dreg(0), g_platform.cpu_get_dreg(1),
					g_platform.cpu_get_dreg(2), g_platform.cpu_get_dreg(3));
				printf("  D4 = 0x%08X  D5 = 0x%08X  D6 = 0x%08X  D7 = 0x%08X\n",
					g_platform.cpu_get_dreg(4), g_platform.cpu_get_dreg(5),
					g_platform.cpu_get_dreg(6), g_platform.cpu_get_dreg(7));
				printf("  A0 = 0x%08X  A1 = 0x%08X  A2 = 0x%08X  A3 = 0x%08X\n",
					g_platform.cpu_get_areg(0), g_platform.cpu_get_areg(1),
					g_platform.cpu_get_areg(2), g_platform.cpu_get_areg(3));
				printf("  A4 = 0x%08X  A5 = 0x%08X  A6 = 0x%08X  A7 = 0x%08X\n",
					g_platform.cpu_get_areg(4), g_platform.cpu_get_areg(5),
					g_platform.cpu_get_areg(6), g_platform.cpu_get_areg(7));
				goto exit_loop;

			case 4:  // CPU_EXEC_EMULOP
				// EmulOp executed and returned
				// This is normal for BasiliskII - EmulOp handles Mac OS traps
				// Keep running
				continue;

			case 3:  // CPU_EXEC_EXCEPTION
				printf("\n=== Unhandled Exception ===\n");
				printf("Instructions executed: %lu\n", instruction_count);
				printf("PC = 0x%08x\n", g_platform.cpu_get_pc());
				goto exit_loop;

			case 2:  // CPU_EXEC_BREAKPOINT
				printf("\n=== Breakpoint Hit ===\n");
				printf("PC = 0x%08x\n", g_platform.cpu_get_pc());
				goto exit_loop;

			case 5:  // CPU_EXEC_DIVERGENCE
				fprintf(stderr, "\n=== DualCPU Divergence ===\n");
				fprintf(stderr, "Instructions executed: %lu\n", instruction_count);
				goto exit_loop;
		}
	}
exit_loop:

	// Clean up timer
	printf("\n=== Shutting Down ===\n");
	stop_timer_interrupt();

	// Should never reach here
	ExitAll();
	return 0;
}

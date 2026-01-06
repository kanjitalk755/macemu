/*
 *  timer_interrupt.cpp - 60Hz timer via timerfd
 *
 *  Based on BasiliskII's tick_func() (main_unix.cpp:1492-1515)
 *  Replaces pthread with Linux timerfd for kernel-managed timing.
 *
 *  Called from CPU backend execution loops (UAE and Unicorn).
 */

#include "sysdeps.h"
#include "main.h"
#include "platform.h"
#include "timer_interrupt.h"
#include "rom_patches.h"   // For ROMVersion, ROM_VERSION_CLASSIC
#include "uae_wrapper.h"   // For intlev()
#include "macos_util.h"    // For HasMacStarted()

#include <sys/timerfd.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

// Forward declaration (avoid including timer.h due to C linkage conflicts)
extern "C" uint32 TimerDateTime(void);

// Timer state
static int timer_fd = -1;
static uint64_t interrupt_count = 0;
static uint64_t tick_counter = 0;
static bool timer_initialized = false;

extern "C" {

/*
 *  Initialize timer system
 *
 *  Creates a 60.15Hz periodic timerfd (matches BasiliskII timing)
 *  This is the master heartbeat for the emulator.
 */
void setup_timer_interrupt(void)
{
	// Create non-blocking timerfd
	timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (timer_fd < 0) {
		perror("timerfd_create");
		fprintf(stderr, "ERROR: Failed to create timer (timerfd not supported?)\n");
		return;
	}

	// Set to 60.15 Hz periodic timer (16,625 microseconds = 16,625,000 nanoseconds)
	// This matches BasiliskII's timing exactly (see main_unix.cpp:1502)
	struct itimerspec spec;
	spec.it_interval.tv_sec = 0;
	spec.it_interval.tv_nsec = 16625000;  // 60.15 Hz
	spec.it_value.tv_sec = 0;
	spec.it_value.tv_nsec = 16625000;     // Initial expiration

	if (timerfd_settime(timer_fd, 0, &spec, NULL) < 0) {
		perror("timerfd_settime");
		close(timer_fd);
		timer_fd = -1;
		return;
	}

	timer_initialized = true;
	interrupt_count = 0;
	tick_counter = 0;

	printf("Timer: Initialized 60.15 Hz timer (timerfd, fd=%d)\n", timer_fd);
}

/*
 *  one_second() - Called every 60 ticks (~1 second)
 *
 *  Matches BasiliskII main_unix.cpp:1450-1465
 */
static void one_second(void)
{
	// Pseudo Mac 1Hz interrupt, update local time
	WriteMacInt32(0x20c, TimerDateTime());

	SetInterruptFlag(INTFLAG_1HZ);
	// Note: TriggerInterrupt() will be called from poll_timer_interrupt()
}

/*
 *  one_tick() - Called every 16.625ms (60.15 Hz)
 *
 *  Matches BasiliskII main_unix.cpp:1467-1490
 */
static void one_tick(void)
{
	// Every 60 ticks = ~1 second
	if (++tick_counter >= 60) {
		tick_counter = 0;
		one_second();
	}

	// Set 60Hz interrupt flag
	SetInterruptFlag(INTFLAG_60HZ);

	// Trigger CPU-level interrupt via platform API
	// IMPORTANT: Only trigger interrupts after Mac has booted!
	// This matches BasiliskII's check (main_unix.cpp:1486)
	//
	// During early boot, Mac ROM initializes interrupt vectors and other critical
	// state. Taking interrupts too early can cause crashes or divergence between
	// different CPU backends. BasiliskII checks HasMacStarted() before triggering
	// 60Hz interrupts.
	//
	// The ROM_VERSION_CLASSIC check allows Classic Mac ROMs (Plus, SE) to bypass
	// this guard as they have different boot sequences.

	// Debug: Check what HasMacStarted returns
	static bool debug_logged = false;
	bool mac_started = HasMacStarted();
	if (!debug_logged && interrupt_count < 5) {
		uint32_t cfc_value = ReadMacInt32(0xcfc);
		fprintf(stderr, "[DEBUG Timer] tick=%llu ROM=0x%04x mac_started=%d cfc=0x%08x (expected 0x574C5343='WLSC')\n",
		        (unsigned long long)interrupt_count, ROMVersion, mac_started, cfc_value);
		if (interrupt_count >= 4) debug_logged = true;
	}

	if (ROMVersion != ROM_VERSION_CLASSIC || mac_started) {
		extern Platform g_platform;
		if (g_platform.cpu_trigger_interrupt) {
			int level = intlev();
			if (level > 0) {
				g_platform.cpu_trigger_interrupt(level);
			}
		}
	}
}

/*
 *  Poll timer - call from CPU execution loop
 *
 *  Returns number of timer expirations (usually 0 or 1, but can be >1 if system is lagging)
 */
uint64_t poll_timer_interrupt(void)
{
	// Suppress timer interrupts during CPU tracing for deterministic execution
	static bool tracing_mode = (getenv("CPU_TRACE") != NULL);
	if (tracing_mode) {
		return 0;  // No interrupts during tracing
	}

	if (!timer_initialized || timer_fd < 0) {
		return 0;
	}

	// Read from timerfd (non-blocking)
	uint64_t expirations;
	ssize_t ret = read(timer_fd, &expirations, sizeof(expirations));

	if (ret < 0) {
		if (errno != EAGAIN) {
			// Real error (not just "no data available")
			perror("timerfd read");
		}
		return 0;  // No timer expiration yet
	}

	// Timer fired! Process each expiration
	for (uint64_t i = 0; i < expirations; i++) {
		one_tick();
		interrupt_count++;
	}

	// If expirations > 1, we're lagging (CPU can't keep up with real-time)
	if (expirations > 1) {
		static bool warned = false;
		if (!warned) {
			fprintf(stderr, "Timer: Warning - System lagging (%llu missed ticks)\n",
			        (unsigned long long)(expirations - 1));
			warned = true;  // Only warn once
		}
	}

	return expirations;
}

/*
 *  Stop timer
 */
void stop_timer_interrupt(void)
{
	if (!timer_initialized) {
		return;
	}

	if (timer_fd >= 0) {
		close(timer_fd);
		timer_fd = -1;
	}

	timer_initialized = false;
	printf("Timer: Stopped after %llu interrupts (%llu seconds)\n",
	       (unsigned long long)interrupt_count,
	       (unsigned long long)interrupt_count / 60);
}

/*
 *  Get statistics
 */
uint64_t get_timer_interrupt_count(void)
{
	return interrupt_count;
}

}  // extern "C"

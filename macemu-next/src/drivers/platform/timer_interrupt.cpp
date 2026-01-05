/*
 *  timer_interrupt.cpp - 60Hz timer via polling
 *
 *  Checks wall-clock time to generate periodic interrupts.
 *  Called from CPU backend execution loops (UAE and Unicorn).
 *
 *  This replaces the previous SIGALRM-based approach which had issues
 *  with signal masking and async-signal-safe constraints.
 */

#include "sysdeps.h"
#include "main.h"
#include "platform.h"
#include "timer_interrupt.h"
#include "uae_wrapper.h"  // For intlev()
#include "macos_util.h"   // For HasMacStarted()
#include "cpu_trace.h"    // For cpu_trace_should_log()
#include <time.h>
#include <stdio.h>

// Timer state
static uint64_t last_timer_ns = 0;
static uint64_t interrupt_count = 0;
static bool timer_initialized = false;

extern "C" {

/*
 *  Initialize timer system
 */
void setup_timer_interrupt(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	last_timer_ns = now.tv_sec * 1000000000ULL + now.tv_nsec;
	interrupt_count = 0;
	timer_initialized = true;

	printf("Timer: Initialized 60 Hz timer (polling-based)\n");
}

/*
 *  Poll timer - call from CPU execution loop
 *  Returns number of timer expirations (usually 0 or 1)
 */
uint64_t poll_timer_interrupt(void)
{
	if (!timer_initialized) {
		return 0;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint64_t now_ns = now.tv_sec * 1000000000ULL + now.tv_nsec;

	// Check if 16.667ms have passed (60 Hz)
	uint64_t elapsed = now_ns - last_timer_ns;
	if (elapsed < 16667000ULL) {
		return 0;  // Not time yet
	}

	// Timer fired! Update last fire time
	last_timer_ns = now_ns;
	interrupt_count++;

	// Set Mac-level interrupt flag (for video/audio callbacks)
	SetInterruptFlag(INTFLAG_60HZ);

	// Trigger CPU-level interrupt via platform API
	// This works for both UAE and Unicorn backends:
	// - UAE: Sets SPCFLAG_INT, will be processed by do_specialties()
	// - Unicorn: Sets g_pending_interrupt_level, will be checked by hook_block()
	//
	// IMPORTANT: Only trigger interrupts after Mac has booted!
	// During early boot, Mac ROM initializes interrupt vectors and other critical
	// state. Taking interrupts too early can cause crashes or divergence between
	// different CPU backends. BasiliskII checks HasMacStarted() before triggering
	// 60Hz interrupts (see main_unix.cpp:1486).
	//
	// ALSO: Disable interrupts during CPU tracing for deterministic traces.
	// Timer interrupts cause non-deterministic behavior between backends due to
	// timing differences in when poll_timer_interrupt() is called, leading to
	// trace divergence. When CPU_TRACE is active, suppress timer interrupts.
	if (HasMacStarted() && !cpu_trace_is_enabled()) {
		extern Platform g_platform;
		if (g_platform.cpu_trigger_interrupt) {
			int level = intlev();
			if (level > 0) {
				g_platform.cpu_trigger_interrupt(level);
			}
		}
	}

	return 1;  // One expiration
}

/*
 *  Stop timer
 */
void stop_timer_interrupt(void)
{
	if (!timer_initialized) {
		return;
	}

	timer_initialized = false;
	printf("Timer: Stopped after %llu interrupts\n",
	       (unsigned long long)interrupt_count);
}

/*
 *  Get statistics
 */
uint64_t get_timer_interrupt_count(void)
{
	return interrupt_count;
}

}  // extern "C"

/*
 *  timer_interrupt.cpp - 60Hz timer interrupt implementation
 *
 *  Uses POSIX setitimer() with SIGALRM to generate periodic interrupts
 *  for Mac VBL (Vertical Blank) timing.
 */

#include "sysdeps.h"
#include "main.h"
#include "platform.h"
#include "uae_wrapper.h"  // For intlev()
#include <signal.h>
#include <sys/time.h>
#include <stdio.h>

// Local variables
static bool timer_installed = false;
static uint64_t interrupt_count = 0;

/*
 *  Timer signal handler
 *
 *  Called by kernel when SIGALRM fires (every 16.667ms for 60Hz).
 *  This handler must be async-signal-safe!
 */
static void timer_signal_handler(int signum)
{
	// Set Mac interrupt flag for video/audio callbacks
	// This will be checked in emul_op.cpp's EmulOp() handler
	SetInterruptFlag(INTFLAG_60HZ);

	// Trigger CPU-level interrupt via platform API
	// This works for both UAE and Unicorn backends:
	// - UAE: Sets SPCFLAG_INT, will be processed by do_specialties()
	// - Unicorn: Sets g_pending_interrupt_level, will be checked by hook_block()
	extern Platform g_platform;
	if (g_platform.cpu_trigger_interrupt) {
		int level = intlev();
		if (level > 0) {
			g_platform.cpu_trigger_interrupt(level);
		}
	}

	// Increment counter (for statistics)
	interrupt_count++;
}

/*
 *  Setup timer interrupt
 *
 *  interval_us: Interrupt interval in microseconds (16667 for 60Hz)
 */
bool setup_timer_interrupt(int interval_us)
{
	if (timer_installed) {
		fprintf(stderr, "Timer: Already installed\n");
		return false;
	}

	// Register signal handler
	struct sigaction sa;
	sa.sa_handler = timer_signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;  // Restart interrupted syscalls

	if (sigaction(SIGALRM, &sa, NULL) < 0) {
		perror("Timer: sigaction failed");
		return false;
	}

	// Set up periodic timer using setitimer
	struct itimerval timer;
	timer.it_value.tv_sec = 0;
	timer.it_value.tv_usec = interval_us;     // Initial delay
	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_usec = interval_us;  // Periodic interval

	if (setitimer(ITIMER_REAL, &timer, NULL) < 0) {
		perror("Timer: setitimer failed");
		return false;
	}

	timer_installed = true;
	interrupt_count = 0;

	printf("Timer: Installed %d Hz interrupt (%d microseconds)\n",
	       1000000 / interval_us, interval_us);

	return true;
}

/*
 *  Stop timer interrupt
 */
void stop_timer_interrupt(void)
{
	if (!timer_installed) {
		return;
	}

	// Disable timer
	struct itimerval timer;
	timer.it_value.tv_sec = 0;
	timer.it_value.tv_usec = 0;
	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_usec = 0;

	setitimer(ITIMER_REAL, &timer, NULL);

	// Restore default SIGALRM handler
	signal(SIGALRM, SIG_DFL);

	timer_installed = false;

	printf("Timer: Stopped after %llu interrupts\n",
	       (unsigned long long)interrupt_count);
}

/*
 *  Get timer statistics
 */
uint64_t get_timer_interrupt_count(void)
{
	return interrupt_count;
}

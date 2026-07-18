/*
 *  timer_windows.cpp - Time Manager emulation, Windows specific stuff
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
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

#include "main.h"
#include "macos_util.h"
#include "timer.h"
#include "audio.h"

/* Always include when available so DESCENT_MOVIE_DIAGNOSTICS is independent
 * of cmake wait-logging. SheepShaver stages this header; Basilisk II may not. */
#if defined(SHEEPSHAVER) || (defined(QD3D_WAIT_LOGGING_ENABLED) && QD3D_WAIT_LOGGING_ENABLED)
#include "gfx_log.h"
#else
#ifndef QD3D_WAIT_LOGGING_ENABLED
#define QD3D_WAIT_LOGGING_ENABLED 0
#endif
#ifndef DESCENT_MOVIE_DIAGNOSTICS
#define DESCENT_MOVIE_DIAGNOSTICS 0
#endif
#define QD3D_WAIT_LOG(...) do { } while (0)
#endif
#ifndef DESCENT_MOVIE_USEC_COALESCE_US
/* Coalesce Microseconds under thrash: guest GetTime/sync loops can hammer
 * A193 tens of thousands of times/sec and starve drawing + audio IRQ.
 * Applies whenever any sound source is active (all apps). 0 disables. */
#define DESCENT_MOVIE_USEC_COALESCE_US 1000
#endif
#if QD3D_WAIT_LOGGING_ENABLED
static bool timer_windows_descent_ii_is_current_application()
{
	return ReadMacInt32(0x0910) == 0x0a446573 &&
	       ReadMacInt32(0x0914) == 0x63656e74 &&
	       (ReadMacInt32(0x0918) & 0xffffff00) == 0x20494900;
}
#endif

#define DEBUG 0
#include "debug.h"


// Helper time functions
#define MSECS2TICKS(MSECS) (((uint64)(MSECS) * frequency) / 1000)
#define USECS2TICKS(USECS) (((uint64)(USECS) * frequency) / 1000000)
#define TICKS2USECS(TICKS) (((uint64)(TICKS) * 1000000) / frequency)

// From main_windows.cpp
extern HANDLE emul_thread;

// Global variables
static uint32 frequency;				// CPU frequency in Hz (< 4 GHz)
static tm_time_t mac_boot_ticks;
static tm_time_t mac_1904_ticks;
static tm_time_t mac_now_diff;


/*
 *  Initialize native Windows timers
 */

void timer_init(void)
{
	D(bug("SysTimerInit\n"));

	LARGE_INTEGER tt;
	if (!QueryPerformanceFrequency(&tt)) {
		ErrorAlert("No high resolution timers available\n");
		QuitEmulator();
	}
	frequency = tt.LowPart;
	D(bug(" frequency %d\n", frequency));

	// mac_boot_ticks is 1.18 us since Basilisk II was started
	QueryPerformanceCounter(&tt);
	mac_boot_ticks = tt.QuadPart;

	// mac_1904_ticks is 1.18 us since Mac time started 1904
	mac_1904_ticks = time(NULL) * frequency;
	mac_now_diff = mac_1904_ticks - mac_boot_ticks;
}


  /*
 *  Return microseconds since boot (64 bit)
 */

void Microseconds(uint32 &hi, uint32 &lo)
{
	D(bug("Microseconds\n"));
#if DESCENT_MOVIE_USEC_COALESCE_US > 0
	/* Any active sound source can host a busy-poll sync loop. Coalesce the
	 * host QPC sample so the emul thread has time for audio IRQ + drawing. */
	if (AudioStatus.num_sources >= 1) {
		static uint32 cache_hi, cache_lo;
		static uint64 cache_qpc;
		static bool cache_valid;
		LARGE_INTEGER now_qpc;
		QueryPerformanceCounter(&now_qpc);
		const uint64 min_delta =
			(uint64)DESCENT_MOVIE_USEC_COALESCE_US * frequency / 1000000ull;
		if (cache_valid &&
		    (uint64)now_qpc.QuadPart - cache_qpc < min_delta &&
		    min_delta > 0) {
			hi = cache_hi;
			lo = cache_lo;
			return;
		}
		LARGE_INTEGER tt;
		tt.QuadPart = TICKS2USECS(now_qpc.QuadPart - mac_boot_ticks);
		hi = tt.HighPart;
		lo = tt.LowPart;
		cache_hi = hi;
		cache_lo = lo;
		cache_qpc = (uint64)now_qpc.QuadPart;
		cache_valid = true;
#if QD3D_WAIT_LOGGING_ENABLED
		goto descent_usec_logged;
#else
		return;
#endif
	}
#endif
	{
	LARGE_INTEGER tt;
	QueryPerformanceCounter(&tt);
	tt.QuadPart = TICKS2USECS(tt.QuadPart - mac_boot_ticks);
	hi = tt.HighPart;
	lo = tt.LowPart;
	}
#if QD3D_WAIT_LOGGING_ENABLED
#if DESCENT_MOVIE_USEC_COALESCE_US > 0
descent_usec_logged:
#endif
	static uint32 last_tick;
	static uint32 calls_this_tick;
	static uint64 first_value;
	static uint64 last_value;
	if (timer_windows_descent_ii_is_current_application()) {
		const uint32 tick = ReadMacInt32(0x016a);
		const uint64 value = ((uint64)hi << 32) | lo;
		if (calls_this_tick && tick != last_tick) {
			QD3D_WAIT_LOG("Microseconds polling tick=%u calls=%u first=%llu last=%llu spanUsec=%llu",
			              last_tick, calls_this_tick,
			              (unsigned long long)first_value,
			              (unsigned long long)last_value,
			              (unsigned long long)(last_value - first_value));
			calls_this_tick = 0;
		}
		if (!calls_this_tick)
			first_value = value;
		last_tick = tick;
		last_value = value;
		calls_this_tick++;
#if DESCENT_MOVIE_DIAGNOSTICS
		// One-shot dump of the movie busy-wait loop code. A burst of 500+
		// Microseconds() calls within one tick only happens inside Descent's
		// movie-startup polling loop; the loop code has been observed at
		// 0x232450..0x2356ac across launches.
		static bool movie_loop_dumped;
		if (!movie_loop_dumped && calls_this_tick == 500) {
			movie_loop_dumped = true;
			const uint32 base = 0x00230000, size = 0x8000;
			if (uint8 *host = Mac2HostAddr(base)) {
				if (FILE *f = fopen("descent_movie_loop.bin", "wb")) {
					fwrite(host, 1, size, f);
					fclose(f);
					QD3D_WAIT_LOG("Movie-loop dump guestBase=0x%08x bytes=0x%x tick=%u",
					              base, size, tick);
				}
			}
		}
#endif
	} else {
		calls_this_tick = 0;
	}
#endif
}


/*
 *  Uncoalesced Microseconds: always a fresh QPC read, no 1 ms coalesce.
 *
 *  The coalesce in Microseconds() returns an identical value for up to 1 ms.
 *  A caller that busy-waits for the clock to advance past a target (QuickTime's
 *  movie sound clock) then spins tens of thousands of times per second while
 *  the value is frozen. This path gives it a monotonic, fine-grained value so
 *  each wait resolves in far fewer polls.
 */

void MicrosecondsRaw(uint32 &hi, uint32 &lo)
{
	LARGE_INTEGER tt;
	QueryPerformanceCounter(&tt);
	tt.QuadPart = TICKS2USECS(tt.QuadPart - mac_boot_ticks);
	hi = tt.HighPart;
	lo = tt.LowPart;
}


/*
 *  Return local date/time in Mac format (seconds since 1.1.1904)
 */

uint32 TimerDateTime(void)
{
	return TimeToMacTime(time(NULL));
}


/*
 *  Get current time
 */

void timer_current_time(tm_time_t &t)
{
	LARGE_INTEGER tt;
	QueryPerformanceCounter(&tt);
	t = tt.QuadPart + mac_now_diff;
}


/*
 *  Add times
 */

void timer_add_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res = a + b;
}


/*
 *  Subtract times
 */

void timer_sub_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res = a - b;
}


/*
 *  Compare times (<0: a < b, =0: a = b, >0: a > b)
 */

int timer_cmp_time(tm_time_t a, tm_time_t b)
{
	tm_time_t r = a - b;
	return r < 0 ? -1 : (r > 0 ? 1 : 0);
}


/*
 *  Convert Mac time value (>0: microseconds, <0: microseconds) to tm_time_t
 */

void timer_mac2host_time(tm_time_t &res, int32 mactime)
{
	if (mactime > 0) {
		// Time in milliseconds
		res = MSECS2TICKS(mactime);
	} else {
		// Time in negative microseconds
		res = USECS2TICKS(-mactime);
	}
}


/*
 *  Convert positive tm_time_t to Mac time value (>0: microseconds, <0: microseconds)
 *  A negative input value for hosttime results in a zero return value
 *  As long as the microseconds value fits in 32 bit, it must not be converted to milliseconds!
 */

int32 timer_host2mac_time(tm_time_t hosttime)
{
	if (hosttime < 0)
		return 0;
	else {
		uint64 t = TICKS2USECS(hosttime);
		if (t > 0x7fffffff)
			return int32(t / 1000);	// Time in milliseconds
		else
			return -int32(t);			// Time in negative microseconds
	}
}


/*
 *  Get current value of microsecond timer
 */

uint64 GetTicks_usec(void)
{
	LARGE_INTEGER tt;
	QueryPerformanceCounter(&tt);
	return TICKS2USECS(tt.QuadPart - mac_boot_ticks);
}


#if QD3D_WAIT_LOGGING_ENABLED
static void log_idle_wait_complete(uint64 started, bool descent_was_current)
{
	if (!descent_was_current)
		return;

	static uint32 wait_count;
	static uint64 total_wait_usec;
	static uint64 max_wait_usec;
	static uint32 last_log_tick;
	const uint64 wait_usec = GetTicks_usec() - started;
	const uint32 tick = ReadMacInt32(0x016a);
	wait_count++;
	total_wait_usec += wait_usec;
	if (wait_usec > max_wait_usec)
		max_wait_usec = wait_usec;
	if (wait_usec >= 50000 || tick - last_log_tick >= 30) {
		QD3D_WAIT_LOG("Idle/Event wait tick=%u waits=%u totalUsec=%llu maxUsec=%llu lastUsec=%llu",
		              tick, wait_count, (unsigned long long)total_wait_usec,
		              (unsigned long long)max_wait_usec,
		              (unsigned long long)wait_usec);
		wait_count = 0;
		total_wait_usec = 0;
		max_wait_usec = 0;
		last_log_tick = tick;
	}
}
#endif


/*
 *  Delay by specified number of microseconds (<1 second)
 */

void Delay_usec(uint32 usec)
{
	// FIXME: fortunately, Delay_usec() is generally used with
	// millisecond resolution anyway
	Sleep(usec / 1000);
}


/*
 *  Suspend emulator thread, virtual CPU in idle mode
 */

struct idle_sentinel {
	idle_sentinel();
	~idle_sentinel();
};
static idle_sentinel idle_sentinel;

static int idle_sem_ok = -1;
static HANDLE idle_sem = NULL;

static HANDLE idle_lock = NULL;
#define LOCK_IDLE WaitForSingleObject(idle_lock, INFINITE)
#define UNLOCK_IDLE ReleaseMutex(idle_lock)

idle_sentinel::idle_sentinel()
{
	idle_sem_ok = 1;
	if ((idle_sem = CreateSemaphore(0, 0, 1, NULL)) == NULL)
		idle_sem_ok = 0;
	if ((idle_lock = CreateMutex(NULL, FALSE, NULL)) == NULL)
		idle_sem_ok = 0;
}

idle_sentinel::~idle_sentinel()
{
	if (idle_lock) {
		ReleaseMutex(idle_lock);
		CloseHandle(idle_lock);
	}
	if (idle_sem) {
		ReleaseSemaphore(idle_sem, 1, NULL);
		CloseHandle(idle_sem);
	}
}

void idle_wait(void)
{
#if QD3D_WAIT_LOGGING_ENABLED
	const bool log_descent_wait = timer_windows_descent_ii_is_current_application();
	const uint64 wait_started = GetTicks_usec();
#endif
	LOCK_IDLE;
	if (idle_sem_ok > 0) {
		idle_sem_ok++;
		UNLOCK_IDLE;
		WaitForSingleObject(idle_sem, INFINITE);
#if QD3D_WAIT_LOGGING_ENABLED
		log_idle_wait_complete(wait_started, log_descent_wait);
#endif
		return;
	}
	UNLOCK_IDLE;

	// Fallback: sleep 10 ms (this should not happen though)
	Delay_usec(10000);
#if QD3D_WAIT_LOGGING_ENABLED
	log_idle_wait_complete(wait_started, log_descent_wait);
#endif
}


/*
 *  Resume execution of emulator thread, events just arrived
 */

void idle_resume(void)
{
	LOCK_IDLE;
	if (idle_sem_ok > 1) {
		idle_sem_ok--;
		UNLOCK_IDLE;
		ReleaseSemaphore(idle_sem, 1, NULL);
		return;
	}
	UNLOCK_IDLE;
}

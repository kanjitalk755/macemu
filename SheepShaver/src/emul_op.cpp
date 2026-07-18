/*
 *  emul_op.cpp - 68k opcodes for ROM patches
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

#include <stdio.h>
#include <cstdio>

#include "sysdeps.h"
#include "main.h"
#include "version.h"
#include "prefs.h"
#include "cpu_emulation.h"
#include "xlowmem.h"
#include "xpram.h"
#include "timer.h"
#include "adb.h"
#include "sony.h"
#include "disk.h"
#include "cdrom.h"
#include "scsi.h"
#include "video.h"
#include "audio.h"
#include "ether.h"
#include "serial.h"
#include "clip.h"
#include "extfs.h"
#include "macos_util.h"
#include "rom_patches.h"
#include "rsrc_patches.h"
#include "name_registry.h"
#include "user_strings.h"
#include "emul_op.h"
#include "thunks.h"

/* Always pull the header so DESCENT_MOVIE_DIAGNOSTICS works without cmake
 * wait-logging. Channel macros still compile out when their switches are off. */
#include "qd3d_init_logging.h"
#if QD3D_WAIT_LOGGING_ENABLED
static bool emul_op_descent_ii_is_current_application()
{
	return ReadMacInt32(0x0910) == 0x0a446573 &&
	       ReadMacInt32(0x0914) == 0x63656e74 &&
	       (ReadMacInt32(0x0918) & 0xffffff00) == 0x20494900;
}
#endif

#if DESCENT_MOVIE_DIAGNOSTICS
/*
 * Descent MVE Microseconds path (d2.log 2026-07-16+):
 *
 * Glue (ret): watermark last(pc); jsr ROM Microseconds; not a future deadline.
 * App "caller" (a7+12) is GetTime: fills TimeRecord with wall µs, scale=1e6,
 * then rtd #8. Stack under EMUL_OP (GetTime did link/#0, push a4, push TR*,
 * jsr glue, glue push hi/lo, jsr ROM):
 *   +0 ret glue, +4 lo, +8 hi, +12 ret GetTime, +16 TimeRecord*,
 *   +20 saved a4, +24 GetTime's caller's a6, +28 ret into GetTime's caller
 *   (the outer spin).
 *
 * Extra logging (session request): wait enter/leave, non-clk state progress,
 * spin-site changes, heartbeats, enriched tickSummary.
 */
static void descent_movie_diag_puts(const char *msg)
{
	std::fputs(msg, stderr);
	std::fflush(stderr);
#ifdef _WIN32
	OutputDebugStringA(msg);
#endif
}

static bool descent_movie_addr_ok(uint32 a)
{
	return a >= 0x1000 && a < 0x40000000;
}

/* Big-endian guest 64-bit (hi,lo) as signed. */
static int64 descent_movie_s64(uint32 hi, uint32 lo)
{
	return int64((uint64(hi) << 32) | uint64(lo));
}

/* Signed 64-bit three-way compare: -1 a<b, 0 equal, +1 a>b. */
static int descent_movie_cmp64(int64 a, int64 b)
{
	if (a < b)
		return -1;
	if (a > b)
		return 1;
	return 0;
}

static void descent_movie_log_microseconds_frame(M68kRegisters *r)
{
	if (AudioStatus.num_sources < 2)
		return;

	static uint32 burst_calls;
	static uint32 burst_get_time;
	static uint32 burst_other;
	static uint32 burst_tick;
	static uint32 movie_phase_calls;
	static int64 first_rem = -1;
	static int64 last_rem = -1;
	static int64 max_rem = -1;
	static uint64 first_deadline;
	static uint64 prev_deadline;
	static bool dumped_glue;
	static uint32 raw_logs_left = 48;
	static uint32 known_ret;
	static uint32 known_dl_addr;

	/* GetTime session tracking (outer wait active while GetTime is the caller). */
	static bool in_gettime_session;
	static uint64 gettime_session_start_wall;
	static uint32 gettime_session_start_n;
	static uint32 gettime_session_calls;
	static uint32 gettime_session_id;
	static uint64 last_gettime_wall;
	static uint32 last_spin_pc;
	static uint32 last_state_ptr;
	static uint64 last_heartbeat_wall;
	static uint32 heartbeat_calls;
	static uint64 last_clk_sample;
	static uint64 last_clk_wall;
	static bool have_clk_sample;
	static uint32 state_progress_events;
	static uint32 spin_site_events;

	const uint32 tick = ReadMacInt32(0x016a);
	if (tick != burst_tick) {
		if (burst_calls >= 20) {
			char sum[512];
			std::snprintf(sum, sizeof(sum),
			              "[QD3D:wait] MovieWait tickSummary tick=%u calls=%u "
			              "getTime=%u other=%u firstGap=%lld lastGap=%lld maxGap=%lld "
			              "last=%llu progressEv=%u spinSiteEv=%u\n",
			              burst_tick, burst_calls, burst_get_time, burst_other,
			              (long long)first_rem, (long long)last_rem,
			              (long long)max_rem, (unsigned long long)prev_deadline,
			              state_progress_events, spin_site_events);
			descent_movie_diag_puts(sum);
		}
		burst_tick = tick;
		burst_calls = 0;
		burst_get_time = 0;
		burst_other = 0;
		first_rem = -1;
		last_rem = -1;
		max_rem = -1;
	}
	burst_calls++;
	movie_phase_calls++;
	heartbeat_calls++;

	const uint64 now = (uint64(r->a[0]) << 32) | uint64(r->d[0]);
	const uint32 a7 = r->a[7];

	/*
	 * Soft-trap stack under jsr Microseconds from the time-gate glue:
	 *   (a7)+0  = ret into glue (after jsr)
	 *   (a7)+4  = pushed UW lo
	 *   (a7)+8  = pushed UW hi
	 *   (a7)+12 = return into *caller of the glue*  ← who is spinning
	 */
	uint32 ret = 0, caller = 0, stk4 = 0, stk8 = 0;
	if (descent_movie_addr_ok(a7) && a7 < 0x40000000 - 16) {
		ret = ReadMacInt32(a7);
		stk4 = ReadMacInt32(a7 + 4);
		stk8 = ReadMacInt32(a7 + 8);
		caller = ReadMacInt32(a7 + 12);
	}

	/* Discover last-time UW address: ret-0x16 from disassembly, verify lea. */
	if (descent_movie_addr_ok(ret) && known_ret != ret) {
		known_ret = ret;
		known_dl_addr = ret - 0x16;
		const uint32 lea_at = ret - 0x0E;
		if (descent_movie_addr_ok(lea_at) && ReadMacInt16(lea_at) == 0x43FA) {
			const int16 disp = int16(ReadMacInt16(lea_at + 2));
			known_dl_addr = uint32(int32(lea_at + 2) + disp);
		}
		if (!dumped_glue) {
			dumped_glue = true;
			const uint32 base = ret > 0x80 ? ret - 0x80 : 0x1000;
			if (uint8 *host = Mac2HostAddr(base)) {
				if (FILE *f = fopen("descent_micros_glue.bin", "wb")) {
					fwrite(host, 1, 0x100, f);
					fclose(f);
				}
			}
			char msg[384];
			std::snprintf(msg, sizeof(msg),
			              "[QD3D:wait] MovieWait glue ret=0x%08x lastAddr=0x%08x fileBase=0x%08x "
			              "lea=%04x caller=0x%08x now=%llu\n",
			              ret, known_dl_addr, base,
			              descent_movie_addr_ok(ret - 0x0E) ? ReadMacInt16(ret - 0x0E) : 0,
			              caller, (unsigned long long)now);
			descent_movie_diag_puts(msg);
		}
	}

	uint64 last_t = 0;
	int64 delta = 0; /* now - last; always ≥0 if last is a watermark */
	if (known_dl_addr && descent_movie_addr_ok(known_dl_addr)) {
		last_t = (uint64(ReadMacInt32(known_dl_addr)) << 32) |
		         uint64(ReadMacInt32(known_dl_addr + 4));
		delta = int64(now) - int64(last_t);
		prev_deadline = last_t;
		if (movie_phase_calls == 1)
			first_deadline = last_t;
		if (delta > max_rem)
			max_rem = delta;
		if (first_rem < 0)
			first_rem = delta;
		last_rem = delta;
	}

	/* Stack words: +12 GetTime ret, +16 TimeRecord*, +20 saved a4,
	 * +24 outer a6, +28 outer ret (spin site after GetTime call). */
	static uint32 caller_pc[8];
	static uint32 caller_n[8];
	static uint32 caller_dumped_mask;
	static uint32 deep[6];
	for (int i = 0; i < 6; i++)
		deep[i] = 0;
	if (descent_movie_addr_ok(a7) && a7 < 0x40000000 - 40) {
		for (int i = 0; i < 6; i++)
			deep[i] = ReadMacInt32(a7 + 12 + uint32(i * 4));
	}

	/* Detect GetTime return site: next insn is movea.l (sp)+,a1 (0x225F). */
	const bool looks_like_gettime =
		descent_movie_addr_ok(caller) && ReadMacInt16(caller) == 0x225F;
	if (looks_like_gettime)
		burst_get_time++;
	else
		burst_other++;

	bool first_sight_caller = false;
	if (descent_movie_addr_ok(caller)) {
		int slot = -1;
		for (int i = 0; i < 8; i++) {
			if (caller_pc[i] == caller) {
				slot = i;
				break;
			}
			if (slot < 0 && caller_n[i] == 0)
				slot = i;
		}
		if (slot >= 0) {
			if (caller_n[slot] == 0)
				first_sight_caller = true;
			caller_pc[slot] = caller;
			caller_n[slot]++;
			if (first_sight_caller && !(caller_dumped_mask & (1u << slot))) {
				caller_dumped_mask |= (1u << slot);
				const uint32 cbase = caller > 0x800 ? caller - 0x800 : 0x1000;
				char fname[64];
				std::snprintf(fname, sizeof(fname),
				              "descent_micros_caller_%u.bin", slot);
				if (uint8 *host = Mac2HostAddr(cbase)) {
					if (FILE *f = fopen(fname, "wb")) {
						fwrite(host, 1, 0x1000, f);
						fclose(f);
					}
				}
				char m2[320];
				std::snprintf(m2, sizeof(m2),
				              "[QD3D:wait] MovieWait callerDump slot=%u pc=0x%08x "
				              "fileBase=0x%08x getTime=%d "
				              "deep=%08x/%08x/%08x/%08x/%08x/%08x\n",
				              slot, caller, cbase, looks_like_gettime ? 1 : 0,
				              deep[0], deep[1], deep[2], deep[3], deep[4], deep[5]);
				descent_movie_diag_puts(m2);
			}
		}
	}

	/* Close GetTime session if wall has advanced a lot without GetTime. */
	if (in_gettime_session && !looks_like_gettime && last_gettime_wall) {
		const int64 idle = int64(now) - int64(last_gettime_wall);
		if (idle > 5000) {
			char msg[384];
			std::snprintf(msg, sizeof(msg),
			              "[QD3D:wait] MovieWait waitLeave id=%u reason=idle "
			              "calls=%u n0=%u n1=%u wall0=%llu wall1=%llu durUs=%lld "
			              "tick=%u\n",
			              gettime_session_id, gettime_session_calls,
			              gettime_session_start_n, movie_phase_calls,
			              (unsigned long long)gettime_session_start_wall,
			              (unsigned long long)now, (long long)idle, tick);
			descent_movie_diag_puts(msg);
			in_gettime_session = false;
			gettime_session_calls = 0;
		}
	}

	/*
	 * Outer wait state (a4 @ 8(outer_a6)): clock wait compares
	 * state+0x1c / +0x24 (deadlines) to converted clock "now".
	 */
	static bool dumped_outer;
	if (looks_like_gettime && movie_phase_calls >= 10) {
		const uint32 tr = deep[1];
		const uint32 outer_a6 = deep[3];
		const uint32 outer_ret = deep[4];
		uint32 spin_pc = outer_ret;
		if (!descent_movie_addr_ok(spin_pc) || spin_pc < 0x2000 ||
		    spin_pc >= 0x40000000 ||
		    (spin_pc >= 0x1c00000 && spin_pc < 0x20000000)) {
			if (descent_movie_addr_ok(outer_a6))
				spin_pc = ReadMacInt32(outer_a6 + 4);
		}

		uint32 state = 0;
		if (descent_movie_addr_ok(outer_a6))
			state = ReadMacInt32(outer_a6 + 8);

		/* Session enter */
		if (!in_gettime_session) {
			in_gettime_session = true;
			gettime_session_id++;
			gettime_session_start_wall = now;
			gettime_session_start_n = movie_phase_calls;
			gettime_session_calls = 0;
			char msg[384];
			std::snprintf(msg, sizeof(msg),
			              "[QD3D:wait] MovieWait waitEnter id=%u n=%u wall=%llu "
			              "spinPc=0x%08x outerA6=0x%08x state=0x%08x getTime=0x%08x "
			              "tick=%u\n",
			              gettime_session_id, movie_phase_calls,
			              (unsigned long long)now, spin_pc, outer_a6, state,
			              caller, tick);
			descent_movie_diag_puts(msg);
		}
		gettime_session_calls++;
		last_gettime_wall = now;

		/* Spin site change = wait advanced to a different poll PC */
		if (spin_pc && spin_pc != last_spin_pc) {
			spin_site_events++;
			char msg[320];
			std::snprintf(msg, sizeof(msg),
			              "[QD3D:wait] MovieWait spinSite id=%u from=0x%08x to=0x%08x "
			              "n=%u wall=%llu tick=%u\n",
			              gettime_session_id, last_spin_pc, spin_pc,
			              movie_phase_calls, (unsigned long long)now, tick);
			descent_movie_diag_puts(msg);
			last_spin_pc = spin_pc;
		}
		if (state && state != last_state_ptr) {
			char msg[256];
			std::snprintf(msg, sizeof(msg),
			              "[QD3D:wait] MovieWait statePtr from=0x%08x to=0x%08x "
			              "n=%u wall=%llu tick=%u\n",
			              last_state_ptr, state, movie_phase_calls,
			              (unsigned long long)now, tick);
			descent_movie_diag_puts(msg);
			last_state_ptr = state;
		}

		if (!dumped_outer && descent_movie_addr_ok(spin_pc) && spin_pc >= 0x2000) {
			dumped_outer = true;
			const uint32 sbase = spin_pc > 0x1000 ? spin_pc - 0x1000 : 0x1000;
			if (uint8 *host = Mac2HostAddr(sbase)) {
				if (FILE *f = fopen("descent_micros_outer.bin", "wb")) {
					fwrite(host, 1, 0x2000, f);
					fclose(f);
				}
			}
			uint32 fr[16];
			for (int i = 0; i < 16; i++)
				fr[i] = 0;
			if (descent_movie_addr_ok(outer_a6) && outer_a6 >= 0x40) {
				for (int i = 0; i < 8; i++)
					fr[i] = ReadMacInt32(outer_a6 - 0x20 + uint32(i * 4));
				for (int i = 0; i < 8; i++)
					fr[8 + i] = ReadMacInt32(outer_a6 + 4 + uint32(i * 4));
			}
			uint32 tr_hi = 0, tr_lo = 0, tr_scale = 0, tr_base = 0;
			if (descent_movie_addr_ok(tr)) {
				tr_hi = ReadMacInt32(tr);
				tr_lo = ReadMacInt32(tr + 4);
				tr_scale = ReadMacInt32(tr + 8);
				tr_base = ReadMacInt32(tr + 12);
			}
			char msg[896];
			std::snprintf(msg, sizeof(msg),
			              "[QD3D:wait] MovieWait outerDump spinPc=0x%08x fileBase=0x%08x "
			              "bytes=0x2000 outerA6=0x%08x getTime=0x%08x tr=0x%08x "
			              "trVal=%08x%08x scale=%u base=0x%08x now=%llu "
			              "state=0x%08x rate=%08x "
			              "t0c=%08x%08x t14=%08x%08x t1c=%08x%08x t24=%08x%08x "
			              "ci40=%08x ci44=%08x t48=%08x%08x "
			              "tick=%u n=%u\n",
			              spin_pc, sbase, outer_a6, caller, tr,
			              tr_hi, tr_lo, tr_scale, tr_base,
			              (unsigned long long)now, state,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x00) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x0c) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x10) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x14) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x18) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x1c) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x20) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x24) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x28) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x40) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x44) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x48) : 0,
			              descent_movie_addr_ok(state) ? ReadMacInt32(state + 0x4c) : 0,
			              tick, movie_phase_calls);
			descent_movie_diag_puts(msg);

			/* Full outer frame snapshot for decoding locals / compare temps. */
			char frm[640];
			std::snprintf(frm, sizeof(frm),
			              "[QD3D:wait] MovieWait outerFrame a6=0x%08x "
			              "-20=%08x -1c=%08x -18=%08x -14=%08x -10=%08x -0c=%08x "
			              "-08=%08x -04=%08x +04=%08x +08=%08x +0c=%08x +10=%08x "
			              "+14=%08x +18=%08x +1c=%08x +20=%08x tick=%u\n",
			              outer_a6,
			              fr[0], fr[1], fr[2], fr[3], fr[4], fr[5], fr[6], fr[7],
			              fr[8], fr[9], fr[10], fr[11], fr[12], fr[13], fr[14], fr[15],
			              tick);
			descent_movie_diag_puts(frm);

			/*
			 * Decoded from descent_micros_outer.bin (spin at file+0x1000):
			 *   link a6,#-42; a4=state@8(a6); a3=12(a6)
			 *   if state+0x44: $18A8 sel 0x80A6/0x810B → TR@-18(a6)
			 *   else if state+0x40: _ComponentDispatch(ci,TR,0x00040001)
			 *   copy TR→local; rate==0x10000? else scale; write now→state+4/+8
			 *   if (state+0x4c & 3)==0 skip range math; else cmp now vs t1c/t24
			 *   success: $18A8 sel 0x80AA (may store through a3)
			 * spinPc = right after ComponentDispatch (addq #4,a7).
			 */
			char dec[480];
			std::snprintf(dec, sizeof(dec),
			              "[QD3D:wait] MovieWait outerDecode spinPc=0x%08x "
			              "link=-42 trLocal=-18(a6) clkWrite=state+4 "
			              "flags=state+0x4c&3 t1c=+0x1c t24=+0x24 "
			              "successSel=0x80AA tick=%u\n",
			              spin_pc, tick);
			descent_movie_diag_puts(dec);

			if (descent_movie_addr_ok(spin_pc) && spin_pc >= 0x20) {
				char ops[288];
				std::snprintf(ops, sizeof(ops),
				              "[QD3D:wait] MovieWait spinOps -10..+10: "
				              "%04x %04x %04x %04x %04x | %04x %04x %04x %04x %04x\n",
				              ReadMacInt16(spin_pc - 0x10),
				              ReadMacInt16(spin_pc - 0x0C),
				              ReadMacInt16(spin_pc - 0x08),
				              ReadMacInt16(spin_pc - 0x04),
				              ReadMacInt16(spin_pc - 0x02),
				              ReadMacInt16(spin_pc),
				              ReadMacInt16(spin_pc + 2),
				              ReadMacInt16(spin_pc + 4),
				              ReadMacInt16(spin_pc + 6),
				              ReadMacInt16(spin_pc + 8));
				descent_movie_diag_puts(ops);
			}

			/* Caller of outer clock sample: return PC @ 4(outer_a6). */
			const uint32 caller_ret_pc = descent_movie_addr_ok(outer_a6)
				? ReadMacInt32(outer_a6 + 4) : 0;
			if (descent_movie_addr_ok(caller_ret_pc) && caller_ret_pc >= 0x40) {
				const uint32 cbase = caller_ret_pc > 0x800 ? caller_ret_pc - 0x800 : 0x1000;
				if (uint8 *host = Mac2HostAddr(cbase)) {
					if (FILE *f = fopen("descent_micros_outer_caller.bin", "wb")) {
						fwrite(host, 1, 0x1000, f);
						fclose(f);
					}
				}
				char cop[320];
				std::snprintf(cop, sizeof(cop),
				              "[QD3D:wait] MovieWait outerCaller ret=0x%08x "
				              "fileBase=0x%08x entry=0x%08x "
				              "beforeRet=%04x %04x %04x %04x after=%04x %04x %04x %04x\n",
				              caller_ret_pc, cbase, spin_pc > 0x6C ? spin_pc - 0x6C : 0,
				              ReadMacInt16(caller_ret_pc - 8), ReadMacInt16(caller_ret_pc - 6),
				              ReadMacInt16(caller_ret_pc - 4), ReadMacInt16(caller_ret_pc - 2),
				              ReadMacInt16(caller_ret_pc), ReadMacInt16(caller_ret_pc + 2),
				              ReadMacInt16(caller_ret_pc + 4), ReadMacInt16(caller_ret_pc + 6));
				descent_movie_diag_puts(cop);
			}
		}

		/* Observe state fields. Non-clk changes = progress/exit signals.
		 * Extended through +0x74 — caller uses +0x14,+0x52,+0x64,+0x68,+0x70. */
		if (descent_movie_addr_ok(state) && state >= 0x1000) {
			static uint32 prev[22];
			static bool have_prev_t;
			uint32 cur[22];
			const int offs[22] = {
				0x00, 0x04, 0x08, 0x0c, 0x10, 0x14,
				0x1c, 0x20, 0x24, 0x28, 0x40, 0x44, 0x48, 0x4c,
				0x50, 0x52, 0x54, 0x64, 0x68, 0x6c, 0x70, 0x74
			};
			/* Note: 0x52 is a byte flag; we still read as 32-bit for simplicity. */
			bool any_changed = !have_prev_t;
			bool non_clk_changed = !have_prev_t;
			for (int i = 0; i < 22; i++) {
				cur[i] = ReadMacInt32(state + offs[i]);
				if (have_prev_t && cur[i] != prev[i]) {
					any_changed = true;
					if (i != 1 && i != 2)
						non_clk_changed = true;
				}
			}

			const uint64 clk64 = (uint64(cur[1]) << 32) | uint64(cur[2]);
			const uint8 flag52 = ReadMacInt8(state + 0x52);

			if (non_clk_changed) {
				state_progress_events++;
				char msg[640];
				std::snprintf(msg, sizeof(msg),
				              "[QD3D:wait] MovieWait stateProgress state=0x%08x "
				              "rate=%08x clk=%08x%08x o0c=%08x o10=%08x o14=%08x "
				              "t1c=%08x%08x t24=%08x%08x "
				              "ci40=%08x ci44=%08x t48=%08x%08x "
				              "o50=%08x flag52=%u o54=%08x o64=%08x o68=%08x "
				              "o6c=%08x o70=%08x o74=%08x "
				              "tick=%u n=%u wallNow=%llu session=%u\n",
				              state, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5],
				              cur[6], cur[7], cur[8], cur[9],
				              cur[10], cur[11], cur[12], cur[13],
				              cur[14], flag52, cur[16], cur[17], cur[18],
				              cur[19], cur[20], cur[21],
				              tick, movie_phase_calls,
				              (unsigned long long)now, gettime_session_id);
				descent_movie_diag_puts(msg);

				if (descent_movie_addr_ok(tr)) {
					char trm[256];
					std::snprintf(trm, sizeof(trm),
					              "[QD3D:wait] MovieWait trSnap tr=0x%08x "
					              "val=%08x%08x scale=%u base=0x%08x n=%u wall=%llu\n",
					              tr, ReadMacInt32(tr), ReadMacInt32(tr + 4),
					              ReadMacInt32(tr + 8), ReadMacInt32(tr + 12),
					              movie_phase_calls, (unsigned long long)now);
					descent_movie_diag_puts(trm);
				}
			} else if (any_changed) {
				/* clk-only: first 40 samples fully, then every ~5ms wall */
				static uint32 clk_logs;
				const bool emit = (clk_logs < 40) ||
				                  !have_clk_sample ||
				                  ((now - last_clk_wall) >= 5000);
				if (emit) {
					clk_logs++;
					char msg[448];
					std::snprintf(msg, sizeof(msg),
					              "[QD3D:wait] MovieWait stateTime state=0x%08x "
					              "rate=%08x clk=%08x%08x o0c=%08x o10=%08x o14=%08x "
					              "t1c=%08x%08x t24=%08x%08x t48=%08x%08x "
					              "ci40=%08x tick=%u n=%u wallNow=%llu dClk=%lld dWall=%lld\n",
					              state, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5],
					              cur[6], cur[7], cur[8], cur[9],
					              cur[12], cur[13], cur[10], tick, movie_phase_calls,
					              (unsigned long long)now,
					              have_clk_sample ? (long long)(clk64 - last_clk_sample) : 0LL,
					              have_clk_sample ? (long long)(now - last_clk_wall) : 0LL);
					descent_movie_diag_puts(msg);
				}
			}

			/* clk stall while wall advances inside GetTime spin */
			if (have_clk_sample && looks_like_gettime) {
				const int64 d_wall = int64(now) - int64(last_clk_wall);
				const int64 d_clk = int64(clk64) - int64(last_clk_sample);
				if (d_wall > 20000 && d_clk == 0) {
					char msg[256];
					std::snprintf(msg, sizeof(msg),
					              "[QD3D:wait] MovieWait clkStall dWall=%lld clk=%llu "
					              "n=%u tick=%u session=%u\n",
					              (long long)d_wall, (unsigned long long)clk64,
					              movie_phase_calls, tick, gettime_session_id);
					descent_movie_diag_puts(msg);
				}
			}
			if (any_changed || !have_clk_sample) {
				last_clk_sample = clk64;
				last_clk_wall = now;
				have_clk_sample = true;
			}

			if (any_changed) {
				for (int i = 0; i < 22; i++)
					prev[i] = cur[i];
				have_prev_t = true;
			}

			/* Policy fields + parent frame (rate!=0 path stores helper d0 at 0x14(a6)). */
			{
				static uint64 last_policy_wall;
				static uint8 last_flag52 = 0xFF;
				static uint32 last_helper_d0 = 0xFFFFFFFFu;
				if (flag52 != last_flag52 || !last_policy_wall ||
				    (now - last_policy_wall) >= 50000 || non_clk_changed) {
					last_policy_wall = now;
					last_flag52 = flag52;
					const uint32 caller_ret_pc = descent_movie_addr_ok(outer_a6)
						? ReadMacInt32(outer_a6 + 4) : 0;
					/* Clock-sample frame's saved a6 = policy function frame. */
					const uint32 policy_a6 = descent_movie_addr_ok(outer_a6)
						? ReadMacInt32(outer_a6) : 0;
					uint32 helper_d0 = 0, pol_ret = 0, pol_8 = 0, pol_c = 0;
					uint32 pol_10 = 0, pol_14 = 0, pol_18 = 0;
					if (descent_movie_addr_ok(policy_a6) && policy_a6 >= 0x40) {
						pol_ret = ReadMacInt32(policy_a6 + 4);
						pol_8 = ReadMacInt32(policy_a6 + 8);
						pol_c = ReadMacInt32(policy_a6 + 12);
						pol_10 = ReadMacInt32(policy_a6 + 16);
						pol_14 = ReadMacInt32(policy_a6 + 0x14);
						pol_18 = ReadMacInt32(policy_a6 + 0x18);
						helper_d0 = pol_14; /* rate!=0 path: move.l d0,0x14(a6) */
					}
					const uint32 flags4c = cur[13];
					/* pol10 is often a *second* state object (seen 0x10ece0 vs 0x10ec50). */
					uint32 s2_rate = 0, s2_clk_h = 0, s2_clk_l = 0, s2_4c = 0;
					uint32 s2_14 = 0, s2_52 = 0, s2_64 = 0, s2_70 = 0;
					if (descent_movie_addr_ok(pol_10) && pol_10 >= 0x1000) {
						s2_rate = ReadMacInt32(pol_10);
						s2_clk_h = ReadMacInt32(pol_10 + 4);
						s2_clk_l = ReadMacInt32(pol_10 + 8);
						s2_14 = ReadMacInt32(pol_10 + 0x14);
						s2_4c = ReadMacInt32(pol_10 + 0x4c);
						s2_52 = ReadMacInt8(pol_10 + 0x52);
						s2_64 = ReadMacInt32(pol_10 + 0x64);
						s2_70 = ReadMacInt32(pol_10 + 0x70);
					}
					/* Parent of policy fn: saved a6 chain + return. */
					uint32 grand_a6 = 0, grand_ret = 0, grand_8 = 0, grand_c = 0;
					if (descent_movie_addr_ok(policy_a6)) {
						grand_a6 = ReadMacInt32(policy_a6);
						if (descent_movie_addr_ok(grand_a6) && grand_a6 >= 0x20) {
							grand_ret = ReadMacInt32(grand_a6 + 4);
							grand_8 = ReadMacInt32(grand_a6 + 8);
							grand_c = ReadMacInt32(grand_a6 + 12);
						}
					}
					char msg[704];
					std::snprintf(msg, sizeof(msg),
					              "[QD3D:wait] MovieWait policySnap n=%u "
					              "callerRet=0x%08x policyA6=0x%08x polRet=0x%08x "
					              "pol8=0x%08x polC=0x%08x pol10=0x%08x "
					              "helperD0=0x%08x helperDec=%u pol18=0x%08x "
					              "rate=%08x flags4c=%08x flag&4=%u flag52=%u "
					              "o14=%08x o64=%08x o70=%08x "
					              "s2rate=%08x s2clk=%08x%08x s2_14=%08x s2_4c=%08x "
					              "s2_52=%u s2_64=%08x s2_70=%08x "
					              "grandA6=0x%08x grandRet=0x%08x grand8=0x%08x grandC=0x%08x "
					              "clk=%llu wall=%llu tick=%u\n",
					              movie_phase_calls, caller_ret_pc, policy_a6, pol_ret,
					              pol_8, pol_c, pol_10, helper_d0, helper_d0, pol_18,
					              cur[0], flags4c, (flags4c >> 2) & 1u, flag52,
					              cur[5], cur[17], cur[20],
					              s2_rate, s2_clk_h, s2_clk_l, s2_14, s2_4c,
					              s2_52, s2_64, s2_70,
					              grand_a6, grand_ret, grand_8, grand_c,
					              (unsigned long long)clk64, (unsigned long long)now,
					              tick);
					descent_movie_diag_puts(msg);
					if (helper_d0 != last_helper_d0) {
						char m2[256];
						std::snprintf(m2, sizeof(m2),
						              "[QD3D:wait] MovieWait helperD0chg from=0x%08x(%u) "
						              "to=0x%08x(%u) delta=%d policyA6=0x%08x n=%u tick=%u\n",
						              last_helper_d0, last_helper_d0, helper_d0, helper_d0,
						              int(helper_d0 - last_helper_d0), policy_a6,
						              movie_phase_calls, tick);
						descent_movie_diag_puts(m2);
						last_helper_d0 = helper_d0;
					}
				}
			}

			/*
			 * Simulate outer clock-sample range check. Live: flags&3 always 0
			 * → skipRange; this is NOT a blocking deadline wait. Thrash is the
			 * *caller* re-invoking this sample helper.
			 */
			{
				static uint32 cmp_logs;
				static uint64 last_cmp_wall;
				const int64 t1c = descent_movie_s64(cur[6], cur[7]);
				const int64 t24 = descent_movie_s64(cur[8], cur[9]);
				const int64 clk_s = int64(clk64);
				const int64 wall_s = int64(now);
				const int c_clk_t1c = descent_movie_cmp64(clk_s, t1c);
				const int c_clk_t24 = descent_movie_cmp64(clk_s, t24);
				const int c_wall_t1c = descent_movie_cmp64(wall_s, t1c);
				const int c_wall_t24 = descent_movie_cmp64(wall_s, t24);
				const uint32 flags4c = cur[13];
				const uint32 flag_lo = flags4c & 3u;
				const uint32 rate = cur[0];
				const uint32 ci40 = cur[10];
				const uint32 ci44 = cur[11];
				const char *path = ci44 ? "vec18A8" : (ci40 ? "compDisp" : "none");
				/* Guest: in-range if now>=t1c and now<=t24 (signed). */
				const int in_clk = (c_clk_t1c >= 0 && c_clk_t24 <= 0) ? 1 : 0;
				const int in_wall = (c_wall_t1c >= 0 && c_wall_t24 <= 0) ? 1 : 0;
				const int skip_range = (flag_lo == 0) ? 1 : 0;

				uint32 a3 = 0;
				if (descent_movie_addr_ok(outer_a6))
					a3 = ReadMacInt32(outer_a6 + 12);

				/* TR at -18(a6) may be previous/partial mid-GetTime. */
				uint32 tr_hi = 0, tr_lo = 0, tr_sc = 0, tr_bs = 0;
				const uint32 tr_local = outer_a6 - 18;
				if (descent_movie_addr_ok(tr_local)) {
					tr_hi = ReadMacInt32(tr_local);
					tr_lo = ReadMacInt32(tr_local + 4);
					tr_sc = ReadMacInt32(tr_local + 8);
					tr_bs = ReadMacInt32(tr_local + 12);
				}

				const bool emit_cmp =
					(cmp_logs < 50) ||
					!last_cmp_wall ||
					(now - last_cmp_wall) >= 20000 ||
					non_clk_changed;
				if (emit_cmp) {
					cmp_logs++;
					last_cmp_wall = now;
					char msg[704];
					std::snprintf(msg, sizeof(msg),
					              "[QD3D:wait] MovieWait compareSim n=%u session=%u "
					              "path=%s rate=%08x flags4c=%08x flag&3=%u skipRange=%d "
					              "t1c=%lld t24=%lld clk=%lld wall=%lld "
					              "cmpClkT1c=%d cmpClkT24=%d inClk=%d "
					              "cmpWallT1c=%d cmpWallT24=%d inWall=%d "
					              "ci40=%08x ci44=%08x a3=0x%08x "
					              "trLocal=%08x%08x sc=%u bs=%08x "
					              "spinPc=0x%08x tick=%u\n",
					              movie_phase_calls, gettime_session_id, path,
					              rate, flags4c, flag_lo, skip_range,
					              (long long)t1c, (long long)t24,
					              (long long)clk_s, (long long)wall_s,
					              c_clk_t1c, c_clk_t24, in_clk,
					              c_wall_t1c, c_wall_t24, in_wall,
					              ci40, ci44, a3,
					              tr_hi, tr_lo, tr_sc, tr_bs,
					              spin_pc, tick);
					descent_movie_diag_puts(msg);

					/* If range check would pass but we keep spinning, flag it. */
					if (in_clk && !skip_range) {
						char m2[256];
						std::snprintf(m2, sizeof(m2),
						              "[QD3D:wait] MovieWait inRangeStillSpinning n=%u "
						              "clk=%lld t1c=%lld t24=%lld flags&3=%u a3=0x%08x "
						              "tick=%u\n",
						              movie_phase_calls, (long long)clk_s,
						              (long long)t1c, (long long)t24, flag_lo, a3,
						              tick);
						descent_movie_diag_puts(m2);
					}
					if (skip_range) {
						char m2[192];
						std::snprintf(m2, sizeof(m2),
						              "[QD3D:wait] MovieWait skipRangePath n=%u "
						              "flags4c=%08x a3=0x%08x clk=%lld tick=%u\n",
						              movie_phase_calls, flags4c, a3,
						              (long long)clk_s, tick);
						descent_movie_diag_puts(m2);
					}
				}

				/* Heartbeat carries compare summary every ~100ms. */
				if (!last_heartbeat_wall)
					last_heartbeat_wall = now;
				if (now - last_heartbeat_wall >= 100000) {
					char msg[512];
					std::snprintf(msg, sizeof(msg),
					              "[QD3D:wait] MovieWait heartbeat session=%u n=%u "
					              "calls=%u clk=%llu wall=%llu spinPc=0x%08x "
					              "state=0x%08x progressEv=%u path=%s skipRange=%d "
					              "inClk=%d inWall=%d flag&3=%u a3=0x%08x tick=%u\n",
					              gettime_session_id, movie_phase_calls, heartbeat_calls,
					              (unsigned long long)clk64, (unsigned long long)now,
					              spin_pc, state, state_progress_events, path,
					              skip_range, in_clk, in_wall, flag_lo, a3, tick);
					descent_movie_diag_puts(msg);
					last_heartbeat_wall = now;
					heartbeat_calls = 0;
				}
			}
		}
	}

	const bool big_gap = (delta > 10000);
	const bool want = raw_logs_left > 0 || movie_phase_calls <= 12 ||
	                  first_sight_caller || big_gap ||
	                  (movie_phase_calls % 1000) == 0;
	if (want) {
		if (raw_logs_left > 0)
			raw_logs_left--;
		char msg[512];
		std::snprintf(msg, sizeof(msg),
		              "[QD3D:wait] MovieWait now=%llu last=%llu gap=%lld tick=%u n=%u "
		              "ret=0x%08x caller=0x%08x getTime=%d stk=%08x/%08x "
		              "deep=%08x/%08x/%08x/%08x/%08x/%08x%s\n",
		              (unsigned long long)now, (unsigned long long)last_t,
		              (long long)delta, tick, movie_phase_calls,
		              ret, caller, looks_like_gettime ? 1 : 0, stk4, stk8,
		              deep[0], deep[1], deep[2], deep[3], deep[4], deep[5],
		              big_gap ? " bigGap" : "");
		descent_movie_diag_puts(msg);
	}

	static bool dumped_callers;
	if (!dumped_callers && movie_phase_calls >= 3000) {
		dumped_callers = true;
		char msg[512];
		std::snprintf(msg, sizeof(msg),
		              "[QD3D:wait] MovieWait callers "
		              "0=0x%08x/%u 1=0x%08x/%u 2=0x%08x/%u 3=0x%08x/%u "
		              "4=0x%08x/%u 5=0x%08x/%u 6=0x%08x/%u 7=0x%08x/%u n=%u\n",
		              caller_pc[0], caller_n[0], caller_pc[1], caller_n[1],
		              caller_pc[2], caller_n[2], caller_pc[3], caller_n[3],
		              caller_pc[4], caller_n[4], caller_pc[5], caller_n[5],
		              caller_pc[6], caller_n[6], caller_pc[7], caller_n[7],
		              movie_phase_calls);
		descent_movie_diag_puts(msg);
	}

	(void)first_deadline;
}
#endif /* DESCENT_MOVIE_DIAGNOSTICS */

#define DEBUG 0
#include "debug.h"

extern bool tick_inhibit;

void PlayStartupSound();

// TVector of MakeExecutable
static uint32 MakeExecutableTvec;


/*
 *  Execute EMUL_OP opcode (called by 68k emulator)
 */

void EmulOp(M68kRegisters *r, uint32 *pc, int selector)
{
	D(bug("EmulOp %04x at %08x\n", selector, *pc));
	switch (selector) {
		case OP_BREAK:				// Breakpoint
			printf("*** Breakpoint\n");
			Dump68kRegs(r);
			break;

		case OP_XPRAM1: {			// Read/write from/to XPRam
			uint32 len = r->d[3];
			uint8 *adr = Mac2HostAddr(r->a[3]);
			D(bug("XPRAMReadWrite d3: %08lx, a3: %p\n", len, adr));
			int ofs = len & 0xffff;
			len >>= 16;
			if (len & 0x8000) {
				len &= 0x7fff;
				for (uint32 i=0; i<len; i++)
					XPRAM[((ofs + i) & 0xff) + 0x1300] = *adr++;
			} else {
				for (uint32 i=0; i<len; i++)
					*adr++ = XPRAM[((ofs + i) & 0xff) + 0x1300];
			}
			break;
		}

		case OP_XPRAM2:				// Read from XPRam
			r->d[1] = XPRAM[(r->d[1] & 0xff) + 0x1300];
			break;

		case OP_XPRAM3:				// Write to XPRam
			XPRAM[(r->d[1] & 0xff) + 0x1300] = r->d[2];
			break;

		case OP_NVRAM1: {			// Read from NVRAM
			int ofs = r->d[0];
			r->d[0] = XPRAM[ofs & 0x1fff];
			bool localtalk = !(XPRAM[0x13e0] || XPRAM[0x13e1]);	// LocalTalk enabled?
			switch (ofs) {
				case 0x13e0:			// Disable LocalTalk (use EtherTalk instead)
					if (localtalk)
						r->d[0] = 0x00;
					break;
				case 0x13e1:
					if (localtalk)
						r->d[0] = 0x01;
					break;
				case 0x13e2:
					if (localtalk)
						r->d[0] = 0x00;
					break;
				case 0x13e3:
					if (localtalk)
						r->d[0] = 0x0a;
					break;
			}
			break;
		}

		case OP_NVRAM2:				// Write to NVRAM
			XPRAM[r->d[0] & 0x1fff] = r->d[1];
			break;

		case OP_NVRAM3:				// Read/write from/to NVRAM
			if (r->d[3]) {
				r->d[0] = XPRAM[(r->d[4] + 0x1300) & 0x1fff];
			} else {
				XPRAM[(r->d[4] + 0x1300) & 0x1fff] = r->d[5];
				r->d[0] = 0;
			}
			break;

		case OP_FIX_MEMTOP:			// Fixes MemTop in BootGlobs during startup
			D(bug("Fix MemTop\n"));
			WriteMacInt32(BootGlobsAddr - 20, RAMBase + RAMSize);	// MemTop
			r->a[6] = RAMBase + RAMSize;
			break;

		case OP_FIX_MEMSIZE: {		// Fixes physical/logical RAM size during startup
			D(bug("Fix MemSize\n"));
			uint32 diff = ReadMacInt32(0x1ef8) - ReadMacInt32(0x1ef4);
			WriteMacInt32(0x1ef8, RAMSize);			// Physical RAM size
			WriteMacInt32(0x1ef4, RAMSize - diff);	// Logical RAM size
			break;
		}

		case OP_FIX_BOOTSTACK:		// Fixes boot stack pointer in boot 3 resource
			D(bug("Fix BootStack\n"));
			r->a[1] = r->a[7] = RAMBase + RAMSize * 3 / 4;
			break;

		case OP_SONY_OPEN:			// Floppy driver functions
			r->d[0] = SonyOpen(r->a[0], r->a[1]);
			break;
		case OP_SONY_PRIME:
			r->d[0] = SonyPrime(r->a[0], r->a[1]);
			break;
		case OP_SONY_CONTROL:
			r->d[0] = SonyControl(r->a[0], r->a[1]);
			break;
		case OP_SONY_STATUS:
			r->d[0] = SonyStatus(r->a[0], r->a[1]);
			break;

		case OP_DISK_OPEN:			// Disk driver functions
			r->d[0] = DiskOpen(r->a[0], r->a[1]);
			break;
		case OP_DISK_PRIME:
			r->d[0] = DiskPrime(r->a[0], r->a[1]);
			break;
		case OP_DISK_CONTROL:
			r->d[0] = DiskControl(r->a[0], r->a[1]);
			break;
		case OP_DISK_STATUS:
			r->d[0] = DiskStatus(r->a[0], r->a[1]);
			break;

		case OP_CDROM_OPEN:			// CD-ROM driver functions
			r->d[0] = CDROMOpen(r->a[0], r->a[1]);
			break;
		case OP_CDROM_PRIME:
			r->d[0] = CDROMPrime(r->a[0], r->a[1]);
			break;
		case OP_CDROM_CONTROL:
			r->d[0] = CDROMControl(r->a[0], r->a[1]);
			break;
		case OP_CDROM_STATUS:
			r->d[0] = CDROMStatus(r->a[0], r->a[1]);
			break;

		case OP_AUDIO_DISPATCH:		// Audio component functions
#if DESCENT_MOVIE_DIAGNOSTICS
			DescentMovieDiagNote68kStack(r->a[7]);
#endif
			r->d[0] = AudioDispatch(r->a[3], r->a[4]);
			break;

		case OP_SOUNDIN_OPEN:		// Sound input driver functions
			r->d[0] = SoundInOpen(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_PRIME:
			r->d[0] = SoundInPrime(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_CONTROL:
			r->d[0] = SoundInControl(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_STATUS:
			r->d[0] = SoundInStatus(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_CLOSE:
			r->d[0] = SoundInClose(r->a[0], r->a[1]);
			break;

		case OP_ADBOP:				// ADBOp() replacement
			ADBOp(r->d[0], Mac2HostAddr(ReadMacInt32(r->a[0])));
			break;

		case OP_INSTIME:			// InsTime() replacement
			r->d[0] = InsTime(r->a[0], r->d[1]);
			break;
		case OP_RMVTIME:			// RmvTime() replacement
			r->d[0] = RmvTime(r->a[0]);
			break;
		case OP_PRIMETIME:			// PrimeTime() replacement
			r->d[0] = PrimeTime(r->a[0], r->d[0]);
			break;

		case OP_MICROSECONDS:		// Microseconds() replacement
			Microseconds(r->a[0], r->d[0]);
#if ENABLE_NATIVE_MICROSECONDS_PATCH
			/* Everything below is the Descent II movie-timing experiment layered
			 * on the plain Microseconds() replacement above: the audio
			 * thrash-mixer service plus the QT-clock fast path and A82A/GetTime
			 * probes. Superseded by the native Cinepak decoder and gated off as
			 * a unit (the service crashed — nested Execute68k clobbered LR).
			 * The baseline case is just the Microseconds() fill + break. */
			/* Twin of the FN=1 NATIVE_MICROSECONDS op. */
			AudioServicePendingInterrupt();
#if DESCENT_MOVIE_QT_CLOCK_FASTPATH
			/* QuickTime's sound-clock GetTime reaches ROM Microseconds through a
			 * 68k glue that `jsr $408324C0`s it ~20k/s (descent-movie §32). The
			 * PPC->68k EMUL_OP crossing per call is the movie freeze. Patch that
			 * one jsr to OP_QT_CLOCK_MICROS (a minimal handler) so the call is
			 * cheap. Self-locating: this A193 arrived via the glue, so the 68k
			 * return address on top of stack is the instruction AFTER the jsr;
			 * the jsr is the preceding 6 bytes. Verify the exact opcode
			 * (jsr abs.l $408324C0 = 4EB9 4083 24C0) before writing. One-shot. */
			if (AudioStatus.num_sources >= 2) {
				static bool qt_clock_patched;
				if (!qt_clock_patched) {
					const uint32 a7 = r->a[7];
					if (a7 >= 0x1000 && a7 < 0x40000000 - 4) {
						const uint32 ret = ReadMacInt32(a7);
						const uint32 jsr = ret - 6;
						if (jsr >= 0x1000 && jsr < 0x40000000 - 6 &&
						    ReadMacInt16(jsr) == 0x4EB9 &&
						    ReadMacInt16(jsr + 2) == 0x4083 &&
						    ReadMacInt16(jsr + 4) == 0x24C0) {
							/* 6 bytes: EMUL_OP (2) + nop + nop. The op fills
							 * a0:d0 exactly as the ROM routine would. */
							WriteMacInt16(jsr, M68K_EMUL_OP_QT_CLOCK_MICROS);
							WriteMacInt16(jsr + 2, 0x4E71);	// nop
							WriteMacInt16(jsr + 4, 0x4E71);	// nop
							FlushCodeCache(jsr, jsr + 6);
							qt_clock_patched = true;
							fprintf(stderr,
							        "[QD3D:wait] qtClockFastPath patched jsr=0x%08x "
							        "ret=0x%08x tick=%u\n",
							        jsr, ret, ReadMacInt32(0x016a));
							fflush(stderr);
						}
					}
				}
			}
#endif
			/* One-shot: dump the 68k frames above the movie GetTime glue so
			 * the sync loop (parent/grandparent of the clock sample) can be
			 * decoded offline. Uses the §4.6 stack layout: a7+12 = ret into
			 * the 68k GetTime helper (next opcode 0x225F), a7+24 = outer
			 * clock-sample a6. Walks the a6 chain; 4KB around each return
			 * address goes to descent_frame_retN.bin in the cwd. */
#if DESCENT_MOVIE_DIAGNOSTICS
			if (AudioStatus.num_sources >= 2) {
				static bool frames_dumped;
				const uint32 a7 = r->a[7];
				if (!frames_dumped && a7 >= 0x1000 && a7 < 0x40000000 - 40) {
					const uint32 gt_ret = ReadMacInt32(a7 + 12);
					if (gt_ret >= 0x1000 && gt_ret < 0x40000000 - 4 &&
					    ReadMacInt16(gt_ret) == 0x225F) {
						frames_dumped = true;
						uint32 fp = ReadMacInt32(a7 + 24);
						for (int depth = 0; depth < 6 && fp >= 0x1000 &&
						     fp < 0x40000000 - 8; depth++) {
							const uint32 ret = ReadMacInt32(fp + 4);
							std::fprintf(stderr,
							             "[QD3D:wait] frameWalk depth=%d "
							             "a6=0x%08x ret=0x%08x tick=%u\n",
							             depth, fp, ret, ReadMacInt32(0x016a));
							if (ret >= 0x1000 && ret < 0x40000000) {
								char name[64];
								std::snprintf(name, sizeof(name),
								              "descent_frame_ret%d.bin", depth);
								FILE *f = std::fopen(name, "wb");
								if (f) {
									const uint32 base = ret >= 0x800 ? ret - 0x800 : 0;
									for (uint32 i = 0; i < 0x1000; i += 4) {
										const uint32 v = ReadMacInt32(base + i);
										const uint8 b[4] = {
											(uint8)(v >> 24), (uint8)(v >> 16),
											(uint8)(v >> 8), (uint8)v
										};
										std::fwrite(b, 1, 4, f);
									}
									std::fclose(f);
									std::fprintf(stderr,
									             "[QD3D:wait] frameDump depth=%d "
									             "base=0x%08x file=%s\n",
									             depth, base, name);
								}
							}
							fp = ReadMacInt32(fp);
						}
						std::fflush(stderr);
					}
				}
			}
#endif /* DESCENT_MOVIE_DIAGNOSTICS */
			/*
			 * One-shot: find sample helper that does
			 *   move.l #$00040001,-(sp) ; moveq #0,d0 ; A82A
			 * and replace only that A82A with host GetTime. Global A82A rebind
			 * is unsafe; free GetTime alone free-runs the parent and silences
			 * audio — handler services audio every call and rate-limits.
			 * Default OFF: even with service+rate-limit the mixer loses PB
			 * data (SourcePB data=0 → no audio); see session notes §22.13.
			 */
#if DESCENT_MOVIE_INLINE_A82A_GETTIME || DESCENT_MOVIE_SOUND_CLOCK_WALL
			/* Discovery retries until a site is actually patched: the useful
			 * stack slots are only populated when this A193 came through the
			 * movie GetTime helper, which is a minority of calls. Latching after
			 * one look (the old behaviour) usually burned the attempt on
			 * spin=0x14 / gt=<ROM addr> and patched nothing all run. */
			if (AudioStatus.num_sources >= 2) {
				static bool sites_patched;
				static uint32 scan_attempts;
				const uint32 a7 = r->a[7];
				if (!sites_patched && a7 >= 0x1000 && a7 < 0x40000000 - 40 &&
				    scan_attempts < 20000) {
					scan_attempts++;
					const uint32 maybe_gt = ReadMacInt32(a7 + 12);
					const uint32 outer_a6 = ReadMacInt32(a7 + 24);
					uint32 spin = ReadMacInt32(a7 + 28);
					if (outer_a6 >= 0x1000 &&
					    (spin < 0x2000 || spin >= 0x40000000))
						spin = ReadMacInt32(outer_a6 + 4);
					unsigned found = 0;
					/* Scan a window on BOTH sides of the candidate: the GetTime
					 * call site is not necessarily below the return address. */
					auto patch_a82a_range = [&found](uint32 base, const char *label) {
						if (base < 0x20 || base >= 0x40000000)
							return;
						const uint32 start = base > 0x800 ? base - 0x800 : 0x20;
						const uint32 end = base + 0x800;
						for (uint32 pc = start; pc + 8 < end; pc += 2) {
							if (ReadMacInt16(pc) == 0x2F3C &&
							    ReadMacInt32(pc + 2) == 0x00040001 &&
							    ReadMacInt16(pc + 6) == 0x7000 &&
							    ReadMacInt16(pc + 8) == 0xA82A) {
								WriteMacInt16(pc + 8,
								              M68K_EMUL_OP_COMPONENT_DISPATCH);
								found++;
								std::fprintf(stderr,
								             "[QD3D:wait] inlineA82aPatch at=0x%08x "
								             "%s=0x%08x tick=%u\n",
								             pc + 8, label, base,
								             ReadMacInt32(0x016a));
								std::fflush(stderr);
							}
						}
					};
					/* Only 68k RAM candidates are worth scanning; a ROM/PPC
					 * address (e.g. 0x40814afe) never holds the helper. */
					if (spin >= 0x2000 && spin < 0x40000000)
						patch_a82a_range(spin, "spin");
					if (maybe_gt >= 0x2000 && maybe_gt < 0x40000000)
						patch_a82a_range(maybe_gt, "gt");
					if (found) {
						sites_patched = true;
						std::fprintf(stderr,
						             "[QD3D:wait] scanSites DONE attempts=%u "
						             "sites=%u a7=0x%08x spin=0x%08x gt=0x%08x "
						             "tick=%u\n",
						             scan_attempts, found, a7, spin, maybe_gt,
						             ReadMacInt32(0x016a));
						std::fflush(stderr);
					} else if (scan_attempts == 1 ||
					           (scan_attempts % 4000) == 0) {
						std::fprintf(stderr,
						             "[QD3D:wait] scanSites miss attempt=%u "
						             "a7=0x%08x spin=0x%08x gt=0x%08x tick=%u\n",
						             scan_attempts, a7, spin, maybe_gt,
						             ReadMacInt32(0x016a));
						std::fflush(stderr);
					}
				}
			}
#endif /* DESCENT_MOVIE_INLINE_A82A_GETTIME || DESCENT_MOVIE_SOUND_CLOCK_WALL */
#if DESCENT_MOVIE_DIAGNOSTICS
			descent_movie_log_microseconds_frame(r);
#endif
#endif /* ENABLE_NATIVE_MICROSECONDS_PATCH */
			break;

		case OP_ZERO_SCRAP:			// ZeroScrap() patch
			ZeroScrap();
			break;

		case OP_PUT_SCRAP:			// PutScrap() patch
			PutScrap(ReadMacInt32(r->a[7] + 8), Mac2HostAddr(ReadMacInt32(r->a[7] + 4)), ReadMacInt32(r->a[7] + 12));
			break;

		case OP_GET_SCRAP:			// GetScrap() patch
			GetScrap((void **)Mac2HostAddr(ReadMacInt32(r->a[7] + 4)), ReadMacInt32(r->a[7] + 8), ReadMacInt32(r->a[7] + 12));
			break;

		case OP_DEBUG_STR:			// DebugStr() shows warning message
			if (PrefsFindBool("nogui")) {
				uint8 *pstr = Mac2HostAddr(ReadMacInt32(r->a[7] + 4));
				char str[256];
				int i;
				for (i=0; i<pstr[0]; i++)
					str[i] = pstr[i+1];
				str[i] = 0;
				WarningAlert(str);
			}
			break;

		case OP_INSTALL_DRIVERS: {	// Patch to install our own drivers during startup
			// Install drivers
			InstallDrivers();

			// Patch MakeExecutable()
			MakeExecutableTvec = FindLibSymbol("\023PrivateInterfaceLib", "\016MakeExecutable");
			D(bug("MakeExecutable TVECT at %08x\n", MakeExecutableTvec));
			WriteMacInt32(MakeExecutableTvec, NativeFunction(NATIVE_MAKE_EXECUTABLE));
#if !EMULATED_PPC
			WriteMacInt32(MakeExecutableTvec + 4, (uint32)TOC);
#endif

			// Patch DebugStr()
			static const uint8 proc_template[] = {
				M68K_EMUL_OP_DEBUG_STR >> 8, M68K_EMUL_OP_DEBUG_STR & 0xFF,
				0x4e, 0x74,			// rtd	#4
				0x00, 0x04
			};
			BUILD_SHEEPSHAVER_PROCEDURE(proc);
			WriteMacInt32(0x1dfc, proc);
			break;
		}

		case OP_NAME_REGISTRY:		// Patch Name Registry and initialize CallUniversalProc
			r->d[0] = (uint32)-1;
			PatchNameRegistry();
			InitCallUniversalProc();
			break;

		case OP_RESET:				// Early in MacOS reset
			D(bug("*** RESET ***\n"));
			tick_inhibit = true;
			CDROMRemount(); // for System 7.x
			TimerReset();
			MacOSUtilReset();
			EtherResetCachedAllocation();
			ether_reset();
			AudioReset();
#ifdef USE_SDL_AUDIO
			PlayStartupSound();
#endif
			// Enable DR emulator (disabled for now)
			if (PrefsFindBool("jit68k") && 0) {
				D(bug("DR activated\n"));
				WriteMacInt32(KernelDataAddr + 0x17a0, 3);		// Prepare for DR emulator activation
				WriteMacInt32(KernelDataAddr + 0x17c0, DR_CACHE_BASE);
				WriteMacInt32(KernelDataAddr + 0x17c4, DR_CACHE_SIZE);
				WriteMacInt32(KernelDataAddr + 0x1b04, DR_CACHE_BASE);
				WriteMacInt32(KernelDataAddr + 0x1b00, DR_EMULATOR_BASE);
				memcpy((void *)DR_EMULATOR_BASE, (void *)(ROMBase + 0x370000), DR_EMULATOR_SIZE);
				MakeExecutable(0, DR_EMULATOR_BASE, DR_EMULATOR_SIZE);
			}
			tick_inhibit = false;
			break;

		case OP_IRQ: {			// Level 1 interrupt
#if QD3D_WAIT_LOGGING_ENABLED
			const bool log_descent_irq = emul_op_descent_ii_is_current_application();
			const uint64 irq_started = GetTicks_usec();
			const uint32 irq_flags = InterruptFlags;
			static uint64 previous_via_usec;
			static uint32 via_count;
			uint64 via_delta_usec = 0;
			if (log_descent_irq && (irq_flags & INTFLAG_VIA)) {
				via_delta_usec = previous_via_usec ? irq_started - previous_via_usec : 0;
				previous_via_usec = irq_started;
				via_count++;
			}
#endif
			WriteMacInt16(ReadMacInt32(KernelDataAddr + 0x67c), 0);	// Clear interrupt
			r->d[0] = 0;
			if (HasMacStarted()) {
				if (InterruptFlags & INTFLAG_VIA) {
					ClearInterruptFlag(INTFLAG_VIA);
#if !PRECISE_TIMING
					TimerInterrupt();
#endif
					ExecuteNative(NATIVE_VIDEO_VBL);

					static int tick_counter = 0;
					if (++tick_counter >= 60) {
						tick_counter = 0;
						SonyInterrupt();
						DiskInterrupt();
						CDROMInterrupt();
					}

					r->d[0] = 1;		// Flag: 68k interrupt routine executes VBLTasks etc.
				}
				if (InterruptFlags & INTFLAG_SERIAL) {
					ClearInterruptFlag(INTFLAG_SERIAL);
					SerialInterrupt();
				}
				if (InterruptFlags & INTFLAG_ETHER) {
					ClearInterruptFlag(INTFLAG_ETHER);
					ExecuteNative(NATIVE_ETHER_IRQ);
				}
				if (InterruptFlags & INTFLAG_TIMER) {
					ClearInterruptFlag(INTFLAG_TIMER);
					TimerInterrupt();
				}
				if (InterruptFlags & INTFLAG_AUDIO) {
					ClearInterruptFlag(INTFLAG_AUDIO);
					AudioInterrupt();
				}
				if (InterruptFlags & INTFLAG_ADB) {
					ClearInterruptFlag(INTFLAG_ADB);
					ADBInterrupt();
				}
			} else
				r->d[0] = 1;
			#if QD3D_WAIT_LOGGING_ENABLED
			if (log_descent_irq) {
				const uint64 handler_usec = GetTicks_usec() - irq_started;
				if ((irq_flags & INTFLAG_TIMER) || via_delta_usec >= 25000 ||
				    handler_usec >= 10000 || (via_count && (via_count % 30) == 0)) {
					QD3D_WAIT_LOG("IRQ tick=%u flags=0x%08x viaDeltaUsec=%llu handlerUsec=%llu irqNest=%d",
					              ReadMacInt32(0x016a), irq_flags,
					              (unsigned long long)via_delta_usec,
					              (unsigned long long)handler_usec,
					              (int32)ReadMacInt32(XLM_IRQ_NEST));
				}
			}
			#endif
			break;
		}

		case OP_SCSI_DISPATCH: {	// SCSIDispatch() replacement
			uint32 ret = ReadMacInt32(r->a[7]);
			uint16 sel = ReadMacInt16(r->a[7] + 4);
			r->a[7] += 6;
//			D(bug("SCSIDispatch(%d)\n", sel));
			int stack;
			switch (sel) {
				case 0:		// SCSIReset
					WriteMacInt16(r->a[7], SCSIReset());
					stack = 0;
					break;
				case 1:		// SCSIGet
					WriteMacInt16(r->a[7], SCSIGet());
					stack = 0;
					break;
				case 2:		// SCSISelect
				case 11:	// SCSISelAtn
					WriteMacInt16(r->a[7] + 2, SCSISelect(ReadMacInt8(r->a[7] + 1)));
					stack = 2;
					break;
				case 3:		// SCSICmd
					WriteMacInt16(r->a[7] + 6, SCSICmd(ReadMacInt16(r->a[7]), Mac2HostAddr(ReadMacInt32(r->a[7] + 2))));
					stack = 6;
					break;
				case 4:		// SCSIComplete
					WriteMacInt16(r->a[7] + 12, SCSIComplete(ReadMacInt32(r->a[7]), ReadMacInt32(r->a[7] + 4), ReadMacInt32(r->a[7] + 8)));
					stack = 12;
					break;
				case 5:		// SCSIRead
				case 8:		// SCSIRBlind
					WriteMacInt16(r->a[7] + 4, SCSIRead(ReadMacInt32(r->a[7])));
					stack = 4;
					break;
				case 6:		// SCSIWrite
				case 9:		// SCSIWBlind
					WriteMacInt16(r->a[7] + 4, SCSIWrite(ReadMacInt32(r->a[7])));
					stack = 4;
					break;
				case 10:	// SCSIStat
					WriteMacInt16(r->a[7], SCSIStat());
					stack = 0;
					break;
				case 12:	// SCSIMsgIn
					WriteMacInt16(r->a[7] + 4, 0);
					stack = 4;
					break;
				case 13:	// SCSIMsgOut
					WriteMacInt16(r->a[7] + 2, 0);
					stack = 2;
					break;
				case 14:	// SCSIMgrBusy
					WriteMacInt16(r->a[7], SCSIMgrBusy());
					stack = 0;
					break;
				default:
					printf("FATAL: SCSIDispatch: illegal selector\n");
					stack = 0;
					//!! SysError(12)
			}
			r->a[0] = ret;
			r->a[7] += stack;
			break;
		}

		case OP_SCSI_ATOMIC:		// SCSIAtomic() replacement
			D(bug("SCSIAtomic\n"));
			r->d[0] = (uint32)-7887;
			break;

		case OP_CHECK_SYSV: {		// Check we are not using MacOS < 8.1 with a NewWorld ROM
			r->a[1] = r->d[1];
			r->a[0] = ReadMacInt32(r->d[1]);
			uint32 sysv = ReadMacInt16(r->a[0]);
			D(bug("Detected MacOS version %d.%d.%d\n", (sysv >> 8) & 0xf, (sysv >> 4) & 0xf, sysv & 0xf));
			if (ROMType == ROMTYPE_NEWWORLD && sysv < 0x0801)
				r->d[1] = 0;
			break;
		}

		case OP_NTRB_17_PATCH:
			r->a[2] = ReadMacInt32(r->a[7]);
			r->a[7] += 4;
			if (ReadMacInt16(r->a[2] + 6) == 17)
				PatchNativeResourceManager();
			break;

		case OP_NTRB_17_PATCH2:
			r->a[7] += 8;
			PatchNativeResourceManager();
			break;

		case OP_NTRB_17_PATCH3:
			r->a[2] = ReadMacInt32(r->a[7]);
			r->a[7] += 4;
		 	D(bug("%d %d\n", ReadMacInt16(r->a[2]), ReadMacInt16(r->a[2] + 6)));
			if (ReadMacInt16(r->a[2]) == 11 && ReadMacInt16(r->a[2] + 6) == 17)
				PatchNativeResourceManager();
			break;

		case OP_NTRB_17_PATCH4:
			r->d[0] = ReadMacInt16(r->a[7]);
			r->a[7] += 2;
		 	D(bug("%d %d\n", ReadMacInt16(r->a[2]), ReadMacInt16(r->a[2] + 6)));
			if (ReadMacInt16(r->a[2]) == 11 && ReadMacInt16(r->a[2] + 6) == 17)
				PatchNativeResourceManager();
			break;

		case OP_CHECKLOAD: {		// vCheckLoad() patch
			uint32 type = ReadMacInt32(r->a[7]);
			r->a[7] += 4;
			int16 id = ReadMacInt16(r->a[2]);
			if (r->a[0] == 0)
				break;
			uint32 adr = ReadMacInt32(r->a[0]);
			if (adr == 0)
				break;
			uint16 *p = (uint16 *)Mac2HostAddr(adr);
			uint32 size = ReadMacInt32(adr - 8) & 0xffffff;
			CheckLoad(type, id, p, size);
			break;
		}

		case OP_EXTFS_COMM:			// External file system routines
			WriteMacInt16(r->a[7] + 14, ExtFSComm(ReadMacInt16(r->a[7] + 12), ReadMacInt32(r->a[7] + 8), ReadMacInt32(r->a[7] + 4)));
			break;

		case OP_EXTFS_HFS:
			WriteMacInt16(r->a[7] + 20, ExtFSHFS(ReadMacInt32(r->a[7] + 16), ReadMacInt16(r->a[7] + 14), ReadMacInt32(r->a[7] + 10), ReadMacInt32(r->a[7] + 6), ReadMacInt16(r->a[7] + 4)));
			break;

		case OP_IDLE_TIME:
			// Sleep if no events pending
			if (ReadMacInt32(0x14c) == 0)
				idle_wait();
			r->a[0] = ReadMacInt32(0x2b6);
			break;

		case OP_IDLE_TIME_2:
			// Sleep if no events pending
			if (ReadMacInt32(0x14c) == 0)
				idle_wait();
			r->d[0] = (uint32)-2;
			break;

		/*
		 * Host ClockGetTime (inline, replaces A82A at sample site only).
		 * Stack: [header][TimeRecord*][CI][result] — trap never entered.
		 *
		 * Service pending audio only. Do not nest TimerInterrupt/moreRtn
		 * here (caused powerpc_cpu::execute_illegal abort). Windows
		 * Delay_usec under 1 ms is Sleep(0); full Sleep(1) every N hits
		 * free-runs less hard without hard 1 kHz silence.
		 *
		 * With DESCENT_MOVIE_SOUND_CLOCK_WALL this is CI-specific: only the
		 * ComponentInstance obtained via siSoundClock returns host wall time;
		 * every other CI falls back to the original A82A ComponentDispatch trap.
		 */
		case OP_COMPONENT_DISPATCH: {
			const uint32 sp = r->a[7];
			static uint32 hits;
			hits++;
			const uint32 header = (sp >= 0x1000 && sp < 0x40000000 - 20) ? ReadMacInt32(sp) : 0;
			const uint32 tr = (sp >= 0x1000 && sp < 0x40000000 - 20) ? ReadMacInt32(sp + 4) : 0;
			const uint32 ci = (sp >= 0x1000 && sp < 0x40000000 - 20) ? ReadMacInt32(sp + 8) : 0;
			const uint32 sclk_ci = AudioGetSoundClockCI();
			if (hits <= 4 || (hits % 1000) == 0) {
				std::fprintf(stderr,
				             "[QD3D:wait] compDispEnter n=%u pc=0x%08x sp=0x%08x "
				             "hdr=0x%08x tr=0x%08x ci=0x%08x sclk=0x%08x tick=%u\n",
				             hits, *pc, sp, header, tr, ci, sclk_ci,
				             ReadMacInt32(0x016a));
				std::fflush(stderr);
			}
			/* Anything we do not positively recognise as a GetTime on the
			 * tracked sound clock must run the real trap. Never synthesise a
			 * result (an error in d0) for a call we did not execute: the guest
			 * sequence is `move.l #$00040001,-(sp); moveq #0,d0; A82A`, so d0
			 * is ComponentDispatch's selector, not a return slot yet. */
			const bool host_handled =
				sp >= 0x1000 && sp < 0x40000000 - 20 &&
				header == 0x00040001 &&
				tr >= 0x10000 && tr < 0x40000000 - 16 &&
				sclk_ci != 0 && ci == sclk_ci;

			if (!host_handled) {
				/* Run the original A82A from a ROM scratch thunk:
				 *     A82A ; jmp abs.l <original cleanup PC>
				 * The 68k PC handed to EmulOp already points at the guest's
				 * cleanup instruction (the one after the patched A82A), so the
				 * thunk jumps straight there and the guest's own cleanup pops
				 * the parameters. A82A is entered with the caller's registers
				 * intact — in particular d0, which the guest set to 0 as the
				 * dispatch selector. Do not touch d0/a7 on this path.
				 *
				 * The jump target differs per call site, so rebuild the operand
				 * each time through the host ROM mapping (WriteMacInt32 takes a
				 * Mac address; it must not be handed ROMBase + offset). */
				const uint32 patch_base = COMPONENT_DISPATCH_PATCH_SPACE;
				const uint32 resume_pc = *pc;
				uint16 *p = (uint16 *)(ROMBaseHost + patch_base);
				p[0] = htons(0xA82A);		// A82A ComponentDispatch
				p[1] = htons(0x4EF9);		// jmp abs.l
				p[2] = htons((uint16)(resume_pc >> 16));
				p[3] = htons((uint16)resume_pc);
				FlushCodeCache(ROMBase + patch_base, ROMBase + patch_base + 8);
				*pc = patch_base;
				static uint32 fallback_hits;
				if (++fallback_hits <= 4 || (fallback_hits % 20000) == 0) {
					std::fprintf(stderr,
					             "[QD3D:wait] compDispFallback n=%u ci=0x%08x "
					             "sclk=0x%08x hdr=0x%08x tick=%u sources=%d\n",
					             fallback_hits, ci, sclk_ci, header,
					             ReadMacInt32(0x016a),
					             AudioStatus.num_sources);
					std::fflush(stderr);
				}
				break;
			}

			uint32 hi = 0, lo = 0;

			Microseconds(hi, lo);
			WriteMacInt32(tr + 0, hi);
			WriteMacInt32(tr + 4, lo);
			WriteMacInt32(tr + 8, 1000000);
			WriteMacInt32(tr + 12, 0);
			WriteMacInt32(sp + 12, 0);
			r->a[7] = sp + 12;
			r->d[0] = 0;

			AudioServicePendingInterrupt();
			if (AudioStatus.num_sources >= 2 && (hits & 31) == 0) {
				/* Delay_usec(1000) → Sleep(1) on Windows. */
				Delay_usec(1000);
				AudioServicePendingInterrupt();
			}

			if (hits <= 4 || (hits % 20000) == 0) {
				std::fprintf(stderr,
				             "[QD3D:wait] hostClockGetTime n=%u ci=0x%08x "
				             "val=%08x%08x tick=%u sources=%d\n",
				             hits, ci, hi, lo, ReadMacInt32(0x016a),
				             AudioStatus.num_sources);
				std::fflush(stderr);
			}
			break;
		}

		case OP_QT_CLOCK_MICROS: {
			/* QuickTime sound-clock GetTime. Return wall micros advanced by a
			 * fixed offset so the movie's sound clock reads as if audio already
			 * reached the video's media position — collapsing the ~0.85s A/V
			 * resync freeze when the music source starts late at time 0
			 * (descent-movie §36). Offset applied only during movie phase. */
			uint32 hi, lo;
			Microseconds(hi, lo);
			if (AudioStatus.num_sources >= 2 &&
			    DESCENT_MOVIE_SCLK_OFFSET_US != 0) {
				uint64 v = ((uint64)hi << 32 | lo) + (uint64)DESCENT_MOVIE_SCLK_OFFSET_US;
				hi = (uint32)(v >> 32); lo = (uint32)v;
			}
			r->a[0] = hi;
			r->d[0] = lo;
			break;
		}

		default:
			printf("FATAL: EMUL_OP called with bogus selector %08x\n", selector);
			QuitEmulator();
			break;
	}
}

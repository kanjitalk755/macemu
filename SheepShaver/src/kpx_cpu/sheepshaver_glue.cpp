/*
 *  sheepshaver_glue.cpp - Glue Kheperix CPU to SheepShaver CPU engine interface
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
#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "xlowmem.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if TARGET_OS_MACCATALYST
// Defined in PreferencesViewControllerObjC.mm - drains AppKit's NSEvent queue so
// UIKit input stays live while the emulator owns the main thread on Catalyst.
extern "C" void catalyst_pump_appkit_events(void);
#endif
#include "emul_op.h"
#include "rom_patches.h"
#include "macos_util.h"
#include "timer.h"
#include "audio.h"
#include "block-alloc.hpp"
#include "sigsegv.h"
#include "cpu/ppc/ppc-cpu.hpp"
#include "cpu/ppc/ppc-operations.hpp"
#include "cpu/ppc/ppc-instructions.hpp"
#include "thunks.h"
#include "gfx_log.h"
#if QD3D_WAIT_LOGGING_ENABLED
static bool cpu_descent_ii_is_current_application()
{
	return ReadMacInt32(0x0910) == 0x0a446573 &&
	       ReadMacInt32(0x0914) == 0x63656e74 &&
	       (ReadMacInt32(0x0918) & 0xffffff00) == 0x20494900;
}
#endif

// Used for NativeOp trampolines
#include "video.h"
#include "name_registry.h"
#include "serial.h"
#include "ether.h"
#include "timer.h"
#if defined(ENABLE_GFXACCEL)
#include "rave_engine.h"
#include "gl_engine.h"
#include "dsp_engine.h"
#include "cinepak_hooks.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#ifdef USE_SDL_VIDEO
#include "my_sdl.h"
#endif

#if ENABLE_MON
#include "mon.h"
#include "mon_disass.h"
#endif

#define DEBUG 0
#include "debug.h"

extern "C" {
#include "dis-asm.h"
}

// Emulation time statistics
#ifndef EMUL_TIME_STATS
#define EMUL_TIME_STATS 0
#endif

#if EMUL_TIME_STATS
static clock_t emul_start_time;
static uint32 interrupt_count = 0, ppc_interrupt_count = 0;
static clock_t interrupt_time = 0;
static uint32 exec68k_count = 0;
static clock_t exec68k_time = 0;
static uint32 native_exec_count = 0;
static clock_t native_exec_time = 0;
static uint32 macos_exec_count = 0;
static clock_t macos_exec_time = 0;
#endif

static void enter_mon(void)
{
	// Start up mon in real-mode
#if ENABLE_MON
	const char *arg[4] = {"mon", "-m", "-r", NULL};
	mon(3, arg);
#endif
}

// From main_*.cpp
extern uintptr SignalStackBase();

// From rsrc_patches.cpp
extern "C" void check_load_invoc(uint32 type, int16 id, uint32 h);
extern "C" void named_check_load_invoc(uint32 type, uint32 name, uint32 h);

// PowerPC EmulOp to exit from emulation loop
const uint32 POWERPC_EXEC_RETURN = POWERPC_EMUL_OP | 1;

// Enable Execute68k() safety checks?
#define SAFE_EXEC_68K 1

// Save FP state in Execute68k()?
#define SAVE_FP_EXEC_68K 1

// Interrupts in EMUL_OP mode?
#define INTERRUPTS_IN_EMUL_OP_MODE 1

// Interrupts in native mode?
#define INTERRUPTS_IN_NATIVE_MODE 1

// Pointer to Kernel Data
static KernelData * kernel_data;

// SIGSEGV handler
sigsegv_return_t sigsegv_handler(sigsegv_address_t, sigsegv_address_t);

#if PPC_ENABLE_JIT && PPC_REENTRANT_JIT
// Special trampolines for EmulOp and NativeOp
static uint8 *emul_op_trampoline;
static uint8 *native_op_trampoline;
#endif


/**
 *		PowerPC emulator glue with special 'sheep' opcodes
 **/

enum {
	PPC_I(SHEEP) = PPC_I(MAX),
	PPC_I(SHEEP_MAX)
};

class sheepshaver_cpu
	: public powerpc_cpu
{
	void init_decoder();
	void execute_sheep(uint32 opcode);

public:

	// Constructor
	sheepshaver_cpu();

	// CR & XER accessors
	uint32 get_cr() const		{ return cr().get(); }
	void set_cr(uint32 v)		{ cr().set(v); }
	uint32 get_xer() const		{ return xer().get(); }
	void set_xer(uint32 v)		{ xer().set(v); }

	// Execute NATIVE_OP routine
	void execute_native_op(uint32 native_op);
	static void call_execute_native_op(powerpc_cpu * cpu, uint32 native_op);

	// Execute EMUL_OP routine
	void execute_emul_op(uint32 emul_op);
	static void call_execute_emul_op(powerpc_cpu * cpu, uint32 emul_op);

	// Execute 68k routine
	void execute_68k(uint32 entry, M68kRegisters *r);

	// Execute ppc routine
	void execute_ppc(uint32 entry);

	// Execute MacOS/PPC code
	uint32 execute_macos_code(uint32 tvect, int nargs, uint32 const *args);

#if PPC_ENABLE_JIT
	// Compile one instruction
	virtual int compile1(codegen_context_t & cg_context);
#endif
	// Resource manager thunk
	void get_resource(uint32 old_get_resource);
	static void call_get_resource(powerpc_cpu * cpu, uint32 old_get_resource);

	// Handle MacOS interrupt
	void interrupt(uint32 entry);

	// Diagnostic accessors (crash-context dump, vm watch logging)
	uint32 cur_pc()			{ return pc(); }
	uint32 cur_lr()			{ return lr(); }
	uint32 cur_gpr(int i)	{ return gpr(i); }

	// Make sure the SIGSEGV handler can access CPU registers
	friend sigsegv_return_t sigsegv_handler(sigsegv_info_t *sip);
};

sheepshaver_cpu::sheepshaver_cpu()
{
	init_decoder();

#if PPC_ENABLE_JIT
	if (PrefsFindBool("jit"))
		enable_jit();
#endif
}

void sheepshaver_cpu::init_decoder()
{
	static const instr_info_t sheep_ii_table[] = {
		{ "sheep",
		  (execute_pmf)&sheepshaver_cpu::execute_sheep,
		  PPC_I(SHEEP),
		  D_form, 6, 0, CFLOW_JUMP | CFLOW_TRAP
		}
	};

	const int ii_count = sizeof(sheep_ii_table)/sizeof(sheep_ii_table[0]);
	D(bug("SheepShaver extra decode table has %d entries\n", ii_count));

	for (int i = 0; i < ii_count; i++) {
		const instr_info_t * ii = &sheep_ii_table[i];
		init_decoder_entry(ii);
	}
}

/*		NativeOp instruction format:
		+------------+-------------------------+--+-----------+------------+
		|      6     |                         |FN|    OP     |      2     |
		+------------+-------------------------+--+-----------+------------+
		 0         5 |6                      18 19 20      25 26        31
*/

typedef bit_field< 19, 19 > FN_field;
typedef bit_field< 20, 25 > NATIVE_OP_field;
typedef bit_field< 26, 31 > EMUL_OP_field;

void sheepshaver_cpu::call_execute_emul_op(powerpc_cpu * cpu, uint32 emul_op) {
	static_cast<sheepshaver_cpu *>(cpu)->execute_emul_op(emul_op);
}

// Execute EMUL_OP routine
void sheepshaver_cpu::execute_emul_op(uint32 emul_op)
{
	M68kRegisters r68;
	WriteMacInt32(XLM_68K_R25, gpr(25));
	WriteMacInt32(XLM_RUN_MODE, MODE_EMUL_OP);
	for (int i = 0; i < 8; i++)
		r68.d[i] = gpr(8 + i);
	for (int i = 0; i < 7; i++)
		r68.a[i] = gpr(16 + i);
	r68.a[7] = gpr(1);
	uint32 saved_cr = get_cr() & 0xff9fffff; // mask_operand::compute(11, 8)
	uint32 saved_xer = get_xer();
	uint32 pc = gpr(24);
	EmulOp(&r68, pc, emul_op);
	set_cr(saved_cr);
	set_xer(saved_xer);
	for (int i = 0; i < 8; i++)
		gpr(8 + i) = r68.d[i];
	for (int i = 0; i < 7; i++)
		gpr(16 + i) = r68.a[i];
	gpr(1) = r68.a[7];
	gpr(24) = pc;
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);
}

// Execute SheepShaver instruction
void sheepshaver_cpu::execute_sheep(uint32 opcode)
{
//	D(bug("Extended opcode %08x at %08x (68k pc %08x)\n", opcode, pc(), gpr(24)));
	assert((((opcode >> 26) & 0x3f) == 6) && OP_MAX <= 64 + 3);

	switch (opcode & 0x3f) {
	case 0:		// EMUL_RETURN
		QuitEmulator();
		break;

	case 1:		// EXEC_RETURN
		spcflags().set(SPCFLAG_CPU_EXEC_RETURN);
		break;

	case 2:		// EXEC_NATIVE
		execute_native_op(NATIVE_OP_field::extract(opcode));
		if (FN_field::test(opcode))
			pc() = lr();
		else
			pc() += 4;
		break;

	default:	// EMUL_OP
		execute_emul_op(EMUL_OP_field::extract(opcode) - 3);
		pc() += 4;
		break;
	}
}

// Compile one instruction
#if PPC_ENABLE_JIT
int sheepshaver_cpu::compile1(codegen_context_t & cg_context)
{
#if defined(__aarch64__)
	// Threaded-code bring-up: SHEEP goes through the generic interpreter
	// invoke like everything else. The specialized emission below leans on
	// GPR/PC micro-ops (and, with PPC_REENTRANT_JIT, the EmulOp
	// trampolines) that the arm64 backend does not implement yet.
	return COMPILE_FAILURE;
#endif
	const instr_info_t *ii = cg_context.instr_info;
	if (ii->mnemo != PPC_I(SHEEP))
		return COMPILE_FAILURE;

	int status = COMPILE_FAILURE;
	powerpc_dyngen & dg = cg_context.codegen;
	uint32 opcode = cg_context.opcode;

	switch (opcode & 0x3f) {
	case 0:		// EMUL_RETURN
		dg.gen_invoke(QuitEmulator);
		status = COMPILE_CODE_OK;
		break;

	case 1:		// EXEC_RETURN
		dg.gen_spcflags_set(SPCFLAG_CPU_EXEC_RETURN);
		// Don't check for pending interrupts, we do know we have to
		// get out of this block ASAP
		dg.gen_exec_return();
		status = COMPILE_EPILOGUE_OK;
		break;

	case 2: {	// EXEC_NATIVE
		uint32 selector = NATIVE_OP_field::extract(opcode);
		switch (selector) {
#if !PPC_REENTRANT_JIT
		// Filter out functions that may invoke Execute68k() or
		// CallMacOS(), this would break reentrancy as they could
		// invalidate the translation cache and even overwrite
		// continuation code when we are done with them.
		case NATIVE_PATCH_NAME_REGISTRY:
			dg.gen_invoke(DoPatchNameRegistry);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_VIDEO_INSTALL_ACCEL:
			dg.gen_invoke(VideoInstallAccel);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_VIDEO_VBL:
			dg.gen_invoke(VideoVBL);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_GET_RESOURCE:
		case NATIVE_GET_1_RESOURCE:
		case NATIVE_GET_IND_RESOURCE:
		case NATIVE_GET_1_IND_RESOURCE:
		case NATIVE_R_GET_RESOURCE: {
			static const uint32 get_resource_ptr[] = {
				XLM_GET_RESOURCE,
				XLM_GET_1_RESOURCE,
				XLM_GET_IND_RESOURCE,
				XLM_GET_1_IND_RESOURCE,
				XLM_R_GET_RESOURCE
			};
			uint32 old_get_resource = ReadMacInt32(get_resource_ptr[selector - NATIVE_GET_RESOURCE]);
			typedef void (*func_t)(dyngen_cpu_base, uint32);
			func_t func = &sheepshaver_cpu::call_get_resource;
			dg.gen_invoke_CPU_im(func, old_get_resource);
			status = COMPILE_CODE_OK;
			break;
		}
#endif
		case NATIVE_CHECK_LOAD_INVOC:
			dg.gen_load_T0_GPR(3);
			dg.gen_load_T1_GPR(4);
			dg.gen_se_16_32_T1();
			dg.gen_load_T2_GPR(5);
			dg.gen_invoke_T0_T1_T2((void (*)(uint32, uint32, uint32))check_load_invoc);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NAMED_CHECK_LOAD_INVOC:
			dg.gen_load_T0_GPR(3);
			dg.gen_load_T1_GPR(4);
			dg.gen_load_T2_GPR(5);
			dg.gen_invoke_T0_T1_T2((void (*)(uint32, uint32, uint32))named_check_load_invoc);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_SYNC_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_sync_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_BITBLT_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_bitblt_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_FILLRECT_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_fillrect_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_UNKNOWN_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_unknown_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_BLTMASK_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_bltmask_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_FILLMASK_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_fillmask_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_BITBLT:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_bitblt);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_INVRECT:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_invrect);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_FILLRECT:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_fillrect);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_BLTMASK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_bltmask);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_FILLMASK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_fillmask);
			status = COMPILE_CODE_OK;
			break;
		}
		// Could we fully translate this NativeOp?
		if (status == COMPILE_CODE_OK) {
			if (!FN_field::test(opcode))
				cg_context.done_compile = false;
			else {
				dg.gen_load_T0_LR_aligned();
				dg.gen_set_PC_T0();
				cg_context.done_compile = true;
			}
			break;
		}
#if PPC_REENTRANT_JIT
		// Try to execute NativeOp trampoline
		if (!FN_field::test(opcode))
			dg.gen_set_PC_im(cg_context.pc + 4);
		else {
			dg.gen_load_T0_LR_aligned();
			dg.gen_set_PC_T0();
		}
		dg.gen_mov_32_T0_im(selector);
		dg.gen_jmp(native_op_trampoline);
		cg_context.done_compile = true;
		status = COMPILE_EPILOGUE_OK;
		break;
#else
		// Invoke NativeOp handler
		if (!FN_field::test(opcode)) {
			typedef void (*func_t)(dyngen_cpu_base, uint32);
			func_t func = &sheepshaver_cpu::call_execute_native_op;
			dg.gen_invoke_CPU_im(func, selector);
			cg_context.done_compile = false;
			status = COMPILE_CODE_OK;
		}
		// Otherwise, let it generate a call to execute_sheep() which
		// will cause necessary updates to the program counter
		break;
#endif
	}

	default: {	// EMUL_OP
		uint32 emul_op = EMUL_OP_field::extract(opcode) - 3;
#if PPC_REENTRANT_JIT
		// Try to execute EmulOp trampoline
		dg.gen_set_PC_im(cg_context.pc + 4);
		dg.gen_mov_32_T0_im(emul_op);
		dg.gen_jmp(emul_op_trampoline);
		cg_context.done_compile = true;
		status = COMPILE_EPILOGUE_OK;
		break;
#else
		// Invoke EmulOp handler
		typedef void (*func_t)(dyngen_cpu_base, uint32);
		func_t func = &sheepshaver_cpu::call_execute_emul_op;
		dg.gen_invoke_CPU_im(func, emul_op);
		cg_context.done_compile = false;
		status = COMPILE_CODE_OK;
		break;
#endif
	}
	}
	return status;
}
#endif

// Handle MacOS interrupt
void sheepshaver_cpu::interrupt(uint32 entry)
{
#if EMUL_TIME_STATS
	ppc_interrupt_count++;
	const clock_t interrupt_start = clock();
#endif

	// Save program counters and branch registers
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();
	uint32 saved_sp = gpr(1);

	// Initialize stack pointer to SheepShaver alternate stack base
	gpr(1) = SignalStackBase() - 64;

	// Build trampoline to return from interrupt
	SheepVar32 trampoline = POWERPC_EXEC_RETURN;

	// Prepare registers for nanokernel interrupt routine
	WriteMacInt32(KERNEL_DATA_BASE + 0x004, gpr(1));
	WriteMacInt32(KERNEL_DATA_BASE + 0x018, gpr(6));

	gpr(6) = ReadMacInt32(KERNEL_DATA_BASE + 0x65c);
	assert(gpr(6) != 0);
	WriteMacInt32(gpr(6) + 0x13c, gpr(7));
	WriteMacInt32(gpr(6) + 0x144, gpr(8));
	WriteMacInt32(gpr(6) + 0x14c, gpr(9));
	WriteMacInt32(gpr(6) + 0x154, gpr(10));
	WriteMacInt32(gpr(6) + 0x15c, gpr(11));
	WriteMacInt32(gpr(6) + 0x164, gpr(12));
	WriteMacInt32(gpr(6) + 0x16c, gpr(13));

	gpr(1)  = KernelDataAddr;
	gpr(7)  = ReadMacInt32(KERNEL_DATA_BASE + 0x660);
	gpr(8)  = 0;
	gpr(10) = trampoline.addr();
	gpr(12) = trampoline.addr();
	gpr(13) = get_cr();

	// rlwimi. r7,r7,8,0,0
	uint32 result = op_ppc_rlwimi::apply(gpr(7), 8, 0x80000000, gpr(7));
	record_cr0(result);
	gpr(7) = result;

	gpr(11) = 0xf072; // MSR (SRR1)
	cr().set((gpr(11) & 0x0fff0000) | (get_cr() & ~0x0fff0000));

	// Enter nanokernel
	execute(entry);

	// Restore program counters and branch registers
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;
	gpr(1) = saved_sp;

#if EMUL_TIME_STATS
	interrupt_time += (clock() - interrupt_start);
#endif
}

// Execute 68k routine
void sheepshaver_cpu::execute_68k(uint32 entry, M68kRegisters *r)
{
#if EMUL_TIME_STATS
	exec68k_count++;
	const clock_t exec68k_start = clock();
#endif

#if SAFE_EXEC_68K
	if (ReadMacInt32(XLM_RUN_MODE) != MODE_EMUL_OP)
		printf("FATAL: Execute68k() not called from EMUL_OP mode\n");
#endif

	// Save program counters and branch registers
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();
	uint32 saved_cr = get_cr();

	// Create MacOS stack frame
	// FIXME: make sure MacOS doesn't expect PPC registers to live on top
	uint32 sp = gpr(1);
	gpr(1) -= 56;
	WriteMacInt32(gpr(1), sp);

	// Save PowerPC registers
	uint32 saved_GPRs[19];
	memcpy(&saved_GPRs[0], &gpr(13), sizeof(uint32)*(32-13));
#if SAVE_FP_EXEC_68K
	double saved_FPRs[18];
	memcpy(&saved_FPRs[0], &fpr(14), sizeof(double)*(32-14));
#endif

	// Setup registers for 68k emulator
	cr().set(CR_SO_field<2>::mask());			// Supervisor mode
	for (int i = 0; i < 8; i++)					// d[0]..d[7]
	  gpr(8 + i) = r->d[i];
	for (int i = 0; i < 7; i++)					// a[0]..a[6]
	  gpr(16 + i) = r->a[i];
	gpr(23) = 0;
	gpr(24) = entry;
	gpr(25) = ReadMacInt32(XLM_68K_R25);		// MSB of SR
	gpr(26) = 0;
	gpr(28) = 0;								// VBR
	gpr(29) = ReadMacInt32(KERNEL_DATA_BASE + 0x1074);		// Pointer to opcode table
	gpr(30) = ReadMacInt32(KERNEL_DATA_BASE + 0x1078);		// Address of emulator
	gpr(31) = KernelDataAddr + 0x1000;

	// Push return address (points to EXEC_RETURN opcode) on stack
	gpr(1) -= 4;
	WriteMacInt32(gpr(1), XLM_EXEC_RETURN_OPCODE);

	// Rentering 68k emulator
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);

	// Set r0 to 0 for 68k emulator
	gpr(0) = 0;

	// Execute 68k opcode
	uint32 opcode = ReadMacInt16(gpr(24));
	gpr(27) = (int32)(int16)ReadMacInt16(gpr(24) += 2);
	gpr(29) += opcode * 8;
	execute(gpr(29));

	// Save r25 (contains current 68k interrupt level)
	WriteMacInt32(XLM_68K_R25, gpr(25));

	// Reentering EMUL_OP mode
	WriteMacInt32(XLM_RUN_MODE, MODE_EMUL_OP);

	// Save 68k registers
	for (int i = 0; i < 8; i++)					// d[0]..d[7]
	  r->d[i] = gpr(8 + i);
	for (int i = 0; i < 7; i++)					// a[0]..a[6]
	  r->a[i] = gpr(16 + i);

	// Restore PowerPC registers
	memcpy(&gpr(13), &saved_GPRs[0], sizeof(uint32)*(32-13));
#if SAVE_FP_EXEC_68K
	memcpy(&fpr(14), &saved_FPRs[0], sizeof(double)*(32-14));
#endif

	// Cleanup stack
	gpr(1) += 56;

	// Restore program counters and branch registers
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;
	set_cr(saved_cr);

#if EMUL_TIME_STATS
	exec68k_time += (clock() - exec68k_start);
#endif
}

// Call MacOS PPC code
uint32 sheepshaver_cpu::execute_macos_code(uint32 tvect, int nargs, uint32 const *args)
{
#if EMUL_TIME_STATS
	macos_exec_count++;
	const clock_t macos_exec_start = clock();
#endif

	// Save program counters and branch registers
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();

	// Build trampoline with EXEC_RETURN
	SheepVar32 trampoline = POWERPC_EXEC_RETURN;
	lr() = trampoline.addr();

	gpr(1) -= 64;								// Create stack frame
	uint32 proc = ReadMacInt32(tvect);			// Get routine address
	uint32 toc = ReadMacInt32(tvect + 4);		// Get TOC pointer

	// Save PowerPC registers
	uint32 regs[8];
	regs[0] = gpr(2);
	for (int i = 0; i < nargs; i++)
		regs[i + 1] = gpr(i + 3);

	// Prepare and call MacOS routine
	gpr(2) = toc;
	for (int i = 0; i < nargs; i++)
		gpr(i + 3) = args[i];
	execute(proc);
	uint32 retval = gpr(3);

	// Restore PowerPC registers
	for (int i = 0; i <= nargs; i++)
		gpr(i + 2) = regs[i];

	// Cleanup stack
	gpr(1) += 64;

	// Restore program counters and branch registers
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;

#if EMUL_TIME_STATS
	macos_exec_time += (clock() - macos_exec_start);
#endif

	return retval;
}

// Execute ppc routine
inline void sheepshaver_cpu::execute_ppc(uint32 entry)
{
	// Save branch registers
	uint32 saved_lr = lr();

	SheepVar32 trampoline = POWERPC_EXEC_RETURN;
	WriteMacInt32(trampoline.addr(), POWERPC_EXEC_RETURN);
	lr() = trampoline.addr();

	execute(entry);

	// Restore branch registers
	lr() = saved_lr;
}

void sheepshaver_cpu::call_get_resource(powerpc_cpu * cpu, uint32 old_get_resource) {
	static_cast<sheepshaver_cpu *>(cpu)->get_resource(old_get_resource);
}

// Resource Manager thunk
inline void sheepshaver_cpu::get_resource(uint32 old_get_resource)
{
	uint32 type = gpr(3);
	int16 id = gpr(4);

	// Create stack frame
	gpr(1) -= 56;

	// Call old routine
	execute_ppc(old_get_resource);

	// Call CheckLoad()
	uint32 handle = gpr(3);
	check_load_invoc(type, id, handle);
	gpr(3) = handle;

	// Cleanup stack
	gpr(1) += 56;
}


/**
 *		SheepShaver CPU engine interface
 **/

// PowerPC CPU emulator
static sheepshaver_cpu *ppc_cpu = NULL;

#if QD3D_WAIT_LOGGING_ENABLED && defined(_WIN32)
/* Host-timer PPC profiler.
 *
 * The IRQ-driven guest sampler cannot see inside a masked-IRQ freeze (the movie
 * one-time OPEN setup runs multiple seconds of PPC with interrupts off, so the
 * guest tick counter and the VIA sampler both stall). This host thread reads the
 * live PPC pc/lr on a wall-clock cadence, independent of the guest, so whatever
 * QuickTime routine burns those seconds is captured by address.
 *
 * The reads are intentionally lock-free and racy: we only need approximate hot
 * PCs, and pc()/lr() are single 32-bit fields. Gated + Windows-only (diagnostic).
 */
#include <windows.h>
static volatile bool s_ppc_prof_run = false;
static HANDLE s_ppc_prof_thread = NULL;

static DWORD WINAPI ppc_profiler_main(LPVOID)
{
	uint32 last_tick = 0;
	uint64 last_tick_wall = GetTicks_usec();
	uint32 stall_samples = 0;
	while (s_ppc_prof_run) {
		Sleep(2);
		sheepshaver_cpu *cpu = ppc_cpu;
		if (!cpu)
			continue;
		const uint32 tick = ReadMacInt32(0x016a);
		const uint64 now = GetTicks_usec();
		if (tick != last_tick) {
			last_tick = tick;
			last_tick_wall = now;
			stall_samples = 0;
			continue;
		}
		/* Guest tick has not advanced in real time: a wall-clock stall. Sample
		 * every loop (~2 ms) once the stall passes ~24 ms so even a short
		 * movie-phase hitch (a few dropped frames) yields a PC trail. Cap the
		 * per-stall sample count so a genuine long freeze cannot flood. */
		if (now - last_tick_wall >= 24000ull && stall_samples < 64) {
			QD3D_WAIT_LOG("PPCprof STALL pc=0x%08x lr=0x%08x r2=0x%08x stallUs=%llu tick=%u",
			              cpu->cur_pc(), cpu->cur_lr(), cpu->cur_gpr(2),
			              (unsigned long long)(now - last_tick_wall), tick);
		}
		stall_samples++;
	}
	return 0;
}

static void ppc_profiler_start(void)
{
	if (s_ppc_prof_thread)
		return;
	s_ppc_prof_run = true;
	s_ppc_prof_thread = CreateThread(NULL, 0, ppc_profiler_main, NULL, 0, NULL);
}
#endif /* QD3D_WAIT_LOGGING_ENABLED && _WIN32 */

void FlushCodeCache(uintptr start, uintptr end)
{
	D(bug("FlushCodeCache(%08x, %08x)\n", start, end));
	ppc_cpu->invalidate_cache_range(start, end);
}

// Dump PPC registers
static void dump_registers(void)
{
	ppc_cpu->dump_registers();
}

// Dump log
static void dump_log(void)
{
	ppc_cpu->dump_log();
}

static int read_mem(bfd_vma memaddr, bfd_byte *myaddr, int length, struct disassemble_info *info)
{
	Mac2Host_memcpy(myaddr, memaddr, length);
	return 0;
}

static void dump_disassembly(const uint32 pc, const int prefix_count, const int suffix_count)
{
	struct disassemble_info info;
	INIT_DISASSEMBLE_INFO(info, stderr, fprintf);
	info.read_memory_func = read_mem;

	const int count = prefix_count + suffix_count + 1;
	const uint32 base_addr = pc - prefix_count * 4;
	for (int i = 0; i < count; i++) {
		const bfd_vma addr = base_addr + i * 4;
		fprintf(stderr, "%s0x%8llx:  ", addr == pc ? " >" : "  ", addr);
		print_insn_ppc(addr, &info);
		fprintf(stderr, "\n");
	}
}

// Crash-context diagnostics (StarCraft post-splash SIGSEGV investigation):
// validated guest reads only -- a wild pc/sp must not re-fault inside the
// signal handler (dump_disassembly previously died reading unmapped memory).
static bool guest_addr_ok(uint32 a, uint32 len)
{
	if (a >= RAMBase && a - RAMBase < RAMSize && RAMSize - (a - RAMBase) >= len)
		return true;
	if (a >= ROMBase && a - ROMBase < ROM_AREA_SIZE && ROM_AREA_SIZE - (a - ROMBase) >= len)
		return true;
	const uint32 kbase = (uint32)(KERNEL_DATA_BASE & ~0x3fffu);
	const uint32 kend = (uint32)(KERNEL_DATA_BASE + KERNEL_AREA_SIZE);
	if (a >= kbase && a < kend && kend - a >= len)
		return true;
	// Native-op TVECTs / thunks live in SheepMem (guest 0x51xxxxxx on Windows).
	if (SheepMem::Contains(a) && SheepMem::Size() - (a - SheepMem::Base()) >= len)
		return true;
	return false;
}

static void dump_crash_context(sheepshaver_cpu *cpu)
{
	// Guest stack crawl. PowerOpen ABI: back chain at [sp], saved LR at [sp+8].
	uint32 sp = cpu->cur_gpr(1);
	fprintf(stderr, "guest stack crawl (r1=%08x):\n", sp);
	for (int depth = 0; depth < 32; depth++) {
		if (!guest_addr_ok(sp, 12)) {
			fprintf(stderr, "  [%2d] sp %08x (unmapped; stop)\n", depth, sp);
			break;
		}
		uint32 back = ReadMacInt32(sp);
		uint32 saved_lr = ReadMacInt32(sp + 8);
		fprintf(stderr, "  [%2d] sp %08x back %08x lr %08x\n", depth, sp, back, saved_lr);
		if (back <= sp || back - sp > 0x100000)
			break;
		sp = back;
	}

	// Window around the kernel-data interrupt stack. The StarCraft dump showed
	// this region holding little-endian copies of its own guest addresses;
	// flag any word still matching that signature.
	const uint32 lo = (uint32)KERNEL_DATA_BASE - 0x200;
	const uint32 hi = (uint32)KERNEL_DATA_BASE + 0x40;
	fprintf(stderr, "kernel-area dump [%08x..%08x) ('<' = LE self-address):\n", lo, hi);
	for (uint32 a = lo; a < hi; a += 16) {
		if (!guest_addr_ok(a, 16))
			continue;
		fprintf(stderr, "  %08x:", a);
		for (int i = 0; i < 4; i++) {
			uint32 w = ReadMacInt32(a + i * 4);
			uint32 le = ((w & 0xff) << 24) | ((w & 0xff00) << 8) | ((w >> 8) & 0xff00) | (w >> 24);
			fprintf(stderr, " %08x%c", w, (le - (a + i * 4)) <= 0x100 ? '<' : ' ');
		}
		fprintf(stderr, "\n");
	}

	extern void RsrcLocksDumpOnCrash(void);
	RsrcLocksDumpOnCrash();
}

sigsegv_return_t sigsegv_handler(sigsegv_info_t *sip)
{
#if ENABLE_VOSF
	// Handle screen fault
	extern bool Screen_fault_handler(sigsegv_info_t *sip);
	if (Screen_fault_handler(sip))
		return SIGSEGV_RETURN_SUCCESS;
#endif

	const uintptr addr = (uintptr)sigsegv_get_fault_address(sip);
#if HAVE_SIGSEGV_SKIP_INSTRUCTION
	// Ignore writes to ROM
	if ((addr - (uintptr)ROMBaseHost) < ROM_SIZE)
		return SIGSEGV_RETURN_SKIP_INSTRUCTION;

	if (!ppc_cpu) {
		// Haven't even gotten started yet?
		return SIGSEGV_RETURN_FAILURE;
	}

	// Get program counter of target CPU
	sheepshaver_cpu * const cpu = ppc_cpu;
	const uint32 pc = cpu->pc();

	// Fault while guest PC is in Mac ROM/RAM/DR cache, OR in SheepMem.
	// SheepMem holds native-op TVECTs (EMUL_OP trampolines). When a native
	// handler (e.g. VideoDoDriverIO) faults via WriteMacInt32, guest PC is
	// still the TVECT opcode address - without SheepMem here, ignoresegv
	// never applies and boot dies on the first unmapped guest store.
	bool mac_fault = (pc >= ROMBase && pc < (ROMBase + ROM_AREA_SIZE))
		|| (pc >= RAMBase && pc < (RAMBase + RAMSize))
		|| (pc >= DR_CACHE_BASE && pc < (DR_CACHE_BASE + DR_CACHE_SIZE))
		|| SheepMem::Contains(pc);
	if (mac_fault) {

		// "VM settings" during MacOS 8 installation
		if (pc == ROMBase + 0x488160 && cpu->gpr(20) == 0xf8000000)
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// MacOS 8.5 installation
		else if (pc == ROMBase + 0x488140 && cpu->gpr(16) == 0xf8000000)
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// MacOS 8 serial drivers on startup
		else if (pc == ROMBase + 0x48e080 && (cpu->gpr(8) == 0xf3012002 || cpu->gpr(8) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// MacOS 8.1 serial drivers on startup
		else if (pc == ROMBase + 0x48c5e0 && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
		else if (pc == ROMBase + 0x4a10a0 && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// MacOS 8.6 serial drivers on startup (with DR Cache and OldWorld ROM)
		else if ((pc - DR_CACHE_BASE) < DR_CACHE_SIZE && (cpu->gpr(16) == 0xf3012002 || cpu->gpr(16) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
		else if ((pc - DR_CACHE_BASE) < DR_CACHE_SIZE && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// Ignore writes to the zero page (compare host addresses under NATMEM)
		else if ((uint32)(addr - (uintptr)Mac2HostAddr(SheepMem::ZeroPage())) < (uint32)SheepMem::PageSize())
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// Ignore all other faults, if requested
		if (PrefsFindBool("ignoresegv")) {
			/* Rate-limited diagnostic: a storm of skips (e.g. GrayPage
			 * into an unmapped framebuffer) freezes the emul thread in
			 * the exception handler and looks like a hard lockup. */
			static uint64_t s_skip_count = 0;
			static uint32_t s_last_log_pc = 0;
			s_skip_count++;
			if (s_skip_count <= 8 ||
			    (s_skip_count == 1000) ||
			    (s_skip_count == 100000) ||
			    (pc != s_last_log_pc && (s_skip_count % 100000) == 0)) {
				fprintf(stderr,
				        "[ignoresegv] skip #%llu host_ea=%p guest_pc=%08x\n",
				        (unsigned long long)s_skip_count,
				        (void *)addr, pc);
				fflush(stderr);
				s_last_log_pc = pc;
			}
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
		}
	}
#else
#error "FIXME: You don't have the capability to skip instruction within signal handlers"
#endif

	fprintf(stderr, "SIGSEGV\n");
	fprintf(stderr, "  pc %p\n", sigsegv_get_fault_instruction_address(sip));
	fprintf(stderr, "  ea %p\n", sigsegv_get_fault_address(sip));
	dump_registers();
	dump_log();
	dump_crash_context(cpu);
	if (guest_addr_ok(pc - 8 * 4, (8 + 8 + 1) * 4))
		dump_disassembly(pc, 8, 8);
	else
		fprintf(stderr, "  (pc %08x outside mapped guest areas; disassembly skipped)\n", pc);

	enter_mon();
	QuitEmulator();

	return SIGSEGV_RETURN_FAILURE;
}

/*
 *  Initialize CPU emulation
 */

void init_emul_ppc(void)
{
	// Get pointer to KernelData in host address space
	kernel_data = (KernelData *)Mac2HostAddr(KERNEL_DATA_BASE);

	// Initialize main CPU emulator
	ppc_cpu = new sheepshaver_cpu();
	ppc_cpu->set_register(powerpc_registers::GPR(3), any_register((uint32)ROMBase + 0x30d000));
	ppc_cpu->set_register(powerpc_registers::GPR(4), any_register(KernelDataAddr + 0x1000));
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);

#if ENABLE_MON
	// Install "regs" command in cxmon
	mon_add_command("regs", dump_registers, "regs                     Dump PowerPC registers\n");
	mon_add_command("log", dump_log, "log                      Dump PowerPC emulation log\n");
#endif

#if EMUL_TIME_STATS
	emul_start_time = clock();
#endif

#if QD3D_WAIT_LOGGING_ENABLED && defined(_WIN32) && defined(ENABLE_PPC_PROFILER)
	ppc_profiler_start();
#endif
}

/*
 *  Deinitialize emulation
 */

void exit_emul_ppc(void)
{
#if EMUL_TIME_STATS
	clock_t emul_end_time = clock();

	printf("### Statistics for SheepShaver emulation parts\n");
	const clock_t emul_time = emul_end_time - emul_start_time;
	printf("Total emulation time : %.1f sec\n", double(emul_time) / double(CLOCKS_PER_SEC));
	printf("Total interrupt count: %d (%2.1f Hz)\n", interrupt_count,
		   (double(interrupt_count) * CLOCKS_PER_SEC) / double(emul_time));
	printf("Total ppc interrupt count: %d (%2.1f %%)\n", ppc_interrupt_count,
		   (double(ppc_interrupt_count) * 100.0) / double(interrupt_count));

#define PRINT_STATS(LABEL, VAR_PREFIX) do {								\
		printf("Total " LABEL " count : %d\n", VAR_PREFIX##_count);		\
		printf("Total " LABEL " time  : %.1f sec (%.1f%%)\n",			\
			   double(VAR_PREFIX##_time) / double(CLOCKS_PER_SEC),		\
			   100.0 * double(VAR_PREFIX##_time) / double(emul_time));	\
	} while (0)

	PRINT_STATS("Execute68k[Trap] execution", exec68k);
	PRINT_STATS("NativeOp execution", native_exec);
	PRINT_STATS("MacOS routine execution", macos_exec);

#undef PRINT_STATS
	printf("\n");
#endif

	delete ppc_cpu;
	ppc_cpu = NULL;
}

#if PPC_ENABLE_JIT && PPC_REENTRANT_JIT
// Initialize EmulOp trampolines
void init_emul_op_trampolines(basic_dyngen & dg)
{
	typedef void (*func_t)(dyngen_cpu_base, uint32);
	func_t func;

	// EmulOp
	emul_op_trampoline = dg.gen_start();
	func = &sheepshaver_cpu::call_execute_emul_op;
	dg.gen_invoke_CPU_T0(func);
	dg.gen_exec_return();
	dg.gen_end();

	// NativeOp
	native_op_trampoline = dg.gen_start();
	func = &sheepshaver_cpu::call_execute_native_op;
	dg.gen_invoke_CPU_T0(func);
	dg.gen_exec_return();
	dg.gen_end();

	D(bug("EmulOp trampoline:   %p\n", emul_op_trampoline));
	D(bug("NativeOp trampoline: %p\n", native_op_trampoline));
}
#endif

/*
 *  Emulation loop
 */

void emul_ppc(uint32 entry)
{
#if 0
	ppc_cpu->start_log();
#endif
	// start emulation loop and enable code translation or caching
	ppc_cpu->execute(entry);
}

/*
 *  Handle PowerPC interrupt
 */

void TriggerInterrupt(void)
{
	idle_resume();
#if 0
  WriteMacInt32(0x16a, ReadMacInt32(0x16a) + 1);
#else
  // Trigger interrupt to main cpu only
  if (ppc_cpu)
	  ppc_cpu->trigger_interrupt();
#endif
}

void HandleInterrupt(powerpc_registers *r)
{
#ifdef USE_SDL_VIDEO
	// We must fill in the events queue in the same thread that did call SDL_SetVideoMode()
	SDL_PumpEvents();
#endif
#if TARGET_OS_MACCATALYST
	// Keep UIKit responsive while the emulator owns the main thread on Catalyst.
	// Runs on the emul/main thread (~60Hz for interpreter and JIT), reentrancy-
	// guarded by processing_interrupt in check_spcflags().
	catalyst_pump_appkit_events();
#endif

#if QD3D_WAIT_LOGGING_ENABLED
	static uint32 diagnostic_interrupt_count;
	static uint32 previous_sample_pc;
	static uint32 repeated_sample_count;
	if (cpu_descent_ii_is_current_application()) {
		diagnostic_interrupt_count++;
		if (r->pc == previous_sample_pc)
			repeated_sample_count++;
		else
			repeated_sample_count = 1;
		// While the ROM 68k emulator runs (MODE_68K), the 68k PC lives in
		// PPC r24 and 68k D0-D7/A0-A6 in r8-r23 (the EMUL_OP register
		// convention). Sample the 68k PC so 68k busy-wait loops (Descent's
		// MVE movie player is 68k code) can be located and dumped.
		const bool in_68k = ReadMacInt32(XLM_RUN_MODE) == MODE_68K;
		const uint32 pc68k = in_68k ? r->gpr[24] : 0;
		if ((diagnostic_interrupt_count % 6) == 0 || repeated_sample_count == 4) {
			QD3D_WAIT_LOG("Guest CPU sample=%u tick=%u runMode=%u pc=0x%08x lr=0x%08x ctr=0x%08x sp=0x%08x toc=0x%08x pc68k=0x%08x repeatedPC=%u flags=0x%08x irqNest=%d",
			              diagnostic_interrupt_count, ReadMacInt32(0x016a),
			              ReadMacInt32(XLM_RUN_MODE), r->pc, r->lr, r->ctr,
			              r->gpr[1], r->gpr[2], pc68k, repeated_sample_count,
			              InterruptFlags, (int32)ReadMacInt32(XLM_IRQ_NEST));
		}
		// One-shot dump of the code around a repeatedly sampled application PC
		// (and its LR call site) so the busy-wait condition can be disassembled
		// offline. Application code only; guest addresses are logged above.
		previous_sample_pc = r->pc;
	}
#endif

	// Do nothing if interrupts are disabled
	if (int32(ReadMacInt32(XLM_IRQ_NEST)) > 0)
		return;

	// Update interrupt count
#if EMUL_TIME_STATS
	interrupt_count++;
#endif

	// Interrupt action depends on current run mode
	switch (ReadMacInt32(XLM_RUN_MODE)) {
	case MODE_68K:
		// 68k emulator active, trigger 68k interrupt level 1
		WriteMacInt16(ReadMacInt32(KERNEL_DATA_BASE + 0x67c), 1);
		r->cr.set(r->cr.get() | ReadMacInt32(KERNEL_DATA_BASE + 0x674));
		break;

#if INTERRUPTS_IN_NATIVE_MODE
	case MODE_NATIVE:
		// 68k emulator inactive, in nanokernel?
		if (r->gpr[1] != KernelDataAddr) {

			// Prepare for 68k interrupt level 1
			WriteMacInt16(ReadMacInt32(KERNEL_DATA_BASE + 0x67c), 1);
			WriteMacInt32(ReadMacInt32(KERNEL_DATA_BASE + 0x658) + 0xdc,
						  ReadMacInt32(ReadMacInt32(KERNEL_DATA_BASE + 0x658) + 0xdc)
						  | ReadMacInt32(KERNEL_DATA_BASE + 0x674));

			// Execute nanokernel interrupt routine (this will activate the 68k emulator)
			DisableInterrupt();
			if (ROMType == ROMTYPE_NEWWORLD)
				ppc_cpu->interrupt(ROMBase + 0x312b1c);
			else
				ppc_cpu->interrupt(ROMBase + 0x312a3c);
		}
		break;
#endif

#if INTERRUPTS_IN_EMUL_OP_MODE
	case MODE_EMUL_OP:
		// 68k emulator active, within EMUL_OP routine, execute 68k interrupt routine directly when interrupt level is 0
		if ((ReadMacInt32(XLM_68K_R25) & 7) == 0) {
#if EMUL_TIME_STATS
			const clock_t interrupt_start = clock();
#endif
#if 1
			// Execute full 68k interrupt routine
			M68kRegisters r;
			uint32 old_r25 = ReadMacInt32(XLM_68K_R25);	// Save interrupt level
			WriteMacInt32(XLM_68K_R25, 0x21);			// Execute with interrupt level 1
			static const uint8 proc_template[] = {
				0x3f, 0x3c, 0x00, 0x00,			// move.w	#$0000,-(sp)	(fake format word)
				0x48, 0x7a, 0x00, 0x0a,			// pea		@1(pc)			(return address)
				0x40, 0xe7,						// move		sr,-(sp)		(saved SR)
				0x20, 0x78, 0x00, 0x064,		// move.l	$64,a0
				0x4e, 0xd0,						// jmp		(a0)
				M68K_RTS >> 8, M68K_RTS & 0xff	// @1
			};
			BUILD_SHEEPSHAVER_PROCEDURE(proc);
			Execute68k(proc, &r);
			WriteMacInt32(XLM_68K_R25, old_r25);		// Restore interrupt level
#else
			// Only update cursor
			if (HasMacStarted()) {
				if (InterruptFlags & INTFLAG_VIA) {
					ClearInterruptFlag(INTFLAG_VIA);
					ADBInterrupt();
					ExecuteNative(NATIVE_VIDEO_VBL);
				}
			}
#endif
#if EMUL_TIME_STATS
			interrupt_time += (clock() - interrupt_start);
#endif
		}
		break;
#endif
	}
}

void sheepshaver_cpu::call_execute_native_op(powerpc_cpu * cpu, uint32 selector) {
	static_cast<sheepshaver_cpu *>(cpu)->execute_native_op(selector);
}

// Execute NATIVE_OP routine
void sheepshaver_cpu::execute_native_op(uint32 selector)
{
#if EMUL_TIME_STATS
	native_exec_count++;
	const clock_t native_exec_start = clock();
#endif

	switch (selector) {
	case NATIVE_PATCH_NAME_REGISTRY:
		DoPatchNameRegistry();
		break;
	case NATIVE_VIDEO_INSTALL_ACCEL:
		VideoInstallAccel();
		break;
	case NATIVE_VIDEO_VBL:
		VideoVBL();
		break;
	case NATIVE_VIDEO_DO_DRIVER_IO:
		gpr(3) = (int32)(int16)VideoDoDriverIO(gpr(3), gpr(4), gpr(5), gpr(6), gpr(7));
		break;
	case NATIVE_ETHER_AO_GET_HWADDR:
		AO_get_ethernet_address(gpr(3));
		break;
	case NATIVE_ETHER_AO_ADD_MULTI:
		AO_enable_multicast(gpr(3));
		break;
	case NATIVE_ETHER_AO_DEL_MULTI:
		AO_disable_multicast(gpr(3));
		break;
	case NATIVE_ETHER_AO_SEND_PACKET:
		AO_transmit_packet(gpr(3));
		break;
	case NATIVE_ETHER_IRQ:
		EtherIRQ();
		break;
	case NATIVE_ETHER_INIT:
		gpr(3) = InitStreamModule((void *)gpr(3));
		break;
	case NATIVE_ETHER_TERM:
		TerminateStreamModule();
		break;
	case NATIVE_ETHER_OPEN:
		gpr(3) = ether_open((queue_t *)gpr(3), (void *)gpr(4), gpr(5), gpr(6), (void*)gpr(7));
		break;
	case NATIVE_ETHER_CLOSE:
		gpr(3) = ether_close((queue_t *)gpr(3), gpr(4), (void *)gpr(5));
		break;
	case NATIVE_ETHER_WPUT:
		gpr(3) = ether_wput((queue_t *)gpr(3), (mblk_t *)gpr(4));
		break;
	case NATIVE_ETHER_RSRV:
		gpr(3) = ether_rsrv((queue_t *)gpr(3));
		break;
	case NATIVE_NQD_SYNC_HOOK:
		gpr(3) = NQD_sync_hook(gpr(3));
		break;
	case NATIVE_NQD_UNKNOWN_HOOK:
		gpr(3) = NQD_unknown_hook(gpr(3));
		break;
	case NATIVE_NQD_BITBLT_HOOK:
		gpr(3) = NQD_bitblt_hook(gpr(3));
		break;
	case NATIVE_NQD_BITBLT:
		NQD_bitblt(gpr(3));
		break;
	case NATIVE_NQD_FILLRECT_HOOK:
		gpr(3) = NQD_fillrect_hook(gpr(3));
		break;
	case NATIVE_NQD_INVRECT:
		NQD_invrect(gpr(3));
		break;
	case NATIVE_NQD_FILLRECT:
		NQD_fillrect(gpr(3));
		break;
	case NATIVE_NQD_BLTMASK_HOOK:
		gpr(3) = NQD_bltmask_hook(gpr(3));
		break;
	case NATIVE_NQD_FILLMASK_HOOK:
		gpr(3) = NQD_fillmask_hook(gpr(3));
		break;
	case NATIVE_NQD_BLTMASK:
		NQD_bltmask(gpr(3));
		break;
	case NATIVE_NQD_FILLMASK:
		NQD_fillmask(gpr(3));
		break;
	case NATIVE_SERIAL_NOTHING:
	case NATIVE_SERIAL_OPEN:
	case NATIVE_SERIAL_PRIME_IN:
	case NATIVE_SERIAL_PRIME_OUT:
	case NATIVE_SERIAL_CONTROL:
	case NATIVE_SERIAL_STATUS:
	case NATIVE_SERIAL_CLOSE: {
		typedef int16 (*SerialCallback)(uint32, uint32);
		static const SerialCallback serial_callbacks[] = {
			SerialNothing,
			SerialOpen,
			SerialPrimeIn,
			SerialPrimeOut,
			SerialControl,
			SerialStatus,
			SerialClose
		};
		gpr(3) = serial_callbacks[selector - NATIVE_SERIAL_NOTHING](gpr(3), gpr(4));
		break;
	}
	case NATIVE_GET_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_RESOURCE));
		break;
	case NATIVE_GET_1_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_1_RESOURCE));
		break;
	case NATIVE_GET_IND_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_IND_RESOURCE));
		break;
	case NATIVE_GET_1_IND_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_1_IND_RESOURCE));
		break;
	case NATIVE_R_GET_RESOURCE:
		get_resource(ReadMacInt32(XLM_R_GET_RESOURCE));
		break;
	case NATIVE_MAKE_EXECUTABLE:
		MakeExecutable(0, gpr(4), gpr(5));
		break;
	case NATIVE_CHECK_LOAD_INVOC:
		check_load_invoc(gpr(3), gpr(4), gpr(5));
		break;
	case NATIVE_NAMED_CHECK_LOAD_INVOC:
		named_check_load_invoc(gpr(3), gpr(4), gpr(5));
		break;
	case NATIVE_RAVE_DISPATCH: {
#if !defined(ENABLE_GFXACCEL)
		gpr(3) = (uint32)-1;
		break;
#else
		// Save critical PPC registers that re-entrant PPC code could corrupt.
		// Metal/SDL initialization during DrawContextNew can trigger event
		// processing or CFM callbacks that re-enter the PPC emulator.
		uint32 saved_lr = lr();
		uint32 saved_ctr = ctr();
		uint32 saved_sp = gpr(1);
		uint32 saved_r2 = gpr(2);
		uint32 saved_pc = pc();

		// Read sub-opcode BEFORE dispatch for targeted logging
		uint32 pre_subop = ReadMacInt32(rave_scratch_addr);

		// For hook sub-opcodes (200-207), log full PPC state
		if (pre_subop >= 200 && pre_subop <= 207) {
			D(bug("RAVE NATIVE_OP: subop=%d PC=0x%08x LR=0x%08x CTR=0x%08x SP=0x%08x R2=0x%08x\n",
				   pre_subop, saved_pc, saved_lr, saved_ctr, saved_sp, saved_r2));
			// Dump instructions at LR (return address) to see what caller expects
			D(bug("RAVE NATIVE_OP: instructions at LR 0x%08x:\n", saved_lr));
			for (int di = -2; di < 6; di++) {
				uint32 addr = saved_lr + di * 4;
				uint32 instr = ReadMacInt32(addr);
				D(bug("  [0x%08x] %08x%s\n", addr, instr, di == 0 ? " <-- LR" : ""));
			}
		}

		// PPC calling convention: float arguments go in FPR, not GPR.
		// SetFloat(drawContext, tag, value) has value as a float in fpr(1).
		// We must extract the float bits and place them in r5 so the
		// dispatch handler receives the correct value via gpr(5).
		if (pre_subop == 0) {  // kRaveDrawSetFloat
			float fval = (float)fpr(1);
			uint32 fbits;
			memcpy(&fbits, &fval, sizeof(uint32));
			gpr(5) = fbits;
		}

		uint32 rave_ret = RaveDispatchARC(gpr(3), gpr(4), gpr(5), gpr(6), gpr(7), gpr(8));
		gpr(3) = rave_ret;

		// GetFloat returns float bits in gpr(3). PPC caller also expects
		// the float return value in fpr(1).
		if (pre_subop == 3) {  // kRaveDrawGetFloat
			float fval;
			memcpy(&fval, &rave_ret, sizeof(float));
			fpr(1) = (double)fval;
		}

		// Check all registers for corruption
		if (lr() != saved_lr) {
			printf("RAVE: LR CORRUPTED during native op! was 0x%08x, now 0x%08x - restoring\n",
				   saved_lr, lr());
			lr() = saved_lr;
		}
		if (ctr() != saved_ctr) {
			printf("RAVE: CTR CORRUPTED during native op! was 0x%08x, now 0x%08x - restoring\n",
				   saved_ctr, ctr());
			ctr() = saved_ctr;
		}
		if (gpr(1) != saved_sp) {
			printf("RAVE: SP CORRUPTED during native op! was 0x%08x, now 0x%08x - restoring\n",
				   saved_sp, gpr(1));
			gpr(1) = saved_sp;
		}
		if (gpr(2) != saved_r2) {
			printf("RAVE: R2(TOC) CORRUPTED during native op! was 0x%08x, now 0x%08x - restoring\n",
				   saved_r2, gpr(2));
			gpr(2) = saved_r2;
		}

		// For sub-opcode 204 (DrawContextNew), log the return value
		if (pre_subop == 204) {
			D(bug("RAVE NATIVE_OP: DrawContextNew returning %d, blr will go to LR=0x%08x\n",
				   (int32)rave_ret, lr()));
		}
		break;
#endif
	}
	case NATIVE_OPENGL_DISPATCH: {
#if !defined(ENABLE_GFXACCEL)
		gpr(3) = (uint32)-1;
		break;
#else
		// Save critical PPC registers (same pattern as RAVE -- Metal/SDL init
		// can trigger re-entrant PPC execution that corrupts these).
		uint32 saved_lr = lr();
		uint32 saved_ctr = ctr();
		uint32 saved_sp = gpr(1);
		uint32 saved_r2 = gpr(2);

		// Read sub-opcode from GL scratch word
		uint32 sub_opcode = ReadMacInt32(gl_scratch_addr);

		// Check dispatch-table flag: if set, the game called through the
		// context's internal dispatch table and passed the context index
		// in R3 with real GL args starting at R4.  Shift args left by one.
		extern uint32_t gl_dt_flag_addr;
		uint32 dt_flag = ReadMacInt32(gl_dt_flag_addr);
		WriteMacInt32(gl_dt_flag_addr, 0);  // clear for next call

		uint32 arg_r3, arg_r4, arg_r5, arg_r6, arg_r7, arg_r8, arg_r9, arg_r10;
		if (dt_flag) {
			// Dispatch-table path: skip context index in R3, shift args.
			// r4-r10 become args 0-6. Arg 7 (r10) is set to 0 for now;
			// functions with 9+ args (like glTexImage2D) read additional
			// args from the PPC stack via gl_ppc_stack_arg().
			arg_r3 = gpr(4);  arg_r4 = gpr(5);  arg_r5 = gpr(6);
			arg_r6 = gpr(7);  arg_r7 = gpr(8);  arg_r8 = gpr(9);
			arg_r9 = gpr(10); arg_r10 = 0;
		} else {
			// Stub-patching path: args in normal positions
			arg_r3 = gpr(3);  arg_r4 = gpr(4);  arg_r5 = gpr(5);
			arg_r6 = gpr(6);  arg_r7 = gpr(7);  arg_r8 = gpr(8);
			arg_r9 = gpr(9);  arg_r10 = gpr(10);
		}

		// Generic FPR extraction based on function signature table.
		// PPC ABI passes float/double args in FPR1-FPR13. We extract them
		// into a uint32 array so GLDispatch can reconstruct float values.
		// Dispatch-table calls shift GPR args only; FPR numbering is unchanged
		// because the context index is an integer argument.
		const GLFuncSignature& sig = gl_func_signatures[sub_opcode < GL_MAX_SUBOPCODE ? sub_opcode : 0];
		const int max_ppc_fpr_args = 13;
		const int max_float_mask_bits = (int)(sizeof(sig.float_mask) * 8);
		uint32 float_bits[max_ppc_fpr_args] = {0};  // Max 13 FPR args in PPC ABI
		int fpr_idx = 0;
		for (int i = 0; i < sig.num_args && i < max_float_mask_bits && fpr_idx < max_ppc_fpr_args; i++) {
			if (sig.float_mask & (1u << i)) {
				// This arg position is a float/double -- extract from next FPR.
				// PPC ABI: floats are promoted to double in FPR, cast back to float.
				float fval = (float)fpr(1 + fpr_idx);
				memcpy(&float_bits[fpr_idx], &fval, 4);
				fpr_idx++;
			}
		}

		// Save PPC stack pointer for functions with 9+ args (e.g., glTexImage2D)
		// When dt_flag is set, stack args are shifted by 1 position because
		// the context arg consumed one GPR slot, pushing all subsequent args.
		{
			extern uint32_t gl_ppc_sp;
			extern int gl_ppc_stack_arg_offset;
			gl_ppc_sp = saved_sp;
			gl_ppc_stack_arg_offset = dt_flag ? 1 : 0;
		}

		// Call dispatch with GPR args and extracted float bits
		gpr(3) = GLDispatchARC(arg_r3, arg_r4, arg_r5, arg_r6,
		                    arg_r7, arg_r8, arg_r9, arg_r10,
		                    float_bits, fpr_idx);

		// Restore registers that may have been corrupted
		lr() = saved_lr;
		ctr() = saved_ctr;
		if (gpr(1) != saved_sp) gpr(1) = saved_sp;
		if (gpr(2) != saved_r2) gpr(2) = saved_r2;

		break;
#endif
	}
	case NATIVE_DSP_DISPATCH: {
#if !defined(ENABLE_GFXACCEL)
		gpr(3) = (uint32)-1;
		break;
#else
		// No Metal resources, so no @autoreleasepool wrapper is needed
		// here; the context-lifecycle path wraps in @autoreleasepool once
		// GPU calls (GetBackBuffer vending a MTLTexture) land. Register-
		// preservation pattern mirrors RAVE/GL for re-entrant PPC safety.
		uint32 saved_lr = lr();
		uint32 saved_ctr = ctr();
		uint32 saved_sp = gpr(1);
		uint32 saved_r2 = gpr(2);

		/* Stash caller LR + r11
		 * for the one-shot DSpDispatch diagnostic on unresolved sub-opcodes
		 * 400/401/501/502/600. r11 carries the CFM TVECT address under the
		 * CFM ABI, so it identifies WHICH DSp symbol the caller jumped
		 * through. Globals are defined in dsp_dispatch.cpp; single-thread
		 * (emul) read+write so no synchronisation required. */
		{
			extern uint32_t dsp_caller_lr;
			extern uint32_t dsp_caller_r11;
			dsp_caller_lr = saved_lr;
			dsp_caller_r11 = gpr(11);
		}

		uint32 dsp_ret = DSpDispatch(gpr(3), gpr(4), gpr(5), gpr(6), gpr(7), gpr(8));
		gpr(3) = dsp_ret;

		if (lr() != saved_lr) { lr() = saved_lr; }
		if (ctr() != saved_ctr) { ctr() = saved_ctr; }
		if (gpr(1) != saved_sp) { gpr(1) = saved_sp; }
		if (gpr(2) != saved_r2) { gpr(2) = saved_r2; }
		break;
#endif
	}
  #if defined(ENABLE_NATIVE_CINEPAK_PATCH) \
			&& ENABLE_NATIVE_CINEPAK_PATCH
	case NATIVE_OPENDEFAULTCOMPONENT_CINEPAK_HOOK:
	case NATIVE_FINDNEXTCOMPONENT_CINEPAK_HOOK: {
	#if !defined(ENABLE_GFXACCEL)
			gpr(3) = 0;
			break;
	#else
			/* FN=1 first-instruction hooks: execute_sheep does pc() = lr() right
			 * after this returns, and the handlers run call_macos (nested guest
			 * execution) which clobbers LR/CTR - save/restore is mandatory or the
			 * return lands in the nested callee instead of the real caller. */
			uint32 saved_lr = lr();
			uint32 saved_ctr = ctr();
			uint32 saved_sp = gpr(1);
			uint32 saved_r2 = gpr(2);

			if (selector == NATIVE_OPENDEFAULTCOMPONENT_CINEPAK_HOOK)
				gpr(3) = CinepakOpenDefaultComponentHook(gpr(3), gpr(4));
			else
				gpr(3) = CinepakFindNextComponentHook(gpr(3), gpr(4));

			if (lr() != saved_lr) { lr() = saved_lr; }
			if (ctr() != saved_ctr) { ctr() = saved_ctr; }
			if (gpr(1) != saved_sp) { gpr(1) = saved_sp; }
			if (gpr(2) != saved_r2) { gpr(2) = saved_r2; }
			break;
	#endif
	}
	case NATIVE_CINEPAK_DISPATCH: {
	#if !defined(ENABLE_GFXACCEL)
			gpr(3) = (uint32)-50; /* paramErr */
			break;
	#else
			/* Component entry (via routine descriptor): pure host work, but keep
			 * the same register-preservation pattern for safety. */
			uint32 saved_lr = lr();
			uint32 saved_ctr = ctr();
			uint32 saved_sp = gpr(1);
			uint32 saved_r2 = gpr(2);

			gpr(3) = CinepakDispatch(gpr(3));

			if (lr() != saved_lr) { lr() = saved_lr; }
			if (ctr() != saved_ctr) { ctr() = saved_ctr; }
			if (gpr(1) != saved_sp) { gpr(1) = saved_sp; }
			if (gpr(2) != saved_r2) { gpr(2) = saved_r2; }
			break;
	#endif
	}
#endif /* ENABLE_NATIVE_CINEPAK_PATCH */
	default:
		printf("FATAL: NATIVE_OP called with bogus selector %d\n", selector);
		QuitEmulator();
		break;
	}
#if EMUL_TIME_STATS
	native_exec_time += (clock() - native_exec_start);
#endif
}

/*
 *  Execute 68k subroutine (must be ended with EXEC_RETURN)
 *  This must only be called by the emul_thread when in EMUL_OP mode
 *  r->a[7] is unused, the routine runs on the caller's stack
 */

void Execute68k(uint32 pc, M68kRegisters *r)
{
#if QD3D_WAIT_LOGGING_ENABLED
	/* Tripwire: name any single 68k call that burns real wall time (the movie
	 * one-time setup freeze is an uninterruptible ~1s guest call that the
	 * IRQ-driven sampler cannot catch). Logs entry pc + duration for calls
	 * over the threshold. */
	const uint64 exec68k_t0 = GetTicks_usec();
	ppc_cpu->execute_68k(pc, r);
	const uint64 exec68k_us = GetTicks_usec() - exec68k_t0;
	if (exec68k_us >= 100000ull)
		QD3D_WAIT_LOG("Execute68k SLOW pc=0x%08x usec=%llu tick=%u",
		              pc, (unsigned long long)exec68k_us, ReadMacInt32(0x016a));
#else
	ppc_cpu->execute_68k(pc, r);
#endif
}

/*
 *  Execute 68k A-Trap from EMUL_OP routine
 *  r->a[7] is unused, the routine runs on the caller's stack
 */

void Execute68kTrap(uint16 trap, M68kRegisters *r)
{
	SheepVar proc_var(4);
	uint32 proc = proc_var.addr();
	WriteMacInt16(proc, trap);
	WriteMacInt16(proc + 2, M68K_RTS);
	Execute68k(proc, r);
}

/*
 *  Call MacOS PPC code
 */

uint32 call_macos(uint32 tvect)
{
	return ppc_cpu->execute_macos_code(tvect, 0, NULL);
}

uint32 call_macos1(uint32 tvect, uint32 arg1)
{
	const uint32 args[] = { arg1 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos2(uint32 tvect, uint32 arg1, uint32 arg2)
{
	const uint32 args[] = { arg1, arg2 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos3(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3)
{
	const uint32 args[] = { arg1, arg2, arg3 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos4(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos5(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos6(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5, arg6 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos7(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6, uint32 arg7)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5, arg6, arg7 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

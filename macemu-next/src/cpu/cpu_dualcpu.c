/**
 * DualCPU Backend for Platform API
 *
 * Runs UAE and Unicorn in lockstep for validation.
 * Executes instruction on both CPUs and compares results.
 * Always available, no compile-time dependencies.
 */

#include "sysdeps.h"            // For uae_u32 types
#include "platform.h"
#include "uae_wrapper.h"
#include "unicorn_wrapper.h"
#include "unicorn_validation.h"
#include "uae_cpu/spcflags.h"   // For SPCFLAG_INT
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// External memory pointers from BasiliskII
extern uint32_t RAMBaseMac;
extern uint8_t *RAMBaseHost;
extern uint32_t RAMSize;
extern uint32_t ROMBaseMac;
extern uint8_t *ROMBaseHost;
extern uint32_t ROMSize;

// UAE CPU internals (forward declaration)
extern struct regstruct {
	uint32_t regs[16];
	uint32_t pc;
	uint8_t *pc_p;
	uint8_t *pc_oldp;
	uint32_t spcflags;
	int intmask;
	uint32_t vbr, sfc, dfc;
	uint32_t usp, isp, msp;
	uint16_t sr;
	char t1, t0, s, m, x;
	char stopped;
} regs;

extern bool quit_program;

// CPU Configuration
static void dualcpu_backend_set_type(int cpu_type, int fpu_type) {
	// Set CPU type for both UAE and Unicorn
	uae_set_cpu_type(cpu_type, fpu_type);
	unicorn_set_cpu_type(cpu_type, fpu_type);
	fprintf(stderr, "[DualCPU] CPU type set to %d, FPU=%d (for both UAE and Unicorn)\n", cpu_type, fpu_type);
}

// CPU Lifecycle
static bool dualcpu_backend_init(void) {
	// Initialize UAE
	if (!uae_cpu_init()) {
		fprintf(stderr, "DualCPU: Failed to initialize UAE\n");
		return false;
	}

	// Initialize Unicorn validation
	if (!unicorn_validation_init()) {
		fprintf(stderr, "DualCPU: Failed to initialize Unicorn validation\n");
		return false;
	}

	unicorn_validation_set_enabled(true);
	return true;
}

static void dualcpu_backend_reset(void) {
	// Explicit initialization for both CPUs to Macintosh boot state
	// This is done by the validation module, which initializes both CPUs identically
	// Don't call uae_cpu_reset() here - that would overwrite the explicit state
	// The validation module handles initialization in unicorn_validation_init()
}

static void dualcpu_backend_destroy(void) {
	unicorn_validation_shutdown();
}

// Execution - runs both CPUs and compares
static int dualcpu_backend_execute_one(void) {
	// Execute on both CPUs and validate
	if (!unicorn_validation_step()) {
		return 5;  // CPU_EXEC_DIVERGENCE
	}

	// Check UAE state (validation_step already executed UAE)
	if (regs.stopped) {
		return 1;  // CPU_EXEC_STOPPED
	}

	if (quit_program) {
		quit_program = false;
		return 4;  // CPU_EXEC_EMULOP
	}

	return 0;  // CPU_EXEC_OK
}

static void dualcpu_backend_execute_fast(void) {
	// DualCPU doesn't support fast path (validation is per-instruction)
}

// State Query - delegates to UAE (accesses regs.stopped directly)
static bool dualcpu_backend_is_stopped(void) {
	return regs.stopped != 0;
}

static uint32_t dualcpu_backend_get_pc(void) {
	return uae_get_pc();
}

static uint16_t dualcpu_backend_get_sr(void) {
	return uae_get_sr();
}

static uint32_t dualcpu_backend_get_dreg(int n) {
	return uae_get_dreg(n);
}

static uint32_t dualcpu_backend_get_areg(int n) {
	return uae_get_areg(n);
}

// State Modification - updates both CPUs
static void dualcpu_backend_set_pc(uint32_t pc) {
	uae_set_pc(pc);
	// Unicorn will be synced via validation module
}

static void dualcpu_backend_set_sr(uint16_t sr) {
	uae_set_sr(sr);
	// Unicorn will be synced via validation module
}

static void dualcpu_backend_set_dreg(int n, uint32_t val) {
	uae_set_dreg(n, val);
	// Unicorn will be synced via validation module
}

static void dualcpu_backend_set_areg(int n, uint32_t val) {
	uae_set_areg(n, val);
	// Unicorn will be synced via validation module
}

// Memory Access - delegates to UAE
static void dualcpu_backend_mem_read(uint32_t addr, void *data, uint32_t size) {
	uae_mem_read(addr, data, size);
}

static void dualcpu_backend_mem_write(uint32_t addr, const void *data, uint32_t size) {
	uae_mem_write(addr, data, size);
}

// Interrupts - delegates to UAE internals
static void dualcpu_backend_trigger_interrupt(int level) {
	// Set UAE's interrupt flag directly (same as UAE backend does)
	if (level > 0 && level <= 7) {
		SPCFLAGS_SET(SPCFLAG_INT);
	}
	// Unicorn will be synced via validation module
}

// 68k Trap execution - delegates to UAE implementation
static void dualcpu_backend_execute_68k_trap(uint16_t trap, struct M68kRegisters *r) {
	// Call UAE's native trap execution
	extern void Execute68kTrap(uint16_t trap, struct M68kRegisters *r);
	Execute68kTrap(trap, r);
	// Unicorn will be synced via validation module on next step
}

/**
 * Install DualCPU backend into platform
 */
void cpu_dualcpu_install(Platform *p) {
	p->cpu_name = "DualCPU (UAE + Unicorn Validation)";

	// Configuration
	p->cpu_set_type = dualcpu_backend_set_type;

	// Lifecycle
	p->cpu_init = dualcpu_backend_init;
	p->cpu_reset = dualcpu_backend_reset;
	p->cpu_destroy = dualcpu_backend_destroy;

	// Execution
	p->cpu_execute_one = dualcpu_backend_execute_one;
	p->cpu_execute_fast = NULL;  // No fast path for validation

	// State query
	p->cpu_is_stopped = dualcpu_backend_is_stopped;
	p->cpu_get_pc = dualcpu_backend_get_pc;
	p->cpu_get_sr = dualcpu_backend_get_sr;
	p->cpu_get_dreg = dualcpu_backend_get_dreg;
	p->cpu_get_areg = dualcpu_backend_get_areg;

	// State modification
	p->cpu_set_pc = dualcpu_backend_set_pc;
	p->cpu_set_sr = dualcpu_backend_set_sr;
	p->cpu_set_dreg = dualcpu_backend_set_dreg;
	p->cpu_set_areg = dualcpu_backend_set_areg;

	// Memory access
	p->cpu_mem_read = dualcpu_backend_mem_read;
	p->cpu_mem_write = dualcpu_backend_mem_write;

	// Interrupts
	p->cpu_trigger_interrupt = dualcpu_backend_trigger_interrupt;

	// 68k Trap execution
	p->cpu_execute_68k_trap = dualcpu_backend_execute_68k_trap;

	// EmulOp/Trap handlers - unified handlers that check DUALCPU_MASTER env var
	// to determine which CPU is primary (UAE or Unicorn)
	// Returns true if PC was advanced, false if caller should advance
	extern bool unicorn_validation_unified_emulop(uint16_t opcode, bool is_primary);
	extern bool unicorn_validation_unified_trap(int vector, uint16_t opcode, bool is_primary);
	p->emulop_handler = unicorn_validation_unified_emulop;
	p->trap_handler = unicorn_validation_unified_trap;
}

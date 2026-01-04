/**
 * UAE CPU Backend for Platform API
 *
 * Wraps UAE interpreter to conform to platform CPU interface.
 * Always available, no compile-time dependencies.
 */

#include "platform.h"
#include "sysdeps.h"  // For UAE types (uae_u32, etc.)
#include "uae_wrapper.h"
#include "uae_cpu/spcflags.h"  // For SPCFLAG_INT
#include <stdbool.h>
#include <stddef.h>

// UAE internals (minimal forward declarations)
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
static void uae_backend_set_type(int cpu_type, int fpu_type) {
	uae_set_cpu_type(cpu_type, fpu_type);
}

// CPU Lifecycle
static bool uae_backend_init(void) {
	return uae_cpu_init();
}

static void uae_backend_reset(void) {
	uae_cpu_reset();
}

static void uae_backend_destroy(void) {
	// UAE doesn't need cleanup
}

// Execution
static int uae_backend_execute_one(void) {
	uae_cpu_execute_one();

	if (regs.stopped) {
		return 1;  // CPU_EXEC_STOPPED
	}

	if (quit_program) {
		quit_program = false;
		return 4;  // CPU_EXEC_EMULOP
	}

	return 0;  // CPU_EXEC_OK
}

static void uae_backend_execute_fast(void) {
	// UAE interpreter doesn't have fast path
	// Caller should use execute_one() in a loop
}

// State Query
static bool uae_backend_is_stopped(void) {
	return regs.stopped != 0;
}

static uint32_t uae_backend_get_pc(void) {
	return uae_get_pc();
}

static uint16_t uae_backend_get_sr(void) {
	return uae_get_sr();
}

static uint32_t uae_backend_get_dreg(int n) {
	return uae_get_dreg(n);
}

static uint32_t uae_backend_get_areg(int n) {
	return uae_get_areg(n);
}

// State Modification
static void uae_backend_set_pc(uint32_t pc) {
	uae_set_pc(pc);
}

static void uae_backend_set_sr(uint16_t sr) {
	uae_set_sr(sr);
}

static void uae_backend_set_dreg(int n, uint32_t val) {
	uae_set_dreg(n, val);
}

static void uae_backend_set_areg(int n, uint32_t val) {
	uae_set_areg(n, val);
}

// Memory Access
static void uae_backend_mem_read(uint32_t addr, void *data, uint32_t size) {
	uae_mem_read(addr, data, size);
}

static void uae_backend_mem_write(uint32_t addr, const void *data, uint32_t size) {
	uae_mem_write(addr, data, size);
}

// Interrupts
static void uae_backend_trigger_interrupt(int level) {
	/* Set interrupt flag - UAE's do_specialties() will check this
	 * and call Interrupt(level) which handles everything natively:
	 * - Builds M68K exception stack frame (SR, PC, Format/Vector for 68020+)
	 * - Sets supervisor mode
	 * - Updates interrupt mask
	 * - Reads vector table
	 * - Jumps to interrupt handler
	 * RTE is handled natively by UAE interpreter
	 */
	if (level > 0 && level <= 7) {
		SPCFLAGS_SET(SPCFLAG_INT);
	}
}

/**
 * Install UAE CPU backend into platform
 */
void cpu_uae_install(Platform *p) {
	p->cpu_name = "UAE Interpreter";

	// Configuration
	p->cpu_set_type = uae_backend_set_type;

	// Lifecycle
	p->cpu_init = uae_backend_init;
	p->cpu_reset = uae_backend_reset;
	p->cpu_destroy = uae_backend_destroy;

	// Execution
	p->cpu_execute_one = uae_backend_execute_one;
	p->cpu_execute_fast = NULL;  // No fast path

	// State query
	p->cpu_is_stopped = uae_backend_is_stopped;
	p->cpu_get_pc = uae_backend_get_pc;
	p->cpu_get_sr = uae_backend_get_sr;
	p->cpu_get_dreg = uae_backend_get_dreg;
	p->cpu_get_areg = uae_backend_get_areg;

	// State modification
	p->cpu_set_pc = uae_backend_set_pc;
	p->cpu_set_sr = uae_backend_set_sr;
	p->cpu_set_dreg = uae_backend_set_dreg;
	p->cpu_set_areg = uae_backend_set_areg;

	// Memory access
	p->cpu_mem_read = uae_backend_mem_read;
	p->cpu_mem_write = uae_backend_mem_write;

	// Interrupts
	p->cpu_trigger_interrupt = uae_backend_trigger_interrupt;

	// Memory system (for ROM patching and initialization)
	p->mem_read_byte = uae_mem_read_byte;
	p->mem_read_word = uae_mem_read_word;
	p->mem_read_long = uae_mem_read_long;
	p->mem_write_byte = uae_mem_write_byte;
	p->mem_write_word = uae_mem_write_word;
	p->mem_write_long = uae_mem_write_long;
	p->mem_mac_to_host = uae_mem_mac_to_host;
	p->mem_host_to_mac = uae_mem_host_to_mac;
}

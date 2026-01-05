/**
 * Unicorn CPU Backend for Platform API
 *
 * Wraps Unicorn engine to conform to platform CPU interface.
 * Always available, no compile-time dependencies.
 */

#include "platform.h"
#include "unicorn_wrapper.h"
#include "unicorn_exception.h"
#include "cpu_trace.h"
#include "memory_access.h"  // For direct memory access (UAE-independent)
#include <unicorn/unicorn.h>
#include <unicorn/m68k.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>  // For memset

// M68kRegisters structure (from main.h, duplicated to avoid type conflicts)
struct M68kRegisters {
	uint32_t d[8];
	uint32_t a[8];
	uint16_t sr;
};

// Forward declarations (from macemu globals)
extern uint32_t RAMBaseMac;  // RAM base in Mac address space
extern uint8_t *RAMBaseHost; // RAM base in host address space
extern uint32_t RAMSize;     // RAM size
extern uint32_t ROMBaseMac;  // ROM base in Mac address space
extern uint8_t *ROMBaseHost; // ROM base in host address space
extern uint32_t ROMSize;     // ROM size
extern void EmulOp(uint16_t opcode, struct M68kRegisters *r);
extern int CPUType;          // CPU type from config (2=68020, 3=68030, 4=68040)

static UnicornCPU *unicorn_cpu = NULL;
static uint8_t *unicorn_dummy_buffer = NULL;  // Dummy region for UAE out-of-bounds compatibility
static uint8_t *unicorn_high_mem_buffer = NULL;  // High memory region (hardware registers, etc.)
int unicorn_cpu_type = 2;   // Default to 68020 (extern for DualCPU)
int unicorn_fpu_type = 0;   // Default to no FPU (extern for DualCPU)

// CPU Configuration
static void unicorn_backend_set_type(int cpu_type, int fpu_type) {
	unicorn_cpu_type = cpu_type;
	unicorn_fpu_type = fpu_type;
	fprintf(stderr, "[Unicorn] CPU type set to %d, FPU=%d\n", cpu_type, fpu_type);
}

// For DualCPU backend - simple wrapper function
void unicorn_set_cpu_type(int cpu_type, int fpu_type) {
	unicorn_backend_set_type(cpu_type, fpu_type);
}

// Platform EmulOp handler for Unicorn-only mode
// This needs to use platform API because it's called from within Unicorn's hook context
// and needs to properly sync registers back to Unicorn
static bool unicorn_platform_emulop_handler(uint16_t opcode, bool is_primary) {
	(void)is_primary;  // Unicorn is always primary in standalone mode

	// Build M68kRegisters structure from Unicorn state
	struct M68kRegisters regs;
	for (int i = 0; i < 8; i++) {
		regs.d[i] = unicorn_get_dreg(unicorn_cpu, i);
		regs.a[i] = unicorn_get_areg(unicorn_cpu, i);
	}
	regs.sr = unicorn_get_sr(unicorn_cpu);

	// Call EmulOp handler
	EmulOp(opcode, &regs);

	// IMPORTANT: Write registers back to Unicorn directly
	// We're outside uc_emu_start() so register writes will persist
	for (int i = 0; i < 8; i++) {
		g_platform.cpu_set_dreg(i, regs.d[i]);
		g_platform.cpu_set_areg(i, regs.a[i]);
	}
	g_platform.cpu_set_sr(regs.sr);

	// Debug: Verify A7 write for RESET EmulOp
	if (opcode == 0x7103) {
		uint32_t a7_readback = g_platform.cpu_get_areg(7);
		fprintf(stderr, "[EmulOp 0x7103] Set A7=0x%08X, readback=0x%08X\n",
		        regs.a[7], a7_readback);
	}

	// Return false to indicate PC was not advanced (caller will advance it)
	return false;
}

// Platform trap handler for Unicorn-only mode
// Handles A-line and F-line traps by simulating M68K exceptions
static bool unicorn_platform_trap_handler(int vector, uint16_t opcode, bool is_primary) {
	(void)is_primary; // Unicorn is always primary in standalone mode

	fprintf(stderr, "[DEBUG] Trap handler called: vector=%d, opcode=0x%04X, PC=0x%08X\n",
	        vector, opcode, unicorn_get_pc(unicorn_cpu));

	// Use Unicorn's exception simulation (defined in unicorn_exception.c)
	extern void unicorn_simulate_exception(UnicornCPU *cpu, int vector_nr, uint16_t opcode);
	unicorn_simulate_exception(unicorn_cpu, vector, opcode);

	fprintf(stderr, "[DEBUG] After trap: new PC=0x%08X\n", unicorn_get_pc(unicorn_cpu));

	// Return true to indicate we handled PC advancement
	return true;
}

// Unmapped memory handlers - mimic UAE's dummy_bank behavior
// UAE silently ignores all unmapped reads (returns 0) and writes (no-op)
static bool unicorn_unmapped_read_handler(uc_engine *uc, uc_mem_type type,
                                          uint64_t address, int size,
                                          int64_t value, void *user_data) {
	(void)uc;
	(void)type;
	(void)value;
	(void)user_data;

	// Return 0 for unmapped reads (matches UAE dummy_bank behavior)
	// Note: Unicorn will use the 'value' we would set, but we can't set it in the hook
	// The hook just prevents the error - actual value returned is undefined
	fprintf(stderr, "[Unicorn] Unmapped read at 0x%08lX (size=%d) - returning 0 (UAE compat)\n",
	        address, size);
	return true;  // Prevent UC_ERR_READ_UNMAPPED
}

static bool unicorn_unmapped_write_handler(uc_engine *uc, uc_mem_type type,
                                           uint64_t address, int size,
                                           int64_t value, void *user_data) {
	(void)uc;
	(void)type;
	(void)value;
	(void)user_data;

	// Silently ignore unmapped writes (matches UAE dummy_bank behavior)
	fprintf(stderr, "[Unicorn] Unmapped write at 0x%08lX (size=%d, value=0x%lX) - ignored (UAE compat)\n",
	        address, size, (unsigned long)value);
	return true;  // Prevent UC_ERR_WRITE_UNMAPPED
}

// CPU Lifecycle
static bool unicorn_backend_init(void) {
	if (unicorn_cpu) {
		return true;  // Already initialized
	}

	// Create Unicorn CPU with model from cpu_set_type()
	// Follow same logic as UAE's cpu_level calculation:
	// - If cpu_type==4: use 68040 (with FPU)
	// - Else if fpu_type: use 68030 (68020 with FPU)
	// - Else if cpu_type>=2: use 68020
	// - Else: use 68000
	// NOTE: Unicorn's CPU table uses array indices, not UC_CPU_M68K enum values!
	// Array order: 0=m68000, 1=m68020, 2=m68030, 3=m68040, 4=m68060...
	int uc_model;
	if (unicorn_cpu_type == 4) {
		uc_model = 3;  // 68040 (array index)
	} else {
		if (unicorn_fpu_type)
			uc_model = 2;  // 68030 (array index)
		else if (unicorn_cpu_type >= 2)
			uc_model = 1;  // 68020 (array index)
		else
			uc_model = 0;  // 68000 (array index)
	}

	fprintf(stderr, "[Unicorn] Creating CPU with model %d (array index, cpu_type=%d, fpu=%d) - matches UAE cpu_level\n",
		uc_model, unicorn_cpu_type, unicorn_fpu_type);
	unicorn_cpu = unicorn_create_with_model(UCPU_ARCH_M68K, uc_model);
	if (!unicorn_cpu) {
		fprintf(stderr, "Failed to create Unicorn CPU\n");
		return false;
	}

	// Map RAM to Unicorn
	fprintf(stderr, "[DEBUG] Mapping RAM to unicorn_cpu=%p: Mac=0x%08X Host=%p Size=0x%08X (%u MB)\n",
		(void*)unicorn_cpu, RAMBaseMac, RAMBaseHost, RAMSize, RAMSize / (1024*1024));
	if (!unicorn_map_ram(unicorn_cpu, RAMBaseMac, RAMBaseHost, RAMSize)) {
		fprintf(stderr, "Failed to map RAM to Unicorn\n");
		unicorn_destroy(unicorn_cpu);
		unicorn_cpu = NULL;
		return false;
	}

	// Map ROM as writable (BasiliskII patches ROM during boot)
	fprintf(stderr, "[DEBUG] Mapping ROM to unicorn_cpu=%p: Mac=0x%08X Host=%p Size=0x%08X (%u KB)\n",
		(void*)unicorn_cpu, ROMBaseMac, ROMBaseHost, ROMSize, ROMSize / 1024);
	if (!unicorn_map_rom_writable(unicorn_cpu, ROMBaseMac, ROMBaseHost, ROMSize)) {
		fprintf(stderr, "Failed to map ROM to Unicorn\n");
		unicorn_destroy(unicorn_cpu);
		unicorn_cpu = NULL;
		return false;
	}

	// Map dummy region after ROM to handle UAE's out-of-bounds reads
	// UAE has a bug where it reads past ROM end without bounds checking
	// We need to provide the same memory layout that UAE sees for compatibility
	uint32_t dummy_region_base = ROMBaseMac + ROMSize;
	uint32_t dummy_region_size = 16 * 1024 * 1024;  // 16 MB
	unicorn_dummy_buffer = (uint8_t *)malloc(dummy_region_size);
	if (!unicorn_dummy_buffer) {
		fprintf(stderr, "Failed to allocate dummy region buffer\n");
		unicorn_destroy(unicorn_cpu);
		unicorn_cpu = NULL;
		return false;
	}
	// Fill with 0xFF00FF00 pattern (same as UAE reads from uninitialized memory)
	// Write in big-endian format for M68K
	for (uint32_t i = 0; i < dummy_region_size; i += 4) {
		unicorn_dummy_buffer[i + 0] = 0xFF;  // MSB
		unicorn_dummy_buffer[i + 1] = 0x00;
		unicorn_dummy_buffer[i + 2] = 0xFF;
		unicorn_dummy_buffer[i + 3] = 0x00;  // LSB
	}
	if (!unicorn_map_ram(unicorn_cpu, dummy_region_base, unicorn_dummy_buffer, dummy_region_size)) {
		fprintf(stderr, "Failed to map dummy region to Unicorn\n");
		free(unicorn_dummy_buffer);
		unicorn_dummy_buffer = NULL;
		unicorn_destroy(unicorn_cpu);
		unicorn_cpu = NULL;
		return false;
	}
	fprintf(stderr, "[DEBUG] Dummy region mapped: 0x%08X - 0x%08X (%u MB) with 0xFF00FF00 pattern\n",
		dummy_region_base, dummy_region_base + dummy_region_size, dummy_region_size / (1024*1024));

	// Map high memory region (0xF0000000-0xFFFFFFFF) for hardware registers
	// This matches UAE's behavior where the entire 4GB address space is backed by dummy_bank
	// Addresses like 0xFFFFFFFE/0xFFFFFFFC are common hardware register placeholders
	uint32_t high_mem_base = 0xF0000000;
	uint32_t high_mem_size = 0x10000000;  // 256 MB (top of address space)
	unicorn_high_mem_buffer = (uint8_t *)malloc(high_mem_size);
	if (!unicorn_high_mem_buffer) {
		fprintf(stderr, "Failed to allocate high memory region buffer\n");
		free(unicorn_dummy_buffer);
		unicorn_dummy_buffer = NULL;
		unicorn_destroy(unicorn_cpu);
		unicorn_cpu = NULL;
		return false;
	}
	// Fill with zeros (UAE's dummy_bank returns 0 for reads)
	memset(unicorn_high_mem_buffer, 0, high_mem_size);
	if (!unicorn_map_ram(unicorn_cpu, high_mem_base, unicorn_high_mem_buffer, high_mem_size)) {
		fprintf(stderr, "Failed to map high memory region to Unicorn\n");
		free(unicorn_high_mem_buffer);
		unicorn_high_mem_buffer = NULL;
		free(unicorn_dummy_buffer);
		unicorn_dummy_buffer = NULL;
		unicorn_destroy(unicorn_cpu);
		unicorn_cpu = NULL;
		return false;
	}
	fprintf(stderr, "[DEBUG] High memory region mapped: 0x%08X - 0x%08X (%u MB) - UAE compat for hardware registers\n",
		high_mem_base, high_mem_base + high_mem_size - 1, high_mem_size / (1024*1024));

	// Register unmapped memory hooks as fallback for any remaining unmapped regions
	// This matches UAE's behavior where ALL unmapped memory returns 0 / ignores writes
	uc_engine *uc = (uc_engine *)unicorn_get_uc(unicorn_cpu);
	uc_hook unmapped_read_hook, unmapped_write_hook;
	uc_err err = uc_hook_add(uc, &unmapped_read_hook,
	                         UC_HOOK_MEM_READ_UNMAPPED,
	                         (void*)unicorn_unmapped_read_handler,
	                         NULL, 1, 0);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "Warning: Failed to register unmapped read hook: %s\n", uc_strerror(err));
		// Not fatal - high memory region should cover most cases
	}
	err = uc_hook_add(uc, &unmapped_write_hook,
	                  UC_HOOK_MEM_WRITE_UNMAPPED,
	                  (void*)unicorn_unmapped_write_handler,
	                  NULL, 1, 0);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "Warning: Failed to register unmapped write hook: %s\n", uc_strerror(err));
		// Not fatal - high memory region should cover most cases
	}
	fprintf(stderr, "[DEBUG] Unmapped memory hooks registered - full UAE dummy_bank compatibility\n");

	fprintf(stderr, "[DEBUG] unicorn_cpu instance at init: %p\n", (void*)unicorn_cpu);

	// Initialize CPU tracing from environment variable
	cpu_trace_init();

	// Register atexit handler to print block statistics on exit
	static bool atexit_registered = false;
	if (!atexit_registered) {
		atexit([]() {
			if (unicorn_cpu) {
				unicorn_print_block_stats(unicorn_cpu);
			}
		});
		atexit_registered = true;
	}

	// Register EmulOp handler via platform API
	// EmulOps are handled by UC_HOOK_INSN_INVALID which checks g_platform handlers
	g_platform.emulop_handler = unicorn_platform_emulop_handler;

	// Register trap handler for A-line/F-line traps
	// Traps are handled by UC_HOOK_INSN_INVALID which checks g_platform handlers
	g_platform.trap_handler = unicorn_platform_trap_handler;

	// NOTE: Legacy per-CPU exception handler API removed - UC_HOOK_INSN_INVALID
	// automatically checks g_platform.trap_handler for A-line/F-line exceptions

	return true;
}

static void unicorn_backend_reset(void) {
	if (!unicorn_cpu) return;

	// M68K reset: Initialize registers to power-on state
	// IMPORTANT: Set PC and SR first, THEN registers
	// Setting PC may clear A7 in Unicorn, so A7 must be set after PC
	unicorn_set_pc(unicorn_cpu, ROMBaseMac + 0x2a);
	unicorn_set_sr(unicorn_cpu, 0x2700);  // S=1, I=111

	for (int i = 0; i < 8; i++) {
		unicorn_set_dreg(unicorn_cpu, i, 0);
		unicorn_set_areg(unicorn_cpu, i, 0);
	}

	// Set A7 (SSP) after PC to avoid it being cleared
	unicorn_set_areg(unicorn_cpu, 7, 0x2000);

	// Initialize control registers (68040)
	uc_engine *uc = (uc_engine *)unicorn_get_uc(unicorn_cpu);
	uint32_t zero = 0;
	uc_reg_write(uc, UC_M68K_REG_CR_VBR, &zero);   // Vector Base Register = 0
	uc_reg_write(uc, UC_M68K_REG_CR_CACR, &zero);  // Cache Control Register = 0

	// Verify VBR was actually set to 0
	uint32_t vbr_readback = 0;
	uc_reg_read(uc, UC_M68K_REG_CR_VBR, &vbr_readback);
	fprintf(stderr, "[Unicorn] Reset: VBR=0 (readback=0x%08X), CACR=0\n", vbr_readback);
}

static void unicorn_backend_destroy(void) {
	if (unicorn_cpu) {
		// Print block statistics before destroying
		unicorn_print_block_stats(unicorn_cpu);

		unicorn_destroy(unicorn_cpu);
		unicorn_cpu = NULL;
	}
	if (unicorn_dummy_buffer) {
		free(unicorn_dummy_buffer);
		unicorn_dummy_buffer = NULL;
	}
	if (unicorn_high_mem_buffer) {
		free(unicorn_high_mem_buffer);
		unicorn_high_mem_buffer = NULL;
	}
}

// Execution
static int unicorn_backend_execute_one(void) {
	if (!unicorn_cpu) {
		return 3;  // CPU_EXEC_EXCEPTION
	}

	/* CPU tracing (controlled by CPU_TRACE env var) */
	if (cpu_trace_should_log()) {
		uint32_t pc = unicorn_get_pc(unicorn_cpu);
		uint16_t opcode = 0;
		uc_mem_read((uc_engine*)unicorn_get_uc(unicorn_cpu), pc, &opcode, sizeof(opcode));
		#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		opcode = __builtin_bswap16(opcode);
		#endif

		cpu_trace_log_detailed(
			"Unicorn",
			pc, opcode,
			unicorn_get_dreg(unicorn_cpu, 0),
			unicorn_get_dreg(unicorn_cpu, 1),
			unicorn_get_dreg(unicorn_cpu, 2),
			unicorn_get_dreg(unicorn_cpu, 3),
			unicorn_get_dreg(unicorn_cpu, 4),
			unicorn_get_dreg(unicorn_cpu, 5),
			unicorn_get_dreg(unicorn_cpu, 6),
			unicorn_get_dreg(unicorn_cpu, 7),
			unicorn_get_areg(unicorn_cpu, 0),
			unicorn_get_areg(unicorn_cpu, 1),
			unicorn_get_areg(unicorn_cpu, 2),
			unicorn_get_areg(unicorn_cpu, 3),
			unicorn_get_areg(unicorn_cpu, 4),
			unicorn_get_areg(unicorn_cpu, 5),
			unicorn_get_areg(unicorn_cpu, 6),
			unicorn_get_areg(unicorn_cpu, 7),
			unicorn_get_sr(unicorn_cpu)
		);
	}

	if (!unicorn_execute_one(unicorn_cpu)) {
		uint32_t pc = unicorn_get_pc(unicorn_cpu);
		uint32_t a7 = unicorn_get_areg(unicorn_cpu, 7);
		const char *err_str = unicorn_get_error(unicorn_cpu);

		/* Try to read the opcode at PC */
		uint16_t opcode_be = 0;
		uc_engine *uc = (uc_engine*)unicorn_get_uc(unicorn_cpu);
		uc_mem_read(uc, pc, &opcode_be, 2);
		uint16_t opcode = __builtin_bswap16(opcode_be);

		fprintf(stderr, "Unicorn execution failed: %s (unicorn_cpu=%p)\n",
			err_str, (void*)unicorn_cpu);
		fprintf(stderr, "PC=0x%08X opcode=0x%04X A7=0x%08X\n", pc, opcode, a7);

		/* If it's RTE, dump the stack frame for debugging */
		if (opcode == 0x4E73) {
			uint16_t sr_be, fv_be;
			uint32_t ret_pc_be;
			uc_mem_read(uc, a7, &sr_be, 2);
			uc_mem_read(uc, a7+2, &ret_pc_be, 4);
			uc_mem_read(uc, a7+6, &fv_be, 2);

			fprintf(stderr, "[RTE FAIL] Stack: SR=0x%04X PC=0x%08X FV=0x%04X\n",
				__builtin_bswap16(sr_be), __builtin_bswap32(ret_pc_be), __builtin_bswap16(fv_be));
		}

		return 3;  // CPU_EXEC_EXCEPTION
	}

	cpu_trace_increment();

	// Unicorn doesn't track STOP state separately
	return 0;  // CPU_EXEC_OK
}

static void unicorn_backend_execute_fast(void) {
	// Unicorn doesn't have fast path
}

// State Query
static bool unicorn_backend_is_stopped(void) {
	// Unicorn doesn't track STOP state
	return false;
}

static uint32_t unicorn_backend_get_pc(void) {
	if (!unicorn_cpu) return 0;
	return unicorn_get_pc(unicorn_cpu);
}

static uint16_t unicorn_backend_get_sr(void) {
	if (!unicorn_cpu) return 0;
	return unicorn_get_sr(unicorn_cpu);
}

static uint32_t unicorn_backend_get_dreg(int n) {
	if (!unicorn_cpu) return 0;
	return unicorn_get_dreg(unicorn_cpu, n);
}

static uint32_t unicorn_backend_get_areg(int n) {
	if (!unicorn_cpu) return 0;
	return unicorn_get_areg(unicorn_cpu, n);
}

// State Modification
static void unicorn_backend_set_pc(uint32_t pc) {
	if (!unicorn_cpu) return;
	unicorn_set_pc(unicorn_cpu, pc);
}

static void unicorn_backend_set_sr(uint16_t sr) {
	if (!unicorn_cpu) return;
	unicorn_set_sr(unicorn_cpu, sr);
}

static void unicorn_backend_set_dreg(int n, uint32_t val) {
	if (!unicorn_cpu) return;
	unicorn_set_dreg(unicorn_cpu, n, val);
}

static void unicorn_backend_set_areg(int n, uint32_t val) {
	if (!unicorn_cpu) return;
	unicorn_set_areg(unicorn_cpu, n, val);
}

// Memory Access
static void unicorn_backend_mem_read(uint32_t addr, void *data, uint32_t size) {
	if (!unicorn_cpu) return;
	unicorn_mem_read(unicorn_cpu, addr, data, size);
}

static void unicorn_backend_mem_write(uint32_t addr, const void *data, uint32_t size) {
	if (!unicorn_cpu) return;
	unicorn_mem_write(unicorn_cpu, addr, data, size);
}

// Interrupts
static void unicorn_backend_trigger_interrupt(int level) {
	/* Trigger interrupt via internal wrapper function
	 * This sets g_pending_interrupt_level which will be checked
	 * by hook_block() on the next basic block execution.
	 *
	 * Unicorn uses manual interrupt handling:
	 * - hook_block() checks g_pending_interrupt_level
	 * - Manually builds M68K exception stack frame (SR, PC)
	 * - Sets supervisor mode, updates interrupt mask
	 * - Reads vector table and jumps to handler
	 * - RTE is handled by Unicorn's QEMU M68K implementation
	 */
	unicorn_trigger_interrupt_internal(level);
}

// 68k Trap Execution - Unicorn native implementation
// This allows ROM patches to call Mac OS traps without depending on UAE CPU backend
static void unicorn_backend_execute_68k_trap(uint16_t trap, struct M68kRegisters *r) {
	if (!unicorn_cpu) {
		fprintf(stderr, "[ERROR] unicorn_backend_execute_68k_trap: Unicorn CPU not initialized\n");
		return;
	}

	// Save current PC (we'll restore it after trap execution)
	uint32_t saved_pc = unicorn_get_pc(unicorn_cpu);
	uint32_t saved_sr = unicorn_get_sr(unicorn_cpu);

	// Set registers from input
	for (int i = 0; i < 8; i++) {
		unicorn_set_dreg(unicorn_cpu, i, r->d[i]);
	}
	for (int i = 0; i < 7; i++) {
		unicorn_set_areg(unicorn_cpu, i, r->a[i]);
	}
	unicorn_set_sr(unicorn_cpu, r->sr);

	// Push trap number and M68K_EXEC_RETURN (0x7100) on stack
	// This mimics UAE's Execute68kTrap behavior
	uint32_t sp = r->a[7];
	sp -= 2;
	g_platform.mem_write_word(sp, 0x7100);  // M68K_EXEC_RETURN (EmulOp that returns)
	sp -= 2;
	g_platform.mem_write_word(sp, trap);    // Trap number
	unicorn_set_areg(unicorn_cpu, 7, sp);

	// Set PC to stack (CPU will fetch trap number as opcode)
	unicorn_set_pc(unicorn_cpu, sp);

	// Execute until we hit M68K_EXEC_RETURN (0x7100)
	// We need to run the CPU in a loop, checking for 0x7100 EmulOp
	uc_engine *uc = (uc_engine *)unicorn_get_uc(unicorn_cpu);
	bool returned = false;
	int max_iterations = 100000;  // Safety limit
	int iterations = 0;

	while (!returned && iterations < max_iterations) {
		// Execute one basic block
		uint32_t pc = unicorn_get_pc(unicorn_cpu);
		uc_err err = uc_emu_start(uc, pc, 0xFFFFFFFF, 0, 1);  // Execute 1 instruction

		if (err != UC_ERR_OK) {
			fprintf(stderr, "[ERROR] Execute68kTrap failed at PC=0x%08X: %s\n",
			        pc, uc_strerror(err));
			break;
		}

		// Check if we hit M68K_EXEC_RETURN (0x7100)
		uint32_t current_pc = unicorn_get_pc(unicorn_cpu);
		uint16_t opcode = 0;
		uc_mem_read(uc, current_pc, &opcode, 2);
		#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		opcode = __builtin_bswap16(opcode);
		#endif

		if (opcode == 0x7100) {  // M68K_EXEC_RETURN
			returned = true;
			// Clean up stack (remove trap number and return address)
			sp = unicorn_get_areg(unicorn_cpu, 7);
			sp += 4;  // Pop 2 words
			unicorn_set_areg(unicorn_cpu, 7, sp);
		}

		iterations++;
	}

	if (!returned) {
		fprintf(stderr, "[ERROR] Execute68kTrap did not return after %d iterations\n", iterations);
	}

	// Get registers back from Unicorn
	for (int i = 0; i < 8; i++) {
		r->d[i] = unicorn_get_dreg(unicorn_cpu, i);
	}
	for (int i = 0; i < 7; i++) {
		r->a[i] = unicorn_get_areg(unicorn_cpu, i);
	}
	r->sr = unicorn_get_sr(unicorn_cpu);

	// Restore original PC and SR
	unicorn_set_pc(unicorn_cpu, saved_pc);
	unicorn_set_sr(unicorn_cpu, saved_sr);
}

// Memory access (Unicorn-specific: uses uc_mem_read/write, NOT host pointers)
// These are called from UAE's get_long/put_long functions via platform API
//
// IMPORTANT: During initialization (before unicorn_backend_init()), unicorn_cpu is NULL.
// In this phase, PatchROM() needs to patch ROM, so we fall back to DirectReadMacInt*()
// which patches ROMBaseHost directly. After unicorn_backend_init(), Unicorn copies the
// patched ROM, and all subsequent access uses uc_mem_read/write on Unicorn's internal memory.
static uint32_t unicorn_mem_read_long(uint32_t addr) {
	if (!unicorn_cpu) {
		// Before Unicorn initialization: read from host memory directly
		return DirectReadMacInt32(addr);
	}
	uint32_t value = 0;
	uc_err err = uc_mem_read((uc_engine*)unicorn_get_uc(unicorn_cpu), addr, &value, 4);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "unicorn_mem_read_long: failed to read from 0x%08X: %s\n",
		        addr, uc_strerror(err));
		return 0;
	}
	// Unicorn stores memory in big-endian (M68K native), convert to host byte order for processing
	#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap32(value);
	#else
	return value;
	#endif
}

static uint16_t unicorn_mem_read_word(uint32_t addr) {
	if (!unicorn_cpu) {
		return DirectReadMacInt16(addr);
	}
	uint16_t value = 0;
	uc_err err = uc_mem_read((uc_engine*)unicorn_get_uc(unicorn_cpu), addr, &value, 2);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "unicorn_mem_read_word: failed to read from 0x%08X: %s\n",
		        addr, uc_strerror(err));
		return 0;
	}
	#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap16(value);
	#else
	return value;
	#endif
}

static uint8_t unicorn_mem_read_byte(uint32_t addr) {
	if (!unicorn_cpu) {
		return DirectReadMacInt8(addr);
	}
	uint8_t value = 0;
	uc_err err = uc_mem_read((uc_engine*)unicorn_get_uc(unicorn_cpu), addr, &value, 1);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "unicorn_mem_read_byte: failed to read from 0x%08X: %s\n",
		        addr, uc_strerror(err));
		return 0;
	}
	return value;
}

static void unicorn_mem_write_long(uint32_t addr, uint32_t value) {
	if (!unicorn_cpu) {
		// Before Unicorn initialization: write to host memory directly
		DirectWriteMacInt32(addr, value);
		return;
	}
	// Convert from host byte order to big-endian (M68K native) for Unicorn
	#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	value = __builtin_bswap32(value);
	#endif
	uc_err err = uc_mem_write((uc_engine*)unicorn_get_uc(unicorn_cpu), addr, &value, 4);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "unicorn_mem_write_long: failed to write to 0x%08X: %s\n",
		        addr, uc_strerror(err));
	}
}

static void unicorn_mem_write_word(uint32_t addr, uint16_t value) {
	if (!unicorn_cpu) {
		DirectWriteMacInt16(addr, value);
		return;
	}
	#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	value = __builtin_bswap16(value);
	#endif
	uc_err err = uc_mem_write((uc_engine*)unicorn_get_uc(unicorn_cpu), addr, &value, 2);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "unicorn_mem_write_word: failed to write to 0x%08X: %s\n",
		        addr, uc_strerror(err));
	}
}

static void unicorn_mem_write_byte(uint32_t addr, uint8_t value) {
	if (!unicorn_cpu) {
		DirectWriteMacInt8(addr, value);
		return;
	}
	uc_err err = uc_mem_write((uc_engine*)unicorn_get_uc(unicorn_cpu), addr, &value, 1);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "unicorn_mem_write_byte: failed to write to 0x%08X: %s\n",
		        addr, uc_strerror(err));
	}
}

/**
 * Install Unicorn CPU backend into platform
 */
void cpu_unicorn_install(Platform *p) {
	p->cpu_name = "Unicorn Engine";

	// Configuration
	p->cpu_set_type = unicorn_backend_set_type;

	// Lifecycle
	p->cpu_init = unicorn_backend_init;
	p->cpu_reset = unicorn_backend_reset;
	p->cpu_destroy = unicorn_backend_destroy;

	// Execution
	p->cpu_execute_one = unicorn_backend_execute_one;
	p->cpu_execute_fast = NULL;  // No fast path

	// State query
	p->cpu_is_stopped = unicorn_backend_is_stopped;
	p->cpu_get_pc = unicorn_backend_get_pc;
	p->cpu_get_sr = unicorn_backend_get_sr;
	p->cpu_get_dreg = unicorn_backend_get_dreg;
	p->cpu_get_areg = unicorn_backend_get_areg;

	// State modification
	p->cpu_set_pc = unicorn_backend_set_pc;
	p->cpu_set_sr = unicorn_backend_set_sr;
	p->cpu_set_dreg = unicorn_backend_set_dreg;
	p->cpu_set_areg = unicorn_backend_set_areg;

	// Memory access
	p->cpu_mem_read = unicorn_backend_mem_read;
	p->cpu_mem_write = unicorn_backend_mem_write;

	// Interrupts
	p->cpu_trigger_interrupt = unicorn_backend_trigger_interrupt;

	// 68k Trap execution
	p->cpu_execute_68k_trap = unicorn_backend_execute_68k_trap;

	// Memory system (Unicorn-specific: uses uc_mem_read/write to access Unicorn's internal memory)
	// IMPORTANT: Do NOT use DirectReadMacInt* functions - they read from RAMBaseHost/ROMBaseHost
	// which is UAE's memory space, not Unicorn's internal memory!
	p->mem_read_byte = unicorn_mem_read_byte;
	p->mem_read_word = unicorn_mem_read_word;
	p->mem_read_long = unicorn_mem_read_long;
	p->mem_write_byte = unicorn_mem_write_byte;
	p->mem_write_word = unicorn_mem_write_word;
	p->mem_write_long = unicorn_mem_write_long;

	// Address translation: Unicorn doesn't support direct host pointer access
	// Mac2HostAddr/Host2MacAddr are only valid for UAE's memory space
	// For Unicorn, these should NOT be used - all access must go through uc_mem_read/write
	p->mem_mac_to_host = NULL;  // Not supported for Unicorn
	p->mem_host_to_mac = NULL;  // Not supported for Unicorn
}

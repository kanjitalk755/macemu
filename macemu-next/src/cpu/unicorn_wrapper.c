/**
 * Unicorn Engine Wrapper Implementation
 *
 * ============================================================================
 * CRITICAL ENDIANNESS NOTES:
 * ============================================================================
 * UAE and Unicorn have different memory storage formats:
 *
 * UAE (68k emulator):
 *   - RAM: Stored in LITTLE-ENDIAN (x86 native) in RAMBaseHost
 *   - ROM: Stored in BIG-ENDIAN (as loaded from file) in ROMBaseHost
 *   - get_long/put_long: Byte-swap on-the-fly when accessing memory
 *
 * Unicorn (M68K mode):
 *   - RAM: Expected in BIG-ENDIAN (M68K native)
 *   - ROM: Expected in BIG-ENDIAN (M68K native)
 *   - No automatic byte-swapping
 *
 * When copying memory to Unicorn:
 *   - RAM: MUST byte-swap (LE -> BE) or Unicorn reads garbage
 *   - ROM: NO byte-swap (already BE) or instructions get corrupted
 *
 * See unicorn_map_ram() and unicorn_map_rom_writable() for implementation.
 * ============================================================================
 */

#include "unicorn_wrapper.h"
#include "platform.h"
#include "cpu_trace.h"
#include "timer_interrupt.h"
#include <unicorn/unicorn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* MMIO Trap Region for JIT-compatible EmulOp handling */
#define TRAP_REGION_BASE  0xFF000000UL
#define TRAP_REGION_SIZE  0x00001000UL  /* 4KB = 2048 EmulOp slots */

/* Interrupt handling via platform API
 * Set by unicorn_trigger_interrupt_internal() (called from platform API),
 * checked by hook_block()
 */
static volatile int g_pending_interrupt_level = 0;  /* 0=none, 1-7=interrupt level */

/* Trap context for MMIO approach */
typedef struct {
    uint32_t saved_pc;     /* Original PC where 0x71xx was */
    bool in_emulop;        /* Currently handling EmulOp? */
    bool in_trap;          /* Currently handling A-line/F-line trap? */
    uint16_t trap_opcode;  /* Original trap opcode */
} TrapContext;

/* Block statistics for timing analysis */
typedef struct {
    uint64_t total_blocks;         /* Total number of blocks executed */
    uint64_t total_instructions;   /* Total instructions executed */
    uint64_t block_size_histogram[101];  /* Histogram: [0-99] + [100+] */
    uint32_t min_block_size;       /* Smallest block seen */
    uint32_t max_block_size;       /* Largest block seen */
    uint64_t sum_block_sizes;      /* Sum for average calculation */
} BlockStats;

struct UnicornCPU {
    uc_engine *uc;
    UnicornArch arch;
    char error[256];

    /* Hooks */
    /* NOTE: Per-CPU emulop_handler and exception_handler removed - use g_platform API instead */

    MemoryHookCallback memory_hook;
    void *memory_user_data;
    uc_hook mem_hook_handle;

    /* NOTE: code_hook removed - UC_HOOK_CODE deprecated, using UC_HOOK_INSN_INVALID instead */
    uc_hook block_hook;       // UC_HOOK_BLOCK for interrupts (efficient)
    uc_hook insn_invalid_hook;  // UC_HOOK_INSN_INVALID for EmulOps (no per-instruction overhead)
    uc_hook trap_hook;  // UC_HOOK_MEM_FETCH_UNMAPPED for MMIO trap region
    uc_hook trace_hook; // UC_HOOK_MEM_READ for CPU tracing

    /* MMIO trap context */
    TrapContext trap_ctx;

    /* Block statistics */
    BlockStats block_stats;
};

/* Helper: Convert uc_err to string and store in cpu->error */
static void set_error(UnicornCPU *cpu, uc_err err) {
    if (err != UC_ERR_OK) {
        snprintf(cpu->error, sizeof(cpu->error), "%s", uc_strerror(err));
    }
}

/* MMIO Trap Handler - called when CPU fetches from unmapped trap region
 * This fires even in JIT mode, making it reliable for EmulOp handling
 */
static void trap_mem_fetch_handler(uc_engine *uc, uc_mem_type type,
                                   uint64_t address, int size,
                                   int64_t value, void *user_data) {
    UnicornCPU *cpu = (UnicornCPU *)user_data;

    /* Verify address is in trap region */
    if (address < TRAP_REGION_BASE ||
        address >= TRAP_REGION_BASE + TRAP_REGION_SIZE) {
        fprintf(stderr, "ERROR: Unexpected unmapped fetch at 0x%08lx\n", address);
        return;
    }

    if (!cpu->trap_ctx.in_emulop && !cpu->trap_ctx.in_trap) {
        fprintf(stderr, "WARNING: Trap region access without INSN_INVALID at 0x%08lx\n", address);
        return;
    }

    /* Handle EmulOp */
    if (cpu->trap_ctx.in_emulop) {
        /* Calculate EmulOp number from trap address */
        uint32_t emulop_num = (address - TRAP_REGION_BASE) / 2;
        uint16_t opcode = 0x7100 + emulop_num;

        /* Call platform EmulOp handler (same as UAE uses) */
        if (g_platform.emulop_handler) {
            bool pc_advanced = g_platform.emulop_handler(opcode, false);

            /* Restore PC to instruction AFTER the 0x71xx */
            uint32_t next_pc = cpu->trap_ctx.saved_pc + (pc_advanced ? 0 : 2);
            uc_reg_write(uc, UC_M68K_REG_PC, &next_pc);

            cpu->trap_ctx.in_emulop = false;
            return;
        }
    }

    /* Handle A-line/F-line trap */
    if (cpu->trap_ctx.in_trap) {
        uint16_t opcode = cpu->trap_ctx.trap_opcode;
        int vector = ((opcode & 0xF000) == 0xA000) ? 0xA : 0xB;

        if (g_platform.trap_handler) {
            g_platform.trap_handler(vector, opcode, false);
            /* Handler manages PC, just clear trap flag */
            cpu->trap_ctx.in_trap = false;
            return;
        }
    }

    fprintf(stderr, "ERROR: Trap handler not available for address 0x%08lx\n", address);
}

/* Invalid instruction hook for EmulOp/trap handling
 * NOTE: Requires m68k_stop_interrupt() patch in Unicorn (see external/unicorn/qemu/target/m68k/unicorn.c)
 */
/**
 * Hook for basic block execution (UC_HOOK_BLOCK)
 * Called at the start of each basic block - much more efficient than per-instruction
 * Used for interrupt checking at block boundaries
 */
static void hook_block(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    UnicornCPU *cpu = (UnicornCPU *)user_data;

    uint32_t pc = (uint32_t)address;

    /* Count instructions in this block for statistics */
    /* We estimate instruction count by decoding the block */
    /* M68K instructions are variable length (2-10 bytes), so we need to decode */
    uint32_t block_start = pc;
    uint32_t block_end = pc + size;
    uint32_t insn_count = 0;
    uint32_t current_pc = block_start;

    while (current_pc < block_end) {
        uint16_t opcode = 0;
        if (uc_mem_read(uc, current_pc, &opcode, 2) != UC_ERR_OK) {
            break;  /* Can't read opcode, stop counting */
        }
        #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        opcode = __builtin_bswap16(opcode);
        #endif

        insn_count++;

        /* Simple instruction length estimation for M68K */
        /* This is a rough approximation - actual decoder would be complex */
        /* Most instructions are 2 or 4 bytes */
        uint32_t insn_len = 2;  /* Base opcode word */

        /* Check for immediate data or address extensions */
        /* These patterns commonly have extension words */
        if ((opcode & 0xF000) == 0x0000 ||  /* ORI, ANDI, SUBI, ADDI, etc. */
            (opcode & 0xF000) == 0x2000 ||  /* MOVEA, MOVE.L */
            (opcode & 0xF000) == 0x3000 ||  /* MOVE.W */
            (opcode & 0xF000) == 0x4000 ||  /* LEA, PEA, etc. */
            (opcode & 0xF1C0) == 0x41C0 ||  /* LEA */
            (opcode & 0xF1C0) == 0x4180) {  /* CHK */
            /* May have extension words - add 2 bytes as estimate */
            insn_len += 2;
        }

        current_pc += insn_len;
        if (current_pc > block_end) {
            /* Don't count partial instructions */
            break;
        }
    }

    /* Update block statistics */
    cpu->block_stats.total_blocks++;
    cpu->block_stats.total_instructions += insn_count;
    cpu->block_stats.sum_block_sizes += insn_count;

    /* Update min/max */
    if (cpu->block_stats.min_block_size == 0 || insn_count < cpu->block_stats.min_block_size) {
        cpu->block_stats.min_block_size = insn_count;
    }
    if (insn_count > cpu->block_stats.max_block_size) {
        cpu->block_stats.max_block_size = insn_count;
    }

    /* Update histogram */
    if (insn_count < 100) {
        cpu->block_stats.block_size_histogram[insn_count]++;
    } else {
        cpu->block_stats.block_size_histogram[100]++;  /* 100+ bucket */
    }

    /* Poll timer - may trigger interrupt */
    poll_timer_interrupt();

    /* Check for pending interrupts (platform API) */
    if (g_pending_interrupt_level > 0) {
        int intr_level = g_pending_interrupt_level;
        g_pending_interrupt_level = 0;  /* Clear after reading */

        /* Get current SR to check interrupt mask */
        uint32_t sr;
        uc_reg_read(uc, UC_M68K_REG_SR, &sr);
        int current_mask = (sr >> 8) & 7;

        if (intr_level > current_mask) {
            /* Trigger M68K interrupt - manually build exception stack frame
             * This is the correct approach for Unicorn - we researched using QEMU's
             * m68k_set_irq_level() but it requires accessing internal structs with
             * architecture-dependent offsets, making it fragile and non-portable.
             * Manual stack building is clean, works correctly, and is well-tested.
             */
            uint32_t sp;
            uc_reg_read(uc, UC_M68K_REG_A7, &sp);

            /* Push PC (long, big-endian) */
            sp -= 4;
            uint32_t pc_be = __builtin_bswap32(pc);
            uc_mem_write(uc, sp, &pc_be, 4);

            /* Push SR (word, big-endian) */
            sp -= 2;
            uint16_t sr_be = __builtin_bswap16((uint16_t)sr);
            uc_mem_write(uc, sp, &sr_be, 2);

            /* Update SR: set supervisor mode, set interrupt mask */
            sr |= (1 << 13);  /* S bit */
            sr = (sr & ~0x0700) | ((intr_level & 7) << 8);  /* I2-I0 */
            uc_reg_write(uc, UC_M68K_REG_SR, &sr);
            uc_reg_write(uc, UC_M68K_REG_A7, &sp);

            /* Read interrupt vector and jump to handler */
            uint32_t vbr = 0;  /* TODO: Read VBR for 68020+ */
            uint32_t vector_addr = vbr + (24 + intr_level) * 4;
            uint32_t handler_addr_be;
            uc_mem_read(uc, vector_addr, &handler_addr_be, 4);
            uint32_t handler_addr = __builtin_bswap32(handler_addr_be);

            /* Log interrupt being taken */
            cpu_trace_log_interrupt_taken(intr_level, handler_addr);

            /* Update PC to handler address
             *
             * CRITICAL: We do NOT call uc_emu_stop() here!
             *
             * Calling uc_emu_stop() mid-block causes instruction skipping because:
             * 1. Current translation block (TB) execution is aborted mid-stream
             * 2. Some instructions may have already executed from this block
             * 3. PC might not align with instruction boundaries when resuming
             * 4. Next uc_emu_start() may resume at wrong offset within the block
             *
             * QEMU's approach (from our analysis of Quadra 800 code):
             * - Interrupts are checked BETWEEN translation blocks, not during
             * - cpu_handle_interrupt() runs after each TB completes
             * - When interrupt accepted, PC is updated and execution continues naturally
             * - No forced stop, no TB abortion, no instruction skipping
             *
             * Our solution:
             * - Update PC register to handler address (done above)
             * - Let this hook return normally
             * - Current block will complete (or Unicorn will abort it naturally)
             * - Next block fetch will use the new PC (handler address)
             * - No instruction skipping, clean transition
             *
             * Evidence: Traces showed divergence at instruction #3832 where Unicorn
             * executed PC=0x02081138 instead of correct 0x0208113A (off by 2 bytes)
             * due to uc_emu_stop() aborting mid-block.
             */
            uc_reg_write(uc, UC_M68K_REG_PC, &handler_addr);

            /* Invalidate any cached translations at the new PC to force fresh decode */
            uc_ctl_remove_cache(uc, handler_addr, handler_addr + 4);

            /* Let execution continue naturally - Unicorn will execute handler */
            return;
        }
    }
}

/**
 * Hook for invalid instructions (UC_HOOK_INSN_INVALID)
 * Called when Unicorn encounters an illegal instruction
 * Used for EmulOps (0x71xx) and traps (0xAxxx, 0xFxxx)
 * Returns true to continue execution, false to stop
 */
static bool hook_insn_invalid(uc_engine *uc, void *user_data) {
    UnicornCPU *cpu = (UnicornCPU *)user_data;

    /* Read PC (at illegal instruction) */
    uint32_t pc;
    uc_reg_read(uc, UC_M68K_REG_PC, &pc);

    /* Read opcode at PC */
    uint16_t opcode;
    uc_mem_read(uc, pc, &opcode, sizeof(opcode));
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    opcode = __builtin_bswap16(opcode);
    #endif

    /* Check if EmulOp (0x71xx for M68K) */
    if ((opcode & 0xFF00) == 0x7100) {
        if (g_platform.emulop_handler) {
            /* Call platform handler */
            bool pc_advanced = g_platform.emulop_handler(opcode, false);

            /* Sync ALL registers back from platform to Unicorn */
            if (g_platform.cpu_get_dreg && g_platform.cpu_get_areg) {
                for (int i = 0; i < 8; i++) {
                    uint32_t d = g_platform.cpu_get_dreg(i);
                    uint32_t a = g_platform.cpu_get_areg(i);
                    uc_reg_write(uc, UC_M68K_REG_D0 + i, &d);
                    uc_reg_write(uc, UC_M68K_REG_A0 + i, &a);
                }
                if (g_platform.cpu_get_sr) {
                    uint16_t sr = g_platform.cpu_get_sr();
                    uc_reg_write(uc, UC_M68K_REG_SR, &sr);
                }
            }

            /* Advance PC if handler didn't */
            if (!pc_advanced) {
                pc += 2;
            }

            /* CRITICAL: Invalidate cache and update PC to continue execution */
            uc_ctl_remove_cache(uc, pc - 2, pc + 4);
            uc_reg_write(uc, UC_M68K_REG_PC, &pc);

            /* Return true to continue execution */
            return true;
        }
        /* No platform handler - invalid EmulOp */
        fprintf(stderr, "[UNICORN] Unhandled EmulOp 0x%04X at PC=0x%08X (no platform handler)\n", opcode, pc);
        return false;
    }

    /* Check for A-line trap (0xAxxx) */
    if ((opcode & 0xF000) == 0xA000) {
        if (g_platform.trap_handler) {
            /* Platform trap handler */
            g_platform.trap_handler(0xA, opcode, false);
            /* Handler handles PC advancement - just continue */
            return true;
        }
        /* No platform handler - real A-line exception */
        fprintf(stderr, "[UNICORN] Unhandled A-line trap 0x%04X at PC=0x%08X (no platform handler)\n", opcode, pc);
        return false;
    }

    /* Check for F-line trap (0xFxxx) */
    if ((opcode & 0xF000) == 0xF000) {
        if (g_platform.trap_handler) {
            g_platform.trap_handler(0xF, opcode, false);
            return true;
        }
        /* No platform handler - real F-line exception */
        fprintf(stderr, "[UNICORN] Unhandled F-line trap 0x%04X at PC=0x%08X (no platform handler)\n", opcode, pc);
        return false;
    }

    /* Real invalid instruction - stop execution */
    fprintf(stderr, "[UNICORN] Invalid instruction 0x%04X at PC=0x%08X\n", opcode, pc);
    return false;
}

/* NOTE: Legacy hook_code() function removed (was lines 296-479)
 * UC_HOOK_INSN_INVALID (hook_insn_invalid) handles all EmulOps/traps without per-instruction
 * overhead. Platform API (g_platform) is checked automatically. No UC_HOOK_CODE needed.
 */

/* Memory access hook */
static void hook_memory(uc_engine *uc, uc_mem_type type,
                       uint64_t address, int size, int64_t value,
                       void *user_data)
{
    UnicornCPU *cpu = (UnicornCPU *)user_data;

    if (cpu->memory_hook) {
        UnicornMemType mem_type = (type == UC_MEM_READ || type == UC_MEM_READ_UNMAPPED) ?
                                  UCPU_MEM_READ : UCPU_MEM_WRITE;
        cpu->memory_hook(cpu, mem_type, address, (uint32_t)size,
                        (uint64_t)value, cpu->memory_user_data);
    }
}

/* Memory read trace hook for CPU_TRACE_MEMORY */
static void hook_mem_trace(uc_engine *uc, uc_mem_type type,
                           uint64_t address, int size, int64_t value,
                           void *user_data)
{
    /* Only trace reads */
    if (type != UC_MEM_READ) return;

    /* Read the actual value from memory */
    uint32_t val = 0;
    uc_mem_read(uc, address, &val, size);

    /* Byte-swap if needed (M68K is big-endian) */
    if (size == 2) {
        val = ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
    } else if (size == 4) {
        val = ((val & 0xFF) << 24) | ((val & 0xFF00) << 8) |
              ((val & 0xFF0000) >> 8) | ((val >> 24) & 0xFF);
    }

    cpu_trace_log_mem_read((uint32_t)address, val, size);
}

/* CPU lifecycle */
UnicornCPU* unicorn_create(UnicornArch arch) {
    return unicorn_create_with_model(arch, -1);  /* Use default CPU model */
}

UnicornCPU* unicorn_create_with_model(UnicornArch arch, int cpu_model) {
    UnicornCPU *cpu = calloc(1, sizeof(UnicornCPU));
    if (!cpu) return NULL;

    cpu->arch = arch;

    uc_arch uc_arch;
    uc_mode uc_mode;

    switch (arch) {
        case UCPU_ARCH_M68K:
            uc_arch = UC_ARCH_M68K;
            uc_mode = UC_MODE_BIG_ENDIAN;
            break;
        case UCPU_ARCH_PPC:
            uc_arch = UC_ARCH_PPC;
            uc_mode = UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN;
            break;
        case UCPU_ARCH_PPC64:
            uc_arch = UC_ARCH_PPC;
            uc_mode = UC_MODE_PPC64 | UC_MODE_BIG_ENDIAN;
            break;
        default:
            free(cpu);
            return NULL;
    }

    uc_err err = uc_open(uc_arch, uc_mode, &cpu->uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "Failed to create Unicorn CPU: %s\n", uc_strerror(err));
        free(cpu);
        return NULL;
    }

    /* Set CPU model if specified */
    if (cpu_model >= 0) {
        err = uc_ctl_set_cpu_model(cpu->uc, cpu_model);
        if (err != UC_ERR_OK) {
            fprintf(stderr, "Failed to set Unicorn CPU model: %s\n", uc_strerror(err));
            uc_close(cpu->uc);
            free(cpu);
            return NULL;
        }
    }

    /* Register MMIO trap hook for JIT-compatible EmulOp/trap handling */
    /* IMPORTANT: Don't map the trap region - leave it unmapped! */
    err = uc_hook_add(cpu->uc, &cpu->trap_hook,
                     UC_HOOK_MEM_FETCH_UNMAPPED,
                     trap_mem_fetch_handler,
                     cpu,  /* user_data */
                     TRAP_REGION_BASE,
                     TRAP_REGION_BASE + TRAP_REGION_SIZE - 1);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "Failed to register MMIO trap hook: %s\n", uc_strerror(err));
        uc_close(cpu->uc);
        free(cpu);
        return NULL;
    }

    /* Initialize trap context */
    cpu->trap_ctx.saved_pc = 0;
    cpu->trap_ctx.in_emulop = false;
    cpu->trap_ctx.in_trap = false;
    cpu->trap_ctx.trap_opcode = 0;

    /* Initialize block statistics */
    memset(&cpu->block_stats, 0, sizeof(BlockStats));

    /* Install memory trace hook if CPU_TRACE_MEMORY is enabled */
    if (cpu_trace_memory_enabled()) {
        err = uc_hook_add(cpu->uc, &cpu->trace_hook,
                         UC_HOOK_MEM_READ,
                         hook_mem_trace,
                         cpu,  /* user_data */
                         1, 0);  /* All addresses */
        if (err != UC_ERR_OK) {
            fprintf(stderr, "Warning: Failed to register memory trace hook: %s\n", uc_strerror(err));
            /* Not fatal - continue without memory tracing */
        }
    }

    /* Register UC_HOOK_BLOCK for efficient interrupt checking */
    fprintf(stderr, "[UNICORN] Registering UC_HOOK_BLOCK for interrupt handling\n");
    err = uc_hook_add(cpu->uc, &cpu->block_hook,
                     UC_HOOK_BLOCK,
                     (void*)hook_block,
                     cpu,  /* user_data */
                     1, 0);  /* All addresses */
    if (err != UC_ERR_OK) {
        fprintf(stderr, "Failed to register UC_HOOK_BLOCK: %s\n", uc_strerror(err));
        uc_close(cpu->uc);
        free(cpu);
        return NULL;
    }

    /* Register UC_HOOK_INSN_INVALID for EmulOps/traps without per-instruction overhead */
    fprintf(stderr, "[UNICORN] Registering UC_HOOK_INSN_INVALID for EmulOp/trap handling\n");
    err = uc_hook_add(cpu->uc, &cpu->insn_invalid_hook,
                     UC_HOOK_INSN_INVALID,
                     (void*)hook_insn_invalid,
                     cpu,  /* user_data */
                     1, 0);  /* All addresses */
    if (err != UC_ERR_OK) {
        fprintf(stderr, "Failed to register UC_HOOK_INSN_INVALID: %s\n", uc_strerror(err));
        uc_close(cpu->uc);
        free(cpu);
        return NULL;
    }

    return cpu;
}

void unicorn_destroy(UnicornCPU *cpu) {
    if (!cpu) return;

    if (cpu->uc) {
        uc_close(cpu->uc);
    }
    free(cpu);
}

/* Memory mapping */
bool unicorn_map_ram(UnicornCPU *cpu, uint64_t addr, void *host_ptr, uint64_t size) {
    if (!cpu || !cpu->uc) return false;

    /* First map the memory region */
    uc_err err = uc_mem_map(cpu->uc, addr, size, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        set_error(cpu, err);
        return false;
    }

    /* ============================================================================
     * CRITICAL: Byte-swapping for RAM
     * ============================================================================
     * ENDIANNESS WARNING:
     * - UAE stores RAM in LITTLE-ENDIAN format (x86 native byte order)
     * - UAE's memory accessors (get_long/put_long) byte-swap on-the-fly
     * - Unicorn (M68K mode) expects RAM in BIG-ENDIAN format (M68K native)
     * - We MUST byte-swap RAM when copying from UAE's RAMBaseHost to Unicorn
     * - Without this, Unicorn reads garbage values and diverges immediately
     * ============================================================================
     */
    if (host_ptr) {
        // Allocate temporary buffer for byte-swapped RAM
        uint8_t *swapped_ram = (uint8_t *)malloc(size);
        if (!swapped_ram) {
            fprintf(stderr, "Failed to allocate RAM swap buffer\n");
            return false;
        }

        // Byte-swap from little-endian to big-endian (swap 32-bit values)
        const uint32_t *src32 = (const uint32_t *)host_ptr;
        uint32_t *dst32 = (uint32_t *)swapped_ram;
        for (uint64_t i = 0; i < size / 4; i++) {
            uint32_t val = src32[i];
            // Convert: 0xAABBCCDD (LE in memory) -> 0xDDCCBBAA (BE in memory)
            dst32[i] = ((val & 0xFF) << 24) | ((val & 0xFF00) << 8) |
                       ((val & 0xFF0000) >> 8) | ((val >> 24) & 0xFF);
        }

        err = uc_mem_write(cpu->uc, addr, swapped_ram, size);
        free(swapped_ram);

        if (err != UC_ERR_OK) {
            set_error(cpu, err);
            return false;
        }
    }
    return true;
}

bool unicorn_map_rom(UnicornCPU *cpu, uint64_t addr, const void *host_ptr, uint64_t size) {
    if (!cpu || !cpu->uc) return false;

    /* ROM is read+exec, no write */
    uc_err err = uc_mem_map(cpu->uc, addr, size, UC_PROT_READ | UC_PROT_EXEC);
    if (err != UC_ERR_OK) {
        set_error(cpu, err);
        return false;
    }

    /* Write ROM data */
    if (host_ptr) {
        err = uc_mem_write(cpu->uc, addr, host_ptr, size);
        if (err != UC_ERR_OK) {
            set_error(cpu, err);
            return false;
        }
    }
    return true;
}

bool unicorn_map_rom_writable(UnicornCPU *cpu, uint64_t addr, const void *host_ptr, uint64_t size) {
    if (!cpu || !cpu->uc) return false;

    /* ROM mapped as writable for validation/debugging (BasiliskII patches ROM during boot) */
    uc_err err = uc_mem_map(cpu->uc, addr, size, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        set_error(cpu, err);
        return false;
    }

    /* ============================================================================
     * CRITICAL: NO byte-swapping for ROM!
     * ============================================================================
     * ENDIANNESS WARNING:
     * - ROM is kept in BIG-ENDIAN format (as loaded from ROM file)
     * - ROM is NOT stored in little-endian like RAM
     * - UAE's memory accessors still byte-swap when reading ROM, but the
     *   underlying storage in ROMBaseHost is already big-endian
     * - Unicorn (M68K mode) expects ROM in BIG-ENDIAN format
     * - Therefore, copy ROM directly WITHOUT byte-swapping
     * - DO NOT byte-swap ROM or instructions will be corrupted!
     * ============================================================================
     */
    if (host_ptr) {
        err = uc_mem_write(cpu->uc, addr, host_ptr, size);
        if (err != UC_ERR_OK) {
            set_error(cpu, err);
            return false;
        }
    }
    return true;
}

bool unicorn_unmap(UnicornCPU *cpu, uint64_t addr, uint64_t size) {
    if (!cpu || !cpu->uc) return false;

    uc_err err = uc_mem_unmap(cpu->uc, addr, size);
    if (err != UC_ERR_OK) {
        set_error(cpu, err);
        return false;
    }
    return true;
}

/* Memory access */
bool unicorn_mem_write(UnicornCPU *cpu, uint64_t addr, const void *data, size_t size) {
    if (!cpu || !cpu->uc || !data) return false;

    uc_err err = uc_mem_write(cpu->uc, addr, data, size);
    if (err != UC_ERR_OK) {
        set_error(cpu, err);
        return false;
    }
    return true;
}

bool unicorn_mem_read(UnicornCPU *cpu, uint64_t addr, void *data, size_t size) {
    if (!cpu || !cpu->uc || !data) return false;

    uc_err err = uc_mem_read(cpu->uc, addr, data, size);
    if (err != UC_ERR_OK) {
        set_error(cpu, err);
        return false;
    }
    return true;
}

/* Execution */
bool unicorn_execute_one(UnicornCPU *cpu) {
    if (!cpu || !cpu->uc) return false;

    uint64_t pc;
    uc_reg_read(cpu->uc,
                cpu->arch == UCPU_ARCH_M68K ? UC_M68K_REG_PC : UC_PPC_REG_PC,
                &pc);

    uc_err err = uc_emu_start(cpu->uc, pc, 0xFFFFFFFFFFFFFFFFULL, 0, 1);
    if (err != UC_ERR_OK) {
        /* Check for illegal instruction (EmulOps and traps) */
        if (err == UC_ERR_INSN_INVALID && cpu->arch == UCPU_ARCH_M68K) {
            /* Read opcode at PC */
            uint16_t opcode;
            if (uc_mem_read(cpu->uc, (uint32_t)pc, &opcode, sizeof(opcode)) == UC_ERR_OK) {
                /* M68K is big-endian, swap if needed */
                #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                opcode = __builtin_bswap16(opcode);
                #endif

                static int illegal_count = 0;
                if (illegal_count < 10 || illegal_count > 3685) {
                    fprintf(stderr, "[ILLEGAL #%d] PC=0x%08X opcode=0x%04X\n",
                           illegal_count, (uint32_t)pc, opcode);
                }
                illegal_count++;

                /* MMIO Trap Approach: Redirect PC to unmapped region */

                /* Handle EmulOp (0x71xx) via MMIO trap */
                if ((opcode & 0xFF00) == 0x7100) {
                    /* Save original PC */
                    cpu->trap_ctx.saved_pc = (uint32_t)pc;
                    cpu->trap_ctx.in_emulop = true;

                    /* Calculate trap address in unmapped region */
                    uint32_t emulop_num = opcode & 0xFF;
                    uint32_t trap_addr = TRAP_REGION_BASE + (emulop_num * 2);

                    /* Redirect PC to trap region */
                    uint64_t trap_addr_64 = trap_addr;
                    uc_reg_write(cpu->uc, UC_M68K_REG_PC, &trap_addr_64);

                    /* Resume execution - will trigger UC_HOOK_MEM_FETCH_UNMAPPED */
                    err = uc_emu_start(cpu->uc, trap_addr, 0xFFFFFFFFFFFFFFFFULL, 0, 1);

                    /* Trap handler executed, check if successful */
                    return (err == UC_ERR_OK || !cpu->trap_ctx.in_emulop);
                }

                /* Handle A-line trap (0xAxxx) via MMIO trap */
                if ((opcode & 0xF000) == 0xA000) {
                    cpu->trap_ctx.saved_pc = (uint32_t)pc;
                    cpu->trap_ctx.in_trap = true;
                    cpu->trap_ctx.trap_opcode = opcode;

                    /* Use offset 0x800 in trap region for A-line traps */
                    uint32_t trap_addr = TRAP_REGION_BASE + 0x800;
                    uint64_t trap_addr_64 = trap_addr;
                    uc_reg_write(cpu->uc, UC_M68K_REG_PC, &trap_addr_64);

                    err = uc_emu_start(cpu->uc, trap_addr, 0xFFFFFFFFFFFFFFFFULL, 0, 1);
                    return (err == UC_ERR_OK || !cpu->trap_ctx.in_trap);
                }

                /* Handle F-line trap (0xFxxx) via MMIO trap */
                if ((opcode & 0xF000) == 0xF000) {
                    cpu->trap_ctx.saved_pc = (uint32_t)pc;
                    cpu->trap_ctx.in_trap = true;
                    cpu->trap_ctx.trap_opcode = opcode;

                    /* Use offset 0x900 in trap region for F-line traps */
                    uint32_t trap_addr = TRAP_REGION_BASE + 0x900;
                    uint64_t trap_addr_64 = trap_addr;
                    uc_reg_write(cpu->uc, UC_M68K_REG_PC, &trap_addr_64);

                    err = uc_emu_start(cpu->uc, trap_addr, 0xFFFFFFFFFFFFFFFFULL, 0, 1);
                    return (err == UC_ERR_OK || !cpu->trap_ctx.in_trap);
                }
            }
        }

        set_error(cpu, err);
        return false;
    }
    return true;
}

bool unicorn_execute_n(UnicornCPU *cpu, uint64_t count) {
    if (!cpu || !cpu->uc) return false;

    uint64_t pc;
    uc_reg_read(cpu->uc,
                cpu->arch == UCPU_ARCH_M68K ? UC_M68K_REG_PC : UC_PPC_REG_PC,
                &pc);

    uc_err err = uc_emu_start(cpu->uc, pc, 0xFFFFFFFFFFFFFFFFULL, 0, count);
    if (err != UC_ERR_OK) {
        set_error(cpu, err);
        return false;
    }
    return true;
}

bool unicorn_execute_until(UnicornCPU *cpu, uint64_t end_addr) {
    if (!cpu || !cpu->uc) return false;

    uint64_t pc;
    uc_reg_read(cpu->uc,
                cpu->arch == UCPU_ARCH_M68K ? UC_M68K_REG_PC : UC_PPC_REG_PC,
                &pc);

    uc_err err = uc_emu_start(cpu->uc, pc, end_addr, 0, 0);
    if (err != UC_ERR_OK) {
        set_error(cpu, err);
        return false;
    }
    return true;
}

void unicorn_stop(UnicornCPU *cpu) {
    if (cpu && cpu->uc) {
        uc_emu_stop(cpu->uc);
    }
}

/* Registers - M68K */
uint32_t unicorn_get_dreg(UnicornCPU *cpu, int reg) {
    if (!cpu || !cpu->uc || reg < 0 || reg > 7) return 0;
    uint32_t value;
    uc_reg_read(cpu->uc, UC_M68K_REG_D0 + reg, &value);
    return value;
}

uint32_t unicorn_get_areg(UnicornCPU *cpu, int reg) {
    if (!cpu || !cpu->uc || reg < 0 || reg > 7) return 0;
    uint32_t value;
    uc_reg_read(cpu->uc, UC_M68K_REG_A0 + reg, &value);
    return value;
}

void unicorn_set_dreg(UnicornCPU *cpu, int reg, uint32_t value) {
    if (!cpu || !cpu->uc || reg < 0 || reg > 7) return;
    uc_reg_write(cpu->uc, UC_M68K_REG_D0 + reg, &value);
}

void unicorn_set_areg(UnicornCPU *cpu, int reg, uint32_t value) {
    if (!cpu || !cpu->uc || reg < 0 || reg > 7) return;
    uc_reg_write(cpu->uc, UC_M68K_REG_A0 + reg, &value);
}

uint32_t unicorn_get_pc(UnicornCPU *cpu) {
    if (!cpu || !cpu->uc) return 0;
    uint32_t value;
    uc_reg_read(cpu->uc, UC_M68K_REG_PC, &value);
    return value;
}

void unicorn_set_pc(UnicornCPU *cpu, uint32_t value) {
    if (!cpu || !cpu->uc) return;
    uc_reg_write(cpu->uc, UC_M68K_REG_PC, &value);
}

uint16_t unicorn_get_sr(UnicornCPU *cpu) {
    if (!cpu || !cpu->uc) return 0;
    uint32_t value;
    uc_reg_read(cpu->uc, UC_M68K_REG_SR, &value);
    return (uint16_t)value;
}

void unicorn_set_sr(UnicornCPU *cpu, uint16_t value) {
    if (!cpu || !cpu->uc) return;
    uint32_t v = value;
    uc_reg_write(cpu->uc, UC_M68K_REG_SR, &v);
}

/* Registers - M68K control registers */
uint32_t unicorn_get_cacr(UnicornCPU *cpu) {
    if (!cpu || !cpu->uc) return 0;
    uint32_t value;
    uc_reg_read(cpu->uc, UC_M68K_REG_CR_CACR, &value);
    return value;
}

void unicorn_set_cacr(UnicornCPU *cpu, uint32_t value) {
    if (!cpu || !cpu->uc) return;
    uc_reg_write(cpu->uc, UC_M68K_REG_CR_CACR, &value);
}

uint32_t unicorn_get_vbr(UnicornCPU *cpu) {
    if (!cpu || !cpu->uc) return 0;
    uint32_t value;
    uc_reg_read(cpu->uc, UC_M68K_REG_CR_VBR, &value);
    return value;
}

void unicorn_set_vbr(UnicornCPU *cpu, uint32_t value) {
    if (!cpu || !cpu->uc) return;
    uc_reg_write(cpu->uc, UC_M68K_REG_CR_VBR, &value);
}

/* Registers - PPC */
uint32_t unicorn_get_gpr(UnicornCPU *cpu, int reg) {
    if (!cpu || !cpu->uc || reg < 0 || reg > 31) return 0;
    uint32_t value;
    uc_reg_read(cpu->uc, UC_PPC_REG_0 + reg, &value);
    return value;
}

void unicorn_set_gpr(UnicornCPU *cpu, int reg, uint32_t value) {
    if (!cpu || !cpu->uc || reg < 0 || reg > 31) return;
    uc_reg_write(cpu->uc, UC_PPC_REG_0 + reg, &value);
}

uint32_t unicorn_get_spr(UnicornCPU *cpu, int spr) {
    /* TODO: Implement SPR access based on SPR number */
    return 0;
}

void unicorn_set_spr(UnicornCPU *cpu, int spr, uint32_t value) {
    /* TODO: Implement SPR access based on SPR number */
}

uint32_t unicorn_get_msr(UnicornCPU *cpu) {
    if (!cpu || !cpu->uc) return 0;
    uint32_t value;
    uc_reg_read(cpu->uc, UC_PPC_REG_MSR, &value);
    return value;
}

void unicorn_set_msr(UnicornCPU *cpu, uint32_t value) {
    if (!cpu || !cpu->uc) return;
    uc_reg_write(cpu->uc, UC_PPC_REG_MSR, &value);
}

/* Hooks */

/* NOTE: Legacy per-CPU hook registration functions removed:
 * - unicorn_set_emulop_handler() - EmulOps handled by UC_HOOK_INSN_INVALID + g_platform.emulop_handler
 * - unicorn_set_exception_handler() - Exceptions handled by UC_HOOK_INSN_INVALID + g_platform.trap_handler
 *
 * All EmulOps and traps are now handled via platform API (g_platform) which is checked by
 * hook_insn_invalid() automatically. No per-CPU handlers or UC_HOOK_CODE registration needed.
 */

void unicorn_set_memory_hook(UnicornCPU *cpu, MemoryHookCallback callback, void *user_data) {
    if (!cpu || !cpu->uc) return;

    cpu->memory_hook = callback;
    cpu->memory_user_data = user_data;

    if (callback) {
        uc_hook_add(cpu->uc, &cpu->mem_hook_handle,
                   UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
                   (void *)hook_memory, cpu, 1, 0);
    } else if (cpu->mem_hook_handle) {
        uc_hook_del(cpu->uc, cpu->mem_hook_handle);
        cpu->mem_hook_handle = 0;
    }
}

/* Internal access (for exception handler) */
void* unicorn_get_uc(UnicornCPU *cpu) {
    return cpu ? cpu->uc : NULL;
}

/* Error handling */
const char* unicorn_get_error(UnicornCPU *cpu) {
    return cpu ? cpu->error : "Invalid CPU handle";
}

/* Block statistics */
void unicorn_print_block_stats(UnicornCPU *cpu) {
    if (!cpu) return;

    BlockStats *stats = &cpu->block_stats;

    fprintf(stderr, "\n=== Unicorn Block Execution Statistics ===\n");
    fprintf(stderr, "Total blocks executed:      %lu\n", stats->total_blocks);
    fprintf(stderr, "Total instructions:         %lu\n", stats->total_instructions);

    if (stats->total_blocks > 0) {
        double avg = (double)stats->sum_block_sizes / (double)stats->total_blocks;
        fprintf(stderr, "Average block size:         %.2f instructions\n", avg);
        fprintf(stderr, "Min block size:             %u instructions\n", stats->min_block_size);
        fprintf(stderr, "Max block size:             %u instructions\n", stats->max_block_size);

        /* Calculate median */
        uint64_t median_target = stats->total_blocks / 2;
        uint64_t cumulative = 0;
        uint32_t median = 0;
        for (int i = 0; i <= 100; i++) {
            cumulative += stats->block_size_histogram[i];
            if (cumulative >= median_target) {
                median = i;
                break;
            }
        }
        fprintf(stderr, "Median block size:          ~%u instructions\n", median);

        fprintf(stderr, "\nBlock Size Distribution:\n");
        fprintf(stderr, "Size (insns) | Count        | Percentage | Cumulative\n");
        fprintf(stderr, "-------------|--------------|------------|------------\n");

        cumulative = 0;
        for (int i = 0; i <= 100; i++) {
            if (stats->block_size_histogram[i] > 0) {
                cumulative += stats->block_size_histogram[i];
                double pct = 100.0 * stats->block_size_histogram[i] / stats->total_blocks;
                double cum_pct = 100.0 * cumulative / stats->total_blocks;

                if (i == 100) {
                    fprintf(stderr, "100+         | %12lu | %9.2f%% | %9.2f%%\n",
                            stats->block_size_histogram[i], pct, cum_pct);
                } else {
                    fprintf(stderr, "%-12d | %12lu | %9.2f%% | %9.2f%%\n",
                            i, stats->block_size_histogram[i], pct, cum_pct);
                }
            }
        }

        /* Timing impact analysis */
        fprintf(stderr, "\n=== Interrupt Timing Impact Analysis ===\n");
        fprintf(stderr, "Since interrupts are checked at block boundaries:\n");
        fprintf(stderr, "- Average interrupt latency: %.2f instructions\n", avg);
        fprintf(stderr, "- Maximum interrupt latency: %u instructions\n", stats->max_block_size);
        fprintf(stderr, "- Blocks with size > 10:     ");
        uint64_t large_blocks = 0;
        for (int i = 11; i <= 100; i++) {
            large_blocks += stats->block_size_histogram[i];
        }
        fprintf(stderr, "%lu (%.2f%%)\n", large_blocks,
                100.0 * large_blocks / stats->total_blocks);
    }

    fprintf(stderr, "==========================================\n\n");
}

void unicorn_reset_block_stats(UnicornCPU *cpu) {
    if (!cpu) return;
    memset(&cpu->block_stats, 0, sizeof(BlockStats));
}

/**
 * Trigger interrupt via platform API
 * Sets g_pending_interrupt_level which will be checked by hook_block()
 */
void unicorn_trigger_interrupt_internal(int level) {
    if (level >= 1 && level <= 7) {
        g_pending_interrupt_level = level;
        cpu_trace_log_interrupt_trigger(level);
    } else if (level == 0) {
        g_pending_interrupt_level = 0;  /* Clear interrupt */
    }
}

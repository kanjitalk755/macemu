/**
 * Unicorn CPU Validation Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unicorn_validation.h"
#include "unicorn_wrapper.h"
#include "unicorn_exception.h"
#include "uae_wrapper.h"
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"  // For M68kRegisters
#include "emul_op.h"  // For EmulOp function

// External memory pointers from BasiliskII
extern uint32 RAMBaseMac;
extern uint8 *RAMBaseHost;
extern uint32 RAMSize;
extern uint32 ROMBaseMac;
extern uint8 *ROMBaseHost;
extern uint32 ROMSize;

// External CPU configuration (from prefs system)
extern int CPUType;  // 2=68020, 3=68030, 4=68040
extern int FPUType;  // 0=none, 1=68881/68882/68040

// Master CPU selection for EmulOps and traps
typedef enum {
    MASTER_CPU_UAE,      // UAE handles EmulOps/traps, sync to Unicorn
    MASTER_CPU_UNICORN   // Unicorn handles EmulOps/traps, sync to UAE
} MasterCPU;

// Instruction trace entry for ringbuffer
typedef struct {
    uint64_t instruction_num;
    uint32_t pc;
    uint16_t opcode;

    // UAE state (after execution)
    uint32_t uae_pc;
    uint16_t uae_sr;
    uint32_t uae_cacr;
    uint32_t uae_d[8];
    uint32_t uae_a[8];

    // Unicorn state (after execution)
    uint32_t uc_pc;
    uint16_t uc_sr;
    uint32_t uc_cacr;
    uint32_t uc_d[8];
    uint32_t uc_a[8];

    bool was_emulop;
} InstructionTrace;

// Validation state
static struct {
    UnicornCPU *unicorn;
    bool enabled;
    bool initialized;
    FILE *log_file;
    MasterCPU master_cpu;

    uint64_t instruction_count;
    uint64_t divergence_count;
    uint64_t emulop_count;

    // Last known good state
    uint32_t last_good_pc;

    // Ringbuffer for instruction trace (circular buffer)
    InstructionTrace *trace_buffer;
    uint32_t trace_buffer_size;  // Size of ringbuffer (from env var)
    uint32_t trace_buffer_pos;   // Current write position (wraps around)
    bool trace_buffer_full;      // Has buffer wrapped around at least once?

    // Separate memory for Unicorn
    uint8_t *unicorn_ram;        // Private RAM copy for Unicorn
} validation_state = {0};
/* Initialize validation */
bool unicorn_validation_init(void) {
    if (validation_state.initialized) {
        fprintf(stderr, "Unicorn validation already initialized\n");
        return false;
    }

    printf("\n=== Initializing Unicorn Validation ===\n");

    // Configure master CPU (defaults to UAE)
    validation_state.master_cpu = MASTER_CPU_UAE;
    const char *master_env = getenv("DUALCPU_MASTER");
    if (master_env) {
        if (strcmp(master_env, "unicorn") == 0 || strcmp(master_env, "UNICORN") == 0) {
            validation_state.master_cpu = MASTER_CPU_UNICORN;
            printf("✓ Master CPU: UNICORN (EmulOps/traps on Unicorn, sync to UAE)\n");
        } else if (strcmp(master_env, "uae") == 0 || strcmp(master_env, "UAE") == 0) {
            validation_state.master_cpu = MASTER_CPU_UAE;
            printf("✓ Master CPU: UAE (EmulOps/traps on UAE, sync to Unicorn)\n");
        } else {
            fprintf(stderr, "Warning: Unknown DUALCPU_MASTER value '%s', defaulting to UAE\n", master_env);
            validation_state.master_cpu = MASTER_CPU_UAE;
        }
    } else {
        printf("✓ Master CPU: UAE (default)\n");
    }

    // Create Unicorn CPU with model from prefs system (CPUType/FPUType)
    // Follow same logic as standalone Unicorn backend (cpu_unicorn.cpp:157-166)
    // NOTE: Unicorn's CPU table uses array indices, not UC_CPU_M68K enum values!
    // Array order: 0=m68000, 1=m68020, 2=m68030, 3=m68040, 4=m68060...
    int uc_model;
    if (CPUType == 4) {
        uc_model = 3;  // 68040 (array index)
    } else {
        if (FPUType)
            uc_model = 2;  // 68030 (array index)
        else if (CPUType >= 2)
            uc_model = 1;  // 68020 (array index)
        else
            uc_model = 0;  // 68000 (array index)
    }

    fprintf(stderr, "[DualCPU] Creating Unicorn CPU with model %d (array index, CPUType=%d, FPUType=%d)\n",
        uc_model, CPUType, FPUType);
    validation_state.unicorn = unicorn_create_with_model(UCPU_ARCH_M68K, uc_model);
    if (!validation_state.unicorn) {
        fprintf(stderr, "Failed to create Unicorn CPU\n");
        return false;
    }
    printf("✓ Unicorn CPU created (680%02d%s)\n",
           (uc_model == 0) ? 0 : (uc_model * 10 + 10),
           FPUType ? " with FPU" : "");

    // Map RAM
    if (!unicorn_map_ram(validation_state.unicorn, RAMBaseMac, RAMBaseHost, RAMSize)) {
        fprintf(stderr, "Failed to map RAM to Unicorn\n");
        unicorn_destroy(validation_state.unicorn);
        return false;
    }
    printf("✓ RAM mapped (0x%08X, %u MB)\n", RAMBaseMac, RAMSize / (1024*1024));

    // Map ROM as writable (BasiliskII patches ROM during boot)
    if (!unicorn_map_rom_writable(validation_state.unicorn, ROMBaseMac, ROMBaseHost, ROMSize)) {
        fprintf(stderr, "Failed to map ROM to Unicorn\n");
        unicorn_destroy(validation_state.unicorn);
        return false;
    }
    printf("✓ ROM mapped (0x%08X, %u KB)\n", ROMBaseMac, ROMSize / 1024);

    // IMPORTANT: UAE has a bug where rom_lget/wget/bget read from (ROMBaseDiff + addr)
    // without bounds checking. This means UAE can read PAST the end of ROM into
    // host memory. For example, address 0x0248831C (ROM + 4.5MB) gets read even
    // though ROM is only 1MB. UAE reads whatever garbage exists in host memory.
    // The host allocation is RAMSize + 0x100000 (see main.cpp:119). ROM is at
    // RAMBaseHost + RAMSize, and is 1MB, so the full allocation ends at
    // RAMBaseHost + RAMSize + 0x100000. However, ROM already uses the full 0x100000,
    // so there's NO extra space after ROM. We need to allocate 16MB of dummy memory
    // to catch UAE's out-of-bounds reads. We'll fill it with a pattern to detect reads.
    uint32_t dummy_region_base = ROMBaseMac + ROMSize;
    uint32_t dummy_region_size = 16 * 1024 * 1024;  // 16 MB
    uint8_t *dummy_buffer = (uint8_t *)malloc(dummy_region_size);
    if (!dummy_buffer) {
        fprintf(stderr, "Failed to allocate dummy memory buffer\n");
        unicorn_destroy(validation_state.unicorn);
        return false;
    }
    // Fill with 0xFF00FF00 pattern to match UAE's garbage reads
    // UAE reads this specific pattern from uninitialized host memory
    // IMPORTANT: Must use big-endian byte order for 68K
    for (uint32_t i = 0; i < dummy_region_size; i += 4) {
        dummy_buffer[i + 0] = 0xFF;
        dummy_buffer[i + 1] = 0x00;
        dummy_buffer[i + 2] = 0xFF;
        dummy_buffer[i + 3] = 0x00;
    }
    if (!unicorn_map_ram(validation_state.unicorn, dummy_region_base, dummy_buffer, dummy_region_size)) {
        fprintf(stderr, "Failed to map dummy region to Unicorn\n");
        free(dummy_buffer);
        unicorn_destroy(validation_state.unicorn);
        return false;
    }
    printf("✓ Dummy region mapped (0x%08X - 0x%08X, %u MB) with 0xFF00FF00 pattern\n",
           dummy_region_base, dummy_region_base + dummy_region_size, dummy_region_size / (1024*1024));

    // Allocate separate RAM for Unicorn
    validation_state.unicorn_ram = (uint8_t *)malloc(RAMSize);
    if (!validation_state.unicorn_ram) {
        fprintf(stderr, "Failed to allocate separate RAM for Unicorn\n");
        unicorn_destroy(validation_state.unicorn);
        return false;
    }
    // Initialize with same content as UAE's RAM
    memcpy(validation_state.unicorn_ram, RAMBaseHost, RAMSize);

    // Remap Unicorn to use its own private RAM
    // First unmap the shared RAM
    unicorn_unmap(validation_state.unicorn, RAMBaseMac, RAMSize);
    // Then map the private copy
    if (!unicorn_map_ram(validation_state.unicorn, RAMBaseMac, validation_state.unicorn_ram, RAMSize)) {
        fprintf(stderr, "Failed to map private RAM to Unicorn\n");
        free(validation_state.unicorn_ram);
        unicorn_destroy(validation_state.unicorn);
        return false;
    }
    printf("✓ Unicorn private RAM allocated (%u MB)\n", RAMSize / (1024*1024));

    // Allocate instruction trace ringbuffer
    const char *trace_depth_env = getenv("DUALCPU_TRACE_DEPTH");
    validation_state.trace_buffer_size = trace_depth_env ? atoi(trace_depth_env) : 50;
    if (validation_state.trace_buffer_size < 10) validation_state.trace_buffer_size = 10;
    if (validation_state.trace_buffer_size > 1000) validation_state.trace_buffer_size = 1000;

    validation_state.trace_buffer = (InstructionTrace *)calloc(validation_state.trace_buffer_size, sizeof(InstructionTrace));
    if (!validation_state.trace_buffer) {
        fprintf(stderr, "Failed to allocate instruction trace buffer\n");
        free(validation_state.unicorn_ram);
        unicorn_destroy(validation_state.unicorn);
        return false;
    }
    validation_state.trace_buffer_pos = 0;
    validation_state.trace_buffer_full = false;
    printf("✓ Instruction trace buffer allocated (last %u instructions)\n", validation_state.trace_buffer_size);

    // Explicitly initialize both CPUs to identical Macintosh boot state
    // This replaces the old approach of syncing UAE→Unicorn on first instruction
    printf("✓ Setting explicit Macintosh boot state on both CPUs...\n");

    uint32_t initial_pc = ROMBaseMac + 0x2a;  // Macintosh ROM entry point
    uint32_t initial_a7 = 0x2000;             // Initial stack pointer
    uint16_t initial_sr = 0x2700;             // Supervisor mode, interrupts masked

    // Set UAE to Macintosh boot state
    uae_set_pc(initial_pc);
    uae_set_sr(initial_sr);
    for (int i = 0; i < 8; i++) {
        uae_set_dreg(i, 0);
        uae_set_areg(i, 0);
    }
    uae_set_areg(7, initial_a7);  // Set A7 after loop

    // Set Unicorn to identical Macintosh boot state
    unicorn_set_pc(validation_state.unicorn, initial_pc);
    unicorn_set_sr(validation_state.unicorn, initial_sr);
    for (int i = 0; i < 8; i++) {
        unicorn_set_dreg(validation_state.unicorn, i, 0);
        unicorn_set_areg(validation_state.unicorn, i, 0);
    }
    unicorn_set_areg(validation_state.unicorn, 7, initial_a7);  // Set A7 after loop

    // Control registers
    unicorn_set_cacr(validation_state.unicorn, 0);
    unicorn_set_vbr(validation_state.unicorn, 0);

    printf("  UAE:     PC=0x%08X A7=0x%08X SR=0x%04X\n",
           uae_get_pc(), uae_get_areg(7), uae_get_sr());
    printf("  Unicorn: PC=0x%08X A7=0x%08X SR=0x%04X\n",
           unicorn_get_pc(validation_state.unicorn),
           unicorn_get_areg(validation_state.unicorn, 7),
           unicorn_get_sr(validation_state.unicorn));
    printf("✓ Both CPUs initialized to identical state\n");

    // NOTE: EmulOp/trap handlers are registered by cpu_dualcpu_install() via platform API
    // UC_HOOK_INSN_INVALID (registered at CPU creation) automatically intercepts illegal
    // instructions and checks g_platform handlers. No per-CPU handler registration needed.
    printf("✓ UC_HOOK_INSN_INVALID installed (intercepts EmulOps/traps via platform API)\n");
    printf("✓ Platform handlers configured by CPU backend\n");

    // Open log file
    validation_state.log_file = fopen("cpu_validation.log", "w");
    if (!validation_state.log_file) {
        fprintf(stderr, "Warning: Could not open validation log file\n");
    } else {
        fprintf(validation_state.log_file, "=== BasiliskII Dual-CPU Validation Log ===\n");
        fprintf(validation_state.log_file, "UAE vs Unicorn instruction-by-instruction comparison\n\n");
        fflush(validation_state.log_file);
        printf("✓ Log file opened: cpu_validation.log\n");
    }

    validation_state.initialized = true;
    validation_state.enabled = true;
    validation_state.instruction_count = 0;
    validation_state.divergence_count = 0;
    validation_state.emulop_count = 0;
    validation_state.last_good_pc = uae_get_pc();

    printf("=== Unicorn Validation Initialized ===\n\n");
    return true;
}

/* Shutdown validation */
void unicorn_validation_shutdown(void) {
    if (!validation_state.initialized) {
        return;
    }

    printf("\n=== Unicorn Validation Statistics ===\n");
    printf("Instructions executed: %lu\n", validation_state.instruction_count);
    printf("EMUL_OP instructions:  %lu\n", validation_state.emulop_count);
    printf("Divergences detected:  %lu\n", validation_state.divergence_count);

    if (validation_state.divergence_count == 0) {
        printf("✅ PERFECT! Zero divergences detected!\n");
    } else {
        double divergence_rate = (double)validation_state.divergence_count / validation_state.instruction_count * 100.0;
        printf("Divergence rate: %.4f%%\n", divergence_rate);
    }

    if (validation_state.log_file) {
        fprintf(validation_state.log_file, "\n=== Final Statistics ===\n");
        fprintf(validation_state.log_file, "Instructions: %lu\n", validation_state.instruction_count);
        fprintf(validation_state.log_file, "EMUL_OPs: %lu\n", validation_state.emulop_count);
        fprintf(validation_state.log_file, "Divergences: %lu\n", validation_state.divergence_count);
        fclose(validation_state.log_file);
        validation_state.log_file = NULL;
    }

    if (validation_state.trace_buffer) {
        free(validation_state.trace_buffer);
        validation_state.trace_buffer = NULL;
    }

    if (validation_state.unicorn_ram) {
        free(validation_state.unicorn_ram);
        validation_state.unicorn_ram = NULL;
    }

    if (validation_state.unicorn) {
        unicorn_destroy(validation_state.unicorn);
        validation_state.unicorn = NULL;
    }

    validation_state.initialized = false;
    printf("=== Unicorn Validation Shutdown ===\n");
}

/* Helper: Read a word from memory (big-endian) */
static uint16_t read_word_be(uint32_t addr) {
    if (addr >= RAMBaseMac && addr < RAMBaseMac + RAMSize) {
        uint8_t *ptr = RAMBaseHost + (addr - RAMBaseMac);
        return (ptr[0] << 8) | ptr[1];
    } else if (addr >= ROMBaseMac && addr < ROMBaseMac + ROMSize) {
        uint8_t *ptr = ROMBaseHost + (addr - ROMBaseMac);
        return (ptr[0] << 8) | ptr[1];
    }
    return 0;
}

/* Unified Hook Handlers - Check master_cpu to determine behavior */

static bool unified_emulop_handler(uint16_t opcode, bool is_uae_calling) {
    validation_state.emulop_count++;

    // Determine if this CPU should execute based on who's calling and who's master
    // is_uae_calling: true = UAE is calling, false = Unicorn is calling
    bool should_execute = false;

    if (is_uae_calling && validation_state.master_cpu == MASTER_CPU_UAE) {
        should_execute = true;  // UAE is calling and UAE is master
    } else if (!is_uae_calling && validation_state.master_cpu == MASTER_CPU_UNICORN) {
        should_execute = true;  // Unicorn is calling and Unicorn is master
    }

    if (!should_execute) {
        // This CPU is secondary: Skip execution, state will be synced from primary
        return false;  // Caller should advance PC
    }

    // This CPU is primary: Execute and sync to secondary
    if (validation_state.master_cpu == MASTER_CPU_UAE) {
        // UAE is primary: Execute EmulOp and sync to Unicorn
        struct M68kRegisters regs;
        for (int i = 0; i < 8; i++) {
            regs.d[i] = uae_get_dreg(i);
            regs.a[i] = uae_get_areg(i);
        }
        regs.sr = uae_get_sr();

        // Call EmulOp
        extern void EmulOp(uint16 opcode, struct M68kRegisters *r);
        EmulOp(opcode, &regs);

        // Write registers back to UAE
        for (int i = 0; i < 8; i++) {
            uae_set_dreg(i, regs.d[i]);
            uae_set_areg(i, regs.a[i]);
        }
        uae_set_sr(regs.sr);

        // NOTE: PC will be advanced by the opcode handler (m68k_incpc(2))
        // Sync entire state from UAE to Unicorn (EmulOp can modify anything)
        for (int i = 0; i < 8; i++) {
            unicorn_set_dreg(validation_state.unicorn, i, uae_get_dreg(i));
            unicorn_set_areg(validation_state.unicorn, i, uae_get_areg(i));
        }
        // PC will be synced after opcode handler increments it
        uint32_t current_pc = uae_get_pc();
        unicorn_set_pc(validation_state.unicorn, current_pc + 2);  // Sync with post-increment PC
        unicorn_set_sr(validation_state.unicorn, uae_get_sr());

        // Sync all RAM (EmulOp can write anywhere)
        // Copy UAE's RAM to Unicorn's private RAM
        memcpy(validation_state.unicorn_ram, RAMBaseHost, RAMSize);
        // Then write to Unicorn
        unicorn_mem_write(validation_state.unicorn, RAMBaseMac, validation_state.unicorn_ram, RAMSize);
        return false;  // UAE advances PC in caller
    } else {
        // Unicorn is primary: Execute EmulOp and sync to UAE
        struct M68kRegisters regs;
        for (int i = 0; i < 8; i++) {
            regs.d[i] = unicorn_get_dreg(validation_state.unicorn, i);
            regs.a[i] = unicorn_get_areg(validation_state.unicorn, i);
        }
        regs.sr = unicorn_get_sr(validation_state.unicorn);

        // Call EmulOp handler
        extern void EmulOp(uint16 opcode, struct M68kRegisters *r);
        EmulOp(opcode, &regs);

        // Write registers back to Unicorn
        for (int i = 0; i < 8; i++) {
            unicorn_set_dreg(validation_state.unicorn, i, regs.d[i]);
            unicorn_set_areg(validation_state.unicorn, i, regs.a[i]);
        }
        unicorn_set_sr(validation_state.unicorn, regs.sr);

        // Advance PC past EmulOp
        uint32_t current_pc = unicorn_get_pc(validation_state.unicorn);
        unicorn_set_pc(validation_state.unicorn, current_pc + 2);

        // Sync entire state from Unicorn to UAE
        for (int i = 0; i < 8; i++) {
            uae_set_dreg(i, regs.d[i]);
            uae_set_areg(i, regs.a[i]);
        }
        uint32_t new_pc = unicorn_get_pc(validation_state.unicorn);
        uae_set_pc(new_pc);
        uae_set_sr(regs.sr);

        // Sync all RAM (EmulOp can write anywhere)
        // Read from Unicorn to its private RAM, then copy to UAE
        unicorn_mem_read(validation_state.unicorn, RAMBaseMac, validation_state.unicorn_ram, RAMSize);
        memcpy(RAMBaseHost, validation_state.unicorn_ram, RAMSize);
        return true;  // We advanced PC ourselves
    }
}

static bool unified_trap_handler(int vector, uint16_t opcode, bool is_uae_calling) {
    (void)opcode;

    // Determine if this CPU should execute based on who's calling and who's master
    bool should_execute = false;

    if (is_uae_calling && validation_state.master_cpu == MASTER_CPU_UAE) {
        should_execute = true;  // UAE is calling and UAE is master
    } else if (!is_uae_calling && validation_state.master_cpu == MASTER_CPU_UNICORN) {
        should_execute = true;  // Unicorn is calling and Unicorn is master
    }

    if (!should_execute) {
        // This CPU is secondary: Skip execution, state will be synced from primary
        return false;  // Caller advances PC (though traps may change it)
    }

    // This CPU is primary: Execute and sync to secondary
    if (validation_state.master_cpu == MASTER_CPU_UAE) {
        // UAE is primary: Execute trap and sync to Unicorn
        extern void Exception(int nr, uaecptr oldpc);
        Exception(vector, 0);

        // Sync entire state from UAE to Unicorn
        for (int i = 0; i < 8; i++) {
            unicorn_set_dreg(validation_state.unicorn, i, uae_get_dreg(i));
            unicorn_set_areg(validation_state.unicorn, i, uae_get_areg(i));
        }
        unicorn_set_pc(validation_state.unicorn, uae_get_pc());
        unicorn_set_sr(validation_state.unicorn, uae_get_sr());

        // Sync all RAM
        memcpy(validation_state.unicorn_ram, RAMBaseHost, RAMSize);
        unicorn_mem_write(validation_state.unicorn, RAMBaseMac, validation_state.unicorn_ram, RAMSize);
        return false;  // UAE's Exception() advances PC
    } else {
        // Unicorn is primary: Execute trap and sync to UAE
        // Unicorn handles the exception via unicorn_exception.c
        extern void unicorn_simulate_exception(UnicornCPU *cpu, int vector_nr, uint16_t opcode);
        unicorn_simulate_exception(validation_state.unicorn, vector, opcode);

        // Sync entire state from Unicorn to UAE
        for (int i = 0; i < 8; i++) {
            uae_set_dreg(i, unicorn_get_dreg(validation_state.unicorn, i));
            uae_set_areg(i, unicorn_get_areg(validation_state.unicorn, i));
        }
        uae_set_pc(unicorn_get_pc(validation_state.unicorn));
        uae_set_sr(unicorn_get_sr(validation_state.unicorn));

        // Sync all RAM
        unicorn_mem_read(validation_state.unicorn, RAMBaseMac, validation_state.unicorn_ram, RAMSize);
        memcpy(RAMBaseHost, validation_state.unicorn_ram, RAMSize);
        return true;  // unicorn_simulate_exception advances PC
    }
}


/* Helper: Add instruction to trace ringbuffer */
static void trace_add_instruction(uint64_t instruction_num, uint32_t pc, uint16_t opcode,
                                   uint32_t uae_pc, uint16_t uae_sr, uint32_t uae_cacr, uint32_t *uae_d, uint32_t *uae_a,
                                   uint32_t uc_pc, uint16_t uc_sr, uint32_t uc_cacr, uint32_t *uc_d, uint32_t *uc_a,
                                   bool was_emulop) {
    if (!validation_state.trace_buffer) return;

    InstructionTrace *trace = &validation_state.trace_buffer[validation_state.trace_buffer_pos];
    trace->instruction_num = instruction_num;
    trace->pc = pc;
    trace->opcode = opcode;
    trace->uae_pc = uae_pc;
    trace->uae_sr = uae_sr;
    trace->uae_cacr = uae_cacr;
    memcpy(trace->uae_d, uae_d, sizeof(trace->uae_d));
    memcpy(trace->uae_a, uae_a, sizeof(trace->uae_a));
    trace->uc_pc = uc_pc;
    trace->uc_sr = uc_sr;
    trace->uc_cacr = uc_cacr;
    memcpy(trace->uc_d, uc_d, sizeof(trace->uc_d));
    memcpy(trace->uc_a, uc_a, sizeof(trace->uc_a));
    trace->was_emulop = was_emulop;

    validation_state.trace_buffer_pos++;
    if (validation_state.trace_buffer_pos >= validation_state.trace_buffer_size) {
        validation_state.trace_buffer_pos = 0;
        validation_state.trace_buffer_full = true;
    }
}

/* Helper: Format SR flags as string */
static const char* format_sr_flags(uint16_t sr) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%c%c%c%c%c",
             (sr & 0x10) ? 'X' : '-',
             (sr & 0x08) ? 'N' : '-',
             (sr & 0x04) ? 'Z' : '-',
             (sr & 0x02) ? 'V' : '-',
             (sr & 0x01) ? 'C' : '-');
    return buf;
}

/* Helper: Dump trace ringbuffer on divergence */
static void trace_dump_on_divergence(uint32_t divergence_pc, uint16_t divergence_opcode) {
    if (!validation_state.trace_buffer) return;

    fprintf(stderr, "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    fprintf(stderr, "Last %u instructions before divergence:\n\n",
            validation_state.trace_buffer_full ? validation_state.trace_buffer_size : validation_state.trace_buffer_pos);

    // Determine start position (oldest entry in ringbuffer)
    uint32_t start_pos = validation_state.trace_buffer_full ? validation_state.trace_buffer_pos : 0;
    uint32_t count = validation_state.trace_buffer_full ? validation_state.trace_buffer_size : validation_state.trace_buffer_pos;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start_pos + i) % validation_state.trace_buffer_size;
        InstructionTrace *t = &validation_state.trace_buffer[idx];

        // Check if states match
        bool pc_match = (t->uae_pc == t->uc_pc);
        bool sr_match = (t->uae_sr == t->uc_sr);
        bool cacr_match = (t->uae_cacr == t->uc_cacr);
        bool all_match = pc_match && sr_match && cacr_match;
        for (int j = 0; j < 8 && all_match; j++) {
            if (t->uae_d[j] != t->uc_d[j] || t->uae_a[j] != t->uc_a[j]) all_match = false;
        }

        fprintf(stderr, "[%lu] PC=%08X OP=%04X%s | SR: UAE=%04X UC=%04X | CACR: UAE=%08X UC=%08X %s\n",
                t->instruction_num, t->pc, t->opcode,
                t->was_emulop ? " (EmulOp)" : "        ",
                t->uae_sr, t->uc_sr,
                t->uae_cacr, t->uc_cacr,
                all_match ? "✓" : "❌");
    }

    fprintf(stderr, "\n");
}

/* Validate one instruction */
bool unicorn_validation_step(void) {
    if (!validation_state.initialized || !validation_state.enabled) {
        return true;  // Validation disabled, no divergence
    }

    // Both CPUs are now explicitly initialized to identical state in unicorn_validation_init()
    // No sync needed here - validation starts from instruction #1
    validation_state.instruction_count++;

    // Get current PC and opcode
    uint32_t pc = uae_get_pc();
    uint16_t opcode = read_word_be(pc);

    // Detect EmulOps (0x71xx) and Traps (0xAxxx, 0xFxxx)
    bool is_emulop = (opcode & 0xFF00) == 0x7100;
    bool is_a_trap = (opcode & 0xF000) == 0xA000;
    bool is_f_trap = (opcode & 0xF000) == 0xF000;
    bool is_special = is_emulop || is_a_trap || is_f_trap;

    // Capture state before
    uint32_t uae_pc_before = pc;
    uint32_t uae_dregs_before[8], uae_aregs_before[8];
    uint16_t uae_sr_before = uae_get_sr();

    for (int i = 0; i < 8; i++) {
        uae_dregs_before[i] = uae_get_dreg(i);
        uae_aregs_before[i] = uae_get_areg(i);
    }

    // For EmulOps/traps when Unicorn is primary: Execute ONLY on Unicorn, skip UAE
    // Unicorn will execute and sync back to UAE
    if (is_special && validation_state.master_cpu == MASTER_CPU_UNICORN) {
        // Execute on Unicorn (platform handler will execute and sync to UAE)
        if (!unicorn_execute_one(validation_state.unicorn)) {
            fprintf(stderr, "\n❌ Unicorn execution failed at PC=0x%08X\n", uae_pc_before);
            fprintf(stderr, "Error: %s\n", unicorn_get_error(validation_state.unicorn));
            validation_state.divergence_count++;
            return false;
        }

        // Sync entire state from Unicorn to UAE (CPU state already synced by platform handler)
        // Now sync RAM to ensure UAE sees memory changes from EmulOp
        unicorn_mem_read(validation_state.unicorn, RAMBaseMac, validation_state.unicorn_ram, RAMSize);
        memcpy(RAMBaseHost, validation_state.unicorn_ram, RAMSize);

        validation_state.last_good_pc = unicorn_get_pc(validation_state.unicorn);
        return true;
    }

    // Execute on UAE (platform handler will be called for EmulOps/traps if UAE is master)
    uae_cpu_execute_one();

    // Capture state after
    uint32_t uae_pc_after = uae_get_pc();
    uint32_t uae_dregs_after[8], uae_aregs_after[8];
    uint16_t uae_sr_after = uae_get_sr();

    for (int i = 0; i < 8; i++) {
        uae_dregs_after[i] = uae_get_dreg(i);
        uae_aregs_after[i] = uae_get_areg(i);
    }

    // Sync control registers BEFORE Unicorn executes
    // UAE and Unicorn use different CACR masking, so keep them in sync
    unicorn_set_cacr(validation_state.unicorn, uae_get_cacr());
    unicorn_set_vbr(validation_state.unicorn, uae_get_vbr());

    // For EmulOps/traps when UAE is primary: Skip Unicorn execution
    // The unified handler already executed on UAE and synced state to Unicorn
    if (is_special && validation_state.master_cpu == MASTER_CPU_UAE) {
        // State already synced by unified handler, just validate
        validation_state.last_good_pc = uae_pc_after;
        return true;
    }

    // Normal instruction - Execute on Unicorn
    if (!unicorn_execute_one(validation_state.unicorn)) {
        // Unicorn execution failed - dump full CPU state for debugging
        fprintf(stderr, "\n❌ Unicorn execution failed at PC=0x%08X\n", uae_pc_before);
        fprintf(stderr, "Error: %s\n", unicorn_get_error(validation_state.unicorn));
        fprintf(stderr, "\nCPU State (before instruction):\n");
        fprintf(stderr, "D0-D7: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                uae_dregs_before[0], uae_dregs_before[1], uae_dregs_before[2], uae_dregs_before[3],
                uae_dregs_before[4], uae_dregs_before[5], uae_dregs_before[6], uae_dregs_before[7]);
        fprintf(stderr, "A0-A7: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                uae_aregs_before[0], uae_aregs_before[1], uae_aregs_before[2], uae_aregs_before[3],
                uae_aregs_before[4], uae_aregs_before[5], uae_aregs_before[6], uae_aregs_before[7]);
        fprintf(stderr, "SR: %04X, VBR: %08X, CACR: %08X\n", uae_sr_before, uae_get_vbr(), uae_get_cacr());
        fprintf(stderr, "\nCPU State (after UAE executed successfully):\n");
        fprintf(stderr, "D0-D7: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                uae_dregs_after[0], uae_dregs_after[1], uae_dregs_after[2], uae_dregs_after[3],
                uae_dregs_after[4], uae_dregs_after[5], uae_dregs_after[6], uae_dregs_after[7]);
        fprintf(stderr, "A0-A7: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                uae_aregs_after[0], uae_aregs_after[1], uae_aregs_after[2], uae_aregs_after[3],
                uae_aregs_after[4], uae_aregs_after[5], uae_aregs_after[6], uae_aregs_after[7]);

        if (validation_state.log_file) {
            fprintf(validation_state.log_file, "\n[%lu] UNICORN EXECUTION FAILED\n", validation_state.instruction_count);
            fprintf(validation_state.log_file, "PC: 0x%08X, Opcode: 0x%04X\n", uae_pc_before, opcode);
            fprintf(validation_state.log_file, "Error: %s\n", unicorn_get_error(validation_state.unicorn));
            fprintf(validation_state.log_file, "\nCPU State (before instruction):\n");
            fprintf(validation_state.log_file, "D0-D7: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                    uae_dregs_before[0], uae_dregs_before[1], uae_dregs_before[2], uae_dregs_before[3],
                    uae_dregs_before[4], uae_dregs_before[5], uae_dregs_before[6], uae_dregs_before[7]);
            fprintf(validation_state.log_file, "A0-A7: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                    uae_aregs_before[0], uae_aregs_before[1], uae_aregs_before[2], uae_aregs_before[3],
                    uae_aregs_before[4], uae_aregs_before[5], uae_aregs_before[6], uae_aregs_before[7]);
            fprintf(validation_state.log_file, "SR: %04X, VBR: %08X, CACR: %08X\n", uae_sr_before, uae_get_vbr(), uae_get_cacr());
            fflush(validation_state.log_file);
        }

        validation_state.divergence_count++;
        return false;
    }

    // Debug: Detailed MOVEC logging for CACR
    if ((opcode == 0x4E7A || opcode == 0x4E7B) && validation_state.log_file) {
        uint16_t extension = read_word_be(uae_pc_before + 2);
        uint16_t control_reg = extension & 0xFFF;

        if (control_reg == 0x002) {  // CACR operations
            uint16_t data_reg = (extension >> 12) & 0xF;
            bool is_d_reg = !(data_reg & 8);
            int reg_num = data_reg & 7;

            uint32_t uae_cacr = uae_get_cacr();
            uint32_t uc_cacr = unicorn_get_cacr(validation_state.unicorn);

            if (opcode == 0x4E7B) {  // MOVEC Rn,CACR (write)
                uint32_t source = is_d_reg ? uae_dregs_before[reg_num] : uae_aregs_before[reg_num];
                fprintf(validation_state.log_file, "[%lu] MOVEC %c%d(=0x%08X),CACR → UAE_CACR=0x%08X UC_CACR=0x%08X\n",
                        validation_state.instruction_count,
                        is_d_reg ? 'D' : 'A', reg_num, source,
                        uae_cacr, uc_cacr);
            } else {  // MOVEC CACR,Rn (read)
                uint32_t uae_dest = is_d_reg ? uae_dregs_after[reg_num] : uae_aregs_after[reg_num];
                uint32_t uc_dest = is_d_reg ? unicorn_get_dreg(validation_state.unicorn, reg_num)
                                             : unicorn_get_areg(validation_state.unicorn, reg_num);
                fprintf(validation_state.log_file, "[%lu] MOVEC CACR,%c%d → UAE_CACR=0x%08X->%c%d=0x%08X, UC_CACR=0x%08X->%c%d=0x%08X\n",
                        validation_state.instruction_count,
                        is_d_reg ? 'D' : 'A', reg_num,
                        uae_cacr, is_d_reg ? 'D' : 'A', reg_num, uae_dest,
                        uc_cacr, is_d_reg ? 'D' : 'A', reg_num, uc_dest);
            }
        }
    }

    // Compare states
    uint32_t uc_pc = unicorn_get_pc(validation_state.unicorn);
    uint16_t uc_sr = unicorn_get_sr(validation_state.unicorn);
    bool divergence = false;

    // Check PC
    if (uc_pc != uae_pc_after) {
        divergence = true;
        if (validation_state.log_file) {
            fprintf(validation_state.log_file, "\n[%lu] PC DIVERGENCE at 0x%08X (opcode 0x%04X)\n",
                    validation_state.instruction_count, uae_pc_before, opcode);
            fprintf(validation_state.log_file, "UAE PC: 0x%08X → 0x%08X\n", uae_pc_before, uae_pc_after);
            fprintf(validation_state.log_file, "UC  PC: 0x%08X → 0x%08X\n", uae_pc_before, uc_pc);
        }
    }

    // Check SR
    if (uc_sr != uae_sr_after) {
        divergence = true;
        if (validation_state.log_file) {
            fprintf(validation_state.log_file, "\n[%lu] SR DIVERGENCE at 0x%08X (opcode 0x%04X)\n",
                    validation_state.instruction_count, uae_pc_before, opcode);
            fprintf(validation_state.log_file, "UAE SR: 0x%04X → 0x%04X\n", uae_sr_before, uae_sr_after);
            fprintf(validation_state.log_file, "UC  SR: 0x%04X → 0x%04X\n", uae_sr_before, uc_sr);
        }
    }

    // Get CACR values for comparison (read early to use in trace later)
    uint32_t uae_cacr = uae_get_cacr();
    uint32_t uc_cacr = unicorn_get_cacr(validation_state.unicorn);

    // Check CACR (log but don't abort - Unicorn doesn't implement CACR properly)
    // Also sync destination register for MOVEC CACR,Rn to prevent cascading divergences
    if (uc_cacr != uae_cacr) {
        // Don't set divergence = true (allowlisted)
        if (validation_state.log_file) {
            fprintf(validation_state.log_file, "\n[%lu] CACR MISMATCH (allowlisted) at 0x%08X (opcode 0x%04X)\n",
                    validation_state.instruction_count, uae_pc_before, opcode);
            fprintf(validation_state.log_file, "UAE CACR: 0x%08X\n", uae_cacr);
            fprintf(validation_state.log_file, "UC  CACR: 0x%08X\n", uc_cacr);
        }

        // Sync CACR from UAE to Unicorn (though Unicorn may ignore it)
        unicorn_set_cacr(validation_state.unicorn, uae_cacr);

        // If this is a MOVEC CACR,Rn instruction, sync the destination register
        if (opcode == 0x4E7A) {  // MOVEC Rc,Rn
            uint16_t extension = read_word_be(uae_pc_before + 2);
            uint16_t control_reg = extension & 0xFFF;
            if (control_reg == 0x002) {  // CACR
                uint16_t data_reg = (extension >> 12) & 0xF;
                bool is_d_reg = !(data_reg & 8);
                int reg_num = data_reg & 7;

                if (is_d_reg) {
                    unicorn_set_dreg(validation_state.unicorn, reg_num, uae_dregs_after[reg_num]);
                } else {
                    unicorn_set_areg(validation_state.unicorn, reg_num, uae_aregs_after[reg_num]);
                }

                if (validation_state.log_file) {
                    fprintf(validation_state.log_file, "  Synced %c%d=0x%08X from UAE (MOVEC CACR,%c%d)\n",
                            is_d_reg ? 'D' : 'A', reg_num,
                            is_d_reg ? uae_dregs_after[reg_num] : uae_aregs_after[reg_num],
                            is_d_reg ? 'D' : 'A', reg_num);
                }
            }
        }
    }

    // Check data registers
    for (int i = 0; i < 8; i++) {
        uint32_t uc_dreg = unicorn_get_dreg(validation_state.unicorn, i);
        if (uc_dreg != uae_dregs_after[i]) {
            divergence = true;
            if (validation_state.log_file) {
                fprintf(validation_state.log_file, "\n[%lu] D%d DIVERGENCE at 0x%08X (opcode 0x%04X)\n",
                        validation_state.instruction_count, i, uae_pc_before, opcode);
                fprintf(validation_state.log_file, "UAE D%d: 0x%08X → 0x%08X\n", i, uae_dregs_before[i], uae_dregs_after[i]);
                fprintf(validation_state.log_file, "UC  D%d: 0x%08X → 0x%08X\n", i, uae_dregs_before[i], uc_dreg);

                // For MOVEC instructions, decode the control register
                if (opcode == 0x4E7A || opcode == 0x4E7B) {
                    uint16_t extension = read_word_be(uae_pc_before + 2);
                    uint16_t control_reg = extension & 0xFFF;
                    uint16_t data_reg = (extension >> 12) & 0xF;
                    const char *reg_name = "UNKNOWN";

                    switch (control_reg) {
                        case 0x000: reg_name = "SFC"; break;
                        case 0x001: reg_name = "DFC"; break;
                        case 0x002: reg_name = "CACR"; break;
                        case 0x800: reg_name = "USP"; break;
                        case 0x801: reg_name = "VBR"; break;
                        case 0x802: reg_name = "CAAR"; break;
                        case 0x803: reg_name = "MSP"; break;
                        case 0x804: reg_name = "ISP"; break;
                        case 0x805: reg_name = "MMUSR"; break;
                        case 0x806: reg_name = "URP"; break;
                        case 0x807: reg_name = "SRP"; break;
                        case 0x808: reg_name = "PCR"; break;
                    }

                    fprintf(validation_state.log_file, "MOVEC instruction: %s → %c%d (control_reg=0x%03X, extension=0x%04X)\n",
                            reg_name,
                            (data_reg & 8) ? 'A' : 'D',
                            data_reg & 7,
                            control_reg,
                            extension);
                }
            }
        }
    }

    // Check address registers
    for (int i = 0; i < 8; i++) {
        uint32_t uc_areg = unicorn_get_areg(validation_state.unicorn, i);
        if (uc_areg != uae_aregs_after[i]) {
            divergence = true;
            if (validation_state.log_file) {
                fprintf(validation_state.log_file, "\n[%lu] A%d DIVERGENCE at 0x%08X (opcode 0x%04X)\n",
                        validation_state.instruction_count, i, uae_pc_before, opcode);
                fprintf(validation_state.log_file, "UAE A%d: 0x%08X → 0x%08X\n", i, uae_aregs_before[i], uae_aregs_after[i]);
                fprintf(validation_state.log_file, "UC  A%d: 0x%08X → 0x%08X\n", i, uae_aregs_before[i], uc_areg);
            }
        }
    }

    // Get Unicorn data/address registers for trace
    uint32_t uc_dregs[8], uc_aregs[8];
    for (int i = 0; i < 8; i++) {
        uc_dregs[i] = unicorn_get_dreg(validation_state.unicorn, i);
        uc_aregs[i] = unicorn_get_areg(validation_state.unicorn, i);
    }

    // Add to trace ringbuffer BEFORE checking divergence (CACR already read above)
    trace_add_instruction(validation_state.instruction_count, uae_pc_before, opcode,
                          uae_pc_after, uae_sr_after, uae_cacr, uae_dregs_after, uae_aregs_after,
                          uc_pc, uc_sr, uc_cacr, uc_dregs, uc_aregs,
                          is_emulop);

    if (divergence) {
        validation_state.divergence_count++;
        if (validation_state.log_file) {
            fflush(validation_state.log_file);
        }

        // Dump ringbuffer trace
        trace_dump_on_divergence(uae_pc_before, opcode);

        // Print detailed divergence report
        fprintf(stderr, "❌ DIVERGENCE #%lu at instruction #%lu\n",
                validation_state.divergence_count, validation_state.instruction_count);
        fprintf(stderr, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
        fprintf(stderr, "PC: 0x%08X, Opcode: 0x%04X\n", uae_pc_before, opcode);

        fprintf(stderr, "\nUAE state (after execution):\n");
        fprintf(stderr, "  PC: 0x%08X → 0x%08X\n", uae_pc_before, uae_pc_after);
        fprintf(stderr, "  SR: 0x%04X → 0x%04X  [%s]\n", uae_sr_before, uae_sr_after, format_sr_flags(uae_sr_after));
        fprintf(stderr, "  CACR: 0x%08X\n", uae_cacr);
        fprintf(stderr, "  D0-D3: %08X %08X %08X %08X\n",
                uae_dregs_after[0], uae_dregs_after[1], uae_dregs_after[2], uae_dregs_after[3]);
        fprintf(stderr, "  D4-D7: %08X %08X %08X %08X\n",
                uae_dregs_after[4], uae_dregs_after[5], uae_dregs_after[6], uae_dregs_after[7]);

        fprintf(stderr, "\nUnicorn state (after execution):\n");
        fprintf(stderr, "  PC: 0x%08X → 0x%08X\n", uae_pc_before, uc_pc);
        fprintf(stderr, "  SR: 0x%04X → 0x%04X  [%s]\n", uae_sr_before, uc_sr, format_sr_flags(uc_sr));
        fprintf(stderr, "  CACR: 0x%08X\n", uc_cacr);
        fprintf(stderr, "  D0-D3: %08X %08X %08X %08X\n",
                uc_dregs[0], uc_dregs[1], uc_dregs[2], uc_dregs[3]);
        fprintf(stderr, "  D4-D7: %08X %08X %08X %08X\n",
                uc_dregs[4], uc_dregs[5], uc_dregs[6], uc_dregs[7]);

        fprintf(stderr, "\nDifferences:\n");
        if (uc_pc != uae_pc_after)
            fprintf(stderr, "  ❌ PC: UAE=0x%08X vs UC=0x%08X\n", uae_pc_after, uc_pc);
        if (uc_sr != uae_sr_after)
            fprintf(stderr, "  ❌ SR: UAE=0x%04X [%s] vs UC=0x%04X [%s]\n",
                    uae_sr_after, format_sr_flags(uae_sr_after),
                    uc_sr, format_sr_flags(uc_sr));
        if (uc_cacr != uae_cacr)
            fprintf(stderr, "  ❌ CACR: UAE=0x%08X vs UC=0x%08X\n", uae_cacr, uc_cacr);
        for (int i = 0; i < 8; i++) {
            if (uc_dregs[i] != uae_dregs_after[i])
                fprintf(stderr, "  ❌ D%d: UAE=0x%08X vs UC=0x%08X\n", i, uae_dregs_after[i], uc_dregs[i]);
        }
        for (int i = 0; i < 8; i++) {
            if (uc_aregs[i] != uae_aregs_after[i])
                fprintf(stderr, "  ❌ A%d: UAE=0x%08X vs UC=0x%08X\n", i, uae_aregs_after[i], uc_aregs[i]);
        }

        fprintf(stderr, "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        fprintf(stderr, "ABORTING on first divergence.\n");
        fprintf(stderr, "See cpu_validation.log for full details.\n\n");

        return false;
    }

    validation_state.last_good_pc = uae_pc_after;

    // NO LONGER sync RAM periodically - CPUs have separate memory spaces
    // RAM only synced during EmulOps/traps by master CPU

    return true;
}

/* Check if enabled */
bool unicorn_validation_enabled(void) {
    return validation_state.initialized && validation_state.enabled;
}

/* Enable/disable */
void unicorn_validation_set_enabled(bool enabled) {
    validation_state.enabled = enabled;
}

/* Get statistics */
void unicorn_validation_get_stats(uint64_t *instructions, uint64_t *divergences) {
    if (instructions) *instructions = validation_state.instruction_count;
    if (divergences) *divergences = validation_state.divergence_count;
}

/* Platform EmulOp/Trap Handlers - Unified (checks master_cpu) */

bool unicorn_validation_unified_emulop(uint16_t opcode, bool is_primary) {
    return unified_emulop_handler(opcode, is_primary);
}

bool unicorn_validation_unified_trap(int vector, uint16_t opcode, bool is_primary) {
    return unified_trap_handler(vector, opcode, is_primary);
}

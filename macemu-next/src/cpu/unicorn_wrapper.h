/**
 * Unicorn Engine Wrapper API
 *
 * Clean C API wrapper around Unicorn Engine for BasiliskII/SheepShaver
 */

#ifndef UNICORN_WRAPPER_H
#define UNICORN_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque CPU handle */
typedef struct UnicornCPU UnicornCPU;

/* CPU Architecture */
typedef enum {
    UCPU_ARCH_M68K,
    UCPU_ARCH_PPC,
    UCPU_ARCH_PPC64
} UnicornArch;

/* NOTE: EmulOpHandler and ExceptionHandler typedefs removed - legacy per-CPU API deprecated.
 * Use platform API (g_platform.emulop_handler, g_platform.trap_handler) instead.
 */

/* Memory access hook callback */
typedef enum {
    UCPU_MEM_READ,
    UCPU_MEM_WRITE
} UnicornMemType;

typedef void (*MemoryHookCallback)(UnicornCPU *cpu, UnicornMemType type,
                                   uint64_t address, uint32_t size,
                                   uint64_t value, void *user_data);

/* CPU lifecycle */
UnicornCPU* unicorn_create(UnicornArch arch);
UnicornCPU* unicorn_create_with_model(UnicornArch arch, int cpu_model); /* M68K: UC_CPU_M68K_M68040, etc. */
void unicorn_destroy(UnicornCPU *cpu);

/* Configuration (for DualCPU backend - stores CPU type for later use) */
void unicorn_set_cpu_type(int cpu_type, int fpu_type);

/* Memory mapping */
bool unicorn_map_ram(UnicornCPU *cpu, uint64_t addr, void *host_ptr, uint64_t size);
bool unicorn_map_rom(UnicornCPU *cpu, uint64_t addr, const void *host_ptr, uint64_t size);
bool unicorn_map_rom_writable(UnicornCPU *cpu, uint64_t addr, const void *host_ptr, uint64_t size); /* For validation/debugging */
bool unicorn_unmap(UnicornCPU *cpu, uint64_t addr, uint64_t size);

/* Memory access */
bool unicorn_mem_write(UnicornCPU *cpu, uint64_t addr, const void *data, size_t size);
bool unicorn_mem_read(UnicornCPU *cpu, uint64_t addr, void *data, size_t size);

/* Execution */
bool unicorn_execute_one(UnicornCPU *cpu);
bool unicorn_execute_n(UnicornCPU *cpu, uint64_t count);
bool unicorn_execute_until(UnicornCPU *cpu, uint64_t end_addr);
void unicorn_stop(UnicornCPU *cpu);

/* Registers - M68K specific */
uint32_t unicorn_get_dreg(UnicornCPU *cpu, int reg);  /* D0-D7 */
uint32_t unicorn_get_areg(UnicornCPU *cpu, int reg);  /* A0-A7 */
void unicorn_set_dreg(UnicornCPU *cpu, int reg, uint32_t value);
void unicorn_set_areg(UnicornCPU *cpu, int reg, uint32_t value);

uint32_t unicorn_get_pc(UnicornCPU *cpu);
void unicorn_set_pc(UnicornCPU *cpu, uint32_t value);

uint16_t unicorn_get_sr(UnicornCPU *cpu);
void unicorn_set_sr(UnicornCPU *cpu, uint16_t value);

/* Registers - M68K control registers */
uint32_t unicorn_get_cacr(UnicornCPU *cpu);
void unicorn_set_cacr(UnicornCPU *cpu, uint32_t value);
uint32_t unicorn_get_vbr(UnicornCPU *cpu);
void unicorn_set_vbr(UnicornCPU *cpu, uint32_t value);

/* Registers - PPC specific */
uint32_t unicorn_get_gpr(UnicornCPU *cpu, int reg);   /* GPR0-GPR31 */
void unicorn_set_gpr(UnicornCPU *cpu, int reg, uint32_t value);

uint32_t unicorn_get_spr(UnicornCPU *cpu, int spr);   /* Special registers */
void unicorn_set_spr(UnicornCPU *cpu, int spr, uint32_t value);

uint32_t unicorn_get_msr(UnicornCPU *cpu);
void unicorn_set_msr(UnicornCPU *cpu, uint32_t value);

/* Hooks */
/* NOTE: Legacy per-CPU hook APIs removed (unicorn_set_emulop_handler, unicorn_set_exception_handler)
 * Use platform API (g_platform.emulop_handler, g_platform.trap_handler) instead.
 * These are automatically checked by UC_HOOK_INSN_INVALID at CPU creation time.
 */
void unicorn_set_memory_hook(UnicornCPU *cpu, MemoryHookCallback callback, void *user_data);

/* Internal access (for exception handler and cpu_unicorn.cpp) */
void* unicorn_get_uc(UnicornCPU *cpu);  /* Returns uc_engine* - for exception.c only */

/* Forward declare uc_engine for hook callback */
struct uc_struct;
typedef struct uc_struct uc_engine;

/* Hook callback (for cpu_unicorn.cpp to register directly) */
void hook_code(uc_engine *uc, uint64_t address, uint32_t size, void *user_data);

/* Error handling */
const char* unicorn_get_error(UnicornCPU *cpu);

/* Block statistics (for timing analysis) */
void unicorn_print_block_stats(UnicornCPU *cpu);
void unicorn_reset_block_stats(UnicornCPU *cpu);

/* Interrupt triggering (for platform API) */
void unicorn_trigger_interrupt_internal(int level);

#ifdef __cplusplus
}
#endif

#endif /* UNICORN_WRAPPER_H */

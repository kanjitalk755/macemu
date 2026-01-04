/**
 * UAE CPU Wrapper Implementation
 */

#include "uae_wrapper.h"
#include "cpu_trace.h"
#include "timer_interrupt.h"

// UAE CPU headers
extern "C" {
#include "sysdeps.h"
#include "uae_cpu/main.h"
}

#include "uae_cpu/cpu_emulation.h"  // C++ linkage for Init680x0/Exit680x0
#include "uae_cpu/m68k.h"
#include "uae_cpu/newcpu.h"
#include "uae_cpu/memory.h"

#include <stdlib.h>
#include <string.h>

/* Global UAE registers */
extern struct regstruct regs;

/* CPU/FPU types */
int CPUType = CPU_68020;
int FPUType = FPU_68881;

/* Interrupt flags */
volatile uint32 InterruptFlags = 0;

/* Pending interrupt flag (shared by all CPU backends) */
volatile bool PendingInterrupt = false;

/* Quit flag */
volatile bool QuitEmulator = false;

/* Memory pointers are defined in basilisk_glue.cpp */
extern uint32 RAMBaseMac;
extern uint8 *RAMBaseHost;
extern uint32 RAMSize;
extern uint32 ROMBaseMac;
extern uint8 *ROMBaseHost;
extern uint32 ROMSize;
extern int MacFrameLayout;
#if !REAL_ADDRESSING && !DIRECT_ADDRESSING
extern uint8 *MacFrameBaseHost;
extern uint32 MacFrameSize;
#endif

/* CPU configuration */
void uae_set_cpu_type(int cpu_type, int fpu_type) {
    CPUType = cpu_type;
    FPUType = fpu_type;
}

/* CPU initialization */
bool uae_cpu_init(void) {
    return Init680x0();
}

void uae_cpu_reset(void) {
    m68k_reset();
}

void uae_cpu_exit(void) {
    Exit680x0();
}

/* Memory initialization */
bool uae_mem_init(uint32_t ram_size, uint32_t rom_size) {
    RAMSize = ram_size;
    ROMSize = rom_size;

    RAMBaseHost = (uint8 *)calloc(1, ram_size);
    if (!RAMBaseHost) return false;

    ROMBaseHost = (uint8 *)malloc(rom_size);
    if (!ROMBaseHost) {
        free(RAMBaseHost);
        RAMBaseHost = NULL;
        return false;
    }

    RAMBaseMac = 0x00000000;
    ROMBaseMac = 0x00400000;  /* Typical Mac ROM location */

    return true;
}

void uae_mem_cleanup(void) {
    if (RAMBaseHost) {
        free(RAMBaseHost);
        RAMBaseHost = NULL;
    }
    if (ROMBaseHost) {
        free(ROMBaseHost);
        ROMBaseHost = NULL;
    }
}

/* Memory mapping - UAE uses global pointers, so these are no-ops */
void uae_mem_map_ram(uint32_t addr, uint32_t size) {
    (void)addr;
    (void)size;
    /* UAE memory model uses RAMBaseHost global */
}

void uae_mem_map_rom(uint32_t addr, uint32_t size) {
    (void)addr;
    (void)size;
    /* UAE memory model uses ROMBaseHost global */
}

/* Memory access */
void uae_mem_write(uint32_t addr, const void *data, uint32_t size) {
    if (!data) return;

    uint8_t *dest = NULL;
    /* Determine if writing to RAM or ROM */
    if (addr >= RAMBaseMac && addr < RAMBaseMac + RAMSize) {
        uint32_t offset = addr - RAMBaseMac;
        dest = RAMBaseHost + offset;
    } else if (addr >= ROMBaseMac && addr < ROMBaseMac + ROMSize) {
        uint32_t offset = addr - ROMBaseMac;
        dest = ROMBaseHost + offset;
    }

    if (!dest) return;

    /* UAE stores memory in NATIVE (little-endian) format on x86.
     * Input data is big-endian M68K format, so byte-swap when writing. */
    const uint8_t *src = (const uint8_t *)data;
    if (size >= 2 && (addr & 1) == 0) {
        /* Word-aligned, swap 16-bit words */
        for (uint32_t i = 0; i < size - 1; i += 2) {
            dest[i] = src[i + 1];
            dest[i + 1] = src[i];
        }
        if (size & 1) dest[size - 1] = src[size - 1];  /* Odd trailing byte */
    } else {
        /* Unaligned or single byte */
        memcpy(dest, src, size);
    }
}

void uae_mem_read(uint32_t addr, void *data, uint32_t size) {
    if (!data) return;

    /* Determine if reading from RAM or ROM */
    if (addr >= RAMBaseMac && addr < RAMBaseMac + RAMSize) {
        uint32_t offset = addr - RAMBaseMac;
        memcpy(data, RAMBaseHost + offset, size);
    } else if (addr >= ROMBaseMac && addr < ROMBaseMac + ROMSize) {
        uint32_t offset = addr - ROMBaseMac;
        memcpy(data, ROMBaseHost + offset, size);
    }
}

/* Register access */
uint32_t uae_get_dreg(int reg) {
    if (reg < 0 || reg > 7) return 0;
    return m68k_dreg(regs, reg);
}

uint32_t uae_get_areg(int reg) {
    if (reg < 0 || reg > 7) return 0;
    return m68k_areg(regs, reg);
}

uint32_t uae_get_pc(void) {
    return m68k_getpc();
}

uint16_t uae_get_sr(void) {
    extern void MakeSR(void);
    MakeSR();  /* Build SR from separate flag variables (OPTIMIZED_FLAGS) */
    return regs.sr;
}

uint32_t uae_get_cacr(void) {
    extern uae_u32 m68k_get_cacr(void);
    return m68k_get_cacr();
}

uint32_t uae_get_vbr(void) {
    extern uae_u32 m68k_get_vbr(void);
    return m68k_get_vbr();
}

void uae_set_dreg(int reg, uint32_t value) {
    if (reg < 0 || reg > 7) return;
    m68k_dreg(regs, reg) = value;
}

void uae_set_areg(int reg, uint32_t value) {
    if (reg < 0 || reg > 7) return;
    m68k_areg(regs, reg) = value;
}

void uae_set_pc(uint32_t value) {
    m68k_setpc(value);
    regs.pc = value;
    fill_prefetch_0();
}

void uae_set_sr(uint16_t value) {
    extern void MakeFromSR(void);
    regs.sr = value;
    MakeFromSR();  /* Extract flags from SR into separate variables (OPTIMIZED_FLAGS) */
}

/* Hook handlers */
static UaeEmulOpHandler g_emulop_handler = NULL;
static void *g_emulop_user_data = NULL;
static UaeTrapHandler g_trap_handler = NULL;
static void *g_trap_user_data = NULL;

void uae_set_emulop_handler(UaeEmulOpHandler handler, void *user_data) {
    g_emulop_handler = handler;
    g_emulop_user_data = user_data;
}

void uae_set_trap_handler(UaeTrapHandler handler, void *user_data) {
    g_trap_handler = handler;
    g_trap_user_data = user_data;
}

/* Internal accessor functions for UAE CPU core */
extern "C" {
    void* uae_get_emulop_handler(void) { return (void*)g_emulop_handler; }
    void* uae_get_emulop_user_data(void) { return g_emulop_user_data; }
    void* uae_get_trap_handler(void) { return (void*)g_trap_handler; }
    void* uae_get_trap_user_data(void) { return g_trap_user_data; }
}

/* Execution */

void uae_cpu_execute_one(void) {
    /* Initialize trace on first call (from shared cpu_trace infrastructure) */
    static bool trace_initialized = false;
    if (!trace_initialized) {
        cpu_trace_init();
        trace_initialized = true;
    }

    /* Poll timer every 100 instructions */
    static int poll_counter = 0;
    if (++poll_counter >= 100) {
        poll_counter = 0;
        poll_timer_interrupt();  /* May set SPCFLAG_INT */
    }

    /* Execute one instruction */
    uae_u32 opcode = GET_OPCODE;

    // Optional trace output (enabled via CPU_TRACE env var)
    if (cpu_trace_should_log()) {
        uae_u32 pc_before = m68k_getpc();
        // Read raw opcode bytes (big-endian) for trace display
        uae_u16 opcode_raw = get_iword(0);
        // Materialize lazy condition codes before reading SR
        MakeSR();
        cpu_trace_log_detailed(
            "UAE",
            pc_before,
            opcode_raw,
            (unsigned int)regs.regs[0],   // D0
            (unsigned int)regs.regs[1],   // D1
            (unsigned int)regs.regs[2],   // D2
            (unsigned int)regs.regs[3],   // D3
            (unsigned int)regs.regs[4],   // D4
            (unsigned int)regs.regs[5],   // D5
            (unsigned int)regs.regs[6],   // D6
            (unsigned int)regs.regs[7],   // D7
            (unsigned int)regs.regs[8],   // A0
            (unsigned int)regs.regs[9],   // A1
            (unsigned int)regs.regs[10],  // A2
            (unsigned int)regs.regs[11],  // A3
            (unsigned int)regs.regs[12],  // A4
            (unsigned int)regs.regs[13],  // A5
            (unsigned int)regs.regs[14],  // A6
            (unsigned int)regs.regs[15],  // A7
            (unsigned int)regs.sr
        );
    }

    (*cpufunctbl[opcode])(opcode);

    cpu_trace_increment();
}

/* Set memory base pointers directly (for dual-CPU mode) */
extern uintptr MEMBaseDiff;  // From basilisk_glue.cpp

void uae_mem_set_ram_ptr(void *ptr, uint32_t size) {
    RAMBaseHost = (uint8 *)ptr;
    RAMSize = size;
    RAMBaseMac = 0x00000000;

    // Set MEMBaseDiff for direct addressing: host_ptr = mac_addr + MEMBaseDiff
    MEMBaseDiff = (uintptr)RAMBaseHost - RAMBaseMac;
}

void uae_mem_set_rom_ptr(void *ptr, uint32_t size) {
    ROMBaseHost = (uint8 *)ptr;
    ROMSize = size;
    ROMBaseMac = 0x00400000;  /* Typical Mac ROM location */
}

void uae_mem_set_rom_ptr_with_addr(void *ptr, uint32_t addr, uint32_t size) {
    ROMBaseHost = (uint8 *)ptr;
    ROMSize = size;
    ROMBaseMac = addr;  /* Use provided ROM address */
}

/* Memory access (individual sizes, big-endian) */
/*
 * IMPORTANT: These functions call the LOW-LEVEL do_get_mem_* functions directly,
 * NOT get_long/put_long, to avoid circular dependency when get_long/put_long
 * redirect to the platform API.
 *
 * do_get_mem_* handles endianness conversion (little-endian storage → big-endian return)
 */
uint8_t uae_mem_read_byte(uint32_t addr) {
    uae_u8 * const m = (uae_u8 *)do_get_real_address(addr);
    return do_get_mem_byte(m);
}

uint16_t uae_mem_read_word(uint32_t addr) {
    uae_u16 * const m = (uae_u16 *)do_get_real_address(addr);
    return do_get_mem_word(m);
}

uint32_t uae_mem_read_long(uint32_t addr) {
    uae_u32 * const m = (uae_u32 *)do_get_real_address(addr);
    return do_get_mem_long(m);
}

void uae_mem_write_byte(uint32_t addr, uint8_t val) {
    uae_u8 * const m = (uae_u8 *)do_get_real_address(addr);
    do_put_mem_byte(m, val);
}

void uae_mem_write_word(uint32_t addr, uint16_t val) {
    uae_u16 * const m = (uae_u16 *)do_get_real_address(addr);
    do_put_mem_word(m, val);
}

void uae_mem_write_long(uint32_t addr, uint32_t val) {
    uae_u32 * const m = (uae_u32 *)do_get_real_address(addr);
    do_put_mem_long(m, val);
}

/* Address translation */
uint8_t* uae_mem_mac_to_host(uint32_t addr) {
    return do_get_real_address(addr);
}

uint32_t uae_mem_host_to_mac(uint8_t *ptr) {
    return do_get_virtual_address(ptr);
}

/* Disassembly */
void uae_disasm(uint32_t addr, uint32_t *next_pc, int count) {
    uaecptr npc;
    m68k_disasm(addr, &npc, count);
    if (next_pc) {
        *next_pc = npc;
    }
}

/* Interrupt handling (shared by all CPU backends) */

extern "C" {

/**
 * Resume from idle state
 * Called when an interrupt occurs to wake up the CPU if it was halted
 * Stub implementation - can be extended if we implement CPU idle states
 */
void idle_resume(void) {
    // Stub - resume from idle if CPU is halted
}

/**
 * Trigger M68K interrupt level 1
 * Called by timers, ADB, and other devices to signal interrupts
 * Backend-agnostic - sets PendingInterrupt flag that all backends check
 */
void TriggerInterrupt(void) {
    idle_resume();  /* Resume from idle if CPU is halted */
    PendingInterrupt = true;  /* Signal interrupt to CPU backend */
}

/**
 * Trigger Non-Maskable Interrupt
 * Not currently implemented
 */
void TriggerNMI(void) {
    // TODO: Implement NMI handling
}

/**
 * Get current interrupt level from InterruptFlags
 * Returns 1 if any interrupt is pending, 0 otherwise
 */
int intlev(void) {
    return InterruptFlags ? 1 : 0;
}

} /* extern "C" */

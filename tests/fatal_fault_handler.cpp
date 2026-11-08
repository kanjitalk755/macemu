// Production fault-handler regression: fatal Mach faults must preserve the
// original failure without entering GUI teardown on the exception thread.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

using uint32 = uint32_t;
using uintptr = uintptr_t;
#if TEST_REAL_MACH_EXCEPTION
#include <sys/mman.h>
#include <unistd.h>
#include "sigsegv.h"
#else
enum sigsegv_return_t {
    SIGSEGV_RETURN_SUCCESS,
    SIGSEGV_RETURN_FAILURE,
    SIGSEGV_RETURN_SKIP_INSTRUCTION
};
struct sigsegv_info_t { void *address; void *instruction; };
static void *sigsegv_get_fault_address(sigsegv_info_t *info) { return info->address; }
static void *sigsegv_get_fault_instruction_address(sigsegv_info_t *info) { return info->instruction; }
#endif

constexpr uint32 ROMBase = 0x50000000, ROM_SIZE = 0x400000, ROM_AREA_SIZE = 0x500000;
constexpr uint32 RAMBase = 0x10000000, RAMSize = 0x10000000;
constexpr uint32 DR_CACHE_BASE = 0x60000000, DR_CACHE_SIZE = 0x100000;
// Keep fake address ranges distinct and representable on 32-bit hosts too.
static void *ROMBaseHost = reinterpret_cast<void *>(uintptr(0x30000000));
struct SheepMem {
    static uintptr ZeroPage() { return 0x40000000; }
    static uintptr PageSize() { return 4096; }
};
struct sheepshaver_cpu {
    uint32 pc() const { return ROMBase + 0x1000; }
    uint32 gpr(int) const { return 0; }
};
static sheepshaver_cpu cpu;
static sheepshaver_cpu *ppc_cpu = &cpu;
static bool ignore_fault = false, screen_fault = false;
static bool PrefsFindBool(const char *) { return ignore_fault; }
bool Screen_fault_handler(sigsegv_info_t *) { return screen_fault; }
static int register_dumps = 0, log_dumps = 0, disassembly_dumps = 0, monitor_calls = 0, quit_calls = 0;
static void dump_registers() { ++register_dumps; }
static void dump_log() { ++log_dumps; }
static void dump_disassembly(uint32, int, int) { ++disassembly_dumps; }
static void enter_mon() { ++monitor_calls; }
void QuitEmulator()
{
#if TEST_REAL_MACH_EXCEPTION
    // A real emulator would enter GUI teardown here. Exit with a distinct code
    // so the parent detects that path without depending on SDL or AppKit.
    std::fputs("Unexpected QuitEmulator on the Mach exception thread\n", stderr);
    std::_Exit(90);
#else
    ++quit_calls;
#endif
}

#define HAVE_SIGSEGV_SKIP_INSTRUCTION 1
#define ENABLE_VOSF 1
#include "fault_handler.inc"

int main()
{
#if TEST_REAL_MACH_EXCEPTION
    // Exercise the real Mach port, server thread, production callback, and
    // unhandled-exception response in an expendable subprocess.
    assert(sigsegv_install_handler(sigsegv_handler));
    void *page = mmap(nullptr, getpagesize(), PROT_NONE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(page != MAP_FAILED);
    std::fputs("Triggering a protected-page read\n", stderr);
    volatile unsigned char value = *static_cast<volatile unsigned char *>(page);
    (void)value;
    return 91; // A fatal exception must not resume the faulting instruction.
#else
    sigsegv_info_t fault {reinterpret_cast<void *>(uintptr(0x70006838)),
                          reinterpret_cast<void *>(uintptr(0x12345678))};
    // Recoverable accesses must still return before fatal diagnostics/teardown.
    screen_fault = true;
    assert(sigsegv_handler(&fault) == SIGSEGV_RETURN_SUCCESS);
    screen_fault = false;
    sigsegv_info_t rom_write {ROMBaseHost, fault.instruction};
    assert(sigsegv_handler(&rom_write) == SIGSEGV_RETURN_SKIP_INSTRUCTION);
    ignore_fault = true;
    assert(sigsegv_handler(&fault) == SIGSEGV_RETURN_SKIP_INSTRUCTION);
    ignore_fault = false;
    assert(register_dumps == 0 && log_dumps == 0 && disassembly_dumps == 0);
    assert(monitor_calls == 0 && quit_calls == 0);

    sigsegv_return_t result = SIGSEGV_RETURN_SUCCESS;
    std::thread exception_thread([&] { result = sigsegv_handler(&fault); });
    exception_thread.join();
    assert(result == SIGSEGV_RETURN_FAILURE);
    assert(register_dumps == 1 && log_dumps == 1 && disassembly_dumps == 1);
    assert(monitor_calls == 1);
    assert(quit_calls == (HAVE_MACH_EXCEPTIONS ? 0 : 1));
    std::puts("Fatal fault diagnostics, propagation, and recoverable accesses passed.");
#endif
}

# Debugging Crashes with Stack Traces

This document explains the crash handling system and how to get detailed debugging information from crashes.

## Overview

Both **BasiliskII** and the **WebRTC server** now have comprehensive crash handlers that print:
- Signal information (SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE)
- CPU register state (instruction pointer, stack pointer, etc.)
- Full stack backtrace with function names and line numbers
- Emulator/JIT state (for BasiliskII)
- Server state (for WebRTC server)

## Building with Debug Symbols

### Quick Rebuild (Already Configured)

The Makefiles have been updated to build with full debug symbols by default:
- `-g3`: Maximum debug information
- `-O0`: No optimization (makes debugging easier)
- `-fno-omit-frame-pointer`: Preserve frame pointers for better backtraces
- `-rdynamic`: Export symbols for backtrace_symbols()

```bash
# Rebuild WebRTC server with debug symbols
cd web-streaming
make clean && make

# Rebuild BasiliskII with debug symbols
cd BasiliskII/src/Unix
make clean && make
```

### Rebuilding from Configure (if needed)

If you need to regenerate the build system:

```bash
# WebRTC server
cd web-streaming
autoconf               # Regenerate configure from configure.ac
./configure            # Regenerate Makefile from Makefile.in
make clean && make

# BasiliskII
cd BasiliskII/src/Unix
autoconf               # Regenerate configure from configure.ac
./configure --enable-ipc-video --enable-ipc-audio
make clean && make
```

## Reading Crash Reports

### Example Crash Output

When a crash occurs, you'll see output like this:

```
╔════════════════════════════════════════════════════════════════╗
║              FATAL CRASH IN WEBRTC SERVER                      ║
╚════════════════════════════════════════════════════════════════╝

Signal:  11 (SIGSEGV (Segmentation Fault))
Code:    2
Address: 0x7465c57e30a8 (invalid memory access)

=== REGISTER STATE ===
  RIP: 0x0000606692a1e109  (instruction pointer)
  RSP: 0x00007465c77fa760  (stack pointer)
  RBP: 0x0000161152341168  (base pointer)
  RAX: 0x00007465c57e3000  RBX: 0x00007465c77fa7b0
  RCX: 0x0000000000000000  RDX: 0x00007465c77fb6e0
=== END REGISTER STATE ===

=== BACKTRACE ===
  [ 0] ./build/macemu-webrtc(crash_handler+0xca) [0x606692a1a1ca]
  [ 1] /lib/x86_64-linux-gnu/libc.so.6(+0x45330) [0x7465d2845330]
  [ 2] ./build/macemu-webrtc(encode_video_frame+0x109) [0x606692a1e109]
  [ 3] /lib/x86_64-linux-gnu/libstdc++.so.6(+0xecdb4) [0x7465d2cecdb4]
  ...
=== END BACKTRACE ===

=== SERVER STATE ===
  Emulator connected: YES
  Emulator PID:       219771
  Codec:              PNG
=== END SERVER STATE ===
```

### Understanding the Backtrace

With debug symbols (`-g3 -O0 -rdynamic`), you'll see:
- **Function names**: `encode_video_frame` instead of just addresses
- **Line offsets**: `+0x109` shows offset within the function
- **File names**: When using `addr2line` (see below)

## Getting More Detail with addr2line

To get exact source file and line numbers, use `addr2line`:

```bash
# Get the instruction pointer from RIP register
# Example: RIP: 0x0000606692a1e109

# Convert to source location
addr2line -e ./build/macemu-webrtc -f -C 0x606692a1e109

# Output will show:
# encode_video_frame
# /home/mick/macemu/web-streaming/server/server.cpp:1234
```

For each stack frame address:
```bash
addr2line -e ./build/macemu-webrtc -f -C 0x606692a1a1ca
```

## Using GDB for Post-Mortem Debugging

If a core dump is generated, you can analyze it with GDB:

### Enable Core Dumps
```bash
# Set unlimited core dump size
ulimit -c unlimited

# Check core dump pattern
cat /proc/sys/kernel/core_pattern
```

### Analyze Core Dump
```bash
# Load the core dump in GDB
gdb ./build/macemu-webrtc core

# Inside GDB:
(gdb) bt              # Print backtrace
(gdb) bt full         # Print backtrace with local variables
(gdb) frame 2         # Switch to frame 2
(gdb) list            # Show source code around crash
(gdb) info locals     # Show local variables
(gdb) print variable  # Print value of variable
```

## BasiliskII-Specific Crash Information

When BasiliskII crashes, you'll also see:

### M68K Emulator State
```
=== EMULATED M68K STATE ===
D0: 00000000  D1: 00000000  D2: 00000000  D3: 00000000
D4: 00000000  D5: 00000000  D6: 00000000  D7: 00000000
A0: 00000000  A1: 00000000  A2: 00000000  A3: 00000000
A4: 00000000  A5: 00000000  A6: 00000000  A7: 00000000
PC: 00400000  SR: 2700
=== END M68K STATE ===
```

### JIT Compiler State (if JIT_DEBUG enabled)
```
=== JIT COMPILER STATE ===
### Host addresses
MEM_BASE    : 7f1234567000
PC_P        : 0x7f1234568000
SPCFLAGS    : 0x7f1234569000
...
### M68k processor state
...
### Block in Mac address space
M68K block   : 0x00400000
Native block : 0x7f1234500000 (256 bytes)
=== END JIT STATE ===
```

## Signal Handler Architecture

### BasiliskII
1. **Early crash handler** (main_unix.cpp:458-498)
   - Handles: SIGABRT, SIGBUS, SIGILL, SIGFPE
   - Prints comprehensive crash report
   - Calls JIT compiler state dump

2. **sigsegv.cpp framework** (installed later)
   - Handles: SIGSEGV (for VOSF/JIT recovery)
   - If recovery fails → `sigsegv_dump_state()` with backtrace

### WebRTC Server
1. **Graceful shutdown** (server.cpp:132-135)
   - Handles: SIGINT, SIGTERM
   - Clean exit with resource cleanup

2. **Fatal crash handler** (server.cpp:138-216)
   - Handles: SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE
   - Prints comprehensive crash report
   - Prints server state (connections, codec, etc.)

## Flags Explanation

- **-g3**: Maximum debug info (includes macro definitions)
- **-O0**: No optimization (easier to debug, variables not optimized away)
- **-fno-omit-frame-pointer**: Keep frame pointers (better stack traces)
- **-rdynamic**: Export symbols to dynamic symbol table (for backtrace_symbols)
- **-fwrapv**: Define signed overflow behavior (safer for emulation)

## Performance Note

Debug builds (`-O0`) are **significantly slower** than release builds (`-O2`).

For production use, you may want to use:
- **-g3 -O2**: Optimized but still debuggable (some variables may be optimized away)
- **-g -O2**: Standard release with debug symbols

To switch back to optimized builds, edit:
- `web-streaming/configure.ac` line 128
- `BasiliskII/src/Unix/configure.ac` lines 233-238

Then rebuild:
```bash
autoconf && ./configure && make clean && make
```

## Files Modified

### New Files
- `BasiliskII/src/Unix/crash_handler.h` - Shared crash handling utilities

### Modified Files
- `BasiliskII/src/Unix/main_unix.cpp` - Enhanced crash handler
- `BasiliskII/src/Unix/configure.ac` - Debug flags
- `BasiliskII/src/Unix/Makefile` - Debug flags
- `web-streaming/server/server.cpp` - Crash handler
- `web-streaming/configure.ac` - Debug flags
- `web-streaming/Makefile.in` - Link with CXXFLAGS
- `web-streaming/Makefile` - Debug flags

## Troubleshooting

### "No symbols" in backtrace
- Verify binary has debug info: `file ./build/macemu-webrtc`
- Should say: `with debug_info, not stripped`
- Check: `readelf -S binary | grep debug_info`

### Missing line numbers
- Ensure built with `-g3 -O0`
- Check CXXFLAGS: `make -n | grep CXXFLAGS`

### Incomplete backtrace
- Ensure `-fno-omit-frame-pointer` and `-rdynamic`
- Check LDFLAGS: `make -n | grep LDFLAGS`

### Core dump not generated
- Check limits: `ulimit -c`
- Check pattern: `cat /proc/sys/kernel/core_pattern`
- Check apport: `sudo systemctl status apport`

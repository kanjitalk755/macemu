# Commands Reference

Build, test, debug, and trace commands for macemu-next.

---

## Build Commands

### Basic Build

```bash
cd macemu-next
meson setup build
meson compile -C build
```

This builds all three backends (UAE, Unicorn, DualCPU).

### Clean Build

```bash
rm -rf build
meson setup build
meson compile -C build
```

### Backend-Specific Build

```bash
# Build with specific backend as default
meson setup build -Dcpu_backend=unicorn  # or uae, dualcpu
meson compile -C build
```

### Debug Build

```bash
meson setup build --buildtype=debug
meson compile -C build
```

### Release Build

```bash
meson setup build --buildtype=release
meson compile -C build
```

---

## Configuration Commands

### Generate Default Config

```bash
./build/macemu-next --save-config
# Creates ~/.config/macemu-next/config.json
```

### Use Custom Config

```bash
./build/macemu-next --config myconfig.json ~/quadra.rom
```

### Edit Config

```bash
# User config
nano ~/.config/macemu-next/config.json

# Or copy example
cp config.example.json ~/.config/macemu-next/config.json
```

See **[JSON_CONFIG.md](JSON_CONFIG.md)** for complete configuration documentation.

---

## Run Commands

### Basic Execution

```bash
# Unicorn backend (primary)
CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# UAE backend (legacy)
CPU_BACKEND=uae ./build/macemu-next ~/quadra.rom

# DualCPU validation
CPU_BACKEND=dualcpu ./build/macemu-next ~/quadra.rom

# With custom config
CPU_BACKEND=unicorn ./build/macemu-next --config myconfig.json ~/quadra.rom
```

### With Timeout

```bash
# Auto-exit after N seconds (useful for testing)
EMULATOR_TIMEOUT=5 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom
```

---

## Environment Variables

### Backend Selection

| Variable | Values | Purpose |
|----------|--------|---------|
| `CPU_BACKEND` | uae, unicorn, dualcpu | Select CPU backend |

**Examples**:
```bash
CPU_BACKEND=uae      # Use UAE interpreter
CPU_BACKEND=unicorn  # Use Unicorn JIT (default/recommended)
CPU_BACKEND=dualcpu  # Run both in lockstep for validation
```

### Debugging & Tracing

| Variable | Values | Purpose |
|----------|--------|---------|
| `EMULATOR_TIMEOUT` | seconds | Auto-exit after N seconds |
| `CPU_TRACE` | N or N-M | Trace N instructions or range |
| `CPU_TRACE_MEMORY` | 0 or 1 | Include memory accesses in trace |
| `CPU_TRACE_QUIET` | 0 or 1 | Suppress normal output, trace only |
| `EMULOP_VERBOSE` | 0 or 1 | Log EmulOp calls |

**Examples**:
```bash
# Trace first 100 instructions
CPU_TRACE=0-100 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# Trace with memory accesses
CPU_TRACE=0-1000 CPU_TRACE_MEMORY=1 CPU_BACKEND=uae ./build/macemu-next ~/quadra.rom

# Trace specific range (quiet mode)
CPU_TRACE=29500-29600 CPU_TRACE_QUIET=1 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# Verbose EmulOp logging
EMULOP_VERBOSE=1 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom
```

### DualCPU Validation

| Variable | Values | Purpose |
|----------|--------|---------|
| `DUALCPU_TRACE_DEPTH` | N | History depth for divergence analysis |
| `DUALCPU_MASTER` | uae or unicorn | Which CPU is "correct" on divergence |

**Examples**:
```bash
# DualCPU with 10-instruction history on divergence
DUALCPU_TRACE_DEPTH=10 CPU_BACKEND=dualcpu ./build/macemu-next ~/quadra.rom

# Trust Unicorn as master (unusual)
DUALCPU_MASTER=unicorn CPU_BACKEND=dualcpu ./build/macemu-next ~/quadra.rom
```

---

## Trace Comparison

### Generate Traces

```bash
# Generate UAE trace
EMULATOR_TIMEOUT=2 CPU_TRACE=0-250000 CPU_BACKEND=uae \
    ./build/macemu-next ~/quadra.rom > uae_250k.log 2>&1

# Generate Unicorn trace
EMULATOR_TIMEOUT=2 CPU_TRACE=0-250000 CPU_BACKEND=unicorn \
    ./build/macemu-next ~/quadra.rom > unicorn_250k.log 2>&1
```

### Compare Traces (Manual)

```bash
# Find first difference
diff uae_250k.log unicorn_250k.log | head -50

# Count divergences
diff uae_250k.log unicorn_250k.log | grep '^<' | wc -l
```

### Compare Traces (Script)

```bash
# Using provided script (if exists)
./scripts/compare_traces.sh uae_250k.log unicorn_250k.log

# Using trace analyzer (if exists)
python3 scripts/trace_analyzer.py --sequential uae_250k.log unicorn_250k.log
```

### Run Traces Script (Wrapper)

```bash
# Simplified trace comparison (if run_traces.sh exists)
./scripts/run_traces.sh 250000  # Generate and compare 250k instruction traces
```

---

## Testing Commands

### Boot Test

```bash
# Simple boot test
./build/tests/boot/test_boot ~/quadra.rom

# With specific backend
CPU_BACKEND=unicorn ./build/tests/boot/test_boot ~/quadra.rom
```

### Unit Tests

```bash
# Run all Meson tests
meson test -C build

# Run specific test
meson test -C build test_unicorn_m68k
```

### Validation Test

```bash
# Run DualCPU for extended validation
EMULATOR_TIMEOUT=30 DUALCPU_TRACE_DEPTH=20 CPU_BACKEND=dualcpu \
    ./build/macemu-next ~/quadra.rom
```

Expected: Should validate 500k+ instructions before timeout

---

## Debug Commands

### Run with GDB

```bash
# Basic GDB
gdb --args ./build/macemu-next ~/quadra.rom

# With environment variables
gdb --args env CPU_BACKEND=unicorn EMULATOR_TIMEOUT=5 \
    ./build/macemu-next ~/quadra.rom

# GDB commands:
(gdb) run
(gdb) break unicorn_backend_execute_one
(gdb) continue
(gdb) print cpu->uc
(gdb) backtrace
```

### Trace Specific Instruction Range

```bash
# Find where Unicorn diverges from UAE
# 1. Run both with same range
EMULATOR_TIMEOUT=5 CPU_TRACE=29000-30000 CPU_BACKEND=uae \
    ./build/macemu-next ~/quadra.rom > uae_range.log

EMULATOR_TIMEOUT=5 CPU_TRACE=29000-30000 CPU_BACKEND=unicorn \
    ./build/macemu-next ~/quadra.rom > unicorn_range.log

# 2. Compare
diff uae_range.log unicorn_range.log | head -100
```

### Memory Dump

```bash
# If memory dump functionality exists
# Add memory inspection at specific PC values
CPU_TRACE=29500-29600 CPU_TRACE_MEMORY=1 CPU_BACKEND=unicorn \
    ./build/macemu-next ~/quadra.rom
```

---

## Common Workflows

### Workflow 1: Test a Change

```bash
# 1. Build
meson compile -C build

# 2. Quick test (5 seconds)
EMULATOR_TIMEOUT=5 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# 3. Validation test (30 seconds)
EMULATOR_TIMEOUT=30 CPU_BACKEND=dualcpu ./build/macemu-next ~/quadra.rom

# 4. If validation passes, change is likely correct
```

### Workflow 2: Investigate Divergence

```bash
# 1. Run DualCPU to find divergence point
EMULATOR_TIMEOUT=10 DUALCPU_TRACE_DEPTH=20 CPU_BACKEND=dualcpu \
    ./build/macemu-next ~/quadra.rom 2>&1 | tee divergence.log

# 2. Extract divergence instruction number (e.g., 29518)
grep "DIVERGENCE" divergence.log

# 3. Generate detailed traces around divergence
CPU_TRACE=29500-29600 CPU_TRACE_MEMORY=1 CPU_BACKEND=uae \
    ./build/macemu-next ~/quadra.rom > uae_detail.log

CPU_TRACE=29500-29600 CPU_TRACE_MEMORY=1 CPU_BACKEND=unicorn \
    ./build/macemu-next ~/quadra.rom > unicorn_detail.log

# 4. Analyze difference
diff uae_detail.log unicorn_detail.log
```

### Workflow 3: Performance Testing

```bash
# 1. Build release version
meson setup build-release --buildtype=release
meson compile -C build-release

# 2. Run timed tests
time EMULATOR_TIMEOUT=60 CPU_BACKEND=uae \
    ./build-release/macemu-next ~/quadra.rom

time EMULATOR_TIMEOUT=60 CPU_BACKEND=unicorn \
    ./build-release/macemu-next ~/quadra.rom

# 3. Compare execution times
```

### Workflow 4: Trace Comparison

```bash
# 1. Generate traces (same timeout, different backends)
EMULATOR_TIMEOUT=2 CPU_TRACE=0-250000 CPU_BACKEND=uae \
    ./build/macemu-next ~/quadra.rom > uae.log 2>&1

EMULATOR_TIMEOUT=2 CPU_TRACE=0-250000 CPU_BACKEND=unicorn \
    ./build/macemu-next ~/quadra.rom > unicorn.log 2>&1

# 2. Find first divergence
./scripts/compare_traces.sh uae.log unicorn.log

# 3. Analyze divergence in detail (from step 2 output)
```

---

## Script Reference

### compare_traces.sh (if exists)

```bash
./scripts/compare_traces.sh <uae_log> <unicorn_log>
```

Finds first divergence between UAE and Unicorn traces.

### trace_analyzer.py (if exists)

```bash
# Sequential comparison (find exact divergence point)
python3 scripts/trace_analyzer.py --sequential uae.log unicorn.log

# Statistical analysis
python3 scripts/trace_analyzer.py --stats uae.log unicorn.log
```

### run_traces.sh (if exists)

```bash
# Generate and compare N instruction traces
./scripts/run_traces.sh 250000

# Custom timeout
EMULATOR_TIMEOUT=5 ./scripts/run_traces.sh 100000
```

---

## Troubleshooting

### Build Issues

**Problem**: Meson setup fails
```bash
# Check Meson version
meson --version  # Should be 0.55+

# Check dependencies
pkg-config --libs glib-2.0
```

**Problem**: Unicorn not found
```bash
# Update submodules
git submodule update --init --recursive

# Build Unicorn manually
cd external/unicorn
mkdir build && cd build
cmake .. && make
```

### Runtime Issues

**Problem**: "ROM file not found"
```bash
# Check ROM path
ls -lh ~/quadra.rom

# Try absolute path
./build/macemu-next /home/user/quadra.rom
```

**Problem**: Segmentation fault
```bash
# Run with GDB
gdb --args ./build/macemu-next ~/quadra.rom
(gdb) run
(gdb) backtrace  # When it crashes
```

**Problem**: DualCPU divergence immediately
```bash
# Check if both backends built correctly
meson compile -C build

# Enable verbose output
EMULOP_VERBOSE=1 DUALCPU_TRACE_DEPTH=5 CPU_BACKEND=dualcpu \
    ./build/macemu-next ~/quadra.rom
```

---

## Quick Reference

```bash
# Standard test run
EMULATOR_TIMEOUT=5 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# Validation run
EMULATOR_TIMEOUT=30 CPU_BACKEND=dualcpu ./build/macemu-next ~/quadra.rom

# Trace comparison
EMULATOR_TIMEOUT=2 CPU_TRACE=0-250000 CPU_BACKEND=uae ./build/macemu-next ~/quadra.rom > uae.log
EMULATOR_TIMEOUT=2 CPU_TRACE=0-250000 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom > uni.log
diff uae.log uni.log | head -50

# Debug specific range
CPU_TRACE=29500-29600 CPU_TRACE_MEMORY=1 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# Performance test
time EMULATOR_TIMEOUT=60 CPU_BACKEND=unicorn ./build-release/macemu-next ~/quadra.rom
```

---

**Last Updated**: January 3, 2026
**See Also**: [Architecture.md](Architecture.md), [TodoStatus.md](TodoStatus.md)

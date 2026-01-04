# CPU Trace Analysis Scripts

This directory contains tools for debugging and analyzing CPU execution divergence between UAE and Unicorn backends.

## Quick Start

### 1. Run Comprehensive Traces

```bash
cd /home/mick/macemu-dual-cpu/macemu-next/scripts
./run_traces.sh
```

This will:
- Run UAE, Unicorn, and DualCPU backends for 250k instructions
- Create timestamped output directory in `/tmp/macemu_traces_YYYYMMDD_HHMMSS/`
- Generate full logs and extracted trace files
- Show summary of divergence

**Output files:**
```
/tmp/macemu_traces_20260104_123456/
├── uae_full.log       - Complete UAE output (includes debug messages)
├── unicorn_full.log   - Complete Unicorn output
├── dualcpu_full.log   - Complete DualCPU output
├── uae_trace.txt      - UAE trace lines only ([NNNNN] format)
├── unicorn_trace.txt  - Unicorn trace lines only
└── dualcpu_trace.txt  - DualCPU trace lines only
```

### 2. Analyze Divergence

```bash
./trace_analyzer.py /tmp/macemu_traces_*/uae_trace.txt /tmp/macemu_traces_*/unicorn_trace.txt
```

**Features:**
- Side-by-side disassembly comparison
- Event extraction (interrupts triggered/taken, EmulOps)
- Context display around divergence points
- Highlighting of register differences

**Example output:**
```
════════════════════════════════════════════════════════════════════
 EVENT SUMMARY
════════════════════════════════════════════════════════════════════

Trace 1 (UAE):
  Interrupts Triggered: 0
  Interrupts Taken:     2
  EmulOps:              0

Trace 2 (Unicorn):
  Interrupts Triggered: 41
  Interrupts Taken:     1
  EmulOps:              0

════════════════════════════════════════════════════════════════════
 SIDE-BY-SIDE TRACE COMPARISON
════════════════════════════════════════════════════════════════════

────────────────────────────────────────────────────────────────────
 DIVERGENCE #1 at instruction index 3832
────────────────────────────────────────────────────────────────────

     [03831]
     T2: >>> INTERRUPT TRIGGERED: level=1 <<<

>>>> [03832]
     T1: !0208113A 102D | moveb %a5@(-13),%d0  | SR: 2704
     T2: !02081138 7000 | moveq #0,%d0         | SR: 2700
         D0-D7:
         T1: 00000005 ...
         T2: 00000000 ...
             DIFF: D0: 00000005 vs 00000000
```

## Scripts

### run_traces.sh

**Purpose**: Automated trace collection from all three backends

**Usage:**
```bash
./run_traces.sh [instruction_count] [rom_path] [timeout]
```

**Arguments:**
- `instruction_count`: Number of instructions to trace (default: 250000)
- `rom_path`: Path to ROM file (default: ~/quadra.rom)
- `timeout`: Emulator timeout in seconds (default: 10)

**Examples:**
```bash
# Default: 250k instructions, 10 second timeout
./run_traces.sh

# Custom: 100k instructions, 5 second timeout
./run_traces.sh 100000 ~/quadra.rom 5

# Quick test: 10k instructions
./run_traces.sh 10000
```

### trace_analyzer.py

**Purpose**: Side-by-side comparison of CPU traces with disassembly

**Usage:**
```bash
./trace_analyzer.py TRACE1 TRACE2 [OPTIONS]
```

**Required Arguments:**
- `TRACE1`: First trace file (typically UAE)
- `TRACE2`: Second trace file (typically Unicorn)

**Optional Arguments:**
- `--rom PATH`: Path to ROM file for disassembly (default: ~/quadra.rom)
- `--context N`: Number of instructions to show around divergence (default: 10)
- `--max-divergences N`: Maximum divergences to display (default: 5)
- `--rom-base ADDR`: ROM base address in hex (default: 0x02000000)

**Examples:**
```bash
# Basic comparison
./trace_analyzer.py uae.log unicorn.log

# More context, custom ROM
./trace_analyzer.py uae.log unicorn.log --context 20 --rom ~/custom.rom

# Show more divergences
./trace_analyzer.py uae.log unicorn.log --max-divergences 10
```

**Requirements:**
- Python 3
- `m68k-linux-gnu-objdump` for disassembly (install: `sudo apt-get install binutils-m68k-linux-gnu`)

## Trace Format

### Instruction Traces

Standard format:
```
[NNNNN] PC OPCODE | D0-D7 | A0-A7 | SR FLAGS
```

Example:
```
[03832] 0208113A 102D | 00000005 00000000 ... | 0200DCE4 00001EA0 ... | 2704 00100
```

### Event Markers

**Interrupt Triggered:**
```
[NNNNN] @@INTR_TRIG level
```
Example: `[03831] @@INTR_TRIG 1`

**Interrupt Taken:**
```
[NNNNN] @@INTR_TAKE level handler_addr
```
Example: `[175169] @@INTR_TAKE 1 02009B60`

**EmulOp Executed:**
```
[NNNNN] @@EMULOP opcode
```
Example: `[12345] @@EMULOP 7103`

## Environment Variables

### For Emulator

- `CPU_BACKEND`: Select backend (`uae`, `unicorn`, `dualcpu`)
- `CPU_TRACE`: Trace range (`N` or `start-end`)
- `CPU_TRACE_MEMORY`: Enable memory access tracing (set to `1`)
- `CPU_TRACE_QUIET`: Suppress banner messages (set to `1`)
- `EMULATOR_TIMEOUT`: Timeout in seconds

**Examples:**
```bash
# Trace first 1000 instructions with Unicorn
CPU_BACKEND=unicorn CPU_TRACE=0-1000 ./build/macemu-next ~/quadra.rom

# Trace with memory access logging
CPU_TRACE=0-5000 CPU_TRACE_MEMORY=1 CPU_BACKEND=uae ./build/macemu-next ~/quadra.rom

# DualCPU mode with custom trace depth
CPU_BACKEND=dualcpu DUALCPU_TRACE_DEPTH=50 ./build/macemu-next ~/quadra.rom
```

## Workflow Example

### Finding and Analyzing Divergence

1. **Run full traces:**
   ```bash
   cd macemu-next/scripts
   ./run_traces.sh
   ```

2. **Note the output directory:**
   ```
   Output: /tmp/macemu_traces_20260104_123456
   ```

3. **Analyze divergence:**
   ```bash
   ./trace_analyzer.py \
       /tmp/macemu_traces_20260104_123456/uae_trace.txt \
       /tmp/macemu_traces_20260104_123456/unicorn_trace.txt
   ```

4. **Narrow down to specific range:**
   ```bash
   # If divergence found at instruction 3832, zoom in:
   ./run_traces.sh 5000  # Just 5k instructions
   ./trace_analyzer.py /tmp/macemu_traces_*/uae_trace.txt \
                       /tmp/macemu_traces_*/unicorn_trace.txt \
                       --context 20
   ```

5. **Check event summary:**
   Look for interrupt timing differences in the "EVENT SUMMARY" section

## Known Issues

### Current Divergence

**Status:** CRITICAL BUG IDENTIFIED

**Location:** Instruction #3832

**Cause:** Unicorn's interrupt handling in `hook_block()` calls `uc_emu_stop()` which causes instruction skipping when interrupts are triggered mid-block.

**Evidence:**
- Interrupt triggered at #3831 (Unicorn only)
- At #3832, Unicorn executes wrong instruction (PC off by 2 bytes)
- D0 register diverges (0x05 vs 0x00)
- Cascade leads to crash at PC 0x02009B88

**Documentation:** See [../docs/DIVERGENCE_ROOT_CAUSE.md](../docs/DIVERGENCE_ROOT_CAUSE.md)

**Fix Status:** Pending - need to handle interrupts inline without `uc_emu_stop()`

## Contributing

When adding new diagnostic features:

1. Use the `@@EVENT_TYPE data` format for parseable events
2. Update `trace_analyzer.py` to parse new events
3. Update this README with examples
4. Test with both small (1k) and large (250k) traces

## See Also

- [Phase 1 Implementation Plan](../docs/Phase1Implementation.md)
- [Todo Status](../docs/TodoStatus.md)
- [Divergence Root Cause Analysis](../docs/DIVERGENCE_ROOT_CAUSE.md)
- [Interrupt Timing Analysis](../docs/deepdive/InterruptTimingAnalysis.md)

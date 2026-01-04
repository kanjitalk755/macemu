# Quick Start: CPU Trace Analysis

## TL;DR - Run This

```bash
# From macemu-next/scripts directory
cd /home/mick/macemu-dual-cpu/macemu-next/scripts

# 1. Collect traces (5 seconds, 250k instructions)
./run_traces.sh

# 2. Analyze divergence (uses output from step 1)
./trace_analyzer.py /tmp/macemu_traces_*/uae_trace.txt \
                    /tmp/macemu_traces_*/unicorn_trace.txt
```

## What You'll See

### Step 1 Output
```
════════════════════════════════════════════════════════════════
  macemu-next CPU Trace Runner
════════════════════════════════════════════════════════════════
Binary:      .../build/macemu-next
ROM:         ~/quadra.rom
Instructions: 250000 (range: 0-250000)
Timeout:     10s
Output:      /tmp/macemu_traces_20260104_123456
════════════════════════════════════════════════════════════════

Running UAE...     ✓ 250002 instructions
Running Unicorn... ✓ 144016 instructions
Running DualCPU... ✓ 216704 instructions

⚠ DIVERGENCE DETECTED
```

### Step 2 Output
```
════════════════════════════════════════════════════════════════
 EVENT SUMMARY
════════════════════════════════════════════════════════════════

Trace 1 (UAE):
  Interrupts Triggered: 0
  Interrupts Taken:     2

Trace 2 (Unicorn):
  Interrupts Triggered: 41  ⚠️ WAY MORE!
  Interrupts Taken:     1

════════════════════════════════════════════════════════════════
 SIDE-BY-SIDE COMPARISON
════════════════════════════════════════════════════════════════

[03831]
T2: >>> INTERRUPT TRIGGERED: level=1 <<<

>>>> [03832] ⚠️ FIRST DIVERGENCE
T1: 0208113A 102D | moveb %a5@(-13),%d0  | D0=00000005
T2: 02081138 7000 | moveq #0,%d0         | D0=00000000  ⚠️ WRONG!
                                                         ⚠️ PC off by 2!
```

## What This Means

**The Bug**: When Unicorn triggers an interrupt, it skips the next instruction.

**Why**: The `hook_block()` function calls `uc_emu_stop()` when an interrupt is pending, which loses sync with the instruction stream.

**Impact**:
- Wrong instruction executed
- Registers diverge
- Eventually crashes at PC 0x02009B88

## Next Steps

See [README.md](README.md) for:
- Detailed usage
- Command-line options
- Environment variables
- Workflow examples

See [../docs/DIVERGENCE_ROOT_CAUSE.md](../docs/DIVERGENCE_ROOT_CAUSE.md) for:
- Complete root cause analysis
- Proposed fix strategies
- Implementation plan

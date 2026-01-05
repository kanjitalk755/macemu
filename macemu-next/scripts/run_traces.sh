#!/bin/bash
# Comprehensive CPU trace runner for macemu-next
# Runs UAE, Unicorn, and DualCPU backends with configurable traces
#
# Usage: ./run_traces.sh [instruction_count] [rom_path] [timeout]
#   instruction_count: Number of instructions to trace (default: 250000)
#   rom_path: Path to ROM file (default: ~/quadra.rom)
#   timeout: Emulator timeout in seconds (default: 10)
#
# Examples:
#   ./run_traces.sh                    # 250k instructions, 10 sec timeout
#   ./run_traces.sh 100000 ~/rom.bin 5 # 100k instructions, 5 sec timeout
#
# Output: Creates timestamped directory with trace logs and analysis

set -e

# Find script directory and macemu-next root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MACEMU_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$MACEMU_ROOT/build"
MACEMU_BIN="$BUILD_DIR/macemu-next"
CONFIG_FILE="$SCRIPT_DIR/trace-config.json"

# Check if binary exists
if [ ! -f "$MACEMU_BIN" ]; then
    echo "Error: macemu-next binary not found at $MACEMU_BIN"
    echo "Please build first: cd $MACEMU_ROOT && meson compile -C build"
    exit 1
fi

# Check if config exists
if [ ! -f "$CONFIG_FILE" ]; then
    echo "Error: Config file not found at $CONFIG_FILE"
    exit 1
fi

# Parse arguments
INSN_COUNT="${1:-250000}"
ROM="${2:-$HOME/quadra.rom}"
TIMEOUT="${3:-10}"
TRACE_RANGE="0-$INSN_COUNT"

# Check ROM exists
if [ ! -f "$ROM" ]; then
    echo "Error: ROM file not found: $ROM"
    exit 1
fi

# Create output directory
OUTDIR="/tmp/macemu_traces_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTDIR"

echo "════════════════════════════════════════════════════════════════"
echo "  macemu-next CPU Trace Runner"
echo "════════════════════════════════════════════════════════════════"
echo "Binary:      $MACEMU_BIN"
echo "Config:      $CONFIG_FILE"
echo "ROM:         $ROM"
echo "Instructions: $INSN_COUNT (range: $TRACE_RANGE)"
echo "Timeout:     ${TIMEOUT}s"
echo "Output:      $OUTDIR"
echo "════════════════════════════════════════════════════════════════"
echo ""

# Enable core dumps
ulimit -c unlimited 2>/dev/null || true

# ============================================================================
# Step 1: Run UAE backend
# ============================================================================
echo "┌────────────────────────────────────────────────────────────────┐"
echo "│ Step 1/3: Running UAE (interpreter baseline)                  │"
echo "└────────────────────────────────────────────────────────────────┘"

env EMULATOR_TIMEOUT=$TIMEOUT CPU_TRACE="$TRACE_RANGE" CPU_BACKEND=uae \
    "$MACEMU_BIN" --config "$CONFIG_FILE" "$ROM" > "$OUTDIR/uae_full.log" 2>&1
UAE_EXIT=$?

# Extract just trace lines
grep '^\[' "$OUTDIR/uae_full.log" > "$OUTDIR/uae_trace.txt" || true

UAE_COUNT=$(wc -l < "$OUTDIR/uae_trace.txt")

echo "  ✓ UAE completed (exit code: $UAE_EXIT)"
echo "    Instructions logged: $UAE_COUNT"
echo ""

# ============================================================================
# Step 2: Run Unicorn backend
# ============================================================================
echo "┌────────────────────────────────────────────────────────────────┐"
echo "│ Step 2/3: Running Unicorn (JIT target)                        │"
echo "└────────────────────────────────────────────────────────────────┘"

env EMULATOR_TIMEOUT=$TIMEOUT CPU_TRACE="$TRACE_RANGE" CPU_BACKEND=unicorn \
    "$MACEMU_BIN" --config "$CONFIG_FILE" "$ROM" > "$OUTDIR/unicorn_full.log" 2>&1
UNICORN_EXIT=$?

# Extract just trace lines
grep '^\[' "$OUTDIR/unicorn_full.log" > "$OUTDIR/unicorn_trace.txt" || true

UC_COUNT=$(wc -l < "$OUTDIR/unicorn_trace.txt")

echo "  ✓ Unicorn completed (exit code: $UNICORN_EXIT)"
echo "    Instructions logged: $UC_COUNT"
echo ""

# ============================================================================
# Step 3: Run DualCPU backend (if available)
# ============================================================================
echo "┌────────────────────────────────────────────────────────────────┐"
echo "│ Step 3/3: Running DualCPU (lockstep validation)               │"
echo "└────────────────────────────────────────────────────────────────┘"

env EMULATOR_TIMEOUT=$TIMEOUT DUALCPU_TRACE_DEPTH=20 CPU_BACKEND=dualcpu \
    "$MACEMU_BIN" --config "$CONFIG_FILE" "$ROM" > "$OUTDIR/dualcpu_full.log" 2>&1 || true
DUALCPU_EXIT=$?

# Extract just trace lines
grep '^\[' "$OUTDIR/dualcpu_full.log" > "$OUTDIR/dualcpu_trace.txt" || true

DC_COUNT=$(wc -l < "$OUTDIR/dualcpu_trace.txt")

echo "  ✓ DualCPU completed (exit code: $DUALCPU_EXIT)"
echo "    Instructions logged: $DC_COUNT"
echo ""

# ============================================================================
# Step 4: Summary
# ============================================================================
echo "════════════════════════════════════════════════════════════════"
echo "  Summary"
echo "════════════════════════════════════════════════════════════════"
echo ""

echo "Trace Statistics:"
echo "  UAE:     $UAE_COUNT instructions (exit: $UAE_EXIT)"
echo "  Unicorn: $UC_COUNT instructions (exit: $UNICORN_EXIT)"
echo "  DualCPU: $DC_COUNT instructions (exit: $DUALCPU_EXIT)"
echo ""

echo "Output Files:"
echo "  $OUTDIR/uae_full.log       - Full UAE output"
echo "  $OUTDIR/unicorn_full.log   - Full Unicorn output"
echo "  $OUTDIR/dualcpu_full.log   - Full DualCPU output"
echo "  $OUTDIR/uae_trace.txt      - UAE trace lines only"
echo "  $OUTDIR/unicorn_trace.txt  - Unicorn trace lines only"
echo "  $OUTDIR/dualcpu_trace.txt  - DualCPU trace lines only"
echo ""

# Check for divergence
if [ $UAE_COUNT -ne $UC_COUNT ]; then
    echo "⚠ DIVERGENCE DETECTED:"
    echo "  UAE and Unicorn traces have different lengths"
    echo "  UAE: $UAE_COUNT, Unicorn: $UC_COUNT"
    echo ""
fi

echo "Next Steps:"
echo "  1. Analyze traces with trace_analyzer.py:"
echo "     cd $SCRIPT_DIR"
echo "     ./trace_analyzer.py $OUTDIR/uae_trace.txt $OUTDIR/unicorn_trace.txt"
echo ""
echo "  2. Check full logs for errors:"
echo "     less $OUTDIR/uae_full.log"
echo "     less $OUTDIR/unicorn_full.log"
echo ""

if [ -f core ]; then
    echo "Core dump available: core"
    echo "  Debug with: gdb $MACEMU_BIN core"
    echo ""
fi

echo "════════════════════════════════════════════════════════════════"

#!/usr/bin/env python3
"""
Enhanced Trace Analyzer - Side-by-side comparison with context and event extraction

Usage:
    ./trace_analyzer_v2.py trace1.log trace2.log [--rom ROM_PATH] [--context N]

Shows divergence with disassembly, events (interrupts, EmulOps), and side-by-side view
"""

import re
import subprocess
import sys
import argparse
import os
from collections import defaultdict

class M68kDisassembler:
    """Disassemble M68K ROM using objdump"""

    def __init__(self, rom_path, rom_base=0x02000000):
        self.rom_path = rom_path
        self.rom_base = rom_base
        self.disasm_cache = {}
        if os.path.exists(rom_path):
            self._load_disassembly()

    def _load_disassembly(self):
        """Pre-load entire ROM disassembly for fast lookups"""
        print(f"Disassembling ROM from {self.rom_path}...", file=sys.stderr)

        cmd = [
            'm68k-linux-gnu-objdump',
            '-D',
            '-b', 'binary',
            '-m', 'm68k:68040',
            f'--adjust-vma={hex(self.rom_base)}',
            self.rom_path
        ]

        try:
            result = subprocess.run(cmd, capture_output=True, text=True, check=True)

            for line in result.stdout.splitlines():
                match = re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(.+)$', line)
                if match:
                    addr = int(match.group(1), 16)
                    opcode = match.group(2)
                    disasm = match.group(3).strip()

                    if '\t' in disasm:
                        disasm = disasm.split('\t', 1)[1]

                    self.disasm_cache[addr] = {
                        'opcode': opcode,
                        'disasm': disasm
                    }

            print(f"Loaded {len(self.disasm_cache)} instructions from ROM", file=sys.stderr)

        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            print(f"Warning: Could not load ROM disassembly: {e}", file=sys.stderr)

    def get_instruction(self, pc):
        """Get disassembled instruction for a given PC"""
        if pc in self.disasm_cache:
            return self.disasm_cache[pc]['disasm']
        return f"<unknown at {pc:08X}>"


class TraceEntry:
    """Represents a single trace line"""

    def __init__(self, line):
        self.raw = line.strip()
        self.inst_num = -1
        self.pc = 0
        self.opcode = "????"
        self.d_regs = []
        self.a_regs = []
        self.sr = "????"
        self.valid = False
        self.event_type = None  # "INTR_TRIG", "INTR_TAKE", "EMULOP", or None
        self.event_data = {}
        self.parse()

    def parse(self):
        """Parse trace line - instruction or event"""

        # Check for event markers
        if '@@INTR_TRIG' in self.raw:
            # Format: [count] @@INTR_TRIG level
            match = re.match(r'\[(\d+)\]\s+@@INTR_TRIG\s+(\d+)', self.raw)
            if match:
                self.inst_num = int(match.group(1))
                self.event_type = 'INTR_TRIG'
                self.event_data = {'level': int(match.group(2))}
                self.valid = True
            return

        if '@@INTR_TAKE' in self.raw:
            # Format: [count] @@INTR_TAKE level handler
            match = re.match(r'\[(\d+)\]\s+@@INTR_TAKE\s+(\d+)\s+([0-9A-Fa-f]+)', self.raw)
            if match:
                self.inst_num = int(match.group(1))
                self.event_type = 'INTR_TAKE'
                self.event_data = {
                    'level': int(match.group(2)),
                    'handler': int(match.group(3), 16)
                }
                self.valid = True
            return

        if '@@EMULOP' in self.raw:
            # Format: [count] @@EMULOP opcode
            match = re.match(r'\[(\d+)\]\s+@@EMULOP\s+([0-9A-Fa-f]+)', self.raw)
            if match:
                self.inst_num = int(match.group(1))
                self.event_type = 'EMULOP'
                self.event_data = {'opcode': int(match.group(2), 16)}
                self.valid = True
            return

        # Parse instruction: [NNNNN] PC OPCODE | D0-D7 | A0-A7 | SR
        match = re.match(r'\[(\d+)\]\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{4})\s+\|(.*)$', self.raw)

        if match:
            self.valid = True
            self.inst_num = int(match.group(1))
            self.pc = int(match.group(2), 16)
            self.opcode = match.group(3)
            self.rest = match.group(4).strip()

            parts = [p.strip() for p in self.rest.split('|')]
            if len(parts) >= 3:
                self.d_regs = parts[0].split()[:8]
                self.a_regs = parts[1].split()[:8]
                self.sr = parts[2].split()[0] if parts[2].split() else "????"

    def is_event(self):
        return self.event_type is not None

    def event_str(self):
        if self.event_type == 'INTR_TRIG':
            return f">>> INTERRUPT TRIGGERED: level={self.event_data['level']} <<<"
        elif self.event_type == 'INTR_TAKE':
            return f">>> INTERRUPT TAKEN: level={self.event_data['level']}, handler=0x{self.event_data['handler']:08X} <<<"
        elif self.event_type == 'EMULOP':
            return f">>> EMULOP: 0x{self.event_data['opcode']:04X} <<<"
        return ""

    def __str__(self):
        return self.raw


def read_trace(filename):
    """Read trace file and return list of TraceEntry objects"""
    entries = []

    with open(filename, 'r') as f:
        for line in f:
            if line.startswith('['):
                entry = TraceEntry(line)
                if entry.valid:
                    entries.append(entry)

    return entries


def compare_side_by_side(trace1, trace2, disassembler, context=10, max_divergences=5):
    """Compare traces side-by-side with context around divergences"""

    print("\n" + "=" * 140)
    print(" SIDE-BY-SIDE TRACE COMPARISON WITH DISASSEMBLY")
    print("=" * 140)
    print(f"Trace 1: {len(trace1)} entries")
    print(f"Trace 2: {len(trace2)} entries")
    print("=" * 140 + "\n")

    # Separate instructions from events for proper alignment
    # Events are kept with their instruction index for display purposes
    trace1_instrs = [(i, e) for i, e in enumerate(trace1) if not e.is_event()]
    trace2_instrs = [(i, e) for i, e in enumerate(trace2) if not e.is_event()]

    print(f"Trace 1: {len(trace1_instrs)} instructions (excluding events)")
    print(f"Trace 2: {len(trace2_instrs)} instructions (excluding events)")
    print()

    min_len = min(len(trace1_instrs), len(trace2_instrs))
    divergence_count = 0
    divergence_points = []

    # Find all divergence points (comparing instructions only, ignoring events)
    for i in range(min_len):
        idx1, e1 = trace1_instrs[i]
        idx2, e2 = trace2_instrs[i]

        # Check if instructions differ
        if e1.pc != e2.pc or e1.d_regs != e2.d_regs or e1.a_regs != e2.a_regs or e1.sr != e2.sr:
            divergence_points.append(i)
            if len(divergence_points) >= max_divergences:
                break

    if not divergence_points:
        print("✓ NO DIVERGENCE FOUND - Traces match perfectly!\n")
        return

    # Show each divergence with context
    for div_idx, div_point in enumerate(divergence_points, 1):
        idx1, e1_div = trace1_instrs[div_point]
        idx2, e2_div = trace2_instrs[div_point]

        print(f"\n{'─' * 140}")
        print(f" DIVERGENCE #{div_idx} at instruction index {div_point} (inst #{e1_div.inst_num})")
        print(f"{'─' * 140}\n")

        # Show context before
        start = max(0, div_point - context)
        end = min(min_len, div_point + context + 1)

        for i in range(start, end):
            idx1, e1 = trace1_instrs[i]
            idx2, e2 = trace2_instrs[i]

            marker = ">>>>" if i == div_point else "    "

            # Check if there are events between this instruction and the previous one
            # Show events from trace1
            if i > 0:
                prev_idx1 = trace1_instrs[i-1][0]
                for event_idx in range(prev_idx1 + 1, idx1):
                    if event_idx < len(trace1) and trace1[event_idx].is_event():
                        print(f"     [{trace1[event_idx].inst_num:05d}]")
                        print(f"     T1: {trace1[event_idx].event_str()}")
                        print()

            # Show events from trace2
            if i > 0:
                prev_idx2 = trace2_instrs[i-1][0]
                for event_idx in range(prev_idx2 + 1, idx2):
                    if event_idx < len(trace2) and trace2[event_idx].is_event():
                        print(f"     [{trace2[event_idx].inst_num:05d}]")
                        print(f"     T2: {trace2[event_idx].event_str()}")
                        print()

            # Get disassembly
            disasm1 = disassembler.get_instruction(e1.pc)
            disasm2 = disassembler.get_instruction(e2.pc)

            # Check what differs
            pc_diff = e1.pc != e2.pc
            op_diff = e1.opcode != e2.opcode
            d_diff = e1.d_regs != e2.d_regs
            a_diff = e1.a_regs != e2.a_regs
            sr_diff = e1.sr != e2.sr

            # Format instruction display
            print(f"{marker} [{e1.inst_num:05d}]")

            # Show PC and disassembly
            pc_marker = "!" if pc_diff else " "
            print(f"     T1: {pc_marker}{e1.pc:08X} {e1.opcode} | {disasm1:40s} | SR: {e1.sr}")
            print(f"     T2: {pc_marker}{e2.pc:08X} {e2.opcode} | {disasm2:40s} | SR: {e2.sr}")

            # Show differing registers
            if d_diff:
                print(f"         D0-D7:")
                d_str1 = ' '.join(e1.d_regs[:8])
                d_str2 = ' '.join(e2.d_regs[:8])
                print(f"         T1: {d_str1}")
                print(f"         T2: {d_str2}")

                # Highlight which D regs differ
                diff_regs = []
                for j in range(min(len(e1.d_regs), len(e2.d_regs))):
                    if e1.d_regs[j] != e2.d_regs[j]:
                        diff_regs.append(f"D{j}: {e1.d_regs[j]} vs {e2.d_regs[j]}")
                if diff_regs:
                    print(f"             DIFF: {', '.join(diff_regs)}")

            if a_diff:
                print(f"         A0-A7:")
                a_str1 = ' '.join(e1.a_regs[:8])
                a_str2 = ' '.join(e2.a_regs[:8])
                print(f"         T1: {a_str1}")
                print(f"         T2: {a_str2}")

                # Highlight which A regs differ
                diff_regs = []
                for j in range(min(len(e1.a_regs), len(e2.a_regs))):
                    if e1.a_regs[j] != e2.a_regs[j]:
                        diff_regs.append(f"A{j}: {e1.a_regs[j]} vs {e2.a_regs[j]}")
                if diff_regs:
                    print(f"             DIFF: {', '.join(diff_regs)}")

            print()

    print(f"\n{'=' * 140}")
    print(f"Total divergences found: {len(divergence_points)}")
    if len(divergence_points) >= max_divergences:
        print(f"(Showing first {max_divergences}, use --max-divergences to see more)")
    print("=" * 140 + "\n")


def extract_events(trace):
    """Extract interrupt and EmulOp events from trace"""
    events = {
        'interrupts_triggered': [],
        'interrupts_taken': [],
        'emulops': []
    }

    for entry in trace:
        if entry.event_type == 'INTR_TRIG':
            events['interrupts_triggered'].append({
                'inst_num': entry.inst_num,
                'level': entry.event_data['level']
            })
        elif entry.event_type == 'INTR_TAKE':
            events['interrupts_taken'].append({
                'inst_num': entry.inst_num,
                'level': entry.event_data['level'],
                'handler': entry.event_data['handler']
            })
        elif entry.event_type == 'EMULOP':
            events['emulops'].append({
                'inst_num': entry.inst_num,
                'opcode': entry.event_data['opcode']
            })

    return events


def print_event_summary(trace1, trace2):
    """Print summary of events in both traces"""
    events1 = extract_events(trace1)
    events2 = extract_events(trace2)

    print("\n" + "=" * 140)
    print(" EVENT SUMMARY")
    print("=" * 140)

    print(f"\nTrace 1:")
    print(f"  Interrupts Triggered: {len(events1['interrupts_triggered'])}")
    print(f"  Interrupts Taken:     {len(events1['interrupts_taken'])}")
    print(f"  EmulOps:              {len(events1['emulops'])}")

    print(f"\nTrace 2:")
    print(f"  Interrupts Triggered: {len(events2['interrupts_triggered'])}")
    print(f"  Interrupts Taken:     {len(events2['interrupts_taken'])}")
    print(f"  EmulOps:              {len(events2['emulops'])}")

    # Show first few interrupts taken
    print("\nFirst Interrupts Taken:")
    print(f"  Trace 1: {events1['interrupts_taken'][:5]}")
    print(f"  Trace 2: {events2['interrupts_taken'][:5]}")

    print("=" * 140 + "\n")


def main():
    parser = argparse.ArgumentParser(
        description='Enhanced trace analyzer with side-by-side comparison',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )

    parser.add_argument('trace1', help='First trace file (e.g., uae_250k.log)')
    parser.add_argument('trace2', help='Second trace file (e.g., unicorn_250k.log)')
    parser.add_argument('--rom', default=os.path.expanduser('~/quadra.rom'),
                       help='Path to ROM file (default: ~/quadra.rom)')
    parser.add_argument('--context', type=int, default=10,
                       help='Context lines around divergence (default: 10)')
    parser.add_argument('--max-divergences', type=int, default=5,
                       help='Maximum divergences to show (default: 5)')
    parser.add_argument('--rom-base', type=lambda x: int(x, 0), default=0x02000000,
                       help='ROM base address (default: 0x02000000)')

    args = parser.parse_args()

    # Load ROM disassembly
    disassembler = M68kDisassembler(args.rom, args.rom_base)

    # Load traces
    print(f"Reading trace 1 from {args.trace1}...", file=sys.stderr)
    trace1 = read_trace(args.trace1)
    print(f"Loaded {len(trace1)} entries from trace 1", file=sys.stderr)

    print(f"Reading trace 2 from {args.trace2}...", file=sys.stderr)
    trace2 = read_trace(args.trace2)
    print(f"Loaded {len(trace2)} entries from trace 2", file=sys.stderr)

    # Print event summary
    print_event_summary(trace1, trace2)

    # Compare side-by-side
    compare_side_by_side(trace1, trace2, disassembler, args.context, args.max_divergences)


if __name__ == '__main__':
    main()

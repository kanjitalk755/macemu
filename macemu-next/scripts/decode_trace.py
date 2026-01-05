#!/usr/bin/env python3
"""
Decode m68k instructions in CPU trace files using objdump.
Usage: ./decode_trace.py <trace_file> [start_line] [end_line]
"""
import sys
import re
import subprocess
import tempfile

def disassemble_opcode(pc, opcode):
    """Use m68k-linux-gnu-objdump to disassemble a single opcode."""
    # Create a binary file with the opcode
    opcode_bytes = bytes([
        (opcode >> 8) & 0xFF,
        opcode & 0xFF
    ])

    with tempfile.NamedTemporaryFile(delete=False, suffix='.bin') as f:
        f.write(opcode_bytes)
        temp_file = f.name

    try:
        # Disassemble using objdump
        result = subprocess.run(
            ['m68k-linux-gnu-objdump', '-D', '-b', 'binary', '-m', 'm68k',
             f'--adjust-vma={pc}', temp_file],
            capture_output=True,
            text=True,
            timeout=1
        )

        # Parse output - look for the disassembled instruction
        for line in result.stdout.split('\n'):
            # Format is like: "  2009a80:	5041           	addqw #8,%d1"
            match = re.search(r':\s+[0-9a-f]+\s+(.+)', line)
            if match:
                return match.group(1).strip()

        return f"??? (0x{opcode:04x})"
    except Exception as e:
        return f"ERROR: {e}"
    finally:
        import os
        os.unlink(temp_file)

def decode_trace_line(line):
    """Parse and decode a single trace line."""
    # Format: [12345] 02009A80 5041 | D0-D7... | A0-A7... | SR flags
    match = re.match(r'\[(\d+)\]\s+([0-9A-F]{8})\s+([0-9A-F]{4})\s+\|', line)
    if not match:
        return line  # Not a trace line, return as-is

    inst_num = match.group(1)
    pc = int(match.group(2), 16)
    opcode = int(match.group(3), 16)

    # Disassemble the instruction
    disasm = disassemble_opcode(pc, opcode)

    # Insert the disassembly after the opcode
    return f"[{inst_num}] {pc:08X} {opcode:04X} {disasm:30s} | {line.split('|', 1)[1]}"

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <trace_file> [start_line] [end_line]")
        sys.exit(1)

    trace_file = sys.argv[1]
    start_line = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    end_line = int(sys.argv[3]) if len(sys.argv) > 3 else None

    with open(trace_file, 'r') as f:
        for line_num, line in enumerate(f, 1):
            if line_num < start_line:
                continue
            if end_line and line_num > end_line:
                break

            line = line.rstrip()
            if line.startswith('['):
                print(decode_trace_line(line))
            else:
                print(line)

if __name__ == '__main__':
    main()

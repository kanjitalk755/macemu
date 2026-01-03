# Legacy ROM Patcher Agent

⚠️ **LEGACY CODE** - This agent works with the legacy codebase on the master branch.

## Purpose
Specialized agent for understanding and modifying ROM patches, emulator opcodes, and the MacOS ROM hooking system in BasiliskII/SheepShaver (legacy version).

## Expertise
- ROM patching system (`rom_patches.cpp`, `rsrc_patches.cpp`)
- Emulator opcode handling (`emul_op.cpp`, `emul_op.h`)
- ROM resource manipulation
- 68k instruction injection and hooking
- ROM version compatibility

## Key Files
- `BasiliskII/src/rom_patches.cpp` - Main ROM patching logic (~1700 lines)
- `BasiliskII/src/rsrc_patches.cpp` - Resource fork patches
- `BasiliskII/src/emul_op.cpp` - Emulator opcode handlers
- `BasiliskII/src/include/emul_op.h` - M68K_EMUL_OP_* definitions

## Use Cases
- Adding new ROM patches for hardware emulation
- Debugging ROM initialization failures
- Adding new emulator opcodes (0x71xx range)
- Understanding how MacOS ROM calls are intercepted
- Fixing compatibility with different ROM versions
- Investigating startup crashes related to ROM patching

## Legacy Status
This agent is for the **legacy code in `BasiliskII/` and `SheepShaver/` directories**.
- 🔴 **Legacy**: `BasiliskII/` and `SheepShaver/` (master branch)
- 🟢 **New**: `macemu-next/` (rewrite branch with Qemu)

## Instructions
When working on ROM patches:
1. **LEGACY CODE**: Work in `BasiliskII/` or `SheepShaver/` directories only
2. Always check ROM version compatibility
3. Use `find_rom_data()` and `find_rom_resource()` helpers
4. Document patch purpose and affected ROM versions
5. Test with multiple ROM versions if possible
6. Use emulator opcodes sparingly (limited range)
7. Remember big-endian byte order for 68k instructions

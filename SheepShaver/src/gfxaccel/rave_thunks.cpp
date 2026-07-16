/*
 *  rave_thunks.cpp - RAVE PPC-to-native thunk allocation
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  Allocates 53 PPC-callable TVECTs in SheepMem for RAVE draw and engine
 *  methods. Each TVECT writes a sub-opcode to a scratch word then executes
 *  NATIVE_RAVE_DISPATCH to reach the native dispatch handler.
 *
 *  Each TVECT is a proper PPC transition vector: 8-byte header (code_ptr, TOC)
 *  followed by the thunk code. This is required because CFM code (including
 *  the RAVE manager) expects function pointers to be TVECT pointers.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "thunks.h"
#include "rave_engine.h"

// Storage for TVECT addresses and scratch word
uint32_t rave_method_tvects[RAVE_MAX_SUBOPCODE];
uint32_t rave_scratch_addr = 0;

// Sentinel engine handle and saved original TVECTs (set by RaveInstallHooks)
uint32_t rave_sentinel_engine = 0;
uint32_t rave_orig_get_first_engine = 0;
uint32_t rave_orig_get_next_engine = 0;
uint32_t rave_orig_engine_gestalt = 0;
uint32_t rave_orig_engine_check_device = 0;
uint32_t rave_orig_draw_context_new = 0;
uint32_t rave_orig_texture_new = 0;
uint32_t rave_orig_bitmap_new = 0;
uint32_t rave_orig_color_table_new = 0;
uint32_t rave_orig_draw_context_delete = 0;
uint32_t rave_orig_texture_delete = 0;
uint32_t rave_orig_bitmap_delete = 0;
uint32_t rave_orig_color_table_delete = 0;
uint32_t rave_orig_texture_bind_color_table = 0;
uint32_t rave_orig_bitmap_bind_color_table = 0;
uint32_t rave_orig_texture_detach = 0;
uint32_t rave_orig_bitmap_detach = 0;
uint32_t rave_orig_access_texture = 0;
uint32_t rave_orig_access_texture_end = 0;
uint32_t rave_orig_access_bitmap = 0;
uint32_t rave_orig_access_bitmap_end = 0;
uint32_t rave_orig_engine_enable = 0;
uint32_t rave_orig_engine_disable = 0;

/*
 *  Allocate a single RAVE TVECT thunk in SheepMem
 *
 *  Layout (32 bytes total):
 *    +0:  code_ptr (= base + 8, points to first instruction)
 *    +4:  TOC (= 0, unused)
 *    +8:  lis   r11, scratch_hi16     -- load upper half of scratch address
 *   +12:  ori   r11, r11, scratch_lo16 -- load lower half
 *   +16:  li    r12, method_id        -- load the sub-opcode
 *   +20:  stw   r12, 0(r11)           -- write method ID to scratch word
 *   +24:  <rave_opcode>               -- execute NATIVE_RAVE_DISPATCH
 *   +28:  blr                         -- return to PPC caller
 *
 *  Uses r11 and r12 as scratch registers (volatile in PPC ABI),
 *  preserving r3-r10 which carry the original RAVE method arguments.
 *
 *  NOTE: r0 cannot be used as the base register in stw because PPC
 *  treats r0 in the rA field of load/store instructions as literal 0.
 *
 *  Returns the TVECT address (base), NOT the code address.
 */
static uint32 AllocateRaveTVECT(int method_id, uint32 rave_opcode)
{
	uint32 scratch_hi = (rave_scratch_addr >> 16) & 0xFFFF;
	uint32 scratch_lo = rave_scratch_addr & 0xFFFF;

	// Reserve 32 bytes: 8-byte TVECT header + 24 bytes (6 instructions)
	uint32 base = SheepMem::ReserveProc(32);
	uint32 code = base + 8;

	// Write TVECT header: {code_ptr, TOC}
	WriteMacInt32(base + 0, code);	// Code pointer -> first instruction
	WriteMacInt32(base + 4, 0);		// TOC (unused)

	// PPC instruction encodings:
	// lis rD, imm     = addis rD, 0, imm = 0x3C000000 | (rD << 21) | (imm & 0xFFFF)
	// ori rS, rA, imm = 0x60000000 | (rS << 21) | (rA << 16) | (imm & 0xFFFF)
	// li rD, imm      = addi rD, 0, imm  = 0x38000000 | (rD << 21) | (imm & 0xFFFF)
	// stw rS, d(rA)   = 0x90000000 | (rS << 21) | (rA << 16) | (d & 0xFFFF)

	const uint32 r11 = 11;
	const uint32 r12 = 12;

	// lis r11, scratch_hi16
	WriteMacInt32(code + 0, 0x3C000000 | (r11 << 21) | (scratch_hi & 0xFFFF));

	// ori r11, r11, scratch_lo16
	WriteMacInt32(code + 4, 0x60000000 | (r11 << 21) | (r11 << 16) | (scratch_lo & 0xFFFF));

	// li r12, method_id
	WriteMacInt32(code + 8, 0x38000000 | (r12 << 21) | (method_id & 0xFFFF));

	// stw r12, 0(r11)
	WriteMacInt32(code + 12, 0x90000000 | (r12 << 21) | (r11 << 16) | (0 & 0xFFFF));

	// NATIVE_RAVE_DISPATCH opcode
	WriteMacInt32(code + 16, rave_opcode);

	// blr
	WriteMacInt32(code + 20, 0x4E800020);

	return base;  // Return TVECT address (not code address)
}

/*
 *  Initialize all RAVE TVECTs
 */
void RaveThunksInit(void)
{
	// Allocate 32 bytes for the scratch word + diagnostic buffers
	// Layout: [0..3] sub-opcode scratch, [4..7] spare,
	//         [8..15] TQADevice struct, [16..19] gestalt response
	rave_scratch_addr = SheepMem::Reserve(32);
	WriteMacInt32(rave_scratch_addr, 0);

	// Get the native opcode for RAVE dispatch
	uint32 rave_opcode = NativeOpcode(NATIVE_RAVE_DISPATCH);

	// Clear the tvects array
	memset(rave_method_tvects, 0, sizeof(rave_method_tvects));

	int tvect_count = 0;

	// Allocate TVECTs for 35 draw methods (sub-opcodes 0-34)
	for (int i = 0; i < kRaveDrawMethodCount; i++) {
		rave_method_tvects[i] = AllocateRaveTVECT(i, rave_opcode);
		tvect_count++;
	}

	// Allocate TVECTs for 18 engine methods (sub-opcodes 100-117)
	for (int i = 0; i < kRaveEngineMethodCount; i++) {
		int method_id = kRaveEngineDrawPrivateNew + i;
		rave_method_tvects[method_id] = AllocateRaveTVECT(method_id, rave_opcode);
		tvect_count++;
	}

	// Allocate TVECTs for 5 hook thunks (sub-opcodes 200-204)
	// These are simple dispatch-to-native thunks (same as engine thunks).
	// The native handler does the sentinel check and chains to original if needed.
	for (int i = 0; i < kRaveHookCount; i++) {
		int method_id = kRaveHookGetFirstEngine + i;
		rave_method_tvects[method_id] = AllocateRaveTVECT(method_id, rave_opcode);
		tvect_count++;
	}

	// Allocate TVECTs for 4 ATI RaveExtFuncs (sub-opcodes 300-303)
	for (int i = 0; i < kRaveATIMethodCount; i++) {
		int method_id = kRaveATIClearDrawBuffer + i;
		rave_method_tvects[method_id] = AllocateRaveTVECT(method_id, rave_opcode);
		tvect_count++;
	}

	// Allocate sentinel TQAEngine struct in SheepMem.
	//
	// The RAVE manager's internal TQAEngine struct is opaque, but the manager
	// reads engine method TVECTs from known offsets within it. If ANY API
	// function that takes a TQAEngine* is not hooked (e.g. QATextureDetach,
	// QAAccessTexture), the RAVE manager's original code runs and reads
	// method pointers from the engine struct at fixed offsets.
	//
	// Observed layout (from crash analysis - lwz r12, 0x24(sentinel) for
	// TextureDetach = tag 5, giving header_size=16):
	//   +0x00: magic / internal field
	//   +0x04: GetMethod TVECT (or internal pointer)
	//   +0x08: refCon (or internal field)
	//   +0x0C: internal field (next engine, flags, etc.)
	//   +0x10: tag 0 = DrawPrivateNew TVECT
	//   +0x14: tag 1 = DrawPrivateDelete TVECT
	//   ...
	//   +0x10 + tag*4: engine method TVECT for tag N
	//   +0x54: tag 17 = AccessBitmapEnd TVECT
	//
	// Total needed: 0x10 + 18*4 = 0x58 = 88 bytes.
	// We allocate 128 bytes for safety and fill every 4-byte slot with a
	// valid engine method TVECT so ANY offset read returns a callable pointer.
	//
	#define RAVE_SENTINEL_SIZE 128
	#define RAVE_ENGINE_METHOD_TABLE_OFFSET 0x10

	rave_sentinel_engine = SheepMem::Reserve(RAVE_SENTINEL_SIZE);
	memset(Mac2HostAddr(rave_sentinel_engine), 0, RAVE_SENTINEL_SIZE);
	WriteMacInt32(rave_sentinel_engine, RAVE_ENGINE_MAGIC);

	// Store GetMethod TVECT at offset +4 (common layout for engine struct)
	WriteMacInt32(rave_sentinel_engine + 4,
	              rave_method_tvects[kRaveEngineDrawPrivateNew]);

	// Populate method table at offset 0x10: tag N -> rave_method_tvects[100+N]
	for (int tag = 0; tag < kRaveEngineMethodCount; tag++) {
		uint32 tvect = rave_method_tvects[kRaveEngineDrawPrivateNew + tag];
		WriteMacInt32(rave_sentinel_engine + RAVE_ENGINE_METHOD_TABLE_OFFSET + tag * 4,
		              tvect);
	}

	// Defense in depth: fill ALL remaining zero slots with valid engine method
	// TVECTs. If the header size assumption is wrong, this ensures any read
	// gets a callable TVECT (worst case: wrong method, but no crash).
	// Skip offset 0 (magic value) -- start from offset 4.
	uint32 fallback_tvect = rave_method_tvects[kRaveEngineDrawPrivateNew];
	for (uint32 off = 4; off < RAVE_SENTINEL_SIZE; off += 4) {
		if (ReadMacInt32(rave_sentinel_engine + off) == 0)
			WriteMacInt32(rave_sentinel_engine + off, fallback_tvect);
	}

	RAVE_LOG("RaveThunksInit: allocated %d TVECTs, scratch at 0x%08x, sentinel at 0x%08x",
			 tvect_count, rave_scratch_addr, rave_sentinel_engine);

	// TVECT diagnostic dump gated by rave_logging_enabled. Test: RAVEABITests.testTVECT_loggingGate
	if (rave_logging_enabled) {
		printf("RAVE ThunksInit: scratch=0x%08x, rave_opcode=0x%08x\n",
		       rave_scratch_addr, rave_opcode);
		for (int i = 0; i < kRaveDrawMethodCount; i++) {
			uint32 base = rave_method_tvects[i];
			uint32 code = ReadMacInt32(base);       // code_ptr at TVECT[0]
			uint32 instr = ReadMacInt32(code);       // first instruction
			printf("  draw[%2d] TVECT=0x%08x code=0x%08x instr=0x%08x\n",
			       i, base, code, instr);
		}
	}
}

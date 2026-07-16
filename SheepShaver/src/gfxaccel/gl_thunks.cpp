/*
 *  gl_thunks.cpp - OpenGL PPC-to-native thunk allocation
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  Allocates PPC-callable TVECTs in SheepMem for all GL/AGL/GLU/GLUT
 *  functions (~643 total). Each TVECT writes a sub-opcode to a scratch
 *  word then executes NATIVE_OPENGL_DISPATCH to reach the native handler.
 *
 *  Pattern is identical to rave_thunks.cpp. Each TVECT is a proper PPC
 *  transition vector: 8-byte header (code_ptr, TOC) followed by thunk code.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "thunks.h"
#include "gl_engine.h"
#include "gl_defer.h"
#include "gl_ppc_emit.h"   // ppc_lis/ppc_ori/ppc_stw/ppc_blr (Track-A PPC encoders)
#include <cassert>

// Storage for TVECT addresses and scratch words
uint32_t gl_method_tvects[GL_MAX_SUBOPCODE];
uint32_t gl_scratch_addr = 0;

// Dispatch-table TVECTs (called when game accesses context's internal dispatch table).
// These shift R3-R10 left by one position because the game passes the context
// index in R3 and real GL args start at R4.
uint32_t gl_dt_method_tvects[GL_MAX_SUBOPCODE];

// Per-slot diagnostic no-op TVECTs, one per context-handle dispatch slot.
// GLPopulateDispatchTable installs these into every slot it doesn't map to a
// real GL function, so a game that reads a GL function descriptor directly from
// the context handle (e.g. an extension slot beyond core 1.1) gets a safe
// logging no-op instead of jumping through uninitialized guest heap. Each
// encodes its slot as sub-opcode GL_DT_DIAG_BASE + slot.
uint32_t gl_dt_diag_tvects[GL_CTX_DISPATCH_SLOTS];

// gl_dt_flag_addr — runtime calling-convention discriminator.
//
// Two PPC calling conventions reach NativeGLDispatch:
//   (A) FindLibSymbol TVECT (stub call): AllocateGLTVECT sets flag=0.
//       GPR3..GPR10 carry the real GL function arguments directly.
//   (B) Dispatch-table slot: AllocateGLDispatchTableTVECT sets flag=1.
//       GPR3 = context index; GPR4..GPR10 carry the real arguments
//       shifted by one register.
//
// gl_dispatch.cpp reads this flag into gl_ppc_stack_arg_offset:
//   - flag=0 → args start at GPR3 (standard PPC ABI)
//   - flag=1 → args start at GPR4 (context index in GPR3 is consumed)
//
// For 9+ argument functions (glTexImage2D, glTexSubImage2D), the flag also
// determines the stack argument offset (PPC calling convention passes args
// 9+ on the stack; the offset differs by one slot between conventions).
//
// Single-threaded by design: the emulator thread sets the flag, reads it,
// and dispatches — no race. Do not eliminate; it's a runtime invariant.
uint32_t gl_dt_flag_addr = 0;  // 1 = dispatch-table call, 0 = stub call
// gl_logging_enabled is defined in gl_dispatch.cpp (single definition)

/*
 *  Function signature table -- maps sub-opcode to argument type info.
 *
 *  float_mask: bit N = 1 means arg N is a float/double (from FPR).
 *  Only the most commonly used functions have explicit signatures.
 *  Functions not in this table default to {0, 0} (all-integer/pointer args).
 *
 *  PPC ABI: floats/doubles are passed in FPR1-FPR13.
 *  Integer/pointer args go in GPR3-GPR10.
 *  The generic dispatch handler uses this table to extract FPR values.
 */
// Mutable copy that gets populated at init time
GLFuncSignature gl_func_signatures[GL_MAX_SUBOPCODE];
// gl_func_signatures is the extern array declared in gl_engine.h
// It's non-const here because we populate it at init time, but declared
// as const extern for read-only access from dispatch code

/*
 *  Allocate a single GL TVECT thunk in SheepMem
 *
 *  Layout is identical to AllocateRaveTVECT (32 bytes):
 *    +0:  code_ptr (= base + 8)
 *    +4:  TOC (= 0)
 *    +8:  lis   r11, scratch_hi16
 *   +12:  ori   r11, r11, scratch_lo16
 *   +16:  li    r12, method_id
 *   +20:  stw   r12, 0(r11)
 *   +24:  <gl_opcode>    -- NATIVE_OPENGL_DISPATCH
 *   +28:  blr
 */
static uint32 AllocateGLTVECT(int method_id, uint32 gl_opcode)
{
	uint32 scratch_hi = (gl_scratch_addr >> 16) & 0xFFFF;
	uint32 scratch_lo = gl_scratch_addr & 0xFFFF;

	uint32 base = SheepMem::ReserveProc(32);
	uint32 code = base + 8;

	// TVECT header
	WriteMacInt32(base + 0, code);
	WriteMacInt32(base + 4, 0);

	const uint32 r11 = 11;
	const uint32 r12 = 12;

	// lis r11, scratch_hi16
	WriteMacInt32(code + 0, 0x3C000000 | (r11 << 21) | (scratch_hi & 0xFFFF));
	// ori r11, r11, scratch_lo16
	WriteMacInt32(code + 4, 0x60000000 | (r11 << 21) | (r11 << 16) | (scratch_lo & 0xFFFF));
	// li r12, method_id
	WriteMacInt32(code + 8, 0x38000000 | (r12 << 21) | (method_id & 0xFFFF));
	// stw r12, 0(r11)
	WriteMacInt32(code + 12, 0x90000000 | (r12 << 21) | (r11 << 16));
	// NATIVE_OPENGL_DISPATCH opcode
	WriteMacInt32(code + 16, gl_opcode);
	// blr
	WriteMacInt32(code + 20, 0x4E800020);

	return base;
}

/*
 *  AllocateGLDispatchTableTVECT - thunk for dispatch-table calls
 *
 *  Same as AllocateGLTVECT but also writes 1 to gl_dt_flag_addr so the
 *  native dispatch handler knows to shift GPR args left by one (the game
 *  passes the context index in R3, real GL args start at R4).
 *
 *  Layout (48 bytes):
 *    +0:  code_ptr (= base + 8)
 *    +4:  TOC (= 0)
 *    +8:  lis   r11, flag_hi16
 *   +12:  ori   r11, r11, flag_lo16
 *   +16:  li    r12, 1
 *   +20:  stw   r12, 0(r11)          -- set flag = 1
 *   +24:  lis   r11, scratch_hi16
 *   +28:  ori   r11, r11, scratch_lo16
 *   +32:  li    r12, method_id
 *   +36:  stw   r12, 0(r11)          -- write sub-opcode
 *   +40:  <gl_opcode>                -- NATIVE_OPENGL_DISPATCH
 *   +44:  blr
 */
static uint32 AllocateGLDispatchTableTVECT(int method_id, uint32 gl_opcode)
{
	uint32 flag_hi = (gl_dt_flag_addr >> 16) & 0xFFFF;
	uint32 flag_lo = gl_dt_flag_addr & 0xFFFF;
	uint32 scratch_hi = (gl_scratch_addr >> 16) & 0xFFFF;
	uint32 scratch_lo = gl_scratch_addr & 0xFFFF;

	uint32 base = SheepMem::ReserveProc(48);
	uint32 code = base + 8;

	// TVECT header
	WriteMacInt32(base + 0, code);
	WriteMacInt32(base + 4, 0);

	const uint32 r11 = 11;
	const uint32 r12 = 12;

	// Set dispatch-table flag = 1
	WriteMacInt32(code + 0, 0x3C000000 | (r11 << 21) | (flag_hi & 0xFFFF));       // lis r11, flag_hi
	WriteMacInt32(code + 4, 0x60000000 | (r11 << 21) | (r11 << 16) | (flag_lo & 0xFFFF)); // ori r11, r11, flag_lo
	WriteMacInt32(code + 8, 0x38000000 | (r12 << 21) | 1);                        // li r12, 1
	WriteMacInt32(code + 12, 0x90000000 | (r12 << 21) | (r11 << 16));             // stw r12, 0(r11)

	// Write sub-opcode to scratch (same as normal thunk)
	WriteMacInt32(code + 16, 0x3C000000 | (r11 << 21) | (scratch_hi & 0xFFFF));   // lis r11, scratch_hi
	WriteMacInt32(code + 20, 0x60000000 | (r11 << 21) | (r11 << 16) | (scratch_lo & 0xFFFF)); // ori r11, r11, scratch_lo
	WriteMacInt32(code + 24, 0x38000000 | (r12 << 21) | (method_id & 0xFFFF));    // li r12, method_id
	WriteMacInt32(code + 28, 0x90000000 | (r12 << 21) | (r11 << 16));             // stw r12, 0(r11)

	// NATIVE_OPENGL_DISPATCH opcode
	WriteMacInt32(code + 32, gl_opcode);
	// blr
	WriteMacInt32(code + 36, 0x4E800020);

	return base;
}

/*
 *  EmitDeferFallback - shared "trap normally" tail for Track-A deferrable stubs.
 *
 *  Background (Track A immediate-mode batching): a deferrable opcode's stub
 *  (Task 8) buffers the call into the guest ring and returns WITHOUT trapping --
 *  but only while deferral is enabled AND the ring has room. When deferral is
 *  DISABLED or the ring is FULL, the stub instead takes the normal trap path for
 *  that one call. Because the GLDispatch drain-first hook flushes the ring into
 *  im_vertices before the trapped call runs, simply trapping the single call is
 *  correct -- there is no overflow-drain/retry dance. Both the "disabled" and
 *  "full" cases branch (unconditional `b`, +/-32 MiB range) to this ONE shared
 *  routine.
 *
 *  The routine is exactly the TAIL of AllocateGLTVECT: it writes the sub-opcode
 *  (already in r12, set by the stub) to the scratch word, then executes
 *  NATIVE_OPENGL_DISPATCH and returns. It is reached by a direct `b` to its
 *  first instruction, so -- unlike a TVECT, which is *called* through its
 *  [code_ptr][toc] header -- it needs NO 8-byte header. ReserveProc returns a
 *  raw executable region; we emit straight code there and publish the address of
 *  the first instruction.
 *
 *  Emitted code (4 instructions):
 *    lis  r11, scratch_hi16
 *    ori  r11, r11, scratch_lo16
 *    stw  r12, 0(r11)            -- store the sub-opcode the stub left in r12
 *    <gl_opcode>                 -- NATIVE_OPENGL_DISPATCH
 *    blr
 *
 *  gl_defer_common_tail is set to the address of the FIRST instruction (the
 *  `lis`), i.e. the `b` target Task 8's stubs must branch to. (Its header
 *  comment in gl_defer.h is updated to reflect that it now holds this fallback
 *  routine's code address, not a TVECT addr.)
 */
static void EmitDeferFallback(uint32 gl_opcode)
{
	uint32 scratch_hi = (gl_scratch_addr >> 16) & 0xFFFF;
	uint32 scratch_lo = gl_scratch_addr & 0xFFFF;

	// 5 instructions, no TVECT header (this is a `b` target, not a callee).
	uint32 code = SheepMem::ReserveProc(5 * 4);

	const int r11 = 11;
	const int r12 = 12;

	WriteMacInt32(code +  0, ppc_lis(r11, scratch_hi));            // lis  r11, scratch_hi
	WriteMacInt32(code +  4, ppc_ori(r11, r11, scratch_lo));       // ori  r11, r11, scratch_lo
	WriteMacInt32(code +  8, ppc_stw(r12, 0, r11));                // stw  r12, 0(r11)
	WriteMacInt32(code + 12, gl_opcode);                          // NATIVE_OPENGL_DISPATCH
	WriteMacInt32(code + 16, ppc_blr());                          // blr

	gl_defer_common_tail = code;  // code entry = the `b fallback` target for Task 8
}

/*
 *  EmitDeferStub - emit a per-opcode immediate-mode capture stub (Task 8).
 *
 *  This replaces AllocateGLTVECT for every deferrable opcode. It is a proper
 *  TVECT (8-byte [code_ptr][toc] header, because the game CALLS it as the GL
 *  function) whose body BUFFERS the call's arguments into the guest-memory ring
 *  and returns -- WITHOUT trapping -- when deferral is enabled and the ring has
 *  room. When deferral is disabled (kill-switch / display-list gate clears the
 *  enabled word) or the ring is full, it branches to the shared
 *  gl_defer_common_tail fallback, which writes the sub-opcode (left in r12) to
 *  the scratch word and takes the normal trap path for that single call.
 *
 *  Register discipline (see KEY ABI FACTS in the task brief):
 *    - r3.. carry the integer/pointer args; f1.. carry the float args densely.
 *    - r12 holds the sub-opcode the whole way through (needed both for the ring
 *      header AND by the fallback tail).
 *    - r0, r11 and r12 are the only scratch we touch: r11 is the address/base
 *      reg, r0 the value temp for the header/ptr-copy. The commit block uses r12
 *      as the value temp instead of r0 because PPC addi treats RA==0 as the
 *      literal constant 0 (so `addi r0,r0,n` would NOT accumulate r0's contents);
 *      r12's sub_opcode is dead on the success path so it is safe to reuse there.
 *      None of r0/r11/r12 is an argument register for our opcodes (args are
 *      r3..r10, f1..), so the ptr snapshot (which reads the arg ptr in r(3+scalar)
 *      and writes through r11) never clobbers a live arg.
 *
 *  Wire layout emitted (must match GLDeferDecodeRecord byte-for-byte):
 *    [u32 sub_opcode][u32 size_bytes][scalar x u32 from r3..][fpr_count x u32
 *     from f1.. via stfs, dense][round_up_4(ptr_bytes) snapshot]
 */
static int gl_round_up_4(int n) { return (n + 3) & ~3; }
static int gl_popcount16(uint32 v) { int n = 0; while (v) { n += (v & 1u); v >>= 1; } return n; }

static uint32 EmitDeferStub(int sub_opcode, const GLDeferDesc &desc, uint32 gl_opcode)
{
	const int scalar  = desc.scalar_gpr_count;
	const int fprc    = gl_popcount16(desc.fpr_mask);
	const int ptrb    = desc.ptr_bytes;
	const int ptr_pad = gl_round_up_4(ptrb);
	const int need    = 8 + 4 * scalar + 4 * fprc + ptr_pad;   // size_bytes header value

	// ptr copy breakdown: as many full words as fit, then one halfword, then one
	// byte for whatever remains (ptr_bytes is never both an odd word remainder
	// AND > 2 leftover for our opcodes, but the generic word/half/byte split
	// covers any value).
	const int ptr_words = ptrb / 4;
	int rem = ptrb - ptr_words * 4;
	const bool ptr_half = (rem >= 2);
	if (ptr_half) rem -= 2;
	const bool ptr_byte = (rem >= 1);

	// Instruction count (must be exact so ReserveProc is sized correctly):
	//   li r12              : 1
	//   enabled: lis,ori,lwz,cmpwi,bne,b   : 6
	//   space:   lis,ori,lwz,cmplwi,ble,b  : 6
	//   wptr:    lis,ori,add               : 3
	//   header:  stw r12; li r0; stw r0    : 3
	//   scalars                            : scalar
	//   fprs                               : fprc
	//   ptr:     (lwz+stw) per word + (lhz+sth) + (lbz+stb)
	//   commit:  head(lis,ori,lwz,addi,stw)+count(lis,ori,lwz,addi,stw) : 10
	//   blr                                : 1
	const int ninstr = 1 + 6 + 6 + 3 + 3
	                 + scalar + fprc
	                 + ptr_words * 2 + (ptr_half ? 2 : 0) + (ptr_byte ? 2 : 0)
	                 + 10 + 1;

	const uint32 enabled_hi = (gl_defer_enabled_addr >> 16) & 0xFFFF;
	const uint32 enabled_lo =  gl_defer_enabled_addr        & 0xFFFF;
	const uint32 head_hi    = (gl_defer_head_addr    >> 16) & 0xFFFF;
	const uint32 head_lo    =  gl_defer_head_addr           & 0xFFFF;
	const uint32 count_hi   = (gl_defer_count_addr   >> 16) & 0xFFFF;
	const uint32 count_lo   =  gl_defer_count_addr          & 0xFFFF;
	const uint32 ring_hi    = (gl_defer_ring_base    >> 16) & 0xFFFF;
	const uint32 ring_lo    =  gl_defer_ring_base           & 0xFFFF;

	uint32 base = SheepMem::ReserveProc(8 + 4 * ninstr);
	uint32 code = base + 8;

	// TVECT header (this stub is CALLED through its [code_ptr][toc] header).
	WriteMacInt32(base + 0, code);
	WriteMacInt32(base + 4, 0);

	const int r0 = 0, r11 = 11, r12 = 12;

	int idx = 0;
	#define A() (code + 4 * idx)                       // absolute addr of slot idx
	#define EMIT(word) do { WriteMacInt32(code + 4 * idx, (word)); ++idx; } while (0)

	// sub-opcode -> r12 (ring header + fallback tail both need it)
	EMIT(ppc_li(r12, sub_opcode));

	// --- enabled check: if (*enabled == 0) goto fallback -----------------------
	EMIT(ppc_lis(r11, enabled_hi));
	EMIT(ppc_ori(r11, r11, enabled_lo));
	EMIT(ppc_lwz(r0, 0, r11));
	EMIT(ppc_cmpwi(r0, 0));
	{ uint32 a = A(); EMIT(ppc_bne(a, a + 8)); }       // r0 != 0 -> skip the long branch
	{ uint32 a = A(); EMIT(ppc_b(a, gl_defer_common_tail)); }  // disabled -> fallback

	// --- space check: if (head >u RING_SIZE-need) goto fallback ----------------
	EMIT(ppc_lis(r11, head_hi));
	EMIT(ppc_ori(r11, r11, head_lo));
	EMIT(ppc_lwz(r0, 0, r11));                          // r0 = head (byte offset)
	EMIT(ppc_cmplwi(r0, (uint32)(GL_DEFER_RING_SIZE - need)));  // UNSIGNED compare
	{ uint32 a = A(); EMIT(ppc_ble(a, a + 8)); }       // head <=u limit -> space ok
	{ uint32 a = A(); EMIT(ppc_b(a, gl_defer_common_tail)); }  // full -> fallback

	// --- write pointer = ring_base + head --------------------------------------
	// r0 still holds head from the space check (untouched above).
	EMIT(ppc_lis(r11, ring_hi));
	EMIT(ppc_ori(r11, r11, ring_lo));
	EMIT(ppc_add(r11, r11, r0));                        // r11 = ring_base + head

	// --- record header ---------------------------------------------------------
	EMIT(ppc_stw(r12, 0, r11));                         // [0] sub_opcode
	EMIT(ppc_li(r0, need));
	EMIT(ppc_stw(r0, 4, r11));                          // [4] size_bytes

	// --- scalar GPRs: r(3+k) -> (8 + 4k)(r11) ----------------------------------
	for (int k = 0; k < scalar; ++k)
		EMIT(ppc_stw(3 + k, 8 + 4 * k, r11));

	// --- FPRs (dense): f(1+n) -> (8 + 4*scalar + 4*n)(r11) ---------------------
	const int fpr_base_off = 8 + 4 * scalar;
	for (int n = 0; n < fprc; ++n)
		EMIT(ppc_stfs(1 + n, fpr_base_off + 4 * n, r11));

	// --- trailing pointer snapshot ---------------------------------------------
	// src ptr arg = r(3+scalar); dst offset = 8 + 4*scalar + 4*fprc.
	if (ptrb > 0) {
		const int src     = 3 + scalar;
		const int dst_off = 8 + 4 * scalar + 4 * fprc;
		int off = 0;
		for (int w = 0; w < ptr_words; ++w, off += 4) {
			EMIT(ppc_lwz(r0, off, src));
			EMIT(ppc_stw(r0, dst_off + off, r11));
		}
		if (ptr_half) {
			EMIT(ppc_lhz(r0, off, src));
			EMIT(ppc_sth(r0, dst_off + off, r11));
			off += 2;
		}
		if (ptr_byte) {
			EMIT(ppc_lbz(r0, off, src));
			EMIT(ppc_stb(r0, dst_off + off, r11));
			off += 1;
		}
	}

	// --- commit: head += need; count += 1 --------------------------------------
	// IMPORTANT: addi's RA==0 means "literal 0", NOT register r0 -- so the loaded
	// value MUST be in a register != r0 for the add to actually accumulate. We use
	// r11 as the address reg and r12 as the value temp (r12's sub_opcode is dead on
	// the success path; the fallback path never reaches here).
	EMIT(ppc_lis(r11, head_hi));
	EMIT(ppc_ori(r11, r11, head_lo));
	EMIT(ppc_lwz(r12, 0, r11));
	EMIT(ppc_addi(r12, r12, need));    // r12 = head + need (RA=r12 != 0 -> real add)
	EMIT(ppc_stw(r12, 0, r11));

	EMIT(ppc_lis(r11, count_hi));
	EMIT(ppc_ori(r11, r11, count_lo));
	EMIT(ppc_lwz(r12, 0, r11));
	EMIT(ppc_addi(r12, r12, 1));       // r12 = count + 1
	EMIT(ppc_stw(r12, 0, r11));

	EMIT(ppc_blr());

	#undef A
	#undef EMIT

	// Sanity: the slots we wrote must exactly match the reserved size.
	assert(idx == ninstr && "EmitDeferStub instruction count mismatch");

	return base;
}

/*
 *  Populate function signature table for known GL functions.
 *
 *  This tells the dispatch handler which arguments are floats (FPR)
 *  vs integers/pointers (GPR). Only functions with float args need entries.
 */
static void InitFuncSignatures()
{
	memset(gl_func_signatures, 0, sizeof(gl_func_signatures));

	// Helper macro: set signature for a sub-opcode
	#define SIG(sub, nargs, fmask) \
		gl_func_signatures[sub] = { (uint8_t)(nargs), (uint16_t)(fmask) }

	// Core GL functions with float arguments:
	// Note: the "ctx" pointer is arg0 in the dispatch table but is NOT
	// passed through the thunk -- it's the GLContext looked up by the handler.
	// So num_args here is the PPC-visible arg count (r3 onwards).

	// accum(op, value) -- op=int, value=float
	SIG(GL_SUB_ACCUM, 2, 0x02);
	// alpha_func(func, ref) -- func=int, ref=float
	SIG(GL_SUB_ALPHA_FUNC, 2, 0x02);
	// clear_accum(r, g, b, a) -- 4 floats
	SIG(GL_SUB_CLEAR_ACCUM, 4, 0x0F);
	// clear_color(r, g, b, a) -- 4 floats
	SIG(GL_SUB_CLEAR_COLOR, 4, 0x0F);
	// clear_depth(depth) -- 1 double (1 FPR slot)
	SIG(GL_SUB_CLEAR_DEPTH, 1, 0x01);
	// clear_index(c) -- 1 float
	SIG(GL_SUB_CLEAR_INDEX, 1, 0x01);
	// color3f(r, g, b)
	SIG(GL_SUB_COLOR3F, 3, 0x07);
	// color3d(r, g, b)
	SIG(GL_SUB_COLOR3D, 3, 0x07);
	// color4f(r, g, b, a)
	SIG(GL_SUB_COLOR4F, 4, 0x0F);
	// color4d(r, g, b, a)
	SIG(GL_SUB_COLOR4D, 4, 0x0F);
	// depth_range(near, far) -- 2 doubles
	SIG(GL_SUB_DEPTH_RANGE, 2, 0x03);
	// fogf(pname, param) -- pname=int, param=float
	SIG(GL_SUB_FOGF, 2, 0x02);
	// frustum(l, r, b, t, n, f) -- 6 doubles
	SIG(GL_SUB_FRUSTUM, 6, 0x3F);
	// indexd(c) -- 1 double
	SIG(GL_SUB_INDEXD, 1, 0x01);
	// indexf(c) -- 1 float
	SIG(GL_SUB_INDEXF, 1, 0x01);
	// lightf(light, pname, param) -- light=int, pname=int, param=float
	SIG(GL_SUB_LIGHTF, 3, 0x04);
	// light_modelf(pname, param) -- pname=int, param=float
	SIG(GL_SUB_LIGHT_MODELF, 2, 0x02);
	// line_width(width) -- 1 float
	SIG(GL_SUB_LINE_WIDTH, 1, 0x01);
	// materialf(face, pname, param)
	SIG(GL_SUB_MATERIALF, 3, 0x04);
	// normal3f(x, y, z) -- 3 floats
	SIG(GL_SUB_NORMAL3F, 3, 0x07);
	// normal3d(x, y, z)
	SIG(GL_SUB_NORMAL3D, 3, 0x07);
	// ortho(l, r, b, t, n, f) -- 6 doubles
	SIG(GL_SUB_ORTHO, 6, 0x3F);
	// pass_through(token) -- 1 float
	SIG(GL_SUB_PASS_THROUGH, 1, 0x01);
	// pixel_storef(pname, param) -- pname=int, param=float
	SIG(GL_SUB_PIXEL_STOREF, 2, 0x02);
	// pixel_transferf(pname, param)
	SIG(GL_SUB_PIXEL_TRANSFERF, 2, 0x02);
	// pixel_zoom(xfactor, yfactor) -- 2 floats
	SIG(GL_SUB_PIXEL_ZOOM, 2, 0x03);
	// point_size(size) -- 1 float
	SIG(GL_SUB_POINT_SIZE, 1, 0x01);
	// polygon_offset(factor, units) -- 2 floats
	SIG(GL_SUB_POLYGON_OFFSET, 2, 0x03);
	// rotatef(angle, x, y, z) -- 4 floats
	SIG(GL_SUB_ROTATEF, 4, 0x0F);
	// rotated(angle, x, y, z) -- 4 doubles
	SIG(GL_SUB_ROTATED, 4, 0x0F);
	// scalef(x, y, z) -- 3 floats
	SIG(GL_SUB_SCALEF, 3, 0x07);
	// scaled(x, y, z)
	SIG(GL_SUB_SCALED, 3, 0x07);
	// tex_coord1f(s)
	SIG(GL_SUB_TEX_COORD1F, 1, 0x01);
	// tex_coord1d(s)
	SIG(GL_SUB_TEX_COORD1D, 1, 0x01);
	// tex_coord2f(s, t)
	SIG(GL_SUB_TEX_COORD2F, 2, 0x03);
	// tex_coord2d(s, t)
	SIG(GL_SUB_TEX_COORD2D, 2, 0x03);
	// tex_coord3f(s, t, r)
	SIG(GL_SUB_TEX_COORD3F, 3, 0x07);
	// tex_coord3d(s, t, r)
	SIG(GL_SUB_TEX_COORD3D, 3, 0x07);
	// tex_coord4f(s, t, r, q)
	SIG(GL_SUB_TEX_COORD4F, 4, 0x0F);
	// tex_coord4d(s, t, r, q)
	SIG(GL_SUB_TEX_COORD4D, 4, 0x0F);
	// tex_envf(target, pname, param) -- target=int, pname=int, param=float
	SIG(GL_SUB_TEX_ENVF, 3, 0x04);
	// tex_gend(coord, pname, param)
	SIG(GL_SUB_TEX_GEND, 3, 0x04);
	// tex_genf(coord, pname, param)
	SIG(GL_SUB_TEX_GENF, 3, 0x04);
	// tex_parameterf(target, pname, param)
	SIG(GL_SUB_TEX_PARAMETERF, 3, 0x04);
	// translatef(x, y, z) -- 3 floats
	SIG(GL_SUB_TRANSLATEF, 3, 0x07);
	// translated(x, y, z)
	SIG(GL_SUB_TRANSLATED, 3, 0x07);
	// vertex2f(x, y)
	SIG(GL_SUB_VERTEX2F, 2, 0x03);
	// vertex2d(x, y)
	SIG(GL_SUB_VERTEX2D, 2, 0x03);
	// vertex3f(x, y, z)
	SIG(GL_SUB_VERTEX3F, 3, 0x07);
	// vertex3d(x, y, z)
	SIG(GL_SUB_VERTEX3D, 3, 0x07);
	// vertex4f(x, y, z, w)
	SIG(GL_SUB_VERTEX4F, 4, 0x0F);
	// vertex4d(x, y, z, w)
	SIG(GL_SUB_VERTEX4D, 4, 0x0F);
	// rectf(x1, y1, x2, y2)
	SIG(GL_SUB_RECTF, 4, 0x0F);
	// rectd(x1, y1, x2, y2)
	SIG(GL_SUB_RECTD, 4, 0x0F);
	// eval_coord1f(u)
	SIG(GL_SUB_EVAL_COORD1F, 1, 0x01);
	// eval_coord1d(u)
	SIG(GL_SUB_EVAL_COORD1D, 1, 0x01);
	// eval_coord2f(u, v)
	SIG(GL_SUB_EVAL_COORD2F, 2, 0x03);
	// eval_coord2d(u, v)
	SIG(GL_SUB_EVAL_COORD2D, 2, 0x03);
	// bitmap(w, h, xorig, yorig, xmove, ymove, bitmap)
	//   w=int, h=int, xorig=float, yorig=float, xmove=float, ymove=float, ptr=int
	SIG(GL_SUB_BITMAP, 7, 0x3C);
	// raster_pos2f(x, y)
	SIG(GL_SUB_RASTER_POS2F, 2, 0x03);
	// raster_pos2d(x, y)
	SIG(GL_SUB_RASTER_POS2D, 2, 0x03);
	// raster_pos3f(x, y, z)
	SIG(GL_SUB_RASTER_POS3F, 3, 0x07);
	// raster_pos3d(x, y, z)
	SIG(GL_SUB_RASTER_POS3D, 3, 0x07);
	// raster_pos4f(x, y, z, w)
	SIG(GL_SUB_RASTER_POS4F, 4, 0x0F);
	// raster_pos4d(x, y, z, w)
	SIG(GL_SUB_RASTER_POS4D, 4, 0x0F);
	// convolution_parameterf(target, pname, params) -- target=int, pname=int, params=float
	SIG(GL_SUB_CONVOLUTION_PARAMETERF, 3, 0x04);

	// Extension functions with floats:
	// blend_color_EXT(r, g, b, a) -- 4 floats
	SIG(GL_SUB_BLEND_COLOR_EXT, 4, 0x0F);
	// blend_color (GL 1.2)(r, g, b, a)
	SIG(GL_SUB_BLEND_COLOR_1_2, 4, 0x0F);
	// secondary_color3f_EXT(r, g, b)
	SIG(GL_SUB_SECONDARY_COLOR3F_EXT, 3, 0x07);
	// secondary_color3d_EXT(r, g, b)
	SIG(GL_SUB_SECONDARYCOLOR3D_EXT, 3, 0x07);
	// multi_tex_coord1f_ARB(target, s) -- target=int, s=float
	SIG(GL_SUB_MULTI_TEX_COORD1F_ARB, 2, 0x02);
	// multi_tex_coord2f_ARB(target, s, t)
	SIG(GL_SUB_MULTI_TEX_COORD2F_ARB, 3, 0x06);
	// multi_tex_coord3f_ARB(target, s, t, r)
	SIG(GL_SUB_MULTI_TEX_COORD3F_ARB, 4, 0x0E);
	// multi_tex_coord4f_ARB(target, s, t, r, q)
	SIG(GL_SUB_MULTI_TEX_COORD4F_ARB, 5, 0x1E);
	// multi_tex_coord*d_ARB doubles (target=int, rest=double)
	SIG(GL_SUB_MULTI_TEX_COORD1D_ARB, 2, 0x02);
	SIG(GL_SUB_MULTI_TEX_COORD2D_ARB, 3, 0x06);
	SIG(GL_SUB_MULTI_TEX_COORD3D_ARB, 4, 0x0E);
	SIG(GL_SUB_MULTI_TEX_COORD4D_ARB, 5, 0x1E);

	// GLU functions with floats:
	// gluPerspective(fovy, aspect, zNear, zFar) -- 4 doubles
	SIG(GL_SUB_GLU_PERSPECTIVE, 4, 0x0F);
	// gluLookAt(eyeX..upZ) -- 9 doubles
	SIG(GL_SUB_GLU_LOOKAT, 9, 0x1FF);
	// gluOrtho2D(left, right, bottom, top) -- 4 doubles
	SIG(GL_SUB_GLU_ORTHO2D, 4, 0x0F);
	// gluSphere(quad, radius, slices, stacks)
	SIG(GL_SUB_GLU_SPHERE, 4, 0x02);  // quad=ptr, radius=double, slices=int, stacks=int
	// gluCylinder(quad, base, top, height, slices, stacks)
	SIG(GL_SUB_GLU_CYLINDER, 6, 0x0E);  // quad=ptr, base=dbl, top=dbl, height=dbl, slices=int, stacks=int
	// gluDisk(quad, inner, outer, slices, loops)
	SIG(GL_SUB_GLU_DISK, 5, 0x06);  // quad=ptr, inner=dbl, outer=dbl, slices=int, loops=int
	// gluPickMatrix(x, y, delX, delY, viewport) -- 4 doubles + ptr
	SIG(GL_SUB_GLU_PICKMATRIX, 5, 0x0F);

	// GLUT functions with floats/doubles:
	// glutWireSphere(radius, slices, stacks) -- radius=double
	SIG(GL_SUB_GLUT_WIRESPHERE, 3, 0x01);
	// glutSolidSphere(radius, slices, stacks)
	SIG(GL_SUB_GLUT_SOLIDSPHERE, 3, 0x01);
	// glutWireCone(base, height, slices, stacks) -- 2 doubles
	SIG(GL_SUB_GLUT_WIRECONE, 4, 0x03);
	// glutSolidCone(base, height, slices, stacks)
	SIG(GL_SUB_GLUT_SOLIDCONE, 4, 0x03);
	// glutWireCube(size) -- 1 double
	SIG(GL_SUB_GLUT_WIRECUBE, 1, 0x01);
	// glutSolidCube(size)
	SIG(GL_SUB_GLUT_SOLIDCUBE, 1, 0x01);
	// glutWireTorus(innerRadius, outerRadius, sides, rings) -- 2 doubles
	SIG(GL_SUB_GLUT_WIRETORUS, 4, 0x03);
	// glutSolidTorus
	SIG(GL_SUB_GLUT_SOLIDTORUS, 4, 0x03);
	// glutWireTeapot(size) -- 1 double
	SIG(GL_SUB_GLUT_WIRETEAPOT, 1, 0x01);
	// glutSolidTeapot
	SIG(GL_SUB_GLUT_SOLIDTEAPOT, 1, 0x01);
	// glutSetColor(ndx, r, g, b) -- ndx=int, r/g/b=float
	SIG(GL_SUB_GLUT_SETCOLOR, 4, 0x0E);

	// Map functions with float/double args:
	// map1f(target, u1, u2, stride, order, points)
	SIG(GL_SUB_MAP1F, 6, 0x06);  // target=int, u1=float, u2=float, stride=int, order=int, points=ptr
	// map1d(target, u1, u2, stride, order, points)
	SIG(GL_SUB_MAP1D, 6, 0x06);
	// map2f(target, u1, u2, ustride, uorder, v1, v2, vstride, vorder, points)
	SIG(GL_SUB_MAP2F, 10, 0x66);  // floats at positions 1,2,5,6 = 0x66
	// map2d(target, u1, u2, ustride, uorder, v1, v2, vstride, vorder, points)
	SIG(GL_SUB_MAP2D, 10, 0x66);
	// map_grid1f(un, u1, u2) -- un=int, u1=float, u2=float
	SIG(GL_SUB_MAP_GRID1F, 3, 0x06);
	// map_grid1d(un, u1, u2)
	SIG(GL_SUB_MAP_GRID1D, 3, 0x06);
	// map_grid2f(un, u1, u2, vn, v1, v2) -- un=int, u1=float, u2=float, vn=int, v1=float, v2=float
	SIG(GL_SUB_MAP_GRID2F, 6, 0x36);  // float_mask: bits 1,2,4,5 = 0x36
	// map_grid2d(un, u1, u2, vn, v1, v2) -- same pattern with doubles
	SIG(GL_SUB_MAP_GRID2D, 6, 0x36);

	// GLU functions with float/double args (previously missing):
	// gluProject(objX, objY, objZ, model, proj, viewport, winX, winY, winZ)
	SIG(GL_SUB_GLU_PROJECT, 9, 0x07);  // doubles at positions 0,1,2
	// gluUnProject(winX, winY, winZ, model, proj, viewport, objX, objY, objZ)
	SIG(GL_SUB_GLU_UNPROJECT, 9, 0x07);
	// gluPartialDisk(quad, inner, outer, slices, loops, startAngle, sweepAngle)
	SIG(GL_SUB_GLU_PARTIALDISK, 7, 0x66);  // doubles at positions 1,2,5,6
	// gluTessProperty(tess, which, data)
	SIG(GL_SUB_GLU_TESSPROPERTY, 3, 0x04);  // double at position 2
	// gluTessNormal(tess, x, y, z)
	SIG(GL_SUB_GLU_TESSNORMAL, 4, 0x0E);  // doubles at positions 1,2,3
	// gluNurbsProperty(nurb, property, value)
	SIG(GL_SUB_GLU_NURBSPROPERTY, 3, 0x04);  // float at position 2

	#undef SIG
}

/*
 *  Initialize all GL TVECTs
 */
bool GLThunksInit(void)
{
	// Allocate scratch word
	gl_scratch_addr = SheepMem::Reserve(4);
	WriteMacInt32(gl_scratch_addr, 0);

	// Allocate dispatch-table flag word
	gl_dt_flag_addr = SheepMem::Reserve(4);
	WriteMacInt32(gl_dt_flag_addr, 0);

	// Track A immediate-mode batching: guest-memory command ring + control words.
	gl_defer_ring_base    = SheepMem::Reserve(GL_DEFER_RING_SIZE);
	gl_defer_head_addr    = SheepMem::Reserve(4); WriteMacInt32(gl_defer_head_addr, 0);
	gl_defer_count_addr   = SheepMem::Reserve(4); WriteMacInt32(gl_defer_count_addr, 0);
	gl_defer_enabled_addr = SheepMem::Reserve(4); WriteMacInt32(gl_defer_enabled_addr, 1); // default ON
	GLDeferBuildDescriptors();

	// Get the native opcode for GL dispatch
	uint32 gl_opcode = NativeOpcode(NATIVE_OPENGL_DISPATCH);

	// Track A: emit the shared deferral fallback routine BEFORE the TVECT loop,
	// so gl_defer_common_tail is set before any deferrable stub `b`s to it. This is
	// the live fallback target the deferrable stubs branch to when deferral is
	// disabled or the ring is full (stub routing landed in Task 8).
	EmitDeferFallback(gl_opcode);

	// Clear the tvects arrays
	memset(gl_method_tvects, 0, sizeof(gl_method_tvects));
	memset(gl_dt_method_tvects, 0, sizeof(gl_dt_method_tvects));
	memset(gl_dt_diag_tvects, 0, sizeof(gl_dt_diag_tvects));

	int tvect_count = 0;

	// Core GL (0-335): 336 TVECTs (stub-patching + dispatch-table variants)
	for (int i = GL_CORE_FIRST; i <= GL_CORE_LAST; i++) {
		// Track A: deferrable immediate-mode setters get a capture stub that
		// buffers into the ring; everything else keeps the plain trap TVECT.
		// The dispatch-table variant is always the plain dt TVECT (dt path is
		// out of scope for batching).
		gl_method_tvects[i] = GLDeferIsDeferrable(i)
			? EmitDeferStub(i, gl_defer_desc[i], gl_opcode)
			: AllocateGLTVECT(i, gl_opcode);
		gl_dt_method_tvects[i] = AllocateGLDispatchTableTVECT(i, gl_opcode);
		tvect_count++;
	}

	// Diagnostic no-op TVECTs: one per context-handle dispatch slot. Games that
	// read GL function descriptors directly from the context handle may index
	// slots beyond core 1.1 (extension entry points). GLPopulateDispatchTable
	// installs one of these in every slot it can't map to a real GL function, so
	// such a read hits a safe logging no-op instead of uninitialized guest heap.
	// The slot is encoded in the sub-opcode (GL_DT_DIAG_BASE + slot) so the
	// handler can report exactly which slot the game read.
	for (int s = 0; s < GL_CTX_DISPATCH_SLOTS; s++) {
		gl_dt_diag_tvects[s] = AllocateGLDispatchTableTVECT(GL_DT_DIAG_BASE + s, gl_opcode);
		tvect_count++;
	}

	// Extensions (400-503): stub-patch TVECT + dispatch-table TVECT. The latter
	// lets games that read extension entry points directly from the context
	// handle's dispatch table (e.g. Cro-Mag Rally's glLockArraysEXT at slot 339)
	// reach the real handler instead of the diagnostic no-op. Placement into the
	// table is driven by gl_dispatch_ext_slots[] in GLPopulateDispatchTable.
	for (int i = GL_EXT_FIRST; i <= GL_EXT_LAST; i++) {
		gl_method_tvects[i] = GLDeferIsDeferrable(i)
			? EmitDeferStub(i, gl_defer_desc[i], gl_opcode)
			: AllocateGLTVECT(i, gl_opcode);
		gl_dt_method_tvects[i] = AllocateGLDispatchTableTVECT(i, gl_opcode);
		tvect_count++;
	}

	// AGL (600-632): 33 TVECTs
	for (int i = GL_AGL_FIRST; i <= GL_AGL_LAST; i++) {
		gl_method_tvects[i] = AllocateGLTVECT(i, gl_opcode);
		tvect_count++;
	}

	// GLU (700-753): 54 TVECTs
	for (int i = GL_GLU_FIRST; i <= GL_GLU_LAST; i++) {
		gl_method_tvects[i] = AllocateGLTVECT(i, gl_opcode);
		tvect_count++;
	}

	// GLUT (800-915): 116 TVECTs
	for (int i = GL_GLUT_FIRST; i <= GL_GLUT_LAST; i++) {
		gl_method_tvects[i] = AllocateGLTVECT(i, gl_opcode);
		tvect_count++;
	}

	// Initialize function signature table
	InitFuncSignatures();

	GL_LOG("GLThunksInit: allocated %d TVECTs (%d bytes), scratch at 0x%08x",
	       tvect_count, tvect_count * 32, gl_scratch_addr);
	GL_LOG("  Core GL:    %d (sub %d-%d)", GL_CORE_LAST - GL_CORE_FIRST + 1, GL_CORE_FIRST, GL_CORE_LAST);
	GL_LOG("  Extensions: %d (sub %d-%d)", GL_EXT_LAST - GL_EXT_FIRST + 1, GL_EXT_FIRST, GL_EXT_LAST);
	GL_LOG("  AGL:        %d (sub %d-%d)", GL_AGL_LAST - GL_AGL_FIRST + 1, GL_AGL_FIRST, GL_AGL_LAST);
	GL_LOG("  GLU:        %d (sub %d-%d)", GL_GLU_LAST - GL_GLU_FIRST + 1, GL_GLU_FIRST, GL_GLU_LAST);
	GL_LOG("  GLUT:       %d (sub %d-%d)", GL_GLUT_LAST - GL_GLUT_FIRST + 1, GL_GLUT_FIRST, GL_GLUT_LAST);
	GL_LOG("GLThunksInit: deferred-batching ring at 0x%08x size %d", gl_defer_ring_base, GL_DEFER_RING_SIZE);
	GL_LOG("GLThunksInit: deferral fallback routine at 0x%08x", gl_defer_common_tail);

	return true;
}

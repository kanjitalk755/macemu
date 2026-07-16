#ifndef GL_PPC_EMIT_H
#define GL_PPC_EMIT_H
/*
 *  gl_ppc_emit.h - 32-bit PowerPC instruction encoders for Track-A batching
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  Hand-written PPC machine-code encoders used to author the GL deferred-
 *  batching thunks (Track A). Each function returns the 32-bit big-endian
 *  instruction WORD (the value you pass to WriteMacInt32, which stores it
 *  big-endian into guest memory).
 *
 *  These mirror the inline encodings already used by AllocateGLTVECT in
 *  gl_thunks.cpp (lis/ori/li/stw/blr) and the NATIVE_*_DISPATCH tail, but
 *  factored into named, individually testable helpers so Task 8 can emit the
 *  deferrable capture stubs (and the shared fallback) without open-coding the
 *  bit math at every site.
 *
 *  Header-only / all `static inline`: no translation unit, so no Xcode target
 *  membership is needed, and a standalone host test can include this header
 *  directly to verify the encoders against an assembler.
 *
 *  Register numbering: GPRs r0..r31 and FPRs f0..f31 are 0..31.
 *  Displacements (d) and immediates are masked to 16 bits; signed values are
 *  two's-complement and only their low 16 bits are encoded (the hardware
 *  sign-extends D-form displacements / SI immediates at execution).
 *
 *  All encoders verified against llvm-mc (ppc32) ground truth -- see
 *  tests/gl_ppc_emit_test.cpp for the comparison harness and the b/bc
 *  displacement round-trips.
 */

#include <cstdint>
#include <cassert>

/* ------------------------------------------------------------------------- *
 *  XO-form integer arithmetic (register, register)
 *
 *  XO-form layout (32 bits, MSB..LSB):
 *      [ 0..5 ] primary opcode 31 (0x1F)
 *      [ 6..10] D  (target register, 5 bits)  -> shift << 21
 *      [11..15] A  (source register, 5 bits)  -> shift << 16
 *      [16..20] B  (source register, 5 bits)  -> shift << 11
 *      [21    ] OE (overflow-enable, 0 here)
 *      [22..30] extended opcode 266 (add)     -> 266 << 1 = 0x214
 *      [31    ] Rc (record, 0 here)
 * ------------------------------------------------------------------------- */

// add D, A, B  ->  D = A + B (XO-form, OE=0 Rc=0). Encoded base 0x7C000214.
//   add 11,11,0 -> 0x7D6B0214 (verified vs llvm-mc, see gl_ppc_emit_test.cpp).
static inline uint32_t ppc_add(int D, int A, int B) {
    return 0x7C000214u | ((uint32_t)(D & 31) << 21) | ((uint32_t)(A & 31) << 16) |
           ((uint32_t)(B & 31) << 11);
}

/* ------------------------------------------------------------------------- *
 *  D-form load/store and arithmetic-immediate instructions
 *
 *  D-form layout (32 bits, MSB..LSB):
 *      [ 0..5 ] primary opcode (6 bits)
 *      [ 6..10] D / S  (target or source register, 5 bits) -> shift << 21
 *      [11..15] A      (base / source register, 5 bits)     -> shift << 16
 *      [16..31] d / SI / UI (16-bit displacement or immediate)
 * ------------------------------------------------------------------------- */

// lis D, imm16  ->  addis D,0,imm16 (load immediate, shifted; A field = 0)
//   primary opcode 15 (0x3C). With A=0 this is the "load high half" idiom.
static inline uint32_t ppc_lis(int D, uint32_t imm16) {
    return 0x3C000000u | ((uint32_t)(D & 31) << 21) | (imm16 & 0xFFFFu);
}

// ori A, S, imm16  ->  A = S | (imm16 & 0xFFFF). Primary opcode 24 (0x60).
//   Note D-form S is the [6..10] field (<<21) and A is the [11..15] field (<<16).
static inline uint32_t ppc_ori(int A, int S, uint32_t imm16) {
    return 0x60000000u | ((uint32_t)(S & 31) << 21) | ((uint32_t)(A & 31) << 16) |
           (imm16 & 0xFFFFu);
}

// addi D, A, simm16  ->  D = A + sign_extend(simm16). Primary opcode 14 (0x38).
//   If A == 0 the constant 0 is used, making this "li" (see ppc_li).
static inline uint32_t ppc_addi(int D, int A, int32_t simm16) {
    return 0x38000000u | ((uint32_t)(D & 31) << 21) | ((uint32_t)(A & 31) << 16) |
           ((uint32_t)simm16 & 0xFFFFu);
}

// li D, simm16  ->  addi D,0,simm16 (load signed immediate).
static inline uint32_t ppc_li(int D, int32_t simm16) {
    return ppc_addi(D, 0, simm16);
}

// lwz D, d(A)  ->  D = mem32[A + sign_extend(d)]. Primary opcode 32 (0x80).
static inline uint32_t ppc_lwz(int D, int32_t d, int A) {
    return 0x80000000u | ((uint32_t)(D & 31) << 21) | ((uint32_t)(A & 31) << 16) |
           ((uint32_t)d & 0xFFFFu);
}

// stw S, d(A)  ->  mem32[A + sign_extend(d)] = S. Primary opcode 36 (0x90).
static inline uint32_t ppc_stw(int S, int32_t d, int A) {
    return 0x90000000u | ((uint32_t)(S & 31) << 21) | ((uint32_t)(A & 31) << 16) |
           ((uint32_t)d & 0xFFFFu);
}

// lbz D, d(A)  ->  D = zero_ext(mem8[A + d]). Primary opcode 34 (0x88).
static inline uint32_t ppc_lbz(int D, int32_t d, int A) {
    return 0x88000000u | ((uint32_t)(D & 31) << 21) | ((uint32_t)(A & 31) << 16) |
           ((uint32_t)d & 0xFFFFu);
}

// stb S, d(A)  ->  mem8[A + d] = S[24..31]. Primary opcode 38 (0x98).
static inline uint32_t ppc_stb(int S, int32_t d, int A) {
    return 0x98000000u | ((uint32_t)(S & 31) << 21) | ((uint32_t)(A & 31) << 16) |
           ((uint32_t)d & 0xFFFFu);
}

// lhz D, d(A)  ->  D = zero_ext(mem16[A + d]). Primary opcode 40 (0xA0).
static inline uint32_t ppc_lhz(int D, int32_t d, int A) {
    return 0xA0000000u | ((uint32_t)(D & 31) << 21) | ((uint32_t)(A & 31) << 16) |
           ((uint32_t)d & 0xFFFFu);
}

// sth S, d(A)  ->  mem16[A + d] = S[16..31]. Primary opcode 44 (0xB0).
static inline uint32_t ppc_sth(int S, int32_t d, int A) {
    return 0xB0000000u | ((uint32_t)(S & 31) << 21) | ((uint32_t)(A & 31) << 16) |
           ((uint32_t)d & 0xFFFFu);
}

// stfs S, d(A)  ->  mem32[A + d] = single(FPR S). Primary opcode 52 (0xD0).
//   S is an FPR (f0..f31); same D-form field layout as the GPR stores.
static inline uint32_t ppc_stfs(int S, int32_t d, int A) {
    return 0xD0000000u | ((uint32_t)(S & 31) << 21) | ((uint32_t)(A & 31) << 16) |
           ((uint32_t)d & 0xFFFFu);
}

/* ------------------------------------------------------------------------- *
 *  Compare-immediate (D-form, cr0, L=0 -> 32-bit compare)
 *
 *      [ 0..5 ] primary opcode
 *      [ 6..8 ] BF  (target CR field; 0 = cr0 -> left as 0 here)
 *      [   9  ] (reserved, 0)
 *      [  10  ] L   (0 -> 32-bit) -> 0 here
 *      [11..15] A   (register being compared) -> shift << 16
 *      [16..31] SI / UI (immediate)
 * ------------------------------------------------------------------------- */

// cmpwi A, simm16  ->  cmpi cr0, 0(=L), A, simm16. Primary opcode 11 (0x2C).
static inline uint32_t ppc_cmpwi(int A, int32_t simm16) {
    return 0x2C000000u | ((uint32_t)(A & 31) << 16) | ((uint32_t)simm16 & 0xFFFFu);
}

// cmplwi A, uimm16  ->  cmpli cr0, 0(=L), A, uimm16. Primary opcode 10 (0x28).
static inline uint32_t ppc_cmplwi(int A, uint32_t uimm16) {
    return 0x28000000u | ((uint32_t)(A & 31) << 16) | (uimm16 & 0xFFFFu);
}

/* ------------------------------------------------------------------------- *
 *  Branches
 * ------------------------------------------------------------------------- */

// blr  ->  bclr 20,0 : branch unconditionally to LR (return). 0x4E800020.
static inline uint32_t ppc_blr(void) {
    return 0x4E800020u;
}

// b  (I-form, AA=0 LK=0): branch to (fromAddr + sign_extend(LI||0b00)).
//   We compute LI from the byte delta (toAddr - fromAddr); the two low bits are
//   forced 0 by the 0x03FFFFFC mask. Range is +/- 32 MiB. Primary opcode 18.
//
//   layout: [0..5]=18(0x48>>2) [6..29]=LI [30]=AA=0 [31]=LK=0
//   so the 24-bit field already includes the low two (zero) bits when we mask
//   the byte delta with 0x03FFFFFC.
static inline uint32_t ppc_b(uint32_t fromAddr, uint32_t toAddr) {
    // Range/alignment check (Task 7): the I-form branch field is a signed 26-bit
    // byte displacement (24-bit LI + 2 implied zero bits). Assert the delta is
    // 4-byte aligned and fits signed-26 BEFORE masking, so an out-of-range target
    // traps in debug instead of silently truncating into the wrong branch.
    const int32_t delta = (int32_t)(toAddr - fromAddr);
    assert((delta & 0x3) == 0 && "ppc_b target not 4-byte aligned");
    assert(delta >= -(1 << 25) && delta < (1 << 25) && "ppc_b displacement out of signed-26 range");
    return 0x48000000u | ((uint32_t)delta & 0x03FFFFFCu);
}

// bc BO,BI (B-form, AA=0 LK=0): conditional branch to fromAddr + delta.
//   The relative displacement field BD is 14 bits + 2 implied zero bits; we
//   mask the byte delta with 0xFFFC. Range is +/- 32 KiB. Primary opcode 16.
//
//   layout: [0..5]=16(0x40>>2) [6..10]=BO [11..15]=BI [16..29]=BD [30]=AA [31]=LK
static inline uint32_t ppc_bc(int BO, int BI, uint32_t fromAddr, uint32_t toAddr) {
    // Range/alignment check (Task 7): the B-form displacement is a signed 16-bit
    // byte field (14-bit BD + 2 implied zero bits). Assert 4-byte alignment and
    // signed-16 fit BEFORE masking so an out-of-range short branch traps in debug.
    const int32_t delta = (int32_t)(toAddr - fromAddr);
    assert((delta & 0x3) == 0 && "ppc_bc target not 4-byte aligned");
    assert(delta >= -(1 << 15) && delta < (1 << 15) && "ppc_bc displacement out of signed-16 range");
    return 0x40000000u | ((uint32_t)(BO & 31) << 21) | ((uint32_t)(BI & 31) << 16) |
           ((uint32_t)delta & 0xFFFCu);
}

// Convenience conditional branches on cr0.
//   cr0 BI bits: 0 = LT, 1 = GT, 2 = EQ.
//   BO 12 (0b01100) = branch if the BI condition bit is TRUE;
//   BO  4 (0b00100) = branch if the BI condition bit is FALSE.
static inline uint32_t ppc_beq(uint32_t from, uint32_t to) { return ppc_bc(12, 2, from, to); } // EQ true
static inline uint32_t ppc_bne(uint32_t from, uint32_t to) { return ppc_bc(4,  2, from, to); } // EQ false
static inline uint32_t ppc_bgt(uint32_t from, uint32_t to) { return ppc_bc(12, 1, from, to); } // GT true
static inline uint32_t ppc_ble(uint32_t from, uint32_t to) { return ppc_bc(4,  1, from, to); } // GT false

#endif  // GL_PPC_EMIT_H

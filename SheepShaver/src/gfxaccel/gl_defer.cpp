/*
 *  gl_defer.cpp - Track-A GL immediate-mode deferred-batching descriptor table
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  ---------------------------------------------------------------------------
 *  Task 2 scope: the hand-authored descriptor table only.
 *
 *  Each GLDeferDesc tells the deferred-batching ring how to snapshot the args
 *  of one immediate-mode setter call so it can be replayed later through
 *  GLDispatch verbatim. The table is HAND-AUTHORED: gl_func_signatures[] only
 *  covers float functions ({0,0} for integer-only calls), so it cannot be used
 *  to derive integer GPR arg counts.
 *
 *  Argument wire model (matches gl_dispatch.cpp / sheepshaver_glue.cpp):
 *    - scalar_gpr_count integer/pointer args arrive verbatim in r3.. .
 *    - Each floating arg (float OR double) occupies exactly ONE 32-bit slot in
 *      float_bits[] (the glue casts every FPR value down to a 32-bit float),
 *      so fpr_mask treats f and d scalar variants identically.
 *    - The "...v" array variants take scalar_gpr_count int args followed by a
 *      single trailing pointer (at GPR index scalar_gpr_count); ptr_bytes is the
 *      exact guest array size to snapshot = component_count * per-component size
 *      (f=4 d=8 i=4 s=2 b=1 ub=1 us=2 ui=4).
 *    - An opcode is EITHER scalar/FP (ptr_bytes==0) OR trailing-pointer; never
 *      both. deferrable==1 marks every authored entry.
 *
 *  Task 3 scope (added below): GLDeferDecodeRecord() -- reads one captured
 *  record out of the guest ring and reconstructs the exact (gpr[], float_bits[])
 *  argument tuple the original trapped call would have handed GLDispatch, so the
 *  record can be replayed verbatim. The trailing-pointer snapshot is exposed as
 *  an IN-RING guest address (not the original, possibly-reused, source pointer).
 *
 *  NOT in scope here (later Track-A tasks): the SheepMem ring address externs
 *  and GLDrainDeferred().
 */

#include "gl_defer.h"

#include <cstdint>
#include <cstring>   // memset
#include <cassert>

// Guest big-endian memory read resolves to the vm_read_memory_4 inline in
// cpu_emulation.h (which honors guest endianness).
#include "sysdeps.h"
#include "cpu_emulation.h"   // ReadMacInt32 / WriteMacInt32

// Real-build symbols GLDrainDeferred replays through. Declared here (no extra
// header exposes them); definitions live in gl_thunks.cpp / gl_dispatch.cpp.
extern uint32_t gl_scratch_addr;            // gl_thunks.cpp: scratch word holding the sub-opcode
extern uint32_t gl_ppc_sp;                  // gl_dispatch.cpp: PPC SP for stack-arg reads
extern int      gl_ppc_stack_arg_offset;    // gl_dispatch.cpp: dispatch-table stack shift
extern uint32_t GLDispatch(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                           uint32_t r7, uint32_t r8, uint32_t r9, uint32_t r10,
                           const uint32_t *float_bits, int num_float_args);

// Host authoritative descriptor table + host re-entrancy guard.
GLDeferDesc gl_defer_desc[GL_MAX_SUBOPCODE];
bool gl_defer_draining = false;

// Permanent failsafe latch. Set true the first time GLDrainDeferred detects ring
// corruption; once set, deferral must never be re-enabled for the rest of the
// session (glEndList consults this before re-enabling). Latching prevents the
// failsafe from "flapping" -- re-tripping and dropping vertices every batch when
// glEndList would otherwise unconditionally re-enable deferral.
bool gl_defer_failsafe_tripped = false;

// Diagnostic counters for the deferred-batching path.
// Logging is gated by gl_logging_enabled (via GL_LOG) so these are silent in
// shipping/logging-off builds; the counters themselves are always incremented
// (uint64 wraps in ~100 years at 60fps, negligible).
uint64_t g_defer_records_drained = 0;  // total records replayed across all drain calls
uint64_t g_defer_drain_calls     = 0;  // total GLDrainDeferred calls that did work
uint64_t g_defer_fallbacks       = 0;  // deferrable opcodes that hit GLDispatch directly

// SheepMem ring addresses (set in GLDeferInit / GLThunksInit in a later task).
// Defined here, initialized to 0 so the ring reads as "not allocated" until then;
// GLDrainDeferred null-guards on gl_defer_count_addr == 0. Harmless in the
// standalone unit-test build (nothing touches them there).
uint32_t gl_defer_ring_base    = 0;
uint32_t gl_defer_head_addr    = 0;
uint32_t gl_defer_count_addr   = 0;
uint32_t gl_defer_enabled_addr = 0;
uint32_t gl_defer_common_tail  = 0;

namespace {

// One authored row: opcode + its descriptor fields. deferrable is forced to 1
// when scattered, so it is not repeated here.
struct GLDeferRow {
    uint16_t op;
    uint8_t  scalar_gpr_count;
    uint16_t fpr_mask;
    uint8_t  ptr_bytes;
};

// Common fpr masks for N consecutive floating args (each one FPR slot).
//   1 float -> 0x1, 2 -> 0x3, 3 -> 0x7, 4 -> 0xF.
#define FPR1 0x1
#define FPR2 0x3
#define FPR3 0x7
#define FPR4 0xF

// Per-component byte sizes for the "...v" array variants.
#define BV_b  1
#define BV_ub 1
#define BV_s  2
#define BV_us 2
#define BV_i  4
#define BV_ui 4
#define BV_f  4
#define BV_d  8

// The hand-authored deferrable table. Only immediate-mode per-vertex setters
// are present; state changes, queries, glEnd, glArrayElement, glEvalCoord*,
// glEvalMesh*, glEvalPoint* and glRect* are intentionally absent.
//
// Variants with no GL_SUB_ constant in gl_engine.h are simply omitted (no row);
// see the SKIPPED list in the source-control commit / task report. In this
// codebase those are: every glFogCoord* variant (only glFog* parameter setters
// exist as GL_SUB_FOG{F,FV,I,IV}, which are NOT per-vertex setters).
const GLDeferRow kRows[] = {
    // --- glBegin --------------------------------------------------------------
    // glBegin(GLenum mode): one int GPR.
    { GL_SUB_BEGIN, 1, 0, 0 },

    // --- glVertex{2,3,4}{s,i,f,d} + v variants --------------------------------
    // 2 components
    { GL_SUB_VERTEX2S, 2, 0, 0 },
    { GL_SUB_VERTEX2I, 2, 0, 0 },
    { GL_SUB_VERTEX2F, 0, FPR2, 0 },
    { GL_SUB_VERTEX2D, 0, FPR2, 0 },
    { GL_SUB_VERTEX2SV, 0, 0, 2 * BV_s },
    { GL_SUB_VERTEX2IV, 0, 0, 2 * BV_i },
    { GL_SUB_VERTEX2FV, 0, 0, 2 * BV_f },
    { GL_SUB_VERTEX2DV, 0, 0, 2 * BV_d },
    // 3 components
    { GL_SUB_VERTEX3S, 3, 0, 0 },
    { GL_SUB_VERTEX3I, 3, 0, 0 },
    { GL_SUB_VERTEX3F, 0, FPR3, 0 },
    { GL_SUB_VERTEX3D, 0, FPR3, 0 },
    { GL_SUB_VERTEX3SV, 0, 0, 3 * BV_s },
    { GL_SUB_VERTEX3IV, 0, 0, 3 * BV_i },
    { GL_SUB_VERTEX3FV, 0, 0, 3 * BV_f },
    { GL_SUB_VERTEX3DV, 0, 0, 3 * BV_d },
    // 4 components
    { GL_SUB_VERTEX4S, 4, 0, 0 },
    { GL_SUB_VERTEX4I, 4, 0, 0 },
    { GL_SUB_VERTEX4F, 0, FPR4, 0 },
    { GL_SUB_VERTEX4D, 0, FPR4, 0 },
    { GL_SUB_VERTEX4SV, 0, 0, 4 * BV_s },
    { GL_SUB_VERTEX4IV, 0, 0, 4 * BV_i },
    { GL_SUB_VERTEX4FV, 0, 0, 4 * BV_f },
    { GL_SUB_VERTEX4DV, 0, 0, 4 * BV_d },

    // --- glColor{3,4}{b,s,i,f,d,ub,us,ui} + v variants ------------------------
    // 3 components
    { GL_SUB_COLOR3B, 3, 0, 0 },
    { GL_SUB_COLOR3S, 3, 0, 0 },
    { GL_SUB_COLOR3I, 3, 0, 0 },
    { GL_SUB_COLOR3F, 0, FPR3, 0 },
    { GL_SUB_COLOR3D, 0, FPR3, 0 },
    { GL_SUB_COLOR3UB, 3, 0, 0 },
    { GL_SUB_COLOR3US, 3, 0, 0 },
    { GL_SUB_COLOR3UI, 3, 0, 0 },
    { GL_SUB_COLOR3BV, 0, 0, 3 * BV_b },
    { GL_SUB_COLOR3SV, 0, 0, 3 * BV_s },
    { GL_SUB_COLOR3IV, 0, 0, 3 * BV_i },
    { GL_SUB_COLOR3FV, 0, 0, 3 * BV_f },
    { GL_SUB_COLOR3DV, 0, 0, 3 * BV_d },
    { GL_SUB_COLOR3UBV, 0, 0, 3 * BV_ub },
    { GL_SUB_COLOR3USV, 0, 0, 3 * BV_us },
    { GL_SUB_COLOR3UIV, 0, 0, 3 * BV_ui },
    // 4 components
    { GL_SUB_COLOR4B, 4, 0, 0 },
    { GL_SUB_COLOR4S, 4, 0, 0 },
    { GL_SUB_COLOR4I, 4, 0, 0 },
    { GL_SUB_COLOR4F, 0, FPR4, 0 },
    { GL_SUB_COLOR4D, 0, FPR4, 0 },
    { GL_SUB_COLOR4UB, 4, 0, 0 },
    { GL_SUB_COLOR4US, 4, 0, 0 },
    { GL_SUB_COLOR4UI, 4, 0, 0 },
    { GL_SUB_COLOR4BV, 0, 0, 4 * BV_b },
    { GL_SUB_COLOR4SV, 0, 0, 4 * BV_s },
    { GL_SUB_COLOR4IV, 0, 0, 4 * BV_i },
    { GL_SUB_COLOR4FV, 0, 0, 4 * BV_f },
    { GL_SUB_COLOR4DV, 0, 0, 4 * BV_d },
    { GL_SUB_COLOR4UBV, 0, 0, 4 * BV_ub },
    { GL_SUB_COLOR4USV, 0, 0, 4 * BV_us },
    { GL_SUB_COLOR4UIV, 0, 0, 4 * BV_ui },

    // --- glTexCoord{1,2,3,4}{s,i,f,d} + v variants ----------------------------
    // 1 component
    { GL_SUB_TEX_COORD1S, 1, 0, 0 },
    { GL_SUB_TEX_COORD1I, 1, 0, 0 },
    { GL_SUB_TEX_COORD1F, 0, FPR1, 0 },
    { GL_SUB_TEX_COORD1D, 0, FPR1, 0 },
    { GL_SUB_TEX_COORD1SV, 0, 0, 1 * BV_s },
    { GL_SUB_TEX_COORD1IV, 0, 0, 1 * BV_i },
    { GL_SUB_TEX_COORD1FV, 0, 0, 1 * BV_f },
    { GL_SUB_TEX_COORD1DV, 0, 0, 1 * BV_d },
    // 2 components
    { GL_SUB_TEX_COORD2S, 2, 0, 0 },
    { GL_SUB_TEX_COORD2I, 2, 0, 0 },
    { GL_SUB_TEX_COORD2F, 0, FPR2, 0 },
    { GL_SUB_TEX_COORD2D, 0, FPR2, 0 },
    { GL_SUB_TEX_COORD2SV, 0, 0, 2 * BV_s },
    { GL_SUB_TEX_COORD2IV, 0, 0, 2 * BV_i },
    { GL_SUB_TEX_COORD2FV, 0, 0, 2 * BV_f },
    { GL_SUB_TEX_COORD2DV, 0, 0, 2 * BV_d },
    // 3 components
    { GL_SUB_TEX_COORD3S, 3, 0, 0 },
    { GL_SUB_TEX_COORD3I, 3, 0, 0 },
    { GL_SUB_TEX_COORD3F, 0, FPR3, 0 },
    { GL_SUB_TEX_COORD3D, 0, FPR3, 0 },
    { GL_SUB_TEX_COORD3SV, 0, 0, 3 * BV_s },
    { GL_SUB_TEX_COORD3IV, 0, 0, 3 * BV_i },
    { GL_SUB_TEX_COORD3FV, 0, 0, 3 * BV_f },
    { GL_SUB_TEX_COORD3DV, 0, 0, 3 * BV_d },
    // 4 components
    { GL_SUB_TEX_COORD4S, 4, 0, 0 },
    { GL_SUB_TEX_COORD4I, 4, 0, 0 },
    { GL_SUB_TEX_COORD4F, 0, FPR4, 0 },
    { GL_SUB_TEX_COORD4D, 0, FPR4, 0 },
    { GL_SUB_TEX_COORD4SV, 0, 0, 4 * BV_s },
    { GL_SUB_TEX_COORD4IV, 0, 0, 4 * BV_i },
    { GL_SUB_TEX_COORD4FV, 0, 0, 4 * BV_f },
    { GL_SUB_TEX_COORD4DV, 0, 0, 4 * BV_d },

    // --- glNormal3{b,s,i,f,d} + v variants ------------------------------------
    { GL_SUB_NORMAL3B, 3, 0, 0 },
    { GL_SUB_NORMAL3S, 3, 0, 0 },
    { GL_SUB_NORMAL3I, 3, 0, 0 },
    { GL_SUB_NORMAL3F, 0, FPR3, 0 },
    { GL_SUB_NORMAL3D, 0, FPR3, 0 },
    { GL_SUB_NORMAL3BV, 0, 0, 3 * BV_b },
    { GL_SUB_NORMAL3SV, 0, 0, 3 * BV_s },
    { GL_SUB_NORMAL3IV, 0, 0, 3 * BV_i },
    { GL_SUB_NORMAL3FV, 0, 0, 3 * BV_f },
    { GL_SUB_NORMAL3DV, 0, 0, 3 * BV_d },

    // --- glMultiTexCoord{1,2,3,4}{s,i,f,d}ARB + v variants --------------------
    // The leading GLenum target is one int GPR; v variants take target + ptr.
    // fpr_mask bits are arg-position indices (bit 0 = arg0, bit 1 = arg1, ...).
    // arg0 is the integer GLenum target, so the FP components start at bit 1 --
    // hence (FPRn << 1), not FPRn.
    // 1 component
    { GL_SUB_MULTI_TEX_COORD1S_ARB, 2, 0, 0 },
    { GL_SUB_MULTI_TEX_COORD1I_ARB, 2, 0, 0 },
    { GL_SUB_MULTI_TEX_COORD1F_ARB, 1, (FPR1 << 1), 0 },
    { GL_SUB_MULTI_TEX_COORD1D_ARB, 1, (FPR1 << 1), 0 },
    { GL_SUB_MULTI_TEX_COORD1SV_ARB, 1, 0, 1 * BV_s },
    { GL_SUB_MULTI_TEX_COORD1IV_ARB, 1, 0, 1 * BV_i },
    { GL_SUB_MULTI_TEX_COORD1FV_ARB, 1, 0, 1 * BV_f },
    { GL_SUB_MULTI_TEX_COORD1DV_ARB, 1, 0, 1 * BV_d },
    // 2 components
    { GL_SUB_MULTI_TEX_COORD2S_ARB, 3, 0, 0 },
    { GL_SUB_MULTI_TEX_COORD2I_ARB, 3, 0, 0 },
    { GL_SUB_MULTI_TEX_COORD2F_ARB, 1, (FPR2 << 1), 0 },
    { GL_SUB_MULTI_TEX_COORD2D_ARB, 1, (FPR2 << 1), 0 },
    { GL_SUB_MULTI_TEX_COORD2SV_ARB, 1, 0, 2 * BV_s },
    { GL_SUB_MULTI_TEX_COORD2IV_ARB, 1, 0, 2 * BV_i },
    { GL_SUB_MULTI_TEX_COORD2FV_ARB, 1, 0, 2 * BV_f },
    { GL_SUB_MULTI_TEX_COORD2DV_ARB, 1, 0, 2 * BV_d },
    // 3 components
    { GL_SUB_MULTI_TEX_COORD3S_ARB, 4, 0, 0 },
    { GL_SUB_MULTI_TEX_COORD3I_ARB, 4, 0, 0 },
    { GL_SUB_MULTI_TEX_COORD3F_ARB, 1, (FPR3 << 1), 0 },
    { GL_SUB_MULTI_TEX_COORD3D_ARB, 1, (FPR3 << 1), 0 },
    { GL_SUB_MULTI_TEX_COORD3SV_ARB, 1, 0, 3 * BV_s },
    { GL_SUB_MULTI_TEX_COORD3IV_ARB, 1, 0, 3 * BV_i },
    { GL_SUB_MULTI_TEX_COORD3FV_ARB, 1, 0, 3 * BV_f },
    { GL_SUB_MULTI_TEX_COORD3DV_ARB, 1, 0, 3 * BV_d },
    // 4 components
    { GL_SUB_MULTI_TEX_COORD4S_ARB, 5, 0, 0 },
    { GL_SUB_MULTI_TEX_COORD4I_ARB, 5, 0, 0 },
    { GL_SUB_MULTI_TEX_COORD4F_ARB, 1, (FPR4 << 1), 0 },
    { GL_SUB_MULTI_TEX_COORD4D_ARB, 1, (FPR4 << 1), 0 },
    { GL_SUB_MULTI_TEX_COORD4SV_ARB, 1, 0, 4 * BV_s },
    { GL_SUB_MULTI_TEX_COORD4IV_ARB, 1, 0, 4 * BV_i },
    { GL_SUB_MULTI_TEX_COORD4FV_ARB, 1, 0, 4 * BV_f },
    { GL_SUB_MULTI_TEX_COORD4DV_ARB, 1, 0, 4 * BV_d },

    // --- glSecondaryColor3{b,s,i,f,d,ub,us,ui}EXT + v variants ----------------
    // (constant naming in gl_engine.h is inconsistent for the b/bv/d rows.)
    { GL_SUB_SECONDARYCOLOR3B_EXT, 3, 0, 0 },
    { GL_SUB_SECONDARY_COLOR3S_EXT, 3, 0, 0 },
    { GL_SUB_SECONDARY_COLOR3I_EXT, 3, 0, 0 },
    { GL_SUB_SECONDARY_COLOR3F_EXT, 0, FPR3, 0 },
    { GL_SUB_SECONDARYCOLOR3D_EXT, 0, FPR3, 0 },
    { GL_SUB_SECONDARY_COLOR3UB_EXT, 3, 0, 0 },
    { GL_SUB_SECONDARY_COLOR3US_EXT, 3, 0, 0 },
    { GL_SUB_SECONDARY_COLOR3UI_EXT, 3, 0, 0 },
    { GL_SUB_SECONDARYCOLOR3BV_EXT, 0, 0, 3 * BV_b },
    { GL_SUB_SECONDARY_COLOR3SV_EXT, 0, 0, 3 * BV_s },
    { GL_SUB_SECONDARY_COLOR3IV_EXT, 0, 0, 3 * BV_i },
    { GL_SUB_SECONDARY_COLOR3FV_EXT, 0, 0, 3 * BV_f },
    { GL_SUB_SECONDARY_COLOR3DV_EXT, 0, 0, 3 * BV_d },
    { GL_SUB_SECONDARY_COLOR3UBV_EXT, 0, 0, 3 * BV_ub },
    { GL_SUB_SECONDARY_COLOR3USV_EXT, 0, 0, 3 * BV_us },
    { GL_SUB_SECONDARY_COLOR3UIV_EXT, 0, 0, 3 * BV_ui },

    // --- glFogCoord{f,d} + v variants -----------------------------------------
    // SKIPPED: no GL_SUB_FOG_COORD*/GL_SUB_FOGCOORD* constants exist in
    // gl_engine.h. (GL_SUB_FOG{F,FV,I,IV} are glFog* parameter setters, not the
    // per-vertex glFogCoord* setters, so they are deliberately not included.)

    // --- glEdgeFlag{,v} -------------------------------------------------------
    // glEdgeFlag(GLboolean): one int GPR. glEdgeFlagv: pointer to one GLboolean.
    { GL_SUB_EDGE_FLAG, 1, 0, 0 },
    { GL_SUB_EDGE_FLAGV, 0, 0, 1 * BV_b },

    // --- glIndex{i,f,d,s,ub} + v variants -------------------------------------
    { GL_SUB_INDEXI, 1, 0, 0 },
    { GL_SUB_INDEXF, 0, FPR1, 0 },
    { GL_SUB_INDEXD, 0, FPR1, 0 },
    { GL_SUB_INDEXS, 1, 0, 0 },
    { GL_SUB_INDEXUB, 1, 0, 0 },
    { GL_SUB_INDEXIV, 0, 0, 1 * BV_i },
    { GL_SUB_INDEXFV, 0, 0, 1 * BV_f },
    { GL_SUB_INDEXDV, 0, 0, 1 * BV_d },
    { GL_SUB_INDEXSV, 0, 0, 1 * BV_s },
    { GL_SUB_INDEXUBV, 0, 0, 1 * BV_ub },
};

// popcount over a 16-bit mask (table is tiny; keep it dependency-free).
int popcount16(uint16_t v) {
    int n = 0;
    while (v) { n += (v & 1u); v >>= 1; }
    return n;
}

int round_up_4(int n) { return (n + 3) & ~3; }

// Worst-case wire size of one record for a given descriptor:
//   8 (header) + 4*scalar + 4*popcount(fpr_mask) + round_up_4(ptr_bytes).
int record_size(const GLDeferDesc &d) {
    return 8
         + 4 * (int)d.scalar_gpr_count
         + 4 * popcount16(d.fpr_mask)
         + round_up_4((int)d.ptr_bytes);
}

}  // namespace

void GLDeferBuildDescriptors(void) {
    std::memset(gl_defer_desc, 0, sizeof(gl_defer_desc));

    for (const GLDeferRow &row : kRows) {
        assert(row.op < GL_MAX_SUBOPCODE && "descriptor opcode out of range");
        GLDeferDesc &d = gl_defer_desc[row.op];
        // No opcode should be authored twice.
        assert(d.deferrable == 0 && "duplicate descriptor opcode");

        d.scalar_gpr_count = row.scalar_gpr_count;
        d.fpr_mask         = row.fpr_mask;
        d.ptr_bytes        = row.ptr_bytes;
        d.deferrable       = 1;

        // An opcode is EITHER scalar/FP OR trailing-pointer, never both.
        assert(!(d.ptr_bytes > 0 && d.fpr_mask != 0) &&
               "descriptor cannot mix fpr args and a trailing pointer");

        // Every record must fit the ring's worst-case record budget.
        assert(record_size(d) <= GL_DEFER_MAX_RECORD &&
               "descriptor record exceeds GL_DEFER_MAX_RECORD");
    }
}

bool GLDeferIsDeferrable(uint32_t op) {
    return op < GL_MAX_SUBOPCODE && gl_defer_desc[op].deferrable;
}

// Decode one captured record at guest address `rec` into `out`, reconstructing
// the exact argument tuple GLDispatch would replay. Returns the record's total
// size in bytes (read from the header), so the caller can advance to the next
// record with `rec += return value`.
//
// Wire layout (big-endian guest words, read via ReadMacInt32):
//   rec+0 : sub_opcode
//   rec+4 : size_bytes
//   rec+8 : scalar_gpr_count u32 scalars, then popcount(fpr_mask) u32 float bits,
//           then (if ptr_bytes>0) round_up_4(ptr_bytes) snapshot bytes.
//
// out->gpr[]:        scalars verbatim; if the opcode has a trailing pointer, the
//                    next slot holds the IN-RING guest address of the snapshot
//                    (so the GL handler dereferences the captured copy, never the
//                    guest's original buffer, which may already be reused).
// out->float_bits[]: the dense float words in ascending fpr_mask bit order.
// Unused gpr[]/float_bits[] slots are zeroed.
uint32_t GLDeferDecodeRecord(uint32_t rec, GLDeferDecoded *out) {
    std::memset(out, 0, sizeof(*out));

    const uint32_t sub_opcode = ReadMacInt32(rec + 0);
    const uint32_t size_bytes = ReadMacInt32(rec + 4);

    out->sub_opcode = sub_opcode;

    assert(sub_opcode < GL_MAX_SUBOPCODE && "decoded sub_opcode out of range");
    const GLDeferDesc &d = gl_defer_desc[sub_opcode];

    const int scalar_count = (int)d.scalar_gpr_count;
    const int fpr_count    = popcount16(d.fpr_mask);

    // Scalars: r3, r4, ... captured verbatim, immediately after the 8-byte header.
    const uint32_t scalars_off = rec + 8;
    for (int k = 0; k < scalar_count; ++k)
        out->gpr[k] = ReadMacInt32(scalars_off + 4u * (uint32_t)k);

    // FPRs: densely packed 32-bit float words after the scalars.
    const uint32_t fprs_off = scalars_off + 4u * (uint32_t)scalar_count;
    for (int n = 0; n < fpr_count; ++n)
        out->float_bits[n] = ReadMacInt32(fprs_off + 4u * (uint32_t)n);
    out->nfloat = fpr_count;

    // Trailing pointer: the snapshot lives in the ring right after the FPR words.
    // Expose its in-ring guest address as the pointer argument.
    if (d.ptr_bytes > 0) {
        // The `+ 4*fpr_count` term is always 0 in practice: GLDeferBuildDescriptors
        // asserts no opcode mixes fpr_mask with ptr_bytes (a ...v array variant is
        // pointer-only). Kept here for defensiveness so the offset is correct even
        // if that invariant ever loosens.
        const uint32_t ptr_off = fprs_off + 4u * (uint32_t)fpr_count;
        out->gpr[scalar_count] = ptr_off;
        out->ngpr = scalar_count + 1;
    } else {
        out->ngpr = scalar_count;
    }

    // Header size must agree with the layout we just walked.
    assert(size_bytes == (uint32_t)(8
                + 4 * scalar_count
                + 4 * fpr_count
                + round_up_4((int)d.ptr_bytes)) &&
           "record size header disagrees with descriptor layout");

    return size_bytes;
}

// Replay every buffered record through the existing GLDispatch path, then clear
// the ring. Stays a no-op until the ring is allocated and records are actually
// captured (gl_defer_count_addr == 0 here).
void GLDrainDeferred(void) {
    if (gl_defer_draining) return;          // re-entrancy guard
    if (!gl_defer_count_addr) return;       // ring not allocated yet -> safe no-op

    uint32_t count = ReadMacInt32(gl_defer_count_addr);
    if (!count) return;

    g_defer_drain_calls++;
    gl_defer_draining = true;

    uint32_t total = ReadMacInt32(gl_defer_head_addr);   // bytes written
    WriteMacInt32(gl_defer_count_addr, 0);               // reset BEFORE replay so replayed calls see an empty ring
    WriteMacInt32(gl_defer_head_addr, 0);

    // One-time error log cap: avoids flooding the log on persistent ring corruption.
    static int s_failsafe_log_count = 0;

    uint32_t off = 0;
    for (uint32_t i = 0; i < count && off < total; i++) {
        // ---- Lightweight structural failsafe (always compiled; cheap integer checks) ----
        // 1. Peek the sub_opcode from the record header before decoding.
        uint32_t rec_base = gl_defer_ring_base + off;
        uint32_t sub_op   = ReadMacInt32(rec_base + 0);
        uint32_t hdr_size = ReadMacInt32(rec_base + 4);

        // 2. Validate: opcode must be in range and deferrable.
        bool opcode_ok = (sub_op < GL_MAX_SUBOPCODE) && gl_defer_desc[sub_op].deferrable;

        // 3. Validate: hdr_size must be nonzero and equal to the descriptor-derived size.
        uint32_t desc_size = 0;
        if (opcode_ok) {
            const GLDeferDesc &desc = gl_defer_desc[sub_op];
            desc_size = (uint32_t)(8
                + 4 * (int)desc.scalar_gpr_count
                + 4 * popcount16(desc.fpr_mask)
                + round_up_4((int)desc.ptr_bytes));
        }
        bool size_ok = (hdr_size != 0) && opcode_ok && (hdr_size == desc_size);

        // 4. Validate: record must not walk past the written region.
        bool bounds_ok = (off + hdr_size <= total) && (hdr_size != 0);

        if (!opcode_ok || !size_ok || !bounds_ok) {
            // Ring corruption or a stub bug detected -- fail safe.
            if (s_failsafe_log_count < 16) {
                s_failsafe_log_count++;
                GL_LOG("GLDrainDeferred failsafe: ring corruption at record %u "
                       "(off=%u hdr_size=%u desc_size=%u total=%u sub_op=%u "
                       "opcode_ok=%d size_ok=%d bounds_ok=%d) "
                       "-- disabling deferral, dropping tail, degrading to direct-trap",
                       i, off, hdr_size, desc_size, total, sub_op,
                       (int)opcode_ok, (int)size_ok, (int)bounds_ok);
            }
            // Disable deferral so subsequent guest calls trap directly (proven path),
            // and latch it so glEndList cannot re-enable deferral for this session.
            gl_defer_failsafe_tripped = true;
            WriteMacInt32(gl_defer_enabled_addr, 0);
            // Ring is already zeroed (count+head reset above); clear the guard and bail.
            gl_defer_draining = false;
            return;
        }

        // ---- Record is structurally sound; decode and replay. ----
        GLDeferDecoded d;
        uint32_t sz = GLDeferDecodeRecord(rec_base, &d);
        if (sz == 0) break;                              // safety: malformed record (belt+suspenders)

        WriteMacInt32(gl_scratch_addr, d.sub_opcode);
        gl_ppc_sp = 0;
        gl_ppc_stack_arg_offset = 0;
        GLDispatch(d.gpr[0], d.gpr[1], d.gpr[2], d.gpr[3],
                   d.gpr[4], d.gpr[5], d.gpr[6], d.gpr[7],
                   d.float_bits, d.nfloat);
        g_defer_records_drained++;
        off += sz;
    }

    gl_defer_draining = false;
}

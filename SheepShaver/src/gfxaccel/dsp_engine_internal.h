/*
 *  dsp_engine_internal.h - Private-typed DSp <-> DMC helpers shared across
 *                          dsp_engine.cpp + dsp_draw_context.mm only.
 *                          Not exported via include/.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Provides the typed DSpMapStateToDMCOwnerTyped() wrapper below.
 *  Rationale: the public DSp header include/dsp_draw_context.h
 *  MUST NOT transitively pull in display_mode_controller.h, so the public
 *  helper DSpMapStateToDMCOwner() returns uint32_t. gfxaccel-internal
 *  consumers (dsp_draw_context.mm SetState handler; the bg/fg hooks)
 *  include THIS private header and call the typed wrapper — which is a
 *  trivial cast — so they get the proper DMCOwner enum type without
 *  leaking DMC into every DSp public consumer.
 */

#ifndef DSP_ENGINE_INTERNAL_H
#define DSP_ENGINE_INTERNAL_H

#include <stdint.h>
#include "display_mode_controller.h"    /* DMCOwner - internal use only */

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Typed wrapper over DSpMapStateToDMCOwner() — returns the proper DMCOwner
 *  enum for call sites inside the gfxaccel tree. Implementation is a trivial
 *  static-inline cast of the uint32_t-typed public function declared in
 *  include/dsp_draw_context.h.
 */
static inline DMCOwner DSpMapStateToDMCOwnerTyped(uint32_t dsp_state)
{
	extern uint32_t DSpMapStateToDMCOwner(uint32_t);   /* fwd decl */
	return (DMCOwner)DSpMapStateToDMCOwner(dsp_state);
}

#ifdef __cplusplus
}
#endif

#endif /* DSP_ENGINE_INTERNAL_H */

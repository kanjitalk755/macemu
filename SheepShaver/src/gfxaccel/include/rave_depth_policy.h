/*
 *  rave_depth_policy.h - RAVE depth-buffer texture policy helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef RAVE_DEPTH_POLICY_H
#define RAVE_DEPTH_POLICY_H

#include <stdint.h>

#include "rave_texture_alpha_policy.h"

#define RAVE_CONTEXT_NO_Z_BUFFER (1u << 0)

static inline int RaveContextUsesMetalDepthAttachment(uint32_t context_flags)
{
	return (context_flags & RAVE_CONTEXT_NO_Z_BUFFER) == 0;
}

static inline int RaveDepthWriteTagsEnableWrites(uint32_t standard_z_buffer_mask,
                                                 uint32_t ati_depth_write_enable)
{
	return standard_z_buffer_mask != 0 && ati_depth_write_enable != 0;
}

static inline int RaveGLBlendFactorsAreOpaqueOverwrite(uint32_t gl_blend_src,
                                                       uint32_t gl_blend_dst)
{
	return gl_blend_src == RAVE_GL_ONE && gl_blend_dst == RAVE_GL_ZERO;
}

static inline int RaveDrawDepthWriteEnabledForBlendFactors(
	uint32_t standard_z_buffer_mask,
	uint32_t ati_depth_write_enable,
	int blend_mode,
	uint32_t gl_blend_src,
	uint32_t gl_blend_dst)
{
	if (!RaveDepthWriteTagsEnableWrites(standard_z_buffer_mask,
	                                    ati_depth_write_enable)) {
		return 0;
	}

	if (blend_mode == 2 &&
	    !RaveGLBlendFactorsAreOpaqueOverwrite(gl_blend_src, gl_blend_dst)) {
		return 0;
	}

	return 1;
}

#endif /* RAVE_DEPTH_POLICY_H */

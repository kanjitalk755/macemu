/*
 *  gl_synthetic_symbol_policy.h - synthetic OpenGLLibrary CFM symbol policy.
 *
 *  Policy: synthesize only advertised extension entry points whose backend
 *  dispatch handlers are complete enough to expose. Core OpenGL 1.2 symbols
 *  are intentionally not synthesized while GL_VERSION reports 1.1; callers
 *  should discover supported 1.2-era behavior through explicit extensions.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GL_SYNTHETIC_SYMBOL_POLICY_H
#define GL_SYNTHETIC_SYMBOL_POLICY_H

#include <stddef.h>
#include <stdint.h>

#include "gl_engine.h"

static inline bool GLPascalStringEqualsLiteral(const char *actual,
                                               const char *literal)
{
	if (actual == nullptr || literal == nullptr) return false;
	const uint8_t len = (uint8_t)literal[0];
	if ((uint8_t)actual[0] != len) return false;
	for (uint8_t i = 0; i <= len; i++) {
		if ((uint8_t)actual[i] != (uint8_t)literal[i]) return false;
	}
	return true;
}

static inline bool GLSyntheticFindLibSymbolSubOpcode(const char *lib,
                                                     const char *sym,
                                                     uint16_t *out_sub_opcode)
{
	if (out_sub_opcode != nullptr) *out_sub_opcode = 0;
	if (!GLPascalStringEqualsLiteral(lib, "\015OpenGLLibrary")) return false;

	struct GLSyntheticSymbolEntry {
		const char *pascal_sym;
		uint16_t sub_opcode;
	};
	static const GLSyntheticSymbolEntry symbols[] = {
		{ "\020aglNextPixelFormat", (uint16_t)GL_SUB_AGL_NEXTPIXELFORMAT },
		{ "\017glBlendColorEXT", (uint16_t)GL_SUB_BLEND_COLOR_EXT },
		{ "\022glBlendEquationEXT", (uint16_t)GL_SUB_BLEND_EQUATION_EXT },
		{ "\017glLockArraysEXT", (uint16_t)GL_SUB_LOCK_ARRAYS_EXT },
		{ "\021glUnlockArraysEXT", (uint16_t)GL_SUB_UNLOCK_ARRAYS_EXT },
		{ "\030glClientActiveTextureARB", (uint16_t)GL_SUB_CLIENT_ACTIVE_TEXTURE_ARB },
		{ "\022glActiveTextureARB", (uint16_t)GL_SUB_ACTIVE_TEXTURE_ARB },
		{ "\024glMultiTexCoord1dARB", (uint16_t)GL_SUB_MULTI_TEX_COORD1D_ARB },
		{ "\025glMultiTexCoord1dvARB", (uint16_t)GL_SUB_MULTI_TEX_COORD1DV_ARB },
		{ "\024glMultiTexCoord1fARB", (uint16_t)GL_SUB_MULTI_TEX_COORD1F_ARB },
		{ "\025glMultiTexCoord1fvARB", (uint16_t)GL_SUB_MULTI_TEX_COORD1FV_ARB },
		{ "\024glMultiTexCoord1iARB", (uint16_t)GL_SUB_MULTI_TEX_COORD1I_ARB },
		{ "\025glMultiTexCoord1ivARB", (uint16_t)GL_SUB_MULTI_TEX_COORD1IV_ARB },
		{ "\024glMultiTexCoord1sARB", (uint16_t)GL_SUB_MULTI_TEX_COORD1S_ARB },
		{ "\025glMultiTexCoord1svARB", (uint16_t)GL_SUB_MULTI_TEX_COORD1SV_ARB },
		{ "\024glMultiTexCoord2dARB", (uint16_t)GL_SUB_MULTI_TEX_COORD2D_ARB },
		{ "\025glMultiTexCoord2dvARB", (uint16_t)GL_SUB_MULTI_TEX_COORD2DV_ARB },
		{ "\024glMultiTexCoord2fARB", (uint16_t)GL_SUB_MULTI_TEX_COORD2F_ARB },
		{ "\025glMultiTexCoord2fvARB", (uint16_t)GL_SUB_MULTI_TEX_COORD2FV_ARB },
		{ "\024glMultiTexCoord2iARB", (uint16_t)GL_SUB_MULTI_TEX_COORD2I_ARB },
		{ "\025glMultiTexCoord2ivARB", (uint16_t)GL_SUB_MULTI_TEX_COORD2IV_ARB },
		{ "\024glMultiTexCoord2sARB", (uint16_t)GL_SUB_MULTI_TEX_COORD2S_ARB },
		{ "\025glMultiTexCoord2svARB", (uint16_t)GL_SUB_MULTI_TEX_COORD2SV_ARB },
		{ "\024glMultiTexCoord3dARB", (uint16_t)GL_SUB_MULTI_TEX_COORD3D_ARB },
		{ "\025glMultiTexCoord3dvARB", (uint16_t)GL_SUB_MULTI_TEX_COORD3DV_ARB },
		{ "\024glMultiTexCoord3fARB", (uint16_t)GL_SUB_MULTI_TEX_COORD3F_ARB },
		{ "\025glMultiTexCoord3fvARB", (uint16_t)GL_SUB_MULTI_TEX_COORD3FV_ARB },
		{ "\024glMultiTexCoord3iARB", (uint16_t)GL_SUB_MULTI_TEX_COORD3I_ARB },
		{ "\025glMultiTexCoord3ivARB", (uint16_t)GL_SUB_MULTI_TEX_COORD3IV_ARB },
		{ "\024glMultiTexCoord3sARB", (uint16_t)GL_SUB_MULTI_TEX_COORD3S_ARB },
		{ "\025glMultiTexCoord3svARB", (uint16_t)GL_SUB_MULTI_TEX_COORD3SV_ARB },
		{ "\024glMultiTexCoord4dARB", (uint16_t)GL_SUB_MULTI_TEX_COORD4D_ARB },
		{ "\025glMultiTexCoord4dvARB", (uint16_t)GL_SUB_MULTI_TEX_COORD4DV_ARB },
		{ "\024glMultiTexCoord4fARB", (uint16_t)GL_SUB_MULTI_TEX_COORD4F_ARB },
		{ "\025glMultiTexCoord4fvARB", (uint16_t)GL_SUB_MULTI_TEX_COORD4FV_ARB },
		{ "\024glMultiTexCoord4iARB", (uint16_t)GL_SUB_MULTI_TEX_COORD4I_ARB },
		{ "\025glMultiTexCoord4ivARB", (uint16_t)GL_SUB_MULTI_TEX_COORD4IV_ARB },
		{ "\024glMultiTexCoord4sARB", (uint16_t)GL_SUB_MULTI_TEX_COORD4S_ARB },
		{ "\025glMultiTexCoord4svARB", (uint16_t)GL_SUB_MULTI_TEX_COORD4SV_ARB },
		{ "\031glLoadTransposeMatrixdARB", (uint16_t)GL_SUB_LOAD_TRANSPOSE_MATRIXD_ARB },
		{ "\031glLoadTransposeMatrixfARB", (uint16_t)GL_SUB_LOAD_TRANSPOSE_MATRIXF_ARB },
		{ "\031glMultTransposeMatrixdARB", (uint16_t)GL_SUB_MULT_TRANSPOSE_MATRIXD_ARB },
		{ "\031glMultTransposeMatrixfARB", (uint16_t)GL_SUB_MULT_TRANSPOSE_MATRIXF_ARB },
		{ "\025glSecondaryColor3bEXT", (uint16_t)GL_SUB_SECONDARYCOLOR3B_EXT },
		{ "\026glSecondaryColor3bvEXT", (uint16_t)GL_SUB_SECONDARYCOLOR3BV_EXT },
		{ "\025glSecondaryColor3dEXT", (uint16_t)GL_SUB_SECONDARYCOLOR3D_EXT },
		{ "\026glSecondaryColor3dvEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3DV_EXT },
		{ "\025glSecondaryColor3fEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3F_EXT },
		{ "\026glSecondaryColor3fvEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3FV_EXT },
		{ "\025glSecondaryColor3iEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3I_EXT },
		{ "\026glSecondaryColor3ivEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3IV_EXT },
		{ "\025glSecondaryColor3sEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3S_EXT },
		{ "\026glSecondaryColor3svEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3SV_EXT },
		{ "\026glSecondaryColor3ubEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3UB_EXT },
		{ "\027glSecondaryColor3ubvEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3UBV_EXT },
		{ "\026glSecondaryColor3uiEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3UI_EXT },
		{ "\027glSecondaryColor3uivEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3UIV_EXT },
		{ "\026glSecondaryColor3usEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3US_EXT },
		{ "\027glSecondaryColor3usvEXT", (uint16_t)GL_SUB_SECONDARY_COLOR3USV_EXT },
		{ "\032glSecondaryColorPointerEXT", (uint16_t)GL_SUB_SECONDARY_COLOR_POINTER_EXT },
	};

	for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
		if (!GLPascalStringEqualsLiteral(sym, symbols[i].pascal_sym))
			continue;
		if (out_sub_opcode != nullptr)
			*out_sub_opcode = symbols[i].sub_opcode;
		return true;
	}

	return false;
}

#endif /* GL_SYNTHETIC_SYMBOL_POLICY_H */

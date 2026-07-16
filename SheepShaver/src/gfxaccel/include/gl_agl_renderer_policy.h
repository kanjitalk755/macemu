/*
 *  gl_agl_renderer_policy.h - AGL renderer identity advertised to guests.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GL_AGL_RENDERER_POLICY_H
#define GL_AGL_RENDERER_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Classic AGL uses the same renderer-id family as CGL. 0x00020400 is the
 * generic float/software renderer; Unreal Tournament rejects that as a
 * hardware renderer. Advertise ATI Rage 128, matching the GL_RENDERER string
 * and the RAVE ATI vendor posture.
 */
static inline uint32_t GLAGLRendererID(void)
{
	return 0x00021000u; /* kCGLRendererATIRage128ID */
}

static inline uint32_t GLAGLRendererVideoMemoryBytes(void)
{
	return 16u * 1024u * 1024u;
}

static inline uint32_t GLAGLRendererTextureMemoryBytes(void)
{
	return 16u * 1024u * 1024u;
}

static inline bool GLAGLRendererIsAccelerated(void)
{
	return true;
}

#endif /* GL_AGL_RENDERER_POLICY_H */

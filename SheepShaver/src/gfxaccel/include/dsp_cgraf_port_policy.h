/*
 *  dsp_cgraf_port_policy.h - classic QuickDraw CGrafPort layout helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  DrawSprocket back/alt-buffer shims are PixMap-shaped for direct byte
 *  access, but the front buffer is the screen display and can be consumed as
 *  a real CGrafPort by QuickDraw/RAVE setup code.
 */
#ifndef DSP_CGRAF_PORT_POLICY_H
#define DSP_CGRAF_PORT_POLICY_H

#include <stdint.h>

#define DSP_CGRAFPORT_OFF_PORT_PIXMAP   2u
#define DSP_CGRAFPORT_OFF_PORT_VERSION  6u
#define DSP_CGRAFPORT_OFF_GRAF_VARS     8u
#define DSP_CGRAFPORT_OFF_PORT_RECT    16u
#define DSP_CGRAFPORT_OFF_VIS_RGN      24u
#define DSP_CGRAFPORT_OFF_CLIP_RGN     28u
#define DSP_CGRAFPORT_OFF_RGB_FG_COLOR 36u
#define DSP_CGRAFPORT_OFF_RGB_BK_COLOR 42u
#define DSP_CGRAFPORT_OFF_PN_LOC       48u
#define DSP_CGRAFPORT_OFF_PN_SIZE      52u
#define DSP_CGRAFPORT_OFF_PN_MODE      56u
#define DSP_CGRAFPORT_OFF_TX_FONT      68u
#define DSP_CGRAFPORT_OFF_TX_MODE      72u
#define DSP_CGRAFPORT_OFF_TX_SIZE      74u
#define DSP_CGRAFPORT_OFF_FG_COLOR     80u
#define DSP_CGRAFPORT_OFF_BK_COLOR     84u
#define DSP_CGRAFPORT_OFF_GRAF_PROCS  104u
#define DSP_CGRAFPORT_MIN_SIZE        108u

#define DSP_REGION_OFF_SIZE             0u
#define DSP_REGION_OFF_BBOX             2u
#define DSP_RECT_REGION_SIZE           10u

static inline bool DSpLooksLikeColorCGrafPort(uint16_t port_version)
{
	return (port_version & 0xC000u) != 0;
}

static inline uint32_t DSpPixMapExtraFieldOffsetForPortVersion(
	uint16_t port_version,
	uint32_t compact_offset,
	uint32_t real_quickdraw_offset)
{
	return DSpLooksLikeColorCGrafPort(port_version)
	           ? real_quickdraw_offset
	           : compact_offset;
}

#endif /* DSP_CGRAF_PORT_POLICY_H */

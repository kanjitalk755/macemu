/*
 *  dsp_back_buffer_cgraf_policy.h - DrawSprocket back-buffer CGrafPtr layout.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_BACK_BUFFER_CGRAF_POLICY_H
#define DSP_BACK_BUFFER_CGRAF_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "dsp_cgraf_port_policy.h"
#include "dsp_pixmap_offsets.h"

static inline uint32_t DSpBackBufferPixMapRecordSize(void)
{
	return (uint32_t)DSP_MAINDEVICE_PIXMAP_SIZE;
}

static inline uint32_t DSpBackBufferPixMapHandleSize(void)
{
	return 4u;
}

static inline uint32_t DSpBackBufferCGrafPortSize(void)
{
	return (uint32_t)DSP_CGRAFPORT_MIN_SIZE;
}

static inline uint16_t DSpBackBufferPixMapRowBytesField(uint32_t row_bytes)
{
	return (uint16_t)(0x8000u | (row_bytes & 0x3FFFu));
}

#endif /* DSP_BACK_BUFFER_CGRAF_POLICY_H */

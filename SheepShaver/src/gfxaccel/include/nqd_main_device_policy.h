/*
 *  nqd_main_device_policy.h - NQD policy for DSp-redirected MainDevice writes.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef NQD_MAIN_DEVICE_POLICY_H
#define NQD_MAIN_DEVICE_POLICY_H

#include <stdint.h>

typedef struct NQDMainDevicePixMapSnapshot {
	bool     valid;
	uint32_t baseAddr;
	uint32_t rowBytes;
	uint16_t pixelSize;
} NQDMainDevicePixMapSnapshot;

static inline bool NQDIsSupportedPixMapDepth(uint32_t pixel_size)
{
	return pixel_size == 1 || pixel_size == 2 || pixel_size == 4 ||
	       pixel_size == 8 || pixel_size == 16 || pixel_size == 32;
}

static inline bool NQDIsPackedMainDeviceAlias(
    NQDMainDevicePixMapSnapshot snap,
    uint32_t dest_base,
    int32_t dest_row_bytes,
    uint32_t packet_pixel_size)
{
	if (!snap.valid || dest_base != snap.baseAddr) return false;
	if (dest_row_bytes <= 0) return false;
	if (dest_row_bytes == (int32_t)snap.rowBytes) return false;
	if (!NQDIsSupportedPixMapDepth(packet_pixel_size)) return false;
	if (!NQDIsSupportedPixMapDepth(snap.pixelSize)) return false;
	if (packet_pixel_size >= 8) return false;
	return snap.pixelSize == 16 || snap.pixelSize == 32;
}

static inline bool NQDShouldDropStaleMainDeviceParams(
    NQDMainDevicePixMapSnapshot snap,
    uint32_t dest_base,
    int32_t dest_row_bytes,
    uint32_t pixel_size)
{
	if (!snap.valid || dest_base != snap.baseAddr) return false;
	if (!NQDIsSupportedPixMapDepth(snap.pixelSize)) return false;
	if (NQDIsPackedMainDeviceAlias(snap, dest_base, dest_row_bytes,
	                               pixel_size)) {
		return false;
	}
	return dest_row_bytes != (int32_t)snap.rowBytes;
}

static inline uint32_t NQDEffectiveMainDevicePixelSize(
    NQDMainDevicePixMapSnapshot snap,
    uint32_t dest_base,
    int32_t dest_row_bytes,
    uint32_t packet_pixel_size)
{
	if (!snap.valid || dest_base != snap.baseAddr) return packet_pixel_size;
	if (!NQDIsSupportedPixMapDepth(snap.pixelSize)) return packet_pixel_size;
	if (dest_row_bytes != (int32_t)snap.rowBytes &&
	    !NQDIsPackedMainDeviceAlias(snap, dest_base, dest_row_bytes,
	                                packet_pixel_size)) {
		return packet_pixel_size;
	}
	return snap.pixelSize;
}

static inline bool NQDShouldUseCPUPackedMainDevicePath(
    NQDMainDevicePixMapSnapshot snap,
    uint32_t dest_base,
    int32_t dest_row_bytes,
    uint32_t pixel_size)
{
	if (!snap.valid || dest_base != snap.baseAddr) return false;
	if (pixel_size >= 8) return false;
	if (!NQDIsSupportedPixMapDepth(snap.pixelSize)) return false;
	return pixel_size == snap.pixelSize && dest_row_bytes == (int32_t)snap.rowBytes;
}

static inline bool NQDShouldUseCPUMixedDepthMainDeviceBitBlt(
    NQDMainDevicePixMapSnapshot snap,
    uint32_t dest_base,
    int32_t dest_row_bytes,
    uint32_t src_pixel_size,
    uint32_t dest_pixel_size,
    uint32_t transfer_mode)
{
	if (!snap.valid || dest_base != snap.baseAddr) return false;
	if (dest_row_bytes != (int32_t)snap.rowBytes) return false;
	if (!NQDIsSupportedPixMapDepth(snap.pixelSize)) return false;
	if (dest_pixel_size != snap.pixelSize) return false;
	if (src_pixel_size >= 8) return false;
	if (src_pixel_size == dest_pixel_size) return false;
	if (dest_pixel_size != 16 && dest_pixel_size != 32) return false;
	return transfer_mode <= 7;
}

#endif /* NQD_MAIN_DEVICE_POLICY_H */

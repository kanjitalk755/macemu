/*
 *  rave_device_summary.h - Small helpers for decoding RAVE TQADevice headers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  The full TQADevice payload lives in guest memory, but the device type and
 *  Mac memory-device fields are stable RAVE 1.6 ABI values. Keeping these in
 *  one tested place avoids scattering raw offsets through diagnostics.
 */

#ifndef RAVE_DEVICE_SUMMARY_H
#define RAVE_DEVICE_SUMMARY_H

#include <stdint.h>

enum {
	kRaveDeviceTypeMemory  = 0,
	kRaveDeviceTypeGDevice = 1,
	kRaveDeviceTypeWin32DC = 2,
	kRaveDeviceTypeDDSurface = 3
};

enum {
	kRaveDeviceOff_Type = 0,
	kRaveDeviceOff_MemoryRowBytes = 4,
	kRaveDeviceOff_MemoryPixelType = 8,
	kRaveDeviceOff_MemoryWidth = 12,
	kRaveDeviceOff_MemoryHeight = 16,
	kRaveDeviceOff_MemoryBaseAddr = 20,
	kRaveDeviceOff_GDeviceHandle = 4
};

static inline const char *RaveDeviceTypeName(uint32_t device_type)
{
	switch (device_type) {
		case kRaveDeviceTypeMemory:    return "kQADeviceMemory";
		case kRaveDeviceTypeGDevice:   return "kQADeviceGDevice";
		case kRaveDeviceTypeWin32DC:   return "kQADeviceWin32DC";
		case kRaveDeviceTypeDDSurface: return "kQADeviceDDSurface";
		default:                       return "unknown";
	}
}

#endif /* RAVE_DEVICE_SUMMARY_H */

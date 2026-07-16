/*
 *  dsp_guest_address.h - DSp guest-address validation helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  DSp can receive a nonzero Host2MacAddr() result for host-owned Metal
 *  buffers. That value is only safe to publish into guest PixMaps when the
 *  whole byte range lies inside the guest RAM aperture.
 */

#ifndef DSP_GUEST_ADDRESS_H
#define DSP_GUEST_ADDRESS_H

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

static inline bool DSpGuestRAMContains(uint32_t mac_addr,
                                       uint32_t byte_count,
                                       uint32_t ram_base,
                                       uint32_t ram_size)
{
	if (mac_addr == 0 || byte_count == 0 || ram_size == 0) return false;
	if (mac_addr < ram_base) return false;

	uint64_t start = (uint64_t)mac_addr;
	uint64_t end = start + (uint64_t)byte_count;
	uint64_t ram_start = (uint64_t)ram_base;
	uint64_t ram_end = ram_start + (uint64_t)ram_size;

	return end >= start && start >= ram_start && end <= ram_end;
}

static inline uint32_t DSpUsableGuestBaseOrZero(uint32_t mapped_addr,
                                                uint32_t byte_count,
                                                uint32_t ram_base,
                                                uint32_t ram_size)
{
	return DSpGuestRAMContains(mapped_addr, byte_count, ram_base, ram_size)
	    ? mapped_addr
	    : 0;
}

static inline uint32_t DSpUsableDirectGuestBaseOrZero(
    uint32_t mapped_addr,
    uint32_t byte_count,
    uint32_t ram_base,
    uint32_t ram_size,
    uintptr_t resolved_host_addr,
    uintptr_t expected_host_addr)
{
	if (!DSpGuestRAMContains(mapped_addr, byte_count, ram_base, ram_size)) {
		return 0;
	}
	if (resolved_host_addr == 0 || expected_host_addr == 0) return 0;
	return resolved_host_addr == expected_host_addr ? mapped_addr : 0;
}

#endif /* DSP_GUEST_ADDRESS_H */

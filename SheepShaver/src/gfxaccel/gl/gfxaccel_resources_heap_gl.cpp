/*
 *  gfxaccel_resources_heap_gl.cpp - OpenGL/host-memory heap (mm_* side)
 * 
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"
#include "vec_data.h"
#include "gfxaccel_resources_heap.h"

#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdio>

struct Alloc { void *ptr; uint32_t len; };
static std::vector<Alloc> s_allocs[kHeapCount];
static uint32_t s_live[kHeapCount] = {};
static uint64_t s_off[kHeapCount] = {};
static std::vector<uint8_t> s_rave_ring;
static bool s_ring = false;

extern "C" void gfxaccel_resources_heap_mm_init(void)
{
	for (int i = 0; i < kHeapCount; i++) {
		s_allocs[i].clear();
		s_live[i] = 0;
		s_off[i] = 0;
	}
}
extern "C" void gfxaccel_resources_heap_mm_shutdown(void)
{
	for (int i = 0; i < kHeapCount; i++) {
		for (size_t a = 0; a < s_allocs[i].size(); a++) std::free(s_allocs[i][a].ptr);
		s_allocs[i].clear();
		s_live[i] = 0;
		s_off[i] = 0;
	}
	s_rave_ring.clear();
	s_ring = false;
}
extern "C" void *gfxaccel_resources_heap_mm_get(uint32_t heap_id)
{
	if (heap_id >= kHeapCount) return NULL;
	return (void *)(uintptr_t)(heap_id + 1);
}
extern "C" void *gfxaccel_resources_heap_mm_alloc_buffer(uint32_t heap_id, uint32_t length, uint32_t)
{
	if (heap_id >= kHeapCount || !length) return NULL;
	void *p = std::calloc(1, length);
	if (!p) return NULL;
	s_allocs[heap_id].push_back({p, length});
	s_live[heap_id]++;
	s_off[heap_id] += length;
	return p;
}
extern "C" void gfxaccel_resources_heap_mm_lru_purge(void) {}
extern "C" uint64_t gfxaccel_resources_heap_mm_reset(uint32_t heap_id)
{
	if (heap_id >= kHeapCount || s_live[heap_id]) return 0;
	uint64_t prev = s_off[heap_id];
	for (size_t a = 0; a < s_allocs[heap_id].size(); a++)
		std::free(s_allocs[heap_id][a].ptr);
	s_allocs[heap_id].clear();
	s_off[heap_id] = 0;
	return prev;
}
extern "C" void gfxaccel_resources_heap_mm_note_allocation_released(uint32_t heap_id)
{
	if (heap_id < kHeapCount && s_live[heap_id]) s_live[heap_id]--;
}

/* Free a host buffer previously returned by heap_alloc_buffer. */
extern "C" void gfxaccel_resources_heap_mm_free_buffer(uint32_t heap_id, void *ptr)
{
	if (heap_id >= kHeapCount || !ptr) return;
	std::vector<Alloc> &vec = s_allocs[heap_id];
	for (size_t i = 0; i < vec.size(); i++) {
		if (vec[i].ptr == ptr) {
			std::free(vec[i].ptr);
			vec.erase(vec.begin() + (std::ptrdiff_t)i);
			if (s_live[heap_id]) s_live[heap_id]--;
			return;
		}
	}
}
extern "C" uint32_t gfxaccel_resources_heap_mm_live_allocation_count(uint32_t heap_id)
{
	return heap_id < kHeapCount ? s_live[heap_id] : 0;
}

void gfxaccel_resources_heap_note_gpu_commit(uint32_t, void *) {}
uint64_t gfxaccel_resources_heap_reset_gpu_idle(uint32_t heap_id)
{
	return gfxaccel_resources_heap_reset(heap_id);
}
void gfxaccel_handle_memory_warning(void) {}
int32_t gfxaccel_heap_wait_for_eviction(uint64_t) { return kGfxAccelResNoErr; }

int32_t gfxaccel_rave_ring_init(void) { s_ring = true; return 0; }
void gfxaccel_rave_ring_shutdown(void) { s_rave_ring.clear(); s_ring = false; }
void *gfxaccel_rave_ring_stage(const void *data, uint32_t size, uint32_t *out_offset)
{
	if (!s_ring) gfxaccel_rave_ring_init();
	if (out_offset) *out_offset = 0;
	s_rave_ring.resize(size);
	if (data && size) std::memcpy(vec_data(s_rave_ring), data, size);
	return vec_data(s_rave_ring);
}
void gfxaccel_rave_ring_frame_end(void *) {}
int32_t gfxaccel_rave_ring_submission_near_exhaustion(void) { return 0; }

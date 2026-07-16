/*
 *  pso_archive_gl.cpp - PSO archive stub (OpenGL has no MTLBinaryArchive)
 */
#include "pso_archive.h"

int32_t pso_archive_init(void) { return kGfxAccelErrPSOArchiveNotAvailable; }
void pso_archive_shutdown(void) {}
void *pso_archive_lookup_render(void *) { return nullptr; }
void *pso_archive_lookup_compute(void *) { return nullptr; }
uint32_t pso_archive_is_available(void) { return 0; }
void pso_archive_set_on_descriptor(void *) {}

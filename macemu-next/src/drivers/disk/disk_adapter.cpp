/*
 *  disk_adapter.cpp - Disk adapter layer
 */

#include "sysdeps.h"
#include "platform.h"
#include "disk.h"

void DiskInit(void)
{
	g_platform.disk_init();
}

void DiskExit(void)
{
	g_platform.disk_exit();
}

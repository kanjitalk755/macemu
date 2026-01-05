/*
 *  video_adapter.cpp - Video adapter layer
 *
 *  Thin adapter that defers to g_platform function pointers.
 *  Replaces video_dummy.cpp - no changes to core code needed.
 */

#include "sysdeps.h"
#include "platform.h"
#include "video.h"

bool VideoInit(bool classic)
{
	return g_platform.video_init(classic);
}

void VideoExit(void)
{
	g_platform.video_exit();
}

void VideoRefresh(void)
{
	g_platform.video_refresh();
}

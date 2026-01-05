/*
 *  audio_adapter.cpp - Audio adapter layer
 */

#include "sysdeps.h"
#include "platform.h"
#include "audio.h"

void AudioInit(void)
{
	g_platform.audio_init();
}

void AudioExit(void)
{
	g_platform.audio_exit();
}

/*
 *  serial_adapter.cpp - Serial adapter layer
 */

#include "sysdeps.h"
#include "platform.h"
#include "serial.h"

void SerialInit(void)
{
	g_platform.serial_init();
}

void SerialExit(void)
{
	g_platform.serial_exit();
}

/*
 *  ether_adapter.cpp - Ethernet adapter layer
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "platform.h"
#include "ether.h"

bool ether_init(void)
{
	return g_platform.ether_init();
}

void ether_exit(void)
{
	g_platform.ether_exit();
}

void ether_reset(void)
{
	g_platform.ether_reset();
}

int16 ether_add_multicast(uint32 pb)
{
	return g_platform.ether_add_multicast(pb);
}

int16 ether_del_multicast(uint32 pb)
{
	return g_platform.ether_del_multicast(pb);
}

int16 ether_attach_ph(uint16 type, uint32 handler)
{
	return g_platform.ether_attach_ph(type, handler);
}

int16 ether_detach_ph(uint16 type)
{
	return g_platform.ether_detach_ph(type);
}

int16 ether_write(uint32 wds)
{
	return g_platform.ether_write(wds);
}

bool ether_start_udp_thread(int socket_fd)
{
	return g_platform.ether_start_udp_thread(socket_fd);
}

void ether_stop_udp_thread(void)
{
	g_platform.ether_stop_udp_thread();
}

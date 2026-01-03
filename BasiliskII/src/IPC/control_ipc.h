/*
 *  control_ipc.h - Control socket and input handling for IPC system
 *
 *  Handles:
 *  - Unix domain socket server (accept connections from WebRTC server)
 *  - Input processing (keyboard, mouse, commands, ping)
 *  - Audio frame requests
 *
 *  Separated from video_ipc.cpp for better modularity and to use epoll
 *  for zero-latency input handling instead of sleep-based polling.
 */

#ifndef CONTROL_IPC_H
#define CONTROL_IPC_H

#include <sys/types.h>
#include "ipc_protocol.h"

/*
 *  Initialize control socket system
 *  Creates Unix domain socket at /tmp/macemu-<pid>.sock
 *  Returns true on success
 */
bool ControlIPCInit(MacEmuIPCBuffer* shm);

/*
 *  Start control socket thread
 *  Thread handles connections and input processing
 */
void ControlIPCStart();

/*
 *  Stop and cleanup control socket
 *  Stops thread, closes connections, removes socket file
 */
void ControlIPCExit();

/*
 *  Get control socket fd (for sending eventfd to server)
 *  Returns -1 if not connected
 */
int ControlIPCGetSocket();

#endif // CONTROL_IPC_H

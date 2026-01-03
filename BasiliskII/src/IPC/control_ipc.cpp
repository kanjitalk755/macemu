/*
 *  control_ipc.cpp - Control socket and input handling for IPC system
 *
 *  Uses epoll for zero-latency input handling instead of sleep-based polling.
 *  Handles keyboard, mouse, commands, ping, and audio requests from server.
 */

#include "sysdeps.h"
#include "control_ipc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>

#include "cpu_emulation.h"
#include "main.h"
#include "adb.h"
#include "ipc_protocol.h"
#include "audio_ipc.h"

// Debug output
#define D(x) ;

// Global state
static int listen_socket = -1;             // Listening socket for server connections
static int control_socket = -1;            // Connected server socket
static std::string socket_path;            // Path to Unix domain socket
static MacEmuIPCBuffer* video_shm = nullptr;  // Shared memory buffer

static std::thread control_thread;
static std::atomic<bool> control_thread_running(false);

// Ping echo state (for latency measurement)
static uint32_t last_echoed_ping_seq = 0;
static int ping_echo_frames_remaining = 0;

/*
 *  Create Unix socket for input (emulator owns this)
 */

static bool create_control_socket() {
    pid_t pid = getpid();
    socket_path = std::string(MACEMU_CONTROL_SOCK_PREFIX) + std::to_string(pid) +
                  std::string(MACEMU_CONTROL_SOCK_SUFFIX);

    // Remove any stale socket
    unlink(socket_path.c_str());

    listen_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_socket < 0) {
        fprintf(stderr, "IPC: Failed to create socket: %s\n", strerror(errno));
        return false;
    }

    // Set non-blocking for accept()
    int flags = fcntl(listen_socket, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(listen_socket, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "IPC: Failed to bind socket to %s: %s\n", socket_path.c_str(), strerror(errno));
        close(listen_socket);
        listen_socket = -1;
        return false;
    }

    if (listen(listen_socket, 1) < 0) {
        fprintf(stderr, "IPC: Failed to listen on socket: %s\n", strerror(errno));
        close(listen_socket);
        listen_socket = -1;
        unlink(socket_path.c_str());
        return false;
    }

    fprintf(stderr, "IPC: Listening for server on '%s'\n", socket_path.c_str());
    return true;
}

static void destroy_control_socket() {
    if (control_socket >= 0) {
        close(control_socket);
        control_socket = -1;
    }
    if (listen_socket >= 0) {
        close(listen_socket);
        listen_socket = -1;
    }
    if (!socket_path.empty()) {
        unlink(socket_path.c_str());
    }
}

/*
 *  Echo ping back to server (called from video refresh thread)
 */

void macemu_echo_ping(MacEmuIPCBuffer* buf, uint32_t sequence,
                      uint64_t t1_browser_send_ms, uint64_t t2_server_forward_us,
                      uint64_t t3_emulator_recv_us) {
    if (!buf || sequence <= last_echoed_ping_seq) {
        return;  // Already echoed or invalid
    }

    // Echo ping multiple times (5 frames) to handle packet loss
    ping_echo_frames_remaining = 5;
    last_echoed_ping_seq = sequence;

    // Capture T4 timestamp: emulator finished processing, ready to echo
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t4_emulator_echo_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    // Write echo to SHM (server reads via acquire fence on sequence number)
    buf->ping_timestamps.t1_browser_ms = t1_browser_send_ms;
    buf->ping_timestamps.t2_server_us = t2_server_forward_us;
    buf->ping_timestamps.t3_emulator_us = t3_emulator_recv_us;
    buf->ping_timestamps.t4_frame_us = t4_emulator_echo_us;

    // Atomic store with release: guarantees all above writes visible before sequence update
    ATOMIC_STORE(buf->ping_sequence, sequence);

    D(bug("IPC: Echo ping #%u (t1=%llu t2=%llu t3=%llu t4=%llu)\n",
          sequence, t1_browser_send_ms, t2_server_forward_us,
          t3_emulator_recv_us, t4_emulator_echo_us));
}

void macemu_echo_ping_if_pending(MacEmuIPCBuffer* buf) {
    if (!buf || ping_echo_frames_remaining <= 0) {
        return;
    }

    // Re-echo same ping data for reliability (UDP-like delivery)
    uint32_t seq = buf->ping_sequence;
    if (seq > 0) {
        ATOMIC_STORE(buf->ping_sequence, seq);  // Trigger server read again
        D(bug("IPC: Re-echo ping #%u (frames_remaining=%d)\n", seq, ping_echo_frames_remaining));
    }

    ping_echo_frames_remaining--;
}

/*
 *  Process binary input from server
 */

static void process_binary_input(const uint8_t* data, size_t len) {
    if (len < sizeof(MacEmuInputHeader)) return;

    const MacEmuInputHeader* hdr = (const MacEmuInputHeader*)data;

    switch (hdr->type) {
        case MACEMU_INPUT_KEY: {
            if (len < sizeof(MacEmuKeyInput)) return;
            const MacEmuKeyInput* key = (const MacEmuKeyInput*)data;
            ADBKeyDown(key->mac_keycode);
            if (!(hdr->flags & MACEMU_KEY_DOWN)) {
                ADBKeyUp(key->mac_keycode);
            }
            break;
        }
        case MACEMU_INPUT_MOUSE: {
            if (len < sizeof(MacEmuMouseInput)) return;
            const MacEmuMouseInput* mouse = (const MacEmuMouseInput*)data;

            static bool debug_mouse = (getenv("MACEMU_DEBUG_MOUSE") != nullptr);
            static int mouse_event_count = 0;

            // Check if absolute or relative mode
            bool absolute = (mouse->hdr.flags & MACEMU_MOUSE_ABSOLUTE) != 0;

            // Call ADBMouseMoved() - behavior depends on absolute flag
            int x, y;
            if (absolute) {
                // Reinterpret int16_t as uint16_t for absolute coordinates
                x = static_cast<uint16_t>(mouse->x);
                y = static_cast<uint16_t>(mouse->y);
                ADBSetRelMouseMode(false);
            } else {
                x = mouse->x;
                y = mouse->y;
                ADBSetRelMouseMode(true);
            }

            if (debug_mouse && (mouse_event_count++ % 60 == 0)) {
                fprintf(stderr, "[IPC] Mouse event #%d: mode=%s x=%d y=%d buttons=0x%02x\n",
                        mouse_event_count, absolute ? "abs" : "rel", x, y, mouse->buttons);
            }

            if (absolute || x != 0 || y != 0) {
                ADBMouseMoved(x, y);
            }

            // Handle button changes
            static uint8_t last_buttons = 0;
            uint8_t changed = mouse->buttons ^ last_buttons;
            if (changed & MACEMU_MOUSE_LEFT) {
                if (mouse->buttons & MACEMU_MOUSE_LEFT)
                    ADBMouseDown(0);
                else
                    ADBMouseUp(0);
            }
            if (changed & MACEMU_MOUSE_RIGHT) {
                if (mouse->buttons & MACEMU_MOUSE_RIGHT)
                    ADBMouseDown(1);
                else
                    ADBMouseUp(1);
            }
            last_buttons = mouse->buttons;
            break;
        }
        case MACEMU_INPUT_PING: {
            if (len < sizeof(MacEmuPingInput)) return;
            const MacEmuPingInput* ping = (const MacEmuPingInput*)data;

            // Capture T3 timestamp: emulator received ping
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t t3_emulator_recv_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

            D(bug("IPC: Received ping #%u (t1=%llu t2=%llu t3=%llu)\n",
                  ping->sequence, ping->t1_browser_send_ms, ping->t2_server_recv_us, t3_emulator_recv_us));

            // Echo will happen in next video frame refresh
            macemu_echo_ping(video_shm, ping->sequence, ping->t1_browser_send_ms,
                           ping->t2_server_recv_us, t3_emulator_recv_us);
            break;
        }
        case MACEMU_INPUT_AUDIO_REQUEST: {
            if (len < sizeof(MacEmuAudioRequestInput)) return;
            const MacEmuAudioRequestInput* req = (const MacEmuAudioRequestInput*)data;
#ifdef ENABLE_IPC_AUDIO
            audio_request_data(req->requested_samples);
#else
            (void)req;  // Unused if audio disabled
#endif
            break;
        }
        case MACEMU_INPUT_MOUSE_MODE: {
            if (len < sizeof(MacEmuMouseModeInput)) return;
            const MacEmuMouseModeInput* mode = (const MacEmuMouseModeInput*)data;
            bool relative = (mode->mode == 1);
            ADBSetRelMouseMode(relative);
            fprintf(stderr, "IPC: Mouse mode changed to %s\n", relative ? "relative" : "absolute");
            break;
        }
        case MACEMU_INPUT_COMMAND: {
            if (len < sizeof(MacEmuCommandInput)) return;
            const MacEmuCommandInput* cmd = (const MacEmuCommandInput*)data;
            switch (cmd->command) {
                case MACEMU_CMD_START:
                    // Already running
                    break;
                case MACEMU_CMD_STOP:
                    fprintf(stderr, "IPC: Stop command received, triggering clean shutdown\n");
                    // Detach control thread so QuitEmulator() won't try to join it
                    // (we're running IN the control thread, can't join ourselves!)
                    if (control_thread.joinable()) {
                        control_thread.detach();
                    }
                    // Now safe to call QuitEmulator from this thread
                    QuitEmulator();
                    // Will never reach here
                    break;
                case MACEMU_CMD_RESET:
                    fprintf(stderr, "IPC: Reset command received\n");
                    // For restart, we still use exit() with special code
                    // Main will detect this and restart
                    exit(75);
                    break;
                case MACEMU_CMD_PAUSE:
                    if (video_shm) video_shm->state = MACEMU_STATE_PAUSED;
                    break;
                case MACEMU_CMD_RESUME:
                    if (video_shm) video_shm->state = MACEMU_STATE_RUNNING;
                    break;
            }
            break;
        }
        default:
            fprintf(stderr, "IPC: Unknown input type %d\n", hdr->type);
            break;
    }
}

/*
 *  Control socket thread - uses epoll for zero-latency input
 */

static void control_socket_thread() {
    uint8_t buffer[256];

    // Default to relative mouse mode (matches browser default)
    ADBSetRelMouseMode(true);

    // Create epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        fprintf(stderr, "IPC: Failed to create epoll: %s\n", strerror(errno));
        return;
    }

    // Add listen socket to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_socket;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_socket, &ev) < 0) {
        fprintf(stderr, "IPC: Failed to add listen socket to epoll: %s\n", strerror(errno));
        close(epoll_fd);
        return;
    }

    fprintf(stderr, "IPC: Control thread started with epoll (zero-latency input)\n");

    struct epoll_event events[2];

    while (control_thread_running) {
        // Wait for events with 100ms timeout (for clean shutdown check)
        int n = epoll_wait(epoll_fd, events, 2, 100);

        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "IPC: epoll_wait error: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            // New connection on listen socket
            if (fd == listen_socket && (events[i].events & EPOLLIN)) {
                if (control_socket < 0) {
                    struct sockaddr_un addr;
                    socklen_t len = sizeof(addr);
                    int new_fd = accept(listen_socket, (struct sockaddr*)&addr, &len);
                    if (new_fd >= 0) {
                        // Set non-blocking
                        int flags = fcntl(new_fd, F_GETFL, 0);
                        if (flags >= 0) {
                            fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);
                        }

                        control_socket = new_fd;
                        fprintf(stderr, "IPC: Server connected\n");

                        // Send eventfds to server via SCM_RIGHTS
                        if (video_shm && video_shm->frame_ready_eventfd >= 0) {
                            int fds[2];
                            int num_fds = 0;

                            // Always send video eventfd
                            fds[num_fds++] = video_shm->frame_ready_eventfd;

                            // Add audio eventfd if available
                            if (video_shm->audio_ready_eventfd >= 0) {
                                fds[num_fds++] = video_shm->audio_ready_eventfd;
                            }

                            struct msghdr msg = {};
                            struct cmsghdr *cmsg;
                            char buf[CMSG_SPACE(sizeof(int) * 2)];
                            char data = 'E';
                            struct iovec iov = { &data, 1 };

                            msg.msg_iov = &iov;
                            msg.msg_iovlen = 1;
                            msg.msg_control = buf;
                            msg.msg_controllen = sizeof(buf);

                            cmsg = CMSG_FIRSTHDR(&msg);
                            cmsg->cmsg_level = SOL_SOCKET;
                            cmsg->cmsg_type = SCM_RIGHTS;
                            cmsg->cmsg_len = CMSG_LEN(sizeof(int) * num_fds);
                            memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * num_fds);

                            if (sendmsg(control_socket, &msg, 0) > 0) {
                                fprintf(stderr, "IPC: Sent eventfd %d to server for low-latency sync\n",
                                        video_shm->frame_ready_eventfd);
                                if (num_fds > 1) {
                                    fprintf(stderr, "IPC: Sent audio eventfd %d to server\n",
                                            video_shm->audio_ready_eventfd);
                                }
                            } else {
                                fprintf(stderr, "IPC: Failed to send eventfd: %s\n", strerror(errno));
                            }
                        }

                        // Add control socket to epoll
                        ev.events = EPOLLIN;
                        ev.data.fd = control_socket;
                        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, control_socket, &ev);
                    }
                }
            }
            // Data available on control socket
            else if (fd == control_socket && (events[i].events & EPOLLIN)) {
                ssize_t n = recv(control_socket, buffer, sizeof(buffer), 0);
                if (n > 0) {
                    // Process complete messages
                    size_t offset = 0;
                    while (offset < (size_t)n) {
                        if (offset + sizeof(MacEmuInputHeader) > (size_t)n) break;
                        const MacEmuInputHeader* hdr = (const MacEmuInputHeader*)(buffer + offset);
                        size_t msg_size = 0;
                        switch (hdr->type) {
                            case MACEMU_INPUT_KEY:     msg_size = sizeof(MacEmuKeyInput); break;
                            case MACEMU_INPUT_MOUSE:   msg_size = sizeof(MacEmuMouseInput); break;
                            case MACEMU_INPUT_COMMAND: msg_size = sizeof(MacEmuCommandInput); break;
                            case MACEMU_INPUT_PING:    msg_size = sizeof(MacEmuPingInput); break;
                            case MACEMU_INPUT_AUDIO_REQUEST: msg_size = sizeof(MacEmuAudioRequestInput); break;
                            case MACEMU_INPUT_MOUSE_MODE: msg_size = sizeof(MacEmuMouseModeInput); break;
                            default: msg_size = sizeof(MacEmuInputHeader); break;
                        }
                        if (offset + msg_size > (size_t)n) break;
                        process_binary_input(buffer + offset, msg_size);
                        offset += msg_size;
                    }
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    // Connection closed or error
                    fprintf(stderr, "IPC: Server disconnected\n");
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, control_socket, nullptr);
                    close(control_socket);
                    control_socket = -1;
                }
            }
        }
    }

    close(epoll_fd);
    fprintf(stderr, "IPC: Control thread exiting\n");
}

/*
 *  Public API
 */

bool ControlIPCInit(MacEmuIPCBuffer* shm) {
    video_shm = shm;
    return create_control_socket();
}

void ControlIPCStart() {
    control_thread_running = true;
    control_thread = std::thread(control_socket_thread);
}

void ControlIPCExit() {
    control_thread_running = false;

    // Control thread may have been detached if QuitEmulator() was called from within it
    if (control_thread.joinable()) {
        control_thread.join();
    }

    destroy_control_socket();
}

int ControlIPCGetSocket() {
    return control_socket;
}

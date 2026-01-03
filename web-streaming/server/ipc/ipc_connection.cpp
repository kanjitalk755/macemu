/*
 * IPC Connection Manager Implementation
 */

#include "ipc_connection.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

namespace ipc {

IPCConnection::IPCConnection()
    : pid_(-1)
    , connected_(false)
    , video_shm_(nullptr)
    , video_shm_fd_(-1)
    , control_socket_(-1)
    , frame_ready_eventfd_(-1)
    , audio_ready_eventfd_(-1)
{
}

IPCConnection::~IPCConnection() {
    disconnect();
}

bool IPCConnection::connect_video_shm(pid_t pid) {
    shm_name_ = std::string(MACEMU_VIDEO_SHM_PREFIX) + std::to_string(pid);

    video_shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0);
    if (video_shm_fd_ < 0) {
        // Not an error during scanning - emulator may not exist yet
        return false;
    }

    // Map shared memory (read-write for server - needs to update audio_ring_read_pos)
    video_shm_ = (MacEmuIPCBuffer*)mmap(nullptr, sizeof(MacEmuIPCBuffer),
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         video_shm_fd_, 0);
    if (video_shm_ == MAP_FAILED) {
        fprintf(stderr, "IPC: Failed to map video SHM for PID %d: %s\n", pid, strerror(errno));
        close(video_shm_fd_);
        video_shm_fd_ = -1;
        video_shm_ = nullptr;
        return false;
    }

    if (getenv("MACEMU_DEBUG_CONNECTION")) {
        fprintf(stderr, "IPC: Mapped SHM at %p (size %zu bytes)\n", (void*)video_shm_, sizeof(MacEmuIPCBuffer));
    }

    // Validate
    int result = macemu_validate_ipc_buffer(video_shm_, pid);
    if (result != 0) {
        fprintf(stderr, "IPC: SHM validation failed for PID %d (error %d)\n", pid, result);
        munmap(video_shm_, sizeof(MacEmuIPCBuffer));
        close(video_shm_fd_);
        video_shm_fd_ = -1;
        video_shm_ = nullptr;
        return false;
    }

    fprintf(stderr, "IPC: Connected to video SHM '%s' (%dx%d)\n",
            shm_name_.c_str(), video_shm_->width, video_shm_->height);
    return true;
}

void IPCConnection::disconnect_video_shm() {
    if (video_shm_ && video_shm_ != MAP_FAILED) {
        if (getenv("MACEMU_DEBUG_CONNECTION")) {
            fprintf(stderr, "IPC: Unmapping SHM at %p\n", (void*)video_shm_);
        }

        munmap(video_shm_, sizeof(MacEmuIPCBuffer));
        video_shm_ = nullptr;
    }
    if (video_shm_fd_ >= 0) {
        close(video_shm_fd_);
        video_shm_fd_ = -1;
    }
    shm_name_.clear();
}

bool IPCConnection::connect_control_socket(pid_t pid) {
    socket_path_ = std::string(MACEMU_CONTROL_SOCK_PREFIX) + std::to_string(pid) +
                   std::string(MACEMU_CONTROL_SOCK_SUFFIX);

    control_socket_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (control_socket_ < 0) {
        fprintf(stderr, "IPC: Failed to create socket: %s\n", strerror(errno));
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(control_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(control_socket_);
        control_socket_ = -1;
        return false;
    }

    // Set non-blocking
    int flags = fcntl(control_socket_, F_GETFL, 0);
    if (flags < 0) {
        fprintf(stderr, "IPC: Failed to get socket flags: %s\n", strerror(errno));
        close(control_socket_);
        control_socket_ = -1;
        return false;
    }
    if (fcntl(control_socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "IPC: Failed to set non-blocking mode: %s\n", strerror(errno));
        close(control_socket_);
        control_socket_ = -1;
        return false;
    }

    fprintf(stderr, "IPC: Connected to control socket '%s'\n", socket_path_.c_str());

    // Receive eventfds from emulator via SCM_RIGHTS for low-latency notifications
    // The emulator sends video and audio eventfds immediately after accepting the connection
    struct msghdr msg = {};
    struct cmsghdr *cmsg;
    char buf[CMSG_SPACE(sizeof(int) * 2)];  // Space for 2 file descriptors
    char data;
    struct iovec iov = { &data, 1 };

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    // Try to receive the eventfds (with short timeout since it's sent immediately)
    if (fcntl(control_socket_, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        fprintf(stderr, "IPC: Warning: Failed to set blocking mode: %s\n", strerror(errno));
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    if (setsockopt(control_socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        fprintf(stderr, "IPC: Warning: Failed to set socket timeout: %s\n", strerror(errno));
    }

    ssize_t n = recvmsg(control_socket_, &msg, 0);
    if (n > 0 && data == 'E') {
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                // Receive 1 or 2 eventfds (video, and optionally audio)
                size_t num_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                int* fds = (int*)CMSG_DATA(cmsg);

                if (num_fds >= 1) {
                    frame_ready_eventfd_ = fds[0];
                    fprintf(stderr, "IPC: Received eventfd %d from emulator for low-latency sync\n", frame_ready_eventfd_);
                }
                if (num_fds >= 2) {
                    audio_ready_eventfd_ = fds[1];
                    fprintf(stderr, "IPC: Received audio eventfd %d from emulator\n", audio_ready_eventfd_);
                }
                break;
            }
        }
    }

    // Restore non-blocking mode
    if (fcntl(control_socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "IPC: Warning: Failed to restore non-blocking mode: %s\n", strerror(errno));
    }

    return true;
}

void IPCConnection::disconnect_control_socket() {
    if (control_socket_ >= 0) {
        close(control_socket_);
        control_socket_ = -1;
    }
    if (frame_ready_eventfd_ >= 0) {
        close(frame_ready_eventfd_);
        frame_ready_eventfd_ = -1;
    }
    if (audio_ready_eventfd_ >= 0) {
        close(audio_ready_eventfd_);
        audio_ready_eventfd_ = -1;
    }
    socket_path_.clear();
}

bool IPCConnection::connect_to_emulator(pid_t pid) {
    if (connected_) {
        disconnect();
    }

    // First try to connect to SHM
    if (!connect_video_shm(pid)) {
        return false;
    }

    // Then try to connect to control socket
    if (!connect_control_socket(pid)) {
        disconnect_video_shm();
        return false;
    }

    pid_ = pid;
    connected_ = true;
    return true;
}

void IPCConnection::disconnect() {
    disconnect_control_socket();
    disconnect_video_shm();
    pid_ = -1;
    connected_ = false;
}

bool IPCConnection::is_connected() const {
    return connected_;
}

pid_t IPCConnection::get_pid() const {
    return pid_;
}

MacEmuIPCBuffer* IPCConnection::get_shm() {
    return video_shm_;
}

const MacEmuIPCBuffer* IPCConnection::get_shm() const {
    return video_shm_;
}

int IPCConnection::get_frame_eventfd() const {
    return frame_ready_eventfd_;
}

int IPCConnection::get_audio_eventfd() const {
    return audio_ready_eventfd_;
}

int IPCConnection::get_control_socket() const {
    return control_socket_;
}

std::string IPCConnection::get_shm_name() const {
    return shm_name_;
}

std::string IPCConnection::get_socket_path() const {
    return socket_path_;
}

// Input sending methods

bool IPCConnection::send_key_input(int mac_keycode, bool down) {
    if (control_socket_ < 0) return false;

    MacEmuKeyInput msg;
    msg.hdr.type = MACEMU_INPUT_KEY;
    msg.hdr.flags = down ? MACEMU_KEY_DOWN : MACEMU_KEY_UP;
    msg.hdr._reserved = 0;
    msg.mac_keycode = mac_keycode;
    msg.modifiers = 0;  // TODO: track modifier state
    msg._reserved = 0;

    return send(control_socket_, &msg, sizeof(msg), MSG_NOSIGNAL) == sizeof(msg);
}

bool IPCConnection::send_mouse_input(int dx, int dy, uint8_t buttons, uint64_t browser_timestamp_ms, bool absolute) {
    if (control_socket_ < 0) return false;

    MacEmuMouseInput msg;
    msg.hdr.type = MACEMU_INPUT_MOUSE;
    msg.hdr.flags = absolute ? MACEMU_MOUSE_ABSOLUTE : 0;
    msg.hdr._reserved = 0;
    msg.x = dx;  // Either delta (relative) or coordinate (absolute)
    msg.y = dy;
    msg.buttons = buttons;
    memset(msg._reserved, 0, sizeof(msg._reserved));
    msg.timestamp_ms = browser_timestamp_ms;

    return send(control_socket_, &msg, sizeof(msg), MSG_NOSIGNAL) == sizeof(msg);
}

bool IPCConnection::send_mouse_mode_change(bool relative) {
    if (control_socket_ < 0) return false;

    MacEmuMouseModeInput msg;
    msg.hdr.type = MACEMU_INPUT_MOUSE_MODE;
    msg.hdr.flags = 0;
    msg.hdr._reserved = 0;
    msg.mode = relative ? 1 : 0;
    memset(msg._reserved, 0, sizeof(msg._reserved));

    return send(control_socket_, &msg, sizeof(msg), MSG_NOSIGNAL) == sizeof(msg);
}

bool IPCConnection::send_command(uint8_t command) {
    if (control_socket_ < 0) return false;

    MacEmuCommandInput msg;
    msg.hdr.type = MACEMU_INPUT_COMMAND;
    msg.hdr.flags = 0;
    msg.hdr._reserved = 0;
    msg.command = command;
    memset(msg._reserved, 0, sizeof(msg._reserved));

    return send(control_socket_, &msg, sizeof(msg), MSG_NOSIGNAL) == sizeof(msg);
}

bool IPCConnection::send_ping_input(uint32_t sequence, uint64_t t1_browser_send_ms) {
    if (control_socket_ < 0) return false;

    // Add server receive timestamp (t2)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t2_server_recv_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    MacEmuPingInput msg;
    msg.hdr.type = MACEMU_INPUT_PING;
    msg.hdr.flags = 0;
    msg.hdr._reserved = 0;
    msg.sequence = sequence;
    msg.t1_browser_send_ms = t1_browser_send_ms;
    msg.t2_server_recv_us = t2_server_recv_us;
    msg.t3_emulator_recv_us = 0;  // Will be filled by emulator

    return send(control_socket_, &msg, sizeof(msg), MSG_NOSIGNAL) == sizeof(msg);
}

// Helper functions

std::vector<pid_t> scan_for_emulators() {
    std::vector<pid_t> pids;

    DIR* dir = opendir("/dev/shm");
    if (!dir) return pids;

    struct dirent* entry;
    const char* prefix = "macemu-video-";
    size_t prefix_len = strlen(prefix);

    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, prefix, prefix_len) == 0) {
            pid_t pid = atoi(entry->d_name + prefix_len);
            if (pid > 0) {
                // Check if process still exists
                if (kill(pid, 0) == 0) {
                    pids.push_back(pid);
                }
            }
        }
    }
    closedir(dir);

    return pids;
}

bool try_connect_to_emulator(pid_t pid, IPCConnection& conn) {
    return conn.connect_to_emulator(pid);
}

} // namespace ipc

/*
 * IPC Connection Manager
 *
 * Manages connection to emulator's IPC resources (shared memory, control socket, eventfds).
 * The emulator owns and creates these resources; the server connects to them by PID.
 *
 * Architecture:
 * - Emulator creates SHM at /macemu-video-{PID}
 * - Emulator creates Unix socket at /tmp/macemu-{PID}.sock
 * - Server connects read-only to SHM, read-write to socket
 * - Eventfds transferred via SCM_RIGHTS for frame/audio notifications
 */

#ifndef IPC_CONNECTION_H
#define IPC_CONNECTION_H

#include "ipc_protocol.h"
#include <string>
#include <vector>
#include <sys/types.h>

namespace ipc {

/**
 * IPC Connection Manager
 * Encapsulates all communication with a single emulator instance
 */
class IPCConnection {
public:
    IPCConnection();
    ~IPCConnection();

    // Connection management
    bool connect_to_emulator(pid_t pid);
    void disconnect();
    bool is_connected() const;
    pid_t get_pid() const;

    // Resource access
    MacEmuIPCBuffer* get_shm();
    const MacEmuIPCBuffer* get_shm() const;
    int get_frame_eventfd() const;
    int get_audio_eventfd() const;
    int get_control_socket() const;

    // Input sending
    bool send_key_input(int mac_keycode, bool down);
    bool send_mouse_input(int dx, int dy, uint8_t buttons, uint64_t browser_timestamp_ms, bool absolute = false);
    bool send_mouse_mode_change(bool relative);
    bool send_command(uint8_t command);
    bool send_ping_input(uint32_t sequence, uint64_t t1_browser_send_ms);

    // Connection info
    std::string get_shm_name() const;
    std::string get_socket_path() const;

private:
    // Shared memory
    bool connect_video_shm(pid_t pid);
    void disconnect_video_shm();

    // Control socket
    bool connect_control_socket(pid_t pid);
    void disconnect_control_socket();

    // State
    pid_t pid_;
    bool connected_;

    // IPC handles
    MacEmuIPCBuffer* video_shm_;
    int video_shm_fd_;
    int control_socket_;
    int frame_ready_eventfd_;
    int audio_ready_eventfd_;

    // Connection info
    std::string shm_name_;
    std::string socket_path_;
};

/**
 * Scan /dev/shm for running emulator instances
 * @return Vector of PIDs with valid macemu SHM
 */
std::vector<pid_t> scan_for_emulators();

/**
 * Try to connect to an emulator by PID
 * Convenience function that creates connection and validates
 * @param pid Emulator PID to connect to
 * @param conn IPCConnection to populate
 * @return true if successfully connected
 */
bool try_connect_to_emulator(pid_t pid, IPCConnection& conn);

} // namespace ipc

#endif // IPC_CONNECTION_H

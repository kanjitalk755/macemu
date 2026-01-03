/*
 * Emulator Process Manager
 *
 * Manages the lifecycle of emulator processes (BasiliskII/SheepShaver)
 * when the server is responsible for starting them.
 */

#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <string>
#include <sys/types.h>

// Forward declarations
namespace server_config {
    struct ServerConfig;
}

namespace emulator {

/**
 * Emulator Process Manager
 * Handles finding, starting, stopping, and monitoring emulator processes
 */
class ProcessManager {
public:
    explicit ProcessManager(const server_config::ServerConfig& config);
    ~ProcessManager();

    /**
     * Find emulator executable
     * @return Path to emulator, or empty string if not found
     */
    std::string find_emulator() const;

    /**
     * Start the emulator process
     * @return true if started successfully
     */
    bool start();

    /**
     * Stop the emulator process (graceful then force)
     */
    void stop();

    /**
     * Check emulator status
     * @return 0 if still running, -1 if not running, positive if exited with code
     */
    int check_status();

    /**
     * Get PID of started emulator
     * @return PID or -1 if not started
     */
    pid_t get_pid() const;

    /**
     * Check if emulator was started by this manager
     * @return true if we started it
     */
    bool is_managed() const;

private:
    const server_config::ServerConfig& config_;
    pid_t started_pid_;
};

} // namespace emulator

#endif // PROCESS_MANAGER_H

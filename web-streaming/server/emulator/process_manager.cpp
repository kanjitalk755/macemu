/*
 * Emulator Process Manager Implementation
 */

#include "process_manager.h"
#include "../config/server_config.h"
#include "ipc_protocol.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

namespace emulator {

ProcessManager::ProcessManager(const server_config::ServerConfig& config)
    : config_(config)
    , started_pid_(-1)
{
}

ProcessManager::~ProcessManager() {
    // Don't auto-stop in destructor - let main() control lifecycle
}

std::string ProcessManager::find_emulator() const {
    // If path explicitly set, use it
    if (!config_.emulator_path.empty()) {
        if (access(config_.emulator_path.c_str(), X_OK) == 0) {
            return config_.emulator_path;
        }
        fprintf(stderr, "Emulator: Specified path not executable: %s\n",
                config_.emulator_path.c_str());
        return "";
    }

    // Look for emulator in bin/ subdirectory only
    const char* candidates[] = {
        "./bin/BasiliskII",
        "./bin/SheepShaver",
        nullptr
    };

    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], X_OK) == 0) {
            char* resolved = realpath(candidates[i], nullptr);
            if (resolved) {
                std::string path(resolved);
                free(resolved);
                return path;
            }
        }
    }

    return "";
}

bool ProcessManager::start() {
    if (started_pid_ > 0) {
        // Already started one, check if still alive
        int status;
        pid_t result = waitpid(started_pid_, &status, WNOHANG);
        if (result == 0) {
            // Still running
            return true;
        }
        // Exited
        started_pid_ = -1;
    }

    std::string emu_path = find_emulator();
    if (emu_path.empty()) {
        fprintf(stderr, "Emulator: No emulator found. Place BasiliskII or SheepShaver in ./bin/\n");
        return false;
    }

    fprintf(stderr, "Emulator: Starting %s --config %s\n",
            emu_path.c_str(), config_.prefs_path.c_str());

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Emulator: Fork failed: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        // Child process

        // Close server's file descriptors
        for (int fd = 3; fd < 1024; fd++) {
            close(fd);
        }

        // Pass debug flags to emulator via environment variables
        if (config_.debug_mode_switch) setenv("MACEMU_DEBUG_MODE_SWITCH", "1", 1);
        if (config_.debug_perf) setenv("MACEMU_DEBUG_PERF", "1", 1);
        if (config_.debug_frames) setenv("MACEMU_DEBUG_FRAMES", "1", 1);
        if (config_.debug_audio) setenv("MACEMU_DEBUG_AUDIO", "1", 1);

        // Execute emulator with prefs file
        // BasiliskII uses --config, SheepShaver uses --prefs
        if (emu_path.find("SheepShaver") != std::string::npos) {
            execl(emu_path.c_str(), emu_path.c_str(),
                  "--prefs", config_.prefs_path.c_str(), nullptr);
        } else {
            execl(emu_path.c_str(), emu_path.c_str(),
                  "--config", config_.prefs_path.c_str(), nullptr);
        }

        // If exec fails
        fprintf(stderr, "Emulator: Exec failed: %s\n", strerror(errno));
        _exit(1);
    }

    // Parent process
    started_pid_ = pid;
    fprintf(stderr, "Emulator: Started with PID %d\n", pid);
    return true;
}

void ProcessManager::stop() {
    if (started_pid_ <= 0) return;

    fprintf(stderr, "Emulator: Stopping PID %d\n", started_pid_);

    // Send SIGTERM for graceful shutdown
    kill(started_pid_, SIGTERM);

    // Wait up to 3 seconds
    for (int i = 0; i < 30; i++) {
        int status;
        pid_t result = waitpid(started_pid_, &status, WNOHANG);
        if (result != 0) {
            started_pid_ = -1;
            fprintf(stderr, "Emulator: Stopped\n");
            return;
        }
        usleep(100000);  // 100ms
    }

    // Force kill
    fprintf(stderr, "Emulator: Force killing\n");
    kill(started_pid_, SIGKILL);
    waitpid(started_pid_, nullptr, 0);
    started_pid_ = -1;
}

int ProcessManager::check_status() {
    if (started_pid_ <= 0) return -1;

    int status;
    pid_t result = waitpid(started_pid_, &status, WNOHANG);
    if (result > 0) {
        // Emulator exited
        int exit_code = -1;
        if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
            fprintf(stderr, "Emulator: Exited with code %d\n", exit_code);
            if (exit_code == 75) {
                fprintf(stderr, "Emulator: Restart requested (exit code 75)\n");
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            fprintf(stderr, "Emulator: Killed by signal %d\n", sig);
        }
        started_pid_ = -1;
        return exit_code;
    }

    return 0;  // Still running
}

pid_t ProcessManager::get_pid() const {
    return started_pid_;
}

bool ProcessManager::is_managed() const {
    return started_pid_ > 0;
}

} // namespace emulator

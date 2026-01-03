/*
 * video_ipc_sheep.cpp - IPC video driver for SheepShaver
 *
 * Based on BasiliskII/src/IPC/video_ipc.cpp but adapted for SheepShaver's
 * VideoInfo/VidLocals architecture instead of monitor_desc.
 *
 * This driver creates shared memory and Unix domain socket for communication
 * with the standalone WebRTC streaming server.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "user_strings.h"
#include "video.h"
#include "video_defs.h"
#include "vm_alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>

#include "ipc_protocol.h"
#include "control_ipc.h"

#define DEBUG 0
#include "debug.h"

// IPC resources (emulator-owned)
static MacEmuIPCBuffer* video_shm = nullptr;
static int shm_fd = -1;
static std::string shm_name;
static std::atomic<bool> video_thread_running(false);
static std::thread video_thread;

// Mac framebuffer (allocated via vm_acquire)
static uint8* the_buffer = nullptr;
static uint32 the_buffer_size = 0;

// Current frame parameters
static int frame_width = 1024;
static int frame_height = 768;
static int frame_depth = 32;
static int frame_bytes_per_row = 0;
static uint32 current_palette[256 * 3];  // RGB triplets for indexed modes

// Debug flags (read from environment once at startup)
static bool g_debug_perf = false;
static bool g_debug_mode_switch = false;
static bool g_debug_frames = false;

// Helper function for computing bytes per row
static inline int TrivialBytesPerRow(int width, int depth_code) {
    int bytes_per_pixel;
    switch (depth_code) {
        case APPLE_1_BIT: bytes_per_pixel = 1; break;
        case APPLE_2_BIT: bytes_per_pixel = 1; break;
        case APPLE_4_BIT: bytes_per_pixel = 1; break;
        case APPLE_8_BIT: bytes_per_pixel = 1; break;
        case APPLE_16_BIT: bytes_per_pixel = 2; break;
        case APPLE_32_BIT: bytes_per_pixel = 4; break;
        default: bytes_per_pixel = 1; break;
    }
    return width * bytes_per_pixel;
}

// Forward declarations
static bool create_video_shm();
static void destroy_video_shm();
static void video_refresh_thread();
static void convert_frame_to_bgra();
static void macemu_frame_complete();


/*
 *  Shared memory creation (emulator owns this)
 */

static bool create_video_shm()
{
    pid_t pid = getpid();
    shm_name = "/macemu-video-" + std::to_string(pid);

    // Remove any stale SHM with same name
    shm_unlink(shm_name.c_str());

    // Create new shared memory
    shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0600);
    if (shm_fd < 0) {
        fprintf(stderr, "IPC: Failed to create shared memory %s: %s\n",
                shm_name.c_str(), strerror(errno));
        return false;
    }

    // Set size
    size_t shm_size = sizeof(MacEmuIPCBuffer);
    if (ftruncate(shm_fd, shm_size) < 0) {
        fprintf(stderr, "IPC: Failed to size shared memory: %s\n", strerror(errno));
        close(shm_fd);
        shm_unlink(shm_name.c_str());
        return false;
    }

    // Map into memory
    video_shm = (MacEmuIPCBuffer*)mmap(nullptr, shm_size,
                                        PROT_READ | PROT_WRITE,
                                        MAP_SHARED, shm_fd, 0);
    if (video_shm == MAP_FAILED) {
        fprintf(stderr, "IPC: Failed to mmap shared memory: %s\n", strerror(errno));
        close(shm_fd);
        shm_unlink(shm_name.c_str());
        return false;
    }

    // Initialize header using the protocol helper function
    macemu_init_ipc_buffer(video_shm, pid, frame_width, frame_height);

    fprintf(stderr, "IPC: Created shared memory: %s (%zu bytes)\n",
            shm_name.c_str(), shm_size);

    return true;
}

static void destroy_video_shm()
{
    if (video_shm) {
        video_shm->state = MACEMU_STATE_STOPPED;
        munmap(video_shm, sizeof(MacEmuIPCBuffer));
        video_shm = nullptr;
    }

    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }

    if (!shm_name.empty()) {
        shm_unlink(shm_name.c_str());
        shm_name.clear();
    }
}


/*
 *  Frame signaling
 */

static void macemu_frame_complete()
{
    if (!video_shm) return;

    // Use the helper function from ipc_protocol.h
    auto now = std::chrono::steady_clock::now();
    auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    ::macemu_frame_complete(video_shm, timestamp_us);
}


/*
 *  Frame conversion (Mac framebuffer → BGRA in SHM)
 */

static void convert_frame_to_bgra()
{
    if (!video_shm || !the_buffer) return;

    // Get next write buffer (triple buffering)
    // Note: write_index/ready_index are plain uint32_t, not atomics
    // They're synchronized by the eventfd write in macemu_frame_complete()
    uint32_t write_idx = video_shm->write_index;
    uint32_t next_write = (write_idx + 1) % MACEMU_NUM_BUFFERS;
    uint32_t ready_idx = video_shm->ready_index;

    // Skip if server is still reading (avoid tearing)
    if (next_write == ready_idx) {
        return;
    }

    uint8_t* dest = video_shm->frames[write_idx];
    const uint8_t* src = the_buffer;

    // Convert based on current depth
    // TODO: Implement actual conversion (for now, just copy if 32-bit)
    if (frame_depth == 32) {
        // Mac framebuffer is likely ARGB, need to convert to BGRA
        for (int y = 0; y < frame_height; y++) {
            const uint8_t* src_row = src + y * frame_bytes_per_row;
            uint8_t* dest_row = dest + y * MACEMU_MAX_WIDTH * 4;

            for (int x = 0; x < frame_width; x++) {
                // Mac: A R G B (big-endian ARGB)
                // SHM: B G R A (BGRA for libyuv)
                uint8_t a = src_row[x * 4 + 0];
                uint8_t r = src_row[x * 4 + 1];
                uint8_t g = src_row[x * 4 + 2];
                uint8_t b = src_row[x * 4 + 3];

                dest_row[x * 4 + 0] = b;
                dest_row[x * 4 + 1] = g;
                dest_row[x * 4 + 2] = r;
                dest_row[x * 4 + 3] = 0xFF; // Always opaque
            }
        }
    }

    // Don't update write_index here - macemu_frame_complete() handles that
}


/*
 *  Video refresh thread (runs at 60 FPS)
 */

static void video_refresh_thread()
{
    fprintf(stderr, "IPC: Video refresh thread started\n");

    static uint64_t frame_count = 0;

    while (video_thread_running) {
        auto frame_start = std::chrono::steady_clock::now();

        // Convert Mac framebuffer to BGRA
        convert_frame_to_bgra();

        // Signal server
        macemu_frame_complete();

        frame_count++;
        if (g_debug_frames && (frame_count % 60 == 0)) {
            fprintf(stderr, "[Emulator] Sent frame #%lu (60 fps)\n", frame_count);
        }

        // 60 FPS = 16.67ms per frame
        auto frame_end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame_end - frame_start);

        if (elapsed.count() < 16) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16 - elapsed.count()));
        }
    }

    fprintf(stderr, "IPC: Video refresh thread stopped\n");
}


/*
 *  Platform-specific video functions (called from video.cpp)
 */

bool VideoInit(void)
{
    D(bug("VideoInit\n"));

    // Read debug flags
    g_debug_perf = (getenv("MACEMU_DEBUG_PERF") != nullptr);
    g_debug_mode_switch = (getenv("MACEMU_DEBUG_MODE_SWITCH") != nullptr);
    g_debug_frames = (getenv("MACEMU_DEBUG_FRAMES") != nullptr);

    fprintf(stderr, "IPC: Initializing SheepShaver video driver (v3)\n");

    // Parse screen prefs (format: "ipc/1024/768")
    const char* mode_str = PrefsFindString("screen");
    if (mode_str) {
        int w, h;
        if (sscanf(mode_str, "ipc/%d/%d", &w, &h) == 2) {
            frame_width = w;
            frame_height = h;
        }
    }

    // Clamp to supported range
    if (frame_width < 512) frame_width = 512;
    if (frame_width > MACEMU_MAX_WIDTH) frame_width = MACEMU_MAX_WIDTH;
    if (frame_height < 384) frame_height = 384;
    if (frame_height > MACEMU_MAX_HEIGHT) frame_height = MACEMU_MAX_HEIGHT;

    frame_depth = 32;  // Start with 32-bit (most common)
    frame_bytes_per_row = TrivialBytesPerRow(frame_width, APPLE_32_BIT);

    // Create IPC resources
    if (!create_video_shm()) {
        return false;
    }

    if (!ControlIPCInit(video_shm)) {
        destroy_video_shm();
        return false;
    }

    // Allocate Mac framebuffer
    the_buffer_size = frame_bytes_per_row * frame_height;
    the_buffer = (uint8*)vm_acquire(the_buffer_size, VM_MAP_DEFAULT | VM_MAP_32BIT);
    if (!the_buffer) {
        fprintf(stderr, "IPC: Failed to allocate framebuffer (%u bytes)\n", the_buffer_size);
        ControlIPCExit();
        destroy_video_shm();
        return false;
    }

    // Set global video state
    screen_base = (uint32)Host2MacAddr(the_buffer);
    VModes[0].viType = DIS_SCREEN;
    VModes[0].viRowBytes = frame_bytes_per_row;
    VModes[0].viXsize = frame_width;
    VModes[0].viYsize = frame_height;
    VModes[0].viAppleMode = APPLE_32_BIT;
    VModes[0].viAppleID = APPLE_CUSTOM;

    // Mark end of modes list
    VModes[1].viType = DIS_INVALID;

    cur_mode = 0;
    display_type = DIS_SCREEN;

    // Start video refresh thread
    video_thread_running = true;
    video_thread = std::thread(video_refresh_thread);

    // Start control socket thread (input handling)
    ControlIPCStart();

    fprintf(stderr, "IPC: Video initialized (%dx%d @ %d bpp)\n",
            frame_width, frame_height, frame_depth);
    fprintf(stderr, "IPC: Waiting for server to connect (PID %d)...\n", getpid());

    return true;
}


void VideoExit(void)
{
    D(bug("VideoExit\n"));

    // Stop video thread
    video_thread_running = false;
    if (video_thread.joinable()) {
        video_thread.join();
    }

    // Cleanup IPC resources
    ControlIPCExit();
    destroy_video_shm();

    // Free framebuffer
    if (the_buffer) {
        vm_release(the_buffer, the_buffer_size);
        the_buffer = nullptr;
        the_buffer_size = 0;
    }

    fprintf(stderr, "IPC: Video driver shut down\n");
}


void VideoQuitFullScreen(void)
{
    D(bug("VideoQuitFullScreen\n"));
    // No-op for IPC driver
}


void VideoVBL(void)
{
    // Trigger VBL interrupt via VSL
    if (private_data && private_data->interruptsEnabled) {
        VSLDoInterruptService(private_data->vslServiceID);
    }
}


void video_set_palette(void)
{
    D(bug("video_set_palette\n"));

    // Copy palette from Mac OS globals to our cache
    for (int i = 0; i < 256; i++) {
        current_palette[i * 3 + 0] = mac_pal[i].red;
        current_palette[i * 3 + 1] = mac_pal[i].green;
        current_palette[i * 3 + 2] = mac_pal[i].blue;
    }

    // TODO: Use palette for indexed mode conversion
}


void video_set_gamma(int gamma)
{
    D(bug("video_set_gamma(%d)\n", gamma));
    // TODO: Implement gamma correction for IPC driver
    // For now, this is a no-op
}


void video_set_cursor(void)
{
    D(bug("video_set_cursor\n"));

    if (!video_shm || !private_data) return;

    // Update cursor position for browser rendering
    // TODO: Hardware cursor bitmap not yet implemented in protocol
    video_shm->cursor_x = private_data->cursorX;
    video_shm->cursor_y = private_data->cursorY;
    video_shm->cursor_visible = private_data->cursorVisible ? 1 : 0;
}


bool video_can_change_cursor(void)
{
    // IPC driver supports hardware cursor
    return true;
}


int16 video_mode_change(VidLocals *csSave, uint32 ParamPtr)
{
    D(bug("video_mode_change\n"));

    uint16 mode = ReadMacInt16(ParamPtr + csMode);
    uint32 id = ReadMacInt32(ParamPtr + csData);

    // Find matching mode in VModes[]
    for (int i = 0; VModes[i].viType != DIS_INVALID; i++) {
        if (VModes[i].viAppleMode == mode && VModes[i].viAppleID == id) {
            cur_mode = i;
            csSave->saveMode = mode;
            csSave->saveData = id;
            csSave->saveBaseAddr = screen_base;

            // Update frame parameters
            frame_width = VModes[i].viXsize;
            frame_height = VModes[i].viYsize;
            frame_bytes_per_row = VModes[i].viRowBytes;

            // Update SHM
            if (video_shm) {
                video_shm->width = frame_width;
                video_shm->height = frame_height;
            }

            if (g_debug_mode_switch) {
                fprintf(stderr, "IPC: Mode change: %dx%d @ mode 0x%02x\n",
                        frame_width, frame_height, mode);
            }

            return noErr;
        }
    }

    return paramErr;
}


void video_set_dirty_area(int x, int y, int w, int h)
{
    // TODO: Track dirty rectangles for optimization
    // For now, we refresh the entire frame every tick
}


/*
 *  Get video shared memory pointer (for audio IPC)
 */

MacEmuIPCBuffer* IPC_GetVideoSHM(void)
{
    return video_shm;
}

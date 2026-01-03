/*
 *  video_ipc.cpp - IPC-based video driver for standalone WebRTC server
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *  IPC mode (C) 2024
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 *  NOTES:
 *    New architecture (v3): Emulator OWNS all IPC resources.
 *
 *    - Emulator creates SHM at /macemu-video-{PID}
 *    - Emulator creates Unix socket at /tmp/macemu-{PID}.sock
 *    - Emulator converts Mac framebuffer to I420 using libyuv
 *    - Server connects by PID and reads I420 directly for H.264 encoding
 *
 *    Triple buffering with atomics - no locks, minimal latency.
 */

#include "sysdeps.h"

#ifdef ENABLE_IPC_VIDEO

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

// POSIX IPC
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

// libyuv for format conversion
#include <libyuv.h>

#include "cpu_emulation.h"
#include "main.h"
#include "adb.h"
#include "macos_util.h"
#include "prefs.h"
#include "user_strings.h"
#include "video.h"
#include "video_defs.h"
#include "video_blit.h"
#include "vm_alloc.h"

// IPC protocol definitions
#include "ipc_protocol.h"

// Audio subsystem (for pull model requests)
#include "audio.h"
#ifdef ENABLE_IPC_AUDIO
#include "audio_ipc.h"
#endif
#include "control_ipc.h"

#define DEBUG 0
#include "debug.h"

// Forward declarations
static bool IPC_VideoInit(bool classic);
static void IPC_VideoExit(void);
static void IPC_VideoQuitFullScreen(void);
static void IPC_VideoInterrupt(void);
static void IPC_VideoRefresh(void);

// Supported video modes
using std::vector;
static vector<VIDEO_MODE> VideoModes;

// Mac Screen Width and Height
#ifdef USE_SDL_VIDEO
extern uint32 MacScreenWidth;
extern uint32 MacScreenHeight;
#else
uint32 MacScreenWidth;
uint32 MacScreenHeight;
#endif

// Global variables
static uint32 frame_skip;
static uint8 *the_buffer = NULL;           // Mac frame buffer (local, not in SHM)
static uint32 the_buffer_size;             // Size of allocated buffer
static bool classic_mode = false;          // Classic Mac video mode

// Frame buffer properties
static int frame_width = 640;
static int frame_height = 480;
static int frame_depth = 32;               // Bits per pixel
static int frame_bytes_per_row;

// Threading
static std::thread video_thread;
static std::atomic<bool> video_thread_running(false);

// IPC handles - emulator creates and owns these
static int video_shm_fd = -1;
static MacEmuIPCBuffer* video_shm = nullptr;
static std::string shm_name;

// Ping tracking - NOT in shared memory, purely emulator-side state
static uint32_t last_echoed_ping_seq = 0;  // Last ping seq we've set t4 for
static uint32_t ping_echo_frames_remaining = 0;  // How many more frames to echo this ping (0 = no echo)

// Mouse latency tracking (browser timestamp → emulator receive)
static std::atomic<uint64_t> mouse_latency_total_ms(0);
static std::atomic<int> mouse_latency_samples(0);
static std::chrono::steady_clock::time_point latency_epoch;
static bool latency_epoch_set = false;

// Palette for indexed color modes
static uint8 current_palette[256 * 3];

// Debug flags (read once at initialization)
static bool g_debug_perf = false;
static bool g_debug_mode_switch = false;
static bool g_debug_mouse = false;


/*
 *  Create shared memory 
 */

static bool create_video_shm() {
    pid_t pid = getpid();
    shm_name = std::string(MACEMU_VIDEO_SHM_PREFIX) + std::to_string(pid);

    // Remove any stale shm
    shm_unlink(shm_name.c_str());

    video_shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (video_shm_fd < 0) {
        fprintf(stderr, "IPC: Failed to create video SHM '%s': %s\n",
                shm_name.c_str(), strerror(errno));
        return false;
    }

    // Fixed size for all resolutions up to 1080p
    size_t shm_size = sizeof(MacEmuIPCBuffer);
    if (ftruncate(video_shm_fd, shm_size) < 0) {
        fprintf(stderr, "IPC: Failed to size video SHM: %s\n", strerror(errno));
        close(video_shm_fd);
        shm_unlink(shm_name.c_str());
        video_shm_fd = -1;
        return false;
    }

    video_shm = (MacEmuIPCBuffer*)mmap(nullptr, shm_size,
                                          PROT_READ | PROT_WRITE, MAP_SHARED,
                                          video_shm_fd, 0);
    if (video_shm == MAP_FAILED) {
        fprintf(stderr, "IPC: Failed to map video SHM: %s\n", strerror(errno));
        close(video_shm_fd);
        shm_unlink(shm_name.c_str());
        video_shm_fd = -1;
        video_shm = nullptr;
        return false;
    }

    // Initialize header
    macemu_init_ipc_buffer(video_shm, pid, frame_width, frame_height);

    // Check if eventfd creation failed
    if (video_shm->frame_ready_eventfd < 0) {
        fprintf(stderr, "IPC: Failed to initialize video buffer (eventfd creation failed)\n");
        munmap(video_shm, shm_size);
        close(video_shm_fd);
        shm_unlink(shm_name.c_str());
        video_shm_fd = -1;
        video_shm = nullptr;
        return false;
    }

    fprintf(stderr, "IPC: Created video SHM '%s' (%dx%d, %.1f MB)\n",
            shm_name.c_str(), frame_width, frame_height,
            shm_size / (1024.0 * 1024.0));
    return true;
}

static void destroy_video_shm() {
    if (video_shm && video_shm != MAP_FAILED) {
        munmap(video_shm, sizeof(MacEmuIPCBuffer));
        video_shm = nullptr;
    }
    if (video_shm_fd >= 0) {
        close(video_shm_fd);
        shm_unlink(shm_name.c_str());
        video_shm_fd = -1;
    }
}


/*
 *  Create Unix socket for input (emulator owns this)
 */



/*
 *  Update ping timestamp on frame completion
 *  Called after each frame is complete to check if we should set t4 for a waiting ping
 */
static void update_ping_on_frame_complete(uint64_t timestamp_us) {
    if (!video_shm) return;

    // Read current ping sequence (atomic read-acquire ensures we see all timestamp writes)
    uint32_t current_ping_seq = ATOMIC_LOAD(video_shm->ping_sequence);

    // Check if this is a NEW ping that we haven't echoed yet
    if (current_ping_seq > last_echoed_ping_seq) {
        // Set t4 timestamp (when frame completed after ping arrived)
        video_shm->ping_timestamps.t4_frame_us = timestamp_us;

        // Update our tracking state
        last_echoed_ping_seq = current_ping_seq;
        ping_echo_frames_remaining = 5;  // Echo in next 5 frames

        // Debug logging - only if debug_perf enabled
        if (g_debug_perf) {
            fprintf(stderr, "[Emulator] Ping #%u ready (t4=%llu)\n",
                    current_ping_seq, (unsigned long long)timestamp_us);
        }
    } else if (ping_echo_frames_remaining > 0) {
        // Still echoing previous ping - silently decrement counter
        ping_echo_frames_remaining--;
    }
}


/*
 *  Convert Mac framebuffer to BGRA and signal frame complete
 *
 *  Always outputs BGRA (bytes B,G,R,A in memory) which is libyuv "ARGB".
 *  This simplifies the server to a single code path.
 *
 *  - 32-bit Mac: Shuffle A,R,G,B → B,G,R,A using libyuv::ARGBToBGRA
 *  - 16-bit/indexed: Convert to BGRA directly
 */

static void convert_frame_to_bgra() {
    if (!video_shm || !the_buffer) return;

    uint32_t width = video_shm->width;
    uint32_t height = video_shm->height;
    if (width == 0 || height == 0) return;

    // Get write buffer pointer (plain read - we own the write buffer)
    uint32_t write_idx = video_shm->write_index;
    uint8_t* dst_frame = macemu_get_frame_ptr(video_shm, write_idx);
    int dst_stride = macemu_get_bgra_stride();

    // Always output BGRA
    video_shm->pixel_format = MACEMU_PIXFMT_BGRA;

    switch (frame_depth) {
        case 32: {
            // Mac 32-bit: bytes A,R,G,B in memory (libyuv calls this "BGRA")
            // Convert to bytes B,G,R,A (libyuv calls this "ARGB")
            // libyuv::ARGBToBGRA shuffles BGRA→ARGB (or equivalently ARGB→BGRA)
            // It's a symmetric shuffle: ABGR↔RGBA, so ARGBToBGRA works both ways
            libyuv::ARGBToBGRA(
                the_buffer, frame_bytes_per_row,  // Source: A,R,G,B bytes (Mac native)
                dst_frame, dst_stride,             // Dest: B,G,R,A bytes
                width, height
            );
            break;
        }
        case 16: {
            // Mac big-endian RGB555 - convert to BGRA (B,G,R,A bytes)
            const uint8_t* src = the_buffer;
            for (uint32_t row = 0; row < height; row++) {
                const uint16_t* src_row = (const uint16_t*)(src + row * frame_bytes_per_row);
                uint8_t* dst_row = dst_frame + row * dst_stride;
                for (uint32_t x = 0; x < width; x++) {
                    // Mac big-endian RGB555: 0RRRRRGGGGGBBBBB
                    uint16_t pixel = src_row[x];
                    // Swap bytes if little-endian host
                    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                    pixel = (pixel >> 8) | (pixel << 8);
                    #endif
                    uint8_t r = ((pixel >> 10) & 0x1F) << 3;
                    uint8_t g = ((pixel >> 5) & 0x1F) << 3;
                    uint8_t b = (pixel & 0x1F) << 3;
                    // BGRA byte order (B,G,R,A)
                    dst_row[x * 4 + 0] = b;
                    dst_row[x * 4 + 1] = g;
                    dst_row[x * 4 + 2] = r;
                    dst_row[x * 4 + 3] = 255;
                }
            }
            break;
        }
        case 8:
        case 4:
        case 2:
        case 1: {
            // Indexed color modes - expand to BGRA using palette
            const uint8_t* src = the_buffer;
            for (uint32_t row = 0; row < height; row++) {
                const uint8_t* src_row = src + row * frame_bytes_per_row;
                uint8_t* dst_row = dst_frame + row * dst_stride;

                for (uint32_t x = 0; x < width; x++) {
                    uint8_t index = 0;

                    switch (frame_depth) {
                        case 8:
                            index = src_row[x];
                            break;
                        case 4:
                            index = (src_row[x / 2] >> (4 - (x % 2) * 4)) & 0x0F;
                            break;
                        case 2:
                            index = (src_row[x / 4] >> (6 - (x % 4) * 2)) & 0x03;
                            break;
                        case 1:
                            index = (src_row[x / 8] >> (7 - (x % 8))) & 0x01;
                            break;
                    }

                    // Look up RGB from palette
                    uint8_t r = current_palette[index * 3 + 0];
                    uint8_t g = current_palette[index * 3 + 1];
                    uint8_t b = current_palette[index * 3 + 2];

                    // BGRA byte order (B,G,R,A)
                    dst_row[x * 4 + 0] = b;
                    dst_row[x * 4 + 1] = g;
                    dst_row[x * 4 + 2] = r;
                    dst_row[x * 4 + 3] = 255;
                }
            }
            break;
        }
        default:
            // Unknown format - fill with gray
            for (uint32_t row = 0; row < height; row++) {
                uint8_t* dst_row = dst_frame + row * dst_stride;
                for (uint32_t x = 0; x < width; x++) {
                    dst_row[x * 4 + 0] = 128;  // B
                    dst_row[x * 4 + 1] = 128;  // G
                    dst_row[x * 4 + 2] = 128;  // R
                    dst_row[x * 4 + 3] = 255;  // A
                }
            }
            break;
    }

    // Compute dirty rectangle by comparing with previous frame (triple buffering)
    // write_idx (already loaded above) = buffer we just wrote to
    // ready_index = buffer server is reading (frame N-1)
    // prev_index  = the third buffer (frame N-2), our comparison baseline
    uint32_t ready_idx = video_shm->ready_index;  // Plain read - we don't race with server

    // On first few frames, indices might be the same - always send full frame
    // Once triple buffering is active, all 3 indices will be different
    bool can_compare = (write_idx != ready_idx);
    uint32_t prev_idx = 0;
    if (can_compare) {
        prev_idx = 3 - write_idx - ready_idx;  // The third buffer (0+1+2=3)
        // Validate prev_idx is in bounds
        if (prev_idx >= MACEMU_NUM_BUFFERS) {
            can_compare = false;  // Safety check
        }
    }

    uint8_t* curr_frame = dst_frame;  // Frame we just converted
    uint8_t* prev_frame = can_compare ? macemu_get_frame_ptr(video_shm, prev_idx) : nullptr;

    // Find bounding box of changed pixels
    // Check every pixel for accuracy (still fast - uses uint32 comparison)
    uint32_t min_x = width, max_x = 0;
    uint32_t min_y = height, max_y = 0;
    bool found_change = false;

    // Only compare if we have a valid previous frame
    if (prev_frame) {
        // Check every row from top and bottom to find vertical bounds quickly
        for (uint32_t y = 0; y < height; y++) {
            const uint32_t* curr_row = (const uint32_t*)(curr_frame + y * dst_stride);
            const uint32_t* prev_row = (const uint32_t*)(prev_frame + y * dst_stride);

            // Check if this row has any changes
            bool row_changed = false;
            for (uint32_t x = 0; x < width; x++) {
                if (curr_row[x] != prev_row[x]) {
                    row_changed = true;
                    if (!found_change) {
                        found_change = true;
                        min_y = y;
                    }
                    max_y = y;
                    if (x < min_x) min_x = x;
                    if (x > max_x) max_x = x;
                }
            }
        }
    }

    // Store dirty rect in SHM for server to read
    // Plain writes - synchronized by eventfd write in macemu_frame_complete()
    if (!prev_frame) {
        // First frame or can't compare - send full frame
        video_shm->dirty_x = 0;
        video_shm->dirty_y = 0;
        video_shm->dirty_width = width;
        video_shm->dirty_height = height;
    } else if (!found_change) {
        // No changes - signal this with width=0
        video_shm->dirty_x = 0;
        video_shm->dirty_y = 0;
        video_shm->dirty_width = 0;
        video_shm->dirty_height = 0;
    } else {
        // Add small 1-pixel margin for safety (PNG filtering artifacts)
        uint32_t dirty_x = (min_x > 1) ? min_x - 1 : 0;
        uint32_t dirty_y = (min_y > 1) ? min_y - 1 : 0;
        uint32_t dirty_w = (max_x < width - 2) ? (max_x - dirty_x + 2) : (width - dirty_x);
        uint32_t dirty_h = (max_y < height - 2) ? (max_y - dirty_y + 2) : (height - dirty_y);

        // If dirty rect is >75% of screen, just use full frame
        uint32_t dirty_pixels = dirty_w * dirty_h;
        uint32_t total_pixels = width * height;
        if (dirty_pixels > (total_pixels * 3 / 4)) {
            dirty_x = 0;
            dirty_y = 0;
            dirty_w = width;
            dirty_h = height;
        }

        video_shm->dirty_x = dirty_x;
        video_shm->dirty_y = dirty_y;
        video_shm->dirty_width = dirty_w;
        video_shm->dirty_height = dirty_h;
    }

    // Update cursor position for browser rendering
    int cursor_x = 0, cursor_y = 0;
    ADBGetMousePos(&cursor_x, &cursor_y);
    video_shm->cursor_x = (uint16_t)cursor_x;
    video_shm->cursor_y = (uint16_t)cursor_y;
    video_shm->cursor_visible = 1;  // Always visible for now (could check Mac cursor state later)

    // Get timestamp and signal frame complete
    // Use CLOCK_REALTIME for Unix epoch timestamp (needed for browser sync)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t timestamp_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    macemu_frame_complete(video_shm, timestamp_us);

    // Check if we need to update ping t4 timestamp
    update_ping_on_frame_complete(timestamp_us);
}


/*
 *  Video refresh thread - converts frames and handles mouse input
 */

static void video_refresh_thread()
{
    auto last_frame_time = std::chrono::steady_clock::now();
    auto last_stats_time = std::chrono::steady_clock::now();
    int target_fps = 60;  // Increased from 30 to 60 FPS for lower latency
    int frames_sent = 0;

    // Dirty rect statistics
    uint64_t total_full_pixels = 0;   // Total pixels if all frames were full
    uint64_t total_dirty_pixels = 0;  // Actual pixels sent via dirty rects
    int dirty_rect_frames = 0;
    int full_frames = 0;
    int skipped_frames = 0;

    while (video_thread_running) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame_time);

        // Print stats every 3 seconds
        auto stats_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats_time);
        if (stats_elapsed.count() >= 3000) {
            float fps = frames_sent * 1000.0f / stats_elapsed.count();

            // Calculate average mouse latency
            int lat_samples = mouse_latency_samples.exchange(0);
            uint64_t lat_total = mouse_latency_total_ms.exchange(0);
            float avg_mouse_ms = lat_samples > 0 ? (float)lat_total / lat_samples : 0;

            // Write latency stats to SHM for server to read
            // Store as x10 for 0.1ms precision (e.g., 125 = 12.5ms)
            if (video_shm) {
                uint32_t latency_x10 = (uint32_t)(avg_mouse_ms * 10);
                ATOMIC_STORE(video_shm->mouse_latency_avg_ms, latency_x10);
                ATOMIC_STORE(video_shm->mouse_latency_samples, (uint32_t)lat_samples);
            }

            // Calculate bandwidth savings from dirty rects
            float bandwidth_saved_pct = 0.0f;
            if (total_full_pixels > 0) {
                bandwidth_saved_pct = 100.0f * (1.0f - (float)total_dirty_pixels / total_full_pixels);
            }

            if (g_debug_perf) {
                int sock = ControlIPCGetSocket();
                fprintf(stderr, "[Emulator] fps=%.1f frames=%d | mouse: latency=%.1fms | dirty: rects=%d full=%d skip=%d saved=%.0f%% | server=%s\n",
                        fps, frames_sent,
                        avg_mouse_ms,
                        dirty_rect_frames, full_frames, skipped_frames, bandwidth_saved_pct,
                        sock >= 0 ? "connected" : "waiting");
            }

            frames_sent = 0;
            dirty_rect_frames = 0;
            full_frames = 0;
            skipped_frames = 0;
            total_full_pixels = 0;
            total_dirty_pixels = 0;
            last_stats_time = now;
        }

        // Rate limit to target FPS
        int frame_interval = 1000 / target_fps;
        if (elapsed.count() < frame_interval) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        last_frame_time = now;

        // Convert frame to BGRA and signal complete
        convert_frame_to_bgra();
        frames_sent++;

        // Track dirty rect statistics
        if (video_shm) {
            uint32_t dirty_w = video_shm->dirty_width;  // Plain read for stats
            uint32_t dirty_h = video_shm->dirty_height;
            uint32_t width = video_shm->width;
            uint32_t height = video_shm->height;

            uint32_t full_frame_pixels = width * height;
            total_full_pixels += full_frame_pixels;

            if (dirty_w == 0) {
                // No changes
                skipped_frames++;
            } else if (dirty_w == width && dirty_h == height) {
                // Full frame
                full_frames++;
                total_dirty_pixels += full_frame_pixels;
            } else {
                // Dirty rect
                dirty_rect_frames++;
                total_dirty_pixels += (dirty_w * dirty_h);
            }
        }
    }
}


/*
 *  IPC monitor descriptor
 */

class IPC_monitor_desc : public monitor_desc {
public:
    IPC_monitor_desc(const vector<video_mode> &available_modes,
                     video_depth default_depth, uint32 default_id)
        : monitor_desc(available_modes, default_depth, default_id) {}
    ~IPC_monitor_desc() {}

    virtual void switch_to_current_mode(void);
    virtual void set_palette(uint8 *pal, int num);
    virtual void set_gamma(uint8 *gamma, int num);

    void video_close(void);
};

static IPC_monitor_desc *the_monitor = NULL;


void IPC_monitor_desc::switch_to_current_mode(void)
{
    const video_mode &mode = get_current_mode();

    // Update internal tracking
    frame_width = mode.x;
    frame_height = mode.y;
    frame_bytes_per_row = mode.bytes_per_row;

    switch (mode.depth) {
        case VDEPTH_1BIT:  frame_depth = 1; break;
        case VDEPTH_2BIT:  frame_depth = 2; break;
        case VDEPTH_4BIT:  frame_depth = 4; break;
        case VDEPTH_8BIT:  frame_depth = 8; break;
        case VDEPTH_16BIT: frame_depth = 16; break;
        case VDEPTH_32BIT: frame_depth = 32; break;
        default:           frame_depth = 8; break;
    }

    // Update SHM dimensions
    if (video_shm) {
        video_shm->width = frame_width;
        video_shm->height = frame_height;
    }

    // Update Mac's view of the buffer
    uint32 mac_addr = (uint32)Host2MacAddr(the_buffer);
    set_mac_frame_base(mac_addr);

    MacScreenWidth = mode.x;
    MacScreenHeight = mode.y;

    if (g_debug_mode_switch) {
        fprintf(stderr, "IPC: Switched to mode %dx%d @ %d bpp (bytes_per_row=%d)\n",
                mode.x, mode.y, frame_depth, frame_bytes_per_row);
    }
}


void IPC_monitor_desc::set_palette(uint8 *pal, int num)
{
    // Store palette for indexed color conversion
    for (int i = 0; i < num && i < 256; i++) {
        current_palette[i * 3 + 0] = pal[i * 3 + 0];
        current_palette[i * 3 + 1] = pal[i * 3 + 1];
        current_palette[i * 3 + 2] = pal[i * 3 + 2];
    }

    if (g_debug_mode_switch) {
        fprintf(stderr, "IPC: set_palette(%d entries) [0]=(%d,%d,%d) [1]=(%d,%d,%d)\n",
                num,
                current_palette[0], current_palette[1], current_palette[2],
                current_palette[3], current_palette[4], current_palette[5]);
    }
}


void IPC_monitor_desc::set_gamma(uint8 *gamma, int num)
{
    // Not implemented
}


void IPC_monitor_desc::video_close(void)
{
    video_thread_running = false;
    if (video_thread.joinable()) {
        video_thread.join();
    }
}


/*
 *  Initialization
 */

static void add_mode(uint32 width, uint32 height, uint32 resolution_id,
                     uint32 bytes_per_row, video_depth depth)
{
    video_mode mode;
    mode.x = width;
    mode.y = height;
    mode.resolution_id = resolution_id;
    mode.bytes_per_row = bytes_per_row;
    mode.depth = depth;
    VideoModes.push_back(mode);
}

static void add_standard_modes(uint32 width, uint32 height, uint32 resolution_id)
{
    add_mode(width, height, resolution_id,
             TrivialBytesPerRow(width, VDEPTH_1BIT), VDEPTH_1BIT);
    add_mode(width, height, resolution_id,
             TrivialBytesPerRow(width, VDEPTH_2BIT), VDEPTH_2BIT);
    add_mode(width, height, resolution_id,
             TrivialBytesPerRow(width, VDEPTH_4BIT), VDEPTH_4BIT);
    add_mode(width, height, resolution_id,
             TrivialBytesPerRow(width, VDEPTH_8BIT), VDEPTH_8BIT);
    add_mode(width, height, resolution_id,
             TrivialBytesPerRow(width, VDEPTH_16BIT), VDEPTH_16BIT);
    add_mode(width, height, resolution_id,
             TrivialBytesPerRow(width, VDEPTH_32BIT), VDEPTH_32BIT);
}


#ifndef USE_SDL_VIDEO

#ifdef SHEEPSHAVER
bool VideoInit(void)
{
    const bool classic = false;
#else
bool VideoInit(bool classic)
{
#endif
    return IPC_VideoInit(classic);
}

#endif // !USE_SDL_VIDEO


static bool IPC_VideoInit(bool classic)
{
    classic_mode = classic;

    frame_skip = PrefsFindInt32("frameskip");

    // Read debug flags FIRST so we can use them during initialization
    g_debug_perf = (getenv("MACEMU_DEBUG_PERF") != nullptr);
    g_debug_mode_switch = (getenv("MACEMU_DEBUG_MODE_SWITCH") != nullptr);
    g_debug_mouse = (getenv("MACEMU_DEBUG_MOUSE") != nullptr);

    // Get screen mode from preferences
    int default_width = 640;
    int default_height = 480;
    const char *mode_str = PrefsFindString("screen");

    if (g_debug_mode_switch) {
        fprintf(stderr, "[MODE] Parsing screen preference: '%s'\n", mode_str ? mode_str : "(null)");
    }

    if (mode_str) {
        // Try different format strings (both / and x as separator)
        if (sscanf(mode_str, "ipc/%dx%d", &default_width, &default_height) != 2) {
            if (sscanf(mode_str, "ipc/%d/%d", &default_width, &default_height) != 2) {
                if (sscanf(mode_str, "win/%dx%d", &default_width, &default_height) != 2) {
                    if (sscanf(mode_str, "win/%d/%d", &default_width, &default_height) != 2) {
                        if (sscanf(mode_str, "dga/%dx%d", &default_width, &default_height) != 2) {
                            if (sscanf(mode_str, "dga/%d/%d", &default_width, &default_height) != 2) {
                                if (sscanf(mode_str, "%dx%d", &default_width, &default_height) != 2) {
                                    sscanf(mode_str, "%d/%d", &default_width, &default_height);
                                }
                            }
                        }
                    }
                }
            }
        }
        if (g_debug_mode_switch) {
            fprintf(stderr, "[MODE] Parsed resolution from prefs: %dx%d\n", default_width, default_height);
        }
    }

    // Clamp to supported range
    int requested_width = default_width;
    int requested_height = default_height;
    if (default_width < 512) default_width = 512;
    if (default_width > MACEMU_MAX_WIDTH) default_width = MACEMU_MAX_WIDTH;
    if (default_height < 384) default_height = 384;
    if (default_height > MACEMU_MAX_HEIGHT) default_height = MACEMU_MAX_HEIGHT;

    if (g_debug_mode_switch && (requested_width != default_width || requested_height != default_height)) {
        fprintf(stderr, "[MODE] Resolution clamped from %dx%d to %dx%d (limits: %d-%d x %d-%d)\n",
                requested_width, requested_height, default_width, default_height,
                512, MACEMU_MAX_WIDTH, 384, MACEMU_MAX_HEIGHT);
    }

    if (classic) {
        default_width = 512;
        default_height = 342;
    }

    frame_width = default_width;
    frame_height = default_height;
    frame_depth = 32;
    frame_bytes_per_row = TrivialBytesPerRow(frame_width, VDEPTH_32BIT);

    fprintf(stderr, "IPC: Initializing video driver (v3, emulator-owned resources)\n");

    // Create IPC resources (emulator owns these)
    if (!create_video_shm()) {
        return false;
    }

    if (!ControlIPCInit(video_shm)) {
        destroy_video_shm();
        return false;
    }

    // Allocate local frame buffer for Mac to write to
    // Mac writes here, then we convert to I420 in the SHM
    the_buffer_size = TrivialBytesPerRow(MACEMU_MAX_WIDTH, VDEPTH_32BIT) * MACEMU_MAX_HEIGHT;
    the_buffer = (uint8*)vm_acquire(the_buffer_size);
    if (!the_buffer) {
        fprintf(stderr, "IPC: Failed to allocate frame buffer\n");
        ControlIPCExit();
        destroy_video_shm();
        return false;
    }
    memset(the_buffer, 0, the_buffer_size);

    // Initialize palette to grayscale
    for (int i = 0; i < 256; i++) {
        current_palette[i * 3 + 0] = 255 - i;
        current_palette[i * 3 + 1] = 255 - i;
        current_palette[i * 3 + 2] = 255 - i;
    }

    // Build list of video modes
    VideoModes.clear();

    if (classic) {
        add_mode(512, 342, 0x80, 64, VDEPTH_1BIT);
    } else {
        add_standard_modes(default_width, default_height, 0x80);
    }

    // Create monitor descriptor
    video_depth vdepth = VDEPTH_32BIT;
    switch (frame_depth) {
        case 1:  vdepth = VDEPTH_1BIT; break;
        case 2:  vdepth = VDEPTH_2BIT; break;
        case 4:  vdepth = VDEPTH_4BIT; break;
        case 8:  vdepth = VDEPTH_8BIT; break;
        case 16: vdepth = VDEPTH_16BIT; break;
        case 32: vdepth = VDEPTH_32BIT; break;
    }

    the_monitor = new IPC_monitor_desc(VideoModes, vdepth, 0x80);
    VideoMonitors.push_back(the_monitor);

    uint32 mac_frame_base = (uint32)Host2MacAddr(the_buffer);
    the_monitor->set_mac_frame_base(mac_frame_base);

    MacScreenWidth = default_width;
    MacScreenHeight = default_height;

    // Set emulator state to running
    if (video_shm) {
        video_shm->state = MACEMU_STATE_RUNNING;
    }

    // Start video refresh thread (frame conversion)
    video_thread_running = true;
    video_thread = std::thread(video_refresh_thread);

    // Start control socket thread (input handling - zero-latency epoll)
    ControlIPCStart();

    fprintf(stderr, "IPC: Video initialized (%dx%d @ %d bpp)\n",
            default_width, default_height, frame_depth);
    fprintf(stderr, "IPC: Waiting for server to connect (PID %d)...\n", getpid());

    return true;
}


/*
 *  Deinitialization
 */

#ifndef USE_SDL_VIDEO
void VideoExit(void)
{
    IPC_VideoExit();
}
#endif

static void IPC_VideoExit(void)
{
    // Stop threads
    video_thread_running = false;

    if (video_thread.joinable()) {
        video_thread.join();
    }

    // Stop control socket thread
    ControlIPCExit();

    if (the_monitor) {
        the_monitor->video_close();
    }

    // Free local frame buffer
    if (the_buffer) {
        vm_release(the_buffer, the_buffer_size);
        the_buffer = NULL;
    }
    the_buffer_size = 0;

    // Clean up IPC resources
    destroy_video_shm();

    VideoModes.clear();

    fprintf(stderr, "IPC: Video shutdown complete\n");
}


static void IPC_VideoQuitFullScreen(void)
{
    // Nothing to do
}

#ifndef USE_SDL_VIDEO
void VideoQuitFullScreen(void)
{
    IPC_VideoQuitFullScreen();
}
#endif


static void IPC_VideoInterrupt(void)
{
    // Nothing to do - handled by video thread
}

#ifndef USE_SDL_VIDEO
void VideoInterrupt(void)
{
    IPC_VideoInterrupt();
}
#endif


static void IPC_VideoRefresh(void)
{
    // Nothing to do - handled by video thread
}

#ifndef USE_SDL_VIDEO
void VideoRefresh(void)
{
    IPC_VideoRefresh();
}
#endif


/*
 *  Get video SHM pointer (for audio_ipc.cpp)
 */

MacEmuIPCBuffer* IPC_GetVideoSHM(void)
{
    return video_shm;
}


#endif // ENABLE_IPC_VIDEO

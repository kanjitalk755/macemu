/*
 * WebRTC Server Module
 *
 * Complete WebRTC server with signaling, peer management, and media sending
 * Extracted from server.cpp for better modularity
 *
 * TODO: Future refactoring could split this into:
 * - Signaling server (WebSocket/SDP/ICE)
 * - Peer manager (connection lifecycle)
 * - Media sender (frame/audio distribution)
 */

#ifndef WEBRTC_SERVER_H
#define WEBRTC_SERVER_H

#include "../codec.h"
#include <rtc/rtc.hpp>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace webrtc {

// Forward declaration
struct PeerConnection;

/**
 * Peer Connection Wrapper
 */
struct PeerConnection {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::Track> audio_track;
    std::shared_ptr<rtc::DataChannel> data_channel;
    std::string id;
    CodecType codec = CodecType::H264;
    bool ready = false;
    bool needs_first_frame = true;  // PNG/RAW peers need full first frame
    bool has_remote_description = false;
    std::vector<std::pair<std::string, std::string>> pending_candidates;  // candidate, mid
};

/**
 * WebRTC Server
 *
 * Manages WebRTC signaling and peer connections for streaming video/audio to browsers
 */
class Server {
public:
    // Callback types for input handling (keyboard, mouse, commands)
    using KeyInputCallback = std::function<bool(int, bool)>;
    using MouseInputCallback = std::function<bool(int, int, uint8_t, uint64_t)>;
    using PingInputCallback = std::function<bool(uint32_t, uint64_t)>;
    using CommandCallback = std::function<void(uint8_t)>;

    Server();
    ~Server();

    // Initialize WebRTC server
    bool init(int signaling_port);

    // Shutdown server
    void shutdown();

    // Send media to all connected peers
    void send_h264_frame(const std::vector<uint8_t>& data, bool is_keyframe);
    void send_av1_frame(const std::vector<uint8_t>& data, bool is_keyframe);
    void send_png_frame(const std::vector<uint8_t>& data, uint64_t t1_frame_ready_ms,
                        uint32_t ping_seq, uint64_t t4_echo_ms);
    void send_audio_to_all_peers(const std::vector<uint8_t>& opus_data);

    // Get peer count
    int get_peer_count() const { return peer_count_; }

    // Set input callbacks (called when browser sends input via DataChannel)
    void set_key_input_callback(KeyInputCallback cb) { key_input_cb_ = cb; }
    void set_mouse_input_callback(MouseInputCallback cb) { mouse_input_cb_ = cb; }
    void set_ping_input_callback(PingInputCallback cb) { ping_input_cb_ = cb; }
    void set_command_callback(CommandCallback cb) { command_cb_ = cb; }

    // Configuration setters (must be called before init())
    void set_stun_config(bool enable, const std::string& server);
    void set_codec(CodecType codec) { default_codec_ = codec; }

private:
    void process_signaling(std::shared_ptr<rtc::WebSocket> ws, const std::string& msg);
    void handle_text_message(std::shared_ptr<PeerConnection> peer, const std::string& message);

    std::atomic<bool> initialized_;
    std::atomic<int> peer_count_;

    int port_;
    std::unique_ptr<rtc::WebSocketServer> ws_server_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point audio_start_time_;

    std::mutex peers_mutex_;
    std::map<std::string, std::shared_ptr<PeerConnection>> peers_;
    std::map<rtc::WebSocket*, std::string> ws_to_peer_id_;

    uint32_t ssrc_;
    CodecType default_codec_;
    bool enable_stun_;
    std::string stun_server_;

    // Input callbacks
    KeyInputCallback key_input_cb_;
    MouseInputCallback mouse_input_cb_;
    PingInputCallback ping_input_cb_;
    CommandCallback command_cb_;
};

} // namespace webrtc

#endif // WEBRTC_SERVER_H

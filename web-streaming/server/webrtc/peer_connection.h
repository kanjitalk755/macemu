/*
 * WebRTC Peer Connection Module
 *
 * Manages individual WebRTC peer connections with video, audio, and data channels
 */

#ifndef PEER_CONNECTION_H
#define PEER_CONNECTION_H

#include "../codec.h"
#include <rtc/rtc.hpp>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace webrtc {

/**
 * Peer Connection Wrapper
 *
 * Encapsulates a single WebRTC peer with its tracks and state
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
 * Peer Connection Factory
 *
 * Creates and configures WebRTC peer connections
 */
class PeerFactory {
public:
    // Input handler callback: receives binary message from data channel
    using InputHandler = std::function<void(const std::byte*, size_t)>;

    PeerFactory(bool enable_stun, const std::string& stun_server);

    // Create a new peer connection with the given ID and codec
    std::shared_ptr<PeerConnection> create_peer(
        const std::string& peer_id,
        CodecType codec,
        InputHandler input_handler
    );

private:
    void setup_data_channel(
        std::shared_ptr<PeerConnection> peer,
        InputHandler input_handler
    );

    bool enable_stun_;
    std::string stun_server_;
};

} // namespace webrtc

#endif // PEER_CONNECTION_H

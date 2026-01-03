# Zero-Config Networking for BasiliskII

## Problem Statement

Current TUN/TAP networking requires too many manual steps:
1. Create TAP device manually
2. Write/find a config script
3. Configure bridging to physical interface
4. Set up IP addressing
5. Configure each emulator instance

**Goal:** Make networking "just work" with zero configuration, supporting both:
- **Peer-to-peer** between BasiliskII instances (anywhere on internet)
- **LAN access** to real network devices (printers, file servers, etc.)

## Current State Analysis

### Existing Network Backends in BasiliskII

| Backend | Root? | Use Case | Issues |
|---------|-------|----------|--------|
| `NET_IF_TUNTAP` | Yes | Full L2 access | **Manual setup required** ⚠️ |
| UDP Tunnel | No | P2P over internet | Manual peer config, no LAN access |
| `NET_IF_SLIRP` | No | Internet only | NAT kills P2P, no AppleTalk |
| `NET_IF_VDE` | No | Local P2P | Requires vde_switch daemon |
| `NET_IF_SHEEPNET` | Yes | Legacy Linux | Kernel module required |

**Key Files:**
- [BasiliskII/src/ether.cpp](../BasiliskII/src/ether.cpp) - Platform-agnostic driver
- [BasiliskII/src/Unix/ether_unix.cpp](../BasiliskII/src/Unix/ether_unix.cpp) - Unix backends
- [BasiliskII/src/ether.cpp:93-156](../BasiliskII/src/ether.cpp#L93-L156) - UDP tunnel implementation

## Proposed Solution: Hybrid Auto-Config

Combine the best of both worlds:

```
┌─────────────────────────────────────────────────────────┐
│  BasiliskII Auto-Network (ether auto)                   │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  ┌──────────────┐         ┌──────────────────────────┐ │
│  │   TAP Mode   │         │   UDP Tunnel Mode        │ │
│  │   (Layer 2)  │         │   (Layer 3 via sockets)  │ │
│  └──────┬───────┘         └───────┬──────────────────┘ │
│         │                         │                     │
│         │ If root/CAP_NET_ADMIN   │ If no privileges    │
│         ▼                         ▼                     │
│  ┌─────────────┐           ┌──────────────┐            │
│  │ Auto-Bridge │           │ P2P Only     │            │
│  │ to LAN      │           │ (mDNS disco) │            │
│  └─────────────┘           └──────────────┘            │
│         │                         │                     │
│         ├─────────────────────────┤                     │
│         │  mDNS Discovery Layer   │                     │
│         │  (announce + browse)    │                     │
│         └─────────────────────────┘                     │
└─────────────────────────────────────────────────────────┘
```

### Three Operating Modes

#### Mode 1: Full Bridge (Best Experience)
**Requirements:** Root or `CAP_NET_ADMIN`

**Auto-setup steps:**
1. Create TAP device (`tap0`, `tap1`, etc.)
2. Generate unique MAC: `B2:XX:XX:XX:XX:XX` (locally administered)
   - `XX` based on hostname hash + PID for uniqueness
3. Auto-detect physical interface (`eth0`, `enp0s3`, `wlan0`, etc.)
4. Create bridge `br-basilisk`
5. Add physical interface to bridge
6. Add TAP to bridge
7. Bring everything up

**Result:**
- ✅ Full LAN access (DHCP, printers, file servers)
- ✅ P2P with other BasiliskII instances (via mDNS)
- ✅ AppleTalk to real Macs on network
- ✅ Internet access

**Cleanup on exit:**
- Remove TAP from bridge
- TAP device destroyed when FD closes
- Bridge left running (shared by multiple instances)

#### Mode 2: P2P Only (Fallback)
**Requirements:** None (unprivileged)

**Auto-setup steps:**
1. Try TAP (will fail without root)
2. Fall back to UDP socket
3. Generate unique MAC from local IP
4. Enable broadcast socket option

**Result:**
- ✅ P2P with other BasiliskII instances (via mDNS)
- ✅ AppleTalk over UDP tunnel
- ❌ No LAN access
- ❌ No internet (unless combined with SLIRP - see Mode 3)

#### Mode 3: Hybrid Split (Advanced)
**For users wanting both P2P and internet without root:**

```
┌─────────────────┐
│  Mac OS Guest   │
├─────────────────┤
│  EtherTalk NIC  │ ──► UDP tunnel + mDNS (P2P AppleTalk)
│  TCP/IP Stack   │ ──► SLIRP (Internet access)
└─────────────────┘
```

**Implementation:** Dual network drivers (requires MacOS config)

## mDNS Discovery Protocol

### Service Advertisement

**Service Type:** `_basilisk-ether._udp.local`

**TXT Record:**
```
mac=b2aabbccddee      # Virtual MAC address (hex)
port=6066             # UDP port for tunneling
version=1.0           # Protocol version
mode=bridge|p2p       # Operating mode
hostname=mymac        # Friendly name
```

**Example Registration:**
```c
// Avahi/Bonjour service
Name: "BasiliskII on mymac"
Type: _basilisk-ether._udp
Port: 6066
TXT:  mac=b2001a2b3c4d&port=6066&mode=bridge&version=1.0
```

### Peer Discovery Flow

```
Instance A                              Instance B
    │                                       │
    ├─► Advertise: MAC=B2:00:1A:2B:3C:4D   │
    │                                       ◄─┐ Browse
    │                                       │ │ (finds Instance A)
    │   ┌───────────────────────────────────┘ │
    │   │ Add to peer table:                  │
    │   │ B2:00:1A:2B:3C:4D → 10.0.0.5:6066  │
    │   └───────────────────────────────────┐ │
    │                                       │ │
    │   ◄─── Advertise: MAC=B2:00:5E:6F:7A:8B
    │                                       │
    ├─┐ Browse                              │
    │ │ (finds Instance B)                  │
    │ └───────────────────────────────────┐ │
    │     Add to peer table:              │ │
    │     B2:00:5E:6F:7A:8B → 10.0.0.10:6066
    │                                     │ │
    │                                     └─┘
    ├──── Ethernet packet (dest=B2:00:5E...) ───►
    │        Lookup peer table                   │
    │        Send UDP to 10.0.0.10:6066          │
    │                                             │
    ◄──── Ethernet packet (dest=B2:00:1A...) ────┤
          Received via UDP                       │
```

### Peer Table Management

```c
struct ether_peer {
    uint8_t mac[6];           // Virtual MAC address
    uint32_t ip;              // IP address
    uint16_t port;            // UDP port
    time_t last_seen;         // For timeout
    char hostname[64];        // Friendly name
    enum { BRIDGE, P2P } mode; // Operating mode
};

std::map<std::string, ether_peer> peer_table;
```

**Timeout:** Remove peers not seen for 60 seconds (handle crashes/shutdowns)

## Implementation Plan

### Phase 1: Auto-Config TAP (Mode 1)

**File:** `BasiliskII/src/Unix/ether_auto_tuntap.cpp` (new)

**Functions:**
```c
// Create TAP and auto-configure
int auto_configure_tuntap(
    const char *tap_name,      // e.g., "tap0"
    unsigned char *mac_out,    // Returns generated MAC
    bool verbose               // Print status messages
);

// Returns:
//   0 = Success with bridge (full LAN access)
//   1 = Success without bridge (P2P only)
//  -1 = Failure

// Cleanup on exit
void auto_cleanup_tuntap(const char *tap_name);
```

**Implementation details:**
1. **TAP creation:** Already done by existing code
2. **MAC generation:**
   ```c
   mac[0] = 0xB2;  // Locally administered unicast
   mac[1] = 0x00;
   mac[2-3] = hash(hostname);  // Stable across reboots
   mac[4-5] = getpid();        // Unique per instance
   ```
3. **Interface detection:** Parse `/proc/net/dev`, skip virtual interfaces
4. **Bridging:** Use `ioctl()` + netlink or shell out to `ip` command
5. **Privilege detection:** Try bridge setup, fall back gracefully

**Integration point:** [ether_unix.cpp:434-464](../BasiliskII/src/Unix/ether_unix.cpp#L434-L464)

Replace manual script execution with:
```cpp
if (net_if_type == NET_IF_TUNTAP) {
    unsigned char mac[6];
    int result = auto_configure_tuntap(ifr.ifr_name, mac, true);

    if (result == 0) {
        printf("✓ Full network access enabled\n");
    } else if (result == 1) {
        printf("✓ P2P mode (run with sudo for LAN access)\n");
    } else {
        goto open_error;
    }

    memcpy(ether_addr, mac, 6);
}
```

### Phase 2: mDNS Discovery Layer

**File:** `BasiliskII/src/Unix/ether_mdns.cpp` (new)

**Library:** Avahi (Linux) or dns_sd (macOS/BSD)

**Functions:**
```c
// Initialize mDNS (advertise + browse)
int mdns_init(
    const uint8_t *mac,    // Our MAC address
    uint16_t port          // Our UDP port
);

// Lookup peer by MAC address
struct ether_peer* mdns_lookup_peer(const uint8_t *mac);

// Get all peers (for broadcast)
void mdns_get_all_peers(struct ether_peer **peers, size_t *count);

// Shutdown
void mdns_cleanup(void);
```

**Integration point:** [ether.cpp:332-339](../BasiliskII/src/ether.cpp#L332-L339)

Replace hardcoded IP-from-MAC with peer lookup:
```cpp
// Extract destination address
struct ether_peer *peer = NULL;
if (is_broadcast(packet)) {
    // Send to all discovered peers
    size_t count;
    struct ether_peer *peers;
    mdns_get_all_peers(&peers, &count);
    for (size_t i = 0; i < count; i++) {
        send_udp_packet(packet, len, &peers[i]);
    }
} else {
    // Lookup specific peer
    peer = mdns_lookup_peer(packet);  // dest MAC
    if (peer) {
        send_udp_packet(packet, len, peer);
    } else {
        // Unknown MAC - drop or try broadcast
        return eMultiErr;
    }
}
```

### Phase 3: Unified "Auto" Mode

**Configuration:**
```
ether auto              # NEW: Auto-detect best mode
ether auto-bridge       # Force bridge mode (fail if no root)
ether auto-p2p          # Force P2P mode (never use TAP)
```

**Decision tree:**
```cpp
if (mode == "auto") {
    if (can_create_tap()) {
        // Try Mode 1 (bridge)
        if (bridge_success)
            use_tap_with_bridge();
        else
            use_tap_without_bridge();  // P2P only
    } else {
        // Fall back to Mode 2 (UDP tunnel)
        use_udp_tunnel();
    }

    // Always enable mDNS for P2P
    mdns_init(ether_addr, udp_port);
}
```

## Migration Path

### Backward Compatibility

All existing configs continue to work:
```
ether tun              # Manual TUN/TAP (unchanged)
ether slirp            # SLIRP NAT (unchanged)
ether vde              # VDE (unchanged)
udptunnel true         # Manual UDP tunnel (unchanged)
```

### Recommended New Defaults

**For developers/power users (have root):**
```
ether auto-bridge
```
- Best experience
- Full LAN access
- Automatic P2P

**For regular users (no root):**
```
ether auto-p2p
```
- P2P networking works
- Internet via SLIRP if needed
- No privilege escalation

**Auto-detect (smart default):**
```
ether auto
```
- Tries bridge, falls back to P2P
- One config works everywhere

## Security Considerations

### Privilege Escalation

**Problem:** Bridging requires root

**Solutions:**
1. **Setuid helper binary** (like Docker)
   ```
   /usr/lib/basilisk/tap-helper  (setuid root)
   ```
   - Only performs TAP/bridge operations
   - Drops privileges immediately
   - Minimal attack surface

2. **Capabilities** (Linux)
   ```
   sudo setcap cap_net_admin+ep BasiliskII
   ```
   - No full root needed
   - Only network admin capability

3. **PolicyKit/sudo rules**
   ```
   # /etc/sudoers.d/basilisk
   user ALL=(root) NOPASSWD: /usr/lib/basilisk/tap-helper
   ```

4. **Graceful degradation** (recommended)
   - Try privileged operations
   - Fall back to unprivileged mode
   - Tell user how to enable full access

### Network Isolation

**mDNS filtering:**
- Only respond to `_basilisk-ether._udp` services
- Validate TXT records before adding peers
- Ignore malformed advertisements

**MAC address validation:**
- Only accept `B2:*` prefixes (our namespace)
- Prevents MAC spoofing attacks

**UDP packet validation:**
- Check Ethernet frame structure
- Drop malformed packets
- Rate limiting on peer discovery

## Testing Plan

### Test Scenarios

1. **Single instance, no root**
   - Should create UDP socket
   - Should advertise via mDNS
   - Should report "P2P mode"

2. **Single instance, with root**
   - Should create TAP
   - Should bridge to eth0
   - Should get DHCP from network
   - Should advertise via mDNS

3. **Two instances, same host, no root**
   - Should discover each other
   - Should communicate via UDP tunnel
   - AppleTalk should work

4. **Two instances, different hosts, bridged**
   - Should discover via mDNS
   - Should see each other on LAN
   - AppleTalk should work
   - Should access real Macs on network

5. **Mixed mode**
   - Instance A: bridged (with root)
   - Instance B: P2P only (no root)
   - Should still discover each other
   - Should tunnel via UDP

### Validation

```bash
# Check TAP created
ip link show tap0

# Check bridge membership
bridge link show

# Check mDNS advertisement
avahi-browse -a | grep basilisk

# Check peer discovery
# (add debug option to dump peer table)
BasiliskII --dump-peers

# Test AppleTalk connectivity
# (from Mac OS Chooser)
```

## Performance Considerations

### Latency
- **Bridge mode:** ~100µs overhead (negligible)
- **UDP tunnel:** Depends on network (~1-50ms typical)
- **mDNS discovery:** Only on peer join/leave, not per-packet

### Throughput
- **Bridge mode:** Wire speed (hardware limited)
- **UDP tunnel:** Network bandwidth limited
- **CPU overhead:** Minimal (async I/O threads)

### Memory
- **Peer table:** ~128 bytes per peer
- **mDNS:** ~100KB (Avahi library)
- **Packet buffers:** Already allocated (1516 bytes)

## Future Enhancements

### Phase 4: Cloud Relay (Optional)
For NAT traversal between instances on different networks:

```
Instance A (NAT)  ←──► Relay Server ←──► Instance B (NAT)
                       (STUN/TURN)
```

Uses existing UDP tunnel + mDNS, adds:
- WebRTC-style NAT traversal
- Fallback relay server
- Encrypted tunnels

### Phase 5: Performance Optimizations

1. **Zero-copy packet forwarding** (bridge mode)
2. **Packet batching** (UDP tunnel)
3. **Multicast optimization** (single packet for broadcasts)
4. **QoS prioritization** (AppleTalk control packets)

### Phase 6: GUI Integration

```
┌─────────────────────────────────────┐
│  BasiliskII Network Status          │
├─────────────────────────────────────┤
│  Mode: Bridged (br-basilisk)        │
│  MAC:  B2:00:1A:2B:3C:4D            │
│  IP:   192.168.1.100 (DHCP)         │
│                                     │
│  Discovered Peers:                  │
│  ☑ mymac2    (192.168.1.101)        │
│  ☑ laptop    (10.0.0.5)             │
│                                     │
│  [ Refresh ]  [ Advanced... ]       │
└─────────────────────────────────────┘
```

## References

### Code Locations
- **UDP Tunnel:** [ether.cpp:93-156](../BasiliskII/src/ether.cpp#L93-L156)
- **TUN/TAP Setup:** [ether_unix.cpp:434-464](../BasiliskII/src/Unix/ether_unix.cpp#L434-L464)
- **Packet TX:** [ether.cpp:313-362](../BasiliskII/src/ether.cpp#L313-L362)
- **Packet RX:** [ether.cpp:414-452](../BasiliskII/src/ether.cpp#L414-L452)

### External References
- [Linux Bridge Documentation](https://www.kernel.org/doc/Documentation/networking/bridge.txt)
- [TUN/TAP Driver](https://www.kernel.org/doc/Documentation/networking/tuntap.txt)
- [Avahi Developer Documentation](https://www.avahi.org/doxygen/html/)
- [AppleTalk Protocol](https://en.wikipedia.org/wiki/AppleTalk)
- [EtherTalk Specification](https://en.wikipedia.org/wiki/EtherTalk)

## Summary

**Zero-config networking is achievable** by combining:
1. **Auto-configured TUN/TAP** (when privileged)
2. **UDP tunnel fallback** (when unprivileged)
3. **mDNS discovery** (always enabled)

**Result:** User types `ether auto` and networking "just works" - both P2P and LAN access, with graceful degradation based on available privileges.

**Estimated effort:** ~1000 lines of code across 3 new files + modifications to existing ether_unix.cpp

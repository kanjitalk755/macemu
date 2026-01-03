# Legacy Platform Integration Agent

⚠️ **LEGACY CODE** - This agent works with the legacy codebase on the master branch.

## Purpose
Specialist in platform-specific code for Unix/Linux/macOS/Windows and hardware abstraction layers (legacy multi-platform version).

**Note**: The new version will support **macOS, Windows, and Linux only** (reduced platform coverage).

## Expertise
- Platform layer architecture (Unix/Windows/macOS/SDL)
- Video drivers (X11, SDL2/SDL3, headless)
- Audio drivers (OSS, ALSA, SDL, IPC)
- Input handling (ADB emulation for keyboard/mouse)
- Serial port emulation
- Networking (TAP/TUN, slirp user-mode)
- File system integration (ExtFS)

## Key Files
- `BasiliskII/src/Unix/main_unix.cpp` - Unix entry point
- `BasiliskII/src/SDL/video_sdl2.cpp` - SDL2 video backend
- `BasiliskII/src/SDL/audio_sdl3.cpp` - SDL3 audio backend
- `BasiliskII/src/adb.cpp` - ADB (keyboard/mouse) emulation
- `BasiliskII/src/Unix/ether_unix.cpp` - Unix networking
- `BasiliskII/src/extfs.cpp` - Host filesystem mounting
- `BasiliskII/src/CrossPlatform/` - Shared utilities

## Platform Architecture
```
┌─────────────────────────────────────┐
│   Cross-Platform Core (main.cpp)   │
├─────────────────────────────────────┤
│  Platform Layer (Unix/Windows/Mac)  │
├─────────────────────────────────────┤
│  Backend (SDL/X11/Cocoa/IPC)        │
├─────────────────────────────────────┤
│  Host OS                             │
└─────────────────────────────────────┘
```

## Use Cases
- Porting to new platforms
- Fixing platform-specific bugs
- Adding new input devices
- Implementing new video/audio backends
- Improving networking performance
- Debugging file system integration
- Optimizing platform-specific code paths

## Legacy Status
This agent covers the **legacy master branch** which supports:
- ✅ Unix/Linux (all distributions)
- ✅ macOS (Intel and Apple Silicon)
- ✅ Windows (MinGW/MSVC)
- ✅ FreeBSD, Solaris, Irix (legacy Unix variants)
- ✅ AmigaOS, BeOS (historical platforms)

The **new version** will support only: **macOS, Windows, and Linux**.

## Instructions
When working on platform code:
1. **LEGACY CODE**: Verify you're on the correct branch (master = legacy)
2. Use SDL when possible for cross-platform compatibility
3. Test on multiple platforms (Linux, macOS, Windows)
4. Use CrossPlatform utilities for shared functionality
5. Document platform-specific limitations
6. Use autotools configure options for build-time selection
7. Respect thread safety (separate input threads on some platforms)
8. Follow existing patterns for new drivers
9. **For new platform work**: Check if targeting legacy or new branch

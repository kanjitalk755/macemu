#!/bin/bash
# Quick script to build the Debian package

set -e

echo "=== macemu-webrtc Debian Package Builder ==="
echo ""

# Check if in correct directory
if [ ! -f "debian/control" ]; then
    echo "Error: Must run from macemu root directory (where debian/ folder exists)"
    exit 1
fi

# Check for packaging tools
if ! command -v dpkg-buildpackage &> /dev/null; then
    echo "Installing packaging tools..."
    sudo apt-get update
    sudo apt-get install -y devscripts debhelper build-essential
fi

# Check build dependencies
echo "Checking build dependencies..."
if ! dpkg-checkbuilddeps 2>/dev/null; then
    echo ""
    echo "Installing missing build dependencies..."
    sudo apt-get install -y \
        autoconf automake libtool cmake pkg-config git \
        libssl-dev \
        libopenh264-dev \
        libsvtav1-dev libsvtav1enc-dev \
        libvpx-dev \
        libwebp-dev \
        libopus-dev \
        libyuv-dev \
        libsdl2-dev \
        libgtk-3-dev
fi

echo ""
echo "Building package..."
echo ""

# Build package
dpkg-buildpackage -us -uc -b -j$(nproc)

echo ""
echo "=== Build Complete! ==="
echo ""
echo "Package created:"
ls -lh ../*.deb 2>/dev/null | grep macemu-webrtc || echo "No .deb file found!"
echo ""
echo "To install:"
echo "  sudo dpkg -i ../macemu-webrtc_*.deb"
echo "  sudo apt-get install -f"
echo ""
echo "After installation:"
echo "  1. Copy ROMs to: /var/lib/macemu-webrtc/storage/roms/"
echo "  2. Copy disk images to: /var/lib/macemu-webrtc/storage/images/"
echo "  3. Edit config: /var/lib/macemu-webrtc/macemu-config.json"
echo "  4. Start: sudo systemctl start macemu-webrtc"
echo "  5. Access: http://localhost:8000"
echo ""

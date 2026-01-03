#!/bin/bash
# Build script for macemu WebRTC stack
# Builds BasiliskII, SheepShaver, and the WebRTC server

set -e  # Exit on error

echo "======================================"
echo "Building macemu WebRTC Stack"
echo "======================================"
echo ""

# Get script directory (macemu root)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Step 1: Build BasiliskII
echo -e "${BLUE}[1/3] Building BasiliskII with IPC support...${NC}"
cd BasiliskII/src/Unix

if [ ! -f configure ]; then
    echo "Running autogen.sh..."
    ./autogen.sh
fi

./configure --enable-ipc-video --enable-ipc-audio --enable-jit
make -j$(nproc)

echo -e "${GREEN}✓ BasiliskII built: BasiliskII/src/Unix/BasiliskII${NC}"
echo ""

# Step 2: Build SheepShaver
cd "$SCRIPT_DIR"
echo -e "${BLUE}[2/3] Building SheepShaver with IPC support...${NC}"
cd SheepShaver/src/Unix

if [ ! -f configure ]; then
    echo "Running autogen.sh..."
    ./autogen.sh
fi

./configure --enable-ipc-video --enable-ipc-audio --enable-jit
make -j$(nproc)

echo -e "${GREEN}✓ SheepShaver built: SheepShaver/src/Unix/SheepShaver${NC}"
echo ""

# Step 3: Build WebRTC Server
cd "$SCRIPT_DIR"
echo -e "${BLUE}[3/3] Building WebRTC streaming server...${NC}"
cd web-streaming

if [ ! -f configure ]; then
    echo "Generating configure script with autoreconf..."
    autoreconf -fi
    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to generate configure script"
        exit 1
    fi
fi

./configure
make -j$(nproc)

echo -e "${GREEN}✓ Server built: web-streaming/build/macemu-webrtc${NC}"
echo ""

# Create bin directory with symlinks for local testing
echo -e "${BLUE}Creating bin/ directory for local testing...${NC}"
cd "$SCRIPT_DIR"
mkdir -p bin

ln -sf ../BasiliskII/src/Unix/BasiliskII bin/basiliskii-webrtc
ln -sf ../SheepShaver/src/Unix/SheepShaver bin/sheepshaver-webrtc
ln -sf ../web-streaming/build/macemu-webrtc bin/macemu-webrtc-server

echo -e "${GREEN}✓ Created symlinks in bin/:${NC}"
echo "  bin/basiliskii-webrtc -> BasiliskII/src/Unix/BasiliskII"
echo "  bin/sheepshaver-webrtc -> SheepShaver/src/Unix/SheepShaver"
echo "  bin/macemu-webrtc-server -> web-streaming/build/macemu-webrtc"
echo ""

# Create user directories
echo -e "${BLUE}Setting up ~/.macemu directories...${NC}"
mkdir -p ~/.macemu/storage/roms
mkdir -p ~/.macemu/storage/images
mkdir -p ~/.macemu/client
mkdir -p ~/.config/BasiliskII
mkdir -p ~/.config/SheepShaver

echo -e "${GREEN}✓ Created user directories:${NC}"
echo "  ~/.macemu/storage/roms"
echo "  ~/.macemu/storage/images"
echo "  ~/.macemu/client"
echo "  ~/.config/BasiliskII"
echo "  ~/.config/SheepShaver"
echo ""

# Copy client files
echo -e "${BLUE}Copying web client files...${NC}"
cp -r "$SCRIPT_DIR/web-streaming/client/"* ~/.macemu/client/
echo -e "${GREEN}✓ Web client copied to ~/.macemu/client/${NC}"
echo ""

# Copy example config if it doesn't exist
if [ ! -f ~/.macemu/macemu-config.json ]; then
    if [ -f "$SCRIPT_DIR/web-streaming/macemu-config.json" ]; then
        echo -e "${BLUE}Copying example config...${NC}"
        cp "$SCRIPT_DIR/web-streaming/macemu-config.json" ~/.macemu/
        echo -e "${GREEN}✓ Config copied to ~/.macemu/macemu-config.json${NC}"
    fi
fi
echo ""

echo "======================================"
echo -e "${GREEN}Build Complete!${NC}"
echo "======================================"
echo ""
echo "Next steps:"
echo "  1. Add ROM files to ~/.macemu/storage/roms/"
echo "  2. Add disk images to ~/.macemu/storage/images/"
echo "  3. Edit config: ~/.macemu/macemu-config.json"
echo ""
echo "To run locally (without installing to /usr/bin/):"
echo "  export PATH=\"$SCRIPT_DIR/bin:\$PATH\""
echo "  macemu-webrtc-server"
echo ""
echo "To install system-wide:"
echo "  sudo cp bin/basiliskii-webrtc /usr/bin/"
echo "  sudo cp bin/sheepshaver-webrtc /usr/bin/"
echo "  sudo cp bin/macemu-webrtc-server /usr/bin/"
echo ""

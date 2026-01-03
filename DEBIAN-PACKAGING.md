# Building Debian Package for macemu-webrtc

This guide explains how to build a `.deb` package for easy installation on Ubuntu/Debian systems.

## Prerequisites

### Install Packaging Tools

```bash
sudo apt-get update
sudo apt-get install -y devscripts debhelper build-essential
```

### Install Build Dependencies

```bash
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
```

## Building the Package

### Method 1: Using dpkg-buildpackage (Recommended)

```bash
# From the macemu root directory
cd /home/mick/macemu

# Build the package
dpkg-buildpackage -us -uc -b

# This will create the .deb file in the parent directory
ls -lh ../*.deb
```

Expected output:
- `macemu-webrtc_1.0.0-1_amd64.deb` - The installable package
- `macemu-webrtc_1.0.0-1_amd64.buildinfo` - Build information
- `macemu-webrtc_1.0.0-1_amd64.changes` - Changes file

### Method 2: Using debuild

```bash
cd /home/mick/macemu

# Build with debuild (includes lintian checks)
debuild -us -uc -b

# Package will be in parent directory
ls -lh ../*.deb
```

### Build Options

- `-us -uc` - Don't sign the package (for local use)
- `-b` - Binary-only build (no source package)
- `-j$(nproc)` - Parallel build (faster)
- `-d` - Skip dependency check (use with caution)

**Full parallel build:**
```bash
dpkg-buildpackage -us -uc -b -j$(nproc)
```

## Installing the Package

### Install Locally

```bash
# Install the .deb package
sudo dpkg -i ../macemu-webrtc_1.0.0-1_amd64.deb

# If there are dependency issues, fix them
sudo apt-get install -f
```

### Post-Installation Setup

```bash
# 1. Add ROM files
sudo cp /path/to/your/roms/*.ROM /var/lib/macemu-webrtc/storage/roms/

# 2. Add disk images
sudo cp /path/to/your/disks/*.dsk /var/lib/macemu-webrtc/storage/images/

# 3. Fix ownership
sudo chown -R macemu:macemu /var/lib/macemu-webrtc/storage

# 4. Edit configuration
sudo nano /var/lib/macemu-webrtc/macemu-config.json

# 5. Start the service
sudo systemctl start macemu-webrtc

# 6. Enable on boot
sudo systemctl enable macemu-webrtc

# 7. Check status
sudo systemctl status macemu-webrtc

# 8. Access the web UI
# Open browser to: http://localhost:8000
```

## Package Contents

The package installs to:

```
/usr/bin/
├── basiliskii-webrtc        # BasiliskII emulator
├── sheepshaver-webrtc       # SheepShaver emulator
└── macemu-webrtc-server     # WebRTC streaming server

/usr/share/macemu-webrtc/
└── client/                  # Web client files
    ├── index.html
    ├── client.js
    ├── styles.css
    └── *.svg

/etc/macemu-webrtc/
└── macemu-config.json       # Default configuration

/lib/systemd/system/
└── macemu-webrtc.service    # Systemd service

/var/lib/macemu-webrtc/      # Working directory (created on install)
├── bin/
│   ├── BasiliskII -> /usr/bin/basiliskii-webrtc
│   └── SheepShaver -> /usr/bin/sheepshaver-webrtc
├── storage/
│   ├── roms/                # Place ROM files here
│   └── images/              # Place disk images here
├── client/                  # Web client (copied from /usr/share)
└── macemu-config.json       # Active config (copied from /etc)

/usr/share/doc/macemu-webrtc/
└── README-WEBRTC.md         # Documentation
```

## Systemd Service

The package includes a systemd service that:
- Runs as dedicated `macemu` user
- Starts automatically on boot (if enabled)
- Restarts on failure
- Has security hardening enabled

**Service commands:**
```bash
# Start
sudo systemctl start macemu-webrtc

# Stop
sudo systemctl stop macemu-webrtc

# Restart
sudo systemctl restart macemu-webrtc

# Status
sudo systemctl status macemu-webrtc

# Logs
sudo journalctl -u macemu-webrtc -f

# Enable on boot
sudo systemctl enable macemu-webrtc

# Disable on boot
sudo systemctl disable macemu-webrtc
```

## Uninstalling

```bash
# Remove package (keep config and data)
sudo apt-get remove macemu-webrtc

# Purge package (remove everything including data)
sudo apt-get purge macemu-webrtc

# Note: Purge will delete /var/lib/macemu-webrtc including ROMs and disk images!
# Backup your data first:
sudo cp -r /var/lib/macemu-webrtc/storage ~/macemu-backup
```

## Troubleshooting Build Issues

### Missing Dependencies

```bash
# Install missing build dependencies
sudo apt-get build-dep macemu-webrtc

# Or manually check what's missing
dpkg-checkbuilddeps
```

### Clean Build

```bash
# Clean previous build artifacts
debian/rules clean
# or
debclean

# Then rebuild
dpkg-buildpackage -us -uc -b
```

### Lintian Warnings

```bash
# Check package quality
lintian ../macemu-webrtc_1.0.0-1_amd64.deb

# Ignore minor warnings, fix serious errors
```

### Build Logs

```bash
# Check build log for errors
less ../macemu-webrtc_1.0.0-1_amd64.build

# Or watch build in real-time
dpkg-buildpackage -us -uc -b 2>&1 | tee build.log
```

## Creating a Repository

To distribute your package, create a simple apt repository:

### Method 1: GitHub Releases

```bash
# 1. Upload .deb to GitHub releases
# 2. Users download and install:
wget https://github.com/user/repo/releases/download/v1.0.0/macemu-webrtc_1.0.0-1_amd64.deb
sudo dpkg -i macemu-webrtc_1.0.0-1_amd64.deb
sudo apt-get install -f
```

### Method 2: PPA (Personal Package Archive)

For Ubuntu, use Launchpad PPA:

```bash
# Sign the package
debuild -S

# Upload to Launchpad
dput ppa:your-name/macemu ../macemu-webrtc_1.0.0-1_source.changes

# Users add PPA:
sudo add-apt-repository ppa:your-name/macemu
sudo apt-get update
sudo apt-get install macemu-webrtc
```

### Method 3: Simple APT Repository

```bash
# Create repository directory
mkdir -p ~/apt-repo

# Copy .deb file
cp ../macemu-webrtc_1.0.0-1_amd64.deb ~/apt-repo/

# Generate Packages file
cd ~/apt-repo
dpkg-scanpackages . /dev/null | gzip -9c > Packages.gz

# Serve via HTTP (nginx/apache) or GitHub Pages
# Users add to sources.list:
# deb [trusted=yes] http://your-server/apt-repo ./
```

## Customizing the Package

### Change Version

Edit `debian/changelog`:
```bash
dch -v 1.0.1-1 "New upstream release"
```

### Update Maintainer Info

Edit `debian/control` and `debian/changelog`:
- Replace "Your Name <your.email@example.com>" with your details

### Add Dependencies

Edit `debian/control`:
- Add to `Build-Depends:` for build-time dependencies
- Add to `Depends:` for runtime dependencies

### Modify Install Paths

Edit `debian/rules` in the `override_dh_auto_install` section.

## Testing the Package

### Test Installation

```bash
# Install in a clean container
docker run -it ubuntu:22.04 bash

# Inside container:
apt-get update
apt-get install -y /path/to/macemu-webrtc_1.0.0-1_amd64.deb
systemctl status macemu-webrtc
```

### Test Upgrade

```bash
# Install old version
sudo dpkg -i macemu-webrtc_1.0.0-1_amd64.deb

# Install new version
sudo dpkg -i macemu-webrtc_1.0.1-1_amd64.deb

# Check config preserved
cat /var/lib/macemu-webrtc/macemu-config.json
```

### Test Removal

```bash
# Install
sudo dpkg -i macemu-webrtc_1.0.0-1_amd64.deb

# Remove
sudo apt-get remove macemu-webrtc

# Check service stopped
systemctl status macemu-webrtc

# Purge
sudo apt-get purge macemu-webrtc

# Check data removed
ls /var/lib/macemu-webrtc
```

## Advanced: Building for Multiple Architectures

```bash
# Build for arm64
dpkg-buildpackage -us -uc -b -a arm64

# Build for armhf
dpkg-buildpackage -us -uc -b -a armhf

# Cross-compile (requires cross-compilation toolchain)
dpkg-buildpackage -us -uc -b --host-arch=arm64
```

## CI/CD Integration

### GitHub Actions Example

Create `.github/workflows/build-deb.yml`:

```yaml
name: Build DEB Package

on:
  push:
    tags:
      - 'v*'

jobs:
  build:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v3

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y devscripts debhelper
          sudo apt-get install -y $(grep Build-Depends debian/control | cut -d: -f2 | tr ',' '\n')

      - name: Build package
        run: dpkg-buildpackage -us -uc -b

      - name: Upload artifact
        uses: actions/upload-artifact@v3
        with:
          name: deb-package
          path: ../*.deb

      - name: Create release
        uses: softprops/action-gh-release@v1
        with:
          files: ../*.deb
```

## Support

For issues with packaging:
- Check `debian/` directory files
- Review build logs
- Test in clean environment (Docker/VM)
- Consult Debian Policy Manual: https://www.debian.org/doc/debian-policy/

---

🤖 *Generated with [Claude Code](https://claude.com/claude-code)*

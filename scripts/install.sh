#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

DRIVER_NAME="R26Audio.driver"
DRIVER_INSTALL_DIR="/Library/Audio/Plug-Ins/HAL"
DAEMON_INSTALL_DIR="/usr/local/bin"
LAUNCHD_PLIST_DIR="/Library/LaunchDaemons"
LAUNCHD_PLIST_NAME="com.r26bridge.r26d.plist"

echo "=== Roland R-26 USB Audio Bridge Installer ==="
echo ""

# Build if needed
if [ ! -d "$BUILD_DIR" ] || [ ! -f "$BUILD_DIR/r26d" ]; then
    echo "Building..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(sysctl -n hw.ncpu)
    cd "$PROJECT_DIR"
    echo "Build complete."
    echo ""
fi

# Check for root
if [ "$(id -u)" -ne 0 ]; then
    echo "This script requires root privileges to install the audio driver."
    echo "Please run: sudo $0"
    exit 1
fi

# Find the driver bundle
DRIVER_BUNDLE=""
if [ -d "$BUILD_DIR/$DRIVER_NAME" ]; then
    DRIVER_BUNDLE="$BUILD_DIR/$DRIVER_NAME"
elif [ -d "$BUILD_DIR/R26Audio.driver" ]; then
    DRIVER_BUNDLE="$BUILD_DIR/R26Audio.driver"
fi

if [ -z "$DRIVER_BUNDLE" ]; then
    # CMake MODULE with BUNDLE sometimes puts it in a different spot
    DRIVER_BUNDLE=$(find "$BUILD_DIR" -name "$DRIVER_NAME" -type d | head -1)
fi

if [ -z "$DRIVER_BUNDLE" ] || [ ! -d "$DRIVER_BUNDLE" ]; then
    echo "ERROR: Could not find $DRIVER_NAME in build directory."
    echo "Contents of build directory:"
    ls -la "$BUILD_DIR"
    exit 1
fi

echo "Installing driver..."

# Remove old driver if present
if [ -d "$DRIVER_INSTALL_DIR/$DRIVER_NAME" ]; then
    echo "  Removing old driver..."
    rm -rf "$DRIVER_INSTALL_DIR/$DRIVER_NAME"
fi

# Copy driver bundle
echo "  Copying $DRIVER_NAME to $DRIVER_INSTALL_DIR/"
cp -R "$DRIVER_BUNDLE" "$DRIVER_INSTALL_DIR/"
chown -R root:wheel "$DRIVER_INSTALL_DIR/$DRIVER_NAME"
chmod -R 755 "$DRIVER_INSTALL_DIR/$DRIVER_NAME"

echo "Installing daemon..."

# Copy daemon binary
cp "$BUILD_DIR/r26d" "$DAEMON_INSTALL_DIR/r26d"
chown root:wheel "$DAEMON_INSTALL_DIR/r26d"
chmod 755 "$DAEMON_INSTALL_DIR/r26d"

echo "Installing launchd plist..."

# Create launchd plist for the daemon
cat > "$LAUNCHD_PLIST_DIR/$LAUNCHD_PLIST_NAME" << 'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.r26bridge.r26d</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/local/bin/r26d</string>
    </array>
    <key>RunAtLoad</key>
    <false/>
    <key>KeepAlive</key>
    <dict>
        <key>SuccessfulExit</key>
        <false/>
    </dict>
    <key>StandardOutPath</key>
    <string>/tmp/r26d.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/r26d.log</string>
</dict>
</plist>
PLIST

chown root:wheel "$LAUNCHD_PLIST_DIR/$LAUNCHD_PLIST_NAME"
chmod 644 "$LAUNCHD_PLIST_DIR/$LAUNCHD_PLIST_NAME"

echo ""
echo "=== Installation Complete ==="
echo ""
echo "Restart CoreAudio to load the driver:"
echo "  sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod"
echo ""
echo "To start the USB bridge daemon (connect R-26 first):"
echo "  sudo launchctl load $LAUNCHD_PLIST_DIR/$LAUNCHD_PLIST_NAME"
echo "  sudo launchctl start com.r26bridge.r26d"
echo ""
echo "Or run manually:"
echo "  sudo r26d"
echo ""
echo "To probe the R-26 USB descriptors without capturing:"
echo "  sudo r26d --probe"
echo ""
echo "The device will appear as 'Roland R-26' in Audio MIDI Setup"
echo "and in any DAW (Logic, Ableton, Reaper, etc.)."
echo ""
echo "Logs: /tmp/r26d.log"

#!/bin/bash
set -euo pipefail

DRIVER_NAME="R26Audio.driver"
DRIVER_INSTALL_DIR="/Library/Audio/Plug-Ins/HAL"
DAEMON_INSTALL_DIR="/usr/local/bin"
LAUNCHD_PLIST_DIR="/Library/LaunchDaemons"
LAUNCHD_PLIST_NAME="com.r26bridge.r26d.plist"

echo "=== Roland R-26 USB Audio Bridge Uninstaller ==="
echo ""

if [ "$(id -u)" -ne 0 ]; then
    echo "This script requires root privileges."
    echo "Please run: sudo $0"
    exit 1
fi

# Stop daemon
echo "Stopping daemon..."
launchctl stop com.r26bridge.r26d 2>/dev/null || true
launchctl unload "$LAUNCHD_PLIST_DIR/$LAUNCHD_PLIST_NAME" 2>/dev/null || true

# Remove launchd plist
if [ -f "$LAUNCHD_PLIST_DIR/$LAUNCHD_PLIST_NAME" ]; then
    echo "Removing launchd plist..."
    rm -f "$LAUNCHD_PLIST_DIR/$LAUNCHD_PLIST_NAME"
fi

# Remove daemon
if [ -f "$DAEMON_INSTALL_DIR/r26d" ]; then
    echo "Removing daemon..."
    rm -f "$DAEMON_INSTALL_DIR/r26d"
fi

# Remove driver
if [ -d "$DRIVER_INSTALL_DIR/$DRIVER_NAME" ]; then
    echo "Removing audio driver..."
    rm -rf "$DRIVER_INSTALL_DIR/$DRIVER_NAME"
fi

# Clean up shared memory
echo "Cleaning up shared memory..."
rm -f /dev/shm/r26audio 2>/dev/null || true

echo ""
echo "Restarting CoreAudio..."
launchctl kickstart -kp system/com.apple.audio.coreaudiod

echo ""
echo "=== Uninstall Complete ==="

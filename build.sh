#!/bin/bash
set -e

BUILD_DIR="build"
CMAKE_OPTS="-DPICO_PLATFORM=rp2350"

# Optional: embed a ROM file
# Usage: NES_ROM=path/to/game.nes ./build.sh
if [ -n "$NES_ROM" ]; then
    # Resolve to absolute path
    NES_ROM_ABS="$(cd "$(dirname "$NES_ROM")" && pwd)/$(basename "$NES_ROM")"
    CMAKE_OPTS="$CMAKE_OPTS -DNES_ROM_PATH=$NES_ROM_ABS"
fi

# Optional: CPU speed (252, 378, 504)
if [ -n "$CPU_SPEED" ]; then
    CMAKE_OPTS="$CMAKE_OPTS -DCPU_SPEED=$CPU_SPEED"
fi

# Optional: video mode (240p or 480p)
if [ -n "$VIDEO_MODE" ]; then
    CMAKE_OPTS="$CMAKE_OPTS -DVIDEO_MODE=$VIDEO_MODE"
fi

# Optional: composite TV output instead of HDMI
# Usage: VIDEO_COMPOSITE=1 ./build.sh
if [ "${VIDEO_COMPOSITE:-0}" = "1" ]; then
    CMAKE_OPTS="$CMAKE_OPTS -DVIDEO_COMPOSITE=ON"
fi

# USB HID host mode (disabled by default for USB serial logging)
# Usage: USB_HID=1 ./build.sh  to enable (release builds use release.sh)
if [ "${USB_HID:-0}" = "1" ]; then
    CMAKE_OPTS="$CMAKE_OPTS -DUSB_HID_ENABLED=ON"
else
    CMAKE_OPTS="$CMAKE_OPTS -DUSB_HID_ENABLED=OFF"
fi

# Clean build dir if video output mode changed (CMake caches the option)
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    CACHED=$(grep -s 'VIDEO_COMPOSITE:BOOL=' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2)
    WANTED="OFF"
    [ "${VIDEO_COMPOSITE:-0}" = "1" ] && WANTED="ON"
    if [ "$CACHED" != "$WANTED" ]; then
        rm -rf "$BUILD_DIR"
    fi
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake $CMAKE_OPTS ../src/platform/pico
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo ""
echo "Build complete. Firmware: build/frank-nes.uf2"

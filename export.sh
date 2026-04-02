#!/bin/bash
# Builds the plugin and packages it for distribution.
# Usage: ./export.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
EXPORT_DIR="$SCRIPT_DIR/dist/SyncComms"
ZIP_FILE="$SCRIPT_DIR/dist/SyncComms.zip"

CMAKE="/c/Program Files/CMake/bin/cmake.exe"

echo "=== Building Release ==="
"$CMAKE" -B "$BUILD_DIR" -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTS=OFF
"$CMAKE" --build "$BUILD_DIR" --config Release

echo "=== Packaging ==="
rm -rf "$EXPORT_DIR" "$ZIP_FILE"
mkdir -p "$EXPORT_DIR/bakkesmod/plugins/settings"
mkdir -p "$EXPORT_DIR/bakkesmod/cfg"

cp "$BUILD_DIR/plugins/Release/SyncComms.dll" "$EXPORT_DIR/bakkesmod/plugins/"
cp "$SCRIPT_DIR/data/synccomms.set"            "$EXPORT_DIR/bakkesmod/plugins/settings/"
cp "$SCRIPT_DIR/data/synccomms.cfg"            "$EXPORT_DIR/bakkesmod/cfg/"

cat > "$EXPORT_DIR/README.txt" << 'EOF'
SyncComms - Replay Audio Sync for Rocket League
================================================

INSTALLATION:

1. Copy the "bakkesmod" folder from this archive into:
   %APPDATA%/bakkesmod/

   (Merge with the existing bakkesmod folder when prompted.)

2. Open the file:
   %APPDATA%/bakkesmod/bakkesmod/cfg/plugins.cfg

   Add this line at the end:
   plugin load synccomms

3. Launch Rocket League. The plugin will load automatically.

USAGE:

- Enable the plugin in BakkesMod settings (F2) > Plugins > SyncComms
- Audio is captured automatically during any match
- Open a saved replay to hear the captured audio synced to gameplay
- Use the settings tab to pick which app to capture (Discord, etc.)
EOF

cd "$SCRIPT_DIR/dist"
"$CMAKE" -E tar cf "$ZIP_FILE" --format=zip "SyncComms"

echo ""
echo "=== Done ==="
echo "Package ready: $ZIP_FILE"

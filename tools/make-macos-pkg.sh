#!/bin/sh
# Build the (unsigned) macOS installer package from a completed universal
# plugin build on the Mac. Run ON the Mac from the repo root after:
#   ./scripts/fetch-vendor.sh && ./plugin/fetch-deps.sh
#   cmake -B plugin/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
#   cmake --build plugin/build -j
# Produces dist/ElfDrumLXR-<version>-macos.pkg with VST3 + AU + CLAP +
# Standalone payloads. UNSIGNED: Gatekeeper will warn on other machines,
# and notarisation requires Garth's Apple Developer ID (not available to
# the build) — see STATUS.md "Release model".
set -e
cd "$(dirname "$0")/.."
VERSION=$(grep -o 'VERSION [0-9.]*' plugin/CMakeLists.txt | head -1 | awk '{print $2}')
A=plugin/build/ElfDrumLXR_artefacts/Release
ROOT=$(mktemp -d)/root
mkdir -p "$ROOT/Library/Audio/Plug-Ins/VST3" \
         "$ROOT/Library/Audio/Plug-Ins/Components" \
         "$ROOT/Library/Audio/Plug-Ins/CLAP" \
         "$ROOT/Applications"
cp -R "$A/VST3/ElfDrumLXR.vst3" "$ROOT/Library/Audio/Plug-Ins/VST3/"
cp -R "$A/AU/ElfDrumLXR.component" "$ROOT/Library/Audio/Plug-Ins/Components/"
cp -R "$A/CLAP/ElfDrumLXR.clap" "$ROOT/Library/Audio/Plug-Ins/CLAP/"
cp -R "$A/Standalone/ElfDrumLXR.app" "$ROOT/Applications/"
mkdir -p dist
pkgbuild --root "$ROOT" --identifier com.garth.elfdrumlxr \
         --version "$VERSION" "dist/ElfDrumLXR-$VERSION-macos.pkg"
echo "built dist/ElfDrumLXR-$VERSION-macos.pkg (unsigned)"

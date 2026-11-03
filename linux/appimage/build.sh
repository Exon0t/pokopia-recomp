#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Linux launcher AppImage: linux/appimage/build/PokopiaLauncher-<version>-x86_64.AppImage
#
#   linux/appimage/build.sh            (from the repository root; needs podman and the .NET 10 SDK)
#
# 1. linux-tools (the AppImage's resources), every download pinned and checked:
#    python/   python-build-standalone CPython 3.13 with lz4 and capstone (the lifter and the shader extractor)
#    zig/      Zig 0.17.0 for Linux (the release's own build uses Zig 0.17.0 for Windows: same compiler, same output)
#    spirv_harness/  the shader translator, built here from tools/shaders with the Ryubing sources it pins, published
#              self-contained (no .NET install needed on the Deck), server GC on
#    scripts/  pkp_linux.py, fetch_release.py, steam_shortcut.py
#    runtime/  pkp_rt.dll of this repository (linux/winbuild/build.sh must have built it): the launcher puts it into the game
#              folder in place of the release's (it runs on AMD GPUs, the Steam Deck's included)
# 2. The Tauri launcher built and bundled as an AppImage inside the Ubuntu 22.04 container (linux/appimage/Containerfile),
#    so it runs on any distribution with glibc 2.35 or newer, SteamOS included.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=$ROOT/linux/appimage/build
LT=$OUT/linux-tools
DL=$HOME/.cache/pokopia-appimage/downloads
mkdir -p "$OUT" "$DL"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1

PY_URL='https://github.com/astral-sh/python-build-standalone/releases/download/20261026/cpython-3.13.18%2B20261026-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz'
PY_SHA=df15fe60d3ff55bf9dcf25824b5572756bb8816ff93865a273d3301d7066a8b7
ZIG_URL='https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz'
ZIG_SHA=1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026
APPIMAGETOOL_URL='https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage'
APPIMAGETOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
RUNTIME_URL='https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64'
RUNTIME_SHA=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
RYU_URL='https://codeberg.org/Software_arch/Ryubing.git'
RYU_COMMIT=a82350bb774f70fcbd41c9987bf67a3775409963

fetch() {  # url sha256 -> path in the download cache
    local f="$DL/$(basename "${1//%2B/+}")"
    if ! echo "$2  $f" | sha256sum -c --status 2>/dev/null; then
        curl -fsSL --retry 3 -o "$f.part" "$1"
        echo "$2  $f.part" | sha256sum -c - >&2
        mv "$f.part" "$f"
    fi
    echo "$f"
}

echo "== linux-tools"
rm -rf "$LT"; mkdir -p "$LT/scripts"

echo "-- Python"
tar -xzf "$(fetch "$PY_URL" "$PY_SHA")" -C "$LT"            # -> $LT/python
"$LT/python/bin/python3" -m pip install --quiet --no-cache-dir --disable-pip-version-check --no-warn-script-location \
    lz4==4.4.5 capstone==5.0.9
"$LT/python/bin/python3" -I -c 'import lz4.block, capstone; print("   python", __import__("sys").version.split()[0], "lz4", lz4.__version__)'

echo "-- Zig"
mkdir -p "$LT/zig"; tar -xJf "$(fetch "$ZIG_URL" "$ZIG_SHA")" -C "$LT/zig" --strip-components=1
echo "   zig $("$LT/zig/zig" version)"

echo "-- shader translator"
TP=$ROOT/tools/shaders/third_party
if [ ! -d "$TP/ryubing/src/Ryujinx.Graphics.Shader" ]; then
    rm -rf "$TP/ryubing-src" "$TP/ryubing"; mkdir -p "$TP/ryubing-src" "$TP/ryubing/src"
    git -C "$TP/ryubing-src" init -q
    git -C "$TP/ryubing-src" fetch -q --depth 1 "$RYU_URL" "$RYU_COMMIT"
    git -C "$TP/ryubing-src" checkout -q FETCH_HEAD
    for p in Ryujinx.Common Spv.Generator Ryujinx.Graphics.Shader; do
        rsync -a --exclude obj --exclude bin "$TP/ryubing-src/src/$p/" "$TP/ryubing/src/$p/"
    done
    for f in Directory.Build.props Directory.Packages.props nuget.config LICENSE.txt; do cp "$TP/ryubing-src/$f" "$TP/ryubing/$f"; done
    printf '{\n  "sdk": {\n    "version": "10.0.100",\n    "rollForward": "latestFeature"\n  }\n}\n' > "$TP/ryubing/global.json"
fi
dotnet publish "$ROOT/tools/shaders/harness/SpirvHarness.csproj" -c Release -r linux-x64 --self-contained true \
    -p:ServerGarbageCollection=true -p:InvariantGlobalization=true -o "$LT/spirv_harness" -v quiet -nologo
# optional LTTng tracing provider: CoreCLR loads it only if liblttng-ust is installed; nothing here uses it
rm -f "$LT/spirv_harness/libcoreclrtraceptprovider.so"
echo "   spirv_harness: $(du -sh "$LT/spirv_harness" | cut -f1)"

echo "-- scripts"
cp "$ROOT/linux/pkp_linux.py" "$ROOT/linux/appimage/fetch_release.py" "$ROOT/linux/appimage/steam_shortcut.py" "$LT/scripts/"

echo "-- runtime"
RT=$ROOT/game-build/build/linux-rt/pkp_rt.dll
[ -f "$RT" ] || { echo "No $RT: build the runtime first (linux/winbuild/build.sh)." >&2; exit 1; }
mkdir -p "$LT/runtime" && cp "$RT" "$LT/runtime/pkp_rt.dll"
echo "   pkp_rt.dll $(sha256sum "$RT" | cut -c1-16)"
echo "   linux-tools: $(du -sh --apparent-size "$LT" | cut -f1)"

echo "== launcher AppImage (container)"
# Tauri packages the launcher alone: linuxdeploy rewrites every ELF file it finds in the AppDir (library paths) and would
# do that to the self-contained Python and .NET runtime too, so linux-tools is added afterwards and the AppDir is packed
# again with appimagetool, which takes files as they are.
mkdir -p "$HOME/.cache/pokopia-appimage/cargo-registry"
rm -rf "$ROOT/launcher/src-tauri/target/release/bundle"    # Tauri reuses old staging (appimage_deb/, the AppDir)
podman run --rm --security-opt label=disable -v "$ROOT:/src" \
    -v "$HOME/.cache/pokopia-appimage/cargo-registry:/usr/local/cargo/registry" \
    -e APPIMAGE_EXTRACT_AND_RUN=1 -e NO_STRIP=true -w /src/launcher pokopia-appimage-build \
    sh -c 'npm ci --no-audit --no-fund >/dev/null && npm run tauri build -- --bundles appimage'
AD=$ROOT/launcher/src-tauri/target/release/bundle/appimage/PokopiaLauncher.AppDir
[ -x "$AD/AppRun" ] || { echo "no AppDir at $AD" >&2; exit 1; }

echo "== add linux-tools, pack with appimagetool"
rm -rf "$AD/usr/lib/PokopiaLauncher/linux-tools"; mkdir -p "$AD/usr/lib/PokopiaLauncher"
cp -a "$LT" "$AD/usr/lib/PokopiaLauncher/linux-tools"
cp "$(fetch "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA")" "$OUT/appimagetool"; chmod +x "$OUT/appimagetool"
cp "$(fetch "$RUNTIME_URL" "$RUNTIME_SHA")" "$OUT/runtime-x86_64"
VER=$(python3 -I -c 'import json; print(json.load(open("launcher/src-tauri/tauri.conf.json"))["version"])')
NAME=PokopiaLauncher-$VER-x86_64.AppImage
podman run --rm --security-opt label=disable -v "$ROOT:/src" -e APPIMAGE_EXTRACT_AND_RUN=1 -e ARCH=x86_64 -w /src pokopia-appimage-build \
    linux/appimage/build/appimagetool --no-appstream --runtime-file linux/appimage/build/runtime-x86_64 \
    launcher/src-tauri/target/release/bundle/appimage/PokopiaLauncher.AppDir "linux/appimage/build/$NAME" > "$OUT/appimagetool.log" 2>&1 \
    || { tail -20 "$OUT/appimagetool.log" >&2; exit 1; }
echo "== $OUT/$NAME ($(du -h "$OUT/$NAME" | cut -f1))"

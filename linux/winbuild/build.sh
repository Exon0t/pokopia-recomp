#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Windows runtime pkp_rt.dll (+ pkp_rt.lib) on Linux, in the pokopia-winbuild container (linux/winbuild/Containerfile).
#
#   linux/winbuild/build.sh sdk    once: downloads the MSVC CRT and the Windows SDK from Microsoft with xwin.
#                                  Running it means accepting Microsoft's licence terms for them (xwin --accept-license).
#   linux/winbuild/build.sh        tools/textures (astc2bc7 + bc7e), libvpx 1.16.0, then game-build with PKP_RT_DLL=ON
#
# Output: game-build/build/linux-rt/pkp_rt.dll and pkp_rt.lib. Caches (SDK, downloads, libvpx) in ~/.cache/pokopia-winbuild.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
C=$HOME/.cache/pokopia-winbuild
mkdir -p "$C"

run() {  # runs a bash command in the container: repo at /src, caches at /cache, the SDK at /xwin
    podman run --rm --security-opt label=disable -v "$ROOT:/src" -v "$C:/cache" -v "$C/xwin:/xwin:ro" -w /src pokopia-winbuild bash -euo pipefail -c "$1"
}

if [ "${1:-}" = sdk ]; then
    podman run --rm --security-opt label=disable -v "$C:/cache" pokopia-winbuild \
        xwin --accept-license --cache-dir /cache/xwin-dl splat --output /cache/xwin
    rm -rf "$C/xwin-dl"
    echo "SDK ready in $C/xwin"
    exit 0
fi
[ -f "$C/xwin/crt/lib/x86_64/libcmt.lib" ] || { echo "No MSVC CRT/SDK yet: run '$0 sdk' first (it accepts Microsoft's licence)." >&2; exit 1; }
TC=/src/linux/winbuild/clang-cl-xwin.cmake

# Windows file names are case-insensitive and sources spell some SDK headers differently from the SDK; xwin adds the common
# variants as links, these are the ones it misses (target: the file as the SDK has it)
fixup() { [ -e "$C/xwin/$1" ] || ln -s "$2" "$C/xwin/$1"; }
fixup sdk/include/um/Softpub.h SoftPub.h

echo "== tools/textures (astc2bc7, bc7e_ispc, astcdec DLLs)"
run "cmake -S tools/textures -B tools/textures/build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$TC \
        -DA2B_ISPC=/src/linux/winbuild/ispc-win.sh >/dev/null \
     && cmake --build tools/textures/build/release --target astc2bc7 bc7e_ispc astcdec-avx2-shared astcdec-sse4.1-shared astcdec-sse2-shared \
     && { cp tools/textures/build/release/astcenc/Source/astcdec-*-shared.dll tools/textures/build/release/ 2>/dev/null || true; }"

echo "== libvpx 1.16.0 (VP8/VP9 decoders, static, MSVC ABI)"
if [ ! -f "$C/libvpx-install/lib/vpx.lib" ]; then
    run '
X=/xwin
# -idirafter: after the clang resource headers (as /imsvc does for clang-cl), so the clang intrinsics headers win over MSVC ones
INC="-idirafter $X/crt/include -idirafter $X/sdk/include/ucrt -idirafter $X/sdk/include/um -idirafter $X/sdk/include/shared"
export CC="clang --target=x86_64-pc-windows-msvc -fms-runtime-lib=static -fuse-ld=lld $INC -D_CRT_SECURE_NO_WARNINGS"
export CXX="clang++ --target=x86_64-pc-windows-msvc -fms-runtime-lib=static -fuse-ld=lld $INC"
export LD="$CC" AR=llvm-ar AS=nasm NM=llvm-nm STRIP=llvm-strip RANLIB=llvm-ranlib
export LDFLAGS="-L$X/crt/lib/x86_64 -L$X/sdk/lib/um/x86_64 -L$X/sdk/lib/ucrt/x86_64"
rm -rf /cache/libvpx-build /cache/libvpx-install; mkdir -p /cache/libvpx-build; cd /cache/libvpx-build
/cache/src/libvpx/configure --target=x86_64-win64-gcc --as=nasm --prefix=/cache/libvpx-install \
    --disable-examples --disable-tools --disable-docs --disable-unit-tests \
    --disable-vp8-encoder --disable-vp9-encoder --enable-vp8-decoder --enable-vp9-decoder \
    --enable-static --disable-shared --enable-multithread --enable-runtime-cpu-detect
make -j"$(nproc)"
make install
mv /cache/libvpx-install/lib/libvpx.a /cache/libvpx-install/lib/vpx.lib'
fi

echo "== pkp_rt.dll"
run "cmake -S game-build -B game-build/build/linux-rt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$TC \
        -DPKP_RT_DLL=ON -DPKP_VERSION=0.1.1 -DNXRT_DEPS_DIR=/cache/deps \
        '-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /MAP' \
        -DNXRT_A2B_DIR=/src/tools/textures/build/release \
        -DNXRT_LIBVPX_INCLUDE=/cache/libvpx-install/include -DNXRT_LIBVPX_LIB=/cache/libvpx-install/lib/vpx.lib \
     && cmake --build game-build/build/linux-rt --target pkp_rt"
ls -la game-build/build/linux-rt/pkp_rt.dll game-build/build/linux-rt/pkp_rt.lib

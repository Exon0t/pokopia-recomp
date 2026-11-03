#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# ISPC for Windows objects on Linux. The Linux build of ISPC cannot target Windows (--target-os=windows is not built in), so
# this takes the same command line, has ISPC emit LLVM bitcode for Linux instead, and compiles each module with clang for
# x86_64-pc-windows-msvc: the code generator (calling convention, COFF, stack probes) is then Windows'. Multi-target builds
# give the same files as the Windows ISPC: <out>.obj (the CPU dispatcher) and <out>_<isa>.obj per target.
set -euo pipefail
args=() out=""
while [ $# -gt 0 ]; do
    case "$1" in
        --target-os=windows) args+=(--target-os=linux) ;;
        -o) out=$2; shift ;;
        *) args+=("$1") ;;
    esac
    shift
done
[ -n "$out" ] || { echo "ispc-win: no -o given" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
base=$(basename "${out%.*}")
ispc "${args[@]}" --emit-llvm -o "$tmp/$base.bc"
# ISPC applies a target's CPU when it generates code itself; in bitcode it is not recorded, so give clang the CPU ISPC uses
# for each target (sse4: Nehalem, avx2: Haswell, avx512skx: Skylake-AVX512). The dispatcher picks one by CPUID at run time.
for bc in "$tmp"/*.bc; do
    name=$(basename "${bc%.bc}")
    case "$name" in
        *_sse4) cpu=nehalem ;; *_sse2) cpu=x86-64 ;; *_avx1) cpu=sandybridge ;; *_avx2) cpu=haswell ;;
        *_avx512skx) cpu=skylake-avx512 ;; *_avx512spr) cpu=sapphirerapids ;; *) cpu=x86-64 ;;
    esac
    clang --target=x86_64-pc-windows-msvc -march="$cpu" -O2 -Wno-override-module -c "$bc" -o "$(dirname "$out")/$name.obj"
done

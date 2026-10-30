#!/bin/bash
# build_aot_docker.sh: compiles the game's x86-64 code ahead of time into native AArch64 code,
# libbbaot.so, for aarch64 Linux (glibc 2.35 or newer), in a Linux Docker container.
#
# The compiler is bb-recomp from the Bloodborne Android Rebuild Tool (ducvd89,
# github.com/ducvd89/Bloodborne-android-rebuild-tool; GPL-3.0-or-later plus its
# ADDITIONAL_TERMS.md), built unmodified from your checkout of it. Its front end is FEXCore 2609.1,
# taken from this tree's third_party/FEX (the FEX the port's libbbcpu.so runs), its back end the
# official LLVM 23.1.3 release build. The tool links Android libraries itself; this script asks it
# for object files instead (--obj) and links them against an Ubuntu 22.04 arm64 sysroot, so the
# library loads with glibc 2.35 and newer.
#
# The library must be made from the exact game image bbport runs: out/boot-linked.bin in bbport's
# data folder (on the handheld ~/.local/share/bbport/out/boot-linked.bin, written at every launch
# by scripts/prepare.py, link_libc.py and link_modules.py; take link.json from there too). Besides
# it the tool reads four game files, for their headers only: eboot.bin, sce_module/libc.prx,
# sce_module/libSceFios2.prx and sce_sys/param.sfo. A folder with just those four is enough.
#
# The port runs it with the ahead-of-time guest CPU (cpu/aot, out/aot-cpu/libbbcpu.so):
#   BB_AOT=<path>/libbbaot.so bash run.sh     (or put it at <data>/aot/<sha256 of boot-linked.bin>/ and use BB_AOT=auto)
# libbbaot.so is the game's own code, translated: keep it to yourself, never commit, ship or upload it.
#
# Hosts: macOS on Apple Silicon (Docker Desktop, OrbStack or colima) or Linux on arm64 or x86-64
# (Docker Engine). The container runs Docker's own architecture, never emulated. On an x86-64
# host bb-recomp is built for x86-64 (it then needs a CPU with AVX2) and still compiles for AArch64.
#
# Usage:  ./build_aot_docker.sh --game DIR [--boot FILE] [--out DIR] [--host PROFILE] [aot]
#         ./build_aot_docker.sh [--out DIR] device-tool
#         ./build_aot_docker.sh tool | shell | clean
#   aot     (default) build bb-recomp when needed, compile the game image, link libbbaot.so
#   device-tool  no game files: bb-recomp for the handheld itself (aarch64 Linux, fully static, the
#           tool's own arm64/build.sh aarch64) with what it needs to write a glibc libbbaot.so there
#           (share/bb-recomp: the x87 helpers, a libbbcpu.so stub, glibc's libm, compiler-rt, the
#           variant catalog) and the tool's licenses, into --out (default: aot-tool next to this
#           tree). Copy that folder to the handheld's data folder (~/.local/share/bbport/aot-tool);
#           the launcher's Native CPU builds the library there from the game on the device
#           (scripts/aot_build.py). On an x86-64 host it also downloads the AArch64 LLVM (1.1 GB)
#   tool    only build bb-recomp. The first run downloads LLVM (1.1 GB, 12 GB unpacked) and an
#           arm64 sysroot, and builds FEXCore and the tool (10 to 30 minutes)
#   shell   interactive shell in the build container (the Docker volume is at /aot)
#   clean   delete the Docker image and volume (LLVM, sysroot, FEX copy, build, objects)
#   --game  the game folder, or the four-file folder above (mounted read-only)
#   --boot  bbport's boot-linked.bin, copied from the handheld (recommended). Without it the
#           image is made here from --game with this tree's scripts (the whole game folder
#           needed, no mods): the same file only when the game, mods and scripts are the same
#   --out   where libbbaot.so and its reports go (default: aot-out next to this tree, outside
#           any repository)
#   --host  bb-recomp's target profile: armv8.2 | armv8.4 (default, the tool's own) | armv9
#           (adds Armv8.5 flag instructions: Snapdragon 8 Gen 2 and newer, 8 Elite included)
#
# Settings (environment variables):
#   BB_RECOMP_SRC  the Rebuild Tool checkout (default: Bloodborne-android-rebuild-tool next to
#                  this tree)
#   BB_FEX_SRC     FEX 2609.1 with its submodules (default: this tree's third_party/FEX)
#   JOBS           compiler processes while building bb-recomp (default: from Docker's RAM,
#                  about 2 GB each)
#   BB_AOT_JOBS    bb-recomp's worker threads (default: its own choice, sized to the free RAM;
#                  about 0.5 GB + 0.8 GB per thread). The result does not depend on it
#   BB_AOT_OPT     0 | 1 (default) | 2: bb-recomp's optimisation level
#   BB_AOT_VARIANTS 1 (default) | 0: compile the tool's variant catalog (variants/*.txt: the port's patches as precompiled
#                  variants, its hook sites as native hook slots). Without it every function the port patches at start-up
#                  (patches.bin: the heap and resolution patches, the effect switches ...) has no native code and stops the game
#
# Results in --out: libbbaot.so (~125 MB); libbbaot.txt (what it was made from and with, what
# the runtime must provide); bb-recomp.log and compile.txt (the compiler's log and summary).
# The script runs no git commands and writes nothing into the tool's or this tree's folders.
#
# Written for macOS's stock bash 3.2 and any newer bash.
set -eo pipefail

LLVM_VER=${BB_LLVM_VERSION:-23.1.3}
IMAGE_REV=1
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
SRC=${BB_SRC:-$HERE}
MODE=aot
GAME=""
BOOT=""
OUT=""
HOST_PROFILE=armv8.4

die() { echo "ERROR: $*" >&2; exit 1; }
usage() { echo "Usage: $0 --game DIR [--boot FILE] [--out DIR] [--host armv8.2|armv8.4|armv9] [aot|device-tool|tool|shell|clean]" >&2; exit 2; }
norm_arch() {
    case $1 in
        aarch64|arm64|armv8*) echo aarch64 ;;
        x86_64|amd64|x64|x86-64|x86) echo x86_64 ;;
        *) echo "" ;;
    esac
}

while [ $# -gt 0 ]; do
    case $1 in
        --game) [ $# -ge 2 ] || usage; GAME=$2; shift 2 ;;
        --game=*) GAME=${1#--game=}; shift ;;
        --boot) [ $# -ge 2 ] || usage; BOOT=$2; shift 2 ;;
        --boot=*) BOOT=${1#--boot=}; shift ;;
        --out) [ $# -ge 2 ] || usage; OUT=$2; shift 2 ;;
        --out=*) OUT=${1#--out=}; shift ;;
        --host) [ $# -ge 2 ] || usage; HOST_PROFILE=$2; shift 2 ;;
        --host=*) HOST_PROFILE=${1#--host=}; shift ;;
        -h|--help) awk 'NR > 1 && /^set -eo/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
        aot|device-tool|tool|shell|clean) MODE=$1; shift ;;
        *) echo "ERROR: unknown argument '$1'" >&2; usage ;;
    esac
done
case $HOST_PROFILE in armv8.2|armv8.4|armv9) ;; *) die "--host must be armv8.2, armv8.4 or armv9" ;; esac
AOT_OPT=${BB_AOT_OPT:-1}
AOT_VARIANTS=${BB_AOT_VARIANTS:-1}
case $AOT_VARIANTS in 0|1) ;; *) die "BB_AOT_VARIANTS must be 0 or 1" ;; esac
case $AOT_OPT in 0|1|2) ;; *) die "BB_AOT_OPT must be 0, 1 or 2" ;; esac
AOT_JOBS=${BB_AOT_JOBS:-}
case $AOT_JOBS in ''|[1-9]|[1-9][0-9]) ;; *) die "BB_AOT_JOBS must be a number from 1 to 99" ;; esac

command -v docker >/dev/null || die "docker not found. Install Docker Desktop, OrbStack, colima or Docker Engine."
docker info >/dev/null 2>&1 || die "Docker is installed but not running (or this user may not use it)."
ARCH=$(norm_arch "$(docker info --format '{{.Architecture}}' 2>/dev/null)")
[ -n "$ARCH" ] || die "cannot tell Docker's CPU architecture (docker info --format '{{.Architecture}}')"
if [ "$ARCH" = aarch64 ]; then
    PLATFORM=linux/arm64; LARCH=ARM64; MARCH=armv8-a
else
    PLATFORM=linux/amd64; LARCH=X64; MARCH=x86-64-v3
fi
IMAGE=bbport-aot-builder:$IMAGE_REV-$ARCH
VOLUME=bbport-aot-$ARCH

if [ "$MODE" = clean ]; then
    docker image rm "$IMAGE" 2>/dev/null || true
    docker volume rm "$VOLUME" 2>/dev/null || true
    echo "Removed the Docker image $IMAGE and volume $VOLUME (LLVM, sysroot, FEX copy, build, objects)."
    echo "Libraries already written to an --out folder were not touched."
    exit 0
fi

SRC=$(cd -- "$SRC" 2>/dev/null && pwd) || die "BB_SRC directory not found"
[ -f "$SRC/scripts/link_modules.py" ] && [ -d "$SRC/cpu" ] ||
    die "$SRC is not the ARM_bloodborne_pc tree (no scripts/link_modules.py or cpu/). Put this script in its root or set BB_SRC."
TOOL=${BB_RECOMP_SRC:-$(dirname -- "$SRC")/Bloodborne-android-rebuild-tool}
TOOL=$(cd -- "$TOOL" 2>/dev/null && pwd) ||
    die "the Rebuild Tool checkout was not found at ${BB_RECOMP_SRC:-$(dirname -- "$SRC")/Bloodborne-android-rebuild-tool} (set BB_RECOMP_SRC)"
for f in bbrecomp.cmake fex_project.cmake src/main.cpp src/compile.cpp helpers/bbr_x87_all.c fetch-sysroot.py; do
    [ -f "$TOOL/$f" ] || die "$TOOL is not the Rebuild Tool's source (no $f)"
done
FEXSRC=${BB_FEX_SRC:-$SRC/third_party/FEX}
FEXSRC=$(cd -- "$FEXSRC" 2>/dev/null && pwd) || die "FEX source not found (${BB_FEX_SRC:-$SRC/third_party/FEX})"
for f in FEXCore/CMakeLists.txt External/fmt/CMakeLists.txt External/vixl/CMakeLists.txt Source/Common/cpp-optparse/OptionParser.h; do
    [ -f "$FEXSRC/$f" ] || die "FEX at $FEXSRC is incomplete (no $f). Check out the submodules yourself: git submodule update --init --recursive"
done
# The FEX version the port last built (build.sh writes it): bb-recomp is made for FEX-2609.1.
fexver="$SRC/out/fex/generated/git_version.h"
if [ -f "$fexver" ] && ! grep -q '"FEX-2609.1"' "$fexver"; then
    echo "WARNING: the port's last FEX build was $(sed -n 's/.*GIT_DESCRIBE_STRING "\(.*\)".*/\1/p' "$fexver"), not FEX-2609.1:" \
         "bb-recomp and the guest state layout it compiles for are made for 2609.1." >&2
fi
if [ "$ARCH" = x86_64 ]; then
    avx2=""
    if [ "$(uname -s)" = Darwin ]; then
        sysctl -n machdep.cpu.leaf7_features 2>/dev/null | grep -q AVX2 && avx2=1
    else
        grep -qw avx2 /proc/cpuinfo 2>/dev/null && avx2=1
    fi
    [ -n "$avx2" ] || echo "WARNING: this CPU may lack AVX2; the x86-64 build of bb-recomp needs it (Intel 2013+, AMD Ryzen)." >&2
fi

mounts=(-v "$VOLUME":/aot -v "$TOOL":/src/tool:ro -v "$FEXSRC":/src/fex:ro -v "$SRC":/fork:ro)
BOOT_FILE=""
if [ "$MODE" = aot ]; then
    [ -n "$GAME" ] || die "--game DIR is required (the game folder, or a folder with eboot.bin, sce_module/libc.prx, sce_module/libSceFios2.prx and sce_sys/param.sfo)"
    GAME=$(cd -- "$GAME" 2>/dev/null && pwd) || die "game folder not found: $GAME"
    for f in eboot.bin sce_module/libc.prx sce_module/libSceFios2.prx sce_sys/param.sfo; do
        [ -f "$GAME/$f" ] || die "no $f in $GAME"
    done
    mounts+=(-v "$GAME":/in/game:ro)
    if [ -n "$BOOT" ]; then
        [ -f "$BOOT" ] || die "boot image not found: $BOOT"
        [ "$(head -c 8 "$BOOT")" = BBPROBE5 ] || die "$BOOT is not a boot-linked.bin (BBPROBE5) made by scripts/link_modules.py"
        boot_dir=$(cd -- "$(dirname -- "$BOOT")" && pwd)
        BOOT_FILE=/in/boot/$(basename -- "$BOOT")
        mounts+=(-v "$boot_dir":/in/boot:ro)
        [ -f "$boot_dir/link.json" ] || echo "NOTE: no link.json next to $BOOT: bb-recomp assumes the default modules (libc.prx, libSceFios2.prx)."
    else
        [ -d "$GAME/dvdroot_ps4" ] ||
            die "without --boot the image is made from the whole game folder, and $GAME has no dvdroot_ps4. Copy out/boot-linked.bin (and out/link.json) from bbport's data folder and pass --boot."
        echo "NOTE: no --boot: making boot-linked.bin from $GAME with this tree's scripts. The library fits only if the handheld runs the same game files (no mods that change eboot.bin or sce_module) and the same scripts."
    fi
    OUT=${OUT:-$(dirname -- "$SRC")/aot-out}
    mkdir -p "$OUT" || die "cannot create $OUT"
    OUT=$(cd -- "$OUT" && pwd)
    case "$OUT/" in
        "$SRC/"*|"$TOOL/"*) echo "WARNING: $OUT is inside a repository. Never commit libbbaot.so (it is the game's code)." >&2 ;;
    esac
    mounts+=(-v "$OUT":/out)
fi
if [ "$MODE" = device-tool ]; then
    OUT=${OUT:-$(dirname -- "$SRC")/aot-tool}
    mkdir -p "$OUT" || die "cannot create $OUT"
    OUT=$(cd -- "$OUT" && pwd)
    mounts+=(-v "$OUT":/out)
fi

mem=$(docker info --format '{{.MemTotal}}' 2>/dev/null || echo 0)
ncpu=$(docker info --format '{{.NCPU}}' 2>/dev/null || echo 0)
case $mem in ''|*[!0-9]*) mem=0 ;; esac
case $ncpu in ''|*[!0-9]*) ncpu=0 ;; esac
mem_mib=$((mem / 1048576))
if [ -z "${JOBS:-}" ]; then
    JOBS=$((mem_mib / 2048))
    if [ "$ncpu" -gt 0 ] && [ "$JOBS" -gt "$ncpu" ]; then JOBS=$ncpu; fi
    if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
fi
case $JOBS in ''|*[!0-9]*|0) die "JOBS must be a positive number" ;; esac
echo "==> Docker ($ARCH): ${ncpu} CPUs, ${mem_mib} MiB RAM; bb-recomp build with JOBS=${JOBS}"
if [ "$mem_mib" -lt 7500 ]; then
    echo "WARNING: little RAM for this. Give Docker 8 GB or more (Docker Desktop: Settings > Resources > Memory)." >&2
fi
echo "==> Rebuild Tool: $TOOL"
echo "==> FEX:          $FEXSRC"

# Linux host, rootful Docker Engine: files the container writes into --out belong to root; they
# are given back to this user (ownership only). Not with Podman, rootless Docker or Docker Desktop.
OWNER=""
if [ "$(uname -s)" = Linux ]; then
    case "$(docker --version 2>/dev/null) $(docker info --format '{{.OperatingSystem}} {{.SecurityOptions}}' 2>/dev/null)" in
        *odman*|*rootless*|*"Docker Desktop"*) ;;
        *) OWNER="${SUDO_UID:-$(id -u)}:${SUDO_GID:-$(id -g)}" ;;
    esac
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "==> Making the Docker image $IMAGE (Ubuntu 24.04 with build tools; once)"
    printf '%s\n' \
        'FROM ubuntu:24.04' \
        'ENV DEBIAN_FRONTEND=noninteractive' \
        'RUN apt-get update \' \
        ' && apt-get install -y --no-install-recommends build-essential binutils cmake ninja-build python3 \' \
        '      curl ca-certificates zstd xz-utils zlib1g-dev libzstd-dev libxml2-dev \' \
        ' && rm -rf /var/lib/apt/lists/*' |
        docker build --platform "$PLATFORM" -t "$IMAGE" - || die "docker build of $IMAGE failed"
fi

# Runs inside the container. Paths: /aot (volume), /src/tool, /src/fex, /fork (this tree), all
# read-only but /aot; /in/game, /in/boot (read-only) and /out for aot.
INNER=$(cat <<'INNER_SCRIPT'
set -euo pipefail
mode=$1
say() { echo "==> $*"; }
fail() { echo "ERROR: $*" >&2; exit 1; }
L=$A/llvm/LLVM-$LLVM_VER-Linux-$LARCH
SYSROOT=$A/sysroot-jammy-arm64
GCC_DIR=$SYSROOT/usr/lib/gcc/aarch64-linux-gnu/12
B=$A/build-tool
W=$A/work
mkdir -p "$A"

if [ "$mode" = shell ]; then
    if [ -x "$L/bin/clang" ]; then export PATH=$L/bin:$PATH; fi
    cd "$A"
    echo "Volume: $A (llvm/, sysroot-jammy-arm64/, fex-src/, build-tool/, work/). Sources: $TOOL, $FEX, $FORK (read-only). exit leaves."
    exec bash
fi
if [ -n "${OWNER:-}" ] && [ -d "$OUT" ]; then
    trap 'find "$OUT" -user 0 -exec chown -h "$OWNER" {} + 2>/dev/null || true' EXIT
fi

# LLVM: the official release build (clang, lld, static libraries, CMake package).
if [ ! -x "$L/bin/clang" ]; then
    mkdir -p "$A/llvm"
    f=$A/llvm/LLVM-$LLVM_VER-Linux-$LARCH.tar.zst
    if [ ! -f "$f.complete" ]; then
        say "Downloading LLVM $LLVM_VER for $LARCH (about 1.1 GB, once)"
        curl -fL --retry 3 -C - -o "$f" \
            "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/LLVM-$LLVM_VER-Linux-$LARCH.tar.zst" ||
            fail "LLVM download failed (run again to resume)"
        touch "$f.complete"
    fi
    say "Unpacking LLVM (about 12 GB)"
    rm -rf "$A/llvm/x"; mkdir -p "$A/llvm/x"
    zstd -d --long=31 -c "$f" | tar -x -C "$A/llvm/x"   # the archive uses a 1 GiB window
    top=$(ls "$A/llvm/x")
    [ -x "$A/llvm/x/$top/bin/clang" ] || fail "unexpected LLVM archive layout ($top)"
    mv "$A/llvm/x/$top" "$L"
    rmdir "$A/llvm/x"
    rm -f "$f" "$f.complete"
fi
export PATH=$L/bin:$PATH

# Ubuntu 22.04 arm64 sysroot (glibc 2.35, libgcc 12): what libbbaot.so is linked against.
if [ ! -f "$SYSROOT/.complete" ]; then
    say "Fetching an Ubuntu 22.04 arm64 sysroot for the link (glibc 2.35; once)"
    rm -rf "$SYSROOT"
    python3 -I "$TOOL/fetch-sysroot.py" "$SYSROOT" --arch arm64 > "$A/sysroot.log" 2>&1 ||
        { tail -20 "$A/sysroot.log" >&2; fail "fetch-sysroot.py failed (log: $A/sysroot.log in the volume)"; }
    [ -e "$SYSROOT/usr/lib/aarch64-linux-gnu/libm.so.6" ] || [ -e "$SYSROOT/lib/aarch64-linux-gnu/libm.so.6" ] ||
        fail "the sysroot has no libm.so.6"
    [ -f "$GCC_DIR/crtbeginS.o" ] || fail "the sysroot has no GCC 12 runtime ($GCC_DIR)"
    touch "$SYSROOT/.complete"
fi

# FEX: a copy of the port's tree without its test binaries (bb-recomp's CMake builds in it).
if [ ! -f "$A/fex-src/.complete" ]; then
    say "Copying FEX from the port's third_party/FEX (without its test binaries; once)"
    rm -rf "$A/fex-src"; mkdir -p "$A/fex-src"
    tar -C "$FEX" --exclude=.git --exclude='./External/fex-*-bins' -cf - . | tar -C "$A/fex-src" -xf -
    touch "$A/fex-src/.complete"
fi

# device-tool: bb-recomp for the handheld (static aarch64 glibc, the tool's arm64/build.sh aarch64, run on a copy of its
# sources in the volume, with this volume's LLVM, sysroot and FEX copy where that script looks for them) and its bundle.
if [ "$mode" = device-tool ]; then
    ARM=$A/llvm/LLVM-$LLVM_VER-Linux-ARM64
    if [ ! -f "$ARM/lib/cmake/llvm/LLVMConfig.cmake" ]; then   # x86-64 hosts: the AArch64 libraries the static tool links
        f=$A/llvm/LLVM-$LLVM_VER-Linux-ARM64.tar.zst
        if [ ! -f "$f.complete" ]; then
            say "Downloading LLVM $LLVM_VER for ARM64 (about 1.1 GB, once): the libraries of the handheld's bb-recomp"
            curl -fL --retry 3 -C - -o "$f" \
                "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/LLVM-$LLVM_VER-Linux-ARM64.tar.zst" ||
                fail "LLVM download failed (run again to resume)"
            touch "$f.complete"
        fi
        rm -rf "$A/llvm/xa"; mkdir -p "$A/llvm/xa"
        zstd -d --long=31 -c "$f" | tar -x -C "$A/llvm/xa"
        top=$(ls "$A/llvm/xa")
        mv "$A/llvm/xa/$top" "$ARM"; rmdir "$A/llvm/xa"; rm -f "$f" "$f.complete"
    fi
    T=$A/tool-src
    say "Copying the Rebuild Tool's sources into the volume (without .git, out and .local-deps)"
    mkdir -p "$T"
    tar -C "$TOOL" --exclude=./.git --exclude=./out --exclude=./.local-deps -cf - . | tar -C "$T" -xf -
    # Where arm64/build.sh looks: the host compiler as "the x86-64 LLVM" (on an arm64 host the AArch64 LLVM itself: it only
    # runs clang, lld and llvm-ar from it), zlib and zstd sources, the AArch64 LLVM and the jammy sysroot in its .local-deps.
    X=$A/x86deps
    mkdir -p "$X/llvm/x" "$X/deps/dl" "$T/.local-deps/llvm-arm64/x"
    ln -sfn "$L" "$X/llvm/x/LLVM-$LLVM_VER-Linux-X64"
    ln -sfn "$ARM" "$T/.local-deps/llvm-arm64/x/LLVM-$LLVM_VER-Linux-ARM64"
    ln -sfn "$SYSROOT" "$T/.local-deps/sysroot-jammy-arm64"
    [ -f "$X/deps/dl/zlib-1.3.1.tar.xz" ] || curl -fL --retry 3 -o "$X/deps/dl/zlib-1.3.1.tar.xz" \
        https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.xz || fail "zlib download failed"
    [ -f "$X/deps/dl/zstd-1.5.7.tar.gz" ] || curl -fL --retry 3 -o "$X/deps/dl/zstd-1.5.7.tar.gz" \
        https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz || fail "zstd download failed"
    say "Building bb-recomp for aarch64 Linux, static (arm64/build.sh aarch64, $JOBS jobs; the first build takes 10 to 30 minutes)"
    if ! (cd "$T" && BB_X86_DEPS="$X" BB_FEX_TREE="$A/fex-src" BB_JOBS="$JOBS" bash arm64/build.sh aarch64) > "$A/device-tool-build.log" 2>&1; then
        grep -v '^\[[0-9]*/[0-9]*\] ' "$A/device-tool-build.log" | tail -40 >&2
        fail "building the handheld's bb-recomp failed (log: $A/device-tool-build.log in the volume; 'build_aot_docker.sh shell' to look)"
    fi
    exe=$T/out/arm64/bb-recomp
    [ -x "$exe" ] || fail "arm64/build.sh made no $exe"
    version=$(sed -n 's/^#define BBR_TOOL_VERSION "\(.*\)"/\1/p' "$TOOL/src/version.h" | head -n 1)
    P=$A/device-tool
    rm -rf "$P"; mkdir -p "$P/bin" "$P/share/bb-recomp/variants" "$P/licenses"
    llvm-strip -o "$P/bin/bb-recomp" "$exe"
    # The bundle the tool's in-process link reads (src/bundle.cpp): the Android names, glibc contents.
    S=$P/share/bb-recomp
    CL=(clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" --gcc-install-dir="$GCC_DIR")
    "${CL[@]}" -O2 -fPIC -fvisibility=hidden -fno-stack-protector -ffp-contract=off \
        -c "$TOOL/helpers/bbr_x87_all.c" -o "$S/helpers-aarch64.o" || fail "compiling the x87 helpers failed"
    "${CL[@]}" -fuse-ld=lld -shared -nostdlib -Wl,-soname,libbbcpu.so -x c /dev/null -o "$S/libbbcpu-stub.so" ||
        fail "making the libbbcpu.so stub failed"
    libm=$(find "$SYSROOT" -name libm.so.6 | head -n 1)
    [ -n "$libm" ] || fail "the sysroot has no libm.so.6"
    cp -L "$libm" "$S/libm.so"   # SONAME libm.so.6: the library's DT_NEEDED, with glibc's symbol versions
    builtins=$(find "$ARM/lib/clang" -path '*aarch64*' -name 'libclang_rt.builtins*.a' | head -n 1)
    [ -n "$builtins" ] || fail "no AArch64 compiler-rt builtins in $ARM/lib/clang"
    cp "$builtins" "$S/libclang_rt.builtins-aarch64-android.a"
    cp "$TOOL"/variants/*.txt "$S/variants/"
    cp "$TOOL/LICENSE" "$TOOL/NOTICES" "$TOOL/ADDITIONAL_TERMS.md" "$P/licenses/"
    cp "$TOOL/helpers/NOTICE" "$P/licenses/helpers-NOTICE"
    {
        echo "bb-recomp $version for aarch64 Linux (a fully static executable: any distribution, glibc not needed)."
        echo
        echo "The Bloodborne Android Rebuild Tool by ducvd89, https://github.com/ducvd89/Bloodborne-android-rebuild-tool"
        echo "(GPL-3.0-or-later with the additional terms in licenses/ADDITIONAL_TERMS.md), built unmodified from its"
        echo "sources by bbport's build_aot_docker.sh device-tool with the tool's own arm64/build.sh aarch64. The"
        echo "share/bb-recomp files are bbport's: the tool's link inputs made for glibc (x87 helpers, a libbbcpu.so stub,"
        echo "Ubuntu 22.04's libm.so.6, LLVM's compiler-rt builtins) and the tool's variant catalog."
        echo
        echo "Copy this folder to the handheld's data folder as aot-tool, e.g."
        echo "    scp -r aot-tool <user>@<handheld>:.local/share/bbport/"
        echo "then in the launcher: Game -> Native CPU -> Build. It writes the game's code compiled for the device into"
        echo "~/.local/share/bbport/aot/: that file is the game's own code: never share it."
        echo
        echo "made:      $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "LLVM:      $LLVM_VER"
        echo "catalog:   $(cat "$S"/variants/*.txt | sha256sum | cut -c1-16)"
    } > "$P/README.txt"
    echo "$version" > "$P/VERSION"
    rm -rf "$OUT/bin" "$OUT/share" "$OUT/licenses"
    cp -a "$P/." "$OUT/"
    say "The handheld's bb-recomp $version: $(du -h "$OUT/bin/bb-recomp" | cut -f1), in the output folder"
    exit 0
fi

# bb-recomp, unmodified (FEXCore's front end + LLVM). Incremental after the first build.
say "Building bb-recomp ($JOBS jobs; the first build takes 10 to 30 minutes)"
if [ ! -f "$B/build.ninja" ]; then
    cmake -S "$TOOL" -B "$B" -G Ninja -DFEX_SOURCE_DIR="$A/fex-src" -DBB_RECOMP_LLVM_DIR="$L/lib/cmake/llvm" \
        -DBB_RECOMP_CC="$L/bin/clang" -DBB_RECOMP_CXX="$L/bin/clang++" -DBB_RECOMP_MARCH="$MARCH" \
        -DBB_RECOMP_JOBS="$JOBS" ${DEPS_PREFIX:+-DBB_RECOMP_DEPS_PREFIX="$DEPS_PREFIX"} > "$A/tool-configure.log" 2>&1 ||
        { tail -30 "$A/tool-configure.log" >&2; rm -rf "$B"; fail "configuring bb-recomp failed"; }
fi
if ! cmake --build "$B" > "$A/tool-build.log" 2>&1; then
    grep -v '^\[[0-9]*/[0-9]*\] ' "$A/tool-build.log" | tail -40 >&2
    fail "building bb-recomp failed (full log: $A/tool-build.log in the volume; 'build_aot_docker.sh shell' to look)"
fi
[ -x "$B/bb-recomp" ] || fail "the build made no bb-recomp"
tool_version=$("$B/bb-recomp" --version 2>/dev/null | head -n 1 || true)
say "${tool_version:-bb-recomp} is ready"
[ "$mode" = tool ] && exit 0

# The game image.
mkdir -p "$W" "$A/bundle"
rm -rf "$W/image" "$W/libbbaot.so"
if [ -n "$BOOT_FILE" ]; then
    boot=$BOOT_FILE
    boot_from="copied from bbport (--boot)"
else
    say "Making boot-linked.bin from the game folder with the port's scripts"
    mkdir -p "$W/image"
    for s in prepare link_libc link_modules; do
        PYTHONDONTWRITEBYTECODE=1 python3 "$FORK/scripts/$s.py" "$GAME" --out "$W/image" || fail "scripts/$s.py failed"
    done
    boot=$W/image/boot-linked.bin
    boot_from="made here from --game with the port's scripts"
fi
[ "$(head -c 8 "$boot")" = BBPROBE5 ] || fail "$boot is not a BBPROBE5 image"
boot_sha=$(sha256sum "$boot" | cut -d' ' -f1)

# The x87 helpers (plain C) for glibc: bb-recomp copies this object from its bundle folder in
# place of its Android NDK compile, and with --obj it links nothing itself.
CL=(clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" --gcc-install-dir="$GCC_DIR")
"${CL[@]}" -O2 -fPIC -fvisibility=hidden -fno-stack-protector -ffp-contract=off \
    -c "$TOOL/helpers/bbr_x87_all.c" -o "$A/bundle/helpers-aarch64.o" || fail "compiling the x87 helpers failed"

# Objects of an earlier run for the same image, options and tool are kept: then only the link runs.
variants=()
variants_note="none (BB_AOT_VARIANTS=0)"
if [ "$AOT_VARIANTS" = 1 ]; then
    ls "$TOOL"/variants/*.txt >/dev/null 2>&1 || fail "the tool has no variant catalog ($TOOL/variants/*.txt)"
    variants=(--variants "$TOOL/variants")
    variants_note="the tool's variants/ ($(cat "$TOOL"/variants/*.txt | sha256sum | cut -c1-16))"
fi
key="$boot_sha -O$AOT_OPT --host $HOST_PROFILE ${tool_version:-bb-recomp} variants: $variants_note"
if [ -f "$W/obj/.key" ] && [ "$(cat "$W/obj/.key")" = "$key" ] && [ -f "$W/compile.txt" ]; then
    say "The objects of this image and these options are compiled already: linking only"
else
    rm -rf "$W/obj" "$W/compile.txt" "$W/bb-recomp.log"; mkdir -p "$W/obj"
    say "Compiling the game's code: bb-recomp -O$AOT_OPT --host $HOST_PROFILE, 64 shards${AOT_JOBS:+, $AOT_JOBS threads} (minutes)"
    args=(--boot "$boot" --game-dir "$GAME" --obj "$W/obj/libbbaot.o" --bundle-dir "$A/bundle"
          --shards 64 "-O$AOT_OPT" --host "$HOST_PROFILE" --stats "$W/compile.txt" --no-nice ${variants[@]+"${variants[@]}"})
    if [ -n "$AOT_JOBS" ]; then args+=(--jobs "$AOT_JOBS"); fi
    if ! "$B/bb-recomp" "${args[@]}" 2>&1 | tee "$W/bb-recomp.log"; then
        cp "$W/bb-recomp.log" "$OUT/bb-recomp.log" 2>/dev/null || true
        fail "bb-recomp failed (log: bb-recomp.log in the output folder)"
    fi
    echo "$key" > "$W/obj/.key"
fi

say "Linking libbbaot.so for aarch64 Linux (glibc)"
objs=("$W"/obj/*.o)
[ "${#objs[@]}" -ge 3 ] || fail "bb-recomp wrote no objects"
"${CL[@]}" -fuse-ld=lld -shared -Wl,-soname,libbbaot.so -Wl,-Bsymbolic -Wl,-z,noexecstack \
    -Wl,-z,max-page-size=16384 -Wl,--hash-style=gnu -Wl,--build-id=none \
    -o "$W/libbbaot.so" "${objs[@]}" -lm || fail "linking libbbaot.so failed"

# Checks: the table bb-recomp writes, and what the library needs from the runtime and the system.
cat > "$A/elfcheck.py" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 1:
    sys.exit('not a 64-bit little-endian ELF file')
machine, = struct.unpack_from('<H', d, 18)
phoff, shoff = struct.unpack_from('<QQ', d, 32)
phentsize, phnum, shentsize, shnum = struct.unpack_from('<HHHH', d, 54)
loads = []
for i in range(phnum):
    t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
    if t == 1:
        loads.append((va, off, fsz, msz))
secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]
def cstr(off):
    return d[off:d.index(b'\0', off)].decode()
def first(kind):
    return next((s for s in secs if s[1] == kind), None)
dynsym = first(11)
if dynsym is None:
    sys.exit('no dynamic symbol table')
dynstr = secs[dynsym[6]]
syms, undef = {}, []
for i in range(1, dynsym[5] // 24):
    nm, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', d, dynsym[4] + i * 24)
    name = cstr(dynstr[4] + nm)
    if shndx == 0:
        undef.append(name)
    else:
        syms[name] = (value, size)
def value(name):
    if name not in syms:
        return None
    va, size = syms[name]
    size = size or 8
    for v, o, fsz, msz in loads:
        if v <= va < v + msz:
            if va + size <= v + fsz:
                return int.from_bytes(d[o + va - v:o + va - v + size], 'little')
            return 0
    return None
needed = []
dyn = first(6)
for i in range(dyn[5] // 16 if dyn else 0):
    tag, val = struct.unpack_from('<qQ', d, dyn[4] + i * 16)
    if tag == 0:
        break
    if tag == 1:
        needed.append(cstr(dynstr[4] + val))
versions = set()
for s in (x for x in secs if x[1] == 0x6ffffffe):
    strtab, off = secs[s[6]][4], s[4]
    while True:
        vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from('<HHIII', d, off)
        a = off + vn_aux
        for _ in range(vn_cnt):
            vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from('<IHHII', d, a)
            versions.add(cstr(strtab + vna_name))
            if not vna_next:
                break
            a += vna_next
        if not vn_next:
            break
        off += vn_next
def vkey(v):
    return tuple(int(x) for x in v.split('_', 1)[1].split('.') if x.isdigit())
glibc = sorted((v for v in versions if v.startswith('GLIBC_')), key=vkey)
flags = value('bbr_abi_flags') or 0
names = ['resume entries', 'dirty check', 'variants', 'hook slots', 'granular entries']
print('machine:          ' + ('AArch64' if machine == 183 else 'e_machine %d (not AArch64)' % machine))
print('ABI version:      %s (cpu/aot accepts 2)' % value('bbr_abi_version'))
print('units:            %s in %s functions' % (value('bbr_unit_count'), value('bbr_func_count')))
img = value('bbr_image_hash')
print('image hash:       %s (FNV-1a 64 of the game image)' % ('%#018x' % img if img is not None else None))
print('ABI flags:        %#x (%s)' % (flags, ', '.join(n for b, n in enumerate(names) if flags >> b & 1) or 'none'))
print('needs libraries:  ' + ', '.join(needed))
print('needs glibc:      ' + (glibc[-1] + ' or newer' if glibc else 'no versioned symbols'))
bbr = sorted(n for n in undef if n.startswith('bbr_'))
other = sorted(n for n in undef if not n.startswith('bbr_'))
print('from the runtime (the guest CPU library must export these, default visibility):')
for n in bbr:
    print('  ' + n)
print('from the system (%d): %s' % (len(other), ' '.join(other)))
missing = [n for n in ('bbr_units', 'bbr_unit_count', 'bbr_abi_version', 'bbr_image_base', 'bbr_image_hash') if n not in syms]
if missing:
    print('MISSING exports: ' + ' '.join(missing))
if machine != 183 or missing:
    sys.exit(1)
PY
python3 "$A/elfcheck.py" "$W/libbbaot.so" > "$W/check.txt" || { cat "$W/check.txt" >&2; fail "libbbaot.so failed its checks"; }

{
    echo "libbbaot.so: the game's x86-64 code compiled ahead of time for aarch64 Linux (glibc)."
    echo "Personal use only: it is the game's code. Never commit, ship or upload it."
    echo
    echo "made:             $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "game image:       sha256 $boot_sha, $boot_from"
    echo "compiler:         ${tool_version:-bb-recomp}, -O$AOT_OPT --host $HOST_PROFILE --shards 64 --obj"
    echo "variant catalog:  $variants_note"
    echo "                  (Bloodborne Android Rebuild Tool by ducvd89, unmodified; FEX-2609.1 front end)"
    echo "LLVM:             $LLVM_VER ($(clang --version | head -n 1))"
    echo "linked against:   Ubuntu 22.04 arm64 sysroot (glibc 2.35), ld.lld, -Bsymbolic"
    echo "size:             $(du -h "$W/libbbaot.so" | cut -f1)"
    cat "$W/check.txt"
} > "$W/libbbaot.txt"

cp "$W/libbbaot.so" "$OUT/.libbbaot.so.part" && mv -f "$OUT/.libbbaot.so.part" "$OUT/libbbaot.so"
cp "$W/libbbaot.txt" "$W/compile.txt" "$OUT/"
if [ -f "$W/bb-recomp.log" ]; then cp "$W/bb-recomp.log" "$OUT/"; fi
echo
cat "$W/libbbaot.txt"
INNER_SCRIPT
)

tty_flags=()
if [ -t 0 ] && [ -t 1 ]; then tty_flags=(-it); fi

# "${arr[@]+"${arr[@]}"}" keeps bash 3.2 happy with an empty array.
docker run --rm ${tty_flags[@]+"${tty_flags[@]}"} --platform "$PLATFORM" \
    "${mounts[@]}" \
    -e A=/aot -e TOOL=/src/tool -e FEX=/src/fex -e FORK=/fork -e GAME=/in/game -e OUT=/out \
    -e BOOT_FILE="$BOOT_FILE" -e LLVM_VER="$LLVM_VER" -e LARCH="$LARCH" -e MARCH="$MARCH" \
    -e JOBS="$JOBS" -e AOT_JOBS="$AOT_JOBS" -e AOT_OPT="$AOT_OPT" -e AOT_VARIANTS="$AOT_VARIANTS" -e HOST_PROFILE="$HOST_PROFILE" \
    -e OWNER="$OWNER" \
    "$IMAGE" bash -c "$INNER" inner "$MODE"

case $MODE in
    aot) echo "==> Done: $OUT/libbbaot.so (report: $OUT/libbbaot.txt)" ;;
    tool) echo "==> Done: bb-recomp is built in the Docker volume $VOLUME." ;;
    device-tool) echo "==> Done: $OUT (copy it to the handheld as ~/.local/share/bbport/aot-tool; README.txt)" ;;
esac

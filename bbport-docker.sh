#!/bin/bash
# bbport-docker.sh: build ARM_bloodborne_pc (Linux aarch64 + FEXCore) inside Docker on an
# Apple Silicon Mac. Uses the project's own Nix environment (shell.nix), pinned to one nixpkgs
# revision so the toolchain doesn't drift.
#
# Usage:  ./bbport-docker.sh [build|test|appimage|shell|clean]
#   build     (default) clone the repo into a Docker volume, run build.sh, copy binaries to ./artifacts
#   test      same, then the project's own unit tests (build.sh --test)
#   appimage  same, then package artifacts/Bloodborne-bbport-aarch64.AppImage like the GitHub
#             release (packaging/appimage.sh; the first run builds the patched Turnip driver)
#   shell   interactive shell inside the build environment, in the repo directory
#   clean   delete the image and the cached volumes (Nix store, source tree)
#
# Settings (environment variables):
#   REPO_URL   repository (default: https://github.com/MaSieS4Fun/ARM_bloodborne_pc.git)
#   REPO_REF   branch, tag or commit to check out (default: the repo's default branch)
#   BB_LTO     ON|OFF link-time optimization (default here: OFF, which is faster and uses far less RAM;
#              upstream defaults to ON)
#   GITHUB_TOKEN  optional GitHub token for Nix downloads, if GitHub's API rate limit hits
#   JOBS       parallel compiler processes (default: picked from Docker's RAM, about 2 GB each).
#              If the compiler still gets "Killed" (out of memory), lower it: JOBS=2 ./bbport-docker.sh
#
# Works with Docker Desktop, OrbStack or colima. Needs ~15 GB free disk on the first run.
# Written for macOS's stock bash 3.2.
set -eo pipefail

NIXPKGS_REV=6510408d147d8c5e6d76043e899e9251756f6a75   # nixos-unstable, 2026-10-10
IMAGE="bbport-builder:${NIXPKGS_REV:0:8}"
NIX_VOLUME=bbport-nix
WORK_VOLUME=bbport-work
REPO_URL=${REPO_URL:-https://github.com/MaSieS4Fun/ARM_bloodborne_pc.git}
REPO_REF=${REPO_REF:-}
BB_LTO=${BB_LTO:-OFF}
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
ARTIFACTS="$HERE/artifacts"
MODE=${1:-build}

die() { echo "ERROR: $*" >&2; exit 1; }

case $MODE in build|test|appimage|shell|clean) ;; *) die "unknown command '$MODE' (build|test|appimage|shell|clean)";; esac

command -v docker >/dev/null || die "docker not found. Install Docker Desktop, OrbStack or colima."
docker info >/dev/null 2>&1 || die "Docker is installed but not running. Start Docker Desktop / OrbStack first."

if [ "$MODE" = clean ]; then
    docker image rm "$IMAGE" 2>/dev/null || true
    docker volume rm "$NIX_VOLUME" "$WORK_VOLUME" 2>/dev/null || true
    echo "Removed $IMAGE and volumes $NIX_VOLUME, $WORK_VOLUME (./artifacts left untouched)."
    exit 0
fi

[ "$(uname -m)" = arm64 ] || echo "WARNING: this Mac is not Apple Silicon; an arm64 container would run emulated (very slow)." >&2

mem=$(docker info --format '{{.MemTotal}}' 2>/dev/null || echo 0)
ncpu=$(docker info --format '{{.NCPU}}' 2>/dev/null || echo 0)
case $mem in ''|*[!0-9]*) mem=0;; esac
case $ncpu in ''|*[!0-9]*) ncpu=0;; esac
mem_mib=$((mem / 1048576))
# Heavy C++ files (shadPS4 core, GCC 16, -g) take up to ~2 GB each; ninja's default
# (cores + 2 jobs) ran a 10-core Mac out of memory.
if [ -z "${JOBS:-}" ]; then
    JOBS=$((mem_mib / 2048))
    if [ "$ncpu" -gt 0 ] && [ "$JOBS" -gt "$ncpu" ]; then JOBS=$ncpu; fi
    if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
fi
case $JOBS in ''|*[!0-9]*|0) die "JOBS must be a positive number";; esac
echo "==> Docker VM: ${ncpu} CPUs, ${mem_mib} MiB RAM; compiling with JOBS=${JOBS}"
if [ "$mem_mib" -lt 7500 ]; then
    echo "WARNING: little RAM for this build. Give Docker 8 GB or more" \
         "(Docker Desktop: Settings > Resources > Memory), or expect a slow build." >&2
fi

# Image: the official Nix image plus a pinned nixpkgs tree (NIX_PATH), so shell.nix's
# `import <nixpkgs>` always resolves to the same package set.
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "==> Building image $IMAGE"
    docker build --platform linux/arm64 -t "$IMAGE" --build-arg NIXPKGS_REV="$NIXPKGS_REV" - <<'DOCKERFILE'
FROM nixos/nix:latest
ARG NIXPKGS_REV
RUN mkdir -p /opt/nixpkgs \
 && curl -fsSL "https://github.com/NixOS/nixpkgs/archive/${NIXPKGS_REV}.tar.gz" \
    | tar -xz --strip-components=1 -C /opt/nixpkgs \
 && (command -v git >/dev/null \
     || nix-env --option sandbox false --option filter-syscalls false -f /opt/nixpkgs -iA git) \
 && git config --global --add safe.directory '*'
ENV NIX_PATH=nixpkgs=/opt/nixpkgs
WORKDIR /work
DOCKERFILE
fi

# Runs inside the container. $1 = mode.
read -r -d '' INNER <<'INNER_SCRIPT' || true
set -euo pipefail
mode=$1; shift
nixopts=(--option sandbox false --option filter-syscalls false)
# For every nix command here: flakes for `nix bundle` (AppImage), no build sandbox inside Docker,
# and derivations built from source (Turnip) limited to $BB_JOBS cores, like the compiler.
export NIX_CONFIG="experimental-features = nix-command flakes
sandbox = false
filter-syscalls = false
max-jobs = 1
cores = ${BB_JOBS:-2}"
# Optional GitHub token (any classic token, no scopes): 5000 API requests/hour instead of 60.
if [ -n "${GITHUB_TOKEN:-}" ]; then
    NIX_CONFIG="$NIX_CONFIG
access-tokens = github.com=${GITHUB_TOKEN}"
fi
command -v git >/dev/null || nix-env "${nixopts[@]}" -f /opt/nixpkgs -iA git
if [ ! -d /work/bbport/.git ]; then
    echo "==> Cloning $REPO_URL (with submodules: FEX and its test data are large)"
    git clone --recursive "$REPO_URL" /work/bbport
fi
cd /work/bbport
if [ -n "${REPO_REF:-}" ]; then
    git fetch --tags origin "$REPO_REF" && git checkout -q FETCH_HEAD
fi
git submodule update --init --recursive
echo "==> Commit: $(git log -1 --format='%h %s')"
echo "==> Kernel page size: $(grep -m1 KernelPageSize /proc/self/smaps | tr -s ' ' | cut -d' ' -f2-) (FEXCore assumes 4 kB)"
if [ "$mode" = shell ]; then
    exec nix-shell "${nixopts[@]}" shell.nix
fi
# Keep build logs next to the artifacts whether the build works or not.
trap 'mkdir -p /artifacts; cp -f out/*.log /artifacts/ 2>/dev/null || true' EXIT
# build.sh calls plain `ninja` (cores + 2 jobs). This wrapper, first in PATH, adds -j $BB_JOBS.
mkdir -p /tmp/bbwrap
cat > /tmp/bbwrap/ninja <<'WRAPPER'
#!/bin/sh
real=$(PATH=${PATH#/tmp/bbwrap:}; export PATH; command -v ninja)
exec "$real" -j "${BB_JOBS:-2}" "$@"
WRAPPER
chmod +x /tmp/bbwrap/ninja
# FEX turns LTO on by default, which needs llvm-ar and LLVM's linker plugin; shell.nix's clang has
# neither ("CMAKE_C_COMPILER_AR-NOTFOUND: command not found"). build.sh configures FEX only when
# out/fex/build.ninja is missing, and CMake keeps existing cache entries, so a pre-seeded
# ENABLE_LTO=OFF turns it off without touching build.sh.
if [ -f out/fex/CMakeCache.txt ] && ! grep -q '^ENABLE_LTO:BOOL=OFF$' out/fex/CMakeCache.txt; then
    echo "==> FEX was configured with LTO: starting its build directory (out/fex) over without it"
    rm -rf out/fex
fi
if [ ! -f out/fex/CMakeCache.txt ]; then
    mkdir -p out/fex && echo 'ENABLE_LTO:BOOL=OFF' > out/fex/CMakeCache.txt
fi
echo "==> Building (BB_LTO=${BB_LTO}, ${BB_JOBS} parallel jobs); the first run downloads the toolchain from cache.nixos.org"
nix-shell "${nixopts[@]}" shell.nix --run 'PATH=/tmp/bbwrap:$PATH bash build.sh'
mkdir -p /artifacts
cp -f out/bb-probe out/bb-gpu-capabilities out/gpu/libbbgpu.so /artifacts/
if [ -f out/fex/libbbcpu.so ]; then cp -f out/fex/libbbcpu.so /artifacts/; fi
echo "==> Artifacts:"
nix-shell "${nixopts[@]}" shell.nix --run \
    'for f in /artifacts/*; do case $f in *.log) continue;; esac; printf "  %-28s %s\n" "$(basename "$f")" "$(readelf -h "$f" | sed -n "s/ *Machine: *//p")"; done'
if [ "$mode" = test ]; then
    # No GPU, display or gamepad in the container: tests that need them may fail.
    echo "==> Running the project's tests (build.sh --test)"
    nix-shell "${nixopts[@]}" shell.nix --run 'PATH=/tmp/bbwrap:$PATH bash build.sh --test' || { echo "==> Some tests failed (see above)"; exit 3; }
    echo "==> Tests passed"
fi
if [ "$mode" = appimage ]; then
    # packaging/default.nix copies fsr4_shaders/ into the package, so the FSR 4 assets come first
    # (tools/fetch_fsr4_assets.sh; its optimizer step runs perl, which shell.nix lacks).
    echo "==> FSR 4 assets (downloaded from GitHub; the package includes them)"
    perl_bin=$(nix-build '<nixpkgs>' -A perl --no-out-link)/bin
    nix-shell shell.nix --run "PATH=$perl_bin:\$PATH bash tools/fetch_fsr4_assets.sh"
    # appimage.sh names the bundler without a commit (github:ralismark/nix-appimage), so Nix asks
    # GitHub's API for its latest commit: 60 anonymous requests per hour per IP, often used up on
    # shared IPs. A copy of the script with the bundler pinned to a commit skips that API call
    # (pinned inputs download as plain archives).
    bundler_rev=7946addbc0d97e358a6d7aefe5e82310f0fe6b18   # ralismark/nix-appimage, 2025-09-15
    # Plain bash: the Nix base image has no sed/cmp outside nix-shell.
    original=$(cat packaging/appimage.sh)
    pinned=${original//github:ralismark\/nix-appimage /github:ralismark/nix-appimage/${bundler_rev} }
    printf '%s\n' "$pinned" > packaging/.appimage-pinned.sh
    if [ "$pinned" = "$original" ]; then
        echo "WARNING: appimage.sh changed upstream; running it unpinned" >&2
    fi
    echo "==> Packaging the AppImage (nix bundle). The first run builds the patched Turnip (Mesa) driver from source"
    nix-shell shell.nix --run 'BB_NIXPKGS=/opt/nixpkgs bash packaging/.appimage-pinned.sh'
    cp -f dist/Bloodborne-bbport-aarch64.AppImage /artifacts/
fi
INNER_SCRIPT

tty_flags=()
if [ -t 0 ] && [ -t 1 ]; then tty_flags=(-it); fi
mkdir -p "$ARTIFACTS"

# "${arr[@]+"${arr[@]}"}" keeps bash 3.2 happy with an empty array.
docker run --rm ${tty_flags[@]+"${tty_flags[@]}"} \
    --platform linux/arm64 \
    -v "$NIX_VOLUME":/nix -v "$WORK_VOLUME":/work -v "$ARTIFACTS":/artifacts \
    -e REPO_URL="$REPO_URL" -e REPO_REF="$REPO_REF" -e BB_LTO="$BB_LTO" -e BB_JOBS="$JOBS" \
    -e GITHUB_TOKEN="${GITHUB_TOKEN:-}" \
    -e SDL_VIDEO_DRIVER=dummy -e SDL_AUDIO_DRIVER=dummy \
    "$IMAGE" bash -c "$INNER" inner "$MODE"

case $MODE in
    shell) ;;
    appimage) echo "==> Done. $ARTIFACTS/Bloodborne-bbport-aarch64.AppImage (for ARM64 Linux devices, not macOS)." ;;
    *) echo "==> Done. Linux aarch64 binaries in $ARTIFACTS (they run on Linux ARM64, not on macOS)." ;;
esac

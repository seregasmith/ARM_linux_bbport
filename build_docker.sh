#!/bin/bash
# build_docker.sh: build this repository (ARM_bloodborne_pc) in a Linux Docker container on any
# host, for either target architecture:
#   aarch64  ARM64 Linux devices (the game's x86-64 code runs in FEXCore)
#            -> dist/Bloodborne-bbport-aarch64.AppImage
#   x86_64   x86-64 Linux PCs and the Steam Deck (the game's code runs directly on the CPU, no FEX)
#            -> dist/Bloodborne-bbport-x86_64.AppImage
# Hosts: macOS on Apple Silicon or Intel (Docker Desktop, OrbStack or colima), Linux on x86-64 or
# arm64 (Docker Engine). A target that matches Docker's own architecture builds natively; the
# other one runs emulated (see "Emulation" below).
# Usage:  ./build_docker.sh [--arch aarch64|x86_64] [build|test|appimage|shell|clean]
#   --arch    target architecture (or BB_ARCH=...; arm64 and amd64 work too). Default: Docker's own
#             architecture, which needs no emulation (aarch64 on Apple Silicon, x86_64 on a PC)
#   build     (default) bash build.sh: bb-probe, gpu/libbbgpu.so, and on aarch64 fex/libbbcpu.so
#   test      build, then the project's own unit tests (build.sh --test)
#   appimage  build, then dist/Bloodborne-bbport-<arch>.AppImage like the GitHub release
#             (packaging/appimage.sh; for aarch64 the first run compiles the patched Turnip driver)
#   shell     interactive shell in that target's build environment, in the source tree
#   clean     delete that target's Docker image and its own Nix store volume (not one named by
#             BB_NIX_VOLUME; the source tree is untouched)
#
# Build directories (build.sh always writes out/ and pgo/, so each target gets its own):
#   aarch64  out/ and pgo/ of the tree
#   x86_64   build-x86_64/out and build-x86_64/pgo, mounted over out/ and pgo/ in the container
#            (.gitignore's out/ and pgo/ patterns cover them)
#   Both targets share dist/ (the AppImage names carry the arch), fsr4_shaders/ and
#   packaging/runtime-paths.nix (rewritten by every appimage run): one build of a tree at a time.
#
# The script runs no git commands on the repository. Check out the submodules yourself first:
#   git submodule update --init --recursive
# (third_party/FEX is needed for aarch64 only.) build.sh applies gpu/patches/fsr-vulkan/*.patch
# inside the fsr-vulkan submodule (git status then shows that submodule as modified).
# Linux host with Docker Engine: files made in the container belong to root there, so at the end
# the script gives the root-owned files in what the build writes back to you (out/, pgo/, dist/,
# fsr4_shaders/, packaging/runtime-paths.nix, the fsr-vulkan submodule and its .git/modules
# directory, which build.sh's own git commands can touch). Ownership only, never contents. Not
# with rootless Docker, Podman or Docker Desktop, which map the container's root to you already.
#
# Emulation (a target other than Docker's architecture) works, but is slower:
#   Apple Silicon -> x86_64: Rosetta. Docker Desktop: Settings > General > "Use Rosetta for
#     x86_64/amd64 emulation on Apple Silicon" (without it QEMU: much slower). OrbStack: built in.
#     colima: colima start --vm-type vz --vz-rosetta
#   Intel/AMD -> aarch64: QEMU, many times slower than native; for appimage, the Turnip (Mesa)
#     compile alone can take hours. Docker Desktop and OrbStack include QEMU. Linux: register it
#     once per boot: docker run --privileged --rm tonistiigi/binfmt --install arm64
#     (or install the distribution's qemu-user-static package).
#   Some tests (test) can fail under emulation even though the build is fine.
#
# Settings (environment variables):
#   BB_ARCH       target, as --arch (the option wins)
#   BB_SRC        source tree to build (default: the directory this script is in)
#   BB_LTO        ON|OFF link-time optimization of the GPU library (default here: OFF, faster and far
#                 less RAM; upstream defaults to ON). FEX is always built without LTO.
#   JOBS          parallel compiler processes (default: picked from Docker's RAM, about 2 GB each).
#                 If the compiler still gets "Killed" (out of memory): JOBS=2 ./build_docker.sh
#   BB_NIX_VOLUME an existing Docker volume to use as the target's Nix store instead of this
#                 script's own (bbport-nix for aarch64, bbport-nix-x86_64). The DriveClub,
#                 Gravity Rush 2 and inFAMOUS build_docker.sh pin the same nixpkgs, so their volumes
#                 can be shared to skip downloading the toolchain again (and, for appimage,
#                 building the same Turnip again: the derivation is the same):
#                   BB_NIX_VOLUME=dc-nix ./build_docker.sh                         (aarch64)
#                   BB_NIX_VOLUME=gr2-nix ./build_docker.sh                        (aarch64)
#                   BB_NIX_VOLUME=iss-nix-x86_64 ./build_docker.sh --arch x86_64
#                 The volume must exist already (docker volume ls) and hold the target
#                 architecture's Nix: the script stops instead of creating an empty one. `clean`
#                 deletes only this script's own bbport-nix* volumes.
#   GITHUB_TOKEN  optional GitHub token for Nix downloads, if GitHub's API rate limit hits
#   BB_TURNIP_SRC aarch64 only: a Mesa source tree on this machine for the AppImage's Turnip instead
#                 of nixpkgs' Mesa release (packaging/default.nix), e.g. the turnip/gen8 branch:
#                   git clone --depth 1 -b turnip/gen8 https://github.com/whitebelyash/mesa-unified
#                 Mounted read-only into the container; only the appimage step uses it. (x86_64
#                 packages nixpkgs' Mesa with the radeon and intel Vulkan drivers, no Turnip.)
#   BB_TURNIP_PATCHES  0: build that tree without the port's Turnip patches (default: applied
#                 where they apply, Adreno 830 chip IDs added where missing)
#
# Written for macOS's stock bash 3.2 and any newer bash.
set -eo pipefail

NIXPKGS_REV=6510408d147d8c5e6d76043e899e9251756f6a75   # nixos-unstable, 2026-10-10
BB_LTO=${BB_LTO:-OFF}
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
SRC=${BB_SRC:-$HERE}
ARCH=${BB_ARCH:-}
MODE=build

die() { echo "ERROR: $*" >&2; exit 1; }
usage() { echo "Usage: $0 [--arch aarch64|x86_64] [build|test|appimage|shell|clean]" >&2; exit 2; }
# aarch64 | x86_64 | "" for the spellings uname, Docker and people use.
norm_arch() {
    case $1 in
        aarch64|arm64|armv8*) echo aarch64 ;;
        x86_64|amd64|x64|x86-64|x86) echo x86_64 ;;
        *) echo "" ;;
    esac
}

while [ $# -gt 0 ]; do
    case $1 in
        --arch) [ $# -ge 2 ] || usage; ARCH=$2; shift 2 ;;
        --arch=*) ARCH=${1#--arch=}; shift ;;
        -h|--help) awk 'NR > 1 && /^set -eo/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
        build|test|appimage|shell|clean) MODE=$1; shift ;;
        *) echo "ERROR: unknown argument '$1'" >&2; usage ;;
    esac
done

command -v docker >/dev/null || die "docker not found. Install Docker Desktop, OrbStack, colima or Docker Engine."
docker info >/dev/null 2>&1 || die "Docker is installed but not running (or this user may not use it)."

DOCKER_ARCH=$(norm_arch "$(docker info --format '{{.Architecture}}' 2>/dev/null)")
[ -n "$DOCKER_ARCH" ] || die "cannot tell Docker's CPU architecture (docker info --format '{{.Architecture}}')"
if [ -z "$ARCH" ]; then
    ARCH=$DOCKER_ARCH
else
    wanted=$(norm_arch "$ARCH")
    [ -n "$wanted" ] || die "unknown target architecture '$ARCH' (aarch64 or x86_64)"
    ARCH=$wanted
fi
case $ARCH in
    aarch64) PLATFORM=linux/arm64 ;;
    x86_64)  PLATFORM=linux/amd64 ;;
esac
DOCKER_PLATFORM_ARCH=${PLATFORM#linux/}   # arm64 | amd64, as `docker image inspect` reports it

# Per target: Docker image, Nix store volume (a store holds one architecture's Nix itself, so they
# can't share one), and build directory.
if [ "$ARCH" = aarch64 ]; then
    IMAGE="bbport-builder:${NIXPKGS_REV:0:8}"
    OWN_VOLUME=bbport-nix
    BUILD_DIR=""
else
    IMAGE="bbport-builder:${NIXPKGS_REV:0:8}-$ARCH"
    OWN_VOLUME=bbport-nix-$ARCH
    BUILD_DIR=build-$ARCH
fi
NIX_VOLUME=${BB_NIX_VOLUME:-$OWN_VOLUME}
OUT_DESC=${BUILD_DIR:+$BUILD_DIR/}out

if [ "$MODE" = clean ]; then
    docker image rm "$IMAGE" 2>/dev/null || true
    echo "Removed the $ARCH build image $IMAGE."
    if [ "$NIX_VOLUME" = "$OWN_VOLUME" ]; then
        docker volume rm "$NIX_VOLUME" 2>/dev/null || true
        echo "Removed the $ARCH Nix store volume $NIX_VOLUME (the next build downloads it again)."
    else
        echo "Kept the Nix store volume $NIX_VOLUME: it is not this script's own ($OWN_VOLUME)."
    fi
    echo "The source tree was not touched (this target's build outputs: $OUT_DESC/, dist/)."
    exit 0
fi
# A volume named by BB_NIX_VOLUME is used only if it exists: `docker run -v` would otherwise
# create an empty one under that name and download everything into it.
if [ "$NIX_VOLUME" != "$OWN_VOLUME" ] && ! docker volume inspect "$NIX_VOLUME" >/dev/null 2>&1; then
    echo "ERROR: BB_NIX_VOLUME=$NIX_VOLUME: no such Docker volume. Volumes here:" >&2
    docker volume ls --format '  {{.Name}}' >&2 || true
    die "name an existing one, or unset BB_NIX_VOLUME to use this script's own ($OWN_VOLUME)"
fi

# The source tree: the project's own files must be there, and the submodules checked out.
SRC=$(cd -- "$SRC" 2>/dev/null && pwd) || die "BB_SRC directory not found"
[ -f "$SRC/build.sh" ] && [ -f "$SRC/shell.nix" ] && [ -d "$SRC/gpu" ] ||
    die "$SRC is not the ARM_bloodborne_pc source tree (no build.sh/shell.nix/gpu). Put this script in its root or set BB_SRC."
is_empty() { [ -z "$(ls -A "$1" 2>/dev/null)" ]; }
submodule_paths() { # paths from a .gitmodules file, without git
    while read -r key _ value; do
        if [ "$key" = path ]; then echo "$value"; fi
    done < "$1"
}
missing=""
for p in $(submodule_paths "$SRC/.gitmodules"); do
    if [ "$ARCH" != aarch64 ] && [ "$p" = third_party/FEX ]; then continue; fi   # FEX: aarch64 only
    if is_empty "$SRC/$p"; then missing="$missing $p"; fi
done
if [ "$ARCH" = aarch64 ] && [ -f "$SRC/third_party/FEX/.gitmodules" ]; then
    for p in $(submodule_paths "$SRC/third_party/FEX/.gitmodules"); do
        case $p in *fex-*-bins) continue ;; esac   # FEX's test data: not needed to build
        if is_empty "$SRC/third_party/FEX/$p"; then missing="$missing third_party/FEX/$p"; fi
    done
fi
if [ -n "$missing" ]; then
    echo "ERROR: submodules are not checked out in $SRC:" >&2
    for p in $missing; do echo "  $p" >&2; done
    echo "This script runs no git commands on your repository. Check them out yourself:" >&2
    echo "  cd \"$SRC\" && git submodule update --init --recursive" >&2
    exit 1
fi

emulation_help() {
    echo "How to enable it:" >&2
    if [ "$(uname -s)" = Darwin ]; then
        echo "  Docker Desktop: Settings > General > \"Use Rosetta for x86_64/amd64 emulation on Apple Silicon\"" >&2
        echo "                  (arm64 containers on an Intel Mac: built in, QEMU)" >&2
        echo "  OrbStack:       built in" >&2
        echo "  colima:         colima start --vm-type vz --vz-rosetta   (Apple Silicon, x86_64 target)" >&2
    else
        echo "  Register QEMU for $PLATFORM (lasts until reboot):" >&2
        echo "    docker run --privileged --rm tonistiigi/binfmt --install $DOCKER_PLATFORM_ARCH" >&2
        echo "  or install your distribution's qemu-user-static package (binfmt support included)." >&2
    fi
}

echo "==> Target: $ARCH ($PLATFORM); Docker runs on $DOCKER_ARCH"
if [ "$ARCH" != "$DOCKER_ARCH" ]; then
    echo "==> Emulated build (Rosetta or QEMU): slower than a native $ARCH build. Checking that $PLATFORM containers run"
    got=$(docker run --rm --platform "$PLATFORM" nixos/nix:latest uname -m 2>&1 | tail -n 1) || true
    if [ "$(norm_arch "$got")" != "$ARCH" ]; then
        echo "ERROR: this Docker can't run $PLATFORM containers (test output: ${got:-nothing})." >&2
        emulation_help
        exit 1
    fi
    if [ "$ARCH" = aarch64 ]; then
        echo "    aarch64 under QEMU is many times slower than native; the appimage step's Turnip compile can take hours." >&2
    fi
fi

mem=$(docker info --format '{{.MemTotal}}' 2>/dev/null || echo 0)
ncpu=$(docker info --format '{{.NCPU}}' 2>/dev/null || echo 0)
case $mem in ''|*[!0-9]*) mem=0 ;; esac
case $ncpu in ''|*[!0-9]*) ncpu=0 ;; esac
mem_mib=$((mem / 1048576))
# Heavy C++ files (shadPS4 core, GCC 16, -g) take up to ~2 GB each; ninja's default
# (cores + 2 jobs) ran a 10-core Mac out of memory.
if [ -z "${JOBS:-}" ]; then
    JOBS=$((mem_mib / 2048))
    if [ "$ncpu" -gt 0 ] && [ "$JOBS" -gt "$ncpu" ]; then JOBS=$ncpu; fi
    if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
fi
case $JOBS in ''|*[!0-9]*|0) die "JOBS must be a positive number" ;; esac
echo "==> Source tree: $SRC (build directory: $OUT_DESC/)"
# A Mesa tree for Turnip (aarch64 appimage step): mounted read-only at /turnip-src in the container.
turnip_flags=()
if [ -n "${BB_TURNIP_SRC:-}" ]; then
    if [ "$ARCH" != aarch64 ]; then
        echo "WARNING: BB_TURNIP_SRC is for aarch64 builds only (Turnip is the Adreno driver); ignored for $ARCH." >&2
    else
        TURNIP_SRC=$(cd -- "$BB_TURNIP_SRC" 2>/dev/null && pwd) || die "BB_TURNIP_SRC directory not found: $BB_TURNIP_SRC"
        [ -f "$TURNIP_SRC/VERSION" ] && [ -d "$TURNIP_SRC/src/freedreno/vulkan" ] ||
            die "BB_TURNIP_SRC is not a Mesa source tree (no VERSION or src/freedreno/vulkan): $TURNIP_SRC"
        turnip_flags=(-v "$TURNIP_SRC":/turnip-src:ro -e BB_TURNIP_SRC=/turnip-src -e BB_TURNIP_PATCHES="${BB_TURNIP_PATCHES:-}")
        echo "==> Turnip: Mesa $(cat "$TURNIP_SRC/VERSION") from $TURNIP_SRC" \
             "(port's patches: $([ "${BB_TURNIP_PATCHES:-}" = 0 ] && echo off || echo where they apply))"
        [ "$MODE" = appimage ] || echo "    (used by the appimage step only)"
    fi
fi
echo "==> Docker VM: ${ncpu} CPUs, ${mem_mib} MiB RAM; compiling with JOBS=${JOBS}"
if [ "$NIX_VOLUME" != "$OWN_VOLUME" ]; then
    echo "==> Nix store: shared volume $NIX_VOLUME (BB_NIX_VOLUME)"
fi
if [ "$mem_mib" -lt 7500 ]; then
    echo "WARNING: little RAM for this build. Give Docker 8 GB or more" \
         "(Docker Desktop: Settings > Resources > Memory), or expect a slow build." >&2
fi

# Linux host, rootful Docker Engine: the build's files belong to root on the host; the container
# gives them back to this user at the end (BB_OWNER). sudo ./build_docker.sh: to the sudo user.
OWNER=""
if [ "$(uname -s)" = Linux ]; then
    case "$(docker --version 2>/dev/null) $(docker info --format '{{.OperatingSystem}} {{.SecurityOptions}}' 2>/dev/null)" in
        # Podman, rootless Docker, Docker Desktop for Linux: they map the container's root to this
        # user already (a chown in the container would give the files to a subordinate ID).
        *odman*|*rootless*|*"Docker Desktop"*) ;;
        *) OWNER="${SUDO_UID:-$(id -u)}:${SUDO_GID:-$(id -g)}" ;;
    esac
fi

# Mount points that exist before Docker needs them (Docker Engine would create missing ones as
# root). x86_64: its own out/ and pgo/ go over the tree's.
mkdir -p "$SRC/out" "$SRC/pgo"
build_mounts=()
if [ -n "$BUILD_DIR" ]; then
    mkdir -p "$SRC/$BUILD_DIR/out" "$SRC/$BUILD_DIR/pgo"
    build_mounts=(-v "$SRC/$BUILD_DIR/out":/src/out -v "$SRC/$BUILD_DIR/pgo":/src/pgo)
fi

# Image: the official Nix image plus a pinned nixpkgs tree (NIX_PATH), so shell.nix's
# `import <nixpkgs>` always resolves to the same package set. git is there for build.sh's own
# `git apply` of the FSR-Vulkan patches; safe.directory lets it work on the mounted tree.
image_arch=$(docker image inspect --format '{{.Architecture}}' "$IMAGE" 2>/dev/null || true)
if [ "$image_arch" != "$DOCKER_PLATFORM_ARCH" ]; then
    if [ -n "$image_arch" ]; then echo "==> $IMAGE is a $image_arch image: building it again for $DOCKER_PLATFORM_ARCH"; fi
    echo "==> Building image $IMAGE ($PLATFORM)"
    docker build --platform "$PLATFORM" -t "$IMAGE" --build-arg NIXPKGS_REV="$NIXPKGS_REV" - <<'DOCKERFILE'
FROM nixos/nix:latest
ARG NIXPKGS_REV
RUN mkdir -p /opt/nixpkgs \
 && curl -fsSL "https://github.com/NixOS/nixpkgs/archive/${NIXPKGS_REV}.tar.gz" \
    | tar -xz --strip-components=1 -C /opt/nixpkgs \
 && (command -v git >/dev/null \
     || nix-env --option sandbox false --option filter-syscalls false --option build-users-group '' \
            -f /opt/nixpkgs -iA git) \
 && git config --global --add safe.directory '*'
ENV NIX_PATH=nixpkgs=/opt/nixpkgs
WORKDIR /src
DOCKERFILE
fi

# A shared volume hides the image's own /nix: its Nix runs every command here. One made for the
# other architecture would run there too (natively on an arm64 Docker VM) and build for it.
if [ "$NIX_VOLUME" != "$OWN_VOLUME" ]; then
    vol_system=$(docker run --rm --platform "$PLATFORM" -v "$NIX_VOLUME":/nix "$IMAGE" \
        nix-instantiate --eval --expr builtins.currentSystem 2>/dev/null | tr -d '"' | tail -n 1) || true
    if [ "$vol_system" != "$ARCH-linux" ]; then
        die "BB_NIX_VOLUME=$NIX_VOLUME holds ${vol_system:-no working} Nix ($ARCH-linux needed: x86_64 volumes end in -x86_64)"
    fi
fi

# Runs inside the container, in /src (the mounted source tree). $1 = mode.
# Only the Nix image's own tools here (bash, coreutils, findutils, grep, curl, tar, git): no sed
# outside nix-shell.
read -r -d '' INNER <<'INNER_SCRIPT' || true
set -euo pipefail
mode=$1; shift
cd /src
# Linux host with Docker Engine: root-owned files in what the build writes go back to the host
# user, also when the build fails. Ownership only.
if [ -n "${BB_OWNER:-}" ]; then
    give_back() {
        local p
        for p in out pgo dist fsr4_shaders packaging/runtime-paths.nix gpu/third_party/fsr-vulkan \
                 .git/modules/gpu/third_party/fsr-vulkan; do
            if [ -e "$p" ] || [ -L "$p" ]; then
                find "$p" -user 0 -exec chown -h "$BB_OWNER" {} + 2>/dev/null || true
            fi
        done
    }
    trap give_back EXIT
fi
# For every nix command here: flakes for `nix bundle` (AppImage), no build sandbox inside Docker,
# no separate build users (root in a throwaway container: builds do not depend on the image's
# nixbld group, which a rebuilt or pruned image may lack), no seccomp filter (unsupported under
# Rosetta and QEMU), and derivations built from source limited to $BB_JOBS cores, like the compiler.
# Without the sandbox every build runs with HOME=/homeless-shelter on the container's real file
# system, and Nix refuses to start a build while that directory exists ("home directory
# /homeless-shelter exists; please remove it"). Anything that writes to that home creates it and
# stops every build after it. Three guards:
# - HOME and XDG_CACHE_HOME are /root for every command here, nix-shell commands included (Nix
#   itself keeps caches in ~/.cache/nix: run with HOME=/homeless-shelter, it creates the directory);
# - a post-build hook removes it after each build (max-jobs = 1: builds run one at a time, and Nix
#   waits for the hook before it starts the next one);
# - each step here removes it before it starts and after it ends, and names what was in it;
# - a watcher removes it whenever it appears: without build users Nix cannot kill what a builder
#   started, and such a leftover process (a test's D-Bus or dconf service, a font cache writer)
#   can create it after the hook ran, between two builds;
# - a step that still stops on it runs again (up to 3 times): what Nix built is kept, so it goes on
#   from the build it stopped at.
export HOME=/root XDG_CACHE_HOME=/root/.cache
mkdir -p /tmp/nixhooks "$XDG_CACHE_HOME"
printf '#!/bin/sh\nrm -rf /homeless-shelter\n' > /tmp/nixhooks/post-build
chmod +x /tmp/nixhooks/post-build
clear_homeless() { # $1: what left it (for the message)
    if [ -e /homeless-shelter ]; then
        echo "==> Removing /homeless-shelter left by $1 (contents: $(ls -A /homeless-shelter /homeless-shelter/.cache 2>/dev/null | tr '\n' ' '))"
        rm -rf /homeless-shelter
    fi
}
clear_homeless "the image"
# The watcher (it ends with the container). The first few finds are named, with what was inside.
(
    seen=0
    while sleep 0.2; do
        [ -e /homeless-shelter ] || continue
        if [ "$seen" -lt 5 ]; then
            echo "==> /homeless-shelter appeared during a Nix step (contents: $(ls -A /homeless-shelter /homeless-shelter/.cache /homeless-shelter/.config 2>/dev/null | tr '\n' ' ')): removed" >&2
        fi
        seen=$((seen + 1))
        rm -rf /homeless-shelter
    done
) &
# in_shell LABEL COMMAND: COMMAND in the build environment (shell.nix), with the home above.
in_shell() {
    local status attempt
    for attempt in 1 2 3; do
        status=0
        clear_homeless "an earlier step"
        nix-shell shell.nix --run "export HOME=/root XDG_CACHE_HOME=/root/.cache; $2" 2>&1 |
            tee /tmp/step.log || status=$?
        clear_homeless "$1"
        if [ "$status" -ne 0 ] && [ "$attempt" -lt 3 ] &&
           grep -q 'home directory "/homeless-shelter" exists' /tmp/step.log; then
            echo "==> Nix stopped on /homeless-shelter during $1: running it again (attempt $((attempt + 1)) of 3; what was built is kept)"
            continue
        fi
        return "$status"
    done
}
export NIX_CONFIG="experimental-features = nix-command flakes
sandbox = false
build-users-group =
filter-syscalls = false
max-jobs = 1
cores = ${BB_JOBS:-2}
post-build-hook = /tmp/nixhooks/post-build"
# Optional GitHub token (any classic token, no scopes): 5000 API requests/hour instead of 60.
if [ -n "${GITHUB_TOKEN:-}" ]; then
    NIX_CONFIG="$NIX_CONFIG
access-tokens = github.com=${GITHUB_TOKEN}"
fi
echo "==> Container: $(uname -m)"
if [ "$BB_ARCH" = aarch64 ]; then
    echo "==> Kernel page size: $(grep -m1 KernelPageSize /proc/self/smaps | tr -s ' ' | cut -d' ' -f2-) (FEXCore assumes 4 kB)"
fi
if [ "$mode" = shell ]; then
    # exec would skip the EXIT trap: run the shell, then give the files back.
    nix-shell shell.nix --command 'export HOME=/root XDG_CACHE_HOME=/root/.cache; return'
    exit $?
fi
# build.sh calls plain `ninja` (cores + 2 jobs). This wrapper, first in PATH, adds -j $BB_JOBS.
mkdir -p /tmp/bbwrap
cat > /tmp/bbwrap/ninja <<'WRAPPER'
#!/bin/sh
real=$(PATH=${PATH#/tmp/bbwrap:}; export PATH; command -v ninja)
exec "$real" -j "${BB_JOBS:-2}" "$@"
WRAPPER
chmod +x /tmp/bbwrap/ninja
if [ "$BB_ARCH" = aarch64 ]; then
    # FEX turns LTO on by default, which needs llvm-ar and LLVM's linker plugin; shell.nix's clang
    # has neither ("CMAKE_C_COMPILER_AR-NOTFOUND: command not found"). build.sh configures FEX only
    # when out/fex/build.ninja is missing, and CMake keeps existing cache entries, so a pre-seeded
    # ENABLE_LTO=OFF turns it off without touching build.sh.
    if [ -f out/fex/CMakeCache.txt ] && ! grep -q '^ENABLE_LTO:BOOL=OFF$' out/fex/CMakeCache.txt; then
        echo "==> FEX was configured with LTO: starting its build directory (out/fex) over without it"
        rm -rf out/fex
    fi
    if [ ! -f out/fex/CMakeCache.txt ]; then
        mkdir -p out/fex && echo 'ENABLE_LTO:BOOL=OFF' > out/fex/CMakeCache.txt
    fi
fi
echo "==> Building for ${BB_ARCH} (BB_LTO=${BB_LTO}, ${BB_JOBS} parallel jobs); logs: out/gpu-build.log$([ "$BB_ARCH" = aarch64 ] && echo ', out/fex-build.log')"
in_shell "the build" 'PATH=/tmp/bbwrap:$PATH bash build.sh'
echo "==> Built:"
in_shell "the build summary" \
    'for f in out/bb-probe out/bb-gpu-capabilities out/gpu/libbbgpu.so out/gpu/libbbnet.so out/fex/libbbcpu.so; do [ -f "$f" ] && printf "  %-26s %s\n" "$f" "$(readelf -h "$f" | sed -n "s/ *Machine: *//p")"; done; true'
if [ "$mode" = test ]; then
    # No GPU, display or gamepad in the container, and maybe emulation: some tests may fail.
    echo "==> Running the project's tests (build.sh --test)"
    in_shell "the tests" 'PATH=/tmp/bbwrap:$PATH bash build.sh --test' || { echo "==> Some tests failed (see above)"; exit 3; }
    echo "==> Tests passed"
fi
if [ "$mode" = appimage ]; then
    # packaging/default.nix copies fsr4_shaders/ into the package, so the FSR 4 assets come first
    # (tools/fetch_fsr4_assets.sh; its optimizer step runs perl, which shell.nix lacks).
    echo "==> FSR 4 assets (downloaded from GitHub into fsr4_shaders/; the package includes them)"
    perl_bin=$(nix-build '<nixpkgs>' -A perl --no-out-link)/bin
    clear_homeless "the perl download"
    in_shell "the FSR 4 asset download" "PATH=$perl_bin:\$PATH bash tools/fetch_fsr4_assets.sh"
    # appimage.sh names the bundler without a commit (github:ralismark/nix-appimage), so Nix asks
    # GitHub's API for its latest commit: 60 anonymous requests per hour per IP, often used up on
    # shared IPs. A copy of the script with the bundler pinned to a commit skips that API call
    # (pinned inputs download as plain archives). The copy lives in /tmp, not in the tree; it runs
    # from packaging/ as "appimage.sh" so its `cd "$(dirname "$0")/.."` still lands in /src.
    bundler_rev=7946addbc0d97e358a6d7aefe5e82310f0fe6b18   # ralismark/nix-appimage, 2025-09-15
    original=$(cat packaging/appimage.sh)
    pinned=${original//github:ralismark\/nix-appimage /github:ralismark/nix-appimage/${bundler_rev} }
    printf '%s\n' "$pinned" > /tmp/appimage-pinned.sh
    if [ "$pinned" = "$original" ]; then
        echo "WARNING: appimage.sh changed upstream; running it unpinned" >&2
    fi
    if [ "$BB_ARCH" = aarch64 ]; then
        echo "==> Packaging the AppImage (nix bundle). The first run builds the patched Turnip (Mesa) driver from source"
    else
        echo "==> Packaging the AppImage (nix bundle). The first run builds some packages from source" \
             "(e.g. GTK 4 without GStreamer, SDL3, dxil-spirv; maybe a MinGW cross compiler for fsr4cap.exe)"
    fi
    if in_shell "the packaging" 'set -o pipefail; cd packaging && BB_NIXPKGS=/opt/nixpkgs bash -c "$(cat /tmp/appimage-pinned.sh)" appimage.sh 2>&1 | tee /tmp/appimage.log'; then
        exit 0
    fi
    # The bundler's last step packs the package's closure with `mksquashfs <each store path> ...
    # -no-strip`. Under emulation (an x86_64 build on Apple Silicon: Rosetta or QEMU) mksquashfs
    # 4.6.1 fails there with any two sources ("Can't have two different sources with same
    # pathname"), natively it works. Everything that step needs is built by then, so it is done
    # here instead with the bundler's own commands, its one -no-strip call given the closure as a
    # single staged directory (hard links next to the store: no copies). Then the rest of
    # appimage.sh: the AppImage into dist/, the package's GC root.
    drv=$(grep -o "/nix/store/[a-z0-9]\{32\}-[^ '\"]*\.AppImage\.drv" /tmp/appimage.log | head -n 1 || true)
    if ! grep -q "two different sources with same pathname" /tmp/appimage.log || [ -z "$drv" ]; then
        exit 1
    fi
    echo "==> mksquashfs can't pack the closure list under emulation; packing the AppImage here from $drv"
    cat > /tmp/assemble-appimage.sh <<'ASSEMBLE'
# $1: the bundler's AppImage derivation, $2: where to write the AppImage.
set -euo pipefail
drv=$1 result=$2
text=$(cat "$drv")
# Its build command: the "buildCommand" string of the derivation's environment, unescaped.
cmd=${text#*\(\"buildCommand\",\"}
cmd=${cmd%%\"\),\(*}
p=$'\x01'
cmd=${cmd//\\\\/$p}; cmd=${cmd//\\n/$'\n'}; cmd=${cmd//\\t/$'\t'}; cmd=${cmd//\\r/$'\r'}
cmd=${cmd//\\\"/\"}; cmd=${cmd//$p/\\}
inputs=${text#*\(\"nativeBuildInputs\",\"}
inputs=${inputs%%\"*}
squashfs=""
for w in $inputs; do if [ -x "$w/bin/mksquashfs" ]; then squashfs=$w; fi; done
[ -n "$squashfs" ] || { echo "ERROR: no mksquashfs among the derivation's inputs ($inputs)" >&2; exit 1; }
case $cmd in *mksquashfs*-no-strip*) ;; *) echo "ERROR: unexpected build command in $drv" >&2; exit 1 ;; esac
export PATH="$squashfs/bin:$PATH"
stage=/nix/.bbport-appimage-root   # in the store's file system, so cp -l makes hard links
# mksquashfs SOURCES... DEST OPTIONS...: with -no-strip, the sources become one directory tree.
mksquashfs() {
    case " $* " in *" -no-strip "*) ;; *) command mksquashfs "$@"; return ;; esac
    local args=("$@") i=0 a opts=()
    while [ "$i" -lt "${#args[@]}" ] && [[ ${args[i]} != -* ]]; do i=$((i + 1)); done
    local dest=${args[i - 1]} sources=("${args[@]:0:i-1}")
    for a in "${args[@]:i}"; do [ "$a" = -no-strip ] || opts+=("$a"); done
    rm -rf "$stage"
    for a in "${sources[@]}"; do
        mkdir -p "$stage$(dirname "$a")"
        cp -al "$a" "$stage$a" 2>/dev/null || cp -a "$a" "$stage$a"
    done
    echo "  mksquashfs: ${#sources[@]} store paths as one directory"
    command mksquashfs "$stage" "$dest" "${opts[@]}"
    rm -rf "$stage"
}
work=$(mktemp -d)
trap 'rm -rf "$work" "$stage"' EXIT
cd "$work"
export out=$work/bbport.AppImage
eval "$cmd"
install -m755 "$out" "$result"
ASSEMBLE
    mkdir -p dist
    in_shell "the AppImage assembly" "bash /tmp/assemble-appimage.sh '$drv' /src/dist/Bloodborne-bbport-$BB_ARCH.AppImage"
    # appimage.sh's last steps: a GC root for the package (keeps what was built from source).
    in_shell "the GC root" 'nix-build --impure packaging -o out/nix-roots/bbport > /dev/null'
    ls -lh "dist/Bloodborne-bbport-$BB_ARCH.AppImage"
fi
INNER_SCRIPT

tty_flags=()
if [ -t 0 ] && [ -t 1 ]; then tty_flags=(-it); fi

# "${arr[@]+"${arr[@]}"}" keeps bash 3.2 happy with an empty array.
docker run --rm ${tty_flags[@]+"${tty_flags[@]}"} \
    --platform "$PLATFORM" \
    -v "$NIX_VOLUME":/nix -v "$SRC":/src \
    ${build_mounts[@]+"${build_mounts[@]}"} \
    -e BB_ARCH="$ARCH" -e BB_OWNER="$OWNER" \
    -e BB_LTO="$BB_LTO" -e BB_JOBS="$JOBS" -e GITHUB_TOKEN="${GITHUB_TOKEN:-}" \
    -e SDL_VIDEO_DRIVER=dummy -e SDL_AUDIO_DRIVER=dummy \
    ${turnip_flags[@]+"${turnip_flags[@]}"} \
    "$IMAGE" bash -c "$INNER" inner "$MODE"

case $MODE in
    shell) ;;
    appimage) echo "==> Done. $SRC/dist/Bloodborne-bbport-$ARCH.AppImage (for $ARCH Linux)." ;;
    *) echo "==> Done. Linux $ARCH build in $SRC/$OUT_DESC (runs inside the container or packaged as an AppImage)." ;;
esac

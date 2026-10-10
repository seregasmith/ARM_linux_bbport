#!/usr/bin/env bash
# Builds dist/Bloodborne-bbport-<arch>.AppImage (x86_64, aarch64): the port's current build (run
# build.sh first), its scripts and the launcher, bundled with their Nix closure (nix-appimage: the AppImage mounts
# its /nix/store with user namespaces, available on SteamOS and most desktops).
# Running it opens the launcher; `--play` starts the game with the launcher's saved settings.
# Data (generated files, saves, bbport.ini): ~/.local/share/bbport (BB_DATA_DIR).
# BB_MANGOHUD_SRC=<MangoHud tree>: bundle that MangoHud (and its MangoHud/MangoHud.conf) instead
# of nixpkgs' release (packaging/default.nix).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
[[ -f out/bb-probe && -f out/gpu/libbbgpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
arch=$(uname -m)
libs=(out/bb-probe out/gpu/libbbgpu.so)
if [[ $arch == aarch64 ]]; then
    [[ -f out/fex/libbbcpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
    libs+=(out/fex/libbbcpu.so)
fi
# NIX: the nix command (default nix; e.g. "nix-portable nix" without a system Nix).
# BB_NIXPKGS: the nixpkgs the build's nix-shell used (-I nixpkgs=...), when not <nixpkgs>.
read -r -a nix <<< "${NIX:-nix}"
include=()
if [[ -n ${BB_NIXPKGS:-} ]]; then include=(-I "nixpkgs=$BB_NIXPKGS"); fi
root=$PWD
# The libraries' store paths: for each library a binary needs (NEEDED), the RUNPATH directory it
# is found in (their closures come along). Not every RUNPATH entry: built in nix-shell, the
# binaries list the lib directories of the whole build environment (the full GCC, Vulkan headers,
# SPIRV-Tools, ...), hundreds of MB the game never loads.
needed_dirs() {
    local elf=$1 lib dir
    local -a rpath
    mapfile -t rpath < <(readelf -d "$elf" | sed -n 's/.*R\{0,1\}U\{0,1\}N\{0,1\}PATH.*\[\(.*\)\]/\1/p' | tr ':' '\n')
    for lib in $(readelf -d "$elf" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        for dir in "${rpath[@]}"; do
            [[ $dir == /nix/store/* && -e $dir/$lib ]] && { echo "$dir"; break; }
        done
    done
}
{
    echo '['
    for elf in "${libs[@]}" $(ls out/libbbport_dlss.so out/gpu/libbbnet.so 2>/dev/null); do
        needed_dirs "$elf"
    done | grep -o '^/nix/store/[^/]*' | sort -u | grep -v -- '-nix-shell$' | sed 's/.*/  "&"/'
    echo ']'
} > packaging/runtime-paths.nix
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd "$work" && "${nix[@]}" bundle --impure "${include[@]}" --bundler github:ralismark/nix-appimage \
    --expr "import $root/packaging {}")
mkdir -p dist
image=$(readlink "$work/bbport.AppImage")
# nix-portable: its /nix/store is ~/.nix-portable/nix/store outside its sandbox.
if [[ ! -e $image && -e ${NP_LOCATION:-$HOME}/.nix-portable$image ]]; then
    image=${NP_LOCATION:-$HOME}/.nix-portable$image
fi
install -m755 "$image" "dist/Bloodborne-bbport-$arch.AppImage"
# A GC root for the package: nix-collect-garbage keeps it, and with it the outputs built here from
# source (the Vulkan-only Mesa, GTK 4 without GStreamer, libadwaita, SDL3), so the next AppImage
# does not build them again (only a nixpkgs update does).
nix-build --impure packaging -o out/nix-roots/bbport > /dev/null
ls -lh "dist/Bloodborne-bbport-$arch.AppImage"

# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# bb-recomp for the device (aarch64 packages only): the Bloodborne Android Rebuild Tool (ducvd89,
# github.com/ducvd89/Bloodborne-android-rebuild-tool, GPL-3.0-or-later with its ADDITIONAL_TERMS.md),
# built from a local checkout of its sources, as the folder scripts/aot_build.py runs (the
# launcher's Game -> Native CPU): bin/bb-recomp; share/bb-recomp, the four link inputs of its
# in-process LLD (src/bundle.cpp) made for glibc under the names the tool looks for, and its variant
# catalog; licenses/; VERSION; README.txt. default.nix puts it into the package as aot-tool/.
#
# The tool's compiler is the official LLVM 23.1.3 release, as in every build of the tool (its
# arm64/build.sh, package.sh, fetch-deps.sh): the native code it writes comes from LLVM's code
# generator, so this is the LLVM it was tested with. It is a pinned download here (no LLVM is
# compiled): only its libraries, headers and CMake package are used. nixpkgs' clang compiles FEX's
# front end and the tool (FEX needs Clang), linked against this package's glibc and libstdc++: the
# tool's own Linux builds link statically against an Ubuntu 22.04 sysroot instead, which the
# AppImage does not need (it carries its glibc). zlib and zstd are static, the versions of the
# tool's fetch-deps.sh. -ffp-contract=off as in the tool's Android build: the floating point inside
# LLVM's and the tool's own heuristics then decides as on the x86-64 PC builds.
#
# The tool's sources are not changed; this build's copy leaves out bbrecomp.cmake's `-static` and
# turns CMake's C++20 module scan off (postPatch below).
{ pkgs, recompSrc, fexSrc }:
let
  lib = pkgs.lib;
  llvmVersion = "23.1.3";
  # The tool's version (src/version.h), line by line: `.` of builtins.match does not cross newlines everywhere.
  versionLine = "#define BBR_TOOL_VERSION \"([^\"]+)\".*";
  version = let
    line = lib.findFirst (l: builtins.match versionLine l != null) null
      (lib.splitString "\n" (builtins.readFile (recompSrc + "/src/version.h")));
  in if line == null then throw "no BBR_TOOL_VERSION in the Rebuild Tool's src/version.h"
     else builtins.head (builtins.match versionLine line);
  # 1.1 GB (12 GB unpacked); sha256 1154f37e... of the file on GitHub's release page.
  llvmTarball = pkgs.fetchurl {
    url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-${llvmVersion}/LLVM-${llvmVersion}-Linux-ARM64.tar.zst";
    hash = "sha256-EVTzfsFbVlT8uMw6iiNSpMxO70S9/33dR/sQ6oXILwE=";
  };
  # Only what the tool's build reads, written straight from the archive (a few GB instead of 12; the
  # archive is decompressed twice): LLVM's CMake package, the headers of LLVM and LLD, the files
  # LLVM's exported targets name, LLD's libraries (the tool links them by path) and compiler-rt's
  # AArch64 builtins. The exported programs are built for another system and never run here: they
  # are empty files (LLVM's CMake package only checks that they exist). No fixup: nothing in it is
  # patched or stripped.
  llvmRelease = pkgs.runCommand "llvm-release-${llvmVersion}-arm64" {
    nativeBuildInputs = [ pkgs.zstd pkgs.gnutar ];
  } ''
    mkdir -p $out
    # tar names members it did not find; whatever is needed is checked below.
    unpack() { zstd -d --long=31 -c ${llvmTarball} | tar -x -C $out --strip-components=1 --wildcards "$@" || true; }
    unpack '*/lib/cmake/*'
    patterns=('*/include/llvm/*' '*/include/llvm-c/*' '*/include/lld/*' '*/lib/liblld*.a' '*/lib/clang/*/lib/aarch64*')
    for f in $(grep -ho '_IMPORT_PREFIX}/[^"]*' $out/lib/cmake/llvm/LLVMExports-*.cmake | sed 's|^_IMPORT_PREFIX}/||' | sort -u); do
      case $f in
        bin/*) mkdir -p "$out/$(dirname "$f")"; : > "$out/$f" ;;
        lib/*) patterns+=("*/$f") ;;
      esac
    done
    unpack "''${patterns[@]}"
    for f in lib/cmake/llvm/LLVMConfig.cmake lib/liblldELF.a lib/liblldCommon.a include/llvm/IR/Module.h include/lld/Common/Driver.h; do
      [ -e "$out/$f" ] || { echo "the LLVM release has no $f" >&2; exit 1; }
    done
    du -sh $out
  '';
  zlibStatic = pkgs.stdenv.mkDerivation {
    pname = "zlib-static";
    version = "1.3.1";
    src = pkgs.fetchurl {
      url = "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.xz";
      hash = "sha256-OO+WuN/lENQnB9nHgYd5FHklQRM+GHCEFGO/pz+IPjI=";
    };
    nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
    cmakeFlags = [ "-DCMAKE_POSITION_INDEPENDENT_CODE=ON" ];
    postInstall = "rm -f $out/lib/libz.so*";
  };
  zstdStatic = pkgs.stdenv.mkDerivation {
    pname = "zstd-static";
    version = "1.5.7";
    src = pkgs.fetchurl {
      url = "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz";
      hash = "sha256-6zPlH0mhXgI5UM14Jcp0pKK0Pbg1SCWsJPwbfuCeb6M=";
    };
    nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
    cmakeDir = "../build/cmake";
    cmakeFlags = [ "-DZSTD_BUILD_PROGRAMS=OFF" "-DZSTD_BUILD_TESTS=OFF" "-DZSTD_BUILD_SHARED=OFF"
                   "-DZSTD_BUILD_STATIC=ON" "-DCMAKE_POSITION_INDEPENDENT_CODE=ON" ];
  };
  # BB_RECOMP_DEPS_PREFIX: the static zlib and zstd LLVM's CMake package and the tool look for.
  deps = pkgs.symlinkJoin { name = "bb-recomp-deps"; paths = [ zlibStatic zstdStatic ]; };
in
pkgs.clangStdenv.mkDerivation {
  pname = "bb-recomp";
  inherit version;
  src = recompSrc;
  nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.python3 ];
  # LLVM's CMake package asks for libxml2 (its WindowsManifest library, which the tool does not link).
  buildInputs = [ pkgs.libxml2 ];
  # The tool's own builds have none of nixpkgs' hardening flags (fortify, stack protector, PIE ...).
  hardeningDisable = [ "all" ];
  env.NIX_CFLAGS_COMPILE = "-ffp-contract=off";
  # The tool's CMakeLists.txt is a driver around FEX's build (an ExternalProject): configured below.
  dontUseCmakeConfigure = true;
  # LLVM is linked in as static libraries: the 12 GB release must not end up in the AppImage's
  # closure through a leftover RUNPATH entry or a path string (the fixup's strip and RUNPATH
  # shrinking remove both; this makes the build fail instead if one stays).
  disallowedReferences = [ llvmRelease ];
  postPatch = ''
    patchShebangs .
    substituteInPlace bbrecomp.cmake --replace-fail "target_link_options(bb-recomp PRIVATE -static)" \
      "# (bbport's packaging/bb-recomp.nix: linked against the package's glibc instead of -static)"
    # FEX's build (the ExternalProject that builds the tool) without CMake's scan for C++20 modules
    # (neither uses modules): the scan runs clang-scan-deps from nixpkgs' unwrapped clang, which does
    # not get the include paths the cc-wrapper adds ("'algorithm' file not found").
    substituteInPlace CMakeLists.txt --replace-fail "    -DENABLE_X86_HOST_DEBUG=ON" \
      "    -DENABLE_X86_HOST_DEBUG=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF"
  '';
  buildPhase = ''
    runHook preBuild
    # FEX 2609.1 (this tree's third_party/FEX, the front end the port's libbbcpu.so runs), copied so
    # that its build can write next to its sources.
    cp -r ${fexSrc} $NIX_BUILD_TOP/fex-src
    chmod -R u+w $NIX_BUILD_TOP/fex-src
    patchShebangs $NIX_BUILD_TOP/fex-src
    cmake -S . -B build -G Ninja \
      -DFEX_SOURCE_DIR=$NIX_BUILD_TOP/fex-src \
      -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
      -DBB_RECOMP_BUNDLED=ON -DBB_RECOMP_NO_X86=ON \
      -DBB_RECOMP_LLVM_DIR=${llvmRelease}/lib/cmake/llvm \
      -DBB_RECOMP_DEPS_PREFIX=${deps} \
      -DBB_RECOMP_MARCH=armv8-a \
      -DBB_RECOMP_VERSION=${version} \
      -DBB_RECOMP_JOBS=$NIX_BUILD_CORES
    cmake --build build
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 build/bb-recomp $out/bin/bb-recomp
    s=$out/share/bb-recomp
    mkdir -p $s/variants $out/licenses
    # The link inputs (src/bundle.cpp), under the names the tool's Android bundle uses:
    # the x87 helpers, as the tool compiles them for the generated library (src/compile.cpp);
    $CC -O2 -fPIC -fvisibility=hidden -fno-stack-protector -ffp-contract=off \
      -c helpers/bbr_x87_all.c -o $s/helpers-aarch64.o
    # an empty libbbcpu.so for the library's DT_NEEDED (the port's guest CPU library provides bbr_*);
    $CC -shared -nostdlib -Wl,-soname,libbbcpu.so -x c /dev/null -o $s/libbbcpu-stub.so
    # this package's libm (an ELF: lib/libm.so is a linker script): DT_NEEDED libm.so.6 with its versions;
    cp -L ${pkgs.glibc}/lib/libm.so.6 $s/libm.so
    # compiler-rt's builtins of the same LLVM release (__divti3, __udivti3).
    builtins=$(find ${llvmRelease}/lib/clang -path '*aarch64*' -name 'libclang_rt.builtins*.a' | head -n 1)
    [ -n "$builtins" ] || { echo "no AArch64 compiler-rt builtins in the LLVM release" >&2; exit 1; }
    cp "$builtins" $s/libclang_rt.builtins-aarch64-android.a
    cp variants/*.txt $s/variants/
    cp LICENSE NOTICES ADDITIONAL_TERMS.md $out/licenses/
    cp helpers/NOTICE $out/licenses/helpers-NOTICE
    echo ${version} > $out/VERSION
    cat > $out/README.txt <<'README'
    bb-recomp for this device: the Bloodborne Android Rebuild Tool by ducvd89,
    https://github.com/ducvd89/Bloodborne-android-rebuild-tool (GPL-3.0-or-later with the additional
    terms in licenses/ADDITIONAL_TERMS.md), built unmodified from its sources by bbport's
    packaging/bb-recomp.nix with the official LLVM 23.1.3 release, linked against the package's glibc.
    share/bb-recomp holds bbport's link inputs for glibc and the tool's variant catalog.

    The launcher's Game -> Native CPU runs it (scripts/aot_build.py). What it writes is the game's
    own code compiled for this device: it stays on the device, never share it.
    README
    runHook postInstall
  '';
  meta = {
    description = "bb-recomp, the Bloodborne Android Rebuild Tool, for the device";
    license = lib.licenses.gpl3Plus;
    platforms = [ "aarch64-linux" ];
  };
}

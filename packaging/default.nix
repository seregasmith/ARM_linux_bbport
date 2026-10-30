# Packaged port: the prebuilt game binaries (build.sh first), the start-up scripts and the GTK4
# launcher, with their whole Nix closure and Mesa's Vulkan drivers. `bash packaging/appimage.sh`
# turns it into an AppImage (Steam Deck); `nix-build packaging` alone gives result/bin/bbport.
{ pkgs ? import <nixpkgs> { }
  # Store paths the prebuilt binaries load libraries from (their RUNPATHs), written by
  # appimage.sh. Nix only finds references to its inputs, and the binaries were built outside.
, runtimePaths ? (if builtins.pathExists ./runtime-paths.nix then import ./runtime-paths.nix else [ ])
}:
let
  lib = pkgs.lib;
  # aarch64 hosts: the game's x86-64 code runs in FEXCore (out/fex/libbbcpu.so, build.sh), or with BB_AOT as ahead-of-time
  # recompiled native code (out/aot-cpu/libbbcpu.so: bin/aot-cpu, which run.sh puts first on the library path).
  arm = pkgs.stdenv.hostPlatform.isAarch64;
  root = ./..;
  # FSR 4.1.1 models are extracted from AMD's DLLs: never in a public package. BB_PACKAGE_FSR411=1
  # (appimage.sh runs nix with --impure) bundles the local fsr4_411 for one's own devices.
  fsr411 = builtins.getEnv "BB_PACKAGE_FSR411" == "1";
  # bb-recomp for the device (packaging/bb-recomp.nix, aarch64): the launcher's Native CPU compiles the
  # game's code on the device with it (aot-tool/ in the package, scripts/aot_build.py). Built from the
  # Rebuild Tool's sources: BB_RECOMP_SRC (build_docker.sh mounts it), else a checkout next to this
  # tree; without either the package has no aot-tool (the launcher then says how to get one).
  # BB_PACKAGE_RECOMP=0: never.
  # (A path, so that ".." is resolved: the filter below compares paths as strings.)
  recompDir = /. + (let e = builtins.getEnv "BB_RECOMP_SRC"; in
    if e != "" then e else toString root + "/../Bloodborne-android-rebuild-tool");
  withRecomp = arm && builtins.getEnv "BB_PACKAGE_RECOMP" != "0"
    && builtins.pathExists (recompDir + "/bbrecomp.cmake");
  aotTool = import ./bb-recomp.nix {
    inherit pkgs;
    recompSrc = builtins.path {
      name = "bb-recomp-src";
      path = recompDir;
      # Its builds, downloads and git data stay out.
      filter = path: type:
        let rel = lib.removePrefix (toString recompDir + "/") (toString path);
        in !(builtins.elem rel [ ".git" "out" ".local-deps" ".idea" ]) && baseNameOf path != "__pycache__";
    };
    # FEX 2609.1 with its submodules, without its test binaries.
    fexSrc = builtins.path {
      name = "fex-src";
      path = root + "/third_party/FEX";
      filter = path: type:
        let name = baseNameOf path;
        in name != ".git" && !(lib.hasPrefix "fex-" name && lib.hasSuffix "-bins" name);
    };
  };
  assetDirs = [ "scripts" "patches" "fsr4_shaders" "launcher" "tools/fsr4cap" ]
    ++ lib.optional fsr411 "fsr4_411";
  # Only what the package needs (the tree also holds builds, profiles and captures).
  wanted = [
    "run.sh" "VERSION" "LICENSE" "NOTICE" "shader_recipes.bin" "image_shapes.txt" "out" "out/bb-probe" "out/bb-gpu-capabilities" "out/gpu" "out/gpu/libbbgpu.so" "tools"
  ] ++ lib.optionals arm [ "out/fex" "out/fex/libbbcpu.so" "out/aot-cpu" "out/aot-cpu/libbbcpu.so" ] ++ assetDirs;
  # DLSS (a build with DLSS_SDK_ROOT): the bridge, NVIDIA's runtime library and its license, as
  # NVIDIA's RTX SDK license allows in an application. Without them the launcher lets the player
  # choose NVIDIA's library (the bridge is still needed).
  dlssFiles = [ "out/libbbport_dlss.so" "out/NVIDIA-DLSS-LICENSE.txt" "out/gpu/libbbnet20.so" ];
  isNgx = rel: lib.hasPrefix "out/libnvidia-ngx-dlss.so." rel || lib.hasPrefix "out/libnvidia-ngx-dlssg.so." rel;
  src = builtins.path {
    name = "bbport-src";
    path = root;
    filter = path: type:
      let rel = lib.removePrefix (toString root + "/") (toString path);
      in builtins.elem rel wanted || builtins.elem rel dlssFiles || isNgx rel
        || lib.any (dir: lib.hasPrefix (dir + "/") rel) assetDirs;
  };
  python = pkgs.python3.withPackages (ps: [ ps.pygobject3 ]);
  # FSR 4.1.1 from the user's own AMD DLL (the launcher's button, tools/fsr4cap/build_assets.sh):
  # its two tools built here, so that the package needs no compiler and no network for it. Proton
  # (GE-Proton 10+) and Steam's runtime come from the user's Steam. On NixOS the recording runs
  # on the host (systemd-run --user, below on PATH; umu-launcher from the host's nix-shell).
  dxilSpirv = pkgs.stdenv.mkDerivation {
    pname = "dxil-spirv";
    version = "0-unstable-7dc5278";
    src = pkgs.fetchFromGitHub {
      owner = "HansKristian-Work";
      repo = "dxil-spirv";
      rev = "7dc52786cdb1f53c54dd5c5c2698dff00ea5f0a3";
      fetchSubmodules = true;
      hash = "sha256-vdSMp6QusmVTGpcl9jr+wQUQy5HLWwjr/5TI8ARMWRI=";
    };
    patches = [ ../tools/fsr4cap/dxil-spirv-class-bindings.patch ];
    nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.python3 ];
    # The CLI and the shared library it links (its own install rules set the RUNPATH).
  };
  fsr4capExe = pkgs.runCommand "fsr4cap.exe" {
    nativeBuildInputs = [ pkgs.pkgsCross.mingwW64.buildPackages.gcc ];
  } ''
    f=${../tools/fsr4cap}
    x86_64-w64-mingw32-gcc -std=c11 -O1 -Wall -I$f/ffx/api/include -I$f/ffx/upscalers/include \
      $f/fsr4cap.c $f/capture.c $f/rootsig.c -o $out -ld3d12 -ldxguid -static
  '';
  # Mesa comes with the package. bbport_vulkan.py adds the host NVIDIA ICD and
  # only its vendor libraries (matching the host's kernel module).
  # aarch64: Adreno (Turnip), Mali (Panfrost), Raspberry Pi (V3DV), Apple (Asahi), AMD.
  icdArch = pkgs.stdenv.hostPlatform.parsed.cpu.name;
  icdNames = if arm then [ "freedreno" "panfrost" "broadcom" "asahi" "radeon" ] else [ "radeon" "intel" ];
  # Mesa 26's Turnip loses the device on the Adreno 740 (SM8550) without these patches. Only
  # freedreno is rebuilt; the other drivers stay the binary cache's Mesa.
  turnipPatches = [
    ./mesa-sm8550/0001-add-a830-chip-id.patch
    ./mesa-sm8550/0001-freedreno-ir3-vulkan-disable-bindless-ubo-const-lowering.patch
    ./mesa-sm8550/001-fix-freedreno-vulkan.patch
  ];
  # BB_TURNIP_SRC: a Mesa source tree for Turnip instead of nixpkgs' release, e.g. a checkout of
  # whitebelyash/mesa-unified's turnip/gen8 branch (Adreno 8xx work not yet in Mesa) or Mesa main.
  # The Adreno 830 chip IDs of 0001-add-a830-chip-id.patch are added where the tree lacks them; the
  # other patches are applied where they apply, skipped where the tree has them already, and a
  # conflict stops the build. BB_TURNIP_PATCHES=0 builds the tree as it is. Without
  # BB_TURNIP_SRC the package is unchanged (nixpkgs' Mesa with the patches above).
  turnipSrc = builtins.getEnv "BB_TURNIP_SRC";
  turnipPortPatches = builtins.getEnv "BB_TURNIP_PATCHES" != "0";
  turnip = (pkgs.mesa.override {
    galliumDrivers = [ "freedreno" ];
    vulkanDrivers = [ "freedreno" ];
    vulkanLayers = [ ];
    enablePatentEncumberedCodecs = false;
  }).overrideAttrs (old: {
    patches = old.patches ++ turnipPatches;
    mesonFlags = map (flag: if lib.hasPrefix "-Dtools=" flag then "-Dtools=" else flag) old.mesonFlags
      ++ [ "-Dgallium-va=disabled" ];
    postInstall = old.postInstall + ''
      mkdir -p $opencl $spirv2dxil
    '';
  } // lib.optionalAttrs (turnipSrc != "") {
    version = lib.removeSuffix "\n" (builtins.readFile (/. + turnipSrc + "/VERSION")) + "-src";
    src = builtins.path {
      name = "mesa-src";
      path = /. + turnipSrc;
      filter = path: type: baseNameOf path != ".git";
    };
    # nixpkgs' own patches only (the port's are applied below, where they still apply).
    patches = old.patches;
    # Not nixpkgs' check that the tree's GL headers match its mesa-gl-headers (the release's): a
    # Vulkan-only build of another tree does not use them.
    postPatch = ''
      patchShebangs .
    '' + lib.optionalString turnipPortPatches ''
      python3 - src/freedreno/common/freedreno_devices.py <<'PY'
      import re, sys
      path = sys.argv[1]
      s = open(path).read()
      anchor = 'GPUId(chip_id=0xffff44050000, name="Adreno (TM) 830"),'
      if anchor not in s:
          sys.exit("bbport: no Adreno 830 entry in freedreno_devices.py (BB_TURNIP_PATCHES=0: none of the port's changes)")
      added = []
      for chip in ("0xffff44050001", "0x44050000"):
          if not re.search(r"chip_id=" + chip + r"\b", s, re.I):
              s = s.replace(anchor, anchor + "\n        GPUId(chip_id=" + chip + ", name=\"Adreno (TM) 830\"),", 1)
              added.append(chip)
      open(path, "w").write(s)
      print("bbport: Adreno 830 chip IDs", "added: " + ", ".join(added) if added else "already in the tree")
      PY
      bb_patch() {
        if patch -p1 -R --dry-run -s -f < "$1" > /dev/null 2>&1; then
          echo "bbport: $(basename "$1" | cut -d- -f2-) is already in this Mesa tree"
        elif patch -p1 --dry-run -s -f < "$1" > /dev/null 2>&1; then
          patch -p1 -s -f < "$1"
          echo "bbport: applied $(basename "$1" | cut -d- -f2-)"
        else
          echo "bbport: $(basename "$1" | cut -d- -f2-) does not apply to this Mesa tree (BB_TURNIP_PATCHES=0: build without the port's patches)" >&2
          exit 1
        fi
      }
      bb_patch ${./mesa-sm8550/0001-freedreno-ir3-vulkan-disable-bindless-ubo-const-lowering.patch}
      bb_patch ${./mesa-sm8550/001-fix-freedreno-vulkan.patch}
    '';
  });
  icdMesa = name: if arm && name == "freedreno" then turnip else pkgs.mesa;
  # BB_MANGOHUD_SRC: a MangoHud 0.8.4 tree with its subprojects already fetched (meson setup),
  # replacing nixpkgs' release, e.g. one that reads the Adreno's load and the Snapdragon sensors.
  # Only its Vulkan layer is bundled.
  mangohudSrc = builtins.getEnv "BB_MANGOHUD_SRC";
  mangohud = if mangohudSrc == "" then pkgs.mangohud else
    (pkgs.mangohud.override { gamescopeSupport = false; nvidiaSupport = false; }).overrideAttrs (old: {
      src = builtins.path {
        name = "mangohud-src";
        path = mangohudSrc;
        filter = path: type:
          !(builtins.elem (lib.removePrefix (mangohudSrc + "/") (toString path))
            [ ".git" "build" "subprojects/packagecache" ])
          && baseNameOf path != "__pycache__";
      };
      postUnpack = "";
      # Its LD_PRELOAD script differs from the release's; the game loads the Vulkan layer.
      patches = lib.filter (patch: !(lib.hasSuffix "preload-nix-workaround.patch" (toString patch)))
        old.patches;
      postPatch = "";
      mesonFlags = old.mesonFlags ++ [ "-Dwith_mangohud_next=false" ];
      buildInputs = old.buildInputs ++ [ pkgs.vulkan-loader pkgs.libdrm pkgs.libGL ];
    });
  icds = lib.concatMapStringsSep ":"
    (name: "${icdMesa name}/share/vulkan/icd.d/${name}_icd.${icdArch}.json") icdNames;
  # Mesa's anti-lag layer (VK_AMD_anti_lag: AMD Anti-Lag 2 on any Mesa driver) from the bundled
  # Mesa, when it has one. The game finds it through BB_BUNDLED_VK_LAYER_PATH and switches it on
  # itself (vulkan/platform.cpp).
  bundledLayers = pkgs.runCommand "bbport-vulkan-layers" { nativeBuildInputs = [ pkgs.jq ]; } ''
    mkdir -p $out/share/vulkan/implicit_layer.d
    json=${pkgs.mesa}/share/vulkan/implicit_layer.d/VkLayer_MESA_anti_lag.json
    if [ -f "$json" ] && [ -f ${pkgs.mesa}/lib/libVkLayer_MESA_anti_lag.so ]; then
      jq ".layer.library_path = \"${pkgs.mesa}/lib/libVkLayer_MESA_anti_lag.so\"" "$json" \
        > $out/share/vulkan/implicit_layer.d/VkLayer_MESA_anti_lag.json
    fi
  '';
  # The launcher's GTK without GStreamer (video playback: flite, x265, ... ~200 MB), printing and
  # the search indexer, which it never uses; libadwaita on top of it.
  gtk4 = (pkgs.gtk4.override { cupsSupport = false; trackerSupport = false; }).overrideAttrs (old: {
    buildInputs = lib.filter (p: !(lib.hasPrefix "gst-" (p.pname or ""))) old.buildInputs;
    mesonFlags = old.mesonFlags ++ [ "-Dmedia-gstreamer=disabled" ];
    # g-ir-scanner's cache lives in ~/.cache: /homeless-shelter in build_docker.sh's unsandboxed
    # builds, which its watcher keeps removing. Without the cache the scan takes a few seconds more.
    env = (old.env or { }) // { GI_SCANNER_DISABLE_CACHE = "1"; };
    # GTK's demo apps' menu entries and AppStream data: AppImage managers showed the port as
    # "GTK Demo" (org.gtk.Demo4.appdata.xml in the closure).
    postInstall = (old.postInstall or "") + ''
      rm -f $out/share/applications/org.gtk.*.desktop $out/share/metainfo/org.gtk.*.xml
    '';
  });
  libadwaita = (pkgs.libadwaita.override { inherit gtk4; }).overrideAttrs (old: {
    env = (old.env or { }) // { GI_SCANNER_DISABLE_CACHE = "1"; };  # as for gtk4 above
  });
  # Fonts: bundled DejaVu and Adwaita plus the host's usual font directories, but not the host's
  # /etc/fonts: on NixOS it names fonts in the host's /nix/store, which the AppImage hides behind
  # its own store in some environments (Steam's FHS sandbox), and the launcher showed boxes.
  fontsConf = pkgs.writeText "bbport-fonts.conf" ''
    <?xml version="1.0"?>
    <!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
    <fontconfig>
      <dir>${pkgs.dejavu_fonts}/share/fonts</dir>
      <dir>${pkgs.adwaita-fonts}/share/fonts</dir>
      <dir>/usr/share/fonts</dir>
      <dir>/usr/local/share/fonts</dir>
      <dir prefix="xdg">fonts</dir>
      <cachedir prefix="xdg">bbport/fontconfig</cachedir>
      <include ignore_missing="yes">${pkgs.fontconfig.out}/etc/fonts/conf.d</include>
    </fontconfig>
  '';
  # Environment the closure needs on any host: icon themes, SVG icon loader, fonts (above) and
  # a UTF-8 locale built into glibc.
  common = ''
      --prefix XDG_DATA_DIRS : ${mangohud}/share:${pkgs.adwaita-icon-theme}/share:${pkgs.hicolor-icon-theme}/share:${gtk4}/share/gsettings-schemas/${gtk4.name} \
      --set-default GDK_PIXBUF_MODULE_FILE ${pkgs.librsvg}/${pkgs.gdk-pixbuf.moduleDir}.cache \
      --set-default FONTCONFIG_FILE ${fontsConf} \
      --set-default LC_ALL C.UTF-8 \
      --prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath [ pkgs.libglvnd pkgs.libx11 pkgs.libxext ]} \
      --set BB_VULKANINFO ${pkgs.vulkan-tools}/bin/vulkaninfo \
      --run '[ -r "''${SSL_CERT_FILE:-}" ] || export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt; [ -d "''${SSL_CERT_DIR:-}" ] || unset SSL_CERT_DIR' \
  '';
in
pkgs.stdenv.mkDerivation {
  pname = "bbport";
  version = "0.1";
  inherit src;
  # The hook adds its GTK to the wrapper (typelibs, schemas): ours, not nixpkgs' with GStreamer.
  nativeBuildInputs = [ pkgs.makeShellWrapper (pkgs.wrapGAppsHook3.override { gtk3 = gtk4; }) pkgs.gobject-introspection ];
  buildInputs = [ gtk4 libadwaita pkgs.adwaita-icon-theme pkgs.librsvg ]
    ++ map builtins.storePath runtimePaths;
  dontBuild = true;
  dontConfigure = true;
  # The binaries live under share/ (next to the scripts run.sh expects): strip them too, which
  # also drops the compiler and header paths their debug info would keep in the closure.
  stripDebugList = [ "share/bbport/bin/gpu" ] ++ lib.optionals arm [ "share/bbport/bin/cpu" "share/bbport/bin/aot-cpu" ];
  # patchelf (RPATH shrinking) corrupts the non-PIE game binary's symbol versions; it finds
  # its library through $ORIGIN/gpu and its other libraries through the build's RUNPATH.
  dontPatchELF = true;
  dontWrapGApps = true; # wrapped once below, together with the launcher's own variables
  installPhase = ''
    runHook preInstall
    d=$out/share/bbport
    mkdir -p $d/bin $out/bin
    cp run.sh VERSION LICENSE NOTICE $d/
    # The recorded game's shader recipes (gpu/render/shader_rebuild.h; no game code in them).
    if [ -f shader_recipes.bin ]; then cp shader_recipes.bin $d/; fi
    if [ -f image_shapes.txt ]; then cp image_shapes.txt $d/; fi
    cp -r scripts patches fsr4_shaders launcher $d/
    # FSR 4.1.1 capture tools (x86-64 only: on AArch64 FSR 4.1.1 cannot run, it needs
    # VK_VALVE_shader_mixed_float_dot_product, and fsr4cap.exe would need a MinGW cross compiler
    # built from source).
    ${lib.optionalString (!arm) ''
      mkdir -p $d/tools
      cp -r tools/fsr4cap $d/tools/
      rm -rf $d/tools/fsr4cap/__pycache__
      mkdir -p $d/tools/fsr4cap/bin
      ln -s ${dxilSpirv}/bin/dxil-spirv $d/tools/fsr4cap/bin/dxil-spirv
      install -m644 ${fsr4capExe} $d/tools/fsr4cap/bin/fsr4cap.exe
    ''}
    find $d -name __pycache__ -type d -prune -exec rm -rf {} +
    # FSR 4.1.1 models only with BB_PACKAGE_FSR411=1 (see above); otherwise run.sh finds them in
    # the data directory (~/.local/share/bbport/fsr4_411).
    if [ -d fsr4_411 ]; then
      ${pkgs.python3}/bin/python3 - <<'PY'
    import sys
    from pathlib import Path
    sys.path.insert(0, 'launcher')
    from bbport_assets import fsr411_problem
    for output in ('1920x1080', '3840x2160'):
        for preset in (0, 4):
            problem = fsr411_problem(Path('fsr4_411'), output, preset)
            if problem:
                raise SystemExit(problem)
    PY
      cp -r fsr4_411 $d/
    fi
    # A menu entry and an icon (#132, #133, #287): nix-appimage puts them at the AppImage's root
    # (.desktop, .DirIcon) for AppImage managers (Gear Lever, AppManager). The icon is the port's
    # own, not the game's art.
    install -Dm644 ${./bbport.png} $out/share/icons/hicolor/256x256/apps/bbport.png
    mkdir -p $out/share/applications
    cat > $out/share/applications/bbport.desktop <<'DESKTOP'
    [Desktop Entry]
    Type=Application
    Name=Bloodborne (bbport)
    Comment=Native PC port launcher for your own copy of Bloodborne
    Exec=bbport
    Icon=bbport
    Terminal=false
    Categories=Game;
    StartupWMClass=bbport
    DESKTOP
    sed -i 's/^    //' $out/share/applications/bbport.desktop
    # AppStream data for software centres and AppImage managers (name, summary, licence).
    mkdir -p $out/share/metainfo
    cat > $out/share/metainfo/bbport.appdata.xml <<'METAINFO'
    <?xml version="1.0" encoding="UTF-8"?>
    <component type="desktop-application">
      <id>bbport</id>
      <name>Bloodborne (bbport)</name>
      <summary>Native PC port launcher for your own copy of Bloodborne</summary>
      <metadata_license>CC0-1.0</metadata_license>
      <description>
        <p>Runs the PS4 game Bloodborne (CUSA03173, v1.09) natively on Linux from the player's own
        game files, with FSR upscaling, frame generation and the community patches.</p>
      </description>
      <launchable type="desktop-id">bbport.desktop</launchable>
      <url type="homepage">https://github.com/deadinside28/bloodborne_pc</url>
      <categories><category>Game</category></categories>
    </component>
    METAINFO
    sed -i 's/^    //' $out/share/metainfo/bbport.appdata.xml
    install -m755 out/bb-probe $d/bin/bb-probe
    install -m755 out/bb-gpu-capabilities $d/bin/bb-gpu-capabilities
    install -Dm755 out/gpu/libbbgpu.so $d/bin/gpu/libbbgpu.so
    ${lib.optionalString arm "install -Dm755 out/fex/libbbcpu.so $d/bin/cpu/libbbcpu.so"}
    ${lib.optionalString arm "install -Dm755 out/aot-cpu/libbbcpu.so $d/bin/aot-cpu/libbbcpu.so"}
    ${lib.optionalString withRecomp "ln -s ${aotTool} $d/aot-tool"}
    ${lib.optionalString (mangohudSrc != "") ''
      if [ -f ${mangohud.src}/MangoHud/MangoHud.conf ]; then
        install -Dm644 ${mangohud.src}/MangoHud/MangoHud.conf $d/mangohud/MangoHud.conf
      fi
    ''}
    # Their debug info names the build environment's headers (the full GCC, Vulkan headers): Nix
    # took those for dependencies, ~300 MB. Symbols stay (crash reports).
    ${pkgs.binutils-unwrapped}/bin/strip --strip-debug $d/bin/bb-probe $d/bin/bb-gpu-capabilities
    # The online module (gpu/bbnet20), when the build has it.
    if [ -f out/gpu/libbbnet20.so ]; then
      install -Dm755 out/gpu/libbbnet20.so $d/bin/gpu/libbbnet20.so
      ${pkgs.binutils-unwrapped}/bin/strip --strip-debug $d/bin/gpu/libbbnet20.so 2>/dev/null || true
    fi
    if [ -f out/libbbport_dlss.so ]; then
      install -m755 out/libbbport_dlss.so $d/bin/libbbport_dlss.so
      for ngx in out/libnvidia-ngx-dlss.so.* out/libnvidia-ngx-dlssg.so.*; do
        [ -f "$ngx" ] && install -m755 "$ngx" $d/bin/
      done
      [ -f out/NVIDIA-DLSS-LICENSE.txt ] && install -m644 out/NVIDIA-DLSS-LICENSE.txt $d/NVIDIA-DLSS-LICENSE.txt
    fi
    runHook postInstall
  '';
  postFixup = ''
    # bin/bbport is a static program (bbport-entry.c) that clears the host's LD_PRELOAD (Steam's
    # overlay) and similar before the wrapper's own dynamic programs start.
    mkdir -p $out/libexec
    gcc -O2 -Wall -Werror -static -L${pkgs.glibc.static}/lib -DTARGET="\"$out/libexec/bbport\"" \
      ${./bbport-entry.c} -o $out/bin/bbport
    makeShellWrapper ${python}/bin/python3 $out/libexec/bbport \
      "''${gappsWrapperArgs[@]}" \
      ${common}      --add-flags "$out/share/bbport/launcher/bbport_vulkan.py ${python}/bin/python3 $out/share/bbport/launcher/bbport_launcher.py" \
      --set BB_PREBUILT 1 \
      --set PYTHON ${pkgs.python3}/bin/python3 \
      --set BB_BUNDLED_VK_DRIVER_FILES ${icds} \
      --set BB_BUNDLED_VK_LAYER_PATH ${bundledLayers}/share/vulkan/implicit_layer.d \
      --prefix PATH : ${lib.makeBinPath [ pkgs.bash pkgs.coreutils pkgs.util-linux pkgs.procps
                                          pkgs.gawk pkgs.gnused pkgs.gnugrep pkgs.spirv-tools pkgs.p7zip
                                          pkgs.systemdMinimal ]} \
      --run 'export BB_DATA_DIR=''${BB_DATA_DIR:-''${XDG_DATA_HOME:-$HOME/.local/share}/bbport}; mkdir -p "$BB_DATA_DIR"'
    # The game alone, without the launcher (settings from the data directory's bbport.ini).
    makeShellWrapper ${pkgs.python3}/bin/python3 $out/bin/bbport-game \
      ${common}      --add-flags "$out/share/bbport/launcher/bbport_vulkan.py ${pkgs.bash}/bin/bash $out/share/bbport/run.sh" \
      --set BB_PREBUILT 1 \
      --set PYTHON ${pkgs.python3}/bin/python3 \
      --set BB_BUNDLED_VK_DRIVER_FILES ${icds} \
      --set BB_BUNDLED_VK_LAYER_PATH ${bundledLayers}/share/vulkan/implicit_layer.d \
      --prefix PATH : ${lib.makeBinPath [ pkgs.bash pkgs.coreutils pkgs.procps ]} \
      --run 'export BB_DATA_DIR=''${BB_DATA_DIR:-''${XDG_DATA_HOME:-$HOME/.local/share}/bbport}; mkdir -p "$BB_DATA_DIR"'
  '';
  passthru = { inherit mangohud turnip; };
  meta.mainProgram = "bbport";
}

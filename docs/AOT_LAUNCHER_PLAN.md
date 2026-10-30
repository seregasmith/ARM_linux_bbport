# Native CPU (AOT) from the Linux ARM launcher: plan (2026-10-30)

Branch `aot`. The launcher on the handheld (aarch64 Linux, Snapdragon 8 Elite) gets a button that compiles the game's
x86-64 code ahead of time into `libbbaot.so` with bb-recomp (the Bloodborne Android Rebuild Tool, ducvd89), tuned for the
fastest code on that device, and the game then runs it instead of FEXCore's JIT.

This file is also the hand-off for the next work session: the facts below were checked in the sources (file:line), so
they need not be read again. Update "Status" and tick the checklists as work lands.

## Status

| | |
|---|---|
| `build_aot_docker.sh` (root) | builds bb-recomp in Docker and links a glibc `libbbaot.so` from a copied `boot-linked.bin`; since 2026-10-30 it compiles the tool's variant catalog too (`BB_AOT_VARIANTS=0`: not). Container steps ran on a made-up image only; **never run under Docker, never on the real game** |
| Runtime that loads `libbbaot.so` | **ported 2026-10-30, not run yet** (phase 1 below): `cpu/aot/` = the Android port's AOT-only engine, built by `build.sh` into `out/aot-cpu/libbbcpu.so` next to FEX's; `run.sh` picks it with `BB_AOT`. Compiles and links (gcc 11, aarch64); `tests/run_aot_native_test.sh` passes; nothing ran with a real library |
| Android port (`~/Work/bbport/Bloodborne-Android`) | checked out (2.2.0, f241353). A read-only reference: nothing of it is built for Android or shipped |
| Rebuild tool checkout | 1.3.0 (6ebb33b): hook sites of the upstream 0.7 fixes, delta rebuilds |
| Launcher feature | **written 2026-10-30, not run on the device yet**: Game -> Native CPU (`launcher/bbport_launcher_aot.py`, aarch64 only), `scripts/aot_build.py` (the build on the device), `--build-aot`. Tested with a stand-in bb-recomp (progress, failure, stop/resume, misses) and GTK mocked, not in a real window |
| bb-recomp for the handheld | **in the aarch64 AppImage** since 2026-10-30 (`packaging/bb-recomp.nix` -> `aot-tool/` in the package; `build_docker.sh --arch aarch64 appimage` mounts the Rebuild Tool checkout next to this tree). The official LLVM 23.1.3 release as a pinned nix download (sha256 checked), nixpkgs' clang, the package's glibc. **Never built yet** (no nix here; a rough syntax check only). The other way, `./build_aot_docker.sh device-tool` + a copy to `~/.local/share/bbport/aot-tool`, stays |

## Never

- Commit, ship or upload `libbbaot.so`, objects or `boot-linked.bin`: they are the game's code. Outputs go outside the
  repositories (`~/Work/bbport/aot-out/`, or the data folder `~/.local/share/bbport/aot/` on the handheld).
- Change the tool's repository in place: copy it, or keep patches in this repo (`patches/bb-recomp/`), and mark the
  modified build (ADDITIONAL_TERMS.md 7(c)). Keep the tool's About notice / attribution (7(b)); do not brand anything with
  the tool's name (7(e)).

## Sources (what each one holds)

Rebuild tool (`../Bloodborne-android-rebuild-tool`, version 1.3.0, ABI 2):

| file | holds |
|---|---|
| `docs/RECOMP_ABI.md` | the runtime contract: unit table, `bbr_*` helpers the runtime provides, dirty units, variants, hook slots, granular entries, misses log, `BB_AOT_ONLY` |
| `docs/RECOMP_HOOKS.md` | inventory of every code writer of the Android port and how each is handled (variant / hook slot / fallback). Template for the fork's inventory |
| `docs/NATIVE_CPU.md`, "The phone build" | the on-device command line, `BBR-PROGRESS` stages and weights, exit codes, SIGTERM stop, `--resume` |
| `docs/STATIC_RECOMPILE_PLAN.md` | measurements: compile time, `.text` size, RAM model, discovery coverage |
| `docs/COMPILER.md` | every command line mode, tests |
| `arm64/build.sh`, `arm64/README-ANDROID.md` | the on-device builds (`android`: bionic; `aarch64`: fully static glibc) |
| `src/codegen.cpp:43-66` | target machine: triple `aarch64-unknown-linux-android30`, `cpu=generic`, `+v8.2a,+lse,+crc,+fp-armv8,+neon` |
| `src/lifter.cpp:124-142` | `--host` profiles (FEX HostFeatures only) |
| `src/compile.cpp:850-875`, `src/bundle.cpp:120-166` | the link of the `.so` and how the bundled build replaces its inputs |
| `variants/*.txt`, `variants/gen_*.py` | variant/hook catalogs, generated from the **Android** repo (`_mainrepo.py` requires `tools/android/build_patch_parts.py`); 1.3.0 added the upstream 0.7 fixes' sites (`hooks-patchers.txt`: cloth 0x1167a80, GX occlusion guard 0xe2f134/0xe2f164/0x21675b0, play time 0x19419de, hit collector 0xb84ce0, character id 0x1c43ee0) |
| `docs/COMPILER.md`, "Delta rebuild" | 1.3.0: per-shard resume keys include only the hook sites in the shard, so a changed catalog rebuilds only the shards it reaches; `--manifest`; `--clean-work` deletes what a delta build needs |

This fork:

| file | holds |
|---|---|
| `src/guest_cpu.h` | the guest CPU interface (no invalidate, no hook-slot calls yet) |
| `cpu/guest_cpu_fex.cpp`, `cpu/bbcpu.cmake`, `build.sh:90-105` | libbbcpu.so on FEXCore, how it is built (`-DTUNE_ARCH=armv8.2-a`) |
| `run.sh:73-84`, `run.sh:328` | image build at every launch (`mods.py`, `prepare.py`, `link_libc.py`, `link_modules.py`), then `bb-probe out/boot-linked.bin --patches out/patches.bin ...` |
| `src/probe.c:739-781` | start-up order: `guest_cpu_code`, relocations, `apply_patches`, `bbgpu_patch_image` |
| `launcher/bbport_launcher_upscalers.py:236-301` | the FSR 4.1.1 builder: the pattern to copy (Gio.SubprocessLauncher + `setsid`, line reader, cancel, exit codes) |
| `launcher/bbport_launcher_common.py` | `PORT_DIR` :27, `PACKAGED` :36, `DATA_DIR` :37, `FSR411_BUILD_ERRORS` :53, `fsr411_build_command` :397, `game_environment` :425 |
| `launcher/bbport_launcher.py:1060-1074` | `--play`, `--build-fsr411` (headless modes) |
| `launcher/bbport_i18n.py` | translations; **the Russian text is the key** (`tr("...")`), add EN/pt_BR/zh_CN/fr_FR entries |
| `launcher/settings_schema.json` | sections and items in 5 languages, shared with the Windows launcher (`platforms: ["linux"]` for this one) |
| `packaging/appimage.sh:16-19` | aarch64 AppImage bundles `out/fex/libbbcpu.so` |
| `Claude outputs/odin_profile_remote.sh` | the measuring script for the handheld (FPS, CPU per thread, clocks, temperature per minute) |

Android port (`../Bloodborne-Android`, 2.2.0 f241353, GPL-2.0-or-later like this fork; a fork of bbport, read only):

| file | holds |
|---|---|
| `cpu/` | the AOT-only guest CPU (`guest_cpu_aot.cpp` 1,139 lines, `_hooks.inc`, `guest_cpu_native.cpp`, cpuid/atomics/state headers, `CMakeLists.txt`); last change 3d8abf7. Copied unchanged to `cpu/aot/` here |
| `src/guest_cpu.h` | the interface with the AOT calls (`guest_cpu_call_floats`, hook slots, `guest_cpu_invalidate`, `guest_cpu_is_native`, `guest_cpu_set_panic_text`); taken over here |
| `build.sh:74-84` | builds `cpu/` on AArch64 Linux hosts (plain glibc: the runtime is not Android-specific) |
| `gpu/shim/bbport_hook_sites.h`, `bbport_frame_rate.cpp`, `bbport_display.cpp`, `bbport_guest_guards.cpp`, `bbport_engine_fixes.cpp` | the code writers as native hook slots (`Slot(...)`); the frame-rate one is the next port (phase 1b) |
| `gpu/shim/bbport_guest_code_hooks.cpp:310-345, 559-657` | `SlotAdapter` and slot-first `InstallGuestCodeHook`; ported here |
| `tests/test_aot_*.cpp`, `aot_test_services.h` | the engine's unit tests (strict/variants: x86-64 hosts only); copied to `tests/` here |
| `docs/AOT_DEVICE_STATUS.md` | Thor results: AOT vs FEX, 0 misses, CPU per thread; bugs found on the device |
| `docs/STATIC_RECOMPILE_PLAN.md`, "Phase 7 status" | FEX removed there: strict mode only, hooks only in slots, what was switched off |
| `docs/AOT_RUNTIME_HANDOFF.md` | the older hybrid (AOT + FEX fallback, branch recomp-rt): history only |

## Verified facts that shape the design

1. **Compile from the fork's own image.** The tool's player modes (`--game`, `--pkgs`) build the image with the Android
   app's `bb-prepare` (`shared/prepare`), so its hash may differ from this fork's. Always run the fork's prepare scripts
   and pass `--boot <boot-linked.bin> --game-dir <game>`. `boot-linked.bin` does not depend on settings: patches go to
   `patches.bin` and are applied at start-up (`probe.c:781`). Assumed deterministic for the same game files, mods that touch
   `eboot.bin`/`sce_module`, and script version.
2. **The tool runs on the handheld.** `arm64/build.sh aarch64` makes a fully static glibc AArch64 executable with LLD in
   the process (`BBR_BUNDLED`). Android 16's seccomp kill does not apply on Linux. Thor (8 Gen 2): `--jobs 4`, 3.8 GB peak,
   5 min 55 s. RAM: about 0.5 GB + 0.8 GB per job. Work folder: ~1 GB; result ~125 MB.
3. **A glibc library without changing the tool.** In the bundled build the link takes four files from `--bundle-dir`
   (`bundle.cpp:138-160`): `helpers-aarch64.o` (copied instead of compiled), `libbbcpu-stub.so` (soname `libbbcpu.so`:
   the same name as this fork's FEX library, so it fits), `libm.so` (DT_NEEDED comes from its SONAME), and
   `libclang_rt.builtins-aarch64-android.a`. A **glibc bundle**: helpers compiled with `--target=aarch64-linux-gnu`
   (as `build_aot_docker.sh` does), glibc's `libm.so.6` from the Ubuntu 22.04 arm64 sysroot (`fetch-sysroot.py`), the
   stub and builtins unchanged. Objects keep the android30 triple; the Docker script already links those for glibc.
   `normaliseVersym` only marks undefined unversioned symbols global: harmless.
4. **Progress, cancel, resume exist.** `--progress` prints `BBR-PROGRESS <stage> <done> <total>`, `BBR-ERROR`,
   `BBR-DONE <status>`. Weights: scan 1 %, discover 2 %, lift 2 %, compile 93 %, variants 1 %, link 1 %. SIGTERM stops
   at the next function (exit 143), finished shards stay; the same command resumes. Only `BBR-DONE 0` is a result.
   Keep `--shards 64` fixed: then the bytes do not depend on `--jobs` or free RAM.
5. **Codegen ignores the device.** `codegen.cpp:53-54` hard-codes `generic` / `+v8.2a`; `--host` changes only the lifter
   (`armv9` adds FlagM2 and FRINTTS). **Do not use `armv9` with bb-recomp 1.3.0** (found 2026-10-30, first library on the
   handheld): with FRINTTS FEX's front end turns `cvttss2si`/`cvttsd2si` into `Vector_FToISized`
   (`OpcodeDispatcher/Vector.cpp`, `CVTFPR_To_GPRImpl`), which the tool does not lower: 2431 units got `bbr_trap` and no
   native code (compile.txt, "unsupported ops"), and the game stopped at the first, libc's sinf (+0x571fc60), with the
   runtime's misleading "patched ... has no variant" (now `kind=unsupported`). `armv8.4` is the tool's default and the
   profile its own builds use. `TargetSpec` already has `cpu` and `features`, and the
   resume key hashes them (`compile.cpp:633`): a `--cpu <name|native>` / `--mattr <features>` patch is a few lines in
   `main.cpp` (+ `sys::getHostCPUName()` for `native`, as the x86 path does).
6. **-O2 is unmeasured at run time.** At build time it costs +10 % wall and gives no smaller code (plan, "Compile
   time"); nobody measured game speed. `--no-dirty-check`: -8.5 % `.text`. `--resume-entries`: +23 % `.text`.
7. **The AOT CPU has no fallback.** A function patched without a variant has no native code: entering it stops the game
   (`STOP: AOT: ...`, exit 22, the address in the misses log); a hook works only in a slot the library has. What writes
   code on aarch64 in this fork (checked 2026-10-30):
   - `InstallGuestCodeHook` (`hooks_jobs.cpp`, `hooks_gx.cpp`, `hooks_heap.cpp`, `bloodborne_kit_restore.cpp`,
     `bloodborne_menu_pointer.cpp`, `bbport_engine_fixes.cpp`, `bbport_gnm_hooks.cpp`, BBSC) returned false on every
     non-x86 host before: nothing written. Now it uses a slot when the AOT library has one at the site, else stays off.
   - `hooks.cpp` `Patch` -> `guest_cpu_hook`: a trap with FEX; slot or refused with AOT (its sites are the catalog's
     `hooks-shim.txt`, the same addresses).
   - `bbport_frame_rate.cpp`: 6 detours + 48 operand redirects with jumps to an x86 stub page. With AOT `run.sh` sets
     `BB_FRAME_HOOKS=0` (the community frame-rate patches, a variant in the catalog if the bytes match) until the
     Android port's slot version is ported (phase 1b).
   - `patches.bin` (`scripts/patches.py`, 549 lines here, 389 there): variants only where this fork writes the same bytes
     as the Android parts. Unchecked: the first run with `BB_AOT_DEBUG=1` lists the patched units.
   - The game menu pages stay off on aarch64 (`bbport_game_menu.cpp:768`); display/aspect hooks: none here yet.
8. **Expected gain, measured on the Thor** (Android `docs/AOT_DEVICE_STATUS.md`, 60 s at a save spot, capped): AOT 29.7 FPS
   at 325 % of a core for the process, FEX 29.4 FPS at 376 %; guest threads 113.6 % vs 124.0 %, FMOD mixer -35 %, GX workers
   -10 %; 0 misses, 0 dirty entries with the catalog. GPU-bound there, so the same FPS: expect less CPU, heat and power and
   no JIT stalls; more FPS only where CPU-bound.
9. **License: no issue.** The Android port is GPL-2.0-or-later (SPDX headers, LICENSE), like this fork. The FEX-derived
   CPUID model (`cpu/aot/guest_cpu_cpuid.h`) is MIT: keep FEX's notice with the package.
10. **Two CPUs, one interface.** FEX's `libbbcpu.so` and the AOT one export the same functions under the same soname, so
    bb-probe (linked against FEX's, RUNPATH `$ORIGIN/gpu:...:$ORIGIN/cpu:...`, new dtags) runs with either; `LD_LIBRARY_PATH`
    is searched before RUNPATH, so `run.sh` selects the AOT one by prepending `out/aot-cpu` (packaged: `bin/aot-cpu`).
11. **The engine checks the library itself** (`cpu/aot/guest_cpu_aot.cpp`: `PrepareImage` :645, `Load` :688): ABI 2, then at the first guest call every unit's
    hash against the patched image; more than a quarter different -> "built from another build of the game", stop.

## Design

### The device build (done 2026-10-30): why on the handheld

The game files live on the handheld; copying them to the Mac for `build_aot_docker.sh` is the hard part. So the library is
built where the game is.

**The compiler comes with the AppImage** (`packaging/bb-recomp.nix`, aarch64 packages): `default.nix` builds it when the
Rebuild Tool's sources are at hand (`BB_RECOMP_SRC`, or `../Bloodborne-android-rebuild-tool`; `build_docker.sh` mounts the
checkout at `/recomp-src`; `BB_PACKAGE_RECOMP=0`: without) and links it as `share/bbport/aot-tool`, which
`scripts/aot_build.py` looks for right after `--tool` and `$BB_RECOMP_DIR` (before `<data>/aot-tool`). How it is built:

- LLVM: the official 23.1.3 release, `LLVM-23.1.3-Linux-ARM64.tar.zst` (`fetchurl`, sha256 `1154f37e...c82f01`, 1,111,058,298
  bytes, measured 2026-10-30), unpacked without fixup and only in part (the archive is decompressed twice): LLVM's CMake
  package, the LLVM and LLD headers, the files `LLVMExports-*.cmake` names, LLD's libraries, compiler-rt's AArch64
  builtins; the exported programs are empty files (never run; the CMake package only checks they exist). A few GB in the
  Nix volume instead of 12 (2026-10-30: the first try filled the Mac's disk, 136 MB left, and Docker failed with I/O
  errors); never in the AppImage: `disallowedReferences` stops the build if bb-recomp keeps a reference to it.
- zlib 1.3.1 and zstd 1.5.7 static (`fetchurl`, the tool's `fetch-deps.sh` versions) as `BB_RECOMP_DEPS_PREFIX`.
- FEX: this tree's `third_party/FEX` without its test binaries, copied into the build (its build writes beside it).
- `pkgs.clangStdenv`, hardening off, `-ffp-contract=off` (as the tool's Android build), `-DBB_RECOMP_BUNDLED=ON
  -DBB_RECOMP_NO_X86=ON -DBB_RECOMP_MARCH=armv8-a`. The tool's sources are not changed; this build's copy of `bbrecomp.cmake`
  drops `-static` (no static glibc in nixpkgs' stdenv): bb-recomp links the package's glibc and libstdc++ dynamically, which
  the AppImage carries.
- The bundle (`share/bb-recomp`): the x87 helpers compiled with the same clang, an empty `libbbcpu.so` stub, the package's
  `libm.so.6` (the library's DT_NEEDED and symbol versions match the AppImage's glibc), compiler-rt's builtins from the
  release; the tool's `variants/`, `licenses/`, `VERSION`, `README.txt`.
- First build: the 1.1 GB download, FEX's front end and the tool (10-30 minutes); afterwards cached in the Nix volume.

**Without the AppImage's copy** (an AppImage built without the tool's sources, a source checkout): the Mac makes the
compiler once, with no game files:

1. Mac: `./build_aot_docker.sh device-tool` -> `../aot-tool/` (`bin/bb-recomp`: the tool's own static aarch64 build;
   `share/bb-recomp/`: the four link inputs of its in-process LLD for glibc under the Android names (fact 3), the variant
   catalog; `licenses/`, `README.txt`, `VERSION`). On Apple Silicon Docker runs arm64 natively: `arm64/build.sh aarch64`
   runs on a copy of the tool's sources in the volume, with the volume's ARM64 LLVM standing in for its "x86-64 LLVM"
   (it only runs clang, lld, llvm-ar from it).
2. `scp -r aot-tool <handheld>:.local/share/bbport/` (or the launcher's *Choose the aot-tool folder*).
3. Handheld, launcher: Game -> Native CPU -> **Build** (or `--build-aot`), then Play with the switch on.

### Launcher (Linux, aarch64 only: on x86-64 the code runs natively)

Done as a group built by code (`build_native_cpu_group`, not `settings_schema.json`): the switch (`native_cpu` ->
`BB_AOT=auto`), the library row (status from `aot_build.status()`: Ready with date/-O/host, built by another tool
version, N new misses -> rebuild, interrupted, not built; Build / Rebuild / Cancel; progress from `BBR-PROGRESS` with
the stage weights and minutes left), the tool row (where it was found, or how to make it; *Choose folder*). The game's
Play button is off while a build runs and Build while the game runs; closing the window stops a build (it resumes).
Strings: English only beside the Russian keys (pt_BR, zh_CN, fr_FR fall back to English). Not done from the list below:
the profile choice (it is automatic: `--host` from `/proc/cpuinfo`, -O1), the charger check, other game folders and PKGs.

The first design, for reference:

- **Status row**: Ready / Not built / Built for another game image / Built by another tool version / Build interrupted
  (resumable). From `$DATA_DIR/aot/<sha256 of boot-linked.bin>/build.json` (tool version, options, date). The launcher
  compares sha256 (hashlib, fast); the runtime does the real `bbr_image_hash` (FNV-1a) check at load.
- **Source**: the game folder of the Game section by default; *Choose folder…* takes another game folder (PKG folders:
  phase 5).
- **Build / Cancel**: one bar with the stage weights, ETA from the finished fraction over time, a note to connect the
  charger (read `/sys/class/power_supply/*/online`). Cancel = SIGTERM to the process group; next Build resumes.
- **Profile**: *Max performance* (default: `--host armv8.4 --cpu native`, the opt level chosen in phase 4; armv9 only
  once the tool lowers `Vector_FToISized`, fact 5) /
  *Compatible* (`--host armv8.2`, generic code: a library that also runs on other ARM devices).
- **Use native CPU** switch (`settings.json`); off = FEX as today.
- **Missing entries**: after the game exits, if `$DATA_DIR/logs/aot-misses.log` has addresses not in the last build,
  show "N missing native entries: rebuild" (adds `--extra-entries`).
- **Headless**: `--build-aot [game dir]` next to `--build-fsr411`, printing the same progress, so SSH and Steam game mode
  work.
- Strings: Russian keys in `tr()`, translations in `bbport_i18n.py`; gamepad navigation must reach the new rows.

### `scripts/aot_build.py` (done; its docstring is the reference)

As built: `game_check`; the three image scripts into `<data>/out` exactly as `run.sh` runs them (no mods view: a mod
that replaces `eboot.bin` or a module gives another image, and `run.sh` then finds no library); sha256 -> `<data>/aot/<sha>/`;
the image copied into `work/image/` (resume and delta builds see the same bytes); the four game files as links in
`<data>/aot/game-files/` (bb-recomp splits internal commands on spaces); misses from `logs/aot-misses.log` kept in
`<sha>/extra-entries.txt` (misses-log format) for every later build; `--host` from `/proc/cpuinfo` (flagm+ilrcpc ->
armv8.4, else armv8.2; never armv9 by itself, fact 5); `recipe` in build.json (`RECIPE`, 2 since the armv9 fix: an older
library shows as to be rebuilt and the launcher leaves `BB_AOT` unset); `trap_units` from compile.txt; jobs = min(cores - 1, (MemAvailable - 1.5 GB) / 0.8 GB). Last line `AOT-DONE <status>
<library|reason>`; exit 0 / 1 / 2 / 3 no tool / 4 not the game / 130, 143 stopped. Only the newest two images are kept.
Not done: the ELF check (the tool checks and writes libbbaot.json; ABI 2 is checked from it).

The first design, for reference:

1. `game_check.py` on the folder; `mods.py` view as `run.sh` makes it.
2. `prepare.py`, `link_libc.py`, `link_modules.py` into `$DATA_DIR/aot/work/image/` -> `boot-linked.bin`, `link.json`;
   sha256 of it = the cache key. A finished library for that key: done.
3. Jobs: `min(cores, (MemAvailable - 2 GB) / 0.8 GB)`, at least 1.
4. ```
   bb-recomp --boot <image>/boot-linked.bin --game-dir <game> --bundle-dir <glibc bundle> --variants <fork catalog> \
       --so $DATA_DIR/aot/work/libbbaot.so --stats $DATA_DIR/aot/work/compile.txt \
       --shards 64 -O<1|2> --host <armv9|armv8.2> [--cpu native] [--extra-entries <misses log>] \
       --jobs <n> --no-nice --progress --resume
   ```
   No `--clean-work`: the kept shards make the next build a delta build (1.3.0; a new catalog or `--extra-entries`
   rebuilds only the shards they reach).
   Relay `BBR-PROGRESS` lines unchanged to stdout (the launcher parses them).
5. On `BBR-DONE 0`: ELF check (the `elfcheck.py` logic in `build_aot_docker.sh`: AArch64, ABI 2, exports), move
   `libbbaot.so` + `libbbaot.json` + `compile.txt` to `$DATA_DIR/aot/<sha>/` (rename, atomic), write `build.json`, delete
   the work folder. Keep only the newest two keys.

### `run.sh` (done, `run.sh:86-122`)

After `content_profile.py`, on aarch64 (`$HOSTTYPE`: the AppImage's PATH has no `uname`) with `BB_AOT` set and not `0`:
`BB_AOT=auto` -> `$data/aot/<sha256 of out/boot-linked.bin>/libbbaot.so`; a missing library or a missing
`out/aot-cpu/libbbcpu.so` (packaged: `bin/aot-cpu/`) prints why and FEX runs. Otherwise it exports `BB_AOT` (absolute),
prepends the AOT CPU's folder to `LD_LIBRARY_PATH`, sets `BB_AOT_MISS_LOG=$data/logs/aot-misses.log`, and `BB_FRAME_HOOKS=0`
unless set (fact 7). The launcher's switch will set `BB_AOT=auto` (or `0`) through `game_environment`; until then the
environment does (`BB_AOT=auto ./Bloodborne-bbport-aarch64.AppImage`, or Steam's launch options).

### Shipping bb-recomp

Done: in the AppImage (above). The first idea, for reference: `arm64/build.sh aarch64` (cross-built on an x86-64 host; needs the x86-64 LLVM 23.1.3 tarball and the FEX tree:
`fetch-deps.sh`, `fetch-fex.sh`) plus the `--cpu`/`--mattr` patch -> `bin/bb-recomp`, `share/bb-recomp/` (glibc
bundle), `share/bb-recomp/variants/` (fork catalog), its LICENSE, NOTICES, ADDITIONAL_TERMS.md in the AppImage
(`packaging/appimage.sh`). About text in the launcher: tool name, author ducvd89, repository, license, "modified".
Option: a `tool-static` mode in `build_aot_docker.sh` to make it.

### Runtime (done 2026-10-30, untested in the game)

Two guest CPUs instead of a hybrid: the Android port removed FEX (its phase 7), and its AOT engine is the one tested on a
device, so it is taken over unchanged and FEX stays as the other library (the launcher switch / `BB_AOT` chooses).

- `cpu/aot/`: the Android `cpu/` files, unchanged (sync with `cp` when they fix something there), and a `CMakeLists.txt`
  of ours. `build.sh` builds it on aarch64 into `out/aot-cpu/` (`BB_AOT_CPU=0`: skip). Not `out/cpu/`: that is FEX's
  place in the package (`$ORIGIN/cpu`).
- `src/guest_cpu.h`: the Android header (comments say which CPU does what). FEX (`cpu/guest_cpu_fex.cpp`) implements the
  new calls (`guest_cpu_invalidate` -> its code invalidation, `guest_cpu_call_floats`, no slots, not native); x86-64
  (`src/runtime_cpu.c`): the Android stubs.
- `gpu/shim/bbport_guest_code_hooks.cpp`: on non-x86 hosts `InstallGuestCodeHook` sets a native slot when the library
  has one (Android's `SlotAdapter`), else stays off as before. With FEX nothing changes.
- Packaging: `bin/aot-cpu/libbbcpu.so` in the AppImage (`packaging/default.nix`, `appimage.sh`).
- Strings in the engine still name "the app" and the Android tool; changing them means a local diff to keep: leave them
  until the launcher shows its own messages.

### Fork variant catalog

Inventory in the form of `RECOMP_HOOKS.md` section 1 for every writer listed in fact 7 (site, size, fixed or computed
bytes, when, condition, handling). Fixed bytes -> variant groups (`group`/`w`/`c`, `excl=` for alternatives: e.g. every
`patches.py` part). Computed bytes (stubs with run-time addresses) -> hook slots (`hook <vaddr> <len> snap|regs ...`)
and the writer changed to use the slot. Generators in this repo (`tools/aot_catalog/`), reading this fork's sources; a
`--check` mode for CI. Diagnostic hooks (Gnm census, free check) stay unhandled.

## Performance levers (measure each against FEX with `odin_profile_remote.sh`, same route)

| lever | expected | cost | default |
|---|---|---|---|
| AOT itself | no JIT stalls, cheaper host calls, less CPU per frame | phases 1-3 | on |
| fork catalog: variants + hook slots | per-frame hooked code (frame rate hooks) stays native | biggest item | on |
| `--cpu native` (8 Elite: `oryon-1`); `--host armv9` once the tool lowers `Vector_FToISized` | better scheduling, newer instructions | tool patch | Max profile |
| `-O2` vs `-O1` | unknown at run time | minutes per build | decide in phase 4 |
| hot code ordering (`--symbol-ordering-file` from a perf profile of the units) | fewer i-cache / iTLB misses over ~100 MB `.text` | profile script + a link flag (tool patch) | later |
| huge pages for the library's text | fewer iTLB misses | runtime remap | experiment |
| `BB_AOT_ONLY` (no FEX in the process) | smaller process, no fallback machinery | zero misses | later |
| link `-z pack-relative-relocs` | faster load (184 k unit table pointers) | tool patch | experiment |
| `--no-dirty-check` | -8.5 % `.text`, one load + branch less per unit | unsafe with code writes | off |
| `--resume-entries` | fewer fallbacks after stack switches | +23 % `.text` | off unless misses need it |

## Phases

1. **Runtime.** Library from `build_aot_docker.sh` (copy `boot-linked.bin`, `link.json` and the four game files from the
   handheld, see `docs/BBPort android repack.md`).
   - [x] clone `ducvd89/Bloodborne-Android` (2.2.0), license checked (GPL-2.0-or-later)
   - [x] `cpu/aot/`, `src/guest_cpu.h`, FEX and x86 stubs, slot-first `InstallGuestCodeHook`, `build.sh`, `run.sh`, packaging
   - [x] compile check on aarch64 (gcc 11: engine, hooks file, runtime C files; FEX file syntax only, it needs clang);
     `tests/run_aot_native_test.sh` passes
   - [ ] `bash build.sh` in the real environment (nix-shell / `build_docker.sh --arch aarch64`): `out/aot-cpu/libbbcpu.so`
   - [ ] the library: on the handheld (phases 2-3 below, the way Sergey can test: the game files are there), or
     `build_aot_docker.sh` on the Mac with files copied from the handheld
   - [ ] on the handheld: run with `BB_AOT=auto BB_AOT_DEBUG=1 BB_AOT_STATS=1` (the launcher's switch sets `BB_AOT`), online
     off and 16:9; read "AOT: image checked: ... differ (patched or hooked), ... have no native code", the patched units,
     and `logs/aot-misses.log`
   - [ ] each stop: a missing entry -> rebuild with `--extra-entries`; a patched function without a variant -> find its
     writer (fact 7) and make it a variant (catalog) or a slot, or switch it off under AOT
   - Run 2 (2026-10-30, armv8.4): 0 trap units, 29 functions on patch variants, the game reached GPU memory set-up. Found:
     (a) the scene size: `patches.py` wrote 1064x600 into the resolution patch's two `mov eax, imm32` (+0x1d96a6b,
     +0x1d96a7a), the catalog's `part:res-scene` has the template's 1280x720, so +0x1d969f0 had no variant. Fixed as the
     Android port does (`bbport_display.cpp`, PatchSceneSize): `patches.py --native-cpu` (run.sh passes it with a library)
     keeps the template there and `src/probe.c` `scene_size_slots` loads BB_RENDER_RES into eax through the hook slots
     `hooks-patchers.txt` declares at both sites. (b) `miss +0x2034831 kind=jump`: a case of the frame pacer's switch
     that `part:fps=uncap` rewrites (the patched function runs on its variant, which reaches that case through the
     dispatcher). The misses loop: Rebuild adds it as an entry (the variant's lift takes the original's entries). Its
     siblings (+0x203480f, +0x2034820, +0x203484b, +0x203486f, +0x20348a4, same 9-byte writes) may follow one run each:
     not seeded by hand, an entry inside an instruction would give the function a trap unit and drop its variants.
   - Run 3: +0x2034831 fixed by the rebuild; the game ran into the frame loop and stopped at `miss +0xfd2e18 kind=jump`.
     That is a cave of `part:fps=uncap` (+0xfd2e16 `jmp +0x19` skips 25 bytes, new code at +0xfd2e18..+0xfd2e2f,
     `jmp +0xfd3562` back); +0xfd355d jumps into it from what the tool takes as another function. bb-recomp's variant
     fixpoint (`variant_compile.cpp`) adds entries only for targets inside the variant's own members, so the cave's
     function gets no unit there: a tool limitation worth reporting to ducvd89 (the Android port runs its frame-rate
     hooks, not fps=uncap, so it never meets it). Workaround: the misses loop. Expected next: +0xfd3562 (the jump back,
     an original instruction boundary). The real fix is phase 1b (frame rate through hook slots, as the Android port).
   - Run 3's log also showed: scene size through slots works ("Display: the game's scene size 1064x600 (native hook
     slots ...)", 30 variants, 0 patched without one); and the image at 0x800104000, not 0x800000000 (the misses log's
     "image base"): cpu/aot's `InitStubs` takes 1 MiB of the low region in `guest_cpu_init`, before the loader maps the
     image, so the port's fixed-base code (bbport_engine_fixes, heap sites, free check, runtime_kernel.c's text ranges, a
     GOT read at 0x800000000 + offset) looked beside the code: "Engine fixes: ... not installed (other code there)".
     Fixed: `guest_cpu_init(runtime_low_map_top)` (src/runtime_memory/low.c), the CPU's own memory from the top of the low
     region down. Under AOT 3 of the 4 engine fixes then run in slots (hooks-patchers.txt has +0x19419de, +0xb84ce0,
     +0x1c43ee0; the light grid's +0x229e990 has none). Offsets are image-relative: no library rebuild for this.
   - Run 4 (2026-10-30 21:01/21:02): the previous AppImage (image at 0x800104000) runs **30 FPS** on native code to the
     menu (14 variants, 0 misses). Uncapped stops at `+0x2034520 kind=patched` after the +0xfd2e18 rebuild (the frame
     pacer's fps=uncap variant no longer built or matched: read compile.txt's variant notes) and +0xfd3562. The new
     AppImage (image at 0x800000000) stopped at once, also at 30 FPS: `outside 0xffff...1860` from libc's malloc. With
     the right base `BbHeapSites` installed its counters (gnm_driver.cpp WatchOverlapLeak) and wrote host function
     pointers into the game's malloc table. Fixed: the heap-site wrappers are x86-64 only. Also: the engine fixes were
     installed before the CPU knew the image (bbgpu_init ran before `guest_cpu_code`), so no slot was found at any
     offset; `guest_cpu_code(image)` now comes right after the image is read.
   - Sergey: no performance difference (CPU use the same). As fact 8 says, the Android author measured the same on the
     Thor (GPU-bound; host threads dominate). Next: a per-thread profile (FEX vs AOT, same spot, uncapped) before more
     AOT work; frame rate through slots (1b) for uncapped play.
   - [x] The profile (2026-10-30): `scripts/bbport_profile.py` (record / summary / compare; /proc and /sys only: per-thread
     CPU split into the game's threads (named in the log's "guest thread '...' created"), the main thread and the port's,
     each core's load and clock, GPU busy from DRM fdinfo (`drm-engine-*`, msm/amdgpu/i915/panfrost) or kgsl, thermal
     zones, battery power, memory, FPS from `Present rate:` (BB_FPS_PRINT) or `Frame stats:`), and Developer ->
     "Record a performance profile" (`launcher/bbport_launcher_profile.py`: every game start into
     `<data>/profiles/<time>/`, summary.txt into the log at exit, "Compare the last two"). tests/test_bbport_profile.py.
     To do on the handheld: FEX uncapped at a fixed spot (what limits the frame rate: the summary's "What limits it");
     FEX and native at 30 FPS at the same spot (CPU, temperature, power).
   - Results (2026-10-30, 1064x600 scene): native and FEX the same (30 FPS: 242 vs 242 % CPU; uncapped ~58 FPS both,
     GPU 85-99 % busy at 1.1 GHz: GPU-bound). Only the FMOD mixer (-14..-24 %) and memory (-10 %, no JIT cache) differ.
     GPU profile: 19.5 ms/frame; SSR patch 1.6 ms, VBAO about 1 ms (its label also held the game's SSAO, 0.1 ms: now left
     out under VBAO, docs/vbao.md),
     copy shader fefebf9f 0.9 ms, FSR 0.7 ms, the rest spread over ~200 passes. At a cap the main thread is ~90 % busy under
     both CPUs: the pacer (+0x2034770) spins on gettimeofday (+0x2034b08; its sleeping loop is off in the release build).
     `src/runtime_time.c` now sleeps there until BB_PACER_MARGIN_US (+ recent wake lateness) before the pacer's own
     deadline (BB_PACER_SLEEP=0: the spin). Measured at 30 FPS (native, same spot), 1 ms margin: main thread 90 -> 34 % of a
     core, process CPU 245 -> 193 %, max temperature 95 -> 78 °C (CPU 81 -> 66 °C), battery 5.8 W (uncapped: 11.7-13.3 W).
     The sleeps wake on time, but 50 ms frames grow as the margin shrinks (per minute: spin 3, 4 ms 14, 2 ms 29, 1 ms 30),
     so the default margin is 8 ms (30 FPS run with it still to do). Uncapped and "uncap + 60 FPS limit" (GPU-bound,
     ~50 FPS) the pacer never sleeps (no frame ends early), so the sleep costs nothing there.
     The 60 and 90 FPS presets stopped at start on the native CPU ("60 FPS++"/"90 FPS++" patch functions the catalog
     has no variant for; only part:fps=uncap): run.sh now runs them as uncap with BB_FPS_LIMIT=60/90.
     Conclusion: no more native-CPU work for speed; it stays an opt-in experiment.
   - Gate: title, Continue, a few minutes of play on AOT; then FEX vs AOT with `odin_profile_remote.sh` (CPU per thread)
1b. **Frame-rate hooks as slots.** Port the slot path of the Android `bbport_frame_rate.cpp` (+ `bbport_hook_sites.h`;
   `Live` at 0x56d3f40 so the redirects are fixed bytes, a variant `patcher:framerate`), then drop the `BB_FRAME_HOOKS=0`
   default from `run.sh`. Check that the fork's hooks and the catalog's `hooks-patchers.txt` sites agree.
2. **Tool on the handheld.**
   - [x] `build_aot_docker.sh device-tool`: static aarch64 build + glibc bundle + catalog + licenses -> `aot-tool/`
   - [ ] run it on the Mac; fix what breaks (`device-tool-build.log` in the volume)
   - [ ] on the handheld: `aot-tool/bin/bb-recomp --version` runs (static: any distribution)
   - [ ] `--cpu`/`--mattr` patch (`patches/bb-recomp/`), version string marked modified (phase 4: speed)
   - [x] `aot-tool` inside the aarch64 AppImage (`packaging/bb-recomp.nix`, licenses and README in it)
   - [ ] build it: `./build_docker.sh --arch aarch64 appimage` with the tool's checkout next to this tree; fix what breaks
     (first suspects: the LLVM release's CMake package in nix (zstd/zlib/libxml2 targets), FEX under `clangStdenv`, a
     `disallowedReferences` hit). Fixed so far: CMake's C++20 module scan (nixpkgs' unwrapped clang-scan-deps misses the
     cc-wrapper's include paths: `CMAKE_CXX_SCAN_FOR_MODULES=OFF` for FEX's ExternalProject, patched into the build's copy
     of the tool's CMakeLists.txt). First real run 2026-10-30: the tool configured, FEX's build started.
   - [ ] the launcher's About text names the tool, its author and license (ADDITIONAL_TERMS 7(b))
   - Gate: Build on the handheld writes a library the game loads. **Passed 2026-10-30**: the AppImage's aot-tool built
     it in 162 s (7 jobs, peak 5.4 GB), the game loaded it (186180 units, 20 patch variants active). Its first stop was
     the armv9 profile (fact 5): rebuild with armv8.4 and go on with phase 1's run checklist.
3. **Launcher.**
   - [x] `scripts/aot_build.py`, `--build-aot`, `run.sh` integration, `game_environment` switch (`BB_AOT=auto`)
   - [x] UI group, status, progress, cancel/resume, misses prompt, English strings; `BB_AOT*` in `bb_env_vars.def` (and
     `cpu/` scanned by `scripts/env_vars.py`), `docs/ENVIRONMENT.md` regenerated
   - [ ] in a real window on the handheld: the group shows, Build runs, Cancel stops, Rebuild resumes, gamepad reaches the rows
   - [ ] pt_BR / zh_CN / fr_FR strings
   - Gate: fresh data folder -> Build -> Play runs on AOT with no terminal
4. **Coverage and speed.**
   - [ ] fork inventory and catalog; runtime counter of time on the fallback
   - [ ] measure: armv8.2 generic / armv9 native, -O1 / -O2 (4 builds, same route, FPS and CPU per thread)
   - Gate: fallback near zero on the route; default profile chosen from numbers
5. **Later.** PKG folders (ps4-pkg-tool for aarch64: `shared/pkg-tool`, or an extract-only mode in the tool), hot code
   ordering, huge pages, `BB_AOT_ONLY`.

## Assumptions (confirm with Sergey when they matter)

- The launcher builds on the handheld (Sergey, 2026-10-30: copying the game to the Mac is the hard part); building on a PC
  stays possible with `build_aot_docker.sh`.
- "Folder with the package" = an extracted game folder first; PKG folders later.
- Handheld: Snapdragon 8 Elite, 8 GB+ RAM, ~1.2 GB free disk during a build.
- FEX stays as the second CPU until AOT has run clean for a while (the Android port removed it only after 0 misses in
  ~20 minutes of play); whether to drop it later is Sergey's call.
- `boot-linked.bin` is deterministic (fact 1). A port update that changes the image scripts shows "Built for another
  game image" and needs a rebuild (minutes).

## Working notes for agents

- The three repositories are mounted for device_bash under `$HOME/mnt/ARM_bloodborne_pc`,
  `$HOME/mnt/Bloodborne-android-rebuild-tool` and `$HOME/mnt/Bloodborne-Android` (read only by agreement): read and edit
  there, do not stage files to the cloud.
- device_bash's VM is aarch64 Ubuntu 22.04 with gcc 11, python3, readelf; no clang, cmake, ninja or nix. Compile checks:
  `g++ -std=c++20 -Wall -Wextra -Werror -Icpu/aot -Isrc -c cpu/aot/guest_cpu_aot.cpp`; FEX headers need clang (gcc:
  `-fsyntax-only -fpermissive` with FEX's include dirs and `out/fex/include`). Deleting files in the folders needs the
  user's approval: write scratch files under `$HOME` outside `mnt/`.
- The cloud sandbox has no Docker and cannot download the Ubuntu sysroot; real runs of `build_aot_docker.sh` happen on
  Sergey's Mac. The handheld is reached as `armada@<handheld>` (data folder `~/.local/share/bbport/`).
- Run no git commands that change state unless asked; branch is `aot`.
- `build_docker.sh` builds without the nix sandbox: every builder's HOME is `/homeless-shelter`, which Nix refuses to
  start a build next to. Its watcher removes that folder only between builds (`build-dir = /tmp/nix-builds` empty);
  removing it during one broke GTK's g-ir-scanner. The whole run's output is in `out/build_docker.log`: read it there
  instead of asking for a paste.

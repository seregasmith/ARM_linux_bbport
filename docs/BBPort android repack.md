
I’ve added `build_aot_docker.sh` next to `build_docker.sh`; it builds `libbbaot.so` for your handheld from your own `boot-linked.bin`. It hasn’t been run under Docker or on the real game yet. Nothing in the port can load the library until the runtime work (step 2) is done.

**What I found in the tool**

- **No changes to their tool were needed.** Its own link step only makes Android libraries, but it can also write plain object files (`--obj`). The script uses that and links the objects itself against an Ubuntu 22.04 arm64 system image, so the library works on glibc 2.35 or newer.
- **It accepts your fork’s image directly.** It reads `boot-linked.bin` in exactly the format your `link_modules.py` writes, so it compiles the image the game actually runs.

**What I checked here**

- I built bb-recomp from your copy of the tool, with LLVM 23.1.3 and FEX 2609.1.
- I ran the script’s container steps on a small made-up x86-64 image: compile, link, report.
- The library came out as AArch64, with the version number their runtime expects and the right exports.

**What hasn’t run yet**

- Docker itself, since there’s no Docker service in my sandbox.
- The real Ubuntu download the link step uses, which is blocked here (I tested the link against a stand-in with the same layout).
- Your real game image.

**How to run it.** Copy six files from the handheld, after a launch with the mods you normally use, because `boot-linked.bin` is rewritten at every launch:

```
mkdir -p ~/Work/bbport/aot-in/game/sce_module ~/Work/bbport/aot-in/game/sce_sys
H=armada@<handheld>; G=<game folder on the handheld>
scp "$H:.local/share/bbport/out/boot-linked.bin" "$H:.local/share/bbport/out/link.json" ~/Work/bbport/aot-in/
scp "$H:$G/eboot.bin" ~/Work/bbport/aot-in/game/
scp "$H:$G/sce_module/libc.prx" "$H:$G/sce_module/libSceFios2.prx" ~/Work/bbport/aot-in/game/sce_module/
scp "$H:$G/sce_sys/param.sfo" ~/Work/bbport/aot-in/game/sce_sys/
cd ~/Work/bbport/ARM_bloodborne_pc
./build_aot_docker.sh --game ~/Work/bbport/aot-in/game --boot ~/Work/bbport/aot-in/boot-linked.bin
```

- **Output:** goes to `~/Work/bbport/aot-out/`, outside both repos. `libbbaot.txt` there lists the functions the runtime will have to supply, which feeds directly into step 2.
- **First run:** downloads about 1.1 GB, uses roughly 16 GB of Docker disk, and takes 10–30 minutes to build bb-recomp. After that, a new library takes a few minutes.
- **Optional:** your Snapdragon 8 Elite can also use `--host armv9`. The default is the tool’s own tested setting.

The script runs no git commands and writes nothing into your repo or the tool’s folder.

For step 2 I’ll need their runtime code too: please download `ducvd89/Bloodborne-Android` the same way, into `~/Work/bbport/Bloodborne-Android`.

Files are in ~/Work/bbport/ARM_bloodborne_pc:

- build_aot_docker.sh
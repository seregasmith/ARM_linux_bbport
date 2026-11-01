# Cheats: Lantern Engine's cheats in this fork (plan)

Branch `cheats`. Status: phase 0 done; phase 1 built (2026-11-01, see *Phase 1: what was built*),
not yet tried in the game.

Cheats picked in the launcher (for the next start) and in the overlay menu (applied live), as in
Lantern Engine, on all three CPUs of this fork: x86-64 (native), aarch64 FEXCore and aarch64
ahead-of-time (`cpu/aot`). Every cheat is off by default and off while playing online. Turning a
cheat off puts the game's bytes back. Items and Blood Echoes gained while a cheat was on stay in
the save.

## Source

[Lantern Engine](https://github.com/Awakenchan/LanternEngine) by Awaken, GPL-3.0-or-later. Pinned
to commit `c5fe051` (0.9.39 beta 27, 2026-11-01); `cheats.cpp` last changed in `4715bca` (0.9.38).
Lantern is built on DarkIzuku/bloodborne_pc, which is built on bbport (this tree's `pc_origin`),
and runs the same game image (CUSA03173 1.09).

| Lantern file | What we take |
|---|---|
| `engine/gpu/shim/engine/cheats.cpp` (927 lines), `cheats.h` | the whole module: signatures, param snapshots, byte writes and restores, refills, tick |
| `engine/gpu/shim/engine/engine_state.cpp/.h` (331 lines) | param tables, player record, player stats, world block: plain memory reads |
| `engine/gpu/shim/bbport_settings.h/.cpp` | the `cheat_*` keys, their parsing and limits |
| `engine/tests/test_settings.cpp` (lines 110–144) | the cheat key tests |
| `engine/gpu/shim/bbport_overlay.cpp` (`CheatsSection`, `Cheat`, `CheatToggle`, `CheatLevels`, `CheatStats`) | the overlay section, EN/RU texts |
| `launcher/windows/LanternLauncher/Views/CheatsPage.cs` | texts for our schema items |

Lantern runs on x86-64 only: it calls the game's functions directly, ticks from a native detour in
the game's frame pacer and writes the game's code once (keep echoes). Those three things are the
porting work on aarch64. Everything else is data in the game's memory.

## The cheats

| # | Cheat | Key | How Lantern does it | aarch64 concern | Phase |
|---|---|---|---|---|---|
| 1 | Blood Vials | `cheat_vials` | EquipParamGoods row 1000: consume bit (+0x44, 0x80) off; refill to the carry cap with AddItem; custom amount: maxNum (+0x36) and cap bit (+0x46) + AddItem | game calls | 1: not used up; 2: refill, custom amount |
| 2 | Quicksilver Bullets | `cheat_bullets` | EquipParamWeapon `bulletConsumeNum` (+0x13a) = 0 for every gun; refill / custom as vials | game calls | 1: not used up; 2: refill, custom amount |
| 3 | Infinite consumables | `cheat_consumables` | consume bit off in 20 EquipParamGoods rows (molotovs, knives, pebbles, papers ...) | none | 1 |
| 4 | Infinite stamina | `cheat_stamina` | WorldChrMan → PlayerIns (+0x60) → modules (+0x3b0) → data (+0x20): stamina (+0x134) = max (+0x138) every tick | none | 1 |
| 5 | Stealth | `cheat_stealth` | the game's debug flags Player Hide / Player Silence = 1 | none | 1 |
| 6 | Firearm range | `cheat_gun_range` (off, 150, 200, 300) | Bullet param life (+0x10) and distance (+0x14) × % for the firearms' BehaviorParam_PC rows | none | 1 |
| 7 | Echo multiplier | `cheat_echo_multiplier` (1, 2, 5, 10) | NpcParam getSoul (+0x28), GameAreaParam (+0x0, +0x4) × N | none | 1 |
| 8 | Add echoes / insight | overlay buttons only | GameDataMan → +8 → echoes +0x94 / insight +0x84, up to 999,999,999 / 99 | none | 1 |
| 9 | No weapon wear | `cheat_no_durability` | for 8 equip slots: Gaitem lookup + ChangeDurability to the weapon's max (+0xbe) | game calls | 2 |
| 10 | Keep echoes on death | `cheat_keep_echoes` | 15 bytes of code changed in DropBloodstain | code write (AOT stops the game) | 3 |

## What this tree already has

- **The image at start:** `bbgpu_patch_image(image, size)` (`gpu/shim/bbgpu.cpp`), called by
  `src/probe.c` before the game runs. `Cheats::Install` goes there.
- **A call on a game thread every frame:** `scePadReadState` (`src/runtime_pad.c`,
  `pad_read_state`). `report_guest_heap` (`src/runtime_pad/gamepad.c`) already calls into the game
  from there with `guest_cpu_call`, on x86-64, FEX and AOT.
- **Game calls and code writes:** `guest_cpu_call` (FEX and AOT put arguments 7+ on the stack),
  `GuestCall` (`gpu/shim/bbport_guest_call.h`), `guest_cpu_invalidate` (`src/guest_cpu.h`),
  `Core::KeepGuestCodeWritable` / `GuestCodeProtection` (`gpu/shim/bbport_guest_code_hooks.cpp`).
- **Guest-visible memory:** `runtime_low_map` (`src/runtime.h`), below 1 TiB.
- **The same addresses in BBSC** (`gpu/bbnet20/bbsc/core/bloodborne_re.cpp`): WorldChrMan
  `0x553e878`, GameDataMan `0x553b130` (echoes `+8 → +0x94`), SoloParamRepository `0x5540340`,
  DropBloodstain `0x13d2910`, PlayerAddEchoes `0x18fc600`, and the stain record
  (`[[0x553b130] + 0x30] + 0x38` echoes held, `[0x553b130] + 0x28` exists) in
  `BbscApplyDeathPenalty`.
- **Overlay:** sections in `gpu/shim/bbport_overlay_sections.cpp`; `Store`, `Checkbox`, `Hint` in
  `bbport_overlay_internal.h`; the list of sections in `bbport_overlay.cpp` (after
  `SavePointSection`).
- **Settings:** `gpu/shim/bbport_settings.h/.cpp` (`bbport.ini`, the schema's `store: ini`),
  `tests/test_settings.cpp`.
- **Launcher:** `launcher/settings_schema.json`, read by the GTK and the WPF launcher: types `bool`,
  `choice`, `int`, plus `visible_when`, `enabled_by`; texts in ru, en, pt_BR, zh_CN, fr_FR. Its
  online text already says *Cheats change the game for the other players too*.
- **Online:** `runtime_net_enabled()` (`src/runtime_net.c`): `BB_ONLINE=1`, except on aarch64,
  where the runtime plays offline even with `BB_ONLINE=1` (*Online: not available on this CPU
  yet*) and loads no online module.

## Address map

Lantern writes PS4 addresses (image at `0x400000`, as `patches/Bloodborne.xml`); this tree uses
offsets in the image: **offset = Lantern address − 0x400000**. Checked against BBSC: WorldChrMan
and GameDataMan are equal, Lantern's echo site lies in PlayerAddEchoes and its blood-mark site in
DropBloodstain. No line of `patches/Bloodborne.xml` touches a cheat site.

| Site | Lantern | Offset here |
|---|---|---|
| EquipParamGoods row lookup | `0x231e3d0` | `0x1f1e3d0` |
| carry cap | `0x231e7e0` | `0x1f1e7e0` |
| inventory slot | `0x18d9f60` | `0x14d9f60` |
| AddItem | `0x18cdfd0` | `0x14cdfd0` |
| goods consume check | `0x18d0dc0` | `0x14d0dc0` |
| weapon bullet consume | `0x1cfa1d0` | `0x18fa1d0` |
| stamina load / clamp | `0x1cb6451` / `0x1cb64a2` | `0x18b6451` / `0x18b64a2` |
| WorldChrMan | `0x593e878` | `0x553e878` |
| Player Hide / Player Silence flags | `0x593e88e` / `0x593e88f` | `0x553e88e` / `0x553e88f` |
| blood mark signature / patch | `0x17d2b82` / `0x17d2bd2` | `0x13d2b82` / `0x13d2bd2` |
| ChangeDurability | `0x18d24e0` | `0x14d24e0` |
| Gaitem lookup | `0x1e89070` | `0x1a89070` |
| insight / echoes add | `0x1cfc567` / `0x1cfc6a4` | `0x18fc567` / `0x18fc6a4` |

## Decisions

| Question | Decision |
|---|---|
| Where the code goes | `gpu/games/bloodborne_cheats.cpp/.h` and `bloodborne_cheats_state.cpp/.h`, beside the other Bloodborne modules (`bloodborne_menu_pointer`, `bloodborne_kit_restore`); the overlay section in a file of its own, `gpu/shim/bbport_overlay_cheats.cpp` (its texts are Lantern's: GPL-3.0-or-later) |
| License | the ported files keep `SPDX-License-Identifier: GPL-3.0-or-later` and Awaken's credit; NOTICE gets a Lantern Engine entry, as for bbnet20's GPL-3.0 parts |
| Tick | `bbgpu_game_tick()` (new), called from `pad_read_state` after the pad lock is released: one path on every CPU. Not a frame hook: aarch64 has none (native detours are x86-64 only, AOT hooks only at catalog slots) |
| Vials / bullets keys | two ini keys each: `cheat_vials` (off, infinite or custom) and `cheat_vials_amount` (1 to 99), since the schema has no "choice or number" item; the same for bullets |
| Online | every cheat off while the game is really online: `runtime_net_enabled()`, not Lantern's check of `BB_ONLINE` (the handheld's launcher sets `BB_ONLINE=1` while the game plays offline: the cheats would always be off there); the overlay says why |
| Another image | each cheat checks Lantern's byte signatures at start; a mismatch makes that cheat *Unavailable* with the reason, never a guess |
| Keep echoes under AOT | phase 3 route A (data only); until then the cheat is *Unavailable on the native CPU* when `BB_AOT` is set |

## Phase 0: checks before coding (0.5 day)

1. **Which thread reads the pad.** Log the thread id and name once in `pad_read_state`. Expected:
   the game's main loop (the one that updates the world). If it is a separate input thread, phase 1
   still works (data only), and phase 2 needs phase 4.1 first.
2. **How often.** Reads per second in the world, in menus and while loading
   (`runtime_pad_report` has the count).
3. **Signatures.** Compare the 23 signature sites in `boot-linked.bin` with every launcher patch
   on (frame rate, resolution, effects). None are expected to differ.
4. **Image pointer.** The `image` passed to `bbgpu_patch_image` is the one the CPU runs, under AOT
   too (it moved once, see AOT_LAUNCHER_PLAN.md, run 3).

### Phase 0: results

| Check | Result |
|---|---|
| 3. Signatures, the sources | **Clear.** `tools/cheat_sites.py`: none of the 3192 writes of `patches/Bloodborne.xml` (every patch, chosen or not) touches a cheat site; no code address in the port's sources is at or near one. Only BBSC names one: a trace hook at the goods row lookup `0x1f1e3d0` (the Row signature's first 6 bytes) and a call of it. BBSC runs only online (never on aarch64 now), when the cheats are off anyway |
| 3. Signatures, the image | **Clear.** All 22 sites match in the handheld's `boot-linked.bin` (sha256 `b5cb1da3…`, the image its AOT library was built from), checked with `tools/cheat_sites.py --boot`. Its `patches.bin` was not checked directly, but the chosen patches (the log: Uncap FPS++, Sprint Fix, SSR, Skip Intro, Model LOD 1, Skip Online/Offline Choice, Larger GX Per-Frame Command Heap) all come from `Bloodborne.xml`, which touches no site |
| 4. Image pointer | **Confirmed** (`src/probe.c`): one allocation, the same pointer to `guest_cpu_code`, `runtime_image_base`, the patches and then `bbgpu_patch_image`, so `Cheats::Install` sees the patched bytes; AOT's own image check comes later (its first guest call). The 2026-10-30 AOT profile (`dist/profiles/20261030_234045`) has the fixed-base engine fixes on: the image is at `0x800000000` under AOT too |
| 1–2. Pad thread and rate | **Settled** (AOT run on the handheld, `BB_PAD_TRACE=1`). One reading thread: `bb-probe`, the main thread, which runs the game's main loop. 51–54 reads/s in the world at the 60 FPS limit: one read a frame. 57/s on the title screen and while loading (9.5/s of them with the world shown): the tick keeps running outside the world. The probe (`src/runtime_pad.c`) logs the reading threads always and the rate with `BB_PAD_TRACE`; checked with `tests/test_pad.c`. No FEX run needed: which thread reads the pad is the game's code, the same on both CPUs |
| Found on the way | `BB_ONLINE=1` on the handheld while the game plays offline (the run's log has both `BB_ONLINE=1` and *Online: not available on this CPU yet*): the online decision changed (above). `tests/test_pad.c` had not linked since 0.7 (`gamepad.c`'s heap report calls `guest_cpu_call`): a stub added |

**Decision:** the cheats tick from the pad read (`bbgpu_game_tick`), and phase 2 calls into the
game from there: it is the game's main thread, once a frame. Phase 4.1 is not needed.

## Phase 1: first version, data only (2.5–3 days)

No calls into the game and no code writes: the same on x86-64, FEX and AOT, and the tick's thread
does not matter yet.

1. **`cheats_state.cpp/.h`** from Lantern's `engine_state.cpp`: param tables (repository walk,
   records, file pointer to notice a reload), `player_record`, `player_stat_get/set`,
   `world_player_block`; `host_read_safe` with `process_vm_readv` (as `Patch` in
   `gpu/games/bloodborne/hooks.cpp`). Event flags and menu steps are not needed.
2. **`cheats.cpp/.h`** from Lantern's `cheats.cpp`: `Byte`/`Patch`, `Write`, `RestoreSafe`,
   `Current`, `Forget`, `LoadTable`, `SnapshotGunRange`, `SnapshotEchoes`, `SnapshotConsumables`,
   the stealth flags, `RefillStamina`, `AddStats`, `SharedWorld`, `Unavailable`, `Tick` (400 ms
   debounce, 250 ms interval, 1.5 s settle after a load). `Slot(bn)` becomes
   `image + (bn − 0x400000)`. Vials and bullets in this phase: *infinite* = not used up (vial row
   consume bit off; every gun's `bulletConsumeNum` 0), through the same `Patch` code as
   consumables. The `Cheat` struct (refill) waits for phase 2.
3. **Hook-up** (online: `SharedWorld` asks `runtime_net_enabled()`): `bbgpu.h` declares `bbgpu_game_tick`; `gpu/shim/bbgpu.cpp` calls
   `Cheats::Install` from `bbgpu_patch_image` and `Cheats::Tick` from the tick;
   `src/runtime_pad.c` calls the tick after `pthread_mutex_unlock`; a stub in
   `tests/test_gpu_runtime_stubs.cpp`.
4. **Settings:** `cheat_vials`, `cheat_bullets` (off/infinite), `cheat_consumables`,
   `cheat_stamina`, `cheat_stealth`, `cheat_gun_range`, `cheat_echo_multiplier` in
   `bbport_settings.h/.cpp`, with Lantern's allowed lists; a bad value reads as off and is written
   back as off. Cases in `tests/test_settings.cpp`.
5. **Overlay:** `CheatsSection` in `bbport_overlay_sections.cpp`, called after
   `SavePointSection`: the mode combos, toggles, level combos, the echoes/insight buttons (+1,000,
   +10,000, +100,000 echoes, +1 insight), and *Unavailable: why* under a cheat that cannot run.
   EN/RU texts from Lantern.
6. **Launcher:** a `cheats` section in `settings_schema.json` between `mods` and `online`, in
   groups *Items* (vials, bullets, consumables), *Character* (stamina, stealth, firearm range) and
   *Blood Echoes* (echo multiplier; a note that echoes and insight are added in the overlay).
   Descriptions say the cheats apply live in the overlay (Insert or L3 + R3) and stay off online.
   Five languages. Check that both launchers draw a new section and its icon
   (`launcher/bbport_launcher.py`, `launcher/windows/MainWindow.xaml.cs`, `Store.cs`).
7. **Docs:** README.md and README.ru.md (a Cheats paragraph), the CHANGES file of the release,
   NOTICE (Lantern Engine, GPL-3.0-or-later, Awaken), this plan's status.
8. **In the game:** the test list below on x86-64, FEX and AOT.

Done when the eight phase-1 cheats work on all three CPUs, the log shows *params restored* for
each one turned off, and `BB_AOT_DEBUG=1` lists no new patched unit.

### Phase 1: what was built

| Part | Files |
|---|---|
| Game state | `gpu/games/bloodborne_cheats_state.cpp/.h`: Lantern's `engine_state.cpp` (param repository walk, player record and stats, world block), reads through the runtime's fault recovery (`runtime_fault_recover`, as `bloodborne_kit_restore.cpp`) |
| Cheats | `gpu/games/bloodborne_cheats.cpp/.h`: Lantern's `cheats.cpp` without its calls into the game and its code patch; vials and bullets as byte patches (`SnapshotVials`, `SnapshotBullets`); 11 signature uses, checked against `tools/cheat_sites.py`'s table |
| Hook-up | `bbgpu_game_tick` (`gpu/bbgpu.h`, `gpu/shim/bbgpu.cpp`) from `pad_read_state` (`src/runtime_pad.c`) after the pad lock; `BbCheats::Install` from `bbgpu_patch_image`; a stub in `tests/test_pad.c` |
| Settings | `cheat_vials`, `cheat_bullets` (off, infinite), `cheat_consumables`, `cheat_stamina`, `cheat_stealth` (0, 1), `cheat_gun_range` (off, 150, 200, 300), `cheat_echo_multiplier` (1, 2, 5, 10) in `gpu/shim/bbport_settings.h/.cpp`; cases in `tests/test_settings.cpp` |
| Overlay | `gpu/shim/bbport_overlay_cheats.cpp` (`CheatsSection`, after *Save point*); only a note while online |
| Launcher | `launcher/settings_schema.json` section `cheats` (5 languages; the WPF launcher draws it from the schema, glyph in `launcher/windows/MainWindow.xaml.cs`); GTK: `build_cheat_rows` in `launcher/bbport_launcher.py`, lists and ini defaults in `bbport_launcher_common.py`, texts in `bbport_i18n.py` |
| Test | `tests/test_cheats.cpp`, CMake target `cheats-test` (`gpu/CMakeLists.txt`): a made-up game in memory (image with the checked code, singletons, param repository, player); every cheat on and off, a params reload, a byte changed by someone else, other code at a site, echoes without a character. Passes with GCC 13 and Clang 18 on x86-64 (ASan and UBSan clean) and GCC 11 on aarch64 |
| Docs | README.md, README.ru.md, NOTICE (Lantern Engine; `libbbgpu.so` as a whole GPL-3.0-or-later) |

Found on the way: `settings-test` was already broken before the cheats: its include paths miss
`gpu/` (`shim/bb_env.h`), and its frame generation checks expect `frame_generation=fsr3` in the
file to read as off, which the code no longer does. The cheats' cases pass on their own.

In the game (the list under *Tests in the game*), what to look for in the log:

- At start: `Cheats: 7 of 7 ready (the game's code checked), add echoes / insight ready`.
- With a cheat switched in the overlay: `Cheats: <cheat> on` / `... not used up` within half a
  second, then the snapshot line once the character is in the world (`Cheats: bullets: N weapons
  cost bullets ...`, `Cheats: firearm range: N bullet rows of 10 firearm behaviours`,
  `Cheats: echo multiplier: N enemy and boss echo values`).
- Switched off: `... off, params restored` / `flags restored`.
- After a lamp travel: `Cheats: <cheat>: the game reloaded its params; applying again` (if the
  game reloads them there).
- Any `Cheats: <cheat> unavailable: <why>`: the cheat stopped; the line says why.

## Phase 2: calls into the game: refill, custom amounts, weapon wear (2–2.5 days)

1. **Game calls.** Lantern's `Call(fn, args...)` becomes `guest_cpu_call` (x86-64: a direct call
   as now). AddItem takes 7 arguments: FEX and AOT put the 7th on the guest stack (checked in
   `cpu/guest_cpu_fex.cpp` and `cpu/aot/guest_cpu_aot.cpp`).
2. **Guest-visible buffers.** The structures Lantern passes on its stack (`Handle`, the item
   `{id, 0, 0, 0}`, the Gaitem record) go into one block from `runtime_low_map`, made once:
   aarch64 host stacks are above the addresses the game's code can use (see the comment in
   `gamepad.c`).
3. **Only on the game's thread:** the pad read is it (phase 0). Calls stay in `bbgpu_game_tick`, never
   from the overlay's thread: the overlay's buttons only queue a request (as Lantern's `AddEchoes`).
4. **Vials and bullets:** Lantern's `Cheat` (`Snapshot` with the cap bytes, `Wanted`,
   `Reconcile`, `Run` with row lookup, carry cap, inventory count and AddItem) and its 10
   signatures. *Infinite* now also refills from none to the carry cap; *custom* keeps the count at
   `cheat_vials_amount` / `cheat_bullets_amount`. Schema: the amount is an `int` item
   `visible_when` the mode is `custom`; overlay: a slider under the combo.
5. **No weapon wear:** `RefillDurability` (EquipParamWeapon max at +0xbe, Gaitem lookup, then
   ChangeDurability for the 8 equip slots at equip + 0x94). `cheat_no_durability` in settings,
   schema and overlay.
6. **In the game:** vials at 0 refilled; custom 50, then 10, then off; items put into storage come
   back while on (Lantern's note); weapon wear after many hits; save, quit, load.

## Phase 3: keep echoes on death (0.5–2.5 days)

1. **x86-64 and FEX (0.5 day):** Lantern's code change: 15 bytes at `0x13d2bd2`, after its 101-byte
   signature at `0x13d2b82` matched; write with `KeepGuestCodeWritable` / `GuestCodeProtection`,
   then `guest_cpu_invalidate(address, 15)` (a no-op on x86-64). Off writes the original bytes and
   invalidates again.
2. **AOT**, one of:
   - **A, data only (preferred, 1.5–2 days):** works on every CPU, so step 1 can go. Snapshot the
     echoes when the death step starts; once the stain holds them (`[[0x553b130] + 0x30] + 0x38`, its
     exists byte at `[0x553b130] + 0x28`, as `BbscApplyDeathPenalty` reads them), give them back to the player
     (`+8 → +0x94`) and empty the stain. To find out first: what an empty stain is (held 0, exists
     0, or the stain's object in the world), and that the death and respawn steps don't take the
     echoes a second time.
   - **B, an AOT variant (1 day, after the catalog exists):** DropBloodstain with the 15 changed
     bytes in the fork's variant catalog (AOT_LAUNCHER_PLAN.md, *Fork variant catalog*, not built
     yet). Every user then rebuilds `libbbaot.so`.
   - **Until then:** with `BB_AOT` set the cheat shows *Unavailable on the native CPU*.

## Phase 4: extra work (1.5–3 days, as needed)

1. **A frame tick on the game's main thread (1–1.5 days)**: not needed (phase 0: the main thread
   reads the pad once a frame); kept here in case that changes. Site: the frame pacer's entry, `0x2034770` (an image offset
   in Lantern's `features.cpp` too, not a PS4 address; `part:fps=uncap` patches this function,
   so under AOT the slot must be in its variant as well).
   x86-64: a native detour as in `gpu/games/bloodborne/hooks.cpp`; FEX: `guest_cpu_hook` (a trap:
   check that RCX and R11 are dead there); AOT: a hook slot in the catalog's `hooks-*.txt` and a
   library rebuild.
2. **Launcher notices (0.25 day):** a warning when online is on (*cheats stay off online*), as
   Lantern's page.
3. **Game menu page (1 day, low priority):** *System → Cheats* through `bbport_game_menu.cpp`.
   x86-64 only: the game menu pages are off on aarch64 (`bbport_game_menu.cpp`, `PatchImage`).
4. **Unit tests (0.5 day):** the `Patch` code (write, restore, a byte changed by someone else, a
   param reload) against a fake image and a fake param table in memory, in `tests/`.
5. **Following Lantern (0.25 day per release):** diff `cheats.cpp` against the pinned commit at
   each Lantern release and take the fixes.
6. **Upstream (optional):** offer the port to deadinside28/bloodborne_pc; its files would be
   GPL-3.0-or-later there too.

## Tests in the game

For every cheat on every CPU (x86-64 Linux, aarch64 FEX, aarch64 AOT):

1. On in the overlay: works within a second; the log line `Cheats: ... on`.
2. Off: the game's values back (`params restored` / `flags restored` / `code restored`).
3. Lamp travel and an area load: still on (`the game reloaded its params; applying again` when
   the game reloads its params).
4. Quit and start again: the setting is kept (bbport.ini), the launcher shows it.
5. Online on (x86-64): every cheat off, the overlay says why.
6. AOT only: no `STOP: AOT` and no new line in `logs/aot-misses.log`.

## Risks

| Risk | What to do |
|---|---|
| The pad is read on another thread, or rarely in menus or while loading | settled by phase 0: the main thread, once a frame, also while loading |
| Game calls with host-stack pointers on aarch64 | buffers from `runtime_low_map` (phase 2.2) |
| AOT stops the game at changed code | only keep echoes writes code; route A or B, or unavailable |
| The game reloads its params (area load, title) | Lantern's check of the param file pointer, then applied again |
| A mod or patch changes a checked function | that cheat is *Unavailable*, the others still run |
| Counts gained while on are saved | said in the texts: off does not take them back |
| Lantern changes its cheats | pinned commit, phase 4.5 |

## Estimate

| Phase | Days |
|---|---|
| 0. Checks | 0.5 |
| 1. First version (data only) | 2.5–3 |
| 2. Refill, custom amounts, weapon wear | 2–2.5 |
| 3. Keep echoes (x86-64 / FEX, then AOT) | 0.5 + 1–2 |
| **All ten cheats (0–3)** | **6.5–8.5** |
| 4. Extra work, as needed | 1.5–3 |
| **Everything** | **8–11.5** |

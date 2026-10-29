# Save scum: a save point in the launcher and in the game (plan)

Branch `save_scumming`. Status: steps 1–5 implemented (2026-10-29); not yet tried in the game.

## What was built

| Part | Files |
|---|---|
| Switch | `launcher/settings_schema.json` (`save_snapshots`, Mods and patches, Linux), `bbport_launcher_common.py` (default off, `BB_SAVE_SNAPSHOTS=1`), `gpu/shim/bb_env_vars.def`, `docs/ENVIRONMENT.md` |
| Launcher logic | `scripts/save_snapshots.py` (+ `tests/test_save_snapshots.py`) |
| Launcher UI | `launcher/bbport_launcher_savepoint.py` (switch, button left of Play, popover, confirmations), texts in `bbport_i18n.py` (en, pt_BR, zh_CN, fr_FR) |
| Game logic | `src/runtime_snapshot.c/.h` (same layout and steps as the Python module; + `tests/test_save_snapshot.c`, in `build.sh --test`) |
| Game glue | `src/runtime_savedata.c` (lock, queued restore at start, backup thread, API for the menu), `src/runtime_file.c` (save change counter; + `tests/test_file_mods.c`) |
| Overlay | `gpu/shim/bbport_overlay_sections.cpp` (`SavePointSection`), stubs in `tests/test_gpu_runtime_stubs.cpp` |
| Docs | README.md, README.ru.md, docs/MODS.md |

Differences from the plan below:

- Step 0 was not run before coding. The in-game copy uses both rules instead: it starts when no
  save file is open for writing, no save change is in progress and none ended in the last second,
  and it is dropped and made again if the change counter moved during the copy (every change to
  the saves is counted when it starts and when it ends). If the game keeps a save file open for
  writing all the time, the menu will say *The game kept saving* after ~20 s: then run step 0.
- *Back up now* asks for confirmation only when it replaces an existing save point.
- No mod-set warning on restore (`mods.py` already copies the saves when the mod set changes).
- A queued in-game restore older than 10 minutes is dropped (the restart failed or the player
  quit), by the game at start and by the launcher's next operation.

## Testing in the game (to do)

1. Switch on *Mods and patches → Save point*; start; load a character; open the overlay menu:
   *Back up now* → *Save point made* (the log: `Runtime: save point made`).
2. Pick something up or die; *Restore and restart* (twice) → the game restarts, the log has
   `Runtime: save point restored`, the character is as at the save point.
3. Quit; in the launcher, the button left of Play: *Undo restore* brings back step 2's progress.
4. While the game runs, the popover's buttons are inactive.

The player makes a copy of the game's saves (a *save point*) and later puts it back. This goes
against how the game is meant to be played, so the feature is **off by default** and turned on
on purpose in *Mods and patches*.

## Decisions

| Question | Decision |
|---|---|
| How many backups | **One slot** (one save point; *Back up now* overwrites it) + one automatic *undo* copy taken before every restore |
| Launcher UI | A **button next to Play** (header bar), shown only while the feature is on; it opens a popover with the actions |
| In-game UI | A section in the **port's overlay menu** (Insert / hold L3+R3) |
| Platforms | **Linux launcher only** for now (schema item `platforms: ["linux"]`); the runtime/overlay part is platform-neutral |
| The game's own title menu ("Load Game") | Not now: needs reverse engineering of the game's title-menu builder (as `gpu/shim/bbport_game_menu.cpp` did for System → Display/Effects). A later stretch |

## What the code already has

- **Save layout** (`src/runtime_savedata.c`):
  - `<user>/savedata/<user id>/<title id>/<dir>/` — the game's files (`SPRJ0005/userdata0000..0010`);
  - `<user>/savedata/<user id>/<title id>/<dir>.sce_sys/` — `param.bin`, `icon0.png`;
  - `<user>/savedata/<user id>/<title id>.memory/memory.dat` — SaveDataMemory.
  
  A backup is a copy of the whole `<user>/savedata/` tree. `<user>` is `BB_USER_DIR`, or
  `$data/user` (launcher: `LauncherWindow.user_dir()`). Trophies, the shader cache, `dlss/` and
  `bbsc/` are not in `savedata/` and are not touched.
- **Automatic copies on a mod change** (`scripts/mods.py`, `backup_saves`, issue #212):
  `<user>/savedata-backups/<YYYYmmdd-HHMMSS>/`, the newest 5 kept. Its cleanup deletes *every*
  subfolder beyond 5, so the save point must not live there.
- **Restart from the game**: `runtime_restart()` (`src/probe.c`) — `exec bash run.sh` on Linux,
  a new process with the same command line on Windows; used by the overlay's *Apply and restart
  game* (`gpu/shim/bbport_overlay_sections.cpp`).
- **A hook before the game reads saves**: `runtime_savedata_configure()` (the Bloodborne sound
  hack edits `userdata0010` there). A queued restore is applied here, before the sound hack.
- **Save tracing**: `BB_SAVE_TRACE=1` (every operation on `/savedataN`, `src/runtime_file.c`),
  and the `Runtime: save data '...' mounted at ...` lines.
- **Launcher**: schema-driven sections (`launcher/settings_schema.json`, shared with the Windows
  launcher, `platforms` filter); environment for the game in `game_environment()`
  (`bbport_launcher_common.py`); `self.process` while the game runs; gamepad navigation already
  handles `Gtk.Popover` (`bbport_launcher_gamepad.py`).

## Rules

1. **Never swap saves under a running game.** The character lives in the game's memory; its next
   autosave overwrites a restored file, or the folder ends up half old, half new. A restore
   happens before the game starts, or as *restore and restart*.
2. **Never copy a file while the game writes it.** From the launcher: only while the game is not
   running. In the game: the runtime copies only between save writes (see step 0).
3. **Every restore first copies the current saves to the undo slot**, so a wrong press can be
   taken back.
4. **No step leaves a broken `savedata/`**: copies go to a temporary folder first, then folders
   are swapped by renames (on one file system, inside `<user>`).

## Layout on disk

```
<user>/savedata/                     the game's saves (unchanged)
<user>/savedata-snapshot/            the save point
    savedata/                        a copy of <user>/savedata/
    meta.json                        {"v":1, "created": ISO time, "source": "launcher"|"game",
                                      "mods": [...], "detail": text from param.bin, if any}
<user>/savedata-snapshot.undo/       the saves as they were before the last restore (same layout)
<user>/savedata-snapshot.pending     in-game restore queued for the next start
<user>/.bbport-game.lock             held by the running game (advisory lock)
```

Back up: copy `savedata/` → `savedata-snapshot.new/`, write `meta.json`, then
`savedata-snapshot/` → `savedata-snapshot.old/`, `.new` → `savedata-snapshot/`, delete `.old`.

Restore: copy the save point's `savedata/` → `savedata.restoring/`; current `savedata/` → the undo
slot (replacing the old undo); `savedata.restoring/` → `savedata/`.

Undo restore: the same as a restore, from the undo slot (the current saves become the new undo).

Left-over `.new` / `.old` / `.restoring` folders from a crash are cleaned on the next operation.

## Steps

### 0. Check how the game writes saves

One session with `BB_SAVE_TRACE=1`: load a character, walk, pick up an item, die, light a lamp,
quit to the title screen. Write down:

- whether `SPRJ0005` is mounted once for the whole session, or mounted and unmounted per save;
- how often and in what pattern `userdata00xx` files are written (whole file, or pieces);
- whether *Load Game* on the title screen reads the files again (decides whether a title-screen
  restore could ever skip the restart; not needed for the first version).

Result: the rule for a safe in-game copy — "no slot mounted" if the game unmounts after each save,
otherwise "no write to `/savedataN` for the last ~500 ms, and none starts during the copy" (a
counter and a lock in the file layer).

### 1. The setting

- `launcher/settings_schema.json`, section `mods`: a new group *Save point* (ru *Точка
  сохранения*) with
  - `save_snapshots`: `type: bool`, `store: settings`, `default: false`,
    `platforms: ["linux"]`; the description says what it does and that it bends the game's
    rules (5 languages: ru, en, pt_BR, zh_CN, fr_FR).
- `bbport_launcher_common.py`: the default in the settings defaults; in `game_environment()`
  `env["BB_SAVE_SNAPSHOTS"] = "1"` when on.
- `gpu/shim/bb_env_vars.def`: `BB_SAVE_SNAPSHOTS` (SETTING) — the overlay shows its section only
  with it; `tests/test_env_vars.py` keeps passing.
- `bbport_i18n.py`: the launcher's new texts (the Russian text is the key).

### 2. Backup logic (Python) — `scripts/save_snapshots.py`

- Functions: `info(user)`, `backup(user, source, mods)`, `restore(user)`, `undo(user)`,
  `game_running(user)` (tries the lock non-blocking), `cleanup(user)`.
- A small CLI (`backup | restore | undo | info --user DIR`) for scripts and tests.
- `scripts/mods.py`: its cleanup touches only timestamp-named folders (defensive).
- `tests/test_save_snapshots.py`: backup → change → restore gives the old files; undo brings the
  change back; a crash between the steps (left-over `.new` / `.restoring`) leaves a valid
  `savedata/`; no saves → backup refuses with a message; a held lock → restore refuses.

### 3. Before Play: the launcher

- `bbport_launcher.py`: a `Gtk.MenuButton` in `content_header` next to `launch_button`, visible
  only while `save_snapshots` is on (updated when the switch changes). Its popover:
  - a line: *Save point: 29 Oct, 18:40* (or *No save point yet*), and the detail from
    `meta.json`;
  - **Back up now** (if a save point exists: overwrite it — a confirmation, the time of the old
    one shown);
  - **Restore save point** (a confirmation: "your current progress is kept as the undo copy");
  - **Undo restore** (only when the undo slot exists);
  - **Open folder**.
- Disabled while `self.process` is set, or while `game_running()` says another copy of the game
  runs (desktop shortcut, `--play`); the tooltip says to use the in-game menu instead.
- Results as toasts. Gamepad: the button is reachable by focus moves, the popover by the existing
  popover handling; check A/B in it.

### 4. After Play: the overlay menu

- Runtime (C), a new `src/runtime_snapshot.c` (or inside `runtime_savedata.c`):
  - at start, in `runtime_savedata_configure()`, before the sound hack: take
    `<user>/.bbport-game.lock` (flock; on Windows a file opened without sharing), then apply
    `savedata-snapshot.pending` if present (the same steps as the Python restore), delete the
    marker, log `Runtime: save point restored`;
  - `runtime_snapshot_backup()`: copies `savedata/` under the save lock by the rule from step 0;
  - `runtime_snapshot_queue_restore()`: writes the marker.
- Overlay (`gpu/shim/bbport_overlay_sections.cpp`), a `SavePointSection()` called from
  `Menu()` in `bbport_overlay.cpp`, only with `BB_SAVE_SNAPSHOTS=1`:
  - the save point's time;
  - **Back up now**;
  - **Restore save point and restart** — pressed twice to confirm (like *Quit game*), then
    `runtime_snapshot_queue_restore()` and `runtime_restart()`.
  - Texts in English and Russian (`BbSettings::MenuText`).
- This is also the only way to use the feature in Steam Deck game mode (`--play`, no launcher
  window).
- Tests: `tests/test_runtime.c` or a new C test for applying a pending restore at start.

### 5. Docs

- README.md / README.ru.md: a short *Save point* paragraph (off by default; where the buttons are;
  the restart; online play is not blocked).
- docs/MODS.md: the automatic copies on a mod change and the save point are different folders.
- The next CHANGES file.

## Edge cases

- **Custom saves folder** (`user_dir`): everything is relative to `<user>`, so a moved folder
  takes its save point with it.
- **Mods**: a save point made with other mods can lose those mods' items on load (issue #212);
  `meta.json` keeps the mod set, the launcher warns when it differs from the current one.
- **Online / BBSC**: same save folder (`BBSC_SAVE_DIR` points into `savedata/`); not blocked.
- **Trophies** stay unlocked after a restore (they are not in `savedata/`).
- **Sound hack**: runs after a restore, so a restored `userdata0010` gets its flag again.
- **Disk space**: a Bloodborne save is a few MB; three copies at most (saves, save point, undo).

## Later

- A native item in the game's own menu (next to *Load Game*, or a page in *System* like
  Display/Effects).
- A title-screen restore without a restart, if step 0 shows the game re-reads saves on *Load
  Game*.
- The Windows launcher (`launcher/windows`): the same button and popover.

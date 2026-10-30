# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Developer -> performance profile: scripts/bbport_profile.py records each game start into <data>/profiles/<time>/
(the game's and the port's threads, cores, GPU, temperature, battery power, FPS: summary.txt), and the two newest
profiles compare side by side (FEXCore against the native CPU, or two settings, at the same spot)."""

import shutil
import time

from bbport_launcher_common import *  # noqa: F401,F403

PROFILES_DIR = DATA_DIR / "profiles"
KEEP_PROFILES = 20  # the oldest profile folders go when a new one starts


def profile_folders():
    """The finished profiles (samples and summary written), newest first."""
    try:
        folders = [p for p in PROFILES_DIR.iterdir()
                   if p.is_dir() and (p / "summary.json").is_file() and (p / "samples.csv").is_file()]
    except OSError:
        return []
    return sorted(folders, key=lambda p: p.name, reverse=True)


class PerfProfile:
    """Part of LauncherWindow (bbport_launcher.py)."""

    def build_profile_rows(self, group):
        self.profile_proc = None
        self.profile_log = None
        self.perf_profile_row = Adw.SwitchRow(title=tr("Записывать профиль производительности"),
                                              active=bool(self.settings.get("perf_profile", False)))
        group.add(self.perf_profile_row)
        self.profiles_row = Adw.ActionRow(title=tr("Профили производительности"))
        self.compare_button = Gtk.Button(label=tr("Сравнить два последних"), valign=Gtk.Align.CENTER)
        self.compare_button.connect("clicked", self.on_compare_profiles)
        self.profiles_row.add_suffix(self.compare_button)
        self.profiles_row.add_suffix(flat_button("folder-open-symbolic", tr("Открыть папку профилей"),
                                                 lambda _b: open_folder(self, PROFILES_DIR)))
        group.add(self.profiles_row)
        self.update_profile_rows()

    def update_profile_rows(self):
        if not hasattr(self, "profiles_row"):
            return
        folders = profile_folders()
        if self.profile_proc is not None:
            self.profiles_row.set_subtitle(tr("Идёт запись…"))
        elif folders:
            try:
                s = json.loads((folders[0] / "summary.json").read_text(encoding="utf-8"))
            except (OSError, ValueError):
                s = {}
            fps = s.get("fps_mean")
            self.profiles_row.set_subtitle(tr("Последний: {} — {}, {} FPS, CPU {} %").format(
                folders[0].name.replace("_", " "), s.get("mode", "?"), f"{fps:.1f}" if fps else "?",
                f"{s['process_cpu']:.0f}" if s.get("process_cpu") else "?"))
        else:
            self.profiles_row.set_subtitle(tr("Пока нет: включите запись и запустите игру"))
        self.compare_button.set_sensitive(len(folders) >= 2)

    def profile_environment(self, env):
        """The game's present rate every 5 s in its output (cheap, unlike BB_FRAME_STATS): the profile's FPS."""
        if self.settings.get("perf_profile"):
            env.setdefault("BB_FPS_PRINT", "1")
        return env

    def start_profile(self, pid):
        """A profile of the game just started (pid: its session, setsid in start_game)."""
        if not self.settings.get("perf_profile") or self.profile_proc is not None:
            return
        folder = PROFILES_DIR / time.strftime("%Y%m%d_%H%M%S")
        try:
            folder.mkdir(parents=True, exist_ok=True)
            for old in sorted((p for p in PROFILES_DIR.iterdir() if p.is_dir()), key=lambda p: p.name)[:-KEEP_PROFILES]:
                shutil.rmtree(old, ignore_errors=True)
            self.profile_log = open(folder / "game.log", "w", encoding="utf-8", buffering=1)
        except OSError as error:
            self.toasts.add_toast(Adw.Toast(title=tr("Не удалось начать профиль: {}").format(error.strerror or error)))
            return
        launcher = Gio.SubprocessLauncher.new(Gio.SubprocessFlags.STDERR_MERGE)
        launcher.set_stdout_file_path(str(folder / "profile.txt"))
        launcher.set_cwd(str(PORT_DIR))
        try:
            # Not in the game's process group: stopping the game leaves the profile to write its summary.
            self.profile_proc = launcher.spawnv([
                sys.executable, str(PORT_DIR / "scripts" / "bbport_profile.py"), "record", "--session", str(pid),
                "--out", str(folder), "--game-log", str(folder / "game.log")])
        except GLib.Error as error:
            self.close_profile_log()
            self.toasts.add_toast(Adw.Toast(title=tr("Не удалось начать профиль: {}").format(error.message)))
            return
        self.profile_proc.wait_async(None, self.on_profile_done, folder)
        self.update_profile_rows()

    def profile_line(self, line):
        if self.profile_log is not None:
            try:
                self.profile_log.write(line + "\n")
            except OSError:
                self.close_profile_log()

    def close_profile_log(self):
        """The game's output ends (it exited): the profile sees the game gone and writes its summary."""
        if self.profile_log is not None:
            try:
                self.profile_log.close()
            except OSError:
                pass
            self.profile_log = None

    def stop_profile(self):
        if self.profile_proc is not None:
            self.profile_proc.send_signal(signal.SIGTERM)  # it writes the summary of what it has
        self.close_profile_log()

    def on_profile_done(self, process, result, folder):
        try:
            process.wait_finish(result)
        except GLib.Error:
            pass
        self.profile_proc = None
        self.close_profile_log()
        try:
            summary = (folder / "summary.txt").read_text(encoding="utf-8")
        except OSError:
            summary = ""
        if summary:
            self.append_log(tr("\n— профиль производительности: {} —\n").format(folder) + summary)
            self.toasts.add_toast(Adw.Toast(title=tr("Профиль записан: {}").format(folder)))
        else:
            self.append_log(tr("\n— профиль не записан (подробности: {}) —\n").format(folder / "profile.txt"))
        self.update_profile_rows()

    def on_compare_profiles(self, _button):
        folders = profile_folders()
        if len(folders) < 2:
            self.toasts.add_toast(Adw.Toast(title=tr("Нужно два профиля: запустите игру ещё раз")))
            return
        import bbport_profile  # scripts/ (on sys.path)
        try:
            text = bbport_profile.compare(folders[1], folders[0])
        except (OSError, ValueError, KeyError) as error:
            self.toasts.add_toast(Adw.Toast(title=tr("Не удалось сравнить профили: {}").format(error)))
            return
        self.append_log(tr("\n— сравнение профилей (A — более ранний, B — последний) —\n") + text)
        self.select_section("log")

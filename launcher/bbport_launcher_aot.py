# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The native CPU (aarch64 devices only): the game's x86-64 code compiled ahead of time on this device into libbbaot.so
(scripts/aot_build.py runs bb-recomp, the Bloodborne Android Rebuild Tool), and the switch that runs the game with it
(BB_AOT=auto: run.sh and cpu/aot). Experimental: without a library, or with the switch off, FEXCore runs the game."""

import platform

from bbport_launcher_common import *  # noqa: F401,F403
import aot_build  # noqa: E402  (scripts/, on sys.path through bbport_launcher_common)

# bb-recomp's BBR-PROGRESS stages in order, with their share of the time (aot_build.STAGE_WEIGHTS) and how the row names them.
AOT_STAGES = ("scan", "discover", "lift", "compile", "variants", "link")
AOT_STAGE_NAMES = {
    "scan": "Анализ кода игры",
    "discover": "Поиск кода",
    "lift": "Разбор кода",
    "compile": "Компиляция",
    "variants": "Варианты патчей",
    "link": "Сборка библиотеки",
    "json": "Проверка",
    "done": "Проверка",
}
# aot_build.py exit statuses that have their own message (the rest: its last line).
AOT_BUILD_ERRORS = {
    3: "Нет компилятора для этого устройства: соберите AppImage рядом с исходниками Rebuild Tool (build_docker.sh) "
       "или командой ./build_aot_docker.sh device-tool на ПК или Mac и скопируйте папку aot-tool в {}",
    4: "Папка игры не подходит: нужен Bloodborne 1.09 с чистым eboot.bin (см. раздел «Игра»)",
}


def native_cpu_available():
    """Only aarch64 hosts have a second CPU: on x86-64 the game's code runs on the CPU itself."""
    return platform.machine() == "aarch64"


class NativeCpu:
    """Part of LauncherWindow (bbport_launcher.py)."""

    def build_native_cpu_group(self):
        group = Adw.PreferencesGroup(
            title=tr("Нативный CPU (эксперимент)"),
            description=tr("Код игры заранее переводится в код ARM на этом устройстве (bb-recomp) вместо перевода "
                           "во время игры (FEX): меньше нагрузка на CPU и нагрев, нет подвисаний в новых местах. "
                           "Файл — это код вашей игры: он остаётся на устройстве, не делитесь им."))
        self.native_cpu_row = Adw.SwitchRow(title=tr("Запускать игру нативным кодом"),
                                            active=bool(self.settings.get("native_cpu", False)))
        self.native_cpu_row.connect("notify::active", self.on_native_cpu_switch)
        group.add(self.native_cpu_row)
        self.aot_row = Adw.ActionRow(title=tr("Нативный код игры"))
        self.aot_button = Gtk.Button(label=tr("Собрать"), valign=Gtk.Align.CENTER)
        self.aot_button.connect("clicked", self.on_aot_button)
        self.aot_row.add_suffix(self.aot_button)
        group.add(self.aot_row)
        self.aot_tool_row = Adw.ActionRow(title=tr("Компилятор (bb-recomp)"))
        self.aot_tool_row.add_suffix(flat_button("folder-open-symbolic", tr("Выбрать папку aot-tool"),
                                                 self.on_choose_aot_tool))
        group.add(self.aot_tool_row)
        self.update_aot_rows()
        return group

    def aot_tool_dir(self):
        return self.settings.get("aot_tool_dir") or None

    def update_aot_rows(self, progress=None):
        if not hasattr(self, "aot_row"):
            return
        building = self.aot_build is not None
        game_ok = bool(self.settings["game_dir"]) and (self.game_dir() / "eboot.bin").is_file()
        try:
            state = aot_build.status(DATA_DIR, self.game_dir(), self.aot_tool_dir())
        except Exception:  # the launcher keeps working whatever the folder holds
            state = {"tool": None, "built": False, "interrupted": False, "misses_new": 0}
        self.aot_state = state
        if state["tool"]:
            self.aot_tool_row.set_subtitle(tr("{} (bb-recomp {})").format(state["tool"], state.get("tool_version") or "?"))
        else:
            self.aot_tool_row.set_subtitle(tr(
                "Не найден: AppImage содержит его, если собран рядом с исходниками Rebuild Tool (build_docker.sh). "
                "Или соберите его на ПК или Mac без файлов игры: ./build_aot_docker.sh device-tool, "
                "и скопируйте папку aot-tool в {}").format(DATA_DIR / "aot-tool"))
        if progress:
            self.aot_row.set_subtitle(progress)
        elif not building:
            if state["built"]:
                text = tr("Готов: {}, -O{}, {}").format((state.get("created") or "?").replace("T", " "),
                                                        state.get("opt"), state.get("host"))
                if state.get("outdated"):
                    text += "\n" + tr("Собран по старым правилам (часть кода без нативной версии): пересоберите, "
                                       "до тех пор игру запускает FEX")
                elif state.get("trap_units"):
                    text += "\n" + tr("{} фрагм. кода без нативной версии (инструкции, которые bb-recomp не "
                                       "переводит): игра остановится, если дойдёт до них").format(state["trap_units"])
                if state.get("built_with") and state.get("tool_version") and state["built_with"] != state["tool_version"]:
                    text += "\n" + tr("Собран другой версией bb-recomp ({}): пересоберите").format(state["built_with"])
                if state.get("misses_new"):
                    text += "\n" + tr("Игра дошла до кода, которого нет в библиотеке ({} адр.): пересоберите").format(
                        state["misses_new"])
            elif state["interrupted"]:
                text = tr("Сборка прервана: «Собрать» продолжит с того же места")
            else:
                text = tr("Не собран. Несколько минут, около 4 ГБ свободной памяти и 1 ГБ места; "
                          "подключите зарядку, устройство нагреется")
            self.aot_row.set_subtitle(text)
        self.aot_button.set_label(tr("Отменить") if building else
                                  tr("Пересобрать") if state["built"] else tr("Собрать"))
        self.aot_button.set_sensitive(building or (bool(state["tool"]) and game_ok and self.process is None))
        if self.native_cpu_row.get_active() and not state["built"]:
            self.native_cpu_row.set_subtitle(tr("Библиотеки для этой игры пока нет: игру запустит FEX"))
        elif self.native_cpu_row.get_active() and state.get("outdated"):
            self.native_cpu_row.set_subtitle(tr("Библиотеку нужно пересобрать: пока игру запустит FEX"))
        else:
            self.native_cpu_row.set_subtitle("")

    def on_native_cpu_switch(self, row, _param):
        self.settings["native_cpu"] = row.get_active()
        save_settings(self.settings)
        self.update_aot_rows()

    def on_choose_aot_tool(self, _button):
        def chosen(path):
            problem = aot_build.tool_problem(Path(path))
            if problem:
                self.alert(tr("Это не папка aot-tool"), tr("{}: {}").format(path, problem))
                return
            self.settings["aot_tool_dir"] = path
            save_settings(self.settings)
            self.update_aot_rows()
        self.choose_folder(tr("Папка aot-tool (bin/bb-recomp)"), self.aot_tool_dir() or DATA_DIR, chosen)

    def on_aot_button(self, _button):
        if self.aot_build:
            self.stop_aot_build()
        else:
            self.start_aot_build()

    def aot_build_command(self):
        command = [sys.executable, str(PORT_DIR / "scripts" / "aot_build.py"),
                   "--game", str(self.game_dir()), "--data", str(DATA_DIR)]
        if self.aot_tool_dir():
            command += ["--tool", self.aot_tool_dir()]
        return command

    def start_aot_build(self):
        if self.process is not None:
            return
        self.store()
        launcher = Gio.SubprocessLauncher.new(
            Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        launcher.set_environ([f"{k}={v}" for k, v in os.environ.items()])
        launcher.set_cwd(str(PORT_DIR))
        try:
            # setsid: the script and bb-recomp form one process group, stopped together (SIGTERM: the finished
            # parts stay, the next build goes on from there).
            self.aot_build = launcher.spawnv(["setsid", *self.aot_build_command()])
        except GLib.Error as error:
            self.toasts.add_toast(Adw.Toast(title=tr("Не удалось запустить: {}").format(error.message)))
            return
        self.aot_cancelled = False
        self.aot_result = ""
        self.aot_started = time.monotonic()
        self.aot_stage = None
        self.append_log(tr("\n— сборка нативного кода игры —\n"))
        stream = Gio.DataInputStream.new(self.aot_build.get_stdout_pipe())
        stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self.on_aot_line)
        self.aot_build.wait_async(None, self.on_aot_built)
        self.update_launch_button()
        self.update_aot_rows(tr("Подготовка образа игры…"))

    def aot_progress_text(self, stage, done, total):
        """The row's text for a BBR-PROGRESS line: the stage, the whole build's share done and, once the compile
        is under way, the time left."""
        weights = aot_build.STAGE_WEIGHTS
        finished = sum(weights.get(s, 0) for s in AOT_STAGES[:AOT_STAGES.index(stage)]) if stage in AOT_STAGES else 100
        fraction = (finished + weights.get(stage, 0) * (done / total if total else 1)) / 100
        text = tr("{}: {} %").format(tr(AOT_STAGE_NAMES.get(stage, stage)), int(fraction * 100))
        elapsed = time.monotonic() - self.aot_started
        if stage == "compile" and fraction > 0.1:
            left = max(0, elapsed / fraction * (1 - fraction))
            text += tr(", осталось около {} мин").format(max(1, round(left / 60)))
        return text

    def on_aot_line(self, stream, result):
        try:
            line, _length = stream.read_line_finish_utf8(result)
        except GLib.Error:
            return
        if line is None:
            return
        if line.startswith("BBR-PROGRESS "):
            parts = line.split()
            if len(parts) == 4 and self.aot_build:
                try:
                    self.update_aot_rows(self.aot_progress_text(parts[1], int(parts[2]), int(parts[3])))
                except ValueError:
                    pass
        else:
            self.append_log(line + "\n")
            if line.startswith("AOT-DONE "):
                self.aot_result = line.split(" ", 2)[2] if line.count(" ") >= 2 else ""
            elif line.startswith("aot: ") and self.aot_build and "compiling:" not in line:
                self.update_aot_rows(line[len("aot: "):])
        stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self.on_aot_line)

    def stop_aot_build(self):
        pid = int(self.aot_build.get_identifier())
        self.aot_cancelled = True
        self.update_aot_rows(tr("Останавливается (до нескольких секунд)…"))
        try:
            os.killpg(pid, signal.SIGTERM)
        except OSError:
            self.aot_build.force_exit()

    def on_aot_built(self, process, result):
        try:
            process.wait_finish(result)
        except GLib.Error:
            pass
        status = process.get_exit_status() if process.get_if_exited() else -1
        self.aot_build = None
        cancelled = getattr(self, "aot_cancelled", False)
        self.append_log(tr("— сборка нативного кода завершилась (код {}) —\n").format(status))
        if status == 0:
            if not self.native_cpu_row.get_active():
                self.native_cpu_row.set_active(True)  # built to be used: on (the switch turns it off again)
            self.toasts.add_toast(Adw.Toast(title=tr("Нативный код собран: игра запустится с ним")))
        elif cancelled or status in (130, 143):
            self.toasts.add_toast(Adw.Toast(title=tr("Сборка остановлена: «Собрать» продолжит с того же места")))
        else:
            message = AOT_BUILD_ERRORS.get(status)
            body = tr(message).format(DATA_DIR / "aot-tool") if message else (
                self.aot_result or tr("Сборка не удалась (код {}): подробности в журнале").format(status))
            self.alert(tr("Нативный код не собран"), body)
        self.update_launch_button()
        self.update_aot_rows()

# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The save point (save scumming): its switch in Mods and patches, off by default, and while it is
on a button next to Play with the actions (scripts/save_snapshots.py does the work). The game has
the same actions in its overlay menu (src/runtime_snapshot.c)."""

from bbport_launcher_common import *  # noqa: F401,F403

# What made a copy (meta.json "source"), as the popover says it.
SAVE_POINT_SOURCES = {"launcher": "в лаунчере", "game": "в игре"}


def save_point_time(meta):
    """The copy's time in local time (digits only: the same in every language)."""
    created = (meta or {}).get("created_unix")
    if not isinstance(created, int) or created <= 0:
        return "?"
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(created))


class SavePoint:
    """Part of LauncherWindow (bbport_launcher.py)."""

    def build_save_point_group(self):
        """The switch in Mods and patches (its description from settings_schema.json)."""
        group = Adw.PreferencesGroup(title=tr("Точка сохранения"))
        self.save_snapshots_row = Adw.SwitchRow(title=tr("Точка сохранения (сейв-скам)"),
                                                active=self.settings.get("save_snapshots", False))
        self.save_snapshots_row.connect("notify::active", self.on_save_snapshots_switch)
        group.add(self.save_snapshots_row)
        return group

    def build_save_point_button(self):
        """The button left of Play and its popover, rebuilt each time it opens."""
        self.save_point_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6,
                                      margin_top=12, margin_bottom=12, margin_start=12, margin_end=12)
        self.save_point_popover = Gtk.Popover(child=self.save_point_box)
        self.save_point_popover.connect("show", lambda _popover: self.fill_save_point_popover())
        self.save_point_button = Gtk.MenuButton(icon_name="document-save-symbolic",
                                                tooltip_text=tr("Точка сохранения"),
                                                popover=self.save_point_popover)
        self.content_header.pack_end(self.save_point_button)
        self.update_save_point_button()

    def on_save_snapshots_switch(self, row, _param):
        self.settings["save_snapshots"] = row.get_active()
        save_settings(self.settings)
        self.update_save_point_button()

    def update_save_point_button(self):
        if hasattr(self, "save_point_button"):
            self.save_point_button.set_visible(self.save_snapshots_row.get_active())

    def save_point_running(self):
        """The game runs: started here, or elsewhere (the desktop shortcut, --play) with this
        saves folder."""
        return self.process is not None or save_snapshots.game_running(self.user_dir())

    def fill_save_point_popover(self):
        box = self.save_point_box
        while (child := box.get_first_child()) is not None:
            box.remove(child)
        user = self.user_dir()
        state = save_snapshots.info(user)
        running = self.save_point_running()
        point, undo = state["snapshot"], state["undo"]

        title = Gtk.Label(label=tr("Точка сохранения"), xalign=0)
        title.add_css_class("heading")
        box.append(title)
        if point:
            source = SAVE_POINT_SOURCES.get(point.get("source"))
            text = tr("Сделана: {}").format(save_point_time(point))
            if source:
                text += " (" + tr(source) + ")"
        else:
            text = tr("Точки сохранения пока нет")
        status = Gtk.Label(label=text, xalign=0, wrap=True, max_width_chars=40)
        status.add_css_class("dim-label")
        box.append(status)

        def action(label, handler, sensitive=True, css=None):
            button = Gtk.Button(label=label)
            if css:
                button.add_css_class(css)
            button.set_sensitive(sensitive and not running)
            button.connect("clicked", lambda _button: (self.save_point_popover.popdown(), handler()))
            box.append(button)
            return button

        action(tr("Сделать точку сейчас"), self.on_save_point_backup, state["saves"])
        action(tr("Вернуться к точке"), lambda: self.on_save_point_restore(False), point is not None,
               "suggested-action")
        if undo:
            action(tr("Отменить возврат"), lambda: self.on_save_point_restore(True))
        open_button = Gtk.Button(label=tr("Открыть папку"))
        open_button.connect("clicked", lambda _button: (self.save_point_popover.popdown(),
                                                        open_folder(self, user)))
        box.append(open_button)
        note = None
        if running:
            note = tr("Игра запущена: точка сохранения — в меню порта в игре (Insert / L3+R3)")
        elif not state["saves"]:
            note = tr("Сохранений пока нет: сначала поиграйте")
        if note:
            hint = Gtk.Label(label=note, xalign=0, wrap=True, max_width_chars=40)
            hint.add_css_class("dim-label")
            box.append(hint)

    def confirm_save_point(self, heading, body, label, done):
        """Asks before an action that replaces a copy; `done` runs on yes."""
        dialog_class = getattr(Adw, "AlertDialog", None) or getattr(Adw, "MessageDialog", None)
        if dialog_class is None:
            done()
            return
        if dialog_class is getattr(Adw, "AlertDialog", None):
            dialog = dialog_class(heading=heading, body=body)
        else:
            dialog = dialog_class(transient_for=self, heading=heading, body=body)
        dialog.add_response("cancel", tr("Отмена"))
        dialog.add_response("yes", label)
        dialog.set_response_appearance("yes", Adw.ResponseAppearance.SUGGESTED)
        dialog.set_default_response("cancel")
        dialog.set_close_response("cancel")
        dialog.connect("response", lambda _dialog, response: response == "yes" and done())
        if dialog_class is getattr(Adw, "AlertDialog", None):
            dialog.present(self)
        else:
            dialog.present()

    def save_point_failed(self, error):
        texts = {
            "no_saves": tr("Сохранений пока нет: сначала поиграйте"),
            "none": tr("Точки сохранения пока нет"),
            "running": tr("Игра запущена: точка сохранения — в меню порта в игре (Insert / L3+R3)"),
        }
        text = texts.get(error.code) or tr("Не удалось скопировать сохранения: {}").format(error)
        self.toasts.add_toast(Adw.Toast(title=text, timeout=8))

    def on_save_point_backup(self):
        point = save_snapshots.info(self.user_dir())["snapshot"]

        def backup():
            try:
                save_snapshots.backup(self.user_dir(), "launcher")
            except save_snapshots.SnapshotError as error:
                self.save_point_failed(error)
                return
            self.toasts.add_toast(Adw.Toast(title=tr("Точка сохранения сделана")))
        if point is None:
            backup()
            return
        self.confirm_save_point(
            tr("Заменить точку сохранения?"),
            tr("Точка от {} будет заменена текущими сохранениями.").format(save_point_time(point)),
            tr("Заменить"), backup)

    def on_save_point_restore(self, undo):
        state = save_snapshots.info(self.user_dir())
        meta = state["undo" if undo else "snapshot"]
        if meta is None:
            self.save_point_failed(save_snapshots.SnapshotError("none", ""))
            return

        def restore():
            try:
                save_snapshots.restore(self.user_dir(), undo=undo)
            except save_snapshots.SnapshotError as error:
                self.save_point_failed(error)
                return
            self.update_user_status()
            self.toasts.add_toast(Adw.Toast(title=tr("Возврат отменён") if undo
                                            else tr("Сохранения возвращены к точке")))
        if undo:
            self.confirm_save_point(
                tr("Отменить возврат к точке?"),
                tr("Сохранения снова станут такими, какими были до возврата ({}).").format(save_point_time(meta)),
                tr("Отменить возврат"), restore)
        else:
            self.confirm_save_point(
                tr("Вернуться к точке сохранения?"),
                tr("Сохранения от {} встанут на место текущих. Текущий прогресс останется копией для отмены.").format(
                    save_point_time(meta)),
                tr("Вернуться"), restore)

# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher window driven by a gamepad (handhelds in game mode: no mouse, no keyboard).

bb-gpu-capabilities --pad-events reports the presses through SDL (the game's own gamepad
mapping; under Steam, the controller Steam Input gives). They move the keyboard focus between
the window's controls by where those are on screen, and press the focused one:

    D-pad, left stick    move (held: repeats)     A        press, open a list, toggle
    B                    back: closes a list or a dialog, leaves a row's buttons, returns to
                         the sections
    LB, RB               previous, next section   Start    Play
    Right stick          scroll                   LT, RT   scroll a page
    Left, right on a number: changes it (held: faster)

A and B go by what the button says (Nintendo layouts: A is the east button). The helper runs
while the window waits for the player: not while the game runs (it reads the gamepad itself)
nor while a binding is read (--read-input). The presses count while the launcher's window (or
its dialog) is the active one. Launcher setting gamepad_navigation: off stops it.
"""

from bbport_launcher_common import *  # noqa: F401,F403

PAD_REPEAT_DELAY_MS = 380  # a held direction repeats after this, then every PAD_REPEAT_MS
PAD_REPEAT_MS = 110
PAD_SCROLL_MS = 40  # the right stick scrolls PAD_SCROLL_STEP pixels this often
PAD_SCROLL_STEP = 48
PAD_MOVES = {
    "dpup": "up", "left-up": "up", "dpdown": "down", "left-down": "down",
    "dpleft": "left", "left-left": "left", "dpright": "right", "left-right": "right",
}
PAD_ACTIONS = {
    "leftshoulder": "previous", "rightshoulder": "next", "start": "play",
    "right-up": "scroll-up", "right-down": "scroll-down",
    "lefttrigger": "page-up", "righttrigger": "page-down",
}
PAD_REPEATING = {"up", "down", "left", "right", "scroll-up", "scroll-down", "page-up", "page-down"}


def pad_face_action(name, label):
    """A presses and B goes back by the button's label; unlabelled (SDL does not know it), by
    position: south presses, east goes back."""
    if label in ("A", "cross") or (label is None and name == "a"):
        return "activate"
    if label in ("B", "circle") or (label is None and name == "b"):
        return "back"
    return None


def pad_score(direction, origin, rect):
    """How far rect (x0, y0, x1, y1) lies from origin in direction, None when it does not lie
    that way (its centre and its near edge both past origin's). Widgets beside the way come last
    (the sections from a page's first row going up); then the gap along the move counts, three
    times the gap across it, then the centres' distances: a row's buttons left to right, the
    nearest of equals, a smaller widget before the one around it."""
    ox0, oy0, ox1, oy1 = origin
    x0, y0, x1, y1 = rect
    ocx, ocy, cx, cy = (ox0 + ox1) / 2, (oy0 + oy1) / 2, (x0 + x1) / 2, (y0 + y1) / 2
    if direction == "down":
        if cy <= ocy + 1 or y0 <= oy0 + 1:
            return None
        along, across, ahead, aside = max(0, y0 - oy1), max(0, x0 - ox1, ox0 - x1), cy - ocy, abs(cx - ocx)
    elif direction == "up":
        if cy >= ocy - 1 or y1 >= oy1 - 1:
            return None
        along, across, ahead, aside = max(0, oy0 - y1), max(0, x0 - ox1, ox0 - x1), ocy - cy, abs(cx - ocx)
    elif direction == "right":
        if cx <= ocx + 1 or x0 <= ox0 + 1:
            return None
        along, across, ahead, aside = max(0, x0 - ox1), max(0, y0 - oy1, oy0 - y1), cx - ocx, abs(cy - ocy)
    else:
        if cx >= ocx - 1 or x1 >= ox1 - 1:
            return None
        along, across, ahead, aside = max(0, ox0 - x1), max(0, y0 - oy1, oy0 - y1), ocx - cx, abs(cy - ocy)
    return ((1000 if across > 0 else 0) + along + 3 * across + 0.1 * ahead + 0.02 * aside
            + 1e-7 * (x1 - x0) * (y1 - y0))


def pad_accel(count):
    """A held left or right on a number: steps of 1, then 5, then 20 of its increment."""
    return 1 if count < 10 else 5 if count < 25 else 20


_PAD_TYPES = None


def pad_types():
    """(atomic, containers, skipped) widget classes. Atomic: one target, its insides never (an
    entry's text, a spin row's buttons). Containers: never a target themselves (the focus goes to
    their rows or items). Skipped: never looked into (the window's own close button)."""
    global _PAD_TYPES
    if _PAD_TYPES is None:
        def classes(*names):
            found = (getattr(Adw if name.startswith("Adw.") else Gtk, name.split(".", 1)[1], None)
                     for name in names)
            return tuple(cls for cls in found if isinstance(cls, type))
        _PAD_TYPES = (
            classes("Gtk.SpinButton", "Gtk.Entry", "Gtk.Text", "Gtk.PasswordEntry", "Gtk.SearchEntry",
                    "Gtk.Scale", "Gtk.DropDown", "Adw.SpinRow", "Adw.EntryRow"),
            # The log's text view too: focused, it would scroll the log to its top; the right
            # stick scrolls it.
            classes("Gtk.ListBox", "Gtk.FlowBox", "Gtk.ListView", "Gtk.GridView", "Gtk.ColumnView",
                    "Gtk.ScrolledWindow", "Gtk.Viewport", "Gtk.Stack", "Gtk.Label", "Gtk.TextView",
                    "Adw.ExpanderRow", "Adw.Dialog"),
            classes("Gtk.WindowControls", "Gtk.Popover"),
        )
    return _PAD_TYPES


class GamepadNavigation:
    """Part of LauncherWindow (bbport_launcher.py)."""

    # --- the helper ------------------------------------------------------------------------

    def pad_init(self):
        if not hasattr(self, "pad_held"):
            self.pad_held = {}  # button name -> action, while held
            self.pad_timers = {}  # button name -> repeat timer
            self.pad_memory = {}  # section -> the widget focused there last
            self.pad_process = None
            self.pad_cancel = None
            self.pad_guid = None
            self.pad_failures = 0
            self.pad_started = 0.0
            self.pad_list = None  # the popover list moved in by position, and where
            self.pad_list_index = 0

    def pad_chosen_guid(self):
        """The controller chosen on the Controls page ("" for the first one connected); the
        saved one while the row is being made."""
        row = getattr(self, "gamepad_row", None)
        values = getattr(row, "values", None) if row is not None else None
        if values:
            selected = row.get_selected()
            return values[selected] if 0 <= selected < len(values) else ""
        return self.settings.get("gamepad", "") or ""

    def pad_enabled(self):
        row = getattr(self, "pad_nav_row", None)
        return row.get_active() if row is not None else self.settings.get("gamepad_navigation", True)

    def start_pad_navigation(self):
        """The helper, unless it runs, the setting is off or the game runs."""
        self.pad_init()
        if self.pad_process is not None or not self.pad_enabled() or getattr(self, "process", None):
            return
        tool = PORT_DIR / ("bin" if PACKAGED else "out") / "bb-gpu-capabilities"
        if not tool.is_file():
            return
        guid = self.pad_chosen_guid()
        try:
            process = Gio.Subprocess.new([str(tool), "--pad-events"] + ([guid] if guid else []),
                                         Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_SILENCE)
        except GLib.Error:
            return
        self.pad_process, self.pad_guid, self.pad_started = process, guid, time.monotonic()
        self.pad_cancel = Gio.Cancellable()
        stream = Gio.DataInputStream.new(process.get_stdout_pipe())
        stream.read_line_async(GLib.PRIORITY_DEFAULT, self.pad_cancel, self.on_pad_line, process)
        process.wait_async(None, self.on_pad_exit)

    def stop_pad_navigation(self):
        self.pad_init()
        process, self.pad_process = self.pad_process, None
        self.pad_release_all()
        if self.pad_cancel is not None:
            self.pad_cancel.cancel()
            self.pad_cancel = None
        if process is not None:
            process.force_exit()

    def pad_settings_changed(self):
        """The switch or the chosen controller changed: the helper (re)started or stopped."""
        self.pad_init()
        if not self.pad_enabled():
            self.stop_pad_navigation()
            return
        if self.pad_process is None or self.pad_chosen_guid() != self.pad_guid:
            self.stop_pad_navigation()
            self.pad_failures = 0
            self.start_pad_navigation()

    def on_pad_line(self, stream, result, process):
        try:
            line, _length = stream.read_line_finish_utf8(result)
        except GLib.Error:
            return
        if line is None or process is not self.pad_process:
            return
        self.pad_event(line)
        stream.read_line_async(GLib.PRIORITY_DEFAULT, self.pad_cancel, self.on_pad_line, process)

    def on_pad_exit(self, process, result):
        try:
            process.wait_finish(result)
        except GLib.Error:
            pass
        if process is not self.pad_process:
            return  # stopped on purpose
        self.pad_process = None
        self.pad_release_all()
        # Ended by itself: again a few times (a helper without --pad-events, or without SDL's
        # gamepad support, ends at once every time).
        self.pad_failures = 0 if time.monotonic() - self.pad_started > 60 else self.pad_failures + 1
        if self.pad_failures <= 3:
            GLib.timeout_add_seconds(2 * self.pad_failures + 1, lambda: self.start_pad_navigation() and False)

    # --- presses ---------------------------------------------------------------------------

    def pad_event(self, line):
        words = line.split()
        if not words:
            return
        if words[0] == "reset":
            self.pad_release_all()
            return
        if words[0] not in ("down", "up") or len(words) < 2:
            return  # ready, tick
        name = words[1]
        self.pad_release(name)
        if words[0] == "up":
            return
        action = (PAD_MOVES.get(name) or PAD_ACTIONS.get(name)
                  or pad_face_action(name, words[2] if len(words) > 2 else None))
        window = self.pad_window()
        if action is None or window is None:
            return
        self.pad_held[name] = action
        self.pad_do(window, action, 0)
        if action in PAD_REPEATING:
            self.pad_timers[name] = GLib.timeout_add(PAD_REPEAT_DELAY_MS, self.pad_repeat, name, 1)

    def pad_repeat(self, name, count):
        self.pad_timers.pop(name, None)
        action = self.pad_held.get(name)
        window = self.pad_window()
        if action is None or window is None:
            self.pad_held.pop(name, None)
            return False
        self.pad_do(window, action, count)
        interval = PAD_SCROLL_MS if action.startswith("scroll") else PAD_REPEAT_MS
        self.pad_timers[name] = GLib.timeout_add(interval, self.pad_repeat, name, count + 1)
        return False

    def pad_release(self, name):
        self.pad_held.pop(name, None)
        timer = self.pad_timers.pop(name, None)
        if timer is not None:
            GLib.source_remove(timer)

    def pad_release_all(self):
        for name in list(self.pad_held) + list(self.pad_timers):
            self.pad_release(name)

    def pad_window(self):
        """The window the presses go to: this one or its dialog window while active (the folder
        chooser), None otherwise: the game runs, another program is in front, the setting is off."""
        if getattr(self, "process", None) is not None or not self.pad_enabled():
            return None
        if self.is_active():
            return self
        for window in Gtk.Window.list_toplevels():
            if window != self and window.is_active() and window.get_transient_for() == self:
                return window
        # gamescope (Steam's game mode) shows one program at a time, and its windows may never be
        # told they are active: the dialog window when one shows, else this one.
        if os.environ.get("GAMESCOPE_WAYLAND_DISPLAY") and self.get_mapped():
            for window in Gtk.Window.list_toplevels():
                if window != self and window.get_mapped() and window.get_transient_for() == self:
                    return window
            return self
        return None

    def pad_do(self, window, action, count):
        window.set_focus_visible(True)
        if action in ("up", "down", "left", "right"):
            self.pad_move(window, action, count)
        elif action == "activate":
            self.pad_activate(window)
        elif action == "back":
            self.pad_back(window)
        elif action in ("previous", "next") and window is self:
            self.pad_section(-1 if action == "previous" else 1)
        elif action == "play" and window is self:
            if self.launch_button.get_sensitive():
                self.on_launch(self.launch_button)
        elif action.startswith("scroll-") or action.startswith("page-"):
            self.pad_scroll(window, -1 if action.endswith("-up") else 1, action.startswith("page-"))

    # --- what can take the focus -----------------------------------------------------------

    def pad_scope(self, window):
        """The focus moves within an open list or menu (popover), a dialog, else the window."""
        dialog_class = getattr(Adw, "Dialog", None)
        widget = window.get_focus()
        while widget is not None:
            if isinstance(widget, Gtk.Popover) or (isinstance(dialog_class, type) and isinstance(widget, dialog_class)):
                return widget
            widget = widget.get_parent()
        if window is self and hasattr(self, "get_visible_dialog"):
            dialog = self.get_visible_dialog()
            if dialog is not None:
                return dialog
        return self.pad_open_popover(window) or window

    def pad_open_popover(self, root):
        found = None
        child = root.get_first_child()
        while child is not None:
            if child.get_mapped():
                if isinstance(child, Gtk.Popover):
                    found = child
                found = self.pad_open_popover(child) or found
            child = child.get_next_sibling()
        return found

    def pad_targets(self, window, scope):
        """The widgets the focus can go to in scope, in tree order."""
        atomic, containers, skipped = pad_types()
        targets = []

        def visit(parent):
            widget = parent.get_first_child()
            while widget is not None:
                # can-focus off: nothing inside takes the focus either (a combo row's own list
                # showing its value).
                if widget.get_mapped() and widget.is_sensitive() and widget.get_can_focus() and \
                        not isinstance(widget, skipped):
                    if isinstance(widget, atomic):
                        targets.append(widget)
                    else:
                        if not isinstance(widget, containers) and widget.get_focusable() and \
                                not self.pad_row_switch(widget):
                            targets.append(widget)
                        visit(widget)
                widget = widget.get_next_sibling()

        visit(scope)
        if window is self and scope is self:
            # A section's page fading out keeps its widgets a moment.
            page = self.stack.get_visible_child()
            targets = [t for t in targets if not t.is_ancestor(self.stack) or t.is_ancestor(page)]
        return targets

    @staticmethod
    def pad_row_switch(widget):
        """A row's own switch or check box (the row toggles it): the row is the target."""
        if not isinstance(widget, (Gtk.Switch, Gtk.CheckButton)):
            return False
        row = widget.get_ancestor(Adw.ActionRow)
        return row is not None and row.get_activatable_widget() == widget

    @staticmethod
    def pad_target_of(widget, targets, scope):
        """The target the focused widget belongs to (an entry row for its text)."""
        while widget is not None and widget != scope:
            if widget in targets:
                return widget
            widget = widget.get_parent()
        return None

    @staticmethod
    def pad_row_of(widget, targets):
        """The innermost row (a target) widget is in, or None."""
        while widget is not None:
            if isinstance(widget, Gtk.ListBoxRow) and widget in targets:
                return widget
            widget = widget.get_parent()
        return None

    def pad_land(self, widget, targets):
        """Coming into a row: the row, not one of its buttons."""
        row = self.pad_row_of(widget, targets)
        return row if row is not None else widget

    @staticmethod
    def pad_rect(widget, scope):
        ok, bounds = widget.compute_bounds(scope)
        if not ok:
            return None
        x, y = bounds.get_x(), bounds.get_y()
        return (x, y, x + bounds.get_width(), y + bounds.get_height())

    def pad_in_sidebar(self, widget):
        return widget is not None and widget.is_ancestor(self.sidebar)

    # --- moving ----------------------------------------------------------------------------

    def pad_focus(self, window, widget, show=True):
        if widget is None:
            return
        widget.grab_focus()
        window.set_focus_visible(True)
        if show:
            self.pad_show(widget)
        if window is self:
            if self.pad_in_sidebar(widget):
                # The section follows the focus where both panes show.
                if not self.split.get_collapsed() and self.sidebar.get_selected_row() != widget:
                    self.sidebar.select_row(widget)
            elif widget.is_ancestor(self.stack):
                self.pad_memory[self.section_name] = widget

    @staticmethod
    def pad_content(scrolled):
        """What a scrolled window scrolls, when it is a plain widget in a viewport: places in it
        do not change with the scrolling (a list view or a text view scrolls itself)."""
        child = scrolled.get_child()
        return child.get_child() if isinstance(child, Gtk.Viewport) else None

    def pad_show(self, widget):
        """Scrolled into sight (in its scrolled parents too). By its place in the scrolled content
        and the scroll now: the viewport may have scrolled to the focus already."""
        scrolled = widget.get_ancestor(Gtk.ScrolledWindow)
        while scrolled is not None:
            content = self.pad_content(scrolled)
            ok, bounds = widget.compute_bounds(content) if content is not None else (False, None)
            if ok:
                adjustment, margin = scrolled.get_vadjustment(), 24
                value, page = adjustment.get_value(), adjustment.get_page_size()
                top = bounds.get_y()
                bottom = top + bounds.get_height()
                if top < value + margin:
                    adjustment.set_value(top - margin)
                elif bottom > value + page - margin:
                    adjustment.set_value(min(top - margin, bottom - page + margin))
            parent = scrolled.get_parent()
            scrolled = parent.get_ancestor(Gtk.ScrolledWindow) if parent is not None else None

    def pad_first(self, window, scope, targets):
        """Where the focus starts: the section's last focused widget, else its first."""
        candidates = targets
        if window is self and scope is self:
            if self.split.get_collapsed() and not self.split.get_show_content():
                return self.sidebar.get_selected_row()
            remembered = self.pad_memory.get(getattr(self, "section_name", None))
            if remembered is not None and remembered in targets:
                return remembered
            page = self.stack.get_visible_child()
            candidates = [t for t in targets if t.is_ancestor(page)]
            if not candidates:  # nothing to press there (the log before the game ran)
                return None if self.split.get_collapsed() else self.sidebar.get_selected_row()
        rects = {t: self.pad_rect(t, scope) for t in candidates}
        placed = [t for t in candidates if rects[t] is not None]
        if not placed:
            return None
        return self.pad_land(min(placed, key=lambda t: (rects[t][1], rects[t][0])), targets)

    def pad_move(self, window, direction, count):
        scope = self.pad_scope(window)
        if not isinstance(scope, Gtk.Popover):
            self.pad_list = None
        elif direction in ("up", "down") and self.pad_list_move(scope, -1 if direction == "up" else 1):
            return
        targets = self.pad_targets(window, scope)
        if not targets:
            return
        current = self.pad_target_of(window.get_focus(), targets, scope)
        if current is None:
            self.pad_focus(window, self.pad_first(window, scope, targets))
            return
        if direction in ("left", "right") and \
                self.pad_adjust(current, (1 if direction == "right" else -1) * pad_accel(count)):
            return
        row = self.pad_row_of(current, targets) or current
        rects = {}

        def rect(widget):
            if widget not in rects:
                rects[widget] = self.pad_rect(widget, scope)
            return rects[widget]

        if direction in ("left", "right"):
            # Along a row: the row, then its buttons left to right.
            inside = [t for t in targets if (t == row or t.is_ancestor(row)) and rect(t) is not None]
            inside.sort(key=lambda t: (rect(t)[0], rect(t)[0] - rect(t)[2]))
            if current in inside:
                step = inside.index(current) + (1 if direction == "right" else -1)
                if 0 <= step < len(inside):
                    self.pad_focus(window, inside[step])
                    return
        origin = rect(row)
        if origin is None:
            return
        # Up and down stay on their side: the sections, or the page (past a page's end is not the
        # next section).
        pane = self.pad_in_sidebar(row) if window is self and scope is self and \
            direction in ("up", "down") else None
        best = None
        for target in targets:
            if target == row or target.is_ancestor(row) or row.is_ancestor(target) or rect(target) is None:
                continue
            if pane is not None and self.pad_in_sidebar(target) != pane:
                continue
            # Left and right: only what lies beside (past a row's last button is not the next
            # row's).
            if direction in ("left", "right") and (rect(target)[3] <= origin[1] or rect(target)[1] >= origin[3]):
                continue
            score = pad_score(direction, origin, rect(target))
            if score is not None and (best is None or score < best[0]):
                best = (score, target)
        if best is None:
            # Left from a page's lower rows (below the last section): to the open section.
            if direction == "left" and window is self and scope is self and not self.pad_in_sidebar(row) \
                    and not self.split.get_collapsed():
                self.pad_focus(window, self.sidebar.get_selected_row())
            return
        target = best[1]
        if window is self and scope is self:
            if not self.pad_in_sidebar(row) and self.pad_in_sidebar(target):
                target = self.sidebar.get_selected_row() or target  # back to the open section
            elif self.pad_in_sidebar(row) and target.is_ancestor(self.stack):
                self.pad_focus(window, self.pad_first(window, scope, targets))  # into the section
                return
        self.pad_focus(window, self.pad_land(target, targets))

    def pad_list_move(self, scope, step):
        """Up and down in a popover's list (a combo row's choices) by position: the list makes
        rows only near the visible ones, so where they are on screen does not reach them all."""
        listview = self.pad_find(scope, Gtk.ListView)
        if listview is None or not hasattr(listview, "scroll_to"):
            return False
        model = listview.get_model()
        count = model.get_n_items() if model is not None else 0
        if not count:
            return False
        if self.pad_list != listview:
            self.pad_list = listview
            combo = scope.get_ancestor(Adw.ComboRow)
            selected = combo.get_selected() if combo is not None else 0
            self.pad_list_index = selected if 0 <= selected < count else 0
            focus = scope.get_root().get_focus()
            if focus is None or not focus.is_ancestor(listview):
                step = 0  # first onto the chosen one
        self.pad_list_index = max(0, min(count - 1, self.pad_list_index + step))
        listview.scroll_to(self.pad_list_index, Gtk.ListScrollFlags.FOCUS, None)
        scope.get_root().set_focus_visible(True)
        return True

    @staticmethod
    def pad_adjust(target, step):
        """Left and right on a number change it."""
        spin_row = getattr(Adw, "SpinRow", None)
        if isinstance(target, (Gtk.SpinButton, Gtk.Scale)) or (isinstance(spin_row, type) and isinstance(target, spin_row)):
            adjustment = target.get_adjustment()
            adjustment.set_value(adjustment.get_value() + step * (adjustment.get_step_increment() or 1))
            return True
        return False

    @staticmethod
    def pad_find(root, cls):
        """The first mapped widget of class cls in root (root included)."""
        if isinstance(root, cls):
            return root
        child = root.get_first_child()
        while child is not None:
            if child.get_mapped():
                found = GamepadNavigation.pad_find(child, cls)
                if found is not None:
                    return found
            child = child.get_next_sibling()
        return None

    # --- A, B, sections, scrolling ---------------------------------------------------------

    def pad_activate(self, window):
        scope = self.pad_scope(window)
        if isinstance(scope, Gtk.Popover):
            listview = self.pad_find(scope, Gtk.ListView)
            if listview is not None and self.pad_list_move(scope, 0):
                self.pad_list = None
                combo = scope.get_ancestor(Adw.ComboRow)
                if combo is None:
                    listview.activate_action("list.activate-item", GLib.Variant.new_uint32(self.pad_list_index))
                    return
                # The row's own choice, found by its item (a search filters the popover's list).
                item, model = listview.get_model().get_item(self.pad_list_index), combo.get_model()
                for position in range(model.get_n_items() if model is not None else 0):
                    if model.get_item(position) == item:
                        combo.set_selected(position)
                        break
                scope.popdown()
                return
        targets = self.pad_targets(window, scope)
        current = self.pad_target_of(window.get_focus(), targets, scope)
        if current is None:
            self.pad_focus(window, self.pad_first(window, scope, targets))
            return
        if window is self and self.pad_in_sidebar(current):
            self.sidebar.select_row(current)
            # After the switch: the section's page (collapsed: it slides in over the sections).
            GLib.idle_add(lambda: self.pad_enter_section() and False)
            return
        atomic = pad_types()[0]
        if isinstance(current, Gtk.ListBoxRow) and not current.get_activatable() and not isinstance(current, atomic):
            # A row that does nothing itself: into its buttons.
            inside = [t for t in targets if t != current and t.is_ancestor(current)]
            if inside:
                rects = {t: self.pad_rect(t, scope) for t in inside}
                self.pad_focus(window, min(inside, key=lambda t: rects[t][0] if rects[t] else 0))
            return
        current.activate()

    def pad_enter_section(self):
        targets = self.pad_targets(self, self)
        if targets:
            self.pad_focus(self, self.pad_first(self, self, targets))

    def pad_back(self, window):
        scope = self.pad_scope(window)
        dialog_class = getattr(Adw, "Dialog", None)
        if isinstance(scope, Gtk.Popover):
            scope.popdown()
            self.pad_list = None
            return
        if isinstance(dialog_class, type) and isinstance(scope, dialog_class):
            scope.close()
            return
        targets = self.pad_targets(window, scope)
        current = self.pad_target_of(window.get_focus(), targets, scope)
        row = self.pad_row_of(current, targets) if current is not None else None
        if row is not None and current != row and not isinstance(row, pad_types()[0]):
            self.pad_focus(window, row)  # out of a row's buttons
            return
        if window is not self:
            window.close()  # the folder chooser: cancelled
            return
        if current is not None and self.pad_in_sidebar(current):
            return
        selected = self.sidebar.get_selected_row()
        if selected is None:
            return
        if self.split.get_collapsed():
            # The sections slide in: focused once they are there.
            self.split.set_show_content(False)
            GLib.idle_add(lambda: self.pad_focus(self, selected) and False)
        else:
            self.pad_focus(self, selected)

    def pad_section(self, step):
        names = list(self.sidebar_rows)
        current = getattr(self, "section_name", names[0])
        name = names[(names.index(current) + step) % len(names)] if current in names else names[0]
        in_sidebar = self.pad_in_sidebar(self.get_focus())
        self.select_section(name)
        if in_sidebar and not self.split.get_collapsed():
            self.pad_focus(self, self.sidebar_rows[name])
        else:
            GLib.idle_add(lambda: self.pad_enter_section() and False)

    def pad_scroll(self, window, sign, page):
        """The right stick and the triggers scroll the focused widget's page (the section's when
        the focus is on the sections); a focus scrolled out of sight goes to what came in."""
        scope = self.pad_scope(window)
        focus = window.get_focus()
        scrolled = focus.get_ancestor(Gtk.ScrolledWindow) if focus is not None else None
        if window is self and scope is self and (scrolled is None or self.pad_in_sidebar(focus)):
            scrolled = self.pad_find(self.stack.get_visible_child(), Gtk.ScrolledWindow)
        elif scrolled is None:
            scrolled = self.pad_find(scope, Gtk.ScrolledWindow)
        if scrolled is None:
            return
        adjustment = scrolled.get_vadjustment()
        before = adjustment.get_value()
        adjustment.set_value(before + sign * (adjustment.get_page_size() * 0.9 if page else PAD_SCROLL_STEP))
        content = self.pad_content(scrolled)
        if adjustment.get_value() == before or content is None or focus is None or \
                not focus.is_ancestor(scrolled):
            return
        value, height = adjustment.get_value(), adjustment.get_page_size()

        def place(widget):
            ok, bounds = widget.compute_bounds(content)
            return (bounds.get_y(), bounds.get_y() + bounds.get_height()) if ok else None

        def in_sight(spot):
            return spot is not None and spot[0] >= value and spot[1] <= value + height

        targets = self.pad_targets(window, scope)
        current = self.pad_target_of(focus, targets, scope) or focus
        if in_sight(place(current)):
            return
        seen = [(place(t), t) for t in targets if t.is_ancestor(scrolled)]
        seen = [(spot, t) for spot, t in seen if in_sight(spot)]
        if seen:
            _spot, target = (min if sign > 0 else max)(seen, key=lambda item: item[0][0])
            self.pad_focus(window, self.pad_land(target, targets), show=False)

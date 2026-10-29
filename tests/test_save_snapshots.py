# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The save point (scripts/save_snapshots.py): backup, restore, undo, the game's lock, and what an
interrupted operation leaves behind."""
from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import contextlib
import io
import json
import os
import tempfile
import time
import unittest
from pathlib import Path

import save_snapshots as snap

try:
    import fcntl
except ImportError:
    fcntl = None

GAME = '1/CUSA03173/SPRJ0005'  # inside savedata/


def write_saves(user, text, extra=None):
    """<user>/savedata like the game's: the character file, the system file and the metadata."""
    files = {f'{GAME}/userdata0000': text, f'{GAME}/userdata0010': 'system ' + text,
             f'{GAME}.sce_sys/param.bin': 'param ' + text}
    files.update(extra or {})
    for name, content in files.items():
        path = Path(user) / 'savedata' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8')


def character(saves):
    """The character file in a savedata folder (the saves, or a copy's)."""
    return (Path(saves) / GAME / 'userdata0000').read_text(encoding='utf-8')


class SaveSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.user = Path(self.temp.name)
        self.quiet = contextlib.redirect_stderr(io.StringIO())
        self.quiet.__enter__()

    def tearDown(self):
        self.quiet.__exit__(None, None, None)
        self.temp.cleanup()

    def test_backup_without_saves_refuses(self):
        with self.assertRaises(snap.SnapshotError) as caught:
            snap.backup(self.user)
        self.assertEqual(caught.exception.code, 'no_saves')
        self.assertFalse((self.user / snap.SNAPSHOT).exists())

    def test_backup_copies_the_saves_and_skips_temporary_copies(self):
        write_saves(self.user, 'level 10', {f'{GAME}/userdata0000.1.bbtmp': 'half written'})
        meta = snap.backup(self.user, mods=['a mod'])
        copy = self.user / snap.SNAPSHOT / snap.SAVES
        self.assertEqual(character(copy), 'level 10')
        self.assertFalse((copy / GAME / 'userdata0000.1.bbtmp').exists())
        stored = json.loads((self.user / snap.SNAPSHOT / snap.META).read_text(encoding='utf-8'))
        self.assertEqual(stored, meta)
        self.assertEqual(stored['source'], 'launcher')
        self.assertEqual(stored['mods'], ['a mod'])
        self.assertLessEqual(abs(stored['created_unix'] - time.time()), 5)
        self.assertEqual(snap.info(self.user)['snapshot'], meta)

    def test_backup_replaces_the_save_point(self):
        write_saves(self.user, 'level 10')
        snap.backup(self.user)
        write_saves(self.user, 'level 20')
        snap.backup(self.user)
        self.assertEqual(character(self.user / snap.SNAPSHOT / snap.SAVES), 'level 20')
        self.assertEqual(sorted(p.name for p in self.user.iterdir()), sorted([snap.SAVES, snap.LOCK, snap.SNAPSHOT]))

    def test_restore_brings_the_save_point_back_and_keeps_an_undo_copy(self):
        write_saves(self.user, 'before the boss')
        snap.backup(self.user)
        write_saves(self.user, 'died, souls lost', {f'{GAME}/userdata0001': 'second character'})
        snap.restore(self.user)
        self.assertEqual(character(self.user / snap.SAVES), 'before the boss')
        # The restored folder is the save point's: files made since are gone too.
        self.assertFalse((self.user / snap.SAVES / GAME / 'userdata0001').exists())
        self.assertEqual(character(self.user / snap.UNDO / snap.SAVES), 'died, souls lost')
        self.assertEqual(snap.read_meta(self.user / snap.UNDO)['source'], 'before-restore')
        # The save point stays, for the next try.
        self.assertEqual(character(self.user / snap.SNAPSHOT / snap.SAVES), 'before the boss')
        write_saves(self.user, 'died again')
        snap.restore(self.user)
        self.assertEqual(character(self.user / snap.SAVES), 'before the boss')
        self.assertEqual(character(self.user / snap.UNDO / snap.SAVES), 'died again')

    def test_undo_takes_the_restore_back(self):
        write_saves(self.user, 'save point')
        snap.backup(self.user)
        write_saves(self.user, 'progress')
        snap.restore(self.user)
        snap.restore(self.user, undo=True)
        self.assertEqual(character(self.user / snap.SAVES), 'progress')
        # Undoing again swaps once more.
        self.assertEqual(character(self.user / snap.UNDO / snap.SAVES), 'save point')
        leftovers = [p.name for p in self.user.iterdir() if p.name.endswith(('.new', '.old', '.restoring', '.restored', '.replaced'))]
        self.assertEqual(leftovers, [])

    def test_restore_without_a_copy_refuses(self):
        write_saves(self.user, 'progress')
        for undo in (False, True):
            with self.assertRaises(snap.SnapshotError) as caught:
                snap.restore(self.user, undo=undo)
            self.assertEqual(caught.exception.code, 'none')
        self.assertEqual(character(self.user / snap.SAVES), 'progress')

    def test_restore_into_a_folder_without_saves(self):
        write_saves(self.user, 'save point')
        snap.backup(self.user)
        import shutil
        shutil.rmtree(self.user / snap.SAVES)
        snap.restore(self.user)
        self.assertEqual(character(self.user / snap.SAVES), 'save point')
        self.assertIsNone(snap.read_meta(self.user / snap.UNDO))

    @unittest.skipIf(fcntl is None, 'no flock here')
    def test_a_running_game_blocks_backup_and_restore(self):
        write_saves(self.user, 'progress')
        snap.backup(self.user)
        fd = os.open(self.user / snap.LOCK, os.O_RDWR | os.O_CREAT, 0o644)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)  # what the game does at start
            self.assertTrue(snap.game_running(self.user))
            for action in (lambda: snap.backup(self.user), lambda: snap.restore(self.user)):
                with self.assertRaises(snap.SnapshotError) as caught:
                    action()
                self.assertEqual(caught.exception.code, 'running')
        finally:
            os.close(fd)
        self.assertFalse(snap.game_running(self.user))

    def test_interrupted_backup_is_dropped_or_finished(self):
        write_saves(self.user, 'old point')
        snap.backup(self.user)
        # Killed while copying: no meta.json yet, the old save point stays.
        partial = self.user / (snap.SNAPSHOT + '.new')
        write_saves(partial, 'half copied')
        snap.recover(self.user)
        self.assertFalse(partial.exists())
        self.assertEqual(character(self.user / snap.SNAPSHOT / snap.SAVES), 'old point')
        # Killed after the copy was complete (meta.json written) but before the swap: finished.
        complete = self.user / (snap.SNAPSHOT + '.new')
        complete.mkdir()
        (complete / snap.SAVES).mkdir()
        write_saves(complete, 'new point')
        snap._write_meta(complete, 'game')
        snap.recover(self.user)
        self.assertEqual(character(self.user / snap.SNAPSHOT / snap.SAVES), 'new point')
        # Killed between the two renames of the swap: the old one comes back.
        os.rename(self.user / snap.SNAPSHOT, self.user / (snap.SNAPSHOT + '.old'))
        snap.recover(self.user)
        self.assertEqual(character(self.user / snap.SNAPSHOT / snap.SAVES), 'new point')

    def test_interrupted_restore_never_loses_the_saves(self):
        write_saves(self.user, 'progress')
        user = self.user
        # The saves moved aside, the restored copy complete: the restore is finished.
        write_saves(user / 'x', 'save point')
        os.rename(user / 'x' / snap.SAVES, user / (snap.SAVES + '.restored'))
        os.rename(user / snap.SAVES, user / (snap.SAVES + '.replaced'))
        snap.recover(user)
        self.assertEqual(character(user / snap.SAVES), 'save point')
        self.assertFalse((user / (snap.SAVES + '.replaced')).exists())
        # The saves moved aside and no restored copy: they come back.
        os.rename(user / snap.SAVES, user / (snap.SAVES + '.replaced'))
        snap.recover(user)
        self.assertEqual(character(user / snap.SAVES), 'save point')
        # A copy still being made, or complete while the saves were not moved yet: dropped.
        write_saves(user / 'y', 'other')
        os.rename(user / 'y' / snap.SAVES, user / (snap.SAVES + '.restoring'))
        write_saves(user / 'z', 'other')
        os.rename(user / 'z' / snap.SAVES, user / (snap.SAVES + '.restored'))
        snap.recover(user)
        self.assertEqual(character(user / snap.SAVES), 'save point')
        self.assertFalse((user / (snap.SAVES + '.restoring')).exists())
        self.assertFalse((user / (snap.SAVES + '.restored')).exists())

    def test_stale_queued_restore_is_dropped(self):
        pending = self.user / snap.PENDING
        now = int(time.time())
        pending.write_text(f'created_unix={now - 30}\n', encoding='utf-8')
        snap.recover(self.user, now=now)
        self.assertTrue(pending.exists())
        self.assertTrue(snap.info(self.user)['pending'])
        snap.recover(self.user, now=now + snap.PENDING_MAX_AGE)
        self.assertFalse(pending.exists())


if __name__ == '__main__':
    unittest.main()

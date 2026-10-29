# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The save point (save scumming): one copy of the game's saves, and the way back to it.

    <user>/savedata/                   the game's saves
    <user>/savedata-snapshot/          the save point: savedata/ (a copy) and meta.json
    <user>/savedata-snapshot.undo/     the saves as they were before the last restore
    <user>/savedata-snapshot.pending   a restore asked for in the game, applied at its next start
    <user>/.bbport-game.lock           locked by the running game

The launcher uses this module while the game is not running; the game does the same in C
(src/runtime_snapshot.c: the in-game menu, and the queued restore at start). Both use the same
names and the same steps: every copy is made under a temporary name and put in place by renames,
so a crash or a kill leaves the old state or the new one, and the next operation finishes or drops
what was left half done (recover()). Copies made by the game's file layer while it writes a save
(*.bbtmp) are never copied.

    python3 scripts/save_snapshots.py info|backup|restore|undo --user DIR
"""
import argparse
import contextlib
import json
import os
import shutil
import sys
import time
from pathlib import Path

try:
    import fcntl
except ImportError:  # Windows: no lock; the feature is offered by the Linux launcher only
    fcntl = None

SAVES = 'savedata'
SNAPSHOT = 'savedata-snapshot'
UNDO = 'savedata-snapshot.undo'
PENDING = 'savedata-snapshot.pending'
LOCK = '.bbport-game.lock'
META = 'meta.json'
# A restore queued in the game is applied at the next start within this many seconds (the game
# restarts at once); an older one (the restart failed, the player quit) is dropped.
PENDING_MAX_AGE = 600


class SnapshotError(Exception):
    """code: 'no_saves' (nothing to back up), 'none' (no save point or undo copy), 'running' (the
    game runs) or 'io' (a file operation failed; the saves are as they were)."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def _remove(path):
    path = Path(path)
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def _copy_file(source, target):
    shutil.copy2(source, target)
    with open(target, 'rb+') as f:
        os.fsync(f.fileno())


def _copy_tree(source, target):
    shutil.copytree(source, target, copy_function=_copy_file,
                    ignore=shutil.ignore_patterns('*.bbtmp'))


def _write_meta(slot, source, mods=None, now=None):
    now = int(time.time() if now is None else now)
    meta = {'v': 1, 'created_unix': now,
            'created': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(now)), 'source': source}
    if mods is not None:
        meta['mods'] = list(mods)
    part = Path(slot) / (META + '.part')
    with open(part, 'w', encoding='utf-8') as f:
        f.write(json.dumps(meta, ensure_ascii=False) + '\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(part, Path(slot) / META)
    return meta


def _complete(slot):
    slot = Path(slot)
    return (slot / META).is_file() and (slot / SAVES).is_dir()


def _swap_in(final, fresh):
    """fresh takes final's place; final, if any, is removed."""
    final, fresh = Path(final), Path(fresh)
    old = final.with_name(final.name + '.old')
    _remove(old)
    if final.exists():
        os.rename(final, old)
    try:
        os.rename(fresh, final)
    except OSError:
        if old.exists() and not final.exists():
            os.rename(old, final)
        raise
    _remove(old)


def _recover_slot(final):
    final = Path(final)
    fresh, old = final.with_name(final.name + '.new'), final.with_name(final.name + '.old')
    if fresh.exists():
        if _complete(fresh):
            _swap_in(final, fresh)
        else:
            _remove(fresh)
    if old.exists():
        if final.exists():
            _remove(old)
        else:
            os.rename(old, final)


def _pending_time(user):
    try:
        text = (Path(user) / PENDING).read_text(encoding='utf-8')
    except OSError:
        return None
    for line in text.splitlines():
        if line.startswith('created_unix='):
            try:
                return int(line.split('=', 1)[1])
            except ValueError:
                return 0
    return 0


def recover(user, now=None):
    """Finishes or drops what an interrupted operation left (as src/runtime_snapshot.c does)."""
    user = Path(user)
    saves = user / SAVES
    restoring, restored, replaced = (user / (SAVES + suffix) for suffix in ('.restoring', '.restored', '.replaced'))
    try:
        _remove(restoring)
        _recover_slot(user / SNAPSHOT)
        _recover_slot(user / UNDO)
        if not saves.exists():
            if restored.exists():
                os.rename(restored, saves)
            elif replaced.exists():
                os.rename(replaced, saves)
        if saves.exists():
            _remove(restored)
            _remove(replaced)
        queued = _pending_time(user)
        now = int(time.time() if now is None else now)
        if queued is not None and not -60 <= now - queued <= PENDING_MAX_AGE:
            _remove(user / PENDING)
    except OSError as error:
        raise SnapshotError('io', f'save point: {error}') from error


def read_meta(slot):
    """The slot's meta.json (a dict), or None when it holds no complete copy."""
    slot = Path(slot)
    if not _complete(slot):
        return None
    try:
        meta = json.loads((slot / META).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        meta = {}
    return meta if isinstance(meta, dict) else {}


def has_saves(user):
    saves = Path(user) / SAVES
    return saves.is_dir() and any(saves.iterdir())


def info(user):
    """{'saves': bool, 'snapshot': meta or None, 'undo': meta or None, 'pending': bool}"""
    user = Path(user)
    return {'saves': has_saves(user), 'snapshot': read_meta(user / SNAPSHOT),
            'undo': read_meta(user / UNDO), 'pending': (user / PENDING).is_file()}


@contextlib.contextmanager
def game_lock(user):
    """Held while the launcher changes the saves: refuses when the game runs (it locks the file
    for as long as it runs, src/runtime_savedata.c)."""
    if fcntl is None:
        yield
        return
    user = Path(user)
    user.mkdir(parents=True, exist_ok=True)
    fd = os.open(user / LOCK, os.O_RDWR | os.O_CREAT | getattr(os, 'O_CLOEXEC', 0), 0o644)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise SnapshotError('running', 'the game is running') from error
        yield
    finally:
        os.close(fd)


def game_running(user):
    if not (Path(user) / LOCK).exists():
        return False  # no game has run with this folder (nothing is created here)
    try:
        with game_lock(user):
            return False
    except SnapshotError:
        return True


def backup(user, source='launcher', mods=None):
    """Copies the saves to the save point (replacing the old one); returns its meta."""
    user = Path(user)
    with game_lock(user):
        recover(user)
        if not has_saves(user):
            raise SnapshotError('no_saves', 'no saves to back up yet')
        fresh = user / (SNAPSHOT + '.new')
        try:
            _remove(fresh)
            fresh.mkdir()
            _copy_tree(user / SAVES, fresh / SAVES)
            meta = _write_meta(fresh, source, mods)
            _swap_in(user / SNAPSHOT, fresh)
        except OSError as error:
            with contextlib.suppress(OSError):
                _remove(fresh)
            raise SnapshotError('io', f'save point not made: {error}') from error
        print(f'Save point: the saves copied to {user / SNAPSHOT}', file=sys.stderr)
        return meta


def restore(user, undo=False):
    """Puts the save point (undo=True: the undo copy) in place of the saves; the saves as they
    were become the undo copy. Returns the restored copy's meta."""
    user = Path(user)
    with game_lock(user):
        recover(user)
        source = user / (UNDO if undo else SNAPSHOT)
        meta = read_meta(source)
        if meta is None:
            raise SnapshotError('none', 'no undo copy' if undo else 'no save point yet')
        saves = user / SAVES
        restoring, restored, replaced = (user / (SAVES + suffix) for suffix in ('.restoring', '.restored', '.replaced'))
        fresh_undo = user / (UNDO + '.new')
        try:
            _copy_tree(source / SAVES, restoring)
            os.rename(restoring, restored)
            if saves.exists():
                _remove(fresh_undo)
                fresh_undo.mkdir()
                _copy_tree(saves, fresh_undo / SAVES)
                _write_meta(fresh_undo, 'before-restore')
                _swap_in(user / UNDO, fresh_undo)
                os.rename(saves, replaced)
            try:
                os.rename(restored, saves)
            except OSError:
                if replaced.exists() and not saves.exists():
                    os.rename(replaced, saves)
                raise
            _remove(replaced)
        except OSError as error:
            with contextlib.suppress(OSError, SnapshotError):
                _remove(restoring)
                _remove(fresh_undo)
                if saves.exists():  # else recover() finishes the restore (the saves were moved aside)
                    _remove(restored)
                recover(user)
            raise SnapshotError('io', f'saves not restored: {error}') from error
        print(f'Save point: the saves restored from {source}', file=sys.stderr)
        return meta


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action', choices=('info', 'backup', 'restore', 'undo'))
    parser.add_argument('--user', required=True, type=Path, help='the user folder (holds savedata/)')
    args = parser.parse_args()
    try:
        if args.action == 'info':
            print(json.dumps(info(args.user), ensure_ascii=False, indent=2))
        elif args.action == 'backup':
            backup(args.user, 'cli')
        else:
            restore(args.user, undo=args.action == 'undo')
    except SnapshotError as error:
        print(f'Save point: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

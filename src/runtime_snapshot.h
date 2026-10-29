// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* The save point (save scumming, the launcher's Mods and patches switch): one copy of the game's
 * saves, and the way back to it. Folders in the user folder:
 *   savedata/                   the game's saves
 *   savedata-snapshot/          the save point: savedata/ (a copy) and meta.json
 *   savedata-snapshot.undo/     the saves as they were before the last restore
 *   savedata-snapshot.pending   a restore asked for in the game, applied at its next start
 * scripts/save_snapshots.py does the same for the launcher, with the same names and steps.
 * Plain file operations only (no runtime state): tests/test_save_snapshot.c builds it alone. */
#ifndef BB_RUNTIME_SNAPSHOT_H
#define BB_RUNTIME_SNAPSHOT_H
#include <stdint.h>

enum {
    SNAPSHOT_OK = 0,
    SNAPSHOT_BUSY = -1,     /* the game changed its saves during the copy: nothing replaced */
    SNAPSHOT_NO_SAVES = -2, /* no saves to back up yet */
    SNAPSHOT_NONE = -3,     /* no save point (or undo copy) to restore */
    SNAPSHOT_IO = -4,       /* a file operation failed; the saves and the copies are as they were */
    SNAPSHOT_STALE = -5,    /* a queued restore older than its limit: dropped, nothing restored */
};

/* A restore queued in the game is applied at the next start within this many seconds. */
#define SNAPSHOT_PENDING_MAX_AGE 600

/* Asked after the copy is made and before it replaces the save point: 0 drops it (SNAPSHOT_BUSY). */
typedef int (*SnapshotCheck)(void *context);

/* Copies <user>/savedata to the save point, replacing it. `source` goes to meta.json ("game"). */
int snapshot_backup(const char *user, const char *source, SnapshotCheck still_valid, void *context);
/* Puts the save point (undo: the undo copy) in place of the saves; the saves become the undo copy. */
int snapshot_restore(const char *user, int undo);
/* When the save point (undo: the undo copy) was made (Unix time), 0 when there is none. */
int64_t snapshot_created(const char *user, int undo);
/* Queues a restore of the save point for the next start (SNAPSHOT_NONE without one). */
int snapshot_queue_restore(const char *user, int64_t now);
/* At start: applies a queued restore made at most max_age seconds before `now`. 1 when it restored,
 * 0 when none was queued, else an error (the marker is removed either way). */
int snapshot_apply_pending(const char *user, int64_t now, int64_t max_age);
/* Finishes or drops what an interrupted operation left. */
int snapshot_recover(const char *user);
/* The error's text for the log. */
const char *snapshot_error(int code);
#endif

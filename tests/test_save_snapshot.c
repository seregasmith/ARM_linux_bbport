// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Standalone test for the save point (src/runtime_snapshot.c only): backup, restore, undo, the
 * restore queued in the game, and what an interrupted operation leaves.
 * Usage: save-snapshot-test [temp_dir]   (default: out/save_snapshot_test_tmp) */
#define _GNU_SOURCE
#include "runtime_snapshot.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GAME "savedata/1/CUSA03173/SPRJ0005"
static char user[1024];

static void path_of(char *out, const char *name) { snprintf(out, 2048, "%s/%s", user, name); }
static void make_parents(const char *path) {
    char buffer[2048];
    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char *p = buffer + 1; *p; ++p) if (*p == '/') { *p = 0; mkdir(buffer, 0755); *p = '/'; }
}
static void put(const char *name, const char *text) {
    char path[2048];
    path_of(path, name);
    make_parents(path);
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}
/* The file's text ("" when it is missing). */
static const char *get(const char *name) {
    static char text[256];
    char path[2048];
    path_of(path, name);
    text[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return text;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = 0;
    fclose(f);
    return text;
}
static int there(const char *name) { char path[2048]; struct stat s; path_of(path, name); return !lstat(path, &s); }
static void saves(const char *text) {
    char buffer[128];
    put(GAME "/userdata0000", text);
    snprintf(buffer, sizeof(buffer), "system %s", text);
    put(GAME "/userdata0010", buffer);
    put("savedata/1/CUSA03173/SPRJ0005.sce_sys/param.bin", "param");
}
static void wipe(const char *path) {
    char command[2200];
    snprintf(command, sizeof(command), "rm -rf '%s'", path);
    assert(system(command) == 0);
}
static void move(const char *from, const char *to) {
    char a[2048], b[2048];
    path_of(a, from);
    path_of(b, to);
    assert(rename(a, b) == 0);
}
static int changed_during_copy(void *context) { (void)context; return 0; }
static int unchanged(void *context) { ++*(int *)context; return 1; }

int main(int argc, char **argv) {
    snprintf(user, sizeof(user), "%s", argc > 1 ? argv[1] : "out/save_snapshot_test_tmp");
    wipe(user);
    make_parents(user);
    assert(mkdir(user, 0755) == 0);

    /* Nothing to back up or restore yet. */
    assert(snapshot_backup(user, "game", NULL, NULL) == SNAPSHOT_NO_SAVES);
    assert(snapshot_restore(user, 0) == SNAPSHOT_NONE);
    assert(snapshot_queue_restore(user, time(NULL)) == SNAPSHOT_NONE);
    assert(snapshot_created(user, 0) == 0);

    /* Backup: the copy and its meta.json; the file layer's temporary copies are left out; the
     * files keep their times (the game's load menu shows them). */
    saves("before the boss");
    put(GAME "/userdata0000.3.bbtmp", "half written");
    char param[2048];
    path_of(param, "savedata/1/CUSA03173/SPRJ0005.sce_sys/param.bin");
    struct timespec old_times[2] = {{1700000000, 0}, {1700000000, 0}};
    assert(utimensat(AT_FDCWD, param, old_times, 0) == 0);
    int checks = 0;
    assert(snapshot_backup(user, "game", unchanged, &checks) == SNAPSHOT_OK && checks == 1);
    assert(!strcmp(get("savedata-snapshot/" GAME "/userdata0000"), "before the boss"));
    assert(!there("savedata-snapshot/" GAME "/userdata0000.3.bbtmp"));
    assert(strstr(get("savedata-snapshot/meta.json"), "\"source\": \"game\""));
    int64_t created = snapshot_created(user, 0);
    assert(created > 0 && llabs((long long)(created - time(NULL))) < 5);
    struct stat copied;
    path_of(param, "savedata-snapshot/savedata/1/CUSA03173/SPRJ0005.sce_sys/param.bin");
    assert(!stat(param, &copied) && copied.st_mtim.tv_sec == 1700000000);

    /* Saves changed during the copy: nothing replaced, nothing left. */
    saves("changed");
    assert(snapshot_backup(user, "game", changed_during_copy, NULL) == SNAPSHOT_BUSY);
    assert(!strcmp(get("savedata-snapshot/" GAME "/userdata0000"), "before the boss"));
    assert(!there("savedata-snapshot.new"));

    /* Restore: the save point's folder as it was, the saves before it as the undo copy. */
    saves("died, souls lost");
    put(GAME "/userdata0001", "second character");
    assert(snapshot_restore(user, 0) == SNAPSHOT_OK);
    assert(!strcmp(get(GAME "/userdata0000"), "before the boss"));
    assert(!there(GAME "/userdata0001"));
    assert(!strcmp(get("savedata-snapshot.undo/" GAME "/userdata0000"), "died, souls lost"));
    assert(strstr(get("savedata-snapshot.undo/meta.json"), "before-restore"));
    assert(snapshot_created(user, 1) > 0);
    assert(!strcmp(get("savedata-snapshot/" GAME "/userdata0000"), "before the boss"));
    /* Undo, and undo the undo. */
    assert(snapshot_restore(user, 1) == SNAPSHOT_OK);
    assert(!strcmp(get(GAME "/userdata0000"), "died, souls lost"));
    assert(!strcmp(get("savedata-snapshot.undo/" GAME "/userdata0000"), "before the boss"));
    assert(snapshot_restore(user, 1) == SNAPSHOT_OK);
    assert(!strcmp(get(GAME "/userdata0000"), "before the boss"));

    /* A restore queued in the game: applied at the next start while fresh, dropped once stale. */
    saves("progress");
    int64_t now = time(NULL);
    assert(snapshot_apply_pending(user, now, SNAPSHOT_PENDING_MAX_AGE) == 0);
    assert(snapshot_queue_restore(user, now) == SNAPSHOT_OK && there("savedata-snapshot.pending"));
    assert(snapshot_apply_pending(user, now + 5, SNAPSHOT_PENDING_MAX_AGE) == 1);
    assert(!there("savedata-snapshot.pending"));
    assert(!strcmp(get(GAME "/userdata0000"), "before the boss"));
    saves("progress");
    assert(snapshot_queue_restore(user, now) == SNAPSHOT_OK);
    assert(snapshot_apply_pending(user, now + SNAPSHOT_PENDING_MAX_AGE + 1, SNAPSHOT_PENDING_MAX_AGE) == SNAPSHOT_STALE);
    assert(!there("savedata-snapshot.pending"));
    assert(!strcmp(get(GAME "/userdata0000"), "progress"));

    /* Interrupted backups: an incomplete copy is dropped, a complete one finished, a slot moved
     * aside comes back. */
    put("savedata-snapshot.new/" GAME "/userdata0000", "half copied");
    assert(snapshot_recover(user) == SNAPSHOT_OK);
    assert(!there("savedata-snapshot.new"));
    assert(!strcmp(get("savedata-snapshot/" GAME "/userdata0000"), "before the boss"));
    put("savedata-snapshot.new/" GAME "/userdata0000", "new point");
    put("savedata-snapshot.new/meta.json", "{\"v\": 1, \"created_unix\": 1800000000, \"created\": \"\", \"source\": \"launcher\"}\n");
    assert(snapshot_recover(user) == SNAPSHOT_OK);
    assert(!strcmp(get("savedata-snapshot/" GAME "/userdata0000"), "new point"));
    assert(snapshot_created(user, 0) == 1800000000);
    move("savedata-snapshot", "savedata-snapshot.old");
    assert(snapshot_recover(user) == SNAPSHOT_OK);
    assert(!strcmp(get("savedata-snapshot/" GAME "/userdata0000"), "new point"));

    /* Interrupted restores never lose the saves. */
    put("savedata.restored/1/CUSA03173/SPRJ0005/userdata0000", "restored");
    move("savedata", "savedata.replaced");
    assert(snapshot_recover(user) == SNAPSHOT_OK);
    assert(!strcmp(get(GAME "/userdata0000"), "restored") && !there("savedata.replaced"));
    move("savedata", "savedata.replaced");
    assert(snapshot_recover(user) == SNAPSHOT_OK);
    assert(!strcmp(get(GAME "/userdata0000"), "restored"));
    put("savedata.restoring/1/CUSA03173/SPRJ0005/userdata0000", "partial");
    put("savedata.restored/1/CUSA03173/SPRJ0005/userdata0000", "not moved in");
    assert(snapshot_recover(user) == SNAPSHOT_OK);
    assert(!there("savedata.restoring") && !there("savedata.restored"));
    assert(!strcmp(get(GAME "/userdata0000"), "restored"));

    /* A save point made by scripts/save_snapshots.py is read the same. */
    put("savedata-snapshot/meta.json", "{\"v\": 1, \"created_unix\": 1793300000, \"created\": \"2026-10-29T15:33:20Z\", \"source\": \"launcher\", \"mods\": []}\n");
    assert(snapshot_created(user, 0) == 1793300000);

    wipe(user);
    puts("save point test: ok");
    return 0;
}

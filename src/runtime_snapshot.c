// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* The save point (runtime_snapshot.h). Every copy is made under a temporary name and put in place
 * by renames, so a crash or a kill leaves the old state or the new one; snapshot_recover() (run
 * first by every operation, and by scripts/save_snapshots.py's recover()) finishes or drops what
 * was left:
 *   <slot>.new          a copy being made (slot: savedata-snapshot, savedata-snapshot.undo):
 *                       complete once its meta.json exists (written last), then it takes the
 *                       slot's place; an incomplete one is dropped
 *   <slot>.old          the slot being replaced: back if the slot is gone, else dropped
 *   savedata.restoring  a restored copy being made: dropped
 *   savedata.restored   the restored copy, complete: takes savedata's place once savedata was
 *                       moved aside, dropped if it was not (the restore did not happen)
 *   savedata.replaced   the saves moved aside: back if savedata is gone, else dropped
 * The copies the game's file layer writes a save into (*.bbtmp, runtime_file.c) are never copied. */
#define _GNU_SOURCE
#include "runtime_snapshot.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif
#define PATH_SIZE 1024
#define META "meta.json"

typedef struct {
    char saves[PATH_SIZE], snapshot[PATH_SIZE], undo[PATH_SIZE], pending[PATH_SIZE];
    char restoring[PATH_SIZE], restored[PATH_SIZE], replaced[PATH_SIZE];
} Paths;

static int join(char *out, const char *dir, const char *name) {
    return (size_t)snprintf(out, PATH_SIZE, "%s/%s", dir, name) >= PATH_SIZE ? -1 : 0;
}
static int suffixed(char *out, const char *path, const char *suffix) {
    return (size_t)snprintf(out, PATH_SIZE, "%s%s", path, suffix) >= PATH_SIZE ? -1 : 0;
}
static int paths(const char *user, Paths *p) {
    if (!user || !*user) return -1;
    return join(p->saves, user, "savedata") || join(p->snapshot, user, "savedata-snapshot") ||
           join(p->undo, user, "savedata-snapshot.undo") || join(p->pending, user, "savedata-snapshot.pending") ||
           suffixed(p->restoring, p->saves, ".restoring") || suffixed(p->restored, p->saves, ".restored") ||
           suffixed(p->replaced, p->saves, ".replaced") ? -1 : 0;
}
static int exists(const char *path) { struct stat s; return !lstat(path, &s); }
static int is_dir(const char *path) { struct stat s; return !stat(path, &s) && S_ISDIR(s.st_mode); }
static int dot(const char *name) { return !strcmp(name, ".") || !strcmp(name, ".."); }
static int temporary(const char *name) {
    size_t n = strlen(name);
    return n > 6 && !strcmp(name + n - 6, ".bbtmp");
}
static int has_entries(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));) found = !dot(e->d_name);
    closedir(d);
    return found;
}
/* A slot (or its .new) holding a complete copy: savedata/ and meta.json. */
static int complete(const char *slot) {
    char meta[PATH_SIZE], saves[PATH_SIZE];
    return !join(meta, slot, META) && !join(saves, slot, "savedata") && exists(meta) && is_dir(saves);
}
static void sync_dir(const char *path) {
#ifndef _WIN32
    int d = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (d >= 0) { fsync(d); close(d); }
#else
    (void)path;
#endif
}

/* Removes a file or a folder with all it holds (symbolic links themselves, not what they name). */
static int remove_tree(const char *path) {
    struct stat s;
    if (lstat(path, &s)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(s.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    int r = 0;
    for (struct dirent *e; (e = readdir(d));) {
        char child[PATH_SIZE];
        if (dot(e->d_name)) continue;
        if (join(child, path, e->d_name) || remove_tree(child)) r = -1;
    }
    closedir(d);
    return r || rmdir(path) ? -1 : 0;
}

static int copy_file(const char *from, const char *to, const struct stat *s) {
    int in = open(from, O_RDONLY | O_BINARY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_BINARY | O_CLOEXEC, (int)(s->st_mode & 0777) | 0600);
    if (out < 0) { close(in); return -1; }
    char buffer[65536];
    int r = 0;
    while (!r) {
        ssize_t n = read(in, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { r = n < 0 ? -1 : 0; break; }
        for (ssize_t done = 0; done < n && !r;) {
            ssize_t w = write(out, buffer + done, (size_t)(n - done));
            if (w < 0 && errno != EINTR) r = -1;
            else if (w > 0) done += w;
        }
    }
#ifndef _WIN32
    if (!r) { /* the game's load menu shows a save's time from its files */
        struct timespec times[2] = {s->st_atim, s->st_mtim};
        futimens(out, times);
    }
#endif
    if (!r && fsync(out)) r = -1;
    if (close(out)) r = -1;
    close(in);
    return r;
}

/* Copies a folder to `to` (which must not exist), leaving out the file layer's temporary copies. */
static int copy_tree(const char *from, const char *to) {
    if (mkdir(to, 0755)) return -1;
    DIR *d = opendir(from);
    if (!d) return -1;
    int r = 0;
    for (struct dirent *e; !r && (e = readdir(d));) {
        char a[PATH_SIZE], b[PATH_SIZE];
        struct stat s;
        if (dot(e->d_name) || temporary(e->d_name)) continue;
        if (join(a, from, e->d_name) || join(b, to, e->d_name)) { r = -1; break; }
        if (stat(a, &s)) { r = -1; break; }
        if (S_ISDIR(s.st_mode)) r = copy_tree(a, b);
        else if (S_ISREG(s.st_mode)) r = copy_file(a, b, &s);
    }
    closedir(d);
    if (!r) sync_dir(to);
    return r;
}

/* meta.json, written last into a copy being made (it marks the copy complete). The same fields as
 * scripts/save_snapshots.py writes. */
static int write_meta(const char *slot, const char *source, int64_t now) {
    char path[PATH_SIZE], part[PATH_SIZE], stamp[32] = "";
    if (join(path, slot, META) || join(part, slot, META ".part")) return -1;
    time_t t = (time_t)now;
    struct tm tm;
#ifdef _WIN32
    if (!gmtime_s(&tm, &t)) strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
#else
    if (gmtime_r(&t, &tm)) strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
#endif
    FILE *f = fopen(part, "wb");
    if (!f) return -1;
    int ok = fprintf(f, "{\"v\": 1, \"created_unix\": %" PRId64 ", \"created\": \"%s\", \"source\": \"%s\"}\n",
                     now, stamp, source) > 0;
    ok = !fflush(f) && !fsync(fileno(f)) && ok;
    ok = !fclose(f) && ok;
    if (!ok || rename(part, path)) { unlink(part); return -1; }
    return 0;
}
static int64_t read_created(const char *slot) {
    char path[PATH_SIZE], text[4096];
    if (!complete(slot) || join(path, slot, META)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    const char *p = strstr(text, "\"created_unix\"");
    if (!p || !(p = strchr(p, ':'))) return 0;
    long long value = strtoll(p + 1, NULL, 10);
    return value > 0 ? (int64_t)value : 0;
}

/* `fresh` takes `final`'s place; the old `final`, if any, is removed. */
static int swap_in(const char *final, const char *fresh) {
    char old[PATH_SIZE];
    if (suffixed(old, final, ".old") || remove_tree(old)) return -1;
    if (exists(final) && rename(final, old)) return -1;
    if (rename(fresh, final)) {
        if (exists(old) && !exists(final)) rename(old, final);
        return -1;
    }
    remove_tree(old);
    return 0;
}
static int recover_slot(const char *final) {
    char fresh[PATH_SIZE], old[PATH_SIZE];
    if (suffixed(fresh, final, ".new") || suffixed(old, final, ".old")) return -1;
    int r = 0;
    if (exists(fresh)) r |= complete(fresh) ? swap_in(final, fresh) : remove_tree(fresh);
    if (exists(old)) r |= exists(final) ? remove_tree(old) : rename(old, final);
    return r ? -1 : 0;
}

int snapshot_recover(const char *user) {
    Paths p;
    if (paths(user, &p)) return SNAPSHOT_IO;
    int r = remove_tree(p.restoring);
    r |= recover_slot(p.snapshot);
    r |= recover_slot(p.undo);
    if (!exists(p.saves)) {
        if (exists(p.restored)) r |= rename(p.restored, p.saves);
        else if (exists(p.replaced)) r |= rename(p.replaced, p.saves);
    }
    if (exists(p.saves)) {
        r |= remove_tree(p.restored);
        r |= remove_tree(p.replaced);
    }
    return r ? SNAPSHOT_IO : SNAPSHOT_OK;
}

int snapshot_backup(const char *user, const char *source, SnapshotCheck still_valid, void *context) {
    Paths p;
    char fresh[PATH_SIZE], copy[PATH_SIZE];
    if (paths(user, &p) || suffixed(fresh, p.snapshot, ".new") || join(copy, fresh, "savedata")) return SNAPSHOT_IO;
    if (snapshot_recover(user)) return SNAPSHOT_IO;
    if (!is_dir(p.saves) || !has_entries(p.saves)) return SNAPSHOT_NO_SAVES;
    if (remove_tree(fresh) || mkdir(fresh, 0755) || copy_tree(p.saves, copy)) {
        remove_tree(fresh);
        return SNAPSHOT_IO;
    }
    if (still_valid && !still_valid(context)) {
        remove_tree(fresh);
        return SNAPSHOT_BUSY;
    }
    if (write_meta(fresh, source ? source : "game", (int64_t)time(NULL)) || swap_in(p.snapshot, fresh)) {
        remove_tree(fresh);
        return SNAPSHOT_IO;
    }
    sync_dir(user);
    return SNAPSHOT_OK;
}

int snapshot_restore(const char *user, int undo) {
    Paths p;
    char source[PATH_SIZE], fresh_undo[PATH_SIZE], undo_copy[PATH_SIZE];
    if (paths(user, &p) || join(source, undo ? p.undo : p.snapshot, "savedata") ||
        suffixed(fresh_undo, p.undo, ".new") || join(undo_copy, fresh_undo, "savedata")) return SNAPSHOT_IO;
    if (snapshot_recover(user)) return SNAPSHOT_IO;
    if (!complete(undo ? p.undo : p.snapshot)) return SNAPSHOT_NONE;
    /* The copy to restore, complete before anything else changes. */
    if (copy_tree(source, p.restoring) || rename(p.restoring, p.restored)) goto failed;
    if (exists(p.saves)) {
        /* The saves as they are become the undo copy (after the copy above: restoring the undo
         * copy swaps the two). */
        if (remove_tree(fresh_undo) || mkdir(fresh_undo, 0755) || copy_tree(p.saves, undo_copy) ||
            write_meta(fresh_undo, "before-restore", (int64_t)time(NULL)) || swap_in(p.undo, fresh_undo))
            goto failed;
        if (rename(p.saves, p.replaced)) goto failed;
    }
    if (rename(p.restored, p.saves)) {
        if (!exists(p.saves) && exists(p.replaced)) rename(p.replaced, p.saves);
        goto failed;
    }
    remove_tree(p.replaced);
    sync_dir(user);
    return SNAPSHOT_OK;
failed:
    remove_tree(p.restoring);
    remove_tree(fresh_undo);
    if (exists(p.saves)) remove_tree(p.restored);
    snapshot_recover(user);
    return SNAPSHOT_IO;
}

int64_t snapshot_created(const char *user, int undo) {
    Paths p;
    return paths(user, &p) ? 0 : read_created(undo ? p.undo : p.snapshot);
}

int snapshot_queue_restore(const char *user, int64_t now) {
    Paths p;
    char part[PATH_SIZE];
    if (paths(user, &p) || suffixed(part, p.pending, ".part")) return SNAPSHOT_IO;
    if (snapshot_recover(user)) return SNAPSHOT_IO;
    if (!complete(p.snapshot)) return SNAPSHOT_NONE;
    FILE *f = fopen(part, "wb");
    if (!f) return SNAPSHOT_IO;
    int ok = fprintf(f, "created_unix=%" PRId64 "\n", now) > 0;
    ok = !fflush(f) && !fsync(fileno(f)) && ok;
    ok = !fclose(f) && ok;
    unlink(p.pending);
    if (!ok || rename(part, p.pending)) { unlink(part); return SNAPSHOT_IO; }
    sync_dir(user);
    return SNAPSHOT_OK;
}

int snapshot_apply_pending(const char *user, int64_t now, int64_t max_age) {
    Paths p;
    if (paths(user, &p)) return SNAPSHOT_IO;
    FILE *f = fopen(p.pending, "rb");
    if (!f) return 0;
    char line[64] = "";
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    long long queued = 0;
    if (sscanf(line, "created_unix=%lld", &queued) != 1) queued = 0;
    unlink(p.pending); /* once: a restore that fails is not tried at every start */
    if (queued <= 0 || now - queued > max_age || queued - now > 60) return SNAPSHOT_STALE;
    int r = snapshot_restore(user, 0);
    return r ? r : 1;
}

const char *snapshot_error(int code) {
    switch (code) {
    case SNAPSHOT_OK: return "done";
    case SNAPSHOT_BUSY: return "the game kept changing its saves";
    case SNAPSHOT_NO_SAVES: return "no saves yet";
    case SNAPSHOT_NONE: return "no save point";
    case SNAPSHOT_IO: return "a file operation failed (the saves are as they were)";
    case SNAPSHOT_STALE: return "queued too long ago";
    }
    return "unknown";
}

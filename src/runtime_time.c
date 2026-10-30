// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Explicit guest timeval layout; host pointers never escape this boundary. */
#define _DEFAULT_SOURCE
#include "runtime.h"
#include "guest_cpu.h"
#include "../gpu/shim/bb_env.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
typedef struct { int64_t seconds, microseconds; } GuestTimeval;
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;

/* bbport: Bloodborne 1.09's frame pacer (+0x2034770, gpu/shim/bbport_frame_rate.cpp) waits out each frame's time by
 * calling gettimeofday in a loop (+0x2034af0..+0x2034b75). Its other loop, which sleeps until 5 ms before the deadline, is
 * not taken in the release build (the switch, +0xfe3980, returns 1), so at a frame cap (30 FPS, the launcher's FPS limit)
 * the main thread spins for the rest of every frame: about a quarter of a core at 30 FPS on a handheld, under FEXCore and
 * the native CPU alike (profiles of 2026-10-30). Called from that loop, gettimeofday sleeps until 1 ms before the pacer's
 * deadline, which it takes from the pacer's own numbers: the frame's time target in seconds at [rbp - 0x44], 4 bytes below
 * the timeval the loop passes ([rbp - 0x40]), and the frame's start, which the pacer copies from [pacer + 0x28] on entry:
 * the time its previous gettimeofday call got. The game's loop then spins the last BB_PACER_MARGIN_US (8 ms), as before,
 * and as long again as the sleeps woke late recently (the worst lateness, falling slowly): a coarse timer (a virtual
 * machine's: up to 5 ms) costs spinning, not late frames. BB_PACER_SLEEP=0: the game's spin. With BB_FRAME_STATS, every
 * 10 s: how late the sleeps woke and how often the pacer let a frame go after its deadline.
 * Handheld, 2026-10-30, 30 FPS (late frames: 50 ms ones per minute after loading): spinning 235 % CPU, 3 late; 1 ms margin
 * 160 %, 30 late; 2 ms 192 %, 29 late; 4 ms 177 %, 14 late. The sleeps woke on time (15-30 us late on average) and no
 * frame went past the pacer's deadline: the late frames come from what the main thread meets after a long sleep (its
 * core used by other threads, colder caches, or another core), so a longer spin before the deadline keeps them down. */
enum { PACER_BEGIN = 0x203497c, PACER_SPIN = 0x2034b08, PACER_SLEEPING = 0x2034bea };

/* The pacer is the code this was written for (another build of the game: no sleeping). */
static int pacer_code_ok(void) {
    static const struct { uint32_t at; uint8_t n; uint8_t bytes[11]; } expect[] = {
        {0x2034971, 11, {0x48, 0x8d, 0x7d, 0xc0, 0x31, 0xf6, 0xe8, 0xac, 0x9d, 0xb8, 0x00}}, /* lea rdi, [rbp - 0x40]; call gettimeofday */
        {0x2034ac6, 5, {0xc5, 0xfa, 0x11, 0x5d, 0xbc}},                                      /* vmovss [rbp - 0x44], xmm3: the target */
        {0x2034aea, 4, {0x4c, 0x8d, 0x75, 0xc0}},                                            /* lea r14, [rbp - 0x40] */
        {0x2034afe, 10, {0x31, 0xf6, 0x4c, 0x89, 0xf7, 0xe8, 0x20, 0x9c, 0xb8, 0x00}},       /* the loop's call */
    };
    for (size_t i = 0; i < sizeof(expect) / sizeof(*expect); ++i)
        if (memcmp((const void *)(runtime_image_base + expect[i].at), expect[i].bytes, expect[i].n)) return 0;
    return 1;
}

/* A gettimeofday of the pacer (`caller`: its return offset): sleeps in its wait loop; `now` is the time to return. */
static void pacer_time(const GuestTimeval *value, uintptr_t caller, struct timeval *now) {
    static int enabled = -1, stats;
    static int64_t last_us, start_us, late_us, margin_us = 8000;
    /* BB_FRAME_STATS: per 10 s, sleeps, their lateness (sum, worst), frames let go after the deadline, the time slept. */
    static int64_t report_us, sleeps, late_sum, late_worst, past, past_worst, slept_us;
    if (enabled < 0) {
        enabled = !bb_env_off("BB_PACER_SLEEP") && pacer_code_ok();
        const char *margin = bb_env("BB_PACER_MARGIN_US");
        if (margin && atoi(margin) >= 0) margin_us = atoi(margin) > 20000 ? 20000 : atoi(margin);
        stats = bb_env("BB_FRAME_STATS") != NULL;
        if (enabled)
            printf("Runtime: the frame pacer sleeps until %lld us before each frame's deadline instead of spinning "
                   "(BB_PACER_SLEEP=0: spins; BB_PACER_MARGIN_US)\n", (long long)margin_us);
        else
            printf("Runtime: the frame pacer spins as the game has it (BB_PACER_SLEEP=0, or another build)\n");
    }
    int64_t now_us = (int64_t)now->tv_sec * 1000000 + now->tv_usec;
    if (caller == PACER_BEGIN) {
        start_us = last_us; /* [pacer + 0x20] = [pacer + 0x28]: the time of the pacer's previous call */
    } else if (caller == PACER_SPIN && enabled && start_us && value) {
#ifndef _WIN32
        float target;
        memcpy(&target, (const unsigned char *)value - 4, sizeof(target)); /* [rbp - 0x44] */
        if (target > 0.0f && target < 1.0f) {
            const int64_t deadline = start_us + (int64_t)(target * 1e6f);
            const int64_t wake = deadline - margin_us - late_us;
            if (wake > now_us + 100 && wake - now_us < 1000000) {
                const struct timespec at = {(time_t)(wake / 1000000), (long)(wake % 1000000) * 1000};
                const int64_t before = now_us;
                while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &at, NULL) == EINTR) {}
                gettimeofday(now, NULL);
                now_us = (int64_t)now->tv_sec * 1000000 + now->tv_usec;
                /* How late it woke: the worst at once, then down by 1/64 of the difference per frame (2 s at 30 FPS
                 * to fall by a third). */
                const int64_t late = now_us - wake < 0 ? 0 : (now_us - wake > 20000 ? 20000 : now_us - wake);
                late_us = late > late_us ? late : late_us - (late_us - late) / 64;
                ++sleeps;
                late_sum += late;
                if (late > late_worst) late_worst = late;
                slept_us += now_us - before;
                if (now_us > deadline) {
                    ++past;
                    if (now_us - deadline > past_worst) past_worst = now_us - deadline;
                }
            }
        }
        if (stats && now_us - report_us >= 10000000) {
            if (report_us && sleeps)
                printf("Runtime: frame pacer (10 s): %lld sleeps, %.1f ms each, woke %lld us late on average and %lld at "
                       "worst; %lld past the frame's deadline (at worst %lld us); margin now %lld us\n",
                       (long long)sleeps, slept_us / 1000.0 / sleeps, (long long)(late_sum / sleeps),
                       (long long)late_worst, (long long)past, (long long)past_worst, (long long)(margin_us + late_us));
            report_us = now_us;
            sleeps = late_sum = late_worst = past = past_worst = slept_us = 0;
        }
#endif
    }
    last_us = now_us;
}

static ABI int guest_gettimeofday(GuestTimeval *value,GuestTimezone *zone) {
    struct timeval t;
    struct timezone z;
    if (gettimeofday(&t,&z)) {
        /* Guest errno/TLS failure path is not implemented yet. */
        perror("STOP: host gettimeofday failed"); exit(21);
    }
    if (runtime_image_base) {
        const uintptr_t caller = guest_cpu_return_address((uintptr_t)__builtin_return_address(0)) - runtime_image_base;
        if (caller == PACER_BEGIN || caller == PACER_SPIN || caller == PACER_SLEEPING) pacer_time(value, caller, &t);
    }
    if (value) { value->seconds=t.tv_sec; value->microseconds=t.tv_usec; }
    if (zone) { zone->minuteswest=z.tz_minuteswest; zone->dsttime=z.tz_dsttime; }
    return 0;
}
uintptr_t runtime_time_resolve(const char *name) {
    if (runtime_nid_is(name, "n88vx3C5nW8")) return (uintptr_t)guest_gettimeofday;
    return 0;
}

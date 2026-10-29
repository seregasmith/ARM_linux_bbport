// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: a lock that spins instead of sleeping, for a few instructions' worth of work. Waiters
// only read the word until it looks free, so they do not bounce its cache line.
#pragma once

#include <atomic>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace Common {

class SpinLock {
public:
    SpinLock() = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() {
        while (locked.exchange(true, std::memory_order_acquire)) {
            while (locked.load(std::memory_order_relaxed)) {
#if defined(__x86_64__)
                _mm_pause();
#elif defined(__aarch64__)
                asm volatile("yield");
#endif
            }
        }
    }
    void unlock() {
        locked.store(false, std::memory_order_release);
    }
    [[nodiscard]] bool try_lock() {
        return !locked.load(std::memory_order_relaxed) &&
               !locked.exchange(true, std::memory_order_acquire);
    }

private:
    std::atomic<bool> locked{false};
};

} // namespace Common

// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the CPU's time stamp counter, read in order with the code around it. Other hosts than
// x86-64: the guest's counter (guest_cpu.h), the one the game's own RDTSC reads in FEXCore, at the
// frequency the runtime reports (runtime_tsc_frequency).
#pragma once

#if defined(__x86_64__)
#include <x86intrin.h>
#else
#include "../../src/guest_cpu.h"
#endif

#include "common/types.h"

namespace Common {

inline u64 FencedRDTSC() {
#if defined(__x86_64__)
    _mm_lfence();
    const u64 tsc = __rdtsc();
    _mm_lfence();
    return tsc;
#else
    return guest_cpu_tsc();
#endif
}

} // namespace Common

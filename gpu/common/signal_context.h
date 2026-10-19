// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what a SIGSEGV handler needs from its ucontext (x86-64 Linux; AArch64 Linux: the host
// PC and the data abort's syndrome).
#pragma once

#include <sys/ucontext.h>
#if defined(__aarch64__)
#include <algorithm>
#include <cstdint>
#include <cstring>
#endif

#include "common/types.h"

namespace Common {

#if defined(__aarch64__)
/// The data abort's ESR: the kernel stores it in a record of the signal frame's reserved area
/// (struct esr_context of <asm/sigcontext.h>, which conflicts with glibc's signal headers).
inline std::uint64_t FaultEsr(void* context) {
    struct Record {
        std::uint32_t magic, size;
        std::uint64_t esr;
    };
    constexpr std::uint32_t EsrMagic = 0x45535201;
    const auto& mcontext = static_cast<ucontext_t*>(context)->uc_mcontext;
    const auto* reserved = reinterpret_cast<const unsigned char*>(mcontext.__reserved);
    for (std::size_t offset = 0; offset + 8 <= sizeof(mcontext.__reserved);) {
        Record record;
        std::memcpy(&record, reserved + offset,
                    std::min(sizeof(record), sizeof(mcontext.__reserved) - offset));
        if (record.magic == 0 || record.size == 0) {
            break;
        }
        if (record.magic == EsrMagic) {
            return record.esr;
        }
        offset += record.size;
    }
    return 0;
}
#endif

/// The faulting instruction (AArch64: the host's, guest code's JIT translation included; the
/// guest's own address comes from guest_cpu_signal_regs).
inline void* GetRip(void* context) {
#if defined(__aarch64__)
    return reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.pc);
#else
    return reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#endif
}

/// The fault was a write (x86-64: page fault error code bit 1; AArch64: ESR_ELx.WnR).
inline bool IsWriteError(void* context) {
#if defined(__aarch64__)
    return FaultEsr(context) & 0x40;
#else
    return static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_ERR] & 0x2;
#endif
}

} // namespace Common

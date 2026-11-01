// SPDX-FileCopyrightText: Copyright 2026 Awaken (Lantern Engine); bbport contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Bloodborne (1.09): the game's state for the cheats (bloodborne_cheats_state.h). Lantern Engine's
// engine_state.cpp (4715bca), the parts the cheats use; reads go through the runtime's fault
// recovery (as bloodborne_kit_restore.cpp) instead of the platform's ReadProcessMemory.
//
// Reverse engineering (Lantern's), offsets in the image:
//  - [0x553b130] GameDataMan: + 0x08 the player's record (stats at the offsets of kStats).
//  - [0x553e878] WorldChrMan: + 0x60 the player's character, its map block at + 0x3f8.
//  - [0x5540340] the param repository: 63 slots of 0x48 bytes from + 0x70, each a count (s32) and
//    up to 8 holders ("caps"); a holder's name is a wide string at + 0x18 (inline below 8
//    characters of capacity, + 0x30), its file at [[cap + 0x70] + 0x70]: rows (u16) at + 0x0a,
//    format + 0x2d, flags + 0x2e, the records from + 0x40 (+ 0x34 in format 2): an id and the
//    row's offset in the file, 12 bytes each, or 0x18 with 64-bit offsets (format > 3, flags & 2).
#include "bloodborne_cheats_state.h"

#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstring>

#include "bbport_toggles.h" // runtime_fault_recover

namespace BbCheats {
namespace {

constexpr std::size_t kGameDataMan = 0x553b130;
constexpr std::size_t kWorldChrMan = 0x553e878;
constexpr std::size_t kParamRepository = 0x5540340;

std::uint64_t g_image = 0;

template <typename T> bool Peek(std::uint64_t at, T* out) {
    return at && host_read_safe(reinterpret_cast<const void*>(at), out, sizeof(T));
}
std::uint64_t Follow(std::uint64_t at) {
    std::uint64_t v = 0;
    return Peek(at, &v) ? v : 0;
}
std::uint64_t Singleton(std::size_t slot) { return g_image ? Follow(g_image + slot) : 0; }

struct Stat {
    const char* name;
    std::uint32_t at;
};
constexpr Stat kStats[] = {{"hp", 0x14},       {"vitality", 0x40},   {"endurance", 0x48}, {"strength", 0x58},
                           {"skill", 0x60},    {"bloodtinge", 0x68}, {"arcane", 0x70},    {"insight", 0x84},
                           {"level", 0x90},    {"echoes", 0x94}};
const Stat* StatNamed(const char* name) {
    if (!name) return nullptr;
    for (const auto& s : kStats)
        if (!std::strcmp(s.name, name)) return &s;
    return nullptr;
}
std::atomic<std::uint64_t> g_said_player{0};

constexpr std::size_t kPlayerChr = 0x60, kPlayerBlock = 0x3f8;

constexpr unsigned kSlots = 63;
constexpr std::size_t kSlotBase = 0x70, kSlotSize = 0x48;

bool NameIs(std::uint64_t cap, const char* wanted) {
    std::uint64_t capacity = 0;
    if (!Peek(cap + 0x30, &capacity)) return false;
    const std::uint64_t text = capacity < 8 ? cap + 0x18 : Follow(cap + 0x18);
    for (unsigned i = 0; i < 64; ++i) {
        char16_t c = 0;
        if (!Peek(text + 2 * i, &c) || c != char16_t(static_cast<unsigned char>(wanted[i]))) return false;
        if (!c) return true;
    }
    return false;
}
std::uint64_t FindCap(const char* wanted) {
    const std::uint64_t repository = Singleton(kParamRepository);
    if (!repository || !wanted) return 0;
    for (unsigned s = 0; s < kSlots; ++s) {
        const std::uint64_t slot = repository + kSlotBase + kSlotSize * s;
        std::int32_t count = 0;
        if (!Peek(slot, &count) || count <= 0) continue;
        for (int k = 0; k < count && k < 8; ++k) {
            const std::uint64_t cap = Follow(slot + 8 + 8 * k);
            if (cap && NameIs(cap, wanted)) return cap;
        }
    }
    return 0;
}
bool Describe(std::uint64_t cap, ParamTable* t) {
    t->cap = cap;
    t->file = params_table_file(cap);
    return t->file && Peek(t->file + 0xa, &t->rows) && t->rows && Peek(t->file + 0x2d, &t->format) &&
           Peek(t->file + 0x2e, &t->flags);
}
bool Wide(const ParamTable& t) { return t.format > 3 && (t.flags & 2); }
std::size_t Stride(const ParamTable& t) { return Wide(t) ? 0x18 : 12; }
std::uint64_t Records(const ParamTable& t) { return t.file + (t.format == 2 ? 0x34 : 0x40); }

} // namespace

bool host_read_safe(const void* p, void* out, std::size_t n) {
    if (reinterpret_cast<std::uintptr_t>(p) < 0x10000 || !out) return false;
    sigjmp_buf* const outer = runtime_fault_recover;
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = outer;
        return false;
    }
    runtime_fault_recover = &recover;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::memcpy(out, p, n);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    runtime_fault_recover = outer;
    return true;
}

void StateInitialize(std::uint64_t image) { g_image = image; }

std::uint64_t player_record() {
    const std::uint64_t man = Singleton(kGameDataMan);
    const std::uint64_t record = man ? Follow(man + 8) : 0;
    if (record && g_said_player.load(std::memory_order_relaxed) != record) {
        std::int32_t level = 0, echoes = 0, insight = 0;
        if (Peek(record + 0x90, &level) && level > 0 && Peek(record + 0x94, &echoes) && Peek(record + 0x84, &insight) &&
            g_said_player.exchange(record) != record)
            std::printf("Cheats: player: level %d, %d Blood Echoes, %d Insight\n", level, echoes, insight);
    }
    return record;
}

bool player_stat_get(const char* name, std::int64_t* value) {
    const Stat* s = StatNamed(name);
    const std::uint64_t record = s ? player_record() : 0;
    std::int32_t v = 0;
    if (!record || !Peek(record + s->at, &v)) return false;
    *value = v;
    return true;
}

bool player_stat_set(const char* name, std::int64_t value) {
    const Stat* s = StatNamed(name);
    const std::uint64_t record = s ? player_record() : 0;
    std::int32_t now = 0;
    if (!record || !Peek(record + s->at, &now)) return false;
    const auto v = static_cast<std::int32_t>(value);
    std::memcpy(reinterpret_cast<void*>(record + s->at), &v, sizeof v);
    return true;
}

bool world_player_block(std::uint32_t* block) {
    const std::uint64_t man = Singleton(kWorldChrMan), chr = man ? Follow(man + kPlayerChr) : 0;
    std::uint32_t v = 0xffffffffu;
    if (!chr || !Peek(chr + kPlayerBlock, &v) || v == 0xffffffffu) return false;
    if (block) *block = v;
    return true;
}

bool params_table(const char* table, ParamTable* out) {
    if (!out) return false;
    const std::uint64_t cap = FindCap(table);
    ParamTable t;
    if (!cap || !Describe(cap, &t)) return false;
    *out = t;
    return true;
}

std::uint64_t params_table_file(std::uint64_t cap) {
    const std::uint64_t holder = cap ? Follow(cap + 0x70) : 0;
    return holder ? Follow(holder + 0x70) : 0;
}

bool params_records(const ParamTable& t, std::vector<ParamRecord>* out) {
    if (!t.file || !t.rows || !out) return false;
    const std::size_t stride = Stride(t);
    std::vector<std::uint8_t> bytes(stride * t.rows);
    if (!host_read_safe(reinterpret_cast<const void*>(Records(t)), bytes.data(), bytes.size())) return false;
    out->clear();
    out->reserve(t.rows);
    for (std::size_t i = 0; i < t.rows; ++i) {
        const std::uint8_t* r = bytes.data() + stride * i;
        ParamRecord rec{};
        std::uint64_t offset = 0;
        std::memcpy(&rec.id, r, 4);
        if (Wide(t)) {
            std::memcpy(&offset, r + 8, 8);
        } else {
            std::uint32_t o = 0;
            std::memcpy(&o, r + 4, 4);
            offset = o;
        }
        if (offset < 0x34 || offset > 64u << 20) return false;
        rec.data = t.file + offset;
        out->push_back(rec);
    }
    return true;
}

} // namespace BbCheats

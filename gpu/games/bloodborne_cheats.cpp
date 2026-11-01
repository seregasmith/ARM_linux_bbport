// SPDX-FileCopyrightText: Copyright 2026 Awaken (Lantern Engine); bbport contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Bloodborne (1.09): cheats (bloodborne_cheats.h), ported from Lantern Engine's
// engine/gpu/shim/engine/cheats.cpp (github.com/Awakenchan/LanternEngine at 4715bca; the plan and
// its phases: docs/CHEATS_PLAN.md). Names and structure follow Lantern's, to keep the two easy to
// compare.
//
// Phase 1: only what changes the game's data: param bytes (items not used up, firearm range, the
// echoes enemies give), the game's stealth debug flags, the player's stamina, echoes and insight.
// No call into the game and no byte of its code written, so it runs the same on every CPU
// (x86-64, FEXCore, the ahead-of-time code).
//
// Differences from Lantern:
//  - The tick is the pad read (bbgpu_game_tick: the game's main thread, once a frame; phase 0),
//    not a detour in the frame pacer: aarch64 has no native detours.
//  - Infinite Blood Vials / Quicksilver Bullets are "not used up" (the vial row's consume bit,
//    every gun's bulletConsumeNum), through the same byte patches as the consumables. Lantern's
//    refill to the carry cap and its custom amounts call into the game: phase 2.
//  - Online is the runtime's own answer (runtime_net_enabled): on aarch64 the game plays offline
//    with BB_ONLINE=1 set, which Lantern's check of the variable would take for online.
//  - The addresses stay Lantern's PS4 addresses (the eboot at 0x400000); Slot() makes them
//    addresses in the image (offset = Lantern's address - 0x400000).
#include "bloodborne_cheats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <span>
#include <vector>

#include "bbport_settings.h"
#include "bloodborne_cheats_state.h"

// bb-probe (src/runtime_net.c): 1 while the game plays online. Weak: the GPU library's tests run
// without the runtime.
extern "C" int runtime_net_enabled(void) __attribute__((weak));

namespace BbCheats {
namespace {
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kPreferredGuestSlide = 0x400000; ///< Lantern's addresses: the eboot's PS4 base
constexpr auto kDebounce = std::chrono::milliseconds(400);
constexpr auto kInterval = std::chrono::milliseconds(250);
constexpr auto kSettle = std::chrono::milliseconds(1500);
constexpr const char* kNotVerified = "the game's code was not checked";
constexpr const char* kLater = "not in this version yet";

constexpr std::size_t kGoodsRowSize = 0x6c, kMaxNum = 0x36, kConsumeByte = 0x44;
constexpr std::uint8_t kIsConsume = 0x80;
constexpr std::size_t kWeaponRowSize = 0x13c, kBulletConsumeNum = 0x13a;
constexpr std::uint32_t kBloodVial = 1000;

// The game's goods consume check (reads the row's consume bit) and the guns' bullet cost (reads
// bulletConsumeNum): the param bytes the cheats change are the ones this code reads.
constexpr std::uint8_t kSigConsume[] = {0xe8,0x0b,0xd6,0xa4,0x00,0x48,0x89,0xde,0x48,0x8b,0x45,0xa0,0x48,
    0x85,0xc0,0x0f,0x84,0xc5,0x02,0x00,0x00,0x8b,0x40,0x42,0xa9,0x00,0x00,0x80,0x00,0x0f,0x84,0xb7,0x02,
    0x00,0x00};
constexpr std::uint8_t kSigBullets[] = {0xe8,0xcb,0xf8,0x62,0x00,0x48,0x8b,0x45,0x98,0x45,0x31,0xe4,0x41,
    0xbd,0x00,0x00,0x00,0x00,0x48,0x85,0xc0,0x74,0x08,0x44,0x0f,0xbe,0xa8,0x3a,0x01,0x00,0x00};
struct Signature { std::uint64_t bn; std::span<const std::uint8_t> bytes; };

std::uint64_t g_slide = 0;
std::uint8_t* g_image = nullptr;
std::size_t g_image_size = 0;
bool g_installed = false;
std::uint64_t g_record = 0;
std::uint32_t g_block = 0;
Clock::time_point g_since{}, g_check{};
bool g_present = false, g_settled = false;
std::uint64_t Slot(std::uint64_t bn) { return g_slide + (bn - kPreferredGuestSlide); }
template <typename T> bool Read(std::uint64_t at, T* out) { return at && host_read_safe(reinterpret_cast<const void*>(at), out, sizeof(T)); }
bool Plausible(std::uint64_t p) { std::uint64_t v = 0; return p >= 0x10000 && p < (std::uint64_t(1) << 40) && Read(p, &v); }
bool Matches(std::uint64_t bn, std::span<const std::uint8_t> bytes) {
    const std::size_t at = bn - kPreferredGuestSlide;
    return g_image && at <= g_image_size && bytes.size() <= g_image_size - at &&
           !std::memcmp(g_image + at, bytes.data(), bytes.size());
}

struct Byte { std::uint64_t at; std::uint8_t original, now; };

bool LoadTable(const char* table, std::size_t row_size, ParamTable* t, std::vector<ParamRecord>* records,
               std::vector<std::uint8_t>* data, std::uint64_t* lo) {
    if (!params_table(table, t) || !params_records(*t, records) || records->size() < 2 ||
        (*records)[1].data - (*records)[0].data != row_size) return false;
    std::uint64_t first = ~std::uint64_t(0), last = 0;
    for (const ParamRecord& r : *records) { first = std::min(first, r.data); last = std::max(last, r.data); }
    if (last - first > 16 * 1024 * 1024) return false;
    data->resize(last - first + row_size);
    *lo = first;
    return host_read_safe(reinterpret_cast<const void*>(first), data->data(), data->size());
}

constexpr std::uint64_t kWorldChrMan = 0x593e878;
constexpr std::uint64_t kPlayerIns = 0x60, kModules = 0x3b0, kDataModule = 0x20, kStamina = 0x134;
constexpr std::uint8_t kSigStaminaLoad[] = {0x49,0x8b,0x8c,0x24,0xb0,0x03,0x00,0x00,0x4c,0x8b,0x79,0x20,0x41,0x8b,
    0x9f,0x34,0x01,0x00,0x00};
constexpr std::uint8_t kSigStaminaClamp[] = {0x48,0x8d,0x05,0xe1,0x83,0xc8,0x03,0x80,0x38,0x00,0x75,0x1f,0xb8,0xce,
    0xff,0xff,0xff,0x83,0xfb,0xce,0x7c,0x0e,0x41,0x8b,0x87,0x38,0x01,0x00,0x00,0x39,0xd8,0x0f,0x4e,0xd8,0x89,
    0xd8,0x41,0x89,0x87,0x34,0x01,0x00,0x00};
constexpr std::uint64_t kPlayerHide = 0x593e88e, kPlayerSilence = 0x593e88f;
constexpr std::uint8_t kSigStealthMenu[] = {0x48,0x8d,0x35,0xfc,0x48,0x06,0x03,0x48,0x8d,0x15,0xc9,0xd3,0xc1};
constexpr std::uint8_t kSigSilentMenu[] = {0x48,0x8d,0x35,0xda,0x48,0x06,0x03,0x48,0x8d,0x15,0x88,0xd3,0xc1};
constexpr std::uint8_t kSigStealthUse[] = {0x48,0x8d,0x05,0xac,0x93,0xae,0x03,0x8a,0x00,0x88,0x43,0x64};
constexpr std::uint8_t kSigSilentUse[] = {0x48,0x8d,0x05,0xc5,0x95,0xae,0x03,0x80,0x38,0x00};
constexpr std::int64_t kEchoCap = 999999999, kInsightCap = 99;
constexpr std::uint8_t kSigInsight[] = {0x44,0x03,0xb8,0x84,0x00,0x00,0x00,0x78,0x0d,0x41,0x83,0xff,0x63,0xb9,0x63,
    0x00,0x00,0x00,0x41,0x0f,0x4e,0xcf,0x89,0x88,0x84,0x00,0x00,0x00};
constexpr std::uint8_t kSigEchoes[] = {0x48,0x8b,0x93,0xc0,0x03,0x00,0x00,0x8b,0xb2,0x94,0x00,0x00,0x00,0x31,0xc9,
    0x89,0xf7,0x01,0xc7,0x78,0x0e,0x81,0xff,0xff,0xc9,0x9a,0x3b,0xb9,0xff,0xc9,0x9a,0x3b,0x0f,0x4e,0xcf,0x89,
    0x8a,0x94,0x00,0x00,0x00};
constexpr std::uint32_t kFirearmVariations[] = {600, 610, 1000, 1010, 1400, 1410, 1500, 3300, 3500, 3600};
constexpr std::size_t kBehaviorRowSize = 0x20, kBehaviorVariation = 0x0, kBehaviorRefType = 0x9, kBehaviorRefId = 0xc;
constexpr std::size_t kBulletRowSize = 0xa0, kBulletLife = 0x10, kBulletDist = 0x14, kBulletVelocity = 0x28,
    kBulletNext = 0x68;
constexpr std::size_t kNpcRowSize = 0x184, kGetSoul = 0x28, kAreaRowSize = 0x20;
constexpr std::uint32_t kConsumables[] = {
    1100, 1101,
    1110, 1120,
    1200, 1201, 1250, 1251,
    1210, 1240, 1230,
    1220, 1260, 1270,
    1300, 1320, 1330,
    1410,
    2030, 2090,
};
constexpr std::size_t kGoodsType = 0x3a, kRefCategory = 0x3b;

struct Patch {
    std::vector<Byte> bytes;
    std::vector<ParamTable> tables;
    bool snapped = false;
};
bool Current(const Patch& p) {
    for (const ParamTable& t : p.tables)
        if (params_table_file(t.cap) != t.file) return false;
    return true;
}
void Forget(Patch& p) { p.snapped = false; p.bytes.clear(); p.tables.clear(); }
std::vector<std::uint8_t> Originals(const Patch& p) {
    std::vector<std::uint8_t> v(p.bytes.size());
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = p.bytes[i].original;
    return v;
}
/// Writes the bytes that differ from `want`, after checking that each still holds what the patch
/// last wrote (nobody else changed it); false, and nothing written, when one does not.
bool Write(Patch& p, const std::vector<std::uint8_t>& want) {
    bool change = false;
    std::vector<std::uint8_t> run;
    for (std::size_t i = 0, j = 0; i < p.bytes.size(); i = j) {
        bool differs = want[i] != p.bytes[i].now;
        for (j = i + 1; j < p.bytes.size() && p.bytes[j].at == p.bytes[j - 1].at + 1; ++j)
            differs = differs || want[j] != p.bytes[j].now;
        if (!differs) continue;
        run.resize(j - i);
        if (!host_read_safe(reinterpret_cast<const void*>(p.bytes[i].at), run.data(), run.size())) return false;
        for (std::size_t k = i; k < j; ++k)
            if (want[k] != p.bytes[k].now && run[k - i] != p.bytes[k].now) return false;
        change = true;
    }
    if (!change) return true;
    for (std::size_t i = 0; i < p.bytes.size(); ++i) {
        if (want[i] == p.bytes[i].now) continue;
        std::memcpy(reinterpret_cast<void*>(p.bytes[i].at), &want[i], 1);
        p.bytes[i].now = want[i];
    }
    return true;
}
void RestoreSafe(Patch& p) {
    if (!p.snapped) return;
    if (!Current(p)) { Forget(p); return; }
    if (!Write(p, Originals(p))) {
        for (Byte& b : p.bytes) {
            std::uint8_t current = 0;
            if (b.now == b.original || !Read(b.at, &current) || current != b.now) continue;
            std::memcpy(reinterpret_cast<void*>(b.at), &b.original, 1);
            b.now = b.original;
        }
    }
}

enum Kind { Refill, Flags, Params };
struct Extra {
    Which which;
    Kind kind;
    const char* name;
    std::atomic<const char*> unavailable{kNotVerified};
    bool disabled = false;
    int active = 0, pending = 0;
    Clock::time_point pending_since{};
    Patch patch{};
    int failures = 0;
};
Extra g_extras[] = {
    {Vials, Params, "vials"}, {Bullets, Params, "bullets"},
    {Stamina, Refill, "stamina"}, {Stealth, Flags, "stealth"}, {GunRange, Params, "firearm range"},
    {EchoMultiplier, Params, "echo multiplier"}, {Consumables, Params, "consumables"},
};
Extra* Find(Which which) {
    for (Extra& e : g_extras)
        if (e.which == which) return &e;
    return nullptr;
}
Extra& ExtraOf(Which which) {
    Extra* e = Find(which);
    return e ? *e : g_extras[0];
}
std::atomic<const char*> g_echoes_unavailable{kNotVerified};
std::atomic<long long> g_add_echoes{0}, g_add_insight{0};

void Disable(Extra& e, const char* why) {
    if (e.disabled) return;
    e.disabled = true;
    RestoreSafe(e.patch);
    e.unavailable.store(why);
    std::printf("Cheats: %s unavailable: %s\n", e.name, why);
}

bool SharedWorld() {
    static const bool shared = [] {
        const bool on = runtime_net_enabled && runtime_net_enabled() != 0;
        if (on) std::printf("Cheats: off while playing online\n");
        return on;
    }();
    return shared;
}

int SettingOf(const Extra& e, const BbSettings::Values& s) {
    switch (e.which) {
    case Vials: return s.cheat_vials.load(std::memory_order_relaxed) == BbSettings::CheatInfinite ? 1 : 0;
    case Bullets: return s.cheat_bullets.load(std::memory_order_relaxed) == BbSettings::CheatInfinite ? 1 : 0;
    case Stamina: return s.cheat_stamina.load(std::memory_order_relaxed);
    case Stealth: return s.cheat_stealth.load(std::memory_order_relaxed);
    case Consumables: return s.cheat_consumables.load(std::memory_order_relaxed);
    case GunRange: {
        const int v = s.cheat_gun_range.load(std::memory_order_relaxed);
        return std::find(std::begin(BbSettings::CheatGunRanges), std::end(BbSettings::CheatGunRanges), v) !=
                   std::end(BbSettings::CheatGunRanges) ? v : 0;
    }
    case EchoMultiplier: {
        const int v = s.cheat_echo_multiplier.load(std::memory_order_relaxed);
        return v > 1 && std::find(std::begin(BbSettings::CheatEchoMultipliers),
                                  std::end(BbSettings::CheatEchoMultipliers), v) !=
                            std::end(BbSettings::CheatEchoMultipliers) ? v : 0;
    }
    default: return 0;
    }
}

const char* SnapshotVials(Patch& p) {
    ParamTable goods;
    std::vector<ParamRecord> records;
    std::vector<std::uint8_t> data;
    std::uint64_t lo = 0;
    if (!LoadTable("EquipParamGoods", kGoodsRowSize, &goods, &records, &data, &lo))
        return "EquipParamGoods not found or its rows are not 0x6c bytes";
    const auto it = std::find_if(records.begin(), records.end(), [](const ParamRecord& r) { return r.id == kBloodVial; });
    if (it == records.end()) return "the Blood Vial's goods row is missing";
    const std::uint8_t* row = data.data() + (it->data - lo);
    std::int16_t max_num = 0;
    std::memcpy(&max_num, row + kMaxNum, 2);
    if (max_num < 1 || max_num > 999) return "the Blood Vial's maxNum is implausible";
    if (!(row[kConsumeByte] & kIsConsume)) return "the Blood Vial row is not consumable";
    p.bytes.clear();
    p.bytes.push_back({it->data + kConsumeByte, row[kConsumeByte], row[kConsumeByte]});
    p.tables = {goods};
    return nullptr;
}

const char* SnapshotBullets(Patch& p) {
    ParamTable weapons;
    std::vector<ParamRecord> records;
    std::vector<std::uint8_t> data;
    std::uint64_t lo = 0;
    if (!LoadTable("EquipParamWeapon", kWeaponRowSize, &weapons, &records, &data, &lo))
        return "EquipParamWeapon not found or its rows are not 0x13c bytes";
    p.bytes.clear();
    std::size_t none = 0;
    for (const ParamRecord& r : records) {
        const auto v = static_cast<std::int8_t>(data[r.data - lo + kBulletConsumeNum]);
        if (v > 64 || v < -1) return "a weapon's bulletConsumeNum is out of range";
        if (v < 0) { ++none; continue; }
        if (v) p.bytes.push_back({r.data + kBulletConsumeNum, std::uint8_t(v), std::uint8_t(v)});
    }
    if (p.bytes.empty()) return "no weapon uses bullets";
    p.tables = {weapons};
    std::printf("Cheats: bullets: %zu weapons cost bullets, %zu without a cost (-1) left alone\n",
                p.bytes.size(), none);
    return nullptr;
}

const char* SnapshotGunRange(Patch& p) {
    ParamTable behavior, bullets;
    std::vector<ParamRecord> brecords, records;
    std::vector<std::uint8_t> bdata, data;
    std::uint64_t blo = 0, lo = 0;
    if (!LoadTable("BehaviorParam_PC", kBehaviorRowSize, &behavior, &brecords, &bdata, &blo))
        return "BehaviorParam_PC not found or its rows are not 0x20 bytes";
    if (!LoadTable("Bullet", kBulletRowSize, &bullets, &records, &data, &lo))
        return "Bullet not found or its rows are not 0xa0 bytes";
    const auto row = [&](std::int32_t id) -> const ParamRecord* {
        const auto it = std::find_if(records.begin(), records.end(), [&](const ParamRecord& r) { return std::int32_t(r.id) == id; });
        return it == records.end() ? nullptr : &*it;
    };
    const auto reach = [&](bool firearm, std::vector<std::uint64_t>* out, std::size_t* variations) {
        std::vector<std::uint32_t> seen;
        for (const ParamRecord& b : brecords) {
            const std::uint8_t* r = bdata.data() + (b.data - blo);
            std::uint32_t variation = 0;
            std::int32_t id = 0;
            std::memcpy(&variation, r + kBehaviorVariation, 4);
            std::memcpy(&id, r + kBehaviorRefId, 4);
            const bool gun = std::find(std::begin(kFirearmVariations), std::end(kFirearmVariations), variation) !=
                             std::end(kFirearmVariations);
            if (r[kBehaviorRefType] != 1 || gun != firearm) continue;
            if (gun && variations && std::find(seen.begin(), seen.end(), variation) == seen.end()) {
                seen.push_back(variation);
                ++*variations;
            }
            for (int depth = 0; depth < 8 && id > 0; ++depth) {
                const ParamRecord* bullet = row(id);
                if (!bullet || std::find(out->begin(), out->end(), bullet->data) != out->end()) break;
                out->push_back(bullet->data);
                std::memcpy(&id, data.data() + (bullet->data - lo) + kBulletNext, 4);
            }
        }
    };
    std::vector<std::uint64_t> guns, others;
    std::size_t variations = 0;
    reach(true, &guns, &variations);
    reach(false, &others, nullptr);
    if (variations != std::size(kFirearmVariations)) return "a firearm's behaviour rows are missing";
    p.bytes.clear();
    std::size_t rows = 0;
    for (std::uint64_t at : guns) {
        if (std::find(others.begin(), others.end(), at) != others.end()) continue;
        const std::uint8_t* r = data.data() + (at - lo);
        float life = 0, dist = 0, velocity = 0;
        std::memcpy(&life, r + kBulletLife, 4);
        std::memcpy(&dist, r + kBulletDist, 4);
        std::memcpy(&velocity, r + kBulletVelocity, 4);
        if (!std::isfinite(velocity) || velocity <= 0) continue;
        if (velocity > 200 || !std::isfinite(life) || life <= 0 || life > 5 || !std::isfinite(dist) || dist < 0 ||
            dist > 50)
            return "a firearm bullet's life, range or speed is implausible";
        for (std::size_t off : {kBulletLife, kBulletDist})
            for (std::size_t k = 0; k < 4; ++k) p.bytes.push_back({at + off + k, r[off + k], r[off + k]});
        ++rows;
    }
    if (rows < 20 || rows > 120) return "unexpected number of firearm bullet rows";
    p.tables = {behavior, bullets};
    std::printf("Cheats: firearm range: %zu bullet rows of %zu firearm behaviours\n", rows, variations);
    return nullptr;
}

const char* SnapshotEchoes(Patch& p) {
    p.bytes.clear();
    p.tables.clear();
    std::size_t rows = 0;
    struct Field { const char* table; std::size_t size; std::size_t offsets[2]; std::size_t count; std::uint32_t most; };
    const Field fields[] = {{"NpcParam", kNpcRowSize, {kGetSoul, 0}, 1, 1000000},
                            {"GameAreaParam", kAreaRowSize, {0x0, 0x4}, 2, 10000000}};
    for (const Field& f : fields) {
        ParamTable t;
        std::vector<ParamRecord> records;
        std::vector<std::uint8_t> data;
        std::uint64_t lo = 0;
        if (!LoadTable(f.table, f.size, &t, &records, &data, &lo))
            return f.size == kNpcRowSize ? "NpcParam not found or its rows are not 0x184 bytes"
                                         : "GameAreaParam not found or its rows are not 0x20 bytes";
        for (const ParamRecord& r : records) {
            for (std::size_t k = 0; k < f.count; ++k) {
                const std::uint8_t* v = data.data() + (r.data - lo) + f.offsets[k];
                std::uint32_t echoes = 0;
                std::memcpy(&echoes, v, 4);
                if (!echoes) continue;
                if (echoes > f.most) return "an enemy's Blood Echoes are implausible";
                for (std::size_t b = 0; b < 4; ++b) p.bytes.push_back({r.data + f.offsets[k] + b, v[b], v[b]});
                ++rows;
            }
        }
        p.tables.push_back(t);
    }
    if (rows < 100) return "too few enemies give Blood Echoes";
    std::printf("Cheats: echo multiplier: %zu enemy and boss echo values\n", rows);
    return nullptr;
}

const char* SnapshotConsumables(Patch& p) {
    ParamTable goods;
    std::vector<ParamRecord> records;
    std::vector<std::uint8_t> data;
    std::uint64_t lo = 0;
    if (!LoadTable("EquipParamGoods", kGoodsRowSize, &goods, &records, &data, &lo))
        return "EquipParamGoods not found or its rows are not 0x6c bytes";
    p.bytes.clear();
    for (std::uint32_t id : kConsumables) {
        const auto it = std::find_if(records.begin(), records.end(), [&](const ParamRecord& r) { return r.id == id; });
        if (it == records.end()) return "a consumable's goods row is missing";
        const std::uint8_t* row = data.data() + (it->data - lo);
        std::int16_t max_num = 0;
        std::memcpy(&max_num, row + kMaxNum, 2);
        if (row[kGoodsType] != 0 || max_num < 2 || max_num > 99 || !(row[kConsumeByte] & kIsConsume) ||
            (row[kRefCategory] != 1 && row[kRefCategory] != 2))
            return "a consumable's goods row is not a stackable consumable";
        p.bytes.push_back({it->data + kConsumeByte, row[kConsumeByte], row[kConsumeByte]});
    }
    p.tables = {goods};
    return nullptr;
}

std::vector<std::uint8_t> WantedOf(const Extra& e, int mode) {
    std::vector<std::uint8_t> want = Originals(e.patch);
    if (!mode) return want;
    switch (e.which) {
    case Stealth: std::fill(want.begin(), want.end(), std::uint8_t(1)); break;
    case Vials:
    case Consumables: for (std::uint8_t& b : want) b = std::uint8_t(b & ~kIsConsume); break;
    case Bullets: std::fill(want.begin(), want.end(), std::uint8_t(0)); break;
    case GunRange:
        for (std::size_t i = 0; i + 4 <= want.size(); i += 4) {
            float v = 0;
            std::memcpy(&v, want.data() + i, 4);
            v = v * float(mode) / 100.0f;
            std::memcpy(want.data() + i, &v, 4);
        }
        break;
    case EchoMultiplier:
        for (std::size_t i = 0; i + 4 <= want.size(); i += 4) {
            std::uint32_t v = 0;
            std::memcpy(&v, want.data() + i, 4);
            v *= std::uint32_t(mode);
            std::memcpy(want.data() + i, &v, 4);
        }
        break;
    default: break;
    }
    return want;
}

void LogSwitch(const Extra& e, int mode) {
    switch (e.which) {
    case Vials: std::printf(mode ? "Cheats: Blood Vials not used up\n" : "Cheats: Blood Vials off, params restored\n"); break;
    case Bullets:
        std::printf(mode ? "Cheats: Quicksilver Bullets not used up\n" : "Cheats: Quicksilver Bullets off, params restored\n");
        break;
    case Stamina: std::printf("Cheats: infinite stamina %s\n", mode ? "on" : "off"); break;
    case Stealth: std::printf(mode ? "Cheats: stealth on (Player Hide, Player Silence)\n" : "Cheats: stealth off, flags restored\n"); break;
    case GunRange:
        if (mode) std::printf("Cheats: firearm range %d%%\n", mode);
        else std::printf("Cheats: firearm range off, params restored\n");
        break;
    case EchoMultiplier:
        if (mode) std::printf("Cheats: echo multiplier x%d\n", mode);
        else std::printf("Cheats: echo multiplier off, params restored\n");
        break;
    case Consumables: std::printf(mode ? "Cheats: infinite consumables on\n" : "Cheats: infinite consumables off, params restored\n"); break;
    default: break;
    }
}

void Switch(Extra& e, int mode) {
    e.active = mode;
    e.failures = 0;
    g_check = Clock::time_point{};
    LogSwitch(e, mode);
    if (e.kind == Flags && mode && !e.patch.snapped) {
        e.patch.bytes.clear();
        for (std::uint64_t bn : {kPlayerHide, kPlayerSilence}) {
            std::uint8_t v = 0;
            if (!Read(Slot(bn), &v) || v > 1) { Disable(e, "a debug flag holds an unexpected value"); return; }
            e.patch.bytes.push_back({Slot(bn), v, v});
        }
        e.patch.snapped = true;
    }
    if (e.kind == Flags || !mode) {
        if (!e.patch.snapped) return;
        if (!Current(e.patch)) { Forget(e.patch); return; }
        if (!Write(e.patch, WantedOf(e, mode))) Disable(e, "a byte changed outside the cheat");
        if (e.kind == Flags && !mode) Forget(e.patch);
    }
}

std::uint64_t DataModule() {
    std::uint64_t man = 0, chr = 0, modules = 0, data = 0;
    if (!Read(Slot(kWorldChrMan), &man) || !Plausible(man) || !Read(man + kPlayerIns, &chr) || !Plausible(chr) ||
        !Read(chr + kModules, &modules) || !Plausible(modules) || !Read(modules + kDataModule, &data) || !Plausible(data))
        return 0;
    return data;
}

void RefillStamina() {
    const std::uint64_t data = DataModule();
    std::int32_t value[2] = {};
    if (!data || !Read(data + kStamina, &value) || value[1] < 1 || value[1] > 2000 || value[0] < -50 ||
        value[0] > value[1] || value[0] == value[1])
        return;
    std::memcpy(reinterpret_cast<void*>(data + kStamina), &value[1], 4);
}

void Run(Extra& e) {
    if (e.kind != Params) return;
    if (e.patch.snapped && !Current(e.patch)) {
        std::printf("Cheats: %s: the game reloaded its params; applying again\n", e.name);
        Forget(e.patch);
    }
    if (!e.patch.snapped) {
        const char* why = e.which == GunRange ? SnapshotGunRange(e.patch)
                        : e.which == EchoMultiplier ? SnapshotEchoes(e.patch)
                        : e.which == Vials ? SnapshotVials(e.patch)
                        : e.which == Bullets ? SnapshotBullets(e.patch)
                                             : SnapshotConsumables(e.patch);
        if (why) { Forget(e.patch); Disable(e, why); return; }
        e.patch.snapped = true;
    }
    if (!Write(e.patch, WantedOf(e, e.active))) Disable(e, "a param byte changed outside the cheat");
}

void AddStats() {
    const long long echoes = g_add_echoes.exchange(0), insight = g_add_insight.exchange(0);
    std::int64_t v = 0;
    if (echoes > 0) {
        if (!player_stat_get("echoes", &v) || v < 0 || v > kEchoCap) {
            std::printf("Cheats: Blood Echoes not readable; nothing added\n");
        } else {
            const std::int64_t now = std::min<std::int64_t>(kEchoCap, v + echoes);
            player_stat_set("echoes", now);
            std::printf("Cheats: +%lld Blood Echoes (%lld held)\n", static_cast<long long>(now - v), static_cast<long long>(now));
        }
    }
    if (insight > 0) {
        if (!player_stat_get("insight", &v) || v < 0 || v > kInsightCap) {
            std::printf("Cheats: Insight not readable; nothing added\n");
        } else {
            const std::int64_t now = std::min<std::int64_t>(kInsightCap, v + insight);
            player_stat_set("insight", now);
            std::printf("Cheats: +%lld Insight (%lld held)\n", static_cast<long long>(now - v), static_cast<long long>(now));
        }
    }
}

void InstallExtras() {
    const auto check = [](Which which, std::initializer_list<Signature> signatures) {
        for (const Signature& s : signatures) {
            if (!Matches(s.bn, s.bytes)) {
                Extra& e = ExtraOf(which);
                e.unavailable.store("the game's code differs from the verified 1.09 image");
                std::printf("Cheats: %s unavailable: the game's code differs at %#llx\n", e.name,
                            static_cast<unsigned long long>(s.bn - kPreferredGuestSlide));
                return false;
            }
        }
        ExtraOf(which).unavailable.store(nullptr);
        return true;
    };
    check(Vials, {{0x18d0dc0, kSigConsume}});
    check(Bullets, {{0x1cfa1d0, kSigBullets}});
    check(Stamina, {{0x1cb6451, kSigStaminaLoad}, {0x1cb64a2, kSigStaminaClamp}});
    check(Stealth, {{0x1d214b7, kSigStealthMenu}, {0x1d214f9, kSigSilentMenu}, {0x1e554db, kSigStealthUse},
                    {0x1e552c3, kSigSilentUse}});
    check(GunRange, {});
    check(EchoMultiplier, {});
    check(Consumables, {{0x18d0dc0, kSigConsume}});
    const bool stats = Matches(0x1cfc567, kSigInsight) && Matches(0x1cfc6a4, kSigEchoes);
    g_echoes_unavailable.store(stats ? nullptr : "the game's code differs from the verified 1.09 image");
    if (!stats) std::printf("Cheats: add echoes / insight unavailable: the game's code differs\n");
}
} // namespace

bool Install(std::uint8_t* image, std::size_t size) {
    g_image = image;
    g_image_size = size;
    g_slide = reinterpret_cast<std::uint64_t>(image);
    StateInitialize(g_slide);
    InstallExtras();
    g_installed = true;
    bool any = !g_echoes_unavailable.load();
    std::size_t ready = 0;
    for (Extra& e : g_extras) {
        if (e.unavailable.load()) e.disabled = true;
        any = any || !e.disabled;
        ready += !e.disabled;
    }
    g_image = nullptr;
    std::printf("Cheats: %zu of %zu ready (the game's code checked), add echoes / insight %s\n", ready,
                std::size(g_extras), g_echoes_unavailable.load() ? "unavailable" : "ready");
    return any;
}

bool Online() { return SharedWorld(); }

const char* Unavailable(Which which) {
    if (SharedWorld()) return "off while playing online (turn online off in the launcher to use cheats)";
    if (which == Echoes) return g_echoes_unavailable.load();
    if (which < 0 || which >= WhichCount) return kNotVerified;
    const Extra* e = Find(which);
    return e ? e->unavailable.load() : kLater;
}

void AddEchoes(int amount) {
    if (amount > 0 && g_installed && !g_echoes_unavailable.load()) g_add_echoes.fetch_add(std::min(amount, int(kEchoCap)));
}
void AddInsight(int amount) {
    if (amount > 0 && g_installed && !g_echoes_unavailable.load()) g_add_insight.fetch_add(std::min(amount, int(kInsightCap)));
}

void Tick() {
    if (!g_installed) return;
    const auto& s = BbSettings::Get();
    const bool shared = SharedWorld();
    if (shared) {
        g_add_echoes.store(0, std::memory_order_relaxed);
        g_add_insight.store(0, std::memory_order_relaxed);
    }
    bool idle = g_add_echoes.load(std::memory_order_relaxed) <= 0 && g_add_insight.load(std::memory_order_relaxed) <= 0;
    for (const Extra& e : g_extras) idle = idle && (e.disabled || ((shared || !SettingOf(e, s)) && !e.pending && !e.active));
    if (idle) return;
    const auto now = Clock::now();
    for (Extra& e : g_extras) {
        if (e.disabled) continue;
        const int value = shared ? 0 : SettingOf(e, s);
        if (value != e.pending) { e.pending = value; e.pending_since = now; }
        if (e.pending != e.active && now - e.pending_since >= kDebounce) Switch(e, e.pending);
    }
    Extra& stamina = ExtraOf(Stamina);
    if (g_settled && !stamina.disabled && stamina.active) RefillStamina();
    if (now < g_check) return;
    g_check = now + kInterval;
    const std::uint64_t record = player_record();
    std::uint32_t block = 0;
    const bool present = Plausible(record) && world_player_block(&block);
    if (!present || !g_present || record != g_record || block != g_block) {
        g_present = present;
        g_record = record;
        g_block = block;
        g_since = now;
        g_settled = false;
        if (!present) {
            const bool echoes = g_add_echoes.exchange(0) > 0, insight = g_add_insight.exchange(0) > 0;
            if (echoes || insight) std::printf("Cheats: no character in the world; Blood Echoes / Insight not added\n");
        }
        return;
    }
    if (now - g_since < kSettle) return;
    g_settled = true;
    for (Extra& e : g_extras) {
        if (!e.disabled && e.active) Run(e);
    }
    if (!g_echoes_unavailable.load()) AddStats();
}

} // namespace BbCheats

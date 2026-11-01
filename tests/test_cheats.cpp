// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// games/bloodborne_cheats.cpp against a made-up game in memory (no game, no GPU): an image with
// the code the cheats check at their sites, the singletons, a param repository with the tables
// they change, a player. Every cheat on (the bytes as wanted), off (the game's bytes back), a
// reload of the params (applied again), a byte changed by someone else (that cheat stops), and the
// game's code differing at a site (that cheat unavailable, the others ready).
#include <atomic>
#include <cassert>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bbport_settings.h"
#include "bloodborne_cheats.h"

// The runtime's fault recovery point (bb-probe defines it; nothing here faults).
extern "C" {
__thread sigjmp_buf* runtime_fault_recover = nullptr;
}

namespace {

using u8 = std::uint8_t;
using u64 = std::uint64_t;

constexpr u64 kBase = 0x400000; // the PS4 addresses' base (Lantern's)
constexpr u64 kImageSize = 0x5600000;

u8* image = nullptr;
u8* heap = nullptr;
u64 heap_used = 0;

template <typename T> T& At(u64 address) { return *reinterpret_cast<T*>(address); }
u64 Alloc(u64 size) {
    const u64 at = reinterpret_cast<u64>(heap) + heap_used;
    heap_used += (size + 15) & ~u64(15);
    assert(heap_used < (u64(64) << 20));
    return at;
}
void Put(u64 ps4_address, const char* hex) {
    u8* at = image + (ps4_address - kBase);
    for (std::size_t i = 0; hex[2 * i]; ++i) at[i] = u8(std::stoi(std::string(hex + 2 * i, 2), nullptr, 16));
}

// The code the cheats check (tools/cheat_sites.py, checked against the 1.09 image).
void PutCode() {
    Put(0x18d0dc0, "e80bd6a4004889de488b45a04885c00f84c50200008b4042a9000080000f84b7020000");
    Put(0x1cfa1d0, "e8cbf86200488b45984531e441bd000000004885c07408440fbea83a010000");
    Put(0x1cb6451, "498b8c24b00300004c8b7920418b9f34010000");
    Put(0x1cb64a2, "488d05e183c803803800751fb8ceffffff83fbce7c0e418b873801000039d80f4ed889d841898734010000");
    Put(0x1d214b7, "488d35fc480603488d15c9d3c1");
    Put(0x1d214f9, "488d35da480603488d1588d3c1");
    Put(0x1e554db, "488d05ac93ae038a00884364");
    Put(0x1e552c3, "488d05c595ae03803800");
    Put(0x1cfc567, "4403b884000000780d4183ff63b963000000410f4ecf898884000000");
    Put(0x1cfc6a4, "488b93c00300008bb29400000031c989f701c7780e81ffffc99a3bb9ffc99a3b0f4ecf898a94000000");
}

// ---- The param repository ([image + 0x5540340]) ------------------------------------------------

struct Table {
    const char* name;
    std::size_t row_size;
    std::vector<std::uint32_t> ids;
    u64 cap = 0, file = 0;
    u64 Row(std::uint32_t id) const {
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == id) return file + 0x40 + 12 * ids.size() + i * row_size;
        assert(!"no such row");
        return 0;
    }
};

/// A file of `t`'s rows (format 3: 12-byte records from + 0x40, the rows after them).
u64 MakeFile(const Table& t) {
    const u64 records = 0x40, rows = records + 12 * t.ids.size();
    const u64 file = Alloc(rows + t.row_size * t.ids.size());
    At<std::uint16_t>(file + 0xa) = std::uint16_t(t.ids.size());
    At<u8>(file + 0x2d) = 3;
    At<u8>(file + 0x2e) = 0;
    for (std::size_t i = 0; i < t.ids.size(); ++i) {
        At<std::uint32_t>(file + records + 12 * i) = t.ids[i];
        At<std::uint32_t>(file + records + 12 * i + 4) = std::uint32_t(rows + t.row_size * i);
    }
    return file;
}
void SetFile(Table& t, u64 file) {
    t.file = file;
    At<u64>(At<u64>(t.cap + 0x70) + 0x70) = file;
}

void MakeRepository(std::vector<Table*> tables) {
    const u64 repository = Alloc(0x70 + 63 * 0x48);
    At<u64>(reinterpret_cast<u64>(image) + 0x5540340) = repository;
    for (std::size_t s = 0; s < tables.size(); ++s) {
        Table& t = *tables[s];
        const u64 slot = repository + 0x70 + 0x48 * (s * 3 % 63); // spread over the slots
        At<std::int32_t>(slot) = 1;
        t.cap = Alloc(0x80);
        At<u64>(slot + 8) = t.cap;
        const std::size_t length = std::strlen(t.name);
        u64 text = t.cap + 0x18;
        if (length >= 8) {
            text = Alloc(2 * (length + 1));
            At<u64>(t.cap + 0x18) = text;
        }
        At<u64>(t.cap + 0x30) = length >= 8 ? length : 7;
        for (std::size_t i = 0; i <= length; ++i) At<char16_t>(text + 2 * i) = char16_t(t.name[i]);
        const u64 holder = Alloc(0x80);
        At<u64>(t.cap + 0x70) = holder;
        SetFile(t, MakeFile(t));
    }
}

constexpr std::uint32_t kConsumables[] = {1100, 1101, 1110, 1120, 1200, 1201, 1250, 1251, 1210, 1240,
                                          1230, 1220, 1260, 1270, 1300, 1320, 1330, 1410, 2030, 2090};
constexpr std::uint32_t kFirearms[] = {600, 610, 1000, 1010, 1400, 1410, 1500, 3300, 3500, 3600};

Table goods{"EquipParamGoods", 0x6c, {}}, weapons{"EquipParamWeapon", 0x13c, {10, 11, 12, 13, 14}},
    behavior{"BehaviorParam_PC", 0x20, {}}, bullets{"Bullet", 0xa0, {}}, npcs{"NpcParam", 0x184, {}},
    areas{"GameAreaParam", 0x20, {1, 2, 3}};
constexpr std::int8_t kBulletCosts[] = {1, 2, 0, -1, 1};

void FillGoods(const Table& t) {
    const auto item = [&](std::uint32_t id, std::int16_t max_num) {
        const u64 row = t.Row(id);
        At<std::int16_t>(row + 0x36) = max_num;
        At<u8>(row + 0x3a) = 0;    // goodsType
        At<u8>(row + 0x3b) = 1;    // refCategory
        At<u8>(row + 0x44) = 0x81; // consumed (0x80) and another bit that must stay
    };
    item(1000, 20);
    for (std::uint32_t id : kConsumables) item(id, 10);
}

void MakeParams() {
    goods.ids.push_back(900);
    goods.ids.push_back(1000);
    for (std::uint32_t id : kConsumables) goods.ids.push_back(id);
    for (std::uint32_t i = 0; i < 25; ++i) behavior.ids.push_back(100 + i);
    behavior.ids.push_back(200);
    for (std::uint32_t i = 1; i <= 25; ++i) bullets.ids.push_back(i);
    bullets.ids.push_back(99);
    for (std::uint32_t i = 0; i < 110; ++i) npcs.ids.push_back(5000 + i);
    MakeRepository({&goods, &weapons, &behavior, &bullets, &npcs, &areas});

    FillGoods(goods);
    for (std::size_t i = 0; i < weapons.ids.size(); ++i)
        At<std::int8_t>(weapons.Row(weapons.ids[i]) + 0x13a) = kBulletCosts[i];
    // 25 firearm behaviours, each with its own bullet; one other behaviour (a hunter tool) with bullet 99.
    for (std::uint32_t i = 0; i < 25; ++i) {
        const u64 row = behavior.Row(100 + i);
        At<std::uint32_t>(row + 0x0) = kFirearms[i % 10];
        At<u8>(row + 0x9) = 1;
        At<std::int32_t>(row + 0xc) = std::int32_t(i + 1);
    }
    const u64 tool = behavior.Row(200);
    At<std::uint32_t>(tool) = 9999;
    At<u8>(tool + 0x9) = 1;
    At<std::int32_t>(tool + 0xc) = 99;
    for (std::uint32_t id : bullets.ids) {
        const u64 row = bullets.Row(id);
        At<float>(row + 0x10) = 1.0f;   // life
        At<float>(row + 0x14) = 10.0f;  // distance
        At<float>(row + 0x28) = 100.0f; // velocity
    }
    for (std::uint32_t i = 0; i < npcs.ids.size(); ++i) At<std::uint32_t>(npcs.Row(npcs.ids[i]) + 0x28) = 100 + i;
    for (std::uint32_t id : areas.ids) {
        At<std::uint32_t>(areas.Row(id) + 0x0) = 1000 * id;
        At<std::uint32_t>(areas.Row(id) + 0x4) = 2000 * id;
    }
}

// ---- The player ---------------------------------------------------------------------------------

u64 record = 0, chr = 0, data = 0;

void MakePlayer() {
    const u64 game_data = Alloc(0x10);
    record = Alloc(0x400);
    At<u64>(reinterpret_cast<u64>(image) + 0x553b130) = game_data;
    At<u64>(game_data + 8) = record;
    At<std::int32_t>(record + 0x90) = 10;  // level
    At<std::int32_t>(record + 0x94) = 500; // Blood Echoes
    At<std::int32_t>(record + 0x84) = 3;   // Insight
    const u64 world = Alloc(0x80);
    chr = Alloc(0x400);
    const u64 modules = Alloc(0x40);
    data = Alloc(0x200);
    At<u64>(reinterpret_cast<u64>(image) + 0x553e878) = world;
    At<u64>(world + 0x60) = chr;
    At<std::uint32_t>(chr + 0x3f8) = 0x0f000000; // the map block
    At<u64>(chr + 0x3b0) = modules;
    At<u64>(modules + 0x20) = data;
    At<std::int32_t>(data + 0x134) = 50;  // stamina
    At<std::int32_t>(data + 0x138) = 100; // its maximum
}

void Ticks(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        BbCheats::Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void MakeGame() {
    // Below 1 TiB, as the game's memory (the cheats take higher pointers for garbage).
    void* memory = mmap(reinterpret_cast<void*>(0x7000000000ull), kImageSize + (u64(64) << 20),
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(memory != MAP_FAILED && reinterpret_cast<u64>(memory) < (u64(1) << 40));
    image = static_cast<u8*>(memory);
    heap = image + kImageSize;
    PutCode();
    MakeParams();
    MakePlayer();
}

const char* Why(BbCheats::Which which) {
    const char* why = BbCheats::Unavailable(which);
    return why ? why : "ready";
}

/// The game's code differs at the stamina clamp: stamina unavailable, the rest ready.
int OtherCode() {
    MakeGame();
    image[0x1cb64a2 - kBase] ^= 0xff;
    assert(BbCheats::Install(image, kImageSize));
    assert(BbCheats::Unavailable(BbCheats::Stamina) != nullptr);
    assert(std::strstr(BbCheats::Unavailable(BbCheats::Stamina), "differs"));
    for (auto which : {BbCheats::Vials, BbCheats::Bullets, BbCheats::Stealth, BbCheats::GunRange,
                       BbCheats::EchoMultiplier, BbCheats::Consumables, BbCheats::Echoes})
        assert(!BbCheats::Unavailable(which));
    auto& s = BbSettings::Get();
    s.cheat_stamina = true;
    Ticks(2200);
    assert(At<std::int32_t>(data + 0x134) == 50); // left alone
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (const pid_t child = fork(); child == 0) {
        _exit(OtherCode());
    } else {
        int status = 0;
        assert(child > 0 && waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    MakeGame();
    const u64 vial = goods.Row(1000) + 0x44;
    assert(BbCheats::Install(image, kImageSize));
    for (int w = 0; w < BbCheats::WhichCount; ++w) {
        const auto which = BbCheats::Which(w);
        const bool later = which == BbCheats::KeepEchoes || which == BbCheats::NoDurability;
        std::printf("cheat %d: %s\n", w, Why(which));
        assert(later == (BbCheats::Unavailable(which) != nullptr));
    }
    assert(!BbCheats::Online());

    // Everything on.
    auto& s = BbSettings::Get();
    s.cheat_vials = BbSettings::CheatInfinite;
    s.cheat_bullets = BbSettings::CheatInfinite;
    s.cheat_consumables = true;
    s.cheat_stamina = true;
    s.cheat_stealth = true;
    s.cheat_gun_range = 200;
    s.cheat_echo_multiplier = 5;
    BbCheats::AddEchoes(1000);
    BbCheats::AddInsight(1);
    Ticks(2400);
    assert(At<u8>(vial) == 0x01);
    for (std::uint32_t id : kConsumables) assert(At<u8>(goods.Row(id) + 0x44) == 0x01);
    for (std::size_t i = 0; i < weapons.ids.size(); ++i) {
        const auto now = At<std::int8_t>(weapons.Row(weapons.ids[i]) + 0x13a);
        assert(now == (kBulletCosts[i] < 0 ? -1 : 0));
    }
    for (std::uint32_t i = 1; i <= 25; ++i) {
        assert(At<float>(bullets.Row(i) + 0x10) == 2.0f && At<float>(bullets.Row(i) + 0x14) == 20.0f);
        assert(At<float>(bullets.Row(i) + 0x28) == 100.0f); // the speed stays
    }
    assert(At<float>(bullets.Row(99) + 0x14) == 10.0f); // not a firearm's
    for (std::uint32_t i = 0; i < npcs.ids.size(); ++i) assert(At<std::uint32_t>(npcs.Row(npcs.ids[i]) + 0x28) == 5 * (100 + i));
    assert(At<std::uint32_t>(areas.Row(3) + 0x4) == 5 * 6000);
    assert(image[0x553e88e] == 1 && image[0x553e88f] == 1); // Player Hide, Player Silence
    assert(At<std::int32_t>(data + 0x134) == 100);
    At<std::int32_t>(data + 0x134) = 30;
    Ticks(50);
    assert(At<std::int32_t>(data + 0x134) == 100); // refilled every frame
    assert(At<std::int32_t>(record + 0x94) == 1500 && At<std::int32_t>(record + 0x84) == 4);

    // Everything off: the game's bytes back.
    s.cheat_vials = BbSettings::CheatOff;
    s.cheat_bullets = BbSettings::CheatOff;
    s.cheat_consumables = false;
    s.cheat_stamina = false;
    s.cheat_stealth = false;
    s.cheat_gun_range = 0;
    s.cheat_echo_multiplier = 1;
    Ticks(800);
    assert(At<u8>(vial) == 0x81);
    for (std::uint32_t id : kConsumables) assert(At<u8>(goods.Row(id) + 0x44) == 0x81);
    for (std::size_t i = 0; i < weapons.ids.size(); ++i)
        assert(At<std::int8_t>(weapons.Row(weapons.ids[i]) + 0x13a) == kBulletCosts[i]);
    for (std::uint32_t i = 1; i <= 25; ++i)
        assert(At<float>(bullets.Row(i) + 0x10) == 1.0f && At<float>(bullets.Row(i) + 0x14) == 10.0f);
    for (std::uint32_t i = 0; i < npcs.ids.size(); ++i) assert(At<std::uint32_t>(npcs.Row(npcs.ids[i]) + 0x28) == 100 + i);
    assert(image[0x553e88e] == 0 && image[0x553e88f] == 0);
    At<std::int32_t>(data + 0x134) = 30;
    Ticks(50);
    assert(At<std::int32_t>(data + 0x134) == 30); // no refill

    // The game loads its params again (a new file): applied to the new one.
    s.cheat_vials = BbSettings::CheatInfinite;
    Ticks(800);
    assert(At<u8>(vial) == 0x01);
    const u64 old_vial = vial;
    SetFile(goods, MakeFile(goods));
    FillGoods(goods);
    Ticks(600);
    assert(At<u8>(goods.Row(1000) + 0x44) == 0x01);
    assert(At<u8>(old_vial) == 0x01); // the old file is the game's to free: left as it was

    // Someone else changes a byte the cheat wrote: switching off stops that cheat, the byte stays.
    const u64 now_vial = goods.Row(1000) + 0x44;
    At<u8>(now_vial) = 0x55;
    s.cheat_vials = BbSettings::CheatOff;
    Ticks(800);
    assert(BbCheats::Unavailable(BbCheats::Vials) != nullptr);
    assert(At<u8>(now_vial) == 0x55);
    std::printf("vials: %s\n", Why(BbCheats::Vials));

    // No character in the world: the echoes asked for are dropped.
    At<std::uint32_t>(chr + 0x3f8) = 0xffffffffu;
    BbCheats::AddEchoes(1000);
    Ticks(400);
    assert(At<std::int32_t>(record + 0x94) == 1500);

    std::puts("PASS: cheats on and off, params reloaded, a byte changed by someone else, other code at a site, "
              "echoes without a character");
}

// SPDX-FileCopyrightText: Copyright 2026 Awaken (Lantern Engine); bbport contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Bloodborne (1.09): what the cheats (bloodborne_cheats.cpp) read of the game's state: its param
// tables, the player's record and stats, the block the player is in. From Lantern Engine's
// engine/gpu/shim/engine/engine_state.cpp (github.com/Awakenchan/LanternEngine at 4715bca), with
// its names, so the two stay easy to compare. Plain memory reads, made safe by the runtime's fault
// recovery; addresses are offsets in the image.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace BbCheats {

/// The image's address, once (Install): the singletons are read through it.
void StateInitialize(std::uint64_t image);
/// Copies game memory; false where nothing is mapped.
bool host_read_safe(const void* p, void* out, std::size_t n);

/// A param table: its holder (the "cap") in the param repository, the loaded file, its rows and format.
struct ParamTable {
    std::uint64_t cap = 0, file = 0;
    std::uint16_t rows = 0;
    std::uint8_t format = 0, flags = 0;
};
struct ParamRecord {
    std::uint32_t id;
    std::uint64_t data; ///< the row's bytes
};
/// The loaded table named `table` (EquipParamGoods, Bullet, ...): false while the params are not loaded.
bool params_table(const char* table, ParamTable* out);
/// The file a table holds now: another value once the game loads its params again.
std::uint64_t params_table_file(std::uint64_t cap);
bool params_records(const ParamTable& table, std::vector<ParamRecord>* out);

/// The player's record (GameDataMan + 8): stats, inventory, equipment; 0 without a character.
std::uint64_t player_record();
/// "hp", "vitality", "endurance", "strength", "skill", "bloodtinge", "arcane", "insight", "level", "echoes".
bool player_stat_get(const char* name, std::int64_t* value);
bool player_stat_set(const char* name, std::int64_t value);
/// The map block the player's character is in: false while there is none (title, loading).
bool world_player_block(std::uint32_t* block);

} // namespace BbCheats

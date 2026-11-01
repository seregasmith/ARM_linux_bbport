// SPDX-FileCopyrightText: Copyright 2026 Awaken (Lantern Engine); bbport contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Bloodborne (1.09): cheats, ported from Lantern Engine (docs/CHEATS_PLAN.md). Chosen in the
// launcher (bbport.ini) or the overlay menu (BbSettings cheat_*), applied live; off puts the game's
// values back. Off while playing online.
#pragma once

#include <cstddef>
#include <cstdint>

namespace BbCheats {

/// Lantern's list (the later phases fill in KeepEchoes and NoDurability).
enum Which : int {
    Vials = 0, Bullets, Stamina, Stealth, GunRange, KeepEchoes, EchoMultiplier, NoDurability, Consumables,
    Echoes, ///< the overlay's add-echoes / add-insight buttons
    WhichCount
};

/// Checks the game's code at each cheat's sites (bbgpu_patch_image: after the startup patches) and
/// keeps the image's address. False when no cheat can run.
bool Install(std::uint8_t* image, std::size_t size);
/// Once a frame, on the game's main thread (bbgpu_game_tick: its pad read).
void Tick();
/// Why a cheat cannot run (for the overlay), or null when it can.
const char* Unavailable(Which which);
/// The game plays online: every cheat is off.
bool Online();
/// Added at the next tick to the character in the world, up to the game's limits (any thread).
void AddEchoes(int amount);
void AddInsight(int amount);

} // namespace BbCheats

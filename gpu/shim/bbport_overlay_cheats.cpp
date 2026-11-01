// SPDX-FileCopyrightText: Copyright 2026 Awaken (Lantern Engine); bbport contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the overlay menu's Cheats section (games/bloodborne_cheats.cpp), after Lantern Engine's
// (engine/gpu/shim/bbport_overlay.cpp at 4715bca, github.com/Awakenchan/LanternEngine): its widgets
// and texts, in this menu's style (one section under the others, no search). Applied at once; the
// launcher's Cheats section sets them for the next start.
#include "bbport_overlay_internal.h"

#include <atomic>
#include <cstddef>
#include <cstdio>

#include "bloodborne_cheats.h"
#include "imgui.h"

namespace BbOverlay::detail {
namespace {

void CheatUnavailable(BbCheats::Which which) {
    if (const char* why = BbCheats::Unavailable(which)) {
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("%s: %s", BbSettings::MenuText("Unavailable", "Недоступно"), why);
        ImGui::PopTextWrapPos();
    }
}

/// Blood Vials / Quicksilver Bullets: off or infinite (not used up; the custom amounts come later).
void CheatMode(const char* label, std::atomic<int>& value, BbCheats::Which which, const char* hint) {
    ImGui::PushID(label);
    const char* modes[] = {BbSettings::MenuText("Off", "Выкл."),
                           BbSettings::MenuText("Infinite", "Бесконечно")};
    int mode = value == BbSettings::CheatInfinite ? 1 : 0;
    if (ImGui::Combo(label, &mode, modes, 2)) {
        Store(value, mode ? BbSettings::CheatInfinite : BbSettings::CheatOff, true);
    }
    Hint(hint);
    CheatUnavailable(which);
    ImGui::PopID();
}

void CheatToggle(const char* label, std::atomic<bool>& value, BbCheats::Which which, const char* hint) {
    ImGui::PushID(label);
    Checkbox(label, value);
    Hint(hint);
    CheatUnavailable(which);
    ImGui::PopID();
}

template <std::size_t N>
void CheatLevels(const char* label, std::atomic<int>& value, const int (&levels)[N], const char* const (&names)[N],
                 BbCheats::Which which, const char* hint) {
    ImGui::PushID(label);
    const int current = value;
    int index = 0;
    for (std::size_t i = 0; i < N; ++i) {
        if (levels[i] == current) {
            index = int(i);
        }
    }
    if (ImGui::Combo(label, &index, names, int(N))) {
        Store(value, levels[index], true);
    }
    Hint(hint);
    CheatUnavailable(which);
    ImGui::PopID();
}

void CheatStats() {
    ImGui::PushID("cheat stats");
    const bool off = BbCheats::Unavailable(BbCheats::Echoes) != nullptr;
    ImGui::BeginDisabled(off);
    ImGui::TextUnformatted(BbSettings::MenuText("Blood Echoes", "Отголоски крови"));
    for (int amount : {1000, 10000, 100000}) {
        char label[32];
        std::snprintf(label, sizeof(label), "+%d,%03d", amount / 1000, 0);
        ImGui::SameLine();
        if (ImGui::Button(label)) {
            BbCheats::AddEchoes(amount);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(BbSettings::MenuText("+1 Insight", "+1 озарение"))) {
        BbCheats::AddInsight(1);
    }
    ImGui::EndDisabled();
    Hint(BbSettings::MenuText(
        "Added to the character in the world, up to the game's limits (999,999,999 Blood Echoes, 99 Insight). "
        "They are saved with your character: turning cheats off does not take them back.",
        "Добавляются персонажу в игре, до пределов игры (999 999 999 отголосков, 99 озарения). Они "
        "сохраняются вместе с персонажем: выключение читов их не отнимает."));
    CheatUnavailable(BbCheats::Echoes);
    ImGui::PopID();
}

} // namespace

/// The menu's "Cheats" section: applied at once, off online (the launcher's Cheats section sets
/// them for the next start).
void CheatsSection(BbSettings::Values& s) {
    ImGui::SeparatorText(BbSettings::MenuText("Cheats", "Читы"));
    if (BbCheats::Online()) {
        ProblemText(BbSettings::MenuText(
            "Off while playing online, so other players never meet them. Turn online off in the "
            "launcher to use them.",
            "Выключены в игре по сети, чтобы другие игроки с ними не встречались. Выключите сетевую "
            "игру в лаунчере, чтобы ими пользоваться."));
        return;
    }
    CheatMode(BbSettings::MenuText("Blood Vials", "Флаконы крови"), s.cheat_vials, BbCheats::Vials,
              BbSettings::MenuText(
                  "Infinite: a Blood Vial is not used up when you drink it (you need at least one). Applies "
                  "at once; Off restores the game's item data.",
                  "Бесконечно: флакон крови не тратится, когда вы его пьёте (нужен хотя бы один). "
                  "Применяется сразу; Выкл. возвращает данные предметов игры."));
    CheatMode(BbSettings::MenuText("Quicksilver Bullets", "Ртутные пули"), s.cheat_bullets, BbCheats::Bullets,
              BbSettings::MenuText(
                  "Infinite: firearms and the guns of trick weapons do not use up Quicksilver Bullets. Off "
                  "restores the game's weapon data.",
                  "Бесконечно: огнестрельное оружие и огнестрел трансформируемого оружия не тратят ртутные "
                  "пули. Выкл. возвращает данные оружия игры."));
    CheatToggle(BbSettings::MenuText("Infinite consumables", "Бесконечные расходники"), s.cheat_consumables,
                BbCheats::Consumables,
                BbSettings::MenuText(
                    "Molotovs, knives, pebbles, oil urns, antidotes, sedatives, elixirs, papers, Numbing Mist, "
                    "Shining Coins and similar tools are not used up. Not Blood Vials or Quicksilver Bullets "
                    "(above), key items, materials, or Insight and Blood Echo items. Off restores the item data.",
                    "Коктейли Молотова, ножи, камешки, масло, антидоты, успокоительное, эликсиры, бумага, "
                    "онемляющий туман, блестящие монеты и подобные инструменты не расходуются. Не флаконы крови и "
                    "ртутные пули (выше), не ключевые предметы, материалы, предметы озарения и отголосков. Выкл. "
                    "возвращает данные предметов."));
    CheatToggle(BbSettings::MenuText("Infinite stamina", "Бесконечная выносливость"), s.cheat_stamina,
                BbCheats::Stamina,
                BbSettings::MenuText("Your stamina stays full (enemies are not affected). Off: nothing to restore.",
                                     "Ваша выносливость всегда полная (на врагов не действует). Выкл.: "
                                     "восстанавливать нечего."));
    CheatToggle(BbSettings::MenuText("Stealth", "Скрытность"), s.cheat_stealth, BbCheats::Stealth,
                BbSettings::MenuText("Enemies neither see nor hear you: the game's debug flags Player Hide and "
                                     "Player Silence. Off puts the flags back as they were.",
                                     "Враги вас не видят и не слышат: отладочные флаги игры Player Hide и Player "
                                     "Silence. Выкл. возвращает флаги как были."));
    {
        const char* const names[] = {BbSettings::MenuText("Off", "Выкл."), "150%", "200%", "300%"};
        CheatLevels(BbSettings::MenuText("Firearm range", "Дальность огнестрела"), s.cheat_gun_range,
                    BbSettings::CheatGunRanges, names, BbCheats::GunRange,
                    BbSettings::MenuText(
                        "Bullets of the pistols, rifles, blunderbusses, cannons, Gatling Gun and the trick "
                        "weapons' guns fly farther. Not Rosmarinus, the Flamesprayer, arcane or hunter tools. "
                        "Off restores the game's bullet data.",
                        "Пули пистолетов, ружей, мушкетонов, пушек, пулемёта и огнестрела трансформируемого "
                        "оружия летят дальше. Не Розмарин, огнемёт, тайные и охотничьи инструменты. Выкл. "
                        "возвращает данные пуль игры."));
    }
    {
        const char* const names[] = {BbSettings::MenuText("x1 (off)", "x1 (выкл.)"), "x2", "x5", "x10"};
        CheatLevels(BbSettings::MenuText("Blood Echo multiplier", "Множитель отголосков"), s.cheat_echo_multiplier,
                    BbSettings::CheatEchoMultipliers, names, BbCheats::EchoMultiplier,
                    BbSettings::MenuText(
                        "Blood Echoes from enemies and bosses times this. An enemy already loaded may give the "
                        "normal amount until the area loads again. x1 restores the game's data; echoes gained "
                        "stay in your save.",
                        "Отголоски за врагов и боссов умножаются. Уже загруженный враг может дать обычное "
                        "количество, пока область не загрузится заново. x1 возвращает данные игры; полученные "
                        "отголоски остаются в сохранении."));
    }
    CheatStats();
}

} // namespace BbOverlay::detail

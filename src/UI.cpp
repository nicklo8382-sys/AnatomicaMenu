#include <string_view>
#include "UI.h"
#include "Colour.h"
#include "Drip.h"
#include "Virgin.h"
#include "Bridge.h"
#include "Extras.h"

#include <SimpleIni.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

// ---------------------------------------------------------------------------
//  Settings live in two places:
//   * the GLOB records of AnatomicaGape.esp (what the gape script reads)
//   * Data/SKSE/Plugins/AnatomicaMenu.ini (so they carry over to every save)
// ---------------------------------------------------------------------------
namespace {
    constexpr const char* kPlugin = "AnatomicaGape.esp";
    constexpr const char* kIni = "Data/SKSE/Plugins/AnatomicaMenu.ini";
    constexpr const char* kJiggleBase = "Data/SKSE/Plugins/AnatomicaMenu/JiggleBase/";
    constexpr const char* kJiggleOut = "Data/SKSE/Plugins/";

    struct GlobalSetting {
        const char* key;       // INI key
        RE::FormID id;         // local form id in AnatomicaGape.esp
        const char* label;
        float def, lo, hi;
        const char* fmt;
        const char* help;
    };

    // Same order / defaults as AnatomicaGape.esp
    GlobalSetting kGape[] = {
        {"Max", 0x801, "After sex: how open (% of full gape)", 0.8f, 0.0f, 1.0f, "pct",
         "How wide the anus stays open once sex ends, if penetration lasted long enough to fully dilate. 100% = the full Anatomica Gape shape."},
        {"During", 0x805, "During sex: gape left when the schlong pulls out (%)", 0.5f, 0.0f, 1.0f, "pct",
         "While the schlong is inside, PPA holds the ring open. When it slides out but the anal sex goes on, the anus stays this open (x how dilated it is so far), holds for the time below, then closes slowly with the closing time - so slow pull-outs close gradually instead of snapping shut. 0% = off."},
        {"Build", 0x802, "During sex: seconds of penetration for full after-sex gape", 30.0f, 2.0f, 300.0f, "%.0f s",
         "How long penetration has to last before the after-sex gape reaches its full size. Shorter sex = smaller gape afterwards."},
        {"DuringHold", 0x80C, "During sex: stays open after the schlong leaves for", 2.0f, 0.0f, 10.0f, "%.1f s",
         "When penetration pauses but the scene goes on (position change, non-anal action), the gape holds this long, then closes with the closing time below. After the scene ends the after-sex time is used instead."},
        {"Hold", 0x803, "After sex: stays fully open for", 5.0f, 1.0f, 300.0f, "%.0f s",
         "Seconds the gape stays at full size after sex before it starts closing (1 s - 5 min)."},
        {"Decay", 0x804, "After sex: time to close completely", 30.0f, 5.0f, 60.0f, "%.0f s",
         "Seconds to go from fully open to closed (one partner)."},
        {"Multi", 0x807, "After sex: open/close time multiplier per extra partner", 2.0f, 1.0f, 4.0f, "x%.2f",
         "Each extra partner in a row multiplies both the open time and the closing time by this (up to 3 extra partners)."},
        {"WinkMin", 0x80F, "After sex: shortest wait between clenches", 2.5f, 0.5f, 30.0f, "%.1f s",
         "Every so often the open anus gives a quick squeeze and slowly lets go. Clenches come at random between the shortest and longest wait."},
        {"WinkMax", 0x810, "After sex: longest wait between clenches", 7.0f, 0.5f, 60.0f, "%.1f s",
         "Clenches come at random between the shortest and longest wait. Set both the same for a steady rhythm."},
        {"Clench", 0x811, "After sex: clench strength (% it closes)", 0.4f, 0.0f, 0.9f, "pct",
         "How far each clench squeezes the gape shut (varies a little each time). 0% = no clenching."},
        {"Breath", 0x812, "After sex: breathing strength (% it pulses)", 0.15f, 0.0f, 0.6f, "pct",
         "A slow, uneven tighten-and-relax of the open anus, so it never sits frozen. 0% = still."},
        {"BreathSpeed", 0x813, "After sex: breathing speed", 1.0f, 0.25f, 3.0f, "x%.2f",
         "How fast the breathing goes. x1 = one slow tighten-and-relax about every 3-5 s; x2 = twice as fast, x0.5 = half as fast."},
        {"Radius", 0x808, "No-OStim detection: max distance of partner's schlong bones from the canal", 3.0f, 1.0f, 8.0f, "%.1f units",
         "Only used when OStim is not installed. Counts as penetration when any of the partner's schlong bones (NPC Genitals01-06) is within this distance of the canal's centre line, inside its first 8 units. The canal is located from this actor's pelvis, thigh and spine bones."},
        {"Who", 0x80E, "Who gets the gape (0 = only you, 1 = you + followers, 2 = everyone)", 2.0f, 0.0f, 2.0f, "%.0f",
         "Fewer actors = less script work. 'You + followers' uses the game's follower/teammate flag. Partners are still detected for everyone."},
        {"IdleTick", 0x809, "Detection: check interval when idle", 1.0f, 0.25f, 5.0f, "%.2f s",
         "How often the script looks for sex when nothing is happening. Higher = less script load."},
    };
    constexpr RE::FormID kEnabledId = 0x806;
    constexpr RE::FormID kWinkId = 0x80D;

    struct JigglePart {
        const char* key;
        const char* label;
        const char* file;
        float mult;
    };
    JigglePart kJiggle[] = {
        {"Chest", "Chest", "CBPConfig_Anatomica_MaleBreast.txt", 1.0f},
        {"Belly", "Belly", "CBPConfig_Anatomica_MaleBelly.txt", 1.0f},
        {"Butt", "Butt", "CBPConfig_Anatomica_MaleButt.txt", 1.0f},
    };

    float g_values[std::size(kGape)];
    bool g_enabled = true;
    bool g_wink = true;
    bool g_loaded = false;
    std::string g_status;

    RE::TESGlobal* GetGlobal(RE::FormID id) {
        auto* dh = RE::TESDataHandler::GetSingleton();
        return dh ? dh->LookupForm<RE::TESGlobal>(id, kPlugin) : nullptr;
    }

    void LoadIni() {
        for (size_t i = 0; i < std::size(kGape); ++i) g_values[i] = kGape[i].def;
        CSimpleIniA ini;
        if (ini.LoadFile(kIni) >= 0) {
            for (size_t i = 0; i < std::size(kGape); ++i)
                g_values[i] = std::clamp(static_cast<float>(ini.GetDoubleValue("Gape", kGape[i].key, kGape[i].def)),
                                         kGape[i].lo, kGape[i].hi);
            g_enabled = ini.GetBoolValue("Gape", "Enabled", true);
            g_wink = ini.GetBoolValue("Gape", "Wink", true);
            for (auto& p : kJiggle) p.mult = static_cast<float>(ini.GetDoubleValue("Jiggle", p.key, 1.0));
        }
        g_loaded = true;
    }

    void SaveIni() {
        CSimpleIniA ini;
        ini.LoadFile(kIni);
        for (size_t i = 0; i < std::size(kGape); ++i) ini.SetDoubleValue("Gape", kGape[i].key, g_values[i]);
        ini.SetBoolValue("Gape", "Enabled", g_enabled);
        ini.SetBoolValue("Gape", "Wink", g_wink);
        for (auto& p : kJiggle) ini.SetDoubleValue("Jiggle", p.key, p.mult);
        ini.SaveFile(kIni);
    }

    void PushGlobals() {
        for (size_t i = 0; i < std::size(kGape); ++i)
            if (auto* g = GetGlobal(kGape[i].id))
                g->value = (std::string_view(kGape[i].key) == "Who") ? std::round(g_values[i]) : g_values[i];
        if (auto* g = GetGlobal(kEnabledId)) g->value = g_enabled ? 1.0f : 0.0f;
        if (auto* g = GetGlobal(kWinkId)) g->value = g_wink ? 1.0f : 0.0f;
    }

    // Scale a CBPC bounce config: amplitude * m, movement ranges * max(1, sqrt(m)).
    bool WriteJiggle(const JigglePart& p) {
        std::ifstream in(std::string(kJiggleBase) + p.file);
        if (!in) return false;
        static const std::regex line_re(
            R"(^(\S+\.)(amplitude|maxoffset|[XYZ](?:max|min)offset(?:Rot)?)(\s+)(-?[\d.]+)(\s+)(-?[\d.]+)(.*)$)");
        const float m = std::max(0.0f, p.mult);
        const float range = std::max(1.0f, std::sqrt(m));
        std::ostringstream out;
        out << "# Written by the Anatomica menu (" << p.label << " x" << m << "). Edit in-game via the SKSE menu.\n";
        std::string line;
        while (std::getline(in, line)) {
            std::smatch mt;
            if (std::regex_match(line, mt, line_re)) {
                const float f = (mt[2] == "amplitude") ? m : range;
                const float a = std::stof(mt[4].str()) * f;
                const float b = std::stof(mt[6].str()) * f;
                out << mt[1] << mt[2] << mt[3] << std::format("{:.4f}", a) << mt[5] << std::format("{:.4f}", b) << mt[7] << '\n';
            } else {
                out << line << '\n';
            }
        }
        std::ofstream o(std::string(kJiggleOut) + p.file, std::ios::trunc);
        if (!o) return false;
        o << out.str();
        return true;
    }

    void HelpMarker(const char* text) {
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip("%s", text);
    }
}

void UI::ReloadSettings() {
    LoadIni();
    PushGlobals();
    logger::info("Anatomica: settings reloaded (preset)");
}

void UI::ApplySavedSettings() {
    if (!g_loaded) LoadIni();
    PushGlobals();
    logger::info("Anatomica: settings applied to {}", kPlugin);
}

void UI::Register() {
    if (!SKSEMenuFramework::IsInstalled()) {
        logger::warn("SKSE Menu Framework not installed - Anatomica menu disabled");
        return;
    }
    LoadIni();
    SKSEMenuFramework::SetSection("Anatomica");
    SKSEMenuFramework::AddSectionItem("General", Colour::RenderGeneral);
    SKSEMenuFramework::AddSectionItem("Gape", Gape::Render);
    SKSEMenuFramework::AddSectionItem("Movement", Motion::Render);
    SKSEMenuFramework::AddSectionItem("Colours", Colour::RenderColours);
    SKSEMenuFramework::AddSectionItem("Per race", Colour::RenderRaces);
    SKSEMenuFramework::AddSectionItem("Skin rules", Colour::RenderRules);
    SKSEMenuFramework::AddSectionItem("After sex", Colour::RenderAfter);
    SKSEMenuFramework::AddSectionItem("Virginity", Virgin::RenderPage);
    SKSEMenuFramework::AddSectionItem("Integrations", Bridge::RenderPage);
    if (Extras::On()) SKSEMenuFramework::AddSectionItem("Extras", Extras::RenderPage);   // only with the separate Anatomica Extras mod
    if (kCum) SKSEMenuFramework::AddSectionItem("Cum & drips", Drip::RenderPage);   // off for now (Features.h)
    SKSEMenuFramework::AddSectionItem("Status", Colour::RenderStatus);
    SKSEMenuFramework::AddSectionItem("Testing", Colour::RenderTesting);
    SKSEMenuFramework::AddSectionItem("Jiggle", Jiggle::Render);
    SKSEMenuFramework::AddSectionItem("About", About::Render);
    Colour::RegisterWindow();
}

namespace {
    bool IsMotionKey(const char* key) {
        const std::string_view k(key);
        return k == "WinkMin" || k == "WinkMax" || k == "Clench" || k == "Breath" || k == "BreathSpeed";
    }
    bool SliderFor(size_t i) {
        bool changed = false;
        if (std::string_view(kGape[i].fmt) == "pct") {
            float pct = g_values[i] * 100.0f;
            if (ImGuiMCP::SliderFloat(kGape[i].label, &pct, kGape[i].lo * 100.0f, kGape[i].hi * 100.0f, "%.0f%%")) {
                g_values[i] = pct / 100.0f;
                changed = true;
            }
        } else {
            changed |= ImGuiMCP::SliderFloat(kGape[i].label, &g_values[i], kGape[i].lo, kGape[i].hi, kGape[i].fmt);
        }
        HelpMarker(kGape[i].help);
        return changed;
    }
}

void __stdcall UI::Gape::Render() {
    if (!GetGlobal(kEnabledId)) {
        ImGuiMCP::TextWrapped("AnatomicaGape.esp is not loaded. Install the \"After-sex gape and dilation\" option and enable the plugin.");
        return;
    }
    bool changed = false;
    changed |= ImGuiMCP::Checkbox("Enable gape (after sex, and during sex if set above 0%)", &g_enabled);
    ImGuiMCP::Separator();
    for (size_t i = 0; i < std::size(kGape); ++i)
        if (!IsMotionKey(kGape[i].key)) changed |= SliderFor(i);
    ImGuiMCP::Spacing();
    if (Colour::ConfirmButton("Reset to defaults")) {
        for (size_t i = 0; i < std::size(kGape); ++i)
            if (!IsMotionKey(kGape[i].key)) g_values[i] = kGape[i].def;
        g_enabled = true;
        changed = true;
    }
    if (changed) {
        PushGlobals();
        SaveIni();
    }
    ImGuiMCP::Spacing();
    ImGuiMCP::TextDisabled("Changes apply immediately and are kept for every save.");
}

void __stdcall UI::Motion::Render() {
    if (!GetGlobal(kEnabledId)) {
        ImGuiMCP::TextWrapped("AnatomicaGape.esp is not loaded. Install the \"After-sex gape and dilation\" option and enable the plugin.");
        return;
    }
    ImGuiMCP::TextWrapped("After sex, while the anus is still gaping, it can keep moving: a slow, uneven breathing "
                          "(tighten and relax) and now and then a quick clench that lets go slowly. Both fade out as the gape closes. "
                          "Only after the scene - during sex PPA moves the anus.");
    ImGuiMCP::Separator();
    bool changed = false;
    changed |= ImGuiMCP::Checkbox("Enable after-sex movement", &g_wink);
    for (size_t i = 0; i < std::size(kGape); ++i)
        if (IsMotionKey(kGape[i].key)) changed |= SliderFor(i);
    ImGuiMCP::Spacing();
    if (Colour::ConfirmButton("Reset movement to defaults")) {
        for (size_t i = 0; i < std::size(kGape); ++i)
            if (IsMotionKey(kGape[i].key)) g_values[i] = kGape[i].def;
        g_wink = true;
        changed = true;
    }
    if (changed) {
        PushGlobals();
        SaveIni();
    }
    ImGuiMCP::Spacing();
    ImGuiMCP::TextDisabled("Changes apply immediately and are kept for every save.");
}

void __stdcall UI::Jiggle::Render() {
    ImGuiMCP::TextWrapped("Male jiggle strength for the Anatomica CBPC option (1.0 = Anatomica default, 0 = off).");
    ImGuiMCP::Separator();
    bool changed = false;
    for (auto& p : kJiggle) changed |= ImGuiMCP::SliderFloat(p.label, &p.mult, 0.0f, 3.0f, "x%.2f");
    if (ImGuiMCP::Button("Apply")) {
        int ok = 0;
        for (auto& p : kJiggle) ok += WriteJiggle(p) ? 1 : 0;
        SaveIni();
        g_status = ok == static_cast<int>(std::size(kJiggle))
                       ? "Saved. CBPC reads these files when the game starts - restart to feel the change."
                       : "Could not write the CBPC files (is the Anatomica CBPC option installed?).";
    }
    ImGuiMCP::SameLine();
    if (Colour::ConfirmButton("Reset jiggle")) {
        for (auto& p : kJiggle) p.mult = 1.0f;
        changed = true;
    }
    if (changed) g_status = "Not applied yet - press Apply.";
    if (!g_status.empty()) ImGuiMCP::TextWrapped("%s", g_status.c_str());
}

void __stdcall UI::About::Render() {
    ImGuiMCP::TextWrapped("Anatomica - anatomical anus for HIMBO.");
    ImGuiMCP::TextWrapped("The gape settings drive AnatomicaGape.esp; the body has to be built in BodySlide with \"Build Morphs\" for the gape to show.");
    ImGuiMCP::TextWrapped("Settings file: Data/SKSE/Plugins/AnatomicaMenu.ini");
}

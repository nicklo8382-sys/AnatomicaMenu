#include "UI.h"

#include <SimpleIni.h>

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
        {"Max", 0x801, "Gape after sex", 0.8f, 0.0f, 1.0f, "%.2f",
         "How far it stays open after sex at full dilation (0-1 = 0-100% of the Anatomica Gape morph)."},
        {"During", 0x805, "Gape during sex", 0.15f, 0.0f, 0.6f, "%.2f",
         "Slight visible gape while being penetrated. 0 = none."},
        {"Build", 0x802, "Seconds to fully dilate", 30.0f, 2.0f, 300.0f, "%.0f s",
         "How long penetration has to last before the anus is fully dilated."},
        {"Hold", 0x803, "Stays open for", 10.0f, 0.0f, 300.0f, "%.0f s",
         "Seconds the gape stays fully open after sex before it starts closing."},
        {"Decay", 0x804, "Closes over", 120.0f, 5.0f, 1800.0f, "%.0f s",
         "Seconds to close again after one partner."},
        {"Multi", 0x807, "Per extra partner", 2.0f, 1.0f, 4.0f, "x%.2f",
         "The closing time is multiplied by this for every extra partner (capped at 3 extra)."},
        {"Radius", 0x808, "Detection radius", 3.0f, 1.0f, 8.0f, "%.1f",
         "How close another actor's schlong has to be to the anus to count as penetration."},
        {"IdleTick", 0x809, "Idle check interval", 1.0f, 0.25f, 5.0f, "%.2f s",
         "How often the script looks for penetration when nothing is happening. Higher = less load."},
    };
    constexpr RE::FormID kEnabledId = 0x806;

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
                g_values[i] = static_cast<float>(ini.GetDoubleValue("Gape", kGape[i].key, kGape[i].def));
            g_enabled = ini.GetBoolValue("Gape", "Enabled", true);
            for (auto& p : kJiggle) p.mult = static_cast<float>(ini.GetDoubleValue("Jiggle", p.key, 1.0));
        }
        g_loaded = true;
    }

    void SaveIni() {
        CSimpleIniA ini;
        ini.LoadFile(kIni);
        for (size_t i = 0; i < std::size(kGape); ++i) ini.SetDoubleValue("Gape", kGape[i].key, g_values[i]);
        ini.SetBoolValue("Gape", "Enabled", g_enabled);
        for (auto& p : kJiggle) ini.SetDoubleValue("Jiggle", p.key, p.mult);
        ini.SaveFile(kIni);
    }

    void PushGlobals() {
        for (size_t i = 0; i < std::size(kGape); ++i)
            if (auto* g = GetGlobal(kGape[i].id)) g->value = g_values[i];
        if (auto* g = GetGlobal(kEnabledId)) g->value = g_enabled ? 1.0f : 0.0f;
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
    SKSEMenuFramework::AddSectionItem("Gape", Gape::Render);
    SKSEMenuFramework::AddSectionItem("Jiggle", Jiggle::Render);
    SKSEMenuFramework::AddSectionItem("About", About::Render);
}

void __stdcall UI::Gape::Render() {
    if (!GetGlobal(kEnabledId)) {
        ImGuiMCP::TextWrapped("AnatomicaGape.esp is not loaded. Install the \"After-sex gape and dilation\" option and enable the plugin.");
        return;
    }
    bool changed = false;
    changed |= ImGuiMCP::Checkbox("Enable after-sex gape", &g_enabled);
    ImGuiMCP::Separator();
    for (size_t i = 0; i < std::size(kGape); ++i) {
        changed |= ImGuiMCP::SliderFloat(kGape[i].label, &g_values[i], kGape[i].lo, kGape[i].hi, kGape[i].fmt);
        HelpMarker(kGape[i].help);
    }
    ImGuiMCP::Spacing();
    if (ImGuiMCP::Button("Reset to defaults")) {
        for (size_t i = 0; i < std::size(kGape); ++i) g_values[i] = kGape[i].def;
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
    if (ImGuiMCP::Button("Reset")) {
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

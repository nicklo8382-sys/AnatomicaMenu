#include "Colour.h"
#include "SKSEMenuFramework.h"

#include <SimpleIni.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
//  Anus colour, skin-blend overlay and after-sex looks, per character.
//
//  The DLL decides what each man should look like (menu settings: everyone / you / per race /
//  random, strength, wetness, tender reddening). The gape script calls
//  AnatomicaNative.LookFor(actor) every few seconds and applies the answer with RaceMenu
//  (NiOverride) overrides on the "3BA_Anus" shape and on the RaceMenu overlay slot that holds
//  anatomica_blend. Only CHANGES are sent, so a man who already looks right costs one native call.
//
//  Two methods:
//   * Tint (default, experimental): one neutral (grey) anus texture, coloured per actor with the
//     shader tint = his skin tone x the picked colour. Any colour, any strength (0% = his skin).
//   * Textures: the five pre-made colour textures (always works; no strength / custom colour).
//
//  Textures ship in 00 Core: textures\actors\character\male\Anatomica\colours\*.dds
// ---------------------------------------------------------------------------
namespace {
    constexpr const char* kIni = "Data/SKSE/Plugins/AnatomicaMenu.ini";
    constexpr const char* kDir = "textures\\actors\\character\\male\\Anatomica\\colours\\";

    // choice values: -2 = "use the default / your race" (only for player and races),
    //                -1 = installed colour (no override), 0..4 = preset, 5 = custom, 6 = random,
    //                7 = smart random (a colour that fits his skin)
    constexpr int kUseDefault = -2, kInstalled = -1, kCustom = 5, kRandom = 6, kSmart = 7, kPresets = 5;
    // random pool bits: 0-4 presets, 5 custom, 6-11 "your colours", 12 smart
    constexpr int kPaletteSize = 6, kPaletteBit = 6, kSmartBit = 12, kPoolBits = 13;
    constexpr int kAllPool = (1 << kPoolBits) - 1;
    constexpr int kDefaultPool = 0x3F;   // the five shades + custom
    const char* kColourNames[kPresets + 1] = {"Pink", "Light pink", "Rose / deep red", "Mauve brown", "Skin matched", "Custom"};
    const char* kHarmonyItems[] = {"Same colour family as his skin", "Neighbouring colours (recommended)", "Contrasting colours"};
    const char* kColourFiles[kPresets] = {"pink.dds", "lightpink.dds", "rose.dds", "mauve.dds", "skin.dds"};
    constexpr int kSkinPreset = 4;

    // average colour (0-255) of each pre-made anus texture and of its skin-blend overlay
    // (measured from the shipped textures - the tint method reproduces these)
    constexpr float kBlendRGB[kPresets][3] = {
        {233.5f, 148.3f, 157.4f}, {241.7f, 178.0f, 194.7f}, {192.6f, 99.5f, 110.6f}, {168.1f, 122.9f, 122.4f}, {227.2f, 190.6f, 176.1f}};
    // average colour (0-255) of each preset on the rim (the outer face, 3BA_AnusRim)
    constexpr float kRimRGB[kPresets][3] = {
        {142.3f, 86.9f, 92.1f}, {173.3f, 126.3f, 138.1f}, {115.4f, 55.6f, 63.5f}, {99.7f, 72.4f, 69.9f}, {139.1f, 113.1f, 103.8f}};

    // ---- generated rim textures --------------------------------------------------------------
    // Any colour / strength / reddening is baked into a small DXT1 texture (rim detail x colour),
    // written once to Data\textures\...\Anatomica\gen\ and reused. The game then applies the
    // character's skin tone on top (the rim shape is skin-tinted).
    constexpr const char* kGenDir = "textures\\actors\\character\\male\\Anatomica\\gen\\";
    std::vector<std::uint8_t> g_detail;   // 512x512, 128 = 1.0
    int g_detailW = 0, g_detailH = 0;
    bool g_detailTried = false;

    bool LoadDetail() {
        if (g_detailTried) return !g_detail.empty();
        g_detailTried = true;
        std::ifstream f("Data/SKSE/Plugins/AnatomicaMenu/rim_detail.bin", std::ios::binary);
        char magic[4]{};
        std::uint32_t w = 0, h = 0;
        if (!f.read(magic, 4) || std::string_view(magic, 4) != "ARD1") return false;
        f.read(reinterpret_cast<char*>(&w), 4);
        f.read(reinterpret_cast<char*>(&h), 4);
        if (w == 0 || h == 0 || w > 4096 || h > 4096 || (w & 3) || (h & 3)) return false;
        g_detail.resize(static_cast<size_t>(w) * h);
        if (!f.read(reinterpret_cast<char*>(g_detail.data()), static_cast<std::streamsize>(g_detail.size()))) {
            g_detail.clear();
            return false;
        }
        g_detailW = static_cast<int>(w);
        g_detailH = static_cast<int>(h);
        return true;
    }

    std::uint16_t To565(float r, float gg, float b) {
        auto q = [](float v, int bits) { return static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * ((1 << bits) - 1))); };
        return static_cast<std::uint16_t>((q(r, 5) << 11) | (q(gg, 6) << 5) | q(b, 5));
    }

    // one DXT1 mip level of (colour x detail): every 4x4 block is a line between two shades of
    // the same colour, which DXT1 stores exactly
    void EncodeLevel(const std::vector<float>& lum, int w, int h, const float c[3], std::string& out) {
        const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                float l[16], lo = 1e9f, hi = -1e9f;
                for (int k = 0; k < 16; ++k) {
                    const int x = std::min(bx * 4 + (k & 3), w - 1), y = std::min(by * 4 + (k >> 2), h - 1);
                    l[k] = lum[static_cast<size_t>(y) * w + x];
                    lo = std::min(lo, l[k]);
                    hi = std::max(hi, l[k]);
                }
                std::uint16_t c0 = To565(c[0] * hi, c[1] * hi, c[2] * hi), c1 = To565(c[0] * lo, c[1] * lo, c[2] * lo);
                std::uint32_t idx = 0;
                if (c0 != c1) {
                    bool swapped = false;
                    if (c0 < c1) { std::swap(c0, c1); swapped = true; }
                    for (int k = 0; k < 16; ++k) {
                        float t = hi > lo ? (l[k] - lo) / (hi - lo) : 1.0f;   // 1 = bright end
                        if (swapped) t = 1.0f - t;
                        // palette order: 0 = c0 (t 1), 2 = 2/3, 3 = 1/3, 1 = c1 (t 0)
                        const std::uint32_t code = t > 0.833f ? 0u : t > 0.5f ? 2u : t > 0.167f ? 3u : 1u;
                        idx |= code << (2 * k);
                    }
                } else if (c0 == 0 && c1 == 0) {
                    c0 = 1;   // keep 4-colour mode (c0 > c1): index 1 = c1 = black
                    idx = 0x55555555u;
                } else {
                    c1 = static_cast<std::uint16_t>(c0 - 1);   // c0 > c1 keeps 4-colour mode
                }
                out.append(reinterpret_cast<const char*>(&c0), 2);
                out.append(reinterpret_cast<const char*>(&c1), 2);
                out.append(reinterpret_cast<const char*>(&idx), 4);
            }
        }
    }

    // colour 0-1 (before the skin tone) -> texture path, written on first use
    std::string GenRimTexture(const float colour[3]) {
        if (!LoadDetail()) return {};
        int q[3];
        for (int i = 0; i < 3; ++i) q[i] = std::clamp(static_cast<int>(std::lround(colour[i] * 31.0f)), 0, 31);   // 32 steps
        const std::string rel = std::format("{}rim_{:02d}{:02d}{:02d}.dds", kGenDir, q[0], q[1], q[2]);
        const std::filesystem::path file = std::filesystem::path("Data") / std::filesystem::path(std::string(rel));
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) return rel;
        const float c[3] = {q[0] / 31.0f, q[1] / 31.0f, q[2] / 31.0f};
        int w = g_detailW, h = g_detailH, levels = 1;
        while ((w >> levels) >= 1 && (h >> levels) >= 1) ++levels;
        std::string data;
        std::vector<float> lum(g_detail.size());
        for (size_t i = 0; i < lum.size(); ++i) lum[i] = g_detail[i] / 128.0f;
        for (int lv = 0; lv < levels; ++lv) {
            EncodeLevel(lum, w, h, c, data);
            if (w == 1 && h == 1) break;
            const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
            std::vector<float> nl(static_cast<size_t>(nw) * nh);
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x) {
                    const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1), y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
                    nl[static_cast<size_t>(y) * nw + x] = 0.25f * (lum[static_cast<size_t>(y0) * w + x0] + lum[static_cast<size_t>(y0) * w + x1] +
                                                                   lum[static_cast<size_t>(y1) * w + x0] + lum[static_cast<size_t>(y1) * w + x1]);
                }
            lum.swap(nl);
            w = nw;
            h = nh;
        }
        // DDS header (DXT1, mipmapped)
        std::uint32_t hdr[32]{};
        hdr[0] = 0x20534444u;                        // "DDS "
        hdr[1] = 124;                                 // header size
        hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000;   // caps|height|width|pixelformat|mipmapcount|linearsize
        hdr[3] = static_cast<std::uint32_t>(g_detailH);
        hdr[4] = static_cast<std::uint32_t>(g_detailW);
        hdr[5] = static_cast<std::uint32_t>(std::max(1, g_detailW / 4) * std::max(1, g_detailH / 4) * 8);
        hdr[7] = static_cast<std::uint32_t>(levels);
        hdr[19] = 32;                                 // pixel format size
        hdr[20] = 0x4;                                // DDPF_FOURCC
        hdr[21] = 0x31545844u;                        // "DXT1"
        hdr[27] = 0x1000 | 0x8 | 0x400000;            // texture | complex | mipmap
        std::filesystem::create_directories(file.parent_path(), ec);
        const auto tmp = std::filesystem::path(file).concat(".tmp");
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            if (!o) return {};
            o.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
            o.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!o) return {};
        }
        std::filesystem::rename(tmp, file, ec);
        if (ec) return {};
        logger::info("Anatomica colour: made {}", rel);
        return rel;
    }

    const char* kDefaultItems[] = {"Installed colour (no change)", "Pink", "Light pink", "Rose / deep red",
                                   "Mauve brown", "Skin matched", "Custom", "Random", "Smart random (fits his skin)"};
    const char* kRaceItems[] = {"Use the default", "Installed colour (no change)", "Pink", "Light pink",
                                "Rose / deep red", "Mauve brown", "Skin matched", "Custom", "Random",
                                "Smart random (fits his skin)"};
    const char* kPlayerItems[] = {"Same as your race", "Installed colour (no change)", "Pink", "Light pink",
                                  "Rose / deep red", "Mauve brown", "Skin matched", "Custom", "Random",
                                  "Smart random (fits his skin)"};
    const char* kScopeItems[] = {"Everyone", "You and your followers", "Only you"};
    const char* kMethodItems[] = {"Any colour + strength", "Only the 5 fixed shades"};

    struct Settings {
        bool enabled = true;
        float interval = 3.0f;
        int scope = 0;
        int method = 0;              // 0 tint, 1 textures
        float strength = 1.0f;
        bool randStrength = false;   // each man gets a random strength between strength and strengthMax
        float strengthMax = 1.0f;
        float custom[3] = {0.55f, 0.30f, 0.32f};
        bool blendMatch = true;
        bool autoRing = true;        // give NPCs the skin-blend ring if they don't have it
        int def = kInstalled;
        int player = kUseDefault;
        int pool = kDefaultPool;
        float palette[kPaletteSize][3] = {{0.45f, 0.22f, 0.30f}, {0.60f, 0.35f, 0.38f}, {0.38f, 0.26f, 0.22f},
                                          {0.50f, 0.28f, 0.45f}, {0.30f, 0.32f, 0.40f}, {0.55f, 0.40f, 0.30f}};
        int seed = 0;               // "re-roll everyone" bumps this
        int harmony = 1;            // smart random: 0 same family, 1 neighbouring, 2 contrasting
        // after sex
        bool wet = true;
        float wetStrength = 0.8f;
        float wetTime = 180.0f;
        bool tender = true;
        float tenderStrength = 0.6f;
        float tenderTime = 600.0f;
        bool tenderBlend = true;
    };

    std::mutex g_lock;
    bool g_loaded = false;
    Settings g;
    struct RaceSet {
        int choice = kUseDefault;
        float rgb[3] = {0.55f, 0.30f, 0.32f};   // this race's own Custom colour
        float strength = -1.0f;                 // own colour strength (from), < 0 = use the Colours page one
        float strengthMax = -1.0f;              // own strength (to); same as from = fixed
        int pool = -1;                          // own random colours (bits as the Colours page), -1 = use those
        float palette[6][3] = {{0.45f, 0.22f, 0.30f}, {0.60f, 0.35f, 0.38f}, {0.38f, 0.26f, 0.22f},
                               {0.50f, 0.28f, 0.45f}, {0.30f, 0.32f, 0.40f}, {0.55f, 0.40f, 0.30f}};
    };
    std::map<std::string, RaceSet> g_races;    // "Plugin.esp|00ABCD" -> settings
    int g_installed = -2;                       // installer colour (preset index), -2 = not checked yet
    std::map<std::string, int> g_raceDefaults;  // race picks made in the installer (SKSE/Plugins/AnatomicaMenu/Races/*.ini)
    std::map<std::string, int> g_reroll;        // "Plugin|refID" -> times re-rolled
    constexpr const char* kRaceDefaultsDir = "Data/SKSE/Plugins/AnatomicaMenu/Races";

    int RaceDefault(const std::string& key) {
        auto it = g_raceDefaults.find(key);
        return it == g_raceDefaults.end() ? kUseDefault : it->second;
    }

    struct RaceRow {
        std::string key;
        std::string name;    // unique ImGui label
        std::string plain;   // shown in lists
    };
    // skin rules (Smart random): a man whose skin looks like `skin` (hue within +-tol) gets `anus`
    struct Rule {
        bool on = true;
        std::string race;            // "" = all races
        float skin[3] = {0.60f, 0.25f, 0.20f};
        float tol = 35.0f;           // degrees on the colour wheel
        float anus[3] = {0.25f, 0.30f, 0.55f};
    };
    constexpr int kMaxRules = 16;
    std::vector<Rule> g_rules;
    std::vector<RaceRow> g_raceRows;
    std::map<std::string, bool> g_raceTouched;  // race colour picked this session (don't overwrite)
    constexpr int kWheel = ImGuiMCP::ImGuiColorEditFlags_PickerHueWheel;

    // what was last sent to the script for each actor (only changes are sent)
    struct Sent {
        std::string tex;   // "" = no texture override
        int tint = -1, blend = -1, swap = 0, gloss = -1, spec = -1, add = -1;
    };
    std::unordered_map<RE::FormID, Sent> g_sent;
    std::unordered_map<RE::FormID, std::string> g_pendingTex;

    // after-sex: how worked each actor was and when
    struct Work {
        float peak = 0.0f;
        double last = 0.0;
    };
    std::unordered_map<RE::FormID, Work> g_work;

    double Now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    std::string RaceKey(const RE::TESRace* race) {
        if (!race) return {};
        const auto* file = race->GetFile(0);
        const std::string plugin = file ? std::string(file->GetFilename()) : std::string("?");
        return std::format("{}|{:06X}", plugin, race->GetLocalFormID());
    }

    float GetF(CSimpleIniA& ini, const char* sec, const char* key, float def) {
        return static_cast<float>(ini.GetDoubleValue(sec, key, def));
    }

    void LoadRaceDefaults() {
        g_raceDefaults.clear();
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(kRaceDefaultsDir, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".ini") continue;
            CSimpleIniA f;
            if (f.LoadFile(e.path().string().c_str()) < 0) continue;
            CSimpleIniA::TNamesDepend keys;
            f.GetAllKeys("ColourRaces", keys);
            for (const auto& k : keys) g_raceDefaults[k.pItem] = static_cast<int>(f.GetLongValue("ColourRaces", k.pItem, kUseDefault));
        }
        for (const auto& [k, v] : g_raceDefaults) g_races[k].choice = std::clamp(v, kUseDefault, kSmart);
        if (!g_raceDefaults.empty()) logger::info("Anatomica colour: {} race picks from the installer", g_raceDefaults.size());
    }

    void LoadIni() {
        LoadRaceDefaults();
        CSimpleIniA ini;
        if (ini.LoadFile(kIni) >= 0) {
            const long ver = ini.GetLongValue("Colour", "Version", 1);
            g.enabled = ini.GetBoolValue("Colour", "Enabled", g.enabled);
            g.interval = GetF(ini, "Colour", "Interval", g.interval);
            g.scope = static_cast<int>(ini.GetLongValue("Colour", "Scope", g.scope));
            g.method = static_cast<int>(ini.GetLongValue("Colour", "Method", g.method));
            g.strength = GetF(ini, "Colour", "Strength", g.strength);
            g.randStrength = ini.GetBoolValue("Colour", "RandomStrength", g.randStrength);
            g.strengthMax = GetF(ini, "Colour", "StrengthMax", g.strengthMax);
            g.custom[0] = GetF(ini, "Colour", "CustomR", g.custom[0]);
            g.custom[1] = GetF(ini, "Colour", "CustomG", g.custom[1]);
            g.custom[2] = GetF(ini, "Colour", "CustomB", g.custom[2]);
            g.blendMatch = ini.GetBoolValue("Colour", "BlendMatch", g.blendMatch);
            g.autoRing = ini.GetBoolValue("Colour", "AutoRing", g.autoRing);
            g.def = static_cast<int>(ini.GetLongValue("Colour", "Default", kInstalled));
            g.player = static_cast<int>(ini.GetLongValue("Colour", "Player", kUseDefault));
            g.pool = static_cast<int>(ini.GetLongValue("Colour", "RandomPool", kDefaultPool));
            for (int i = 0; i < kPaletteSize; ++i) {
                const char* v = ini.GetValue("Colour", std::format("Palette{}", i).c_str(), nullptr);
                if (v) std::sscanf(v, "%f,%f,%f", &g.palette[i][0], &g.palette[i][1], &g.palette[i][2]);
                for (auto& c : g.palette[i]) c = std::clamp(c, 0.0f, 1.0f);
            }
            g.seed = static_cast<int>(ini.GetLongValue("Colour", "Seed", 0));
            g.harmony = static_cast<int>(ini.GetLongValue("Colour", "Harmony", g.harmony));
            CSimpleIniA::TNamesDepend keys;
            ini.GetAllKeys("ColourRaces", keys);
            for (const auto& k : keys) g_races[k.pItem].choice = static_cast<int>(ini.GetLongValue("ColourRaces", k.pItem, kUseDefault));
            keys.clear();
            ini.GetAllKeys("ColourRaceRGB", keys);
            for (const auto& k : keys) {
                auto& r = g_races[k.pItem];
                const char* v = ini.GetValue("ColourRaceRGB", k.pItem, "");
                std::sscanf(v, "%f,%f,%f", &r.rgb[0], &r.rgb[1], &r.rgb[2]);
                for (auto& c : r.rgb) c = std::clamp(c, 0.0f, 1.0f);
            }
            keys.clear();
            ini.GetAllKeys("ColourRaceStrength", keys);
            for (const auto& k : keys)
                g_races[k.pItem].strength = std::clamp(GetF(ini, "ColourRaceStrength", k.pItem, -1.0f), -1.0f, 1.0f);
            keys.clear();
            ini.GetAllKeys("ColourRaceStrengthMax", keys);
            for (const auto& k : keys)
                g_races[k.pItem].strengthMax = std::clamp(GetF(ini, "ColourRaceStrengthMax", k.pItem, -1.0f), -1.0f, 1.0f);
            keys.clear();
            ini.GetAllKeys("ColourRacePool", keys);
            for (const auto& k : keys) g_races[k.pItem].pool = static_cast<int>(ini.GetLongValue("ColourRacePool", k.pItem, -1));
            keys.clear();
            ini.GetAllKeys("ColourRacePalette", keys);
            for (const auto& k : keys) {
                auto& r = g_races[k.pItem];
                const char* v = ini.GetValue("ColourRacePalette", k.pItem, "");
                float* f = &r.palette[0][0];
                std::sscanf(v, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", f, f + 1, f + 2, f + 3, f + 4, f + 5, f + 6,
                            f + 7, f + 8, f + 9, f + 10, f + 11, f + 12, f + 13, f + 14, f + 15, f + 16, f + 17);
                for (int i = 0; i < 18; ++i) f[i] = std::clamp(f[i], 0.0f, 1.0f);
            }
            g_rules.clear();
            const int nr = std::clamp(static_cast<int>(ini.GetLongValue("ColourRules", "Count", 0)), 0, kMaxRules);
            for (int i = 0; i < nr; ++i) {
                const char* v = ini.GetValue("ColourRules", std::format("Rule{}", i).c_str(), nullptr);
                if (!v) continue;
                // on;race;r,g,b;tol;r,g,b
                std::string str(v);
                std::vector<std::string> parts;
                size_t pos = 0;
                for (size_t q; (q = str.find(';', pos)) != std::string::npos; pos = q + 1) parts.push_back(str.substr(pos, q - pos));
                parts.push_back(str.substr(pos));
                if (parts.size() != 5) continue;
                Rule r;
                r.on = parts[0] == "1";
                r.race = parts[1];
                std::sscanf(parts[2].c_str(), "%f,%f,%f", &r.skin[0], &r.skin[1], &r.skin[2]);
                r.tol = std::clamp(static_cast<float>(std::atof(parts[3].c_str())), 5.0f, 90.0f);
                std::sscanf(parts[4].c_str(), "%f,%f,%f", &r.anus[0], &r.anus[1], &r.anus[2]);
                g_rules.push_back(r);
            }
            keys.clear();
            ini.GetAllKeys("ColourReroll", keys);
            for (const auto& k : keys) g_reroll[k.pItem] = static_cast<int>(ini.GetLongValue("ColourReroll", k.pItem, 0));
            if (ver < 2) {   // 2.2 test builds used 5 = random
                auto fix = [](int& v) { if (v == 5) v = kRandom; };
                fix(g.def);
                fix(g.player);
                for (auto& [k, v] : g_races) fix(v.choice);
            }
            g.wet = ini.GetBoolValue("AfterSex", "Wet", g.wet);
            g.wetStrength = GetF(ini, "AfterSex", "WetStrength", g.wetStrength);
            g.wetTime = GetF(ini, "AfterSex", "WetTime", g.wetTime);
            g.tender = ini.GetBoolValue("AfterSex", "Tender", g.tender);
            g.tenderStrength = GetF(ini, "AfterSex", "TenderStrength", g.tenderStrength);
            g.tenderTime = GetF(ini, "AfterSex", "TenderTime", g.tenderTime);
            g.tenderBlend = ini.GetBoolValue("AfterSex", "TenderBlend", g.tenderBlend);
        }
        g.interval = std::clamp(g.interval, 1.0f, 180.0f);
        g.harmony = std::clamp(g.harmony, 0, 2);
        for (auto& [k, v] : g_races) v.choice = std::clamp(v.choice, kUseDefault, kSmart);
        g.scope = std::clamp(g.scope, 0, 2);
        g.method = std::clamp(g.method, 0, 1);
        g.strength = std::clamp(g.strength, 0.0f, 1.0f);
        g.strengthMax = std::clamp(g.strengthMax, 0.0f, 1.0f);
        for (auto& c : g.custom) c = std::clamp(c, 0.0f, 1.0f);
        g.def = std::clamp(g.def, kInstalled, kSmart);
        g.player = std::clamp(g.player, kUseDefault, kSmart);
        g.pool &= kAllPool;
        if (g.pool == 0) g.pool = kDefaultPool;
        g.wetStrength = std::clamp(g.wetStrength, 0.0f, 1.0f);
        g.wetTime = std::clamp(g.wetTime, 10.0f, 3600.0f);
        g.tenderStrength = std::clamp(g.tenderStrength, 0.0f, 1.0f);
        g.tenderTime = std::clamp(g.tenderTime, 10.0f, 3600.0f);
        g_loaded = true;
    }

    void SaveIni() {
        CSimpleIniA ini;
        ini.LoadFile(kIni);
        ini.SetLongValue("Colour", "Version", 2);
        ini.SetBoolValue("Colour", "Enabled", g.enabled);
        ini.SetDoubleValue("Colour", "Interval", g.interval);
        ini.SetLongValue("Colour", "Scope", g.scope);
        ini.SetLongValue("Colour", "Method", g.method);
        ini.SetDoubleValue("Colour", "Strength", g.strength);
        ini.SetBoolValue("Colour", "RandomStrength", g.randStrength);
        ini.SetDoubleValue("Colour", "StrengthMax", g.strengthMax);
        ini.SetDoubleValue("Colour", "CustomR", g.custom[0]);
        ini.SetDoubleValue("Colour", "CustomG", g.custom[1]);
        ini.SetDoubleValue("Colour", "CustomB", g.custom[2]);
        ini.SetBoolValue("Colour", "BlendMatch", g.blendMatch);
        ini.SetBoolValue("Colour", "AutoRing", g.autoRing);
        ini.SetLongValue("Colour", "Default", g.def);
        ini.SetLongValue("Colour", "Player", g.player);
        ini.SetLongValue("Colour", "RandomPool", g.pool);
        for (int i = 0; i < kPaletteSize; ++i)
            ini.SetValue("Colour", std::format("Palette{}", i).c_str(),
                         std::format("{:.3f},{:.3f},{:.3f}", g.palette[i][0], g.palette[i][1], g.palette[i][2]).c_str());
        ini.SetLongValue("Colour", "Seed", g.seed);
        ini.SetLongValue("Colour", "Harmony", g.harmony);
        ini.Delete("ColourReroll", nullptr);
        for (const auto& [k, v] : g_reroll)
            if (v) ini.SetLongValue("ColourReroll", k.c_str(), v);
        ini.Delete("ColourRaces", nullptr);
        ini.Delete("ColourRaceRGB", nullptr);
        ini.Delete("ColourRaceStrength", nullptr);
        ini.Delete("ColourRaceStrengthMax", nullptr);
        ini.Delete("ColourRacePool", nullptr);
        ini.Delete("ColourRacePalette", nullptr);
        ini.Delete("ColourRules", nullptr);
        ini.SetLongValue("ColourRules", "Count", static_cast<long>(g_rules.size()));
        for (size_t i = 0; i < g_rules.size(); ++i) {
            const auto& r = g_rules[i];
            ini.SetValue("ColourRules", std::format("Rule{}", i).c_str(),
                         std::format("{};{};{:.3f},{:.3f},{:.3f};{:.0f};{:.3f},{:.3f},{:.3f}", r.on ? 1 : 0, r.race, r.skin[0], r.skin[1],
                                     r.skin[2], r.tol, r.anus[0], r.anus[1], r.anus[2]).c_str());
        }
        for (const auto& [k, v] : g_races) {
            if (v.choice != RaceDefault(k)) ini.SetLongValue("ColourRaces", k.c_str(), v.choice);   // only what differs from the installer
            if (v.choice == kCustom)
                ini.SetValue("ColourRaceRGB", k.c_str(), std::format("{:.3f},{:.3f},{:.3f}", v.rgb[0], v.rgb[1], v.rgb[2]).c_str());
            if (v.strength >= 0.0f) ini.SetDoubleValue("ColourRaceStrength", k.c_str(), v.strength);
            if (v.strength >= 0.0f && v.strengthMax >= 0.0f) ini.SetDoubleValue("ColourRaceStrengthMax", k.c_str(), v.strengthMax);
            if (v.pool >= 0) {
                ini.SetLongValue("ColourRacePool", k.c_str(), v.pool);
                std::string pal;
                for (int i = 0; i < 6; ++i)
                    pal += std::format("{}{:.3f},{:.3f},{:.3f}", i ? "," : "", v.palette[i][0], v.palette[i][1], v.palette[i][2]);
                ini.SetValue("ColourRacePalette", k.c_str(), pal.c_str());
            }
        }
        ini.SetBoolValue("AfterSex", "Wet", g.wet);
        ini.SetDoubleValue("AfterSex", "WetStrength", g.wetStrength);
        ini.SetDoubleValue("AfterSex", "WetTime", g.wetTime);
        ini.SetBoolValue("AfterSex", "Tender", g.tender);
        ini.SetDoubleValue("AfterSex", "TenderStrength", g.tenderStrength);
        ini.SetDoubleValue("AfterSex", "TenderTime", g.tenderTime);
        ini.SetBoolValue("AfterSex", "TenderBlend", g.tenderBlend);
        ini.SaveFile(kIni);
    }

    void BuildRaces() {
        if (!g_raceRows.empty()) return;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;
        for (auto* race : dh->GetFormArray<RE::TESRace>()) {
            if (!race || !race->data.flags.any(RE::RACE_DATA::Flag::kPlayable)) continue;
            std::string name = race->GetFullName();
            if (name.empty()) name = RaceKey(race);
            g_raceRows.push_back({RaceKey(race), name + "##" + RaceKey(race), name});
        }
        std::sort(g_raceRows.begin(), g_raceRows.end(), [](const RaceRow& a, const RaceRow& b) { return a.name < b.name; });
    }

    std::string RefKey(const RE::TESObjectREFR* r) {
        if (!r) return {};
        if ((r->GetFormID() >> 24) == 0xFF) return std::format("FF|{:08X}", r->GetFormID());
        const auto* file = r->GetFile(0);
        const std::string plugin = file ? std::string(file->GetFilename()) : std::string("?");
        return std::format("{}|{:06X}", plugin, r->GetLocalFormID());
    }

    // stable per-actor random numbers (change with "re-roll")
    std::uint32_t Hash(RE::Actor* a, std::uint32_t salt) {
        std::uint32_t h = a->GetFormID() * 2654435761u;
        h ^= static_cast<std::uint32_t>(g.seed) * 0x9E3779B9u;
        if (auto it = g_reroll.find(RefKey(a)); it != g_reroll.end()) h ^= static_cast<std::uint32_t>(it->second) * 0x85EBCA6Bu;
        h ^= salt * 0xC2B2AE35u;
        h ^= h >> 16;
        h *= 0x7FEB352Du;
        h ^= h >> 15;
        h *= 0x846CA68Bu;
        h ^= h >> 16;
        return h;
    }
    float Rand01(RE::Actor* a, std::uint32_t salt) { return (Hash(a, salt) & 0xFFFFFF) / 16777216.0f; }

    // pool entry: 0-4 preset, 5 custom, 6-11 your colours, 12 smart
    int RandomFor(RE::Actor* a, int bits) {
        std::vector<int> pool;
        for (int i = 0; i < kPoolBits; ++i)
            if (bits & (1 << i)) pool.push_back(i);
        if (pool.empty()) return 0;
        return pool[Hash(a, 1) % pool.size()];
    }

    void RGBtoHSV(const float c[3], float& h, float& s, float& v) {
        const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]}), d = mx - mn;
        v = mx;
        s = mx > 0.0f ? d / mx : 0.0f;
        if (d <= 0.0f) { h = 0.0f; return; }
        if (mx == c[0]) h = 60.0f * std::fmod((c[1] - c[2]) / d, 6.0f);
        else if (mx == c[1]) h = 60.0f * ((c[2] - c[0]) / d + 2.0f);
        else h = 60.0f * ((c[0] - c[1]) / d + 4.0f);
        if (h < 0.0f) h += 360.0f;
    }
    void HSVtoRGB(float h, float s, float v, float out[3]) {
        h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f);
        const float c = v * s, x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
        float r = 0, gg = 0, b = 0;
        if (h < 60) { r = c; gg = x; } else if (h < 120) { r = x; gg = c; } else if (h < 180) { gg = c; b = x; }
        else if (h < 240) { gg = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
        out[0] = r + m; out[1] = gg + m; out[2] = b + m;
    }

    void SkinTone(RE::Actor* a, float out[3]);

    // smart random: a colour built around his skin colour, so a red-scaled argonian doesn't get a green anus
    float HueDist(float a, float b) {
        float d = std::fabs(a - b);
        return d > 180.0f ? 360.0f - d : d;
    }

    // skin rules: every enabled rule for his race (or all races) whose skin colour matches his
    bool RuleFor(RE::Actor* a, const float skin[3], float out[3]) {
        if (g_rules.empty()) return false;
        const std::string rk = RaceKey(a->GetRace());
        float h, s, v;
        RGBtoHSV(skin, h, s, v);
        std::vector<const Rule*> hits;
        for (const auto& r : g_rules) {
            if (!r.on || (!r.race.empty() && r.race != rk)) continue;
            float rh, rs, rv;
            RGBtoHSV(r.skin, rh, rs, rv);
            bool match;
            if (rs < 0.12f) match = s < 0.18f && std::fabs(v - rv) < 0.3f;    // greyish / plain skin rule
            else match = s >= 0.08f && HueDist(h, rh) <= r.tol;
            if (match) hits.push_back(&r);
        }
        if (hits.empty()) return false;
        const Rule* r = hits[Hash(a, 31) % hits.size()];
        std::copy(std::begin(r->anus), std::end(r->anus), out);
        return true;
    }

    void SmartFor(RE::Actor* a, float out[3]) {
        float skin[3], h, sat, v;
        SkinTone(a, skin);
        if (RuleFor(a, skin, out)) return;
        RGBtoHSV(skin, h, sat, v);
        if (sat < 0.08f) h = 350.0f;   // no real skin colour (plain tint): start from pink
        const float r1 = Rand01(a, 11), r2 = Rand01(a, 12), r3 = Rand01(a, 13), sign = Rand01(a, 14) < 0.5f ? -1.0f : 1.0f;
        float off = 0.0f;
        switch (g.harmony) {
        case 0: off = (r1 * 2.0f - 1.0f) * 15.0f; break;                 // same family
        case 1: off = sign * (12.0f + r1 * 33.0f); break;                // neighbouring (analogous)
        default: off = 180.0f + (r1 * 2.0f - 1.0f) * 40.0f; break;       // contrasting (complementary side)
        }
        HSVtoRGB(h + off, 0.28f + r2 * 0.32f, 0.36f + r3 * 0.20f, out);
    }

    bool InScope(RE::Actor* a) {
        if (a->IsPlayerRef()) return true;
        if (g.scope == 2) return false;
        if (g.scope == 1) return a->IsPlayerTeammate();
        return true;
    }

    // which installer colour is in Data: compare the installed anus texture with the colour textures
    int InstalledPreset() {
        if (g_installed != -2) return g_installed;
        g_installed = 0;   // pink unless we can tell otherwise
        auto sample = [](const std::string& path, std::string& out) {
            std::ifstream f("Data/" + path, std::ios::binary);
            if (!f) return false;
            f.seekg(4096);
            out.resize(65536);
            f.read(out.data(), static_cast<std::streamsize>(out.size()));
            return f.gcount() == static_cast<std::streamsize>(out.size());
        };
        std::string inst, other;
        if (!sample("textures\\actors\\character\\male\\Anatomica\\anatomica_rim.dds", inst)) return g_installed;
        for (int i = 0; i < kPresets; ++i) {
            if (sample(std::string(kDir) + kColourFiles[i], other) && other == inst) {
                g_installed = i;
                break;
            }
        }
        logger::info("Anatomica colour: installer colour = {}", kColourNames[g_installed]);
        return g_installed;
    }

    struct Pick {
        int choice = kInstalled;   // -1 installed, 0-4 preset, 5 = colour in rgb
        float rgb[3] = {0, 0, 0};
        float strength = 1.0f;
    };

    Pick PickFor(RE::Actor* a) {
        Pick p;
        p.strength = g.strength;
        std::copy(std::begin(g.custom), std::end(g.custom), p.rgb);
        if (!g.enabled || !InScope(a)) return p;
        int choice = kUseDefault;
        if (a->IsPlayerRef()) choice = g.player;
        const RaceSet* race = nullptr;
        if (auto it = g_races.find(RaceKey(a->GetRace())); it != g_races.end() && it->second.choice != kUseDefault) race = &it->second;
        bool raceCustom = false;
        if (choice == kUseDefault && race) {
            choice = race->choice;
            raceCustom = true;
        }
        {
            float lo = g.strength, hi = g.randStrength ? g.strengthMax : g.strength;
            if (race && race->strength >= 0.0f && !a->IsPlayerRef()) {
                lo = race->strength;
                hi = race->strengthMax >= 0.0f ? race->strengthMax : race->strength;
            }
            if (hi < lo) std::swap(lo, hi);
            p.strength = lo + (hi - lo) * Rand01(a, 21);   // random strength, fixed per man
        }
        if (choice == kUseDefault) choice = g.def;
        if (choice == kCustom && raceCustom) std::copy(std::begin(race->rgb), std::end(race->rgb), p.rgb);
        if (choice == kRandom) {
            const bool racePool = raceCustom && race->choice == kRandom && race->pool > 0;
            const int e = RandomFor(a, racePool ? race->pool : g.pool);
            if (e < kPresets) {
                choice = e;
            } else if (e == kCustom) {
                choice = kCustom;
                if (raceCustom) std::copy(std::begin(race->rgb), std::end(race->rgb), p.rgb);
            } else if (e < kSmartBit) {
                choice = kCustom;
                const float* c = racePool ? race->palette[e - kPaletteBit] : g.palette[e - kPaletteBit];
                std::copy(c, c + 3, p.rgb);
            } else {
                choice = kSmart;
            }
        }
        if (choice == kSmart) {
            SmartFor(a, p.rgb);
            choice = kCustom;
        }
        p.choice = std::clamp(choice, kInstalled, kCustom);
        return p;
    }

    // the actor's skin tone (0-1) - what the game multiplies his skin (and the anus) with
    void SkinTone(RE::Actor* a, float out[3]) {
        out[0] = out[1] = out[2] = 1.0f;
        const auto* npc = a->GetActorBase();
        if (!npc) return;
        const auto& c = npc->bodyTintColor;
        if (c.red == 0 && c.green == 0 && c.blue == 0) return;   // not set
        out[0] = c.red / 255.0f;
        out[1] = c.green / 255.0f;
        out[2] = c.blue / 255.0f;
    }

    void AnusTarget(int choice, const float* custom, float out[3]) {   // 0-255
        if (choice == kCustom) {
            for (int i = 0; i < 3; ++i) out[i] = custom[i] * 255.0f;
        } else {
            for (int i = 0; i < 3; ++i) out[i] = kRimRGB[choice][i];
        }
    }
    void BlendTarget(int choice, const float* custom, float out[3]) {  // 0-255
        if (choice == kCustom) {
            for (int i = 0; i < 3; ++i) out[i] = std::clamp(custom[i] * 1.85f + 0.06f, 0.0f, 1.0f) * 255.0f;
        } else {
            for (int i = 0; i < 3; ++i) out[i] = kBlendRGB[choice][i];
        }
    }

    int PackRGB(const float c[3]) {   // 0-1 -> 0xRRGGBB
        int r = std::clamp(static_cast<int>(std::lround(c[0] * 255.0f)), 0, 255);
        int gg = std::clamp(static_cast<int>(std::lround(c[1] * 255.0f)), 0, 255);
        int b = std::clamp(static_cast<int>(std::lround(c[2] * 255.0f)), 0, 255);
        return (r << 16) | (gg << 8) | b;
    }

    // 1 for the first 20% of the time, then a smooth fade to 0
    float Fade(double elapsed, float duration) {
        if (elapsed <= 0.0) return 1.0f;
        float x = static_cast<float>(elapsed / duration);
        if (x <= 0.2f) return 1.0f;
        x = std::clamp((x - 0.2f) / 0.8f, 0.0f, 1.0f);
        return 1.0f - x * x * (3.0f - 2.0f * x);
    }
    float Quant(float v) { return std::round(std::clamp(v, 0.0f, 1.0f) * 20.0f) / 20.0f; }

    // the full wanted look of one actor
    struct Look {
        std::string tex;
        int tint = -1;              // anus shader tint, -1 = none
        int blend = -1;             // overlay tint, -1 = installed
        int swap = 0;               // 1 = overlay uses the white base texture (tinted)
        int gloss = -1, spec = -1;  // x100, -1 = none
        int add = -1;               // ring colour to ADD if he has no skin-blend paint, -1 = don't
    };

    Look Want(RE::Actor* a) {
        Look L;
        const Pick pk = PickFor(a);
        const int choice = pk.choice;
        float skin[3];
        SkinTone(a, skin);

        // after-sex levels
        float wet = 0.0f, tender = 0.0f;
        if (auto it = g_work.find(a->GetFormID()); it != g_work.end()) {
            const double e = Now() - it->second.last;
            if (g.wet) wet = Quant(it->second.peak * g.wetStrength * Fade(e, g.wetTime));
            if (g.tender) tender = Quant(it->second.peak * g.tenderStrength * Fade(e, g.tenderTime));
            if (e > std::max(g.wetTime, g.tenderTime) + 5.0) g_work.erase(it);
        }
        const float red[3] = {1.0f, 1.0f - 0.40f * tender, 1.0f - 0.35f * tender};

        // rim texture: a shipped shade, or a generated one (any colour / strength / reddening).
        // The game puts the character's skin tone on top, so no shader tint is needed.
        const bool any = choice >= 0 && (g.method == 0 || choice == kCustom);
        if ((choice >= 0 || tender > 0.0f) && (any || tender > 0.0f)) {
            const int col = choice >= 0 ? choice : InstalledPreset();
            const float s = (g.method == 0 && choice >= 0) ? pk.strength : 1.0f;
            float pick[3], base[3], c[3];
            AnusTarget(col, pk.rgb, pick);
            AnusTarget(kSkinPreset, pk.rgb, base);
            for (int i = 0; i < 3; ++i) c[i] = (base[i] + (pick[i] - base[i]) * s) / 255.0f * red[i];
            L.tex = GenRimTexture(c);
        }
        if (L.tex.empty() && choice >= 0) L.tex = std::string(kDir) + kColourFiles[std::min(choice, kPresets - 1)];

        // skin-blend overlay (the game skin-tones the anus, so the ring follows the skin tone too)
        const bool npcRing = g.autoRing && !a->IsPlayerRef();
        if ((g.blendMatch && choice >= 0) || npcRing) {
            const int col = (g.blendMatch && choice >= 0) ? choice : InstalledPreset();
            const float s = (g.method == 0 && col != kCustom && choice >= 0) ? pk.strength : 1.0f;
            float pick[3], base[3], c[3];
            BlendTarget(col, pk.rgb, pick);
            BlendTarget(kSkinPreset, pk.rgb, base);
            for (int i = 0; i < 3; ++i) {
                c[i] = (base[i] + (pick[i] - base[i]) * s) / 255.0f * skin[i];
                if (g.tenderBlend) c[i] *= red[i];
            }
            L.blend = PackRGB(c);
            L.swap = 1;
            if (npcRing) L.add = L.blend;
        } else if (g.tenderBlend && tender > 0.0f) {
            L.blend = PackRGB(red);   // redden the installed overlay as it is
            L.swap = 0;
        }

        if (wet > 0.0f) {
            L.gloss = static_cast<int>(std::lround((30.0f + 90.0f * wet) * 100.0f));
            L.spec = static_cast<int>(std::lround((3.0f + 7.0f * wet) * 100.0f));
        }
        return L;
    }

    // ---- Papyrus ------------------------------------------------------------------------------
    // Int[] LookFor(Actor): [tex, anusTint, blendTint, blendSwap, gloss x100, spec x100, ringToAdd]
    //   -2 = unchanged, -1 = remove / back to installed, -3 (blend only) = clean up if ours,
    //   tex 0 = set (ask ColourFor for the path), tints >= 0 = 0xRRGGBB
    std::vector<std::int32_t> LookFor(RE::StaticFunctionTag*, RE::Actor* a) {
        std::vector<std::int32_t> out{-2, -2, -2, 0, -2, -2, -1};
        if (!a) return out;
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        const auto id = a->GetFormID();
        const Look L = Want(a);
        g_pendingTex[id] = L.tex;
        auto it = g_sent.find(id);
        if (it == g_sent.end()) {
            // first time this session: send the whole look
            out[0] = L.tex.empty() ? -1 : 0;
            out[1] = L.tint;
            out[2] = L.blend >= 0 ? L.blend : -3;
            out[3] = L.swap;
            out[4] = L.gloss;
            out[5] = L.gloss >= 0 ? L.spec : -1;
            out[6] = L.add;
            g_sent[id] = {L.tex, L.tint, L.blend, L.swap, L.gloss, L.spec, L.add};
            return out;
        }
        Sent& s = it->second;
        if (L.tex != s.tex) out[0] = L.tex.empty() ? -1 : 0;
        if (L.tint >= 0 || L.tint != s.tint) out[1] = L.tint;   // tints are re-sent: the game can reset them
        if (L.blend != s.blend || L.swap != s.swap || L.add != s.add) {
            out[2] = L.blend;
            out[3] = L.swap;
            out[6] = L.add;
        }
        if (L.gloss != s.gloss || L.spec != s.spec) {
            out[4] = L.gloss;
            out[5] = L.gloss >= 0 ? L.spec : -1;
        }
        s = {L.tex, L.tint, L.blend, L.swap, L.gloss, L.spec, L.add};
        return out;
    }

    RE::BSFixedString ColourFor(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!a) return "";
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        auto it = g_pendingTex.find(a->GetFormID());
        if (it != g_pendingTex.end()) return RE::BSFixedString(it->second.c_str());
        return RE::BSFixedString(Want(a).tex.c_str());
    }

    float ColourInterval(RE::StaticFunctionTag*) {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        return g.interval;
    }

    // called by the gape script while the actor is being penetrated (dil = how dilated, 0-1)
    void Worked(RE::StaticFunctionTag*, RE::Actor* a, float dil) {
        if (!a) return;
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        auto& w = g_work[a->GetFormID()];
        const double now = Now();
        float cur = 0.0f;
        if (w.last > 0.0) cur = w.peak * Fade(now - w.last, std::max(g.wetTime, g.tenderTime));
        w.peak = std::clamp(std::max(cur, std::max(dil, 0.15f)), 0.0f, 1.0f);
        w.last = now;
    }

    // ---- menu helpers -------------------------------------------------------------------------
    bool ComboValue(const char* label, int* value, int offset, const char* const items[], int count) {
        int idx = *value + offset;
        if (ImGuiMCP::Combo(label, &idx, items, count)) {
            *value = idx - offset;
            return true;
        }
        return false;
    }
    void Help(const char* text) {
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::BeginTooltip();
            ImGuiMCP::PushTextWrapPos(ImGuiMCP::GetFontSize() * 30.0f);
            ImGuiMCP::TextUnformatted(text);
            ImGuiMCP::PopTextWrapPos();
            ImGuiMCP::EndTooltip();
        }
    }
    bool Pct(const char* label, float* v) {
        float p = *v * 100.0f;
        if (ImGuiMCP::SliderFloat(label, &p, 0.0f, 100.0f, "%.0f%%")) {
            *v = p / 100.0f;
            return true;
        }
        return false;
    }
    // shades / Custom / your six colours / Smart tick boxes; returns true when changed
    bool PoolEditor(int& bits, float palette[6][3]) {
        bool changed = false;
        auto box = [&](const char* label, int bit) {
            bool on = (bits & (1 << bit)) != 0;
            if (ImGuiMCP::Checkbox(label, &on)) {
                bits = on ? (bits | (1 << bit)) : (bits & ~(1 << bit));
                if (bits == 0) bits = 1 << bit;   // keep at least one
                changed = true;
            }
        };
        for (int i = 0; i <= kCustom; ++i) {
            box(kColourNames[i], i);
            if (i != 2 && i != kCustom) ImGuiMCP::SameLine();
        }
        box("Smart (fits his skin, uses Skin rules)", kSmartBit);
        ImGuiMCP::TextDisabled("Your colours (tick to use, click a swatch for the colour wheel):");
        for (int i = 0; i < kPaletteSize; ++i) {
            ImGuiMCP::PushID(i);
            box("##use", kPaletteBit + i);
            ImGuiMCP::SameLine();
            changed |= ImGuiMCP::ColorEdit3("##col", palette[i], kWheel | ImGuiMCP::ImGuiColorEditFlags_NoInputs);
            ImGuiMCP::PopID();
            if (i < kPaletteSize - 1) ImGuiMCP::SameLine();
        }
        return changed;
    }
    bool Minutes(const char* label, float* seconds, float lo, float hi) {
        float m = *seconds / 60.0f;
        if (ImGuiMCP::SliderFloat(label, &m, lo, hi, "%.1f min")) {
            *seconds = m * 60.0f;
            return true;
        }
        return false;
    }
}

namespace Colour {
    // a button that asks "Are you sure?" before it counts as pressed
    bool ConfirmButton(const char* label) {
        static std::string pending;
        if (pending == label) {
            ImGuiMCP::TextUnformatted("Are you sure?");
            ImGuiMCP::SameLine();
            const std::string yes = std::string("Yes##") + label, no = std::string("No##") + label;
            if (ImGuiMCP::Button(yes.c_str())) {
                pending.clear();
                return true;
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button(no.c_str())) pending.clear();
            return false;
        }
        if (ImGuiMCP::Button(label)) pending = label;
        return false;
    }

    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("ColourFor"sv, "AnatomicaNative"sv, ColourFor);
        vm->RegisterFunction("LookFor"sv, "AnatomicaNative"sv, LookFor);
        vm->RegisterFunction("ColourInterval"sv, "AnatomicaNative"sv, ColourInterval);
        vm->RegisterFunction("Worked"sv, "AnatomicaNative"sv, Worked);
    }

    void Reset() {
        std::lock_guard lk(g_lock);
        g_sent.clear();
        g_pendingTex.clear();
        g_work.clear();
    }

    void __stdcall RenderColours() {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        bool changed = false;
        ImGuiMCP::TextWrapped("Anus colour per character, applied to nearby men with RaceMenu and kept with your save "
                              "(needs RaceMenu and AnatomicaGape.esp).");
        ImGuiMCP::Separator();
        changed |= ImGuiMCP::Checkbox("Colour picker on", &g.enabled);
        Help("Off = everyone goes back to the colour you picked in the installer. After-sex wetness and reddening "
             "(After sex page) still work.");
        changed |= ImGuiMCP::SliderFloat("Check every", &g.interval, 1.0f, 180.0f, "%.0f s", ImGuiMCP::ImGuiSliderFlags_Logarithmic);
        Help("How often nearby men are checked (1 s - 3 min). Shorter = new men and after-sex looks show up sooner; longer = "
             "less script work. A man who already looks right costs almost nothing, so 3 s is fine for most games.");
        changed |= ComboValue("Applies to", &g.scope, 0, kScopeItems, 3);
        changed |= ComboValue("Method", &g.method, 0, kMethodItems, 2);
        Help("Any colour: Custom, your colours, Smart random, colour strength and reddening all work - each colour is "
             "made once as a small texture (textures\\...\\Anatomica\\gen, safe to delete) and reused. "
             "Fixed shades: only the five shipped textures. Either way the game adds the man's own skin tone on top.");
        if (g.method == 0) {
            changed |= Pct(g.randStrength ? "Colour strength from" : "Colour strength", &g.strength);
            Help("100% = the full picked colour. 0% = the character's own skin tone (no pink). Values between blend the two.");
            changed |= ImGuiMCP::Checkbox("Random strength per man", &g.randStrength);
            Help("Each man gets his own strength somewhere between the two values (fixed per man, changes with re-roll).");
            if (g.randStrength) changed |= Pct("Colour strength to", &g.strengthMax);
        } else {
            ImGuiMCP::TextDisabled("Colour strength: tint method only.");
        }
        changed |= ImGuiMCP::ColorEdit3("Custom colour", g.custom, kWheel);
        Help("Used wherever \"Custom\" is picked (always tinted, in both methods). Pick the colour you want the anus to be; "
             "it is still shaded by the skin tone.");
        changed |= ImGuiMCP::Checkbox("Skin-blend overlay matches the colour", &g.blendMatch);
        Help("The RaceMenu \"Anatomica Blend\" body paint (the soft ring around the anus) takes the same colour.");
        changed |= ImGuiMCP::Checkbox("Give NPCs the skin-blend ring", &g.autoRing);
        Help("Men who don't have the \"Anatomica Blend\" paint get it automatically (one free RaceMenu body-overlay slot), "
             "coloured to match, so the anus fades into the skin instead of having a hard edge. Not added to you - apply "
             "it yourself in RaceMenu (Body Paint > Anatomica Blend).");

        ImGuiMCP::SeparatorText("Who gets which colour");
        changed |= ComboValue("Everyone (default)", &g.def, 1, kDefaultItems, 9);
        changed |= ComboValue("You (player)", &g.player, 2, kPlayerItems, 10);
        ImGuiMCP::TextDisabled("Per-race picks: Per race page.");

        ImGuiMCP::SeparatorText("Random picks from");
        changed |= PoolEditor(g.pool, g.palette);

        ImGuiMCP::SeparatorText("Smart random");
        changed |= ComboValue("Colours chosen", &g.harmony, 0, kHarmonyItems, 3);
        Help("Smart random first follows your Skin rules page (e.g. red argonian -> blue anus). With no matching rule it "
             "reads his skin colour and picks an anus colour around it. Same family = his own hue, "
             "deeper. Neighbouring = hues next to his on the colour wheel (a red argonian gets reds, oranges, magentas - "
             "never green). Contrasting = the opposite side of the wheel, for bold looks.");
        ImGuiMCP::TextDisabled("Random and Smart random are fixed per man until you re-roll.");
        if (ConfirmButton("Re-roll everyone")) {
            ++g.seed;
            changed = true;
        }
        Help("Gives every man a new random / smart-random colour.");
        ImGuiMCP::SameLine();
        {
            auto sel = RE::Console::GetSelectedRef();
            auto* act = sel ? sel->As<RE::Actor>() : nullptr;
            const std::string lbl = act ? std::format("Re-roll {}", act->GetName()) : std::string("Re-roll selected man");
            if (ImGuiMCP::Button(lbl.c_str()) && act) {
                ++g_reroll[RefKey(act)];
                changed = true;
            }
            Help("Open the console (~) and click a man to select him, then press this to give just him a new random colour.");
        }
        ImGuiMCP::Spacing();
        if (ConfirmButton("Reset colour settings")) {
            const Settings d;
            g.enabled = d.enabled;
            g.interval = d.interval;
            g.scope = d.scope;
            g.method = d.method;
            g.strength = d.strength;
            g.randStrength = d.randStrength;
            g.strengthMax = d.strengthMax;
            std::copy(std::begin(d.custom), std::end(d.custom), g.custom);
            g.blendMatch = d.blendMatch;
            g.autoRing = d.autoRing;
            g.def = d.def;
            g.player = d.player;
            g.pool = d.pool;
            g.harmony = d.harmony;
            changed = true;
        }
        if (changed) SaveIni();
        ImGuiMCP::Spacing();
        ImGuiMCP::TextDisabled("Save size: a few small RaceMenu entries per coloured man (well under 1 KB each).");
    }

    void __stdcall RenderRaces() {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        BuildRaces();
        bool changed = false;
        if (!g_raceDefaults.empty())
            ImGuiMCP::TextDisabled("Races set in the installer start with that colour; Reset all races goes back to them.");
        ImGuiMCP::TextWrapped("Colour per playable race. \"Use the default\" follows Everyone on the Colours page. "
                              "Custom (and Random, when it lands on Custom) uses this race's own colour - click the "
                              "swatch for the colour wheel. Own colour strength only applies with the Tint method.");
        ImGuiMCP::Separator();
        if (g_raceRows.empty()) ImGuiMCP::TextDisabled("Races appear here once a game is loaded.");
        for (const auto& r : g_raceRows) {
            auto& rs = g_races[r.key];
            ImGuiMCP::PushID(r.key.c_str());
            if (ComboValue(r.name.c_str(), &rs.choice, 2, kRaceItems, 10)) {
                if (rs.choice == kCustom && !g_raceTouched[r.key]) std::copy(std::begin(g.custom), std::end(g.custom), rs.rgb);
                changed = true;
            }
            if (rs.choice != kUseDefault && rs.choice != kInstalled) {
                ImGuiMCP::Indent();
                if (rs.choice == kCustom || rs.choice == kRandom) {
                    if (ImGuiMCP::ColorEdit3("Custom colour for this race", rs.rgb, kWheel)) {
                        g_raceTouched[r.key] = true;
                        changed = true;
                    }
                }
                if (rs.choice == kRandom) {
                    bool ownPool = rs.pool > 0;
                    if (ImGuiMCP::Checkbox("Own random colours", &ownPool)) {
                        rs.pool = ownPool ? g.pool : -1;
                        if (ownPool) std::copy(&g.palette[0][0], &g.palette[0][0] + 18, &rs.palette[0][0]);
                        changed = true;
                    }
                    Help("Pick which colours Random may give this race, separate from the Colours page.");
                    if (ownPool) changed |= PoolEditor(rs.pool, rs.palette);
                }
                if (g.method == 0) {
                    bool own = rs.strength >= 0.0f;
                    if (ImGuiMCP::Checkbox("Own colour strength", &own)) {
                        rs.strength = own ? g.strength : -1.0f;
                        rs.strengthMax = own ? (g.randStrength ? g.strengthMax : g.strength) : -1.0f;
                        changed = true;
                    }
                    if (own) {
                        changed |= Pct("from##s", &rs.strength);
                        changed |= Pct("to##s", &rs.strengthMax);
                        Help("Each man of this race gets a strength between the two (set both the same for a fixed strength).");
                    }
                }
                ImGuiMCP::Unindent();
            }
            ImGuiMCP::PopID();
        }
        ImGuiMCP::Spacing();
        if (ConfirmButton("Reset all races")) {
            g_races.clear();
            for (const auto& [k, v] : g_raceDefaults) g_races[k].choice = v;   // back to the installer's picks
            changed = true;
        }
        if (changed) SaveIni();
    }

    void __stdcall RenderRules() {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        BuildRaces();
        bool changed = false;
        ImGuiMCP::TextWrapped("Skin rules decide the anus colour from the man's skin colour. They are used by Smart random "
                              "(pick it for Everyone, a race, or tick Smart in a Random list). Example: race Argonian, "
                              "skin red within 35 degrees -> blue anus; skin blue -> purple anus. If several rules match "
                              "one man he gets one of them at random (fixed per man). No match = the normal Smart colour.");
        ImGuiMCP::TextDisabled("Skin colour is read from the character's skin tone (RaceMenu / CK tint). Skins coloured only "
                               "by a texture, not a tint, read as plain.");
        ImGuiMCP::Separator();
        std::vector<const char*> items{"All races"};
        for (const auto& r : g_raceRows) items.push_back(r.plain.c_str());
        int del = -1;
        for (size_t i = 0; i < g_rules.size(); ++i) {
            auto& r = g_rules[i];
            ImGuiMCP::PushID(static_cast<int>(i));
            changed |= ImGuiMCP::Checkbox("##on", &r.on);
            ImGuiMCP::SameLine();
            int sel = 0;
            for (size_t k = 0; k < g_raceRows.size(); ++k)
                if (g_raceRows[k].key == r.race) sel = static_cast<int>(k) + 1;
            ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 9.0f);
            if (ImGuiMCP::Combo("##race", &sel, items.data(), static_cast<int>(items.size()))) {
                r.race = sel == 0 ? std::string() : g_raceRows[sel - 1].key;
                changed = true;
            }
            ImGuiMCP::SameLine();
            changed |= ImGuiMCP::ColorEdit3("skin##s", r.skin, kWheel | ImGuiMCP::ImGuiColorEditFlags_NoInputs);
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 7.0f);
            changed |= ImGuiMCP::SliderFloat("##tol", &r.tol, 5.0f, 90.0f, "+-%.0f deg");
            ImGuiMCP::SameLine();
            changed |= ImGuiMCP::ColorEdit3("anus##a", r.anus, kWheel | ImGuiMCP::ImGuiColorEditFlags_NoInputs);
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Remove")) del = static_cast<int>(i);
            ImGuiMCP::PopID();
        }
        if (del >= 0) {
            g_rules.erase(g_rules.begin() + del);
            changed = true;
        }
        if (g_rules.empty()) ImGuiMCP::TextDisabled("No rules yet.");
        ImGuiMCP::Spacing();
        if (static_cast<int>(g_rules.size()) < kMaxRules && ImGuiMCP::Button("Add rule")) {
            g_rules.push_back({});
            changed = true;
        }
        Help("Tick = rule on. Race = who it applies to. Skin swatch + degrees = which skin colours match (how far round the "
             "colour wheel from that colour). Anus swatch = the colour they get.");
        if (changed) {
            SaveIni();
        }
    }

    void __stdcall RenderAfter() {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        bool changed = false;
        ImGuiMCP::TextWrapped("Looks that follow anal sex and fade afterwards. Stronger and longer sex = stronger "
                              "effect (up to the strength set here).");
        ImGuiMCP::SeparatorText("Wetness");
        changed |= ImGuiMCP::Checkbox("Wet shine after sex", &g.wet);
        changed |= Pct("Wetness strength", &g.wetStrength);
        Help("How glossy the anus gets (stronger, tighter highlights).");
        changed |= Minutes("Wetness dries in", &g.wetTime, 0.5f, 30.0f);
        ImGuiMCP::SeparatorText("Tender reddening");
        changed |= ImGuiMCP::Checkbox("Tender reddening after sex", &g.tender);
        changed |= Pct("Reddening strength", &g.tenderStrength);
        Help("How red and flushed the anus gets.");
        changed |= Minutes("Reddening fades in", &g.tenderTime, 0.5f, 60.0f);
        changed |= ImGuiMCP::Checkbox("Also redden the skin-blend area", &g.tenderBlend);
        Help("Reddens the RaceMenu \"Anatomica Blend\" paint around the anus too (characters who have it applied).");
        ImGuiMCP::Spacing();
        if (ConfirmButton("Reset after-sex settings")) {
            const Settings d;
            g.wet = d.wet;
            g.wetStrength = d.wetStrength;
            g.wetTime = d.wetTime;
            g.tender = d.tender;
            g.tenderStrength = d.tenderStrength;
            g.tenderTime = d.tenderTime;
            g.tenderBlend = d.tenderBlend;
            changed = true;
        }
        if (changed) SaveIni();
        ImGuiMCP::Spacing();
        ImGuiMCP::TextDisabled("Updates every \"Check every\" seconds (Colours page). Nothing extra is saved: it fades on its own.");
    }
}

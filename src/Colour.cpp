#include "Colour.h"
#include "Drip.h"
#include "Virgin.h"
#include "Size.h"
#include "Extras.h"
#include "Bridge.h"
#include "Fluid.h"
#include "Engine.h"
#include "SKSEMenuFramework.h"
#include "UI.h"
#include "Wink.h"
#include "Maker.h"

#include <SimpleIni.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <map>
#include <random>
#include <mutex>
#include <set>
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
    std::vector<std::uint8_t> g_edge;     // 0 at the opening, 255 toward the outer edge of the rim
    int g_detailW = 0, g_detailH = 0;
    bool g_detailTried = false;

    bool LoadDetail() {
        if (g_detailTried) return !g_detail.empty();
        g_detailTried = true;
        std::ifstream f("Data/SKSE/Plugins/AnatomicaMenu/rim_detail.bin", std::ios::binary);
        char magic[4]{};
        std::uint32_t w = 0, h = 0;
        if (!f.read(magic, 4)) return false;
        const std::string_view m(magic, 4);
        if (m != "ARD1" && m != "ARD2") return false;
        f.read(reinterpret_cast<char*>(&w), 4);
        f.read(reinterpret_cast<char*>(&h), 4);
        if (w == 0 || h == 0 || w > 4096 || h > 4096 || (w & 3) || (h & 3)) return false;
        const size_t n = static_cast<size_t>(w) * h;
        g_detail.resize(n);
        if (!f.read(reinterpret_cast<char*>(g_detail.data()), static_cast<std::streamsize>(n))) {
            g_detail.clear();
            return false;
        }
        g_edge.assign(n, 255);
        if (m == "ARD2") f.read(reinterpret_cast<char*>(g_edge.data()), static_cast<std::streamsize>(n));
        g_detailW = static_cast<int>(w);
        g_detailH = static_cast<int>(h);
        return true;
    }

    std::uint16_t To565(const float c[3]) {
        auto q = [](float v, int bits) { return static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * ((1 << bits) - 1))); };
        return static_cast<std::uint16_t>((q(c[0], 5) << 11) | (q(c[1], 6) << 5) | q(c[2], 5));
    }

    // one DXT1 mip level from an RGB float image: per 4x4 block the two end colours are the
    // darkest and brightest texel, the rest are put on the line between them
    void EncodeLevel(const std::vector<float>& rgb, int w, int h, std::string& out) {
        const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                float px[16][3];
                int lo = 0, hi = 0;
                float llo = 1e9f, lhi = -1e9f;
                for (int k = 0; k < 16; ++k) {
                    const int x = std::min(bx * 4 + (k & 3), w - 1), y = std::min(by * 4 + (k >> 2), h - 1);
                    const float* src = &rgb[(static_cast<size_t>(y) * w + x) * 3];
                    for (int c = 0; c < 3; ++c) px[k][c] = std::clamp(src[c], 0.0f, 1.0f);
                    const float l = 0.299f * px[k][0] + 0.587f * px[k][1] + 0.114f * px[k][2];
                    if (l < llo) { llo = l; lo = k; }
                    if (l > lhi) { lhi = l; hi = k; }
                }
                std::uint16_t c0 = To565(px[hi]), c1 = To565(px[lo]);
                std::uint32_t idx = 0;
                if (c0 == c1) {
                    if (c0 == 0) { c0 = 1; idx = 0x55555555u; }   // all black: every texel = c1
                    else c1 = static_cast<std::uint16_t>(c0 - 1);
                } else {
                    const bool swapped = c0 < c1;
                    if (swapped) std::swap(c0, c1);
                    const float* A = swapped ? px[lo] : px[hi];   // colour of c0
                    const float* B = swapped ? px[hi] : px[lo];   // colour of c1
                    float d[3] = {A[0] - B[0], A[1] - B[1], A[2] - B[2]};
                    const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                    for (int k = 0; k < 16; ++k) {
                        float t = dd > 1e-8f ? ((px[k][0] - B[0]) * d[0] + (px[k][1] - B[1]) * d[1] + (px[k][2] - B[2]) * d[2]) / dd : 1.0f;
                        const std::uint32_t code = t > 0.833f ? 0u : t > 0.5f ? 2u : t > 0.167f ? 3u : 1u;
                        idx |= code << (2 * k);
                    }
                }
                out.append(reinterpret_cast<const char*>(&c0), 2);
                out.append(reinterpret_cast<const char*>(&c1), 2);
                out.append(reinterpret_cast<const char*>(&idx), 4);
            }
        }
    }

    // everything that goes into one rim texture (all 0-1)
    struct RimTex {
        float colour[3];            // rim colour x skin tone
        float lube[3] = {1, 1, 1};  // wetness colour laid over it
        float lubeAmt = 0.0f;
        float bruise[3] = {1, 1, 1};
        float bruiseAmt = 0.0f;     // toward the outer edge
        float film[3] = {1, 1, 1};
        float filmAmt = 0.0f;       // cum film on the inner edge (where it comes out)
        float frothAmt = 0.0f;      // little bubbles on the inner part of the rim
    };

    // the textures Anatomica makes for itself (rims, ring colours, glossy masks...) - a cache, remade when needed.
    // Kept under a size budget: over it, the oldest go until it is down to three quarters. Also drops what older
    // versions made and nothing uses any more (cum textures, full-size ring colours).
    int g_genBudgetMB = 200;   // copy of the setting, for the code above Settings
    std::uintmax_t GenFolderBytes() {
        std::error_code ec;
        std::uintmax_t n = 0;
        const std::filesystem::path dir = std::filesystem::path("Data") / std::filesystem::path(std::string(kGenDir));
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) if (e.is_regular_file(ec)) n += e.file_size(ec);
        return n;
    }
    // returns what it did, for the menu; everything = delete all of them (they are remade as needed)
    std::string CleanGenFolder(int budgetMB, bool force = false, bool everything = false) {
        static bool done = false;
        if (done && !force) return {};
        done = true;
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path("Data") / std::filesystem::path(std::string(kGenDir));
        std::vector<std::tuple<std::filesystem::file_time_type, std::filesystem::path, std::uintmax_t>> files;
        std::uintmax_t total = 0;
        int legacy = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string n = e.path().filename().string();
            // anatomica_blend_tint_RRGGBB.dds = a full-size (2048) ring colour from 2.3j and older (cum_*.dds are the cum
            // colours of the streaks on a wet schlong: still used, kept)
            const bool oldRing = n.rfind("anatomica_blend_tint_", 0) == 0 && n.size() == std::string("anatomica_blend_tint_000000.dds").size();
            if (oldRing || everything) { if (std::filesystem::remove(e.path(), ec)) ++legacy; continue; }
            const auto sz = e.file_size(ec);
            total += sz;
            files.emplace_back(e.last_write_time(ec), e.path(), sz);
        }
        const std::uintmax_t budget = static_cast<std::uintmax_t>(std::max(20, budgetMB)) * 1024 * 1024;
        int removed = 0;
        if (total > budget) {
            std::sort(files.begin(), files.end());
            for (auto& [t, path, sz] : files) {
                if (total <= budget * 3 / 4) break;
                if (std::filesystem::remove(path, ec)) { total -= sz; ++removed; }
            }
        }
        if (legacy || removed) { Maker::ForgetDone(); Fluid::ForgetMade(); }   // what was made before may be gone now
        if (legacy || removed)
            logger::info("Anatomica colour: tidied the gen folder ({} {} textures, {} oldest removed; now {:.0f} MB)", legacy, everything ? "made" : "old-version", removed, total / 1048576.0);
        if (everything) return std::format("deleted {} textures - each is made again when a man needs it", legacy);
        if (!legacy && !removed)
            return std::format("nothing to tidy: {:.0f} MB is under the {} MB limit", total / 1048576.0, std::max(20, budgetMB));
        return std::format("removed {} textures, now {:.0f} MB", legacy + removed, total / 1048576.0);
    }

    // the rim texture for a look, made in the background (Maker.cpp) the first time it is needed:
    // returns its path, kRimPending while it is being made (he keeps the rim he has), "" if it cannot be made
    constexpr const char* kRimPending = "\x01pending";
    bool BuildRim(const int k[16], int la, int ba, int fa, int fr, const std::string& rel, const std::filesystem::path& file);
    std::string GenRimTexture(const RimTex& t) {
        if (!LoadDetail()) return {};
        CleanGenFolder(g_genBudgetMB);
        auto q = [](float v, int steps) { return std::clamp(static_cast<int>(std::lround(v * steps)), 0, steps); };
        int k[16] = {q(t.colour[0], 31), q(t.colour[1], 31), q(t.colour[2], 31), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const int la = q(t.lubeAmt, 10), ba = q(t.bruiseAmt, 10), fa = q(t.filmAmt, 10), fr = q(t.frothAmt, 6);
        if (la) { k[3] = q(t.lube[0], 15); k[4] = q(t.lube[1], 15); k[5] = q(t.lube[2], 15); k[6] = la; }
        if (ba) { k[7] = q(t.bruise[0], 15); k[8] = q(t.bruise[1], 15); k[9] = q(t.bruise[2], 15); k[10] = ba; }
        if (fa) { k[11] = q(t.film[0], 15); k[12] = q(t.film[1], 15); k[13] = q(t.film[2], 15); k[14] = fa; }
        k[15] = fr;
        std::string key = "rim";
        for (int i = 0; i < (fa || fr ? 16 : 11); ++i) key += std::format("_{}", k[i]);
        if (!la && !ba && !fa && !fr) key = std::format("rim_{:02d}{:02d}{:02d}", k[0], k[1], k[2]);
        const std::string rel = std::format("{}{}.dds", kGenDir, key);
        const std::filesystem::path file = std::filesystem::path("Data") / std::filesystem::path(std::string(rel));
        switch (Maker::Of(rel)) {
            case Maker::State::done: return rel;
            case Maker::State::busy: return kRimPending;
            case Maker::State::failed: return {};
            default: break;
        }
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) { Maker::MarkDone(rel); return rel; }
        std::array<int, 16> kk{};
        std::copy(std::begin(k), std::end(k), kk.begin());
        Maker::Queue(rel, [kk, la, ba, fa, fr, rel, file]() { return BuildRim(kk.data(), la, ba, fa, fr, rel, file); });
        return kRimPending;
    }
    bool BuildRim(const int k[16], int la, int ba, int fa, int fr, const std::string& rel, const std::filesystem::path& file) {
        std::error_code ec;
        // the quantised values, so the file always matches its name
        const float c[3] = {k[0] / 31.0f, k[1] / 31.0f, k[2] / 31.0f};
        const float L[3] = {k[3] / 15.0f, k[4] / 15.0f, k[5] / 15.0f}, Lamt = la / 10.0f;
        const float Bc[3] = {k[7] / 15.0f, k[8] / 15.0f, k[9] / 15.0f}, Bamt = ba / 10.0f;
        const float Fc[3] = {k[11] / 15.0f, k[12] / 15.0f, k[13] / 15.0f}, Famt = fa / 10.0f, Froth = fr / 6.0f;
        // bubbles: one possible bubble per 9x9 cell, placed and sized by a hash of the cell
        auto hash = [](std::uint32_t x, std::uint32_t y, std::uint32_t s) {
            std::uint32_t h = x * 374761393u + y * 668265263u + s * 2246822519u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
        };
        const int dw = g_detailW;
        int w = g_detailW, h = g_detailH, levels = 1;
        while ((w >> levels) >= 1 && (h >> levels) >= 1) ++levels;
        std::vector<float> rgb(static_cast<size_t>(w) * h * 3);
        for (size_t i = 0; i < g_detail.size(); ++i) {
            const float det = g_detail[i] / 128.0f, edge = g_edge[i] / 255.0f;
            for (int ch = 0; ch < 3; ++ch) {
                float v = c[ch] * det;
                if (Bamt > 0.0f) v *= 1.0f + (Bc[ch] - 1.0f) * Bamt * edge;
                if (Lamt > 0.0f) v += (L[ch] * (0.75f + 0.25f * det) - v) * Lamt;
                if (Famt > 0.0f) v += (Fc[ch] * (0.8f + 0.2f * det) - v) * Famt * std::pow(1.0f - edge, 1.5f);
                rgb[i * 3 + ch] = v;
            }
            if (Froth > 0.0f) {
                const int px = static_cast<int>(i % dw), py = static_cast<int>(i / dw), cx = px / 9, cy = py / 9;
                float best = 0.0f;
                for (int oy = -1; oy <= 1; ++oy)
                    for (int ox = -1; ox <= 1; ++ox) {
                        const std::uint32_t X = static_cast<std::uint32_t>(cx + ox), Y = static_cast<std::uint32_t>(cy + oy);
                        if (hash(X, Y, 1) > Froth * 0.85f * std::pow(1.0f - edge, 0.7f)) continue;
                        const float bx = (cx + ox) * 9 + 1 + hash(X, Y, 2) * 7, by = (cy + oy) * 9 + 1 + hash(X, Y, 3) * 7;
                        const float r = 1.2f + 3.0f * hash(X, Y, 4) * hash(X, Y, 5);
                        const float d = std::sqrt((px - bx) * (px - bx) + (py - by) * (py - by)) / r;
                        if (d < 1.0f) best = std::max(best, 0.55f + 0.45f * (1.0f - d * d) + (d > 0.8f ? -0.15f : 0.0f));
                    }
                if (best > 0.0f)
                    for (int ch = 0; ch < 3; ++ch) rgb[i * 3 + ch] += (0.93f * best - rgb[i * 3 + ch]) * 0.85f;
            }
        }
        std::string data;
        for (int lv = 0; lv < levels; ++lv) {
            EncodeLevel(rgb, w, h, data);
            if (w == 1 && h == 1) break;
            const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
            std::vector<float> n2(static_cast<size_t>(nw) * nh * 3);
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x)
                    for (int ch = 0; ch < 3; ++ch) {
                        const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1), y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
                        auto at = [&](int xx, int yy) { return rgb[(static_cast<size_t>(yy) * w + xx) * 3 + ch]; };
                        n2[(static_cast<size_t>(y) * nw + x) * 3 + ch] = 0.25f * (at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1));
                    }
            rgb.swap(n2);
            w = nw;
            h = nh;
        }
        std::uint32_t hdr[32]{};
        hdr[0] = 0x20534444u;
        hdr[1] = 124;
        hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000;
        hdr[3] = static_cast<std::uint32_t>(g_detailH);
        hdr[4] = static_cast<std::uint32_t>(g_detailW);
        hdr[5] = static_cast<std::uint32_t>(std::max(1, g_detailW / 4) * std::max(1, g_detailH / 4) * 8);
        hdr[7] = static_cast<std::uint32_t>(levels);
        hdr[19] = 32;
        hdr[20] = 0x4;
        hdr[21] = 0x31545844u;   // DXT1
        hdr[27] = 0x1000 | 0x8 | 0x400000;
        std::filesystem::create_directories(file.parent_path(), ec);
        const auto tmp = std::filesystem::path(file).concat(".tmp");
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            if (!o) return false;
            o.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
            o.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!o) return false;
        }
        std::filesystem::rename(tmp, file, ec);
        if (ec) return false;
        logger::info("Anatomica colour: made {}", rel);
        return true;
    }

    const char* kDefaultItems[] = {"Installed colour (no change)", "Pink", "Light pink", "Rose / deep red",
                                   "Mauve brown", "Skin matched", "Custom", "Random", "Skin-fit (follows Skin rules)"};
    const char* kRaceItems[] = {"Use the default", "Installed colour (no change)", "Pink", "Light pink",
                                "Rose / deep red", "Mauve brown", "Skin matched", "Custom", "Random",
                                "Skin-fit (follows Skin rules)"};
    const char* kPlayerItems[] = {"Same as your race", "Installed colour (no change)", "Pink", "Light pink",
                                  "Rose / deep red", "Mauve brown", "Skin matched", "Custom", "Random",
                                  "Skin-fit (follows Skin rules)"};
    const char* kFollowerItems[] = {"Same as their race", "Installed colour (no change)", "Pink", "Light pink",
                                    "Rose / deep red", "Mauve brown", "Skin matched", "Custom", "Random",
                                    "Skin-fit (follows Skin rules)"};
    const char* kScopeItems[] = {"Everyone", "You and your followers", "Only you"};
    const char* kMethodItems[] = {"Any colour + strength", "Only the 5 fixed shades"};

    struct Settings {
        bool enabled = true;
        float interval = 3.0f;
        float loadDelay = 10.0f;     // seconds Anatomica's script waits after a loading screen
        float radius = 2100.0f;      // colour men within this distance of the player (game units, ~70 per metre)
        int scope = 0;
        int method = 0;              // 0 tint, 1 textures
        float strength = 1.0f;
        bool randStrength = false;   // each man gets a random strength between strength and strengthMax
        float strengthMax = 1.0f;
        float custom[3] = {0.55f, 0.30f, 0.32f};
        bool blendMatch = true;
        int ringRes = 1;             // the ring colour textures Anatomica makes: 0 = 2048, 1 = 1024, 2 = 512
        int genBudget = 200;         // MB the folder of made textures may take before the oldest go
        bool autoRing = true;        // give NPCs the skin-blend ring if they don't have it
        bool playerRing = true;      // and you
        int def = kInstalled;
        int player = kUseDefault;
        int pool = kDefaultPool;
        float palette[kPaletteSize][3] = {{0.45f, 0.22f, 0.30f}, {0.60f, 0.35f, 0.38f}, {0.38f, 0.26f, 0.22f},
                                          {0.50f, 0.28f, 0.45f}, {0.30f, 0.32f, 0.40f}, {0.55f, 0.40f, 0.30f}};
        int seed = 0;               // "re-roll everyone" bumps this
        int harmony = 1;            // smart random: 0 same family, 1 neighbouring, 2 contrasting
        int ruleMode = 0;           // skin rules: 0 = any matching rule at random, 1 = rules for his race win over "All races"
        // after sex
        bool wet = true;
        float wetStrength = 0.8f;
        float wetTime = 180.0f;
        bool wetBuild = true;        // every new round adds to the wetness (up to wetCap)
        float wetCap = 2.0f;
        bool wetColourOn = false;    // lube / cum colour on the rim
        float wetColour[3] = {0.95f, 0.93f, 0.88f};
        float wetColourAmt = 0.4f;
        bool wetCumOn = false;       // cum in the wetness (separate from the lube colour): a cum film on the rim, cum-coloured beads
        float wetCum[3] = {0.96f, 0.95f, 0.90f};
        float wetCumAmt = 0.5f;      // its opacity
        bool schlongCum = true;      // the giver's schlong: cum on it (streaks, a drip from the tip), not only the wet shine
        bool schlongCumRace = true;  // his race's cum colour when it has one
        float schlongCumCol[3] = {0.96f, 0.95f, 0.90f};
        float schlongCumAmt = 0.85f; // its opacity
        bool schlongDrip = false;    // a drip from the tip now and then (off: only streaks)
        bool waterDry = true;        // swimming / rain dries it faster
        float waterDryMul = 4.0f;
        bool schlongWet = true;      // the partner's schlong gets the shine too
        float schlongStrength = 0.8f;
        float schlongTime = 120.0f;
        bool tender = true;
        float tenderStrength = 0.6f;
        float tenderTime = 600.0f;
        bool tenderBlend = true;
        bool tenderStages = true;    // bright red first, deep red later
        float tenderCol1[3] = {1.0f, 0.55f, 0.6f};
        float tenderCol2[3] = {0.78f, 0.45f, 0.52f};
        float tenderSplit = 0.35f;   // part of the fade time spent going from colour 1 to colour 2
        bool tenderPartners = true;  // more partners in a row = redder
        float tenderPerPartner = 0.25f;
        bool bruise = true;          // purple toward the edge after very long sex or many partners
        int bruisePartners = 3;
        float bruiseLong = 180.0f;   // seconds of penetration in one go
        float bruiseCol[3] = {0.62f, 0.48f, 0.78f};
        float bruiseStrength = 0.6f;
        bool glossy = true;          // glossy skin round the hole and up the crevice after sex (its own effect, like bruising)
        float glossyStrength = 0.8f;
        float glossyTime = 900.0f;   // seconds to fade
        bool swell = true;           // puffy rim (needs the body built with morphs)
        float swellStrength = 0.6f;
        bool sensitive = true;       // sharper clench, slower breathing while tender
        float sensClench = 0.5f;
        float sensBreath = 0.4f;
        bool healSleep = true;       // sleeping / waiting clears it
        bool healMagic = true;       // healing spells, potions and food heal it, a bit at a time
        float healPotion = 0.5f;     // % of the redness / bruising / swelling healed per point of health a potion gives back
        float healFood = 0.1f;       // the same for food and drink
        float healSpell = 10.0f;     // % per healing spell cast
        bool healFoodOn = false;     // food and drink count too (off: wine, stews... and their effects never heal)
        bool healSpellOn = false;    // healing spells count too (off: only health potions heal)
        bool wetBurst = true;        // extra gloss for the first moments after a round
        float wetBurstSecs = 20.0f;
        float wetBurstBoost = 0.5f;
        bool cumByRace = true;       // the wetness colour follows the giver's race (Cum & drips page)
        bool slowClose = true;       // the gape closes slower while he is tender
        float slowCloseAmt = 0.5f;
        bool rested = true;          // first time in a while = redder
        float restedMinutes = 30.0f;
        float restedExtra = 0.3f;
        bool loose = true;           // lasting looseness: a little gape that stays and recovers over days
        float looseAdd = 0.03f;      // per round
        float looseCap = 0.2f;
        float looseRecover = 0.05f;  // per game day
        bool drip = true;            // SMP Cum Drip: anal strands on men after sex
        float dripChance = 1.0f;
        float dripMinSecs = 20.0f;   // penetration needed in the round
        bool dripCfg = true;         // write the anal_ settings below into SMPCumDrip.json
        float dripLifeMin = 200.0f, dripLifeMax = 220.0f;
        float dripFallMin = 60.0f, dripFallMax = 80.0f;
        bool dripTrail = true;
        int statusKey = 0;           // status window hotkey: DirectX scan code (mouse buttons 256+), 0 = none
        int statusMods = 0;          // modifiers held with it: 1 Ctrl, 2 Shift, 4 Alt
        int moveKey = 0, moveMods = 0;   // unlock the window to move it (mouse cursor), press again to lock
        bool statusMsg = false;      // also show the one-line message
        float skinOnRim = 1.0f;      // how much of the man's skin tone goes into the rim colour
        int follower = kUseDefault;
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
        bool cumOwn = false;                    // own wetness colour when a man of this race is the giver
        float cum[3] = {0.95f, 0.93f, 0.88f};
        float palette[6][3] = {{0.45f, 0.22f, 0.30f}, {0.60f, 0.35f, 0.38f}, {0.38f, 0.26f, 0.22f},
                               {0.50f, 0.28f, 0.45f}, {0.30f, 0.32f, 0.40f}, {0.55f, 0.40f, 0.30f}};
    };
    std::map<std::string, RaceSet> g_races;    // "Plugin.esp|00ABCD" -> settings
    struct ManSet {
        int choice = kCustom;
        float rgb[3] = {0.55f, 0.30f, 0.32f};
    };
    std::map<std::string, ManSet> g_perMan;     // one man's own colour (Status page), by reference
    int g_installed = -2;                       // installer colour (preset index), -2 = not checked yet
    std::map<std::string, int> g_raceDefaults;  // race picks made in the installer (SKSE/Plugins/AnatomicaMenu/Races/*.ini)
    std::map<std::string, int> g_reroll;        // "Plugin|refID" -> times re-rolled
    std::set<RE::FormID> g_ringReset;           // re-rolled: take his skin-blend ring back off first, then redo it
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
    // a drop-down section (closed until clicked) - keeps the pages short
    bool Section(const char* title) { return ImGuiMCP::CollapsingHeader(title); }
    constexpr bool kLube = true;    // lube colour: the outer wet layer of the rim (and the rim texture)
    struct Rule {
        bool on = true;
        std::string race;            // "" = all races
        float skin[3] = {0.60f, 0.25f, 0.20f};
        float tol = 35.0f;           // degrees on the colour wheel
        float anus[3] = {0.25f, 0.30f, 0.55f};
        bool blendOwn = false;       // its own anus blend (skin-blend ring) colour; off = the ring follows the anus colour
        float blend[3] = {0.55f, 0.60f, 0.85f};
    };
    constexpr int kMaxRules = 48;
    std::vector<Rule> g_rules;
    std::vector<RaceRow> g_raceRows;
    std::map<std::string, bool> g_raceTouched;  // race colour picked this session (don't overwrite)
    constexpr int kWheel = ImGuiMCP::ImGuiColorEditFlags_PickerHueWheel;

    // what was last sent to the script for each actor (only changes are sent)
    struct Sent {
        std::string tex;   // "" = no texture override
        int tint = -1, blend = -1, swap = 0, gloss = -1, spec = -1, add = -1;
        int swell = 0, sgloss = -1;
    };
    std::unordered_map<RE::FormID, Sent> g_sent;
    std::unordered_map<RE::FormID, std::string> g_pendingTex;

    // after-sex: how worked each actor was and when
    struct Work {
        float peak = 0.0f;       // how worked (0-1), for reddening
        double last = 0.0;       // last time he was penetrated
        float wetPeak = 0.0f;    // wetness (0 - WetCap)
        float wetBase = 0.0f;    // wetness left over when this round started
        double wetLast = 0.0;
        double dryShift = 0.0;   // extra drying (water / rain)
        double lastEval = 0.0;
        int partners = 0;        // partners in a row this round
        float workSecs = 0.0f;   // seconds of penetration this round
        std::string giverRace;   // race of the last man giving (wetness colour)
        bool dripDone = false;   // SMP Cum Drip already started for this round
        bool looseDone = false;  // looseness already added for this round
        float glossPeak = 0.0f;  // glossy skin (0-1)
        float bruiseMul = 1.0f;  // healing takes the bruising down bit by bit
        float restedBoost = 0.0f;
        float szLoose = 1.0f, szTender = 1.0f;   // partner size this round (TNG): looseness / soreness
        float roughTender = 1.0f, roughTime = 1.0f;   // extras: taken as a captive - sorer, for longer
        RE::FormID szFirst = 0;
    };
    std::unordered_map<RE::FormID, Work> g_work;
    std::unordered_map<RE::FormID, double> g_lastRound;   // when his last round was (for "first time in a while")
    std::unordered_map<RE::FormID, float> g_looseSeen;    // looseness the script last reported (Status page)
    std::unordered_map<RE::FormID, float> g_looseRest;    // extra days of rest for his looseness (healing potions / spells)
    struct Arousal { float wet = 0.0f, sens = 0.0f; };
    std::unordered_map<RE::FormID, Arousal> g_arousal;     // arousal mods (Bridge.cpp): a little wet, twitchier
    struct SchlongWork {
        float peak = 0.0f;
        double last = 0.0;
    };
    std::unordered_map<RE::FormID, SchlongWork> g_schlong;

    double Now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // (re)start the gape script's update loop (AnatomicaNative.Kick) - after every load, and from the Testing page
    void KickScript() {
        if (auto* t = SKSE::GetTaskInterface())
            t->AddTask([]() {
                auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
                if (!vm) return;
                auto* args = RE::MakeFunctionArguments();
                RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
                vm->DispatchStaticCall("AnatomicaNative", "Kick", args, cb);
            });
    }
    std::atomic<double> g_lastLook{0.0};   // when the script last asked for a look (is it running?)
    std::atomic<int> g_looks{0};

    std::string RaceKey(const RE::TESRace* race) {
        if (!race) return {};
        const auto* file = race->GetFile(0);
        const std::string plugin = file ? std::string(file->GetFilename()) : std::string("?");
        return std::format("{}|{:06X}", plugin, race->GetLocalFormID());
    }

    float GetF(CSimpleIniA& ini, const char* sec, const char* key, float def) {
        return static_cast<float>(ini.GetDoubleValue(sec, key, def));
    }

    // the newer after-sex settings, read (save = false) or written (save = true)
    void AfterSexIni(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "AfterSex";
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto F = [&](const char* k, float& v, float lo, float hi) {
            if (save) ini.SetDoubleValue(S, k, v);
            else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi);
        };
        auto I = [&](const char* k, int& v, int lo, int hi) {
            if (save) ini.SetLongValue(S, k, v);
            else v = std::clamp(static_cast<int>(ini.GetLongValue(S, k, v)), lo, hi);
        };
        auto C = [&](const char* k, float* c) {
            if (save) {
                ini.SetValue(S, k, std::format("{:.3f},{:.3f},{:.3f}", c[0], c[1], c[2]).c_str());
            } else if (const char* v = ini.GetValue(S, k, nullptr)) {
                std::sscanf(v, "%f,%f,%f", &c[0], &c[1], &c[2]);
                for (int i = 0; i < 3; ++i) c[i] = std::clamp(c[i], 0.0f, 1.0f);
            }
        };
        B("WetBuild", g.wetBuild); F("WetCap", g.wetCap, 1.0f, 3.0f);
        B("WetColourOn", g.wetColourOn); C("WetColour", g.wetColour); F("WetColourAmt", g.wetColourAmt, 0.0f, 1.0f);
        B("WaterDry", g.waterDry); F("WaterDryMul", g.waterDryMul, 1.0f, 10.0f);
        B("SchlongWet", g.schlongWet); F("SchlongStrength", g.schlongStrength, 0.0f, 1.0f); F("SchlongTime", g.schlongTime, 10.0f, 3600.0f);
        B("WetCumOn", g.wetCumOn); C("WetCum", g.wetCum); F("WetCumAmt", g.wetCumAmt, 0.0f, 1.0f);
        B("SchlongCum", g.schlongCum); B("SchlongDrip", g.schlongDrip); B("SchlongCumRace", g.schlongCumRace); C("SchlongCumColour", g.schlongCumCol); F("SchlongCumAmt", g.schlongCumAmt, 0.0f, 1.0f);
        B("TenderStages", g.tenderStages); C("TenderColour1", g.tenderCol1); C("TenderColour2", g.tenderCol2);
        F("TenderSplit", g.tenderSplit, 0.05f, 0.95f);
        B("TenderPartners", g.tenderPartners); F("TenderPerPartner", g.tenderPerPartner, 0.0f, 1.0f);
        B("Bruise", g.bruise); I("BruisePartners", g.bruisePartners, 2, 6); F("BruiseLong", g.bruiseLong, 30.0f, 1800.0f);
        C("BruiseColour", g.bruiseCol); F("BruiseStrength", g.bruiseStrength, 0.0f, 1.0f);
        B("Glossy", g.glossy); F("GlossyStrength", g.glossyStrength, 0.0f, 1.0f); F("GlossyTime", g.glossyTime, 30.0f, 7200.0f);
        B("Swell", g.swell); F("SwellStrength", g.swellStrength, 0.0f, 1.0f);
        B("Sensitive", g.sensitive); F("SensClench", g.sensClench, 0.0f, 1.0f); F("SensBreath", g.sensBreath, 0.0f, 0.8f);
        B("HealSleep", g.healSleep); B("HealMagic", g.healMagic);
        F("HealPotion", g.healPotion, 0.0f, 5.0f); F("HealFood", g.healFood, 0.0f, 2.0f); F("HealSpell", g.healSpell, 0.0f, 100.0f);
        B("HealFoodOn", g.healFoodOn); B("HealSpellOn", g.healSpellOn);
        B("WetBurst", g.wetBurst); F("WetBurstSecs", g.wetBurstSecs, 5.0f, 120.0f); F("WetBurstBoost", g.wetBurstBoost, 0.0f, 1.0f);
        B("CumByRace", g.cumByRace);
        B("SlowClose", g.slowClose); F("SlowCloseAmt", g.slowCloseAmt, 0.0f, 2.0f);
        B("Rested", g.rested); F("RestedMinutes", g.restedMinutes, 5.0f, 600.0f); F("RestedExtra", g.restedExtra, 0.0f, 1.0f);
        B("Loose", g.loose); F("LooseAdd", g.looseAdd, 0.0f, 0.2f); F("LooseCap", g.looseCap, 0.0f, 0.6f);
        F("LooseRecover", g.looseRecover, 0.0f, 1.0f);
        I("StatusKey", g.statusKey, 0, 300); I("StatusMods", g.statusMods, 0, 7);
        I("MoveKey", g.moveKey, 0, 300); I("MoveMods", g.moveMods, 0, 7); B("StatusMsg", g.statusMsg);
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

    // [ColourRules] Count = n, RuleN = on;race;r,g,b;tolerance;r,g,b
    std::vector<Rule> ParseRules(CSimpleIniA& ini) {
        std::vector<Rule> out;
        const int nr = std::clamp(static_cast<int>(ini.GetLongValue("ColourRules", "Count", 0)), 0, kMaxRules);
        for (int i = 0; i < nr; ++i) {
            const char* v = ini.GetValue("ColourRules", std::format("Rule{}", i).c_str(), nullptr);
            if (!v) continue;
            std::string str(v);
            std::vector<std::string> parts;
            size_t pos = 0;
            for (size_t q; (q = str.find(';', pos)) != std::string::npos; pos = q + 1) parts.push_back(str.substr(pos, q - pos));
            parts.push_back(str.substr(pos));
            if (parts.size() < 5 || parts.size() > 6) continue;
            Rule r;
            r.on = parts[0] == "1";
            r.race = parts[1];
            std::sscanf(parts[2].c_str(), "%f,%f,%f", &r.skin[0], &r.skin[1], &r.skin[2]);
            r.tol = std::clamp(static_cast<float>(std::atof(parts[3].c_str())), 5.0f, 90.0f);
            std::sscanf(parts[4].c_str(), "%f,%f,%f", &r.anus[0], &r.anus[1], &r.anus[2]);
            if (parts.size() == 6 && parts[5] != "-")   // the anus blend colour (6th part; "-" or missing = follows the anus)
                r.blendOwn = std::sscanf(parts[5].c_str(), "%f,%f,%f", &r.blend[0], &r.blend[1], &r.blend[2]) == 3;
            out.push_back(r);
        }
        return out;
    }
    // default rules shipped with the mod: Data/SKSE/Plugins/AnatomicaMenu/Rules/*.ini (e.g. Argonian.ini)
    std::vector<Rule> DefaultRules() {
        std::vector<Rule> out;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator("Data/SKSE/Plugins/AnatomicaMenu/Rules", ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".ini") continue;
            CSimpleIniA f;
            if (f.LoadFile(e.path().string().c_str()) < 0) continue;
            for (auto& r : ParseRules(f))
                if (static_cast<int>(out.size()) < kMaxRules) out.push_back(r);
        }
        return out;
    }

    void LoadIni() {
        LoadRaceDefaults();
        bool rulesFromUser = false;
        CSimpleIniA ini;
        if (ini.LoadFile(kIni) >= 0) {
            const long ver = ini.GetLongValue("Colour", "Version", 1);
            g.enabled = ini.GetBoolValue("Colour", "Enabled", g.enabled);
            g.interval = GetF(ini, "Colour", "Interval", g.interval);
            g.radius = GetF(ini, "Colour", "Radius", g.radius);
            g.loadDelay = GetF(ini, "Colour", "LoadDelay", g.loadDelay);
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
            g.playerRing = ini.GetBoolValue("Colour", "PlayerRing", g.playerRing);
            g.ringRes = std::clamp(static_cast<int>(ini.GetLongValue("Colour", "RingTextureSize", g.ringRes)), 0, 2);
            g.genBudget = std::clamp(static_cast<int>(ini.GetLongValue("Colour", "MadeTexturesMB", g.genBudget)), 20, 4000);
            g_genBudgetMB = g.genBudget;
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
            g.ruleMode = std::clamp(static_cast<int>(ini.GetLongValue("Colour", "RuleMode", g.ruleMode)), 0, 1);
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
            if (ini.GetValue("ColourRules", "Count", nullptr)) {
                g_rules = ParseRules(ini);
                rulesFromUser = true;
            }
            keys.clear();
            ini.GetAllKeys("ColourRaceCum", keys);
            for (const auto& k : keys) {
                auto& r = g_races[k.pItem];
                if (std::sscanf(ini.GetValue("ColourRaceCum", k.pItem, ""), "%f,%f,%f", &r.cum[0], &r.cum[1], &r.cum[2]) == 3) r.cumOwn = true;
                for (auto& c : r.cum) c = std::clamp(c, 0.0f, 1.0f);
            }
            keys.clear();
            ini.GetAllKeys("ColourMan", keys);
            for (const auto& k : keys) {
                ManSet m;
                if (std::sscanf(ini.GetValue("ColourMan", k.pItem, ""), "%d;%f,%f,%f", &m.choice, &m.rgb[0], &m.rgb[1], &m.rgb[2]) >= 1) {
                    m.choice = std::clamp(m.choice, kInstalled, kSmart);
                    g_perMan[k.pItem] = m;
                }
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
            AfterSexIni(ini, false);
            g.skinOnRim = GetF(ini, "Colour", "SkinOnRim", g.skinOnRim);
            g.follower = static_cast<int>(ini.GetLongValue("Colour", "Follower", g.follower));
        }
        if (!rulesFromUser) g_rules = DefaultRules();   // first run: the shipped rules (Argonian colours)
        g.interval = std::clamp(g.interval, 1.0f, 180.0f);
        g.radius = std::clamp(g.radius, 70.0f, 7000.0f);
        g.skinOnRim = std::clamp(g.skinOnRim, 0.0f, 1.0f);
        g.follower = std::clamp(g.follower, kUseDefault, kSmart);
        g.loadDelay = std::clamp(g.loadDelay, 0.0f, 60.0f);
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
        {
            CSimpleIniA di;
            di.LoadFile(kIni);
            Drip::Ini(di, false);
            Virgin::Ini(di, false);
            Bridge::Ini(di, false);
        }
        g_loaded = true;
    }

    void SaveIni() {
        CSimpleIniA ini;
        ini.LoadFile(kIni);
        ini.SetLongValue("Colour", "Version", 2);
        ini.SetBoolValue("Colour", "Enabled", g.enabled);
        ini.SetDoubleValue("Colour", "Interval", g.interval);
        ini.SetDoubleValue("Colour", "Radius", g.radius);
        ini.SetDoubleValue("Colour", "LoadDelay", g.loadDelay);
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
        ini.SetBoolValue("Colour", "PlayerRing", g.playerRing);
        ini.SetLongValue("Colour", "RingTextureSize", g.ringRes);
        ini.SetLongValue("Colour", "MadeTexturesMB", g.genBudget);
        ini.SetLongValue("Colour", "Default", g.def);
        ini.SetLongValue("Colour", "Player", g.player);
        ini.SetLongValue("Colour", "RandomPool", g.pool);
        for (int i = 0; i < kPaletteSize; ++i)
            ini.SetValue("Colour", std::format("Palette{}", i).c_str(),
                         std::format("{:.3f},{:.3f},{:.3f}", g.palette[i][0], g.palette[i][1], g.palette[i][2]).c_str());
        ini.SetLongValue("Colour", "Seed", g.seed);
        ini.SetLongValue("Colour", "Harmony", g.harmony);
        ini.SetLongValue("Colour", "RuleMode", g.ruleMode);
        ini.Delete("ColourReroll", nullptr);
        for (const auto& [k, v] : g_reroll)
            if (v) ini.SetLongValue("ColourReroll", k.c_str(), v);
        ini.Delete("ColourRaces", nullptr);
        ini.Delete("ColourRaceRGB", nullptr);
        ini.Delete("ColourRaceStrength", nullptr);
        ini.Delete("ColourRaceStrengthMax", nullptr);
        ini.Delete("ColourRacePool", nullptr);
        ini.Delete("ColourRacePalette", nullptr);
        ini.Delete("ColourRaceCum", nullptr);
        ini.Delete("ColourRules", nullptr);
        ini.SetLongValue("ColourRules", "Count", static_cast<long>(g_rules.size()));
        for (size_t i = 0; i < g_rules.size(); ++i) {
            const auto& r = g_rules[i];
            ini.SetValue("ColourRules", std::format("Rule{}", i).c_str(),
                         std::format("{};{};{:.3f},{:.3f},{:.3f};{:.0f};{:.3f},{:.3f},{:.3f};{}", r.on ? 1 : 0, r.race, r.skin[0], r.skin[1],
                                     r.skin[2], r.tol, r.anus[0], r.anus[1], r.anus[2],
                                     r.blendOwn ? std::format("{:.3f},{:.3f},{:.3f}", r.blend[0], r.blend[1], r.blend[2]) : std::string("-")).c_str());
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
            if (v.cumOwn) ini.SetValue("ColourRaceCum", k.c_str(), std::format("{:.3f},{:.3f},{:.3f}", v.cum[0], v.cum[1], v.cum[2]).c_str());
        }
        ini.Delete("ColourMan", nullptr);
        for (const auto& [k, m] : g_perMan)
            ini.SetValue("ColourMan", k.c_str(), std::format("{};{:.3f},{:.3f},{:.3f}", m.choice, m.rgb[0], m.rgb[1], m.rgb[2]).c_str());
        ini.SetBoolValue("AfterSex", "Wet", g.wet);
        ini.SetDoubleValue("AfterSex", "WetStrength", g.wetStrength);
        ini.SetDoubleValue("AfterSex", "WetTime", g.wetTime);
        ini.SetBoolValue("AfterSex", "Tender", g.tender);
        ini.SetDoubleValue("AfterSex", "TenderStrength", g.tenderStrength);
        ini.SetDoubleValue("AfterSex", "TenderTime", g.tenderTime);
        ini.SetBoolValue("AfterSex", "TenderBlend", g.tenderBlend);
        AfterSexIni(ini, true);
        ini.SetDoubleValue("Colour", "SkinOnRim", g.skinOnRim);
        ini.SetLongValue("Colour", "Follower", g.follower);
        Drip::Ini(ini, true);
        Virgin::Ini(ini, true);
        Bridge::Ini(ini, true);
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
    // the rules that can apply to him (indices into g_rules), after the rule mode: "race wins" drops the All-races
    // rules when one for his own race matches
    std::vector<int> RuleHits(RE::Actor* a, const float skin[3]) {
        std::vector<int> hits;
        if (g_rules.empty() || !a) return hits;
        const std::string rk = RaceKey(a->GetRace());
        float h, s, v;
        RGBtoHSV(skin, h, s, v);
        bool raceHit = false;
        for (size_t i = 0; i < g_rules.size(); ++i) {
            const Rule& r = g_rules[i];
            if (!r.on || (!r.race.empty() && r.race != rk)) continue;
            float rh, rs, rv;
            RGBtoHSV(r.skin, rh, rs, rv);
            bool match;
            if (rs < 0.12f) match = s < 0.18f && std::fabs(v - rv) < 0.3f;    // greyish / plain skin rule
            else match = s >= 0.08f && HueDist(h, rh) <= r.tol;
            if (match) { hits.push_back(static_cast<int>(i)); raceHit |= !r.race.empty(); }
        }
        if (g.ruleMode == 1 && raceHit) std::erase_if(hits, [](int i) { return g_rules[i].race.empty(); });
        return hits;
    }
    int RuleChosen(RE::Actor* a, const std::vector<int>& hits) { return hits.empty() ? -1 : hits[Hash(a, 31) % hits.size()]; }
    bool RuleFor(RE::Actor* a, const float skin[3], float out[3], float* blendOut = nullptr, bool* blendOwn = nullptr) {
        const auto hits = RuleHits(a, skin);
        if (hits.empty()) return false;
        const Rule* r = &g_rules[RuleChosen(a, hits)];
        std::copy(std::begin(r->anus), std::end(r->anus), out);
        if (blendOwn) *blendOwn = r->blendOwn;
        if (blendOut && r->blendOwn) std::copy(std::begin(r->blend), std::end(r->blend), blendOut);
        return true;
    }

    void SmartFor(RE::Actor* a, float out[3], float* blendOut = nullptr, bool* blendOwn = nullptr) {
        float skin[3], h, sat, v;
        SkinTone(a, skin);
        if (RuleFor(a, skin, out, blendOut, blendOwn)) return;
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
        bool active = false;       // picker on and this man in scope
        bool blendOwn = false;     // a skin rule gave the ring (anus blend) its own colour
        float blend[3] = {0, 0, 0};
        int ruleIdx = -1;          // the skin rule his colour came from (-1 = none)
    };

    Pick PickFor(RE::Actor* a) {
        Pick p;
        p.strength = g.strength;
        std::copy(std::begin(g.custom), std::end(g.custom), p.rgb);
        if (!g.enabled || !InScope(a)) return p;
        p.active = true;
        int choice = kUseDefault;
        const ManSet* man = nullptr;
        if (auto mi = g_perMan.find(RefKey(a)); mi != g_perMan.end()) {   // his own colour (Status page)
            man = &mi->second;
            choice = man->choice;
        } else if (a->IsPlayerRef()) {
            choice = g.player;
        } else if (g.follower != kUseDefault && a->IsPlayerTeammate()) {
            choice = g.follower;
        }
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
        if (choice == kCustom && man) std::copy(std::begin(man->rgb), std::end(man->rgb), p.rgb);
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
            SmartFor(a, p.rgb, p.blend, &p.blendOwn);
            float sk[3];
            SkinTone(a, sk);
            p.ruleIdx = RuleChosen(a, RuleHits(a, sk));
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
        int swell = 0;              // x1000, puffy rim morph
        int sgloss = -1, sspec = -1;  // x100, shine on his schlong (-1 = none)
    };

    // smooth 0-1 levels for one man (also used by the movement code)
    struct Levels {
        float wet = 0.0f, tender = 0.0f, bruise = 0.0f, stage = 0.0f;   // stage 0 = bright red, 1 = deep red
        float gloss = 0.0f;   // glossy skin round the hole
        float worn = 0.0f;    // the part of the wetness that came from sex (arousal alone never colours him)
    };
    Levels LevelsFor(RE::FormID id, RE::Actor* a) {
        Levels lv;
        float aroused = 0.0f;
        if (g.wet) if (auto ar = g_arousal.find(id); ar != g_arousal.end()) aroused = ar->second.wet * g.wetStrength;
        auto it = g_work.find(id);
        if (it == g_work.end()) { lv.wet = aroused; return lv; }
        Work& w = it->second;
        const double now = Now();
        // water dries it faster
        if (a && g.waterDry && w.lastEval > 0.0) {
            bool water = false;
            if (auto* st = a->AsActorState(); st && st->IsSwimming()) water = true;
            if (!water) {
                auto* cell = a->GetParentCell();
                auto* sky = RE::Sky::GetSingleton();
                if (cell && !cell->IsInteriorCell() && sky && sky->currentWeather &&
                    sky->currentWeather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy))
                    water = true;
            }
            if (water) w.dryShift += (now - w.lastEval) * (g.waterDryMul - 1.0f);
        }
        if (a) w.lastEval = now;
        const double e = now - w.last;
        if (g.wet) {
            const double ew = now - w.wetLast + w.dryShift;
            lv.wet = std::min(1.0f, w.wetPeak) * g.wetStrength * Fade(ew, g.wetTime * std::max(1.0f, w.wetPeak));
            // "just finished": extra gloss for the first moments after the last stroke
            if (g.wetBurst && ew < g.wetBurstSecs) lv.wet = std::min(1.0f, lv.wet * (1.0f + g.wetBurstBoost * static_cast<float>(1.0 - ew / g.wetBurstSecs)));
        }
        if (g.tender) {
            const Virgin::Mods vm = Virgin::For(id);   // his first time: redder, and it fades slower
            float p = w.peak * vm.tender * w.szTender * w.roughTender;   // a bigger man / a captive: sorer
            if (g.tenderPartners && w.partners > 1) p *= 1.0f + g.tenderPerPartner * static_cast<float>(w.partners - 1);
            if (g.rested) p *= 1.0f + w.restedBoost;
            lv.tender = std::min(1.0f, p * g.tenderStrength * Fade(e, g.tenderTime * vm.tenderTime * w.roughTime));
            lv.stage = std::clamp(static_cast<float>(e / (g.tenderTime * g.tenderSplit)), 0.0f, 1.0f);
            lv.stage = lv.stage * lv.stage * (3.0f - 2.0f * lv.stage);
            if (g.bruise && (w.partners >= g.bruisePartners || w.workSecs >= g.bruiseLong))
                lv.bruise = g.bruiseStrength * w.bruiseMul * Fade(e, g.tenderTime * 1.5f);
        }
        lv.worn = lv.wet;
        lv.wet = std::max(lv.wet, aroused);
        if (g.glossy) lv.gloss = std::min(1.0f, w.glossPeak * g.glossyStrength * Fade(e, g.glossyTime));
        const double keep = std::max({static_cast<double>(g.wetTime) * std::max(1.0f, w.wetPeak) + w.dryShift, g.tenderTime * 1.5 * std::max(1.0f, w.roughTime) * std::max(1.0f, Virgin::For(id).tenderTime), static_cast<double>(g.glossyTime)});
        if (e > keep + 5.0 && now - w.wetLast > keep + 5.0) g_work.erase(it);
        return lv;
    }

    Look Want(RE::Actor* a) {
        Look L;
        const Pick pk = PickFor(a);
        const int choice = pk.choice;
        float skin[3];
        SkinTone(a, skin);
        for (float& v : skin) v = 1.0f + (v - 1.0f) * g.skinOnRim;   // how much skin tone goes into the rim

        const Levels lv = LevelsFor(a->GetFormID(), a);
        const float wet = Quant(lv.wet), tender = Quant(lv.tender), bruise = Quant(lv.bruise);
        const float worn = Quant(lv.worn);   // the wear colours: only after sex, never from arousal alone
        // reddening colour: a multiplier on the rim / ring colour
        float red[3];
        for (int i = 0; i < 3; ++i) {
            const float stageCol = g.tenderStages ? g.tenderCol1[i] + (g.tenderCol2[i] - g.tenderCol1[i]) * Quant(lv.stage)
                                                  : (i == 0 ? 1.0f : i == 1 ? 0.6f : 0.65f);
            red[i] = 1.0f + (stageCol - 1.0f) * tender;
        }

        // rim texture, made for this man: colour x his skin tone x reddening, plus wetness colour and
        // bruising. (The rim is not skin-tinted by the game - that shader path streaks it.)
        const bool lubeOn = kLube && g.wetColourOn && worn > 0.0f;
        // cum in the wetness (its own colour and opacity, apart from the lube colour): a cum film on the rim
        const float film = g.wetCumOn && worn > 0.0f ? Quant(std::clamp(worn, 0.0f, 1.0f) * g.wetCumAmt) : 0.0f;
        float filmCol[3] = {g.wetCum[0], g.wetCum[1], g.wetCum[2]};
        if (film > 0.0f && kCum && g.cumByRace)
            if (auto wi = g_work.find(a->GetFormID()); wi != g_work.end() && !wi->second.giverRace.empty())
                if (auto ri = g_races.find(wi->second.giverRace); ri != g_races.end() && ri->second.cumOwn)
                    for (int i = 0; i < 3; ++i) filmCol[i] = ri->second.cum[i];
        const float froth = Quant(Drip::Froth(a->GetFormID()));
        // his experience (Virginity page): a virgin's rim paler and pinker, a well-used man's darker and softer
        const Virgin::Mods vmod = Virgin::For(a->GetFormID());
        const float rimExp = std::round(std::clamp(vmod.rim, -1.0f, 1.0f) * 4.0f) / 4.0f;
        if (pk.active || tender > 0.0f || lubeOn || bruise > 0.0f || film > 0.0f || froth > 0.0f || rimExp != 0.0f) {
            const int col = choice >= 0 ? choice : InstalledPreset();
            const float s = (g.method == 0 && choice >= 0) ? pk.strength : 1.0f;
            float pick[3], base[3];
            AnusTarget(col, pk.rgb, pick);
            AnusTarget(kSkinPreset, pk.rgb, base);
            RimTex t{};
            for (int i = 0; i < 3; ++i) {
                t.colour[i] = (base[i] + (pick[i] - base[i]) * s) / 255.0f * red[i] * skin[i];
                if (rimExp < 0.0f) t.colour[i] += (std::min(1.0f, (i == 0 ? 0.98f : i == 1 ? 0.80f : 0.84f)) - t.colour[i]) * 0.14f * -rimExp;
                else if (rimExp > 0.0f) t.colour[i] *= 1.0f - (i == 0 ? 0.10f : 0.20f) * rimExp;
                t.lube[i] = g.wetColour[i];
                if (kCum && g.cumByRace) {
                    if (auto wi = g_work.find(a->GetFormID()); wi != g_work.end() && !wi->second.giverRace.empty())
                        if (auto ri = g_races.find(wi->second.giverRace); ri != g_races.end() && ri->second.cumOwn) t.lube[i] = ri->second.cum[i];
                }
                t.bruise[i] = g.bruiseCol[i];
            }
            t.lubeAmt = lubeOn ? worn * g.wetColourAmt : 0.0f;
            t.filmAmt = film;
            for (int i = 0; i < 3; ++i) t.film[i] = filmCol[i];
            t.frothAmt = froth;
            t.bruiseAmt = bruise;
            L.tex = GenRimTexture(t);
            if (L.tex.empty() && col >= 0 && col < kPresets) L.tex = std::string(kDir) + kColourFiles[col];
            // (kRimPending: being made in the background - LookForImpl keeps the rim he has until it is ready)
        }

        // skin-blend ring
        const bool npcRing = a->IsPlayerRef() ? g.playerRing : g.autoRing;   // a ring for a man without one
        if ((g.blendMatch && choice >= 0) || npcRing) {
            const int col = (g.blendMatch && choice >= 0) ? choice : InstalledPreset();
            const float s = (g.method == 0 && choice >= 0) ? pk.strength : 1.0f;   // same strength as the rim
            float pick[3], base[3], c[3];
            BlendTarget(col, pk.rgb, pick);
            if (pk.blendOwn && g.blendMatch && choice >= 0) for (int i = 0; i < 3; ++i) pick[i] = pk.blend[i] * 255.0f;   // the skin rule's anus blend colour
            BlendTarget(kSkinPreset, pk.rgb, base);
            for (int i = 0; i < 3; ++i) {
                c[i] = (base[i] + (pick[i] - base[i]) * s) / 255.0f * skin[i];
                if (g.tenderBlend) c[i] *= red[i];
            }
            L.blend = PackRGB(c);
            L.swap = 1;
            if (npcRing) L.add = L.blend;
        } else if (g.tenderBlend && tender > 0.0f) {
            L.blend = PackRGB(red);   // redden the installed ring as it is
            L.swap = 0;
        }

        (void)wet;   // wet gloss: Fluid.cpp applies it straight from the DLL now (L.gloss / L.spec stay -1 = removed)
        if (g.swell) L.swell = static_cast<int>(std::lround(Quant(std::min(1.0f, lv.tender * g.swellStrength + vmod.swell)) * 1000.0f));

        // his schlong: coated with cum and glossy (Fluid.cpp, started by the drip code when he pulls out)
        return L;
    }

    // ---- for the simulated cum: who is wet, and with what colour
    std::vector<Colour::WetInfo> WetNowImpl() {
        std::vector<Colour::WetInfo> out;
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        std::vector<RE::FormID> ids;
        for (auto& [id, w] : g_work) ids.push_back(id);
        for (auto& [id, ar] : g_arousal) if (ar.wet > 0.0f && !g_work.count(id)) ids.push_back(id);
        for (auto id : ids) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!a) continue;
            const Levels lv = LevelsFor(id, a);
            if (lv.wet <= 0.0f && lv.gloss <= 0.0f) continue;
            Colour::WetInfo wi{id, lv.wet, {0.93f, 0.93f, 0.92f}};
            wi.gloss = lv.gloss;
            if (kLube && g.wetColourOn && lv.worn > 0.0f) {   // aroused only: clear, no wear colour
                for (int i = 0; i < 3; ++i) wi.rgb[i] = wi.lrgb[i] = g.wetColour[i];
                wi.lube = g.wetColourAmt;   // the outer wet layer takes this colour (Fluid)
            }
            if (g.wetCumOn && lv.worn > 0.0f) {   // the beads are cum then
                for (int i = 0; i < 3; ++i) wi.rgb[i] = g.wetCum[i];
                if (kCum && g.cumByRace)
                    if (auto wk = g_work.find(id); wk != g_work.end() && !wk->second.giverRace.empty())
                        if (auto ri = g_races.find(wk->second.giverRace); ri != g_races.end() && ri->second.cumOwn)
                            for (int i = 0; i < 3; ++i) wi.rgb[i] = ri->second.cum[i];
                wi.cum = g.wetCumAmt;
            }
            out.push_back(wi);
        }
        // the giver's schlong, wet from the round (with cum off this is all there is to it)
        if (g.schlongWet) {
            const double now = Now();
            for (auto& [id, sw] : g_schlong) {
                if (sw.last <= 0.0) continue;
                const float sc = sw.peak * g.schlongStrength * Fade(now - sw.last, g.schlongTime);
                if (sc <= 0.01f) continue;
                auto f = std::find_if(out.begin(), out.end(), [&](const Colour::WetInfo& w) { return w.id == id; });
                if (f == out.end()) { out.push_back(Colour::WetInfo{id, 0.0f, {0.93f, 0.93f, 0.92f}}); f = out.end() - 1; }
                f->schlong = sc;
                if (kCum && g.schlongCum) {   // cum on it: streaks (and a drip from the tip) - part of the cum switched off in Features.h
                    f->scum = g.schlongCumAmt;
                    f->sdrip = kCum && g.schlongDrip;   // the drip from the tip: only with the cum system (Features.h)
                    for (int i = 0; i < 3; ++i) f->srgb[i] = g.schlongCumCol[i];
                    if (g.schlongCumRace)
                        if (auto* who = RE::TESForm::LookupByID<RE::Actor>(id))
                            if (auto ri = g_races.find(RaceKey(who->GetRace())); ri != g_races.end() && ri->second.cumOwn)
                                for (int i = 0; i < 3; ++i) f->srgb[i] = ri->second.cum[i];
                }
            }
        }
        return out;
    }

    // ---- Papyrus ------------------------------------------------------------------------------
    // Int[] LookFor(Actor): [tex, anusTint, blendTint, blendSwap, gloss x100, spec x100, ringToAdd,
    //                        swell x1000, schlong gloss x100, schlong spec x100]
    //   -2 = unchanged, -1 = remove / back to installed, -3 (blend only) = clean up if ours,
    //   tex 0 = set (ask ColourFor for the path), tints >= 0 = 0xRRGGBB
    std::vector<std::int32_t> LookForImpl(RE::Actor* a);
    // the skin-blend ring is coloured straight by the DLL now (Engine.cpp): the script leaves it alone
    std::vector<std::int32_t> LookFor(RE::StaticFunctionTag*, RE::Actor* a) {
        auto out = LookForImpl(a);
        if (out.size() > 6 && Engine::RingDirect()) { out[2] = -2; out[6] = -1; }
        return out;
    }
    // what the ring should be for this man: colour (0xRRGGBB, -1 = his installed paint), swap (1 = white base
    // texture, tinted), add (colour to give a man without the ring, -1 = don't)
    Colour::RingWant RingImpl(RE::Actor* a) {
        Colour::RingWant w;
        if (!a) return w;
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        const Look L = Want(a);
        w.col = L.blend; w.swap = L.swap; w.add = L.add;
        return w;
    }
    std::vector<std::int32_t> LookForImpl(RE::Actor* a) {
        std::vector<std::int32_t> out{-2, -2, -2, 0, -2, -2, -1, -2, -2, -2};
        if (!a) return out;
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        const auto id = a->GetFormID();
        g_lastLook = Now(); ++g_looks;
        if (g_ringReset.erase(id)) {
            // re-rolled: the script removes the ring we added (or restores his own paint); the whole look follows next time
            g_sent.erase(id);
            out[2] = -4;
            return out;
        }
        Look L = Want(a);
        auto it = g_sent.find(id);
        const bool making = L.tex == kRimPending;   // his new rim is still being made: keep the one he has for now
        if (making) L.tex = it != g_sent.end() ? it->second.tex : std::string(kRimPending);
        if (!making) g_pendingTex[id] = L.tex;
        if (it == g_sent.end()) {
            // first time this session: send the whole look
            out[0] = making ? -2 : (L.tex.empty() ? -1 : 0);
            out[1] = L.tint;
            out[2] = L.blend >= 0 ? L.blend : -3;
            out[3] = L.swap;
            out[4] = L.gloss;
            out[5] = L.gloss >= 0 ? L.spec : -1;
            out[6] = L.add;
            out[7] = L.swell;
            out[8] = L.sgloss;
            out[9] = L.sgloss >= 0 ? L.sspec : -1;
            g_sent[id] = {L.tex, L.tint, L.blend, L.swap, L.gloss, L.spec, L.add, L.swell, L.sgloss};
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
        if (L.swell != s.swell) out[7] = L.swell;
        if (L.sgloss != s.sgloss) {
            out[8] = L.sgloss;
            out[9] = L.sgloss >= 0 ? L.sspec : -1;
        }
        s = {L.tex, L.tint, L.blend, L.swap, L.gloss, L.spec, L.add, L.swell, L.sgloss};
        return out;
    }

    RE::BSFixedString ColourFor(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!a) return "";
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        auto it = g_pendingTex.find(a->GetFormID());
        if (it != g_pendingTex.end()) return RE::BSFixedString(it->second.c_str());
        const std::string t = Want(a).tex;
        return RE::BSFixedString(t == kRimPending ? "" : t.c_str());
    }

    // ---- loading screens: Anatomica's script waits a little after each one ---------------------
    double g_loadEnd = 0.0;
    bool g_loading = false;
    struct LoadSink : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (e && e->menuName == RE::LoadingMenu::MENU_NAME) {
                std::lock_guard lk(g_lock);
                g_loading = e->opening;
                if (!e->opening) g_loadEnd = Now();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    LoadSink g_loadSink;
    bool g_sinkAdded = false;

    // seconds the script should still wait (0 = go)
    float LoadPause(RE::StaticFunctionTag*) {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        if (g.loadDelay <= 0.0f) return 0.0f;
        if (g_loading) return g.loadDelay;
        const double left = g.loadDelay - (Now() - g_loadEnd);
        return left > 0.0 ? static_cast<float>(left) : 0.0f;
    }

    // men near the player with their 3D loaded (any cell - the script's own scan only sees the
    // player's cell, so men across a cell border in exteriors were never coloured)
    std::vector<RE::Actor*> NearbyMen(RE::StaticFunctionTag*, float radius) {
        std::vector<RE::Actor*> out;
        if (radius <= 0.0f) {   // 0 = use the menu setting
            std::lock_guard lk(g_lock);
            if (!g_loaded) LoadIni();
            radius = g.radius;
        }
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!pl || !lists) return out;
        const auto ppos = pl->GetPosition();
        for (auto& h : lists->highActorHandles) {
            auto ptr = h.get();
            RE::Actor* a = ptr.get();
            if (!a || a == pl || !a->Is3DLoaded()) continue;
            const auto* npc = a->GetActorBase();
            if (!npc || npc->GetSex() != RE::SEX::kMale) continue;
            if (radius > 0.0f && a->GetPosition().GetDistance(ppos) > radius) continue;
            out.push_back(a);
            if (out.size() >= 48) break;
        }
        return out;
    }

    float ColourInterval(RE::StaticFunctionTag*) {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        return g.interval;
    }

    // called by the gape script while the actor is being penetrated (dil = how dilated, 0-1,
    // partners = partners in a row, partner = who is giving)
    void Worked(RE::StaticFunctionTag*, RE::Actor* a, float dil, float partners, RE::Actor* partner) {
        if (!a) return;
        Drip::OnWorked(a, partner, dil);   // before our lock (it has its own)
        Virgin::OnWorked(a, partner);
        const Size::K sk = partner && partner != a ? Size::For(partner) : Size::K{};   // before our lock (TNG is asked on the VM)
        const Extras::Rough rough = Extras::RoughFor(a, partner);
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        const double now = Now();
        const Levels before = LevelsFor(a->GetFormID(), nullptr);
        auto& w = g_work[a->GetFormID()];
        const bool newRound = w.last <= 0.0 || now - w.last > 20.0;
        dil = std::clamp(std::max(dil, 0.15f), 0.0f, 1.0f);
        // reddening level
        float cur = 0.0f;
        if (w.last > 0.0) cur = w.peak * Fade(now - w.last, g.tenderTime);
        w.peak = std::clamp(std::max(cur, dil), 0.0f, 1.0f);
        {   // glossy skin: like the redness, but on its own clock
            const float gc = w.last > 0.0 ? w.glossPeak * Fade(now - w.last, g.glossyTime) : 0.0f;
            w.glossPeak = std::clamp(std::max(gc, dil), 0.0f, 1.0f);
        }
        // wetness: a new round adds to what is left (up to the cap)
        if (newRound) {
            w.wetBase = (g.wetBuild && g.wetStrength > 0.0f) ? before.wet / g.wetStrength : 0.0f;
            w.partners = 0;
            w.workSecs = 0.0f;
            w.dryShift = 0.0;
            w.dripDone = false;
            w.looseDone = false;
            w.bruiseMul = 1.0f;
            w.szLoose = sk.loose; w.szTender = sk.tender; w.szFirst = partner ? partner->GetFormID() : 0;
            w.roughTender = rough.tender; w.roughTime = rough.time;
            // first time in a while = redder
            const auto lr = g_lastRound.find(a->GetFormID());
            const bool restedNow = lr == g_lastRound.end() || now - lr->second > g.restedMinutes * 60.0;
            w.restedBoost = restedNow ? g.restedExtra : 0.0f;
        } else {
            w.workSecs += static_cast<float>(std::min(now - w.last, 2.0));
        }
        w.wetPeak = std::clamp(g.wetBuild ? w.wetBase + dil : std::max(before.wet, dil), 0.0f, g.wetBuild ? g.wetCap : 1.0f);
        w.partners = std::max(w.partners, static_cast<int>(partners + 0.5f));
        w.last = now;
        w.wetLast = now;
        g_lastRound[a->GetFormID()] = now;
        if (partner && partner != a) {
            if (!w.szFirst || partner->GetFormID() == w.szFirst) { w.szLoose = sk.loose; w.szTender = sk.tender; w.szFirst = partner->GetFormID(); }
            else { w.szLoose = std::max(w.szLoose, sk.loose); w.szTender = std::max(w.szTender, sk.tender); }
            w.giverRace = RaceKey(partner->GetRace());
        }
        if (partner && partner != a) {
            auto& sw = g_schlong[partner->GetFormID()];
            float sc = 0.0f;
            if (sw.last > 0.0) sc = sw.peak * Fade(now - sw.last, g.schlongTime);
            sw.peak = std::clamp(std::max(sc, std::max(dil, 0.4f)), 0.0f, 1.0f);
            sw.last = now;
        }
    }

    // names of the shapes of his schlong (the armor in slot 52 - SOS / TNG), for the shine
    std::vector<RE::BSFixedString> SchlongNodes(RE::StaticFunctionTag*, RE::Actor* a) {
        std::vector<RE::BSFixedString> out;
        if (!a) return out;
        const auto& biped = a->GetBiped2();
        if (!biped) return out;
        auto* obj = biped->objects[RE::BIPED_OBJECTS::kModPelvisSecondary].partClone.get();
        if (!obj) return out;
        RE::BSVisit::TraverseScenegraphGeometries(obj, [&](RE::BSGeometry* geo) {
            if (geo && !geo->name.empty() && out.size() < 16) out.emplace_back(geo->name.c_str());
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        return out;
    }

    // old SMP Cum Drip hook (replaced by Anatomica's own drips, Drip.cpp) - kept so older scripts still bind
    bool DripDue(RE::StaticFunctionTag*, RE::Actor*) { return false; }

    // how much slower the gape closes right now (1 = normal)
    float CloseSlow(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!a) return 1.0f;
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        const float full = Drip::HoldOpen(a->GetFormID());   // still full of cum: stays open longer
        if (!g.slowClose) return full;
        return (1.0f + g.slowCloseAmt * LevelsFor(a->GetFormID(), nullptr).tender) * full;
    }

    // lasting looseness: cur = what he has now, days = game days since it was last updated.
    // Returns the new value (a round that just ended adds to it; time heals it).
    float LooseFor(RE::StaticFunctionTag*, RE::Actor* a, float cur, float days) {
        if (!a) return cur;
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        if (!g.loose) return 0.0f;
        const Virgin::Mods vm = Virgin::For(a->GetFormID());   // experience: how much a round adds, how loose he can get
        float rest = 0.0f;
        if (auto r = g_looseRest.find(a->GetFormID()); r != g_looseRest.end()) { rest = r->second; g_looseRest.erase(r); }
        float v = std::max(0.0f, cur - g.looseRecover * (std::max(0.0f, days) + rest));
        if (auto it = g_work.find(a->GetFormID()); it != g_work.end()) {
            Work& w = it->second;
            if (!w.looseDone && Now() - w.last >= 3.0) {
                w.looseDone = true;
                v += g.looseAdd * vm.looseAdd * w.szLoose * std::max(0.25f, w.peak);
            }
        }
        v = std::clamp(v, 0.0f, std::min(0.6f, g.looseCap * vm.looseCap));
        g_looseSeen[a->GetFormID()] = v;
        return v;
    }


    // ---- status (Status page + hotkey) --------------------------------------------------------
    const char* ChoiceName(int c) {
        static const char* n[] = {"Installed colour", "Pink", "Light pink", "Rose", "Mauve brown", "Skin matched", "Custom", "Random", "Skin-fit"};
        return (c >= kInstalled && c <= kSmart) ? n[c + 1] : "?";
    }
    struct Status {
        bool ok = false;
        std::string name, race, colour;
        float rim[3]{}, skin[3]{}, ring[3]{};
        bool ringOurs = false;   // the skin-blend ring is coloured by Anatomica (else: his own paint / none, untouched)
        float wet = 0, tender = 0, bruise = 0, swell = 0, schlong = 0, gape = 0, loose = 0;
        int partners = 0;
        float secs = 0;
        bool own = false;
    };
    Status StatusOf(RE::Actor* a) {   // caller holds g_lock
        Status st;
        if (!a) return st;
        st.ok = true;
        st.name = a->GetName();
        if (auto* r = a->GetRace()) st.race = r->GetFullName();
        const Pick pk = PickFor(a);
        st.own = g_perMan.count(RefKey(a)) > 0;
        st.colour = pk.active ? ChoiceName(pk.choice) : "Picker off / out of range";
        SkinTone(a, st.skin);
        float pick[3], base[3];
        const int col = pk.choice >= 0 ? pk.choice : InstalledPreset();
        AnusTarget(col, pk.rgb, pick);
        AnusTarget(kSkinPreset, pk.rgb, base);
        const float sMix = (g.method == 0 && pk.choice >= 0) ? pk.strength : 1.0f;
        for (int i = 0; i < 3; ++i) {
            const float sk = 1.0f + (st.skin[i] - 1.0f) * g.skinOnRim;
            st.rim[i] = std::clamp((base[i] + (pick[i] - base[i]) * sMix) / 255.0f * sk, 0.0f, 1.0f);
        }
        {   // the skin-blend ring, as the colour pass works it out (Want)
            const bool addRing = a->IsPlayerRef() ? g.playerRing : g.autoRing;
            if (pk.active && ((g.blendMatch && pk.choice >= 0) || addRing)) {
                const int rc = (g.blendMatch && pk.choice >= 0) ? pk.choice : InstalledPreset();
                const float rs = (g.method == 0 && pk.choice >= 0) ? pk.strength : 1.0f;
                float rp[3], rb[3];
                BlendTarget(rc, pk.rgb, rp);
                if (pk.blendOwn && g.blendMatch && pk.choice >= 0) for (int i = 0; i < 3; ++i) rp[i] = pk.blend[i] * 255.0f;
                BlendTarget(kSkinPreset, pk.rgb, rb);
                for (int i = 0; i < 3; ++i) {
                    const float sk = 1.0f + (st.skin[i] - 1.0f) * g.skinOnRim;
                    st.ring[i] = std::clamp((rb[i] + (rp[i] - rb[i]) * rs) / 255.0f * sk, 0.0f, 1.0f);
                }
                st.ringOurs = true;
            }
        }
        const auto id = a->GetFormID();
        const Levels lv = LevelsFor(id, nullptr);
        st.wet = lv.wet, st.tender = lv.tender, st.bruise = lv.bruise;
        st.swell = g.swell ? std::min(1.0f, lv.tender * g.swellStrength + Virgin::For(id).swell) : 0.0f;
        if (auto it = g_work.find(id); it != g_work.end()) st.partners = it->second.partners, st.secs = it->second.workSecs;
        if (auto it = g_schlong.find(id); it != g_schlong.end())
            st.schlong = it->second.peak * g.schlongStrength * Fade(Now() - it->second.last, g.schlongTime);
        if (auto it = g_looseSeen.find(id); it != g_looseSeen.end()) st.loose = it->second;
        st.gape = Wink::GapeOf(id);
        if (const float eg = Engine::GapeOf(id); eg >= 0.0f) st.gape = std::max(st.gape, eg);
        return st;
    }
    void ColourRow(const char* label, const float c[3], const char* note = nullptr) {
        ImGuiMCP::PushID(label);
        ImGuiMCP::ColorButton("##sw", ImGuiMCP::ImVec4{c[0], c[1], c[2], 1.0f}, ImGuiMCP::ImGuiColorEditFlags_NoTooltip,
                              ImGuiMCP::ImVec2{ImGuiMCP::GetFontSize() * 2.2f, ImGuiMCP::GetFontSize() * 1.1f});
        ImGuiMCP::SameLine();
        const auto b = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        if (note) ImGuiMCP::Text("%s: %s", label, note);
        else ImGuiMCP::Text("%s: #%02X%02X%02X", label, b(c[0]), b(c[1]), b(c[2]));
        ImGuiMCP::PopID();
    }
    void ColourRows(const Status& st) {
        ColourRow("Skin colour", st.skin);
        ColourRow("Anus colour (rim)", st.rim);
        if (st.ringOurs) ColourRow("Ring colour (skin-blend)", st.ring);
        else {
            const float grey[3] = {0.5f, 0.5f, 0.5f};
            ColourRow("Ring colour (skin-blend)", grey, "not coloured by Anatomica (his own RaceMenu paint, or no ring)");
        }
    }
    std::string StatusLine(const Status& st) {
        return std::format("{}: {} | wet {:.0f}% | red {:.0f}% | bruise {:.0f}% | swell {:.0f}% | gape {:.0f}% | loose {:.0f}%",
                           st.name, st.colour, st.wet * 100, st.tender * 100, st.bruise * 100, st.swell * 100, st.gape * 100,
                           st.loose * 100);
    }
    RE::FormID g_inspect = 0;   // last man looked at with the hotkey

    // ---- hotkeys: any key or mouse button with any of Ctrl / Shift / Alt, bound by pressing it ------------
    std::atomic<int> g_capture{0};            // 1 = the next key press becomes the status hotkey, 2 = the move hotkey
    std::atomic<int> g_modsDown{0};           // Ctrl 1 / Shift 2 / Alt 4 held right now
    std::atomic<bool> g_winOpen{false}, g_winMove{false};
    double g_lastCombo = 0.0;
    SKSEMenuFramework::Model::WindowInterface* g_statusWin = nullptr;

    int ModBit(std::uint32_t code) {
        switch (code) {
            case 0x1D: case 0x9D: return 1;   // Ctrl
            case 0x2A: case 0x36: return 2;   // Shift
            case 0x38: case 0xB8: return 4;   // Alt
            default: return 0;
        }
    }
    std::string KeyLabel(int code, int mods) {
        if (code <= 0) return "None";
        static const std::map<int, const char*> names = {
            {0x01, "Esc"}, {0x0E, "Backspace"}, {0x0F, "Tab"}, {0x1C, "Enter"}, {0x39, "Space"}, {0x3A, "Caps Lock"},
            {0x29, "`"}, {0x0C, "-"}, {0x0D, "="}, {0x1A, "["}, {0x1B, "]"}, {0x27, ";"}, {0x28, "'"}, {0x2B, "\\"},
            {0x33, ","}, {0x34, "."}, {0x35, "/"}, {0x45, "Num Lock"}, {0x46, "Scroll Lock"}, {0xC5, "Pause"},
            {0xD2, "Insert"}, {0xD3, "Delete"}, {0xC7, "Home"}, {0xCF, "End"}, {0xC9, "Page Up"}, {0xD1, "Page Down"},
            {0xC8, "Up"}, {0xD0, "Down"}, {0xCB, "Left"}, {0xCD, "Right"}, {0x52, "Num 0"}, {0x4F, "Num 1"},
            {0x50, "Num 2"}, {0x51, "Num 3"}, {0x4B, "Num 4"}, {0x4C, "Num 5"}, {0x4D, "Num 6"}, {0x47, "Num 7"},
            {0x48, "Num 8"}, {0x49, "Num 9"}, {0xB5, "Num /"}, {0x37, "Num *"}, {0x4A, "Num -"}, {0x4E, "Num +"},
            {0x53, "Num ."}, {0x9C, "Num Enter"}, {0x57, "F11"}, {0x58, "F12"}};
        static const char* row1 = "1234567890", * q = "QWERTYUIOP", * a = "ASDFGHJKL", * z = "ZXCVBNM";
        std::string k;
        if (code >= 256) k = std::format("Mouse {}", code - 256 + 1);
        else if (auto it = names.find(code); it != names.end()) k = it->second;
        else if (code >= 0x02 && code <= 0x0B) k = std::string(1, row1[code - 0x02]);
        else if (code >= 0x10 && code <= 0x19) k = std::string(1, q[code - 0x10]);
        else if (code >= 0x1E && code <= 0x26) k = std::string(1, a[code - 0x1E]);
        else if (code >= 0x2C && code <= 0x32) k = std::string(1, z[code - 0x2C]);
        else if (code >= 0x3B && code <= 0x44) k = std::format("F{}", code - 0x3B + 1);
        else k = std::format("Key {:02X}", code);
        std::string m;
        if (mods & 1) m += "Ctrl + ";
        if (mods & 2) m += "Shift + ";
        if (mods & 4) m += "Alt + ";
        return m + k;
    }

    // who the window shows: the man under the crosshair, else the console selection, else the last one, else you
    void PickInspect() {
        RE::Actor* a = nullptr;
        if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
            auto ref = pick->GetActiveTarget().get();
            if (ref) a = ref->As<RE::Actor>();
        }
        if (!a) if (auto sel = RE::Console::GetSelectedRef()) a = sel->As<RE::Actor>();
        if (!a && g_inspect) a = RE::TESForm::LookupByID<RE::Actor>(g_inspect);
        if (!a) a = RE::PlayerCharacter::GetSingleton();
        if (a) g_inspect = a->GetFormID();
    }

    void ApplyWindowState() {
        if (!g_statusWin) return;
        g_statusWin->IsOpen = g_winOpen.load();
        g_statusWin->BlockUserInput = g_winOpen.load() && g_winMove.load();   // cursor only while moving it
    }

    // one button event: track the modifiers, take a binding, or fire a hotkey. Returns true if it was used.
    bool HandleButton(const RE::ButtonEvent* b) {
        if (!b) return false;
        const auto dev = b->GetDevice();
        if (dev != RE::INPUT_DEVICE::kKeyboard && dev != RE::INPUT_DEVICE::kMouse) return false;
        const std::uint32_t raw = b->GetIDCode();
        const int code = dev == RE::INPUT_DEVICE::kMouse ? 256 + static_cast<int>(raw) : static_cast<int>(raw);
        if (dev == RE::INPUT_DEVICE::kKeyboard) {
            if (const int mb = ModBit(raw)) {
                if (b->IsPressed()) g_modsDown |= mb; else g_modsDown &= ~mb;
                return false;   // a modifier alone is never the key
            }
        }
        if (!b->IsDown()) return false;
        const int mods = g_modsDown.load();
        if (const int cap = g_capture.load()) {
            if (code == 256) return false;   // left click: the menu's own button
            std::lock_guard lk(g_lock);
            if (code != 0x01) {   // Esc cancels
                if (cap == 1) g.statusKey = code, g.statusMods = mods;
                else g.moveKey = code, g.moveMods = mods;
                SaveIni();
            }
            g_capture = 0;
            return true;
        }
        int sk, sm, mk, mm;
        {
            std::lock_guard lk(g_lock);
            sk = g.statusKey, sm = g.statusMods, mk = g.moveKey, mm = g.moveMods;
        }
        const double now = Now();
        if (now - g_lastCombo < 0.2) return false;   // the same press seen twice (game + menu input)
        if (sk > 0 && code == sk && mods == sm) {
            g_lastCombo = now;
            if (!g_winOpen) {
                PickInspect();
                g_winOpen = true;
            } else {
                // looking at someone else: switch to him; looking at the same man (or nobody): close
                const RE::FormID before = g_inspect;
                PickInspect();
                if (g_inspect == before) { g_winOpen = false; g_winMove = false; }
            }
            ApplyWindowState();
            if (g.statusMsg && g_winOpen) {
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(g_inspect)) {
                    std::string line;
                    {
                        std::lock_guard lk(g_lock);
                        if (!g_loaded) LoadIni();
                        line = StatusLine(StatusOf(a));
                    }
                    RE::SendHUDMessage::ShowHUDMessage(line.c_str());
                }
            }
            return true;
        }
        if (mk > 0 && code == mk && mods == mm && g_winOpen) {
            g_lastCombo = now;
            g_winMove = !g_winMove;
            ApplyWindowState();
            return true;
        }
        return false;
    }

    struct KeySink : RE::BSTEventSink<RE::InputEvent*> {
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* ev, RE::BSTEventSource<RE::InputEvent*>*) override {
            if (!ev) return RE::BSEventNotifyControl::kContinue;
            for (auto* e = *ev; e; e = e->next) {
                if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
                const auto* b = e->AsButtonEvent();
                if (!b) continue;
                // in menus only the binding is taken (not the hotkeys)
                auto* ui = RE::UI::GetSingleton();
                if (ui && ui->GameIsPaused() && !g_capture && !(b->GetDevice() == RE::INPUT_DEVICE::kKeyboard && ModBit(b->GetIDCode()))) continue;
                HandleButton(b);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    // the menu framework's own input hook: sees keys while its windows are open (binding, unlocking the window)
    bool __stdcall FrameworkInput(RE::InputEvent* ev) {
        for (auto* e = ev; e; e = e->next) {
            if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
            HandleButton(e->AsButtonEvent());
        }
        return false;
    }

    // ---- the status window: movable, live, over the game -------------------------------------------
    void Bar(const char* label, float v) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s %.0f%%", label, v * 100.0f);
        ImGuiMCP::ProgressBar(std::clamp(v, 0.0f, 1.0f), ImGuiMCP::ImVec2{ImGuiMCP::GetFontSize() * 14.0f, 0.0f}, buf);
    }
    void __stdcall RenderStatusWindow() {
        if (!g_winOpen) return;
        const bool moving = g_winMove;
        int flags = ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize | ImGuiMCP::ImGuiWindowFlags_NoCollapse | ImGuiMCP::ImGuiWindowFlags_NoFocusOnAppearing;
        if (!moving) flags |= ImGuiMCP::ImGuiWindowFlags_NoInputs | ImGuiMCP::ImGuiWindowFlags_NoNav;
        ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2{40.0f, 200.0f}, ImGuiMCP::ImGuiCond_FirstUseEver);
        ImGuiMCP::SetNextWindowBgAlpha(moving ? 0.9f : 0.6f);
        bool open = true;
        if (ImGuiMCP::Begin("Anatomica status", moving ? &open : nullptr, flags)) {
            RE::Actor* a = g_inspect ? RE::TESForm::LookupByID<RE::Actor>(g_inspect) : nullptr;
            if (!a) {
                ImGuiMCP::TextUnformatted("Look at a man and press the status hotkey.");
            } else {
                Status st;
                {
                    std::lock_guard lk(g_lock);
                    if (!g_loaded) LoadIni();
                    st = StatusOf(a);
                }
                ImGuiMCP::Text("%s  (%s)", st.name.c_str(), st.race.c_str());
                ImGuiMCP::Text("Colour pick: %s%s", st.colour.c_str(), st.own ? " (his own)" : "");
                ColourRows(st);
                Bar("Gape", st.gape);
                Bar("Looseness", st.loose / 0.6f);
                Bar("Wet", st.wet);
                Bar("Red", st.tender);
                Bar("Bruise", st.bruise);
                Bar("Swelling", st.swell);
                if (st.schlong > 0.01f) Bar("Schlong shine", st.schlong);
                ImGuiMCP::Text("This round: %d partner(s), %.0f s", st.partners, st.secs);
                ImGuiMCP::TextUnformatted(Drip::Status(a).c_str());
                ImGuiMCP::TextUnformatted(Virgin::Status(a).c_str());
            }
            if (moving) {
                ImGuiMCP::Separator();
                ImGuiMCP::TextDisabled("Drag the title bar to move it.");
                if (ImGuiMCP::Button("Lock in place")) { g_winMove = false; ApplyWindowState(); }
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Close")) open = false;
            }
        }
        ImGuiMCP::End();
        if (!open) { g_winOpen = false; g_winMove = false; ApplyWindowState(); }
    }

    KeySink g_keySink;

    // ---- healing: sleeping / waiting and healing magic clear the after-sex looks ---------------
    struct SleepSink : RE::BSTEventSink<RE::TESSleepStopEvent> {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStopEvent*, RE::BSTEventSource<RE::TESSleepStopEvent>*) override {
            std::lock_guard lk(g_lock);
            if (g.healSleep) {
                if (!g_work.empty()) logger::info("Anatomica: sleeping / waiting cleared the after-sex looks of {} men", g_work.size());
                g_work.clear();
                g_schlong.clear();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    bool HealthRestore(const RE::EffectSetting* mgef) {
        if (!mgef || mgef->IsHostile() || mgef->data.primaryAV != RE::ActorValue::kHealth) return false;
        const auto arch = mgef->GetArchetype();
        return arch == RE::EffectSetting::Archetype::kValueModifier || arch == RE::EffectSetting::Archetype::kPeakValueModifier ||
               arch == RE::EffectSetting::Archetype::kDualValueModifier;
    }
    std::unordered_map<RE::FormID, double> g_drankAt;   // a potion / food just now: its own effect is not counted again as a spell
    // heal part of his redness, bruising and swelling (fraction 0-1). Caller holds g_lock.
    void HealPart(RE::FormID id, float frac, const char* what) {
        auto it = g_work.find(id);
        if (it == g_work.end() || frac <= 0.0f) return;
        frac = std::clamp(frac, 0.0f, 1.0f);
        Work& w = it->second;
        if (w.peak <= 0.01f && w.bruiseMul <= 0.01f) return;
        w.peak *= 1.0f - frac;
        w.bruiseMul *= 1.0f - frac;
        logger::info("Anatomica: {} healed {:.1f}% of the redness of {:08X}", what, frac * 100.0f, id);
    }
    // drinking a potion / eating: how much health it gives back decides how much it heals
    struct DrinkSink : RE::BSTEventSink<RE::TESEquipEvent> {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* e, RE::BSTEventSource<RE::TESEquipEvent>*) override {
            if (!e || !e->equipped || !e->actor) return RE::BSEventNotifyControl::kContinue;
            auto* item = RE::TESForm::LookupByID<RE::AlchemyItem>(e->baseObject);
            if (!item || item->IsPoison()) return RE::BSEventNotifyControl::kContinue;
            float total = 0.0f;
            for (auto* eff : item->effects)
                if (eff && HealthRestore(eff->baseEffect)) total += eff->effectItem.magnitude * static_cast<float>(std::max<std::uint32_t>(1, eff->effectItem.duration));
            if (total <= 0.0f) return RE::BSEventNotifyControl::kContinue;
            const RE::FormID id = e->actor->GetFormID();
            std::lock_guard lk(g_lock);
            g_drankAt[id] = Now();
            if (!g.healMagic) return RE::BSEventNotifyControl::kContinue;
            const bool food = item->IsFood();
            if (food && !g.healFoodOn) return RE::BSEventNotifyControl::kContinue;
            HealPart(id, total * (food ? g.healFood : g.healPotion) / 100.0f, food ? "food / drink" : "a potion");
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    struct MagicSink : RE::BSTEventSink<RE::TESMagicEffectApplyEvent> {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* e, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override {
            if (!e || !e->target) return RE::BSEventNotifyControl::kContinue;
            const auto* mgef = RE::TESForm::LookupByID<RE::EffectSetting>(e->magicEffect);
            if (!HealthRestore(mgef)) return RE::BSEventNotifyControl::kContinue;
            // only real healing: not abilities and always-on effects that games and mods (re)apply all the time
            if (mgef->data.castingType == RE::MagicSystem::CastingType::kConstantEffect) return RE::BSEventNotifyControl::kContinue;
            const RE::FormID id = e->target->GetFormID();
            std::lock_guard lk(g_lock);
            if (!g.healMagic || !g.healSpellOn) return RE::BSEventNotifyControl::kContinue;
            if (auto d = g_drankAt.find(id); d != g_drankAt.end() && Now() - d->second < 2.0) return RE::BSEventNotifyControl::kContinue;   // the potion itself
            HealPart(id, g.healSpell / 100.0f, "a healing spell");
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    DrinkSink g_drinkSink;
    SleepSink g_sleepSink;
    MagicSink g_magicSink;

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
        box("Skin-fit colour (follows Skin rules)", kSmartBit);
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
    std::vector<WetInfo> WetNow() { return WetNowImpl(); }
    void WorkedNow(RE::Actor* a, float dil, float partners, RE::Actor* partner) { Worked(nullptr, a, dil, partners, partner); }
    std::vector<std::int32_t> LookNow(RE::Actor* a) { return LookFor(nullptr, a); }
    std::string ColourPath(RE::Actor* a) { const RE::BSFixedString f = ColourFor(nullptr, a); return f.c_str() ? f.c_str() : ""; }
    float CloseSlowFor(RE::Actor* a) { return CloseSlow(nullptr, a); }
    float LooseNow(RE::Actor* a, float cur, float days) { return LooseFor(nullptr, a, cur, days); }
    Snapshot SnapshotOf(RE::Actor* a) {
        Snapshot o;
        if (!a) return o;
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        const Status st = StatusOf(a);
        o.wet = st.wet; o.tender = st.tender; o.bruise = st.bruise; o.swell = st.swell; o.schlong = st.schlong;
        o.gape = st.gape; o.loose = st.loose; o.partners = st.partners;
        o.lube = kLube && g.wetColourOn; o.cum = g.wetCumOn;
        if (auto ar = g_arousal.find(a->GetFormID()); ar != g_arousal.end()) o.aroused = ar->second.wet > 0.0f;
        return o;
    }
    void AddWet(RE::Actor* a, float amount) {   // lube: wet, nothing else
        if (!a) return;
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        auto& w = g_work[a->GetFormID()];
        const double now = Now();
        if (w.last <= 0.0) { w.last = now - 3.5; w.looseDone = true; }
        w.wetPeak = std::clamp(std::max(w.wetPeak, amount), 0.0f, g.wetBuild ? g.wetCap : 1.0f);
        w.wetLast = now;
        g_sent.erase(a->GetFormID());
    }
    void Bathed(RE::FormID id, bool clearWet, float soothe) {   // washing: dry him, ease the redness
        std::lock_guard lk(g_lock);
        if (auto it = g_work.find(id); it != g_work.end()) {
            if (clearWet) { it->second.wetPeak = 0.0f; it->second.wetBase = 0.0f; it->second.glossPeak = 0.0f; }
            it->second.peak *= std::clamp(1.0f - soothe, 0.0f, 1.0f);
        }
        if (clearWet) g_schlong.erase(id);
        g_sent.erase(id);
    }
    void SetArousal(RE::FormID id, float wet, float sens) {
        std::lock_guard lk(g_lock);
        if (wet <= 0.0f && sens <= 0.0f) { g_arousal.erase(id); return; }
        g_arousal[id] = {wet, sens};
    }
    void RestHours(RE::FormID id, float hours) { std::lock_guard lk(g_lock); g_looseRest[id] += std::max(0.0f, hours) / 24.0f; }
    std::vector<RE::Actor*> Men() { return NearbyMen(nullptr, 0.0f); }
    RingWant RingNow(RE::Actor* a) { return RingImpl(a); }
    float Interval() { return ColourInterval(nullptr); }
    float LoadPauseNow() { return LoadPause(nullptr); }
    std::vector<std::string> SchlongShapeNames(RE::Actor* a) {
        std::vector<std::string> out;
        for (auto& n : SchlongNodes(nullptr, a)) out.emplace_back(n.c_str() ? n.c_str() : "");
        return out;
    }
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
        vm->RegisterFunction("NearbyMen"sv, "AnatomicaNative"sv, NearbyMen);
        vm->RegisterFunction("LoadPause"sv, "AnatomicaNative"sv, LoadPause);
        vm->RegisterFunction("Worked"sv, "AnatomicaNative"sv, Worked);
        vm->RegisterFunction("SchlongNodes"sv, "AnatomicaNative"sv, SchlongNodes);
        vm->RegisterFunction("DripDue"sv, "AnatomicaNative"sv, DripDue);
        vm->RegisterFunction("CloseSlow"sv, "AnatomicaNative"sv, CloseSlow);
        vm->RegisterFunction("LooseFor"sv, "AnatomicaNative"sv, LooseFor);
        Drip::RegisterFuncs(vm);
        Bridge::RegisterFuncs(vm);
        Engine::RegisterFuncs(vm);
    }

    // the giver's race cum colour (Cum & drips page); false = none set
    bool CumColour(RE::Actor* giver, float out[3]) {
        if (!giver) return false;
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        auto it = g_races.find(RaceKey(giver->GetRace()));
        if (!kCum || it == g_races.end() || !it->second.cumOwn) return false;   // per-race cum colours: cum system only
        for (int i = 0; i < 3; ++i) out[i] = it->second.cum[i];
        return true;
    }

    // the cum settings that live with the colours (shown on the Cum & drips page)
    void RenderCumColours() {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        BuildRaces();
        bool changed = false;
        changed |= ImGuiMCP::Checkbox("The wetness colour (After sex page) follows the giver's race too", &g.cumByRace);
        Help("With the wetness colour switched on, the gloss on his anus takes the cum colour of the man who gave.");
        ImGuiMCP::TextDisabled(kCum ? "Tick a race to give it its own cum colour (used for his drips, squirts, puddles, the film on the rim, ...)"
                                    : "Tick a race to give it its own cum colour (the cum in the wetness, and the cum on his schlong - After sex page)");
        if (g_raceRows.empty()) ImGuiMCP::TextDisabled("Races appear here once a game is loaded.");
        if (!g_raceRows.empty() && ImGuiMCP::TreeNode("Cum colour per race")) {
            for (const auto& r : g_raceRows) {
                auto& rs = g_races[r.key];
                ImGuiMCP::PushID(r.key.c_str());
                if (ImGuiMCP::Checkbox("##own", &rs.cumOwn)) changed = true;
                ImGuiMCP::SameLine();
                if (rs.cumOwn) {
                    changed |= ImGuiMCP::ColorEdit3("##cum", rs.cum, kWheel | ImGuiMCP::ImGuiColorEditFlags_NoInputs);
                    ImGuiMCP::SameLine();
                }
                ImGuiMCP::TextUnformatted(r.name.c_str());
                ImGuiMCP::PopID();
            }
            if (ConfirmButton("Clear all race cum colours")) {
                for (auto& [k, v] : g_races) v.cumOwn = false;
                changed = true;
            }
            ImGuiMCP::TreePop();
        }
        if (changed) SaveIni();
    }

    int RingTexLevel() { std::lock_guard lk(g_lock); if (!g_loaded) LoadIni(); return g.ringRes; }
    void TidyMade() { std::lock_guard lk(g_lock); if (!g_loaded) LoadIni(); CleanGenFolder(g.genBudget); }
    void SaveNow() {
        std::lock_guard lk(g_lock);
        SaveIni();
    }

    void Reset() {
        if (!g_sinkAdded) {
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_loadSink);
                g_sinkAdded = true;
            }
        }
        static bool keySink = false;
        if (!keySink) {
            if (auto* in = RE::BSInputDeviceManager::GetSingleton()) {
                in->AddEventSink<RE::InputEvent*>(&g_keySink);
                keySink = true;
            }
        }
        static bool scriptSinks = false;
        if (!scriptSinks) {
            if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) {
                src->AddEventSink<RE::TESSleepStopEvent>(&g_sleepSink);
                src->AddEventSink<RE::TESMagicEffectApplyEvent>(&g_magicSink);
                src->AddEventSink<RE::TESEquipEvent>(&g_drinkSink);
                scriptSinks = true;
            }
        }
        std::lock_guard lk(g_lock);
        g_loadEnd = Now();   // a save was just loaded
        g_loading = false;
        g_sent.clear();
        g_pendingTex.clear();
        g_work.clear();
        g_schlong.clear();
        g_looks = 0;
        Drip::Reset();
        KickScript();   // the script's loop restarts even if its load event never arrives
    }

    float TenderLevel(RE::FormID id) {
        std::lock_guard lk(g_lock);
        if (!g_loaded || !g.sensitive) return 0.0f;
        float sens = 0.0f;
        if (auto ar = g_arousal.find(id); ar != g_arousal.end()) sens = ar->second.sens;
        return std::max(LevelsFor(id, nullptr).tender, sens);
    }
    float SensClench() { std::lock_guard lk(g_lock); return g.sensClench; }
    float SensBreath() { std::lock_guard lk(g_lock); return g.sensBreath; }

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
        {
            float m = g.radius / 70.0f;
            if (ImGuiMCP::SliderFloat("Distance", &m, 1.0f, 100.0f, "%.0f m")) {
                g.radius = m * 70.0f;
                changed = true;
            }
            Help("Only men this close to you are coloured (and get after-sex looks). 30 m covers a room or a camp; "
                 "further = more men checked each time.");
        }
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
        changed |= Pct("Skin tone on the rim", &g.skinOnRim);
        Help("How much of each man's own skin tone is mixed into the rim colour. 100% = it always sits in his skin "
             "(a blue anus on red skin comes out dark); lower = picked colours show truer on strongly coloured skin "
             "(argonians, khajiit, dunmer).");
        changed |= ImGuiMCP::ColorEdit3("Custom colour", g.custom, kWheel);
        Help("Used wherever \"Custom\" is picked (always tinted, in both methods). Pick the colour you want the anus to be; "
             "it is still shaded by the skin tone.");
        changed |= ImGuiMCP::Checkbox("Skin-blend overlay matches the colour", &g.blendMatch);
        Help("The RaceMenu \"Anatomica Blend\" body paint (the soft ring around the anus) takes the same colour.");
        changed |= ImGuiMCP::Checkbox("Give NPCs the skin-blend ring", &g.autoRing);
        Help("Men who don't have the \"Anatomica Blend\" paint get it automatically (one free RaceMenu body-overlay slot), "
             "coloured to match, so the anus fades into the skin instead of having a hard edge.");
        changed |= ImGuiMCP::Checkbox("Give you the skin-blend ring too", &g.playerRing);
        Help("The same for you, in a free RaceMenu body-paint slot - no need to add Anatomica Blend in RaceMenu yourself. "
             "Off: only the ring you painted on yourself in RaceMenu (if any) is coloured.");
        {
            static const char* res[] = {"2048 (5.5 MB each)", "1024 (1.4 MB each)", "512 (0.35 MB each)"};
            changed |= ImGuiMCP::Combo("Ring colour texture size", &g.ringRes, res, 3);
            Help("Each ring colour is saved as its own texture. The ring is a soft fade, so 1024 looks the same in game "
                 "at a quarter of the size; 512 is smaller still, slightly softer. Applies to colours made from now on.");
            changed |= ImGuiMCP::SliderInt("Made textures may take up (MB)", &g.genBudget, 20, 2000);
            g_genBudgetMB = g.genBudget;
            static std::string sz;
            if (ImGuiMCP::Button("Check size")) sz = std::format("{:.0f} MB now", GenFolderBytes() / 1048576.0);
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Tidy now")) sz = CleanGenFolder(g.genBudget, true);   // (the page holds g_lock)
            ImGuiMCP::SameLine();
            if (ConfirmButton("Delete them all")) {   // a fresh start: everything is made again as it is needed
                sz = CleanGenFolder(g.genBudget, true, true);
                g_sent.clear();   // every man's look is sent again, so the textures he needs are remade
            }
            if (!sz.empty()) { ImGuiMCP::SameLine(); ImGuiMCP::TextUnformatted(sz.c_str()); }
            Help("Anatomica makes textures for itself as it goes (rims, ring colours, glossy skin) and keeps them in "
                 "textures\\actors\\character\\male\\Anatomica\\gen (MO2: in overwrite). They are a cache - anything "
                 "deleted is made again when needed. Over this size the oldest are removed at the next game start "
                 "(or now, with Tidy now), down to three quarters of it.");
        }

        if (Section("Who gets which colour")) {
        changed |= ComboValue("Everyone (default)", &g.def, 1, kDefaultItems, 9);
        changed |= ComboValue("You (player)", &g.player, 2, kPlayerItems, 10);
        changed |= ComboValue("Your followers", &g.follower, 2, kFollowerItems, 10);
        ImGuiMCP::TextDisabled("Per-race picks: Per race page.");

        }
        if (Section("Random picks from")) {
        changed |= PoolEditor(g.pool, g.palette);

        }
        if (Section("Skin-fit colour")) {
        changed |= ComboValue("When no Skin rule matches", &g.harmony, 0, kHarmonyItems, 3);
        Help("Skin-fit first follows your Skin rules page (e.g. red argonian -> blue anus). With no matching rule it "
             "reads his skin colour and picks an anus colour around it. Same family = his own hue, "
             "deeper. Neighbouring = hues next to his on the colour wheel (a red argonian gets reds, oranges, magentas - "
             "never green). Contrasting = the opposite side of the wheel, for bold looks.");
        }

        if (ConfirmButton("Re-roll everyone")) {
            ++g.seed;
            for (const auto& [rid, snt] : g_sent) g_ringReset.insert(rid);   // their rings are redone too
            changed = true;
        }
        Help("Gives every man a new Random / Skin-fit colour.");
        ImGuiMCP::SameLine();
        {
            auto sel = RE::Console::GetSelectedRef();
            auto* act = sel ? sel->As<RE::Actor>() : nullptr;
            const std::string lbl = act ? std::format("Re-roll {}", act->GetName()) : std::string("Re-roll selected man");
            if (ImGuiMCP::Button(lbl.c_str()) && act) {
                ++g_reroll[RefKey(act)];
                g_ringReset.insert(act->GetFormID());
                changed = true;
            }
            Help("Open the console (~) and click a man to select him, then press this to give just him a new random colour.");
        }
        ImGuiMCP::Spacing();
        if (ConfirmButton("Reset colour settings")) {
            const Settings d;
            g.enabled = d.enabled;
            g.interval = d.interval;
            g.radius = d.radius;
            g.scope = d.scope;
            g.method = d.method;
            g.strength = d.strength;
            g.randStrength = d.randStrength;
            g.strengthMax = d.strengthMax;
            std::copy(std::begin(d.custom), std::end(d.custom), g.custom);
            g.blendMatch = d.blendMatch;
            g.autoRing = d.autoRing; g.playerRing = d.playerRing;
            g.def = d.def;
            g.player = d.player;
            g.pool = d.pool;
            g.harmony = d.harmony;
            g.follower = d.follower;
            g.skinOnRim = d.skinOnRim;
            changed = true;
        }
        if (changed) SaveIni();
        ImGuiMCP::Spacing();
        ImGuiMCP::TextDisabled("Save size: a few small RaceMenu entries per coloured man (well under 1 KB each).");
        ImGuiMCP::Spacing();
        ImGuiMCP::TextWrapped("Note: Random and Skin-fit picks are fixed per man. After changing the random colours, "
                              "Skin rules or the Skin-fit setting, press Re-roll everyone (or Re-roll one man) to see the "
                              "new picks.");
    }

    void __stdcall RenderGeneral() {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        bool changed = false;
        ImGuiMCP::TextWrapped("Settings for all of Anatomica's script: the gape, movement, colours, wetness and reddening.");
        ImGuiMCP::Separator();
        changed |= ImGuiMCP::SliderFloat("Wait after loading screens", &g.loadDelay, 0.0f, 30.0f, "%.0f s");
        Help("After every loading screen (loading a save, going through a door, fast travel) Anatomica's script waits this "
             "long before doing anything, so other mods' scripts get the busy first seconds. 0 = no wait.");
        if (Section("Presets")) {
        ImGuiMCP::TextWrapped("Save every Anatomica setting (all pages) to a slot, and load it again later - or share the "
                              "file (Data\\SKSE\\Plugins\\AnatomicaMenu\\Presets\\SlotN.ini).");
        for (int slot = 1; slot <= 5; ++slot) {
            ImGuiMCP::PushID(slot);
            const std::filesystem::path f = std::format("Data/SKSE/Plugins/AnatomicaMenu/Presets/Slot{}.ini", slot);
            std::error_code ec;
            const bool has = std::filesystem::exists(f, ec);
            ImGuiMCP::Text("Slot %d%s", slot, has ? "" : " (empty)");
            ImGuiMCP::SameLine();
            if (ConfirmButton(std::format("Save##s{}", slot).c_str())) {
                SaveIni();
                std::filesystem::create_directories(f.parent_path(), ec);
                std::filesystem::copy_file(kIni, f, std::filesystem::copy_options::overwrite_existing, ec);
            }
            if (has) {
                ImGuiMCP::SameLine();
                if (ConfirmButton(std::format("Load##l{}", slot).c_str())) {
                    std::filesystem::copy_file(f, kIni, std::filesystem::copy_options::overwrite_existing, ec);
                    g = Settings{};
                    g_races.clear();
                    g_perMan.clear();
                    g_reroll.clear();
                    g_rules.clear();
                    g_sent.clear();
                    LoadIni();
                    UI::ReloadSettings();
                }
            }
            ImGuiMCP::PopID();
        }

        }
        ImGuiMCP::Spacing();
        if (ConfirmButton("Reset general settings")) {
            g.loadDelay = Settings{}.loadDelay;
            changed = true;
        }
        if (changed) SaveIni();
    }

    void RenderRacesBody();
    void __stdcall RenderRaces() {
        RenderRacesBody();
        // cum colour per race: off together with the rest of Anatomica's cum (Features.h) - they live on the Cum & drips page
    }
    void RenderRacesBody() {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        BuildRaces();
        bool changed = false;
        if (!g_raceDefaults.empty())
            ImGuiMCP::TextDisabled("Races set in the installer start with that colour; Reset all races goes back to them.");
        ImGuiMCP::TextDisabled(kCum ? "Cum colours per race are on the Cum & drips page." : "Cum colours per race are at the bottom of this page.");
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

    std::string g_ruleNote;
    RE::Actor* TestWho();
    void __stdcall RenderRules() {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        BuildRaces();
        bool changed = false;
        ImGuiMCP::TextColored(ImGuiMCP::ImVec4{0.55f, 0.85f, 1.0f, 1.0f},
                              "Tip: colours can be dragged and dropped - drag any colour swatch onto another to copy it across.");
        ImGuiMCP::Separator();
        {
            static const char* modes[] = {"Any matching rule, at random", "His race's rules win over \"All races\""};
            changed |= ImGuiMCP::Combo("When several rules match", &g.ruleMode, modes, 2);
            Help("At random: every rule that matches him counts - an All-races rule as much as one for his race - and he gets "
                 "one of them (fixed per man). That gives the most variety, e.g. a blue argonian can get purple from his race's "
                 "rule or another colour from an All-races rule.\nRace wins: if a rule for his own race matches, the All-races "
                 "rules are left out for him; they only apply to races with no matching rule of their own.");
        }
        ImGuiMCP::TextWrapped("Skin rules decide the anus colour from the man's skin colour. They are used by Smart random "
                              "(pick it for Everyone, a race, or tick Smart in a Random list). Example: race Argonian, "
                              "skin red within 35 degrees -> blue anus; skin blue -> purple anus. If several rules match "
                              "one man he gets one of them (fixed per man; which ones count is set above). No match = the "
                              "normal Smart colour.");
        ImGuiMCP::TextDisabled("Skin colour is read from the character's skin tone (RaceMenu / CK tint). Skins coloured only "
                               "by a texture, not a tint, read as plain.");
        ImGuiMCP::Separator();
        std::vector<const char*> items{"All races"};
        for (const auto& r : g_raceRows) items.push_back(r.plain.c_str());
        int del = -1;
        // the man selected in the console (or you): which rules match him, and the one he gets
        RE::Actor* who = TestWho();
        std::vector<int> hits;
        int chosen = -1;
        bool smart = false;
        if (who) {
            float sk[3];
            SkinTone(who, sk);
            hits = RuleHits(who, sk);
            chosen = RuleChosen(who, hits);
            const Pick pk = PickFor(who);
            smart = pk.active && pk.ruleIdx >= 0;
            const char* nm = who->GetDisplayFullName();
            if (hits.empty()) ImGuiMCP::TextDisabled("%s: no rule matches his skin - he gets the normal Smart colour.", nm ? nm : "?");
            else if (smart) ImGuiMCP::TextColored(ImGuiMCP::ImVec4{0.5f, 0.95f, 0.5f, 1.0f}, "%s: the rule marked APPLIED is his colour now (%d match).", nm ? nm : "?", static_cast<int>(hits.size()));
            else ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.8f, 0.35f, 1.0f}, "%s: %d rule(s) match, but his colour is not Smart random - skin rules are not used for him.", nm ? nm : "?", static_cast<int>(hits.size()));
        }
        if (!g_rules.empty())
            ImGuiMCP::TextDisabled("On | Race | Skin colour | matches within | Anus colour | Anus blend (ring) colour - tick to give it its own");
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
            changed |= ImGuiMCP::Checkbox("##bo", &r.blendOwn);
            ImGuiMCP::SameLine();
            if (r.blendOwn) changed |= ImGuiMCP::ColorEdit3("anus blend##b", r.blend, kWheel | ImGuiMCP::ImGuiColorEditFlags_NoInputs);
            else ImGuiMCP::TextDisabled("anus blend: follows the anus");
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Remove")) del = static_cast<int>(i);
            if (static_cast<int>(i) == chosen) {
                ImGuiMCP::SameLine();
                if (smart) ImGuiMCP::TextColored(ImGuiMCP::ImVec4{0.5f, 0.95f, 0.5f, 1.0f}, "<< APPLIED");
                else ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.8f, 0.35f, 1.0f}, "<< his pick (if he were Smart random)");
            } else if (std::find(hits.begin(), hits.end(), static_cast<int>(i)) != hits.end()) {
                ImGuiMCP::SameLine();
                ImGuiMCP::TextDisabled("< matches him too");
            }
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
        ImGuiMCP::SameLine();
        if (static_cast<int>(g_rules.size()) < kMaxRules && ImGuiMCP::Button("Add a rule for the selected man's skin")) {
            if (RE::Actor* who = TestWho()) {
                Rule r;
                r.race = RaceKey(who->GetRace());
                SkinTone(who, r.skin);
                r.tol = 25.0f;
                const Status st = StatusOf(who);   // start from the anus colour he has now
                // the colours he has now, without his skin tone (the colour pass multiplies that in again)
                auto unskin = [&](float v, int i) { const float sk = 1.0f + (st.skin[i] - 1.0f) * g.skinOnRim; return std::clamp(sk > 0.05f ? v / sk : v, 0.0f, 1.0f); };
                for (int i = 0; i < 3; ++i) r.anus[i] = unskin(st.rim[i], i);
                if (st.ringOurs) { r.blendOwn = true; for (int i = 0; i < 3; ++i) r.blend[i] = unskin(st.ring[i], i); }   // and the ring he has now
                g_rules.push_back(r);
                g_ruleNote = std::format("added: {} ({}), skin {:02X}{:02X}{:02X}", who->GetName(), st.race,
                                         static_cast<int>(r.skin[0] * 255), static_cast<int>(r.skin[1] * 255), static_cast<int>(r.skin[2] * 255));
                changed = true;
            }
        }
        Help("The man you click in the console (~), or you: a new rule for his race and his exact skin tone, starting with "
             "the anus and anus blend (ring) colours he has now - pick the colours you want for skins like his.");
        if (!g_ruleNote.empty()) ImGuiMCP::TextDisabled("%s", g_ruleNote.c_str());
        ImGuiMCP::SameLine();
        if (ConfirmButton("Load the default rules")) {
            g_rules = DefaultRules();
            changed = true;
        }
        Help("Replaces the list with the rules that come with Anatomica (argonian scale colours). Tick = rule on. Race = who it applies to. Skin swatch + degrees = which skin colours match (how far round the "
             "colour wheel from that colour). Anus swatch = the colour they get.");
        if (changed) {
            SaveIni();
        }
    }

    struct KeyName {
        int code;
        const char* name;
    };
    constexpr KeyName kKeys[] = {{0, "None"}, {0x43, "F9"}, {0x44, "F10"}, {0x57, "F11"}, {0x58, "F12"}, {0xD2, "Insert"},
                                 {0xD3, "Delete"}, {0xC7, "Home"}, {0xCF, "End"}, {0xC9, "Page Up"}, {0xD1, "Page Down"},
                                 {0x46, "Scroll Lock"}, {0xC5, "Pause"}, {0x52, "Num 0"}, {0x4F, "Num 1"}, {0x50, "Num 2"},
                                 {0x51, "Num 3"}, {0x4B, "Num 4"}, {0x4C, "Num 5"}, {0x4D, "Num 6"}, {0x47, "Num 7"},
                                 {0x48, "Num 8"}, {0x49, "Num 9"}, {0xB5, "Num /"}, {0x37, "Num *"}, {0x4A, "Num -"},
                                 {0x4E, "Num +"}, {0x53, "Num ."}};

    void __stdcall RenderStatus() {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        bool changed = false;
        // hotkeys: bound by pressing them (any key or mouse button, with Ctrl / Shift / Alt if you like)
        auto bindRow = [&](const char* what, int which, int& key, int& mods) {
            ImGuiMCP::PushID(which);
            ImGuiMCP::Text("%s:", what);
            ImGuiMCP::SameLine();
            if (g_capture == which) {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.85f, 0.3f, 1.0f}, "press the keys now... (Esc = cancel)");
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Cancel")) g_capture = 0;
            } else {
                ImGuiMCP::TextUnformatted(KeyLabel(key, mods).c_str());
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Set")) g_capture = which;
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Clear")) { key = 0; mods = 0; changed = true; }
            }
            ImGuiMCP::PopID();
        };
        bindRow("Status window hotkey", 1, g.statusKey, g.statusMods);
        Help("Press Set, then the key you want - hold Ctrl, Shift and/or Alt with it for a combination (e.g. Ctrl + Shift + "
             "Alt + F1). Mouse buttons work too. In game: look at a man and press it to open a live window with his stats; "
             "press it on another man to switch to him, on the same man to close it.");
        bindRow("Move the window", 2, g.moveKey, g.moveMods);
        Help("While the window is open: press it to unlock the window (mouse cursor, game paused for input), drag it by "
             "its title bar, then press it again or click Lock in place. The position is remembered.");
        changed |= ImGuiMCP::Checkbox("Also show a one-line message when the hotkey is pressed", &g.statusMsg);
        {
            bool open = g_winOpen;
            if (ImGuiMCP::Checkbox("Show the status window now", &open)) {
                if (open) PickInspect();
                g_winOpen = open; g_winMove = false; ApplyWindowState();
            }
        }
        ImGuiMCP::Separator();
        RE::Actor* a = nullptr;
        if (auto sel = RE::Console::GetSelectedRef()) a = sel->As<RE::Actor>();
        if (!a && g_inspect) a = RE::TESForm::LookupByID<RE::Actor>(g_inspect);
        if (!a) {
            ImGuiMCP::TextWrapped("Open the console (~) and click a man (or use the hotkey on him), then come back here.");
            if (changed) SaveIni();
            return;
        }
        const Status st = StatusOf(a);
        ImGuiMCP::SeparatorText(st.name.c_str());
        ImGuiMCP::Text("Race: %s", st.race.c_str());
        ImGuiMCP::Text("Colour pick: %s%s", st.colour.c_str(), st.own ? " (his own)" : "");
        ColourRows(st);
        Help("Skin colour: his skin tone as the game tints it. Anus colour: the rim as Anatomica colours it (before "
             "reddening, bruising and wetness). Ring colour: the soft skin-blend ring around it (RaceMenu paint).");
        ImGuiMCP::Text("Wet %.0f%%   Red %.0f%%   Bruise %.0f%%   Swelling %.0f%%", st.wet * 100, st.tender * 100, st.bruise * 100, st.swell * 100);
        ImGuiMCP::Text("Gape %.0f%%   Looseness %.0f%%   Schlong shine %.0f%%", st.gape * 100, st.loose * 100, st.schlong * 100);
        ImGuiMCP::Text("This round: %d partner(s), %.0f s of penetration", st.partners, st.secs);
        ImGuiMCP::TextUnformatted(Drip::Status(a).c_str());
        ImGuiMCP::TextUnformatted(Virgin::Status(a).c_str());

        ImGuiMCP::SeparatorText("His own colour");
        const std::string key = RefKey(a);
        bool own = g_perMan.count(key) > 0;
        if (ImGuiMCP::Checkbox("Give him his own colour", &own)) {
            if (own) g_perMan[key] = ManSet{};
            else g_perMan.erase(key);
            changed = true;
        }
        Help("Overrides his race, Random and everything else - just for him.");
        if (auto it = g_perMan.find(key); it != g_perMan.end()) {
            ImGuiMCP::Indent();
            changed |= ComboValue("Colour##man", &it->second.choice, 1, kDefaultItems, 9);
            if (it->second.choice == kCustom) changed |= ImGuiMCP::ColorEdit3("His colour", it->second.rgb, kWheel);
            ImGuiMCP::Unindent();
        }
        if (changed) SaveIni();
        ImGuiMCP::Spacing();
        ImGuiMCP::TextDisabled("Values update every \"Check every\" seconds (Colours page).");
    }

    // ---------------------------------------------------------------- Testing page: make anything happen now
    // a Papyrus global (NiOverride) from the game thread
    template <class... Args>
    void CallStatic(const char* cls, const char* fn, Args... args) {
        if (auto* t = SKSE::GetTaskInterface())
            t->AddTask([=]() {
                auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
                if (!vm) return;
                auto* fa = RE::MakeFunctionArguments(Args(args)...);
                RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
                vm->DispatchStaticCall(cls, fn, fa, cb);
            });
    }
    void SetMorph(RE::Actor* a, const char* morph, const char* key, float v) {
        RE::TESObjectREFR* ref = a;
        CallStatic("NiOverride", "SetBodyMorph", ref, RE::BSFixedString(morph), RE::BSFixedString(key), v);
        CallStatic("NiOverride", "UpdateModelWeight", ref);
    }
    // An after-sex gape the way the gape script itself starts one: it keeps its own dilation state in
    // body-morph slots (key AnatomicaGape) and rewrites "Anatomica Gape" from it every tick, so setting the
    // morph alone is undone on the next tick. AGTime is the script's clock (Utility.GetCurrentRealTime).
    void GapeTest(RE::Actor* a, float dil) {
        RE::TESObjectREFR* ref = a;
        const float now = static_cast<float>(RE::GetDurationOfApplicationRunTime()) / 1000.0f;
        auto set = [&](const char* m, float v) {
            CallStatic("NiOverride", "SetBodyMorph", ref, RE::BSFixedString(m), RE::BSFixedString("AnatomicaGape"), v);
        };
        set("AGDil", dil);
        set("AGTime", now);
        set("AGWas", 0.0f);
        set("AGEp", 1.0f);
        set("Anatomica Gape", dil);     // open at once; the script takes over from the next tick
        CallStatic("NiOverride", "UpdateModelWeight", ref);
    }
    RE::Actor* TestWho() {
        if (auto sel = RE::Console::GetSelectedRef()) if (auto* a = sel->As<RE::Actor>()) return a;
        return RE::PlayerCharacter::GetSingleton();
    }
    // a whole round of sex that just ended: dil = how rough (0-1), partners, seconds
    float LooseCapFor(RE::Actor* a) {   // the most lasting looseness he can have (his experience included)
        const float k = Virgin::For(a->GetFormID()).looseCap;
        std::lock_guard lk(g_lock);
        return std::min(0.6f, g.looseCap * k);
    }
    void FakeRound(RE::Actor* a, float dil, int partners, float secs) {
        Worked(nullptr, a, dil, static_cast<float>(partners), nullptr);
        std::lock_guard lk(g_lock);
        auto& w = g_work[a->GetFormID()];
        w.peak = std::clamp(dil, 0.0f, 1.0f);
        w.glossPeak = std::clamp(dil, 0.0f, 1.0f);
        w.wetPeak = g.wetBuild ? std::min(g.wetCap, w.wetBase + dil) : dil;
        w.partners = partners;
        w.workSecs = secs;
        w.last = w.wetLast = Now() - 3.5;   // the round is over (looseness, drips etc. react to that)
        g_sent.erase(a->GetFormID());       // send the whole look on the next pass
    }

    void __stdcall RenderTesting() {
        RE::Actor* a = TestWho();
        static std::string last;
        auto act = [&](const char* label, auto&& fn) {
            if (ImGuiMCP::Button(label)) last = std::string(label) + ": " + fn();
        };
        // a test can only show what is switched on: say which of the features it needs are off (and where)
        auto off = [](std::initializer_list<std::pair<bool, const char*>> need) {
            std::string o;
            for (auto& [on, what] : need) if (!on) o += (o.empty() ? "" : ", ") + std::string(what);
            return o.empty() ? o : " - NOT shown, switched off: " + o;
        };
        auto gapeGlob = [](RE::FormID id) {   // AnatomicaGape.esp switches (Gape / Movement pages)
            auto* dh = RE::TESDataHandler::GetSingleton();
            auto* gl = dh ? dh->LookupForm<RE::TESGlobal>(id, "AnatomicaGape.esp") : nullptr;
            return !gl || gl->value > 0.5f;
        };
        const bool gapeOn = gapeGlob(0x806), clenchOn = gapeGlob(0x80D);
        const bool picker = g.enabled, inScope = InScope(a);
        ImGuiMCP::TextWrapped("Everything here happens to the man you click in the console (~), or to you if nobody is "
                              "selected. Close the menu to see it. Looks (colour, wetness, redness...) appear on the next "
                              "check (Colours page, \"Check every\").");
        {
            const double ago = g_lastLook > 0.0 ? Now() - g_lastLook : -1.0;
            float iv; { std::lock_guard lk(g_lock); iv = g.interval; }
            if (ago < 0.0) ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1, 0.5f, 0.4f, 1}, "The Anatomica script has not asked for any looks since loading.");
            else if (ago > iv * 2.0 + 15.0) ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1, 0.5f, 0.4f, 1}, "The Anatomica script last checked %.0f s ago - it looks stopped.", ago);
            else ImGuiMCP::TextDisabled("The Anatomica script is running (last check %.0f s ago, %d men checked since loading).", ago, g_looks.load());
            ImGuiMCP::SameLine();
            act("Restart the script", [&] { KickScript(); return std::string("asked the gape script to start again"); });
            ImGuiMCP::TextDisabled("%s", Engine::Status().c_str());
        }
        ImGuiMCP::SeparatorText(a ? a->GetName() : "nobody");
        if (!a) return;

        if (Section("Colour and ring")) {
        act("Re-roll his colour", [&] { std::lock_guard lk(g_lock); ++g_reroll[RefKey(a)]; g_ringReset.insert(a->GetFormID()); SaveIni(); return std::string("new random / Skin-fit colour, ring redone") + off({{picker, "colour picker (Colours page)"}, {inScope, "he is outside \"Who\" (Colours page)"}}); });
        ImGuiMCP::SameLine();
        act("Re-apply his look now", [&] { std::lock_guard lk(g_lock); g_sent.erase(a->GetFormID()); return std::string("sent again on the next check") + off({{picker, "colour picker (Colours page)"}, {inScope, "he is outside \"Who\" (Colours page)"}}); });
        ImGuiMCP::SameLine();
        act("Reset his ring", [&] { std::lock_guard lk(g_lock); g_ringReset.insert(a->GetFormID()); return std::string("ring taken off and redone") + off({{picker, "colour picker (Colours page)"}, {a->IsPlayerRef() ? g.playerRing : g.autoRing, a->IsPlayerRef() ? "\"Give you the skin-blend ring too\" (Colours page; your own painted ring is still used)" : "\"Give NPCs the skin-blend ring\" (Colours page; a painted ring is still used)"}}); });

        }
        if (Section("After sex")) {
        act("Gentle round", [&] { FakeRound(a, 0.35f, 1, 45.0f); return std::string("1 partner, 45 s, gentle") + off({{picker, "colour picker (Colours page)"}, {g.wet, "wetness"}, {g.tender, "tender reddening"}, {g.glossy, "glossy skin"}, {g.loose, "lasting looseness"}}); });
        ImGuiMCP::SameLine();
        act("Rough round", [&] { FakeRound(a, 1.0f, std::max(3, g.bruisePartners), std::max(240.0f, g.bruiseLong + 10.0f)); return std::string("rough, several partners: wet, red, bruised, swollen") + off({{picker, "colour picker (Colours page)"}, {g.wet, "wetness"}, {g.tender, "tender reddening"}, {g.bruise, "bruising"}, {g.swell, "swelling"}, {g.glossy, "glossy skin"}, {g.loose, "lasting looseness"}}); });
        ImGuiMCP::SameLine();
        act("Wet only", [&] { FakeRound(a, 0.6f, 1, 20.0f); std::lock_guard lk(g_lock); g_work[a->GetFormID()].peak = 0.0f; return std::string("wet, not red") + off({{g.wet, "wetness (After sex page)"}}); });
        act("First time in a while", [&] { std::lock_guard lk(g_lock); g_lastRound.erase(a->GetFormID()); return std::string("his next round counts as rested (redder)") + off({{g.tender, "tender reddening"}, {g.rested, "\"first time in a while\" (After sex page)"}}); });
        ImGuiMCP::SameLine();
        if (kCum) act("Schlong coated (as giver)", [&] { const float c[3] = {0.96f, 0.95f, 0.90f}; Fluid::Coat(a->GetFormID(), 1.0f, c, 90.0f); return std::string("his schlong is wet with cum (needs a schlong on him)"); });
        else act("Schlong wet (as giver)", [&] { std::lock_guard lk(g_lock); auto& sw = g_schlong[a->GetFormID()]; sw.peak = 1.0f; sw.last = Now(); return std::string(g.schlongWet ? "his schlong is wet (needs a schlong on him)"
                                                  : "NOT shown: \"The giver's schlong comes out wet\" is off (After sex page > Wet schlong)"); });
        ImGuiMCP::SameLine();
        act("Clear after-sex looks", [&] { std::lock_guard lk(g_lock); g_work.erase(a->GetFormID()); g_schlong.erase(a->GetFormID()); g_sent.erase(a->GetFormID()); return std::string("back to normal on the next check"); });

        }
        if (Section("Gape and movement")) {
        act("Gape him open", [&] { if (!Engine::TestGape(a, 0.8f)) GapeTest(a, 0.8f); return std::string("opened; it holds, then closes on its own (Gape page timings)") + off({{gapeOn, "the after-sex gape (Gape page)"}}); });
        ImGuiMCP::SameLine();
        act("Clench now", [&] { return std::string(Wink::TestClench(a) ? "clenched" : "only works while he is gaping (the movement code follows him then)") + off({{clenchOn, "clenching (Movement page)"}}); });
        act("Loosen to the cap", [&] { const float v = LooseCapFor(a); if (!Engine::SetLooseNow(a, v)) SetMorph(a, "Anatomica Gape", "AnatomicaLoose", v); { std::lock_guard lk(g_lock); g_looseSeen[a->GetFormID()] = v; } return std::format("lasting looseness {:.0f}%", v * 100.0f) + off({{g.loose, "lasting looseness (After sex page) - it will not stay"}}); });
        ImGuiMCP::SameLine();
        act("Remove looseness", [&] {
            if (!Engine::SetLooseNow(a, 0.0f)) SetMorph(a, "Anatomica Gape", "AnatomicaLoose", 0.0f);
            std::lock_guard lk(g_lock);
            g_looseSeen[a->GetFormID()] = 0.0f;
            g_looseRest.erase(a->GetFormID());
            if (auto it = g_work.find(a->GetFormID()); it != g_work.end()) it->second.looseDone = true;   // the last round must not add it back
            return std::string("tight again"); });

        if (kCum) {
            ImGuiMCP::SeparatorText("Cum");
            Drip::RenderTests(a);
        }

        }
        if (Section("Virginity")) {
        Virgin::RenderTests(a);

        }
        if (Section("Status")) {
        {
            const bool onHim = g_winOpen && g_inspect == a->GetFormID();   // pressed again: close it
            act(onHim ? "Close the status window" : "Open the status window on him", [&] {
                if (onHim) { g_winOpen = false; ApplyWindowState(); return std::string("closed"); }
                g_inspect = a->GetFormID(); g_winOpen = true; g_winMove = false; ApplyWindowState(); return std::string("open");
            });
        }

        }
        if (!last.empty()) { ImGuiMCP::Spacing(); ImGuiMCP::TextDisabled("Last: %s", last.c_str()); }
    }

    void RegisterWindow() {
        g_statusWin = SKSEMenuFramework::AddWindow(RenderStatusWindow, false);
        if (g_statusWin) { g_statusWin->IsOpen = false; g_statusWin->BlockUserInput = false; }
        SKSEMenuFramework::AddInputEvent(FrameworkInput);
    }

    void __stdcall RenderAfter() {
        std::lock_guard lk(g_lock);
        if (!g_loaded) LoadIni();
        bool changed = false;
        auto Indented = [&](bool on, auto&& body) {
            if (!on) return;
            ImGuiMCP::Indent();
            body();
            ImGuiMCP::Unindent();
        };
        ImGuiMCP::TextWrapped("Looks that follow anal sex and fade afterwards. Longer and rougher sex = stronger effect "
                              "(up to the strength set here). Every part can be switched off on its own.");

        // ---------------------------------------------------------------- wetness
        if (Section("Wetness")) {
        changed |= ImGuiMCP::Checkbox("Wet shine after sex", &g.wet);
        Indented(g.wet, [&] {
            changed |= Pct("Shine strength", &g.wetStrength);
            Help("How wet he gets: a glassy, water-like shine on the rim and a clear wet layer over it. (The glossy skin "
                 "around the hole and up the crevice is its own effect, further down.)");
            changed |= Minutes("Dries in", &g.wetTime, 0.5f, 30.0f);
            changed |= ImGuiMCP::Checkbox("Builds up over rounds", &g.wetBuild);
            Help("Another round before he has dried adds to the wetness instead of starting again - he stays wet longer.");
            if (g.wetBuild) {
                ImGuiMCP::SameLine();
                float pct = g.wetCap * 100.0f;
                ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 8.0f);
                if (ImGuiMCP::SliderFloat("up to##cap", &pct, 100.0f, 300.0f, "%.0f%%")) {
                    g.wetCap = pct / 100.0f;
                    changed = true;
                }
            }
            if (kLube) {   // coloured lube: switched off (it did not show well on the rim)
            changed |= ImGuiMCP::Checkbox("Anus wear colour (outer rim)", &g.wetColourOn);
            Help("Colours the clear wet layer over the outside of the rim (and tints the rim a little) while he is wet - "
                 "lube, cum, anything you like - fading as he dries.");
            Indented(g.wetColourOn, [&] {
                changed |= ImGuiMCP::ColorEdit3("Wetness colour", g.wetColour, kWheel);
                changed |= Pct("Colour amount", &g.wetColourAmt);
                auto preset = [&](const char* label, float r, float gg, float b, float amt) {
                    if (ImGuiMCP::Button(label)) {
                        g.wetColour[0] = r, g.wetColour[1] = gg, g.wetColour[2] = b, g.wetColourAmt = amt;
                        changed = true;
                    }
                };
                preset("Clear lube", 0.9f, 0.9f, 0.92f, 0.15f);
                ImGuiMCP::SameLine();
                preset("Milky (cum)", 0.95f, 0.93f, 0.88f, 0.45f);
                ImGuiMCP::SameLine();
                preset("Pink lube", 0.95f, 0.55f, 0.7f, 0.35f);
                ImGuiMCP::SameLine();
                preset("Blue lube", 0.45f, 0.65f, 0.95f, 0.35f);
            });
            }
            changed |= ImGuiMCP::Checkbox("Extra gloss right after", &g.wetBurst);
            Help("For the first moments after the last stroke he is glossier than the slider says, then settles.");
            Indented(g.wetBurst, [&] {
                changed |= ImGuiMCP::SliderFloat("for##burst", &g.wetBurstSecs, 5.0f, 120.0f, "%.0f s");
                changed |= Pct("extra##burst", &g.wetBurstBoost);
            });
            changed |= ImGuiMCP::Checkbox("Anus wear colour (inner rim)", &g.wetCumOn);
            Help("Separate from the outer rim colour above: colours the inner part of the rim - where it is worked - in its own "
                 "colour and opacity (cum, soreness, anything you like), fading as he dries.");
            Indented(g.wetCumOn, [&] {
                changed |= ImGuiMCP::ColorEdit3("Wear colour##wc", g.wetCum, kWheel);
                changed |= Pct("Wear opacity##wc", &g.wetCumAmt);
                if (kCum) {   // per-race cum colours: only with the cum system (Features.h)
                    changed |= ImGuiMCP::Checkbox("The giver's race colour when it has one##wc", &g.cumByRace);
                    Help("Per race page: races with a cum colour of their own use it here (also for the lube colour above).");
                }
            });
            Fluid::RenderWetSettings();
            changed |= ImGuiMCP::Checkbox("Water dries it faster", &g.waterDry);
            Help("Swimming, or standing outside in the rain, dries him faster.");
            if (g.waterDry) {
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 8.0f);
                changed |= ImGuiMCP::SliderFloat("x faster##water", &g.waterDryMul, 1.0f, 10.0f, "x%.1f");
            }
        });

        }
        if (Section("Wet schlong")) {
        if (kCum) {
            ImGuiMCP::TextWrapped("The giver's schlong coming out wet with cum (gloss, streaks, a drip from the tip) is on the "
                                  "Cum & drips page now: \"His schlong comes out wet with cum\".");
        } else {
            changed |= ImGuiMCP::Checkbox("The giver's schlong comes out wet", &g.schlongWet);
            Help("After a round the partner's glans glistens wet (the skin itself, nothing on top), drying off over the "
                 "time set here.");
            Indented(g.schlongWet, [&] {
                changed |= Pct("Shine strength##sw", &g.schlongStrength);
                changed |= Minutes("Dries in##sw", &g.schlongTime, 0.5f, 60.0f);
                if (kCum) {   // cum streaks: off for now with the rest of Anatomica's cum (Features.h); a fluid mod may do it later
                changed |= ImGuiMCP::Checkbox("Cum on it (streaks)", &g.schlongCum);
                Help("Cum streaks running down his schlong in the colour and opacity set here, fading with the shine.");
                Indented(g.schlongCum, [&] {
                    if (kCum) changed |= ImGuiMCP::Checkbox("A drip from the tip now and then", &g.schlongDrip);
                    changed |= ImGuiMCP::ColorEdit3("Cum colour##sc", g.schlongCumCol, kWheel);
                    changed |= Pct("Cum opacity##sc", &g.schlongCumAmt);
                    changed |= ImGuiMCP::Checkbox("His race's cum colour when it has one##sc", &g.schlongCumRace);
                });
                }
            });
        }

        // ---------------------------------------------------------------- reddening
        }
        if (Section("Tender reddening")) {
        changed |= ImGuiMCP::Checkbox("Tender reddening after sex", &g.tender);
        Indented(g.tender, [&] {
            changed |= Pct("Reddening strength", &g.tenderStrength);
            changed |= Minutes("Fades in", &g.tenderTime, 0.5f, 60.0f);
            changed |= ImGuiMCP::Checkbox("Also redden the skin-blend ring", &g.tenderBlend);
            changed |= ImGuiMCP::Checkbox("Colour changes as it fades", &g.tenderStages);
            Help("Starts as the first colour, turns into the second, then fades back to normal.");
            Indented(g.tenderStages, [&] {
                changed |= ImGuiMCP::ColorEdit3("Fresh", g.tenderCol1, kWheel);
                changed |= ImGuiMCP::ColorEdit3("Later", g.tenderCol2, kWheel);
                changed |= Pct("Turns to \"Later\" after (part of the fade time)", &g.tenderSplit);
            });
            changed |= ImGuiMCP::Checkbox("More partners = redder", &g.tenderPartners);
            if (g.tenderPartners) {
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 8.0f);
                changed |= Pct("per extra partner##pp", &g.tenderPerPartner);
            }
            changed |= ImGuiMCP::Checkbox("Bruising after very long sex or many partners", &g.bruise);
            Indented(g.bruise, [&] {
                changed |= ImGuiMCP::SliderInt("Partners in a row", &g.bruisePartners, 2, 6);
                changed |= Minutes("or penetration lasting", &g.bruiseLong, 0.5f, 30.0f);
                changed |= ImGuiMCP::ColorEdit3("Bruise colour", g.bruiseCol, kWheel);
                changed |= Pct("Bruise strength", &g.bruiseStrength);
                Help("A faint tint toward the outer edge of the rim. Lasts a little longer than the redness.");
            });
            changed |= ImGuiMCP::Checkbox("Glossy skin round the hole and up the crevice", &g.glossy);
            Help("After sex the skin around his hole and up the crevice has a clear, glassy wet layer, fading over its own "
                 "time - apart from the wetness of the hole itself (above).");
            Indented(g.glossy, [&] {
                changed |= Pct("Gloss strength", &g.glossyStrength);
                changed |= Minutes("Fades in##glossy", &g.glossyTime, 0.5f, 120.0f);
                Fluid::RenderGlossSettings();
            });
            changed |= ImGuiMCP::Checkbox("Swelling (puffy rim)", &g.swell);
            Help("Needs the body built in BodySlide with \"Build Morphs\" ticked (Anatomica Swell).");
            if (g.swell) {
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 8.0f);
                changed |= Pct("##swell", &g.swellStrength);
            }
            changed |= ImGuiMCP::Checkbox("First time in a while = redder", &g.rested);
            Indented(g.rested, [&] {
                changed |= Minutes("\"A while\" is", &g.restedMinutes, 5.0f, 600.0f);
                changed |= Pct("extra redness##rested", &g.restedExtra);
            });
            changed |= ImGuiMCP::Checkbox("Gape closes slower while tender", &g.slowClose);
            if (g.slowClose) {
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 8.0f);
                float pct = g.slowCloseAmt * 100.0f;
                if (ImGuiMCP::SliderFloat("longer##slow", &pct, 0.0f, 200.0f, "+%.0f%%")) {
                    g.slowCloseAmt = pct / 100.0f;
                    changed = true;
                }
            }
            changed |= ImGuiMCP::Checkbox("Sensitive while tender", &g.sensitive);
            Help("While he is still red, the clenches are sharper and the breathing slower (Movement page).");
            Indented(g.sensitive, [&] {
                changed |= Pct("Sharper clenches", &g.sensClench);
                changed |= Pct("Slower breathing", &g.sensBreath);
            });
        });

        // ---------------------------------------------------------------- looseness
        }
        if (Section("Lasting looseness")) {
        changed |= ImGuiMCP::Checkbox("Each round leaves him a little looser", &g.loose);
        Help("A small gape that stays after the after-sex gape has closed and heals over game days. Saved with each "
             "man (RaceMenu body morph). Turning this off lets every man heal back over the next minutes.");
        Indented(g.loose, [&] {
            changed |= Pct("Per round", &g.looseAdd);
            changed |= Pct("At most", &g.looseCap);
            changed |= Pct("Heals per game day", &g.looseRecover);
        });

        if (kCum) {
            ImGuiMCP::SeparatorText("Drips");
            ImGuiMCP::TextDisabled("Strands, squirts, puddles and the leg trail have their own page: Cum & drips.");
        }

        // ---------------------------------------------------------------- healing
        }
        if (Section("Healing")) {
        changed |= ImGuiMCP::Checkbox("Sleeping or waiting clears everything", &g.healSleep);
        changed |= ImGuiMCP::Checkbox("Health potions heal redness, bruising and swelling (a bit at a time)", &g.healMagic);
        Help("A health potion heals by how much health it gives back. Several in a row add up. Only health potions count "
             "unless you switch the others on below - NPCs get all kinds of effects from other mods.");
        Indented(g.healMagic, [&] {
            changed |= ImGuiMCP::SliderFloat("Per point of health", &g.healPotion, 0.0f, 5.0f, "%.2f%%");
            Help("A potion that restores 50 health heals 25% at 0.5%; an ultimate one (100+) about half or more.");
            changed |= ImGuiMCP::Checkbox("Food and drink count too", &g.healFoodOn);
            if (g.healFoodOn) {
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 8.0f);
                changed |= ImGuiMCP::SliderFloat("per point##food", &g.healFood, 0.0f, 2.0f, "%.2f%%");
            }
            changed |= ImGuiMCP::Checkbox("Healing spells count too", &g.healSpellOn);
            if (g.healSpellOn) {
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 8.0f);
                changed |= ImGuiMCP::SliderFloat("per cast##spell", &g.healSpell, 0.0f, 100.0f, "%.0f%%");
            }
        });

        }
        ImGuiMCP::Spacing();
        if (ConfirmButton("Reset after-sex settings")) {
            const Settings d;
            g.wet = d.wet, g.wetStrength = d.wetStrength, g.wetTime = d.wetTime, g.wetBuild = d.wetBuild, g.wetCap = d.wetCap;
            g.wetColourOn = d.wetColourOn, g.wetColourAmt = d.wetColourAmt;
            std::copy(std::begin(d.wetColour), std::end(d.wetColour), g.wetColour);
            g.waterDry = d.waterDry, g.waterDryMul = d.waterDryMul;
            g.schlongWet = d.schlongWet, g.schlongStrength = d.schlongStrength, g.schlongTime = d.schlongTime;
            g.wetCumOn = d.wetCumOn, g.wetCumAmt = d.wetCumAmt, g.schlongCum = d.schlongCum, g.schlongCumRace = d.schlongCumRace, g.schlongCumAmt = d.schlongCumAmt;
            std::copy(std::begin(d.wetCum), std::end(d.wetCum), g.wetCum);
            std::copy(std::begin(d.schlongCumCol), std::end(d.schlongCumCol), g.schlongCumCol);
            g.tender = d.tender, g.tenderStrength = d.tenderStrength, g.tenderTime = d.tenderTime, g.tenderBlend = d.tenderBlend;
            g.tenderStages = d.tenderStages, g.tenderSplit = d.tenderSplit;
            std::copy(std::begin(d.tenderCol1), std::end(d.tenderCol1), g.tenderCol1);
            std::copy(std::begin(d.tenderCol2), std::end(d.tenderCol2), g.tenderCol2);
            g.tenderPartners = d.tenderPartners, g.tenderPerPartner = d.tenderPerPartner;
            g.bruise = d.bruise, g.bruisePartners = d.bruisePartners, g.bruiseLong = d.bruiseLong, g.bruiseStrength = d.bruiseStrength;
            g.glossy = d.glossy, g.glossyStrength = d.glossyStrength, g.glossyTime = d.glossyTime;
            std::copy(std::begin(d.bruiseCol), std::end(d.bruiseCol), g.bruiseCol);
            g.swell = d.swell, g.swellStrength = d.swellStrength;
            g.sensitive = d.sensitive, g.sensClench = d.sensClench, g.sensBreath = d.sensBreath;
            g.healSleep = d.healSleep, g.healMagic = d.healMagic;
            g.healPotion = d.healPotion, g.healFood = d.healFood, g.healSpell = d.healSpell;
            g.healFoodOn = d.healFoodOn, g.healSpellOn = d.healSpellOn;
            g.wetBurst = d.wetBurst, g.wetBurstSecs = d.wetBurstSecs, g.wetBurstBoost = d.wetBurstBoost, g.cumByRace = d.cumByRace;
            g.slowClose = d.slowClose, g.slowCloseAmt = d.slowCloseAmt;
            g.rested = d.rested, g.restedMinutes = d.restedMinutes, g.restedExtra = d.restedExtra;
            g.loose = d.loose, g.looseAdd = d.looseAdd, g.looseCap = d.looseCap, g.looseRecover = d.looseRecover;
            changed = true;
        }
        if (changed) SaveIni();
        ImGuiMCP::Spacing();
        ImGuiMCP::TextDisabled("Updates every \"Check every\" seconds (Colours page). Nothing extra is saved: it fades on its own.");
    }
}

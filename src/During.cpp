#include "During.h"
#include "Align.h"
#include "Lang.h"
#include "Colour.h"
#include "SKSEMenuFramework.h"
#include "Mnc.h"
#include "Engine.h"
#include "Size.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
    // PPA's anus settings in Anatomica's override file (what the installer ships)
    struct Tight {
        float scale = 0.4f;       // Scale: how far he opens around the schlong
        float scaleMax = 1.1f;    // ScaleMax: the widest PPA opens him
        float grip = 1.0f;        // GripStrength: how hard the rim grips the shaft
        float fwd = 1.0f;         // GripClampForward: how far the rim is drawn in on the push
        float back = 0.3f;        // GripClampBackward: how far it is pulled out on the withdraw
        float rub = 1.5f;         // RubIntensity
    };
    Tight g_t;
    bool g_read = false, g_found = false;
    std::string g_note;
    std::atomic<bool> g_donutOn{false};
    std::atomic<float> g_donutAmt{0.5f};
    std::atomic<int> g_donutStyle{0};   // 2.5m/n: During::kRingSliders (Ring, Donut, Rosebud, Lips, Pucker)
    float g_styleAmt[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};   // 2.5n: each ring shape's own strength (x How puffy)
    std::mutex g_wmx;
    bool g_canalOn = true, g_bulgeOn = false;
    During::Want MakeW() {     // 2.5: the movement defaults per shape
        During::Want w;
        using namespace During;
        w.travel[kTentacle] = w.travel[kBeaded] = w.travel[kSegmented] = true;
        w.twitch[kSegmented] = true;
        w.build[kKnot] = w.build[kDoubleKnot] = true;
        return w;
    }
    During::Want g_w = MakeW();   // the amounts (used when the tick box is on)
    // 2.5: the bulge shape for each schlong type (TNG / the shape table) and for his size; 0 = by size
    std::map<std::string, int> g_typeShape;   // type key -> shape (only what was changed; else the default)
    int g_sizeShape[3] = {During::kTip, During::kShaft, During::kColossal};   // smaller / not known, L, XL
    std::map<std::string, During::Knots> g_typeKnots;   // 2.5d/e: type key -> its knots (which, how strong)
    During::Knots g_sizeKnots[3];
    int TypeDefault(const std::string& key) {
        using namespace During;
        if (key == "knotted") return kKnot;
        if (key == "flared") return kHorse;
        if (key == "ridged" || key == "barbed") return kRidged;
        if (key == "tentacle") return kBeaded;
        if (key == "corkscrew") return kHooked;
        if (key == "colossal") return kColossal;
        if (key == "segmented") return kSegmented;
        if (key == "pizzle") return kBeaded;
        if (key == "elemental") return kRidged;
        if (key == "tapered" || key == "cold" || key == "piston" || key == "twin") return kShaft;
        return kAuto;
    }
    int TypeShape(const std::string& key) {
        if (auto it = g_typeShape.find(key); it != g_typeShape.end()) return it->second;
        return TypeDefault(key);
    }
    int g_bulgeWho = 0;        // 0 everyone, 1 you and your followers, 2 only you

    constexpr const char* kToml = "Data/SKSE/Plugins/ppa-override-configs/anatomica.toml";
    struct Key { const char* name; float Tight::*v; };
    constexpr Key kKeys[] = {{"Scale", &Tight::scale}, {"ScaleMax", &Tight::scaleMax}, {"GripStrength", &Tight::grip},
                             {"GripClampForward", &Tight::fwd}, {"GripClampBackward", &Tight::back}, {"RubIntensity", &Tight::rub}};

    std::string Trim(const std::string& s) {
        const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    constexpr const char* kTomlBottom = "Data/SKSE/Plugins/ppa-override-configs/anatomica_bottom.toml";   // 2.5v: its anus copy follows
    std::vector<std::string> Lines(const char* path = kToml) {
        std::vector<std::string> out;
        std::ifstream f(path);
        for (std::string l; std::getline(f, l);) out.push_back(l);
        return out;
    }
    // the [Override.Anus] section: first line after its header .. the next header
    bool Section(const std::vector<std::string>& L, size_t& from, size_t& to) {
        for (size_t i = 0; i < L.size(); ++i)
            if (Trim(L[i]) == "[Override.Anus]") {
                from = i + 1;
                to = L.size();
                for (size_t k = from; k < L.size(); ++k)
                    if (!Trim(L[k]).empty() && Trim(L[k])[0] == '[') { to = k; break; }
                return true;
            }
        return false;
    }
    // "Key = value  # comment" -> key, value
    bool KeyOf(const std::string& line, std::string& key, float* val) {
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#') return false;
        const auto eq = t.find('=');
        if (eq == std::string::npos) return false;
        key = Trim(t.substr(0, eq));
        if (val) *val = static_cast<float>(std::atof(t.substr(eq + 1).c_str()));
        return true;
    }
    void ReadToml() {
        g_read = true;
        const auto L = Lines();
        size_t a = 0, b = 0;
        g_found = Section(L, a, b);
        if (!g_found) return;
        for (size_t i = a; i < b; ++i) {
            std::string k;
            float v = 0.0f;
            if (!KeyOf(L[i], k, &v)) continue;
            for (const auto& kk : kKeys) if (k == kk.name) g_t.*kk.v = v;
        }
    }
    bool WriteTomlTo(const char* path);
    bool WriteToml() {
        if (!WriteTomlTo(kToml)) return false;
        std::error_code ec;
        if (std::filesystem::exists(kTomlBottom, ec)) WriteTomlTo(kTomlBottom);   // the bottom's copy of the same settings
        return true;
    }
    bool WriteTomlTo(const char* path) {
        const std::string kTomlP = path;
        auto L = Lines(path);
        size_t a = 0, b = 0;
        if (!Section(L, a, b)) return false;
        for (const auto& kk : kKeys) {
            const std::string line = std::format("{} = {:.2f}", kk.name, g_t.*kk.v);
            bool done = false;
            for (size_t i = a; i < b && !done; ++i) {
                std::string k;
                if (KeyOf(L[i], k, nullptr) && k == kk.name) { L[i] = line; done = true; }
            }
            if (!done) { L.insert(L.begin() + static_cast<std::ptrdiff_t>(a), line); ++b; }
        }
        const std::string tmp = kTomlP + ".tmp";
        {
            std::ofstream o(tmp, std::ios::trunc);
            if (!o) return false;
            for (const auto& l : L) o << l << "\n";
            if (!o) return false;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, kTomlP, ec);
        if (ec) {   // some setups cannot replace the file: write it in place
            std::filesystem::remove(tmp, ec);
            std::ofstream o(kTomlP, std::ios::trunc);
            if (!o) return false;
            for (const auto& l : L) o << l << "\n";
            return static_cast<bool>(o);
        }
        return true;
    }

    void Help(const char* t) {
        ImGuiMCP::SameLine();
        Tr::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::BeginTooltip();
            ImGuiMCP::PushTextWrapPos(ImGuiMCP::GetFontSize() * 30.0f);
            Tr::TextUnformatted(t);
            ImGuiMCP::PopTextWrapPos();
            ImGuiMCP::EndTooltip();
        }
    }
}

namespace During {
    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "DuringSex";
        bool on = g_donutOn;
        float amt = g_donutAmt;
        if (save) {
            ini.SetBoolValue(S, "Donut", on);
            ini.SetDoubleValue(S, "DonutAmount", amt);
            ini.SetLongValue(S, "DonutStyle", g_donutStyle);
            for (int i = 0; i < 5; ++i) ini.SetDoubleValue(S, std::format("DonutStyleStrength{}", i).c_str(), g_styleAmt[i]);
        } else {
            g_donutOn = ini.GetBoolValue(S, "Donut", on);
            g_donutAmt = std::clamp(static_cast<float>(ini.GetDoubleValue(S, "DonutAmount", amt)), 0.0f, 2.0f);
            g_donutStyle = std::clamp(static_cast<int>(ini.GetLongValue(S, "DonutStyle", 0)), 0, 4);
            for (int i = 0; i < 5; ++i)
                g_styleAmt[i] = std::clamp(static_cast<float>(ini.GetDoubleValue(S, std::format("DonutStyleStrength{}", i).c_str(), g_styleAmt[i])), 0.0f, 2.0f);
        }
        std::lock_guard lk(g_wmx);
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto F = [&](const char* k, float& v, float lo, float hi) {
            if (save) ini.SetDoubleValue(S, k, v);
            else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi);
        };
        B("OpenInside", g_canalOn); F("OpenInsideAmount", g_w.canal, 0.0f, 1.5f);
        B("RingFollows", g_w.ring); F("RingGrip", g_w.grip, 0.0f, 1.5f); F("RingMaxOut", g_w.maxOut, 0.0f, 3.0f);
        F("RingMaxIn", g_w.maxIn, 0.0f, 3.0f); F("RingGrowExtra", g_w.grow, 0.0f, 2.0f);
        B("Bulge", g_bulgeOn); F("BulgeAmount", g_w.bulge, 0.0f, 3.0f); F("BulgeStart", g_w.bulgeStart, 2.0f, 30.0f);
        F("BulgeRandom", g_w.bulgeRandom, 0.0f, 1.0f);
        g_w.bulgeShape = 0;   // 2.5p: always from his schlong (the one-shape-for-everyone choice is gone)
        F("BulgeShaftStrength", g_w.shaftK, 0.0f, 2.0f); F("BulgeHeadStrength", g_w.headK, 0.0f, 2.0f);
        F("BulgeBuildTime", g_w.buildTime, 1.0f, 60.0f); F("BulgeTravelSpeed", g_w.travelSpeed, 0.1f, 2.0f); B("BulgeHoldFull", g_w.holdFull);
        B("BeastOpen", g_w.beastOpen); F("BeastOpenFit", g_w.beastFit, 0.5f, 1.5f);   // 2.6q
        B("BulgeHipFade", g_w.hipFade); F("BulgeHipFrom", g_w.hipFrom, 30.0f, 150.0f);
        F("BulgeStomach", g_w.belly, 0.0f, 2.0f); F("BulgeStomachTime", g_w.bellyTime, 1.0f, 180.0f); F("BulgeStomachRandom", g_w.bellyRandom, 0.0f, 1.0f);
        B("BulgeSuperDeep", g_w.deep); F("BulgeSuperDeepChance", g_w.deepChance, 0.0f, 1.0f);
        B("BulgeImprint", g_w.imprint); F("BulgeImprintAmount", g_w.imprintAmt, 0.0f, 0.5f); F("BulgeImprintMinutes", g_w.imprintMin, 0.5f, 15.0f);
        for (int i = 1; i < kShapeCount; ++i) {   // movement per shape: 1 travelling, 2 twitching, 4 build-up
            const std::string k = std::format("BulgeMove{}", i);
            if (save) ini.SetLongValue(S, k.c_str(), (g_w.travel[i] ? 1 : 0) | (g_w.twitch[i] ? 2 : 0) | (g_w.build[i] ? 4 : 0));
            else {
                const long f = ini.GetLongValue(S, k.c_str(), -1);
                if (f >= 0) { g_w.travel[i] = f & 1; g_w.twitch[i] = f & 2; g_w.build[i] = f & 4; }
            }
        }
        {   // the shape for each schlong type / size
            constexpr const char* T = "BulgeByType";
            static const char* sz[3] = {"(size) smaller", "(size) L", "(size) XL"};
            constexpr const char* TK = "BulgeKnotsByType";   // 2.5d/e: "mask|k1|k2" (mask 1 knot 1, 2 knot 2, 3 both)
            if (save) {
                for (auto& [k, v] : g_typeShape) ini.SetLongValue(T, k.c_str(), v);
                for (int i = 0; i < 3; ++i) ini.SetLongValue(T, sz[i], g_sizeShape[i]);
                for (auto& [k, v] : g_typeKnots) ini.SetValue(TK, k.c_str(), v.Save().c_str());
                for (int i = 0; i < 3; ++i) ini.SetValue(TK, sz[i], g_sizeKnots[i].Save().c_str());
            } else {
                CSimpleIniA::TNamesDepend kk;
                ini.GetAllKeys(TK, kk);
                for (auto& k : kk) {
                    const Knots v = Knots::Load(ini.GetValue(TK, k.pItem, "0"));
                    bool isSize = false;
                    for (int i = 0; i < 3; ++i) if (std::string(k.pItem) == sz[i]) { g_sizeKnots[i] = v; isSize = true; }
                    if (!isSize) g_typeKnots[k.pItem] = v;
                }
                CSimpleIniA::TNamesDepend keys;
                ini.GetAllKeys(T, keys);
                for (auto& k : keys) {
                    const int v = std::clamp(static_cast<int>(ini.GetLongValue(T, k.pItem, 0)), 0, kShapeCount - 1);
                    bool isSize = false;
                    for (int i = 0; i < 3; ++i) if (std::string(k.pItem) == sz[i]) { g_sizeShape[i] = std::max(1, v); isSize = true; }
                    if (!isSize) g_typeShape[k.pItem] = v;
                }
            }
        }
        B("BulgeBigOnly", g_w.bulgeBig);
        F("BulgeOutline", g_w.outline, 0.0f, 1.0f); F("BulgePulse", g_w.pulse, 0.0f, 1.0f);
        F("BulgeLinger", g_w.linger, 0.5f, 30.0f);
        if (save) ini.SetLongValue(S, "BulgeThrough", g_w.bulgeThrough);
        else g_w.bulgeThrough = std::clamp(static_cast<int>(ini.GetLongValue(S, "BulgeThrough", g_w.bulgeThrough)), 0, 2);
        if (save) ini.SetLongValue(S, "BulgeWho", g_bulgeWho);
        else g_bulgeWho = std::clamp(static_cast<int>(ini.GetLongValue(S, "BulgeWho", g_bulgeWho)), 0, 2);
        Mnc::Ini(ini, save);
        Align::Ini(ini, save);   // 2.5v [BottomAlign]
    }
    const char* ShapeName(int shape) {
        static const char* n[kShapeCount] = {"From his schlong", "A bump at the tip", "The whole shaft", "Horse (flared head)",
                                             "Knot", "Tentacle (wandering)", "Ridged", "Segmented (chain of bulbs)", "Colossal",
                                             "Hooked", "Double knot", "Long tentacle"};
        return (shape >= 0 && shape < kShapeCount) ? n[shape] : "";
    }
    // 2.5k: the bulge choice for one schlong (Integrations page): shape + knots; zero names what 0 means
    bool BulgeChoice(int& shape, Knots& k, const std::string& id, const char* zero) {
        const char* opts[kShapeCount];
        opts[0] = zero;
        for (int i = 1; i < kShapeCount; ++i) opts[i] = ShapeName(i);
        bool c = false;
        ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 13.0f);
        c |= Tr::Combo(std::format("Bulge shape##bs_{}", id).c_str(), &shape, opts, kShapeCount);
        bool k1 = k.mask & 1, k2 = k.mask & 2, kc = false;
        ImGuiMCP::SameLine(); kc |= Tr::Checkbox(std::format("Knot 1##k1_{}", id).c_str(), &k1);
        ImGuiMCP::SameLine(); kc |= Tr::Checkbox(std::format("Knot 2##k2_{}", id).c_str(), &k2);
        if (kc) { k.mask = (k1 ? 1 : 0) | (k2 ? 2 : 0); c = true; }
        const bool show1 = k1 || shape == kKnot || shape == kDoubleKnot, show2 = k2 || shape == kDoubleKnot;
        if (show1) {
            float v = k.k1 * 100.0f;
            ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 10.0f);
            if (Tr::SliderFloat(std::format("Knot 1 strength##s1_{}", id).c_str(), &v, 0.0f, 200.0f, "%.0f%%")) { k.k1 = v / 100.0f; c = true; }
        }
        if (show2) {
            float v = k.k2 * 100.0f;
            if (show1) ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 10.0f);
            if (Tr::SliderFloat(std::format("Knot 2 strength##s2_{}", id).c_str(), &v, 0.0f, 200.0f, "%.0f%%")) { k.k2 = v / 100.0f; c = true; }
        }
        {   // 2.5s: this schlong's own shaft / head strength (on top of the During sex page's)
            float sv = k.shaftK * 100.0f, hv = k.headK * 100.0f;
            ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 10.0f);
            if (Tr::SliderFloat(std::format("Shaft##sk_{}", id).c_str(), &sv, 0.0f, 200.0f, "%.0f%%")) { k.shaftK = sv / 100.0f; c = true; }
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 10.0f);
            if (Tr::SliderFloat(std::format("Head##hk_{}", id).c_str(), &hv, 0.0f, 200.0f, "%.0f%%")) { k.headK = hv / 100.0f; c = true; }
            Help("This schlong's own bulge strength: the shaft (the sections up his belly) and the head at the front. 100% = no "
                 "change; they multiply the Shaft / Head strength on the During sex page, which set it for everyone.");
        }
        return c;
    }
    bool RenderTypeBulge(const std::string& key) {
        std::lock_guard lk(g_wmx);
        int v = TypeShape(key);
        Knots kn = g_typeKnots.count(key) ? g_typeKnots[key] : Knots{};
        if (!BulgeChoice(v, kn, "ty_" + key, "By his size")) return false;
        g_typeShape[key] = v;
        g_typeKnots[key] = kn;
        return true;
    }
    bool RenderSizeBulge() {
        std::lock_guard lk(g_wmx);
        bool ch = false;
        static const char* szn[3] = {"XS / S / M, or size not known", "L", "XL"};
        for (int i = 0; i < 3; ++i) {
            Tr::TextUnformatted(szn[i]);
            int v = g_sizeShape[i];
            if (BulgeChoice(v, g_sizeKnots[i], std::format("sz{}", i), "A bump at the tip")) { g_sizeShape[i] = std::max(1, v); ch = true; }
        }
        return ch;
    }
    std::string Knots::Save() const { return std::format("{}|{:.2f}|{:.2f}|{:.2f}|{:.2f}", mask, k1, k2, shaftK, headK); }
    Knots Knots::Load(const char* v) {
        Knots k;
        if (!v) return k;
        float a = 1.0f, b = 1.0f, c = 1.0f, d = 1.0f;
        int m = 0;
        const int n = std::sscanf(v, "%d|%f|%f|%f|%f", &m, &a, &b, &c, &d);
        if (n >= 1) k.mask = m & 3;
        if (n >= 2) k.k1 = std::clamp(a, 0.0f, 2.0f);
        if (n >= 3) k.k2 = std::clamp(b, 0.0f, 2.0f);
        if (n >= 4) k.shaftK = std::clamp(c, 0.0f, 2.0f);
        if (n >= 5) k.headK = std::clamp(d, 0.0f, 2.0f);
        return k;
    }
    int AutoShape(RE::Actor* p, Knots* knots, std::string* why) {
        if (knots) *knots = {};
        if (!p) return kTip;
        Mnc::Override ov;   // 2.5k: a More Nasty Critters group's own settings win
        const bool hasOv = Size::IsCreatureActor(p) && Mnc::Active() && Mnc::OverrideFor(p, &ov);
        if (hasOv && ov.shape > 0) {
            if (knots) *knots = ov.knots;
            if (why) *why = std::format("{} - its group's own settings ({})", ShapeName(ov.shape), Mnc::GroupName(p));
            return ov.shape;
        }
        const std::string key = Size::ShapeKey(Size::Shape(p));
        int t = 0, sz[3];
        Knots tk, szk[3];
        {
            std::lock_guard lk(g_wmx);
            if (!key.empty()) {
                t = TypeShape(key);
                if (auto it = g_typeKnots.find(key); it != g_typeKnots.end()) tk = it->second;
            }
            std::copy(std::begin(g_sizeShape), std::end(g_sizeShape), sz);
            std::copy(std::begin(g_sizeKnots), std::end(g_sizeKnots), szk);
        }
        const int cat = Size::Cat(p);
        const int si = cat == 4 ? 2 : cat >= 3 ? 1 : 0;
        if (hasOv) tk = ov.knots;   // its own knots on its type's shape
        if (knots) {
            if (t > 0) *knots = tk;
            else {   // his type goes by size: the size row's knots, plus any ticked for his type (its own strength wins)
                Knots k = szk[si];
                k.shaftK *= tk.shaftK; k.headK *= tk.headK;   // 2.5s: his type's strength on top of his size's
                k.mask |= tk.mask;
                if (tk.mask & 1) k.k1 = tk.k1;
                if (tk.mask & 2) k.k2 = tk.k2;
                *knots = k;
            }
        }
        if (why) {
            static const char* szn[3] = {"XS / S / M or not known", "L", "XL"};
            std::string from = key.empty() ? std::string("his schlong type not found") : std::format("schlong type \"{}\"", key);
            if (Size::IsCreatureActor(p) && Mnc::Active()) {
                const std::string g = Mnc::GroupName(p);
                from += g.empty() ? " (creature in no More Nasty Critters group)" : std::format(" (More Nasty Critters group \"{}\"{})", g, hasOv ? ", own settings for its knots" : "");
            }
            *why = t > 0 ? std::format("{} - set on {}", ShapeName(t), from) : std::format("{} - {} goes by size: {}", ShapeName(sz[si]), from, szn[si]);
        }
        return t > 0 ? t : sz[si];
    }
    Want Get() {
        std::lock_guard lk(g_wmx);
        Want w = g_w;
        if (!g_canalOn) w.canal = 0.0f;
        if (!g_bulgeOn) w.bulge = 0.0f;
        return w;
    }
    bool BulgeFor(RE::Actor* man, RE::Actor* player) {
        int who;
        { std::lock_guard lk(g_wmx); who = g_bulgeWho; }
        if (man == player) return true;
        if (who == 2) return false;
        if (who == 1) return man && man->IsPlayerTeammate();
        return true;
    }
    float Donut() { return g_donutOn ? g_donutAmt.load() : 0.0f; }
    int DonutStyle() { return g_donutStyle.load(); }
    float DonutStyleStrength(int i) { return (i >= 0 && i < 5) ? g_styleAmt[i] : 1.0f; }
    void SyncPPA() {
        ReadToml();
        // PPA opens and places the hole. Earlier 2.4 test builds turned PPA's opening down to almost none - put the defaults back.
        if (!g_found || g_t.scale > 0.05f || g_t.scaleMax > 0.1f) return;
        const Tight d{};
        g_t.scale = d.scale;
        g_t.scaleMax = d.scaleMax;
        if (WriteToml()) logger::info("Anatomica: PPA's own opening restored (Scale {:.2f} / ScaleMax {:.2f})", d.scale, d.scaleMax);
    }

    void __stdcall RenderPage() {
        if (!g_read) ReadToml();
        bool changed = false;
        Tr::TextWrapped("How he looks and how tight he is while he is being taken. (What happens after the scene is on the "
                              "Gape, Movement and After sex pages.)");
        ImGuiMCP::Separator();

        if (Tr::CollapsingHeader("How tight he is (PPA)###tight")) {
            if (!g_found) {
                Tr::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.4f, 1.0f},
                                      "Needs the installer's \"PPA integration\" option (and Penetration Physics / PPA): its file "
                                      "SKSE\\Plugins\\ppa-override-configs\\anatomica.toml was not found.");
                ImGuiMCP::BeginDisabled();
            }
            Tr::TextWrapped("PPA (Penetration Physics) places the schlong in him and grips it. These are PPA's settings for "
                                  "Anatomica's anus - every man with Anatomica's skeleton bones.");
            static const char* presets[] = {"Choose a preset...", "Very tight", "Tight", "Anatomica default", "Loose", "Very loose"};
            static int pick = 0;
            if (Tr::Combo("Preset", &pick, presets, 6) && pick > 0) {
                constexpr float sc[5][2] = {{0.25f, 0.80f}, {0.32f, 0.95f}, {0.40f, 1.10f}, {0.55f, 1.50f}, {0.70f, 2.00f}};
                g_t.scale = sc[pick - 1][0];
                g_t.scaleMax = sc[pick - 1][1];
                if (pick == 3) g_t = Tight{};
                pick = 0;
                changed = true;
            }
            Help("Sets how far and how wide he opens. \"Anatomica default\" puts every setting below back to what the mod ships.");
            changed |= Tr::SliderFloat("How far he opens around the schlong", &g_t.scale, 0.0f, 1.5f, "%.2f");
            Help("PPA \"Scale\": how far the ring spreads for a schlong. Lower = tighter, he grips the shaft; higher = he opens "
                 "wider. Anatomica ships 0.40 (PPA's own default, 2.0, tears the cleft open on this mesh).");
            changed |= Tr::SliderFloat("The widest he can open", &g_t.scaleMax, 0.3f, 3.0f, "%.2f");
            Help("PPA \"ScaleMax\": a cap for big schlongs. Lower = even a huge one only opens him so far.");
            changed |= Tr::SliderFloat("Grip on the shaft (drag)", &g_t.grip, 0.0f, 2.0f, "%.2f");
            Help("PPA \"GripStrength\": how hard the rim holds onto the shaft - drawn in on the push, pulled out (a small pucker) "
                 "on the withdraw. 0 = no drag.");
            changed |= Tr::SliderFloat("Drawn in on the push", &g_t.fwd, 0.0f, 3.0f, "%.2f");
            Help("PPA \"GripClampForward\": how far the grip may pull the rim into his body.");
            changed |= Tr::SliderFloat("Pulled out on the withdraw", &g_t.back, 0.0f, 1.5f, "%.2f");
            Help("PPA \"GripClampBackward\": how far the grip may pull the rim out of his body (the pucker). Over 0.6 can look "
                 "extreme.");
            changed |= Tr::SliderFloat("Rubbing", &g_t.rub, 0.0f, 3.0f, "%.2f");
            Help("PPA \"RubIntensity\": how much the rim moves with each stroke.");
            if (changed) g_note = "Not saved yet - press Save to PPA.";
            if (Tr::Button("Save to PPA")) {
                g_note = WriteToml() ? "Saved. Press F5 in game (PPA's reload key) to use it now, or it applies at the next game start."
                                     : "Could not write anatomica.toml (is the PPA integration option installed?).";
                if (g_note[0] == 'S') logger::info("Anatomica: PPA tightness saved (Scale {:.2f}, ScaleMax {:.2f}, Grip {:.2f})", g_t.scale, g_t.scaleMax, g_t.grip);
            }
            ImGuiMCP::SameLine();
            if (Tr::Button("Read again")) { ReadToml(); g_note = "Read from anatomica.toml."; }
            if (!g_note.empty()) Tr::TextWrapped("%s", g_note.c_str());
            Tr::TextDisabled("PPA's settings are for every man at once. Saved in PPA's file, not in your Anatomica presets.");
            if (!g_found) ImGuiMCP::EndDisabled();
        }

        if (Tr::CollapsingHeader("The hole, the rim and the inside###inside")) {
            Tr::TextDisabled("These use Anatomica's own sliders (Anatomica Gape / Canal / Pout / Bulge 1-5): rebuild the body in "
                                   "BodySlide with \"Build Morphs\" ticked after updating Anatomica.");
            bool ch = false;
            std::lock_guard lk(g_wmx);
            ch |= Tr::Checkbox("Open the inside round the schlong", &g_canalOn);
            Help("While a schlong is in him the canal opens along its length, so the shaft no longer pushes through the inside "
                 "walls, opened by his partner's size. Recommended on.");
            if (g_canalOn) {
                float pct = g_w.canal * 100.0f;
                if (Tr::SliderFloat("How wide inside", &pct, 0.0f, 150.0f, "%.0f%%")) { g_w.canal = pct / 100.0f; ch = true; }
            }
            ch |= Tr::Checkbox("The rim follows the shaft", &g_w.ring);
            Help("The whole anus - the rim, the skin round it and the first part of the inside - moves with the shaft: drawn into "
                 "him on each push, pulled out with it on each withdraw, settling back as the strokes stop. Each way has its own "
                 "limit.");
            if (g_w.ring) {
                float gp = g_w.grip * 100.0f;
                if (Tr::SliderFloat("Grip on the shaft", &gp, 0.0f, 150.0f, "%.0f%%")) { g_w.grip = gp / 100.0f; ch = true; }
                Help("How much of each stroke the anus follows: 100% = it moves almost as far as the shaft does (up to the limits "
                     "below), 30% = a light tug.");
                ch |= Tr::SliderFloat("Most it is pulled out", &g_w.maxOut, 0.0f, 3.0f, "%.2f units");
                Help("The farthest it may stick out of him on a withdraw (game units, about 1.4 cm each). 0.5 is natural, 1.0 a big "
                     "pout, 2-3 extreme.");
                ch |= Tr::SliderFloat("Most it is drawn in", &g_w.maxIn, 0.0f, 3.0f, "%.2f units");
                Help("The farthest it may be drawn into him on a push.");
                float gr = g_w.grow * 100.0f;
                if (Tr::SliderFloat("Pull-out grows as he is worked", &gr, 0.0f, 200.0f, "+%.0f%%")) { g_w.grow = gr / 100.0f; ch = true; }
                Help("The pull-out limit grows over the scene as he is worked open, by up to this much (+100% = twice as far by the "
                     "end, +200% = three times). 0% = it never grows. It goes back with the gape after the scene.");
            }
            ch |= Tr::Checkbox("Open him round a werewolf, Vampire Lord or werebear", &g_w.beastOpen);
            Help("PPA leaves werewolves, Vampire Lords and werebears out of its scenes, so nothing opens him round them. With "
                 "this on Anatomica does it itself: his hole opens as wide as their schlong is where it passes through him, "
                 "following every thrust, and closes as they pull out. Human partners stay with PPA.");
            if (g_w.beastOpen) {
                float bf = g_w.beastFit * 100.0f;
                if (Tr::SliderFloat("How snug round them", &bf, 50.0f, 150.0f, "%.0f%%")) { g_w.beastFit = bf / 100.0f; ch = true; }
                Help("100% = the hole exactly as wide as the schlong. Lower is tighter (the schlong sinks into the rim a little), "
                     "higher leaves a gap round it.");
            }
            ch |= Tr::Checkbox("Belly bulge", &g_bulgeOn);
            Help("When the tip is deep in him his lower belly bulges where it is, and the bump moves up and down with each "
                 "stroke. Off by default.");
            if (g_bulgeOn) {
                float pct = g_w.bulge * 100.0f;
                if (Tr::SliderFloat("Bulge strength", &pct, 0.0f, 200.0f, "%.0f%%")) { g_w.bulge = pct / 100.0f; ch = true; }
                Help("The bulge shapes are built extreme, so 100% is the full, very obvious shape: 20-40% is a natural bump, "
                     "100% extreme, more than that exaggerated.");
                {
                    float sk = g_w.shaftK * 100.0f, hk = g_w.headK * 100.0f;
                    if (Tr::SliderFloat("Shaft strength", &sk, 0.0f, 200.0f, "%.0f%%")) { g_w.shaftK = sk / 100.0f; ch = true; }
                    Help("Only the shaft part (the sections up his belly, the Long tentacle too), on top of Bulge strength. 100% = as built. "
                         "For everyone: each schlong type can add its own on top (Integrations page).");
                    if (Tr::SliderFloat("Head strength", &hk, 0.0f, 200.0f, "%.0f%%")) { g_w.headK = hk / 100.0f; ch = true; }
                    Help("Only the head at the front (round, flared, colossal or hooked), on top of Bulge strength. For everyone: each "
                         "schlong type can add its own on top (Integrations page).");
                    Tr::TextDisabled("How strong it ends up: Bulge strength x Shaft / Head strength here x that schlong's own Shaft / "
                                           "Head (Integrations page, 100%% = no change). Knots: that schlong's Knot 1 / 2 strength.");
                }
                ch |= Tr::Checkbox("Super deep", &g_w.deep);
                Help("Some scenes go further: two more sections up his belly, almost to his chest, with the head at the end. "
                     "Rolled at the start of each scene. Works with every shape (rebuild the body with Build Morphs: the "
                     "Anatomica Bulge Deep sliders). The head travels the whole longer path, so it reaches the top only at "
                     "his deepest strokes.");
                if (g_w.deep) {
                    float dc = g_w.deepChance * 100.0f;
                    ImGuiMCP::Indent();
                    if (Tr::SliderFloat("Chance per scene", &dc, 0.0f, 100.0f, "%.0f%%")) { g_w.deepChance = dc / 100.0f; ch = true; }
                    Help("Rolled once when a scene's bulge first shows (after half a minute without one). 100% = every scene.");
                    const std::string last = Engine::LastDeepRoll();
                    Tr::TextDisabled("%s", last.empty() ? "No scene rolled yet this session." : last.c_str());
                    ImGuiMCP::Unindent();
                }
                if (Tr::TreeNode("Stomach###stomach")) {
                    float b = g_w.belly * 100.0f, r = g_w.bellyRandom * 100.0f;
                    if (Tr::SliderFloat("Stomach strength", &b, 0.0f, 200.0f, "%.0f%%")) { g_w.belly = b / 100.0f; ch = true; }
                    Help("The whole lower belly rounds out around the shaft (Anatomica Bulge Belly), on top of Bulge strength. 0% = off.");
                    ch |= Tr::SliderFloat("Stomach rises over", &g_w.bellyTime, 1.0f, 180.0f, "%.0f s");
                    Help("How long it takes to rise to its full size while he is taken. It goes back down after, more slowly.");
                    if (Tr::SliderFloat("Stomach randomness", &r, 0.0f, 100.0f, "%.0f%%")) { g_w.bellyRandom = r / 100.0f; ch = true; }
                    Help("How much it swells and settles from stroke to stroke. 0% = it rises evenly.");
                    ImGuiMCP::TreePop();
                }
                Tr::TextDisabled("The bulge's shape (and knots) comes from his partner's schlong: set for each schlong type and "
                                       "size on the Integrations page, under Partner size and shape%s.", Mnc::Active() ? " (creatures: More Nasty Critters)" : "");
                if (Tr::TreeNode("Movement for each shape###moves")) {
                    Tr::TextDisabled("Travelling: the head runs up and back down by itself, at its own pace (speed below).");
                    Tr::TextDisabled("Twitching: small, sudden jumps now and then.");
                    Tr::TextDisabled("Build-up: grows to the full shape over time instead of at once.");
                    if (ImGuiMCP::BeginTable("##mv", 4)) {
                        for (int i = 1; i < kShapeCount; ++i) {
                            ImGuiMCP::TableNextRow();
                            ImGuiMCP::TableNextColumn(); Tr::TextUnformatted(ShapeName(i));
                            ImGuiMCP::TableNextColumn(); ch |= Tr::Checkbox(std::format("Travelling##tr{}", i).c_str(), &g_w.travel[i]);
                            ImGuiMCP::TableNextColumn(); ch |= Tr::Checkbox(std::format("Twitching##tw{}", i).c_str(), &g_w.twitch[i]);
                            ImGuiMCP::TableNextColumn(); ch |= Tr::Checkbox(std::format("Build-up##bu{}", i).c_str(), &g_w.build[i]);
                        }
                        ImGuiMCP::EndTable();
                    }
                    {
                        float ts = g_w.travelSpeed * 100.0f;
                        if (Tr::SliderFloat("Travelling speed", &ts, 10.0f, 200.0f, "%.0f%%")) { g_w.travelSpeed = ts / 100.0f; ch = true; }
                        Help("100% = once up and back for each of his thrusts (as fast as he is going); 50% = every other "
                             "thrust; 200% = twice per thrust. When he is still it keeps the pace of his last strokes.");
                    }
                    ch |= Tr::SliderFloat("Build-up time", &g_w.buildTime, 1.0f, 60.0f, "%.0f s");
                    Help("How long the build-up takes to reach the full shape. It still moves in and out with his strokes while it grows.");
                    ch |= Tr::Checkbox("Hold the shape when full", &g_w.holdFull);
                    Help("Build-up: once it is full the shaft stays where it got to and only the head keeps moving.");
                    ImGuiMCP::TreePop();
                }
                ch |= Tr::Checkbox("Fade its bottom when his legs are pulled up", &g_w.hipFade);
                Help("In poses with his thighs drawn up towards his chest the skin above his groin folds, and the bottom of the bulge "
                     "would fold with it into a sharp crease. This fades the lowest part of the bulge (and the bottom of the stomach) "
                     "as his hips bend - standing, kneeling and most other poses are not affected.");
                if (g_w.hipFade) {
                    ImGuiMCP::Indent();
                    ch |= Tr::SliderFloat("Starts fading at a hip bend of", &g_w.hipFrom, 30.0f, 150.0f, "%.0f deg");
                    Help("0 = standing, 90 = sitting (thighs level), 130+ = knees towards his chest. It is gone 40 degrees further on. "
                         "Lower = it fades sooner.");
                    ImGuiMCP::Unindent();
                }
                float rnd = g_w.bulgeRandom * 100.0f;
                if (Tr::SliderFloat("Randomness", &rnd, 0.0f, 100.0f, "%.0f%%")) { g_w.bulgeRandom = rnd / 100.0f; ch = true; }
                Help("Each stroke is a little different - how high it rises and how big it gets. 0% = the same every stroke.");
                ch |= Tr::SliderFloat("Shows from this deep", &g_w.bulgeStart, 2.0f, 30.0f, "%.0f units");
                Help("How far in the tip has to be (game units, about 1.4 cm each) before the bulge starts. Then it fills up his "
                     "belly over the rest of each stroke (as deep as his strokes have been reaching, at least 10 units): section by "
                     "section from the bottom, the head running up ahead of it and back down as he pulls out. Lower = even average "
                     "schlongs show; higher = only very long ones.");
                ch |= Tr::Checkbox("Only from big partners (L and XL)", &g_w.bulgeBig);
                Help("Needs The New Gentleman for the size.");
                {
                    float o = g_w.outline * 100.0f;
                    {
                        if (Tr::SliderFloat("Trail under the bump", &o, 0.0f, 100.0f, "%.0f%%")) { g_w.outline = o / 100.0f; ch = true; }
                        Help("For schlongs whose shape is \"A bump at the tip\": a fainter trail below it, where the shaft is. 0% = only the bump.");
                    }
                    float pu = g_w.pulse * 100.0f;
                    if (Tr::SliderFloat("Thrust pulse", &pu, 0.0f, 100.0f, "%.0f%%")) { g_w.pulse = pu / 100.0f; ch = true; }
                    Help("Each fast push gives a quick extra bump that dies away at once - it reacts to the thrusts. 0% = smooth.");
                    ch |= Tr::SliderFloat("Goes down over", &g_w.linger, 0.5f, 30.0f, "%.1f s");
                    Help("After he pulls out the bump eases down over this long (up to half as long again after a long, hard "
                         "round) instead of vanishing at once.");
                    ch |= Tr::Checkbox("Imprint after sex", &g_w.imprint);
                    Help("After it goes down a faint copy of the last shape stays on his belly for a while, so what he was with "
                         "can still be seen, then fades away.");
                    if (g_w.imprint) {
                        float ia = g_w.imprintAmt * 100.0f;
                        if (Tr::SliderFloat("Imprint strength", &ia, 0.0f, 50.0f, "%.0f%%")) { g_w.imprintAmt = ia / 100.0f; ch = true; }
                        Help("Of the full shape (at the bulge strength). 10-20% is a faint ghost.");
                        ch |= Tr::SliderFloat("Imprint lasts", &g_w.imprintMin, 0.5f, 15.0f, "%.1f min");
                    }
                    static const char* thr[] = {"Nothing - only when he is bare", "Clothes, but not armour", "Clothes and armour"};
                    ch |= Tr::Combo("Shows through", &g_w.bulgeThrough, thr, 3);
                    Help("What the bulge may show through. Nothing: only when nothing covers his body (revealing armour, "
                         "Integrations page, counts as bare). Clothes: also through robes, shirts and other clothing. Clothes and "
                         "armour: always - it can poke through a stiff cuirass.");
                }
                static const char* who[] = {"Everyone", "You and your followers", "Only you"};
                ch |= Tr::Combo("Who gets the bulge", &g_bulgeWho, who, 3);
            }
            if (ch) {
                g_wmx.unlock();
                Colour::SaveNow();
                SyncPPA();
                g_wmx.lock();
            }
        }

        if (Tr::CollapsingHeader("Shape while he is taken###shape")) {
            bool on = g_donutOn;
            float amt = g_donutAmt;
            bool ch = false;
            ch |= Tr::Checkbox("Puffy ring (donut) while he is taken", &on);
            Help("The rim swells up into a raised ring round the hole as he is worked open - more the longer it goes on - and "
                 "settles a few seconds after the schlong leaves him (the after-sex swelling, After sex page, carries on from there). "
                 "Uses the Anatomica Ring slider: rebuild the body with \"Build Morphs\" in BodySlide.");
            if (on) {
                int st = g_donutStyle;
                static const char* styles[] = {"Puffy ring (a raised band)", "Donut (a full, round ring)", "Rosebud (the inside rolls out)",
                                               "Pout lips (fuller above and below)", "Pucker (drawn in, deep folds)"};
                if (Tr::Combo("Ring shape", &st, styles, 5)) { g_donutStyle = st; ch = true; }
                Help("Puffy ring: a raised band round the hole (Anatomica Ring). Donut: the rim rolls up into a fat, round ring "
                     "round an open hole (Anatomica Donut). Rosebud: the inner wall rolls out past the rim in a smooth, rounded bud "
                     "(Anatomica Rosebud). Pout lips: the rim swells into two soft lips above and below the hole (Anatomica Lips). "
                     "Pucker: the rim draws in round the hole in deep folds (Anatomica Pucker).");
                float sp = g_styleAmt[st] * 100.0f;
                if (Tr::SliderFloat("This shape's strength", &sp, 0.0f, 200.0f, "%.0f%%")) { g_styleAmt[st] = sp / 100.0f; ch = true; }
                Help("Each ring shape keeps its own strength (on top of \"How puffy, at most\"), so switching shapes keeps how "
                     "strong you set each one.");
                float pct = amt * 100.0f;
                if (Tr::SliderFloat("How puffy, at most", &pct, 0.0f, 200.0f, "%.0f%%")) { amt = pct / 100.0f; ch = true; }
            }
            if (ch) {
                g_donutOn = on;
                g_donutAmt = amt;
                Colour::SaveNow();
            }
        }

        Align::RenderSection();   // 2.5v: men and futas as the bottom (saves itself)
    }
}

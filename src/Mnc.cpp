#include "Mnc.h"
#include "During.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace {
    std::mutex g_mx;
    bool g_checked = false, g_active = false;
    std::unordered_map<std::string, std::string> g_raceGroup;    // race editor ID (lower case) -> group
    std::map<std::string, int> g_groupRaces;                      // group -> how many races (menu)
    std::unordered_map<RE::FormID, int> g_styleArmor;             // style armour -> shape
    std::map<std::string, int> g_shape;                           // group -> shape (from the ini / menu; else the default)
    struct Seen { int shape = 0; double at = -100.0; };
    std::unordered_map<RE::FormID, Seen> g_seen;                  // per creature, a few seconds

    double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    std::string Lower(std::string x) { for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return x; }
    bool Has(const std::string& s, std::initializer_list<const char*> w) {
        for (auto* x : w) if (s.find(x) != std::string::npos) return true;
        return false;
    }

    // the extra groups (not in MNC's files): matched by words in the race's editor ID
    struct WordGroup { const char* name; std::initializer_list<const char*> words; };
    const WordGroup kWordGroups[] = {
        {"Daedroth", {"daedroth"}},
        {"Demons", {"dremora", "demon", "daedra", "xivilai", "devil"}},
        {"Thri-kreen", {"thrikreen", "thri_kreen", "thri-kreen", "kreen"}},
        {"Kwama", {"kwama"}},
    };

    int DefaultShape(const std::string& group) {
        using namespace During;
        const std::string g = Lower(group);
        if (g == "daedroth" || g == "demons") return kRidged;
        if (g == "thri-kreen" || g == "kwama") return kSegmented;
        if (Has(g, {"canine", "dog", "wolves", "wolf", "fox", "werewol", "hound"})) return kKnot;
        if (Has(g, {"horse"})) return kHorse;
        if (Has(g, {"dragonpriest"})) return kShaft;
        if (Has(g, {"dragon", "sabre", "boar", "atronach", "daedr"})) return kRidged;
        if (Has(g, {"chaurus", "spider", "ashhopper", "dwarven", "scrib"})) return kSegmented;
        if (Has(g, {"lurker", "seeker", "netch", "spriggan", "icewraith", "wisp"})) return kTentacle;
        if (Has(g, {"troll", "giant", "gargoyle", "vampirelord", "bear", "mammoth", "horker"})) return kColossal;
        if (Has(g, {"draugr", "falmer", "hagraven", "riekling", "skeever", "deer", "goat", "cow", "elk"})) return kShaft;
        if (Has(g, {"chicken", "rabbit", "slaughterfish", "mudcrab"})) return kTip;
        return 0;   // automatic (by its schlong / size)
    }

    // a forgiving reader for these files: strings, ':' '[' ']' '{' '}' (missing / extra commas are fine)
    struct Tok { char t; std::string s; };   // t = 's' string, or the character
    std::vector<Tok> Lex(const std::string& txt) {
        std::vector<Tok> out;
        for (size_t i = 0; i < txt.size(); ++i) {
            const char c = txt[i];
            if (c == '"') {
                std::string s;
                for (++i; i < txt.size() && txt[i] != '"'; ++i) {
                    if (txt[i] == '\\' && i + 1 < txt.size()) ++i;
                    s += txt[i];
                }
                out.push_back({'s', s});
            } else if (c == ':' || c == '[' || c == ']' || c == '{' || c == '}') out.push_back({c, {}});
            else if (c == '/' && i + 1 < txt.size() && txt[i + 1] == '/') { while (i < txt.size() && txt[i] != '\n') ++i; }
        }
        return out;
    }
    std::string ReadFile(const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        return ss.str();
    }
    RE::FormID FormOf(const std::string& v) {   // "__formData|Plugin.esp|0x1234"
        const auto a = v.find('|'), b = v.rfind('|');
        if (a == std::string::npos || b == a) return 0;
        const std::string plugin = v.substr(a + 1, b - a - 1);
        RE::FormID id = 0;
        try { id = static_cast<RE::FormID>(std::stoul(v.substr(b + 1), nullptr, 16)); } catch (...) { return 0; }
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* f = dh ? dh->LookupForm(id & 0xFFFFFF, plugin) : nullptr;
        return f ? f->GetFormID() : 0;
    }

    void Load() {   // under g_mx
        g_checked = true;
        auto* dh = RE::TESDataHandler::GetSingleton();
        g_active = dh && dh->LookupModByName("MoreNastyCritters.esp");
        if (!g_active) return;
        namespace fs = std::filesystem;
        std::error_code ec;
        // race groups
        const fs::path reg = "Data/creatures.d/MNC_SLRaceRegistration.d";
        for (auto& e : fs::directory_iterator(reg, ec)) {
            if (!e.is_regular_file() || Lower(e.path().extension().string()) != ".json") continue;
            const auto T = Lex(ReadFile(e.path()));
            for (size_t i = 0; i + 2 < T.size(); ++i) {
                if (!(T[i].t == 's' && T[i].s == "raceGroups" && T[i + 1].t == ':' && T[i + 2].t == '{')) continue;
                int depth = 0;
                for (size_t j = i + 2; j < T.size(); ++j) {
                    if (T[j].t == '{') ++depth;
                    else if (T[j].t == '}') { if (--depth == 0) break; }
                    else if (depth == 1 && T[j].t == 's' && j + 2 < T.size() && T[j + 1].t == ':' && T[j + 2].t == '[') {
                        const std::string group = T[j].s;
                        size_t k = j + 3;
                        for (; k < T.size() && T[k].t != ']'; ++k)
                            if (T[k].t == 's' && !T[k].s.empty()) {
                                auto [it, added] = g_raceGroup.emplace(Lower(T[k].s), group);
                                if (added) ++g_groupRaces[group];
                            }
                        j = k;
                    }
                }
                break;
            }
        }
        for (auto& w : kWordGroups) g_groupRaces.emplace(w.name, 0);
        // Creature Framework styles: the armour worn when aroused
        for (auto& e : fs::directory_iterator("Data/creatures.d", ec)) {
            if (!e.is_regular_file() || Lower(e.path().extension().string()) != ".json") continue;
            const auto T = Lex(ReadFile(e.path()));
            std::string name;
            for (size_t i = 0; i + 2 < T.size(); ++i)
                if (T[i].t == 's' && (T[i].s == "ModName" || T[i].s == "modID") && T[i + 1].t == ':' && T[i + 2].t == 's') name += " " + Lower(T[i + 2].s);
            int shape = 0;
            if (Has(name, {"dog style", "canine", "wolf style", "dragsdog", "weredog"})) shape = During::kKnot;
            else if (Has(name, {"horse style", "dragshorse", "werehorse"})) shape = During::kHorse;
            else if (Has(name, {"dragon style", "dragsdrag"})) shape = During::kRidged;
            if (!shape) continue;
            for (size_t i = 0; i + 2 < T.size(); ++i)
                if (T[i].t == 's' && (T[i].s == "arousedArmor" || T[i].s == "normalArmor") && T[i + 1].t == ':' && T[i + 2].t == 's')
                    if (const auto id = FormOf(T[i + 2].s)) g_styleArmor[id] = shape;
        }
        logger::info("Anatomica: More Nasty Critters - {} races in {} groups, {} style armours", g_raceGroup.size(), g_groupRaces.size(),
                     g_styleArmor.size());
    }
    void Ensure() { if (!g_checked) Load(); }
    int GroupShape(const std::string& g) {
        if (auto it = g_shape.find(g); it != g_shape.end()) return it->second;
        return DefaultShape(g);
    }
}

namespace Mnc {
    bool Active() {
        std::lock_guard lk(g_mx);
        Ensure();
        return g_active;
    }
    int ShapeFor(RE::Actor* a) {
        if (!a) return 0;
        std::lock_guard lk(g_mx);
        Ensure();
        if (!g_active) return 0;
        auto& seen = g_seen[a->GetFormID()];
        if (Now() - seen.at < 5.0) return seen.shape;
        int shape = 0;
        // the style he wears (werewolves, dragons)
        if (!g_styleArmor.empty())
            if (auto* inv = a->GetInventoryChanges(); inv && inv->entryList)
                for (auto* e : *inv->entryList)
                    if (e && e->object && e->IsWorn())
                        if (auto it = g_styleArmor.find(e->object->GetFormID()); it != g_styleArmor.end()) { shape = it->second; break; }
        if (!shape)
            if (auto* race = a->GetRace()) {
                const std::string ed = race->formEditorID.c_str() ? Lower(race->formEditorID.c_str()) : std::string{};
                if (auto it = g_raceGroup.find(ed); it != g_raceGroup.end()) shape = GroupShape(it->second);
                else
                    for (auto& w : kWordGroups)
                        if (Has(ed, w.words)) { shape = GroupShape(w.name); break; }
            }
        seen = {shape, Now()};
        return shape;
    }
    std::vector<Group> Groups() {
        std::lock_guard lk(g_mx);
        Ensure();
        std::vector<Group> out;
        for (auto& [g, n] : g_groupRaces) out.push_back({g, GroupShape(g), n});
        return out;
    }
    void SetShape(const std::string& group, int shape) {
        std::lock_guard lk(g_mx);
        g_shape[group] = shape;
        g_seen.clear();
    }
    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "CreatureBulge";
        std::lock_guard lk(g_mx);
        if (save) {
            for (auto& [g, v] : g_shape) ini.SetLongValue(S, g.c_str(), v);
            return;
        }
        CSimpleIniA::TNamesDepend keys;
        ini.GetAllKeys(S, keys);
        for (auto& k : keys) g_shape[k.pItem] = std::clamp(static_cast<int>(ini.GetLongValue(S, k.pItem, 0)), 0, During::kShapeCount - 1);
        g_seen.clear();
    }
}

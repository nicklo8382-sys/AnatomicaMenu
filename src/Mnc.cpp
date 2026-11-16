#include "Mnc.h"
#include "During.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
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
    std::map<std::string, std::string> g_type;                    // group -> type key set in the menu ("auto" = automatic)
    std::unordered_map<RE::FormID, std::string> g_styleType;      // style armour -> type key
    std::map<std::string, Mnc::Override> g_ov;                    // 2.5k: group -> its own settings (an override of its type)
    std::map<std::string, std::vector<std::string>> g_custom;     // 2.5q: kinds added in the menu: name -> words in the race ID

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
        {"Clannfear", {"clannfear"}},
    };

    std::string DefaultType(const std::string& group) {   // 2.5f: the Size type a group counts as
        const std::string g = Lower(group);
        if (g == "daedroth" || g == "demons") return "ridged";
        if (g == "thri-kreen" || g == "kwama") return "segmented";
        if (g == "clannfear") return "twin";
        if (Has(g, {"fox", "goat", "deer", "skeever", "rabbit", "elk"})) return "tapered";
        if (Has(g, {"canine", "dog", "wolves", "wolf", "werewol", "hound"})) return "knotted";
        if (Has(g, {"horse"})) return "flared";
        if (Has(g, {"boar", "cow", "bull", "minotaur"})) return "pizzle";
        if (Has(g, {"giant", "mammoth", "lurker", "troll", "bear", "gargoyle", "vampirelord", "ogre", "ogrim"})) return "colossal";
        if (Has(g, {"dwarven", "centurion"})) return "piston";
        if (Has(g, {"chaurus", "spider", "ashhopper", "scrib"})) return "segmented";
        if (Has(g, {"draugr", "dragonpriest", "skeleton", "zombie"})) return "cold";
        if (Has(g, {"atronach", "ash guardian", "ashguardian"})) return "elemental";
        if (Has(g, {"dragon"})) return "ridged";
        if (Has(g, {"sabre"})) return "barbed";
        if (Has(g, {"lurker", "seeker", "netch", "spriggan", "icewraith", "wisp"})) return "tentacle";
        return {};
    }
    std::string GroupType(const std::string& g) {
        if (auto it = g_type.find(g); it != g_type.end()) return it->second == "auto" ? std::string{} : it->second;
        return DefaultType(g);
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
                        std::string group = T[j].s;
                        if (Lower(group) == "dogs") group = "Canines";   // 2.5q: the same thing - one group
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
            const char* type = nullptr;
            if (Has(name, {"dog style", "canine", "wolf style", "dragsdog", "weredog"})) type = "knotted";
            else if (Has(name, {"horse style", "dragshorse", "werehorse"})) type = "flared";
            else if (Has(name, {"dragon style", "dragsdrag"})) type = "ridged";
            if (!type) continue;
            for (size_t i = 0; i + 2 < T.size(); ++i)
                if (T[i].t == 's' && (T[i].s == "arousedArmor" || T[i].s == "normalArmor") && T[i + 1].t == ':' && T[i + 2].t == 's')
                    if (const auto id = FormOf(T[i + 2].s)) g_styleType[id] = type;
        }
        logger::info("Anatomica: More Nasty Critters - {} races in {} groups, {} style armours", g_raceGroup.size(), g_groupRaces.size(),
                     g_styleType.size());
    }
    void Ensure() { if (!g_checked) Load(); }
    std::string GroupOf(RE::Actor* a) {   // under g_mx
        auto* race = a ? a->GetRace() : nullptr;
        if (!race) return {};
        const std::string ed = race->formEditorID.c_str() ? Lower(race->formEditorID.c_str()) : std::string{};
        for (auto& [name, words] : g_custom)   // 2.5q: kinds added in the menu come first
            for (auto& w : words) if (!w.empty() && ed.find(w) != std::string::npos) return name;
        if (auto it = g_raceGroup.find(ed); it != g_raceGroup.end()) return it->second;
        for (auto& w : kWordGroups) if (Has(ed, w.words)) return w.name;
        // 2.5r: a race MNC does not list (a follower dog, another mod's wolf...): by the group's name in its race ID, so it
        // still gets that group's settings instead of a plain guess. Longest names first (werewolf before wolf).
        static const std::pair<const char*, const char*> kAlias[] = {
            {"dog", "Canines"}, {"hound", "Canines"}, {"husky", "Canines"}, {"mastiff", "Canines"}, {"mutt", "Canines"},
            {"sabercat", "SabreCats"}, {"mare", "Horses"}, {"stallion", "Horses"}, {"pony", "Horses"}};
        std::vector<std::pair<std::string, std::string>> stems;   // stem -> group
        for (auto& [g, n] : g_groupRaces) {
            std::string st = Lower(g);
            if (st.ends_with("wolves")) st = st.substr(0, st.size() - 3) + "f";
            else if (st.ends_with("es") && !st.ends_with("ces") && !st.ends_with("ges") && !st.ends_with("res")) st.resize(st.size() - 2);
            else if (st.ends_with("s")) st.pop_back();
            if (st.size() >= 3) stems.emplace_back(st, g);
        }
        for (auto& [w, g] : kAlias) if (g_groupRaces.count(g)) stems.emplace_back(w, g);
        std::sort(stems.begin(), stems.end(), [](auto& x, auto& y) { return x.first.size() > y.first.size(); });
        for (auto& [st, g] : stems) if (ed.find(st) != std::string::npos) return g;
        return {};
    }
}

namespace Mnc {
    bool Active() {
        std::lock_guard lk(g_mx);
        Ensure();
        return g_active;
    }
    std::string Override::Save() const {
        return std::format("{}|{:.2f}|{:.2f}|{:.2f}|{:.2f}|{}|{}", on ? 1 : 0, open, build, loose, tender, shape, knots.Save());
    }
    Override Override::Load(const char* v) {
        Override o;
        if (!v) return o;
        int on = 0, sh = 0;
        float a = 1, b = 1, c = 1, d = 1;
        const int n = std::sscanf(v, "%d|%f|%f|%f|%f|%d", &on, &a, &b, &c, &d, &sh);
        if (n >= 1) o.on = on != 0;
        if (n >= 5) { o.open = std::clamp(a, 0.5f, 2.0f); o.build = std::clamp(b, 0.5f, 2.0f); o.loose = std::clamp(c, 0.5f, 2.0f); o.tender = std::clamp(d, 0.5f, 2.0f); }
        if (n >= 6) o.shape = std::clamp(sh, 0, During::kShapeCount - 1);
        // the rest is its knots and shaft / head strength (Knots' own format)
        const char* p = v;
        for (int bars = 0; *p && bars < 6; ++p) if (*p == '|') ++bars;
        if (n >= 6 && *p) o.knots = During::Knots::Load(p);
        return o;
    }
    bool OverrideFor(RE::Actor* a, Override* out) {
        if (!a) return false;
        std::lock_guard lk(g_mx);
        Ensure();
        if (!g_active) return false;
        const std::string g = GroupOf(a);
        if (g.empty()) return false;
        auto it = g_ov.find(g);
        if (it == g_ov.end() || !it->second.on) return false;
        if (out) *out = it->second;
        return true;
    }
    void SetOverride(const std::string& group, const Override& o) {
        std::lock_guard lk(g_mx);
        g_ov[group] = o;
    }
    std::string TypeFor(RE::Actor* a) {
        if (!a) return {};
        std::lock_guard lk(g_mx);
        Ensure();
        if (!g_active) return {};
        if (!g_styleType.empty())
            if (auto* inv = a->GetInventoryChanges(); inv && inv->entryList)
                for (auto* e : *inv->entryList)
                    if (e && e->object && e->IsWorn())
                        if (auto it = g_styleType.find(e->object->GetFormID()); it != g_styleType.end()) return it->second;
        const std::string g = GroupOf(a);
        return g.empty() ? std::string{} : GroupType(g);
    }
    void SetType(const std::string& group, const std::string& typeKey) {
        std::lock_guard lk(g_mx);
        g_type[group] = typeKey.empty() ? std::string("auto") : typeKey;
    }
    std::string GroupName(RE::Actor* a) {
        std::lock_guard lk(g_mx);
        Ensure();
        return g_active ? GroupOf(a) : std::string{};
    }
    std::vector<Group> Groups() {
        std::lock_guard lk(g_mx);
        Ensure();
        std::vector<Group> out;
        for (auto& [g, words] : g_custom) {   // 2.5q: added in the menu (first)
            std::string w;
            for (auto& x : words) w += (w.empty() ? "" : ", ") + x;
            out.insert(out.begin(), {g, -1, GroupType(g), g_ov.count(g) ? g_ov[g] : Override{}, w});
        }
        for (auto& [g, n] : g_groupRaces) if (!g_custom.count(g)) out.push_back({g, n, GroupType(g), g_ov.count(g) ? g_ov[g] : Override{}, {}});
        return out;
    }
    void AddKind(const std::string& name, const std::string& words) {
        std::vector<std::string> v;
        std::string cur;
        for (char c : words + ",") {
            if (c == ',') {
                const auto a = cur.find_first_not_of(" \t"), b = cur.find_last_not_of(" \t");
                if (a != std::string::npos) v.push_back(Lower(cur.substr(a, b - a + 1)));
                cur.clear();
            } else cur += c;
        }
        std::lock_guard lk(g_mx);
        if (!name.empty() && !v.empty()) g_custom[name] = v;
    }
    void RemoveKind(const std::string& name) {
        std::lock_guard lk(g_mx);
        g_custom.erase(name);
        g_ov.erase(name);
        g_type.erase(name);
    }
    std::string RaceId(RE::Actor* a) {
        auto* race = a ? a->GetRace() : nullptr;
        return race && race->formEditorID.c_str() ? Lower(race->formEditorID.c_str()) : std::string{};
    }
    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* SO = "CreatureOverride";
        std::lock_guard lk(g_mx);
        if (save) {
            for (auto& [g, v] : g_type) ini.SetValue("CreatureType", g.c_str(), v.c_str());
            for (auto& [g, v] : g_ov) ini.SetValue(SO, g.c_str(), v.Save().c_str());
            ini.Delete("CreatureKinds", nullptr);
            for (auto& [g, words] : g_custom) {
                std::string w;
                for (auto& x : words) w += (w.empty() ? "" : ", ") + x;
                ini.SetValue("CreatureKinds", g.c_str(), w.c_str());
            }
            return;
        }
        CSimpleIniA::TNamesDepend kt;
        ini.GetAllKeys("CreatureType", kt);
        for (auto& k : kt) g_type[k.pItem] = Lower(ini.GetValue("CreatureType", k.pItem, "auto"));
        CSimpleIniA::TNamesDepend ko;
        ini.GetAllKeys(SO, ko);
        for (auto& k : ko) g_ov[k.pItem] = Override::Load(ini.GetValue(SO, k.pItem, "0"));
        CSimpleIniA::TNamesDepend kc;
        ini.GetAllKeys("CreatureKinds", kc);
        g_custom.clear();
        for (auto& k : kc) {
            std::vector<std::string> v;
            std::string cur, words = std::string(ini.GetValue("CreatureKinds", k.pItem, "")) + ",";
            for (char c : words) {
                if (c == ',') { const auto a = cur.find_first_not_of(" \t"), b = cur.find_last_not_of(" \t"); if (a != std::string::npos) v.push_back(Lower(cur.substr(a, b - a + 1))); cur.clear(); }
                else cur += c;
            }
            if (!v.empty()) g_custom[k.pItem] = v;
        }
    }
}

#include "Size.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <vector>
#include <cstdio>

// See Size.h. TNG's API is Papyrus only, so its answer comes back through the VM a frame or two later; until then the
// partner counts as average. Nothing here calls into the rest of Anatomica (safe to call under other modules' locks).

namespace {
    struct Settings {
        bool on = true;
        bool gape = true;          // how far / how fast he opens
        bool loose = true;         // lasting looseness a round adds
        bool tender = true;        // how sore and red he gets
        float strength = 1.0f;     // 0-2: how much size matters
        bool creatures = true;     // creatures count as the biggest
        int creatureCat = Size::kCreature;   // 3 = L, 4 = XL, 5 = bigger than any man
        bool shape = true;         // the shape of his schlong matters too (cut / uncut, knotted, flared, ridged...)
        float shapeStrength = 1.0f;
    } g;
    //                               XS     S     M     L     XL   creature
    constexpr float kOpen[6]   = {0.75f, 0.88f, 1.0f, 1.15f, 1.30f, 1.50f};
    constexpr float kBuild[6]  = {1.30f, 1.15f, 1.0f, 0.85f, 0.70f, 0.55f};   // time to open up (x)
    constexpr float kLoose[6]  = {0.60f, 0.80f, 1.0f, 1.25f, 1.50f, 1.90f};
    constexpr float kTender[6] = {0.75f, 0.90f, 1.0f, 1.15f, 1.30f, 1.50f};
    constexpr const char* kName[6] = {"XS", "S", "M", "L", "XL", "creature"};

    double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    struct Known { int cat = Size::kUnknown; double at = 0.0; bool asking = false; };
    std::mutex g_mx;
    std::unordered_map<RE::FormID, Known> g_known;

    struct Answer : RE::BSScript::IStackCallbackFunctor {   // TNG_PapyrusUtil.GetActorSize: 0-4, -1 = a problem
        RE::FormID id = 0;
        void operator()(RE::BSScript::Variable r) override {
            const int v = r.IsInt() ? r.GetSInt() : -1;
            std::lock_guard lk(g_mx);
            auto& k = g_known[id];
            k.cat = (v >= 0 && v <= 4) ? v : Size::kUnknown;
            k.at = Now();
            k.asking = false;
        }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
    };

    void Ask(RE::FormID id) {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) { std::lock_guard lk(g_mx); g_known[id].asking = false; return; }
        tasks->AddTask([id]() {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!vm || !a) { std::lock_guard lk(g_mx); g_known[id].asking = false; return; }
            auto c = RE::make_smart<Answer>();
            c->id = id;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb(c);
            if (!vm->DispatchStaticCall("TNG_PapyrusUtil", "GetActorSize", RE::MakeFunctionArguments(static_cast<RE::Actor*>(a)), cb)) {
                std::lock_guard lk(g_mx);
                auto& k = g_known[id];
                k.asking = false;
                k.at = Now();
            }
        });
    }

    bool IsCreature(RE::Actor* a) {
        auto* race = a ? a->GetRace() : nullptr;
        if (!race) return false;
        static RE::BGSKeyword* npcKw = nullptr;
        static bool tried = false;
        if (!tried) { tried = true; if (auto* dh = RE::TESDataHandler::GetSingleton()) npcKw = dh->LookupForm<RE::BGSKeyword>(0x13794, "Skyrim.esm"); }   // ActorTypeNPC
        std::string ed = race->formEditorID.c_str() ? race->formEditorID.c_str() : "";
        for (auto& ch : ed) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ed.find("werewolf") != std::string::npos || ed.find("werebear") != std::string::npos || ed.find("vampirelord") != std::string::npos) return true;
        return npcKw && !race->HasKeyword(npcKw);
    }

    float Scale(float base) { return std::max(0.2f, 1.0f + (base - 1.0f) * g.strength); }
    float ScaleS(float base) { return std::max(0.2f, 1.0f + (base - 1.0f) * g.shapeStrength); }

    // ---- schlong shapes: a table anyone can change or extend without a new DLL ---------------------------------------
    // Shapes: built in (below), then every Data/SKSE/Plugins/AnatomicaMenu/Schlongs/*.ini (compatibility files from
    // schlong mods, read A-Z), then the menu's own ([ShapeTypes] in AnatomicaMenu.ini). A later one with the same key
    // replaces the earlier one.
    // Which shape a man has: the menu's choice for him > [Actors] in a file > a keyword "Anatomica_Shape_<Key>" on his
    // schlong armour > [Forms] > [Names] > TNG's own types > a guess from the armour's name.
    struct ShapeDef { std::string key, name, word, desc, source; float open = 1.0f, build = 1.0f, loose = 1.0f, tender = 1.0f; };
    std::vector<ShapeDef> g_defs;   // [0] = not known
    using Pairs = std::vector<std::pair<std::string, std::string>>;   // (shape key, target)
    Pairs g_namesRaw, g_formsRaw, g_actorsRaw, g_racesRaw, g_menuActors;
    std::unordered_map<std::string, std::string> g_byName;            // armour name (lower case) -> key
    std::unordered_map<RE::FormID, std::string> g_byForm, g_byActor;  // resolved once the game's forms are there
    bool g_resolved = false;
    std::vector<std::string> g_fileList;                              // compatibility files found (menu)
    constexpr const char* kShapeDir = "Data/SKSE/Plugins/AnatomicaMenu/Schlongs";

    std::string LowerS(std::string x) { for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return x; }
    std::string TrimS(std::string x) {
        const auto a = x.find_first_not_of(" \t\r\n\""), b = x.find_last_not_of(" \t\r\n\"");
        return a == std::string::npos ? std::string{} : x.substr(a, b - a + 1);
    }
    std::vector<std::string> SplitS(const std::string& s, char sep) {
        std::vector<std::string> out; std::string cur;
        for (char c : s) { if (c == sep) { out.push_back(TrimS(cur)); cur.clear(); } else cur += c; }
        out.push_back(TrimS(cur));
        return out;
    }
    int DefIndex(const std::string& key) {
        const std::string k = LowerS(key);
        for (size_t i = 1; i < g_defs.size(); ++i) if (g_defs[i].key == k) return static_cast<int>(i);
        return 0;
    }
    // "Name | word | open | build | loose | soreness | description" (later fields can be left out)
    void Define(const std::string& key, const std::string& v, const std::string& src) {
        const std::string k = LowerS(TrimS(key));
        if (k.empty()) return;
        int i = DefIndex(k);
        if (!i) { g_defs.push_back({k, k, k, "", src}); i = static_cast<int>(g_defs.size()) - 1; }
        ShapeDef& d = g_defs[i];
        d.source = src;
        const auto f = SplitS(v, '|');
        auto num = [&](size_t n, float& out) { if (f.size() > n && !f[n].empty()) try { out = std::clamp(std::stof(f[n]), 0.2f, 4.0f); } catch (...) {} };
        if (f.size() > 0 && !f[0].empty()) d.name = f[0];
        if (f.size() > 1) d.word = f[1];
        num(2, d.open); num(3, d.build); num(4, d.loose); num(5, d.tender);
        if (f.size() > 6) d.desc = f[6];
    }
    void Builtins() {
        g_defs.clear();
        g_defs.push_back({"", "not known", "", "", "built in"});
        //        key         name                 word           open   build  loose  sore   description
        auto add = [](const char* k, const char* n, const char* w, float o, float b, float l, float t, const char* d) {
            g_defs.push_back({k, n, w, d, "built in", o, b, l, t});
        };
        add("cut",      "Circumcised",       "circumcised", 1.00f, 1.00f, 1.00f, 1.10f, "A little more friction: a little sorer.");
        add("uncut",    "Uncut (foreskin)",  "uncut",       1.00f, 1.05f, 0.95f, 0.90f, "The foreskin glides: a little gentler.");
        add("knotted",  "Knotted",           "knotted",     1.25f, 0.90f, 1.30f, 1.10f, "The knot stretches him much wider and loosens him more.");
        add("flared",   "Flared",            "flared",      1.20f, 0.90f, 1.20f, 1.05f, "The flared head opens him wider, looser.");
        add("ridged",   "Ridged / shaped",   "ridged",      1.00f, 1.00f, 1.05f, 1.25f, "Ridges, plates and scales: sorer.");
        add("barbed",   "Barbed",            "barbed",      1.00f, 1.00f, 1.00f, 1.40f, "Barbs: much sorer.");
        add("tentacle", "Tentacle",          "tentacled",   0.90f, 0.80f, 1.00f, 0.95f, "Slides in quickly, opens him less.");
    }
    // a guess from an armour / race name (when nothing tells us)
    bool HasW(const std::string& s, std::initializer_list<const char*> ws) { for (auto* w : ws) if (s.find(w) != std::string::npos) return true; return false; }
    std::string Guess(const std::string& n) {
        if (n.empty()) return {};
        if (HasW(n, {"circumcis", "(cut)", " cut "})) return "cut";
        if (HasW(n, {"uncut", "foreskin", "regular", "muscular", "human", "nord", "imperial", "breton", "redguard", "altmer", "bosmer", "dunmer", "orsimer", "orc", "elder", "afflicted"})) return "uncut";
        if (HasW(n, {"canine", "knot", "dog", "wolf", "fox", "husky"})) return "knotted";
        if (HasW(n, {"horse", "equine", "flare", "centaur", "ungulate"})) return "flared";
        if (HasW(n, {"tentacle"})) return "tentacle";
        if (HasW(n, {"khajiit", "feline", "barb", "sabre", "cat "})) return "barbed";
        if (HasW(n, {"argonian", "croc", "drake", "wyvern", "serpent", "plated", "segment", "reptil", "dragon", "hybrid", "twist", "ridge", "lizard", "shark"})) return "ridged";
        return {};
    }
    RE::FormID ResolveForm(const std::string& t) {   // "Plugin.esp|ID" (ID without the load order byte); "Player"
        if (LowerS(t) == "player") return 0x14;
        const auto p = SplitS(t, '|');
        if (p.size() < 2) return 0;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return 0;
        RE::FormID id = 0;
        try { id = static_cast<RE::FormID>(std::stoul(p[1], nullptr, 16)) & 0xFFFFFF; } catch (...) { return 0; }
        return dh->LookupFormID(id, p[0]);
    }
    void Resolve() {   // under g_mx
        if (g_resolved || !RE::TESDataHandler::GetSingleton()) return;
        g_resolved = true;
        g_byForm.clear(); g_byActor.clear();
        for (auto& [k, t] : g_formsRaw) if (auto id = ResolveForm(t)) g_byForm[id] = k;
        for (auto& [k, t] : g_actorsRaw) if (auto id = ResolveForm(t)) g_byActor[id] = k;
        for (auto& [k, t] : g_menuActors) if (auto id = ResolveForm(t)) g_byActor[id] = k;   // the menu's choices win
    }
    void LoadShapeFiles() {
        g_namesRaw.clear(); g_formsRaw.clear(); g_actorsRaw.clear(); g_racesRaw.clear(); g_fileList.clear();
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(kShapeDir, ec))
            if (e.is_regular_file(ec) && LowerS(e.path().extension().string()) == ".ini") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (auto& f : files) {
            CSimpleIniA ini(false, true);   // the same key may repeat: one line per schlong
            if (ini.LoadFile(f.string().c_str()) < 0) continue;
            const std::string src = f.filename().string();
            g_fileList.push_back(src);
            CSimpleIniA::TNamesDepend keys;
            ini.GetAllKeys("Shapes", keys);
            for (auto& k : keys) Define(k.pItem, ini.GetValue("Shapes", k.pItem, ""), src);
            auto pairs = [&](const char* sec, Pairs& out) {
                CSimpleIniA::TNamesDepend ks;
                ini.GetAllKeys(sec, ks);
                for (auto& k : ks) {
                    CSimpleIniA::TNamesDepend vals;
                    ini.GetAllValues(sec, k.pItem, vals);
                    for (auto& v : vals) for (auto& t : SplitS(v.pItem, ',')) if (!t.empty()) out.push_back({LowerS(TrimS(k.pItem)), t});
                }
            };
            pairs("Names", g_namesRaw); pairs("Forms", g_formsRaw); pairs("Actors", g_actorsRaw); pairs("Races", g_racesRaw);
        }
        g_byName.clear();
        for (auto& [k, n] : g_namesRaw) g_byName[LowerS(n)] = k;
    }

    // every schlong armour (slot 52: TNG's own and its add-ons, SOS-style schlongs) by the armatures it is made of - TNG
    // builds each man's skin out of those armatures, so his skin tells which one he has
    std::unordered_map<RE::TESObjectARMA*, RE::TESObjectARMO*> g_byArma;
    bool g_indexed = false;
    void Index() {
        if (g_indexed) return;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;
        g_indexed = true;
        for (auto* armo : dh->GetFormArray<RE::TESObjectARMO>()) {
            if (!armo || !armo->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kModPelvisSecondary)) continue;
            if (armo->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) continue;   // skins / outfits that also cover 52
            for (auto* arma : armo->armorAddons) if (arma && !g_byArma.count(arma)) g_byArma[arma] = armo;
        }
        logger::info("Anatomica: {} schlong armatures indexed (partner shape)", g_byArma.size());
    }

    struct ShapeInfo { int shape = 0; std::string from, how; };
    ShapeInfo Pick(const std::string& key, std::string from, const char* how) { return {DefIndex(key), std::move(from), how}; }
    ShapeInfo Classify(RE::TESObjectARMO* armo, RE::TESObjectARMA* arma) {   // under g_mx
        if (!armo) return {};
        const std::string nm = armo->GetName() ? armo->GetName() : "";
        std::string from = nm.empty() ? std::format("{:08X}", armo->GetFormID()) : nm;
        for (std::uint32_t i = 0; i < armo->numKeywords; ++i) {   // a schlong mod's own cue: keyword Anatomica_Shape_<Key>
            auto* kw = armo->keywords[i];
            const std::string e = kw && kw->formEditorID.c_str() ? LowerS(kw->formEditorID.c_str()) : "";
            if (e.rfind("anatomica_shape_", 0) == 0) if (int s = DefIndex(e.substr(16))) return {s, from, "its keyword"};
        }
        if (auto it = g_byForm.find(armo->GetFormID()); it != g_byForm.end()) return Pick(it->second, from, "a compatibility file");
        if (arma) if (auto it = g_byForm.find(arma->GetFormID()); it != g_byForm.end()) return Pick(it->second, from, "a compatibility file");
        if (auto it = g_byName.find(LowerS(nm)); it != g_byName.end()) return Pick(it->second, from, "a compatibility file");
        const auto* file = armo->GetFile(0);
        if (file && LowerS(std::string(file->GetFilename())) == "thenewgentleman.esp") {   // TNG's own (names are $translation keys)
            const RE::FormID id = armo->GetLocalFormID();
            if (id == 0xA01) return Pick("cut", "TNG Circumcised", "TNG's type");
            if (id >= 0xA02 && id <= 0xA05) return Pick("uncut", (id == 0xA02 || id == 0xA04) ? "TNG Muscular" : "TNG Regular", "TNG's type");
            return {0, from, ""};
        }
        if (auto k = Guess(" " + LowerS(nm) + " "); !k.empty()) return Pick(k, from, "a guess from its name");
        return {0, from, ""};
    }
    ShapeInfo ShapeOfActor(RE::Actor* a) {   // under g_mx
        if (!a) return {};
        Resolve();
        if (auto it = g_byActor.find(a->GetFormID()); it != g_byActor.end()) return Pick(it->second, "", "set for him");
        if (auto* base = a->GetActorBase()) if (auto it = g_byActor.find(base->GetFormID()); it != g_byActor.end()) return Pick(it->second, "", "set for him");
        if (auto* race = a->GetRace(); race && IsCreature(a)) {   // creatures: by race
            const std::string ed = race->formEditorID.c_str() ? LowerS(race->formEditorID.c_str()) : "";
            const std::string from = std::string("creature: ") + (race->GetName() && *race->GetName() ? race->GetName() : ed);
            for (auto& [k, part] : g_racesRaw) if (!part.empty() && ed.find(LowerS(part)) != std::string::npos) return Pick(k, from, "a compatibility file");
            std::string k = Guess(" " + ed + " ");
            if (k.empty() && HasW(ed, {"werewolf"})) k = "knotted";
            return Pick(k, from, k.empty() ? "" : "a guess from its race");
        }
        Index();
        // a schlong worn as an item (SOS-style) first, then the armatures of his skin (TNG)
        if (auto* worn = a->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kModPelvisSecondary); worn && !worn->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) {
            ShapeInfo r = Classify(worn, nullptr);
            if (r.shape) return r;
        }
        if (auto* skin = a->GetSkin())
            for (auto* arma : skin->armorAddons)
                if (auto it = g_byArma.find(arma); it != g_byArma.end()) return Classify(it->second, arma);
        return {};
    }
    struct ShapeKnown { ShapeInfo s; double at = 0.0; };
    std::unordered_map<RE::FormID, ShapeKnown> g_shapes;   // under g_mx

    std::string FormKey(RE::Actor* a) {   // "Plugin|ID" that survives load order changes ("" for spawned actors)
        if (!a) return {};
        if (a->IsPlayerRef()) return "Player";
        RE::TESForm* f = a;
        if (a->IsDynamicForm()) f = a->GetActorBase();   // a spawned copy: by his base
        const auto* file = f ? f->GetFile(0) : nullptr;
        if (!file || !f || f->IsDynamicForm()) return {};
        return std::format("{}|{:06X}", file->GetFilename(), f->GetLocalFormID());
    }

    void ShapesIni(CSimpleIniA& ini, bool save) {
        std::lock_guard lk(g_mx);
        if (save) {
            ini.Delete("ShapeTypes", nullptr);
            for (auto& d : g_defs)
                if (d.source == "menu")
                    ini.SetValue("ShapeTypes", d.key.c_str(), std::format("{} | {} | {:.2f} | {:.2f} | {:.2f} | {:.2f} | {}", d.name, d.word, d.open, d.build, d.loose, d.tender, d.desc).c_str());
            ini.Delete("ShapeByActor", nullptr);
            for (auto& [k, t] : g_menuActors) ini.SetValue("ShapeByActor", t.c_str(), k.c_str());
            return;
        }
        Builtins();
        LoadShapeFiles();
        CSimpleIniA::TNamesDepend keys;
        ini.GetAllKeys("ShapeTypes", keys);
        for (auto& k : keys) Define(k.pItem, ini.GetValue("ShapeTypes", k.pItem, ""), "menu");
        g_menuActors.clear();
        keys.clear();
        ini.GetAllKeys("ShapeByActor", keys);   // Plugin|ID = shape key
        for (auto& k : keys) g_menuActors.push_back({LowerS(TrimS(ini.GetValue("ShapeByActor", k.pItem, ""))), k.pItem});
        g_resolved = false;
        g_shapes.clear();
        logger::info("Anatomica: {} schlong shapes ({} compatibility file{})", g_defs.size() - 1, g_fileList.size(), g_fileList.size() == 1 ? "" : "s");
    }
    bool Section(const char* title) { return ImGuiMCP::CollapsingHeader(title); }
    void Help(const char* t) {
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::PushTextWrapPos(420.0f); ImGuiMCP::TextUnformatted(t); ImGuiMCP::PopTextWrapPos(); ImGuiMCP::EndTooltip(); }
    }
}

namespace Size {
    void Ini(CSimpleIniA& ini, bool save) {
        const char* S = "PartnerSize";
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto F = [&](const char* k, float& v, float lo, float hi) { if (save) ini.SetDoubleValue(S, k, v); else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi); };
        B("Enabled", g.on); B("Gape", g.gape); B("Looseness", g.loose); B("Soreness", g.tender);
        F("Strength", g.strength, 0.0f, 2.0f); B("Creatures", g.creatures);
        if (save) ini.SetLongValue(S, "CreatureSize", g.creatureCat);
        else g.creatureCat = std::clamp(static_cast<int>(ini.GetLongValue(S, "CreatureSize", g.creatureCat)), 3, 5);
        B("Shape", g.shape); F("ShapeStrength", g.shapeStrength, 0.0f, 2.0f);
        ShapesIni(ini, save);
    }

    bool HasTNG() {
        static int have = -1;
        if (have < 0) have = REX::W32::GetModuleHandleW(L"TheNewGentleman.dll") ? 1 : 0;
        return have == 1;
    }

    int Cat(RE::Actor* p) {
        if (!p) return kUnknown;
        if (IsCreature(p)) return g.creatures ? g.creatureCat : kUnknown;
        if (!HasTNG()) return kUnknown;
        const RE::FormID id = p->GetFormID();
        bool ask = false;
        int cat = kUnknown;
        {
            std::lock_guard lk(g_mx);
            auto& k = g_known[id];
            cat = k.cat;
            if (!k.asking && (k.at <= 0.0 || Now() - k.at > 300.0)) { k.asking = true; ask = true; }
        }
        if (ask) Ask(id);
        return cat;
    }

    int CatOf(RE::FormID id) {
        if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && IsCreature(a)) return g.creatures ? g.creatureCat : kUnknown;
        std::lock_guard lk(g_mx);
        auto it = g_known.find(id);
        return it == g_known.end() ? kUnknown : it->second.cat;
    }

    int Shape(RE::Actor* p) {
        if (!p) return kShapeUnknown;
        const RE::FormID id = p->GetFormID();
        std::lock_guard lk(g_mx);
        if (g_defs.empty()) Builtins();
        if (auto it = g_shapes.find(id); it != g_shapes.end() && Now() - it->second.at < 120.0) return it->second.s.shape;
        const ShapeInfo si = ShapeOfActor(p);
        g_shapes[id] = {si, Now()};
        return si.shape;
    }
    std::string ShapeWord(int s) {
        std::lock_guard lk(g_mx);
        return (s > 0 && s < static_cast<int>(g_defs.size())) ? g_defs[s].word : std::string{};
    }
    std::string Describe(RE::Actor* p) {
        if (!p) return {};
        const int c = Cat(p), sh = Shape(p);
        std::string sz = (c == kCreature) ? std::string("monstrous") : std::string(Word(c));
        const std::string w = ShapeWord(sh);
        if (sz.empty()) return w;
        return w.empty() ? sz : sz + ", " + w;
    }

    const char* Word(int cat) {
        switch (cat) {
            case 0: return "small";
            case 1: return "modest";
            case 3: return "big";
            case 4: return "huge";
            case 5: return "monstrous";
            default: return "";
        }
    }

    K SizeFor(RE::Actor* p) {
        K k;
        if (!g.on) return k;
        const int c = Cat(p);
        if (c < 0 || c > 5) return k;
        if (g.gape) { k.open = Scale(kOpen[c]); k.build = Scale(kBuild[c]); }
        if (g.loose) k.loose = Scale(kLoose[c]);
        if (g.tender) k.tender = Scale(kTender[c]);
        return k;
    }
    K ShapeFor(RE::Actor* p);
    K For(RE::Actor* p) {
        const K a = SizeFor(p), b = ShapeFor(p);
        return {a.open * b.open, a.build * b.build, a.loose * b.loose, a.tender * b.tender};
    }
    K ShapeFor(RE::Actor* p) {
        K k;
        if (!g.on || !g.shape) return k;
        const int s = Shape(p);
        std::lock_guard lk(g_mx);
        if (s <= 0 || s >= static_cast<int>(g_defs.size())) return k;
        const ShapeDef& d = g_defs[s];
        if (g.gape) { k.open = ScaleS(d.open); k.build = ScaleS(d.build); }
        if (g.loose) k.loose = ScaleS(d.loose);
        if (g.tender) k.tender = ScaleS(d.tender);
        return k;
    }

    bool RenderSection() {
        const bool noTNG = !HasTNG();
        if (!Section(noTNG ? "Partner size and shape (The New Gentleman)  (not installed)###tng" : "Partner size and shape (The New Gentleman)###tng")) return false;
        bool ch = false;
        if (noTNG) ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.4f, 1.0f}, "The New Gentleman is not installed - every man counts as average size (greyed out). "
                                                                                     "Creatures, and shapes set in the menu or by compatibility files, still work.");
        ch |= ImGuiMCP::Checkbox("The size of the man taking him matters", &g.on);
        Help("Read from The New Gentleman (each man's size: XS, S, M, L, XL). A bigger man opens him further and faster, "
             "loosens him more and leaves him sorer; a smaller one the reverse. An average (M) man changes nothing.");
        if (g.on) {
            ImGuiMCP::Indent();
            ch |= ImGuiMCP::Checkbox("How far and how fast he opens", &g.gape);
            ch |= ImGuiMCP::Checkbox("Lasting looseness", &g.loose);
            ch |= ImGuiMCP::Checkbox("How sore and red he gets", &g.tender);
            float pct = g.strength * 100.0f;
            ImGuiMCP::BeginDisabled(noTNG);
            if (ImGuiMCP::SliderFloat("How much size matters", &pct, 0.0f, 200.0f, "%.0f%%")) { g.strength = pct / 100.0f; ch = true; }
            ImGuiMCP::EndDisabled();
            Help("100% = an XL man opens him 30% further, loosens him 50% more and makes him 30% sorer; an XS man 25% less "
                 "open, 40% less loosening. 200% doubles those differences.");
            ch |= ImGuiMCP::Checkbox("Creatures count too", &g.creatures);
            Help("Horses, werewolves, trolls, dragons and other creatures (no TNG size of their own).");
            if (g.creatures) {
                ImGuiMCP::Indent();
                static const char* items[] = {"Like an L man", "Like an XL man", "Bigger than any man"};
                int sel = std::clamp(g.creatureCat - 3, 0, 2);
                if (ImGuiMCP::Combo("A creature counts", &sel, items, 3)) { g.creatureCat = sel + 3; ch = true; }
                ImGuiMCP::Unindent();
            }
            ch |= ImGuiMCP::Checkbox("The shape of his schlong matters too", &g.shape);
            Help("Read from the schlong TNG gave him (TNG's own Circumcised / Muscular / Regular, add-ons by their name or "
                 "their mod's compatibility file). Each shape opens, loosens and makes him sore its own way - see Shapes below, "
                 "where every shape can be changed and new ones added.");
            if (g.shape) {
                ImGuiMCP::Indent();
                float sp = g.shapeStrength * 100.0f;
                if (ImGuiMCP::SliderFloat("How much the shape matters", &sp, 0.0f, 200.0f, "%.0f%%")) { g.shapeStrength = sp / 100.0f; ch = true; }
                ImGuiMCP::Unindent();
            }
            // ---- the selected man: what was found, and a shape of your own choosing for him
            static std::string shown;
            static RE::FormID shownId = 0;
            RE::Actor* sel = nullptr;
            if (auto r = RE::Console::GetSelectedRef()) sel = r->As<RE::Actor>();
            if (!sel) sel = RE::PlayerCharacter::GetSingleton();
            if (ImGuiMCP::Button("Size and shape of the selected man") || (shownId && shownId != sel->GetFormID())) {
                const int c = Cat(sel);
                ShapeInfo si;
                std::string nmShape;
                {
                    std::lock_guard lk(g_mx);
                    g_shapes.erase(sel->GetFormID());
                    si = ShapeOfActor(sel);
                    g_shapes[sel->GetFormID()] = {si, Now()};
                    nmShape = si.shape ? g_defs[si.shape].name : std::string("shape not known");
                }
                const char* nm = sel->GetDisplayFullName();
                shown = std::format("{}: {}, {}{}{}", nm ? nm : "?", c < 0 ? std::string("size not known yet (asked TNG - press again)") : std::string(kName[c]), nmShape,
                                    si.from.empty() ? std::string{} : std::format(" - \"{}\"", si.from), si.how.empty() ? std::string{} : std::format(" ({})", si.how));
                shownId = sel->GetFormID();
            }
            Help("The man you click in the console (~), or you. Shows which schlong was found and how its shape was decided.");
            if (!shown.empty()) ImGuiMCP::TextWrapped("%s", shown.c_str());
            {
                std::lock_guard lk(g_mx);
                const std::string fk = FormKey(sel);
                std::string cur;
                for (auto& [k, t] : g_menuActors) if (t == fk) cur = k;
                const int ci = cur.empty() ? 0 : DefIndex(cur);
                if (!fk.empty() && ImGuiMCP::BeginCombo("His schlong is", ci ? g_defs[ci].name.c_str() : "Found automatically")) {
                    auto set = [&](const std::string& key) {
                        std::erase_if(g_menuActors, [&](auto& p) { return p.second == fk; });
                        if (!key.empty()) g_menuActors.push_back({key, fk});
                        g_resolved = false; g_shapes.clear(); shownId = 0; ch = true;
                    };
                    if (ImGuiMCP::Selectable("Found automatically", ci == 0)) set("");
                    for (size_t i = 1; i < g_defs.size(); ++i)
                        if (ImGuiMCP::Selectable(g_defs[i].name.c_str(), static_cast<int>(i) == ci)) set(g_defs[i].key);
                    ImGuiMCP::EndCombo();
                }
                if (fk.empty()) ImGuiMCP::TextDisabled("(a spawned actor without a base of his own: his shape can't be set here)");
            }
            Help("Your own choice for this man (or yourself) - a unique custom build, a follower... It wins over everything else "
                 "and is kept in AnatomicaMenu.ini.");
            // ---- the shapes themselves
            if (ImGuiMCP::TreeNode("Shapes (change them, or add your own)")) {
                std::lock_guard lk(g_mx);
                ImGuiMCP::TextDisabled("x1.00 = no change. Open: how far he gapes. Build: time to open up (lower = faster). "
                                       "Loose: lasting looseness. Sore: soreness and redness.");
                if (!g_fileList.empty()) {
                    std::string fl;
                    for (auto& f : g_fileList) fl += (fl.empty() ? "" : ", ") + f;
                    ImGuiMCP::TextDisabled("Compatibility files: %s", fl.c_str());
                }
                int remove = 0;
                for (size_t i = 1; i < g_defs.size(); ++i) {
                    ShapeDef& d = g_defs[i];
                    ImGuiMCP::PushID(static_cast<int>(i));
                    if (ImGuiMCP::TreeNode("shape", "%s  (%s)", d.name.c_str(), d.source.c_str())) {
                        bool e = false;
                        char nb[64]; std::snprintf(nb, sizeof(nb), "%s", d.name.c_str());
                        if (ImGuiMCP::InputText("Name", nb, sizeof(nb))) { d.name = nb; e = true; }
                        char wb[48]; std::snprintf(wb, sizeof(wb), "%s", d.word.c_str());
                        if (ImGuiMCP::InputText("Word (SkyrimNet: \"his huge ___ cock\")", wb, sizeof(wb))) { d.word = wb; e = true; }
                        e |= ImGuiMCP::SliderFloat("Open", &d.open, 0.5f, 2.0f, "x%.2f");
                        e |= ImGuiMCP::SliderFloat("Build", &d.build, 0.5f, 2.0f, "x%.2f");
                        e |= ImGuiMCP::SliderFloat("Loose", &d.loose, 0.5f, 2.0f, "x%.2f");
                        e |= ImGuiMCP::SliderFloat("Sore", &d.tender, 0.5f, 2.0f, "x%.2f");
                        if (!d.desc.empty()) ImGuiMCP::TextDisabled("%s", d.desc.c_str());
                        if (d.source == "menu" && ImGuiMCP::SmallButton("Remove this shape")) remove = static_cast<int>(i);
                        if (d.source == "menu") Help("Back to how the built-in shape or a compatibility file defines it (if one does).");
                        if (e) { d.source = "menu"; ch = true; }
                        ImGuiMCP::TreePop();
                    }
                    ImGuiMCP::PopID();
                }
                if (remove) {   // drop the menu's version: back to the built-in / compatibility file one (if any)
                    std::vector<ShapeDef> keep;
                    for (size_t i = 1; i < g_defs.size(); ++i) if (g_defs[i].source == "menu" && static_cast<int>(i) != remove) keep.push_back(g_defs[i]);
                    Builtins();
                    LoadShapeFiles();
                    for (auto& d : keep) {
                        int i = DefIndex(d.key);
                        if (!i) { g_defs.push_back(d); } else g_defs[i] = d;
                    }
                    g_resolved = false; g_shapes.clear(); ch = true;
                }
                static char nk[32] = "";
                ImGuiMCP::InputText("##newshape", nk, sizeof(nk));
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Add a new shape") && nk[0]) {
                    std::string key = LowerS(TrimS(nk));
                    for (auto& c : key) if (c == ' ' || c == '|' || c == '=') c = '_';
                    if (!DefIndex(key)) { g_defs.push_back({key, TrimS(nk), LowerS(TrimS(nk)), "", "menu"}); ch = true; }
                    nk[0] = 0;
                }
                Help("Type a name and add it; then set it on a man above (\"His schlong is\"), or map schlongs to it in a "
                     "compatibility file.");
                ImGuiMCP::TreePop();
            }
            ImGuiMCP::Unindent();
        }
        return ch;
    }
}

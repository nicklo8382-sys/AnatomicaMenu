#include "Align.h"
#include "Lang.h"
#include "Engine.h"
#include "Size.h"
#include "Colour.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <mutex>
#include <cstdio>
#include <filesystem>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

// See Align.h.
namespace {
    struct Settings {
        bool move = true;          // move a male / futa bottom so his anus is where the animation aims
        float strength = 1.0f;     // of the vagina -> anus gap (0.5 - 1.5)
        float speed = 8.0f;        // units a second it eases in / out
        int futa = 0;              // 0 follow PPA (FutasUseAnus), 1 always, 2 never
        int bottom = 0;            // his own schlong in PPA while he is taken: 0 automatic, 1 always on, 2 always off
        std::set<std::string> letWork;   // OStim scenes where the bottom's schlong always works
    } g;
    std::mutex g_mx;   // g, the states (menu thread vs main thread)

    constexpr const char* kMorph = "Anatomica PPA Bottom";
    constexpr const char* kKey = "Anatomica";
    constexpr const char* kPpaMain = "Data/SKSE/Plugins/accurate-penetration.toml";

    // where PPA puts the holes (its pelvis-bone offsets): the vagina (PPA's own), the anus (Anatomica's for men, PPA's for women)
    const RE::NiPoint3 kVagina{0.0736564f, -1.36951f, -3.33551f};
    const RE::NiPoint3 kAnusMan{-0.0012f, -4.5148f, -5.9643f};
    const RE::NiPoint3 kAnusWoman{0.0f, -4.95285f, -1.17131f};

    // OStim's action factions (OStim.esp): who is doing what right now
    RE::TESFaction* g_vagT = nullptr;      // target of vaginal sex / vaginal tail sex
    RE::TESFaction* g_vagTailT = nullptr;
    std::vector<RE::TESFaction*> g_takenF;  // target of anal / vaginal (tail) sex: he is the bottom
    std::vector<RE::TESFaction*> g_usedF;   // his schlong has a job: he penetrates, he is sucked / stroked / ridden...
    bool g_facTried = false, g_facOk = false;
    int g_futasUseAnus = -1;                // PPA's FutasUseAnus: -1 not read, 0 no, 1 yes

    void Factions() {
        if (g_facTried) return;
        g_facTried = true;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !dh->LookupModByName("OStim.esp")) return;
        auto F = [&](RE::FormID id) { return dh->LookupForm<RE::TESFaction>(id, "OStim.esp"); };
        g_vagT = F(0xE91);
        g_vagTailT = F(0xF87);
        for (RE::FormID id : {0xE50u, 0xE91u, 0xF83u, 0xF87u}) if (auto* f = F(id)) g_takenF.push_back(f);
        // actor of anal / vaginal sex, facial / rubbing on the face; target of blowjob (and deepthroat), boobjob, buttjob, footjob,
        // grinding (penis), handjob, penile licking, tailjob, thighjob
        for (RE::FormID id : {0xE4Fu, 0xE90u, 0xE84u, 0xE53u, 0xE56u, 0xE59u, 0xE60u, 0xE65u, 0xE71u, 0xE7Au, 0xF7Fu, 0xE89u})
            if (auto* f = F(id)) g_usedF.push_back(f);
        g_facOk = g_vagT && !g_takenF.empty();
        logger::info("Anatomica: bottoms - OStim action factions {}", g_facOk ? "found" : "not found (needs OStim Standalone)");
    }
    bool FutasUseAnus() {
        if (g_futasUseAnus < 0) {
            g_futasUseAnus = 0;
            std::ifstream in(kPpaMain);
            std::string line;
            while (std::getline(in, line)) {
                auto p = line.find("FutasUseAnus");
                if (p == std::string::npos || line.find('#') < p) continue;
                g_futasUseAnus = line.find("true", p) != std::string::npos ? 1 : 0;
                break;
            }
        }
        return g_futasUseAnus == 1;
    }
    bool In(RE::Actor* a, RE::TESFaction* f) { return f && a->IsInFaction(f); }
    bool InAny(RE::Actor* a, const std::vector<RE::TESFaction*>& fs) { for (auto* f : fs) if (a->IsInFaction(f)) return true; return false; }

    struct St {
        RE::NiPoint3 cur{}, tgt{};       // the offset now (eased) and where it is heading (world units)
        RE::NiPoint3 applied{};          // the part of cur already in his position
        RE::NiPoint3 expect{};           // where we last put him
        bool haveExpect = false;
        int resets = 0;                  // times something else put him back (OStim aligning)
        int loggedResets = 0;
        bool moving = false;             // logged: the move is on
        int lastFac = -1;                // logged: which action factions he was in
        bool morph = false;              // his schlong is switched off in PPA
        std::string scene;               // his OStim scene ("" = not known)
        int vagScene = -1;               // 2.5y: the scene, as made, has him as the target of vaginal sex (OStim turns it into
                                         // anal for a man, so its factions don't say so): -1 not known, 0 no, 1 yes
        std::string vagSceneOf;          // the scene that answer is for
        double asked = -10.0;
        int tid = -1, pos = -1;          // 2.5z: his OStim thread and position in it
        double vagSince = -1.0;          // when the vaginal-scene move first wanted to start (pose settles first)
        std::string alignDone;           // the scene his OStim alignment was already moved for
    };
    std::unordered_map<RE::FormID, St> g_st;
    std::string g_lastScene;             // the last bottom's scene (the "let it work here" button)
    std::string g_lastWho;

    // his OStim scene, asked in the background: OActor.GetThreadID -> OThread.GetScene
    // his OStim scene, asked in the background: OActor.GetThreadID -> OThread.GetScene, OThread.GetActorPosition ->
    // OMetadata.FindActionForTarget(scene, position, "vaginalsex"): was the animation made for a vagina where he is?
    struct VagCb : RE::BSScript::IStackCallbackFunctor {
        RE::FormID id = 0;
        std::string scene;
        void operator()(RE::BSScript::Variable r) override {
            std::lock_guard lk(g_mx);
            if (auto it = g_st.find(id); it != g_st.end()) {
                it->second.vagScene = (r.IsInt() && r.GetSInt() >= 0) ? 1 : 0;
                it->second.vagSceneOf = scene;
                logger::info("Anatomica bottoms: scene {} has vaginal sex on his position: {}", scene, it->second.vagScene ? "yes" : "no");
            }
        }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
    };
    struct PosCb : RE::BSScript::IStackCallbackFunctor {
        RE::FormID id = 0;
        std::string scene;
        void operator()(RE::BSScript::Variable r) override {
            const int pos = r.IsInt() ? r.GetSInt() : -1;
            {
                std::lock_guard lk(g_mx);
                if (auto it = g_st.find(id); it != g_st.end()) it->second.pos = pos;
            }
            if (pos < 0 || scene.empty()) return;
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) return;
            auto* cb = new VagCb;
            cb->id = id; cb->scene = scene;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> p(cb);
            vm->DispatchStaticCall("OMetadata", "FindActionForTarget", RE::MakeFunctionArguments(RE::BSFixedString(scene), static_cast<std::int32_t>(pos), RE::BSFixedString("vaginalsex")), p);
        }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
    };
    struct SceneCb : RE::BSScript::IStackCallbackFunctor {
        RE::FormID id = 0;
        int tid = -1;
        void operator()(RE::BSScript::Variable r) override {
            const std::string sc = r.IsString() ? std::string(r.GetString()) : std::string();
            bool ask = false;
            {
                std::lock_guard lk(g_mx);
                if (auto it = g_st.find(id); it != g_st.end()) { it->second.scene = sc; ask = !sc.empty() && it->second.vagSceneOf != sc; }
            }
            if (!ask) return;
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!vm || !a) return;
            auto* cb = new PosCb;
            cb->id = id; cb->scene = sc;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> p(cb);
            vm->DispatchStaticCall("OThread", "GetActorPosition", RE::MakeFunctionArguments(static_cast<std::int32_t>(tid), static_cast<RE::Actor*>(a)), p);
        }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
    };
    struct ThreadCb : RE::BSScript::IStackCallbackFunctor {
        RE::FormID id = 0;
        void operator()(RE::BSScript::Variable r) override {
            const int tid = r.IsInt() ? r.GetSInt() : -1;
            {
                std::lock_guard lk(g_mx);
                if (auto it = g_st.find(id); it != g_st.end()) it->second.tid = tid;
            }
            if (tid < 0) return;
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) return;
            auto* cb = new SceneCb;
            cb->id = id; cb->tid = tid;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> p(cb);
            vm->DispatchStaticCall("OThread", "GetScene", RE::MakeFunctionArguments(static_cast<std::int32_t>(tid)), p);
        }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
    };
    void AskScene(RE::Actor* a) {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) return;
        auto* cb = new ThreadCb;
        cb->id = a->GetFormID();
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> p(cb);
        vm->DispatchStaticCall("OActor", "GetThreadID", RE::MakeFunctionArguments(static_cast<RE::Actor*>(a)), p);
    }

    RE::NiPoint3 Sub(const RE::NiPoint3& a, const RE::NiPoint3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    RE::NiPoint3 Add(const RE::NiPoint3& a, const RE::NiPoint3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    RE::NiPoint3 Mul(const RE::NiPoint3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    float Length(const RE::NiPoint3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
    RE::NiPoint3 Rot(const RE::NiMatrix3& m, const RE::NiPoint3& v) {   // NiMatrix3 rows times v
        return {m.entry[0][0] * v.x + m.entry[0][1] * v.y + m.entry[0][2] * v.z,
                m.entry[1][0] * v.x + m.entry[1][1] * v.y + m.entry[1][2] * v.z,
                m.entry[2][0] * v.x + m.entry[2][1] * v.y + m.entry[2][2] * v.z};
    }
    // the move that puts his anus where a vagina would be (world units), from his pelvis as it is now
    bool Gap(RE::Actor* a, bool man, float strength, RE::NiPoint3& out) {
        auto* root = a->Get3D(false);
        auto* pel = root ? root->GetObjectByName(RE::BSFixedString("NPC Pelvis [Pelv]")) : nullptr;
        if (!pel) return false;
        const RE::NiPoint3 d = Sub(kVagina, man ? kAnusMan : kAnusWoman);
        out = Rot(pel->world.rotate, Mul(d, pel->world.scale * strength));
        return true;
    }
}

namespace {
    // ---- 2.5z: OStim's own alignment (its C++ thread API, OStim.dll "RequestPluginAPI_Thread"). OStim puts every actor
    // back on its spot each frame, so moving him ourselves only flickers; his alignment offset in the scene is what lasts.
    // Only the functions used are named; the order is OStim's interface order (IThreadInterface, interface version 1).
    struct OAlign { float x, y, z, scale, rotation, sosBend; };
    struct IOStimThread {
        virtual void f00() noexcept = 0; virtual void f01() noexcept = 0; virtual void f02() noexcept = 0; virtual void f03() noexcept = 0;
        virtual void f04() noexcept = 0; virtual void f05() noexcept = 0; virtual void f06() noexcept = 0; virtual void f07() noexcept = 0;
        virtual void f08() noexcept = 0; virtual void f09() noexcept = 0; virtual void f10() noexcept = 0; virtual void f11() noexcept = 0;
        virtual void f12() noexcept = 0; virtual void f13() noexcept = 0; virtual void f14() noexcept = 0; virtual void f15() noexcept = 0;
        virtual void f16() noexcept = 0; virtual void f17() noexcept = 0; virtual void f18() noexcept = 0; virtual void f19() noexcept = 0;
        virtual void f20() noexcept = 0; virtual void f21() noexcept = 0;
        virtual bool GetActorAlignment(std::uint32_t threadID, std::uint32_t actorIndex, OAlign* out) noexcept = 0;   // slot 22
        virtual std::uint8_t SetActorAlignment(std::uint32_t threadID, std::uint32_t actorIndex, const OAlign* data) noexcept = 0;   // slot 23
    };
    IOStimThread* g_oapi = nullptr;
    bool g_oapiTried = false;
    IOStimThread* OApi() {
        if (g_oapiTried) return g_oapi;
        g_oapiTried = true;
        auto* mod = REX::W32::GetModuleHandleA("OStim.dll");
        if (!mod) return nullptr;
        using Req = IOStimThread* (*)(std::uint8_t, const char*, REL::Version);
        auto req = reinterpret_cast<Req>(REX::W32::GetProcAddress(mod, "RequestPluginAPI_Thread"));
        if (req) g_oapi = req(0, "AnatomicaMenu", REL::Version(2, 5, 0, 0));
        logger::info("Anatomica bottoms: OStim alignment API {}", g_oapi ? "found" : "not found - his move can't last (OStim puts him back)");
        return g_oapi;
    }
    // what we set, per scene and position, so a scene OStim kept our alignment for isn't moved twice
    const char* kAlignFile = "Data/SKSE/Plugins/AnatomicaMenu/OStimAlign.ini";
    std::map<std::string, std::vector<std::array<float, 3>>> g_alignSet;
    bool g_alignLoaded = false;
    void AlignLoad() {
        if (g_alignLoaded) return;
        g_alignLoaded = true;
        std::ifstream in(kAlignFile);
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find(" = ");
            if (line.empty() || line[0] == ';' || eq == std::string::npos) continue;
            std::array<float, 3> v{};
            if (std::sscanf(line.c_str() + eq + 3, "%f,%f,%f", &v[0], &v[1], &v[2]) == 3) g_alignSet[line.substr(0, eq)].push_back(v);
        }
    }
    void AlignSave() {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(kAlignFile).parent_path(), ec);
        std::ofstream o(kAlignFile, std::ios::trunc);
        o << "; Anatomica: OStim alignments it moved for a man / futa where the scene expects a woman (scene#position = x,y,z)\n"
             "; Delete this file to let Anatomica move them again; undo a move with OStim's Align menu.\n";
        for (auto& [k, vs] : g_alignSet)
            for (auto& v : vs) o << k << " = " << v[0] << "," << v[1] << "," << v[2] << "\n";
    }
}

namespace Align {
    bool SchlongOff(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_st.find(id);
        return it != g_st.end() && it->second.morph;
    }
    void Tick(const std::vector<RE::Actor*>& all, float dt, double now) {
        Factions();
        if (!g_facOk) return;
        std::lock_guard lk(g_mx);
        std::set<RE::FormID> seen;
        for (auto* a : all) {
            if (!a || a->IsDead() || !a->Is3DLoaded()) continue;
            const bool taken = InAny(a, g_takenF);
            auto it = g_st.find(a->GetFormID());
            if (!taken && it == g_st.end()) continue;   // nothing to do and nothing to undo
            const auto* npc = a->GetActorBase();
            const bool man = npc && npc->GetSex() == RE::SEX::kMale;
            const bool schlong = man || (taken && Size::HasSchlong(a));
            St& s = it != g_st.end() ? it->second : g_st[a->GetFormID()];
            seen.insert(a->GetFormID());

            // his scene (for the "let it work here" list)
            if (taken && now - s.asked > 1.5) { s.asked = now; AskScene(a); }
            if (taken && schlong) {
                g_lastScene = s.scene;
                const char* nm = a->GetDisplayFullName();
                g_lastWho = nm ? nm : "";
            }

            // 1. the move: a man / futa as the target of vaginal sex
            // OStim's own factions, or (2.5y) the scene as made: a man put where a woman would be taken vaginally - OStim
            // then calls it anal, but the animation still aims at where a vagina would be
            const bool vag = In(a, g_vagT) || In(a, g_vagTailT) || (taken && s.vagScene == 1 && s.vagSceneOf == s.scene && !s.scene.empty());
            const bool futaOk = !man && schlong && (g.futa == 1 || (g.futa == 0 && FutasUseAnus()));
            RE::NiPoint3 want{};
            if (g.move && vag && (man || futaOk)) {
                if (!Gap(a, man, g.strength, want)) want = {};
            }
            {   // 2.5x: what OStim says he is doing, and the move, in the log
                const int fac = (vag ? 1 : 0) | (taken ? 2 : 0) | (InAny(a, g_usedF) ? 4 : 0) | (schlong ? 8 : 0) | (s.vagScene == 1 ? 16 : 0);
                const char* nm = a->GetDisplayFullName();
                if (fac != s.lastFac) {
                    s.lastFac = fac;
                    logger::info("Anatomica bottoms: {} - vaginal target {} (scene {} made vaginal for him: {}), taken {}, his schlong has a job {}, has a schlong {} ({})",
                                 nm ? nm : "?", vag, s.scene.empty() ? "?" : s.scene, s.vagScene < 0 ? "?" : s.vagScene ? "yes" : "no",
                                 taken, (fac & 4) != 0, schlong, man ? "man" : "woman");
                }
                const bool mv = Length(want) > 0.01f;
                if (mv != s.moving) {
                    s.moving = mv;
                    logger::info("Anatomica bottoms: {} {}", nm ? nm : "?",
                                 mv ? std::format("moved {:.1f} units ({:.1f}, {:.1f}, {:.1f}) so his anus is where the animation aims", Length(want), want.x, want.y, want.z)
                                    : std::string("move eased back"));
                }
                if (s.resets >= s.loggedResets + 10) {
                    s.loggedResets = s.resets;
                    logger::info("Anatomica bottoms: {} - something put him back {} times (OStim lining up?)", nm ? nm : "?", s.resets);
                }
            }
            // follow the pelvis gently (it turns with every thrust), then ease the offset at most speed units a second
            const float k = std::min(1.0f, dt * 2.0f);
            s.tgt = Add(s.tgt, Mul(Sub(want, s.tgt), k));
            RE::NiPoint3 step = Sub(s.tgt, s.cur);
            const float len = Length(step), maxStep = g.speed * dt;
            if (len > maxStep && len > 1e-4f) step = Mul(step, maxStep / len);
            s.cur = Add(s.cur, step);
            if (Length(want) < 1e-3f && Length(s.cur) < 0.02f) s.cur = {};
            // 2.5z: OStim's alignment for his place in the scene, once per scene (OStim keeps it, and puts him there
            // every frame); without OStim's API, his position (OStim puts him back, so it only flickers)
            if (Length(want) > 0.01f) { if (s.vagSince < 0.0) s.vagSince = now; } else s.vagSince = -1.0;
            if (auto* api = OApi(); api && s.tid >= 0 && s.pos >= 0) {
                if (Length(want) > 0.01f && now - s.vagSince > 0.8 && s.alignDone != s.scene && !s.scene.empty()) {
                    s.alignDone = s.scene;
                    AlignLoad();
                    OAlign cur{};
                    if (api->GetActorAlignment(static_cast<std::uint32_t>(s.tid), static_cast<std::uint32_t>(s.pos), &cur)) {
                        const std::string key = s.scene + "#" + std::to_string(s.pos);
                        bool already = false;
                        for (auto& v : g_alignSet[key])
                            if (std::abs(v[0] - cur.x) < 0.05f && std::abs(v[1] - cur.y) < 0.05f && std::abs(v[2] - cur.z) < 0.05f) already = true;
                        const char* nm = a->GetDisplayFullName();
                        if (already) logger::info("Anatomica bottoms: {} - OStim already has him moved in {}", nm ? nm : "?", s.scene);
                        else {
                            // world -> the scene's frame: OStim puts x / y at centre + (cos*x + sin*y, -sin*x + cos*y), centre.r =
                            // his heading less his own alignment rotation
                            const float r = a->GetAngleZ() - cur.rotation * 0.01745329f;
                            const float c = std::cos(r), sn = std::sin(r);
                            OAlign nw = cur;
                            nw.x = cur.x + c * want.x - sn * want.y;
                            nw.y = cur.y + sn * want.x + c * want.y;
                            nw.z = cur.z + want.z;
                            api->SetActorAlignment(static_cast<std::uint32_t>(s.tid), static_cast<std::uint32_t>(s.pos), &nw);
                            g_alignSet[key].push_back({nw.x, nw.y, nw.z});
                            AlignSave();
                            logger::info("Anatomica bottoms: {} - OStim alignment in {} (position {}) moved by {:.1f} units: {:.1f},{:.1f},{:.1f} -> {:.1f},{:.1f},{:.1f}",
                                         nm ? nm : "?", s.scene, s.pos, Length(want), cur.x, cur.y, cur.z, nw.x, nw.y, nw.z);
                        }
                    }
                }
            } else {
                RE::NiPoint3 pos = a->GetPosition();
                if (s.haveExpect && Length(Sub(pos, s.expect)) > 0.25f) { s.applied = {}; if (Length(s.cur) > 0.05f) ++s.resets; }
                const RE::NiPoint3 d = Sub(s.cur, s.applied);
                if (Length(d) > 0.01f) {
                    pos = Add(pos, d);
                    a->SetPosition(pos, true);
                    s.applied = s.cur;
                }
                s.expect = pos;
                s.haveExpect = Length(s.cur) > 0.0f || Length(s.applied) > 0.0f;
            }

            // 2. his own schlong in PPA
            bool off = false;
            if (taken && schlong) {
                if (g.bottom == 2) off = true;
                else if (g.bottom == 0) off = !InAny(a, g_usedF) && !(s.scene.size() && g.letWork.count(s.scene));
            }
            if (off != s.morph) {
                Engine::BodyMorph(a, kMorph, kKey, off ? 1.0f : 0.0f);
                s.morph = off;
                logger::info("Anatomica: {}'s schlong {} in PPA (scene {})", a->GetDisplayFullName() ? a->GetDisplayFullName() : "?",
                             off ? "off" : "on again", s.scene.empty() ? "?" : s.scene);
            }
        }
        // gone (unloaded, out of range): their morph back to 0, forget them
        for (auto it = g_st.begin(); it != g_st.end();) {
            if (seen.count(it->first)) { ++it; continue; }
            const bool active = Length(it->second.cur) > 0.0f || it->second.morph;
            if (active) {
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(it->first)) {
                    if (it->second.morph) Engine::BodyMorph(a, kMorph, kKey, 0.0f);
                    // a scene that ended: OStim has put him somewhere new already; nothing to move back
                }
            }
            it = g_st.erase(it);
        }
    }

    bool RenderSection() {
        if (!Tr::CollapsingHeader("Men and futas as the bottom###bottoms")) return false;
        bool ch = false;
        std::lock_guard lk(g_mx);
        Factions();
        if (!g_facOk) Tr::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.4f, 1.0f}, "Needs OStim Standalone (it says who does what in a scene).");
        Tr::TextWrapped("Animations made for a woman aim the top's schlong at a vagina. A man has none, so it ends up between his "
                        "thighs. These fix that, and keep the bottom's own schlong out of the way.");
        ch |= Tr::Checkbox("Move him so the animation finds his anus", &g.move);
        auto help = [](const char* t) {
            ImGuiMCP::SameLine();
            Tr::TextDisabled("(?)");
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::BeginTooltip();
                ImGuiMCP::PushTextWrapPos(ImGuiMCP::GetFontSize() * 30.0f);
                Tr::TextUnformatted(t);
                ImGuiMCP::PopTextWrapPos();
                ImGuiMCP::EndTooltip();
            }
        };
        help("While OStim has him as the one taken in vaginal sex, his body is moved by the gap between where a vagina would "
             "be and his anus (a few units, eased in and out), so his anus is where the animation aims and PPA takes him "
             "anally.");
        if (g.move) {
            ImGuiMCP::Indent();
            float pct = g.strength * 100.0f;
            if (Tr::SliderFloat("How far", &pct, 50.0f, 150.0f, "%.0f%%")) { g.strength = pct / 100.0f; ch = true; }
            help("100% = exactly the gap between the two. More if the schlong still lands a little in front of him, less if "
                 "it goes past.");
            ch |= Tr::SliderFloat("How fast it eases in", &g.speed, 2.0f, 30.0f, "%.0f units/s");
            static const char* fu[] = {"Like PPA (when it sends futas to the anus)", "Always", "Never"};
            ch |= Tr::Combo("Futas", &g.futa, fu, 3);
            help("A futa has a vagina, so only move her when PPA uses her anus (PPA's \"Futas use anus\").");
            ImGuiMCP::Unindent();
        }
        static const char* bo[] = {"Off while he is taken, unless the scene uses it", "Always on (as before)", "Always off while he is taken"};
        ch |= Tr::Combo("The bottom's schlong in PPA", &g.bottom, bo, 3);
        help("With a wider PPA angle the bottom's own schlong can catch on the top and open him. Off while he is taken: PPA "
             "leaves it alone - unless the scene gives it something to do (he penetrates someone, he is sucked or stroked), "
             "or the scene is on the list below. Needs anatomica_bottom.toml (PPA integration option); F5 reloads PPA.");
        if (g.bottom == 0) {
            ImGuiMCP::Indent();
            if (!g_lastScene.empty()) {
                const bool listed = g.letWork.count(g_lastScene) > 0;
                Tr::Text("Current scene: %s (%s)", g_lastScene.c_str(), g_lastWho.c_str());
                if (Tr::Button(listed ? "Stop letting his schlong work in this scene" : "Let his schlong work in this scene")) {
                    if (listed) g.letWork.erase(g_lastScene); else g.letWork.insert(g_lastScene);
                    ch = true;
                }
            } else Tr::TextDisabled("Start a scene with a man as the bottom to pick it here.");
            if (!g.letWork.empty()) {
                Tr::TextDisabled("%d scenes on the list.", static_cast<int>(g.letWork.size()));
                ImGuiMCP::SameLine();
                if (Tr::SmallButton("Clear the list")) { g.letWork.clear(); ch = true; }
            }
            ImGuiMCP::Unindent();
        }
        // what it is doing now
        for (auto& [id, s] : g_st) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!a) continue;
            const char* nm = a->GetDisplayFullName();
            if (Length(s.cur) > 0.01f || s.morph)
                Tr::TextDisabled("%s: moved %.1f units%s%s", nm ? nm : "?", Length(s.cur), s.morph ? ", schlong off in PPA" : "",
                                 s.resets > 20 ? " - OStim keeps putting him back" : "");
        }
        if (ch) {
            g_mx.unlock();
            Colour::SaveNow();
            g_mx.lock();
        }
        return ch;
    }

    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "BottomAlign";
        std::lock_guard lk(g_mx);
        if (save) {
            ini.SetBoolValue(S, "Move", g.move);
            ini.SetDoubleValue(S, "Strength", g.strength);
            ini.SetDoubleValue(S, "Speed", g.speed);
            ini.SetLongValue(S, "Futa", g.futa);
            ini.SetLongValue(S, "BottomSchlong", g.bottom);
            std::string l;
            for (auto& sc : g.letWork) l += (l.empty() ? "" : ",") + sc;
            ini.SetValue(S, "LetWorkScenes", l.c_str());
        } else {
            g.move = ini.GetBoolValue(S, "Move", g.move);
            g.strength = std::clamp(static_cast<float>(ini.GetDoubleValue(S, "Strength", g.strength)), 0.5f, 1.5f);
            g.speed = std::clamp(static_cast<float>(ini.GetDoubleValue(S, "Speed", g.speed)), 2.0f, 30.0f);
            g.futa = std::clamp(static_cast<int>(ini.GetLongValue(S, "Futa", g.futa)), 0, 2);
            g.bottom = std::clamp(static_cast<int>(ini.GetLongValue(S, "BottomSchlong", g.bottom)), 0, 2);
            g.letWork.clear();
            std::stringstream ss(ini.GetValue(S, "LetWorkScenes", ""));
            for (std::string t; std::getline(ss, t, ',');) if (!t.empty()) g.letWork.insert(t);
        }
    }
}

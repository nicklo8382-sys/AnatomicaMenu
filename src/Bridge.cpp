#include "Bridge.h"
#include "Colour.h"
#include "Engine.h"
#include "Virgin.h"
#include "Size.h"
#include "Extras.h"
#include "Wink.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// See Bridge.h. Everything another mod is asked to do goes through the Papyrus VM (their APIs are Papyrus), queued,
// never waited on. Nothing here holds a lock while calling into Colour / Virgin / Engine.

namespace {
    struct Settings {
        // SkyrimNet
        bool sn = true;
        bool snStatus = true;      // decorators + the prompt pack
        bool snMemory = true;      // persistent memories: virginity taken / come back
        bool snReact = true;       // the partner says something right after his first time
        bool actInspect = true, actLube = true, actClench = true, actAsk = true;
        bool snScene = true;       // short-lived "what you can see" notes while he is naked and it shows
        bool snRemarks = true;     // someone nearby comments on it now and then
        float remarkMinutes = 5.0f;
        float remarkChance = 0.35f;
        // SLSF Reloaded
        bool fame = true;
        int fameLo = 2, fameHi = 10;   // per time he is taken (the player only: SLSF fame is his)
        float fameFirst = 3.0f;        // his first time counts this many times over
        bool fameGroup = true;         // several men in a row: Group fame too
        // arousal (SexLab Aroused / OSL Aroused / SLO Aroused NG)
        bool arousal = true;
        int arousalFrom = 60;          // arousal from which he starts to get wet
        float arousalWet = 0.35f;      // how wet at 100
        bool arousalSens = true;       // and twitchier (with "Sensitive while tender", After sex page)
        int exposureFirst = 15;        // his first time arouses him this much
        int exposureEach = 0;          // every time he is taken
        // Bathing in Skyrim
        bool bath = true;
        bool bathDry = true;
        float bathSoothe = 0.5f;
        float bathHours = 2.0f;
    } g;

    double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    // what SkyrimNet has done with us this session (Integrations page)
    std::atomic<int> g_decOk{0}, g_actOk{0}, g_decAsk{0}, g_actAsk{0}, g_actRun{0}, g_sentMem{0}, g_sentReact{0}, g_sentScene{0}, g_sentRemark{0};
    std::atomic<int> g_callsSelf{0}, g_callsSeen{0}, g_callsKnows{0};
    double g_regAt = 0.0;
    std::mt19937 g_rng{std::random_device{}()};
    float Rand01() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng); }

    // ---- who is here
    bool HasSkyrimNet() {
        static int have = -1;
        if (have < 0) have = REX::W32::GetModuleHandleW(L"SkyrimNet.dll") ? 1 : 0;
        return have == 1;
    }
    bool HasPlugin(const char* name) {
        auto* dh = RE::TESDataHandler::GetSingleton();
        return dh && (dh->LookupLoadedModByName(name) || dh->LookupLoadedLightModByName(name));   // ESL-flagged ones too
    }
    bool HasSLSF() { static int h = -1; if (h < 0) h = HasPlugin("SLSF Reloaded.esp") ? 1 : 0; return h == 1; }
    RE::TESFaction* ArousalFaction() {
        static bool tried = false; static RE::TESFaction* f = nullptr;
        if (!tried) { tried = true; if (auto* dh = RE::TESDataHandler::GetSingleton()) f = dh->LookupForm<RE::TESFaction>(0x03FC36, "SexLabAroused.esm"); }
        return f;
    }
    RE::TESQuest* ArousalQuest() {
        static bool tried = false; static RE::TESQuest* q = nullptr;
        if (!tried) { tried = true; if (auto* dh = RE::TESDataHandler::GetSingleton()) q = dh->LookupForm<RE::TESQuest>(0x04290F, "SexLabAroused.esm"); }
        return q;
    }
    bool SN() { return g.sn && HasSkyrimNet(); }

    std::string Name(RE::Actor* a) {
        if (!a) return "someone";
        const char* n = a->GetDisplayFullName();
        return n && *n ? n : "someone";
    }
    std::string NameOf(RE::FormID id) { return Name(RE::TESForm::LookupByID<RE::Actor>(id)); }
    bool IsMan(RE::Actor* a) { const auto* npc = a ? a->GetActorBase() : nullptr; return npc && npc->GetSex() == RE::SEX::kMale; }
    bool Naked(RE::Actor* a) { return a && Extras::Exposed(a); }   // no body armour (or, with the extras, armour that leaves his backside bare)
    std::string Days(float d) {
        if (d < 0.0f) return "";
        if (d < 1.0f / 24.0f) return "just now";
        if (d < 1.0f) { const int h = std::max(1, static_cast<int>(std::lround(d * 24.0f))); return std::format("{} hour{} ago", h, h == 1 ? "" : "s"); }
        const int n = static_cast<int>(std::lround(d));
        return n <= 1 ? "a day ago" : std::format("{} days ago", n);
    }
    std::string Join(const std::vector<std::string>& v) {
        std::string o;
        for (size_t i = 0; i < v.size(); ++i) o += (i == 0 ? "" : (i + 1 == v.size() ? " and " : ", ")) + v[i];
        return o;
    }

    // ---- the VM (everything queued: these run on the main thread, never waited on)
    template <class... A>
    void Static(const char* script, const char* fn, A... args) {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;
        tasks->AddTask([script, fn, args...]() mutable {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) return;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
            vm->DispatchStaticCall(script, fn, RE::MakeFunctionArguments(std::move(args)...), cb);
        });
    }
    void Narrate(const std::string& text, RE::Actor* speaker, RE::Actor* target) {
        if (!SN() || text.empty()) return;
        ++g_sentReact;
        Static("SkyrimNetApi", "DirectNarration", RE::BSFixedString(text), speaker, target);
    }
    void Remember(const std::string& text, RE::Actor* a, RE::Actor* b) {
        if (!SN() || text.empty()) return;
        ++g_sentMem;
        Static("SkyrimNetApi", "RegisterPersistentEvent", RE::BSFixedString(text), a, b);
    }
    void Exposure(RE::FormID id, int amount) {   // SexLab Aroused's own way to arouse someone
        if (amount == 0 || !ArousalQuest()) return;
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;
        tasks->AddTask([id, amount]() {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            auto* q = ArousalQuest();
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!a || !q || !vm) return;
            auto* policy = vm->GetObjectHandlePolicy();
            if (!policy) return;
            const RE::VMHandle h = policy->GetHandleForObject(RE::TESQuest::FORMTYPE, q);
            if (h == policy->EmptyHandle()) return;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
            vm->DispatchMethodCall(h, "slaFrameworkScr", "UpdateActorExposure",
                                   RE::MakeFunctionArguments(static_cast<RE::Actor*>(a), static_cast<std::int32_t>(amount), RE::BSFixedString("Anatomica")), cb);
        });
    }

    // ---- what he is like, in words
    // what anyone looking at his naked backside can see
    std::string Seen(RE::Actor* man, bool needNaked) {
        if (!IsMan(man) || (needNaked && !Naked(man))) return {};
        const Colour::Snapshot s = Colour::SnapshotOf(man);
        const Virgin::Info v = Virgin::Get(man->GetFormID());
        std::vector<std::string> p;
        if (v.known) {
            if (v.virgin) p.push_back(v.lost > 0 ? "tight and closed, as if it had never been touched" : "a small, tight pucker that has plainly never been used");
            else if (v.exp < 3.0f) p.push_back("tight, only lightly used");
            else if (v.exp < 10.0f) p.push_back("soft and used-looking");
            else p.push_back("dark, soft and puffy - a well-used hole");
        }
        if (s.gape > 0.5f) p.push_back("gaping wide open");
        else if (s.gape > 0.15f) p.push_back("still gaping open");
        else if (s.loose > 0.25f) p.push_back("never quite closing all the way");
        if (s.tender > 0.6f) p.push_back("angry red and sore");
        else if (s.tender > 0.25f) p.push_back("red and sore-looking");
        if (s.swell > 0.25f) p.push_back("with a puffy, swollen rim");
        if (s.wet > 0.2f) p.push_back(s.cum ? "glistening with cum" : (s.lube ? "slick and glossy with lube" : "wet and glossy"));
        std::string out;
        if (!p.empty()) out = std::format("{}'s hole is {}.", Name(man), Join(p));
        if (s.schlong > 0.15f) out += (out.empty() ? "" : " ") + std::format("{}'s cock is still wet and glossy from fucking someone.", Name(man));
        return out;
    }
    // what he himself knows
    std::string Self(RE::Actor* man) {
        if (!IsMan(man)) return {};
        const Virgin::Info v = Virgin::Get(man->GetFormID());
        const Colour::Snapshot s = Colour::SnapshotOf(man);
        std::vector<std::string> out;
        const std::string me = Name(man);
        if (v.known) {
            if (v.virgin && v.lost == 0) out.push_back(std::format("{} is an anal virgin: no man has ever had him.", me));
            else if (v.virgin)
                out.push_back(std::format("{}'s body has tightened back up since he was last taken; he is as good as a virgin again{}.", me,
                                          v.back > 1 ? std::format(" (it has happened {} times)", v.back) : ""));
            else out.push_back(std::format("{} is not an anal virgin; he would call himself {}.", me, v.level));
            if (v.times > 0)
                out.push_back(std::format("He has been taken {} time{} by {} man{}.", v.times, v.times == 1 ? "" : "s", v.partners.size(), v.partners.size() == 1 ? "" : "s"));
            if (v.firstEverBy) {
                std::string t = std::format("{} took his virginity", NameOf(v.firstEverBy));
                if (v.firstBy && v.firstBy != v.firstEverBy) t += std::format("; after he had tightened up again, {} took it a second time", NameOf(v.firstBy));
                out.push_back(t + ".");
            }
            if (!v.virgin && v.daysSinceFirst >= 0.0f && v.daysSinceFirst < 1.0f) out.push_back("He is still sore and tender from losing it.");
            if (!v.virgin && v.mode == 2 && v.daysSince > 1.0f) out.push_back(std::format("He was last taken {}; with the rest he is tightening up again.", Days(v.daysSince)));
            for (auto& [pid, c] : v.partners)
                if (c >= 4) { out.push_back(std::format("{} has had him {} times - a regular.", NameOf(pid), c)); break; }
            {   // the biggest who has had him (partner size, as far as it is known)
                RE::FormID big = 0; int bc = -1;
                for (auto& [pid, c] : v.partners) { const int sc = Size::CatOf(pid); if (sc > bc) { bc = sc; big = pid; } }
                if (big && bc >= 3) out.push_back(std::format("The biggest to have had him is {}, {} - he felt every inch.", NameOf(big), bc == Size::kCreature ? "a monstrous creature" : (bc == 4 ? "who is huge" : "who is big")));
            }
        }
        const std::string seen = Seen(man, false);
        if (!seen.empty() && (s.gape > 0.15f || s.tender > 0.25f || s.wet > 0.2f || s.swell > 0.25f || s.schlong > 0.15f)) out.push_back("Right now: " + seen);
        std::string o;
        for (auto& x : out) o += (o.empty() ? "" : " ") + x;
        return o;
    }
    // what this person remembers about the men they have had
    std::string Knows(RE::Actor* npc) {
        if (!npc) return {};
        std::vector<std::string> out;
        for (auto& h : Virgin::HadBy(npc->GetFormID())) {
            const std::string him = NameOf(h.man);
            std::string t = std::format("{} has had {} {} time{}", Name(npc), him, h.times, h.times == 1 ? "" : "s");
            if (h.tookFirst) t += std::format(" and took his virginity");
            else if (h.tookIt) t += std::format(" and took his virginity again after he had tightened back up");
            if (h.daysSince >= 0.0f) t += std::format(" (last {})", Days(h.daysSince));
            out.push_back(t + ".");
            if (out.size() >= 4) break;
        }
        std::string o;
        for (auto& x : out) o += (o.empty() ? "" : " ") + x;
        return o;
    }

    // ---- what SkyrimNet has done with us this session (Integrations page)
    struct CountOk : RE::BSScript::IStackCallbackFunctor {   // SkyrimNetApi.Register*: 0 = accepted
        std::atomic<int>* ok = nullptr;
        void operator()(RE::BSScript::Variable r) override { if (ok && r.IsInt() && r.GetSInt() == 0) ++*ok; }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
    };

    // ---- natives (AnatomicaNative.SN*)
    std::string SNSelf(RE::StaticFunctionTag*, RE::Actor* a) { ++g_callsSelf; return g.snStatus ? Self(a) : std::string{}; }
    std::string SNSeen(RE::StaticFunctionTag*, RE::Actor* a) { ++g_callsSeen; return g.snStatus ? Seen(a, true) : std::string{}; }
    std::string SNKnows(RE::StaticFunctionTag*, RE::Actor* a) { ++g_callsKnows; return g.snStatus ? Knows(a) : std::string{}; }

    // a man near the speaker who fits (naked when it needs to be)
    RE::Actor* ManNear(RE::Actor* speaker, bool naked) {
        if (!speaker) return nullptr;
        std::vector<RE::Actor*> all;
        if (auto* pl = RE::PlayerCharacter::GetSingleton()) all.push_back(pl);
        if (auto* lists = RE::ProcessLists::GetSingleton())
            for (auto& h : lists->highActorHandles) { auto p = h.get(); if (p) all.push_back(p.get()); if (all.size() > 60) break; }
        for (auto* a : all)
            if (a && a != speaker && IsMan(a) && !a->IsDead() && a->Is3DLoaded() && (!naked || Naked(a)) &&
                a->GetPosition().GetDistance(speaker->GetPosition()) < 400.0f)
                return a;
        return nullptr;
    }
    bool Fit(RE::Actor* sp) { return sp && !sp->IsDead() && !sp->IsInCombat() && !sp->IsChild(); }
    bool SNCanInspect(RE::StaticFunctionTag*, RE::Actor* sp, RE::BSFixedString, RE::BSFixedString) { return SN() && g.actInspect && Fit(sp) && ManNear(sp, true); }
    bool SNCanLube(RE::StaticFunctionTag*, RE::Actor* sp, RE::BSFixedString, RE::BSFixedString) { return SN() && g.actLube && Fit(sp) && ManNear(sp, true); }
    bool SNCanClench(RE::StaticFunctionTag*, RE::Actor* sp, RE::BSFixedString, RE::BSFixedString) { return SN() && g.actClench && Fit(sp) && ManNear(sp, true); }
    bool SNCanAsk(RE::StaticFunctionTag*, RE::Actor* sp, RE::BSFixedString, RE::BSFixedString) { return SN() && g.actAsk && Fit(sp) && ManNear(sp, false); }

    void SNDo(RE::StaticFunctionTag*, RE::BSFixedString what, RE::Actor* sp, RE::Actor* t, RE::BSFixedString how) {
        const std::string w = what.c_str() ? what.c_str() : "";
        if (!sp) return;
        ++g_actRun;
        if (!t || !IsMan(t)) t = ManNear(sp, w != "ask");
        if (!t) return;
        const std::string S = Name(sp), T = Name(t);
        if (w == "inspect") {
            std::string seen = Seen(t, false);
            if (seen.empty()) seen = std::format("{}'s hole looks unremarkable.", T);
            Narrate(std::format("{} spreads {} and looks closely at his hole. {}", S, T, seen), sp, t);
        } else if (w == "lube") {
            Colour::AddWet(t, 0.7f);
            Narrate(std::format("{} slicks {}'s hole with lube, working it in until he is wet and ready.", S, T), sp, t);
        } else if (w == "clench") {
            std::string h = how.c_str() ? how.c_str() : "";
            for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (h.find("relax") != std::string::npos || h.find("open") != std::string::npos) {
                const float open = 0.35f * Virgin::For(t->GetFormID()).open;
                Engine::TestGape(t, open);
                Narrate(std::format("At {}'s order {} relaxes and lets his hole open a little.", S, T), t, sp);
            } else {
                if (!Wink::TestClench(t)) Wink::BlinkNow(t);
                Narrate(std::format("At {}'s order {} clenches his hole tight.", S, T), t, sp);
            }
        } else if (w == "ask") {
            const Virgin::Info v = Virgin::Get(t->GetFormID());
            std::string truth;
            if (!v.known) truth = "he does not say";
            else if (v.virgin && v.lost == 0) truth = "the truth is that he is: no man has ever had him";
            else if (v.virgin) truth = "the truth is that he has been had before, but his body has tightened back up since - as good as a virgin";
            else truth = std::format("the truth is that he is not: he has been taken {} time{}", v.times, v.times == 1 ? "" : "s");
            Narrate(std::format("{} asks {} whether he is still an anal virgin ({}). He answers in his own way.", S, T, truth), t, sp);
        }
    }

    void RegisterSN() {
        if (!SN()) return;
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;
        tasks->AddTask([]() {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) return;
            g_decOk = 0; g_actOk = 0; g_decAsk = 0; g_actAsk = 0; g_regAt = Now();
            auto dec = [&](const char* id, const char* fn) {
                auto c = RE::make_smart<CountOk>(); c->ok = &g_decOk;
                RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb(c);
                ++g_decAsk;
                vm->DispatchStaticCall("SkyrimNetApi", "RegisterDecorator", RE::MakeFunctionArguments(RE::BSFixedString(id), RE::BSFixedString("AnatomicaNative"), RE::BSFixedString(fn)), cb);
            };
            dec("anatomica_self", "SNSelf");
            dec("anatomica_seen", "SNSeen");
            dec("anatomica_knows", "SNKnows");
            auto act = [&](const char* name, const char* desc, const char* elig, const char* exec, const char* schema) {
                auto c = RE::make_smart<CountOk>(); c->ok = &g_actOk;
                RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb(c);
                ++g_actAsk;
                vm->DispatchStaticCall("SkyrimNetApi", "RegisterAction",
                    RE::MakeFunctionArguments(RE::BSFixedString(name), RE::BSFixedString(desc), RE::BSFixedString("AnatomicaNative"), RE::BSFixedString(elig),
                                              RE::BSFixedString("AnatomicaNative"), RE::BSFixedString(exec), RE::BSFixedString(""), RE::BSFixedString("PAPYRUS"),
                                              static_cast<std::int32_t>(1), RE::BSFixedString(schema), RE::BSFixedString(""), RE::BSFixedString("")), cb);
            };
            act("AnatomicaInspectHole", "Look closely at a naked man's anus - bend him over or spread him - to see what state it is in (tight or used, gaping, red, wet). Use when the speaker wants to check, examine or admire it.",
                "SNCanInspect", "SNInspect", "{\"target\": \"Actor\"}");
            act("AnatomicaLubeHole", "Slick a naked man's anus with lube or oil so he is wet and ready.",
                "SNCanLube", "SNLube", "{\"target\": \"Actor\"}");
            act("AnatomicaClenchOrRelax", "Order a naked man to clench his anus tight, or to relax it and let it open a little.",
                "SNCanClench", "SNClench", "{\"target\": \"Actor\", \"how\": \"clench or relax\"}");
            act("AnatomicaAskVirgin", "Ask a man whether he is still an anal virgin. He will answer.",
                "SNCanAsk", "SNAsk", "{\"target\": \"Actor\"}");
            Extras::RegisterSN(vm);   // anatomica_lot (only with the extras mod)
            logger::info("Anatomica: registered with SkyrimNet (3 decorators, 4 actions{})", Extras::On() ? ", extras' anatomica_lot" : "");
        });
    }

    // ---- what happens over time
    struct Pending { RE::FormID man = 0, partner = 0; std::string text; double since = 0.0; };
    std::mutex g_mx;
    std::vector<Pending> g_react;
    std::unordered_map<RE::FormID, std::string> g_sceneSent;
    std::unordered_map<RE::FormID, double> g_sceneAt, g_remarkAt;
    std::unordered_map<RE::FormID, int> g_arousalSent;
    double g_lastTick = 0.0, g_lastRemark = 0.0;

    // ---- bathing
    bool IsBath(RE::FormID mgefId) {
        auto* mg = RE::TESForm::LookupByID<RE::EffectSetting>(mgefId);
        if (!mg) return false;
        if (const auto* f = mg->GetFile(0); f && std::string_view(f->GetFilename()) == "Bathing in Skyrim.esp") {
            switch (mg->GetLocalFormID()) {
                case 0x026: case 0x02A: case 0x02B: case 0x02C: case 0x02F: return true;
                default: return false;
            }
        }
        std::string n = mg->GetFullName() ? mg->GetFullName() : "";
        for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return n.find("bath") != std::string::npos || n.find("wash") != std::string::npos || n.find("soap") != std::string::npos;
    }
    struct BathSink : RE::BSTEventSink<RE::TESMagicEffectApplyEvent> {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* e, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override {
            if (!e || !e->target || !g.bath || !IsBath(e->magicEffect)) return RE::BSEventNotifyControl::kContinue;
            auto* a = e->target->As<RE::Actor>();
            if (!IsMan(a)) return RE::BSEventNotifyControl::kContinue;
            {
                std::lock_guard lk(g_mx);
                static std::unordered_map<RE::FormID, double> last;
                double& t = last[a->GetFormID()];
                if (Now() - t < 20.0) return RE::BSEventNotifyControl::kContinue;   // one bath, several effects
                t = Now();
            }
            Colour::Bathed(a->GetFormID(), g.bathDry, g.bathSoothe);
            if (g.bathHours > 0.0f) Virgin::RestFor(a, g.bathHours, "a bath");
            return RE::BSEventNotifyControl::kContinue;
        }
    } g_bathSink;

    bool Section(const char* title) { return ImGuiMCP::CollapsingHeader(title); }   // a drop-down section
    void Help(const char* t) {
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::PushTextWrapPos(420.0f); ImGuiMCP::TextUnformatted(t); ImGuiMCP::PopTextWrapPos(); ImGuiMCP::EndTooltip(); }
    }
    void Missing(const char* what) { ImGuiMCP::TextDisabled("%s is not installed: nothing here does anything.", what); }
}

namespace Bridge {
    void Ini(CSimpleIniA& ini, bool save) {
        const char* S = "Integrations";
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto I = [&](const char* k, int& v, int lo, int hi) { if (save) ini.SetLongValue(S, k, v); else v = std::clamp(static_cast<int>(ini.GetLongValue(S, k, v)), lo, hi); };
        auto F = [&](const char* k, float& v, float lo, float hi) { if (save) ini.SetDoubleValue(S, k, v); else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi); };
        B("SkyrimNet", g.sn); B("SNStatus", g.snStatus); B("SNMemory", g.snMemory); B("SNReact", g.snReact);
        B("SNInspect", g.actInspect); B("SNLube", g.actLube); B("SNClench", g.actClench); B("SNAsk", g.actAsk);
        B("SNScene", g.snScene); B("SNRemarks", g.snRemarks); F("SNRemarkMinutes", g.remarkMinutes, 0.5f, 60.0f); F("SNRemarkChance", g.remarkChance, 0.0f, 1.0f);
        B("SLSF", g.fame); I("SLSFFameMin", g.fameLo, 0, 50); I("SLSFFameMax", g.fameHi, 1, 100); F("SLSFFirst", g.fameFirst, 1.0f, 10.0f); B("SLSFGroup", g.fameGroup);
        B("Arousal", g.arousal); I("ArousalFrom", g.arousalFrom, 0, 99); F("ArousalWet", g.arousalWet, 0.0f, 1.0f); B("ArousalSensitive", g.arousalSens);
        I("ArousalFirstTime", g.exposureFirst, 0, 100); I("ArousalEachTime", g.exposureEach, 0, 50);
        B("Bathing", g.bath); B("BathDries", g.bathDry); F("BathSoothes", g.bathSoothe, 0.0f, 1.0f); F("BathHours", g.bathHours, 0.0f, 48.0f);
        if (g.fameHi < g.fameLo) g.fameHi = g.fameLo;
        Size::Ini(ini, save);
        Extras::Ini(ini, save);
    }

    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("SNSelf"sv, "AnatomicaNative"sv, SNSelf);
        vm->RegisterFunction("SNSeen"sv, "AnatomicaNative"sv, SNSeen);
        vm->RegisterFunction("SNKnows"sv, "AnatomicaNative"sv, SNKnows);
        vm->RegisterFunction("SNCanInspect"sv, "AnatomicaNative"sv, SNCanInspect);
        vm->RegisterFunction("SNCanLube"sv, "AnatomicaNative"sv, SNCanLube);
        vm->RegisterFunction("SNCanClench"sv, "AnatomicaNative"sv, SNCanClench);
        vm->RegisterFunction("SNCanAsk"sv, "AnatomicaNative"sv, SNCanAsk);
        vm->RegisterFunction("SNDo"sv, "AnatomicaNative"sv, SNDo);
        Extras::RegisterFuncs(vm);
    }

    void OnGameLoaded() {
        static bool sink = false;
        if (!sink) if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) { src->AddEventSink<RE::TESMagicEffectApplyEvent>(&g_bathSink); sink = true; }
        {
            std::lock_guard lk(g_mx);
            g_react.clear(); g_sceneSent.clear(); g_sceneAt.clear(); g_remarkAt.clear(); g_arousalSent.clear();
        }
        RegisterSN();
    }

    void Taken(RE::FormID manId, RE::FormID partnerId, bool first, std::uint32_t lostBefore, RE::FormID prevBy, RE::FormID everBy) {
        auto* man = RE::TESForm::LookupByID<RE::Actor>(manId);
        auto* partner = partnerId ? RE::TESForm::LookupByID<RE::Actor>(partnerId) : nullptr;
        if (!man) return;
        // SLSF Reloaded: his reputation where it happened (SLSF fame is the player's)
        if (g.fame && HasSLSF() && man->IsPlayerRef()) {
            const float k = first ? g.fameFirst : 1.0f;
            const int lo = std::clamp(static_cast<int>(std::lround(g.fameLo * k)), 0, 100), hi = std::clamp(static_cast<int>(std::lround(g.fameHi * k)), lo + 1, 100);
            Static("AnatomicaNative", "SendFame", RE::BSFixedString("Anal"), static_cast<std::int32_t>(lo), static_cast<std::int32_t>(hi));
            if (g.fameGroup && Colour::SnapshotOf(man).partners >= 3)
                Static("AnatomicaNative", "SendFame", RE::BSFixedString("Group"), static_cast<std::int32_t>(lo), static_cast<std::int32_t>(hi));
        }
        // arousal
        if (g.arousal) Exposure(manId, first ? g.exposureFirst : g.exposureEach);
        if (!first || !SN()) return;
        // SkyrimNet: both remember it; the partner reacts once the round is over
        const std::string M = Name(man), P = partner ? Name(partner) : std::string{};
        std::string with;   // the partner's size, when it stands out (TNG)
        if (partner) {   // "with his huge, knotted cock" / "with his uncut cock"
            const std::string d = Size::Describe(partner);
            if (!d.empty()) with = std::format(" with {} {} cock", Size::Cat(partner) == Size::kCreature ? "its" : "his", d);
        }
        std::string mem, react;
        const Extras::How how = Extras::HowNow(man, partner);   // with the extras: taken as a captive / for money
        if (lostBefore == 0 && how == Extras::How::captive) {
            mem = partner ? std::format("{} took {}'s anal virginity{} while he was a helpless captive - he had no say in it.", P, M, with)
                          : std::format("{} lost his anal virginity as a captive, with no say in it.", M);
            react = partner ? std::format("{} has just taken captive {}'s virginity{}: his first time with any man, and not by his choice.", P, M, with) : std::string{};
        } else if (lostBefore == 0 && how == Extras::How::paid) {
            mem = partner ? std::format("{} paid to take {}'s anal virginity{} - his first time with any man, sold for coin.", P, M, with)
                          : std::format("{} sold his anal virginity.", M);
            react = partner ? std::format("{} has just had {}'s virginity{}, paid for: his first time with any man.", P, M, with) : std::string{};
        } else if (lostBefore == 0) {
            mem = partner ? std::format("{} took {}'s anal virginity{} - his first time with any man.", P, M, with) : std::format("{} lost his anal virginity.", M);
            react = partner ? std::format("{} has just taken {}'s virginity{}: his first time with any man.", P, M, with) : std::string{};
        } else if (partner && prevBy == partnerId) {
            mem = std::format("{} had {} again after his body had tightened back up - tight as a virgin again.", P, M);
            react = std::format("{} has just had {} again and found him as tight as a virgin after his rest.", P, M);
        } else {
            const std::string firstName = everBy ? NameOf(everBy) : std::string("another man");
            mem = partner ? std::format("{} took {}'s virginity again: his body had tightened back up since {} first had him.", P, M, firstName)
                          : std::format("{} lost his virginity again after his body had tightened back up.", M);
            react = partner ? std::format("{} has just taken {}'s virginity again - he was as tight as a virgin, though {} had him first.", P, M, firstName) : std::string{};
        }
        if (g.snMemory) Remember(mem, partner ? partner : man, man);
        if (g.snReact && !react.empty()) {
            std::lock_guard lk(g_mx);
            g_react.push_back({manId, partnerId, react, Now()});
        }
    }

    void Restored(RE::FormID manId) {
        if (!SN() || !g.snMemory) return;
        auto* man = RE::TESForm::LookupByID<RE::Actor>(manId);
        if (!man) return;
        Remember(std::format("{}'s body has recovered: he is tight again, as good as a virgin once more.", Name(man)), man, man);
    }

    void Tick(RE::Actor* player, const std::vector<RE::Actor*>& all) {
        const double now = Now();
        if (now - g_lastTick < 2.0 || !player) return;
        g_lastTick = now;
        // arousal: read the shared SexLab Aroused faction for the men nearby
        if (auto* fac = ArousalFaction()) {
            for (auto* a : all) {
                if (!IsMan(a) || a->IsDead()) continue;
                int rank = g.arousal ? a->GetFactionRank(fac, a->IsPlayerRef()) : -1;
                rank = std::clamp(rank, -1, 100);
                const RE::FormID id = a->GetFormID();
                int& sent = g_arousalSent[id];
                if (std::abs(rank - sent) < 3 && !(rank < g.arousalFrom && sent >= g.arousalFrom)) continue;
                sent = rank;
                const float over = rank >= g.arousalFrom ? static_cast<float>(rank - g.arousalFrom) / static_cast<float>(std::max(1, 100 - g.arousalFrom)) : 0.0f;
                Colour::SetArousal(id, over * g.arousalWet, g.arousalSens ? 0.5f * over : 0.0f);
            }
        }
        if (!SN()) return;
        // reactions after his first time: once he is no longer being penetrated (or after two minutes)
        std::vector<Pending> due;
        {
            std::lock_guard lk(g_mx);
            for (auto it = g_react.begin(); it != g_react.end();) {
                const double ago = Virgin::Get(it->man).workedAgo;
                if ((ago >= 0.0 && ago > 12.0) || now - it->since > 120.0) { due.push_back(*it); it = g_react.erase(it); }
                else ++it;
            }
        }
        for (auto& p : due) Narrate(p.text, RE::TESForm::LookupByID<RE::Actor>(p.partner), RE::TESForm::LookupByID<RE::Actor>(p.man));
        // what can be seen right now (naked men near the player)
        for (auto* man : all) {
            if (!IsMan(man) || man->IsDead() || man->GetPosition().GetDistance(player->GetPosition()) > 1500.0f) continue;
            const RE::FormID id = man->GetFormID();
            const std::string seen = (g.snScene || g.snRemarks) ? Seen(man, true) : std::string{};
            const Colour::Snapshot s = seen.empty() ? Colour::Snapshot{} : Colour::SnapshotOf(man);
            const bool shows = !seen.empty() && (s.gape > 0.15f || s.tender > 0.25f || s.wet > 0.2f || s.swell > 0.25f || s.schlong > 0.15f);
            if (g.snScene && shows) {
                std::lock_guard lk(g_mx);
                if (g_sceneSent[id] != seen || now - g_sceneAt[id] > 20.0) {
                    g_sceneSent[id] = seen; g_sceneAt[id] = now; ++g_sentScene;
                    Static("SkyrimNetApi", "RegisterShortLivedEvent", RE::BSFixedString(std::format("anatomica_seen_{:08X}", id)), RE::BSFixedString("anatomica_seen"),
                           RE::BSFixedString(seen), RE::BSFixedString(""), static_cast<std::int32_t>(30000), man, static_cast<RE::Actor*>(nullptr));
                }
            }
            if (g.snRemarks && shows && (s.gape > 0.3f || s.tender > 0.4f || s.wet > 0.4f)) {
                {
                    std::lock_guard lk(g_mx);
                    if (now - g_remarkAt[id] < g.remarkMinutes * 60.0 || now - g_lastRemark < 60.0) continue;
                    g_remarkAt[id] = now;   // one roll per cooldown
                }
                if (Rand01() > g.remarkChance) continue;
                RE::Actor* who = nullptr; float best = 700.0f;
                for (auto* o : all) {
                    if (!o || o == man || o->IsDead() || o->IsChild() || o->IsInCombat() || o->IsPlayerRef()) continue;
                    const float d = o->GetPosition().GetDistance(man->GetPosition());
                    bool los = false;
                    if (d < best && o->HasLineOfSight(man, los) && los) { best = d; who = o; }
                }
                if (!who) continue;
                { std::lock_guard lk(g_mx); g_lastRemark = now; }
                ++g_sentRemark;
                Narrate(std::format("{} notices {} naked - {}", Name(who), Name(man), seen), who, man);
            }
        }
    }

    void __stdcall RenderPage() {
        bool ch = false;
        ImGuiMCP::TextWrapped("Anatomica with other mods. Each part only does something when its mod is installed.");

        if (Section("SkyrimNet")) {
        if (!HasSkyrimNet()) Missing("SkyrimNet");
        else {
            // is it working? (this session)
            const ImGuiMCP::ImVec4 good{0.5f, 0.9f, 0.5f, 1.0f}, bad{1.0f, 0.55f, 0.4f, 1.0f};
            if (g_regAt <= 0.0) ImGuiMCP::TextColored(bad, "Not registered with SkyrimNet yet this session (it happens a few seconds after loading a save).");
            else {
                const bool all = g_decOk == g_decAsk && g_actOk == g_actAsk;
                ImGuiMCP::TextColored(all ? good : bad, "SkyrimNet accepted %d of %d status functions and %d of %d actions (%.0f s ago).",
                                      g_decOk.load(), g_decAsk.load(), g_actOk.load(), g_actAsk.load(), Now() - g_regAt);
            }
            ImGuiMCP::TextDisabled("Since loading: SkyrimNet asked for his state %d / what is seen %d / what partners know %d times; actions done %d.",
                                   g_callsSelf.load(), g_callsSeen.load(), g_callsKnows.load(), g_actRun.load());
            ImGuiMCP::TextDisabled("Sent: memories %d, reactions and remarks %d, scene notes %d.", g_sentMem.load(), g_sentReact.load(), g_sentScene.load());
            Help("SkyrimNet only asks for a man's state when it writes a prompt about him (when someone talks to or about "
                 "him), so the first numbers stay at 0 until then.");
            static std::string pv[3];
            static std::string who;
            if (ImGuiMCP::Button("Show what SkyrimNet gets for the selected man")) {
                RE::Actor* a = nullptr;
                if (auto sel = RE::Console::GetSelectedRef()) a = sel->As<RE::Actor>();
                if (!a) a = RE::PlayerCharacter::GetSingleton();
                who = Name(a);
                pv[0] = Self(a); pv[1] = Seen(a, true); pv[2] = Knows(a);
            }
            Help("The man you click in the console (~), or you. Exactly the text SkyrimNet's prompts get.");
            if (!who.empty()) {
                ImGuiMCP::TextWrapped("%s - what he knows: %s", who.c_str(), pv[0].empty() ? "(nothing)" : pv[0].c_str());
                ImGuiMCP::TextWrapped("What others see (naked only): %s", pv[1].empty() ? "(nothing - dressed, or nothing shows)" : pv[1].c_str());
                ImGuiMCP::TextWrapped("What he remembers of men he has had: %s", pv[2].empty() ? "(nothing)" : pv[2].c_str());
            }
        }
        ch |= ImGuiMCP::Checkbox("Use SkyrimNet", &g.sn);
        if (g.sn) {
            ImGuiMCP::Indent();
            ch |= ImGuiMCP::Checkbox("What people know and see", &g.snStatus);
            Help("A man knows his own state and history; men who have had him remember it; anyone who sees him naked sees "
                 "how his hole looks right now (tight or used, gaping, red, wet). Needs the Anatomica prompt pack (installed "
                 "with the mod).");
            ch |= ImGuiMCP::Checkbox("Memories", &g.snMemory);
            Help("When he loses his virginity both remember who took it and when; when his body tightens back up he "
                 "remembers that (quietly - nobody reacts).");
            ch |= ImGuiMCP::Checkbox("Reaction after his first time", &g.snReact);
            Help("Once the round is over, the man who took it says something about it - aware whether it was his real "
                 "first time or a restored one.");
            ImGuiMCP::TextUnformatted("Things NPCs can choose to do:");
            ImGuiMCP::Indent();
            ch |= ImGuiMCP::Checkbox("Inspect his hole", &g.actInspect);
            ch |= ImGuiMCP::Checkbox("Lube him up", &g.actLube);
            ch |= ImGuiMCP::Checkbox("Order him to clench or relax", &g.actClench);
            ch |= ImGuiMCP::Checkbox("Ask if he is a virgin", &g.actAsk);
            ImGuiMCP::Unindent();
            Help("Only offered when it makes sense: a naked man close by (asking does not need him naked).");
            ch |= ImGuiMCP::Checkbox("Scene awareness", &g.snScene);
            Help("While he is naked and it shows (gaping, red, wet...), people nearby are told what they can see. It goes "
                 "away when he recovers or dresses.");
            ch |= ImGuiMCP::Checkbox("People comment on it", &g.snRemarks);
            if (g.snRemarks) {
                ImGuiMCP::Indent();
                ch |= ImGuiMCP::SliderFloat("At most every (minutes, per man)", &g.remarkMinutes, 0.5f, 60.0f, "%.1f");
                float pct = g.remarkChance * 100.0f;
                if (ImGuiMCP::SliderFloat("Chance", &pct, 0.0f, 100.0f, "%.0f%%")) { g.remarkChance = pct / 100.0f; ch = true; }
                ImGuiMCP::Unindent();
            }
            if (ImGuiMCP::Button("Register with SkyrimNet again")) RegisterSN();
            Help("Done on every load. Use this if you switched SkyrimNet parts on in the middle of a game.");
            ImGuiMCP::Unindent();
        }

        }
        ch |= Size::RenderSection();
        if (Section("SLSF Reloaded (fame)")) {
        if (!HasSLSF()) Missing("SLSF Reloaded");
        ch |= ImGuiMCP::Checkbox("Being taken adds Anal fame", &g.fame);
        Help("Where it happens, each time you (the player) are taken. SLSF fame belongs to the player only.");
        if (g.fame) {
            ImGuiMCP::Indent();
            ch |= ImGuiMCP::SliderInt("Fame, at least", &g.fameLo, 0, 50);
            ch |= ImGuiMCP::SliderInt("Fame, at most", &g.fameHi, 1, 100);
            if (g.fameHi < g.fameLo) g.fameHi = g.fameLo;
            ch |= ImGuiMCP::SliderFloat("Your first time counts", &g.fameFirst, 1.0f, 10.0f, "x%.1f");
            ch |= ImGuiMCP::Checkbox("Several men in a row: Group fame too", &g.fameGroup);
            ImGuiMCP::Unindent();
        }

        }
        if (Section("Arousal (SexLab Aroused / OSL Aroused / SLO Aroused NG)")) {
        if (!ArousalFaction()) Missing("An arousal mod");
        ch |= ImGuiMCP::Checkbox("Arousal makes him wet and twitchy", &g.arousal);
        if (g.arousal) {
            ImGuiMCP::Indent();
            ch |= ImGuiMCP::SliderInt("From arousal", &g.arousalFrom, 0, 99);
            float pct = g.arousalWet * 100.0f;
            if (ImGuiMCP::SliderFloat("How wet at full arousal", &pct, 0.0f, 100.0f, "%.0f%%")) { g.arousalWet = pct / 100.0f; ch = true; }
            ch |= ImGuiMCP::Checkbox("Twitchier", &g.arousalSens);
            Help("Sharper clenches and slower, heavier breathing of the hole - uses \"Sensitive while tender\" (After sex page).");
            ch |= ImGuiMCP::SliderInt("His first time arouses him", &g.exposureFirst, 0, 100);
            ch |= ImGuiMCP::SliderInt("Every time he is taken", &g.exposureEach, 0, 50);
            ImGuiMCP::Unindent();
        }

        }
        if (Section("Bathing")) {
        ch |= ImGuiMCP::Checkbox("Washing", &g.bath);
        Help("Bathing in Skyrim (or any bath / wash / soap effect).");
        if (g.bath) {
            ImGuiMCP::Indent();
            ch |= ImGuiMCP::Checkbox("Dries him (and the giver's schlong)", &g.bathDry);
            float pct = g.bathSoothe * 100.0f;
            if (ImGuiMCP::SliderFloat("Soothes the redness by", &pct, 0.0f, 100.0f, "%.0f%%")) { g.bathSoothe = pct / 100.0f; ch = true; }
            ch |= ImGuiMCP::SliderFloat("Counts as rest", &g.bathHours, 0.0f, 48.0f, "%.1f h");
            Help("Like a healing potion: hours off his waits (virginity coming back, looseness, the first-time reaction - "
                 "what counts is set on the Virginity page).");
            ImGuiMCP::Unindent();
        }
        }
        if (ch) Colour::SaveNow();
    }
}

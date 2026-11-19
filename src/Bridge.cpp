#include "Bridge.h"
#include "Lang.h"
#include "Mu.h"
#include "OStrandsLink.h"
#include "Colossal.h"
#include "Colour.h"
#include "Engine.h"
#include "Virgin.h"
#include "Size.h"
#include "Extras.h"
#include "Wink.h"
#include "During.h"
#include "Kinds.h"
#include "Mnc.h"
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
        // 2.5o: the belly bulge and what kinds of partner he takes
        bool snBulge = true;       // the bulge in what is seen / what he feels
        bool madeFor = true;       // "made for it": after enough times with one kind
        int madeForTimes = 8;
        float madeForForget = 30.0f;   // game days without it and it fades
        bool looseFrom = true;     // what last stretched him
        bool bulgeMem = true;      // partners remember the bulge they gave him
        bool rumours = true;       // people where it happened talk about it
        int rumourMin = 3;
        bool slsfBeast = true;     // SLSF: creatures add Bestiality fame (the player)
        bool firsts = true;        // first knot / creature / horse / colossal / super deep: memories + a reaction
        bool watchBulge = true;    // people watching react to the bulge
        bool snCum = true;         // 2.6f: cum in him (OStrands): seen, felt, remarked on, remembered
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
    // 2.5o: the belly bulge
    const char* BulgeShapeWords(int shape) {
        switch (shape) {
            case During::kTip: return "a hard bump";
            case During::kHorse: return "a broad, flared head with a ring along the shaft behind it";
            case During::kKnot: return "a cock and a fat knot";
            case During::kTentacle: return "something that writhes and wanders under the skin";
            case During::kRidged: return "a ridged shaft";
            case During::kSegmented: return "a knobbly, segmented shaft";
            case During::kColossal: return "an enormous, thick shaft";
            case During::kHooked: return "a cock that hooks off to one side at its end";
            case During::kDoubleKnot: return "a cock with two fat knots";
            case During::kBeaded: return "a long tentacle curling inside him";
            default: return "the outline of a cock";
        }
    }
    const char* ReachWords(float r, bool deep) {
        if (deep && r > 6.2f) return "almost up to his chest";
        if (r > 4.2f) return "well past his navel";
        if (r > 2.4f) return "up to his navel";
        return "low in his belly";
    }
    std::string BulgeSeen(RE::Actor* man) {
        if (!g.snBulge || !man) return {};
        const Engine::Bulge b = Engine::BulgeOf(man->GetFormID());
        const std::string M = Name(man);
        if (b.on) {
            const char* how = b.amount > 0.7f ? "huge, stretched" : b.amount > 0.3f ? "clear" : "faint";
            std::string who;
            if (auto* p = RE::TESForm::LookupByID<RE::Actor>(b.partner)) {
                const Kinds::Kind k = Kinds::KindOf(p);
                who = k.key.starts_with("creature:") ? std::format(" - the {}'s cock moving inside him", k.key.substr(9))
                                                      : std::format(" - {}'s cock moving inside him", Name(p));
            }
            std::string t = std::format("{}'s belly shows a {} bulge of {}, {}{}.", M, how, BulgeShapeWords(b.shape), ReachWords(b.reach, b.deep), who);
            if (b.belly > 0.3f) t += " His whole stomach is rounded out with it.";
            return t;
        }
        if (b.imprint > 0.03f) return std::format("{}'s belly still shows a faint outline of {} where it was.", M, BulgeShapeWords(b.shape));
        return {};
    }
    // what anyone looking at his naked backside can see
    std::string Seen(RE::Actor* man, bool needNaked) {
        if (!IsMan(man)) return {};
        const std::string bulge = BulgeSeen(man);   // 2.5o: shows when the bulge itself is allowed to (its own "shows through")
        if (needNaked && !Naked(man)) return bulge;
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
        if (g.looseFrom && (s.loose > 0.25f || s.gape > 0.15f)) {   // 2.5o: what stretched him
            const Kinds::Info k = Kinds::Get(man->GetFormID(), g.madeForTimes, g.madeForForget);
            if (k.lastDays >= 0.0f && k.lastDays < 2.0f && (k.lastCreature || !k.lastKind.empty()))
                out += (out.empty() ? "" : " ") + std::format("It looks stretched by something big - {}.", k.lastCreature && Mnc::Active() ? "an animal's cock" : k.lastKind);
        }
        if (!bulge.empty()) out += (out.empty() ? "" : " ") + bulge;
        if (g.snCum) {   // 2.6f: cum in him (OStrands)
            const OStrandsLink::Held h = OStrandsLink::HeldOf(man->GetFormID());
            std::string c;
            if (h.flooding) c = std::format("Cum is pouring out of {}'s hole in a steady stream.", Name(man));
            else if (h.leaking && h.vol > 0.05f) c = std::format("Cum is dribbling out of {}'s hole and running down his thighs.", Name(man));
            else if (h.vol > 0.5f) c = std::format("{}'s hole is full of cum; it glistens at the rim.", Name(man));
            if (h.vol > 2.0f && OStrandsLink::Belly(man->GetFormID()) > 0.3f) c += (c.empty() ? "" : " ") + std::format("His belly is visibly rounded, swollen with it.");
            if (!c.empty()) out += (out.empty() ? "" : " ") + c;
        }
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
                if (big && bc >= 3) out.push_back(std::format("The biggest to have had him is {}, {} - he felt every inch.", NameOf(big), bc == Size::kCreature ? (Mnc::Active() ? "a monstrous creature" : "who is enormous") : (bc == 4 ? "who is huge" : "who is big")));
            }
        }
        {   // 2.5o: what kinds he takes, how deep it has gone, what stretched him, what he feels right now
            const Kinds::Info k = Kinds::Get(man->GetFormID(), g.madeForTimes, g.madeForForget);
            if (g.madeFor && !k.madeFor.empty())
                out.push_back(k.madeForCreature ? std::format("His body seems made for {} - he takes them easily and well.", k.madeForMany)
                                                : std::format("He takes {} like he was made for them.", k.madeForMany));
            if (g.snBulge && k.everDeep) out.push_back("He has been taken so deep that it bulged almost up to his chest.");
            else if (g.snBulge && k.deepest > 4.2f) out.push_back("He has been taken deep enough to see it bulge past his navel.");
            if (g.looseFrom && k.lastDays >= 0.0f && k.lastDays < 2.0f && (s.loose > 0.25f || s.gape > 0.15f)) {
                if (k.lastCreature && !k.lastKindMany.empty()) out.push_back(std::format("He is still loose from the {} {}.", k.lastKindMany, Days(k.lastDays)));
                else if (k.lastPartner) out.push_back(std::format("He is still loose from {} {}.", NameOf(k.lastPartner), Days(k.lastDays)));
            }
            const Engine::Bulge b = Engine::BulgeOf(man->GetFormID());
            if (g.snBulge && b.on) out.push_back(std::format("Right now he can feel it reaching {} inside him.", ReachWords(b.reach, b.deep)));
        }
        if (g.snCum) {   // 2.6f: what he feels of the cum in him
            const OStrandsLink::Held h = OStrandsLink::HeldOf(man->GetFormID());
            if (h.filling) out.push_back("Right now he is being filled - he can feel it pumping into him.");
            if (h.flooding) out.push_back("It is pouring out of him right now and he cannot hold it in.");
            else if (h.leaking) out.push_back("He can feel it trickling out of him and down his thighs.");
            if (h.plugged && h.vol > 0.2f) out.push_back("A plug holds it all in him.");
            if (h.vol > 3.0f) out.push_back("He is stuffed full of cum - his belly sloshes with it.");
            else if (h.vol > 1.0f) out.push_back("He is full of cum; he can feel it inside him.");
            else if (h.vol > 0.2f) out.push_back("There is still some cum inside him.");
            const Kinds::StuffedInfo st = Kinds::StuffedOf(man->GetFormID());   // 2.6g
            if (st.times >= 2) out.push_back(std::format("He has been stuffed full of cum {} times{}.", st.times,
                                                         st.daysSince >= 0.0f && st.daysSince < 1.0f ? ", the last time today" : ""));
            else if (st.times == 1) out.push_back("He has been stuffed full of cum once before.");
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
            if (g.madeFor) {   // 2.5o
                const Kinds::Info k = Kinds::Get(h.man, g.madeForTimes, g.madeForForget);
                if (!k.madeFor.empty()) t += std::format("; {} takes {} like he was made for them", him, k.madeForMany);
            }
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
    std::string SNRumour(RE::StaticFunctionTag*, RE::Actor* a) { return (g.snStatus && g.rumours && IsMan(a)) ? Kinds::Rumour(a, g.rumourMin) : std::string{}; }

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
            dec("anatomica_rumour", "SNRumour");   // 2.5o
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
            logger::info("Anatomica: registered with SkyrimNet (4 decorators, 4 actions{})", Extras::On() ? ", extras' anatomica_lot" : "");
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
    struct BulgeRound { bool active = false, watched = false; float most = 0.0f; bool deep = false; RE::FormID partner = 0; int shape = 0; double offAt = 0.0; };
    std::unordered_map<RE::FormID, BulgeRound> g_bulgeRound;
    std::unordered_map<RE::FormID, double> g_floodSeen;   // 2.6f: the last flood of his someone may have remarked on   // 2.5o: per man, the bulge this round (memories, watchers)

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
            OStrandsLink::Bathed(a->GetFormID());   // 2.6g: it all runs out of him
            if (g.bathHours > 0.0f) Virgin::RestFor(a, g.bathHours, "a bath");
            return RE::BSEventNotifyControl::kContinue;
        }
    } g_bathSink;

    bool Section(const char* title) { return Tr::CollapsingHeader(title); }   // a drop-down section
    void Help(const char* t) {
        ImGuiMCP::SameLine();
        Tr::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::PushTextWrapPos(420.0f); Tr::TextUnformatted(t); ImGuiMCP::PopTextWrapPos(); ImGuiMCP::EndTooltip(); }
    }
    // a mod that is not installed: a red line, and its settings greyed out (kept as they are, for when it is added)
    void Missing(const char* what) { Tr::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.4f, 1.0f}, "%s is not installed - these settings do nothing until it is (they are kept).", what); }
    std::string Title(const char* title, const char* id, bool missing) {   // 2.5v: translated here (its ID stays the same)
        return std::format("{}{}###{}", Lang::T(title), missing ? std::string("  ") + Lang::T("(not installed)") : std::string(), id);
    }
}

namespace Bridge {
    void Ini(CSimpleIniA& ini, bool save) {
        const char* S = "Integrations";
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto I = [&](const char* k, int& v, int lo, int hi) { if (save) ini.SetLongValue(S, k, v); else v = std::clamp(static_cast<int>(ini.GetLongValue(S, k, v)), lo, hi); };
        auto F = [&](const char* k, float& v, float lo, float hi) { if (save) ini.SetDoubleValue(S, k, v); else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi); };
        B("SkyrimNet", g.sn); B("SNStatus", g.snStatus); B("SNMemory", g.snMemory); B("SNReact", g.snReact);
        B("SNInspect", g.actInspect); B("SNLube", g.actLube); B("SNClench", g.actClench); B("SNAsk", g.actAsk);
        B("SNBulge", g.snBulge); B("SNMadeFor", g.madeFor); I("SNMadeForTimes", g.madeForTimes, 2, 100); F("SNMadeForForget", g.madeForForget, 2.0f, 365.0f);
        B("SNLooseFrom", g.looseFrom); B("SNBulgeMemory", g.bulgeMem); B("SNRumours", g.rumours); I("SNRumourMin", g.rumourMin, 1, 50);
        B("SLSFBestiality", g.slsfBeast); B("SNFirsts", g.firsts); B("SNWatchBulge", g.watchBulge); B("SNCum", g.snCum);
        B("SNScene", g.snScene); B("SNRemarks", g.snRemarks); F("SNRemarkMinutes", g.remarkMinutes, 0.5f, 60.0f); F("SNRemarkChance", g.remarkChance, 0.0f, 1.0f);
        B("SLSF", g.fame); I("SLSFFameMin", g.fameLo, 0, 50); I("SLSFFameMax", g.fameHi, 1, 100); F("SLSFFirst", g.fameFirst, 1.0f, 10.0f); B("SLSFGroup", g.fameGroup);
        B("Arousal", g.arousal); I("ArousalFrom", g.arousalFrom, 0, 99); F("ArousalWet", g.arousalWet, 0.0f, 1.0f); B("ArousalSensitive", g.arousalSens);
        I("ArousalFirstTime", g.exposureFirst, 0, 100); I("ArousalEachTime", g.exposureEach, 0, 50);
        B("Bathing", g.bath); B("BathDries", g.bathDry); F("BathSoothes", g.bathSoothe, 0.0f, 1.0f); F("BathHours", g.bathHours, 0.0f, 48.0f);
        if (g.fameHi < g.fameLo) g.fameHi = g.fameLo;
        Size::Ini(ini, save);
        Mu::Ini(ini, save);
        OStrandsLink::Ini(ini, save);   // 2.5w
        Colossal::Ini(ini, save);       // 2.6c
        Extras::Ini(ini, save);
    }

    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("SNSelf"sv, "AnatomicaNative"sv, SNSelf);
        vm->RegisterFunction("SNSeen"sv, "AnatomicaNative"sv, SNSeen);
        vm->RegisterFunction("SNKnows"sv, "AnatomicaNative"sv, SNKnows);
        vm->RegisterFunction("SNRumour"sv, "AnatomicaNative"sv, SNRumour);
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
            g_react.clear(); g_sceneSent.clear(); g_sceneAt.clear(); g_remarkAt.clear(); g_arousalSent.clear(); g_bulgeRound.clear(); g_floodSeen.clear();
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
        // 2.5o: what kind took him (made for it, rumours, what stretched him) and his firsts
        const std::uint32_t fresh = Kinds::OnTaken(man, partner);
        if (g.slsfBeast && g.fame && HasSLSF() && man->IsPlayerRef() && partner && Size::IsCreatureActor(partner) && Mnc::Active())
            Static("AnatomicaNative", "SendFame", RE::BSFixedString("Bestiality"), static_cast<std::int32_t>(g.fameLo), static_cast<std::int32_t>(std::max(g.fameLo + 1, g.fameHi)));
        if (fresh && g.firsts && SN() && partner) {
            const std::string M = Name(man), P = Name(partner);
            const Kinds::Kind k = Kinds::KindOf(partner);
            std::vector<std::string> what;
            if (fresh & Kinds::kFirstCreature) what.push_back(k.key.starts_with("creature:") ? std::format("his first time with an animal - a {}", k.key.substr(9)) : "his first time with an animal");
            if (fresh & Kinds::kFirstKnot) what.push_back("the first time a knot was pushed into him");
            if (fresh & Kinds::kFirstFlare) what.push_back("the first flared cock he has taken");
            if (fresh & Kinds::kFirstColossal) what.push_back("the biggest thing he has ever taken");
            if (!what.empty()) {
                const std::string w = Join(what);
                if (g.snMemory) Remember(std::format("{} took {}: {}.", P, M, w), partner, man);
                std::lock_guard lk(g_mx);
                g_react.push_back({manId, partnerId, std::format("{} has just had {} - {}.", P, M, w), Now()});
            }
        }
        if (!first || !SN()) return;
        // SkyrimNet: both remember it; the partner reacts once the round is over
        const std::string M = Name(man), P = partner ? Name(partner) : std::string{};
        std::string with;   // the partner's size, when it stands out (TNG)
        if (partner) {   // "with his huge, knotted cock" / "with his uncut cock"
            const std::string d = Size::Describe(partner);
            if (!d.empty()) with = std::format(" with {} {} cock", Size::Cat(partner) == Size::kCreature && Mnc::Active() ? "its" : "his", d);
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
        // 2.6f: fillings that just finished (OStrands): the giver remembers, a first Colossal Load is a first
        if (g.snCum) {
            for (auto& f : OStrandsLink::TakeFilled()) {
                auto* man = RE::TESForm::LookupByID<RE::Actor>(f.man);
                auto* giver = RE::TESForm::LookupByID<RE::Actor>(f.giver);
                if (!man || !giver || f.loads < 0.3f) continue;
                const char* how = f.colossal ? "an enormous load" : f.loads > 1.5f ? "a big load" : "his load";
                if (g.snMemory) Remember(std::format("{} came deep inside {}, filling him with {}.", Name(giver), Name(man), how), giver, man);
                if (f.colossal && g.firsts && (Kinds::OnColossalLoad(f.man) & Kinds::kFirstColossalLoad)) {
                    const std::string t = std::format("{} filled {} with more cum than he had ever taken - it kept coming until it ran out of him.", Name(giver), Name(man));
                    if (g.snMemory) Remember(t, giver, man);
                    std::lock_guard lk(g_mx);
                    g_react.push_back({f.man, f.giver, t, now});
                }
            }
        }
        if (g.snCum) {   // 2.6g: stuffed full - counted, and remembered
            for (auto id : OStrandsLink::TakeStuffed()) {
                const std::uint16_t n = Kinds::OnStuffed(id);
                auto* man = RE::TESForm::LookupByID<RE::Actor>(id);
                if (man && g.snMemory && (n == 1 || n == 5 || n == 10 || n % 25 == 0))
                    Remember(n == 1 ? std::format("{} was stuffed so full of cum that his belly swelled with it - the first time.", Name(man))
                                    : std::format("{} has been stuffed full of cum {} times now.", Name(man), n), man, man);
            }
        }
        // what can be seen right now (naked men near the player)
        for (auto* man : all) {
            if (!IsMan(man) || man->IsDead() || man->GetPosition().GetDistance(player->GetPosition()) > 1500.0f) continue;
            const RE::FormID id = man->GetFormID();
            const std::string seen = (g.snScene || g.snRemarks) ? Seen(man, true) : std::string{};
            const Colour::Snapshot s = seen.empty() ? Colour::Snapshot{} : Colour::SnapshotOf(man);
            const bool bulgeShows = !BulgeSeen(man).empty();
            const bool shows = !seen.empty() && (bulgeShows || s.gape > 0.15f || s.tender > 0.25f || s.wet > 0.2f || s.swell > 0.25f || s.schlong > 0.15f);
            // 2.5o: the bulge this round - how far it went, firsts, people watching, the partner's memory of it after
            {
                const Engine::Bulge b = Engine::BulgeOf(id);
                BulgeRound fin{};
                bool finished = false, watch = false;
                std::uint32_t fresh = 0;
                if (b.on) fresh = Kinds::OnReach(id, b.reach, b.deep && b.reach > 6.2f);
                {
                    std::lock_guard lk(g_mx);
                    auto& r = g_bulgeRound[id];
                    if (b.on) {
                        if (!r.active) r = BulgeRound{true};
                        r.most = std::max(r.most, b.reach); r.deep = r.deep || (b.deep && b.reach > 6.2f);
                        if (b.partner) r.partner = b.partner;
                        r.shape = b.shape; r.offAt = now;
                        if (!r.watched && (r.most > 4.2f || r.deep)) { r.watched = true; watch = true; }
                    } else if (r.active && now - r.offAt > 10.0) { fin = r; finished = true; r.active = false; }
                }
                RE::Actor* partner = RE::TESForm::LookupByID<RE::Actor>(finished ? fin.partner : b.partner);
                if ((fresh & Kinds::kFirstDeep) && g.firsts && partner) {
                    const std::string t = std::format("{} took {} so deep that it bulged almost up to his chest - deeper than anything before.", Name(partner), Name(man));
                    if (g.snMemory) Remember(t, partner, man);
                    std::lock_guard lk(g_mx);
                    g_react.push_back({id, partner->GetFormID(), t, now});
                }
                if (finished && g.bulgeMem && partner && fin.most > 2.4f) {
                    const Kinds::Kind k = Kinds::KindOf(partner);
                    const std::string by = k.key.starts_with("creature:") ? std::format("The {}'s cock", k.key.substr(9)) : std::format("{}'s cock", Name(partner));
                    Remember(std::format("{} had {}'s belly bulging {} - its shape plain to see through his skin.", by, Name(man), ReachWords(fin.most, fin.deep)), partner, man);
                }
                if (watch && g.watchBulge && g.snRemarks && Rand01() <= g.remarkChance) {
                    RE::Actor* who = nullptr; float best = 900.0f;
                    for (auto* o : all) {
                        if (!o || o == man || o == partner || o->IsDead() || o->IsChild() || o->IsInCombat() || o->IsPlayerRef()) continue;
                        const float d = o->GetPosition().GetDistance(man->GetPosition());
                        bool los = false;
                        if (d < best && o->HasLineOfSight(man, los) && los) { best = d; who = o; }
                    }
                    const std::string bs = BulgeSeen(man);
                    if (who && !bs.empty()) { ++g_sentRemark; Narrate(std::format("{} watches as {}", Name(who), bs), who, man); }
                }
            }
            if (g.snCum && g.snRemarks) {   // 2.6f: someone nearby sees it pour out of him
                const OStrandsLink::Held h = OStrandsLink::HeldOf(id);
                bool fresh = false;
                {
                    std::lock_guard lk(g_mx);
                    double& seenAt = g_floodSeen[id];
                    if (h.floodAt > seenAt) { seenAt = h.floodAt; fresh = true; }
                }
                if (fresh && Rand01() <= g.remarkChance) {
                    RE::Actor* who = nullptr; float best = 900.0f;
                    for (auto* o : all) {
                        if (!o || o == man || o->IsDead() || o->IsChild() || o->IsInCombat() || o->IsPlayerRef()) continue;
                        const float d = o->GetPosition().GetDistance(man->GetPosition());
                        bool los = false;
                        if (d < best && o->HasLineOfSight(man, los) && los) { best = d; who = o; }
                    }
                    if (who) {
                        ++g_sentRemark;
                        Narrate(std::format("{} watches as cum floods out of {}'s hole and down his thighs.", Name(who), Name(man)), who, man);
                        if (g.snMemory) Remember(std::format("{} saw cum pour out of {}'s hole in a stream, more than one man should hold.", Name(who), Name(man)), who, man);   // 2.6g: to tease him with later
                    }
                }
            }
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
        Tr::TextWrapped("Anatomica with other mods. Each part only does something when its mod is installed.");

        const bool offSN = !HasSkyrimNet(), offFame = !HasSLSF(), offAr = !ArousalFaction();
        if (Section(Title("SkyrimNet", "sn", offSN).c_str())) {
        if (offSN) Missing("SkyrimNet");
        else {
            // is it working? (this session)
            const ImGuiMCP::ImVec4 good{0.5f, 0.9f, 0.5f, 1.0f}, bad{1.0f, 0.55f, 0.4f, 1.0f};
            if (g_regAt <= 0.0) Tr::TextColored(bad, "Not registered with SkyrimNet yet this session (it happens a few seconds after loading a save).");
            else {
                const bool all = g_decOk == g_decAsk && g_actOk == g_actAsk;
                Tr::TextColored(all ? good : bad, "SkyrimNet accepted %d of %d status functions and %d of %d actions (%.0f s ago).",
                                      g_decOk.load(), g_decAsk.load(), g_actOk.load(), g_actAsk.load(), Now() - g_regAt);
            }
            Tr::TextDisabled("Since loading: SkyrimNet asked for his state %d / what is seen %d / what partners know %d times; actions done %d.",
                                   g_callsSelf.load(), g_callsSeen.load(), g_callsKnows.load(), g_actRun.load());
            Tr::TextDisabled("Sent: memories %d, reactions and remarks %d, scene notes %d.", g_sentMem.load(), g_sentReact.load(), g_sentScene.load());
            Help("SkyrimNet only asks for a man's state when it writes a prompt about him (when someone talks to or about "
                 "him), so the first numbers stay at 0 until then.");
            static std::string pv[4];
            static std::string who;
            if (Tr::Button("Show what SkyrimNet gets for the selected man")) {
                RE::Actor* a = nullptr;
                if (auto sel = RE::Console::GetSelectedRef()) a = sel->As<RE::Actor>();
                if (!a) a = RE::PlayerCharacter::GetSingleton();
                who = Name(a);
                pv[0] = Self(a); pv[1] = Seen(a, true); pv[2] = Knows(a); pv[3] = Kinds::Rumour(a, g.rumourMin);
            }
            Help("The man you click in the console (~), or you. Exactly the text SkyrimNet's prompts get.");
            if (!who.empty()) {
                Tr::TextWrapped("%s - what he knows: %s", who.c_str(), pv[0].empty() ? "(nothing)" : pv[0].c_str());
                Tr::TextWrapped("What others see (naked only): %s", pv[1].empty() ? "(nothing - dressed, or nothing shows)" : pv[1].c_str());
                Tr::TextWrapped("What he remembers of men he has had: %s", pv[2].empty() ? "(nothing)" : pv[2].c_str());
                Tr::TextWrapped("Rumours where he is: %s", pv[3].empty() ? "(nothing)" : pv[3].c_str());
            }
        }
        ImGuiMCP::BeginDisabled(offSN);
        ch |= Tr::Checkbox("Use SkyrimNet", &g.sn);
        if (g.sn) {
            ImGuiMCP::Indent();
            ch |= Tr::Checkbox("What people know and see", &g.snStatus);
            Help("A man knows his own state and history; men who have had him remember it; anyone who sees him naked sees "
                 "how his hole looks right now (tight or used, gaping, red, wet). Needs the Anatomica prompt pack (installed "
                 "with the mod).");
            ch |= Tr::Checkbox("Memories", &g.snMemory);
            Help("When he loses his virginity both remember who took it and when; when his body tightens back up he "
                 "remembers that (quietly - nobody reacts).");
            ch |= Tr::Checkbox("Reaction after his first time", &g.snReact);
            Help("Once the round is over, the man who took it says something about it - aware whether it was his real "
                 "first time or a restored one.");
            Tr::TextUnformatted("Things NPCs can choose to do:");
            ImGuiMCP::Indent();
            ch |= Tr::Checkbox("Inspect his hole", &g.actInspect);
            ch |= Tr::Checkbox("Lube him up", &g.actLube);
            ch |= Tr::Checkbox("Order him to clench or relax", &g.actClench);
            ch |= Tr::Checkbox("Ask if he is a virgin", &g.actAsk);
            ImGuiMCP::Unindent();
            Help("Only offered when it makes sense: a naked man close by (asking does not need him naked).");
            ch |= Tr::Checkbox("Scene awareness", &g.snScene);
            Help("While he is naked and it shows (gaping, red, wet...), people nearby are told what they can see. It goes "
                 "away when he recovers or dresses.");
            ch |= Tr::Checkbox("People comment on it", &g.snRemarks);
            if (g.snRemarks) {
                ImGuiMCP::Indent();
                ch |= Tr::SliderFloat("At most every (minutes, per man)", &g.remarkMinutes, 0.5f, 60.0f, "%.1f");
                float pct = g.remarkChance * 100.0f;
                if (Tr::SliderFloat("Chance", &pct, 0.0f, 100.0f, "%.0f%%")) { g.remarkChance = pct / 100.0f; ch = true; }
                ImGuiMCP::Unindent();
            }
            // 2.5o
            Tr::SeparatorText("Belly bulge and what he takes");
            ch |= Tr::Checkbox("The belly bulge", &g.snBulge);
            Help("While the bulge shows, people see its shape and how far up it goes (a knot, a flared head, almost up to his "
                 "chest...), and he feels it. Shown whenever the bulge itself shows (its \"Shows through\" setting, During sex "
                 "page), and the imprint after.");
            ch |= Tr::Checkbox("\"Made for it\"", &g.madeFor);
            Help(Mnc::Active() ? "After he has taken one kind often enough - knotted, flared... or one kind of creature (wolves, "
                                 "horses...) - he, and the men who have had him, know he takes them like he was made for them. It "
                                 "fades if he goes long enough without."
                               : "After he has taken one kind of schlong often enough - knotted, flared, ridged... - he, and the "
                                 "men who have had him, know he takes them like he was made for them. It fades if he goes long "
                                 "enough without.");
            if (g.madeFor) {
                ImGuiMCP::Indent();
                ch |= Tr::SliderInt("After this many times", &g.madeForTimes, 2, 100);
                ch |= Tr::SliderFloat("Fades after (game days without)", &g.madeForForget, 2.0f, 365.0f, "%.0f");
                ImGuiMCP::Unindent();
            }
            ch |= Tr::Checkbox("What stretched him", &g.looseFrom);
            Help(Mnc::Active() ? "While he is still loose or gaping, he knows (and it can be seen) what did it - \"still loose from "
                                 "the wolves last night\"."
                               : "While he is still loose or gaping, he knows (and it can be seen) who did it - \"still loose from "
                                 "Farkas last night\".");
            ch |= Tr::Checkbox("Partners remember the bulge", &g.bulgeMem);
            ch |= Tr::Checkbox("Cum in him (OStrands)", &g.snCum);
            Help("With the OStrands link: people see it dribbling or pouring out of him and his belly full of it, he feels it, "
                 "someone watching may remark on a flood, the man who filled him remembers it, and his first Colossal Load is a first.");
            Help("After a round where the bulge went past his lower belly, the one who gave it to him remembers how far it went.");
            ch |= Tr::Checkbox("Firsts", &g.firsts);
            Help(Mnc::Active() ? "His first creature, first knot, first flared cock, the biggest he has taken and his first super "
                                 "deep: each becomes a memory, and the partner says something after."
                               : "His first knot, first flared cock, the biggest he has taken and his first super deep: each "
                                 "becomes a memory, and the partner says something after.");
            ch |= Tr::Checkbox("People watching react to the bulge", &g.watchBulge);
            Help("When it goes past his navel (or super deep), someone watching may say something - uses \"People comment on "
                 "it\" and its chance.");
            ch |= Tr::Checkbox("Rumours", &g.rumours);
            Help(Mnc::Active() ? "Where it keeps happening, people talk: \"Around Whiterun people whisper that he lets wolves have "
                                 "him.\" Heard by anyone talking to him in that hold. Needs the Anatomica prompt pack."
                               : "Where it keeps happening, people talk: \"Around Whiterun it is said that he has a taste for "
                                 "knotted cocks.\" Heard by anyone talking to him in that hold. Needs the Anatomica prompt pack.");
            if (g.rumours) {
                ImGuiMCP::Indent();
                ch |= Tr::SliderInt("After this many times there", &g.rumourMin, 1, 50);
                ImGuiMCP::Unindent();
            }
            if (Tr::Button("Register with SkyrimNet again")) RegisterSN();
            Help("Done on every load. Use this if you switched SkyrimNet parts on in the middle of a game.");
            ImGuiMCP::Unindent();
        }
        ImGuiMCP::EndDisabled();
        }
        ch |= Size::RenderSection();
        ch |= Extras::RenderRevealing();   // revealing armour (main mod)
        if (Section(Title("SLSF Reloaded (fame)", "slsf", offFame).c_str())) {
        if (offFame) Missing("SLSF Reloaded");
        ImGuiMCP::BeginDisabled(offFame);
        ch |= Tr::Checkbox("Being taken adds Anal fame", &g.fame);
        Help("Where it happens, each time you (the player) are taken. SLSF fame belongs to the player only.");
        if (g.fame) {
            ImGuiMCP::Indent();
            ch |= Tr::SliderInt("Fame, at least", &g.fameLo, 0, 50);
            ch |= Tr::SliderInt("Fame, at most", &g.fameHi, 1, 100);
            if (g.fameHi < g.fameLo) g.fameHi = g.fameLo;
            ch |= Tr::SliderFloat("Your first time counts", &g.fameFirst, 1.0f, 10.0f, "x%.1f");
            ch |= Tr::Checkbox("Several men in a row: Group fame too", &g.fameGroup);
            if (Mnc::Active()) ch |= Tr::Checkbox("Creatures: Bestiality fame too", &g.slsfBeast);   // 2.5o (More Nasty Critters only)
            ImGuiMCP::Unindent();
        }
        ImGuiMCP::EndDisabled();
        }
        if (Section(Title("Arousal (SexLab Aroused / OSL Aroused / SLO Aroused NG)", "arousal", offAr).c_str())) {
        if (offAr) Missing("An arousal mod");
        ImGuiMCP::BeginDisabled(offAr);
        ch |= Tr::Checkbox("Arousal makes him wet and twitchy", &g.arousal);
        if (g.arousal) {
            ImGuiMCP::Indent();
            ch |= Tr::SliderInt("From arousal", &g.arousalFrom, 0, 99);
            float pct = g.arousalWet * 100.0f;
            if (Tr::SliderFloat("How wet at full arousal", &pct, 0.0f, 100.0f, "%.0f%%")) { g.arousalWet = pct / 100.0f; ch = true; }
            ch |= Tr::Checkbox("Twitchier", &g.arousalSens);
            Help("Sharper clenches and slower, heavier breathing of the hole - uses \"Sensitive while tender\" (After sex page).");
            ch |= Tr::SliderInt("His first time arouses him", &g.exposureFirst, 0, 100);
            ch |= Tr::SliderInt("Every time he is taken", &g.exposureEach, 0, 50);
            ImGuiMCP::Unindent();
        }
        ImGuiMCP::EndDisabled();
        }
        if (Section("Bathing")) {
        ch |= Tr::Checkbox("Washing", &g.bath);
        Help("Bathing in Skyrim (or any bath / wash / soap effect).");
        if (g.bath) {
            ImGuiMCP::Indent();
            ch |= Tr::Checkbox("Dries him (and the giver's schlong)", &g.bathDry);
            float pct = g.bathSoothe * 100.0f;
            if (Tr::SliderFloat("Soothes the redness by", &pct, 0.0f, 100.0f, "%.0f%%")) { g.bathSoothe = pct / 100.0f; ch = true; }
            ch |= Tr::SliderFloat("Counts as rest", &g.bathHours, 0.0f, 48.0f, "%.1f h");
            Help("Like a healing potion: hours off his waits (virginity coming back, looseness, the first-time reaction - "
                 "what counts is set on the Virginity page).");
            ImGuiMCP::Unindent();
        }
        }
        ch |= Mu::RenderSection();   // 2.5v
        ch |= OStrandsLink::RenderSection();   // 2.5w
        ch |= Colossal::RenderSection();       // 2.6c
        Colour::RenderOStrandsCum();           // 2.6f
        if (ch) Colour::SaveNow();
    }
}

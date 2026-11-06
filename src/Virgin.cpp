#include "Virgin.h"
#include "Colour.h"
#include "Bridge.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <functional>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// Anal virginity and experience. See Virgin.h.
//
// Threads: For() is called by the gape engine and the colour code (under their own locks); this file never calls
// back into them while holding its own lock. OVirginity is only ever called through the SKSE task queue.

namespace {
    enum Group { kPlayer = 0, kFollower = 1, kOther = 2 };
    enum Return { kOnce = 0, kDays = 1, kSmart = 2 };
    const char* kGroupName[3] = {"You", "Your followers", "Everyone else"};
    const char* kGroupKey[3] = {"Player", "Followers", "Others"};

    struct GroupSet {
        bool on = true;
        int ret = kSmart;      // how virginity comes back
        int amount = 3;        // kDays: after this long without being taken ...
        bool inHours = false;  // ... counted in hours (1-72) instead of days (1-30)
        float chance = 0.35f;  // a man of this group starts as a virgin (without OVirginity)
    };
    struct Settings {
        bool on = true;
        GroupSet grp[3] = {{true, kSmart, 3, false, 1.0f}, {true, kSmart, 3, false, 0.4f}, {true, kSmart, 3, false, 0.35f}};
        bool useOV = true;         // ask / tell OVirginity when it is installed
        bool tight = true;         // tightness follows experience
        bool rim = true;           // the rim looks like his experience
        bool first = true;         // stronger reaction to his first time
        bool clench = true;        // clenching follows experience
        bool memory = true;        // a regular partner loosens him less
        float firstDays = 1.0f;    // how long the first-time reaction lasts (game days)
        // healing potions and spells take time off his waits (virginity coming back, lasting looseness, first-time reaction)
        bool potions = true;  float potionHours = 6.0f; bool potionScale = true; bool food = false;
        bool spells = false;  float spellHours = 3.0f;   // off by default: NPCs get all kinds of healing effects from other mods
        float cooldown = 30.0f;    // seconds before the next one counts (no chugging ten minor potions)
        bool healVirgin = true, healLoose = true, healFirst = true;
    } g;

    struct Man {
        std::int8_t virgin = -1;   // 1 yes, 0 no, -1 not known yet
        float exp = 0.0f;          // experience (about one point per time he is taken)
        std::uint32_t times = 0, lost = 0, back = 0;   // times taken / virginities lost / virginities come back
        float lastDay = -1.0f;     // game day he was last taken
        float firstDay = -1.0f;    // game day of his (latest) first time
        float decayDay = -1.0f;    // smart: experience worn off up to this day
        std::vector<std::pair<RE::FormID, std::uint16_t>> partners;   // who took him, how often
        RE::FormID firstBy = 0;       // who took it the latest time (0 = unknown / nobody yet)
        RE::FormID firstEverBy = 0;   // who took it the very first time
        // this session only
        double workedAt = 0.0;
        RE::FormID roundPartner = 0;
        bool asked = false;
        std::uint32_t timesAtAsk = 0;
    };

    std::mutex g_mx;
    std::unordered_map<RE::FormID, Man> g_men;
    std::uint32_t g_seed = 0;
    constexpr std::uint32_t kRec = 'VRGN';

    double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    float Today() { auto* c = RE::Calendar::GetSingleton(); return c ? c->GetDaysPassed() : 0.0f; }
    void NewSeed() { g_seed = std::random_device{}(); if (!g_seed) g_seed = 0x9E3779B9u; }

    float Roll(RE::FormID id) {   // the same answer every time for the same man in the same game
        std::uint32_t h = (id ^ g_seed) * 2654435761u;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
        return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0x1000000);
    }

    int GroupOf(RE::Actor* a) {
        if (!a) return kOther;
        if (a->IsPlayerRef()) return kPlayer;
        if (a->IsPlayerTeammate()) return kFollower;
        static RE::TESFaction* follower = RE::TESForm::LookupByID<RE::TESFaction>(0x0005C84E);   // CurrentFollowerFaction
        if (follower && a->IsInFaction(follower)) return kFollower;
        return kOther;
    }
    bool Covered(RE::Actor* a) { return g.on && a && g.grp[GroupOf(a)].on; }

    // ---- OVirginity (optional): its own chances and lists decide who starts a virgin, and it hears when that changes
    RE::TESQuest* OVQuest() {
        static bool tried = false;
        static RE::TESQuest* q = nullptr;
        if (!tried) {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return nullptr;
            tried = true;
            if (dh->LookupLoadedModByName("OVirginity.esp")) q = dh->LookupForm<RE::TESQuest>(0x800, "OVirginity.esp");
            logger::info("Anatomica virginity: OVirginity {}", q ? "found" : "not installed");
        }
        return q;
    }
    bool OVOn() { return g.useOV && OVQuest(); }

    struct OVAnswer : RE::BSScript::IStackCallbackFunctor {
        RE::FormID id = 0;
        std::uint32_t timesAtAsk = 0;
        void operator()(RE::BSScript::Variable r) override {
            if (!r.IsBool()) return;
            std::lock_guard lk(g_mx);
            auto it = g_men.find(id);
            if (it == g_men.end()) return;
            Man& m = it->second;
            if (m.times != timesAtAsk || m.lastDay >= 0.0f) return;   // he has a history with us by now: ours stands
            m.virgin = r.GetBool() ? 1 : 0;
        }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
    };

    // what = "ask" (isVirgin), "virgin" / "taken" (setVirginity), "list" (add to the virginities-taken list)
    void OVCall(RE::FormID id, std::string what, std::uint32_t timesAtAsk = 0) {
        if (!OVOn()) return;
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;
        tasks->AddTask([id, what, timesAtAsk]() {
            auto* q = OVQuest();
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!q || !a || !vm) return;
            auto* policy = vm->GetObjectHandlePolicy();
            if (!policy) return;
            const RE::VMHandle h = policy->GetHandleForObject(RE::TESQuest::FORMTYPE, q);
            if (h == policy->EmptyHandle()) return;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
            if (what == "ask") {
                auto ans = RE::make_smart<OVAnswer>();
                ans->id = id; ans->timesAtAsk = timesAtAsk;
                cb = ans;
                vm->DispatchMethodCall(h, "OVirginityScript", "isVirgin", RE::MakeFunctionArguments(static_cast<RE::Actor*>(a)), cb);
            } else if (what == "virgin" || what == "taken") {
                vm->DispatchMethodCall(h, "OVirginityScript", "setVirginity", RE::MakeFunctionArguments(static_cast<RE::Actor*>(a), what == "virgin", false), cb);
            } else if (what == "list") {
                vm->DispatchMethodCall(h, "OVirginityScript", "addActorToVirginitiesTakenList", RE::MakeFunctionArguments(static_cast<RE::Actor*>(a)), cb);
            }
        });
    }

    // ---- his state
    struct Call { RE::FormID id; std::string what; std::uint32_t times = 0; };
    struct After { std::vector<Call> calls; std::vector<std::function<void()>> later; };   // calls to make once unlocked

    void Resolve(Man& m, RE::FormID id, int grp, After& out) {
        if (m.virgin == -1) m.virgin = Roll(id) < g.grp[grp].chance ? 1 : 0;
        if (!m.asked && OVOn()) {   // once a session: a man we have no history with takes OVirginity's word for it
            m.asked = true;
            if (m.lastDay < 0.0f) { m.timesAtAsk = m.times; out.calls.push_back({id, "ask", m.times}); }
            else out.calls.push_back({id, m.virgin == 1 ? "virgin" : "taken"});   // ours stands: tell it
        }
    }

    float WaitDays(const GroupSet& gs) {
        return gs.inHours ? static_cast<float>(std::clamp(gs.amount, 1, 72)) / 24.0f : static_cast<float>(std::clamp(gs.amount, 1, 30));
    }
    std::string DaysHours(float d) {
        d = std::max(0.0f, d);
        const int total = static_cast<int>(std::lround(d * 24.0f));
        const int dd = total / 24, hh = total % 24;
        if (dd == 0) return std::format("{} hour{}", hh, hh == 1 ? "" : "s");
        return hh ? std::format("{} day{} {} hour{}", dd, dd == 1 ? "" : "s", hh, hh == 1 ? "" : "s") : std::format("{} day{}", dd, dd == 1 ? "" : "s");
    }

    // virginity coming back while he rests
    void ApplyReturn(Man& m, RE::FormID id, int grp, After& out) {
        if (m.virgin != 0 || m.lastDay < 0.0f) return;
        const GroupSet& gs = g.grp[grp];
        const float today = Today();
        const float rest = today - m.lastDay;
        bool back = false;
        if (gs.ret == kDays) back = rest >= WaitDays(gs);
        else if (gs.ret == kSmart) {
            // a day's grace, then experience wears off - slower for a man who has been taken many times
            const float from = std::max(m.decayDay, m.lastDay + 1.0f);
            if (today > from) {
                const float rate = 1.0f / (1.0f + static_cast<float>(m.times) / 30.0f);
                m.exp = std::max(0.0f, m.exp - rate * (today - from));
                m.decayDay = today;
            }
            back = m.exp <= 0.0f && rest >= 3.0f;
        }
        if (!back) return;
        m.virgin = 1; m.exp = 0.0f; m.firstDay = -1.0f; ++m.back;
        out.calls.push_back({id, "virgin"});
        out.later.push_back([id]() { Bridge::Restored(id); });
        logger::info("Anatomica virginity: {:08X} is a virgin again", id);
    }

    void Flush(After& out) {
        for (auto& c : out.calls) OVCall(c.id, c.what, c.times);
        for (auto& f : out.later) f();   // SkyrimNet / SLSF / arousal (Bridge.cpp)
    }

    void Taken(Man& m, RE::Actor* a, RE::Actor* partner, After& out) {
        const RE::FormID pid = partner ? partner->GetFormID() : 0;
        const bool wasVirgin = m.virgin == 1;
        ++m.times;
        float gain = 1.0f;
        if (pid) {
            auto it = std::find_if(m.partners.begin(), m.partners.end(), [&](auto& p) { return p.first == pid; });
            const int seen = it == m.partners.end() ? 0 : it->second;
            if (g.memory) gain = seen >= 3 ? 0.5f : (seen == 0 ? 1.5f : 1.0f);   // a new man takes more out of him than a regular
            if (it == m.partners.end()) {
                if (m.partners.size() >= 32) m.partners.erase(std::min_element(m.partners.begin(), m.partners.end(), [](auto& x, auto& y) { return x.second < y.second; }));
                m.partners.push_back({pid, 1});
            } else if (it->second < 60000) ++it->second;
        }
        m.exp += gain;
        const float today = Today();
        {   // fame / arousal / memories for every time he is taken (Bridge decides what is switched on)
            const RE::FormID man = a->GetFormID();
            const RE::FormID prevBy = m.firstBy, everBy = m.firstEverBy;
            const std::uint32_t lostBefore = m.lost;
            out.later.push_back([man, pid, wasVirgin, prevBy, everBy, lostBefore]() { Bridge::Taken(man, pid, wasVirgin, lostBefore, prevBy, everBy); });
        }
        if (wasVirgin) {
            m.firstBy = pid;
            if (m.lost == 0 || !m.firstEverBy) m.firstEverBy = pid;
            m.virgin = 0; ++m.lost; m.firstDay = today;
            out.calls.push_back({a->GetFormID(), "taken"});
            if (partner && partner->IsPlayerRef()) out.calls.push_back({a->GetFormID(), "list"});
            logger::info("Anatomica virginity: {:08X} lost his virginity{}", a->GetFormID(), partner ? std::format(" to {:08X}", pid) : "");
        }
        m.lastDay = today; m.decayDay = today;
    }

    // ---- healing potions and spells: each takes some hours off his waits
    std::unordered_map<RE::FormID, double> g_healAt, g_potionAt;

    bool HealthRestore(const RE::EffectSetting* mgef) {
        if (!mgef || mgef->IsHostile() || mgef->data.primaryAV != RE::ActorValue::kHealth) return false;
        const auto arch = mgef->GetArchetype();
        return arch == RE::EffectSetting::Archetype::kValueModifier || arch == RE::EffectSetting::Archetype::kPeakValueModifier ||
               arch == RE::EffectSetting::Archetype::kDualValueModifier;
    }
    bool IsMan(RE::Actor* a) {
        const auto* npc = a ? a->GetActorBase() : nullptr;
        return npc && npc->GetSex() == RE::SEX::kMale;
    }

    void Rest(RE::Actor* a, float hours, const char* what) {
        if (!a || hours <= 0.0f || !g.on) return;
        const RE::FormID id = a->GetFormID();
        const double now = Now();
        const bool covered = Covered(a);
        {
            std::lock_guard lk(g_mx);
            double& at = g_healAt[id];
            if (at > 0.0 && now - at < g.cooldown) return;
            at = now;
            if (covered)
                if (auto it = g_men.find(id); it != g_men.end()) {
                    Man& m = it->second;
                    const float d = hours / 24.0f;
                    if (g.healVirgin && m.virgin == 0) {   // as if he had rested that much longer
                        if (m.lastDay >= 0.0f) m.lastDay -= d;
                        if (m.decayDay >= 0.0f) m.decayDay -= d;
                    }
                    if (g.healFirst && m.firstDay >= 0.0f) m.firstDay -= d;
                }
        }
        if (g.healLoose) Colour::RestHours(id, hours);
        logger::info("Anatomica virginity: {} took {:.1f} hours off {:08X}'s waits", what, hours, id);
    }

    struct EquipSink : RE::BSTEventSink<RE::TESEquipEvent> {   // drinking a potion is "equipping" it
        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* e, RE::BSTEventSource<RE::TESEquipEvent>*) override {
            if (!e || !e->equipped || !e->actor || !g.on || !g.potions) return RE::BSEventNotifyControl::kContinue;
            auto* item = RE::TESForm::LookupByID<RE::AlchemyItem>(e->baseObject);
            if (!item || item->IsPoison() || (item->IsFood() && !g.food)) return RE::BSEventNotifyControl::kContinue;
            auto* a = e->actor->As<RE::Actor>();
            if (!IsMan(a)) return RE::BSEventNotifyControl::kContinue;
            float total = 0.0f;   // how much health it gives back, all told
            for (auto* eff : item->effects)
                if (eff && HealthRestore(eff->baseEffect)) total += eff->effectItem.magnitude * static_cast<float>(std::max<std::uint32_t>(1, eff->effectItem.duration));
            if (total <= 0.0f) return RE::BSEventNotifyControl::kContinue;
            { std::lock_guard lk(g_mx); g_potionAt[a->GetFormID()] = Now(); }
            const float k = g.potionScale ? std::clamp(total / 50.0f, 0.25f, 3.0f) : 1.0f;   // minor (25) half, ultimate (100+) double
            Rest(a, g.potionHours * k, "a potion");
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    struct SpellSink : RE::BSTEventSink<RE::TESMagicEffectApplyEvent> {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* e, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override {
            if (!e || !e->target || !g.on || !g.spells) return RE::BSEventNotifyControl::kContinue;
            if (!HealthRestore(RE::TESForm::LookupByID<RE::EffectSetting>(e->magicEffect))) return RE::BSEventNotifyControl::kContinue;
            auto* a = e->target->As<RE::Actor>();
            if (!IsMan(a)) return RE::BSEventNotifyControl::kContinue;
            {   // a potion's own effect lands here too: that one was counted already
                std::lock_guard lk(g_mx);
                if (auto it = g_potionAt.find(a->GetFormID()); it != g_potionAt.end() && Now() - it->second < 2.0) return RE::BSEventNotifyControl::kContinue;
            }
            Rest(a, g.spellHours, "a healing spell");
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    EquipSink g_equipSink;
    SpellSink g_spellSink;
    void EnsureSinks() {
        static bool done = false;
        if (done) return;
        if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) {
            src->AddEventSink<RE::TESEquipEvent>(&g_equipSink);
            src->AddEventSink<RE::TESMagicEffectApplyEvent>(&g_spellSink);
            done = true;
        }
    }

    float Level(const Man& m) { return std::clamp(m.exp / 12.0f, 0.0f, 1.0f); }
    const char* LevelName(const Man& m) {
        if (m.virgin == 1) return "virgin";
        if (m.exp < 3.0f) return "inexperienced";
        if (m.exp < 10.0f) return "experienced";
        return "well used";
    }

    void Help(const char* t) {
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::PushTextWrapPos(420.0f); ImGuiMCP::TextUnformatted(t); ImGuiMCP::PopTextWrapPos(); ImGuiMCP::EndTooltip(); }
    }
}

namespace Virgin {
    void Ini(CSimpleIniA& ini, bool save) {
        const char* S = "Virginity";
        auto B = [&](const std::string& k, bool& v) { if (save) ini.SetBoolValue(S, k.c_str(), v); else v = ini.GetBoolValue(S, k.c_str(), v); };
        auto I = [&](const std::string& k, int& v, int lo, int hi) { if (save) ini.SetLongValue(S, k.c_str(), v); else v = std::clamp(static_cast<int>(ini.GetLongValue(S, k.c_str(), v)), lo, hi); };
        auto F = [&](const std::string& k, float& v, float lo, float hi) { if (save) ini.SetDoubleValue(S, k.c_str(), v); else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k.c_str(), v)), lo, hi); };
        std::lock_guard lk(g_mx);
        B("On", g.on); B("UseOVirginity", g.useOV);
        B("Tightness", g.tight); B("RimLook", g.rim); B("FirstTime", g.first); B("Clenching", g.clench); B("PartnerMemory", g.memory);
        F("FirstTimeDays", g.firstDays, 0.25f, 7.0f);
        B("Potions", g.potions); F("PotionHours", g.potionHours, 0.0f, 48.0f); B("PotionScale", g.potionScale); B("Food", g.food);
        B("Spells", g.spells); F("SpellHours", g.spellHours, 0.0f, 48.0f); F("HealCooldown", g.cooldown, 0.0f, 300.0f);
        B("HealVirginity", g.healVirgin); B("HealLooseness", g.healLoose); B("HealFirstTime", g.healFirst);
        for (int i = 0; i < 3; ++i) {
            const std::string k = kGroupKey[i];
            B(k + "On", g.grp[i].on); I(k + "Return", g.grp[i].ret, 0, 2); B(k + "InHours", g.grp[i].inHours); I(k + "Wait", g.grp[i].amount, 1, 72);
            if (!save && !g.grp[i].inHours) g.grp[i].amount = std::min(g.grp[i].amount, 30);
            F(k + "Chance", g.grp[i].chance, 0.0f, 1.0f);
        }
    }

    void Save(SKSE::SerializationInterface* a) {
        std::lock_guard lk(g_mx);
        if (!a->OpenRecord(kRec, 2)) return;
        a->WriteRecordData(&g_seed, sizeof g_seed);
        std::uint32_t n = 0;
        auto keep = [](const Man& m) { return m.virgin != -1 && (m.lastDay >= 0.0f || m.back > 0 || m.times > 0); };
        for (auto& [id, m] : g_men) if (keep(m)) ++n;
        a->WriteRecordData(&n, sizeof n);
        for (auto& [id, m] : g_men) {
            if (!keep(m)) continue;
            a->WriteRecordData(&id, sizeof id);
            a->WriteRecordData(&m.virgin, sizeof m.virgin);
            a->WriteRecordData(&m.exp, sizeof m.exp);
            a->WriteRecordData(&m.times, sizeof m.times);
            a->WriteRecordData(&m.lost, sizeof m.lost);
            a->WriteRecordData(&m.back, sizeof m.back);
            a->WriteRecordData(&m.lastDay, sizeof m.lastDay);
            a->WriteRecordData(&m.firstDay, sizeof m.firstDay);
            a->WriteRecordData(&m.decayDay, sizeof m.decayDay);
            a->WriteRecordData(&m.firstBy, sizeof m.firstBy);
            a->WriteRecordData(&m.firstEverBy, sizeof m.firstEverBy);
            const std::uint16_t np = static_cast<std::uint16_t>(std::min<size_t>(m.partners.size(), 64));
            a->WriteRecordData(&np, sizeof np);
            for (std::uint16_t i = 0; i < np; ++i) {
                a->WriteRecordData(&m.partners[i].first, sizeof(RE::FormID));
                a->WriteRecordData(&m.partners[i].second, sizeof(std::uint16_t));
            }
        }
    }

    bool Load(SKSE::SerializationInterface* a, std::uint32_t type, std::uint32_t ver, std::uint32_t) {
        if (type != kRec) return false;
        if (ver != 1 && ver != 2) return true;
        std::lock_guard lk(g_mx);
        g_men.clear();
        a->ReadRecordData(&g_seed, sizeof g_seed);
        std::uint32_t n = 0;
        a->ReadRecordData(&n, sizeof n);
        for (std::uint32_t k = 0; k < n; ++k) {
            RE::FormID id = 0;
            Man m;
            a->ReadRecordData(&id, sizeof id);
            a->ReadRecordData(&m.virgin, sizeof m.virgin);
            a->ReadRecordData(&m.exp, sizeof m.exp);
            a->ReadRecordData(&m.times, sizeof m.times);
            a->ReadRecordData(&m.lost, sizeof m.lost);
            a->ReadRecordData(&m.back, sizeof m.back);
            a->ReadRecordData(&m.lastDay, sizeof m.lastDay);
            a->ReadRecordData(&m.firstDay, sizeof m.firstDay);
            a->ReadRecordData(&m.decayDay, sizeof m.decayDay);
            if (ver >= 2) {
                RE::FormID fb = 0, fe = 0, r = 0;
                a->ReadRecordData(&fb, sizeof fb);
                a->ReadRecordData(&fe, sizeof fe);
                if (fb && a->ResolveFormID(fb, r)) m.firstBy = r;
                if (fe && a->ResolveFormID(fe, r)) m.firstEverBy = r;
            }
            std::uint16_t np = 0;
            a->ReadRecordData(&np, sizeof np);
            for (std::uint16_t i = 0; i < np; ++i) {
                RE::FormID pid = 0; std::uint16_t c = 0;
                a->ReadRecordData(&pid, sizeof pid);
                a->ReadRecordData(&c, sizeof c);
                RE::FormID now = 0;
                if (a->ResolveFormID(pid, now)) m.partners.push_back({now, c});
            }
            RE::FormID now = 0;
            if (a->ResolveFormID(id, now)) g_men[now] = std::move(m);
        }
        logger::info("Anatomica virginity: {} men loaded", g_men.size());
        return true;
    }

    void Revert() {
        std::lock_guard lk(g_mx);
        g_men.clear();
        NewSeed();
    }

    void Watch(RE::Actor* a) {
        EnsureSinks();
        if (!Covered(a)) return;
        After out;
        {
            std::lock_guard lk(g_mx);
            auto& m = g_men[a->GetFormID()];
            const int grp = GroupOf(a);
            Resolve(m, a->GetFormID(), grp, out);
            ApplyReturn(m, a->GetFormID(), grp, out);
        }
        Flush(out);
    }

    void OnWorked(RE::Actor* a, RE::Actor* partner) {
        if (!Covered(a)) return;
        if (partner == a) partner = nullptr;
        After out;
        {
            std::lock_guard lk(g_mx);
            Man& m = g_men[a->GetFormID()];
            const int grp = GroupOf(a);
            const double now = Now();
            const RE::FormID pid = partner ? partner->GetFormID() : 0;
            const bool newRound = m.workedAt <= 0.0 || now - m.workedAt > 60.0;
            const bool newMan = !newRound && pid && pid != m.roundPartner;   // the next one in a row
            if (newRound || newMan) {
                Resolve(m, a->GetFormID(), grp, out);
                ApplyReturn(m, a->GetFormID(), grp, out);
                Taken(m, a, partner, out);
            }
            m.workedAt = now;
            if (pid) m.roundPartner = pid;
            m.lastDay = Today();
        }
        Flush(out);
    }

    Mods For(RE::FormID id) {
        Mods o;
        auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
        if (!Covered(a)) return o;
        After out;
        {
            std::lock_guard lk(g_mx);
            auto it = g_men.find(id);
            if (it == g_men.end()) return o;
            Man& m = it->second;
            const int grp = GroupOf(a);
            Resolve(m, id, grp, out);
            ApplyReturn(m, id, grp, out);
            const bool virgin = m.virgin == 1;
            const float t = Level(m);
            if (g.tight) {   // a virgin is tight and slow to open; he loosens over his first times, not all at once
                if (virgin) { o.open = 0.6f; o.build = 1.8f; o.close = 1.4f; o.looseAdd = 0.6f; o.looseCap = 0.5f; }
                else { o.open = 0.75f + 0.35f * t; o.build = 1.4f - 0.5f * t; o.close = 1.2f - 0.4f * t; o.looseCap = 0.6f + 0.9f * t; }
            }
            if (g.memory && !virgin && m.roundPartner) {   // the round with a regular partner leaves less behind
                auto p = std::find_if(m.partners.begin(), m.partners.end(), [&](auto& x) { return x.first == m.roundPartner; });
                if (p != m.partners.end() && p->second >= 4) o.looseAdd *= 0.5f;
            }
            if (g.rim) {
                o.rim = virgin ? -1.0f : std::clamp(-0.3f + 1.3f * t, -1.0f, 1.0f);
                o.swell = std::max(0.0f, t - 0.4f) * 0.35f;
            }
            if (g.clench) o.clench = virgin ? 0.6f : 0.7f + 0.5f * t;   // a virgin clenches a lot; a used man relaxes
            if (g.first && m.firstDay >= 0.0f) {
                const float since = Today() - m.firstDay;
                if (since < g.firstDays) {
                    const float k = 1.0f - since / std::max(g.firstDays, 0.05f);
                    o.tender *= 1.0f + 0.8f * k;
                    o.tenderTime *= 1.0f + 1.0f * k;
                    o.swell += 0.25f * k;
                    o.clench *= 1.0f - 0.35f * k;
                }
            }
        }
        Flush(out);
        return o;
    }

    Info Get(RE::FormID id) {
        Info o;
        auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
        if (!Covered(a)) return o;
        Watch(a);
        std::lock_guard lk(g_mx);
        auto it = g_men.find(id);
        if (it == g_men.end() || it->second.virgin == -1) return o;
        const Man& m = it->second;
        const GroupSet& gs = g.grp[GroupOf(a)];
        o.known = true; o.virgin = m.virgin == 1; o.exp = m.exp; o.level = LevelName(m);
        o.times = m.times; o.lost = m.lost; o.back = m.back; o.partners = m.partners;
        o.firstBy = m.firstBy; o.firstEverBy = m.firstEverBy;
        const float today = Today();
        o.daysSince = m.lastDay >= 0.0f ? today - m.lastDay : -1.0f;
        o.daysSinceFirst = m.firstDay >= 0.0f ? today - m.firstDay : -1.0f;
        o.workedAgo = m.workedAt > 0.0 ? Now() - m.workedAt : -1.0;
        o.mode = gs.ret;
        if (m.virgin == 0) {
            if (gs.ret == kDays) o.backIn = std::max(0.0f, WaitDays(gs) - std::max(0.0f, o.daysSince));
            else if (gs.ret == kSmart) {
                const float rest = std::max(0.0f, o.daysSince);
                const float rate = 1.0f / (1.0f + static_cast<float>(m.times) / 30.0f);
                o.backIn = std::max(3.0f - rest, (rest < 1.0f ? 1.0f - rest : 0.0f) + m.exp / rate);
            }
        }
        return o;
    }
    void RestFor(RE::Actor* a, float hours, const char* why) { Rest(a, hours, why); }

    std::vector<Had> HadBy(RE::FormID partner) {
        std::vector<Had> out;
        if (!g.on || !partner) return out;
        std::lock_guard lk(g_mx);
        const float today = Today();
        for (auto& [id, m] : g_men)
            for (auto& [pid, c] : m.partners)
                if (pid == partner) {
                    Had h;
                    h.man = id; h.times = c; h.tookIt = m.firstBy == partner; h.tookFirst = m.firstEverBy == partner;
                    h.daysSince = m.lastDay >= 0.0f ? today - m.lastDay : -1.0f;
                    out.push_back(h);
                    break;
                }
        return out;
    }

    std::string Status(RE::Actor* a) {
        if (!a) return {};
        if (!g.on) return "Virginity: off";
        if (!Covered(a)) return std::format("Virginity: off for {}", kGroupName[GroupOf(a)]);
        Watch(a);
        std::lock_guard lk(g_mx);
        auto it = g_men.find(a->GetFormID());
        if (it == g_men.end()) return "Virginity: not known yet";
        const Man& m = it->second;
        const GroupSet& gs = g.grp[GroupOf(a)];
        std::string back;
        if (m.virgin == 0) {
            const float rest = m.lastDay >= 0.0f ? Today() - m.lastDay : 0.0f;
            if (gs.ret == kOnce) back = "for good";
            else if (gs.ret == kDays) back = "back in " + DaysHours(WaitDays(gs) - rest);
            else {
                const float rate = 1.0f / (1.0f + static_cast<float>(m.times) / 30.0f);
                const float need = std::max(3.0f - rest, (rest < 1.0f ? 1.0f - rest : 0.0f) + m.exp / rate);
                back = "back in about " + DaysHours(need) + " (smart)";
            }
        }
        return std::format("Virginity: {}{} | experience {:.1f} | taken {} time{} by {} partner{}{}", LevelName(m),
                           m.virgin == 0 ? std::format(" ({})", back) : "", m.exp, m.times, m.times == 1 ? "" : "s", m.partners.size(),
                           m.partners.size() == 1 ? "" : "s", m.back ? std::format(" | virgin again {} time{}", m.back, m.back == 1 ? "" : "s") : "");
    }

    void __stdcall RenderPage() {
        bool ch = false;
        Settings& s = g;   // the menu runs on the render thread; plain reads/writes of these flags are fine
        ch |= ImGuiMCP::Checkbox("Anal virginity and experience", &s.on);
        Help("Each man is a virgin or not, and gains experience every time he is taken. The first time and his experience "
             "show in how tight he is, how his rim looks, how he reacts and how much he clenches (each can be switched off below).");
        if (!s.on) { if (ch) Colour::SaveNow(); return; }

        ImGuiMCP::SeparatorText("OVirginity");
        if (OVQuest()) {
            ch |= ImGuiMCP::Checkbox("Use OVirginity", &s.useOV);
            Help("OVirginity is installed: its chances and its always / never virgin lists decide who starts a virgin, and "
                 "it is told when a man loses his virginity or gets it back (so ORomance, OProstitution and other mods see "
                 "it). When you are the one taking it, he goes on its \"virginities taken\" list.");
        } else ImGuiMCP::TextDisabled("OVirginity is not installed: the chances below decide who starts a virgin.");

        for (int i = 0; i < 3; ++i) {
            GroupSet& gs = s.grp[i];
            ImGuiMCP::SeparatorText(kGroupName[i]);
            ImGuiMCP::PushID(i);
            ch |= ImGuiMCP::Checkbox("On", &gs.on);
            if (gs.on) {
                float pct = gs.chance * 100.0f;
                if (ImGuiMCP::SliderFloat(i == kPlayer ? "Chance you start a virgin" : "Chance a man starts a virgin", &pct, 0.0f, 100.0f, "%.0f%%")) {
                    gs.chance = pct / 100.0f; ch = true;
                }
                if (OVOn()) Help("With OVirginity in use this is only for men it has no answer for yet.");
                const char* modes[] = {"One and done (never comes back)", "Comes back after a set time", "Smart (comes back as he tightens up again)"};
                ch |= ImGuiMCP::Combo("Virginity comes back", &gs.ret, modes, 3);
                Help("One and done: once it is gone it stays gone.\n"
                     "After a set time: he is a virgin again once that long passes (in days or hours) without anyone taking him.\n"
                     "Smart: after a day's rest his experience wears off, slower the more often he has been taken; once it "
                     "is all gone (and at least 3 days have passed) he is a virgin again. Taken once: back in about 3 days. "
                     "Well used: weeks.");
                if (gs.ret == kDays) {
                    int unit = gs.inHours ? 1 : 0;
                    const char* units[] = {"Days", "Hours"};
                    ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 7.0f);
                    if (ImGuiMCP::Combo("Count in", &unit, units, 2)) {
                        const bool h = unit == 1;
                        if (h != gs.inHours) {   // keep about the same wait
                            gs.amount = h ? std::clamp(gs.amount * 24, 1, 72) : std::clamp((gs.amount + 12) / 24, 1, 30);
                            gs.inHours = h;
                            ch = true;
                        }
                    }
                    ch |= ImGuiMCP::SliderInt(gs.inHours ? "Hours" : "Days", &gs.amount, 1, gs.inHours ? 72 : 30);
                }
            }
            ImGuiMCP::PopID();
        }

        ImGuiMCP::SeparatorText("What his virginity and experience change");
        ch |= ImGuiMCP::Checkbox("Tightness", &s.tight);
        Help("A virgin opens slowly, gapes less and snaps shut quickly; he loosens over his first few times instead of all at "
             "once. A well-used man opens easily, gapes wider and can keep more lasting looseness.");
        ch |= ImGuiMCP::Checkbox("Rim look", &s.rim);
        Help("A virgin's rim is paler and tighter-looking; a well-used man's is darker and softer, and a little puffy "
             "(the puffiness needs Swelling on the After sex page).");
        ch |= ImGuiMCP::Checkbox("First-time reaction", &s.first);
        Help("His first time: stronger reddening that heals slower, more swelling, and more clenching afterwards. With "
             "\"Sensitive while tender\" (After sex page) he also twitches more at a touch.");
        if (s.first) {
            ImGuiMCP::Indent();
            ch |= ImGuiMCP::SliderFloat("Lasts (game days)", &s.firstDays, 0.25f, 7.0f, "%.2f");
            ImGuiMCP::Unindent();
        }
        ch |= ImGuiMCP::Checkbox("Clenching", &s.clench);
        Help("A virgin clenches often; he relaxes as he gains experience.");
        ch |= ImGuiMCP::Checkbox("Partner memory", &s.memory);
        Help("A new man takes more out of him than a regular partner: rounds with a man who has had him a few times add less "
             "experience and less lasting looseness.");

        ImGuiMCP::SeparatorText("Healing potions and spells");
        ImGuiMCP::TextDisabled("Each one takes time off his waits, as if he had rested that much longer.");
        ch |= ImGuiMCP::Checkbox("Potions", &s.potions);
        if (s.potions) {
            ImGuiMCP::Indent();
            ch |= ImGuiMCP::SliderFloat("Hours off per potion", &s.potionHours, 0.0f, 48.0f, "%.1f h");
            ch |= ImGuiMCP::Checkbox("Stronger potions take more off", &s.potionScale);
            Help("Measured by how much health the potion gives back: a minor one counts half, a plentiful one as set, an "
                 "ultimate one double (up to three times).");
            ch |= ImGuiMCP::Checkbox("Food that restores health counts too", &s.food);
            ImGuiMCP::Unindent();
        }
        ch |= ImGuiMCP::Checkbox("Healing spells", &s.spells);
        if (s.spells) {
            ImGuiMCP::Indent();
            ch |= ImGuiMCP::SliderFloat("Hours off per spell", &s.spellHours, 0.0f, 48.0f, "%.1f h");
            ImGuiMCP::Unindent();
        }
        if (s.potions || s.spells) {
            ch |= ImGuiMCP::SliderFloat("Wait before the next one counts", &s.cooldown, 0.0f, 300.0f, "%.0f s");
            ImGuiMCP::TextUnformatted("Takes time off:");
            ImGuiMCP::Indent();
            ch |= ImGuiMCP::Checkbox("His virginity coming back", &s.healVirgin);
            Help("After a set time: the wait gets shorter. Smart: counts as rest, so his experience wears off sooner. "
                 "One and done: no effect.");
            ch |= ImGuiMCP::Checkbox("Lasting looseness healing", &s.healLoose);
            Help("Lasting looseness (After sex page) heals a little every game day. A potion or spell counts as that many "
                 "extra hours of healing: with \"Heals per game day\" at 5%, a potion taking 6 hours off heals about "
                 "1.25% of looseness on top. It never adds looseness, only takes it away faster.");
            ch |= ImGuiMCP::Checkbox("The first-time reaction", &s.healFirst);
            Help("After losing his virginity he is redder, more swollen and clenches more for a while (\"Lasts\", above, "
                 "in game days). A potion or spell makes that time run out sooner by the hours it takes off - it eases him "
                 "back to normal faster. His virginity stays lost either way.");
            ImGuiMCP::Unindent();
            ImGuiMCP::TextDisabled("Redness, bruising and swelling: After sex page (healing clears them).");
        }
        if (ch) Colour::SaveNow();
    }

    void RenderTests(RE::Actor* a) {
        static std::string last;
        if (!a) return;
        auto act = [&](const char* label, auto fn) { if (ImGuiMCP::Button(label)) last = fn(); };
        act("Make him a virgin", [&] {
            After out;
            { std::lock_guard lk(g_mx); auto& m = g_men[a->GetFormID()]; m.virgin = 1; m.exp = 0.0f; m.firstDay = -1.0f; m.asked = true; out.calls.push_back({a->GetFormID(), "virgin"}); }
            Flush(out);
            return std::string("a virgin again");
        });
        ImGuiMCP::SameLine();
        act("Take his virginity", [&] {
            After out;
            { std::lock_guard lk(g_mx); auto& m = g_men[a->GetFormID()]; Resolve(m, a->GetFormID(), GroupOf(a), out); m.virgin = 1; Taken(m, a, nullptr, out); }
            Flush(out);
            return std::string("his first time, just now");
        });
        ImGuiMCP::SameLine();
        act("+5 experience", [&] { std::lock_guard lk(g_mx); auto& m = g_men[a->GetFormID()]; if (m.virgin != 0) m.virgin = 0; m.exp += 5.0f; m.times += 5; if (m.lastDay < 0.0f) m.lastDay = Today(); return std::format("experience {:.1f}", m.exp); });
        ImGuiMCP::SameLine();
        act("Pretend 5 days passed", [&] {
            std::lock_guard lk(g_mx);
            auto& m = g_men[a->GetFormID()];
            for (float* d : {&m.lastDay, &m.firstDay, &m.decayDay}) if (*d >= 0.0f) *d -= 5.0f;
            return std::string("5 days of rest (as if)");
        });
        ImGuiMCP::SameLine();
        act("Forget him", [&] { std::lock_guard lk(g_mx); g_men.erase(a->GetFormID()); return std::string("rolled again next time"); });
        ImGuiMCP::TextUnformatted(Status(a).c_str());
        if (!last.empty()) ImGuiMCP::TextDisabled("Virginity: %s", last.c_str());
    }
}

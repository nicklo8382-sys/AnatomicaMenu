#include "Kinds.h"
#include "During.h"
#include "Mnc.h"
#include "Size.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <format>
#include <unordered_map>

namespace {
    struct KindRec { std::uint16_t times = 0; float lastDay = -1.0f; std::string one, many; bool creature = false; };
    struct Man {
        std::map<std::string, KindRec> kinds;            // kind key -> how often, when last
        std::map<std::string, std::uint16_t> where;      // "hold|kind" -> how often (rumours)
        float deepest = 0.0f;
        bool everDeep = false;
        std::uint32_t firsts = 0;
        std::string lastKind;
        RE::FormID lastPartner = 0;
        float lastDay = -1.0f;
    };
    std::mutex g_mx;
    std::unordered_map<RE::FormID, Man> g_men;
    constexpr std::uint32_t kRec = 'ANKD';
    struct Stuffed { std::uint16_t times = 0; float lastDay = -1.0f; };   // 2.6g: how often he was stuffed full of cum (OStrands)
    std::unordered_map<RE::FormID, Stuffed> g_stuffed;
    constexpr std::uint32_t kRecStuffed = 'ANKS';

    float Day() { auto* c = RE::Calendar::GetSingleton(); return c ? c->GetDaysPassed() : 0.0f; }
    std::string Lower(std::string x) { for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return x; }
    std::string Plural(const std::string& w) {
        if (w.empty()) return w;
        if (w == "wolf" || w == "werewolf") return w.substr(0, w.size() - 1) + "ves";
        if (w.back() == 's' || w.back() == 'x' || w.ends_with("sh") || w.ends_with("ch")) return w + "es";
        if (w.back() == 'y' && w.size() > 1 && std::string("aeiou").find(w[w.size() - 2]) == std::string::npos) return w.substr(0, w.size() - 1) + "ies";
        return w + "s";
    }
    void WStr(SKSE::SerializationInterface* a, const std::string& s) {
        const std::uint16_t n = static_cast<std::uint16_t>(std::min<size_t>(s.size(), 200));
        a->WriteRecordData(&n, sizeof n);
        if (n) a->WriteRecordData(s.data(), n);
    }
    std::string RStr(SKSE::SerializationInterface* a) {
        std::uint16_t n = 0;
        a->ReadRecordData(&n, sizeof n);
        std::string s(n, '\0');
        if (n) a->ReadRecordData(s.data(), n);
        return s;
    }
}

namespace Kinds {
    Kind KindOf(RE::Actor* p) {
        Kind k;
        if (!p) return k;
        if (Size::IsCreatureActor(p) && Mnc::Active()) {   // the creature itself
            std::string n = p->GetRace() && p->GetRace()->GetName() ? p->GetRace()->GetName() : "";
            n = Lower(n);
            for (const char* cut : {" race", "race"}) if (n.ends_with(cut)) n.erase(n.size() - std::string(cut).size());
            if (n.empty()) n = "creature";
            k = {"creature:" + n, n + " cock", Plural(n), true};
            return k;
        }
        const std::string key = Size::ShapeKey(Size::Shape(p));
        if (key.empty() || key == "cut" || key == "uncut") return k;   // nothing stands out
        std::string w = Size::ShapeWord(Size::Shape(p));
        if (w.empty()) w = key;
        k = {key, w + " cock", w + " cocks", false};   // 2.5u: creatures are only named / treated as such with More Nasty Critters
        return k;
    }
    std::string HoldOf(RE::Actor* a) {
        auto* loc = a ? a->GetCurrentLocation() : nullptr;
        static RE::BGSKeyword* hold = nullptr;
        static bool tried = false;
        if (!tried) { tried = true; if (auto* dh = RE::TESDataHandler::GetSingleton()) hold = dh->LookupForm<RE::BGSKeyword>(0x016771, "Skyrim.esm"); }   // LocTypeHold
        for (int i = 0; loc && i < 8; ++i, loc = loc->parentLoc)
            if (hold && loc->HasKeyword(hold)) { const char* n = loc->GetFullName(); return n && *n ? n : ""; }
        return {};
    }
    std::uint32_t OnTaken(RE::Actor* man, RE::Actor* partner) {
        if (!man) return 0;
        const Kind k = KindOf(partner);
        int shape = 0;
        During::Knots kn;
        if (partner) shape = During::AutoShape(partner, &kn);
        const std::string hold = HoldOf(man);
        const float day = Day();
        std::uint32_t now = 0;
        if (partner && Size::IsCreatureActor(partner) && Mnc::Active()) now |= kFirstCreature;   // 2.5u: only with More Nasty Critters
        if (shape == During::kKnot || shape == During::kDoubleKnot || (kn.mask & 3) || k.key == "knotted") now |= kFirstKnot;
        if (shape == During::kHorse || k.key == "flared") now |= kFirstFlare;
        if (shape == During::kColossal || k.key == "colossal") now |= kFirstColossal;
        std::lock_guard lk(g_mx);
        auto& m = g_men[man->GetFormID()];
        if (!k.key.empty()) {
            auto& r = m.kinds[k.key];
            r.times = static_cast<std::uint16_t>(std::min<int>(r.times + 1, 60000));
            r.lastDay = day; r.one = k.one; r.many = k.many; r.creature = k.creature;
            if (!hold.empty()) { auto& w = m.where[hold + "|" + k.key]; w = static_cast<std::uint16_t>(std::min<int>(w + 1, 60000)); }
        }
        m.lastKind = k.key;
        m.lastPartner = partner ? partner->GetFormID() : 0;
        m.lastDay = day;
        const std::uint32_t fresh = now & ~m.firsts;
        m.firsts |= now;
        return fresh;
    }
    std::uint32_t OnReach(RE::FormID man, float reach, bool deep) {
        std::lock_guard lk(g_mx);
        auto& m = g_men[man];
        m.deepest = std::max(m.deepest, reach);
        std::uint32_t fresh = 0;
        if (deep && !m.everDeep) { m.everDeep = true; if (!(m.firsts & kFirstDeep)) fresh = kFirstDeep; m.firsts |= kFirstDeep; }
        return fresh;
    }
    std::uint32_t OnColossalLoad(RE::FormID man) {
        std::lock_guard lk(g_mx);
        auto& m = g_men[man];
        if (m.firsts & kFirstColossalLoad) return 0;
        m.firsts |= kFirstColossalLoad;
        return kFirstColossalLoad;
    }
    Info Get(RE::FormID id, int madeForTimes, float forgetDays) {
        Info i;
        const float day = Day();
        std::lock_guard lk(g_mx);
        auto it = g_men.find(id);
        if (it == g_men.end()) return i;
        const Man& m = it->second;
        int best = 0;
        for (auto& [key, r] : m.kinds) {
            if (r.times < madeForTimes || r.lastDay < 0.0f || day - r.lastDay > forgetDays) continue;
            if (r.times > best) { best = r.times; i.madeFor = r.one; i.madeForMany = r.many; i.madeForCreature = r.creature; }
        }
        i.deepest = m.deepest;
        i.everDeep = m.everDeep;
        if (!m.lastKind.empty()) if (auto k = m.kinds.find(m.lastKind); k != m.kinds.end()) { i.lastKind = k->second.one; i.lastKindMany = k->second.many; i.lastCreature = k->second.creature; }
        i.lastPartner = m.lastPartner;
        i.lastDays = m.lastDay >= 0.0f ? day - m.lastDay : -1.0f;
        return i;
    }
    std::string Rumour(RE::Actor* man, int minTimes) {
        if (!man) return {};
        const std::string hold = HoldOf(man);
        if (hold.empty()) return {};
        std::lock_guard lk(g_mx);
        auto it = g_men.find(man->GetFormID());
        if (it == g_men.end()) return {};
        std::string bestMany; int best = 0; bool creature = false;
        for (auto& [k, n] : it->second.where) {
            if (!k.starts_with(hold + "|") || n < minTimes || n <= best) continue;
            const std::string key = k.substr(hold.size() + 1);
            if (auto r = it->second.kinds.find(key); r != it->second.kinds.end()) { best = n; bestMany = r->second.many; creature = r->second.creature; }
        }
        if (bestMany.empty()) return {};
        const char* nm = man->GetDisplayFullName();
        const std::string who = nm && *nm ? nm : "he";
        if (creature) return std::format("Around {} people whisper that {} lets {} have him - it has happened more than once.", hold, who, bestMany);
        return std::format("Around {} it is said that {} has a taste for {}.", hold, who, bestMany);
    }

    std::uint16_t OnStuffed(RE::FormID man) {
        std::lock_guard lk(g_mx);
        auto& s = g_stuffed[man];
        if (s.times < 60000) ++s.times;
        s.lastDay = Day();
        return s.times;
    }
    StuffedInfo StuffedOf(RE::FormID man) {
        std::lock_guard lk(g_mx);
        auto it = g_stuffed.find(man);
        if (it == g_stuffed.end()) return {};
        return {it->second.times, it->second.lastDay >= 0.0f ? Day() - it->second.lastDay : -1.0f};
    }

    void Save(SKSE::SerializationInterface* a) {
        std::lock_guard lk(g_mx);
        if (a->OpenRecord(kRecStuffed, 1)) {   // 2.6g (its own record: older saves just do not have it)
            const std::uint32_t n = static_cast<std::uint32_t>(g_stuffed.size());
            a->WriteRecordData(&n, sizeof n);
            for (auto& [id, s] : g_stuffed) { a->WriteRecordData(&id, sizeof id); a->WriteRecordData(&s.times, sizeof s.times); a->WriteRecordData(&s.lastDay, sizeof s.lastDay); }
        }
        if (!a->OpenRecord(kRec, 1)) return;
        const std::uint32_t n = static_cast<std::uint32_t>(g_men.size());
        a->WriteRecordData(&n, sizeof n);
        for (auto& [id, m] : g_men) {
            a->WriteRecordData(&id, sizeof id);
            a->WriteRecordData(&m.deepest, sizeof m.deepest);
            const std::uint8_t ed = m.everDeep ? 1 : 0;
            a->WriteRecordData(&ed, sizeof ed);
            a->WriteRecordData(&m.firsts, sizeof m.firsts);
            WStr(a, m.lastKind);
            a->WriteRecordData(&m.lastPartner, sizeof m.lastPartner);
            a->WriteRecordData(&m.lastDay, sizeof m.lastDay);
            const std::uint16_t nk = static_cast<std::uint16_t>(std::min<size_t>(m.kinds.size(), 200));
            a->WriteRecordData(&nk, sizeof nk);
            std::uint16_t i = 0;
            for (auto& [k, r] : m.kinds) {
                if (i++ >= nk) break;
                WStr(a, k); WStr(a, r.one); WStr(a, r.many);
                a->WriteRecordData(&r.times, sizeof r.times);
                a->WriteRecordData(&r.lastDay, sizeof r.lastDay);
                const std::uint8_t c = r.creature ? 1 : 0;
                a->WriteRecordData(&c, sizeof c);
            }
            const std::uint16_t nw = static_cast<std::uint16_t>(std::min<size_t>(m.where.size(), 400));
            a->WriteRecordData(&nw, sizeof nw);
            i = 0;
            for (auto& [k, c] : m.where) {
                if (i++ >= nw) break;
                WStr(a, k);
                a->WriteRecordData(&c, sizeof c);
            }
        }
    }
    bool Load(SKSE::SerializationInterface* a, std::uint32_t type, std::uint32_t ver, std::uint32_t) {
        if (type == kRecStuffed) {
            if (ver != 1) return true;
            std::lock_guard lk(g_mx);
            g_stuffed.clear();
            std::uint32_t n = 0;
            a->ReadRecordData(&n, sizeof n);
            for (std::uint32_t j = 0; j < n; ++j) {
                RE::FormID id = 0, now = 0; Stuffed s;
                a->ReadRecordData(&id, sizeof id); a->ReadRecordData(&s.times, sizeof s.times); a->ReadRecordData(&s.lastDay, sizeof s.lastDay);
                if (a->ResolveFormID(id, now)) g_stuffed[now] = s;
            }
            return true;
        }
        if (type != kRec) return false;
        if (ver != 1) return true;
        std::lock_guard lk(g_mx);
        g_men.clear();
        std::uint32_t n = 0;
        a->ReadRecordData(&n, sizeof n);
        for (std::uint32_t j = 0; j < n; ++j) {
            RE::FormID id = 0, now = 0;
            Man m;
            a->ReadRecordData(&id, sizeof id);
            a->ReadRecordData(&m.deepest, sizeof m.deepest);
            std::uint8_t ed = 0;
            a->ReadRecordData(&ed, sizeof ed);
            m.everDeep = ed != 0;
            a->ReadRecordData(&m.firsts, sizeof m.firsts);
            m.lastKind = RStr(a);
            RE::FormID lp = 0;
            a->ReadRecordData(&lp, sizeof lp);
            if (lp) { RE::FormID r = 0; if (a->ResolveFormID(lp, r)) m.lastPartner = r; }
            a->ReadRecordData(&m.lastDay, sizeof m.lastDay);
            std::uint16_t nk = 0;
            a->ReadRecordData(&nk, sizeof nk);
            for (std::uint16_t i = 0; i < nk; ++i) {
                const std::string k = RStr(a);
                KindRec r;
                r.one = RStr(a); r.many = RStr(a);
                a->ReadRecordData(&r.times, sizeof r.times);
                a->ReadRecordData(&r.lastDay, sizeof r.lastDay);
                std::uint8_t c = 0;
                a->ReadRecordData(&c, sizeof c);
                r.creature = c != 0;
                m.kinds[k] = r;
            }
            std::uint16_t nw = 0;
            a->ReadRecordData(&nw, sizeof nw);
            for (std::uint16_t i = 0; i < nw; ++i) {
                const std::string k = RStr(a);
                std::uint16_t c = 0;
                a->ReadRecordData(&c, sizeof c);
                m.where[k] = c;
            }
            if (a->ResolveFormID(id, now)) g_men[now] = std::move(m);
        }
        return true;
    }
    void Revert() {
        std::lock_guard lk(g_mx);
        g_men.clear();
        g_stuffed.clear();
    }
}

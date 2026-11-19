#pragma once
#include <SimpleIni.h>
#include <string>
#include <vector>

// 2.5o: what kinds of partner each man has taken (for SkyrimNet): how often each kind, when last, where (for rumours),
// how deep it has gone, what last stretched him, and his firsts. A kind is the partner's schlong type (knotted, flared,
// ridged...) or, with More Nasty Critters installed, the creature itself (wolf, horse, troll...). Kept in the co-save.
namespace Kinds {
    void Save(SKSE::SerializationInterface* a);
    bool Load(SKSE::SerializationInterface* a, std::uint32_t type, std::uint32_t ver, std::uint32_t len);   // true = it was ours
    void Revert();

    struct Kind { std::string key, one, many; bool creature = false; };   // key, "a horse" / "horse cock", "horses" / "horse cocks"
    Kind KindOf(RE::Actor* partner);                 // "" key when nothing stands out (a plain human schlong)

    enum First : std::uint32_t { kFirstCreature = 1, kFirstKnot = 2, kFirstFlare = 4, kFirstDeep = 8, kFirstColossal = 16, kFirstColossalLoad = 32 };
    // a round with this partner (from Bridge::Taken): counts it, notes where; returns the firsts it was (First bits)
    std::uint32_t OnTaken(RE::Actor* man, RE::Actor* partner);
    // the bulge went this far this round (sections 0-8; deep = super deep); returns the firsts it was (kFirstDeep)
    std::uint32_t OnReach(RE::FormID man, float reach, bool deep);
    // 2.6f: he was just filled with a Colossal Load (OStrands); returns kFirstColossalLoad the first time
    std::uint32_t OnColossalLoad(RE::FormID man);
    // 2.6g: stuffed full of cum (OStrands): counts it, returns how many times now; and what is known of it
    std::uint16_t OnStuffed(RE::FormID man);
    struct StuffedInfo { std::uint16_t times = 0; float daysSince = -1.0f; };
    StuffedInfo StuffedOf(RE::FormID man);

    struct Info {
        std::string madeFor, madeForMany;   // the kind he was made for ("" = none yet): one / many wording
        bool madeForCreature = false;
        float deepest = 0.0f;               // the furthest any bulge has gone (sections)
        bool everDeep = false;
        std::string lastKind;               // what last had him ("" = a plain man) and how long ago (game days)
        std::string lastKindMany;
        RE::FormID lastPartner = 0;
        float lastDays = -1.0f;
        bool lastCreature = false;
    };
    Info Get(RE::FormID man, int madeForTimes, float forgetDays);
    // what people say about him where he is now (his creature / unusual partners there), "" when nothing
    std::string Rumour(RE::Actor* man, int minTimes);
    std::string HoldOf(RE::Actor* a);               // the hold he is in ("" when not known)
}

#pragma once
#include <SimpleIni.h>
#include <string>

// 2.6c: Colossal Load (with OStrands 0.8.0.4+). Who finishes with a bigger, heavier load, and how big:
//  * men: by schlong size (The New Gentleman L counts half, XL full) and/or a chance per scene - no creatures needed;
//  * with More Nasty Critters: creatures, by their type (knotted, flared, colossal...), and any MNC group can have its own.
// OStrands asks for each giver (Anatomica_LoadKind) and multiplies its normal [Semen] values by these.
namespace Colossal {
    struct Load {                               // multipliers on OStrands' normal load (1 = as normal)
        float amount = 2.0f, force = 1.3f, pulses = 1.4f, thick = 1.3f, sticky = 1.2f, stretch = 1.2f;
        bool ownColour = false;
        float rgb[3]{0.925f, 0.914f, 0.871f};
        std::string Save() const;
        static Load Parse(const char* v, const Load& def);
    };
    // weight 0 = a normal load, 1 = his full Colossal Load (in between for TNG L); false = Colossal Load is off
    bool For(RE::Actor* giver, RE::Actor* receiver, float& weight, Load& load);
    std::string Describe(RE::Actor* giver);
    bool IsWerewolf(RE::Actor* a);              // 2.6i: a werewolf / werebear beast form
    float RingBoost(RE::Actor* giver);          // 2.6i: x what wells out round the rim while he fills someone (werewolves)     // one line for the menu ("" = nothing to say)
    bool RenderSection();                       // Integrations page
    void Ini(CSimpleIniA& ini, bool save);      // [ColossalLoad], [ColossalLoadTypes], [ColossalLoadGroups]
}

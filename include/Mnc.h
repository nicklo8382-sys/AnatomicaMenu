#pragma once
#include <SimpleIni.h>
#include "During.h"
#include <string>
#include <vector>

// More Nasty Critters (2.5): only does anything when MoreNastyCritters.esp is loaded (its menu section is hidden otherwise).
// Read once from MNC's own files:
//  * Data/creatures.d/*.json - the Creature Framework styles: the armour a creature wears when aroused tells its style
//    (Dog / Canine -> knotted, Horse -> flared, Dragon -> ridged). Werewolves and dragons have these.
//  * Data/creatures.d/MNC_SLRaceRegistration.d/*.json - race editor IDs sorted into groups (Canines, Trolls, Spiders...).
// Races in no group are tried by name for a few extra groups (Daedroth, Demons, Thri-kreen, Kwama, Clannfear).
// Each group counts as a schlong type (Size: open / build / loose / sore, and the bulge shape and knots set for that type),
// and can instead have its own settings (2.5k override).
namespace Mnc {
    bool Active();                                  // MoreNastyCritters.esp is loaded (reads the files the first time)
    std::string TypeFor(RE::Actor* creature);       // his schlong type key (his style, else his group's type), "" = not decided
    struct Override {                               // a group's own settings, used instead of its type's when on
        bool on = false;
        float open = 1.0f, build = 1.0f, loose = 1.0f, tender = 1.0f;
        int shape = 0;                              // bulge shape (During::BulgeShape), 0 = its type's
        During::Knots knots;
        std::string Save() const;
        static Override Load(const char* v);
    };
    bool OverrideFor(RE::Actor* creature, Override* out);   // true when his group has its own settings switched on
    struct Group { std::string name; int races; std::string type; Override ov; };
    std::vector<Group> Groups();                    // for the menu (sorted by name)
    void SetType(const std::string& group, const std::string& typeKey);   // "" = automatic
    void SetOverride(const std::string& group, const Override& o);
    void Ini(CSimpleIniA& ini, bool save);          // [CreatureType] group = type, [CreatureOverride] group = its own settings
}

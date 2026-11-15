#pragma once
#include <SimpleIni.h>
#include "During.h"
#include <string>
#include <vector>

// More Nasty Critters (2.5): the belly-bulge shape for creatures. Only does anything when MoreNastyCritters.esp is loaded
// (the menu section is hidden otherwise). Read once from MNC's own files:
//  * Data/creatures.d/*.json - the Creature Framework styles: the armour a creature wears when aroused tells its style
//    (Dog / Canine -> knot, Horse -> flared head, Dragon -> ridged). Werewolves and dragons have these.
//  * Data/creatures.d/MNC_SLRaceRegistration.d/*.json - race editor IDs sorted into groups (Canines, Trolls, Spiders...),
//    each group given a bulge shape (defaults below, changeable in the menu, saved in [CreatureBulge]).
// Races in no group are tried by name for a few extra groups (Daedroth, Demons, Thri-kreen, Kwama).
namespace Mnc {
    bool Active();                                  // MoreNastyCritters.esp is loaded (reads the files the first time)
    int ShapeFor(RE::Actor* creature, During::Knots* knots = nullptr);   // the bulge shape (During shape number), 0 = not
                                                    // decided here; knots: his group's knots (which, how strong)
    std::string TypeFor(RE::Actor* creature);       // 2.5f: his schlong type key for Size (open / loose / sore), "" = not decided
    struct Group { std::string name; int shape; int races; During::Knots knots; std::string type; };
    std::vector<Group> Groups();                    // for the menu (sorted by name)
    void SetShape(const std::string& group, int shape);
    void SetKnots(const std::string& group, const During::Knots& knots);
    void SetType(const std::string& group, const std::string& typeKey);   // "" = automatic
    void Ini(CSimpleIniA& ini, bool save);          // [CreatureBulge] group = shape, [CreatureBulgeKnots] group = knots, [CreatureType] group = type
}

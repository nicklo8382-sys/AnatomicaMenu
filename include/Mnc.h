#pragma once
#include <SimpleIni.h>
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
    int ShapeFor(RE::Actor* creature);              // the bulge shape (During shape number), 0 = not decided here
    struct Group { std::string name; int shape; int races; };
    std::vector<Group> Groups();                    // for the menu (sorted by name)
    void SetShape(const std::string& group, int shape);
    void Ini(CSimpleIniA& ini, bool save);          // [CreatureBulge] group = shape
}

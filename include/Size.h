#pragma once
#include <SimpleIni.h>
#include <string>

// How big the man (or creature) giving it is, from The New Gentleman (TNG_PapyrusUtil.GetActorSize, asked in the
// background and remembered for a few minutes). Bigger partners open him further and faster, loosen him more and leave
// him sorer; smaller ones the reverse. Creatures (horses, werewolves, dragons...) count as the far end of the scale.
// Without TNG everything stays neutral (creatures still count, if that is switched on).
namespace Size {
    void Ini(CSimpleIniA& ini, bool save);   // [PartnerSize] in AnatomicaMenu.ini (loaded / saved with [Integrations])

    constexpr int kUnknown = -1, kCreature = 5;
    int Cat(RE::Actor* partner);             // -1 not known (yet), 0-4 TNG XS..XL, 5 creature; asks TNG when needed
    int CatOf(RE::FormID partner);           // only what is already known (no asking)
    const char* Word(int cat);               // "small" .. "huge", "monstrous"; "" for average / unknown

    // the shape of his schlong (the armour / armature TNG gave him, by TNG's own types or the add-on's name)
    // The shapes are a table: built in (cut, uncut, knotted, flared, ridged, barbed, tentacle), changed / added to by
    // compatibility files (Data/SKSE/Plugins/AnatomicaMenu/Schlongs/*.ini) and in the menu - no new DLL needed.
    constexpr int kShapeUnknown = 0;
    int Shape(RE::Actor* partner);           // index into that table (0 = not known)
    std::string ShapeWord(int shape);        // "uncut", "knotted"...; "" when not known
    std::string Describe(RE::Actor* partner);   // "huge, knotted" / "uncut" / "" (for memories and reactions)

    struct K { float open = 1.0f, build = 1.0f, loose = 1.0f, tender = 1.0f; };
    K For(RE::Actor* partner);               // size x shape; neutral when off or not known
    bool HasTNG();

    bool RenderSection();                    // its part of the Integrations page (true = a setting changed)
}

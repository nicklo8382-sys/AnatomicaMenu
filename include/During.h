#pragma once
#include <SimpleIni.h>

// "During sex" page: how he looks and how tight he is while he is being taken.
//  * How tight he is: PPA's opening for Anatomica (how far it spreads the ring, the widest it goes, grip / drag, rubbing),
//    written into PPA's override file for Anatomica (SKSE/Plugins/ppa-override-configs/anatomica.toml). PPA reads it
//    when the game starts, or at once with its reload key (F5).
//  * Inside: the canal opens round the schlong (no clipping inside him), the rim and entrance are drawn out with the
//    shaft as it pulls out (the "suck"), and an optional belly bulge that follows the tip (new BodySlide sliders).
//  * Shape: a puffy, swollen rim ("donut") that builds while he is penetrated (the Anatomica Swell body morph, its own
//    key, so the after-sex swelling still adds to it) and settles a few seconds after.
namespace During {
    void Ini(CSimpleIniA& ini, bool save);   // [DuringSex] in AnatomicaMenu.ini (loaded / saved with the colour settings)
    float Donut();                           // 0 = off, else the most the ring swells while he is taken (Anatomica Ring)
    void SyncPPA();                          // the hole follows the schlong: keep PPA's own opening at (almost) none
    // the rest of the page, read by the gape engine each tick (0 = off)
    struct Want {
        float canal = 1.0f;       // open the inside (Anatomica Canal) round the schlong while it is in him
        bool holeFollow = true;   // the hole opens to the schlong's own thickness where it passes (Anatomica Gape)
        float holeSize = 1.0f;    // how snug: 1 = exactly his thickness, more = a little wider
        bool shift = true;        // the hole moves sideways with the shaft (Anatomica Shift / Shift Deep), not only opens
        float shiftMax = 2.0f;    // the most it moves (units)
        int holeDetail = 1;       // 0 Normal (4 directions, 3 depths), 1 Fine (8 x 2 rings, 6 depths), 2 Super fine (16 x 3, 10)
        bool ring = true;         // the rim follows the shaft: drawn in on the push, pulled out on the withdraw (Anatomica Pout)
        float grip = 0.6f;        // how strongly it follows the shaft
        float maxOut = 0.5f;      // the most it is pulled out (units)
        float maxIn = 0.5f;       // the most it is drawn in (units)
        float grow = 0.0f;        // the pull-out limit grows by this much (0-2 = 0-200%) the more he is worked
        float bulge = 0.4f;       // belly bulge strength (the sliders are built extreme: 100% = the full shape)
        float bulgeStart = 6.0f;  // how deep the tip has to be before it shows (units along his canal)
        bool bulgeBig = false;    // only from big partners (TNG L / XL, creatures)
        int bulgeShape = 0;       // 0 from his schlong's shape, 1 bump at the tip, 2 long shaft, 3 big head, 4 knot, 5 tentacle
        float bulgeRandom = 0.3f; // how much each stroke varies (height / size)
        float outline = 0.6f;     // the shaft behind the tip shows too (0 = only the tip)
        float pulse = 0.35f;      // a quick extra bump on each fast push
        float linger = 4.0f;      // seconds it takes to go down after he pulls out (longer when worked hard)
        int bulgeThrough = 0;     // shows through: 0 nothing (only when bare / revealing armour), 1 clothes, 2 clothes and armour
    };
    Want Get();
    bool BulgeFor(RE::Actor* man, RE::Actor* player);   // he is in the bulge's "who" (everyone / you + followers / you)
    void __stdcall RenderPage();
}

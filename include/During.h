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
    float Donut();                           // 0 = off, else the most it swells while he is taken (0-1)
    // the rest of the page, read by the gape engine each tick (0 = off)
    struct Want {
        float canal = 1.0f;       // open the inside (Anatomica Canal) round the schlong while it is in him
        float suck = 0.5f;        // the rim and entrance drawn out with the shaft as it pulls out (Anatomica Pout)
        float bulge = 0.0f;       // belly bulge strength (Anatomica Bulge 1-5)
        float bulgeStart = 10.0f; // how deep the tip has to be before it shows (units along his canal)
        bool bulgeBig = false;    // only from big partners (TNG L / XL, creatures)
        float outline = 0.6f;     // the shaft behind the tip shows too (0 = only the tip)
        float pulse = 0.35f;      // a quick extra bump on each fast push
        float linger = 4.0f;      // seconds it takes to go down after he pulls out (longer when worked hard)
        int bulgeThrough = 0;     // shows through: 0 nothing (only when bare / revealing armour), 1 clothes, 2 clothes and armour
    };
    Want Get();
    bool BulgeFor(RE::Actor* man, RE::Actor* player);   // he is in the bulge's "who" (everyone / you + followers / you)
    void __stdcall RenderPage();
}

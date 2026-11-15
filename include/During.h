#pragma once
#include <SimpleIni.h>
#include <string>

// "During sex" page: how he looks and how tight he is while he is being taken.
//  * How tight he is: PPA's opening for Anatomica (how far it spreads the ring, the widest it goes, grip / drag, rubbing),
//    written into PPA's override file for Anatomica (SKSE/Plugins/ppa-override-configs/anatomica.toml). PPA reads it
//    when the game starts, or at once with its reload key (F5).
//  * Inside: the canal opens round the schlong (no clipping inside him), the rim and entrance are drawn out with the
//    shaft as it pulls out (the "suck"), and an optional belly bulge that follows the tip (new BodySlide sliders).
//  * Shape: a puffy, swollen rim ("donut") that builds while he is penetrated (the Anatomica Swell body morph, its own
//    key, so the after-sex swelling still adds to it) and settles a few seconds after.
namespace During {
    // the belly-bulge shapes (2.5: Ridged .. Long tentacle (kBeaded) added at the end; numbers are saved in the ini)
    enum BulgeShape { kAuto = 0, kTip, kShaft, kHorse, kKnot, kTentacle, kRidged, kSegmented, kColossal, kHooked, kDoubleKnot,
                      kBeaded, kShapeCount };
    const char* ShapeName(int shape);        // menu name ("From his schlong" for 0)
    // 2.5e: the knots for one schlong (type / size / creature group): which are added to his shape (1 knot 1, 2 knot 2) and how
    // strong each is (also for the Knot / Double knot shapes' own knots). Saved as "mask|k1|k2".
    struct Knots {
        int mask = 0;
        float k1 = 1.0f, k2 = 1.0f;
        std::string Save() const;
        static Knots Load(const char* v);
    };
    // "From his schlong": creature (More Nasty Critters), his TNG type, his size; knots (if given): that schlong's knots
    int AutoShape(RE::Actor* partner, Knots* knots = nullptr);
    void Ini(CSimpleIniA& ini, bool save);   // [DuringSex] in AnatomicaMenu.ini (loaded / saved with the colour settings)
    float Donut();                           // 0 = off, else the most the ring swells while he is taken (Anatomica Ring)
    void SyncPPA();                          // puts PPA's own opening back if a 2.4 test build had turned it to (almost) none
    // the rest of the page, read by the gape engine each tick (0 = off)
    struct Want {
        float canal = 1.0f;       // open the inside (Anatomica Canal) round the schlong while it is in him
        bool ring = true;         // the rim follows the shaft: drawn in on the push, pulled out on the withdraw (Anatomica Pout)
        float grip = 0.6f;        // how strongly it follows the shaft
        float maxOut = 0.5f;      // the most it is pulled out (units)
        float maxIn = 0.5f;       // the most it is drawn in (units)
        float grow = 0.0f;        // the pull-out limit grows by this much (0-2 = 0-200%) the more he is worked
        float bulge = 0.4f;       // belly bulge strength (the sliders are built extreme: 100% = the full shape)
        float bulgeStart = 6.0f;  // how deep the tip has to be before it shows (units along his canal)
        bool bulgeBig = false;    // only from big partners (TNG L / XL, creatures)
        int bulgeShape = 0;       // BulgeShape (0 = from his schlong)
        float shaftK = 1.0f, headK = 1.0f;   // 2.5: strength of the shaft / the head (x Bulge strength)
        bool travel[kShapeCount]{}, twitch[kShapeCount]{}, build[kShapeCount]{};   // 2.5 movement, per shape
        float buildTime = 12.0f;  // 2.5 build-up: seconds to the full shape (1-60)
        bool holdFull = true;     // 2.5 build-up: once full the shaft stays, only the head moves
        float belly = 0.5f, bellyTime = 30.0f, bellyRandom = 0.3f;   // 2.5f stomach: how far the whole lower belly rounds out (0 = off),
                                                                      // seconds to rise to it, how much it varies stroke to stroke
        bool imprint = false;     // 2.5 after sex: a faint copy of the last shape lingers
        float imprintAmt = 0.15f, imprintMin = 3.0f;   // how strong (of the full shape), minutes until it is gone
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

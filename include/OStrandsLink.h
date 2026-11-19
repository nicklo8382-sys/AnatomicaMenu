#pragma once
#include <SimpleIni.h>

// 2.5w: OStrands link (only when OStrands 0.8.0.3 or newer is installed). OStrands draws the cum; Anatomica tells it
// where his hole really is (the lip as it is now: gape, wink and PPA's pull all in it) and takes what OStrands puts in
// him. What he holds then comes back out of him through OStrands' fluid: oozing while he is open, a gush as the
// schlong slides out, a squirt when he clenches, and it seeps out round the shaft while he is still being taken.
// Anatomica's own cum (Features.h) can stay off - this does not need it.
namespace OStrandsLink {
    void Install();                                               // SKSE load: the worker thread
    void Reset();                                                 // a save was loaded / new game: he holds nothing
    bool Installed();                                             // OStrands with the link is loaded
    void OnWorked(RE::Actor* recv, RE::Actor* partner, float dil);   // the gape script: he is being taken
    void OnClench(RE::FormID id, float amp);                      // Wink: a clench just started
    std::string Status(RE::Actor* a);                             // one line for the Status page ("" = nothing)
    bool RenderSection();                                         // Integrations page
    void Ini(CSimpleIniA& ini, bool save);                        // [OStrands]
}

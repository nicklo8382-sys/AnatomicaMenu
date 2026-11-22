#pragma once
#include <SimpleIni.h>
#include <string>
#include <vector>

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
    float Belly(RE::FormID id);
    // 2.6f: for SkyrimNet - what he holds now, and the fillings that just finished
    struct Held { float vol = 0.0f; bool flooding = false, leaking = false, filling = false, plugged = false; double floodAt = 0.0;
                  RE::FormID giver = 0; bool stuffed = false; float most = 0.0f; };   // 2.6i: who is filling him, over "stuffed", the most he holds
    void Bathed(RE::FormID id);
    void PrecumIn(RE::Actor* recv, RE::Actor* giver, float loads);   // 2.6j: OStrands precum into him: held, and it wets him                                   // 2.6g: he washed (Bathing in Skyrim) - it all runs out
    std::vector<RE::FormID> TakeStuffed();                        // 2.6g: men who just got stuffed full (SkyrimNet)
    Held HeldOf(RE::FormID id);
    int GiverCount(RE::FormID id);                                // 2.6j: how many men's cum he holds now
    struct Filled { RE::FormID man = 0, giver = 0; float loads = 0.0f; bool colossal = false; };
    std::vector<Filled> TakeFilled();                                   // 2.6e: the Stomach shape for what he holds (0 = none)
    void RenderTests(RE::Actor* a);
    std::string TestFacial(RE::Actor* a, int kind, RE::Actor* giver);   // 2.6k: Testing page (0 normal, 1 Colossal, 2 light, 3 the giver's colour)                               // 2.6d: Testing page buttons (fill, gush, squirt, ooze, empty)
    // 2.6u: OStrands drops at a world point (tears, cum on his face); stream: 0 = loose drops, else kept to link them
    bool FaceStrand(RE::Actor* host, const RE::NiPoint3& at, float scale, float life, float maxLen, const float rgb[3]);   // 2.6v
    bool EmitAt(RE::Actor* host, const RE::NiPoint3& at, const RE::NiPoint3& vel, int count, const float rgb[3], std::uint32_t* stream = nullptr);
    bool RenderSection();                                         // Integrations page
    void Ini(CSimpleIniA& ini, bool save);                        // [OStrands]
}

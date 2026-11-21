#pragma once
#include <SimpleIni.h>

// 2.6j: "Mouth & Face" - what oral does to his face and throat (OStim blowjob / deepthroat, he is the one with the mouth):
//  * jaw: his mouth opens as wide as the schlong is thick (the face's own "big aah" mouth shape, every frame)
//  * throat bulge (experimental): his neck swells as it goes deep (RaceMenu node scale on the neck bone, the head scaled back)
//  * oral experience: kept with the save - a beginner gags more, a practised mouth hardly at all
//  * gagging: going deep can make him gag (eyes squeezed, brows pulled in), more for a beginner and a bigger schlong
//  * OBlush: while OBlush is blushing him, gagging / his first time make the blush stronger for a moment
namespace Face {
    void Install();                                   // SKSE load: the worker thread
    void Reset();                                     // a save was loaded
    void Ini(CSimpleIniA& ini, bool save);            // [Face]
    void Save(SKSE::SerializationInterface* a);
    bool Load(SKSE::SerializationInterface* a, std::uint32_t type, std::uint32_t ver, std::uint32_t len);   // true = ours
    void Revert();
    struct Oral { float exp = 0.0f; std::uint32_t times = 0, gags = 0; const char* level = ""; };
    Oral OralOf(RE::FormID id);                       // his oral experience (SkyrimNet / DDF later)
    void Swallowed(RE::Actor* recv, float loads);     // OStrands: this much (in loads) was swallowed - gulps
    void OnFace(RE::Actor* recv, RE::Actor* giver, float loads);
    void TestLayer(RE::Actor* a, int layer);   // 2.6k: Testing page - one face look on him for 20 s (-1 = all)
    void ClearFace(RE::Actor* a);              // 2.6k: Testing page - take every face look off him   // 2.6j: cum on his face (a facial)
    void Bathed(RE::FormID id);                       // he washed: cum, tears and sweat off
    bool Wiped(RE::FormID id);                        // SkyrimNet clean-up: cum and tears off his face (true = there was some)
    // 2.6j: for DDF lines - who is sucking (or just was), how it is going
    struct OralState { RE::FormID id = 0, partner = 0; bool sucking = false, deep = false, gagging = false, swallowed = false; float faceCum = 0.0f, exp = 0.0f; };
    std::vector<OralState> OralNow();
    std::string SeenText(RE::Actor* a);               // 2.6j SkyrimNet: what anyone can see on his face ("" = nothing)
    std::string SelfText(RE::Actor* a);               // 2.6j SkyrimNet: what he knows of his own mouth
    void __stdcall RenderPage();                      // the "Mouth & Face" page
}

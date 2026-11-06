#pragma once

// Anus colour, skin-blend overlay matching and after-sex looks (wetness, tender reddening).
// The DLL decides what each man should look like; the gape script applies it with RaceMenu
// (NiOverride) every few seconds. Menu: its own section, "Anatomica Colour".
namespace Colour {
    bool ConfirmButton(const char* label);   // button with an "Are you sure?" step
    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm);
    void Reset();                       // a save was loaded / new game
    float TenderLevel(RE::FormID id);   // 0-1 reddening right now (0 when "sensitivity" is off)
    float SensClench();                 // how much sharper clenches get while tender (0-1)
    float SensBreath();                 // how much slower breathing gets while tender (0-1)
    bool CumColour(RE::Actor* giver, float out[3]);   // his race's cum colour (Per race page); false = none
    void SaveNow();                     // write AnatomicaMenu.ini now
    void RenderCumColours();            // cum colour per race + wetness follows the giver (Cum & drips page)
    void __stdcall RenderGeneral();   // settings for the whole script (wait after loading screens)
    void __stdcall RenderColours();
    void __stdcall RenderRaces();
    void __stdcall RenderRules();
    void __stdcall RenderAfter();
    void __stdcall RenderStatus();
    void __stdcall RenderTesting();    // the Testing page: every effect on demand
    void RegisterWindow();             // the live status window + hotkey input (call after the menu sections)
    // for the DLL gape engine (Engine.cpp): the same answers the script got from AnatomicaNative
    void WorkedNow(RE::Actor* a, float dil, float partners, RE::Actor* partner);
    std::vector<std::int32_t> LookNow(RE::Actor* a);
    std::string ColourPath(RE::Actor* a);
    float CloseSlowFor(RE::Actor* a);
    float LooseNow(RE::Actor* a, float cur, float days);
    std::vector<RE::Actor*> Men();
    float Interval();
    float LoadPauseNow();
    std::vector<std::string> SchlongShapeNames(RE::Actor* a);
    struct RingWant { int col = -1; int swap = 0; int add = -1; };
    RingWant RingNow(RE::Actor* a);
    struct WetInfo {
        RE::FormID id; float wet; float rgb[3];
        float cum = 0.0f;                 // cum in the wetness: opacity (0 = the beads are clear / lube)
        float schlong = 0.0f;             // his schlong wet from the round (giver), 0-1
        float scum = 0.0f;                // cum on his schlong: opacity (0 = only the wet shine)
        float srgb[3] = {0.96f, 0.95f, 0.90f};
    };
    std::vector<WetInfo> WetNow();     // every man who is wet right now (0-1) and the colour of it (Fluid draws it)
}

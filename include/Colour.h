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
    int RingTexLevel();                 // ring colour textures: mip levels to drop (0 = 2048, 1 = 1024, 2 = 512)
    bool RingStrong();                  // "Stronger skin-blend ring": ring colour textures with doubled opacity
    void TidyMade();                    // keep the folder of textures Anatomica makes under its size budget (game start)
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
    std::string InnerPath(RE::Actor* a);   // the inside (canal) texture: "" = the installed one, "\x01" = being made (keep his)
    float InnerMoist();                    // "always moist inside": extra shine on the canal even when dry (0-1)
    float CloseSlowFor(RE::Actor* a);
    float LooseNow(RE::Actor* a, float cur, float days);
    void RestHours(RE::FormID id, float hours);   // healing: this many extra hours of rest for his lasting looseness
    // for the integrations (Bridge.cpp)
    struct Snapshot { float wet = 0, tender = 0, bruise = 0, swell = 0, schlong = 0, gape = 0, loose = 0; int partners = 0; bool lube = false, cum = false, aroused = false; };
    Snapshot SnapshotOf(RE::Actor* a);             // how he looks right now (0-1 each)
    void AddWet(RE::Actor* a, float amount);       // lube him up (wet only)
    void Bathed(RE::FormID id, bool clearWet, float soothe);   // washing: dry him (and his schlong), ease redness by this much
    void SetArousal(RE::FormID id, float wet, float sens);     // arousal: a little wet (0-1), twitchier (0-1)
    std::vector<RE::Actor*> Men();
    float Interval();
    float LoadPauseNow();
    std::vector<std::string> SchlongShapeNames(RE::Actor* a);
    struct RingWant { int col = -1; int swap = 0; int add = -1; };
    RingWant RingNow(RE::Actor* a);
    struct WetInfo {
        RE::FormID id; float wet; float rgb[3];
        float cum = 0.0f;                 // cum in the wetness: opacity (0 = the beads are clear / lube)
        float gloss = 0.0f;               // glossy skin round the hole and up the crevice (0-1)
        float lube = 0.0f;                // lube colour on the outer wet layer: how much (0 = clear)
        float lrgb[3] = {1.0f, 1.0f, 1.0f};
        float schlong = 0.0f;             // his schlong wet from the round (giver), 0-1
        float scum = 0.0f;                // cum on his schlong: opacity (0 = only the wet shine)
        bool sdrip = false;               // and now and then a drip from the tip
        float srgb[3] = {0.96f, 0.95f, 0.90f};
        float shine[3] = {1.0f, 1.0f, 1.0f};   // wet shine colour: the colour of the wet highlights on the rim and inside
        float shineAmt = 0.0f;                 // how much (0 = clear, as before)
        float innerWet = 1.0f;                 // the inside shines this much more than the rim while wet
    };
    std::vector<WetInfo> WetNow();     // every man who is wet right now (0-1) and the colour of it (Fluid draws it)
}

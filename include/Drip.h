#pragma once
#include <SimpleIni.h>

// Anatomica's own cum drip: SMP strands hanging from the anus (meshes and physics ship with the
// mod), squirts, puddles on the floor and a trail down the leg. Fed by OStim / SexLab climax
// events (the gape script resolves who received) or, without a framework, by the end of a round.
namespace Drip {
    void Install();                                        // SKSE load: co-save + worker thread
    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm);
    void Reset();                                          // a save was loaded / new game
    void Ini(CSimpleIniA& ini, bool save);                 // settings ([Drip] in AnatomicaMenu.ini)
    void OnWorked(RE::Actor* receiver, RE::Actor* partner, float dil);   // gape script: he is being penetrated
    void OnClench(RE::FormID id, float amp);              // Wink: a clench just started
    bool Film(RE::FormID id, float rgb[3], float& amt);    // cum film for the rim texture (false = none)
    float HoldOpen(RE::FormID id);                         // gape closes this many times slower (1 = normal)
    std::string Status(RE::Actor* a);                      // one line for the Status page
    void __stdcall RenderPage();
}

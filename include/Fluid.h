#pragma once
#include <SimpleIni.h>

// Anatomica's runtime cum: strands, streams, drops and rivulets simulated by the DLL every frame (no SMP) and drawn
// with two small skinned templates (a round tube and a flat rivulet, 16 bones each) whose bones are moved to the
// simulated points. They hang from the real lip of the anus (read live from the body mesh, so the gape, the wink and
// PPA all move it), collide with and run down the actual body surface (the morphed body mesh, OBody included), pool
// at the floor (puddles) and dry off. Drip.cpp decides when and how much; this decides how it looks and moves.
namespace Fluid {
    enum class Kind { Squirt, Spray, Ooze, Gush, Land };

    void Install();                                  // start the frame loop (SKSE load)
    void Reset();                                    // a save was loaded: forget everything
    void Ini(CSimpleIniA& ini, bool save);           // [Fluid] in AnatomicaMenu.ini
    void RenderSettings();                           // "Look and physics" (Cum & drips page)

    // ---- what Drip asks for (any thread) ----
    void Hang(RE::FormID id, float amount, float phase, const float rgb[3]);   // strands from the lip; amount 0 = none, phase 0-1 = how far the next drip has grown
    void Release(RE::FormID id, float amount);                                   // the drip falls now
    void Squirt(RE::FormID id, float amount, const float rgb[3], Kind kind, float hold = 0.0f);
    void String(RE::FormID id, RE::FormID partner, const float rgb[3]);          // the string to him as he pulls out
    void Trail(RE::FormID id, float level, int side, const float rgb[3]);        // rivulets down the leg; level 0-3, side 1 = left, 2 = right
    void Coat(RE::FormID giver, float amount, const float rgb[3], float seconds); // his schlong: wet, cum streaks, a drip from the tip
    void Clear(RE::FormID id);                                                    // everything on him gone (wash, bath, test)
    float WetOf(RE::FormID id);                                                   // how wet he is now (0-1)
    std::vector<RE::FormID> WetMen();                                             // everyone wet right now
    bool Busy(RE::FormID id);                                                     // a squirt / ooze still running
    std::vector<std::pair<RE::FormID, float>> TakeLandings();                    // drops that reached the floor: (actor, amount) -> puddles
    std::string Status(RE::FormID id);                                            // one line for the Status page
    bool TestString(RE::Actor* a, const float rgb[3]);
    void SlowMotion(bool on);                                                     // Testing page: everything at 15% speed
    bool SlowMotion();                           // a string to an invisible partner pulling away
}

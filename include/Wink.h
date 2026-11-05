#pragma once
#include "PCH.h"

namespace Wink {
    // Registers the AnatomicaNative Papyrus functions and starts the per-frame bone updater.
    void Register();
    // Forget all actors (save loaded / new game).
    void Reset();
    // current after-sex gape the movement code is following (0 if untracked) - for the Status page
    float GapeOf(RE::FormID id);
    // Testing page: a clench right now (only for a man the movement code is already following)
    bool TestClench(RE::Actor* a);
    // the DLL gape engine talks to the movement code directly (same as the script's SetGape / Blink)
    void SetGapeNow(RE::Actor* a, float g);
    void BlinkNow(RE::Actor* a);
}

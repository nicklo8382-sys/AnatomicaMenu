#pragma once
#include "PCH.h"

namespace Wink {
    // Registers the AnatomicaNative Papyrus functions and starts the per-frame bone updater.
    void Register();
    // Forget all actors (save loaded / new game).
    void Reset();
}

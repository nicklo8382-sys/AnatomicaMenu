#pragma once
#include "SKSEMenuFramework.h"

namespace UI {
    void Register();

    // Called when a save is loaded / a new game starts: pushes the INI values
    // into the AnatomicaGape.esp globals so settings are the same in every save.
    void ApplySavedSettings();

    namespace Gape {
        void __stdcall Render();
    }
    namespace Jiggle {
        void __stdcall Render();
    }
    namespace About {
        void __stdcall Render();
    }
}

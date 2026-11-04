#pragma once
#include "SKSEMenuFramework.h"

namespace UI {
    void Register();

    // Called when a save is loaded / a new game starts: pushes the INI values
    // into the AnatomicaGape.esp globals so settings are the same in every save.
    void ApplySavedSettings();
    // re-read AnatomicaMenu.ini (after loading a preset) and push it into the plugin
    void ReloadSettings();

    namespace Gape {
        void __stdcall Render();
    }
    namespace Motion {
        void __stdcall Render();
    }
    namespace Jiggle {
        void __stdcall Render();
    }
    namespace About {
        void __stdcall Render();
    }
}

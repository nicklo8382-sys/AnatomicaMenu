#pragma once
#include <SimpleIni.h>

// 2.6i: Dynamic Dialogue Framework (DDF) - Anatomica's dialogue pack (Data/TMSDynamicDialogue/Packs/Anatomica.json)
// picks its lines by RaceMenu body-morph values. Anatomica writes his state as morphs that change nothing on the body
// (key "AnatomicaDDF"): taken, first time / experience, how open, bulge, deep, partner's size, filled, Colossal Load,
// too full, flooding, plugged - and "remarks" on the man taking him. A category switched off here reads 0, so its lines
// never play. Only does anything with DDF (TMS_DynamicDialogue.dll) installed.
namespace Talk {
    void Install();                               // SKSE load: the worker thread
    void Reset();                                 // a save was loaded: forget what was written (cleared as men come by)
    bool Installed();                             // DDF is loaded
    bool RenderSection();                         // Integrations page
    void Ini(CSimpleIniA& ini, bool save);        // [DDF]
}

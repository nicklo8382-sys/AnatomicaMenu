#pragma once
#include <SimpleIni.h>
#include <vector>

// 2.5v: MuDynamicNormalMap (only when MuDynamicNormalMap.dll is installed). It rebuilds a man's normal map from his
// body as it is now, so the belly bulge, the stomach and the gape are shaded properly. On its own it watches every
// vertex and rebuilds both men's maps every second or two all through a scene. Here Anatomica asks it for an update
// (its QUpdateNormalmap) when the bulge / stomach / gape has really changed - at most every few seconds while it
// moves, and once more when it settles - so MuDynamicNormalMap's own real-time detection can be turned down or off.
// Anatomica also ships anatomica_n.dds: the fine folds and wrinkles of its own normal map as a detail map that
// MuDynamicNormalMap puts back on top (otherwise the 701-vertex mesh loses them).
namespace Mu {
    bool Installed();
    void Tick(const std::vector<RE::Actor*>& all, double now);   // from the gape engine's tick
    bool RenderSection();                    // Integrations page
    void Ini(CSimpleIniA& ini, bool save);   // [MuDynamicNormalMap]
}

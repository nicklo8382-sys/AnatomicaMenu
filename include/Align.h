#pragma once
#include <SimpleIni.h>
#include <vector>

// 2.5v: a man (or a futa) as the bottom in an animation made for a woman.
//  * M/F animations aim the top's schlong at a vagina. A man has none, so PPA finds nothing and the schlong ends up
//    between his thighs. While OStim has him as the target of vaginal sex, his body is moved by the gap between where
//    a vagina would be and his anus (a few units, eased in and out), so his anus sits where the animation aims and PPA
//    takes him anally as usual. Futas too, when PPA sends futas to the anus (its FutasUseAnus).
//  * The bottom's own schlong in PPA: while he is taken it can be switched off in PPA (an invisible body morph,
//    "Anatomica PPA Bottom", read by Anatomica's PPA override file anatomica_bottom.toml), so it can't hook the top.
//    It stays on when the scene gives it something to do (he penetrates someone, he is sucked / stroked...), and
//    for any OStim scene put on the "let it work here" list.
// OStim only (its action factions say who does what).
namespace Align {
    void Tick(const std::vector<RE::Actor*>& all, float dt, double now);   // from the gape engine's tick (main thread)
    bool RenderSection();                    // During sex page (true = a setting changed)
    void Ini(CSimpleIniA& ini, bool save);   // [BottomAlign] in AnatomicaMenu.ini
}

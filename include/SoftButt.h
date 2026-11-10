#pragma once
#include <SimpleIni.h>
#include <vector>

// Soft butt (SMP), optional and off by default: an invisible "SMP object" (AnatomicaSoftButt.esp, installer option)
// worn by the men chosen here hands his butt bones to FSMP - softer, heavier jiggle that collides with hands and
// partners. Heavy on performance. Those men are taken off CBPC's butt jiggle (CBPCMasterConfig_HIMBO.txt) so the two
// never fight - CBPC reads that when the game starts, so a change of "who" needs a restart.
namespace SoftButt {
    void Ini(CSimpleIniA& ini, bool save);   // [SoftButt] in AnatomicaMenu.ini
    void Tick(RE::Actor* player, const std::vector<RE::Actor*>& men);   // put it on / take it off (game thread)
    void RenderSection();                    // on the Jiggle page
}

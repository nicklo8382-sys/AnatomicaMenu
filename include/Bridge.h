#pragma once
#include <SimpleIni.h>

// Anatomica and other mods: SkyrimNet (what NPCs know and see, memories, reactions, actions, scene awareness),
// SLSF Reloaded (anal fame), SexLab Aroused / OSL Aroused (arousal -> wetness, twitchiness; his first time arouses
// him) and Bathing in Skyrim (washing dries him, soothes him and counts as rest). Everything is optional and does
// nothing when its mod is not installed. Menu: the "Integrations" page.
namespace Bridge {
    void Ini(CSimpleIniA& ini, bool save);                   // [Integrations] in AnatomicaMenu.ini
    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm);   // AnatomicaNative.SN* (SkyrimNet decorators and actions)
    void OnGameLoaded();                                     // a save was loaded / new game: register with SkyrimNet
    void Tick(RE::Actor* player, const std::vector<RE::Actor*>& all);   // Engine's update (main thread); paces itself

    // from Virgin.cpp (never called with its lock held)
    void Taken(RE::FormID man, RE::FormID partner, bool first, std::uint32_t lostBefore, RE::FormID prevBy, RE::FormID everBy);
    void Restored(RE::FormID man);

    void __stdcall RenderPage();
}

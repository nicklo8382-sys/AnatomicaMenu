#pragma once

// Anus colour, skin-blend overlay matching and after-sex looks (wetness, tender reddening).
// The DLL decides what each man should look like; the gape script applies it with RaceMenu
// (NiOverride) every few seconds. Menu: its own section, "Anatomica Colour".
namespace Colour {
    bool ConfirmButton(const char* label);   // button with an "Are you sure?" step
    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm);
    void Reset();                       // a save was loaded / new game
    void __stdcall RenderColours();
    void __stdcall RenderRaces();
    void __stdcall RenderRules();
    void __stdcall RenderAfter();
}

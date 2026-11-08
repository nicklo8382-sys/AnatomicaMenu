#pragma once
#include <SimpleIni.h>
#include <string>

// Optional extras, kept out of the main download: they only switch on when the separate "Anatomica Extras" mod is
// installed (it ships Data/SKSE/Plugins/AnatomicaMenu/Extras.ini, which also holds the keyword / faction lists).
//  * Revealing armour: a man whose armour leaves his backside bare (Advanced Nudity Detection / SexLab Aroused
//    keywords) counts as naked for what people see, the inspect action and the glossy skin.
//  * How it happened: taken while a captive (slavery, bondage, the slave auction...) or for money (Public Whore,
//    The Ancient Profession): memories and reactions say so, and a captive is left sorer for longer.
//  * Slave Auction: the house knows what it is selling - SkyrimNet gets his virginity / experience as a lot.
namespace Extras {
    bool On();                                   // the extras mod is installed
    void Ini(CSimpleIniA& ini, bool save);       // [Extras] in AnatomicaMenu.ini (the settings; lists are in Extras.ini)
    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm);   // AnatomicaNative.SNLot
    void RegisterSN(RE::BSScript::Internal::VirtualMachine* vm);   // the anatomica_lot decorator (Bridge, on load)

    bool Exposed(RE::Actor* a);                  // nothing covers his backside (naked, or revealing armour)

    enum class How { normal, captive, paid };
    How HowNow(RE::Actor* man, RE::Actor* partner);   // checked when he is taken (cached a few seconds)
    struct Rough { float tender = 1.0f, time = 1.0f; };
    Rough RoughFor(RE::Actor* man, RE::Actor* partner);   // soreness for this round (neutral unless captive)

    std::string Lot(RE::Actor* a);               // Slave Auction: what the house tells buyers ("" = not a lot)

    void __stdcall RenderPage();                 // the "Extras" page (only added when On())
}

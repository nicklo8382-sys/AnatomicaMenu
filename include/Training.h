#pragma once
#include <SimpleIni.h>
#include <string>

// 2.6j: Training - how big a man has been conditioned to take comfortably (kept with the save). It grows with time being
// taken (faster with a partner bigger than he is trained for, and going deep) and while he wears a plug, and fades when he
// is left alone. A trained man opens faster, gets less sore and his gape closes sooner; DDF / SkyrimNet hear about it.
// Also here: the belly inflation kept with the save (repeated stuffing leaves his belly a little bigger for a while), or,
// with "Lasting belly" off, each load makes his belly grow a little faster for that filling (+5% a load, stacking).
namespace Training {
    void Install();                                   // SKSE load: the worker
    void Reset();
    void Ini(CSimpleIniA& ini, bool save);            // [Training]
    void Save(SKSE::SerializationInterface* a);
    bool Load(SKSE::SerializationInterface* a, std::uint32_t type, std::uint32_t ver, std::uint32_t len);
    void Revert();
    void Apply(RE::FormID id, float& open, float& build, float& close, float& tender);   // Virgin::For: the training's effect
    float Level(RE::FormID id);                       // 0 (XS) .. 5 (beyond XL), -1 = not tracked
    const char* Word(float level);                    // "a small one" .. "anything"
    bool TooBig(RE::FormID id, int partnerCat);       // the partner is well past what he is trained for
    // belly inflation (OStrandsLink)
    void Stuffed(RE::FormID id, float loads, bool colossal);   // a load went in
    float Lasting(RE::FormID id);                     // extra belly kept with the save (0..)
    float Faster(RE::FormID id);                      // x the belly grows from what he holds (1 = normal)
    void Emptied(RE::FormID id);                      // he is empty again: the faster growth starts over
    // 2.6j: Vampire Lords (OStim Beast Form)
    void Bred(RE::FormID id, RE::Actor* giver);       // a load went in (counts only a Vampire Lord's: thrall, cold seed)
    float LordLevel(RE::FormID id);                   // how used he is to Vampire Lords, 0..5
    float Thrall(RE::FormID id);                      // breedings by Vampire Lords once he is a thrall (0 = not one)
    bool Cold(RE::FormID id);                         // a Vampire Lord's cold seed is in him
    float ColdHold(RE::FormID id);                    // x how fast it runs out of him (1 = normal)
    float Bond(RE::FormID id);                        // the blood bond glow on a thrall now (0 = none)
    RE::FormID Scent(RE::FormID id);                  // the werewolf whose scent is on him (0 = none)
    void RenderTraining();                            // Virginity page section
    void RenderInflation();                           // OStrands belly section
    std::string Status(RE::Actor* a);
}

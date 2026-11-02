#include "Wink.h"
#include "Colour.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

// ---------------------------------------------------------------------------
//  After-sex motion of the open anus, done with the anus bones every frame.
//
//  The gape is the "Anatomica Gape" body morph, which is exactly "the four NPC xx Anus2 bones moved
//  outward by 2 units x gape". Moving those bones INWARD by 2 x gape x closure narrows it smoothly,
//  at frame rate (the Papyrus script can only change morphs a few times a second).
//
//  Two layers, both scaled by the current gape (so they fade out as it closes):
//   * breathing - a slow, irregular tighten/relax of ~15% (three out-of-step sine waves; the sides
//                 and the top/bottom slightly out of sync so the shape changes a little)
//   * clench    - every few seconds (the script decides when) a quick partial squeeze (~40% closed
//                 in 0.25 s) that lets go slowly over ~1.6 s, like a muscle twitch
//  Only after the scene: the script sends gape 0 while the actor is in a scene (PPA owns the bones
//  then). When there is nothing to do the bones are put back exactly where they were.
// ---------------------------------------------------------------------------
namespace {
    using clock = std::chrono::steady_clock;

    // opening directions in the actor's (skin) space - same as the gape morph was built with
    const RE::NiPoint3 kF1{0.99999964f, -0.00066655f, -0.00053324f};
    const RE::NiPoint3 kF2{-0.00040875f, -0.92233755f, 0.38638488f};
    constexpr float kA = 2.0f;  // morph units per unit of gape

    enum Bone { LB, RB, LT, RT, kBones };
    const char* kBoneNames[kBones] = {"NPC LB Anus2", "NPC RB Anus2", "NPC LT Anus2", "NPC RT Anus2"};

    RE::NiPoint3 Dir(int b) {
        switch (b) {
            case LB: return {-kF1.x, -kF1.y, -kF1.z};
            case RB: return kF1;
            case RT: return kF2;
            default: return {-kF2.x, -kF2.y, -kF2.z};
        }
    }

    struct State {
        float gTarget = 0.0f;    // gape from the script (0 while in a scene)
        float g = 0.0f;          // smoothed gape used for the bones
        float time = 0.0f;       // running clock for the breathing
        float phase[3] = {};     // per-actor random phases, so actors don't breathe in sync
        float clenchT = -1.0f;   // seconds into the current clench, -1 = none
        float clenchAmp = 0.0f;
        float sinceSet = 0.0f;   // seconds since the script last sent the gape
        bool touched = false;    // bones currently offset by us
        bool logged = false;
        RE::NiPoint3 rest[kBones];
    };

    std::mutex g_lock;
    std::unordered_map<RE::FormID, State> g_states;
    std::mt19937 g_rng{std::random_device{}()};

    constexpr float kAttack = 0.25f, kHoldTop = 0.08f, kRelease = 1.6f;
    constexpr float kClenchEnd = kAttack + kHoldTop + kRelease;

    float SmootherStep(float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
    }

    // settings from AnatomicaGape.esp (written by the menu)
    float GlobalOr(RE::FormID id, float def) {
        if (auto* dh = RE::TESDataHandler::GetSingleton())
            if (auto* g = dh->LookupForm<RE::TESGlobal>(id, "AnatomicaGape.esp")) return g->value;
        return def;
    }

    // slow irregular 0..1 wave: three sines that never line up
    float Breath(const State& s, float offset) {
        constexpr float tau = 6.2831853f;
        const float t = s.time;
        const float v = 0.5f * std::sin(tau * 0.21f * t + s.phase[0] + offset) +
                        0.3f * std::sin(tau * 0.34f * t + s.phase[1] + 1.7f * offset) +
                        0.2f * std::sin(tau * 0.09f * t + s.phase[2]);
        return 0.5f + 0.5f * v;
    }

    float Clench(const State& s) {
        if (s.clenchT < 0.0f) return 0.0f;
        const float t = s.clenchT;
        if (t < kAttack) return s.clenchAmp * SmootherStep(t / kAttack);
        if (t < kAttack + kHoldTop) return s.clenchAmp;
        return s.clenchAmp * (1.0f - SmootherStep((t - kAttack - kHoldTop) / kRelease));
    }

    // closure per bone (0 = as the gape morph has it, 1 = fully shut)
    void Closure(const State& s, float breathAmp, float out[kBones]) {
        const float cl = Clench(s);
        const float side = breathAmp * Breath(s, 0.0f);
        const float slit = breathAmp * Breath(s, 0.6f);   // top/bottom a little out of step with the sides
        out[LB] = out[RB] = std::min(0.95f, side + cl);
        out[LT] = out[RT] = std::min(0.95f, slit + cl * 0.85f);
    }

    RE::NiPoint3 Mul(const RE::NiMatrix3& m, const RE::NiPoint3& v) {
        return {m.entry[0][0] * v.x + m.entry[0][1] * v.y + m.entry[0][2] * v.z,
                m.entry[1][0] * v.x + m.entry[1][1] * v.y + m.entry[1][2] * v.z,
                m.entry[2][0] * v.x + m.entry[2][1] * v.y + m.entry[2][2] * v.z};
    }
    RE::NiPoint3 MulT(const RE::NiMatrix3& m, const RE::NiPoint3& v) {
        return {m.entry[0][0] * v.x + m.entry[1][0] * v.y + m.entry[2][0] * v.z,
                m.entry[0][1] * v.x + m.entry[1][1] * v.y + m.entry[2][1] * v.z,
                m.entry[0][2] * v.x + m.entry[1][2] * v.y + m.entry[2][2] * v.z};
    }

    // runs on the game thread
    void Apply(float dt) {
        const float breathAmp = std::clamp(GlobalOr(0x812, 0.15f), 0.0f, 0.6f);
        const float breathSpeed = std::clamp(GlobalOr(0x813, 1.0f), 0.1f, 4.0f);
        std::lock_guard lk(g_lock);
        for (auto it = g_states.begin(); it != g_states.end();) {
            auto& s = it->second;
            s.sinceSet += dt;
            s.time += dt * breathSpeed;
            // follow the script's gape smoothly (it only updates a few times a second)
            const float k = 1.0f - std::exp(-dt / 0.15f);
            s.g += (s.gTarget - s.g) * k;
            if (s.clenchT >= 0.0f) {
                s.clenchT += dt;
                if (s.clenchT >= kClenchEnd) s.clenchT = -1.0f;
            }
            const bool stale = s.sinceSet > 3.0f;   // script stopped talking to us
            if (stale) s.gTarget = 0.0f, s.clenchT = -1.0f;

            float c[kBones];
            Closure(s, breathAmp, c);
            const bool active = s.g > 0.002f && (c[LB] > 0.0005f || c[LT] > 0.0005f);
            if (!active && !s.touched) {          // nothing to do: don't even look the bones up
                if (stale) { it = g_states.erase(it); continue; }
                ++it;
                continue;
            }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->first);
            RE::NiAVObject* root = actor ? actor->Get3D(false) : nullptr;
            RE::NiAVObject* nodes[kBones] = {};
            bool ok = root != nullptr;
            for (int b = 0; ok && b < kBones; ++b) {
                nodes[b] = root->GetObjectByName(kBoneNames[b]);
                ok = nodes[b] && nodes[b]->parent;
            }
            if (!ok) {
                if (!s.logged) {
                    logger::warn("Anatomica motion: {:08X} has no 3D or no NPC xx Anus2 bones", it->first);
                    s.logged = true;
                }
                s.touched = false;
                if (stale || !actor) { it = g_states.erase(it); continue; }
                ++it;
                continue;
            }
            if (active) {
                if (!s.touched) {
                    logger::info("Anatomica motion: moving the anus bones of {} (gape {:.2f})", actor->GetName(), s.g);
                    for (int b = 0; b < kBones; ++b) s.rest[b] = nodes[b]->local.translate;
                    s.touched = true;
                }
                const auto& rw = root->world;
                for (int b = 0; b < kBones; ++b) {
                    const float amt = -kA * s.g * c[b];
                    const RE::NiPoint3 d = Dir(b);
                    RE::NiPoint3 w = Mul(rw.rotate, RE::NiPoint3{d.x * amt, d.y * amt, d.z * amt});
                    w.x *= rw.scale; w.y *= rw.scale; w.z *= rw.scale;
                    const auto& pw = nodes[b]->parent->world;
                    RE::NiPoint3 l = MulT(pw.rotate, w);
                    const float inv = pw.scale > 1e-6f ? 1.0f / pw.scale : 1.0f;
                    nodes[b]->local.translate = {s.rest[b].x + l.x * inv, s.rest[b].y + l.y * inv, s.rest[b].z + l.z * inv};
                }
            } else if (s.touched) {
                for (int b = 0; b < kBones; ++b) nodes[b]->local.translate = s.rest[b];
                s.touched = false;
                logger::info("Anatomica motion: anus bones of {} back at rest", actor->GetName());
            }
            if (stale && !s.touched) { it = g_states.erase(it); continue; }
            ++it;
        }
    }


    void Loop() {
        auto last = clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            auto now = clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            {
                std::lock_guard lk(g_lock);
                if (g_states.empty()) continue;
            }
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->GameIsPaused()) continue;
            dt = std::min(dt, 0.1f);
            if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([dt]() { Apply(dt); });
        }
    }

    // ---- Papyrus: AnatomicaNative ----
    void SetGape(RE::StaticFunctionTag*, RE::Actor* a, float g) {
        if (!a) return;
        std::lock_guard lk(g_lock);
        auto found = g_states.find(a->GetFormID());
        if (found == g_states.end() && g < 0.01f) return;   // closed and untracked: nothing to do
        if (found == g_states.end()) {
            logger::info("Anatomica motion: after-sex gape {:.2f} on {}", g, a->GetName());
            auto& n = g_states[a->GetFormID()];
            std::uniform_real_distribution<float> ph(0.0f, 6.2831853f);
            for (auto& p : n.phase) p = ph(g_rng);
        }
        auto& s = g_states[a->GetFormID()];
        s.gTarget = std::max(0.0f, g);
        s.sinceSet = 0.0f;
    }
    // "Blink" from the script = time for a clench
    void Blink(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!a) return;
        std::lock_guard lk(g_lock);
        auto it = g_states.find(a->GetFormID());
        if (it == g_states.end()) return;
        auto& s = it->second;
        if (s.clenchT >= 0.0f) return;
        const float strength = std::clamp(GlobalOr(0x811, 0.4f), 0.0f, 0.9f);
        std::uniform_real_distribution<float> var(0.7f, 1.0f);
        s.clenchAmp = strength * var(g_rng);
        s.clenchT = 0.0f;
        logger::info("Anatomica motion: clench on {} ({:.0f}% closed)", a->GetName(), s.clenchAmp * 100.0f);
    }
    bool IsBlinking(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!a) return false;
        std::lock_guard lk(g_lock);
        auto it = g_states.find(a->GetFormID());
        return it != g_states.end() && it->second.clenchT >= 0.0f;
    }
    int Version(RE::StaticFunctionTag*) { return 2; }

    bool RegisterFuncs(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("SetGape"sv, "AnatomicaNative"sv, SetGape);
        vm->RegisterFunction("Blink"sv, "AnatomicaNative"sv, Blink);
        vm->RegisterFunction("IsBlinking"sv, "AnatomicaNative"sv, IsBlinking);
        vm->RegisterFunction("Version"sv, "AnatomicaNative"sv, Version);
        Colour::RegisterFuncs(vm);
        return true;
    }
}

namespace Wink {
    void Register() {
        const bool ok = SKSE::GetPapyrusInterface()->Register(RegisterFuncs);
        logger::info("Anatomica motion: Papyrus functions (AnatomicaNative) {}", ok ? "registered" : "FAILED to register");
        std::thread(Loop).detach();
    }
    void Reset() {
        // a save was loaded: bones come fresh from the new 3D, forget everything
        {
            std::lock_guard lk(g_lock);
            g_states.clear();
        }
        Colour::Reset();
    }
}

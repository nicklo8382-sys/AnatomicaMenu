#include "Wink.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_map>

// ---------------------------------------------------------------------------
//  Smooth wink, done with the anus bones every frame (like an eyelid blink)
//
//  The gape itself is the "Anatomica Gape" body morph, which the Papyrus script can only update a
//  few times a second (that is why the old morph-based wink looked like a slideshow). The gape
//  morph is exactly "the four NPC xx Anus2 bones moved outward by 2 units x gape" (that is how it
//  was built), so moving those bones INWARD by 2 x gape x closure cancels it precisely - and bones
//  can be moved every frame. The script tells us the current gape (SetGape) and when to blink
//  (Blink); everything else happens here at frame rate.
//
//  Blink: the sides (LB/RB) close to a slit, the slit (LT/RT) shuts, a short hold, then it reopens
//  in reverse - but only to 75% of what was open before, so every blink leaves it a bit tighter.
//  The step-down resets when the gape is gone (closed or penetrated again).
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
        float gTarget = 0.0f;    // gape from the script
        float g = 0.0f;          // smoothed gape used for the bones
        float residual = 0.0f;   // closure kept from earlier blinks (0 = full gape, 1 = shut)
        float blinkT = -1.0f;    // seconds into the current blink, -1 = none
        float r0 = 0.0f, r1 = 0.0f;
        float sinceSet = 0.0f;   // seconds since the script last sent the gape
        bool touched = false;    // bones currently offset by us
        RE::NiPoint3 rest[kBones];
        bool haveRest = false;
    };

    std::mutex g_lock;
    std::unordered_map<RE::FormID, State> g_states;

    constexpr float kOpen = 0.6f, kShut = 0.95f, kHold = 1.15f, kSlit = 1.5f, kEnd = 2.2f;

    float SmootherStep(float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
    }

    // closure per bone (0 = as the gape morph has it, 1 = fully shut)
    void Closure(const State& s, float out[kBones]) {
        float side = s.residual, slit = s.residual;
        if (s.blinkT >= 0.0f) {
            const float t = s.blinkT;
            if (t < kOpen) {
                side = s.r0 + (1.0f - s.r0) * SmootherStep(t / kOpen);
                slit = s.r0;
            } else if (t < kShut) {
                side = 1.0f;
                slit = s.r0 + (1.0f - s.r0) * SmootherStep((t - kOpen) / (kShut - kOpen));
            } else if (t < kHold) {
                side = slit = 1.0f;
            } else if (t < kSlit) {
                side = 1.0f;
                slit = 1.0f - (1.0f - s.r1) * SmootherStep((t - kHold) / (kSlit - kHold));
            } else {
                slit = s.r1;
                side = 1.0f - (1.0f - s.r1) * SmootherStep((t - kSlit) / (kEnd - kSlit));
            }
        }
        out[LB] = out[RB] = side;
        out[LT] = out[RT] = slit;
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
        std::lock_guard lk(g_lock);
        for (auto it = g_states.begin(); it != g_states.end();) {
            auto& s = it->second;
            s.sinceSet += dt;
            // follow the script's gape smoothly (it only updates a few times a second)
            const float k = 1.0f - std::exp(-dt / 0.12f);
            s.g += (s.gTarget - s.g) * k;
            if (s.blinkT >= 0.0f) {
                s.blinkT += dt;
                if (s.blinkT >= kEnd) {
                    s.blinkT = -1.0f;
                    s.residual = s.r1;
                }
            }
            if (s.gTarget < 0.01f && s.blinkT < 0.0f) s.residual = 0.0f;   // gape gone: start fresh
            const bool stale = s.sinceSet > 3.0f;                             // script stopped talking to us
            if (stale) s.gTarget = 0.0f, s.blinkT = -1.0f, s.residual = 0.0f;

            float c[kBones];
            Closure(s, c);
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
                s.touched = false;
                s.haveRest = false;
                if (stale || !actor) { it = g_states.erase(it); continue; }
                ++it;
                continue;
            }
            if (active) {
                if (!s.touched) {
                    for (int b = 0; b < kBones; ++b) s.rest[b] = nodes[b]->local.translate;
                    s.haveRest = true;
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
        if (found == g_states.end() && g < 0.01f) return;   // closed and never blinked: nothing to track
        auto& s = g_states[a->GetFormID()];
        s.gTarget = std::max(0.0f, g);
        s.sinceSet = 0.0f;
    }
    void Blink(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!a) return;
        std::lock_guard lk(g_lock);
        auto& s = g_states[a->GetFormID()];
        if (s.blinkT >= 0.0f) return;
        s.r0 = s.residual;
        s.r1 = 1.0f - (1.0f - s.r0) * 0.75f;   // reopens to 75% of what was open
        s.blinkT = 0.0f;
        s.sinceSet = 0.0f;
    }
    bool IsBlinking(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!a) return false;
        std::lock_guard lk(g_lock);
        auto it = g_states.find(a->GetFormID());
        return it != g_states.end() && it->second.blinkT >= 0.0f;
    }
    int Version(RE::StaticFunctionTag*) { return 1; }

    bool RegisterFuncs(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("SetGape"sv, "AnatomicaNative"sv, SetGape);
        vm->RegisterFunction("Blink"sv, "AnatomicaNative"sv, Blink);
        vm->RegisterFunction("IsBlinking"sv, "AnatomicaNative"sv, IsBlinking);
        vm->RegisterFunction("Version"sv, "AnatomicaNative"sv, Version);
        return true;
    }
}

namespace Wink {
    void Register() {
        SKSE::GetPapyrusInterface()->Register(RegisterFuncs);
        std::thread(Loop).detach();
    }
    void Reset() {
        // a save was loaded: bones come fresh from the new 3D, forget everything
        std::lock_guard lk(g_lock);
        g_states.clear();
    }
}

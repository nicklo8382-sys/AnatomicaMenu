#include "Face.h"
#include "Colour.h"
#include "Engine.h"
#include "Fluid.h"
#include "Lang.h"
#include "Size.h"
#include "Sounds.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Face {
    namespace {
        constexpr std::uint32_t kRec = 'ANOR';
        constexpr std::uint32_t kPhBigAah = 1;                          // phoneme "BigAah"
        constexpr std::uint32_t kModBrowInL = 4, kModBrowInR = 5, kModSquintL = 12, kModSquintR = 13;

        struct Settings {
            bool on = true;
            bool jaw = true;
            bool toDeepFit = true;        // 2.6n: with SMP Head - DeepFit installed the mouth (jaw, lips, throat) is DeepFit's alone - it opens it to the schlong
            float jawMax = 0.8f;          // how wide (0-1 of the "big aah" shape) for the thickest schlong
            float jawMin = 0.6f;          // 2.6k: at least this wide for any schlong in his mouth (a slim one still needs room)
            bool throat = false;          // experimental: neck bone scale
            float throatMax = 0.10f;      // +10% neck at most
            float deepFrom = 4.0f;        // units past his lips that count as deep (in his throat)
            bool throatBody = true;       // 2.6y TEST: the body's throat bulge (Anatomica Throat 1-4, only in the "(Throat Test)" BodySlide set)
            float throatBodyMax = 1.0f;   // how strong (1 = as built), for the thickest schlong
            float throatReach = 0.0f;     // counts the schlong this much longer (units) - for when it stops short of his neck
            bool exp = true;
            bool gag = true;
            float gagChance = 0.5f;       // chance each time it goes deep (a beginner, an average schlong)
            bool gagByForce = true;       // 2.6k: rougher (faster, deeper strokes) gags more, gentle less
            float gagForce = 1.0f;        //   ... how much (0 = not at all, 1 = from x0.35 gentle to x1.6 rough)
            bool gagSound = true;         // 2.6k: a gag sound (Data\Sound\Fx\Anatomica\gag, when there are files)
            float gagVol = 1.0f;
            float gagTime = 1.4f;         // seconds a gag lasts
            bool blush = true;            // OBlush
            bool noTongue = true;         // 2.6k: no OStim tongue (Ahegao Expressions and the like) while his mouth is full
            float blushBoost = 0.35f;     // stronger by this much (alpha)
            float blushSecs = 8.0f;
            bool gulp = true;             // a gulp when he swallows (OStrands 0.8.0.8+ tells us)
            float gulpVol = 1.0f;
            // 2.6j: how his face looks (layers drawn from his own head mesh)
            bool looks = true;
            bool lipWet = true;  float lipWetSecs = 90.0f;     // wet, glossy lips after oral
            bool lipRed = true;  float lipRedMins = 4.0f;      // minutes of sucking to full red; fades over 10x that
            bool flush = true;   bool flushOBlush = false;     // a flush on his cheeks as he gets excited (not with OBlush unless ticked)
            float flushMax = 1.0f;
            bool tears = true;   float tearsSecs = 120.0f;     // gagging brings tears
            bool faceCum = true; float faceCumHours = 2.0f;    // cum on his face (a messy mouth, a facial)
            bool sweat = true;   float sweatMax = 1.0f;        // he sweats as he gets excited (face and body)
            float sweatSecs = 120.0f;                          // 2.6u: how long it takes to dry after
            float fitScale = 1.0f, fitUp = 0.0f, fitOut = 0.0f;
        } g;
        struct Look {
            float v[Fluid::kFaceLayers]{};
            float rgb[3] = {0.96f, 0.95f, 0.90f};
            double cumGameH = -1.0;       // game hour the cum went on
            float cumStart = 0.0f;
            float sent[Fluid::kFaceLayers]{};
            double wetUntil = 0.0;
        };
        std::unordered_map<RE::FormID, Look> g_look;
        RE::TESFaction* g_exciteF = nullptr;
        double g_testUntil = 0.0; RE::FormID g_testId = 0; int g_testLayer = -1;   // 2.6k: -1 = every layer
        // 2.6j: each race can have its own fitting (a werewolf's muzzle, a khajiit) - "Plugin|ID" of the race
        std::map<std::string, Fluid::FaceFit> g_raceFit;
        std::string RaceKey(RE::Actor* a) {
            auto* r = a ? a->GetRace() : nullptr;
            const auto* f = r ? r->GetFile(0) : nullptr;
            return (r && f) ? std::format("{}|{:06X}", f->GetFilename(), r->GetLocalFormID()) : std::string{};
        }
        const Fluid::FaceFit* FitFor(RE::Actor* a) {
            auto it = g_raceFit.find(RaceKey(a));
            return it == g_raceFit.end() ? nullptr : &it->second;
        }

        struct Exp { float exp = 0.0f; std::uint32_t times = 0, gags = 0; };
        struct State {
            RE::FormID partner = 0;
            float jaw = 0.0f, jawTarget = 0.0f;
            float gag = 0.0f;             // 0-1 how strong the gag face is now
            double gagUntil = 0.0, lastSeen = 0.0, lastOral = -1e9, blushUntil = 0.0;
            float throatSent = 0.0f;
            float throatCur = 0.0f, throatWant = 0.0f;   // 2.6j: eased - it swells over a few strokes and goes down slowly
            double deepAt = -1e9;
            float thrCur[4]{}, thrSent[4]{}, thrWant[4]{};   // 2.6y: the body's throat sections (0-1 x strength), eased
            float thrReach = -100.0f;     // how far below the neck seam the schlong reaches (mesh units, for the log / menu)
            float depth = -100.0f;
            bool deep = false, blushing = false;
            float faceSent = -1.0f;
            double tongueAt = 0.0;        // 2.6k: when we last took a tongue out of his mouth
            float lastDepth = -100.0f, force = 0.0f;   // 2.6k: how hard he is being used (stroke speed, eased)
        };
        void NoTongue(RE::Actor* a) {   // OStim's "tongue" equip object off him (OActor.UnequipObject, as the mods that put it on use)
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm || !a) return;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
            vm->DispatchStaticCall("OActor", "UnequipObject", RE::MakeFunctionArguments(static_cast<RE::Actor*>(a), RE::BSFixedString("tongue")), cb);
        }

        std::mutex g_mx;
        std::unordered_map<RE::FormID, State> g_st;
        std::unordered_map<RE::FormID, Exp> g_exp;
        std::unordered_map<RE::FormID, double> g_swallowAt;   // 2.6j: when he last swallowed (DDF)
        struct Gulps { float owed = 0.0f; int left = 0; double next = 0.0; };
        std::unordered_map<RE::FormID, Gulps> g_gulps;   // swallowed, gulps still to make
        std::atomic<bool> g_any{false}, g_queued{false}, g_frameQueued{false};
        RE::TESFaction* g_mouthF = nullptr;
        RE::TESFaction* g_cockF = nullptr;
        bool g_looked = false;
        int g_oblush = -1;
        std::mt19937 g_rng{std::random_device{}()};

        double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
        float Rand() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng); }
        bool OBlush() {
            if (g_oblush < 0) if (auto* dh = RE::TESDataHandler::GetSingleton()) g_oblush = dh->LookupModByName("OBlush.esp") ? 1 : 0;
            return g_oblush == 1;
        }
        void Factions() {
            if (g_looked) return;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;
            g_looked = true;
            g_mouthF = dh->LookupForm<RE::TESFaction>(0xE52, "OStim.esp");   // OStimBlowjobActorFaction: the one with the mouth
            g_cockF = dh->LookupForm<RE::TESFaction>(0xE53, "OStim.esp");    // OStimBlowjobTargetFaction: the one with the schlong
            g_exciteF = dh->LookupForm<RE::TESFaction>(0xD93, "OStim.esp");  // OStimExcitementFaction: rank = excitement 0-100
            logger::info("Anatomica mouth & face: OStim blowjob factions {}", g_mouthF && g_cockF ? "found" : "NOT found (no oral detection)");
        }
        bool Male(RE::Actor* a) { auto* b = a ? a->GetActorBase() : nullptr; return b && b->GetSex() == RE::SEX::kMale; }
        RE::NiAVObject* Bone(RE::Actor* a, const char* n) { auto* r = a ? a->Get3D(false) : nullptr; return r ? r->GetObjectByName(RE::BSFixedString(n)) : nullptr; }
        bool MouthOf(RE::Actor* a, RE::NiPoint3& mouth, RE::NiPoint3& fwd) {
            auto* head = Bone(a, "NPC Head [Head]");
            auto* magic = Bone(a, "NPC Head MagicNode [Hmag]");
            if (!head || !magic) return false;
            fwd = magic->world.translate - head->world.translate;
            if (fwd.Unitize() < 1e-3f) return false;
            mouth = head->world.translate + (magic->world.translate - head->world.translate) * 0.7f;
            return true;
        }
        bool TipOf(RE::Actor* p, RE::NiPoint3& tip) {
            auto* g6 = Bone(p, "NPC Genitals06 [Gen06]");
            if (!g6) return false;
            tip = g6->world.translate + g6->world.rotate * RE::NiPoint3{0.0f, 1.1f * g6->world.scale, 0.0f};
            return true;
        }
        // 2.6y: how far the schlong reaches down the front of his neck, in his body mesh's own units (0 = the neck seam,
        // 3 = the end of Anatomica Throat 1, 12 = the end of 4); -100 = not in his neck. The body's neck is skinned to
        // NPC Spine2 alone, so a point taken into that bone's frame and back through its bind pose lands on the mesh as built.
        float ThroatReach(RE::Actor* a, RE::Actor* p, float extra) {
            auto* sp2 = Bone(a, "NPC Spine2 [Spn2]");
            if (!sp2 || !p) return -100.0f;
            static const char* kG[6] = {"NPC Genitals01 [Gen01]", "NPC Genitals02 [Gen02]", "NPC Genitals03 [Gen03]",
                                        "NPC Genitals04 [Gen04]", "NPC Genitals05 [Gen05]", "NPC Genitals06 [Gen06]"};
            std::vector<RE::NiPoint3> pts;
            for (auto* n : kG) if (auto* b = Bone(p, n)) pts.push_back(b->world.translate);
            RE::NiPoint3 tip;
            if (pts.size() < 2 || !TipOf(p, tip)) return -100.0f;
            pts.push_back(tip);
            if (extra > 0.0f) { RE::NiPoint3 d = tip - pts[pts.size() - 2]; if (d.Unitize() > 1e-3f) pts.push_back(tip + d * extra); }
            const RE::NiTransform inv = sp2->world.Invert();
            // HIMBO's Spine2 bind (skin-to-bone): local = R * v + t
            constexpr float c = 0.99103f, s_ = 0.13361f, ty = 18.07073f, tz = -89.63808f;
            auto mesh = [&](const RE::NiPoint3& w) {
                const RE::NiPoint3 l = inv * w;
                const float ly = l.y - ty, lz = l.z - tz;
                return RE::NiPoint3{l.x, c * ly + s_ * lz, -s_ * ly + c * lz};   // R transposed
            };
            float best = -100.0f;
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                const RE::NiPoint3 m0 = mesh(pts[i]), m1 = mesh(pts[i + 1]);
                const int n = std::clamp(static_cast<int>((m1 - m0).Length() / 0.5f) + 1, 1, 64);
                for (int j = 0; j <= n; ++j) {
                    const RE::NiPoint3 m = m0 + (m1 - m0) * (static_cast<float>(j) / n);
                    if (std::abs(m.x) > 4.0f || m.y < -7.0f || m.y > 5.0f || m.z < 94.0f || m.z > 112.0f) continue;   // in his neck / under the top of his chest
                    best = std::max(best, 109.6f - m.z);
                }
            }
            return best;
        }
        void ThroatBody(RE::Actor* a, State& s, float dt, bool clear) {   // the sections eased to what they want, sent when they change
            static const char* kN[4] = {"Anatomica Throat 1", "Anatomica Throat 2", "Anatomica Throat 3", "Anatomica Throat 4"};
            if (clear) {
                bool any = false;
                for (int i = 0; i < 4; ++i) { any |= s.thrSent[i] > 0.0f; s.thrCur[i] = s.thrSent[i] = s.thrWant[i] = 0.0f; }
                if (any && a) { Engine::ClearMorphKey(a, "AnatomicaThroat"); Engine::UpdateBody(a); }
                return;
            }
            bool ch = false;
            const float k = std::min(1.0f, dt * 5.0f);
            for (int i = 0; i < 4; ++i) {
                s.thrCur[i] += (s.thrWant[i] - s.thrCur[i]) * k;
                if (s.thrWant[i] <= 0.0f && s.thrCur[i] < 0.01f) s.thrCur[i] = 0.0f;
                if (std::abs(s.thrCur[i] - s.thrSent[i]) > 0.02f || (s.thrCur[i] == 0.0f && s.thrSent[i] > 0.0f)) {
                    Engine::BodyMorph(a, kN[i], "AnatomicaThroat", s.thrCur[i]); s.thrSent[i] = s.thrCur[i]; ch = true;
                }
            }
            if (ch) Engine::UpdateBody(a);
        }
        float Thick(RE::Actor* p) {   // 0.45 (XS) .. 1 (XL)
            const int c = p ? Size::Cat(p) : Size::kUnknown;
            static const float k[5] = {0.45f, 0.6f, 0.75f, 0.9f, 1.0f};
            if (c == Size::kCreature) return 1.0f;   // 2.6j: a beast form (OStim Beast Form) or creature: the thickest
            return (c >= 0 && c <= 4) ? k[c] : 0.75f;
        }
        float ExpK(RE::FormID id) {   // 1 beginner .. 0.15 practised
            if (!g.exp) return 0.6f;
            auto it = g_exp.find(id);
            const float e = it == g_exp.end() ? 0.0f : it->second.exp;
            return 1.0f - 0.85f * std::clamp(e / 6.0f, 0.0f, 1.0f);
        }
        const char* Level(float e) { return e < 0.05f ? "never" : e < 1.0f ? "new to it" : e < 4.0f ? "used to it" : "practised"; }

        void ThroatScale(RE::Actor* a, float k) {   // neck scaled up, the head scaled back down (NiOverride, through Papyrus)
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm || !a) return;
            const bool fem = !Male(a);
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
            auto call = [&](const char* fn, const char* node, float s, bool remove) {
                RE::TESObjectREFR* ref = a;
                if (remove) {
                    auto* args = RE::MakeFunctionArguments(std::move(ref), false, bool(fem), RE::BSFixedString(node), RE::BSFixedString("Anatomica"));
                    vm->DispatchStaticCall("NiOverride", fn, args, cb);
                } else {
                    auto* args = RE::MakeFunctionArguments(std::move(ref), false, bool(fem), RE::BSFixedString(node), RE::BSFixedString("Anatomica"), float(s));
                    vm->DispatchStaticCall("NiOverride", fn, args, cb);
                }
            };
            auto update = [&](const char* node) {
                RE::TESObjectREFR* ref = a;
                auto* args = RE::MakeFunctionArguments(std::move(ref), false, bool(fem), RE::BSFixedString(node));
                vm->DispatchStaticCall("NiOverride", "UpdateNodeTransform", args, cb);
            };
            if (k <= 0.001f) {
                call("RemoveNodeTransformScale", "NPC Neck [Neck]", 0.0f, true);
                call("RemoveNodeTransformScale", "NPC Head [Head]", 0.0f, true);
            } else {
                call("AddNodeTransformScale", "NPC Neck [Neck]", 1.0f + k, false);
                call("AddNodeTransformScale", "NPC Head [Head]", 1.0f / (1.0f + k), false);
            }
            update("NPC Neck [Neck]");
            update("NPC Head [Head]");
        }

        std::vector<RE::Actor*> Nearby() {
            std::vector<RE::Actor*> v;
            if (auto* p = RE::PlayerCharacter::GetSingleton()) v.push_back(p);
            if (auto* pl = RE::ProcessLists::GetSingleton())
                for (auto& h : pl->highActorHandles) if (auto a = h.get(); a && a->Is3DLoaded()) v.push_back(a.get());
            return v;
        }

        float GameHours() { auto* c = RE::Calendar::GetSingleton(); return c ? c->GetHoursPassed() : 0.0f; }
        // 2.6j: flush and sweat from his excitement, everything fading, sent to the face layers (under g_mx)
        void Looks(float dt, double now) {
            const bool on = g.on && g.looks;
            if (on && g_exciteF) {
                for (RE::Actor* a : Nearby()) {
                    if (!Male(a) || !a->IsInFaction(g_exciteF)) continue;
                    const float ex = std::clamp(static_cast<float>(a->GetFactionRank(g_exciteF, a->IsPlayerRef())) / 100.0f, 0.0f, 1.0f);
                    if (ex <= 0.02f && !g_look.contains(a->GetFormID())) continue;
                    auto& L = g_look[a->GetFormID()];
                    const bool flushOk = g.flush && (!OBlush() || g.flushOBlush);
                    if (flushOk) L.v[Fluid::kFlush] = std::max(L.v[Fluid::kFlush], g.flushMax * std::clamp(ex * 1.2f - 0.1f, 0.0f, 1.0f));
                    if (g.sweat) {   // builds up through the scene, more as he nears the end
                        const float want = g.sweatMax * std::clamp(ex * 1.1f, 0.0f, 1.0f);
                        if (want > L.v[Fluid::kFaceSweat]) L.v[Fluid::kFaceSweat] = std::min(want, L.v[Fluid::kFaceSweat] + dt / 40.0f);
                        L.v[Fluid::kBodySweat] = L.v[Fluid::kFaceSweat];
                    }
                }
            }
            const float gh = GameHours();
            for (auto it = g_look.begin(); it != g_look.end();) {
                Look& L = it->second;
                auto* a = RE::TESForm::LookupByID<RE::Actor>(it->first);
                const bool exc = a && g_exciteF && a->IsInFaction(g_exciteF);
                if (now > L.wetUntil) L.v[Fluid::kLipWet] = std::max(0.0f, L.v[Fluid::kLipWet] - dt / 20.0f);
                L.v[Fluid::kLipRed] = std::max(0.0f, L.v[Fluid::kLipRed] - dt / (600.0f * std::max(g.lipRedMins, 0.2f)));
                if (!exc) {
                    L.v[Fluid::kFlush] = std::max(0.0f, L.v[Fluid::kFlush] - dt / 45.0f);
                    L.v[Fluid::kFaceSweat] = L.v[Fluid::kBodySweat] = std::max(0.0f, L.v[Fluid::kFaceSweat] - dt / std::max(g.sweatSecs, 5.0f));
                } else if (g_exciteF) {
                    const float ex = std::clamp(static_cast<float>(a->GetFactionRank(g_exciteF, a->IsPlayerRef())) / 100.0f, 0.0f, 1.0f);
                    const float fw = g.flushMax * std::clamp(ex * 1.2f - 0.1f, 0.0f, 1.0f);
                    if (L.v[Fluid::kFlush] > fw) L.v[Fluid::kFlush] = std::max(fw, L.v[Fluid::kFlush] - dt / 45.0f);   // (after he came the flush eases)
                }
                L.v[Fluid::kTears] = std::max(0.0f, L.v[Fluid::kTears] - dt / std::max(g.tearsSecs, 5.0f));
                if (L.cumGameH >= 0.0) {
                    const float left = 1.0f - static_cast<float>(gh - L.cumGameH) / std::max(g.faceCumHours, 0.1f);
                    L.v[Fluid::kFaceCum] = std::clamp(L.cumStart * left, 0.0f, 1.0f);
                    if (left <= 0.0f || gh < L.cumGameH) { L.cumGameH = -1.0; L.v[Fluid::kFaceCum] = 0.0f; }
                }
                L.v[Fluid::kLipWet] = L.v[Fluid::kLipRed] = 0.0f;   // 2.6v: gone
                if (!g.flush || (OBlush() && !g.flushOBlush)) L.v[Fluid::kFlush] = 0.0f;
                L.v[Fluid::kTears] = 0.0f;   // 2.6v: gone
                if (!g.faceCum) L.v[Fluid::kFaceCum] = 0.0f;
                if (!g.sweat) L.v[Fluid::kFaceSweat] = L.v[Fluid::kBodySweat] = 0.0f;
                float out[Fluid::kFaceLayers];
                bool any = false, changed = false;
                for (int i = 0; i < Fluid::kFaceLayers; ++i) {
                    out[i] = on ? L.v[i] : 0.0f;
                    if (now < g_testUntil && it->first == g_testId && (g_testLayer < 0 || g_testLayer == i)) out[i] = 1.0f;
                    any |= out[i] > 0.01f;
                    if (std::abs(out[i] - L.sent[i]) > 0.02f || (out[i] <= 0.01f && L.sent[i] > 0.0f)) changed = true;
                }
                if (changed) {
                    Fluid::FaceLook(it->first, out, L.rgb, FitFor(a));
                    for (int i = 0; i < Fluid::kFaceLayers; ++i) L.sent[i] = out[i] <= 0.01f ? 0.0f : out[i];
                }
                if (!any && !changed && !(now < g_testUntil && it->first == g_testId)) { it = g_look.erase(it); continue; }
                ++it;
            }
            if (now < g_testUntil && g_testId && !g_look.contains(g_testId)) g_look[g_testId];
        }

        // ~4 times a second: who is sucking whom, how deep, how wide his jaw should be, gags, experience
        bool DeepFitHere() { static const bool on = REX::W32::GetModuleHandleW(L"DeepFit.dll") != nullptr; return on; }
        void Scan(float dt) {
            Factions();
            std::lock_guard lk(g_mx);
            const double now = Now();
            if (!g.on || !g_mouthF || !g_cockF) {
                for (auto& [id, s] : g_st) { s.jawTarget = 0.0f; s.gagUntil = 0.0; }
            } else {
                const auto all = Nearby();
                for (RE::Actor* a : all) {
                    if (!Male(a) || !a->IsInFaction(g_mouthF)) continue;
                    RE::NiPoint3 mouth, fwd;
                    if (!MouthOf(a, mouth, fwd)) continue;
                    RE::Actor* best = nullptr; float bestD = 60.0f; RE::NiPoint3 bestTip;
                    for (RE::Actor* p : all) {
                        if (p == a || !p->IsInFaction(g_cockF)) continue;
                        RE::NiPoint3 tip;
                        if (!TipOf(p, tip)) continue;
                        const float d = tip.GetDistance(mouth);
                        if (d < bestD) { bestD = d; best = p; bestTip = tip; }
                    }
                    State& s = g_st[a->GetFormID()];
                    if (now - s.lastSeen > 60.0 && g.exp) ++g_exp[a->GetFormID()].times;   // a new time
                    s.lastSeen = now;
                    s.partner = best ? best->GetFormID() : 0;
                    if (!best) { s.jawTarget = 0.0f; s.depth = -100.0f; for (float& w : s.thrWant) w = 0.0f; s.thrReach = -100.0f; ThroatBody(a, s, dt, !g.throatBody); continue; }
                    const RE::NiPoint3 rel = bestTip - mouth;
                    s.depth = -rel.Dot(fwd);                                   // past his lips, into his mouth / throat
                    const float side = (rel + fwd * s.depth).Length();         // off the line of his mouth
                    const float k = Thick(best);
                    const bool in = s.depth > -0.8f && side < 4.0f;
                    const bool leave = g.toDeepFit && DeepFitHere();   // 2.6k: DeepFit moves the mouth (HPH heads); nobody else's mouth is moved
                    s.jawTarget = (g.jaw && in && !leave) ? std::max(g.jawMax * k, std::min(g.jawMin, g.jawMax)) : 0.0f;
                    if (g.noTongue && in && now - s.tongueAt > 1.5) { s.tongueAt = now; NoTongue(a); }   // 2.6k: the schlong pushed it about
                    const bool deep = in && s.depth > g.deepFrom;
                    // 2.6k: how forceful: the strokes' speed in and out (units a second), eased over a few seconds
                    if (in && s.lastDepth > -50.0f && dt > 0.0f) {
                        const float speed = std::abs(s.depth - s.lastDepth) / dt;
                        const float f = std::clamp((speed - 4.0f) / 14.0f, 0.0f, 1.0f);
                        s.force += (f - s.force) * std::min(1.0f, dt / 3.0f);
                    }
                    s.lastDepth = in ? s.depth : -100.0f;
                    const float forceK = g.gagByForce ? 1.0f + g.gagForce * ((0.35f + 1.25f * s.force) - 1.0f) : 1.0f;
                    if (deep && !s.deep && g.gag && now > s.gagUntil + 1.0 && Rand() < g.gagChance * ExpK(a->GetFormID()) * (0.6f + 0.6f * k) * forceK) {
                        s.gagUntil = now + g.gagTime;
                        if (g.gagSound) Sounds::Play("gag", a, g.gagVol * (0.8f + 0.4f * s.force));   // (nothing when the folder is empty)
                        ++g_exp[a->GetFormID()].gags;
                        if (g.looks && g.tears) { auto& L = g_look[a->GetFormID()]; L.v[Fluid::kTears] = std::min(1.0f, L.v[Fluid::kTears] + 0.35f); }
                        if (g.blush && OBlush()) s.blushUntil = now + g.blushSecs;
                    }
                    s.deep = deep;
                    if (g.looks && in) {   // sucking: wet lips, slowly redder
                        auto& L = g_look[a->GetFormID()];
                        if (g.lipWet) { L.v[Fluid::kLipWet] = 1.0f; L.wetUntil = now + g.lipWetSecs; }
                        if (g.lipRed) L.v[Fluid::kLipRed] = std::min(1.0f, L.v[Fluid::kLipRed] + dt / (60.0f * std::max(g.lipRedMins, 0.2f)));
                    }
                    if (g.exp && in) g_exp[a->GetFormID()].exp = std::min(12.0f, g_exp[a->GetFormID()].exp + dt / 60.0f * (deep ? 1.0f : 0.3f));
                    // throat bulge (experimental)
                    // 2.6j: it used to follow each stroke in and out 4 times a second, so the neck (and the head on it) jumped with
                    // every change - now it holds while he keeps going deep and eases up and down in steps too small to see
                    if (g.throat && deep && !DeepFitHere()) {   // 2.6k: SMP Head - DeepFit bulges the throat itself
                        s.deepAt = now;
                        s.throatWant = std::max(s.throatWant * 0.98f, g.throatMax * k * std::clamp((s.depth - g.deepFrom) / 4.0f + 0.4f, 0.0f, 1.0f));
                    }
                    const float th = (g.throat && now - s.deepAt < 2.0) ? s.throatWant : 0.0f;
                    const float rate = (th > s.throatCur ? 0.03f : 0.02f) * dt;   // a full +10% neck takes ~3 s to come, ~5 s to go
                    s.throatCur = th > s.throatCur ? std::min(th, s.throatCur + rate) : std::max(th, s.throatCur - rate);
                    if (th <= 0.0f && s.throatCur <= 0.0f) s.throatWant = 0.0f;
                    if (std::abs(s.throatCur - s.throatSent) > 0.003f || (s.throatCur <= 0.0f && s.throatSent > 0.0f)) { ThroatScale(a, s.throatCur); s.throatSent = s.throatCur; }
                    // 2.6y TEST: the body's throat bulge - each section of the front of his neck fills as the schlong passes it
                    s.thrReach = (g.throatBody && in) ? ThroatReach(a, best, g.throatReach) : -100.0f;
                    for (int i = 0; i < 4; ++i)
                        s.thrWant[i] = g.throatBody ? g.throatBodyMax * k * std::clamp((s.thrReach - 3.0f * i) / 3.0f, 0.0f, 1.0f) : 0.0f;
                    ThroatBody(a, s, dt, !g.throatBody);
                }
            }
            // gulps: one for about every quarter load swallowed, a second or so apart (at most 4 a finish)
            for (auto it = g_gulps.begin(); it != g_gulps.end();) {
                Gulps& q = it->second;
                if (q.left > 0 && now >= q.next) {
                    if (auto* a = RE::TESForm::LookupByID<RE::Actor>(it->first); a && a->Is3DLoaded() && g.gulp)
                        Sounds::Play("gulp", a, g.gulpVol);
                    --q.left;
                    q.next = now + 0.75 + 0.4 * Rand();
                }
                if (q.left <= 0 && now > q.next + 3.0) { it = g_gulps.erase(it); continue; }
                ++it;
            }
            Looks(dt, now);
            // men no longer at it: let the jaw close, take the throat back, then forget them
            for (auto it = g_st.begin(); it != g_st.end();) {
                State& s = it->second;
                auto* a = RE::TESForm::LookupByID<RE::Actor>(it->first);
                const bool gone = !a || !a->Is3DLoaded() || now - s.lastSeen > 0.6;
                if (gone) { s.jawTarget = 0.0f; s.deep = false; }
                if (gone && s.throatSent > 0.0f && a) { ThroatScale(a, 0.0f); s.throatSent = 0.0f; s.throatCur = 0.0f; s.throatWant = 0.0f; }
                if (gone) ThroatBody(a, s, dt, true);
                if (s.blushing && now > s.blushUntil && a) { Engine::BlushBoost(a, 0.0f); s.blushing = false; }
                if (!s.blushing && now < s.blushUntil && a) s.blushing = Engine::BlushBoost(a, g.blushBoost);
                if (gone && s.jaw < 0.01f && s.gag < 0.01f && !s.blushing && s.faceSent <= 0.0f) { it = g_st.erase(it); continue; }
                ++it;
            }
            g_any = !g_st.empty();
        }

        // every frame while someone is at it: the jaw and the gag face (OStim sets its own face; ours goes on top)
        void Frame(float dt) {
            std::lock_guard lk(g_mx);
            const double now = Now();
            for (auto& [id, s] : g_st) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!a || !a->Is3DLoaded()) continue;
                s.jaw += (s.jawTarget - s.jaw) * std::min(1.0f, dt * 6.0f);
                const float gagT = now < s.gagUntil ? 1.0f : 0.0f;
                s.gag += (gagT - s.gag) * std::min(1.0f, dt * (gagT > s.gag ? 14.0f : 3.0f));
                auto* fg = a->GetFaceGenAnimationData();
                if (!fg) continue;
                const float face = std::max(s.jaw, s.gag);
                if (face < 0.005f && s.faceSent <= 0.0f) continue;
                const bool leaveMouth = g.toDeepFit && DeepFitHere();   // 2.6k: the gag face keeps its eyes and brows, the mouth is DeepFit's
                if (!leaveMouth && (s.jaw > 0.005f || s.faceSent > 0.0f)) fg->phenomeKeyFrame.SetValue(kPhBigAah, std::clamp(s.jaw + 0.25f * s.gag, 0.0f, 1.0f));
                for (auto m : {kModSquintL, kModSquintR}) fg->modifierKeyFrame.SetValue(m, 0.85f * s.gag);
                for (auto m : {kModBrowInL, kModBrowInR}) fg->modifierKeyFrame.SetValue(m, 0.7f * s.gag);
                s.faceSent = face;
            }
        }

        void Loop() {
            double lastScan = 0.0, last = Now();
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(g_any ? 16 : 250));
                auto* ui = RE::UI::GetSingleton();
                if (!ui || ui->GameIsPaused()) { last = Now(); continue; }
                const double now = Now();
                const float dt = static_cast<float>(std::clamp(now - last, 0.0, 0.25));
                last = now;
                auto* tq = SKSE::GetTaskInterface();
                if (!tq) continue;
                if (now - lastScan >= 0.25 && !g_queued.exchange(true)) {
                    const float sdt = static_cast<float>(std::clamp(now - lastScan, 0.0, 1.0));
                    lastScan = now;
                    tq->AddTask([sdt]() { g_queued = false; Scan(sdt); });
                }
                if (g_any && !g_frameQueued.exchange(true)) tq->AddTask([dt]() { g_frameQueued = false; Frame(dt); });
            }
        }

        void Help(const char* t) {
            ImGuiMCP::SameLine();
            Tr::TextDisabled("(?)");
            if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::PushTextWrapPos(420.0f); Tr::TextUnformatted(t); ImGuiMCP::PopTextWrapPos(); ImGuiMCP::EndTooltip(); }
        }
    }

    void Install() { std::thread(Loop).detach(); }
    void Swallowed(RE::Actor* recv, float loads) {
        if (!recv || loads <= 0.0f) return;
        std::lock_guard lk(g_mx);
        if (g.on && g.looks && g.lipWet) { auto& L = g_look[recv->GetFormID()]; L.v[Fluid::kLipWet] = 1.0f; L.wetUntil = Now() + g.lipWetSecs * 1.5f; }
        g_swallowAt[recv->GetFormID()] = Now();
        if (!g.on || !g.gulp) return;
        Gulps& q = g_gulps[recv->GetFormID()];
        q.owed += loads;
        const int add = static_cast<int>(q.owed / 0.25f);
        if (add > 0) {
            q.owed -= 0.25f * static_cast<float>(add);
            if (q.left <= 0) q.next = Now() + 0.35 + 0.3 * Rand();
            q.left = std::min(4, q.left + add);
        } else if (q.left <= 0 && q.next < Now()) {   // a small swallow still gets one gulp
            q.left = 1;
            q.next = Now() + 0.35 + 0.3 * Rand();
            q.owed = 0.0f;
        }
    }
    void TestLayer(RE::Actor* a, int layer) {   // 2.6k: Testing page
        if (!a) return;
        std::lock_guard lk(g_mx);
        g_testId = a->GetFormID(); g_testUntil = Now() + 20.0; g_testLayer = layer;
        if (auto it = g_look.find(g_testId); it != g_look.end()) for (auto& sv : it->second.sent) sv = -1.0f;
    }
    void ClearFace(RE::Actor* a) {   // 2.6k: Testing page - everything off his face now
        if (!a) return;
        { std::lock_guard lk(g_mx); g_testUntil = 0.0; }
        Bathed(a->GetFormID());
        std::lock_guard lk(g_mx);
        if (auto it = g_look.find(a->GetFormID()); it != g_look.end()) { for (auto& v : it->second.v) v = 0.0f; it->second.wetUntil = 0.0; }
    }
    void OnFace(RE::Actor* recv, RE::Actor* giver, float loads) {
        if (!recv || loads <= 0.0f) return;
        float rgb[3] = {0.96f, 0.95f, 0.90f};
        if (giver) Colour::CumColour(giver, rgb);
        std::lock_guard lk(g_mx);
        if (!g.on || !g.looks || !g.faceCum) return;
        Look& L = g_look[recv->GetFormID()];
        const float now = std::max(L.v[Fluid::kFaceCum], 0.0f);
        L.cumStart = std::min(1.0f, now + 0.35f + 0.4f * std::min(loads, 1.5f));
        L.cumGameH = GameHours();
        for (int c = 0; c < 3; ++c) L.rgb[c] = rgb[c];
    }
    void Bathed(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_look.find(id);
        if (it == g_look.end()) return;
        Look& L = it->second;
        L.cumGameH = -1.0; L.v[Fluid::kFaceCum] = 0.0f; L.v[Fluid::kTears] = 0.0f; L.v[Fluid::kFaceSweat] = L.v[Fluid::kBodySweat] = 0.0f;
    }
    std::vector<OralState> OralNow() {
        std::lock_guard lk(g_mx);
        std::vector<OralState> out;
        const double t = Now();
        auto add = [&](RE::FormID id) -> OralState& {
            for (auto& n : out) if (n.id == id) return n;
            out.push_back({}); out.back().id = id;
            if (auto it = g_exp.find(id); it != g_exp.end()) out.back().exp = it->second.exp;
            return out.back();
        };
        for (auto& [id, s] : g_st) {
            if (t - s.lastSeen > 30.0) continue;
            OralState& n = add(id);
            n.partner = s.partner;
            n.sucking = t - s.lastSeen < 1.0 && s.partner != 0;
            n.deep = s.deep;
            n.gagging = t < s.gagUntil + 2.0;
        }
        for (auto& [id, at] : g_swallowAt) if (t - at < 20.0) add(id).swallowed = true;
        for (auto& [id, L] : g_look) if (L.v[Fluid::kFaceCum] > 0.05f) add(id).faceCum = L.v[Fluid::kFaceCum];
        std::erase_if(g_swallowAt, [&](const auto& p) { return t - p.second > 60.0; });
        return out;
    }
    std::string SeenText(RE::Actor* a) {
        if (!a) return {};
        std::lock_guard lk(g_mx);
        if (!g.on || !g.looks) return {};
        auto it = g_look.find(a->GetFormID());
        if (it == g_look.end()) return {};
        const Look& L = it->second;
        const std::string n = a->GetName() ? a->GetName() : "He";
        std::vector<std::string> p;
        if (L.v[Fluid::kFaceCum] > 0.3f) p.push_back(std::format("{}'s face is streaked with cum, dripping from his chin", n));
        else if (L.v[Fluid::kFaceCum] > 0.05f) p.push_back(std::format("{} has traces of cum on his face", n));
        if (L.v[Fluid::kTears] > 0.2f) p.push_back("his eyes are wet, tears running down his cheeks");
        if (L.v[Fluid::kLipRed] > 0.4f) p.push_back("his lips are red and swollen");
        else if (L.v[Fluid::kLipWet] > 0.4f) p.push_back("his lips are wet and glossy");
        if (L.v[Fluid::kFlush] > 0.4f) p.push_back("his cheeks are flushed");
        if (L.v[Fluid::kFaceSweat] > 0.4f) p.push_back("he is sweaty");
        if (p.empty()) return {};
        std::string o = p[0];
        if (o.rfind(n, 0) != 0) o = std::format("{}: {}", n, o);
        for (size_t i = 1; i < p.size(); ++i) o += (i + 1 == p.size() ? " and " : ", ") + p[i];
        return o + ".";
    }
    std::string SelfText(RE::Actor* a) {
        if (!a) return {};
        std::lock_guard lk(g_mx);
        if (!g.on) return {};
        std::vector<std::string> out;
        if (auto it = g_exp.find(a->GetFormID()); g.exp && it != g_exp.end() && it->second.times > 0) {
            const Exp& e = it->second;
            const char* lv = Level(e.exp);
            out.push_back(std::format("At sucking cock he is {} ({} time{}{}).", lv, e.times, e.times == 1 ? "" : "s",
                                      e.gags > 0 ? std::format(", he has gagged {} time{}", e.gags, e.gags == 1 ? "" : "s") : ""));
        }
        if (auto it = g_swallowAt.find(a->GetFormID()); it != g_swallowAt.end() && Now() - it->second < 120.0) out.push_back("He has just swallowed a load; he can still taste it.");
        if (auto it = g_st.find(a->GetFormID()); it != g_st.end() && Now() - it->second.lastSeen < 1.0 && it->second.partner) {
            auto* p = RE::TESForm::LookupByID<RE::Actor>(it->second.partner);
            out.push_back(std::format("Right now he has {}'s cock in his mouth{}.", p && p->GetName() ? p->GetName() : "a man's", it->second.deep ? ", deep in his throat" : ""));
        }
        std::string o;
        for (auto& x : out) o += (o.empty() ? "" : " ") + x;
        return o;
    }
    bool Wiped(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_look.find(id);
        if (it == g_look.end()) return false;
        Look& L = it->second;
        const bool had = L.v[Fluid::kFaceCum] > 0.05f || L.v[Fluid::kTears] > 0.05f;
        L.cumGameH = -1.0; L.v[Fluid::kFaceCum] = 0.0f; L.v[Fluid::kTears] = 0.0f;
        return had;
    }
    void Reset() {
        std::lock_guard lk(g_mx);
        g_look.clear();
        g_st.clear();
        g_any = false;
    }
    Oral OralOf(RE::FormID id) {
        std::lock_guard lk(g_mx);
        Oral o;
        if (auto it = g_exp.find(id); it != g_exp.end()) { o.exp = it->second.exp; o.times = it->second.times; o.gags = it->second.gags; }
        o.level = Level(o.exp);
        return o;
    }

    void Save(SKSE::SerializationInterface* a) {
        std::lock_guard lk(g_mx);
        if (!a->OpenRecord(kRec, 1)) return;
        const std::uint32_t n = static_cast<std::uint32_t>(g_exp.size());
        a->WriteRecordData(&n, sizeof n);
        for (auto& [id, e] : g_exp) {
            a->WriteRecordData(&id, sizeof id); a->WriteRecordData(&e.exp, sizeof e.exp);
            a->WriteRecordData(&e.times, sizeof e.times); a->WriteRecordData(&e.gags, sizeof e.gags);
        }
    }
    bool Load(SKSE::SerializationInterface* a, std::uint32_t type, std::uint32_t ver, std::uint32_t) {
        if (type != kRec) return false;
        if (ver != 1) return true;
        std::lock_guard lk(g_mx);
        g_exp.clear();
        std::uint32_t n = 0;
        a->ReadRecordData(&n, sizeof n);
        for (std::uint32_t j = 0; j < n; ++j) {
            RE::FormID id = 0, now = 0; Exp e;
            a->ReadRecordData(&id, sizeof id); a->ReadRecordData(&e.exp, sizeof e.exp);
            a->ReadRecordData(&e.times, sizeof e.times); a->ReadRecordData(&e.gags, sizeof e.gags);
            if (a->ResolveFormID(id, now)) g_exp[now] = e;
        }
        return true;
    }
    void Revert() {
        std::lock_guard lk(g_mx);
        g_exp.clear();
    }

    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "Face";
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto F = [&](const char* k, float& v, float lo, float hi) { if (save) ini.SetDoubleValue(S, k, v); else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi); };
        std::lock_guard lk(g_mx);
        B("On", g.on); B("Jaw", g.jaw); B("LeaveMouthToDeepFit", g.toDeepFit); F("JawMax", g.jawMax, 0.1f, 1.0f); F("JawMin", g.jawMin, 0.0f, 1.0f);
        B("ThroatBulge", g.throat); F("ThroatMax", g.throatMax, 0.01f, 0.3f); F("DeepFrom", g.deepFrom, 1.0f, 12.0f);
        B("ThroatBodyTest", g.throatBody); F("ThroatBodyStrength", g.throatBodyMax, 0.1f, 3.0f); F("ThroatBodyReach", g.throatReach, 0.0f, 12.0f);
        B("OralExperience", g.exp); B("Gag", g.gag); F("GagChance", g.gagChance, 0.0f, 1.0f);
        B("GagByForce", g.gagByForce); F("GagForce", g.gagForce, 0.0f, 2.0f); B("GagSound", g.gagSound); F("GagVolume", g.gagVol, 0.0f, 2.0f); F("GagTime", g.gagTime, 0.3f, 4.0f);
        B("NoTongueWhileSucking", g.noTongue);
        B("OBlush", g.blush); F("OBlushBoost", g.blushBoost, 0.0f, 1.0f); F("OBlushSeconds", g.blushSecs, 1.0f, 30.0f);
        B("Gulp", g.gulp); F("GulpVolume", g.gulpVol, 0.0f, 2.0f);
        B("Looks", g.looks); B("LipWet", g.lipWet); F("LipWetSeconds", g.lipWetSecs, 5.0f, 600.0f); B("LipRed", g.lipRed); F("LipRedMinutes", g.lipRedMins, 0.2f, 30.0f);
        B("Flush", g.flush); B("FlushWithOBlush", g.flushOBlush); F("FlushMax", g.flushMax, 0.1f, 1.0f);
        B("Tears", g.tears); F("TearsSeconds", g.tearsSecs, 5.0f, 900.0f); B("FaceCum", g.faceCum); F("FaceCumHours", g.faceCumHours, 0.1f, 24.0f);
        B("Sweat", g.sweat); F("SweatMax", g.sweatMax, 0.1f, 1.0f); F("SweatSeconds", g.sweatSecs, 5.0f, 1800.0f);
        F("FitScale", g.fitScale, 0.5f, 2.0f); F("FitUp", g.fitUp, -1.0f, 1.0f); F("FitOut", g.fitOut, -1.0f, 1.0f);
        if (!save) Fluid::SetFaceFit({g.fitScale, g.fitUp, g.fitOut});
        constexpr const char* RF = "FaceFitRaces";   // race = size, up, forward
        if (save) {
            ini.Delete(RF, nullptr);
            for (auto& [k, f] : g_raceFit) ini.SetValue(RF, k.c_str(), std::format("{:.3f}, {:.3f}, {:.3f}", f.scale, f.up, f.out).c_str());
        } else {
            g_raceFit.clear();
            CSimpleIniA::TNamesDepend keys;
            ini.GetAllKeys(RF, keys);
            for (auto& k : keys) {
                Fluid::FaceFit f;
                if (std::sscanf(ini.GetValue(RF, k.pItem, ""), "%f , %f , %f", &f.scale, &f.up, &f.out) == 3)
                    g_raceFit[k.pItem] = {std::clamp(f.scale, 0.5f, 2.0f), std::clamp(f.up, -1.0f, 1.0f), std::clamp(f.out, -1.0f, 1.0f)};
            }
        }
    }

    void __stdcall RenderPage() {
        bool ch = false;
        {
            std::lock_guard lk(g_mx);
            Tr::TextWrapped("What oral does to his face and throat, while he is the one with the mouth in an OStim scene (blowjob, deepthroat).");
            ch |= Tr::Checkbox("Mouth & Face", &g.on);
            if (g.on) {
                if (Tr::CollapsingHeader("Jaw###fjaw")) {
                    if (DeepFitHere()) {
                        ch |= Tr::Checkbox("Leave the mouth to SMP Head - DeepFit", &g.toDeepFit);
                        Help("DeepFit is installed: it opens the mouth as wide as the schlong is thick (or as wide as it goes, its Mega mouth), fits the lips round it and bulges the throat (DeepFit.ini). On: Anatomica leaves the mouth to it. Off: Anatomica opens the jaw too (Widest / At least below). Gags still squeeze his eyes and brows.");
                    }
                    ch |= Tr::Checkbox("His mouth opens as wide as the schlong is thick", &g.jaw);
                    Help("Uses his face's own open-mouth shape, on top of OStim's expression. Thickness from The New Gentleman's size (XS to XL); average when not known.");
                    if (g.jaw) {
                        float j = g.jawMax * 100.0f;
                        if (Tr::SliderFloat("Widest", &j, 10.0f, 100.0f, "%.0f%% (for the thickest)")) { g.jawMax = j / 100.0f; ch = true; }
                        float jm = g.jawMin * 100.0f;
                        if (Tr::SliderFloat("At least", &jm, 0.0f, 100.0f, "%.0f%% (any schlong)")) { g.jawMin = jm / 100.0f; ch = true; }
                        Help("However slim the schlong, his mouth opens at least this wide while it is in.");
                    }
                }
                if (Tr::CollapsingHeader("Deep and gagging###fdeep")) {
                    ch |= Tr::SliderFloat("Deep from", &g.deepFrom, 1.0f, 12.0f, "%.1f units past his lips");
                    Help("How far in the tip has to be to count as deep, in his throat.");
                    ch |= Tr::Checkbox("He can gag", &g.gag);
                    Help("When it goes deep: eyes squeezed shut and brows pulled in for a moment. More likely for a beginner and a bigger schlong.");
                    if (g.gag) {
                        float c = g.gagChance * 100.0f;
                        if (Tr::SliderFloat("Chance", &c, 0.0f, 100.0f, "%.0f%% each time it goes deep (a beginner)")) { g.gagChance = c / 100.0f; ch = true; }
                        ch |= Tr::SliderFloat("Lasts", &g.gagTime, 0.3f, 4.0f, "%.1f s");
                        ch |= Tr::Checkbox("Rougher gags more, gentler less", &g.gagByForce);
                        Help("How forceful it is, from how fast and deep the strokes go: a slow, gentle blowjob gags him about a third as often, a rough one half again more.");
                        if (g.gagByForce) {
                            float f = g.gagForce * 100.0f;
                            if (Tr::SliderFloat("How much it matters", &f, 0.0f, 200.0f, "%.0f%%")) { g.gagForce = f / 100.0f; ch = true; }
                        }
                        ch |= Tr::Checkbox("Gag sounds", &g.gagSound);
                        Help("WAV files in Data\\Sound\\Fx\\Anatomica\\gag (any names; add or replace them) - one plays when he gags, louder when it is rough. None are included yet.");
                        if (g.gagSound) {
                            ch |= Tr::SliderFloat("Gag volume", &g.gagVol, 0.0f, 2.0f, "x%.1f");
                            Tr::TextDisabled("%s", Lang::Fmt("{} gag sound files found", Sounds::Count("gag")).c_str());
                        }
                    }
                    ch |= Tr::Checkbox("Throat bulge (experimental)", &g.throat);
                    Help("His neck swells as it goes deep. A test: the neck bone is scaled up (and the head back down), so the whole neck "
                         "thickens a little and the head can twitch - not a shaped bulge. With SMP Head - DeepFit installed this does nothing: DeepFit bulges the throat itself.");
                    if (g.throat) {
                        float t = g.throatMax * 100.0f;
                        if (Tr::SliderFloat("At most", &t, 1.0f, 30.0f, "+%.0f%% neck")) { g.throatMax = t / 100.0f; ch = true; }
                    }
                    ch |= Tr::Checkbox("Throat bulge on the body (test)", &g.throatBody);
                    Help("A test. The front of his neck, from the neck seam down to the top of his chest, swells along the schlong as it "
                         "goes down past the seam (DeepFit bulges the throat inside his head). Needs the body built from the "
                         "\"(Throat Test)\" set in BodySlide (Build Morphs ticked); the normal set leaves it out again.");
                    if (g.throatBody) {
                        ch |= Tr::SliderFloat("How much it swells", &g.throatBodyMax, 0.1f, 3.0f, "x%.1f");
                        ch |= Tr::SliderFloat("Count the schlong longer by", &g.throatReach, 0.0f, 12.0f, "%.1f units");
                        Help("If it never reaches his neck (the bulge stays flat), this makes it count as longer.");
                    }
                }
                if (Tr::CollapsingHeader("Oral experience###fexp")) {
                    ch |= Tr::Checkbox("Oral experience", &g.exp);
                    Help("Kept with the save. Grows with time spent sucking (more when deep). A beginner gags far more than a practised mouth.");
                }
                if (Tr::CollapsingHeader("Swallowing###fgulp")) {
                    ch |= Tr::Checkbox("A gulp when he swallows", &g.gulp);
                    Help("OStrands 0.8.0.8 or newer tells Anatomica how much of a finish in the mouth was swallowed (its Mouth and spit settings): a gulp for about every quarter load, up to 4.");
                    if (g.gulp) {
                        float v = g.gulpVol * 100.0f;
                        if (Tr::SliderFloat("Gulp volume", &v, 0.0f, 200.0f, "%.0f%%")) { g.gulpVol = v / 100.0f; ch = true; }
                        const int n = Sounds::Count("gulp");
                        if (n == 0) Tr::TextDisabled("No gulp sounds found in Data\\Sound\\Fx\\Anatomica\\gulp.");
                        else Tr::TextDisabled(Lang::Fmt("{} gulp sounds (Data\\Sound\\Fx\\Anatomica\\gulp - add or replace WAV files there).", n).c_str());
                    }
                }
                ch |= Tr::Checkbox("No tongue out while his mouth is full", &g.noTongue);
                Help("Mods such as Ahegao Expressions put OStim's tongue in his mouth as he gets excited; during a blowjob the schlong pushes it about. While he sucks, Anatomica takes it out again (every second and a half).");
                if (Tr::CollapsingHeader("OBlush###fblush")) {
                    if (!OBlush()) Tr::TextDisabled("OBlush is not installed.");
                    ch |= Tr::Checkbox("Blush harder when he gags", &g.blush);
                    Help("While OBlush is blushing him (turn on its male blushing in its MCM), a gag makes the blush stronger for a few seconds, then back to OBlush's own.");
                    if (g.blush) {
                        float b = g.blushBoost * 100.0f;
                        if (Tr::SliderFloat("Stronger by", &b, 0.0f, 100.0f, "%.0f%%")) { g.blushBoost = b / 100.0f; ch = true; }
                        ch |= Tr::SliderFloat("For", &g.blushSecs, 1.0f, 30.0f, "%.0f s");
                    }
                }
            }
            if (g.on && Tr::CollapsingHeader("How his face looks###flook")) {
                ch |= Tr::Checkbox("Face looks", &g.looks);
                Help("Layers drawn from his own head mesh (his head bones find his mouth, eyes and cheeks), so they fit any head. "
                     "Made once per head and kept in the textures folder.");
                if (g.looks) {
                    // 2.6v: wet lips, red lips and tears are gone (painted they never looked right)
                    ch |= Tr::Checkbox("Flushed cheeks", &g.flush);
                    Help("A flush on his cheeks and nose as he gets excited in a scene (OStim's excitement), easing after. Off while OBlush is installed unless ticked below.");
                    if (g.flush) {
                        ch |= Tr::Checkbox("Even with OBlush", &g.flushOBlush);
                        float f = g.flushMax * 100.0f;
                        if (Tr::SliderFloat("Flush strength", &f, 10.0f, 100.0f, "%.0f%%")) { g.flushMax = f / 100.0f; ch = true; }
                    }
                    ch |= Tr::Checkbox("Cum on his face", &g.faceCum);
                    Help("A facial (the pull-out chance on OStrands' page) leaves cum on his face: strands of it hang from where it "
                         "landed - cheeks, nose, lips, chin and brow - for minutes, then drip off. Needs OStrands 0.8.0.9e or newer.");
                    if (g.faceCum) { ImGuiMCP::SameLine(); ImGuiMCP::SetNextItemWidth(160.0f); ch |= Tr::SliderFloat("##fc", &g.faceCumHours, 0.1f, 24.0f, "%.1f game hours"); }
                    ch |= Tr::Checkbox("Sweat", &g.sweat);
                    Help("His skin itself grows glossier, face and body, the more excited he gets in a scene (any scene), up to the "
                         "strength set, and dries again over the time set after.");
                    if (g.sweat) {
                        float w = g.sweatMax * 100.0f;
                        if (Tr::SliderFloat("Sweat strength", &w, 10.0f, 100.0f, "%.0f%%")) { g.sweatMax = w / 100.0f; ch = true; }
                        ch |= Tr::SliderFloat("Dries in", &g.sweatSecs, 5.0f, 1800.0f, "%.0f s");
                    }
                    ImGuiMCP::Separator();
                    Tr::TextDisabled("Fitting (if the layers sit wrong on a head):");
                    // 2.6j: the race of the one in the console / under the crosshair (or yours) can have its own
                    RE::Actor* ft = nullptr;
                    if (auto sel = RE::Console::GetSelectedRef()) ft = sel->As<RE::Actor>();
                    if (!ft) if (auto* cr = RE::CrosshairPickData::GetSingleton()) if (auto r = cr->GetActiveTarget().get()) ft = r->As<RE::Actor>();
                    if (!ft) ft = RE::PlayerCharacter::GetSingleton();
                    const std::string rk = RaceKey(ft);
                    const char* rname = ft && ft->GetRace() && ft->GetRace()->GetFullName() ? ft->GetRace()->GetFullName() : "?";
                    bool own = !rk.empty() && g_raceFit.contains(rk);
                    if (!rk.empty() && Tr::Checkbox(Lang::Fmt("Own fitting for {}", std::string(rname)).c_str(), &own)) {
                        if (own) g_raceFit[rk] = {g.fitScale, g.fitUp, g.fitOut}; else g_raceFit.erase(rk);
                        if (auto lit = g_look.find(ft->GetFormID()); lit != g_look.end()) for (auto& sv : lit->second.sent) sv = -1.0f;   // resend now
                        ch = true;
                    }
                    Help("The one clicked in the console or under your crosshair (or you): his race gets its own size and position, the rest keep the default below.");
                    bool fit = false;
                    if (own) {
                        auto& rf = g_raceFit[rk];
                        fit |= Tr::SliderFloat("Size", &rf.scale, 0.5f, 2.0f, "%.2fx");
                        fit |= Tr::SliderFloat("Up / down", &rf.up, -1.0f, 1.0f, "%.2f");
                        fit |= Tr::SliderFloat("Forward / back", &rf.out, -1.0f, 1.0f, "%.2f");
                        if (fit) { if (auto lit = g_look.find(ft->GetFormID()); lit != g_look.end()) for (auto& sv : lit->second.sent) sv = -1.0f; ch = true; }
                    } else {
                        fit |= Tr::SliderFloat("Size", &g.fitScale, 0.5f, 2.0f, "%.2fx");
                        fit |= Tr::SliderFloat("Up / down", &g.fitUp, -1.0f, 1.0f, "%.2f");
                        fit |= Tr::SliderFloat("Forward / back", &g.fitOut, -1.0f, 1.0f, "%.2f");
                        if (fit) { Fluid::SetFaceFit({g.fitScale, g.fitUp, g.fitOut}); ch = true; }
                    }
                    if (Tr::Button("Show every layer on him for 20 s")) {
                        RE::Actor* t = nullptr;
                        if (auto sel = RE::Console::GetSelectedRef()) t = sel->As<RE::Actor>();
                        if (!t) if (auto* cr = RE::CrosshairPickData::GetSingleton()) if (auto r = cr->GetActiveTarget().get()) t = r->As<RE::Actor>();
                        if (!t) t = RE::PlayerCharacter::GetSingleton();
                        if (t) { g_testId = t->GetFormID(); g_testUntil = Now() + 20.0; g_testLayer = -1; }
                    }
                    Help("The one clicked in the console or under your crosshair, or you. To check the fitting: lips, cheeks, tears, cum and sweat all at once.");
                    if (g_testId) { const auto info = Fluid::FaceInfo(g_testId); if (!info.empty()) Tr::TextDisabled(info.c_str()); }
                }
            }
            if (Tr::CollapsingHeader("Right now###fnow")) {
                if (g_st.empty()) Tr::TextDisabled("Nobody is at it.");
                for (auto& [id, s] : g_st) {
                    auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                    auto* p = s.partner ? RE::TESForm::LookupByID<RE::Actor>(s.partner) : nullptr;
                    const auto e = g_exp.contains(id) ? g_exp[id] : Exp{};
                    Tr::TextUnformatted(Lang::Fmt("{} - partner {}, {:.1f} units in{}, jaw {:.0f}%, oral experience: {} ({:.1f}), gagged {} times",
                        std::string(a ? a->GetName() : "?"), std::string(p ? p->GetName() : "none"), s.depth, s.deep ? " (deep)" : "",
                        s.jaw * 100.0f, std::string(Lang::T(Level(e.exp))), e.exp, e.gags).c_str());
                    if (g.throatBody)
                        Tr::TextDisabled("%s", Lang::Fmt("Throat on the body (test): the schlong reaches {:.1f} units below the neck seam, sections {:.0f} / {:.0f} / {:.0f} / {:.0f}%",
                            std::max(s.thrReach, 0.0f), s.thrCur[0] * 100.0f, s.thrCur[1] * 100.0f, s.thrCur[2] * 100.0f, s.thrCur[3] * 100.0f).c_str());
                }
            }
        }
        if (ch) Colour::SaveNow();
    }
}

// ---- for OStrands 0.8.0.8+ (it looks this up in AnatomicaMenu.dll): this much of a finish in his mouth was swallowed
extern "C" {
    __declspec(dllexport) void Anatomica_Swallow(RE::Actor* recv, RE::Actor* giver, float loads) {
        (void)giver;
        Face::Swallowed(recv, loads);
    }
    // 2.6j: OStrands 0.8.0.8+: a blowjob finish pulled out onto his face
    __declspec(dllexport) void Anatomica_Facial(RE::Actor* recv, RE::Actor* giver, float loads) {
        Face::OnFace(recv, giver, loads);
    }
}

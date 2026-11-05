#include "Drip.h"
#include "Fluid.h"
#include "Colour.h"
#include "Wink.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
//  Cum drip (all Anatomica's own assets, AnatomicaGape.esp):
//   * strands  - 3 sizes x 4 lengths of SMP strand hanging from the anus (slot 56). A drip grows
//                through the 4 lengths, falls off (puddle on the floor) and the next one starts,
//                until he is empty. More in him = thicker strands, more of them, slower to empty.
//   * squirt   - 3 quick frames of a jet out of the anus (slot 59), then a puddle.
//   * puddles  - placed on the floor under him, removed after a while (and after loading a save).
//   * leg trail- a raised, glossy rivulet mesh on the skin (slot 45) that grows down one thigh.
//  The colour of everything follows the cum colour of the giver's race (Cum & drips page) or the
//  colour picked here; tinted textures are generated into textures\...\Anatomica\gen\.
// ---------------------------------------------------------------------------
namespace {
    using clock_t_ = std::chrono::steady_clock;
    double Now() { return std::chrono::duration<double>(clock_t_::now().time_since_epoch()).count(); }

    constexpr const char* kEsp = "AnatomicaGape.esp";
    constexpr RE::FormID kStrandArmo = 0x830;   // + (tier-1)*4: one armor per size; the 4 lengths are shown/hidden inside it
    constexpr RE::FormID kSquirtArmo = 0x84C;   // frames 1-3; frames 4-6 at 0x8A0 (see SquirtArmo)
    constexpr int kSquirtFrames = 6;
    constexpr RE::FormID SquirtArmo(int frame) { return frame <= 3 ? kSquirtArmo + frame - 1 : 0x8A0 + frame - 4; }
    // bulge, first spurt, full spurt, torn off, second pulse, dribble
    constexpr double kSquirtTime[kSquirtFrames] = {0.12, 0.12, 0.16, 0.14, 0.2, 0.9};
    constexpr RE::FormID kOverArmo = 0x870;     // + frame-1: comes back out around his schlong (still inside)
    constexpr RE::FormID kSprayArmo = 0x8B0;    // + frame-1: squirts out AROUND his schlong (still inside), 4 frames
    constexpr RE::FormID kTipArmo = 0x878;      // + stage-1: left on the giver's tip after he pulls out (slot 44)
    constexpr auto kTipSlot = RE::BGSBipedObjectForm::BipedObjectSlot::kModMouth;
    constexpr RE::FormID kTrailArmo = 0x888;    // left 1-3, right 1-3: the trail down his leg (slot 45, mesh)
    constexpr RE::FormID kStringArmo = 0x880;   // + frame-1: the string to the partner when he pulls out (slot 59)
    constexpr auto kTrailSlot = RE::BGSBipedObjectForm::BipedObjectSlot::kModNeck;
    constexpr RE::FormID kPuddleStat = 0x858;   // + size (0 small, 1 medium, 2 large)
    constexpr const char* kGenDir = "Data/textures/actors/character/male/Anatomica/gen/";
    constexpr const char* kGenTex = "textures\\actors\\character\\male\\Anatomica\\gen\\";
    constexpr const char* kDetail = "Data/SKSE/Plugins/AnatomicaMenu/drip_detail.bin";

    struct Settings {
        bool on = true;
        bool trigOStim = true, trigSexLab = true, trigRound = true;
        bool showDuring = true;          // strands while still in the scene (off = when it ends)
        int loadCap = 5;                 // loads he can hold (1-5)
        float vol = 1.0f;                // one load
        float manMin = 0.7f, manMax = 1.4f;   // each man's own amount (fixed per man)
        bool firstBig = true; float firstMul = 2.0f; float firstGap = 60.0f;   // minutes since his last
        bool multi = true; float multiPer = 0.25f;                              // per extra partner
        float leakTime = 240.0f;         // seconds for one full load to drain
        bool squirt = true; float squirtChance = 0.4f; float squirtMin = 1.0f; bool squirtOverflow = true;
        bool overflow = true; float overflowChance = 0.25f; float overflowOut = 0.3f; float overflowHold = 8.0f;
        bool tip = true; float tipTime = 90.0f;                                     // leftover on the giver
        float tipFwd = 1.4f, tipDown = 0.4f;   // where it hangs: along his schlong past the last bone / toward the underside
        bool gush = true; bool gushScene = true; bool gushStand = true; float gushMin = 0.6f; float gushFrac = 0.45f;
        bool bath = true;                                                           // bathing mods wash it off
        bool film = true; float filmStrength = 0.35f;                               // cum film: a thin mesh over the inner rim
        bool clench = true; float clenchPush = 0.3f, clenchDribble = 0.35f; bool clenchOnly = false;
        bool froth = true; float frothAfter = 40.0f, frothTime = 180.0f;            // seconds of penetration / how long it stays
        bool string = true; float stringChance = 0.6f;                              // the string to him when he pulls out
        bool fullOpen = true; float fullOpenAmt = 1.0f;                             // gape closes slower while full
        bool land = true; float landChance = 0.8f;                                  // a spurt when he lands
        bool puddles = true; float puddleLife = 300.0f; int puddleMax = 24;
        bool walkDrops = true; float walkEvery = 6.0f;
        bool leg = true; float legStay = 600.0f; float legAlpha = 0.85f;
        bool raceColour = true; float colour[3] = {0.96f, 0.95f, 0.90f}; float opacity = 0.85f;
        bool wash = true;
    } g;

    struct State {
        RE::FormID self = 0;
        float vol = 0.0f, volStart = 0.0f;
        int loads = 0;
        std::set<RE::FormID> givers;
        double lastClimax = 0.0;
        // scene / round
        int waitKey = 0;               // showing waits for this scene to end (0 = show now)
        double waitSince = 0.0;
        double lastWorked = 0.0, roundStart = 0.0;
        bool roundOver = true, climaxInRound = false;
        std::map<RE::FormID, std::array<float, 3>> roundPartners;   // partner -> his cum colour
        // strands
        int tier = 0, stage = 0;
        double cycleStart = 0.0; float cycleLen = 20.0f, dropAmt = 0.4f;
        RE::FormID worn = 0;           // strand armor on him (local id) or 0
        double lastWornCheck = 0.0;
        // squirt
        int squirtFrame = 0; double squirtNext = 0.0; float squirtAmt = 0.0f;
        int ofFrame = 0; double ofNext = 0.0; float ofAmt = 0.0f, ofHold = 0.0f;   // overflow around the shaft / gush
        std::map<RE::FormID, std::array<float, 3>> roundGivers;   // came in him this round (-> leftover on their tip)
        int sceneKey = 0;                                           // the framework scene he is in (0 = none)
        bool wasDown = false; double gushAt = 0.0;                  // sitting / lying -> gush when he stands
        int ofMax = 3;                                              // last overflow frame (2 = a small dribble)
        RE::FormID ofBase = kOverArmo; double ofStep = 0.45;        // ooze frames, or the spray around the shaft
        float roundSecs = 0.0f, roundRough = 0.0f;                  // this round: penetration time, sum of dilation
        double frothStart = 0.0; float frothLevel = 0.0f; int trailWorn = 0;   // froth: painted into the rim texture
        int strFrame = 0; double strNext = 0.0;
        bool pulledOut = true; double lastString = 0.0;              // the string: right as he slides out
        double lastStep = 0.0; bool inAir = false; double airSince = 0.0;
        // colour (volume-weighted mix of the givers' colours)
        float rgb[3] = {0.96f, 0.95f, 0.90f};
        std::string texSent;
        // floor / leg
        float leaked = 0.0f; double emptySince = 0.0; int legSide = 0; int legSent = -2; int legAlphaSent = -1;
        RE::NiPoint3 lastPos{}; double lastPosT = 0.0, lastWalkDrop = 0.0; bool moving = false;
    };

    struct Puddle { RE::FormID ref; double expires; };

    std::mutex g_mx;
    std::unordered_map<RE::FormID, State> g_st;
    std::map<std::pair<RE::FormID, RE::FormID>, double> g_recent;    // (receiver, giver) -> last climax
    std::unordered_map<RE::FormID, double> g_giverLast;               // giver -> last climax (first-load bonus)
    std::vector<Puddle> g_puddles;
    std::vector<RE::FormID> g_staleRefs;                              // puddles from the save, to delete
    std::atomic<bool> g_needEvents{true};
    std::mt19937 g_rng{std::random_device{}()};
    float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }

    // ---------------------------------------------------------------- forms
    RE::TESObjectARMO* Armo(RE::FormID local) {
        auto* dh = RE::TESDataHandler::GetSingleton();
        return dh ? dh->LookupForm<RE::TESObjectARMO>(local, kEsp) : nullptr;
    }
    bool PluginPresent() {
        auto* dh = RE::TESDataHandler::GetSingleton();
        return dh && Armo(kStrandArmo) != nullptr;
    }
    bool IsOurs(const RE::TESObjectARMO* a) {
        if (!a) return false;
        const auto* f = a->GetFile(0);
        if (!f || std::string_view(f->GetFilename()) != kEsp) return false;
        const auto id = a->GetLocalFormID();
        return (id >= kStrandArmo && id < kStrandArmo + 12) || (id >= kSquirtArmo && id < kSquirtArmo + 3) || (id >= 0x8A0 && id < 0x8A3) ||
               (id >= kOverArmo && id < kOverArmo + 3) || (id >= kTipArmo && id < kTipArmo + 2) || (id >= kSprayArmo && id < kSprayArmo + 4) ||
               (id >= kTrailArmo && id < kTrailArmo + 6) || (id >= kStringArmo && id < kStringArmo + 3);
    }
    // the strands are made for every race: add his race to the armor addons if it is missing
    void AllowRace(RE::TESObjectARMO* armo, RE::TESRace* race) {
        if (!armo || !race) return;
        for (auto* aa : armo->armorAddons) {
            if (!aa || aa->race == race) continue;
            bool has = false;
            for (auto* r : aa->additionalRaces) if (r == race) { has = true; break; }
            if (!has) aa->additionalRaces.push_back(race);
        }
    }

    // ---------------------------------------------------------------- NiOverride (texture of the cum)
    void NiOvrTex(RE::TESObjectREFR* ref, const char* node, const std::string& tex) {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm || !ref) return;
        auto* args = RE::MakeFunctionArguments(std::move(ref), false, RE::BSFixedString(node), static_cast<std::int32_t>(9),
                                               static_cast<std::int32_t>(0), RE::BSFixedString(tex.c_str()), false);
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
        vm->DispatchStaticCall("NiOverride", "AddNodeOverrideString", args, cb);
    }

    // ---------------------------------------------------------------- tinted textures
    struct Detail { std::vector<std::uint8_t> cum, pud; bool ok = false; } g_detail;
    void LoadDetail() {
        if (g_detail.ok) return;
        g_detail.cum.assign(64 * 64 * 2, 0); g_detail.pud.assign(128 * 128 * 2, 0);
        std::ifstream f(kDetail, std::ios::binary);
        char magic[4] = {};
        if (f && f.read(magic, 4) && std::string_view(magic, 4) == "ADD1") {
            f.read(reinterpret_cast<char*>(g_detail.cum.data()), static_cast<std::streamsize>(g_detail.cum.size()));
            f.read(reinterpret_cast<char*>(g_detail.pud.data()), static_cast<std::streamsize>(g_detail.pud.size()));
            if (f) { g_detail.ok = true; return; }
        }
        // fallback: plain (no pattern), round puddle
        for (int i = 0; i < 64 * 64; ++i) g_detail.cum[i * 2] = 128, g_detail.cum[i * 2 + 1] = 215;
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) {
                const float u = (x + 0.5f) / 64.0f - 1.0f, v = (y + 0.5f) / 64.0f - 1.0f, r = std::sqrt(u * u + v * v);
                g_detail.pud[(y * 128 + x) * 2] = 128;
                g_detail.pud[(y * 128 + x) * 2 + 1] = static_cast<std::uint8_t>(std::clamp((0.8f - r) / 0.06f, 0.0f, 1.0f) * 190.0f);
            }
        g_detail.ok = true;
    }
    // uncompressed 32-bit DDS with mips (64x64 / 128x128 - a few KB)
    bool WriteDds(const std::filesystem::path& p, int n, const std::vector<std::uint8_t>& det, const float rgb[3], float opacity) {
        std::vector<std::uint8_t> lvl(static_cast<size_t>(n) * n * 4);
        for (int i = 0; i < n * n; ++i) {
            const float l = det[i * 2] / 128.0f;
            const float a = det[i * 2 + 1] / 255.0f * std::clamp(opacity / 0.85f, 0.2f, 1.2f);
            lvl[i * 4 + 0] = static_cast<std::uint8_t>(std::clamp(rgb[2] * l * 255.0f, 0.0f, 255.0f));   // B
            lvl[i * 4 + 1] = static_cast<std::uint8_t>(std::clamp(rgb[1] * l * 255.0f, 0.0f, 255.0f));   // G
            lvl[i * 4 + 2] = static_cast<std::uint8_t>(std::clamp(rgb[0] * l * 255.0f, 0.0f, 255.0f));   // R
            lvl[i * 4 + 3] = static_cast<std::uint8_t>(std::clamp(a * 255.0f, 0.0f, 255.0f));
        }
        int mips = 1; for (int s = n; s > 1; s /= 2) ++mips;
        std::uint32_t h[32] = {};
        h[0] = 124; h[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x8; h[2] = n; h[3] = n; h[4] = n * 4; h[6] = mips;
        h[18] = 32; h[19] = 0x41; h[21] = 32; h[22] = 0x00FF0000; h[23] = 0x0000FF00; h[24] = 0x000000FF; h[25] = 0xFF000000;
        h[26] = 0x1000 | 0x8 | 0x400000;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        if (!f) return false;
        f.write("DDS ", 4);
        f.write(reinterpret_cast<const char*>(h), 124);
        std::vector<std::uint8_t> cur = lvl; int s = n;
        for (int m = 0; m < mips; ++m) {
            f.write(reinterpret_cast<const char*>(cur.data()), static_cast<std::streamsize>(cur.size()));
            if (s == 1) break;
            const int ns = s / 2; std::vector<std::uint8_t> nx(static_cast<size_t>(ns) * ns * 4);
            for (int y = 0; y < ns; ++y)
                for (int x = 0; x < ns; ++x)
                    for (int c = 0; c < 4; ++c)
                        nx[(y * ns + x) * 4 + c] = static_cast<std::uint8_t>(
                            (cur[((2 * y) * s + 2 * x) * 4 + c] + cur[((2 * y) * s + 2 * x + 1) * 4 + c] +
                             cur[((2 * y + 1) * s + 2 * x) * 4 + c] + cur[((2 * y + 1) * s + 2 * x + 1) * 4 + c] + 2) / 4);
            cur.swap(nx); s = ns;
        }
        return static_cast<bool>(f);
    }
    std::string Quant(const float rgb[3], float opacity) {
        auto q = [](float v) { return std::clamp(static_cast<int>(std::lround(v * 31.0f)) * 8, 0, 255); };
        return std::format("{:02X}{:02X}{:02X}{:02X}", q(rgb[0]), q(rgb[1]), q(rgb[2]), std::clamp(static_cast<int>(opacity * 20.0f), 0, 20));
    }
    bool IsDefaultLook(const float rgb[3], float opacity) {
        const float d[3] = {0.96f, 0.95f, 0.90f};
        return Quant(rgb, opacity) == Quant(d, 0.85f);
    }
    // "" = the built-in texture is right
    std::string CumTex(const float rgb[3], float opacity, bool puddle) {
        if (IsDefaultLook(rgb, opacity)) return {};
        LoadDetail();
        const std::string name = std::string(puddle ? "pud_" : "cum_") + Quant(rgb, opacity) + ".dds";
        const std::filesystem::path p = std::filesystem::path(kGenDir) / name;
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            float c[3]; for (int i = 0; i < 3; ++i) c[i] = std::clamp(std::lround(rgb[i] * 31.0f) * 8 / 255.0f, 0.0f, 1.0f);
            if (!WriteDds(p, puddle ? 128 : 64, puddle ? g_detail.pud : g_detail.cum, c, opacity)) return {};
        }
        return std::string(kGenTex) + name;
    }

    // ---------------------------------------------------------------- per-man amount
    float ManMul(RE::FormID giver) {
        std::uint32_t h = giver * 2654435761u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        const float t = (h & 0xFFFF) / 65535.0f;
        const float lo = std::min(g.manMin, g.manMax), hi = std::max(g.manMin, g.manMax);
        return lo + (hi - lo) * t;
    }

    // ---------------------------------------------------------------- game-thread actions
    void Task(std::function<void()> fn) {
        if (auto* t = SKSE::GetTaskInterface()) t->AddTask(std::move(fn));
    }
    void EquipStrand(RE::FormID actorId, RE::FormID newLocal, RE::FormID oldLocal, std::string tex) {
        Task([=]() {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(actorId);
            auto* em = RE::ActorEquipManager::GetSingleton();
            if (!a || !em) return;
            if (newLocal) {
                if (auto* arm = Armo(newLocal)) {
                    AllowRace(arm, a->GetRace());
                    if (!tex.empty()) NiOvrTex(a, "AnatomicaCum", tex);
                    if (a->GetInventoryCounts([arm](RE::TESBoundObject& o) { return &o == arm; }).empty())
                        a->AddObjectToContainer(arm, nullptr, 1, nullptr);
                    em->EquipObject(a, arm, nullptr, 1, nullptr, false, true, false, true);
                }
            }
            if (oldLocal && oldLocal != newLocal) {
                if (auto* arm = Armo(oldLocal)) {
                    em->UnequipObject(a, arm, nullptr, 1, nullptr, false, true, false, true, nullptr);
                    a->RemoveItem(arm, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                }
            }
        });
    }
    // take every copy of our armors that match off him and out of his inventory, whatever we think he wears
    void StripOurs(RE::FormID actorId, bool tipsOnly) {
        Task([=]() {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(actorId);
            auto* em = RE::ActorEquipManager::GetSingleton();
            if (!a || !em) return;
            auto inv = a->GetInventoryCounts([&](RE::TESBoundObject& o) {
                auto* arm = o.As<RE::TESObjectARMO>();
                if (!IsOurs(arm)) return false;
                const auto id = arm->GetLocalFormID();
                return !tipsOnly || (id >= kTipArmo && id < kTipArmo + 2);
            });
            for (auto& [obj, n] : inv) {
                if (n <= 0) continue;
                em->UnequipObject(a, obj, nullptr, n, nullptr, false, true, false, true, nullptr);
                a->RemoveItem(obj, n, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            }
        });
    }
    void PlacePuddle(RE::FormID actorId, float amount, std::string tex) {
        Task([=]() {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(actorId);
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!a || !dh || !a->Is3DLoaded()) return;
            const int size = amount < 0.25f ? 0 : (amount < 0.7f ? 1 : 2);
            auto* base = dh->LookupForm<RE::TESBoundObject>(kPuddleStat + size, kEsp);
            if (!base) return;
            auto ref = a->PlaceObjectAtMe(base, false);
            if (!ref) return;
            // under the anus: a little behind him
            const float ang = a->GetAngleZ();
            const float back = 5.0f + Rand(0.0f, 3.0f), side = Rand(-1.5f, 1.5f);
            RE::NiPoint3 p = a->GetPosition();
            p.x += -std::sin(ang) * back + std::cos(ang) * side;
            p.y += -std::cos(ang) * back - std::sin(ang) * side;
            ref->SetPosition(p);
            ref->SetAngle(RE::NiPoint3(0.0f, 0.0f, Rand(0.0f, 6.2831853f)));
            if (!tex.empty()) NiOvrTex(ref.get(), "AnatomicaPuddle", tex);
            std::lock_guard lk(g_mx);
            g_puddles.push_back({ref->GetFormID(), Now() + g.puddleLife});
        });
    }
    void DeleteRef(RE::FormID id) {
        Task([=]() {
            if (auto* r = RE::TESForm::LookupByID<RE::TESObjectREFR>(id)) {
                r->Disable();
                r->SetDelete(true);
            }
        });
    }

    // ---------------------------------------------------------------- loads
    // slot 59 is shared by the squirt, the overflow and the string: one at a time
    bool Slot59Free(const State& s) { return !Fluid::Busy(s.self); }
    // something is inside him (penetration seen in the last 3 s): never a squirt then
    bool Penetrated(const State& s, double now) { return s.lastWorked > 0.0 && now - s.lastWorked < 3.0; }
    // comes back out of him: an ooze down the skin (hold = how long it hangs), a gush, a small spurt (landing)
    void StartOverflow(State& s, float amt, double now, float hold = -1.0f, int lastFrame = 3) {
        (void)now;
        if (!Slot59Free(s) || amt <= 0.0f) return;
        const Fluid::Kind k = (lastFrame == 2 && hold < 1.3f) ? Fluid::Kind::Land : (hold >= 3.0f ? Fluid::Kind::Gush : Fluid::Kind::Ooze);
        Fluid::Squirt(s.self, amt, s.rgb, k, hold > 0.0f ? hold : g.overflowHold);
        s.leaked += amt;
    }
    // a squirt while something is still inside him: it sprays out around the shaft instead of out of an open hole
    void StartSpray(State& s, float amt, double now) {
        (void)now;
        if (!Slot59Free(s) || amt <= 0.0f) return;
        Fluid::Squirt(s.self, amt, s.rgb, Fluid::Kind::Spray);
        s.leaked += amt;
    }
    // out of the open hole
    void StartSquirt(State& s, float amt) {
        if (amt <= 0.0f) return;
        Fluid::Squirt(s.self, amt, s.rgb, Fluid::Kind::Squirt);
        s.leaked += amt;
    }
    // he pulled out: what is left on his tip (a pearl, then a strand that drips)
    void StartTips(State& s) {
        if (g.tip && g.on)
            for (auto& [gid, col] : s.roundGivers) Fluid::Coat(gid, 1.0f, col.data(), g.tipTime);   // his schlong: wet with it
        s.roundGivers.clear();
    }

    void AddLoad(RE::FormID recvId, const float col[3], int key, float amount) {
        auto& s = g_st[recvId];
        s.self = recvId;
        const double now = Now();
        // still inside him: a framework scene, or penetration in the last few seconds
        const bool inside = key != 0 || now - s.lastWorked < 3.0;
        if (key) s.sceneKey = key;
        if (s.loads >= g.loadCap) {
            // full: it comes straight back out - around his schlong while he is still in, else a squirt
            if (g.squirtOverflow) {
                if (inside) StartSpray(s, amount * 0.6f, now);
                else if (Slot59Free(s) && !Penetrated(s, now)) StartSquirt(s, amount * 0.6f);
            }
            return;
        }
        const float w0 = std::max(s.vol, 0.0f), w1 = amount;
        for (int i = 0; i < 3; ++i) s.rgb[i] = (s.rgb[i] * w0 + col[i] * w1) / std::max(w0 + w1, 0.001f);
        if (s.vol <= 0.001f) for (int i = 0; i < 3; ++i) s.rgb[i] = col[i];
        s.loads += 1;
        s.vol += amount;
        s.volStart = s.vol;
        s.lastClimax = now;
        s.climaxInRound = true;
        s.emptySince = 0.0;
        if (s.legSide == 0) s.legSide = Rand(0.0f, 1.0f) < 0.5f ? 1 : 2;
        // some of it comes back out around him while he is still in - more likely with big loads and as he fills up
        if (g.overflow && inside && Slot59Free(s)) {
            const float big = amount / std::max(g.vol, 0.05f);
            const float fill = static_cast<float>(s.loads) / static_cast<float>(std::max(g.loadCap, 1));
            const float p = std::clamp(g.overflowChance * big * (0.5f + fill), 0.0f, 0.95f);
            if (Rand(0.0f, 1.0f) < p) {
                const float out = std::min(s.vol, amount * g.overflowOut * Rand(0.7f, 1.3f));
                s.vol -= out;
                if (big >= 1.6f && g.squirt) StartSpray(s, out, now);   // a big load squirts out around him
                else StartOverflow(s, out, now);
            }
        }
        if (!g.showDuring && key != 0) { s.waitKey = key; s.waitSince = now; }
    }

    // the giver's cum colour: his race's (race list on this page) or the colour picked here
    void GiverColour(RE::Actor* giver, float out[3]) {
        out[0] = g.colour[0]; out[1] = g.colour[1]; out[2] = g.colour[2];
        if (giver && g.raceColour) Colour::CumColour(giver, out);
    }

    float AmountFor(State& s, RE::Actor* giver) {
        float amount = g.vol;
        if (giver) {
            amount *= ManMul(giver->GetFormID());
            const double now = Now();
            const auto it = g_giverLast.find(giver->GetFormID());
            if (g.firstBig && (it == g_giverLast.end() || now - it->second > g.firstGap * 60.0)) amount *= g.firstMul;
            g_giverLast[giver->GetFormID()] = now;
            s.givers.insert(giver->GetFormID());
        }
        if (g.multi && s.givers.size() > 1) amount *= 1.0f + g.multiPer * static_cast<float>(s.givers.size() - 1);
        return amount;
    }

    // ---------------------------------------------------------------- Papyrus
    // recv got a load from giver; key: OStim thread + 1, SexLab 1000 + thread, 0 = none
    void DripClimax(RE::StaticFunctionTag*, RE::Actor* recv, RE::Actor* giver, std::int32_t key) {
        if (!recv || !giver || recv == giver) return;
        const auto* gb = giver->GetActorBase();
        const auto* rb = recv->GetActorBase();
        if (!gb || gb->GetSex() != RE::SEX::kMale || !rb || rb->GetSex() != RE::SEX::kMale) return;   // Anatomica is a male body
        float col[3];
        GiverColour(giver, col);   // before our lock (the colour code has its own)
        std::lock_guard lk(g_mx);
        if (!g.on) return;
        const bool sexlab = key >= 1000;
        if (sexlab ? !g.trigSexLab : (key > 0 && !g.trigOStim)) return;
        const double now = Now();
        auto& last = g_recent[{recv->GetFormID(), giver->GetFormID()}];
        if (now - last < 4.0) return;   // the same climax reported twice (OStim + SexLab, SLSO + SexLab)
        last = now;
        auto& s = g_st[recv->GetFormID()];
        s.self = recv->GetFormID();
        s.roundGivers[giver->GetFormID()] = {col[0], col[1], col[2]};
        AddLoad(recv->GetFormID(), col, key, AmountFor(s, giver));
    }

    bool NeedEvents(RE::StaticFunctionTag*) { return g_needEvents.exchange(false); }

    // leg trail for the gape script: [stage (-1 nothing to do, 0 clear, 1-3), side (0 left, 1 right), alpha x100, colour]
    // the leg trail used to be a RaceMenu overlay applied by the script; it is a mesh now. This only tells
    // the script to remove an overlay it may still have on him from the older version.
    std::vector<std::int32_t> DripTrail(RE::StaticFunctionTag*, RE::Actor* a) {
        std::vector<std::int32_t> out{-1, 0, 0, 0};
        if (!a) return out;
        std::lock_guard lk(g_mx);
        auto it = g_st.find(a->GetFormID());
        if (it == g_st.end()) return out;
        if (it->second.legSent > 0) { it->second.legSent = 0; out[0] = 0; }
        return out;
    }

    // ---------------------------------------------------------------- worker
    // the strand root follows the lower lip as he gapes (the gape is a morph, not a bone), and the cum pooled
    // in the canal is hidden while something is inside him
    // by name - SMP renames the bones it merges from an armor ("hdtSSEPhysics_AutoRename_... AD2_R"), so a plain
    // lookup can miss them: fall back to a name that ENDS with the one asked for
    RE::NiAVObject* FindNode(RE::NiAVObject* root, const std::string& name) {
        if (auto* o = root->GetObjectByName(RE::BSFixedString(name.c_str()))) return o;
        RE::NiAVObject* hit = nullptr;
        RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* o) {
            const char* n = o ? o->name.c_str() : nullptr;
            if (n) {
                const std::string_view sv(n);
                if (sv.size() > name.size() && sv.substr(sv.size() - name.size()) == name && (sv[sv.size() - name.size() - 1] == ' ' || sv[sv.size() - name.size() - 1] == '_')) {
                    hit = o;
                    return RE::BSVisit::BSVisitControl::kStop;
                }
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        return hit;
    }

    void AdjustNodes(RE::Actor* a, State& s, double now) {
        auto* root = a->Get3D(false);
        if (!root) return;
        const bool cum = s.vol > 0.01f;   // worn only for the wet sheen: the cum pieces stay hidden
        auto show = [](RE::NiAVObject* o, bool on) { if (!o) return; if (on) o->GetFlags().reset(RE::NiAVObject::Flag::kHidden); else o->GetFlags().set(RE::NiAVObject::Flag::kHidden); };
        show(FindNode(root, "AnatomicaFilm"), g.film && cum);
        show(FindNode(root, "AnatomicaLip"), cum);
        show(FindNode(root, "AnatomicaPool"), cum && now - s.lastWorked >= 2.5);
    }

    void ClearActor(RE::FormID id, State& s) {
        if (s.worn) EquipStrand(id, 0, s.worn, {});
        s.worn = 0;
        Fluid::Clear(id);
        s.squirtFrame = 0; s.ofFrame = 0; s.strFrame = 0; s.trailWorn = 0; s.frothLevel = 0.0f;
        s.vol = 0.0f; s.loads = 0; s.givers.clear(); s.tier = s.stage = 0; s.cycleStart = 0.0;
    }

    void Step() {
        const double now = Now();
        auto* pl = RE::PlayerCharacter::GetSingleton();
        std::lock_guard lk(g_mx);
        // stale puddles from a loaded save
        for (auto id : g_staleRefs) DeleteRef(id);
        g_staleRefs.clear();
        for (auto& [lid, amt] : Fluid::TakeLandings()) {
            if (!g.puddles || amt < 0.03f) continue;
            auto it = g_st.find(lid);
            const float* rgb = it != g_st.end() ? it->second.rgb : g.colour;
            PlacePuddle(lid, amt, CumTex(rgb, g.opacity, true));
        }
        for (auto it = g_puddles.begin(); it != g_puddles.end();) {
            if (now >= it->expires || static_cast<int>(g_puddles.size()) > g.puddleMax) { DeleteRef(it->ref); it = g_puddles.erase(it); }
            else ++it;
        }
        for (auto wid : Fluid::WetMen()) if (!g_st.count(wid)) g_st[wid].self = wid;   // wet without cum: the sheen needs the rim armor
        for (auto it = g_st.begin(); it != g_st.end();) {
            const RE::FormID id = it->first;
            State& s = it->second;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!a || a->IsDead()) { ClearActor(id, s); it = g_st.erase(it); continue; }
            ++it;
            if (!a->Is3DLoaded()) continue;
            s.self = id;
            if (!g.on) { if (s.worn || s.vol > 0.0f) ClearActor(id, s); continue; }

            // ---- round tracking (penetration seen by the gape script)
            if (!s.roundOver && now - s.lastWorked > 3.0) {
                s.roundOver = true;
                if (g.trigRound && !s.climaxInRound) {
                    for (auto& [pid, pc] : s.roundPartners) {
                        auto* p = RE::TESForm::LookupByID<RE::Actor>(pid);
                        AddLoad(id, pc.data(), 0, AmountFor(s, p));
                        s.roundGivers[pid] = pc;
                    }
                }
                StartTips(s);
                // froth after a long / rough round
                if (g.froth && s.roundSecs > 0.0f) {
                    const float avgDil = s.roundRough / std::max(s.roundSecs, 0.1f);
                    const float lvl = std::clamp((s.roundSecs - 0.5f * g.frothAfter) / std::max(g.frothAfter, 1.0f), 0.0f, 1.0f) * (0.6f + 0.6f * avgDil);
                    const float left = s.frothLevel * std::max(0.0f, 1.0f - static_cast<float>((now - s.frothStart) / std::max(g.frothTime, 1.0f)));
                    if (lvl > 0.05f && lvl > left) { s.frothLevel = lvl; s.frothStart = now; }
                }
                // he pulled out: a string to him, or maybe a squirt
                const bool string = now - s.lastString < 4.0;   // a string just went: no squirt on top of it
                if (!string && g.squirt && Slot59Free(s) && !Penetrated(s, now) && s.waitKey == 0 && s.vol >= g.squirtMin && Rand(0.0f, 1.0f) < g.squirtChance) {
                    const float amt = s.vol * 0.35f; s.vol -= amt; StartSquirt(s, amt);
                }
            }
            // he just slid out (no penetration for half a second): maybe a string between his hole and the tip
            if (!s.roundOver && !s.pulledOut && now - s.lastWorked > 0.45) {
                s.pulledOut = true;
                RE::FormID partner = 0;
                for (auto& [pid, pc] : s.roundPartners) { partner = pid; break; }
                if (g.string && partner && (s.climaxInRound || s.vol > 0.05f) && now - s.lastString > 8.0 && Rand(0.0f, 1.0f) < g.stringChance) {
                    Fluid::String(id, partner, s.rgb);
                    s.lastString = now;
                }
            }
            if (s.waitKey && now - s.waitSince > 600.0) s.waitKey = 0;   // scene end never came
            // ---- the trail down his leg: longer the more has leaked; gone a while after he is empty, or when he dresses
            {
                int stage = 0;
                if (g.leg && s.leaked > 0.05f) {
                    stage = s.leaked < 0.6f ? 1 : (s.leaked < 1.6f ? 2 : 3);
                    if (s.emptySince > 0.0 && now - s.emptySince > g.legStay) { stage = 0; s.leaked = 0.0f; }
                }
                if (a->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) stage = 0;
                // rivulets running down the inner thigh (Fluid); they dry off on their own
                if (stage != s.trailWorn) { Fluid::Trail(id, static_cast<float>(stage), s.legSide == 2 ? 2 : 1, s.rgb); s.trailWorn = stage; }
            }
            AdjustNodes(a, s, now);
            const double stepDt = s.lastStep > 0.0 ? std::min(now - s.lastStep, 1.0) : 0.0;
            s.lastStep = now;
            if (s.waitKey) { Fluid::Hang(id, 0.0f, 0.0f, s.rgb); continue; }

            // ---- a spurt when he lands
            {
                const bool air = a->IsInMidair();
                if (air && !s.inAir) s.airSince = now;
                if (!air && s.inAir && now - s.airSince > 0.35 && g.land && s.vol > 0.05f && Slot59Free(s) && Rand(0.0f, 1.0f) < g.landChance) {
                    const float amt = std::min(s.vol, 0.12f + 0.1f * static_cast<float>(std::max(s.tier, 1)));
                    s.vol -= amt;
                    StartOverflow(s, amt, now, 1.2f, 2);
                }
                s.inAir = air;
            }

            // ---- gush: he sat / lay down and gets up (or the scene ended)
            if (auto* ast = a->AsActorState()) {
                const bool down = ast->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal;
                if (s.wasDown && !down && g.gush && g.gushStand && s.vol >= g.gushMin && s.gushAt == 0.0) s.gushAt = now + 0.8;
                s.wasDown = down;
            }
            if (s.gushAt > 0.0 && now >= s.gushAt && Slot59Free(s)) {
                s.gushAt = 0.0;
                if (g.gush && s.vol >= g.gushMin) {
                    const float amt = s.vol * g.gushFrac;
                    s.vol -= amt;
                    StartOverflow(s, amt, now, 3.0f);
                }
            }

            // ---- wash off
            if (g.wash) if (auto* st = a->AsActorState(); st && st->IsSwimming()) {
                if (s.vol > 0.0f || s.leaked > 0.0f) { ClearActor(id, s); s.leaked = 0.0f; s.emptySince = 0.0; }
                continue;
            }

            const std::string tex;   // the rim armor keeps its own look
            // ---- strands
            if (s.vol <= 0.01f) {
                Fluid::Hang(id, 0.0f, 0.0f, s.rgb);
                // still wet: the rim armor stays on for its wet sheen (Fluid fades it); dry or dressed: off
                const bool wet = Fluid::WetOf(id) > 0.03f && !a->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody);
                if (wet && s.worn != kStrandArmo) { EquipStrand(id, kStrandArmo, s.worn, {}); s.worn = kStrandArmo; s.lastWornCheck = now; }
                else if (!wet && s.worn) { EquipStrand(id, 0, s.worn, {}); s.worn = 0; }
                if (s.loads > 0) { s.loads = 0; s.givers.clear(); s.vol = 0.0f; s.cycleStart = 0.0; s.emptySince = now; }
            } else {
                s.tier = s.vol < 1.5f ? 1 : (s.vol < 3.0f ? 2 : 3);
                if (s.cycleStart <= 0.0) {
                    s.cycleStart = now;
                    s.dropAmt = std::min(s.vol, 0.3f + 0.15f * s.tier);
                    s.cycleLen = std::clamp(g.leakTime * s.dropAmt / std::max(s.volStart, s.dropAmt), 6.0f, 180.0f) * Rand(0.8f, 1.2f);
                }
                if (g.clench && g.clenchOnly && !s.moving) s.cycleStart += stepDt;   // frozen between clenches
                const float f = static_cast<float>((now - s.cycleStart) / s.cycleLen);
                if (f >= 1.0f) {
                    // the drip falls (Fluid drops it; the puddle comes where it lands)
                    s.vol -= s.dropAmt; s.leaked += s.dropAmt;
                    Fluid::Release(id, s.dropAmt);
                    s.cycleStart = 0.0; s.stage = 0;
                } else {
                    s.stage = f < 0.2f ? 1 : (f < 0.45f ? 2 : (f < 0.75f ? 3 : 4));
                    Fluid::Hang(id, s.vol, f, s.rgb);
                    const RE::FormID want = kStrandArmo + (s.tier - 1) * 4;   // only changes when he gets fuller / emptier
                    if (want != s.worn) {
                        EquipStrand(id, want, s.worn, tex);
                        s.worn = want; s.lastWornCheck = now;
                    } else if (s.worn && now - s.lastWornCheck > 10.0) {
                        // something (a framework undressing him) really took it off: put it back
                        s.lastWornCheck = now;
                        auto* on = a->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kModChestSecondary);
                        auto* mine = Armo(s.worn);
                        if (mine && on != mine) EquipStrand(id, s.worn, 0, tex);
                    }
                }
                // walking: drops on the way
                const auto pos = a->GetPosition();
                if (now - s.lastPosT > 1.0) { s.moving = pos.GetDistance(s.lastPos) > 40.0f; s.lastPos = pos; s.lastPosT = now; }
                if (g.walkDrops && g.puddles && s.moving && now - s.lastWalkDrop > g.walkEvery) {
                    s.lastWalkDrop = now;
                    const float amt = std::min(s.vol, 0.08f);
                    s.vol -= amt; s.leaked += amt;
                    Fluid::Release(id, amt);
                }
            }
        }
        (void)pl;
    }

    // our armor on a man we know nothing about (a save made while he was dripping): take it off
    void CleanStrays() {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* pls = RE::ProcessLists::GetSingleton();
        std::vector<RE::Actor*> actors;
        if (pl) actors.push_back(pl);
        if (pls) for (auto& h : pls->highActorHandles) if (auto p = h.get()) actors.push_back(p.get());
        for (auto* a : actors) {
            if (!a || !a->Is3DLoaded()) continue;
            for (auto slot : {RE::BGSBipedObjectForm::BipedObjectSlot::kModChestSecondary, RE::BGSBipedObjectForm::BipedObjectSlot::kModArmRight, kTipSlot, kTrailSlot}) {
                auto* arm = a->GetWornArmor(slot);
                if (!IsOurs(arm)) continue;
                bool known = false;
                {
                    std::lock_guard lk(g_mx);
                    auto it = g_st.find(a->GetFormID());
                    // only the rim armor is still worn; any other piece of ours is from an older version (SMP strands,
                    // squirt frames, the tip, the leg mesh) and comes off
                    known = it != g_st.end() && it->second.worn == arm->GetLocalFormID();
                }
                if (!known) EquipStrand(a->GetFormID(), 0, arm->GetLocalFormID(), {});
            }
        }
    }

    // the worker only keeps time; everything that touches the game runs as a task on the game thread
    std::atomic<bool> g_stepQueued{false};
    void Loop() {
        double lastClean = 0.0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
            auto* ui = RE::UI::GetSingleton();
            if (!ui || ui->GameIsPaused()) continue;
            if (g_stepQueued.exchange(true)) continue;   // the last one has not run yet
            const double now = Now();
            const bool clean = now - lastClean > 6.0;
            if (clean) lastClean = now;
            Task([clean]() {
                g_stepQueued = false;
                if (!PluginPresent()) return;
                Step();
                if (clean) CleanStrays();
            });
        }
    }

    // ---------------------------------------------------------------- scene end (mod events)
    struct ModSink : RE::BSTEventSink<SKSE::ModCallbackEvent> {
        RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* e, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override {
            if (!e) return RE::BSEventNotifyControl::kContinue;
            const std::string_view name = e->eventName.c_str() ? e->eventName.c_str() : "";
            int key = 0;
            if (name == "ostim_thread_end") key = static_cast<int>(e->numArg) + 1;
            else if (name == "AnimationEnd") key = 1000 + std::atoi(e->strArg.c_str() ? e->strArg.c_str() : "0");
            if (key) {
                std::lock_guard lk(g_mx);
                for (auto& [id, s] : g_st)
                    if (s.waitKey == key || s.sceneKey == key) {
                        s.waitKey = 0; s.sceneKey = 0;
                        StartTips(s);
                        // he gets up: a gush, or maybe a squirt
                        if (g.gush && g.gushScene && s.vol >= g.gushMin) s.gushAt = Now() + 2.0;
                        else if (g.squirt && Slot59Free(s) && !Penetrated(s, Now()) && s.vol >= g.squirtMin && Rand(0.0f, 1.0f) < g.squirtChance) {
                            const float amt = s.vol * 0.35f; s.vol -= amt; StartSquirt(s, amt);
                        }
                    }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    } g_modSink;

    // ---------------------------------------------------------------- bathing mods wash it off
    // Bathing in Skyrim (Renewed): its 'clean' / soapy / bathing-animation effects; any other mod: an effect
    // whose name says bath / wash / soap
    bool IsBathEffect(RE::FormID mgefId) {
        auto* mg = RE::TESForm::LookupByID<RE::EffectSetting>(mgefId);
        if (!mg) return false;
        if (const auto* f = mg->GetFile(0); f && std::string_view(f->GetFilename()) == "Bathing in Skyrim.esp") {
            switch (mg->GetLocalFormID()) {
                case 0x026: case 0x02A: case 0x02B: case 0x02C: case 0x02F: return true;
                default: return false;
            }
        }
        std::string n = mg->GetFullName() ? mg->GetFullName() : "";
        for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return n.find("bath") != std::string::npos || n.find("wash") != std::string::npos || n.find("soap") != std::string::npos;
    }
    struct BathSink : RE::BSTEventSink<RE::TESMagicEffectApplyEvent> {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* e, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override {
            if (!e || !e->target || !g.bath || !IsBathEffect(e->magicEffect)) return RE::BSEventNotifyControl::kContinue;
            const RE::FormID id = e->target->GetFormID();
            std::lock_guard lk(g_mx);
            if (auto it = g_st.find(id); it != g_st.end()) {
                ClearActor(id, it->second);
                it->second.leaked = 0.0f; it->second.emptySince = 0.0; it->second.gushAt = 0.0;
            }
            Fluid::Clear(id);   // the giver: his coated schlong too
            return RE::BSEventNotifyControl::kContinue;
        }
    } g_bathSink;

    // ---------------------------------------------------------------- co-save: puddles to remove next load
    constexpr std::uint32_t kRec = 'PUDL';
    void OnSave(SKSE::SerializationInterface* a) {
        std::lock_guard lk(g_mx);
        if (!a->OpenRecord(kRec, 1)) return;
        const std::uint32_t n = static_cast<std::uint32_t>(g_puddles.size());
        a->WriteRecordData(&n, sizeof n);
        for (auto& p : g_puddles) a->WriteRecordData(&p.ref, sizeof p.ref);
    }
    void OnLoad(SKSE::SerializationInterface* a) {
        std::uint32_t type, ver, len;
        std::lock_guard lk(g_mx);
        while (a->GetNextRecordInfo(type, ver, len)) {
            if (type != kRec) continue;
            std::uint32_t n = 0;
            a->ReadRecordData(&n, sizeof n);
            for (std::uint32_t i = 0; i < n; ++i) {
                RE::FormID old = 0, now = 0;
                a->ReadRecordData(&old, sizeof old);
                if (a->ResolveFormID(old, now)) g_staleRefs.push_back(now);
            }
        }
    }
    void OnRevert(SKSE::SerializationInterface*) {
        std::lock_guard lk(g_mx);
        g_puddles.clear();
    }

    // ---------------------------------------------------------------- menu helpers
    void Help(const char* text) {
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::BeginTooltip();
            ImGuiMCP::PushTextWrapPos(ImGuiMCP::GetFontSize() * 30.0f);
            ImGuiMCP::TextUnformatted(text);
            ImGuiMCP::PopTextWrapPos();
            ImGuiMCP::EndTooltip();
        }
    }
    bool Pct(const char* label, float* v, float hi = 1.0f) {
        float p = *v * 100.0f;
        if (ImGuiMCP::SliderFloat(label, &p, 0.0f, hi * 100.0f, "%.0f%%")) { *v = p / 100.0f; return true; }
        return false;
    }
    bool Mins(const char* label, float* seconds, float lo, float hi) {
        float m = *seconds / 60.0f;
        if (ImGuiMCP::SliderFloat(label, &m, lo, hi, "%.1f min")) { *seconds = m * 60.0f; return true; }
        return false;
    }
    template <class F> void Indent(bool on, F&& body) {
        if (!on) return;
        ImGuiMCP::Indent();
        body();
        ImGuiMCP::Unindent();
    }
}

namespace Drip {
    std::string CumTexFor(const float rgb[3], float opacity) { return CumTex(rgb, opacity, false); }   // no lock: Fluid calls it from its frame
    float TipForward() { return g.tipFwd; }
    float TipDown() { return g.tipDown; }
    void DripTestNative(RE::StaticFunctionTag*, RE::BSFixedString what);
    RE::Actor* TestTarget();
    std::string RunTest(RE::Actor* a, std::string what);

    void Install() {
        if (auto* s = SKSE::GetSerializationInterface()) {
            s->SetUniqueID('ANDR');
            s->SetSaveCallback(OnSave);
            s->SetLoadCallback(OnLoad);
            s->SetRevertCallback(OnRevert);
        }
        std::thread(Loop).detach();
        Fluid::Install();
    }

    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("DripClimax"sv, "AnatomicaNative"sv, DripClimax);
        vm->RegisterFunction("NeedEvents"sv, "AnatomicaNative"sv, NeedEvents);
        vm->RegisterFunction("DripTrail"sv, "AnatomicaNative"sv, DripTrail);
        vm->RegisterFunction("DripTest"sv, "AnatomicaNative"sv, DripTestNative);
    }

    void Reset() {
        static bool sink = false;
        if (!sink) {
            if (auto* src = SKSE::GetModCallbackEventSource()) { src->AddEventSink(&g_modSink); sink = true; }
        }
        static bool bathSink = false;
        if (!bathSink) {
            if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) { src->AddEventSink<RE::TESMagicEffectApplyEvent>(&g_bathSink); bathSink = true; }
        }
        std::lock_guard lk(g_mx);
        g_st.clear();          // armor left on men is removed by CleanStrays
        Fluid::Reset();
        g_recent.clear();
        g_needEvents = true;   // mod event registrations do not survive a load
    }

    void OnWorked(RE::Actor* recv, RE::Actor* partner, float dil) {
        if (!recv) return;
        std::array<float, 3> pc{};
        const bool male = partner && partner != recv && partner->GetActorBase() && partner->GetActorBase()->GetSex() == RE::SEX::kMale;
        if (male) GiverColour(partner, pc.data());
        std::lock_guard lk(g_mx);
        if (!g.on) return;
        auto& s = g_st[recv->GetFormID()];
        s.self = recv->GetFormID();
        const double now = Now();
        if (s.roundOver) {
            s.roundOver = false; s.climaxInRound = false; s.roundPartners.clear(); s.roundStart = now;
            s.roundSecs = 0.0f; s.roundRough = 0.0f;
        } else {
            const float dt = static_cast<float>(std::min(now - s.lastWorked, 2.0));
            s.roundSecs += dt; s.roundRough += dt * std::clamp(dil, 0.0f, 1.0f);
        }
        s.lastWorked = now;
        s.pulledOut = false;
        if (male) s.roundPartners[partner->GetFormID()] = pc;
    }

    void OnClench(RE::FormID id, float amp) {
        std::lock_guard lk(g_mx);
        if (!g.on || !g.clench) return;
        auto it = g_st.find(id);
        if (it == g_st.end()) return;
        State& s = it->second;
        if (s.vol <= 0.01f || s.waitKey) return;
        const double now = Now();
        if (s.cycleStart > 0.0) s.cycleStart -= g.clenchPush * amp * s.cycleLen;   // the drip grows faster
        if (Slot59Free(s) && Rand(0.0f, 1.0f) < g.clenchDribble * amp) {
            const float amt = std::min(s.vol, 0.05f + 0.1f * amp);
            s.vol -= amt;
            StartOverflow(s, amt, now, 1.5f, 2);
        }
    }

    bool Film(RE::FormID id, float rgb[3], float& amt) {
        std::lock_guard lk(g_mx);
        amt = 0.0f;
        if (!g.on || !g.film) return false;
        auto it = g_st.find(id);
        if (it == g_st.end()) return false;
        const State& s = it->second;
        if (s.waitKey) return false;
        if (s.vol > 0.01f) amt = g.filmStrength * std::clamp(0.35f + 0.65f * s.vol / std::max(g.vol * 2.0f, 0.1f), 0.0f, 1.0f);
        else if (s.emptySince > 0.0) amt = g.filmStrength * 0.35f * std::max(0.0f, 1.0f - static_cast<float>((Now() - s.emptySince) / 120.0));
        for (int i = 0; i < 3; ++i) rgb[i] = s.rgb[i];
        return amt > 0.0f;
    }

    float Froth(RE::FormID id) {
        std::lock_guard lk(g_mx);
        if (!g.on || !g.froth) return 0.0f;
        auto it = g_st.find(id);
        if (it == g_st.end() || it->second.frothLevel <= 0.0f) return 0.0f;
        const auto& s = it->second;
        return std::clamp(s.frothLevel * (1.0f - static_cast<float>((Now() - s.frothStart) / std::max(g.frothTime, 1.0f))), 0.0f, 1.0f);
    }

    float HoldOpen(RE::FormID id) {
        std::lock_guard lk(g_mx);
        if (!g.on || !g.fullOpen) return 1.0f;
        auto it = g_st.find(id);
        if (it == g_st.end()) return 1.0f;
        return 1.0f + g.fullOpenAmt * std::clamp(it->second.vol / std::max(g.vol * 2.0f, 0.1f), 0.0f, 1.0f);
    }

    // ---- testing: make any effect happen now on the console-selected man (or you)
    RE::Actor* TestTarget() {
        if (auto sel = RE::Console::GetSelectedRef()) if (auto* a = sel->As<RE::Actor>()) return a;
        return RE::PlayerCharacter::GetSingleton();
    }
    std::string RunTest(RE::Actor* a, std::string what) {
        if (!a) return "nobody to test on";
        for (auto& c : what) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        float col[3]; GiverColour(nullptr, col);
        std::lock_guard lk(g_mx);
        auto& s = g_st[a->GetFormID()];
        s.self = a->GetFormID();
        const double now = Now();
        auto ensure = [&](float v) { if (s.vol < v) { if (s.vol <= 0.01f) for (int i = 0; i < 3; ++i) s.rgb[i] = col[i]; s.vol = v; s.volStart = std::max(s.volStart, v); if (s.loads == 0) s.loads = 1; } };
        auto freeSlot = [&]() { s.squirtFrame = 0; s.ofFrame = 0; s.strFrame = 0; };
        if (what == "load") { AddLoad(a->GetFormID(), col, 0, g.vol); return "one load added"; }
        if (what == "fill") { while (s.loads < g.loadCap) AddLoad(a->GetFormID(), col, 0, g.vol); return "filled to the cap"; }
        if (what == "squirt") { ensure(1.0f); freeSlot(); s.lastWorked = 0.0; const float amt = s.vol * 0.35f; s.vol -= amt; StartSquirt(s, amt); return "squirt"; }
        if (what == "spray") { ensure(1.0f); freeSlot(); StartSpray(s, 0.4f, now); return "spray around the schlong"; }
        if (what == "ooze") { ensure(1.0f); freeSlot(); StartOverflow(s, 0.3f, now); return "ooze around the schlong"; }
        if (what == "gush") { ensure(1.5f); freeSlot(); const float amt = s.vol * g.gushFrac; s.vol -= amt; StartOverflow(s, amt, now, 3.0f); return "gush"; }
        if (what == "string") { ensure(0.5f); return Fluid::TestString(a, s.rgb) ? "a string to a partner pulling away (invisible)" : "he has no 3D"; }
        if (what == "froth") { s.frothLevel = 1.0f; s.frothStart = now; return "froth (needs the colour feature on)"; }
        if (what == "trail") { ensure(0.5f); s.leaked = 3.0f; s.emptySince = 0.0; if (s.legSide == 0) s.legSide = 1; s.trailWorn = -1; return "full leg trail"; }
        if (what == "tip" || what == "coat") { Fluid::Coat(a->GetFormID(), 1.0f, col, g.tipTime); return "his schlong is coated (needs a schlong on him)"; }
        if (what == "drip") { ensure(1.0f); if (s.cycleStart > 0.0) s.cycleStart = now - s.cycleLen; return "the next drip falls"; }
        if (what == "clear") { ClearActor(a->GetFormID(), s); s.leaked = 0.0f; s.emptySince = 0.0; s.frothLevel = 0.0f;
            StripOurs(a->GetFormID(), false);    // everything of ours, even pieces we lost track of
            return "cleared"; }
        return "unknown test: load fill squirt spray ooze gush string drip froth trail coat clear";
    }
    // console: cgf "AnatomicaNative.DripTest" "squirt"   (on the console-selected man, or you)
    void DripTestNative(RE::StaticFunctionTag*, RE::BSFixedString what) {
        const std::string r = RunTest(TestTarget(), what.c_str() ? what.c_str() : "");
        RE::ConsoleLog::GetSingleton()->Print("Anatomica: %s", r.c_str());
    }

    void RenderTests(RE::Actor* who) {
        static std::string last;
        const char* tests[][3] = {
            {"Add a load", "load", "one load in him (strands, film, ooze)"},
            {"Fill him up", "fill", "loads up to the cap"},
            {"Squirt", "squirt", "a squirt out of the open hole (pulses, drops, puddle)"},
            {"Spray around schlong", "spray", "the squirt while something is still inside him"},
            {"Ooze around schlong", "ooze", "overflow around the shaft"},
            {"Gush", "gush", "a gush as when he stands up"},
            {"String", "string", "a string to an invisible partner pulling away: thins, snaps"},
            {"Drip now", "drip", "the drip hanging from him falls"},
            {"Froth", "froth", "bubbles on the rim (colour feature on)"},
            {"Leg trail", "trail", "rivulets running down his leg"},
            {"Coat schlong (as giver)", "coat", "his schlong wet with cum: streaks and a drip from the tip"},
            {"Clear all cum", "clear", "removes all of it"}};
        int n = 0;
        for (auto& t : tests) {
            if (n++ % 4) ImGuiMCP::SameLine();
            if (ImGuiMCP::Button(t[0])) last = RunTest(who, t[1]);
            if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::TextUnformatted(t[2]); ImGuiMCP::EndTooltip(); }
        }
        if (!last.empty()) ImGuiMCP::TextDisabled("Cum: %s", last.c_str());
        bool slow = Fluid::SlowMotion();
        if (ImGuiMCP::Checkbox("Slow motion (to watch squirts, strings and drops)", &slow)) Fluid::SlowMotion(slow);
        ImGuiMCP::TextDisabled("Console: cgf \"AnatomicaNative.DripTest\" \"squirt\"  (load fill squirt spray ooze gush string drip froth trail coat clear)");
    }

    std::string Status(RE::Actor* a) {
        if (!a) return {};
        std::lock_guard lk(g_mx);
        auto it = g_st.find(a->GetFormID());
        if (it == g_st.end() || (it->second.vol <= 0.0f && it->second.leaked <= 0.0f)) return "Cum: none | " + Fluid::Status(a->GetFormID());
        const auto& s = it->second;
        return std::format("Cum: {} load{} ({:.1f} in him, {} partner{}), leaked {:.1f}{} | {}", s.loads, s.loads == 1 ? "" : "s", s.vol,
                           s.givers.size(), s.givers.size() == 1 ? "" : "s", s.leaked, s.waitKey ? " - waiting for the scene to end" : "",
                           Fluid::Status(a->GetFormID()));
    }

    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "Drip";
        std::lock_guard lk(g_mx);
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto F = [&](const char* k, float& v, float lo, float hi) {
            if (save) ini.SetDoubleValue(S, k, v);
            else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi);
        };
        auto I = [&](const char* k, int& v, int lo, int hi) {
            if (save) ini.SetLongValue(S, k, v);
            else v = std::clamp(static_cast<int>(ini.GetLongValue(S, k, v)), lo, hi);
        };
        B("On", g.on); B("OStim", g.trigOStim); B("SexLab", g.trigSexLab); B("RoundEnd", g.trigRound); B("ShowDuring", g.showDuring);
        I("LoadCap", g.loadCap, 1, 5); F("Volume", g.vol, 0.1f, 4.0f); F("ManMin", g.manMin, 0.1f, 4.0f); F("ManMax", g.manMax, 0.1f, 4.0f);
        B("FirstBig", g.firstBig); F("FirstMul", g.firstMul, 1.0f, 5.0f); F("FirstGap", g.firstGap, 1.0f, 1440.0f);
        B("Multi", g.multi); F("MultiPer", g.multiPer, 0.0f, 1.0f); F("LeakTime", g.leakTime, 20.0f, 3600.0f);
        B("Squirt", g.squirt); F("SquirtChance", g.squirtChance, 0.0f, 1.0f); F("SquirtMin", g.squirtMin, 0.0f, 10.0f); B("SquirtOverflow", g.squirtOverflow);
        B("Overflow", g.overflow); F("OverflowChance", g.overflowChance, 0.0f, 1.0f); F("OverflowOut", g.overflowOut, 0.05f, 0.9f);
        F("OverflowHold", g.overflowHold, 1.0f, 60.0f);
        B("Tip", g.tip); F("TipTime", g.tipTime, 10.0f, 1800.0f);
        F("TipForward", g.tipFwd, -3.0f, 5.0f); F("TipDown", g.tipDown, -2.0f, 2.0f);
        B("Gush", g.gush); B("GushScene", g.gushScene); B("GushStand", g.gushStand); F("GushMin", g.gushMin, 0.0f, 10.0f);
        F("GushFrac", g.gushFrac, 0.05f, 0.9f); B("Bath", g.bath);
        B("Film", g.film); F("FilmStrength", g.filmStrength, 0.05f, 1.0f);
        B("Clench", g.clench); F("ClenchPush", g.clenchPush, 0.0f, 1.0f); F("ClenchDribble", g.clenchDribble, 0.0f, 1.0f); B("ClenchOnly", g.clenchOnly);
        B("Froth", g.froth); F("FrothAfter", g.frothAfter, 5.0f, 600.0f); F("FrothTime", g.frothTime, 10.0f, 1800.0f);
        B("String", g.string); F("StringChance", g.stringChance, 0.0f, 1.0f);
        B("FullOpen", g.fullOpen); F("FullOpenAmt", g.fullOpenAmt, 0.0f, 3.0f);
        B("Land", g.land); F("LandChance", g.landChance, 0.0f, 1.0f);
        B("Puddles", g.puddles); F("PuddleLife", g.puddleLife, 10.0f, 3600.0f); I("PuddleMax", g.puddleMax, 1, 100);
        B("WalkDrops", g.walkDrops); F("WalkEvery", g.walkEvery, 1.0f, 60.0f);
        B("Leg", g.leg); F("LegStay", g.legStay, 30.0f, 7200.0f); F("LegAlpha", g.legAlpha, 0.1f, 1.0f);
        Fluid::Ini(ini, save);
        B("RaceColour", g.raceColour); F("Opacity", g.opacity, 0.2f, 1.0f); B("Wash", g.wash);
        if (save) ini.SetValue(S, "Colour", std::format("{:.3f},{:.3f},{:.3f}", g.colour[0], g.colour[1], g.colour[2]).c_str());
        else if (const char* v = ini.GetValue(S, "Colour", nullptr)) {
            std::sscanf(v, "%f,%f,%f", &g.colour[0], &g.colour[1], &g.colour[2]);
            for (auto& c : g.colour) c = std::clamp(c, 0.0f, 1.0f);
        }
    }

    void __stdcall RenderPage() {
        bool ch = false;
        Settings& s = g;   // the menu runs on the render thread; values are plain floats/bools
        if (!PluginPresent()) {
            ImGuiMCP::TextWrapped("AnatomicaGape.esp (Lingering Gape) is not active - the drip meshes live in it.");
            return;
        }
        ch |= ImGuiMCP::Checkbox("Cum drips from the anus", &s.on);
        Help("Anatomica's own cum: strands hanging from his hole, squirts, oozing, a string to the partner, rivulets down "
             "his leg, puddles. Simulated by the DLL every frame against his real body shape - no SMP needed.");
        Indent(s.on, [&] {
            ImGuiMCP::SeparatorText("When");
            ch |= ImGuiMCP::Checkbox("OStim: each time a partner climaxes in him", &s.trigOStim);
            ch |= ImGuiMCP::Checkbox("SexLab: each climax (Separate Orgasm supported)", &s.trigSexLab);
            ch |= ImGuiMCP::Checkbox("End of a round, if no climax was counted (one load per partner)", &s.trigRound);
            Help("Works without any framework: when the penetration stops for a few seconds. Also catches scenes where the "
                 "framework sends no climax.");
            ch |= ImGuiMCP::Checkbox("Show it while still in the scene", &s.showDuring);
            Help("Off = the loads are counted during the scene and the strands appear when the scene ends.");

            ImGuiMCP::SeparatorText("How much");
            ch |= ImGuiMCP::SliderInt("Loads he can hold", &s.loadCap, 1, 5);
            Help("Each climax is one load. Once he is full, the next ones come straight back out (squirt, if on).");
            ch |= ImGuiMCP::SliderFloat("One load", &s.vol, 0.2f, 3.0f, "%.2f");
            ch |= ImGuiMCP::SliderFloat("Each man: from", &s.manMin, 0.2f, 3.0f, "x%.2f");
            ch |= ImGuiMCP::SliderFloat("Each man: to", &s.manMax, 0.2f, 3.0f, "x%.2f");
            Help("Every man gets his own amount somewhere in this range (always the same for him).");
            ch |= ImGuiMCP::Checkbox("First load in a while is bigger", &s.firstBig);
            Indent(s.firstBig, [&] {
                ch |= ImGuiMCP::SliderFloat("Bigger by", &s.firstMul, 1.0f, 5.0f, "x%.1f");
                ch |= ImGuiMCP::SliderFloat("If he has not come for", &s.firstGap, 5.0f, 600.0f, "%.0f min");
            });
            ch |= ImGuiMCP::Checkbox("More partners = more", &s.multi);
            Indent(s.multi, [&] { ch |= Pct("Extra per extra partner", &s.multiPer); });

            ImGuiMCP::SeparatorText("While he is being filled");
            ch |= ImGuiMCP::Checkbox("Some comes back out around his schlong", &s.overflow);
            Help("While the partner is still inside: a ring of cum squeezes out around the shaft, runs down from the hole "
                 "and drips (puddle). Big loads and a fuller man make it more likely.");
            Indent(s.overflow, [&] {
                ch |= Pct("Chance (one normal load, half full)", &s.overflowChance);
                ch |= Pct("How much of the load comes out", &s.overflowOut, 0.9f);
                ch |= ImGuiMCP::SliderFloat("Stays around the hole for", &s.overflowHold, 2.0f, 30.0f, "%.0f s");
            });

            ImGuiMCP::SeparatorText("Leaking");
            ch |= Mins("A full load drains in", &s.leakTime, 0.5f, 30.0f);
            Help("A drip grows, falls (puddle) and the next one starts until he is empty. Bigger amounts = thicker strands, "
                 "more of them.");
            ch |= ImGuiMCP::Checkbox("Squirt when he pulls out / the scene ends", &s.squirt);
            Help("Pulses out of the open hole as a stream that pulls apart into drops; they splash on him or the floor "
                 "(puddles). While something is still inside him it never shoots out of an open hole: it sprays out around "
                 "his schlong instead (when he is already full, or with a big load).");
            Indent(s.squirt, [&] {
                ch |= Pct("Chance", &s.squirtChance);
                ch |= ImGuiMCP::SliderFloat("Only with at least", &s.squirtMin, 0.0f, 5.0f, "%.1f in him");
            });
            ch |= ImGuiMCP::Checkbox("When he is already full, it comes straight back out", &s.squirtOverflow);
            Help("Around the schlong while the partner is still in him, as a squirt otherwise.");
            ch |= ImGuiMCP::Checkbox("Puddles on the floor", &s.puddles);
            Indent(s.puddles, [&] {
                ch |= Mins("Puddles stay", &s.puddleLife, 0.5f, 60.0f);
                ch |= ImGuiMCP::SliderInt("At most", &s.puddleMax, 4, 60);
                ch |= ImGuiMCP::Checkbox("Drops while he walks", &s.walkDrops);
                Indent(s.walkDrops, [&] { ch |= ImGuiMCP::SliderFloat("A drop every", &s.walkEvery, 2.0f, 30.0f, "%.0f s"); });
            });
            ch |= ImGuiMCP::Checkbox("Runs down his leg", &s.leg);
            Help("Rivulets that run from the hole down the inside of one thigh over his actual body, more the more has "
                 "leaked, leaving a wet trail that dries (drying time: Look and physics). Gone when he dresses.");
            Indent(s.leg, [&] {
                ch |= Mins("Stays after he is empty", &s.legStay, 0.5f, 120.0f);
            });
            ch |= ImGuiMCP::Checkbox("A gush when he gets up", &s.gush);
            Help("A bigger amount runs out of him at once: around the hole and down, then a puddle.");
            Indent(s.gush, [&] {
                ch |= ImGuiMCP::Checkbox("When a scene ends", &s.gushScene);
                ch |= ImGuiMCP::Checkbox("After sitting or lying down", &s.gushStand);
                ch |= ImGuiMCP::SliderFloat("Only with at least##gush", &s.gushMin, 0.0f, 5.0f, "%.1f in him");
                ch |= Pct("How much comes out", &s.gushFrac, 0.9f);
            });
            ch |= ImGuiMCP::Checkbox("Swimming washes it all off", &s.wash);
            ch |= ImGuiMCP::Checkbox("Bathing mods wash it off", &s.bath);
            Help("Bathing in Skyrim (and its follower / wash rag bathing), or any mod whose bathing effect is called bath, "
                 "wash or soap. Also cleans the giver's tip.");

            ImGuiMCP::SeparatorText("At the hole");
            ch |= ImGuiMCP::Checkbox("Cum film over the rim while it is in him", &s.film);
            Help("A thin glossy layer of cum lying on the inner edge of the rim, creeping further down at the bottom. A mesh "
                 "on top of the skin - the rim's own colour is never changed - and it opens with him when he gapes. The cum "
                 "pooled inside the canal shows whenever he gapes and hides while he is penetrated.");
            ch |= ImGuiMCP::Checkbox("His clenches push it out", &s.clench);
            Help("Each clench (Movement page) makes the drip grow faster and sometimes squeezes a little out.");
            Indent(s.clench, [&] {
                ch |= Pct("Push per clench", &s.clenchPush);
                ch |= Pct("Chance of a dribble", &s.clenchDribble);
                ch |= ImGuiMCP::Checkbox("Only leaks when he clenches or walks (no timer)", &s.clenchOnly);
            });
            ch |= ImGuiMCP::Checkbox("Froth at the rim after a long or rough round", &s.froth);
            Indent(s.froth, [&] {
                ch |= ImGuiMCP::SliderFloat("Full froth after", &s.frothAfter, 10.0f, 300.0f, "%.0f s of penetration");
                ch |= Mins("Froth stays", &s.frothTime, 0.5f, 20.0f);
            });
            Help("Little bubbles painted onto the rim texture, so they stay on the rim however wide he gapes. Rougher "
                 "(wider) rounds make more. Needs the colour feature on (Colours page).");
            ch |= ImGuiMCP::Checkbox("A string to him when he pulls out", &s.string);
            Indent(s.string, [&] { ch |= Pct("Chance##string", &s.stringChance); });
            Help("A string between his hole and the partner's tip as he pulls out: it thins as they part and snaps "
                 "(distance: Look and physics). One half springs back and hangs from the hole, the other stays on his tip.");
            ch |= ImGuiMCP::Checkbox("The gape stays open longer while he is full", &s.fullOpen);
            Indent(s.fullOpen, [&] { ch |= ImGuiMCP::SliderFloat("Slower by up to", &s.fullOpenAmt, 0.0f, 3.0f, "+%.1fx"); });
            ch |= ImGuiMCP::Checkbox("A spurt when he lands from a jump or fall", &s.land);
            Indent(s.land, [&] { ch |= Pct("Chance##land", &s.landChance); });

            ImGuiMCP::SeparatorText("The giver");
            ch |= ImGuiMCP::Checkbox("His schlong comes out wet with cum", &s.tip);
            Help("When he pulls out his schlong glistens, streaks of cum run down it and now and then a drop hangs from "
                 "the tip and falls. Fades over the time below, or when he bathes.");
            Indent(s.tip, [&] {
                ch |= Mins("Stays for", &s.tipTime, 0.25f, 10.0f);
                if (ImGuiMCP::TreeNode("Tip position (only if his schlong mesh can't be read)")) {
                    ch |= ImGuiMCP::SliderFloat("Along the schlong", &s.tipFwd, -3.0f, 5.0f, "%.1f");
                    ch |= ImGuiMCP::SliderFloat("Toward the underside", &s.tipDown, -2.0f, 2.0f, "%.1f");
                    ImGuiMCP::TreePop();
                }
            });

            ImGuiMCP::SeparatorText("Look and physics");
            if (ch) { Colour::SaveNow(); ch = false; }
            Fluid::RenderSettings();

            ImGuiMCP::SeparatorText("Colour");
            ch |= ImGuiMCP::Checkbox("Colour by the giver's race (list below)", &s.raceColour);
            ch |= ImGuiMCP::ColorEdit3("Colour", s.colour, ImGuiMCP::ImGuiColorEditFlags_PickerHueWheel);
            Help("Used when the giver's race has no cum colour of its own. Several men mix their colours.");
            if (ch) { Colour::SaveNow(); ch = false; }   // save before the race list (it saves on its own)
            Colour::RenderCumColours();
            if (ImGuiMCP::Button("Back to defaults")) { s = Settings{}; ch = true; }

            ImGuiMCP::TextDisabled("Test every cum effect on the Testing page.");
        });
        if (ch) Colour::SaveNow();
    }
}

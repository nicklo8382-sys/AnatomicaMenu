#include "OStrandsLink.h"
#include "Training.h"
#include "Lang.h"
#include "Fluid.h"
#include "Engine.h"
#include "Colour.h"
#include "Virgin.h"
#include "Colossal.h"
#include "Face.h"
#include <vector>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

// See OStrandsLink.h.
namespace {
    struct Settings {
        bool on = true;                 // take what OStrands puts in him and let it out through OStrands
        float leakMinutes = 4.0f;       // a full load drains in this long while he is open
        float gush = 0.45f;             // share of what he holds that gushes out as the schlong slides out
        bool clench = true;             // a clench squirts some out
        float clenchShare = 0.15f;      // share squirted per full clench
        bool seep = true;               // it seeps out round the shaft while he is still being taken
        int perLoad = 150;              // OStrands particles in one load
        float maxLoads = 5.0f;          // the most he holds; more comes straight back out
        bool dressed = true;            // nothing comes out while his body slot is covered
        // 2.6e flood: when he gapes open, a chance that it pours out in one steady stream
        bool flood = true;
        float floodChance = 0.6f;       // chance per gape (each time he opens past the threshold)
        float floodDrain = 0.7f;        // share of what he holds that a flood lets out
        float floodSeconds = 6.0f;      // how long the stream runs
        float floodGape = 0.35f;        // how open he has to be (0-1 of the full gape)
        bool holdIn = false;            // 2.6e: no slow ooze while he is closed - it only comes out when he gapes, clenches or is emptied
        float floodFree = 0.3f;         // 2.6e: chance a flood costs nothing (it pours, but he stays as full)
        bool belly = true;              // 2.6e: his belly rounds out with what he holds
        float bellyMax = 1.0f;          // ... this much of the Stomach shape (Anatomica Bulge Belly) when he holds "full"
        float bellyFull = 5.0f;         // ... at this many loads
        bool walk = true;               // 2.6e: it drips out of him while he walks / runs
        float walkRate = 0.15f;         // loads a minute while walking (double running)
        float walkFree = 0.5f;          // chance each drip costs nothing
        bool stand = true;              // 2.6e: a flood when he gets up and walks off after sex
        float standChance = 0.7f;
        float standDrain = 0.5f;        // share of what he holds
        float standFree = 0.2f;         // chance it costs nothing
        bool wetTrail = true;           // 2.6f: a flood or leak leaves his rim and thighs wet and glossy
        // 2.6f: by his experience - a tight / virgin man vs a well-used one (in between by how used he is)
        bool byExp = true;
        float holdTight = 0.6f, holdUsed = 1.6f;       // x Most he holds
        float squirtTight = 0.7f, squirtUsed = 1.5f;   // x how hard and how much a clench squirts
        float floodTight = 0.5f, floodUsed = 1.5f;     // x the flood chances
        // 2.6f: the ring - while a load is pumped into him fully in, it squirts out all round the shaft
        bool ring = true;
        bool ringFullOnly = true;
        float ringDepth = 9.0f;         // units in (the tip) that count as fully in
        float ringShare = 0.25f;        // share of what goes in that squirts back out round the shaft
        float ringRadius = 1.8f;        // 2.6h: how far round his hole the string lies (units; about the shaft's radius)
        bool plug = true;               // 2.6g: a worn plug holds it all in; pulling it out floods
        float plugDrain = 0.85f;
        bool bath = true;               // 2.6g: washing (Bathing in Skyrim) lets it all out, slowly
        float stuffedAt = 3.0f;         // 2.6g: "stuffed full" from this many loads (SkyrimNet)
        // 2.6i: some stays hidden in him after he seems empty - crouching or sitting can squirt it out for a few game hours
        bool late = true;
        float lateShare = 0.2f;         // share of the most he held
        float lateHours = 4.0f;         // game hours it can still come out
        float lateChance = 0.35f;       // chance per check (every ~8 s) while he crouches / sits / kneels
        // 2.6i: strings of it ease out from inside his canal as it oozes
        bool innerStrings = true;
        float innerChance = 0.25f;
        // 2.6i: cum on his skin (a RaceMenu cum overlay other mods can see)
        bool overlay = true;
        float overlayHours = 2.0f;      // game hours it stays after he is empty
        // 2.6j: holding and overflow
        bool gravity = true;            // how fast it runs out follows how he lies (bottom up holds it, standing / squatting drains)
        bool squeeze = true;            // while he is full, thrusts squeeze some out round the shaft; a new man going in pushes more out
        float squeezeShare = 0.04f;     // of what he holds, each deep thrust
        bool pressure = true;           // lying on his full belly pushes spurts out
        bool shift = true;              // 2.6j: changing into / out of a beast form squeezes some out (OStim Beast Form)
    } g;

    struct State {
        float vol = 0.0f;               // loads in him
        float rgb[3] = {0.96f, 0.95f, 0.90f};
        double lastWorked = 0.0, lastIn = 0.0;
        bool plugged = false;
        float gushLeft = 0.0f, squirtLeft = 0.0f, squirtSpeed = 0.0f;   // loads still to come out in a gush / squirt
        float floodLeft = 0.0f, floodRate = 0.0f, lastGape = 0.0f;      // 2.6e flood: loads still to pour, loads per second
        std::uint32_t floodStream = 0;
        bool floodFree = false;         // 2.6e: this flood costs nothing
        RE::NiPoint3 lastPos{};         // 2.6e: to tell when he walks
        bool hasPos = false, standDone = true;
        float walkAcc = 0.0f;
        float ringLeft = 0.0f;          // 2.6f: loads still to squirt round the shaft
        std::uint32_t ringStream = 0;
        float ringParts = 0.0f;         // 2.6h: particles waiting to make the next string round his rim
        double floodAt = 0.0, leakAt = 0.0;   // 2.6f: when it last started flooding / dripped (SkyrimNet)
        struct Pool { RE::FormID giver = 0; float rgb[3]{}; float vol = 0.0f; float glow[4]{}; };   // 2.6i: glow (rgb + strength, 0 = none)
        std::vector<Pool> pools;        // 2.6g: each giver's cum and its colour (streaks in a flood)
        bool plugWorn = false;          // 2.6g
        double plugCheck = 0.0;
        bool stuffed = false;           // 2.6g: over "stuffed full" now (counted once each time)
        RE::FormID giver = 0;           // 2.6f: who is filling him now, and how much this time
        float climax = 0.0f;
        double climaxLast = 0.0;
        float acc = 0.0f;               // particles owed (fractions carried over)
        std::uint32_t stream = 0, squirtStream = 0;
        // 2.6i
        float peak = 0.0f;              // the most he held this time
        float residue = 0.0f;           // hidden in him after he seems empty (a late squirt)
        float residueUntil = 0.0f;      // game hours
        double lateCheck = 0.0;
        float emptyAt = -1.0f;          // game hours he was emptied (overlay fades after)
        int ovlSent = 0; float ovlAlphaSent = -1.0f; int ovlColSent = -1; double ovlAt = 0.0;
        bool washed = false;            // washed since he was last filled: nothing hidden, no cum on his skin after
        float lastDepth = -100.0f;      // 2.6j: how deep the last tick (thrusts)
        float maxDepth = 0.0f; bool outStroke = false;
        bool deepIn = false;
        RE::FormID lastTaker = 0;       // 2.6j: the man in him last time (a new one pushes it out)
        double pressAt = 0.0;
        int beast = -1;                 // 2.6j: his beast form last look (-1 = not looked yet)
    };
    float GameHours() { auto* c = RE::Calendar::GetSingleton(); return c ? c->GetHoursPassed() : 0.0f; }
    bool Low(RE::Actor* a) {   // crouching, sitting or kneeling
        if (!a) return false;
        if (a->IsSneaking()) return true;
        auto* st = a->AsActorState();
        return st && st->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSitting;
    }

    using clock_t_ = std::chrono::steady_clock;
    double Now() { return std::chrono::duration<double>(clock_t_::now().time_since_epoch()).count(); }

    std::mutex g_mx;
    std::unordered_map<RE::FormID, State> g_st;
    std::atomic<bool> g_queued{false};
    std::uint32_t g_nextStream = 1;
    std::mt19937 g_rng{std::random_device{}()};
    float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }

    // ---- OStrands (0_OStrands.dll): its exports
    using EmitFn = bool (*)(RE::Actor*, std::uint32_t, const float*, const float*, int, const float*);
    using OnFn = bool (*)();
    using EmitGlowFn = bool (*)(RE::Actor*, std::uint32_t, const float*, const float*, int, const float*, const float*);
    struct Api { bool tried = false; int version = 0; EmitFn emit = nullptr; OnFn semenOn = nullptr; EmitGlowFn emitGlow = nullptr; } g_api;
    void Resolve() {
        if (g_api.tried) return;
        g_api.tried = true;
        auto* m = REX::W32::GetModuleHandleW(L"0_OStrands.dll");
        if (!m) return;
        using VerFn = int (*)();
        auto ver = reinterpret_cast<VerFn>(REX::W32::GetProcAddress(m, "OStrands_ApiVersion"));
        g_api.emit = reinterpret_cast<EmitFn>(REX::W32::GetProcAddress(m, "OStrands_Emit"));
        g_api.semenOn = reinterpret_cast<OnFn>(REX::W32::GetProcAddress(m, "OStrands_SemenOn"));
        g_api.emitGlow = reinterpret_cast<EmitGlowFn>(REX::W32::GetProcAddress(m, "OStrands_EmitGlow"));
        if (!ver || !g_api.emit) { g_api.emit = nullptr; return; }
        g_api.version = ver();
        logger::info("Anatomica: OStrands link v{} - OStrands draws his cum", g_api.version);
    }

    bool Male(RE::Actor* a) {
        const auto* b = a ? a->GetActorBase() : nullptr;
        return b && b->GetSex() == RE::SEX::kMale;
    }

    bool Covered(RE::Actor* a) {
        auto* armor = a ? a->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody) : nullptr;
        return armor != nullptr;
    }

    // 2.6f: how used he is (0 = virgin / tight, 1 = well used); a mix of the two settings by it
    float Lvl(RE::FormID id) {
        const Virgin::Info v = Virgin::Get(id);
        if (!v.known) return 0.5f;
        return v.virgin ? 0.0f : std::clamp(v.exp / 12.0f, 0.0f, 1.0f);
    }
    float ByExp(float tight, float used, float lvl) { return g.byExp ? tight + (used - tight) * lvl : 1.0f; }
    std::vector<OStrandsLink::Filled> g_filled;   // 2.6f: finished fillings for SkyrimNet
    std::vector<RE::FormID> g_stuffedEv;          // 2.6g: men who just got stuffed full
    std::vector<std::pair<RE::Actor*, float>>* g_wets = nullptr;   // 2.6f: wet him after the tick (Colour's lock)
    void Wet(RE::Actor* a, float w) {
        if (!g_wets) return;
        for (auto& [x, v] : *g_wets) if (x == a) { v = std::max(v, w); return; }
        g_wets->emplace_back(a, w);
    }

    // 2.6g: the colour for this bit of what comes out - one giver's, picked by how much of his is in there (streaks)
    const float* g_pickGlow = nullptr;   // 2.6i: the glow of the pool PickRgb just picked (Out sends it along)
    const float* PickRgb(const auto& s) {
        float total = 0.0f;
        for (auto& p : s.pools) total += p.vol;
        g_pickGlow = nullptr;
        if (s.pools.size() < 2 || total <= 0.0f) {
            if (s.pools.empty()) return s.rgb;
            g_pickGlow = s.pools[0].glow;
            return s.pools[0].rgb;
        }
        float r = Rand(0.0f, total);
        for (auto& p : s.pools) { if ((r -= p.vol) <= 0.0f) { g_pickGlow = p.glow; return p.rgb; } }
        g_pickGlow = s.pools.back().glow;
        return s.pools.back().rgb;
    }
    // 2.6g: a plug in him (Devious Devices / any worn item with an anal plug keyword)
    bool PlugWorn(RE::Actor* a) {
        auto inv = a->GetInventory([](RE::TESBoundObject& o) { return o.IsArmor(); });
        for (auto& [obj, data] : inv) {
            if (!data.second || !data.second->IsWorn()) continue;
            auto* kf = obj->As<RE::BGSKeywordForm>();
            if (!kf) continue;
            for (std::uint32_t i = 0; i < kf->numKeywords; ++i) {
                auto* kw = kf->keywords[i];
                if (!kw) continue;
                std::string e = kw->GetFormEditorID();
                for (auto& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (e.find("plug") != std::string::npos && e.find("vaginal") == std::string::npos) return true;
            }
        }
        return false;
    }

    // emit n particles from his lip (centre c, pointing out along o) with this speed (units/s) and spread
    void Out(RE::Actor* a, std::uint32_t stream, const RE::NiPoint3& at, const RE::NiPoint3& vel, int n, const float rgb[3]) {
        const float* glow = g_pickGlow;
        g_pickGlow = nullptr;
        if (n <= 0 || !g_api.emit) return;
        const float p[3] = {at.x, at.y, at.z}, v[3] = {vel.x, vel.y, vel.z};
        if (glow && glow[3] > 0.0f && g_api.emitGlow) g_api.emitGlow(a, stream, p, v, n, rgb, glow);   // 2.6i: his glowing cum (OStrands 0.8.0.7+)
        else g_api.emit(a, stream, p, v, n, rgb);
    }

    void StepOne(RE::Actor* a, State& s, float dt, double now) {
        RE::NiPoint3 c, d;
        if (!Fluid::HoleFrame(a, c, d)) return;
        RE::NiPoint3 out = c - d;
        if (out.Unitize() < 1e-3f) return;
        const RE::NiPoint3 down{0.0f, 0.0f, -1.0f};
        RE::NiPoint3 side = out.Cross(down);
        if (side.Unitize() < 1e-3f) side = {1.0f, 0.0f, 0.0f};
        const RE::NiPoint3 low = c + out * 0.3f + down * 0.35f;
        const bool plugged = now - s.lastWorked < 1.2;
        const float per = static_cast<float>(std::max(g.perLoad, 10));
        if (s.plugged && !plugged && s.vol > 0.05f) {   // he just slid out: a gush, bigger the fuller he is
            s.gushLeft += s.vol * g.gush;
            s.stream = g_nextStream++;
        }
        s.plugged = plugged;
        if (g.dressed && Covered(a)) { s.gushLeft = s.squirtLeft = 0.0f; return; }
        float gape = std::clamp(Engine::GapeOf(a->GetFormID()), 0.0f, 1.0f);
        auto take = [&](float loads) {
            loads = std::min(loads, s.vol);
            const float k = s.vol > 0.0f ? (s.vol - loads) / s.vol : 0.0f;
            s.vol -= loads;
            for (auto& pl : s.pools) pl.vol *= k;
            return loads;
        };
        // 2.6g: a plug holds it all in; pulling it out floods
        if (now - s.plugCheck > 1.0) {
            s.plugCheck = now;
            const bool worn = g.plug && PlugWorn(a);
            if (s.plugWorn && !worn && s.vol > 0.05f) {
                s.floodLeft = s.vol * g.plugDrain;
                s.floodRate = s.floodLeft / std::max(g.floodSeconds * 0.7f, 0.5f);
                s.floodStream = g_nextStream++;
                s.floodFree = false;
                s.floodAt = now;
            }
            s.plugWorn = worn;
        }
        if (s.plugWorn) { s.gushLeft = s.squirtLeft = s.ringLeft = 0.0f; return; }
        // 2.6j gravity: bottom up it stays in, standing it runs, squatting (opening pointing down) it pours
        const float grav = g.gravity ? 0.2f + 1.8f * std::pow(std::clamp(0.5f - 0.5f * out.z, 0.0f, 1.0f), 1.5f) : 1.0f;
        // 2.6j: while he is full and being taken, each deep thrust squeezes a little out round the shaft, and a new man
        // going in pushes out more
        if (g.squeeze && s.vol > 0.2f) {
            const Engine::During dn = Engine::DuringOf(a->GetFormID());
            const bool in = dn.depth > 2.0f && now - s.lastWorked < 1.0;
            if (in) {
                const RE::FormID taker = s.giver ? s.giver : Engine::BulgeOf(a->GetFormID()).partner;
                if (!s.deepIn && taker && s.lastTaker && taker != s.lastTaker) {   // someone else now: pushed out
                    s.ringLeft += s.vol * 0.25f;
                    if (s.ringLeft <= s.vol * 0.25f + 0.001f) s.ringStream = g_nextStream, g_nextStream += 8;
                }
                if (taker) s.lastTaker = taker;
                // a thrust = back out past 60% of his deepest lately, then in past 85% again
                s.maxDepth = std::max(dn.depth, s.maxDepth - dt * 0.5f);
                if (dn.depth < s.maxDepth * 0.6f) s.outStroke = true;
                const bool deepNow = s.outStroke && dn.depth > s.maxDepth * 0.85f;
                if (deepNow) s.outStroke = false;
                if (deepNow && s.ringLeft <= 0.0f) {   // a thrust: a little wells out round the shaft
                    s.ringStream = g_nextStream, g_nextStream += 8;
                    s.ringLeft += s.vol * g.squeezeShare * std::min(s.vol / 2.0f, 2.0f);
                }
            }
            s.deepIn = in;
            s.lastDepth = dn.depth;
        }
        // 2.6j: the change into / out of a beast form squeezes some out of him
        {
            const int bf = Colossal::BeastForm(a);
            if (s.beast >= 0 && bf != s.beast && g.shift && s.vol > 0.3f && !plugged) {
                s.gushLeft += s.vol * 0.3f;
                s.stream = g_nextStream++;
                logger::info("Anatomica: {} changed form with {:.2f} loads in him - a gush", a->GetName(), s.vol);
            }
            s.beast = bf;
        }
        // 2.6j: lying on his full belly - spurts pushed out
        if (g.pressure && s.vol > 1.0f && !plugged && now - s.pressAt > 6.0) {
            auto* pelvis = a->Get3D(false) ? a->Get3D(false)->GetObjectByName(RE::BSFixedString("NPC Pelvis [Pelv]")) : nullptr;
            if (pelvis && pelvis->world.translate.z - a->GetPosition().z < 22.0f && out.z > 0.3f) {
                s.pressAt = now + Rand(0.0f, 6.0f);
                s.squirtLeft += s.vol * 0.08f;
                s.squirtSpeed = Rand(10.0f, 18.0f);
                s.squirtStream = g_nextStream++;
            }
        }
        // 2.6i: a little stays hidden in him once he seems empty; crouching / sitting can squirt it out later
        {
            const float gh = GameHours();
            s.peak = std::max(s.peak, s.vol);
            const bool flowing = s.gushLeft > 0.0f || s.squirtLeft > 0.0f || s.floodLeft > 0.0f || s.ringLeft > 0.0f;
            if (s.vol <= 0.03f && !flowing && s.peak >= 0.3f) {
                Training::Emptied(a->GetFormID());   // 2.6j: the faster belly growth starts over
                if (g.late && !s.washed) { s.residue = s.peak * g.lateShare; s.residueUntil = gh + g.lateHours; }
                s.emptyAt = s.washed ? -100.0f : gh;
                s.peak = 0.0f;
            }
            if (s.residue > 0.0f && gh > s.residueUntil) s.residue = 0.0f;
            if (g.late && s.residue > 0.01f && !plugged && now - s.lateCheck > 8.0 && !(g.dressed && Covered(a))) {
                s.lateCheck = now;
                if (Low(a) && Rand(0.0f, 1.0f) < g.lateChance) {
                    const float r = std::min(s.residue, std::max(0.08f, s.residue * Rand(0.4f, 1.0f)));
                    s.residue -= r;
                    s.vol += r;
                    s.squirtLeft += r;
                    s.squirtSpeed = Rand(14.0f, 24.0f);
                    s.squirtStream = g_nextStream++;
                    s.leakAt = now;
                    if (g.wetTrail) Wet(a, 0.6f);
                }
            }
        }
        // 2.6e flood: each time he opens up past the threshold, a chance it all starts pouring out in one stream
        const float lvl = Lvl(a->GetFormID());
        if (g.flood && !plugged && gape >= g.floodGape && s.lastGape < g.floodGape && s.vol > 0.05f && s.floodLeft <= 0.0f &&
            Rand(0.0f, 1.0f) < g.floodChance * ByExp(g.floodTight, g.floodUsed, lvl)) {
            s.floodAt = now;
            s.floodLeft = s.vol * g.floodDrain;
            s.floodRate = s.floodLeft / std::max(g.floodSeconds, 0.5f);
            s.floodStream = g_nextStream++;
            s.floodFree = Rand(0.0f, 1.0f) < g.floodFree;
        }
        s.lastGape = gape;
        // 2.6e: walking - how fast he moves (units a second)
        const RE::NiPoint3 pos = a->GetPosition();
        float speed = 0.0f;
        if (s.hasPos && dt > 0.0f) speed = RE::NiPoint3(pos.x - s.lastPos.x, pos.y - s.lastPos.y, 0.0f).Length() / dt;
        s.lastPos = pos; s.hasPos = true;
        if (plugged) s.standDone = false;
        // 2.6e: he gets up and walks off after sex - a chance it floods out of him
        if (g.stand && !s.standDone && !plugged && speed > 30.0f && now - s.lastWorked < 120.0) {
            s.standDone = true;
            if (s.vol > 0.05f && s.floodLeft <= 0.0f && Rand(0.0f, 1.0f) < g.standChance * ByExp(g.floodTight, g.floodUsed, lvl)) {
                s.floodAt = now;
                s.floodLeft = s.vol * g.standDrain;
                s.floodRate = s.floodLeft / std::max(g.floodSeconds * 0.6f, 0.5f);
                s.floodStream = g_nextStream++;
                s.floodFree = Rand(0.0f, 1.0f) < g.standFree;
            }
        }
        if (s.ringLeft > 0.0f) {   // 2.6h: what is pumped into him fully in wells out round his rim as a string lying on the edge
            const float now_ = take(std::min(s.ringLeft, s.ringLeft * dt / 0.3f + 0.01f));
            s.ringLeft = s.vol <= 0.001f ? 0.0f : s.ringLeft - now_;
            s.ringParts += now_ * per;
            const RE::NiPoint3 up = out.Cross(side);
            const float hole = std::clamp(Engine::DuringOf(a->GetFormID()).hole, 0.0f, 1.0f);
            const float r = g.ringRadius * (0.8f + 0.4f * hole);
            // OStrands links consecutive particles of one stream that sit closer than ~0.8 units: points ~0.45 apart all
            // the way round make one closed string; a bigger load lays two particles per point (a thicker string)
            const int pts = std::clamp(static_cast<int>(6.2831853f * r / 0.45f), 12, 48);
            const bool last = s.ringLeft <= 0.0f;
            while (s.ringParts >= static_cast<float>(pts) || (last && s.ringParts >= 6.0f)) {
                const int thick = s.ringParts >= 2.5f * pts ? 2 : 1;
                const int use = std::min(static_cast<int>(s.ringParts), pts * thick);
                const int n = std::max(6, use / thick);
                s.ringStream = g_nextStream++;
                const float t0 = Rand(0.0f, 6.2831853f);
                for (int i = 0; i <= n; ++i) {   // one past the start, so the string closes on itself
                    const float t = t0 + 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
                    const RE::NiPoint3 radial = side * std::cos(t) + up * std::sin(t);
                    const float swell = Rand(0.9f, 1.1f);
                    Out(a, s.ringStream, c + radial * (r * swell) + out * 0.35f, radial * Rand(6.0f, 10.0f) + out * Rand(4.0f, 8.0f),
                        thick, PickRgb(s));
                }
                s.ringParts -= static_cast<float>(use);
                if (last) { s.ringParts = 0.0f; break; }
            }
            if (last) s.ringParts = 0.0f;
        }
        if (s.floodLeft > 0.0f) {   // a steady pour from his lip, down and a little out, as one stream
            if (g.wetTrail) Wet(a, 0.8f);
            const float want = std::min(s.floodLeft, s.floodRate * dt * Rand(0.8f, 1.2f));
            const float now_ = s.floodFree ? want : take(want);   // 2.6e: a free flood pours, but he stays as full
            s.floodLeft = (!s.floodFree && s.vol <= 0.001f) ? 0.0f : s.floodLeft - now_;
            const int n = static_cast<int>(std::lround(now_ * per + s.acc));
            s.acc = now_ * per + s.acc - static_cast<float>(n);
            Out(a, s.floodStream, c + out * 0.3f + down * 0.15f, out * Rand(2.5f, 4.5f) + down * 2.5f + side * Rand(-0.4f, 0.4f), n, PickRgb(s));
            return;
        }
        // squirt (a clench): fast, along the canal's axis
        if (s.squirtLeft > 0.0f) {
            const float now_ = take(std::min(s.squirtLeft, s.squirtLeft * dt / 0.25f + 0.02f));
            s.squirtLeft -= now_;
            const int n = static_cast<int>(std::lround(now_ * per));
            const RE::NiPoint3 v = (out * 0.9f + down * 0.25f + side * Rand(-0.08f, 0.08f)) * s.squirtSpeed;
            Out(a, s.squirtStream, c + out * 0.3f, v, n, PickRgb(s));
        }
        // gush: the moment he is empty of the schlong
        if (s.gushLeft > 0.0f) {
            const float now_ = take(std::min(s.gushLeft, s.gushLeft * dt / 0.6f + 0.02f));
            s.gushLeft -= now_;
            const int n = static_cast<int>(std::lround(now_ * per));
            const RE::NiPoint3 v = out * Rand(8.0f, 14.0f) + down * 4.0f + side * Rand(-1.5f, 1.5f);
            Out(a, s.stream, c + out * 0.3f, v, n, PickRgb(s));
            return;
        }
        if (s.vol <= 0.001f) return;
        if (g.walk && !plugged && speed > 30.0f) {   // 2.6e: it drips out of him as he walks (more running), down his thighs
            s.walkAcc += g.walkRate / 60.0f * (speed > 250.0f ? 2.0f : 1.0f) * std::min(s.vol, 3.0f) * per * dt * grav;
            const int n = static_cast<int>(s.walkAcc);
            if (n > 0) {
                s.walkAcc -= static_cast<float>(n);
                if (Rand(0.0f, 1.0f) >= g.walkFree) take(static_cast<float>(n) / per);
                s.leakAt = now;
                if (g.wetTrail) Wet(a, 0.5f);
                Out(a, g_nextStream++, low + side * Rand(-0.8f, 0.8f), down * 2.0f + out * 0.5f, n, PickRgb(s));
            }
        }
        if (plugged) {   // still being taken: it seeps out round the shaft
            if (!g.seep) return;
            s.acc += s.vol * 0.02f * per * dt;
            const int n = static_cast<int>(s.acc);
            if (n <= 0) return;
            s.acc -= static_cast<float>(n);
            take(static_cast<float>(n) / per);
            const float t = Rand(0.0f, 6.2831853f);
            const RE::NiPoint3 radial = side * std::cos(t) + out.Cross(side) * std::sin(t);
            Out(a, g_nextStream++, c + radial * g.ringRadius + out * 0.35f, radial * 4.0f + out * 3.0f + down * 2.0f, n, PickRgb(s));   // 2.6h: at his rim, outside the shaft
            return;
        }
        // open: it oozes out, faster the wider he gapes (2.6e: not at all while he is closed, if he holds it in)
        if (g.holdIn && gape < 0.15f) return;
        const float rate = 1.0f / std::max(g.leakMinutes * 60.0f, 10.0f) * (0.5f + 2.0f * gape) * (0.4f + 0.6f * std::min(s.vol, 2.0f)) * grav *
                           Training::ColdHold(a->GetFormID());   // 2.6j: a Vampire Lord's cold seed stays in him longer
        s.acc += rate * per * dt;
        const int n = static_cast<int>(s.acc);
        if (n <= 0) return;
        s.acc -= static_cast<float>(n);
        take(static_cast<float>(n) / per);
        if (g.innerStrings && s.vol > 0.05f && Rand(0.0f, 1.0f) < g.innerChance * 0.2f) {
            // 2.6i: a string eases out from inside his canal: a linked rope that starts deep and slides out over the rim
            const int k = static_cast<int>(Rand(6.0f, 11.0f));
            take(static_cast<float>(k) / per);
            const std::uint32_t rs = g_nextStream++;
            RE::NiPoint3 in = d - c;
            if (in.Unitize() < 1e-3f) in = out * -1.0f;
            const float len = 0.38f * static_cast<float>(k);
            for (int i = 0; i < k; ++i) {
                const float t = 1.0f - static_cast<float>(i) / static_cast<float>(k - 1);   // deepest first, so it follows the string out
                const RE::NiPoint3 p = c + in * (len * t * 0.55f) + out * 0.15f;
                Out(a, rs, p, out * Rand(1.5f, 2.6f) + down * 0.6f, 1, PickRgb(s));
            }
            return;
        }
        if (Rand(0.0f, 1.0f) < 0.05f) s.stream = g_nextStream++;
        Out(a, s.stream, low, out * Rand(1.0f, 3.0f) + down * 2.0f, n, PickRgb(s));
    }

    void Tick() {
        Resolve();
        if (!g_api.emit || !g.on) return;
        static double last = 0.0;
        const double now = Now();
        const float dt = static_cast<float>(std::clamp(now - (last > 0.0 ? last : now - 0.1), 0.0, 0.5));
        last = now;
        if (g_api.semenOn && !g_api.semenOn()) return;
        std::vector<std::pair<RE::Actor*, float>> wets;
        struct Ovl { RE::Actor* a; int level; int col; float alpha; };
        std::vector<Ovl> ovls;
        const float gh = GameHours();
        {
        std::lock_guard lk(g_mx);
        g_wets = &wets;
        for (auto it = g_st.begin(); it != g_st.end();) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(it->first);
            State& s = it->second;
            if (s.giver && now - s.climaxLast > 3.0) { g_filled.push_back({it->first, s.giver, s.climax, false}); s.giver = 0; s.climax = 0.0f; }
            // 2.6i: cum on his skin - film while he holds it, dripping when full or running out; fades after he is empty
            int level = 0; float alpha = 0.0f;
            if (g.overlay && a) {
                const bool running = now - s.floodAt < 30.0 || now - s.leakAt < 10.0;
                if (s.vol > 0.05f || running) {
                    level = (s.vol >= 1.5f || (running && s.vol > 0.3f)) ? 2 : 1;
                    alpha = 0.55f + 0.15f * std::min(s.vol, 3.0f);
                } else if (s.emptyAt >= 0.0f && gh - s.emptyAt < g.overlayHours) {
                    level = 1;
                    alpha = 0.65f * (1.0f - (gh - s.emptyAt) / std::max(g.overlayHours, 0.1f)) + 0.1f;
                }
                const int col = (static_cast<int>(std::clamp(s.rgb[0], 0.0f, 1.0f) * 255.0f) << 16) |
                                (static_cast<int>(std::clamp(s.rgb[1], 0.0f, 1.0f) * 255.0f) << 8) | static_cast<int>(std::clamp(s.rgb[2], 0.0f, 1.0f) * 255.0f);
                // (2.6j: and again every few seconds while it shows - a rebuilt body loses a paint put straight on the 3D)
                if (level != s.ovlSent || std::abs(alpha - s.ovlAlphaSent) > 0.04f || col != s.ovlColSent || (level > 0 && now - s.ovlAt > 5.0)) {
                    if (a->Is3DLoaded()) {
                        ovls.push_back({a, level, col, alpha});
                        s.ovlSent = level; s.ovlAlphaSent = alpha; s.ovlColSent = col; s.ovlAt = now;
                    }
                }
            } else if (a && s.ovlSent) {
                ovls.push_back({a, 0, 0, 0.0f});
                s.ovlSent = 0;
            }
            const bool done = s.vol <= 0.001f && s.gushLeft <= 0.0f && s.squirtLeft <= 0.0f && s.floodLeft <= 0.0f && s.ringLeft <= 0.0f && !s.giver &&
                              s.residue <= 0.01f && s.ovlSent == 0;
            if (!a || a->IsDead() || (done && now - s.lastWorked > 5.0)) { it = g_st.erase(it); continue; }
            if (s.stuffed && s.vol < g.stuffedAt * 0.5f) s.stuffed = false;
            if (a->Is3DLoaded()) StepOne(a, s, dt, now);
            ++it;
        }
        g_wets = nullptr;
        }
        for (auto& [a, w] : wets) Colour::AddWet(a, w);   // (outside our lock: Colour has its own)
        for (auto& o : ovls) Engine::CumOverlay(o.a, o.level, o.col, o.alpha);
    }

    void Loop() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            {
                std::lock_guard lk(g_mx);
                if (g_st.empty()) continue;
            }
            auto* ui = RE::UI::GetSingleton();
            if (!ui || ui->GameIsPaused()) continue;
            if (g_queued.exchange(true)) continue;
            if (auto* t = SKSE::GetTaskInterface()) t->AddTask([]() { g_queued = false; Tick(); });
            else g_queued = false;
        }
    }

    void Help(const char* t) {
        ImGuiMCP::SameLine();
        Tr::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::PushTextWrapPos(420.0f); Tr::TextUnformatted(t); ImGuiMCP::PopTextWrapPos(); ImGuiMCP::EndTooltip(); }
    }

    // what OStrands put in him: loads (1 = one full load), its colour (0-1, null = default)
    void SemenIn(RE::Actor* recv, RE::Actor* giver, float loads, const float* rgb) {
        if (!recv || loads <= 0.0f || !Male(recv)) return;
        (void)giver;
        std::lock_guard lk(g_mx);
        if (!g.on) return;
        auto& s = g_st[recv->GetFormID()];
        const double now = Now();
        // 2.6f: who is filling him - one climax = loads arriving with less than 3 s between them
        if (giver && (s.giver != giver->GetFormID() || now - s.climaxLast > 3.0)) Colossal::Climax(giver);   // 2.6j: a new finish (a werewolf howls)
        if (s.giver && (s.giver != (giver ? giver->GetFormID() : 0) || now - s.climaxLast > 3.0)) {
            g_filled.push_back({recv->GetFormID(), s.giver, s.climax, false});
            s.climax = 0.0f;
        }
        s.giver = giver ? giver->GetFormID() : 0;
        s.washed = false;   // 2.6i
        s.climax += loads;
        s.climaxLast = now;
        // 2.6f: the ring - pumped in while fully in, some squirts straight back out all round the shaft
        if (g.ring && s.lastWorked > 0.0 && now - s.lastWorked < 1.5) {
            const Engine::During d = Engine::DuringOf(recv->GetFormID());
            if (!g.ringFullOnly || d.depth >= g.ringDepth) {
                if (s.ringLeft <= 0.0f) s.ringStream = g_nextStream, g_nextStream += 8;
                s.ringLeft += loads * g.ringShare * Colossal::RingBoost(giver);   // 2.6i: a werewolf's wells out far more
            }
        }
        const float cap = g.maxLoads * ByExp(g.holdTight, g.holdUsed, Lvl(recv->GetFormID()));   // 2.6f: a well-used man holds more
        const float room = std::max(cap - s.vol, 0.0f);
        const float kept = std::min(loads, room);
        if (rgb) {
            const float w0 = std::max(s.vol, 0.0f), w1 = std::max(kept, 0.001f);
            for (int i = 0; i < 3; ++i) s.rgb[i] = s.vol <= 0.001f ? rgb[i] : (s.rgb[i] * w0 + rgb[i] * w1) / (w0 + w1);
        }
        {   // 2.6g: his own pool, in his colour
            const RE::FormID gid = giver ? giver->GetFormID() : 0;
            auto it = std::find_if(s.pools.begin(), s.pools.end(), [&](auto& p) { return p.giver == gid; });
            if (it == s.pools.end()) {
                if (s.pools.size() >= 4) {   // the smallest makes room
                    auto sm = std::min_element(s.pools.begin(), s.pools.end(), [](auto& x, auto& y) { return x.vol < y.vol; });
                    s.pools.erase(sm);
                }
                State::Pool np; np.giver = gid;
                for (int i = 0; i < 3; ++i) np.rgb[i] = rgb ? rgb[i] : s.rgb[i];
                if (giver) Colour::CumGlow(giver, np.glow);   // 2.6i
                s.pools.push_back(np);
                it = s.pools.end() - 1;
            }
            it->vol += kept;
        }
        s.vol += kept;
        s.lastIn = now;
        if (kept > 0.0f) {   // 2.6j: inflation
            float cw = 0.0f; Colossal::Load cl;
            Training::Stuffed(recv->GetFormID(), kept, giver && Colossal::For(giver, recv, cw, cl) && cw > 0.0f);
        }
        if (giver) {   // 2.6j: a Vampire Lord's - the thrall count, his cold seed; his hunger is fed
            Training::Bred(recv->GetFormID(), giver);
            Colossal::Finished(giver);
        }
        if (loads > kept) {   // full: the rest comes straight back out - 2.6g: squirting out round the shaft while he is still being filled
            if (s.lastWorked > 0.0 && now - s.lastWorked < 1.5) {
                if (s.ringLeft <= 0.0f) s.ringStream = g_nextStream, g_nextStream += 8;
                s.ringLeft += loads - kept;
            } else s.gushLeft += loads - kept;
        }
        if (!s.stuffed && s.vol >= g.stuffedAt) { s.stuffed = true; g_stuffedEv.push_back(recv->GetFormID()); }
    }
}

namespace OStrandsLink {
    void Install() { std::thread(Loop).detach(); }
    void Reset() { std::lock_guard lk(g_mx); g_st.clear(); }
    bool Installed() { Resolve(); return g_api.emit != nullptr; }

    void OnWorked(RE::Actor* recv, RE::Actor* partner, float dil) {
        (void)partner; (void)dil;
        if (!recv) return;
        std::lock_guard lk(g_mx);
        auto it = g_st.find(recv->GetFormID());
        if (it != g_st.end()) it->second.lastWorked = Now();
        else if (Installed() && g.on) g_st[recv->GetFormID()].lastWorked = Now();   // so the first load knows he is plugged
    }

    void OnClench(RE::FormID id, float amp) {
        std::lock_guard lk(g_mx);
        if (!g.on || !g.clench) return;
        auto it = g_st.find(id);
        if (it == g_st.end() || it->second.vol <= 0.02f || it->second.plugged) return;
        State& s = it->second;
        const float e = ByExp(g.squirtTight, g.squirtUsed, Lvl(id));   // 2.6f: a well-used man squirts harder
        s.squirtLeft += s.vol * std::min(g.clenchShare * e, 0.9f) * std::clamp(amp, 0.2f, 1.5f);
        s.squirtSpeed = (12.0f + 18.0f * std::clamp(amp, 0.0f, 1.5f)) * e;
        s.squirtStream = g_nextStream++;
    }

    // 2.6d: the Testing page - see it come out without a schlong in the way
    void RenderTests(RE::Actor* a) {
        static std::string last;
        if (!a) return;
        if (!Installed()) { Tr::TextDisabled("OStrands 0.8.0.3+ is not installed."); return; }
        if (!Male(a)) { Tr::TextDisabled("Only men (Anatomica's anus)."); return; }
        RE::NiPoint3 hc, hd;
        if (!Fluid::HoleFrame(a, hc, hd)) { Tr::TextDisabled("No Anatomica rim found on him."); return; }
        bool on;
        { std::lock_guard lk(g_mx); on = g.on; }
        if (!on) Tr::TextColored(ImGuiMCP::ImVec4{1.0f, 0.5f, 0.4f, 1.0f}, "\"Take the cum and let it out of him\" is off (Integrations page, OStrands).");
        auto act = [&](const char* label, auto&& fn) {
            if (Tr::Button(label)) { std::lock_guard lk(g_mx); State& s = g_st[a->GetFormID()]; s.lastWorked = 0.0; s.plugged = false; last = Lang::TS(fn(s)); }
        };
        act("Fill him: 1 load", [&](State& s) { s.vol = std::min(s.vol + 1.0f, g.maxLoads); return std::string("1 load in him"); });
        ImGuiMCP::SameLine();
        act("Fill him: 3 loads", [&](State& s) { s.vol = std::min(s.vol + 3.0f, g.maxLoads); return std::string("3 loads in him"); });
        ImGuiMCP::SameLine();
        act("Gush now", [&](State& s) {
            if (s.vol <= 0.02f) return std::string("he holds nothing - fill him first");
            s.gushLeft += s.vol * g.gush; s.stream = g_nextStream++;
            return std::string("a gush, as when the schlong slides out");
        });
        ImGuiMCP::SameLine();
        act("Squirt now", [&](State& s) {
            if (s.vol <= 0.02f) return std::string("he holds nothing - fill him first");
            s.squirtLeft += s.vol * g.clenchShare; s.squirtSpeed = 30.0f; s.squirtStream = g_nextStream++;
            return std::string("a squirt, as from a full clench");
        });
        ImGuiMCP::SameLine();
        act("Ring round his rim", [&](State& s) {
            if (s.vol <= 0.02f) return std::string("he holds nothing - fill him first");
            if (s.ringLeft <= 0.0f) s.ringStream = g_nextStream, g_nextStream += 8;
            s.ringLeft += std::min(s.vol, 0.5f);
            return std::string("a string of cum round his rim, as when he is filled fully in");
        });
        ImGuiMCP::SameLine();
        act("Flood now", [&](State& s) {
            if (s.vol <= 0.02f) return std::string("he holds nothing - fill him first");
            s.floodLeft = s.vol * g.floodDrain; s.floodRate = s.floodLeft / std::max(g.floodSeconds, 0.5f); s.floodStream = g_nextStream++;
            s.floodFree = Rand(0.0f, 1.0f) < g.floodFree;
            return std::string("a flood: a steady stream out of him");
        });
        ImGuiMCP::SameLine();
        act("Empty him", [&](State& s) { s.vol = s.gushLeft = s.squirtLeft = s.floodLeft = s.ringLeft = 0.0f; s.pools.clear(); return std::string("nothing in him"); });
        Tr::TextDisabled("Left alone he oozes it out over time (faster when gaping: Gape and movement > Gape him open). Close the menu to see it.");
        {
            std::lock_guard lk(g_mx);
            auto it = g_st.find(a->GetFormID());
            Tr::TextDisabled("%s", Lang::Fmt("In him: {:.2f} loads", it == g_st.end() ? 0.0f : it->second.vol).c_str());
        }
        if (!last.empty()) Tr::TextDisabled("%s", last.c_str());
    }

    // 2.6k: Testing page - a facial: spurts from just in front of his face, at it, as a finish pulled out of his mouth
    // kind 0 a normal load, 1 a Colossal Load, 2 a light one (a few strings), 3 in a giver's colour (the one picked)
    std::string TestFacial(RE::Actor* a, int kind, RE::Actor* giver) {
        if (!a) return "nobody";
        if (!Installed()) return "OStrands 0.8.0.3+ is not installed";
        auto* root = a->Get3D(false);
        auto* head = root ? root->GetObjectByName(RE::BSFixedString("NPC Head [Head]")) : nullptr;
        auto* mg = root ? root->GetObjectByName(RE::BSFixedString("NPC Head MagicNode [Hmag]")) : nullptr;
        if (!head || !mg) return "no head found";
        RE::NiPoint3 fwd = mg->world.translate - head->world.translate;
        const float len = fwd.Length();
        if (len < 0.01f) return "no head found";
        fwd = fwd / len;
        const RE::NiPoint3 up = head->world.rotate * RE::NiPoint3{0.0f, 0.0f, 1.0f};
        const RE::NiPoint3 side = fwd.Cross(up);
        const RE::NiPoint3 mouth = head->world.translate + (mg->world.translate - head->world.translate) * 0.7f;
        float rgb[3] = {0.925f, 0.914f, 0.871f};
        if (kind == 3 && giver) Colour::CumColour(giver, rgb);
        const int spurts = kind == 1 ? 7 : kind == 2 ? 3 : 5;
        const int each = kind == 1 ? 34 : kind == 2 ? 8 : 18;
        for (int i = 0; i < spurts; ++i) {   // fanned a little: some across his lips, some higher on his cheek and nose
            const float sx = Rand(-1.0f, 1.0f), sy = Rand(-0.3f, 1.0f);
            const RE::NiPoint3 from = mouth + fwd * 9.0f + side * (sx * 1.5f) + up * (sy * 1.5f - 1.0f);
            const RE::NiPoint3 to = mouth + side * (sx * 2.2f) + up * (sy * 2.5f);
            RE::NiPoint3 dir = to - from;
            dir = dir / std::max(dir.Length(), 0.01f);
            Out(a, g_nextStream++, from, dir * (kind == 1 ? 3.2f : 2.6f) + up * 0.4f, each, rgb);
        }
        Face::OnFace(a, kind == 3 ? giver : nullptr, kind == 1 ? 2.0f : kind == 2 ? 0.3f : 1.0f);   // the cum layer on his face
        return kind == 1 ? "a Colossal facial" : kind == 2 ? "a light facial" : kind == 3 ? "a facial in that man's colour" : "a facial";
    }

    // 2.6j: precum from OStrands while he is taken - a little more in him, and it wets him (lube)
    void PrecumIn(RE::Actor* recv, RE::Actor* giver, float loads) {
        if (!recv || loads <= 0.0f || !Male(recv)) return;
        (void)giver;
        {
            std::lock_guard lk(g_mx);
            if (!g.on) return;
            auto& s = g_st[recv->GetFormID()];
            s.vol = std::min(s.vol + loads, g.maxLoads * 1.5f);
            s.washed = false;
        }
        Colour::AddWet(recv, 0.15f);   // (outside our lock: Colour has its own)
    }
    void Bathed(RE::FormID id) {
        std::lock_guard lk(g_mx);
        if (!g.on || !g.bath) return;
        auto it = g_st.find(id);
        if (it == g_st.end()) return;
        it->second.washed = true;
        if (it->second.vol <= 0.05f || it->second.plugWorn) { it->second.residue = 0.0f; it->second.emptyAt = -100.0f; return; }
        State& s = it->second;
        s.residue = 0.0f;       // 2.6i: washed out: nothing hidden left, and the cum on his skin goes
        s.emptyAt = -100.0f;
        s.floodLeft = s.vol;
        s.floodRate = s.floodLeft / std::max(g.floodSeconds * 2.5f, 3.0f);
        s.floodStream = g_nextStream++;
        s.floodFree = false;
        s.floodAt = Now();
    }
    std::vector<RE::FormID> TakeStuffed() {
        std::lock_guard lk(g_mx);
        std::vector<RE::FormID> out;
        out.swap(g_stuffedEv);
        return out;
    }

    Held HeldOf(RE::FormID id) {
        std::lock_guard lk(g_mx);
        Held h;
        auto it = g_st.find(id);
        if (it == g_st.end()) return h;
        const double now = Now();
        h.vol = it->second.vol;
        h.flooding = it->second.floodLeft > 0.0f;
        h.floodAt = it->second.floodAt;
        h.leaking = now - it->second.leakAt < 4.0 || h.flooding;
        h.filling = it->second.giver != 0;
        h.plugged = it->second.plugWorn;
        h.giver = it->second.giver;
        h.stuffed = it->second.stuffed || it->second.vol >= g.maxLoads * 0.95f;
        h.most = g.maxLoads;
        return h;
    }
    int GiverCount(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_st.find(id);
        if (it == g_st.end()) return 0;
        int n = 0;
        for (auto& p : it->second.pools) if (p.vol > 0.05f) ++n;
        return n;
    }
    std::vector<Filled> TakeFilled() {
        std::lock_guard lk(g_mx);
        std::vector<Filled> out;
        out.swap(g_filled);
        for (auto& f : out) {   // was it a Colossal Load?
            float w = 0.0f; Colossal::Load l;
            auto* gv = RE::TESForm::LookupByID<RE::Actor>(f.giver);
            if (gv && Colossal::For(gv, nullptr, w, l)) f.colossal = w > 0.0f;
        }
        return out;
    }

    float Belly(RE::FormID id) {
        std::lock_guard lk(g_mx);
        if (!g.on || !g.belly) return 0.0f;
        auto it = g_st.find(id);
        if (it == g_st.end() || it->second.vol <= 0.0f) return g.bellyMax * Training::Lasting(id);   // 2.6j: a lasting belly stays when he is empty
        const float f = std::clamp(it->second.vol * Training::Faster(id) / std::max(g.bellyFull, 0.5f), 0.0f, 1.0f);   // 2.6j: faster growth
        return g.bellyMax * (f * (2.0f - f) + Training::Lasting(id));   // rounds out fast at first, then more slowly (+ 2.6j lasting)
    }

    std::string Status(RE::Actor* a) {
        if (!a) return {};
        std::lock_guard lk(g_mx);
        auto it = g_st.find(a->GetFormID());
        if (it == g_st.end() || it->second.vol <= 0.001f) return {};
        return Lang::Fmt(it->second.vol >= 0.995f && it->second.vol < 1.005f ? "OStrands cum in him: {:.2f} load" : "OStrands cum in him: {:.2f} loads", it->second.vol);
    }

    bool RenderSection() {
        const bool inst = Installed();
        if (!Tr::CollapsingHeader(inst ? "OStrands###ostrands" : "OStrands  (not installed)###ostrands")) return false;
        bool ch = false;
        std::lock_guard lk(g_mx);
        if (!inst) {
            Tr::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.4f, 1.0f}, "%s is not installed - these settings do nothing until it is (they are kept).", "OStrands 0.8.0.3+");
            ImGuiMCP::BeginDisabled();
        }
        Tr::TextWrapped("OStrands draws the cum. It asks Anatomica where his hole really is (gape, wink and PPA's pull included), "
                        "and hands over what goes into him; Anatomica lets it back out through OStrands' fluid.");
        ch |= Tr::Checkbox("Take the cum and let it out of him", &g.on);
        Help("Off: OStrands keeps what goes in and leaks it itself (no gush on pull-out, no squirt on a clench).");
        if (g.on) {
            ImGuiMCP::Indent();
            ch |= Tr::SliderFloat("A full load drains in", &g.leakMinutes, 0.5f, 120.0f, "%.1f min");
            Help("While he is open. The wider he gapes, the faster it runs out.");
            ch |= Tr::Checkbox("He holds it in until he gapes", &g.holdIn);
            Help("No slow ooze while he is closed: it only comes out when he gapes, clenches, floods or a schlong slides out.");
            ch |= Tr::Checkbox("Flood when he gapes", &g.flood);
            Help("Each time he opens up past the gape below, there is a chance it pours out of him in one steady stream "
                 "instead of a trickle.");
            if (g.flood) {
                ImGuiMCP::Indent();
                float fc = g.floodChance * 100.0f, fd = g.floodDrain * 100.0f, fg = g.floodGape * 100.0f, ff = g.floodFree * 100.0f;
                if (Tr::SliderFloat("Chance a flood costs nothing", &ff, 0.0f, 100.0f, "%.0f%%")) { g.floodFree = ff / 100.0f; ch = true; }
                Help("It pours out of him all the same, but he stays as full as he was.");
                if (Tr::SliderFloat("Chance each time he gapes", &fc, 0.0f, 100.0f, "%.0f%%")) { g.floodChance = fc / 100.0f; ch = true; }
                if (Tr::SliderFloat("How much a flood lets out", &fd, 5.0f, 100.0f, "%.0f%% of what he holds")) { g.floodDrain = fd / 100.0f; ch = true; }
                ch |= Tr::SliderFloat("Flood lasts", &g.floodSeconds, 1.0f, 30.0f, "%.0f s");
                if (Tr::SliderFloat("Starts once he gapes", &fg, 5.0f, 100.0f, "%.0f%%")) { g.floodGape = fg / 100.0f; ch = true; }
                Help("How open he has to be (of his full gape). After sex the gape is set on the Gape page.");
                ImGuiMCP::Unindent();
            }
            float pct = g.gush * 100.0f;
            if (Tr::SliderFloat("Gush as the schlong slides out", &pct, 0.0f, 100.0f, "%.0f%%")) { g.gush = pct / 100.0f; ch = true; }
            ch |= Tr::Checkbox("Seeps out round the shaft while he is taken", &g.seep);
            ch |= Tr::Checkbox("A clench squirts some out", &g.clench);
            if (g.clench) {
                float c = g.clenchShare * 100.0f;
                if (Tr::SliderFloat("Squirted per clench", &c, 1.0f, 60.0f, "%.0f%%")) { g.clenchShare = c / 100.0f; ch = true; }
            }
            ch |= Tr::Checkbox("Flood when he gets up after sex", &g.stand);
            Help("When he starts walking within two minutes of being taken, a chance it floods out of him.");
            if (g.stand) {
                ImGuiMCP::Indent();
                float sc = g.standChance * 100.0f, sd = g.standDrain * 100.0f, sf = g.standFree * 100.0f;
                if (Tr::SliderFloat("Chance when he gets up", &sc, 0.0f, 100.0f, "%.0f%%")) { g.standChance = sc / 100.0f; ch = true; }
                if (Tr::SliderFloat("How much it lets out##st", &sd, 5.0f, 100.0f, "%.0f%% of what he holds")) { g.standDrain = sd / 100.0f; ch = true; }
                if (Tr::SliderFloat("Chance it costs nothing##st", &sf, 0.0f, 100.0f, "%.0f%%")) { g.standFree = sf / 100.0f; ch = true; }
                ImGuiMCP::Unindent();
            }
            ch |= Tr::Checkbox("Drips out of him as he walks", &g.walk);
            Help("While he holds some and walks (twice as much running), drops run out of him and down his thighs.");
            if (g.walk) {
                ImGuiMCP::Indent();
                float wf = g.walkFree * 100.0f;
                ch |= Tr::SliderFloat("How much while walking", &g.walkRate, 0.01f, 1.0f, "%.2f loads a minute");
                if (Tr::SliderFloat("Chance a drip costs nothing", &wf, 0.0f, 100.0f, "%.0f%%")) { g.walkFree = wf / 100.0f; ch = true; }
                ImGuiMCP::Unindent();
            }
            ch |= Tr::Checkbox("His belly rounds out with what he holds", &g.belly);
            Help("Uses the Stomach shape (Anatomica Bulge Belly; the body built with Build Morphs). It goes down again as it comes out.");
            if (g.belly) {
                ImGuiMCP::Indent();
                float bm = g.bellyMax * 100.0f;
                if (Tr::SliderFloat("Belly when full", &bm, 10.0f, 200.0f, "%.0f%%")) { g.bellyMax = bm / 100.0f; ch = true; }
                ch |= Tr::SliderFloat("Full at", &g.bellyFull, 1.0f, 10.0f, "%.1f loads");
                Training::RenderInflation();   // 2.6j
                ImGuiMCP::Unindent();
            }
            ch |= Tr::SliderFloat("Most he holds", &g.maxLoads, 1.0f, 10.0f, "%.1f loads");
            Help("More than this comes straight back out.");
            ch |= Tr::Checkbox("Wells out round his rim while he is filled", &g.ring);
            Help("While a load is pumped into him, some wells straight back out and lies round the edge of his hole as a string of cum, as when he is too full to take it all.");
            if (g.ring) {
                ImGuiMCP::Indent();
                float rs = g.ringShare * 100.0f;
                if (Tr::SliderFloat("How much wells back out", &rs, 5.0f, 80.0f, "%.0f%% of each load")) { g.ringShare = rs / 100.0f; ch = true; }
                ch |= Tr::Checkbox("Only when he is fully in", &g.ringFullOnly);
                if (g.ringFullOnly) ch |= Tr::SliderFloat("Fully in from", &g.ringDepth, 2.0f, 30.0f, "%.0f units deep");
                ch |= Tr::SliderFloat("Size of the ring", &g.ringRadius, 0.8f, 4.0f, "%.1f units round his hole");
                Help("It wells out of him as a string of cum lying round the edge of his hole. Make it bigger if it sits inside "
                     "a thick schlong, smaller if it stands off his rim.");
                ImGuiMCP::Unindent();
            }
            ch |= Tr::Checkbox("A plug holds it in", &g.plug);
            Help("While he wears a plug (Devious Devices, or any worn item with an anal plug keyword) nothing comes out. Pulling it out floods.");
            if (g.plug) {
                float pd = g.plugDrain * 100.0f;
                ImGuiMCP::Indent();
                if (Tr::SliderFloat("Pulling it out lets out", &pd, 10.0f, 100.0f, "%.0f%% of what he holds")) { g.plugDrain = pd / 100.0f; ch = true; }
                ImGuiMCP::Unindent();
            }
            ch |= Tr::Checkbox("Some stays hidden in him", &g.late);   // 2.6i
            Help("After he seems empty, a little is still in him for a while: crouching, sitting or kneeling can squirt it out.");
            if (g.late) {
                ImGuiMCP::Indent();
                float ls = g.lateShare * 100.0f;
                if (Tr::SliderFloat("How much stays", &ls, 0.0f, 80.0f, "%.0f%% of the most he held")) { g.lateShare = ls / 100.0f; ch = true; }
                ch |= Tr::SliderFloat("For", &g.lateHours, 0.5f, 48.0f, "%.1f game hours");
                float lc = g.lateChance * 100.0f;
                if (Tr::SliderFloat("Chance when he crouches or sits", &lc, 0.0f, 100.0f, "%.0f%% (checked every few seconds)")) { g.lateChance = lc / 100.0f; ch = true; }
                ImGuiMCP::Unindent();
            }
            ch |= Tr::Checkbox("Strings ease out from inside him", &g.innerStrings);
            Help("While it oozes out of him, now and then a string of it slides out from inside his canal over the rim.");
            if (g.innerStrings) {
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(140.0f);
                float ic = g.innerChance * 100.0f;
                if (Tr::SliderFloat("##inner", &ic, 0.0f, 100.0f, "%.0f%% as often")) { g.innerChance = ic / 100.0f; ch = true; }
            }
            ch |= Tr::Checkbox("Cum shows on his skin", &g.overlay);
            Help("A RaceMenu body overlay round his anus while he holds cum, dripping when full or running out, fading after he is empty "
                 "(washing takes it off). Other mods that look for cum overlays (DDF, cum-overlay mods) see it. Uses one free body overlay slot.");
            if (g.overlay) ch |= Tr::SliderFloat("Stays after he is empty", &g.overlayHours, 0.1f, 48.0f, "%.1f game hours");
            ch |= Tr::Checkbox("It runs out by how he lies", &g.gravity);   // 2.6j
            Help("Bottom up it stays in him, standing it runs out, squatting it pours.");
            ch |= Tr::Checkbox("Thrusts squeeze it out", &g.squeeze);
            Help("While he is full and being taken, each deep thrust squeezes a little out round the shaft, and a new man going in pushes out more.");
            if (g.squeeze) {
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(140.0f);
                float sq = g.squeezeShare * 100.0f;
                if (Tr::SliderFloat("##sq", &sq, 0.0f, 20.0f, "%.0f%% a thrust")) { g.squeezeShare = sq / 100.0f; ch = true; }
            }
            ch |= Tr::Checkbox("Lying on his full belly pushes it out", &g.pressure);
            ch |= Tr::Checkbox("Changing form squeezes it out", &g.shift);
            Help("OStim Beast Form: turning into a werewolf, werebear or Vampire Lord (or back) while he holds cum pushes a gush out of him.");
            ch |= Tr::Checkbox("Washing lets it all out", &g.bath);
            Help("Bathing in Skyrim: when he washes, it all slowly runs out of him.");
            ch |= Tr::SliderFloat("Stuffed full at", &g.stuffedAt, 1.0f, 10.0f, "%.1f loads");
            Help("SkyrimNet: from this much he counts as stuffed full - it is counted (he and others remember how often), and watchers notice.");
            ch |= Tr::Checkbox("Leaves him wet", &g.wetTrail);
            Help("A flood or walking drips leave his rim and skin wet and glossy (After sex page, Wetness).");
            ch |= Tr::Checkbox("By his experience", &g.byExp);
            Help("A tight or virgin man holds less, squirts softer and floods less often; a well-used man holds more, squirts harder "
                 "and floods more. In between by how used he is (Virginity page).");
            if (g.byExp && ImGuiMCP::BeginTable("##exp", 3)) {
                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableNextColumn(); Tr::TextDisabled("");
                ImGuiMCP::TableNextColumn(); Tr::TextDisabled("Tight / virgin");
                ImGuiMCP::TableNextColumn(); Tr::TextDisabled("Well used");
                auto row = [&](const char* label, const char* id, float& t, float& u) {
                    ImGuiMCP::TableNextRow();
                    ImGuiMCP::TableNextColumn(); Tr::TextUnformatted(label);
                    ImGuiMCP::TableNextColumn(); ch |= ImGuiMCP::SliderFloat(std::format("##t{}", id).c_str(), &t, 0.2f, 3.0f, "x%.2f");
                    ImGuiMCP::TableNextColumn(); ch |= ImGuiMCP::SliderFloat(std::format("##u{}", id).c_str(), &u, 0.2f, 3.0f, "x%.2f");
                };
                row("How much he holds", "h", g.holdTight, g.holdUsed);
                row("How hard he squirts", "s", g.squirtTight, g.squirtUsed);
                row("How often he floods", "f", g.floodTight, g.floodUsed);
                ImGuiMCP::EndTable();
            }
            ch |= Tr::Checkbox("Nothing comes out while he is dressed", &g.dressed);
            ch |= Tr::SliderInt("OStrands drops per load", &g.perLoad, 40, 400, "%d");
            Help("How many of OStrands' fluid drops one load is. OStrands' own load is about 150-250.");
            ImGuiMCP::Unindent();
        }
        if (!inst) ImGuiMCP::EndDisabled();
        return ch;
    }

    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "OStrands";
        std::lock_guard lk(g_mx);
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto F = [&](const char* k, float& v, float lo, float hi) { if (save) ini.SetDoubleValue(S, k, v); else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi); };
        auto I = [&](const char* k, int& v, int lo, int hi) { if (save) ini.SetLongValue(S, k, v); else v = std::clamp(static_cast<int>(ini.GetLongValue(S, k, v)), lo, hi); };
        B("Link", g.on); F("LeakMinutes", g.leakMinutes, 0.5f, 120.0f); F("Gush", g.gush, 0.0f, 1.0f); B("Clench", g.clench);
        F("ClenchShare", g.clenchShare, 0.01f, 0.6f); B("Seep", g.seep); I("PerLoad", g.perLoad, 40, 400); F("MaxLoads", g.maxLoads, 1.0f, 10.0f);
        B("Dressed", g.dressed);
        B("Flood", g.flood); F("FloodChance", g.floodChance, 0.0f, 1.0f); F("FloodDrain", g.floodDrain, 0.05f, 1.0f);
        F("FloodSeconds", g.floodSeconds, 1.0f, 30.0f); F("FloodGape", g.floodGape, 0.05f, 1.0f); B("HoldIn", g.holdIn);
        F("FloodFree", g.floodFree, 0.0f, 1.0f); B("Belly", g.belly); F("BellyMax", g.bellyMax, 0.1f, 2.0f); F("BellyFull", g.bellyFull, 1.0f, 10.0f);
        B("Walk", g.walk); F("WalkRate", g.walkRate, 0.01f, 1.0f); F("WalkFree", g.walkFree, 0.0f, 1.0f);
        B("WetTrail", g.wetTrail); B("ByExp", g.byExp); F("HoldTight", g.holdTight, 0.2f, 3.0f); F("HoldUsed", g.holdUsed, 0.2f, 3.0f);
        F("SquirtTight", g.squirtTight, 0.2f, 3.0f); F("SquirtUsed", g.squirtUsed, 0.2f, 3.0f); F("FloodTight", g.floodTight, 0.2f, 3.0f); F("FloodUsed", g.floodUsed, 0.2f, 3.0f);
        B("Plug", g.plug); F("PlugDrain", g.plugDrain, 0.1f, 1.0f); B("Bath", g.bath); F("StuffedAt", g.stuffedAt, 1.0f, 10.0f);
        B("Late", g.late); F("LateShare", g.lateShare, 0.0f, 0.8f); F("LateHours", g.lateHours, 0.5f, 48.0f); F("LateChance", g.lateChance, 0.0f, 1.0f);
        B("InnerStrings", g.innerStrings); F("InnerChance", g.innerChance, 0.0f, 1.0f);
        B("Overlay", g.overlay); F("OverlayHours", g.overlayHours, 0.1f, 48.0f);
        B("Gravity", g.gravity); B("Squeeze", g.squeeze); F("SqueezeShare", g.squeezeShare, 0.0f, 0.2f); B("Pressure", g.pressure); B("ShiftGush", g.shift);
        B("Ring", g.ring); B("RingFullOnly", g.ringFullOnly); F("RingDepth", g.ringDepth, 2.0f, 30.0f); F("RingShare", g.ringShare, 0.05f, 0.8f);
        F("RingRadius", g.ringRadius, 0.8f, 4.0f);
        B("Stand", g.stand); F("StandChance", g.standChance, 0.0f, 1.0f); F("StandDrain", g.standDrain, 0.05f, 1.0f); F("StandFree", g.standFree, 0.0f, 1.0f);
    }
}

// ---- for OStrands (it looks these up in AnatomicaMenu.dll)
extern "C" {
    __declspec(dllexport) int Anatomica_ApiVersion() { return 1; }

    // where his hole really is: its centre and a point 4 units down his canal (world). false = no Anatomica rim on him
    __declspec(dllexport) bool Anatomica_HoleFrame(RE::Actor* a, float* centre, float* deep) {
        if (!a || !centre || !deep || !Male(a)) return false;
        RE::NiPoint3 c, d;
        if (!Fluid::HoleFrame(a, c, d)) return false;
        centre[0] = c.x; centre[1] = c.y; centre[2] = c.z;
        deep[0] = d.x; deep[1] = d.y; deep[2] = d.z;
        return true;
    }

    // the giver's cum colour from the Per race page (0-1). false = none set (only with Anatomica's cum on, Features.h)
    __declspec(dllexport) bool Anatomica_CumColour(RE::Actor* giver, float* rgb) {
        return giver && rgb && Colour::CumColour(giver, rgb);
    }

    // 2.6i: does this man's cum glow (vampires, daedra, races picked on the Cum colour (OStrands) section)? rgb 0-1 + strength
    __declspec(dllexport) bool Anatomica_CumGlow(RE::Actor* giver, float* out) {
        return giver && out && Colour::CumGlow(giver, out);
    }

    // true: give Anatomica what goes into this man's anus (it lets it out again through OStrands)
    __declspec(dllexport) bool Anatomica_TakesOver(RE::Actor* recv) {
        if (!recv || !Male(recv) || !OStrandsLink::Installed()) return false;
        {
            std::lock_guard lk(g_mx);
            if (!g.on) return false;
        }
        RE::NiPoint3 c, d;
        return Fluid::HoleFrame(recv, c, d);
    }

    // OStrands put this much in him (loads: 1 = one full load; rgb 0-1 or null)
    // 2.6j: OStrands 0.8.0.8+ - precum into his anus (he holds it, it wets him)
    __declspec(dllexport) void Anatomica_Precum(RE::Actor* recv, RE::Actor* giver, float loads) {
        OStrandsLink::PrecumIn(recv, giver, loads);
    }

    __declspec(dllexport) void Anatomica_SemenIn(RE::Actor* recv, RE::Actor* giver, float loads, const float* rgb) {
        SemenIn(recv, giver, loads, rgb);
    }
}

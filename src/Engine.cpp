#include "SKSEMenuFramework.h"
#include "During.h"
#include "Fluid.h"
#include "Extras.h"
#include "SoftButt.h"
#include "Engine.h"
#include "Virgin.h"
#include "Size.h"
#include "Bridge.h"
#include "Colour.h"
#include "Drip.h"
#include "SKEE.h"
#include "Wink.h"
#include "Maker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <array>
#include <format>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------------------------------------------
//  The gape engine (a port of the gape script's loop to C++, see make_gape2.py):
//   * penetration: another man's schlong bone inside this man's canal entrance (the canal is placed from bones that
//     are rigid with the pelvis - pelvis, both thigh joints, spine0 - measured on the Anatomica mesh)
//   * dilation builds while he is penetrated, holds after, then closes; more partners in a row = longer
//   * "Anatomica Gape" body morph written straight through RaceMenu's BodyMorph interface
//   * after the scene: tells the movement code (Wink) the gape and when to clench
//   * every few seconds: the anus colour / tint / swell / looseness / skin-blend ring through RaceMenu's Override
//     and Overlay interfaces (what the script's colour pass did)
//  ~10 checks a second for a handful of men: a few bone positions and some vector maths each - microseconds.
// ---------------------------------------------------------------------------------------------------------------
namespace {
    // the tight-follow slider sets (BodySlide: hole4.py = Normal, hole5.py = Fine / Super fine). Depths are mesh units along
    // his canal (scaled by his size at run time): where each ring / inside piece is measured on the schlong.
    struct HoleSet {
        int n = 4, rings = 1;
        std::vector<float> ringD, inD;
        std::vector<std::string> names;   // rings * n hole pieces, then the inside pieces
    };
    std::array<HoleSet, 3> MakeHoleSets() {
        std::array<HoleSet, 3> x;
        x[0].n = 4; x[0].rings = 1; x[0].ringD = {0.0f}; x[0].inD = {2.9f, 5.1f, 6.8f};
        x[0].names = {"Anatomica Hole Right", "Anatomica Hole Up", "Anatomica Hole Left", "Anatomica Hole Down",
                      "Anatomica Inside 1", "Anatomica Inside 2", "Anatomica Inside 3"};
        x[1].n = 8; x[1].rings = 2; x[1].ringD = {0.0f, 1.6f}; x[1].inD = {2.65f, 3.55f, 4.45f, 5.35f, 6.25f, 7.15f};
        x[2].n = 16; x[2].rings = 3; x[2].ringD = {0.0f, 1.25f, 2.15f};
        for (int i = 0; i < 10; ++i) x[2].inD.push_back(2.85f + 0.5f * static_cast<float>(i));
        for (int l = 1; l < 3; ++l) {
            const char* pre = l == 1 ? "Anatomica Fine" : "Anatomica XFine";
            for (int r = 0; r < x[l].rings; ++r)
                for (int q = 0; q < x[l].n; ++q) x[l].names.push_back(std::format("{} Hole {}{}", pre, "ABC"[r], q + 1));
            for (size_t j = 0; j < x[l].inD.size(); ++j) x[l].names.push_back(std::format("{} Inside {}", pre, j + 1));
        }
        return x;
    }
    const HoleSet& HoleSetOf(int lvl) {
        static const std::array<HoleSet, 3> sets = MakeHoleSets();
        return sets[std::clamp(lvl, 0, 2)];
    }
    using clk = std::chrono::steady_clock;
    double Now() { return std::chrono::duration<double>(clk::now().time_since_epoch()).count(); }
    std::mt19937 g_rng{std::random_device{}()};
    float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }

    SKEE::IBodyMorphInterface* g_morph = nullptr;
    SKEE::IOverrideInterface* g_ovr = nullptr;
    SKEE::IOverlayInterface* g_ovl = nullptr;
    std::uint32_t g_vMorph = 0, g_vOvr = 0, g_vOvl = 0;
    bool g_asked = false;

    constexpr const char* kEsp = "AnatomicaGape.esp";
    constexpr const char* K = "AnatomicaGape";   // the morph key everything of ours is stored under

    float Glob(RE::FormID id, float def) {
        if (auto* dh = RE::TESDataHandler::GetSingleton())
            if (auto* g = dh->LookupForm<RE::TESGlobal>(id, kEsp)) return g->value;
        return def;
    }

    struct V {
        float x = 0, y = 0, z = 0;
        V() = default;
        V(float a, float b, float c) : x(a), y(b), z(c) {}
        V(const RE::NiPoint3& p) : x(p.x), y(p.y), z(p.z) {}
        RE::NiPoint3 ni() const { return {x, y, z}; }
        V operator+(const V& o) const { return {x + o.x, y + o.y, z + o.z}; }
        V operator-(const V& o) const { return {x - o.x, y - o.y, z - o.z}; }
        V operator*(float s) const { return {x * s, y * s, z * s}; }
    };
    float Dot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    V Cross(const V& a, const V& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    float Len(const V& a) { return std::sqrt(Dot(a, a)); }

    RE::NiAVObject* Node(RE::NiAVObject* root, const char* n) { return root ? root->GetObjectByName(RE::BSFixedString(n)) : nullptr; }

    // the canal entrance of this man, from bones rigid with his pelvis
    struct Canal { bool ok = false; V h, a; float sc = 1.0f; V pelvis; V x, u, f; };
    Canal CanalOf(RE::Actor* act) {
        Canal c;
        auto* root = act->Get3D(false);
        auto* p = Node(root, "NPC Pelvis [Pelv]");
        auto* l = Node(root, "NPC L Thigh [LThg]");
        auto* r = Node(root, "NPC R Thigh [RThg]");
        auto* s = Node(root, "NPC Spine [Spn0]");
        if (!p || !l || !r || !s) return c;
        const V P = p->world.translate, L = l->world.translate, R = r->world.translate, S = s->world.translate;
        V x = R - L;
        const float lx = Len(x);
        if (lx < 1e-4f) return c;
        x = x * (1.0f / lx);
        c.sc = lx / 13.23f;
        V u = S - P;
        u = u - x * Dot(u, x);
        const float lu = Len(u);
        if (lu < 1e-4f) return c;
        u = u * (1.0f / lu);
        const V f = Cross(x, u);
        c.h = P + (u * 0.1604f + f * 7.4795f) * c.sc;
        c.a = u * -0.2664f + f * -0.9639f;
        c.pelvis = P;
        c.x = x; c.u = u; c.f = f;
        c.ok = true;
        return c;
    }
    // the same frame on the Anatomica mesh (bind pose, BodySlide units): pelvis and its axes - for the belly bulge
    const V kMeshP{0.0f, 0.0f, 68.9113f}, kMeshX{1.0f, 0.0f, 0.0f}, kMeshU{0.0f, -0.810145f, 0.586230f}, kMeshF{0.0f, -0.586230f, -0.810145f};
    V ToMesh(const Canal& c, const V& w) {
        const V r = (w - c.pelvis) * (1.0f / c.sc);
        return kMeshP + kMeshX * Dot(r, c.x) + kMeshU * Dot(r, c.u) + kMeshF * Dot(r, c.f);
    }
    // the belly bulge may show through what he wears (During sex page: 0 only bare, 1 clothes too, 2 armour too)
    bool BulgeShows(RE::Actor* a, int through) {
        if (through >= 2 || Extras::Exposed(a)) return true;
        auto* body = a->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody);
        if (!body) return true;
        return through >= 1 && body->IsClothing();
    }
    // how deep o's schlong is in him: the tip (past the last bone) measured along his canal axis, and where it is
    // the fit outline (During sex page, testing): drawn over the game by a HUD element
    struct FitDbg {
        std::vector<std::vector<RE::NiPoint3>> green, yellow;
        std::vector<RE::NiPoint3> red, blue;
        std::chrono::steady_clock::time_point at;
    };
    std::mutex g_dbgMx;
    std::unordered_map<RE::FormID, FitDbg> g_dbg;
    struct Tip { bool ok = false; float depth = 0.0f; V p; };
    Tip TipIn(const Canal& c, RE::Actor* o) {
        Tip t;
        auto* root = o ? o->Get3D(false) : nullptr;
        auto* g6 = Node(root, "NPC Genitals06 [Gen06]");
        auto* g5 = Node(root, "NPC Genitals05 [Gen05]");
        if (!g6 || !g5) return t;
        V d = V(g6->world.translate) - V(g5->world.translate);
        const float ld = Len(d);
        t.p = V(g6->world.translate) + (ld > 1e-4f ? d * (1.5f * c.sc / ld) : V{});
        t.depth = Dot(t.p - c.h, c.a) / c.sc;
        t.ok = true;
        return t;
    }
    // where o's schlong is: 2 = a bone in the first 8 units of the canal, within rad of its axis;
    // 1 = the tip still at the entrance (just outside it, PPA still spreading the ring round it); 0 = out
    // where o's shaft (the line of his schlong bones, out to the tip) crosses his canal at depth d (units along it)
    bool ShaftAt(const Canal& c, RE::Actor* o, float d, V& out) {
        auto* root = o ? o->Get3D(false) : nullptr;
        if (!root) return false;
        static const char* kGen[] = {"NPC Genitals01 [Gen01]", "NPC Genitals02 [Gen02]", "NPC Genitals03 [Gen03]",
                                     "NPC Genitals04 [Gen04]", "NPC Genitals05 [Gen05]", "NPC Genitals06 [Gen06]"};
        std::vector<V> pts;
        for (auto* nm : kGen) if (auto* n = Node(root, nm)) pts.push_back(V(n->world.translate));
        if (pts.size() < 2) return false;
        const Tip t = TipIn(c, o);
        if (t.ok) pts.push_back(t.p);
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            const float ta = Dot(pts[k] - c.h, c.a) / c.sc, tb = Dot(pts[k + 1] - c.h, c.a) / c.sc;
            if ((ta - d) * (tb - d) <= 0.0f && std::abs(tb - ta) > 1e-4f) {
                out = pts[k] + (pts[k + 1] - pts[k]) * ((d - ta) / (tb - ta));
                return true;
            }
        }
        return false;
    }
    int Inside(const Canal& c, RE::Actor* o, float rad) {
        auto* root = o->Get3D(false);
        if (!root) return 0;
        static const char* kGen[] = {"NPC Genitals06 [Gen06]", "NPC Genitals05 [Gen05]", "NPC Genitals04 [Gen04]",
                                     "NPC Genitals03 [Gen03]", "NPC Genitals02 [Gen02]", "NPC Genitals01 [Gen01]"};
        const float r = rad * c.sc, tol = -0.5f * r, r2 = r * r, tmax = 8.0f * c.sc;
        const float nearT = -3.0f * c.sc, nr2 = r2 * 2.6f;   // ~1.6x the radius, up to 3 units in front of the hole
        int best = 0;
        for (auto* nm : kGen) {
            auto* n = Node(root, nm);
            if (!n) continue;
            const V v = V(n->world.translate) - c.h;
            const float t = Dot(v, c.a);
            if (t > tmax) continue;
            const float d2 = Dot(v, v) - t * t;
            if (t >= tol && d2 < r2) return 2;
            if (t >= nearT && d2 < nr2) best = 1;
        }
        return best;
    }

    struct St {
        float dil = 0.0f, ep = 0.0f;
        RE::FormID lastP = 0;
        bool was = false, init = false;
        double worked = 0.0, winkAt = 0.0, partnerNear = 0.0;
        float sent = -1.0f, dilSent = -1.0f, g = 0.0f;
        bool inScene = false;
        float sz = 1.0f;          // partner size (TNG): how far he opens this round
        RE::FormID szFirst = 0;   // the round's first partner (his size can still be on its way from TNG)
        float donut = 0.0f, donutSent = -1.0f;   // During sex page: the puffy rim while he is taken
        float depth = -100.0f;                   // how deep the schlong in him is (last tick), for the withdraw
        float pout = 0.0f, canal = 0.0f, bulge[19]{};   // During sex page: rim, open inside, belly bulge (Seg 1-6, Head 1-6, Knot, Flare 1-6)
        float pulse = 0.0f, bulgeDil = 0.0f;     // thrust pulse; how worked he was while it bulged (lingering)
        float hole = 0.0f, holeSent = -100.0f;
        // the hole following the schlong, per detail level (Normal / Fine / Super fine): the entrance rings (directions x
        // rings) then the inside depths. All three are kept so a switch eases the old set closed.
        float hv[3][64]{}, hs[3][64]{};
        bool hsInit = false;
        double inLast = -100.0;                  // last time a schlong was found in him by the bones
        float off = -1.0f;                       // the shaft's centre off his canal centre at the entrance (units)
        bool held = false;                       // kept in by the hold this tick
        float reachMax = 0.0f;                   // how deep the tip has been lately (the inside stays open behind it)
        float shE[2]{}, shD[2]{}, shSent[4]{-100.0f, -100.0f, -100.0f, -100.0f};   // the hole moved with the shaft: entrance, inside (right, up)
        float ringPos = 0.0f;                    // the rim carried along by the shaft (units, + = out of him)
        float depthAvg = -100.0f;                // where the strokes are centred (slow average of the depth)
        float rndH = 0.0f, rndA = 0.0f;          // this stroke's bulge randomness
        float lastVel = 0.0f;
        bool ringSwellCleared = false;
        double bulgeLast = -100.0;               // last time it bulged
        float poutSent = -100.0f, canalSent = -100.0f, bulgeSent[19]{-100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f,
                                                                       -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f};
        double testBulge = 0.0; int testShape = 1;   // Testing page: show the bulge
    };
    std::mutex g_mx;
    std::unordered_map<RE::FormID, St> g_st;
    std::mutex g_gapeMx;                                 // only for GapeOf (the status page asks under other locks)
    std::unordered_map<RE::FormID, float> g_gape;
    struct DuringNow { float depth = -100.0f, hole = 0.0f, ring = 0.0f, off = -1.0f; bool held = false; };
    std::unordered_map<RE::FormID, DuringNow> g_during;   // for the status window
    std::atomic<bool> g_queued{false};
    double g_last = 0.0, g_lastCol = 0.0;

    float Smooth(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }

    void SetMorph(RE::Actor* a, const char* name, const char* key, float v) { if (g_morph) g_morph->SetMorph(a, name, key, v); }
    float GetMorph(RE::Actor* a, const char* name, const char* key) { return g_morph ? g_morph->GetMorph(a, name, key) : 0.0f; }

    bool Allowed(RE::Actor* a, RE::Actor* player) {
        if (a == player) return true;
        const float who = Glob(0x80E, 2.0f);
        if (who < 0.5f) return false;
        if (who < 1.5f) return a->IsPlayerTeammate();
        return true;
    }

    // ---- one man, one tick
    void Process(RE::Actor* act, St& s, float dt, double now, const std::vector<RE::Actor*>& others, RE::Actor* player) {
        if (!s.init) {   // pick up where a save left him
            s.init = true;
            s.dil = std::clamp(GetMorph(act, "AGDil", K), 0.0f, 1.0f);
            s.ep = std::clamp(GetMorph(act, "AGEp", K), 0.0f, 8.0f);
            s.sent = GetMorph(act, "Anatomica Gape", K);
            s.dilSent = s.dil;
            if (GetMorph(act, "Anatomica Wink", K) != 0.0f) { SetMorph(act, "Anatomica Wink", K, 0.0f); s.sent = -1.0f; }   // older version's wink morph
        }
        const float mx = Glob(0x801, 0.8f), build = std::max(Glob(0x802, 30.0f), 0.5f), hold0 = Glob(0x803, 5.0f);
        const float decay = std::max(Glob(0x804, 30.0f), 0.5f), during = Glob(0x805, 0.5f), multi = std::max(Glob(0x807, 2.0f), 1.0f);
        const float rad = Glob(0x808, 3.0f), duringHold = Glob(0x80C, 2.0f);
        const Virgin::Mods vm = Virgin::For(act->GetFormID());   // his virginity / experience (Virginity page)
        const Canal c = CanalOf(act);
        RE::Actor* partner = nullptr;
        RE::Actor* atMan = nullptr;   // the man whose tip is at his entrance
        bool atEntrance = false;   // his tip is right at the hole (an almost-full pull-out): no left-over gape yet
        float nearest = 1e9f;
        if (c.ok)
            for (auto* o : others) {
                if (o == act) continue;
                const float d = act->GetPosition().GetDistance(o->GetPosition());
                if (d > 90.0f) continue;
                Size::Cat(o);   // close enough to be his partner: ask TNG now, so his size is known when it starts
                if (s.lastP && o->GetFormID() == s.lastP) {
                    auto* oroot = o->Get3D(false);
                    if (auto* op = Node(oroot, "NPC Pelvis [Pelv]")) nearest = std::min(nearest, Len(V(op->world.translate) - c.pelvis));
                }
                const int in = Inside(c, o, rad);
                if (in == 2) { partner = o; break; }
                if (in == 1 && s.was) { atEntrance = true; atMan = o; }   // only the man already in him (not someone walking past)
            }
        // the hold: the bone check alone flickers (a shaft a little off his canal's centre drops out of it every other
        // moment, and the hole then snapped shut and back). The man who was in him stays in as long as his tip is still
        // past the entrance and not far off to the side.
        // Also the first time: a shaft that goes in off to one side never lined up with the bone check at all, so the hole
        // only opened on the odd thrust that did. Whoever's schlong crosses his entrance near the hole with the tip past it
        // is in him (the one already in him first).
        s.held = false;
        if (partner) s.inLast = now;
        else if (c.ok) {
            RE::Actor* best = nullptr;
            for (auto* o : others) {
                if (o == act || act->GetPosition().GetDistance(o->GetPosition()) > 90.0f) continue;
                const Tip t = TipIn(c, o);
                if (!t.ok || t.depth < 0.3f || t.depth > 16.0f) continue;
                V p;
                if (!ShaftAt(c, o, 0.0f, p)) continue;
                const V v = p - c.h;
                if (Len(v - c.a * Dot(v, c.a)) / c.sc > 6.5f) continue;
                best = o;
                if (o->GetFormID() == s.lastP) break;
            }
            if (best) { partner = best; s.held = true; s.inLast = now; }
        }
        float g = 0.0f, gd = 0.0f;
        if (partner) {
            // ---- being penetrated: dilation builds; the ring is held open by PPA, not by the morph
            if (!s.was) {
                if (s.ep < 1.0f) s.ep = 1.0f;
                else if (partner->GetFormID() != s.lastP) s.ep += 1.0f;
                s.lastP = partner->GetFormID();
                s.was = true;
            }
            const Size::K sk = Size::For(partner);   // a bigger man opens him further and faster
            if (!s.szFirst || partner->GetFormID() == s.szFirst) { s.sz = sk.open; s.szFirst = partner->GetFormID(); }
            else s.sz = std::max(s.sz, sk.open);
            s.dil = std::min(1.0f, s.dil + dt / (build * vm.build * sk.build));
            s.worked = now;
            s.partnerNear = now;
            s.inScene = true;
            Colour::WorkedNow(act, s.dil, s.ep, partner);
            g = 0.0f;
        } else if (atEntrance) {
            // pulled almost all the way out: PPA still holds the ring round his tip - adding the left-over gape on
            // top of that is what made it go wide. Count it as still in (no building), the gape starts once he is out.
            s.worked = now;
            s.partnerNear = now;
            s.inScene = true;
            g = 0.0f;
        } else {
            s.was = false;
            // still in the scene: the partner's hips are right at his (a pause, a slow pull-out between strokes)
            if (nearest < 45.0f) s.partnerNear = now;
            s.inScene = s.worked > 0.0 && now - s.partnerNear < 1.5 && now - s.worked < 30.0;
            if (s.dil > 0.0f) {
                const float pm = std::pow(multi, std::clamp(s.ep - 1.0f, 0.0f, 3.0f));
                const float hold = s.inScene ? duringHold : hold0 * pm;
                const float t = static_cast<float>(now - s.worked);
                if (s.inScene) {
                    // during the scene: what is left when he slides out, held a moment, then easing shut
                    const float e = during * vm.open * s.sz * Smooth(s.dil);
                    const float in = Smooth(t / 0.4f);   // eased in over 0.4 s, not snapped open (a quick stroke back in never shows it)
                    g = e * in * (t < hold ? 1.0f : 1.0f - Smooth((t - hold) / decay));
                } else {
                    if (s.worked <= 0.0 || t >= hold) {
                        const float q = std::max(1.0f, Colour::CloseSlowFor(act));
                        s.dil -= dt * vm.close / (pm * decay * q);
                        if (s.dil <= 0.0f) { s.dil = 0.0f; s.ep = 0.0f; s.sz = 1.0f; s.szFirst = 0; }
                    }
                    g = mx * vm.open * s.sz * Smooth(s.dil);
                    // clenches (the movement code does the motion)
                    if (Glob(0x80D, 1.0f) > 0.5f && g > 0.05f) {
                        gd = g;
                        if (now >= s.winkAt) {
                            if (s.winkAt > 0.0) Wink::BlinkNow(act);
                            float lo = Glob(0x80F, 2.5f), hi = Glob(0x810, 7.0f);
                            if (hi < lo) hi = lo;
                            lo *= vm.clench; hi *= vm.clench;   // a virgin clenches more often
                            s.winkAt = now + Rand(lo, hi) + 1.9;
                        }
                    }
                }
            }
        }
        s.g = g;
        { std::lock_guard gl(g_gapeMx); g_gape[act->GetFormID()] = g; }
        Wink::SetGapeNow(act, gd);
        // a clench closes the gape morph itself for a moment (the bones alone get overwritten by anus physics)
        const float shown = g * (1.0f - std::clamp(Wink::ClosureNow(act->GetFormID()), 0.0f, 0.95f));
        bool update = false;
        if (std::abs(shown - s.sent) > 0.002f || (shown == 0.0f && s.sent != 0.0f)) {
            SetMorph(act, "Anatomica Gape", K, shown);
            s.sent = shown;
            update = true;
        }
        {   // During sex page: the rim swells into a puffy ring while he is taken, and settles a few seconds after
            const float amt = During::Donut();
            const float want = (amt > 0.0f && (partner || atEntrance || s.inScene)) ? amt * Smooth(s.dil) : 0.0f;
            if (want > s.donut) s.donut = std::min(want, s.donut + dt / 1.5f);
            else s.donut = std::max(want, s.donut - dt / 4.0f);
            if (s.donutSent < 0.0f || std::abs(s.donut - s.donutSent) > 0.01f || (s.donut == 0.0f && s.donutSent > 0.0f)) {
                {
                    if (!s.ringSwellCleared) { SetMorph(act, "Anatomica Swell", "AnatomicaDuring", 0.0f); s.ringSwellCleared = true; }   // 2.4 test builds used Swell
                    SetMorph(act, "Anatomica Ring", "AnatomicaDuring", s.donut);
                    update = true;
                }
                s.donutSent = s.donut;
            }
        }
        if (std::abs(s.dil - s.dilSent) > 0.02f || (s.dil == 0.0f && s.dilSent != 0.0f)) {   // kept with the save
            SetMorph(act, "AGDil", K, s.dil);
            SetMorph(act, "AGEp", K, s.ep);
            s.dilSent = s.dil;
        }
        // During sex page: the hole sized to the schlong, the rim carried by the shaft, the inside opened, the belly bulge
        {
            const During::Want dw = During::Get();
            RE::Actor* in = partner ? partner : atMan;
            Tip tip;
            if (in && c.ok) tip = TipIn(c, in);
            const float vel = (tip.ok && s.depth > -50.0f && dt > 0.0f) ? (tip.depth - s.depth) / dt : 0.0f;   // units / s, > 0 = pushing in
            s.depth = tip.ok ? tip.depth : -100.0f;
            s.reachMax = (tip.ok && in) ? std::max(tip.depth, s.reachMax - dt * 4.0f) : std::max(0.0f, s.reachMax - dt * 8.0f);
            auto ease = [&](float& v, float want, float up, float down) {
                v = want > v ? std::min(want, v + dt / up) : std::max(want, v - dt / down);
            };
            // the hole follows the schlong as tightly as it can: the entrance opened in four directions to the shaft's own
            // extent (an oval or off-centre shaft opens each side as far as it reaches) and the inside at three depths to its
            // thickness there (a flared head or a knot widens the inside where it is, it closes again behind it). This replaces
            // the round opening (Anatomica Gape / Canal) and PPA's own opening; the rim's pull in and out stays.
            // detail: Normal = 4 directions + 3 depths, Fine = 8 x 2 rings + 6, Super fine = 16 x 3 rings + 10 (During page)
            const int lvl = std::clamp(dw.holeDetail, 0, 2);
            if (!s.hsInit) { for (auto& row : s.hs) for (auto& v : row) v = -100.0f; s.hsInit = true; }
            float want[3][64]{};
            const HoleSet& hset = HoleSetOf(lvl);
            s.off = -1.0f;
            if (in && c.ok && dw.holeFollow) {
                V upw = c.u - c.a * Dot(c.u, c.a);
                const float lu = Len(upw);
                upw = lu > 1e-4f ? upw * (1.0f / lu) : c.u;
                std::vector<float> depths;
                for (float d : hset.ringD) depths.push_back(d * c.sc);
                for (float d : hset.inD) depths.push_back(d * c.sc);
                std::vector<RE::NiPoint3> hints;
                for (float d : depths) {
                    V p;
                    hints.push_back(ShaftAt(c, in, d / c.sc, p) ? p.ni() : (c.h + c.a * d).ni());
                }
                const auto sl = Fluid::SlicesAt(in, c.h.ni(), c.a.ni(), c.x.ni(), upw.ni(), depths, hset.n, 3.8f * c.sc, &hints, dw.fitOutline);
                const float fallback = 0.95f * std::clamp(Size::For(in).open, 0.7f, 1.6f);   // his mesh could not be read: by his size
                s.off = sl[0].r > 0.0f ? std::sqrt(sl[0].cx * sl[0].cx + sl[0].cy * sl[0].cy) / c.sc : -1.0f;
                // the hole moved with the shaft: the entrance to where the shaft's centre is at the mouth, the inside to where
                // it is about 5 units in (the canal bends with an angled shaft)
                float wantE[2]{}, wantD[2]{};
                if (dw.shift && sl[0].r > 0.0f) {
                    wantE[0] = sl[0].cx / c.sc; wantE[1] = sl[0].cy / c.sc;
                    size_t best = 0; float bd = 1e9f;
                    for (size_t k = 0; k < depths.size(); ++k)
                        if (sl[k].r > 0.0f && std::abs(depths[k] / c.sc - 5.0f) < bd) { bd = std::abs(depths[k] / c.sc - 5.0f); best = k; }
                    if (bd < 3.0f) { wantD[0] = sl[best].cx / c.sc; wantD[1] = sl[best].cy / c.sc; }
                    else { wantD[0] = wantE[0]; wantD[1] = wantE[1]; }
                    for (float* w : {wantE, wantD}) {
                        const float l = std::sqrt(w[0] * w[0] + w[1] * w[1]);
                        if (l > dw.shiftMax && l > 1e-4f) { w[0] *= dw.shiftMax / l; w[1] *= dw.shiftMax / l; }
                    }
                }
                for (int j = 0; j < 2; ++j) { ease(s.shE[j], wantE[j], 0.08f, 0.08f); ease(s.shD[j], wantD[j], 0.1f, 0.1f); }
                auto ss = [](float a0, float a1, float x) { const float t = std::clamp((x - a0) / (a1 - a0), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
                int i = 0;
                for (int r = 0; r < hset.rings; ++r) {
                    // where the hole's centre now is at this ring's depth (the two shift zones blend from 1.5 to 3.5 units)
                    const float wd = ss(1.5f, 3.5f, hset.ringD[r]);
                    const float mx = s.shE[0] * (1.0f - wd) + s.shD[0] * wd, my = s.shE[1] * (1.0f - wd) + s.shD[1] * wd;
                    for (int q = 0; q < hset.n; ++q, ++i) {
                        const float ang = 6.2831853f * static_cast<float>(q) / static_cast<float>(hset.n);
                        // how far the shaft reaches this way from the (moved) centre; a ring the shaft does not reach stays shut
                        float e = sl[r].r > 0.0f ? sl[r].q[q] / c.sc - (mx * std::cos(ang) + my * std::sin(ang)) : (r == 0 ? fallback : 0.0f);
                        if (r > 0 && sl[r].r <= 0.0f && sl[0].r <= 0.0f) e = fallback;   // mesh unreadable: every ring by his size
                        want[lvl][i] = std::clamp((e * dw.holeSize - 0.08f) / 1.35f, 0.0f, 3.0f);
                    }
                }
                if (dw.canal > 0.0f)
                    for (size_t k = 0; k < hset.inD.size(); ++k, ++i) {
                        const auto& x = sl[hset.rings + k];
                        if (x.r > 0.0f) want[lvl][i] = std::min(dw.canal, 1.0f) * std::clamp((x.r / c.sc * dw.holeSize - 0.1f) / 1.4f, 0.0f, 3.0f);
                    }
                // no funnel: behind the entrance the inside never closes tighter than a gentle taper of the entrance's
                // opening, as far in as the tip has been (a ring or depth the shaft is not measured at clamped shut
                // and pinched the walls into a star); past the tip it closes over a couple of units
                {
                    float entr = 0.0f;
                    for (int q = 0; q < hset.n; ++q) entr += want[lvl][q];
                    entr /= static_cast<float>(hset.n);
                    const float reach = std::max(tip.ok ? tip.depth : 0.0f, s.reachMax);
                    auto floorAt = [&](float d) {
                        return entr * (1.0f - 0.04f * d) * (1.0f - ss(reach, reach + 2.5f, d));
                    };
                    int j = hset.n;   // ring A sets the floor; rings B / C keep their own direction, at least that
                    for (int r = 1; r < hset.rings; ++r)
                        for (int q = 0; q < hset.n; ++q, ++j)
                            want[lvl][j] = std::max(want[lvl][j], want[lvl][q] * (1.0f - 0.04f * hset.ringD[r]) * (1.0f - ss(reach, reach + 2.5f, hset.ringD[r])));
                    for (size_t k = 0; k < hset.inD.size(); ++k, ++j)
                        want[lvl][j] = std::max(want[lvl][j], std::min(dw.canal, 1.0f) * floorAt(hset.inD[k]));
                }
                if (dw.fitOutline) {   // the fit outline: what was measured (green) and what the hole is asked to be (yellow)
                    FitDbg fd;
                    fd.at = std::chrono::steady_clock::now();
                    fd.red.push_back(c.h.ni());
                    for (const auto& hp : hints) fd.blue.push_back(hp);
                    const V rt = c.x, upv = upw;
                    auto dirAt = [&](float t) { return rt * std::cos(t) + upv * std::sin(t); };
                    const float wN = 6.2831853f / static_cast<float>(hset.n);
                    for (size_t k = 0; k < depths.size(); ++k) {
                        const V ck = c.h + c.a * depths[k];
                        if (sl[k].hasQd && sl[k].r > 0.0f) {
                            std::vector<RE::NiPoint3> g;
                            for (int q = 0; q < 24; ++q) g.push_back((ck + dirAt(6.2831853f * q / 24.0f) * sl[k].qd[q]).ni());
                            fd.green.push_back(std::move(g));
                        }
                        // yellow: from the values actually sent (eased, capped), back through the same calibration
                        const float dm = depths[k] / c.sc;
                        const float wd = ss(1.5f, 3.5f, dm);
                        const V cen = ck + (rt * (s.shE[0] * (1.0f - wd) + s.shD[0] * wd) + upv * (s.shE[1] * (1.0f - wd) + s.shD[1] * wd)) * c.sc;
                        std::vector<RE::NiPoint3> y;
                        if (static_cast<int>(k) < hset.rings) {
                            const float* v = &s.hv[lvl][k * hset.n];
                            for (int q = 0; q < 24; ++q) {
                                const float t = 6.2831853f * q / 24.0f;
                                float val = 0.0f;
                                for (int j = 0; j < hset.n; ++j) {
                                    float d = t - wN * static_cast<float>(j);
                                    d = std::atan2(std::sin(d), std::cos(d));
                                    if (std::abs(d) < wN) { const float cw = std::cos(d * static_cast<float>(hset.n) / 4.0f); val += cw * cw * v[j]; }
                                }
                                y.push_back((cen + dirAt(t) * ((val * 1.35f + 0.08f) * c.sc)).ni());
                            }
                        } else {
                            const float val = s.hv[lvl][hset.rings * hset.n + (k - hset.rings)];
                            if (val <= 0.001f) continue;
                            for (int q = 0; q < 24; ++q) y.push_back((cen + dirAt(6.2831853f * q / 24.0f) * ((val * 1.4f + 0.1f) * c.sc)).ni());
                        }
                        fd.yellow.push_back(std::move(y));
                    }
                    std::lock_guard dl(g_dbgMx);
                    g_dbg[act->GetFormID()] = std::move(fd);
                }
            } else {
                for (int j = 0; j < 2; ++j) { ease(s.shE[j], 0.0f, 0.08f, 0.25f); ease(s.shD[j], 0.0f, 0.1f, 0.25f); }
            }
            for (int l = 0; l < 3; ++l) {
                const size_t cnt = HoleSetOf(l).names.size();
                for (size_t i = 0; i < cnt; ++i) ease(s.hv[l][i], want[l][i], 0.12f, want[l][i] > 0.0f ? 0.15f : (in ? 0.2f : 0.35f));   // (full open in 0.12 s: one bad reading cannot jump it)
            }
            {   // for the status window: the entrance ring, on average
                float sum = 0.0f;
                for (int q = 0; q < hset.n; ++q) sum += s.hv[lvl][q];
                s.hole = sum / static_cast<float>(hset.n);
            }
            // without it: the inside opened round the schlong by his size (the round Anatomica Canal)
            if (dw.holeFollow) s.canal = 0.0f;
            else ease(s.canal, (partner && dw.canal > 0.0f) ? dw.canal * std::clamp(Size::For(partner).open, 0.7f, 1.4f) : 0.0f, 0.25f, 0.8f);
            // the rim carried along by the shaft: in on the push, out on the withdraw, springing back to rest
            if (tip.ok && dw.ring) {
                // follows the stroke itself: deeper than where the strokes are centred = drawn in, shallower = pulled out
                if (s.depthAvg < -50.0f) s.depthAvg = tip.depth;
                s.depthAvg += (tip.depth - s.depthAvg) * std::min(1.0f, dt / 0.9f);
                const float maxOut = dw.maxOut * (1.0f + dw.grow * Smooth(s.dil));
                const float want = std::clamp(-(tip.depth - s.depthAvg) * dw.grip * 0.8f, -dw.maxIn, maxOut);
                s.ringPos += (want - s.ringPos) * std::min(1.0f, dt / 0.06f);
            } else {
                s.depthAvg = -100.0f;
                s.ringPos += (0.0f - s.ringPos) * std::min(1.0f, dt / 0.35f);
                if (std::abs(s.ringPos) < 0.01f) s.ringPos = 0.0f;
            }
            // belly bulge
            // belly bulge: six segments that fill one after another from the base (Seg 1 must be full before Seg 2 starts...),
            // a head that rolls along at the end of the filled part (Head 1-6, neighbours blended) and a knot low down
            float bw[19]{};   // Seg 1-6, Head 1-6, Knot, Flare 1-6
            s.pulse = std::max(0.0f, s.pulse - dt / 0.22f);   // a fast push adds a quick extra bump that dies away at once
            if (vel > 4.0f) s.pulse = std::max(s.pulse, dw.pulse * std::clamp((vel - 4.0f) / 12.0f, 0.0f, 1.0f));
            if (vel > 2.0f && s.lastVel <= 2.0f) {   // a new push: this stroke's randomness
                s.rndH = Rand(-1.0f, 1.0f) * dw.bulgeRandom;
                s.rndA = Rand(-1.0f, 1.0f) * dw.bulgeRandom;
            }
            s.lastVel = vel;
            bool bulging = false;
            const bool test = now < s.testBulge;
            if (test || (tip.ok && partner && dw.bulge > 0.0f && During::BulgeFor(act, player) && BulgeShows(act, dw.bulgeThrough))) {
                const int cat = test ? 4 : Size::Cat(partner);
                if (test || !dw.bulgeBig || cat >= 3) {
                    int shape = dw.bulgeShape;
                    if (test) shape = s.testShape;
                    else if (shape == 0) {   // from his schlong's shape
                        const std::string w = Size::ShapeWord(Size::Shape(partner));
                        shape = w.find("knot") != std::string::npos ? 4 : w.find("flare") != std::string::npos ? 3
                              : w.find("tentacle") != std::string::npos ? 5 : (cat >= 3 ? 2 : 1);
                    }
                    // how far in, past "shows from": 0 .. 1 = the whole path filled
                    const float p = test ? 0.55f + 0.45f * std::sin(static_cast<float>(now) * 1.6f)
                                         : std::clamp((tip.depth - dw.bulgeStart) / 10.0f, 0.0f, 1.0f);
                    const float A = (test ? 0.6f : dw.bulge * std::clamp(Size::For(partner).open, 0.7f, 1.5f)) *
                                    (1.0f + s.pulse) * (1.0f + 0.4f * s.rndA);
                    float segK = 1.0f, headK = 0.6f, knotK = 0.0f, wob = 0.0f;
                    switch (shape) {
                        case 1: segK = dw.outline * 0.4f; headK = 1.0f; break;   // a bump at the tip (and a faint trail)
                        case 3: segK = 1.15f; headK = 1.2f; break;                  // big head (horse): the flared head
                        case 4: knotK = 1.0f; break;                                // knot
                        case 5: segK = 0.65f; headK = 0.5f; wob = 0.5f; break;      // tentacle: thin, the head wanders
                        default: break;                                             // the whole shaft
                    }
                    float filled = std::clamp(p * 6.0f + s.rndH * 0.6f, 0.0f, 6.0f);
                    if (wob > 0.0f) filled = std::clamp(filled + wob * std::sin(static_cast<float>(now) * 2.3f + static_cast<float>(act->GetFormID() % 7)), 0.0f, 6.0f);
                    if (A > 0.0f && filled > 0.0f) {
                        for (int k = 0; k < 6; ++k) bw[k] = A * segK * std::clamp(filled - static_cast<float>(k), 0.0f, 1.0f);
                        const int hb = shape == 3 ? 13 : 6;   // the round head, or the flared (horse) one
                        const float hi = filled - 0.6f;   // head k sits 0.6 of a segment back from the end of segment k + 1
                        if (hi <= 0.0f) bw[hb] = A * headK * std::clamp(filled / 0.6f, 0.0f, 1.0f);
                        else {
                            const int h0 = std::min(4, static_cast<int>(hi));
                            const float fr = std::clamp(hi - static_cast<float>(h0), 0.0f, 1.0f);
                            bw[hb + h0] = A * headK * (1.0f - fr);
                            bw[hb + h0 + 1] = A * headK * fr;
                        }
                        if (knotK > 0.0f) bw[12] = A * knotK * std::clamp((filled - 1.2f) / 0.8f, 0.0f, 1.0f);   // once it is in
                        bulging = true;
                    }
                }
            }
            if (bulging) s.bulgeLast = now, s.bulgeDil = std::max(s.bulgeDil, s.dil);
            const float linger = (now - s.bulgeLast < 30.0) ? std::max(0.8f, dw.linger * (0.5f + Smooth(s.bulgeDil))) : 0.8f;
            if (!bulging && now - s.bulgeLast > 30.0) s.bulgeDil = 0.0f;
            for (int i = 0; i < 19; ++i) ease(s.bulge[i], bw[i], 0.08f, bulging ? 0.25f : linger);
            // (the first time each session it is always sent: a save made mid-scene keeps the morphs it had)
            auto send = [&](const char* morph, float v, float& sent) {
                if (sent < -50.0f || std::abs(v - sent) > 0.02f || (std::abs(v) < 1e-4f && std::abs(sent) > 1e-4f)) {
                    SetMorph(act, morph, "AnatomicaDuring", v);
                    update = true;
                    sent = v;
                }
            };
            send("Anatomica Gape", 0.0f, s.holeSent);   // (2.4 test builds opened the hole with the gape shape: cleared)
            send("Anatomica Shift Right", s.shE[0], s.shSent[0]);
            send("Anatomica Shift Up", s.shE[1], s.shSent[1]);
            send("Anatomica Shift Deep Right", s.shD[0], s.shSent[2]);
            send("Anatomica Shift Deep Up", s.shD[1], s.shSent[3]);
            for (int l = 0; l < 3; ++l) {
                const auto& nm = HoleSetOf(l).names;
                for (size_t i = 0; i < nm.size(); ++i) send(nm[i].c_str(), s.hv[l][i], s.hs[l][i]);
            }
            send("Anatomica Canal", s.canal, s.canalSent);
            send("Anatomica Pout", s.ringPos, s.poutSent);
            static const char* kBulge[19] = {"Anatomica Bulge Seg 1", "Anatomica Bulge Seg 2", "Anatomica Bulge Seg 3",
                                             "Anatomica Bulge Seg 4", "Anatomica Bulge Seg 5", "Anatomica Bulge Seg 6",
                                             "Anatomica Bulge Head 1", "Anatomica Bulge Head 2", "Anatomica Bulge Head 3",
                                             "Anatomica Bulge Head 4", "Anatomica Bulge Head 5", "Anatomica Bulge Head 6",
                                             "Anatomica Bulge Knot",
                                             "Anatomica Bulge Flare 1", "Anatomica Bulge Flare 2", "Anatomica Bulge Flare 3",
                                             "Anatomica Bulge Flare 4", "Anatomica Bulge Flare 5", "Anatomica Bulge Flare 6"};
            for (int i = 0; i < 19; ++i) send(kBulge[i], s.bulge[i], s.bulgeSent[i]);
            { std::lock_guard gl(g_gapeMx); g_during[act->GetFormID()] = {tip.ok ? tip.depth : -100.0f, s.hole, s.ringPos, s.off, s.held}; }
        }
        if (update && g_morph) g_morph->UpdateModelWeight(act, false);
    }

    // ---- RaceMenu node overrides
    struct SetV : SKEE::IOverrideInterface::SetVariant {
        Type t = Type::None; std::int32_t i = 0; float f = 0.0f; std::string s;
        Type GetType() override { return t; }
        std::int32_t Int() override { return i; }
        float Float() override { return f; }
        const char* String() override { return s.c_str(); }
    };
    struct GetV : SKEE::IOverrideInterface::GetVariant {
        std::string s; std::int32_t i = 0; float f = 0.0f; bool got = false;
        void Int(const std::int32_t v) override { i = v; got = true; }
        void Float(const float v) override { f = v; got = true; }
        void String(const char* v) override { s = v ? v : ""; got = true; }
        void Bool(const bool) override {}
        void TextureSet(const RE::BGSTextureSet*) override {}
    };
    std::uint8_t Idx(int key, int index) { return (key == 9 || key == 6) ? static_cast<std::uint8_t>(std::clamp(index, 0, 254)) : 255; }
    void Put(RE::Actor* a, const char* node, int key, int index, SetV& v, bool persist) {
        const auto ix = Idx(key, index);
        if (persist) g_ovr->AddNodeOverride(a, false, node, static_cast<std::uint16_t>(key), ix, v);
        g_ovr->SetNodeProperty(a, false, node, static_cast<std::uint16_t>(key), ix, v, false);
    }
    void PutStr(RE::Actor* a, const char* node, int key, int index, const std::string& str, bool persist = true) { SetV v; v.t = SetV::Type::String; v.s = str; Put(a, node, key, index, v, persist); }
    void PutInt(RE::Actor* a, const char* node, int key, std::int32_t x, bool persist = true) { SetV v; v.t = SetV::Type::Int; v.i = x; Put(a, node, key, -1, v, persist); }
    void PutFloat(RE::Actor* a, const char* node, int key, float x, bool persist = true) { SetV v; v.t = SetV::Type::Float; v.f = x; Put(a, node, key, -1, v, persist); }
    void Remove(RE::Actor* a, const char* node, int key, int index) { g_ovr->RemoveNodeOverride(a, false, node, static_cast<std::uint16_t>(key), Idx(key, index)); }
    std::string GetStr(RE::Actor* a, const char* node, int key, int index) { GetV v; g_ovr->GetNodeOverride(a, false, node, static_cast<std::uint16_t>(key), Idx(key, index), v); return v.s; }
    float GetFloat(RE::Actor* a, const char* node, int key) { GetV v; g_ovr->GetNodeOverride(a, false, node, static_cast<std::uint16_t>(key), Idx(key, -1), v); return v.f; }

    constexpr const char* kBlendBase = "textures\\actors\\character\\male\\Anatomica\\colours\\anatomica_blend_base.dds";
    constexpr const char* kBlendOrig = "Actors\\Character\\Overlays\\Anatomica\\anatomica_blend.dds";

    // the RaceMenu body paint slot that holds Anatomica's ring: tint it, put it back, or add one
    void BlendOne(RE::Actor* a, int col, int swap, int add) {
        if (!g_ovl) return;
        if (add >= 0 && !g_ovl->HasOverlays(a)) g_ovl->AddOverlays(a, true);
        const std::uint32_t n = g_vOvl >= 2 ? g_ovl->GetOverlayCount(SKEE::IOverlayInterface::OverlayType::Normal, SKEE::IOverlayInterface::OverlayLocation::Body) : 6;
        int found = -1, freeSlot = -1;
        std::string cur;
        for (std::uint32_t i = 0; i < n && i < 64; ++i) {
            const std::string node = std::format("Body [Ovl{}]", i);
            const std::string tex = GetStr(a, node.c_str(), 9, 0);
            if (tex.find("anatomica_blend") != std::string::npos) { found = static_cast<int>(i); cur = tex; break; }
            if (freeSlot < 0 && (tex.empty() || tex.find("default.dds") != std::string::npos)) freeSlot = static_cast<int>(i);
        }
        if (found < 0) {   // no ring on him: add one in a free slot (NPC ring option)
            if (add < 0 || freeSlot < 0) return;
            const std::string node = std::format("Body [Ovl{}]", freeSlot);
            PutStr(a, node.c_str(), 9, 0, kBlendBase);
            PutInt(a, node.c_str(), 7, add);
            PutFloat(a, node.c_str(), 8, 0.999f);   // 0.999 = a ring Anatomica added
            return;
        }
        const std::string node = std::format("Body [Ovl{}]", found);
        if (col == -4) {   // re-roll: a ring we added comes off; his own paint goes back to the installed look
            const float fa = GetFloat(a, node.c_str(), 8);
            if (fa > 0.9985f && fa < 0.9995f) {
                Remove(a, node.c_str(), 9, 0); Remove(a, node.c_str(), 7, -1); Remove(a, node.c_str(), 8, -1);
                return;
            }
            col = -1;
        }
        const bool isBase = cur.find("blend_base") != std::string::npos;
        if (col == -3) { if (!isBase) return; col = -1; }
        if (col < 0) {
            if (isBase) PutStr(a, node.c_str(), 9, 0, kBlendOrig);
            PutInt(a, node.c_str(), 7, 0xFFFFFF);
            return;
        }
        if (swap == 1) { if (!isBase) PutStr(a, node.c_str(), 9, 0, kBlendBase); }
        else if (isBase) PutStr(a, node.c_str(), 9, 0, kBlendOrig);
        PutInt(a, node.c_str(), 7, col);
    }

    // the inside (the canal, 3BA_Anus): its own colour texture (Colours page > Inside colour) and "always moist inside".
    // With RaceMenu's C++ override interface straight on it, else through NiOverride's Papyrus functions (older RaceMenu).
    struct InnerSent { std::string path = "\x02"; float moist = -1.0f; };
    std::unordered_map<RE::FormID, InnerSent> g_innerSent;
    void PapyrusNode(RE::Actor* a, const char* node, int key, int index, const std::string* str, float f, bool remove) {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) return;
        RE::TESObjectREFR* ref = a;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
        if (remove) {
            auto* args = RE::MakeFunctionArguments(std::move(ref), false, RE::BSFixedString(node), static_cast<std::int32_t>(key), static_cast<std::int32_t>(index));
            vm->DispatchStaticCall("NiOverride", "RemoveNodeOverride", args, cb);
        } else if (str) {
            auto* args = RE::MakeFunctionArguments(std::move(ref), false, RE::BSFixedString(node), static_cast<std::int32_t>(key), static_cast<std::int32_t>(index), RE::BSFixedString(*str), true);
            vm->DispatchStaticCall("NiOverride", "AddNodeOverrideString", args, cb);
        } else {
            auto* args = RE::MakeFunctionArguments(std::move(ref), false, RE::BSFixedString(node), static_cast<std::int32_t>(key), static_cast<std::int32_t>(index), std::move(f), true);
            vm->DispatchStaticCall("NiOverride", "AddNodeOverrideFloat", args, cb);
        }
    }
    void InnerOne(RE::Actor* a) {
        auto* root = a->Get3D(false);
        if (!root || !Node(root, "3BA_Anus")) return;
        const auto* npc = a->GetActorBase();
        if (!npc || npc->GetSex() != RE::SEX::kMale) return;
        auto& st = g_innerSent[a->GetFormID()];
        const std::string in = Colour::InnerPath(a);
        if (in != "\x01" && in != st.path) {   // "\x01" = his new one is being made: keep what he has
            constexpr const char* kInstalledInner = "textures\\actors\\character\\male\\Anatomica\\anatomica.dds";
            if (g_ovr) {
                const std::string cur = GetStr(a, "3BA_Anus", 9, 0);
                if (in.empty()) {
                    if (!cur.empty()) { PutStr(a, "3BA_Anus", 9, 0, kInstalledInner, false); Remove(a, "3BA_Anus", 9, 0); }
                } else if (in != cur) PutStr(a, "3BA_Anus", 9, 0, in);
            } else if (in.empty()) {
                if (st.path != "\x02") { const std::string inst = kInstalledInner; PapyrusNode(a, "3BA_Anus", 9, 0, &inst, 0.0f, false); }
                PapyrusNode(a, "3BA_Anus", 9, 0, nullptr, 0.0f, true);
            } else {
                PapyrusNode(a, "3BA_Anus", 9, 0, &in, 0.0f, false);
            }
            if (!in.empty()) logger::info("Anatomica inside colour: {} ({:08X}) -> {}", a->GetName(), a->GetFormID(), in);
            st.path = in;
        }
        const float m = Colour::InnerMoist();
        if (std::abs(m - st.moist) > 0.005f) {
            const float gl = 30.0f + 170.0f * m, sp = 3.0f + 5.0f * m;   // the installed inside: glossiness 30, specular 3
            if (m > 0.001f) {
                if (g_ovr) { PutFloat(a, "3BA_Anus", 2, gl); PutFloat(a, "3BA_Anus", 3, sp); }
                else { PapyrusNode(a, "3BA_Anus", 2, -1, nullptr, gl, false); PapyrusNode(a, "3BA_Anus", 3, -1, nullptr, sp, false); }
            } else if (st.moist > 0.001f) {
                if (g_ovr) {
                    PutFloat(a, "3BA_Anus", 2, 30.0f, false); PutFloat(a, "3BA_Anus", 3, 3.0f, false);
                    Remove(a, "3BA_Anus", 2, -1); Remove(a, "3BA_Anus", 3, -1);
                } else {
                    PapyrusNode(a, "3BA_Anus", 2, -1, nullptr, 30.0f, false); PapyrusNode(a, "3BA_Anus", 3, -1, nullptr, 3.0f, false);
                    PapyrusNode(a, "3BA_Anus", 2, -1, nullptr, 0.0f, true); PapyrusNode(a, "3BA_Anus", 3, -1, nullptr, 0.0f, true);
                }
            }
            st.moist = m;
        }
    }

    // after-sex swelling as a raised ring (After sex page, "Ring-shaped"): Anatomica Ring under its own key
    std::unordered_map<RE::FormID, float> g_ringSwell;
    void RingSwellOne(RE::Actor* a) {
        if (!g_morph || !a) return;
        const auto* npc = a->GetActorBase();
        if (!npc || npc->GetSex() != RE::SEX::kMale) return;
        const float v = Colour::SwellRingNow(a);
        auto it = g_ringSwell.find(a->GetFormID());
        const float was = it == g_ringSwell.end() ? -1.0f : it->second;
        if (was >= 0.0f && std::abs(v - was) < 0.01f) return;   // (the first time each session it is always sent)
        SetMorph(a, "Anatomica Ring", "AnatomicaSwell", v);
        g_morph->UpdateModelWeight(a, false);
        g_ringSwell[a->GetFormID()] = v;
    }

    // what the script's ColourOne did, for one man
    void ColourOne(RE::Actor* a) {
        auto* root = a->Get3D(false);
        if (!root || !Node(root, "3BA_AnusRim")) return;
        const auto* npc = a->GetActorBase();
        if (!npc || npc->GetSex() != RE::SEX::kMale) return;
        const std::vector<std::int32_t> v = Colour::LookNow(a);
        if (v.size() < 6) return;
        bool model = false;
        // texture of the rim
        if (v[0] == -1) Remove(a, "3BA_AnusRim", 9, 0);
        else if (v[0] == 0) {
            const std::string w = Colour::ColourPath(a);
            if (!w.empty() && w != GetStr(a, "3BA_AnusRim", 9, 0)) PutStr(a, "3BA_AnusRim", 9, 0, w);
        }
        // anus tint
        if (v[1] == -1) Remove(a, "3BA_AnusRim", 7, -1);
        else if (v[1] >= 0) PutInt(a, "3BA_AnusRim", 7, v[1]);
        // the old wet gloss (Fluid does the gloss now): clear what an older version left
        if (v[4] == -1) for (const char* nd : {"3BA_AnusRim", "3BA_Anus"}) { Remove(a, nd, 2, -1); Remove(a, nd, 3, -1); }
        InnerOne(a);   // the inside: its own colour texture, "always moist"

        if (v.size() > 9) {
            if (v[7] != -2 && g_morph) {
                const float f = static_cast<float>(v[7]) / 1000.0f;
                if (std::abs(GetMorph(a, "Anatomica Swell", K) - f) > 0.001f) { SetMorph(a, "Anatomica Swell", K, f); model = true; }
            }
            if (v[8] == -1) for (auto& nm : Colour::SchlongShapeNames(a)) { Remove(a, nm.c_str(), 2, -1); Remove(a, nm.c_str(), 3, -1); }
        }
        // lasting looseness: healed over game days
        if (g_morph) {
            const float lv = GetMorph(a, "Anatomica Gape", "AnatomicaLoose");
            const float lt = GetMorph(a, "AGLooseT", K);
            const float gt = RE::Calendar::GetSingleton() ? RE::Calendar::GetSingleton()->GetCurrentGameTime() : 0.0f;
            const float days = (lt > 0.0f && lt <= gt) ? gt - lt : 0.0f;
            const float nl = Colour::LooseNow(a, lv, days);
            if (std::abs(nl - lv) > 0.002f) { SetMorph(a, "Anatomica Gape", "AnatomicaLoose", nl); model = true; }
            if (nl > 0.0f && gt - lt > 0.04f) SetMorph(a, "AGLooseT", K, gt);
        }
        if (model && g_morph) g_morph->UpdateModelWeight(a, false);
        // RaceMenu skin-blend ring
        if (v[2] != -2) BlendOne(a, v[2], v[3], v.size() > 6 ? v[6] : -1);
    }

    // ---- the skin-blend ring, straight on the RaceMenu overlay's material (for RaceMenu versions whose override
    // interface we can't use). Not stored anywhere: it is checked again every colour pass and put back if the game
    // rebuilt him.
    constexpr const char* kRingOrigTex = "textures\\actors\\character\\Overlays\\Anatomica\\anatomica_blend.dds";
    std::string Lower(std::string x) { for (auto& ch : x) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return x; }
    RE::BSLightingShaderMaterialBase* MatOf(RE::BSGeometry* geo) {
        auto* lsp = geo ? geo->lightingShaderProp_cast() : nullptr;
        return lsp ? static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material) : nullptr;
    }
    std::string DiffuseOf(RE::BSGeometry* geo) {
        auto* m = MatOf(geo);
        const char* p = (m && m->textureSet) ? m->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
        return p ? Lower(p) : std::string{};
    }
    bool IsTint(RE::BSLightingShaderMaterialBase* m) {
        return m && (m->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint || m->GetFeature() == RE::BSShaderMaterial::Feature::kHairTint);
    }
    // The ring's own texture, coloured in the file: the game does not draw a tint on these paint overlays (a white ring
    // although the material says reddish-brown), so the colour goes into the texture instead. The texture is DXT5:
    // every 4x4 block keeps two colours and blends between them, so scaling those two colours colours the whole block
    // (alpha untouched, mipmaps included) - same size, same detail, written once per colour.
    // Made in the background (Maker.cpp), straight at the chosen size: only the mip levels that are kept are read and
    // coloured (1024 = the base's 2nd level onward). Returns the path, kRingPending while it is being made, "" if it
    // cannot be made (the ring keeps the tint).
    constexpr const char* kRingPending = "\x01pending";
    bool MakeRing(int r, int gg, int b, int drop, bool strong, const std::filesystem::path& out, const std::string& tex) {
        std::ifstream in("Data/textures/actors/character/male/Anatomica/colours/anatomica_blend_base.dds", std::ios::binary);
        std::uint8_t hdr[128]{};
        if (!in.read(reinterpret_cast<char*>(hdr), 128) || std::memcmp(hdr, "DDS ", 4) != 0 || std::memcmp(hdr + 84, "DXT5", 4) != 0) {
            logger::info("Anatomica: the ring texture could not be read as a loose DXT5 file - the ring keeps the tint");
            return false;
        }
        std::uint32_t h = 0, w = 0, mips = 1;
        std::memcpy(&h, hdr + 12, 4); std::memcpy(&w, hdr + 16, 4); std::memcpy(&mips, hdr + 28, 4);
        mips = std::max(1u, mips);
        std::uint64_t skip = 0;
        for (int l = 0; l < drop && mips > 1; ++l) {   // the bigger levels: not read at all
            skip += static_cast<std::uint64_t>(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
            w = std::max(1u, w / 2); h = std::max(1u, h / 2); --mips;
        }
        in.seekg(static_cast<std::streamoff>(128 + skip));
        std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (d.size() < 16) return false;
        const float k[3] = {r / 255.0f, gg / 255.0f, b / 255.0f};
        auto tint = [&](std::uint8_t* p) {
            std::uint16_t c; std::memcpy(&c, p, 2);
            const int r5 = static_cast<int>(std::lround(((c >> 11) & 31) * k[0]));
            const int g6 = static_cast<int>(std::lround(((c >> 5) & 63) * k[1]));
            const int b5 = static_cast<int>(std::lround((c & 31) * k[2]));
            c = static_cast<std::uint16_t>((std::clamp(r5, 0, 31) << 11) | (std::clamp(g6, 0, 63) << 5) | std::clamp(b5, 0, 31));
            std::memcpy(p, &c, 2);
        };
        for (size_t o = 0; o + 16 <= d.size(); o += 16) { tint(&d[o + 8]); tint(&d[o + 10]); }   // colour half of each block
        if (strong)   // "Stronger skin-blend ring": the two alpha end values of each block doubled (the block keeps its mode)
            for (size_t o = 0; o + 16 <= d.size(); o += 16) {
                const int a0 = d[o], a1 = d[o + 1];
                int n0 = std::min(255, a0 * 2), n1 = std::min(255, a1 * 2);
                if (a0 > a1 && n0 <= n1) n1 = n0 - 1;   // 8-value blocks must stay a0 > a1
                d[o] = static_cast<std::uint8_t>(n0); d[o + 1] = static_cast<std::uint8_t>(std::max(0, n1));
            }
        const std::uint32_t lin = std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * 16;
        std::memcpy(hdr + 12, &h, 4); std::memcpy(hdr + 16, &w, 4); std::memcpy(hdr + 20, &lin, 4); std::memcpy(hdr + 28, &mips, 4);
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        const auto tmp = std::filesystem::path(out).concat(".tmp");   // whole file or nothing: the game never sees half of one
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f || !f.write(reinterpret_cast<const char*>(hdr), 128) || !f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()))) return false;
        }
        std::filesystem::rename(tmp, out, ec);
        if (ec) return false;
        logger::info("Anatomica: made {}", tex);
        return true;
    }
    std::string TintedRing(int rgb) {
        auto q = [](int c) { return std::clamp((c + 4) / 8 * 8, 0, 255); };
        const int r = q((rgb >> 16) & 255), gg = q((rgb >> 8) & 255), b = q(rgb & 255);
        const int key = (r << 16) | (gg << 8) | b;
        const int drop = std::clamp(Colour::RingTexLevel(), 0, 2);   // smaller copies: the ring is a soft fade
        const bool strong = Colour::RingStrong();
        const std::string name = std::format("anatomica_blend_tint_{:06X}_{}{}.dds", key, 2048 >> drop, strong ? "_x2" : "");
        const std::filesystem::path out = std::filesystem::path("Data/textures/actors/character/male/Anatomica/gen") / name;
        const std::string tex = std::string("textures\\actors\\character\\male\\Anatomica\\gen\\") + name;
        switch (Maker::Of(tex)) {
            case Maker::State::done: return tex;
            case Maker::State::busy: return kRingPending;
            case Maker::State::failed: return {};
            default: break;
        }
        std::error_code ec;
        if (std::filesystem::exists(out, ec)) { Maker::MarkDone(tex); return tex; }
        Maker::Queue(tex, [r, gg, b, drop, strong, out, tex]() { return MakeRing(r, gg, b, drop, strong, out, tex); });
        return kRingPending;
    }
    bool IsTintedRing(const std::string& diffuse) { return diffuse.find("blend_tint_") != std::string::npos; }

    // tex: "" = keep; rgb < 0 = keep the tint; alpha < 0 = keep
    void RingLook(RE::BSGeometry* geo, const char* tex, int rgb, float alpha) {
        auto* lsp = geo ? geo->lightingShaderProp_cast() : nullptr;
        auto* mat = MatOf(geo);
        if (!lsp || !mat) return;
        const bool texChange = tex && *tex && DiffuseOf(geo).find(Lower(tex).substr(9)) == std::string::npos;
        const bool needTint = rgb >= 0 && !IsTint(mat);
        if (texChange || needTint) {
            RE::BSLightingShaderMaterialBase* nm = needTint ? RE::BSLightingShaderMaterialBase::CreateMaterial<RE::BSLightingShaderMaterialFacegenTint>()
                                                            : static_cast<RE::BSLightingShaderMaterialBase*>(mat->Create());
            if (!nm) return;
            nm->CopyMembers(mat);
            if (texChange) {
                if (auto* ts = RE::BSShaderTextureSet::Create()) {
                    for (std::uint32_t i = 0; i < RE::BSTextureSet::Texture::kTotal; ++i) {
                        const char* pth = mat->textureSet ? mat->textureSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i)) : nullptr;
                        if (pth && *pth) ts->SetTexturePath(static_cast<RE::BSTextureSet::Texture>(i), pth);
                    }
                    ts->SetTexturePath(RE::BSTextureSet::Texture::kDiffuse, tex);
                    nm->ClearTextures();
                    nm->OnLoadTextureSet(0, ts);
                }
            }
            if (needTint) {   // what RaceMenu does for a tinted paint: a face-tint material
                lsp->SetFlags(RE::BSShaderProperty::EShaderPropertyFlag8::kFace, false);
                lsp->SetFlags(RE::BSShaderProperty::EShaderPropertyFlag8::kFaceGenRGBTint, true);
            }
            lsp->SetMaterial(nm, true);
            lsp->SetupGeometry(geo);
            lsp->FinishSetupGeometry(geo);
            nm->~BSLightingShaderMaterialBase();
            RE::free(nm);
            mat = MatOf(geo);
        }
        if (rgb >= 0 && IsTint(mat)) {
            auto* t = static_cast<RE::BSLightingShaderMaterialFacegenTint*>(mat);
            t->tintColor.red = ((rgb >> 16) & 255) / 255.0f;
            t->tintColor.green = ((rgb >> 8) & 255) / 255.0f;
            t->tintColor.blue = (rgb & 255) / 255.0f;
        }
        if (alpha >= 0.0f && mat && std::abs(mat->materialAlpha - alpha) > 0.01f) lsp->SetMaterialAlpha(alpha);
    }
    std::unordered_map<RE::FormID, double> g_askedOverlays;
    void AskOverlays(RE::Actor* a) {   // NPCs only get RaceMenu's paint slots when something asks for them
        auto& t = g_askedOverlays[a->GetFormID()];
        if (t > 0.0 && Now() - t < 60.0) return;
        t = Now();
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) return;
        RE::TESObjectREFR* ref = a;
        auto* args = RE::MakeFunctionArguments(std::move(ref));
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
        vm->DispatchStaticCall("NiOverride", "AddOverlays", args, cb);
    }
    // what we last put on each man's ring: nothing is touched again until the colour he should have changes, or the
    // game rebuilds his body / RaceMenu re-applies its paint (the ring's material is then a different one)
    struct RingDone { RE::NiAVObject* root = nullptr; RE::NiPointer<RE::BSGeometry> geo;   // held: a redress can rebuild the overlays
                      RE::BSShaderMaterial* mat = nullptr; int col = -9, swap = -9, add = -9; int tex = -1; };   // tex: size / strength settings
    std::unordered_map<RE::FormID, RingDone> g_ringDone;
    std::unordered_map<RE::FormID, std::string> g_ringLog;
    // one line in the log whenever what we find / do on a man's ring changes (to see why a ring stays white)
    void RingReport(RE::Actor* a, const char* what, RE::BSGeometry* geo, const Colour::RingWant& w) {
        std::string line = std::format("ring on {} ({:08X}): {} | wanted colour {} swap {} add {}", a->GetName(), a->GetFormID(), what,
                                       w.col >= 0 ? std::format("{:06X}", w.col) : std::string("none (his own paint)"), w.swap, w.add);
        if (geo) {
            auto* m = MatOf(geo);
            line += std::format(" | {} texture {} | {}", geo->name.c_str() ? geo->name.c_str() : "?", DiffuseOf(geo), IsTint(m) ? "tint material" : "plain material");
            if (IsTint(m) && m->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
                auto* t = static_cast<RE::BSLightingShaderMaterialFacegenTint*>(m);
                line += std::format(" {:.2f} {:.2f} {:.2f}", t->tintColor.red, t->tintColor.green, t->tintColor.blue);
            }
            if (auto* lsp = geo->lightingShaderProp_cast()) line += std::format(" alpha {:.2f}", m ? m->materialAlpha : -1.0f), (void)lsp;
        }
        auto& last = g_ringLog[a->GetFormID()];
        if (last == line) return;
        last = line;
        logger::info("Anatomica {}", line);
    }
    void RingOne(RE::Actor* a) {
        auto* root = a->Get3D(false);
        if (!root) return;
        const Colour::RingWant w = Colour::RingNow(a);
        const int texSet = Colour::RingTexLevel() * 2 + (Colour::RingStrong() ? 1 : 0);   // changed in the menu: redo the ring
        if (auto it = g_ringDone.find(a->GetFormID()); it != g_ringDone.end()) {
            const RingDone& d = it->second;
            // still the ring he wears? (a rebuilt body puts a new overlay shape there)
            const bool same = !d.geo || Node(root, d.geo->name.c_str()) == d.geo.get();
            auto* lsp = d.geo ? d.geo->lightingShaderProp_cast() : nullptr;
            if (same && d.root == root && d.col == w.col && d.swap == w.swap && d.add == w.add && d.tex == texSet && (!d.geo || (lsp && lsp->material == d.mat))) return;
        }
        RingDone done{root, nullptr, nullptr, w.col, w.swap, w.add, texSet};
        struct Mark { RingDone& d; RE::FormID id; ~Mark() { if (d.geo) if (auto* l = d.geo->lightingShaderProp_cast()) d.mat = l->material; g_ringDone[id] = d; } } mark{done, a->GetFormID()};
        if (!Node(root, "3BA_AnusRim")) return;
        const auto* npc = a->GetActorBase();
        if (!npc || npc->GetSex() != RE::SEX::kMale) return;
        RE::BSGeometry* ring = nullptr;
        RE::BSGeometry* freeSlot = nullptr;
        bool any = false;
        for (int i = 0; i < 32; ++i) {
            auto* o = Node(root, std::format("Body [Ovl{}]", i).c_str());
            auto* geo = o ? o->AsGeometry() : nullptr;
            if (!geo) continue;
            any = true;
            const std::string d = DiffuseOf(geo);
            if (d.find("anatomica_blend") != std::string::npos) { ring = geo; break; }
            if (!freeSlot && (d.empty() || d.find("default.dds") != std::string::npos)) freeSlot = geo;
        }
        if (!ring) {
            if (w.add < 0) { RingReport(a, any ? "no Anatomica ring among his RaceMenu paints, none to add" : "no RaceMenu paint slots", nullptr, w); return; }
            if (!any) { RingReport(a, "no RaceMenu paint slots yet: asked RaceMenu for them", nullptr, w); AskOverlays(a); done.root = nullptr; return; }   // look again once RaceMenu has added them
            if (!freeSlot) { RingReport(a, "no free RaceMenu paint slot for a ring", nullptr, w); return; }
            const std::string tt = TintedRing(w.add);
            if (tt == kRingPending) { RingReport(a, "his ring colour is being made", nullptr, w); done.root = nullptr; return; }   // look again next pass
            if (!tt.empty()) RingLook(freeSlot, tt.c_str(), -1, 1.0f);   // a ring for a man without one (NPC ring option)
            else RingLook(freeSlot, kBlendBase, w.add, 1.0f);
            done.geo = RE::NiPointer<RE::BSGeometry>(freeSlot);
            RingReport(a, "added a ring in a free slot", freeSlot, w);
            return;
        }
        done.geo = RE::NiPointer<RE::BSGeometry>(ring);
        const bool isBase = DiffuseOf(ring).find("blend_base") != std::string::npos || IsTintedRing(DiffuseOf(ring));
        if (w.col < 0) {   // his own paint, as installed
            if (isBase) RingLook(ring, kRingOrigTex, IsTint(MatOf(ring)) ? 0xFFFFFF : -1, -1.0f);
            else if (IsTint(MatOf(ring))) RingLook(ring, nullptr, 0xFFFFFF, -1.0f);
            RingReport(a, "left as his own paint", ring, w);
            return;
        }
        if (w.swap == 1) {
            const std::string tt = TintedRing(w.col);
            if (tt == kRingPending) { RingReport(a, "his ring colour is being made", ring, w); done.root = nullptr; return; }   // keeps the ring he has for now
            if (!tt.empty()) {
                RingLook(ring, tt.c_str(), IsTint(MatOf(ring)) ? 0xFFFFFF : -1, -1.0f);   // the colour is in the texture now
                RingReport(a, "coloured (in the texture)", ring, w);
                return;
            }
        }
        RingLook(ring, w.swap == 1 ? kBlendBase : kRingOrigTex, w.col, -1.0f);
        RingReport(a, "coloured (tint)", ring, w);
    }

    void Tick() {
        g_queued = false;
        const double now = Now();
        const float dt = g_last > 0.0 ? static_cast<float>(std::min(now - g_last, 0.5)) : 0.0f;
        g_last = now;
        if (dt <= 0.0f) return;
        if (Colour::LoadPauseNow() > 0.05f) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) return;
        const bool on = Glob(0x806, 1.0f) > 0.5f;
        // everyone loaded nearby (partners can be anyone), and the men to look after
        std::vector<RE::Actor*> all{player};
        if (auto* pl = RE::ProcessLists::GetSingleton())
            for (auto& h : pl->highActorHandles) {
                auto p = h.get();
                if (p && p.get() != player && p->Is3DLoaded()) all.push_back(p.get());
                if (all.size() > 80) break;
            }
        std::lock_guard lk(g_mx);
        if (on && g_morph) {
            for (auto* a : all) {
                const auto* npc = a->GetActorBase();
                if (!npc || npc->GetSex() != RE::SEX::kMale || a->IsDead()) continue;
                if (!Allowed(a, player)) continue;
                if (a != player && a->GetPosition().GetDistance(player->GetPosition()) > 4000.0f) continue;
                Virgin::Watch(a);   // virgin or not: known (and OVirginity asked) before anything happens
                Process(a, g_st[a->GetFormID()], dt, now, all, player);
            }
        }
        Bridge::Tick(player, all);   // other mods: arousal, SkyrimNet (paces itself)
        // men out of reach: forget them (their morph stays as it is, saved by RaceMenu)
        for (auto it = g_st.begin(); it != g_st.end();) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(it->first);
            if (!a || !a->Is3DLoaded()) { std::lock_guard gl(g_gapeMx); g_gape.erase(it->first); it = g_st.erase(it); }
            else ++it;
        }
        // the colour pass (everything through RaceMenu's interface; or, with an older RaceMenu, just the ring here)
        if (now - g_lastCol > std::max(Colour::Interval(), 1.0f)) {
            g_lastCol = now;
            const bool full = g_ovr && g_ovl;
            if (full) ColourOne(player); else { RingOne(player); InnerOne(player); }
            RingSwellOne(player);
            const auto men = Colour::Men();
            for (auto* a : men) { if (full) ColourOne(a); else { RingOne(a); InnerOne(a); } RingSwellOne(a); }
            SoftButt::Tick(player, men);   // Jiggle page: the SMP soft butt on the men chosen (off by default)
        }
    }

    void Loop() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (!g_asked) continue;
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->GameIsPaused()) { g_last = 0.0; continue; }
            if (g_queued.exchange(true)) continue;
            if (auto* t = SKSE::GetTaskInterface()) t->AddTask([]() { Tick(); });
            else g_queued = false;
        }
    }

    std::int32_t EngineModeNative(RE::StaticFunctionTag*) { return Engine::Mode(); }
}

namespace Engine {
    // the fit outline over the game (During sex page > Show the fit outline)
    void __stdcall DrawFitOutline() {
        if (!::During::Get().fitOutline) return;
        auto* cam = RE::Main::WorldRootCamera();
        auto* io = ImGuiMCP::GetIO();
        if (!cam || !io) return;
        const float W = io->DisplaySize.x, H = io->DisplaySize.y;
        auto* dl = ImGuiMCP::GetForegroundDrawList();
        if (!dl) return;
        auto proj = [&](const RE::NiPoint3& p, ImGuiMCP::ImVec2& out) {
            float x = 0, y = 0, z = 0;
            if (!cam->WorldPtToScreenPt3(p, x, y, z, 1e-5f) || z < 0.0f) return false;
            out = ImGuiMCP::ImVec2{x * W, (1.0f - y) * H};
            return true;
        };
        auto loop = [&](const std::vector<RE::NiPoint3>& pts, ImGuiMCP::ImU32 col, float th) {
            std::vector<ImGuiMCP::ImVec2> v;
            for (const auto& p : pts) { ImGuiMCP::ImVec2 o; if (!proj(p, o)) return; v.push_back(o); }
            if (v.size() >= 2) ImGuiMCP::ImDrawListManager::AddPolyline(dl, v.data(), static_cast<int>(v.size()), col, ImGuiMCP::ImDrawFlags_Closed, th);
        };
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lk(g_dbgMx);
        for (auto it = g_dbg.begin(); it != g_dbg.end();) {
            if (now - it->second.at > std::chrono::milliseconds(400)) { it = g_dbg.erase(it); continue; }
            const auto& d = it->second;
            for (const auto& g : d.green) loop(g, 0xFF40FF40u, 2.0f);    // ABGR: green
            for (const auto& y : d.yellow) loop(y, 0xFF30E0FFu, 1.5f);   // yellow
            for (const auto& p : d.red) { ImGuiMCP::ImVec2 o; if (proj(p, o)) ImGuiMCP::ImDrawListManager::AddCircleFilled(dl, o, 4.0f, 0xFF3030FFu, 12); }
            for (const auto& p : d.blue) { ImGuiMCP::ImVec2 o; if (proj(p, o)) ImGuiMCP::ImDrawListManager::AddCircleFilled(dl, o, 3.0f, 0xFFFF9030u, 12); }
            ++it;
        }
    }
    void RegisterHud() { SKSEMenuFramework::AddHudElement(DrawFitOutline); }

    void Install() { std::thread(Loop).detach(); }

    void Connect() {
        if (g_asked) return;
        g_asked = true;
        SKEE::InterfaceExchangeMessage msg;
        auto* m = SKSE::GetMessagingInterface();
        if (!m || !m->Dispatch(SKEE::InterfaceExchangeMessage::kMessage_ExchangeInterface, &msg, sizeof(SKEE::InterfaceExchangeMessage*), "SKEE") || !msg.interfaceMap) {
            logger::warn("Anatomica engine: RaceMenu (SKEE) did not answer - the gape script does everything as before");
            return;
        }
        if (auto* p = msg.interfaceMap->QueryInterface("BodyMorph")) {
            g_vMorph = p->GetVersion();
            if (g_vMorph >= 3) g_morph = static_cast<SKEE::IBodyMorphInterface*>(p);
        }
        if (auto* p = msg.interfaceMap->QueryInterface("Override")) {
            g_vOvr = p->GetVersion();
            if (g_vOvr >= 2) g_ovr = static_cast<SKEE::IOverrideInterface*>(p);
        }
        if (auto* p = msg.interfaceMap->QueryInterface("Overlay")) {
            g_vOvl = p->GetVersion();
            if (g_vOvl >= 2) g_ovl = static_cast<SKEE::IOverlayInterface*>(p);
        }
        logger::info("Anatomica engine: RaceMenu BodyMorph v{} {}, Override v{} {}, Overlay v{} -> mode {}", g_vMorph, g_morph ? "used" : "not used",
                     g_vOvr, g_ovr ? "used" : "not used", g_vOvl, Mode());
    }

    void Reset() {
        std::lock_guard lk(g_mx);
        g_st.clear();
        g_ringDone.clear();
        g_ringSwell.clear();
        g_innerSent.clear();   // the inside is sent again once (its overrides are kept with the save)
        { std::lock_guard gl(g_gapeMx); g_gape.clear(); g_during.clear(); }
        g_last = 0.0; g_lastCol = 0.0;
    }

    int Mode() { return g_morph ? ((g_ovr && g_ovl) ? 3 : 1) : 0; }

    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("EngineMode"sv, "AnatomicaNative"sv, EngineModeNative);
    }

    bool TestGape(RE::Actor* a, float dil) {
        if (!a || !g_morph) return false;
        std::lock_guard lk(g_mx);
        auto& s = g_st[a->GetFormID()];
        if (!s.init) { s.init = true; s.sent = -1.0f; }
        s.dil = std::clamp(dil, 0.0f, 1.0f);
        s.ep = std::max(s.ep, 1.0f);
        s.worked = Now();
        s.partnerNear = 0.0;
        s.was = false;
        return true;
    }

    bool SetLooseNow(RE::Actor* a, float v) {
        if (!a || !g_morph) return false;
        if (v <= 0.0f) g_morph->ClearMorph(a, "Anatomica Gape", "AnatomicaLoose");   // gone, not just 0
        else SetMorph(a, "Anatomica Gape", "AnatomicaLoose", v);
        const float gt = RE::Calendar::GetSingleton() ? RE::Calendar::GetSingleton()->GetCurrentGameTime() : 0.0f;
        SetMorph(a, "AGLooseT", K, v > 0.0f ? gt : 0.0f);   // healing counts from now
        g_morph->UpdateModelWeight(a, false);
        return true;
    }
    Engine::During DuringOf(RE::FormID id) {
        std::lock_guard gl(g_gapeMx);
        auto it = g_during.find(id);
        if (it == g_during.end()) return {};
        return {it->second.depth, it->second.hole, it->second.ring, it->second.off, it->second.held};
    }
    bool TestBulge(RE::Actor* a, int shape) {
        if (!a || !g_morph) return false;
        std::lock_guard lk(g_mx);
        auto& s = g_st[a->GetFormID()];
        s.testBulge = Now() + 8.0;
        s.testShape = std::clamp(shape, 1, 5);
        return true;
    }
    float GapeOf(RE::FormID id) {
        std::lock_guard gl(g_gapeMx);
        auto it = g_gape.find(id);
        return it == g_gape.end() ? -1.0f : it->second;
    }

    std::string Status() {
        return std::format("RaceMenu interface: body morphs {}, overrides {}, overlays {} - {}",
                           g_morph ? "yes" : "no", g_ovr ? "yes" : "no", g_ovl ? "yes" : "no",
                           Mode() == 3 ? "gape and colour run in the DLL" : (Mode() == 1 ? "gape and the skin-blend ring run in the DLL, the rest of the colour in the script" : "the script does everything"));
    }

    bool NodeFloat(RE::Actor* a, const char* node, int key, float value) {
        if (!g_ovr || !a) return false;
        PutFloat(a, node, key, value, false);
        return true;
    }
    bool RingDirect() { return g_asked && !(g_ovr && g_ovl); }
    bool NodeRemove(RE::Actor* a, const char* node, int key, int index) {
        if (!g_ovr || !a) return false;
        Remove(a, node, key, index);
        return true;
    }
}

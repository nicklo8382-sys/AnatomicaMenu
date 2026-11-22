#include "SKSEMenuFramework.h"
#include "Lang.h"
#include "OStrandsLink.h"
#include "During.h"
#include "Fluid.h"
#include "Extras.h"
#include "SoftButt.h"
#include "Engine.h"
#include "Colossal.h"
#include "Align.h"
#include "Mu.h"
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
    // 2.6: how far his hips are bent (degrees, 0 = standing, 90 = thighs level as when sitting), both legs averaged.
    // 2.6c: measured against his body's real up (the canal frame's u leans back 54 degrees - standing read as 54 before)
    float HipBend(RE::Actor* act, const Canal& c) {
        auto* root = act->Get3D(false);
        const V up = c.u * 0.586230f + c.f * -0.810145f;   // the mesh's up (0,0,1) in that frame
        float sum = 0.0f; int n = 0;
        for (auto [t, k] : {std::pair{"NPC L Thigh [LThg]", "NPC L Calf [LClf]"}, std::pair{"NPC R Thigh [RThg]", "NPC R Calf [RClf]"}}) {
            auto* tn = Node(root, t); auto* kn = Node(root, k);
            if (!tn || !kn) continue;
            V d = V(kn->world.translate) - V(tn->world.translate);
            const float ld = Len(d);
            if (ld < 1e-3f) continue;
            sum += std::acos(std::clamp(-Dot(d * (1.0f / ld), up), -1.0f, 1.0f)) * 57.29578f; ++n;
        }
        return n ? sum / static_cast<float>(n) : 0.0f;
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
    struct Tip { bool ok = false; float depth = 0.0f; V p; };
    // 2.6m: the schlong's bones base to tip - a werewolf / Vampire Lord / werebear's hangs on its own bones (their
    // skeletons carry the human genital bones too, unused: he never counted as in, so the anus did not react)
    std::vector<const char*> ShaftBones(RE::Actor* o, RE::NiAVObject* root) {
        static const std::vector<const char*> kHuman{"NPC Genitals01 [Gen01]", "NPC Genitals02 [Gen02]", "NPC Genitals03 [Gen03]",
                                                     "NPC Genitals04 [Gen04]", "NPC Genitals05 [Gen05]", "NPC Genitals06 [Gen06]"};
        static const std::vector<std::vector<const char*>> kBeast{
            {"WWD 2", "WWD 3", "WWD 4", "WWD 5", "WWD 6", "WWD 7", "WWD 8", "WWD 9"},
            {"VLDick02", "VLDick03", "VLDick04", "VLDick05", "VLDick06"},
            {"BearD 4", "BearD 5", "BearD 6", "BearD 7", "BearD 8", "BearD 9"}};
        if (o && root && Colossal::BeastForm(o) != 0)
            for (auto& c : kBeast) if (Node(root, c.front()) && Node(root, c.back())) return c;
        return kHuman;
    }
    Tip TipIn(const Canal& c, RE::Actor* o) {
        Tip t;
        auto* root = o ? o->Get3D(false) : nullptr;
        const auto bones = ShaftBones(o, root);
        auto* g6 = Node(root, bones[bones.size() - 1]);
        auto* g5 = Node(root, bones[bones.size() - 2]);
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
    int Inside(const Canal& c, RE::Actor* o, float rad) {
        auto* root = o->Get3D(false);
        if (!root) return 0;
        auto kGen = ShaftBones(o, root);   // 2.6m: tip first
        std::reverse(kGen.begin(), kGen.end());
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

    // ---- 2.6s: steering a beast form's schlong into the hole (PPA never guides werewolves / Vampire Lords / werebears)
    // The gape tick says who is in whom (g_guide: the beast -> the man he is in, how hard to steer); every frame, right after
    // the beast's animation has posed him (his UpdateAnimation, below), his schlong bones are turned a little - at the base,
    // then the middle - so the shaft runs into the hole and along the canal. Only the beast's own schlong bones are touched,
    // and the animation sets them again every frame (if it ever doesn't, the last pose we wrote is undone first).
    // 2.6w: the hole is measured on the main thread (the gape tick) and kept in the beast's own frame - the frame hook runs
    // while the game poses actors on other threads, and reading the other man's bones there (while an outfit change
    // rebuilt them) could crash the game with no log
    struct Guide { RE::FormID bottom = 0; double until = 0.0; float maxDeg = 45.0f; V hL, aL; float sc = 1.0f; };
    std::mutex g_guideMx;
    std::unordered_map<RE::FormID, Guide> g_guide;
    std::atomic<int> g_guideN{0};   // how many are being steered (read every frame without the lock)
    constexpr int kSteer = 10;  // 2.6w: the schlong bones that may be bent (a werewolf has 8)
    struct GuideMem { RE::NiMatrix3 orig[kSteer], wrote[kSteer]; bool did[kSteer]{}; bool have = false; V bend[kSteer]; V h, a; bool hasHole = false; double logAt = -100.0; float worst = 0.0f; };
    std::unordered_map<RE::FormID, GuideMem> g_guideMem;   // under g_guideMx
    RE::NiMatrix3 AxisAngle(const V& axis, float ang) {
        const float c = std::cos(ang), sn = std::sin(ang), t = 1.0f - c;
        const float x = axis.x, y = axis.y, z = axis.z;
        RE::NiMatrix3 m;
        m.entry[0][0] = t * x * x + c;     m.entry[0][1] = t * x * y - sn * z; m.entry[0][2] = t * x * z + sn * y;
        m.entry[1][0] = t * x * y + sn * z; m.entry[1][1] = t * y * y + c;     m.entry[1][2] = t * y * z - sn * x;
        m.entry[2][0] = t * x * z - sn * y; m.entry[2][1] = t * y * z + sn * x; m.entry[2][2] = t * z * z + c;
        return m;
    }
    bool Same(const RE::NiMatrix3& a, const RE::NiMatrix3& b) {
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) if (std::abs(a.entry[i][j] - b.entry[i][j]) > 1e-5f) return false;
        return true;
    }
    // 2.6t: the bend is kept per bone as a small turn in its parent's frame and eased towards what this frame wants (it
    // was applied fresh every frame in 2.6s - with the hole read from the other actor's bones while they were being posed,
    // and switched off and on near the limits, the schlong shook). Off = eased back to none, never cut.
    void SteerNow(RE::Actor* beast, float dt) {
        Guide gd;
        const double now = Now();
        {
            std::lock_guard lk(g_guideMx);
            auto it = g_guide.find(beast->GetFormID());
            if (it == g_guide.end()) return;
            gd = it->second;
        }
        auto* root = beast->Get3D(false);
        if (!root) return;
        const auto chain = ShaftBones(beast, root);
        if (chain.size() < 5 || std::strstr(chain[0], "Genitals")) return;   // only a beast's own schlong bones
        const size_t nc = chain.size();
        // 2.6w (like PPA): the base turns so the shaft goes in through the middle of his hole; every bone past the entrance
        // then lies along his canal (pulled back onto its centre line as it goes), so the part inside him follows the canal
        // instead of cutting across it; the part still outside stays as the animation has it
        const int nb = static_cast<int>(std::min<size_t>(nc - 1, kSteer));   // bones with a segment after them
        RE::NiAVObject* n[kSteer] = {};
        for (int k = 0; k < nb; ++k) { n[k] = Node(root, chain[k]); if (!n[k] || !n[k]->parent) return; }
        auto* tipN = Node(root, chain.back());
        if (!tipN) return;
        std::lock_guard lk(g_guideMx);
        GuideMem& m = g_guideMem[beast->GetFormID()];
        // the animation sets these bones each frame; if it didn't, put back what it last gave (no bending on top of our bend)
        if (m.have) for (int k = 0; k < nb; ++k) if (m.did[k] && Same(n[k]->local.rotate, m.wrote[k])) n[k]->local.rotate = m.orig[k];
        m.have = false;
        for (auto& d : m.did) d = false;
        const bool on = now < gd.until;
        Canal c;
        if (on && gd.sc > 0.0f) {   // his hole, from where the gape tick put it in our frame
            const auto& R = root->world;
            const RE::NiPoint3 h = R * gd.hL.ni();
            V a = V(R.rotate * gd.aL.ni());
            const float la = Len(a);
            if (la > 1e-4f) { c.h = V(h); c.a = a * (1.0f / la); c.sc = gd.sc; c.ok = true; }
        }
        if (c.ok) {   // eased (the tick only measures it a dozen times a second)
            if (!m.hasHole) { m.h = c.h; m.a = c.a; m.hasHole = true; }
            const float kh = 1.0f - std::exp(-std::max(dt, 0.001f) / 0.08f);
            m.h = m.h + (c.h - m.h) * kh;
            V a2 = m.a + (c.a - m.a) * kh;
            const float la2 = Len(a2);
            if (la2 > 1e-4f) m.a = a2 * (1.0f / la2);
            c.h = m.h; c.a = m.a;
        }
        const float ke = 1.0f - std::exp(-std::max(dt, 0.001f) / 0.12f);   // the bend's own easing (~0.12 s)
        RE::NiUpdateData ud{};
        float worst = 0.0f;
        bool any = false;
        bool gate = c.ok;
        if (gate) {   // only with the tip at his hole or in him
            V tip(tipN->world.translate);
            if (Dot(tip - c.h, c.a) < -9.0f * c.sc) gate = false;
        }
        // a world turn taking direction u to direction v, at most cap radians, as a turn in the bone's parent's frame
        auto turnTo = [&](RE::NiAVObject* nd, V u, V v, float cap, float& angOut) -> V {
            angOut = 0.0f;
            const float lu = Len(u), lv = Len(v);
            if (lu < 1e-3f || lv < 1e-3f) return V{};
            u = u * (1.0f / lu); v = v * (1.0f / lv);
            V ax = Cross(u, v);
            const float la = Len(ax);
            const float ang = std::atan2(la, Dot(u, v));
            angOut = ang;
            if (la < 1e-5f || ang < 0.002f) return V{};
            ax = ax * (1.0f / la);
            const RE::NiMatrix3 PT = nd->parent->world.rotate.Transpose();
            return V(PT * ax.ni()) * std::min(ang, cap);
        };
        const float maxR = gd.maxDeg * 0.01745329f;
        for (int k = 0; k < nb; ++k) {
            V want{};
            if (gate) {
                const V p(n[k]->world.translate);
                auto* nxt = (k + 1 < static_cast<int>(nc)) ? Node(root, chain[k + 1]) : nullptr;
                const V q = nxt ? V(nxt->world.translate) : V(tipN->world.translate);
                const float seg = Len(q - p);
                const float dp = Dot(p - c.h, c.a);   // how far past the entrance this bone is (< 0 = still outside)
                if (k == 0) {
                    // the base: the point of the shaft as far from the base as the hole is, put on the hole's centre
                    const V entry = c.h + c.a * (0.4f * c.sc);
                    const float want_d = Len(entry - p);
                    float acc = 0.0f; V at = q;
                    V prevP = p;
                    for (size_t i = 1; i < nc; ++i) {
                        auto* ni = Node(root, chain[i]);
                        if (!ni) break;
                        const V pi(ni->world.translate);
                        const float l = Len(pi - prevP);
                        if (acc + l >= want_d && l > 1e-4f) { at = prevP + (pi - prevP) * ((want_d - acc) / l); acc = want_d; break; }
                        acc += l; prevP = pi; at = pi;
                    }
                    float ang = 0.0f;
                    want = turnTo(n[k], at - p, entry - p, maxR, ang);
                    if (ang > 1.2f) { gate = false; want = V{}; }   // more than ~70 degrees off: not going into him
                    worst = std::max(worst, ang);
                } else if (dp > -0.5f * c.sc && seg > 1e-3f) {
                    // past the entrance: lie along the canal, aimed at the centre line a segment further in
                    const V aimPt = c.h + c.a * (std::max(dp, 0.0f) + seg);
                    float ang = 0.0f;
                    want = turnTo(n[k], q - p, aimPt - p, maxR, ang);   // 2.6x: each as far as the base (it barely held the centre)
                    worst = std::max(worst, ang);
                }
            }
            m.bend[k] = m.bend[k] + (want - m.bend[k]) * ke;
            const float bl = Len(m.bend[k]);
            if (bl < 1e-4f) continue;
            any = true;
            m.orig[k] = n[k]->local.rotate;
            n[k]->local.rotate = AxisAngle(m.bend[k] * (1.0f / bl), bl) * n[k]->local.rotate;
            m.wrote[k] = n[k]->local.rotate;
            m.did[k] = true;
            m.have = true;
            for (size_t i = static_cast<size_t>(k); i < nc; ++i)   // the bones from here to the tip follow at once
                if (auto* qn = Node(root, chain[i])) qn->UpdateWorldData(&ud);
        }
        if (!any && !on) {   // eased all the way back and no longer asked for: forget him
            g_guideMem.erase(beast->GetFormID());
            g_guide.erase(beast->GetFormID());
            g_guideN = static_cast<int>(g_guide.size());
            return;
        }
        m.worst = std::max(m.worst, worst);
        if (now - m.logAt > 20.0 && m.worst > 0.0f) {
            m.logAt = now;
            const char* bn = beast->GetDisplayFullName();
            logger::info("Anatomica guide: {}'s schlong steered into the hole (it pointed up to {:.0f} degrees off the canal; bends at most {:.0f})",
                         bn ? bn : "?", m.worst * 57.29578f, gd.maxDeg);
            m.worst = 0.0f;
        }
    }

    // the game's own call that poses an actor each frame: ours runs right after it
    struct AnimHook {
        static void Char(RE::Character* a, float dt) { g_charAnim(a, dt); if (a && g_guideN.load(std::memory_order_relaxed) > 0) SteerNow(a, dt); }
        static void Player(RE::PlayerCharacter* a, float dt) { g_playerAnim(a, dt); if (a && g_guideN.load(std::memory_order_relaxed) > 0) SteerNow(a, dt); }
        static inline REL::Relocation<decltype(Char)> g_charAnim;
        static inline REL::Relocation<decltype(Player)> g_playerAnim;
        static void Install() {
            REL::Relocation<std::uintptr_t> vc{RE::VTABLE_Character[0]};
            REL::Relocation<std::uintptr_t> vp{RE::VTABLE_PlayerCharacter[0]};
            g_charAnim = vc.write_vfunc(0x7D, Char);
            g_playerAnim = vp.write_vfunc(0x7D, Player);
            logger::info("Anatomica guide: hooked the actor animation update (beast schlongs steered into the hole)");
        }
    };

    // the belly-bulge sliders (2.5: 56): Seg 1-6, Head 1-6, Knot, Flare 1-6, Ridge 1-6, Bead 1-6, Big 1-6, Big Head 1-6,
    // Hook 1-6, Knot 2, Tentacle 1-6
    constexpr int kSeg = 0, kHead = 6, kKnot1 = 12, kFlare = 13, kRidge = 19, kBead = 25, kBig = 31, kBigHead = 37, kHook = 43,
                  kKnot2 = 49, kTent = 50, kBelly = 56, kDeepSeg = 57, kDeepHead = 59, kDeepFlare = 61,
                  kDeepBig = 63, kDeepBigHead = 65, kDeepTent = 67,
                  kDeepRidge = 69, kDeepBead = 71, kDeepHook = 73, kNB = 75;
    const std::vector<std::string>& BulgeNames() {
        static const std::vector<std::string> n = [] {
            std::vector<std::string> v(kNB);
            auto fam = [&](int at, const char* f) { for (int i = 0; i < 6; ++i) v[at + i] = std::format("Anatomica Bulge {} {}", f, i + 1); };
            fam(kSeg, "Seg"); fam(kHead, "Head"); fam(kFlare, "Flare"); fam(kRidge, "Ridge"); fam(kBead, "Bead"); fam(kBig, "Big");
            fam(kBigHead, "Big Head"); fam(kHook, "Hook"); fam(kTent, "Tentacle");
            v[kKnot1] = "Anatomica Bulge Knot"; v[kKnot2] = "Anatomica Bulge Knot 2"; v[kBelly] = "Anatomica Bulge Belly";
            for (int i = 0; i < 2; ++i) {   // 2.5k super deep: two more sections and their heads
                v[kDeepSeg + i] = std::format("Anatomica Bulge Deep Seg {}", i + 1);
                v[kDeepHead + i] = std::format("Anatomica Bulge Deep Head {}", i + 1);
                v[kDeepFlare + i] = std::format("Anatomica Bulge Deep Flare {}", i + 1);
                v[kDeepBig + i] = std::format("Anatomica Bulge Deep Big {}", i + 1);          // 2.5l: Colossal
                v[kDeepBigHead + i] = std::format("Anatomica Bulge Deep Big Head {}", i + 1);
                v[kDeepTent + i] = std::format("Anatomica Bulge Deep Tentacle {}", i + 1);    // 2.5l: Long tentacle
                v[kDeepRidge + i] = std::format("Anatomica Bulge Deep Ridge {}", i + 1);      // 2.5s: Ridged, Segmented, Hooked
                v[kDeepBead + i] = std::format("Anatomica Bulge Deep Bead {}", i + 1);
                v[kDeepHook + i] = std::format("Anatomica Bulge Deep Hook {}", i + 1);
            }
            return v;
        }();
        return n;
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
        int donutStyleSent = -1;                 // 2.5m: which slider carries it (Ring / Donut)
        float depth = -100.0f;                   // how deep the schlong in him is (last tick), for the withdraw
        float pout = 0.0f, canal = 0.0f, bulge[kNB]{};   // During sex page: rim, open inside, belly bulge (BulgeNames)
        float imp[kNB]{};                        // 2.5 imprint: the shape as it last was
        float bulgeReach = 0.0f;                 // 2.5: how deep his strokes have been reaching (the head's travel)
        float grow = 0.0f, fillHold = 0.0f;      // 2.5 build-up: how far grown (0-1), how far the held shaft reaches
        double moveT = 0.0;                      // 2.5 travelling: its phase (radians)
        double lastPush = -100.0; float strokeT = 1.2f;   // 2.5g: when the last thrust began, the time between thrusts
        float tw = 0.0f, twWant = 0.0f; double twNext = 0.0;   // 2.5 twitching
        float bellyG = 0.0f, rndB = 0.0f, hipF = 0.0f;
        double hipLog = 0.0;                     // 2.6: last hip-bend log line        // 2.5f stomach: how far risen (0-1), this stroke's variation
        bool deep = false;                       // 2.5k: this scene goes super deep (rolled when a scene's bulge starts)
        float deepRolledAt = -2.0f;              // 2.5t: the setting it was rolled with (-1 = off): rolled again when it changes
        float pulse = 0.0f, bulgeDil = 0.0f;     // thrust pulse; how worked he was while it bulged (lingering)
        float hole = 0.0f, holeSent = -100.0f;
        float beastHole = 0.0f;                  // 2.6q: the hole opened round a beast form's schlong (Anatomica Gape, AnatomicaDuring)
        RE::FormID beastLogged = 0; double beastLogAt = -100.0; float beastMax = 0.0f, beastR = 0.0f; bool beastGuess = false;
        double inLast = -100.0;                  // last time a schlong was found in him by the bones
        bool held = false;                       // kept in by the hold this tick
        float reachMax = 0.0f;                   // how deep the tip has been lately (the inside stays open behind it)
        float ringPos = 0.0f;                    // the rim carried along by the shaft (units, + = out of him)
        float depthAvg = -100.0f;                // where the strokes are centred (slow average of the depth)
        float rndH = 0.0f, rndA = 0.0f;          // this stroke's bulge randomness
        float lastVel = 0.0f;
        bool ringSwellCleared = false;
        double bulgeLast = -100.0;               // last time it bulged
        float poutSent = -100.0f, canalSent = -100.0f, bulgeSent[kNB];
        St() { std::fill(std::begin(bulgeSent), std::end(bulgeSent), -100.0f); }
        double testBulge = 0.0; int testShape = 1;   // Testing page: show the bulge
    };
    std::mutex g_mx;
    std::unordered_map<RE::FormID, St> g_st;
    std::mutex g_gapeMx;                                 // only for GapeOf (the status page asks under other locks)
    std::unordered_map<RE::FormID, float> g_gape;
    struct DuringNow { float depth = -100.0f, hole = 0.0f, ring = 0.0f, off = -1.0f; bool held = false; };
    std::unordered_map<RE::FormID, DuringNow> g_during;   // for the status window
    std::unordered_map<RE::FormID, Engine::Bulge> g_bulgeNow;   // 2.5o: for SkyrimNet (under g_gapeMx)
    std::string g_lastDeep;                                      // 2.5s: the last super deep roll (During sex page)
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
        Canal c = CanalOf(act);
        if (c.ok) {   // the hole where his rim's bones really carry it (PPA drags it to the shaft), not where the pelvis alone puts it
            RE::NiPoint3 hc, hd;
            if (Fluid::HoleFrame(act, hc, hd)) {
                V a = V(hd) - V(hc);
                const float la = Len(a);
                if (la > 0.5f * c.sc) { c.h = V(hc); c.a = a * (1.0f / la); }
            }
        }
        RE::Actor* partner = nullptr;
        RE::Actor* atMan = nullptr;   // the man whose tip is at his entrance
        bool atEntrance = false;   // his tip is right at the hole (an almost-full pull-out): no left-over gape yet
        float nearest = 1e9f;
        if (c.ok)
            for (auto* o : others) {
                if (o == act) continue;
                if (Align::SchlongOff(o->GetFormID())) continue;   // 2.5x: a bottom whose schlong is switched off doesn't count as in him
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
        // The bones are not where the schlong is: PPA bends the schlong MESH into his hole and leaves the bones where the
        // animation put them, so the bone check misses him (or finds him only on the odd thrust that lines up). The mesh is
        // the truth: whoever has enough of his schlong's actual surface inside the canal is in him (the one already in first).
        s.held = false;
        if (partner) s.inLast = now;
        else if (c.ok) {
            RE::Actor* best = nullptr;
            for (auto* o : others) {
                if (o == act || act->GetPosition().GetDistance(o->GetPosition()) > 90.0f) continue;
                if (Align::SchlongOff(o->GetFormID())) continue;
                if (Fluid::InTube(o, c.h.ni(), c.a.ni(), 3.2f * c.sc, -0.3f * c.sc, 9.0f * c.sc) < 6) continue;
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
            const int style = During::DonutStyle();
            if (s.donutSent < 0.0f || std::abs(s.donut - s.donutSent) > 0.01f || (s.donut == 0.0f && s.donutSent > 0.0f) || style != s.donutStyleSent) {
                {
                    if (!s.ringSwellCleared) { SetMorph(act, "Anatomica Swell", "AnatomicaDuring", 0.0f); s.ringSwellCleared = true; }   // 2.4 test builds used Swell
                    static const char* kRingSl[5] = {"Anatomica Ring", "Anatomica Donut", "Anatomica Rosebud", "Anatomica Lips", "Anatomica Pucker"};
                    for (int i = 0; i < 5; ++i)   // 2.5n: the chosen ring shape (its own strength); the others off
                        if (i == style || i == s.donutStyleSent || s.donutStyleSent < 0)
                            SetMorph(act, kRingSl[i], "AnatomicaDuring", i == style ? s.donut * During::DonutStyleStrength(i) : 0.0f);
                    update = true;
                }
                s.donutSent = s.donut;
                s.donutStyleSent = style;
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
            if (in && c.ok) {
                tip = TipIn(c, in);
                // the real tip: the deepest point of his schlong's mesh inside the canal (PPA bends the mesh, not the bones)
                float deep = 0.0f;
                if (Fluid::InTube(in, c.h.ni(), c.a.ni(), 3.2f * c.sc, -0.3f * c.sc, 14.0f * c.sc, &deep) >= 6) {
                    tip.ok = true;
                    tip.depth = deep / c.sc;
                    tip.p = c.h + c.a * deep;
                }
            }
            const float vel = (tip.ok && s.depth > -50.0f && dt > 0.0f) ? (tip.depth - s.depth) / dt : 0.0f;   // units / s, > 0 = pushing in
            s.depth = tip.ok ? tip.depth : -100.0f;
            s.reachMax = (tip.ok && in) ? std::max(tip.depth, s.reachMax - dt * 4.0f) : std::max(0.0f, s.reachMax - dt * 8.0f);
            auto ease = [&](float& v, float want, float up, float down) {
                v = want > v ? std::min(want, v + dt / up) : std::max(want, v - dt / down);
            };
            // the inside opened round the schlong by his size (the round Anatomica Canal); PPA opens and places the entrance
            ease(s.canal, (partner && dw.canal > 0.0f) ? dw.canal * std::clamp(Size::For(partner).open, 0.7f, 1.4f) : 0.0f, 0.25f, 0.8f);
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
            // belly bulge: six sections that fill one after another from the base, a head at the front of the filled part
            // (six positions, neighbours blended) and knots low down. Each shape has its own slider family (BulgeNames).
            float bw[kNB]{};
            s.pulse = std::max(0.0f, s.pulse - dt / 0.22f);   // a fast push adds a quick extra bump that dies away at once
            if (vel > 4.0f) s.pulse = std::max(s.pulse, dw.pulse * std::clamp((vel - 4.0f) / 12.0f, 0.0f, 1.0f));
            if (vel > 2.0f && s.lastVel <= 2.0f) {   // a new push: this stroke's randomness
                const double gap = now - s.lastPush;
                if (gap > 0.25 && gap < 4.0) s.strokeT += (static_cast<float>(gap) - s.strokeT) * 0.35f;   // 2.5g: his pace
                s.lastPush = now;
                s.rndH = Rand(-1.0f, 1.0f) * dw.bulgeRandom;
                s.rndA = Rand(-1.0f, 1.0f) * dw.bulgeRandom;
                s.rndB = Rand(-1.0f, 1.0f);
            }
            s.lastVel = vel;
            // 2.5: how deep his strokes reach (slowly forgotten): the head travels over the whole stroke, not a fixed 10 units
            if (tip.ok && in) s.bulgeReach = std::max(tip.depth, s.bulgeReach - dt * 1.5f);
            bool bulging = false;
            const bool test = now < s.testBulge;
            if (test || (tip.ok && partner && dw.bulge > 0.0f && During::BulgeFor(act, player) && BulgeShows(act, dw.bulgeThrough))) {
                const int cat = test ? 4 : Size::Cat(partner);
                if (test || !dw.bulgeBig || cat >= 3) {
                    int shape = test ? s.testShape : dw.bulgeShape;
                    During::Knots kn;   // 2.5d/e: his schlong's knots (added to any shape, also a shape picked for everyone) and their strength
                    const int autoShape = partner ? During::AutoShape(partner, &kn) : During::kTip;
                    if (shape <= 0 || shape >= During::kShapeCount) shape = autoShape;
                    if (partner && !test) {   // 2.5z: which shape his partner gives him, and why (once per partner)
                        static std::unordered_map<RE::FormID, RE::FormID> logged;
                        if (auto& lp = logged[act->GetFormID()]; lp != partner->GetFormID()) {
                            lp = partner->GetFormID();
                            std::string why;
                            During::AutoShape(partner, nullptr, &why);
                            const char* an = act->GetDisplayFullName();
                            const char* pn = partner->GetDisplayFullName();
                            logger::info("Anatomica bulge: {} from {} - {} ({})", an ? an : "?", pn ? pn : "?", During::ShapeName(shape), why);
                        }
                    }
                    // how far in, past "shows from": 0 .. 1 = the whole path filled
                    const float range = std::max(10.0f, s.bulgeReach - dw.bulgeStart);
                    float p = test ? 0.55f + 0.45f * std::sin(static_cast<float>(now) * 1.6f)
                                   : std::clamp((tip.depth - dw.bulgeStart) / range, 0.0f, 1.0f);
                    // travelling: up and back at its own pace - 100% = once per thrust (his pace), kept when he is still
                    s.moveT += dt * 6.2831853 * dw.travelSpeed / std::clamp(s.strokeT, 0.35f, 4.0f);
                    if (s.moveT > 6283.1853) s.moveT -= 6283.1853;
                    if (dw.travel[shape]) p = 0.5f - 0.5f * std::cos(static_cast<float>(s.moveT));
                    if (dw.twitch[shape]) {
                        if (now >= s.twNext) { s.twWant = Rand(-0.14f, 0.14f); s.twNext = now + Rand(0.25f, 1.3f); }
                        s.tw += (s.twWant - s.tw) * std::min(1.0f, dt / 0.04f);   // a quick jump...
                        s.twWant *= std::max(0.0f, 1.0f - dt / 0.18f);             // ...that settles again
                        p = std::clamp(p + s.tw, 0.0f, 1.0f);
                    }
                    float A = (test ? 0.6f : dw.bulge * std::clamp(Size::For(partner).open, 0.7f, 1.5f) * Size::BeastBulge(partner)) *
                              (1.0f + s.pulse) * (1.0f + 0.4f * s.rndA);
                    if (dw.build[shape] && !test) {   // build-up: grows to the full shape over the build time
                        s.grow = std::min(1.0f, s.grow + dt / std::max(1.0f, dw.buildTime));
                        A *= 0.08f + 0.92f * Smooth(s.grow);
                    }
                    const float deepSet = dw.deep ? dw.deepChance : -1.0f;
                    if (now - s.bulgeLast > 30.0 || deepSet != s.deepRolledAt) {   // a new scene (or the setting changed mid-scene): super deep?
                        s.deepRolledAt = deepSet;
                        s.deep = dw.deep && Rand(0.0f, 1.0f) < dw.deepChance;
                        if (dw.deep) {
                            const char* nm = act->GetDisplayFullName();
                            std::lock_guard gl(g_gapeMx);
                            g_lastDeep = std::format("{}'s scene {} super deep ({:.0f}% chance).", nm ? nm : "?", s.deep ? "went" : "did not go", dw.deepChance * 100.0f);
                        }
                    }
                    int seg = kSeg, head = kHead, knots = 0;
                    float segK = 1.0f, headK = 0.6f, wob = 0.0f;
                    bool blend = true;   // shaft and head mixed as one (the head keeps its size as it slides)
                    switch (shape) {
                        case During::kTip: segK = dw.outline * 0.4f; headK = 1.0f; break;
                        case During::kHorse: segK = 1.15f; head = kFlare; headK = 1.2f; break;
                        case During::kKnot: knots = 1; break;
                        case During::kTentacle: segK = 0.65f; headK = 0.5f; wob = 0.5f; break;
                        case During::kRidged: seg = kRidge; break;
                        case During::kSegmented: seg = kBead; head = -1; blend = false; break;
                        case During::kColossal: seg = kBig; head = kBigHead; headK = 1.0f; break;
                        case During::kHooked: head = kHook; headK = 1.0f; break;
                        case During::kDoubleKnot: knots = 2; break;
                        case During::kBeaded: seg = kTent; head = -1; blend = false; break;   // 2.5c: sections, like the shaft
                        default: break;   // the whole shaft
                    }
                    segK *= dw.shaftK * kn.shaftK;   // 2.5s: the During sex page's strength x this schlong's own
                    headK *= dw.headK * kn.headK;
                    // 2.5k super deep: two more sections towards his chest (shapes built on the plain shaft, round or flared head)
                    // (2.5l: Colossal and Long tentacle too, on their own longer paths)
                    // (2.5s: every shape - Ridged, Segmented and Hooked have their own deep sections too)
                    const bool deepOk = test ? dw.deep : s.deep;
                    const int NS = deepOk ? 8 : 6;
                    const int deepSeg = seg == kBig ? kDeepBig : seg == kTent ? kDeepTent : seg == kRidge ? kDeepRidge : seg == kBead ? kDeepBead : kDeepSeg;
                    const int deepHead = head == kFlare ? kDeepFlare : head == kBigHead ? kDeepBigHead : head == kHook ? kDeepHook : kDeepHead;
                    auto segAt = [&](int k) { return k < 6 ? seg + k : deepSeg + (k - 6); };
                    // hooked, super deep: its normal end (Hook 6, with the hook) becomes the plain round head - the hook is at the deep end
                    auto headAt = [&](int h) { return (head == kHook && deepOk && h == 5) ? kHead + 5 : h < 6 ? head + h : deepHead + (h - 6); };
                    auto headF = [&](int h) { return (head == kHook && deepOk && h == 5) ? 0.6f : 1.0f; };
                    const float NSf = static_cast<float>(NS);
                    float filled = std::clamp(p * NSf + s.rndH * 0.6f, 0.0f, NSf);
                    if (wob > 0.0f) filled = std::clamp(filled + wob * std::sin(static_cast<float>(now) * 2.3f + static_cast<float>(act->GetFormID() % 7)), 0.0f, NSf);
                    // build-up, hold when full: once grown, the shaft stays as far as it got and only the head moves
                    const bool hold = dw.build[shape] && dw.holdFull && s.grow >= 1.0f && !test;
                    if (hold) s.fillHold = std::max(s.fillHold, filled); else s.fillHold = 0.0f;
                    if (A > 0.0f && filled > 0.0f) {
                        const float hi = filled - 0.6f;   // head k sits 0.6 of a section back from the end of section k + 1
                        int h0 = 0; float fr = 0.0f;
                        if (hi > 0.0f) { h0 = std::min(NS - 2, static_cast<int>(hi)); fr = std::clamp(hi - static_cast<float>(h0), 0.0f, 1.0f); }
                        if (seg >= 0) {
                            if (hold || !blend || head < 0) {
                                const float f = hold ? std::max(filled, s.fillHold) : filled;
                                for (int k = 0; k < NS; ++k) bw[segAt(k)] = A * segK * std::clamp(f - static_cast<float>(k), 0.0f, 1.0f);
                            } else if (hi <= 0.0f) bw[seg] = A * segK * filled;
                            else {   // the mix each head was built against: sections before it full, its own at 0.6
                                for (int k = 0; k < h0; ++k) bw[segAt(k)] = A * segK;
                                bw[segAt(h0)] = A * segK * (0.6f + 0.4f * fr);
                                if (h0 + 1 < NS) bw[segAt(h0 + 1)] = A * segK * 0.6f * fr;
                            }
                        }
                        if (head >= 0) {
                            if (hi <= 0.0f) bw[head] = A * headK * std::clamp(filled / 0.6f, 0.0f, 1.0f);
                            else {
                                // 2.5c hooked: the last position carries the hook - bring it in only over the end of the stroke
                                const float fh = (shape == During::kHooked && h0 == NS - 2) ? fr * fr * fr : fr;
                                bw[headAt(h0)] = A * headK * headF(h0) * (1.0f - fh);
                                bw[headAt(h0 + 1)] = A * headK * headF(h0 + 1) * fh;
                            }
                        }
                        // knots: from the shape, or ticked for his schlong (2.5d); each its own strength
                        if (knots >= 1 || (kn.mask & 1)) bw[kKnot1] = A * kn.k1 * std::clamp((filled - 1.8f) / 0.8f, 0.0f, 1.0f);   // once it is in (2.5j: knot at 2.0)
                        if (knots >= 2 || (kn.mask & 2)) bw[kKnot2] = A * kn.k2 * std::clamp((filled - 3.6f) / 0.8f, 0.0f, 1.0f);
                        // 2.5f stomach: the whole lower belly rises over the scene (its own time), varying stroke to stroke
                        if (dw.belly > 0.0f) {
                            s.bellyG = std::min(1.0f, s.bellyG + dt / std::max(1.0f, dw.bellyTime));
                            bw[kBelly] = dw.bulge * dw.belly * Smooth(s.bellyG) * std::max(0.0f, 1.0f + 0.5f * dw.bellyRandom * s.rndB);
                        }
                        // 2.6: legs drawn up - the skin above his groin folds, so the bottom of the bulge fades (eased, not snapped)
                        {
                            const float bend = c.ok ? HipBend(act, c) : 0.0f;
                            const float want = dw.hipFade ? Smooth(std::clamp((bend - dw.hipFrom) / 40.0f, 0.0f, 1.0f)) : 0.0f;
                            if (!test && now - s.hipLog > 15.0) {   // for tuning "Starts fading at a hip bend of"
                                s.hipLog = now;
                                const char* an = act->GetDisplayFullName();
                                logger::info("Anatomica bulge: {} hip bend {:.0f} deg, bottom of the bulge faded {:.0f}%", an ? an : "?", bend, want * 100.0f);
                            }
                            s.hipF += (want - s.hipF) * std::min(1.0f, dt / 0.3f);
                            const float hf = s.hipF;
                            if (hf > 0.001f) {
                                if (seg >= 0) { bw[segAt(0)] *= 1.0f - hf; bw[segAt(1)] *= 1.0f - 0.5f * hf; }
                                if (head >= 0) { bw[headAt(0)] *= 1.0f - hf; bw[headAt(1)] *= 1.0f - 0.3f * hf; }
                                bw[kBelly] *= 1.0f - 0.5f * hf;
                                bw[kKnot1] *= 1.0f - 0.6f * hf;   // 2.6c: the low knot folds with it too
                            }
                        }
                        bulging = true;
                        {   // 2.5o: what SkyrimNet can say about it
                            std::lock_guard gl(g_gapeMx);
                            auto& b = g_bulgeNow[act->GetFormID()];
                            if (now - b.since > 30.0 && !b.on) { b.roundMax = 0.0f; b.roundDeep = false; }
                            b.on = true; b.shape = shape; b.reach = filled; b.roundMax = std::max(b.roundMax, filled);
                            b.deep = deepOk; b.roundDeep = b.roundDeep || (deepOk && filled > 6.2f);
                            b.amount = A; b.belly = bw[kBelly]; b.imprint = 0.0f; b.partner = partner ? partner->GetFormID() : 0; b.since = now;
                        }
                    }
                }
            }
            if (bulging) {
                s.bulgeLast = now, s.bulgeDil = std::max(s.bulgeDil, s.dil);
                std::copy(std::begin(bw), std::end(bw), std::begin(s.imp));   // 2.5 imprint: the shape as it last was
            }
            const float linger = (now - s.bulgeLast < 30.0) ? std::max(0.8f, dw.linger * (0.5f + Smooth(s.bulgeDil))) : 0.8f;
            if (!bulging && now - s.bulgeLast > 30.0) s.bulgeDil = 0.0f;
            if (!bulging && now - s.bulgeLast > 3.0) { s.grow = 0.0f; s.fillHold = 0.0f; }
            if (!bulging) {   // 2.5o: no longer showing (the round's most is kept a while for memories)
                std::lock_guard gl(g_gapeMx);
                if (auto it = g_bulgeNow.find(act->GetFormID()); it != g_bulgeNow.end()) {
                    it->second.on = false;
                    float im = 0.0f;
                    if (dw.imprint && dw.bulge > 0.0f) {
                        const float age = static_cast<float>(now - s.bulgeLast) / (60.0f * std::max(0.5f, dw.imprintMin));
                        im = age < 1.0f ? dw.imprintAmt * (1.0f - Smooth(age)) : 0.0f;
                    }
                    it->second.imprint = im;
                }
            }
            if (!bulging) s.bellyG = std::max(0.0f, s.bellyG - dt / std::max(2.0f, 2.0f * dw.bellyTime));   // settles slowly   // build-up starts again next time
            if (!bulging && !(tip.ok && partner) && dw.imprint && dw.bulge > 0.0f) {   // 2.5 imprint: a faint copy that fades over the minutes set
                const float age = static_cast<float>(now - s.bulgeLast) / (60.0f * std::max(0.5f, dw.imprintMin));
                const float k = age < 1.0f ? dw.imprintAmt * (1.0f - Smooth(age)) : 0.0f;
                for (int i = 0; i < kNB; ++i) bw[i] = s.imp[i] * k;
            }
            bw[kBelly] = std::max(bw[kBelly], OStrandsLink::Belly(act->GetFormID()));   // 2.6e: full of what OStrands put in him
            for (int i = 0; i < kNB; ++i) ease(s.bulge[i], bw[i], 0.08f, bulging ? 0.25f : linger);
            // (the first time each session it is always sent: a save made mid-scene keeps the morphs it had)
            auto send = [&](const char* morph, float v, float& sent) {
                if (sent < -50.0f || std::abs(v - sent) > 0.02f || (std::abs(v) < 1e-4f && std::abs(sent) > 1e-4f)) {
                    SetMorph(act, morph, "AnatomicaDuring", v);
                    update = true;
                    sent = v;
                }
            };
            // 2.6q: PPA leaves werewolves / Vampire Lords / werebears out of its scenes (it never tracks them as a performer),
            // so nothing opened him round one. Anatomica opens the hole itself (the Anatomica Gape shape on its own key): as
            // wide as his schlong is where it passes through the hole now - a knot or a thick base opens it further as it
            // goes in - and closing as he pulls out. The gape shape moves the four rim bones out 2 units per 1.0.
            {
                float want = 0.0f;
                RE::Actor* bo = partner ? partner : atMan;
                if (dw.beastGuide && c.ok && bo && Colossal::BeastForm(bo) != 0) {   // 2.6s: steer him in, for the next half second
                    std::lock_guard gl(g_guideMx);
                    Guide gd2{act->GetFormID(), now + 0.5, dw.beastGuideMax};
                    if (auto* br = bo->Get3D(false)) {   // his hole in the beast's own frame (the frame hook must not read our bones)
                        const RE::NiTransform inv = br->world.Invert();
                        gd2.hL = V(inv * c.h.ni());
                        // 2.6x: which way inside him: up towards his stomach (his Spine1, a little to the front), as PPA's canal
                        // runs - the canal axis read off his rim pointed down at his pelvis (the schlong was bent low in him)
                        V dir = c.a;
                        if (auto* sp = Node(act->Get3D(false), "NPC Spine1 [Spn1]")) {
                            const V target = V(sp->world.translate) + c.f * (-2.5f * c.sc);   // (f points out of his back)
                            const V d = target - c.h;
                            const float ld = Len(d);
                            if (ld > 3.0f * c.sc && Dot(d, c.a) > -0.2f * ld) dir = d * (1.0f / ld);
                        }
                        gd2.aL = V(inv.rotate * dir.ni());
                        gd2.sc = c.sc;
                    } else gd2.sc = 0.0f;
                    g_guide[bo->GetFormID()] = gd2;
                    g_guideN = static_cast<int>(g_guide.size());
                }
                if (dw.beastOpen && c.ok && bo && Colossal::BeastForm(bo) != 0) {
                    float r = Fluid::GirthAt(bo, (c.h + c.a * (0.4f * c.sc)).ni(), c.a.ni());
                    bool guess = false;
                    if (r > 4.5f * c.sc) r = -1.0f;   // 2.6s: a slice across him at a slant (he was off to the side) reads far too thick
                    if (r < 0.0f && partner) { r = 2.5f * c.sc; guess = true; }   // his mesh can't be read: a werewolf's usual thickness
                    if (r > 0.0f) {
                        constexpr float kRest = 0.5f;   // the closed hole's own radius (mesh units)
                        want = std::clamp((r / c.sc * dw.beastFit - kRest) / 2.0f, 0.0f, 4.0f);   // 2.6r: up to 300% fit
                        s.beastMax = std::max(s.beastMax, want);
                        s.beastR = std::max(s.beastR, r / c.sc);
                        s.beastGuess = guess;
                    }
                    if (bo->GetFormID() != s.beastLogged || now - s.beastLogAt > 20.0) {
                        if (s.beastMax > 0.0f) {
                            const char* an = act->GetDisplayFullName();
                            const char* bn = bo->GetDisplayFullName();
                            logger::info("Anatomica beast open: {} round {} ({:08X}) - PPA leaves him out, so Anatomica opens the hole: "
                                         "schlong up to {:.1f} units across at the hole{}, opened up to {:.2f} (fit {:.0f}%)",
                                         an ? an : "?", bn ? bn : "?", bo->GetFormID(), s.beastR * 2.0f,
                                         s.beastGuess ? " (his mesh could not be read: a usual werewolf size)" : "", s.beastMax, dw.beastFit * 100.0f);
                            s.beastLogged = bo->GetFormID(); s.beastLogAt = now; s.beastMax = 0.0f; s.beastR = 0.0f;
                        }
                    }
                }
                ease(s.beastHole, want, 0.15f, want > 0.0f ? 0.3f : 0.5f);   // opens on the push, closes on the pull-out
                if (s.beastHole < 0.005f && want == 0.0f) s.beastHole = 0.0f;
            }
            send("Anatomica Gape", s.beastHole, s.holeSent);   // 0 unless a beast form is in him (2.4 test builds used it for every man)
            send("Anatomica Canal", s.canal, s.canalSent);
            send("Anatomica Pout", s.ringPos, s.poutSent);
            for (int i = 0; i < kNB; ++i) send(BulgeNames()[i].c_str(), s.bulge[i], s.bulgeSent[i]);
            { std::lock_guard gl(g_gapeMx); g_during[act->GetFormID()] = {tip.ok ? tip.depth : -100.0f, s.canal, s.ringPos, -1.0f, s.held}; }
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

    void CumOverlayOne(RE::Actor* a, int level, int col, float alpha);   // (below: it needs the paint-slot helpers)

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
    // 2.6i: cum on his skin round the anus, as a RaceMenu body paint whose texture path has "cumoverlays" in it, so other
    // mods that look for cum overlays (DDF's cum conditions, cum-overlay mods) see it. level 0 = take it off.
    // 2.6j: with RaceMenu's override interface (v2+) it is kept with the save; otherwise (older RaceMenu, as on Nick's game)
    // it goes straight on the paint slot in the 3D, the way the ring does - redone every few seconds in case the body is rebuilt.
    void CumOverlayOne(RE::Actor* a, int level, int col, float alpha) {
        (void)col;   // (the game does not draw a tint on these paints; the texture is cream white)
        if (!a) return;
        static const char* kTex[3] = {"textures\\actors\\character\\overlays\\default.dds",
                                      "textures\\actors\\character\\male\\Anatomica\\cumoverlays\\anatomica_anal_1.dds",
                                      "textures\\actors\\character\\male\\Anatomica\\cumoverlays\\anatomica_anal_2.dds"};
        level = std::clamp(level, 0, 2);
        alpha = std::clamp(alpha, 0.05f, 1.0f);
        if (g_ovr && g_ovl) {
            if (level > 0 && !g_ovl->HasOverlays(a)) g_ovl->AddOverlays(a, true);
            const std::uint32_t n = g_vOvl >= 2 ? g_ovl->GetOverlayCount(SKEE::IOverlayInterface::OverlayType::Normal, SKEE::IOverlayInterface::OverlayLocation::Body) : 6;
            int found = -1, freeSlot = -1;
            for (std::uint32_t i = 0; i < n && i < 64; ++i) {
                const std::string node = std::format("Body [Ovl{}]", i);
                const std::string tex = Lower(GetStr(a, node.c_str(), 9, 0));
                if (tex.find("anatomica\\cumoverlays") != std::string::npos) { found = static_cast<int>(i); break; }
                if (freeSlot < 0 && (tex.empty() || tex.find("default.dds") != std::string::npos)) freeSlot = static_cast<int>(i);
            }
            if (level <= 0) {
                if (found < 0) return;
                const std::string node = std::format("Body [Ovl{}]", found);
                Remove(a, node.c_str(), 9, 0); Remove(a, node.c_str(), 8, -1);
                PutStr(a, node.c_str(), 9, 0, kTex[0], false); PutFloat(a, node.c_str(), 8, 0.0f, false);
                return;
            }
            const int slot = found >= 0 ? found : freeSlot;
            if (slot < 0) return;
            const std::string node = std::format("Body [Ovl{}]", slot);
            PutStr(a, node.c_str(), 9, 0, kTex[level]);
            PutFloat(a, node.c_str(), 8, alpha);
            return;
        }
        auto* root = a->Get3D(false);
        if (!root) return;
        RE::BSGeometry* ours = nullptr;
        RE::BSGeometry* freeSlot = nullptr;
        bool any = false;
        for (int i = 0; i < 32; ++i) {
            auto* o = Node(root, std::format("Body [Ovl{}]", i).c_str());
            auto* geo = o ? o->AsGeometry() : nullptr;
            if (!geo) continue;
            any = true;
            const std::string d = DiffuseOf(geo);
            if (d.find("anatomica\\cumoverlays") != std::string::npos) { ours = geo; break; }
            if (!freeSlot && (d.empty() || d.find("default.dds") != std::string::npos)) freeSlot = geo;
        }
        if (level <= 0) {
            if (ours) RingLook(ours, kTex[0], -1, 0.0f);
            return;
        }
        if (!ours && !any) { AskOverlays(a); return; }   // no paint slots yet: RaceMenu adds them, next pass puts it on
        RE::BSGeometry* geo = ours ? ours : freeSlot;
        if (geo) RingLook(geo, kTex[level], -1, alpha);
    }

    // 2.6j: OBlush's face blush (its "dd2blush" paint on the face) - made stronger for a moment, then put back
    struct BlushNow { RE::NiPointer<RE::BSGeometry> geo; float base = -1.0f; };
    std::unordered_map<RE::FormID, BlushNow> g_blush;
    bool BlushBoostOne(RE::Actor* a, float add) {
        auto* root = a ? a->Get3D(false) : nullptr;
        if (!root) return false;
        auto& b = g_blush[a->GetFormID()];
        if (add <= 0.0f) {   // back to OBlush's own strength
            if (b.geo && b.base >= 0.0f) if (auto* l = b.geo->lightingShaderProp_cast()) l->SetMaterialAlpha(b.base);
            g_blush.erase(a->GetFormID());
            return true;
        }
        if (!b.geo) {
            for (int i = 0; i < 32 && !b.geo; ++i) {
                auto* o = Node(root, std::format("Face [Ovl{}]", i).c_str());
                auto* geo = o ? o->AsGeometry() : nullptr;
                if (geo && DiffuseOf(geo).find("dd2blush") != std::string::npos) b.geo = RE::NiPointer<RE::BSGeometry>(geo);
            }
            if (!b.geo) { g_blush.erase(a->GetFormID()); return false; }   // OBlush is not blushing him right now
            auto* m = MatOf(b.geo.get());
            b.base = m ? m->materialAlpha : 0.5f;
        }
        if (auto* l = b.geo->lightingShaderProp_cast()) l->SetMaterialAlpha(std::clamp(b.base + add, 0.0f, 1.0f));
        return true;
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
            // 2.6k: his ring colour drifts a little as he flushes and calms (a new 1024 texture every 3 s made the game hitch);
            // small drifts (8 of 255 a channel) keep the ring he has
            auto close = [](int x, int y) {
                if (x == y) return true;
                if (x < 0 || y < 0) return false;
                for (int sh : {16, 8, 0}) if (std::abs(((x >> sh) & 255) - ((y >> sh) & 255)) > 8) return false;
                return true;
            };
            if (same && d.root == root && close(d.col, w.col) && d.swap == w.swap && close(d.add, w.add) && d.tex == texSet && (!d.geo || (lsp && lsp->material == d.mat))) return;
        }
        RingDone done{root, nullptr, nullptr, w.col, w.swap, w.add, texSet};
        struct Mark { RingDone& d; RE::FormID id; ~Mark() { if (d.geo) if (auto* l = d.geo->lightingShaderProp_cast()) d.mat = l->material; g_ringDone[id] = d; } } mark{done, a->GetFormID()};
        if (!Node(root, "3BA_AnusRim")) return;
        if (Colossal::BeastForm(a) != 0) return;   // 2.6m: a beast body's paint slots are laid out for its fur - the ring painted white patches on his arms and legs
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
            if (!freeSlot) {   // 2.6l: right after loading RaceMenu may still be filling its slots - look again a few times
                static std::unordered_map<RE::FormID, int> tries;
                if (++tries[a->GetFormID()] <= 10) { done.root = nullptr; if (tries[a->GetFormID()] == 1) RingReport(a, "no free RaceMenu paint slot yet: looking again", nullptr, w); return; }
                RingReport(a, "no free RaceMenu paint slot for a ring", nullptr, w); return;
            }
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
        Align::Tick(all, dt, now);   // 2.5v: male / futa bottoms in vaginal animations, the bottom's schlong in PPA
        Mu::Tick(all, now);          // 2.5v: MuDynamicNormalMap updates when the bulge / gape has really changed
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

    void Install() { std::thread(Loop).detach(); AnimHook::Install(); }
    void BodyMorph(RE::Actor* a, const char* name, const char* key, float v) { if (g_morph && a) g_morph->SetMorph(a, name, key, v); }
    float MorphSum(RE::Actor* a, const char* name) { return (g_morph && a) ? g_morph->GetBodyMorphs(a, name) : 0.0f; }
    void ClearMorphKey(RE::Actor* a, const char* key) { if (g_morph && a) g_morph->ClearBodyMorphKeys(a, key); }
    void UpdateBody(RE::Actor* a) { if (g_morph && a) g_morph->UpdateModelWeight(a, false); }
    void CumOverlay(RE::Actor* a, int level, int col, float alpha) { CumOverlayOne(a, level, col, alpha); }
    // 2.6j: a Vampire Lord's thrall - a faint red glow over his skin (a RaceMenu body paint slot of its own; 0 = off)
    bool BondGlow(RE::Actor* a, float strength) {
        if (!g_ovr || !g_ovl || !a) return false;
        static const char* kGlow = "textures\\actors\\character\\male\\Anatomica\\bond_glow.dds";
        static const char* kNone = "textures\\actors\\character\\overlays\\default.dds";
        strength = std::clamp(strength, 0.0f, 1.0f);
        if (strength > 0.0f && !g_ovl->HasOverlays(a)) g_ovl->AddOverlays(a, true);
        const std::uint32_t n = g_vOvl >= 2 ? g_ovl->GetOverlayCount(SKEE::IOverlayInterface::OverlayType::Normal, SKEE::IOverlayInterface::OverlayLocation::Body) : 6;
        int found = -1, freeSlot = -1;
        for (std::uint32_t i = 0; i < n && i < 64; ++i) {
            const std::string node = std::format("Body [Ovl{}]", i);
            const std::string tex = Lower(GetStr(a, node.c_str(), 9, 0));
            if (tex.find("anatomica\\bond_glow") != std::string::npos) { found = static_cast<int>(i); break; }
            if (freeSlot < 0 && (tex.empty() || tex.find("default.dds") != std::string::npos)) freeSlot = static_cast<int>(i);
        }
        if (strength <= 0.0f) {
            if (found < 0) return true;
            const std::string node = std::format("Body [Ovl{}]", found);
            PutStr(a, node.c_str(), 9, 0, kNone, false); PutFloat(a, node.c_str(), 8, 0.0f, false); PutFloat(a, node.c_str(), 1, 0.0f, false);
            return true;
        }
        const int slot = found >= 0 ? found : freeSlot;
        if (slot < 0) return false;
        const std::string node = std::format("Body [Ovl{}]", slot);
        PutStr(a, node.c_str(), 9, 0, kGlow, false);
        PutInt(a, node.c_str(), 7, 0x9E1010, false);             // tint
        PutFloat(a, node.c_str(), 8, 0.12f + 0.25f * strength, false);   // alpha
        PutInt(a, node.c_str(), 0, 0xFF1A1A, false);             // emissive colour
        PutFloat(a, node.c_str(), 1, 0.8f * strength, false);    // emissive strength
        return true;
    }
    bool BlushBoost(RE::Actor* a, float add) { return BlushBoostOne(a, add); }
    std::vector<RE::FormID> DuringIds() {
        std::lock_guard gl(g_gapeMx);
        std::vector<RE::FormID> v;
        v.reserve(g_during.size());
        for (auto& [id, d] : g_during) v.push_back(id);
        return v;
    }

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
    std::string LastDeepRoll() { std::lock_guard gl(g_gapeMx); return g_lastDeep; }
    Engine::Bulge BulgeOf(RE::FormID id) {
        std::lock_guard gl(g_gapeMx);
        auto it = g_bulgeNow.find(id);
        return it == g_bulgeNow.end() ? Engine::Bulge{} : it->second;
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
        s.testShape = std::clamp(shape, 1, ::During::kShapeCount - 1);
        return true;
    }
    float GapeOf(RE::FormID id) {
        std::lock_guard gl(g_gapeMx);
        auto it = g_gape.find(id);
        return it == g_gape.end() ? -1.0f : it->second;
    }

    std::string Status() {
        auto yn = [](bool b) { return std::string(Lang::T(b ? "yes" : "no")); };
        return Lang::Fmt("RaceMenu interface: body morphs {}, overrides {}, overlays {} - {}", yn(g_morph), yn(g_ovr), yn(g_ovl),
                         std::string(Lang::T(Mode() == 3 ? "gape and colour run in the DLL" : (Mode() == 1 ? "gape and the skin-blend ring run in the DLL, the rest of the colour in the script" : "the script does everything"))));
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

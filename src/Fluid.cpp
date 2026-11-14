#include "Fluid.h"
#include "Extras.h"
#include "Colour.h"
#include "Drip.h"
#include "Engine.h"
#include "Wink.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <format>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------------------------------------------
//  Runtime cum (see Fluid.h).
//
//  Pieces ("ropes") are chains of up to 16 simulated points:
//    Strand  - hangs from a point (the lip of his anus, or the tip of a schlong). Grows, sags, stretches like goo
//              (its rest length creeps toward the stretched length), drops fall off its end.
//    String  - held at both ends (his lip and the partner's tip): thins as it is pulled long, breaks into two
//              strands that spring back, the partner's half stays on his tip and drips.
//    Free    - drops and pieces of a stream in flight: fall, stick to skin, reach the floor (puddle).
//    Rivulet - runs over the body surface (stuck to the skin, slides down along it, leaves a thin wet trail
//              that dries). Also the beads of the wet film around the hole and the streaks on a schlong.
//  The body surface is the real skinned body mesh, read back from the game (morphs included) and posed every
//  frame with the bones; the lip is the inner edge of the rim shape (3BA_AnusRim), so everything sits where the
//  opening really is, gaping or not.
// ---------------------------------------------------------------------------------------------------------------
namespace {
    using clk = std::chrono::steady_clock;
    double Now() { return std::chrono::duration<double>(clk::now().time_since_epoch()).count(); }
    std::mt19937 g_rng{std::random_device{}()};
    float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }

    // ------------------------------------------------------------------------------------------- math
    struct V3 {
        float x = 0, y = 0, z = 0;
        V3() = default;
        V3(float a, float b, float c) : x(a), y(b), z(c) {}
        V3(const RE::NiPoint3& p) : x(p.x), y(p.y), z(p.z) {}
        RE::NiPoint3 ni() const { return {x, y, z}; }
        V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        V3 operator*(float s) const { return {x * s, y * s, z * s}; }
        V3 operator/(float s) const { return {x / s, y / s, z / s}; }
        V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
        V3& operator-=(const V3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
        V3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    };
    float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    V3 Cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    float Len(const V3& a) { return std::sqrt(Dot(a, a)); }
    V3 Norm(const V3& a, const V3& fb = {0, 0, 1}) { const float l = Len(a); return l > 1e-6f ? a / l : fb; }
    V3 Lerp(const V3& a, const V3& b, float t) { return a + (b - a) * t; }

    // world = R * (s * v) + t   (NiTransform semantics)
    struct Xf {
        float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        V3 t;
        float s = 1.0f;
        Xf() = default;
        Xf(const RE::NiTransform& n) {
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r[i][j] = n.rotate.entry[i][j];
            t = V3(n.translate); s = n.scale;
        }
        V3 Rot(const V3& v) const { return {r[0][0] * v.x + r[0][1] * v.y + r[0][2] * v.z, r[1][0] * v.x + r[1][1] * v.y + r[1][2] * v.z, r[2][0] * v.x + r[2][1] * v.y + r[2][2] * v.z}; }
        V3 RotT(const V3& v) const { return {r[0][0] * v.x + r[1][0] * v.y + r[2][0] * v.z, r[0][1] * v.x + r[1][1] * v.y + r[2][1] * v.z, r[0][2] * v.x + r[1][2] * v.y + r[2][2] * v.z}; }
        V3 Apply(const V3& v) const { return Rot(v * s) + t; }
        V3 Axis(int c) const { return {r[0][c], r[1][c], r[2][c]}; }
    };
    Xf Mul(const Xf& a, const Xf& b) {
        Xf o;
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
        o.s = a.s * b.s;
        o.t = a.Apply(b.t);
        return o;
    }

    // positions are full 32-bit floats when the slot before the next attribute is 16 bytes, whatever the flag says:
    // skinned shapes (schlongs, the body) come without VF_FULLPREC but store full floats - read as half floats they
    // landed hundreds of units off and the shaft was never measured
    bool FullPos(const RE::BSGraphics::VertexDesc& d, std::uint32_t stride) {
        if (d.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC)) return true;
        const std::uint32_t oP = d.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION);
        std::uint32_t next = stride;
        for (auto a : {RE::BSGraphics::Vertex::VA_TEXCOORD0, RE::BSGraphics::Vertex::VA_TEXCOORD1, RE::BSGraphics::Vertex::VA_NORMAL,
                       RE::BSGraphics::Vertex::VA_BINORMAL, RE::BSGraphics::Vertex::VA_COLOR, RE::BSGraphics::Vertex::VA_SKINNING,
                       RE::BSGraphics::Vertex::VA_EYEDATA}) {
            if (!d.HasFlag(static_cast<RE::BSGraphics::Vertex::Flags>(1u << static_cast<unsigned>(a)))) continue;
            const std::uint32_t off = d.GetAttributeOffset(a);
            if (off > oP && off < next) next = off;
        }
        return next - oP >= 16;
    }
    float Half(std::uint16_t h) {
        const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
        float v;
        if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
        else if (e == 31) v = m ? NAN : INFINITY;
        else v = std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
        return s ? -v : v;
    }

    // ------------------------------------------------------------------------------------------- settings
    struct Settings {
        bool on = true;
        float thick = 1.0f;          // radius multiplier
        float opacity = 0.85f;       // see-through (also the generated textures)
        float shine = 1.0f;          // specular strength
        float gloss = 320.0f;        // glossiness (higher = smaller, sharper highlight)
        float gravity = 520.0f;      // units / s^2 (real gravity is ~690)
        float viscosity = 0.55f;     // 0 = runny, 1 = very thick: strands stretch slowly, flow slowly
        float stick = 0.8f;          // how much it clings to skin
        float maxLen = 11.0f;        // longest hanging strand before the end drops off
        float breakDist = 9.0f;      // the string to the partner snaps around this far (+-30%)
        float flow = 1.0f;           // how fast it runs down the skin
        float dry = 150.0f;          // seconds for trails / film / streaks to dry off
        int maxPieces = 22;          // pieces drawn per man
        bool collide = true;         // runs over / rests on the body instead of passing through
        bool wetFilm = true;         // water beads on the skin around the hole while he is wet
        int beadMax = 32;            // most beads on one man
        float beadSize = 1.0f;       // bead size multiplier
        bool coatShine = true;       // the giver's schlong glistens while coated
        bool cleft = true;           // a clear glossy layer over the skin around the hole and up the crevice while he is wet
        float cleftAmt = 0.8f;       // how strong
        float cleftSize = 1.0f;      // how far it reaches
        float rimShine = 2.5f;       // how glassy-wet the rim gets while he is wet (x)
        int quality = 2;             // 1 fast, 2 normal, 3 smooth (simulation sub-steps)
    } g;

    // ------------------------------------------------------------------------------------------- templates
    constexpr const char* kRopeNif = "actors\\character\\Anatomica\\fluid\\ad_rope.nif";
    constexpr const char* kRivNif = "actors\\character\\Anatomica\\fluid\\ad_rivulet.nif";
    constexpr const char* kSheenTex = "textures\\actors\\character\\male\\Anatomica\\drip\\ad_sheen.dds";
    constexpr const char* kBeadNif = "actors\\character\\Anatomica\\fluid\\ad_beads.nif";
    constexpr int kBones = 16;
    RE::NiPointer<RE::NiNode> g_tplRope, g_tplRiv, g_tplBead;
    bool g_tplTried = false;

    RE::NiPointer<RE::NiNode> LoadModel(const char* path) {
        RE::NiPointer<RE::NiNode> out;
        RE::BSModelDB::DBTraits::ArgsType args{};
        const auto err = RE::BSModelDB::Demand(path, out, args);
        if (!out) logger::warn("Anatomica fluid: could not load meshes\\{} ({})", path, static_cast<int>(err));
        return out;
    }

    // ------------------------------------------------------------------------------------------- skinned shape readback
    // the shape's own vertex data (SKEE writes body morphs into it), skinned with the current bone transforms
    struct Surface {
        RE::NiPointer<RE::BSGeometry> geo;     // held: an outfit / schlong swap can free the shape under us
        std::uint32_t count = 0;               // vertices in the shape
        std::vector<std::uint32_t> idx;        // which vertices we keep
        std::vector<V3> v, n;                  // skin space
        std::vector<std::array<std::uint8_t, 4>> bi;
        std::vector<std::array<float, 4>> bw;
        std::vector<V3> wp, wn, prev;          // world now / last frame
        double readAt = 0.0;
        bool ok = false;
        // spatial grid (sorted cell keys)
        float cell = 1.6f;
        std::vector<std::pair<std::int64_t, std::uint32_t>> grid;

        static std::int64_t Key(int x, int y, int z) { return (static_cast<std::int64_t>(x + 4096) << 26) ^ (static_cast<std::int64_t>(y + 4096) << 13) ^ static_cast<std::int64_t>(z + 4096); }

        // filter(skin-space position) -> keep; stride: keep every n-th of those
        bool Read(RE::BSGeometry* g_, const std::function<bool(const V3&)>& filter, int stride, const std::vector<std::uint32_t>* only = nullptr) {
            ok = false; geo = RE::NiPointer<RE::BSGeometry>(g_);
            if (!geo) return false;
            auto& rd = geo->GetGeometryRuntimeData();
            auto* si = rd.skinInstance.get();
            if (!si || !si->skinData) return false;
            RE::NiSkinPartition* sp = si->skinPartition.get();
            if (!sp) sp = si->skinData->skinPartition.get();
            if (!sp || sp->numPartitions == 0) return false;
            auto& part = sp->partitions[0];
            if (!part.buffData || !part.buffData->rawVertexData) return false;
            const auto desc = part.vertexDesc;
            std::uint64_t raw = 0;
            std::memcpy(&raw, &desc, sizeof(raw));
            const std::uint32_t stride_ = static_cast<std::uint32_t>(raw & 0xF) * 4;
            if (stride_ == 0 || !desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED)) return false;
            const bool full = FullPos(desc, stride_);
            const bool hasN = desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL);
            const std::uint32_t oP = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION);
            const std::uint32_t oN = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL);
            const std::uint32_t oS = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
            const std::uint8_t* d = part.buffData->rawVertexData;
            count = sp->vertexCount;
            const std::uint32_t nb = si->skinData->GetBoneCount();
            idx.clear(); v.clear(); n.clear(); bi.clear(); bw.clear();
            auto take = [&](std::uint32_t i) {
                const std::uint8_t* p = d + static_cast<size_t>(i) * stride_;
                V3 pos;
                if (full) { float f[3]; std::memcpy(f, p + oP, 12); pos = {f[0], f[1], f[2]}; }
                else { std::uint16_t h[3]; std::memcpy(h, p + oP, 6); pos = {Half(h[0]), Half(h[1]), Half(h[2])}; }
                V3 nrm{0, 0, 1};
                if (hasN) { const std::uint8_t* q = p + oN; nrm = Norm(V3(q[0] / 127.5f - 1.0f, q[1] / 127.5f - 1.0f, q[2] / 127.5f - 1.0f)); }
                std::uint16_t hw[4]; std::memcpy(hw, p + oS, 8);
                std::array<std::uint8_t, 4> b{};
                std::memcpy(b.data(), p + oS + 8, 4);
                std::array<float, 4> w{};
                float sum = 0;
                for (int k = 0; k < 4; ++k) { w[k] = b[k] < nb ? std::max(0.0f, Half(hw[k])) : 0.0f; sum += w[k]; }
                if (sum < 1e-4f) return;
                for (auto& x : w) x /= sum;
                idx.push_back(i); v.push_back(pos); n.push_back(nrm); bi.push_back(b); bw.push_back(w);
            };
            if (only) {
                for (auto i : *only) if (i < count) take(i);
            } else {
                int c = 0;
                for (std::uint32_t i = 0; i < count; ++i) {
                    const std::uint8_t* p = d + static_cast<size_t>(i) * stride_;
                    V3 pos;
                    if (full) { float f[3]; std::memcpy(f, p + oP, 12); pos = {f[0], f[1], f[2]}; }
                    else { std::uint16_t h[3]; std::memcpy(h, p + oP, 6); pos = {Half(h[0]), Half(h[1]), Half(h[2])}; }
                    if (!filter(pos)) continue;
                    if ((c++ % std::max(stride, 1)) == 0) take(i);
                }
            }
            wp.assign(v.size(), {}); wn.assign(v.size(), {}); prev.clear();
            readAt = Now();
            ok = !v.empty();
            return ok;
        }

        // skin every kept vertex with the bones as they are now
        bool Pose() {
            if (!ok || !geo) return false;
            auto* si = geo->GetGeometryRuntimeData().skinInstance.get();
            if (!si || !si->skinData || !si->bones) return false;
            const std::uint32_t nb = si->skinData->GetBoneCount();
            std::vector<Xf> M(nb);
            for (std::uint32_t b = 0; b < nb; ++b) {
                auto* bone = si->bones[b];
                if (!bone) return false;
                M[b] = Mul(Xf(bone->world), Xf(si->skinData->GetBoneDataSkinToBone(b)));
            }
            prev = wp;
            for (size_t i = 0; i < v.size(); ++i) {
                V3 p, q;
                for (int k = 0; k < 4; ++k) {
                    const float w = bw[i][k];
                    if (w <= 0.0f) continue;
                    const Xf& m = M[bi[i][k]];
                    p += m.Apply(v[i]) * w;
                    q += m.Rot(n[i]) * w;
                }
                wp[i] = p; wn[i] = Norm(q);
            }
            if (prev.size() != wp.size()) prev = wp;
            grid.clear(); grid.reserve(wp.size());
            for (size_t i = 0; i < wp.size(); ++i)
                grid.push_back({Key(static_cast<int>(std::floor(wp[i].x / cell)), static_cast<int>(std::floor(wp[i].y / cell)), static_cast<int>(std::floor(wp[i].z / cell))), static_cast<std::uint32_t>(i)});
            std::sort(grid.begin(), grid.end());
            return true;
        }

        // nearest kept vertex within maxD (-1 = none)
        int Nearest(const V3& p, float maxD) const {
            if (grid.empty()) return -1;
            const int cx = static_cast<int>(std::floor(p.x / cell)), cy = static_cast<int>(std::floor(p.y / cell)), cz = static_cast<int>(std::floor(p.z / cell));
            const int r = std::max(1, static_cast<int>(std::ceil(maxD / cell)));
            int best = -1; float bd = maxD * maxD;
            for (int dx = -r; dx <= r; ++dx)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dz = -r; dz <= r; ++dz) {
                        const auto key = Key(cx + dx, cy + dy, cz + dz);
                        auto it = std::lower_bound(grid.begin(), grid.end(), std::pair<std::int64_t, std::uint32_t>{key, 0});
                        for (; it != grid.end() && it->first == key; ++it) {
                            const V3 d = wp[it->second] - p;
                            const float dd = Dot(d, d);
                            if (dd < bd) { bd = dd; best = static_cast<int>(it->second); }
                        }
                    }
            return best;
        }
        V3 Moved(int i) const { return (i >= 0 && static_cast<size_t>(i) < prev.size()) ? wp[i] - prev[i] : V3{}; }
    };

    // ------------------------------------------------------------------------------------------- pieces
    enum class Type { Strand, String, Free, Rivulet };
    struct Pt { V3 x, xp; float r = 0.1f; bool pin = false; float skin = 0.0f; };
    struct SurfPt { int s = -1; V3 off; float r = 0.1f; };   // a point held on a surface vertex
    struct Rope {
        Type type = Type::Free;
        bool onSchlong = false;          // its surface / anchor is the schlong (coat) instead of the body
        std::vector<Pt> p;
        std::vector<float> rest;
        float vol = 0.2f;                // how much cum it is (radius comes from it)
        float r0 = 0.1f;
        float age = 0.0f, life = 12.0f, alpha = 1.0f;
        float rgb[3] = {0.96f, 0.95f, 0.90f};
        // Strand
        int lip = -1;                    // lip point it hangs from (-1: anchorSurf, or the partner's tip when tipOf)
        int anchorSurf = -1;             // hangs from this surface vertex (schlong tip)
        float phase = 0.0f, target = 1.0f;
        bool grows = true;
        float holdFor = 0.0f;            // drop off after this long (0 = when Drip says)
        // String
        RE::FormID partner = 0; bool virtualPartner = false; V3 vpStart, vpDir; float breakAt = 9.0f, lenRest = 0.0f;
        // Rivulet
        std::vector<SurfPt> path;
        V3 head; int headS = -1; float speed = 1.0f; float drain = 0.0f; bool stopped = false; float wander = 0.0f;
        bool bead = false;               // wet film bead: barely moves
        // Free
        bool landed = false; float onSkin = 0.0f;
        bool dropNow = false;            // Strand: the drip falls on the next step
        bool dead = false;
        int vis = -1;
        std::uint32_t uid = 0;
    };

    struct Emitter {
        Fluid::Kind kind = Fluid::Kind::Squirt;
        float t = 0.0f, dur = 0.4f, amount = 0.5f, acc = 0.0f, hold = 0.0f;
        float rgb[3] = {0.96f, 0.95f, 0.90f};
        std::uint32_t rope = 0;          // stream piece being filled (its uid)
        int pulses = 2;
        bool spawned = false;
    };

    struct Visual {
        RE::NiPointer<RE::NiNode> root;
        RE::NiAVObject* bone[kBones] = {};
        RE::BSGeometry* geo = nullptr;
        RE::NiPointer<RE::NiNode> host;  // the node it hangs on (his root, or the world around him for free drops)
        bool flat = false;
        bool shown = false;
        std::string tex;
        float alpha = -1.0f;
    };

    struct Actor {
        RE::FormID id = 0;
        RE::NiAVObject* root3D = nullptr;
        RE::NiPointer<RE::NiAVObject> rootRef;   // keeps the node our pieces hang on alive until we have taken them off
        Surface body, rim, schlong;
        bool lipOk = false;
        std::vector<V3> lip;             // the inner edge of the rim, world
        V3 lipC, lipN{0, -0.81f, -0.59f}, lipDown{0, 0, -1};
        float lipR = 0.3f;
        int lipLow = 0;                  // index of the lowest lip point
        V3 floorUp{0, 0, 1}; float floorZ = 0.0f;
        bool tipOk = false; V3 tip, tipAxis{0, 1, 0}; int tipSurf = -1; float tipTrack = 0.0f;
        std::vector<Rope> ropes;
        std::vector<Emitter> emit;
        std::vector<Visual> vis[4];      // 0 round (on him), 1 flat, 2 water beads (16 per piece), 3 round free drops (in the world)
        struct Bead { int s = -1; V3 off; float r = 0.05f, grow = 0.0f; };
        std::vector<Bead> beads;
        double beadAt = 0.0;
        bool logged = false, purge = false, coatLogged = false;
        int streakLog = 0;               // what the log has said about this coat's streaks (bits)
        // what Drip / Colour want
        float hang = 0.0f, hangPhase = 0.0f; float hangRgb[3] = {0.96f, 0.95f, 0.90f};
        float trail = 0.0f; int trailSide = 1; float trailRgb[3] = {0.96f, 0.95f, 0.90f}; float trailDone = 0.0f;
        float wet = 0.0f; float wetRgb[3] = {0.95f, 0.93f, 0.88f};
        float swet = 0.0f;               // his schlong wet from the round (giver, After sex page), 0-1
        float wetCum = 0.0f;             // cum in the wetness: bead opacity (0 = clear water / lube)
        float gloss = 0.0f;              // glossy skin round the hole (After sex page), 0-1
        float lube = 0.0f; float lrgb[3] = {1.0f, 1.0f, 1.0f};   // lube colour for the outer wet layer
        float shineRgb[3] = {1.0f, 1.0f, 1.0f}; float shineAmt = 0.0f;   // wet shine colour (After sex page)
        float innerWet = 1.0f;           // the inside shines this much more than the rim
        std::string wetTex;              // what the wet layer shows now ("" = its own texture)
        float coatAlpha = 1.0f;          // cum on his schlong: opacity
        bool dripOn = true;              // a drip from his tip now and then
        float coat = 0.0f, coatLeft = 0.0f, coatSecs = 90.0f; float coatRgb[3] = {0.96f, 0.95f, 0.90f}; double coatDrip = 0.0;
        float glossSent = -1.0f, sglossSent = -1.0f; double glossAt = 0.0;
        float sheenSent = -1.0f;
        std::string sheenKey;                    // which shapes / colour the schlong's wet layers were made for
        struct Sheen { RE::NiPointer<RE::NiAVObject> obj; RE::BSGeometry* src = nullptr; RE::NiPointer<RE::NiNode> parent; };   // parent held so it can't vanish under us
        std::vector<Sheen> sheens;               // clear glossy copies over his schlong while it is coated
        Sheen cleftSheen;                        // the clear glossy copy of his body, masked to the hole and crevice
        RE::BSGeometry* cleftGeo = nullptr; V3 cleftC0; bool cleftC0ok = false;   // his hole in his body's mesh space
        std::string cleftTex; float cleftSent = -1.0f;
        double rimReadAt = 0.0, bodyReadAt = 0.0, schlongReadAt = 0.0, lastSeen = 0.0;
        bool lipTried = false;
    };

    std::mutex g_mx;
    std::unordered_map<RE::FormID, Actor> g_act;
    std::vector<std::pair<RE::FormID, float>> g_land;
    std::atomic<bool> g_queued{false};
    double g_lastTick = 0.0, g_lastWetPoll = 0.0;

    Actor& Get(RE::FormID id) { auto& a = g_act[id]; a.id = id; return a; }

    // ------------------------------------------------------------------------------------------- the look
    float Radius(float vol) { return std::clamp(0.075f + 0.06f * std::sqrt(std::max(vol, 0.0f)), 0.05f, 0.32f) * g.thick; }

    std::unordered_map<std::uint32_t, std::string> g_texCache;
    std::atomic<bool> g_forgetMade{false};   // set from the menu (any thread), acted on in the frame under g_mx
    const std::string& TexFor(const float rgb[3]) {
        auto q = [](float x) { return static_cast<std::uint32_t>(std::clamp(static_cast<int>(std::lround(x * 31.0f)), 0, 31)); };
        const std::uint32_t key = q(rgb[0]) | (q(rgb[1]) << 5) | (q(rgb[2]) << 10) | (static_cast<std::uint32_t>(std::lround(g.opacity * 20.0f)) << 15);
        auto it = g_texCache.find(key);
        if (it != g_texCache.end()) return it->second;
        return g_texCache.emplace(key, Drip::CumTexFor(rgb, g.opacity)).first->second;
    }

    void SetLookTex(Visual& v, const std::string& tex);
    void SetLook(Visual& v, const float rgb[3]) { SetLookTex(v, TexFor(rgb)); }
    void SetLookTex(Visual& v, const std::string& tex) {
        if (!v.geo) return;
        if (tex == v.tex && v.tex != "?") return;
        auto* lsp = v.geo->lightingShaderProp_cast();
        if (!lsp) return;
        auto* mat = static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material);
        if (!mat) return;
        auto* nm = static_cast<RE::BSLightingShaderMaterialBase*>(mat->Create());
        if (!nm) return;
        nm->CopyMembers(mat);
        if (!tex.empty()) {
            auto* ts = RE::BSShaderTextureSet::Create();
            if (ts) {
                for (std::uint32_t i = 0; i < RE::BSTextureSet::Texture::kTotal; ++i) {
                    const char* pth = mat->textureSet ? mat->textureSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i)) : nullptr;
                    if (pth && *pth) ts->SetTexturePath(static_cast<RE::BSTextureSet::Texture>(i), pth);
                }
                ts->SetTexturePath(RE::BSTextureSet::Texture::kDiffuse, tex.c_str());
                nm->ClearTextures();
                nm->OnLoadTextureSet(0, ts);
            }
        }
        nm->specularPower = g.gloss;
        nm->specularColorScale = 0.8f * g.shine;
        nm->materialAlpha = 1.0f;
        lsp->SetMaterial(nm, true);
        lsp->SetupGeometry(v.geo);
        lsp->FinishSetupGeometry(v.geo);
        nm->~BSLightingShaderMaterialBase();
        RE::free(nm);
        v.tex = tex;
        v.alpha = -1.0f;
    }

    Visual* MakeVisual(Actor& A, int kind) {
        const bool flat = kind == 1 || kind == 2;
        auto& tpl = kind == 2 ? g_tplBead : (kind == 1 ? g_tplRiv : g_tplRope);
        if (!tpl) return nullptr;
        // free drops hang in the world around him, not on him: under his own root they would be carried along by
        // every move the game gives him after we placed them (they twitched along with his body)
        RE::NiNode* node = nullptr;
        if (kind == 3 && A.root3D && A.root3D->parent) node = A.root3D->parent;
        if (!node) node = A.root3D ? A.root3D->AsNode() : nullptr;
        if (!node) return nullptr;
        // a clone of the template, its contents moved into a plain node: the template's root comes back from the
        // model loader as a fade node, and a fade node inside the actor's own one is not something to risk
        RE::NiPointer<RE::NiObject> keep(tpl->Clone());
        auto* cn = keep ? keep->AsNode() : nullptr;
        if (!cn) return nullptr;
        auto* box = RE::NiNode::Create(2);
        if (!box) return nullptr;
        box->name = "AnatomicaFluid";
        std::vector<RE::NiAVObject*> kids;
        for (auto& c : cn->GetChildren()) if (c) kids.push_back(c.get());
        RE::NiPointer<RE::NiNode> boxRef(box);
        for (auto* c : kids) {
            RE::NiPointer<RE::NiAVObject> hold(c);
            cn->DetachChild2(c);
            box->AttachChild(c, true);
        }
        Visual v;
        v.root = boxRef;
        v.flat = flat;
        for (int k = 0; k < kBones; ++k) {
            const auto nm = std::format("ADF{:02d}", k);
            v.bone[k] = box->GetObjectByName(RE::BSFixedString(nm.c_str()));
            if (!v.bone[k]) return nullptr;
        }
        RE::BSVisit::TraverseScenegraphGeometries(box, [&](RE::BSGeometry* geo) {
            if (!v.geo) v.geo = geo;
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        if (!v.geo) return nullptr;
        if (auto* si = v.geo->GetGeometryRuntimeData().skinInstance.get()) {
            si->rootParent = box;
            // the skin follows the bones through these pointers: make sure they are our copy's bones, not the template's
            int fixed = 0;
            const std::uint32_t nb = si->skinData ? si->skinData->GetBoneCount() : 0;
            if (si->bones && si->boneWorldTransforms)
                for (std::uint32_t b = 0; b < nb; ++b)
                    if (si->bones[b] && si->boneWorldTransforms[b] != &si->bones[b]->world) { si->boneWorldTransforms[b] = &si->bones[b]->world; ++fixed; }
            static bool told = false;
            if (!told) { told = true; logger::info("Anatomica fluid: skin binding of a new piece: {} of {} bone links fixed", fixed, nb); }
        }
        // drawn wherever it is: the bound is set from the real positions every time it is posed
        v.geo->GetModelData().modelBound.center = {0.0f, 0.0f, 55.0f};
        v.geo->GetModelData().modelBound.radius = 90.0f;
        box->local = RE::NiTransform();
        box->GetFlags().set(RE::NiAVObject::Flag::kHidden);
        node->AttachChild(box, true);
        v.host.reset(node);
        if (A.rootRef.get() != A.root3D) A.rootRef.reset(A.root3D);
        v.tex = "?";
        if (!A.logged) { A.logged = true; logger::info("Anatomica fluid: first piece drawn on {:08X}", A.id); }
        A.vis[kind].push_back(std::move(v));
        return &A.vis[kind].back();
    }

    // only while we hold the node they hang on (rootRef), so it can't have been freed under us
    void DropSheens(Actor& A);
    void DropCleft(Actor& A);
    void DropVisuals(Actor& A) {
        DropSheens(A);
        DropCleft(A);
        for (auto& list : A.vis)
            for (auto& v : list) {
                if (!v.root) continue;
                v.root->GetFlags().set(RE::NiAVObject::Flag::kHidden);
                if (v.host && v.root->parent == v.host.get()) v.host->DetachChild2(v.root.get());
            }
        for (auto& list : A.vis) list.clear();
        for (auto& r : A.ropes) r.vis = -1;
        A.rootRef.reset();
    }

    // 16 evenly spaced points along a polyline (with radius)
    void Resample(const std::vector<V3>& pts, const std::vector<float>& rad, V3 out[kBones], float rOut[kBones]) {
        const size_t n = pts.size();
        if (n == 0) return;
        if (n == 1) { for (int k = 0; k < kBones; ++k) { out[k] = pts[0] + V3(0, 0, -0.01f * k); rOut[k] = rad[0]; } return; }
        std::vector<float> acc(n, 0.0f);
        for (size_t i = 1; i < n; ++i) acc[i] = acc[i - 1] + Len(pts[i] - pts[i - 1]);
        const float L = std::max(acc[n - 1], 1e-4f);
        size_t j = 0;
        for (int k = 0; k < kBones; ++k) {
            const float s = L * static_cast<float>(k) / (kBones - 1);
            while (j + 2 < n && acc[j + 1] < s) ++j;
            const float seg = std::max(acc[j + 1] - acc[j], 1e-5f);
            const float t = std::clamp((s - acc[j]) / seg, 0.0f, 1.0f);
            out[k] = Lerp(pts[j], pts[j + 1], t);
            rOut[k] = rad[j] + (rad[j + 1] - rad[j]) * t;
        }
        if (L < 1e-3f) for (int k = 0; k < kBones; ++k) out[k] = pts[0] + V3(0, 0, -0.002f * k);
    }

    // place the bones: Y along the piece, Z out of the skin (flat) - bone scale = radius
    void Pose(Actor& A, Visual& v, const V3 pts[kBones], const float rad[kBones], const V3* normals, float alpha) {
        const Xf W(v.host ? v.host->world : A.root3D->world);
        const float inv = W.s > 1e-6f ? 1.0f / W.s : 1.0f;
        V3 side{1, 0, 0};
        for (int k = 0; k < kBones; ++k) {
            const V3 tgt = Norm(pts[std::min(k + 1, kBones - 1)] - pts[std::max(k - 1, 0)], {0, 0, -1});
            V3 Z;
            if (normals) Z = Norm(normals[k] - tgt * Dot(normals[k], tgt), Norm(Cross(tgt, {1, 0, 0})));
            else {   // parallel transport keeps the round tube from twisting
                side = Norm(side - tgt * Dot(side, tgt), Norm(Cross(tgt, {0, 0, 1}), {1, 0, 0}));
                Z = Cross(side, tgt);
            }
            const V3 X = Cross(tgt, Z);
            // world rotation columns X, Y=tgt, Z -> local = W^T * that
            const V3 cx = W.RotT(X), cy = W.RotT(tgt), cz = W.RotT(Z);
            auto& lt = v.bone[k]->local;
            lt.rotate.entry[0][0] = cx.x; lt.rotate.entry[1][0] = cx.y; lt.rotate.entry[2][0] = cx.z;
            lt.rotate.entry[0][1] = cy.x; lt.rotate.entry[1][1] = cy.y; lt.rotate.entry[2][1] = cy.z;
            lt.rotate.entry[0][2] = cz.x; lt.rotate.entry[1][2] = cz.y; lt.rotate.entry[2][2] = cz.z;
            lt.translate = (W.RotT(pts[k] - W.t) * inv).ni();
            lt.scale = std::max(rad[k], 0.004f) * inv;
        }
        {   // its bound where it really is now (the bones have moved it away from where the template was made)
            V3 c; for (int k = 0; k < kBones; ++k) c += pts[k];
            c = c / static_cast<float>(kBones);
            float rr = 0.0f; for (int k = 0; k < kBones; ++k) rr = std::max(rr, Len(pts[k] - c) + rad[k]);
            auto& mb = v.geo->GetModelData().modelBound;
            mb.center = (W.RotT(c - W.t) * inv).ni();
            mb.radius = (rr + 3.0f) * inv;
        }
        if (v.root->GetFlags().any(RE::NiAVObject::Flag::kHidden)) v.root->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
        RE::NiUpdateData ud{};
        v.root->Update(ud);
        if (std::abs(alpha - v.alpha) > 0.03f) {
            if (auto* lsp = v.geo->lightingShaderProp_cast()) lsp->SetMaterialAlpha(std::clamp(alpha, 0.0f, 1.0f));
            v.alpha = alpha;
        }
        v.shown = true;
    }

    // each bead on its own bone: Z out of the skin, scale = its radius
    void PoseBeads(Actor& A, Visual& v, const V3 pts[kBones], const float rad[kBones], const V3 normals[kBones], float alpha) {
        const Xf W(v.host ? v.host->world : A.root3D->world);
        const float inv = W.s > 1e-6f ? 1.0f / W.s : 1.0f;
        for (int k = 0; k < kBones; ++k) {
            const V3 Z = Norm(normals[k], {0, 0, 1});
            const V3 X = Norm(Cross({0, 0, 1}, Z), {1, 0, 0});
            const V3 Y = Cross(Z, X);
            const V3 cx = W.RotT(X), cy = W.RotT(Y), cz = W.RotT(Z);
            auto& lt = v.bone[k]->local;
            lt.rotate.entry[0][0] = cx.x; lt.rotate.entry[1][0] = cx.y; lt.rotate.entry[2][0] = cx.z;
            lt.rotate.entry[0][1] = cy.x; lt.rotate.entry[1][1] = cy.y; lt.rotate.entry[2][1] = cy.z;
            lt.rotate.entry[0][2] = cz.x; lt.rotate.entry[1][2] = cz.y; lt.rotate.entry[2][2] = cz.z;
            lt.translate = (W.RotT(pts[k] + Z * 0.004f - W.t) * inv).ni();
            lt.scale = std::max(rad[k], 0.0005f) * inv;
        }
        {   // its bound where it really is now (the bones have moved it away from where the template was made)
            V3 c; for (int k = 0; k < kBones; ++k) c += pts[k];
            c = c / static_cast<float>(kBones);
            float rr = 0.0f; for (int k = 0; k < kBones; ++k) rr = std::max(rr, Len(pts[k] - c) + rad[k]);
            auto& mb = v.geo->GetModelData().modelBound;
            mb.center = (W.RotT(c - W.t) * inv).ni();
            mb.radius = (rr + 3.0f) * inv;
        }
        if (v.root->GetFlags().any(RE::NiAVObject::Flag::kHidden)) v.root->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
        RE::NiUpdateData ud{};
        v.root->Update(ud);
        if (std::abs(alpha - v.alpha) > 0.03f) {
            if (auto* lsp = v.geo->lightingShaderProp_cast()) lsp->SetMaterialAlpha(std::clamp(alpha, 0.0f, 1.0f));
            v.alpha = alpha;
        }
        v.shown = true;
    }

    // ------------------------------------------------------------------------------------------- anchors
    constexpr std::uint32_t kRimVerts = 17126;          // 3BA_AnusRim of Anatomica HD (HIMBO)
    constexpr std::uint32_t kLipFirst = 15590, kLipCount = 192;   // its inner edge, in order around the hole
    constexpr int kLip = 32;

    RE::BSGeometry* FindGeo(RE::NiAVObject* root, const char* name) {
        auto* o = root->GetObjectByName(RE::BSFixedString(name));
        return o ? o->AsGeometry() : nullptr;
    }
    RE::BSGeometry* BodyGeo(RE::Actor* a, RE::NiAVObject* root) {
        if (auto* g_ = FindGeo(root, "HIMBO - Body HP")) return g_;
        RE::BSGeometry* best = nullptr; std::uint32_t most = 0;
        const auto& biped = a->GetBiped2();
        RE::NiAVObject* body = biped ? biped->objects[RE::BIPED_OBJECTS::kBody].partClone.get() : nullptr;
        RE::BSVisit::TraverseScenegraphGeometries(body ? body : root, [&](RE::BSGeometry* geo) {
            if (geo->name == "AnatomicaSheen") return RE::BSVisit::BSVisitControl::kContinue;   // our own glossy copy
            auto& rd = geo->GetGeometryRuntimeData();
            if (rd.skinInstance && rd.skinInstance->skinPartition && rd.skinInstance->skinPartition->vertexCount > most) {
                most = rd.skinInstance->skinPartition->vertexCount; best = geo;
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        return best;
    }

    void UpdateLip(Actor& A, RE::Actor* a, RE::NiAVObject* root) {
        const double now = Now();
        A.lipOk = false;
        auto* rimGeo = FindGeo(root, "3BA_AnusRim");
        if (rimGeo && (rimGeo != A.rim.geo.get() || now - A.rimReadAt > 4.0)) {
            std::vector<std::uint32_t> ids;
            for (int k = 0; k < kLip; ++k) ids.push_back(kLipFirst + static_cast<std::uint32_t>(k) * kLipCount / kLip);
            A.rim.Read(rimGeo, nullptr, 1, &ids);
            if (A.rim.count != kRimVerts) A.rim.ok = false;   // not the Anatomica rim we know
            A.rimReadAt = now;
        }
        if (rimGeo && A.rim.ok && A.rim.Pose() && A.rim.wp.size() == kLip) {
            A.lip = A.rim.wp;
            A.lipOk = true;
        }
        auto* pel = root->GetObjectByName(RE::BSFixedString("NPC Pelvis [Pelv]"));
        if (!pel) return;
        const Xf P(pel->world);
        A.lipN = Norm(P.Rot(V3(0, -0.81f, -0.59f)));
        if (!A.lipOk) {   // another body: a ring at the measured opening, opened with the gape
            const float gp = std::clamp(Wink::GapeOf(A.id), 0.0f, 1.5f);
            const V3 c = P.Apply(V3(0, -5.35f, -6.16f));
            const V3 e1 = Norm(P.Rot(V3(1, 0, 0))), e2 = Norm(Cross(A.lipN, e1));
            const float r = (0.15f + 1.1f * gp) * P.s;
            A.lip.resize(kLip);
            for (int k = 0; k < kLip; ++k) {
                const float t = 6.2831853f * k / kLip;
                A.lip[k] = c + e1 * (std::cos(t) * r) + e2 * (std::sin(t) * r * 0.8f);
            }
        }
        V3 c;
        for (auto& p : A.lip) c += p;
        A.lipC = c / static_cast<float>(A.lip.size());
        float r = 0; int low = 0; float lowZ = 1e9f;
        for (int k = 0; k < static_cast<int>(A.lip.size()); ++k) {
            r += Len(A.lip[k] - A.lipC);
            if (A.lip[k].z < lowZ) { lowZ = A.lip[k].z; low = k; }
        }
        A.lipR = r / static_cast<float>(A.lip.size());
        if (A.lipOk && A.lip.size() >= 8) {   // Newell: the normal of the lip loop
            V3 n;
            for (size_t i = 0; i < A.lip.size(); ++i) {
                const V3& a = A.lip[i]; const V3& b = A.lip[(i + 1) % A.lip.size()];
                n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y);
            }
            if (Len(n) > 1e-4f) {
                n = Norm(n);
                if (Dot(n, A.lipN) < 0.0f) n = n * -1.0f;
                if (Dot(n, A.lipN) > 0.5f) A.lipN = Norm(n + A.lipN);   // only trust it when it agrees roughly with the pelvis
            }
        }
        if (A.lipOk) {   // the rim data closed while he gapes (morph not in the readback): open the ring to the gape
            const float want = (0.15f + 1.1f * std::clamp(Wink::GapeOf(A.id), 0.0f, 1.5f)) * P.s;
            if (A.lipR < want * 0.5f) {
                const float k = want / std::max(A.lipR, 0.02f);
                for (auto& p : A.lip) p = A.lipC + (p - A.lipC) * k;
                A.lipR = want;
            }
        }
        A.lipLow = low;
        A.floorZ = a->GetPosition().z;
        (void)a;
    }

    // the partner's tip: the schlong's vertex farthest along his last schlong bone, or a point past that bone
    bool TipOf(RE::Actor* a, Actor& P) {
        P.tipOk = false;
        auto* root = a ? a->Get3D(false) : nullptr;
        if (!root) return false;
        auto* g6 = root->GetObjectByName(RE::BSFixedString("NPC Genitals06 [Gen06]"));
        if (!g6) return false;
        const Xf G(g6->world);
        P.tipAxis = Norm(G.Rot(V3(0, 1, 0)));
        if (P.schlong.ok && !P.schlong.wp.empty()) {
            float best = -1e9f; int bi = -1;
            for (size_t i = 0; i < P.schlong.wp.size(); ++i) {
                const float d = Dot(P.schlong.wp[i] - G.t, P.tipAxis) - 0.3f * Dot(P.schlong.wp[i] - G.t, Norm(G.Rot(V3(0, 0, -1))));
                if (d > best) { best = d; bi = static_cast<int>(i); }
            }
            if (bi >= 0) { P.tip = P.schlong.wp[bi]; P.tipSurf = bi; P.tipOk = true; return true; }
        }
        P.tip = G.Apply(V3(0, 1.1f + Drip::TipForward(), 0.245f + Drip::TipDown()));
        P.tipSurf = -1;
        P.tipOk = true;
        return true;
    }

    RE::BSGeometry* SchlongGeo(RE::Actor* a, std::uint32_t* verts = nullptr) {
        const auto& biped = a->GetBiped2();
        RE::NiAVObject* obj = biped ? biped->objects[RE::BIPED_OBJECTS::kModPelvisSecondary].partClone.get() : nullptr;
        RE::BSGeometry* best = nullptr; std::uint32_t most = 0;
        if (obj) RE::BSVisit::TraverseScenegraphGeometries(obj, [&](RE::BSGeometry* geo) {
            if (geo->name == "AnatomicaSheen") return RE::BSVisit::BSVisitControl::kContinue;
            auto& rd = geo->GetGeometryRuntimeData();
            if (rd.skinInstance && rd.skinInstance->skinPartition && rd.skinInstance->skinPartition->vertexCount > most) {
                most = rd.skinInstance->skinPartition->vertexCount; best = geo;
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        if (verts) *verts = most;
        return best;
    }
    void ReadSchlong(Actor& A, RE::Actor* a) {
        std::uint32_t most = 0;
        RE::BSGeometry* best = SchlongGeo(a, &most);
        if (!best) { A.schlong.ok = false; A.schlong.geo.reset(); return; }
        const int stride = std::max(1, static_cast<int>(most / 1800));
        A.schlong.Read(best, [](const V3&) { return true; }, stride);
        A.schlong.cell = 0.8f;
        A.schlongReadAt = Now();
    }

    // ------------------------------------------------------------------------------------------- spawning
    std::uint32_t g_uid = 0;
    float g_slow = 1.0f;
    double g_traceUntil = 0.0, g_traceNext = 0.0;   // a squirt was asked for: log what happens for a moment
    Rope& NewRope(Actor& A, Type t, const float rgb[3]) {
        A.ropes.emplace_back();
        Rope& r = A.ropes.back();
        r.uid = ++g_uid;
        r.type = t;
        for (int i = 0; i < 3; ++i) r.rgb[i] = rgb[i];
        return r;
    }

    void StartStrand(Actor& A, int lipIdx, float vol, const float rgb[3]) {
        Rope& r = NewRope(A, Type::Strand, rgb);
        r.lip = lipIdx; r.vol = vol; r.r0 = Radius(vol);
        const V3 a = A.lip[lipIdx] + A.lipN * 0.04f;
        const int n = 12;
        for (int i = 0; i < n; ++i) { Pt p; p.x = p.xp = a + V3(0, 0, -0.03f * i); p.r = r.r0; r.p.push_back(p); }
        r.p[0].pin = true;
        r.rest.assign(n - 1, 0.03f);
        r.life = 1e9f;
    }

    void StartRivulet(Actor& A, Surface& S, const V3& at, float vol, const float rgb[3], bool onSchlong, bool bead = false) {
        const int s = S.Nearest(at, 2.5f);
        if (s < 0) return;
        Rope& r = NewRope(A, Type::Rivulet, rgb);
        r.onSchlong = onSchlong;
        r.vol = vol; r.r0 = Radius(vol) * (bead ? 1.1f : 1.0f);
        r.head = S.wp[s] + S.wn[s] * 0.01f; r.headS = s;
        r.path.push_back({s, r.head - S.wp[s], r.r0});
        r.speed = (bead ? 0.05f : Rand(0.7f, 1.3f) * (1.4f - 0.9f * g.viscosity)) * g.flow;
        r.bead = bead;
        r.life = g.dry * (bead ? 0.6f : 1.0f) * Rand(0.85f, 1.15f);
        r.wander = Rand(0.0f, 6.28f);
    }

    void SpawnFree(Actor& A, const V3& at, const V3& vel, float vol, const float rgb[3], int n = 3) {
        Rope& r = NewRope(A, Type::Free, rgb);
        r.vol = vol; r.r0 = Radius(vol);
        const float h = 1.0f / 120.0f;
        for (int i = 0; i < n; ++i) {
            Pt p; p.x = at - Norm(vel) * (0.06f * i); p.xp = p.x - vel * h; p.r = r.r0 * (i == 0 ? 1.1f : 0.8f); r.p.push_back(p);
        }
        r.rest.assign(n - 1, 0.06f);
        r.life = 9.0f;
    }

    // ------------------------------------------------------------------------------------------- simulation
    struct Ctx { Actor* A; RE::Actor* a; Actor* partner = nullptr; };

    V3 Anchor(const Actor& A, const Rope& r, const Actor* other) {
        if (r.anchorSurf >= 0 && r.onSchlong && A.schlong.ok && static_cast<size_t>(r.anchorSurf) < A.schlong.wp.size())
            return A.schlong.wp[r.anchorSurf] + A.schlong.wn[r.anchorSurf] * 0.03f;
        if (r.lip >= 0 && r.lip < static_cast<int>(A.lip.size())) return A.lip[r.lip] + A.lipN * 0.04f;
        (void)other;
        return r.p.empty() ? V3{} : r.p[0].x;
    }

    // the opening (set per man before his pieces are stepped): the skin around the hole must not stop what comes out
    V3 g_holeC, g_holeN{0, -0.8f, -0.6f}; float g_holeR = 0.0f;
    bool InHole(const V3& x) {
        if (g_holeR <= 0.0f) return false;
        const V3 d = x - g_holeC;
        const float ax = Dot(d, g_holeN);                 // along the way out
        const V3 radial = d - g_holeN * ax;
        return ax > -3.0f && ax < 1.2f && Len(radial) < g_holeR + 0.45f;   // inside the canal mouth, or just leaving it
    }
    void Collide(Surface& S, Pt& p, float stick) {
        if (!g.collide || !S.ok || S.grid.empty()) return;
        if (InHole(p.x)) return;
        const int s = S.Nearest(p.x, p.r + 1.2f);
        if (s < 0) { p.skin = std::max(0.0f, p.skin - 0.05f); return; }
        const V3 n = S.wn[s];
        const float d = Dot(p.x - S.wp[s], n);
        const float want = p.r * 0.75f;
        if (d < want && d > -2.5f) {
            p.x += n * (want - d);
            p.x += S.Moved(s);       // carried along with the skin
            V3 vel = p.x - p.xp;
            const float vn = Dot(vel, n);
            V3 vt = vel - n * vn;
            vt *= (1.0f - stick);
            p.xp = p.x - vt - n * std::max(vn, 0.0f) * 0.2f;
            p.skin = std::min(1.0f, p.skin + 0.1f);
        } else p.skin = std::max(0.0f, p.skin - 0.05f);
    }

    void Verlet(Rope& r, float h, float drag) {
        const V3 gv{0, 0, -g.gravity};
        const float vmax = 300.0f * h;   // nothing moves faster than this (units / s) - keeps a bad frame from exploding
        for (auto& p : r.p) {
            if (p.pin) { p.xp = p.x; continue; }
            V3 vel = (p.x - p.xp) * (1.0f - drag * h);
            const float vl = Len(vel);
            if (vl > vmax) vel *= vmax / vl;
            p.xp = p.x;
            p.x += vel + gv * (h * h);
        }
    }

    void Constrain(Rope& r, int iters, float stiff, float plastic, float h) {
        const size_t n = r.p.size();
        if (n < 2) return;
        for (int it = 0; it < iters; ++it)
            for (size_t i = 0; i + 1 < n; ++i) {
                Pt& a = r.p[i]; Pt& b = r.p[i + 1];
                const V3 d = b.x - a.x;
                const float L = Len(d);
                if (L < 1e-6f) continue;
                const float rest = r.rest[i];
                const float wa = a.pin ? 0.0f : 1.0f, wb = b.pin ? 0.0f : 1.0f;
                if (wa + wb <= 0.0f) continue;
                const float c = (L - rest) / L * stiff / (wa + wb);
                a.x += d * (c * wa);
                b.x -= d * (c * wb);
            }
        // goo: a stretched strand slowly gets longer instead of springing back
        if (plastic > 0.0f)
            for (size_t i = 0; i + 1 < n; ++i) {
                const float L = Len(r.p[i + 1].x - r.p[i].x);
                if (L > r.rest[i]) r.rest[i] += (L - r.rest[i]) * std::min(1.0f, plastic * h);
            }
    }

    void Land(Actor& A, Rope& r, Pt& p) {
        p.x.z = A.floorZ + p.r * 0.5f;
        p.xp = p.x;
        if (!r.landed && r.type == Type::Free) {
            r.landed = true;
            r.life = std::min(r.life, r.age + 1.2f);
            if (kCum) g_land.push_back({A.id, r.vol});   // puddles are part of the cum switched off in Features.h
        }
    }

    void StepStrand(Actor& A, Rope& r, float h, int sub) {
        const V3 a = Anchor(A, r, nullptr);
        r.p[0].x = r.p[0].xp = a;
        // length: grows with the drip that is forming (Drip's phase), longer for more cum
        if (r.grows) {
            const float want = (0.25f + (g.maxLen * (0.35f + 0.25f * std::min(r.vol, 2.0f))) * r.phase) * (r.onSchlong ? 0.35f : 1.0f);
            r.target = want;
            const float per = std::max(r.target / static_cast<float>(r.p.size() - 1), 0.02f);
            for (auto& x : r.rest) x += (per - x) * std::min(1.0f, h * 2.5f);
        }
        Verlet(r, h, 1.8f + 3.0f * g.viscosity);
        Constrain(r, 2 + sub, 0.9f, 0.15f + 0.4f * (1.0f - g.viscosity), h);
        for (size_t i = 1; i < r.p.size(); ++i) {
            if (r.onSchlong) Collide(A.schlong, r.p[i], g.stick * 0.5f);
            else Collide(A.body, r.p[i], g.stick * 0.6f);
            if (r.p[i].x.z < A.floorZ + r.p[i].r) r.p[i].x.z = A.floorZ + r.p[i].r;
        }
        // thick at the lip, thin in the middle, a heavy bead at the end
        const size_t n = r.p.size();
        for (size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            const float top = 1.35f * std::exp(-t * 7.0f);
            const float mid = 1.0f - 0.45f * std::sin(t * 3.14159f) * std::clamp(r.target / 4.0f, 0.0f, 1.0f);
            const float end = (0.9f + 1.3f * r.phase) * std::exp(-std::pow((1.0f - t) / 0.12f, 2.0f));
            r.p[i].r = r.r0 * std::max({mid, top, end});
        }
        // too long: the end tears off on its own
        float L = 0;
        for (size_t i = 0; i + 1 < n; ++i) L += Len(r.p[i + 1].x - r.p[i].x);
        if (L > g.maxLen * 1.25f) r.dropNow = true;
        if (r.holdFor > 0.0f && r.age > r.holdFor) r.dropNow = true;
    }

    // the end of a hanging strand drops off (a free drop), the strand springs back short
    void DropOff(Actor& A, Rope& r, float amount) {
        const size_t n = r.p.size();
        if (n < 4) return;
        const size_t cut = n * 2 / 3;
        const V3 vel = (r.p[n - 1].x - r.p[n - 1].xp) * 120.0f;
        float rgb[3] = {r.rgb[0], r.rgb[1], r.rgb[2]};
        const V3 at = r.p[n - 1].x;
        const V3 mid = r.p[cut].x;
        Rope drop;
        drop.type = Type::Free;
        for (int i = 0; i < 3; ++i) drop.rgb[i] = rgb[i];
        drop.vol = std::max(amount, 0.05f); drop.r0 = Radius(drop.vol) * 1.1f;
        for (size_t i = cut; i < n; ++i) { Pt p = r.p[i]; p.pin = false; p.r = drop.r0 * (i == n - 1 ? 1.2f : 0.7f); drop.p.push_back(p); }
        for (size_t i = 0; i + 1 < drop.p.size(); ++i) drop.rest.push_back(std::max(0.02f, Len(drop.p[i + 1].x - drop.p[i].x) * 0.6f));
        drop.life = 9.0f;
        (void)at; (void)vel; (void)mid;
        // the strand: back to a short stub that starts growing again
        const V3 a = r.p[0].x;
        for (size_t i = 0; i < n; ++i) { r.p[i].x = r.p[i].xp = Lerp(a, r.p[std::min(i, cut)].x, 0.35f); }
        for (auto& x : r.rest) x = 0.03f;
        r.phase = 0.0f;
        A.ropes.push_back(std::move(drop));
    }

    void StepString(Actor& A, Rope& r, Actor* P, float h, int sub) {
        const V3 a = A.lipOk || !A.lip.empty() ? A.lip[A.lipLow] + A.lipN * 0.05f : r.p.front().x;
        V3 b;
        if (r.virtualPartner) b = r.vpStart + r.vpDir * std::min(r.age * 7.0f, 30.0f);
        else if (P && P->tipOk) b = P->tip;
        else { r.dead = true; return; }
        r.p.front().x = r.p.front().xp = a;
        r.p.back().x = r.p.back().xp = b;
        const float D = Len(b - a);
        if (r.age < 0.05f && D > r.breakAt * 0.7f) { r.dead = true; return; }   // already too far apart: no string
        if (r.lenRest <= 0.0f) r.lenRest = D;
        r.lenRest += (D - r.lenRest) * std::min(1.0f, h * (1.5f + 3.0f * (1.0f - g.viscosity)));
        const float per = std::max(r.lenRest, 0.2f) / static_cast<float>(r.p.size() - 1);
        for (auto& x : r.rest) x = per;
        Verlet(r, h, 3.0f);
        Constrain(r, 3 + sub, 0.8f, 0.0f, h);
        for (size_t i = 1; i + 1 < r.p.size(); ++i) Collide(A.body, r.p[i], g.stick * 0.3f);
        // thins as it is drawn out (the same cum over a longer string), beads up when thin
        const float rr = std::sqrt(std::max(r.vol, 0.01f) * 0.02f / std::max(D, 0.3f)) * g.thick;
        const size_t n = r.p.size();
        for (size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            const float ends = 1.0f + 1.4f * (std::exp(-t * 9.0f) + std::exp(-(1.0f - t) * 9.0f));
            const float bead = rr < 0.05f ? 1.0f + 0.6f * std::pow(std::max(0.0f, std::sin(t * 40.0f + r.age * 2.0f)), 8.0f) : 1.0f;
            r.p[i].r = std::clamp(rr, 0.012f, 0.2f) * ends * bead;
        }
        if (D > r.breakAt || rr < 0.014f) {
            // snap: each half springs back to its own end
            const size_t mid = n / 2;
            Rope lipHalf; lipHalf.type = Type::Strand; lipHalf.lip = A.lipLow; lipHalf.vol = r.vol * 0.5f; lipHalf.r0 = Radius(lipHalf.vol);
            for (int i = 0; i < 3; ++i) lipHalf.rgb[i] = r.rgb[i];
            for (size_t i = 0; i <= mid; ++i) lipHalf.p.push_back(r.p[i]);
            lipHalf.p[0].pin = true;
            for (size_t i = 0; i + 1 < lipHalf.p.size(); ++i) lipHalf.rest.push_back(0.12f);
            lipHalf.phase = 0.6f; lipHalf.holdFor = Rand(3.0f, 7.0f); lipHalf.life = 1e9f;
            A.ropes.push_back(std::move(lipHalf));
            if (P && !r.virtualPartner) {
                Rope tipHalf; tipHalf.type = Type::Strand; tipHalf.onSchlong = true; tipHalf.anchorSurf = P->tipSurf; tipHalf.vol = r.vol * 0.4f;
                tipHalf.r0 = Radius(tipHalf.vol);
                for (int i = 0; i < 3; ++i) tipHalf.rgb[i] = r.rgb[i];
                for (size_t i = n - 1; i + 1 > mid; --i) tipHalf.p.push_back(r.p[i]);
                tipHalf.p[0].pin = true;
                for (size_t i = 0; i + 1 < tipHalf.p.size(); ++i) tipHalf.rest.push_back(0.1f);
                tipHalf.phase = 0.5f; tipHalf.holdFor = Rand(2.0f, 5.0f); tipHalf.life = 1e9f;
                if (tipHalf.anchorSurf >= 0) P->ropes.push_back(std::move(tipHalf));
            }
            r.dead = true;
        }
    }

    void StepFree(Actor& A, Rope& r, float h, int sub) {
        Verlet(r, h, 0.4f + 1.5f * g.viscosity);
        Constrain(r, 1 + sub, 0.5f, 0.3f, h);
        bool touching = false;
        for (auto& p : r.p) {
            Collide(A.body, p, g.stick);
            if (p.skin > 0.5f) touching = true;
            if (p.x.z < A.floorZ + p.r) Land(A, r, p);
        }
        // stuck on the skin and slowed down: it runs down him from here
        if (touching && !r.landed) {
            r.onSkin += h;
            const float sp = Len(r.p[0].x - r.p[0].xp) / h;
            if (r.onSkin > 0.25f && sp < 30.0f && A.body.ok) {
                int rivs = 0; for (auto& o : A.ropes) if (o.type == Type::Rivulet && !o.dead) ++rivs;
                if (rivs < g.maxPieces / 2) StartRivulet(A, A.body, r.p[0].x, r.vol, r.rgb, false);
                r.dead = true;
            }
        } else r.onSkin = 0.0f;
        // pieces of a stream pull apart into drops
        for (size_t i = 0; i + 1 < r.p.size(); ++i)
            if (Len(r.p[i + 1].x - r.p[i].x) > std::max(r.rest[i] * 3.0f, 0.6f) && r.p.size() > 3) {
                Rope tail; tail.type = Type::Free; tail.vol = r.vol * 0.5f; tail.r0 = r.r0; tail.life = r.life; tail.age = r.age;
                for (int c = 0; c < 3; ++c) tail.rgb[c] = r.rgb[c];
                tail.p.assign(r.p.begin() + static_cast<long>(i) + 1, r.p.end());
                tail.rest.assign(r.rest.begin() + static_cast<long>(i) + 1, r.rest.end());
                r.p.resize(i + 1); r.rest.resize(i); r.vol *= 0.5f;
                if (tail.p.size() >= 2) A.ropes.push_back(std::move(tail));
                break;
            }
    }

    void StepRivulet(Actor& A, Rope& r, float h) {
        Surface& S = r.onSchlong ? A.schlong : A.body;
        if (!S.ok || S.wp.empty()) { r.dead = true; return; }
        // carried with the skin
        for (auto& sp : r.path) if (sp.s < 0 || static_cast<size_t>(sp.s) >= S.wp.size()) sp.s = -1;
        if (r.headS >= 0 && static_cast<size_t>(r.headS) < S.wp.size()) r.head += S.Moved(r.headS);
        if (!r.stopped && r.vol > 0.005f) {
            const int s = S.Nearest(r.head, 1.5f);
            if (s < 0) {   // ran off the edge: it falls
                SpawnFree(A, r.head, V3(0, 0, -8), r.vol, r.rgb, 3);
                r.stopped = true;
            } else {
                const V3 n = S.wn[s];
                const V3 gdir{0, 0, -1};
                V3 gt = gdir - n * Dot(gdir, n);
                const float steep = Len(gt);
                r.wander += h * Rand(-3.0f, 3.0f);
                const V3 side = Norm(Cross(n, Norm(gt, {0, 0, -1})), {1, 0, 0});
                const V3 dir = Norm(Norm(gt, {0, 0, -1}) + side * (0.35f * std::sin(r.wander)), {0, 0, -1});
                const float sp = r.speed * (1.5f + 3.5f * steep) * std::clamp(0.4f + r.vol, 0.3f, 1.6f) * (r.onSchlong ? 0.5f : 1.0f);
                V3 q = r.head + dir * (sp * h);
                const int s2 = S.Nearest(q, 1.5f);
                if (s2 >= 0) {
                    const V3 n2 = S.wn[s2];
                    q = q - n2 * (Dot(q - S.wp[s2], n2) - 0.012f);
                    r.head = q; r.headS = s2;
                }
                const SurfPt& last = r.path.back();
                const V3 lastW = (last.s >= 0 ? S.wp[last.s] : V3{}) + last.off;
                if (Len(r.head - lastW) > (r.bead ? 0.1f : 0.45f)) {
                    r.path.push_back({r.headS, r.head - S.wp[r.headS], r.r0});
                    r.vol -= r.drain > 0.0f ? r.drain : 0.006f;   // leaves a film behind it
                }
                if (r.path.size() > 64) {   // keep it light: thin out the old part of the trail
                    std::vector<SurfPt> keep;
                    for (size_t i = 0; i < r.path.size(); ++i) if (i % 2 == 0 || i + 8 > r.path.size()) keep.push_back(r.path[i]);
                    r.path.swap(keep);
                }
                if (r.head.z < A.floorZ + 0.5f) { r.stopped = true; if (kCum) g_land.push_back({A.id, r.vol}); r.vol = 0.0f; }
            }
            if (r.bead && r.age > 2.0f) r.stopped = true;
        }
        if (r.vol <= 0.005f) r.stopped = true;
    }

    // ------------------------------------------------------------------------------------------- emitters
    void StepEmitter(Actor& A, Emitter& e, float h) {
        using K = Fluid::Kind;
        e.t += h;
        if (A.lip.empty()) return;
        const V3 down{0, 0, -1};
        switch (e.kind) {
            case K::Squirt:
            case K::Land: {
                // pulses out of the open hole: a thick stream that pulls apart into drops
                const float pulseLen = e.kind == K::Land ? 0.18f : 0.32f, gap = 0.22f;
                const float cyc = pulseLen + gap;
                const int pulse = static_cast<int>(e.t / cyc);
                if (pulse >= e.pulses) { e.dur = 0.0f; break; }
                const float inPulse = e.t - pulse * cyc;
                if (inPulse > pulseLen) { e.rope = 0; break; }
                const float strength = (pulse == 0 ? 1.0f : 0.6f) * std::clamp(0.6f + e.amount, 0.6f, 2.0f);
                const float speed = (e.kind == K::Land ? 14.0f : 34.0f) * strength * Rand(0.85f, 1.1f);
                const V3 dir = Norm(A.lipN * 0.85f + down * 0.35f + V3(Rand(-0.08f, 0.08f), Rand(-0.08f, 0.08f), Rand(-0.05f, 0.05f)));
                e.acc += h;
                const float every = 1.0f / 75.0f;
                while (e.acc >= every) {
                    e.acc -= every;
                    Rope* cur = nullptr;
                    for (auto& rr : A.ropes) if (e.rope && rr.uid == e.rope && !rr.dead) { cur = &rr; break; }
                    if (!cur || cur->p.size() >= 14 || cur->type != Type::Free) {
                        Rope& r = NewRope(A, Type::Free, e.rgb);
                        r.vol = e.amount * 0.2f; r.r0 = Radius(e.amount) * 1.1f; r.life = 9.0f;
                        e.rope = A.ropes.back().uid;
                        cur = &A.ropes.back();
                    }
                    Rope& r = *cur;
                    Pt p; p.x = A.lipC + A.lipN * 0.35f; p.xp = p.x - dir * (speed / 120.0f);
                    p.r = r.r0 * Rand(0.8f, 1.15f) * (inPulse < 0.05f ? 1.3f : 1.0f);
                    if (!r.p.empty()) r.rest.push_back(std::max(0.05f, speed * every * 0.9f));
                    r.p.push_back(p);
                }
                break;
            }
            case K::Spray: {
                // something is inside him: it squirts out AROUND the shaft, from all round the stretched lip
                if (e.t > 0.45f) { e.dur = 0.0f; break; }
                e.acc += h;
                const float every = 1.0f / 40.0f;
                while (e.acc >= every) {
                    e.acc -= every;
                    const int k = static_cast<int>(Rand(0.0f, static_cast<float>(A.lip.size()) - 0.01f));
                    const V3 radial = Norm(A.lip[k] - A.lipC, down);
                    const V3 v = (radial * 0.75f + A.lipN * 0.55f + down * 0.2f) * (Rand(12.0f, 26.0f) * std::clamp(0.6f + e.amount, 0.6f, 1.8f));
                    SpawnFree(A, A.lip[k] + radial * 0.15f + A.lipN * 0.1f, v, e.amount * 0.08f, e.rgb, 2);
                }
                break;
            }
            case K::Ooze:
            case K::Gush: {
                // runs out of the bottom of the lip (around him if he is still in) and down the skin
                if (!e.spawned) {
                    e.spawned = true;
                    const bool gush = e.kind == K::Gush;
                    const int n = gush ? 3 : 1 + (e.amount > 0.4f ? 1 : 0);
                    for (int i = 0; i < n; ++i) {
                        const int k = (A.lipLow + static_cast<int>(Rand(-3.0f, 3.0f)) + static_cast<int>(A.lip.size())) % static_cast<int>(A.lip.size());
                        StartStrand(A, k, e.amount * (gush ? 0.6f : 0.8f), e.rgb);
                        Rope& r = A.ropes.back();
                        r.phase = 0.4f; r.holdFor = std::max(1.5f, e.hold * Rand(0.6f, 1.0f)); r.grows = true;
                        if (A.body.ok) StartRivulet(A, A.body, A.lip[k] + down * 0.4f, e.amount * (gush ? 0.5f : 0.35f), e.rgb, false);
                    }
                    if (gush) for (int i = 0; i < 3; ++i)
                        SpawnFree(A, A.lip[A.lipLow] + A.lipN * 0.2f, (A.lipN * 6.0f + down * 4.0f) * Rand(0.6f, 1.4f), e.amount * 0.15f, e.rgb, 3);
                }
                e.dur = 0.0f;
                break;
            }
        }
    }

    // ------------------------------------------------------------------------------------------- per-man upkeep
    void StepBeads(Actor& A, float h);
    void Wanted(Actor& A, double now, float h) {
        // hanging strands: how many and how big comes from how much is in him
        int strands = 0;
        for (auto& r : A.ropes) if (r.type == Type::Strand && r.lip >= 0 && r.holdFor <= 0.0f && !r.dead) ++strands;
        const int want = A.hang <= 0.01f ? 0 : (A.hang < 1.5f ? 1 : (A.hang < 3.0f ? 2 : 3));
        if (strands < want && !A.lip.empty()) {
            const int off[3] = {0, 3, -3};
            const int k = (A.lipLow + off[strands] + static_cast<int>(A.lip.size())) % static_cast<int>(A.lip.size());
            StartStrand(A, k, std::min(A.hang, 2.5f) * (strands == 0 ? 0.5f : 0.3f), A.hangRgb);
        }
        int seen = 0;
        for (auto& r : A.ropes) {
            if (r.type != Type::Strand || r.lip < 0 || r.holdFor > 0.0f || r.dead) continue;
            if (seen++ >= want) { r.holdFor = r.age + 0.5f; continue; }
            r.phase = std::clamp(A.hangPhase * (seen == 1 ? 1.0f : 0.7f), 0.0f, 1.0f);
            for (int i = 0; i < 3; ++i) r.rgb[i] = A.hangRgb[i];
        }
        // leg trail: rivulets started one after another (1-3)
        if (A.trail > 0.05f && A.body.ok && !A.lip.empty()) {
            int rivs = 0;
            for (auto& r : A.ropes) if (r.type == Type::Rivulet && !r.bead && !r.onSchlong && !r.dead) ++rivs;
            const float target = std::min(3.0f, A.trail);
            if (A.trailDone < target - 0.3f && rivs < 4) {
                const int k = (A.lipLow + (A.trailSide == 1 ? 2 : -2) + static_cast<int>(A.lip.size())) % static_cast<int>(A.lip.size());
                StartRivulet(A, A.body, A.lip[k] + V3(0, 0, -0.5f), 0.35f + 0.15f * target, A.trailRgb, false);
                if (!A.ropes.empty() && A.ropes.back().type == Type::Rivulet) {
                    A.ropes.back().life = g.dry * 3.0f;
                    A.ropes.back().drain = 0.004f;
                }
                A.trailDone += 1.0f;
            }
        }
        // water beads around the hole: more and bigger the wetter he is
        A.beads.clear();   // water beads: taken out (they did not look right); the glossy layer does the wet look now
        (void)&StepBeads;
        // a coated schlong: streaks running down him, and now and then a drip off the tip
        if (A.coatLeft > 0.0f && A.coat > 0.05f && !(A.streakLog & 1) && (!A.schlong.ok || !A.tipOk)) {
            A.streakLog |= 1;
            logger::info("Anatomica wet: {:08X} no cum streaks: {}", A.id, !A.schlong.ok ? "his schlong mesh could not be read" : "the tip of his schlong was not found");
        }
        if (A.coatLeft > 0.0f && A.coat > 0.05f && A.schlong.ok) {
            int streaks = 0, drips = 0;
            for (auto& r : A.ropes) if (r.onSchlong && !r.dead) (r.type == Type::Rivulet ? streaks : drips)++;
            if (streaks < static_cast<int>(1 + 3 * std::min(A.coat, 1.0f)) && Rand(0.0f, 1.0f) < h * 2.0f && A.tipOk) {
                const int s = static_cast<int>(Rand(0.0f, static_cast<float>(A.schlong.wp.size()) - 0.01f));
                const V3 p = A.schlong.wp[s];
                if (Dot(p - A.tip, A.tipAxis) > -4.0f) {
                    StartRivulet(A, A.schlong, p, Rand(0.04f, 0.1f) * A.coat, A.coatRgb, true);
                    if (!(A.streakLog & 2)) {
                        A.streakLog |= 2;
                        const float* c = A.coatRgb;
                        const std::string& t = TexFor(c);
                        logger::info("Anatomica wet: {:08X} first cum streak - texture {}", A.id, t.empty() ? "the plain white one (default colour, or the colour texture could not be made)" : t);
                    }
                }
            }
            if (A.dripOn && drips == 0 && now > A.coatDrip && A.tipOk && A.tipSurf >= 0 && A.coatLeft > 0.25f) {
                Rope& r = NewRope(A, Type::Strand, A.coatRgb);
                r.onSchlong = true; r.anchorSurf = A.tipSurf; r.vol = 0.15f * A.coat; r.r0 = Radius(r.vol);
                const V3 a = A.tip;
                for (int i = 0; i < 8; ++i) { Pt p; p.x = p.xp = a + V3(0, 0, -0.02f * i); p.r = r.r0; r.p.push_back(p); }
                r.p[0].pin = true; r.rest.assign(7, 0.02f);
                r.phase = 0.0f; r.holdFor = Rand(3.0f, 7.0f); r.life = 1e9f;
                A.coatDrip = now + Rand(6.0f, 14.0f);
            }
        }
    }

    // ---- water beads on the skin. Saturation (his wetness 0-1) decides how many and how big: a few fine droplets when
    // he is barely wet, a crowded field of bigger beads when soaked. Beads keep growing while he stays wet, merge
    // when they touch, and one that gets too heavy for the skin to hold runs off as a rivulet. Drying shrinks them all.
    void StepBeads(Actor& A, float h) {
        auto& B = A.beads;
        Surface& S = A.body;
        if (!g.wetFilm || !S.ok || A.lip.empty()) { B.clear(); return; }
        const float sat = std::clamp(A.wet, 0.0f, 1.0f);
        const float sz = g.beadSize;
        const float maxR = (0.12f + 0.38f * sat * sat) * sz;   // ~3 mm barely wet .. ~1.5 cm soaked
        const int want = static_cast<int>(std::round(g.beadMax * std::min(1.0f, sat * 1.4f)));
        for (auto& b : B) if (b.s < 0 || static_cast<size_t>(b.s) >= S.wp.size()) b.r = 0.0f;
        // new droplets condense, densest right around the hole
        A.beadAt += h;
        const float every = 0.25f / (0.3f + sat);
        while (A.beadAt > every) {
            A.beadAt -= every;
            if (static_cast<int>(B.size()) >= want || sat < 0.02f) break;
            const float ang = Rand(0.0f, 6.2831853f);
            const V3 e1 = Norm(Cross(A.lipN, {0, 0, 1}), {1, 0, 0}), e2 = Cross(A.lipN, e1);
            const float u = Rand(0.0f, 1.0f);
            const float rad = A.lipR + 0.25f + (0.6f + 2.4f * sat) * u * u;
            const V3 p = A.lipC + e1 * (std::cos(ang) * rad) + e2 * (std::sin(ang) * rad * (std::sin(ang) < 0.0f ? 1.4f : 1.0f));
            const int si = S.Nearest(p, 2.0f);
            if (si < 0) continue;
            Actor::Bead b;
            b.s = si;
            b.off = (p - S.wp[si]) - S.wn[si] * Dot(p - S.wp[si], S.wn[si]);
            const float x = Rand(0.0f, 1.0f);
            b.r = (0.05f + 0.07f * sat) * sz * (1.0f + 2.2f * x * x * x * sat);   // mostly fine, a few big ones when soaked
            b.grow = Rand(0.4f, 1.0f);
            B.push_back(b);
        }
        for (auto& b : B) {
            if (b.r <= 0.0f) continue;
            if (sat > 0.02f && b.r < maxR) b.r += h * 0.012f * b.grow * sat * sz;        // keeps gathering while he stays wet
            if (sat < 0.02f || static_cast<int>(B.size()) > want + 4) b.r -= h * 0.02f * sz;   // drying: everything shrinks, the small ones go first
        }
        // touching beads run together into one (the volumes add up)
        for (size_t i = 0; i < B.size(); ++i) {
            if (B[i].r <= 0.0f) continue;
            const V3 pi = S.wp[B[i].s] + B[i].off;
            for (size_t j = i + 1; j < B.size(); ++j) {
                if (B[j].r <= 0.0f) continue;
                const V3 pj = S.wp[B[j].s] + B[j].off;
                if (Len(pi - pj) < (B[i].r + B[j].r) * 0.85f) {
                    const float vi = B[i].r * B[i].r * B[i].r, vj = B[j].r * B[j].r * B[j].r;
                    const V3 p = (pi * vi + pj * vj) / (vi + vj);
                    B[i].r = std::cbrt(vi + vj);
                    B[i].off = p - S.wp[B[i].s];
                    B[j].r = 0.0f;
                }
            }
        }
        // too heavy for the skin to hold: it runs (stickier skin holds bigger beads)
        const float slideR = (0.22f + 0.35f * g.stick) * sz;
        int rivs = 0;
        for (auto& r : A.ropes) if (r.type == Type::Rivulet && !r.dead) ++rivs;
        for (auto& b : B)
            if (b.r > slideR && rivs < g.maxPieces / 2) {
                StartRivulet(A, S, S.wp[b.s] + b.off, b.r * b.r * b.r * 4.0f, A.wetRgb, false);
                if (!A.ropes.empty() && A.ropes.back().type == Type::Rivulet) A.ropes.back().r0 = b.r * 0.9f;
                b.r = 0.0f; ++rivs;
            }
        B.erase(std::remove_if(B.begin(), B.end(), [](const Actor::Bead& b) { return b.r < 0.02f; }), B.end());
    }

    void DrawBeads(Actor& A) {
        auto& B = A.beads;
        Surface& S = A.body;
        const size_t pieces = (B.size() + kBones - 1) / kBones;
        for (size_t pc = 0; pc < pieces; ++pc) {
            if (pc >= A.vis[2].size() && !MakeVisual(A, 2)) break;
            Visual& v = A.vis[2][pc];
            V3 P[kBones], N[kBones]; float R[kBones];
            for (int k = 0; k < kBones; ++k) {
                const size_t i = pc * kBones + k;
                if (i < B.size() && B[i].s >= 0 && static_cast<size_t>(B[i].s) < S.wp.size()) {
                    P[k] = S.wp[B[i].s] + B[i].off; N[k] = S.wn[B[i].s]; R[k] = B[i].r;
                } else { P[k] = A.lipC; N[k] = A.lipN; R[k] = 0.0f; }   // unused: shrunk to nothing
            }
            // beads are clear water unless the wetness is coloured
            const bool clear = A.wetCum <= 0.0f && A.wetRgb[0] > 0.9f && A.wetRgb[1] > 0.9f && A.wetRgb[2] > 0.9f;
            if (clear) SetLookTex(v, kSheenTex); else SetLook(v, A.wetRgb);
            const float wa = 0.6f + 0.35f * std::clamp(A.wet, 0.0f, 1.0f);
            PoseBeads(A, v, P, R, N, A.wetCum > 0.0f ? wa * (0.35f + 0.65f * A.wetCum) : wa);
        }
        for (size_t j = pieces; j < A.vis[2].size(); ++j)
            if (A.vis[2][j].shown) { A.vis[2][j].root->GetFlags().set(RE::NiAVObject::Flag::kHidden); A.vis[2][j].shown = false; }
    }

    // the drip of a strand forming over its hold time (strands that are not driven by Drip)
    void SelfGrow(Rope& r, float h) {
        if (r.type == Type::Strand && r.holdFor > 0.0f && r.phase < 1.0f) r.phase = std::min(1.0f, r.phase + h / std::max(r.holdFor, 0.5f));
    }

    // ------------------------------------------------------------------------------------------- gloss (NiOverride)
    void Gloss(RE::Actor* a, const char* node, float gloss, float spec, bool remove) {
        // straight through RaceMenu's interface when it is there (no Papyrus queue)
        if (remove) { if (Engine::NodeRemove(a, node, 2, -1) && Engine::NodeRemove(a, node, 3, -1)) return; }
        else if (Engine::NodeFloat(a, node, 2, gloss) && Engine::NodeFloat(a, node, 3, spec)) return;
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm || !a) return;
        RE::TESObjectREFR* ref = a;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
        if (remove) {
            for (std::int32_t key : {2, 3}) {
                auto* args = RE::MakeFunctionArguments(std::move(ref), false, RE::BSFixedString(node), std::move(key), static_cast<std::int32_t>(-1));
                vm->DispatchStaticCall("NiOverride", "RemoveNodeOverride", args, cb);
                ref = a;
            }
            return;
        }
        auto* a1 = RE::MakeFunctionArguments(std::move(ref), false, RE::BSFixedString(node), static_cast<std::int32_t>(2), static_cast<std::int32_t>(-1), std::move(gloss), false);
        vm->DispatchStaticCall("NiOverride", "AddNodeOverrideFloat", a1, cb);
        ref = a;
        auto* a2 = RE::MakeFunctionArguments(std::move(ref), false, RE::BSFixedString(node), static_cast<std::int32_t>(3), static_cast<std::int32_t>(-1), std::move(spec), false);
        vm->DispatchStaticCall("NiOverride", "AddNodeOverrideFloat", a2, cb);
    }
    std::vector<std::string> SchlongShapes(RE::Actor* a) {
        std::vector<std::string> out;
        const auto& biped = a->GetBiped2();
        RE::NiAVObject* obj = biped ? biped->objects[RE::BIPED_OBJECTS::kModPelvisSecondary].partClone.get() : nullptr;
        if (obj) RE::BSVisit::TraverseScenegraphGeometries(obj, [&](RE::BSGeometry* geo) {
            if (!geo->name.empty() && out.size() < 8) out.emplace_back(geo->name.c_str());
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        return out;
    }
    // ---- wet shine, straight on the materials (no RaceMenu / Papyrus): each shape gets its own copy of its material
    // once, and we remember what it looked like so it goes back exactly when he dries
    struct ShineBase { RE::BSShaderMaterial* mat = nullptr; float gloss = 30.0f, spec = 3.0f; RE::NiColor col{1.0f, 1.0f, 1.0f}; };
    std::unordered_map<RE::BSGeometry*, ShineBase> g_shine;
    // w = how wet (0 = dry: put it back); col / colAmt = the colour the wet highlights take (wet shine colour)
    void Shine(RE::BSGeometry* geo, float w, float glossAdd, float specAdd, const float* col = nullptr, float colAmt = 0.0f) {
        auto* lsp = geo ? geo->lightingShaderProp_cast() : nullptr;
        auto* mat = lsp ? static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material) : nullptr;
        if (!mat) return;
        auto it = g_shine.find(geo);
        if (it == g_shine.end() || it->second.mat != mat) {
            if (w <= 0.0f) return;   // never touched, nothing to put back
            ShineBase b{nullptr, mat->specularPower, mat->specularColorScale, mat->specularColor};
            if (it != g_shine.end() && it->second.mat) b = ShineBase{nullptr, it->second.gloss, it->second.spec, it->second.col};
            if (auto* nm = static_cast<RE::BSLightingShaderMaterialBase*>(mat->Create())) {   // our own copy: materials are shared between men
                nm->CopyMembers(mat);
                lsp->SetMaterial(nm, true);
                lsp->SetupGeometry(geo);
                lsp->FinishSetupGeometry(geo);
                nm->~BSLightingShaderMaterialBase();
                RE::free(nm);
            }
            mat = static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material);
            if (!mat) return;
            b.mat = mat;
            g_shine[geo] = b;
            it = g_shine.find(geo);
        }
        mat->specularPower = it->second.gloss + glossAdd * w;
        mat->specularColorScale = it->second.spec + specAdd * w;
        {   // the highlight colour: from its own toward the wet shine colour, as wet as he is
            const float k = (col && w > 0.0f) ? std::clamp(colAmt * std::min(1.0f, w * 1.5f), 0.0f, 1.0f) : 0.0f;
            const RE::NiColor& c0 = it->second.col;
            mat->specularColor = k > 0.0f ? RE::NiColor{c0.red + (col[0] - c0.red) * k, c0.green + (col[1] - c0.green) * k, c0.blue + (col[2] - c0.blue) * k} : c0;
        }
        if (w <= 0.0f) g_shine.erase(it);
    }
    std::vector<RE::BSGeometry*> SchlongGeos(RE::Actor* a) {
        std::vector<RE::BSGeometry*> out;
        const auto& biped = a->GetBiped2();
        RE::NiAVObject* obj = biped ? biped->objects[RE::BIPED_OBJECTS::kModPelvisSecondary].partClone.get() : nullptr;
        if (obj) RE::BSVisit::TraverseScenegraphGeometries(obj, [&](RE::BSGeometry* geo) {
            if (out.size() < 8 && geo->GetGeometryRuntimeData().skinInstance && geo->name != "AnatomicaSheen") out.push_back(geo);
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        return out;
    }
    std::vector<RE::BSGeometry*> AllSchlongGeos(RE::Actor* a) {
        std::vector<RE::BSGeometry*> out;
        const auto& biped = a->GetBiped2();
        RE::NiAVObject* obj = biped ? biped->objects[RE::BIPED_OBJECTS::kModPelvisSecondary].partClone.get() : nullptr;
        if (obj) RE::BSVisit::TraverseScenegraphGeometries(obj, [&](RE::BSGeometry* geo) {
            if (out.size() < 12 && geo->name != "AnatomicaSheen") out.push_back(geo);
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        return out;
    }
    // a clear, glassy copy of a schlong shape lying right on it (decal, so it never fights the skin)
    RE::NiAVObject* MakeSheen(RE::BSGeometry* src, const char* tex = kSheenTex, float gloss = -1.0f, float spec = -1.0f) {
        if (!g_tplRope || !src || !src->parent) return nullptr;
        RE::BSGeometry* tplGeo = nullptr;
        RE::BSVisit::TraverseScenegraphGeometries(g_tplRope.get(), [&](RE::BSGeometry* g_) { if (!tplGeo) tplGeo = g_; return RE::BSVisit::BSVisitControl::kContinue; });
        auto* tplLsp = tplGeo ? tplGeo->lightingShaderProp_cast() : nullptr;
        if (!tplLsp) return nullptr;
        RE::NiPointer<RE::NiObject> cl(src->Clone());
        auto* geo = cl ? cl->AsGeometry() : nullptr;
        if (!geo) return nullptr;
        // the copy moves with the same bones as the original
        auto* si = geo->GetGeometryRuntimeData().skinInstance.get();
        auto* so = src->GetGeometryRuntimeData().skinInstance.get();
        if (!si || !so || !si->skinData || !so->skinData || !si->bones || !so->bones) return nullptr;
        const std::uint32_t nb = so->skinData->GetBoneCount();
        for (std::uint32_t b = 0; b < nb; ++b) {
            si->bones[b] = so->bones[b];
            if (si->boneWorldTransforms && so->boneWorldTransforms) si->boneWorldTransforms[b] = so->boneWorldTransforms[b];
        }
        si->rootParent = so->rootParent;
        // the glassy look of the cum template, with the sheen texture
        RE::NiPointer<RE::NiObject> lc(tplLsp->Clone());
        auto* lsp = lc ? netimmerse_cast<RE::BSLightingShaderProperty*>(lc.get()) : nullptr;
        if (!lsp) return nullptr;
        geo->GetGeometryRuntimeData().shaderProperty.reset(lsp);
        if (auto* tplA = tplGeo->GetGeometryRuntimeData().alphaProperty.get()) {
            RE::NiPointer<RE::NiObject> ac(tplA->Clone());
            if (auto* ap = netimmerse_cast<RE::NiAlphaProperty*>(ac.get())) geo->GetGeometryRuntimeData().alphaProperty.reset(ap);
        }
        lsp->SetFlags(RE::BSShaderProperty::EShaderPropertyFlag8::kDynamicDecal, true);
        if (auto* mat = static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material)) {
            if (auto* nm = static_cast<RE::BSLightingShaderMaterialBase*>(mat->Create())) {
                nm->CopyMembers(mat);
                if (auto* ts = RE::BSShaderTextureSet::Create()) {
                    for (std::uint32_t i = 0; i < RE::BSTextureSet::Texture::kTotal; ++i) {
                        const char* pth = mat->textureSet ? mat->textureSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i)) : nullptr;
                        if (pth && *pth) ts->SetTexturePath(static_cast<RE::BSTextureSet::Texture>(i), pth);
                    }
                    ts->SetTexturePath(RE::BSTextureSet::Texture::kDiffuse, tex);
                    nm->ClearTextures();
                    nm->OnLoadTextureSet(0, ts);
                }
                nm->specularPower = gloss > 0.0f ? gloss : g.gloss;
                nm->specularColorScale = spec > 0.0f ? spec : 2.0f * g.shine;
                lsp->SetMaterial(nm, true);
                nm->~BSLightingShaderMaterialBase();
                RE::free(nm);
            }
        }
        lsp->SetupGeometry(geo);
        lsp->FinishSetupGeometry(geo);
        geo->name = "AnatomicaSheen";
        src->parent->AttachChild(geo, true);
        return geo;
    }
    void DropCleft(Actor& A) {
        auto& sh = A.cleftSheen;
        if (sh.obj && sh.parent && sh.obj->parent == sh.parent.get()) sh.parent->DetachChild2(sh.obj.get());
        sh = {};
        A.cleftSent = -1.0f;
    }
    void DropSheens(Actor& A) {
        for (auto& sh : A.sheens)
            if (sh.obj && sh.parent && sh.obj->parent == sh.parent.get()) sh.parent->DetachChild2(sh.obj.get());
        A.sheens.clear();
        A.sheenSent = -1.0f;
    }
    void SetAlpha(RE::BSGeometry* geo, float alpha) {
        if (auto* lsp = geo ? geo->lightingShaderProp_cast() : nullptr) lsp->SetMaterialAlpha(std::clamp(alpha, 0.0f, 1.0f));
    }

    // ---- the glossy layer around his hole and up the crevice: a clear, glassy copy of his body (like the schlong's
    // wet layer) whose texture is see-through everywhere except there. The mask is drawn once per body from the body
    // mesh itself (which of its vertices lie around the hole / in the cleft, and where they sit in its UV map), so it
    // fits any body built on the HIMBO UVs, BodySlide / OBody shape included.
    constexpr const char* kGenDirF = "Data/textures/actors/character/male/Anatomica/gen/";
    constexpr const char* kGenTexF = "textures\\actors\\character\\male\\Anatomica\\gen\\";

    // 0-1: how much of the layer this point of his body (mesh space) gets, c0 = his hole
    float CleftWeight(const V3& p, const V3& c0, float size) {
        const V3 d = p - c0;
        float w = 1.0f - Len(d) / (4.5f * size);                       // all round the hole
        const float top = 15.0f * size;
        if (d.z > -2.0f * size && d.z < top && d.y < 3.0f * size) {     // up the crack (behind him, not toward the front)
            const float t = std::clamp(d.z / top, 0.0f, 1.0f);
            const float hw = (2.3f - 1.0f * t) * size;                  // wider low down, narrowing toward the top
            const float across = 1.0f - std::abs(d.x) / hw;
            const float ends = std::min(std::clamp((d.z + 2.0f * size) / (2.0f * size), 0.0f, 1.0f), std::clamp((top - d.z) / (4.0f * size), 0.0f, 1.0f));
            const float front = std::clamp((3.0f * size - d.y) / (2.0f * size), 0.0f, 1.0f);
            w = std::max(w, across * ends * front);
        }
        w = std::clamp(w, 0.0f, 1.0f);
        return w * w * (3.0f - 2.0f * w);
    }

    bool WriteMaskDds(const std::filesystem::path& p, int n, const std::vector<float>& alpha, const std::uint8_t bgr[3], int specBase = -1) {
        int mips = 1; for (int s_ = n; s_ > 1; s_ /= 2) ++mips;
        std::uint32_t h[31] = {};
        h[0] = 124; h[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x8; h[2] = n; h[3] = n; h[4] = n * 4; h[6] = mips;
        h[18] = 32; h[19] = 0x41; h[21] = 32; h[22] = 0x00FF0000; h[23] = 0x0000FF00; h[24] = 0x000000FF; h[25] = 0xFF000000;
        h[26] = 0x1000 | 0x8 | 0x400000;
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        if (!f) return false;
        f.write("DDS ", 4);
        f.write(reinterpret_cast<const char*>(h), 124);
        std::vector<std::uint8_t> cur(static_cast<size_t>(n) * n * 4);
        for (int i = 0; i < n * n; ++i) {
            if (specBase >= 0) {   // a specular map: grey, bright where the mask is
                const auto v = static_cast<std::uint8_t>(std::clamp(specBase + (255.0f - specBase) * alpha[i], 0.0f, 255.0f));
                cur[i * 4 + 0] = cur[i * 4 + 1] = cur[i * 4 + 2] = v;
                cur[i * 4 + 3] = 255;
            } else {
                cur[i * 4 + 0] = bgr[0]; cur[i * 4 + 1] = bgr[1]; cur[i * 4 + 2] = bgr[2];
                cur[i * 4 + 3] = static_cast<std::uint8_t>(std::clamp(alpha[i] * 255.0f, 0.0f, 255.0f));
            }
        }
        int sz = n;
        for (int m = 0; m < mips; ++m) {
            f.write(reinterpret_cast<const char*>(cur.data()), static_cast<std::streamsize>(cur.size()));
            if (sz == 1) break;
            const int ns = sz / 2; std::vector<std::uint8_t> nx(static_cast<size_t>(ns) * ns * 4);
            for (int y = 0; y < ns; ++y)
                for (int x = 0; x < ns; ++x)
                    for (int c = 0; c < 4; ++c)
                        nx[(y * ns + x) * 4 + c] = static_cast<std::uint8_t>((cur[((2 * y) * sz + 2 * x) * 4 + c] + cur[((2 * y) * sz + 2 * x + 1) * 4 + c] +
                                                                              cur[((2 * y + 1) * sz + 2 * x) * 4 + c] + cur[((2 * y + 1) * sz + 2 * x + 1) * 4 + c] + 2) / 4);
            cur.swap(nx); sz = ns;
        }
        return static_cast<bool>(f);
    }

    // a mask texture for a skinned shape: weight(mesh position, skin weights, their bones' names) decides how much of the
    // layer each vertex gets; the result is painted into its UV map. "" = can't be made (no UVs / no data / nothing)
    using MaskWeight = std::function<float(const V3& pos, const float w[4], const float boneK[4])>;
    std::unordered_map<std::string, std::string> g_cleftTex;
    std::string MakeMask(RE::BSGeometry* geo, const std::string& keyBase, const MaskWeight& weight, const std::function<float(const char*)>& boneFactor,
                         const std::uint8_t bgr[3], int specBase = -1, float* covered = nullptr) {
        auto* si = geo ? geo->GetGeometryRuntimeData().skinInstance.get() : nullptr;
        RE::NiSkinPartition* sp = si ? si->skinPartition.get() : nullptr;
        if (!sp && si && si->skinData) sp = si->skinData->skinPartition.get();
        if (!sp || sp->numPartitions == 0) return {};
        auto& part0 = sp->partitions[0];
        if (!part0.buffData || !part0.buffData->rawVertexData) return {};
        const auto desc = part0.vertexDesc;
        std::uint64_t raw = 0; std::memcpy(&raw, &desc, sizeof(raw));
        const std::uint32_t stride = static_cast<std::uint32_t>(raw & 0xF) * 4;
        if (stride == 0 || !desc.HasFlag(RE::BSGraphics::Vertex::VF_UV)) return {};
        const bool full = FullPos(desc, stride);
        const std::uint32_t oP = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION);
        const std::uint32_t oT = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
        const bool skinned = desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED);
        const std::uint32_t oS = skinned ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING) : 0;
        const std::uint32_t nv = sp->vertexCount;
        // per bone: how much it counts (glans: the tip bones)
        std::vector<float> boneK;
        if (boneFactor && si && si->bones && si->skinData) {
            const std::uint32_t nb = si->skinData->GetBoneCount();
            boneK.assign(nb, 0.0f);
            for (std::uint32_t b = 0; b < nb; ++b) if (si->bones[b]) boneK[b] = boneFactor(si->bones[b]->name.c_str());
        }
        const std::string key = specBase >= 0 ? std::format("{}_{}_s{}", keyBase, nv, specBase) : std::format("{}_{}_{:02X}{:02X}{:02X}", keyBase, nv, bgr[2], bgr[1], bgr[0]);
        static std::unordered_map<std::string, float> s_cover;
        if (auto it = g_cleftTex.find(key); it != g_cleftTex.end()) { if (covered) *covered = s_cover[key]; return it->second; }
        const std::filesystem::path path = std::filesystem::path(kGenDirF) / (key + ".dds");
        const std::string tex = std::string(kGenTexF) + key + ".dds";
        std::error_code ec;
        const bool onDisk = std::filesystem::exists(path, ec);
        // every vertex: how much layer, and where it is on the texture
        const std::uint8_t* d = part0.buffData->rawVertexData;
        std::vector<float> w(nv), u(nv), v(nv);
        int any = 0;
        for (std::uint32_t i = 0; i < nv; ++i) {
            const std::uint8_t* q = d + static_cast<size_t>(i) * stride;
            V3 pos;
            if (full) { float f3[3]; std::memcpy(f3, q + oP, 12); pos = {f3[0], f3[1], f3[2]}; }
            else { std::uint16_t h3[3]; std::memcpy(h3, q + oP, 6); pos = {Half(h3[0]), Half(h3[1]), Half(h3[2])}; }
            std::uint16_t t2[2]; std::memcpy(t2, q + oT, 4);
            u[i] = Half(t2[0]); v[i] = Half(t2[1]);
            float sw[4] = {0, 0, 0, 0}, bk[4] = {0, 0, 0, 0};
            if (skinned) {
                std::uint16_t hw[4]; std::memcpy(hw, q + oS, 8);
                std::uint8_t bi[4]; std::memcpy(bi, q + oS + 8, 4);
                for (int k = 0; k < 4; ++k) { sw[k] = std::max(0.0f, Half(hw[k])); bk[k] = bi[k] < boneK.size() ? boneK[bi[k]] : 0.0f; }
            }
            w[i] = weight(pos, sw, bk);
            if (w[i] > 0.01f) ++any;
        }
        s_cover[key] = nv ? static_cast<float>(any) / static_cast<float>(nv) : 0.0f;
        if (covered) *covered = s_cover[key];
        if (any == 0) { logger::info("Anatomica wet: nothing to cover on {} for {}", geo->name.c_str() ? geo->name.c_str() : "?", keyBase); return g_cleftTex[key] = std::string{}; }
        if (onDisk) return g_cleftTex[key] = tex;
        constexpr int N = 512;
        std::vector<float> A(static_cast<size_t>(N) * N, 0.0f);
        auto put = [&](int x, int y, float a) { if (x >= 0 && y >= 0 && x < N && y < N) { float& c = A[static_cast<size_t>(y) * N + x]; c = std::max(c, a); } };
        int tris = 0;
        for (std::uint32_t pi = 0; pi < sp->numPartitions; ++pi) {
            const auto& pt = sp->partitions[pi];
            if (!pt.triList) continue;
            for (std::uint32_t t = 0; t < pt.triangles; ++t) {
                const std::uint32_t i0 = pt.triList[t * 3], i1 = pt.triList[t * 3 + 1], i2 = pt.triList[t * 3 + 2];
                if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
                if (w[i0] <= 0.01f && w[i1] <= 0.01f && w[i2] <= 0.01f) continue;
                ++tris;
                const float x0 = u[i0] * N, y0 = v[i0] * N, x1 = u[i1] * N, y1 = v[i1] * N, x2 = u[i2] * N, y2 = v[i2] * N;
                const float den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
                if (std::abs(den) < 1e-6f) continue;
                const int bx0 = std::max(0, static_cast<int>(std::floor(std::min({x0, x1, x2}))) - 1), bx1 = std::min(N - 1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))) + 1);
                const int by0 = std::max(0, static_cast<int>(std::floor(std::min({y0, y1, y2}))) - 1), by1 = std::min(N - 1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))) + 1);
                if (bx1 - bx0 > N / 4 || by1 - by0 > N / 4) continue;   // a seam-crossing sliver: skip
                for (int y = by0; y <= by1; ++y)
                    for (int x = bx0; x <= bx1; ++x) {
                        const float px = x + 0.5f, py = y + 0.5f;
                        float a = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den;
                        float b = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den;
                        float c = 1.0f - a - b;
                        const float e = -0.6f / std::sqrt(std::abs(den));   // a pixel of slack so seams close
                        if (a < e || b < e || c < e) continue;
                        a = std::max(a, 0.0f); b = std::max(b, 0.0f); c = std::max(c, 0.0f);
                        const float sum = a + b + c;
                        put(x, y, (a * w[i0] + b * w[i1] + c * w[i2]) / sum);
                    }
            }
        }
        if (tris == 0)   // no triangle list kept: soft dots at the vertices instead
            for (std::uint32_t i = 0; i < nv; ++i) {
                if (w[i] <= 0.01f) continue;
                const int cx = static_cast<int>(u[i] * N), cy = static_cast<int>(v[i] * N);
                for (int dy = -5; dy <= 5; ++dy)
                    for (int dx = -5; dx <= 5; ++dx) {
                        const float r = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / 5.0f;
                        if (r <= 1.0f) put(cx + dx, cy + dy, w[i] * (1.0f - r * r));
                    }
            }
        // soften the edge
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<float> B(A.size(), 0.0f);
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x) {
                    float sum = 0.0f; int cnt = 0;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= N || yy >= N) continue;
                            sum += A[static_cast<size_t>(yy) * N + xx]; ++cnt;
                        }
                    B[static_cast<size_t>(y) * N + x] = std::max(A[static_cast<size_t>(y) * N + x] * 0.6f, sum / static_cast<float>(cnt));
                }
            A.swap(B);
        }
        if (!WriteMaskDds(path, N, A, bgr, specBase)) { logger::info("Anatomica wet: could not write {}", path.string()); return g_cleftTex[key] = std::string{}; }
        logger::info("Anatomica wet: {} mask made for {} ({} of {} vertices, {} triangles)", keyBase, geo->name.c_str() ? geo->name.c_str() : "?", any, nv, tris);
        return g_cleftTex[key] = tex;
    }

    void UpdateCleft(Actor& A, RE::Actor* a, float wet) {
        const float cw = std::clamp(wet, 0.0f, 1.0f);   // strength and fading: After sex page (glossy skin)
        const float q = std::round(cw * 20.0f) / 20.0f;
        // dressed: the biggest shape in his body slot is the clothing, not his skin - no layer then
        const bool dressed = a && !Extras::Exposed(a);   // (with the extras, armour that leaves his backside bare does not count)
        if (q <= 0.0f || dressed || !A.body.ok || !A.body.geo) { if (A.cleftSheen.obj) DropCleft(A); return; }
        RE::BSGeometry* body = A.body.geo.get();
        if (A.cleftGeo != body) { A.cleftGeo = body; A.cleftC0ok = false; A.cleftTex.clear(); DropCleft(A); }
        if (!A.cleftC0ok) {   // his hole in the body's own mesh space (the mask is drawn there)
            if (A.lip.empty()) return;
            const int s = A.body.Nearest(A.lipC, 8.0f);
            if (s < 0) return;
            A.cleftC0 = A.body.v[s];
            A.cleftC0ok = true;
            const V3 c0 = A.cleftC0;
            const float size = g.cleftSize;
            static const std::uint8_t clear[3] = {240, 236, 236};
            A.cleftTex = MakeMask(body, std::format("ad_cleft_{}_{}_{}_{}", std::lround(c0.x * 2.0f), std::lround(c0.y * 2.0f), std::lround(c0.z * 2.0f), std::lround(size * 10.0f)),
                                  [c0, size](const V3& p, const float*, const float*) { return CleftWeight(p, c0, size); }, nullptr, clear);
        }
        if (A.cleftTex.empty()) return;
        auto& sh = A.cleftSheen;
        if (!sh.obj || !sh.parent || sh.obj->parent != body->parent) {
            DropCleft(A);
            if (auto* o = MakeSheen(body, A.cleftTex.c_str())) sh = {RE::NiPointer<RE::NiAVObject>(o), body, RE::NiPointer<RE::NiNode>(o->parent)};
            if (!sh.obj) { A.cleftTex.clear(); return; }   // could not be made: don't try every frame
        }
        if (std::abs(q - A.cleftSent) > 0.01f) { SetAlpha(sh.obj->AsGeometry(), 0.35f + 0.65f * q); A.cleftSent = q; }
    }

    // ---- the outer wet layer in the lube colour: its own little texture (uncompressed, ours), tinted
    constexpr const char* kWetTex = "textures\\actors\\character\\male\\Anatomica\\drip\\ad_wet.dds";
    std::unordered_map<std::uint32_t, std::string> g_wetTex;
    std::string TintedWet(const float rgb[3]) {
        auto q = [](float x) { return static_cast<std::uint32_t>(std::clamp(std::lround(x * 31.0f), 0L, 31L)); };
        const std::uint32_t key = q(rgb[0]) << 10 | q(rgb[1]) << 5 | q(rgb[2]);
        if (key == (31u << 10 | 31u << 5 | 31u)) return {};   // white: its own texture
        if (auto it = g_wetTex.find(key); it != g_wetTex.end()) return it->second;
        const std::string name = std::format("ad_wet_{:04X}.dds", key);
        const std::filesystem::path out = std::filesystem::path(kGenDirF) / name;
        const std::string tex = std::string(kGenTexF) + name;
        std::error_code ec;
        if (std::filesystem::exists(out, ec)) return g_wetTex[key] = tex;
        std::ifstream in("Data/textures/actors/character/male/Anatomica/drip/ad_wet.dds", std::ios::binary);
        std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::uint32_t pf = 0, bits = 0;
        if (d.size() > 128) { std::memcpy(&pf, d.data() + 80, 4); std::memcpy(&bits, d.data() + 88, 4); }
        if (d.size() <= 128 || std::memcmp(d.data(), "DDS ", 4) != 0 || !(pf & 0x40) || bits != 32) {
            logger::info("Anatomica wet: the wet layer texture could not be read - the lube colour stays off");
            return g_wetTex[key] = std::string{};
        }
        const float k[3] = {q(rgb[2]) / 31.0f, q(rgb[1]) / 31.0f, q(rgb[0]) / 31.0f};   // BGRA
        for (size_t o = 128; o + 4 <= d.size(); o += 4)
            for (int c = 0; c < 3; ++c) d[o + c] = static_cast<std::uint8_t>(std::lround(d[o + c] * k[c]));
        std::filesystem::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        if (!f || !f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()))) return g_wetTex[key] = std::string{};
        return g_wetTex[key] = tex;
    }
    bool SetDiffuse(RE::BSGeometry* geo, const std::string& path) {
        auto* lsp = geo ? geo->lightingShaderProp_cast() : nullptr;
        auto* mat = lsp ? static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material) : nullptr;
        if (!mat || !mat->textureSet) return false;
        auto* nm = static_cast<RE::BSLightingShaderMaterialBase*>(mat->Create());
        if (!nm) return false;
        nm->CopyMembers(mat);
        if (auto* ts = RE::BSShaderTextureSet::Create()) {
            for (std::uint32_t i = 0; i < RE::BSTextureSet::Texture::kTotal; ++i) {
                const char* pth = mat->textureSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
                if (pth && *pth) ts->SetTexturePath(static_cast<RE::BSTextureSet::Texture>(i), pth);
            }
            ts->SetTexturePath(RE::BSTextureSet::Texture::kDiffuse, path.c_str());
            nm->ClearTextures();
            nm->OnLoadTextureSet(0, ts);
        }
        lsp->SetMaterial(nm, true);
        lsp->SetupGeometry(geo);
        lsp->FinishSetupGeometry(geo);
        nm->~BSLightingShaderMaterialBase();
        RE::free(nm);
        return true;
    }

    // ---- the glans' specular map (see UpdateGloss)
    std::unordered_map<RE::BSGeometry*, std::string> g_glansOrig;   // its own specular map, to put back when he is dry
    bool IsMSN(RE::BSGeometry* geo) {
        auto* lsp = geo ? geo->lightingShaderProp_cast() : nullptr;
        return lsp && lsp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals);
    }
    std::string SpecOf(RE::BSGeometry* geo) {
        auto* lsp = geo ? geo->lightingShaderProp_cast() : nullptr;
        auto* mat = lsp ? static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material) : nullptr;
        const char* p = (mat && mat->textureSet) ? mat->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kSpecular) : nullptr;
        return p ? p : "";
    }
    bool SetSpecTex(RE::BSGeometry* geo, const std::string& path) {
        auto* lsp = geo ? geo->lightingShaderProp_cast() : nullptr;
        auto* mat = lsp ? static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material) : nullptr;
        if (!mat || !mat->textureSet) return false;
        auto* nm = static_cast<RE::BSLightingShaderMaterialBase*>(mat->Create());
        if (!nm) return false;
        nm->CopyMembers(mat);
        if (auto* ts = RE::BSShaderTextureSet::Create()) {
            for (std::uint32_t i = 0; i < RE::BSTextureSet::Texture::kTotal; ++i) {
                const char* pth = mat->textureSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
                if (pth && *pth) ts->SetTexturePath(static_cast<RE::BSTextureSet::Texture>(i), pth);
            }
            ts->SetTexturePath(RE::BSTextureSet::Texture::kSpecular, path.c_str());
            nm->ClearTextures();
            nm->OnLoadTextureSet(0, ts);
        }
        lsp->SetMaterial(nm, true);
        lsp->SetupGeometry(geo);
        lsp->FinishSetupGeometry(geo);
        nm->~BSLightingShaderMaterialBase();
        RE::free(nm);
        return true;
    }

    void UpdateGloss(Actor& A, RE::Actor* a) {
        auto* root = a->Get3D(false);
        if (!root) return;
        // his hole: glossier skin, and the clear sheen layer of the rim armor (Drip puts it on while he is wet)
        const float w = std::clamp(A.wet, 0.0f, 1.0f);
        const float q = std::round(w * 40.0f) / 40.0f;
        for (const char* nd : {"3BA_AnusRim", "3BA_Anus"}) {
            auto* o = root->GetObjectByName(RE::BSFixedString(nd));
            const float k = (std::string_view(nd) == "3BA_Anus") ? std::clamp(A.innerWet, 0.0f, 3.0f) : 1.0f;   // the inside: wetter than the rim (After sex page)
            Shine(o ? o->AsGeometry() : nullptr, q, 180.0f * g.rimShine * k, 8.0f * g.rimShine * g.shine * k,   // glassy, like water
                  A.shineRgb, A.shineAmt);
        }
        if (!kCum)   // the rim armor's cum pieces (pool in the canal, film, lip): never with cum off
            for (const char* nd : {"AnatomicaPool", "AnatomicaFilm", "AnatomicaLip"})
                if (auto* o = root->GetObjectByName(RE::BSFixedString(nd)); o && !o->GetFlags().any(RE::NiAVObject::Flag::kHidden))
                    o->GetFlags().set(RE::NiAVObject::Flag::kHidden);
        // the rim armor's wet layer: a separate shell over the rim. It does not follow the gape morph and its edge shows
        // as a ring, so it is only used to carry an "Anus wear colour (outer rim)" while he is closed; plain wetness is
        // the rim's own glassy shine and the glossy skin layer.
        const bool wetLayer = q > 0.0f && A.lube > 0.01f && Wink::GapeOf(A.id) < 0.05f;
        if (auto* o = root->GetObjectByName(RE::BSFixedString("AnatomicaWet")); o && o->AsGeometry()) {
            if (wetLayer) {
                o->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
                // lube colour: the layer's own texture, tinted (clear when off)
                std::string want;
                if (A.lube > 0.01f) {
                    float c[3];
                    for (int i = 0; i < 3; ++i) c[i] = 1.0f + (std::clamp(A.lrgb[i], 0.0f, 1.0f) - 1.0f) * std::clamp(A.lube * 1.6f, 0.0f, 1.0f);
                    want = TintedWet(c);
                }
                {   // compared with what the layer really shows (the game may have rebuilt it)
                    auto* lsp = o->AsGeometry()->lightingShaderProp_cast();
                    auto* m = lsp ? static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material) : nullptr;
                    std::string cur = (m && m->textureSet && m->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) ? m->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : "";
                    for (auto& ch : cur) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    std::string wl = want;
                    for (auto& ch : wl) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    const bool ok = want.empty() ? cur.find("ad_wet_") == std::string::npos : cur.find(wl.substr(wl.find_last_of('\\') + 1)) != std::string::npos;
                    if (!ok) { SetDiffuse(o->AsGeometry(), want.empty() ? std::string(kWetTex) : want); A.glossSent = -1.0f; }
                    A.wetTex = want;
                }
                if (std::abs(q - A.glossSent) > 0.01f || A.glossSent < 0.0f) SetAlpha(o->AsGeometry(), std::min(0.65f, 0.2f + 0.45f * q));
            } else if (!o->GetFlags().any(RE::NiAVObject::Flag::kHidden)) o->GetFlags().set(RE::NiAVObject::Flag::kHidden);
        }
        A.glossSent = wetLayer ? q : -1.0f;
        // the skin round his hole and up the crevice: a clear glossy layer while he is wet
        UpdateCleft(A, a, A.gloss);
        // his schlong while coated: glossier skin and a clear wet layer, fading as it dries
        const float sc = std::max(g.coatShine ? std::clamp(A.coatLeft, 0.0f, 1.0f) : 0.0f, std::clamp(A.swet, 0.0f, 1.0f));
        const float sq = std::round(sc * 20.0f) / 20.0f;
        // wet glans: the skin itself glossy (no layer on top). For a shape drawn with a model-space normal map the shine
        // has a texture of its own (specular map): we give it one that is bright on the glans and dull elsewhere, so only
        // the glans glistens. Other shapes can only be glossy as a whole: the shape that is mostly glans gets it.
        if (!A.sheens.empty()) { DropSheens(A); A.sheenKey.clear(); }   // the old clear layer on top: gone
        auto geos = SchlongGeos(a);
        auto tipBones = [](const char* n) -> float {
            const std::string_view v(n ? n : "");
            if (v.find("Gen06") != std::string_view::npos || v.find("Genitals06") != std::string_view::npos) return 1.0f;
            if (v.find("Gen05") != std::string_view::npos || v.find("Genitals05") != std::string_view::npos) return 0.45f;
            return 0.0f;
        };
        auto glans = [](const V3&, const float w[4], const float k[4]) {
            float t = 0.0f;
            for (int i = 0; i < 4; ++i) t += w[i] * k[i];
            t = std::clamp((t - 0.15f) / 0.6f, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        if (sq > 0.0f) {
            std::string told;
            for (auto* geo : geos) {
                static const std::uint8_t grey[3] = {255, 255, 255};
                float cover = 0.0f;
                if (IsMSN(geo)) {
                    const std::string tex = MakeMask(geo, "ad_glanspec", glans, tipBones, grey, 20, &cover);
                    if (tex.empty()) { Shine(geo, 0.0f, 0.0f, 0.0f); continue; }
                    if (!g_glansOrig.count(geo)) g_glansOrig[geo] = SpecOf(geo);
                    if (SpecOf(geo).find("ad_glanspec") == std::string::npos) SetSpecTex(geo, tex);
                    Shine(geo, sq, 450.0f, 14.0f * g.shine);
                    told += std::format(" [{}: glans only]", geo->name.c_str() ? geo->name.c_str() : "?");
                } else {
                    MakeMask(geo, "ad_glans", glans, tipBones, grey, -1, &cover);
                    if (cover > 0.5f) { Shine(geo, sq, 450.0f, 14.0f * g.shine); told += std::format(" [{}: whole shape, it is mostly glans]", geo->name.c_str() ? geo->name.c_str() : "?"); }
                    else { Shine(geo, 0.0f, 0.0f, 0.0f); told += std::format(" [{}: left dry - not mostly glans and no specular map of its own]", geo->name.c_str() ? geo->name.c_str() : "?"); }
                }
            }
            if (!A.coatLogged) { A.coatLogged = true; logger::info("Anatomica wet: wet glans on {:08X}:{}", A.id, told.empty() ? " nothing to make wet" : told); }
        } else {
            for (auto* geo : AllSchlongGeos(a)) {
                Shine(geo, 0.0f, 0.0f, 0.0f);
                if (auto it = g_glansOrig.find(geo); it != g_glansOrig.end()) { SetSpecTex(geo, it->second); g_glansOrig.erase(it); }
            }
        }
        A.sglossSent = sq;
    }

    // ------------------------------------------------------------------------------------------- the frame
    void DrawRope(Actor& A, Rope& r, Visual& v) {
        std::vector<V3> pts; std::vector<float> rad;
        std::vector<V3> nrm;
        const bool flat = r.type == Type::Rivulet;
        if (flat) {
            Surface& S = r.onSchlong ? A.schlong : A.body;
            const size_t n = r.path.size();
            for (size_t i = 0; i < n; ++i) {
                const auto& sp = r.path[i];
                if (sp.s < 0 || static_cast<size_t>(sp.s) >= S.wp.size()) continue;
                pts.push_back(S.wp[sp.s] + sp.off);
                nrm.push_back(S.wn[sp.s]);
                // a thin wet line behind, the drop itself at the head
                const float t = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 1.0f;
                rad.push_back(r.r0 * (r.bead ? 1.0f : (0.45f + 0.35f * t)));
            }
            if (!r.stopped && r.headS >= 0 && !pts.empty()) {
                pts.push_back(r.head); nrm.push_back(S.wn[r.headS]); rad.push_back(r.r0 * (r.bead ? 1.0f : 1.5f));
            }
            if (pts.size() == 1) { pts.push_back(pts[0] + V3(0, 0, -0.06f)); nrm.push_back(nrm[0]); rad.push_back(rad[0]); }
        } else {
            for (auto& p : r.p) { pts.push_back(p.x); rad.push_back(p.r); }
        }
        if (pts.size() < 2) { v.root->GetFlags().set(RE::NiAVObject::Flag::kHidden); v.shown = false; return; }
        V3 P[kBones]; float R[kBones]; V3 N[kBones];
        Resample(pts, rad, P, R);
        for (int k = 0; k < kBones; ++k)
            if (!std::isfinite(P[k].x) || !std::isfinite(P[k].y) || !std::isfinite(P[k].z) || !std::isfinite(R[k])) {
                v.root->GetFlags().set(RE::NiAVObject::Flag::kHidden); v.shown = false; return;
            }
        if (flat) {
            const std::vector<V3>& np = nrm;
            // normals follow the same resampling (nearest point)
            for (int k = 0; k < kBones; ++k) {
                float best = 1e9f; int bi = 0;
                for (size_t i = 0; i < pts.size(); ++i) { const float d = Len(pts[i] - P[k]); if (d < best) { best = d; bi = static_cast<int>(i); } }
                N[k] = np[bi];
            }
        }
        SetLook(v, r.rgb);
        Pose(A, v, P, R, flat ? N : nullptr, r.alpha);
    }

    void Draw(Actor& A) {
        // the newest pieces win when there are more than we draw
        std::vector<int> order;
        for (int i = static_cast<int>(A.ropes.size()) - 1; i >= 0; --i) if (!A.ropes[i].dead) order.push_back(i);
        auto kindOf = [](const Rope& r) { return r.type == Type::Rivulet ? 1 : (r.type == Type::Free ? 3 : 0); };
        int used[4] = {0, 0, 0, 0};
        std::vector<int> slot(A.ropes.size(), -1);
        for (int i : order) {
            const int kind = kindOf(A.ropes[i]);
            if (used[0] + used[1] + used[3] >= g.maxPieces) break;
            if (used[kind] >= static_cast<int>(A.vis[kind].size())) {
                if (!MakeVisual(A, kind)) continue;
            }
            slot[i] = used[kind]++;
        }
        for (size_t i = 0; i < A.ropes.size(); ++i) {
            if (slot[i] < 0) continue;
            const int kind = kindOf(A.ropes[i]);
            DrawRope(A, A.ropes[i], A.vis[kind][slot[i]]);
        }
        DrawBeads(A);
        for (int k : {0, 1, 3})
            for (size_t j = static_cast<size_t>(used[k]); j < A.vis[k].size(); ++j)
                if (A.vis[k][j].shown) { A.vis[k][j].root->GetFlags().set(RE::NiAVObject::Flag::kHidden); A.vis[k][j].shown = false; }
    }

    // a few lines in the log after each squirt / spray / ooze / gush: where the hole is, where the drops go, where they are drawn
    void Trace(Actor& A, RE::NiAVObject* root, double now) {
        if (now > g_traceUntil || now < g_traceNext || (A.ropes.empty() && A.emit.empty())) return;
        g_traceNext = now + 0.25;
        int nf = 0, ns = 0, nr = 0, nst = 0, landed = 0;
        const Rope* first = nullptr;
        for (auto& r : A.ropes) {
            if (r.dead) continue;
            switch (r.type) {
                case Type::Free: ++nf; if (r.landed) ++landed; if (!first) first = &r; break;
                case Type::Strand: ++ns; break;
                case Type::Rivulet: ++nr; break;
                case Type::String: ++nst; break;
            }
        }
        const auto rp = root->world.translate;
        logger::info("fluid trace {:08X}: lip {} c({:.1f} {:.1f} {:.1f}) out({:.2f} {:.2f} {:.2f}) r {:.2f} | root({:.1f} {:.1f} {:.1f}) floor {:.1f} | body {} ({} pts) | emit {} | free {} (landed {}) strands {} trails {} strings {}",
            A.id, A.lipOk ? "rim" : "guess", A.lipC.x, A.lipC.y, A.lipC.z, A.lipN.x, A.lipN.y, A.lipN.z, A.lipR, rp.x, rp.y, rp.z, A.floorZ,
            A.body.ok ? "ok" : "NO", A.body.wp.size(), A.emit.size(), nf, landed, ns, nr, nst);
        if (first && !first->p.empty()) {
            const V3 x = first->p.front().x, u = (first->p.front().x - first->p.front().xp);
            logger::info("    first drop: at({:.1f} {:.1f} {:.1f}) {:.1f} from the hole, {:.1f} out of it, moving {:.0f}/s, on skin {:.1f}, age {:.2f}",
                x.x, x.y, x.z, Len(x - A.lipC), Dot(x - A.lipC, A.lipN), Len(u) * 120.0f, first->p.front().skin, first->age);
        }
        for (int k : {3, 0}) {
            for (auto& v : A.vis[k]) {
                if (!v.shown || !v.bone[0]) continue;
                const auto b0 = v.bone[0]->world.translate, b15 = v.bone[kBones - 1]->world.translate;
                const auto& wb = v.geo->GetModelData().modelBound;
                const auto* si = v.geo->GetGeometryRuntimeData().skinInstance.get();
                const bool linked = si && si->boneWorldTransforms && si->boneWorldTransforms[0] == &v.bone[0]->world;
                logger::info("    drawn ({}): bone0({:.1f} {:.1f} {:.1f}) bone15({:.1f} {:.1f} {:.1f}) scale {:.3f} | on {} | skin linked {} | bound r {:.1f} | hidden {}",
                    k == 3 ? "free" : "on him", b0.x, b0.y, b0.z, b15.x, b15.y, b15.z, v.bone[0]->world.scale,
                    v.host && v.host->name.c_str() ? v.host->name.c_str() : "?", linked, wb.radius, v.root->GetFlags().any(RE::NiAVObject::Flag::kHidden));
                break;
            }
        }
    }

    void Tick() {
        g_queued = false;
        const double now = Now();
        float dt = g_lastTick > 0.0 ? static_cast<float>(now - g_lastTick) : 0.0f;
        g_lastTick = now;
        if (!g_tplTried) {
            g_tplTried = true;
            g_tplRope = LoadModel(kRopeNif);
            g_tplRiv = LoadModel(kRivNif);
            g_tplBead = LoadModel(kBeadNif);
            logger::info("Anatomica fluid: templates {} / {} / {}", g_tplRope ? "ok" : "MISSING", g_tplRiv ? "ok" : "MISSING", g_tplBead ? "ok" : "MISSING");
        }
        // wetness and coated schlongs come from the colour code (after-sex looks)
        std::vector<Colour::WetInfo> wet;
        bool polled = false;
        if (now - g_lastWetPoll > 0.5) { g_lastWetPoll = now; wet = Colour::WetNow(); polled = true; }
        std::lock_guard lk(g_mx);
        if (g_forgetMade.exchange(false)) {   // files deleted: check / make them again
            g_texCache.clear(); g_cleftTex.clear(); g_wetTex.clear();
            for (auto& [id, A] : g_act) { A.cleftTex.clear(); A.cleftC0ok = false; }
        }
        if (polled) {
            for (auto& [id, A] : g_act) A.wet = 0.0f, A.swet = 0.0f, A.gloss = 0.0f, A.lube = 0.0f;
            for (auto& w : wet) {
                if (w.wet <= 0.0f && w.schlong <= 0.0f && w.gloss <= 0.0f) continue;
                auto& A = Get(w.id);
                A.wet = w.wet;
                A.gloss = w.gloss;
                A.lube = w.lube;
                for (int i = 0; i < 3; ++i) A.lrgb[i] = w.lrgb[i];
                for (int i = 0; i < 3; ++i) A.shineRgb[i] = w.shine[i];
                A.shineAmt = w.shineAmt;
                A.innerWet = w.innerWet;
                A.swet = w.schlong;
                A.wetCum = w.cum;
                for (int i = 0; i < 3; ++i) A.wetRgb[i] = w.rgb[i];
                // cum on the giver's schlong (After sex page, Wet schlong): follows his wet shine as it dries
                if (w.scum > 0.0f && w.schlong > 0.01f) {
                    if (A.coatLeft <= 0.0f) {
                        A.coatDrip = now + 1.5; A.tipOk = true; A.coatLogged = false; A.streakLog = 0;
                        logger::info("Anatomica wet: {:08X} schlong coated - cum streaks in {:.2f} {:.2f} {:.2f}, opacity {:.2f}", A.id, w.srgb[0], w.srgb[1], w.srgb[2], w.scum);
                    }
                    A.coat = 1.0f;
                    A.coatLeft = std::clamp(w.schlong, 0.0f, 1.0f);
                    A.coatSecs = 1e6f;   // the After sex page decides how long: no drying of our own on top
                    A.coatAlpha = std::clamp(w.scum, 0.05f, 1.0f);
                    A.dripOn = w.sdrip;
                    for (int i = 0; i < 3; ++i) A.coatRgb[i] = w.srgb[i];
                }
            }
            for (auto& [id, A] : g_act)   // he dried off (or the shine was switched off): the cum goes with it
                if (A.coatSecs > 1e5f && A.swet <= 0.01f && A.coatLeft > 0.0f) { A.coatLeft = 0.0f; A.coatSecs = 90.0f; }
        }
        if (dt <= 0.0f) return;
        dt = std::min(dt, 0.05f);
        dt *= g_slow;   // Testing page: slow motion to watch it
        const int sub = std::clamp(g.quality, 1, 3);
        const float h = dt / static_cast<float>(sub);
        for (auto it = g_act.begin(); it != g_act.end();) {
            Actor& A = it->second;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(A.id);
            auto* root = a && a->Is3DLoaded() ? a->Get3D(false) : nullptr;
            const bool anything = !A.ropes.empty() || !A.emit.empty() || !A.beads.empty() || A.hang > 0.01f || A.trail > 0.05f || A.wet > 0.01f || A.coatLeft > 0.0f || A.tipTrack > 0.0f ||
                                  A.swet > 0.01f || A.gloss > 0.01f || A.cleftSheen.obj || A.glossSent > 0.0f || A.sglossSent > 0.0f;
            if (!a || !root || !(g.on || !kCum)) {   // without cum (Features.h) the switch is not shown: wetness always runs
                if (A.root3D && A.root3D != root) DropVisuals(A);
                A.root3D = root;
                A.ropes.clear(); A.emit.clear();
                if (!anything || !a) { it = g_act.erase(it); continue; }
                ++it;
                continue;
            }
            if (!anything) { DropVisuals(A); it = g_act.erase(it); continue; }
            if (A.purge) { A.purge = false; DropVisuals(A); }
            if (A.root3D != root) { DropVisuals(A); A.root3D = root; A.body.ok = false; A.rim.ok = false; A.schlong.ok = false; }
            if (!A.vis[3].empty() && A.vis[3][0].host.get() != root->parent) DropVisuals(A);   // he moved into another cell
            // surfaces: re-read the mesh now and then (morphs / outfit changes), pose every frame
            const bool needBody = !A.ropes.empty() || !A.emit.empty() || !A.beads.empty() || A.hang > 0.01f || A.trail > 0.05f || A.wet > 0.01f || A.gloss > 0.01f;
            if (needBody) {
                // the shape he wears right now: an outfit change (scene end, redress) swaps it, so look every frame
                auto* bg = BodyGeo(a, root);
                if (bg != A.body.geo.get()) { A.body.ok = false; if (!bg) A.body.geo.reset(); }
                if (bg && (!A.body.ok || now - A.bodyReadAt > 6.0)) {
                    // around the hips and down the legs, behind and between
                    A.body.Read(bg, [](const V3& p) { return p.z > 18.0f && p.z < 82.0f && p.y < 9.0f; }, 2);
                    A.bodyReadAt = now;
                }
                if (A.body.ok) A.body.Pose();
                UpdateLip(A, a, root);
            }
            if (A.coatLeft > 0.0f || A.tipTrack > 0.0f) {
                auto* sg = SchlongGeo(a);
                if (sg != A.schlong.geo.get()) { A.schlong.ok = false; if (!sg) A.schlong.geo.reset(); }
                if (sg && (!A.schlong.ok || now - A.schlongReadAt > 6.0)) ReadSchlong(A, a);
                if (A.schlong.ok) A.schlong.Pose();
                TipOf(a, A);
                A.coatLeft = std::max(0.0f, A.coatLeft - dt / std::max(A.coatSecs, 5.0f));
                if (A.coatLeft <= 0.0f && A.tipTrack <= 0.0f) { A.coat = 0.0f; }
                A.tipTrack = std::max(0.0f, A.tipTrack - dt);
            } else if (A.schlong.ok || A.tipOk) { A.tipOk = false; A.schlong = Surface{}; }
            UpdateGloss(A, a);
            A.ropes.reserve(A.ropes.size() + 256);   // pieces born during the step never move the ones being stepped
            Wanted(A, now, dt);
            if (!A.lip.empty()) { g_holeC = A.lipC; g_holeN = A.lipN; g_holeR = A.lipR; } else g_holeR = 0.0f;
            for (int s = 0; s < sub; ++s) {
                for (auto& e : A.emit) StepEmitter(A, e, h);
                const size_t n = A.ropes.size();
                for (size_t i = 0; i < n; ++i) {
                    Rope& r = A.ropes[i];
                    if (r.dead) continue;
                    switch (r.type) {
                        case Type::Strand:
                            if (r.onSchlong && (!A.schlong.ok || r.anchorSurf < 0)) { r.dead = true; break; }
                            if (!r.onSchlong && A.lip.empty()) { r.dead = true; break; }
                            SelfGrow(r, h);
                            StepStrand(A, r, h, sub);
                            if (r.dropNow) {
                                r.dropNow = false;
                                const bool last = r.holdFor > 0.0f;
                                DropOff(A, A.ropes[i], A.ropes[i].vol * 0.6f);
                                if (last) A.ropes[i].dead = true;
                            }
                            break;
                        case Type::String: {
                            Actor* P = nullptr;
                            if (!r.virtualPartner) {
                                auto pit = g_act.find(r.partner);
                                if (pit != g_act.end()) P = &pit->second;
                            }
                            StepString(A, r, P, h, sub);
                            break;
                        }
                        case Type::Free: StepFree(A, r, h, sub); break;
                        case Type::Rivulet: StepRivulet(A, r, h); break;
                    }
                }
            }
            A.emit.erase(std::remove_if(A.emit.begin(), A.emit.end(), [](const Emitter& e) { return e.dur <= 0.0f || e.t > 6.0f; }), A.emit.end());
            // age, dry, fade
            for (auto& r : A.ropes) {
                r.age += dt;
                if (r.type == Type::Rivulet) {
                    const float dryT = r.life;
                    r.alpha = std::clamp(1.0f - (r.age - dryT * 0.6f) / std::max(dryT * 0.4f, 1.0f), 0.0f, 1.0f) * (r.bead ? std::clamp(0.4f + A.wet, 0.4f, 1.0f) : 1.0f);
                    if (r.age > dryT) r.dead = true;
                    if (r.bead && A.wet < 0.02f && r.age > 3.0f) r.life = std::min(r.life, r.age + 4.0f);
                } else {
                    if (r.age > r.life) r.dead = true;
                    r.alpha = std::clamp((r.life - r.age) / 0.6f, 0.0f, 1.0f);
                }
                if (r.onSchlong) r.alpha *= A.coatAlpha;
                for (auto& p : r.p) if (!std::isfinite(p.x.x) || !std::isfinite(p.x.y) || !std::isfinite(p.x.z) || (r.type == Type::Free && Len(p.x - A.lipC) > 300.0f)) r.dead = true;
                if (r.type == Type::Rivulet && !std::isfinite(r.head.x)) r.dead = true;
            }
            A.ropes.erase(std::remove_if(A.ropes.begin(), A.ropes.end(), [](const Rope& r) { return r.dead; }), A.ropes.end());
            if (A.ropes.size() > 80) A.ropes.erase(A.ropes.begin(), A.ropes.begin() + static_cast<long>(A.ropes.size() - 80));
            Draw(A);
            Trace(A, root, now);
            A.lastSeen = now;
            ++it;
        }
    }

    void Loop() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(6));
            {
                std::lock_guard lk(g_mx);
                if (g_act.empty()) {
                    // nobody wet or cummed on: still look every half second for a man who just got wet
                    g_lastTick = 0.0;
                    static auto idle = std::chrono::steady_clock::time_point{};
                    const auto t = std::chrono::steady_clock::now();
                    if (t - idle < std::chrono::milliseconds(500)) continue;
                    idle = t;
                }
            }
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->GameIsPaused()) { g_lastTick = 0.0; continue; }
            if (g_queued.exchange(true)) continue;
            if (auto* t = SKSE::GetTaskInterface()) t->AddTask([]() { Tick(); });
            else g_queued = false;
        }
    }

    void Help(const char* t) {
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::PushTextWrapPos(420.0f); ImGuiMCP::TextUnformatted(t); ImGuiMCP::PopTextWrapPos(); ImGuiMCP::EndTooltip(); }
    }
}

namespace Fluid {
    void ForgetMade() { g_forgetMade = true; }
    void Install() { std::thread(Loop).detach(); }

    void Reset() {
        std::lock_guard lk(g_mx);
        for (auto& [id, A] : g_act) DropVisuals(A);
        g_texCache.clear();
        g_act.clear();
        g_land.clear();
    }

    void Hang(RE::FormID id, float amount, float phase, const float rgb[3]) {
        if (!kCum) return;   // cum is off in this build (Features.h)
        std::lock_guard lk(g_mx);
        if (amount <= 0.01f && g_act.find(id) == g_act.end()) return;
        auto& A = Get(id);
        A.hang = amount; A.hangPhase = std::clamp(phase, 0.0f, 1.0f);
        for (int i = 0; i < 3; ++i) A.hangRgb[i] = rgb[i];
    }
    void Release(RE::FormID id, float amount) {
        if (!kCum) return;   // cum is off in this build (Features.h)
        std::lock_guard lk(g_mx);
        auto it = g_act.find(id);
        if (it == g_act.end()) { g_land.push_back({id, amount}); return; }
        Actor& A = it->second;
        for (auto& r : A.ropes)
            if (r.type == Type::Strand && r.lip >= 0 && r.holdFor <= 0.0f && !r.dead) {
                r.vol = std::max(r.vol, amount);
                r.dropNow = true;   // drops off on the next step
                return;
            }
        g_land.push_back({id, amount});   // nothing hanging (not loaded): straight to a puddle
    }
    void Squirt(RE::FormID id, float amount, const float rgb[3], Kind kind, float hold) {
        if (!kCum) return;   // cum is off in this build (Features.h)
        std::lock_guard lk(g_mx);
        auto& A = Get(id);
        Emitter e;
        e.kind = kind; e.amount = amount; e.hold = hold;
        e.dur = 1.0f; e.pulses = kind == Kind::Land ? 1 : (amount > 0.5f ? 3 : 2);
        for (int i = 0; i < 3; ++i) e.rgb[i] = rgb[i];
        A.emit.push_back(e);
        g_traceUntil = Now() + 2.5; g_traceNext = 0.0;
    }
    void String(RE::FormID id, RE::FormID partner, const float rgb[3]) {
        if (!kCum) return;   // cum is off in this build (Features.h)
        std::lock_guard lk(g_mx);
        auto& A = Get(id);
        auto& P = Get(partner);
        P.tipTrack = 30.0f;   // keep his tip tracked while the string lasts
        Rope r;
        r.type = Type::String; r.partner = partner; r.vol = 0.35f;
        for (int i = 0; i < 3; ++i) r.rgb[i] = rgb[i];
        r.breakAt = g.breakDist * Rand(0.7f, 1.3f);
        const V3 a = A.lip.empty() ? V3{} : A.lipC;
        for (int i = 0; i < 16; ++i) { Pt p; p.x = p.xp = a; p.r = 0.05f; r.p.push_back(p); }
        r.p.front().pin = r.p.back().pin = true;
        r.rest.assign(15, 0.02f);
        r.life = 30.0f;
        A.ropes.push_back(std::move(r));
    }
    bool TestString(RE::Actor* a, const float rgb[3]) {
        if (!kCum || !a) return false;
        std::lock_guard lk(g_mx);
        auto& A = Get(a->GetFormID());
        Rope r;
        r.type = Type::String; r.virtualPartner = true; r.vol = 0.35f;
        for (int i = 0; i < 3; ++i) r.rgb[i] = rgb[i];
        r.breakAt = g.breakDist * Rand(0.7f, 1.3f);
        if (A.lip.empty()) {   // not posed yet: start from his pelvis
            auto* root = a->Get3D(false);
            auto* pel = root ? root->GetObjectByName(RE::BSFixedString("NPC Pelvis [Pelv]")) : nullptr;
            if (!pel) return false;
            const Xf P(pel->world);
            r.vpStart = P.Apply(V3(0, -5.6f, -6.4f));
            r.vpDir = Norm(P.Rot(V3(0, -0.9f, -0.2f)));
        } else {
            r.vpStart = A.lipC + A.lipN * 0.3f;
            r.vpDir = Norm(A.lipN + V3(0, 0, 0.3f));
        }
        for (int i = 0; i < 16; ++i) { Pt p; p.x = p.xp = r.vpStart; p.r = 0.05f; r.p.push_back(p); }
        r.p.front().pin = r.p.back().pin = true;
        r.rest.assign(15, 0.02f);
        r.life = 20.0f;
        A.ropes.push_back(std::move(r));
        A.hang = std::max(A.hang, 0.0f);
        return true;
    }
    void Trail(RE::FormID id, float level, int side, const float rgb[3]) {
        if (!kCum) return;   // cum is off in this build (Features.h)
        std::lock_guard lk(g_mx);
        if (level <= 0.05f) {
            auto it = g_act.find(id);
            if (it == g_act.end()) return;
            it->second.trail = 0.0f; it->second.trailDone = 0.0f;
            return;
        }
        auto& A = Get(id);
        A.trail = level; A.trailSide = side;
        for (int i = 0; i < 3; ++i) A.trailRgb[i] = rgb[i];
    }
    void Coat(RE::FormID giver, float amount, const float rgb[3], float seconds) {
        if (!kCum) return;   // cum is off in this build (Features.h)
        std::lock_guard lk(g_mx);
        auto& A = Get(giver);
        A.coat = std::clamp(amount, 0.1f, 1.5f);
        A.coatLeft = 1.0f; A.coatSecs = std::max(seconds, 10.0f);
        for (int i = 0; i < 3; ++i) A.coatRgb[i] = rgb[i];
        A.coatDrip = Now() + 1.5;
        A.tipOk = true;
        A.coatLogged = false;
        A.dripOn = true;
    }
    void Clear(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_act.find(id);
        if (it == g_act.end()) return;
        auto& A = it->second;
        A.ropes.clear(); A.emit.clear(); A.beads.clear();
        A.hang = 0.0f; A.trail = 0.0f; A.trailDone = 0.0f; A.coatLeft = 0.0f; A.coat = 0.0f; A.tipOk = false;
        A.purge = true;
    }
    void SlowMotion(bool on) { g_slow = on ? 0.15f : 1.0f; }
    bool SlowMotion() { return g_slow < 0.99f; }
    float WetOf(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_act.find(id);
        return it == g_act.end() ? 0.0f : it->second.wet;
    }
    std::vector<RE::FormID> WetMen() {
        std::lock_guard lk(g_mx);
        std::vector<RE::FormID> out;
        for (auto& [id, A] : g_act) if (A.wet > 0.02f) out.push_back(id);
        return out;
    }
    bool Busy(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_act.find(id);
        return it != g_act.end() && !it->second.emit.empty();
    }
    std::vector<std::pair<RE::FormID, float>> TakeLandings() {
        std::lock_guard lk(g_mx);
        std::vector<std::pair<RE::FormID, float>> out;
        out.swap(g_land);
        return out;
    }
    std::string Status(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_act.find(id);
        if (it == g_act.end()) return kCum ? "no cum on him" : "dry";
        const auto& A = it->second;
        if (!kCum)
            return std::format("Wet look: {} beads, hole {:.0f}%, schlong {:.0f}% | body {}", A.beads.size(), std::max(A.glossSent, 0.0f) * 100.0f,
                               std::max(A.sglossSent, 0.0f) * 100.0f, A.body.ok ? std::format("{} pts", A.body.wp.size()) : "-");
        int n[4] = {};
        for (auto& r : A.ropes) n[static_cast<int>(r.type)]++;
        return std::format("{} hanging, {} string, {} drops, {} on the skin, {} beads, {} drawn | lip {} | body {} | schlong {}", n[0], n[1], n[2], n[3],
                           A.beads.size(), A.vis[0].size() + A.vis[1].size() + A.vis[2].size() + A.vis[3].size(),
                           A.lipOk ? "found" : "estimated", A.body.ok ? std::format("{} pts", A.body.wp.size()) : "-",
                           A.schlong.ok ? std::format("{} pts", A.schlong.wp.size()) : "-");
    }

    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "Fluid";
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto F = [&](const char* k, float& v, float lo, float hi) {
            if (save) ini.SetDoubleValue(S, k, v);
            else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi);
        };
        auto I = [&](const char* k, int& v, int lo, int hi) {
            if (save) ini.SetLongValue(S, k, v);
            else v = std::clamp(static_cast<int>(ini.GetLongValue(S, k, v)), lo, hi);
        };
        B("On", g.on); F("Thickness", g.thick, 0.4f, 2.5f); F("Opacity", g.opacity, 0.2f, 1.0f);
        F("Shine", g.shine, 0.0f, 2.0f); F("Gloss", g.gloss, 20.0f, 1000.0f); F("Gravity", g.gravity, 100.0f, 1200.0f);
        F("Viscosity", g.viscosity, 0.0f, 1.0f); F("Stickiness", g.stick, 0.0f, 1.0f); F("MaxLength", g.maxLen, 2.0f, 30.0f);
        F("BreakDistance", g.breakDist, 2.0f, 40.0f); F("Flow", g.flow, 0.2f, 3.0f); F("DryTime", g.dry, 20.0f, 1200.0f);
        I("MaxPieces", g.maxPieces, 4, 48); B("Collide", g.collide); B("WetFilm", g.wetFilm); I("BeadMax", g.beadMax, 8, 64); F("BeadSize", g.beadSize, 0.3f, 3.0f); B("CoatShine", g.coatShine); I("Quality", g.quality, 1, 3);
        B("Cleft", g.cleft); F("CleftAmount", g.cleftAmt, 0.0f, 1.0f); F("CleftSize", g.cleftSize, 0.5f, 2.0f); F("RimShine", g.rimShine, 0.5f, 5.0f);
    }

    // the wet rim (After sex page, under Wetness)
    void RenderWetSettings() {
        bool ch = false;
        ch |= ImGuiMCP::SliderFloat("Wet rim shine", &g.rimShine, 0.5f, 5.0f, "x%.1f");
        Help("How glassy the rim gets while he is wet: higher = a sharper, brighter, more water-like shine.");
        if (ch) Colour::SaveNow();
    }
    // the glossy skin's reach (After sex page, glossy skin)
    void RenderGlossSettings() {
        bool ch = false;
        ch |= ImGuiMCP::SliderFloat("How far it reaches", &g.cleftSize, 0.5f, 2.0f, "x%.2f");
        Help("Around the hole and how far up the crevice. A new size is drawn the next time he gets glossy.");
        if (ch) Colour::SaveNow();
    }

    void RenderSettings() {
        bool ch = false;
        ch |= ImGuiMCP::Checkbox("Simulated cum (strands, streams, drops, running down him)", &g.on);
        Help("Everything here is simulated by the DLL every frame: it hangs from the real edge of his hole, runs down his "
             "actual body (OBody / BodySlide shape included) and drops to the floor. Off = none of it is shown.");
        if (!g.on) { if (ch) Colour::SaveNow(); return; }
        ImGuiMCP::SeparatorText("Look");
        ch |= ImGuiMCP::SliderFloat("Thickness", &g.thick, 0.4f, 2.5f, "x%.2f");
        ch |= ImGuiMCP::SliderFloat("Opacity", &g.opacity, 0.2f, 1.0f, "%.2f");
        Help("How see-through it is (the colour itself is on this page: per race or your own).");
        ch |= ImGuiMCP::SliderFloat("Shine", &g.shine, 0.0f, 2.0f, "%.2f");
        ch |= ImGuiMCP::SliderFloat("Gloss", &g.gloss, 20.0f, 1000.0f, "%.0f");
        Help("Higher = a smaller, sharper highlight, like a wet glassy surface. Also used for the wet look on his anus.");
        ImGuiMCP::SeparatorText("How it moves");
        ch |= ImGuiMCP::SliderFloat("Thickness of the fluid (viscosity)", &g.viscosity, 0.0f, 1.0f, "%.2f");
        Help("0 = runny: falls fast, strands snap back. 1 = thick and gooey: stretches slowly, clings, drips late.");
        ch |= ImGuiMCP::SliderFloat("Stickiness on skin", &g.stick, 0.0f, 1.0f, "%.2f");
        ch |= ImGuiMCP::SliderFloat("Runs down the skin", &g.flow, 0.2f, 3.0f, "x%.1f");
        ch |= ImGuiMCP::SliderFloat("Gravity", &g.gravity, 100.0f, 1200.0f, "%.0f");
        ch |= ImGuiMCP::SliderFloat("Longest strand before it drips", &g.maxLen, 2.0f, 30.0f, "%.1f");
        ch |= ImGuiMCP::SliderFloat("String to him snaps at", &g.breakDist, 2.0f, 40.0f, "%.1f");
        Help("When he pulls out a string can stretch between his hole and the tip of the partner's schlong. It thins as "
             "it gets longer and snaps around this distance (+-30%): one half springs back and hangs from the hole, the "
             "other stays on his tip and drips.");
        ch |= ImGuiMCP::Checkbox("Collides with the body", &g.collide);
        ImGuiMCP::SeparatorText("Wet and leftovers");
        ch |= ImGuiMCP::Checkbox("Water beads around his hole while he is wet", &g.wetFilm);
        Help("Like water on skin: fine droplets when he is barely wet, more and bigger beads the wetter he gets. They keep "
             "gathering while he stays wet, run together when they touch, and one too heavy for the skin runs down as a "
             "trickle. As he dries they shrink and go. Clear water, or the wetness colour (After sex page) when that is on.");
        if (g.wetFilm) {
            ImGuiMCP::Indent();
            ch |= ImGuiMCP::SliderInt("Most beads", &g.beadMax, 8, 64);
            ch |= ImGuiMCP::SliderFloat("Bead size", &g.beadSize, 0.3f, 3.0f, "x%.2f");
            ImGuiMCP::TextDisabled("How long the skin holds a bead before it runs: Stickiness on skin (above).");
            ImGuiMCP::Unindent();
        }
        ch |= ImGuiMCP::Checkbox("The giver's schlong glistens while coated", &g.coatShine);
        ch |= ImGuiMCP::SliderFloat("Drying time", &g.dry, 20.0f, 1200.0f, "%.0f s");
        Help("How long trails on the skin, beads and streaks take to dry off.");
        ImGuiMCP::SeparatorText("Performance");
        ch |= ImGuiMCP::SliderInt("Pieces drawn per man", &g.maxPieces, 4, 48);
        ch |= ImGuiMCP::SliderInt("Simulation quality", &g.quality, 1, 3);
        if (ch) Colour::SaveNow();
    }

    // ---- the hole follows the schlong (During sex page): its thickness where it passes through his hole
    namespace {
        // shaft[i]: the vertex belongs to the shaft (weighted at least half to Genitals01-06) - not the scrotum, the base
        // skin or the pelvis, which hang across the entrance when he is deep and pulled the measured centre down
        struct Girth { Surface s; double readAt = 0.0; std::vector<std::uint8_t> shaft; };
        void ReadGirth(Girth& G, RE::BSGeometry* geo, std::uint32_t most) {
            G.s.Read(geo, [](const V3&) { return true; }, std::max(1, static_cast<int>(most / 1500)));
            G.shaft.assign(G.s.v.size(), 1);
            auto* si = geo ? geo->GetGeometryRuntimeData().skinInstance.get() : nullptr;
            if (!G.s.ok || !si || !si->skinData || !si->bones) return;
            const std::uint32_t nb = si->skinData->GetBoneCount();
            std::vector<std::uint8_t> isShaft(nb, 0);
            for (std::uint32_t b = 0; b < nb; ++b)
                if (si->bones[b] && std::strstr(si->bones[b]->name.c_str(), "Genitals0")) isShaft[b] = 1;
            size_t kept = 0;
            std::vector<std::uint8_t> m(G.s.v.size(), 0);
            for (size_t i = 0; i < G.s.v.size(); ++i) {
                float w = 0.0f;
                for (int k = 0; k < 4; ++k) if (G.s.bi[i][k] < nb && isShaft[G.s.bi[i][k]]) w += G.s.bw[i][k];
                if (w >= 0.5f) { m[i] = 1; ++kept; }
            }
            if (kept >= 30) G.shaft = std::move(m);   // a schlong not on these bones: keep everything
        }
        std::unordered_map<RE::FormID, Girth> g_girth;
        std::mutex g_girthMx;
    }
    float GirthAt(RE::Actor* giver, const RE::NiPoint3& point, const RE::NiPoint3& normal) {
        if (!giver) return -1.0f;
        std::lock_guard lk(g_girthMx);
        std::uint32_t most = 0;
        RE::BSGeometry* geo = SchlongGeo(giver, &most);
        if (!geo) return -1.0f;
        Girth& G = g_girth[giver->GetFormID()];
        const double now = Now();
        if (G.s.geo.get() != geo || now - G.readAt > 8.0) {
            ReadGirth(G, geo, most);
            G.readAt = now;
        }
        if (!G.s.ok || !G.s.Pose()) return -1.0f;
        const V3 c(point), n = Norm(V3(normal));
        // the vertices in a thin slice through the plane: their spread round the slice's centre is his thickness there
        std::vector<V3> sl;
        for (float band : {0.35f, 0.7f}) {
            sl.clear();
            for (const auto& w : G.s.wp) if (std::abs(Dot(w - c, n)) < band) sl.push_back(w);
            if (sl.size() >= 8) break;
        }
        if (sl.size() < 8) return -1.0f;
        V3 m;
        for (const auto& w : sl) m += w;
        m = m / static_cast<float>(sl.size());
        std::vector<float> r;
        r.reserve(sl.size());
        for (const auto& w : sl) { V3 d = w - m; d = d - n * Dot(d, n); r.push_back(Len(d)); }
        std::sort(r.begin(), r.end());
        return r[r.size() * 3 / 4];   // the outer part of the slice (a few stray inner vertices do not shrink it)
    }
    std::string SchlongDesc(RE::Actor* giver) {
        if (!giver) return "none";
        std::uint32_t most = 0;
        RE::BSGeometry* g = SchlongGeo(giver, &most);
        if (!g) return "none";
        return std::format("{} ({} verts)", g->name.c_str() ? g->name.c_str() : "?", most);
    }
    std::string SchlongBones(RE::Actor* giver) {
        RE::BSGeometry* g = giver ? SchlongGeo(giver) : nullptr;
        if (!g) return "-";
        auto* si = g->GetGeometryRuntimeData().skinInstance.get();
        if (!si || !si->skinData || !si->bones) return "no skin";
        const std::uint32_t nb = si->skinData->GetBoneCount();
        std::string out = std::format("{} bones:", nb);
        for (std::uint32_t b = 0; b < nb && b < 24; ++b) out += std::format(" {}", si->bones[b] ? si->bones[b]->name.c_str() : "null");
        return out;
    }
    std::vector<RE::NiPoint3> SchlongSample(RE::Actor* giver, int n) {
        std::vector<RE::NiPoint3> out;
        if (!giver || n <= 0) return out;
        std::lock_guard lk(g_girthMx);
        std::uint32_t most = 0;
        RE::BSGeometry* geo = SchlongGeo(giver, &most);
        if (!geo) return out;
        Girth& G = g_girth[giver->GetFormID()];
        const double now = Now();
        if (G.s.geo.get() != geo || now - G.readAt > 8.0) {
            ReadGirth(G, geo, most);
            G.readAt = now;
        }
        if (!G.s.ok || !G.s.Pose()) return out;
        const size_t step = std::max<size_t>(1, G.s.wp.size() / static_cast<size_t>(n));
        for (size_t k = 0; k < G.s.wp.size(); k += step) out.push_back({G.s.wp[k].x, G.s.wp[k].y, G.s.wp[k].z});
        return out;
    }
    SchlongReport ReportSchlong(RE::Actor* giver, const RE::NiPoint3& origin, const RE::NiPoint3& axis, float sc) {
        SchlongReport R;
        if (!giver) return R;
        auto* root = giver->Get3D(false);
        if (!root) { R.lines.push_back("  no 3D"); return R; }
        RE::BSGeometry* picked = SchlongGeo(giver);
        // his schlong bone line
        std::vector<V3> gen;
        for (const char* nm : {"NPC Genitals01 [Gen01]", "NPC Genitals02 [Gen02]", "NPC Genitals03 [Gen03]", "NPC Genitals04 [Gen04]",
                               "NPC Genitals05 [Gen05]", "NPC Genitals06 [Gen06]"})
            if (auto* n = root->GetObjectByName(RE::BSFixedString(nm))) gen.push_back(V3(n->world.translate));
        auto distLine = [&](const V3& p) {
            float best = 1e9f;
            for (size_t k = 0; k + 1 < gen.size(); ++k) {
                const V3 a = gen[k], b = gen[k + 1], ab = b - a;
                const float l2 = Dot(ab, ab);
                const float t = l2 > 1e-6f ? std::clamp(Dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
                best = std::min(best, Len(p - (a + ab * t)));
            }
            return best;
        };
        const V3 o(origin), ax = Norm(V3(axis));
        auto inHole = [&](const V3& p) {
            const V3 d = p - o; const float t = Dot(d, ax);
            return t > -0.3f * sc && t < 9.0f * sc && Len(d - ax * t) < 3.2f * sc;
        };
        RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geo) {
            auto& rd = geo->GetGeometryRuntimeData();
            auto* si = rd.skinInstance.get();
            if (!si || !si->skinData || !si->bones) return RE::BSVisit::BSVisitControl::kContinue;
            const std::uint32_t nb = si->skinData->GetBoneCount();
            bool genBone = false;
            for (std::uint32_t b = 0; b < nb; ++b)
                if (si->bones[b] && std::strstr(si->bones[b]->name.c_str(), "Genitals")) { genBone = true; break; }
            if (!genBone) return RE::BSVisit::BSVisitControl::kContinue;
            {   // RaceMenu overlay copies of the body: identical, only clutter
                const std::string_view nm = geo->name.c_str() ? geo->name.c_str() : "";
                if (nm.find("[Ovl") != std::string_view::npos || nm.find("[SOvl") != std::string_view::npos) return RE::BSVisit::BSVisitControl::kContinue;
            }
            RE::NiSkinPartition* sp = si->skinPartition.get();
            if (!sp) sp = si->skinData->skinPartition.get();
            bool hidden = false;
            for (RE::NiAVObject* x = geo; x; x = x->parent) if (x->GetFlags().any(RE::NiAVObject::Flag::kHidden)) { hidden = true; break; }
            std::string line = std::format("  mesh '{}'{} type {} | {} verts | {} partitions | {} bones | {}{}", geo->name.c_str() ? geo->name.c_str() : "?",
                geo == picked ? " [PICKED]" : "", static_cast<int>(geo->GetType().get()), sp ? sp->vertexCount : 0, sp ? sp->numPartitions : 0, nb,
                hidden ? "HIDDEN" : "shown", geo->parent ? std::format(" | under '{}'", geo->parent->name.c_str() ? geo->parent->name.c_str() : "?") : "");
            if (!sp || sp->numPartitions == 0 || !sp->partitions[0].buffData || !sp->partitions[0].buffData->rawVertexData) {
                R.lines.push_back(line + " | no vertex data");
                return RE::BSVisit::BSVisitControl::kContinue;
            }
            for (std::uint32_t p = 0; p < sp->numPartitions && p < 6; ++p) line += std::format(" | part{}: {}v {}b", p, sp->partitions[p].vertices, sp->partitions[p].numBones);
            R.lines.push_back(line);
            auto& part0 = sp->partitions[0];
            const auto desc = part0.vertexDesc;
            std::uint64_t raw = 0;
            std::memcpy(&raw, &desc, sizeof(raw));
            const std::uint32_t stride = static_cast<std::uint32_t>(raw & 0xF) * 4;
            if (stride == 0 || !desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED)) { R.lines.push_back("    not skinned vertex data"); return RE::BSVisit::BSVisitControl::kContinue; }
            const bool full = FullPos(desc, stride);
            const std::uint32_t oP = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION);
            const std::uint32_t oS = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
            const std::uint8_t* d = part0.buffData->rawVertexData;
            const std::uint32_t count = sp->vertexCount;
            {   // the raw facts: how the vertex data is laid out, and where the engine itself thinks the mesh is
                const auto& wb = geo->worldBound;
                R.lines.push_back(std::format("    raw: desc {:016X} | stride {} | pos at {} | skin at {} | {} | engine bound centre {:.0f},{:.0f},{:.0f} r {:.0f} ({:.1f} units from the hole)",
                    raw, stride, oP, oS, full ? "full floats" : "half floats", wb.center.x, wb.center.y, wb.center.z, wb.radius, Len(V3(wb.center) - o) / sc));
                for (std::uint32_t i = 0; i < 3 && i < count; ++i) {
                    const std::uint8_t* q = d + static_cast<size_t>(i) * stride;
                    std::string hex;
                    for (std::uint32_t b = 0; b < stride && b < 48; ++b) hex += std::format("{:02X}", q[b]);
                    R.lines.push_back(std::format("    v{} bytes {}", i, hex));
                }
                if (nb > 0 && si->bones[0]) {
                    const auto& bw0 = si->bones[0]->world;
                    const auto& sk0 = si->skinData->GetBoneDataSkinToBone(0);
                    R.lines.push_back(std::format("    bone0 '{}' world {:.1f},{:.1f},{:.1f} s{:.2f} | skinToBone {:.1f},{:.1f},{:.1f} s{:.2f} | rootParentToSkin {:.1f},{:.1f},{:.1f} s{:.2f}",
                        si->bones[0]->name.c_str(), bw0.translate.x, bw0.translate.y, bw0.translate.z, bw0.scale, sk0.translate.x, sk0.translate.y, sk0.translate.z, sk0.scale,
                        si->skinData->rootParentToSkin.translate.x, si->skinData->rootParentToSkin.translate.y, si->skinData->rootParentToSkin.translate.z, si->skinData->rootParentToSkin.scale));
                }
            }
            std::vector<Xf> M(nb);
            for (std::uint32_t b = 0; b < nb; ++b) M[b] = si->bones[b] ? Mul(Xf(si->bones[b]->world), Xf(si->skinData->GetBoneDataSkinToBone(b))) : Xf();
            const Xf rp(si->skinData->rootParentToSkin);
            // dynamic vertex data (a morphable shape keeps its live positions there)
            const float* dyn = nullptr; std::uint32_t dynStride = 0;
            if (auto* dts = geo->AsDynamicTriShape()) {
                auto& dd = dts->GetDynamicTrishapeRuntimeData();
                if (dd.dynamicData && count) { dyn = static_cast<const float*>(dd.dynamicData); dynStride = dd.dataSize / count / 4; }
            }
            auto pos = [&](std::uint32_t i) {
                const std::uint8_t* q = d + static_cast<size_t>(i) * stride;
                if (full) { float f[3]; std::memcpy(f, q + oP, 12); return V3(f[0], f[1], f[2]); }
                std::uint16_t h[3]; std::memcpy(h, q + oP, 6); return V3(Half(h[0]), Half(h[1]), Half(h[2]));
            };
            auto skin = [&](std::uint32_t i, const V3& v, const std::uint16_t* remap, std::uint16_t remapN, bool withRP) {
                const std::uint8_t* q = d + static_cast<size_t>(i) * stride;
                std::uint16_t hw[4]; std::memcpy(hw, q + oS, 8);
                std::uint8_t bb[4]; std::memcpy(bb, q + oS + 8, 4);
                V3 out; float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    std::uint32_t b = bb[k];
                    if (remap) { if (b >= remapN) continue; b = remap[b]; }
                    if (b >= nb) continue;
                    const float w = std::max(0.0f, Half(hw[k]));
                    if (w <= 0.0f) continue;
                    out += (withRP ? Mul(M[b], rp) : M[b]).Apply(v) * w; sum += w;
                }
                return sum > 1e-4f ? out / sum : V3();
            };
            auto inv = [](const Xf& a) {
                Xf o;
                for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) o.r[r][c] = a.r[c][r];
                o.s = a.s != 0.0f ? 1.0f / a.s : 1.0f;
                o.t = o.Rot(a.t * -1.0f) * o.s;
                return o;
            };
            std::vector<Xf> Mi(nb);
            for (std::uint32_t b = 0; b < nb; ++b) Mi[b] = si->bones[b] ? Mul(Xf(si->bones[b]->world), inv(Xf(si->skinData->GetBoneDataSkinToBone(b)))) : Xf();
            std::vector<V3> va[6];
            const std::uint32_t step = std::max<std::uint32_t>(1, count / 300);
            for (std::uint32_t i = 0; i < count; i += step) {
                const V3 v = pos(i);
                va[0].push_back(skin(i, v, nullptr, 0, false));
                va[2].push_back(skin(i, v, nullptr, 0, true));
                va[4].push_back(Xf(geo->world).Apply(v));   // E: no skinning, just the shape's own transform
                {   // F: the skin transform inverted
                    const std::uint8_t* q = d + static_cast<size_t>(i) * stride;
                    std::uint16_t hw[4]; std::memcpy(hw, q + oS, 8);
                    std::uint8_t bb[4]; std::memcpy(bb, q + oS + 8, 4);
                    V3 out; float sum = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        if (bb[k] >= nb) continue;
                        const float w = std::max(0.0f, Half(hw[k]));
                        if (w <= 0.0f) continue;
                        out += Mi[bb[k]].Apply(v) * w; sum += w;
                    }
                    va[5].push_back(sum > 1e-4f ? out / sum : V3());
                }
                if (dyn && dynStride >= 3) va[3].push_back(skin(i, V3(dyn[i * dynStride], dyn[i * dynStride + 1], dyn[i * dynStride + 2]), nullptr, 0, false));
            }
            for (std::uint32_t p = 0; p < sp->numPartitions; ++p) {   // B: bone indices as partition-local
                auto& pt = sp->partitions[p];
                if (!pt.bones) continue;
                for (std::uint32_t lv = 0; lv < pt.vertices; lv += step) {
                    const std::uint32_t g = pt.vertexMap ? pt.vertexMap[lv] : lv;
                    if (g >= count) continue;
                    va[1].push_back(skin(g, pos(g), pt.bones, pt.numBones, false));
                }
            }
            static const char* kV[6] = {"A as read now", "B partition bones", "C rootParentToSkin", "D dynamic data", "E no skinning", "F skin inverted"};
            for (int k = 0; k < 6; ++k) {
                if (va[k].empty()) { R.lines.push_back(std::format("    {}: -", kV[k])); continue; }
                std::vector<float> dl;
                int hole = 0, bad = 0, n = 0;
                V3 cen;
                for (const auto& p : va[k]) {
                    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || std::abs(p.x) > 1e6f || std::abs(p.y) > 1e6f || std::abs(p.z) > 1e6f) { ++bad; continue; }
                    dl.push_back(distLine(p) / sc); if (inHole(p)) ++hole; cen += p; ++n;
                }
                if (dl.empty()) { R.lines.push_back(std::format("    {}: all {} points garbage", kV[k], bad)); continue; }
                cen = cen / static_cast<float>(n);
                std::sort(dl.begin(), dl.end());
                const V3 cd = cen - o;
                R.lines.push_back(std::format("    {}: from his bone line median {:.1f} / max {:.1f} units | in the hole {}/{} | garbage {} | centre {:.0f},{:.0f},{:.0f} = {:.1f} units from the hole",
                    kV[k], dl[dl.size() / 2], dl.back(), hole, va[k].size(), bad, cen.x, cen.y, cen.z, Len(cd) / sc));
                if (k >= 4) continue;   // E / F: numbers only
                if (geo == picked) {
                    const size_t st = std::max<size_t>(1, va[k].size() / 70);
                    for (size_t j = 0; j < va[k].size(); j += st) R.samples[k].push_back({va[k][j].x, va[k][j].y, va[k][j].z});
                } else if (k == 0) {
                    const size_t st = std::max<size_t>(1, va[k].size() / 40);
                    for (size_t j = 0; j < va[k].size(); j += st) R.others.push_back({va[k][j].x, va[k][j].y, va[k][j].z});
                }
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        if (R.lines.empty()) R.lines.push_back("  no mesh skinned to his schlong bones");
        return R;
    }
    int InTube(RE::Actor* giver, const RE::NiPoint3& origin, const RE::NiPoint3& axis, float radius, float d0, float d1, float* deepest) {
        if (deepest) *deepest = -1e9f;
        if (!giver) return 0;
        std::lock_guard lk(g_girthMx);
        std::uint32_t most = 0;
        RE::BSGeometry* geo = SchlongGeo(giver, &most);
        if (!geo) return 0;
        Girth& G = g_girth[giver->GetFormID()];
        const double now = Now();
        if (G.s.geo.get() != geo || now - G.readAt > 8.0) {
            ReadGirth(G, geo, most);
            G.readAt = now;
        }
        if (!G.s.ok || !G.s.Pose()) return 0;
        const V3 o(origin), n = Norm(V3(axis));
        const float r2 = radius * radius;
        int cnt = 0;
        for (size_t i = 0; i < G.s.wp.size(); ++i) {
            if (i < G.shaft.size() && !G.shaft[i]) continue;
            const V3& w = G.s.wp[i];
            const V3 d = w - o;
            const float t = Dot(d, n);
            if (t < d0 || t > d1) continue;
            const V3 side = d - n * t;
            if (Dot(side, side) < r2) { ++cnt; if (deepest && t > *deepest) *deepest = t; }
        }
        return cnt;
    }
    namespace {
        struct RimCache { RE::NiPointer<RE::BSGeometry> geo; double at = 0.0; std::vector<std::pair<std::uint32_t, float>> w; bool ok = false; };
        std::unordered_map<RE::FormID, RimCache> g_rim;
        std::mutex g_rimMx;
        const V3 kHoleO{0.0f, -4.64f, 62.645f}, kHoleAx{0.0f, 0.7675f, 0.641f};   // bind pose (BodySlide units): the hole and its canal
    }
    bool HoleFrame(RE::Actor* a, RE::NiPoint3& centre, RE::NiPoint3& deep) {
        if (!a) return false;
        auto* root = a->Get3D(false);
        if (!root) return false;
        RE::BSGeometry* rim = nullptr;
        RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* g) {
            if (g->name == "3BA_AnusRim") { rim = g; return RE::BSVisit::BSVisitControl::kStop; }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        if (!rim) return false;
        auto* si = rim->GetGeometryRuntimeData().skinInstance.get();
        if (!si || !si->skinData || !si->bones) return false;
        const std::uint32_t nb = si->skinData->GetBoneCount();
        std::lock_guard lk(g_rimMx);
        RimCache& C = g_rim[a->GetFormID()];
        const double now = Now();
        if (C.geo.get() != rim || now - C.at > 10.0) {   // the rim's average bone weights round the hole (they never change)
            C.geo = RE::NiPointer<RE::BSGeometry>(rim); C.at = now; C.ok = false; C.w.clear();
            Surface S;
            if (S.Read(rim, [](const V3& p) { return Len(p - kHoleO) < 2.5f; }, 1)) {
                std::vector<float> acc(nb, 0.0f);
                for (size_t i = 0; i < S.v.size(); ++i)
                    for (int k = 0; k < 4; ++k) if (S.bi[i][k] < nb) acc[S.bi[i][k]] += S.bw[i][k];
                float tot = 0.0f;
                for (float x : acc) tot += x;
                if (tot > 0.0f)
                    for (std::uint32_t b = 0; b < nb; ++b) if (acc[b] > 0.0f) C.w.push_back({b, acc[b] / tot});
                C.ok = !C.w.empty();
            }
        }
        if (!C.ok) return false;
        V3 pc, pd;
        for (const auto& [b, w] : C.w) {
            auto* bone = si->bones[b];
            if (!bone) return false;
            const Xf m = Mul(Xf(bone->world), Xf(si->skinData->GetBoneDataSkinToBone(b)));
            pc += m.Apply(kHoleO) * w;
            pd += m.Apply(kHoleO + kHoleAx * 4.0f) * w;
        }
        centre = {pc.x, pc.y, pc.z};
        deep = {pd.x, pd.y, pd.z};
        return true;
    }
    namespace {
        struct EdgeCache { RE::NiPointer<RE::BSGeometry> geo; std::vector<std::uint32_t> idx; Surface S; };
        std::unordered_map<RE::FormID, EdgeCache> g_edge;
        std::mutex g_edgeMx;
    }
    RimRead RimEdge(RE::Actor* a, const RE::NiPoint3& origin, const RE::NiPoint3& axis, const RE::NiPoint3& right, const RE::NiPoint3& up,
                    const std::vector<float>& ringDepths, int n, float band, bool wantPts) {
        RimRead R;
        n = std::clamp(n, 1, 16);
        if (!a || ringDepths.empty()) return R;
        auto* root = a->Get3D(false);
        if (!root) return R;
        RE::BSGeometry* canal = nullptr;
        RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* g) {
            if (g->name == "3BA_Anus") { canal = g; return RE::BSVisit::BSVisitControl::kStop; }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        if (!canal) return R;
        std::lock_guard lk(g_edgeMx);
        EdgeCache& C = g_edge[a->GetFormID()];
        if (C.geo.get() != canal || C.idx.empty()) {   // the canal's first stretch: its mouth and the rings just inside (by depth, so it is found open or shut)
            C.geo = RE::NiPointer<RE::BSGeometry>(canal);
            C.idx.clear();
            Surface S;
            if (S.Read(canal, [](const V3& p) {
                    const V3 d = p - kHoleO; const float t = Dot(d, kHoleAx);
                    return t > -0.4f && t < 3.2f && Len(d - kHoleAx * t) < 5.0f;
                }, 1))
                C.idx = S.idx;
            if (C.idx.empty()) return R;
        }
        // re-read those vertices now (the morphs are written into them) and pose them with his bones as they are now
        if (!C.S.Read(canal, [](const V3&) { return true; }, 1, &C.idx) || !C.S.Pose()) return R;
        const V3 o(origin), nax = Norm(V3(axis)), rt = Norm(V3(right)), upv = Norm(V3(up));
        const float wN = 6.2831853f / static_cast<float>(n);
        float sum[3][16]{}; int cnt[3][16]{};
        const size_t nr = std::min<size_t>(3, ringDepths.size());
        for (const auto& w : C.S.wp) {
            const V3 d = w - o;
            const float t = Dot(d, nax);
            const V3 lat = d - nax * t;
            const float lx = Dot(lat, rt), ly = Dot(lat, upv);
            for (size_t r = 0; r < nr; ++r) {
                if (std::abs(t - ringDepths[r]) > band) continue;
                float ang = std::atan2(ly, lx);
                if (ang < 0.0f) ang += 6.2831853f;
                const int k = static_cast<int>(std::floor(ang / wN + 0.5f)) % n;
                const float dk = wN * static_cast<float>(k);
                sum[r][k] += lx * std::cos(dk) + ly * std::sin(dk);   // how far the edge reaches that way
                ++cnt[r][k];
                if (wantPts && r == 0) R.pts.push_back({w.x, w.y, w.z});
                break;
            }
        }
        for (size_t r = 0; r < nr; ++r)
            for (int k = 0; k < n; ++k)
                if (cnt[r][k] >= 2) { R.q[r][k] = sum[r][k] / static_cast<float>(cnt[r][k]); R.has[r][k] = true; R.ok = true; }
        return R;
    }
    std::vector<Slice> SlicesAt(RE::Actor* giver, const RE::NiPoint3& origin, const RE::NiPoint3& axis, const RE::NiPoint3& right,
                                const RE::NiPoint3& up, const std::vector<float>& depths, int nDirs, float maxSide, const std::vector<RE::NiPoint3>* hints, bool outline) {
        std::vector<Slice> out(depths.size());
        nDirs = std::clamp(nDirs, 1, 16);
        if (!giver) return out;
        std::lock_guard lk(g_girthMx);
        std::uint32_t most = 0;
        RE::BSGeometry* geo = SchlongGeo(giver, &most);
        if (!geo) return out;
        Girth& G = g_girth[giver->GetFormID()];
        const double now = Now();
        if (G.s.geo.get() != geo || now - G.readAt > 8.0) {
            ReadGirth(G, geo, most);
            G.readAt = now;
        }
        if (!G.s.ok || !G.s.Pose()) return out;
        const V3 o(origin), n = Norm(V3(axis)), rt = Norm(V3(right)), upv = Norm(V3(up));
        V3 dirs[16];
        for (int q = 0; q < nDirs; ++q) {
            const float t = 6.2831853f * static_cast<float>(q) / static_cast<float>(nDirs);
            dirs[q] = rt * std::cos(t) + upv * std::sin(t);
        }
        for (size_t k = 0; k < depths.size(); ++k) {
            const V3 c = o + n * depths[k];
            const V3 hc = (hints && k < hints->size()) ? V3((*hints)[k]) : c;   // the side filter follows his shaft, not the canal
            std::vector<V3> sl;
            for (float band : {0.35f, 0.7f}) {
                // the slice plane is endless: only vertices near the shaft count (not his balls, sheath or base wherever
                // the plane crosses them). Without a hint: first wide round the canal, then tight round what that found -
                // the shaft as the mesh is really posed (PPA bends the mesh into him, his bones can be elsewhere).
                V3 cen = hc;
                for (int pass = (hints || maxSide <= 0.0f) ? 1 : 0; pass < 2; ++pass) {
                    const float lim = pass == 0 ? maxSide * 1.8f : maxSide;
                    sl.clear();
                    for (size_t i = 0; i < G.s.wp.size(); ++i) {
                        if (i < G.shaft.size() && !G.shaft[i]) continue;
                        const V3& w = G.s.wp[i];
                        const V3 d = w - c;
                        const float t = Dot(d, n);
                        if (std::abs(t) >= band) continue;
                        if (maxSide > 0.0f) { const V3 dh = w - cen; if (Len(dh - n * Dot(dh, n)) > lim) continue; }
                        sl.push_back(w);
                    }
                    if (pass == 0 && !sl.empty()) {
                        V3 m0;
                        for (const auto& w : sl) m0 += w;
                        cen = m0 / static_cast<float>(sl.size());
                    }
                }
                if (sl.size() >= 8) break;
            }
            if (sl.size() < 8) continue;
            // the whole slice: its spread round its own centre
            V3 m;
            for (const auto& w : sl) m += w;
            m = m / static_cast<float>(sl.size());
            std::vector<float> rr;
            for (const auto& w : sl) { V3 d = w - m; d = d - n * Dot(d, n); rr.push_back(Len(d)); }
            std::sort(rr.begin(), rr.end());
            out[k].r = rr[rr.size() * 3 / 4];
            out[k].cx = Dot(m - c, rt);
            out[k].cy = Dot(m - c, upv);
            // each side: how far the shaft reaches that way from HIS canal's centre (off-centre counts)
            for (int q = 0; q < nDirs; ++q) {
                std::vector<float> e;
                // how far the shaft reaches that way (its near-outermost point; negative = it is all on the other side),
                // so the extent from a moved hole centre is simply this minus the move
                for (const auto& w : sl) e.push_back(Dot(w - c, dirs[q]));
                std::sort(e.begin(), e.end());
                out[k].q[q] = e[std::min(e.size() - 1, e.size() * 19 / 20)];
            }
            if (outline) {   // the fit outline (During sex page, testing): the same measure in 24 directions
                std::vector<float> e;
                for (int q = 0; q < 24; ++q) {
                    const float t = 6.2831853f * static_cast<float>(q) / 24.0f;
                    const V3 dq = rt * std::cos(t) + upv * std::sin(t);
                    e.clear();
                    for (const auto& w : sl) e.push_back(Dot(w - c, dq));
                    std::sort(e.begin(), e.end());
                    out[k].qd[q] = e[std::min(e.size() - 1, e.size() * 19 / 20)];
                }
                out[k].hasQd = true;
            }
        }
        return out;
    }
}

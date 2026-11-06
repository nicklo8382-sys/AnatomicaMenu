#include "Engine.h"
#include "Virgin.h"
#include "Bridge.h"
#include "Colour.h"
#include "Drip.h"
#include "SKEE.h"
#include "Wink.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
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
        V operator+(const V& o) const { return {x + o.x, y + o.y, z + o.z}; }
        V operator-(const V& o) const { return {x - o.x, y - o.y, z - o.z}; }
        V operator*(float s) const { return {x * s, y * s, z * s}; }
    };
    float Dot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    V Cross(const V& a, const V& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    float Len(const V& a) { return std::sqrt(Dot(a, a)); }

    RE::NiAVObject* Node(RE::NiAVObject* root, const char* n) { return root ? root->GetObjectByName(RE::BSFixedString(n)) : nullptr; }

    // the canal entrance of this man, from bones rigid with his pelvis
    struct Canal { bool ok = false; V h, a; float sc = 1.0f; V pelvis; };
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
        c.ok = true;
        return c;
    }
    // any of o's schlong bones in the first 8 units of the canal, within rad of its axis
    bool Inside(const Canal& c, RE::Actor* o, float rad) {
        auto* root = o->Get3D(false);
        if (!root) return false;
        static const char* kGen[] = {"NPC Genitals06 [Gen06]", "NPC Genitals05 [Gen05]", "NPC Genitals04 [Gen04]",
                                     "NPC Genitals03 [Gen03]", "NPC Genitals02 [Gen02]", "NPC Genitals01 [Gen01]"};
        const float r = rad * c.sc, tol = -0.5f * r, r2 = r * r, tmax = 8.0f * c.sc;
        for (auto* nm : kGen) {
            auto* n = Node(root, nm);
            if (!n) continue;
            const V v = V(n->world.translate) - c.h;
            const float t = Dot(v, c.a);
            if (t < tol || t > tmax) continue;
            if (Dot(v, v) - t * t < r2) return true;
        }
        return false;
    }

    struct St {
        float dil = 0.0f, ep = 0.0f;
        RE::FormID lastP = 0;
        bool was = false, init = false;
        double worked = 0.0, winkAt = 0.0, partnerNear = 0.0;
        float sent = -1.0f, dilSent = -1.0f, g = 0.0f;
        bool inScene = false;
    };
    std::mutex g_mx;
    std::unordered_map<RE::FormID, St> g_st;
    std::mutex g_gapeMx;                                 // only for GapeOf (the status page asks under other locks)
    std::unordered_map<RE::FormID, float> g_gape;
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
    void Process(RE::Actor* act, St& s, float dt, double now, const std::vector<RE::Actor*>& others) {
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
        float nearest = 1e9f;
        if (c.ok)
            for (auto* o : others) {
                if (o == act) continue;
                const float d = act->GetPosition().GetDistance(o->GetPosition());
                if (d > 90.0f) continue;
                if (s.lastP && o->GetFormID() == s.lastP) {
                    auto* oroot = o->Get3D(false);
                    if (auto* op = Node(oroot, "NPC Pelvis [Pelv]")) nearest = std::min(nearest, Len(V(op->world.translate) - c.pelvis));
                }
                if (Inside(c, o, rad)) { partner = o; break; }
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
            s.dil = std::min(1.0f, s.dil + dt / (build * vm.build));
            s.worked = now;
            s.partnerNear = now;
            s.inScene = true;
            Colour::WorkedNow(act, s.dil, s.ep, partner);
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
                    const float e = during * vm.open * Smooth(s.dil);
                    g = t < hold ? e : e * (1.0f - Smooth((t - hold) / decay));
                } else {
                    if (s.worked <= 0.0 || t >= hold) {
                        const float q = std::max(1.0f, Colour::CloseSlowFor(act));
                        s.dil -= dt * vm.close / (pm * decay * q);
                        if (s.dil <= 0.0f) { s.dil = 0.0f; s.ep = 0.0f; }
                    }
                    g = mx * vm.open * Smooth(s.dil);
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
        if (std::abs(s.dil - s.dilSent) > 0.02f || (s.dil == 0.0f && s.dilSent != 0.0f)) {   // kept with the save
            SetMorph(act, "AGDil", K, s.dil);
            SetMorph(act, "AGEp", K, s.ep);
            s.dilSent = s.dil;
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
                      RE::BSShaderMaterial* mat = nullptr; int col = -9, swap = -9, add = -9; };
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
        if (auto it = g_ringDone.find(a->GetFormID()); it != g_ringDone.end()) {
            const RingDone& d = it->second;
            // still the ring he wears? (a rebuilt body puts a new overlay shape there)
            const bool same = !d.geo || Node(root, d.geo->name.c_str()) == d.geo.get();
            auto* lsp = d.geo ? d.geo->lightingShaderProp_cast() : nullptr;
            if (same && d.root == root && d.col == w.col && d.swap == w.swap && d.add == w.add && (!d.geo || (lsp && lsp->material == d.mat))) return;
        }
        RingDone done{root, nullptr, nullptr, w.col, w.swap, w.add};
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
            RingLook(freeSlot, kBlendBase, w.add, 1.0f);   // a ring for a man without one (NPC ring option)
            done.geo = RE::NiPointer<RE::BSGeometry>(freeSlot);
            RingReport(a, "added a ring in a free slot", freeSlot, w);
            return;
        }
        done.geo = RE::NiPointer<RE::BSGeometry>(ring);
        const bool isBase = DiffuseOf(ring).find("blend_base") != std::string::npos;
        if (w.col < 0) {   // his own paint, as installed
            if (isBase) RingLook(ring, kRingOrigTex, IsTint(MatOf(ring)) ? 0xFFFFFF : -1, -1.0f);
            else if (IsTint(MatOf(ring))) RingLook(ring, nullptr, 0xFFFFFF, -1.0f);
            RingReport(a, "left as his own paint", ring, w);
            return;
        }
        RingLook(ring, w.swap == 1 ? kBlendBase : kRingOrigTex, w.col, -1.0f);
        RingReport(a, "coloured", ring, w);
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
                Process(a, g_st[a->GetFormID()], dt, now, all);
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
            if (full) ColourOne(player); else RingOne(player);
            for (auto* a : Colour::Men()) { if (full) ColourOne(a); else RingOne(a); }
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
        { std::lock_guard gl(g_gapeMx); g_gape.clear(); }
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
        SetMorph(a, "Anatomica Gape", "AnatomicaLoose", std::max(0.0f, v));
        const float gt = RE::Calendar::GetSingleton() ? RE::Calendar::GetSingleton()->GetCurrentGameTime() : 0.0f;
        SetMorph(a, "AGLooseT", K, v > 0.0f ? gt : 0.0f);   // healing counts from now
        g_morph->UpdateModelWeight(a, false);
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

#pragma once
#include <SimpleIni.h>

// Anatomica's runtime cum: strands, streams, drops and rivulets simulated by the DLL every frame (no SMP) and drawn
// with two small skinned templates (a round tube and a flat rivulet, 16 bones each) whose bones are moved to the
// simulated points. They hang from the real lip of the anus (read live from the body mesh, so the gape, the wink and
// PPA all move it), collide with and run down the actual body surface (the morphed body mesh, OBody included), pool
// at the floor (puddles) and dry off. Drip.cpp decides when and how much; this decides how it looks and moves.
namespace Fluid {
    enum class Kind { Squirt, Spray, Ooze, Gush, Land };

    void Install();                                  // start the frame loop (SKSE load)
    void Reset();                                    // a save was loaded: forget everything
    void ForgetMade();                               // its made textures were deleted (tidy): make them again when needed
    void Ini(CSimpleIniA& ini, bool save);           // [Fluid] in AnatomicaMenu.ini
    void RenderSettings();                           // "Look and physics" (Cum & drips page)
    void RenderWetSettings();                        // the wet rim's shine (After sex page, Wetness)
    void RenderGlossSettings();                      // glossy skin's reach (After sex page)

    // ---- what Drip asks for (any thread) ----
    void Hang(RE::FormID id, float amount, float phase, const float rgb[3]);   // strands from the lip; amount 0 = none, phase 0-1 = how far the next drip has grown
    void Release(RE::FormID id, float amount);                                   // the drip falls now
    void Squirt(RE::FormID id, float amount, const float rgb[3], Kind kind, float hold = 0.0f);
    void String(RE::FormID id, RE::FormID partner, const float rgb[3]);          // the string to him as he pulls out
    void Trail(RE::FormID id, float level, int side, const float rgb[3]);        // rivulets down the leg; level 0-3, side 1 = left, 2 = right
    void Coat(RE::FormID giver, float amount, const float rgb[3], float seconds); // his schlong: wet, cum streaks, a drip from the tip
    void Clear(RE::FormID id);                                                    // everything on him gone (wash, bath, test)
    float WetOf(RE::FormID id);                                                   // how wet he is now (0-1)
    std::vector<RE::FormID> WetMen();                                             // everyone wet right now
    bool Busy(RE::FormID id);                                                     // a squirt / ooze still running
    std::vector<std::pair<RE::FormID, float>> TakeLandings();                    // drops that reached the floor: (actor, amount) -> puddles
    std::string Status(RE::FormID id);                                            // one line for the Status page
    bool TestString(RE::Actor* a, const float rgb[3]);
    void SlowMotion(bool on);                                                     // Testing page: everything at 15% speed
    bool SlowMotion();                           // a string to an invisible partner pulling away
    // how thick the giver's schlong is where it passes through a plane (his hole): radius in world units, -1 = not known.
    // Reads his schlong mesh (refreshed every few seconds) and skins it as it is now - so a flared head, a knot or a
    // thicker base all count as they pass.
    float GirthAt(RE::Actor* giver, const RE::NiPoint3& point, const RE::NiPoint3& normal);
    // the same at several depths along his canal at once, and in four directions from the canal's centre (right, up,
    // left, down): an oval or off-centre shaft opens each side to its own extent. -1 = no schlong at that depth.
    // how many of giver's schlong vertices (as the mesh is actually posed - PPA bends the mesh, not his bones) lie inside
    // a tube round his canal: radius, from depth d0 to d1 (world units along axis)
    // where his hole really is: the resting hole centre and a point 4 units down his canal, carried by the bones his anus
    // rim is weighted to (PPA pulls those bones over to the shaft - the pelvis alone missed it by up to 7 units). Our own
    // morphs are not in it. false = no Anatomica rim found on him
    bool HoleFrame(RE::Actor* a, RE::NiPoint3& centre, RE::NiPoint3& deep);
    // where his hole's edge really is right now (his canal mesh as posed, with every morph and PPA's pull in it): for each
    // ring depth (world units along the axis from origin) and each of n directions, how far the edge reaches that way
    // from origin (has = seen). pts: the edge points (for the fit outline)
    struct RimRead { bool ok = false; float q[3][16]{}; bool has[3][16]{}; std::vector<RE::NiPoint3> pts; };
    RimRead RimEdge(RE::Actor* a, const RE::NiPoint3& origin, const RE::NiPoint3& axis, const RE::NiPoint3& right, const RE::NiPoint3& up,
                    const std::vector<float>& ringDepths, int n, float band, bool wantPts);   // band: how thick each ring is (world units)
    std::string SchlongDesc(RE::Actor* giver);
    std::string SchlongBones(RE::Actor* giver);   // testing: the bones his schlong mesh is skinned to
    std::vector<RE::NiPoint3> SchlongSample(RE::Actor* giver, int n);   // testing: n points of his schlong mesh as posed
    // testing: every mesh on giver skinned to his schlong bones, each read four ways (A as the mod does it, B with
    // per-partition bone lists, C through rootParentToSkin, D from the live dynamic vertex data); lines describe each,
    // samples[variant] = points of the mesh the mod picks (A-D), others = variant A of every other schlong mesh
    struct SchlongReport { std::vector<std::string> lines; std::vector<RE::NiPoint3> samples[4]; std::vector<RE::NiPoint3> others; };
    SchlongReport ReportSchlong(RE::Actor* giver, const RE::NiPoint3& origin, const RE::NiPoint3& axis, float sc);   // testing: which mesh is read as his schlong ("none" = no schlong found)
    int InTube(RE::Actor* giver, const RE::NiPoint3& origin, const RE::NiPoint3& axis, float radius, float d0, float d1,
               float* deepest = nullptr);   // deepest: how far along the axis the deepest of them is (the real tip)
    // q[k]: how far the shaft reaches in direction k (angle k * 360 / nDirs from 'right' towards 'up'), up to 16 directions
    struct Slice { float r = -1.0f; float q[16]{}; float cx = 0.0f, cy = 0.0f; float qd[24]{}; bool hasQd = false; };   // qd: the outline in 24 directions (only asked for the fit outline)   // cx / cy: the shaft's centre off his canal centre (right / up)
    std::vector<Slice> SlicesAt(RE::Actor* giver, const RE::NiPoint3& origin, const RE::NiPoint3& axis, const RE::NiPoint3& right,
                                const RE::NiPoint3& up, const std::vector<float>& depths, int nDirs = 4,
                                float maxSide = 0.0f,    // > 0: only schlong vertices this close to the centre line (world units)
                                const std::vector<RE::NiPoint3>* hints = nullptr, bool outline = false);   // per depth: where the shaft's bone line crosses (the side filter's centre)
}

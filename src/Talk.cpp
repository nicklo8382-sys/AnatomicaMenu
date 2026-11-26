#include "Talk.h"
#include "Colossal.h"
#include "Colour.h"
#include "Engine.h"
#include "Face.h"
#include "Lang.h"
#include "OStrandsLink.h"
#include "Size.h"
#include "Training.h"
#include "Virgin.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Talk {
    namespace {
        constexpr const char* kKey = "AnatomicaDDF";
        constexpr const char* kPack = "Data/TMSDynamicDialogue/Packs/Anatomica.json";

        // kinds of lines: each can be switched off, and allowed only in some scenes (chance rolled per man per scene)
        enum Cat { kCFirst, kCStretch, kCFilled, kCRemarks, kCAfter, kCSore, kCRegular, kCShape, kCPairs, kCPlug, kCOral, kCBeast, kCBreed, kCN };
        struct CatDef { const char* ini; const char* name; const char* help; };
        constexpr CatDef kCats[kCN] = {
            {"FirstTime", "First time and experience", "A virgin's first time, a man still getting used to it, one who has taken plenty."},
            {"Stretch", "Stretch and bulge", "How far he is opened up, the belly bulge, going very deep, a very big partner."},
            {"Filled", "Being filled (OStrands)", "Being filled, a Colossal Load, too full to take more, running out of him afterwards. Needs the OStrands link."},
            {"Remarks", "His partner remarks on him", "The man taking him says things about him too (how tight, how much he takes, the bulge)."},
            {"After", "Afterwards", "After the scene: him gaping or clenching, and his partner noticing how open he is."},
            {"Sore", "Still sore", "His next scene while he is still red, swollen or loose from the last one."},
            {"Regular", "Regular partners", "A man who has had him many times, or the man who took his first time."},
            {"Shape", "Schlong shape", "Knotted (and tied), flared, ridged, barbed or uncut - each has its own lines."},
            {"Pairs", "Back and forth", "A question from his partner and his answer (\"Can you take more?\" - \"Give me everything.\")."},
            {"Plug", "Plugs", "A plug going in at the end to keep it inside him, and him wearing it. Needs the OStrands link."},
            {"Oral", "Mouth and face", "While he sucks: how used to it he is, going deep and gagging, swallowing, cum on his face - and the man he is sucking says things too."},
            {"Beast", "Beast forms", "OStim Beast Form: him taken while he is a werewolf, werebear or Vampire Lord, being taken by one, and what the beast says."},
            {"Breeding", "Breeding talk", "Dirty talk about being bred while he is filled, stuffed full, knotted or taken by one man after another (talk only)."},
        };
        struct Settings {
            bool on = true;
            bool muteNet = false;   // quiet while SkyrimNet is running
            std::array<bool, kCN> cat{};
            std::array<float, kCN> chance{};   // 0-1 per scene
            Settings() { cat.fill(true); chance.fill(1.0f); }
        } g;

        // the morphs the pack reads (on the man being taken unless noted)
        enum M { kTaken, kFirst, kExp, kOpen, kBulge, kDeep, kBig, kFilled, kFilling, kColossal, kFull, kFlood, kPlug,
                 kRemarks /*top*/, kGaping, kSeesGape /*top*/, kSore, kRegular, kHisFirst, kShape, kKnotTied, kPairs /*top*/, kPlugIn,
                 kSucking, kOralExp, kThroat, kGag, kSwallow, kFacial, kSucked /*top*/, kBeast, kByBeast, kBeastTop /*top*/, kTrained, kTooBig, kBred, kBreedTop /*top*/,
                 kThrall, kColdSeed, kLordUsed, kThrallTop /*top*/, kBond, kScentTop /*top*/, kMoonTop /*top*/, kN };   // 2.6j: Vampire Lords
        constexpr std::array<const char*, kN> kNames{
            "AnatomicaDDF Taken", "AnatomicaDDF First", "AnatomicaDDF Exp", "AnatomicaDDF Open", "AnatomicaDDF Bulge",
            "AnatomicaDDF Deep", "AnatomicaDDF Big", "AnatomicaDDF Filled", "AnatomicaDDF Filling", "AnatomicaDDF Colossal",
            "AnatomicaDDF Full", "AnatomicaDDF Flood", "AnatomicaDDF Plug", "AnatomicaDDF Remarks", "AnatomicaDDF Gaping",
            "AnatomicaDDF SeesGape", "AnatomicaDDF Sore", "AnatomicaDDF Regular", "AnatomicaDDF HisFirst", "AnatomicaDDF Shape",
            "AnatomicaDDF KnotTied", "AnatomicaDDF Pairs", "AnatomicaDDF PlugIn",
            "AnatomicaDDF Sucking", "AnatomicaDDF OralExp", "AnatomicaDDF Throat", "AnatomicaDDF Gag", "AnatomicaDDF Swallow", "AnatomicaDDF Facial", "AnatomicaDDF Sucked",
            "AnatomicaDDF Beast", "AnatomicaDDF ByBeast", "AnatomicaDDF BeastTop",
            "AnatomicaDDF Trained", "AnatomicaDDF TooBig", "AnatomicaDDF Bred", "AnatomicaDDF BreedTop",
            "AnatomicaDDF Thrall", "AnatomicaDDF ColdSeed", "AnatomicaDDF LordUsed", "AnatomicaDDF ThrallTop",
            "AnatomicaDDF Bond", "AnatomicaDDF ScentTop", "AnatomicaDDF MoonTop"};
        using Vals = std::array<float, kN>;

        // 2.7c: the values go to DDF as faction ranks (Anatomica.esp), not RaceMenu body morphs - DDF could not reach
        // RaceMenu on some setups ("RaceMenuBodyMorph interface unavailable": every Anatomica line was rejected), and a
        // faction rank is something every mod can read (SkyrimNet's get_faction_rank, dialogue / perk conditions, other
        // DDF packs). Rank = value x 10 (0-127). The scene factions (Anatomica_DDF_*, 0x801.., in kNames' order) are set
        // while he is in a scene and taken off after; the state factions (Anatomica_State_*, after them) stay on him.
        constexpr std::uint32_t kFacBase = 0x801;
        enum St { kSVirgin, kSExp, kSOralExp, kSTrained, kSSore, kSGape, kSFilled, kSPlugged, kSBond, kSColdSeed, kSN };
        constexpr std::uint32_t kStateBase = kFacBase + kN;
        std::array<RE::TESFaction*, kN> g_fac{};
        std::array<RE::TESFaction*, kSN> g_sfac{};
        int g_facOk = -1;   // -1 not looked yet, 0 Anatomica.esp's factions not there, 1 found
        bool Factions() {
            if (g_facOk >= 0) return g_facOk == 1;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return false;
            bool ok = true;
            for (int i = 0; i < kN; ++i) ok &= (g_fac[i] = dh->LookupForm<RE::TESFaction>(kFacBase + i, "Anatomica.esp")) != nullptr;
            for (int i = 0; i < kSN; ++i) ok &= (g_sfac[i] = dh->LookupForm<RE::TESFaction>(kStateBase + i, "Anatomica.esp")) != nullptr;
            g_facOk = ok ? 1 : 0;
            logger::info("Anatomica: {} (Anatomica.esp)", ok ? "DDF / state factions found" : "DDF / state factions missing - update Anatomica.esp (no DDF lines, no faction states)");
            return ok;
        }
        std::int8_t Rank(float v) { return static_cast<std::int8_t>(std::clamp<long>(std::lround(v * 10.0f), 0L, 127L)); }
        std::unordered_map<RE::FormID, std::array<int, kSN>> g_stateSent;   // what each man's state factions say now (-1 = not set)

        struct Man {
            double lastNow = -1e9;            // when he was last being taken
            std::array<bool, kCN> allow{};    // rolled at the start of each scene
            float soreAt = 0.0f;              // how sore / loose he was when this scene started
            bool plugWas = false;
            double plugAt = -1e9;             // a plug just went in
            RE::FormID top = 0;               // the man taking him (kept for the afterwards remarks)
            double oralAt = -1e9;             // 2.6j: when he last sucked
            std::vector<RE::FormID> bredBy;   // 2.6j: the men who have filled him this scene
        };

        std::mutex g_mx;
        std::unordered_map<RE::FormID, Vals> g_sent;   // what each man has now
        std::unordered_map<RE::FormID, Man> g_men;
        std::unordered_map<RE::FormID, double> g_fillAt, g_colAt;
        std::atomic<bool> g_sweep{true};
        std::atomic<bool> g_queued{false};
        std::atomic<int> g_have{-1};   // 1 = DDF is loaded, 0 = not (yet), -2 = not there and we stopped asking
        int g_net = -1;
        std::atomic<bool> g_loaded{false};   // SKSE finished loading (kDataLoaded): every plugin is in by now
        std::atomic<int> g_checksLeft{3};    // after that, this many more looks before a "no" is final
        // ask Windows whether DDF's DLL is loaded (the worker thread only, so the 3 checks are not used up by the menu)
        bool Probe() {
            if (g_have == 1) return true;
            if (g_have == -2) return false;
            if (g_loaded && g_checksLeft-- <= 0) {
                g_have = -2;
                logger::info("Anatomica: Dynamic Dialogue Framework not installed - stopped looking for it");
                return false;
            }
            g_have = REX::W32::GetModuleHandleW(L"TMS_DynamicDialogue.dll") ? 1 : 0;
            if (g_have == 1) logger::info("Anatomica: Dynamic Dialogue Framework found - Anatomica's DDF lines can play");
            return g_have == 1;
        }
        int g_live = 0;
        std::mt19937 g_rng{std::random_device{}()};

        bool Net() {   // (only a "yes" is kept: SKSE may not have loaded it yet the first time we look)
            if (g_net != 1) g_net = REX::W32::GetModuleHandleW(L"SkyrimNet.dll") ? 1 : 0;
            return g_net == 1;
        }
        double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
        bool Roll(float p) { return p >= 1.0f || std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng) < p; }

        void Help(const char* t) {
            ImGuiMCP::SameLine();
            Tr::TextDisabled("(?)");
            if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::PushTextWrapPos(420.0f); Tr::TextUnformatted(t); ImGuiMCP::PopTextWrapPos(); ImGuiMCP::EndTooltip(); }
        }

        void Write(RE::Actor* a, const Vals& v) {
            const bool fresh = !g_sent.contains(a->GetFormID());
            if (fresh && v[kTaken] > 0.0f)
                logger::info("Anatomica DDF: lines possible for {} (exp {:.0f}, first {:.0f}, open {:.2f}, filled {:.2f})", a->GetName(), v[kExp], v[kFirst], v[kOpen], v[kFilled]);
            if (!Factions()) return;
            auto& sent = g_sent[a->GetFormID()];
            for (int i = 0; i < kN; ++i) {
                // 2.7c: every scene faction on him from the start, at rank 0 too (a "<= 0" condition needs him in it)
                if (fresh || Rank(sent[i]) != Rank(v[i])) {
                    a->AddToFaction(g_fac[i], Rank(v[i]));
                    sent[i] = v[i];
                }
            }
        }
        void Unwrite(RE::Actor* a) {   // 2.7c: the scene over - off the scene factions
            if (!Factions()) return;
            for (int i = 0; i < kN; ++i) if (a->IsInFaction(g_fac[i])) a->RemoveFromFaction(g_fac[i]);
        }
        // 2.7c: his lasting state as factions (also without DDF): virgin, experience, oral experience, training, soreness,
        // gape, how full he is, a plug, a Vampire Lord's bond / cold seed. Only what changed is written.
        void WriteState(RE::Actor* a) {
            if (!a || !Factions()) return;
            const RE::FormID id = a->GetFormID();
            std::array<int, kSN> r;
            r.fill(-1);
            const Virgin::Info vi = Virgin::Get(id);
            if (vi.known) {
                r[kSVirgin] = vi.virgin ? 1 : 0;
                const std::string lvl = vi.level ? vi.level : "";
                r[kSExp] = lvl == "virgin" ? 0 : lvl == "inexperienced" ? 1 : lvl == "experienced" ? 2 : 3;
            }
            const Face::Oral o = Face::OralOf(id);
            r[kSOralExp] = o.exp < 0.05f ? 0 : o.exp < 1.0f ? 1 : o.exp < 4.0f ? 2 : 3;
            if (const float tl = Training::Level(id); tl >= 0.0f) r[kSTrained] = Rank(tl);
            const Colour::Snapshot sn = Colour::SnapshotOf(a);
            r[kSSore] = static_cast<int>(std::lround(std::clamp(std::max({sn.tender, sn.swell, sn.loose}), 0.0f, 1.0f) * 100.0f));
            r[kSGape] = static_cast<int>(std::lround(std::clamp(Engine::GapeOf(id), 0.0f, 1.0f) * 100.0f));
            if (OStrandsLink::Installed()) {
                const OStrandsLink::Held h = OStrandsLink::HeldOf(id);
                r[kSFilled] = Rank(h.vol);
                r[kSPlugged] = h.plugged ? 1 : 0;
            }
            r[kSBond] = Training::Bond(id) > 0.0f ? 1 : 0;
            r[kSColdSeed] = Training::Cold(id) ? 1 : 0;
            auto it = g_stateSent.find(id);
            if (it == g_stateSent.end()) { it = g_stateSent.emplace(id, std::array<int, kSN>{}).first; it->second.fill(-2); }
            for (int i = 0; i < kSN; ++i) {
                if (r[i] < 0 || r[i] == it->second[i]) continue;
                a->AddToFaction(g_sfac[i], static_cast<std::int8_t>(std::min(r[i], 127)));
                it->second[i] = r[i];
            }
        }

        int ShapeCode(RE::Actor* partner) {
            if (!partner) return 0;
            const std::string k = Size::ShapeKey(Size::Shape(partner));
            if (k == "knotted") return 1;
            if (k == "flared") return 2;
            if (k == "ridged") return 3;
            if (k == "barbed") return 4;
            if (k == "uncut") return 5;
            return 0;
        }

        void Tick(bool ddf) {
            std::lock_guard lk(g_mx);
            if (g_sweep.exchange(false)) {   // a save keeps RaceMenu's morphs (before 2.7c) and the factions: clear ours off everyone loaded
                auto clear = [](RE::Actor* a) { Engine::ClearMorphKey(a, kKey); Unwrite(a); };
                if (auto* pl = RE::ProcessLists::GetSingleton()) {
                    for (auto& h : pl->highActorHandles) if (auto a = h.get()) clear(a.get());
                }
                if (auto* p = RE::PlayerCharacter::GetSingleton()) clear(p);
                g_sent.clear();
                g_stateSent.clear();
            }
            // 2.7c: the lasting state factions - the player always, and the men in a scene
            WriteState(RE::PlayerCharacter::GetSingleton());
            for (RE::FormID id : Engine::DuringIds())
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && a->Is3DLoaded() && !a->IsPlayerRef()) WriteState(a);
            if (!ddf) return;   // (no DDF: only the state factions)
            const bool quiet = !g.on || (g.muteNet && Net());
            const double t = Now();
            std::unordered_map<RE::FormID, Vals> want;
            auto top = [&](RE::FormID id) -> Vals& { return want[id]; };
            int live = 0;
            for (RE::FormID id : Engine::DuringIds()) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!a || !a->Is3DLoaded()) continue;
                const Virgin::Info vi = Virgin::Get(id);
                const bool now = vi.workedAgo >= 0.0 && vi.workedAgo < 3.0;     // being taken right now
                const bool lately = vi.workedAgo >= 0.0 && vi.workedAgo < 60.0;  // and just after (DDF's "linger")
                const OStrandsLink::Held h = OStrandsLink::HeldOf(id);
                Man& m = g_men[id];
                if (now && t - m.lastNow > 60.0) {   // a new scene: roll which kinds may talk, how sore he came in
                    m.bredBy.clear();
                    for (int c = 0; c < kCN; ++c) m.allow[c] = g.cat[c] && Roll(g.chance[c]);
                    const Colour::Snapshot sn = Colour::SnapshotOf(a);
                    m.soreAt = std::max({sn.tender, sn.swell, sn.loose});
                }
                if (now) m.lastNow = t;
                const bool plug = h.plugged;
                if (plug && !m.plugWas) m.plugAt = t;
                m.plugWas = plug;
                if (!lately && h.vol <= 0.02f && t - m.plugAt > 60.0) continue;
                Vals v{};
                if (!quiet) {
                    ++live;
                    auto on = [&](Cat c) { return m.allow[c]; };
                    v[kTaken] = now ? 1.0f : 0.5f;   // 1 = now, 0.5 = just after / still full
                    const Engine::Bulge b = Engine::BulgeOf(id);
                    if (b.partner) m.top = b.partner;
                    else if (h.giver) m.top = h.giver;
                    auto* tp = m.top ? RE::TESForm::LookupByID<RE::Actor>(m.top) : nullptr;
                    const Engine::During d = Engine::DuringOf(id);
                    const float open = std::clamp(std::max(d.hole, Engine::GapeOf(id)), 0.0f, 1.0f);
                    v[kExp] = 2.0f;   // nothing known: "experienced" (no first-time / well-used lines)
                    if (on(kCFirst) && vi.known) {
                        const std::string lvl = vi.level ? vi.level : "";
                        v[kExp] = lvl == "virgin" ? 0.0f : lvl == "inexperienced" ? 1.0f : lvl == "experienced" ? 2.0f : 3.0f;
                        v[kFirst] = (vi.virgin || (vi.daysSinceFirst >= 0.0f && vi.daysSinceFirst < 0.1f)) ? 1.0f : 0.0f;
                    }
                    if (on(kCStretch) && now) {
                        v[kOpen] = open;
                        v[kBulge] = b.on ? 1.0f : 0.0f;
                        v[kDeep] = b.deep ? 1.0f : 0.0f;
                        const int cat = m.top ? Size::CatOf(m.top) : Size::kUnknown;
                        v[kBig] = cat == Size::kCreature ? 4.0f : (cat >= 0 && cat <= 4) ? static_cast<float>(cat) : 2.0f;   // 0 XS .. 4 XL (2 = average / not known; 2.6j: a beast form counts as XL)
                    }
                    if (OStrandsLink::Installed()) {
                        if (on(kCFilled)) {
                            v[kFilled] = h.vol;
                            if (h.filling) g_fillAt[id] = t;
                            v[kFilling] = g_fillAt.contains(id) && t - g_fillAt[id] < 20.0 ? 1.0f : 0.0f;
                            v[kFull] = h.stuffed ? 1.0f : 0.0f;
                            v[kFlood] = (h.flooding || h.leaking) && !now ? 1.0f : 0.0f;
                            if (h.giver) {
                                if (auto* gv = RE::TESForm::LookupByID<RE::Actor>(h.giver)) {
                                    float w = 0.0f; Colossal::Load l;
                                    if (Colossal::For(gv, a, w, l) && w > 0.0f) g_colAt[id] = t;
                                }
                            }
                            v[kColossal] = g_colAt.contains(id) && t - g_colAt[id] < 90.0 ? 1.0f : 0.0f;
                        }
                        if (on(kCPlug)) {
                            v[kPlug] = plug ? 1.0f : 0.0f;
                            v[kPlugIn] = plug && t - m.plugAt < 60.0 ? 1.0f : 0.0f;
                        }
                    }
                    if (on(kCAfter) && !now && lately && open >= 0.3f) v[kGaping] = 1.0f;
                    if (on(kCSore) && now && m.soreAt >= 0.25f && v[kFirst] < 0.5f) v[kSore] = 1.0f;
                    if (on(kCRegular) && m.top) {
                        for (auto& [pid, n] : vi.partners) if (pid == m.top) v[kRegular] = static_cast<float>(n);
                        if (vi.firstEverBy == m.top && v[kFirst] < 0.5f) v[kHisFirst] = 1.0f;
                    }
                    if (on(kCShape) && now && tp) {
                        v[kShape] = static_cast<float>(ShapeCode(tp));
                        if (v[kShape] == 1.0f && (b.deep || d.held)) v[kKnotTied] = 1.0f;
                    }
                    if (on(kCStretch)) {   // 2.6j: training
                        const float tl = Training::Level(id);
                        v[kTrained] = std::max(0.0f, tl);
                        if (now && m.top) v[kTooBig] = Training::TooBig(id, Size::CatOf(m.top)) ? 1.0f : 0.0f;
                    }
                    if (h.filling && h.giver && std::find(m.bredBy.begin(), m.bredBy.end(), h.giver) == m.bredBy.end()) m.bredBy.push_back(h.giver);
                    if (on(kCBreed)) {   // 2.6j: breeding talk
                        v[kBred] = static_cast<float>(m.bredBy.size());
                        if (m.top && (h.filling || now)) top(m.top)[kBreedTop] = 1.0f;
                    }
                    if (on(kCBeast) && now) {   // 2.6j: OStim Beast Form
                        v[kBeast] = static_cast<float>(Colossal::BeastForm(a));
                        if (tp) v[kByBeast] = static_cast<float>(Colossal::BeastForm(tp));
                    }
                    if (on(kCBeast)) {   // 2.6j: Vampire Lords - their cold seed in him; while one takes him, his thrall mark and how used to them he is
                        v[kColdSeed] = Training::Cold(id) ? 1.0f : 0.0f;
                        if (now && tp && Colossal::BeastForm(tp) == 3) {
                            v[kThrall] = Training::Thrall(id);
                            v[kLordUsed] = Training::LordLevel(id);
                            if (v[kThrall] > 0.0f) top(m.top)[kThrallTop] = 1.0f;
                        }
                        v[kBond] = Training::Bond(id) > 0.0f ? 1.0f : 0.0f;
                        if (now && tp && (Colossal::BeastForm(tp) == 1 || Colossal::BeastForm(tp) == 2)) {   // a werewolf taking him
                            if (const RE::FormID sc = Training::Scent(id); sc && sc != m.top) top(m.top)[kScentTop] = 1.0f;   // smells another wolf on him
                            top(m.top)[kMoonTop] = Colossal::Moon();
                        }
                    }
                    if (m.top) {   // the man taking him
                        Vals& tv = top(m.top);
                        if (on(kCBeast) && now && tp && Colossal::BeastForm(tp)) tv[kBeastTop] = static_cast<float>(Colossal::BeastForm(tp));   // 1 werewolf, 2 werebear, 3 Vampire Lord
                        if (on(kCRemarks) && now) tv[kRemarks] = 1.0f;
                        if (on(kCRemarks) && on(kCAfter) && v[kGaping] > 0.0f) tv[kSeesGape] = 1.0f;
                        if (on(kCPairs) && now) tv[kPairs] = 1.0f;
                    }
                }
                Vals& w = want[id];
                for (int i : {kRemarks, kSeesGape, kPairs, kSucked, kBeastTop, kBreedTop, kThrallTop, kScentTop, kMoonTop}) v[i] = std::max(v[i], w[i]);   // (a man can be taken and take someone at once)
                w = v;
            }
            // 2.6j: his mouth (Mouth & Face page) - merged into what he has from being taken
            if (!quiet && g.cat[kCOral]) {
                for (const Face::OralState& o : Face::OralNow()) {
                    auto* a = RE::TESForm::LookupByID<RE::Actor>(o.id);
                    if (!a || !a->Is3DLoaded()) continue;
                    Man& m = g_men[o.id];
                    if (o.sucking && t - m.lastNow > 60.0 && t - m.oralAt > 60.0) m.allow[kCOral] = Roll(g.chance[kCOral]);
                    if (o.sucking) m.oralAt = t;
                    if (!m.allow[kCOral]) continue;
                    ++live;
                    Vals& v = want[o.id];
                    v[kSucking] = o.sucking ? 1.0f : 0.5f;
                    v[kOralExp] = o.exp < 0.05f ? 0.0f : o.exp < 1.0f ? 1.0f : o.exp < 4.0f ? 2.0f : 3.0f;
                    v[kThroat] = o.sucking && o.deep ? 1.0f : 0.0f;
                    v[kGag] = o.gagging ? 1.0f : 0.0f;
                    v[kSwallow] = o.swallowed ? 1.0f : 0.0f;
                    v[kFacial] = o.faceCum > 0.05f ? 1.0f : 0.0f;
                    if (o.partner && o.sucking) want[o.partner][kSucked] = 1.0f;
                }
            }
            g_live = live;
            for (auto& [id, v] : want) {
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && a->Is3DLoaded()) Write(a, v);
            }
            std::vector<RE::FormID> gone;
            for (auto& [id, s] : g_sent) if (!want.contains(id)) gone.push_back(id);
            for (RE::FormID id : gone) {
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id)) Unwrite(a);
                g_sent.erase(id);
            }
        }

        void Loop() {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                const bool ddf = Probe();
                if (!ddf) {
                    // 2.7c: without DDF the state factions still go on (every 2 s); while it may still load, wait
                    std::this_thread::sleep_for(std::chrono::seconds(g_have == -2 ? 2 : 5));
                    if (g_have != -2) continue;
                }
                auto* ui = RE::UI::GetSingleton();
                if (!ui || ui->GameIsPaused()) continue;
                if (g_queued.exchange(true)) continue;
                if (auto* tq = SKSE::GetTaskInterface()) tq->AddTask([ddf]() { g_queued = false; Tick(ddf); });
                else g_queued = false;
            }
        }
    }

    // 2.6j: Anatomica's thread starts while SKSE is still loading the other plugins (TMS_DynamicDialogue.dll loads after
    // AnatomicaMenu.dll), so the worker keeps looking until SKSE has finished, then 3 more times, then stops for good.
    bool Installed() { return g_have == 1; }
    void OnDataLoaded() { g_loaded = true; }
    void Install() { std::thread(Loop).detach(); }
    void Reset() {
        std::lock_guard lk(g_mx);
        g_sent.clear();
        g_men.clear();
        g_fillAt.clear();
        g_colAt.clear();
        g_sweep = true;
    }

    bool RenderSection() {
        if (!Tr::CollapsingHeader("Dialogue (DDF)###ddf")) return false;
        bool ch = false;
        if (!Installed()) {
            Tr::TextWrapped("Needs Dynamic Dialogue Framework (DDF) for OStim Standalone - these settings do nothing until it is installed (they are kept).");
            Tr::TextDisabled("(not installed)");
        }
        std::error_code ec;
        const bool pack = std::filesystem::exists(kPack, ec);
        Tr::TextWrapped("Lines during sex about what is happening to him: his first time or how used to it he is, how far he is "
                        "stretched, the bulge, being filled. They come from Anatomica's DDF dialogue pack and show as DDF subtitles (no voice).");
        if (!pack) Tr::TextDisabled("Anatomica's DDF pack (Data\\TMSDynamicDialogue\\Packs\\Anatomica.json) is not installed - pick the installer's DDF option.");
        std::lock_guard lk(g_mx);
        ch |= Tr::Checkbox("Anatomica's lines", &g.on);
        Help("Off: the pack stays loaded in DDF but none of its lines can play.");
        if (g.on) {
            ImGuiMCP::Indent();
            Tr::TextDisabled("Each kind: on or off, and how often a scene gets it (rolled for each man at the start of each scene).");
            for (int c = 0; c < kCN; ++c) {
                ImGuiMCP::PushID(c);
                ch |= Tr::Checkbox(kCats[c].name, &g.cat[c]);
                Help(kCats[c].help);
                if (g.cat[c]) {
                    ImGuiMCP::SameLine();
                    ImGuiMCP::SetNextItemWidth(140.0f);
                    float pc = g.chance[c] * 100.0f;
                    if (Tr::SliderFloat("##ch", &pc, 0.0f, 100.0f, "%.0f%% of scenes")) { g.chance[c] = pc / 100.0f; ch = true; }
                }
                ImGuiMCP::PopID();
            }
            ch |= Tr::Checkbox("Quiet while SkyrimNet is running", &g.muteNet);
            Help("SkyrimNet writes its own lines in scenes. On: Anatomica's DDF lines stay quiet whenever SkyrimNet is installed, so they don't talk over each other.");
            if (g.muteNet && Net()) Tr::TextDisabled("SkyrimNet is installed: Anatomica's lines are quiet.");
            Tr::TextDisabled("Your own lines: write them in DDF (its Author Mode, or a .json in Data\\TMSDynamicDialogue\\Packs) using the "
                             "Anatomica factions (Anatomica.esp) listed in Anatomica's README.");
            ImGuiMCP::Unindent();
        }
        if (Installed()) Tr::TextDisabled(Lang::Fmt("Men with Anatomica lines possible right now: {}", g_live).c_str());
        return ch;
    }

    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "DDF";
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto F = [&](const char* k, float& v) {
            if (save) ini.SetDoubleValue(S, k, v);
            else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), 0.0f, 1.0f);
        };
        std::lock_guard lk(g_mx);
        B("Lines", g.on); B("QuietWithSkyrimNet", g.muteNet);
        for (int c = 0; c < kCN; ++c) {
            B(kCats[c].ini, g.cat[c]);
            F((std::string(kCats[c].ini) + "Chance").c_str(), g.chance[c]);
        }
    }
}

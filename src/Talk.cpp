#include "Talk.h"
#include "Colossal.h"
#include "Colour.h"
#include "Engine.h"
#include "Lang.h"
#include "OStrandsLink.h"
#include "Size.h"
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
        enum Cat { kCFirst, kCStretch, kCFilled, kCRemarks, kCAfter, kCSore, kCRegular, kCShape, kCPairs, kCPlug, kCN };
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
                 kRemarks /*top*/, kGaping, kSeesGape /*top*/, kSore, kRegular, kHisFirst, kShape, kKnotTied, kPairs /*top*/, kPlugIn, kN };
        constexpr std::array<const char*, kN> kNames{
            "AnatomicaDDF Taken", "AnatomicaDDF First", "AnatomicaDDF Exp", "AnatomicaDDF Open", "AnatomicaDDF Bulge",
            "AnatomicaDDF Deep", "AnatomicaDDF Big", "AnatomicaDDF Filled", "AnatomicaDDF Filling", "AnatomicaDDF Colossal",
            "AnatomicaDDF Full", "AnatomicaDDF Flood", "AnatomicaDDF Plug", "AnatomicaDDF Remarks", "AnatomicaDDF Gaping",
            "AnatomicaDDF SeesGape", "AnatomicaDDF Sore", "AnatomicaDDF Regular", "AnatomicaDDF HisFirst", "AnatomicaDDF Shape",
            "AnatomicaDDF KnotTied", "AnatomicaDDF Pairs", "AnatomicaDDF PlugIn"};
        using Vals = std::array<float, kN>;

        struct Man {
            double lastNow = -1e9;            // when he was last being taken
            std::array<bool, kCN> allow{};    // rolled at the start of each scene
            float soreAt = 0.0f;              // how sore / loose he was when this scene started
            bool plugWas = false;
            double plugAt = -1e9;             // a plug just went in
            RE::FormID top = 0;               // the man taking him (kept for the afterwards remarks)
        };

        std::mutex g_mx;
        std::unordered_map<RE::FormID, Vals> g_sent;   // what each man has now
        std::unordered_map<RE::FormID, Man> g_men;
        std::unordered_map<RE::FormID, double> g_fillAt, g_colAt;
        std::atomic<bool> g_sweep{true};
        std::atomic<bool> g_queued{false};
        int g_have = -1, g_net = -1;
        int g_live = 0;
        std::mt19937 g_rng{std::random_device{}()};

        bool Net() {
            if (g_net < 0) g_net = REX::W32::GetModuleHandleW(L"SkyrimNet.dll") ? 1 : 0;
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
            auto& sent = g_sent[a->GetFormID()];
            for (int i = 0; i < kN; ++i) {
                if (std::abs(sent[i] - v[i]) > 0.02f) {
                    Engine::BodyMorph(a, kNames[i], kKey, v[i]);   // no model update: these names move nothing on the body
                    sent[i] = v[i];
                }
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

        void Tick() {
            if (!Installed()) return;
            std::lock_guard lk(g_mx);
            if (g_sweep.exchange(false)) {   // a save keeps RaceMenu's morphs: clear ours off everyone loaded
                if (auto* pl = RE::ProcessLists::GetSingleton()) {
                    for (auto& h : pl->highActorHandles) if (auto a = h.get()) Engine::ClearMorphKey(a.get(), kKey);
                }
                if (auto* p = RE::PlayerCharacter::GetSingleton()) Engine::ClearMorphKey(p, kKey);
                g_sent.clear();
            }
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
                        v[kBig] = (cat >= 0 && cat <= 4) ? static_cast<float>(cat) : 2.0f;   // 0 XS .. 4 XL (2 = average / not known)
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
                    if (m.top) {   // the man taking him
                        Vals& tv = top(m.top);
                        if (on(kCRemarks) && now) tv[kRemarks] = 1.0f;
                        if (on(kCRemarks) && on(kCAfter) && v[kGaping] > 0.0f) tv[kSeesGape] = 1.0f;
                        if (on(kCPairs) && now) tv[kPairs] = 1.0f;
                    }
                }
                Vals& w = want[id];
                for (int i : {kRemarks, kSeesGape, kPairs}) v[i] = std::max(v[i], w[i]);   // (a man can be taken and take someone at once)
                w = v;
            }
            g_live = live;
            for (auto& [id, v] : want) {
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && a->Is3DLoaded()) Write(a, v);
            }
            std::vector<RE::FormID> gone;
            for (auto& [id, s] : g_sent) if (!want.contains(id)) gone.push_back(id);
            for (RE::FormID id : gone) {
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id)) Engine::ClearMorphKey(a, kKey);
                g_sent.erase(id);
            }
        }

        void Loop() {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                if (!Installed()) { std::this_thread::sleep_for(std::chrono::seconds(5)); continue; }
                auto* ui = RE::UI::GetSingleton();
                if (!ui || ui->GameIsPaused()) continue;
                if (g_queued.exchange(true)) continue;
                if (auto* tq = SKSE::GetTaskInterface()) tq->AddTask([]() { g_queued = false; Tick(); });
                else g_queued = false;
            }
        }
    }

    bool Installed() {
        if (g_have < 0) g_have = REX::W32::GetModuleHandleW(L"TMS_DynamicDialogue.dll") ? 1 : 0;
        return g_have == 1;
    }
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
                             "\"AnatomicaDDF\" body morphs listed in Anatomica's README.");
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

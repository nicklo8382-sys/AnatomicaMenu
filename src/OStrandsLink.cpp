#include "OStrandsLink.h"
#include "Lang.h"
#include "Fluid.h"
#include "Engine.h"
#include "Colour.h"

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
    } g;

    struct State {
        float vol = 0.0f;               // loads in him
        float rgb[3] = {0.96f, 0.95f, 0.90f};
        double lastWorked = 0.0, lastIn = 0.0;
        bool plugged = false;
        float gushLeft = 0.0f, squirtLeft = 0.0f, squirtSpeed = 0.0f;   // loads still to come out in a gush / squirt
        float acc = 0.0f;               // particles owed (fractions carried over)
        std::uint32_t stream = 0, squirtStream = 0;
    };

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
    struct Api { bool tried = false; int version = 0; EmitFn emit = nullptr; OnFn semenOn = nullptr; } g_api;
    void Resolve() {
        if (g_api.tried) return;
        g_api.tried = true;
        auto* m = REX::W32::GetModuleHandleW(L"0_OStrands.dll");
        if (!m) return;
        using VerFn = int (*)();
        auto ver = reinterpret_cast<VerFn>(REX::W32::GetProcAddress(m, "OStrands_ApiVersion"));
        g_api.emit = reinterpret_cast<EmitFn>(REX::W32::GetProcAddress(m, "OStrands_Emit"));
        g_api.semenOn = reinterpret_cast<OnFn>(REX::W32::GetProcAddress(m, "OStrands_SemenOn"));
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

    // emit n particles from his lip (centre c, pointing out along o) with this speed (units/s) and spread
    void Out(RE::Actor* a, std::uint32_t stream, const RE::NiPoint3& at, const RE::NiPoint3& vel, int n, const float rgb[3]) {
        if (n <= 0 || !g_api.emit) return;
        const float p[3] = {at.x, at.y, at.z}, v[3] = {vel.x, vel.y, vel.z};
        g_api.emit(a, stream, p, v, n, rgb);
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
        auto take = [&](float loads) { loads = std::min(loads, s.vol); s.vol -= loads; return loads; };
        // squirt (a clench): fast, along the canal's axis
        if (s.squirtLeft > 0.0f) {
            const float now_ = take(std::min(s.squirtLeft, s.squirtLeft * dt / 0.25f + 0.02f));
            s.squirtLeft -= now_;
            const int n = static_cast<int>(std::lround(now_ * per));
            const RE::NiPoint3 v = (out * 0.9f + down * 0.25f + side * Rand(-0.08f, 0.08f)) * s.squirtSpeed;
            Out(a, s.squirtStream, c + out * 0.3f, v, n, s.rgb);
        }
        // gush: the moment he is empty of the schlong
        if (s.gushLeft > 0.0f) {
            const float now_ = take(std::min(s.gushLeft, s.gushLeft * dt / 0.6f + 0.02f));
            s.gushLeft -= now_;
            const int n = static_cast<int>(std::lround(now_ * per));
            const RE::NiPoint3 v = out * Rand(8.0f, 14.0f) + down * 4.0f + side * Rand(-1.5f, 1.5f);
            Out(a, s.stream, c + out * 0.3f, v, n, s.rgb);
            return;
        }
        if (s.vol <= 0.001f) return;
        if (plugged) {   // still being taken: it seeps out round the shaft
            if (!g.seep) return;
            s.acc += s.vol * 0.02f * per * dt;
            const int n = static_cast<int>(s.acc);
            if (n <= 0) return;
            s.acc -= static_cast<float>(n);
            take(static_cast<float>(n) / per);
            const float t = Rand(0.0f, 6.2831853f);
            const RE::NiPoint3 radial = side * std::cos(t) + out.Cross(side) * std::sin(t);
            Out(a, g_nextStream++, c + radial * 1.2f + out * 0.2f, radial * 4.0f + out * 3.0f + down * 2.0f, n, s.rgb);
            return;
        }
        // open: it oozes out, faster the wider he gapes
        const float rate = 1.0f / std::max(g.leakMinutes * 60.0f, 10.0f) * (0.5f + 2.0f * gape) * (0.4f + 0.6f * std::min(s.vol, 2.0f));
        s.acc += rate * per * dt;
        const int n = static_cast<int>(s.acc);
        if (n <= 0) return;
        s.acc -= static_cast<float>(n);
        take(static_cast<float>(n) / per);
        if (Rand(0.0f, 1.0f) < 0.05f) s.stream = g_nextStream++;
        Out(a, s.stream, low, out * Rand(1.0f, 3.0f) + down * 2.0f, n, s.rgb);
    }

    void Tick() {
        Resolve();
        if (!g_api.emit || !g.on) return;
        static double last = 0.0;
        const double now = Now();
        const float dt = static_cast<float>(std::clamp(now - (last > 0.0 ? last : now - 0.1), 0.0, 0.5));
        last = now;
        if (g_api.semenOn && !g_api.semenOn()) return;
        std::lock_guard lk(g_mx);
        for (auto it = g_st.begin(); it != g_st.end();) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(it->first);
            State& s = it->second;
            const bool done = s.vol <= 0.001f && s.gushLeft <= 0.0f && s.squirtLeft <= 0.0f;
            if (!a || a->IsDead() || (done && now - s.lastWorked > 5.0)) { it = g_st.erase(it); continue; }
            if (a->Is3DLoaded()) StepOne(a, s, dt, now);
            ++it;
        }
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
        const float room = std::max(g.maxLoads - s.vol, 0.0f);
        const float kept = std::min(loads, room);
        if (rgb) {
            const float w0 = std::max(s.vol, 0.0f), w1 = std::max(kept, 0.001f);
            for (int i = 0; i < 3; ++i) s.rgb[i] = s.vol <= 0.001f ? rgb[i] : (s.rgb[i] * w0 + rgb[i] * w1) / (w0 + w1);
        }
        s.vol += kept;
        s.lastIn = Now();
        if (loads > kept) s.gushLeft += loads - kept;   // full: the rest comes straight back out
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
        s.squirtLeft += s.vol * g.clenchShare * std::clamp(amp, 0.2f, 1.5f);
        s.squirtSpeed = 12.0f + 18.0f * std::clamp(amp, 0.0f, 1.5f);
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
        act("Empty him", [&](State& s) { s.vol = s.gushLeft = s.squirtLeft = 0.0f; return std::string("nothing in him"); });
        Tr::TextDisabled("Left alone he oozes it out over time (faster when gaping: Gape and movement > Gape him open). Close the menu to see it.");
        {
            std::lock_guard lk(g_mx);
            auto it = g_st.find(a->GetFormID());
            Tr::TextDisabled("%s", Lang::Fmt("In him: {:.2f} loads", it == g_st.end() ? 0.0f : it->second.vol).c_str());
        }
        if (!last.empty()) Tr::TextDisabled("%s", last.c_str());
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
            ch |= Tr::SliderFloat("A full load drains in", &g.leakMinutes, 0.5f, 30.0f, "%.1f min");
            Help("While he is open. The wider he gapes, the faster it runs out.");
            float pct = g.gush * 100.0f;
            if (Tr::SliderFloat("Gush as the schlong slides out", &pct, 0.0f, 100.0f, "%.0f%%")) { g.gush = pct / 100.0f; ch = true; }
            ch |= Tr::Checkbox("Seeps out round the shaft while he is taken", &g.seep);
            ch |= Tr::Checkbox("A clench squirts some out", &g.clench);
            if (g.clench) {
                float c = g.clenchShare * 100.0f;
                if (Tr::SliderFloat("Squirted per clench", &c, 1.0f, 60.0f, "%.0f%%")) { g.clenchShare = c / 100.0f; ch = true; }
            }
            ch |= Tr::SliderFloat("Most he holds", &g.maxLoads, 1.0f, 10.0f, "%.1f loads");
            Help("More than this comes straight back out.");
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
        B("Link", g.on); F("LeakMinutes", g.leakMinutes, 0.5f, 30.0f); F("Gush", g.gush, 0.0f, 1.0f); B("Clench", g.clench);
        F("ClenchShare", g.clenchShare, 0.01f, 0.6f); B("Seep", g.seep); I("PerLoad", g.perLoad, 40, 400); F("MaxLoads", g.maxLoads, 1.0f, 10.0f);
        B("Dressed", g.dressed);
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
    __declspec(dllexport) void Anatomica_SemenIn(RE::Actor* recv, RE::Actor* giver, float loads, const float* rgb) {
        SemenIn(recv, giver, loads, rgb);
    }
}

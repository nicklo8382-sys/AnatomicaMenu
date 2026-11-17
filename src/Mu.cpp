#include "Mu.h"
#include "Lang.h"
#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

// See Mu.h.
namespace {
    struct Settings {
        bool on = true;             // Anatomica asks for the updates
        float every = 2.5f;         // seconds between updates while the shape keeps changing
        float settle = 1.0f;        // the shape has stood still this long: one last update
        float change = 0.08f;       // how much the shape has to change (0-1 of the full bulge / gape) to count
        bool peak = true;           // a travelling bulge: update when it is near the top of its run
        bool remind = true;         // a note at game start while MuDynamicNormalMap.ini still has the settings that cost the most
    } g;

    // ---- MuDynamicNormalMap.ini: the settings that work best with Anatomica asking for the updates
    constexpr const char* kIni = "Data/SKSE/Plugins/MuDynamicNormalMap.ini";
    struct Rec { const char* key; std::string want; const char* label; const char* why; bool tick; std::string now; };
    Rec g_recs[] = {
        {"RevertNormalMap", "false", "No flash back to the plain map while it updates",
         "With true, every update first shows the original normal map until the new one is done - with Anatomica asking "
         "during sex you'd see it flicker.", true, {}},
        {"DetectTickMS", "5000", "Its own checks less often",
         "Anatomica asks for the updates while he is taken, so MuDynamicNormalMap's own vertex watching can slow right down. "
         "Other changes (RaceMenu, weight, other mods' morphs) still update, just later. 1000 = its default (every second).", true, {}},
        {"UpdateDistance", "1000", "Only men nearby get rebuilt",
         "0 = everyone loaded, however far. Saves GPU work in towns.", false, {}},
        {"UpdateDistanceVramSave", "true", "Free the video memory of men further away",
         "Goes with the distance above: their rebuilt maps are dropped and made again when they come close.", false, {}},
        {"ProcessingInLoading", "true", "First rebuild during the loading screen",
         "Men don't pop from flat to shaded when you arrive - loading screens take a little longer.", false, {}},
    };
    int g_detectMs = 5000;   // the DetectTickMS asked for (slider)
    int g_distance = 1000;   // the UpdateDistance asked for (slider; 0 = everyone loaded)
    bool g_iniRead = false;
    std::string g_iniNote;
    bool g_reminded = false;
    std::string Trim(std::string x) {
        const auto a = x.find_first_not_of(" \t\r\n"), b = x.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
    }
    std::string Lower(std::string x) { for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return x; }
    // "Key = value" (not a comment): the key, the value
    bool KV(const std::string& line, std::string& k, std::string& v) {
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') return false;
        const auto eq = t.find('=');
        if (eq == std::string::npos) return false;
        k = Trim(t.substr(0, eq)); v = Trim(t.substr(eq + 1));
        if (auto c = v.find_first_of("#;"); c != std::string::npos) v = Trim(v.substr(0, c));
        return true;
    }
    void ReadIni() {
        g_iniRead = true;
        for (auto& r : g_recs) r.now.clear();
        std::ifstream in(kIni);
        std::string line, k, v;
        while (std::getline(in, line))
            if (KV(line, k, v)) for (auto& r : g_recs) if (k == r.key) r.now = v;
    }
    bool Good(const Rec& r) { return Lower(r.now) == r.want; }
    bool CostsMost() { return !Good(g_recs[0]) || std::atoi(g_recs[1].now.c_str()) < 2000; }   // still checking every second or two
    std::string ApplyIni() {
        std::vector<std::string> L;
        {
            std::ifstream in(kIni);
            for (std::string l; std::getline(in, l);) L.push_back(l);
        }
        if (L.empty()) return "MuDynamicNormalMap.ini was not found.";
        std::error_code ec;
        const std::filesystem::path bak = std::string(kIni) + ".anatomica_backup";
        if (!std::filesystem::exists(bak, ec)) std::filesystem::copy_file(kIni, bak, ec);
        int changed = 0;
        for (auto& line : L) {
            std::string k, v;
            if (!KV(line, k, v)) continue;
            for (auto& r : g_recs) {
                if (!r.tick || k != r.key || Lower(v) == r.want) continue;
                const auto eq = line.find('=');
                const auto vs = line.find_first_not_of(" \t", eq + 1);
                const auto ve = vs == std::string::npos ? std::string::npos : vs + v.size();
                line = line.substr(0, vs == std::string::npos ? eq + 1 : vs) + r.want + (ve == std::string::npos ? "" : line.substr(ve));
                ++changed;
            }
        }
        if (!changed) return "Nothing to change - the ticked settings are already set.";
        const std::string tmp = std::string(kIni) + ".tmp";
        {
            std::ofstream o(tmp, std::ios::trunc);
            for (auto& l : L) o << l << "\n";
            if (!o) return "Could not write MuDynamicNormalMap.ini.";
        }
        std::filesystem::rename(tmp, kIni, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            std::ofstream o(kIni, std::ios::trunc);
            for (auto& l : L) o << l << "\n";
            if (!o) return "Could not write MuDynamicNormalMap.ini.";
        }
        ReadIni();
        logger::info("Anatomica: MuDynamicNormalMap.ini - {} settings changed (backup MuDynamicNormalMap.ini.anatomica_backup)", changed);
        return std::format("Changed {} settings (the old file is kept as MuDynamicNormalMap.ini.anatomica_backup). "
                           "MuDynamicNormalMap reads them at the next game start (or hold its hotkey for 3 seconds).", changed);
    }
    std::mutex g_mx;
    int g_installed = -1;
    int g_asked = 0;                // updates asked this session (menu)

    // what changes his shape: the bulge (how strong, how far up), the stomach, the gape, the puffy rim (after-sex swelling and
    // the ring shapes while he is taken) and the imprint left afterwards
    struct Look { float bulge = 0.0f, reach = 0.0f, belly = 0.0f, gape = 0.0f, rim = 0.0f, imprint = 0.0f; };
    float Diff(const Look& a, const Look& b) {
        return std::max({std::abs(a.bulge - b.bulge), std::abs(a.reach - b.reach) / 8.0f, std::abs(a.belly - b.belly), std::abs(a.gape - b.gape),
                         std::abs(a.rim - b.rim), std::abs(a.imprint - b.imprint)});
    }
    struct St { Look sent, seen; double sentAt = -100.0, movedAt = -100.0; bool dirty = false; };
    std::unordered_map<RE::FormID, St> g_st;

    void Ask(RE::Actor* a) {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) return;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
        // body only (0x4): the anus and the belly are part of it
        vm->DispatchStaticCall("MuDynamicNormalMap", "QUpdateNormalmap", RE::MakeFunctionArguments(static_cast<RE::Actor*>(a), static_cast<std::int32_t>(0x4)), cb);
        ++g_asked;
    }
}

namespace Mu {
    bool Installed() {
        if (g_installed < 0) {
            std::error_code ec;
            g_installed = std::filesystem::exists("Data/SKSE/Plugins/MuDynamicNormalMap.dll", ec) ? 1 : 0;
        }
        return g_installed == 1;
    }

    void Tick(const std::vector<RE::Actor*>& all, double now) {
        if (!Installed()) return;
        std::lock_guard lk(g_mx);
        if (!g.on) { g_st.clear(); return; }
        if (!g_reminded && g.remind) {   // once a session, while its ini still costs the most
            g_reminded = true;
            if (!g_iniRead) ReadIni();
            if (CostsMost())
                if (auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton()) {
                    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
                    vm->DispatchStaticCall("Debug", "Notification", RE::MakeFunctionArguments(RE::BSFixedString(Lang::T(
                        "Anatomica: MuDynamicNormalMap found - its recommended settings are on the Integrations page."))), cb);
                }
        }
        for (auto* a : all) {
            if (!a || !a->Is3DLoaded()) continue;
            const auto* npc = a->GetActorBase();
            if (!npc || npc->GetSex() != RE::SEX::kMale) continue;
            const auto id = a->GetFormID();
            const auto b = Engine::BulgeOf(id);
            const float gp = Engine::GapeOf(id);
            float rim = 0.0f;
            for (const char* m : {"Anatomica Swell", "Anatomica Ring", "Anatomica Donut", "Anatomica Rosebud", "Anatomica Lips", "Anatomica Pucker"})
                rim = std::max(rim, Engine::MorphSum(a, m));
            Look now_{b.on ? b.amount : 0.0f, b.on ? b.reach : 0.0f, b.belly, std::max(gp, 0.0f), rim, b.imprint};
            auto it = g_st.find(id);
            if (it == g_st.end()) {
                if (Diff(now_, Look{}) < g.change) continue;   // nothing going on with him
                it = g_st.emplace(id, St{}).first;
            }
            St& s = it->second;
            if (Diff(now_, s.seen) > 0.01f) { s.seen = now_; s.movedAt = now; }
            if (Diff(now_, s.sent) >= g.change) s.dirty = true;
            const bool still = now - s.movedAt >= g.settle;
            // while it moves: at most every few seconds; once it stands still: straight away.
            // A bulge running up and back with each stroke: only when it is near the top of its run, so the shading
            // matches the shape that shows most (a map takes longer to make than one stroke)
            const bool peakOk = !g.peak || !b.on || b.roundMax <= 0.0f || b.reach >= b.roundMax * 0.85f;
            if (s.dirty && ((now - s.sentAt >= g.every && peakOk) || (still && now - s.sentAt >= 0.5) || now - s.sentAt >= g.every * 3.0f)) {
                Ask(a);
                s.sent = now_;
                s.sentAt = now;
                s.dirty = false;
            }
            // back to nothing and sent: forget him
            if (!s.dirty && Diff(now_, Look{}) < 0.01f && Diff(s.sent, Look{}) < 0.01f) g_st.erase(it);
        }
    }

    bool RenderSection() {
        const bool inst = Installed();
        if (!Tr::CollapsingHeader(inst ? "MuDynamicNormalMap###mdnm" : "MuDynamicNormalMap  (not installed)###mdnm")) return false;
        bool ch = false;
        std::lock_guard lk(g_mx);
        if (!inst) {
            Tr::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.4f, 1.0f}, "%s is not installed - these settings do nothing until it is (they are kept).", "MuDynamicNormalMap");
            ImGuiMCP::BeginDisabled();
        }
        Tr::TextWrapped("MuDynamicNormalMap rebuilds a man's normal map from his body as it is now, so the belly bulge, the "
                        "stomach and the gape are lit properly. Anatomica's detail map (anatomica_n.dds) gives it back the "
                        "fine folds and wrinkles of the anus.");
        ch |= Tr::Checkbox("Anatomica asks for the updates", &g.on);
        auto help = [](const char* t) {
            ImGuiMCP::SameLine();
            Tr::TextDisabled("(?)");
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::BeginTooltip();
                ImGuiMCP::PushTextWrapPos(ImGuiMCP::GetFontSize() * 30.0f);
                Tr::TextUnformatted(t);
                ImGuiMCP::PopTextWrapPos();
                ImGuiMCP::EndTooltip();
            }
        };
        help("Only when his bulge, stomach or gape has really changed: every few seconds while it moves, and once more when it "
             "settles. Then turn MuDynamicNormalMap's own real-time detection down (DetectTickMS = 5000 or more) or off "
             "(RealtimeDetect = false) in MuDynamicNormalMap.ini - it otherwise rebuilds both men every second or two all "
             "through a scene.");
        if (g.on) {
            ImGuiMCP::Indent();
            ch |= Tr::SliderFloat("While it moves, at most every", &g.every, 0.5f, 10.0f, "%.1f s");
            ch |= Tr::SliderFloat("Settled after", &g.settle, 0.3f, 5.0f, "%.1f s");
            float pct = g.change * 100.0f;
            if (Tr::SliderFloat("Change that counts", &pct, 2.0f, 40.0f, "%.0f%%")) { g.change = pct / 100.0f; ch = true; }
            help("How much the bulge, stomach, gape or puffy rim has to change before it is worth a new normal map. Lower = "
                 "closer shading, more work.");
            ch |= Tr::Checkbox("Travelling bulge: shade it at the top of its run", &g.peak);
            help("A normal map takes longer to make than one stroke, so a bulge running up and back can't be followed. This "
                 "updates when it is near the furthest it goes, so the shading matches the shape that shows most. Off: whenever "
                 "the time is up, wherever it is.");
            Tr::TextDisabled("Updates asked this session: %d", g_asked);
            ImGuiMCP::Unindent();
        }
        if (Tr::TreeNode("Recommended MuDynamicNormalMap settings###mdnmini")) {
            if (!g_iniRead) ReadIni();
            if (CostsMost() && g.on)
                Tr::TextColored(ImGuiMCP::ImVec4{1.0f, 0.8f, 0.3f, 1.0f}, "MuDynamicNormalMap still rebuilds every second or two by itself - tick and apply the first two.");
            {   // how often its own checks run: chosen here
                int ms = g_detectMs;
                ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 12.0f);
                if (Tr::SliderInt("Its own checks every", &ms, 1000, 15000, "%d ms")) {
                    g_detectMs = std::clamp(ms / 500 * 500, 1000, 15000);
                    g_recs[1].want = std::to_string(g_detectMs);
                    ch = true;
                }
                help("DetectTickMS: how often MuDynamicNormalMap looks for bodies that changed shape by itself. Anatomica asks "
                     "for the updates during sex, so this can be slow: 5000 (5 s) is a good start, 1000 is its default.");
                int dist = g_distance;
                ImGuiMCP::SetNextItemWidth(ImGuiMCP::GetFontSize() * 12.0f);
                if (Tr::SliderInt("Rebuild men within", &dist, 0, 5000, dist == 0 ? "everyone" : "%d units")) {
                    g_distance = dist < 250 ? 0 : std::clamp(dist / 50 * 50, 250, 5000);
                    g_recs[2].want = std::to_string(g_distance);
                    ch = true;
                }
                help("UpdateDistance: about 2 m per 100 units (1000 = 20 m). 0 = everyone loaded, however far.");
            }
            for (auto& r : g_recs) {
                ImGuiMCP::PushID(r.key);
                if (Good(r)) { Tr::TextDisabled("%s: done (%s = %s)", Lang::T(r.label), r.key, r.now.c_str()); }
                else {
                    Tr::Checkbox(r.label, &r.tick);
                    help(r.why);
                    ImGuiMCP::SameLine();
                    Tr::TextDisabled("%s: %s -> %s", r.key, r.now.empty() ? "?" : r.now.c_str(), r.want.c_str());
                }
                ImGuiMCP::PopID();
            }
            if (Tr::Button("Apply the ticked settings")) g_iniNote = ApplyIni();
            ImGuiMCP::SameLine();
            if (Tr::Button("Read again")) { ReadIni(); g_iniNote.clear(); }
            if (!g_iniNote.empty()) Tr::TextWrapped("%s", g_iniNote.c_str());
            Tr::TextDisabled("Only changes the lines named here; with Mod Organizer the edited copy goes to Overwrite.");
            ch |= Tr::Checkbox("Remind me at game start while these are not set", &g.remind);
            ImGuiMCP::TreePop();
        }
        if (!inst) ImGuiMCP::EndDisabled();
        return ch;
    }

    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "MuDynamicNormalMap";
        std::lock_guard lk(g_mx);
        if (save) {
            ini.SetBoolValue(S, "AnatomicaAsks", g.on);
            ini.SetDoubleValue(S, "Every", g.every);
            ini.SetDoubleValue(S, "Settle", g.settle);
            ini.SetDoubleValue(S, "Change", g.change);
            ini.SetBoolValue(S, "Remind", g.remind);
            ini.SetLongValue(S, "DetectTickMS", g_detectMs);
            ini.SetLongValue(S, "UpdateDistance", g_distance);
            ini.SetBoolValue(S, "Peak", g.peak);
        } else {
            g.remind = ini.GetBoolValue(S, "Remind", g.remind);
            g_detectMs = std::clamp(static_cast<int>(ini.GetLongValue(S, "DetectTickMS", g_detectMs)), 1000, 15000);
            g_recs[1].want = std::to_string(g_detectMs);
            g.on = ini.GetBoolValue(S, "AnatomicaAsks", g.on);
            g.every = std::clamp(static_cast<float>(ini.GetDoubleValue(S, "Every", g.every)), 0.5f, 10.0f);
            g.settle = std::clamp(static_cast<float>(ini.GetDoubleValue(S, "Settle", g.settle)), 0.3f, 5.0f);
            g.change = std::clamp(static_cast<float>(ini.GetDoubleValue(S, "Change", g.change)), 0.02f, 0.4f);
        }
    }
}

#include "During.h"
#include "Colour.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
    // PPA's anus settings in Anatomica's override file (what the installer ships)
    struct Tight {
        float scale = 0.4f;       // Scale: how far he opens around the schlong
        float scaleMax = 1.1f;    // ScaleMax: the widest PPA opens him
        float grip = 1.0f;        // GripStrength: how hard the rim grips the shaft
        float fwd = 1.0f;         // GripClampForward: how far the rim is drawn in on the push
        float back = 0.3f;        // GripClampBackward: how far it is pulled out on the withdraw
        float rub = 1.5f;         // RubIntensity
    };
    Tight g_t;
    bool g_read = false, g_found = false;
    std::string g_note;
    std::atomic<bool> g_donutOn{false};
    std::atomic<float> g_donutAmt{0.5f};
    std::mutex g_wmx;
    bool g_canalOn = true, g_suckOn = true, g_bulgeOn = false;
    During::Want g_w;          // the amounts (used when the tick box is on)
    int g_bulgeWho = 0;        // 0 everyone, 1 you and your followers, 2 only you

    constexpr const char* kToml = "Data/SKSE/Plugins/ppa-override-configs/anatomica.toml";
    struct Key { const char* name; float Tight::*v; };
    constexpr Key kKeys[] = {{"Scale", &Tight::scale}, {"ScaleMax", &Tight::scaleMax}, {"GripStrength", &Tight::grip},
                             {"GripClampForward", &Tight::fwd}, {"GripClampBackward", &Tight::back}, {"RubIntensity", &Tight::rub}};

    std::string Trim(const std::string& s) {
        const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    std::vector<std::string> Lines() {
        std::vector<std::string> out;
        std::ifstream f(kToml);
        for (std::string l; std::getline(f, l);) out.push_back(l);
        return out;
    }
    // the [Override.Anus] section: first line after its header .. the next header
    bool Section(const std::vector<std::string>& L, size_t& from, size_t& to) {
        for (size_t i = 0; i < L.size(); ++i)
            if (Trim(L[i]) == "[Override.Anus]") {
                from = i + 1;
                to = L.size();
                for (size_t k = from; k < L.size(); ++k)
                    if (!Trim(L[k]).empty() && Trim(L[k])[0] == '[') { to = k; break; }
                return true;
            }
        return false;
    }
    // "Key = value  # comment" -> key, value
    bool KeyOf(const std::string& line, std::string& key, float* val) {
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#') return false;
        const auto eq = t.find('=');
        if (eq == std::string::npos) return false;
        key = Trim(t.substr(0, eq));
        if (val) *val = static_cast<float>(std::atof(t.substr(eq + 1).c_str()));
        return true;
    }
    void ReadToml() {
        g_read = true;
        const auto L = Lines();
        size_t a = 0, b = 0;
        g_found = Section(L, a, b);
        if (!g_found) return;
        for (size_t i = a; i < b; ++i) {
            std::string k;
            float v = 0.0f;
            if (!KeyOf(L[i], k, &v)) continue;
            for (const auto& kk : kKeys) if (k == kk.name) g_t.*kk.v = v;
        }
    }
    bool WriteToml() {
        auto L = Lines();
        size_t a = 0, b = 0;
        if (!Section(L, a, b)) return false;
        for (const auto& kk : kKeys) {
            const std::string line = std::format("{} = {:.2f}", kk.name, g_t.*kk.v);
            bool done = false;
            for (size_t i = a; i < b && !done; ++i) {
                std::string k;
                if (KeyOf(L[i], k, nullptr) && k == kk.name) { L[i] = line; done = true; }
            }
            if (!done) { L.insert(L.begin() + static_cast<std::ptrdiff_t>(a), line); ++b; }
        }
        const std::string tmp = std::string(kToml) + ".tmp";
        {
            std::ofstream o(tmp, std::ios::trunc);
            if (!o) return false;
            for (const auto& l : L) o << l << "\n";
            if (!o) return false;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, kToml, ec);
        if (ec) {   // some setups cannot replace the file: write it in place
            std::filesystem::remove(tmp, ec);
            std::ofstream o(kToml, std::ios::trunc);
            if (!o) return false;
            for (const auto& l : L) o << l << "\n";
            return static_cast<bool>(o);
        }
        return true;
    }

    void Help(const char* t) {
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::BeginTooltip();
            ImGuiMCP::PushTextWrapPos(ImGuiMCP::GetFontSize() * 30.0f);
            ImGuiMCP::TextUnformatted(t);
            ImGuiMCP::PopTextWrapPos();
            ImGuiMCP::EndTooltip();
        }
    }
}

namespace During {
    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "DuringSex";
        bool on = g_donutOn;
        float amt = g_donutAmt;
        if (save) {
            ini.SetBoolValue(S, "Donut", on);
            ini.SetDoubleValue(S, "DonutAmount", amt);
        } else {
            g_donutOn = ini.GetBoolValue(S, "Donut", on);
            g_donutAmt = std::clamp(static_cast<float>(ini.GetDoubleValue(S, "DonutAmount", amt)), 0.0f, 1.0f);
        }
        std::lock_guard lk(g_wmx);
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto F = [&](const char* k, float& v, float lo, float hi) {
            if (save) ini.SetDoubleValue(S, k, v);
            else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi);
        };
        B("OpenInside", g_canalOn); F("OpenInsideAmount", g_w.canal, 0.0f, 1.5f);
        B("Suck", g_suckOn); F("SuckAmount", g_w.suck, 0.0f, 1.5f);
        B("Bulge", g_bulgeOn); F("BulgeAmount", g_w.bulge, 0.0f, 2.5f); F("BulgeStart", g_w.bulgeStart, 4.0f, 30.0f);
        B("BulgeBigOnly", g_w.bulgeBig);
        F("BulgeOutline", g_w.outline, 0.0f, 1.0f); F("BulgePulse", g_w.pulse, 0.0f, 1.0f);
        F("BulgeLinger", g_w.linger, 0.5f, 30.0f);
        if (save) ini.SetLongValue(S, "BulgeThrough", g_w.bulgeThrough);
        else g_w.bulgeThrough = std::clamp(static_cast<int>(ini.GetLongValue(S, "BulgeThrough", g_w.bulgeThrough)), 0, 2);
        if (save) ini.SetLongValue(S, "BulgeWho", g_bulgeWho);
        else g_bulgeWho = std::clamp(static_cast<int>(ini.GetLongValue(S, "BulgeWho", g_bulgeWho)), 0, 2);
    }
    Want Get() {
        std::lock_guard lk(g_wmx);
        Want w = g_w;
        if (!g_canalOn) w.canal = 0.0f;
        if (!g_suckOn) w.suck = 0.0f;
        if (!g_bulgeOn) w.bulge = 0.0f;
        return w;
    }
    bool BulgeFor(RE::Actor* man, RE::Actor* player) {
        int who;
        { std::lock_guard lk(g_wmx); who = g_bulgeWho; }
        if (man == player) return true;
        if (who == 2) return false;
        if (who == 1) return man && man->IsPlayerTeammate();
        return true;
    }
    float Donut() { return g_donutOn ? g_donutAmt.load() : 0.0f; }

    void __stdcall RenderPage() {
        if (!g_read) ReadToml();
        bool changed = false;
        ImGuiMCP::TextWrapped("How he looks and how tight he is while he is being taken. (What happens after the scene is on the "
                              "Gape, Movement and After sex pages.)");
        ImGuiMCP::Separator();

        if (ImGuiMCP::CollapsingHeader("How tight he is (PPA)###tight")) {
            if (!g_found) {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.4f, 1.0f},
                                      "Needs the installer's \"PPA integration\" option (and Penetration Physics / PPA): its file "
                                      "SKSE\\Plugins\\ppa-override-configs\\anatomica.toml was not found.");
                ImGuiMCP::BeginDisabled();
            }
            ImGuiMCP::TextWrapped("During sex PPA (Penetration Physics) opens his anus around the schlong. These are PPA's settings "
                                  "for Anatomica's anus - every man with Anatomica's skeleton bones.");
            static const char* presets[] = {"Choose a preset...", "Very tight", "Tight", "Anatomica default", "Loose", "Very loose"};
            static int pick = 0;
            if (ImGuiMCP::Combo("Preset", &pick, presets, 6) && pick > 0) {
                constexpr float sc[5][2] = {{0.25f, 0.80f}, {0.32f, 0.95f}, {0.40f, 1.10f}, {0.55f, 1.50f}, {0.70f, 2.00f}};
                g_t.scale = sc[pick - 1][0];
                g_t.scaleMax = sc[pick - 1][1];
                if (pick == 3) g_t = Tight{};
                pick = 0;
                changed = true;
            }
            Help("Sets how far and how wide he opens. \"Anatomica default\" puts every setting below back to what the mod ships.");
            changed |= ImGuiMCP::SliderFloat("How far he opens around the schlong", &g_t.scale, 0.1f, 1.5f, "%.2f");
            Help("PPA \"Scale\": how far the ring spreads for a schlong. Lower = tighter, he grips the shaft; higher = he opens "
                 "wider. Anatomica ships 0.40 (PPA's own default, 2.0, tears the cleft open on this mesh).");
            changed |= ImGuiMCP::SliderFloat("The widest he can open", &g_t.scaleMax, 0.3f, 3.0f, "%.2f");
            Help("PPA \"ScaleMax\": a cap for big schlongs. Lower = even a huge one only opens him so far.");
            changed |= ImGuiMCP::SliderFloat("Grip on the shaft (drag)", &g_t.grip, 0.0f, 2.0f, "%.2f");
            Help("PPA \"GripStrength\": how hard the rim holds onto the shaft - drawn in on the push, pulled out (a small pucker) "
                 "on the withdraw. 0 = no drag.");
            changed |= ImGuiMCP::SliderFloat("Drawn in on the push", &g_t.fwd, 0.0f, 3.0f, "%.2f");
            Help("PPA \"GripClampForward\": how far the grip may pull the rim into his body.");
            changed |= ImGuiMCP::SliderFloat("Pulled out on the withdraw", &g_t.back, 0.0f, 1.5f, "%.2f");
            Help("PPA \"GripClampBackward\": how far the grip may pull the rim out of his body (the pucker). Over 0.6 can look "
                 "extreme.");
            changed |= ImGuiMCP::SliderFloat("Rubbing", &g_t.rub, 0.0f, 3.0f, "%.2f");
            Help("PPA \"RubIntensity\": how much the rim moves with each stroke.");
            if (changed) g_note = "Not saved yet - press Save to PPA.";
            if (ImGuiMCP::Button("Save to PPA")) {
                g_note = WriteToml() ? "Saved. Press F5 in game (PPA's reload key) to use it now, or it applies at the next game start."
                                     : "Could not write anatomica.toml (is the PPA integration option installed?).";
                if (g_note[0] == 'S') logger::info("Anatomica: PPA tightness saved (Scale {:.2f}, ScaleMax {:.2f}, Grip {:.2f})", g_t.scale, g_t.scaleMax, g_t.grip);
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Read again")) { ReadToml(); g_note = "Read from anatomica.toml."; }
            if (!g_note.empty()) ImGuiMCP::TextWrapped("%s", g_note.c_str());
            ImGuiMCP::TextDisabled("PPA's settings are for every man at once. Saved in PPA's file, not in your Anatomica presets.");
            if (!g_found) ImGuiMCP::EndDisabled();
        }

        if (ImGuiMCP::CollapsingHeader("Inside him: opening, suck, belly bulge###inside")) {
            ImGuiMCP::TextDisabled("These use Anatomica's own sliders (Anatomica Canal / Pout / Bulge 1-5): rebuild the body in "
                                   "BodySlide with \"Build Morphs\" ticked after updating Anatomica.");
            bool ch = false;
            std::lock_guard lk(g_wmx);
            ch |= ImGuiMCP::Checkbox("Open the inside round the schlong", &g_canalOn);
            Help("While a schlong is in him the canal opens along its whole length, so the shaft no longer pushes through the "
                 "inside walls (PPA only opens the entrance). Bigger partners open it a little more. Recommended on.");
            if (g_canalOn) {
                float pct = g_w.canal * 100.0f;
                if (ImGuiMCP::SliderFloat("How wide inside", &pct, 0.0f, 150.0f, "%.0f%%")) { g_w.canal = pct / 100.0f; ch = true; }
            }
            ch |= ImGuiMCP::Checkbox("Suck on the schlong as it leaves", &g_suckOn);
            Help("As the shaft pulls out his rim and the first part of the inside are drawn out with it - a pout that follows "
                 "each withdraw and snaps back on the next push. Faster pull-outs draw it out more. Works with PPA's grip "
                 "(How tight he is, above), which holds the entrance itself.");
            if (g_suckOn) {
                float pct = g_w.suck * 100.0f;
                if (ImGuiMCP::SliderFloat("How strong the suck", &pct, 0.0f, 150.0f, "%.0f%%")) { g_w.suck = pct / 100.0f; ch = true; }
            }
            ch |= ImGuiMCP::Checkbox("Belly bulge", &g_bulgeOn);
            Help("When the tip is deep in him his lower belly bulges where it is, and the bump moves up and down with each "
                 "stroke. Off by default.");
            if (g_bulgeOn) {
                float pct = g_w.bulge * 100.0f;
                if (ImGuiMCP::SliderFloat("Bulge strength", &pct, 0.0f, 250.0f, "%.0f%%")) { g_w.bulge = pct / 100.0f; ch = true; }
                ch |= ImGuiMCP::SliderFloat("Shows from this deep", &g_w.bulgeStart, 4.0f, 30.0f, "%.0f units");
                Help("How far in the tip has to be (game units, about 1.4 cm each) before the bulge starts; it grows over the "
                     "next 5 units. Lower = even average schlongs show; higher = only very long ones.");
                ch |= ImGuiMCP::Checkbox("Only from big partners (L, XL and creatures)", &g_w.bulgeBig);
                Help("Needs The New Gentleman for the size (creatures always count).");
                {
                    float o = g_w.outline * 100.0f;
                    if (ImGuiMCP::SliderFloat("Shaft outline", &o, 0.0f, 100.0f, "%.0f%%")) { g_w.outline = o / 100.0f; ch = true; }
                    Help("The length of the shaft shows up his belly behind the tip, not just a bump at the tip. 0% = only the tip.");
                    float pu = g_w.pulse * 100.0f;
                    if (ImGuiMCP::SliderFloat("Thrust pulse", &pu, 0.0f, 100.0f, "%.0f%%")) { g_w.pulse = pu / 100.0f; ch = true; }
                    Help("Each fast push gives a quick extra bump that dies away at once - it reacts to the thrusts. 0% = smooth.");
                    ch |= ImGuiMCP::SliderFloat("Goes down over", &g_w.linger, 0.5f, 30.0f, "%.1f s");
                    Help("After he pulls out the bump eases down over this long (up to half as long again after a long, hard "
                         "round) instead of vanishing at once.");
                    static const char* thr[] = {"Nothing - only when he is bare", "Clothes, but not armour", "Clothes and armour"};
                    ch |= ImGuiMCP::Combo("Shows through", &g_w.bulgeThrough, thr, 3);
                    Help("What the bulge may show through. Nothing: only when nothing covers his body (revealing armour, "
                         "Integrations page, counts as bare). Clothes: also through robes, shirts and other clothing. Clothes and "
                         "armour: always - it can poke through a stiff cuirass.");
                }
                static const char* who[] = {"Everyone", "You and your followers", "Only you"};
                ch |= ImGuiMCP::Combo("Who gets the bulge", &g_bulgeWho, who, 3);
            }
            if (ch) {
                g_wmx.unlock();
                Colour::SaveNow();
                g_wmx.lock();
            }
        }

        if (ImGuiMCP::CollapsingHeader("Shape while he is taken###shape")) {
            bool on = g_donutOn;
            float amt = g_donutAmt;
            bool ch = false;
            ch |= ImGuiMCP::Checkbox("Puffy rim (donut) while he is taken", &on);
            Help("The rim swells into a soft, puffy ring as he is worked open - more the longer it goes on - and settles a few "
                 "seconds after the schlong leaves him (the after-sex swelling, After sex page, carries on from there). Uses "
                 "the Anatomica Swell slider: the body needs \"Build Morphs\" in BodySlide.");
            if (on) {
                float pct = amt * 100.0f;
                if (ImGuiMCP::SliderFloat("How puffy, at most", &pct, 0.0f, 100.0f, "%.0f%%")) { amt = pct / 100.0f; ch = true; }
            }
            if (ch) {
                g_donutOn = on;
                g_donutAmt = amt;
                Colour::SaveNow();
            }
        }
    }
}

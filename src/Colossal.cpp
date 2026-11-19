#include "Colossal.h"
#include "Lang.h"
#include "Mnc.h"
#include "Size.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <random>
#include <unordered_map>

namespace Colossal {
    namespace {
        //                          amount force pulses thick sticky stretch
        Load L(float a, float f, float p, float t, float s, float st) { Load l; l.amount = a; l.force = f; l.pulses = p; l.thick = t; l.sticky = s; l.stretch = st; return l; }
        Load LC(float a, float f, float p, float t, float s, float st, float r, float g, float b) {
            Load l = L(a, f, p, t, s, st); l.ownColour = true; l.rgb[0] = r; l.rgb[1] = g; l.rgb[2] = b; return l;
        }
        // a sensible start for every creature type (each can be changed in the menu)
        const std::map<std::string, Load>& Defaults() {
            static const std::map<std::string, Load> d{
                {"knotted",   L(3.0f, 1.1f, 2.0f, 1.2f, 0.85f, 1.0f)},   // a long tie: many spurts, thin and runny
                {"flared",    L(4.0f, 1.6f, 1.6f, 1.6f, 1.0f, 1.2f)},    // horses: huge and forceful
                {"colossal",  L(3.5f, 1.4f, 1.4f, 1.8f, 1.5f, 1.4f)},    // trolls, giants, bears: thick and heavy
                {"tapered",   L(1.5f, 1.0f, 1.2f, 0.9f, 1.0f, 1.0f)},
                {"pizzle",    L(2.5f, 1.5f, 1.2f, 1.1f, 0.9f, 1.0f)},
                {"segmented", LC(2.0f, 0.8f, 1.0f, 1.4f, 2.0f, 1.6f, 0.86f, 0.90f, 0.72f)},   // chitin: thick, pale green-yellow
                {"ridged",    L(3.0f, 1.4f, 1.3f, 1.4f, 1.2f, 1.2f)},    // dragons, daedroth
                {"barbed",    L(2.0f, 1.2f, 1.2f, 1.2f, 1.0f, 1.0f)},
                {"twin",      L(2.0f, 1.1f, 1.3f, 1.1f, 1.0f, 1.0f)},
                {"tentacle",  L(2.5f, 1.0f, 2.0f, 1.3f, 1.8f, 1.8f)},
                {"cold",      LC(0.8f, 0.8f, 0.8f, 1.0f, 1.6f, 1.0f, 0.80f, 0.82f, 0.80f)},  // undead: little, cold and grey
                {"piston",    L(1.5f, 2.0f, 1.0f, 1.0f, 0.7f, 0.8f)},    // dwarven: few hard, oily spurts
                {"elemental", LC(1.5f, 1.3f, 1.0f, 1.2f, 1.3f, 1.2f, 0.82f, 0.90f, 0.98f)},
            };
            return d;
        }
        Load CreatureDefault() { return L(2.5f, 1.3f, 1.4f, 1.3f, 1.2f, 1.2f); }

        struct Settings {
            bool on = true;
            bool bySize = true;            // TNG L (half) / XL (full)
            float chance = 0.10f;          // any other man, per scene
            Load men = L(2.0f, 1.3f, 1.4f, 1.3f, 1.2f, 1.2f);
            bool creatures = true;         // (More Nasty Critters only)
            std::map<std::string, Load> types;              // type key -> its load
            std::map<std::string, std::pair<bool, Load>> groups;   // MNC group -> own load on, the load
        } g;
        std::mutex g_mx;

        struct Roll { double last = 0.0; bool hit = false; };
        std::unordered_map<RE::FormID, Roll> g_rolls;      // the chance, once per scene (a scene = until he is not asked about for 3 minutes)
        double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
        std::mt19937& Rng() { static std::mt19937 r{std::random_device{}()}; return r; }

        Load TypeLoad(const std::string& key) {
            if (auto it = g.types.find(key); it != g.types.end()) return it->second;
            if (auto it = Defaults().find(key); it != Defaults().end()) return it->second;
            return CreatureDefault();
        }
        Load Mix(const Load& l, float w) {   // w 0 = normal, 1 = l
            Load o = l;
            auto m = [w](float v) { return 1.0f + (v - 1.0f) * w; };
            o.amount = m(l.amount); o.force = m(l.force); o.pulses = m(l.pulses); o.thick = m(l.thick); o.sticky = m(l.sticky); o.stretch = m(l.stretch);
            return o;
        }
        bool Male(RE::Actor* a) { auto* b = a ? a->GetActorBase() : nullptr; return b && b->GetSex() != RE::SEX::kFemale; }

        void Help(const char* t) {
            ImGuiMCP::SameLine();
            Tr::TextDisabled("(?)");
            if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::PushTextWrapPos(420.0f); Tr::TextUnformatted(t); ImGuiMCP::PopTextWrapPos(); ImGuiMCP::EndTooltip(); }
        }
        // the six multipliers and the colour of one load
        bool Edit(const std::string& id, Load& l) {
            bool ch = false;
            auto sl = [&](const char* label, float* v, float lo, float hi) {
                ch |= Tr::SliderFloat(std::format("{}##{}", label, id).c_str(), v, lo, hi, "x%.2f");
            };
            sl("Amount", &l.amount, 0.2f, 8.0f);
            sl("Spurts", &l.pulses, 0.3f, 4.0f);
            sl("Force", &l.force, 0.3f, 3.0f);
            sl("Strand thickness", &l.thick, 0.3f, 3.0f);
            sl("Thick and sticky", &l.sticky, 0.2f, 3.0f);
            sl("Stretch", &l.stretch, 0.2f, 3.0f);
            ch |= Tr::Checkbox(std::format("Own colour##oc{}", id).c_str(), &l.ownColour);
            if (l.ownColour) {
                ImGuiMCP::SameLine();
                ch |= Tr::ColorEdit3(std::format("##col{}", id).c_str(), l.rgb, ImGuiMCP::ImGuiColorEditFlags_NoInputs);
            }
            return ch;
        }
    }

    std::string Load::Save() const {
        return std::format("{:.2f}|{:.2f}|{:.2f}|{:.2f}|{:.2f}|{:.2f}|{}|{:.3f}|{:.3f}|{:.3f}", amount, force, pulses, thick, sticky, stretch,
                           ownColour ? 1 : 0, rgb[0], rgb[1], rgb[2]);
    }
    Load Load::Parse(const char* v, const Load& def) {
        Load l = def;
        if (!v || !*v) return l;
        float f[10]{};
        int n = 0;
        std::string s(v);
        size_t p = 0;
        while (n < 10 && p <= s.size()) {
            const size_t q = s.find('|', p);
            const std::string part = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
            try { f[n] = std::stof(part); } catch (...) { return def; }
            ++n;
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (n < 6) return def;
        auto c = [](float x, float lo, float hi) { return std::isfinite(x) ? std::clamp(x, lo, hi) : 1.0f; };
        l.amount = c(f[0], 0.2f, 8.0f); l.force = c(f[1], 0.3f, 3.0f); l.pulses = c(f[2], 0.3f, 4.0f);
        l.thick = c(f[3], 0.3f, 3.0f); l.sticky = c(f[4], 0.2f, 3.0f); l.stretch = c(f[5], 0.2f, 3.0f);
        if (n >= 10) { l.ownColour = f[6] > 0.5f; for (int i = 0; i < 3; ++i) l.rgb[i] = std::clamp(f[7 + i], 0.0f, 1.0f); }
        return l;
    }

    bool For(RE::Actor* giver, RE::Actor* receiver, float& weight, Load& load) {
        (void)receiver;
        weight = 0.0f;
        load = Load{};
        if (!giver) return false;
        const bool creature = Size::IsCreatureActor(giver);
        const bool mnc = creature && Mnc::Active();
        const std::string key = mnc ? Mnc::TypeFor(giver) : std::string();
        const std::string group = mnc ? Mnc::GroupName(giver) : std::string();
        const int cat = creature ? -1 : Size::Cat(giver);
        std::lock_guard lk(g_mx);
        if (!g.on) return false;
        if (creature) {   // without More Nasty Critters creatures stay as they are
            if (!mnc || !g.creatures) return true;
            if (auto it = g.groups.find(group); !group.empty() && it != g.groups.end() && it->second.first) load = it->second.second;
            else load = TypeLoad(key);
            weight = 1.0f;
            return true;
        }
        if (!Male(giver)) return true;
        float w = 0.0f;
        if (g.bySize) w = cat == 4 ? 1.0f : cat == 3 ? 0.5f : 0.0f;
        if (w < 1.0f && g.chance > 0.0f) {
            auto& r = g_rolls[giver->GetFormID()];
            const double now = Now();
            if (now - r.last > 180.0) r.hit = std::uniform_real_distribution<float>(0.0f, 1.0f)(Rng()) < g.chance;
            r.last = now;
            if (r.hit) w = 1.0f;
        }
        if (w <= 0.0f) return true;
        weight = w;
        load = Mix(g.men, w);
        return true;
    }

    std::string Describe(RE::Actor* giver) {
        float w = 0.0f;
        Load l;
        if (!giver || !For(giver, nullptr, w, l)) return {};
        const char* nm = giver->GetDisplayFullName();
        if (w <= 0.0f) return Lang::Fmt("{}: a normal load", std::string(nm ? nm : "?"));
        return Lang::Fmt("{}: Colossal Load {:.0f}% - amount x{:.2f}, spurts x{:.2f}, force x{:.2f}", std::string(nm ? nm : "?"), w * 100.0f, l.amount, l.pulses, l.force);
    }

    bool RenderSection() {
        if (!Tr::CollapsingHeader("Colossal Load (OStrands)###colossal")) return false;
        bool ch = false;
        std::unique_lock lk(g_mx);
        Tr::TextWrapped("Some finishes are bigger: more cum, more and harder spurts, thicker and stickier. OStrands draws it "
                        "(0.8.0.4 or newer, with \"Anatomica's Colossal Load\" on in its Anatomica settings); Anatomica decides who and how much.");
        ch |= Tr::Checkbox("Colossal Load", &g.on);
        Help("Off: OStrands uses its own settings for everyone.");
        if (!g.on) return ch;
        if (Tr::TreeNode("Men###clmen")) {
            ch |= Tr::Checkbox("By his schlong size", &g.bySize);
            Help("Needs The New Gentleman: size L gets half of the Colossal Load below, XL all of it. Average and smaller stay normal.");
            float pc = g.chance * 100.0f;
            if (Tr::SliderFloat("Chance per scene for any other man", &pc, 0.0f, 100.0f, "%.0f%%")) { g.chance = pc / 100.0f; ch = true; }
            Help("Rolled once per man per scene: when it hits, his finish is a full Colossal Load whatever his size. 0% = off.");
            Tr::TextDisabled("A man's Colossal Load (x his normal load):");
            ch |= Edit("men", g.men);
            ImGuiMCP::TreePop();
        }
        if (Mnc::Active()) {
            if (Tr::TreeNode("Creatures (More Nasty Critters)###clcr")) {
                ch |= Tr::Checkbox("Creatures finish with a Colossal Load", &g.creatures);
                Help("Each by its type (set for each More Nasty Critters group on this page, under More Nasty Critters). A group can have its own below.");
                if (g.creatures) {
                    if (Tr::TreeNode("By creature type###cltypes")) {
                        for (auto& [key, name] : Size::CreatureTypeList()) {
                            if (key == "cut" || key == "uncut") continue;
                            if (!Tr::TreeNode(std::format("{}###clt_{}", Lang::T(name.c_str()), key).c_str())) continue;
                            Load l = TypeLoad(key);
                            if (Edit("t" + key, l)) { g.types[key] = l; ch = true; }
                            if (Tr::SmallButton(std::format("Default##cld{}", key).c_str())) { g.types.erase(key); ch = true; }
                            ImGuiMCP::TreePop();
                        }
                        ImGuiMCP::TreePop();
                    }
                    if (Tr::TreeNode("Groups with their own load###clgroups")) {
                        Tr::TextDisabled("Tick a group to give it its own load instead of its type's (troll and bear are both Colossal, for example).");
                        for (auto& gr : Mnc::Groups()) {
                            auto it = g.groups.find(gr.name);
                            bool own = it != g.groups.end() && it->second.first;
                            if (Tr::Checkbox(std::format("{}##clg_{}", gr.name, gr.name).c_str(), &own)) {
                                auto& e = g.groups[gr.name];
                                if (own && it == g.groups.end()) e.second = TypeLoad(gr.type.empty() ? std::string("colossal") : gr.type);
                                e.first = own;
                                ch = true;
                                it = g.groups.find(gr.name);
                            }
                            if (own && it != g.groups.end()) {
                                ImGuiMCP::Indent();
                                ch |= Edit("g" + gr.name, it->second.second);
                                ImGuiMCP::Unindent();
                            }
                        }
                        ImGuiMCP::TreePop();
                    }
                }
                ImGuiMCP::TreePop();
            }
        }
        if (auto sel = RE::Console::GetSelectedRef(); sel && sel->As<RE::Actor>()) {
            lk.unlock();
            const std::string d = Describe(sel->As<RE::Actor>());
            lk.lock();
            if (!d.empty()) Tr::TextDisabled("%s", d.c_str());
        } else {
            Tr::TextDisabled("Click someone in the console (~) to see what his load would be.");
        }
        return ch;
    }

    void Ini(CSimpleIniA& ini, bool save) {
        std::lock_guard lk(g_mx);
        constexpr const char* S = "ColossalLoad";
        if (save) {
            ini.SetBoolValue(S, "On", g.on); ini.SetBoolValue(S, "BySize", g.bySize); ini.SetDoubleValue(S, "Chance", g.chance);
            ini.SetValue(S, "Men", g.men.Save().c_str()); ini.SetBoolValue(S, "Creatures", g.creatures);
            ini.Delete("ColossalLoadTypes", nullptr);
            for (auto& [k, l] : g.types) ini.SetValue("ColossalLoadTypes", k.c_str(), l.Save().c_str());
            ini.Delete("ColossalLoadGroups", nullptr);
            for (auto& [k, e] : g.groups) ini.SetValue("ColossalLoadGroups", k.c_str(), ((e.first ? "1|" : "0|") + e.second.Save()).c_str());
            return;
        }
        g.on = ini.GetBoolValue(S, "On", g.on); g.bySize = ini.GetBoolValue(S, "BySize", g.bySize);
        g.chance = std::clamp(static_cast<float>(ini.GetDoubleValue(S, "Chance", g.chance)), 0.0f, 1.0f);
        g.men = Load::Parse(ini.GetValue(S, "Men", ""), g.men); g.creatures = ini.GetBoolValue(S, "Creatures", g.creatures);
        CSimpleIniA::TNamesDepend keys;
        ini.GetAllKeys("ColossalLoadTypes", keys);
        for (auto& k : keys) g.types[k.pItem] = Load::Parse(ini.GetValue("ColossalLoadTypes", k.pItem, ""), TypeLoad(k.pItem));
        keys.clear();
        ini.GetAllKeys("ColossalLoadGroups", keys);
        for (auto& k : keys) {
            const std::string v = ini.GetValue("ColossalLoadGroups", k.pItem, "");
            if (v.size() < 2) continue;
            g.groups[k.pItem] = {v[0] == '1', Load::Parse(v.c_str() + 2, CreatureDefault())};
        }
    }
}

// ---- for OStrands 0.8.0.4+ (it looks this up in AnatomicaMenu.dll)
extern "C" {
    // out[12]: 0 weight (0 = normal load, 1 = full Colossal Load), 1 amount, 2 force, 3 spurts, 4 strand thickness, 5 thick and sticky,
    // 6 stretch (all x the normal load), 7 own colour (1/0), 8-10 colour 0-1. false = Colossal Load is off in Anatomica (OStrands decides)
    __declspec(dllexport) bool Anatomica_LoadKind(RE::Actor* giver, RE::Actor* receiver, float* out) {
        if (!out) return false;
        float w = 0.0f;
        Colossal::Load l;
        if (!Colossal::For(giver, receiver, w, l)) return false;
        out[0] = w; out[1] = l.amount; out[2] = l.force; out[3] = l.pulses; out[4] = l.thick; out[5] = l.sticky; out[6] = l.stretch;
        out[7] = (w > 0.0f && l.ownColour) ? 1.0f : 0.0f;
        for (int i = 0; i < 3; ++i) out[8 + i] = l.rgb[i];
        if (w <= 0.0f) for (int i = 1; i <= 6; ++i) out[i] = 1.0f;
        return true;
    }
}

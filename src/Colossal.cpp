#include "Colossal.h"
#include "Lang.h"
#include "Mnc.h"
#include "Size.h"
#include "Sounds.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <atomic>
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
            bool ww = true;                // 2.6i: men transformed into werewolves / werebears (no More Nasty Critters needed)
            bool wwObf = true;             // 2.6j: with OStim Beast Form, only while its "Big loads" MCM option is on
            Load wwLoad = L(4.5f, 1.7f, 2.2f, 1.9f, 1.5f, 1.6f);
            float wwRing = 2.0f;           // x how much wells out round his rim while a werewolf fills him
            float wwMoon = 0.5f;           // 2.6j: on a full-moon night a werewolf's load is this much bigger (half for a nearly full moon)
            bool wwHowl = true;            // 2.6j: a werewolf howls when he finishes
            float wwHowlChance = 0.6f;
            float wwHowlVolume = 1.0f;
            bool vl = true;                // 2.6j: Vampire Lords get their own big load (Beast Form leaves them normal)
            bool vlObf = false;            //   ... only while OStim Beast Form's "Big loads" is on
            Load vlLoad = L(3.5f, 1.5f, 1.8f, 1.6f, 1.4f, 1.4f);
            float vlNight = 0.3f;          // 2.6j: at night a Vampire Lord's load is this much bigger...
            float vlDay = -0.2f;           //   ... and by day this much (below 0 = smaller)
            float vlHunger = 1.0f;         //   ... how much his hunger changes it: 100% = starving gives twice the load, 0 = none, below 0 = smaller
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
        // 2.6j: OStim Beast Form's "Big loads" (its OCumBeastPie script, read on the main thread every 10 s)
        std::atomic<int> g_obfBig{-1};   // -1 Beast Form not found / not read, 0 off, 1 on
        std::atomic<double> g_obfAt{-100.0};
        std::atomic<bool> g_obfAsked{false};
        double Clock() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
        void ReadObf() {
            int r = -1;
            auto* dh = RE::TESDataHandler::GetSingleton();
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            RE::TESQuest* q = nullptr;
            if (dh && vm)
                for (const char* p : {"OStim_Beast_Form_Merged.esp", "OStim_Beast_Form.esp"})
                    if (dh->LookupModByName(p) && (q = dh->LookupForm<RE::TESQuest>(0x822, p))) break;
            if (q) {
                auto* pol = vm->GetObjectHandlePolicy();
                const auto h = pol ? pol->GetHandleForObject(RE::TESQuest::FORMTYPE, q) : 0;
                RE::BSTSmartPointer<RE::BSScript::Object> obj;
                if (h && vm->FindBoundObject(h, "OCumBeastPie", obj) && obj) {
                    const auto* huge = obj->GetProperty("HugeLoad");
                    const auto* on = obj->GetProperty("BeastLoadEnabled");
                    r = (huge && huge->GetBool() && (!on || on->GetBool())) ? 1 : 0;
                }
            }
            if (g_obfBig.exchange(r) != r && r >= 0) logger::info("Anatomica: OStim Beast Form's Big loads are {}", r == 1 ? "on" : "off");
            g_obfAt = Clock();
            g_obfAsked = false;
        }
        int ObfBig() {   // any thread
            if (Clock() - g_obfAt > 10.0 && !g_obfAsked.exchange(true)) {
                if (auto* t = SKSE::GetTaskInterface()) t->AddTask([]() { ReadObf(); });
                else g_obfAsked = false;
            }
            return g_obfBig;
        }
        bool Were(RE::Actor* a) {   // 2.6i: the vanilla werewolf / werebear beast races, or any race named like one
            auto* race = a ? a->GetRace() : nullptr;
            if (!race) return false;
            static RE::TESRace* ww = nullptr; static RE::TESRace* wb = nullptr; static bool looked = false;
            if (!looked) {
                looked = true;
                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                    ww = dh->LookupForm<RE::TESRace>(0x0CDD84, "Skyrim.esm");
                    wb = dh->LookupForm<RE::TESRace>(0x01E17B, "Dragonborn.esm");
                }
            }
            if (race == ww || (wb && race == wb)) return true;
            std::string n = race->GetFullName() ? race->GetFullName() : "";
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return n.find("werewolf") != std::string::npos || n.find("werebear") != std::string::npos;
        }

        // 2.6j: the player's vampire hunger (PlayerVampireQuest's script, read on the main thread every 10 s)
        std::atomic<float> g_plHunger{-1.0f};
        std::atomic<double> g_plAt{-100.0};
        std::atomic<bool> g_plAsked{false};
        std::unordered_map<RE::FormID, float> g_fedDay;   // NPC Vampire Lords: the game day each last finished (under g_mx)
        float Today() { auto* c = RE::Calendar::GetSingleton(); return c ? c->GetDaysPassed() : 0.0f; }
        void ReadPlayerHunger() {
            float h = -1.0f;
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            auto* q = RE::TESForm::LookupByID<RE::TESQuest>(0x000EAFD5);   // PlayerVampireQuest
            if (vm && q) {
                auto* pol = vm->GetObjectHandlePolicy();
                const auto hd = pol ? pol->GetHandleForObject(RE::TESQuest::FORMTYPE, q) : 0;
                RE::BSTSmartPointer<RE::BSScript::Object> obj;
                if (hd && vm->FindBoundObject(hd, "PlayerVampireQuestScript", obj) && obj) {
                    if (const auto* last = obj->GetProperty("LastFeedTime"); last && last->IsFloat() && last->GetFloat() > 0.0f)
                        h = std::clamp((Today() - last->GetFloat()) / 3.0f, 0.0f, 1.0f);   // starving after three days
                    else if (const auto* st = obj->GetProperty("VampireStatus"); st && st->IsInt() && st->GetSInt() > 0)
                        h = std::clamp((static_cast<float>(st->GetSInt()) - 1.0f) / 3.0f, 0.0f, 1.0f);   // stage 1 fed .. 4 starving
                }
            }
            g_plHunger = h;
            g_plAt = Clock();
            g_plAsked = false;
        }
        float PlayerHunger() {   // any thread
            if (Clock() - g_plAt > 10.0 && !g_plAsked.exchange(true)) {
                if (auto* t = SKSE::GetTaskInterface()) t->AddTask([]() { ReadPlayerHunger(); });
                else g_plAsked = false;
            }
            return g_plHunger;
        }

        bool Night() { auto* c = RE::Calendar::GetSingleton(); const float h = c ? c->GetHour() : 12.0f; return h >= 20.0f || h < 5.0f; }
        // 2.6j: Skyrim's moons go through eight phases (the climate's phase length in days, 3 in vanilla); 1 full, 0.5 nearly full
        float MoonNow() {
            auto* sky = RE::Sky::GetSingleton();
            auto* cal = RE::Calendar::GetSingleton();
            if (!sky || !cal || !Night()) return 0.0f;
            int len = sky->currentClimate ? sky->currentClimate->timing.GetPhaseLength() : 3;
            if (len <= 0) len = 3;
            const int phase = static_cast<int>(cal->GetDaysPassed() / static_cast<float>(len)) % 8;   // 0 full .. 4 new .. 7 waxing gibbous
            return phase == 0 ? 1.0f : (phase == 1 || phase == 7) ? 0.5f : 0.0f;
        }
        Load Scaled(Load l, float f) {
            f = std::max(0.25f, f);
            l.amount = std::clamp(l.amount * f, 0.2f, 8.0f);
            l.pulses = std::clamp(l.pulses * std::sqrt(f), 0.3f, 4.0f);
            return l;
        }

        bool Lord(RE::Actor* a) {   // 2.6j: the Dawnguard Vampire Lord race, or any race named like one
            auto* race = a ? a->GetRace() : nullptr;
            if (!race) return false;
            static RE::TESRace* vl = nullptr; static bool looked = false;
            if (!looked) { looked = true; if (auto* dh = RE::TESDataHandler::GetSingleton()) vl = dh->LookupForm<RE::TESRace>(0x00283A, "Dawnguard.esm"); }
            if (vl && race == vl) return true;
            std::string n = race->GetFullName() ? race->GetFullName() : "";
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return n.find("vampire lord") != std::string::npos;
        }

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
            sl("Thickness (strands and globs)", &l.thick, 0.3f, 3.0f);
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
        const bool were = Were(giver);
        const bool lord = !were && Lord(giver);
        const int obf = (were || lord) ? ObfBig() : -1;
        const float plHunger = lord && giver->IsPlayerRef() ? PlayerHunger() : -1.0f;
        std::lock_guard lk(g_mx);
        if (!g.on) return false;
        if (were && g.ww && !(g.wwObf && obf == 0)) {   // (2.6j: Beast Form installed with its Big loads off: a normal load)   // 2.6i: a man turned werewolf - his own huge load, More Nasty Critters or not
            weight = 1.0f;
            load = Scaled(g.wwLoad, 1.0f + g.wwMoon * MoonNow());   // 2.6j: the full moon
            return true;
        }
        if (lord && g.vl && !(g.vlObf && obf == 0)) {   // 2.6j: a Vampire Lord's own big load
            weight = 1.0f;
            load = g.vlLoad;
            float h = plHunger;   // his hunger
            if (h < 0.0f) {
                auto it = g_fedDay.find(giver->GetFormID());
                h = it == g_fedDay.end() ? 0.5f : std::clamp((Today() - it->second) / 2.0f, 0.0f, 1.0f);
            }
            const float f = (1.0f + g.vlHunger * h) * (1.0f + (Night() ? g.vlNight : g.vlDay));   // his hunger, and the night
            if (f != 1.0f) load = Scaled(load, f);
            return true;
        }
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

    bool IsWerewolf(RE::Actor* a) { return Were(a); }
    int BeastForm(RE::Actor* a) {   // 2.6j
        if (Were(a)) {
            auto* race = a->GetRace();
            std::string n = race && race->GetFullName() ? race->GetFullName() : "";
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            static RE::TESRace* wb = nullptr; static bool looked = false;
            if (!looked) { looked = true; if (auto* dh = RE::TESDataHandler::GetSingleton()) wb = dh->LookupForm<RE::TESRace>(0x01E17B, "Dragonborn.esm"); }
            return (wb && race == wb) || n.find("werebear") != std::string::npos ? 2 : 1;
        }
        return Lord(a) ? 3 : 0;
    }
    float Hunger(RE::Actor* a) {
        if (!a || !Lord(a)) return 0.0f;
        if (a->IsPlayerRef()) { const float h = PlayerHunger(); if (h >= 0.0f) return h; }
        std::lock_guard lk(g_mx);
        auto it = g_fedDay.find(a->GetFormID());
        return it == g_fedDay.end() ? 0.5f : std::clamp((Today() - it->second) / 2.0f, 0.0f, 1.0f);
    }
    float Moon() { return MoonNow(); }
    void Climax(RE::Actor* giver) {
        if (!giver || !Were(giver) || BeastForm(giver) != 1) return;   // a werewolf (a werebear does not howl)
        float chance, vol;
        { std::lock_guard lk(g_mx); if (!g.wwHowl) return; chance = g.wwHowlChance; vol = g.wwHowlVolume; }
        if (std::uniform_real_distribution<float>(0.0f, 1.0f)(Rng()) >= chance) return;
        const RE::ActorHandle h = giver->GetHandle();
        if (auto* t = SKSE::GetTaskInterface()) t->AddTask([h, vol]() {
            auto a = h.get();
            if (!a) return;
            if (Sounds::Count("Howl") > 0) { Sounds::Play("Howl", a.get(), vol); return; }   // your own (Sound\Fx\Anatomica\Howl)
            if (!Sounds::PlayFile("Sound\\Fx\\NPC\\Werewolf\\npc_werewolf_shout_01.wav", a.get(), vol))   // else the game's werewolf howl
                Sounds::PlayFile("Sound\\Fx\\NPC\\Werewolf\\npc_werewolf_shout_01.xwm", a.get(), vol);
        });
    }
    void Finished(RE::Actor* giver) {
        if (!giver || giver->IsPlayerRef() || !Lord(giver)) return;
        std::lock_guard lk(g_mx);
        g_fedDay[giver->GetFormID()] = Today();
    }
    float RingBoost(RE::Actor* giver) {
        if (!giver || !Were(giver)) return 1.0f;
        std::lock_guard lk(g_mx);
        return g.on && g.ww ? g.wwRing : 1.0f;
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
        if (Tr::TreeNode("Werewolves (transformed men)###clww")) {   // 2.6i
            ch |= Tr::Checkbox("A man turned werewolf or werebear finishes with his own huge load", &g.ww);
            Help("The beast forms of men (Vampire Lords have their own, below). Works without More Nasty Critters.");
            if (g.ww) {
                ch |= Tr::Checkbox("Follow OStim Beast Form's Big loads", &g.wwObf);
                Help("With OStim Beast Form installed: only while its \"Big loads\" MCM option is on (Vampire Lords have their own setting below).");
                if (ObfBig() >= 0) Tr::TextDisabled(ObfBig() == 1 ? "OStim Beast Form: Big loads on" : "OStim Beast Form: Big loads off");
                ch |= Edit("ww", g.wwLoad);
                ch |= Tr::SliderFloat("Wells out round his rim", &g.wwRing, 1.0f, 4.0f, "x%.1f");
                float mp = g.wwMoon * 100.0f;   // 2.6j
                if (Tr::SliderFloat("Full moon", &mp, 0.0f, 200.0f, "+%.0f%%")) { g.wwMoon = mp / 100.0f; ch = true; }
                Help("On a full-moon night a werewolf's load is this much bigger (half as much when the moon is nearly full). Skyrim's moons go "
                     "through eight phases, a few days each - the same ones you see in the sky (and that Moonlight Tales uses).");
                if (const float mn = MoonNow(); mn > 0.0f) Tr::TextDisabled(mn >= 1.0f ? "Tonight the moon is full." : "Tonight the moon is nearly full.");
                ch |= Tr::Checkbox("Howls when he finishes", &g.wwHowl);
                Help("A werewolf throws his head back and howls as he finishes. Your own sounds: put WAV files in Data\\Sound\\Fx\\Anatomica\\Howl; "
                     "without them it is the game's werewolf howl (or the one a werewolf sound mod replaced it with).");
                if (g.wwHowl) {
                    float hc = g.wwHowlChance * 100.0f;
                    if (Tr::SliderFloat("Howl chance", &hc, 0.0f, 100.0f, "%.0f%%")) { g.wwHowlChance = hc / 100.0f; ch = true; }
                    ch |= Tr::SliderFloat("Howl volume", &g.wwHowlVolume, 0.1f, 2.0f, "x%.1f");
                }
                Help("How much more of it wells straight back out round the bottom's rim while a werewolf fills him (Integrations page, OStrands: Wells out round his rim).");
            }
            ImGuiMCP::TreePop();
        }
        if (Tr::TreeNode("Vampire Lords (transformed men)###clvl")) {   // 2.6j
            ch |= Tr::Checkbox("A man turned Vampire Lord finishes with his own big load", &g.vl);
            Help("OStim Beast Form gives Vampire Lords a normal load; this gives them one of their own. Works without More Nasty Critters.");
            if (g.vl) {
                ch |= Tr::Checkbox("Only while OStim Beast Form's Big loads is on##vlobf", &g.vlObf);
                Help("Off: Vampire Lords always get it. On: only while Beast Form's \"Big loads\" MCM option is on.");
                ch |= Edit("vl", g.vlLoad);
                float np = g.vlNight * 100.0f, dp = g.vlDay * 100.0f;   // 2.6j
                if (Tr::SliderFloat("At night", &np, -100.0f, 200.0f, "%+.0f%%")) { g.vlNight = np / 100.0f; ch = true; }
                if (Tr::SliderFloat("By day", &dp, -100.0f, 200.0f, "%+.0f%%")) { g.vlDay = dp / 100.0f; ch = true; }
                Help("A Vampire Lord is strongest at night (8 pm to 5 am): his load changes by this much. It stacks with his hunger.");
                float hp = g.vlHunger * 100.0f;
                if (Tr::SliderFloat("Hunger", &hp, -100.0f, 300.0f, "%.0f%%")) { g.vlHunger = hp / 100.0f; ch = true; }
                Help("How much his hunger for blood changes his load. 100% = starving, he gives twice it; 0% = hunger does nothing; "
                     "below 0% a hungry Vampire Lord gives less instead. Your own hunger is your vampire stage (feeding resets it); "
                     "another Vampire Lord's grows over two game days since he last finished.");
                if (auto* pl = RE::PlayerCharacter::GetSingleton(); pl && Lord(pl)) {
                    lk.unlock();
                    const float h = Hunger(pl);
                    lk.lock();
                    Tr::TextDisabled("%s", Lang::Fmt("Your hunger now: {:.0f}%", h * 100.0f).c_str());
                }
            }
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
            ini.SetBoolValue(S, "Werewolves", g.ww); ini.SetBoolValue(S, "WerewolvesFollowBeastForm", g.wwObf); ini.SetValue(S, "Werewolf", g.wwLoad.Save().c_str()); ini.SetDoubleValue(S, "WerewolfRing", g.wwRing);
            ini.SetBoolValue(S, "VampireLords", g.vl); ini.SetBoolValue(S, "VampireLordsFollowBeastForm", g.vlObf); ini.SetValue(S, "VampireLord", g.vlLoad.Save().c_str()); ini.SetDoubleValue(S, "VampireLordHunger", g.vlHunger);
            ini.SetDoubleValue(S, "VampireLordNight", g.vlNight); ini.SetDoubleValue(S, "VampireLordDay", g.vlDay);
            ini.SetDoubleValue(S, "WerewolfFullMoon", g.wwMoon); ini.SetBoolValue(S, "WerewolfHowl", g.wwHowl);
            ini.SetDoubleValue(S, "WerewolfHowlChance", g.wwHowlChance); ini.SetDoubleValue(S, "WerewolfHowlVolume", g.wwHowlVolume);
            ini.Delete("ColossalLoadTypes", nullptr);
            for (auto& [k, l] : g.types) ini.SetValue("ColossalLoadTypes", k.c_str(), l.Save().c_str());
            ini.Delete("ColossalLoadGroups", nullptr);
            for (auto& [k, e] : g.groups) ini.SetValue("ColossalLoadGroups", k.c_str(), ((e.first ? "1|" : "0|") + e.second.Save()).c_str());
            return;
        }
        g.on = ini.GetBoolValue(S, "On", g.on); g.bySize = ini.GetBoolValue(S, "BySize", g.bySize);
        g.chance = std::clamp(static_cast<float>(ini.GetDoubleValue(S, "Chance", g.chance)), 0.0f, 1.0f);
        g.men = Load::Parse(ini.GetValue(S, "Men", ""), g.men); g.creatures = ini.GetBoolValue(S, "Creatures", g.creatures);
        g.ww = ini.GetBoolValue(S, "Werewolves", g.ww); g.wwObf = ini.GetBoolValue(S, "WerewolvesFollowBeastForm", g.wwObf); g.wwLoad = Load::Parse(ini.GetValue(S, "Werewolf", ""), g.wwLoad);
        g.wwRing = std::clamp(static_cast<float>(ini.GetDoubleValue(S, "WerewolfRing", g.wwRing)), 1.0f, 4.0f);
        g.vl = ini.GetBoolValue(S, "VampireLords", g.vl); g.vlObf = ini.GetBoolValue(S, "VampireLordsFollowBeastForm", g.vlObf); g.vlLoad = Load::Parse(ini.GetValue(S, "VampireLord", ""), g.vlLoad);
        g.vlHunger = std::clamp(static_cast<float>(ini.GetDoubleValue(S, "VampireLordHunger", g.vlHunger)), -1.0f, 3.0f);
        auto rd = [&](const char* k, float& v, float lo, float hi) { v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi); };
        rd("VampireLordNight", g.vlNight, -1.0f, 2.0f); rd("VampireLordDay", g.vlDay, -1.0f, 2.0f); rd("WerewolfFullMoon", g.wwMoon, 0.0f, 2.0f);
        g.wwHowl = ini.GetBoolValue(S, "WerewolfHowl", g.wwHowl); rd("WerewolfHowlChance", g.wwHowlChance, 0.0f, 1.0f); rd("WerewolfHowlVolume", g.wwHowlVolume, 0.1f, 2.0f);
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

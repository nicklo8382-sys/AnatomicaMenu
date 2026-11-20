#include "Training.h"
#include "Engine.h"
#include "Lang.h"
#include "OStrandsLink.h"
#include "Size.h"
#include "Virgin.h"
#include "Colour.h"
#include "Colossal.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Training {
    namespace {
        constexpr std::uint32_t kRec = 'ANTN';
        struct Settings {
            bool on = true;
            float gain = 1.0f;          // x how fast he trains
            float decay = 0.15f;        // levels lost per game day left alone
            float decayAfter = 1.0f;    // game days before it starts to fade
            bool plugs = true;          // wearing a plug trains him
            float plugRate = 0.3f;      // levels per game hour worn... (scaled below)
            float cap = 5.0f;           // the most he can be trained to (5 = beyond XL)
            // belly inflation
            bool inflate = true;
            bool lasting = true;        // on: kept with the save and shrinks over days; off: grows faster per load, this filling only
            float per = 0.05f;          // lasting: belly added each time he is filled (x the full belly)
            float maxLasting = 0.5f;    // lasting: at most
            float shrink = 0.1f;        // lasting: shrinks this much a game day
            bool colossalDouble = true;
            float fastPct = 0.05f;      // off: each load makes it grow this much faster (stacks)
            int fastMax = 10;           // ... up to this many loads
            // 2.6j: Vampire Lords (OStim Beast Form)
            bool lord = true;           // being taken by Vampire Lords is counted on its own
            bool thrall = true;         // bred by Vampire Lords again and again: he feels like their thrall
            float thrallAt = 3.0f;      //   ... after this many breedings
            float thrallFade = 1.0f;    //   ... one fades a game day (after a day left alone)
            bool cold = true;           // a Vampire Lord's seed is cold: he holds it longer
            float coldHours = 6.0f;     //   ... for this many game hours after
            float coldHold = 0.5f;      //   ... it runs out of him this fast (x)
            bool bond = true;           // a thrall's skin glows faintly red for a while after a Vampire Lord breeds him
            float bondHours = 12.0f;
            float bondStrength = 0.6f;
            // werewolves
            bool scent = true;          // a werewolf who breeds him leaves his scent on him (SkyrimNet)
            float scentDays = 2.0f;
        } g;
        struct Man { float level = -1.0f; float lastDay = -1.0f; float infl = 0.0f; float inflDay = -1.0f; int stacks = 0;
                     float lord = 0.0f; float lordDay = -1.0f; float thrall = 0.0f; float thrallDay = -1.0f; float coldUntil = -1.0f;
                     float bondUntil = -1.0f; RE::FormID scentBy = 0; float scentUntil = -1.0f; };   // 2.6j
        std::mutex g_mx;
        std::unordered_map<RE::FormID, Man> g_men;
        std::atomic<bool> g_queued{false};
        float Today() { auto* c = RE::Calendar::GetSingleton(); return c ? c->GetDaysPassed() : 0.0f; }
        double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
        void Help(const char* t) {
            ImGuiMCP::SameLine();
            Tr::TextDisabled("(?)");
            if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::PushTextWrapPos(420.0f); Tr::TextUnformatted(t); ImGuiMCP::PopTextWrapPos(); ImGuiMCP::EndTooltip(); }
        }

        // once a second (main thread): who is being trained right now
        void Tick(float dt) {
            const float today = Today();
            std::vector<RE::FormID> ids = Engine::DuringIds();
            struct Seen { RE::FormID id; Virgin::Info vi; int cat; bool deep; bool plug; bool lord; };
            std::vector<Seen> now;
            for (RE::FormID id : ids) {
                const Virgin::Info vi = Virgin::Get(id);
                const Engine::Bulge b = Engine::BulgeOf(id);
                const int cat = b.partner ? Size::CatOf(b.partner) : Size::kUnknown;
                auto* pa = b.partner ? RE::TESForm::LookupByID<RE::Actor>(b.partner) : nullptr;
                now.push_back({id, vi, cat, b.deep, OStrandsLink::HeldOf(id).plugged, pa && Colossal::BeastForm(pa) == 3});
            }
            std::lock_guard lk(g_mx);
            if (!g.on) return;
            for (auto& s : now) {
                Man& m = g_men[s.id];
                if (m.level < 0.0f) m.level = s.vi.known ? (s.vi.virgin ? 0.0f : std::clamp(s.vi.exp / 5.0f, 0.0f, 2.5f)) : 1.0f;   // where he starts
                const bool taken = s.vi.workedAgo >= 0.0 && s.vi.workedAgo < 3.0;
                if (taken) {
                    const float c = s.cat < 0 ? 2.0f : static_cast<float>(s.cat);
                    const float k = c > m.level ? 1.0f + 0.5f * (c - m.level) : 0.3f;   // a bigger man than he is used to trains him
                    m.level = std::min(g.cap, m.level + dt / 60.0f * 0.05f * g.gain * k * (s.deep ? 1.5f : 1.0f));
                    m.lastDay = today;
                    if (s.lord && g.lord) {   // 2.6j: used to Vampire Lords (0..5, like the training levels)
                        m.lord = std::min(5.0f, m.lord + dt / 60.0f * 0.1f * g.gain);
                        m.lordDay = today;
                    }
                } else if (s.plug && g.plugs) {
                    m.level = std::min(std::min(g.cap, 3.0f), m.level + dt / 60.0f * 0.02f * g.plugRate * g.gain);   // a plug: up to L
                    m.lastDay = today;
                }
            }
            // left alone: it fades; the lasting belly shrinks
            for (auto it = g_men.begin(); it != g_men.end();) {
                Man& m = it->second;
                if (m.inflDay >= 0.0f && m.infl > 0.0f && today > m.inflDay) {
                    m.infl = std::max(0.0f, m.infl - (today - m.inflDay) * g.shrink);
                    m.inflDay = today;
                }
                ++it;
            }
        }
        // fading by game days (checked against the day he was last trained, so time skips count)
        void Fade() {
            const float today = Today();
            std::lock_guard lk(g_mx);
            for (auto& [id, m] : g_men) {
                if (m.lordDay >= 0.0f && m.lord > 0.0f) {   // 2.6j: Vampire Lords - fades as the training does
                    static std::unordered_map<RE::FormID, float> lordFaded;
                    const float idle = today - m.lordDay - g.decayAfter;
                    float& done = lordFaded[id];
                    if (idle <= 0.0f || idle < done) done = std::max(0.0f, idle);
                    else { m.lord = std::max(0.0f, m.lord - (idle - done) * g.decay); done = idle; }
                }
                if (m.thrallDay >= 0.0f && m.thrall > 0.0f) {   // the thrall mark fades a breeding a day, after a day alone
                    static std::unordered_map<RE::FormID, float> thrallFaded;
                    const float idle = today - m.thrallDay - 1.0f;
                    float& done = thrallFaded[id];
                    if (idle <= 0.0f || idle < done) done = std::max(0.0f, idle);
                    else { m.thrall = std::max(0.0f, m.thrall - (idle - done) * g.thrallFade); done = idle; }
                }
                if (m.lastDay < 0.0f || m.level <= 0.0f) continue;
                const float idle = today - m.lastDay - g.decayAfter;
                if (idle <= 0.0f) continue;
                static std::unordered_map<RE::FormID, float> faded;   // the idle days already taken off
                float& done = faded[id];
                if (idle < done) done = 0.0f;
                m.level = std::max(0.0f, m.level - (idle - done) * g.decay);
                done = idle;
            }
        }
        // 2.6j: the blood bond glow on thralls nearby (main thread, every few seconds; set again now and then, as a new 3D loses it)
        std::unordered_map<RE::FormID, std::pair<float, double>> g_glow;   // what each has now, and when it was set
        float BondOf(RE::FormID id);
        void GlowTick() {
            static double next = 0.0;
            const double t = Now();
            if (t < next) return;
            next = t + 3.0;
            std::vector<RE::Actor*> near;
            if (auto* pl = RE::PlayerCharacter::GetSingleton()) near.push_back(pl);
            if (auto* pl = RE::ProcessLists::GetSingleton())
                for (auto& h : pl->highActorHandles) if (auto a = h.get(); a && a->Is3DLoaded()) near.push_back(a.get());
            for (RE::Actor* a : near) {
                const RE::FormID id = a->GetFormID();
                const float want = std::round(BondOf(id) * 20.0f) / 20.0f;
                auto it = g_glow.find(id);
                const bool had = it != g_glow.end();
                if (!had && want <= 0.0f) continue;
                if (had && it->second.first == want && t - it->second.second < 30.0) continue;
                Engine::BondGlow(a, want);
                if (want <= 0.0f) g_glow.erase(id);
                else g_glow[id] = {want, t};
            }
        }
        void Loop() {
            double last = Now();
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto* ui = RE::UI::GetSingleton();
                if (!ui || ui->GameIsPaused()) { last = Now(); continue; }
                const double t = Now();
                const float dt = static_cast<float>(std::clamp(t - last, 0.0, 5.0));
                last = t;
                if (g_queued.exchange(true)) continue;
                if (auto* tq = SKSE::GetTaskInterface()) tq->AddTask([dt]() { g_queued = false; Tick(dt); Fade(); GlowTick(); });
                else g_queued = false;
            }
        }
    }

    void Install() { std::thread(Loop).detach(); }
    void Reset() { std::lock_guard lk(g_mx); for (auto& [id, m] : g_men) m.stacks = 0; }

    void Apply(RE::FormID id, float& open, float& build, float& close, float& tender) {
        std::lock_guard lk(g_mx);
        if (!g.on) return;
        auto it = g_men.find(id);
        if (it == g_men.end() || it->second.level <= 0.0f) return;
        const float k = std::clamp(it->second.level / 4.0f, 0.0f, 1.25f);
        build *= 1.0f - 0.35f * k;    // opens faster
        close *= 1.0f + 0.4f * k;     // the gape closes sooner
        tender *= 1.0f - 0.45f * k;   // less sore
        open *= 1.0f + 0.1f * k;
    }
    float Level(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_men.find(id);
        return (it == g_men.end() || !g.on) ? -1.0f : it->second.level;
    }
    const char* Word(float l) {
        return l < 0.0f ? "" : l < 0.75f ? "a small one" : l < 1.75f ? "an average one" : l < 2.75f ? "a big one" : l < 3.75f ? "a huge one" : l < 4.6f ? "the biggest men" : "anything";
    }
    bool TooBig(RE::FormID id, int cat) {
        if (cat < 0) return false;
        const float l = Level(id);
        return l >= 0.0f && static_cast<float>(cat) - l >= 1.5f;
    }

    void Stuffed(RE::FormID id, float loads, bool colossal) {
        std::lock_guard lk(g_mx);
        if (!g.inflate || loads <= 0.0f) return;
        Man& m = g_men[id];
        if (g.lasting) {
            m.infl = std::min(g.maxLasting, m.infl + g.per * std::min(loads, 2.0f) * (colossal && g.colossalDouble ? 2.0f : 1.0f));
            m.inflDay = Today();
        } else {
            m.stacks = std::min(g.fastMax, m.stacks + (colossal && g.colossalDouble ? 2 : 1));
        }
    }
    float Lasting(RE::FormID id) {
        std::lock_guard lk(g_mx);
        if (!g.inflate || !g.lasting) return 0.0f;
        auto it = g_men.find(id);
        return it == g_men.end() ? 0.0f : it->second.infl;
    }
    float Faster(RE::FormID id) {
        std::lock_guard lk(g_mx);
        if (!g.inflate || g.lasting) return 1.0f;
        auto it = g_men.find(id);
        return it == g_men.end() ? 1.0f : 1.0f + g.fastPct * static_cast<float>(it->second.stacks);
    }
    void Emptied(RE::FormID id) {
        std::lock_guard lk(g_mx);
        if (auto it = g_men.find(id); it != g_men.end()) it->second.stacks = 0;
    }

    void Bred(RE::FormID id, RE::Actor* giver) {   // 2.6j
        const int bf = giver ? Colossal::BeastForm(giver) : 0;
        if (bf == 0) return;
        std::lock_guard lk(g_mx);
        Man& m = g_men[id];
        const float today = Today();
        if (bf != 3) {   // a werewolf / werebear: his scent
            if (g.scent) { m.scentBy = giver->GetFormID(); m.scentUntil = today + g.scentDays; }
            return;
        }
        if (g.thrall && today - m.thrallDay > 0.02f) {   // one breeding per climax (loads a few seconds apart count once)
            m.thrall += 1.0f;
            m.thrallDay = today;
        }
        if (g.cold) m.coldUntil = today + g.coldHours / 24.0f;
        if (g.bond && g.thrall && m.thrall >= g.thrallAt) m.bondUntil = today + g.bondHours / 24.0f;
    }
    namespace { float BondOf(RE::FormID id) { return Bond(id); } }
    float Bond(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_men.find(id);
        if (!g.bond || it == g_men.end()) return 0.0f;
        const float left = (it->second.bondUntil - Today()) * 24.0f;
        return left <= 0.0f ? 0.0f : g.bondStrength * std::clamp(left / std::max(g.bondHours, 0.5f) * 1.5f, 0.15f, 1.0f);   // fades over the last two thirds
    }
    RE::FormID Scent(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_men.find(id);
        return (!g.scent || it == g_men.end() || Today() >= it->second.scentUntil) ? 0 : it->second.scentBy;
    }
    float LordLevel(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_men.find(id);
        return (it == g_men.end() || !g.lord) ? 0.0f : it->second.lord;
    }
    float Thrall(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_men.find(id);
        return (it == g_men.end() || !g.thrall || it->second.thrall < g.thrallAt) ? 0.0f : it->second.thrall;
    }
    bool Cold(RE::FormID id) {
        std::lock_guard lk(g_mx);
        auto it = g_men.find(id);
        return g.cold && it != g_men.end() && Today() < it->second.coldUntil;
    }
    float ColdHold(RE::FormID id) { return Cold(id) ? g.coldHold : 1.0f; }

    void Save(SKSE::SerializationInterface* a) {
        std::lock_guard lk(g_mx);
        if (!a->OpenRecord(kRec, 2)) return;
        const std::uint32_t n = static_cast<std::uint32_t>(g_men.size());
        a->WriteRecordData(&n, sizeof n);
        for (auto& [id, m] : g_men) {
            a->WriteRecordData(&id, sizeof id); a->WriteRecordData(&m.level, sizeof m.level); a->WriteRecordData(&m.lastDay, sizeof m.lastDay);
            a->WriteRecordData(&m.infl, sizeof m.infl); a->WriteRecordData(&m.inflDay, sizeof m.inflDay);
            a->WriteRecordData(&m.lord, sizeof m.lord); a->WriteRecordData(&m.lordDay, sizeof m.lordDay);   // 2: Vampire Lords
            a->WriteRecordData(&m.thrall, sizeof m.thrall); a->WriteRecordData(&m.thrallDay, sizeof m.thrallDay); a->WriteRecordData(&m.coldUntil, sizeof m.coldUntil);
            a->WriteRecordData(&m.bondUntil, sizeof m.bondUntil); a->WriteRecordData(&m.scentBy, sizeof m.scentBy); a->WriteRecordData(&m.scentUntil, sizeof m.scentUntil);
        }
    }
    bool Load(SKSE::SerializationInterface* a, std::uint32_t type, std::uint32_t ver, std::uint32_t) {
        if (type != kRec) return false;
        if (ver != 1 && ver != 2) return true;
        std::lock_guard lk(g_mx);
        g_men.clear();
        std::uint32_t n = 0;
        a->ReadRecordData(&n, sizeof n);
        for (std::uint32_t i = 0; i < n; ++i) {
            RE::FormID id = 0, now = 0; Man m;
            a->ReadRecordData(&id, sizeof id); a->ReadRecordData(&m.level, sizeof m.level); a->ReadRecordData(&m.lastDay, sizeof m.lastDay);
            a->ReadRecordData(&m.infl, sizeof m.infl); a->ReadRecordData(&m.inflDay, sizeof m.inflDay);
            if (ver >= 2) {
                a->ReadRecordData(&m.lord, sizeof m.lord); a->ReadRecordData(&m.lordDay, sizeof m.lordDay);
                a->ReadRecordData(&m.thrall, sizeof m.thrall); a->ReadRecordData(&m.thrallDay, sizeof m.thrallDay); a->ReadRecordData(&m.coldUntil, sizeof m.coldUntil);
                a->ReadRecordData(&m.bondUntil, sizeof m.bondUntil); a->ReadRecordData(&m.scentBy, sizeof m.scentBy); a->ReadRecordData(&m.scentUntil, sizeof m.scentUntil);
                if (RE::FormID sb = 0; m.scentBy && a->ResolveFormID(m.scentBy, sb)) m.scentBy = sb; else m.scentBy = 0;
            }
            if (a->ResolveFormID(id, now)) g_men[now] = m;
        }
        return true;
    }
    void Revert() { std::lock_guard lk(g_mx); g_men.clear(); g_glow.clear(); }

    void Ini(CSimpleIniA& ini, bool save) {
        constexpr const char* S = "Training";
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto F = [&](const char* k, float& v, float lo, float hi) { if (save) ini.SetDoubleValue(S, k, v); else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi); };
        std::lock_guard lk(g_mx);
        B("On", g.on); F("Gain", g.gain, 0.1f, 5.0f); F("Decay", g.decay, 0.0f, 2.0f); F("DecayAfter", g.decayAfter, 0.0f, 30.0f);
        B("Plugs", g.plugs); F("PlugRate", g.plugRate, 0.0f, 3.0f); F("Cap", g.cap, 1.0f, 5.0f);
        B("Inflate", g.inflate); B("LastingBelly", g.lasting); F("PerStuffing", g.per, 0.0f, 0.5f); F("MaxLasting", g.maxLasting, 0.0f, 2.0f);
        F("ShrinkPerDay", g.shrink, 0.0f, 2.0f); B("ColossalDouble", g.colossalDouble); F("FasterPerLoad", g.fastPct, 0.0f, 0.5f);
        if (save) ini.SetLongValue(S, "FasterMaxLoads", g.fastMax); else g.fastMax = std::clamp(static_cast<int>(ini.GetLongValue(S, "FasterMaxLoads", g.fastMax)), 1, 50);
        B("VampireLords", g.lord); B("Thrall", g.thrall); F("ThrallAfter", g.thrallAt, 1.0f, 20.0f); F("ThrallFade", g.thrallFade, 0.0f, 5.0f);
        B("ColdSeed", g.cold); F("ColdHours", g.coldHours, 0.5f, 48.0f); F("ColdHold", g.coldHold, 0.1f, 1.0f);
        B("BloodBond", g.bond); F("BloodBondHours", g.bondHours, 0.5f, 72.0f); F("BloodBondGlow", g.bondStrength, 0.05f, 1.0f);
        B("WerewolfScent", g.scent); F("WerewolfScentDays", g.scentDays, 0.1f, 14.0f);
    }

    void RenderTraining() {
        if (!Tr::CollapsingHeader("Training###trn")) return;
        bool ch = false;
        {
            std::lock_guard lk(g_mx);
            ch |= Tr::Checkbox("Training", &g.on);
            Help("How big he has been conditioned to take comfortably, kept with the save. It grows while he is taken (faster with a man bigger "
                 "than he is used to, and going deep) and fades when he is left alone. A trained man opens faster, gets less sore and his gape "
                 "closes sooner. DDF and SkyrimNet know about it.");
            if (g.on) {
                ch |= Tr::SliderFloat("Trains", &g.gain, 0.1f, 5.0f, "x%.1f as fast");
                ch |= Tr::SliderFloat("Fades", &g.decay, 0.0f, 2.0f, "%.2f sizes a game day");
                ch |= Tr::SliderFloat("Starts fading after", &g.decayAfter, 0.0f, 30.0f, "%.1f game days alone");
                ch |= Tr::Checkbox("A plug trains him too", &g.plugs);
                Help("Wearing a plug slowly trains him, up to a big one.");
                if (g.plugs) ch |= Tr::SliderFloat("Plug training", &g.plugRate, 0.0f, 3.0f, "x%.1f");
                float cp = g.cap;
                if (Tr::SliderFloat("At most", &cp, 1.0f, 5.0f, "%.1f (4 = XL, 5 = beyond)")) { g.cap = cp; ch = true; }
            }
            if (Tr::TreeNode("Vampire Lords (OStim Beast Form)###trnvl")) {   // 2.6j
                ch |= Tr::Checkbox("Count Vampire Lords on their own", &g.lord);
                Help("Being taken by Vampire Lords is kept apart from his other training: he grows used to them (it fades like the training). DDF and SkyrimNet know about it.");
                ch |= Tr::Checkbox("Thrall", &g.thrall);
                Help("Bred by Vampire Lords again and again, he feels bound to them like a thrall - DDF lines and SkyrimNet. It fades when he is left alone.");
                if (g.thrall) {
                    ch |= Tr::SliderFloat("A thrall after", &g.thrallAt, 1.0f, 20.0f, "%.0f breedings");
                    ch |= Tr::SliderFloat("It fades", &g.thrallFade, 0.0f, 5.0f, "%.1f breedings a game day");
                }
                ch |= Tr::Checkbox("Cold seed", &g.cold);
                Help("A Vampire Lord's seed is cold: for a while after, it runs out of him more slowly and his belly feels cold (SkyrimNet).");
                if (g.cold) {
                    ch |= Tr::SliderFloat("Cold for", &g.coldHours, 0.5f, 48.0f, "%.1f game hours");
                    float hp = g.coldHold * 100.0f;
                    if (Tr::SliderFloat("Runs out of him at", &hp, 10.0f, 100.0f, "%.0f%% the speed")) { g.coldHold = hp / 100.0f; ch = true; }
                }
                ch |= Tr::Checkbox("Blood bond", &g.bond);
                Help("Each time a Vampire Lord breeds his thrall, the thrall's skin glows faintly red for a while (a RaceMenu body paint slot; needs a free one). SkyrimNet mentions it.");
                if (g.bond) {
                    ch |= Tr::SliderFloat("Glows for", &g.bondHours, 0.5f, 72.0f, "%.1f game hours");
                    float bs = g.bondStrength * 100.0f;
                    if (Tr::SliderFloat("Glow", &bs, 5.0f, 100.0f, "%.0f%%")) { g.bondStrength = bs / 100.0f; ch = true; }
                }
                ImGuiMCP::TreePop();
            }
            if (Tr::TreeNode("Werewolves (OStim Beast Form)###trnww")) {   // 2.6j
                ch |= Tr::Checkbox("His scent", &g.scent);
                Help("A werewolf who breeds him leaves his scent on him: other werewolves can smell who marked him (SkyrimNet).");
                if (g.scent) ch |= Tr::SliderFloat("It lasts", &g.scentDays, 0.1f, 14.0f, "%.1f game days");
                ImGuiMCP::TreePop();
            }
        }
        if (ch) Colour::SaveNow();
    }
    void RenderInflation() {
        bool ch = false;
        {
            std::lock_guard lk(g_mx);
            ch |= Tr::Checkbox("Inflation", &g.inflate);
            Help("Being filled again and again makes his belly bigger.");
            if (g.inflate) {
                ch |= Tr::Checkbox("Lasting belly", &g.lasting);
                Help("On: each time he is filled his belly stays a little bigger, kept with the save, and slowly goes down over the days.\n"
                     "Off: nothing lasts - instead each load makes his belly grow a little faster for this filling (it stacks), and it starts over once he is empty.");
                if (g.lasting) {
                    float p = g.per * 100.0f;
                    if (Tr::SliderFloat("Each filling adds", &p, 0.0f, 50.0f, "%.0f%% belly")) { g.per = p / 100.0f; ch = true; }
                    float mx = g.maxLasting * 100.0f;
                    if (Tr::SliderFloat("Lasting at most", &mx, 0.0f, 200.0f, "%.0f%% belly")) { g.maxLasting = mx / 100.0f; ch = true; }
                    float sh = g.shrink * 100.0f;
                    if (Tr::SliderFloat("Goes down", &sh, 0.0f, 200.0f, "%.0f%% a game day")) { g.shrink = sh / 100.0f; ch = true; }
                } else {
                    float p = g.fastPct * 100.0f;
                    if (Tr::SliderFloat("Each load grows it faster by", &p, 0.0f, 50.0f, "%.0f%%")) { g.fastPct = p / 100.0f; ch = true; }
                    ch |= Tr::SliderInt("Stacks up to", &g.fastMax, 1, 50, "%d loads");
                }
                ch |= Tr::Checkbox("A Colossal Load counts double", &g.colossalDouble);
            }
        }
        if (ch) Colour::SaveNow();
    }
    std::string Status(RE::Actor* a) {
        if (!a) return {};
        std::lock_guard lk(g_mx);
        auto it = g_men.find(a->GetFormID());
        if (!g.on || it == g_men.end() || it->second.level < 0.0f) return {};
        const Man& m = it->second;
        std::string s = Lang::Fmt("Training: {:.2f} (takes {} comfortably)", m.level, std::string(Lang::T(Word(m.level))));
        if (m.infl > 0.01f) s += Lang::Fmt(", lasting belly {:.0f}%", m.infl * 100.0f);
        if (m.stacks > 0) s += Lang::Fmt(", belly grows {:.0f}% faster", g.fastPct * 100.0f * m.stacks);
        if (m.lord > 0.01f) s += Lang::Fmt(", used to Vampire Lords {:.2f}", m.lord);
        if (m.thrall > 0.01f) s += Lang::Fmt(", bred by Vampire Lords {:.1f}", m.thrall);
        if (m.scentBy && Today() < m.scentUntil) s += Lang::Fmt(", a werewolf's scent on him");
        return s;
    }
}

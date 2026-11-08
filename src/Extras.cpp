#include "Extras.h"
#include "Virgin.h"
#include "Colour.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// See Extras.h. Nothing here runs unless Data/SKSE/Plugins/AnatomicaMenu/Extras.ini exists (the separate "Anatomica
// Extras" mod). The keyword and faction lists live in that file so they can be changed without a new DLL.

namespace {
    constexpr const char* kListFile = "Data/SKSE/Plugins/AnatomicaMenu/Extras.ini";

    struct Settings {
        bool revealing = true;       // revealing armour counts as naked
        bool how = true;             // captive / paid: memories and reactions say so
        bool captiveSore = true;     // a captive is left sorer, for longer
        float captiveTender = 1.4f;
        float captiveTime = 1.5f;
        bool lot = true;             // Slave Auction: SkyrimNet knows what the house is selling
    } g;

    struct FactionRef { RE::TESFaction* f = nullptr; int minRank = 0; std::string label; };
    struct Lists {
        bool loaded = false;
        std::vector<std::string> revealing, captiveKw;
        std::vector<FactionRef> captive, paid, paidPartner, lot;
        int missing = 0;   // listed factions whose mod is not installed
    } L;
    std::mutex g_mx;

    double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    std::string Lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
    std::string Trim(std::string s) {
        const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
    }
    std::vector<std::string> Split(const std::string& s, char sep) {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string x;
        while (std::getline(ss, x, sep)) if (!(x = Trim(x)).empty()) out.push_back(x);
        return out;
    }
    // "Plugin.esp|ID|minRank, ..." -> factions (entries whose plugin is missing are skipped)
    std::vector<FactionRef> Factions(const char* v) {
        std::vector<FactionRef> out;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !v) return out;
        for (auto& e : Split(v, ',')) {
            const auto p = Split(e, '|');
            if (p.size() < 2) continue;
            RE::FormID id = 0;
            try { id = static_cast<RE::FormID>(std::stoul(p[1], nullptr, 16)); } catch (...) { continue; }
            auto* f = dh->LookupForm<RE::TESFaction>(id & 0xFFFFFF, p[0]);
            if (!f) { ++L.missing; continue; }
            int min = 0;
            if (p.size() > 2) try { min = std::stoi(p[2]); } catch (...) {}
            out.push_back({f, min, p[0]});
        }
        return out;
    }
    void LoadLists() {
        if (L.loaded) return;
        CSimpleIniA ini;
        if (ini.LoadFile(kListFile) < 0) return;
        L.loaded = true;
        for (auto& k : Split(ini.GetValue("Lists", "RevealingKeywords", ""), ',')) L.revealing.push_back(Lower(k));
        for (auto& k : Split(ini.GetValue("Lists", "CaptiveKeywords", ""), ',')) L.captiveKw.push_back(Lower(k));
        L.captive = Factions(ini.GetValue("Lists", "CaptiveFactions", ""));
        L.paid = Factions(ini.GetValue("Lists", "PaidFactions", ""));
        L.paidPartner = Factions(ini.GetValue("Lists", "PaidPartnerFactions", ""));
        L.lot = Factions(ini.GetValue("Lists", "LotFactions", ""));
        logger::info("Anatomica extras: {} revealing keywords, {} captive keywords, factions: {} captive / {} paid / {} client / {} auction ({} listed for mods not installed)",
                     L.revealing.size(), L.captiveKw.size(), L.captive.size(), L.paid.size(), L.paidPartner.size(), L.lot.size(), L.missing);
    }

    bool HasKw(RE::TESObjectARMO* armo, const std::vector<std::string>& names) {
        if (!armo || names.empty()) return false;
        for (std::uint32_t i = 0; i < armo->numKeywords; ++i) {
            auto* kw = armo->keywords[i];
            if (!kw || !kw->formEditorID.c_str()) continue;
            const std::string n = Lower(kw->formEditorID.c_str());
            for (auto& w : names) if (n == w) return true;
        }
        return false;
    }
    bool In(RE::Actor* a, const std::vector<FactionRef>& fs) {
        if (!a) return false;
        for (auto& f : fs)
            if (a->IsInFaction(f.f) && a->GetFactionRank(f.f, a->IsPlayerRef()) >= f.minRank) return true;
        return false;
    }
    bool Bound(RE::Actor* a) {   // wearing a restraint (Devious Devices heavy bondage, ...)
        if (!a || L.captiveKw.empty()) return false;
        for (std::uint32_t b = 0; b < 32; ++b)
            if (HasKw(a->GetWornArmor(static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << b)), L.captiveKw)) return true;
        return false;
    }

    struct Cached { Extras::How how = Extras::How::normal; double at = 0.0; RE::FormID partner = 0; };
    std::unordered_map<RE::FormID, Cached> g_how;

    bool Section(const char* title) { return ImGuiMCP::CollapsingHeader(title); }
    void Help(const char* t) {
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::BeginTooltip(); ImGuiMCP::PushTextWrapPos(420.0f); ImGuiMCP::TextUnformatted(t); ImGuiMCP::PopTextWrapPos(); ImGuiMCP::EndTooltip(); }
    }

    std::string SNLot(RE::StaticFunctionTag*, RE::Actor* a) { return Extras::Lot(a); }
}

namespace Extras {
    bool On() {
        static int on = -1;
        if (on < 0) { std::error_code ec; on = std::filesystem::exists(kListFile, ec) ? 1 : 0; }
        return on == 1;
    }

    void Ini(CSimpleIniA& ini, bool save) {
        const char* S = "Extras";
        auto B = [&](const char* k, bool& v) { if (save) ini.SetBoolValue(S, k, v); else v = ini.GetBoolValue(S, k, v); };
        auto F = [&](const char* k, float& v, float lo, float hi) { if (save) ini.SetDoubleValue(S, k, v); else v = std::clamp(static_cast<float>(ini.GetDoubleValue(S, k, v)), lo, hi); };
        B("RevealingArmour", g.revealing); B("HowItHappened", g.how); B("CaptiveSorer", g.captiveSore);
        F("CaptiveSoreness", g.captiveTender, 1.0f, 3.0f); F("CaptiveSoreTime", g.captiveTime, 1.0f, 4.0f); B("SlaveAuctionLot", g.lot);
    }

    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm) {
        if (On()) vm->RegisterFunction("SNLot"sv, "AnatomicaExtras"sv, SNLot);
    }
    void RegisterSN(RE::BSScript::Internal::VirtualMachine* vm) {
        if (!On() || !vm) return;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
        vm->DispatchStaticCall("SkyrimNetApi", "RegisterDecorator",
                               RE::MakeFunctionArguments(RE::BSFixedString("anatomica_lot"), RE::BSFixedString("AnatomicaExtras"), RE::BSFixedString("SNLot")), cb);
    }

    bool Exposed(RE::Actor* a) {
        if (!a) return false;
        auto* body = a->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody);
        if (!On() || !g.revealing) return !body;
        std::lock_guard lk(g_mx);
        LoadLists();
        auto* under = a->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kModPelvisPrimary);   // slot 49: underwear / loincloths
        if (under == body) under = nullptr;
        auto covers = [&](RE::TESObjectARMO* x) { return x && !HasKw(x, L.revealing); };
        return !covers(body) && !covers(under);
    }

    How HowNow(RE::Actor* man, RE::Actor* partner) {
        if (!On() || !man) return How::normal;
        std::lock_guard lk(g_mx);
        LoadLists();
        const RE::FormID pid = partner ? partner->GetFormID() : 0;
        auto& c = g_how[man->GetFormID()];
        if (c.at > 0.0 && Now() - c.at < 5.0 && c.partner == pid) return c.how;
        How h = How::normal;
        if (In(man, L.captive) || In(man, L.lot) || Bound(man)) h = How::captive;
        else if (In(man, L.paid) || In(partner, L.paidPartner)) h = How::paid;
        c = {h, Now(), pid};
        return h;
    }

    Rough RoughFor(RE::Actor* man, RE::Actor* partner) {
        if (!On() || !g.captiveSore || HowNow(man, partner) != How::captive) return {};
        return {g.captiveTender, g.captiveTime};
    }

    std::string Lot(RE::Actor* a) {
        if (!On() || !g.lot || !a) return {};
        const auto* npc = a->GetActorBase();
        if (!npc || npc->GetSex() != RE::SEX::kMale) return {};
        {
            std::lock_guard lk(g_mx);
            LoadLists();
            if (!In(a, L.lot)) return {};
        }
        const Virgin::Info v = Virgin::Get(a->GetFormID());
        if (!v.known) return {};
        const char* nm = a->GetDisplayFullName();
        const std::string N = nm && *nm ? nm : "This lot";
        if (v.virgin && v.lost == 0)
            return std::format("The house has examined {}: he is an untouched anal virgin - no man has ever had him. Buyers pay a premium for that, and the auctioneer makes the most of it.", N);
        if (v.virgin)
            return std::format("The house has examined {}: he has been had before, but his body has tightened back up - he is sold as good as a virgin.", N);
        return std::format("The house has examined {}: he is no virgin ({}; taken {} time{}). The auctioneer sells him as experienced rather than pure.", N, v.level, v.times, v.times == 1 ? "" : "s");
    }

    void __stdcall RenderPage() {
        bool ch = false;
        ImGuiMCP::TextWrapped("Extras from the separate Anatomica Extras mod. The keyword and faction lists are in "
                              "Data\\SKSE\\Plugins\\AnatomicaMenu\\Extras.ini.");
        {
            std::lock_guard lk(g_mx);
            LoadLists();
            ImGuiMCP::TextDisabled("Found: %d revealing keywords, %d restraint keywords; factions %d captive, %d paid, %d client, %d auction (%d listed for mods you do not have).",
                                   static_cast<int>(L.revealing.size()), static_cast<int>(L.captiveKw.size()), static_cast<int>(L.captive.size()),
                                   static_cast<int>(L.paid.size()), static_cast<int>(L.paidPartner.size()), static_cast<int>(L.lot.size()), L.missing);
        }
        if (Section("Revealing armour")) {
            ch |= ImGuiMCP::Checkbox("Armour that leaves his backside bare counts as naked", &g.revealing);
            Help("Thongs, C-strings, loincloths open at the back, harnesses, \"nearly naked\" sets (Advanced Nudity Detection and "
                 "SexLab Aroused keywords). People then see the state he is in, NPCs can inspect him, and the glossy skin after sex shows.");
        }
        if (Section("How it happened")) {
            ch |= ImGuiMCP::Checkbox("Captive or paid: memories and reactions say so", &g.how);
            Help("Taken while a captive (enslaved, jailed, bound, up for auction) or for money (Public Whore clients, "
                 "working for The Ancient Profession): what he and his partner remember, and what is said after his first time, "
                 "reflect it.");
            ch |= ImGuiMCP::Checkbox("A captive is left sorer, for longer", &g.captiveSore);
            if (g.captiveSore) {
                ImGuiMCP::Indent();
                ch |= ImGuiMCP::SliderFloat("Soreness", &g.captiveTender, 1.0f, 3.0f, "x%.2f");
                ch |= ImGuiMCP::SliderFloat("Takes longer to fade", &g.captiveTime, 1.0f, 4.0f, "x%.2f");
                ImGuiMCP::Unindent();
            }
            static std::string shown;
            if (ImGuiMCP::Button("Check the selected man")) {
                RE::Actor* a = nullptr;
                if (auto sel = RE::Console::GetSelectedRef()) a = sel->As<RE::Actor>();
                if (!a) a = RE::PlayerCharacter::GetSingleton();
                const How h = HowNow(a, nullptr);
                const char* nm = a->GetDisplayFullName();
                shown = std::format("{}: {}{}", nm ? nm : "?", h == How::captive ? "captive" : h == How::paid ? "working for money" : "free",
                                    Exposed(a) ? ", backside bare" : ", covered");
            }
            Help("The man you click in the console (~), or you. Paying clients are checked on the partner when it happens.");
            if (!shown.empty()) { ImGuiMCP::SameLine(); ImGuiMCP::TextUnformatted(shown.c_str()); }
        }
        if (Section("SkyrimNet Slave Auction")) {
            ch |= ImGuiMCP::Checkbox("The house knows what it is selling", &g.lot);
            Help("A man up for sale: the auctioneer and buyers are told whether he is an anal virgin, and how experienced he is.");
        }
        if (ch) Colour::SaveNow();
    }
}

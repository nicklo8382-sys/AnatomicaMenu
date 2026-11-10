#include "SoftButt.h"
#include "Colour.h"
#include "SKSEMenuFramework.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace {
    std::atomic<int> g_who{0};        // 0 off, 1 only you, 2 you and your followers, 3 every man
    int g_cbpcWho = -1;               // what CBPCMasterConfig_HIMBO.txt says now (-1 = not read yet)
    std::string g_note;
    constexpr const char* kEsp = "AnatomicaSoftButt.esp";
    constexpr const char* kCbpc = "Data/SKSE/Plugins/CBPCMasterConfig_HIMBO.txt";

    RE::TESObjectARMO* Item() {
        static RE::TESObjectARMO* armo = nullptr;
        static bool tried = false;
        if (!tried) {
            tried = true;
            if (auto* dh = RE::TESDataHandler::GetSingleton()) armo = dh->LookupForm<RE::TESObjectARMO>(0x801, kEsp);
            logger::info("Anatomica soft butt: {}", armo ? "AnatomicaSoftButt.esp found" : "AnatomicaSoftButt.esp not loaded (option not installed)");
        }
        return armo;
    }
    bool Wants(RE::Actor* a, RE::Actor* player, int who) {
        if (who <= 0 || !a) return false;
        const auto* npc = a->GetActorBase();
        if (!npc || npc->GetSex() != RE::SEX::kMale || a->IsDead()) return false;
        if (a == player) return true;
        if (who == 1) return false;
        if (who == 2) return a->IsPlayerTeammate();
        return true;
    }
    int CountOf(RE::Actor* a, RE::TESObjectARMO* armo) {
        auto inv = a->GetInventory([armo](RE::TESBoundObject& o) { return &o == armo; });
        int n = 0;
        for (auto& [obj, data] : inv) n += data.first;
        return n;
    }
    bool Worn(RE::Actor* a, RE::TESObjectARMO* armo) {
        auto changes = a->GetInventoryChanges();
        if (!changes || !changes->entryList) return false;
        for (auto* e : *changes->entryList)
            if (e && e->object == armo && e->IsWorn()) return true;
        return false;
    }
    // CBPC: the men who wear the soft butt are taken off CBPC's male butt jiggle (read when the game starts)
    std::string CbpcCond(int who) {
        switch (who) {
            case 1: return "NOT IsFemale() AND NOT ActorFormId(00000014)";
            case 2: return "NOT IsFemale() AND NOT ActorFormId(00000014) AND NOT IsPlayerTeammate()";
            case 3: return "NOT IsFemale() AND IsFemale()";   // never: no man has CBPC's butt jiggle
            default: return "NOT IsFemale()";
        }
    }
    int ReadCbpc() {
        std::ifstream f(kCbpc);
        if (!f) return -2;   // no Anatomica CBPC option installed
        for (std::string l; std::getline(f, l);) {
            if (l.rfind("NPC L Butt=MaleLButt=", 0) != 0) continue;
            const std::string c = l.substr(std::string("NPC L Butt=MaleLButt=").size());
            for (int w = 3; w >= 0; --w) if (c.rfind(CbpcCond(w), 0) == 0 && (w != 0 || c.find("AND") == std::string::npos)) return w;
        }
        return 0;
    }
    bool WriteCbpc(int who) {
        std::ifstream f(kCbpc);
        if (!f) return false;
        std::vector<std::string> L;
        for (std::string l; std::getline(f, l);) {
            if (l.rfind("NPC L Butt=MaleLButt=", 0) == 0) l = "NPC L Butt=MaleLButt=" + CbpcCond(who);
            else if (l.rfind("NPC R Butt=MaleRButt=", 0) == 0) l = "NPC R Butt=MaleRButt=" + CbpcCond(who);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            L.push_back(l);
        }
        f.close();
        std::ofstream o(kCbpc, std::ios::trunc);
        if (!o) return false;
        for (auto& l : L) o << l << "\n";
        return static_cast<bool>(o);
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

namespace SoftButt {
    void Ini(CSimpleIniA& ini, bool save) {
        if (save) ini.SetLongValue("SoftButt", "Who", g_who.load());
        else g_who = std::clamp(static_cast<int>(ini.GetLongValue("SoftButt", "Who", g_who.load())), 0, 3);
    }

    void Tick(RE::Actor* player, const std::vector<RE::Actor*>& men) {
        auto* armo = Item();
        if (!armo) return;
        const int who = g_who.load();
        auto* em = RE::ActorEquipManager::GetSingleton();
        if (!em) return;
        std::vector<RE::Actor*> all{player};
        for (auto* a : men) if (a && a != player) all.push_back(a);
        for (auto* a : all) {
            if (!a || !a->Is3DLoaded()) continue;
            const bool want = Wants(a, player, who);
            const int have = CountOf(a, armo);
            if (want) {
                if (have == 0) a->AddObjectToContainer(armo, nullptr, 1, nullptr);
                if (!Worn(a, armo)) em->EquipObject(a, armo, nullptr, 1, nullptr, true, true, false, true);
            } else if (have > 0) {
                if (Worn(a, armo)) em->UnequipObject(a, armo, nullptr, 1, nullptr, true, true, false, true);
                a->RemoveItem(armo, have, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            }
        }
    }

    void RenderSection() {
        if (!ImGuiMCP::CollapsingHeader("Soft butt (SMP physics)###softbutt")) return;
        const bool has = Item() != nullptr;
        if (!has) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.4f, 1.0f},
                                  "Not installed: needs the installer's \"Soft butt (SMP)\" option (AnatomicaSoftButt.esp) and FSMP.");
            ImGuiMCP::BeginDisabled();
        }
        ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.75f, 0.3f, 1.0f},
                              "Warning: heavy on performance. Every man who gets it runs his own SMP physics - keep it to you, or "
                              "you and your followers, unless your PC has power to spare.");
        ImGuiMCP::TextWrapped("SMP takes over his butt from CBPC: a softer, heavier jiggle that is pushed by hands and partners' "
                              "bodies. Off by default.");
        static const char* who[] = {"Off", "Only you", "You and your followers", "Every man (heaviest)"};
        int w = g_who.load();
        if (ImGuiMCP::Combo("Who gets the soft butt", &w, who, 4)) {
            g_who = w;
            Colour::SaveNow();
            if (WriteCbpc(w)) {
                g_cbpcWho = w;
                g_note = "Saved. Restart the game: CBPC reads who it should leave alone only when the game starts (until then CBPC "
                         "and SMP both move those men's butts).";
            } else {
                g_note = "The soft butt is put on at once, but CBPCMasterConfig_HIMBO.txt (Anatomica's CBPC option) was not found - "
                         "take those men off CBPC's butt jiggle yourself, or the two will fight.";
            }
        }
        Help("Only you / you and your followers / every man nearby. The men chosen wear an invisible item (slot 61) that carries "
             "the physics; it is taken off again when they no longer count. They are also taken off CBPC's butt jiggle, which "
             "needs a game restart to take effect.");
        if (g_cbpcWho == -1) g_cbpcWho = ReadCbpc();
        if (g_cbpcWho >= 0 && g_cbpcWho != w)
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.75f, 0.3f, 1.0f}, "Restart the game to finish the change (CBPC).");
        if (!g_note.empty()) ImGuiMCP::TextWrapped("%s", g_note.c_str());
        if (!has) ImGuiMCP::EndDisabled();
    }
}

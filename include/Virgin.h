#pragma once
#include <SimpleIni.h>
#include <vector>

// Anal virginity and experience for Anatomica's men. Each man is a virgin or not (rolled once, or asked from
// OVirginity when it is installed - and told back to it), and gains experience every time he is taken. Virginity can
// come back (after a set number of days, or "smart": as he rests and tightens up again), separately for you, your
// followers and everyone else. Experience feeds the rest of Anatomica: how tight he is, how his rim looks, how strongly
// he reacts the first time, how much he clenches, and how much a regular partner loosens him.
namespace Virgin {
    void Ini(CSimpleIniA& ini, bool save);            // [Virginity] in AnatomicaMenu.ini
    // co-save (Drip owns the callbacks and passes these on)
    void Save(SKSE::SerializationInterface* a);
    bool Load(SKSE::SerializationInterface* a, std::uint32_t type, std::uint32_t ver, std::uint32_t len);   // true = it was ours
    void Revert();

    void Watch(RE::Actor* a);                         // a man nearby (Engine): find out early whether he is a virgin
    void OnWorked(RE::Actor* a, RE::Actor* partner);  // he is being penetrated right now (every update while it lasts)

    // how experience changes the rest of Anatomica (all neutral when the feature or his group is off)
    struct Mods {
        float open = 1.0f;       // how far he gapes (x)
        float build = 1.0f;      // time it takes him to open up (x)
        float close = 1.0f;      // how fast he closes again (x)
        float clench = 1.0f;     // time between clenches (x, lower = more often)
        float looseAdd = 1.0f;   // lasting looseness a round adds (x)
        float looseCap = 1.0f;   // the most lasting looseness he can have (x)
        float tender = 1.0f;     // reddening strength (x)
        float tenderTime = 1.0f; // how long it takes to fade (x)
        float swell = 0.0f;      // extra puffiness of the rim (0-1, added)
        float rim = 0.0f;        // rim colour: -1 paler / tighter pucker (virgin) .. +1 darker, softer (well used)
    };
    Mods For(RE::FormID id);

    // what is known about him, for SkyrimNet and the other integrations (Bridge.cpp)
    struct Info {
        bool known = false;           // covered by the Virginity page and rolled / asked
        bool virgin = false;
        float exp = 0.0f;
        const char* level = "";       // virgin / inexperienced / experienced / well used
        std::uint32_t times = 0, lost = 0, back = 0;
        std::vector<std::pair<RE::FormID, std::uint16_t>> partners;
        RE::FormID firstBy = 0, firstEverBy = 0;
        float daysSince = -1.0f;      // game days since he was last taken (-1 = never)
        float daysSinceFirst = -1.0f; // since his latest first time
        double workedAgo = -1.0;      // real seconds since he was last being penetrated (-1 = not this session)
        int mode = 0;                 // 0 one and done, 1 set time, 2 smart
        float backIn = -1.0f;         // game days until it comes back (-1 = never / is a virgin)
    };
    Info Get(RE::FormID id);
    void RestFor(RE::Actor* a, float hours, const char* why);   // healing / bathing: as if he had rested this long
    struct Had { RE::FormID man = 0; std::uint32_t times = 0; bool tookIt = false, tookFirst = false; float daysSince = -1.0f; };
    std::vector<Had> HadBy(RE::FormID partner);       // the men this partner has had (SkyrimNet: what he remembers)

    std::string Status(RE::Actor* a);                 // one line for the Status page
    void __stdcall RenderPage();                      // the "Virginity" page
    void RenderTests(RE::Actor* a);                   // its part of the Testing page
}

#pragma once

// The gape, its timing, the anus colour and the RaceMenu overrides - done in the DLL with RaceMenu's own C++
// interface (no Papyrus), so a busy script engine can't slow them down. The gape script then only listens for
// climax events. If RaceMenu doesn't offer an interface, that part stays with the script as before.
namespace Engine {
    void Install();                       // SKSE load: the worker thread
    void Connect();                       // after all plugins loaded: ask RaceMenu for its interfaces
    void Reset();                         // a save was loaded
    int Mode();                           // 0 = script does everything, 1 = gape here, 3 = gape + colour here
    void RegisterFuncs(RE::BSScript::IVirtualMachine* vm);
    bool TestGape(RE::Actor* a, float dil);       // an after-sex gape now (Testing page)
    float GapeOf(RE::FormID id);                  // the gape morph we last set (-1 = not ours)
    struct During { float depth = -100.0f, hole = 0.0f, ring = 0.0f, off = -1.0f; bool held = false; };   // while he is taken: how deep, hole size, rim pull, how far the shaft is off the hole's centre (-1 = not measured), kept in by the hold
    During DuringOf(RE::FormID id);
    // 2.5o: his belly bulge right now (SkyrimNet / Bridge): showing, which shape, how far up (sections, 0-8), the most this
    // round, super deep, how strong, the stomach, the partner, an imprint lingering afterwards
    struct Bulge { bool on = false; int shape = 0; float reach = 0.0f, roundMax = 0.0f; bool deep = false, roundDeep = false;
                   float amount = 0.0f, belly = 0.0f, imprint = 0.0f; RE::FormID partner = 0; double since = 0.0; };
    Bulge BulgeOf(RE::FormID id);
    std::string LastDeepRoll();                   // 2.5s: "X's scene went super deep (30% chance)" - the last roll
    bool TestBulge(RE::Actor* a, int shape);      // Testing page: show the belly bulge on him for 8 s (shape 1-11, During::BulgeShape)
    bool SetLooseNow(RE::Actor* a, float v);      // lasting looseness, right now (false = no RaceMenu interface)
    std::string Status();                         // one line: what is connected
    // RaceMenu node overrides (no Papyrus); false = not available, use the old way
    bool NodeFloat(RE::Actor* a, const char* node, int key, float value);
    bool NodeRemove(RE::Actor* a, const char* node, int key, int index);
    bool RingDirect();                    // the skin-blend ring is coloured here, straight on its material
}

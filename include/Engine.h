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
    struct During { float depth = -100.0f, hole = 0.0f, ring = 0.0f; };   // while he is taken: how deep, hole size, rim pull
    During DuringOf(RE::FormID id);
    bool TestBulge(RE::Actor* a, int shape);      // Testing page: show the belly bulge on him for 8 s (shape 1-5)
    bool SetLooseNow(RE::Actor* a, float v);      // lasting looseness, right now (false = no RaceMenu interface)
    std::string Status();                         // one line: what is connected
    // RaceMenu node overrides (no Papyrus); false = not available, use the old way
    bool NodeFloat(RE::Actor* a, const char* node, int key, float value);
    bool NodeRemove(RE::Actor* a, const char* node, int key, int index);
    bool RingDirect();                    // the skin-blend ring is coloured here, straight on its material
}

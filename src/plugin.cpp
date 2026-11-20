#include "During.h"
#include "Training.h"
#include "Plugin.h"
#include "Wink.h"
#include "Drip.h"
#include "Engine.h"
#include "Bridge.h"
#include "Colour.h"
#include "Lang.h"
#include "Talk.h"
#include "Face.h"

void OnMessage(SKSE::MessagingInterface::Message* message) {
    switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            Lang::Refresh();      // 2.5v: the game's language is known now
            Colour::TidyMade();   // the folder of textures Anatomica makes: under its size budget before anything uses it
            During::SyncPPA();    // puts PPA's own opening back if a test build had turned it down
            Talk::OnDataLoaded(); // 2.6j: DDF is either loaded by now or not there
            break;
        case SKSE::MessagingInterface::kPostPostLoad:
            Engine::Connect();   // RaceMenu's interfaces (it answers from its PostLoad on)
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            Engine::Reset();
            Wink::Reset();
            Bridge::OnGameLoaded();   // SkyrimNet decorators / actions, bathing
            Talk::Reset();            // 2.6i
            Face::Reset();            // 2.6j
            Training::Reset();        // 2.6j
            UI::ApplySavedSettings();
            break;
        default:
            break;
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    UI::Register();
    Wink::Register();
    Drip::Install();
    Engine::Install();
    logger::info("Anatomica menu loaded");
    return true;
}

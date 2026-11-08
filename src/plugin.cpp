#include "Plugin.h"
#include "Wink.h"
#include "Drip.h"
#include "Engine.h"
#include "Bridge.h"
#include "Colour.h"

void OnMessage(SKSE::MessagingInterface::Message* message) {
    switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            Colour::TidyMade();   // the folder of textures Anatomica makes: under its size budget before anything uses it
            break;
        case SKSE::MessagingInterface::kPostPostLoad:
            Engine::Connect();   // RaceMenu's interfaces (it answers from its PostLoad on)
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            Engine::Reset();
            Wink::Reset();
            Bridge::OnGameLoaded();   // SkyrimNet decorators / actions, bathing
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

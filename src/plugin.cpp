#include "Plugin.h"
#include "Wink.h"
#include "Drip.h"

void OnMessage(SKSE::MessagingInterface::Message* message) {
    switch (message->type) {
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            Wink::Reset();
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
    logger::info("Anatomica menu loaded");
    return true;
}

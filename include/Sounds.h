#pragma once
#include <string>

// 2.6j: sounds Anatomica plays itself, from WAV files in Data/Sound/Fx/Anatomica/<kind>/ (any file names - add, remove or
// replace them without a new plugin). One is picked at random (not the same twice in a row), a little higher or lower each
// time, and it follows the actor's head.
namespace Sounds {
    bool Play(const char* kind, RE::Actor* a, float volume, const char* node = "NPC Head [Head]");
    int Count(const char* kind);   // how many files there are for this kind
    bool PlayFile(const char* path, RE::Actor* a, float volume, const char* node = "NPC Head [Head]");   // 2.6j: one file (loose or in a BSA)
}

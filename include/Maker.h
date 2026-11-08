#pragma once
#include <functional>
#include <string>

// One background thread that makes Anatomica's own textures (rims, ring colours), so the game never waits for them.
// Jobs are keyed by the file they make: asking again while it is being made does nothing.
namespace Maker {
    enum class State { none, busy, done, failed };
    State Of(const std::string& key);                  // what is known about this file
    void Queue(const std::string& key, std::function<bool()> job);   // job returns false if it could not be made
    void MarkDone(const std::string& key);              // found on disk: no need to make it
    void ForgetDone();                                  // files were deleted (tidy): check the disk again
}

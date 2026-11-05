// Everything Windows-specific the overlay needs, kept out of main.cpp and ui.cpp so those stay
// free of <windows.h> (its `near` and `far` macros collide with ordinary names there).
// All paths are UTF-8 with forward slashes.
#pragma once
#include <cstdint>
#include <string>

#include "live.h"

namespace fh {
namespace platform {

std::string dataDir();       // %LOCALAPPDATA%/UTMTFTM
std::string exeDir();        // folder holding this program
std::string steamDir();      // Steam's install folder
std::string steamvrDir();    // SteamVR's install folder
std::string documentsDir();

void sleepMs(int ms);        // accurate to about a millisecond below 50 ms, without changing the system timer
uint32_t currentPid();

// "exe.<name>" for that process ("" if it can't be read): the key for games with no Steam app id.
std::string processExeKey(uint32_t pid);

// live.shm mapped read/write, created if missing. nullptr on failure.
fhlive::Shared* mapLive();

// Add or remove the driver add-on with SteamVR's own vrpathreg tool. SteamVR loads it on its next start.
bool setDriverRegistered(bool on, const std::string& driverDir);

}  // namespace platform
}  // namespace fh

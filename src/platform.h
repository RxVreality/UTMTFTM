// Everything specific to one operating system that the overlay needs, kept out of main.cpp and ui.cpp so those
// stay the same on both (and free of <windows.h>, whose `near` and `far` macros collide with ordinary names there).
// platform_windows.cpp and platform_linux.cpp each implement this. All paths are UTF-8 with forward slashes.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "live.h"

namespace fh {
namespace platform {

std::string dataDir();       // %LOCALAPPDATA%/UTMTFTM, ~/.local/share/UTMTFTM
std::string exeDir();        // folder holding this program
std::string steamDir();      // Steam's install folder
std::string steamvrDir();    // SteamVR's install folder
std::string documentsDir();  // where SteamVR keeps its "steamvr" folder of per-user bindings
std::vector<std::string> configDirs();  // SteamVR's config folders, the ones in openvrpaths.vrpath, then Steam's own

void sleepMs(int ms);        // accurate to about a millisecond below 50 ms, without changing the system timer
uint32_t currentPid();

// "exe.<name>" for that process ("" if it can't be read): the key for games with no Steam app id.
// On Linux a process Steam started says its app id even when SteamVR doesn't know it, giving "steam.app.<id>".
std::string processExeKey(uint32_t pid);

#ifndef _WIN32
// The working part of processExeKey, on the raw contents of /proc/<pid>/environ and /proc/<pid>/cmdline.
std::string exeKeyFrom(const std::string& environ, const std::string& cmdline);
#endif

// A path as SteamVR wants it in a binding setting. It has to come out as file:///C:/... or file:///home/...
inline std::string fileUrl(const std::string& path) { return "file:///" + (!path.empty() && path[0] == '/' ? path.substr(1) : path); }

// live.shm mapped read/write, created if missing. nullptr on failure.
fhlive::Shared* mapLive();

// Add or remove the driver add-on with SteamVR's own vrpathreg tool. SteamVR loads it on its next start.
bool setDriverRegistered(bool on, const std::string& driverDir);

}  // namespace platform
}  // namespace fh

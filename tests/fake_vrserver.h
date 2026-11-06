// The server half of the simulated SteamVR (see fake_steamvr.cpp). It's a separate file because openvr.h and
// openvr_driver.h can't share a translation unit.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fakeserver {

struct Pose {
  double pos[3];
  double rot[4];  // w, x, y, z
};

// Loads the real driver the way vrserver does and starts reporting poses through its hook. False if it won't load.
// hooked: send the hands through the host call the driver hooks. Off, they reach SteamVR some other way, as a controller
// whose driver doesn't report through that call would.
// leftDevice, rightDevice: which devices are the hands, whose controllers then set their buttons up through the driver's hook.
bool start(const char* driverPath, bool hooked, uint32_t leftDevice = 0, uint32_t rightDevice = 0);
void stop();

// A controller reports a button, through the driver's hook. hand 0 is left, 1 is right.
void reportButton(int hand, const char* name, float value);
// What's reached SteamVR, in order: "R/input/a/click" and its value.
std::vector<std::pair<std::string, float>> heardButtons();
void clearHeardButtons();

void setDevicePose(uint32_t device, const Pose& pose);  // where a device really is
Pose seenPose(uint32_t device);                         // where SteamVR, and so every game, sees it

}  // namespace fakeserver

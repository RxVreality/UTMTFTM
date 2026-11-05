// The server half of the simulated SteamVR (see fake_steamvr.cpp). It's a separate file because openvr.h and
// openvr_driver.h can't share a translation unit.
#pragma once
#include <cstdint>

namespace fakeserver {

struct Pose {
  double pos[3];
  double rot[4];  // w, x, y, z
};

// Loads the real driver the way vrserver does and starts reporting poses through its hook. False if it won't load.
// hooked: send the hands through the host call the driver hooks. Off, they reach SteamVR some other way, as a controller
// whose driver doesn't report through that call would.
bool start(const char* driverPath, bool hooked);
void stop();

void setDevicePose(uint32_t device, const Pose& pose);  // where a device really is
Pose seenPose(uint32_t device);                         // where SteamVR, and so every game, sees it

}  // namespace fakeserver

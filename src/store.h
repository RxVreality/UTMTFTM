// Profiles (global default + per-game overrides) and writing them into SteamVR per-app bindings.
#pragma once
#include "live.h"
#include "placement.h"
#include "xform.h"
#include <map>
#include <string>
#include <vector>

namespace fh {

struct HandPair {
  Offset left, right;
  bool zero() const { return left.zero() && right.zero(); }
};

struct AppProfile {
  std::string name;
  bool override_global = false;  // false → this game follows the global default
  HandPair hands;
  std::string original_url;      // CurrentURL before we took over ("" = none was set)
  std::string settings_key;      // e.g. frame_controller_250820_CurrentURL_openxr
  bool managed = false;          // CurrentURL currently points at our file
  bool live = false;             // the driver add-on reaches this game (offsets apply live, no binding)
};

struct Store {
  float max_pos = 0.15f;   // metres (SteamVR hard cap 0.30)
  float max_rot = 180.f;   // rotation is never limited (any orientation is valid); only distance is
  HandPair global;
  std::map<std::string, AppProfile> apps;

  // Ghost resting pose per hand (grip frame), matched once by the user to a game's own hand.
  Offset ghost[2];
  bool ghostMatched[2] = {false, false};
  // Matched pose; an unmatched hand mirrors the other one; neither -> the OpenXR spec anchor.
  Offset ghostBase(int hand) const { return ghostBaseFor(ghost, ghostMatched, hand); }

  static std::string dir();          // %LOCALAPPDATA%/UTMTFTM
  static std::string bindingsDir();  // %LOCALAPPDATA%/UTMTFTM/bindings
  bool load();
  bool save() const;

  // The offsets that should be active for an app.
  HandPair effective(const std::string& app) const;

  // Write the app's binding file with `effective(app)` and point SteamVR at it (live setting;
  // the game picks it up on its next launch). If the effective offsets are zero and we manage
  // the app, the original binding is restored instead. Returns a human-readable status.
  std::string apply(const std::string& app, std::string* err);
  // Put the app back on its original binding (used for live games so offsets aren't applied twice).
  void restoreBinding(const std::string& app);

  // Hand the driver add-on the running game's offsets (live.shm), turned from the grip frame into the device's
  // own frame. `drag` replaces them while a hand is being placed.
  void publishLive(const HandPair* drag = nullptr) const;

  // The running game.
  uint32_t scenePid = 0;
  std::string sceneKey;

  // The SteamVR driver add-on.
  bool useDriver = false;
  uint32_t device[2] = {fhlive::kNoDevice, fhlive::kNoDevice};  // tracked device index per hand
  M gripFromRaw[2] = {ident(), ident()};                        // the OpenXR grip pose in the device's frame
  bool dashboardUp = false;  // the driver's offsets stand aside while the dashboard's open, so its pointers point true
  static uint64_t driverPoseUpdates();  // the driver's running count of poses it has seen
  static M appliedDeviceOffset(int hand);  // what the driver has put in the poses being reported now
  static void askTruePoses(bool on);  // while placing, have the driver report where the controllers really are
  static bool driverTruePose(int hand, M* out);  // that report, in SteamVR's raw tracking space. False until there is one

  // The controller type in use ("frame_controller", "oculus_touch", "knuckles"), empty until one is seen: binding files and
  // SteamVR's per-game keys are named after it.
  static std::string controllerType;
};

void logf(const char* fmt, ...);

}  // namespace fh

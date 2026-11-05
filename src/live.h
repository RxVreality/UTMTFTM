// Shared-memory channel between the UTMTFTM overlay and the SteamVR driver add-on.
// File: %LOCALAPPDATA%\UTMTFTM\live.shm (fixed size). The file outlives the overlay, so a game keeps its offsets
// even with the overlay closed.
#pragma once
#include <atomic>
#include <cstdint>

namespace fhlive {

constexpr uint32_t kDriverMagic = 0x31444846;  // "FHD1"
constexpr uint32_t kNoDevice = 0xFFFFFFFFu;

// An offset in the device's own frame, the way the driver applies it: device' = device * offset.
struct DeviceOffset {
  float pos[3];
  float rot[4];  // x, y, z, w
};

struct DriverChannel {
  // The overlay writes these. Seqlock: seq is odd mid-update.
  uint32_t magic;
  std::atomic<uint32_t> seq;
  uint32_t device[2];       // tracked device index of the left and right hand
  DeviceOffset want[2];
  // The driver writes these.
  std::atomic<uint64_t> poseUpdates;  // counts every pose it sees: a moving count means it's alive
  std::atomic<uint32_t> appliedSeq;   // seqlock for applied
  uint32_t reserved;
  DeviceOffset applied[2];            // what the poses being reported now have in them
  // While the overlay asks (it's placing a hand), the driver also reports where each controller really is:
  // before any offset, in SteamVR's raw tracking space. Seqlock per hand; 0 means nothing reported yet.
  std::atomic<uint32_t> wantTrue;
  std::atomic<uint32_t> trueSeq[2];
  DeviceOffset truePose[2];
};

struct Shared {
  DriverChannel driver;
};

}  // namespace fhlive

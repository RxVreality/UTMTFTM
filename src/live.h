// Shared-memory channel between the UTMTFTM overlay and the SteamVR driver add-on.
// File: live.shm in the data folder (fixed size), which is %LOCALAPPDATA%\UTMTFTM on Windows and ~/.local/share/UTMTFTM
// on Linux. The file outlives the overlay, so a game keeps its offsets even with the overlay closed.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#endif

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
  // Counts the poses seen for each hand's device. If a hand is on and this stays still, its driver reports some way
  // the hook can't see, and moving it won't work. Kept last so the fields above stay where they've always been.
  std::atomic<uint64_t> handUpdates[2];
};

struct Shared {
  DriverChannel driver;
};

// Two programs read this file, and a torn or misaligned atomic faults on ARM rather than just misbehaving.
static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free, "the atomics have to work across processes");
static_assert(sizeof(DeviceOffset) == 28 && offsetof(DriverChannel, poseUpdates) == 72 && offsetof(DriverChannel, handUpdates) == 216, "the file layout changed");

// Writing a seqlock. x86 keeps stores in order by itself and ARM doesn't: without the fence the new fields can become
// visible before the odd number does, and a reader takes half-written values for a finished update.
inline void seqWriteBegin(std::atomic<uint32_t>& seq) {
  seq.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
}
inline void seqWriteEnd(std::atomic<uint32_t>& seq) { seq.fetch_add(1, std::memory_order_release); }

#ifndef _WIN32
// The driver runs inside SteamVR and the overlay may be started from a terminal, and both have to land on the same
// file. So this goes by the home folder alone and ignores XDG_DATA_HOME, which one of them might have and the other not.
inline std::string linuxHome() {
  const char* h = std::getenv("HOME");
  if (h && h[0]) return h;
  const passwd* p = getpwuid(getuid());
  return p && p->pw_dir ? p->pw_dir : ".";
}
inline std::string linuxDataDir() { return linuxHome() + "/.local/share/UTMTFTM"; }
#endif

}  // namespace fhlive

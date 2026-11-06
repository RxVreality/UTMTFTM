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

// ---------------------------------------------------------------- buttons
// The driver also sits on the call controllers report their buttons through. That's what lets one button play back a
// recorded sequence, and lets a rewired button work straight away. See the buttons section of driver_main.cpp.
constexpr uint32_t kInputMagic = 0x32495446;  // "FTI2": the driver that's loaded has the button hook

constexpr uint32_t kEventRing = 256, kMaxMacros = 12, kMaxSteps = 32, kMaxRoutes = 16, kMaxChords = 6, kMaxChordInputs = 4;
constexpr uint8_t kNoHand = 255;

// What the driver heard, for the overlay while it's recording. Nothing goes in here the rest of the time.
struct InputEvent {
  std::atomic<uint32_t> seq;  // the write number it's filled in for, set last
  uint32_t ms;                // the driver's clock
  float value;                // 0 or 1 for a button, otherwise how far
  uint8_t hand;               // 0 left, 1 right
  uint8_t reserved[3];
  char name[32];              // "/input/a/click"
};

enum StepKind : uint8_t { StepPress, StepRelease, StepTouch, StepUntouch, StepPush, StepCentre };

struct WireStep {
  uint16_t delayMs;  // how long after the step before it
  uint8_t hand, kind;
  int8_t x, y;       // StepPush: which way, -100 to 100
  char path[26];     // "/input/a"
};
struct WireMacro {
  uint8_t hand, onTouch, repeat, count;
  char path[28];  // what plays it
  WireStep steps[kMaxSteps];
};
// While this game runs, a press of one button reaches the game as a press of another. toHand kNoHand: as nothing.
struct WireRoute {
  uint8_t fromHand, toHand;
  char from[27], to[27];
};
// Controls held together press a button that already does what the combo should.
struct WireChordInput {
  uint8_t hand, onTouch;
  char path[26];
};
struct WireChord {
  uint8_t count, targetHand;
  char target[26];
  WireChordInput in[kMaxChordInputs];
};
struct InputTable {
  uint32_t macros, routes, chords, reserved;
  WireMacro macro[kMaxMacros];
  WireRoute route[kMaxRoutes];
  WireChord chord[kMaxChords];
};

struct InputChannel {
  // The driver writes these.
  std::atomic<uint32_t> magic;
  std::atomic<uint32_t> components;     // how many buttons, sticks and sensors it's heard about being set up
  std::atomic<uint64_t> reports;        // counts every report it recognised: a moving count means this works on this controller
  std::atomic<uint64_t> unknownReports; // reports from something it never saw set up: it loaded too late to name it
  std::atomic<uint64_t> macrosPlayed;
  std::atomic<uint64_t> ticks;          // how many times SteamVR has called its RunFrame: nothing plays without that
  std::atomic<uint32_t> evWrite;        // next event to fill. Only moves while recording
  std::atomic<uint32_t> nowMs;          // its clock, so the overlay can say how long to record for
  // The overlay writes these.
  std::atomic<uint32_t> enabled;        // macros and the rest may act: there's a game, the dashboard's shut, nobody's placing a hand
  std::atomic<uint32_t> recordUntilMs;  // while the driver's clock is before this it records and holds every button back from
                                        // SteamVR, so the dashboard doesn't get them. It has to be kept moving, so a crash can't leave buttons dead
  std::atomic<uint32_t> roomToggle;     // goes up by one to have the Steam button double-pressed, which is Room View's shortcut
  std::atomic<uint32_t> roomToggleDone; // the driver: the last one it acted on. Only moves if there's a Steam button to press
  std::atomic<uint32_t> tableSeq;       // seqlock for table
  uint32_t reserved;
  InputEvent events[kEventRing];
  InputTable table;
};

struct Shared {
  DriverChannel driver;
  // After driver, so an older driver that's still loaded never sees it. Its magic says whether this one is really there.
  InputChannel input;
};

// Two programs read this file, and a torn or misaligned atomic faults on ARM rather than just misbehaving.
static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free, "the atomics have to work across processes");
static_assert(sizeof(DeviceOffset) == 28 && offsetof(DriverChannel, poseUpdates) == 72 && offsetof(DriverChannel, handUpdates) == 216, "the file layout changed");
static_assert(sizeof(InputEvent) == 48 && sizeof(WireStep) == 32 && sizeof(WireRoute) == 56 && sizeof(WireChord) == 140 && alignof(InputChannel) == 8, "the file layout changed");

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

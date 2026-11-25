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

constexpr uint32_t kEventRing = 256, kMaxMacros = 12, kMaxSteps = 32, kMaxRoutes = 16, kMaxChords = 6, kMaxChordInputs = 4, kMaxLinks = 32;
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
  uint8_t hand, onTouch;  // 0 pressed, 1 touched, or pushed one way: a ChordWay. Ways only go to a driver that says kFeatureWays
  char path[26];
};
enum ChordWay : uint8_t { ChordNorth = 2, ChordEast, ChordSouth, ChordWest };

// One part of one control. For links.
enum LinkPart : uint8_t { LinkClick = 1, LinkTouch, LinkValue, LinkMove, LinkNorth, LinkEast, LinkSouth, LinkWest };
// While this game runs, one part of a control does what another part did when the game launched. The overlay works these
// out from the binding the game loaded and the one it would load now, which is how a whole layout, a stick pushed one way or a
// press on its own can change with no restart. Several from the same part all happen together. toHand kNoHand: it does nothing.
struct WireLink {
  uint8_t fromHand, fromPart, toHand, toPart;
  float amount;  // a LinkValue it sets, how far. 1 otherwise
  char from[24], to[24];  // "/input/thumbstick"
};
struct WireChord {
  uint8_t count, targetHand;
  char target[26];
  WireChordInput in[kMaxChordInputs];
};
struct InputTable {
  uint32_t macros, routes, chords, links;  // links was always 0 before the driver had them, so an older one ignores them
  WireMacro macro[kMaxMacros];
  WireRoute route[kMaxRoutes];
  WireChord chord[kMaxChords];
  WireLink link[kMaxLinks];  // last, so everything before it stays where an older driver looks
};

// What the loaded driver can do beyond macros, swaps and combos of touches and presses. It says so in InputChannel::features.
constexpr uint32_t kFeatureLinks = 1, kFeatureWays = 2;
// A link from a stick pushed one way to nothing holds that way at the middle: pushing it does nothing in the game. Older
// drivers ignored those links, so the overlay only counts on it from a driver that says so.
constexpr uint32_t kFeatureHold = 4;

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
  uint32_t unused[2];                   // were for Room View, kept so nothing after them moves
  std::atomic<uint32_t> tableSeq;       // seqlock for table
  std::atomic<uint32_t> features;       // the driver writes this: kFeature bits. 0 from a driver older than links
  InputEvent events[kEventRing];
  InputTable table;
};

// ---------------------------------------------------------------- Decaffeinate
// Smoothing the shake out of each hand, in the same pose call the offsets go through. The filter is in decaf.h.
constexpr uint32_t kDecafMagic = 0x31464344;  // "DCF1": the driver that's loaded can smooth

struct DecafSettings {
  float rotMinHz, rotBeta, posMinHz, posBeta, speedHz;  // see decaf::Params. rotMinHz 0: off
};
struct DecafChannel {
  std::atomic<uint32_t> magic;    // the driver writes this
  // The overlay writes these. Seqlock: seq is odd mid-update. The settings themselves come from the overlay, so the four
  // steps can be retuned without touching the driver or restarting SteamVR.
  std::atomic<uint32_t> seq;
  DecafSettings want[2];          // left, right
  std::atomic<uint32_t> measure;  // while the page that shows it is open: measure the shake even with smoothing off
  // The driver writes these: how much each hand shakes while held still, before and after smoothing. Thousandths of a
  // degree, and micrometres.
  std::atomic<uint32_t> rotRaw[2], rotOut[2], posRaw[2], posOut[2];
  uint32_t reserved;
  std::atomic<uint64_t> smoothed[2];  // poses smoothed for each hand: a moving count means it's working on this controller
};

// Decaffeinate sits at a fixed place in the file, well past the end of input, so input can grow without moving it. A
// driver and an overlay from different versions then still agree where it is, and an older file is simply made longer,
// with zeros: off.
constexpr size_t kDecafAt = 64 * 1024;

// ---------------------------------------------------------------- placing a hand
// While one hand drags the other's in-game hand, the driver moves the held one itself, on each of its own poses, from where
// the holding hand is right then. Nothing waits on the overlay, whose drawing took 40 ms a frame over Virtual Desktop and
// made placing lag (J4, 14 November). The overlay still works the offset out for itself, to show and to save.
constexpr uint32_t kDragMagic = 0x31475244;  // "DRG1": the driver that's loaded can do this

struct DragChannel {
  std::atomic<uint32_t> magic;  // the driver writes this
  std::atomic<uint32_t> seq;    // the overlay writes the next four. Seqlock
  uint32_t active;              // 1 while a hand is held. All zeros, as in a new file: nothing is
  uint32_t held, by;            // the hand being placed and the hand holding it: 0 left, 1 right
  DeviceOffset start;           // the held hand's device offset when it was grabbed. On its first pose after that, the driver
                                // works out where it sits in the holding hand's frame, and keeps it there
  std::atomic<uint64_t> moved;  // the driver writes: poses it has moved this way. A moving count means it's doing it
};
constexpr size_t kDragAt = kDecafAt + 256;

// ---------------------------------------------------------------- stick drift
// A worn stick that doesn't come back to the middle. The driver fixes each stick report on its way to SteamVR, in the button
// hook, with the numbers the overlay worked out from the stick check (the maths is drift.h). While the overlay asks, it also
// keeps the last reports of each stick, so the page can show the stick live and run the check from them.
constexpr uint32_t kDriftMagic = 0x31465244;  // "DRF1": the driver that's loaded can do this
constexpr uint32_t kDriftRing = 512, kDriftDirs = 8;

struct DriftSettings {  // one stick, the same layout as drift::Params. All zeros: off
  uint32_t on;
  float cx, cy, dead, gate, learnPerSec, learnMax;
  float reach[kDriftDirs];
};
// 0.6.4 (J4, 18 and 20 November): whether each stick's flicks are held back, and how. The same layout as drift::Flicks. All
// zeros: no flicks held back.
struct FlickSettings {
  uint32_t on;
  float big, out, most, stay, back;
  uint32_t spare[2];
};
// "FLK2": the driver that's loaded holds flicks back by what they look like. 0.6.4's first test build wrote "FLK1" and held
// them by a map of where they started, which the overlay doesn't send any more: that one needs SteamVR restarted too.
constexpr uint32_t kFlickMagic = 0x324B4C46;
struct DriftSample {
  std::atomic<uint32_t> seq;  // the write number it's filled in for, set last
  uint32_t ms;                // the driver's clock
  float x, y;                 // what the stick said
  float ox, oy;               // what went on to SteamVR
  uint8_t touch;              // 0 no thumb on it, 1 a thumb, 2 this stick can't tell
  uint8_t reserved[3];
};
struct DriftChannel {
  std::atomic<uint32_t> magic;    // the driver writes this
  std::atomic<uint32_t> seq;      // the overlay writes want. Seqlock
  DriftSettings want[2];          // left, right
  std::atomic<uint32_t> measure;  // the overlay: while the page that shows them is open, keep the reports in ring
  // The driver writes the rest.
  std::atomic<uint32_t> found[2];     // 1: this hand has a stick. 2 as well: it can feel a thumb on it
  std::atomic<uint32_t> touchBad[2];  // its touch sensor missed a real push, so touch isn't used until the settings change
  std::atomic<uint32_t> learnSeq[2];  // seqlock for learned
  float learned[2][2];                // where the middle has got to by learning while you play, as x, y
  std::atomic<uint32_t> spikes[2];    // one-report spikes held back
  uint32_t reserved;
  std::atomic<uint64_t> fixed[2];     // reports changed: a moving count means it's working on this controller
  std::atomic<uint32_t> write[2];     // the next sample to fill, per hand
  DriftSample ring[2][kDriftRing];
  // 0.6.4, after everything 0.6.3 had, so 0.6.3's driver and program never see it. A zeroed file means no map.
  std::atomic<uint32_t> flickMagic;   // the driver writes kFlickMagic
  std::atomic<uint32_t> flicksHeld[2];  // the driver: flicks away from the middle held back, per hand
  uint32_t reserved2;
  FlickSettings flick[2];             // the overlay writes these with want, under the same seq
};
// Its own fixed place, past the drag channel with room to spare, for the same reasons as Decaffeinate's.
constexpr size_t kDriftAt = kDragAt + 256;

struct Shared {
  DriverChannel driver;
  // After driver, so an older driver that's still loaded never sees it. Its magic says whether this one is really there.
  InputChannel input;
  unsigned char room[kDecafAt - sizeof(DriverChannel) - sizeof(InputChannel)];  // for input to grow into. Nothing reads it
  DecafChannel decaf;
  unsigned char room2[kDragAt - kDecafAt - sizeof(DecafChannel)];  // for Decaffeinate to grow into
  DragChannel drag;
  unsigned char room3[kDriftAt - kDragAt - sizeof(DragChannel)];  // for placing to grow into
  DriftChannel drift;
};

// Two programs read this file, and a torn or misaligned atomic faults on ARM rather than just misbehaving.
static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free, "the atomics have to work across processes");
static_assert(sizeof(DeviceOffset) == 28 && offsetof(DriverChannel, poseUpdates) == 72 && offsetof(DriverChannel, handUpdates) == 216, "the file layout changed");
static_assert(sizeof(InputEvent) == 48 && sizeof(WireStep) == 32 && sizeof(WireRoute) == 56 && sizeof(WireChord) == 140 && alignof(InputChannel) == 8, "the file layout changed");
static_assert(sizeof(DecafSettings) == 20 && offsetof(DecafChannel, measure) == 48 && offsetof(DecafChannel, smoothed) == 88 && sizeof(DecafChannel) == 104 &&
              offsetof(Shared, input) == sizeof(DriverChannel) && offsetof(Shared, decaf) == kDecafAt, "the file layout changed");
static_assert(offsetof(DragChannel, start) == 20 && offsetof(DragChannel, moved) == 48 && sizeof(DragChannel) == 56 && offsetof(Shared, drag) == kDragAt,
              "the file layout changed");
static_assert(sizeof(DriftSettings) == 60 && sizeof(DriftSample) == 28 && offsetof(DriftChannel, measure) == 128 && offsetof(DriftChannel, fixed) == 184 &&
              offsetof(DriftChannel, ring) == 208 && offsetof(Shared, drift) == kDriftAt, "the file layout changed");
static_assert(sizeof(FlickSettings) == 32 && offsetof(DriftChannel, flickMagic) == 208 + sizeof(DriftSample) * 2 * kDriftRing &&
              offsetof(DriftChannel, flick) == 28896 && sizeof(DriftChannel) == 28960, "the file layout changed");
static_assert(sizeof(WireLink) == 56 && offsetof(InputTable, link) == 16 + sizeof(WireMacro) * kMaxMacros + sizeof(WireRoute) * kMaxRoutes + sizeof(WireChord) * kMaxChords, "links go after everything that was there before");
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "features sits where a plain number was");

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

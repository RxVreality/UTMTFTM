// driver_utmtftm: a SteamVR add-on with no devices of its own. It sits between SteamVR's device drivers and
// SteamVR itself and moves the two hand controllers' poses by the offsets the overlay publishes in
// live.shm (see live.h). Every game then sees the moved controller, live, whichever API it uses,
// and nothing is loaded inside the game.
//
// It hooks IVRServerDriverHost::TrackedDevicePoseUpdated, the call every driver reports poses through. That
// runs at the tracking rate for every device, so unchanged offsets come from a per-thread cache checked
// against the shared sequence number: no allocation, no file I/O and no locks on that path.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <share.h>
#define FH_EXPORT __declspec(dllexport)
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#define FH_EXPORT __attribute__((visibility("default")))
#endif

#include <openvr_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../live.h"

namespace {

// Cleared when the driver shuts down, while another driver's thread may still be reporting a pose.
std::atomic<fhlive::DriverChannel*> g_ch{nullptr};

#ifdef _WIN32
int lastError() { return int(GetLastError()); }
void spinPause() { YieldProcessor(); }

std::wstring dataDir() {
  wchar_t b[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", b, MAX_PATH);
  return (n && n < MAX_PATH ? std::wstring(b) : std::wstring(L".")) + L"\\UTMTFTM";
}

FILE* openLog() {
  std::wstring d = dataDir();
  CreateDirectoryW(d.c_str(), nullptr);
  return _wfsopen((d + L"\\driver.log").c_str(), L"a", _SH_DENYNO);
}

fhlive::Shared* mapShared() {
  std::wstring d = dataDir();
  CreateDirectoryW(d.c_str(), nullptr);
  HANDLE f = CreateFileW((d + L"\\live.shm").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return nullptr;
  HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, sizeof(fhlive::Shared), nullptr);
  CloseHandle(f);
  if (!map) return nullptr;
  void* v = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(fhlive::Shared));
  CloseHandle(map);
  return static_cast<fhlive::Shared*>(v);
}

bool writeSlot(void** slot, void* value) {
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
  *slot = value;
  VirtualProtect(slot, sizeof(void*), old, &old);
  return true;
}
#else
int lastError() { return errno; }
void spinPause() {
#if defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}

FILE* openLog() {
  std::error_code ec;
  std::filesystem::create_directories(fhlive::linuxDataDir(), ec);
  return fopen((fhlive::linuxDataDir() + "/driver.log").c_str(), "a");
}

fhlive::Shared* mapShared() {
  std::error_code ec;
  std::filesystem::create_directories(fhlive::linuxDataDir(), ec);
  int f = open((fhlive::linuxDataDir() + "/live.shm").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (f < 0) return nullptr;
  struct stat st {};
  if (fstat(f, &st) != 0 || (st.st_size < off_t(sizeof(fhlive::Shared)) && ftruncate(f, sizeof(fhlive::Shared)) != 0)) {
    close(f);
    return nullptr;
  }
  void* v = mmap(nullptr, sizeof(fhlive::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
  close(f);
  return v == MAP_FAILED ? nullptr : static_cast<fhlive::Shared*>(v);
}

// What the memory holding addr is allowed to do right now. Unlike VirtualProtect, mprotect doesn't say what it was.
int protectionOf(const void* addr) {
  int prot = PROT_READ;  // what a vtable normally is, if the kernel's list can't be read
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return prot;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    unsigned long long lo, hi;
    char r, w, x;
    if (sscanf(line, "%llx-%llx %c%c%c", &lo, &hi, &r, &w, &x) != 5) continue;
    if (reinterpret_cast<uintptr_t>(addr) < lo || reinterpret_cast<uintptr_t>(addr) >= hi) continue;
    prot = (r == 'r' ? PROT_READ : 0) | (w == 'w' ? PROT_WRITE : 0) | (x == 'x' ? PROT_EXEC : 0);
    break;
  }
  fclose(f);
  return prot;
}

bool writeSlot(void** slot, void* value) {
  const int prot = protectionOf(slot);
  const uintptr_t page = reinterpret_cast<uintptr_t>(slot) & ~(uintptr_t(sysconf(_SC_PAGESIZE)) - 1);
  void* start = reinterpret_cast<void*>(page);
  if (!(prot & PROT_WRITE) && mprotect(start, size_t(sysconf(_SC_PAGESIZE)), prot | PROT_WRITE) != 0) return false;
  __atomic_store_n(slot, value, __ATOMIC_RELEASE);  // another CPU may be calling through it, and must see the whole pointer
  if (!(prot & PROT_WRITE)) mprotect(start, size_t(sysconf(_SC_PAGESIZE)), prot);
  return true;
}
#endif

void logf(const char* fmt, ...) {
  static FILE* f = nullptr;
  if (!f) f = openLog();
  if (!f) return;
  va_list a;
  va_start(a, fmt);
  vfprintf(f, fmt, a);
  va_end(a);
  fputs("\n", f);
  fflush(f);
}

// The mapping stays until SteamVR exits: another driver's thread could be part way through a pose when this one shuts down.
void closeShared() { g_ch.store(nullptr, std::memory_order_release); }

const fhlive::DeviceOffset kIdentity = {{0, 0, 0}, {0, 0, 0, 1}};

bool isIdentity(const fhlive::DeviceOffset& o) {
  return std::fabs(o.pos[0]) < 1e-6f && std::fabs(o.pos[1]) < 1e-6f && std::fabs(o.pos[2]) < 1e-6f && std::fabs(std::fabs(o.rot[3]) - 1.f) < 1e-6f;
}

// Whatever is in the shared file, SteamVR never gets a broken pose from here. False if it can't be used.
// Written so a NaN fails every comparison.
bool usable(fhlive::DeviceOffset& o) {
  const float n2 = o.rot[0] * o.rot[0] + o.rot[1] * o.rot[1] + o.rot[2] * o.rot[2] + o.rot[3] * o.rot[3];
  if (!(n2 > 0.25f && n2 < 4.f)) return false;
  for (float p : o.pos) if (!(std::fabs(p) < 5.f)) return false;
  if (std::fabs(n2 - 1.f) > 1e-4f) {
    const float n = std::sqrt(n2);
    for (float& q : o.rot) q /= n;
  }
  return true;
}

// Tell the overlay what's in the poses from here on, so it can work out where the controller really is.
void publishApplied(fhlive::DriverChannel* ch, const fhlive::DeviceOffset o[2]) {
  static std::atomic_flag busy = ATOMIC_FLAG_INIT;
  if (busy.test_and_set(std::memory_order_acquire)) return;
  fhlive::seqWriteBegin(ch->appliedSeq);
  memcpy(ch->applied, o, sizeof(fhlive::DeviceOffset) * 2);
  fhlive::seqWriteEnd(ch->appliedSeq);
  busy.clear(std::memory_order_release);
}

struct OffsetCache {
  uint32_t seq = 1;  // odd: never matches a published sequence
  uint32_t device[2] = {fhlive::kNoDevice, fhlive::kNoDevice};
  bool on[2] = {false, false};
  fhlive::DeviceOffset o[2];
};
thread_local OffsetCache t_cache;

void refreshCache(fhlive::DriverChannel* ch, OffsetCache& c) {
  for (int tries = 0; tries < 8; ++tries) {
    uint32_t s0 = ch->seq.load(std::memory_order_acquire);
    if (s0 & 1) { spinPause(); continue; }
    uint32_t magic = ch->magic, dev[2] = {ch->device[0], ch->device[1]};
    fhlive::DeviceOffset o[2];
    memcpy(o, ch->want, sizeof o);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (ch->seq.load(std::memory_order_acquire) != s0) continue;  // the overlay got in: read again
    bool valid = magic == fhlive::kDriverMagic;
    for (int h = 0; h < 2; ++h) {
      c.device[h] = valid ? dev[h] : fhlive::kNoDevice;
      if (!usable(o[h])) o[h] = kIdentity;
      c.on[h] = valid && !isIdentity(o[h]);
      c.o[h] = c.on[h] ? o[h] : kIdentity;
    }
    c.seq = s0;
    publishApplied(ch, c.o);
    return;
  }
  // Still contended: keep the offsets from last time rather than letting the hand jump.
}

vr::HmdQuaternion_t qmul(const vr::HmdQuaternion_t& a, const vr::HmdQuaternion_t& b) {
  vr::HmdQuaternion_t r;
  r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
  r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
  r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
  r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
  return r;
}

void qrot(const vr::HmdQuaternion_t& q, const double v[3], double out[3]) {
  const double cx = q.y * v[2] - q.z * v[1], cy = q.z * v[0] - q.x * v[2], cz = q.x * v[1] - q.y * v[0];
  out[0] = v[0] + 2 * (q.w * cx + q.y * cz - q.z * cy);
  out[1] = v[1] + 2 * (q.w * cy + q.z * cx - q.x * cz);
  out[2] = v[2] + 2 * (q.w * cz + q.x * cy - q.y * cx);
}

// device' = world * pose * driverFromHead * offset. Only the last transform changes, so SteamVR's prediction
// and velocities keep working as if the controller were simply built with the hand in that place.
void applyOffset(vr::DriverPose_t& p, const fhlive::DeviceOffset& o) {
  const double v[3] = {o.pos[0], o.pos[1], o.pos[2]};
  double t[3];
  qrot(p.qDriverFromHeadRotation, v, t);
  for (int i = 0; i < 3; ++i) p.vecDriverFromHeadTranslation[i] += t[i];
  vr::HmdQuaternion_t r;
  r.w = o.rot[3]; r.x = o.rot[0]; r.y = o.rot[1]; r.z = o.rot[2];
  p.qDriverFromHeadRotation = qmul(p.qDriverFromHeadRotation, r);
}

// The controller itself: world * pose * driverFromHead, with no offset in it. The overlay reads this while
// placing, because what it publishes changes the poses it would otherwise be reading back.
void publishTrue(fhlive::DriverChannel* ch, int hand, const vr::DriverPose_t& p) {
  double inPose[3], inWorld[3];
  qrot(p.qRotation, p.vecDriverFromHeadTranslation, inPose);
  for (int i = 0; i < 3; ++i) inPose[i] += p.vecPosition[i];
  qrot(p.qWorldFromDriverRotation, inPose, inWorld);
  const vr::HmdQuaternion_t q = qmul(qmul(p.qWorldFromDriverRotation, p.qRotation), p.qDriverFromHeadRotation);
  fhlive::DeviceOffset o;
  for (int i = 0; i < 3; ++i) o.pos[i] = float(inWorld[i] + p.vecWorldFromDriverTranslation[i]);
  o.rot[0] = float(q.x); o.rot[1] = float(q.y); o.rot[2] = float(q.z); o.rot[3] = float(q.w);
  fhlive::seqWriteBegin(ch->trueSeq[hand]);
  ch->truePose[hand] = o;
  fhlive::seqWriteEnd(ch->trueSeq[hand]);
}

using PoseFn = void (*)(void* self, uint32_t which, const vr::DriverPose_t& pose, uint32_t size);

thread_local bool t_inPose = false;

void onPose(PoseFn next, void* self, uint32_t which, const vr::DriverPose_t& pose, uint32_t size) {
  fhlive::DriverChannel* ch = g_ch.load(std::memory_order_relaxed);
  // An older host interface can pass a pose on to a newer one, which is hooked too. That pose has been moved already.
  if (!ch || t_inPose) { next(self, which, pose, size); return; }
  ch->poseUpdates.store(ch->poseUpdates.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  OffsetCache& c = t_cache;
  if (ch->seq.load(std::memory_order_acquire) != c.seq) refreshCache(ch, c);
  int hand = which == c.device[0] ? 0 : which == c.device[1] ? 1 : -1;
  if (hand >= 0) ch->handUpdates[hand].store(ch->handUpdates[hand].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  const bool known = size >= sizeof(vr::DriverPose_t);
  if (hand >= 0 && known && pose.poseIsValid && ch->wantTrue.load(std::memory_order_relaxed)) publishTrue(ch, hand, pose);
  // A driver built against a newer SDK may pass a longer struct: keep whatever comes after the part we know.
  constexpr uint32_t kMaxPose = 512;
  alignas(vr::DriverPose_t) unsigned char copy[kMaxPose];
  const vr::DriverPose_t* out = &pose;
  if (hand >= 0 && c.on[hand] && known && size <= kMaxPose) {
    memcpy(copy, &pose, size);
    vr::DriverPose_t& moved = *reinterpret_cast<vr::DriverPose_t*>(copy);
    applyOffset(moved, c.o[hand]);
    out = &moved;
  }
  t_inPose = true;
  next(self, which, *out, size);
  t_inPose = false;
}

// ---------------------------------------------------------------- the hook
// A separate hook per interface version: each has its own vtable and its own original function.
constexpr int kVersions = 3;
const char* const kHostVersions[kVersions] = {"IVRServerDriverHost_006", "IVRServerDriverHost_005", "IVRServerDriverHost_004"};
constexpr int kPoseSlot = 1;  // TrackedDevicePoseUpdated, after TrackedDeviceAdded, in all of these
PoseFn g_next[kVersions] = {nullptr, nullptr, nullptr};
void** g_vtable[kVersions] = {nullptr, nullptr, nullptr};

template <int I>
void hookPose(void* self, uint32_t which, const vr::DriverPose_t& pose, uint32_t size) {
  onPose(g_next[I], self, which, pose, size);
}
const PoseFn kHooks[kVersions] = {hookPose<0>, hookPose<1>, hookPose<2>};

void installHooks(vr::IVRDriverContext* ctx) {
  for (int i = 0; i < kVersions; ++i) {
    vr::EVRInitError e = vr::VRInitError_None;
    void* host = ctx->GetGenericInterface(kHostVersions[i], &e);
    if (!host || e != vr::VRInitError_None) { logf("%s: not offered", kHostVersions[i]); continue; }
    void** vtable = *static_cast<void***>(host);
    bool mine = false;  // two versions can share one vtable: hook it once
    for (int k = 0; k < kVersions; ++k) mine = mine || vtable[kPoseSlot] == reinterpret_cast<void*>(kHooks[k]);
    if (mine) { logf("%s: shares a hooked vtable", kHostVersions[i]); continue; }
    g_next[i] = reinterpret_cast<PoseFn>(vtable[kPoseSlot]);
    if (!writeSlot(&vtable[kPoseSlot], reinterpret_cast<void*>(kHooks[i]))) { g_next[i] = nullptr; logf("%s: hook failed (%d)", kHostVersions[i], lastError()); continue; }
    g_vtable[i] = vtable;
    logf("%s: hooked", kHostVersions[i]);
  }
}

void removeHooks() {
  for (int i = 0; i < kVersions; ++i) {
    if (g_vtable[i] && g_vtable[i][kPoseSlot] == reinterpret_cast<void*>(kHooks[i]))
      writeSlot(&g_vtable[i][kPoseSlot], reinterpret_cast<void*>(g_next[i]));
    g_vtable[i] = nullptr;
  }
}

// ---------------------------------------------------------------- buttons
// Every controller driver reports its buttons through IVRDriverInput, the same way it reports poses through the host above.
// Sitting on that call lets one button play back a recorded sequence, and lets a rewired button work with no game restart.
//
// A bug here could stick a button in every game, so the rules are:
//   - The call itself only remembers what the controller says, and passes it on unless this button's been taken over. No locks,
//     no allocation, no file I/O. Everything else happens in RunFrame, on SteamVR's own thread, and only when there's something to do.
//   - Whatever this holds down, it knows about, and lets go of the moment it's told to stand down: the dashboard opening, a hand
//     being placed, the game closing, or SteamVR shutting down. Letting go means telling SteamVR what your hand is really doing.
//   - Recording holds buttons back from SteamVR, so the overlay has to keep asking. If it dies, recording ends by itself.
using BoolCreateFn = vr::EVRInputError (*)(void*, vr::PropertyContainerHandle_t, const char*, vr::VRInputComponentHandle_t*);
using BoolUpdateFn = vr::EVRInputError (*)(void*, vr::VRInputComponentHandle_t, bool, double);
using ScalarCreateFn = vr::EVRInputError (*)(void*, vr::PropertyContainerHandle_t, const char*, vr::VRInputComponentHandle_t*, vr::EVRScalarType, vr::EVRScalarUnits);
using ScalarUpdateFn = vr::EVRInputError (*)(void*, vr::VRInputComponentHandle_t, float, double);

constexpr uint32_t kTaken = 1;  // the game doesn't get what this reports
constexpr uint32_t kRouted = 2; // and something else gets it instead

constexpr uint32_t kCompSlots = 512;
struct Comp {
  std::atomic<vr::VRInputComponentHandle_t> handle{0};
  char name[40] = {0};
  vr::PropertyContainerHandle_t container = 0;
  bool scalar = false;
  std::atomic<float> phys{0};        // what the controller said last, whoever got it
  std::atomic<uint32_t> flags{0};
  std::atomic<Comp*> route{nullptr};
  std::atomic<bool> heldBack{false}; // said something while recording that SteamVR hasn't heard
  float lastLogged = -2;             // while recording: sticks and triggers report constantly, and most of that isn't news
  // RunFrame's own.
  int holds = 0;
  bool mine = false;
};
Comp g_comp[kCompSlots];
Comp* g_compList[kCompSlots];
std::atomic<uint32_t> g_compCount{0};
std::atomic_flag g_compLock = ATOMIC_FLAG_INIT;

std::atomic<fhlive::InputChannel*> g_in{nullptr};
std::atomic<bool> g_recording{false};
std::atomic<vr::PropertyContainerHandle_t> g_handContainer[2];

uint32_t slotOf(vr::VRInputComponentHandle_t h) { return uint32_t((h * 0x9E3779B97F4A7C15ull) >> 55) % kCompSlots; }

Comp* findComp(vr::VRInputComponentHandle_t h) {
  if (!h) return nullptr;
  uint32_t i = slotOf(h);
  for (uint32_t tries = 0; tries < kCompSlots; ++tries, i = (i + 1) % kCompSlots) {
    const vr::VRInputComponentHandle_t have = g_comp[i].handle.load(std::memory_order_acquire);
    if (have == h) return &g_comp[i];
    if (!have) return nullptr;
  }
  return nullptr;
}

void addComp(vr::VRInputComponentHandle_t h, vr::PropertyContainerHandle_t container, const char* name, bool scalar) {
  if (!h || !name) return;
  while (g_compLock.test_and_set(std::memory_order_acquire)) spinPause();
  uint32_t i = slotOf(h), tries = 0;
  for (; tries < kCompSlots; ++tries, i = (i + 1) % kCompSlots) {
    const vr::VRInputComponentHandle_t have = g_comp[i].handle.load(std::memory_order_relaxed);
    if (have == h || !have) break;
  }
  if (tries < kCompSlots && !g_comp[i].handle.load(std::memory_order_relaxed)) {
    Comp& c = g_comp[i];
    snprintf(c.name, sizeof c.name, "%s", name);
    c.container = container;
    c.scalar = scalar;
    c.handle.store(h, std::memory_order_release);
    const uint32_t n = g_compCount.load(std::memory_order_relaxed);
    g_compList[n] = &c;
    g_compCount.store(n + 1, std::memory_order_release);
    if (fhlive::InputChannel* in = g_in.load(std::memory_order_relaxed)) in->components.store(n + 1, std::memory_order_relaxed);
  }
  g_compLock.clear(std::memory_order_release);
}

constexpr int kInputVersions = 5;
const char* const kInputVersionNames[kInputVersions] = {"IVRDriverInput_005", "IVRDriverInput_004", "IVRDriverInput_003", "IVRDriverInput_002", "IVRDriverInput_001"};
// The same in every version: the SDK's header stops at _003, and Valve's own drivers call _005 with the same four first.
constexpr int kSlotCreateBool = 0, kSlotUpdateBool = 1, kSlotCreateScalar = 2, kSlotUpdateScalar = 3;
BoolCreateFn g_nextCreateBool[kInputVersions];
BoolUpdateFn g_nextUpdateBool[kInputVersions];
ScalarCreateFn g_nextCreateScalar[kInputVersions];
ScalarUpdateFn g_nextUpdateScalar[kInputVersions];
void** g_inputVtable[kInputVersions];
void* g_inputSelf[kInputVersions];
int g_inputMain = -1;  // the version this sends its own presses through

// Straight to SteamVR, past the hook.
void sendRaw(Comp* c, float v, double offset = 0) {
  if (g_inputMain < 0) return;
  const vr::VRInputComponentHandle_t h = c->handle.load(std::memory_order_relaxed);
  if (c->scalar) g_nextUpdateScalar[g_inputMain](g_inputSelf[g_inputMain], h, v, offset);
  else g_nextUpdateBool[g_inputMain](g_inputSelf[g_inputMain], h, v > 0.5f, offset);
}

int handOfContainer(vr::PropertyContainerHandle_t c) {
  if (!c) return fhlive::kNoHand;
  if (c == g_handContainer[0].load(std::memory_order_relaxed)) return 0;
  if (c == g_handContainer[1].load(std::memory_order_relaxed)) return 1;
  return fhlive::kNoHand;
}

uint32_t nowMs();

// Only while recording.
void logReport(fhlive::InputChannel* in, Comp* c, float v) {
  const int hand = handOfContainer(c->container);
  if (hand == fhlive::kNoHand || strncmp(c->name, "/input/", 7) != 0 || strncmp(c->name, "/input/system", 13) == 0) return;
  if (c->scalar && std::fabs(v - c->lastLogged) < 0.02f) return;
  c->lastLogged = v;
  const uint32_t n = in->evWrite.fetch_add(1, std::memory_order_acq_rel);
  fhlive::InputEvent& e = in->events[n % fhlive::kEventRing];
  e.seq.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  e.ms = nowMs();
  e.value = v;
  e.hand = uint8_t(hand);
  snprintf(e.name, sizeof e.name, "%s", c->name);
  e.seq.store(n + 1, std::memory_order_release);
}

// Is this something that recording holds back? The Steam button never is, so you can always get out.
bool holdBackWhileRecording(const Comp* c) {
  return handOfContainer(c->container) != fhlive::kNoHand && strncmp(c->name, "/input/", 7) == 0 && strncmp(c->name, "/input/system", 13) != 0;
}

template <class Next, class Value>
vr::EVRInputError onUpdate(Next next, void* self, vr::VRInputComponentHandle_t h, Value v, double offset) {
  fhlive::InputChannel* in = g_in.load(std::memory_order_relaxed);
  if (!in) return next(self, h, v, offset);
  Comp* c = findComp(h);
  if (!c) {
    in->unknownReports.store(in->unknownReports.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return next(self, h, v, offset);
  }
  in->reports.store(in->reports.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  const float f = float(v);
  c->phys.store(f, std::memory_order_relaxed);
  if (g_recording.load(std::memory_order_relaxed) && holdBackWhileRecording(c)) {
    logReport(in, c, f);
    c->heldBack.store(true, std::memory_order_relaxed);
    return vr::VRInputError_None;
  }
  const uint32_t flags = c->flags.load(std::memory_order_acquire);
  if (!flags) return next(self, h, v, offset);
  if (flags & kRouted)
    if (Comp* to = c->route.load(std::memory_order_acquire)) sendRaw(to, f, offset);
  return vr::VRInputError_None;
}

template <int I> vr::EVRInputError hookCreateBool(void* self, vr::PropertyContainerHandle_t c, const char* name, vr::VRInputComponentHandle_t* h) {
  const vr::EVRInputError r = g_nextCreateBool[I](self, c, name, h);
  if (r == vr::VRInputError_None && h) addComp(*h, c, name, false);
  return r;
}
template <int I> vr::EVRInputError hookUpdateBool(void* self, vr::VRInputComponentHandle_t h, bool v, double t) {
  return onUpdate(g_nextUpdateBool[I], self, h, v, t);
}
template <int I> vr::EVRInputError hookCreateScalar(void* self, vr::PropertyContainerHandle_t c, const char* name, vr::VRInputComponentHandle_t* h, vr::EVRScalarType type, vr::EVRScalarUnits units) {
  const vr::EVRInputError r = g_nextCreateScalar[I](self, c, name, h, type, units);
  if (r == vr::VRInputError_None && h) addComp(*h, c, name, true);
  return r;
}
template <int I> vr::EVRInputError hookUpdateScalar(void* self, vr::VRInputComponentHandle_t h, float v, double t) {
  return onUpdate(g_nextUpdateScalar[I], self, h, v, t);
}
const BoolCreateFn kHookCreateBool[kInputVersions] = {hookCreateBool<0>, hookCreateBool<1>, hookCreateBool<2>, hookCreateBool<3>, hookCreateBool<4>};
const BoolUpdateFn kHookUpdateBool[kInputVersions] = {hookUpdateBool<0>, hookUpdateBool<1>, hookUpdateBool<2>, hookUpdateBool<3>, hookUpdateBool<4>};
const ScalarCreateFn kHookCreateScalar[kInputVersions] = {hookCreateScalar<0>, hookCreateScalar<1>, hookCreateScalar<2>, hookCreateScalar<3>, hookCreateScalar<4>};
const ScalarUpdateFn kHookUpdateScalar[kInputVersions] = {hookUpdateScalar<0>, hookUpdateScalar<1>, hookUpdateScalar<2>, hookUpdateScalar<3>, hookUpdateScalar<4>};

void installInputHooks(vr::IVRDriverContext* ctx) {
  for (int i = 0; i < kInputVersions; ++i) {
    vr::EVRInitError e = vr::VRInitError_None;
    void* input = ctx->GetGenericInterface(kInputVersionNames[i], &e);
    if (!input || e != vr::VRInitError_None) { logf("%s: not offered", kInputVersionNames[i]); continue; }
    void** vtable = *static_cast<void***>(input);
    if (!vtable) continue;
    bool shared = false;  // two versions can share one vtable: hook it once
    for (int k = 0; k < kInputVersions; ++k) shared = shared || vtable[kSlotUpdateBool] == reinterpret_cast<void*>(kHookUpdateBool[k]);
    if (shared) { logf("%s: shares a hooked vtable", kInputVersionNames[i]); continue; }
    g_nextCreateBool[i] = reinterpret_cast<BoolCreateFn>(vtable[kSlotCreateBool]);
    g_nextUpdateBool[i] = reinterpret_cast<BoolUpdateFn>(vtable[kSlotUpdateBool]);
    g_nextCreateScalar[i] = reinterpret_cast<ScalarCreateFn>(vtable[kSlotCreateScalar]);
    g_nextUpdateScalar[i] = reinterpret_cast<ScalarUpdateFn>(vtable[kSlotUpdateScalar]);
    g_inputSelf[i] = input;
    g_inputVtable[i] = vtable;
    const bool ok = writeSlot(&vtable[kSlotCreateBool], reinterpret_cast<void*>(kHookCreateBool[i])) && writeSlot(&vtable[kSlotUpdateBool], reinterpret_cast<void*>(kHookUpdateBool[i])) &&
                    writeSlot(&vtable[kSlotCreateScalar], reinterpret_cast<void*>(kHookCreateScalar[i])) && writeSlot(&vtable[kSlotUpdateScalar], reinterpret_cast<void*>(kHookUpdateScalar[i]));
    logf("%s: %s", kInputVersionNames[i], ok ? "hooked" : "hook failed");
    if (ok && g_inputMain < 0) g_inputMain = i;
  }
}

void removeInputHooks() {
  for (int i = 0; i < kInputVersions; ++i) {
    void** v = g_inputVtable[i];
    if (!v) continue;
    if (v[kSlotCreateBool] == reinterpret_cast<void*>(kHookCreateBool[i])) writeSlot(&v[kSlotCreateBool], reinterpret_cast<void*>(g_nextCreateBool[i]));
    if (v[kSlotUpdateBool] == reinterpret_cast<void*>(kHookUpdateBool[i])) writeSlot(&v[kSlotUpdateBool], reinterpret_cast<void*>(g_nextUpdateBool[i]));
    if (v[kSlotCreateScalar] == reinterpret_cast<void*>(kHookCreateScalar[i])) writeSlot(&v[kSlotCreateScalar], reinterpret_cast<void*>(g_nextCreateScalar[i]));
    if (v[kSlotUpdateScalar] == reinterpret_cast<void*>(kHookUpdateScalar[i])) writeSlot(&v[kSlotUpdateScalar], reinterpret_cast<void*>(g_nextUpdateScalar[i]));
    g_inputVtable[i] = nullptr;
  }
}

// ---------------- what RunFrame works from, built from the overlay's table
struct Set { Comp* c; float v; };
struct PlayStep {
  uint32_t delayMs = 0;
  std::vector<Set> set;
  std::vector<Comp*> letGo;
};
struct Playing {
  std::vector<PlayStep> steps;
  std::vector<Comp*> held;
  bool running = false;
  size_t at = 0;
  uint32_t due = 0;
};
struct Macro {
  Comp* source = nullptr;
  std::vector<Comp*> taken;
  bool repeat = false;
  bool was = false;
  bool fresh = false;  // this press started after it was armed, so holding it on can go round again
  Playing play;
};
struct Route { Comp* from; Comp* to; };
struct ChordIn { Comp* source; };
struct Chord {
  std::vector<Comp*> sources;
  std::vector<Set> set;
  std::vector<Comp*> held;
  bool on = false;
};
struct Compiled {
  std::vector<Macro> macros;
  std::vector<Route> routes;
  std::vector<Chord> chords;
};
Compiled g_run;
Playing g_roomPress;
bool g_armed = false;
uint32_t g_seenSeq = 1, g_seenComps = 0, g_seenRoom = 0, g_seenDevice[2] = {fhlive::kNoDevice, fhlive::kNoDevice};
bool g_seenRoomAtStart = false;
bool g_wasRecording = false;
std::chrono::steady_clock::time_point g_clockStart;

uint32_t nowMs() { return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_clockStart).count()); }
// Wraps after 49 days, so times are compared by how far apart they are.
bool reached(uint32_t now, uint32_t when) { return int32_t(now - when) >= 0; }

Comp* compAt(int hand, const char* path, const char* part) {
  if (hand != 0 && hand != 1) return nullptr;
  const vr::PropertyContainerHandle_t want = g_handContainer[hand].load(std::memory_order_relaxed);
  if (!want) return nullptr;
  char name[64];
  snprintf(name, sizeof name, "%s/%s", path, part);
  // The newest first: a controller that was switched off and on again sets everything up again.
  for (uint32_t n = g_compCount.load(std::memory_order_acquire); n-- > 0;) {
    Comp* c = g_compList[n];
    if (c->container == want && !strcmp(c->name, name)) return c;
  }
  return nullptr;
}

void takeOver(Comp* c) { c->flags.fetch_or(kTaken, std::memory_order_acq_rel); }
void hold(Comp* c, float v) {
  ++c->holds;
  takeOver(c);
  sendRaw(c, v);
}
void letGo(Comp* c) {
  if (c->holds > 0) --c->holds;
  if (c->holds > 0) return;
  if (!c->mine) c->flags.fetch_and(~kTaken, std::memory_order_acq_rel);
  sendRaw(c, c->mine ? 0.f : c->phys.load(std::memory_order_relaxed));
}

void pressInto(int hand, const char* path, std::vector<Set>& set) {
  for (const char* part : {"touch", "value", "force", "click"})
    if (Comp* c = compAt(hand, path, part)) set.push_back({c, 1.f});
}
void pressesOf(int hand, const char* path, std::vector<Comp*>& out) {
  for (const char* part : {"click", "force", "value", "touch"})
    if (Comp* c = compAt(hand, path, part)) out.push_back(c);
}

// What a macro's button says it's doing. Pushed through a trigger's pull if it has no click of its own.
Comp* sourceOf(int hand, const char* path, bool onTouch) {
  if (onTouch) return compAt(hand, path, "touch");
  if (Comp* c = compAt(hand, path, "click")) return c;
  return compAt(hand, path, "value");
}
bool isActive(const Comp* c, bool was) {
  const float v = c->phys.load(std::memory_order_relaxed);
  if (!c->scalar) return v > 0.5f;
  return was ? v > 0.4f : v > 0.75f;
}

void stopPlaying(Playing& p) {
  for (Comp* c : p.held) letGo(c);
  p.held.clear();
  p.running = false;
}

void startPlaying(Playing& p, uint32_t now) {
  p.running = true;
  p.at = 0;
  p.due = now;
}

// One step each time at most, so the game has a chance to see a press before its release.
void tickPlaying(Playing& p, uint32_t now) {
  if (!p.running) return;
  if (p.at < p.steps.size() && reached(now, p.due)) {
    const PlayStep& s = p.steps[p.at];
    for (const Set& a : s.set) {
      hold(a.c, a.v);
      p.held.push_back(a.c);
    }
    for (Comp* c : s.letGo) {
      auto it = std::find(p.held.begin(), p.held.end(), c);
      if (it == p.held.end()) continue;
      p.held.erase(it);
      letGo(c);
    }
    ++p.at;
    if (p.at < p.steps.size()) p.due = now + p.steps[p.at].delayMs;
    else p.due = now + 20;  // the last one gets a moment on screen too
    return;
  }
  if (p.at >= p.steps.size() && reached(now, p.due)) stopPlaying(p);
}

// Tell SteamVR what your hands are doing, for everything this was keeping from it.
void standDown() {
  if (!g_armed) return;
  for (Macro& m : g_run.macros) {
    stopPlaying(m.play);
    for (Comp* c : m.taken) {
      c->mine = false;
      if (c->holds == 0) {
        c->flags.fetch_and(~kTaken, std::memory_order_acq_rel);
        sendRaw(c, c->phys.load(std::memory_order_relaxed));
      }
    }
  }
  for (Chord& ch : g_run.chords) {
    for (Comp* c : ch.held) letGo(c);
    ch.held.clear();
    ch.on = false;
  }
  for (Route& r : g_run.routes) {
    r.from->route.store(nullptr, std::memory_order_release);
    r.from->flags.fetch_and(~(kTaken | kRouted), std::memory_order_acq_rel);
    sendRaw(r.from, r.from->phys.load(std::memory_order_relaxed));
    if (r.to) sendRaw(r.to, r.to->phys.load(std::memory_order_relaxed));
  }
  g_armed = false;
}

void arm() {
  if (g_armed) return;
  for (Macro& m : g_run.macros) {
    for (Comp* c : m.taken) {
      c->mine = true;
      takeOver(c);
      if (c->phys.load(std::memory_order_relaxed) != 0.f) sendRaw(c, 0.f);  // the game shouldn't be left thinking it's still held
    }
    m.was = m.source && isActive(m.source, false);  // something you're already holding has to be let go and pressed again
    m.fresh = false;
  }
  for (Route& r : g_run.routes) {
    r.from->route.store(r.to, std::memory_order_release);
    r.from->flags.fetch_or(kTaken | kRouted, std::memory_order_acq_rel);
    if (r.from->phys.load(std::memory_order_relaxed) != 0.f) sendRaw(r.from, 0.f);
  }
  g_armed = true;
}

void compile(const fhlive::InputTable& t) {
  g_run = Compiled{};
  for (uint32_t i = 0; i < t.macros && i < fhlive::kMaxMacros; ++i) {
    const fhlive::WireMacro& w = t.macro[i];
    Macro m;
    m.source = sourceOf(w.hand, w.path, w.onTouch != 0);
    if (!m.source) continue;  // that controller isn't on yet
    m.repeat = w.repeat != 0;
    if (w.onTouch) m.taken.push_back(m.source);
    else
      for (const char* part : {"click", "value", "force"})
        if (Comp* c = compAt(w.hand, w.path, part)) m.taken.push_back(c);
    for (uint32_t s = 0; s < w.count && s < fhlive::kMaxSteps; ++s) {
      const fhlive::WireStep& ws = w.steps[s];
      PlayStep step;
      step.delayMs = ws.delayMs;
      switch (ws.kind) {
        case fhlive::StepPress: pressInto(ws.hand, ws.path, step.set); break;
        case fhlive::StepRelease: pressesOf(ws.hand, ws.path, step.letGo); break;
        case fhlive::StepTouch:
          if (Comp* c = compAt(ws.hand, ws.path, "touch")) step.set.push_back({c, 1.f});
          break;
        case fhlive::StepUntouch:
          if (Comp* c = compAt(ws.hand, ws.path, "touch")) step.letGo.push_back(c);
          break;
        case fhlive::StepPush:
          if (Comp* c = compAt(ws.hand, ws.path, "touch")) step.set.push_back({c, 1.f});
          if (Comp* c = compAt(ws.hand, ws.path, "x")) step.set.push_back({c, ws.x / 100.f});
          if (Comp* c = compAt(ws.hand, ws.path, "y")) step.set.push_back({c, ws.y / 100.f});
          break;
        case fhlive::StepCentre:
          for (const char* part : {"x", "y", "touch"})
            if (Comp* c = compAt(ws.hand, ws.path, part)) step.letGo.push_back(c);
          break;
        default: break;
      }
      m.play.steps.push_back(std::move(step));
    }
    g_run.macros.push_back(std::move(m));
  }
  for (uint32_t i = 0; i < t.routes && i < fhlive::kMaxRoutes; ++i) {
    const fhlive::WireRoute& w = t.route[i];
    for (const char* part : {"click", "touch", "value", "force", "x", "y"}) {
      Comp* from = compAt(w.fromHand, w.from, part);
      if (!from) continue;
      Comp* to = w.toHand == fhlive::kNoHand ? nullptr : compAt(w.toHand, w.to, part);
      g_run.routes.push_back({from, to});
    }
  }
  for (uint32_t i = 0; i < t.chords && i < fhlive::kMaxChords; ++i) {
    const fhlive::WireChord& w = t.chord[i];
    Chord ch;
    bool complete = w.count > 0;
    for (uint32_t k = 0; k < w.count && k < fhlive::kMaxChordInputs; ++k) {
      Comp* s = sourceOf(w.in[k].hand, w.in[k].path, w.in[k].onTouch != 0);
      if (!s) complete = false;
      else ch.sources.push_back(s);
    }
    pressInto(w.targetHand, w.target, ch.set);
    if (complete && !ch.set.empty()) g_run.chords.push_back(std::move(ch));
  }
}

void readTable(fhlive::InputChannel* in, fhlive::InputTable& t, uint32_t& seq) {
  for (int tries = 0; tries < 8; ++tries) {
    const uint32_t s0 = in->tableSeq.load(std::memory_order_acquire);
    if (s0 & 1) { spinPause(); continue; }
    memcpy(&t, &in->table, sizeof t);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (in->tableSeq.load(std::memory_order_acquire) != s0) continue;
    seq = s0;
    return;
  }
}

void refreshHands(fhlive::DriverChannel* ch, bool& changed) {
  for (int h = 0; h < 2; ++h) {
    const uint32_t dev = ch->magic == fhlive::kDriverMagic ? ch->device[h] : fhlive::kNoDevice;
    if (dev == g_seenDevice[h]) continue;
    g_seenDevice[h] = dev;
    changed = true;
    g_handContainer[h].store(dev == fhlive::kNoDevice ? 0 : vr::VRPropertiesRaw()->TrackedDeviceToPropertyContainer(dev), std::memory_order_relaxed);
  }
}

// The Steam button pressed twice, the way a thumb would, which SteamVR takes as Room View's shortcut.
void startRoomPress(uint32_t now) {
  Comp* c = compAt(1, "/input/system", "click");
  if (!c) c = compAt(0, "/input/system", "click");
  if (!c) { logf("room view: no Steam button to press"); return; }
  g_roomPress = Playing{};
  for (int i = 0; i < 2; ++i) {
    PlayStep down, up;
    down.delayMs = i ? 70 : 0;
    down.set.push_back({c, 1.f});
    up.delayMs = 70;
    up.letGo.push_back(c);
    g_roomPress.steps.push_back(down);
    g_roomPress.steps.push_back(up);
  }
  startPlaying(g_roomPress, now);
}

void tickInput() {
  fhlive::InputChannel* in = g_in.load(std::memory_order_relaxed);
  fhlive::DriverChannel* ch = g_ch.load(std::memory_order_relaxed);
  if (!in || !ch) return;
  in->ticks.store(in->ticks.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  const uint32_t now = nowMs();
  in->nowMs.store(now, std::memory_order_relaxed);

  // Recording ends by itself unless the overlay keeps asking.
  const uint32_t until = in->recordUntilMs.load(std::memory_order_relaxed);
  const bool recording = until != 0 && !reached(now, until);
  if (recording != g_wasRecording) {
    g_wasRecording = recording;
    g_recording.store(recording, std::memory_order_relaxed);
    if (!recording) {
      for (uint32_t n = g_compCount.load(std::memory_order_acquire), i = 0; i < n; ++i)
        if (g_compList[i]->heldBack.exchange(false, std::memory_order_relaxed) && !g_compList[i]->flags.load(std::memory_order_relaxed)) sendRaw(g_compList[i], g_compList[i]->phys.load(std::memory_order_relaxed));
    } else {
      for (uint32_t n = g_compCount.load(std::memory_order_acquire), i = 0; i < n; ++i) g_compList[i]->lastLogged = -2;
    }
  }

  // What to do has changed: stand down from the old table first, so nothing stays held.
  const uint32_t seq = in->tableSeq.load(std::memory_order_acquire);
  const uint32_t comps = g_compCount.load(std::memory_order_acquire);
  bool handsChanged = false;
  refreshHands(ch, handsChanged);
  if (!(seq & 1) && (seq != g_seenSeq || comps != g_seenComps || handsChanged)) {
    standDown();
    static fhlive::InputTable table;
    uint32_t read = g_seenSeq;
    readTable(in, table, read);
    g_seenSeq = read;
    g_seenComps = comps;
    compile(table);
    logf("buttons: %u known, %zu macros, %zu swaps, %zu live combos", comps, g_run.macros.size(), g_run.routes.size(), g_run.chords.size());
  }

  const bool wanted = in->enabled.load(std::memory_order_relaxed) != 0 && !recording;
  if (wanted != g_armed) {
    if (wanted) arm();
    else standDown();
  }

  const uint32_t room = in->roomToggle.load(std::memory_order_relaxed);
  if (room != g_seenRoom) {
    g_seenRoom = room;
    if (!g_roomPress.running) {
      startRoomPress(now);
      in->roomToggleDone.store(room, std::memory_order_relaxed);
    }
  }
  tickPlaying(g_roomPress, now);

  if (!g_armed) return;
  for (Macro& m : g_run.macros) {
    const bool pressed = m.source && isActive(m.source, m.was);
    if (pressed && !m.was) {
      m.fresh = true;
      if (!m.play.running) {
        startPlaying(m.play, now);
        in->macrosPlayed.store(in->macrosPlayed.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      }
    }
    if (!pressed) m.fresh = false;
    tickPlaying(m.play, now);
    // Held down on a repeating one, it goes round again. Not for a press from before it was armed, like the B that just cancelled
    // placing a hand: that one has to be let go and pressed again, the same as it does for one that plays once.
    if (m.repeat && pressed && !m.play.running && m.fresh) startPlaying(m.play, now + 50);
    m.was = pressed;
  }
  for (Chord& ch2 : g_run.chords) {
    bool all = true;
    for (Comp* s : ch2.sources) all = all && isActive(s, ch2.on);
    if (all == ch2.on) continue;
    ch2.on = all;
    if (all) {
      for (const Set& a : ch2.set) {
        hold(a.c, a.v);
        ch2.held.push_back(a.c);
      }
    } else {
      for (Comp* c : ch2.held) letGo(c);
      ch2.held.clear();
    }
  }
}

void startInput(vr::IVRDriverContext* ctx, fhlive::InputChannel* in) {
  g_clockStart = std::chrono::steady_clock::now();
  // Nothing from last time counts. The overlay fills all this in once it's up.
  in->enabled.store(0, std::memory_order_relaxed);
  in->recordUntilMs.store(0, std::memory_order_relaxed);
  in->components.store(0, std::memory_order_relaxed);
  in->reports.store(0, std::memory_order_relaxed);
  in->unknownReports.store(0, std::memory_order_relaxed);
  in->macrosPlayed.store(0, std::memory_order_relaxed);
  in->ticks.store(0, std::memory_order_relaxed);
  in->evWrite.store(0, std::memory_order_relaxed);
  in->nowMs.store(0, std::memory_order_relaxed);
  g_seenRoom = in->roomToggle.load(std::memory_order_relaxed);
  in->roomToggleDone.store(g_seenRoom, std::memory_order_relaxed);
  fhlive::seqWriteBegin(in->tableSeq);
  in->table.macros = in->table.routes = in->table.chords = 0;
  fhlive::seqWriteEnd(in->tableSeq);
  g_in.store(in, std::memory_order_release);
  installInputHooks(ctx);
  if (g_inputMain >= 0) in->magic.store(fhlive::kInputMagic, std::memory_order_release);
}

void stopInput() {
  standDown();
  stopPlaying(g_roomPress);
  g_recording.store(false, std::memory_order_relaxed);
  if (fhlive::InputChannel* in = g_in.load(std::memory_order_relaxed)) in->magic.store(0, std::memory_order_release);
  removeInputHooks();
  g_in.store(nullptr, std::memory_order_release);
  g_run = Compiled{};
}

class Provider : public vr::IServerTrackedDeviceProvider {
 public:
  vr::EVRInitError Init(vr::IVRDriverContext* ctx) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
    fhlive::Shared* shm = mapShared();
    if (!shm) { logf("couldn't open live.shm (%d): offsets are off", lastError()); return vr::VRInitError_None; }
    g_ch.store(&shm->driver, std::memory_order_release);
    // Nothing is applied until the overlay says which devices are the hands this time.
    fhlive::DriverChannel* ch = &shm->driver;
    fhlive::seqWriteBegin(ch->seq);
    ch->magic = 0;
    fhlive::seqWriteEnd(ch->seq);
    fhlive::DeviceOffset none[2] = {kIdentity, kIdentity};
    publishApplied(ch, none);
    installHooks(ctx);
    startInput(ctx, &shm->input);
    logf("loaded");
    return vr::VRInitError_None;
  }
  void Cleanup() override {
    stopInput();
    removeHooks();
    closeShared();
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
  }
  const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
  void RunFrame() override { tickInput(); }
  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override {}
  void LeaveStandby() override {}
};

Provider g_provider;

}  // namespace

extern "C" FH_EXPORT void* HmdDriverFactory(const char* name, int* code) {
  if (!strcmp(name, vr::IServerTrackedDeviceProvider_Version)) return &g_provider;
  if (code) *code = vr::VRInitError_Init_InterfaceNotFound;
  return nullptr;
}

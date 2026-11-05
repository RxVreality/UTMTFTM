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

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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
bool openShared() {
  fhlive::Shared* shm = mapShared();
  if (!shm) return false;
  g_ch.store(&shm->driver, std::memory_order_release);
  return true;
}

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

class Provider : public vr::IServerTrackedDeviceProvider {
 public:
  vr::EVRInitError Init(vr::IVRDriverContext* ctx) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
    if (!openShared()) { logf("couldn't open live.shm (%d): offsets are off", lastError()); return vr::VRInitError_None; }
    // Nothing is applied until the overlay says which devices are the hands this time.
    fhlive::DriverChannel* ch = g_ch.load(std::memory_order_relaxed);
    fhlive::seqWriteBegin(ch->seq);
    ch->magic = 0;
    fhlive::seqWriteEnd(ch->seq);
    fhlive::DeviceOffset none[2] = {kIdentity, kIdentity};
    publishApplied(ch, none);
    installHooks(ctx);
    logf("loaded");
    return vr::VRInitError_None;
  }
  void Cleanup() override {
    removeHooks();
    closeShared();
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
  }
  const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
  void RunFrame() override {}
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

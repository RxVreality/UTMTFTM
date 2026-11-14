// Proves the SteamVR driver with no headset: the real library is loaded the way vrserver loads it and handed a
// fake host, so poses go through the real hook exactly as a device driver's would.
// This file can't share a translation unit with openvr.h, so it works on the channel in live.h directly.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <openvr_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "decaf.h"
#include "live.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct FakeHost : vr::IVRServerDriverHost {
  uint32_t which = 0, size = 0;
  int calls = 0;
  unsigned char last[512] = {0};
  vr::IVRServerDriverHost* passOnTo = nullptr;  // acts like an older interface handing the pose to a newer, hooked one
  bool TrackedDeviceAdded(const char*, vr::ETrackedDeviceClass, vr::ITrackedDeviceServerDriver*) override { return false; }
  void TrackedDevicePoseUpdated(uint32_t w, const vr::DriverPose_t& p, uint32_t s) override {
    if (passOnTo) {
      vr::IVRServerDriverHost* to = passOnTo;
      passOnTo = nullptr;
      to->TrackedDevicePoseUpdated(w, p, s);
      return;
    }
    which = w; size = s; ++calls;
    memcpy(last, &p, s);
  }
  void VsyncEvent(double) override {}
  void VendorSpecificEvent(uint32_t, vr::EVREventType, const vr::VREvent_Data_t&, double) override {}
  bool IsExiting() override { return false; }
  bool PollNextEvent(vr::VREvent_t*, uint32_t) override { return false; }
  void GetRawTrackedDevicePoses(float, vr::TrackedDevicePose_t*, uint32_t) override {}
  void RequestRestart(const char*, const char*, const char*, const char*) override {}
  uint32_t GetFrameTimings(vr::Compositor_FrameTiming*, uint32_t) override { return 0; }
  void SetDisplayEyeToHead(uint32_t, const vr::HmdMatrix34_t&, const vr::HmdMatrix34_t&) override {}
  void SetDisplayProjectionRaw(uint32_t, const vr::HmdRect2_t&, const vr::HmdRect2_t&) override {}
  void SetRecommendedRenderTargetSize(uint32_t, uint32_t, uint32_t) override {}
  const vr::DriverPose_t& pose() const { return *reinterpret_cast<const vr::DriverPose_t*>(last); }
};

// Where SteamVR would hear about buttons. It remembers what it was told, in order.
struct FakeInput : vr::IVRDriverInput {
  struct Heard { vr::VRInputComponentHandle_t h; float v; };
  std::mutex m;
  std::vector<Heard> heard;
  vr::VRInputComponentHandle_t next = 100;
  vr::EVRInputError CreateBooleanComponent(vr::PropertyContainerHandle_t, const char*, vr::VRInputComponentHandle_t* h) override { *h = next++; return vr::VRInputError_None; }
  vr::EVRInputError UpdateBooleanComponent(vr::VRInputComponentHandle_t h, bool v, double) override {
    std::lock_guard<std::mutex> lock(m);
    heard.push_back({h, v ? 1.f : 0.f});
    return vr::VRInputError_None;
  }
  vr::EVRInputError CreateScalarComponent(vr::PropertyContainerHandle_t, const char*, vr::VRInputComponentHandle_t* h, vr::EVRScalarType, vr::EVRScalarUnits) override { *h = next++; return vr::VRInputError_None; }
  vr::EVRInputError UpdateScalarComponent(vr::VRInputComponentHandle_t h, float v, double) override {
    std::lock_guard<std::mutex> lock(m);
    heard.push_back({h, v});
    return vr::VRInputError_None;
  }
  vr::EVRInputError CreateHapticComponent(vr::PropertyContainerHandle_t, const char*, vr::VRInputComponentHandle_t*) override { return vr::VRInputError_None; }
  vr::EVRInputError CreateSkeletonComponent(vr::PropertyContainerHandle_t, const char*, const char*, const char*, vr::EVRSkeletalTrackingLevel, const vr::VRBoneTransform_t*, uint32_t, vr::VRInputComponentHandle_t*) override { return vr::VRInputError_None; }
  vr::EVRInputError UpdateSkeletonComponent(vr::VRInputComponentHandle_t, vr::EVRSkeletalMotionRange, const vr::VRBoneTransform_t*, uint32_t) override { return vr::VRInputError_None; }
};

struct FakeProperties : vr::IVRProperties {
  vr::ETrackedPropertyError ReadPropertyBatch(vr::PropertyContainerHandle_t, vr::PropertyRead_t*, uint32_t) override { return vr::TrackedProp_Success; }
  vr::ETrackedPropertyError WritePropertyBatch(vr::PropertyContainerHandle_t, vr::PropertyWrite_t*, uint32_t) override { return vr::TrackedProp_Success; }
  const char* GetPropErrorNameFromEnum(vr::ETrackedPropertyError) override { return ""; }
  vr::PropertyContainerHandle_t TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t i) override { return 1000 + i; }
};

struct FakeContext : vr::IVRDriverContext {
  void* host = nullptr;
  void* input = nullptr;
  FakeProperties properties;
  void* GetGenericInterface(const char* v, vr::EVRInitError* e) override {
    if (e) *e = vr::VRInitError_None;
    if (!strcmp(v, "IVRServerDriverHost_006")) return host;
    if (!strcmp(v, "IVRDriverInput_005") && input) return input;
    if (!strcmp(v, vr::IVRProperties_Version)) return &properties;
    if (!strncmp(v, "IVRServerDriverHost_", 20) || !strncmp(v, "IVRDriverInput_", 15)) {  // older versions, which this PC's drivers don't need here
      if (e) *e = vr::VRInitError_Init_InvalidInterface;
      return nullptr;
    }
    static char placeholder[64];  // the SDK's setup asks for the rest up front; the driver never calls them
    return placeholder;
  }
  vr::DriverHandle_t GetDriverHandle() override { return 1; }
};

// ---------------------------------------------------------------- maths, with the same conventions as the driver
struct Q { double w, x, y, z; };
struct V { double x, y, z; };
static Q mul(Q a, Q b) {
  return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
static V rot(Q q, V v) {
  Q r = mul(mul(q, {0, v.x, v.y, v.z}), {q.w, -q.x, -q.y, -q.z});
  return {r.x, r.y, r.z};
}
static V add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
struct Rigid { Q q; V t; };
static Rigid compose(Rigid a, Rigid b) { return {mul(a.q, b.q), add(a.t, rot(a.q, b.t))}; }
static Rigid inverse(Rigid a) {
  Q c{a.q.w, -a.q.x, -a.q.y, -a.q.z};
  V t = rot(c, a.t);
  return {c, {-t.x, -t.y, -t.z}};
}
static double gapMetres(Rigid a, Rigid b) {
  return std::sqrt((a.t.x - b.t.x) * (a.t.x - b.t.x) + (a.t.y - b.t.y) * (a.t.y - b.t.y) + (a.t.z - b.t.z) * (a.t.z - b.t.z));
}
static double gapDegrees(Rigid a, Rigid b) {
  double s = std::fabs(a.q.w * b.q.w + a.q.x * b.q.x + a.q.y * b.q.y + a.q.z * b.q.z);
  return 2 * std::acos(s > 1 ? 1 : s) * 57.29578;
}
static Q fromHmd(vr::HmdQuaternion_t q) { return {q.w, q.x, q.y, q.z}; }
static Rigid devicePose(const vr::DriverPose_t& p) {
  Rigid w{fromHmd(p.qWorldFromDriverRotation), {p.vecWorldFromDriverTranslation[0], p.vecWorldFromDriverTranslation[1], p.vecWorldFromDriverTranslation[2]}};
  Rigid d{fromHmd(p.qRotation), {p.vecPosition[0], p.vecPosition[1], p.vecPosition[2]}};
  Rigid h{fromHmd(p.qDriverFromHeadRotation), {p.vecDriverFromHeadTranslation[0], p.vecDriverFromHeadTranslation[1], p.vecDriverFromHeadTranslation[2]}};
  return compose(compose(w, d), h);
}
static bool same(Rigid a, Rigid b, double eps = 1e-5) {
  double s = a.q.w * b.q.w + a.q.x * b.q.x + a.q.y * b.q.y + a.q.z * b.q.z;
  return std::fabs(std::fabs(s) - 1) < eps && std::fabs(a.t.x - b.t.x) < eps && std::fabs(a.t.y - b.t.y) < eps && std::fabs(a.t.z - b.t.z) < eps;
}

static std::mt19937 rng(23);
static double U(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }
static Q randQ() {
  Q q{U(-1, 1), U(-1, 1), U(-1, 1), U(-1, 1)};
  double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  return {q.w / n, q.x / n, q.y / n, q.z / n};
}
static vr::HmdQuaternion_t toHmd(Q q) { vr::HmdQuaternion_t h; h.w = q.w; h.x = q.x; h.y = q.y; h.z = q.z; return h; }
static vr::DriverPose_t randPose() {
  vr::DriverPose_t p{};
  p.qWorldFromDriverRotation = toHmd(randQ());
  p.qDriverFromHeadRotation = toHmd(randQ());
  p.qRotation = toHmd(randQ());
  for (int i = 0; i < 3; ++i) {
    p.vecWorldFromDriverTranslation[i] = U(-2, 2);
    p.vecDriverFromHeadTranslation[i] = U(-0.1, 0.1);
    p.vecPosition[i] = U(-2, 2);
    p.vecVelocity[i] = U(-1, 1);
    p.vecAngularVelocity[i] = U(-3, 3);
  }
  p.poseIsValid = true;
  p.deviceIsConnected = true;
  p.result = vr::TrackingResult_Running_OK;
  return p;
}
static fhlive::DeviceOffset randOffset() {
  Q q = randQ();
  return {{float(U(-0.15, 0.15)), float(U(-0.15, 0.15)), float(U(-0.15, 0.15))}, {float(q.x), float(q.y), float(q.z), float(q.w)}};
}
static Rigid asRigid(const fhlive::DeviceOffset& o) { return {{o.rot[3], o.rot[0], o.rot[1], o.rot[2]}, {o.pos[0], o.pos[1], o.pos[2]}}; }
static fhlive::DeviceOffset asOffset(Rigid r) { return {{float(r.t.x), float(r.t.y), float(r.t.z)}, {float(r.q.x), float(r.q.y), float(r.q.z), float(r.q.w)}}; }
// A device report that puts the controller at `device`, whatever the driver's own world and head transforms are.
static vr::DriverPose_t reportFor(Rigid device, Rigid world, Rigid head) {
  Rigid r = compose(compose(inverse(world), device), inverse(head));
  vr::DriverPose_t p = randPose();
  p.qWorldFromDriverRotation = toHmd(world.q);
  p.vecWorldFromDriverTranslation[0] = world.t.x; p.vecWorldFromDriverTranslation[1] = world.t.y; p.vecWorldFromDriverTranslation[2] = world.t.z;
  p.qDriverFromHeadRotation = toHmd(head.q);
  p.vecDriverFromHeadTranslation[0] = head.t.x; p.vecDriverFromHeadTranslation[1] = head.t.y; p.vecDriverFromHeadTranslation[2] = head.t.z;
  p.qRotation = toHmd(r.q);
  p.vecPosition[0] = r.t.x; p.vecPosition[1] = r.t.y; p.vecPosition[2] = r.t.z;
  return p;
}
static Q axisTurn(double x, double y, double z, double deg) {
  double h = deg / 57.29578 * 0.5, s = std::sin(h), n = std::sqrt(x * x + y * y + z * z);
  return {std::cos(h), x / n * s, y / n * s, z / n * s};
}
static const fhlive::DeviceOffset kNone = {{0, 0, 0}, {0, 0, 0, 1}};

static void publish(fhlive::DriverChannel& ch, uint32_t left, uint32_t right, const fhlive::DeviceOffset& l, const fhlive::DeviceOffset& r) {
  fhlive::seqWriteBegin(ch.seq);
  ch.magic = fhlive::kDriverMagic;
  ch.device[0] = left; ch.device[1] = right;
  ch.want[0] = l; ch.want[1] = r;
  fhlive::seqWriteEnd(ch.seq);
}

using Factory = void* (*)(const char*, int*);

#ifdef _WIN32
static Factory loadDriver() {
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::filesystem::path dll = std::filesystem::path(self).parent_path() / "driver" / "bin" / "win64" / "driver_utmtftm.dll";
  HMODULE mod = LoadLibraryW(dll.c_str());
  if (!mod) { printf("FAILED: couldn't load %s (%lu)\n", dll.string().c_str(), GetLastError()); return nullptr; }
  return reinterpret_cast<Factory>(GetProcAddress(mod, "HmdDriverFactory"));
}
static fhlive::Shared* openShared(const std::string& home) {
  HANDLE f = CreateFileW((std::filesystem::path(home) / "UTMTFTM" / "live.shm").c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return nullptr;
  HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, sizeof(fhlive::Shared), nullptr);
  return static_cast<fhlive::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(fhlive::Shared)));
}
#else
static Factory loadDriver() {
  char self[4096];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self);
  std::filesystem::path so = std::filesystem::path(std::string(self, n > 0 ? size_t(n) : 0)).parent_path() / "driver" / "bin" / "linuxarm64" / "driver_utmtftm.so";
  void* mod = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!mod) { printf("FAILED: couldn't load %s (%s)\n", so.c_str(), dlerror()); return nullptr; }
  return reinterpret_cast<Factory>(dlsym(mod, "HmdDriverFactory"));
}
static fhlive::Shared* openShared(const std::string& home) {
  int f = open((home + "/.local/share/UTMTFTM/live.shm").c_str(), O_RDWR | O_CLOEXEC);
  if (f < 0) return nullptr;
  void* v = mmap(nullptr, sizeof(fhlive::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
  close(f);
  return v == MAP_FAILED ? nullptr : static_cast<fhlive::Shared*>(v);
}
// A compiled class's vtable is read-only once it's loaded, as vrserver's are. "r--p" and the like, from the kernel.
static std::string protectionOf(const void* addr) {
  FILE* f = fopen("/proc/self/maps", "r");
  char line[512], perms[8] = "";
  while (f && fgets(line, sizeof line, f)) {
    unsigned long long lo, hi;
    char p[8];
    if (sscanf(line, "%llx-%llx %7s", &lo, &hi, p) == 3 && reinterpret_cast<uintptr_t>(addr) >= lo && reinterpret_cast<uintptr_t>(addr) < hi) { strcpy(perms, p); break; }
  }
  if (f) fclose(f);
  return perms;
}
#endif

int main() {
  namespace fs = std::filesystem;
  // A folder of its own, because the driver's file lives there and two runs at once (another copy of the code, say) would share it.
#ifdef _WIN32
  const unsigned long me = GetCurrentProcessId();
#else
  const unsigned long me = (unsigned long)getpid();
#endif
  std::string home = (fs::temp_directory_path() / ("fh-test-driver-" + std::to_string(me))).string();
  std::error_code ec;
  // The driver still has its file open when this ends, so a run can't always tidy up after itself. Older ones go here.
  for (fs::directory_iterator it(fs::temp_directory_path(), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.rfind("fh-test-driver", 0) != 0) continue;
    std::error_code e2;
    const auto when = fs::last_write_time(it->path(), e2);
    if (!e2 && fs::file_time_type::clock::now() - when > std::chrono::hours(1)) fs::remove_all(it->path(), e2);
  }
  ec.clear();
  fs::remove_all(home, ec);
  fs::create_directories(home);
#ifdef _WIN32
  _putenv_s("LOCALAPPDATA", home.c_str());
#else
  setenv("HOME", home.c_str(), 1);
#endif

  Factory factory = loadDriver();
  CHECK(factory, "vrserver looks for HmdDriverFactory");
  if (!factory) return 1;
  int code = 0;
  CHECK(!factory("IServerTrackedDeviceProvider_999", &code), "an unknown interface must be refused");
  auto* provider = static_cast<vr::IServerTrackedDeviceProvider*>(factory(vr::IServerTrackedDeviceProvider_Version, &code));
  CHECK(provider, "provider");
  if (!provider) return 1;

  FakeHost host;
  // volatile: the compiler knows a vtable never changes, and would skip it for these. The hook is exactly that change.
  vr::IVRServerDriverHost* volatile api = &host;
  void* volatile* vtable = *reinterpret_cast<void* volatile**>(&host);
  void* originalSlot = vtable[1];
#ifndef _WIN32
  const std::string protectionBefore = protectionOf(const_cast<void**>(&vtable[1]));
  CHECK(protectionBefore.size() > 1 && protectionBefore[1] == '-', "the test needs a read-only vtable to mean anything (got '%s')", protectionBefore.c_str());
#endif
  FakeInput fakeInput;
  FakeContext ctx;
  ctx.host = &host;
  ctx.input = &fakeInput;
  vr::IVRDriverInput* volatile inputApi = &fakeInput;
  void* volatile* inputVtable = *reinterpret_cast<void* volatile**>(&fakeInput);
  void* const originalInputSlots[4] = {inputVtable[0], inputVtable[1], inputVtable[2], inputVtable[3]};
  CHECK(provider->Init(&ctx) == vr::VRInitError_None, "Init");
  for (int i = 0; i < 4; ++i) CHECK(inputVtable[i] != originalInputSlots[i], "button call %d must be hooked", i);
  CHECK(vtable[1] != originalSlot, "the pose call must be hooked");
#ifndef _WIN32
  CHECK(protectionOf(const_cast<void**>(&vtable[1])) == protectionBefore, "the vtable's memory must go back to how it was");
#endif

  fhlive::Shared* shm = openShared(home);
  CHECK(shm, "the driver should have created live.shm");
  if (!shm) return 1;
  fhlive::DriverChannel& ch = shm->driver;

  // A. Before the overlay has said anything, every pose passes through untouched.
  vr::DriverPose_t p = randPose();
  uint64_t before = ch.poseUpdates.load();
  api->TrackedDevicePoseUpdated(3, p, sizeof p);
  CHECK(host.calls == 1 && host.which == 3 && host.size == sizeof p, "must reach the real function");
  CHECK(!memcmp(host.last, &p, sizeof p), "untouched before the overlay publishes");
  CHECK(ch.poseUpdates.load() == before + 1, "poses must be counted, it's how the overlay knows the driver's alive");

  // B. Only the two hand devices move, each by its own offset, and only the last transform changes.
  for (int i = 0; i < 20000; ++i) {
    fhlive::DeviceOffset l = randOffset(), r = randOffset();
    publish(ch, 3, 4, l, r);
    p = randPose();
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    CHECK(same(devicePose(host.pose()), compose(devicePose(p), asRigid(l))), "left hand (sample %d)", i);
    api->TrackedDevicePoseUpdated(4, p, sizeof p);
    CHECK(same(devicePose(host.pose()), compose(devicePose(p), asRigid(r))), "right hand (sample %d)", i);
    api->TrackedDevicePoseUpdated(0, p, sizeof p);
    CHECK(!memcmp(host.last, &p, sizeof p), "the headset must never move (sample %d)", i);
    api->TrackedDevicePoseUpdated(5, p, sizeof p);
    CHECK(!memcmp(host.last, &p, sizeof p), "other devices must not move (sample %d)", i);
  }
  {
    // Each hand's poses are counted on their own, so the overlay can tell the hook really sees the controllers.
    publish(ch, 3, 4, kNone, kNone);
    p = randPose();
    uint64_t l0 = ch.handUpdates[0].load(), r0 = ch.handUpdates[1].load();
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    CHECK(ch.handUpdates[0].load() == l0 + 1 && ch.handUpdates[1].load() == r0, "left hand counted");
    api->TrackedDevicePoseUpdated(4, p, sizeof p);
    CHECK(ch.handUpdates[0].load() == l0 + 1 && ch.handUpdates[1].load() == r0 + 1, "right hand counted");
    api->TrackedDevicePoseUpdated(0, p, sizeof p);
    api->TrackedDevicePoseUpdated(5, p, sizeof p);
    CHECK(ch.handUpdates[0].load() == l0 + 1 && ch.handUpdates[1].load() == r0 + 1, "the headset and other devices aren't hands");
  }
  {
    // What SteamVR predicts from has to stay as the device reported it.
    fhlive::DeviceOffset l = randOffset();
    publish(ch, 3, 4, l, kNone);
    p = randPose();
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    const vr::DriverPose_t& o = host.pose();
    CHECK(!memcmp(o.vecPosition, p.vecPosition, sizeof p.vecPosition) && !memcmp(o.vecVelocity, p.vecVelocity, sizeof p.vecVelocity) &&
              !memcmp(o.vecAngularVelocity, p.vecAngularVelocity, sizeof p.vecAngularVelocity) && o.poseIsValid == p.poseIsValid,
          "only the local transform may change");
    // A hand with no offset goes through bit for bit.
    api->TrackedDevicePoseUpdated(4, p, sizeof p);
    CHECK(!memcmp(host.last, &p, sizeof p), "no offset: untouched");
  }

  // C. A change shows on the next pose, and the overlay can read back what's in the poses.
  for (int i = 0; i < 2000; ++i) {
    fhlive::DeviceOffset l = randOffset(), r = randOffset();
    publish(ch, 3, 4, l, r);
    p = randPose();
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    CHECK(same(devicePose(host.pose()), compose(devicePose(p), asRigid(l))), "drag sample %d wasn't in the next pose", i);
    CHECK(!memcmp(&ch.applied[0], &l, sizeof l) && !memcmp(&ch.applied[1], &r, sizeof r), "readback sample %d", i);
  }

  // D. Controllers can swap device numbers while running.
  {
    fhlive::DeviceOffset l = randOffset();
    publish(ch, 7, 8, l, kNone);
    p = randPose();
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    CHECK(!memcmp(host.last, &p, sizeof p), "the old device must stop moving");
    api->TrackedDevicePoseUpdated(7, p, sizeof p);
    CHECK(same(devicePose(host.pose()), compose(devicePose(p), asRigid(l))), "the new device must move");
    publish(ch, 3, 4, l, kNone);
  }

  // E. A driver built on a newer SDK may pass a longer struct: the extra bytes survive.
  {
    fhlive::DeviceOffset l = randOffset();
    publish(ch, 3, 4, l, kNone);
    alignas(vr::DriverPose_t) unsigned char big[sizeof(vr::DriverPose_t) + 32];
    p = randPose();
    memcpy(big, &p, sizeof p);
    memset(big + sizeof p, 0x5A, 32);
    api->TrackedDevicePoseUpdated(3, *reinterpret_cast<vr::DriverPose_t*>(big), sizeof big);
    CHECK(host.size == sizeof big && same(devicePose(host.pose()), compose(devicePose(p), asRigid(l))), "longer struct moved");
    CHECK(host.last[sizeof p] == 0x5A && host.last[sizeof big - 1] == 0x5A, "longer struct's tail");
    api->TrackedDevicePoseUpdated(3, p, sizeof p - 8);  // shorter than we know: don't touch it
    CHECK(host.size == sizeof p - 8 && !memcmp(host.last, &p, sizeof p - 8), "shorter struct");
  }

  // F. Never a torn read: while the overlay publishes flat out, every pose has one offset or the other.
  {
    fhlive::DeviceOffset a = randOffset(), b = randOffset();
    publish(ch, 3, 4, a, kNone);
    p = randPose();
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    Rigid wa = compose(devicePose(p), asRigid(a)), wb = compose(devicePose(p), asRigid(b));
    std::atomic<bool> stop{false};
    std::thread writer([&] { while (!stop) { publish(ch, 3, 4, a, kNone); publish(ch, 3, 4, b, kNone); } });
    int torn = 0;
    for (int i = 0; i < 300000; ++i) {
      api->TrackedDevicePoseUpdated(3, p, sizeof p);
      Rigid got = devicePose(host.pose());
      if (!same(got, wa) && !same(got, wb)) ++torn;
    }
    stop = true;
    writer.join();
    CHECK(torn == 0, "%d torn reads", torn);
  }

  // G. The report of where the controllers really are, for placing: only when asked, and never with an offset in it.
  {
    fhlive::DeviceOffset l = randOffset();
    publish(ch, 3, 4, l, kNone);
    p = randPose();
    ch.wantTrue.store(0);
    uint32_t s0 = ch.trueSeq[0].load(), s1 = ch.trueSeq[1].load();
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    CHECK(ch.trueSeq[0].load() == s0, "nobody asked: no report");
    ch.wantTrue.store(1);
    for (int i = 0; i < 20000; ++i) {
      l = randOffset();
      publish(ch, 3, 4, l, kNone);
      p = randPose();
      api->TrackedDevicePoseUpdated(3, p, sizeof p);
      CHECK(same(asRigid(ch.truePose[0]), devicePose(p), 1e-4), "left report (sample %d)", i);
      CHECK(same(devicePose(host.pose()), compose(devicePose(p), asRigid(l))), "the game still gets the moved pose (sample %d)", i);
      api->TrackedDevicePoseUpdated(4, p, sizeof p);
      CHECK(same(asRigid(ch.truePose[1]), devicePose(p), 1e-4), "right report, with no offset (sample %d)", i);
    }
    s0 = ch.trueSeq[0].load(); s1 = ch.trueSeq[1].load();
    CHECK(!(s0 & 1) && !(s1 & 1) && s0 > 0, "reports finish their sequence");
    api->TrackedDevicePoseUpdated(5, p, sizeof p);
    api->TrackedDevicePoseUpdated(0, p, sizeof p);
    CHECK(ch.trueSeq[0].load() == s0 && ch.trueSeq[1].load() == s1, "other devices don't report");
    vr::DriverPose_t lost = randPose();
    lost.poseIsValid = false;
    api->TrackedDevicePoseUpdated(3, lost, sizeof lost);
    CHECK(ch.trueSeq[0].load() == s0, "a lost pose isn't a report");
    api->TrackedDevicePoseUpdated(3, p, sizeof p - 8);
    CHECK(ch.trueSeq[0].load() == s0, "a struct shorter than we know isn't read");
  }

  // H. Placing end to end through the real hook. The overlay reads the reports, works out the offset the way
  // placing does, and publishes it a few updates late. The game's grip has to stay on the grabbing hand, and the
  // hand has to settle where it was left, however late the offset arrives.
  for (int lag : {0, 1, 2, 3, 6, 12}) {
    Rigid world{randQ(), {U(-1, 1), U(-1, 1), U(-1, 1)}}, head{randQ(), {U(-0.05, 0.05), U(-0.05, 0.05), U(-0.05, 0.05)}};
    Rigid gripL{axisTurn(1, 0, 0, 2.81), {-0.0031, -0.0043, 0.0995}}, gripR{axisTurn(1, 0, 0, 2.81), {0.0031, -0.0043, 0.0995}};
    Rigid fit0{randQ(), {0.2, 1.1, -0.3}}, grabber0{randQ(), {-0.2, 1.15, -0.3}};
    Rigid start = asRigid(randOffset());  // the hand already has an offset when it's taken
    Rigid offset = start, taken{}, target{};
    std::vector<fhlive::DeviceOffset> queue;
    publish(ch, 3, 4, asOffset(compose(compose(gripL, offset), inverse(gripL))), kNone);
    double movingMetres = 0, settledMetres = 0, settledDegrees = 0;
    const int moveFor = 400, total = 700;
    for (int t = 0; t < total; ++t) {
      double a = std::min(t, moveFor) / double(moveFor);
      double w = a * (1 - a) * 4;  // out and back part way, stopping dead at the end
      Rigid fit = compose(fit0, Rigid{axisTurn(0, 1, 0, 8 * std::sin(a * 9)), {0.03 * std::sin(a * 7), 0, 0.02 * a}});
      Rigid grabber = compose(grabber0, Rigid{axisTurn(1, 0.4, -0.2, 70 * a + 10 * w), {0.25 * a + 0.05 * w, -0.12 * a, 0.2 * a}});
      vr::DriverPose_t fr = reportFor(fit, world, head), gr = reportFor(grabber, world, head);
      api->TrackedDevicePoseUpdated(3, fr, sizeof fr);
      Rigid gameGrip = compose(devicePose(host.pose()), gripL);
      api->TrackedDevicePoseUpdated(4, gr, sizeof gr);

      Rigid fitGrip = compose(asRigid(ch.truePose[0]), gripL), grabGrip = compose(asRigid(ch.truePose[1]), gripR);
      if (t == 0) {
        taken = compose(inverse(grabGrip), compose(fitGrip, offset));
        CHECK(same(gameGrip, compose(fitGrip, offset), 1e-3), "the game's grip starts where placing thinks it is");
      }
      if (t >= moveFor + lag + 2) {
        settledMetres = std::max(settledMetres, gapMetres(gameGrip, target));
        settledDegrees = std::max(settledDegrees, gapDegrees(gameGrip, target));
      } else if (t > 0) {
        movingMetres = std::max(movingMetres, gapMetres(gameGrip, target));
      }
      target = compose(grabGrip, taken);
      offset = compose(inverse(fitGrip), target);
      queue.push_back(asOffset(compose(compose(gripL, offset), inverse(gripL))));
      if ((int)queue.size() > lag) {
        publish(ch, 3, 4, queue.front(), kNone);
        queue.erase(queue.begin());
      }
    }
    printf("placing, offset %2d updates late: %.2f mm behind at worst while moving, %.3f mm and %.3f deg once still\n", lag, movingMetres * 1000, settledMetres * 1000, settledDegrees);
    CHECK(settledMetres < 0.0005 && settledDegrees < 0.15, "the hand must settle where it was left (lag %d: %.3f mm, %.3f deg)", lag, settledMetres * 1000, settledDegrees);
  }
  ch.wantTrue.store(0);

  // I. Never a torn report while the driver writes flat out.
  {
    ch.wantTrue.store(1);
    publish(ch, 3, 4, kNone, kNone);
    vr::DriverPose_t a = randPose(), b = randPose();
    Rigid ra = devicePose(a), rb = devicePose(b);
    api->TrackedDevicePoseUpdated(3, a, sizeof a);
    std::atomic<bool> stop{false};
    std::thread writer([&] { while (!stop) { api->TrackedDevicePoseUpdated(3, a, sizeof a); api->TrackedDevicePoseUpdated(3, b, sizeof b); } });
    int torn = 0, read = 0;
    for (int i = 0; i < 300000; ++i) {
      uint32_t s = ch.trueSeq[0].load(std::memory_order_acquire);
      if (s & 1) continue;
      fhlive::DeviceOffset o = ch.truePose[0];
      std::atomic_thread_fence(std::memory_order_acquire);
      if (ch.trueSeq[0].load(std::memory_order_acquire) != s) continue;
      ++read;
      if (!same(asRigid(o), ra, 1e-4) && !same(asRigid(o), rb, 1e-4)) ++torn;
    }
    stop = true;
    writer.join();
    ch.wantTrue.store(0);
    CHECK(torn == 0 && read > 0, "%d torn of %d reports", torn, read);
  }

  // J. A pose handed on from one hooked interface to another moves once, and reports the real controller.
  {
    fhlive::DeviceOffset l = randOffset();
    publish(ch, 3, 4, l, kNone);
    ch.wantTrue.store(1);
    p = randPose();
    host.passOnTo = api;
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    CHECK(!host.passOnTo, "the test's hand-on happened");
    CHECK(same(devicePose(host.pose()), compose(devicePose(p), asRigid(l))), "a pose handed on must move once, not twice");
    CHECK(same(asRigid(ch.truePose[0]), devicePose(p), 1e-4), "the report must stay the real controller");
    ch.wantTrue.store(0);
  }

  // K. Broken offsets never reach SteamVR, whatever's in the shared file.
  {
    const float nan = std::nanf(""), inf = HUGE_VALF;
    const fhlive::DeviceOffset broken[] = {
        {{nan, 0, 0}, {0, 0, 0, 1}}, {{0, inf, 0}, {0, 0, 0, 1}}, {{0, 0, 6.f}, {0, 0, 0, 1}},
        {{0.1f, 0, 0}, {nan, 0, 0, 1}}, {{0.1f, 0, 0}, {0, 0, 0, 0}}, {{0.1f, 0, 0}, {0, 0, inf, 1}}, {{0.1f, 0, 0}, {50, 50, 50, 50}},
    };
    for (const auto& b : broken) {
      publish(ch, 3, 4, b, kNone);
      p = randPose();
      api->TrackedDevicePoseUpdated(3, p, sizeof p);
      CHECK(!memcmp(host.last, &p, sizeof p), "a broken offset must leave the pose as it was");
      CHECK(!memcmp(&ch.applied[0], &kNone, sizeof kNone), "and be reported as no offset");
    }
    // A slightly off quaternion is only scaled back to length one, so a pose can't pick up a stretch.
    fhlive::DeviceOffset longer = {{0.1f, 0, 0}, {0, 0.70710678f * 1.5f, 0, 0.70710678f * 1.5f}};
    fhlive::DeviceOffset unit = {{0.1f, 0, 0}, {0, 0.70710678f, 0, 0.70710678f}};
    publish(ch, 3, 4, longer, kNone);
    p = randPose();
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    CHECK(same(devicePose(host.pose()), compose(devicePose(p), asRigid(unit))), "quaternion scaled back to length one");
  }

  // L. What it adds to each pose update.
  {
    using C = std::chrono::steady_clock;
    const int N = 5000000;
    p = randPose();
    publish(ch, 3, 4, randOffset(), kNone);
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    // The cost of the fake host alone, for subtracting. Through the saved function, not the hook.
    auto direct = [&](uint32_t which) { host.which = which; host.size = sizeof p; ++host.calls; memcpy(host.last, &p, sizeof p); };
    auto t0 = C::now();
    for (int i = 0; i < N; ++i) direct(5);
    auto t1 = C::now();
    for (int i = 0; i < N; ++i) api->TrackedDevicePoseUpdated(5, p, sizeof p);
    auto t2 = C::now();
    for (int i = 0; i < N; ++i) api->TrackedDevicePoseUpdated(3, p, sizeof p);
    auto t3 = C::now();
    ch.wantTrue.store(1);
    for (int i = 0; i < N; ++i) api->TrackedDevicePoseUpdated(3, p, sizeof p);
    auto t4 = C::now();
    ch.wantTrue.store(0);
    auto ns = [&](C::time_point a, C::time_point b) { return std::chrono::duration<double, std::nano>(b - a).count() / N; };
    double base = ns(t0, t1), other = ns(t1, t2) - base, handOn = ns(t2, t3) - base, placing = ns(t3, t4) - base;
    printf("cost per pose update: other devices %.0f ns, a hand with an offset %.0f ns, the same hand while placing %.0f ns\n", other, handOn, placing);
    CHECK(other < 2000 && handOn < 2000 && placing < 2000, "the driver has become expensive");
    double perSecond = (2 * 500 * handOn + 3 * 500 * other) / 1e9 * 100;
    printf("at 500 updates/s for 2 hands and 3 other devices, that's %.4f%% of one CPU core\n", perSecond);
  }

  // ---------------------------------------------------------------- Decaffeinate
  {
    fhlive::DecafChannel& dc = shm->decaf;
    CHECK(dc.magic.load() == fhlive::kDecafMagic, "the overlay needs to be able to see the driver can smooth");
    auto setDecaf = [&](const fhlive::DecafSettings& l, const fhlive::DecafSettings& r) {
      fhlive::seqWriteBegin(dc.seq);
      dc.want[0] = l; dc.want[1] = r;
      fhlive::seqWriteEnd(dc.seq);
    };
    auto asSettings = [](const decaf::Params& q) { return fhlive::DecafSettings{q.rotMinHz, q.rotBeta, q.posMinHz, q.posBeta, q.speedHz}; };
    const fhlive::DecafSettings off{}, strong = asSettings(decaf::preset(2));
    using C = std::chrono::steady_clock;
    auto waitMs = [](double ms) { const auto end = C::now() + std::chrono::duration<double, std::milli>(ms); while (C::now() < end) {} };
    std::normal_distribution<double> n01(0, 1);

    // A hand held still with the tracking shaking it about by about a third of a degree and a millimetre, 500 times a second.
    const Q base = randQ();
    const V basePos = {0.2, 1.3, -0.4};
    auto shaky = [&]() {
      vr::DriverPose_t s = randPose();
      const Q q = mul(axisTurn(U(-1, 1), U(-1, 1), U(-1, 1), n01(rng) * 0.35), base);
      s.qRotation = toHmd(q);
      s.vecPosition[0] = basePos.x + n01(rng) * 0.001; s.vecPosition[1] = basePos.y + n01(rng) * 0.001; s.vecPosition[2] = basePos.z + n01(rng) * 0.001;
      for (int i = 0; i < 3; ++i) { s.vecVelocity[i] = n01(rng) * 0.02; s.vecAngularVelocity[i] = n01(rng) * 0.3; s.vecAcceleration[i] = s.vecAngularAcceleration[i] = 0; }
      return s;
    };
    auto angleTo = [&](const vr::HmdQuaternion_t& h) { return gapDegrees({fromHmd(h), {0, 0, 0}}, {base, {0, 0, 0}}); };

    // Off: nothing changes, on either hand.
    publish(ch, 3, 4, kNone, kNone);
    setDecaf(off, off);
    for (uint32_t dev : {3u, 4u, 5u}) {
      p = shaky();
      api->TrackedDevicePoseUpdated(dev, p, sizeof p);
      CHECK(!memcmp(host.last, &p, sizeof p), "Decaffeinate off: device %u's pose goes through untouched", dev);
    }

    // Strong on the left hand only.
    setDecaf(strong, off);
    const uint64_t smoothed0 = dc.smoothed[0].load();
    double inSq = 0, outSq = 0, inW = 0, outW = 0;
    int n = 0;
    bool rightUntouched = true, othersUntouched = true, finite = true;
    for (int i = 0; i < 600; ++i) {
      p = shaky();
      api->TrackedDevicePoseUpdated(3, p, sizeof p);
      const vr::DriverPose_t o = host.pose();
      for (double x : o.vecPosition) finite = finite && std::isfinite(x);
      if (i >= 300) {
        const double a = angleTo(p.qRotation), b = angleTo(o.qRotation);
        inSq += a * a; outSq += b * b;
        inW += p.vecAngularVelocity[0] * p.vecAngularVelocity[0]; outW += o.vecAngularVelocity[0] * o.vecAngularVelocity[0];
        ++n;
      }
      vr::DriverPose_t r = shaky();
      api->TrackedDevicePoseUpdated(4, r, sizeof r);
      rightUntouched = rightUntouched && !memcmp(host.last, &r, sizeof r);
      api->TrackedDevicePoseUpdated(5, r, sizeof r);
      othersUntouched = othersUntouched && !memcmp(host.last, &r, sizeof r);
      waitMs(2);
    }
    const double inRms = std::sqrt(inSq / n), outRms = std::sqrt(outSq / n);
    printf("Decaffeinate Strong through the driver: a hand shaking %.3f degrees comes out shaking %.3f (%.1f times less)\n", inRms, outRms, inRms / outRms);
    CHECK(outRms < inRms / 3, "Strong takes at least two thirds of the shake out of a held-still hand (%.3f > %.3f)", inRms, outRms);
    CHECK(std::sqrt(outW / n) < std::sqrt(inW / n) / 3, "and the velocity SteamVR steps the pose on by is smoothed too, or the shake comes back");
    CHECK(rightUntouched, "the other hand, with it off, is untouched");
    CHECK(othersUntouched, "devices that aren't hands are untouched");
    CHECK(finite, "never a broken number");
    CHECK(dc.smoothed[0].load() >= smoothed0 + 590 && dc.smoothed[1].load() == 0, "the overlay can see it's working, hand by hand");
    CHECK(dc.rotRaw[0].load() > 100 && dc.rotOut[0].load() < dc.rotRaw[0].load() / 2,
          "the shake meter sees it and sees it go (%u > %u thousandths of a degree)", dc.rotRaw[0].load(), dc.rotOut[0].load());

    // It only touches the tracked pose. The hand's offset still goes on afterwards, the same as without it.
    const fhlive::DeviceOffset l = randOffset();
    publish(ch, 3, 4, l, kNone);
    p = shaky();
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    const vr::DriverPose_t both = host.pose();
    setDecaf(off, off);
    api->TrackedDevicePoseUpdated(3, p, sizeof p);
    const vr::DriverPose_t offsetOnly = host.pose();
    CHECK(!memcmp(&both.qDriverFromHeadRotation, &offsetOnly.qDriverFromHeadRotation, sizeof both.qDriverFromHeadRotation) &&
          !memcmp(both.vecDriverFromHeadTranslation, offsetOnly.vecDriverFromHeadTranslation, sizeof both.vecDriverFromHeadTranslation),
          "with both on, the offset goes on exactly as it does alone");
    CHECK(angleTo(both.qRotation) < angleTo(p.qRotation) + 0.2, "and the smoothing's still in it");
    publish(ch, 3, 4, kNone, kNone);

    // Lost tracking goes through as it came, and it starts again from the next good pose with no slide.
    setDecaf(strong, strong);
    vr::DriverPose_t lost = shaky();
    lost.poseIsValid = false;
    lost.result = vr::TrackingResult_Running_OutOfRange;
    api->TrackedDevicePoseUpdated(3, lost, sizeof lost);
    CHECK(!memcmp(host.last, &lost, sizeof lost), "a pose with no tracking goes through untouched");
    vr::DriverPose_t back = shaky();
    back.vecPosition[0] += 0.5;  // it's come back half a metre away
    api->TrackedDevicePoseUpdated(3, back, sizeof back);
    CHECK(!memcmp(host.last, &back, sizeof back), "the first pose after it comes back is where the hand really is");

    // Measuring with it off: the page can show how much you shake before you switch it on.
    setDecaf(off, off);
    dc.measure.store(1);
    dc.rotRaw[1].store(0);
    bool measuredUntouched = true;
    for (int i = 0; i < 200; ++i) {
      vr::DriverPose_t r = shaky();
      api->TrackedDevicePoseUpdated(4, r, sizeof r);
      measuredUntouched = measuredUntouched && !memcmp(host.last, &r, sizeof r);
      waitMs(2);
    }
    dc.measure.store(0);
    CHECK(measuredUntouched, "measuring changes nothing in the pose");
    CHECK(dc.rotRaw[1].load() > 100 && dc.rotRaw[1].load() == dc.rotOut[1].load(), "measuring with it off: shake seen, and nothing taken out (%u, %u)",
          dc.rotRaw[1].load(), dc.rotOut[1].load());

    // Whatever's in the file, nothing broken comes out of it.
    const float nan = std::nanf("");
    setDecaf({nan, nan, nan, nan, nan}, {1e30f, -5.f, -1.f, 1e30f, 0.f});
    bool sane = true;
    for (int i = 0; i < 50; ++i) {
      for (uint32_t dev : {3u, 4u}) {
        p = shaky();
        api->TrackedDevicePoseUpdated(dev, p, sizeof p);
        const vr::DriverPose_t o = host.pose();
        for (double x : o.vecPosition) sane = sane && std::isfinite(x);
        sane = sane && std::isfinite(o.qRotation.w) && std::isfinite(o.qRotation.x) && std::isfinite(o.vecAngularVelocity[0]);
      }
    }
    CHECK(sane, "nonsense settings in the file never make a broken pose");

    // What it adds to each pose update, with Strong on both hands.
    {
      const int N = 2000000;
      setDecaf(strong, strong);
      p = shaky();
      auto direct = [&](uint32_t which) { host.which = which; host.size = sizeof p; ++host.calls; memcpy(host.last, &p, sizeof p); };
      auto t0 = C::now();
      for (int i = 0; i < N; ++i) direct(3);
      auto t1 = C::now();
      for (int i = 0; i < N; ++i) api->TrackedDevicePoseUpdated(3, p, sizeof p);
      auto t2 = C::now();
      setDecaf(off, off);
      for (int i = 0; i < N; ++i) api->TrackedDevicePoseUpdated(3, p, sizeof p);
      auto t3 = C::now();
      auto ns = [&](C::time_point a, C::time_point b) { return std::chrono::duration<double, std::nano>(b - a).count() / N; };
      const double on = ns(t1, t2) - ns(t0, t1), offCost = ns(t2, t3) - ns(t0, t1);
      printf("Decaffeinate cost per hand pose: %.0f ns on, %.0f ns off. At 1000 poses a second on both hands, %.4f%% of one CPU core\n", on, offCost,
             2 * 1000 * on / 1e9 * 100);
      CHECK(on < 2000, "Decaffeinate has become expensive (%.0f ns a pose)", on);
    }
    setDecaf(off, off);
  }

  // ---------------------------------------------------------------- buttons
  {
    fhlive::InputChannel& in = shm->input;
    CHECK(in.magic.load() == fhlive::kInputMagic, "the overlay needs to be able to see the button hook is there");

    // Two hands, as the Frame's controllers set themselves up. Their devices are 3 and 4, which the fake says are containers 1003 and 1004.
    std::map<std::string, vr::VRInputComponentHandle_t> H;
    auto makeBool = [&](vr::PropertyContainerHandle_t c, const char* hand, const char* name) {
      vr::VRInputComponentHandle_t h = 0;
      inputApi->CreateBooleanComponent(c, name, &h);
      H[std::string(hand) + name] = h;
    };
    auto makeScalar = [&](vr::PropertyContainerHandle_t c, const char* hand, const char* name) {
      vr::VRInputComponentHandle_t h = 0;
      inputApi->CreateScalarComponent(c, name, &h, vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
      H[std::string(hand) + name] = h;
    };
    for (auto [c, hand] : {std::pair<vr::PropertyContainerHandle_t, const char*>{1003, "L"}, {1004, "R"}}) {
      for (const char* n : {"/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch", "/input/trigger/click", "/input/trigger/touch", "/input/system/click", "/input/thumbstick/touch"}) makeBool(c, hand, n);
      for (const char* n : {"/input/trigger/value", "/input/thumbstick/x", "/input/thumbstick/y"}) makeScalar(c, hand, n);
    }
    CHECK(H["R/input/b/click"] != 0, "the real SteamVR still gets to make its handles");
    CHECK(in.components.load() == H.size(), "it hears about every control being set up (%u of %zu)", in.components.load(), H.size());
    publish(ch, 3, 4, kNone, kNone);

    using Clock = std::chrono::steady_clock;
    const auto tick = [&] { provider->RunFrame(); };
    // SteamVR's main loop, at about its usual rate.
    const auto runFor = [&](int ms) {
      const auto end = Clock::now() + std::chrono::milliseconds(ms);
      while (Clock::now() < end) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    };
    const auto clearHeard = [&] { std::lock_guard<std::mutex> lock(fakeInput.m); fakeInput.heard.clear(); };
    const auto heardNow = [&] { std::lock_guard<std::mutex> lock(fakeInput.m); return fakeInput.heard; };
    const auto said = [&](const std::string& who, float v) {
      for (const auto& e : heardNow()) if (e.h == H[who] && e.v == v) return true;
      return false;
    };
    const auto indexOf = [&](const std::string& who, float v) {
      const auto all = heardNow();
      for (size_t i = 0; i < all.size(); ++i) if (all[i].h == H[who] && all[i].v == v) return int(i);
      return -1;
    };
    const auto lastSaid = [&](const std::string& who) {
      float v = -1;
      for (const auto& e : heardNow()) if (e.h == H[who]) v = e.v;
      return v;
    };
    const auto setTable = [&](const fhlive::InputTable& t) {
      fhlive::seqWriteBegin(in.tableSeq);
      memcpy(&in.table, &t, sizeof t);
      fhlive::seqWriteEnd(in.tableSeq);
    };
    const auto step = [](fhlive::WireStep& s, int hand, const char* path, int kind, int delay) {
      s = fhlive::WireStep{};
      s.hand = uint8_t(hand);
      s.kind = uint8_t(kind);
      s.delayMs = uint16_t(delay);
      snprintf(s.path, sizeof s.path, "%s", path);
    };
    tick();

    // A report goes straight through while nothing's been asked for, and the count moves.
    clearHeard();
    const uint64_t reportsBefore = in.reports.load();
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], true, 0);
    inputApi->UpdateScalarComponent(H["R/input/trigger/value"], 0.5f, 0);
    CHECK(said("R/input/a/click", 1.f) && said("R/input/trigger/value", 0.5f), "with nothing recorded, buttons reach SteamVR untouched");
    CHECK(in.reports.load() == reportsBefore + 2, "the count of reports moves, so the page knows this controller's being heard");
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], false, 0);
    inputApi->UpdateScalarComponent(H["R/input/trigger/value"], 0.f, 0);
    const uint64_t unknownBefore = in.unknownReports.load();
    inputApi->UpdateBooleanComponent(99999, true, 0);
    CHECK(in.unknownReports.load() == unknownBefore + 1 && !heardNow().empty() && heardNow().back().h == 99999, "something it never saw set up still reaches SteamVR, and gets counted");

    // The asked-for example: B plays press A, press trigger, release trigger, release A.
    static fhlive::InputTable table;
    memset(&table, 0, sizeof table);
    table.macros = 1;
    {
      fhlive::WireMacro& m = table.macro[0];
      m.hand = 1;
      snprintf(m.path, sizeof m.path, "/input/b");
      m.count = 4;
      step(m.steps[0], 1, "/input/a", fhlive::StepPress, 0);
      step(m.steps[1], 1, "/input/trigger", fhlive::StepPress, 40);
      step(m.steps[2], 1, "/input/trigger", fhlive::StepRelease, 40);
      step(m.steps[3], 1, "/input/a", fhlive::StepRelease, 40);
    }
    setTable(table);
    in.enabled.store(1);
    tick();
    clearHeard();

    inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
    CHECK(!said("R/input/b/click", 1.f), "the button that plays it doesn't reach the game");
    runFor(35);
    CHECK(in.macrosPlayed.load() == 1, "pressing it starts it");
    CHECK(said("R/input/a/click", 1.f), "A goes down straight away");
    CHECK(said("R/input/a/touch", 1.f), "and a thumb's on it, like a real press");
    CHECK(!said("R/input/trigger/click", 1.f), "the trigger waits its turn");
    runFor(200);
    const int aDown = indexOf("R/input/a/click", 1.f), trigDown = indexOf("R/input/trigger/click", 1.f), trigValue = indexOf("R/input/trigger/value", 1.f), trigUp = indexOf("R/input/trigger/click", 0.f), aUp = indexOf("R/input/a/click", 0.f);
    CHECK(aDown >= 0 && trigDown > aDown && trigUp > trigDown && aUp > trigUp, "in the order recorded: %d %d %d %d", aDown, trigDown, trigUp, aUp);
    CHECK(trigValue >= 0, "the trigger's pull goes all the way as well, for a game that reads that");
    CHECK(lastSaid("R/input/a/click") == 0.f && lastSaid("R/input/trigger/click") == 0.f && lastSaid("R/input/trigger/value") == 0.f && lastSaid("R/input/a/touch") == 0.f, "nothing's left held down");
    CHECK(!said("R/input/b/click", 1.f), "and the game never saw B");
    const uint64_t playedOnce = in.macrosPlayed.load();
    clearHeard();
    runFor(120);
    CHECK(in.macrosPlayed.load() == playedOnce && heardNow().empty(), "still holding B doesn't play it again");
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
    runFor(20);
    CHECK(heardNow().empty(), "and letting go of it says nothing");

    // Your own thumb on A while the combo's holding A: the game keeps seeing it held, then what your thumb's really doing.
    clearHeard();
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], true, 0);  // physically on it from before
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
    runFor(10);
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], false, 0);  // let go mid-combo
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], true, 0);   // and back on
    CHECK(lastSaid("R/input/a/click") == 1.f, "while it's held by the combo, your own letting go doesn't reach the game");
    runFor(200);
    CHECK(lastSaid("R/input/a/click") == 1.f, "when the combo lets go, the game hears the truth: your thumb's still on it");
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], false, 0);
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
    CHECK(lastSaid("R/input/a/click") == 0.f, "and then that it's come off");
    runFor(20);

    // The dashboard opens mid-sequence: everything's let go of at once and buttons are yours again.
    clearHeard();
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
    runFor(15);
    CHECK(said("R/input/a/click", 1.f), "started");
    in.enabled.store(0);
    tick();
    CHECK(lastSaid("R/input/a/click") == 0.f, "the dashboard opening lets go of what it's holding");
    CHECK(lastSaid("R/input/b/click") == 1.f, "and tells SteamVR what your hand's really doing");
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
    CHECK(lastSaid("R/input/b/click") == 0.f, "with the dashboard open B is just B");
    clearHeard();
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
    runFor(30);
    CHECK(said("R/input/b/click", 1.f) && !said("R/input/a/click", 1.f), "and it doesn't play");
    // Still holding it when the dashboard shuts: the game's told it's up, and it won't play until it's pressed again.
    in.enabled.store(1);
    tick();
    CHECK(lastSaid("R/input/b/click") == 0.f, "shutting the dashboard with B held tells the game it isn't");
    clearHeard();
    runFor(30);
    CHECK(!said("R/input/a/click", 1.f), "and doesn't set the combo off");
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
    runFor(10);

    // Changing what's saved while it's playing lets go too.
    clearHeard();
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
    runFor(15);
    fhlive::InputTable none;
    memset(&none, 0, sizeof none);
    setTable(none);
    tick();
    CHECK(lastSaid("R/input/a/click") == 0.f && lastSaid("R/input/b/click") == 1.f, "a new table stands down from the old one");
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
    CHECK(lastSaid("R/input/b/click") == 0.f, "and B is B again");
    setTable(table);
    tick();

    // Repeat while held.
    table.macro[0].repeat = 1;
    setTable(table);
    tick();
    const uint64_t startPlayed = in.macrosPlayed.load();
    clearHeard();
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
    runFor(600);
    int downs = 0;
    for (const auto& e : heardNow()) if (e.h == H["R/input/a/click"] && e.v == 1.f) ++downs;
    CHECK(downs >= 2, "a repeating one goes round again while it's held (%d times)", downs);
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
    runFor(250);
    CHECK(lastSaid("R/input/a/click") == 0.f, "and stops, with nothing held");
    clearHeard();
    runFor(120);
    CHECK(heardNow().empty(), "and stays stopped");
    (void)startPlayed;
    // J4, 7 November: B cancelled placing a hand, and the macro on B played anyway. Macros come back on as the placing ends, with B
    // still down, and a repeating one took that for being held. It has to be let go and pressed again, the same as any other.
    in.enabled.store(0);
    tick();
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
    runFor(10);
    in.enabled.store(1);
    tick();
    clearHeard();
    runFor(400);
    CHECK(!said("R/input/a/click", 1.f), "a press from before macros came back on doesn't start a repeating one");
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
    runFor(20);
    CHECK(!said("R/input/a/click", 1.f), "nor does letting go of it");
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
    runFor(600);
    int again = 0;
    for (const auto& e : heardNow()) if (e.h == H["R/input/a/click"] && e.v == 1.f) ++again;
    CHECK(again >= 2, "a new press plays it and goes round again (%d times)", again);
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
    runFor(250);
    CHECK(lastSaid("R/input/a/click") == 0.f, "and stops, with nothing held");
    clearHeard();
    runFor(120);
    CHECK(heardNow().empty(), "and stays stopped");
    table.macro[0].repeat = 0;

    // Resting on it plays it, and pressing it is left alone.
    table.macro[0].onTouch = 1;
    setTable(table);
    tick();
    clearHeard();
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
    CHECK(said("R/input/b/click", 1.f), "with a touch playing it, the press still reaches the game");
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
    inputApi->UpdateBooleanComponent(H["R/input/b/touch"], true, 0);
    runFor(20);
    CHECK(!said("R/input/b/touch", 1.f) && said("R/input/a/click", 1.f), "and the touch plays it instead of reaching the game");
    runFor(200);
    inputApi->UpdateBooleanComponent(H["R/input/b/touch"], false, 0);
    runFor(10);
    table.macro[0].onTouch = 0;

    // A stick pushed, and let go back to where your thumb really is.
    memset(&table, 0, sizeof table);
    table.macros = 1;
    {
      fhlive::WireMacro& m = table.macro[0];
      m.hand = 0;
      snprintf(m.path, sizeof m.path, "/input/a");
      m.count = 2;
      step(m.steps[0], 1, "/input/thumbstick", fhlive::StepPush, 0);
      m.steps[0].y = 100;
      step(m.steps[1], 1, "/input/thumbstick", fhlive::StepCentre, 40);
    }
    setTable(table);
    tick();
    clearHeard();
    inputApi->UpdateScalarComponent(H["R/input/thumbstick/x"], 0.25f, 0);
    inputApi->UpdateBooleanComponent(H["L/input/a/click"], true, 0);
    runFor(15);
    CHECK(said("R/input/thumbstick/y", 1.f), "a macro on one hand can play the other hand's stick");
    runFor(120);
    CHECK(lastSaid("R/input/thumbstick/y") == 0.f && lastSaid("R/input/thumbstick/x") == 0.25f, "and hands the stick back as your thumb has it");
    inputApi->UpdateBooleanComponent(H["L/input/a/click"], false, 0);
    inputApi->UpdateScalarComponent(H["R/input/thumbstick/x"], 0.f, 0);

    // A rewired button: A's press reaches the game as B's, straight away.
    memset(&table, 0, sizeof table);
    table.routes = 2;
    table.route[0].fromHand = 1; table.route[0].toHand = 1;
    snprintf(table.route[0].from, sizeof table.route[0].from, "/input/a");
    snprintf(table.route[0].to, sizeof table.route[0].to, "/input/b");
    table.route[1].fromHand = 0; table.route[1].toHand = fhlive::kNoHand;
    snprintf(table.route[1].from, sizeof table.route[1].from, "/input/b");
    setTable(table);
    tick();
    clearHeard();
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], true, 0);
    CHECK(said("R/input/b/click", 1.f) && !said("R/input/a/click", 1.f), "A's press arrives as B's");
    inputApi->UpdateBooleanComponent(H["R/input/a/touch"], true, 0);
    CHECK(said("R/input/b/touch", 1.f), "and so does its touch");
    inputApi->UpdateBooleanComponent(H["L/input/b/click"], true, 0);
    CHECK(!said("L/input/b/click", 1.f), "a button rewired to nothing does nothing");
    inputApi->UpdateBooleanComponent(H["L/input/b/click"], false, 0);
    in.enabled.store(0);
    tick();
    CHECK(lastSaid("R/input/b/click") == 0.f && lastSaid("R/input/a/click") == 1.f, "the dashboard opening puts A back as A and lifts B");
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], false, 0);
    inputApi->UpdateBooleanComponent(H["R/input/a/touch"], false, 0);
    in.enabled.store(1);
    tick();

    // A combo made while the game's running: two touches press a button that already does the job.
    memset(&table, 0, sizeof table);
    table.chords = 1;
    table.chord[0].count = 2;
    table.chord[0].targetHand = 1;
    snprintf(table.chord[0].target, sizeof table.chord[0].target, "/input/a");
    table.chord[0].in[0].hand = 0; table.chord[0].in[0].onTouch = 1;
    snprintf(table.chord[0].in[0].path, sizeof table.chord[0].in[0].path, "/input/a");
    table.chord[0].in[1].hand = 0; table.chord[0].in[1].onTouch = 1;
    snprintf(table.chord[0].in[1].path, sizeof table.chord[0].in[1].path, "/input/b");
    setTable(table);
    tick();
    clearHeard();
    inputApi->UpdateBooleanComponent(H["L/input/a/touch"], true, 0);
    tick();
    CHECK(!said("R/input/a/click", 1.f), "one of two isn't the combo");
    CHECK(said("L/input/a/touch", 1.f), "and the touch still reaches the game, as SteamVR's own chords let it");
    inputApi->UpdateBooleanComponent(H["L/input/b/touch"], true, 0);
    tick();
    CHECK(said("R/input/a/click", 1.f), "both is");
    inputApi->UpdateBooleanComponent(H["L/input/a/touch"], false, 0);
    tick();
    CHECK(lastSaid("R/input/a/click") == 0.f, "letting go of either ends it");
    inputApi->UpdateBooleanComponent(H["L/input/b/touch"], false, 0);
    tick();

    // Recording: everything goes to the overlay and nothing to the dashboard, then SteamVR's told the truth afterwards.
    memset(&none, 0, sizeof none);
    setTable(none);
    in.enabled.store(0);
    tick();
    clearHeard();
    const uint32_t cursorStart = in.evWrite.load();
    in.recordUntilMs.store(in.nowMs.load() + 1000);
    tick();
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], true, 0);
    inputApi->UpdateScalarComponent(H["R/input/trigger/value"], 0.9f, 0);
    inputApi->UpdateScalarComponent(H["R/input/trigger/value"], 0.905f, 0);
    inputApi->UpdateBooleanComponent(H["R/input/system/click"], true, 0);
    inputApi->UpdateBooleanComponent(H["R/input/system/click"], false, 0);
    CHECK(!said("R/input/a/click", 1.f) && !said("R/input/trigger/value", 0.9f), "while recording, the dashboard doesn't get your presses");
    CHECK(said("R/input/system/click", 1.f), "but the Steam button still works, so you can always get out");
    const uint32_t written = in.evWrite.load() - cursorStart;
    CHECK(written == 2, "it logged the A and one trigger move, not the tiny wobble or the Steam button (%u)", written);
    const fhlive::InputEvent& first = in.events[cursorStart % fhlive::kEventRing];
    CHECK(first.seq.load() == cursorStart + 1 && first.hand == 1 && first.value == 1.f && !strcmp(first.name, "/input/a/click"), "an event says who and what");
    // Steam Link says every button again about 360 times a second, changed or not, and on J4's PC that flood filled the ring
    // before the overlay could read it. A change goes straight through, and a button that hasn't changed only now and then.
    {
      const uint32_t before = in.evWrite.load();
      for (int i = 0; i < 50; ++i) inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
      CHECK(in.evWrite.load() - before == 1, "fifty reports of B up go to the overlay once (%u)", in.evWrite.load() - before);
      inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
      CHECK(in.evWrite.load() - before == 2, "B going down goes straight away");
      for (int i = 0; i < 20; ++i) inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
      CHECK(in.evWrite.load() - before == 2, "and saying it's still down doesn't, at first");
      in.recordUntilMs.store(in.nowMs.load() + 1000);
      runFor(130);
      in.recordUntilMs.store(in.nowMs.load() + 1000);
      inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
      CHECK(in.evWrite.load() - before == 3, "but a tenth of a second on it does, so the overlay knows it's held (%u)", in.evWrite.load() - before);
      inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
      CHECK(in.evWrite.load() - before == 4, "and letting go goes straight away");
    }
    in.recordUntilMs.store(0);
    tick();
    CHECK(lastSaid("R/input/a/click") == 1.f && lastSaid("R/input/trigger/value") == 0.905f, "stopping tells SteamVR what your hands are doing now");
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], false, 0);
    inputApi->UpdateScalarComponent(H["R/input/trigger/value"], 0.f, 0);

    // The overlay dies mid-recording: it isn't asking any more, so buttons come back by themselves.
    in.recordUntilMs.store(in.nowMs.load() + 60);
    tick();
    clearHeard();
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], true, 0);
    CHECK(!said("R/input/a/click", 1.f), "held back while it's recording");
    runFor(100);
    CHECK(said("R/input/a/click", 1.f), "and back without anyone asking, if the overlay stops asking");
    inputApi->UpdateBooleanComponent(H["R/input/a/click"], false, 0);
    CHECK(lastSaid("R/input/a/click") == 0.f, "back to passing straight through");

    // A controller switched off and on again sets itself up with new handles, and what's saved still works.
    memset(&table, 0, sizeof table);
    table.macros = 1;
    {
      fhlive::WireMacro& m = table.macro[0];
      m.hand = 1;
      snprintf(m.path, sizeof m.path, "/input/b");
      m.count = 1;
      step(m.steps[0], 1, "/input/a", fhlive::StepPress, 0);
    }
    setTable(table);
    in.enabled.store(1);
    tick();
    makeBool(1004, "R", "/input/b/click");
    makeBool(1004, "R", "/input/a/click");
    tick();
    clearHeard();
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], true, 0);
    CHECK(!said("R/input/b/click", 1.f), "the new button's taken over");
    runFor(15);
    CHECK(said("R/input/a/click", 1.f), "and presses the new A");
    runFor(60);
    CHECK(lastSaid("R/input/a/click") == 0.f, "a step left held at the end gets let go of");
    inputApi->UpdateBooleanComponent(H["R/input/b/click"], false, 0);
    runFor(10);

    // Controllers that aren't the hands: a tracker or the headset are left alone.
    memset(&none, 0, sizeof none);
    vr::VRInputComponentHandle_t tracker = 0;
    inputApi->CreateBooleanComponent(1009, "/input/system/click", &tracker);
    setTable(none);
    in.enabled.store(0);
    tick();
    in.recordUntilMs.store(in.nowMs.load() + 1000);
    tick();
    clearHeard();
    inputApi->UpdateBooleanComponent(tracker, true, 0);
    bool trackerThrough = false;
    for (const auto& e : heardNow()) trackerThrough = trackerThrough || e.h == tracker;
    CHECK(trackerThrough, "recording only holds back the two hands");
    in.recordUntilMs.store(0);
    tick();

    // What this adds to each button report.
    {
      using C = std::chrono::steady_clock;
      const int N = 3000000;
      const vr::VRInputComponentHandle_t h = H["R/input/thumbstick/x"];
      // Qualified, this call skips the vtable, and so the hook: that's the fake's own lock and list, to take away.
      clearHeard();
      auto t0 = C::now();
      for (int i = 0; i < N; ++i) fakeInput.FakeInput::UpdateScalarComponent(h, 0.5f, 0);
      auto t1 = C::now();
      clearHeard();
      auto t2 = C::now();
      for (int i = 0; i < N; ++i) inputApi->UpdateScalarComponent(h, 0.5f, 0);
      auto t3 = C::now();
      clearHeard();
      const double bare = std::chrono::duration<double, std::nano>(t1 - t0).count() / N, hooked = std::chrono::duration<double, std::nano>(t3 - t2).count() / N;
      printf("cost per button report: %.1f ns added (%.1f through the hook, %.1f without), with nothing recorded and no combo on that button\n", hooked - bare, hooked, bare);
      printf("at 60000 reports/s, a guess at a ceiling, that's %.4f%% of one CPU core\n", (hooked - bare) * 60000 / 1e9 * 100);
      CHECK(hooked - bare < 500, "the button hook has become expensive");
    }
    {
      std::lock_guard<std::mutex> lock(fakeInput.m);
      fakeInput.heard.clear();
      fakeInput.heard.shrink_to_fit();
    }
    in.enabled.store(0);
    tick();
  }

  // M. Shutting down leaves SteamVR as it found it.
  provider->Cleanup();
  CHECK(vtable[1] == originalSlot, "the hook must come out on Cleanup");
  CHECK(shm->decaf.magic.load() == 0, "and the overlay can see smoothing has gone with it");
  for (int i = 0; i < 4; ++i) CHECK(inputVtable[i] == originalInputSlots[i], "button call %d must come out on Cleanup", i);
  p = randPose();
  api->TrackedDevicePoseUpdated(3, p, sizeof p);
  CHECK(!memcmp(host.last, &p, sizeof p), "after Cleanup poses are untouched");

  printf("%s: %d checks, %d failures\n", fails ? "FAILED" : "PASSED", checks, fails);
  fs::remove_all(home, ec);
  return fails ? 1 : 0;
}

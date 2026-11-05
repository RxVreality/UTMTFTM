// The server half of the simulated SteamVR: loads the real driver the way vrserver does and reports poses through its
// hook at about the tracking rate, from another thread, the way a device driver would. See fake_steamvr.cpp.
#include <dlfcn.h>

#include <openvr_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "fake_vrserver.h"

namespace fakeserver {
namespace {

struct Host : vr::IVRServerDriverHost {
  std::mutex m;
  std::map<uint32_t, vr::DriverPose_t> seen;
  bool TrackedDeviceAdded(const char*, vr::ETrackedDeviceClass, vr::ITrackedDeviceServerDriver*) override { return false; }
  void TrackedDevicePoseUpdated(uint32_t which, const vr::DriverPose_t& pose, uint32_t) override {
    std::lock_guard<std::mutex> lock(m);
    seen[which] = pose;
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
};

struct Context : vr::IVRDriverContext {
  void* host = nullptr;
  void* GetGenericInterface(const char* v, vr::EVRInitError* e) override {
    if (e) *e = vr::VRInitError_None;
    if (!strcmp(v, "IVRServerDriverHost_006")) return host;
    if (!strncmp(v, "IVRServerDriverHost_", 20)) {
      if (e) *e = vr::VRInitError_Init_InvalidInterface;
      return nullptr;
    }
    static char placeholder[64];  // the SDK's setup asks for the rest up front, and the driver never calls them
    return placeholder;
  }
  vr::DriverHandle_t GetDriverHandle() override { return 1; }
};

struct Rigid {
  double q[4];  // w, x, y, z
  double t[3];
};
void qmul(const double a[4], const double b[4], double r[4]) {
  r[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
  r[1] = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
  r[2] = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
  r[3] = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
}
void qrot(const double q[4], const double v[3], double out[3]) {
  const double p[4] = {0, v[0], v[1], v[2]}, c[4] = {q[0], -q[1], -q[2], -q[3]};
  double a[4], b[4];
  qmul(q, p, a);
  qmul(a, c, b);
  out[0] = b[1]; out[1] = b[2]; out[2] = b[3];
}
Rigid compose(const Rigid& a, const Rigid& b) {
  Rigid r;
  qmul(a.q, b.q, r.q);
  qrot(a.q, b.t, r.t);
  for (int i = 0; i < 3; ++i) r.t[i] += a.t[i];
  return r;
}
Rigid inverse(const Rigid& a) {
  Rigid r;
  r.q[0] = a.q[0]; r.q[1] = -a.q[1]; r.q[2] = -a.q[2]; r.q[3] = -a.q[3];
  qrot(r.q, a.t, r.t);
  for (double& x : r.t) x = -x;
  return r;
}
vr::HmdQuaternion_t hq(const double q[4]) {
  vr::HmdQuaternion_t h;
  h.w = q[0]; h.x = q[1]; h.y = q[2]; h.z = q[3];
  return h;
}
void fromHq(const vr::HmdQuaternion_t& h, double q[4]) { q[0] = h.w; q[1] = h.x; q[2] = h.y; q[3] = h.z; }

// A device driver's own world and head transforms, deliberately not identity, because the real ones aren't.
const Rigid kWorld = {{std::cos(0.3926990817), 0, std::sin(0.3926990817), 0}, {0.4, -0.1, 0.25}};
const Rigid kHead = {{std::cos(0.2617993878), std::sin(0.2617993878), 0, 0}, {0.01, -0.02, 0.03}};

vr::DriverPose_t reportFor(const Pose& p) {
  Rigid device;
  memcpy(device.q, p.rot, sizeof device.q);
  memcpy(device.t, p.pos, sizeof device.t);
  const Rigid r = compose(compose(inverse(kWorld), device), inverse(kHead));
  vr::DriverPose_t d{};
  d.qWorldFromDriverRotation = hq(kWorld.q);
  d.qDriverFromHeadRotation = hq(kHead.q);
  d.qRotation = hq(r.q);
  for (int i = 0; i < 3; ++i) {
    d.vecWorldFromDriverTranslation[i] = kWorld.t[i];
    d.vecDriverFromHeadTranslation[i] = kHead.t[i];
    d.vecPosition[i] = r.t[i];
  }
  d.poseIsValid = true;
  d.deviceIsConnected = true;
  d.result = vr::TrackingResult_Running_OK;
  return d;
}

Host g_host;
Context g_ctx;
vr::IServerTrackedDeviceProvider* g_provider = nullptr;
std::thread g_thread;
std::atomic<bool> g_run{false};
std::mutex g_poseLock;
std::map<uint32_t, Pose> g_poses;
bool g_hooked = true;

void reportLoop() {
  // volatile: otherwise the compiler can call the function directly, and so miss the hook in the vtable.
  vr::IVRServerDriverHost* volatile api = &g_host;
  while (g_run.load()) {
    std::map<uint32_t, Pose> now;
    {
      std::lock_guard<std::mutex> lock(g_poseLock);
      now = g_poses;
    }
    for (const auto& [device, pose] : now) {
      const vr::DriverPose_t report = reportFor(pose);
      // Qualified, this call skips the vtable, and so the hook.
      if (device != 0 && !g_hooked) g_host.Host::TrackedDevicePoseUpdated(device, report, sizeof report);
      else api->TrackedDevicePoseUpdated(device, report, sizeof report);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
}

}  // namespace

bool start(const char* driverPath, bool hooked) {
  void* mod = dlopen(driverPath, RTLD_NOW | RTLD_LOCAL);
  if (!mod) { fprintf(stderr, "fake vrserver: couldn't load %s: %s\n", driverPath, dlerror()); return false; }
  using Factory = void* (*)(const char*, int*);
  auto factory = reinterpret_cast<Factory>(dlsym(mod, "HmdDriverFactory"));
  if (!factory) return false;
  int code = 0;
  g_provider = static_cast<vr::IServerTrackedDeviceProvider*>(factory(vr::IServerTrackedDeviceProvider_Version, &code));
  if (!g_provider) return false;
  g_ctx.host = static_cast<vr::IVRServerDriverHost*>(&g_host);
  if (g_provider->Init(&g_ctx) != vr::VRInitError_None) return false;
  g_hooked = hooked;
  g_run = true;
  g_thread = std::thread(reportLoop);
  return true;
}

void stop() {
  if (g_run.exchange(false)) g_thread.join();
  if (g_provider) g_provider->Cleanup();
  g_provider = nullptr;
}

void setDevicePose(uint32_t device, const Pose& pose) {
  std::lock_guard<std::mutex> lock(g_poseLock);
  g_poses[device] = pose;
}

Pose seenPose(uint32_t device) {
  vr::DriverPose_t d{};
  {
    std::lock_guard<std::mutex> lock(g_host.m);
    auto it = g_host.seen.find(device);
    if (it == g_host.seen.end()) return Pose{{0, 0, 0}, {1, 0, 0, 0}};
    d = it->second;
  }
  Rigid w, p, h;
  fromHq(d.qWorldFromDriverRotation, w.q);
  fromHq(d.qRotation, p.q);
  fromHq(d.qDriverFromHeadRotation, h.q);
  for (int i = 0; i < 3; ++i) {
    w.t[i] = d.vecWorldFromDriverTranslation[i];
    p.t[i] = d.vecPosition[i];
    h.t[i] = d.vecDriverFromHeadTranslation[i];
  }
  const Rigid r = compose(compose(w, p), h);
  Pose out;
  memcpy(out.pos, r.t, sizeof out.pos);
  memcpy(out.rot, r.q, sizeof out.rot);
  return out;
}

}  // namespace fakeserver

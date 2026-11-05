// Proves the SteamVR driver with no headset: the real DLL is loaded the way vrserver loads it and handed a
// fake host, so poses go through the real hook exactly as a device driver's would.
// This file can't share a translation unit with openvr.h, so it works on the channel in live.h directly.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <openvr_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

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

struct FakeContext : vr::IVRDriverContext {
  void* host = nullptr;
  void* GetGenericInterface(const char* v, vr::EVRInitError* e) override {
    if (e) *e = vr::VRInitError_None;
    if (!strcmp(v, "IVRServerDriverHost_006")) return host;
    if (!strncmp(v, "IVRServerDriverHost_", 20)) {  // older versions, which this PC's drivers don't need here
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
  ch.seq.fetch_add(1, std::memory_order_acq_rel);
  ch.magic = fhlive::kDriverMagic;
  ch.device[0] = left; ch.device[1] = right;
  ch.want[0] = l; ch.want[1] = r;
  ch.seq.fetch_add(1, std::memory_order_release);
}

int main() {
  namespace fs = std::filesystem;
  std::string home = (fs::temp_directory_path() / "fh-test-driver").string();
  std::error_code ec;
  fs::remove_all(home, ec);
  fs::create_directories(home);
  _putenv_s("LOCALAPPDATA", home.c_str());

  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  fs::path dll = fs::path(self).parent_path() / "driver" / "bin" / "win64" / "driver_utmtftm.dll";
  HMODULE mod = LoadLibraryW(dll.c_str());
  if (!mod) { printf("FAILED: couldn't load %s (%lu)\n", dll.string().c_str(), GetLastError()); return 1; }
  using Factory = void* (*)(const char*, int*);
  auto factory = reinterpret_cast<Factory>(GetProcAddress(mod, "HmdDriverFactory"));
  CHECK(factory, "vrserver looks for HmdDriverFactory");
  if (!factory) return 1;
  int code = 0;
  CHECK(!factory("IServerTrackedDeviceProvider_999", &code), "an unknown interface must be refused");
  auto* provider = static_cast<vr::IServerTrackedDeviceProvider*>(factory(vr::IServerTrackedDeviceProvider_Version, &code));
  CHECK(provider, "provider");
  if (!provider) return 1;

  FakeHost host;
  vr::IVRServerDriverHost* api = &host;
  void** vtable = *reinterpret_cast<void***>(&host);
  void* originalSlot = vtable[1];
  FakeContext ctx;
  ctx.host = &host;
  CHECK(provider->Init(&ctx) == vr::VRInitError_None, "Init");
  CHECK(vtable[1] != originalSlot, "the pose call must be hooked");

  HANDLE f = CreateFileW((fs::path(home) / "UTMTFTM" / "live.shm").c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(f != INVALID_HANDLE_VALUE, "the driver should have created live.shm");
  if (f == INVALID_HANDLE_VALUE) return 1;
  HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, sizeof(fhlive::Shared), nullptr);
  auto* shm = static_cast<fhlive::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(fhlive::Shared)));
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

  // M. Shutting down leaves SteamVR as it found it.
  provider->Cleanup();
  CHECK(vtable[1] == originalSlot, "the hook must come out on Cleanup");
  p = randPose();
  api->TrackedDevicePoseUpdated(3, p, sizeof p);
  CHECK(!memcmp(host.last, &p, sizeof p), "after Cleanup poses are untouched");

  printf("%s: %d checks, %d failures\n", fails ? "FAILED" : "PASSED", checks, fails);
  return fails ? 1 : 0;
}

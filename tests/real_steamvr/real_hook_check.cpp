// Tries the driver add-on's button hook, and the panel's image fix, against the real SteamVR with no headset. It runs with SteamVR's
// null headset and tests/real_steamvr/driver_testctl.cpp as a controller. It connects as a game, drawing frames and reading events, because
// games are who the add-on is for and what they see is the question. See tests/real_steamvr/run.ps1. Windows only.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi.h>

#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/live.h"

static int fails = 0, checks = 0;
static void check(bool ok, const char* what) {
  ++checks;
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  fflush(stdout);
  if (!ok) ++fails;
}
static void wait(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static fhlive::Shared* mapShared() {
  char dir[MAX_PATH];
  GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH);
  const std::string path = std::string(dir) + "\\UTMTFTM\\live.shm";
  HANDLE f = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return nullptr;
  HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, sizeof(fhlive::Shared), nullptr);
  CloseHandle(f);
  return map ? static_cast<fhlive::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(fhlive::Shared))) : nullptr;
}

static void press(uint32_t device, const char* name, float v) {
  char req[96], out[64] = {0};
  snprintf(req, sizeof req, "%s %f", name, v);
  vr::VRDebug()->DriverDebugRequest(device, req, out, sizeof out);
}

// A real button: the thumb touches it, then pushes it. That's what a controller reports, and what the add-on sends.
static void thumb(uint32_t device, const char* path, bool down) {
  const std::string p(path);
  const bool trigger = p == "/input/trigger";
  if (down) {
    press(device, (p + "/touch").c_str(), 1);
    if (trigger) press(device, (p + "/value").c_str(), 1);
    press(device, (p + "/click").c_str(), 1);
  } else {
    press(device, (p + "/click").c_str(), 0);
    if (trigger) press(device, (p + "/value").c_str(), 0);
    press(device, (p + "/touch").c_str(), 0);
  }
}

// What SteamVR itself is seeing as down. Asked through its own legacy state, which is worked out from the same reports.
static uint64_t buttonsDown(uint32_t device) {
  vr::VRControllerState_t s{};
  vr::VRSystem()->GetControllerState(device, &s, sizeof s);
  return s.ulButtonPressed;
}

static void sees(uint32_t device, uint64_t want, const char* what) {
  const uint64_t got = buttonsDown(device);
  char line[240];
  snprintf(line, sizeof line, "%s (wanted %llx, SteamVR says %llx)", what, (unsigned long long)want, (unsigned long long)got);
  check(got == want, line);
}

// A single read can land on a bad moment, and can miss a flicker. This keeps every change it sees, so anything missing, out of order or
// flickering shows up, and says when.
struct Seen { uint64_t value; int atMs; };
static std::vector<Seen> watch(uint32_t device, int forMs, int fromMs = 0) {
  std::vector<Seen> out;
  const auto start = std::chrono::steady_clock::now();
  uint64_t last = ~0ull;
  for (;;) {
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    if (ms >= forMs) break;
    const uint64_t v = buttonsDown(device);
    if (v != last) {
      out.push_back({v, fromMs + ms});
      last = v;
    }
    wait(2);
  }
  return out;
}

static std::string describe(const std::vector<Seen>& t) {
  std::string s;
  char one[64];
  for (const Seen& e : t) {
    snprintf(one, sizeof one, "%s%llx at %d ms", s.empty() ? "" : ", ", (unsigned long long)e.value, e.atMs);
    s += one;
  }
  return s;
}

// Exactly these, in this order, and nothing between them. It may start from nothing, if the first read beat the first press.
static void sawInOrder(std::vector<Seen> t, const std::vector<uint64_t>& want, const char* what) {
  const std::string saw = describe(t);
  if (!t.empty() && t.front().value == 0) t.erase(t.begin());
  bool same = t.size() == want.size();
  for (size_t i = 0; same && i < want.size(); ++i) same = t[i].value == want[i];
  char line[512];
  snprintf(line, sizeof line, "%s (SteamVR says %s)", what, saw.c_str());
  check(same, line);
}

static void addStep(fhlive::WireMacro& m, uint8_t kind, const char* path, uint16_t delayMs) {
  fhlive::WireStep& s = m.steps[m.count++];
  s.hand = 1;
  s.kind = kind;
  s.delayMs = delayMs;
  snprintf(s.path, sizeof s.path, "%s", path);
}

template <class T> static void drop(T*& x) { if (x) { x->Release(); x = nullptr; } }

// The graphics card SteamVR is using.
static IDXGIAdapter1* vrAdapter() {
  int32_t adapterIndex = 0;
  vr::VRSystem()->GetDXGIOutputInfo(&adapterIndex);
  IDXGIFactory1* factory = nullptr;
  IDXGIAdapter1* adapter = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) factory->EnumAdapters1(adapterIndex < 0 ? 0 : UINT(adapterIndex), &adapter);
  drop(factory);
  return adapter;
}

// What a game does all the time: wait for the next frame, hand one over, read its events. Connected as an overlay with none of this, a held
// button read 0 from the moment SteamVR's keyboard helper connected on 2026-11-06, and went on reading 0. The events that might say why are
// printed with the time, to compare with the button reads and vrserver.txt.
static std::atomic<bool> stopGame{false}, gameStopped{false};
static void beAGame() {
  IDXGIAdapter1* adapter = vrAdapter();
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Texture2D* tex = nullptr;
  HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
  drop(adapter);
  if (SUCCEEDED(hr)) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = 64;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = dev->CreateTexture2D(&d, nullptr, &tex);
  }
  if (FAILED(hr)) printf("note  no picture to hand SteamVR, so this game isn't drawing\n");

  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
  vr::EVRCompositorError lastError = vr::VRCompositorError_None;
  while (!stopGame.load()) {
    vr::EVRCompositorError err = vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    if (err == vr::VRCompositorError_None && tex) {
      vr::Texture_t vt{tex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
      err = vr::VRCompositor()->Submit(vr::Eye_Left, &vt);
      if (err == vr::VRCompositorError_None) err = vr::VRCompositor()->Submit(vr::Eye_Right, &vt);
    }
    if (err != lastError) printf("note  drawing frames: compositor error %d\n", int(err));
    lastError = err;
    if (err != vr::VRCompositorError_None) wait(10);

    vr::VREvent_t ev;
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
      switch (ev.eventType) {
        case vr::VREvent_Quit:
        case vr::VREvent_InputFocusChanged:
        case vr::VREvent_SceneApplicationChanged:
        case vr::VREvent_SceneApplicationStateChanged:
        case vr::VREvent_DashboardActivated:
        case vr::VREvent_DashboardDeactivated:
        case vr::VREvent_EnterStandbyMode:
        case vr::VREvent_LeaveStandbyMode:
        case vr::VREvent_ProcessConnected:
        case vr::VREvent_ProcessDisconnected: {
          SYSTEMTIME st;
          GetLocalTime(&st);
          printf("note  %02d:%02d:%02d.%03d %s, process %u\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 vr::VRSystem()->GetEventTypeNameFromEnum(vr::EVREventType(ev.eventType)), ev.data.process.pid);
          break;
        }
        default: break;
      }
    }
  }
  drop(tex);
  drop(ctx);
  drop(dev);
  gameStopped.store(true);
}

// Does what the overlay's upload does, then reads back what SteamVR is really holding for this overlay. It reads on a second device,
// because a read on the first flushes that device, which is the very thing being looked for: a copy of SteamVR's left sitting in its queue.
static void checkPanelImage() {
  IDXGIAdapter1* adapter = vrAdapter();
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Device* readDev = nullptr;
  ID3D11DeviceContext* readCtx = nullptr;
  const D3D_DRIVER_TYPE kind = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
  HRESULT hr = D3D11CreateDevice(adapter, kind, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
  HRESULT hr2 = D3D11CreateDevice(adapter, kind, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &readDev, nullptr, &readCtx);
  drop(adapter);
  if (FAILED(hr) || FAILED(hr2)) { printf("note  no D3D device, skipping the image check\n"); return; }

  vr::VROverlayHandle_t o = 0;
  vr::VROverlay()->CreateOverlay("utmtftm.imagecheck", "UTMTFTM image check", &o);
  vr::VROverlay()->SetOverlayWidthInMeters(o, 1.f);
  vr::VROverlay()->ShowOverlay(o);

  const uint32_t W = 64, H = 64;
  ID3D11Texture2D* tex[3] = {nullptr, nullptr, nullptr};
  D3D11_TEXTURE2D_DESC d{};
  d.Width = W; d.Height = H;
  d.MipLevels = 1; d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  for (auto& t : tex) dev->CreateTexture2D(&d, nullptr, &t);

  std::vector<uint8_t> px(size_t(W) * H * 4);
  int next = 0;
  const auto send = [&](uint8_t red, bool flushAfter) {
    for (size_t i = 0; i < px.size(); i += 4) { px[i] = red; px[i + 1] = 0; px[i + 2] = 0; px[i + 3] = 255; }
    ID3D11Texture2D* t = tex[next];
    next = (next + 1) % 3;
    ctx->UpdateSubresource(t, 0, nullptr, px.data(), W * 4, 0);
    ctx->Flush();
    vr::Texture_t vt{t, vr::TextureType_DirectX, vr::ColorSpace_Auto};
    vr::VROverlay()->SetOverlayTexture(o, &vt);
    if (flushAfter) ctx->Flush();
  };
  // What SteamVR is holding.
  const auto holding = [&]() -> int {
    void* view = nullptr;
    void* ref = readDev;
    uint32_t w = 0, h = 0, fmt = 0;
    vr::ETextureType api = vr::TextureType_Invalid;
    vr::EColorSpace cs = vr::ColorSpace_Auto;
    vr::VRTextureBounds_t bounds{};
    const auto e = vr::VROverlay()->GetOverlayTexture(o, &view, ref, &w, &h, &fmt, &api, &cs, &bounds);
    if (e != vr::VROverlayError_None || !view) { printf("note  GetOverlayTexture: %s\n", vr::VROverlay()->GetOverlayErrorNameFromEnum(e)); return -1; }
    ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(view);
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    int value = -1;
    if (res) {
      ID3D11Texture2D* src = nullptr;
      res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src));
      if (src) {
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        if (SUCCEEDED(readDev->CreateTexture2D(&sd, nullptr, &staging))) {
          readCtx->CopyResource(staging, src);
          D3D11_MAPPED_SUBRESOURCE map{};
          if (SUCCEEDED(readCtx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            value = static_cast<const uint8_t*>(map.pData)[0];
            readCtx->Unmap(staging, 0);
          }
          drop(staging);
        }
        drop(src);
      }
      drop(res);
    }
    vr::VROverlay()->ReleaseNativeOverlayHandle(o, view);
    return value;
  };
  const auto sameColour = [](int got, int want) { return got >= 0 && std::abs(got - want) <= 3; };

  send(10, true);
  wait(500);
  const int base = holding();
  printf("note  first image, red 10: SteamVR holds %d\n", base);
  if (base < 0) { printf("note  couldn't read the overlay back, so the image check can't say either way\n"); }
  else {
    int lateCount = 0, rightCount = 0;
    uint8_t red = 40;
    for (int i = 0; i < 6; ++i, red = uint8_t(red + 30)) {
      send(red, false);
      wait(400);
      const int got = holding();
      printf("note  no Flush after handing it over: sent %d, SteamVR holds %d\n", red, got);
      if (!sameColour(got, red)) ++lateCount;
      send(uint8_t(red + 10), true);
      wait(400);
      const int got2 = holding();
      printf("note  Flush after handing it over: sent %d, SteamVR holds %d\n", red + 10, got2);
      if (sameColour(got2, red + 10)) ++rightCount;
    }
    printf("note  without the Flush the picture was out of date %d of 6 times. With it, right %d of 6\n", lateCount, rightCount);
    check(rightCount == 6, "with the Flush, SteamVR holds the newest image every time");
  }
  vr::VROverlay()->DestroyOverlay(o);
  for (auto& t : tex) drop(t);
  drop(readCtx);
  drop(readDev);
  drop(ctx);
  drop(dev);
}

// This private config has never been through Room Setup, so SteamVR's monitor opens it once, a few seconds after starting. A game already
// connected at that moment is told to quit and killed 5 seconds later (vrserver.txt, 2026-11-06), and nothing closes Room Setup afterwards.
// So the check connects as an overlay first, closes Room Setup when it opens, and only then connects as a game.
static bool closeRoomSetup(int forMs) {
  for (int waited = 0; waited < forMs; waited += 100) {
    bool closed = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
      PROCESSENTRY32W p{};
      p.dwSize = sizeof p;
      for (BOOL more = Process32FirstW(snap, &p); more; more = Process32NextW(snap, &p)) {
        if (_wcsicmp(p.szExeFile, L"steamvr_room_setup.exe") != 0) continue;
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, p.th32ProcessID)) {
          closed = TerminateProcess(h, 0) != FALSE;
          CloseHandle(h);
        }
      }
      CloseHandle(snap);
    }
    if (closed) return true;
    wait(100);
  }
  return false;
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Overlay);
  if (e != vr::VRInitError_None) { printf("FAIL VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e)); return 2; }
  const bool roomSetupClosed = closeRoomSetup(20000);
  wait(2000);
  printf("note  Room Setup %s; the room's state is %d, universe %llu\n", roomSetupClosed ? "opened and was closed" : "didn't open",
         int(vr::VRChaperone()->GetCalibrationState()),
         static_cast<unsigned long long>(vr::VRSystem()->GetUint64TrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CurrentUniverseId_Uint64)));
  vr::VR_Shutdown();
  vr::VR_Init(&e, vr::VRApplication_Scene);
  if (e != vr::VRInitError_None) { printf("FAIL VR_Init as a game: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e)); return 2; }
  printf("note  SteamVR is up\n");
  std::thread(beAGame).detach();
  const auto finish = [] {
    stopGame.store(true);
    for (int i = 0; i < 100 && !gameStopped.load(); ++i) wait(20);
    printf("%s: %d checks, %d failures\n", fails ? "FAILED" : "PASSED", checks, fails);
    vr::VR_Shutdown();
    return fails ? 1 : 0;
  };

  uint32_t device = vr::k_unTrackedDeviceIndexInvalid;
  for (int i = 0; i < 300 && device == vr::k_unTrackedDeviceIndexInvalid; ++i) {
    device = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    if (device == vr::k_unTrackedDeviceIndexInvalid) wait(100);
  }
  check(device != vr::k_unTrackedDeviceIndexInvalid, "the test controller came up as the right hand");
  if (device == vr::k_unTrackedDeviceIndexInvalid) return 1;
  printf("note  right hand is device %u\n", device);

  fhlive::Shared* shm = mapShared();
  check(shm != nullptr, "the driver made its shared file");
  if (!shm) return 1;
  fhlive::InputChannel& in = shm->input;
  check(in.magic.load() == fhlive::kInputMagic, "the real SteamVR offered the button interface, and the hook went in");
  printf("note  %u controls set up through the hook\n", in.components.load());
  check(in.components.load() >= 9, "the controller's Create calls came through the hook with a name and a handle");

  // RunFrame is what plays everything. Is it called, for a driver with no devices of its own, and how often?
  const uint64_t ticks0 = in.ticks.load();
  wait(2000);
  const uint64_t ticks1 = in.ticks.load();
  printf("note  SteamVR called RunFrame %.0f times a second\n", (ticks1 - ticks0) / 2.0);
  check(ticks1 - ticks0 > 40, "RunFrame runs often enough to play a sequence");

  // Tell the driver which device the hand is, as the overlay does.
  fhlive::seqWriteBegin(shm->driver.seq);
  shm->driver.magic = fhlive::kDriverMagic;
  shm->driver.device[0] = fhlive::kNoDevice;
  shm->driver.device[1] = device;
  for (auto& w : shm->driver.want) { memset(&w, 0, sizeof w); w.rot[3] = 1; }
  fhlive::seqWriteEnd(shm->driver.seq);
  wait(200);

  // SteamVR works its legacy state out from a map it loads a little after the first program connects. Until then it reads nothing.
  const uint64_t r0 = in.reports.load();
  bool mapUp = false;
  const auto mapStart = std::chrono::steady_clock::now();
  for (int i = 0; i < 100 && !mapUp; ++i) {
    thumb(device, "/input/b", true);
    wait(100);
    mapUp = buttonsDown(device) != 0;
    thumb(device, "/input/b", false);
    wait(100);
  }
  printf("note  SteamVR's button map took %.1f s to show a press\n", std::chrono::duration<float>(std::chrono::steady_clock::now() - mapStart).count());
  check(mapUp, "SteamVR's legacy state shows a press, so everything below can be read from it");
  check(in.reports.load() > r0, "the controller's report came through the hook");

  // SteamVR goes on starting programs of its own for several seconds. On 2026-11-06 a read of a held button came back 0 just as its keyboard
  // helper connected. So hold one through all that, with the add-on doing nothing, to compare against vrserver.txt. Nothing else runs until it's over.
  {
    SYSTEMTIME st;
    GetLocalTime(&st);
    thumb(device, "/input/a", true);
    const std::vector<Seen> held = watch(device, 10000);
    thumb(device, "/input/a", false);
    wait(150);
    printf("note  A held through SteamVR's start-up, from %02d:%02d:%02d.%03d: %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, describe(held).c_str());
    printf("note  after that, SteamVR says input is %savailable to this program, and the game in focus is %s\n", vr::VRSystem()->IsInputAvailable() ? "" : "not ",
           vr::VRCompositor()->GetCurrentSceneFocusProcess() == GetCurrentProcessId() ? "this one" : "another one, or none");
  }

  // What each button looks like to SteamVR when nothing's changing it.
  const auto measure = [&](const char* path, const char* name) -> uint64_t {
    thumb(device, path, true);
    wait(150);
    const uint64_t down = buttonsDown(device);
    thumb(device, path, false);
    wait(150);
    printf("note  %s held: %llx. Let go: %llx\n", name, (unsigned long long)down, (unsigned long long)buttonsDown(device));
    return down;
  };
  const uint64_t seenA = measure("/input/a", "A");
  const uint64_t seenB = measure("/input/b", "B");
  const uint64_t seenTrigger = measure("/input/trigger", "the trigger");
  press(device, "/input/a/click", 1);
  wait(150);
  printf("note  A's click with no touch: %llx\n", (unsigned long long)buttonsDown(device));
  press(device, "/input/a/click", 0);
  wait(150);
  const bool canRead = seenA && seenB && seenTrigger && seenA != seenB && seenA != seenTrigger && seenB != seenTrigger;
  check(canRead, "SteamVR tells A, B and the trigger apart, so a sequence can be followed");
  // With nothing to read, every check below wanting "nothing held" would pass without meaning anything, as they did on 2026-11-06.
  if (!canRead) {
    printf("note  skipping the button checks, because they can't tell anything if SteamVR isn't showing presses\n");
    checkPanelImage();
    return finish();
  }

  // The sequence below, done by hand with the add-on doing nothing, as something to compare it to.
  const std::vector<uint64_t> order{seenA, seenA | seenTrigger, seenA, 0};
  {
    std::vector<Seen> byHand;
    int soFar = 0;
    const auto look = [&](int ms) {
      for (const Seen& s : watch(device, ms, soFar))
        if (byHand.empty() || byHand.back().value != s.value) byHand.push_back(s);
      soFar += ms;
    };
    thumb(device, "/input/a", true);
    look(200);
    thumb(device, "/input/trigger", true);
    look(300);
    thumb(device, "/input/trigger", false);
    look(300);
    thumb(device, "/input/a", false);
    look(200);
    sawInOrder(byHand, order, "by hand, SteamVR shows A, then A and the trigger together, then A, then nothing");
  }

  static fhlive::InputTable table;
  const auto publish = [&]() {
    fhlive::seqWriteBegin(in.tableSeq);
    memcpy(&in.table, &table, sizeof table);
    fhlive::seqWriteEnd(in.tableSeq);
    wait(150);
  };

  // What MMeNDtal asked for: one press of B plays press A, pull the trigger, let go of the trigger, let go of A. SteamVR should see
  // each of those in turn and never B, which stays held the whole way through.
  memset(&table, 0, sizeof table);
  table.macros = 1;
  {
    fhlive::WireMacro& m = table.macro[0];
    m.hand = 1;
    snprintf(m.path, sizeof m.path, "/input/b");
    addStep(m, fhlive::StepPress, "/input/a", 0);
    addStep(m, fhlive::StepPress, "/input/trigger", 200);
    addStep(m, fhlive::StepRelease, "/input/trigger", 300);
    addStep(m, fhlive::StepRelease, "/input/a", 300);
  }
  publish();
  in.enabled.store(1);
  wait(150);

  const uint64_t played0 = in.macrosPlayed.load();
  for (int play = 1; play <= 5; ++play) {
    thumb(device, "/input/b", true);
    const std::vector<Seen> t = watch(device, 1000);
    char what[96];
    snprintf(what, sizeof what, "play %d of B: A, then A and the trigger together, then A, then nothing, and never B", play);
    sawInOrder(t, order, what);
    thumb(device, "/input/b", false);
    wait(100);
    sees(device, 0, "and letting go of B does nothing more");
  }
  check(in.macrosPlayed.load() == played0 + 5, "each press of B played it once, on SteamVR's own loop");

  // The dashboard opening halfway through: everything's let go, and SteamVR's told what your hands are really doing.
  thumb(device, "/input/b", true);
  wait(100);
  in.enabled.store(0);
  wait(100);
  sees(device, seenB, "the dashboard opening halfway lets go of A and tells SteamVR B is held");
  thumb(device, "/input/b", false);
  wait(100);
  sees(device, 0, "and nothing's left held");

  // Recording holds everything back, and lets go by itself if nobody keeps asking.
  const uint32_t cursor = in.evWrite.load();
  in.recordUntilMs.store(in.nowMs.load() + 1000);
  wait(100);
  thumb(device, "/input/a", true);
  wait(150);
  sees(device, 0, "while recording, SteamVR doesn't get A");
  check(in.evWrite.load() != cursor, "the overlay does");
  wait(1200);
  sees(device, seenA, "and when nobody asks any more, SteamVR's told A is held");
  thumb(device, "/input/a", false);
  wait(150);
  sees(device, 0, "and let go");

  // A rewired button, with no restart: A reaches SteamVR as B.
  memset(&table, 0, sizeof table);
  table.routes = 1;
  table.route[0].fromHand = 1;
  table.route[0].toHand = 1;
  snprintf(table.route[0].from, sizeof table.route[0].from, "/input/a");
  snprintf(table.route[0].to, sizeof table.route[0].to, "/input/b");
  publish();
  in.enabled.store(1);
  wait(150);
  thumb(device, "/input/a", true);
  wait(150);
  sees(device, seenB, "a rewired button: A reaches SteamVR as B, straight away");
  thumb(device, "/input/a", false);
  wait(150);
  sees(device, 0, "and lets go");

  // A button set to do nothing.
  table.route[0].toHand = fhlive::kNoHand;
  publish();
  wait(150);
  thumb(device, "/input/a", true);
  wait(150);
  sees(device, 0, "a button set to nothing reaches nothing");
  thumb(device, "/input/a", false);
  wait(100);

  // A combo of touches, played by the driver rather than SteamVR's own chords.
  memset(&table, 0, sizeof table);
  table.chords = 1;
  table.chord[0].count = 2;
  table.chord[0].targetHand = 1;
  snprintf(table.chord[0].target, sizeof table.chord[0].target, "/input/b");
  table.chord[0].in[0].hand = 1;
  table.chord[0].in[0].onTouch = 1;
  snprintf(table.chord[0].in[0].path, sizeof table.chord[0].in[0].path, "/input/a");
  table.chord[0].in[1].hand = 1;
  table.chord[0].in[1].onTouch = 1;
  snprintf(table.chord[0].in[1].path, sizeof table.chord[0].in[1].path, "/input/trigger");
  publish();
  wait(150);
  press(device, "/input/a/touch", 1);
  wait(150);
  sees(device, 0, "a combo of touches: one touch alone does nothing");
  press(device, "/input/trigger/touch", 1);
  wait(150);
  sees(device, seenB, "both touches press B");
  press(device, "/input/a/touch", 0);
  wait(150);
  sees(device, 0, "taking one off lets go");
  press(device, "/input/trigger/touch", 0);
  wait(100);

  // A sequence set off by a touch.
  memset(&table, 0, sizeof table);
  table.macros = 1;
  {
    fhlive::WireMacro& m = table.macro[0];
    m.hand = 1;
    m.onTouch = 1;
    snprintf(m.path, sizeof m.path, "/input/a");
    addStep(m, fhlive::StepPress, "/input/b", 0);
    addStep(m, fhlive::StepRelease, "/input/b", 300);
  }
  publish();
  wait(150);
  const uint64_t played1 = in.macrosPlayed.load();
  press(device, "/input/a/touch", 1);
  wait(100);
  check(in.macrosPlayed.load() == played1 + 1, "touching A started a sequence");
  sees(device, seenB, "which presses B");
  wait(400);
  sees(device, 0, "and lets go");
  press(device, "/input/a/touch", 0);
  wait(100);

  in.enabled.store(0);
  memset(&table, 0, sizeof table);
  publish();

  // Room View's shortcut, sent through the same hook. Nothing to see with no camera, but SteamVR takes the presses.
  const uint32_t room0 = in.roomToggleDone.load();
  in.roomToggle.fetch_add(1);
  wait(500);
  check(in.roomToggleDone.load() != room0, "the Steam button got double-pressed");

  checkPanelImage();
  return finish();
}

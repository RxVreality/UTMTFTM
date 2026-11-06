// Tries the driver add-on's button hook, and the panel's image fix, against the real SteamVR with no headset. It runs with SteamVR's
// null headset and tests/real_steamvr/driver_testctl.cpp as a controller. See tests/real_steamvr/run.ps1. Windows only.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <openvr.h>

#include <algorithm>
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

// Is SteamVR itself seeing this button down? Asked through its own legacy state, which is worked out from the same reports.
static uint64_t buttonsDown(uint32_t device) {
  vr::VRControllerState_t s{};
  vr::VRSystem()->GetControllerState(device, &s, sizeof s);
  return s.ulButtonPressed;
}

template <class T> static void drop(T*& x) { if (x) { x->Release(); x = nullptr; } }

// Does what the overlay's upload does, then reads back what SteamVR is really holding for this overlay.
static void checkPanelImage() {
  int32_t adapterIndex = 0;
  vr::VRSystem()->GetDXGIOutputInfo(&adapterIndex);
  IDXGIFactory1* factory = nullptr;
  IDXGIAdapter1* adapter = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) factory->EnumAdapters1(adapterIndex < 0 ? 0 : UINT(adapterIndex), &adapter);
  drop(factory);
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
  drop(adapter);
  if (FAILED(hr)) { printf("note  no D3D device, skipping the image check\n"); return; }

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
  // What SteamVR is holding, read back on this device.
  const auto holding = [&]() -> int {
    void* view = nullptr;
    void* ref = dev;
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
        if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &staging))) {
          ctx->CopyResource(staging, src);
          D3D11_MAPPED_SUBRESOURCE map{};
          if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            value = static_cast<const uint8_t*>(map.pData)[0];
            ctx->Unmap(staging, 0);
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
  vr::VR_Shutdown;  // nothing else on this device matters after this
  for (auto& t : tex) drop(t);
  drop(ctx);
  drop(dev);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Overlay);
  if (e != vr::VRInitError_None) { printf("FAIL VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e)); return 2; }
  printf("note  SteamVR is up\n");

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

  // A report goes through the hook and on to SteamVR.
  const uint64_t r0 = in.reports.load();
  press(device, "/input/a/click", 1);
  wait(150);
  const uint64_t downA = buttonsDown(device);
  check(in.reports.load() > r0, "the controller's report came through the hook");
  printf("note  with A held, SteamVR's own state says %llx\n", (unsigned long long)downA);
  press(device, "/input/a/click", 0);
  wait(150);
  const uint64_t upA = buttonsDown(device);
  printf("note  with A let go, %llx\n", (unsigned long long)upA);
  check(downA != upA, "and on to SteamVR, which sees A go down and come up");

  press(device, "/input/b/click", 1);
  wait(150);
  const uint64_t downB = buttonsDown(device);
  press(device, "/input/b/click", 0);
  wait(150);
  printf("note  with B held, SteamVR's own state says %llx\n", (unsigned long long)downB);

  // The real thing: B plays press A, wait, release A. SteamVR should see A, and never B.
  static fhlive::InputTable table;
  memset(&table, 0, sizeof table);
  table.macros = 1;
  fhlive::WireMacro& m = table.macro[0];
  m.hand = 1;
  snprintf(m.path, sizeof m.path, "/input/b");
  m.count = 2;
  m.steps[0].hand = 1;
  m.steps[0].kind = fhlive::StepPress;
  snprintf(m.steps[0].path, sizeof m.steps[0].path, "/input/a");
  m.steps[1].hand = 1;
  m.steps[1].kind = fhlive::StepRelease;
  m.steps[1].delayMs = 300;
  snprintf(m.steps[1].path, sizeof m.steps[1].path, "/input/a");
  fhlive::seqWriteBegin(in.tableSeq);
  memcpy(&in.table, &table, sizeof table);
  fhlive::seqWriteEnd(in.tableSeq);
  in.enabled.store(1);
  wait(200);

  const uint64_t played0 = in.macrosPlayed.load();
  press(device, "/input/b/click", 1);
  wait(150);
  const uint64_t during = buttonsDown(device);
  check(in.macrosPlayed.load() == played0 + 1, "pressing B started the sequence, running on SteamVR's own loop");
  check(during == downA, "and SteamVR sees A held, sent by the add-on, and not B");
  wait(400);
  const uint64_t after = buttonsDown(device);
  check(after == upA, "then sees A let go, with nothing left held");
  press(device, "/input/b/click", 0);
  wait(100);

  // The dashboard opening hands buttons back.
  in.enabled.store(0);
  wait(100);
  press(device, "/input/b/click", 1);
  wait(150);
  check(buttonsDown(device) == downB, "with the dashboard open, B is B again");
  press(device, "/input/b/click", 0);
  wait(100);

  // Recording holds everything back, and lets go by itself if nobody keeps asking.
  const uint32_t cursor = in.evWrite.load();
  in.recordUntilMs.store(in.nowMs.load() + 1000);
  wait(100);
  press(device, "/input/a/click", 1);
  wait(150);
  check(buttonsDown(device) == upA, "while recording, SteamVR doesn't get A");
  check(in.evWrite.load() != cursor, "the overlay does");
  wait(1200);
  check(buttonsDown(device) == downA, "and when nobody asks any more, SteamVR's told A is held");
  press(device, "/input/a/click", 0);
  wait(150);
  check(buttonsDown(device) == upA, "and let go");

  // Room View's shortcut, sent through the same hook. Nothing to see with no camera, but SteamVR takes the presses.
  const uint32_t room0 = in.roomToggleDone.load();
  in.roomToggle.fetch_add(1);
  wait(500);
  check(in.roomToggleDone.load() != room0, "the Steam button got double-pressed");

  checkPanelImage();

  printf("%s: %d checks, %d failures\n", fails ? "FAILED" : "PASSED", checks, fails);
  vr::VR_Shutdown();
  return fails ? 1 : 0;
}

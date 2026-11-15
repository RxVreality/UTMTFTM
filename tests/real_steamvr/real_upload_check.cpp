// Tries the overlay picture hand-over (src/d3dupload.cpp) against the real SteamVR with its null headset, first with the graphics card idle,
// then with a pretend heavy game keeping the card busy, which is what Pavlov did on J4's PC (2026-11-14: a bubble picture took 18-54 ms
// there, 6 ms in a light game, and the whole program stalled up to 2.3 s). It hands over pictures the way the program does, 60 a second,
// the old way (TextureType_DirectX: SteamVR copies on our device and we wait) and the new way (shared textures, never waiting), and
// measures what each costs the loop, how long each picture takes to reach SteamVR, and whether SteamVR ends up holding the newest one.
// Started by run.ps1 -Check real_upload_check. Windows only. The pretend game is a thread with its own D3D11 device, so the PC's
// GPU guard keeps it along with the SteamVR this program started.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/overlay_upload.h"
#include "../../src/ui.h"

using Clock = std::chrono::steady_clock;
static int fails = 0, checks = 0;
static void check(bool ok, const char* what) {
  ++checks;
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++fails;
}
static void wait(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
template <class T> static void drop(T*& x) { if (x) { x->Release(); x = nullptr; } }
static double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
// What the program's platform::sleepMs does: a high-resolution timer, since Sleep(1) can take 15 ms.
static void napMs(int ms) {
  static HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  LARGE_INTEGER due;
  due.QuadPart = -10000LL * ms;
  if (t && SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(t, INFINITE);
  else Sleep(DWORD(ms));
}
static double threadCpuMs() {
  FILETIME c, e, k, u;
  GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
  const auto v = [](const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
  return v(k) + v(u);
}

static IDXGIAdapter1* vrAdapter() {
  int32_t adapterIndex = 0;
  vr::VRSystem()->GetDXGIOutputInfo(&adapterIndex);
  IDXGIFactory1* factory = nullptr;
  IDXGIAdapter1* adapter = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) factory->EnumAdapters1(adapterIndex < 0 ? 0 : UINT(adapterIndex), &adapter);
  drop(factory);
  return adapter;
}

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

// ---------------------------------------------------------------- the pretend heavy game
// Draws a big picture with a slow shader over and over, two frames queued like a game, tuned to about 11 ms a frame, so the card is
// always busy. Nothing is shown and it makes no sound.
static std::atomic<bool> hogStop{false}, hogRunning{false};
static bool hogScene = false;  // true: it hands its frames to SteamVR like a real game, so the compositor is busy too
static std::atomic<double> hogFrameMs{0}, hogGpuMs{0};
static std::atomic<int> hogFrames{0};
static const char* kHogShader = R"(
cbuffer K : register(b0) { uint n; uint3 pad; };
float4 vs(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 ps(float4 pos : SV_Position) : SV_Target {
  float2 p = pos.xy * 0.0007; float a = 0;
  [loop] for (uint i = 0; i < n; ++i) { p = frac(float2(p.x * p.x - p.y * p.y, 2 * p.x * p.y) + float2(0.31, 0.53)); a += sin(p.x * 6.2) * cos(p.y * 6.2); }
  return float4(a, a * 0.5, 0, 1);
}
)";
static void hog() {
  IDXGIAdapter1* adapter = vrAdapter();
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
  drop(adapter);
  if (FAILED(hr)) { printf("note  pretend game: no device\n"); return; }
  ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
  D3DCompile(kHogShader, strlen(kHogShader), "hog", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err);
  drop(err);
  D3DCompile(kHogShader, strlen(kHogShader), "hog", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err);
  if (err) { printf("note  pretend game shader: %s\n", static_cast<const char*>(err->GetBufferPointer())); drop(err); }
  if (!vsb || !psb) { printf("note  pretend game: shaders didn't compile\n"); return; }
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
  dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
  drop(vsb); drop(psb);
  D3D11_TEXTURE2D_DESC d{};
  d.Width = d.Height = 2048;
  d.MipLevels = d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* rt = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  dev->CreateTexture2D(&d, nullptr, &rt);
  dev->CreateRenderTargetView(rt, nullptr, &rtv);
  D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  ID3D11Buffer* cb = nullptr;
  dev->CreateBuffer(&bd, nullptr, &cb);
  ID3D11Query* q[2] = {nullptr, nullptr};
  D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
  for (auto& x : q) dev->CreateQuery(&qd, &x);
  D3D11_VIEWPORT vp{0, 0, 2048, 2048, 0, 1};
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  ctx->RSSetViewports(1, &vp);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs, nullptr, 0);
  ctx->PSSetShader(ps, nullptr, 0);
  ctx->PSSetConstantBuffers(0, 1, &cb);

  uint32_t n = 16;
  double avg = 0;
  int frame = 0;
  auto lastDone = Clock::now();
  // As a game: SteamVR paces it, and the card's time for each frame is tuned to just inside one refresh.
  float hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
  if (hz < 30) hz = 90;
  const double budget = 1000.0 / hz;
  ID3D11Query *dj[4] = {}, *ts0[4] = {}, *ts1[4] = {};
  D3D11_QUERY_DESC djd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, tsd{D3D11_QUERY_TIMESTAMP, 0};
  for (int i = 0; i < 4; ++i) { dev->CreateQuery(&djd, &dj[i]); dev->CreateQuery(&tsd, &ts0[i]); dev->CreateQuery(&tsd, &ts1[i]); }
  double gpuAvg = 0;
  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
  if (hogScene) printf("note  pretend game hands SteamVR its frames, %.0f a second\n", hz);
  hogRunning.store(true);
  while (!hogStop.load() && hogScene) {
    vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    uint32_t k[4] = {n, 0, 0, 0};
    ctx->UpdateSubresource(cb, 0, nullptr, k, 0, 0);
    const int f = frame & 3;
    ctx->Begin(dj[f]);
    ctx->End(ts0[f]);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->Draw(3, 0);
    ctx->End(ts1[f]);
    ctx->End(dj[f]);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    vr::Texture_t vt{rt, vr::TextureType_DirectX, vr::ColorSpace_Auto};
    vr::VRCompositor()->Submit(vr::Eye_Left, &vt);
    vr::VRCompositor()->Submit(vr::Eye_Right, &vt);
    ++frame;
    const double ms = msSince(lastDone);
    lastDone = Clock::now();
    avg = avg == 0 ? ms : avg * 0.9 + ms * 0.1;
    hogFrameMs.store(avg);
    hogFrames.fetch_add(1);
    if (frame >= 4) {  // the card's own time for a frame three back, without waiting for it
      const int g = (frame - 4) & 3;
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
      UINT64 a = 0, b = 0;
      if (ctx->GetData(dj[g], &dd, sizeof dd, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dd.Disjoint &&
          ctx->GetData(ts0[g], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && ctx->GetData(ts1[g], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
        const double gms = double(b - a) * 1000.0 / double(dd.Frequency);
        gpuAvg = gpuAvg == 0 ? gms : gpuAvg * 0.9 + gms * 0.1;
        hogGpuMs.store(gpuAvg);
        if (frame % 8 == 0 && frame < 600) {
          if (gpuAvg < budget * 0.80) n = uint32_t(n * 1.25) + 1;
          else if (gpuAvg > budget * 0.95) n = std::max<uint32_t>(1, uint32_t(n / 1.15));
        }
      }
    }
  }
  if (!hogScene) ctx->OMSetRenderTargets(1, &rtv, nullptr);
  while (!hogStop.load() && !hogScene) {
    uint32_t k[4] = {n, 0, 0, 0};
    ctx->UpdateSubresource(cb, 0, nullptr, k, 0, 0);
    ctx->Draw(3, 0);
    ctx->End(q[frame & 1]);
    ctx->Flush();
    ++frame;
    // Two frames queued, like a game: wait for the one before this to finish before drawing another.
    if (frame >= 2) {
      BOOL done = FALSE;
      while (ctx->GetData(q[frame & 1], &done, sizeof done, 0) != S_OK) Sleep(0);
      const double ms = msSince(lastDone);
      lastDone = Clock::now();
      avg = avg == 0 ? ms : avg * 0.9 + ms * 0.1;
      hogFrameMs.store(avg);
      hogFrames.fetch_add(1);
      // Tune to about 11 ms a frame, as a game at 90 frames a second that the card only just keeps up with.
      if (frame % 8 == 0 && frame < 400) {
        if (avg < 9) n = uint32_t(n * 1.4) + 1;
        else if (avg > 13) n = std::max<uint32_t>(1, uint32_t(n / 1.3));
      }
    }
  }
  ctx->ClearState();
  ctx->Flush();
  for (int i = 0; i < 4; ++i) { drop(dj[i]); drop(ts0[i]); drop(ts1[i]); }
  for (auto& x : q) drop(x);
  drop(cb); drop(rtv); drop(rt); drop(ps); drop(vs); drop(ctx); drop(dev);
  printf("note  pretend game: %u shader steps a pixel, %.1f ms a frame at the end, the card's time %.1f ms\n", n, avg, gpuAvg);
  hogRunning.store(false);
}

// ---------------------------------------------------------------- reading back what SteamVR holds
struct Reader {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  bool init() {
    IDXGIAdapter1* adapter = vrAdapter();
    const HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    drop(adapter);
    return SUCCEEDED(hr);
  }
  // The red value of the top-left pixel SteamVR is holding for this overlay, -1 if it can't be read.
  int holding(vr::VROverlayHandle_t o) {
    void* view = nullptr;
    uint32_t w = 0, h = 0, fmt = 0;
    vr::ETextureType api = vr::TextureType_Invalid;
    vr::EColorSpace cs = vr::ColorSpace_Auto;
    vr::VRTextureBounds_t bounds{};
    const auto e = vr::VROverlay()->GetOverlayTexture(o, &view, dev, &w, &h, &fmt, &api, &cs, &bounds);
    if (e != vr::VROverlayError_None || !view) { printf("note  GetOverlayTexture: %s\n", vr::VROverlay()->GetOverlayErrorNameFromEnum(e)); return -1; }
    ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(view);
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    int value = -1;
    ID3D11Texture2D* src = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src));
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
    }
    drop(src);
    drop(res);
    vr::VROverlay()->ReleaseNativeOverlayHandle(o, view);
    return value;
  }
};

// ---------------------------------------------------------------- one run of pictures
struct Result {
  double callAvg = 0, callMax = 0, cpuPerPicture = 0, worstGap = 0, lagAvg = 0, lagMax = 0;
  uint64_t given = 0, replaced = 0;
  int sent = 0, held = -1;
  bool shared = true;
};
static Result run(const char* name, fh::OverlayUploader& up, vr::VROverlayHandle_t o, Reader& reader, uint8_t phase) {
  const uint32_t W = 1200, H = 760;  // the size of the program's panel
  std::vector<uint8_t> px(size_t(W) * H * 4);
  up.takeStats();
  Result r;
  double callSum = 0, cpu = 0;
  const int n = 120;  // two seconds at 60 a second
  auto tickStart = Clock::now();
  uint8_t last = 0;
  for (int i = 0; i < n; ++i) {
    const auto now = Clock::now();
    if (i > 0) r.worstGap = std::max(r.worstGap, msSince(tickStart));
    tickStart = now;
    last = uint8_t(20 + (i * 7) % 230);
    for (size_t k = 0; k < px.size(); k += 4) { px[k] = last; px[k + 1] = phase; px[k + 2] = 0; px[k + 3] = 255; }
    // What the program's loop does: hand it over, then nap in small steps, passing on what the card has finished.
    const double c0 = threadCpuMs();
    const auto t0 = Clock::now();
    up.upload(o, px.data(), W, H);
    up.pump();
    double work = msSince(t0);
    const auto wake = tickStart + std::chrono::milliseconds(16);
    while (up.waiting() && Clock::now() + std::chrono::milliseconds(1) < wake) {
      napMs(1);
      const auto p0 = Clock::now();
      up.pump();
      work += msSince(p0);
    }
    cpu += threadCpuMs() - c0;
    callSum += work;
    r.callMax = std::max(r.callMax, work);
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(wake - Clock::now()).count();
    if (left > 0) napMs(int(left));
  }
  // Let the last one land, then look at what SteamVR holds.
  for (int i = 0; i < 300 && up.waiting(); ++i) { Sleep(5); up.pump(); }
  const fh::OverlayUploader::Stats s = up.takeStats();
  Sleep(300);
  r.held = reader.holding(o);
  r.sent = last;
  r.callAvg = callSum / n;
  r.cpuPerPicture = cpu / n;
  r.given = s.given;
  r.replaced = s.replaced;
  r.lagAvg = s.given ? s.lagMsSum / s.given : 0;
  r.lagMax = s.lagMsMax;
  r.shared = s.shared;
  printf("note  %-34s loop cost %.2f ms a picture (worst %.1f), CPU %.2f ms a picture, longest gap between pictures %.0f ms; "
         "reached SteamVR %.1f ms after drawing (worst %.0f), %llu given, %llu replaced by newer ones; SteamVR holds %d, last sent %d\n",
         name, r.callAvg, r.callMax, r.cpuPerPicture, r.worstGap, r.lagAvg, r.lagMax, (unsigned long long)r.given, (unsigned long long)r.replaced, r.held, r.sent);
  return r;
}
// Drawing the pages the way the program does, timed: cairo draws the panel's text with DirectWrite on Windows (FreeType on the Frame),
// and if that goes through the graphics card it would wait behind a game too.
using namespace fh;
struct DrawTimes { double dash = 0, dashMax = 0, decaf = 0, decafMax = 0, panel = 0, panelMax = 0, rgba = 0, cpu = 0; };
static DrawTimes timeDrawing(const char* when) {
  Canvas dash(1200, 760), hint(820, 300);
  std::vector<Button> b;
  DrawTimes t;
  const int n = 30;
  for (int i = -1; i < n; ++i) {  // the first draw fills the font caches and isn't counted
    const double c0 = threadCpuMs();
    DashModel m;
    m.game = "Pavlov VR";
    m.live = LiveState::Live;
    m.status = "Saved for Pavlov VR, move " + std::to_string(i);
    auto t0 = Clock::now();
    drawDashboard(dash, m, b);
    const double d1 = msSince(t0);
    DecafModel dm;
    dm.game = "Pavlov VR";
    dm.live = LiveState::Live;
    dm.level = 2;
    dm.hands[0] = {true, 0.31f + 0.01f * i, 0.038f};
    dm.hands[1] = {true, 0.27f, 0.035f};
    t0 = Clock::now();
    drawDecaf(dash, dm, b);
    const double d2 = msSince(t0);
    PanelModel p;
    p.header = "LEFT HAND";
    p.chip = "GRIP";
    p.text = "Grab the ghost with your right hand";
    p.status = std::to_string(i) + ".0 cm forward, 2.5 cm up";
    p.keys = {{"GRIP", "Fine-tune"}, {"A / X", "Reset ghost"}, {"B / Y", "Cancel"}};
    t0 = Clock::now();
    drawPanel(hint, p);
    const double d3 = msSince(t0);
    t0 = Clock::now();
    dash.toRGBA();
    const double d4 = msSince(t0);
    const double c = threadCpuMs() - c0;
    if (i < 0) continue;
    t.dash += d1 / n; t.dashMax = std::max(t.dashMax, d1);
    t.decaf += d2 / n; t.decafMax = std::max(t.decafMax, d2);
    t.panel += d3 / n; t.panelMax = std::max(t.panelMax, d3);
    t.rgba += d4 / n;
    t.cpu += c / n;
  }
  printf("note  drawing, %s: Hands page %.1f ms (worst %.1f), Decaf page %.1f ms (worst %.1f), placing panel %.1f ms (worst %.1f), "
         "to RGBA %.1f ms; CPU %.1f ms for all four\n", when, t.dash, t.dashMax, t.decaf, t.decafMax, t.panel, t.panelMax, t.rgba, t.cpu);
  return t;
}

// What the program asks SteamVR every loop, timed one at a time, 5 ms apart for a second: each is a trip to SteamVR's compositor
// or server. If those slow down while a game keeps the compositor busy, the program's loop does too.
static double timeCalls(const char* when, vr::VROverlayHandle_t o) {
  const char* names[6] = {"IsDashboardVisible", "IsOverlayVisible", "GetPrimaryDashboardDevice", "SetOverlayTransformAbsolute", "SetOverlayColor", "PollNextEvent"};
  double sum[6] = {}, worst[6] = {};
  const int n = 200;
  vr::HmdMatrix34_t m{};
  m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
  m.m[1][3] = 1.5f; m.m[2][3] = -1;
  for (int i = 0; i < n; ++i) {
    for (int c = 0; c < 6; ++c) {
      const auto t0 = Clock::now();
      switch (c) {
        case 0: vr::VROverlay()->IsDashboardVisible(); break;
        case 1: vr::VROverlay()->IsOverlayVisible(o); break;
        case 2: vr::VROverlay()->GetPrimaryDashboardDevice(); break;
        case 3: vr::VROverlay()->SetOverlayTransformAbsolute(o, vr::TrackingUniverseStanding, &m); break;
        case 4: vr::VROverlay()->SetOverlayColor(o, 1.f, 1.f, float(i % 2)); break;
        default: { vr::VREvent_t ev; while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {} break; }
      }
      const double d = msSince(t0);
      sum[c] += d;
      worst[c] = std::max(worst[c], d);
    }
    napMs(5);
  }
  double total = 0;
  std::string line;
  for (int c = 0; c < 6; ++c) {
    char b[128];
    snprintf(b, sizeof b, "%s%s %.2f (worst %.1f)", c ? ", " : "", names[c], sum[c] / n, worst[c]);
    line += b;
    total += sum[c] / n;
  }
  printf("note  calls to SteamVR, %s, ms each: %s\n", when, line.c_str());
  return total;
}

static bool same(int got, int want) { return got >= 0 && std::abs(got - want) <= 3; }

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Overlay);
  if (e != vr::VRInitError_None) { printf("FAIL VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e)); return 2; }
  printf("note  Room Setup %s\n", closeRoomSetup(15000) ? "opened and was closed" : "didn't open");
  wait(1500);
  // A game from here on, like Pavlov. SteamVR quits a game that's connected when Room Setup opens, so only now.
  vr::VR_Shutdown();
  vr::VR_Init(&e, vr::VRApplication_Scene);
  if (e != vr::VRInitError_None) { printf("FAIL VR_Init as a game: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e)); return 2; }
  hogScene = true;

  Reader reader;
  if (!reader.init()) { printf("FAIL no D3D device to read back with\n"); return 2; }
  vr::VROverlayHandle_t o = 0;
  vr::VROverlay()->CreateOverlay("utmtftm.uploadcheck", "UTMTFTM upload check", &o);
  vr::VROverlay()->SetOverlayWidthInMeters(o, 1.f);
  vr::VROverlay()->ShowOverlay(o);

  std::string info;
  fh::OverlayUploader oldWay, newWay, newNoPrio;
  const bool a = oldWay.init(&info, 0, false);
  printf("note  old way: %s\n", info.c_str());
  const bool b = newWay.init(&info, 7, true);
  printf("note  new way: %s\n", info.c_str());
  const bool c = newNoPrio.init(&info, 0, true);
  printf("note  new way, normal priority: %s\n", info.c_str());
  check(a && b && c, "a D3D11 device for each way");
  if (!(a && b && c)) { printf("FAILED: %d of %d\n", fails, checks); vr::VR_Shutdown(); return 1; }

  const double callsIdle = timeCalls("nothing drawing", o);
  const DrawTimes drawIdle = timeDrawing("card idle");
  const Result oldIdle = run("old way, card idle:", oldWay, o, reader, 1);
  const Result newIdle = run("new way, card idle:", newWay, o, reader, 2);
  check(newIdle.shared, "SteamVR takes shared textures");
  check(same(newIdle.held, newIdle.sent), "card idle: SteamVR holds the newest picture, the new way");
  check(newIdle.lagAvg < 6, "card idle: the new way's pictures reach SteamVR within 6 ms on average");

  std::thread game(hog);
  for (int i = 0; i < 100 && !hogRunning.load(); ++i) wait(20);
  wait(4000);  // tuning
  printf("note  pretend game running: %.1f ms a frame, %d frames so far\n", hogFrameMs.load(), hogFrames.load());
  const double callsBusy = timeCalls("game running", o);
  const DrawTimes drawBusy = timeDrawing("card busy");
  const Result oldBusy = run("old way, card busy:", oldWay, o, reader, 3);
  const Result newBusy = run("new way, card busy:", newWay, o, reader, 4);
  const Result newBusy0 = run("new way, card busy, normal priority:", newNoPrio, o, reader, 5);
  printf("note  pretend game at the end: %.1f ms a frame\n", hogFrameMs.load());
  hogStop.store(true);
  game.join();

  printf("note  pretend game: the card's time %.1f ms a frame, a frame every %.1f ms\n", hogGpuMs.load(), hogFrameMs.load());
  check(hogGpuMs.load() > 6, "the pretend game kept the card busy (over 6 ms of its time a frame)");
  printf("note  all six calls together: %.2f ms with nothing drawing, %.2f ms with the game running\n", callsIdle, callsBusy);
  check(same(newBusy.held, newBusy.sent), "card busy: SteamVR holds the newest picture, the new way");
  check(same(newBusy0.held, newBusy0.sent), "card busy, normal priority: SteamVR holds the newest picture, the new way");
  check(newBusy.callAvg < 1.5 && newBusy.callMax < 8, "card busy: handing a picture over costs the loop under 1.5 ms on average and never 8 ms");
  check(newBusy.worstGap < 40, "card busy: the loop never stalls (no gap between pictures over 40 ms)");
  printf("note  card busy, old way vs new: loop cost %.2f vs %.2f ms a picture, worst %.1f vs %.1f, CPU %.2f vs %.2f ms a picture, longest gap %.0f vs %.0f ms\n",
         oldBusy.callAvg, newBusy.callAvg, oldBusy.callMax, newBusy.callMax, oldBusy.cpuPerPicture, newBusy.cpuPerPicture, oldBusy.worstGap, newBusy.worstGap);
  printf("note  drawing all four, card idle vs busy: %.1f vs %.1f ms\n", drawIdle.dash + drawIdle.decaf + drawIdle.panel + drawIdle.rgba,
         drawBusy.dash + drawBusy.decaf + drawBusy.panel + drawBusy.rgba);
  printf("note  card idle, old way: loop cost %.2f ms a picture (worst %.1f); SteamVR holds %d, last sent %d\n", oldIdle.callAvg, oldIdle.callMax, oldIdle.held, oldIdle.sent);
  printf("note  card busy, new way: pictures reach SteamVR %.1f ms after drawing with priority 7, %.1f ms with normal priority\n", newBusy.lagAvg, newBusy0.lagAvg);

  vr::VROverlay()->DestroyOverlay(o);
  vr::VR_Shutdown();  // Valve: before the textures SteamVR was given are destroyed
  oldWay.shutdown();
  newWay.shutdown();
  newNoPrio.shutdown();
  printf("%s: %d of %d\n", fails ? "FAILED" : "PASSED", fails ? fails : checks, checks);
  return fails ? 1 : 0;
}

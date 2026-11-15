// The Windows side of overlay_upload.h: overlay images go to SteamVR as shared D3D11 textures, and nothing here waits for the card.
//
// Why not TextureType_DirectX any more: with it, SteamVR copies our texture on OUR device and that copy has to finish before the
// next picture, so every picture waited for the graphics card, and the driver spun the CPU while it waited. In a heavy game the
// card is busy with the game, so the waits grew long. J4, Pavlov over Virtual Desktop on a Quest 3, 2026-11-14: one picture of
// the bubble took 18-54 ms (6 ms in a light game), the whole program stalled for up to 2.3 s at a time, and it used 30% of a
// core for the same pictures that cost 3.5% in the light game. Everything the program does waits behind that loop, so the panel,
// the bubble and letting go of a placed hand all came late. The Steam Frame never showed it.
//
// Now: with TextureType_DXGISharedHandle the compositor reads our texture itself, so SteamVR puts nothing on our device. A picture
// goes into a staging texture (Map, never waiting), one CopyResource into a shared texture, an event query after it, and a Flush.
// pump() looks at the query without waiting; once the card has done the copy, SteamVR gets that texture. A newer picture that
// arrives while one is still on its way replaces the waiting pixels, because only the newest matters, so the card is never more
// than one picture behind and nothing piles up. If SteamVR ever refuses a shared texture, it goes back to the old way.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <vector>

#include "overlay_upload.h"

namespace fh {

namespace {
using Clock = std::chrono::steady_clock;
constexpr int kRing = 3;  // textures per overlay. A slot is written again only two pictures after SteamVR moved off it
template <class T> void drop(T*& x) { if (x) { x->Release(); x = nullptr; } }
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
}  // namespace

struct OverlayUploader::Impl {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  bool shared = true;  // false: the old way, TextureType_DirectX, SteamVR copying on our device
  Stats stats;

  struct Slot {
    ID3D11Texture2D* staging = nullptr;  // we write here, with the CPU
    ID3D11Texture2D* tex = nullptr;      // SteamVR reads this one
    HANDLE handle = nullptr;             // tex's share handle
    ID3D11Query* done = nullptr;         // set when the card has copied staging into tex
  };
  struct Target {
    uint32_t w = 0, h = 0;
    Slot slot[kRing];
    int shown = -1;   // the slot SteamVR has
    int flying = -1;  // the slot the card is copying into
    Clock::time_point flyingSince{};
    std::vector<uint8_t> pending;  // the newest picture, waiting for the copy before it to finish
    uint32_t pw = 0, ph = 0;
    bool hasPending = false;
    Clock::time_point pendingSince{};
  };
  std::map<vr::VROverlayHandle_t, Target> targets;

  void destroy(Target& t) {
    for (auto& s : t.slot) { drop(s.staging); drop(s.tex); drop(s.done); s.handle = nullptr; }
    t.w = t.h = 0;
    t.shown = t.flying = -1;
  }
  bool create(Target& t, uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h;
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    for (auto& s : t.slot) {
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      d.CPUAccessFlags = 0;
      d.MiscFlags = shared ? D3D11_RESOURCE_MISC_SHARED : 0;
      if (FAILED(dev->CreateTexture2D(&d, nullptr, &s.tex))) return false;
      if (shared) {
        IDXGIResource* r = nullptr;
        if (FAILED(s.tex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&r)))) return false;
        const HRESULT hr = r->GetSharedHandle(&s.handle);
        drop(r);
        if (FAILED(hr) || !s.handle) return false;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &s.staging))) return false;
        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT, 0};
        if (FAILED(dev->CreateQuery(&q, &s.done))) return false;
      }
    }
    t.w = w; t.h = h;
    t.shown = t.flying = -1;
    return true;
  }

  // The old way: SteamVR copies the texture on our device, and we wait. Only if SteamVR refuses shared textures.
  vr::EVROverlayError direct(vr::VROverlayHandle_t o, Target& t, const uint8_t* rgba, uint32_t w, uint32_t h) {
    if (t.w != w || t.h != h) {
      destroy(t);
      if (!create(t, w, h)) { destroy(t); return vr::VROverlayError_RequestFailed; }
    }
    const int s = (t.shown + 1) % kRing;
    ctx->UpdateSubresource(t.slot[s].tex, 0, nullptr, rgba, w * 4, 0);
    ctx->Flush();  // the texture must hold the new pixels before SteamVR copies it
    vr::Texture_t vt{t.slot[s].tex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
    const vr::EVROverlayError e = vr::VROverlay()->SetOverlayTexture(o, &vt);
    // SteamVR's copy goes on this device's queue. With nothing else to draw, D3D11 is happy to sit on it until the next image,
    // which is why a tap on the panel used to show nothing until the laser moved.
    ctx->Flush();
    t.shown = s;
    ++stats.given;
    return e;
  }

  // Starts the card copying a picture into the next slot. false: the slot's staging texture is still busy, try again next pump.
  bool submit(Target& t, const uint8_t* rgba, uint32_t w, uint32_t h, Clock::time_point since) {
    if (t.w != w || t.h != h) {
      destroy(t);
      if (!create(t, w, h)) { destroy(t); return false; }
    }
    const int s = (t.shown + 1) % kRing;  // the oldest: SteamVR moved off it two pictures ago
    Slot& sl = t.slot[s];
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(sl.staging, 0, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (FAILED(hr)) return false;  // DXGI_ERROR_WAS_STILL_DRAWING: never wait for it
    const size_t row = size_t(w) * 4;
    auto* dst = static_cast<uint8_t*>(m.pData);
    if (m.RowPitch == row) std::memcpy(dst, rgba, row * h);
    else for (uint32_t y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * m.RowPitch, rgba + y * row, row);
    ctx->Unmap(sl.staging, 0);
    ctx->CopyResource(sl.tex, sl.staging);
    ctx->End(sl.done);
    ctx->Flush();  // starts the copy; it doesn't wait for it
    t.flying = s;
    t.flyingSince = since;
    return true;
  }

  // Hands SteamVR a finished copy, then starts the waiting picture. Never waits.
  vr::EVROverlayError step(vr::VROverlayHandle_t o, Target& t) {
    vr::EVROverlayError e = vr::VROverlayError_None;
    if (t.flying >= 0) {
      BOOL done = FALSE;
      if (ctx->GetData(t.slot[t.flying].done, &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done) return e;
      vr::Texture_t vt{t.slot[t.flying].handle, vr::TextureType_DXGISharedHandle, vr::ColorSpace_Auto};
      e = vr::VROverlay()->SetOverlayTexture(o, &vt);
      if (e != vr::VROverlayError_None) {
        // This SteamVR won't take a shared texture: the old way from now on, starting with the newest picture.
        shared = false;
        for (auto& [oo, tt] : targets) destroy(tt);
        return e;
      }
      const double lag = msSince(t.flyingSince);
      ++stats.given;
      stats.lagMsSum += lag;
      stats.lagMsMax = std::max(stats.lagMsMax, lag);
      t.shown = t.flying;
      t.flying = -1;
    }
    if (t.hasPending) {
      if (submit(t, t.pending.data(), t.pw, t.ph, t.pendingSince)) t.hasPending = false;
      else if (t.w == 0) t.hasPending = false;  // the textures couldn't be made: the next upload() says so, and SetOverlayRaw takes it
    }
    return e;
  }

  void timed(Clock::time_point t0) {
    const double ms = msSince(t0);
    ++stats.calls;
    stats.callMsSum += ms;
    stats.callMsMax = std::max(stats.callMsMax, ms);
  }
};

OverlayUploader::OverlayUploader() : p(new Impl) {}
OverlayUploader::~OverlayUploader() { shutdown(); delete p; }
bool OverlayUploader::ready() const { return p->dev != nullptr; }

bool OverlayUploader::init(std::string* info, int gpuPriority, bool sharedTextures) {
  p->shared = sharedTextures;
  // The GPU the headset is driven by.
  int32_t index = 0;
  vr::VRSystem()->GetDXGIOutputInfo(&index);
  IDXGIFactory1* factory = nullptr;
  IDXGIAdapter1* adapter = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    factory->EnumAdapters1(index < 0 ? 0 : (UINT)index, &adapter);
  drop(factory);
  // Our copies are tiny: no driver worker threads to spin for them.
  const UINT flags = D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
  HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                 D3D11_SDK_VERSION, &p->dev, nullptr, &p->ctx);
  // A few small copies a second: ask the card to fit them in between the game's work rather than after it.
  std::string prio = "normal card priority";
  if (SUCCEEDED(hr) && gpuPriority != 0) {
    IDXGIDevice* dx = nullptr;
    if (SUCCEEDED(p->dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx)))) {
      const HRESULT ph = dx->SetGPUThreadPriority(gpuPriority);
      INT got = 0;
      dx->GetGPUThreadPriority(&got);
      char buf[96];
      if (SUCCEEDED(ph)) snprintf(buf, sizeof buf, "card priority %d", int(got));
      else snprintf(buf, sizeof buf, "card priority refused (0x%08lx), at %d", (unsigned long)ph, int(got));
      prio = buf;
      drop(dx);
    }
  }
  if (info) {
    char name[128] = "unknown adapter";
    DXGI_ADAPTER_DESC1 d{};
    if (adapter && SUCCEEDED(adapter->GetDesc1(&d))) WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
    *info = FAILED(hr) ? std::string("D3D11CreateDevice failed on ") + name
                       : std::string(name) + (p->shared ? ", shared textures, " : ", copied by SteamVR, ") + prio;
  }
  drop(adapter);
  return SUCCEEDED(hr);
}

vr::EVROverlayError OverlayUploader::upload(vr::VROverlayHandle_t o, const uint8_t* rgba, uint32_t w, uint32_t h) {
  if (!ready() || !w || !h) return vr::VROverlayError_RequestFailed;
  if (p->dev->GetDeviceRemovedReason() != S_OK) return vr::VROverlayError_RequestFailed;  // the card was reset: SetOverlayRaw
  const auto t0 = Clock::now();
  Impl::Target& t = p->targets[o];
  vr::EVROverlayError e = vr::VROverlayError_None;
  if (p->shared) p->step(o, t);  // a copy that's finished goes to SteamVR first, so this picture can take the next slot
  if (!p->shared) {
    e = p->direct(o, t, rgba, w, h);
  } else if (t.flying < 0 && !t.hasPending && p->submit(t, rgba, w, h, t0)) {
    // on its way
  } else if (t.flying < 0 && !t.hasPending && t.w == 0) {
    e = vr::VROverlayError_RequestFailed;  // the textures couldn't be made: SetOverlayRaw takes this one
  } else {
    if (t.hasPending) ++p->stats.replaced;
    t.pending.assign(rgba, rgba + size_t(w) * h * 4);
    t.pw = w; t.ph = h;
    t.hasPending = true;
    t.pendingSince = t0;
  }
  p->timed(t0);
  return e;
}

void OverlayUploader::pump() {
  if (!ready() || !p->shared) return;
  const auto t0 = Clock::now();
  bool any = false;
  for (auto& [o, t] : p->targets) {
    if (t.flying < 0 && !t.hasPending) continue;
    any = true;
    p->step(o, t);
    if (!p->shared) break;
  }
  if (any) p->timed(t0);
}

bool OverlayUploader::waiting() const {
  if (!p->shared) return false;
  for (const auto& [o, t] : p->targets)
    if (t.flying >= 0 || t.hasPending) return true;
  return false;
}

OverlayUploader::Stats OverlayUploader::takeStats() {
  Stats s = p->stats;
  s.shared = p->shared;
  p->stats = Stats{};
  return s;
}

void OverlayUploader::shutdown() {
  for (auto& [o, t] : p->targets) p->destroy(t);
  p->targets.clear();
  drop(p->ctx);
  drop(p->dev);
}

}  // namespace fh

// The Windows side of overlay_upload.h: overlay images go to SteamVR as D3D11 textures.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <map>

#include "overlay_upload.h"

namespace fh {

namespace {
constexpr int kRing = 3;  // textures per overlay, so we never rewrite the one SteamVR was just given
template <class T> void drop(T*& x) { if (x) { x->Release(); x = nullptr; } }
}  // namespace

struct OverlayUploader::Impl {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  struct Target {
    uint32_t w = 0, h = 0;
    ID3D11Texture2D* tex[kRing] = {nullptr, nullptr, nullptr};
    int next = 0;
  };
  std::map<vr::VROverlayHandle_t, Target> targets;

  void destroy(Target& t) {
    for (auto& x : t.tex) drop(x);
    t = Target{};
  }
  bool create(Target& t, uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h;
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (auto& x : t.tex)
      if (FAILED(dev->CreateTexture2D(&d, nullptr, &x))) return false;
    t.w = w; t.h = h;
    return true;
  }
};

OverlayUploader::OverlayUploader() : p(new Impl) {}
OverlayUploader::~OverlayUploader() { shutdown(); delete p; }
bool OverlayUploader::ready() const { return p->dev != nullptr; }

bool OverlayUploader::init(std::string* info) {
  // The GPU the headset is driven by.
  int32_t index = 0;
  vr::VRSystem()->GetDXGIOutputInfo(&index);
  IDXGIFactory1* factory = nullptr;
  IDXGIAdapter1* adapter = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    factory->EnumAdapters1(index < 0 ? 0 : (UINT)index, &adapter);
  drop(factory);
  HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &p->dev, nullptr, &p->ctx);
  if (info) {
    char name[128] = "unknown adapter";
    DXGI_ADAPTER_DESC1 d{};
    if (adapter && SUCCEEDED(adapter->GetDesc1(&d))) WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
    *info = FAILED(hr) ? std::string("D3D11CreateDevice failed on ") + name : std::string(name);
  }
  drop(adapter);
  return SUCCEEDED(hr);
}

vr::EVROverlayError OverlayUploader::upload(vr::VROverlayHandle_t o, const uint8_t* rgba, uint32_t w, uint32_t h) {
  if (!ready() || !w || !h) return vr::VROverlayError_RequestFailed;
  Impl::Target& t = p->targets[o];
  if (t.w != w || t.h != h) {
    p->destroy(t);
    if (!p->create(t, w, h)) { p->destroy(t); return vr::VROverlayError_RequestFailed; }
  }
  ID3D11Texture2D* tex = t.tex[t.next];
  t.next = (t.next + 1) % kRing;
  p->ctx->UpdateSubresource(tex, 0, nullptr, rgba, w * 4, 0);
  p->ctx->Flush();  // the texture must hold the new pixels before SteamVR copies it
  vr::Texture_t vt{tex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
  const vr::EVROverlayError e = vr::VROverlay()->SetOverlayTexture(o, &vt);
  // SteamVR's copy goes on this device's queue. With nothing else to draw, D3D11 is happy to sit on it until the next image,
  // which is why a tap on the panel used to show nothing until the laser moved.
  p->ctx->Flush();
  return e;
}

void OverlayUploader::shutdown() {
  for (auto& [o, t] : p->targets) p->destroy(t);
  p->targets.clear();
  drop(p->ctx);
  drop(p->dev);
}

}  // namespace fh

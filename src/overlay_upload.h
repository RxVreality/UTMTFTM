// GPU path for overlay images: SetOverlayTexture with a real texture instead of SetOverlayRaw.
// d3dupload.cpp does it with D3D11 on Windows, and vkupload.cpp with Vulkan on Linux.
//
// Why: SetOverlayRaw hands every frame to the compositor in a shared-memory block that the
// compositor frees on its own schedule. Each client may have 201 outstanding; at 60 Hz (the
// placement bubble) they pile up within seconds and then EVERY upload is refused, dashboard
// included, so the dashboard froze on an old image (found on the Steam Frame, 2026-11-01).
#pragma once
#include <openvr.h>

#include <cstdint>
#include <string>

namespace fh {

class OverlayUploader {
 public:
  OverlayUploader();
  ~OverlayUploader();
  OverlayUploader(const OverlayUploader&) = delete;
  OverlayUploader& operator=(const OverlayUploader&) = delete;

  // Call after VR_Init. false = no usable device; callers fall back to SetOverlayRaw.
  bool init(std::string* info);
  bool ready() const;

  // Copy straight-alpha RGBA8 pixels into this overlay's next texture and hand it to SteamVR.
  vr::EVROverlayError upload(vr::VROverlayHandle_t o, const uint8_t* rgba, uint32_t w, uint32_t h);

  // Release everything. Valve: call VR_Shutdown BEFORE destroying textures handed to SteamVR.
  void shutdown();

 private:
  struct Impl;
  Impl* p;
};

}  // namespace fh

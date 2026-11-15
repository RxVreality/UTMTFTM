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
  // gpuPriority (Windows): asked of the graphics card for our little copies, -7 to 7. The card may say no; info says which.
  // sharedTextures false (Windows): the old way, TextureType_DirectX, which waits for the card. For comparing in tests.
  bool init(std::string* info, int gpuPriority = 7, bool sharedTextures = true);
  bool ready() const;

  // Copy straight-alpha RGBA8 pixels into this overlay's next texture and hand it to SteamVR.
  // On Windows this never waits for the graphics card: the picture reaches SteamVR from pump() once the card has copied it,
  // and a newer picture for the same overlay replaces one still waiting. None means taken.
  vr::EVROverlayError upload(vr::VROverlayHandle_t o, const uint8_t* rgba, uint32_t w, uint32_t h);

  // Call every loop: hands SteamVR each picture the card has finished copying, and starts the next one. Never waits.
  void pump();
  // A picture is still on its way to SteamVR, so pump() has something to do.
  bool waiting() const;

  // What the hand-over cost since the last call (Windows; zeros elsewhere).
  struct Stats {
    uint64_t given = 0;      // pictures SteamVR was given
    uint64_t replaced = 0;   // pictures a newer one replaced before the card got to them
    double lagMsSum = 0;     // from upload() to SteamVR having it, summed over given
    double lagMsMax = 0;
    double callMsSum = 0;    // time spent inside upload() and pump(), so the loop's own cost
    double callMsMax = 0;
    uint64_t calls = 0;
    bool shared = true;      // false once SteamVR refused a shared texture and the old way took over
  };
  Stats takeStats();

  // Release everything. Valve: call VR_Shutdown BEFORE destroying textures handed to SteamVR.
  void shutdown();

 private:
  struct Impl;
  Impl* p;
};

}  // namespace fh

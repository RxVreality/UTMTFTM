// The dim around the globes while you place a hand, drawn per eye like the ghost, on a big plane facing the headset.
//
// SteamVR won't show the cameras in just one spot. It blends them over the whole view (Room View, translucent), it doesn't
// give the picture to anyone else on a Frame, and an overlay can only draw on top. So this comes at it from the other side:
// with Room View on, everything outside the globes goes dark, and the only place left to look is where your real hands are.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ghost3d.h"

namespace fh {

struct SpotHole { V3 c; float r; };

// Black, with the alpha doing all the work: dim outside the holes, clear inside, and a soft edge between. Same layout as the
// ghost: the left eye's half, then the right eye's, and v going down the plane.
inline void drawSpot(std::vector<uint8_t>& rgba, int W, int H, const GhostRenderer::Plane& pl, const V3 eyes[2], const SpotHole* holes, int count, float dim, float softM = 0.02f) {
  rgba.resize(size_t(2 * W) * H * 4);
  const float top = dim * 255.f;
  for (int e = 0; e < 2; ++e) {
    const V3 E = eyes[e];
    for (int py = 0; py < H; ++py) {
      const float v = (py + 0.5f) / H;
      const V3 row = pl.c + pl.Y * ((0.5f - v) * pl.S) - pl.X * (0.5f * pl.S);
      uint8_t* out = rgba.data() + (size_t(py) * 2 * W + size_t(e) * W) * 4;
      for (int px = 0; px < W; ++px, out += 4) {
        const V3 P = row + pl.X * ((px + 0.5f) / W * pl.S);
        const V3 d = norm(P - E);
        // How far outside the nearest globe this eye's line of sight passes.
        float outside = 1e9f;
        for (int i = 0; i < count; ++i) {
          const V3 toC = holes[i].c - E;
          const float along = dot(toC, d);
          const float miss = along > 0 ? len(toC - d * along) : len(toC);
          outside = std::min(outside, miss - holes[i].r);
        }
        const float t = std::clamp((outside + softM * 0.5f) / softM, 0.f, 1.f);
        out[0] = out[1] = out[2] = 0;
        out[3] = uint8_t(top * t * t * (3 - 2 * t));
      }
    }
  }
}

}  // namespace fh

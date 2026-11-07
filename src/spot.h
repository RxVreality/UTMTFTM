// The see-through globes, drawn per eye like the ghost, on a plane facing the headset. Headsets differ, so there are two ways in.
//
// On a Frame, SteamVR won't show the cameras in just one spot. It blends them over the whole view (Room View, translucent), it
// doesn't give the picture to anyone else, and an overlay can only draw on top. So drawSpot comes at it from the other side: with
// Room View on, everything outside the globes goes dark, and the only place left to look is where your real hands are.
//
// Virtual Desktop can turn one exact colour into the real room, after the picture reaches the headset. So anything drawn in that
// colour is a real window, and drawKeyWindows draws the globes in it. The catch is that an overlay sits on top of the game, so a
// solid window hides the in-game hand too. Stripes leave gaps for it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ghost3d.h"

namespace fh {

struct SpotHole { V3 c; float r; };

// How far outside the nearest globe this eye's line of sight through P passes. Negative is inside.
inline float spotMiss(const V3& E, const V3& P, const SpotHole* holes, int count) {
  const V3 d = norm(P - E);
  float outside = 1e9f;
  for (int i = 0; i < count; ++i) {
    const V3 toC = holes[i].c - E;
    const float along = dot(toC, d);
    const float miss = along > 0 ? len(toC - d * along) : len(toC);
    outside = std::min(outside, miss - holes[i].r);
  }
  return outside;
}

// Hands paint(pixel, row, how far outside a globe) every pixel of both eyes. Only pixels near a globe's edge care about the exact
// distance, so it's looked at in 8 pixel blocks: a block well clear of any edge (by more than its corners differ) gets one value,
// and the rest work out every pixel. That's what makes a sharp picture cheap enough to redraw 30 times a second on a headset.
// Filling in between the corners is cheaper still, but it turns the circles into lumpy polygons. edge is how far either side of
// a globe's outline the paint changes at all. Same layout as the ghost: the left eye's half, then the right eye's, and v going down the plane.
template <class Paint>
inline void forSpotPixels(std::vector<uint8_t>& rgba, int W, int H, const GhostRenderer::Plane& pl, const V3 eyes[2], const SpotHole* holes, int count, float edge, Paint paint) {
  constexpr int kStep = 8;
  rgba.resize(size_t(2 * W) * H * 4);
  const int gw = W / kStep + 2, gh = H / kStep + 2;
  std::vector<float> grid(size_t(gw) * gh);
  const V3 across = pl.X * (pl.S / W), down = pl.Y * (-pl.S / H);
  const V3 corner = pl.c + pl.Y * (0.5f * pl.S) - pl.X * (0.5f * pl.S);
  for (int e = 0; e < 2; ++e) {
    const V3 E = eyes[e];
    for (int j = 0; j < gh; ++j)
      for (int i = 0; i < gw; ++i) grid[size_t(j) * gw + i] = spotMiss(E, corner + across * float(i * kStep) + down * float(j * kStep), holes, count);
    for (int j = 0; j + 1 < gh && j * kStep < H; ++j)
      for (int i = 0; i + 1 < gw && i * kStep < W; ++i) {
        const float* c = grid.data() + size_t(j) * gw + i;
        const float lo = std::min({c[0], c[1], c[gw], c[gw + 1]}), hi = std::max({c[0], c[1], c[gw], c[gw + 1]}), spread = hi - lo;
        const bool clear = lo - spread > edge || hi + spread < -edge;
        for (int py = j * kStep; py < std::min(H, (j + 1) * kStep); ++py) {
          uint8_t* out = rgba.data() + (size_t(py) * 2 * W + size_t(e) * W + i * kStep) * 4;
          for (int px = i * kStep; px < std::min(W, (i + 1) * kStep); ++px, out += 4)
            paint(out, py, clear ? lo : spotMiss(E, corner + across * (px + 0.5f) + down * (py + 0.5f), holes, count));
        }
      }
  }
}

// Black, with the alpha doing all the work: dim outside the holes, clear inside, and a soft edge between.
inline void drawSpot(std::vector<uint8_t>& rgba, int W, int H, const GhostRenderer::Plane& pl, const V3 eyes[2], const SpotHole* holes, int count, float dim, float softM = 0.03f) {
  const float top = dim * 255.f;
  forSpotPixels(rgba, W, H, pl, eyes, holes, count, softM * 0.5f, [&](uint8_t* out, int, float outside) {
    const float t = std::clamp((outside + softM * 0.5f) / softM, 0.f, 1.f);
    out[0] = out[1] = out[2] = 0;
    out[3] = uint8_t(top * t * t * (3 - 2 * t));
  });
}

// The colour Virtual Desktop is set to turn into the room. Every pixel has it, even the clear ones, so that blending between
// pixels can only ever fade this colour out and never mixes in a darker one.
constexpr uint8_t kKeyR = 255, kKeyG = 0, kKeyB = 255;
constexpr int kKeyStripePx = 5;  // whole rows, so they stay crisp, and level, so they look the same from both eyes

// The globes in that colour, and nothing anywhere else. The edge is one pixel: a soft one would be neither colour nor game.
inline void drawKeyWindows(std::vector<uint8_t>& rgba, int W, int H, const GhostRenderer::Plane& pl, const V3 eyes[2], const SpotHole* holes, int count, bool stripes) {
  const float pixel = pl.S / W;
  forSpotPixels(rgba, W, H, pl, eyes, holes, count, pixel * 0.5f, [&](uint8_t* out, int py, float outside) {
    float a = std::clamp(0.5f - outside / pixel, 0.f, 1.f);
    if (stripes && (py / kKeyStripePx) % 2) a = 0;
    out[0] = kKeyR; out[1] = kKeyG; out[2] = kKeyB;
    out[3] = uint8_t(a * 255.f + 0.5f);
  });
}

// How wide a plane has to be for both eyes to see all of every globe on it, and a little room for the hands to move before it's
// redone. It's only a window around the hands, so it stays small and its pixels stay fine. It follows each globe's outline as that
// eye sees it, a circle that lands on the plane stretched when the plane's at an angle. When there's no fitting it (your head in a
// globe, or your hands out either side of you, where you can't see both at once anyway) it stops at the most that's useful.
inline float keyPlaneSize(const GhostRenderer::Plane& pl, const V3 eyes[2], const SpotHole* holes, int count) {
  constexpr float kMoveRoom = 0.2f, kMost = 4.8f;
  constexpr int kAround = 16;
  const V3 away = pl.N * -1.f;
  float reach = 0;
  for (int e = 0; e < 2; ++e) {
    const V3 E = eyes[e];
    const float planeAway = dot(pl.c - E, away);
    for (int i = 0; i < count; ++i) {
      const V3 toC = holes[i].c - E;
      const float D = len(toC), r = holes[i].r;
      if (r >= 0.95f * D) return kMost;
      const V3 a = toC * (1.f / D);
      const V3 side = norm(std::fabs(a.y) > 0.9f ? cross(a, V3{1, 0, 0}) : cross(a, V3{0, 1, 0})), over = cross(a, side);
      const V3 ring = a * (D - r * r / D);
      const float ringR = r * std::sqrt(1 - (r / D) * (r / D));
      for (int k = 0; k < kAround; ++k) {
        const float th = 6.2831853f * k / kAround;
        const V3 sight = ring + side * (ringR * std::cos(th)) + over * (ringR * std::sin(th));
        const float facing = dot(sight, away);
        if (facing <= 0.02f * len(sight)) return kMost;
        const V3 onPlane = E + sight * (planeAway / facing) - pl.c;
        reach = std::max({reach, std::fabs(dot(onPlane, pl.X)), std::fabs(dot(onPlane, pl.Y))});
      }
    }
  }
  return std::min(2 * reach * 1.03f + kMoveRoom, kMost);
}

}  // namespace fh

// Placement math shared by the overlay and the tests (so the tests exercise the real code).
#pragma once
#include "xform.h"

namespace fh {

// What SteamVR reports for one controller while placing.
struct ControllerPoses {
  bool valid = false, gripValid = false;
  M raw = ident(), grip = ident();
};

// Turns what SteamVR reports into where the controller really is, for the placing maths.
// With the driver in use, every reported pose has a published offset in it, a few updates old, and the raw and
// grip poses needn't carry the same one. So nothing may be worked out from them: it would go into the next offset,
// come back in the poses, and grow until the hand is flung away. The position comes from the driver's own report,
// and the grip frame stays as SteamVR's table gave it.
inline void trueControllerPoses(ControllerPoses& H, M& gripFromRaw, bool useDriver, const M* driverReport, const M& standingFromRaw) {
  if (!H.valid) return;
  if (!useDriver) {
    if (H.gripValid) gripFromRaw = mul(inv(H.raw), H.grip);
    return;
  }
  if (!driverReport) { H.valid = false; return; }
  H.raw = mul(standingFromRaw, *driverReport);
  H.grip = mul(H.raw, gripFromRaw);
}

// Where a game draws this hand's grip once offset o is applied.
//   live (driver add-on):  grip * O
//   binding route:         SteamVR offsets the raw pose: raw * O * (raw^-1 * grip)
inline M gripWithOffset(bool live, const M& raw, const M& grip, const Offset& o) {
  if (live) return mul(grip, fromOffset(o));
  M C = mul(inv(raw), grip);
  return mul(mul(raw, fromOffset(o)), C);
}

// Place mode: the grabbing hand holds the ghost at world pose W; the ghost is drawn at
// gripWithOffset(o) * base. Solve for the offset o that puts it there.
inline Offset offsetFromHeld(bool live, const M& raw, const M& grip, const M& W, const Offset& base) {
  M Gn = mul(W, inv(fromOffset(base)));
  if (live) return toOffset(mul(inv(grip), Gn));
  M C = mul(inv(raw), grip);
  return toOffset(mul(mul(inv(raw), Gn), inv(C)));
}

// Match mode: the offset stays put (grip-with-offset G); the ghost's resting pose moves to W.
inline Offset baseFromHeld(const M& G, const M& W) { return toOffset(mul(inv(G), W)); }

// Resting pose actually used for a hand: matched, else the other hand mirrored, else the spec anchor.
inline Offset ghostBaseFor(const Offset base[2], const bool matched[2], int h) {
  if (matched[h]) return base[h];
  if (matched[1 - h]) return mirrorOffset(base[1 - h]);
  return Offset{};
}

// Size of the drawing plane for the distance-limit bubble (radius r, centred on the controller).
// Seen from r + 0.2 m or further, the bubble's silhouette is at most r / sqrt(1 - (r/d)^2) <= 1.25 r
// from the centre, so 2.4 r + 0.1 always contains it.
inline float bubblePlaneSize(float r) { return 2.4f * r + 0.10f; }

}  // namespace fh

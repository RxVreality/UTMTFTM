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

// Fine adjust: one tap on one of the six numbers on the page. axis: 0 forward, 1 up, 2 right, 3 pitch, 4 yaw, 5 roll.
// dir is which way that number goes, and all six read positive the way their name says: forward, up, right, nose up,
// turned right, rolled clockwise. Distance keeps to the limit, unless the hand's already past it and this brings it
// closer. Rotation never has a limit, it just wraps at 180. limited: the tap was refused, and o comes back as it was.
inline Offset nudged(Offset o, int axis, int dir, float stepMetres, float stepDegrees, float maxPos, bool* limited = nullptr) {
  if (limited) *limited = false;
  const Offset before = o;
  const float metres = dir * stepMetres, degrees = dir * stepDegrees;
  switch (axis) {
    case 0: o.pos[2] -= metres; break;
    case 1: o.pos[1] += metres; break;
    case 2: o.pos[0] += metres; break;
    case 3: o.rot[0] += degrees; break;
    case 4: o.rot[1] -= degrees; break;  // the stored angle turns left as it grows
    case 5: o.rot[2] -= degrees; break;  // and rolls anticlockwise
    default: return o;
  }
  if (axis < 3 && posLen(o) > maxPos + 1e-4f && posLen(o) > posLen(before)) {
    if (limited) *limited = true;
    return before;
  }
  // Tenths of a millimetre and hundredths of a degree, so a tap there and a tap back land exactly where they started.
  for (float& p : o.pos) p = std::round(p * 1e4f) / 1e4f;
  for (float& r : o.rot) {
    r = std::round(r * 100.f) / 100.f;
    if (r > 180.f) r -= 360.f;
    else if (r <= -180.f) r += 360.f;
  }
  return o;
}

// Size of the drawing plane for the distance-limit bubble (radius r, centred on the controller).
// Seen from r + 0.2 m or further, the bubble's silhouette is at most r / sqrt(1 - (r/d)^2) <= 1.25 r
// from the centre, so 2.4 r + 0.1 always contains it.
inline float bubblePlaneSize(float r) { return 2.4f * r + 0.10f; }

}  // namespace fh

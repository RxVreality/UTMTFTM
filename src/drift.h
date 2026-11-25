// Stick drift: a worn stick that doesn't come back to the middle when you let go (J4, 14 November, for Quest controllers).
// Pure maths, no VR calls. The driver runs fix() on every stick report (it has to stay cheap and never allocate); the overlay
// runs the stick check through the rest of this, to work out the numbers it sends; the tests run both on made-up sticks.
//
// What a worn stick does, and what each part here does about it:
//   - It rests off the middle. fix() measures everything from where it rests instead (Params cx, cy).
//   - It lands somewhere a little different each time it springs back, depending on which way it came from, and it jitters
//     there. A round dead zone around the resting spot, just big enough to hold every landing and the jitter (dead).
//   - It wanders over the hours. While your thumb's off the stick, whatever it reports near the middle is drift by definition,
//     so the middle slowly follows it (learnPerSec, learnMax), and anything that close reads as the middle (gate).
//   - It no longer reaches the edge in some direction. A full push there is stretched to count as all the way (reach).
//   - A worn track throws the odd spike: one report well out and straight back. From the middle, a jump like that is held
//     back for 6 ms; a real push is still there after that, a spike isn't.
//   - It flicks (J4's Quest 3 sticks, 18 November): the reading jumps, faster than a thumb can move, to another point and
//     straight back, like dust or a worn patch on the track. Where it jumps from changes from one go round to the next, so
//     it's caught by what it looks like, not where (J4, 20 November: "proximity does not determine where an area jumps"; his
//     real check files, 0.6.4's start map caught half): away from the middle, a jump of half the stick or more that isn't a
//     let-go or a push on further out is held at the last whole report until it's back, has stayed there, keeps going, or
//     four reports have gone by (Flicks).
// Past the dead zone the push is stretched so it still starts from nothing and still reaches all the way: no jump at its edge.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace drift {

constexpr int kDirs = 8;     // reach is measured in eight directions
constexpr int kSlices = 32;  // and the edge is traced in 32 slices
constexpr float kPi = 3.14159265358979f;

// What fixing one stick takes. The overlay works these out from the stick check, the driver applies them. All zero: off.
// The same layout as fhlive::DriftSettings, which is what goes through live.shm.
struct Params {
  uint32_t on = 0;
  float cx = 0, cy = 0;     // where the stick rests, as it reports it
  float dead = 0;           // this close to there reads as the middle
  float gate = 0;           // with the thumb off the stick, this close reads as the middle too. 0: don't go by touch
  float learnPerSec = 0;    // with the thumb off and the stick near the middle, how fast the middle follows it. 0: it doesn't
  float learnMax = 0;       // how far from cx, cy that may take it
  float reach[kDirs] = {};  // how far from cx, cy a full push gets, east first then anticlockwise. 0: as far as the stick goes
};

// How a stick that flicks has its flicks held back. The same layout as fhlive::FlickSettings. All zero: none are (the
// middle's own spike hold still works). Measured from the middle (Params cx, cy plus what's been learned).
struct Flicks {
  uint32_t on = 0;   // hold them back
  float big = 0;     // a jump between two reports at least this big...
  float out = 0;     // ...from at least this far out, landing outside the middle, and not on further out the same way
  float most = 0;    // held for this many reports at most
  float stay = 0;    // a new report this close to the one before it: it stayed there, so it's real
  float back = 0;    // back within this share of the jump (and 0.12 at least) of where it was: it was a flick
  uint32_t spare[2] = {};
};
// The numbers, from J4's three real check files of 20 November replayed through it (tests/data/j4-2026-11-20.txt, test_drift):
// big flicks reaching the game, 16 of 31 with 0.6.4's first test build's start map, 4 with this; his good right stick's real
// moves held up once, for one report, in all three checks (a model of 0.6.4's rule held them up 161 times).
constexpr float kHoldBig = 0.45f, kHoldOut = 0.5f, kHoldMost = 4, kHoldStay = 0.15f, kHoldBack = 0.4f;
inline Flicks holdFlicks() {
  Flicks f;
  f.on = 1;
  f.big = kHoldBig;
  f.out = kHoldOut;
  f.most = kHoldMost;
  f.stay = kHoldStay;
  f.back = kHoldBack;
  return f;
}

inline float len(float x, float y) { return std::sqrt(x * x + y * y); }
inline float fin(float v, float lo, float hi) { return std::isfinite(v) ? std::min(std::max(v, lo), hi) : 0.f; }

// Whatever's in the file, nothing silly gets through.
inline Params sane(const Params& in) {
  Params p;
  if (!in.on) return p;
  p.on = 1;
  p.cx = fin(in.cx, -0.5f, 0.5f);
  p.cy = fin(in.cy, -0.5f, 0.5f);
  p.dead = fin(in.dead, 0.f, 0.5f);
  p.gate = fin(in.gate, 0.f, 0.5f);
  p.learnPerSec = fin(in.learnPerSec, 0.f, 5.f);
  p.learnMax = fin(in.learnMax, 0.f, 0.3f);
  for (int k = 0; k < kDirs; ++k) p.reach[k] = in.reach[k] > 0 ? fin(in.reach[k], 0.4f, 1.6f) : 0.f;
  return p;
}
inline Flicks sane(const Flicks& in) {
  Flicks f;
  if (!in.on) return f;
  f.big = fin(in.big, 0.2f, 2.f);
  f.out = fin(in.out, 0.f, 0.95f);
  f.most = fin(in.most, 0.f, 6.f);
  f.stay = fin(in.stay, 0.f, 0.3f);
  f.back = fin(in.back, 0.f, 0.8f);
  f.on = f.big > 0 && f.most >= 1 ? 1 : 0;
  if (!f.on) f = Flicks{};
  return f;
}
inline bool any(const Flicks& f) { return f.on != 0; }

// Which slice of the edge a point is in, measured from the middle: 32 round, 0 to the right, anticlockwise.
inline int sliceOf(float dx, float dy) {
  float a = std::atan2(dy, dx);
  if (a < 0) a += 2 * kPi;
  return std::min(kSlices - 1, int(a / (2 * kPi) * kSlices));
}

// One stick's running state, the driver's own.
struct State {
  float lx = 0, ly = 0;  // the middle as learned while playing, measured from cx, cy
  double lastT = -1;
  double farOff = 0;     // how long it's been pushed well past the gate with no thumb felt
  bool touchBad = false; // that happened for long enough: the touch sensor misses, so it isn't believed any more
  bool atMiddle = true;  // the game was last given the middle
  float px = 0, py = 0;  // the last report
  double jumpT = -1;     // when it jumped out from the middle in one go, while that's being held back
  uint32_t spikes = 0;   // jumps that came straight back: held back, never reached the game
  // Flicks, away from the middle.
  double gap = 0.011;    // how far apart its reports come, on average, in seconds
  float gx = 0, gy = 0;  // the last report that went on to the game as it was
  float qx = 0, qy = 0;  // and as it was when this report began. Some controllers send x and y as two calls, a moment apart:
                         // a flick that shows in the second is held from before the first, so half of it doesn't get out
  double holdT = -1;     // a jump away from there is being held back since then, to see if it comes straight back
  double holdW = 0;      // for this long at most (for the driver's own clock, when the controller goes quiet)
  float jump = 0;        // how far the jump that started it went
  float speed = 0;       // how far the stick was going each report just before it
  float furthest = 0;    // the furthest from gx, gy a whole report's been since
  int held = 0;          // reports since it started
  float hx = 0, hy = 0;  // the latest call while it's held
  float ex = 0, ey = 0;  // and where the report before this one ended
  double letT = -1;      // when the last one was let go: not held again in the same report
  float vx = 0, vy = 0;  // which way the stick was going: from where the report before last began to where the last began
  bool twoCalls = false; // this controller sends x and y as two calls: a report is only whole with the second
  bool retake = false;   // this call changed the other axis's answer from what its own call just sent: send that again
  uint32_t flicks = 0;   // jumps away from the middle that came straight back: held back, never reached the game
};

// How far from the middle (mx, my) the stick can go along the unit direction (dx, dy): as measured, or else to the edge of
// the round range a stick reports.
inline float reachAt(const Params& p, float mx, float my, float dx, float dy) {
  float a = std::atan2(dy, dx);
  if (a < 0) a += 2 * kPi;
  const float f = a / (2 * kPi / kDirs);
  const int k0 = int(f) % kDirs, k1 = (k0 + 1) % kDirs;
  const float w = f - std::floor(f);
  const float b = mx * dx + my * dy, toEdge = -b + std::sqrt(std::max(0.f, b * b - (mx * mx + my * my - 1)));
  const float r0 = p.reach[k0] > 0 ? p.reach[k0] : toEdge, r1 = p.reach[k1] > 0 ? p.reach[k1] : toEdge;
  return r0 + (r1 - r0) * w;
}

// A held flick is due out (it hasn't come back in time, and the controller hasn't reported again): the driver's RunFrame asks.
inline bool holdDue(const State& s, double now) { return s.holdT >= 0 && now - s.holdT >= s.holdW; }

// Away from the middle, one call (raw x, y). newReport: not the second half of a report that began a moment ago. True while
// it's a jump being held back, and the game should keep gx, gy.
inline bool holdFlick(const Flicks& fl, State& s, float x, float y, float dead, float mx, float my, double now, bool newReport) {
  const float r = len(x - mx, y - my);
  if (s.holdT >= 0) {
    if (newReport) {
      ++s.held;
      s.ex = s.hx;
      s.ey = s.hy;
    }
    // Where it's got to, from where it was. Whether it stayed or kept going is only asked of a whole report: the first of two
    // calls has the new x with the old y, which can be half way between a flick and where the thumb is.
    const bool whole = !s.twoCalls || !newReport;
    const float d = len(x - s.gx, y - s.gy), moved = len(x - s.ex, y - s.ey);
    bool keep = false;
    if (r <= dead) {
      // In the middle: let go. A spring back can overshoot the other way for a report on its way there; the middle goes now.
    } else if (d <= std::max(0.12f, fl.back * s.jump) + s.speed * float(s.held)) {
      ++s.flicks;  // back where it was, or where the thumb's got to since: a flick, and the game never saw it
    } else if (whole && s.held >= 1 && d > s.furthest + 0.1f) {
      // Still going away on a later report: a real sweep right across. It goes now.
    } else if (whole && s.held >= 2 && moved > 1e-4f && moved < fl.stay) {
      // A second new report (not the same one sent again) where the last one was: it stayed there, so it's real. One isn't
      // enough: a flick can last two reports.
    } else if (s.held >= int(fl.most) || now - s.holdT >= s.holdW) {
      // Held long enough: the flicks in J4's real check files were all back within four reports.
    } else {
      keep = true;
    }
    if (keep) {
      s.hx = x;
      s.hy = y;
      if (whole) s.furthest = std::max(s.furthest, d);
      if (s.held == 0) s.jump = std::max(s.jump, d);  // the jump's whole size, once the second call of its report shows it
      return true;
    }
    s.holdT = -1;
    s.letT = now;
    s.retake = s.retake || !newReport;  // the first half of this report went out held
  } else if (fl.on && (newReport || now - s.letT > 0.002)) {
    // Measured from the last whole report: the first half of this one may be half of a flick.
    const float bx = newReport ? s.gx : s.qx, by = newReport ? s.gy : s.qy;
    const float jx = x - bx, jy = y - by, j = len(jx, jy), rb = len(bx - mx, by - my), v = len(s.vx, s.vy);
    // On out the same way, within 25 degrees: a push. On round the edge the way it was already going: a fast roll. A let-go
    // lands in the middle. None of those is ever held: a flick goes somewhere else, and on J4's sticks it went back the way
    // they'd come, or right across into the middle.
    const bool further = r > rb && (x - mx) * (bx - mx) + (y - my) * (by - my) > 0.906f * r * rb;
    const bool onward = rb > 0.85f && r > 0.85f && v > 0.03f && jx * s.vx + jy * s.vy > 0.5f * j * v;
    // And only from somewhere the stick was steady: where it had just jumped to can be a flick from the middle that's gone
    // through (those are never held), and coming back from there is the thumb, not another flick.
    const bool steady = v < fl.big;
    if (j >= fl.big && rb >= fl.out && r > dead && !further && !onward && steady) {
      s.holdT = now;
      s.holdW = std::min(0.08, std::max(0.004, (double(fl.most) + 0.5) * s.gap));
      s.jump = s.furthest = j;
      s.speed = std::min(v, 0.3f);
      s.held = 0;
      s.hx = s.ex = x;
      s.hy = s.ey = y;
      s.retake = s.retake || (!newReport && (bx != s.gx || by != s.gy));
      s.gx = bx;
      s.gy = by;
      return true;
    }
  }
  s.gx = x;
  s.gy = y;
  return false;
}

// One report. touch: 1 a thumb's on the stick, 0 it isn't, -1 this stick can't tell. now in seconds, any steady clock.
inline void fix(const Params& p, const Flicks& fl, State& s, float x, float y, int touch, double now, float& ox, float& oy) {
  if (!p.on) { ox = x; oy = y; return; }
  const double dt = s.lastT < 0 ? 0 : std::min(std::max(now - s.lastT, 0.0), 0.1);
  const bool newReport = s.lastT < 0 || now - s.lastT > 0.002;  // x and y as two calls come well under a millisecond apart
  s.lastT = now;
  if (newReport) {
    s.vx = s.gx - s.qx;
    s.vy = s.gy - s.qy;
    s.qx = s.gx;
    s.qy = s.gy;
  } else {
    s.twoCalls = true;
  }
  if (dt > 0.0003) s.gap += (std::min(dt, 0.02) - s.gap) * 0.05;
  const float mx = p.cx + s.lx, my = p.cy + s.ly;
  float vx = x - mx, vy = y - my, r = std::sqrt(vx * vx + vy * vy);
  const bool byTouch = p.gate > 0 && touch >= 0 && !s.touchBad;
  // Pushed well past the gate for a quarter of a second with no thumb felt: this sensor misses (a nail, a glove, wear), so it
  // stops counting. A flick's spring back is far shorter than that.
  if (byTouch && touch == 0 && r > p.gate + 0.2f) {
    s.farOff += dt;
    if (s.farOff > 0.25) s.touchBad = true;
  } else {
    s.farOff = 0;
  }
  const bool off = byTouch && touch == 0 && !s.touchBad;
  if (off && r < p.gate) {
    // Thumb off and near the middle: that's the stick drifting, nothing else. Follow it, slowly.
    if (p.learnPerSec > 0 && r < std::max(p.dead, 0.05f) + 0.1f) {
      const float a = float(std::min(1.0, p.learnPerSec * dt));
      float lx = s.lx + vx * a, ly = s.ly + vy * a;
      const float l = std::sqrt(lx * lx + ly * ly);
      if (l > p.learnMax) { lx *= p.learnMax / l; ly *= p.learnMax / l; }
      s.lx = lx;
      s.ly = ly;
    }
    s.atMiddle = true;
    s.jumpT = -1;
    s.holdT = -1;
    s.px = x;
    s.py = y;
    ox = oy = 0;
    return;
  }
  // A jump out from the middle in one report: wait 6 ms to see if it stays.
  const float jump = len(x - s.px, y - s.py);
  s.px = x;
  s.py = y;
  // Away from the middle, a flick: the game keeps the last good report until it's back, or has stayed.
  if (!s.atMiddle && (s.holdT >= 0 || any(fl)) && holdFlick(fl, s, x, y, p.dead, mx, my, now, newReport)) {
    vx = s.gx - mx;
    vy = s.gy - my;
    r = std::sqrt(vx * vx + vy * vy);
  }
  if (r <= p.dead || r < 1e-6f) {
    if (s.jumpT >= 0) ++s.spikes;
    s.jumpT = -1;
    s.holdT = -1;
    s.atMiddle = true;
    ox = oy = 0;
    return;
  }
  if (s.atMiddle && s.jumpT < 0 && jump > 0.08f) s.jumpT = now;
  if (s.jumpT >= 0) {
    if (now - s.jumpT < 0.006) { ox = oy = 0; return; }
    s.jumpT = -1;
  }
  if (s.atMiddle) {  // out from the middle: this is where flicks are measured from, the whole report too
    s.gx = s.qx = x;
    s.gy = s.qy = y;
    s.vx = s.vy = 0;
  }
  s.atMiddle = false;
  const float dx = vx / r, dy = vy / r;
  const float most = std::max(reachAt(p, mx, my, dx, dy), p.dead + 0.05f);
  const float t = std::min(1.f, (r - p.dead) / (most - p.dead));
  ox = dx * t;
  oy = dy * t;
}
// A stick with no flick map: 0.6.3's fix.
inline void fix(const Params& p, State& s, float x, float y, int touch, double now, float& ox, float& oy) {
  static const Flicks none;
  fix(p, none, s, x, y, touch, now, ox, oy);
}

// ---------------------------------------------------------------- the stick check, for the overlay
struct Sample { float x, y; int touch; double t; };  // touch as for fix(). t in seconds
struct Dot { float x, y; };

// The smallest circle around all the points (Welzl, by the incremental way: these lists are short).
struct Circle { float x = 0, y = 0, r = 0; };
inline Circle circleOf(std::vector<Dot> pts) {
  Circle c;
  if (pts.empty()) return c;
  const auto in = [](const Circle& k, const Dot& d) { return len(d.x - k.x, d.y - k.y) <= k.r + 1e-5f; };
  const auto two = [](const Dot& a, const Dot& b) { return Circle{(a.x + b.x) / 2, (a.y + b.y) / 2, len(a.x - b.x, a.y - b.y) / 2}; };
  const auto three = [&](const Dot& a, const Dot& b, const Dot& d) {
    const float bx = b.x - a.x, by = b.y - a.y, cx = d.x - a.x, cy = d.y - a.y, den = 2 * (bx * cy - by * cx);
    if (std::fabs(den) < 1e-9f) {  // in a line: the two furthest apart
      Circle k = two(a, b), k2 = two(a, d), k3 = two(b, d);
      if (k2.r > k.r) k = k2;
      if (k3.r > k.r) k = k3;
      return k;
    }
    const float ux = (cy * (bx * bx + by * by) - by * (cx * cx + cy * cy)) / den, uy = (bx * (cx * cx + cy * cy) - cx * (bx * bx + by * by)) / den;
    return Circle{a.x + ux, a.y + uy, len(ux, uy)};
  };
  c = {pts[0].x, pts[0].y, 0};
  for (size_t i = 1; i < pts.size(); ++i) {
    if (in(c, pts[i])) continue;
    c = {pts[i].x, pts[i].y, 0};
    for (size_t j = 0; j < i; ++j) {
      if (in(c, pts[j])) continue;
      c = two(pts[i], pts[j]);
      for (size_t k = 0; k < j; ++k)
        if (!in(c, pts[k])) c = three(pts[i], pts[j], pts[k]);
    }
  }
  return c;
}

inline float percentile(std::vector<float> v, float q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5f))];
}

// Step 1, left alone: where it rests (the middle of where it's been, each way on its own, so a spike can't drag it) and how much
// it jitters there. Only samples with no thumb on the stick count, when it can tell.
struct Rest { bool ok = false; float x = 0, y = 0, jitter = 0; int used = 0; bool touched = false; bool pushed = false; };
inline Rest restOf(const std::vector<Sample>& v) {
  Rest r;
  std::vector<float> xs, ys;
  int touched = 0;
  for (const Sample& s : v) {
    if (s.touch == 1) { ++touched; continue; }
    xs.push_back(s.x);
    ys.push_back(s.y);
  }
  r.touched = touched > int(v.size() / 3);
  r.used = int(xs.size());
  if (xs.size() < 20) return r;
  r.x = percentile(xs, 0.5f);
  r.y = percentile(ys, 0.5f);
  std::vector<float> d;
  for (size_t i = 0; i < xs.size(); ++i) d.push_back(len(xs[i] - r.x, ys[i] - r.y));
  r.jitter = percentile(d, 0.98f);
  r.pushed = r.jitter > 0.3f;  // someone moved it: that's not resting
  r.ok = !r.pushed;
  return r;
}

// Steps 2 and 3: the flicks in what the stick reported. A flick is a report (or a few) that leaves the way the stick was going
// and comes straight back: from a, out to the furthest report, and back to where the thumb has got to in the meantime, within
// 45 ms, the out point much further from that path than the path is long. A thumb can't push out and come back that fast; a
// let-go never comes back out. From near the middle doesn't count: the rest and the landings measure that.
struct Flick { float fromX, fromY, toX, toY; float size; double t, ms; size_t first, end; };  // [first, end) the reports out
constexpr float kFlickMin = 0.08f;
constexpr float kFlickBig = 0.15f;  // one this big is enough to call it worn
inline float toSegment(float px, float py, float ax, float ay, float bx, float by) {
  const float ux = bx - ax, uy = by - ay, l2 = ux * ux + uy * uy;
  const float k = l2 > 1e-12f ? std::min(1.f, std::max(0.f, ((px - ax) * ux + (py - ay) * uy) / l2)) : 0.f;
  return len(px - ax - ux * k, py - ay - uy * k);
}
inline std::vector<Flick> flicksOf(const std::vector<Sample>& v, float cx, float cy) {
  std::vector<Flick> out;
  for (size_t i = 1; i < v.size(); ++i) {
    const Sample& a = v[i - 1];
    if (len(v[i].x - a.x, v[i].y - a.y) < kFlickMin || len(a.x - cx, a.y - cy) < 0.2f) continue;
    for (size_t j = i + 1; j < v.size() && v[j].t - a.t <= 0.045; ++j) {
      const float chord = len(v[j].x - a.x, v[j].y - a.y);
      float dev = 0;
      size_t pk = i;
      for (size_t k = i; k < j; ++k) {
        const float d = toSegment(v[k].x, v[k].y, a.x, a.y, v[j].x, v[j].y);
        if (d > dev) { dev = d; pk = k; }
      }
      if (dev >= kFlickMin && chord <= 0.5f * dev) {
        out.push_back({a.x, a.y, v[pk].x, v[pk].y, dev, v[i].t, (v[j].t - v[i].t) * 1000, i, j});
        i = j;
        break;
      }
    }
  }
  return out;
}
// One sample per report: a controller that sends x and y as two calls a moment apart shows up as two samples, the first with
// the new x and the old y. Half a report like that can look like a flick of its own, so only the whole one is kept.
inline std::vector<Sample> wholeReports(const std::vector<Sample>& v) {
  std::vector<Sample> out;
  out.reserve(v.size());
  for (const Sample& s : v) {
    if (!out.empty() && s.t - out.back().t < 0.002 && s.t >= out.back().t) out.back() = s;
    else out.push_back(s);
  }
  return out;
}
// The samples with the flicks taken out: what the thumb really did.
inline std::vector<Sample> withoutFlicks(const std::vector<Sample>& v, const std::vector<Flick>& fl) {
  std::vector<Sample> out;
  out.reserve(v.size());
  size_t f = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    while (f < fl.size() && fl[f].end <= i) ++f;
    if (f < fl.size() && i >= fl[f].first && i < fl[f].end) continue;
    out.push_back(v[i]);
  }
  return out;
}
// How far apart its reports come: the middle of the gaps between reports that aren't at the same moment.
inline double reportGap(const std::vector<Sample>& v) {
  std::vector<float> g;
  for (size_t i = 1; i < v.size(); ++i)
    if (v[i].t - v[i - 1].t > 0.0003) g.push_back(float(v[i].t - v[i - 1].t));
  return g.empty() ? 0.011 : percentile(g, 0.5f);
}

// Step 2, round the edge: how far it got from (cx, cy) in each of 32 slices. 0: it hasn't been out that way yet.
inline std::vector<float> edgeOf(const std::vector<Sample>& v, float cx, float cy) {
  std::vector<float> reach(kSlices, 0.f);
  for (const Sample& s : v) {
    const float dx = s.x - cx, dy = s.y - cy, r = len(dx, dy);
    if (r < 0.6f) continue;  // only pushes most of the way out count as the edge
    const int i = sliceOf(dx, dy);
    reach[i] = std::max(reach[i], r);
  }
  return reach;
}
inline float edgeDone(const std::vector<float>& reach) {
  int n = 0;
  for (float r : reach) n += r > 0;
  return float(n) / kSlices;
}

// Step 3, flick and let go: where it came to rest each time, and which way it had been pushed. A let-go is a push most of the
// way out, then back near the middle and still for 0.15 s.
struct Landing { float x, y; float fromAngle; };
inline std::vector<Landing> landingsOf(const std::vector<Sample>& v, float cx, float cy) {
  std::vector<Landing> out;
  enum { Idle, Out, Back } st = Idle;
  float outAngle = 0, outR = 0;
  size_t w0 = 0;  // while it's back near the middle: where the still stretch starts
  const auto still = [&](size_t a, size_t i) {
    float lo[2] = {9, 9}, hi[2] = {-9, -9};
    for (size_t k = a; k <= i; ++k) {
      lo[0] = std::min(lo[0], v[k].x); hi[0] = std::max(hi[0], v[k].x);
      lo[1] = std::min(lo[1], v[k].y); hi[1] = std::max(hi[1], v[k].y);
    }
    return hi[0] - lo[0] < 0.04f && hi[1] - lo[1] < 0.04f;
  };
  for (size_t i = 0; i < v.size(); ++i) {
    const float dx = v[i].x - cx, dy = v[i].y - cy, r = len(dx, dy);
    if (r > 0.7f) {
      if (st != Out) { st = Out; outR = 0; }
      if (r > outR) { outR = r; outAngle = std::atan2(dy, dx); }
      continue;
    }
    if (st == Out) {
      if (r < 0.4f) { st = Back; w0 = i; }
      continue;
    }
    if (st != Back) continue;
    while (w0 < i && !still(w0, i)) ++w0;
    if (v[i].t - v[w0].t >= 0.15 && i - w0 + 1 >= 3) {
      float sx = 0, sy = 0;
      for (size_t k = w0; k <= i; ++k) { sx += v[k].x; sy += v[k].y; }
      const float n = float(i - w0 + 1);
      out.push_back({sx / n, sy / n, outAngle});
      st = Idle;
    }
  }
  return out;
}
// How many of the four ways (up, right, down, left) it's been let go from, twice each, as 0..1.
inline float landingsDone(const std::vector<Landing>& l) {
  int per[4] = {0, 0, 0, 0};
  for (const Landing& d : l) {
    float a = d.fromAngle;
    if (a < 0) a += 2 * kPi;
    per[int(std::fmod(a + kPi / 4, 2 * kPi) / (kPi / 2)) % 4]++;
  }
  int got = 0;
  for (int k = 0; k < 4; ++k) got += std::min(per[k], 2);
  return got / 8.f;
}

// What the check found, and the numbers to send.
struct Findings {
  bool drifts = false;
  Params p;
  Flicks fl;          // how its flicks are held back, for the driver. All zero: it doesn't flick
  float offset = 0;   // how far it rests from the true middle
  float spread = 0;   // how far apart its landings are
  float jitter = 0;
  float worstReach = 1;  // the least of how far a full push gets, as a share of how far it should, where that's sure
  int worstDir = -1;     // and which way that is, 0 east then anticlockwise in eighths
  float reachSeen[kDirs] = {};  // how far it got each way, as a share, sure or not (0: not out that way), for the next check
  bool touchUsed = false;
  std::vector<Flick> flicks;  // every flick in steps 2 and 3
  uint32_t flickWays = 0;     // bit k: flicks started from way k (as worstDir)
  bool flicky = false;        // enough of them, again at the same spots, to call it worn
  double gap = 0;             // its reports come this far apart, seconds
  std::string verdict;
  std::vector<std::string> lines;
};

inline const char* wayName(float angle) {
  static const char* const names[8] = {"to the right", "up and to the right", "up", "up and to the left", "to the left", "down and to the left", "down", "down and to the right"};
  float a = angle;
  if (a < 0) a += 2 * kPi;
  return names[int(std::fmod(a + kPi / 8, 2 * kPi) / (kPi / 4)) % 8];
}
inline int wayOf(float angle) {
  float a = angle;
  if (a < 0) a += 2 * kPi;
  return int(std::fmod(a + kPi / 8, 2 * kPi) / (kPi / 4)) % 8;
}
inline const char* wayShort(int k) {
  static const char* const names[8] = {"right", "up-right", "up", "up-left", "left", "down-left", "down", "down-right"};
  return names[((k % 8) + 8) % 8];
}

// Everything the check heard from one stick.
struct CheckData {
  Rest rest;
  std::vector<Sample> edge;     // step 2, rolling round the edge. Empty: skipped
  std::vector<Sample> flick;    // step 3, flicking and letting go. Empty: skipped
  std::vector<Landing> landed;  // step 3's let-gos
  bool hasTouch = false;        // the controller can feel a thumb on the stick
  bool touchMissed = false;     // and it missed a real push
  float prevReach[kDirs] = {};  // what the last check of this controller saw each way (Findings::reachSeen). 0: nothing
};

inline Findings analyse(const CheckData& d) {
  Findings f;
  const Rest& rest = d.rest;
  std::vector<Dot> pts{{rest.x, rest.y}};
  for (const Landing& l : d.landed) pts.push_back({l.x, l.y});
  const Circle c = circleOf(pts);
  f.jitter = rest.jitter;
  f.spread = c.r * 2;
  f.offset = len(c.x, c.y);
  Params& p = f.p;
  p.cx = c.x;
  p.cy = c.y;
  p.dead = std::min(0.4f, std::max(0.04f, c.r + rest.jitter + 0.02f));

  // Flicks, in steps 2 and 3, a whole report at a time.
  const std::vector<Sample> edgeAll = wholeReports(d.edge), pushAll = wholeReports(d.flick);
  std::vector<Flick> fe = flicksOf(edgeAll, c.x, c.y), ff = flicksOf(pushAll, c.x, c.y);
  const std::vector<Sample> edge = withoutFlicks(edgeAll, fe), push = withoutFlicks(pushAll, ff);
  f.flicks = fe;
  f.flicks.insert(f.flicks.end(), ff.begin(), ff.end());
  f.gap = reportGap(d.edge.empty() ? d.flick : d.edge);
  for (const Flick& k : f.flicks) f.flickWays |= 1u << wayOf(std::atan2(k.fromY - c.y, k.fromX - c.x));
  // Worn: two or more, or one big one. One slow roll passes a spot once, so one is all a check may see; a stick that isn't
  // worn never jumps a sixth of the way and straight back. A small one-off could be anything.
  f.flicky = f.flicks.size() >= 2 || (f.flicks.size() == 1 && f.flicks[0].size >= kFlickBig);
  // Held back by what a flick looks like, wherever it happens: where they start moved from one check to the next on J4's
  // sticks (6, then 7, then 4 spots), so a map of them missed most.
  if (f.flicky) f.fl = holdFlicks();

  // Reach: for each of the eight ways, the shortest of the four slices around it while rolling, or how far step 3's pushes
  // got that way if that's further, flicks taken out. Short only counts where it's sure: flicks start that way, step 3's pushes
  // that way were short too, or the last check of this controller found it short the same way. One quick roll that didn't go
  // all the way out on a stick that's fine doesn't make it touchier that way. A way it never went keeps 0: as far as it goes.
  if (!edge.empty() || !push.empty()) {
    const std::vector<float> roll = edgeOf(edge, c.x, c.y);
    for (int k = 0; k < kDirs; ++k) {
      float m = 9;
      bool all = !edge.empty();
      for (int s = -2; s < 2 && all; ++s) {
        const float r = roll[(k * (kSlices / kDirs) + s + kSlices) % kSlices];
        if (r <= 0) all = false;
        m = std::min(m, r);
      }
      if (!all) m = 0;
      const float a = k * 2 * kPi / kDirs, dx = std::cos(a), dy = std::sin(a);
      // Step 3's pushes this way: within half an eighth of it, most of the way out. The third furthest, so the odd spike
      // that wasn't big enough to count as a flick doesn't make it reach further than it does.
      float top[3] = {0, 0, 0};
      int out = 0;
      for (const Sample& s : push) {
        const float sx = s.x - c.x, sy = s.y - c.y, r = len(sx, sy);
        if (r < 0.6f || (sx * dx + sy * dy) < r * std::cos(kPi / 8)) continue;
        ++out;
        if (r > top[2]) {
          top[2] = r;
          if (top[2] > top[1]) std::swap(top[1], top[2]);
          if (top[1] > top[0]) std::swap(top[0], top[1]);
        }
      }
      const float best = top[2];
      const float got = std::max(m, out >= 3 ? best : 0.f);
      if (got <= 0) continue;
      const float b = c.x * dx + c.y * dy, should = -b + std::sqrt(std::max(0.f, b * b - (c.x * c.x + c.y * c.y - 1)));
      const float share = got / should;
      f.reachSeen[k] = share;
      if (share >= 0.9f) { p.reach[k] = got * 0.97f; continue; }
      bool sure = out >= 3 && best / should < 0.9f && m > 0;  // rolled short and pushed short, the same way
      for (const Flick& fk : f.flicks) {
        const float fx = fk.fromX - c.x, fy = fk.fromY - c.y;
        sure = sure || (f.flicky && fx * dx + fy * dy >= len(fx, fy) * std::cos(1.5f * kPi / kDirs));
      }
      // Short there last time too, by about as much: a worn stick stops at the same place each time, a thumb that didn't go
      // all the way out stops somewhere new.
      sure = sure || (d.prevReach[k] > 0 && d.prevReach[k] < 0.9f && std::fabs(d.prevReach[k] - share) < 0.06f);
      if (!sure) continue;
      p.reach[k] = got * 0.97f;
      if (share < f.worstReach) { f.worstReach = share; f.worstDir = k; }
    }
  }
  f.touchUsed = d.hasTouch && !d.touchMissed;
  if (f.touchUsed) {
    p.gate = std::min(0.35f, std::max(0.15f, std::max(2 * p.dead, p.dead + 0.1f)));
    p.learnPerSec = 0.5f;
    p.learnMax = std::min(0.25f, std::max(0.1f, 2 * p.dead));
  }
  // Drift a game would notice: resting, or landing, more than a few percent from the middle; flicking; or not reaching the
  // edge where that's sure.
  f.drifts = f.offset + rest.jitter > 0.05f || f.spread > 0.08f || f.flicky || (f.worstDir >= 0 && f.worstReach < 0.9f);
  p.on = f.drifts ? 1 : 0;
  if (!f.drifts) f.fl = Flicks{};

  char b[160];
  const int pct = int(std::lround(f.offset * 100));
  if (!f.drifts) {
    f.verdict = "No drift";
    f.lines = {"Comes back to the middle", "every time. Left as it is."};
    return f;
  }
  if (f.offset >= 0.03f) {
    f.verdict = std::string("Drifts ") + wayName(std::atan2(c.y, c.x));
    snprintf(b, sizeof b, "Rests %d%% off the middle.", pct);
    f.lines.push_back(b);
  } else {
    f.verdict = "Drifts";
  }
  if (f.flicky) {
    // "Jumps at 1 spot: left." on one line, or "Jumps at 2 spots:" then "left and down-right." Up to four ways named.
    std::vector<std::string> ways;
    for (int w = 0; w < 8; ++w)
      if (f.flickWays >> w & 1u) ways.push_back(wayShort(w));
    snprintf(b, sizeof b, "Jumps at %d spot%s:", int(ways.size()), ways.size() == 1 ? "" : "s");
    std::string list;
    if (ways.size() > 4) list = "all round the edge";
    else
      for (size_t i = 0; i < ways.size(); ++i) list += (i == 0 ? "" : i + 1 == ways.size() ? " and " : ", ") + ways[i];
    const std::string one = std::string(b) + " " + list + ".";
    if (one.size() <= 28) f.lines.push_back(one);
    else { f.lines.push_back(b); f.lines.push_back(list + "."); }
  }
  if (f.spread > 0.04f) {
    f.lines.push_back("Lands in a different spot each");
    snprintf(b, sizeof b, "time you let go, up to %d%% apart.", int(std::lround(f.spread * 100)));
    f.lines.push_back(b);
  }
  if (rest.jitter > 0.03f) {
    snprintf(b, sizeof b, "Jitters %d%% while left alone.", int(std::lround(rest.jitter * 100)));
    f.lines.push_back(b);
  }
  if (f.worstDir >= 0 && f.worstReach < 0.9f) {
    // Two lines: one would be squeezed small beside the stick.
    snprintf(b, sizeof b, "Only gets %d%% of the way", int(std::lround(f.worstReach * 100)));
    f.lines.push_back(b);
    f.lines.push_back(std::string(wayName(f.worstDir * 2 * kPi / kDirs)) + ".");
  } else if (!d.edge.empty()) {
    f.lines.push_back("Reaches the edge all round.");
  }
  return f;
}
// 0.6.3's way in: no step 3 samples, no earlier check.
inline Findings analyse(const Rest& rest, const std::vector<Sample>& edgeSamples, const std::vector<Landing>& landed, bool hasTouch, bool touchMissed) {
  CheckData d;
  d.rest = rest;
  d.edge = edgeSamples;
  d.landed = landed;
  d.hasTouch = hasTouch;
  d.touchMissed = touchMissed;
  return analyse(d);
}

}  // namespace drift

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

// One report. touch: 1 a thumb's on the stick, 0 it isn't, -1 this stick can't tell. now in seconds, any steady clock.
inline void fix(const Params& p, State& s, float x, float y, int touch, double now, float& ox, float& oy) {
  if (!p.on) { ox = x; oy = y; return; }
  const double dt = s.lastT < 0 ? 0 : std::min(std::max(now - s.lastT, 0.0), 0.1);
  s.lastT = now;
  const float mx = p.cx + s.lx, my = p.cy + s.ly;
  const float vx = x - mx, vy = y - my, r = std::sqrt(vx * vx + vy * vy);
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
    s.px = x;
    s.py = y;
    ox = oy = 0;
    return;
  }
  // A jump out from the middle in one report: wait 6 ms to see if it stays.
  const float jump = len(x - s.px, y - s.py);
  s.px = x;
  s.py = y;
  if (r <= p.dead || r < 1e-6f) {
    if (s.jumpT >= 0) ++s.spikes;
    s.jumpT = -1;
    s.atMiddle = true;
    ox = oy = 0;
    return;
  }
  if (s.atMiddle && s.jumpT < 0 && jump > 0.08f) s.jumpT = now;
  if (s.jumpT >= 0) {
    if (now - s.jumpT < 0.006) { ox = oy = 0; return; }
    s.jumpT = -1;
  }
  s.atMiddle = false;
  const float dx = vx / r, dy = vy / r;
  const float most = std::max(reachAt(p, mx, my, dx, dy), p.dead + 0.05f);
  const float t = std::min(1.f, (r - p.dead) / (most - p.dead));
  ox = dx * t;
  oy = dy * t;
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

// Step 2, round the edge: how far it got from (cx, cy) in each of 32 slices. 0: it hasn't been out that way yet.
inline std::vector<float> edgeOf(const std::vector<Sample>& v, float cx, float cy) {
  std::vector<float> reach(kSlices, 0.f);
  for (const Sample& s : v) {
    const float dx = s.x - cx, dy = s.y - cy, r = len(dx, dy);
    if (r < 0.6f) continue;  // only pushes most of the way out count as the edge
    float a = std::atan2(dy, dx);
    if (a < 0) a += 2 * kPi;
    const int i = std::min(kSlices - 1, int(a / (2 * kPi) * kSlices));
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
  float offset = 0;   // how far it rests from the true middle
  float spread = 0;   // how far apart its landings are
  float jitter = 0;
  float worstReach = 1;  // the least of how far a full push gets, as a share of how far it should
  int worstDir = -1;     // and which way that is, 0 east then anticlockwise in eighths
  bool touchUsed = false;
  std::string verdict;
  std::vector<std::string> lines;
};

inline const char* wayName(float angle) {
  static const char* const names[8] = {"to the right", "up and to the right", "up", "up and to the left", "to the left", "down and to the left", "down", "down and to the right"};
  float a = angle;
  if (a < 0) a += 2 * kPi;
  return names[int(std::fmod(a + kPi / 8, 2 * kPi) / (kPi / 4)) % 8];
}

// hasTouch: the controller can feel a thumb on the stick. edgeSamples and landed may be empty: a skipped step.
inline Findings analyse(const Rest& rest, const std::vector<Sample>& edgeSamples, const std::vector<Landing>& landed, bool hasTouch, bool touchMissed) {
  Findings f;
  std::vector<Dot> pts{{rest.x, rest.y}};
  for (const Landing& l : landed) pts.push_back({l.x, l.y});
  const Circle c = circleOf(pts);
  f.jitter = rest.jitter;
  f.spread = c.r * 2;
  f.offset = len(c.x, c.y);
  Params& p = f.p;
  p.cx = c.x;
  p.cy = c.y;
  // How far it gets each way, measured from the middle the fix will use.
  const std::vector<float> edge = edgeSamples.empty() ? std::vector<float>{} : edgeOf(edgeSamples, c.x, c.y);
  p.dead = std::min(0.4f, std::max(0.04f, c.r + rest.jitter + 0.02f));
  // Reach: for each of the eight ways, the shortest of the four slices around it, a little short so a full push makes it.
  // A way it never went keeps 0: as far as the stick goes.
  if (!edge.empty()) {
    for (int k = 0; k < kDirs; ++k) {
      float m = 9;
      bool all = true;
      for (int d = -2; d < 2; ++d) {
        const float r = edge[(k * (kSlices / kDirs) + d + kSlices) % kSlices];
        if (r <= 0) all = false;
        m = std::min(m, r);
      }
      if (!all) continue;
      p.reach[k] = m * 0.97f;
      const float a = k * 2 * kPi / kDirs, dx = std::cos(a), dy = std::sin(a);
      const float b = c.x * dx + c.y * dy, should = -b + std::sqrt(std::max(0.f, b * b - (c.x * c.x + c.y * c.y - 1)));
      if (m / should < f.worstReach) { f.worstReach = m / should; f.worstDir = k; }
    }
  }
  f.touchUsed = hasTouch && !touchMissed;
  if (f.touchUsed) {
    p.gate = std::min(0.35f, std::max(0.15f, std::max(2 * p.dead, p.dead + 0.1f)));
    p.learnPerSec = 0.5f;
    p.learnMax = std::min(0.25f, std::max(0.1f, 2 * p.dead));
  }
  // Drift a game would notice: resting, or landing, more than a few percent from the middle, or not reaching the edge.
  f.drifts = f.offset + rest.jitter > 0.05f || f.spread > 0.08f || f.worstReach < 0.9f;
  p.on = f.drifts ? 1 : 0;

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
    snprintf(b, sizeof b, "Only gets %d%% of the way %s.", int(std::lround(f.worstReach * 100)), wayName(f.worstDir * 2 * kPi / kDirs));
    f.lines.push_back(b);
  } else if (!edge.empty()) {
    f.lines.push_back("Reaches the edge all round.");
  }
  return f;
}

}  // namespace drift

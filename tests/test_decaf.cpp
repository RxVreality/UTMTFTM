// Decaffeinate, measured on made-up hands: how much shake each step takes out, and how late it makes a moving hand.
//
// The hand is built the way a real one behaves when you aim: a slow sway you mean, a trembling of 8 to 12 Hz you don't
// (physiological tremor, a few tenths of a degree with your arm out), and the tracking's own noise on top. The gyro
// reports the real turning, tremor included, with a little noise of its own. What a game gets is what SteamVR makes of
// a pose: moved forward by its velocity to when the frame is shown, so that's what's measured.
//
// test_decaf [folder]  also draws the traces as decaf_still.png and decaf_flick.png in that folder.
#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "../src/decaf.h"

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

constexpr double kPi = decaf::kPi, kDeg = kPi / 180.0;
constexpr double kPredict = 0.030;  // how far ahead SteamVR moves a controller pose for the frame: about right at 90 Hz

void quatFromYawPitch(double yaw, double pitch, double q[4]) {
  const double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
  // yaw about Y, then pitch about X
  q[0] = cy * cp; q[1] = cy * sp; q[2] = sy * cp; q[3] = -sy * sp;
}
void qmul(const double a[4], const double b[4], double r[4]) {
  r[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
  r[1] = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
  r[2] = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
  r[3] = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
}
void fromRotVec(const double v[3], double q[4]) {
  const double a = decaf::len3(v);
  if (a < 1e-15) { q[0] = 1; q[1] = v[0] / 2; q[2] = v[1] / 2; q[3] = v[2] / 2; decaf::normalize4(q); return; }
  const double s = std::sin(a / 2) / a;
  q[0] = std::cos(a / 2); q[1] = v[0] * s; q[2] = v[1] * s; q[3] = v[2] * s;
}
// Where the barrel points: the controller's forward, -Z.
void aimOf(const double q[4], double d[3]) {
  const double w = q[0], x = q[1], y = q[2], z = q[3];
  d[0] = -(2 * (x * z + w * y));
  d[1] = -(2 * (y * z - w * x));
  d[2] = -(1 - 2 * (x * x + y * y));
}
double aimAngle(const double a[4], const double b[4]) {
  double u[3], v[3];
  aimOf(a, u); aimOf(b, v);
  const double c = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
  const double cx = u[1] * v[2] - u[2] * v[1], cy = u[2] * v[0] - u[0] * v[2], cz = u[0] * v[1] - u[1] * v[0];
  return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), c);
}
// What SteamVR would show: the pose moved on by its own velocity.
void predicted(const decaf::Pose& p, double ahead, double q[4], double pos[3]) {
  double v[3] = {p.angVel[0] * ahead, p.angVel[1] * ahead, p.angVel[2] * ahead}, dq[4];
  fromRotVec(v, dq);
  qmul(dq, p.q, q);
  for (int i = 0; i < 3; ++i) pos[i] = p.pos[i] + p.vel[i] * ahead;
}

// What the hand means to do: aim direction and grip position over time, with no shake in it.
struct Intent {
  std::function<void(double t, double& yaw, double& pitch, double pos[3])> at;
};

struct Hand {
  double tremorDeg = 0.18;    // typical size of the tremor in each direction
  double tremorMm = 0.35;
  double noiseDeg = 0.02;     // tracking noise per pose
  double noiseMm = 0.15;
  double gyroNoise = 0.01;    // rad/s
  double velNoise = 0.004;    // m/s
  bool reportsVelocity = true;
};

struct Trace {
  std::vector<double> t, idealYaw, rawYaw, outYaw;  // degrees, after SteamVR's step forward
};

struct Result {
  double shakeRaw = 0, shakeOut = 0;     // RMS of aim error vs what you meant, while still, degrees, as the game sees it
  double posShakeRaw = 0, posShakeOut = 0;  // millimetres
  double lagMax = 0;                     // the furthest the aim falls behind, degrees, not counting SteamVR's step forward
  double lagMaxPredicted = 0;            // the same with it, which is what the game sees
  double posLagMax = 0;                  // millimetres
  double settleMs = 0;                   // after the hand stops, how long until the aim is within 0.1 degrees and stays
  double meterRaw = 0, meterOut = 0;     // what the driver's own shake meter says, degrees
  bool finite = true;
  Trace trace;
};

// Runs the hand through the filter at a given pose rate.
Result run(const Intent& intent, double seconds, double hz, const decaf::Params& prm, const Hand& hand, unsigned seed,
           double stillFrom, double stillTo, double moveFrom, double moveTo, double stopAt = -1, double gapAt = -1) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> n01(0, 1);
  std::uniform_real_distribution<double> ph(0, 2 * kPi);
  // Tremor: a few components between 8 and 12 Hz on each axis, in angle and in position.
  struct Comp { double f, phase[5]; };
  std::vector<Comp> comps;
  for (double f : {8.2, 9.1, 10.3, 11.6}) {
    Comp c{f, {}};
    for (double& p : c.phase) p = ph(rng);
    comps.push_back(c);
  }
  const double tremA = hand.tremorDeg * kDeg / std::sqrt(comps.size() / 2.0);
  const double tremP = hand.tremorMm * 1e-3 / std::sqrt(comps.size() / 2.0);
  auto truth = [&](double t, double q[4], double pos[3], double qIdeal[4], double posIdeal[3]) {
    double yaw, pitch;
    intent.at(t, yaw, pitch, posIdeal);
    quatFromYawPitch(yaw, pitch, qIdeal);
    double ty = 0, tp = 0, tr[3] = {0, 0, 0};
    for (const Comp& c : comps) {
      const double w = 2 * kPi * c.f * t;
      ty += std::sin(w + c.phase[0]);
      tp += std::sin(w + c.phase[1]);
      for (int i = 0; i < 3; ++i) tr[i] += std::sin(w + c.phase[2 + i]);
    }
    quatFromYawPitch(yaw + ty * tremA, pitch + tp * tremA, q);
    for (int i = 0; i < 3; ++i) pos[i] = posIdeal[i] + tr[i] * tremP;
  };

  decaf::Filter f;
  Result r;
  const double dt = 1.0 / hz, h = 1e-4;
  double sumRaw = 0, sumOut = 0, sumPR = 0, sumPO = 0;
  int nStill = 0;
  double lastBad = stopAt;
  for (double t = 0; t < seconds; t += dt) {
    if (gapAt >= 0 && t >= gapAt && t < gapAt + 0.5) continue;  // the controller drops out for half a second
    double q[4], pos[3], qI[4], pI[3], q2[4], pos2[3], qI2[4], pI2[3];
    truth(t, q, pos, qI, pI);
    truth(t + h, q2, pos2, qI2, pI2);
    decaf::Pose p{};
    // The gyro: the real turning, in world space, plus its own noise.
    double d[4];
    decaf::between(q2, q, d);
    if (d[0] < 0) for (double& x : d) x = -x;
    const double s = std::sqrt(d[1] * d[1] + d[2] * d[2] + d[3] * d[3]), ang = 2 * std::atan2(s, d[0]);
    for (int i = 0; i < 3; ++i) {
      p.angVel[i] = hand.reportsVelocity ? (s > 0 ? d[i + 1] / s * ang / h : 0) + n01(rng) * hand.gyroNoise : 0;
      p.vel[i] = hand.reportsVelocity ? (pos2[i] - pos[i]) / h + n01(rng) * hand.velNoise : 0;
    }
    // Tracking noise on the pose itself.
    double nv[3] = {n01(rng) * hand.noiseDeg * kDeg, n01(rng) * hand.noiseDeg * kDeg, n01(rng) * hand.noiseDeg * kDeg}, nq[4];
    fromRotVec(nv, nq);
    qmul(nq, q, p.q);
    for (int i = 0; i < 3; ++i) p.pos[i] = pos[i] + n01(rng) * hand.noiseMm * 1e-3;

    double rawPredQ[4], rawPredP[3];
    predicted(p, kPredict, rawPredQ, rawPredP);
    f.step(p, t, prm, true);
    for (double x : p.q) r.finite = r.finite && std::isfinite(x);
    for (double x : p.pos) r.finite = r.finite && std::isfinite(x);
    double outPredQ[4], outPredP[3];
    predicted(p, kPredict, outPredQ, outPredP);

    // What you meant, at the moment the frame is shown.
    double qAim[4], pAim[3], dummyQ[4], dummyP[3];
    truth(t + kPredict, dummyQ, dummyP, qAim, pAim);

    if (t >= stillFrom && t < stillTo) {
      const double er = aimAngle(rawPredQ, qAim), eo = aimAngle(outPredQ, qAim);
      sumRaw += er * er; sumOut += eo * eo;
      double dr = 0, dO = 0;
      for (int i = 0; i < 3; ++i) { dr += (rawPredP[i] - pAim[i]) * (rawPredP[i] - pAim[i]); dO += (outPredP[i] - pAim[i]) * (outPredP[i] - pAim[i]); }
      sumPR += dr; sumPO += dO;
      ++nStill;
    }
    if (t >= moveFrom && t < moveTo) {
      r.lagMax = std::max(r.lagMax, aimAngle(p.q, qI) / kDeg);
      r.lagMaxPredicted = std::max(r.lagMaxPredicted, aimAngle(outPredQ, qAim) / kDeg);
      double dp = 0;
      for (int i = 0; i < 3; ++i) dp += (p.pos[i] - pI[i]) * (p.pos[i] - pI[i]);
      r.posLagMax = std::max(r.posLagMax, std::sqrt(dp) * 1e3);
    }
    if (stopAt >= 0 && t >= stopAt && aimAngle(p.q, qI) / kDeg > 0.1 + 2.5 * hand.tremorDeg * (prm.on() ? 0.35 : 1.0)) lastBad = t;

    double y, pi, pp[3];
    intent.at(t + kPredict, y, pi, pp);
    auto yawOf = [](const double qq[4]) { double a[3]; aimOf(qq, a); return std::atan2(-a[0], -a[2]) / kDeg; };
    r.trace.t.push_back(t);
    r.trace.idealYaw.push_back(yawOf(qAim));
    r.trace.rawYaw.push_back(yawOf(rawPredQ));
    r.trace.outYaw.push_back(yawOf(outPredQ));
  }
  if (nStill) {
    r.shakeRaw = std::sqrt(sumRaw / nStill) / kDeg;
    r.shakeOut = std::sqrt(sumOut / nStill) / kDeg;
    r.posShakeRaw = std::sqrt(sumPR / nStill) * 1e3;
    r.posShakeOut = std::sqrt(sumPO / nStill) * 1e3;
  }
  if (stopAt >= 0) r.settleMs = (lastBad - stopAt) * 1e3;
  r.meterRaw = f.rotShakeRaw() / kDeg;
  r.meterOut = f.rotShakeOut() / kDeg;
  return r;
}

// Aiming down sights: a gentle sway you mean, nothing else.
Intent aiming() {
  return {[](double t, double& yaw, double& pitch, double pos[3]) {
    yaw = 0.25 * kDeg * std::sin(2 * kPi * 0.21 * t);
    pitch = -2 * kDeg + 0.15 * kDeg * std::sin(2 * kPi * 0.33 * t + 1);
    pos[0] = 0.18 + 0.002 * std::sin(2 * kPi * 0.25 * t);
    pos[1] = 1.35;
    pos[2] = -0.45;
  }};
}
// Tracking a walking target: 10 degrees a second from 1 s to 3 s.
Intent tracking() {
  return {[](double t, double& yaw, double& pitch, double pos[3]) {
    const double tt = std::clamp(t - 1.0, 0.0, 2.0);
    yaw = 10 * kDeg * tt;
    pitch = -2 * kDeg;
    pos[0] = 0.18; pos[1] = 1.35; pos[2] = -0.45;
  }};
}
// A flick to a new target: 40 degrees in a fifth of a second (peak about 400 degrees a second), then still.
// The hand swings 40 cm at the same time, the way an arm does.
Intent flick() {
  return {[](double t, double& yaw, double& pitch, double pos[3]) {
    const double u = std::clamp((t - 1.0) / 0.2, 0.0, 1.0), e = u * u * (3 - 2 * u);
    yaw = 40 * kDeg * e;
    pitch = -2 * kDeg;
    pos[0] = 0.18 - 0.4 * e; pos[1] = 1.35; pos[2] = -0.45;
  }};
}

void drawTraces(const std::string& path, const char* title, const std::vector<std::pair<std::string, const Trace*>>& traces,
                double t0, double t1, bool showRaw) {
  const int W = 1200, H = 520, L = 70, R = 30, T = 70, B = 60;
  cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  cairo_t* c = cairo_create(s);
  cairo_set_source_rgb(c, 0.09, 0.09, 0.09);
  cairo_paint(c);
  // Range from what's drawn.
  double lo = 1e9, hi = -1e9;
  const Trace& first = *traces.front().second;
  for (size_t i = 0; i < first.t.size(); ++i) {
    if (first.t[i] < t0 || first.t[i] > t1) continue;
    lo = std::min(lo, first.idealYaw[i]); hi = std::max(hi, first.idealYaw[i]);
    if (showRaw) { lo = std::min(lo, first.rawYaw[i]); hi = std::max(hi, first.rawYaw[i]); }
    for (auto& tr : traces) { lo = std::min(lo, tr.second->outYaw[i]); hi = std::max(hi, tr.second->outYaw[i]); }
  }
  const double pad = (hi - lo) * 0.08 + 1e-6;
  lo -= pad; hi += pad;
  auto X = [&](double t) { return L + (t - t0) / (t1 - t0) * (W - L - R); };
  auto Y = [&](double v) { return T + (hi - v) / (hi - lo) * (H - T - B); };
  cairo_select_font_face(c, "Segoe UI", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(c, 22);
  cairo_set_source_rgb(c, 0.95, 0.95, 0.93);
  cairo_move_to(c, L, 38);
  cairo_show_text(c, title);
  cairo_set_font_size(c, 14);
  cairo_select_font_face(c, "Segoe UI", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  // Grid: a line per tenth of a degree if it's tight, otherwise per degree or five.
  const double span = hi - lo, step = span < 2 ? 0.1 : span < 12 ? 1 : 5;
  cairo_set_line_width(c, 1);
  for (double v = std::ceil(lo / step) * step; v <= hi; v += step) {
    cairo_set_source_rgb(c, 0.2, 0.2, 0.2);
    cairo_move_to(c, L, Y(v)); cairo_line_to(c, W - R, Y(v)); cairo_stroke(c);
    char b[32];
    std::snprintf(b, sizeof b, step < 1 ? "%.1f°" : "%.0f°", v);
    cairo_set_source_rgb(c, 0.6, 0.6, 0.58);
    cairo_move_to(c, 8, Y(v) + 5); cairo_show_text(c, b);
  }
  for (double t = std::ceil(t0 * 10) / 10; t <= t1 + 1e-9; t += 0.1) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f s", t);
    cairo_set_source_rgb(c, 0.6, 0.6, 0.58);
    cairo_move_to(c, X(t) - 14, H - 30); cairo_show_text(c, b);
  }
  auto line = [&](const std::vector<double>& v, double r, double g, double b, double w) {
    cairo_set_source_rgb(c, r, g, b);
    cairo_set_line_width(c, w);
    bool started = false;
    for (size_t i = 0; i < first.t.size(); ++i) {
      if (first.t[i] < t0 || first.t[i] > t1) continue;
      if (!started) { cairo_move_to(c, X(first.t[i]), Y(v[i])); started = true; }
      else cairo_line_to(c, X(first.t[i]), Y(v[i]));
    }
    cairo_stroke(c);
  };
  double lx = L;
  auto legend = [&](const std::string& name, double r, double g, double b) {
    cairo_set_source_rgb(c, r, g, b);
    cairo_rectangle(c, lx, H - 20, 18, 6); cairo_fill(c);
    cairo_set_source_rgb(c, 0.85, 0.85, 0.83);
    cairo_move_to(c, lx + 24, H - 12); cairo_show_text(c, name.c_str());
    cairo_text_extents_t e; cairo_text_extents(c, name.c_str(), &e);
    lx += 24 + e.x_advance + 30;
  };
  if (showRaw) { line(first.rawYaw, 0.55, 0.55, 0.55, 1.2); legend("Off: what the game gets now", 0.55, 0.55, 0.55); }
  const double shades[][3] = {{1.0, 0.85, 0.0}, {1.0, 0.6, 0.1}, {0.95, 0.3, 0.25}};
  for (size_t k = 0; k < traces.size(); ++k) {
    line(traces[k].second->outYaw, shades[k % 3][0], shades[k % 3][1], shades[k % 3][2], 2.2);
    legend(traces[k].first, shades[k % 3][0], shades[k % 3][1], shades[k % 3][2]);
  }
  const double dash[2] = {6, 5};
  cairo_set_dash(c, dash, 2, 0);
  line(first.idealYaw, 0.95, 0.95, 0.93, 1.5);
  cairo_set_dash(c, nullptr, 0, 0);
  legend("Where you meant to aim", 0.95, 0.95, 0.93);
  cairo_destroy(c);
  cairo_surface_write_to_png(s, path.c_str());
  cairo_surface_destroy(s);
}

// Lag is measured on a hand with no shake at all, so what's left is only the delay the filter adds.
Hand cleanHand() {
  Hand h;
  h.tremorDeg = h.tremorMm = h.noiseDeg = h.noiseMm = h.gyroNoise = h.velNoise = 0;
  return h;
}
double meanBehind(const Trace& tr, double t0, double t1) {
  double s = 0;
  int n = 0;
  for (size_t i = 0; i < tr.t.size(); ++i)
    if (tr.t[i] >= t0 && tr.t[i] < t1) { s += tr.idealYaw[i] - tr.outYaw[i]; ++n; }
  return n ? s / n : 0;
}
double crosses(const Trace& tr, double level) {
  for (size_t i = 0; i < tr.t.size(); ++i)
    if (tr.outYaw[i] >= level) return tr.t[i];
  return 1e9;
}
// When it gets within this much of the end of the flick and stays there.
double arrives(const Trace& tr, double end, double within) {
  double last = 0;
  for (size_t i = 0; i < tr.t.size(); ++i)
    if (std::fabs(tr.outYaw[i] - end) > within) last = tr.t[i];
  return last;
}
double overshoot(const Trace& tr, double end) {
  double m = 0;
  for (size_t i = 0; i < tr.t.size(); ++i) m = std::max(m, tr.outYaw[i] - end);
  return m;
}

// How wobbly the aim looks: what's left once a fifth of a second's average is taken away, so slow sway doesn't count.
double wobble(const Trace& tr, bool raw, double t0, double t1) {
  const std::vector<double>& v = raw ? tr.rawYaw : tr.outYaw;
  double dt = tr.t.size() > 1 ? tr.t[1] - tr.t[0] : 0.002, s = 0;
  const int half = int(0.1 / dt + 0.5);
  int n = 0;
  for (size_t i = half; i + half < v.size(); ++i) {
    if (tr.t[i] < t0 || tr.t[i] >= t1) continue;
    double m = 0;
    for (int k = -half; k <= half; ++k) m += v[i + k];
    m /= 2 * half + 1;
    s += (v[i] - m) * (v[i] - m);
    ++n;
  }
  return n ? std::sqrt(s / n) : 0;
}

struct Score {
  double shakeRaw, shakeOut, posRaw, posOut, trackDeg, half, ninety, arrive, over, swingMm, wobRaw, wobOut;
};
Score score(const decaf::Params& p, const Hand& hand, double hz = 500, unsigned seed = 7) {
  Score s{};
  Result st = run(aiming(), 6, hz, p, hand, seed, 1, 6, -1, -1);
  s.shakeRaw = st.shakeRaw; s.shakeOut = st.shakeOut; s.posRaw = st.posShakeRaw; s.posOut = st.posShakeOut;
  s.wobRaw = wobble(st.trace, true, 1, 6);
  s.wobOut = wobble(st.trace, false, 1, 6);
  Hand clean = cleanHand();
  clean.reportsVelocity = hand.reportsVelocity;
  Result tr = run(tracking(), 4, hz, p, clean, seed, -1, -1, -1, -1);
  s.trackDeg = meanBehind(tr.trace, 1.6, 3.0);
  Result fl = run(flick(), 2.5, hz, p, clean, seed, -1, -1, 0.95, 1.6);
  Result off = run(flick(), 2.5, hz, decaf::Params{}, clean, seed, -1, -1, 0.95, 1.6);
  s.half = (crosses(fl.trace, 20) - crosses(off.trace, 20)) * 1e3;
  s.ninety = (crosses(fl.trace, 36) - crosses(off.trace, 36)) * 1e3;
  s.arrive = (arrives(fl.trace, 40, 0.2) - arrives(off.trace, 40, 0.2)) * 1e3;
  s.over = overshoot(fl.trace, 40);
  s.swingMm = fl.posLagMax;
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  std::string out;
  bool sweep = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--sweep") sweep = true;
    else out = argv[i];
  }
  const char* names[] = {"Off", "Tuned", "Strong", "Maximum"};
  Hand hand;

  if (sweep) {
    std::printf("rotMin beta  posMin beta   aim error  x less  wobble x less  grip x less  track ms  flick 90%% arrive ms  swing mm\n");
    for (float mn : {0.2f, 0.3f, 0.6f, 1.2f})
      for (float be : {8.f, 12.f, 22.f, 40.f})
        for (float pm : {0.5f, 1.5f})
          for (float pb : {30.f, 60.f}) {
            decaf::Params p{mn, be, pm, pb, 1.f};
            Score s = score(p, hand);
            std::printf("%5.1f %5.0f %6.1f %5.0f   %8.3f %6.1f   %6.1f      %6.1f   %7.0f   %6.0f %8.0f %8.1f\n", mn, be, pm, pb, s.shakeOut,
                        s.shakeRaw / s.shakeOut, s.wobRaw / s.wobOut, s.posRaw / s.posOut, s.trackDeg / 10 * 1e3, s.ninety, s.arrive, s.swingMm);
          }
    return 0;
  }

  std::printf("Decaffeinate on a made-up hand, 500 poses a second, as the game sees it\n");
  std::printf("%-8s %17s %17s %16s %10s %21s %10s %9s\n", "step", "aim error (deg)", "wobble (deg)", "grip shake (mm)", "track", "flick late by", "overshoot", "swing");
  std::printf("%-8s %17s %17s %16s %10s %21s %10s %9s\n", "", "", "", "", "", "half / 90% / there", "", "");
  Score sc[decaf::kLevels];
  Result still[decaf::kLevels];
  for (int lv = 0; lv < decaf::kLevels; ++lv) {
    const decaf::Params p = decaf::preset(lv);
    sc[lv] = score(p, hand);
    still[lv] = run(aiming(), 6, 500, p, hand, 7, 1, 6, -1, -1);
    std::printf("%-8s %7.3f > %6.3f %7.3f > %6.3f %7.2f > %6.2f %7.0f ms %5.0f /%4.0f /%4.0f ms %8.2f° %6.1f mm\n", names[lv], sc[lv].shakeRaw, sc[lv].shakeOut,
                sc[lv].wobRaw, sc[lv].wobOut, sc[lv].posRaw, sc[lv].posOut, sc[lv].trackDeg / 10 * 1e3, sc[lv].half, sc[lv].ninety, sc[lv].arrive,
                sc[lv].over, sc[lv].swingMm);
  }
  std::printf("  aim error: how far off where you meant, wobble and sway together. wobble: the fast shake alone.\n");
  std::printf("  track: how far behind a target moving 10 degrees a second, as time. flick: 40 degrees in 0.2 s, how much later\n"
              "  than with it off it gets halfway, 90%% of the way, and settles within 0.2 degrees. swing: furthest behind in a 40 cm swing.\n");

  std::printf("\nWhat it should do\n");
  {
    // Off: every pose goes on exactly as it came.
    decaf::Filter f;
    bool same = true;
    std::mt19937 rng(3);
    std::normal_distribution<double> n(0, 1);
    for (int i = 0; i < 2000; ++i) {
      decaf::Pose p{};
      for (int k = 0; k < 3; ++k) { p.pos[k] = n(rng); p.vel[k] = n(rng); p.angVel[k] = n(rng); p.acc[k] = n(rng); p.angAcc[k] = n(rng); }
      for (int k = 0; k < 4; ++k) p.q[k] = n(rng);
      decaf::normalize4(p.q);
      decaf::Pose before = p;
      f.step(p, i * 0.002, decaf::Params{}, i % 2 == 0);
      same = same && std::memcmp(&before, &p, sizeof p) == 0;
    }
    check(same, "Off changes nothing, measuring or not");
  }
  check(sc[1].shakeOut < sc[0].shakeRaw / 4, "Tuned: aim at least 4 times closer to where you meant");
  check(sc[2].shakeOut < sc[0].shakeRaw / 6, "Strong: at least 6 times");
  check(sc[3].shakeOut < sc[0].shakeRaw / 5, "Maximum: at least 5 times (it trades some of that for being heavier)");
  check(sc[1].wobOut < sc[0].wobRaw / 4 && sc[3].wobOut < sc[0].wobRaw / 15, "the fast wobble: at least 4 times less (Tuned), 15 times less (Maximum)");
  check(sc[1].wobOut > sc[2].wobOut && sc[2].wobOut > sc[3].wobOut, "each step wobbles less than the one before");
  check(sc[1].posOut < sc[0].posRaw / 3 && sc[3].posOut < sc[0].posRaw / 2.5, "the grip's position steadies too");
  check(sc[1].trackDeg / 10 * 1e3 < 35 && sc[2].trackDeg / 10 * 1e3 < 60 && sc[3].trackDeg / 10 * 1e3 < 100,
        "following a slow target it's behind by under 35 ms (Tuned), 60 ms (Strong), 100 ms (Maximum)");
  check(sc[1].ninety < 8 && sc[2].ninety < 12 && sc[3].ninety < 15, "a fast flick gets 90% of the way under 8 ms later (Tuned), 12 ms (Strong), 15 ms (Maximum)");
  check(sc[1].arrive < 10 && sc[2].arrive < 18 && sc[3].arrive < 25, "and settles on target under 10 ms later (Tuned), 18 ms (Strong), 25 ms (Maximum)");
  check(sc[3].over <= sc[0].over + 0.05, "it doesn't overshoot any more than SteamVR does by itself");
  check(sc[1].swingMm < 20 && sc[3].swingMm < 25, "a 40 cm swing (3 m/s at its fastest) stays within 2 cm (Tuned) and 2.5 cm (Maximum)");

  std::printf("\nThe driver's own shake meter (what the page can show), Strong, held still\n");
  std::printf("  meter %.3f° > %.3f°   measured %.3f° > %.3f° (the meter doesn't include SteamVR's step forward)\n", still[2].meterRaw, still[2].meterOut,
              sc[2].shakeRaw, sc[2].shakeOut);
  check(still[2].meterRaw > 0.05 && still[2].meterOut < still[2].meterRaw / 2.5, "the meter sees the shake and sees it go");
  bool finite = true;
  for (int lv = 0; lv < decaf::kLevels; ++lv) finite = finite && still[lv].finite;
  check(finite, "never a broken number");

  std::printf("\nThe same at other pose rates (it goes by time, not by pose)\n");
  for (double hz : {90.0, 250.0, 1000.0}) {
    Score s = score(decaf::preset(2), hand, hz, 11);
    std::printf("  %4.0f Hz: shake %.3f > %.3f, flick 90%% %.0f ms later, settles %.0f ms later\n", hz, s.shakeRaw, s.shakeOut, s.ninety, s.arrive);
    char what[128];
    std::snprintf(what, sizeof what, "Strong at %.0f Hz: shake within 30%% of 500 Hz and settling within 30 ms", hz);
    check(std::fabs(s.shakeOut / sc[2].shakeOut - 1) < 0.3 && std::fabs(s.arrive - sc[2].arrive) < 30, what);
  }

  std::printf("\nA controller that doesn't report its velocity\n");
  {
    Hand quiet = hand;
    quiet.reportsVelocity = false;
    Score s = score(decaf::preset(2), quiet);
    std::printf("  Strong: shake %.3f > %.3f, flick 90%% %.0f ms later, settles %.0f ms later\n", s.shakeRaw, s.shakeOut, s.ninety, s.arrive);
    check(s.shakeOut < s.shakeRaw / 2.5 && s.ninety < 30 && s.arrive < 150, "still steadies and still keeps up, working the speed out from the poses");
  }

  std::printf("\nDropping out and coming back\n");
  {
    Result g = run(flick(), 3, 500, decaf::preset(3), hand, 7, -1, -1, -1, -1, -1, 1.05);
    // The hand reappears 0.5 s later at the end of the flick: the filter starts again there rather than gliding over.
    double worst = 0;
    for (size_t i = 0; i < g.trace.t.size(); ++i)
      if (g.trace.t[i] > 1.55 && g.trace.t[i] < 1.6) worst = std::max(worst, std::fabs(g.trace.outYaw[i] - g.trace.idealYaw[i]));
    std::printf("  aim error just after coming back: %.2f°\n", worst);
    check(worst < 1.0 && g.finite, "a hand that comes back after half a second starts where it is, with no slide");
  }
  {
    decaf::Filter f;
    decaf::Pose p{};
    p.q[0] = 1;
    f.step(p, 0.0, decaf::preset(2), false);
    decaf::Pose bad = p;
    bad.pos[0] = std::nan("");
    bad.q[1] = std::nan("");
    f.step(bad, 0.002, decaf::preset(2), true);
    decaf::Pose next = p;
    next.pos[0] = 0.01;
    f.step(next, 0.004, decaf::preset(2), true);
    bool ok = true;
    for (double x : next.pos) ok = ok && std::isfinite(x);
    for (double x : next.q) ok = ok && std::isfinite(x);
    check(ok && std::isfinite(f.rotShakeRaw()) && std::isfinite(f.posShakeOut()), "a broken pose from a driver goes through untouched and doesn't poison the next ones");
  }

  if (!out.empty()) {
    const Trace* t[3] = {&still[1].trace, &still[2].trace, &still[3].trace};
    drawTraces(out + "/decaf_still.png", "Aiming down sights, held still: left and right, over 2 seconds",
               {{"Tuned", t[0]}, {"Strong", t[1]}, {"Maximum", t[2]}}, 2.0, 4.0, true);
    Result fl[decaf::kLevels];
    for (int lv = 0; lv < decaf::kLevels; ++lv) fl[lv] = run(flick(), 2.5, 500, decaf::preset(lv), hand, 7, -1, -1, -1, -1);
    drawTraces(out + "/decaf_flick.png", "A fast flick to a new target (400° a second at the peak)",
               {{"Tuned", &fl[1].trace}, {"Strong", &fl[2].trace}, {"Maximum", &fl[3].trace}}, 0.9, 1.6, true);
    std::printf("\nPictures: %s/decaf_still.png, decaf_flick.png\n", out.c_str());
  }
  std::printf("\n%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
  return g_fail ? 1 : 0;
}

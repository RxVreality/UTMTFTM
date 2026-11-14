// Decaffeinate: takes the shake out of a held-still hand without making a moving one feel late.
//
// It's a One Euro filter (Casiez, Roussel and Vogel, CHI 2012), the same kind Monado and Meta use on hand poses: a
// smoothing filter whose strength follows how fast the hand is going. Held still, as when aiming down sights, it
// smooths hard and the micro-shake goes. Moving, it lets go, so a flick or a swing arrives on time.
//
// Position and rotation are filtered apart, each with its own speed. The speed comes from the velocity the
// controller's driver reports (its gyro, which is clean), or from the poses themselves if a driver never reports one.
// That velocity is smoothed as a direction before it's measured, so a hand trembling back and forth reads as still
// rather than as fast: that's what keeps it smoothing while you aim.
//
// The velocities going on to SteamVR are smoothed by the same amount as the pose. SteamVR moves every pose forward by
// its velocity to where the hand will be when the frame is shown, and a raw velocity would put the shake back in.
//
// No allocation, no locks, no openvr: the driver calls step() on every pose, and tests/test_decaf.cpp runs the same
// code on made-up hands to measure what it does.
#pragma once
#include <cmath>

namespace decaf {

// How hard to smooth. Cutoffs are in Hz: lower is smoother. Beta is how fast the cutoff rises with speed, per
// radian a second for rotation and per metre a second for position. rotMinHz 0 means off.
// speedHz is how fast the velocity used for speed may change: low enough that a tremor's back and forth cancels out.
struct Params {
  float rotMinHz = 0, rotBeta = 0, posMinHz = 0, posBeta = 0, speedHz = 2;
  bool on() const { return rotMinHz > 0; }
};

// The four steps, named after H3VR2's: Disabled, Tuned Default, Strong, Maximum. tests/test_decaf.cpp prints what each
// costs in lag and saves in shake. Changing these only needs a new overlay, not a SteamVR restart.
constexpr int kLevels = 4;
inline Params preset(int level) {
  switch (level) {
    case 1: return {1.2f, 30.f, 1.5f, 60.f, 1.f};
    case 2: return {0.6f, 15.f, 1.0f, 50.f, 1.f};
    case 3: return {0.25f, 10.f, 0.6f, 40.f, 1.f};
    default: return {};
  }
}

// One pose, in the driver's own space. Quaternions are w, x, y, z.
struct Pose {
  double pos[3], q[4];
  double vel[3], acc[3], angVel[3], angAcc[3];
};

// Longer than this between poses and it starts again from the new one, rather than gliding across the gap.
constexpr double kMaxGap = 0.25;
// Shake is only measured while turning slower than this (rad/s, about 20 degrees a second): a moving hand isn't shaking.
constexpr double kStillRad = 0.35;
constexpr double kStillM = 0.15;
// The steady line shake is measured from, in Hz, and how long the measurement averages over, in seconds.
constexpr double kRefHz = 2.0, kShakeAvg = 1.0;
constexpr double kPi = 3.14159265358979323846;

inline double alphaFor(double hz, double dt) {
  const double tau = 1.0 / (2.0 * kPi * hz);
  return dt / (dt + tau);
}
inline double len3(const double v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
inline void lerp3(double a[3], const double b[3], double t) {
  for (int i = 0; i < 3; ++i) a[i] += (b[i] - a[i]) * t;
}
inline void normalize4(double q[4]) {
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (n > 0) for (int i = 0; i < 4; ++i) q[i] /= n;
}
// Part of the way from a to b, the short way round. The steps here are small, so a normalised lerp is as good as a slerp.
inline void nlerp(double a[4], const double b[4], double t) {
  const double s = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -1.0 : 1.0;
  for (int i = 0; i < 4; ++i) a[i] += (s * b[i] - a[i]) * t;
  normalize4(a);
}
// a * conj(b): the turn that takes b to a, in world space.
inline void between(const double a[4], const double b[4], double d[4]) {
  d[0] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  d[1] = -a[0] * b[1] + a[1] * b[0] - a[2] * b[3] + a[3] * b[2];
  d[2] = -a[0] * b[2] + a[1] * b[3] + a[2] * b[0] - a[3] * b[1];
  d[3] = -a[0] * b[3] - a[1] * b[2] + a[2] * b[1] + a[3] * b[0];
}
// The angle between two orientations, in radians. atan2 keeps it accurate down at the tiny angles shake is made of.
inline double angleBetween(const double a[4], const double b[4]) {
  double d[4];
  between(a, b, d);
  return 2.0 * std::atan2(std::sqrt(d[1] * d[1] + d[2] * d[2] + d[3] * d[3]), std::fabs(d[0]));
}

class Filter {
 public:
  // Forget everything: the next pose goes through as it is and starts it again.
  void reset() { primed_ = false; }
  bool primed() const { return primed_; }

  // Smooths p in place. t is in seconds on any steady clock. With p.on() false it changes nothing, but still keeps the
  // shake measurement going when measure is set, so the overlay can show how much there is before you switch it on.
  void step(Pose& p, double t, const Params& prm, bool measure) {
    // A broken pose from a driver goes on as it came, and the next good one starts again: NaN would otherwise stay in here for good.
    bool finite = std::isfinite(t);
    for (int i = 0; i < 3; ++i) finite = finite && std::isfinite(p.pos[i]) && std::isfinite(p.vel[i]) && std::isfinite(p.angVel[i]) && std::isfinite(p.acc[i]) && std::isfinite(p.angAcc[i]);
    for (int i = 0; i < 4; ++i) finite = finite && std::isfinite(p.q[i]);
    if (!finite) { primed_ = false; return; }
    const double dt = t - t_;
    if (!primed_ || !(dt > 0) || dt > kMaxGap) { prime(p, t); return; }
    t_ = t;

    // How fast it's going: the driver's own velocity once it's shown it reports one, otherwise worked out from the poses.
    double v[3], w[3];
    if (len3(p.vel) > 0) gotVel_ = true;
    if (len3(p.angVel) > 0) gotAngVel_ = true;
    if (gotVel_) {
      for (int i = 0; i < 3; ++i) v[i] = p.vel[i];
    } else {
      for (int i = 0; i < 3; ++i) v[i] = (p.pos[i] - rawPos_[i]) / dt;
    }
    if (gotAngVel_) {
      for (int i = 0; i < 3; ++i) w[i] = p.angVel[i];
    } else {
      double d[4];
      between(p.q, rawQ_, d);
      if (d[0] < 0) for (double& x : d) x = -x;
      const double s = std::sqrt(d[1] * d[1] + d[2] * d[2] + d[3] * d[3]);
      const double ang = 2.0 * std::atan2(s, d[0]);
      for (int i = 0; i < 3; ++i) w[i] = s > 0 ? d[i + 1] / s * ang / dt : 0;
    }
    for (int i = 0; i < 3; ++i) { rawPos_[i] = p.pos[i]; }
    for (int i = 0; i < 4; ++i) { rawQ_[i] = p.q[i]; }
    const double aS = alphaFor(prm.speedHz > 0 ? prm.speedHz : 2.0, dt);
    lerp3(speedV_, v, aS);
    lerp3(speedW_, w, aS);
    const double linSpeed = len3(speedV_), angSpeed = len3(speedW_);

    if (prm.on()) {
      const double aP = alphaFor(prm.posMinHz + prm.posBeta * linSpeed, dt);
      const double aR = alphaFor(prm.rotMinHz + prm.rotBeta * angSpeed, dt);
      lerp3(out_.pos, p.pos, aP);
      lerp3(out_.vel, p.vel, aP);
      lerp3(out_.acc, p.acc, aP);
      nlerp(out_.q, p.q, aR);
      lerp3(out_.angVel, p.angVel, aR);
      lerp3(out_.angAcc, p.angAcc, aR);
    } else {
      out_ = p;
    }

    if (measure || prm.on()) {
      // The steady line: a plain smoothing of the raw hand, the same whatever the setting. Shake is how far a pose sits
      // off it, before and after, averaged over about a second while the hand is still.
      const double aRef = alphaFor(kRefHz, dt);
      lerp3(refPos_, p.pos, aRef);
      nlerp(refQ_, p.q, aRef);
      const double aAvg = dt / (dt + kShakeAvg);
      if (angSpeed < kStillRad) {
        const double r = angleBetween(p.q, refQ_), o = angleBetween(out_.q, refQ_);
        rotRaw2_ += (r * r - rotRaw2_) * aAvg;
        rotOut2_ += (o * o - rotOut2_) * aAvg;
      }
      if (linSpeed < kStillM) {
        double dr[3], dO[3];
        for (int i = 0; i < 3; ++i) { dr[i] = p.pos[i] - refPos_[i]; dO[i] = out_.pos[i] - refPos_[i]; }
        const double r = len3(dr), o = len3(dO);
        posRaw2_ += (r * r - posRaw2_) * aAvg;
        posOut2_ += (o * o - posOut2_) * aAvg;
      }
    }
    if (prm.on()) p = out_;
  }

  // Shake while still, as a typical size: radians and metres, before and after.
  double rotShakeRaw() const { return std::sqrt(rotRaw2_); }
  double rotShakeOut() const { return std::sqrt(rotOut2_); }
  double posShakeRaw() const { return std::sqrt(posRaw2_); }
  double posShakeOut() const { return std::sqrt(posOut2_); }

 private:
  void prime(const Pose& p, double t) {
    primed_ = true;
    t_ = t;
    out_ = p;
    for (int i = 0; i < 3; ++i) { rawPos_[i] = refPos_[i] = p.pos[i]; speedV_[i] = speedW_[i] = 0; }
    for (int i = 0; i < 4; ++i) { rawQ_[i] = refQ_[i] = p.q[i]; }
  }

  bool primed_ = false, gotVel_ = false, gotAngVel_ = false;
  double t_ = 0;
  Pose out_{};
  double rawPos_[3]{}, rawQ_[4]{1, 0, 0, 0};
  double speedV_[3]{}, speedW_[3]{};
  double refPos_[3]{}, refQ_[4]{1, 0, 0, 0};
  double rotRaw2_ = 0, rotOut2_ = 0, posRaw2_ = 0, posOut2_ = 0;
};

}  // namespace decaf

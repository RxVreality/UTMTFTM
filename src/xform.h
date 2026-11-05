// Rigid-transform helpers on vr::HmdMatrix34_t (row-major 3x4: rotation | translation).
#pragma once
#include <openvr.h>
#include <cmath>
#include <algorithm>

namespace fh {

using M = vr::HmdMatrix34_t;
constexpr float kDeg = 3.14159265358979f / 180.f;

inline M ident() { M m{}; m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.f; return m; }

inline M mul(const M& a, const M& b) {
  M r{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 4; ++j) {
      float s = (j == 3) ? a.m[i][3] : 0.f;
      for (int k = 0; k < 3; ++k) s += a.m[i][k] * b.m[k][j];
      r.m[i][j] = s;
    }
  }
  return r;
}

// Inverse of a rigid transform (R^T, -R^T t).
inline M inv(const M& a) {
  M r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
  for (int i = 0; i < 3; ++i)
    r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
  return r;
}

inline float dist(const M& a, const M& b) {
  float dx = a.m[0][3] - b.m[0][3], dy = a.m[1][3] - b.m[1][3], dz = a.m[2][3] - b.m[2][3];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// SteamVR "Raw Device Offsets": position [x,y,z] metres (X+ right, Y+ up, Z- forward)
// and rotation [pitch, yaw, roll] degrees. Convention ASSUMED here: R = Ry(yaw) * Rx(pitch) * Rz(roll),
// offset applied in the device's local frame (pose' = raw * offset). Verify in headset; if rotation
// comes out wrong, this pair of functions is the only place to change.
struct Offset {
  float pos[3] = {0, 0, 0};
  float rot[3] = {0, 0, 0};  // pitch, yaw, roll (deg)
  bool zero() const {
    for (int i = 0; i < 3; ++i)
      if (std::fabs(pos[i]) > 1e-5f || std::fabs(rot[i]) > 1e-3f) return false;
    return true;
  }
};

inline M fromOffset(const Offset& o) {
  float p = o.rot[0] * kDeg, y = o.rot[1] * kDeg, r = o.rot[2] * kDeg;
  float cp = std::cos(p), sp = std::sin(p), cy = std::cos(y), sy = std::sin(y), cr = std::cos(r), sr = std::sin(r);
  // Ry*Rx
  float A[3][3] = {{cy, sy * sp, sy * cp}, {0, cp, -sp}, {-sy, cy * sp, cy * cp}};
  float Rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
  M m{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m.m[i][j] = A[i][0] * Rz[0][j] + A[i][1] * Rz[1][j] + A[i][2] * Rz[2][j];
  for (int i = 0; i < 3; ++i) m.m[i][3] = o.pos[i];
  return m;
}

inline Offset toOffset(const M& m) {
  Offset o;
  float sp = std::clamp(-m.m[1][2], -1.f, 1.f);
  o.rot[0] = std::asin(sp) / kDeg;
  o.rot[1] = std::atan2(m.m[0][2], m.m[2][2]) / kDeg;
  o.rot[2] = std::atan2(m.m[1][0], m.m[1][1]) / kDeg;
  for (int i = 0; i < 3; ++i) o.pos[i] = m.m[i][3];
  return o;
}

inline bool finite(const M& m) {
  for (const auto& row : m.m)
    for (float v : row)
      if (!std::isfinite(v)) return false;
  return true;
}

inline bool finite(const Offset& o) {
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(o.pos[i]) || !std::isfinite(o.rot[i])) return false;
  return true;
}

inline float posLen(const Offset& o) {
  return std::sqrt(o.pos[0] * o.pos[0] + o.pos[1] * o.pos[1] + o.pos[2] * o.pos[2]);
}

inline bool beyond(const Offset& o, float maxPos, float maxRot) {
  if (posLen(o) > maxPos + 1e-4f) return true;
  for (float a : o.rot)
    if (std::fabs(a) > maxRot + 1e-3f) return true;
  return false;
}

// Pull an offset back inside the limits (let go too far and it's pulled back).
inline Offset clampOffset(Offset o, float maxPos, float maxRot) {
  float l = posLen(o);
  if (l > maxPos && l > 0) for (float& p : o.pos) p *= maxPos / l;
  for (float& a : o.rot) a = std::clamp(a, -maxRot, maxRot);
  return o;
}

inline Offset lerp(const Offset& a, const Offset& b, float t) {
  Offset r;
  for (int i = 0; i < 3; ++i) {
    r.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
    r.rot[i] = a.rot[i] + (b.rot[i] - a.rot[i]) * t;
  }
  return r;
}

// Mirror a grip-frame transform across the grip's X axis (left <-> right hand): S*m*S, S = diag(-1,1,1).
// Grip frames of the two hands are mirror images across X (palm +X on the left, -X on the right).
inline M mirrorX(const M& a) {
  M m = a;
  m.m[0][1] = -a.m[0][1]; m.m[0][2] = -a.m[0][2];
  m.m[1][0] = -a.m[1][0]; m.m[2][0] = -a.m[2][0];
  m.m[0][3] = -a.m[0][3];
  return m;
}
inline Offset mirrorOffset(const Offset& o) { return toOffset(mirrorX(fromOffset(o))); }

inline M rotX(float deg) {
  M m = ident();
  float c = std::cos(deg * kDeg), s = std::sin(deg * kDeg);
  m.m[1][1] = c; m.m[1][2] = -s; m.m[2][1] = s; m.m[2][2] = c;
  return m;
}

inline M translate(float x, float y, float z) { M m = ident(); m.m[0][3] = x; m.m[1][3] = y; m.m[2][3] = z; return m; }

// Rotation part as a quaternion (x, y, z, w).
inline void toQuat(const M& m, float q[4]) {
  float tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
  if (tr > 0) {
    float s = std::sqrt(tr + 1.f) * 2;
    q[3] = 0.25f * s; q[0] = (m.m[2][1] - m.m[1][2]) / s; q[1] = (m.m[0][2] - m.m[2][0]) / s; q[2] = (m.m[1][0] - m.m[0][1]) / s;
  } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
    float s = std::sqrt(1.f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
    q[3] = (m.m[2][1] - m.m[1][2]) / s; q[0] = 0.25f * s; q[1] = (m.m[0][1] + m.m[1][0]) / s; q[2] = (m.m[0][2] + m.m[2][0]) / s;
  } else if (m.m[1][1] > m.m[2][2]) {
    float s = std::sqrt(1.f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
    q[3] = (m.m[0][2] - m.m[2][0]) / s; q[0] = (m.m[0][1] + m.m[1][0]) / s; q[1] = 0.25f * s; q[2] = (m.m[1][2] + m.m[2][1]) / s;
  } else {
    float s = std::sqrt(1.f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
    q[3] = (m.m[1][0] - m.m[0][1]) / s; q[0] = (m.m[0][2] + m.m[2][0]) / s; q[1] = (m.m[1][2] + m.m[2][1]) / s; q[2] = 0.25f * s;
  }
  float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (n > 0) for (int i = 0; i < 4; ++i) q[i] /= n;
}
inline M fromPosQuat(const float p[3], const float q[4]) {
  float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  float x = 0, y = 0, z = 0, w = 1;
  if (n > 1e-6f) { x = q[0] / n; y = q[1] / n; z = q[2] / n; w = q[3] / n; }
  M m{};
  m.m[0][0] = 1 - 2 * (y * y + z * z); m.m[0][1] = 2 * (x * y - z * w); m.m[0][2] = 2 * (x * z + y * w); m.m[0][3] = p[0];
  m.m[1][0] = 2 * (x * y + z * w); m.m[1][1] = 1 - 2 * (x * x + z * z); m.m[1][2] = 2 * (y * z - x * w); m.m[1][3] = p[1];
  m.m[2][0] = 2 * (x * z - y * w); m.m[2][1] = 2 * (y * z + x * w); m.m[2][2] = 1 - 2 * (x * x + y * y); m.m[2][3] = p[2];
  return m;
}

}  // namespace fh

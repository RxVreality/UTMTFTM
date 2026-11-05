// Stereo "ghost hand": a procedural hand mesh rasterised per eye onto a side-by-side overlay quad,
// so it reads as a real 3D object (correct depth + perspective) without a GPU pipeline.
// Look: see-through shell, bright fresnel rim (the "outline"), palm side a little brighter,
// plus a thin tether from the real controller to the ghost.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xform.h"

namespace fh {

struct V3 { float x, y, z; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 norm(V3 a) { float l = len(a); return l > 1e-9f ? a * (1.f / l) : V3{0, 0, 1}; }
inline V3 xfP(const M& m, V3 p) {
  return {m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3],
          m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3],
          m.m[2][0] * p.x + m.m[2][1] * p.y + m.m[2][2] * p.z + m.m[2][3]};
}
inline V3 xfN(const M& m, V3 n) {
  return {m.m[0][0] * n.x + m.m[0][1] * n.y + m.m[0][2] * n.z, m.m[1][0] * n.x + m.m[1][1] * n.y + m.m[1][2] * n.z,
          m.m[2][0] * n.x + m.m[2][1] * n.y + m.m[2][2] * n.z};
}
inline V3 pos(const M& m) { return {m.m[0][3], m.m[1][3], m.m[2][3]}; }

struct Mesh {
  std::vector<V3> p, n;
  std::vector<float> palm;  // 0..1: how much this vertex is on the palm side
  std::vector<uint32_t> idx;
};

// Tube along a polyline with per-point radius, rounded caps. Frames are parallel-transported.
inline void addTube(Mesh& m, const std::vector<V3>& pts, const std::vector<float>& rad, V3 palmDir, int seg = 12) {
  const int capRings = 4;
  std::vector<V3> T(pts.size());
  for (size_t i = 0; i < pts.size(); ++i) {
    V3 a = pts[i > 0 ? i - 1 : 0], b = pts[i + 1 < pts.size() ? i + 1 : i];
    T[i] = norm(b - a);
  }
  V3 ref = std::fabs(T[0].y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0};
  V3 N = norm(cross(ref, T[0]));
  struct Ring { V3 c, t, u, v; float r; };
  std::vector<Ring> rings;
  // start cap
  for (int k = capRings; k >= 1; --k) {
    float a = (float)k / capRings * 1.5707963f;
    V3 B = cross(T[0], N);
    rings.push_back({pts[0] - T[0] * (rad[0] * std::sin(a)), T[0], N, B, rad[0] * std::cos(a)});
  }
  for (size_t i = 0; i < pts.size(); ++i) {
    if (i > 0) {  // parallel transport N
      N = norm(N - T[i] * dot(N, T[i]));
    }
    V3 B = cross(T[i], N);
    rings.push_back({pts[i], T[i], N, B, rad[i]});
  }
  size_t L = pts.size() - 1;
  for (int k = 1; k <= capRings; ++k) {
    float a = (float)k / capRings * 1.5707963f;
    V3 B = cross(T[L], N);
    rings.push_back({pts[L] + T[L] * (rad[L] * std::sin(a)), T[L], N, B, std::max(rad[L] * std::cos(a), 0.0004f)});
  }
  uint32_t base = (uint32_t)m.p.size();
  for (size_t ri = 0; ri < rings.size(); ++ri) {
    const Ring& R = rings[ri];
    for (int s = 0; s < seg; ++s) {
      float ang = (float)s / seg * 6.2831853f;
      V3 dir = R.u * std::cos(ang) + R.v * std::sin(ang);
      V3 nn = dir;
      // caps: tilt normals along the tube axis
      if (ri < (size_t)capRings) nn = norm(dir * R.r + R.t * -(rad[0] - R.r + 1e-4f));
      else if (ri >= rings.size() - capRings) nn = norm(dir * R.r + R.t * (rad[L] - R.r + 1e-4f));
      m.p.push_back(R.c + dir * R.r);
      m.n.push_back(nn);
      m.palm.push_back(std::max(0.f, dot(nn, palmDir)));
    }
  }
  for (size_t ri = 0; ri + 1 < rings.size(); ++ri)
    for (int s = 0; s < seg; ++s) {
      uint32_t a = base + ri * seg + s, b = base + ri * seg + (s + 1) % seg;
      uint32_t c = a + seg, d = b + seg;
      m.idx.insert(m.idx.end(), {a, c, b, b, c, d});
    }
}

inline void addEllipsoid(Mesh& m, V3 c, V3 r, V3 palmDir, int lat = 12, int lon = 18) {
  uint32_t base = (uint32_t)m.p.size();
  for (int i = 0; i <= lat; ++i) {
    float th = (float)i / lat * 3.1415927f;
    for (int j = 0; j < lon; ++j) {
      float ph = (float)j / lon * 6.2831853f;
      V3 u{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
      m.p.push_back(c + V3{u.x * r.x, u.y * r.y, u.z * r.z});
      V3 nn = norm({u.x / r.x, u.y / r.y, u.z / r.z});
      m.n.push_back(nn);
      m.palm.push_back(std::max(0.f, dot(nn, palmDir)));
    }
  }
  for (int i = 0; i < lat; ++i)
    for (int j = 0; j < lon; ++j) {
      uint32_t a = base + i * lon + j, b = base + i * lon + (j + 1) % lon, c2 = a + lon, d = b + lon;
      m.idx.insert(m.idx.end(), {a, c2, b, b, c2, d});
    }
}

// A hand closed around a controller handle, in the OpenXR grip frame:
// handle along Z (-Z = forward out of the fist), +Y up. Left palm faces +X (right palm -X).
inline Mesh buildHand(bool left) {
  Mesh m;
  float sx = left ? 1.f : -1.f;  // mirror for the right hand
  V3 palmDir{sx, 0, 0};
  auto P = [&](float x, float y, float z) { return V3{x * sx, y, z}; };
  // back of the hand: a flattened slab beside the handle, knuckle row at the top
  addEllipsoid(m, P(-0.037f, 0.002f, 0.000f), {0.0150f, 0.040f, 0.046f}, palmDir);
  // heel of the palm, wrapping a little toward the handle
  addEllipsoid(m, P(-0.024f, -0.026f, 0.012f), {0.014f, 0.020f, 0.030f}, palmDir);
  // four fingers wrap the handle; index (front, -Z) is more open and rests on the trigger
  const float zs[4] = {-0.0285f, -0.0095f, 0.0095f, 0.0270f};
  const float rr[4] = {0.0102f, 0.0106f, 0.0100f, 0.0088f};
  const float curlEnd[4] = {165.f, 200.f, 205.f, 210.f};   // degrees around the handle
  const float reach[4] = {0.032f, 0.031f, 0.030f, 0.027f};  // finger radius around the handle axis
  const float tilt[4] = {-0.006f, -0.001f, 0.002f, 0.005f}; // fingers fan slightly along Z
  for (int f = 0; f < 4; ++f) {
    std::vector<V3> pts;
    std::vector<float> rad;
    const int N = 11;
    for (int k = 0; k < N; ++k) {
      float t = (float)k / (N - 1);
      float ang = (40.f + (curlEnd[f] - 40.f) * t) * 3.1415927f / 180.f;
      float rr2 = reach[f] * (1.0f - 0.10f * t);
      pts.push_back(P(-std::cos(ang) * rr2, std::sin(ang) * rr2 + 0.004f, zs[f] + tilt[f] * t));
      rad.push_back(rr[f] * (1.0f - 0.18f * t));
    }
    pts.insert(pts.begin(), P(-0.040f, 0.032f, zs[f] * 0.95f));  // knuckle on the back of the hand
    rad.insert(rad.begin(), rr[f] * 1.08f);
    addTube(m, pts, rad, palmDir);
  }
  // thumb: from the heel of the palm, up and over the top-front of the handle
  {
    std::vector<V3> pts = {P(-0.028f, -0.012f, -0.026f), P(-0.020f, 0.014f, -0.046f), P(-0.004f, 0.030f, -0.054f),
                           P(0.012f, 0.034f, -0.050f), P(0.022f, 0.030f, -0.042f)};
    addTube(m, pts, {0.0135f, 0.0118f, 0.0104f, 0.0094f, 0.0085f}, palmDir);
  }
  // wrist, toward the forearm (back and down)
  addTube(m, {P(-0.032f, -0.034f, 0.026f), P(-0.036f, -0.062f, 0.060f)}, {0.022f, 0.023f}, palmDir);
  return m;
}

inline Mesh buildSphere(float r, int lat = 20, int lon = 32) {
  Mesh m;
  addEllipsoid(m, {0, 0, 0}, {r, r, r}, {0, 0, 0}, lat, lon);
  return m;
}

// A generic controller in the grip frame: handle along Z through the palm, tracking head above the thumb.
inline Mesh buildController() {
  Mesh m;
  V3 none{0, 0, 0};
  addTube(m, {{0, -0.004f, 0.050f}, {0, 0.000f, 0.000f}, {0, 0.006f, -0.045f}}, {0.016f, 0.018f, 0.017f}, none, 14);
  addEllipsoid(m, {0, 0.018f, -0.070f}, {0.030f, 0.016f, 0.034f}, none, 10, 16);
  return m;
}

// Hand frame from joints: origin at the wrist, u = wrist -> middle knuckle, a = pinky -> index knuckle.
struct Frame3 { V3 o, a, u, n; };
inline Frame3 handFrame(const V3 j[31]) {
  Frame3 f;
  f.o = j[1];
  f.u = norm(j[12] - j[1]);
  V3 a = j[7] - j[22];
  f.a = norm(a - f.u * dot(a, f.u));
  f.n = cross(f.a, f.u);
  return f;
}
// Rigidly move joints from one hand frame onto another.
inline void mapJoints(const V3 src[31], const Frame3& from, const Frame3& to, V3 dst[31]) {
  for (int i = 0; i < 31; ++i) {
    V3 d = src[i] - from.o;
    float x = dot(d, from.a), y = dot(d, from.u), z = dot(d, from.n);
    dst[i] = to.o + to.a * x + to.u * y + to.n * z;
  }
}
// Open hand placed in the OpenXR GRIP frame exactly as the spec defines it (what games attach hands to):
//   -Z runs little finger -> thumb along the handle, palm normal +X (left) / -X (right),
//   so an open hand's fingers point -Y (forward), and the palm wraps the handle at the origin.
inline void restJoints(bool left, V3 out[31]);
inline void gripHandJoints(bool left, V3 out[31]) {
  V3 rest[31];
  restJoints(left, rest);
  Frame3 to;
  to.a = {0, 0, -1};   // pinky -> index
  to.u = {0, -1, 0};   // wrist -> fingers
  to.n = cross(to.a, to.u);
  to.o = {0, 0, 0};
  mapJoints(rest, handFrame(rest), to, out);
  // centre the palm (wrist..middle knuckle midpoint) beside the handle, on the palm side
  V3 mid = (out[1] + out[12]) * 0.5f;
  V3 target{left ? -0.030f : 0.030f, 0.0f, 0.0f};
  for (int i = 0; i < 31; ++i) out[i] = out[i] + (target - mid);
}
// A plain hand worked out from ordinary proportions, for when SteamVR's glove can't be read. It's written as the left
// hand: +Z runs wrist to fingers, +Y towards the thumb, and the palm faces -X. The right hand is its mirror image.
// Joints go root, wrist, thumb (4), then index, middle, ring and little (5 each), then five copies near the tips.
inline void restJoints(bool left, V3 out[31]) {
  struct Finger { V3 base, knuckle; float len[3], fan; };
  static const Finger kFingers[4] = {  // metres; fan spreads the finger sideways, in radians
      {{0.000f, 0.020f, 0.015f}, {0.010f, 0.036f, 0.086f}, {0.042f, 0.025f, 0.020f}, 0.03f},
      {{0.002f, 0.007f, 0.016f}, {0.014f, 0.012f, 0.088f}, {0.046f, 0.028f, 0.022f}, 0.00f},
      {{0.001f, -0.006f, 0.016f}, {0.010f, -0.012f, 0.084f}, {0.042f, 0.027f, 0.022f}, -0.04f},
      {{-0.002f, -0.018f, 0.015f}, {0.002f, -0.034f, 0.076f}, {0.032f, 0.019f, 0.018f}, -0.09f}};
  const float kCurl[3] = {0.35f, 0.75f, 1.10f};  // a relaxed hand: each bone bends further towards the palm, in radians
  out[0] = out[1] = {0, 0, 0};
  const V3 thumbDir = norm(V3{-0.35f, 0.55f, 0.76f});
  out[2] = {-0.018f, 0.030f, 0.025f};
  out[3] = out[2] + thumbDir * 0.042f;
  out[4] = out[3] + thumbDir * 0.032f;
  out[5] = out[4] + thumbDir * 0.028f;
  for (int f = 0; f < 4; ++f) {
    const Finger& d = kFingers[f];
    V3* j = out + 6 + f * 5;
    j[0] = d.base;
    j[1] = d.knuckle;
    for (int s = 0; s < 3; ++s) {
      const float c = std::cos(kCurl[s]);
      j[s + 2] = j[s + 1] + V3{-std::sin(kCurl[s]), std::sin(d.fan) * c, std::cos(d.fan) * c} * d.len[s];
    }
  }
  const int nearTip[5] = {4, 9, 14, 19, 24};
  for (int i = 0; i < 5; ++i) out[26 + i] = out[nearTip[i]];
  if (!left) for (int i = 0; i < 31; ++i) out[i].x = -out[i].x;
}

// Per-eye software raster onto the overlay plane. Output RGBA (straight alpha), width 2W (L|R), height H.
struct GhostRenderer {
  int W, H;                    // per eye; buffers hold two eyes side by side (L | R)
  std::vector<uint8_t> rgba;   // premultiplied white, tinted later
  std::vector<float> zbuf;
  std::vector<V3> wp, wn;
  bool additive = false;       // glass shells: add light, don't hide what's inside
  bool opaque = false;         // solid objects: full coverage, brightness in colour only
  explicit GhostRenderer(int w = 336, int h = 336) : W(w), H(h) {
    rgba.resize(size_t(2 * W) * H * 4);
    zbuf.resize(size_t(2 * W) * H);
  }

  struct Plane { V3 c, X, Y, N; float S; };

  // Project world point from eye E onto plane; returns false if behind.
  static bool proj(const Plane& pl, V3 E, V3 V, float& u, float& v, float& w) {
    V3 d = V - E;
    float dn = dot(d, pl.N);
    if (dn > -1e-5f) return false;  // N points toward viewer
    float t = dot(pl.c - E, pl.N) / dn;
    V3 P = E + d * t - pl.c;
    u = dot(P, pl.X) / pl.S + 0.5f;
    v = 0.5f - dot(P, pl.Y) / pl.S;
    w = -dn;  // distance along the view normal (for depth)
    return true;
  }

  void tri(int eye, float x0, float y0, float w0, float i0, float x1, float y1, float w1, float i1, float x2, float y2,
           float w2, float i2) {
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::fabs(area) < 1e-6f) return;
    int minx = std::max(0, (int)std::floor(std::min({x0, x1, x2})));
    int maxx = std::min(W - 1, (int)std::ceil(std::max({x0, x1, x2})));
    int miny = std::max(0, (int)std::floor(std::min({y0, y1, y2})));
    int maxy = std::min(H - 1, (int)std::ceil(std::max({y0, y1, y2})));
    float iw0 = 1 / w0, iw1 = 1 / w1, iw2 = 1 / w2;
    for (int y = miny; y <= maxy; ++y)
      for (int x = minx; x <= maxx; ++x) {
        float px = x + 0.5f, py = y + 0.5f;
        float b0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
        float b1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
        float b2 = 1 - b0 - b1;
        if (b0 < 0 || b1 < 0 || b2 < 0) continue;
        float iw = b0 * iw0 + b1 * iw1 + b2 * iw2;
        size_t k = size_t(y) * 2 * W + eye * W + x;
        float I = (b0 * i0 * iw0 + b1 * i1 * iw1 + b2 * i2 * iw2) / iw;
        if (additive) {
          uint8_t* o = &rgba[k * 4];
          uint8_t a = (uint8_t)std::min(255.f, o[3] + I * 255.f);
          o[0] = o[1] = o[2] = a; o[3] = a;
          continue;
        }
        if (iw <= zbuf[k]) continue;
        zbuf[k] = iw;
        // SteamVR composites overlays as PREMULTIPLIED alpha: colour must be scaled by alpha.
        uint8_t a = (uint8_t)std::min(255.f, I * 255.f);
        uint8_t* o = &rgba[k * 4];
        o[0] = o[1] = o[2] = a; o[3] = opaque ? 255 : a;
      }
  }

  void line(int eye, float x0, float y0, float x1, float y1, float alpha, float width) {
    float dx = x1 - x0, dy = y1 - y0, L = std::sqrt(dx * dx + dy * dy);
    if (L < 1e-3f) return;
    int steps = (int)(L * 2) + 1;
    for (int s = 0; s <= steps; ++s) {
      float t = (float)s / steps;
      if (((int)(t * L / 6)) % 2) continue;  // dashed
      float cx = x0 + dx * t, cy = y0 + dy * t;
      for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox) {
          if (ox * ox + oy * oy > width * width) continue;
          int x = (int)cx + ox, y = (int)cy + oy;
          if (x < 0 || y < 0 || x >= W || y >= H) continue;
          size_t k = size_t(y) * 2 * W + eye * W + x;
          if (zbuf[k] > 0) continue;  // never draw the tether over the hand
          uint8_t* o = &rgba[k * 4];
          uint8_t a = std::max<uint8_t>(o[3], (uint8_t)(alpha * 255));
          o[0] = o[1] = o[2] = a; o[3] = a;
        }
    }
  }

  // handPose: world pose of the ghost (grip frame). eyes: world eye positions. tetherFrom: real controller.
  void clear() {
    std::fill(rgba.begin(), rgba.end(), 0);
    std::fill(zbuf.begin(), zbuf.end(), 0.f);
  }

  // Glass shell: faint fill, bright fresnel rim (the outline), palm side a little brighter. gain dims a mesh.
  // light (world direction towards the light): solid shading instead, for real objects like the controller.
  void drawMesh(const Mesh& m, const M& pose, const Plane& pl, const V3* eyes, int nEyes, float gain = 1.f,
                bool glass = false, const V3* light = nullptr) {
    additive = glass;
    opaque = light != nullptr;
    wp.resize(m.p.size()); wn.resize(m.n.size());
    for (size_t i = 0; i < m.p.size(); ++i) { wp[i] = xfP(pose, m.p[i]); wn[i] = norm(xfN(pose, m.n[i])); }
    std::vector<float> sx(m.p.size()), sy(m.p.size()), sw(m.p.size()), si(m.p.size());
    std::vector<uint8_t> ok(m.p.size());
    for (int eye = 0; eye < nEyes; ++eye) {
      V3 E = eyes[eye];
      for (size_t i = 0; i < m.p.size(); ++i) {
        float u = 0, v = 0, w = 1;
        ok[i] = proj(pl, E, wp[i], u, v, w);
        sx[i] = u * W; sy[i] = v * H; sw[i] = w;
        float ndv = std::fabs(dot(wn[i], norm(E - wp[i])));
        if (light)
          si[i] = gain * std::min(1.f, (0.20f + 0.85f * std::max(0.f, dot(wn[i], *light))) * (0.62f + 0.38f * ndv));
        else if (glass)  // the limit bubble: clear in the middle so your hand shows through, with a crisp rim
          si[i] = gain * std::min(1.f, 0.025f + 1.15f * std::pow(1.f - ndv, 3.4f));
        else
          si[i] = gain * std::min(1.f, 0.14f + 0.12f * ndv + 1.05f * std::pow(1.f - ndv, 2.4f) + 0.10f * (m.palm.empty() ? 0.f : m.palm[i]));
      }
      for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
        uint32_t a = m.idx[t], b = m.idx[t + 1], c = m.idx[t + 2];
        if (!ok[a] || !ok[b] || !ok[c]) continue;
        tri(eye, sx[a], sy[a], sw[a], si[a], sx[b], sy[b], sw[b], si[b], sx[c], sy[c], sw[c], si[c]);
      }
    }
  }

  // Dashed line between two world points, never drawn over a mesh.
  void drawTether(const Plane& pl, const V3* eyes, int nEyes, V3 from, V3 to, float alpha = 0.7f) {
    for (int eye = 0; eye < nEyes; ++eye) {
      float u0, v0, w0, u1, v1, w1;
      if (proj(pl, eyes[eye], from, u0, v0, w0) && proj(pl, eyes[eye], to, u1, v1, w1))
        line(eye, u0 * W, v0 * H, u1 * W, v1 * H, alpha, 1.2f);
    }
  }
};

}  // namespace fh

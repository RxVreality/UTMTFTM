// A hand of our own, for a SteamVR with no glove to read, like the one on the Steam Frame. The Frame does have SteamVR's hand
// skeleton and its closing animation, so the bones end up exactly where the glove's do on Windows. This wraps a smooth skin
// around them: a distance field the shape of a gloved hand, turned into triangles with surface nets.
// Each finger melts into the palm but stays apart from its neighbours, so touching fingers keep the crease between them.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ghost3d.h"

namespace fh {

struct HandField {
  struct Cone { V3 a, b; float ra, rb; };
  static constexpr int kFingers = 5, kFingerBones = 3, kPalmBones = 7;
  Cone palm[kPalmBones];
  Cone finger[kFingers][kFingerBones];
  Frame3 frame;

  static float coneDist(const Cone& c, V3 p) {
    const V3 ab = c.b - c.a;
    const float dd = dot(ab, ab);
    const float t = dd > 1e-10f ? std::clamp(dot(p - c.a, ab) / dd, 0.f, 1.f) : 0.f;
    return len(p - (c.a + ab * t)) - (c.ra + (c.rb - c.ra) * t);
  }
  static float smoothMin(float a, float b, float k) {
    const float h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.f, 1.f);
    return b + (a - b) * h - k * h * (1 - h);
  }
  // A rounded oval tube around the wrist, y0 to y1 along the arm.
  float tube(V3 p, float rx, float rz, float y0, float y1, float round) const {
    const V3 d = p - frame.o;
    const float x = dot(d, frame.a) + 0.003f, y = dot(d, frame.u), z = dot(d, frame.n) - 0.006f;
    const float ring = std::sqrt(x * x / (rx * rx) + z * z / (rz * rz));
    const float radial = (ring - 1) * std::min(rx, rz) + round;
    const float along = std::max(y0 + round - y, y - y1 + round);
    const float qx = std::max(radial, 0.f), qy = std::max(along, 0.f);
    return std::min(std::max(radial, along), 0.f) + std::sqrt(qx * qx + qy * qy) - round;
  }
  float operator()(V3 p) const {
    float hand = tube(p, 0.037f, 0.027f, -0.024f, 0.022f, 0.005f);  // the glove's cuff, reaching back past the wrist
    for (const Cone& c : palm) hand = smoothMin(hand, coneDist(c, p), 0.014f);
    hand = smoothMin(hand, tube(p, 0.0398f, 0.0298f, -0.012f, 0.004f, 0.002f), 0.0015f);  // the strap across it
    float out = 1e9f;
    for (const auto& bones : finger) {
      float f = 1e9f;
      for (const Cone& c : bones) f = std::min(f, coneDist(c, p));
      out = std::min(out, smoothMin(hand, f, 0.007f));
    }
    return out;
  }
  V3 gradient(V3 p) const {
    const float e = 0.0005f;
    const V3 g{(*this)({p.x + e, p.y, p.z}) - (*this)({p.x - e, p.y, p.z}),
               (*this)({p.x, p.y + e, p.z}) - (*this)({p.x, p.y - e, p.z}),
               (*this)({p.x, p.y, p.z + e}) - (*this)({p.x, p.y, p.z - e})};
    const float l = len(g);
    return l > 1e-9f ? g * (1 / l) : V3{0, 1, 0};
  }
};

// Radii in metres, measured off the gloved hand: fingers from the knuckle to the tip, then the bones of the palm.
// Bones: 1 wrist, 2-5 thumb (0,1,2,end), 6-10 index (meta,0,1,2,end), 11-15 middle, 16-20 ring, 21-25 pinky.
inline HandField handField(const V3 j[31]) {
  HandField f;
  f.frame = handFrame(j);
  const float r[4][4] = {{0.0108f, 0.0098f, 0.0086f, 0.0074f},
                         {0.0112f, 0.0104f, 0.0092f, 0.0078f},
                         {0.0108f, 0.0098f, 0.0086f, 0.0074f},
                         {0.0098f, 0.0080f, 0.0070f, 0.0060f}};
  for (int n = 0; n < 4; ++n) {
    const V3* b = j + 6 + n * 5;
    f.palm[n] = {b[0], b[1], 0.0165f, 0.0128f};
    for (int s = 0; s < 3; ++s) f.finger[n][s] = {b[s + 1], b[s + 2], r[n][s], r[n][s + 1]};
  }
  f.palm[4] = {j[2], j[3], 0.0185f, 0.0135f};  // the ball of the thumb
  f.palm[5] = {j[21], (j[21] + j[22]) * 0.5f, 0.0175f, 0.0150f};  // the pad under the little finger
  f.palm[6] = {(j[3] + j[4]) * 0.5f, (j[6] + j[7]) * 0.6f, 0.0100f, 0.0120f};  // the web between the thumb and the palm
  f.finger[4][0] = {j[3], j[4], 0.0138f, 0.0120f};
  f.finger[4][1] = {j[4], j[5], 0.0120f, 0.0100f};
  f.finger[4][2] = f.finger[4][1];
  return f;
}

// Triangles for where a field crosses zero. Vertices sit on the surface and their normals come from the field,
// so it shades smoothly at any grid size. Triangles wind counter-clockwise seen from outside.
template <class Field>
Mesh surfaceNets(const Field& f, V3 lo, V3 hi, float h) {
  const int nx = int((hi.x - lo.x) / h) + 2, ny = int((hi.y - lo.y) / h) + 2, nz = int((hi.z - lo.z) / h) + 2;
  const auto at = [&](int x, int y, int z) { return size_t(x) + size_t(y) * nx + size_t(z) * nx * ny; };
  const auto place = [&](float x, float y, float z) { return V3{lo.x + x * h, lo.y + y * h, lo.z + z * h}; };
  std::vector<float> v(size_t(nx) * ny * nz);
  for (int z = 0; z < nz; ++z)
    for (int y = 0; y < ny; ++y)
      for (int x = 0; x < nx; ++x) v[at(x, y, z)] = f(place(float(x), float(y), float(z)));

  Mesh m;
  std::vector<int32_t> vertexAt(v.size(), -1);
  static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (int z = 0; z + 1 < nz; ++z)
    for (int y = 0; y + 1 < ny; ++y)
      for (int x = 0; x + 1 < nx; ++x) {
        float c[8];
        int inside = 0;
        for (int i = 0; i < 8; ++i) {
          c[i] = v[at(x + (i & 1), y + ((i >> 1) & 1), z + ((i >> 2) & 1))];
          if (c[i] < 0) ++inside;
        }
        if (inside == 0 || inside == 8) continue;
        V3 sum{0, 0, 0};
        int crossings = 0;
        for (const auto& e : kEdges) {
          const float a = c[e[0]], b = c[e[1]];
          if ((a < 0) == (b < 0)) continue;
          const float t = a / (a - b);
          const auto corner = [](int i) { return V3{float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1)}; };
          sum = sum + corner(e[0]) + (corner(e[1]) - corner(e[0])) * t;
          ++crossings;
        }
        const V3 s = sum * (1.f / crossings);
        V3 p = place(x + s.x, y + s.y, z + s.z);
        for (int step = 0; step < 2; ++step) p = p - f.gradient(p) * f(p);  // settle onto the surface
        vertexAt[at(x, y, z)] = int32_t(m.p.size());
        m.p.push_back(p);
        m.n.push_back(f.gradient(p));
      }

  const auto quad = [&](size_t a, size_t b, size_t c, size_t d, bool flip) {
    const int32_t q[4] = {vertexAt[a], vertexAt[b], vertexAt[c], vertexAt[d]};
    if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[3] < 0) return;
    if (flip) m.idx.insert(m.idx.end(), {uint32_t(q[0]), uint32_t(q[2]), uint32_t(q[1]), uint32_t(q[0]), uint32_t(q[3]), uint32_t(q[2])});
    else m.idx.insert(m.idx.end(), {uint32_t(q[0]), uint32_t(q[1]), uint32_t(q[2]), uint32_t(q[0]), uint32_t(q[2]), uint32_t(q[3])});
  };
  for (int z = 1; z + 1 < nz; ++z)
    for (int y = 1; y + 1 < ny; ++y)
      for (int x = 1; x + 1 < nx; ++x) {
        const bool here = v[at(x, y, z)] < 0;
        if (here != (v[at(x + 1, y, z)] < 0)) quad(at(x, y - 1, z - 1), at(x, y, z - 1), at(x, y, z), at(x, y - 1, z), !here);
        if (here != (v[at(x, y + 1, z)] < 0)) quad(at(x - 1, y, z - 1), at(x - 1, y, z), at(x, y, z), at(x, y, z - 1), !here);
        if (here != (v[at(x, y, z + 1)] < 0)) quad(at(x - 1, y - 1, z), at(x, y - 1, z), at(x, y, z), at(x - 1, y, z), !here);
      }
  return m;
}

// The whole hand around these joints. A few tenths of a second's work, done once.
inline Mesh handSkin(const V3 j[31]) {
  const HandField f = handField(j);
  V3 lo = j[1], hi = j[1];
  const auto reach = [&](V3 p, float r) {
    lo = {std::min(lo.x, p.x - r), std::min(lo.y, p.y - r), std::min(lo.z, p.z - r)};
    hi = {std::max(hi.x, p.x + r), std::max(hi.y, p.y + r), std::max(hi.z, p.z + r)};
  };
  for (int i = 1; i <= 25; ++i) reach(j[i], 0.026f);
  reach(j[1], 0.070f);  // the cuff
  return surfaceNets(f, lo, hi, 0.0022f);
}

}  // namespace fh

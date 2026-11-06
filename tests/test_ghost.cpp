// Regression tests: rotation freedom, drag accuracy, pull-back, open-palm anchoring,
// and that the ghost never clips out of its drawing plane. Run: make test
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <utility>

#include "ghost3d.h"
#include "spot.h"
#include "handskin.h"
#include "placement.h"
#include "xform.h"

using namespace fh;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool same(const M& a, const M& b, float eps = 2e-4f) {
  for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) if (std::fabs(a.m[i][j] - b.m[i][j]) > eps) return false;
  return true;
}
static std::mt19937 rng(1234);
static float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
static M randRot(float maxDeg = 180) {
  // random axis-angle (uniform over all orientations when maxDeg = 180)
  V3 ax = norm({U(-1, 1), U(-1, 1), U(-1, 1)});
  float ang = U(-maxDeg, maxDeg) * kDeg, c = std::cos(ang), s = std::sin(ang), t = 1 - c;
  M m = ident();
  m.m[0][0] = t * ax.x * ax.x + c;        m.m[0][1] = t * ax.x * ax.y - s * ax.z; m.m[0][2] = t * ax.x * ax.z + s * ax.y;
  m.m[1][0] = t * ax.x * ax.y + s * ax.z; m.m[1][1] = t * ax.y * ax.y + c;        m.m[1][2] = t * ax.y * ax.z - s * ax.x;
  m.m[2][0] = t * ax.x * ax.z - s * ax.y; m.m[2][1] = t * ax.y * ax.z + s * ax.x; m.m[2][2] = t * ax.z * ax.z + c;
  return m;
}
static M randPose(float spread = 1.0f) {
  M m = randRot();
  m.m[0][3] = U(-spread, spread); m.m[1][3] = U(0.5f, 1.8f); m.m[2][3] = U(-spread, spread);
  return m;
}

// The hand drawn when SteamVR has no glove to read, as on a Steam Frame.
static void testHandSkin() {
  V3 j[31];
  gripHandJoints(false, j);
  const auto t0 = std::chrono::steady_clock::now();
  const Mesh m = handSkin(j);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const size_t tris = m.idx.size() / 3;
  printf("hand skin: %zu triangles, %zu vertices, built in %.0f ms\n", tris, m.p.size(), ms);
  CHECK(tris > 5000 && tris < 60000, "the hand should be detailed but affordable (%zu triangles)", tris);
  CHECK(m.p.size() == m.n.size(), "one normal per vertex");
  CHECK(ms < 3000, "built once at startup, so it mustn't be slow (%.0f ms)", ms);

  int badPoint = 0;
  for (size_t i = 0; i < m.p.size(); ++i) {
    const float nl = len(m.n[i]);
    if (!std::isfinite(m.p[i].x + m.p[i].y + m.p[i].z) || !(std::fabs(nl - 1) < 1e-3f)) ++badPoint;
  }
  CHECK(badPoint == 0, "%d vertices are broken", badPoint);

  // Closed: no holes to see through. Every edge belongs to an even number of triangles.
  std::map<std::pair<uint32_t, uint32_t>, int> edges;
  int facingOut = 0;
  for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
    const uint32_t v[3] = {m.idx[t], m.idx[t + 1], m.idx[t + 2]};
    for (int e = 0; e < 3; ++e) {
      const uint32_t a = v[e], b = v[(e + 1) % 3];
      ++edges[{std::min(a, b), std::max(a, b)}];
    }
    const V3 face = cross(m.p[v[1]] - m.p[v[0]], m.p[v[2]] - m.p[v[0]]);
    if (dot(face, m.n[v[0]] + m.n[v[1]] + m.n[v[2]]) > 0) ++facingOut;
  }
  int open = 0;
  for (const auto& e : edges) if (e.second % 2) ++open;
  CHECK(open == 0, "%d edges are open, so the hand has holes", open);
  CHECK(facingOut >= tris * 0.98, "triangles should face outward (%d of %zu do)", facingOut, tris);

  const HandField f = handField(j);
  for (int i = 1; i <= 25; ++i) CHECK(f(j[i]) < 0, "bone %d should be inside the skin", i);
  CHECK(f(j[1] + V3{0.5f, 0, 0}) > 0.3f, "far away is outside");
  // Each surface point really is on the surface.
  float worst = 0;
  for (const V3& p : m.p) worst = std::max(worst, std::fabs(f(p)));
  CHECK(worst < 0.002f, "vertices should sit on the surface (worst %.2f mm)", worst * 1000);
  // Fingers stay apart: halfway between two fingertips that aren't touching is outside the hand.
  const float gap = len(j[10] - j[15]);
  if (gap > 0.022f) CHECK(f((j[10] + j[15]) * 0.5f) > 0, "fingers must not melt together (tips %.1f mm apart)", gap * 1000);
}

int main() {
  testHandSkin();

  // 1. Any orientation survives the offset round trip (incl. gimbal-ish pitch near +-90).
  for (int i = 0; i < 5000; ++i) {
    M r = randRot();
    if (i % 10 == 0) r = mul(r, rotX(i % 20 ? 89.99f : -89.99f));
    r.m[0][3] = U(-.3f, .3f); r.m[1][3] = U(-.3f, .3f); r.m[2][3] = U(-.3f, .3f);
    CHECK(same(fromOffset(toOffset(r)), r, 5e-3f), "euler round trip, sample %d", i);
  }

  // 2. Dragging: the ghost follows the grabbing hand EXACTLY, through any rotation on every axis,
  //    and rotation never counts as "past the limit".
  float maxPos = 0.15f, maxRot = 180.f;
  for (int i = 0; i < 3000; ++i) {
    M F = randPose();                         // the hand being placed (grip)
    Offset O0; for (int k = 0; k < 3; ++k) { O0.pos[k] = U(-.05f, .05f); O0.rot[k] = U(-170, 170); }
    O0.rot[0] = U(-85, 85);
    M G0 = mul(F, fromOffset(O0));
    M Ot = mul(translate(G0.m[0][3] + .03f, G0.m[1][3], G0.m[2][3]), randRot());  // other hand near the ghost
    M taken = mul(inv(Ot), G0);
    M Ot2 = mul(Ot, randRot(180));            // twist the grabbing hand any amount, any axis
    Ot2.m[0][3] += U(-.05f, .05f); Ot2.m[1][3] += U(-.05f, .05f); Ot2.m[2][3] += U(-.05f, .05f);
    M want = mul(Ot2, taken);
    Offset O1 = toOffset(mul(inv(F), want));
    CHECK(same(mul(F, fromOffset(O1)), want, 5e-3f), "ghost follows drag, sample %d", i);
    Offset big = O1; for (float& p : big.pos) p = 0;  // rotation only
    CHECK(!beyond(big, maxPos, maxRot), "rotation flagged as over the limit (rot %.1f %.1f %.1f)", O1.rot[0], O1.rot[1], O1.rot[2]);
  }

  // 3. Pull-back keeps the orientation you chose and only shortens the distance.
  for (int i = 0; i < 1000; ++i) {
    Offset o; for (int k = 0; k < 3; ++k) { o.pos[k] = U(-.4f, .4f); o.rot[k] = U(-180, 180); }
    Offset c = clampOffset(o, maxPos, maxRot);
    CHECK(posLen(c) <= maxPos + 1e-5f, "pull-back distance");
    for (int k = 0; k < 3; ++k) CHECK(c.rot[k] == o.rot[k], "pull-back changed rotation");
  }

  // 4. Open palm sits in the grip frame as the OpenXR spec defines it.
  for (int side = 0; side < 2; ++side) {
    bool left = side == 0;
    V3 j[31];
    gripHandJoints(left, j);
    V3 mid = (j[1] + j[12]) * 0.5f;
    CHECK(std::fabs(mid.x - (left ? -0.03f : 0.03f)) < 1e-4f && std::fabs(mid.y) < 1e-4f && std::fabs(mid.z) < 1e-4f,
          "%s palm centre at (%.3f %.3f %.3f)", left ? "left" : "right", mid.x, mid.y, mid.z);
    V3 fingers = norm(j[12] - j[1]);
    CHECK(fingers.y < -0.95f, "%s fingers should point -Y (forward), got %.2f %.2f %.2f", left ? "left" : "right", fingers.x, fingers.y, fingers.z);
    V3 across = norm(j[7] - j[22]);  // pinky -> index
    CHECK(across.z < -0.9f, "%s pinky->index should be -Z (toward thumb), got z %.2f", left ? "left" : "right", across.z);
    // Palm side found from the anatomy itself: the rest pose's fingers curl toward the palm.
    V3 n = norm(cross(j[12] - j[1], j[22] - j[1]));
    float curl = dot(j[15] - j[13], n);
    V3 palm = curl > 0 ? n : n * -1.f;
    CHECK(left ? palm.x > 0.7f : palm.x < -0.7f, "%s palm should face %sX, faces %.2f %.2f %.2f", left ? "left" : "right",
          left ? "+" : "-", palm.x, palm.y, palm.z);
    CHECK(j[5].z < j[22].z, "%s thumb should be on the -Z side", left ? "left" : "right");
  }

  // 5. The ghost never clips out of its drawing plane: plane re-centres at 5 cm drift, any hand orientation.
  {
    float S = 0.36f;
    for (int i = 0; i < 3000; ++i) {
      bool left = i & 1;
      V3 j[31];
      gripHandJoints(left, j);
      V3 head{0, 1.6f, 0};
      M G = randRot();
      V3 c0{U(-.2f, .2f), U(1.0f, 1.4f), U(-.6f, -.3f)};  // plane centre, in front of the head
      V3 drift = norm({U(-1, 1), U(-1, 1), U(-1, 1)}) * U(0, 0.05f);
      V3 midLocal = (j[1] + j[14]) * 0.5f;  // same centre the overlay uses
      V3 midWorld = c0 + drift;
      V3 off = midWorld - xfP(G, midLocal);
      G.m[0][3] += off.x; G.m[1][3] += off.y; G.m[2][3] += off.z;
      V3 N = norm(head - c0), up{0, 1, 0};
      V3 X = norm(cross(up, N)), Y = cross(N, X);
      GhostRenderer::Plane pl{c0, X, Y, N, S};
      V3 eyes[2] = {head + X * -0.032f, head + X * 0.032f};
      for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 26; ++k) {
          float u, v, w;
          bool ok = GhostRenderer::proj(pl, eyes[e], xfP(G, j[k]), u, v, w);
          CHECK(ok && u > 0.02f && u < 0.98f && v > 0.02f && v < 0.98f, "joint %d off the plane (u %.2f v %.2f)", k, u, v);
        }
    }
  }

  // 6. The limit bubble always fits its drawing plane (any limit 10-30 cm, viewed from >= r + 20 cm).
  for (int i = 0; i < 3000; ++i) {
    float r = U(0.10f, 0.30f);
    Mesh sph = buildSphere(r, 12, 18);
    V3 c{U(-.3f, .3f), U(1.0f, 1.4f), U(-.6f, -.2f)};
    V3 dir = norm({U(-1, 1), U(-0.5f, 1), U(-1, 1)});
    V3 head = c + dir * U(r + 0.2f, 1.2f);
    V3 N = norm(head - c), up{0, 1, 0};
    if (std::fabs(dot(up, N)) > 0.95f) up = {0, 0, -1};
    V3 X = norm(cross(up, N)), Y = cross(N, X);
    GhostRenderer::Plane pl{c, X, Y, N, bubblePlaneSize(r)};
    V3 eyes[2] = {head + X * -0.032f, head + X * 0.032f};
    for (int e = 0; e < 2; ++e)
      for (size_t k = 0; k < sph.p.size(); ++k) {
        float u = 0, v = 0, w = 0;
        bool ok = GhostRenderer::proj(pl, eyes[e], c + sph.p[k], u, v, w);
        if (ok && dot(sph.n[k], norm(eyes[e] - (c + sph.p[k]))) < -0.05f) continue;  // far side, hidden by the near side's outline
        CHECK(ok && u > 0.005f && u < 0.995f && v > 0.005f && v < 0.995f, "bubble clipped (r %.2f, u %.3f v %.3f)", r, u, v);
      }
  }

  // The dim around the globes: clear on the hands from each eye, dark everywhere else, and each eye's hole in the right place.
  {
    const V3 eyes[2] = {{-0.032f, 1.6f, 0.f}, {0.032f, 1.6f, 0.f}};
    const SpotHole holes[2] = {{{0.2f, 1.1f, -0.4f}, 0.15f}, {{-0.3f, 1.2f, -0.45f}, 0.13f}};
    V3 centre = (holes[0].c + holes[1].c) * 0.5f, toHead = norm((eyes[0] + eyes[1]) * 0.5f - centre);
    V3 X = norm(cross(V3{0, 1, 0}, toHead)), Y = cross(toHead, X);
    GhostRenderer::Plane pl{centre, X, Y, toHead, 3.f};
    std::vector<uint8_t> px;
    drawSpot(px, 256, 256, pl, eyes, holes, 2, 0.8f);
    CHECK(px.size() == 512u * 256u * 4u, "two eyes side by side (%zu bytes)", px.size());
    auto alphaAt = [&](int eye, V3 world) {
      float u = 0, v = 0, w = 0;
      if (!GhostRenderer::proj(pl, eyes[eye], world, u, v, w) || u < 0 || u >= 1 || v < 0 || v >= 1) return -1;
      const int x = int(u * 256), y = int(v * 256);
      return int(px[(size_t(y) * 512 + size_t(eye) * 256 + x) * 4 + 3]);
    };
    for (int e = 0; e < 2; ++e) {
      for (const SpotHole& h : holes) CHECK(alphaAt(e, h.c) == 0, "eye %d can see straight through to a hand (alpha %d)", e, alphaAt(e, h.c));
      CHECK(alphaAt(e, {1.1f, 1.1f, -0.4f}) == int(0.8f * 255), "eye %d: well away from the hands it's dim (alpha %d)", e, alphaAt(e, {1.1f, 1.1f, -0.4f}));
      // A point just inside and just outside the globe's edge as this eye sees it.
      const V3 sideways = norm(cross(norm(holes[0].c - eyes[e]), {0, 1, 0}));
      CHECK(alphaAt(e, holes[0].c + sideways * 0.12f) == 0, "eye %d: inside the globe it's clear (alpha %d)", e, alphaAt(e, holes[0].c + sideways * 0.12f));
      CHECK(alphaAt(e, holes[0].c + sideways * 0.22f) > 150, "eye %d: past it, it's dim (alpha %d)", e, alphaAt(e, holes[0].c + sideways * 0.22f));
    }
    // Black all the way, so the dim darkens rather than tints, and there's no point adding light on the premultiplied path.
    bool black = true;
    for (size_t i = 0; i < px.size(); i += 4) black = black && px[i] == 0 && px[i + 1] == 0 && px[i + 2] == 0;
    CHECK(black, "only the alpha changes");
    using C = std::chrono::steady_clock;
    const auto t0 = C::now();
    for (int i = 0; i < 20; ++i) drawSpot(px, 256, 256, pl, eyes, holes, 2, 0.8f);
    const double ms = std::chrono::duration<double, std::milli>(C::now() - t0).count() / 20;
    printf("drawing the dim for both eyes: %.2f ms, while a hand's being placed\n", ms);
    CHECK(ms < 12, "the dim is too slow (%.1f ms)", ms);
  }

  printf("%s: %d checks, %d failures\n", fails ? "FAILED" : "PASSED", checks, fails);
  return fails ? 1 : 0;
}

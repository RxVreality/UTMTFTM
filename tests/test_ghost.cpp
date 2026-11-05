// Regression tests: rotation freedom, drag accuracy, pull-back, open-palm anchoring,
// and that the ghost never clips out of its drawing plane. Run: make test
#include <cstdio>
#include <random>

#include "ghost3d.h"
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

int main() {
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

  printf("%s: %d checks, %d failures\n", fails ? "FAILED" : "PASSED", checks, fails);
  return fails ? 1 : 0;
}

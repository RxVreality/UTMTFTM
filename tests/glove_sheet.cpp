// Review tool, needs SteamVR running: for every hand controller SteamVR can draw, prints where the grip sits and how far
// the hand pushes into it, then writes controllers.png with each one drawn by the dashboard's own preview and from the side.
// It gets models the same way the overlay does, so this is also a check that loading works for each of them.
// Usage: glove_sheet <out-dir> [model ...]
#include <openvr.h>

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "controller_vr.h"
#include "ghost3d.h"
#include "glove.h"
#include "platform.h"
#include "ui.h"

using namespace fh;

static void cameraFor(const std::vector<V3>& pts, V3 dir, GhostRenderer::Plane& pl, V3& cam) {
  V3 lo = pts[0], hi = pts[0];
  for (const auto& p : pts) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  V3 c = (lo + hi) * 0.5f;
  float r = 0;
  for (const auto& p : pts) r = std::max(r, len(p - c));
  V3 N = norm(dir), up{0, 1, 0};
  if (std::fabs(dot(N, up)) > 0.95f) up = {0, 0, -1};
  cam = c + N * (0.5f + r);
  V3 X = norm(cross(up, N)), Y = cross(N, X);
  pl = {c, X, Y, N, r * 2.2f};
}

// Solid hand (yellow) and controller (grey), depth-sorted, into the surface at (ox, oy).
static void renderView(cairo_surface_t* s, int ox, int oy, int S, const Mesh& hand, const Mesh& ctl, V3 dir) {
  std::vector<V3> pts = hand.p;
  pts.insert(pts.end(), ctl.p.begin(), ctl.p.end());
  GhostRenderer::Plane pl{};
  V3 cam;
  cameraFor(pts, dir, pl, cam);
  V3 eyes[1] = {cam}, light = norm(dir + V3{0.2f, 0.6f, 0.1f});
  GhostRenderer rc(S, S), rh(S, S);
  rc.clear(); rh.clear();
  M I = ident();
  rc.drawMesh(ctl, I, pl, eyes, 1, 1.f, false, &light);
  rh.drawMesh(hand, I, pl, eyes, 1, 1.f, false, &light);
  cairo_surface_flush(s);
  uint8_t* d = cairo_image_surface_get_data(s);
  int st = cairo_image_surface_get_stride(s);
  for (int y = 0; y < S; ++y)
    for (int x = 0; x < S; ++x) {
      size_t k = size_t(y) * 2 * S + x;
      bool hc = rc.zbuf[k] > 0, hh = rh.zbuf[k] > 0;
      float v[3] = {0.10f, 0.10f, 0.11f};
      if (hc && (!hh || rc.zbuf[k] > rh.zbuf[k])) { float i = rc.rgba[k * 4] / 255.f; v[0] = v[1] = v[2] = 0.82f * i; }
      else if (hh) { float i = rh.rgba[k * 4] / 255.f; v[0] = 1.f * i; v[1] = 0.82f * i; v[2] = 0.05f * i; }
      uint8_t* o = d + (oy + y) * st + (ox + x) * 4;
      o[0] = uint8_t(v[2] * 255); o[1] = uint8_t(v[1] * 255); o[2] = uint8_t(v[0] * 255); o[3] = 255;
    }
  cairo_surface_mark_dirty(s);
}

// How far inside the controller a point is, in metres. An odd number of surfaces along +X means it's inside.
struct Solid {
  struct Tri { V3 a, b, c; float y0, y1, z0, z1; };
  std::vector<Tri> tris;
  std::vector<V3> sample;
  explicit Solid(const Mesh& m) {
    for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
      V3 a = m.p[m.idx[t]], b = m.p[m.idx[t + 1]], c = m.p[m.idx[t + 2]];
      tris.push_back({a, b, c, std::min({a.y, b.y, c.y}), std::max({a.y, b.y, c.y}), std::min({a.z, b.z, c.z}), std::max({a.z, b.z, c.z})});
    }
    for (size_t i = 0; i < m.p.size(); i += 2) sample.push_back(m.p[i]);
  }
  float depth(V3 v) const {
    int hits = 0;
    for (const Tri& t : tris) {
      if (v.y < t.y0 || v.y > t.y1 || v.z < t.z0 || v.z > t.z1) continue;
      V3 e1 = t.b - t.a, e2 = t.c - t.a, d{1, 0, 0};
      V3 h = cross(d, e2);
      float det = dot(e1, h);
      if (std::fabs(det) < 1e-12f) continue;
      float f = 1 / det;
      V3 s2 = v - t.a;
      float u = f * dot(s2, h);
      if (u < 0 || u > 1) continue;
      V3 q = cross(s2, e1);
      float w = f * dot(d, q);
      if (w < 0 || u + w > 1) continue;
      if (f * dot(e2, q) > 0) ++hits;
    }
    if (!(hits & 1)) return 0;
    float best = 1e9f;
    for (const V3& p : sample) { V3 d = v - p; best = std::min(best, dot(d, d)); }
    return std::sqrt(best);
  }
};

static bool isHandController(const std::string& n) {
  for (const char* skip : {"{system}", "generic", "ref_controller", "steam_controller", "mrhat", "tracker", "ev1_3", "ev2_0", "wireless_b"})
    if (n.find(skip) != std::string::npos) return false;
  for (const char* want : {"controller", "knu_"})
    if (n.find(want) != std::string::npos) return true;
  return false;
}

int main(int argc, char** argv) {
  std::string out = argc > 1 ? argv[1] : ".";
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Utility);
  if (e != vr::VRInitError_None) { printf("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e)); return 1; }

  std::vector<std::string> models;
  for (int i = 2; i < argc; ++i) models.push_back(argv[i]);
  if (models.empty()) {
    uint32_t n = vr::VRRenderModels()->GetRenderModelCount();
    for (uint32_t i = 0; i < n; ++i) {
      char name[256] = {0};
      vr::VRRenderModels()->GetRenderModelName(i, name, sizeof name);
      if (isHandController(name)) models.push_back(name);
    }
    std::sort(models.begin(), models.end());
  }
  printf("%zu models\n", models.size());

  std::string dir = platform::steamvrDir();
  const int S = 256, cols = 4, labelH = 26, cellW = S * 2, cellH = S + labelH;
  int rows = int(models.size() + cols - 1) / cols;
  cairo_surface_t* sheet = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cols * cellW, std::max(1, rows) * cellH);
  cairo_t* c = cairo_create(sheet);
  cairo_set_source_rgb(c, 0.06, 0.06, 0.07);
  cairo_paint(c);
  cairo_select_font_face(c, "Segoe UI", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(c, 15);

  printf("%-52s %-12s %8s %8s %8s %7s | deepest push into it at the grip curl, mm: thumb index middle ring pinky palm\n", "model", "grip", "x", "y", "z", "tris");
  int done = 0, failed = 0;
  for (size_t mi = 0; mi < models.size(); ++mi) {
    const std::string& name = models[mi];
    bool left = name.find("left") != std::string::npos;
    ModelLoader loader(name);
    ModelLoad r = ModelLoad::Loading;
    for (int tries = 0; tries < 1500 && r == ModelLoad::Loading; ++tries) {
      r = loader.step();
      if (r == ModelLoad::Loading) platform::sleepMs(20);
    }
    M grip = ident();
    const char* from = gripOnModel(name, grip);
    if (r != ModelLoad::Done) { printf("%-52s DID NOT LOAD\n", name.c_str()); ++failed; continue; }
    ++done;
    const Mesh& raw = loader.mesh;

    M toGrip = inv(grip);
    Mesh ctl = raw;
    for (auto& p : ctl.p) p = xfP(toGrip, p);
    for (auto& n : ctl.n) n = norm(xfN(toGrip, n));

    Mesh hand;
    V3 j[31];
    std::vector<uint8_t> group;
    M seat;
    float deepest[kFingerCount + 1] = {0};
    if (posedRightGlove(dir, kGripCurl, hand, j, &group) && handInGrip(dir, left, seat)) {
      if (left) mirrorHand(hand, j);
      for (auto& p : hand.p) p = xfP(seat, p);
      for (auto& n : hand.n) n = norm(xfN(seat, n));
      Solid solid(ctl);
      for (size_t i = 0; i < hand.p.size(); i += 3) deepest[group[i]] = std::max(deepest[group[i]], solid.depth(hand.p[i]));
    }
    printf("%-52s %-12s %8.4f %8.4f %8.4f %7zu |", name.c_str(), from ? from : "NONE", grip.m[0][3], grip.m[1][3], grip.m[2][3], raw.idx.size() / 3);
    for (float d : deepest) printf(" %5.1f", d * 1000);
    printf("\n");
    fflush(stdout);

    int ox = int(mi % cols) * cellW, oy = int(mi / cols) * cellH;
    Canvas cv(S, S);
    setControllerModel(left, raw, grip);
    drawHandPreview(cv, 0, 0, S, left, Offset{}, Offset{}, 1.0, 0.824, 0.0);
    cairo_surface_flush(cv.s);
    cairo_set_source_surface(c, cv.s, ox, oy + labelH);
    cairo_paint(c);
    renderView(sheet, ox + S, oy + labelH, S, hand, ctl, {left ? -1.f : 1.f, 0.2f, 0.25f});
    cairo_set_source_rgb(c, 0.92, 0.92, 0.9);
    cairo_move_to(c, ox + 8, oy + 18);
    cairo_show_text(c, (name + "  [" + (from ? from : "no grip") + "]").c_str());
  }
  cairo_surface_flush(sheet);
  std::string png = out + "/controllers.png";
  cairo_surface_write_to_png(sheet, png.c_str());
  cairo_destroy(c);
  cairo_surface_destroy(sheet);
  printf("loaded %d, failed %d, wrote %s\n", done, failed, png.c_str());
  vr::VR_Shutdown();
  return failed ? 1 : 0;
}

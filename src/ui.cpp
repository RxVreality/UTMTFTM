#include "ui.h"

#include "macro.h"

#include "glove.h"
#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

#ifndef _WIN32
#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace fh {

// ---------------------------------------------------------------- canvas
Canvas::Canvas(int W, int H) : w(W), h(H) {
  s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  c = cairo_create(s);
  rgba.resize(size_t(w) * h * 4);
}
Canvas::~Canvas() { cairo_destroy(c); cairo_surface_destroy(s); }
void Canvas::clear() { cairo_save(c); cairo_set_operator(c, CAIRO_OPERATOR_CLEAR); cairo_paint(c); cairo_restore(c); }
const std::vector<uint8_t>& Canvas::toRGBA() {
  cairo_surface_flush(s);
  const uint8_t* src = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint8_t* p = src + y * stride + x * 4;  // BGRA premultiplied
      uint8_t* d = &rgba[(size_t(y) * w + x) * 4];
      uint8_t a = p[3];
      if (a == 255) { d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = 255; continue; }
      if (a == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
      auto un = [a](uint8_t v) { return uint8_t(std::min(255, v * 255 / a)); };
      d[0] = un(p[2]); d[1] = un(p[1]); d[2] = un(p[0]); d[3] = a;
    }
  return rgba;
}
bool Canvas::savePNG(const std::string& path) {
  cairo_surface_flush(s);
  return cairo_surface_write_to_png(s, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

// ---------------------------------------------------------------- theme + primitives
// A calibration tool with some speed in it: graphite, printed-looking off-white, one hi-vis yellow for the action.
// Shapes lean at one shared angle instead of being rounded, and ruled lines replace cards. Yellow only ever means "do this".
namespace {
struct RGB { double r, g, b; };
const RGB INK{0.082, 0.082, 0.078}, RAISED{0.145, 0.145, 0.138}, LINE{0.30, 0.30, 0.285};
const RGB PAPER{0.925, 0.925, 0.905}, DIM{0.62, 0.62, 0.59}, FAINT{0.40, 0.40, 0.38};
const RGB SIGNAL{1.0, 0.824, 0.0}, ALERT{1.0, 0.33, 0.20}, LIVE{0.36, 0.86, 0.38};

const double kLean = 0.20;  // sideways shift per unit of height: every shape's sides stay parallel, whatever its size

void set(cairo_t* c, RGB k, double a = 1) { cairo_set_source_rgba(c, k.r, k.g, k.b, a); }
void box(cairo_t* c, double x, double y, double w, double h, RGB k, double a = 1) {
  cairo_rectangle(c, x, y, w, h); set(c, k, a); cairo_fill(c);
}
void hline(cairo_t* c, double x0, double x1, double y, RGB k = LINE, double a = 1, double width = 1) { box(c, x0, y, x1 - x0, width, k, a); }

// A shape leaning forward, inside the box x, y, w, h: its top edge starts d in from the left, its bottom edge ends d short of the right.
void leanPath(cairo_t* c, double x, double y, double w, double h) {
  double d = h * kLean;
  cairo_new_sub_path(c);
  cairo_move_to(c, x + d, y);
  cairo_line_to(c, x + w, y);
  cairo_line_to(c, x + w - d, y + h);
  cairo_line_to(c, x, y + h);
  cairo_close_path(c);
}
void slab(cairo_t* c, double x, double y, double w, double h, RGB k, double a = 1) {
  leanPath(c, x, y, w, h); set(c, k, a); cairo_fill(c);
}
void frame(cairo_t* c, double x, double y, double w, double h, RGB k, double a = 1, double width = 2) {
  leanPath(c, x + width / 2, y + width / 2, w - width, h - width); set(c, k, a);
  cairo_set_line_width(c, width); cairo_set_line_join(c, CAIRO_LINE_JOIN_MITER); cairo_stroke(c);
}

enum Face { Body, Semi };
#ifdef _WIN32
// Bahnschrift is Windows' own DIN-style face, on every PC since Windows 10 1709. It's a variable font, and through
// cairo only its regular and bold load: its other named weights silently come out as Arial.
void face(cairo_t* c, Face f, double size) {
  static const bool din = [] {
    const char* w = std::getenv("WINDIR");
    return w && std::ifstream(std::string(w) + "\\Fonts\\bahnschrift.ttf").good();
  }();
  cairo_select_font_face(c, din ? "Bahnschrift" : "Segoe UI", CAIRO_FONT_SLANT_NORMAL, f == Body ? CAIRO_FONT_WEIGHT_NORMAL : CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(c, size);
}
#else
// Linux has no Bahnschrift, and which fonts a headset has isn't ours to count on, so Barlow, the closest free match,
// ships with the program. Both faces stay loaded for as long as it runs.
cairo_font_face_t* loadFace(const char* file) {
  static FT_Library ft = nullptr;
  if (!ft && FT_Init_FreeType(&ft)) return nullptr;
  FT_Face f = nullptr;
  if (FT_New_Face(ft, (platform::exeDir() + "/fonts/" + file).c_str(), 0, &f)) return nullptr;
  return cairo_ft_font_face_create_for_ft_face(f, 0);
}
void face(cairo_t* c, Face f, double size) {
  static cairo_font_face_t* const faces[2] = {loadFace("Barlow-Regular.ttf"), loadFace("Barlow-SemiBold.ttf")};
  static cairo_font_options_t* const options = [] {  // exact letter widths, so spaced-out capitals sit evenly
    cairo_font_options_t* o = cairo_font_options_create();
    cairo_font_options_set_hint_metrics(o, CAIRO_HINT_METRICS_OFF);
    return o;
  }();
  if (faces[f]) cairo_set_font_face(c, faces[f]);
  else cairo_select_font_face(c, "sans-serif", CAIRO_FONT_SLANT_NORMAL, f == Body ? CAIRO_FONT_WEIGHT_NORMAL : CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_options(c, options);
  cairo_set_font_size(c, size);
}
#endif
double textW(cairo_t* c, const std::string& s, double size, bool bold) {
  face(c, bold ? Semi : Body, size);
  cairo_text_extents_t e; cairo_text_extents(c, s.c_str(), &e);
  return e.x_advance;
}
// align: 0 left, 1 centre, 2 right. Shrinks the font to fit maxW if given.
double text(cairo_t* c, double x, double y, double size, const std::string& s, bool bold, RGB k, double a = 1,
            int align = 0, double maxW = 0) {
  double w = textW(c, s, size, bold);
  while (maxW > 0 && w > maxW && size > 10) { size -= 1; w = textW(c, s, size, bold); }
  face(c, bold ? Semi : Body, size);
  double sx = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
  cairo_move_to(c, sx, y);
  set(c, k, a);
  cairo_show_text(c, s.c_str());
  return w;
}

// Capitals, spaced out a little, the way a maker prints them on a panel.
std::vector<std::string> utf8Chars(const std::string& s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size();) {
    unsigned char b = s[i];
    size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    out.push_back(s.substr(i, n));
    i += n;
  }
  return out;
}
std::string upper(std::string s) {
  for (auto& ch : s) if (static_cast<unsigned char>(ch) < 0x80) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  return s;
}
double capsW(cairo_t* c, const std::string& s, double size, double track, Face f = Semi) {
  face(c, f, size);
  double w = 0;
  for (const auto& ch : utf8Chars(s)) { cairo_text_extents_t e; cairo_text_extents(c, ch.c_str(), &e); w += e.x_advance + track; }
  return w > 0 ? w - track : 0;
}
double caps(cairo_t* c, double x, double y, double size, const std::string& raw, RGB k, double a = 1, int align = 0, double track = 1.4, double maxW = 0, Face f = Semi) {
  std::string s = upper(raw);
  double w = capsW(c, s, size, track, f);
  while (maxW > 0 && w > maxW && size > 11) { size -= 1; w = capsW(c, s, size, track, f); }
  face(c, f, size);
  double px = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
  set(c, k, a);
  for (const auto& ch : utf8Chars(s)) {
    cairo_text_extents_t e; cairo_text_extents(c, ch.c_str(), &e);
    cairo_move_to(c, px, y);
    cairo_show_text(c, ch.c_str());
    px += e.x_advance + track;
  }
  return w;
}
// The quieter kind: lighter weight, for naming things rather than doing them.
double label(cairo_t* c, double x, double y, double size, const std::string& s, RGB k = DIM, int align = 0) {
  return caps(c, x, y, size, s, k, 1, align, 1.6, 0, Body);
}

// A small indicator light.
void lamp(cairo_t* c, double x, double y, double s, RGB k) { slab(c, x, y, s * 1.2, s, k); }

const std::string* g_hover = nullptr;  // the laser and press states of the page being drawn, for click feedback
const std::string* g_down = nullptr;

// Static press states over any tappable shape: outlined under the laser, darker while held.
// onFill: the shape is filled with a bright colour, so the marks have to be dark to show.
void fxOver(cairo_t* c, double x, double y, double w, double h, const std::string& id, bool enabled = true, bool onFill = false) {
  if (!g_hover || !g_down) return;
  bool hov = *g_hover == id, dn = *g_down == id && enabled;
  if (!hov && !dn) return;
  if (dn) slab(c, x, y, w, h, {0, 0, 0}, 0.22);
  else if (enabled && !onFill) slab(c, x, y, w, h, PAPER, 0.10);
  double in = onFill ? 4 : 0;
  frame(c, x + in, y + in, w - 2 * in, h - 2 * in, onFill ? INK : PAPER, enabled ? 1 : 0.35, onFill ? 2 : 3);
}

enum Style { Outline, Primary, Danger, Solid, OnSignal };

void button(cairo_t* c, std::vector<Button>& out, double x, double y, double w, double h, const std::string& id,
            const std::string& text, Style style, bool enabled = true) {
  out.push_back({x, y, w, h, id, enabled});
  bool dn = g_down && *g_down == id && enabled;
  if (dn) { x += 2.5; y += 2.5; w -= 5; h -= 5; }  // pushed in
  bool filled = enabled && (style == Primary || style == Danger || style == Solid);
  if (filled) slab(c, x, y, w, h, style == Primary ? SIGNAL : style == Danger ? ALERT : PAPER);
  else if (!enabled) frame(c, x, y, w, h, FAINT, 0.6, 2);
  else frame(c, x, y, w, h, style == OnSignal ? INK : PAPER, style == OnSignal ? 1 : 0.7, 2);
  fxOver(c, x, y, w, h, id, enabled, filled || style == OnSignal);
  double size = std::min(h * 0.42, 23.0);
  RGB ink = !enabled ? FAINT : (filled || style == OnSignal) ? INK : PAPER;
  caps(c, x + w / 2, y + h / 2 + size * 0.36, size, text, ink, 1, 1, 1.4, w - 20 - h * kLean);
}

// A plain pictogram of an open hand, cut off square at the wrist like a safety sign, leaning with everything else.
// (cx, cy) is the middle of the shape; s is roughly its height.
void handMark(cairo_t* c, double cx, double cy, double s, RGB k) {
  cairo_save(c);
  cairo_translate(c, cx, cy);
  cairo_matrix_t lean;
  cairo_matrix_init(&lean, 1, 0, -kLean, 1, 0, 0);
  cairo_transform(c, &lean);
  cairo_scale(c, s, s);
  set(c, k);
  cairo_move_to(c, -0.250, -0.040);
  cairo_line_to(c, 0.240, -0.040);
  cairo_line_to(c, 0.240, 0.150);
  cairo_curve_to(c, 0.240, 0.250, 0.170, 0.300, 0.130, 0.345);
  cairo_line_to(c, 0.130, 0.500);
  cairo_line_to(c, -0.150, 0.500);
  cairo_line_to(c, -0.150, 0.345);
  cairo_curve_to(c, -0.190, 0.300, -0.250, 0.250, -0.250, 0.150);
  cairo_close_path(c);
  cairo_fill(c);
  cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width(c, 0.104);
  const double f[4][2] = {{-0.198, -0.300}, {-0.066, -0.440}, {0.066, -0.470}, {0.190, -0.390}};
  for (const auto& d : f) {
    cairo_move_to(c, d[0], 0.0);
    cairo_line_to(c, d[0], d[1]);
    cairo_stroke(c);
  }
  cairo_set_line_width(c, 0.116);
  cairo_move_to(c, 0.180, 0.200);
  cairo_line_to(c, 0.410, -0.050);
  cairo_stroke(c);
  cairo_restore(c);
}

// The app's mark: a black hand on a yellow slab, with its own offset shadow behind it.
void markTile(cairo_t* c, double x, double y, double w, double h) {
  slab(c, x, y, w, h, SIGNAL);
  double s = h * 0.66, cx = x + w * 0.49, cy = y + h * 0.545;
  handMark(c, cx - h * 0.085, cy - h * 0.050, s, {0.78, 0.62, 0.0});
  handMark(c, cx + h * 0.030, cy + h * 0.025, s, INK);
}

void check(cairo_t* c, double x, double y, double s, RGB k) {
  cairo_move_to(c, x, y + s * 0.55); cairo_line_to(c, x + s * 0.38, y + s * 0.9); cairo_line_to(c, x + s, y + s * 0.15);
  set(c, k); cairo_set_line_width(c, s * 0.2); cairo_set_line_cap(c, CAIRO_LINE_CAP_BUTT);
  cairo_set_line_join(c, CAIRO_LINE_JOIN_MITER); cairo_stroke(c);
}

std::string fmt(const char* f, double v) { char b[64]; snprintf(b, sizeof b, f, v); return b; }
}  // namespace

// ---------------------------------------------------------------- click feedback
const Button* hitButton(const std::vector<Button>& b, double x, double y) {
  for (const auto& k : b)
    if (x >= k.x && x <= k.x + k.w && y >= k.y && y <= k.y + k.h) return &k;
  return nullptr;
}

// ---------------------------------------------------------------- text helpers
std::string friendlyOffset(const Offset& o) {
  if (o.zero()) return "No offset";
  std::string out;
  auto part = [&](float v, const char* pos, const char* neg) {
    if (std::fabs(v) < 0.005f) return;
    if (!out.empty()) out += ", ";
    out += fmt("%.1f cm ", std::fabs(v) * 100) + (v > 0 ? pos : neg);
  };
  part(-o.pos[2], "forward", "back");
  part(o.pos[1], "up", "down");
  part(o.pos[0], "right", "left");
  M m = fromOffset(o);
  float tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
  float ang = std::acos(std::clamp((tr - 1) / 2, -1.f, 1.f)) / kDeg;
  if (ang >= 0.5f) {
    if (!out.empty()) out += "  \xC2\xB7  ";
    out += fmt("turned %.0f\xC2\xB0", ang);
  }
  return out.empty() ? "No offset" : out;
}

// ---------------------------------------------------------------- 3D preview
// The controller each hand is on, as SteamVR draws it, moved into the grip frame so it sits in the hand where
// the game puts it. Until SteamVR has said what it is, a generic shape.
static Mesh g_controller[2];
static bool g_controllerSet[2] = {false, false};
static int g_controllerRev = 0;

void setControllerModel(bool left, const Mesh& inDeviceFrame, const M& gripFromRaw) {
  Mesh& m = g_controller[left ? 0 : 1];
  m = inDeviceFrame;
  M toGrip = inv(gripFromRaw);
  for (auto& p : m.p) p = xfP(toGrip, p);
  for (auto& n : m.n) n = norm(xfN(toGrip, n));
  g_controllerSet[left ? 0 : 1] = true;
  ++g_controllerRev;
}

static const Mesh& controllerModel(bool left) {
  static const Mesh generic = buildController();
  return g_controllerSet[left ? 0 : 1] ? g_controller[left ? 0 : 1] : generic;
}

void drawHandPreview(Canvas& cv, double x, double y, double size, bool left, const Offset& applied,
                     const Offset& ghostBase, double r, double g, double b) {
  int S = (int)size;
  struct Cached { std::string key; cairo_surface_t* img = nullptr; };
  static Cached cache[2];
  char kb[512];
  snprintf(kb, sizeof kb, "%d|%d|%.5f,%.5f,%.5f,%.3f,%.3f,%.3f|%.5f,%.5f,%.5f,%.3f,%.3f,%.3f|%.3f,%.3f,%.3f", S, g_controllerRev,
           applied.pos[0], applied.pos[1], applied.pos[2], applied.rot[0], applied.rot[1], applied.rot[2],
           ghostBase.pos[0], ghostBase.pos[1], ghostBase.pos[2], ghostBase.rot[0], ghostBase.rot[1], ghostBase.rot[2], r, g, b);
  Cached& ce = cache[left ? 0 : 1];
  if (ce.img && ce.key == kb) {
    cairo_t* c = cv.c;
    cairo_save(c);
    cairo_rectangle(c, x, y, size, size);
    cairo_clip(c);
    set(c, INK); cairo_paint(c);
    cairo_set_source_surface(c, ce.img, x, y);
    cairo_paint(c);
    cairo_restore(c);
    return;
  }
  const Mesh& controller = controllerModel(left);
  const Mesh& hand = gripHandMesh(left);
  bool moved = !applied.zero();
  // Your own view of your own hand: the camera sits behind the wrist and above it, looking down over the back of
  // the hand, so the wrist end is nearest and the knuckles and trigger point away. The long lens keeps the wrist
  // from ballooning. Grey = where the game puts the hand, coloured = where yours goes. With no offset there's only the one.
  float sx = left ? -1.f : 1.f;
  M G0 = ident();
  M D = mul(G0, fromOffset(ghostBase));
  M H = mul(mul(G0, fromOffset(applied)), fromOffset(ghostBase));
  // Frame to the content: every vertex of both hands and the controller fits, as large as possible,
  // with the camera always far enough back (any offset up to the limit, any rotation).
  std::vector<V3> pts;
  pts.reserve(2 * hand.p.size() + controller.p.size());
  for (const auto& p : hand.p) { if (moved) pts.push_back(xfP(D, p)); pts.push_back(xfP(H, p)); }
  for (const auto& p : controller.p) pts.push_back(xfP(G0, p));
  V3 lo = pts[0], hi = pts[0];
  for (const auto& p : pts) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  V3 centre = (lo + hi) * 0.5f;
  float radius = 0;
  for (const auto& p : pts) radius = std::max(radius, len(p - centre));
  V3 N = norm(V3{sx * -0.06f, 0.52f, -0.85f}), up = norm(V3{0, 0, -1});
  V3 cam = centre + N * (0.90f + radius);
  V3 X = norm(cross(up, N)), Y = cross(N, X);
  float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
  for (const auto& p : pts) {
    V3 q = cam + (p - cam) * (dot(centre - cam, N) / dot(p - cam, N));  // onto the plane through centre
    float u = dot(q - centre, X), v = dot(q - centre, Y);
    u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, v); v1 = std::max(v1, v);
  }
  V3 c2 = centre + X * ((u0 + u1) * 0.5f) + Y * ((v0 + v1) * 0.5f);
  float span = std::max(u1 - u0, v1 - v0) * 1.08f;
  GhostRenderer::Plane pl{c2, X, Y, N, span};
  V3 eyes[1] = {cam};

  // Drawn at three times the size and averaged down, so the edges come out smooth.
  const int kSS = 3, R = S * kSS;
  GhostRenderer rc(R, R), rd(moved ? R : 1, moved ? R : 1), rh(R, R);
  V3 light = norm(N * 0.60f + up * 0.70f + X * (sx * 0.30f));
  rc.clear();
  rc.drawMesh(controller, G0, pl, eyes, 1, 1.0f, false, &light);
  rh.clear();
  rh.drawMesh(hand, H, pl, eyes, 1, 1.0f, false, &light);
  if (moved) { rd.clear(); rd.drawMesh(hand, D, pl, eyes, 1, 1.0f, false, &light); }

  cairo_surface_t* img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, S, S);
  uint8_t* d = cairo_image_surface_get_data(img);
  int stride = cairo_image_surface_get_stride(img);
  for (int yy = 0; yy < S; ++yy)
    for (int xx = 0; xx < S; ++xx) {
      float sum[4] = {0, 0, 0, 0};
      for (int sy = 0; sy < kSS; ++sy)
        for (int sxx = 0; sxx < kSS; ++sxx) {
          size_t k = size_t(yy * kSS + sy) * 2 * R + (xx * kSS + sxx);
          // Up to three surfaces meet here. Nearest first: the controller is solid, your hand almost
          // solid, and the game's hand a faint grey behind it.
          struct Layer { float z, a, c[3]; } lay[3];
          int n = 0;
          if (rc.zbuf[k] > 0) { float i = rc.rgba[k * 4] / 255.f; lay[n++] = {rc.zbuf[k], 1.f, {0.74f * i, 0.74f * i, 0.72f * i}}; }
          if (rh.zbuf[k] > 0) {
            float i = rh.rgba[k * 4] / 255.f, v = 0.18f + 0.95f * i;
            lay[n++] = {rh.zbuf[k], 0.97f, {std::min(1.f, float(r) * v), std::min(1.f, float(g) * v), std::min(1.f, float(b) * v)}};
          }
          if (moved && rd.zbuf[k] > 0) { float i = rd.rgba[k * 4] / 255.f; lay[n++] = {rd.zbuf[k], 0.42f, {0.70f * i, 0.70f * i, 0.68f * i}}; }
          std::sort(lay, lay + n, [](const Layer& p, const Layer& q) { return p.z > q.z; });
          float pm[4] = {0, 0, 0, 0};  // premultiplied, built front to back
          for (int l = 0; l < n; ++l) {
            float t = (1 - pm[3]) * lay[l].a;
            for (int ch = 0; ch < 3; ++ch) pm[ch] += t * lay[l].c[ch];
            pm[3] += t;
          }
          for (int ch = 0; ch < 4; ++ch) sum[ch] += pm[ch];
        }
      const float kN = float(kSS * kSS);
      uint8_t* o = d + yy * stride + xx * 4;
      o[0] = uint8_t(std::min(1.f, sum[2] / kN) * 255); o[1] = uint8_t(std::min(1.f, sum[1] / kN) * 255);
      o[2] = uint8_t(std::min(1.f, sum[0] / kN) * 255); o[3] = uint8_t(std::min(1.f, sum[3] / kN) * 255);
    }
  cairo_surface_mark_dirty(img);
  cairo_t* c = cv.c;
  cairo_save(c);
  cairo_rectangle(c, x, y, size, size);
  cairo_clip(c);
  set(c, INK); cairo_paint(c);
  cairo_set_source_surface(c, img, x, y);
  cairo_paint(c);
  cairo_restore(c);
  if (ce.img) cairo_surface_destroy(ce.img);
  ce.img = img; ce.key = kb;
}

// ---------------------------------------------------------------- dashboard
// The top of both pages: the mark and name, which page this is, what's running and how it's connected, and Close.
static void pageHeader(cairo_t* c, std::vector<Button>& btn, int width, const std::string& game, LiveState live, bool confirmClose, DashPage page) {
  const double L = 44, R = width - 44;
  markTile(c, L, 20, 80, 64);
  caps(c, L + 94, 58, 42, "UTMTFTM", PAPER, 1, 0, 3.5);
  label(c, L + 95, 82, 15, "Use the medium to fix the medium");
  const int nTabs = 4;
  const double tabX = 456, tabY = 34, tabH = 42, tabW = 104, tabD = tabH * kLean;
  const struct { DashPage p; const char* id; const char* name; } tabs[4] = {
      {DashPage::Hands, "page_hands", "Hands"}, {DashPage::Bindings, "page_bindings", "Bindings"}, {DashPage::Combos, "page_combos", "Combos"}, {DashPage::Decaf, "page_decaf", "Decaf"}};
  for (int i = 0; i < nTabs; ++i) {
    const double x = tabX + i * (tabW - tabD + 3);
    const bool on = tabs[i].p == page || (page == DashPage::Recorded && tabs[i].p == DashPage::Combos);
    if (on) slab(c, x, tabY, tabW, tabH, PAPER);
    else frame(c, x, tabY, tabW, tabH, PAPER, 0.45, 2);
    fxOver(c, x, tabY, tabW, tabH, tabs[i].id, true, on);
    caps(c, x + tabW / 2, tabY + 27, 16, tabs[i].name, on ? INK : PAPER, 1, 1, 1.2);
    btn.push_back({i == 0 ? x : x + tabD / 2, tabY, tabW - tabD / 2, tabH, tabs[i].id});
  }
  if (confirmClose) button(c, btn, R - 214, 30, 214, 46, "close", "Tap again to close", Danger);
  else button(c, btn, R - 110, 30, 110, 46, "close", "Close", Outline);
  const double right = R - (confirmClose ? 234 : 130);
  // The page's note has the whole name. Up here it has to stay big enough to read and clear of the tabs, so it loses its end.
  const double nameW = right - (tabX + nTabs * tabW - (nTabs - 1) * tabD + 12);
  if (!game.empty()) {
    double size = 21;
    while (size > 17 && textW(c, game, size, false) > nameW) size -= 1;
    auto chars = utf8Chars(game);
    std::string cut = game;
    for (size_t n = chars.size(); n > 1 && textW(c, cut, size, false) > nameW; --n) {
      cut.clear();
      for (size_t i = 0; i + 1 < n; ++i) cut += chars[i];
      while (!cut.empty() && cut.back() == ' ') cut.pop_back();
      cut += "...";
    }
    text(c, right, 50, size, cut, false, PAPER, 1, 2);
  } else {
    text(c, right, 50, 21, "No game running", false, DIM, 1, 2, nameW);
  }
  if (live == LiveState::Live || live == LiveState::NextLaunch) {
    const bool on = live == LiveState::Live;
    const double w = caps(c, right, 77, 15, on ? "Live in game" : "Applies next launch", PAPER, 1, 2);
    lamp(c, right - w - 24, 65, 12, on ? LIVE : SIGNAL);
  }
  hline(c, L, R, 98, PAPER, 0.9, 2);
}

std::string dashKey(const DashModel& m) {
  std::string k = m.game + "|" + std::to_string((int)m.live) + "|" + std::to_string(m.saveForGame) +
                  std::to_string(m.gameHasCustom) + std::to_string(m.needsMatch) + std::to_string(m.armed) +
                  std::to_string(m.confirmClose) + "|" + std::to_string(m.maxCm) + "|" + m.status + std::to_string(m.statusWarn);
  char b[96];
  for (int h = 0; h < 2; ++h) {
    const HandCard& cd = m.cards[h];
    for (int i = 0; i < 3; ++i) {
      snprintf(b, sizeof b, "|%.4f,%.2f", cd.applied.pos[i], cd.applied.rot[i]); k += b;
      snprintf(b, sizeof b, "|%.4f,%.2f", m.ghostBase[h].pos[i], m.ghostBase[h].rot[i]); k += b;
    }
    k += "|" + std::to_string((int)cd.badge) + std::to_string(cd.ghostMatched);
  }
  k += "|fx:" + m.hover + "," + m.down + "|" + std::to_string(m.statusGood) + std::to_string(m.fine) + std::to_string(m.fineBig) + m.hint;
  return k;
}

// The one-choice-of-several control: the chosen one solid, the others outlined.
struct Segment { std::string id, text; bool on, enabled; };
static void segments(cairo_t* c, std::vector<Button>& out, double x, double y, double w, double h, const std::vector<Segment>& list, double textSize = 19) {
  const double d = h * kLean, gap = 3;
  const double each = (w + d * (list.size() - 1) - gap * (list.size() - 1)) / list.size();
  for (size_t i = 0; i < list.size(); ++i) {
    const Segment& s = list[i];
    const double bx = x + i * (each - d + gap);
    if (s.on) slab(c, bx, y, each, h, PAPER);
    else frame(c, bx, y, each, h, s.enabled ? PAPER : FAINT, s.enabled ? 0.45 : 0.5, 2);
    fxOver(c, bx, y, each, h, s.id, s.enabled, s.on);
    caps(c, bx + each / 2, y + h / 2 + textSize * 0.36, textSize, s.text, s.on ? INK : s.enabled ? PAPER : FAINT, 1, 1, 1.4, each - 24);
    const double hitX = i == 0 ? bx : bx + d / 2;
    out.push_back({hitX, y, each - d / 2, h, s.id, s.enabled});
  }
}

bool parseNudge(const std::string& id, int& hand, int& axis, int& dir) {
  if (id.size() != 11 || id.rfind("nudge_", 0) != 0 || id[7] != '_' || id[9] != '_') return false;
  hand = id[6] - '0';
  axis = id[8] - '0';
  dir = id[10] == 'p' ? 1 : -1;
  return (hand == 0 || hand == 1) && axis >= 0 && axis <= 5 && (id[10] == 'p' || id[10] == 'm');
}

std::string nudgeHint(const std::string& id, bool big) {
  int hand = 0, axis = 0, dir = 0;
  if (!parseNudge(id, hand, axis, dir)) return "";
  const std::string who = hand ? "your right hand " : "your left hand ", far = big ? "1 cm " : "1 mm ", turn = big ? "5\xC2\xB0 " : "1\xC2\xB0 ";
  switch (axis) {
    case 0: return "Moves " + who + far + (dir > 0 ? "forward." : "back.");
    case 1: return "Moves " + who + far + (dir > 0 ? "up." : "down.");
    case 2: return "Moves " + who + far + (dir > 0 ? "to the right." : "to the left.");
    case 3: return "Tips " + who + turn + (dir > 0 ? "up." : "down.");
    case 4: return "Turns " + who + turn + (dir > 0 ? "to the right." : "to the left.");
    default: return "Rolls " + who + turn + (dir > 0 ? "clockwise." : "anticlockwise.");
  }
}

// What's on a fine adjust arrow. Filled in the colour it's drawn with.
enum class Glyph { Up, Down, Left, Right, Clockwise, Anticlockwise };
static void glyph(cairo_t* c, double cx, double cy, double s, Glyph g, RGB k) {
  set(c, k);
  auto tri = [&](double x0, double y0, double x1, double y1, double x2, double y2) {
    cairo_move_to(c, x0, y0); cairo_line_to(c, x1, y1); cairo_line_to(c, x2, y2); cairo_close_path(c); cairo_fill(c);
  };
  switch (g) {
    case Glyph::Up: tri(cx - s, cy + s * 0.7, cx + s, cy + s * 0.7, cx, cy - s * 0.8); break;
    case Glyph::Down: tri(cx - s, cy - s * 0.7, cx + s, cy - s * 0.7, cx, cy + s * 0.8); break;
    case Glyph::Left: tri(cx + s * 0.7, cy - s, cx + s * 0.7, cy + s, cx - s * 0.8, cy); break;
    case Glyph::Right: tri(cx - s * 0.7, cy - s, cx - s * 0.7, cy + s, cx + s * 0.8, cy); break;
    case Glyph::Clockwise:
    case Glyph::Anticlockwise: {
      // A ring, open at the bottom, with the arrowhead where it ends.
      const bool cw = g == Glyph::Clockwise;
      const double pi = 3.14159265, r = s * 0.9, end = cw ? pi * 0.15 : pi * 0.85;
      cairo_set_line_width(c, s * 0.42);
      cairo_set_line_cap(c, CAIRO_LINE_CAP_BUTT);
      cairo_new_sub_path(c);
      if (cw) cairo_arc(c, cx, cy, r, -pi * 1.15, end);
      else cairo_arc_negative(c, cx, cy, r, pi * 0.15, end);
      cairo_stroke(c);
      const double ax = cx + r * std::cos(end), ay = cy + r * std::sin(end), a = s * 0.6, sign = cw ? -1 : 1;
      tri(ax - a, ay - s * 0.05, ax + a, ay - s * 0.05, ax + sign * s * 0.25, ay + a);
      break;
    }
  }
}

void drawDashboard(Canvas& cv, const DashModel& m, std::vector<Button>& btn) {
  btn.clear();
  g_hover = &m.hover;
  g_down = &m.down;
  cairo_t* c = cv.c;
  set(c, INK); cairo_paint(c);
  const double L = 44, R = cv.w - 44, mid = cv.w / 2.0;

  pageHeader(c, btn, cv.w, m.game, m.live, m.confirmClose, DashPage::Hands);

  // the next thing to do. Waiting on the dashboard beats everything else, live games explain the bubble,
  // and only a running game that can't move live offers the ghost match.
  const double headY = 138, subY = 164;
  if (m.armed) {
    slab(c, L, 108, R - L, 60, SIGNAL);
    text(c, L + 28, 134, 25, "Now close the dashboard", true, INK);
    text(c, L + 22, 158, 18, "You can start grabbing as soon as it's shut.", false, INK, 1, 0, R - L - 220);
    button(c, btn, R - 158, 116, 134, 44, "cancel_armed", "Cancel", OnSignal);
  } else if (m.fine) {
    text(c, L, headY, 27, m.live == LiveState::NoGame ? "Fine adjust the default for every game" : "Fine adjust your hands", true, PAPER);
    text(c, L, subY, 18, m.live == LiveState::Live ? "Tap the arrows. The hand you aren't pointing with moves in the game as you tap."
                         : m.live == LiveState::NoGame ? "Tap the arrows. Start a game first to see your hands move as you tap."
                                                       : "Tap the arrows. This game takes new offsets the next time it launches.", false, DIM, 1, 0, R - L);
  } else if (m.live == LiveState::Live) {
    text(c, L, headY, 27, "Grab your in-game hand and move it", true, PAPER);
    text(c, L, subY, 18, "A bubble shows how far it can go (" + std::to_string(m.maxCm) + " cm). Let go outside it and it's pulled back.", false, DIM, 1, 0, R - L);
  } else if (m.live == LiveState::NoGame) {
    text(c, L, headY, 27, "Set a default for every game", true, PAPER);
    text(c, L, subY, 18, "Or start a game first to see your hands move live as you place them.", false, DIM, 1, 0, R - L);
  } else if (m.needsMatch) {
    text(c, L, headY, 27, "Match the ghost to your in-game hands", true, PAPER);
    text(c, L, subY, 18, "This game can't move your hands live, so a ghost previews them. Match it once.", false, DIM, 1, 0, R - L - 240);
    button(c, btn, R - 210, 118, 210, 46, "match", "Match ghost", Solid);
  } else {
    text(c, L, headY, 27, "Ghost matched to your in-game hands", true, PAPER);
    text(c, L, subY, 18, "It previews your hands in games that can't move them live.", false, DIM, 1, 0, R - L - 260);
    button(c, btn, R - 230, 118, 230, 46, "match", "Re-match ghost", Outline);
  }

  // the two hands, side by side, each under its own leaning name plate
  const char* title[2] = {"Left hand", "Right hand"};
  const double cw = mid - 12 - L;
  for (int h = 0; h < 2; ++h) {
    const HandCard& cd = m.cards[h];
    double x = h ? mid + 12 : L;
    slab(c, x, 178, cw, 40, RAISED);
    caps(c, x + 20, 207, 24, title[h], PAPER, 1, 0, 2);
    if (cd.badge == Badge::GameCustom) label(c, x + cw - 18, 205, 15, "This game", DIM, 2);
    else if (cd.badge == Badge::GlobalDefault) label(c, x + cw - 18, 205, 15, "All-games default", DIM, 2);
    else if (cd.badge == Badge::AllGamesDefault) label(c, x + cw - 18, 205, 15, "All games", DIM, 2);

    const double ps = 256, py = 224;
    drawHandPreview(cv, x - 4, py, ps, h == 0, cd.applied, m.ghostBase[h], SIGNAL.r, SIGNAL.g, SIGNAL.b);

    // the readout, ruled like a spec sheet. Always all six, so the eye knows where each number lives.
    const Offset& o = cd.applied;
    // Each reads positive the way its name says. Yaw and roll are stored the other way round.
    const char* lab[6] = {"Forward", "Up", "Right", "Pitch", "Yaw", "Roll"};
    double val[6] = {-o.pos[2] * 100, o.pos[1] * 100, o.pos[0] * 100, o.rot[0], -o.rot[1], -o.rot[2]};
    const Glyph less[6] = {Glyph::Down, Glyph::Down, Glyph::Left, Glyph::Down, Glyph::Left, Glyph::Anticlockwise};
    const Glyph more[6] = {Glyph::Up, Glyph::Up, Glyph::Right, Glyph::Up, Glyph::Right, Glyph::Clockwise};
    const double tx0 = x + 268, tx1 = x + cw, rowH = 41, top = 228, arrowW = 44, arrowH = 34;
    for (int i = 0; i < 6; ++i) {
      double ry = top + i * rowH, base = ry + 29;
      bool cm = i < 3, none = std::fabs(val[i]) < (cm ? 0.05 : 0.5);
      label(c, tx0, base, 16, lab[i]);
      // With the arrows out, the number sits between them, and ends where the arrow on the right starts.
      double end = tx1;
      if (m.fine) {
        end = tx1 - arrowW - 8;
        const double ay = ry + (rowH - arrowH) / 2 + 1;
        for (int side = 0; side < 2; ++side) {
          const std::string id = std::string("nudge_") + (h ? "1_" : "0_") + std::to_string(i) + (side ? "_p" : "_m");
          const double ax = side ? tx1 - arrowW : tx1 - 2 * arrowW - 100;
          frame(c, ax, ay, arrowW, arrowH, PAPER, 0.7, 2);
          fxOver(c, ax, ay, arrowW, arrowH, id);
          glyph(c, ax + arrowW / 2, ay + arrowH / 2, 7.5, side ? more[i] : less[i], PAPER);
          btn.push_back({ax, ay, arrowW, arrowH, id});
        }
      }
      const char* unit = cm ? "cm" : "\xC2\xB0";
      double uw = cm ? capsW(c, "CM", 14, 1, Body) : 12;
      if (cm) caps(c, end, base, 14, "cm", none ? FAINT : DIM, 1, 2, 1, 0, Body);
      else text(c, end, base, 25, unit, true, none ? FAINT : PAPER, 1, 2);
      std::string v = none ? (cm ? "0.0" : "0") : fmt(cm ? "%+.1f" : "%+.0f", val[i]);
      size_t minus = v.find('-');
      if (minus != std::string::npos) v.replace(minus, 1, "\xE2\x80\x93");  // an en dash: this font has no true minus sign
      text(c, end - uw - (cm ? 6 : 1), base, 25, v, true, none ? FAINT : PAPER, 1, 2);
      if (i < 5) hline(c, tx0, tx1, ry + rowH, i == 2 ? DIM : LINE, 0.8);
    }
    double bty = 490, bth = 54, rw = 150;
    button(c, btn, x, bty, cw - rw - 10, bth, h ? "place_right" : "place_left", "Move", Primary);
    button(c, btn, x + cw - rw, bty, rw, bth, h ? "reset_right" : "reset_left", "Reset", Outline, !o.zero());
  }
  hline(c, L, R, 558);

  // actions for both hands, and where they're saved
  double ay = 572, ah = 54, sx = mid + 12;
  if (m.fine) {
    const double stepW = label(c, L, ay + 34, 16, "Step");
    const double segL = L + stepW + 16;
    segments(c, btn, segL, ay, L + cw - 150 - segL, ah, {{"fine_small", "1 mm, 1\xC2\xB0", !m.fineBig, true}, {"fine_big", "1 cm, 5\xC2\xB0", m.fineBig, true}});
    button(c, btn, L + cw - 140, ay, 140, ah, "fine_toggle", "Done", Solid);
  } else {
    button(c, btn, L, ay, cw - 190, ah, "place_both", "Move both hands", Outline);
    button(c, btn, L + cw - 180, ay, 180, ah, "fine_toggle", "Fine adjust", Outline);
  }
  double saveW = label(c, sx, ay + 34, 16, "Save for");
  double segX = sx + saveW + 18, segD = ah * kLean, segGap = 3, segW = (R - segX + segD - segGap) / 2;
  for (int i = 0; i < 2; ++i) {
    bool on = (i == 0) == m.saveForGame, enabled = i == 1 || !m.game.empty();
    double bx = segX + i * (segW - segD + segGap);
    std::string id = i == 0 ? "scope_game" : "scope_global";
    if (on) slab(c, bx, ay, segW, ah, PAPER);
    else frame(c, bx, ay, segW, ah, enabled ? PAPER : FAINT, enabled ? 0.45 : 0.5, 2);
    fxOver(c, bx, ay, segW, ah, id, enabled, on);
    caps(c, bx + segW / 2, ay + ah / 2 + 8, 21, i == 0 ? "This game" : "All games", on ? INK : enabled ? PAPER : FAINT, 1, 1, 1.4, segW - 30);
    double hitX = i == 0 ? bx : bx + segD / 2;
    btn.push_back({hitX, ay, segW - segD / 2, ah, id, enabled});
  }

  // how far a hand may go, as stops on a steel rule, its ticks leaning with everything else
  double fy = 640, fh = 44;
  double maxW = label(c, L, fy + 29, 16, "Max distance");
  const int lims[4] = {10, 15, 20, 30};
  const double rx = L + maxW + 16, rd = fh * kLean, rw = std::min(92.0, (L + cw - rx - rd) / 4);
  hline(c, rx, rx + 4 * rw, fy + fh - 1, PAPER, 0.45);
  cairo_set_line_width(c, 1);
  set(c, PAPER, 0.45);
  for (int t = 0; t <= 20; ++t) {
    double tx = rx + t * rw / 5, th = t % 5 == 0 ? 12 : 6;
    cairo_move_to(c, tx, fy + fh - 1);
    cairo_line_to(c, tx + th * kLean, fy + fh - 1 - th);
  }
  cairo_stroke(c);
  for (int i = 0; i < 4; ++i) {
    double bx = rx + i * rw;
    bool on = m.maxCm == lims[i];
    std::string id = "lim_" + std::to_string(lims[i]);
    if (on) slab(c, bx, fy, rw + rd, fh, PAPER);
    fxOver(c, bx, fy, rw + rd, fh, id, true, on);
    caps(c, bx + (rw + rd) / 2, fy + 28, 18, std::to_string(lims[i]) + " cm", on ? INK : PAPER, 1, 1, 1);
    btn.push_back({bx + rd / 2, fy, rw, fh, id});
  }
  if (m.gameHasCustom) button(c, btn, sx, fy, R - sx, fh, "use_global", "Use the all-games default", Outline);

  // one line of status along the bottom edge
  hline(c, L, R, 700);
  const double baseY = 734;
  std::string st = m.status;
  if (!st.empty()) {
    lamp(c, L, baseY - 14, 14, m.statusWarn ? ALERT : m.statusGood ? LIVE : SIGNAL);
    text(c, L + 28, baseY, 20, st, true, PAPER, 1, 0, R - L - 28);
  } else if (!m.hint.empty()) {
    text(c, L, baseY, 19, m.hint, false, PAPER, 1, 0, R - L);
  } else {
    if (m.armed || m.live == LiveState::NoGame || m.fine) {}  // the line above the hands already says it
    else if (m.live == LiveState::Live) st = "Changes show on your in-game hands as you move them.";
    else st = "This game takes new offsets the next time it launches.";
    if (!st.empty()) text(c, L, baseY, 19, st, false, DIM, 1, 0, R - L);
  }
  g_hover = g_down = nullptr;
}

// ---------------------------------------------------------------- Decaffeinate page
const char* decafName(int level) {
  static const char* const names[kDecafSteps] = {"Off", "Tuned", "Strong", "Maximum"};
  return level >= 0 && level < kDecafSteps ? names[level] : "Off";
}

std::string decafKey(const DecafModel& m) {
  char b[160];
  std::string k = m.game + "|" + std::to_string((int)m.live) + "|" + std::to_string(m.level) + std::to_string(m.saveForGame) + std::to_string(m.gameHasOwn) +
                  std::to_string(m.ready) + std::to_string(m.confirmClose) + "|" + m.status + std::to_string(m.statusWarn) + std::to_string(m.statusGood);
  for (const DecafHand& h : m.hands) {
    // To the hundredth of a degree: the meter moves, but not so finely it redraws on every pass.
    snprintf(b, sizeof b, "|%d,%.2f,%.2f", h.measured, h.raw, h.out);
    k += b;
  }
  return k + "|fx:" + m.hover + "," + m.down;
}

void drawDecaf(Canvas& cv, const DecafModel& m, std::vector<Button>& btn) {
  btn.clear();
  g_hover = &m.hover;
  g_down = &m.down;
  cairo_t* c = cv.c;
  set(c, INK); cairo_paint(c);
  const double L = 44, R = cv.w - 44, mid = cv.w / 2.0;
  pageHeader(c, btn, cv.w, m.game, m.live, m.confirmClose, DashPage::Decaf);

  text(c, L, 138, 27, "Take the shake out of your aim", true, PAPER);
  text(c, L, 164, 18, "Smooths the tiny trembles of a hand held still, and lets go the moment you move. Made for aiming down sights.", false, DIM, 1, 0, R - L);

  // The four steps, as tiles: what each feels like, and what it measured on a test hand (tests/test_decaf.cpp).
  struct Step { const char* line1; const char* line2; const char* measured; };
  const Step steps[kDecafSteps] = {
      {"Your hands exactly", "as they're tracked.", ""},
      {"Takes the edge off.", "You won't feel it working.", "5x less shake, slow moves 24 ms late"},
      {"Steady when you aim.", "A touch of weight on slow moves.", "10x less shake, slow moves 49 ms late"},
      {"As steady as it gets.", "Slow moves feel heavy.", "18x less shake, slow moves 80 ms late"},
  };
  const double ty = 188, th = 132, td = th * kLean, gap = 4;
  const double tw = (R - L + td * (kDecafSteps - 1) - gap * (kDecafSteps - 1)) / kDecafSteps;
  for (int i = 0; i < kDecafSteps; ++i) {
    const double x = L + i * (tw - td + gap);
    const bool on = m.level == i;
    const std::string id = "decaf_" + std::to_string(i);
    if (on) slab(c, x, ty, tw, th, PAPER);
    else frame(c, x, ty, tw, th, PAPER, 0.45, 2);
    fxOver(c, x, ty, tw, th, id, true, on);
    const RGB ink = on ? INK : PAPER, soft = on ? INK : DIM;
    // Text sits inside the lean: each line starts further right the higher it is.
    auto lx = [&](double y) { return x + 24 + (ty + th - y) * kLean; };
    caps(c, lx(ty + 40), ty + 40, 26, decafName(i), ink, 1, 0, 2);
    text(c, lx(ty + 72), ty + 72, 17, steps[i].line1, false, ink, 1, 0, tw - 40 - td);
    text(c, lx(ty + 94), ty + 94, 17, steps[i].line2, false, ink, 1, 0, tw - 40 - td);
    if (steps[i].measured[0]) text(c, lx(ty + 120), ty + 120, 13.5, steps[i].measured, false, soft, on ? 0.75 : 1, 0, tw - 40 - td);
    btn.push_back({i == 0 ? x : x + td / 2, ty, tw - td / 2, th, id});
  }

  // How much each hand shakes while held still, as tracked and after, live from the driver.
  const char* title[2] = {"Left hand", "Right hand"};
  const double cw = mid - 12 - L, py = 342;
  for (int h = 0; h < 2; ++h) {
    const double x = h ? mid + 12 : L;
    const DecafHand& d = m.hands[h];
    slab(c, x, py, cw, 40, RAISED);
    caps(c, x + 20, py + 29, 24, title[h], PAPER, 1, 0, 2);
    label(c, x + cw - 18, py + 27, 15, "Shake while held still", DIM, 2);
    const double barL = x + 190, barR = x + cw - 92, barH = 26;
    const double scale = std::max(0.2f, std::max(d.raw, d.out) * 1.15f);
    auto bar = [&](double y, const char* name, float v, RGB k, bool show) {
      label(c, x, y + 20, 16, name);
      hline(c, barL, barR, y + barH, LINE, 1);
      if (show) {
        const double w = std::max(barH * kLean + 2, (barR - barL) * std::min(1.0, v / scale));
        slab(c, barL, y, w, barH, k);
        text(c, x + cw - 4, y + 22, 24, fmt("%.2f", v) + "\xC2\xB0", true, PAPER, 1, 2);
      } else {
        text(c, x + cw - 4, y + 22, 24, "\xE2\x80\x93", true, FAINT, 1, 2);
      }
    };
    const bool on = m.level > 0;
    bar(py + 62, "As tracked", d.raw, DIM, d.measured);
    bar(py + 106, "Decaffeinated", on ? d.out : d.raw, on ? SIGNAL : FAINT, d.measured);
    std::string verdict;
    RGB vk = DIM;
    if (!m.ready) verdict = "Needs SteamVR restarted once";
    else if (m.live == LiveState::NoGame) verdict = "Start a game to measure it";
    else if (!d.measured) verdict = "Hold this hand still to measure it";
    else if (!on) verdict = "Pick a step to steady it";
    else if (d.out > 0.0005f) { verdict = fmt("%.0fx steadier", d.raw / d.out); vk = PAPER; }
    else { verdict = "Steady"; vk = PAPER; }
    text(c, x, py + 182, 22, verdict, vk.r == PAPER.r, vk, 1, 0, cw);
  }
  hline(c, L, R, 558);

  // Where it's saved, the same as the Hands tab.
  const double ay = 572, ah = 54, sx = mid + 12;
  if (m.gameHasOwn) button(c, btn, L, ay, cw, ah, "decaf_use_global", "Use the all-games step", Outline);
  else text(c, L, ay + 34, 17, m.game.empty() ? "Every game uses this unless one has its own." : "This game uses the all-games step.", false, DIM, 1, 0, cw);
  const double saveW = label(c, sx, ay + 34, 16, "Save for");
  segments(c, btn, sx + saveW + 18, ay, R - (sx + saveW + 18), ah,
           {{"scope_game", "This game", m.saveForGame && !m.game.empty(), !m.game.empty()}, {"scope_global", "All games", !m.saveForGame || m.game.empty(), true}}, 21);

  text(c, L, 664, 17, "Some games have smoothing of their own, like H3VR2's Controller Filtering. Use one or the other: two at once feel late.",
       false, DIM, 1, 0, R - L);

  hline(c, L, R, 700);
  const double baseY = 734;
  if (!m.status.empty()) {
    lamp(c, L, baseY - 14, 14, m.statusWarn ? ALERT : m.statusGood ? LIVE : SIGNAL);
    text(c, L + 28, baseY, 20, m.status, true, PAPER, 1, 0, R - L - 28);
  } else {
    const char* st = !m.ready ? "The add-on SteamVR has loaded is older. Restart SteamVR once and it's ready."
                     : m.live == LiveState::NoGame ? "It only works with a game running, so SteamVR's own pointers stay as they are."
                                                   : "Changes reach your in-game hands straight away. No restart.";
    text(c, L, baseY, 19, st, false, m.ready ? DIM : PAPER, 1, 0, R - L);
  }
  g_hover = g_down = nullptr;
}

// ---------------------------------------------------------------- bindings page
ControllerPicture::ControllerPicture(std::shared_ptr<cairo_surface_t> image, std::vector<float> nearest, std::map<std::string, Spot> spots, int sizePx, V3 centre, V3 x, V3 y, V3 n, V3 eye, float span)
    : image_(std::move(image)), nearest_(std::move(nearest)), spots_(std::move(spots)), size_(sizePx), c_(centre), x_(x), y_(y), n_(n), eye_(eye), span_(span) {}

const ControllerPicture::Spot* ControllerPicture::spot(const std::string& part) const {
  auto it = spots_.find(part);
  return it == spots_.end() ? nullptr : &it->second;
}

void ControllerPicture::place(V3 p, double& x, double& y, bool& behind) const {
  GhostRenderer::Plane pl{c_, x_, y_, n_, span_};
  float u = 0, v = 0, w = 0;
  behind = false;
  if (!GhostRenderer::proj(pl, eye_, p, u, v, w)) { x = y = -1; return; }
  x = u;
  y = v;
  const int px = int(u * size_), py = int(v * size_);
  if (px < 0 || py < 0 || px >= size_ || py >= size_) return;
  const float front = nearest_[size_t(py) * size_ + px];
  behind = front > 0 && front < w - 0.012f;  // a part's middle is a little under its own surface
}

// Seen from in front and to the outside, so the trigger and the grip show as well as the buttons on top. The body is
// drawn dark and everything you can press pale, which is how you tell them apart at this size.
ControllerPicture bakeController(const Mesh& mesh, const std::vector<ModelPart>& parts, bool left, int S) {
  const float sx = left ? 1.f : -1.f;
  Mesh body, controls;
  auto copy = [&](Mesh& to, const ModelPart* part) {
    const size_t first = part ? part->firstIndex : 0, count = part ? part->indexCount : mesh.idx.size();
    const uint32_t base = (uint32_t)to.p.size();
    std::map<uint32_t, uint32_t> seen;
    for (size_t i = first; i < first + count && i < mesh.idx.size(); ++i) {
      const uint32_t v = mesh.idx[i];
      auto it = seen.find(v);
      if (it == seen.end()) {
        it = seen.emplace(v, base + uint32_t(seen.size())).first;
        to.p.push_back(mesh.p[v]);
        to.n.push_back(mesh.n[v]);
      }
      to.idx.push_back(it->second);
    }
  };
  for (const ModelPart& part : parts) {
    const bool isBody = part.name == "body" || part.name == "status" || part.name == "led" || part.name == "trackhat";
    copy(isBody ? body : controls, &part);
  }
  if (parts.empty()) copy(body, nullptr);

  std::vector<V3> pts = body.p;
  pts.insert(pts.end(), controls.p.begin(), controls.p.end());
  if (pts.empty()) pts.push_back({0, 0, 0});
  V3 lo = pts[0], hi = pts[0];
  for (const V3& p : pts) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const V3 centre = (lo + hi) * 0.5f;
  float radius = 0;
  for (const V3& p : pts) radius = std::max(radius, len(p - centre));
  const V3 N = norm(V3{sx * 0.68f, 0.36f, -0.64f}), up = norm(V3{0, 0.25f, -1});
  const V3 cam = centre + N * (0.60f + radius);
  const V3 X = norm(cross(up, N)), Y = cross(N, X);
  float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
  for (const V3& p : pts) {
    const V3 q = cam + (p - cam) * (dot(centre - cam, N) / dot(p - cam, N));
    const float u = dot(q - centre, X), v = dot(q - centre, Y);
    u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, v); v1 = std::max(v1, v);
  }
  const V3 c2 = centre + X * ((u0 + u1) * 0.5f) + Y * ((v0 + v1) * 0.5f);
  const float span = std::max(u1 - u0, v1 - v0) * 1.06f;
  GhostRenderer::Plane pl{c2, X, Y, N, span};
  V3 eyes[1] = {cam};

  const int kSS = 3, R = S * kSS;
  GhostRenderer draw(R, R);
  draw.clear();
  const V3 light = norm(N * 0.60f + up * 0.70f + X * (sx * -0.30f));
  const M I = ident();
  draw.drawMesh(body, I, pl, eyes, 1, 0.55f, false, &light);
  draw.drawMesh(controls, I, pl, eyes, 1, 1.0f, false, &light);

  cairo_surface_t* img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, S, S);
  uint8_t* d = cairo_image_surface_get_data(img);
  const int stride = cairo_image_surface_get_stride(img);
  std::vector<float> nearest(size_t(S) * S, 0.f);
  for (int y = 0; y < S; ++y)
    for (int x = 0; x < S; ++x) {
      float lum = 0, cover = 0, closest = 0;
      for (int a = 0; a < kSS; ++a)
        for (int b = 0; b < kSS; ++b) {
          const size_t k = size_t(y * kSS + a) * 2 * R + (x * kSS + b);
          if (draw.zbuf[k] > 0) { lum += draw.rgba[k * 4] / 255.f; cover += 1; closest = std::max(closest, draw.zbuf[k]); }
        }
      lum /= kSS * kSS;
      cover /= kSS * kSS;
      nearest[size_t(y) * S + x] = closest > 0 ? 1 / closest : 0;
      uint8_t* o = d + y * stride + x * 4;
      const uint8_t v = uint8_t(std::min(1.f, lum * 0.92f) * 255);
      o[0] = o[1] = o[2] = v;
      o[3] = uint8_t(cover * 255);
    }
  cairo_surface_mark_dirty(img);

  // Each part's line ends on the middle of what can be seen of it. A trigger's centre is inside the controller.
  std::map<std::string, ControllerPicture::Spot> spots;
  for (const ModelPart& part : parts) {
    double sumX = 0, sumY = 0;
    int visible = 0;
    for (size_t i = part.firstIndex; i < size_t(part.firstIndex) + part.indexCount && i < mesh.idx.size(); ++i) {
      float u = 0, v = 0, w = 0;
      if (!GhostRenderer::proj(pl, cam, mesh.p[mesh.idx[i]], u, v, w)) continue;
      const int px = int(u * S), py = int(v * S);
      if (px < 0 || py < 0 || px >= S || py >= S) continue;
      const float front = nearest[size_t(py) * S + px];
      if (front > 0 && w <= front + 0.002f) { sumX += u; sumY += v; ++visible; }
    }
    if (visible > 0) spots[part.name] = {sumX / visible, sumY / visible, false};
    else {
      float u = 0, v = 0, w = 0;
      if (GhostRenderer::proj(pl, cam, part.centre, u, v, w)) spots[part.name] = {u, v, true};
    }
  }
  return ControllerPicture(std::shared_ptr<cairo_surface_t>(img, cairo_surface_destroy), std::move(nearest), std::move(spots), S, c2, X, Y, N, cam, span);
}

std::string bindKey(const BindModel& m) {
  std::string k = m.game + "|" + std::to_string((int)m.live) + "|" + m.ref + "|" + m.layoutName + std::to_string(m.layoutOn) +
                  std::to_string(m.layoutOther) + std::to_string(m.layoutOk) + std::to_string(m.saveForGame) + m.controls +
                  std::to_string(m.wiringChanged) + std::to_string(m.confirmClose) + "|" + m.armed + "|" + m.status +
                  std::to_string(m.statusWarn) + std::to_string(m.statusGood) + "|fx:" + m.hover + "," + m.down + "|";
  for (const BindSide* s : {&m.theirs, &m.yours}) {
    k += s->title + "," + s->note + ",";
    for (int h = 0; h < 2; ++h) {
      k += s->pic[h] ? "p" : "-";
      for (const BindRow& r : s->rows[h]) k += r.id + "=" + r.action + ";";
    }
  }
  for (const auto& l : m.lit) k += l + ",";
  k += "|" + m.prompt + std::to_string(m.picking) + m.target + m.chipsLabel + std::to_string(m.directions) + std::to_string(m.stickAxes);
  for (const auto& ch : m.chips) k += ch.id + "=" + ch.text + std::to_string(ch.on) + ";";
  return k;
}

// One hand: the controller in the middle, the buttons down either side with a line to where each one really is.
static void bindHand(cairo_t* c, std::vector<Button>& btn, const std::string& armedId, const std::string& targetId, const std::set<std::string>& litRows, const ControllerPicture* pic, const std::vector<BindRow>& rows, double X0, double W, double y0, double H) {
  const double S = 184, px = X0 + (W - S) / 2, py = y0 + (H - S) / 2, cellW = (W - S) / 2 - 4, rowH = 34;
  if (pic) {
    cairo_set_source_surface(c, pic->image(), px, py);
    cairo_paint(c);
  }
  std::vector<const BindRow*> col[2];
  for (const BindRow& r : rows) {
    if (r.x >= 0) col[r.x < 0.5 ? 0 : 1].push_back(&r);
    else col[col[0].size() <= col[1].size() ? 0 : 1].push_back(&r);
  }
  for (int a = 0; a < 2; ++a)
    while (col[a].size() > 6) {  // too many on one side: the nearest to the middle goes across
      auto nearest = std::min_element(col[a].begin(), col[a].end(), [](const BindRow* p, const BindRow* q) { return std::fabs(p->x - 0.5) < std::fabs(q->x - 0.5); });
      col[1 - a].push_back(*nearest);
      col[a].erase(nearest);
    }
  for (int a = 0; a < 2; ++a) {
    std::stable_sort(col[a].begin(), col[a].end(), [](const BindRow* p, const BindRow* q) { return p->y < q->y; });
    const double top = y0 + (H - col[a].size() * rowH) / 2;
    for (size_t i = 0; i < col[a].size(); ++i) {
      const BindRow& r = *col[a][i];
      // Being rewired, or pressed as part of a combo, it's solid yellow. Touched as part of a combo, it's outlined in it.
      const bool lit = litRows.count(r.id) > 0, armed = armedId == r.id || targetId == r.id || r.mark == 2, touched = r.mark == 1;
      const double cx = a == 0 ? X0 : X0 + W - cellW, cy = top + i * rowH, mid = cy + rowH / 2;
      if (armed) slab(c, cx, cy + 1, cellW, rowH - 2, SIGNAL);
      else if (touched) frame(c, cx, cy + 1, cellW, rowH - 2, SIGNAL, 1, 2);
      else if (lit) slab(c, cx, cy + 1, cellW, rowH - 2, PAPER, 0.14);
      if (r.x >= 0) {
        const double ex = px + r.x * S, ey = py + r.y * S, sx = a == 0 ? cx + cellW : cx;
        const bool yellow = armed || touched, strong = yellow || lit;
        set(c, yellow ? SIGNAL : PAPER, strong ? 1 : 0.42);
        cairo_set_line_width(c, strong ? 2 : 1);
        cairo_move_to(c, sx, mid);
        cairo_line_to(c, ex, ey);
        cairo_stroke(c);
        const double dot = strong ? 5 : 3.5;
        if (r.behind) frame(c, ex - dot, ey - dot, dot * 2.4, dot * 2, yellow ? SIGNAL : PAPER, strong ? 1 : 0.8, 1.2);
        else slab(c, ex - dot, ey - dot, dot * 2.4, dot * 2, yellow ? SIGNAL : PAPER, strong ? 1 : 0.85);
      }
      const int align = a == 0 ? 2 : 0;
      const double tx = a == 0 ? cx + cellW - 10 : cx + 12, tw = cellW - 18;
      if (r.mark) {
        caps(c, tx, mid + 6, 17, r.markText.empty() ? (r.mark == 2 ? "Press " : "Touch ") + r.name : r.markText, armed ? INK : SIGNAL, 1, align, 1.2, tw);
      } else if (r.unused) {
        label(c, tx, cy + 14, 13, r.name, armed ? INK : DIM, align);
        text(c, tx, cy + 31, 17, "Not used", false, armed ? INK : FAINT, 1, align, tw);
      } else if (r.action.empty()) {
        caps(c, tx, mid + 7, 18, r.name, armed ? INK : PAPER, 1, align, 1.2, tw);
      } else {
        label(c, tx, cy + 14, 13, r.name, armed ? INK : DIM, align);
        // Two things on one button only stay if they can still be read.
        std::string says = r.action;
        const size_t both = says.find(" / ");
        if (both != std::string::npos && textW(c, says, 15, true) > tw) says = says.substr(0, both);
        text(c, tx, cy + 31, 18, says, true, armed ? INK : PAPER, 1, align, tw);
      }
      fxOver(c, cx, cy + 1, cellW, rowH - 2, r.id, true, armed);
      btn.push_back({cx, cy, cellW, rowH, r.id, true});
    }
  }
}

void drawBindings(Canvas& cv, const BindModel& m, std::vector<Button>& btn) {
  btn.clear();
  g_hover = &m.hover;
  g_down = &m.down;
  cairo_t* c = cv.c;
  set(c, INK); cairo_paint(c);
  const double L = 44, R = cv.w - 44, mid = cv.w / 2.0;
  pageHeader(c, btn, cv.w, m.game, m.live, m.confirmClose, DashPage::Bindings);

  // Which controller to hold yours up against. As many as this SteamVR can draw, so the names shrink to share the line.
  const double chipY = 108, chipH = 36, chipD = chipH * kLean;
  const double compareW = label(c, L, chipY + 24, 16, "Compare with");
  const double chipX0 = L + compareW + 16, chipSpace = R - chipX0;
  double size = 16, total = 0;
  for (; size > 11; size -= 1) {
    total = 0;
    for (const std::string& n : m.refs) total += capsW(c, upper(n), size, 1) + 22 + 3;
    if (total + chipD <= chipSpace) break;
  }
  double chipX = chipX0;
  for (const std::string& n : m.refs) {
    const double w = capsW(c, upper(n), size, 1) + 22 + chipD;
    const bool on = n == m.ref;
    const std::string id = "ref_" + n;
    if (on) slab(c, chipX, chipY, w, chipH, PAPER);
    else frame(c, chipX, chipY, w, chipH, PAPER, 0.35, 1.5);
    fxOver(c, chipX, chipY, w, chipH, id, true, on);
    caps(c, chipX + w / 2, chipY + chipH / 2 + size * 0.36, size, n, on ? INK : PAPER, 1, 1, 1);
    btn.push_back({chipX + chipD / 2, chipY, w - chipD, chipH, id});
    chipX += w - chipD + 3;
  }

  // The two controllers, theirs above yours.
  const BindSide* sides[2] = {&m.theirs, &m.yours};
  const double panelY[2] = {150, 390}, stripH = 30, bodyH = 208, cw = mid - 12 - L;
  for (int i = 0; i < 2; ++i) {
    const BindSide& s = *sides[i];
    const double y = panelY[i];
    hline(c, L, R, y, LINE);
    const double tw = caps(c, L, y + 23, 19, s.title + (i ? " (yours)" : ""), PAPER, 1, 0, 1.6);
    label(c, L + tw + 18, y + 22, 15, s.note, DIM);
    if (i == 1 && m.wiringChanged) button(c, btn, R - 170, y + 2, 170, 26, "bind_reset_wiring", "Undo rewiring", Outline);
    if (i == 0 && !m.controls.empty()) {  // the game's different sets of controls, one at a time
      const double w = capsW(c, upper(m.controls), 16, 1.2), prevX = R - 54 - w - 54;
      button(c, btn, R - 44, y + 2, 44, 26, "bind_controls_next", "", Outline);
      button(c, btn, prevX, y + 2, 44, 26, "bind_controls_prev", "", Outline);
      caps(c, R - 22, y + 21, 18, ">", PAPER, 1, 1, 0);
      caps(c, prevX + 22, y + 21, 18, "<", PAPER, 1, 1, 0);
      caps(c, R - 54, y + 22, 16, m.controls, PAPER, 1, 2, 1.2);
    }
    for (int h = 0; h < 2; ++h)
      bindHand(c, btn, m.armed, m.target, m.lit, s.pic[h], s.rows[h], h ? mid + 12 : L, cw, y + stripH, bodyH);
  }

  // What this game does about it, or the next step if you've picked a button to rewire.
  const double ay = 632, ah = 54, sx = mid + 12;
  hline(c, L, R, ay - 4, LINE);
  if (!m.armed.empty()) {
    slab(c, L, ay, R - L, ah, SIGNAL);
    const std::string prompt = m.prompt.empty() ? "Now tap what your " + m.armedName + " should do, on the " + m.ref + " above" : m.prompt;
    text(c, L + 26, ay + 35, 23, prompt, true, INK, 1, 0, R - L - (m.picking ? 180 : 320));
    if (!m.picking) button(c, btn, R - 290, ay + 7, 130, 40, "bind_nothing", "Nothing", OnSignal);
    button(c, btn, R - 150, ay + 7, 130, 40, "bind_cancel", "Cancel", OnSignal);
  } else {
    const double useW = label(c, L, ay + 34, 16, m.hasGame ? "This game uses" : "Games use");
    const double segX = L + useW + 16;
    segments(c, btn, segX, ay, L + cw - segX, ah, {{"bind_dev", "Developer's", !m.layoutOn && !m.layoutOther, true}, {"bind_layout", m.layoutName, m.layoutOn, m.layoutOk}});
    const double saveW = label(c, sx, ay + 34, 16, "Save for");
    const double seg2 = sx + saveW + 18;
    segments(c, btn, seg2, ay, R - seg2, ah, {{"bind_scope_game", "This game", m.saveForGame, m.hasGame}, {"bind_scope_global", "All games", !m.saveForGame, true}});
  }

  hline(c, L, R, 700);
  const double baseY = 734;
  if (!m.armed.empty() && !m.chips.empty()) {
    // One part of the control, under the prompt. Stick directions can be named either way, and the switch for that sits
    // at the end, where the names it changes are.
    const double chipY = 709, chipH = 40;
    const double lw = label(c, L, chipY + 26, 16, m.chipsLabel);
    const double switchW = m.directions ? 290 : 0;
    const double x0 = L + lw + 16, x1 = R - (switchW ? switchW + 24 : 0);
    std::vector<Segment> list;
    for (const BindModel::Chip& ch : m.chips) list.push_back({ch.id, ch.text, ch.on, true});
    segments(c, btn, x0, chipY, std::min(x1 - x0, 150.0 * list.size()), chipH, list, 16);  // a few don't stretch across
    if (switchW) {
      const double sw = label(c, R - switchW, chipY + 26, 16, "Names");
      segments(c, btn, R - switchW + sw + 14, chipY, switchW - sw - 14, chipH, {{"bind_dirs_compass", "N E S W", !m.stickAxes, true}, {"bind_dirs_axes", "Y+ X+", m.stickAxes, true}}, 16);
    }
  } else if (!m.status.empty()) {
    lamp(c, L, baseY - 14, 14, m.statusWarn ? ALERT : m.statusGood ? LIVE : SIGNAL);
    text(c, L + 28, baseY, 20, m.status, true, PAPER, 1, 0, R - L - 28);
  } else {
    const std::string st = m.hasGame ? "Point at a button to find it on the other controller. Tap one of yours to change what it does."
                                     : "No game running, so this shows how the buttons match up. Tap one of yours to change it.";
    text(c, L, baseY, 19, st, false, DIM, 1, 0, R - L);
  }
  g_hover = g_down = nullptr;
}

// ---------------------------------------------------------------- combos page
std::string comboKey(const ComboModel& m) {
  std::string k = m.game + "|" + std::to_string((int)m.live) + "|" + std::to_string(m.step) + std::to_string(m.canMake) + "|" + m.why + "|" + m.picked + "|" + m.note + "|" + m.controls + "|" +
                  std::to_string(m.pageNo) + "/" + std::to_string(m.pages) + "|" + m.status + std::to_string(m.statusWarn) + std::to_string(m.statusGood) + std::to_string(m.confirmClose) + "|fx:" + m.hover + "," + m.down + "|" + m.yours.title + "," + m.yours.note;
  for (int h = 0; h < 2; ++h) {
    k += m.yours.pic[h] ? "p" : "-";
    for (const BindRow& r : m.yours.rows[h]) k += r.id + "=" + r.action + "," + std::to_string(r.mark) + r.markText + ";";
  }
  k += "|" + m.chipsLabel + std::to_string(m.stickAxes);
  for (const auto& ch : m.chips) k += ch.id + "=" + ch.text + std::to_string(ch.on) + ";";
  for (const ComboEntry& e : m.saved) k += e.inputs + ">" + e.action + ";";
  for (const ComboIdea& e : m.ideas) k += e.id + ";";
  for (const ComboChoice& e : m.choices) k += e.id + "=" + e.name + "," + e.where + ";";
  return k;
}

void drawCombos(Canvas& cv, const ComboModel& m, std::vector<Button>& btn) {
  btn.clear();
  g_hover = &m.hover;
  g_down = &m.down;
  cairo_t* c = cv.c;
  set(c, INK); cairo_paint(c);
  const double L = 44, R = cv.w - 44, mid = cv.w / 2.0, cw = mid - 12 - L;
  pageHeader(c, btn, cv.w, m.game, m.live, m.confirmClose, DashPage::Combos);

  const double headY = 138, subY = 164, picY = 174, picH = 236, belowY = picY + picH + 10;
  auto pictures = [&] {
    hline(c, L, R, picY, LINE);
    for (int h = 0; h < 2; ++h) bindHand(c, btn, "", "", std::set<std::string>{}, m.yours.pic[h], m.yours.rows[h], h ? mid + 12 : L, cw, picY + 4, picH);
    hline(c, L, R, belowY - 4, LINE);
  };

  if (m.step == 0) {
    text(c, L, headY, 27, m.canMake ? "Your combos for " + m.game : "Start a game to make a combo", true, PAPER, 1, 0, R - L - 410);
    text(c, L, subY, 18, m.canMake ? "Hold two or more controls together to do one thing. Touching counts, and it doesn't press." : m.why, false, DIM, 1, 0, R - L - 410);
    button(c, btn, R - 230, 116, 230, 48, "combo_new", "New combo", Primary, m.canMake);
    button(c, btn, R - 390, 116, 150, 48, "page_recorded", "Advanced", Outline);
    pictures();
    const double rowH = 44;
    if (m.saved.empty()) {
      text(c, L, belowY + 42, 22, "No combos yet.", true, m.canMake ? PAPER : DIM);
      text(c, L, belowY + 76, 19, "Your controllers can feel a thumb or finger resting on a control without pressing it, so a combo of touches", false, DIM, 1, 0, R - L);
      text(c, L, belowY + 102, 19, "won't set off what's already on those buttons. It's how VRChat players fit mute and gestures on a full controller.", false, DIM, 1, 0, R - L);
    }
    for (size_t i = 0; i < m.saved.size() && i < 6; ++i) {
      const double y = belowY + i * rowH;
      const std::string n = std::to_string(i), rowId = "combo_row_" + n, delId = "combo_del_" + n;
      if (m.hover == rowId) slab(c, L, y + 1, R - L - 130, rowH - 2, PAPER, 0.10);
      caps(c, L + 12, y + 29, 17, m.saved[i].inputs, PAPER, 1, 0, 1.2, mid - L - 30);
      text(c, mid, y + 30, 21, m.saved[i].action, true, PAPER, 1, 0, R - 150 - mid);
      btn.push_back({L, y, R - L - 130, rowH, rowId});
      button(c, btn, R - 120, y + 5, 120, rowH - 10, delId, "Remove", Outline);
      hline(c, L, R, y + rowH, LINE, 0.6);
    }
  } else if (m.step == 1) {
    text(c, L, headY, 27, "What do you do?", true, PAPER);
    text(c, L, subY, 18, "Tap a control once for touch, again for press, and again to clear. A stick can be pushed one way too.", false, DIM, 1, 0, R - L - 120);
    caps(c, R, headY, 19, "1 of 2", DIM, 1, 2);
    pictures();
    const double barH = 64;
    // The stick last tapped, under what's picked: touch it, press it, or push it one way. How the ways are named is a switch
    // at the end, the same one as on the Bindings tab.
    double extra = 0;
    if (!m.chips.empty()) {
      const double chipY = belowY + barH + 12, chipH = 40, switchW = 290;
      const double lw = label(c, L, chipY + 26, 16, m.chipsLabel);
      const double x0 = L + lw + 16, x1 = R - switchW - 24;
      std::vector<Segment> list;
      for (const BindModel::Chip& ch : m.chips) list.push_back({ch.id, ch.text, ch.on, true});
      segments(c, btn, x0, chipY, std::min(x1 - x0, 112.0 * list.size()), chipH, list, 16);
      const double sw = label(c, R - switchW, chipY + 26, 16, "Names");
      segments(c, btn, R - switchW + sw + 14, chipY, switchW - sw - 14, chipH, {{"combo_dirs_compass", "N E S W", !m.stickAxes, true}, {"combo_dirs_axes", "Y+ X+", m.stickAxes, true}}, 16);
      extra = chipH + 12;
    }
    if (!m.picked.empty()) {
      slab(c, L, belowY, R - L, barH, SIGNAL);
      caps(c, L + 30, belowY + 41, 23, m.picked, INK, 1, 0, 1.4, R - L - 340);
      button(c, btn, R - 300, belowY + 10, 130, 44, "combo_cancel", "Cancel", OnSignal);
      button(c, btn, R - 160, belowY + 10, 134, 44, "combo_next", "Next", OnSignal);
    } else {
      frame(c, L, belowY, R - L, barH, LINE, 1, 2);
      text(c, L + 30, belowY + 41, 21, "Nothing picked yet", false, DIM);
      button(c, btn, R - 160, belowY + 10, 134, 44, "combo_cancel", "Cancel", Outline);
    }
    if (!m.note.empty()) text(c, L, belowY + barH + extra + 34, 19, m.note, false, PAPER, 1, 0, R - L);
    if (!m.ideas.empty()) {
      const double y = belowY + barH + extra + 58, h = 50, d = h * kLean;
      label(c, L, y + 32, 15, "Or try");
      const double x0 = L + 76, each = (R - x0 + d * (m.ideas.size() - 1) - 8.0 * (m.ideas.size() - 1)) / m.ideas.size();
      for (size_t i = 0; i < m.ideas.size(); ++i) {
        const double x = x0 + i * (each - d + 8);
        button(c, btn, x, y, each, h, m.ideas[i].id, m.ideas[i].name, Outline);
      }
    }
  } else {
    text(c, L, headY, 27, "What should it do?", true, PAPER);
    caps(c, L, subY, 17, m.picked, SIGNAL, 1, 0, 1.2, R - L - 120);
    caps(c, R, headY, 19, "2 of 2", DIM, 1, 2);
    const double listY = 180;
    hline(c, L, R, listY, LINE);
    if (!m.controls.empty()) {
      const double y = listY + 6, w = capsW(c, upper(m.controls), 16, 1.2), prevX = R - 54 - w - 54;
      button(c, btn, R - 44, y, 44, 26, "combo_group_next", "", Outline);
      button(c, btn, prevX, y, 44, 26, "combo_group_prev", "", Outline);
      caps(c, R - 22, y + 19, 18, ">", PAPER, 1, 1, 0);
      caps(c, prevX + 22, y + 19, 18, "<", PAPER, 1, 1, 0);
      caps(c, R - 54, y + 20, 16, m.controls, PAPER, 1, 2, 1.2);
    }
    const double gridY = listY + 38, h = 62, gap = 6, d = h * kLean, each = (R - L + 2 * d - 2 * 8.0) / 3;
    for (size_t i = 0; i < m.choices.size(); ++i) {
      const int col = int(i % 3), row = int(i / 3);
      const double x = L + col * (each - d + 8), y = gridY + row * (h + gap);
      const ComboChoice& e = m.choices[i];
      frame(c, x, y, each, h, PAPER, 0.7, 2);
      fxOver(c, x, y, each, h, e.id);
      const double tx = x + d + 12, tw = each - 2 * d - 22;
      if (e.where.empty()) text(c, tx, y + 39, 21, e.name, true, PAPER, 1, 0, tw);
      else {
        text(c, tx, y + 29, 20, e.name, true, PAPER, 1, 0, tw);
        label(c, tx, y + 50, 13, e.where, DIM);
      }
      btn.push_back({x + d / 2, y, each - d, h, e.id});
    }
    if (m.choices.empty()) text(c, L, gridY + 40, 21, "This game doesn't list anything a combo could set off.", false, DIM);
    const double footY = 640, footH = 48;
    button(c, btn, L, footY, 150, footH, "combo_back", "Back", Outline);
    if (m.pages > 1) {
      const std::string where = "Page " + std::to_string(m.pageNo + 1) + " of " + std::to_string(m.pages);
      const double w = capsW(c, upper(where), 16, 1.2), prevX = R - 54 - w - 54;
      button(c, btn, R - 44, footY + 6, 44, 36, "combo_page_next", "", Outline);
      button(c, btn, prevX, footY + 6, 44, 36, "combo_page_prev", "", Outline);
      caps(c, R - 22, footY + 30, 18, ">", PAPER, 1, 1, 0);
      caps(c, prevX + 22, footY + 30, 18, "<", PAPER, 1, 1, 0);
      caps(c, R - 54, footY + 30, 16, where, PAPER, 1, 2, 1.2);
    }
  }

  hline(c, L, R, 700);
  const double baseY = 734;
  if (!m.status.empty()) {
    lamp(c, L, baseY - 14, 14, m.statusWarn ? ALERT : m.statusGood ? LIVE : SIGNAL);
    text(c, L + 28, baseY, 20, m.status, true, PAPER, 1, 0, R - L - 28);
  } else if (m.step == 2 && !m.note.empty()) {
    text(c, L, baseY, 19, m.note, false, PAPER, 1, 0, R - L);
  } else if (m.canMake) {
    text(c, L, baseY, 19, "A new combo works right away if what it sets off is already on a button. Otherwise, from the next launch.", false, DIM, 1, 0, R - L);
  }
  g_hover = g_down = nullptr;
}

// ---------------------------------------------------------------- macros page
std::string recordedKey(const RecordedModel& m) {
  std::string k = m.game + "|" + std::to_string((int)m.live) + "|" + std::to_string(m.step) + std::to_string(m.canRecord) + "|" + m.why + "|" + std::to_string(m.countdown) + "|" + std::to_string(m.selected) + "|" + m.name + "|" + m.plays + "|" + m.note + "|" + std::to_string(m.onTouch) + std::to_string(m.repeat) + std::to_string(m.asRecorded) + std::to_string(m.withTouches) + std::to_string(m.hasBareTouches) + std::to_string(m.touches) + std::to_string(m.noteWarn) + "|" + m.status + std::to_string(m.statusWarn) + std::to_string(m.statusGood) + std::to_string(m.confirmClose) + "|fx:" + m.hover + "," + m.down + "|" + std::to_string(m.ask) + std::to_string(m.interview) + std::to_string(m.interviewOn) + std::to_string(m.touchesOn) + "|" + m.feeling + "|" + std::to_string(m.tileFirst) + "|" + std::to_string(m.waitShut) + "|";
  for (const RecordedTile& t : m.tiles) k += t.id + t.name + t.hand + std::to_string(t.kind) + std::to_string(t.both) + ";";
  for (const RecordedRow& r : m.saved) {
    k += r.name + ">" + r.plays + ">";
    for (const RecordedTile& t : r.tiles) k += t.name + t.hand + std::to_string(t.kind) + std::to_string(t.both) + ";";
  }
  return k;
}

// A step: the control's name, with an arrow for which way it went, the way G HUB draws its steps. A touch is the same with
// the arrow hollow-looking, in the quieter colour, because nothing got pressed.
static void recordedTile(cairo_t* c, std::vector<Button>& btn, const RecordedTile& t, double x, double y, double w, double h, bool selected) {
  const bool down = t.kind == macro::Press || t.kind == macro::Push || t.kind == macro::Touch;
  const bool touch = t.kind == macro::Touch || t.kind == macro::Untouch;
  if (selected) slab(c, x, y, w, h, SIGNAL);
  else if (t.kind == macro::Press || t.kind == macro::Push) slab(c, x, y, w, h, RAISED);
  frame(c, x, y, w, h, selected ? INK : PAPER, selected ? 1 : touch ? 0.35 : 0.7, 2);
  if (!t.id.empty()) fxOver(c, x, y, w, h, t.id, true, selected);
  const RGB ink = selected ? INK : PAPER;
  const double d = h * kLean, size = std::min(h * 0.26, 17.0);
  if (!t.hand.empty()) label(c, x + d + 5, y + size * 0.95, size * 0.72, t.hand, selected ? INK : DIM);
  const double nameY = y + h * 0.56;
  caps(c, x + w / 2, nameY, size, t.name, ink, 1, 1, 0.6, w - d - 12);
  const double ay = y + h * 0.78, as = h * 0.085;
  if (t.kind == macro::Push) {
    const Glyph g = t.x > 0 ? Glyph::Right : t.x < 0 ? Glyph::Left : t.y > 0 ? Glyph::Up : Glyph::Down;
    glyph(c, x + w / 2 - d * 0.1, ay, as, g, selected ? INK : SIGNAL);
  } else if (t.kind == macro::Centre) {
    label(c, x + w / 2, ay + as, size * 0.7, "Back", selected ? INK : DIM, 1);
  } else if (t.both) {
    // One press: down and straight back up, with nothing between them.
    const double apart = as * 1.3;
    glyph(c, x + w / 2 - d * 0.1 - apart, ay, as, Glyph::Down, selected ? INK : touch ? DIM : SIGNAL);
    glyph(c, x + w / 2 - d * 0.1 + apart, ay, as, Glyph::Up, selected ? INK : touch ? DIM : PAPER);
    if (touch) label(c, x + w - d - 5, y + size * 0.95, size * 0.62, "Touch", selected ? INK : DIM, 2);
  } else {
    glyph(c, x + w / 2 - d * 0.1, ay, as, down ? Glyph::Down : Glyph::Up, selected ? INK : touch ? DIM : down ? SIGNAL : PAPER);
    if (touch) label(c, x + w - d - 5, y + size * 0.95, size * 0.62, "Touch", selected ? INK : DIM, 2);
  }
  if (!t.id.empty()) btn.push_back({x, y, w, h, t.id});
}

// The whole sequence, wrapping if it has to, or as much of it as was asked for. Returns where it ended.
static double recordedStrip(cairo_t* c, std::vector<Button>& btn, const std::vector<RecordedTile>& tiles, int selected, double x0, double x1, double y, double w, double h, double gapY, int first = 0, int count = 1000) {
  const double d = h * kLean, step = w - d + 6;
  double x = x0;
  const size_t end = std::min(tiles.size(), size_t(first) + size_t(count));
  for (size_t i = size_t(first); i < end; ++i) {
    if (x + w > x1) { x = x0; y += h + gapY; }
    recordedTile(c, btn, tiles[i], x, y, w, h, int(i) == selected);
    x += step;
  }
  return y + h;
}

// A setting's name, and in a word or two what it does to the steps, on the line above it.
static void recordedCaption(cairo_t* c, double x, double y, double w, const std::string& name, const std::string& what) {
  const double lw = label(c, x, y + 16, 15, name);
  if (!what.empty()) text(c, x + lw + 12, y + 16, 15, what, false, DIM, 1, 0, w - lw - 12);
}

// What it can feel right now, for the interview's listening pages.
static void recordedFeeling(cairo_t* c, const RecordedModel& m, double L, double R) {
  if (!m.interview) return;
  const double lw = label(c, L, 672, 15, "Feeling now");
  text(c, L + lw + 14, 673, 20, m.feeling.empty() ? "Nothing" : m.feeling, !m.feeling.empty(), m.feeling.empty() ? DIM : PAPER, 1, 0, R - L - lw - 14);
}

void drawRecorded(Canvas& cv, const RecordedModel& m, std::vector<Button>& btn) {
  btn.clear();
  g_hover = &m.hover;
  g_down = &m.down;
  cairo_t* c = cv.c;
  set(c, INK); cairo_paint(c);
  const double L = 44, R = cv.w - 44, mid = cv.w / 2.0;
  pageHeader(c, btn, cv.w, m.game, m.live, m.confirmClose, DashPage::Recorded);
  const double headY = 138, subY = 164;

  if (m.step == 0) {
    text(c, L, headY, 27, m.canRecord ? "Macros for " + m.game : "Macros", true, PAPER, 1, 0, R - L - 410);
    text(c, L, subY, 18, m.canRecord ? "One button plays a whole sequence you recorded, like a mouse macro. No game restart." : m.why, false, DIM, 1, 0, R - L - 410);
    button(c, btn, R - 230, 116, 230, 48, "macro_record", "Record new", Primary, m.canRecord);
    button(c, btn, R - 390, 116, 150, 48, "macro_back", "Back", Outline);
    const double top = 176, rowH = 88;
    hline(c, L, R, top, LINE);
    if (m.saved.empty()) {
      text(c, L, top + 56, 23, "Nothing recorded yet.", true, m.canRecord ? PAPER : DIM);
      const char* how[3] = {"Tap Record new and do what you'd like one button to do for you. Hold A, pull the trigger, let go of both.",
                            "The steps show up as you go, and you can move them around or delete them afterwards.",
                            "Then press the button that should play them. That's it. It works as soon as you close the dashboard."};
      for (int i = 0; i < 3; ++i) text(c, L, top + 96 + i * 30, 20, how[i], false, DIM, 1, 0, R - L);
    }
    for (size_t i = 0; i < m.saved.size() && i < 5; ++i) {
      const double y = top + i * rowH;
      const std::string n = std::to_string(i);
      text(c, L + 8, y + 34, 23, m.saved[i].name, true, PAPER, 1, 0, 340);
      label(c, L + 8, y + 62, 15, m.saved[i].plays, m.saved[i].plays == "No button yet" ? ALERT : DIM);
      std::vector<Button> none;
      // Two rows of eight is what fits beside the name. Anything more is on the Edit page.
      const int fits = 16, count = int(m.saved[i].tiles.size());
      const double end = recordedStrip(c, none, m.saved[i].tiles, -1, L + 360, R - 240, y + 3, 64, 40, 3, 0, count > fits ? fits - 1 : fits);
      if (count > fits) text(c, R - 276, end - 12, 17, "+" + std::to_string(count - fits + 1), true, DIM);
      button(c, btn, R - 230, y + 19, 110, 50, "macro_edit_" + n, "Edit", Outline);
      button(c, btn, R - 112, y + 19, 112, 50, "macro_del_" + n, "Remove", Outline);
      hline(c, L, R, y + rowH, LINE, 0.6);
    }
    const double optY = 622, optW = mid - 12 - L, rx = mid + 12, segW = 200;
    recordedCaption(c, L, optY, optW, "Interview", "Easy mode: it asks you one question at a time.");
    segments(c, btn, L, optY + 24, segW, 42, {{"macro_interview_off", "Off", !m.interviewOn, true}, {"macro_interview_on", "On", m.interviewOn, true}});
    recordedCaption(c, rx, optY, R - rx, "Capacitive touches", "A resting thumb counts, as well as a press.");
    segments(c, btn, rx, optY + 24, segW, 42, {{"macro_touch_off", "Off", !m.touchesOn, true}, {"macro_touch_on", "On", m.touchesOn, true}});
  } else if (m.waitShut && (m.step == 1 || m.step == 4)) {
    // This part happens with the dashboard shut. Steam Link keeps every button for the dashboard while it's open, and it's
    // easier in the game anyway.
    slab(c, L, 112, R - L, 150, SIGNAL);
    text(c, L + 48, 174, 36, "Now close the dashboard", true, INK, 1, 0, R - L - 80);
    text(c, L + 34, 222, 21, m.step == 1 ? "It records with the dashboard shut. A sign in front of you counts down, then do your sequence."
                                         : "It listens with the dashboard shut. Then press what should play this.",
         false, INK, 1, 0, R - L - 80);
    const char* how[3] = {"Shut it the way you usually do.",
                          m.step == 1 ? "It stops 2 seconds after your last press, and this page opens again by itself." : "As soon as you let go of it, this page opens again by itself.",
                          m.step == 1 ? "Open the dashboard while it records and it stops there, keeping what you did." : "Open the dashboard before then and it stops without choosing."};
    for (int i = 0; i < 3; ++i) text(c, L, 320 + i * 34, 21, how[i], false, DIM, 1, 0, R - L);
    // Back to where you were: the steps you already have stay, which Cancel on the steps page wouldn't do.
    button(c, btn, L, 640, 150, 48, "macro_leave", m.step == 4 ? "Back" : "Cancel", Outline);
  } else if (m.step == 1) {
    slab(c, L, 112, R - L, 150, SIGNAL);
    text(c, L + 48, 174, 36, "Let go of everything", true, INK, 1, 0, R - L - 260);
    text(c, L + 34, 222, 21, "Your buttons stop reaching SteamVR while it records, so don't worry about the laser.", false, INK, 1, 0, R - L - 260);
    caps(c, R - 120, 214, 76, std::to_string(std::max(1, m.countdown)), INK, 1, 1, 0);
    button(c, btn, L, 640, 150, 48, "macro_cancel", "Cancel", Outline);
  } else if (m.step == 2) {
    slab(c, L, 112, R - L, 110, RAISED);
    lamp(c, L + 40, 142, 26, ALERT);
    text(c, L + 84, 164, 34, "Recording. Do your sequence.", true, PAPER, 1, 0, R - L - 120);
    text(c, L + 60, 202, 20, "It stops by itself 2 seconds after your last press.", false, DIM, 1, 0, R - L - 120);
    if (m.tiles.empty()) text(c, L, 290, 22, "Nothing yet.", false, DIM);
    // The newest ones, if there are more than three rows of them.
    const int fits = 39, count = int(m.tiles.size());
    recordedStrip(c, btn, m.tiles, -1, L, R, 244, 96, 96, 10, count > fits ? count - fits : 0, fits);
    recordedFeeling(c, m, L, R);
  } else if (m.step == 3) {
    text(c, L, headY, 27, "Check the steps", true, PAPER);
    text(c, L, subY, 18, "It plays them left to right. Tap one to move or delete it.", false, DIM, 1, 0, R - L - 420);
    const int count = int(m.tiles.size());
    if (count > kRecordedTilesPerPage) {
      const int last = std::min(count, m.tileFirst + kRecordedTilesPerPage);
      label(c, R - 230, 150, 15, std::to_string(m.tileFirst + 1) + " to " + std::to_string(last) + " of " + std::to_string(count), DIM, 2);
      button(c, btn, R - 220, 122, 104, 44, "macro_steps_prev", "Previous", Outline, m.tileFirst > 0);
      button(c, btn, R - 104, 122, 104, 44, "macro_steps_next", "More", Outline, last < count);
    }
    // Everything below stays where it is, however many steps there are, so nothing can land on anything else.
    const double end = recordedStrip(c, btn, m.tiles, m.selected, L, R, 176, 96, 72, 6, m.tileFirst, kRecordedTilesPerPage);
    if (m.selected >= 0) {
      const double ctlY = end + 6, ctlH = 40;
      button(c, btn, L, ctlY, 170, ctlH, "macro_left", "Earlier", Outline, m.selected > 0);
      button(c, btn, L + 180, ctlY, 170, ctlH, "macro_right", "Later", Outline, m.selected + 1 < count);
      button(c, btn, L + 360, ctlY, 170, ctlH, "macro_remove_step", "Delete", Outline);
    }
    const double gridY = 378, rowH = 82, ctlH = 44, colW = mid - 12 - L, rx = mid + 12, row1 = gridY + 6, row2 = row1 + rowH, row3 = row2 + rowH;
    hline(c, L, R, gridY, LINE);
    recordedCaption(c, L, row1, colW, "Button", m.onTouch ? "Resting on this plays the macro." : "Pressing this plays the macro.");
    if (m.plays.empty()) button(c, btn, L, row1 + 24, colW, ctlH, "macro_pick", "Choose a button", Primary);
    else button(c, btn, L, row1 + 24, colW, ctlH, "macro_pick", m.plays, Solid);
    recordedCaption(c, rx, row1, R - rx, "When pressed", "Play it once, or over and over while it's held.");
    segments(c, btn, rx, row1 + 24, R - rx, ctlH, {{"macro_once", "Once", !m.repeat, true}, {"macro_repeat", "While held", m.repeat, true}});
    recordedCaption(c, L, row2, colW, "Name", "");
    text(c, L + 8, row2 + 24 + 30, 21, m.name, true, PAPER, 1, 0, colW - 160);
    button(c, btn, L + colW - 140, row2 + 24, 140, ctlH, "macro_rename", "Rename", Outline);
    recordedCaption(c, rx, row2, R - rx, "Timing", "The gaps you left, or the same short gap each time.");
    segments(c, btn, rx, row2 + 24, R - rx, ctlH, {{"macro_mine", "As I did it", m.asRecorded, true}, {"macro_quick", "Quick", !m.asRecorded, true}});
    if (m.touches) {
      // Always there with Capacitive touches on. Only a take with touches and presses has anything to choose.
      const bool choice = m.touches == 2, kept = m.touches == 3 || (choice && m.withTouches);
      recordedCaption(c, rx, row3, R - rx, "Capacitive touches",
                      choice ? "Where your thumb rested without pressing." : m.touches == 3 ? "It's only touches, so they stay in." : "None this time, only presses.");
      segments(c, btn, rx, row3 + 24, R - rx, ctlH, {{"macro_press_only", "Leave out", !kept, choice}, {"macro_touches", "Keep", kept, choice}});
    }
    button(c, btn, L, 640, 140, 48, "macro_cancel", "Cancel", Outline);
    button(c, btn, L + 150, 640, 190, 48, "macro_again", "Record again", Outline);
    button(c, btn, R - 230, 640, 230, 48, "macro_save", "Save", Primary, !m.plays.empty());
  } else if (m.step == 4) {
    slab(c, L, 112, R - L, 110, SIGNAL);
    text(c, L + 44, 164, 34, m.touchesOn ? "Press or touch what should play this" : "Press what should play this", true, INK, 1, 0, R - L - 80);
    text(c, L + 30, 202, 20, m.touchesOn ? "Any button, on either controller. Touch it without pressing if you'd like a thumb resting there to set it off." : "Any button, on either controller.", false, INK, 1, 0, R - L - 80);
    recordedStrip(c, btn, m.tiles, -1, L, R, 244, 96, 86, 8, 0, 26);
    recordedFeeling(c, m, L, R);
  } else {
    // The interview: one question at a time, with nothing counted until you say so.
    const double bigY = 250, bigH = 96, colW = mid - 12 - L, rx = mid + 12;
    label(c, R, headY - 4, 16, "Question " + std::to_string(m.ask + 1) + " of " + std::to_string(kRecordedQuestions), DIM, 2);
    button(c, btn, L, 640, 140, 48, "macro_cancel", "Cancel", Outline);
    if (m.ask > 0) button(c, btn, L + 150, 640, 140, 48, "macro_ask_back", "Back", Outline);
    if (m.ask == 0) {
      text(c, L, headY, 27, "What should one button do for you?", true, PAPER, 1, 0, R - L - 200);
      text(c, L, subY, 18, "I'll ask a few short questions, and nothing counts until you say so.", false, DIM, 1, 0, R - L);
      text(c, L, 260, 23, "Press Start and wait for the countdown.", false, PAPER);
      text(c, L, 300, 23, "Then do what you'd like that one button to do, the way you'd do it in the game.", false, PAPER, 1, 0, R - L);
      text(c, L, 340, 23, m.touchesOn ? "Holding the controller doesn't count. Only your presses and capacitive touches do." : "Holding the controller doesn't count. Only your presses do.", false, PAPER, 1, 0, R - L);
      text(c, L, 380, 19, m.touchesOn ? "(Capacitive touches are on. You can switch them off at the bottom of the Macros page.)"
                                      : "(Capacitive touches can count too. Switch them on at the bottom of the Macros page first.)", false, DIM, 1, 0, R - L);
      button(c, btn, R - 230, 640, 230, 48, "macro_ask_start", "Start", Primary);
    } else if (m.ask == 1) {
      text(c, L, headY, 27, "Is this what you did?", true, PAPER, 1, 0, R - L - 200);
      text(c, L, subY, 18, "One button will play these, left to right.", false, DIM, 1, 0, R - L);
      recordedStrip(c, btn, m.tiles, -1, L, R, 190, 96, 86, 8, 0, 39);
      button(c, btn, L + 300, 640, 190, 48, "macro_again", "Try again", Outline);
      button(c, btn, R - 280, 640, 280, 48, "macro_ask_next", "Yes, that's it", Primary);
    } else if (m.ask == 2) {
      text(c, L, headY, 27, "Which button should play it?", true, PAPER, 1, 0, R - L - 200);
      text(c, L, subY, 18, m.touchesOn ? "Press it, or touch it without pressing if a resting thumb should set it off." : "Press the one you want, on either controller.", false, DIM, 1, 0, R - L);
      if (m.plays.empty()) button(c, btn, R - 280, 640, 280, 48, "macro_pick", "Choose a button", Primary);
      else {
        text(c, L, 300, 44, m.plays, true, PAPER, 1, 0, R - L);
        button(c, btn, L + 300, 640, 230, 48, "macro_pick", "Choose another", Outline);
        button(c, btn, R - 280, 640, 280, 48, "macro_ask_next", "Yes, use " + m.plays, Primary);
      }
    } else if (m.ask == 3) {
      text(c, L, headY, 27, "What should happen if you hold it down?", true, PAPER, 1, 0, R - L - 200);
      text(c, L, subY, 18, "Tap the one you'd like.", false, DIM, 1, 0, R - L);
      button(c, btn, L, bigY, colW, bigH, "macro_once", "Play it once", Outline);
      text(c, L + 12, bigY + bigH + 34, 19, "Each press plays it one time.", false, DIM, 1, 0, colW - 12);
      button(c, btn, rx, bigY, R - rx, bigH, "macro_repeat", "Keep repeating", Outline);
      text(c, rx + 12, bigY + bigH + 34, 19, "It goes round again until you let go.", false, DIM, 1, 0, R - rx - 12);
    } else if (m.ask == 4) {
      text(c, L, headY, 27, "How fast should it play?", true, PAPER, 1, 0, R - L - 200);
      text(c, L, subY, 18, "Tap the one you'd like.", false, DIM, 1, 0, R - L);
      button(c, btn, L, bigY, colW, bigH, "macro_mine", "The way I did it", Outline);
      text(c, L + 12, bigY + bigH + 34, 19, "With the same gaps you left between presses.", false, DIM, 1, 0, colW - 12);
      button(c, btn, rx, bigY, R - rx, bigH, "macro_quick", "Quick", Outline);
      text(c, rx + 12, bigY + bigH + 34, 19, "A very short gap every time. Some games miss it.", false, DIM, 1, 0, R - rx - 12);
    } else {
      text(c, L, headY, 27, "What should it be called?", true, PAPER, 1, 0, R - L - 200);
      text(c, L, subY, 18, "That's everything. Save it and it works as soon as you close the dashboard.", false, DIM, 1, 0, R - L);
      text(c, L, 250, 34, m.name, true, PAPER, 1, 0, R - L - 180);
      button(c, btn, R - 160, 216, 160, 48, "macro_rename", "Rename", Outline);
      recordedStrip(c, btn, m.tiles, -1, L, R, 290, 96, 86, 8, 0, 26);
      button(c, btn, R - 230, 640, 230, 48, "macro_save", "Save", Primary, !m.plays.empty());
    }
  }

  hline(c, L, R, 700);
  const double baseY = 734;
  if (!m.status.empty()) {
    lamp(c, L, baseY - 14, 14, m.statusWarn ? ALERT : m.statusGood ? LIVE : SIGNAL);
    text(c, L + 28, baseY, 20, m.status, true, PAPER, 1, 0, R - L - 28);
  } else if ((m.step == 3 || (m.step == 5 && m.ask == 2)) && !m.note.empty()) {
    if (m.noteWarn) {
      lamp(c, L, baseY - 14, 14, ALERT);
      text(c, L + 28, baseY, 19, m.note, true, ALERT, 1, 0, R - L - 28);
    } else text(c, L, baseY, 19, m.note, false, PAPER, 1, 0, R - L);
  } else if (m.step == 0 && m.canRecord) {
    text(c, L, baseY, 19, "Close the dashboard and press the button. Macros don't run while the dashboard's open.", false, DIM, 1, 0, R - L);
  }
  g_hover = g_down = nullptr;
}

// ---------------------------------------------------------------- in-headset panel
std::string panelKey(const PanelModel& m) {
  std::string k = m.header + "|" + m.progress + "|" + m.chip + "|" + m.text + "|" + m.status + std::to_string(m.warn) + std::to_string(m.match);
  for (auto& kv : m.keys) k += "|" + kv.first + "=" + kv.second;
  return k;
}

// The leaning plate the in-headset panels sit on. Its left and right edges slope, so lines of text start further
// in the higher up they are: panelLeft and panelRight give the margins at a height.
static void panelPlate(Canvas& cv, RGB acc) {
  cairo_t* c = cv.c;
  cv.clear();
  slab(c, 0, 0, cv.w, cv.h, INK, 0.95);
  frame(c, 0, 0, cv.w, cv.h, LINE, 1, 2);
  cairo_save(c);
  leanPath(c, 0, 0, cv.w, cv.h);
  cairo_clip(c);
  box(c, 0, 0, cv.w, 8, acc);
  cairo_restore(c);
}
static double panelLeft(const Canvas& cv, double y) { return 30 + (cv.h - y) * kLean; }
static double panelRight(const Canvas& cv, double y) { return cv.w - 30 - y * kLean; }

void drawPanel(Canvas& cv, const PanelModel& m) {
  cairo_t* c = cv.c;
  RGB acc = m.warn ? ALERT : m.match ? PAPER : SIGNAL;
  panelPlate(cv, acc);

  caps(c, panelLeft(cv, 54), 54, 21, m.header, m.warn ? ALERT : PAPER, 1, 0, 1.8);
  if (!m.progress.empty()) caps(c, panelRight(cv, 54), 54, 21, m.progress, DIM, 1, 2, 1.8);

  // the one instruction, with the controller input as a key on the left
  double row = 126, L = panelLeft(cv, row - 12), R = panelRight(cv, row - 12);
  double chipH = 48, chipD = chipH * kLean, chipW = 0, gap = 16, tsize = 34;
  if (!m.chip.empty()) chipW = capsW(c, upper(m.chip), 22, 1.6) + 32 + chipD;
  double maxText = R - L - (chipW > 0 ? chipW + gap : 0);
  while (textW(c, m.text, tsize, true) > maxText && tsize > 18) tsize -= 1;
  double x0 = L;
  if (chipW > 0) {
    slab(c, x0, row - 36, chipW, chipH, acc);
    caps(c, x0 + chipW / 2, row - 4, 22, m.chip, INK, 1, 1, 1.6);
    x0 += chipW + gap;
  }
  text(c, x0, row, tsize, m.text, true, PAPER);

  double sy = 184, sl = panelLeft(cv, sy - 8);
  if (!m.status.empty()) text(c, sl, sy, 23, m.status, m.warn, m.warn ? ALERT : PAPER, m.warn ? 1 : 0.8, 0, panelRight(cv, sy - 8) - sl);

  // what the other buttons do, along the bottom
  double ry = cv.h - 72;
  hline(c, ry < 0 ? 0 : (cv.h - ry) * kLean + 2, cv.w - ry * kLean - 2, ry);
  double fy = cv.h - 54, fx = panelLeft(cv, fy + 17);
  for (const auto& kv : m.keys) {
    double kh = 34, kw = capsW(c, upper(kv.first), 17, 1.2) + 20 + kh * kLean;
    frame(c, fx, fy, kw, kh, PAPER, 0.6, 2);
    caps(c, fx + kw / 2, fy + 23, 17, kv.first, PAPER, 1, 1, 1.2);
    double lw = text(c, fx + kw + 8, fy + 23, 18, kv.second, false, DIM);
    fx += kw + 8 + lw + 26;
  }
}

void drawMessage(Canvas& cv, const std::string& title, const std::string& sub, bool good) {
  cairo_t* c = cv.c;
  panelPlate(cv, good ? LIVE : DIM);
  const double y = cv.h / 2.0, L = panelLeft(cv, y), R = panelRight(cv, y);
  double tx = L;
  if (good) {
    slab(c, L, y - 58, 66, 54, LIVE);
    check(c, L + 21, y - 45, 26, INK);
    tx = L + 84;
  }
  text(c, tx, y - 16, 34, title, true, PAPER, 1, 0, R - tx);
  text(c, tx, y + 34, 22, sub, false, DIM, 1, 0, R - tx);
}

void drawIcon(Canvas& cv) {
  cv.clear();
  markTile(cv.c, 0, 0, cv.w, cv.h);
}

}  // namespace fh

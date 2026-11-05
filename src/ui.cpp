#include "ui.h"

#include "glove.h"
#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

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

// Bahnschrift is Windows' own DIN-style face, on every PC since Windows 10 1709. It's a variable font, and through
// cairo only its regular and bold load: its other named weights silently come out as Arial.
enum Face { Body, Semi };
void face(cairo_t* c, Face f, double size) {
  static const bool din = [] {
    const char* w = std::getenv("WINDIR");
    return w && std::ifstream(std::string(w) + "\\Fonts\\bahnschrift.ttf").good();
  }();
  cairo_select_font_face(c, din ? "Bahnschrift" : "Segoe UI", CAIRO_FONT_SLANT_NORMAL, f == Body ? CAIRO_FONT_WEIGHT_NORMAL : CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(c, size);
}
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

const DashModel* g_m = nullptr;  // the dashboard being drawn, for click feedback

// Static press states over any tappable shape: outlined under the laser, darker while held.
// onFill: the shape is filled with a bright colour, so the marks have to be dark to show.
void fxOver(cairo_t* c, double x, double y, double w, double h, const std::string& id, bool enabled = true, bool onFill = false) {
  if (!g_m) return;
  bool hov = g_m->hover == id, dn = g_m->down == id && enabled;
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
  bool dn = g_m && g_m->down == id && enabled;
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
  k += "|fx:" + m.hover + "," + m.down + "|" + std::to_string(m.statusGood);
  return k;
}

void drawDashboard(Canvas& cv, const DashModel& m, std::vector<Button>& btn) {
  btn.clear();
  g_m = &m;
  cairo_t* c = cv.c;
  set(c, INK); cairo_paint(c);
  const double L = 44, R = cv.w - 44, mid = cv.w / 2.0;

  // header: the mark and name on the left; what's running and how it's connected on the right
  markTile(c, L, 20, 80, 64);
  caps(c, L + 94, 58, 42, "UTMTFTM", PAPER, 1, 0, 3.5);
  label(c, L + 95, 82, 15, "Use the medium to fix the medium");
  if (m.confirmClose) button(c, btn, R - 214, 30, 214, 46, "close", "Tap again to close", Danger);
  else button(c, btn, R - 110, 30, 110, 46, "close", "Close", Outline);
  double right = R - (m.confirmClose ? 234 : 130);
  text(c, right, 50, 21, m.game.empty() ? "No game running" : m.game, false, m.game.empty() ? DIM : PAPER, 1, 2, 480);
  if (m.live == LiveState::Live || m.live == LiveState::NextLaunch) {
    bool on = m.live == LiveState::Live;
    double w = caps(c, right, 77, 15, on ? "Live in game" : "Applies next launch", PAPER, 1, 2);
    lamp(c, right - w - 24, 65, 12, on ? LIVE : SIGNAL);
  }
  hline(c, L, R, 98, PAPER, 0.9, 2);

  // the next thing to do. Waiting on the dashboard beats everything else, live games explain the bubble,
  // and only a running game that can't move live offers the ghost match.
  const double headY = 138, subY = 164;
  if (m.armed) {
    slab(c, L, 108, R - L, 60, SIGNAL);
    text(c, L + 28, 134, 25, "Now close the dashboard", true, INK);
    text(c, L + 22, 158, 18, "You can start grabbing as soon as it's shut.", false, INK, 1, 0, R - L - 220);
    button(c, btn, R - 158, 116, 134, 44, "cancel_armed", "Cancel", OnSignal);
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
    const char* lab[6] = {"Forward", "Up", "Right", "Pitch", "Yaw", "Roll"};
    double val[6] = {-o.pos[2] * 100, o.pos[1] * 100, o.pos[0] * 100, o.rot[0], o.rot[1], o.rot[2]};
    const double tx0 = x + 268, tx1 = x + cw, rowH = 41, top = 228;
    for (int i = 0; i < 6; ++i) {
      double ry = top + i * rowH, base = ry + 29;
      bool cm = i < 3, none = std::fabs(val[i]) < (cm ? 0.05 : 0.5);
      label(c, tx0, base, 16, lab[i]);
      const char* unit = cm ? "cm" : "\xC2\xB0";
      double uw = cm ? capsW(c, "CM", 14, 1, Body) : 12;
      if (cm) caps(c, tx1, base, 14, "cm", none ? FAINT : DIM, 1, 2, 1, 0, Body);
      else text(c, tx1, base, 25, unit, true, none ? FAINT : PAPER, 1, 2);
      std::string v = none ? (cm ? "0.0" : "0") : fmt(cm ? "%+.1f" : "%+.0f", val[i]);
      size_t minus = v.find('-');
      if (minus != std::string::npos) v.replace(minus, 1, "\xE2\x80\x93");  // an en dash: this font has no true minus sign
      text(c, tx1 - uw - (cm ? 6 : 1), base, 25, v, true, none ? FAINT : PAPER, 1, 2);
      if (i < 5) hline(c, tx0, tx1, ry + rowH, i == 2 ? DIM : LINE, 0.8);
    }
    double bty = 490, bth = 54, rw = 150;
    button(c, btn, x, bty, cw - rw - 10, bth, h ? "place_right" : "place_left", "Move", Primary);
    button(c, btn, x + cw - rw, bty, rw, bth, h ? "reset_right" : "reset_left", "Reset", Outline, !o.zero());
  }
  hline(c, L, R, 558);

  // actions for both hands, and where they're saved
  double ay = 572, ah = 54, sx = mid + 12;
  button(c, btn, L, ay, cw, ah, "place_both", "Move both hands", Outline);
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
  } else {
    if (m.armed || m.live == LiveState::NoGame) {}  // the line above the hands already says it
    else if (m.live == LiveState::Live) st = "Changes show on your in-game hands as you move them.";
    else st = "This game takes new offsets the next time it launches.";
    if (!st.empty()) text(c, L, baseY, 19, st, false, DIM, 1, 0, R - L);
  }
  g_m = nullptr;
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

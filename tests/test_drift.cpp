// Stick drift, measured on made-up worn sticks: the stick check run on them the way the page runs it, then what a game gets
// with and without the fix, for each thing the page promises.
//
// The worn stick is built the way a worn potentiometer stick behaves: it rests off the middle; its spring is weak, so it
// lands short on the side it came back from, a different spot each time; it jitters a little, with the odd spike; and it no
// longer gets all the way out to one side. A thumb pushing it means a position; letting go sends it back to a landing spot.
// J4's Quest 3 sticks (18 November) drift another way, and the stick can do that too: it rests in the middle, but at
// particular spots on its travel a report now and then jumps to another point, about the same one each time, and straight
// back (spots). Virtual Desktop sends a stick's x and y as two reports, every 14 ms (J4's live.shm, 16 November): pairs.
//
// test_drift [folder]  also draws where the stick came to rest after 200 let-gos, as reported and as the game gets them, as
// drift_landings.png in that folder.
#include <cairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <random>
#include <string>
#include <vector>

#include "../src/drift.h"
#include "../src/live.h"
#include "../src/sticks.h"
#include "../src/ui.h"

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}
std::string f2(const char* fmt, double a, double b = 0) {
  char s[200];
  std::snprintf(s, sizeof s, fmt, a, b);
  return s;
}

constexpr float kPi = drift::kPi;

struct WornStick {
  float restX = -0.08f, restY = 0.11f;  // where it rests on average: up and to the left, 14% out
  float weak = 0.05f;                   // lands up to this far short on the side it came back from
  float jitter = 0.012f;                // and wobbles this much there
  float spikes = 0.005f;                // and one report in this many jumps out 15%
  float leftMost = 0.84f;               // and only gets 84% of the way to the left
  bool touchWorks = true;
  // Flicks: inside a spot (within halfWidth of angle, at least minR out), each report has this chance of jumping to (toX, toY)
  // for that many reports.
  struct Spot { float angle, halfWidth, minR, toX, toY, chance; int reports; };
  std::vector<Spot> spots;
  int flickLeft = 0, comeBack = 0;  // a flick goes straight back: the report after it is where the stick is
  float flickX = 0, flickY = 0;
  bool flicking = false;  // the last report was a flick
  int flicked = 0;        // reports that were
  int flicks = 0;         // and flicks (one flick can be a few reports)
  std::mt19937 rng{11};
  float px = 0, py = 0;                 // where it is, before the noise
  float lastAngle = 0;
  bool thumb = false;

  float noise() { return std::normal_distribution<float>(0.f, jitter / 2.5f)(rng); }
  // The thumb holds it pushed this far each way, as a fraction of all the way: measured from where it rests, since that's
  // where the thumb starts from. Its edge is still round the true middle, so pushing past it stops there.
  void push(float x, float y) {
    thumb = true;
    const float r = std::sqrt(x * x + y * y);
    if (r > 0.2f) lastAngle = std::atan2(y, x);
    px = restX + x;
    py = restY + y;
    const float e = std::sqrt(px * px + py * py);
    if (e > 1) { px /= e; py /= e; }
  }
  void letGo() {
    thumb = false;
    const float s = std::uniform_real_distribution<float>(0.3f, 1.f)(rng);
    px = restX + std::cos(lastAngle) * weak * s;
    py = restY + std::sin(lastAngle) * weak * s;
  }
  // What the controller reports.
  void report(float& x, float& y, int& touch) {
    x = px + noise();
    y = py + noise();
    flicking = false;
    if (flickLeft > 0) {
      --flickLeft;
      flicking = true;
      if (!flickLeft) comeBack = 1;
    } else if (comeBack > 0) {
      --comeBack;
    } else {
      const float r = std::sqrt(px * px + py * py), a = std::atan2(py, px);
      for (const Spot& sp : spots) {
        if (r < sp.minR || std::fabs(std::remainder(a - sp.angle, 2 * kPi)) > sp.halfWidth) continue;
        if (std::uniform_real_distribution<float>(0, 1)(rng) >= sp.chance) continue;
        flickLeft = sp.reports - 1;
        comeBack = flickLeft ? 0 : 1;
        flickX = sp.toX;
        flickY = sp.toY;
        flicking = true;
        ++flicks;
        break;
      }
    }
    if (flicking) {
      x = flickX + noise();
      y = flickY + noise();
      ++flicked;
    }
    if (std::uniform_real_distribution<float>(0, 1)(rng) < spikes) x += 0.15f;  // the odd spike from a worn track
    if (x < -leftMost) x = -leftMost;
    const float r = std::sqrt(x * x + y * y);
    if (r > 1) { x /= r; y /= r; }
    touch = touchWorks ? (thumb ? 1 : 0) : 0;
  }
};

// Runs the three steps of the stick check on it, the way a person would, and works out what to send.
struct Run { std::vector<drift::Sample> rest, edge, flicks; };
struct How {
  float (*roll)(float angle) = nullptr;  // how far out the thumb gets each way while rolling round the edge. Null: all the way
  int laps = 3;                          // round the edge this many times
  double dt = 1.0 / 250;                 // reports this far apart
  bool pairs = false;                    // x and y come as two reports, the way Virtual Desktop sends them
  const float* prevReach = nullptr;      // what the last check of this controller saw each way
};
drift::Findings checkStick(WornStick& s, bool hasTouch, Run* keep = nullptr, How how = {}) {
  Run run;
  double t = 0;
  const double dt = how.dt;
  float lastY = 0;
  auto take = [&](std::vector<drift::Sample>& into) {
    float x, y;
    int touch;
    s.report(x, y, touch);
    if (how.pairs) into.push_back({x, lastY, hasTouch ? touch : -1, t});  // x first, with the y it said last time
    into.push_back({x, y, hasTouch ? touch : -1, t});
    lastY = y;
    t += dt;
  };
  // 1: left alone for four seconds.
  s.letGo();
  for (int i = 0; i < 1000; ++i) take(run.rest);
  const drift::Rest rest = drift::restOf(run.rest);
  // 2: round the edge, three times, slowly.
  for (int i = 0; i < how.laps * 400; ++i) {
    const float a = 2 * kPi * i / 400.f, m = how.roll ? how.roll(a) : 1.f;
    s.push(std::cos(a) * m, std::sin(a) * m);
    take(run.edge);
  }
  s.letGo();
  const std::vector<float> edge = drift::edgeOf(run.edge, rest.x, rest.y);
  // 3: flick and let go, up, right, down and left, twice each.
  for (int k = 0; k < 8; ++k) {
    const float a = kPi / 2 * (k % 4);
    for (int i = 0; i < 40; ++i) { s.push(std::cos(a) * i / 40.f, std::sin(a) * i / 40.f); take(run.flicks); }
    for (int i = 0; i < 20; ++i) { s.push(std::cos(a), std::sin(a)); take(run.flicks); }
    s.letGo();
    for (int i = 0; i < 120; ++i) take(run.flicks);
  }
  const std::vector<drift::Landing> landed = drift::landingsOf(run.flicks, rest.x, rest.y);
  check(rest.ok && !rest.touched, "step 1 sees it resting, with no thumb on it");
  check(drift::edgeDone(edge) >= 0.95f, f2("step 2 sees the whole edge: %.0f%% of it", drift::edgeDone(edge) * 100));
  check(landed.size() == 8 && drift::landingsDone(landed) == 1.f, f2("step 3 sees all 8 let-gos, from all four ways: %.0f found", double(landed.size())));
  if (keep) *keep = run;
  drift::CheckData d;
  d.rest = rest;
  d.edge = run.edge;
  d.flick = run.flicks;
  d.landed = landed;
  d.hasTouch = hasTouch;
  if (how.prevReach)
    for (int k = 0; k < drift::kDirs; ++k) d.prevReach[k] = how.prevReach[k];
  return drift::analyse(d);
}

// J4's Quest 3 sticks: rest dead in the middle, and flick, straight back. The left one as his real check files show it (20
// November): pushed out down or down and to the left, a report now and then lands up in the middle, near 0.1, 0.45, and the
// next is back. The right one is made up, a second stick that flicks somewhere else (his real right one doesn't flick): pushed
// down and to the right, from 70% out, two reports across the middle.
WornStick j4Stick(bool right) {
  WornStick w;
  w.restX = w.restY = 0;
  w.weak = 0.006f;
  w.jitter = 0.004f;
  w.spikes = 0;
  w.leftMost = 1;
  if (!right) w.spots = {{-1.95f, 0.4f, 0.8f, 0.1f, 0.45f, 0.06f, 1}};
  else w.spots = {{-kPi / 4, 0.17f, 0.7f, -0.2f, 0.3f, 0.15f, 2}};
  w.rng.seed(right ? 31 : 29);
  return w;
}

// Rolls a stick round the edge and pushes it about, reported in pairs every 14 ms, through fix(): how far the game's stick
// jumped on a flick report from where it was the report before (worst), and on how many reports that weren't flicks the game
// was kept where it was while the stick moved on (held).
struct Live { float worstRaw = 0, worstOut = 0; int flickReports = 0, reports = 0, held = 0; uint32_t counted = 0; };
Live liveRun(WornStick& s, const drift::Params& p, const drift::Flicks& fl, int laps, float lapReports) {
  Live L;
  drift::State st;
  double t = 0;
  float lastY = 0, outBefore[2] = {0, 0}, rawBefore[2] = {0, 0};
  const auto pair = [&](bool thumb) {
    float x, y;
    int touch;
    s.thumb = thumb;
    s.report(x, y, touch);
    float o1x, o1y, o2x, o2y;
    drift::fix(p, fl, st, x, lastY, touch, t, o1x, o1y);
    st.retake = false;
    drift::fix(p, fl, st, x, y, touch, t, o2x, o2y);
    // The second call can take back what the first sent, when it shows the first was half of a flick: the driver sends that
    // axis again at once, so what the first said stood for a few microseconds. Otherwise it stands till the next report.
    const bool retaken = st.retake;
    st.retake = false;
    const float first = retaken ? 0.f : drift::len(o1x - outBefore[0], o1y - outBefore[1]), second = drift::len(o2x - outBefore[0], o2y - outBefore[1]);
    lastY = y;
    ++L.reports;
    if (s.flicking) {
      ++L.flickReports;
      L.worstRaw = std::max(L.worstRaw, drift::len(x - rawBefore[0], y - rawBefore[1]));
      L.worstOut = std::max({L.worstOut, first, second});
      if (std::getenv("DRIFT_DEBUG") && std::max(first, second) > 0.05f)
        std::printf("        got through at %.3f s: stick %.3f %.3f -> %.3f %.3f, game %.3f %.3f -> %.3f %.3f / %.3f %.3f, holding %d\n", t, rawBefore[0], rawBefore[1], x, y,
                    outBefore[0], outBefore[1], o1x, o1y, o2x, o2y, st.holdT >= 0);
    } else {
      if (st.holdT >= 0) ++L.held;
      outBefore[0] = o2x;
      outBefore[1] = o2y;
      rawBefore[0] = x;
      rawBefore[1] = y;
    }
    t += 0.014;
  };
  for (int i = 0; i < laps * lapReports; ++i) {
    const float a = 2 * kPi * i / lapReports;
    s.push(std::cos(a), std::sin(a));
    pair(true);
  }
  for (int k = 0; k < 8; ++k) {  // and pushes out and let go, each way
    const float a = kPi / 4 * k;
    for (int i = 1; i <= 6; ++i) { s.push(std::cos(a) * i / 6.f, std::sin(a) * i / 6.f); pair(true); }
    for (int i = 0; i < 20; ++i) pair(true);
    s.letGo();
    for (int i = 0; i < 20; ++i) pair(false);
  }
  L.counted = st.flicks;
  return L;
}

double nsPerFix(bool flicks = false) {
  drift::Params p;
  p.on = 1; p.cx = -0.08f; p.cy = 0.11f; p.dead = 0.1f; p.gate = 0.25f; p.learnPerSec = 0.5f; p.learnMax = 0.2f;
  for (int k = 0; k < drift::kDirs; ++k) p.reach[k] = 0.9f;
  drift::Flicks fl;
  if (flicks) fl = drift::holdFlicks();
  drift::State st;
  volatile float sink = 0;
  const int n = 2000000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    float ox, oy;
    drift::fix(p, fl, st, 0.9f * std::sin(i * 0.001f), 0.9f * std::cos(i * 0.0013f), i & 1, i * 0.002, ox, oy);
    sink = sink + ox;
  }
  return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
}

void drawLandings(const std::string& path, const std::vector<drift::Dot>& raw, const std::vector<drift::Dot>& out, const drift::Params& p) {
  const int W = 900, H = 470;
  cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  cairo_t* c = cairo_create(s);
  cairo_set_source_rgb(c, 0.082, 0.082, 0.078);
  cairo_paint(c);
  const double R = 180;
  for (int side = 0; side < 2; ++side) {
    const double cx = 230 + side * 440, cy = 250;
    cairo_set_source_rgb(c, 0.145, 0.145, 0.138);
    cairo_arc(c, cx, cy, R, 0, 2 * kPi);
    cairo_fill(c);
    cairo_set_source_rgba(c, 0.925, 0.925, 0.905, 0.6);
    cairo_set_line_width(c, 2);
    cairo_arc(c, cx, cy, R, 0, 2 * kPi);
    cairo_stroke(c);
    cairo_set_line_width(c, 1);
    cairo_move_to(c, cx - R, cy); cairo_line_to(c, cx + R, cy);
    cairo_move_to(c, cx, cy - R); cairo_line_to(c, cx, cy + R);
    cairo_stroke(c);
    // Drawn 2.5 times bigger than life, so a few percent shows: the ring is 40% of the way out.
    const double k = R * 2.5;
    if (side == 0 && p.on) {
      cairo_set_source_rgba(c, 0.925, 0.925, 0.905, 0.9);
      const double dash[2] = {6, 4};
      cairo_set_dash(c, dash, 2, 0);
      cairo_arc(c, cx + p.cx * k, cy - p.cy * k, p.dead * k, 0, 2 * kPi);
      cairo_stroke(c);
      cairo_set_dash(c, nullptr, 0, 0);
    }
    const auto& v = side ? out : raw;
    cairo_set_source_rgba(c, side ? 1.0 : 0.925, side ? 0.824 : 0.925, side ? 0.0 : 0.905, 0.8);
    for (const drift::Dot& d : v) {
      cairo_arc(c, cx + d.x * k, cy - d.y * k, 3, 0, 2 * kPi);
      cairo_fill(c);
    }
    cairo_set_source_rgb(c, 0.925, 0.925, 0.905);
    cairo_select_font_face(c, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(c, 20);
    cairo_move_to(c, cx - R, 40);
    cairo_show_text(c, side ? "What the game gets, fixed" : "Where the worn stick lands");
  }
  cairo_set_font_size(c, 14);
  cairo_set_source_rgb(c, 0.62, 0.62, 0.59);
  cairo_move_to(c, 50, H - 14);
  cairo_show_text(c, "200 let-gos of a made-up worn stick, drawn 2.5x bigger than life. Dashed: what reads as the middle.");
  cairo_surface_write_to_png(s, path.c_str());
  cairo_destroy(c);
  cairo_surface_destroy(s);
}

// ---------------------------------------------------------------- the page's check, the way the app runs it
// sticks::Drift, the app's own stick drift logic, run against a pretend driver: the same shared channel the real one fills,
// with each report fixed the way the driver fixes it, from the settings the page sends. Two sticks: the left one worn, the
// right one fine. The page's pictures are drawn from what it really found, into the folder, as drift_page_*.png.
alignas(8) unsigned char g_channel[sizeof(fhlive::DriftChannel)];  // zeros, like a new live.shm

struct PretendDriver {
  fhlive::DriftChannel* dc = reinterpret_cast<fhlive::DriftChannel*>(g_channel);
  drift::State st[2];
  uint32_t seen = 1;
  drift::Params p[2];
  drift::Flicks fl[2];
  // One report of a hand's stick: fixed, and kept for the page while it's measuring. What the game gets.
  drift::Dot report(int h, float x, float y, int touch, double t) {
    if (dc->seq.load() != seen) {
      seen = dc->seq.load();
      for (int k = 0; k < 2; ++k) {
        drift::Params q;
        drift::Flicks f;
        std::memcpy(&q, &dc->want[k], sizeof q);
        std::memcpy(&f, &dc->flick[k], sizeof f);
        p[k] = drift::sane(q);
        fl[k] = p[k].on ? drift::sane(f) : drift::Flicks{};
        st[k] = drift::State{};
      }
    }
    float ox, oy;
    drift::fix(p[h], fl[h], st[h], x, y, touch, t, ox, oy);
    if (dc->measure.load()) {
      const uint32_t n = dc->write[h].fetch_add(1);
      fhlive::DriftSample& e = dc->ring[h][n % fhlive::kDriftRing];
      e.seq.store(0);
      e.ms = uint32_t(t * 1000);
      e.x = x; e.y = y; e.ox = ox; e.oy = oy;
      e.touch = uint8_t(touch < 0 ? 2 : touch);
      e.seq.store(n + 1);
    }
    return {ox, oy};
  }
};

void pageCheck(const std::string& folder) {
  std::printf("The page's stick check, run the way the app runs it\n");
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "utmtftm_test_drift";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  PretendDriver drv;
  drv.dc->magic.store(fhlive::kDriftMagic);
  drv.dc->flickMagic.store(fhlive::kFlickMagic);
  drv.dc->found[0].store(3);
  drv.dc->found[1].store(3);
  WornStick sticksIn[2];
  sticksIn[1].restX = 0.004f;
  sticksIn[1].restY = -0.003f;
  sticksIn[1].weak = 0.008f;
  sticksIn[1].jitter = 0.004f;
  sticksIn[1].leftMost = 1.f;
  sticksIn[1].spikes = 0;
  sticksIn[1].rng.seed(23);
  const std::string serials[2] = {"WORN-LEFT", "FINE-RIGHT"};
  const bool frames[2] = {false, false};
  sticks::Drift page(dir.u8string());
  double t = 0;
  const auto frame = [&](bool showing) {  // 10 ms of SteamVR: each stick reports twice, then the app's loop
    for (int k = 0; k < 2; ++k)
      for (int h = 0; h < 2; ++h) {
        float x, y;
        int touch;
        sticksIn[h].report(x, y, touch);
        drv.report(h, x, y, touch, t + k * 0.005);
      }
    t += 0.01;
    page.tick(drv.dc, serials, frames, showing, t);
  };
  const auto draw = [&](const std::string& name) {
    if (folder.empty()) return;
    fh::DriftModel m = page.model();
    m.game = "Pavlov";
    m.live = fh::LiveState::Live;
    fh::Canvas cv(1200, 760);
    std::vector<fh::Button> b;
    fh::drawDrift(cv, m, b);
    cv.savePNG(folder + "/" + name);
  };
  for (int i = 0; i < 20; ++i) frame(true);
  check(page.ready() && page.step() == 0 && !page.model().sticks[0].checked, "the page is up, the driver can do it, nothing's been checked");
  draw("drift_page_first.png");
  check(page.click("drift_check", t) && page.step() == 1, "Check my sticks starts step 1");
  // Step 1, with a thumb still on the right stick for the first second: that one waits for it.
  for (WornStick& s : sticksIn) s.letGo();
  sticksIn[1].thumb = true;
  for (int i = 0; i < 100; ++i) frame(true);
  const fh::DriftModel s1 = page.model();
  check(s1.sticks[0].progress > 0.2f && s1.sticks[1].progress == 0.f && s1.countdown == 4, f2("a thumb on the right stick holds it back (left %.2f, right %.2f)", s1.sticks[0].progress, s1.sticks[1].progress));
  draw("drift_page_step1.png");
  sticksIn[1].thumb = false;
  for (int i = 0; i < 500 && page.step() == 1; ++i) frame(true);
  check(page.step() == 2, f2("both left alone four seconds: step 2 (after %.1f s)", t));
  // Step 2: round the edge, slowly, both at once.
  int n2 = 0;
  for (; n2 < 2000 && page.step() == 2; ++n2) {
    const float a = 2 * kPi * n2 / 250.f;
    for (int h = 0; h < 2; ++h) sticksIn[h].push(std::cos(a + h), std::sin(a + h));
    frame(true);
    if (n2 == 150) draw("drift_page_step2.png");
  }
  check(page.step() == 3, f2("all the way round: step 3 (after %.1f s)", n2 * 0.01));
  for (WornStick& s : sticksIn) s.letGo();
  // Step 3: flick and let go, up, right, down, left, twice each, both sticks together.
  for (int k = 0; k < 8 && page.step() == 3; ++k) {
    const float a = kPi / 2 * (k % 4) + kPi / 2;
    for (int i = 0; i < 10; ++i) { for (WornStick& s : sticksIn) s.push(std::cos(a) * i / 10.f, std::sin(a) * i / 10.f); frame(true); }
    for (int i = 0; i < 5; ++i) { for (WornStick& s : sticksIn) s.push(std::cos(a), std::sin(a)); frame(true); }
    for (WornStick& s : sticksIn) s.letGo();
    for (int i = 0; i < 40; ++i) frame(true);
    if (k == 4) draw("drift_page_step3.png");
  }
  check(page.step() == 4, "eight let-gos: it's worked out what it found");
  const fh::DriftModel r = page.model();
  draw("drift_page_result.png");
  check(r.sticks[0].drifts && r.sticks[0].verdict == "Drifts up and to the left", "the left stick drifts, up and to the left: " + r.sticks[0].verdict);
  check(!r.sticks[1].drifts && r.sticks[1].verdict == "No drift", "the right stick doesn't: " + r.sticks[1].verdict);
  for (const std::string& l : r.sticks[0].lines) std::printf("        \"%s\"\n", l.c_str());
  check(fs::exists(fs::u8path(page.file())), "it's saved, in drift.json");
  drift::Params sent[2];
  std::memcpy(sent, drv.dc->want, sizeof sent);
  check(sent[0].on == 1 && std::fabs(sent[0].cx + 0.08f) < 0.04f && std::fabs(sent[0].cy - 0.11f) < 0.04f, f2("the driver's told to fix the left stick, resting at %.3f, %.3f", sent[0].cx, sent[0].cy));
  check(sent[1].on == 0, "and to leave the right one alone");
  // What the game gets now, from the pretend driver with those settings: 100 let-gos of the left stick, thumb off.
  {
    float worst = 0;
    std::mt19937 rng(3);
    for (int i = 0; i < 100; ++i) {
      const float a = std::uniform_real_distribution<float>(0, 2 * kPi)(rng);
      sticksIn[0].push(std::cos(a), std::sin(a));
      for (int k = 0; k < 5; ++k) frame(true);
      sticksIn[0].letGo();
      for (int k = 0; k < 30; ++k) frame(true);
      float x, y;
      int touch;
      sticksIn[0].report(x, y, touch);
      const drift::Dot o = drv.report(0, x, y, touch, t);
      worst = std::max(worst, drift::len(o.x, o.y));
    }
    check(worst == 0.f, f2("after the check, the game gets the middle every time the left stick's let go (worst %.4f)", worst));
  }
  check(page.click("drift_done", t) && page.step() == 0, "Done goes back to the page");
  draw("drift_page_after.png");
  // Off and on.
  page.click("drift_off", t);
  frame(true);
  check(drv.dc->want[0].on == 0 && !page.fixOn(), "Fix drift off: the driver's told to leave both alone");
  page.click("drift_on", t);
  frame(true);
  check(drv.dc->want[0].on == 1, "and on again");
  // Started again: it remembers.
  {
    sticks::Drift again(dir.u8string());  // the app started again, with the driver still holding its last settings: cleared here
    std::memset(&drv.dc->want, 0, sizeof drv.dc->want);
    fhlive::seqWriteBegin(drv.dc->seq);
    fhlive::seqWriteEnd(drv.dc->seq);
    again.tick(drv.dc, serials, frames, false, t);
    check(drv.dc->want[0].on == 1 && std::fabs(drv.dc->want[0].cx - sent[0].cx) < 1e-4f && std::fabs(drv.dc->want[0].dead - sent[0].dead) < 1e-4f && drv.dc->want[1].on == 0,
          f2("after a restart it sends the same fix, from drift.json (middle x %.5f, was %.5f)", drv.dc->want[0].cx, sent[0].cx));
    check(again.model().sticks[0].checked && again.model().sticks[0].verdict == r.sticks[0].verdict, "and the page still says what it found");
    // Another controller in the right hand: not checked, nothing sent for it.
    const std::string other[2] = {"WORN-LEFT", "SOMEONE-ELSES"};
    again.tick(drv.dc, other, frames, false, t + 1);
    check(!again.model().sticks[1].checked && drv.dc->want[1].on == 0 && drv.dc->want[0].on == 1, "a different controller in a hand isn't fixed with the old one's numbers");
    // Only Frame controllers on: the switch to this page doesn't show.
    const bool both[2] = {true, true};
    again.tick(drv.dc, serials, both, false, t + 2);
    check(again.hidden(), "with only a Frame's controllers on, the Sticks switch is hidden");
    again.tick(drv.dc, serials, frames, false, t + 3);
    check(!again.hidden(), "and with others on, it's there");
  }
  // Leaving part way through a check changes nothing.
  {
    const std::string before = [&] { std::ifstream f(fs::u8path(page.file())); std::stringstream b; b << f.rdbuf(); return b.str(); }();
    page.click("drift_check", t);
    for (int i = 0; i < 50; ++i) frame(true);
    page.leave();
    check(page.step() == 0, "leaving the page stops a check");
    const std::string after = [&] { std::ifstream f(fs::u8path(page.file())); std::stringstream b; b << f.rdbuf(); return b.str(); }();
    check(before == after, "and nothing saved changed");
    page.click("drift_check", t);
    page.click("drift_cancel", t);
    check(page.step() == 0 && page.status == "Cancelled. Nothing changed.", "Cancel says nothing changed");
  }
  // An older driver: it can't, and the page says how to fix that.
  {
    drv.dc->magic.store(0);
    frame(true);
    check(!page.ready(), "an older driver: not ready");
    page.click("drift_check", t);
    check(page.step() == 0 && page.status.find("Restart SteamVR") != std::string::npos, "Check says to restart SteamVR once: " + page.status);
    // 0.6.3's driver, still loaded after 0.6.4 went in: it fixes drift but can't hold flicks back. Restart SteamVR too.
    drv.dc->magic.store(fhlive::kDriftMagic);
    drv.dc->flickMagic.store(0);
    frame(true);
    page.click("drift_check", t);
    check(!page.ready() && page.step() == 0 && page.status.find("Restart SteamVR") != std::string::npos, "0.6.3's driver still loaded: restart SteamVR once too: " + page.status);
    drv.dc->flickMagic.store(fhlive::kFlickMagic);
  }
  fs::remove_all(dir, ec);
}

// Every button inside the picture and none on top of another, as test_ui checks its pictures.
void buttonsFit(const std::vector<fh::Button>& b, int w, int h, const std::string& what) {
  bool in = true, apart = true;
  for (size_t i = 0; i < b.size(); ++i) {
    in = in && b[i].x >= 0 && b[i].y >= 0 && b[i].x + b[i].w <= w && b[i].y + b[i].h <= h;
    for (size_t j = i + 1; j < b.size(); ++j) {
      const double across = std::min(b[i].x + b[i].w, b[j].x + b[j].w) - std::max(b[i].x, b[j].x), down = std::min(b[i].y + b[i].h, b[j].y + b[j].h) - std::max(b[i].y, b[j].y);
      apart = apart && (across <= 24 || down <= 2);
    }
  }
  check(in && apart, what + ": every button inside the page, none on top of another");
}

// J4's flicking sticks through the page itself: the check finds the flicks, says where, saves the map for these controllers,
// the driver holds them back, and the check's samples are kept in a file. Pictures of the result, draft A (the lines) and
// draft B (the lines and marks on the stick), as drift_flicks_A.png and drift_flicks_B.png.
void pageFlicks(const std::string& folder) {
  std::printf("J4, 0.6.3: his flicking sticks through the page\n");
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "utmtftm_test_drift_flicks";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  std::memset(g_channel, 0, sizeof g_channel);
  PretendDriver drv;
  drv.dc->magic.store(fhlive::kDriftMagic);
  drv.dc->flickMagic.store(fhlive::kFlickMagic);
  drv.dc->found[0].store(3);
  drv.dc->found[1].store(3);
  WornStick sticksIn[2] = {j4Stick(false), j4Stick(true)};
  const std::string serials[2] = {"2QTEST0000TEST_Controller_Left", "2QTEST0000TEST_Controller_Right"};
  const bool frames[2] = {false, false};
  sticks::Drift page(dir.u8string());
  double t = 0;
  int reachedGame = 0;
  const auto frame = [&](bool showing) {  // 10 ms: each stick reports twice
    for (int k = 0; k < 2; ++k)
      for (int h = 0; h < 2; ++h) {
        float x, y;
        int touch;
        sticksIn[h].report(x, y, touch);
        const drift::Dot o = drv.report(h, x, y, touch, t + k * 0.005);
        if (sticksIn[h].flicking && drv.p[h].on && drift::any(drv.fl[h])) {
          // What a flick would have shown is the flick's point: the game getting anywhere near it means it got through.
          float fx, fy;
          drift::State probe;
          drift::fix(drv.p[h], probe, x, y, touch, 0, fx, fy);
          drift::fix(drv.p[h], probe, x, y, touch, 1, fx, fy);  // past the middle's 6 ms
          reachedGame += drift::len(o.x - fx, o.y - fy) < 0.05f;
        }
      }
    t += 0.01;
    page.tick(drv.dc, serials, frames, showing, t);
  };
  for (int i = 0; i < 20; ++i) frame(true);
  page.click("drift_check", t);
  for (WornStick& s : sticksIn) s.letGo();
  for (int i = 0; i < 700 && page.step() == 1; ++i) frame(true);
  for (int n = 0; n < 3000 && page.step() == 2; ++n) {
    const float a = 2 * kPi * n / 250.f;
    for (int h = 0; h < 2; ++h) sticksIn[h].push(std::cos(a), std::sin(a));
    frame(true);
  }
  for (WornStick& s : sticksIn) s.letGo();
  for (int k = 0; k < 12 && page.step() == 3; ++k) {
    const float a = kPi / 2 * (k % 4);
    for (int i = 0; i < 10; ++i) { for (WornStick& s : sticksIn) s.push(std::cos(a) * i / 10.f, std::sin(a) * i / 10.f); frame(true); }
    for (int i = 0; i < 8; ++i) { for (WornStick& s : sticksIn) s.push(std::cos(a), std::sin(a)); frame(true); }
    for (WornStick& s : sticksIn) s.letGo();
    for (int i = 0; i < 40; ++i) frame(true);
  }
  check(page.step() == 4, f2("the check runs through to what it found (step %.0f)", page.step()));
  const fh::DriftModel r = page.model();
  for (int h = 0; h < 2; ++h) {
    std::printf("        %s: \"%s\"\n", h ? "right" : "left", r.sticks[h].verdict.c_str());
    for (const std::string& l : r.sticks[h].lines) std::printf("          \"%s\"\n", l.c_str());
  }
  check(r.sticks[0].drifts && r.sticks[1].drifts, "both read as drift: " + r.sticks[0].verdict + ", " + r.sticks[1].verdict);
  const auto says = [&](int h, const std::string& line) {
    for (const std::string& l : r.sticks[h].lines)
      if (l == line) return true;
    return false;
  };
  // J4's pick (20 November, "New wording"): how many times, not where from.
  const auto times = [&](int h) {
    const auto& v = r.sticks[h].lines;
    for (size_t i = 0; i + 1 < v.size(); ++i)
      if (v[i] == "Jumps away and straight back,") return v[i + 1];
    return std::string();
  };
  bool spots = false;
  for (int h = 0; h < 2; ++h)
    for (const std::string& l : r.sticks[h].lines) spots = spots || l.rfind("Jumps at ", 0) == 0;
  check(!times(0).empty() && !times(1).empty() && !spots, "J4's pick: both say how many times they jumped, not where from: " + times(0) + " / " + times(1));
  const int madeR = sticksIn[1].flicks;
  check(times(1) == (madeR == 1 ? std::string("once in this check.") : std::to_string(madeR) + " times in this check."),
        f2("and the right one's count is the flicks it made: %.0f", madeR) + ", \"" + times(1) + "\"");
  check(!r.sticks[0].flickAt.empty() && !r.sticks[1].flickAt.empty() && !r.flickMarks, "the page has where they jump, and draws draft A unless switched");
  drift::Flicks sent[2];
  std::memcpy(sent, drv.dc->flick, sizeof sent);
  check(drift::any(sent[0]) && drift::any(sent[1]) && sent[0].big == drift::kHoldBig && drv.dc->want[0].on && drv.dc->want[1].on,
        "the driver is told to hold flicks back on both, and to fix both");
  // Saved for these controllers.
  {
    std::ifstream f(fs::u8path(page.file()));
    std::stringstream b;
    b << f.rdbuf();
    check(b.str().find("\"flick_map\"") != std::string::npos && b.str().find("\"hold\" : true") != std::string::npos && b.str().find("\"reach_seen\"") != std::string::npos,
          "drift.json keeps that it flicks and what it reached, per controller");
    sticks::Drift again(dir.u8string());
    const auto& sv = again.saved();
    const auto it = sv.find(serials[0]);
    check(it != sv.end() && std::memcmp(&it->second.fl, &sent[0], sizeof sent[0]) == 0 && it->second.flicks > 0,
          "and reads it back the same after a restart");
  }
  // The samples, in a small file, for tuning from a real stick.
  {
    const std::string sf = page.lastSamples;
    check(!sf.empty() && fs::exists(fs::u8path(sf)), "the check's samples are kept in a file in the data folder: " + fs::u8path(sf).filename().u8string());
    std::ifstream f(fs::u8path(sf));
    std::string line;
    int steps[4] = {0, 0, 0, 0}, flickLines = 0;
    while (std::getline(f, line)) {
      if (line.rfind("#   flick", 0) == 0) ++flickLines;
      if (line.size() > 4 && (line[0] == 'L' || line[0] == 'R') && line[1] == ',' && line[2] >= '1' && line[2] <= '3') ++steps[line[2] - '0'];
    }
    check(steps[1] > 100 && steps[2] > 100 && steps[3] > 100 && flickLines > 3, f2("with every step in it (%.0f samples) and each flick found", steps[1] + steps[2] + steps[3]));
    check(fs::file_size(fs::u8path(sf), ec) < 2000000, f2("and it's small: %.0f KB", fs::file_size(fs::u8path(sf), ec) / 1024.0));
  }
  // Live, after the check: rolling round, the flicks never reach the game.
  reachedGame = 0;
  page.click("drift_done", t);
  for (int n = 0; n < 1000; ++n) {
    const float a = 2 * kPi * n / 250.f;
    for (int h = 0; h < 2; ++h) sticksIn[h].push(std::cos(a), std::sin(a));
    frame(false);
  }
  check(reachedGame == 0 && drv.st[0].flicks + drv.st[1].flicks > 10, f2("J4, 0.6.3: rolling after the check, no flick reaches the game (%.0f held back, %.0f got through)",
                                                                  drv.st[0].flicks + drv.st[1].flicks, reachedGame));
  // The pictures: the result page, draft A and draft B.
  if (!folder.empty()) {
    for (int d = 0; d < 2; ++d) {
      for (int h = 0; h < 2; ++h) sticksIn[h].letGo();
      fh::DriftModel m = r;
      m.game = "Pavlov";
      m.live = fh::LiveState::Live;
      m.flickMarks = d == 1;  // draft B only in its own picture: the page itself sets it (kFlickMarks)
      fh::Canvas cv(1200, 760);
      std::vector<fh::Button> b;
      fh::drawDrift(cv, m, b);
      const std::string name = d ? "drift_flicks_B.png" : "drift_flicks_A.png";
      cv.savePNG(folder + "/" + name);
      buttonsFit(b, cv.w, cv.h, name);
    }
  }
  fs::remove_all(dir, ec);
}

// J4's own drift.json from his 16 November check (0.6.3-test, both sticks called short, the fix switched off): 0.6.4 reads it.
void oldFile() {
  std::printf("J4, 0.6.3: his 16 November drift.json still loads\n");
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "utmtftm_test_drift_old";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  {
    std::ofstream f(dir / "drift.json", std::ios::binary);
    f << R"({"controllers":{"2QTEST0000TEST_Controller_Left":{"checked":"2026-11-16","dead":0.04,"drifts":true,"gate":0.15,"learn_max":0.1,
"learn_per_s":0.5,"lines":["Only gets 64% of the way to the left."],"middle":[0.0,0.0],"reach":[0.97,1.0428,0.97,0.97,0.6232,1.0285,0.8401,0.935],
"rest":[0.0,0.0],"touch":true,"verdict":"Drifts"},"2QTEST0000TEST_Controller_Right":{"checked":"2026-11-16","dead":0.04,"drifts":true,"gate":0.15,
"learn_max":0.1,"learn_per_s":0.5,"lines":["Only gets 77% of the way down and to the right."],"middle":[0.0,-0.0],"reach":[0.0,1.0659,0.97,0.8356,0.97,1.0684,0.97,0.7426],
"rest":[0.0,0.0],"touch":true,"verdict":"Drifts"}},"on":false})";
  }
  sticks::Drift page(dir.u8string());
  const auto& sv = page.saved();
  const auto l = sv.find("2QTEST0000TEST_Controller_Left");
  check(sv.size() == 2 && l != sv.end() && l->second.drifts && std::fabs(l->second.p.reach[4] - 0.6232f) < 1e-4f && !page.fixOn(), "both controllers, as saved, with the fix off");
  check(!drift::any(l->second.fl) && l->second.flicks == 0 && l->second.reachSeen[4] == 0, "and no flick map: 0.6.3 never made one");
  std::memset(g_channel, 0, sizeof g_channel);
  PretendDriver drv;
  drv.dc->magic.store(fhlive::kDriftMagic);
  drv.dc->flickMagic.store(fhlive::kFlickMagic);
  drv.dc->found[0].store(3);
  drv.dc->found[1].store(3);
  const std::string serials[2] = {"2QTEST0000TEST_Controller_Left", "2QTEST0000TEST_Controller_Right"};
  const bool frames[2] = {false, false};
  page.tick(drv.dc, serials, frames, false, 1);
  check(drv.dc->want[0].on == 0 && drv.dc->want[1].on == 0, "with the fix off, nothing's sent");
  fs::remove_all(dir, ec);

  // His drift.json from 0.6.4's first test build (20 November, third check): a map of where the left one's flicks started.
  // The map isn't used any more; the left one still has its flicks held back, with no new check.
  std::printf("J4 Shakedown 2026-11-20: his 0.6.4-test drift.json still loads\n");
  fs::create_directories(dir, ec);
  {
    std::ofstream f(dir / "drift.json", std::ios::binary);
    f << R"({"controllers":{"2QTEST0000TEST_Controller_Left":{"checked":"2026-11-20","dead":0.04,"drifts":true,"flick_map":{"anywhere":0.35,
"at_spot":0.1,"cells":[6,7,8,9,10,18,19,20,21,22,38,39,40,41,42,96,97,98,102,103,104,105,106,126,127],"from":[[-0.1007,0.9949],[0.9965,0.0648],
[-0.0112,0.2875],[-0.0062,0.3101],[-0.1939,-0.195],[0.3838,-0.977]],"hold_else":1.5,"hold_spot":2.5,"seen":6},"gate":0.15,"learn_max":0.1,
"learn_per_s":0.5,"lines":["Jumps at 4 spots:","right, up, down-left and down.","Reaches the edge all round."],"middle":[0.0,0.0],
"reach":[0.9706,1.0426,0.9702,0.97,0.9822,1.0097,0.9817,0.984],"reach_seen":[1.0006,1.0748,1.0002,1.0,1.0126,1.0409,1.0121,1.0144],
"rest":[0.0,0.0],"touch":true,"verdict":"Drifts"},"2QTEST0000TEST_Controller_Right":{"checked":"2026-11-20","dead":0.04,"drifts":false,
"gate":0.15,"learn_max":0.1,"learn_per_s":0.5,"lines":["Comes back to the middle","every time. Left as it is."],"middle":[0.0,0.0],
"reach":[0.97,1.0486,0.984,0.9828,0.9778,1.037,0.9871,1.009],"reach_seen":[1.0,1.081,1.0144,1.0132,1.008,1.0691,1.0177,1.0403],"rest":[0.0,0.0],
"touch":true,"verdict":"No drift"}},"on":true})";
  }
  sticks::Drift page64(dir.u8string());
  const auto& sv64 = page64.saved();
  const auto l64 = sv64.find("2QTEST0000TEST_Controller_Left"), r64 = sv64.find("2QTEST0000TEST_Controller_Right");
  check(l64 != sv64.end() && r64 != sv64.end() && page64.fixOn(), "both controllers, with the fix on");
  check(l64 != sv64.end() && drift::any(l64->second.fl) && l64->second.fl.big == drift::kHoldBig && l64->second.flicks == 6,
        "the left one's flicks are held back the new way, with no new check");
  check(r64 != sv64.end() && !drift::any(r64->second.fl), "the right one, which doesn't flick, is left alone");

  std::memset(g_channel, 0, sizeof g_channel);
  PretendDriver drv64;
  drv64.dc->magic.store(fhlive::kDriftMagic);
  drv64.dc->flickMagic.store(0x314B4C46);  // "FLK1": 0.6.4's first test build's driver, still loaded
  drv64.dc->found[0].store(3);
  drv64.dc->found[1].store(3);
  page64.tick(drv64.dc, serials, frames, true, 1);
  check(!page64.ready(), "with that test build's driver still loaded, the page asks for SteamVR to be restarted");
  drv64.dc->flickMagic.store(fhlive::kFlickMagic);
  page64.tick(drv64.dc, serials, frames, true, 2);
  drift::Flicks sent64;
  std::memcpy(&sent64, &drv64.dc->flick[0], sizeof sent64);
  check(page64.ready() && drv64.dc->want[0].on && drift::any(sent64) && !drv64.dc->want[1].on, "after the restart, the left one is fixed and its flicks held back");
  {
    const fh::DriftModel m64 = page64.model();
    std::string all;
    for (const std::string& l : m64.sticks[0].lines) all += l + " / ";
    check(all == "Jumps away and straight back, / 6 times in this check. / Reaches the edge all round. / ",
          "J4's pick: his saved \"Jumps at 4 spots\" reads as how many times instead: " + all);
  }
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------- J4's real sticks
// His three real stick checks with 0.6.4's first test build (Shakedown, 20 November 2026, Quest 3 over Virtual Desktop),
// from tests/data/j4-2026-11-20.txt: what he got, replayed through the check and the fix the way the driver hears them.
// What he said: with Fix drift on "it still jumps with the spot of jitter. It doesn't understand that proximity does not
// determine where an area jumps", "there's still stick drift", and "I do notice delay, a little bit".
struct RealReport { int step; double t; float x, y; int touch; bool oneCall; };
struct RealCheck { std::vector<RealReport> hand[2]; };

std::vector<RealCheck> readReal(const std::string& path) {
  std::vector<RealCheck> out;
  std::ifstream f(std::filesystem::u8path(path));
  std::string line;
  double t[2] = {0, 0};
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (line.rfind("check", 0) == 0) { out.emplace_back(); t[0] = t[1] = 0; continue; }
    std::istringstream in(line);
    std::string hs;
    int dt = 0, x = 0, y = 0, touch = 0, one = 0;
    if (!(in >> hs >> dt >> x >> y >> touch) || hs.size() < 2 || out.empty()) continue;
    in >> one;
    const int h = hs[0] == 'R' ? 1 : 0;
    t[h] += dt / 1000.0;
    out.back().hand[h].push_back({hs[1] - '0', t[h], x / 10000.f, y / 10000.f, touch, one == 1});
  }
  return out;
}

// The samples the overlay got during the check: x first with the y from before, then both, when it came as two calls.
std::vector<drift::Sample> rowsOf(const std::vector<RealReport>& v, int step) {
  std::vector<drift::Sample> out;
  float lastY = 0;
  for (const RealReport& r : v) {
    if (r.step == step) {
      if (!r.oneCall) out.push_back({r.x, lastY, r.touch, r.t});
      out.push_back({r.x, r.y, r.touch, r.t + 0.0002});
    }
    lastY = r.y;
  }
  return out;
}

// With hindsight, the reports that stand off the stick's way: a run of up to four that's well off the line from the report
// before it to the report after, by much more than that line is long. What's found here is what the fix should catch.
struct OffTrack { size_t i, j; float off; };  // reports [i, j); i - 1 and j are on the track
std::vector<OffTrack> offTrack(const std::vector<RealReport>& r) {
  std::vector<OffTrack> out;
  size_t i = 1;
  while (i + 1 < r.size()) {
    bool hit = false;
    for (size_t n = 1; n <= 4 && i + n < r.size(); ++n) {
      const size_t j = i + n;
      const RealReport &a = r[i - 1], &b = r[j];
      float lo = 9, hi = 0;
      for (size_t k = i; k < j; ++k) {
        const float d = drift::toSegment(r[k].x, r[k].y, a.x, a.y, b.x, b.y);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
      }
      if (lo > 0.2f && lo > 1.5f * drift::len(b.x - a.x, b.y - a.y)) {
        out.push_back({i, j, hi});
        i = j;
        hit = true;
        break;
      }
      if (drift::toSegment(r[i].x, r[i].y, a.x, a.y, b.x, b.y) < 0.1f) break;
    }
    if (!hit) ++i;
  }
  return out;
}

// What the game gets for each report, the way the driver sends it: each call's own axis, and the other one again when that
// call changed it (the driver's resend). p.on 0: as the stick said it.
std::vector<drift::Dot> replay(const std::vector<RealReport>& v, const drift::Params& p, const drift::Flicks& fl, uint32_t* flicksHeld = nullptr) {
  std::vector<drift::Dot> game;
  drift::State st;
  float sent[2] = {0, 0}, lastY = 0;
  double reported[2] = {-1, -1};
  for (const RealReport& r : v) {
    const auto call = [&](int axis, float x, float y, double t) {
      float o[2];
      drift::fix(p, fl, st, x, y, r.touch, t, o[0], o[1]);
      const bool retake = st.retake;
      st.retake = false;
      if (p.on && std::fabs(o[1 - axis] - sent[1 - axis]) > 0.01f && (t - reported[1 - axis] > 0.02 || retake)) sent[1 - axis] = o[1 - axis];
      sent[axis] = o[axis];
      reported[axis] = t;
    };
    if (!r.oneCall) call(0, r.x, lastY, r.t);
    call(1, r.x, r.y, r.t + 0.0002);
    lastY = r.y;
    game.push_back({sent[0], sent[1]});
  }
  if (flicksHeld) *flicksHeld = st.flicks;
  return game;
}

void realSticks(const std::string& folder) {
  namespace fs = std::filesystem;
  const std::string path = (fs::path(__FILE__).parent_path() / "data" / "j4-2026-11-20.txt").u8string();
  const std::vector<RealCheck> checks = readReal(path);
  std::printf("J4 Shakedown 2026-11-20: his three real checks (%s)\n", fs::u8path(path).filename().u8string().c_str());
  check(checks.size() == 3 && checks[0].hand[0].size() > 3000 && checks[2].hand[1].size() > 2000, f2("all three are there: %.0f checks, %.0f reports in the first", double(checks.size()), checks.empty() ? 0.0 : double(checks[0].hand[0].size())));
  if (checks.size() != 3) return;

  // The check itself, on what he really did: the same answer each time (drift-7, check twice).
  int leftFlicky = 0, rightClean = 0;
  drift::Findings first[2];
  for (size_t c = 0; c < checks.size(); ++c) {
    for (int h = 0; h < 2; ++h) {
      const std::vector<RealReport>& v = checks[c].hand[h];
      drift::CheckData d;
      d.rest = drift::restOf(rowsOf(v, 1));
      d.edge = rowsOf(v, 2);
      d.flick = rowsOf(v, 3);
      d.landed = drift::landingsOf(d.flick, d.rest.x, d.rest.y);
      d.hasTouch = true;
      const drift::Findings F = drift::analyse(d);
      std::string lines;
      for (const std::string& l : F.lines) lines += " | " + l;
      std::printf("        check %d, %s: %s, %d flicks%s\n", int(c + 1), h ? "right" : "left", F.verdict.c_str(), int(F.flicks.size()), lines.c_str());
      if (h == 0) leftFlicky += F.drifts && F.flicky && drift::any(F.fl);
      else rightClean += !F.drifts && !drift::any(F.fl);
      if (c == 0) first[h] = F;
    }
  }
  check(leftFlicky == 3, f2("J4 Shakedown 2026-11-20: all three checks say his left stick flicks, and hold its flicks back (%.0f of 3)", leftFlicky));
  check(rightClean == 3, f2("J4 Shakedown 2026-11-20: and all three leave his right one alone (%.0f of 3)", rightClean));

  // Live: every report of all three checks, as the driver would have fixed it with his own saved numbers.
  const auto params = [](int h) {
    drift::Params p;
    p.on = 1;
    p.dead = 0.04f;
    p.gate = 0.15f;
    p.learnPerSec = 0.5f;
    p.learnMax = 0.1f;
    const float reach[2][8] = {{0.9706f, 1.0426f, 0.9702f, 0.97f, 0.9822f, 1.0097f, 0.9817f, 0.984f}, {0.97f, 1.0486f, 0.984f, 0.9828f, 0.9778f, 1.037f, 0.9871f, 1.009f}};
    for (int k = 0; k < 8; ++k) p.reach[k] = reach[h][k];
    return p;
  };
  // Where a point goes with no flick in the way: the fix with nothing held, from fresh.
  const auto gameOf = [](const drift::Params& p, float x, float y) {
    drift::State s;
    float ox, oy;
    drift::fix(p, s, x, y, 1, 0, ox, oy);
    drift::fix(p, s, x, y, 1, 1, ox, oy);
    return drift::Dot{ox, oy};
  };
  struct Score { int big = 0, bigSeen = 0, other = 0, otherSeen = 0, pushes = 0, dropped = 0, heldMoves = 0, longest = 0, letGos = 0, longestLetGo = 0; uint32_t counted = 0; };
  const auto score = [&](int h, const drift::Flicks& fl) {
    Score s;
    const drift::Params p = params(h);
    for (const RealCheck& c : checks) {
      const std::vector<RealReport>& v = c.hand[h];
      uint32_t counted = 0;
      const std::vector<drift::Dot> game = replay(v, p, fl, &counted), unheld = replay(v, p, drift::Flicks{});
      s.counted += counted;
      std::vector<bool> off(v.size(), false);
      for (const OffTrack& o : offTrack(v)) {
        const RealReport &a = v[o.i - 1], &b = v[o.j];
        for (size_t k = o.i; k < o.j; ++k) off[k] = true;
        if (drift::len(a.x, a.y) < 0.15f) continue;  // from the middle: never held, on purpose (a quick flick to turn)
        const drift::Dot ga = gameOf(p, a.x, a.y), gb = gameOf(p, b.x, b.y);
        const bool push = drift::len(a.x, a.y) > 0.9f && drift::len(b.x, b.y) > 0.9f;
        for (size_t k = o.i; k < o.j; ++k) {
          const bool seen = drift::toSegment(game[k].x, game[k].y, ga.x, ga.y, gb.x, gb.y) > 0.2f;
          if (o.off >= 0.8f) { ++s.big; s.bigSeen += seen; }
          else { ++s.other; s.otherSeen += seen; }
          if (push) { ++s.pushes; s.dropped += drift::len(game[k].x, game[k].y) < 0.9f; }
        }
      }
      // Held up: a report that isn't a flick, where holding flicks back changed what the game got (against the same fix with
      // nothing held back). A let-go held a report while its spring overshoots the other way is counted apart.
      const auto differs = [&](size_t k) { return drift::len(game[k].x - unheld[k].x, game[k].y - unheld[k].y) > 0.1f; };
      for (size_t k = 0; k < v.size();) {
        if (off[k] || !differs(k)) { ++k; continue; }
        size_t m = k;
        while (m < v.size() && !off[m] && differs(m)) ++m;
        const bool letGo = m < v.size() && drift::len(v[m].x, v[m].y) <= 0.05f;
        if (std::getenv("DRIFT_DEBUG") && m - k >= 3) {
          std::printf("        held %d reports%s:", int(m - k), letGo ? " (let-go)" : "");
          for (size_t q = k > 0 ? k - 1 : 0; q < std::min(v.size(), m + 2); ++q)
            std::printf(" %s%.2f,%.2f>%.2f,%.2f", off[q] ? "*" : "", v[q].x, v[q].y, game[q].x, game[q].y);
          std::printf("\n");
        }
        if (letGo) { ++s.letGos; s.longestLetGo = std::max(s.longestLetGo, int(m - k)); }
        else { ++s.heldMoves; s.longest = std::max(s.longest, int(m - k)); }
        k = std::max(m, k + 1);
      }
    }
    return s;
  };
  const Score raw[2] = {score(0, drift::Flicks{}), score(1, drift::Flicks{})};
  const Score now[2] = {score(0, drift::holdFlicks()), score(1, drift::holdFlicks())};
  for (int h = 0; h < 2; ++h)
    std::printf("        %s: big flicks reaching the game %d of %d unheld, %d now; smaller %d of %d, %d now; full pushes dropping %d of %d, %d now;\n"
                "          held where hindsight sees no flick %d times (on the left, mostly flicks that end in the middle), longest %d reports;\n"
                "          let-gos held while the spring overshoots %d times, longest %d; %u flicks counted\n",
                h ? "right" : "left", raw[h].bigSeen, raw[h].big, now[h].bigSeen, raw[h].otherSeen, raw[h].other, now[h].otherSeen,
                raw[h].dropped, raw[h].pushes, now[h].dropped, now[h].heldMoves, now[h].longest, now[h].letGos, now[h].longestLetGo, now[h].counted);
  check(raw[0].big >= 25 && raw[0].bigSeen * 10 >= raw[0].big * 9, f2("J4 Shakedown 2026-11-20: his left stick throws %.0f big flicks (80%% of the stick or more) in the three checks, %.0f reaching the game unheld", raw[0].big, raw[0].bigSeen));
  check(now[0].bigSeen * 6 <= now[0].big, f2("J4 Shakedown 2026-11-20 (drift-9, drift-13): with the fix, %.0f of %.0f still reach the game (0.6.4's first test build: 16 of 31)", now[0].bigSeen, now[0].big));
  check(now[0].dropped * 10 <= now[0].pushes, f2("J4 Shakedown 2026-11-20 (drift-11): a full push that flicks drops below 90%% on %.0f reports of %.0f", now[0].dropped, now[0].pushes));
  check(now[1].heldMoves <= 6 && now[1].longest <= 4, f2("J4 Shakedown 2026-11-20 (drift-12): his right stick's real moves, all three checks, are held up %.0f times, %.0f reports at most", now[1].heldMoves, now[1].longest));
  check(now[0].longest <= 4 && now[1].longestLetGo <= 2 && now[0].longestLetGo <= 4,
        f2("J4 Shakedown 2026-11-20 (drift-12): nothing is held more than four reports (56 ms); a let-go %.0f at most on the right, %.0f on the left", now[1].longestLetGo, now[0].longestLetGo));
  check(now[0].counted >= 20, f2("and the held flicks are counted: %.0f", now[0].counted));

  // Pictures of his first check's result, and of the page afterwards with Fix drift on while the left stick flicks: the page
  // as it is, and the draft that says how many times instead of where, and why the ring still jumps (kFlickWords). J4 picks.
  if (folder.empty()) return;
  for (int draft = 0; draft < 2; ++draft)
    for (int live = 0; live < 2; ++live) {
      fh::DriftModel m;
      m.game = "Pavlov";
      m.live = fh::LiveState::Live;
      m.step = live ? 0 : 4;
      m.fixOn = true;
      m.explainJumps = draft == 1;
      if (!live) { m.status = "Checked and fixed. It's saved for these controllers."; m.statusGood = true; }
      for (int h = 0; h < 2; ++h) {
        const drift::Findings& F = first[h];
        fh::DriftStick& s = m.sticks[h];
        s.checked = true;
        s.drifts = F.drifts;
        s.verdict = F.verdict;
        s.verdictWarn = F.drifts;
        s.lines = draft ? sticks::flickWords(F.lines, int(F.flicks.size())) : F.lines;
        if (F.drifts) {
          s.rest = {F.p.cx, F.p.cy};
          s.dead = F.p.dead;
          for (const drift::Flick& k : F.flicks) s.flickAt.push_back({k.fromX, k.fromY});
        }
      }
      if (live) {
        // Rolling round the bottom of the left stick, and a flick right now: up into the middle. The ring's there; the game
        // still has the stick where it was.
        fh::DriftStick& s = m.sticks[0];
        for (int i = 0; i < 12; ++i) {
          const float a = -0.3f - 0.12f * i;
          s.trail.push_back({std::cos(a), std::sin(a)});
        }
        s.raw = {0.10f, 0.45f};
        s.out = {std::cos(-1.62f), std::sin(-1.62f)};
        m.sticks[1].raw = m.sticks[1].out = {0, 0};
      }
      fh::Canvas cv(1200, 760);
      std::vector<fh::Button> b;
      fh::drawDrift(cv, m, b);
      const std::string name = std::string("drift_j4_") + (draft ? "new" : "now") + (live ? "_live.png" : "_result.png");
      cv.savePNG(folder + "/" + name);
      buttonsFit(b, cv.w, cv.h, name);
    }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string folder = argc > 1 ? argv[1] : "";

  std::printf("A worn stick: rests 14%% up and to the left, lands up to 5%% short of there, gets 84%% of the way left\n");
  WornStick worn;
  Run run;
  const drift::Findings f = checkStick(worn, true, &run);
  check(f.drifts && f.p.on, "the check says it drifts, and turns the fix on: " + f.verdict);
  check(f.verdict == "Drifts up and to the left", "and which way: " + f.verdict);
  check(std::fabs(f.offset - 0.136f) < 0.04f, f2("it rests %.3f from the middle (made %.3f)", f.offset, 0.136));
  check(f.worstDir == 4 && f.worstReach < 0.92f, f2("it finds the short side, left (dir %.0f), reaching %.2f of the way", f.worstDir, f.worstReach));
  check(f.p.dead < 0.16f, f2("the dead zone is only as big as it needs to be: %.3f", f.p.dead));
  check(f.touchUsed && f.p.gate > f.p.dead && f.p.learnPerSec > 0, "it goes by the touch sensor, which works");
  for (const std::string& l : f.lines) std::printf("        \"%s\"\n", l.c_str());

  {
    std::printf("Letting go, 200 times from every way, thumb off\n");
    drift::State st;
    double t = 0;
    float worstRaw = 0, worstFixed = 0;
    std::vector<drift::Dot> raws, fixeds;
    std::mt19937 rng(5);
    for (int i = 0; i < 200; ++i) {
      const float a = std::uniform_real_distribution<float>(0, 2 * kPi)(rng);
      worn.push(std::cos(a), std::sin(a));
      worn.letGo();
      float x = 0, y = 0, ox = 0, oy = 0;
      int touch = 0;
      for (int k = 0; k < 50; ++k) {  // 0.2 s resting
        worn.report(x, y, touch);
        drift::fix(f.p, st, x, y, touch, t, ox, oy);
        t += 0.004;
        worstRaw = std::max(worstRaw, drift::len(x, y));
        worstFixed = std::max(worstFixed, drift::len(ox, oy));
      }
      raws.push_back({x, y});
      fixeds.push_back({ox, oy});
    }
    check(worstRaw > 0.1f, f2("unfixed, a game would see it up to %.2f out", worstRaw));
    check(worstFixed == 0.f, f2("fixed, the game gets exactly the middle every time: worst %.4f", worstFixed));
    if (!folder.empty()) drawLandings(folder + "/drift_landings.png", raws, fixeds, f.p);
  }
  {
    std::printf("Resting with a thumb on it (no help from the touch sensor)\n");
    drift::State st;
    double t = 0;
    float worst = 0;
    std::mt19937 rng(9);
    for (int i = 0; i < 200; ++i) {
      const float a = std::uniform_real_distribution<float>(0, 2 * kPi)(rng);
      worn.push(std::cos(a), std::sin(a));
      worn.letGo();
      worn.thumb = true;  // resting the thumb on it, not pushing
      float x, y, ox, oy;
      int touch;
      for (int k = 0; k < 50; ++k) {
        worn.report(x, y, touch);
        drift::fix(f.p, st, x, y, touch, t, ox, oy);
        t += 0.004;
        if (k > 2) worst = std::max(worst, drift::len(ox, oy));  // once it's landed
      }
    }
    check(worst == 0.f, f2("the dead zone alone holds it: worst the game sees is %.3f", worst));
    check(st.spikes > 20, f2("%.0f spikes held back, none reached the game", double(st.spikes)));
  }
  {
    std::printf("Pushing it\n");
    drift::State st;
    double t = 0;
    float worstFull = 1, worstAngle = 0, worstHalf = 0;
    bool rising = true;
    for (int d = 0; d < 16; ++d) {
      const float a = 2 * kPi * d / 16;
      float last = -1;
      for (int step = 1; step <= 20; ++step) {
        const float m = step / 20.f;
        worn.jitter = 0;
        worn.spikes = 0;
        worn.push(std::cos(a) * m, std::sin(a) * m);
        float x, y, ox, oy;
        int touch;
        worn.report(x, y, touch);
        drift::fix(f.p, st, x, y, touch, t, ox, oy);
        t += 0.004;
        drift::fix(f.p, st, x, y, touch, t, ox, oy);  // and the next report, the same: past the 6 ms a jump waits
        t += 0.004;
        const float o = drift::len(ox, oy);
        if (o < last - 1e-4f && std::getenv("DRIFT_DEBUG")) std::printf("        dir %d step %d: %.3f after %.3f (raw %.3f %.3f)\n", d, step, o, last, x, y);
        rising = rising && o >= last - 1e-4f;
        last = o;
        if (step == 20) {
          if (std::getenv("DRIFT_DEBUG")) std::printf("        dir %d full: %.3f at %.1f deg (raw %.3f %.3f)\n", d, o, std::atan2(oy, ox) * 180 / kPi, x, y);
          worstFull = std::min(worstFull, o);
          float e = std::fabs(std::atan2(oy, ox) - a);
          if (e > kPi) e = 2 * kPi - e;
          worstAngle = std::max(worstAngle, e * 180 / kPi);
        }
        if (step == 10) worstHalf = std::max(worstHalf, std::fabs(o - 0.5f));
      }
    }
    worn.jitter = 0.012f;
    worn.spikes = 0.005f;
    check(worstFull >= 0.99f, f2("a full push still reaches all the way, every way, the short left side too: least %.3f", worstFull));
    check(rising, "pushing further always gives more, no jump at the dead zone's edge");
    check(worstAngle < 10, f2("and points the way it's pushed: within %.1f degrees", worstAngle));
    check(worstHalf < 0.15f, f2("a half push reads about half: within %.2f", worstHalf));
  }
  {
    std::printf("Wandering while you play: thumb off, it slowly comes to rest somewhere new\n");
    drift::State st;
    double t = 0;
    worn.restX = -0.20f;
    worn.restY = 0.16f;  // 0.26 out now, past where the check found it
    worn.lastAngle = 0;
    worn.letGo();
    float x, y, ox, oy;
    int touch;
    float worst = 0;
    for (int k = 0; k < 2500; ++k) {  // ten seconds, thumb off
      worn.report(x, y, touch);
      drift::fix(f.p, st, x, y, touch, t, ox, oy);
      t += 0.004;
      worst = std::max(worst, drift::len(ox, oy));
    }
    check(worst == 0.f, "with the thumb off the game still gets the middle");
    worn.thumb = true;
    float touched = 0;
    for (int k = 0; k < 250; ++k) {
      worn.report(x, y, touch);
      drift::fix(f.p, st, x, y, touch, t, ox, oy);
      t += 0.004;
      if (k > 2) touched = std::max(touched, drift::len(ox, oy));
    }
    check(touched < 0.05f, f2("and once the thumb's back on, the middle has followed it there: %.3f", touched));
    worn.restX = -0.08f;
    worn.restY = 0.11f;
  }
  {
    std::printf("A touch sensor that misses (it never feels the thumb)\n");
    WornStick bad;
    bad.touchWorks = false;
    drift::State st;
    double t = 0;
    float x, y, ox, oy;
    int touch;
    // A flick and let go: the spring back takes a few reports with no thumb felt, and that's normal.
    bad.push(0, 1);
    for (int k = 0; k < 12; ++k) { bad.report(x, y, touch); drift::fix(f.p, st, x, y, touch, t, ox, oy); t += 0.004; }
    check(!st.touchBad, "a short flick with no thumb felt doesn't count against it");
    // Then really pushing it, and the sensor still says nobody's there.
    bad.push(0.5f, 0);
    float got = 0;
    for (int k = 0; k < 250; ++k) { bad.report(x, y, touch); drift::fix(f.p, st, x, y, touch, t, ox, oy); t += 0.004; got = ox; }
    check(st.touchBad, "a push held a quarter of a second with no thumb felt: the sensor isn't believed any more");
    check(got > 0.3f, f2("and the push gets through: %.2f", got));
  }
  {
    std::printf("A stick with no drift (magnetic sticks, a new controller)\n");
    WornStick good;
    good.restX = 0.004f;
    good.restY = -0.003f;
    good.weak = 0.008f;
    good.jitter = 0.004f;
    good.leftMost = 1.f;
    good.spikes = 0;  // a spike is a worn track jumping and coming straight back: a flick, which is drift since 0.6.4
    const drift::Findings g = checkStick(good, true);
    check(!g.drifts && !g.p.on && g.verdict == "No drift", "it says so, and leaves it alone: " + g.verdict);
    drift::State st;
    float ox, oy;
    drift::fix(g.p, st, 0.0123f, -0.456f, 1, 0, ox, oy);
    check(ox == 0.0123f && oy == -0.456f, "off is exactly what the stick said");
  }
  {
    std::printf("A controller that can't feel a thumb\n");
    WornStick worn2;
    const drift::Findings g = checkStick(worn2, false);
    check(g.drifts && !g.touchUsed && g.p.gate == 0 && g.p.learnPerSec == 0, "the fix still works, but doesn't go by touch or keep learning");
  }
  {
    // J4, 0.6.3 test build (16 November), his Quest 3 sticks over Virtual Desktop: one check said nothing to fix, the next
    // "only gets 64% of the way to the left" and "77% down and to the right". 0.6.3 then stopped short reach counting on a
    // stick that rests in the middle, taking his sticks for healthy ones. They aren't (J4, 18 November): they flick at
    // certain spots, which 0.6.3 never looked for. What still holds: one partial roll on a clean stick isn't worn.
    std::printf("A clean stick's partial roll still isn't worn\n");
    const auto fine = [] {
      WornStick w;
      w.restX = w.restY = 0;
      w.weak = 0;
      w.jitter = 0.004f;
      w.spikes = 0;
      w.leftMost = 1;
      return w;
    };
    const auto leftShort = [](float ang) { return std::cos(ang) < -0.8f ? 0.62f : 1.f; };
    const auto downRightShort = [](float ang) { return std::fabs(std::remainder(ang + kPi / 4, 2 * kPi)) < 0.5f ? 0.74f : 1.f; };
    WornStick left = fine();
    const drift::Findings a = checkStick(left, true, nullptr, How{leftShort, 1});
    check(!a.drifts && !a.p.on && a.verdict == "No drift", "a clean stick rolled 62% out to the left isn't called worn: " + a.verdict);
    check(a.flicks.empty(), f2("and has no flicks: %.0f", double(a.flicks.size())));
    WornStick right = fine();
    const drift::Findings b = checkStick(right, true, nullptr, How{downRightShort, 1});
    check(!b.drifts && !b.p.on && b.verdict == "No drift", "nor one rolled 74% out down and to the right: " + b.verdict);
    check(a.lines.size() == 2 && a.lines[0] == "Comes back to the middle", "and the page says it comes back to the middle, nothing about reach");
    // Step 3 pushes left, all the way: that's how far it really gets that way, whatever the roll did.
    check(a.reachSeen[4] > 0.95f, f2("step 3's push left got all the way, and that's what's kept for next time: %.2f", a.reachSeen[4]));
    // Step 3 never pushes down and to the right, so the roll is all there is that way.
    check(b.reachSeen[7] > 0.65f && b.reachSeen[7] < 0.8f, f2("what the roll saw down and to the right is kept for next time: %.2f", b.reachSeen[7]));
    // Short the same way on the next check of the same controller: that's no partial roll.
    WornStick again = fine();
    How twice{downRightShort, 1};
    twice.prevReach = b.reachSeen;
    const drift::Findings b2 = checkStick(again, true, nullptr, twice);
    check(b2.drifts && b2.worstDir == 7 && b2.worstReach < 0.8f, f2("short down and to the right again on its next check: that counts (%.2f)", b2.worstReach));
    // Rolled short to the left twice, stopping somewhere new each time (62%, then 70%), the way a thumb does: still not short,
    // not even up-left or down-left, which step 3 never pushes and the roll fell short of both times.
    WornStick leftAgain = fine();
    How twiceLeft{[](float ang) { return std::cos(ang) < -0.8f ? 0.70f : 1.f; }, 1};
    twiceLeft.prevReach = a.reachSeen;
    const drift::Findings a2 = checkStick(leftAgain, true, nullptr, twiceLeft);
    check(!a2.drifts, f2("rolled short to the left twice, %.0f%% then %.0f%%: still ", a.reachSeen[3] * 100, a2.reachSeen[3] * 100) + a2.verdict);
    // Pushed short in step 3 as well as rolled short: that counts too, on its own (the worn stick above, 84% to the left).
    check(f.worstDir == 4 && f.worstReach < 0.92f, f2("a stick really short to the left, rolled and pushed, is found short: %.2f", f.worstReach));
    // A stick that drifts is still fixed.
    WornStick drifter;
    const drift::Findings c = checkStick(drifter, true, nullptr, How{nullptr, 1});
    check(c.drifts && c.p.on, "a stick that drifts is still fixed: " + c.verdict);
    // One small jump and back, once: could be anything, so not worn. (One big one is: J4's below.)
    WornStick once = fine();
    once.spots = {{0.5f, 0.008f, 0.5f, 0.79f, 0.43f, 1.f, 1}};
    const drift::Findings o = checkStick(once, true, nullptr, How{nullptr, 1});
    check(o.flicks.size() == 1 && o.flicks[0].size < drift::kFlickBig && !o.drifts,
          f2("one small flick (%.2f) on its own isn't called worn: ", o.flicks.empty() ? 0.0 : o.flicks[0].size) + o.verdict);
  }
  {
    // J4, 18 November: his Quest 3 sticks rest at 0,0, but at certain spots as you circle them the reading flicks at light
    // speed to another point and straight back, and that's why a way never seems to reach 100%. Checked here the way his
    // 16 November check went: over Virtual Desktop (pairs every 14 ms), the left one rolled only 62% out to the left, the
    // right one 74% down and to the right.
    std::printf("J4, 0.6.3: real drift with flicks reads as drift\n");
    How vd;
    vd.dt = 0.014;
    vd.pairs = true;
    vd.laps = 1;
    vd.roll = [](float ang) { return std::cos(ang) < -0.8f ? 0.62f : 1.f; };
    WornStick left = j4Stick(false);
    const drift::Findings L = checkStick(left, true, nullptr, vd);
    vd.roll = [](float ang) { return std::fabs(std::remainder(ang + kPi / 4, 2 * kPi)) < 0.5f ? 0.74f : 1.f; };
    WornStick right = j4Stick(true);
    Run runR;
    const drift::Findings R = checkStick(right, true, &runR, vd);
    if (std::getenv("DRIFT_DEBUG")) {
      for (const drift::Flick& k : R.flicks) std::printf("        found from %.3f %.3f to %.3f %.3f at %.3f s, %.0f ms\n", k.fromX, k.fromY, k.toX, k.toY, k.t, k.ms);
      for (const std::vector<drift::Sample>* v : {&runR.edge, &runR.flicks})
        for (size_t i = 1; i < v->size(); ++i) {
          const drift::Sample &a = (*v)[i - 1], &b = (*v)[i];
          if (drift::len(b.x - a.x, b.y - a.y) > 0.08f) std::printf("        jump at %.3f s: %.3f %.3f -> %.3f %.3f\n", b.t, a.x, a.y, b.x, b.y);
        }
    }
    std::printf("        left: %d flicks made (%d reports), %.0f found, every %.0f ms\n", left.flicks, left.flicked, double(L.flicks.size()), L.gap * 1000);
    std::printf("        \"%s\"\n", L.verdict.c_str());
    for (const std::string& l : L.lines) std::printf("        \"%s\"\n", l.c_str());
    std::printf("        right: %d flicks made (%d reports), %.0f found\n", right.flicks, right.flicked, double(R.flicks.size()));
    std::printf("        \"%s\"\n", R.verdict.c_str());
    for (const std::string& l : R.lines) std::printf("        \"%s\"\n", l.c_str());
    check(L.drifts && L.p.on && L.flicky, "J4, 0.6.3: a stick that rests in the middle but flicks reads as drift: " + L.verdict);
    check(R.drifts && R.p.on && R.flicky, "J4, 0.6.3: and so does the other one: " + R.verdict);
    check(L.offset < 0.02f && R.offset < 0.02f, f2("both rest in the middle (%.3f, %.3f), so it's the flicks that count", L.offset, R.offset));
    check(L.flicks.size() == size_t(left.flicks) && R.flicks.size() == size_t(right.flicks) && right.flicks >= 1,
          f2("every flick is found, and nothing else: %.0f on the left, %.0f on the right", double(L.flicks.size()), double(R.flicks.size())));
    check(R.flicks.size() == 1, "the right one flicked only once in one slow roll, and that's enough");
    int wrongWay = 0;
    for (const drift::Flick& k : L.flicks) {
      const int w = drift::wayOf(std::atan2(k.fromY, k.fromX));
      wrongWay += w != 5 && w != 6;
    }
    for (const drift::Flick& k : R.flicks) wrongWay += drift::wayOf(std::atan2(k.fromY, k.fromX)) != 7;
    check(wrongWay == 0, f2("and found where they are: down or down-left on the left one, down-right on the right one (%.0f elsewhere)", wrongWay));
    check(!L.lines.empty() && L.lines[0].rfind("Jumps at ", 0) == 0, "the page says the left one jumps: " + (L.lines.empty() ? "" : L.lines[0]));
    check(!R.lines.empty() && R.lines[0] == "Jumps at 1 spot: down-right.", "and the right one: " + (R.lines.empty() ? "" : R.lines[0]));
    // Reach: the left one was rolled short to the left, but its step 3 pushes got all the way, so it isn't short there. The
    // right one has no step 3 push down and to the right, and flicks there: its short roll counts.
    check(!(L.worstDir == 4 && L.worstReach < 0.9f) && L.reachSeen[4] > 0.95f, f2("left: rolled short but pushed all the way left, so not short (%.2f)", L.reachSeen[4]));
    check(R.worstDir == 7 && R.worstReach < 0.8f, f2("right: short down and to the right, where it flicks, counts again (%.2f)", R.worstReach));
    check(R.lines.size() >= 2 && R.lines[R.lines.size() - 2].rfind("Only gets 7", 0) == 0 && R.lines.back() == "down and to the right.",
          "and says so, on two lines: " + (R.lines.size() >= 2 ? R.lines[R.lines.size() - 2] + " / " + R.lines.back() : std::string()));
    check(drift::any(L.fl) && L.fl.big == drift::kHoldBig && L.fl.most == drift::kHoldMost, f2("the driver is told to hold back jumps of %.2f or more, for %.0f reports at most", L.fl.big, L.fl.most));
    check(L.lines.size() <= 7 && R.lines.size() <= 7, "and it's few enough lines to fit beside the stick");

    std::printf("J4, 0.6.3: a flick is held back live\n");
    for (int h = 0; h < 2; ++h) {
      WornStick s = j4Stick(h == 1);
      s.rng.seed(77 + h);
      const drift::Findings& F = h ? R : L;
      const drift::Params none{};
      drift::Params on063 = F.p;  // the same fix without the map: 0.6.3's
      const Live raw = liveRun(s, none, drift::Flicks{}, 4, 300);
      s.rng.seed(77 + h);
      const Live old = liveRun(s, on063, drift::Flicks{}, 4, 300);
      s.rng.seed(77 + h);
      const Live now = liveRun(s, F.p, F.fl, 4, 300);
      std::printf("        %s: %d flick reports; the game jumps up to %.2f unfixed, %.2f with 0.6.3's fix, %.3f now; held %d of %d other reports\n",
                  h ? "right" : "left", now.flickReports, raw.worstOut, old.worstOut, now.worstOut, now.held, now.reports);
      check(now.flickReports >= 5 && raw.worstOut > 0.2f && old.worstOut > 0.2f, f2("J4, 0.6.3: unfixed and with 0.6.3's fix the game sees the flicks (%.2f, %.2f)", raw.worstOut, old.worstOut));
      check(now.worstOut < 0.05f, f2("J4, 0.6.3: a flick is held back live: the game moves %.3f at most on one", now.worstOut));
      check(now.counted > 0, f2("and they're counted: %.0f", double(now.counted)));
      check(now.held * 20 < now.reports, f2("the stick isn't held up elsewhere: %.0f reports of %.0f", double(now.held), double(now.reports)));
    }
    // A push from the middle and a quick roll round the way it doesn't flick: never held.
    {
      WornStick s = j4Stick(false);
      s.spots.clear();
      drift::State st;
      double t = 0;
      int held = 0;
      for (int i = 0; i < 400; ++i) {
        const float a = kPi / 2 - kPi / 2 * std::min(1.f, i / 60.f), m = std::min(1.f, i / 8.f);  // out, then a quarter round fast
        s.push(std::cos(a) * m, std::sin(a) * m);
        float x, y, ox, oy;
        int touch;
        s.report(x, y, touch);
        drift::fix(L.p, L.fl, st, x, y, touch, t, ox, oy);
        held += st.holdT >= 0;
        t += 0.014;
      }
      check(held == 0, f2("a push and a quick roll where it doesn't flick is never held up: %.0f reports", double(held)));
    }
    // A full push still reaches all the way, every way, on both.
    for (int h = 0; h < 2; ++h) {
      const drift::Findings& F = h ? R : L;
      float least = 9;
      for (int d = 0; d < 16; ++d) {
        const float a = 2 * kPi * d / 16;
        drift::State st;
        float ox = 0, oy = 0;
        for (int k = 0; k < 6; ++k) drift::fix(F.p, F.fl, st, std::cos(a) * (k + 1) / 6.f, std::sin(a) * (k + 1) / 6.f, 1, k * 0.014, ox, oy);
        drift::fix(F.p, F.fl, st, std::cos(a), std::sin(a), 1, 0.2, ox, oy);
        least = std::min(least, drift::len(ox, oy));
      }
      check(least >= 0.99f, f2("a full push still reaches all the way, every way: least %.3f", least));
    }
  }
  {
    std::printf("Nothing silly from the file\n");
    drift::Params p;
    p.on = 1;
    p.cx = NAN;
    p.cy = 9;
    p.dead = -3;
    p.gate = INFINITY;
    p.reach[0] = 1e9f;
    const drift::Params q = drift::sane(p);
    check(q.cx == 0 && q.cy == 0.5f && q.dead == 0 && q.gate == 0 && q.reach[0] == 1.6f, "NaN, huge and negative numbers are held to sane ones");
    drift::State st;
    float ox, oy;
    drift::fix(q, st, 0.2f, 0.9f, 1, 0, ox, oy);
    check(std::isfinite(ox) && std::isfinite(oy) && drift::len(ox, oy) <= 1.0001f, "and what comes out is a real stick position");
  }
  {
    const double ns = nsPerFix(), nsF = nsPerFix(true);
    check(ns < 200, f2("costs %.0f ns a report", ns));
    check(nsF < 250, f2("and %.0f ns holding flicks back", nsF));
  }
  pageCheck(folder);
  pageFlicks(folder);
  oldFile();
  realSticks(folder);
  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}

// Stick drift, measured on made-up worn sticks: the stick check run on them the way the page runs it, then what a game gets
// with and without the fix, for each thing the page promises.
//
// The worn stick is built the way a worn potentiometer stick behaves: it rests off the middle; its spring is weak, so it
// lands short on the side it came back from, a different spot each time; it jitters a little, with the odd spike; and it no
// longer gets all the way out to one side. A thumb pushing it means a position; letting go sends it back to a landing spot.
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
    if (std::uniform_real_distribution<float>(0, 1)(rng) < spikes) x += 0.15f;  // the odd spike from a worn track
    if (x < -leftMost) x = -leftMost;
    const float r = std::sqrt(x * x + y * y);
    if (r > 1) { x /= r; y /= r; }
    touch = touchWorks ? (thumb ? 1 : 0) : 0;
  }
};

// Runs the three steps of the stick check on it, the way a person would, and works out what to send.
struct Run { std::vector<drift::Sample> rest, edge, flicks; };
// roll: how far out the thumb gets each way while rolling round the edge (all the way, unless it says), laps: how many times.
drift::Findings checkStick(WornStick& s, bool hasTouch, Run* keep = nullptr, float (*roll)(float angle) = nullptr, int laps = 3) {
  Run run;
  double t = 0;
  const double dt = 1.0 / 250;  // reports four milliseconds apart
  auto take = [&](std::vector<drift::Sample>& into) {
    float x, y;
    int touch;
    s.report(x, y, touch);
    into.push_back({x, y, hasTouch ? touch : -1, t});
    t += dt;
  };
  // 1: left alone for four seconds.
  s.letGo();
  for (int i = 0; i < 1000; ++i) take(run.rest);
  const drift::Rest rest = drift::restOf(run.rest);
  // 2: round the edge, three times, slowly.
  for (int i = 0; i < laps * 400; ++i) {
    const float a = 2 * kPi * i / 400.f, m = roll ? roll(a) : 1.f;
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
  return drift::analyse(rest, run.edge, landed, hasTouch, false);
}

double nsPerFix() {
  drift::Params p;
  p.on = 1; p.cx = -0.08f; p.cy = 0.11f; p.dead = 0.1f; p.gate = 0.25f; p.learnPerSec = 0.5f; p.learnMax = 0.2f;
  for (int k = 0; k < drift::kDirs; ++k) p.reach[k] = 0.9f;
  drift::State st;
  volatile float sink = 0;
  const int n = 2000000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    float ox, oy;
    drift::fix(p, st, 0.3f * std::sin(i * 0.001f), 0.3f * std::cos(i * 0.0013f), i & 1, i * 0.002, ox, oy);
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
  // One report of a hand's stick: fixed, and kept for the page while it's measuring. What the game gets.
  drift::Dot report(int h, float x, float y, int touch, double t) {
    if (dc->seq.load() != seen) {
      seen = dc->seq.load();
      for (int k = 0; k < 2; ++k) {
        drift::Params q;
        std::memcpy(&q, &dc->want[k], sizeof q);
        p[k] = drift::sane(q);
        st[k] = drift::State{};
      }
    }
    float ox, oy;
    drift::fix(p[h], st[h], x, y, touch, t, ox, oy);
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
    drv.dc->magic.store(fhlive::kDriftMagic);
  }
  fs::remove_all(dir, ec);
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
    // J4, 0.6.3 test build (16 November), his healthy Quest 3 sticks over Virtual Desktop: one check said nothing to fix, the
    // next "only gets 64% of the way to the left" and "77% down and to the right", from a quick roll that didn't go all the
    // way out. A stick that rests and lands in the middle is left alone, whatever the roll: not reaching only counts on a stick
    // that drifts.
    std::printf("J4, 0.6.3 test build: healthy sticks, rolled quickly and not all the way out in one place\n");
    const auto fine = [] {
      WornStick w;
      w.restX = w.restY = 0;
      w.weak = 0;
      w.jitter = 0.004f;
      w.spikes = 0;
      w.leftMost = 1;
      return w;
    };
    WornStick left = fine();
    const drift::Findings a = checkStick(left, true, nullptr, [](float ang) { return std::cos(ang) < -0.8f ? 0.62f : 1.f; }, 1);
    check(!a.drifts && !a.p.on && a.verdict == "No drift", "J4, 0.6.3: a healthy left stick rolled 62% out to the left isn't called worn: " + a.verdict);
    WornStick right = fine();
    const drift::Findings b = checkStick(right, true, nullptr, [](float ang) {
      return std::fabs(std::remainder(ang + kPi / 4, 2 * kPi)) < 0.5f ? 0.74f : 1.f; }, 1);
    check(!b.drifts && !b.p.on && b.verdict == "No drift", "J4, 0.6.3: a healthy right stick rolled 74% out down and to the right isn't either: " + b.verdict);
    check(a.lines.size() == 2 && a.lines[0] == "Comes back to the middle", "and the page says it comes back to the middle, nothing about reach");

    // A stick that drifts is still fixed, and still made up for where it's short.
    WornStick drifter;
    const drift::Findings c = checkStick(drifter, true, nullptr, nullptr, 1);
    check(c.drifts && c.p.on, "J4, 0.6.3: a stick that drifts is still fixed: " + c.verdict);

    // The worn stick above really is short to the left: its flicks are short too, so that's still found.
    check(f.worstDir == 4 && f.worstReach < 0.92f, f2("a stick really short to the left is still found short: %.2f", f.worstReach));
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
    const double ns = nsPerFix();
    check(ns < 200, f2("costs %.0f ns a report", ns));
  }
  pageCheck(folder);
  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}

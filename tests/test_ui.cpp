// Renders every UTMTFTM screen to PNG (for review) and checks the UI text logic.
// Usage: build/test_ui <out-dir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "glove.h"
#include "objload.h"
#include "placement.h"
#include "platform.h"
#include "ui.h"

using namespace fh;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static Offset off(float f, float u, float r, float p, float y, float rl) {
  Offset o; o.pos[0] = r / 100; o.pos[1] = u / 100; o.pos[2] = -f / 100; o.rot[0] = p; o.rot[1] = y; o.rot[2] = rl;
  return o;
}

// The screens below show a real controller when SteamVR's files are here, or the generic one when they aren't.
static void useFrameController() {
  for (int left = 0; left < 2; ++left) {
    std::string side = left ? "left" : "right", dir = platform::steamvrDir() + "/drivers/frame_controller/resources/rendermodels/frame_controller_" + side;
    Mesh raw;
    if (!loadObj(dir + "/frame_controller_" + side + ".obj", raw)) continue;
    setControllerModel(left != 0, raw, mul(translate(left ? -0.003117f : 0.003117f, -0.004277f, 0.099501f), rotX(2.8091f)));  // its openxr_grip
  }
}

int main(int argc, char** argv) {
  std::string out = argc > 1 ? argv[1] : ".";

  // the preview shows whichever controller SteamVR reports, and each hand's own
  {
    Canvas generic(256, 256), left(256, 256), right(256, 256), rightAgain(256, 256);
    drawHandPreview(generic, 0, 0, 256, true, Offset{}, Offset{}, 1, 0.824, 0);
    std::vector<uint8_t> before = generic.toRGBA();
    Mesh ball = buildSphere(0.07f);
    setControllerModel(true, ball, ident());
    drawHandPreview(left, 0, 0, 256, true, Offset{}, Offset{}, 1, 0.824, 0);
    CHECK(left.toRGBA() != before);
    drawHandPreview(right, 0, 0, 256, false, Offset{}, Offset{}, 1, 0.824, 0);
    std::vector<uint8_t> rightBefore = right.toRGBA();
    Mesh farBall = ball;
    setControllerModel(false, farBall, translate(0, 0, 0.05f));  // the grip sits somewhere else on this one
    drawHandPreview(rightAgain, 0, 0, 256, false, Offset{}, Offset{}, 1, 0.824, 0);
    CHECK(rightAgain.toRGBA() != rightBefore);
  }
  useFrameController();

  // text logic
  CHECK(friendlyOffset(Offset{}) == "No offset");
  CHECK(friendlyOffset(off(3.2f, 0, 0, 0, 0, 0)) == "3.2 cm forward");
  CHECK(friendlyOffset(off(0, -1, 0, 0, 0, 0)) == "1.0 cm down");
  CHECK(friendlyOffset(off(2, 1, 0, 0, 30, 0)).find("turned 30") != std::string::npos);

  // dashboard: live game, custom left, right on global default, ghost matched
  {
    Canvas cv(1200, 760);
    DashModel m;
    m.game = "The Light Brigade: Definitive Edition";
    m.live = LiveState::Live;
    m.cards[0].applied = off(3.2f, -1.0f, 0.5f, 12, 0, -8); m.cards[0].badge = Badge::GameCustom;
    m.cards[1].applied = Offset{}; m.cards[1].badge = Badge::GameCustom;
    m.gameHasCustom = true;
    m.needsMatch = false;
    std::vector<Button> b;
    drawDashboard(cv, m, b);
    cv.savePNG(out + "/dash_live.png");
    bool hasClose = false, hasPlace = false, resetRightEnabled = false;
    for (auto& x : b) { hasClose |= x.id == "close"; hasPlace |= x.id == "place_left"; resetRightEnabled |= x.id == "reset_right" && x.enabled; }
    CHECK(hasClose); CHECK(hasPlace); CHECK(!resetRightEnabled);  // nothing to reset on the right
    for (auto& x : b) CHECK(x.x >= 0 && x.y >= 0 && x.x + x.w <= cv.w && x.y + x.h <= cv.h);
    // dashKey must change when an applied value changes (this is what drives redraws)
    DashModel m2 = m; m2.cards[0].applied.pos[2] -= 0.001f;
    CHECK(dashKey(m) != dashKey(m2));
  }

  // Decaffeinate: its own tab, with the shake meter.
  {
    auto inBounds = [](const std::vector<Button>& b, const Canvas& cv) {
      bool ok = true;
      for (auto& x : b) ok = ok && x.x >= 0 && x.y >= 0 && x.x + x.w <= cv.w && x.y + x.h <= cv.h;
      return ok;
    };
    auto has = [](const std::vector<Button>& b, const std::string& id) {
      for (auto& x : b) if (x.id == id) return true;
      return false;
    };
    {
      Canvas cv(1200, 760);
      DecafModel m;
      m.game = "Pavlov";
      m.live = LiveState::Live;
      m.level = 2;
      m.hands[0] = {true, 0.31f, 0.038f};
      m.hands[1] = {true, 0.27f, 0.035f};
      std::vector<Button> b;
      drawDecaf(cv, m, b);
      cv.savePNG(out + "/decaf_strong.png");
      CHECK(inBounds(b, cv));
      for (int i = 0; i < kDecafSteps; ++i) CHECK(has(b, "decaf_" + std::to_string(i)));
      CHECK(has(b, "page_decaf") && has(b, "scope_game") && has(b, "scope_global"));
      DecafModel m2 = m; m2.hands[0].out = 0.05f;
      CHECK(decafKey(m) != decafKey(m2));  // the meter moving redraws the page

      DecafModel offM = m;
      offM.level = 0;
      offM.hands[1] = {};
      drawDecaf(cv, offM, b);
      cv.savePNG(out + "/decaf_off.png");

      DecafModel own = m;
      own.game = "H3VR";
      own.level = 1;
      own.gameHasOwn = true;
      own.hover = "decaf_3";
      drawDecaf(cv, own, b);
      cv.savePNG(out + "/decaf_own_hover.png");
      CHECK(has(b, "decaf_use_global"));

      DecafModel old = m;
      old.ready = false;
      old.hands[0] = old.hands[1] = {};
      drawDecaf(cv, old, b);
      cv.savePNG(out + "/decaf_needs_restart.png");

      DashModel d;
      d.game = "The Light Brigade: Definitive Edition";
      d.live = LiveState::Live;
      d.cards[0].applied = off(3.2f, -1.0f, 0.5f, 12, 0, -8); d.cards[0].badge = Badge::GameCustom;
      d.cards[1].badge = Badge::GameCustom;
      d.gameHasCustom = true;
      drawDashboard(cv, d, b);
      cv.savePNG(out + "/decaf_hands_tab.png");
      CHECK(inBounds(b, cv) && has(b, "page_decaf"));
    }
  }
  // fine adjust: arrows either side of every number, on both hands, all on the panel, and each says what it does
  {
    Canvas cv(1200, 760);
    DashModel m;
    m.game = "The Light Brigade: Definitive Edition";
    m.live = LiveState::Live;
    m.cards[0].applied = off(3.2f, -1.0f, 0.5f, 12, 3, -8); m.cards[0].badge = Badge::GameCustom;
    m.cards[1].applied = Offset{}; m.cards[1].badge = Badge::GameCustom;
    std::vector<Button> b;
    // The globes are as they were in 0.4.0: no See-through button, and the all-games button has the line to itself.
    m.gameHasCustom = true;
    drawDashboard(cv, m, b);
    cv.savePNG(out + "/dash_use_global.png");
    CHECK(std::none_of(b.begin(), b.end(), [](const Button& x) { return x.id == "see_through"; }));
    CHECK(std::any_of(b.begin(), b.end(), [](const Button& x) { return x.id == "use_global"; }));
    m.gameHasCustom = false;
    m.fine = true;
    drawDashboard(cv, m, b);
    cv.savePNG(out + "/dash_fine.png");
    int arrows = 0;
    for (auto& x : b) {
      int hand = 0, axis = 0, dir = 0;
      if (parseNudge(x.id, hand, axis, dir)) {
        ++arrows;
        CHECK(!nudgeHint(x.id, false).empty());
      }
      CHECK(x.x >= 0 && x.y >= 0 && x.x + x.w <= cv.w && x.y + x.h <= cv.h);
    }
    CHECK(arrows == 24);
    // no two tappable things overlap: a miss on these small ones would nudge the wrong number
    for (size_t i = 0; i < b.size(); ++i)
      for (size_t j = i + 1; j < b.size(); ++j)
        if (b[i].id.rfind("nudge_", 0) == 0 || b[j].id.rfind("nudge_", 0) == 0)
          CHECK(b[i].x + b[i].w <= b[j].x || b[j].x + b[j].w <= b[i].x || b[i].y + b[i].h <= b[j].y || b[j].y + b[j].h <= b[i].y);
    CHECK(nudgeHint("nudge_0_4_m", false) == "Turns your left hand 1\xC2\xB0 to the left.");
    CHECK(nudgeHint("nudge_1_0_p", true) == "Moves your right hand 1 cm forward.");
    CHECK(nudgeHint("place_left", false).empty());
    int h = 0, a = 0, d = 0;
    CHECK(!parseNudge("nudge_2_0_p", h, a, d) && !parseNudge("nudge_0_6_p", h, a, d) && !parseNudge("nudge_0_0_x", h, a, d) && !parseNudge("page_hands", h, a, d));
    // and the redraw key notices
    DashModel m2 = m; m2.fineBig = true;
    CHECK(dashKey(m) != dashKey(m2));
    m2 = m; m2.hint = "x";
    CHECK(dashKey(m) != dashKey(m2));
    // a hint on the bottom line, the big step, and the arrows going away again
    m.fineBig = true;
    m.hover = "nudge_0_4_p";
    m.hint = nudgeHint(m.hover, true);
    drawDashboard(cv, m, b);
    cv.savePNG(out + "/dash_fine_hint.png");
    m.fine = false;
    m.hint.clear();
    drawDashboard(cv, m, b);
    for (auto& x : b) CHECK(x.id.rfind("nudge_", 0) != 0);
    // with no game the words change, and it still has a way out
    DashModel nog;
    nog.saveForGame = false;
    nog.fine = true;
    drawDashboard(cv, nog, b);
    cv.savePNG(out + "/dash_fine_nogame.png");
    bool done = false;
    for (auto& x : b) done |= x.id == "fine_toggle";
    CHECK(done);
  }
  // dashboard: no game, needs match, confirm close
  {
    Canvas cv(1200, 760);
    DashModel m;
    m.saveForGame = false;
    m.needsMatch = true;
    m.confirmClose = true;
    m.cards[0].badge = m.cards[1].badge = Badge::AllGamesDefault;
    m.cards[1].applied = off(0, 2, -1, 0, -20, 0);
    std::vector<Button> b;
    drawDashboard(cv, m, b);
    cv.savePNG(out + "/dash_nogame.png");
    bool scopeGame = false;
    for (auto& x : b) scopeGame |= x.id == "scope_game" && x.enabled;
    CHECK(!scopeGame);  // can't save "for this game" with no game running
    bool match = false;
    for (auto& x : b) match |= x.id == "match";
    CHECK(!match);  // no game, so there's no in-game hand to match a ghost to
  }
  // waiting on the dashboard: the next step is the loudest thing on screen, and it can be backed out of
  {
    Canvas cv(1200, 760);
    DashModel m;
    m.game = "The Light Brigade: Definitive Edition";
    m.live = LiveState::Live;
    m.armed = true;
    m.cards[0].badge = m.cards[1].badge = Badge::GameCustom;
    std::vector<Button> b;
    drawDashboard(cv, m, b);
    cv.savePNG(out + "/dash_armed.png");
    auto has = [&](const char* id) { for (auto& x : b) if (x.id == id) return true; return false; };
    CHECK(has("cancel_armed"));
    m.armed = false;
    drawDashboard(cv, m, b);
    CHECK(!has("cancel_armed"));
  }
  // a game that can't move live: it offers the ghost match
  {
    Canvas cv(1200, 760);
    DashModel m;
    m.game = "No Man's Sky";
    m.live = LiveState::NextLaunch;
    m.needsMatch = true;
    m.cards[0].badge = m.cards[1].badge = Badge::GlobalDefault;
    std::vector<Button> b;
    drawDashboard(cv, m, b);
    cv.savePNG(out + "/dash_nextlaunch.png");
    bool match = false;
    for (auto& x : b) match |= x.id == "match";
    CHECK(match);
  }
  // panels
  {
    Canvas cv(820, 300);
    PanelModel p;
    p.header = "LEFT HAND"; p.progress = "1 / 2"; p.chip = "GRIP"; p.text = "Grab the ghost with your right hand";
    p.status = "No offset yet"; p.keys = {{"A / X", "No offset"}, {"B / Y", "Cancel"}};
    drawPanel(cv, p); cv.savePNG(out + "/panel_grab.png");
    p.chip = ""; p.text = "Move it where your left hand should be, then let go"; p.status = friendlyOffset(off(3.2f, -1, 0, 12, 0, 0));
    drawPanel(cv, p); cv.savePNG(out + "/panel_move.png");
    p.warn = true; p.status = "Past the 15 cm limit. It'll pull back when you let go";
    drawPanel(cv, p); cv.savePNG(out + "/panel_warn.png");
    p.warn = false; p.chip = "TRIGGER"; p.text = "Lock it in"; p.status = friendlyOffset(off(3.2f, -1, 0, 12, 0, 0));
    p.keys = {{"GRIP", "Adjust again"}, {"A / X", "No offset"}, {"B / Y", "Cancel"}};
    drawPanel(cv, p); cv.savePNG(out + "/panel_lock.png");
    p.keys = {{"GRIP", "Adjust again"}, {"Trackpad", "No offset"}, {"Menu", "Cancel"}};  // the longest names any controller has
    drawPanel(cv, p); cv.savePNG(out + "/panel_lock_wand.png");
    PanelModel q;
    q.match = true; q.header = "MATCH GHOST  \xC2\xB7  LEFT HAND"; q.chip = "GRIP"; q.text = "Grab the ghost with your right hand";
    q.status = "Then line it up exactly with your in-game left hand"; q.keys = {{"A / X", "Reset ghost"}, {"B / Y", "Cancel"}};
    drawPanel(cv, q); cv.savePNG(out + "/panel_match.png");
    drawMessage(cv, "Saved for The Light Brigade", "Your hands are live in the game now.", true);
    cv.savePNG(out + "/panel_saved.png");
  }
  // the limit bubble as one eye sees it (controller 45 cm ahead, hand pulled 9 cm forward-left)
  {
    int S = 360;
    GhostRenderer g(S, S);
    float r = 0.15f;
    V3 c{-0.12f, 1.25f, -0.45f}, head{0, 1.6f, 0}, a = c + V3{-0.04f, 0.02f, -0.08f};
    V3 N = norm(head - c), up{0, 1, 0}, X = norm(cross(up, N)), Y = cross(N, X);
    GhostRenderer::Plane pl{c, X, Y, N, bubblePlaneSize(r)};
    V3 eyes[1] = {head};
    Mesh sph = buildSphere(r), dot1 = buildSphere(0.012f, 8, 12), dot2 = buildSphere(0.007f, 6, 10);
    g.clear();
    g.drawMesh(dot2, translate(c.x, c.y, c.z), pl, eyes, 1, 1.6f);
    g.drawMesh(dot1, translate(a.x, a.y, a.z), pl, eyes, 1, 3.0f);
    g.drawTether(pl, eyes, 1, c, a, 0.6f);
    g.drawMesh(sph, translate(c.x, c.y, c.z), pl, eyes, 1, 0.85f, true);
    Canvas cv(S, S);
    cairo_set_source_rgb(cv.c, 0.20, 0.17, 0.14); cairo_paint(cv.c);  // stand-in for a game scene
    uint8_t* d = cairo_image_surface_get_data(cv.s);
    int st = cairo_image_surface_get_stride(cv.s);
    for (int y = 0; y < S; ++y)
      for (int x = 0; x < S; ++x) {
        float p = g.rgba[(size_t(y) * 2 * S + x) * 4] / 255.f;  // premultiplied white, tinted the resting colour
        uint8_t* q = d + y * st + x * 4;
        float tint[3] = {0.925f, 0.925f, 0.905f};
        for (int i = 0; i < 3; ++i) q[2 - i] = (uint8_t)std::min(255.f, q[2 - i] * (1 - p) + 255 * tint[i] * p);
      }
    cairo_surface_mark_dirty(cv.s);
    cv.savePNG(out + "/bubble.png");
  }
  // live dashboard (bubble info bar)
  {
    Canvas cv(1200, 760);
    DashModel m;
    m.game = "The Light Brigade: Definitive Edition";
    m.live = LiveState::Live;
    m.cards[0].applied = off(4.0f, 1.0f, 0, 0, 0, 25); m.cards[0].badge = Badge::GameCustom;
    m.cards[1].badge = Badge::GameCustom;
    std::vector<Button> b;
    drawDashboard(cv, m, b);
    cv.savePNG(out + "/dash_live_bubble.png");
    bool match = false;
    for (auto& x : b) match |= x.id == "match";
    CHECK(!match);  // live games don't need a ghost, so no match button
  }
  // Press feedback: every tap must visibly change the screen. Static states only, no animation.
  {
    auto render = [](const DashModel& m, std::vector<Button>& b) {
      static Canvas cv(1200, 760);
      drawDashboard(cv, m, b);
      return cv.toRGBA();
    };
    auto changed = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const Button& r) {
      int n = 0;  // pixels that differ inside this button's box
      for (int y = (int)r.y; y < (int)(r.y + r.h); ++y)
        for (int x = (int)r.x; x < (int)(r.x + r.w); ++x)
          for (int i = 0; i < 3; ++i) if (a[(size_t(y) * 1200 + x) * 4 + i] != b[(size_t(y) * 1200 + x) * 4 + i]) { ++n; break; }
      return n;
    };
    auto find = [](const std::vector<Button>& b, const std::string& id) {
      for (auto& x : b) if (x.id == id) return x;
      return Button{0, 0, 0, 0, "", false};
    };
    DashModel m;
    m.game = "The Light Brigade: Definitive Edition";
    m.live = LiveState::Live;
    m.cards[0].applied = off(3.2f, -1.0f, 0.5f, 12, 0, -8); m.cards[0].badge = Badge::GameCustom;
    m.cards[1].badge = Badge::GameCustom;
    m.gameHasCustom = true;
    std::vector<Button> b0, b1;
    auto base = render(m, b0);
    Button rl = find(b0, "reset_left"), rr = find(b0, "reset_right"), l30 = find(b0, "lim_30");
    CHECK(rl.enabled);   // left has an offset: Reset is live
    CHECK(!rr.enabled);  // right has none: Reset is greyed...
    CHECK(!rr.id.empty() && hitButton(b0, rr.x + 5, rr.y + 5) && !hitButton(b0, rr.x + 5, rr.y + 5)->enabled);  // ...but still answers taps
    CHECK(!hitButton(b0, 2, 2));
    // hover and held-down each change the button's pixels, and the redraw key
    for (const char* id : {"reset_left", "place_left", "lim_30", "scope_global", "close"}) {
      Button k = find(b0, id);
      DashModel h = m; h.hover = id;
      DashModel d = m; d.hover = id; d.down = id;
      auto ph = render(h, b1), pd = render(d, b1);
      CHECK(changed(base, ph, k) > 50);
      CHECK(changed(ph, pd, k) > 50);
      CHECK(dashKey(h) != dashKey(m) && dashKey(d) != dashKey(h));
    }
    // what each action does shows on the screen itself
    DashModel r = m; r.cards[0].applied = Offset{};  // after Reset left
    auto pr = render(r, b1);
    CHECK(!find(b1, "reset_left").enabled);  // greys out once used
    CHECK(changed(base, pr, rl) > 200);
    DashModel lm = m; lm.maxCm = 30;  // after tapping 30 cm
    auto pl = render(lm, b1);
    CHECK(changed(base, pl, l30) > 200);
    DashModel sm = m; sm.status = "Left hand reset. Live in the game now."; sm.statusGood = true;
    CHECK(dashKey(sm) != dashKey(m));
    // DashFx: hover follows the laser, press clears on release and when the laser leaves
    DashFx fx;
    fx.move("place_left"); fx.press("place_left");
    DashModel f = m; fx.apply(f);
    CHECK(f.hover == "place_left" && f.down == "place_left");
    fx.release(); fx.apply(f);
    CHECK(f.down.empty() && f.hover == "place_left");
    fx.press("lim_10"); fx.leave(); fx.apply(f);
    CHECK(f.down.empty() && f.hover.empty());
    // PNGs for review
    Canvas cv(1200, 760);
    DashModel h = m; h.hover = "place_left";
    drawDashboard(cv, h, b1); cv.savePNG(out + "/dash_hover.png");
    h.down = "place_left";
    drawDashboard(cv, h, b1); cv.savePNG(out + "/dash_pressed.png");
    DashModel g = r; g.hover = "reset_left"; g.status = "Left hand reset. Live in the game now."; g.statusGood = true;
    drawDashboard(cv, g, b1); cv.savePNG(out + "/dash_after_reset.png");
    DashModel e = r; e.hover = "reset_left"; e.status = "Left hand has no offset, so there's nothing to reset.";
    drawDashboard(cv, e, b1); cv.savePNG(out + "/dash_greyed_tap.png");
  }
  // Previews: the real left/right controller is drawn, and nothing (hand or controller) is ever cut off
  // at the preview's edge, for any offset up to the 30 cm limit.
  {
    const int S = 184;
    Canvas cv(S, S);
    int clipped = 0, empty = 0;
    for (int i = 0; i < 400; ++i) {
      bool left = i % 2;
      Offset o;
      if (i >= 2) {
        float r = 0.30f * (i % 7) / 6.f;
        float t = i * 2.399f, z = std::fmod(i * 0.618f, 2.f) - 1.f;
        o.pos[0] = r * std::sqrt(1 - z * z) * std::cos(t); o.pos[1] = r * z; o.pos[2] = r * std::sqrt(1 - z * z) * std::sin(t);
        o.rot[0] = std::fmod(i * 37.f, 360.f) - 180; o.rot[1] = std::fmod(i * 53.f, 360.f) - 180; o.rot[2] = std::fmod(i * 71.f, 360.f) - 180;
      }
      cairo_set_source_rgb(cv.c, 1, 0, 1); cairo_paint(cv.c);
      drawHandPreview(cv, 0, 0, S, left, o, Offset{}, 1.0, 0.824, 0.0);
      const auto& px = cv.toRGBA();
      auto lit = [&](int x, int y) { const uint8_t* p = &px[(size_t(y) * S + x) * 4]; return p[0] + p[1] + p[2] > 3 * 30; };
      bool edge = false; int on = 0;
      for (int k = 20; k < S - 20; ++k)  // the border band, 3 px deep
        for (int d = 0; d < 3; ++d) edge |= lit(k, d) || lit(k, S - 1 - d) || lit(d, k) || lit(S - 1 - d, k);
      for (int y = 0; y < S; y += 2) for (int x = 0; x < S; x += 2) on += lit(x, y);
      clipped += edge; empty += on < 200;
    }
    CHECK(clipped == 0);
    CHECK(empty == 0);
    if (clipped) printf("  %d of 400 previews touch the edge\n", clipped);
  }
  {
    Canvas ic(128, 128);
    drawIcon(ic); ic.savePNG(out + "/icon.png");
    Canvas big(256, 256);
    drawIcon(big); big.savePNG(out + "/icon_256.png");
    // the ghost hand as the headset's overlay draws it, over a stand-in for a game scene
    const int G = 432;
    GhostRenderer gh(G, G);
    V3 head{-0.14f, 0.34f, -0.26f};  // your eyes, looking down at your right hand
    M pose = ident();
    V3 N = norm(head), up = norm(V3{0, -0.15f, -1}), X = norm(cross(up, N)), Y = cross(N, X);
    GhostRenderer::Plane pl{{0, 0, 0}, X, Y, N, 0.36f};
    V3 eyes[1] = {head};
    gh.clear();
    gh.drawMesh(gripHandMesh(false), pose, pl, eyes, 1);
    Canvas sc(G, G);
    cairo_set_source_rgb(sc.c, 0.30, 0.27, 0.22); cairo_paint(sc.c);
    uint8_t* dd = cairo_image_surface_get_data(sc.s);
    int stride = cairo_image_surface_get_stride(sc.s);
    const float tint[3] = {0.925f, 0.925f, 0.905f};
    for (int y = 0; y < G; ++y)
      for (int x = 0; x < G; ++x) {
        float p = gh.rgba[(size_t(y) * 2 * G + x) * 4] / 255.f;
        uint8_t* q = dd + y * stride + x * 4;
        for (int i = 0; i < 3; ++i) q[2 - i] = (uint8_t)std::min(255.f, q[2 - i] * (1 - p) + 255 * tint[i] * p);
      }
    cairo_surface_mark_dirty(sc.s);
    sc.savePNG(out + "/ghost.png");
  }
  printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
  return fails ? 1 : 0;
}

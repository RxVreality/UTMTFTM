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

  // The Windows beta tag (J4, 14 November, picked draft A of two): a BETA tag after the name on every page's header on
  // Windows, none on the Frame. Checked by looking for its yellow after the name, none under it, and that it stays clear of
  // the tabs (they start at x 456).
  {
#ifdef _WIN32
    CHECK(g_betaTag);
#else
    CHECK(!g_betaTag);
#endif
    const bool was = g_betaTag;
    for (int d = 0; d <= 1; ++d) {
      g_betaTag = d == 1;
      Canvas cv(1200, 760);
      DashModel m;
      m.game = "Pavlov VR";
      m.live = LiveState::Live;
      std::vector<Button> b;
      drawDashboard(cv, m, b);
      cv.savePNG(out + (d ? "/beta_tag_on.png" : "/beta_tag_off.png"));
      const auto& px = cv.toRGBA();
      // The tag is in the accent: yellow on the Frame, purple on Windows.
      const bool purple = accent() != Accent::Yellow;
      const auto yellow = [&](int x0, int y0, int x1, int y1) {
        int n = 0;
        for (int y = y0; y < y1; ++y)
          for (int x = x0; x < x1; ++x) {
            const uint8_t* q = &px[(size_t(y) * cv.w + x) * 4];
            if (purple ? (q[0] > 150 && q[1] < 130 && q[2] > 200) : (q[0] > 200 && q[1] > 150 && q[2] < 80)) ++n;
          }
        return n;
      };
      const int tag = yellow(345, 24, 445, 62), under = yellow(139, 66, 445, 90), tabGap = yellow(445, 24, 456, 62);
      CHECK((tag > 50) == (d == 1));
      CHECK(under == 0);
      CHECK(tabGap == 0);
    }
    g_betaTag = was;
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

      // MMeNDtal, 0.6.0: the hand pointing at the page isn't steadied, on purpose, and that read as broken. Its panel says why.
      DecafModel pointing = m;
      pointing.pointHand = 1;
      pointing.hands[1].out = pointing.hands[1].raw;  // the driver measures it as tracked, since it is
      drawDecaf(cv, pointing, b);
      cv.savePNG(out + "/decaf_pointing_right.png");
      CHECK(inBounds(b, cv));
      CHECK(decafKey(pointing) != decafKey(m));

      // ---- a newer version: the bottom line (J4's pick of two drafts, 14 November) in each of its states, and the Update page
      {
        DashModel hands;
        hands.game = "Pavlov";
        hands.live = LiveState::Live;
        const struct { UpdateNote::Kind kind; const char* name; bool button; } kinds[] = {
            {UpdateNote::Ready, "ready", true}, {UpdateNote::Needed, "needed", true}, {UpdateNote::Busy, "busy", false}, {UpdateNote::Installed, "installed", false}};
        for (const auto& k : kinds) {
          setUpdateNote({k.kind, "0.10.12"});  // a long version number, so the longest line is the one checked
          std::vector<Button> hb;
          drawDashboard(cv, hands, hb);
          cv.savePNG(out + "/update_note_" + k.name + ".png");
          CHECK(inBounds(hb, cv) && has(hb, "update_open") == k.button);
          DecafModel d = m;
          std::vector<Button> db;
          drawDecaf(cv, d, db);
          cv.savePNG(out + "/update_note_decaf_" + k.name + ".png");
          CHECK(inBounds(db, cv) && has(db, "update_open") == k.button);
        }
        setUpdateNote({UpdateNote::Ready, "0.6.2"});
        DashModel told = hands;
        told.status = "Saved for Pavlov.";
        std::vector<Button> tb;
        drawDashboard(cv, told, tb);
        CHECK(!has(tb, "update_open"));  // what you just did comes first, and the note's back after
        setUpdateNote({});
        std::vector<Button> plain;
        drawDashboard(cv, hands, plain);
        CHECK(!has(plain, "update_open"));  // nothing newer: the page's own words as always
        setUpdateNote({UpdateNote::Ready, "0.6.2"});
        UpdateModel u;
        u.game = "Pavlov";
        u.live = LiveState::Live;
        u.current = "0.6.1";
        u.latest = "0.6.2";
        u.summary = "Fixes from MMeNDtal's testing on PC VR: the D-pad's left and right, the Guided button hiding Focus 3, a stick pushed one way set to nothing, and where a new macro takes you. The Decaf tab says why the hand pointing at it isn't steadied, and it has a slider for how strong each step is.";
        std::vector<Button> ub;
        drawUpdate(cv, u, ub);
        cv.savePNG(out + "/update_page_ready.png");
        CHECK(inBounds(ub, cv) && has(ub, "update_go") && has(ub, "update_close"));
        UpdateModel need = u;
        need.needed = true;
        need.why = "SteamVR's last update changed how bindings reload. Live changes need 0.6.2.";
        drawUpdate(cv, need, ub);
        cv.savePNG(out + "/update_page_needed.png");
        // Windows only asks; the web browser fetches and you open the installer (J4, 15 November, after MMeNDtal's Trojan
        // warning on 0.6.1). J4 picked Download of two drafts. Before and after the button, and the Frame's own one-tap update.
        const auto apart = [](const std::vector<Button>& b) {
          for (size_t i = 0; i < b.size(); ++i)
            for (size_t j = i + 1; j < b.size(); ++j) {
              const double across = std::min(b[i].x + b[i].w, b[j].x + b[j].w) - std::max(b[i].x, b[j].x),
                           down = std::min(b[i].y + b[i].h, b[j].y + b[j].h) - std::max(b[i].y, b[j].y);
              if (across > 24 && down > 2) return false;
            }
          return true;
        };
        drawUpdate(cv, u, ub);
        cv.savePNG(out + "/update_win_ready.png");
        CHECK(inBounds(ub, cv) && apart(ub) && has(ub, "update_go") && has(ub, "update_close"));
        drawUpdate(cv, need, ub);
        cv.savePNG(out + "/update_win_needed.png");
        CHECK(inBounds(ub, cv) && apart(ub));
        UpdateModel opened = u;
        opened.step = UpdateModel::Opened;
        drawUpdate(cv, opened, ub);
        cv.savePNG(out + "/update_win_opened.png");
        CHECK(inBounds(ub, cv) && apart(ub) && has(ub, "update_go") && has(ub, "update_close"));
        CHECK(updateKey(opened) != updateKey(u));
        UpdateModel noBrowser = opened;
        noBrowser.error = "Couldn't open your web browser. Get it from github.com/RxVreality/UTMTFTM, under Releases.";
        drawUpdate(cv, noBrowser, ub);
        cv.savePNG(out + "/update_win_no_browser_again.png");
        CHECK(inBounds(ub, cv) && apart(ub));
        UpdateModel firstTap = u;
        firstTap.error = noBrowser.error;
        drawUpdate(cv, firstTap, ub);
        cv.savePNG(out + "/update_win_no_browser.png");
        CHECK(inBounds(ub, cv) && apart(ub) && has(ub, "update_go"));
        UpdateModel frame = u;
        frame.windows = false;
        drawUpdate(cv, frame, ub);
        cv.savePNG(out + "/update_frame_ready.png");
        CHECK(inBounds(ub, cv) && apart(ub) && has(ub, "update_go"));
        UpdateModel busy = frame;
        busy.step = UpdateModel::Downloading;
        drawUpdate(cv, busy, ub);
        cv.savePNG(out + "/update_page_downloading.png");
        UpdateModel failed = frame;
        failed.error = "The download didn't match what GitHub lists, so it wasn't installed. Try again.";
        drawUpdate(cv, failed, ub);
        cv.savePNG(out + "/update_page_failed.png");
        UpdateModel done = frame;
        done.step = UpdateModel::Installed;
        setUpdateNote({UpdateNote::Installed, "0.6.2"});
        drawUpdate(cv, done, ub);
        cv.savePNG(out + "/update_page_installed.png");
        CHECK(updateKey(done) != updateKey(u));
        setUpdateNote({});
      }

      {
        // The strength slider (J4, 14 November), at a few places, and held.
        DecafModel slider = m;
        slider.level = 1;
        for (int pct : {25, 50, 100, 150}) {
          slider.strength = pct;
          slider.down = pct == 50 ? "decaf_strength" : "";
          std::vector<Button> sb;
          drawDecaf(cv, slider, sb);
          cv.savePNG(out + "/decaf_strength_" + std::to_string(pct) + ".png");
          CHECK(inBounds(sb, cv) && has(sb, "decaf_strength"));
          DecafModel moved = slider;
          moved.strength = pct + 5;
          CHECK(decafKey(moved) != decafKey(slider));  // dragging it redraws the page
          const Button* bar = findButton(sb, "decaf_strength");
          CHECK(bar && sliderValue(*bar, bar->x + 14, kDecafMinPct, kDecafMaxPct, 5) == 25 && sliderValue(*bar, bar->x + bar->w - 14, kDecafMinPct, kDecafMaxPct, 5) == 150);
          CHECK(bar && sliderValue(*bar, -500, kDecafMinPct, kDecafMaxPct, 5) == 25 && sliderValue(*bar, 5000, kDecafMinPct, kDecafMaxPct, 5) == 150);  // dragged off either end
          CHECK(bar && sliderValue(*bar, bar->x + 14 + (bar->w - 28) * 0.6, kDecafMinPct, kDecafMaxPct, 5) == 100);  // 100% sits 60% along
        }
        // A game that weights your hands itself, on the lighter start (J4's pick, 14 November).
        DecafModel heavy = m;
        heavy.game = "Blade & Sorcery";
        heavy.level = 1;
        heavy.strength = 50;
        heavy.weightNote = "Blade & Sorcery gives your hands weight of its own, and Decaf adds to it. It starts at 50% here.";
        std::vector<Button> hb2;
        drawDecaf(cv, heavy, hb2);
        cv.savePNG(out + "/decaf_weight_note_solid.png");
        CHECK(inBounds(hb2, cv));
        DecafModel maybe = m;
        maybe.game = "Example Game";
        maybe.level = 1;
        maybe.weightMaybe = true;
        maybe.weightNote = "Players say Example Game may give your hands weight of its own. If Decaf feels late here, try less strength.";
        drawDecaf(cv, maybe, hb2);
        cv.savePNG(out + "/decaf_weight_note_maybe.png");
        CHECK(inBounds(hb2, cv) && decafKey(maybe) != decafKey(heavy));
        DecafModel off = m;
        off.level = 0;
        std::vector<Button> ob;
        drawDecaf(cv, off, ob);
        CHECK(!has(ob, "decaf_strength"));  // nothing to make stronger or lighter
      }

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

  // J4, 16 November: the Windows copy is electric purple with white on it, and the Frame's stays
  // yellow, so the two can't be mixed up when a Frame streams from a PC. Each build starts in its own. The sheet puts the
  // same screens side by side in each look, and each look's screens are saved full size too. The checks: each look's
  // armed bar is its own colour, and what's written on it is black on yellow and white on purple.
  {
#ifdef _WIN32
    CHECK(accent() == Accent::Purple);
#else
    CHECK(accent() == Accent::Yellow);
#endif
    const Accent was = accent();
    const bool betaWas = g_betaTag;
    struct Look { Accent a; const char* tag; const char* name; };
    const Look looks[] = {{Accent::Yellow, "yellow", "STEAM FRAME: YELLOW"}, {Accent::Purple, "purple", "WINDOWS: PURPLE"}};
    const int nLooks = int(sizeof looks / sizeof looks[0]);
    const double k = 0.6;
    const int colW = int(1200 * k) + 30, top = 70, panelY = top + int(760 * k) + 24, iconY = panelY + int(300 * k) + 24;
    Canvas sheet(30 + colW * nLooks, iconY + 160);
    cairo_set_source_rgb(sheet.c, 0.03, 0.03, 0.03);
    cairo_paint(sheet.c);
    auto paste = [&](Canvas& cv, double px, double py, double s) {
      cairo_save(sheet.c);
      cairo_translate(sheet.c, px, py);
      cairo_scale(sheet.c, s, s);
      cairo_set_source_surface(sheet.c, cv.s, 0, 0);
      cairo_paint(sheet.c);
      cairo_restore(sheet.c);
    };
    for (int i = 0; i < nLooks; ++i) {
      const Look& L = looks[i];
      setAccent(L.a);
      CHECK(accent() == L.a);
      g_betaTag = L.a != Accent::Yellow;  // the Frame's copy has no beta tag
      const double x = 30 + i * colW;
      cairo_select_font_face(sheet.c, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(sheet.c, 24);
      cairo_set_source_rgb(sheet.c, 0.925, 0.925, 0.905);
      cairo_move_to(sheet.c, x, 46);
      cairo_show_text(sheet.c, L.name);

      Canvas dash(1200, 760);
      DashModel m;
      m.game = "Half-Life: Alyx";
      m.live = LiveState::Live;
      m.armed = true;
      m.cards[0].badge = m.cards[1].badge = Badge::GameCustom;
      std::vector<Button> b;
      drawDashboard(dash, m, b);
      paste(dash, x, top, k);
      dash.savePNG(out + "/accent_" + L.tag + "_dash_armed.png");
      {
        // The "Now close the dashboard" bar, left of its Cancel button.
        const auto& px = dash.toRGBA();
        const int want[3] = {L.a == Accent::Yellow ? 255 : 191, L.a == Accent::Yellow ? 210 : 0, L.a == Accent::Yellow ? 0 : 255};
        int fill = 0, light = 0, dark = 0;
        for (int y = 115; y < 162; ++y)
          for (int x = 70; x < 700; ++x) {
            const uint8_t* q = &px[(size_t(y) * dash.w + x) * 4];
            if (std::abs(q[0] - want[0]) < 30 && std::abs(q[1] - want[1]) < 30 && std::abs(q[2] - want[2]) < 30) ++fill;
            if (q[0] > 230 && q[1] > 230 && q[2] > 230) ++light;
            if (q[0] < 50 && q[1] < 50 && q[2] < 50) ++dark;
          }
        printf("accent %s: bar %d px of its colour, %d light, %d dark\n", L.tag, fill, light, dark);
        CHECK(fill > 15000);
        if (L.a == Accent::Yellow) CHECK(dark > 300 && light == 0);
        else CHECK(light > 300 && dark == 0);
      }
      m.armed = false;
      m.cards[0].applied = off(3.2f, -1.0f, 0.5f, 12, 0, -8);
      m.hover = "place_left";
      drawDashboard(dash, m, b);
      dash.savePNG(out + "/accent_" + L.tag + "_dash_live.png");

      Canvas panel(820, 300);
      PanelModel p;
      p.header = "LEFT HAND"; p.progress = "1 / 2"; p.chip = "GRIP"; p.text = "Grab the ghost with your right hand";
      p.status = "No offset yet"; p.keys = {{"A / X", "No offset"}, {"B / Y", "Cancel"}};
      drawPanel(panel, p);
      paste(panel, x, panelY, k);
      panel.savePNG(out + "/accent_" + L.tag + "_panel.png");

      Canvas icon(256, 256);
      drawIcon(icon);
      paste(icon, x, iconY, 0.5);
      icon.savePNG(out + "/accent_" + L.tag + "_icon.png");
    }
    sheet.savePNG(out + "/accent_sheet.png");
    setAccent(was);
    g_betaTag = betaWas;
  }

  // ---- the stick drift page (J4, 14 November), inside the Decaf tab (J4's pick of two drafts), and the check's steps.
  // Made-up sticks: the left one rests up and to the left and lands somewhere else each time, the right one's fine.
  {
    unsigned seed = 7;
    const auto rnd = [&] { seed = seed * 1103515245u + 12345u; return float((seed >> 8) & 0xFFFF) / 65535.f - 0.5f; };
    const auto cloud = [&](float cx, float cy, float spread, int n) {
      std::vector<DriftDot> v;
      for (int i = 0; i < n; ++i) v.push_back({cx + rnd() * spread, cy + rnd() * spread});
      return v;
    };
    const auto noOverlap = [&](const std::vector<Button>& b, const std::string& what) {
      for (size_t i = 0; i < b.size(); ++i)
        for (size_t j = i + 1; j < b.size(); ++j) {
          const double across = std::min(b[i].x + b[i].w, b[j].x + b[j].w) - std::max(b[i].x, b[j].x), down = std::min(b[i].y + b[i].h, b[j].y + b[j].h) - std::max(b[i].y, b[j].y);
          if (!(across <= 24 || down <= 2)) { ++fails; printf("FAIL %s: %s and %s are on top of each other\n", what.c_str(), b[i].id.c_str(), b[j].id.c_str()); }
        }
    };
    const auto shot = [&](const DriftModel& m, const std::string& name) {
      Canvas cv(1200, 760);
      std::vector<Button> b;
      drawDrift(cv, m, b);
      cv.savePNG(out + "/" + name);
      for (auto& x : b) CHECK(x.x >= 0 && x.y >= 0 && x.x + x.w <= cv.w && x.y + x.h <= cv.h);
      noOverlap(b, name);
      return b;
    };
    DriftModel first;
    first.game = "Pavlov";
    first.live = LiveState::Live;
    first.sticks[0].raw = {-0.10f, 0.12f};
    first.sticks[0].trail = cloud(-0.10f, 0.12f, 0.05f, 40);
    first.sticks[1].raw = {0.004f, -0.006f};
    first.sticks[1].trail = cloud(0, 0, 0.02f, 40);
    {
      const std::vector<Button> b = shot(first, "drift_first.png");
      bool hands = false, sticks = false, check = false;
      for (const Button& x : b) { hands |= x.id == "decaf_part_hands"; sticks |= x.id == "decaf_part_sticks"; check |= x.id == "drift_check"; }
      CHECK(hands && sticks && check);  // the way back to steady hands, and the one thing to do
    }
    // Step 1: leave them alone, with a thumb still on the right stick.
    DriftModel s1 = first;
    s1.step = 1;
    s1.countdown = 3;
    s1.sticks[0].progress = 0.4f;
    s1.sticks[0].countdown = 3;
    s1.sticks[1].countdown = 4;
    s1.sticks[1].progress = 0.1f;
    s1.sticks[1].touched = true;
    shot(s1, "drift_step1.png");
    // Step 2: round the edge, most of the way on the left.
    DriftModel s2 = first;
    s2.step = 2;
    s2.sticks[0].reach.assign(32, 0.f);
    s2.sticks[1].reach.assign(32, 0.f);
    for (int i = 0; i < 26; ++i) s2.sticks[0].reach[i] = 0.95f;
    for (int i = 0; i < 32; ++i) s2.sticks[1].reach[i] = 1.f;
    s2.sticks[0].raw = {std::cos(2 * 3.14159f * 26 / 32) * 0.96f, std::sin(2 * 3.14159f * 26 / 32) * 0.96f};
    s2.sticks[1].raw = {0, 0};
    s2.sticks[0].trail.clear();
    s2.sticks[1].trail.clear();
    s2.sticks[0].progress = 26 / 32.f;
    s2.sticks[1].progress = 1;
    shot(s2, "drift_step2.png");
    // Step 3: flick and let go. Where the left one lands depends on which way it came back from.
    DriftModel s3 = first;
    s3.step = 3;
    s3.sticks[0].landed = {{-0.04f, 0.20f}, {-0.16f, 0.06f}, {-0.07f, 0.02f}, {0.00f, 0.13f}, {-0.12f, 0.17f}};
    s3.sticks[1].landed = {{0.01f, 0.0f}, {-0.01f, 0.01f}, {0.0f, -0.01f}, {0.01f, 0.01f}, {0.0f, 0.0f}, {-0.01f, 0.0f}, {0.01f, -0.01f}, {0.0f, 0.01f}};
    s3.sticks[0].progress = 5 / 8.f;
    s3.sticks[1].progress = 1;
    s3.sticks[0].raw = s3.sticks[0].landed.back();
    shot(s3, "drift_step3.png");
    // What it found, and the page afterwards with the fix on.
    DriftModel s4 = s3;
    s4.step = 4;
    s4.fixOn = true;
    for (DriftStick& s : s4.sticks) { s.checked = true; s.progress = -1; s.trail.clear(); }
    DriftStick& l = s4.sticks[0];
    l.drifts = true;
    l.verdict = "Drifts up and to the left";
    l.verdictWarn = true;
    l.lines = {"Rests 14% off the middle.", "Lands in a different spot each", "time you let go, up to 9% apart.", "Reaches the edge all round."};
    l.rest = {-0.08f, 0.11f};
    l.dead = 0.14f;
    l.landed.push_back({-0.02f, 0.10f});
    l.landed.push_back({-0.13f, 0.12f});
    l.landed.push_back({-0.09f, 0.04f});
    l.raw = {-0.10f, 0.12f};
    l.out = {0, 0};
    DriftStick& r = s4.sticks[1];
    r.verdict = "No drift";
    r.lines = {"Comes back to the middle", "every time. Left as it is."};
    r.out = r.raw;
    shot(s4, "drift_result.png");
    DriftModel after = s4;
    after.step = 0;
    after.sticks[0].landed.clear();
    after.sticks[1].landed.clear();
    after.sticks[0].lines = {"Rests 14% off the middle.", "The game sees the middle", "when you let go."};
    shot(after, "drift_after.png");
    // Pushed half way up while fixed: the game gets it measured from where the stick rests.
    DriftModel push = after;
    push.sticks[0].raw = {-0.08f, 0.62f};
    push.sticks[0].out = {0.0f, 0.42f};
    push.sticks[0].touched = true;
    shot(push, "drift_after_push.png");
    CHECK(driftKey(push) != driftKey(after));
    {  // the Decaf tab's own page, with the switch, and without it on a Frame
      Canvas cv(1200, 760);
      DecafModel dm;
      dm.game = "Pavlov";
      dm.live = LiveState::Live;
      dm.level = 2;
      dm.hands[0] = {true, 0.31f, 0.038f};
      dm.hands[1] = {true, 0.27f, 0.035f};
      std::vector<Button> b;
      drawDecaf(cv, dm, b);
      bool sw = false;
      for (const Button& x : b) sw |= x.id == "decaf_part_sticks";
      CHECK(!sw);
      dm.sticksSwitch = true;
      drawDecaf(cv, dm, b);
      cv.savePNG(out + "/decaf_with_sticks_switch.png");
      sw = false;
      for (const Button& x : b) sw |= x.id == "decaf_part_sticks";
      CHECK(sw);
      noOverlap(b, "decaf_with_sticks_switch.png");
      for (auto& x : b) CHECK(x.x >= 0 && x.y >= 0 && x.x + x.w <= cv.w && x.y + x.h <= cv.h);
    }
  }
  printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
  return fails ? 1 : 0;
}

// UTMTFTM UI: pure drawing (cairo), no VR calls, so every screen can be rendered and checked offline.
#pragma once
#include <cairo.h>

#include <string>
#include <vector>

#include "ghost3d.h"
#include "xform.h"

namespace fh {

struct Canvas {
  int w, h;
  cairo_surface_t* s;
  cairo_t* c;
  std::vector<uint8_t> rgba;
  Canvas(int W, int H);
  ~Canvas();
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;
  void clear();
  const std::vector<uint8_t>& toRGBA();  // straight-alpha RGBA for SetOverlayRaw
  bool savePNG(const std::string& path);
};

// enabled=false: drawn greyed; tapping it explains why instead of doing nothing.
struct Button { double x, y, w, h; std::string id; bool enabled = true; };
const Button* hitButton(const std::vector<Button>& b, double x, double y);

// ---------------------------------------------------------------- dashboard
enum class Badge { None, GameCustom, GlobalDefault, AllGamesDefault };
enum class LiveState { NoGame, Live, NextLaunch };

struct HandCard {
  Offset applied;        // what the game is using right now
  Badge badge = Badge::None;
  bool ghostMatched = false;
};

struct DashModel {
  std::string game;           // "" = no game running
  LiveState live = LiveState::NoGame;
  HandCard cards[2];
  Offset ghostBase[2];        // for the previews
  bool saveForGame = true;    // "Save for: This game" vs "All games"
  bool gameHasCustom = false;
  bool needsMatch = false;    // ghost not matched to a game hand yet
  int maxCm = 15;
  std::string status;         // one line; empty -> contextual hint
  bool statusWarn = false;
  bool statusGood = false;    // a confirmation (green check) rather than plain info
  bool armed = false;         // waiting for the dashboard to close
  bool confirmClose = false;  // "tap again to close"

  // press feedback (static states, no animation): button under the laser / button held down
  std::string hover, down;
};

// Dashboard press feedback as plain state, testable without VR. Static states only (the user
// doesn't want motion in a settings UI): highlighted under the laser, pushed in while held.
// What a tap DID is shown by the screen itself (values, selected chip, greyed Reset) plus one
// confirmation line; tapping a greyed button explains why it's greyed.
struct DashFx {
  std::string hover, down;
  void move(const std::string& id) { hover = id; }
  void leave() { hover.clear(); down.clear(); }
  void press(const std::string& id) { down = id; }
  void release() { down.clear(); }
  void apply(DashModel& m) const { m.hover = hover; m.down = down; }
};

// Stable text form of everything the dashboard shows: redraw whenever it changes.
std::string dashKey(const DashModel& m);
void drawDashboard(Canvas& cv, const DashModel& m, std::vector<Button>& buttons);

// ---------------------------------------------------------------- in-headset instruction panel
struct PanelModel {
  std::string header;     // "LEFT HAND" / "MATCH GHOST · LEFT HAND"
  std::string progress;   // "1 / 2" or ""
  std::string chip;       // controller input to use now: "GRIP", "TRIGGER" or ""
  std::string text;       // the one instruction
  std::string status;     // offset readout / warning
  bool warn = false;
  bool match = false;     // match-ghost mode (different accent)
  std::vector<std::pair<std::string, std::string>> keys;  // footer: {"A / X", "No offset"}, ...
};
std::string panelKey(const PanelModel& m);
void drawPanel(Canvas& cv, const PanelModel& m);
void drawMessage(Canvas& cv, const std::string& title, const std::string& sub, bool good);

void drawIcon(Canvas& cv);

// Human wording for an offset: "3.2 cm forward, 1.0 cm up, turned 12°" / "No offset".
std::string friendlyOffset(const Offset& o);

// Which controller the preview shows on each hand: its mesh in the device's own frame, and where the grip sits on it.
void setControllerModel(bool left, const Mesh& inDeviceFrame, const M& gripFromRaw);

// 3D preview of a hand at an offset next to its controller, composited into the canvas.
void drawHandPreview(Canvas& cv, double x, double y, double size, bool left, const Offset& applied,
                     const Offset& ghostBase, double r, double g, double b);

}  // namespace fh

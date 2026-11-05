// UTMTFTM UI: pure drawing (cairo), no VR calls, so every screen can be rendered and checked offline.
#pragma once
#include <cairo.h>

#include <map>
#include <memory>
#include <set>
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
  bool fine = false;          // fine adjust: arrows either side of each number
  bool fineBig = false;       // and they go 1 cm and 5 degrees a tap, rather than 1 mm and 1 degree
  std::string hint;           // what's under the laser would do, when it isn't obvious

  // press feedback (static states, no animation): button under the laser / button held down
  std::string hover, down;
};

// The pages share the dashboard panel. This says which one's showing.
enum class DashPage { Hands, Bindings, Combos };

// A fine adjust arrow: "nudge_<hand>_<axis>_<p or m>". axis as in nudged(), in placement.h.
bool parseNudge(const std::string& id, int& hand, int& axis, int& dir);
// What it would do, in words, for the line along the bottom of the page. Empty for anything else.
std::string nudgeHint(const std::string& id, bool big);

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

// ---------------------------------------------------------------- bindings page
// A controller drawn once and kept: the image, and how to find a point on it.
class ControllerPicture {
 public:
  struct Spot { double x, y; bool behind; };
  ControllerPicture(std::shared_ptr<cairo_surface_t> image, std::vector<float> nearest, std::map<std::string, Spot> spots, int sizePx, V3 centre, V3 x, V3 y, V3 n, V3 eye, float span);
  cairo_surface_t* image() const { return image_.get(); }
  int size() const { return size_; }
  // Where a point on the controller lands, as 0..1 of the picture, and whether the body's in front of it from here.
  void place(V3 p, double& x, double& y, bool& behind) const;
  // Where a part is on the picture: the middle of as much of it as shows.
  const Spot* spot(const std::string& part) const;

 private:
  std::shared_ptr<cairo_surface_t> image_;
  std::vector<float> nearest_;  // how far away the nearest surface is at each pixel. 0: nothing there
  std::map<std::string, Spot> spots_;
  int size_;
  V3 c_, x_, y_, n_, eye_;
  float span_;
};
ControllerPicture bakeController(const Mesh& mesh, const std::vector<ModelPart>& parts, bool left, int sizePx);

struct BindRow {
  std::string id;       // "y:left/input/a", "t:right/input/b"
  std::string name;     // "A"
  std::string action;   // what it does, "" = its own name says it all
  bool unused = false;  // a game's binding is showing, and this button isn't on it
  double x = -1, y = -1;  // where it sits on the picture, 0..1. Negative: nowhere known
  bool behind = false;    // it's round the back from this angle, so the mark is drawn hollow
  int mark = 0;           // part of a combo: 1 touch, 2 press
};
struct BindSide {
  std::string title, note;
  const ControllerPicture* pic[2] = {nullptr, nullptr};
  std::vector<BindRow> rows[2];
};
struct BindModel {
  std::string game;
  LiveState live = LiveState::NoGame;
  bool hasGame = false;
  std::vector<std::string> refs;  // every controller there is to compare with
  std::string ref;
  std::string controls;  // which of the game's sets of controls is showing, when it has several
  BindSide theirs, yours;
  std::string layoutName;       // "Quest layout"
  bool layoutOn = false;        // this game's on it
  bool layoutOther = false;     // on another controller's layout
  bool layoutOk = false;        // there's something to translate
  bool saveForGame = true;
  bool wiringChanged = false;
  std::string armed, armedName;  // the button of yours being rewired
  std::set<std::string> lit;    // rows lit up because the laser is on them or their match
  std::string status;
  bool statusWarn = false, statusGood = false;
  bool confirmClose = false;
  std::string hover, down;
};
std::string bindKey(const BindModel& m);
void drawBindings(Canvas& cv, const BindModel& m, std::vector<Button>& buttons);

// ---------------------------------------------------------------- combos page
struct ComboEntry { std::string inputs, action; };
struct ComboChoice { std::string id, name, where; };
struct ComboIdea { std::string id, name; };
struct ComboModel {
  std::string game;
  LiveState live = LiveState::NoGame;
  int step = 0;          // 0 what's saved, 1 what you do, 2 what it does
  bool canMake = false;  // there's a game, and bindings for it to add to
  std::string why;       // why not, when it can't
  BindSide yours;
  std::vector<ComboEntry> saved;
  std::vector<ComboIdea> ideas;
  std::string picked;    // "Touch A + Touch B"
  std::string note;      // anything worth knowing about what's picked
  std::string controls;  // which of the game's sets of controls the list is from, when it has several
  std::vector<ComboChoice> choices;  // this page of them
  int pageNo = 0, pages = 1;
  std::string status;
  bool statusWarn = false, statusGood = false;
  bool confirmClose = false;
  std::string hover, down;
};
std::string comboKey(const ComboModel& m);
void drawCombos(Canvas& cv, const ComboModel& m, std::vector<Button>& buttons);

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

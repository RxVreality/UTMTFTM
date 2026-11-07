// Macros: one button plays back a sequence you did with your own hands, the way a Logitech mouse macro does.
// This turns what the controllers report into steps people can read and rearrange, and steps into what the driver plays.
// No VR calls, so all of it runs in the tests.
//
// A controller isn't a mouse. A VR button is really up to three things: resting a thumb on it, pressing it, and on triggers and
// grips how far it's pulled. One press reports all of those, so here they count as one step, "Press A", and playing it back
// sets all of them. Resting a thumb on something happens all the time without meaning anything, so touches stay out of the
// list unless there's nothing else or you ask for them.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "live.h"

namespace fh {
namespace macro {

enum Kind { Press = fhlive::StepPress, Release = fhlive::StepRelease, Touch = fhlive::StepTouch, Untouch = fhlive::StepUntouch, Push = fhlive::StepPush, Centre = fhlive::StepCentre };

struct Step {
  int hand = 0;
  std::string path;  // "/input/a"
  int kind = Press;
  int x = 0, y = 0;  // Push: which way, -100 to 100
  int delayMs = 0;   // how long after the step before it, as you did it
};

struct Recorded {
  std::string name;
  int hand = -1;         // the button that plays it. -1: none yet
  std::string path;
  bool onTouch = false;  // resting on it plays it, rather than pressing it
  bool repeat = false;   // goes round again while it's held
  bool asRecorded = true;  // your timing, rather than the same quick gap between every step
  bool withTouches = false;
  std::vector<Step> steps;  // everything that was recorded, touches included
};

constexpr int kQuickMs = 50;    // the gap between steps unless you ask for your own: the same as G HUB's "standard delay"
constexpr int kShortestMs = 20; // long enough for a game running at 60 frames a second to see it
constexpr int kStopAfterMs = 2000;  // recording stops this long after your last press
constexpr uint32_t kAlreadyHeldMs = 150;  // down this soon after it started listening means it was down before: nobody's that quick

// One report from the driver.
struct Raw {
  uint32_t ms = 0;
  int hand = 0;
  std::string name;  // "/input/a/click"
  float value = 0;
};

// "/input/a/click" -> "/input/a" and "click".
inline bool splitName(const std::string& name, std::string& path, std::string& part) {
  const size_t cut = name.rfind('/');
  if (cut == std::string::npos || cut < 7 || name.rfind("/input/", 0) != 0) return false;
  path = name.substr(0, cut);
  part = name.substr(cut + 1);
  return true;
}

class Recorder {
 public:
  // When it started listening, on the same clock as the reports. Whatever's already down a moment later was down before you
  // started, like the grip you're holding the controller with. That isn't a step, and it doesn't have to be let go of.
  void begin(uint32_t nowMs) {
    begun_ = true;
    startMs_ = nowMs;
  }

  // True if that added a step.
  bool feed(const Raw& r) {
    std::string path, part;
    if (!splitName(r.name, path, part)) return false;
    if (path.find("/finger") != std::string::npos || path.find("/input/skeleton") == 0) return false;  // how your fingers are curled isn't a control
    Control& c = controls_[std::to_string(r.hand) + path];
    const bool early = begun_ && r.ms - startMs_ <= kAlreadyHeldMs;
    bool added = false;
    if (part == "click" || part == "value" || part == "force") {
      // Pressed if any of them says so. A trigger's click can read up while its pull's still most of the way in, and an Index
      // grip's pull can read in while its force reads nothing, so one of them saying "up" doesn't end a press.
      if (part == "click") c.clicked = r.value > 0.5f;
      else {
        bool& pulled = part == "value" ? c.pulled : c.forced;
        c.sawPull = true;
        if (r.value > 0.75f) pulled = true;
        else if (r.value < 0.4f) pulled = false;
      }
      const bool down = c.clicked || c.pulled || c.forced;
      if (down && !c.pressed) added = went(r, path, c, Press, early);
      else if (!down && c.pressed) added = came(r, path, c, Release);
    } else if (part == "touch") {
      if (r.value > 0.5f && !c.touched) added = went(r, path, c, Touch, early);
      else if (r.value <= 0.5f && c.touched) added = came(r, path, c, Untouch);
    } else if (part == "x" || part == "y") {
      (part == "x" ? c.stickX : c.stickY) = r.value;
      const bool out = std::fabs(c.stickX) > 0.6f || std::fabs(c.stickY) > 0.6f;
      const bool back = std::fabs(c.stickX) < 0.3f && std::fabs(c.stickY) < 0.3f;
      if (out && !c.pushed) added = went(r, path, c, Push, early);
      else if (back && c.pushed) added = came(r, path, c, Centre);
    }
    if (r.ms > lastMs_) lastMs_ = r.ms;
    return added;
  }

  // Your last press was long enough ago, and there's something to keep.
  bool finished(uint32_t now) const {
    if (steps_.empty() || !allLetGo()) return false;
    return now >= lastStepMs_ + kStopAfterMs;
  }
  bool full() const { return steps_.size() >= kFullAt; }
  bool empty() const { return steps_.empty(); }
  const std::vector<Step>& soFar() const { return steps_; }  // only what's really happened, for showing as it goes

  // Anything still held at the end gets let go of at the end, so what's shown is what plays.
  std::vector<Step> steps() const {
    std::vector<Step> out = steps_;
    int wait = int(std::min<uint32_t>(lastMs_ > lastStepMs_ ? lastMs_ - lastStepMs_ : 0, 60000));  // as long as it was really held
    for (const auto& [key, c] : controls_) {
      const int hand = key[0] - '0';
      const std::string path = key.substr(1);
      auto add = [&](int kind) {
        Step s;
        s.hand = hand;
        s.path = path;
        s.kind = kind;
        s.delayMs = std::max(wait, kQuickMs);
        wait = 0;
        out.push_back(s);
      };
      if (c.pressed && !c.beforePress) add(Release);
      if (c.pushed && !c.beforePush) add(Centre);
      if (c.touched && !c.beforeTouch) add(Untouch);
    }
    return out;
  }

 private:
  static constexpr size_t kFullAt = 60;  // room for the touches that'll get filtered out
  struct Control {
    bool pressed = false, touched = false, pushed = false;
    bool clicked = false, pulled = false, forced = false, sawPull = false;
    bool beforePress = false, beforeTouch = false, beforePush = false;  // it was like this before listening started
    float stickX = 0, stickY = 0;
  };
  // Something went down. If that was before listening started it's only noted, so its coming back up means nothing either.
  bool went(const Raw& r, const std::string& path, Control& c, int kind, bool early) {
    if (kind == Press) { c.pressed = true; c.beforePress = early; }
    if (kind == Touch) { c.touched = true; c.beforeTouch = early; }
    if (kind == Push) { c.pushed = true; c.beforePush = early; }
    if (early) return false;
    Step s;
    s.hand = r.hand;
    s.path = path;
    s.kind = kind;
    s.delayMs = gapTo(r);
    if (kind == Push) {
      // The four ways a stick gets pushed on purpose. Halfway between two is rounded to the stronger.
      if (std::fabs(c.stickX) > std::fabs(c.stickY)) s.x = c.stickX > 0 ? 100 : -100;
      else s.y = c.stickY > 0 ? 100 : -100;
    }
    keep(s, r);
    return true;
  }
  bool came(const Raw& r, const std::string& path, Control& c, int kind) {
    bool before = false;
    if (kind == Release) { before = c.beforePress; c.pressed = c.beforePress = false; }
    if (kind == Untouch) { before = c.beforeTouch; c.touched = c.beforeTouch = false; }
    if (kind == Centre) { before = c.beforePush; c.pushed = c.beforePush = false; }
    if (before) return false;
    Step s;
    s.hand = r.hand;
    s.path = path;
    s.kind = kind;
    s.delayMs = gapTo(r);
    keep(s, r);
    return true;
  }
  int gapTo(const Raw& r) const { return steps_.empty() ? 0 : int(std::min<uint32_t>(r.ms - lastStepMs_, 60000)); }
  void keep(const Step& s, const Raw& r) {
    steps_.push_back(s);
    lastStepMs_ = r.ms;
  }
  // A thumb resting on something can stay there, and so can a grip or trigger you're still squeezing, because that's how you hold
  // a controller. It's buttons and sticks that have to end, unless touches are all you did.
  bool allLetGo() const {
    bool anyPress = false, stillHeld = false, stillTouched = false;
    for (const Step& s : steps_) anyPress = anyPress || s.kind == Press || s.kind == Push;
    for (const auto& [key, c] : controls_) {
      stillHeld = stillHeld || (c.pressed && !c.beforePress && !c.sawPull) || (c.pushed && !c.beforePush);
      stillTouched = stillTouched || (c.touched && !c.beforeTouch);
    }
    return !stillHeld && (anyPress || !stillTouched);
  }

  std::vector<Step> steps_;
  std::map<std::string, Control> controls_;
  uint32_t lastMs_ = 0, lastStepMs_ = 0, startMs_ = 0;
  bool begun_ = false;
};

// Which button plays it: the first thing you press, or something you touch and let go of without pressing.
class TriggerPicker {
 public:
  // As for Recorder: whatever's already down a moment after this isn't what you're choosing.
  void begin(uint32_t nowMs) {
    begun_ = true;
    startMs_ = nowMs;
  }
  bool feed(const Raw& r) {
    std::string path, part;
    if (done_ || !splitName(r.name, path, part)) return done_;
    const std::string key = std::to_string(r.hand) + path;
    const bool early = begun_ && r.ms - startMs_ <= kAlreadyHeldMs;
    if (part == "click" || part == "value" || part == "force") {
      const std::string partKey = key + "/" + part;
      if (r.value > (part == "click" ? 0.5f : 0.75f)) {
        if (early) before_[partKey] = true;
        else if (!before_.count(partKey)) pick(r.hand, path, false);
      } else if (part == "click" || r.value < 0.4f) before_.erase(partKey);
    } else if (part == "touch") {
      if (r.value > 0.5f) { if (!early) touching_[key] = r.ms; }
      else if (touching_.count(key)) pick(r.hand, path, true);
    }
    return done_;
  }
  bool done() const { return done_; }
  int hand() const { return hand_; }
  const std::string& path() const { return path_; }
  bool onTouch() const { return onTouch_; }

 private:
  void pick(int hand, const std::string& path, bool onTouch) {
    done_ = true;
    hand_ = hand;
    path_ = path;
    onTouch_ = onTouch;
  }
  bool done_ = false, onTouch_ = false, begun_ = false;
  int hand_ = -1;
  uint32_t startMs_ = 0;
  std::string path_;
  std::map<std::string, uint32_t> touching_;
  std::map<std::string, bool> before_;
};

// What's left once touches that were only on the way to a press are gone, and every touch if they aren't wanted.
// A removed step's wait goes to the one after it, so your timing stays your timing.
inline std::vector<Step> shown(const std::vector<Step>& all, bool withTouches) {
  std::vector<bool> drop(all.size(), false);
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i].kind != Touch && all[i].kind != Untouch) continue;
    if (!withTouches) { drop[i] = true; continue; }
    if (all[i].kind != Touch) continue;
    // From this touch to when it stopped: was it pressed in between?
    size_t end = i + 1;
    bool pressed = false;
    for (; end < all.size(); ++end) {
      if (all[end].hand != all[i].hand || all[end].path != all[i].path) continue;
      if (all[end].kind == Untouch) break;
      if (all[end].kind == Press || all[end].kind == Push) pressed = true;
    }
    if (pressed) {
      drop[i] = true;
      if (end < all.size()) drop[end] = true;
    }
  }
  std::vector<Step> out;
  int carried = 0;
  for (size_t i = 0; i < all.size(); ++i) {
    if (drop[i]) { carried += all[i].delayMs; continue; }
    Step s = all[i];
    s.delayMs += carried;
    carried = 0;
    out.push_back(s);
  }
  if (!out.empty()) out[0].delayMs = 0;
  return out;
}

// There were touches with no press behind them, so there's a choice to offer.
inline bool hasBareTouches(const std::vector<Step>& all) {
  const std::vector<Step> kept = shown(all, true);
  return std::any_of(kept.begin(), kept.end(), [](const Step& s) { return s.kind == Touch; });
}

// Only the steps this says are real. A removed step's wait goes to the one after it, the same as for touches above.
template <class Keep>
inline std::vector<Step> onlyWhere(const std::vector<Step>& all, Keep keep) {
  std::vector<Step> out;
  int carried = 0;
  for (const Step& s : all) {
    if (!keep(s)) { carried += s.delayMs; continue; }
    out.push_back(s);
    out.back().delayMs += carried;
    carried = 0;
  }
  if (!out.empty()) out[0].delayMs = 0;
  return out;
}

// One tile per press: a press and its let-go share a tile unless something else happens between them.
struct Tile {
  int first = 0;  // where it starts in the steps
  int count = 1;  // 2 for a press and its let-go
};

inline bool endsIt(const Step& a, const Step& b) {
  if (a.hand != b.hand || a.path != b.path) return false;
  return (a.kind == Press && b.kind == Release) || (a.kind == Touch && b.kind == Untouch) || (a.kind == Push && b.kind == Centre);
}

inline std::vector<Tile> tilesOf(const std::vector<Step>& steps) {
  std::vector<Tile> out;
  for (size_t i = 0; i < steps.size(); ++i) {
    Tile t;
    t.first = int(i);
    if (i + 1 < steps.size() && endsIt(steps[i], steps[i + 1])) {
      t.count = 2;
      ++i;
    }
    out.push_back(t);
  }
  return out;
}

// A tile changes places with the one beside it. The gaps between tiles stay where they were, and what's inside one goes with it.
// Returns which tile it is now, because a press can join up with its let-go once what was between them has moved away.
inline int moveTile(std::vector<Step>& steps, int at, int dir) {
  const std::vector<Tile> tiles = tilesOf(steps);
  const int to = at + dir;
  if (at < 0 || at >= int(tiles.size()) || to < 0 || to >= int(tiles.size())) return at;
  const Tile left = tiles[std::min(at, to)], right = tiles[std::max(at, to)];
  std::vector<Step> out(steps.begin(), steps.begin() + left.first);
  const int gapBefore = steps[left.first].delayMs, gapBetween = steps[right.first].delayMs;
  const int rightAt = int(out.size());
  out.insert(out.end(), steps.begin() + right.first, steps.begin() + right.first + right.count);
  const int leftAt = int(out.size());
  out.insert(out.end(), steps.begin() + left.first, steps.begin() + left.first + left.count);
  out.insert(out.end(), steps.begin() + right.first + right.count, steps.end());
  out[rightAt].delayMs = gapBefore;
  out[leftAt].delayMs = gapBetween;
  steps = std::move(out);
  const int movedStep = dir < 0 ? rightAt : leftAt;
  const std::vector<Tile> now = tilesOf(steps);
  for (size_t i = 0; i < now.size(); ++i)
    if (movedStep >= now[i].first && movedStep < now[i].first + now[i].count) return int(i);
  return to;
}

inline void removeTile(std::vector<Step>& steps, int at) {
  const std::vector<Tile> tiles = tilesOf(steps);
  if (at < 0 || at >= int(tiles.size())) return;
  steps.erase(steps.begin() + tiles[at].first, steps.begin() + tiles[at].first + tiles[at].count);
  if (!steps.empty()) steps[0].delayMs = 0;
}

// What plays: the gaps are either all the same or yours, and never too short for a game to notice.
inline std::vector<Step> played(const Recorded& m) {
  std::vector<Step> out = shown(m.steps, m.withTouches);
  for (size_t i = 0; i < out.size(); ++i) {
    if (i == 0) out[i].delayMs = 0;
    else out[i].delayMs = m.asRecorded ? std::max(out[i].delayMs, kShortestMs) : kQuickMs;
  }
  return out;
}

inline void copyName(char* to, size_t size, const std::string& from) { snprintf(to, size, "%s", from.c_str()); }

inline void toWire(const Recorded& m, fhlive::WireMacro& w) {
  w = fhlive::WireMacro{};
  w.hand = uint8_t(m.hand);
  w.onTouch = m.onTouch ? 1 : 0;
  w.repeat = m.repeat ? 1 : 0;
  copyName(w.path, sizeof w.path, m.path);
  const std::vector<Step> steps = played(m);
  w.count = uint8_t(std::min<size_t>(steps.size(), fhlive::kMaxSteps));
  for (size_t i = 0; i < w.count; ++i) {
    fhlive::WireStep& s = w.steps[i];
    s.hand = uint8_t(steps[i].hand);
    s.kind = uint8_t(steps[i].kind);
    s.x = int8_t(steps[i].x);
    s.y = int8_t(steps[i].y);
    s.delayMs = uint16_t(std::min(steps[i].delayMs, 60000));
    copyName(s.path, sizeof s.path, steps[i].path);
  }
}

}  // namespace macro
}  // namespace fh

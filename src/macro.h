// Recorded combos: one button plays back a sequence you did with your own hands, the way a Logitech mouse macro does.
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
  bool asRecorded = false;  // your timing, rather than the same quick gap between every step
  bool withTouches = false;
  std::vector<Step> steps;  // everything that was recorded, touches included
};

constexpr int kQuickMs = 50;    // the gap between steps unless you ask for your own: the same as G HUB's "standard delay"
constexpr int kShortestMs = 20; // long enough for a game running at 60 frames a second to see it
constexpr int kStopAfterMs = 2000;  // recording stops this long after you've let go of everything

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
  // True if that added a step.
  bool feed(const Raw& r) {
    std::string path, part;
    if (!splitName(r.name, path, part)) return false;
    if (path.find("/finger") != std::string::npos || path.find("/input/skeleton") == 0) return false;  // how your fingers are curled isn't a control
    Control& c = controls_[std::to_string(r.hand) + path];
    bool added = false;
    if (part == "click") {
      c.sawClick = true;
      if (r.value > 0.5f && !c.pressed) added = add(r, path, c, Press);
      else if (r.value <= 0.5f && c.pressed) added = add(r, path, c, Release);
    } else if (part == "value" || part == "force") {
      if (r.value > 0.75f && !c.pressed) added = add(r, path, c, Press);
      else if (!c.sawClick && r.value < 0.4f && c.pressed) added = add(r, path, c, Release);
    } else if (part == "touch") {
      if (r.value > 0.5f && !c.touched) added = add(r, path, c, Touch);
      else if (r.value <= 0.5f && c.touched) added = add(r, path, c, Untouch);
    } else if (part == "x" || part == "y") {
      (part == "x" ? c.stickX : c.stickY) = r.value;
      const bool out = std::fabs(c.stickX) > 0.6f || std::fabs(c.stickY) > 0.6f;
      const bool back = std::fabs(c.stickX) < 0.3f && std::fabs(c.stickY) < 0.3f;
      if (out && !c.pushed) added = add(r, path, c, Push);
      else if (back && c.pushed) added = add(r, path, c, Centre);
    }
    if (r.ms > lastMs_) lastMs_ = r.ms;
    return added;
  }

  // Everything's been let go of for long enough, and there's something to keep.
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
    for (const auto& [key, c] : controls_) {
      if (!c.pressed && !c.touched && !c.pushed) continue;
      const int hand = key[0] - '0';
      const std::string path = key.substr(1);
      auto add = [&](int kind) {
        Step s;
        s.hand = hand;
        s.path = path;
        s.kind = kind;
        s.delayMs = kQuickMs;
        out.push_back(s);
      };
      if (c.pressed) add(Release);
      if (c.pushed) add(Centre);
      if (c.touched) add(Untouch);
    }
    return out;
  }

 private:
  static constexpr size_t kFullAt = 60;  // room for the touches that'll get filtered out
  struct Control {
    bool pressed = false, touched = false, pushed = false, sawClick = false;
    float stickX = 0, stickY = 0;
  };
  bool add(const Raw& r, const std::string& path, Control& c, int kind) {
    Step s;
    s.hand = r.hand;
    s.path = path;
    s.kind = kind;
    s.delayMs = steps_.empty() ? 0 : int(std::min<uint32_t>(r.ms - lastStepMs_, 60000));
    if (kind == Push) {
      // The four ways a stick gets pushed on purpose. Halfway between two is rounded to the stronger.
      if (std::fabs(c.stickX) > std::fabs(c.stickY)) s.x = c.stickX > 0 ? 100 : -100;
      else s.y = c.stickY > 0 ? 100 : -100;
    }
    if (kind == Press) c.pressed = true;
    if (kind == Release) c.pressed = false;
    if (kind == Touch) c.touched = true;
    if (kind == Untouch) c.touched = false;
    if (kind == Push) c.pushed = true;
    if (kind == Centre) c.pushed = false;
    steps_.push_back(s);
    lastStepMs_ = r.ms;
    return true;
  }
  // A thumb resting on something can stay there. It's only presses and pushes that have to end, unless that's all you did.
  bool allLetGo() const {
    bool anyPress = false, stillHeld = false, stillTouched = false;
    for (const Step& s : steps_) anyPress = anyPress || s.kind == Press || s.kind == Push;
    for (const auto& [key, c] : controls_) {
      stillHeld = stillHeld || c.pressed || c.pushed;
      stillTouched = stillTouched || c.touched;
    }
    return !stillHeld && (anyPress || !stillTouched);
  }

  std::vector<Step> steps_;
  std::map<std::string, Control> controls_;
  uint32_t lastMs_ = 0, lastStepMs_ = 0;
};

// Which button plays it: the first thing you press, or something you touch and let go of without pressing.
class TriggerPicker {
 public:
  bool feed(const Raw& r) {
    std::string path, part;
    if (done_ || !splitName(r.name, path, part)) return done_;
    if (part == "click" || part == "value" || part == "force") {
      if (r.value > (part == "click" ? 0.5f : 0.75f)) pick(r.hand, path, false);
    } else if (part == "touch") {
      if (r.value > 0.5f) touching_[std::to_string(r.hand) + path] = r.ms;
      else if (touching_.count(std::to_string(r.hand) + path)) pick(r.hand, path, true);
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
  bool done_ = false, onTouch_ = false;
  int hand_ = -1;
  std::string path_;
  std::map<std::string, uint32_t> touching_;
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

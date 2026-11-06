// Recording: what the controllers report going in, and readable steps coming out, including the example from the person who
// asked for this. No VR anywhere.
#include <cstdio>
#include <string>
#include <vector>

#include "macro.h"

using namespace fh;
using namespace fh::macro;

static int fails = 0, checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++fails;
    printf("FAIL  %s\n", what.c_str());
  }
}

static Raw R(uint32_t ms, int hand, const char* name, float v) {
  Raw r;
  r.ms = ms;
  r.hand = hand;
  r.name = name;
  r.value = v;
  return r;
}

static std::string read(const std::vector<Step>& steps) {
  static const char* words[] = {"press", "release", "touch", "untouch", "push", "centre"};
  std::string out;
  for (const Step& s : steps) out += std::string(s.hand ? "R" : "L") + " " + words[s.kind] + " " + s.path.substr(7) + ", ";
  return out;
}

int main() {
  // The example: press A, press trigger, release trigger, release A. Thumb and finger land on them first, the way they do.
  {
    Recorder r;
    r.feed(R(1000, 1, "/input/a/touch", 1));
    r.feed(R(1080, 1, "/input/a/click", 1));
    r.feed(R(1300, 1, "/input/trigger/touch", 1));
    r.feed(R(1340, 1, "/input/trigger/value", 0.4f));
    r.feed(R(1360, 1, "/input/trigger/value", 0.8f));  // the pull gets there before the click reports
    r.feed(R(1365, 1, "/input/trigger/click", 1));
    r.feed(R(1600, 1, "/input/trigger/click", 0));
    r.feed(R(1610, 1, "/input/trigger/value", 0.1f));
    r.feed(R(1650, 1, "/input/trigger/touch", 0));
    r.feed(R(1800, 1, "/input/a/click", 0));
    r.feed(R(1900, 1, "/input/a/touch", 0));
    check(!r.finished(3000), "doesn't stop until 2 seconds after the last thing happened");
    check(r.finished(3950), "stops 2 seconds after you've let go of everything");
    Recorded m;
    m.steps = r.steps();
    check(read(shown(m.steps, false)) == "R press a, R press trigger, R release trigger, R release a, ", "the example reads back as four steps: " + read(shown(m.steps, false)));
    check(read(shown(m.steps, true)) == read(shown(m.steps, false)), "a touch on the way to a press isn't worth a step of its own");
    check(!hasBareTouches(m.steps), "so there's no choice to offer about touches");

    // G HUB's default: the same gap between all of them. Recorded timing has to carry over the touches that were dropped.
    std::vector<Step> quick = played(m);
    check(quick.size() == 4 && quick[0].delayMs == 0 && quick[1].delayMs == kQuickMs && quick[3].delayMs == kQuickMs, "quick timing is the same gap every time");
    m.asRecorded = true;
    std::vector<Step> mine = played(m);
    check(mine[0].delayMs == 0 && mine[1].delayMs == 1360 - 1080, "your own timing is yours, from the press to the next press (" + std::to_string(mine[1].delayMs) + ")");

    fhlive::WireMacro w;
    m.hand = 1;
    m.path = "/input/b";
    macro::toWire(m, w);
    check(w.count == 4 && w.hand == 1 && std::string(w.path) == "/input/b" && std::string(w.steps[1].path) == "/input/trigger" && w.steps[1].kind == fhlive::StepPress && w.steps[3].kind == fhlive::StepRelease, "and it packs for the driver");
  }

  // Your thumb wandering over things isn't a step, unless you ask for it, and a bare touch is kept apart from a press.
  {
    Recorder r;
    r.feed(R(0, 1, "/input/thumbrest/touch", 0));  // already resting there: letting go isn't news
    r.feed(R(100, 1, "/input/thumbstick/touch", 1));
    r.feed(R(200, 1, "/input/thumbstick/touch", 0));
    r.feed(R(300, 1, "/input/x/click", 1));
    r.feed(R(400, 1, "/input/x/click", 0));
    Recorded m;
    m.steps = r.steps();
    check(read(shown(m.steps, false)) == "R press x, R release x, ", "touches stay out by default");
    check(hasBareTouches(m.steps), "but they're there to ask for");
    check(read(shown(m.steps, true)) == "R touch thumbstick, R untouch thumbstick, R press x, R release x, ", "and they come back when you do");
  }

  // Nothing but touches: that's what you meant.
  {
    Recorder r;
    r.feed(R(100, 0, "/input/thumbstick/touch", 1));
    check(!r.finished(5000), "a recording of touches waits until they've let go");
    r.feed(R(500, 0, "/input/thumbstick/touch", 0));
    check(r.finished(2600), "and then finishes");
  }

  // Holding the controller means grip touch is on the whole time, and it mustn't stop a press-only recording ending.
  {
    Recorder r;
    r.feed(R(50, 0, "/input/grip/touch", 1));
    r.feed(R(100, 1, "/input/a/click", 1));
    r.feed(R(200, 1, "/input/a/click", 0));
    check(r.finished(2300), "a resting hand doesn't hold the recording open");
  }

  // A stick is four directions and a let-go, not a stream of numbers.
  {
    Recorder r;
    r.feed(R(0, 1, "/input/thumbstick/y", 0.2f));
    r.feed(R(10, 1, "/input/thumbstick/y", 0.9f));
    r.feed(R(20, 1, "/input/thumbstick/y", 1.0f));
    r.feed(R(30, 1, "/input/thumbstick/x", 0.1f));
    r.feed(R(200, 1, "/input/thumbstick/y", 0.2f));
    r.feed(R(210, 1, "/input/thumbstick/x", 0.0f));
    const std::vector<Step> s = r.steps();
    check(read(s) == "R push thumbstick, R centre thumbstick, ", "one push and one let-go: " + read(s));
    check(s.size() == 2 && s[0].y == 100 && s[0].x == 0, "up is up");
  }

  // Recording cut off with something still down: it's let go at the end, so what's shown is what plays.
  {
    Recorder r;
    r.feed(R(0, 1, "/input/a/click", 1));
    const std::vector<Step> s = r.steps();
    check(read(s) == "R press a, R release a, ", "whatever's held gets let go: " + read(s));
    check(!r.finished(100000), "and it doesn't call itself finished while something's held");
  }

  // A release with no press means it was already held when recording started.
  {
    Recorder r;
    check(!r.feed(R(0, 1, "/input/trigger/click", 0)), "letting go of something from before isn't a step");
    check(!r.finished(5000), "and doesn't count as having recorded anything");
    check(r.empty(), "so it's still empty");
  }

  // Too quick for a game to see gets stretched. A game at 60 frames a second looks every 16 ms.
  {
    Recorded m;
    Step a; a.path = "/input/a"; a.hand = 1; a.kind = Press;
    Step b = a; b.kind = Release; b.delayMs = 3;
    m.steps = {a, b};
    m.asRecorded = true;
    check(played(m)[1].delayMs == kShortestMs, "a 3 ms press plays for at least 20");
  }

  // Your hand on the wrong control is what picks the wrong button, so be clear which it took.
  {
    TriggerPicker p;
    p.feed(R(0, 1, "/input/b/touch", 1));
    check(!p.done(), "touching might still become a press");
    p.feed(R(50, 1, "/input/b/click", 1));
    check(p.done() && p.hand() == 1 && p.path() == "/input/b" && !p.onTouch(), "pressing picks it as a press");
    TriggerPicker q;
    q.feed(R(0, 0, "/input/thumbrest/touch", 1));
    q.feed(R(300, 0, "/input/thumbrest/touch", 0));
    check(q.done() && q.onTouch() && q.path() == "/input/thumbrest", "touching and letting go picks it as a touch");
    TriggerPicker t;
    t.feed(R(0, 1, "/input/trigger/value", 0.3f));
    check(!t.done(), "a light pull on the trigger isn't a press yet");
    t.feed(R(10, 1, "/input/trigger/value", 0.9f));
    check(t.done() && t.path() == "/input/trigger", "a full one is");
    TriggerPicker u;
    u.feed(R(0, 1, "/input/a/touch", 0));
    check(!u.done(), "letting go of something touched before it started picks nothing");
  }

  // Names that aren't controls at all.
  {
    Recorder r;
    check(!r.feed(R(0, 1, "/proximity", 1)), "the headset's proximity sensor isn't a button");
    check(!r.feed(R(0, 1, "/input/finger/index/value", 1)), "how a finger's curled isn't a button");
  }

  printf("%s: %d checks, %d failures\n", fails ? "FAILED" : "PASSED", checks, fails);
  return fails ? 1 : 0;
}

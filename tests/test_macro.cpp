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

    // Your own timing is the normal setting. J4, 7 November: "Quick is too quick."
    check(m.asRecorded, "a new one plays the way you did it");
    // G HUB's default: the same gap between all of them. Recorded timing has to carry over the touches that were dropped.
    m.asRecorded = false;
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

  // The way you really hold it. A grip and a finger on the trigger from before it started aren't steps, and don't keep it open.
  {
    Recorder r;
    r.begin(1000);
    check(!r.feed(R(1020, 1, "/input/grip/value", 0.95f)), "a firm grip from before it started isn't a step");
    check(!r.feed(R(1030, 1, "/input/grip/click", 1)), "nor is its click");
    check(!r.feed(R(1040, 0, "/input/trigger/value", 0.9f)), "nor a trigger already pulled");
    check(!r.feed(R(1050, 1, "/input/a/touch", 1)), "nor a thumb already resting");
    r.feed(R(1500, 1, "/input/grip/value", 0.97f));
    r.feed(R(1600, 1, "/input/grip/click", 1));
    r.feed(R(2000, 1, "/input/a/click", 1));
    r.feed(R(2100, 1, "/input/a/click", 0));
    r.feed(R(2600, 1, "/input/y/click", 1));
    r.feed(R(2700, 1, "/input/y/click", 0));
    r.feed(R(3500, 1, "/input/grip/value", 0.92f));
    check(read(shown(r.steps(), false)) == "R press a, R release a, R press y, R release y, ", "only the presses show: " + read(shown(r.steps(), false)));
    check(!r.finished(4600), "it waits two seconds from the last press");
    check(r.finished(4700), "and stops then, with the grip still held");
    r.feed(R(5000, 1, "/input/grip/value", 0.1f));
    r.feed(R(5000, 1, "/input/grip/click", 0));
    check(read(shown(r.steps(), false)) == "R press a, R release a, R press y, R release y, ", "and letting go afterwards adds nothing");
  }

  // Squeezed past the line during the take: it shows, since you did it, but it doesn't hold the recording open.
  {
    Recorder r;
    r.begin(0);
    r.feed(R(500, 1, "/input/grip/value", 0.3f));
    r.feed(R(600, 1, "/input/a/click", 1));
    r.feed(R(700, 1, "/input/grip/value", 0.8f));
    r.feed(R(800, 1, "/input/a/click", 0));
    check(r.finished(2800), "a squeeze you never let go of doesn't stop it finishing");
    r.feed(R(2800, 1, "/input/grip/value", 0.8f));
    check(read(r.steps()) == "R press a, R press grip, R release a, R release grip, ", "and it's let go of at the end: " + read(r.steps()));
    check(r.steps().back().delayMs == 2000, "after as long as it was really held (" + std::to_string(r.steps().back().delayMs) + ")");
  }

  // A button left down is different: nobody holds A by accident.
  {
    Recorder r;
    r.begin(0);
    r.feed(R(500, 1, "/input/a/click", 1));
    check(!r.finished(5000), "a button still held keeps it open");
  }

  // A click reading up while the pull's still in, the way the driver reports every button each time, isn't a let-go.
  {
    Recorder r;
    r.begin(0);
    r.feed(R(500, 1, "/input/trigger/value", 0.5f));
    r.feed(R(510, 1, "/input/trigger/value", 0.8f));
    for (uint32_t t = 520; t < 700; t += 10) {
      r.feed(R(t, 1, "/input/trigger/click", 0));
      r.feed(R(t + 5, 1, "/input/trigger/value", 0.8f));
    }
    check(r.soFar().size() == 1, "no flood of tiles (" + std::to_string(r.soFar().size()) + ")");
    r.feed(R(800, 1, "/input/trigger/value", 0.1f));
    check(read(r.soFar()) == "R press trigger, R release trigger, ", "one press: " + read(r.soFar()));
    // And an Index grip: the fingers read in while the squeeze reads nothing.
    Recorder i;
    i.begin(0);
    i.feed(R(500, 0, "/input/grip/value", 0.5f));
    i.feed(R(510, 0, "/input/grip/value", 0.9f));
    i.feed(R(520, 0, "/input/grip/force", 0));
    i.feed(R(530, 0, "/input/grip/value", 0.95f));
    check(read(i.soFar()) == "L press grip, ", "the force reading nothing doesn't end it: " + read(i.soFar()));
  }

  // Thumb already resting when it started: lifting off isn't a step, and neither is a stick your thumb was already holding over.
  {
    Recorder r;
    r.begin(0);
    r.feed(R(30, 1, "/input/a/touch", 1));
    r.feed(R(40, 1, "/input/thumbstick/x", 0.9f));
    check(r.empty(), "nothing yet");
    r.feed(R(800, 1, "/input/a/touch", 0));
    r.feed(R(800, 1, "/input/thumbstick/x", 0.0f));
    check(r.empty(), "and still nothing");
    r.feed(R(900, 1, "/input/a/touch", 1));
    check(!r.empty(), "but after that a new touch is new");
  }

  // One tile per press, split only when something else happens in between.
  {
    const auto step = [](int kind, const char* path, int delay = 100) {
      Step s;
      s.hand = 1;
      s.path = path;
      s.kind = kind;
      s.delayMs = delay;
      return s;
    };
    std::vector<Step> s = {step(Press, "/input/a", 0), step(Release, "/input/a", 50), step(Press, "/input/y", 300), step(Release, "/input/y", 60)};
    std::vector<Tile> t = tilesOf(s);
    check(t.size() == 2 && t[0].count == 2 && t[1].first == 2, "two presses are two tiles");
    s = {step(Press, "/input/a", 0), step(Press, "/input/trigger", 250), step(Release, "/input/trigger", 190), step(Release, "/input/a", 210)};
    t = tilesOf(s);
    check(t.size() == 3 && t[0].count == 1 && t[1].count == 2 && t[2].count == 1, "holding A round a trigger pull splits A, and only A");
    s = {step(Press, "/input/a", 0), step(Release, "/input/b", 50)};
    check(tilesOf(s).size() == 2, "something else's let-go doesn't join up");
    s = {step(Push, "/input/thumbstick", 0), step(Centre, "/input/thumbstick", 50), step(Touch, "/input/b", 90), step(Untouch, "/input/b", 40)};
    t = tilesOf(s);
    check(t.size() == 2 && t[0].count == 2 && t[1].count == 2, "a stick and a touch go the same way");

    // Moving one: what's inside goes with it, and the gaps between tiles stay where they were.
    s = {step(Press, "/input/a", 0), step(Release, "/input/a", 50), step(Press, "/input/y", 300), step(Release, "/input/y", 60), step(Press, "/input/b", 700), step(Release, "/input/b", 70)};
    int now = moveTile(s, 2, -1);
    check(now == 1 && read(s) == "R press a, R release a, R press b, R release b, R press y, R release y, ", "a tile moves whole: " + read(s));
    check(s[2].delayMs == 300 && s[3].delayMs == 70 && s[4].delayMs == 700 && s[5].delayMs == 60, "and its press stays as long as it was");
    now = moveTile(s, 1, 1);
    check(now == 2 && read(s) == "R press a, R release a, R press y, R release y, R press b, R release b, ", "and comes back: " + read(s));
    check(moveTile(s, 0, -1) == 0 && moveTile(s, 2, 1) == 2, "and it can't go off either end");
    // Moving what was in the middle of a held press lets the press join back up.
    s = {step(Press, "/input/a", 0), step(Press, "/input/trigger", 250), step(Release, "/input/trigger", 190), step(Release, "/input/a", 210)};
    now = moveTile(s, 1, -1);
    check(now == 0 && tilesOf(s).size() == 2 && s[0].path == "/input/trigger" && s[0].delayMs == 0 && s[2].delayMs == 250, "and a split press joins back up");
    // Deleting takes the whole press.
    s = {step(Press, "/input/a", 0), step(Release, "/input/a", 50), step(Press, "/input/y", 300), step(Release, "/input/y", 60)};
    removeTile(s, 0);
    check(read(s) == "R press y, R release y, " && s[0].delayMs == 0, "deleting takes the press and its let-go: " + read(s));
    removeTile(s, 5);
    check(s.size() == 2, "and one that isn't there does nothing");
  }

  // Sensors out of an old take: the time they took goes to the step after.
  {
    Step a; a.hand = 1; a.path = "/input/capsense"; a.kind = Push;
    Step b; b.hand = 1; b.path = "/input/a"; b.kind = Press; b.delayMs = 400;
    Step c; c.hand = 1; c.path = "/input/capsense"; c.kind = Centre; c.delayMs = 30;
    Step d; d.hand = 1; d.path = "/input/a"; d.kind = Release; d.delayMs = 50;
    const std::vector<Step> kept = onlyWhere(std::vector<Step>{a, b, c, d}, [](const Step& s) { return s.path == "/input/a"; });
    check(read(kept) == "R press a, R release a, " && kept[0].delayMs == 0 && kept[1].delayMs == 80, "they come out and the timing holds");
  }

  // Choosing a button with your hand already on the controller.
  {
    TriggerPicker p;
    p.begin(1000);
    p.feed(R(1020, 1, "/input/grip/value", 0.95f));
    p.feed(R(1030, 1, "/input/grip/click", 1));
    p.feed(R(1040, 1, "/input/a/touch", 1));
    check(!p.done(), "what's already held isn't what you're choosing");
    p.feed(R(1500, 1, "/input/grip/value", 0.97f));
    p.feed(R(1510, 1, "/input/grip/click", 1));
    check(!p.done(), "and it stays that way while you go on holding it");
    p.feed(R(1600, 1, "/input/a/touch", 0));
    check(!p.done(), "lifting a thumb that was already resting picks nothing");
    p.feed(R(1800, 1, "/input/b/click", 1));
    check(p.done() && p.path() == "/input/b" && !p.onTouch(), "B does");
    TriggerPicker q;
    q.begin(0);
    q.feed(R(20, 1, "/input/grip/value", 0.95f));
    q.feed(R(500, 1, "/input/grip/value", 0.2f));
    q.feed(R(800, 1, "/input/grip/value", 0.9f));
    check(q.done() && q.path() == "/input/grip", "and once it's been let go of, squeezing it again is a choice");
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

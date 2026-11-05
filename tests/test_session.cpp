// Session guarantees: the ghost starts exactly on the matched game hand, moves 1:1 with the
// in-game hand while placing, matching never changes offsets, and the driver gets exactly what was placed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "live.h"
#include "ghost3d.h"
#include "placement.h"
#include "platform.h"
#include "store.h"

using namespace fh;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool same(const M& a, const M& b, float eps = 5e-3f) {
  for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) if (std::fabs(a.m[i][j] - b.m[i][j]) > eps) return false;
  return true;
}
static std::mt19937 rng(77);
static float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
static Offset randOff(float p = 0.1f, float r = 170.f) {
  Offset o;
  for (int k = 0; k < 3; ++k) { o.pos[k] = U(-p, p); o.rot[k] = U(-r, r); }
  o.rot[0] = U(-85, 85);
  return o;
}
static M randPose() { M m = fromOffset(randOff(0.f)); m.m[0][3] = U(-1, 1); m.m[1][3] = U(0.6f, 1.8f); m.m[2][3] = U(-1, 1); return m; }

int main() {
  // A. Placing: the ghost follows the grabbing hand exactly, live and binding route, with a matched
  //    or a mirrored resting pose, through any twist.
  for (int i = 0; i < 4000; ++i) {
    bool live = i & 1;
    M grip = randPose();
    M raw = mul(grip, fromOffset(randOff(0.05f, 60)));   // raw differs from grip by a device transform
    Offset base[2] = {randOff(0.06f, 120), randOff(0.06f, 120)};
    bool matched[2] = {(i & 2) != 0, true};
    Offset B = ghostBaseFor(base, matched, 0);           // left: matched or mirrored from right
    Offset O = randOff(0.08f);
    M W0 = mul(gripWithOffset(live, raw, grip, O), fromOffset(B));
    M Ot = randPose();
    M taken = mul(inv(Ot), W0);
    M Ot2 = mul(Ot, fromOffset(randOff(0.05f)));         // move + twist the grabbing hand
    M W = mul(Ot2, taken);
    Offset O2 = offsetFromHeld(live, raw, grip, W, B);
    CHECK(same(mul(gripWithOffset(live, raw, grip, O2), fromOffset(B)), W), "ghost doesn't follow the hand (sample %d, live %d)", i, live);
  }

  // A2. Live games: you hold the in-game hand itself (no ghost) and it follows exactly; letting go
  //     outside the bubble puts the hand exactly on the bubble's surface with its rotation kept.
  for (int i = 0; i < 3000; ++i) {
    M grip = randPose();
    Offset O = randOff(0.05f);
    M G = gripWithOffset(true, grip, grip, O);          // where the game draws the hand
    M Ot = randPose();
    M taken = mul(inv(Ot), G);
    M W = mul(mul(Ot, fromOffset(randOff(0.20f))), taken);  // drag up to ~35 cm, any twist
    Offset O2 = offsetFromHeld(true, grip, grip, W, Offset{});
    CHECK(same(gripWithOffset(true, grip, grip, O2), W), "live hand doesn't follow (sample %d)", i);
    float r = U(0.10f, 0.30f);
    Offset C = clampOffset(O2, r, 180.f);
    float anchor = len(pos(gripWithOffset(true, grip, grip, C)) - pos(grip));
    CHECK(anchor <= r + 1e-4f, "hand outside the bubble after let-go (%.3f > %.3f)", anchor, r);
    if (posLen(O2) > r) CHECK(std::fabs(anchor - r) < 1e-4f, "pulled-back hand should sit on the bubble surface");
    for (int k = 0; k < 3; ++k) CHECK(C.rot[k] == O2.rot[k], "pull-back changed rotation");
  }

  // B. Matching: the resting pose follows the grabbing hand exactly and never changes the offset.
  for (int i = 0; i < 2000; ++i) {
    M G = mul(randPose(), fromOffset(randOff(0.08f)));
    Offset B0 = randOff(0.06f, 120);
    M Ot = randPose();
    M taken = mul(inv(Ot), mul(G, fromOffset(B0)));
    M W = mul(mul(Ot, fromOffset(randOff(0.05f))), taken);
    Offset B1 = baseFromHeld(G, W);
    CHECK(same(mul(G, fromOffset(B1)), W), "match doesn't follow the hand (sample %d)", i);
  }

  // C. After matching, at zero offset the ghost sits exactly on the game hand wherever the controller goes.
  //    (Game hand = grip * H; matching solves base = H from where the user lined it up.)
  for (int i = 0; i < 2000; ++i) {
    M H = fromOffset(randOff(0.06f, 120));  // the game's own hand placement relative to its grip
    M gripAtMatch = randPose();
    M lined = mul(gripAtMatch, H);          // user lines the ghost up on the game hand
    Offset base = baseFromHeld(gripWithOffset(true, gripAtMatch, gripAtMatch, Offset{}), lined);
    M gripLater = randPose();
    M ghostLater = mul(gripWithOffset(true, gripLater, gripLater, Offset{}), fromOffset(base));
    CHECK(same(ghostLater, mul(gripLater, H)), "ghost not on the game hand at zero offset (sample %d)", i);
    // and with any offset, ghost == the game's hand after the offset is applied
    Offset O = randOff(0.1f);
    M ghostO = mul(gripWithOffset(true, gripLater, gripLater, O), fromOffset(base));
    M gameO = mul(mul(gripLater, fromOffset(O)), H);  // grip' = grip * O, game draws grip' * H
    CHECK(same(ghostO, gameO), "ghost and game hand disagree under an offset (sample %d)", i);
  }

  // D. Mirroring left <-> right.
  for (int i = 0; i < 2000; ++i) {
    Offset o = randOff(0.08f);
    CHECK(same(fromOffset(mirrorOffset(mirrorOffset(o))), fromOffset(o)), "mirror twice != identity");
    M m = mirrorX(fromOffset(o));
    float det = m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) - m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
                m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
    CHECK(std::fabs(det - 1) < 1e-3f, "mirror isn't a proper rotation");
  }
  {
    V3 l[31], r[31];
    gripHandJoints(true, l);
    gripHandJoints(false, r);
    float worst = 0;
    for (int k = 0; k < 26; ++k) worst = std::max(worst, len(V3{-l[k].x, l[k].y, l[k].z} - r[k]));
    CHECK(worst < 0.004f, "SteamVR's right hand isn't the mirror of the left (worst joint %.1f mm)", worst * 1000);
  }

  // E. What the driver is handed.
  {
    std::string home = (std::filesystem::temp_directory_path() / "fh-test-session").string();
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
#ifdef _WIN32
    _putenv_s("LOCALAPPDATA", home.c_str());
#else
    setenv("HOME", home.c_str(), 1);
#endif
    // It gets the move in the device's own frame: raw * want * C == raw * C * O.
    {
      fhlive::Shared* m = platform::mapLive();
      for (int i = 0; i < 3000; ++i) {
        Store d;
        d.useDriver = true;
        d.scenePid = 4242;
        d.sceneKey = "steam.app.1";
        d.device[0] = 3; d.device[1] = 4;
        d.gripFromRaw[0] = fromOffset(randOff(0.12f, 60)); d.gripFromRaw[1] = fromOffset(randOff(0.12f, 60));
        d.global.left = randOff(0.15f); d.global.right = randOff(0.15f);
        d.publishLive();
        CHECK(m->driver.magic == fhlive::kDriverMagic && m->driver.device[0] == 3 && m->driver.device[1] == 4, "driver channel header");
        M raw = randPose();
        for (int h = 0; h < 2; ++h) {
          M want = fromPosQuat(m->driver.want[h].pos, m->driver.want[h].rot);
          const Offset& o = h ? d.global.right : d.global.left;
          M C = d.gripFromRaw[h];
          CHECK(same(mul(mul(raw, want), C), mul(mul(raw, C), fromOffset(o)), 1e-3f), "driver offset doesn't match the grip offset (sample %d)", i);
        }
      }
      Store off;
      off.global.left = randOff(0.15f);
      off.publishLive();
      float q[4] = {m->driver.want[0].rot[0], m->driver.want[0].rot[1], m->driver.want[0].rot[2], m->driver.want[0].rot[3]};
      CHECK(!m->driver.want[0].pos[0] && !m->driver.want[0].pos[1] && !m->driver.want[0].pos[2] && std::fabs(std::fabs(q[3]) - 1) < 1e-5f,
            "with the driver not in use, it's given no offset");
      // and the overlay can undo what the driver reports, to find the real controller
      m->driver.applied[1] = {{0.01f, 0.02f, -0.03f}, {0, 0, 0.70710678f, 0.70710678f}};
      M back = Store::appliedDeviceOffset(1);
      CHECK(std::fabs(back.m[0][3] - 0.01f) < 1e-6f && std::fabs(back.m[0][1] + 1.f) < 1e-5f, "applied offset readback");
      m->driver.applied[1] = {{0, 0, 0}, {0, 0, 0, 1}};
      // With the dashboard open the driver stands aside, so SteamVR's pointers come from the real controller.
      Store dash;
      dash.useDriver = true;
      dash.scenePid = 4242;
      dash.sceneKey = "steam.app.1";
      dash.global.left = randOff(0.15f);
      dash.dashboardUp = true;
      dash.publishLive();
      CHECK(!m->driver.want[0].pos[0] && !m->driver.want[0].pos[1] && !m->driver.want[0].pos[2] && std::fabs(std::fabs(m->driver.want[0].rot[3]) - 1) < 1e-5f,
            "the dashboard's pointers must not carry the offset");
      dash.dashboardUp = false;
      dash.publishLive();
      CHECK(std::fabs(m->driver.want[0].pos[0]) + std::fabs(m->driver.want[0].pos[1]) + std::fabs(m->driver.want[0].pos[2]) > 0, "closing it brings the offset back");
      // With no game running there are no hands to fix, and SteamVR's pointers still have to point true.
      dash.scenePid = 0;
      dash.publishLive();
      CHECK(!m->driver.want[0].pos[0] && !m->driver.want[0].pos[1] && !m->driver.want[0].pos[2] && std::fabs(std::fabs(m->driver.want[0].rot[3]) - 1) < 1e-5f,
            "no game: nothing may be moved");
      dash.scenePid = 4242;
      dash.publishLive();
      CHECK(std::fabs(m->driver.want[0].pos[0]) + std::fabs(m->driver.want[0].pos[1]) + std::fabs(m->driver.want[0].pos[2]) > 0, "a game starting brings the offset back");
      // A scene that isn't a game, like Steam Link when a Frame streams from a PC: the PC's SteamVR has its own offsets.
      dash.sceneKey.clear();
      dash.publishLive();
      CHECK(!m->driver.want[0].pos[0] && !m->driver.want[0].pos[1] && !m->driver.want[0].pos[2] && std::fabs(std::fabs(m->driver.want[0].rot[3]) - 1) < 1e-5f,
            "a scene with no game key: nothing may be moved");
      dash.sceneKey = "steam.app.1";
      // The driver's report of where the controller really is.
      M truth;
      m->driver.trueSeq[1].store(0);
      CHECK(!Store::driverTruePose(1, &truth), "no report yet");
      Store::askTruePoses(true);
      CHECK(m->driver.wantTrue.load() == 1, "asked for true poses");
      m->driver.truePose[1] = {{0.5f, 1.2f, -0.3f}, {0, 0, 0.70710678f, 0.70710678f}};
      m->driver.trueSeq[1].store(2);
      CHECK(Store::driverTruePose(1, &truth) && std::fabs(truth.m[1][3] - 1.2f) < 1e-6f && std::fabs(truth.m[0][1] + 1.f) < 1e-5f, "true pose read");
      m->driver.trueSeq[1].store(3);
      CHECK(!Store::driverTruePose(1, &truth), "a report being written isn't read");
      m->driver.trueSeq[1].store(0);
      Store::askTruePoses(false);
      CHECK(m->driver.wantTrue.load() == 0, "stopped asking");
      // Placing through the driver, closed loop, with the real maths and the real publish. The pretend SteamVR
      // reports the true controller plus what was published some updates ago, and its raw and grip poses can be on
      // different offsets. In the headset that ran away and went NaN within seconds. None of it may reach the offset.
      const int lags[][2] = {{0, 0}, {1, 1}, {2, 1}, {1, 2}, {3, 2}, {2, 3}, {6, 1}, {1, 6}};
      for (const auto& lag : lags) {
        Store d;
        d.useDriver = true;
        d.scenePid = 4242;
        d.sceneKey = "steam.app.1";
        d.device[0] = 3; d.device[1] = 4;
        const M table[2] = {mul(translate(-0.0031f, -0.0043f, 0.0995f), rotX(2.81f)), mul(translate(0.0031f, -0.0043f, 0.0995f), rotX(2.81f))};
        d.gripFromRaw[0] = table[0]; d.gripFromRaw[1] = table[1];
        const M standingFromRaw = randPose(), fit0 = randPose(), grab0 = randPose();
        HandPair work;
        M taken = ident();
        std::vector<M> sent(8, ident());
        float worst = 0;
        for (int t = 0; t < 2700; ++t) {  // 30 seconds at 90 updates a second
          float a = t * 0.02f;
          M fitTrue = mul(fit0, mul(translate(0.02f * std::sin(a * 0.9f), 0, 0.01f * std::sin(a)), rotX(5 * std::sin(a * 0.6f))));
          M grabTrue = mul(grab0, mul(translate(0.08f * std::sin(a), 0.05f * std::sin(a * 1.3f), 0.06f * std::sin(a * 0.7f)), rotX(30 * std::sin(a * 0.5f))));
          ControllerPoses F, Ot;
          F.valid = F.gripValid = Ot.valid = Ot.gripValid = true;
          F.raw = mul(mul(standingFromRaw, fitTrue), sent[lag[0]]);
          F.grip = mul(mul(mul(standingFromRaw, fitTrue), sent[lag[1]]), table[0]);
          Ot.raw = mul(standingFromRaw, grabTrue);
          Ot.grip = mul(Ot.raw, table[1]);
          trueControllerPoses(F, d.gripFromRaw[0], true, &fitTrue, standingFromRaw);
          trueControllerPoses(Ot, d.gripFromRaw[1], true, &grabTrue, standingFromRaw);
          if (t == 0) taken = mul(inv(Ot.grip), gripWithOffset(true, F.raw, F.grip, work.left));
          M W = mul(Ot.grip, taken);
          work.left = offsetFromHeld(true, F.raw, F.grip, W, Offset{});
          d.publishLive(&work);
          M want = fromPosQuat(m->driver.want[0].pos, m->driver.want[0].rot);
          sent.insert(sent.begin(), want);
          sent.pop_back();
          M gameGrip = mul(mul(mul(standingFromRaw, fitTrue), want), table[0]);
          worst = std::max(worst, dist(gameGrip, W));
          CHECK(finite(work.left) && finite(want), "the offset went bad at update %d, lag %d/%d", t, lag[0], lag[1]);
          CHECK(same(gameGrip, W, 2e-3f), "the game's grip left the grabbing hand at update %d, lag %d/%d", t, lag[0], lag[1]);
          CHECK(!memcmp(&d.gripFromRaw[0], &table[0], sizeof(M)), "the grip frame must not be measured from moved poses");
        }
        CHECK(worst < 1e-3f, "worst gap %.5f m, lag %d/%d", worst, lag[0], lag[1]);
      }
      // No report from the driver means no pose, rather than a guess from the moved one.
      {
        ControllerPoses H;
        H.valid = H.gripValid = true;
        M C = ident();
        trueControllerPoses(H, C, true, nullptr, ident());
        CHECK(!H.valid, "no report: the hand waits");
        // With no driver the poses are untouched, so the grip frame can be read from them.
        H.valid = H.gripValid = true;
        H.raw = randPose();
        M grip = fromOffset(randOff(0.1f, 40));
        H.grip = mul(H.raw, grip);
        M learned = ident();
        trueControllerPoses(H, learned, false, nullptr, ident());
        CHECK(same(learned, grip, 1e-3f), "no driver: grip frame learned from the poses");
      }
    }
    // profile round trip, including the ghost resting pose
    Store s;
    s.ghost[0] = randOff(0.05f, 90); s.ghostMatched[0] = true;
    CHECK(s.save(), "save");
    Store t; CHECK(t.load(), "load");
    CHECK(t.ghostMatched[0] && !t.ghostMatched[1] && same(fromOffset(t.ghost[0]), fromOffset(s.ghost[0]), 1e-4f), "ghost pose round trip");
    CHECK(same(fromOffset(t.ghostBase(1)), fromOffset(mirrorOffset(s.ghost[0])), 1e-3f), "unmatched right mirrors left");
    CHECK(t.max_rot >= 180.f, "rotation must never be limited");
  }

  // F. The name a game without a Steam app id is saved under.
  {
    std::string k = platform::processExeKey(platform::currentPid());
    CHECK(k == "exe.test_session", "exe key is the lowercase name without .exe (got '%s')", k.c_str());
#ifndef _WIN32
    using std::string;
    auto nul = [](std::initializer_list<const char*> parts) { string s; for (const char* p : parts) { s += p; s += '\0'; } return s; };
    CHECK(platform::exeKeyFrom(nul({"PATH=/usr/bin", "SteamAppId=448280"}), nul({"app_process64"})) == "steam.app.448280", "Steam's app id wins");
    CHECK(platform::exeKeyFrom(nul({"SteamAppId=0"}), nul({"/opt/thing/Thing"})) == "exe.thing", "an app id of 0 means none");
    CHECK(platform::exeKeyFrom(nul({"SteamAppId=abc"}), nul({"/opt/thing/Thing"})) == "exe.thing", "an app id has to be a number");
    CHECK(platform::exeKeyFrom("", nul({"/usr/bin/FEXInterpreter", "/home/steamos/wine64", "Z:\\Games\\Big Game\\Game.EXE", "-vr"})) == "exe.game", "past FEX and Wine to the game");
    CHECK(platform::exeKeyFrom("", nul({"C:\\Games\\Thing.exe"})) == "exe.thing", "a Windows path");
    CHECK(platform::exeKeyFrom("", "") == "", "a process that can't be read");
#endif
  }

  printf("%s: %d checks, %d failures\n", fails ? "FAILED" : "PASSED", checks, fails);
  return fails ? 1 : 0;
}

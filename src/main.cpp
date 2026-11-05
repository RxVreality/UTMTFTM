// UTMTFTM ("use the medium to fix the medium"): grab-to-place controller offsets, per game or global, for SteamVR on Windows.
// Grab one hand with the other, place it, let go and it stays; let go beyond the distance limit and it's pulled back.
// All drawing lives in ui.cpp (no VR calls) so every screen is rendered and checked offline.
#include <openvr.h>

#include <csignal>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "controller_vr.h"
#include "controls.h"
#include "d3dupload.h"
#include "ghost3d.h"
#include "glove.h"
#include "placement.h"
#include "platform.h"
#include "store.h"
#include "ui.h"
#include "xform.h"

using namespace fh;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static std::string g_root;  // install dir (contains input/, build/)

static volatile sig_atomic_t g_stop = 0;  // SIGTERM/SIGINT: shut down cleanly instead of dying mid-upload
static void onSignal(int) { g_stop = 1; }

// SteamVR's Manage Add-ons switches the driver on and off, and safe mode can block it. SteamVR starts this program
// at launch either way, because auto-launch isn't tied to the add-on, so it has to check and leave by itself.
// A setting that isn't there yet counts as on.
static bool addOnEnabled() {
  vr::EVRSettingsError err = vr::VRSettingsError_None;
  bool on = vr::VRSettings()->GetBool("driver_utmtftm", vr::k_pch_Driver_Enable_Bool, &err);
  if (err == vr::VRSettingsError_None && !on) return false;
  bool blocked = vr::VRSettings()->GetBool("driver_utmtftm", vr::k_pch_Driver_BlockedBySafemode_Bool, &err);
  return !(err == vr::VRSettingsError_None && blocked);
}

// ---------------------------------------------------------------- input
struct Hand : ControllerPoses {
  bool grab = false, grabEdge = false, done = false, reset = false, cancel = false;
};

struct Input {
  vr::VRActionSetHandle_t set = 0;
  vr::VRActionHandle_t aGrab = 0, aDone = 0, aReset = 0, aCancel = 0, aRaw = 0, aGrip = 0, aHaptic = 0;
  vr::VRInputValueHandle_t src[2] = {0, 0};
  Hand h[2];
  bool ok = false;

  bool init() {
    std::string manifest = fs::path(g_root + "/input/actions.json").make_preferred().string();
    auto e = vr::VRInput()->SetActionManifestPath(manifest.c_str());
    if (e != vr::VRInputError_None) { logf("SetActionManifestPath(%s) failed: %d", manifest.c_str(), (int)e); return false; }
    auto I = vr::VRInput();
    I->GetActionSetHandle("/actions/fh", &set);
    I->GetActionHandle("/actions/fh/in/grab", &aGrab);
    I->GetActionHandle("/actions/fh/in/done", &aDone);
    I->GetActionHandle("/actions/fh/in/reset", &aReset);
    I->GetActionHandle("/actions/fh/in/cancel", &aCancel);
    I->GetActionHandle("/actions/fh/in/raw", &aRaw);
    I->GetActionHandle("/actions/fh/in/grip", &aGrip);
    I->GetActionHandle("/actions/fh/out/haptic", &aHaptic);
    I->GetInputSourceHandle("/user/hand/left", &src[0]);
    I->GetInputSourceHandle("/user/hand/right", &src[1]);
    return ok = true;
  }

  bool edge(vr::VRActionHandle_t a, int i, bool* state = nullptr) {
    vr::InputDigitalActionData_t d{};
    vr::VRInput()->GetDigitalActionData(a, &d, sizeof d, src[i]);
    if (state) *state = d.bActive && d.bState;
    return d.bActive && d.bChanged && d.bState;
  }

  // exclusive: while placing, the grips, buttons and sticks belong to this tool and the game doesn't
  // see them (so you can't grab a door or walk off mid-placement). Poses still reach the game.
  void poll(bool exclusive) {
    if (!ok) return;
    vr::VRActiveActionSet_t as{}; as.ulActionSet = set;
    as.nPriority = exclusive ? vr::k_nActionSetOverlayGlobalPriorityMin : 0;
    vr::VRInput()->UpdateActionState(&as, sizeof as, 1);
    for (int i = 0; i < 2; ++i) {
      Hand& H = h[i];
      vr::InputPoseActionData_t pr{}, pg{};
      vr::VRInput()->GetPoseActionDataForNextFrame(aRaw, vr::TrackingUniverseStanding, &pr, sizeof pr, src[i]);
      vr::VRInput()->GetPoseActionDataForNextFrame(aGrip, vr::TrackingUniverseStanding, &pg, sizeof pg, src[i]);
      H.valid = pr.bActive && pr.pose.bPoseIsValid;
      if (H.valid) {
        H.raw = pr.pose.mDeviceToAbsoluteTracking;
        H.gripValid = pg.bActive && pg.pose.bPoseIsValid;
        H.grip = H.gripValid ? pg.pose.mDeviceToAbsoluteTracking : H.raw;
      }
      H.grabEdge = edge(aGrab, i, &H.grab);
      H.done = edge(aDone, i);
      H.reset = edge(aReset, i);
      H.cancel = edge(aCancel, i);
    }
  }

  // Give the controls back to the game. SteamVR keeps the last active set, so this has to be said.
  void release() {
    if (!ok) return;
    vr::VRActiveActionSet_t as{}; as.ulActionSet = set;
    vr::VRInput()->UpdateActionState(&as, sizeof as, 1);
  }

  void buzz(int i, float amp = 0.5f, float dur = 0.04f) {
    if (ok) vr::VRInput()->TriggerHapticVibrationAction(aHaptic, 0, dur, 160, amp, src[i]);
  }
};

// ---------------------------------------------------------------- app
enum class Mode { Idle, Armed, Placing };
struct Step { int hand; bool match; };

static constexpr float kGhostS = 0.36f;    // metres covered by the stereo ghost quad
static constexpr float kGrabReach = 0.16f; // how close your hand must be to grab the hand being placed
static constexpr float kLostHand = 1.0f;   // an offset this far out is a fault, not a placement: keep the last good one

struct App {
  Store store;
  Input in;
  bool saveForGame = true;
  Mode mode = Mode::Idle;
  bool quit = false;
  Clock::time_point lastDashEvent{};  // the laser was last on the panel
  bool mouseLogged = false;
  std::string modelName[2];  // the render model each hand was last seen on
  bool modelPending[2] = {false, false};
  ModelLoader modelLoader[2];

  std::string sceneKey, sceneName;
  uint32_t scenePid = 0;
  bool driverOn = false;   // the driver add-on is loaded in SteamVR and seeing poses
  bool driverCounted = false;
  bool askedTrue = false;   // the driver has been asked to report the true controller poses
  bool truePoseSeen[2] = {false, false};
  float poseCheckCm[2] = {0, 0}, poseCheckDeg[2] = {0, 0};
  int badOffsets = 0;       // offsets thrown out during this grab because they were broken or absurdly far off
  uint64_t driverCount = 0;
  Clock::time_point lastSceneCheck{};

  // placement session
  std::vector<Step> steps;
  size_t si = 0;
  HandPair start, work;               // offsets
  Offset startBase[2], workBase[2];   // ghost resting pose
  bool startMatched[2] = {false, false}, workMatched[2] = {false, false};
  bool anyPlace = false, anyMatch = false;
  bool held = false, movedOnce = false;
  M taken = ident();
  bool animating = false;
  Offset animFrom, animTo;
  float animT = 0;
  std::string placingFor;  // app key, or "" = all games
  bool live = false;       // the driver reaches the running game: the real hand moves too

  // overlays
  vr::VROverlayHandle_t dash = 0, thumb = 0, ghost = 0, hint = 0;
  Canvas dashCv{1200, 760}, thumbCv{128, 128}, hintCv{820, 300};
  GhostRenderer ghost3d{432, 432};
  Mesh handMesh;
  int handMeshFor = -1;
  V3 joints[31];
  GhostRenderer::Plane plane{};
  Mesh bubbleMesh, anchorDot = buildSphere(0.012f, 8, 12), centreDot = buildSphere(0.007f, 6, 10);
  float bubbleR = -1, planeSize = kGhostS;
  M planeXf = ident();
  bool planeValid = false;
  double renderMs = 0; int renderN = 0;

  OverlayUploader uploader;
  Clock::time_point lastRawStream{};
  int uploadFails = 0;

  std::vector<Button> buttons;
  DashFx fx;
  std::string dashDrawn;
  bool dashForce = true, dashPoke = false;
  Clock::time_point lastDashCheck{};
  std::string status;
  bool statusWarn = false, statusGood = false;
  Clock::time_point statusUntil{}, confirmUntil{};
  std::string panelDrawn;
  Clock::time_point messageUntil{};

  const Step& step() const { return steps[si]; }
  int fit() const { return steps[si].hand; }
  int other() const { return 1 - steps[si].hand; }
  Offset& cur() { return fit() == 0 ? work.left : work.right; }

  enum { Info = 0, Good = 1, Warn = 2 };
  void setStatus(const std::string& s, int kind = Info, float secs = 6) {
    status = s; statusWarn = kind == Warn; statusGood = kind == Good;
    statusUntil = Clock::now() + std::chrono::milliseconds(int(secs * 1000));
    logf("status: %s", s.c_str());
  }

  // Overlay images go to SteamVR as D3D11 textures (see d3dupload.h for why not SetOverlayRaw).
  // Returns false if SteamVR didn't take the image, so the caller redraws and retries.
  bool put(vr::VROverlayHandle_t o, const uint8_t* px, uint32_t w, uint32_t h, bool stream) {
    vr::EVROverlayError e = vr::VROverlayError_RequestFailed, raw = vr::VROverlayError_None;
    if (uploader.ready()) e = uploader.upload(o, px, w, h);
    if (e != vr::VROverlayError_None) {
      // Fallback only. Raw blocks pile up, so streamed images (bubble/ghost) are held to 10 Hz here.
      if (stream && Clock::now() - lastRawStream < std::chrono::milliseconds(100)) return true;
      if (stream) lastRawStream = Clock::now();
      raw = vr::VROverlay()->SetOverlayRaw(o, (void*)px, w, h, 4);
    }
    bool ok = e == vr::VROverlayError_None || raw == vr::VROverlayError_None;
    if (e != vr::VROverlayError_None && uploader.ready() && (++uploadFails <= 5 || uploadFails % 500 == 0))
      logf("overlay upload: d3d11 %s, raw %s (%d so far)", vr::VROverlay()->GetOverlayErrorNameFromEnum(e),
           vr::VROverlay()->GetOverlayErrorNameFromEnum(raw), uploadFails);
    return ok;
  }
  bool upload(vr::VROverlayHandle_t o, Canvas& cv) {
    const auto& px = cv.toRGBA();
    return put(o, px.data(), cv.w, cv.h, false);
  }

  bool init() {
    if (!store.load()) store.save();
    in.init();
    std::string gpu;
    if (uploader.init(&gpu)) logf("overlay images via D3D11: %s", gpu.c_str());
    else logf("D3D11 unavailable (%s): falling back to SetOverlayRaw", gpu.c_str());
    refreshControllers();
    auto O = vr::VROverlay();
    O->CreateDashboardOverlay("utmtftm.dashboard", "UTMTFTM", &dash, &thumb);
    O->SetOverlayWidthInMeters(dash, 2.5f);
    O->SetOverlayInputMethod(dash, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t sc{float(dashCv.w), float(dashCv.h)};
    O->SetOverlayMouseScale(dash, &sc);
    O->CreateOverlay("utmtftm.ghost", "UTMTFTM ghost", &ghost);
    O->SetOverlayWidthInMeters(ghost, kGhostS);
    O->SetOverlayFlag(ghost, vr::VROverlayFlags_SideBySide_Parallel, true);
    O->SetOverlaySortOrder(ghost, 10);
    O->CreateOverlay("utmtftm.hint", "UTMTFTM panel", &hint);
    O->SetOverlayWidthInMeters(hint, 0.36f);
    O->SetOverlaySortOrder(hint, 11);
    drawIcon(thumbCv);
    upload(thumb, thumbCv);
    return true;
  }

  // ---------------- scene app tracking
  void refreshControllerType() {
    auto S = vr::VRSystem();
    for (auto role : {vr::TrackedControllerRole_LeftHand, vr::TrackedControllerRole_RightHand}) {
      auto idx = S->GetTrackedDeviceIndexForControllerRole(role);
      if (idx == vr::k_unTrackedDeviceIndexInvalid) continue;
      char t[64] = {0};
      S->GetStringTrackedDeviceProperty(idx, vr::Prop_ControllerType_String, t, sizeof t);
      if (t[0]) { Store::controllerType = t; return; }
    }
  }

  // Which controller each hand is on. Read once a second, so one that's switched on or swapped is picked up by itself.
  // Where the grip sits comes from SteamVR's own table for that model, which doesn't change with the offsets,
  // unlike anything measured from the poses.
  void refreshControllers() {
    refreshControllerType();
    auto S = vr::VRSystem();
    for (int h = 0; h < 2; ++h) {
      auto idx = S->GetTrackedDeviceIndexForControllerRole(h ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
      if (idx == vr::k_unTrackedDeviceIndexInvalid) continue;
      char model[256] = {0};
      S->GetStringTrackedDeviceProperty(idx, vr::Prop_RenderModelName_String, model, sizeof model);
      if (!model[0] || modelName[h] == model) continue;
      modelName[h] = model;
      modelPending[h] = true;
      modelLoader[h] = ModelLoader(model);
    }
  }

  void loadControllerModels() {
    for (int h = 0; h < 2; ++h) {
      if (!modelPending[h]) continue;
      ModelLoad r = modelLoader[h].step();
      if (r == ModelLoad::Loading) continue;
      modelPending[h] = false;
      M grip = ident();
      const char* gripName = gripOnModel(modelName[h], grip);
      store.gripFromRaw[h] = grip;
      const Mesh& mesh = modelLoader[h].mesh;
      logf("%s hand: %s (%s), grip from %s, %zu triangles", h ? "right" : "left", modelName[h].c_str(), Store::controllerType.c_str(), gripName ? gripName : "nowhere: using the controller's own origin", mesh.idx.size() / 3);
      if (r == ModelLoad::Done) setControllerModel(h == 0, mesh, grip);
      modelLoader[h].mesh = Mesh{};
      dashForce = true;
      if (mode == Mode::Idle) store.publishLive();
    }
  }

  // The name a game's offsets are saved under. Steam games use their app id. Anything else uses its
  // exe name. SteamVR's own apps aren't games.
  static std::string gameKey(const std::string& k, uint32_t pid) {
    auto starts = [&](const char* p) { return k.rfind(p, 0) == 0; };
    if (starts("steam.app.")) return k;
    if (starts("system.generated.") || k.empty()) return pid ? platform::processExeKey(pid) : "";
    if (starts("system.") || starts("openvr.") || starts("vrlink.") || starts("steam.")) return "";
    return pid ? platform::processExeKey(pid) : "";
  }

  // The driver counts every pose it sees, so a moving count means it's loaded and working. It stays "on"
  // once seen, because a sleeping headset stops the count for a while.
  void refreshDriver() {
    uint64_t n = Store::driverPoseUpdates();
    if (driverCounted && n != driverCount) {
      if (!driverOn) logf("driver add-on is running");
      driverOn = true;
    }
    driverCount = n;
    driverCounted = true;
    uint32_t dev[2];
    dev[0] = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    dev[1] = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    for (auto& d : dev) if (d == vr::k_unTrackedDeviceIndexInvalid) d = fhlive::kNoDevice;
    if (driverOn == store.useDriver && dev[0] == store.device[0] && dev[1] == store.device[1]) return;
    store.useDriver = driverOn;
    store.device[0] = dev[0];
    store.device[1] = dev[1];
    if (mode == Mode::Idle) store.publishLive();
  }

  void truePoses() {
    M standingFromRaw = vr::VRSystem()->GetRawZeroPoseToStandingAbsoluteTrackingPose();
    for (int h = 0; h < 2; ++h) {
      Hand& H = in.h[h];
      M seen = H.raw, report;
      truePoseSeen[h] = store.useDriver && Store::driverTruePose(h, &report);
      trueControllerPoses(H, store.gripFromRaw[h], store.useDriver, truePoseSeen[h] ? &report : nullptr, standingFromRaw);
      if (!truePoseSeen[h] || !H.valid) continue;
      // Near zero when still means the offset reaches what apps see, exactly once, and the two spaces line up.
      M d = mul(inv(mul(H.raw, Store::appliedDeviceOffset(h))), seen);
      poseCheckCm[h] = dist(d, ident()) * 100;
      poseCheckDeg[h] = std::acos(std::clamp((d.m[0][0] + d.m[1][1] + d.m[2][2] - 1) * 0.5f, -1.f, 1.f)) / kDeg;
    }
  }

  void logPlacing(const char* what) {
    logf("%s %s hand, offset %s | true poses L %s R %s, reported vs true+offset L %.2f cm %.1f deg, R %.2f cm %.1f deg, bad offsets ignored %d", what, fit() ? "right" : "left",
         cur().zero() ? "none" : friendlyOffset(cur()).c_str(), truePoseSeen[0] ? "yes" : "no", truePoseSeen[1] ? "yes" : "no",
         poseCheckCm[0], poseCheckDeg[0], poseCheckCm[1], poseCheckDeg[1], badOffsets);
  }

  void checkScene() {
    if (Clock::now() - lastSceneCheck < std::chrono::seconds(1)) return;
    lastSceneCheck = Clock::now();
    if (!addOnEnabled()) {  // switched off while running: leave now and take the offsets with us
      logf("the add-on was switched off in SteamVR: closing");
      store.useDriver = false;
      quit = true;
      return;
    }
    uint32_t pid = vr::VRApplications()->GetCurrentSceneProcessId();
    char key[256] = {0};
    if (pid) vr::VRApplications()->GetApplicationKeyByProcessId(pid, key, sizeof key);
    static uint32_t lastPid = ~0u;
    std::string k = gameKey(key, pid);
    scenePid = pid;
    refreshControllers();
    refreshDriver();
    if (pid != lastPid) {
      logf("scene pid %u key '%s' -> '%s'", pid, key, k.c_str());
      lastPid = pid;
      store.scenePid = pid;
      store.sceneKey = k;
      if (mode == Mode::Idle) store.publishLive();  // reach this exact process, however it's named
    }
    if (k == sceneKey) { checkLive(); return; }
    sceneKey = k; sceneName.clear();
    if (k.empty()) return;
    char name[256] = {0};
    vr::EVRApplicationError e;
    vr::VRApplications()->GetApplicationPropertyString(key, vr::VRApplicationProperty_Name_String, name, sizeof name, &e);
    sceneName = name[0] ? name : k.rfind("exe.", 0) == 0 ? k.substr(4) : k;
    bool isNew = !store.apps.count(k);
    AppProfile& p = store.apps[k];
    p.name = sceneName;
    logf("scene app: %s (%s)%s", k.c_str(), sceneName.c_str(), isNew ? " [new]" : "");
    checkLive();
    // Global default reaches binding-route games we haven't touched (takes effect on their next launch).
    if (!driverOn && !p.live && !p.override_global && !store.global.zero() && !p.managed) {
      std::string err;
      if (!store.apply(k, &err).empty()) setStatus("Default offsets set up for " + sceneName + ". They apply next time it launches.");
    }
    store.save();
  }

  bool sceneLive() const { return !sceneKey.empty() && driverOn; }

  // With the driver running, this game gets live offsets: drop any offset left in its binding.
  void checkLive() {
    if (!sceneLive()) return;
    AppProfile& p = store.apps[sceneKey];
    if (p.live && !p.managed) return;
    p.live = true;
    store.restoreBinding(sceneKey);
    store.publishLive();
    store.save();
    logf("%s: live offsets through the driver add-on", sceneKey.c_str());
  }

  // What the running game is using right now, including a hand that's being moved.
  HandPair appliedNow() const {
    if (mode == Mode::Placing && live) return work;
    return sceneKey.empty() ? store.global : store.effective(sceneKey);
  }

  // ---------------- dashboard
  DashModel dashModel() {
    DashModel m;
    m.game = sceneKey.empty() ? "" : sceneName;
    m.live = sceneKey.empty() ? LiveState::NoGame : sceneLive() ? LiveState::Live : LiveState::NextLaunch;
    HandPair a = appliedNow();
    bool custom = !sceneKey.empty() && store.apps.count(sceneKey) && store.apps.at(sceneKey).override_global;
    Badge badge = sceneKey.empty() ? Badge::AllGamesDefault : custom ? Badge::GameCustom : Badge::GlobalDefault;
    m.cards[0].applied = a.left; m.cards[1].applied = a.right;
    for (int h = 0; h < 2; ++h) {
      m.cards[h].badge = badge;
      m.cards[h].ghostMatched = store.ghostMatched[h];
      m.ghostBase[h] = store.ghostBase(h);
    }
    m.saveForGame = saveForGame && !sceneKey.empty();
    m.gameHasCustom = custom;
    m.needsMatch = m.live != LiveState::Live && !store.ghostMatched[0] && !store.ghostMatched[1];
    m.maxCm = (int)std::lround(store.max_pos * 100);
    if (Clock::now() < statusUntil) { m.status = status; m.statusWarn = statusWarn; m.statusGood = statusGood; }
    fx.apply(m);
    m.armed = mode == Mode::Armed;
    m.confirmClose = Clock::now() < confirmUntil;
    return m;
  }

  // With no game running, SteamVR can show this panel without reporting the dashboard as visible, so ask about the panel too.
  bool panelShowing() const { return vr::VROverlay()->IsDashboardVisible() || vr::VROverlay()->IsOverlayVisible(dash); }

  // The first image is drawn straight away, shown or not, so the panel can never open blank. After that only when
  // it's on screen, or the laser is on it.
  void refreshDashboard() {
    bool first = dashDrawn.empty();
    if (!first && !dashPoke && !panelShowing()) { dashForce = true; return; }
    if (!dashForce && !dashPoke && Clock::now() - lastDashCheck < std::chrono::milliseconds(200)) return;
    lastDashCheck = Clock::now();
    dashPoke = false;
    DashModel m = dashModel();
    std::string key = dashKey(m);
    if (!dashForce && key == dashDrawn) return;
    drawDashboard(dashCv, m, buttons);
    if (!upload(dash, dashCv)) { dashForce = true; return; }  // SteamVR refused it: try again next tick
    if (first) logf("panel image ready");
    dashDrawn = key;
    dashForce = false;
  }

  // A greyed button was tapped: say why instead of doing nothing.
  void explainGreyed(const std::string& id) {
    if (id == "reset_left" || id == "reset_right")
      setStatus(std::string(id == "reset_left" ? "Left" : "Right") + " hand has no offset, so there's nothing to reset.");
    else if (id == "scope_game") setStatus("Start a game to save offsets just for it.");
    else setStatus("That isn't available right now.");
  }

  void onClick(double x, double y) {
    fx.release();
    dashPoke = true;
    if (const Button* hb = hitButton(buttons, x, y)) {
      const std::string id = hb->id;  // copy: buttons are rebuilt on the next draw
      logf("click %s%s", id.c_str(), hb->enabled ? "" : " (greyed)");
      if (!hb->enabled) { explainGreyed(id); return; }
      if (id == "close") {
        if (Clock::now() < confirmUntil) { quit = true; logf("closed from the dashboard"); }
        else confirmUntil = Clock::now() + std::chrono::seconds(4);
      }
      else if (id == "scope_game") { saveForGame = true; setStatus("Saving for this game only."); }
      else if (id == "scope_global") { saveForGame = false; setStatus("Saving for all games."); }
      else if (id == "cancel_armed") { mode = Mode::Idle; in.release(); setStatus("Cancelled. Nothing changed."); }
      else if (id == "place_left") beginPlacing({0});
      else if (id == "place_right") beginPlacing({1});
      else if (id == "place_both") beginPlacing({0, 1});
      else if (id == "match") beginMatch();
      else if (id == "reset_left" || id == "reset_right") resetHand(id == "reset_left" ? 0 : 1);
      else if (id.rfind("lim_", 0) == 0) {
        store.max_pos = std::stoi(id.substr(4)) / 100.f; store.save();
        setStatus("Max distance set to " + id.substr(4) + " cm.", Good);
      }
      else if (id == "use_global") {
        store.apps[sceneKey].override_global = false;
        commitOffsets(sceneKey, store.global, false);
      }
      if (id != "close") confirmUntil = {};
      dashForce = true;
    }
  }

  // Reset one hand of what's applied now, saved to the chosen scope.
  void resetHand(int h) {
    HandPair p = appliedNow();
    (h == 0 ? p.left : p.right) = Offset{};
    commitOffsets(saveForGame ? sceneKey : "", p, true);
    if (!statusWarn) setStatus(std::string(h == 0 ? "Left" : "Right") + " hand reset" + (sceneLive() ? ". Live in the game now." : "."), Good);
  }

  // Save offsets for this game (app non-empty) or for all games, then apply them.
  void commitOffsets(const std::string& app, const HandPair& hp, bool asOverride) {
    if (!app.empty()) {
      AppProfile& p = store.apps[app];
      if (asOverride) { p.override_global = true; p.hands = hp; }
      if (p.name.empty()) p.name = sceneName;
      if (p.live || driverOn || (app == sceneKey && sceneLive())) {
        p.live = true;
        store.restoreBinding(app);
        store.publishLive();
        setStatus("Saved for " + p.name + ". Live in the game now.", Good);
      } else {
        std::string err, r = store.apply(app, &err);
        if (err.empty()) setStatus("Saved for " + p.name + ". Applies next time it launches.", Good);
        else setStatus("Couldn't save: " + err, Warn);
      }
    } else {
      store.global = hp;
      // "All games" includes the game you're in: drop its own offsets so it uses the default too.
      if (!sceneKey.empty() && store.apps.count(sceneKey)) store.apps[sceneKey].override_global = false;
      store.publishLive();
      for (auto& [k, p] : store.apps) {
        if (p.override_global) continue;
        if (p.live || driverOn) { store.restoreBinding(k); continue; }
        if (p.managed || !hp.zero()) { std::string e; store.apply(k, &e); }
      }
      setStatus("Saved as the default for all games.", Good);
    }
    store.save();
  }

  // ---------------- placement / matching session
  void beginSession(std::vector<Step> s) {
    steps = std::move(s); si = 0;
    placingFor = (saveForGame && !sceneKey.empty()) ? sceneKey : "";
    live = sceneLive();
    start = work = appliedNow();
    for (int h = 0; h < 2; ++h) { startBase[h] = workBase[h] = store.ghost[h]; startMatched[h] = workMatched[h] = store.ghostMatched[h]; }
    anyPlace = anyMatch = false;
    for (auto& st : steps) (st.match ? anyMatch : anyPlace) = true;
    held = animating = false;
    mode = Mode::Armed;
    dashForce = true;
  }

  void beginPlacing(std::vector<int> hands) {
    std::vector<Step> s;
    bool matchQueued = false;
    bool needGhost = !sceneLive() && !sceneKey.empty();  // live games move the real hand; with no game there's no hand to match
    for (int h : hands) {
      // The ghost must start on the game's own hand: match it first if neither hand has been matched.
      if (needGhost && !store.ghostMatched[h] && !store.ghostMatched[1 - h] && !matchQueued) { s.push_back({h, true}); matchQueued = true; }
      s.push_back({h, false});
    }
    beginSession(s);
  }

  void beginMatch() { beginSession({{0, true}, {1, true}}); }

  Offset baseFor(int h) const { return ghostBaseFor(workBase, workMatched, h); }

  void startStep() {
    held = animating = movedOnce = false;
    planeValid = false;
    if (handMeshFor != fit()) {
      const V3* j = gripHandSkeleton(fit() == 0);
      std::copy(j, j + 31, joints);
      handMesh = gripHandMesh(fit() == 0);
      handMeshFor = fit();
    }
    // a match step starts from the current best guess (mirror of the other hand if that one is matched)
    if (step().match && !workMatched[fit()]) workBase[fit()] = baseFor(fit());
    vr::VROverlay()->ShowOverlay(ghost);
    vr::VROverlay()->ShowOverlay(hint);
    panelDrawn.clear();
  }

  void finishSession(bool save) {
    vr::VROverlay()->HideOverlay(ghost);
    mode = Mode::Idle;
    dashForce = true;
    in.release();
    if (!save) {
      store.publishLive();
      setStatus("Cancelled. Nothing changed.");
      showMessage("Cancelled", "Nothing changed.", false);
      return;
    }
    if (anyMatch) {
      for (int h = 0; h < 2; ++h) { store.ghost[h] = workBase[h]; store.ghostMatched[h] = workMatched[h]; }
      store.save();
    }
    if (anyPlace) {
      commitOffsets(placingFor, work, true);
      std::string where = placingFor.empty() ? "all games" : sceneName;
      std::string sub = live ? "Your hands are live in the game now." : !sceneKey.empty() ? "Applies next time the game launches."
                        : driverOn ? "Every game uses it right away." : "Games use it from their next launch.";
      showMessage("Saved for " + where, sub, true);
    } else {
      setStatus("Ghost matched to your in-game hands.", Good);
      showMessage("Ghost matched", "It now starts exactly on your in-game hands.", true);
    }
  }

  void showMessage(const std::string& title, const std::string& sub, bool good) {
    drawMessage(hintCv, title, sub, good);
    panelDrawn = upload(hint, hintCv) ? "#message" : "";
    messageUntil = Clock::now() + std::chrono::seconds(4);
    vr::VROverlay()->ShowOverlay(hint);
    placePanel(nullptr);
  }

  // Where the game will draw this hand's grip with offset o (before the ghost's resting pose).
  M gripWithOffset(int h, const Offset& o) const { return fh::gripWithOffset(live || driverOn, in.h[h].raw, in.h[h].grip, o); }

  void tickSession(float dt) {
    if (mode == Mode::Armed) {
      if (panelShowing()) return;
      mode = Mode::Placing;
      startStep();
    }
    if (mode != Mode::Placing) return;
    int f = fit(), o = other();
    bool match = step().match;
    Hand& F = in.h[f];
    Hand& Ot = in.h[o];
    if (!F.valid || !Ot.valid) {
      PanelModel p; p.header = "UTMTFTM"; p.text = "Waiting for both controllers"; p.match = match;
      showPanel(p, nullptr);
      return;
    }
    if (in.h[0].cancel || in.h[1].cancel) {
      work = start;
      for (int h = 0; h < 2; ++h) { workBase[h] = startBase[h]; workMatched[h] = startMatched[h]; }
      finishSession(false);
      return;
    }

    Offset& base = workBase[f];
    M G = gripWithOffset(f, cur());
    bool over = false;
    if (held) {
      if (!Ot.grab) {  // let go: it stays; past the distance limit it's pulled back (rotation is never limited)
        held = false; movedOnce = true;
        logPlacing("let go of");
        if (!match && beyond(cur(), store.max_pos, store.max_rot)) {
          animFrom = cur(); animTo = clampOffset(cur(), store.max_pos, store.max_rot); animT = 0; animating = true;
          in.buzz(f, 0.8f, 0.08f);
        } else in.buzz(o, 0.3f);
      } else {
        M W = mul(Ot.grip, taken);  // where the grabbing hand now holds the ghost
        if (match) {
          base = baseFromHeld(G, W);
          workMatched[f] = true;
        } else {
          // live: you hold the in-game hand itself; otherwise the ghost (with its resting pose)
          Offset next = offsetFromHeld(live || driverOn, F.raw, F.grip, W, live ? Offset{} : baseFor(f));
          if (finite(next) && posLen(next) < kLostHand) cur() = next;
          else ++badOffsets;
          over = beyond(cur(), store.max_pos, store.max_rot);
        }
        G = gripWithOffset(f, cur());
      }
    } else if (Ot.grabEdge) {
      // live: grab the in-game hand itself (it's drawn at the offset grip); otherwise the ghost
      M W = live ? G : mul(G, fromOffset(baseFor(f)));
      V3 target = live ? pos(G) : xfP(W, (joints[1] + joints[14]) * 0.5f);
      // The grabbing hand may have an offset of its own: reaching with the hand you see counts too.
      V3 seen = pos(gripWithOffset(o, o == 0 ? work.left : work.right));
      if (len(pos(Ot.grip) - target) < kGrabReach || (live && len(seen - target) < kGrabReach)) {
        if (match && !workMatched[f]) { base = baseFor(f); workMatched[f] = true; }
        held = true; animating = false;
        badOffsets = 0;
        taken = mul(inv(Ot.grip), W);
        in.buzz(o, 0.5f);
        logPlacing("grabbed");
      }
    }

    if (!held && (F.reset || Ot.reset)) {
      animFrom = match ? base : cur(); animTo = Offset{}; animT = 0; animating = true; movedOnce = true;
      in.buzz(f, 0.4f);
    }
    if (animating) {
      animT = std::min(1.f, animT + dt / 0.18f);
      float e = 1 - (1 - animT) * (1 - animT);
      (match ? base : cur()) = lerp(animFrom, animTo, e);
      if (match) workMatched[f] = true;
      if (animT >= 1) animating = false;
      G = gripWithOffset(f, cur());
    }

    if (!held && !animating && (F.done || Ot.done)) {
      in.buzz(f, 0.6f, 0.06f);
      if (match) workMatched[f] = true;
      if (si + 1 < steps.size()) { ++si; startStep(); return; }
      finishSession(true);
      return;
    }

    if (live) store.publishLive(&work);  // the in-game hand follows right now

    // Bubble / ghost colour: off-white resting, yellow while held, red past the limit.
    if (over) vr::VROverlay()->SetOverlayColor(ghost, 1.0f, 0.33f, 0.20f);
    else if (held) vr::VROverlay()->SetOverlayColor(ghost, 1.0f, 0.824f, 0.0f);
    else vr::VROverlay()->SetOverlayColor(ghost, 0.925f, 0.925f, 0.905f);
    if (live) {
      renderBubble(F.grip, G);
      showPanel(panelFor(over), &G);
    } else {
      M W = mul(G, fromOffset(baseFor(f)));
      renderGhost(W, F.grip, !match && !cur().zero());
      showPanel(panelFor(over), &W);
    }
  }

  static std::string resetKey() { return buttonNames(Store::controllerType).reset; }
  static std::string cancelKey() { return buttonNames(Store::controllerType).cancel; }

  PanelModel panelFor(bool over) {
    const char* SIDE[2] = {"LEFT", "RIGHT"};
    const char* side[2] = {"left", "right"};
    int f = fit(), o = other();
    PanelModel p;
    p.match = step().match;
    if (steps.size() > 1) p.progress = std::to_string(si + 1) + " / " + std::to_string(steps.size());
    if (p.match) {
      p.header = std::string("MATCH GHOST  \xC2\xB7  ") + SIDE[f] + " HAND";
      if (held) {
        p.text = std::string("Line it up exactly with your in-game ") + side[f] + " hand, then let go";
        p.status = std::string("Keep your ") + side[f] + " hand still and relaxed";
        p.keys = {{cancelKey(), "Cancel"}};
      } else if (movedOnce) {
        p.chip = "TRIGGER"; p.text = "Save the match";
        p.status = "Grab again to fine-tune";
        p.keys = {{"GRIP", "Fine-tune"}, {resetKey(), "Reset ghost"}, {cancelKey(), "Cancel"}};
      } else {
        p.chip = "GRIP"; p.text = std::string("Grab the ghost with your ") + side[o] + " hand";
        p.status = std::string("Then line it up exactly with your in-game ") + side[f] + " hand";
        p.keys = {{resetKey(), "Reset ghost"}, {cancelKey(), "Cancel"}};
      }
      return p;
    }
    p.header = std::string(SIDE[f]) + " HAND";
    p.warn = over;
    std::string readout = cur().zero() ? "No offset yet" : friendlyOffset(cur());
    if (held) {
      p.text = live ? std::string("Move it where it should be, then let go")
                    : std::string("Move it where your ") + side[f] + " hand should be, then let go";
      p.status = over ? "Past the " + std::to_string((int)std::lround(store.max_pos * 100)) + " cm limit. It'll pull back when you let go" : readout;
      p.keys = {{cancelKey(), "Cancel"}};
    } else if (movedOnce) {
      p.chip = "TRIGGER"; p.text = "Lock it in";
      p.status = readout;
      p.keys = {{"GRIP", "Adjust again"}, {resetKey(), "No offset"}, {cancelKey(), "Cancel"}};
    } else {
      p.chip = "GRIP";
      p.text = live ? std::string("Grab your ") + side[f] + " hand with your " + side[o] + " hand"
                    : std::string("Grab the ghost with your ") + side[o] + " hand";
      p.status = live ? "It can move anywhere inside the bubble" : readout;
      if (live && !cur().zero()) p.status = readout;
      p.keys = {{resetKey(), "No offset"}, {cancelKey(), "Cancel"}};
    }
    return p;
  }

  void showPanel(const PanelModel& p, const M* near) {
    std::string key = panelKey(p);
    if (key != panelDrawn) {
      drawPanel(hintCv, p);
      if (upload(hint, hintCv)) panelDrawn = key;
    }
    placePanel(near);
  }

  bool eyePositions(V3 eyes[2]) {
    vr::TrackedDevicePose_t hmd{};
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.022f, &hmd, 1);
    if (!hmd.bPoseIsValid) return false;
    for (int e = 0; e < 2; ++e)
      eyes[e] = pos(mul(hmd.mDeviceToAbsoluteTracking, vr::VRSystem()->GetEyeToHeadTransform(e == 0 ? vr::Eye_Left : vr::Eye_Right)));
    return true;
  }

  // World-fixed drawing plane facing the headset; it only re-centres when the content drifts or
  // the size changes, so a late texture can never shift what's drawn.
  void updatePlane(V3 centre, V3 head, float size, float drift) {
    V3 toHead = norm(head - centre);
    if (planeValid && size == planeSize && len(centre - plane.c) <= drift && dot(toHead, plane.N) >= std::cos(25.f * kDeg)) return;
    if (size != planeSize) { vr::VROverlay()->SetOverlayWidthInMeters(ghost, size); planeSize = size; }
    V3 N = toHead, up{0, 1, 0};
    if (std::fabs(dot(up, N)) > 0.95f) up = {0, 0, -1};
    V3 X = norm(cross(up, N)), Y = cross(N, X);
    plane = {centre, X, Y, N, size};
    planeValid = true;
    M q = ident();
    q.m[0][0] = X.x; q.m[1][0] = X.y; q.m[2][0] = X.z;
    q.m[0][1] = Y.x; q.m[1][1] = Y.y; q.m[2][1] = Y.z;
    q.m[0][2] = N.x; q.m[1][2] = N.y; q.m[2][2] = N.z;
    q.m[0][3] = centre.x; q.m[1][3] = centre.y; q.m[2][3] = centre.z;
    planeXf = q;
  }

  void present() {
    put(ghost, ghost3d.rgba.data(), 2 * ghost3d.W, ghost3d.H, true);
    vr::VROverlay()->SetOverlayTransformAbsolute(ghost, vr::TrackingUniverseStanding, &planeXf);
  }

  // Live games: a glass bubble around the controller showing how far the hand may go,
  // with a dot where the hand is anchored and a faint dot at the controller.
  void renderBubble(const M& controllerGrip, const M& G) {
    V3 eyes[2];
    if (!eyePositions(eyes)) return;
    float r = store.max_pos;
    if (r != bubbleR) { bubbleMesh = buildSphere(r); bubbleR = r; }
    V3 c = pos(controllerGrip);
    updatePlane(c, (eyes[0] + eyes[1]) * 0.5f, bubblePlaneSize(r), 0.04f);
    auto t0 = Clock::now();
    ghost3d.clear();
    ghost3d.drawMesh(centreDot, translate(c.x, c.y, c.z), plane, eyes, 2, 1.6f);
    V3 a = pos(G);
    ghost3d.drawMesh(anchorDot, translate(a.x, a.y, a.z), plane, eyes, 2, 3.0f);
    if (len(a - c) > 0.01f) ghost3d.drawTether(plane, eyes, 2, c, a, 0.6f);
    ghost3d.drawMesh(bubbleMesh, translate(c.x, c.y, c.z), plane, eyes, 2, 0.85f, true);
    present();
    renderMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (++renderN == 600) { logf("bubble render+upload avg %.2f ms", renderMs / renderN); renderN = 0; renderMs = 0; }
  }

  // Stereo 3D ghost: each eye's exact view of the hand, on a world-fixed plane facing the headset.
  void renderGhost(const M& W, const M& controllerGrip, bool tether) {
    V3 eyes[2];
    if (!eyePositions(eyes)) return;
    V3 mid = xfP(W, (joints[1] + joints[14]) * 0.5f);
    updatePlane(mid, (eyes[0] + eyes[1]) * 0.5f, kGhostS, 0.05f);
    auto t0 = Clock::now();
    ghost3d.clear();
    ghost3d.drawMesh(handMesh, W, plane, eyes, 2);
    if (tether) ghost3d.drawTether(plane, eyes, 2, pos(controllerGrip), xfP(W, joints[1]));
    present();
    renderMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (++renderN == 600) { logf("ghost render+upload avg %.2f ms", renderMs / renderN); renderN = 0; renderMs = 0; }
  }

  // Float the panel above the ghost (or in front of the eyes), turned to face the headset.
  void placePanel(const M* near) {
    vr::TrackedDevicePose_t hmd{};
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &hmd, 1);
    if (!hmd.bPoseIsValid) return;
    const auto& H = hmd.mDeviceToAbsoluteTracking;
    float p[3];
    if (near) { for (int i = 0; i < 3; ++i) p[i] = near->m[i][3]; p[1] += 0.22f; }
    else for (int i = 0; i < 3; ++i) p[i] = H.m[i][3] - H.m[i][2] * 0.6f;  // 60 cm in front of the eyes
    float z[3] = {H.m[0][3] - p[0], 0, H.m[2][3] - p[2]};
    float zl = std::sqrt(z[0] * z[0] + z[2] * z[2]);
    if (zl < 1e-4f) return;
    z[0] /= zl; z[2] /= zl;
    M m = ident();
    m.m[0][0] = z[2]; m.m[2][0] = -z[0];
    m.m[0][2] = z[0]; m.m[2][2] = z[2];
    for (int i = 0; i < 3; ++i) m.m[i][3] = p[i];
    vr::VROverlay()->SetOverlayTransformAbsolute(hint, vr::TrackingUniverseStanding, &m);
  }

  // ---------------- main loop
  bool events() {
    vr::VREvent_t ev;
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
      if (ev.eventType == vr::VREvent_Quit) { vr::VRSystem()->AcknowledgeQuit_Exiting(); return false; }
      if (ev.eventType == vr::VREvent_DashboardActivated) dashForce = true;
    }
    while (vr::VROverlay()->PollNextOverlayEvent(dash, &ev, sizeof ev)) {
      double x = ev.data.mouse.x, y = dashCv.h - ev.data.mouse.y;
      const Button* b = nullptr;
      if (ev.eventType == vr::VREvent_MouseMove || ev.eventType == vr::VREvent_MouseButtonDown || ev.eventType == vr::VREvent_MouseButtonUp) {
        lastDashEvent = Clock::now();
        if (!mouseLogged && !vr::VROverlay()->IsDashboardVisible()) {
          logf("the laser is on the panel while SteamVR says the dashboard is shut");
          mouseLogged = true;
        }
      }
      switch (ev.eventType) {
        case vr::VREvent_MouseMove: b = hitButton(buttons, x, y); fx.move(b ? b->id : ""); dashPoke = true; break;
        case vr::VREvent_MouseButtonDown: b = hitButton(buttons, x, y); fx.press(b ? b->id : ""); dashPoke = true; break;
        case vr::VREvent_MouseButtonUp: onClick(x, y); break;
        case vr::VREvent_FocusLeave: fx.leave(); dashPoke = true; break;
        case vr::VREvent_OverlayShown: dashForce = true; break;
        default: break;
      }
    }
    return !quit && !g_stop;
  }

  void run() {
    auto last = Clock::now();
    while (events()) {
      auto now = Clock::now();
      float dt = std::chrono::duration<float>(now - last).count();
      last = now;
      bool dashUp = panelShowing();
      bool busy = mode != Mode::Idle || dashUp || !panelDrawn.empty() || now - lastDashEvent < std::chrono::seconds(2);
      if (dashUp != store.dashboardUp) {  // the driver moves the controller, and the dashboard's pointers with it
        store.dashboardUp = dashUp;
        if (mode != Mode::Placing) store.publishLive();
      }
      bool wantTrue = mode != Mode::Idle && store.useDriver;
      if (wantTrue != askedTrue) { Store::askTruePoses(wantTrue); askedTrue = wantTrue; }
      if (mode != Mode::Idle) { in.poll(mode == Mode::Placing); truePoses(); }  // controllers only matter while placing
      checkScene();
      loadControllerModels();
      tickSession(dt);
      if (mode == Mode::Idle && !panelDrawn.empty()) {
        if (panelDrawn == "#message" && now < messageUntil) placePanel(nullptr);
        else { vr::VROverlay()->HideOverlay(hint); panelDrawn.clear(); }
      }
      refreshDashboard();
      platform::sleepMs(busy ? 11 : 100);  // in the background (dashboard shut, not placing): 10 wakeups/s
    }
    // closing: leave the game exactly as saved
    vr::VROverlay()->HideOverlay(ghost);
    vr::VROverlay()->HideOverlay(hint);
    Store::askTruePoses(false);
    store.dashboardUp = false;
    store.publishLive();
    store.save();
    vr::VROverlay()->DestroyOverlay(ghost);
    vr::VROverlay()->DestroyOverlay(hint);
    vr::VROverlay()->DestroyOverlay(dash);
  }
};

// ---------------------------------------------------------------- entry
int main(int argc, char** argv) {
  g_root = platform::exeDir();
  bool install = argc > 1 && !strcmp(argv[1], "--install");
  bool uninstall = argc > 1 && !strcmp(argv[1], "--uninstall");
  bool open = argc > 1 && !strcmp(argv[1], "--open");  // launched by hand (Steam's VR "+" menu): show it
  if (argc > 2 && !strcmp(argv[1], "--write-icon")) {   // app icon for the launcher / SteamVR manifest
    Canvas cv(256, 256);
    drawIcon(cv);
    return cv.savePNG(argv[2]) ? 0 : 1;
  }

  // Exit codes for the installer and install.cmd / uninstall.cmd: 0 done, 2 SteamVR unreachable, 3 the driver step failed.
  bool regOk = true;
  if (install || uninstall) {
    regOk = platform::setDriverRegistered(install, g_root + "/driver");
    logf("%s: driver %s", install ? "install" : "uninstall", regOk ? "ok" : "FAILED");
  }

  vr::EVRInitError e;
  vr::VR_Init(&e, (install || uninstall) ? vr::VRApplication_Utility : vr::VRApplication_Overlay);
  if (e) { logf("VR_Init: %s", vr::VR_GetVRInitErrorAsEnglishDescription(e)); return (install || uninstall) ? 2 : 1; }

  std::string manifest = fs::path(g_root + "/utmtftm.vrmanifest").make_preferred().string();
  if (install || uninstall) {
    auto A = vr::VRApplications();
    if (install) {
      auto r = A->AddApplicationManifest(manifest.c_str(), false);
      auto r2 = A->SetApplicationAutoLaunch("utmtftm.overlay", true);
      logf("install: AddApplicationManifest=%d SetAutoLaunch=%d", (int)r, (int)r2);
    } else {
      auto r = A->RemoveApplicationManifest(manifest.c_str());
      logf("uninstall: RemoveApplicationManifest=%d", (int)r);
    }
    vr::VR_Shutdown();
    return regOk ? 0 : 3;
  }

  if (!addOnEnabled()) {
    logf("the add-on is switched off in SteamVR: not starting");
    vr::VR_Shutdown();
    return 0;
  }

  // Already running (it starts with SteamVR): just bring its dashboard tab up, never a second copy.
  vr::VROverlayHandle_t existing = 0;
  if (vr::VROverlay()->FindOverlay("utmtftm.dashboard", &existing) == vr::VROverlayError_None && existing) {
    if (open) vr::VROverlay()->ShowDashboard("utmtftm.dashboard");
    logf("already running: %s", open ? "opened its dashboard tab" : "nothing to do");
    vr::VR_Shutdown();
    return 0;
  }

  if (vr::VRApplications()->IsApplicationInstalled("utmtftm.overlay"))
    vr::VRApplications()->IdentifyApplication(platform::currentPid(), "utmtftm.overlay");

  signal(SIGTERM, onSignal);
  signal(SIGINT, onSignal);
  App app;
  app.init();
  app.store.publishLive();
  logf("UTMTFTM running (root %s)", g_root.c_str());
  if (open) vr::VROverlay()->ShowDashboard("utmtftm.dashboard");
  app.run();
  logf("UTMTFTM closed");
  vr::VR_Shutdown();   // Valve: before destroying textures handed to SteamVR
  app.uploader.shutdown();
  return 0;
}

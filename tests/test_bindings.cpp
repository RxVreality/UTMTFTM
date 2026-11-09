// The bindings page's logic and pictures, with no VR running. First against a small made-up SteamVR (fixture_steamvr.h),
// so it means the same everywhere, then against the real one if it's installed. Nothing here changes any real setting:
// the settings are a map in memory and everything written goes in a throwaway folder.
// Usage: test_bindings <folder for pictures>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "bindings.h"
#include "fixture_steamvr.h"
#include "model_files.h"
#include "platform.h"

using namespace fh;
using namespace fh::bind;
namespace fs = std::filesystem;

static int fails = 0, checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) { ++fails; printf("FAIL %s\n", what.c_str()); }
}

struct FakeSettings {
  std::map<std::string, std::string> values;
  int writes = 0;
  Settings make() {
    Settings s;
    s.get = [this](const std::string& section, const std::string& key) { auto it = values.find(section + "/" + key); return it == values.end() ? std::string() : it->second; };
    s.set = [this](const std::string& section, const std::string& key, const std::string& v) { values[section + "/" + key] = v; ++writes; return true; };
    s.remove = [this](const std::string& section, const std::string& key) { values.erase(section + "/" + key); ++writes; };
    return s;
  }
};

static void giveModels(Page& page, const std::string& steamvr) {
  for (const std::string& name : page.modelsWanted()) {
    Mesh mesh;
    std::vector<ModelPart> parts;
    if (modelFromFiles(steamvr, name, mesh, parts)) page.give(name, mesh, parts);
  }
}

static const BindRow* rowOf(const BindSide& s, const std::string& id) {
  for (int h = 0; h < 2; ++h)
    for (const BindRow& r : s.rows[h])
      if (r.id == id) return &r;
  return nullptr;
}

static std::string actionOf(const BindSide& s, const std::string& id) {
  const BindRow* r = rowOf(s, id);
  return r ? r->action : "(no such row)";
}

static void shot(const BindModel& m, const std::string& path) {
  Canvas cv(1200, 760);
  std::vector<Button> b;
  drawBindings(cv, m, b);
  cv.savePNG(path);
  // Everything's on the panel, and nothing's tappable twice over.
  for (const Button& x : b) check(x.x >= 0 && x.y >= 0 && x.x + x.w <= cv.w && x.y + x.h <= cv.h, path + ": " + x.id + " is on the panel");
}

static void shotCombo(const ComboModel& m, const std::string& path) {
  Canvas cv(1200, 760);
  std::vector<Button> b;
  drawCombos(cv, m, b);
  cv.savePNG(path);
  for (const Button& x : b) check(x.x >= 0 && x.y >= 0 && x.x + x.w <= cv.w && x.y + x.h <= cv.h, path + ": " + x.id + " is on the panel");
  // J4, 7 November: Save landed on Keep. Shapes lean, so neighbours' boxes cross a little, but no more than that.
  for (size_t i = 0; i < b.size(); ++i)
    for (size_t j = i + 1; j < b.size(); ++j) {
      const double across = std::min(b[i].x + b[i].w, b[j].x + b[j].w) - std::max(b[i].x, b[j].x), down = std::min(b[i].y + b[i].h, b[j].y + b[j].h) - std::max(b[i].y, b[j].y);
      check(across <= 24 || down <= 2, path + ": " + b[i].id + " and " + b[j].id + " are on top of each other");
    }
}

static void shotRecorded(const RecordedModel& m, const std::string& path) {
  Canvas cv(1200, 760);
  std::vector<Button> b;
  drawRecorded(cv, m, b);
  cv.savePNG(path);
  for (const Button& x : b) check(x.x >= 0 && x.y >= 0 && x.x + x.w <= cv.w && x.y + x.h <= cv.h, path + ": " + x.id + " is on the panel");
  // J4, 7 November: Save landed on Keep. Shapes lean, so neighbours' boxes cross a little, but no more than that.
  for (size_t i = 0; i < b.size(); ++i)
    for (size_t j = i + 1; j < b.size(); ++j) {
      const double across = std::min(b[i].x + b[i].w, b[j].x + b[j].w) - std::max(b[i].x, b[j].x), down = std::min(b[i].y + b[i].h, b[j].y + b[j].h) - std::max(b[i].y, b[j].y);
      check(across <= 24 || down <= 2, path + ": " + b[i].id + " and " + b[j].id + " are on top of each other");
    }
}

static Json::Value readFile(const std::string& path) {
  Json::Value v;
  std::ifstream in(fs::u8path(path));
  Json::CharReaderBuilder b;
  std::string errs;
  Json::parseFromStream(b, in, &v, &errs);
  return v;
}

static std::string chordText(const Json::Value& chord) {
  std::string s = chord["output"].asString() + " <-";
  for (const auto& in : chord["inputs"]) s += " " + in[0].asString() + ":" + in[1].asString();
  return s;
}

static std::set<std::string> sourcesOf(const Json::Value& binding) {
  std::set<std::string> out;
  for (const auto& set : binding["bindings"].getMemberNames())
    for (const auto& src : binding["bindings"][set]["sources"]) {
      for (const auto& in : src["inputs"].getMemberNames())
        out.insert(src["path"].asString() + " " + src["mode"].asString() + " " + in + " " + src["inputs"][in]["output"].asString());
    }
  return out;
}

static void testFixture(const std::string& work, const std::string& out) {
  const std::string steamvr = work + "/steamvr", config = work + "/config", data = work + "/data";
  fixture::writeSteamVR(steamvr, config, work + "/games");
  Dirs d;
  d.steamvr = steamvr;
  d.steam = work + "/steam";
  d.data = data;
  d.config = {config};
  FakeSettings fake;

  Library lib;
  lib.load(d);
  check(lib.profile("frame_controller") && lib.profile("oculus_touch"), "both controllers are read");
  check(lib.headsets.size() == 3, "only controllers with a model on disk are offered");
  check(!lib.headsets.empty() && lib.headsets[0].model[0] == "{frame_controller}frame_controller_left", "a driver's model is asked for by the name that loads it");
  check(lib.headsets.size() > 1 && lib.headsets[1].model[0] == "oculus_quest_plus_controller_left", "and one of SteamVR's own by its plain name");

  // The wiring is SteamVR's own, read backwards: which of theirs each of yours acts as.
  Wiring w = defaultWiring(lib, "oculus_touch", "frame_controller");
  check(w["left/input/dpad_down"] == "left/input/x", "D-pad down acts as X");
  check(w["left/input/dpad_up"] == "left/input/y" && w["left/input/dpad_left"] == "left/input/y", "the other three act as Y");
  check(w["right/input/a"] == "right/input/a" && w["right/input/x"] == "right/input/b" && w["right/input/y"] == "right/input/b", "A stays A, and X and Y act as B");
  check(w["right/input/bumper"] == "right/input/trigger" && w["left/input/bumper"] == "left/input/trigger", "the bumper mirrors to both hands");
  check(w["left/input/thumbstick"] == "left/input/joystick", "the stick mirrors to the left hand too");
  check(w["left/input/view"] == "left/input/system", "View acts as Touch's menu button, which Touch calls system");

  // The other direction has no table of its own in the fixture, so it runs backwards.
  Wiring back = defaultWiring(lib, "frame_controller", "oculus_touch");
  check(back["left/input/x"] == "left/input/dpad_down", "going the other way, X acts as D-pad down");
  check(back["right/input/joystick"] == "right/input/thumbstick", "and the stick is still the stick");
  Wiring self = defaultWiring(lib, "frame_controller", "frame_controller");
  check(self["right/input/x"] == "right/input/x" && self["left/input/dpad_up"] == "left/input/dpad_up", "against itself, everything is itself");

  // One trigger source doing two things, as Vertigo's has. Both have to survive, on the trigger and on the bumper.
  {
    Json::Value from;
    Json::CharReaderBuilder b;
    std::string errs;
    std::stringstream text("{\"bindings\": {\"/actions/default\": {\"sources\": [{\"path\": \"/user/hand/left/input/trigger\", \"mode\": \"trigger\", "
                           "\"inputs\": {\"click\": {\"output\": \"/actions/default/in/fire\"}, \"pull\": {\"output\": \"/actions/default/in/visual\"}}}]}}}");
    Json::parseFromStream(b, text, &from, &errs);
    const std::set<std::string> got = sourcesOf(translate(from, w, *lib.profile("oculus_touch"), *lib.profile("frame_controller")));
    const std::string in = "/user/hand/left/input/", act = "/actions/default/in/";
    check(got.count(in + "bumper button click " + act + "fire"), "the bumper fires");
    check(got.count(in + "bumper scalar_constant value " + act + "visual"), "and counts as a full pull");
    check(got.count(in + "trigger button click " + act + "fire"), "the trigger fires as a press at a set pull");
    check(got.count(in + "trigger trigger click " + act + "fire") && got.count(in + "trigger trigger pull " + act + "visual"), "and keeps what it had");
    check(got.size() == 5, "and that's all of it");
  }

  // A game that reads its stick as a trackpad keeps that mode, as SteamVR's own translation does. Seen on a Frame, 7 November.
  {
    Json::Value from;
    Json::CharReaderBuilder b;
    std::string errs;
    std::stringstream text("{\"bindings\": {\"/actions/default\": {\"sources\": [{\"path\": \"/user/hand/right/input/joystick\", \"mode\": \"trackpad\", "
                           "\"inputs\": {\"position\": {\"output\": \"/actions/default/in/move\"}}}, {\"path\": \"/user/hand/left/input/joystick\", \"mode\": \"joystick\", "
                           "\"inputs\": {\"position\": {\"output\": \"/actions/default/in/turn\"}}}]}}}");
    Json::parseFromStream(b, text, &from, &errs);
    const std::set<std::string> got = sourcesOf(translate(from, w, *lib.profile("oculus_touch"), *lib.profile("frame_controller")));
    check(got.count("/user/hand/right/input/thumbstick trackpad position /actions/default/in/move"), "a stick read as a trackpad stays a trackpad");
    check(got.count("/user/hand/left/input/thumbstick joystick position /actions/default/in/turn"), "and one read as a joystick stays a joystick");
  }

  // Names for what's bound.
  bool pass = false, hide = false;
  check(actionLabel("/actions/ue/in/oculustouch_right_a_click", {}, &pass, &hide) == "A" && pass && !hide, "Unreal's names only say the button");
  actionLabel("/actions/ue/in/oculustouch_left_thumbstick_up", {}, &pass, &hide);
  check(hide, "a stick's four directions don't get rows of their own");
  actionLabel("/actions/ue/in/framecontroller_right_a_touch", {}, &pass, &hide);
  check(hide, "touching isn't something to list");
  check(actionLabel("/actions/x/in/primarybutton", {}, &pass, &hide) == "Primary" && !pass && !hide, "Unity's role names read as they are");
  check(actionLabel("/actions/x/in/leftbuttona", {}, &pass, &hide) == "X" && pass && !hide, "a plugin's own word for a button isn't taken for the game's");
  check(actionLabel("/actions/x/in/rtrigger", {}, &pass, &hide) == "Trigger" && pass && !hide, "nor is a shortened one");
  actionLabel("/actions/x/in/touch_a", {}, &pass, &hide);
  check(hide, "and a touch sensor isn't a separate thing to show");
  check(actionLabel("/actions/ue/in/framecontroller_left_dpadup_click", {}, &pass, &hide) == "D-pad up", "a D-pad direction reads as words");
  check(actionLabel("/actions/x/in/ToggleCrouch", {}, &pass, &hide) == "Toggle Crouch" && !pass && !hide, "a real action's name is split into words");
  check(actionLabel("/actions/x/in/facebuttontop", {}, &pass, &hide) == "Face Button Top", "and so is one that's all lower case around \"button\"");
  check(actionLabel("/actions/x/in/Fire", {{"/actions/x/in/fire", "Fire the gun"}}, &pass, &hide) == "Fire the gun", "the game's own wording wins");

  Page page(d, fake.make());
  page.load();
  page.setYours("frame_controller", "frame_controller_left", "frame_controller_right");
  check(page.reference().name == "Quest 3", "the first one that isn't yours is what it starts on");
  {
    Page named(d, fake.make());
    named.load();
    named.setYours("frame_controller", "{frame_controller}frame_controller_left", "{frame_controller}frame_controller_right");
    check(named.model("", "").yours.title == "Steam Frame", "SteamVR's name for your model is recognised, braces and all");
  }

  // No game: where everything sits, and how yours translates.
  page.setGame("", "", "");
  giveModels(page, steamvr);
  BindModel m = page.model("", "");
  check(m.theirs.pic[0] && m.theirs.pic[1] && m.yours.pic[0] && m.yours.pic[1], "all four pictures are there");
  check(actionOf(m.yours, "y:right/input/x") == "Quest B", "with no game, your X says it's a Quest B");
  check(actionOf(m.yours, "y:right/input/a").empty(), "and your A is just an A");
  check(rowOf(m.yours, "y:left/input/dpad_left") && rowOf(m.yours, "y:left/input/dpad_left")->x >= 0, "the D-pad's directions have somewhere to point");
  check(!rowOf(m.yours, "y:left/input/system"), "the system button isn't offered, it's SteamVR's");
  check(!rowOf(m.yours, "y:right/input/thumbrest"), "nor is a touch-only thumb rest");
  check(rowOf(m.theirs, "t:left/input/system") && rowOf(m.theirs, "t:left/input/system")->name == "Menu", "Touch's left system button is shown as Menu");
  const BindModel lit = page.model("y:right/input/x", "");
  check(lit.lit.count("y:right/input/x") && lit.lit.count("t:right/input/b"), "pointing at yours lights up its match");
  const BindModel litBack = page.model("t:right/input/b", "");
  check(litBack.lit.count("y:right/input/x") && litBack.lit.count("y:right/input/y") && litBack.lit.count("y:right/input/b"), "and the other way, all of them");
  check(bindKey(lit) != bindKey(m), "the redraw key notices the laser");

  // A game with bindings for the Quest and none for the Frame: SteamVR is translating, and the page says so.
  page.setGame("steam.app.12345", "", "Fixture Game");
  m = page.model("", "");
  check(m.yours.note.find("translating") != std::string::npos, "says SteamVR is translating the game's Quest bindings");
  check(actionOf(m.theirs, "t:left/input/x") == "Primary", "Quest's X reads as the game calls it");
  check(actionOf(m.yours, "y:left/input/dpad_down") == "Primary", "and it's on the same action on your D-pad");
  check(actionOf(m.yours, "y:right/input/y") == "Secondary", "your Y is on what the Quest's B does");
  check(actionOf(m.yours, "y:right/input/bumper") == "Trigger", "and your bumper is on the trigger's");
  check(actionOf(m.yours, "y:right/input/trigger").empty(), "a trigger on the trigger has nothing more to say");
  check(rowOf(m.yours, "y:right/input/menu") && rowOf(m.yours, "y:right/input/menu")->unused, "and a button that's not on it says so");
  check(m.layoutOk && !m.layoutOn, "the layout can be switched on");
  check(m.controls.empty(), "one set of controls, so no arrows");
  {
    const BindModel p = page.model("y:left/input/dpad_down", "");
    check(p.lit.count("t:left/input/x") == 1, "pointing at Primary on your D-pad lights up Primary on the Quest");
    check(p.lit.count("t:right/input/a") == 0, "on the same hand, when there's one there");
  }

  // Switching the layout on: a file of ours, and SteamVR's setting pointing at it.
  page.click("bind_layout");
  const std::string key = "steam.app.12345/frame_controller_250820_CurrentURL_openxr";
  const std::string url = fake.values[key];
  check(url.rfind("file:///", 0) == 0, "the game's setting points at a file");
  check(url.find(" ") == std::string::npos, "with no bare spaces in it");
  Json::Value written;
  {
    std::ifstream in(fs::u8path(page.fileFor("steam.app.12345", "frame_controller")));
    Json::CharReaderBuilder b;
    std::string errs;
    check(in && Json::parseFromStream(b, in, &written, &errs), "and the file is there");
  }
  check(written["controller_type"].asString() == "frame_controller", "it's a binding for your controller");
  check(written["interaction_profile"].asString() == "/interaction_profiles/oculus/touch_controller", "the game goes on being told it's a Quest controller");
  check(written["app_key"].asString() == "steam.app.12345", "it says which game it's for");
  check(written["category"].asString() == "openxr", "and the category stays");
  const std::set<std::string> t = sourcesOf(written);
  const std::string out_ = "/actions/oculustouchcontroller/in/";
  check(t.count("/user/hand/left/input/dpad_down button click " + out_ + "primarybutton"), "D-pad down clicks what X clicked");
  check(t.count("/user/hand/right/input/y button click " + out_ + "secondarybutton"), "Y clicks what B clicked");
  check(t.count("/user/hand/left/input/thumbstick joystick position " + out_ + "thumbstick"), "the stick moves what the stick moved, under its new name");
  check(t.count("/user/hand/left/input/view button click " + out_ + "menu"), "View is the menu button");
  check(t.count("/user/hand/right/input/bumper scalar_constant value " + out_ + "trigger"), "a bumper can't half-press, so it reads as a full pull");
  check(t.count("/user/hand/right/input/bumper button click " + out_ + "triggerpressed"), "and it counts as a click too");
  check(!t.count("/user/hand/left/input/x button click " + out_ + "primarybutton"), "nothing's left on a button your hand doesn't have");
  check(written["bindings"]["/actions/oculustouchcontroller"]["poses"][0]["path"].asString() == "/user/hand/left/pose/openxr_grip", "poses come across");
  m = page.model("", "");
  check(m.layoutOn, "the page shows it's on");
  check(page.status.find("next time") != std::string::npos, "and says when it takes effect");
  shot(m, out + "/fixture_layout.png");

  // Rewiring: tap yours, tap theirs. It's saved and written straight away.
  page.click("y:right/input/x");
  m = page.model("", "");
  check(m.armed == "y:right/input/x" && m.armedName == "X", "tapping yours asks what it should do");
  shot(m, out + "/fixture_armed.png");
  page.click("t:right/input/a");
  check(page.wiringChanges["oculus_touch>frame_controller"]["right/input/x"] == "right/input/a", "tapping theirs saves it");
  {
    std::ifstream in(fs::u8path(page.fileFor("steam.app.12345", "frame_controller")));
    Json::CharReaderBuilder b;
    std::string errs;
    Json::parseFromStream(b, in, &written, &errs);
  }
  check(sourcesOf(written).count("/user/hand/right/input/x button click " + out_ + "primarybutton"), "and the file changes with it");
  check(!sourcesOf(written).count("/user/hand/right/input/x button click " + out_ + "secondarybutton"), "X doesn't do the old thing as well");
  m = page.model("", "");
  check(m.wiringChanged, "the undo button is on offer");
  page.click("bind_reset_wiring");
  check(!page.model("", "").wiringChanged, "and it works");

  // Back to the developer's: the setting goes back to how it was, which here is not set at all.
  page.click("bind_dev");
  check(!fake.values.count(key), "the setting is gone again");
  check(!fs::exists(fs::u8path(page.fileFor("steam.app.12345", "frame_controller"))), "and so is our file");
  check(!page.model("", "").layoutOn, "and the page shows that");

  // A setting that was there before us comes back.
  fake.values[key] = "vr-input-workshop://123";
  page.click("bind_layout");
  check(fake.values[key].rfind("file:///", 0) == 0, "we take over someone's workshop binding");
  page.click("bind_dev");
  check(fake.values[key] == "vr-input-workshop://123", "and hand it back untouched");
  fake.values.erase(key);

  // All games: the default reaches a game the next time it starts, and a game's own choice beats it.
  page.click("bind_scope_global");
  page.click("bind_layout");
  check(page.defaults["frame_controller"] == "oculus_touch", "the default's saved");
  check(fake.values.count(key), "and the running game picks it up");
  page.click("bind_scope_game");
  page.click("bind_dev");
  check(!fake.values.count(key), "this game can opt out");
  page.gameStarted();
  check(!fake.values.count(key), "and it stays opted out when it starts again");
  page.setGame("steam.app.12346", "", "No Bindings");
  page.gameStarted();
  check(!fake.values.count("steam.app.12346/frame_controller_250820_CurrentURL_openxr"), "a game with nothing to translate is left alone");
  m = page.model("", "");
  check(!m.layoutOk, "and its layout button is greyed");
  page.clickGreyed("bind_layout");
  check(page.status.find("No Bindings") != std::string::npos, "and says why when you tap it");

  // A broken binding file is treated as no binding, whatever shape it's broken in.
  for (const char* bad : {"not json at all", "[1, 2]", "{\"bindings\": 5}", "{\"bindings\": {\"/actions/x\": 5}}",
                          "{\"bindings\": {\"/actions/x\": {\"sources\": 5}}}", "{\"bindings\": {\"/actions/x\": {\"sources\": [5]}}}",
                          "{\"bindings\": {\"/actions/x\": {\"sources\": [{\"path\": \"/user/hand/left/input/a\", \"mode\": \"button\", \"inputs\": \"oops\"}]}}}",
                          "{\"bindings\": {\"/actions/x\": {\"sources\": [{\"path\": \"/user/hand/left/input/a\", \"mode\": \"button\", \"inputs\": {\"click\": 5}}]}}}",
                          "{\"bindings\": {\"/actions/x\": {\"poses\": [{\"path\": 5}]}}}",
                          "{\"bindings\": {\"/actions/x\": {\"chords\": [{\"inputs\": [5]}]}}}"}) {
    fixture::writeFile(fs::u8path(config) / "openxr" / "steam.app.13_oculus_touch_0_binding.json", bad);
    page.setGame("steam.app.13", "", "Broken Game");
    m = page.model("", "");
    check(!m.layoutOk, std::string("a broken binding offers no layout: ") + bad);
    check(m.theirs.note.find("hasn't set this one up") != std::string::npos, std::string("and says the game has nothing for it: ") + bad);
    shot(m, out + "/fixture_broken.png");
  }
  fs::remove(fs::u8path(config) / "openxr" / "steam.app.13_oculus_touch_0_binding.json");
  page.setGame("steam.app.12346", "", "No Bindings");

  // Removing the add-on puts every game back on what it had, and our files go.
  {
    FakeSettings other;
    other.values["steam.app.1/frame_controller_250820_CurrentURL_openxr"] = "vr-input-workshop://9";
    Page a(d, other.make());
    a.load();
    a.setYours("frame_controller", "frame_controller_left", "frame_controller_right");
    a.setGame("steam.app.12345", "", "Fixture Game");
    a.click("bind_scope_game");
    a.click("bind_layout");
    check(other.values.count(key) == 1, "a game on a layout, about to be removed");
    const std::string file = a.fileFor("steam.app.12345", "frame_controller");
    check(fs::exists(fs::u8path(file)), "with its file");
    Page(d, other.make()).restoreAll();
    check(!other.values.count(key), "removing it takes the setting off");
    check(other.values.count("steam.app.1/frame_controller_250820_CurrentURL_openxr") == 1, "and doesn't touch anyone else's");
    check(!fs::exists(fs::u8path(file)), "and our file's gone");
    a.load();
    a.click("bind_dev");
  }
  page.load();

  // Reopening: everything's still there.
  {
    Page again(d, fake.make());
    again.load();
    check(again.defaults["frame_controller"] == "oculus_touch", "the default survives a restart");
    check(again.games["steam.app.12345|frame_controller"].explicitly && again.games["steam.app.12345|frame_controller"].from.empty(), "and so does the opt-out");
  }
  page.click("bind_scope_global");
  page.click("bind_dev");
  check(page.defaults.empty(), "the default can be cleared");

  // A SteamVR Input game with real names, found through Steam's list.
  page.setGame("steam.app.777", "", "Named Game");
  m = page.model("", "");
  check(actionOf(m.theirs, "t:right/input/trigger") == "Fire the gun", "uses the game's own words for the Quest's trigger");
  check(actionOf(m.yours, "y:right/input/bumper") == "Fire the gun", "and for the developer's choice on your bumper");
  check(actionOf(m.yours, "y:left/input/thumbstick") == "Move", "the stick says what it does");
  check(rowOf(m.yours, "y:right/input/a") && rowOf(m.yours, "y:right/input/a")->unused, "a button it doesn't use says so");
  check(m.yours.note == "As the developer set it", "it's the developer's own, so no translating");
  {
    // Reload is on the Quest's A and your Y here, where the wiring would say the Quest's B.
    BindModel p = page.model("y:right/input/y", "");
    check(p.lit.count("t:right/input/a") == 1, "pointing at Reload on yours lights up Reload on the Quest");
    check(p.lit.count("t:right/input/b") == 0, "and not the button the wiring would pick");
    p = page.model("t:right/input/a", "");
    check(p.lit.count("y:right/input/y") == 1, "and the same the other way");
    check(p.lit.count("y:right/input/a") == 0, "and not the one with the same letter");
    p = page.model("y:left/input/thumbstick", "");
    check(p.lit.count("t:left/input/joystick") == 1, "the stick still finds the stick, by what it does");
    p = page.model("y:right/input/x", "");
    check(p.lit.count("t:right/input/b") == 1, "a button with nothing on it falls back to the wiring");
    page.click("bind_layout");
    p = page.model("y:right/input/y", "");
    check(p.lit.count("t:right/input/b") == 1 && p.lit.count("t:right/input/a") == 0, "on a layout, the wiring is what's happening, so that's the match");
    page.click("bind_dev");
    page.status.clear();
  }
  check(m.controls == "On Foot", "it starts on the biggest set of controls, by the game's name for it");
  check(actionOf(m.theirs, "t:right/input/a") == "Reload", "and shows only that set's");
  page.click("bind_controls_next");
  m = page.model("", "");
  check(m.controls == "Driving", "the arrows move to the next set");
  check(actionOf(m.theirs, "t:right/input/a") == "Horn", "and the rows follow, on the Quest");
  check(actionOf(m.yours, "y:right/input/a") == "Horn", "and on yours");
  check(actionOf(m.theirs, "t:right/input/b") == "Horn", "a right-handed version of a set counts as part of it");
  check(rowOf(m.yours, "y:right/input/bumper")->unused, "and what's not in this set says so");
  page.click("bind_controls_prev");
  check(page.model("", "").controls == "On Foot", "and back");
  shot(m, out + "/fixture_named.png");
  page.click("bind_scope_game");
  page.click("bind_layout");
  check(fake.values.count("steam.app.777/frame_controller_250820_CurrentURL_steamvrinput"), "SteamVR Input's setting has its own spelling");
  {
    std::ifstream in(fs::u8path(page.fileFor("steam.app.777", "frame_controller")));
    Json::CharReaderBuilder b;
    std::string errs;
    Json::parseFromStream(b, in, &written, &errs);
  }
  check(written["options"]["simulated_controller_type"].asString() == "oculus_touch", "and it can be told it's holding the other controller");
  check(written["options"]["simulate_hmd"].asBool() && written["options"]["simulate_rendermodel"].asString() == "name_only", "the way SteamVR's own translation tells it");
  page.click("bind_dev");

  // Your own controller as the reference: nothing to translate until you've rewired something.
  page.setGame("steam.app.777", "", "Named Game");
  page.setReference("Steam Frame");
  m = page.model("", "");
  check(!m.layoutOk && m.layoutName == "Rewired", "against itself, there's nothing to switch on yet");
  page.click("y:right/input/a");
  page.click("t:right/input/y");
  m = page.model("", "");
  check(m.layoutOn && m.wiringChanged, "rewiring it switches it on");
  page.click("bind_reset_wiring");
  page.click("bind_dev");

  // ---- Combos. First a game with its own Frame binding, so everything else in the file has to stay exactly as it was.
  page.setReference("Quest 3");
  page.setGame("steam.app.777", "", "Named Game");
  page.status.clear();
  giveModels(page, steamvr);
  const std::string namedKey = "steam.app.777/frame_controller_250820_CurrentURL_steamvrinput";
  const std::string namedFile = page.fileFor("steam.app.777", "frame_controller");
  ComboModel cm = page.comboModel("", "");
  check(cm.canMake && cm.step == 0 && cm.saved.empty(), "a game with bindings can have combos, and starts with none");
  shotCombo(cm, out + "/fixture_combos_empty.png");
  check(!fake.values.count(namedKey), "and just looking changes nothing");
  auto markOf = [](const ComboModel& m, const std::string& id) {
    for (int h = 0; h < 2; ++h)
      for (const BindRow& r : m.yours.rows[h])
        if (r.id == id) return r.mark;
    return -1;
  };
  page.comboClick("c:right/input/a");
  cm = page.comboModel("", "");
  check(cm.step == 1 && cm.picked == "Touch A", "tapping a control on the list starts a combo with it");
  page.comboClick("combo_cancel");
  check(page.comboModel("", "").step == 0 && page.comboModel("", "").picked.empty(), "and Cancel forgets it");
  check(!fake.values.count(namedKey), "without having written anything");
  page.comboClick("combo_new");
  cm = page.comboModel("", "");
  check(cm.step == 1 && cm.picked.empty(), "New combo starts empty");
  check(!cm.ideas.empty(), "and has some ideas on offer");
  page.comboClick("c:right/input/a");
  cm = page.comboModel("", "");
  check(markOf(cm, "c:right/input/a") == 1 && cm.picked == "Touch A", "one tap is touch");
  check(cm.note.find("brushes") != std::string::npos, "and it warns that one touch alone goes off easily");
  page.comboClick("c:right/input/a");
  cm = page.comboModel("", "");
  check(markOf(cm, "c:right/input/a") == 2 && cm.picked == "Press A", "two taps is press");
  page.comboClick("c:right/input/a");
  check(page.comboModel("", "").picked.empty(), "three taps is off again");
  page.comboClick("c:right/input/a");
  page.comboClick("c:right/input/b");
  cm = page.comboModel("", "");
  check(cm.picked == "Touch A + Touch B", "two controls read as a combo");
  check(cm.note.empty(), "and touches have nothing to warn about");
  shotCombo(cm, out + "/fixture_combos_step1.png");
  page.comboClick("c:right/input/y");
  page.comboClick("c:right/input/y");
  check(page.comboModel("", "").note.find("Reload") != std::string::npos, "pressing something that's already in use says what else it'll do");
  page.comboClick("c:right/input/y");
  page.comboClick("c:right/input/grip");
  check(page.comboModel("", "").note.find("holding the controller") != std::string::npos, "touching the grip says that's probably not what you want");
  page.comboClick("c:right/input/grip");
  page.comboClick("c:right/input/grip");
  check(page.comboModel("", "").picked == "Touch A + Touch B", "and clearing them leaves the other two");

  page.comboClick("combo_next");
  cm = page.comboModel("", "");
  check(cm.step == 2 && cm.controls == "On Foot", "the next step lists what it can do, starting on the main controls");
  auto choiceNamed = [](const ComboModel& m, const std::string& name) -> std::string {
    for (const ComboChoice& e : m.choices) if (e.name == name) return e.id;
    return "";
  };
  check(!choiceNamed(cm, "Fire the gun").empty() && !choiceNamed(cm, "Reload").empty(), "in the game's own words");
  check(choiceNamed(cm, "Move").empty(), "a stick can't be set off by a combo, so it's not on the list");
  for (const ComboChoice& e : cm.choices)
    if (e.name == "Reload") check(e.where == "On Y", "and it says where it is now");
  shotCombo(cm, out + "/fixture_combos_step2.png");
  page.comboClick("combo_group_next");
  check(!choiceNamed(page.comboModel("", ""), "Horn").empty(), "the arrows go through the other sets of controls");
  page.comboClick("combo_group_prev");
  page.comboClick("combo_back");
  check(page.comboModel("", "").picked == "Touch A + Touch B", "Back keeps what you'd picked");
  page.comboClick("combo_next");
  page.comboClick(choiceNamed(page.comboModel("", ""), "Reload"));
  cm = page.comboModel("", "");
  check(cm.step == 0 && cm.saved.size() == 1 && cm.saved[0].inputs == "Touch A + Touch B" && cm.saved[0].action == "Reload", "picking one saves it and goes back to the list");
  check(page.status.find("next time") != std::string::npos, "and says when it takes effect");
  check(cm.yours.rows[0].size() + cm.yours.rows[1].size() > 0, "the controllers are still on the page");
  check(markOf(page.comboModel("combo_row_0", ""), "c:right/input/a") == 1, "pointing at a saved combo lights up its controls");
  check(markOf(page.comboModel("", ""), "c:right/input/a") == 0, "and only then");
  shotCombo(page.comboModel("combo_row_0", ""), out + "/fixture_combos_saved.png");

  check(fake.values[namedKey].rfind("file:///", 0) == 0, "the game's setting points at a file now");
  Json::Value withCombo = readFile(namedFile);
  check(withCombo["bindings"]["/actions/main"]["chords"].size() == 1, "with one chord in it, in the action's own set");
  check(chordText(withCombo["bindings"]["/actions/main"]["chords"][0]) == "/actions/main/in/Reload <- /user/hand/right/input/a:touch /user/hand/right/input/b:touch", "the same way SteamVR's own files write one");
  // What's already on the controller stays. All that's new are the two empty sources a chord's controls sit on.
  {
    Json::Value original;
    Json::CharReaderBuilder b;
    std::string errs;
    std::stringstream text(fixture::kNamedFrame);
    Json::parseFromStream(b, text, &original, &errs);
    check(sourcesOf(withCombo) == sourcesOf(original), "everything the developer set is still there and nothing's been added to a button");
    check(withCombo["bindings"]["/actions/main"]["sources"].size() == original["bindings"]["/actions/main"]["sources"].size() + 2, "it adds a place for each control to sit");
    check(withCombo["controller_type"].asString() == "frame_controller" && withCombo["category"].asString() == "steamvr_input", "it's still the same kind of file");
    check(!withCombo.isMember("options") || !withCombo["options"].isMember("simulated_controller_type"), "and it isn't pretending to be another controller");
  }
  check(!page.model("", "").layoutOn, "it isn't a layout: the Bindings tab still says the developer's");
  check(page.model("", "").yours.note.find("plus 1 combo") != std::string::npos, "but it does say there's a combo");

  // It's still there after a restart, and when the game next starts.
  {
    FakeSettings other = fake;
    Page again(d, other.make());
    again.load();
    again.setYours("frame_controller", "frame_controller_left", "frame_controller_right");
    again.setGame("steam.app.777", "", "Named Game");
    check(again.comboModel("", "").saved.size() == 1, "a saved combo survives a restart");
    again.gameStarted();
    check(readFile(namedFile)["bindings"]["/actions/main"]["chords"].size() == 1, "and isn't doubled when the game starts again");
  }

  // Doing the same controls again replaces it.
  page.comboClick("combo_new");
  page.comboClick("combo_idea_0");
  check(page.comboModel("", "").picked == "Touch A + Touch B", "an idea fills in its controls");
  page.comboClick("combo_next");
  page.comboClick(choiceNamed(page.comboModel("", ""), "Fire the gun"));
  cm = page.comboModel("", "");
  check(cm.saved.size() == 1 && cm.saved[0].action == "Fire the gun", "the same controls again replace the old one");

  // A second one, and just one control.
  page.comboClick("combo_new");
  page.comboClick("c:left/input/thumbstick");
  page.comboClick("combo_next");
  page.comboClick(choiceNamed(page.comboModel("", ""), "Reload"));
  withCombo = readFile(namedFile);
  check(withCombo["bindings"]["/actions/main"]["chords"].size() == 1, "one control isn't a chord");
  check(sourcesOf(withCombo).count("/user/hand/left/input/thumbstick joystick touch /actions/main/in/Reload"), "it's an ordinary binding of its touch, in the mode a stick needs");

  // Switching to the Developer's on the other tab leaves your combos alone.
  page.click("bind_dev");
  check(fake.values[namedKey].rfind("file:///", 0) == 0 && readFile(namedFile)["bindings"]["/actions/main"]["chords"].size() == 1, "Developer's keeps your combos");

  // On a layout, they go in on top of it.
  page.click("bind_layout");
  withCombo = readFile(namedFile);
  check(withCombo["bindings"]["/actions/main"]["chords"].size() == 1, "a layout keeps them too");
  check(withCombo["options"]["simulated_controller_type"].asString() == "oculus_touch", "and it is the layout");
  page.click("bind_dev");

  // Removing them all hands the game back exactly as it was.
  page.comboClick("combo_del_0");
  check(page.comboModel("", "").saved.size() == 1, "Remove takes one off");
  page.comboClick("combo_del_0");
  check(page.comboModel("", "").saved.empty(), "and the other");
  check(!fake.values.count(namedKey), "with none left the setting comes off");
  check(!fs::exists(fs::u8path(namedFile)), "and so does our file");

  // Six is what fits on the page.
  for (int i = 0; i < 6; ++i) {
    page.comboClick("combo_new");
    page.comboClick("c:right/input/a");
    if (i & 1) page.comboClick("c:right/input/b");
    if (i & 2) page.comboClick("c:right/input/x");
    if (i & 4) page.comboClick("c:right/input/y");
    page.comboClick("combo_next");
    page.comboClick(choiceNamed(page.comboModel("", ""), "Reload"));
  }
  cm = page.comboModel("", "");
  check(cm.saved.size() == 6, "six combos fit");
  shotCombo(cm, out + "/fixture_combos_six.png");
  page.comboClick("combo_new");
  check(page.comboModel("", "").step == 0 && page.status.find("Six") != std::string::npos, "and a seventh says why not");
  for (int i = 0; i < 6; ++i) page.comboClick("combo_del_0");
  check(!fake.values.count(namedKey), "and they all come off again");

  // A game SteamVR's translating for you: it goes on top of the same translation, and says it's the other controller's profile.
  page.setGame("steam.app.12345", "", "Fixture Game");
  cm = page.comboModel("", "");
  check(cm.canMake, "a game with only Quest bindings can have combos too");
  page.comboClick("combo_new");
  page.comboClick("c:left/input/dpad_up");
  page.comboClick("c:left/input/dpad_down");
  page.comboClick("combo_next");
  cm = page.comboModel("", "");
  check(!choiceNamed(cm, "Primary").empty(), "its actions are on the list");
  check(choiceNamed(cm, "Primarytouched").empty() && choiceNamed(cm, "Primary Touched").empty(), "touch-reporting actions aren't");
  check(choiceNamed(cm, "Stick").empty(), "and nor is the stick");
  check(cm.note.empty(), "it has role names, so it doesn't say it only names its buttons");
  page.comboClick(choiceNamed(cm, "Primary"));
  {
    const Json::Value w = readFile(page.fileFor("steam.app.12345", "frame_controller"));
    check(w["controller_type"].asString() == "frame_controller" && w["interaction_profile"].asString() == "/interaction_profiles/oculus/touch_controller", "the file's the translated one");
    check(w["bindings"]["/actions/oculustouchcontroller"]["chords"].size() == 1, "and the chord is in it");
    check(sourcesOf(w).count("/user/hand/left/input/dpad_down button click /actions/oculustouchcontroller/in/primarybutton"), "with the translation still underneath");
    check(fake.values.count(key), "and SteamVR's setting points there");
  }
  // Removing the add-on takes all of this back out, and forgetting a saved combo isn't part of that.
  {
    FakeSettings other = fake;
    Page(d, other.make()).restoreAll();
    check(!other.values.count(key), "removing the add-on puts the game back");
    check(!fs::exists(fs::u8path(page.fileFor("steam.app.12345", "frame_controller"))), "and takes our file with it");
    page.load();
    page.setGame("steam.app.12345", "", "Fixture Game");
    check(page.comboModel("", "").saved.size() == 1, "the combo itself is still in the saved list");
    page.comboClick("combo_del_0");
  }
  fake.values.erase(key);

  // Nothing to add to, or nothing running: it says so rather than offering something that can't work.
  page.setGame("steam.app.12346", "", "No Bindings");
  cm = page.comboModel("", "");
  check(!cm.canMake && cm.why.find("Play it once") != std::string::npos, "a game it's never seen says to play it once");
  shotCombo(cm, out + "/fixture_combos_unseen.png");
  page.comboClickGreyed("combo_new");
  check(page.status.find("No Bindings") != std::string::npos, "and the greyed button says why");
  page.status.clear();
  page.comboClick("c:right/input/a");
  check(page.comboModel("", "").step == 0 && page.status.find("No Bindings") != std::string::npos, "tapping a control there explains too, and doesn't start anything");
  page.setGame("", "", "");
  cm = page.comboModel("", "");
  check(!cm.canMake && cm.why.find("particular game") != std::string::npos, "no game running says to start one");
  shotCombo(cm, out + "/fixture_combos_nogame.png");
  page.status.clear();
  page.setGame("steam.app.777", "", "Named Game");

  // A touch-only sensor, like a Quest's thumb rest, is on this page and not the Bindings one.
  {
    Library l2;
    Json::Value touch;
    Json::CharReaderBuilder b;
    std::string errs;
    std::stringstream text(R"({"controller_type": "x", "input_source": {"/input/thumbrest": {"type": "button", "click": false, "touch": true}, "/input/a": {"type": "button", "click": true, "touch": true}, "/input/hidden": {"type": "button", "click": false, "touch": true, "visibility": "InputValueVisibility_SteamVRInternal"}}})");
    Json::parseFromStream(b, text, &touch, &errs);
    fixture::writeFile(fs::u8path(work) / "probe/drivers/x/resources/input/x_profile.json", Json::writeString(Json::StreamWriterBuilder(), touch));
    Dirs pd = d;
    pd.steamvr = work + "/probe";
    l2.load(pd);
    const Profile* p = l2.profile("x");
    check(p && p->find("left", "/input/thumbrest") && p->find("left", "/input/thumbrest")->touchOnly, "a thumb rest counts as a touch sensor");
    check(p && !p->find("left", "/input/thumbrest")->shown, "but isn't a row on the Bindings tab");
    check(p && !p->find("left", "/input/hidden")->touchOnly, "and one SteamVR keeps for itself doesn't count");
    check(p && comboText({{"left/input/thumbrest", "touch"}, {"left/input/a", "click"}}, *p) == "Touch left Thumb rest + Press left A", "both hands have it, so the text says which");
  }

  // Unknown things don't take it down.
  page.setYours("some_future_controller", "", "");
  check(!page.comboModel("", "").canMake, "a controller SteamVR can't describe can't have combos, and doesn't crash");
  m = page.model("", "");
  check(m.yours.note.find("no description") != std::string::npos, "a controller SteamVR can't describe just says so");
  shot(m, out + "/fixture_unknown.png");
  page.setYours("", "", "");
  m = page.model("", "");
  check(m.yours.note.find("Turn your controllers on") != std::string::npos, "no controllers yet says to turn them on");
  shot(m, out + "/fixture_off.png");
}

// ---------------------------------------------------------------- what a guide says a game's buttons do
static std::string openxrBinding(const std::string& type, const std::vector<std::pair<std::string, std::string>>& buttons) {
  std::string sources;
  for (const auto& [path, action] : buttons) {
    if (!sources.empty()) sources += ",";
    sources += R"({"inputs": {"click": {"output": "/actions/game/in/)" + action + R"("}}, "mode": "button", "path": "/user/hand/)" + path + R"("})";
  }
  return R"({"category": "openxr", "controller_type": ")" + type + R"(", "interaction_profile": "/interaction_profiles/x", "name": "x", "bindings": {"/actions/game": {"sources": [)" + sources + "]}}}";
}

static const BindRow* guideRow(const BindSide& side, const std::string& id) {
  for (int hand = 0; hand < 2; ++hand)
    for (const BindRow& r : side.rows[hand])
      if (r.id == id) return &r;
  return nullptr;
}
static std::string guideText(const BindSide& side, const std::string& id) {
  const BindRow* r = guideRow(side, id);
  return r ? r->action : "(no such row)";
}

static macro::Raw rawOf(uint32_t ms, int hand, const char* name, float v) {
  macro::Raw r;
  r.ms = ms;
  r.hand = hand;
  r.name = name;
  r.value = v;
  return r;
}

static void testRecorded(const std::string& work, const std::string& out) {
  const std::string steamvr = work + "/rec-steamvr", config = work + "/rec-config", data = work + "/rec-data";
  fixture::writeSteamVR(steamvr, config, work + "/rec-games");
  Dirs d;
  d.steamvr = steamvr;
  d.steam = work + "/rec-steam";
  d.data = data;
  d.config = {config};
  FakeSettings fake;
  Page page(d, fake.make());
  page.load();
  page.setYours("frame_controller", "{frame_controller}frame_controller_left", "{frame_controller}frame_controller_right");
  page.setGame("steam.app.777", "", "Named Game");
  giveModels(page, steamvr);

  // Without the driver that can do it, the page says so.
  RecordedModel m = page.recordedModel("", "");
  check(!m.canRecord && m.why.find("Restart SteamVR") != std::string::npos, "an old driver still loaded means restart SteamVR: " + m.why);
  shotRecorded(m, out + "/recorded_needs_restart.png");
  page.setHookState(true, false);
  m = page.recordedModel("", "");
  check(!m.canRecord && m.why.find("hasn't heard") != std::string::npos, "and a controller it can't hear says that: " + m.why);
  page.setHookState(true, true);
  m = page.recordedModel("", "");
  check(m.canRecord && m.step == 0 && m.saved.empty(), "otherwise it's ready, with nothing saved");
  shotRecorded(m, out + "/recorded_empty.png");
  fhlive::InputTable t;
  page.fillInputTable(t);
  check(t.macros == 0 && t.routes == 0 && t.chords == 0, "nothing goes to the driver until there's something to do");

  // Record the example.
  page.recordedClick("macro_record");
  page.recordedTick(1000, 0);
  m = page.recordedModel("", "");
  check(m.step == 1 && m.countdown == 3, "first it counts down");
  check(!page.recordedListening(), "and nothing's held back from SteamVR yet");
  shotRecorded(m, out + "/recorded_countdown.png");
  page.recordedTick(2500, 0);
  check(page.recordedModel("", "").countdown == 2, "a second at a time");
  page.recordedTick(4000, 0);
  m = page.recordedModel("", "");
  check(m.step == 2 && page.recordedListening(), "then it's recording");
  check(m.tiles.empty(), "with nothing to show yet");
  page.recordedReport(rawOf(5000, 1, "/input/a/touch", 1));
  page.recordedReport(rawOf(5050, 1, "/input/a/click", 1));
  page.recordedReport(rawOf(5300, 1, "/input/trigger/value", 0.9f));
  page.recordedReport(rawOf(5310, 1, "/input/trigger/click", 1));
  m = page.recordedModel("", "");
  check(m.tiles.size() == 2 && m.tiles[0].name == "A" && m.tiles[1].name == "Trigger", "the steps show up as you go");
  shotRecorded(m, out + "/recorded_recording.png");
  page.recordedReport(rawOf(5490, 1, "/input/trigger/value", 0.1f));
  page.recordedReport(rawOf(5500, 1, "/input/trigger/click", 0));
  page.recordedReport(rawOf(5700, 1, "/input/a/click", 0));
  page.recordedReport(rawOf(5750, 1, "/input/a/touch", 0));
  page.recordedTick(6000, 6500);
  check(page.recordedModel("", "").step == 2, "it waits for you to be sure you've finished");
  page.recordedTick(8000, 7800);
  m = page.recordedModel("", "");
  check(m.step == 3 && !page.recordedListening(), "and stops by itself");
  check(m.tiles.size() == 3 && m.tiles[0].kind == macro::Press && !m.tiles[0].both && m.tiles[1].both && m.tiles[2].kind == macro::Release, "A's held round the trigger, so A splits and the trigger's one tile");
  check(m.name == "A + Trigger", "with a name to start with: " + m.name);
  check(m.tiles[1].id == "macro_tile_1", "and they can be tapped");
  check(m.plays.empty(), "no button yet");
  shotRecorded(m, out + "/recorded_check.png");

  // Can't save without a button.
  page.recordedClick("macro_save");
  check(page.recordedModel("", "").step == 3 && page.status.find("button") != std::string::npos, "saving without a button says what's missing");
  page.status.clear();

  // Moving and deleting.
  page.recordedClick("macro_tile_1");
  m = page.recordedModel("", "");
  check(m.selected == 1, "tapping a step picks it");
  shotRecorded(m, out + "/recorded_selected.png");
  page.recordedClick("macro_left");
  m = page.recordedModel("", "");
  check(m.tiles[0].name == "Trigger" && m.selected == 0, "Earlier moves it, and it stays picked");
  page.recordedClick("macro_right");
  m = page.recordedModel("", "");
  check(m.tiles[0].name == "A" && m.selected == 1 && m.tiles.size() == 2 && m.tiles[0].both && m.tiles[1].both, "and Later moves it past A, which has joined up into one tile");

  // Choosing the button: not done until it's let go of, so the dashboard never gets half a press.
  page.recordedClick("macro_pick");
  page.recordedTick(9000, 9000);
  m = page.recordedModel("", "");
  check(m.step == 4 && page.recordedListening(), "then it listens for the button");
  shotRecorded(m, out + "/recorded_choose.png");
  page.recordedReport(rawOf(9100, 1, "/input/b/touch", 1));
  check(page.recordedModel("", "").step == 4, "a touch might be on its way to a press");
  page.recordedReport(rawOf(9200, 1, "/input/b/click", 1));
  check(page.recordedListening(), "pressed, and still listening until it's let go of");
  page.recordedReport(rawOf(9400, 1, "/input/b/click", 0));
  m = page.recordedModel("", "");
  check(m.step == 3 && !page.recordedListening() && m.plays == "B", "let go, and B plays it: " + m.plays);
  check(m.note.find("stops") != std::string::npos, "and it says B loses its usual job: " + m.note);
  check(m.noteWarn, "in red, because that's a warning");
  check(m.touches == 0, "with Capacitive touches off there's no row for them");
  shotRecorded(m, out + "/recorded_ready.png");

  page.recordedClick("macro_save");
  m = page.recordedModel("", "");
  check(m.step == 0 && m.saved.size() == 1 && m.saved[0].name == "A + Trigger" && m.saved[0].plays == "Press B to play", "saved: " + (m.saved.empty() ? "" : m.saved[0].plays));
  check(m.saved[0].tiles.size() == 2, "and its presses are on the list, one tile each");
  shotRecorded(m, out + "/recorded_saved.png");

  // What the driver gets.
  page.fillInputTable(t);
  check(t.macros == 1 && t.macro[0].hand == 1 && std::string(t.macro[0].path) == "/input/b" && t.macro[0].count == 4 && std::string(t.macro[0].steps[2].path) == "/input/trigger", "the driver gets it, every step of it");
  check(t.macro[0].steps[0].delayMs == 0 && t.macro[0].steps[2].delayMs == 5300 - 5050, "the way you did it, which is the normal setting now");

  // It belongs to this game.
  page.setGame("steam.app.888", "", "Other Game");
  page.fillInputTable(t);
  check(t.macros == 0, "another game doesn't get it");
  page.setGame("", "", "");
  page.fillInputTable(t);
  check(t.macros == 0 && !page.recordedModel("", "").canRecord, "nor does no game");
  page.setGame("steam.app.777", "", "Named Game");

  // It comes back after a restart.
  {
    Page again(d, fake.make());
    again.load();
    again.setYours("frame_controller", "{frame_controller}frame_controller_left", "{frame_controller}frame_controller_right");
    again.setGame("steam.app.777", "", "Named Game");
    fhlive::InputTable t2;
    again.fillInputTable(t2);
    check(t2.macros == 1 && t2.macro[0].count == 4 && !memcmp(&t, &t, sizeof t), "saved combos are there next time");
  }

  // Editing it, repeat and timing.
  page.setHookState(true, true);
  page.recordedClick("macro_edit_0");
  m = page.recordedModel("", "");
  check(m.step == 3 && m.plays == "B" && m.tiles.size() == 2, "Edit brings it back");
  page.recordedClick("macro_repeat");
  page.recordedClick("macro_quick");
  page.recordedSetName("Prone and fire");
  page.recordedClick("macro_save");
  m = page.recordedModel("", "");
  check(m.saved.size() == 1 && m.saved[0].name == "Prone and fire" && m.saved[0].plays == "Press B to play, repeats while held", "edited in place: " + m.saved[0].plays);
  page.fillInputTable(t);
  check(t.macro[0].repeat == 1 && t.macro[0].steps[1].delayMs == macro::kQuickMs && t.macro[0].steps[2].delayMs == macro::kQuickMs, "and Quick is the same short gap each time");

  // A button can only play one thing, and something recorded on top says so.
  page.recordedClick("macro_record");
  page.recordedTick(20000, 20000);
  page.recordedTick(23000, 20000);
  page.recordedReport(rawOf(23100, 0, "/input/thumbstick/y", 0.9f));
  page.recordedReport(rawOf(23200, 0, "/input/thumbstick/y", 0.1f));
  page.recordedTick(24000, 25300);
  m = page.recordedModel("", "");
  check(m.step == 3 && m.tiles.size() == 1 && m.tiles[0].kind == macro::Push && m.tiles[0].both && m.tiles[0].y == 100, "a stick records as a direction, on one tile");
  shotRecorded(m, out + "/recorded_stick.png");
  page.recordedClick("macro_pick");
  page.recordedTick(24100, 25300);
  page.recordedReport(rawOf(25600, 1, "/input/b/click", 1));
  page.recordedReport(rawOf(25700, 1, "/input/b/click", 0));
  page.recordedClick("macro_save");
  m = page.recordedModel("", "");
  check(m.saved.size() == 1 && m.status.find("in place of") != std::string::npos, "the same button replaces what was on it: " + m.status);

  // Recording with nothing pressed doesn't leave you stuck, and neither does choosing a button.
  page.recordedClick("macro_record");
  page.recordedTick(30000, 30000);
  page.recordedTick(33000, 30000);
  check(page.recordedListening(), "listening");
  page.recordedTick(53001, 30000);
  check(!page.recordedListening() && page.recordedModel("", "").step == 0, "nothing for 20 seconds and it gives up");
  page.recordedClick("macro_edit_0");
  page.recordedClick("macro_pick");
  page.recordedTick(60000, 0);
  page.recordedTick(75001, 0);
  check(!page.recordedListening() && page.recordedModel("", "").step == 3, "so does choosing a button");
  page.recordedClick("macro_cancel");

  // The dashboard shutting has to stop it listening, or the game loses its buttons.
  page.recordedClick("macro_record");
  page.recordedTick(80000, 0);
  page.recordedTick(83000, 0);
  check(page.recordedListening(), "listening again");
  page.recordedClick("macro_abort");
  check(!page.recordedListening(), "and the dashboard shutting ends it");

  // Remove.
  page.recordedClick("macro_del_0");
  check(page.recordedModel("", "").saved.empty() && page.status.find("usual job") != std::string::npos, "Remove gives the button back");
  page.fillInputTable(t);
  check(t.macros == 0, "and the driver stops doing it");

  // ---- What J4 found on a real Frame, 7 November.
  // The Frame's raw hand sensor reports like a stick pushed for as long as you're holding on. It isn't a control, and no game can bind it.
  // A firm grip from before it started isn't a step either, and neither of them stops it finishing.
  page.recordedClick("macro_record");
  page.recordedTick(100000, 100000);
  page.recordedTick(103000, 100000);
  page.recordedReport(rawOf(100020, 1, "/input/capsense/x", 0.9f));
  page.recordedReport(rawOf(100020, 1, "/input/grip/value", 0.95f));
  page.recordedReport(rawOf(100030, 1, "/input/grip/click", 1));
  page.recordedReport(rawOf(100040, 1, "/input/grip/touch", 1));
  page.recordedReport(rawOf(100040, 1, "/input/trigger/touch", 1));
  page.recordedReport(rawOf(100500, 1, "/input/a/touch", 1));
  page.recordedReport(rawOf(100550, 1, "/input/a/click", 1));
  page.recordedReport(rawOf(100700, 1, "/input/a/click", 0));
  page.recordedReport(rawOf(101000, 1, "/input/y/click", 1));
  page.recordedReport(rawOf(101200, 1, "/input/y/click", 0));
  page.recordedReport(rawOf(101900, 1, "/input/capsense/x", 0.85f));
  m = page.recordedModel("", "");
  check(m.tiles.size() == 2 && m.tiles[0].name == "A" && m.tiles[1].name == "Y" && m.tiles[0].both && m.tiles[1].both, "a real grip's take is A and Y, and nothing else");
  shotRecorded(m, out + "/recorded_two_presses.png");
  page.recordedTick(105100, 103100);
  check(page.recordedListening(), "not quite two seconds after the last press, it's still listening");
  page.recordedTick(105200, 103200);
  m = page.recordedModel("", "");
  check(m.step == 3 && m.tiles.size() == 2 && !m.hasBareTouches, "and it stops by itself two seconds after, still gripping, with no touches to ask about");
  check(m.asRecorded, "it plays the way you did it unless you say otherwise");
  shotRecorded(m, out + "/recorded_check_grip.png");

  // Choosing a button: with touches off, a thumb resting on one and lifting off doesn't pick it.
  page.recordedClick("macro_pick");
  page.recordedTick(106000, 106000);
  m = page.recordedModel("", "");
  check(!m.touchesOn, "touches are off to begin with");
  shotRecorded(m, out + "/recorded_choose_presses.png");
  page.recordedReport(rawOf(106020, 1, "/input/grip/value", 0.95f));
  page.recordedReport(rawOf(106300, 1, "/input/grip/value", 0.96f));
  page.recordedReport(rawOf(106400, 1, "/input/y/touch", 1));
  page.recordedReport(rawOf(106500, 1, "/input/y/touch", 0));
  page.recordedReport(rawOf(106520, 1, "/input/capsense/x", 0.95f));
  check(page.recordedListening(), "the grip, a touch and the sensor don't choose anything");
  page.recordedReport(rawOf(106700, 1, "/input/x/click", 1));
  page.recordedReport(rawOf(106800, 1, "/input/x/click", 0));
  m = page.recordedModel("", "");
  check(m.step == 3 && m.plays == "X", "pressing X does: " + m.plays);

  // With Capacitive touches on, the row's there even when a take has none, so it's plain that none were felt. Part 2, 7 November:
  // the slide happened in the countdown, so the take had none, and the empty space where the option should be looked broken.
  page.recordedClick("macro_touch_on");
  m = page.recordedModel("", "");
  check(m.step == 3 && m.touches == 1 && !m.hasBareTouches, "Capacitive touches on: the row shows, with nothing to choose");
  shotRecorded(m, out + "/recorded_touches_none.png");

  // On the page, the worst case for room: lots of touches kept, one picked, and every option showing.
  page.recordedClick("macro_cancel");
  page.recordedClick("macro_record");
  page.recordedTick(110000, 110000);
  page.recordedTick(113000, 110000);
  {
    const char* const on[4] = {"/input/a/touch", "/input/b/touch", "/input/x/touch", "/input/y/touch"};
    uint32_t at = 110500;
    for (int i = 0; i < 26; ++i) {
      page.recordedReport(rawOf(at, 1, on[i % 4], 1));
      page.recordedReport(rawOf(at + 20, 1, on[i % 4], 0));
      at += 60;
    }
    page.recordedReport(rawOf(at, 1, "/input/a/click", 1));
    page.recordedReport(rawOf(at + 20, 1, "/input/a/click", 0));
    page.recordedTick(120000, at + 2100);
  }
  m = page.recordedModel("", "");
  check(m.step == 3 && m.tiles.size() == 1 && m.hasBareTouches && !m.withTouches && m.touches == 2, "touches left out to begin with, with Capacitive touches there to ask for them");
  shotRecorded(m, out + "/recorded_touches_row.png");
  page.recordedClick("macro_touches");
  m = page.recordedModel("", "");
  check(m.tiles.size() == 27 && m.tiles[0].kind == macro::Touch && m.tiles[0].both, "Keep brings one quiet tile for each brush");
  shotRecorded(m, out + "/recorded_touches_kept.png");
  page.recordedClick("macro_tile_3");
  m = page.recordedModel("", "");
  check(m.tileFirst == 0 && m.selected == 3, "and one can be picked");
  shotRecorded(m, out + "/recorded_touches_picked.png");
  page.recordedClick("macro_steps_next");
  m = page.recordedModel("", "");
  check(m.tileFirst == kRecordedTilesPerPage && m.selected == -1, "More shows the ones that don't fit");
  shotRecorded(m, out + "/recorded_touches_more.png");
  page.recordedClick("macro_tile_26");
  m = page.recordedModel("", "");
  check(m.selected == 26 && m.tileFirst == kRecordedTilesPerPage, "a tile on that page can be picked");
  page.recordedClick("macro_left");
  page.recordedClick("macro_left");
  page.recordedClick("macro_left");
  m = page.recordedModel("", "");
  check(m.selected == 23 && m.tileFirst == 0, "and moving it back across the fold goes with it");
  shotRecorded(m, out + "/recorded_touches_fold.png");
  // Choosing a button with touches on: a resting thumb can choose.
  page.recordedClick("macro_pick");
  page.recordedTick(121000, 121000);
  m = page.recordedModel("", "");
  check(m.touchesOn, "with Capacitive touches on it says so");
  shotRecorded(m, out + "/recorded_choose_touches.png");
  page.recordedReport(rawOf(121400, 1, "/input/y/touch", 1));
  page.recordedReport(rawOf(121600, 1, "/input/y/touch", 0));
  m = page.recordedModel("", "");
  check(m.step == 3 && m.plays == "Touch Y" && m.onTouch, "and resting on Y chooses Touch Y: " + m.plays);
  page.recordedClick("macro_press_only");
  m = page.recordedModel("", "");
  check(m.note.find("Resting on Y") != std::string::npos && !m.noteWarn, "and says what that means, not in red: " + m.note);
  page.recordedClick("macro_save");
  m = page.recordedModel("", "");
  check(m.step == 0 && !m.saved.empty() && m.saved.back().plays == "Touch Y to play", "saved: " + (m.saved.empty() ? "" : m.saved.back().plays));
  shotRecorded(m, out + "/recorded_list_options.png");
  page.recordedClick("macro_del_" + std::to_string(m.saved.size() - 1));
  page.status.clear();

  // Too many presses for the driver to play says so.
  page.recordedClick("macro_record");
  page.recordedTick(130000, 130000);
  page.recordedTick(133000, 130000);
  {
    uint32_t at = 130500;
    for (int i = 0; i < 20; ++i) {
      page.recordedReport(rawOf(at, 1, i % 2 ? "/input/x/click" : "/input/y/click", 1));
      page.recordedReport(rawOf(at + 20, 1, i % 2 ? "/input/x/click" : "/input/y/click", 0));
      at += 60;
    }
    page.recordedTick(140000, at + 2100);
  }
  m = page.recordedModel("", "");
  check(m.tiles.size() == 20, "twenty presses, twenty tiles");
  page.recordedClick("macro_pick");
  page.recordedTick(141000, 141000);
  page.recordedReport(rawOf(141500, 1, "/input/b/click", 1));
  page.recordedReport(rawOf(141600, 1, "/input/b/click", 0));
  m = page.recordedModel("", "");
  check(m.note.find("first 16 presses") != std::string::npos && m.noteWarn, "and it says only 16 will play, in red: " + m.note);
  shotRecorded(m, out + "/recorded_too_long.png");
  page.recordedClick("macro_cancel");

  // The interview: one question at a time, with nothing counted until you say so.
  page.recordedClick("macro_touch_off");
  page.recordedClick("macro_interview_on");
  m = page.recordedModel("", "");
  check(m.interviewOn, "Interview's a switch on the list");
  page.recordedClick("macro_record");
  m = page.recordedModel("", "");
  check(m.step == 5 && m.ask == 0 && m.interview && !page.recordedListening(), "Record new starts by asking");
  shotRecorded(m, out + "/recorded_ask_1_start.png");
  page.recordedClick("macro_ask_start");
  page.recordedTick(150000, 150000);
  page.recordedTick(153000, 150000);
  m = page.recordedModel("", "");
  check(m.step == 2 && page.recordedListening(), "Start counts down and records as usual");
  page.recordedReport(rawOf(150020, 1, "/input/grip/value", 0.95f));
  page.recordedReport(rawOf(150040, 1, "/input/trigger/touch", 1));
  page.recordedReport(rawOf(150050, 1, "/input/capsense/x", 0.95f));
  page.recordedReport(rawOf(150500, 1, "/input/a/touch", 1));
  m = page.recordedModel("", "");
  check(m.feeling.find("Grip") != std::string::npos && m.feeling.find("Touch A") != std::string::npos && m.feeling.find("Touch Trigger") != std::string::npos && m.feeling.find("apsense") == std::string::npos, "it shows what it can feel: " + m.feeling);
  check(m.tiles.empty(), "and none of that's counted");
  shotRecorded(m, out + "/recorded_ask_feeling.png");
  page.recordedReport(rawOf(150600, 1, "/input/a/click", 1));
  page.recordedReport(rawOf(150700, 1, "/input/a/click", 0));
  page.recordedReport(rawOf(151000, 1, "/input/a/touch", 0));
  page.recordedTick(155000, 153100);
  m = page.recordedModel("", "");
  check(m.step == 5 && m.ask == 1 && m.tiles.size() == 1, "then it asks if that's what you did");
  shotRecorded(m, out + "/recorded_ask_2_check.png");
  page.recordedClick("macro_ask_back");
  check(page.recordedModel("", "").ask == 0, "Back goes back");
  page.recordedClick("macro_ask_start");
  page.recordedTick(156000, 156000);
  page.recordedTick(159000, 156000);
  page.recordedReport(rawOf(156500, 1, "/input/y/click", 1));
  page.recordedReport(rawOf(156600, 1, "/input/y/click", 0));
  page.recordedTick(161000, 158700);
  m = page.recordedModel("", "");
  check(m.step == 5 && m.ask == 1 && m.tiles.size() == 1 && m.tiles[0].name == "Y" && m.name == "Y", "and doing it again replaces it, and the name it came up with: " + m.name);
  page.recordedClick("macro_ask_next");
  m = page.recordedModel("", "");
  check(m.step == 5 && m.ask == 2 && m.plays.empty(), "next, which button");
  shotRecorded(m, out + "/recorded_ask_3_button.png");
  page.recordedClick("macro_ask_next");
  check(page.recordedModel("", "").ask == 2 && page.status.find("button") != std::string::npos, "it won't go on without one");
  page.status.clear();
  page.recordedClick("macro_pick");
  page.recordedTick(162000, 162000);
  check(page.recordedListening(), "Choose a button listens");
  page.recordedReport(rawOf(162400, 1, "/input/b/click", 1));
  page.recordedReport(rawOf(162500, 1, "/input/b/click", 0));
  m = page.recordedModel("", "");
  check(m.step == 5 && m.ask == 2 && m.plays == "B" && !m.note.empty(), "and comes back to ask if that's the one");
  shotRecorded(m, out + "/recorded_ask_3_chosen.png");
  page.recordedClick("macro_ask_next");
  m = page.recordedModel("", "");
  check(m.ask == 3, "next, what holding it does");
  shotRecorded(m, out + "/recorded_ask_4_hold.png");
  page.recordedClick("macro_repeat");
  m = page.recordedModel("", "");
  check(m.ask == 4 && m.repeat, "tapping an answer moves on");
  shotRecorded(m, out + "/recorded_ask_5_speed.png");
  page.recordedClick("macro_mine");
  m = page.recordedModel("", "");
  check(m.ask == 5 && m.asRecorded && m.name == "Y", "then the name");
  shotRecorded(m, out + "/recorded_ask_6_name.png");
  page.recordedClick("macro_save");
  m = page.recordedModel("", "");
  check(m.step == 0 && m.saved.back().plays == "Press B to play, repeats while held", "and it saves the same as any other");
  page.status.clear();
  const size_t lastSaved = m.saved.size() - 1;
  page.recordedClick("macro_edit_" + std::to_string(lastSaved));
  m = page.recordedModel("", "");
  check(m.step == 3 && !m.interview, "Edit goes straight to the options");
  page.recordedClick("macro_cancel");
  page.recordedClick("macro_interview_off");

  // An interview shut with the dashboard goes back to the same question, and doesn't lose the buttons.
  page.recordedClick("macro_interview_on");
  page.recordedClick("macro_record");
  page.recordedClick("macro_ask_start");
  page.recordedTick(170000, 170000);
  page.recordedTick(173000, 170000);
  check(page.recordedListening(), "listening in an interview");
  page.recordedClick("macro_abort");
  check(!page.recordedListening() && page.recordedModel("", "").step == 0, "the dashboard shutting ends it, with nothing to keep");
  page.recordedClick("macro_record");
  page.recordedClick("macro_ask_start");
  page.recordedTick(180000, 180000);
  page.recordedTick(183000, 180000);
  page.recordedTick(203001, 180000);
  m = page.recordedModel("", "");
  check(m.step == 5 && m.ask == 0 && page.status.find("Nothing was pressed") != std::string::npos, "and pressing nothing goes back to the first question");
  page.status.clear();
  page.recordedClick("macro_cancel");
  page.recordedClick("macro_interview_off");

  // Both options last, and an old take loses what was never a control. On the Frame these were saved from before the fix.
  page.recordedClick("macro_touch_on");
  page.recordedClick("macro_interview_on");
  {
    auto& mine = page.recorded["steam.app.777|frame_controller"];
    check(!mine.empty(), "there's a macro to test with");
    macro::Step sensorOn, sensorOff;
    sensorOn.hand = sensorOff.hand = 1;
    sensorOn.path = sensorOff.path = "/input/capsense";
    sensorOn.kind = macro::Push;
    sensorOn.x = 100;
    sensorOff.kind = macro::Centre;
    sensorOff.delayMs = 900;
    if (!mine.empty()) {
      const size_t before = mine[0].steps.size();
      mine[0].steps.insert(mine[0].steps.begin(), sensorOn);
      mine[0].steps.push_back(sensorOff);
      page.recordedClick("macro_touch_on");  // saves
      Page again(d, fake.make());
      again.load();
      check(again.macroTouches && again.macroInterview, "both options are still on next time");
      const auto& loaded = again.recorded["steam.app.777|frame_controller"];
      check(!loaded.empty() && loaded[0].steps.size() == before, "and the sensor's gone from an old take");
      mine[0].steps.erase(mine[0].steps.begin());
      mine[0].steps.pop_back();
    }
  }
  page.recordedClick("macro_touch_off");
  page.recordedClick("macro_interview_off");
  m = page.recordedModel("", "");
  check(!m.touchesOn && !m.interviewOn, "and both go off again");
  // These have left things on the buttons. The rest of this test starts with nothing saved.
  while (!page.recordedModel("", "").saved.empty()) page.recordedClick("macro_del_0");
  page.status.clear();
  page.fillInputTable(t);
  check(t.macros == 0, "nothing left over");

  // ---- A rewired button works straight away, when it only means sending one press as another.
  page.setReference("Quest 3");
  page.gameStarted();
  page.setHookState(false, false);
  page.click("bind_layout");
  check(page.status.find("next time") != std::string::npos, "with no button hook a layout waits for the next launch: " + page.status);
  // A new launch, on that layout.
  page.gameStarted();
  page.setHookState(true, true);
  page.click("y:right/input/a");
  page.click("t:right/input/b");
  check(page.status.find("Live in the game now") != std::string::npos, "rewiring A to do what's already on a button is live: " + page.status);
  page.fillInputTable(t);
  check(t.routes == 1 && t.route[0].fromHand == 1 && std::string(t.route[0].from) == "/input/a" && t.route[0].toHand == 1, "and it goes to the driver as a swap");
  const std::string carrier = t.routes ? t.route[0].to : "";
  check(carrier == "/input/x" || carrier == "/input/y" || carrier == "/input/b", "to a button that's on Quest B's job: " + carrier);
  // The next launch has it in the game's own binding, so the driver has to stop or the swap happens twice.
  page.gameStarted();
  page.fillInputTable(t);
  check(t.routes == 0, "next launch the binding has it, and the swap stops");
  // Nothing: a button rewired to do nothing.
  page.click("y:right/input/x");
  page.click("bind_nothing");
  page.fillInputTable(t);
  check(t.routes == 1 && t.route[0].toHand == fhlive::kNoHand && std::string(t.route[0].from) == "/input/x", "a button set to nothing does nothing right away");
  page.click("bind_reset_wiring");
  page.gameStarted();
  page.fillInputTable(t);
  check(t.routes == 0, "and undoing it undoes that");
  // Switching to a whole other layout isn't one press for another.
  page.click("bind_dev");
  check(page.status.find("next time") != std::string::npos, "a different layout still waits: " + page.status);
  page.gameStarted();

  // ---- A combo made while the game's running works straight away if its action is on a button.
  page.comboClick("combo_new");
  page.comboClick("c:right/input/a");
  page.comboClick("c:right/input/b");
  page.comboClick("combo_next");
  ComboModel cm = page.comboModel("", "");
  check(!cm.choices.empty(), "the combo has actions to pick from");
  if (!cm.choices.empty()) {
    page.comboClick(cm.choices[0].id);
    check(page.status.find("Live in the game now") != std::string::npos, "a combo for something on a button is live: " + page.status);
    page.fillInputTable(t);
    check(t.chords == 1 && t.chord[0].count == 2 && t.chord[0].in[0].onTouch == 1, "and goes to the driver as touches pressing that button");
    page.gameStarted();
    page.fillInputTable(t);
    check(t.chords == 0, "next launch SteamVR does it itself, so the driver stops");
  }
}

// Steam Link runs the dashboard from the headset, and the PC hears nothing from the buttons while it's open (J4's PC,
// 8 November: 922,870 reports in five takes, every one of them 0). So there it records with the dashboard shut.
static void testRecordShut(const std::string& work, const std::string& out) {
  const std::string steamvr = work + "/shut-steamvr", config = work + "/shut-config", data = work + "/shut-data";
  fixture::writeSteamVR(steamvr, config, work + "/shut-games");
  Dirs d;
  d.steamvr = steamvr;
  d.steam = work + "/shut-steam";
  d.data = data;
  d.config = {config};
  FakeSettings fake;
  Page page(d, fake.make());
  page.load();
  page.setYours("frame_controller", "{frame_controller}frame_controller_left", "{frame_controller}frame_controller_right");
  page.setGame("steam.app.777", "", "Named Game");
  giveModels(page, steamvr);
  page.setHookState(true, true);
  page.setRecordShut(true);
  PanelModel sign;

  page.recordedDashboard(true);
  page.recordedClick("macro_record");
  page.recordedDashboard(true);
  page.recordedTick(1000, 1000);
  RecordedModel m = page.recordedModel("", "");
  check(m.step == 1 && m.waitShut, "Record new waits for the dashboard to shut");
  check(!page.recordedListening() && !page.recordedSign(sign), "holding nothing back, and with no sign yet");
  shotRecorded(m, out + "/recorded_shut_wait.png");
  page.recordedTick(9000, 9000);
  check(page.recordedModel("", "").step == 1, "however long the dashboard stays open");

  // Shut: the countdown starts, and the game doesn't get your buttons from now on.
  page.recordedDashboard(false);
  check(!page.recordedModel("", "").waitShut && page.recordedListening(), "shut, it holds your buttons back straight away");
  page.recordedTick(10000, 10000);
  check(page.recordedSign(sign) && sign.chip == "3" && sign.text == "Let go of everything", "and the sign counts down: " + sign.chip + " " + sign.text);
  { Canvas cv(820, 300); drawPanel(cv, sign); cv.savePNG(out + "/recorded_shut_sign_countdown.png"); }
  page.recordedReport(rawOf(10500, 1, "/input/a/click", 1));  // pressed during the countdown: not a step
  page.recordedReport(rawOf(10700, 1, "/input/a/click", 0));
  page.recordedDashboard(false);
  page.recordedTick(13000, 13000);
  m = page.recordedModel("", "");
  check(m.step == 2 && page.recordedListening(), "then it records");
  check(page.recordedSign(sign) && sign.text == "Do your sequence now" && sign.status.rfind("Nothing yet", 0) == 0, "with the sign saying so: " + sign.status);
  page.recordedReport(rawOf(13050, 1, "/input/grip/touch", 1));  // the hand holding the controller, already there
  page.recordedReport(rawOf(13500, 1, "/input/a/touch", 1));
  page.recordedReport(rawOf(13550, 1, "/input/a/click", 1));
  page.recordedReport(rawOf(13700, 1, "/input/a/click", 0));
  page.recordedReport(rawOf(13750, 1, "/input/a/touch", 0));
  page.recordedReport(rawOf(14000, 1, "/input/b/click", 1));
  page.recordedReport(rawOf(14200, 1, "/input/b/click", 0));
  page.recordedDashboard(false);
  page.recordedTick(14300, 14300);
  check(page.recordedSign(sign) && sign.status.find("Heard A, B") == 0, "the sign says what it's heard: " + sign.status);
  check(sign.chip.empty(), "with no countdown left on it: " + sign.chip);
  { Canvas cv(820, 300); drawPanel(cv, sign); cv.savePNG(out + "/recorded_shut_sign_recording.png"); }
  std::string title, sub;
  check(!page.recordedReopen(&title, &sub), "nothing to open yet");
  page.recordedDashboard(false);
  page.recordedTick(16300, 16300);
  m = page.recordedModel("", "");
  check(m.step == 3 && !page.recordedListening(), "2 seconds after the last press it's done");
  check(m.tiles.size() == 2 && m.tiles[0].name == "A" && m.tiles[1].name == "B", "with A then B");
  page.recordedDashboard(false);
  check(page.recordedReopen(&title, &sub) && title == "Got it", "and the dashboard opens again by itself: " + title + ", " + sub);
  check(!page.recordedReopen(), "once");
  check(!page.recordedSign(sign), "the sign goes");
  page.recordedDashboard(true);

  // Choosing the button works the same way.
  page.recordedClick("macro_pick");
  page.recordedDashboard(true);
  m = page.recordedModel("", "");
  check(m.step == 4 && m.waitShut && !page.recordedListening(), "Choose a button waits for the dashboard to shut too");
  shotRecorded(m, out + "/recorded_shut_wait_choose.png");
  page.recordedDashboard(false);
  page.recordedTick(20000, 20000);
  check(page.recordedListening() && page.recordedSign(sign) && sign.header == "CHOOSE THE BUTTON", "shut, it listens, with a sign");
  page.recordedReport(rawOf(20050, 1, "/input/grip/touch", 1));
  page.recordedReport(rawOf(20500, 1, "/input/y/click", 1));
  page.recordedSign(sign);
  check(sign.status.find("Y") != std::string::npos, "it says what it got, and to let go: " + sign.status);
  page.recordedReport(rawOf(20700, 1, "/input/y/click", 0));
  page.recordedDashboard(false);
  m = page.recordedModel("", "");
  check(m.step == 3 && m.plays == "Y", "let go, and Y plays it: " + m.plays);
  check(page.recordedReopen(&title, &sub) && sub.find("Y plays it") == 0, "then back to the dashboard: " + sub);
  page.recordedDashboard(true);

  // Opening the dashboard part way through stops it, and keeps what you'd done.
  page.recordedClick("macro_again");
  page.recordedDashboard(false);
  page.recordedTick(30000, 30000);
  page.recordedTick(33000, 33000);
  check(page.recordedModel("", "").step == 2, "recording again");
  page.recordedReport(rawOf(33500, 0, "/input/trigger/click", 1));
  page.recordedReport(rawOf(33600, 0, "/input/trigger/click", 0));
  page.recordedDashboard(true);
  m = page.recordedModel("", "");
  check(m.step == 3 && m.tiles.size() == 1 && m.tiles[0].name == "Trigger", "the dashboard opened: it stopped with the trigger it heard");
  check(!page.recordedReopen(), "and doesn't try to open what's already open");

  // Changed your mind while it waits.
  page.recordedClick("macro_pick");
  page.recordedClick("macro_leave");
  m = page.recordedModel("", "");
  check(m.step == 3 && !m.waitShut, "leaving the page while it waits goes back to the steps");
  {
    Canvas cv(1200, 760);
    std::vector<Button> b;
    page.recordedClick("macro_pick");
    drawRecorded(cv, page.recordedModel("", ""), b);
    bool back = false, cancel = false;
    for (const Button& x : b) { back = back || x.id == "macro_leave"; cancel = cancel || x.id == "macro_cancel"; }
    check(back && !cancel, "the wait to choose a button only goes back, so the steps aren't thrown away");
    page.recordedClick("macro_leave");
  }
  page.recordedClick("macro_cancel");
  page.recordedClick("macro_record");
  page.recordedClick("macro_leave");
  m = page.recordedModel("", "");
  check(m.step == 0 && !m.waitShut && !page.recordedListening(), "and Cancel cancels the wait");

  // Nothing pressed: it says so.
  page.recordedClick("macro_record");
  page.recordedDashboard(false);
  page.recordedTick(40000, 40000);
  page.recordedTick(43000, 43000);
  page.recordedDashboard(false);
  page.recordedTick(63500, 63500);
  page.recordedDashboard(false);
  check(page.recordedReopen(&title, &sub) && title == "Nothing was pressed", "twenty seconds of nothing ends it: " + title);
  page.recordedDashboard(true);

  // Everywhere else it's as before: the dashboard shutting while it listens stops it.
  page.setRecordShut(false);
  page.recordedClick("macro_record");
  page.recordedDashboard(true);
  check(!page.recordedModel("", "").waitShut, "with the dashboard's own buttons, no waiting");
  page.recordedTick(50000, 50000);
  page.recordedTick(53000, 53000);
  page.recordedDashboard(true);
  check(page.recordedListening(), "it records with the dashboard open");
  page.recordedDashboard(false);
  check(!page.recordedListening() && page.recordedModel("", "").step == 0, "and shutting the dashboard stops it");
  check(!page.recordedSign(sign), "with no sign");
}

static void testGuides(const std::string& work) {
  const std::string steamvr = work + "/gsteamvr", config = work + "/gconfig", guides = work + "/guides.json";
  fixture::writeSteamVR(steamvr, config, work + "/ggames");
  const fs::path xr = fs::u8path(config) / "openxr";
  // Unreal names actions after the key, so each controller's are different and nothing connects them but the names.
  fixture::writeFile(xr / "steam.app.50_oculus_touch_0_binding.json", openxrBinding("oculus_touch", {{"right/input/a", "oculustouch_right_a_click"}, {"right/input/trigger", "oculustouch_right_trigger_click"}, {"left/input/x", "oculustouch_left_x_click"}}));
  fixture::writeFile(xr / "steam.app.50_frame_controller_0_binding.json", openxrBinding("frame_controller", {{"right/input/a", "framecontroller_right_a_click"}, {"right/input/trigger", "framecontroller_right_trigger_click"}, {"right/input/bumper", "framecontroller_right_bumper_click"}, {"left/input/dpad_down", "framecontroller_left_dpaddown_click"}}));
  fixture::writeFile(xr / "steam.app.51_frame_controller_0_binding.json", openxrBinding("frame_controller", {{"right/input/a", "framecontroller_right_a_click"}, {"left/input/dpad_down", "framecontroller_left_dpaddown_click"}}));
  fixture::writeFile(xr / "steam.app.53_oculus_touch_0_binding.json", openxrBinding("oculus_touch", {{"right/input/a", "primarybutton"}}));
  fixture::writeFile(xr / "steam.app.55_oculus_touch_0_binding.json", openxrBinding("oculus_touch", {{"right/input/a", "primarybutton"}, {"right/input/b", "secondarybutton"}}));
  // One thumb-touch action across A and the stick, as VRChat has, and one action on two buttons.
  fixture::writeFile(xr / "steam.app.56_oculus_touch_0_binding.json", R"({"category": "openxr", "controller_type": "oculus_touch", "interaction_profile": "/interaction_profiles/x", "name": "x", "bindings": {"/actions/game": {"sources": [
    {"inputs": {"touch": {"output": "/actions/game/in/thumb"}}, "mode": "button", "path": "/user/hand/right/input/joystick"},
    {"inputs": {"position": {"output": "/actions/game/in/turn"}}, "mode": "joystick", "path": "/user/hand/right/input/joystick"},
    {"inputs": {"touch": {"output": "/actions/game/in/thumb"}}, "mode": "button", "path": "/user/hand/right/input/a"},
    {"inputs": {"click": {"output": "/actions/game/in/jump"}}, "mode": "button", "path": "/user/hand/right/input/a"},
    {"inputs": {"click": {"output": "/actions/game/in/both"}}, "mode": "button", "path": "/user/hand/right/input/b"},
    {"inputs": {"click": {"output": "/actions/game/in/both"}}, "mode": "button", "path": "/user/hand/right/input/trigger"}
  ]}}})");
  fixture::writeFile(guides, R"({"games": [
    {"game": "Frame Known", "from": "the game's own files", "keys": ["steam.app.57"], "on": {"frame_controller": {"right/input/a": "Sprint"}}},
    {"game": "Thumb Game", "from": "guides and reviews", "keys": ["steam.app.56"], "on": {"oculus_touch": {"right/input/joystick": "Turn", "right/input/a": "Jump", "right/input/b": "One thing", "right/input/trigger": "Another"}}},
    {"game": "Unity Game", "from": "guides and reviews", "keys": ["steam.app.12345"], "on": {"oculus_touch": {"right/input/a": "Jump", "right/input/b": "Reload", "right/input/trigger": "Fire", "left/input/joystick": "Move", "left/input/joystick/click": "Run", "left/input/system": "Pause"}}},
    {"game": "Named Game", "from": "guides and reviews", "keys": ["steam.app.777"], "on": {"oculus_touch": {"right/input/trigger": "Wrong words", "right/input/b": "Fills a gap"}}},
    {"game": "Unreal Game", "from": "the game's own files", "keys": ["steam.app.50"], "on": {"oculus_touch": {"right/input/a": "Jump", "right/input/trigger": "Fire", "left/input/x": "Crouch"}}},
    {"game": "Exact Game", "from": "the game's own files", "keys": ["steam.app.51"], "on": {"oculus_touch": {"right/input/a": "Jump"}, "frame_controller": {"right/input/a": "Sprint", "left/input/dpad_down": "Duck"}}},
    {"game": "Never Played", "from": "the developer's guide", "keys": ["steam.app.52"], "on": {"oculus_touch": {"right/input/a": "Jump", "right/input/trigger": "Fire", "left/input/x": "Crouch"}}},
    {"game": "Half Bound", "from": "guides and reviews", "keys": ["steam.app.53"], "on": {"oculus_touch": {"right/input/a": "Jump", "right/input/b": "Nothing hears this"}}},
    {"game": "Translated Exact", "from": "the game's own files", "keys": ["steam.app.55"], "on": {"oculus_touch": {"right/input/a": "Jump", "right/input/b": "Reload"}, "frame_controller": {"right/input/a": "Sprint", "right/input/y": "Duck"}}},
    {"game": "Rift Copy", "from": "guides and reviews", "keys": ["steam.app.54"], "names": ["Other Name"], "on": {"oculus_touch": {"right/input/a": "Jump"}}}
  ]})");

  Dirs d;
  d.steamvr = steamvr;
  d.steam = work + "/gsteam";
  d.data = work + "/gdata";
  d.config = {config};
  d.guides = guides;
  FakeSettings fake;
  Library lib;
  lib.load(d);
  Page page(d, fake.make());
  page.load();
  page.setYours("frame_controller", "{frame_controller}frame_controller_left", "{frame_controller}frame_controller_right");
  std::string quest;
  for (const Headset& h : lib.headsets) if (h.type == "oculus_touch") { quest = h.name; break; }
  page.setReference(quest);

  // A Unity game with one Quest binding: the labels follow SteamVR's translation onto the Frame.
  page.setGame("steam.app.12345", "steam.app.12345", "Unity Game");
  BindModel m = page.model("", "");
  check(guideText(m.theirs, "t:right/input/a") == "Jump", "the guide labels Quest's A");
  check(guideText(m.theirs, "t:left/input/joystick") == "Move / Run", "a stick says both what pushing and pressing it do");
  check(guideText(m.theirs, "t:left/input/system") == "Pause", "and Quest's menu button is labelled");
  check(m.theirs.note == "What Unity Game does by default, from guides and reviews", "the page says where the words came from");
  check(guideText(m.yours, "y:right/input/a") == "Jump", "A on a Frame acts as the Quest's A, so it says Jump");
  check(guideText(m.yours, "y:right/input/x") == "Reload" && guideText(m.yours, "y:right/input/y") == "Reload", "X and Y act as B there");
  check(guideText(m.yours, "y:left/input/thumbstick") == "Move / Run", "a stick carries over");
  check(guideText(m.yours, "y:right/input/bumper") == "Fire", "SteamVR's translation makes the bumper a trigger here, so that's what it says");
  check(m.yours.note.find("translating") != std::string::npos, "the note about translating stays");
  const BindModel lit = page.model("t:right/input/a", "");
  check(lit.lit.count("y:right/input/a") == 1, "pointing at Jump lights up Jump on the other controller");

  // The combo list says the same: what the button does in the game, rather than the button's own name.
  page.comboClick("combo_new");
  page.comboClick("c:left/input/dpad_up");
  page.comboClick("c:left/input/dpad_down");
  page.comboClick("combo_next");
  {
    const ComboModel cm = page.comboModel("", "");
    bool jump = false, primary = false, reload = false;
    std::string listed;
    for (const ComboChoice& e : cm.choices) {
      listed += e.name + "; ";
      jump = jump || e.name == "Jump";
      reload = reload || e.name == "Reload";
      primary = primary || e.name == "Primary";
    }
    check(jump && reload, "a combo can pick what the guide calls it: " + listed);
    check(!primary, "instead of Primary");
    check(cm.note.empty(), "and it doesn't say the game only names its buttons");
  }
  page.comboClick("combo_back");
  page.comboClick("combo_cancel");

  // A game that names its own buttons keeps its own words, and a guide only fills what it leaves blank.
  page.setGame("steam.app.777", "steam.app.777", "Named Game");
  m = page.model("", "");
  check(guideText(m.theirs, "t:right/input/trigger") == "Fire the gun", "a game's own words beat a guide");
  check(guideRow(m.theirs, "t:right/input/b") && guideRow(m.theirs, "t:right/input/b")->unused, "and a button it doesn't use stays Not used, whatever a guide says");

  // Unless the entry says its words are better, for a game whose names are only for its developers.
  {
    fixture::writeFile(work + "/guides_over.json", R"({"games": [{"game": "Named Game", "from": "the developer's guide", "keys": ["steam.app.777"], "over_games_words": true, "on": {"oculus_touch": {"right/input/trigger": "Shoot"}}}]})");
    Dirs over = d;
    over.guides = work + "/guides_over.json";
    FakeSettings overFake;
    Page overPage(over, overFake.make());
    overPage.load();
    overPage.setYours("frame_controller", "{frame_controller}frame_controller_left", "{frame_controller}frame_controller_right");
    overPage.setReference(quest);
    overPage.setGame("steam.app.777", "steam.app.777", "Named Game");
    const BindModel o = overPage.model("", "");
    check(guideText(o.theirs, "t:right/input/trigger") == "Shoot", "an entry can say its words beat the game's");
    check(guideText(o.theirs, "t:right/input/a") == "Reload", "and the game's words stay on what it doesn't cover");
  }

  // Each controller with its own action names: nothing connects them, so nothing carries over, not even A to A. Games do
  // give another controller a different layout. It's still in use, it just doesn't say what for.
  page.setGame("steam.app.50", "steam.app.50", "Unreal Game");
  m = page.model("", "");
  check(guideText(m.theirs, "t:right/input/a") == "Jump", "the controller the guide describes is labelled");
  check(guideText(m.yours, "y:right/input/a").empty(), "framecontroller_right_a isn't assumed to be the same as Touch's A");
  check(guideText(m.yours, "y:right/input/trigger").empty(), "nor is its trigger");
  check(guideText(m.yours, "y:right/input/bumper").empty(), "nor its bumper");
  check(guideRow(m.yours, "y:right/input/a") && !guideRow(m.yours, "y:right/input/a")->unused, "and none of that makes it Not used");

  // A source that covers the Frame by name is used as it stands, with nothing carried over.
  page.setGame("steam.app.51", "steam.app.51", "Exact Game");
  m = page.model("", "");
  check(guideText(m.yours, "y:right/input/a") == "Sprint", "the Frame's own entry beats Touch's");
  check(guideText(m.yours, "y:left/input/dpad_down") == "Duck", "including a button Touch hasn't got");

  // The game has a Frame layout, but it isn't running on it here: SteamVR is translating Quest's. The game thinks it's holding
  // a Quest controller, so the Quest's words are the true ones and the Frame's would be wrong.
  page.setGame("steam.app.55", "steam.app.55", "Translated Exact");
  m = page.model("", "");
  check(m.yours.note.find("translating") != std::string::npos, "this one is being translated");
  check(guideText(m.yours, "y:right/input/a") == "Jump", "so A says what it does through the translation, not what the Frame layout has on it");
  check(guideText(m.yours, "y:right/input/y") == "Reload", "and Y acts as the Quest's B");

  // Not played here, but the source covers the Frame, so there's something to show and the note says where it's from.
  page.setGame("steam.app.57", "steam.app.57", "Frame Known");
  m = page.model("", "");
  check(guideText(m.yours, "y:right/input/a") == "Sprint", "the Frame's own words show with no bindings on disk");
  check(m.yours.note == "From the game's own files", "and the note says where they're from rather than asking you to play it");

  // A touch sensor shared across buttons says nothing about which button is which, and an action on two buttons can't be placed.
  page.setGame("steam.app.56", "steam.app.56", "Thumb Game");
  m = page.model("", "");
  check(guideText(m.yours, "y:right/input/a") == "Jump", "A follows what pressing it does, not the thumb sensor it shares with the stick");
  check(guideText(m.yours, "y:right/input/thumbstick") == "Turn", "and the stick is still the stick");
  check(guideText(m.yours, "y:right/input/x").empty(), "an action on two of the source's buttons could be either, so it stays blank");

  // Never played here, so no bindings at all. The controller the guide describes still gets its words, and nothing else does.
  page.setGame("steam.app.52", "steam.app.52", "Never Played");
  m = page.model("", "");
  check(guideText(m.theirs, "t:right/input/a") == "Jump", "a game with no bindings on disk still gets its guide");
  check(m.theirs.note == "What Never Played does by default, from the developer's guide", "with the same note");
  check(guideText(m.yours, "y:left/input/dpad_down").empty(), "the Frame can't be proven the same, so it stays blank");
  check(m.yours.note == "No bindings found yet. Play it once", "and it says why");

  // A button the game's own binding doesn't have can't do what a guide says.
  page.setGame("steam.app.53", "steam.app.53", "Half Bound");
  m = page.model("", "");
  check(guideText(m.theirs, "t:right/input/a") == "Jump", "what's bound is labelled");
  check(guideRow(m.theirs, "t:right/input/b") && guideRow(m.theirs, "t:right/input/b")->unused && guideText(m.theirs, "t:right/input/b").empty(), "and what isn't stays Not used");

  // Found by name when the key doesn't match, as with a copy that isn't Steam's.
  std::vector<GameGuide> list = loadGuides(guides);
  check(list.size() == 10, "the guides are read");
  check(findGuide(list, "steam.app.999", "", "Rift Copy (Rift)") && findGuide(list, "steam.app.999", "", "Rift Copy (Rift)")->game == "Rift Copy", "brackets don't stop a name matching");
  check(findGuide(list, "steam.app.999", "", "Other Name") && findGuide(list, "steam.app.999", "", "Other Name")->game == "Rift Copy", "nor does a second name");
  check(!findGuide(list, "steam.app.999", "", "Not Listed"), "and a game that isn't listed has no guide");
  check(loadGuides(work + "/missing.json").empty(), "no file just means no guides");

  // No guide, no change.
  page.setGame("steam.app.888", "steam.app.888", "Not Listed");
  m = page.model("", "");
  check(m.theirs.note.find("from") == std::string::npos, "a game with no guide doesn't claim a source");
}

// The table itself, as it ships. Every entry has to be something the page can show, and say where it came from.
static void testGuideTable() {
  const fs::path file = fs::u8path(platform::exeDir()) / "input" / "game_labels.json";
  Json::Value v;
  {
    Json::CharReaderBuilder b;
    std::string errs;
    std::ifstream in(file, std::ios::binary);
    check(in && Json::parseFromStream(b, in, &v, &errs), "input/game_labels.json can be read");
  }
  static const std::map<std::string, std::set<std::string>> rowsOf = {
      {"oculus_touch", {"left/input/x", "left/input/y", "left/input/system", "left/input/trigger", "left/input/grip", "left/input/joystick", "left/input/joystick/click", "right/input/a", "right/input/b", "right/input/trigger", "right/input/grip", "right/input/joystick", "right/input/joystick/click"}},
      {"knuckles", {"left/input/a", "left/input/b", "left/input/trigger", "left/input/grip", "left/input/thumbstick", "left/input/thumbstick/click", "right/input/a", "right/input/b", "right/input/trigger", "right/input/grip", "right/input/thumbstick", "right/input/thumbstick/click"}},
      {"frame_controller", {"left/input/view", "left/input/dpad_up", "left/input/dpad_down", "left/input/dpad_left", "left/input/dpad_right", "left/input/bumper", "left/input/trigger", "left/input/grip", "left/input/thumbstick", "left/input/thumbstick/click", "right/input/menu", "right/input/a", "right/input/b", "right/input/x", "right/input/y", "right/input/bumper", "right/input/trigger", "right/input/grip", "right/input/thumbstick", "right/input/thumbstick/click"}}};
  static const std::set<std::string> sources = {"the game's own files", "the developer's guide", "guides and reviews"};
  std::set<std::string> keys, names;
  int labels = 0;
  for (const Json::Value& e : v["games"]) {
    const std::string game = e["game"].asString();
    check(!game.empty(), "every entry names its game");
    check(sources.count(e["from"].asString()) == 1, game + ": says what kind of source it came from");
    check(e["sources"].isArray() && e["sources"].size() >= 1, game + ": lists its sources");
    for (const Json::Value& s : e["sources"]) check(!s.asString().empty(), game + ": a source isn't blank");
    check(names.insert(game).second, game + ": is only listed once");
    for (const Json::Value& k : e["keys"]) check(keys.insert(k.asString()).second, game + ": " + k.asString() + " isn't claimed twice");
    check(e["on"].size() >= 1, game + ": has something to say");
    check(!e["checked"].asString().empty(), game + ": says when it was checked");
    check(e["headset_checked"].isBool(), game + ": says whether anyone's confirmed it in a headset");
    for (const std::string& type : e["on"].getMemberNames()) {
      auto rows = rowsOf.find(type);
      check(rows != rowsOf.end(), game + ": " + type + " is a controller the table knows");
      if (rows == rowsOf.end()) continue;
      check(e["on"][type].size() >= 1, game + ": " + type + " isn't empty");
      for (const std::string& k : e["on"][type].getMemberNames()) {
        const std::string words = e["on"][type][k].asString();
        check(rows->second.count(k) == 1, game + ": " + k + " is a " + type + " button");
        check(!words.empty() && words.size() <= 30, game + ": " + k + " fits on a row: " + words);
        ++labels;
      }
    }
  }
  printf("game labels: %d games, %d labels\n", int(v["games"].size()), labels);
  check(v["games"].size() >= 1, "the table isn't empty");
}

// Every game the table knows that's been played on this machine, drawn on a Frame next to a Quest 3 for checking by eye. The
// bindings on disk also check the table: a button it labels that the game's own Touch binding hasn't got gets printed.
static void testGuideShots(const std::string& work, const std::string& out) {
  Dirs d;
  d.steamvr = platform::steamvrDir();
  d.steam = platform::steamDir();
  d.data = work + "/shotsdata";
  d.config = platform::configDirs();
  d.guides = (fs::u8path(platform::exeDir()) / "input" / "game_labels.json").generic_u8string();
  Library lib;
  lib.load(d);
  if (lib.profiles.empty()) return;
  FakeSettings fake;
  Page page(d, fake.make());
  page.load();
  page.setYours("frame_controller", "frame_controller_left", "frame_controller_right");
  page.setReference("Quest 3");
  giveModels(page, d.steamvr);
  int drawn = 0;
  std::error_code ec;
  fs::create_directories(fs::u8path(out) / "guides", ec);

  // Anything in a real OpenXR binding that reads as the game's own word gets shown as one, so here's what those are.
  {
    std::set<std::string> words;
    for (const std::string& dir : d.config)
      for (const auto& f : fs::directory_iterator(fs::u8path(dir) / "openxr", ec)) {
        if (f.path().extension() != ".json") continue;
        Json::Value v;
        Json::CharReaderBuilder rb;
        std::string errs;
        std::ifstream in(f.path(), std::ios::binary);
        if (!in || !Json::parseFromStream(rb, in, &v, &errs)) continue;
        for (const auto& set : v["bindings"].getMemberNames())
          for (const auto& src : v["bindings"][set]["sources"])
            for (const auto& name : src["inputs"].getMemberNames()) {
              const std::string output = src["inputs"][name]["output"].asString();
              bool pass = false, hide = false;
              const std::string label = actionLabel(output, {}, &pass, &hide);
              if (!pass && !hide && !label.empty()) words.insert(output.substr(output.rfind('/') + 1) + "=" + label);
            }
      }
    printf("openxr action names read as real words (%zu):", words.size());
    for (const std::string& w : words) printf(" %s;", w.c_str());
    printf("\n");
  }
  Settings settings = fake.make();
  for (const GameGuide& g : loadGuides(d.guides)) {
    const std::string key = g.keys.empty() ? "steam.app.0" : g.keys[0];
    const Found touch = findBinding(d, lib, settings, {key}, "oculus_touch");
    page.setGame(key, key, g.game);
    giveModels(page, d.steamvr);
    const BindModel m = page.model("", "");
    check(m.theirs.note.find(g.from) != std::string::npos || !touch.names.empty(), g.game + ": the page says where its labels came from");
    auto touchWords = g.on.find("oculus_touch");
    if (touch.ok && touch.names.empty() && touchWords != g.on.end())
      for (const auto& [path, words] : touchWords->second) {
        const std::string row = path.size() > 6 && path.compare(path.size() - 6, 6, "/click") == 0 ? path.substr(0, path.size() - 6) : path;
        const BindRow* r = guideRow(m.theirs, "t:" + row);
        if (!r || r->action.find(words) == std::string::npos) printf("  note: %s says %s = %s, but the game's Quest binding doesn't have that button\n", g.game.c_str(), path.c_str(), words.c_str());
      }
    std::string safe = g.game;
    for (char& ch : safe) if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
    shot(m, (fs::u8path(out) / "guides" / (safe + ".png")).generic_u8string());
    // And the Combos page, because its list of what a combo can set off uses the same words.
    if (page.comboModel("", "").canMake) {
      page.comboClick("combo_new");
      page.comboClick("c:right/input/a");
      page.comboClick("c:right/input/b");
      page.comboClick("combo_next");
      shotCombo(page.comboModel("", ""), (fs::u8path(out) / "guides" / (safe + "_combos.png")).generic_u8string());
      page.comboClick("combo_back");
      page.comboClick("combo_cancel");
    }
    ++drawn;
  }
  printf("drew %d games from the table\n", drawn);
}

// ---------------------------------------------------------------- the real SteamVR on this machine, if there is one
static void testReal(const std::string& work, const std::string& out) {
  Dirs d;
  d.steamvr = platform::steamvrDir();
  d.steam = platform::steamDir();
  d.data = work + "/realdata";
  d.config = platform::configDirs();
  Library lib;
  lib.load(d);
  if (lib.profiles.empty()) { printf("no SteamVR on this machine, skipping the real checks\n"); return; }
  printf("real SteamVR: %zu controller types, %zu to compare with\n", lib.profiles.size(), lib.headsets.size());

  // Every controller there is: a model that loads, and each of its rows pointing somewhere on the picture.
  FakeSettings fake;
  Page page(d, fake.make());
  page.load();
  page.setYours("frame_controller", "frame_controller_left", "frame_controller_right");
  page.setGame("", "", "");
  for (const Headset& h : lib.headsets) {
    page.setReference(h.name);
    giveModels(page, d.steamvr);
    const BindModel m = page.model("", "");
    check(m.theirs.pic[0] && m.theirs.pic[1], h.name + ": the picture loads");
    for (int hand = 0; hand < 2; ++hand)
      for (const BindRow& r : m.theirs.rows[hand])
        check(r.x >= 0 && r.x <= 1 && r.y >= 0 && r.y <= 1, h.name + ": " + r.id + " points at a spot on the picture");
    std::string safe = h.name;
    for (char& ch : safe) if (ch == ' ') ch = '_';
    shot(m, out + "/ref_" + safe + ".png");
  }

  // SteamVR leaves its last translation on disk. Doing the same one must give the same result.
  for (const std::string& dir : d.config) {
    Json::Value from, to;
    Json::CharReaderBuilder b;
    std::string errs;
    std::ifstream a(fs::u8path(dir) / "openxr" / "auto-remapping_fromBinding.json"), c(fs::u8path(dir) / "openxr" / "auto-remapping_toBinding.json");
    if (!a || !c || !Json::parseFromStream(b, a, &from, &errs) || !Json::parseFromStream(b, c, &to, &errs)) continue;
    const std::string fromType = from["controller_type"].asString();
    std::set<std::string> want = sourcesOf(to);
    bool onFrame = false;
    for (const auto& s : want) onFrame = onFrame || s.find("/input/dpad_") != std::string::npos;
    if (!onFrame || !lib.profile(fromType)) { printf("SteamVR's last translation wasn't to a Frame, skipping that check\n"); continue; }
    const Json::Value ours = translate(from, defaultWiring(lib, fromType, "frame_controller"), *lib.profile(fromType), *lib.profile("frame_controller"));
    const std::set<std::string> got = sourcesOf(ours);
    int missing = 0, extra = 0;
    for (const auto& s : want) if (!got.count(s)) { ++missing; printf("  SteamVR has, we don't: %s\n", s.c_str()); }
    for (const auto& s : got) if (!want.count(s)) { ++extra; printf("  we have, SteamVR doesn't: %s\n", s.c_str()); }
    check(missing == 0 && extra == 0, "the translation matches SteamVR's own, from " + fromType);
    printf("matches SteamVR's own translation from %s: %zu of %zu\n", fromType.c_str(), got.size() - extra, want.size());
    break;
  }

  // Real games, for the pictures. These only show anything on a machine that's played them.
  struct { const char* key; const char* name; const char* file; } games[] = {
      {"steam.app.555160", "Pavlov VR", "pavlov"},
      {"steam.app.2496180", "GhostTown", "ghosttown"},
      {"steam.app.546560", "Half-Life: Alyx", "alyx"},
      {"steam.app.275850", "No Man's Sky", "nms"},
  };
  page.setReference("Quest 3");
  for (const auto& g : games) {
    page.setGame(g.key, "", g.name);
    giveModels(page, d.steamvr);
    const BindModel m = page.model("", "");
    shot(m, out + "/game_" + g.file + ".png");
    shot(page.model("y:right/input/x", ""), out + "/game_" + g.file + "_hover.png");
    shot(page.model("y:right/input/a", ""), out + "/game_" + g.file + "_hover_a.png");
    if (m.layoutOk) {
      page.click("bind_scope_game");
      page.click("bind_layout");
      shot(page.model("", ""), out + "/game_" + g.file + "_layout.png");
      page.status.clear();
      shot(page.model("y:right/input/x", ""), out + "/game_" + g.file + "_layout_hover.png");
      page.click("bind_dev");
      page.status.clear();  // the program times these out, and the next picture shouldn't show this one's
    }
  }
  page.setGame("steam.app.555160", "", "Pavlov VR");
  page.click("y:left/input/dpad_up");
  shot(page.model("", ""), out + "/game_pavlov_armed.png");
  page.click("bind_cancel");

  // The combos page on the real controllers and real games: the ones with pictures above, and whatever else has run on this
  // machine. Only ever writes in the throwaway folder and the fake settings.
  struct RealGame { std::string key, name, file; };
  std::vector<RealGame> realGames;
  for (const auto& g : games) realGames.push_back({g.key, g.name, g.file});
  {
    std::set<std::string> seen;
    for (const auto& g : games) seen.insert(g.key);
    for (const std::string& dir : d.config) {
      std::error_code ec;
      for (const auto& f : fs::directory_iterator(fs::u8path(dir) / "openxr", ec)) {
        const std::string name = f.path().filename().u8string();
        const size_t at = name.find("_frame_controller_");
        if (at == std::string::npos || !seen.insert(name.substr(0, at)).second || realGames.size() >= 40) continue;
        realGames.push_back({name.substr(0, at), name.substr(0, at), ""});
      }
    }
  }
  for (const RealGame& g : realGames) {
    const std::string shotName = g.file.empty() ? "" : out + "/game_" + g.file + "_combos";
    page.setGame(g.key, "", g.name);
    page.status.clear();
    giveModels(page, d.steamvr);
    ComboModel cm = page.comboModel("", "");
    if (!shotName.empty()) shotCombo(cm, shotName + ".png");
    if (!cm.canMake) continue;
    // Every control this controller has can be picked, and reads back as text.
    for (int h = 0; h < 2; ++h)
      for (const BindRow& r : cm.yours.rows[h]) check(!r.name.empty() && r.id.rfind("c:", 0) == 0, g.name + ": " + r.id + " can be picked");
    page.comboClick("combo_new");
    page.comboClick("c:right/input/a");
    page.comboClick("c:right/input/b");
    if (!shotName.empty()) shotCombo(page.comboModel("", ""), shotName + "_step1.png");
    page.comboClick("c:right/input/grip");
    page.comboClick("c:right/input/grip");
    if (!shotName.empty()) shotCombo(page.comboModel("", ""), shotName + "_step1b.png");
    page.comboClick("c:right/input/grip");
    page.comboClick("combo_next");
    cm = page.comboModel("", "");
    if (!shotName.empty()) shotCombo(cm, shotName + "_step2.png");
    printf("%s: %zu combo choices on this page, of %d page(s), controls '%s'\n", g.name.c_str(), cm.choices.size(), cm.pages, cm.controls.c_str());
    // A game can have nothing that's on or off, so an empty list is fine here. It just says so.
    if (cm.choices.empty()) { page.comboClick("combo_back"); page.comboClick("combo_cancel"); continue; }
    page.comboClick(cm.choices[0].id);
    cm = page.comboModel("", "");
    check(cm.saved.size() == 1, g.name + ": it saves");
    if (!shotName.empty()) shotCombo(page.comboModel("combo_row_0", ""), shotName + "_saved.png");
    const Json::Value w = readFile(page.fileFor(g.key, "frame_controller"));
    int chords = 0;
    for (const auto& set : w["bindings"].getMemberNames()) chords += int(w["bindings"][set]["chords"].size());
    check(chords >= 1, g.name + ": the chord is in the file");
    check(w["controller_type"].asString() == "frame_controller", g.name + ": and the file's for this controller");
    page.comboClick("combo_del_0");
    check(!fs::exists(fs::u8path(page.fileFor(g.key, "frame_controller"))), g.name + ": and it all comes back out");
    page.status.clear();
  }
  page.setGame("steam.app.555160", "", "Pavlov VR");

  // What it costs. None of this runs unless the page is open.
  {
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };
    auto t = Clock::now();
    Library again;
    again.load(d);
    printf("reading SteamVR's controller files: %.1f ms\n", ms(t));
    FakeSettings quiet;
    Page timed(d, quiet.make());
    timed.load();
    timed.setYours("frame_controller", "frame_controller_left", "frame_controller_right");
    timed.setGame("steam.app.275850", "", "No Man's Sky");
    timed.setReference("Quest 3");
    t = Clock::now();
    timed.model("", "");
    printf("finding a game's bindings: %.1f ms\n", ms(t));
    t = Clock::now();
    giveModels(timed, d.steamvr);
    printf("reading four models from disk (the test's way, not SteamVR's): %.1f ms\n", ms(t));
    t = Clock::now();
    timed.model("", "");
    printf("drawing four controllers once: %.1f ms\n", ms(t));
    Canvas cv(1200, 760);
    std::vector<Button> b;
    t = Clock::now();
    for (int i = 0; i < 20; ++i) drawBindings(cv, timed.model(i % 2 ? "y:right/input/a" : "", ""), b);
    printf("redrawing the page for a laser move: %.1f ms\n", ms(t) / 20);
  }

  // Your controller being a Quest, comparing with a Frame: the same page the other way up.
  page.setYours("oculus_touch", "oculus_quest_plus_controller_left", "oculus_quest_plus_controller_right");
  page.setReference("Steam Frame");
  giveModels(page, d.steamvr);
  shot(page.model("", ""), out + "/game_pavlov_quest_yours.png");
}

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : ".";
  std::error_code ec;
  const fs::path tmp = fs::temp_directory_path(ec) / "utmtftm-test-bindings";
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  fs::create_directories(fs::u8path(out), ec);
  testFixture(tmp.generic_u8string(), out);
  testRecorded(tmp.generic_u8string(), out);
  testRecordShut(tmp.generic_u8string(), out);
  testGuides(tmp.generic_u8string());
  testGuideTable();
  testGuideShots(tmp.generic_u8string(), out);
  testReal(tmp.generic_u8string(), out);
  fs::remove_all(tmp, ec);
  printf("test_bindings: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}

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

  // Names for what's bound.
  bool pass = false, hide = false;
  check(actionLabel("/actions/ue/in/oculustouch_right_a_click", {}, &pass, &hide) == "A" && pass && !hide, "Unreal's names only say the button");
  actionLabel("/actions/ue/in/oculustouch_left_thumbstick_up", {}, &pass, &hide);
  check(hide, "a stick's four directions don't get rows of their own");
  actionLabel("/actions/ue/in/framecontroller_right_a_touch", {}, &pass, &hide);
  check(hide, "touching isn't something to list");
  check(actionLabel("/actions/x/in/primarybutton", {}, &pass, &hide) == "Primary" && !pass && !hide, "Unity's role names read as they are");
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
  testReal(tmp.generic_u8string(), out);
  fs::remove_all(tmp, ec);
  printf("test_bindings: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}

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

  // Unknown things don't take it down.
  page.setYours("some_future_controller", "", "");
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

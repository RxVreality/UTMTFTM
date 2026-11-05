// Checks the button bindings for every kind of controller against each other and, when SteamVR is installed,
// against SteamVR's own description of each controller. No headset or running SteamVR needed.
// A wrong path here doesn't fail anywhere visible: that button just silently does nothing on that controller.
#include <json/json.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include "controls.h"
#include "platform.h"

namespace fs = std::filesystem;
using namespace fh;

static int fails = 0, checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) { ++fails; printf("FAIL %s\n", what.c_str()); }
}

static bool readJson(const fs::path& p, Json::Value& v) {
  std::ifstream in(p);
  Json::CharReaderBuilder b;
  std::string errs;
  return in && Json::parseFromStream(b, in, &v, &errs);
}

// Everything bound in one file, as "hand/input" -> the actions it drives.
struct Bound {
  std::map<std::string, std::set<std::string>> byInput;
  std::set<std::string> hasAction(const std::string& action) const {
    std::set<std::string> where;
    for (const auto& [input, actions] : byInput) if (actions.count(action)) where.insert(input);
    return where;
  }
};

static std::string after(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0 ? s.substr(prefix.size()) : ""; }

// SteamVR's description of each controller it ships, by controller_type.
static std::map<std::string, Json::Value> steamProfiles() {
  std::map<std::string, Json::Value> out;
  fs::path root = fs::u8path(platform::steamvrDir());
  std::error_code ec;
  std::vector<fs::path> dirs = {root / "resources" / "input"};
  for (const auto& d : fs::directory_iterator(root / "drivers", ec)) dirs.push_back(d.path() / "resources" / "input");
  for (const auto& d : dirs)
    for (const auto& f : fs::directory_iterator(d, ec)) {
      if (f.path().extension() != ".json" || f.path().filename().string().find("profile") == std::string::npos) continue;
      Json::Value v;
      if (!readJson(f.path(), v) || !v.isMember("input_source") || !v.isMember("controller_type")) continue;
      // A driver's own description beats the default in resources/input.
      out[v["controller_type"].asString()] = v;
    }
  return out;
}

int main() {
  fs::path input = fs::u8path(platform::exeDir()) / "input";
  Json::Value manifest;
  if (!readJson(input / "actions.json", manifest)) { printf("FAIL can't read %s\n", (input / "actions.json").string().c_str()); return 1; }

  auto profiles = steamProfiles();
  printf("SteamVR describes %zu controller types on this PC\n", profiles.size());

  std::set<std::string> ours;
  for (const auto& db : manifest["default_bindings"]) {
    const std::string type = db["controller_type"].asString();
    ours.insert(type);
    Json::Value b;
    check(readJson(input / db["binding_url"].asString(), b), type + ": binding file reads");
    check(b["controller_type"].asString() == type, type + ": binding file names the same controller");
    const Json::Value& set = b["bindings"]["/actions/fh"];

    Bound bound;
    auto note = [&](const std::string& path, const std::string& output) {
      std::string rest = after(path, "/user/hand/");
      bound.byInput[rest].insert(after(output, "/actions/fh/in/"));
    };
    for (const auto& s : set["sources"])
      for (const auto& name : s["inputs"].getMemberNames()) note(s["path"].asString(), s["inputs"][name]["output"].asString());
    for (const auto& p : set["poses"]) note(p["path"].asString(), p["output"].asString());

    // Every control works from either hand, because the hand being placed may be either one.
    for (const char* action : {"grab", "done", "reset", "cancel", "raw", "grip"})
      for (const char* hand : {"left/", "right/"}) {
        bool found = false;
        for (const auto& where : bound.hasAction(action)) found = found || where.rfind(hand, 0) == 0;
        check(found, type + ": " + action + " is on the " + hand + " hand");
      }

    // The button names on the panel really are the buttons bound.
    ButtonNames names = buttonNames(type);
    auto named = [&](const std::string& label, const char* action) {
      std::string token;
      auto one = [&](std::string t) {
        for (auto& c : t) c = char(tolower((unsigned char)c));
        if (t == "menu") t = "application_menu";
        check(bound.byInput["left/input/" + t].count(action) || bound.byInput["right/input/" + t].count(action),
              type + ": panel says '" + label + "' for " + action + ", and " + t + " isn't bound to it");
      };
      for (char c : label + " ") {
        if (c == ' ' || c == '/') { if (!token.empty()) one(token); token.clear(); }
        else token += c;
      }
    };
    named(names.reset, "reset");
    named(names.cancel, "cancel");

    auto pit = profiles.find(type);
    if (pit == profiles.end()) { printf("  %-26s no SteamVR description on this PC, paths not checked\n", type.c_str()); continue; }
    const Json::Value& src = pit->second["input_source"];

    // Every path exists on this controller, and on this side of it.
    auto exists = [&](const std::string& path, const std::string& what) {
      std::string rest = after(path, "/user/hand/");
      size_t slash = rest.find('/');
      std::string hand = rest.substr(0, slash), key = rest.substr(slash);
      check(src.isMember(key), type + ": " + what + " " + path + " isn't on this controller");
      if (src[key].isMember("side")) check(src[key]["side"].asString() == hand, type + ": " + path + " is on the other hand");
    };
    for (const auto& s : set["sources"]) {
      exists(s["path"].asString(), "input");
      std::string key = after(s["path"].asString(), "/user/hand/left");
      if (key.empty()) key = after(s["path"].asString(), "/user/hand/right");
      std::string mode = s["mode"].asString(), kind = src[key]["type"].asString();
      if (mode == "joystick" || mode == "trackpad") check(kind == mode, type + ": " + key + " is a " + kind + ", bound as a " + mode);
    }
    for (const auto& p : set["poses"]) exists(p["path"].asString(), "pose");
    for (const auto& p : set["haptics"]) exists(p["path"].asString(), "haptic");

    // A stick or pad left unbound can walk you off while you're placing a hand.
    for (const auto& key : src.getMemberNames()) {
      std::string kind = src[key]["type"].asString();
      if (kind != "joystick" && kind != "trackpad") continue;
      std::string name = after(key, "/input/");
      for (const char* hand : {"left", "right"})
        check(bound.byInput[std::string(hand) + "/input/" + name].count("hold") > 0, type + ": the game can still read " + hand + " " + name + " while placing");
    }

    // A grip with a force sensor reads as fully closed just from holding the controller, so it has to go by force.
    if (src["/input/grip"]["force"].asBool())
      for (const auto& s : set["sources"])
        if (s["path"].asString().find("/input/grip") != std::string::npos)
          check(s["parameters"]["force_input"].asString() == "force", type + ": holding the controller would count as a grab");
  }

  // A controller SteamVR ships that nothing here binds. These are left out on purpose: hands aren't controllers, and
  // knuckles_ev1 is a dev kit that was never sold. The rest would need SteamVR to guess at a mapping.
  const std::set<std::string> leftOut = {"knuckles_ev1", "svl_hand_interaction_augmented", "gamepad", "tundra_tracker", "vive_tracker"};
  for (const auto& [type, v] : profiles) {
    if (ours.count(type) || leftOut.count(type)) continue;
    if (!v["input_source"].isMember("/input/trigger")) continue;  // headsets, trackers
    check(false, type + ": SteamVR ships this controller and there's no binding for it");
  }

  // Every name resolves, and anything we've never heard of still gets a usable answer.
  check(std::string(buttonNames("some_future_controller").reset) == "A", "an unknown controller gets the usual names");
  check(std::string(buttonNames("").cancel) == "B", "no controller yet gets the usual names");

  printf("test_controllers: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}

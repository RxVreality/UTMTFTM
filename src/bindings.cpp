#include "bindings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "platform.h"

namespace fs = std::filesystem;

namespace fh {
namespace bind {

// ---------------------------------------------------------------- files
static bool readJson(const fs::path& path, Json::Value& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  Json::CharReaderBuilder b;
  std::string errs;
  return Json::parseFromStream(b, in, &out, &errs);
}
static bool readJson(const std::string& path, Json::Value& out) { return readJson(fs::u8path(path), out); }

static std::string pathText(const fs::path& p) { return p.generic_u8string(); }

// Only rewrites the file when something changed, so a game that launches with nothing new doesn't touch the disk.
static bool writeJson(const std::string& path, const Json::Value& v) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "   ";
  const std::string text = Json::writeString(w, v) + "\n";
  const fs::path p = fs::u8path(path);
  {
    std::ifstream in(p, std::ios::binary);
    std::stringstream old;
    old << in.rdbuf();
    if (in && old.str() == text) return true;
  }
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = fs::u8path(path + ".tmp");
  {
    std::ofstream o(tmp, std::ios::binary);
    if (!o) return false;
    o << text;
  }
  fs::rename(tmp, p, ec);
  return !ec;
}

static std::vector<fs::path> inputDirs(const std::string& steamvr) {
  std::vector<fs::path> dirs;
  const fs::path root = fs::u8path(steamvr);
  dirs.push_back(root / "resources" / "input");
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(root / "drivers", ec)) dirs.push_back(d.path() / "resources" / "input");
  return dirs;
}

// The name SteamVR knows a model by, or empty if it isn't on this machine. One that comes with a driver has to be asked
// for as {driver}name, and it won't load by its plain name unless that driver's running.
static std::string modelOnDisk(const std::string& steamvr, const std::string& name) {
  const fs::path root = fs::u8path(steamvr);
  std::error_code ec;
  if (fs::exists(root / "resources" / "rendermodels" / name / (name + ".json"), ec)) return name;
  for (const auto& d : fs::directory_iterator(root / "drivers", ec))
    if (fs::exists(d.path() / "resources" / "rendermodels" / name / (name + ".json"), ec)) return "{" + d.path().filename().u8string() + "}" + name;
  return fs::exists(root / "resources" / "rendermodels" / name / (name + ".obj"), ec) ? name : "";
}

// ---------------------------------------------------------------- what a controller has
static std::string after(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0 ? s.substr(prefix.size()) : ""; }

static std::string titleCase(std::string s) {
  bool start = true;
  for (char& ch : s) {
    if (ch == '_' || ch == '-') ch = ' ';
    if (start) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    start = ch == ' ';
  }
  return s;
}

static std::string inputName(const std::string& key) {
  static const std::map<std::string, std::string> names = {
      {"/input/a", "A"}, {"/input/b", "B"}, {"/input/x", "X"}, {"/input/y", "Y"},
      {"/input/dpad_up", "D-pad up"}, {"/input/dpad_down", "D-pad down"}, {"/input/dpad_left", "D-pad left"}, {"/input/dpad_right", "D-pad right"},
      {"/input/joystick", "Stick"}, {"/input/thumbstick", "Stick"}, {"/input/trackpad", "Trackpad"},
      {"/input/trigger", "Trigger"}, {"/input/grip", "Grip"}, {"/input/bumper", "Bumper"},
      {"/input/menu", "Menu"}, {"/input/view", "View"}, {"/input/application_menu", "Menu"}, {"/input/system", "System"},
      {"/input/pinch", "Pinch"}, {"/input/thumbrest", "Thumb rest"}};
  auto it = names.find(key);
  return it != names.end() ? it->second : titleCase(after(key, "/input/"));
}

const Input* Profile::find(const std::string& hand, const std::string& key) const {
  for (const Input& i : inputs)
    if (i.key == key && (i.side.empty() || i.side == hand)) return &i;
  return nullptr;
}

std::string Profile::rowOf(const std::string& path) const {
  const size_t slash = path.find('/');
  if (slash == std::string::npos) return "";
  const std::string hand = path.substr(0, slash), key = path.substr(slash);
  if (const Input* i = find(hand, key)) {
    if (i->shown || !(key == "/input/menu")) return path;
  }
  // Menu is /input/menu to OpenXR and /input/system in Touch's own description. The same button either way.
  if (key == "/input/menu" || key == "/input/application_menu" || key == "/input/system")
    for (const char* other : {"/input/menu", "/input/application_menu", "/input/view", "/input/system"}) {
      const Input* i = find(hand, other);
      if (i && i->shown) return hand + other;
    }
  return find(hand, key) ? path : "";
}

static Profile readProfile(const Json::Value& v, const fs::path& dir) {
  Profile p;
  p.type = v["controller_type"].asString();
  p.dir = pathText(dir);
  p.defaultBindings = v["default_bindings"];
  for (const auto& pair : v["mirror_mappings"]) {
    const std::string a = after(pair[0].asString(), "/user/hand/"), b = after(pair[1].asString(), "/user/hand/");
    p.mirror[a] = b;
    p.mirror[b] = a;
  }
  const Json::Value& src = v["input_source"];
  for (const auto& key : src.getMemberNames()) {
    const Json::Value& s = src[key];
    if (key.rfind("/pose/", 0) == 0) { p.poses.push_back(key); continue; }
    if (key.rfind("/input/", 0) != 0) continue;
    const std::string kind = s["type"].asString();
    if (kind == "skeleton") continue;
    Input i;
    i.key = key;
    i.name = inputName(key);
    i.kind = kind;
    i.side = s.get("side", "").asString();
    i.click = s.get("click", kind == "button").asBool();
    i.touch = s.get("touch", false).asBool();
    i.value = s.get("value", kind == "trigger").asBool();
    i.force = s.get("force", false).asBool();
    i.order = s.get("order", 50).asInt();
    const std::string vis = s.get("visibility", "").asString();
    const bool physical = kind == "joystick" || kind == "trackpad" || i.click || i.value;
    i.shown = physical && vis.empty() && kind != "pinch";
    i.touchOnly = !physical && i.touch && vis.empty();
    p.inputs.push_back(i);
  }
  // SteamVR keeps the system button for itself. On Touch and its relatives the one on the left is the game's menu button,
  // listed as system because the right one is. Everything else with a separate menu button doesn't need this.
  const bool hasMenu = [&] {
    for (const Input& i : p.inputs)
      if (i.key == "/input/menu" || i.key == "/input/view" || i.key == "/input/application_menu") return true;
    return false;
  }();
  for (Input& i : p.inputs)
    if (i.key == "/input/system") {
      if (!hasMenu && i.side == "left") { i.name = "Menu"; i.shown = true; }
      else i.shown = false;
    }
  std::stable_sort(p.inputs.begin(), p.inputs.end(), [](const Input& a, const Input& b) { return a.order < b.order; });
  return p;
}

const Profile* Library::profile(const std::string& type) const {
  auto it = profiles.find(type);
  return it == profiles.end() ? nullptr : &it->second;
}

void Library::load(const Dirs& d) {
  profiles.clear();
  remaps.clear();
  headsets.clear();
  std::error_code ec;
  for (const fs::path& dir : inputDirs(d.steamvr))
    for (const auto& f : fs::directory_iterator(dir, ec)) {
      if (f.path().extension() != ".json") continue;
      const std::string name = f.path().filename().string();
      if (name.find("profile") != std::string::npos) {
        Json::Value v;
        if (readJson(f.path(), v) && v.isMember("input_source") && v.isMember("controller_type")) {
          // A driver's own description beats a default in resources/input, and a stub is only a placeholder.
          const std::string type = v["controller_type"].asString();
          if (!profiles.count(type) || !v.get("is_stub", false).asBool()) profiles[type] = readProfile(v, dir);
        }
      } else if (name.find("remapping") != std::string::npos && name.find("remapping_") != 0) {
        Json::Value v;
        if (readJson(f.path(), v) && v.isMember("to_controller_type")) {
          const std::string type = v["to_controller_type"].asString();
          // Pico has one from its driver and one in resources/input. Both say much the same.
          if (!remaps.count(type)) remaps[type] = v;
        }
      }
    }
  const struct { const char* name; const char* type; const char* stem; const char* suffix[2]; } known[] = {
      {"Steam Frame", "frame_controller", "frame_controller", {"_left", "_right"}},
      {"Quest 3", "oculus_touch", "oculus_quest_plus_controller", {"_left", "_right"}},
      {"Quest Pro", "oculus_touch", "oculus_quest_pro_controller", {"_left", "_right"}},
      {"Quest 2", "oculus_touch", "oculus_quest2_controller", {"_left", "_right"}},
      {"Quest 1", "oculus_touch", "oculus_quest_controller", {"_left", "_right"}},
      {"Rift S", "oculus_touch", "oculus_rifts_controller", {"_left", "_right"}},
      {"Rift", "oculus_touch", "oculus_cv1_controller", {"_left", "_right"}},
      {"Index", "knuckles", "valve_controller_knu_1_0", {"_left", "_right"}},
      {"Vive", "vive_controller", "vr_controller_vive_1_5", {"", ""}},
      {"Pico 4", "pico_controller", "pico_4_controller", {"_left", "_right"}},
      {"Pico 4 Ultra", "pico_controller", "pico_4_ultra_controller", {"_left", "_right"}},
      {"Focus 3", "vive_focus3_controller", "vive_focus3_controller", {"_left", "_right"}},
  };
  for (const auto& k : known) {
    Headset h;
    h.name = k.name;
    h.type = k.type;
    h.model[0] = modelOnDisk(d.steamvr, std::string(k.stem) + k.suffix[0]);
    h.model[1] = modelOnDisk(d.steamvr, std::string(k.stem) + k.suffix[1]);
    if (profiles.count(h.type) && !h.model[0].empty() && !h.model[1].empty()) headsets.push_back(h);
  }
}

// ---------------------------------------------------------------- a game's bindings
std::string currentUrlKey(const std::string& controllerType, const std::string& category) {
  std::string suffix = category;
  if (suffix == "steamvr_input") suffix = "steamvrinput";  // the setting's spelling, not the binding file's
  return controllerType + "_250820_CurrentURL_" + suffix;
}

static std::string urlDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else out += s[i];
  }
  return out;
}

static std::string pathFromFileUrl(std::string url) {
  url = after(url, "file:///");
  if (url.empty()) return "";
#ifndef _WIN32
  url = "/" + url;
#endif
  return urlDecode(url);
}

// A manifest's names for its actions, in English if it has them.
static void readNames(const Json::Value& manifest, std::map<std::string, std::string>& names) {
  const Json::Value* best = nullptr;
  if (!manifest["localization"].isArray()) return;
  for (const auto& loc : manifest["localization"]) {
    if (!loc.isObject() || !loc["language_tag"].isString()) continue;
    std::string tag = loc["language_tag"].asString();
    for (char& ch : tag) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (tag == "en_us" || !best) best = &loc;
    if (tag == "en_us") break;
  }
  if (!best) return;
  for (const auto& k : best->getMemberNames()) {
    std::string lower = k;
    for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.rfind("/actions/", 0) == 0 && (*best)[k].isString()) names[lower] = (*best)[k].asString();
  }
}

// Where a SteamVR Input game's action file is: Steam lists them, and SteamVR's log says what each game asked for.
static std::string manifestPath(const Dirs& d, const std::vector<std::string>& keys) {
  for (const std::string& dir : d.config) {
    Json::Value m;
    if (!readJson(fs::u8path(dir) / "steamapps.vrmanifest", m) || !m.isObject() || !m["applications"].isArray()) continue;
    for (const auto& a : m["applications"])
      if (a.isObject() && a["app_key"].isString() && a["action_manifest_path"].isString() && std::find(keys.begin(), keys.end(), a["app_key"].asString()) != keys.end()) return a["action_manifest_path"].asString();
  }
  std::string found;
  for (const char* log : {"vrserver.previous.txt", "vrserver.txt"}) {
    std::ifstream in(fs::u8path(d.steam) / "logs" / log);
    std::string line;
    while (std::getline(in, line)) {
      const size_t at = line.find("SetActionManifestPath: Remembering call from ");
      if (at == std::string::npos) continue;
      for (const std::string& key : keys)
        if (!key.empty() && line.find("from " + key + ": ", at) != std::string::npos) found = pathFromFileUrl(line.substr(line.find(": file:///", at) + 2));
    }
  }
  return found;
}

// These come from games and from strangers on the Workshop, so they get looked over before anything relies on their shape.
static bool wellFormed(const Json::Value& v) {
  if (!v.isObject() || !v.isMember("bindings") || !v["bindings"].isObject()) return false;
  auto isPathed = [](const Json::Value& e) { return e.isObject() && e["path"].isString(); };
  for (const auto& setName : v["bindings"].getMemberNames()) {
    const Json::Value& set = v["bindings"][setName];
    if (!set.isObject()) return false;
    for (const char* list : {"sources", "poses", "haptics", "chords"})
      if (set.isMember(list) && !set[list].isArray()) return false;
    for (const auto& src : set["sources"]) {
      if (!isPathed(src) || !src["mode"].isString()) return false;
      if (src.isMember("parameters") && !src["parameters"].isObject()) return false;
      if (!src.isMember("inputs")) continue;
      if (!src["inputs"].isObject()) return false;
      for (const auto& in : src["inputs"].getMemberNames())
        if (!src["inputs"][in].isObject() || !src["inputs"][in]["output"].isString()) return false;
    }
    for (const auto& p : set["poses"]) if (!isPathed(p)) return false;
    for (const auto& chord : set["chords"]) {
      if (!chord.isObject() || !chord["inputs"].isArray()) return false;
      for (const auto& in : chord["inputs"]) if (!in.isArray() || !in[0].isString()) return false;
    }
  }
  return true;
}

static bool foundFile(const std::string& path, Found& out) {
  Json::Value v;
  if (!readJson(path, v) || !wellFormed(v)) return false;
  out.ok = true;
  out.json = v;
  out.path = path;
  out.category = v.get("category", "steamvr_input").asString();
  return true;
}

Found findBinding(const Dirs& d, const Library& lib, const Settings& s, const std::vector<std::string>& keys, const std::string& type) {
  Found out;
  std::map<std::string, std::string> names;
  const std::string manifest = manifestPath(d, keys);
  Json::Value m;
  const bool haveManifest = !manifest.empty() && readJson(manifest, m) && m.isObject();
  if (haveManifest) readNames(m, names);
  std::vector<std::string> onOff;
  if (haveManifest && m["actions"].isArray())
    for (const auto& a : m["actions"])
      if (a.isObject() && a["name"].isString() && a["type"].isString() && a["type"].asString() == "boolean") onOff.push_back(a["name"].asString());

  // What SteamVR is set to use for this game, unless that's our own file.
  const std::string ours = pathText(fs::u8path(d.data) / "bindings");
  for (const std::string& key : keys) {
    if (key.empty() || !s.get) continue;
    for (const char* cat : {"openxr", "steamvr_input", "legacy"}) {
      const std::string url = s.get(key, currentUrlKey(type, cat));
      if (url.empty()) continue;
      std::string path = pathFromFileUrl(url);
      const std::string workshop = after(url, "vr-input-workshop://");
      if (!workshop.empty()) {
        fs::file_time_type best{};
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(fs::u8path(d.steam) / "steamapps" / "workshop" / "content" / "250820" / workshop, ec))
          if (f.is_regular_file() && (path.empty() || f.last_write_time() > best)) { path = pathText(f.path()); best = f.last_write_time(); }
      }
      if (path.empty() || path.rfind(ours, 0) == 0) continue;
      if (foundFile(path, out)) { out.names = names; out.onOff = onOff; return out; }
    }
  }

  // What an OpenXR game asked for when it last ran.
  fs::file_time_type best{};
  std::string newest;
  for (const std::string& dir : d.config) {
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(fs::u8path(dir) / "openxr", ec)) {
      const std::string name = f.path().filename().string();
      for (const std::string& key : keys) {
        if (key.empty()) continue;
        const std::string rest = after(name, key + "_" + type + "_");
        if (rest.empty() || rest.size() < 13 || rest.compare(rest.size() - 13, 13, "_binding.json") != 0) continue;
        if (!std::all_of(rest.begin(), rest.end() - 13, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) continue;
        if (newest.empty() || f.last_write_time() > best) { newest = pathText(f.path()); best = f.last_write_time(); }
      }
    }
  }
  if (!newest.empty() && foundFile(newest, out)) { out.names = names; out.onOff = onOff; return out; }

  // SteamVR ships bindings of its own for some games.
  if (const Profile* p = lib.profile(type))
    for (const auto& db : p->defaultBindings)
      if (std::find(keys.begin(), keys.end(), db["app_key"].asString()) != keys.end() && foundFile(pathText(fs::u8path(p->dir) / fs::u8path(db["binding_url"].asString())), out)) {
        out.names = names;
        out.onOff = onOff;
        return out;
      }

  // The game's own action file lists one for each controller it knows.
  if (haveManifest && m["default_bindings"].isArray())
    for (const auto& db : m["default_bindings"])
      if (db.isObject() && db["controller_type"].isString() && db["binding_url"].isString() && db["controller_type"].asString() == type && foundFile(pathText(fs::u8path(manifest).parent_path() / fs::u8path(db["binding_url"].asString())), out)) {
        out.names = names;
        out.onOff = onOff;
        return out;
      }
  return out;
}

// ---------------------------------------------------------------- translating
static std::string swapHand(const std::string& path) {
  if (path.rfind("left/", 0) == 0) return "right/" + path.substr(5);
  if (path.rfind("right/", 0) == 0) return "left/" + path.substr(6);
  return "";
}

// The same input on the other hand, if this controller has one.
static std::string mirrored(const Profile& p, const std::string& path) {
  auto it = p.mirror.find(path);
  if (it != p.mirror.end()) return it->second;
  const std::string other = swapHand(path);
  return !other.empty() && !p.rowOf(other).empty() ? other : "";
}

static std::vector<std::string> strings(const Json::Value& v) {
  std::vector<std::string> out;
  if (v.isString()) out.push_back(v.asString());
  for (const auto& e : v) if (e.isString()) out.push_back(e.asString());
  return out;
}

using Forward = std::vector<std::pair<std::string, std::vector<std::string>>>;

static bool layoutFrom(const Json::Value& remap, const std::string& from, const Profile& theirs, const Profile& target, Forward& out) {
  for (const auto& layout : remap["layouts"]) {
    if (layout["from_controller_type"].asString() != from) continue;
    auto add = [&](const std::string& a, std::vector<std::string> to) {
      if (!a.empty() && !to.empty()) out.push_back({a, std::move(to)});
    };
    for (const auto& r : layout["autoremappings"]) {
      const std::string a = after(r["from"].asString(), "/user/hand/");
      std::vector<std::string> to;
      for (const std::string& t : strings(r["to"])) to.push_back(after(t, "/user/hand/"));
      if (r.get("mirror", true).asBool()) {
        std::vector<std::string> other;
        for (const std::string& t : to) {
          const std::string m = mirrored(target, t);
          if (!m.empty()) other.push_back(m);
        }
        const std::string ma = mirrored(theirs, a);
        if (!ma.empty()) add(ma, other);
      }
      add(a, to);
    }
    for (const auto& r : layout["remappings"])
      if (r["remapping_mode"].asString() == "replace" && !r["from"].isMember("mode"))
        add(after(r["from"]["path"].asString(), "/user/hand/"), {after(r["to"]["path"].asString(), "/user/hand/")});
    return true;
  }
  return false;
}

// Sticks, pads and menu buttons go by different names on different controllers.
static std::string sameJob(const Profile& p, const std::string& hand, const Input& i) {
  static const std::vector<std::vector<std::string>> families = {
      {"/input/thumbstick", "/input/joystick", "/input/trackpad"},
      {"/input/menu", "/input/application_menu", "/input/view"},
  };
  if (const Input* same = p.find(hand, i.key)) return hand + same->key;
  for (const auto& family : families)
    if (std::find(family.begin(), family.end(), i.key) != family.end())
      for (const std::string& k : family)
        if (p.find(hand, k)) return hand + k;
  return "";
}

Wiring defaultWiring(const Library& lib, const std::string& theirType, const std::string& yourType) {
  Wiring w;
  const Profile* theirs = lib.profile(theirType);
  const Profile* yours = lib.profile(yourType);
  if (!theirs || !yours) return w;
  if (theirType != yourType) {
    Forward f;
    auto mine = lib.remaps.find(yourType);
    if (mine != lib.remaps.end() && layoutFrom(mine->second, theirType, *theirs, *yours, f)) {
      for (const auto& [a, tos] : f) {
        const std::string row = theirs->rowOf(a);
        if (row.empty()) continue;
        for (const std::string& t : tos) {
          const std::string yr = yours->rowOf(t);
          if (!yr.empty() && !w.count(yr)) w[yr] = row;
        }
      }
    } else {
      // There's a table for the other direction, so run it backwards.
      auto other = lib.remaps.find(theirType);
      Forward back;
      if (other != lib.remaps.end() && layoutFrom(other->second, yourType, *yours, *theirs, back))
        for (const auto& [a, tos] : back) {
          const std::string yr = yours->rowOf(a);
          if (yr.empty() || w.count(yr)) continue;
          for (const std::string& t : tos) {
            const std::string row = theirs->rowOf(t);
            if (!row.empty()) { w[yr] = row; break; }
          }
        }
    }
  }
  for (const char* hand : {"left", "right"})
    for (const Input& i : yours->inputs) {
      if (!i.side.empty() && i.side != hand) continue;
      const std::string id = std::string(hand) + i.key;
      if (w.count(id) || (i.key == "/input/system" && !i.shown)) continue;
      const std::string match = theirType == yourType ? id : sameJob(*theirs, hand, i);
      if (!match.empty()) w[id] = theirs->rowOf(match).empty() ? match : theirs->rowOf(match);
    }
  return w;
}

static bool is2D(const Input& i) { return i.kind == "joystick" || i.kind == "trackpad"; }

// One source of their binding, as it has to be on one of your inputs. It can take more than one, or none if your input can't
// do it at all. The cases here follow what SteamVR's own translation writes.
static std::vector<Json::Value> adaptSource(const Json::Value& src, const std::string& path, const Input& to) {
  const std::string mode = src["mode"].asString();
  Json::Value out = src;
  out["path"] = "/user/hand/" + path;
  Json::Value& params = out["parameters"];
  if (!src.isMember("parameters")) out.removeMember("parameters");
  else if (params.get("force_input", "").asString() == "force" && !to.force) params.removeMember("force_input");

  Json::Value inputs(Json::objectValue);
  const Json::Value& from = src["inputs"];
  const auto names = from.getMemberNames();
  if (mode == "joystick" || mode == "trackpad" || mode == "dpad" || mode == "dpad_click" || mode == "dpad_touch" || mode == "scroll") {
    if (!is2D(to)) {
      // A stick can't become a button, but its click can.
      Json::Value button(Json::objectValue);
      for (const char* name : {"click", "touch"})
        if (from.isMember(name) && (std::string(name) == "click" ? to.click : to.touch)) button[name] = from[name];
      if (button.empty()) return {};
      out["mode"] = "button";
      out["inputs"] = button;
      return {out};
    }
    // A stick read as a trackpad stays one, the way SteamVR's own translation leaves it: either mode works on either kind.
    return {out};
  }
  if (mode == "trigger" || mode == "force_sensor" || mode == "scalar_constant") {
    const bool analog = to.value || to.force;
    if (analog) {
      if (mode == "force_sensor" && !to.force) {
        out["mode"] = "trigger";
        if (from.isMember("force")) { inputs["pull"] = from["force"]; out["inputs"] = inputs; }
      }
      std::vector<Json::Value> result = {out};
      // A trigger's click also gets said as a button press at a set pull, because not every controller clicks at the same point.
      if (mode == "trigger" && from.isMember("click")) {
        Json::Value press = out;
        Json::Value click(Json::objectValue);
        click["click"] = from["click"];
        press["mode"] = "button";
        press["inputs"] = click;
        press["parameters"]["click_activate_threshold"] = "0.7";
        press["parameters"]["click_deactivate_threshold"] = "0.65";
        press["parameters"]["haptic_amplitude"] = "0";
        result.push_back(press);
      }
      return result;
    }
    // A plain button can't report how far it's pressed, so that part reads as all the way in, and the rest stays a button.
    Json::Value analogPart(Json::objectValue), buttonPart(Json::objectValue);
    for (const auto& n : names) {
      if (n == "pull" || n == "force" || n == "value") analogPart["value"] = from[n];
      else if (n == "click" && to.click) buttonPart["click"] = from[n];
      else if (n == "touch" && to.touch) buttonPart["touch"] = from[n];
    }
    std::vector<Json::Value> result;
    if (!analogPart.empty()) {
      Json::Value full = out;
      full["mode"] = "scalar_constant";
      full["inputs"] = analogPart;
      result.push_back(full);
    }
    if (!buttonPart.empty()) {
      Json::Value press = out;
      press["mode"] = "button";
      press["inputs"] = buttonPart;
      result.push_back(press);
    }
    return result;
  }
  // Buttons, and anything else that's on or off.
  for (const auto& n : names) {
    if (n == "touch" && !to.touch) { if (to.click && !from.isMember("click")) inputs["click"] = from[n]; }
    else if (n == "click" && !to.click && !to.value && !is2D(to)) {}
    else inputs[n] = from[n];
  }
  if (inputs.empty() || mode == "none") return {};
  out["inputs"] = inputs;
  return {out};
}

// ---------------------------------------------------------------- one part of a control
std::string partOf(const std::string& id) {
  const size_t at = id.find('#');
  return at == std::string::npos ? "" : id.substr(at + 1);
}
std::string baseOf(const std::string& id) { return id.substr(0, id.find('#')); }
bool isDirection(const std::string& p) { return p == "north" || p == "east" || p == "south" || p == "west"; }

namespace {
// Something a part of their control sets off. kind 0: on or off. 1: a stick pushed all the way one way. 2: a full pull.
struct PartOut {
  int kind = 0;
  std::string output;
  int x = 0, y = 0;
  Json::Value params;
};
}  // namespace

// What their control, or one part of it, does in one set of a binding, as things one press of yours could set off.
static std::vector<PartOut> partOutputs(const Json::Value& set, const Profile& theirs, const std::string& their, const std::string& part, bool touch) {
  std::vector<PartOut> out;
  for (const auto& src : set["sources"]) {
    const std::string path = after(src["path"].asString(), "/user/hand/");
    if (path.empty()) continue;
    const std::string row = theirs.rowOf(path);
    if ((row.empty() ? path : row) != their) continue;
    const std::string mode = src["mode"].asString();
    const Json::Value& in = src["inputs"];
    auto put = [&](int kind, const std::string& name) {
      if (!in.isMember(name) || !in[name].isMember("output")) return false;
      PartOut o;
      o.kind = kind;
      o.output = in[name]["output"].asString();
      out.push_back(o);
      return true;
    };
    if (isDirection(part)) {
      // A game that reads the stick as four buttons has one for each way. One that reads it as a stick gets told it's
      // pushed that way, all the way.
      if (mode == "dpad" || mode == "dpad_click" || mode == "dpad_touch") put(0, part);
      else if (mode == "joystick" || mode == "trackpad") {
        const int x = part == "east" ? 1 : part == "west" ? -1 : 0, y = part == "north" ? 1 : part == "south" ? -1 : 0;
        if (in.isMember("position") && in["position"].isMember("output")) {
          PartOut o;
          o.kind = 1;
          o.output = in["position"]["output"].asString();
          o.x = x;
          o.y = y;
          out.push_back(o);
        }
        // Lots of OpenXR games read a stick as two numbers, across and up, each its own action. A number like that only
        // has the one value, which SteamVR's constant sets with its x.
        const char* axis = x ? "position_x" : "position_y";
        if (in.isMember(axis) && in[axis].isMember("output")) {
          PartOut o;
          o.kind = 1;
          o.output = in[axis]["output"].asString();
          o.x = x ? x : y;
          out.push_back(o);
        }
      }
    } else if (touch) {
      put(0, "touch");
    } else {
      if (mode == "button" || mode == "toggle_button" || mode == "joystick" || mode == "trackpad" || mode == "trigger") put(0, "click");
      if (mode == "grab") put(0, "grab");
      if (mode == "trigger") put(2, "pull");
      if (mode == "force_sensor") put(2, "force");
      if (mode == "scalar_constant" && put(2, "value") && src.isMember("parameters")) out.back().params = src["parameters"];
    }
  }
  // Touching yours can press theirs, when theirs has nothing for a touch.
  if (out.empty() && touch && part.empty()) return partOutputs(set, theirs, their, "click", false);
  return out;
}

// A part of yours with a job of its own doesn't do what the rest of that control does.
static void dropPart(Json::Value& sources, const std::string& mine, const std::string& part) {
  const std::string path = "/user/hand/" + baseOf(mine);
  Json::Value kept(Json::arrayValue);
  for (Json::Value s : sources) {
    if (s["path"].asString() == path && s.isMember("inputs")) {
      const std::string mode = s["mode"].asString();
      if (!isDirection(part) || mode == "dpad" || mode == "dpad_click" || mode == "dpad_touch") s["inputs"].removeMember(part);
      if (s["inputs"].empty()) continue;
    }
    kept.append(s);
  }
  sources = kept;
}

// Puts what a part route sets off on your control, the way SteamVR's own binding screen would: a d-pad for a stick pushed
// one way, a constant for a button that pushes a stick, a plain press otherwise. Returns how many it put there.
static int emitPart(Json::Value& sources, const std::string& mine, const Input& to, const std::vector<PartOut>& outs) {
  const std::string path = "/user/hand/" + baseOf(mine), part = partOf(mine);
  int n = 0;
  for (const PartOut& o : outs) {
    if (isDirection(part)) {
      if (o.kind != 0 || !is2D(to)) continue;  // a push can only press something
      // Into a d-pad already on this stick that goes off with a push, if there's one with that way free.
      Json::Value* pad = nullptr;
      for (Json::Value& s : sources)
        if (s["path"].asString() == path && s["mode"].asString() == "dpad" && !s["inputs"].isMember(part) &&
            s.get("parameters", Json::Value()).get("sub_mode", "touch").asString() == "touch") { pad = &s; break; }
      if (!pad) {
        Json::Value s;
        s["path"] = path;
        s["mode"] = "dpad";
        s["parameters"]["sub_mode"] = "touch";
        s["inputs"] = Json::objectValue;
        pad = &sources.append(s);
      }
      (*pad)["inputs"][part]["output"] = o.output;
      ++n;
      continue;
    }
    Json::Value s;
    s["path"] = path;
    if (o.kind == 0) {
      const char* name = part == "touch" ? (to.touch ? "touch" : nullptr) : (to.click || to.value || is2D(to)) ? "click" : to.touch ? "touch" : nullptr;
      if (!name) continue;
      s["mode"] = "button";
      s["inputs"][name]["output"] = o.output;
    } else {
      // Held down, it says the stick's pushed that way, or the trigger's pulled all the way. SteamVR only has that for a
      // button or a trigger, not a stick or a touch.
      if (part == "touch" || is2D(to) || (to.kind != "button" && to.kind != "trigger")) continue;
      s["mode"] = "scalar_constant";
      s["inputs"]["value"]["output"] = o.output;
      if (o.kind == 1) {
        s["parameters"]["on/x"] = std::to_string(o.x);
        s["parameters"]["on/y"] = std::to_string(o.y);
      } else if (o.params.isObject()) s["parameters"] = o.params;
    }
    sources.append(s);
    ++n;
  }
  return n;
}

Json::Value translate(const Json::Value& from, const Wiring& wiring, const Profile& theirs, const Profile& yours, std::map<std::string, int>* hits) {
  // Their input -> the ones of yours that act as it. Parts of controls go in afterwards, on top.
  std::map<std::string, std::vector<std::string>> onto;
  for (const auto& [mine, their] : wiring)
    if (!their.empty() && mine.find('#') == std::string::npos && their.find('#') == std::string::npos) onto[their].push_back(mine);
  auto yourInput = [&](const std::string& id) -> const Input* {
    const std::string base = baseOf(id);
    const size_t slash = base.find('/');
    return slash == std::string::npos ? nullptr : yours.find(base.substr(0, slash), base.substr(slash));
  };
  if (hits)
    for (const auto& [mine, their] : wiring)
      if (!their.empty() && (mine.find('#') != std::string::npos || their.find('#') != std::string::npos)) (*hits)[mine] = 0;
  // Anything the wiring doesn't mention stays where it is, if your controller has it at all. That's the right hand's
  // system button on most of them, which profiles only list on one side.
  std::map<std::string, std::vector<std::string>> same;
  auto targets = [&](const std::string& path) -> const std::vector<std::string>& {
    const std::string row = theirs.rowOf(path);
    auto it = onto.find(row.empty() ? path : row);
    if (it != onto.end()) return it->second;
    std::vector<std::string>& s = same[path];
    if (s.empty() && yourInput(path)) {
      // A button their controller doesn't have on that hand, like Budget Cuts 2's X and Y on a Quest's right hand: SteamVR
      // leaves it on your button of the same name, so this does too. Not if you've set that button to nothing, or to a part.
      auto w = wiring.find(path);
      if (w == wiring.end() || (row.empty() && !w->second.empty() && w->second.find('#') == std::string::npos)) s.push_back(path);
    }
    return s;
  };

  Json::Value out = from;
  Json::Value bindings(Json::objectValue);
  for (const auto& setName : from["bindings"].getMemberNames()) {
    const Json::Value& set = from["bindings"][setName];
    Json::Value nextSet = set;
    Json::Value sources(Json::arrayValue);
    for (const auto& src : set["sources"]) {
      const std::string path = after(src["path"].asString(), "/user/hand/");
      if (path.empty()) { sources.append(src); continue; }
      for (const std::string& mine : targets(path)) {
        const Input* to = yourInput(mine);
        if (!to) continue;
        for (const Json::Value& adapted : adaptSource(src, mine, *to)) sources.append(adapted);
      }
    }
    // Parts with jobs of their own: first they stop doing the rest of the control's, then they get theirs.
    for (const auto& [mine, their] : wiring)
      if (!partOf(mine).empty() && yourInput(mine)) dropPart(sources, mine, partOf(mine));
    for (const auto& [mine, their] : wiring) {
      if (their.empty() || (mine.find('#') == std::string::npos && their.find('#') == std::string::npos)) continue;
      const Input* to = yourInput(mine);
      if (!to) continue;
      // A touch of yours on the whole of theirs is a touch of theirs. Pick one part of theirs, and it's that part.
      const bool touch = partOf(their) == "touch" || (partOf(their).empty() && partOf(mine) == "touch");
      const int n = emitPart(sources, mine, *to, partOutputs(set, theirs, baseOf(their), partOf(their), touch));
      if (hits) (*hits)[mine] += n;
    }
    if (set.isMember("sources") || sources.size()) nextSet["sources"] = sources;

    // A pose your controller doesn't have falls back to its plain pose. Hand skeletons don't change.
    if (set.isMember("poses")) {
      Json::Value poses(Json::arrayValue);
      for (Json::Value p : set["poses"]) {
        const std::string path = p["path"].asString();
        const size_t at = path.find("/pose/");
        if (at != std::string::npos && !yours.poses.empty() && std::find(yours.poses.begin(), yours.poses.end(), path.substr(at)) == yours.poses.end())
          p["path"] = path.substr(0, at) + "/pose/raw";
        poses.append(p);
      }
      nextSet["poses"] = poses;
    }

    if (set.isMember("chords")) {
      Json::Value chords(Json::arrayValue);
      for (Json::Value chord : set["chords"]) {
        bool ok = true;
        for (Json::Value& in : chord["inputs"]) {
          const std::string path = after(in[0].asString(), "/user/hand/");
          const auto& t = targets(path);
          if (t.empty()) { ok = false; break; }
          in[0] = "/user/hand/" + t[0];
        }
        if (ok) chords.append(chord);
      }
      nextSet["chords"] = chords;
    }
    bindings[setName] = nextSet;
  }
  out["bindings"] = bindings;
  out["controller_type"] = yours.type;
  return out;
}

// ---------------------------------------------------------------- what an action is called
static std::string squeezed(const std::string& s) {
  std::string out;
  for (const char ch : s)
    if (std::isalnum(static_cast<unsigned char>(ch))) out += char(std::tolower(static_cast<unsigned char>(ch)));
  return out;
}

static bool endsWith(const std::string& s, const std::string& end) { return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0; }

static std::string spacedWords(const std::string& raw) {
  std::string s;
  for (size_t i = 0; i < raw.size(); ++i) {
    const char ch = raw[i];
    if (i > 0 && std::isupper(static_cast<unsigned char>(ch)) && std::islower(static_cast<unsigned char>(raw[i - 1]))) s += ' ';
    s += ch;
  }
  // "facebuttontop". The one word that's always run into the others.
  const size_t at = s.find("button");
  if (at != std::string::npos && s.find(' ') == std::string::npos) {
    if (at + 6 < s.size()) s.insert(at + 6, " ");
    if (at > 0) s.insert(at, " ");
  }
  return titleCase(s);
}

std::string actionLabel(const std::string& output, const std::map<std::string, std::string>& names, bool* passThrough, bool* hide) {
  bool pass = false, hidden = false;
  std::string lower = output;
  for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  std::string label;
  auto named = names.find(lower);
  const std::string raw = output.substr(output.rfind('/') == std::string::npos ? 0 : output.rfind('/') + 1);
  std::string n = raw;
  for (char& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  if (named != names.end() && !named->second.empty()) {
    label = named->second;
  } else {
    // Unity's OpenXR plugin and Unreal's name actions after the input, so they say nothing about what the game does.
    static const std::vector<std::string> tags = {"oculustouch", "framecontroller", "valveindex", "knuckles", "vivecontroller", "vive", "cosmos", "focus3", "pico", "wmr", "mixedreality", "hpmixedreality", "hand", "left_hand", "right_hand"};
    std::string rest = n;
    bool tagged = false;
    for (const std::string& t : tags)
      if (rest.rfind(t + "_", 0) == 0) { rest = rest.substr(t.size() + 1); tagged = true; break; }
    if (tagged) {
      if (rest.rfind("left_", 0) == 0) rest = rest.substr(5);
      else if (rest.rfind("right_", 0) == 0) rest = rest.substr(6);
    }
    const bool bare = rest.find('_') != std::string::npos || rest.find('-') != std::string::npos;
    for (const char* end : {"_touch", "-touch", "_ready", "-ready", "_force", "_proximity"})
      if (endsWith(rest, end)) hidden = true;
    for (const char* end : {"_up", "_down", "_left", "_right", "_y", "-y"})
      if ((tagged || bare) && endsWith(rest, end) && rest != "dpad_up" && rest != "dpad_down" && rest != "dpad_left" && rest != "dpad_right") hidden = true;
    std::string word = rest;
    for (const char* end : {"_click", "-click", "_value", "_axis", "_x", "-x", "_touch", "-touch", "_pull"})
      if (endsWith(word, end)) { word = word.substr(0, word.size() - std::string(end).size()); pass = true; break; }
    if (tagged) pass = true;
    static const std::map<std::string, std::string> unity = {
        {"primarybutton", "Primary"}, {"secondarybutton", "Secondary"}, {"grippressed", "Grip"}, {"grip", "Grip"},
        {"gripforce", ""}, {"trigger", "Trigger"}, {"triggerpressed", "Trigger"}, {"thumbstick", "Stick"},
        {"thumbstickclicked", "Stick click"}, {"menu", "Menu"}, {"squeeze", "Grip"}, {"primary2daxis", "Stick"},
        {"secondary2daxis", "Trackpad"}, {"select", "Select"}, {"ax", "A / X"}, {"by", "B / Y"}, {"stick", "Stick"}, {"thumbstick_click", "Stick click"},
        {"stick_clicks", "Stick click"}, {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"},
        {"dpadup", "D-pad up"}, {"dpaddown", "D-pad down"}, {"dpadleft", "D-pad left"}, {"dpadright", "D-pad right"}};
    // Other plugins' words for the same thing. They only say which button it came from, however much they look like names.
    static const std::map<std::string, std::string> buttonWords = {
        {"leftbuttona", "X"}, {"leftbuttonb", "Y"}, {"rightbuttona", "A"}, {"rightbuttonb", "B"}, {"leftgrip", "Grip"}, {"rightgrip", "Grip"},
        {"leftthumbstick", "Stick"}, {"rightthumbstick", "Stick"}, {"lthumb", "Stick"}, {"rthumb", "Stick"}, {"leftoculusmenu", "Menu"},
        {"lefttriggerclick", "Trigger"}, {"lefttriggerpull", "Trigger"}, {"righttriggerclick", "Trigger"}, {"righttriggerpull", "Trigger"}, {"ltrigger", "Trigger"}, {"rtrigger", "Trigger"},
        {"ax_button", "A / X"}, {"by_button", "B / Y"}, {"menu_button", "Menu"}, {"select_button", "Select"}, {"system", "System"}, {"click", "Click"},
        {"trackpad", "Trackpad"}, {"trackpadclicked", "Trackpad"}};
    auto plainWord = buttonWords.find(n);
    if (plainWord != buttonWords.end()) { word = n; pass = true; }
    if (n == "trackpadforce" || n.rfind("touch_", 0) == 0) hidden = true;
    auto u = unity.find(word);
    if (endsWith(n, "touched") || endsWith(n, "touch") || n == "gripforce" || endsWith(n, "ready")) hidden = true;
    if (u != unity.end()) { label = u->second; if (word != "primarybutton" && word != "secondarybutton") pass = true; }
    else if (plainWord != buttonWords.end()) label = plainWord->second;
    else label = spacedWords(word == n ? raw : word);
  }
  if (passThrough) *passThrough = pass;
  if (hide) *hide = hidden;
  return label;
}

static std::string lowered(std::string s) {
  for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// "/actions/OnFootControls_Right" -> "/actions/onfootcontrols", and how far from the plain set it is.
static std::string groupOf(const std::string& setName, int* variant = nullptr) {
  std::string g = lowered(setName);
  int v = 0;
  if (endsWith(g, "_right")) { g.resize(g.size() - 6); v = 1; }
  else if (endsWith(g, "_left")) { g.resize(g.size() - 5); v = 2; }
  if (variant) *variant = v;
  return g;
}

std::vector<ControlGroup> controlGroups(const Found& binding) {
  std::vector<ControlGroup> out;
  if (!binding.ok) return out;
  for (const auto& setName : binding.json["bindings"].getMemberNames()) {
    int labelled = 0;
    for (const auto& src : binding.json["bindings"][setName]["sources"])
      for (const auto& in : src["inputs"].getMemberNames()) {
        bool pass = false, hide = false;
        actionLabel(src["inputs"][in]["output"].asString(), binding.names, &pass, &hide);
        if (!hide && in != "touch") ++labelled;
      }
    if (!labelled) continue;
    const std::string id = groupOf(setName);
    auto it = std::find_if(out.begin(), out.end(), [&](const ControlGroup& g) { return g.id == id; });
    if (it == out.end()) {
      auto named = binding.names.find(id);
      out.push_back({id, named != binding.names.end() ? named->second : spacedWords(id.substr(id.rfind('/') + 1)), 0});
      it = out.end() - 1;
    }
    it->labelled += labelled;
  }
  return out;
}

// The set of controls to start on: the biggest that isn't for menus or the developers.
static std::string startingGroup(const std::vector<ControlGroup>& groups) {
  std::string best;
  int most = -1;
  for (const ControlGroup& g : groups) {
    bool aside = false;
    for (const char* word : {"menu", "frontend", "dev", "debug", "temporary", "photo", "map", "scroll"})
      aside = aside || g.id.find(word) != std::string::npos;
    if (!aside && g.labelled > most) { best = g.id; most = g.labelled; }
  }
  return best.empty() && !groups.empty() ? groups[0].id : best;
}

std::vector<RowText> rowsOn(const Profile& p, const char* hand, const Found* binding, const std::string& group) {
  std::vector<RowText> rows;
  std::map<std::string, std::vector<std::pair<int, std::string>>> labels;  // row -> (how much it matters, label)
  std::map<std::string, std::set<std::string>> named;                       // row -> the labels that are the game's own words
  std::map<std::string, bool> onlyPass;
  if (binding && binding->ok) {
    bool haveGroup = false;
    for (const auto& setName : binding->json["bindings"].getMemberNames()) haveGroup = haveGroup || groupOf(setName) == group;
    for (const auto& setName : binding->json["bindings"].getMemberNames()) {
      int variant = 0;
      if (haveGroup && groupOf(setName, &variant) != group) continue;
      for (const auto& src : binding->json["bindings"][setName]["sources"]) {
        const std::string path = after(src["path"].asString(), "/user/hand/");
        if (path.rfind(std::string(hand) + "/", 0) != 0) continue;
        const std::string row = p.rowOf(path);
        if (row.empty()) continue;
        const std::string mode = src["mode"].asString();
        if (mode == "dpad" || mode == "dpad_click" || mode == "dpad_touch" || mode == "none") {
          // A stick used as four buttons. It's bound, but the four names won't fit, and they aren't what matters here.
          bool pass = false, hide = false;
          std::string first;
          for (const auto& in : src["inputs"].getMemberNames()) {
            const std::string l = actionLabel(src["inputs"][in]["output"].asString(), binding->names, &pass, &hide);
            if (first.empty()) first = l;
          }
          if (!first.empty() && !hide && !pass) {
            labels[row].push_back({2 + 10 * variant, first});
            named[row].insert(first);
            onlyPass[row] = false;
          }
          continue;
        }
        for (const auto& in : src["inputs"].getMemberNames()) {
          bool pass = false, hide = false;
          const std::string l = actionLabel(src["inputs"][in]["output"].asString(), binding->names, &pass, &hide);
          if (in == "touch" || in == "ready_ext" || hide || l.empty()) continue;
          const int rank = in == "position" || in == "pull" || in == "force" || in == "position_x" ? 0 : in == "click" ? 1 : 3;
          labels[row].push_back({rank + 10 * variant, l});
          if (!onlyPass.count(row)) onlyPass[row] = true;
          if (!pass) {
            onlyPass[row] = false;
            named[row].insert(l);
          }
        }
      }
    }
  }
  for (const Input& i : p.inputs) {
    if (!i.shown || (!i.side.empty() && i.side != hand)) continue;
    RowText r;
    r.id = std::string(hand) + i.key;
    r.name = i.name;
    auto it = labels.find(r.id);
    if (it != labels.end()) {
      auto list = it->second;
      std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      std::vector<std::string> unique;
      for (const auto& e : list)
        if (std::find(unique.begin(), unique.end(), e.second) == unique.end()) unique.push_back(e.second);
      for (const std::string& u : unique)
        if (named[r.id].count(u)) r.actions.push_back(squeezed(u));
      r.bound = true;
      r.passThrough = onlyPass[r.id];
      r.action = r.passThrough && squeezed(unique[0]) == squeezed(i.name) ? "" : unique[0];
      if (!r.passThrough && unique.size() > 1) r.action += " / " + unique[1];
    }
    rows.push_back(r);
  }
  return rows;
}

// ---------------------------------------------------------------- what a guide says a game's buttons do
std::vector<GameGuide> loadGuides(const std::string& path) {
  std::vector<GameGuide> out;
  Json::Value v;
  if (path.empty() || !readJson(path, v)) return out;
  for (const Json::Value& e : v["games"]) {
    GameGuide g;
    g.game = e["game"].asString();
    g.from = e["from"].asString();
    g.overGamesWords = e["over_games_words"].asBool();
    for (const Json::Value& k : e["keys"]) g.keys.push_back(lowered(k.asString()));
    for (const Json::Value& n : e["names"]) g.names.push_back(n.asString());
    for (const std::string& type : e["on"].getMemberNames())
      for (const std::string& k : e["on"][type].getMemberNames()) g.on[type][k] = e["on"][type][k].asString();
    if (!g.game.empty() && !g.from.empty()) out.push_back(std::move(g));
  }
  return out;
}

// "Beat Saber (Rift)" and "Beat Saber" are the same game to anyone holding the controllers.
static std::string plainName(const std::string& name) {
  std::string out;
  bool inBrackets = false;
  for (char ch : name) {
    if (ch == '(') inBrackets = true;
    else if (ch == ')') inBrackets = false;
    else if (!inBrackets && std::isalnum(static_cast<unsigned char>(ch))) out += char(std::tolower(static_cast<unsigned char>(ch)));
  }
  return out;
}

const GameGuide* findGuide(const std::vector<GameGuide>& guides, const std::string& key, const std::string& alt, const std::string& name) {
  const std::string a = lowered(key), b = lowered(alt);
  for (const GameGuide& g : guides)
    if (std::find(g.keys.begin(), g.keys.end(), a) != g.keys.end() || (!b.empty() && std::find(g.keys.begin(), g.keys.end(), b) != g.keys.end())) return &g;
  const std::string plain = plainName(name);
  if (plain.empty()) return nullptr;
  for (const GameGuide& g : guides) {
    if (plainName(g.game) == plain) return &g;
    for (const std::string& n : g.names) if (plainName(n) == plain) return &g;
  }
  return nullptr;
}

// What a row says: pushing a stick and pressing it are separate actions, and the row has room for both.
static std::string rowWords(const std::map<std::string, std::string>& m, const std::string& row, bool clickOnly) {
  auto plain = m.find(row);
  auto press = m.find(row + "/click");
  if (clickOnly) return press != m.end() ? press->second : plain != m.end() ? plain->second : "";
  if (plain != m.end() && press != m.end() && !plain->second.empty() && !press->second.empty()) return plain->second + " / " + press->second;
  if (plain != m.end() && !plain->second.empty()) return plain->second;
  return press != m.end() ? press->second : "";
}

std::map<std::string, std::string> guideLabels(const GameGuide& g, const Profile& p, const Found* shown, const std::vector<GuideRoute>& routes, bool translated, bool clickOnly) {
  std::map<std::string, std::string> out;
  auto own = translated ? g.on.end() : g.on.find(p.type);

  // Which button each action is on, on the controllers the guide describes. Sensors don't count: VRChat has one thumb-touch
  // action on A, B and the stick, so it says nothing about which button is which. An action on two buttons doesn't prove
  // anything either, so it's left out.
  const auto isSensor = [](const std::string& in) { return in == "touch" || in == "ready_ext" || in == "force" || in == "proximity"; };
  struct Described { const std::map<std::string, std::string>* words; std::map<std::string, std::set<std::string>> rowsOf; };
  std::vector<Described> described;
  if (own == g.on.end())
    for (const GuideRoute& r : routes) {
      auto words = g.on.find(r.type);
      if (words == g.on.end() || !r.binding || !r.binding->ok || !r.profile) continue;
      Described d{&words->second, {}};
      for (const auto& setName : r.binding->json["bindings"].getMemberNames())
        for (const auto& src : r.binding->json["bindings"][setName]["sources"]) {
          const std::string path = after(src["path"].asString(), "/user/hand/");
          const std::string row = r.profile->rowOf(path);
          if (row.empty()) continue;
          for (const auto& in : src["inputs"].getMemberNames())
            if (!isSensor(in)) d.rowsOf[path.substr(0, path.find('/')) + "|" + lowered(src["inputs"][in]["output"].asString())].insert(row);
        }
      described.push_back(std::move(d));
    }

  for (const Input& i : p.inputs) {
    if (!i.shown) continue;
    for (const char* hand : {"left", "right"}) {
      if (!i.side.empty() && i.side != hand) continue;
      const std::string id = std::string(hand) + i.key;
      std::string words;
      if (own != g.on.end()) words = rowWords(own->second, id, clickOnly);
      else if (shown && shown->ok)
        for (const auto& setName : shown->json["bindings"].getMemberNames())
          for (const auto& src : shown->json["bindings"][setName]["sources"]) {
            if (!words.empty() || p.rowOf(after(src["path"].asString(), "/user/hand/")) != id) continue;
            for (const auto& in : src["inputs"].getMemberNames()) {
              if (isSensor(in)) continue;
              const std::string action = std::string(hand) + "|" + lowered(src["inputs"][in]["output"].asString());
              for (const Described& d : described) {
                auto rows = d.rowsOf.find(action);
                if (rows != d.rowsOf.end() && rows->second.size() == 1 && words.empty()) words = rowWords(*d.words, *rows->second.begin(), clickOnly);
              }
            }
          }
      if (!words.empty()) out[id] = words;
    }
  }
  return out;
}

std::vector<std::string> partsFor(const std::string& key) {
  static const std::map<std::string, std::vector<std::string>> table = {
      {"/input/a", {"button_a", "a_button", "button"}},
      {"/input/b", {"button_b", "b_button", "buttonB"}},
      {"/input/x", {"button_x", "x_button", "button"}},
      {"/input/y", {"button_y", "y_button", "buttonB"}},
      {"/input/trigger", {"trigger"}},
      {"/input/grip", {"button_grip", "grip", "squeeze", "rgrip", "lgrip"}},
      {"/input/joystick", {"thumbstick", "joystick"}},
      {"/input/thumbstick", {"thumbstick", "joystick"}},
      {"/input/trackpad", {"trackpad"}},
      {"/input/bumper", {"bumper"}},
      {"/input/menu", {"button_menu", "button_enter", "enter_button", "button"}},
      {"/input/view", {"button_view"}},
      {"/input/application_menu", {"button"}},
      {"/input/system", {"buttonC", "button_enter", "enter_button", "button_menu", "button_system", "sys_button", "home_button", "button"}},
      {"/input/dpad_up", {"dpad"}}, {"/input/dpad_down", {"dpad"}}, {"/input/dpad_left", {"dpad"}}, {"/input/dpad_right", {"dpad"}},
      {"/input/thumbrest", {"thumbrest"}},
  };
  auto it = table.find(key);
  return it == table.end() ? std::vector<std::string>{} : it->second;
}

// ---------------------------------------------------------------- combos
static const Input* inputAt(const Profile& p, const std::string& path) {
  const size_t slash = path.find('/');
  return slash == std::string::npos ? nullptr : p.find(path.substr(0, slash), path.substr(slash));
}

// Which set an action belongs to, as this binding spells it: "/actions/main/in/jump" goes in "/actions/Main".
static std::string setFor(const Json::Value& bindings, const std::string& output) {
  const size_t in = lowered(output).find("/in/");
  const std::string set = in == std::string::npos ? output : output.substr(0, in);
  for (const auto& name : bindings.getMemberNames())
    if (lowered(name) == lowered(set)) return name;
  return set;
}

static std::string sourceMode(const Input* i) {
  if (!i) return "button";
  if (i->kind == "joystick" || i->kind == "trackpad" || i->kind == "trigger") return i->kind;
  return "button";
}

// The d-pad on a stick that a combo pushes one way reads, one that goes off with a push rather than a click. With way, one
// with that way still free. Made if there isn't one, ahead of anything else on that stick, so that whichever way SteamVR
// looks through them it finds a d-pad.
static Json::Value& padFor(Json::Value& sources, const std::string& path, const std::string& way) {
  for (Json::Value& s : sources)
    if (s["path"].asString() == path && s["mode"].asString() == "dpad" && s.get("parameters", Json::Value()).get("sub_mode", "touch").asString() == "touch" &&
        (way.empty() || !s["inputs"].isMember(way)))
      return s;
  Json::Value pad;
  pad["path"] = path;
  pad["mode"] = "dpad";
  pad["parameters"]["sub_mode"] = "touch";
  pad["inputs"] = Json::objectValue;
  Json::Value out(Json::arrayValue);
  Json::ArrayIndex at = sources.size();
  for (Json::ArrayIndex i = 0; i < sources.size(); ++i) {
    if (at == sources.size() && sources[i]["path"].asString() == path) {
      at = out.size();
      out.append(pad);
    }
    out.append(sources[i]);
  }
  if (at == sources.size()) out.append(pad);
  sources = out;
  return sources[at];
}

void addCombos(Json::Value& binding, const std::vector<Combo>& combos, const Profile& yours) {
  if (!binding["bindings"].isObject()) binding["bindings"] = Json::objectValue;
  for (const Combo& combo : combos) {
    if (combo.inputs.empty() || combo.output.empty()) continue;
    Json::Value& set = binding["bindings"][setFor(binding["bindings"], combo.output)];
    if (!set["sources"].isArray()) set["sources"] = Json::arrayValue;
    if (combo.inputs.size() == 1) {
      const ComboInput& only = combo.inputs[0];
      if (isDirection(only.state)) {
        padFor(set["sources"], "/user/hand/" + only.path, only.state)["inputs"][only.state]["output"] = combo.output;
        continue;
      }
      Json::Value src;
      src["path"] = "/user/hand/" + only.path;
      src["mode"] = sourceMode(inputAt(yours, only.path));
      src["inputs"][only.state]["output"] = combo.output;
      set["sources"].append(src);
      continue;
    }
    Json::Value chord;
    chord["output"] = combo.output;
    chord["inputs"] = Json::arrayValue;
    for (const ComboInput& in : combo.inputs) {
      const std::string path = "/user/hand/" + in.path;
      Json::Value pair(Json::arrayValue);
      pair.append(path);
      pair.append(in.state);
      chord["inputs"].append(pair);
      // A push one way is something a d-pad has: SteamVR's binding screen only offers west, east, north, south and center
      // in a chord for a control in a d-pad mode, and SteamVR checks each control in a chord against its modes.
      if (isDirection(in.state)) {
        padFor(set["sources"], path, "");
        continue;
      }
      // Every chord in the files SteamVR and the Workshop keep is on controls that have a source in that set, even one with
      // nothing on it. SteamVR's own files have those too.
      bool has = false;
      for (const auto& src : set["sources"]) has = has || src["path"].asString() == path;
      if (!has) {
        Json::Value src;
        src["path"] = path;
        src["mode"] = sourceMode(inputAt(yours, in.path));
        src["inputs"] = Json::objectValue;
        set["sources"].append(src);
      }
    }
    if (!set["chords"].isArray()) set["chords"] = Json::arrayValue;
    set["chords"].append(chord);
  }
}

std::string comboText(const std::vector<ComboInput>& inputs, const Profile& yours, bool axes) {
  std::string out;
  for (const ComboInput& in : inputs) {
    const Input* i = inputAt(yours, in.path);
    if (!out.empty()) out += " + ";
    const bool push = isDirection(in.state);
    out += in.state == "touch" ? "Touch " : push ? "Push " : "Press ";
    if (i && i->side.empty()) out += in.path.rfind("left/", 0) == 0 ? "left " : "right ";
    out += i ? i->name : titleCase(in.path.substr(in.path.rfind('/') + 1));
    if (push) out += " " + wayName(in.state, axes);
  }
  return out;
}

std::vector<GameAction> comboActions(const Found& binding, const Profile& yours) {
  std::vector<GameAction> out;
  std::map<std::string, size_t> seen;
  std::set<std::string> onOff;
  for (const std::string& a : binding.onOff) onOff.insert(lowered(a));
  auto add = [&](const std::string& output, const std::string& onPath) {
    const std::string key = lowered(output);
    auto it = seen.find(key);
    if (it != seen.end()) return;
    bool pass = false, hide = false;
    const std::string label = actionLabel(output, binding.names, &pass, &hide);
    if (hide || label.empty()) return;
    const size_t in = key.find("/in/");
    if (in == std::string::npos) return;
    GameAction a;
    a.output = output;
    a.label = label;
    a.group = groupOf(output.substr(0, in));
    a.nameless = pass;
    if (!onPath.empty()) {
      const std::string row = yours.rowOf(onPath);
      const Input* i = inputAt(yours, row.empty() ? onPath : row);
      if (i && i->key == "/input/system" && !i->shown) return;
      if (i) a.where = std::string("On ") + (i->side.empty() ? (onPath.rfind("left/", 0) == 0 ? "left " : "right ") : "") + i->name;
      a.row = row.empty() ? onPath : row;
    }
    seen[key] = out.size();
    out.push_back(a);
  };
  if (binding.ok) {
    static const std::set<std::string> notOnOff = {"position", "pull", "force", "value", "scroll", "x", "y", "position_x", "position_y", "ready_ext", "axis"};
    for (const auto& setName : binding.json["bindings"].getMemberNames())
      for (const auto& src : binding.json["bindings"][setName]["sources"]) {
        const std::string path = after(src["path"].asString(), "/user/hand/");
        for (const auto& name : src["inputs"].getMemberNames()) {
          if (notOnOff.count(name)) continue;
          const std::string output = src["inputs"][name]["output"].asString();
          if (output.empty() || (!onOff.empty() && !onOff.count(lowered(output)))) continue;
          add(output, path);
        }
      }
  }
  for (const std::string& a : binding.onOff) add(a, "");
  return out;
}

// ---------------------------------------------------------------- the page
std::string Page::fileFor(const std::string& game, const std::string& yourType) const {
  std::string safe = game;
  for (char& ch : safe)
    if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '.' && ch != '_' && ch != '-') ch = '_';
  return pathText(fs::u8path(d_.data) / "bindings" / (safe + "_" + yourType + "_layout.json"));
}

std::string Page::shortName(const Headset& h) {
  // "Quest" is enough next to a button's name, and it's the same controller on all of them.
  if (h.type == "oculus_touch") return h.name == "Rift" || h.name == "Rift S" ? h.name : "Quest";
  if (h.type == "frame_controller") return "Frame";
  return h.name;
}

void Page::say(const std::string& text, int kind) {
  status = text;
  statusKind = kind;
  ++statusSerial;
}

void Page::loadPrefs() {
  Json::Value v;
  if (!readJson(fs::u8path(d_.data) / "bindings.json", v)) return;
  savedRef_ = v.get("reference", "").asString();
  defaults.clear();
  for (const auto& k : v["defaults"].getMemberNames()) defaults[k] = v["defaults"][k].asString();
  games.clear();
  for (const auto& k : v["games"].getMemberNames()) {
    const Json::Value& g = v["games"][k];
    GameChoice c;
    c.from = g.get("from", "").asString();
    c.explicitly = g.get("explicit", false).asBool();
    c.settingKey = g.get("setting_key", "").asString();
    c.originalUrl = g.get("original_url", "").asString();
    c.managed = g.get("managed", false).asBool();
    games[k] = c;
  }
  wiringChanges.clear();
  for (const auto& k : v["wiring"].getMemberNames()) {
    Wiring w;
    for (const auto& input : v["wiring"][k].getMemberNames()) w[input] = v["wiring"][k][input].asString();
    wiringChanges[k] = w;
  }
  combos.clear();
  for (const auto& k : v["combos"].getMemberNames())
    for (const auto& e : v["combos"][k]) {
      Combo c;
      c.output = e.get("output", "").asString();
      c.label = e.get("label", "").asString();
      for (const auto& pair : e["inputs"])
        if (pair.isArray() && pair.size() == 2 && pair[0].isString() && pair[1].isString()) c.inputs.push_back({pair[0].asString(), pair[1].asString()});
      if (!c.output.empty() && !c.inputs.empty()) combos[k].push_back(c);
    }
  macroTouches = v.get("macro_touches", false).asBool();
  macroInterview = v.get("macro_interview", false).asBool();
  stickAxes = v.get("stick_names", "compass").asString() == "axes";
  recorded.clear();
  for (const auto& k : v["recorded"].getMemberNames()) {
    const Profile* p = lib_.profile(k.substr(k.rfind('|') + 1));
    for (const auto& e : v["recorded"][k]) {
      macro::Recorded m;
      m.name = e.get("name", "").asString();
      m.hand = e.get("hand", -1).asInt();
      m.path = e.get("path", "").asString();
      m.onTouch = e.get("on_touch", false).asBool();
      m.repeat = e.get("repeat", false).asBool();
      m.asRecorded = e.get("as_recorded", false).asBool();
      m.withTouches = e.get("with_touches", false).asBool();
      for (const auto& s : e["steps"]) {
        if (!s.isArray() || s.size() != 6 || !s[1].isString()) continue;
        macro::Step step;
        step.hand = s[0].asInt();
        step.path = s[1].asString();
        step.kind = s[2].asInt();
        step.x = s[3].asInt();
        step.y = s[4].asInt();
        step.delayMs = s[5].asInt();
        m.steps.push_back(step);
      }
      // Sensors that were never controls got into takes made before the recorder learned to leave them out.
      if (p) m.steps = macro::onlyWhere(m.steps, [&](const macro::Step& s) { return p->find(s.hand ? "right" : "left", s.path) != nullptr; });
      if (!m.steps.empty()) recorded[k].push_back(m);
    }
  }
}

void Page::savePrefs() const {
  Json::Value v;
  v["version"] = 1;
  v["reference"] = savedRef_;
  v["defaults"] = Json::objectValue;
  for (const auto& [k, from] : defaults) v["defaults"][k] = from;
  v["games"] = Json::objectValue;
  for (const auto& [k, c] : games) {
    Json::Value g;
    g["from"] = c.from;
    g["explicit"] = c.explicitly;
    g["setting_key"] = c.settingKey;
    g["original_url"] = c.originalUrl;
    g["managed"] = c.managed;
    v["games"][k] = g;
  }
  v["wiring"] = Json::objectValue;
  for (const auto& [k, w] : wiringChanges) {
    v["wiring"][k] = Json::objectValue;
    for (const auto& [input, their] : w) v["wiring"][k][input] = their;
  }
  v["combos"] = Json::objectValue;
  for (const auto& [k, list] : combos) {
    if (list.empty()) continue;
    v["combos"][k] = Json::arrayValue;
    for (const Combo& c : list) {
      Json::Value e;
      e["output"] = c.output;
      e["label"] = c.label;
      e["inputs"] = Json::arrayValue;
      for (const ComboInput& in : c.inputs) {
        Json::Value pair(Json::arrayValue);
        pair.append(in.path);
        pair.append(in.state);
        e["inputs"].append(pair);
      }
      v["combos"][k].append(e);
    }
  }
  v["macro_touches"] = macroTouches;
  v["macro_interview"] = macroInterview;
  v["stick_names"] = stickAxes ? "axes" : "compass";
  v["recorded"] = Json::objectValue;
  for (const auto& [k, list] : recorded) {
    if (list.empty()) continue;
    v["recorded"][k] = Json::arrayValue;
    for (const macro::Recorded& m : list) {
      Json::Value e;
      e["name"] = m.name;
      e["hand"] = m.hand;
      e["path"] = m.path;
      e["on_touch"] = m.onTouch;
      e["repeat"] = m.repeat;
      e["as_recorded"] = m.asRecorded;
      e["with_touches"] = m.withTouches;
      e["steps"] = Json::arrayValue;
      for (const macro::Step& s : m.steps) {
        Json::Value step(Json::arrayValue);
        step.append(s.hand);
        step.append(s.path);
        step.append(s.kind);
        step.append(s.x);
        step.append(s.y);
        step.append(s.delayMs);
        e["steps"].append(step);
      }
      v["recorded"][k].append(e);
    }
  }
  writeJson(pathText(fs::u8path(d_.data) / "bindings.json"), v);
}

void Page::load() {
  lib_.load(d_);
  guides_ = loadGuides(d_.guides);
  loadPrefs();
  loaded_ = true;
  setReference(savedRef_);
}

static std::string withoutBraces(const std::string& model) {
  const size_t end = model.find('}');
  return !model.empty() && model[0] == '{' && end != std::string::npos ? model.substr(end + 1) : model;
}

const Headset* Page::byModel(const std::string& model) const {
  for (const Headset& h : lib_.headsets)
    if (withoutBraces(h.model[0]) == withoutBraces(model) || withoutBraces(h.model[1]) == withoutBraces(model)) return &h;
  return nullptr;
}

void Page::setYours(const std::string& type, const std::string& left, const std::string& right) {
  if (type == yours_.type && left == yours_.model[0] && right == yours_.model[1]) return;
  yours_ = Headset{};
  yours_.type = type;
  yours_.model[0] = left;
  yours_.model[1] = right;
  if (const Headset* h = byModel(left.empty() ? right : left)) yours_.name = h->name;
  else {
    for (const Headset& h : lib_.headsets) if (h.type == type) { yours_.name = h.name; break; }
    if (yours_.name.empty()) yours_.name = type.empty() ? "Your controllers" : titleCase(type);
  }
  if (loaded_ && (ref_.name.empty() || savedRef_.empty())) setReference(savedRef_);
  found_.clear();
  armed_.clear();
}

void Page::setReference(const std::string& name) {
  for (const Headset& h : lib_.headsets)
    if (h.name == name) { ref_ = h; armed_.clear(); return; }
  // Nothing saved: the first one that isn't what's in your hands.
  for (const Headset& h : lib_.headsets)
    if (h.type != yours_.type) { ref_ = h; return; }
  if (!lib_.headsets.empty()) ref_ = lib_.headsets[0];
}

void Page::setGame(const std::string& key, const std::string& alt, const std::string& name) {
  if (key == gameKey_ && alt == gameAlt_ && name == gameName_) return;
  gameKey_ = key;
  gameAlt_ = alt;
  gameName_ = name;
  found_.clear();
  armed_.clear();
  group_.clear();
  comboStep_ = 0;
  picked_.clear();
  comboGroup_.clear();
  comboPage_ = 0;
  recStep_ = 0;
  editing_ = macro::Recorded{};
  launchKnown_ = false;
}

const Found& Page::binding(const std::string& type) {
  auto it = found_.find(type);
  if (it != found_.end()) return it->second;
  Found f;
  if (!gameKey_.empty()) f = findBinding(d_, lib_, s_, {gameKey_, gameAlt_}, type);
  return found_[type] = f;
}

Wiring Page::wiring(const std::string& theirs, const std::string& yours) const {
  Wiring w = defaultWiring(lib_, theirs, yours);
  auto it = wiringChanges.find(theirs + ">" + yours);
  if (it != wiringChanges.end())
    for (const auto& [mine, their] : it->second) w[mine] = their;
  return w;
}

std::string Page::effectiveFrom() const {
  if (!gameKey_.empty()) {
    auto it = games.find(choiceKey());
    if (it != games.end() && it->second.explicitly) return it->second.from;
  }
  auto d = defaults.find(yours_.type);
  return d == defaults.end() ? "" : d->second;
}

static std::string encoded(const std::string& path) {
  std::string out;
  for (const char ch : path) {
    if (ch == ' ') out += "%20";
    else out += ch;
  }
  return out;
}

bool Page::applyNow(const std::string& from) {
  const Profile* theirs = lib_.profile(from);
  const Profile* mine = lib_.profile(yours_.type);
  if (gameKey_.empty() || !theirs || !mine) return false;
  const Found& f = binding(from);
  if (!f.ok) return false;
  // Your own controller, with nothing rewired: this is only here for its combos, so everything else stays exactly as it was.
  auto changed = wiringChanges.find(from + ">" + yours_.type);
  const bool asItWas = from == yours_.type && (changed == wiringChanges.end() || changed->second.empty());
  Json::Value out = asItWas ? f.json : translate(f.json, wiring(from, yours_.type), *theirs, *mine);
  std::string layoutName = titleCase(from);
  for (const Headset& h : lib_.headsets) if (h.type == from) { layoutName = shortName(h); break; }
  if (asItWas) out["name"] = f.json.get("name", "Binding").asString() + ", with combos from UTMTFTM";
  else out["name"] = from == yours_.type ? "Rewired by UTMTFTM" : layoutName + " layout, translated by UTMTFTM";
  out["description"] = "Made by UTMTFTM from this game's " + from + " binding. Switch it back on the UTMTFTM page.";
  addCombos(out, combosHere(), *mine);
  out["app_key"] = gameKey_;
  // A game written for SteamVR Input can be told it's holding the other controller too. An OpenXR game's binding already
  // says, in its interaction_profile, which is what SteamVR's own translation relies on.
  if (f.category != "openxr" && from != yours_.type) {
    bool type = true, model = true, hmd = true;
    auto table = lib_.remaps.find(yours_.type);
    if (table != lib_.remaps.end())
      for (const auto& layout : table->second["layouts"])
        if (layout["from_controller_type"].asString() == from) {
          type = layout.get("simulate_controller_type", true).asBool();
          model = layout.get("simulate_render_model", true).asBool();
          hmd = layout.get("simulate_HMD", true).asBool();
        }
    if (type) {
      out["options"]["simulated_controller_type"] = from;
      out["options"]["simulate_hmd"] = hmd;
      if (model) out["options"]["simulate_rendermodel"] = "name_only";
    }
  }
  const std::string file = fileFor(gameKey_, yours_.type);
  if (!writeJson(file, out)) return false;

  GameChoice& c = games[choiceKey()];
  const std::string key = currentUrlKey(yours_.type, f.category);
  if (!c.managed || c.settingKey != key) {
    if (c.managed && !c.settingKey.empty()) {  // it changed category: put the old setting back first
      if (c.originalUrl.empty()) s_.remove(gameKey_, c.settingKey);
      else s_.set(gameKey_, c.settingKey, c.originalUrl);
    }
    const std::string was = s_.get(gameKey_, key);
    if (pathFromFileUrl(was).rfind(pathText(fs::u8path(d_.data) / "bindings"), 0) != 0) c.originalUrl = was;
    c.settingKey = key;
  }
  if (!s_.set(gameKey_, key, "file:///" + encoded(file[0] == '/' ? file.substr(1) : file))) return false;
  c.managed = true;
  return true;
}

void Page::revertNow() {
  auto it = games.find(choiceKey());
  if (it == games.end() || !it->second.managed) return;
  GameChoice& c = it->second;
  if (c.originalUrl.empty()) s_.remove(gameKey_, c.settingKey);
  else s_.set(gameKey_, c.settingKey, c.originalUrl);
  c.managed = false;
  std::error_code ec;
  fs::remove(fs::u8path(fileFor(gameKey_, yours_.type)), ec);
}

bool Page::reapply() {
  if (gameKey_.empty()) return true;
  found_.clear();
  std::string from = effectiveFrom();
  if (!combosHere().empty() && (from.empty() || !binding(from).ok)) from = comboBase();
  if (from.empty()) { revertNow(); return true; }
  return !binding(from).ok || applyNow(from);
}

void Page::restoreAll() {
  loadPrefs();
  for (auto& [k, c] : games) {
    if (!c.managed) continue;
    const size_t bar = k.find('|');
    if (bar == std::string::npos) continue;
    const std::string game = k.substr(0, bar);
    if (c.originalUrl.empty()) s_.remove(game, c.settingKey);
    else s_.set(game, c.settingKey, c.originalUrl);
    c.managed = false;
    std::error_code ec;
    fs::remove(fs::u8path(fileFor(game, k.substr(bar + 1))), ec);
  }
  savePrefs();
}

void Page::gameStarted() {
  if (!loaded_) load();
  if (gameKey_.empty() || yours_.type.empty()) return;
  reapply();
  savePrefs();
  noteLaunch();
}

std::vector<std::string> Page::modelsWanted() const {
  std::vector<std::string> out;
  for (const Headset* h : {&ref_, &yours_})
    for (const std::string& m : h->model) {
      if (m.empty() || std::find(out.begin(), out.end(), m) != out.end()) continue;
      auto it = models_.find(m);
      if (it == models_.end() || !it->second.have) out.push_back(m);
    }
  return out;
}

void Page::give(const std::string& model, const Mesh& mesh, const std::vector<ModelPart>& parts) {
  Held& h = models_[model];
  h.mesh = mesh;
  h.parts = parts;
  h.have = true;
  h.pic.reset();
}

// A controller's picture, drawn once and kept. The mesh isn't needed after that.
void Page::bake(const std::string& model) {
  auto it = models_.find(model);
  if (it == models_.end() || !it->second.have || it->second.pic) return;
  Held& h = it->second;
  const bool left = model.find("left") != std::string::npos;
  h.pic = std::make_shared<ControllerPicture>(bakeController(h.mesh, h.parts, left, 184));
  h.mesh = Mesh{};
}

static const ModelPart* partNamed(const std::vector<ModelPart>& parts, const std::vector<std::string>& names) {
  for (const std::string& n : names)
    for (const ModelPart& p : parts)
      if (p.name == n) return &p;
  return nullptr;
}

const GameGuide* Page::guide() const {
  return gameKey_.empty() ? nullptr : findGuide(guides_, gameKey_, gameAlt_, gameName_);
}

static std::vector<std::string> splitWords(const std::string& words) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= words.size()) {
    const size_t next = words.find(" / ", at);
    out.push_back(words.substr(at, next == std::string::npos ? next : next - at));
    if (next == std::string::npos) break;
    at = next + 3;
  }
  return out;
}

// The controllers the guide describes, Touch first because that's what most sources describe.
std::vector<GuideRoute> Page::guideRoutes(const GameGuide& g) {
  std::vector<std::string> types;
  if (g.on.count("oculus_touch")) types.push_back("oculus_touch");
  for (const auto& [type, words] : g.on) if (type != "oculus_touch") types.push_back(type);
  std::vector<GuideRoute> routes;
  for (const std::string& type : types) routes.push_back({type, &binding(type), lib_.profile(type)});
  return routes;
}

// The game's own words win where it has any, and a button the game can't hear stays Not used whatever a guide says.
bool Page::guideOn(std::vector<RowText> rows[2], const Profile& p, const Found* shown) {
  const GameGuide* g = guide();
  if (!g) return false;
  const bool hasBinding = shown && shown->ok;
  const std::map<std::string, std::string> labels = guideLabels(*g, p, shown, guideRoutes(*g), hasBinding && !binding(p.type).ok);
  const bool gameNames = hasBinding && !shown->names.empty();
  bool any = false;
  for (int hand = 0; hand < 2; ++hand)
    for (RowText& r : rows[hand]) {
      if (gameNames && !g->overGamesWords && !r.actions.empty()) continue;
      auto it = labels.find(r.id);
      if (it == labels.end() || (hasBinding && !r.bound)) {
        // Next to real words, the button's own name again only gets in the way.
        if (!gameNames) { r.action.clear(); r.actions.clear(); }
        continue;
      }
      r.action = it->second;
      r.actions.clear();
      for (const std::string& w : splitWords(it->second)) r.actions.push_back(squeezed(w));
      r.passThrough = false;
      any = true;
    }
  return any;
}

std::vector<GameAction> Page::namedActions(const Found& active, const Profile& yours) {
  std::vector<GameAction> actions = comboActions(active, yours);
  const GameGuide* g = guide();
  if (!g) return actions;
  const std::map<std::string, std::string> words = guideLabels(*g, yours, &active, guideRoutes(*g), active.ok && !binding(yours.type).ok, true);
  // Unity's actions are shared between the hands, so one can be two things: Jump on the right, Scoreboard on the left.
  std::map<std::string, std::vector<std::string>> saidOn;
  if (active.ok)
    for (const auto& setName : active.json["bindings"].getMemberNames())
      for (const auto& src : active.json["bindings"][setName]["sources"]) {
        const std::string path = after(src["path"].asString(), "/user/hand/");
        const std::string row = yours.rowOf(path);
        auto said = words.find(row.empty() ? path : row);
        if (said == words.end()) continue;
        for (const auto& in : src["inputs"].getMemberNames()) {
          std::vector<std::string>& list = saidOn[lowered(src["inputs"][in]["output"].asString())];
          for (const std::string& w : splitWords(said->second))
            if (std::find(list.begin(), list.end(), w) == list.end()) list.push_back(w);
        }
      }
  for (GameAction& a : actions) {
    if (!active.names.empty() && !g->overGamesWords && !a.nameless) continue;
    auto it = saidOn.find(lowered(a.output));
    if (it == saidOn.end() || it->second.empty()) continue;
    a.label = it->second[0];
    if (it->second.size() > 1) a.label += " / " + it->second[1];
    a.nameless = false;
  }
  return actions;
}

void Page::fillRows(BindSide& side, const std::string& prefix, const Headset& h, const std::vector<RowText> rows[2]) {
  const Profile* prof = lib_.profile(h.type);
  for (int hand = 0; hand < 2; ++hand) {
    const std::string& model = h.model[hand];
    bake(model);
    auto it = models_.find(model);
    const Held* held = it != models_.end() && it->second.have ? &it->second : nullptr;
    side.pic[hand] = held ? held->pic.get() : nullptr;
    for (const RowText& r : rows[hand]) {
      BindRow br;
      br.id = prefix + r.id;
      br.name = r.name;
      br.action = r.action;
      br.unused = r.unused;
      if (held && held->pic && prof) {
        const size_t slash = r.id.find('/');
        const std::string key = r.id.substr(slash);
        const ModelPart* part = partNamed(held->parts, partsFor(key));
        if (part)
          if (const ControllerPicture::Spot* spot = held->pic->spot(part->name)) {
            br.x = spot->x;
            br.y = spot->y;
            br.behind = spot->behind;
          }
        // The Frame's D-pad is one part, so where its four directions sit comes from the buttons they mirror on the other hand.
        if (key.rfind("/input/dpad_", 0) == 0) {
          auto mir = prof->mirror.find(r.id);
          auto other = models_.find(h.model[1 - hand]);
          if (mir != prof->mirror.end() && other != models_.end()) {
            const ModelPart* twin = partNamed(other->second.parts, partsFor(mir->second.substr(mir->second.find('/'))));
            if (twin) held->pic->place({-twin->centre.x, twin->centre.y, twin->centre.z}, br.x, br.y, br.behind);
          }
        }
      }
      side.rows[hand].push_back(br);
    }
  }
}

BindModel Page::model(const std::string& hover, const std::string& down) {
  BindModel m;
  m.hover = hover;
  m.down = down;
  m.game = gameName_;
  for (const Headset& h : lib_.headsets) m.refs.push_back(h.name);
  m.ref = ref_.name;
  m.saveForGame = saveForGame_ && !gameKey_.empty();
  m.hasGame = !gameKey_.empty();
  m.armed = armed_.empty() ? "" : "y:" + baseOf(armed_);
  m.stickAxes = stickAxes;

  const Profile* yourP = lib_.profile(yours_.type);
  const Profile* theirP = lib_.profile(ref_.type);
  const std::string refShort = ref_.name.empty() ? "" : shortName(ref_);

  // Their side: the reference, with what this game has on it.
  std::vector<RowText> theirRows[2];
  bool theirGuide = false;
  const Found* theirBinding = nullptr;
  if (theirP && m.hasGame && binding(ref_.type).ok) theirBinding = &binding(ref_.type);
  // Which of the game's sets of controls is showing, when it has more than one.
  {
    const Found* basis = theirBinding ? theirBinding : (m.hasGame && yourP && binding(yours_.type).ok ? &binding(yours_.type) : nullptr);
    if (basis) {
      const std::vector<ControlGroup> groups = controlGroups(*basis);
      if (groups.size() > 1) {
        auto it = std::find_if(groups.begin(), groups.end(), [&](const ControlGroup& g) { return g.id == group_; });
        if (it == groups.end()) {
          group_ = startingGroup(groups);
          it = std::find_if(groups.begin(), groups.end(), [&](const ControlGroup& g) { return g.id == group_; });
        }
        if (it != groups.end()) m.controls = it->name;
      } else group_.clear();
    } else group_.clear();
  }
  if (theirP) {
    theirRows[0] = rowsOn(*theirP, "left", theirBinding, group_);
    theirRows[1] = rowsOn(*theirP, "right", theirBinding, group_);
    theirGuide = guideOn(theirRows, *theirP, theirBinding);
    for (auto& rows : theirRows)
      for (RowText& r : rows) r.unused = theirBinding && !r.bound;
  }
  m.theirs.title = ref_.name;
  bool anyNamed = false;
  for (const auto& rows : theirRows)
    for (const RowText& r : rows) anyNamed = anyNamed || !r.action.empty();
  if (!m.hasGame) m.theirs.note = "Where everything sits";
  else if (theirGuide) m.theirs.note = "What " + gameName_ + " does by default, from " + guide()->from;
  else if (!theirBinding) m.theirs.note = gameName_ + " hasn't set this one up";
  else if (anyNamed) m.theirs.note = "What " + gameName_ + " has on it";
  else m.theirs.note = gameName_ + " doesn't say what these do";

  // Your side: what it does now, or would do on the layout you're on.
  std::vector<RowText> yourRows[2];
  const std::string from = effectiveFrom();
  const bool onLayout = !from.empty();
  m.layoutOn = onLayout && from == ref_.type;
  m.layoutOther = onLayout && from != ref_.type;
  m.layoutName = ref_.type == yours_.type ? "Rewired" : refShort + " layout";
  const Wiring wired = theirP && yourP ? wiring(ref_.type, yours_.type) : Wiring{};
  auto wc = wiringChanges.find(ref_.type + ">" + yours_.type);
  m.wiringChanged = wc != wiringChanges.end() && !wc->second.empty();

  // What your input acts as on theirs, in words. Nothing at all if it's the same button: what's different is what matters.
  auto describe = [&](const RowText& mine, const std::string& their) -> RowText {
    RowText r;
    if (their.empty()) { r.unused = true; return r; }
    const std::string base = baseOf(their), part = partOf(their);
    for (const auto& rows : theirRows)
      for (const RowText& t : rows)
        if (t.id == base) {
          if (theirBinding && t.unused) { r.unused = true; return r; }
          const bool otherHand = base.substr(0, 5) != mine.id.substr(0, 5);
          if (!part.empty()) {
            // "Stick North", or "Left Stick Y+" when it's the stick in the other hand
            std::string hand = base.substr(0, base.find('/'));
            hand[0] = char(std::toupper(static_cast<unsigned char>(hand[0])));
            r.action = (otherHand ? hand + " " : "") + t.name + " " + directionName(part);
          } else if (!t.action.empty()) r.action = t.action;
          else if (squeezed(t.name) != squeezed(mine.name) || otherHand) r.action = refShort + " " + t.name;
          return r;
        }
    r.action = refShort + " " + titleCase(base.substr(base.rfind('/') + 1)) + (part.empty() ? "" : " " + directionName(part));
    return r;
  };
  auto actsAs = [&](const RowText& mine) -> RowText {
    auto it = wired.find(mine.id);
    RowText r = describe(mine, it == wired.end() ? "" : it->second);
    // Its parts with jobs of their own: "East: Quest B", "Touch: nothing".
    std::string parts;
    for (const char* part : {"north", "east", "south", "west", "click", "touch"}) {
      auto p = wired.find(mine.id + "#" + part);
      if (p == wired.end()) continue;
      const RowText d = describe(mine, p->second);
      std::string name = directionName(part);
      name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
      parts += (parts.empty() ? "" : ", ") + name + ": " + (d.unused ? "nothing" : d.action.empty() ? mine.name : d.action);
    }
    if (!parts.empty()) {
      r.action = r.unused ? parts : r.action.empty() ? parts : r.action + ", " + parts;
      r.unused = false;
    }
    return r;
  };

  if (yourP) {
    const bool showWiring = !m.hasGame || m.layoutOn;
    const Found* native = m.hasGame ? &binding(yours_.type) : nullptr;
    Found translated;
    // Nothing of its own for your controller: SteamVR translates from the best one it has, so show that.
    std::string translatedFrom;
    if (m.hasGame && !native->ok && !m.layoutOn) {
      translatedFrom = autoFrom();
      if (!translatedFrom.empty()) {
        translated = binding(translatedFrom);
        translated.json = translate(translated.json, defaultWiring(lib_, translatedFrom, yours_.type), *lib_.profile(translatedFrom), *yourP);
      }
    }
    const Found* shown = !translatedFrom.empty() ? &translated : native;
    for (int hand = 0; hand < 2; ++hand) {
      yourRows[hand] = rowsOn(*yourP, hand ? "right" : "left", showWiring ? nullptr : shown, group_);
    }
    const bool yourGuide = !showWiring && !m.layoutOther && guideOn(yourRows, *yourP, shown);
    for (int hand = 0; hand < 2; ++hand)
      for (RowText& r : yourRows[hand]) {
        if (showWiring) {
          const RowText a = actsAs(r);
          r.action = a.action;
          r.unused = a.unused;
        } else r.unused = shown && shown->ok && !r.bound;
      }
    if (!m.hasGame) m.yours.note = "How it translates to the " + ref_.name;
    else if (m.layoutOn) m.yours.note = ref_.type == yours_.type ? "Rewired by you" : "On the " + refShort + " layout";
    else if (m.layoutOther) {
      std::string otherName = titleCase(from);
      for (const Headset& h : lib_.headsets) if (h.type == from) { otherName = shortName(h); break; }
      m.yours.note = "On the " + otherName + " layout";
    }
    else if (native->ok) m.yours.note = "As the developer set it";
    else if (!translatedFrom.empty()) {
      std::string otherName = titleCase(translatedFrom);
      for (const Headset& h : lib_.headsets) if (h.type == translatedFrom) { otherName = h.name; break; }
      m.yours.note = "SteamVR is translating the game's " + otherName + " bindings";
    } else if (yourGuide) m.yours.note = "From " + guide()->from;
    else m.yours.note = "No bindings found yet. Play it once";
    if (m.hasGame && !combosHere().empty()) m.yours.note += ", plus " + std::to_string(combosHere().size()) + (combosHere().size() == 1 ? " combo" : " combos");
  } else {
    m.yours.note = yours_.type.empty() ? "Turn your controllers on" : "SteamVR has no description of this controller";
  }
  m.yours.title = yours_.name;

  fillRows(m.theirs, "t:", ref_, theirRows);
  fillRows(m.yours, "y:", yours_, yourRows);

  // Point at a button on one and its match lights up on the other. If the game says what its buttons do, the match is the
  // button that does the same thing, on the same hand if there's one there. Otherwise, or on a layout, where the wiring is
  // exactly what's happening, it's the button that's wired to it.
  auto sameAction = [&](const std::string& id, const std::vector<RowText>* from, const std::vector<RowText>* to, const char* prefix) {
    if (!m.hasGame || m.layoutOn) return false;
    const RowText* row = nullptr;
    for (int h = 0; h < 2; ++h)
      for (const RowText& r : from[h]) if (r.id == id) row = &r;
    if (!row) return false;
    const std::string hand = id.substr(0, id.find('/') + 1);
    for (const std::string& action : row->actions) {
      std::vector<std::string> near, far;
      for (int h = 0; h < 2; ++h)
        for (const RowText& r : to[h])
          if (std::find(r.actions.begin(), r.actions.end(), action) != r.actions.end()) (r.id.rfind(hand, 0) == 0 ? near : far).push_back(r.id);
      if (near.empty() && far.empty()) continue;
      for (const std::string& match : near.empty() ? far : near) m.lit.insert(prefix + match);
      return true;
    }
    return false;
  };
  if (hover.rfind("y:", 0) == 0) {
    m.lit.insert(hover);
    if (!sameAction(hover.substr(2), yourRows, theirRows, "t:"))
      for (const auto& [mine, their] : wired)  // all of it, and any parts with jobs of their own
        if (baseOf(mine) == hover.substr(2) && !their.empty()) m.lit.insert("t:" + baseOf(their));
  } else if (hover.rfind("t:", 0) == 0) {
    m.lit.insert(hover);
    if (!sameAction(hover.substr(2), theirRows, yourRows, "y:"))
      for (const auto& [mine, their] : wired)
        if (baseOf(their) == hover.substr(2)) m.lit.insert("y:" + baseOf(mine));
  }

  const bool changesAnything = ref_.type != yours_.type || m.wiringChanged;
  m.layoutOk = changesAnything && (!m.hasGame || theirBinding != nullptr);
  if (!armed_.empty() && yourP) {
    const std::string base = baseOf(armed_), part = partOf(armed_);
    const Input* i = yourP->find(base.substr(0, base.find('/')), base.substr(base.find('/')));
    m.armedName = i ? i->name : base;
    auto chip = [&](const std::string& id, const std::string& p, bool on) {
      std::string text = p.empty() ? "All of it" : directionName(p);
      text[0] = char(std::toupper(static_cast<unsigned char>(text[0])));
      m.chips.push_back({id, text, on});
      m.directions = m.directions || isDirection(p);
    };
    // Directions in the order each way of naming them reads: round the compass, or up and down the Y axis first.
    const std::vector<std::string> ways = stickAxes ? std::vector<std::string>{"north", "south", "east", "west"} : std::vector<std::string>{"north", "east", "south", "west"};
    const Profile* theirP2 = lib_.profile(ref_.type);
    const size_t ts = target_.find('/');
    const Input* t = theirP2 && ts != std::string::npos ? theirP2->find(target_.substr(0, ts), target_.substr(ts)) : nullptr;
    if (t) {
      // Their stick was tapped: which part of it.
      m.picking = true;
      m.target = "t:" + target_;
      std::string who = yourWords(armed_);
      m.prompt = "Pick what " + who + " does on " + theirWords(target_) + ", below";
      m.chipsLabel = "Which part";
      for (const std::string& w : ways) chip("bind_way_" + w, w, false);
      if (t->click) chip("bind_way_click", "click", false);
      if (t->touch) chip("bind_way_touch", "touch", false);
    } else if (i) {
      m.prompt = "Now tap what " + yourWords(armed_) + " should do, on the " + ref_.name + " above";
      if (is2D(*i) || i->touch) {
        m.chipsLabel = "Or one part";
        chip("bind_part_all", "", part.empty());
        if (is2D(*i))
          for (const std::string& w : ways) chip("bind_part_" + w, w, part == w);
        if (i->click || i->value) chip("bind_part_click", "click", part == "click");
        if (i->touch) chip("bind_part_touch", "touch", part == "touch");
      }
    }
  }
  m.status = status;
  m.statusWarn = statusKind == 2;
  m.statusGood = statusKind == 1;
  return m;
}

// ---------------------------------------------------------------- rewiring one part of a control
std::string wayName(const std::string& part, bool axes) {
  // J4, 9 November: some players think of a stick in compass points and some in axes, so it's their choice. Axes go the
  // way a graph does: Y+ is pushed away from you, X+ is to the right.
  static const std::map<std::string, std::pair<std::string, std::string>> names = {
      {"north", {"North", "Y+"}}, {"east", {"East", "X+"}}, {"south", {"South", "Y-"}}, {"west", {"West", "X-"}}, {"click", {"press", "press"}}, {"touch", {"touch", "touch"}}};
  auto it = names.find(part);
  if (it == names.end()) return part;
  return axes ? it->second.second : it->second.first;
}

std::string Page::directionName(const std::string& part) const { return wayName(part, stickAxes); }

// A control's name, with which hand when the controller has one on each.
std::string Page::handedName(const Profile& p, const std::string& id, bool always) const {
  const std::string base = baseOf(id);
  const size_t slash = base.find('/');
  if (slash == std::string::npos) return base;
  const std::string hand = base.substr(0, slash);
  const Input* i = p.find(hand, base.substr(slash));
  const std::string name = i ? i->name : titleCase(base.substr(base.rfind('/') + 1));
  return (always || !i || i->side.empty()) ? hand + " " + name : name;
}

std::string Page::yourWords(const std::string& id) const {
  const Profile* p = lib_.profile(yours_.type);
  const std::string who = p ? handedName(*p, id, false) : baseOf(id), part = partOf(id);
  if (isDirection(part)) return "pushing your " + who + " " + directionName(part);
  if (part == "click") return "pressing your " + who;
  if (part == "touch") return "touching your " + who;
  return "your " + who;
}

std::string Page::theirWords(const std::string& id) const {
  const Profile* p = lib_.profile(ref_.type);
  const std::string part = partOf(id);
  std::string out = "the " + shortName(ref_) + "'s " + (p ? handedName(*p, id, false) : baseOf(id));
  if (!part.empty()) out += " " + directionName(part);
  return out;
}

bool Page::wire(std::string mine, std::string their) {
  const std::string pair = ref_.type + ">" + yours_.type;
  Wiring& changes = wiringChanges[pair];
  const bool had = changes.count(mine);
  const std::string was = had ? changes[mine] : "";
  changes[mine] = their;
  armed_.clear();
  target_.clear();
  const bool parts = mine.find('#') != std::string::npos || their.find('#') != std::string::npos;
  // With the game's binding to hand, a part that wouldn't set anything off in it isn't kept. Better to say so now than
  // for it to quietly do nothing in the game.
  const Profile* theirP = lib_.profile(ref_.type);
  const Profile* mineP = lib_.profile(yours_.type);
  if (parts && !their.empty() && !gameKey_.empty() && theirP && mineP && binding(ref_.type).ok) {
    std::map<std::string, int> hits;
    translate(binding(ref_.type).json, wiring(ref_.type, yours_.type), *theirP, *mineP, &hits);
    if (!hits[mine]) {
      if (had) changes[mine] = was;
      else changes.erase(mine);
      if (changes.empty()) wiringChanges.erase(pair);
      bool usesIt = false;
      for (const auto& rows : {rowsOn(*theirP, "left", &binding(ref_.type)), rowsOn(*theirP, "right", &binding(ref_.type))})
        for (const RowText& r : rows) usesIt = usesIt || (r.id == baseOf(their) && r.bound);
      std::string why;
      if (!usesIt) why = gameName_ + " doesn't use that.";
      else if (isDirection(partOf(mine))) why = "A push of a stick can only press a button, and " + gameName_ + " reads that as a stick.";
      else why = gameName_ + "'s bindings have nothing there for it to do.";
      say("Not changed. " + why, 2);
      return false;
    }
  }
  if (!gameKey_.empty() && binding(ref_.type).ok) {
    GameChoice& c = games[choiceKey()];
    c.from = ref_.type;
    c.explicitly = true;
    applyNow(ref_.type);
  } else reapply();
  std::string what;
  if (!parts) what = !gameKey_.empty() && binding(ref_.type).ok ? "Rewired, and this game is on that layout now." : "Rewired.";
  else {
    std::string who = yourWords(mine);
    who[0] = char(std::toupper(static_cast<unsigned char>(who[0])));
    if (their.empty()) what = who + " now does nothing.";
    else what = who + (isDirection(partOf(their)) && !isDirection(partOf(mine)) ? " now pushes " : " now does ") + theirWords(their) + ".";
  }
  say(what + afterChange(), 1);
  savePrefs();
  return true;
}

void Page::clickGreyed(const std::string& id) {
  if (id == "bind_layout") {
    if (ref_.type == yours_.type && !wiringChanges.count(ref_.type + ">" + yours_.type)) say("You're comparing your controller with itself. Tap one of its buttons to rewire it.");
    else say(gameName_ + " has no " + ref_.name + " bindings to translate. If you've never played it here, play it once.");
  } else if (id == "bind_scope_game") say("Start a game to save a layout just for it.");
  else if (id == "bind_reset_wiring") say("Nothing's been rewired for the " + ref_.name + ".");
  else say("That isn't available right now.");
}

void Page::click(const std::string& id) {
  if (!gameKey_.empty() && !launchKnown_) noteLaunch();
  auto saved = [&](const std::string& what) {
    say(what + afterChange(), 1);
    savePrefs();
  };
  if (id.rfind("ref_", 0) == 0) {
    savedRef_ = id.substr(4);
    setReference(savedRef_);
    savePrefs();
    return;
  }
  if (id == "bind_scope_game") { saveForGame_ = true; say("Saving for this game only."); return; }
  if (id == "bind_scope_global") { saveForGame_ = false; say("Saving for all games."); return; }
  if (id == "bind_cancel") {
    if (!target_.empty()) target_.clear();  // back to choosing what it should do
    else armed_.clear();
    return;
  }
  if (id == "bind_dirs_compass" || id == "bind_dirs_axes") {
    stickAxes = id == "bind_dirs_axes";
    savePrefs();
    return;
  }
  if (id.rfind("bind_part_", 0) == 0 && !armed_.empty()) {  // one part of yours, or all of it again
    const std::string part = id.substr(10);
    armed_ = baseOf(armed_) + (part == "all" ? "" : "#" + part);
    target_.clear();
    return;
  }
  if (id.rfind("bind_way_", 0) == 0 && !armed_.empty() && !target_.empty()) {
    wire(armed_, target_ + "#" + id.substr(9));
    return;
  }
  if (id == "bind_controls_prev" || id == "bind_controls_next") {
    const Found& basis = binding(ref_.type).ok ? binding(ref_.type) : binding(yours_.type);
    const std::vector<ControlGroup> groups = controlGroups(basis);
    if (groups.empty()) return;
    size_t at = 0;
    for (size_t i = 0; i < groups.size(); ++i) if (groups[i].id == group_) at = i;
    at = (at + (id == "bind_controls_next" ? 1 : groups.size() - 1)) % groups.size();
    group_ = groups[at].id;
    return;
  }
  if (id == "bind_nothing" && !armed_.empty()) {
    if (!partOf(armed_).empty()) { wire(armed_, ""); return; }
    wiringChanges[ref_.type + ">" + yours_.type][armed_] = "";
    armed_.clear();
    target_.clear();
    reapply();
    saved("That button now does nothing.");
    return;
  }
  if (id == "bind_reset_wiring") {
    wiringChanges.erase(ref_.type + ">" + yours_.type);
    armed_.clear();
    reapply();
    saved("Back to the usual translation.");
    return;
  }
  if (id == "bind_dev") {
    if (saveForGame_ && !gameKey_.empty()) {
      GameChoice& c = games[choiceKey()];
      c.from = "";
      c.explicitly = true;
      reapply();
      saved(gameName_ + " is back to the developer's bindings.");
    } else {
      defaults.erase(yours_.type);
      if (!gameKey_.empty()) { games[choiceKey()].explicitly = false; reapply(); }
      saved("All games are back to the developer's bindings.");
    }
    return;
  }
  if (id == "bind_layout") {
    const std::string layout = ref_.type == yours_.type ? "Your rewiring" : "The " + shortName(ref_) + " layout";
    if (saveForGame_ && !gameKey_.empty()) {
      GameChoice& c = games[choiceKey()];
      c.from = ref_.type;
      c.explicitly = true;
      if (applyNow(ref_.type)) saved(layout + " is saved for " + gameName_ + ".");
      else say("Couldn't save that for " + gameName_ + ".", 2);
      savePrefs();
    } else {
      defaults[yours_.type] = ref_.type;
      if (!gameKey_.empty()) {
        games[choiceKey()].explicitly = false;
        applyNow(ref_.type);
      }
      saved(layout + " is the default for every game that has one.");
    }
    return;
  }
  if (id.rfind("y:", 0) == 0) {
    armed_ = baseOf(armed_) == id.substr(2) ? "" : id.substr(2);
    target_.clear();
    return;
  }
  if (id.rfind("t:", 0) == 0) {
    if (armed_.empty()) { say("To change what a button does, tap it on your own controller first."); return; }
    const std::string their = id.substr(2);
    // A stick of theirs has parts: which way to push it, or its press. Your whole stick on it is just a stick, though.
    const Profile* theirP = lib_.profile(ref_.type);
    const Profile* mineP = lib_.profile(yours_.type);
    const size_t ts = their.find('/'), ms = armed_.find('/');
    const Input* t = theirP && ts != std::string::npos ? theirP->find(their.substr(0, ts), their.substr(ts)) : nullptr;
    const Input* mi = mineP && ms != std::string::npos ? mineP->find(armed_.substr(0, ms), baseOf(armed_).substr(ms)) : nullptr;
    const bool wholeStick = partOf(armed_).empty() && mi && is2D(*mi);
    if (t && is2D(*t) && !wholeStick) {
      target_ = their;
      return;
    }
    wire(armed_, their);
    return;
  }
}

// ---------------------------------------------------------------- the combos page
const std::vector<Combo>& Page::combosHere() const {
  static const std::vector<Combo> none;
  auto it = combos.find(choiceKey());
  return it == combos.end() ? none : it->second;
}

std::string Page::autoFrom() {
  std::string from;
  auto table = lib_.remaps.find(yours_.type);
  int best = -1;
  if (table != lib_.remaps.end())
    for (const auto& layout : table->second["layouts"]) {
      const std::string type = layout["from_controller_type"].asString();
      const int pr = layout.get("priority", 0).asInt();
      if (pr > best && lib_.profile(type) && binding(type).ok) { best = pr; from = type; }
    }
  return from;
}

std::string Page::comboBase() {
  if (gameKey_.empty() || !lib_.profile(yours_.type)) return "";
  return binding(yours_.type).ok ? yours_.type : autoFrom();
}

Found Page::activeBinding() {
  std::string from = effectiveFrom();
  if (from.empty() || !binding(from).ok) from = comboBase();
  const Profile* mine = lib_.profile(yours_.type);
  if (from.empty() || !mine || !lib_.profile(from)) return Found{};
  Found f = binding(from);
  auto changed = wiringChanges.find(from + ">" + yours_.type);
  if (from != yours_.type || (changed != wiringChanges.end() && !changed->second.empty())) f.json = translate(f.json, wiring(from, yours_.type), *lib_.profile(from), *mine);
  return f;
}

// Combos people use, for whichever controller this is. Only what it really has makes the list.
std::vector<Page::ComboIdeaOf> Page::comboIdeas(const Profile& p) const {
  auto stick = [&](const char* hand) { return std::string(hand) + (p.find(hand, "/input/thumbstick") ? "/input/thumbstick" : p.find(hand, "/input/joystick") ? "/input/joystick" : "/input/trackpad"); };
  const std::vector<ComboIdeaOf> all = {
      {"Thumb on A + B", {{"right/input/a", "touch"}, {"right/input/b", "touch"}}},
      {"Both triggers", {{"left/input/trigger", "touch"}, {"right/input/trigger", "touch"}}},
      {"Stick + grip", {{stick("right"), "touch"}, {"right/input/grip", "click"}}},
      {"Both grips", {{"left/input/grip", "click"}, {"right/input/grip", "click"}}},
  };
  std::vector<ComboIdeaOf> out;
  for (const auto& idea : all) {
    bool ok = true;
    for (const ComboInput& in : idea.inputs) {
      const Input* i = inputAt(p, in.path);
      ok = ok && i && (in.state == "touch" ? i->touch : (i->click || i->value || i->force));
    }
    if (ok) out.push_back(idea);
  }
  return out;
}

ComboModel Page::comboModel(const std::string& hover, const std::string& down) {
  ComboModel m;
  m.hover = hover;
  m.down = down;
  m.game = gameName_;
  m.step = comboStep_;
  m.status = status;
  m.statusWarn = statusKind == 2;
  m.statusGood = statusKind == 1;
  const Profile* mine = lib_.profile(yours_.type);
  m.yours.title = yours_.name;
  if (gameKey_.empty()) m.why = "A combo sets off something in one particular game, so it needs that game's list of actions.";
  else if (!mine) m.why = yours_.type.empty() ? "Turn your controllers on." : "SteamVR has no description of this controller.";
  else if (comboBase().empty()) m.why = "UTMTFTM hasn't seen this game's controls yet. Play it once, then come back.";
  m.canMake = m.why.empty();
  if (!m.canMake) { comboStep_ = 0; m.step = 0; }
  if (!mine) return m;

  const Found active = m.canMake ? activeBinding() : Found{};
  const std::vector<ControlGroup> controlSets = active.ok ? controlGroups(active) : std::vector<ControlGroup>{};
  const std::string mainControls = controlSets.size() > 1 ? startingGroup(controlSets) : "";
  std::vector<RowText> rows[2];
  for (int hand = 0; hand < 2; ++hand) {
    const char* name = hand ? "right" : "left";
    rows[hand] = rowsOn(*mine, name, active.ok ? &active : nullptr, mainControls);
    for (const Input& i : mine->inputs)
      if (i.touchOnly && (i.side.empty() || i.side == name)) {
        RowText r;
        r.id = std::string(name) + i.key;
        r.name = i.name;
        rows[hand].push_back(r);
      }
  }
  fillRows(m.yours, "c:", yours_, rows);

  // What's picked, or the saved combo the laser's on, lit up on the controllers.
  std::map<std::string, std::string> showing = picked_;
  const std::vector<Combo>& saved = combosHere();
  if (comboStep_ == 0) {
    showing.clear();
    for (const char* prefix : {"combo_row_", "combo_del_"})
      if (hover.rfind(prefix, 0) == 0) {
        const size_t at = size_t(std::atoi(hover.c_str() + std::string(prefix).size()));
        if (at < saved.size()) for (const ComboInput& in : saved[at].inputs) showing[in.path] = in.state;
      }
  }
  for (int hand = 0; hand < 2; ++hand)
    for (BindRow& r : m.yours.rows[hand]) {
      auto it = showing.find(r.id.substr(2));
      if (it == showing.end()) continue;
      r.mark = it->second == "touch" ? 1 : 2;
      if (isDirection(it->second)) r.markText = r.name + " " + directionName(it->second);
    }

  m.stickAxes = stickAxes;
  for (const Combo& c : saved) m.saved.push_back({comboText(c.inputs, *mine, stickAxes), c.label});

  std::vector<ComboInput> picked;
  for (const auto& [path, state] : picked_) picked.push_back({path, state});
  m.picked = comboText(picked, *mine, stickAxes);

  if (comboStep_ == 1) {
    const auto ideas = comboIdeas(*mine);
    for (size_t i = 0; i < ideas.size(); ++i) m.ideas.push_back({"combo_idea_" + std::to_string(i), ideas[i].name});
    // Anything worth knowing about what's picked.
    for (const auto& [path, state] : picked_) {
      if (state == "click" || isDirection(state))
        for (int hand = 0; hand < 2; ++hand)
          for (const RowText& r : rows[hand])
            if (r.id == path && !r.action.empty() && m.note.empty())
              m.note = state == "click" ? "Pressing " + r.name + " also does " + r.action + " on its own, so this may set off both. Touch wouldn't."
                                        : "Pushing " + r.name + " " + directionName(state) + " also does " + r.action + " on its own, so this may do both.";
    }
    for (const auto& [path, state] : picked_)
      if (m.note.empty() && state == "touch" && path.find("/input/grip") != std::string::npos) m.note = "Just holding the controller usually counts as touching the grip, so Press is probably what you want there.";
    if (m.note.empty() && picked_.size() == 1 && picked_.begin()->second == "touch") m.note = "With only one touch, this goes off whenever your thumb or finger brushes it.";
    if (m.note.empty() && picked_.size() == 1 && isDirection(picked_.begin()->second)) m.note = "On its own, this goes off every time you push that way.";

    // The stick last tapped: touch it, press it, or push it one way.
    const Input* s = comboStick_.empty() ? nullptr : inputAt(*mine, comboStick_);
    if (s && is2D(*s)) {
      auto it = picked_.find(comboStick_);
      const std::string now = it == picked_.end() ? "" : it->second;
      m.chipsLabel = (s->side.empty() ? (comboStick_.rfind("left/", 0) == 0 ? "Left " : "Right ") : "") + s->name;
      if (s->touch) m.chips.push_back({"combo_part_touch", "Touch", now == "touch"});
      if (s->click || s->value || s->force) m.chips.push_back({"combo_part_click", "Press", now == "click"});
      // Round the compass, or up and down the Y axis first, as on the Bindings tab.
      const std::vector<std::string> ways = stickAxes ? std::vector<std::string>{"north", "south", "east", "west"} : std::vector<std::string>{"north", "east", "south", "west"};
      for (const std::string& way : ways) m.chips.push_back({"combo_part_" + way, directionName(way), now == way});
    }
  }
  if (comboStep_ == 2) {
    const std::vector<GameAction> actions = namedActions(active, *mine);
    std::vector<std::string> groups;
    for (const GameAction& a : actions)
      if (std::find(groups.begin(), groups.end(), a.group) == groups.end()) groups.push_back(a.group);
    if (std::find(groups.begin(), groups.end(), comboGroup_) == groups.end()) {
      std::vector<ControlGroup> known;
      for (const std::string& g : groups) {
        int count = 0;
        for (const GameAction& a : actions) if (a.group == g) ++count;
        known.push_back({g, g, count});
      }
      comboGroup_ = startingGroup(known);
      comboPage_ = 0;
    }
    if (groups.size() > 1) {
      auto named = active.names.find(comboGroup_);
      m.controls = named != active.names.end() ? named->second : spacedWords(comboGroup_.substr(comboGroup_.rfind('/') + 1));
    }
    std::vector<size_t> here;
    bool allNameless = !actions.empty();
    for (size_t i = 0; i < actions.size(); ++i) {
      if (actions[i].group == comboGroup_) here.push_back(i);
      allNameless = allNameless && actions[i].nameless;
    }
    if (allNameless) m.note = gameName_ + " only names its buttons, so pick the one you'd like this to act as.";
    const size_t perPage = 18;
    m.pages = std::max(1, int((here.size() + perPage - 1) / perPage));
    comboPage_ = std::min(comboPage_, m.pages - 1);
    m.pageNo = comboPage_;
    for (size_t i = comboPage_ * perPage; i < here.size() && i < (comboPage_ + 1) * perPage; ++i)
      m.choices.push_back({"combo_pick_" + std::to_string(here[i]), actions[here[i]].label, actions[here[i]].where});
  }
  return m;
}

void Page::comboClickGreyed(const std::string& id) {
  if (id == "combo_new") {
    if (gameKey_.empty()) say("Start a game first. A combo sets off something in one particular game.");
    else if (!lib_.profile(yours_.type)) say("Turn your controllers on first.");
    else say("UTMTFTM hasn't seen " + gameName_ + "'s controls yet. Play it once, then come back.");
  } else say("That isn't available right now.");
}

void Page::comboClick(const std::string& id) {
  if (!gameKey_.empty() && !launchKnown_) noteLaunch();
  const Profile* mine = lib_.profile(yours_.type);
  if (!mine) return;
  if (id == "combo_new") {
    if (combosHere().size() >= 6) { say("Six combos per game for now, because that's what fits. Remove one first."); return; }
    picked_.clear();
    comboStick_.clear();
    comboStep_ = 1;
    return;
  }
  if (id == "combo_cancel") { picked_.clear(); comboStick_.clear(); comboStep_ = 0; return; }
  if (id == "combo_dirs_compass" || id == "combo_dirs_axes") {  // the same switch as the Bindings tab's
    stickAxes = id == "combo_dirs_axes";
    savePrefs();
    return;
  }
  if (id.rfind("combo_part_", 0) == 0) {  // one part of the stick last tapped. Again turns it off
    if (comboStep_ != 1 || comboStick_.empty()) return;
    const std::string part = id.substr(11);
    const Input* s = inputAt(*mine, comboStick_);
    const bool has = s && (isDirection(part) ? is2D(*s) : part == "touch" ? s->touch : part == "click" && (s->click || s->value || s->force));
    if (!has) return;
    auto it = picked_.find(comboStick_);
    if (it != picked_.end() && it->second == part) picked_.erase(it);
    else picked_[comboStick_] = part;
    return;
  }
  if (id == "combo_next") { if (!picked_.empty()) { comboStep_ = 2; comboPage_ = 0; } return; }
  if (id == "combo_back") { comboStep_ = 1; return; }
  if (id.rfind("c:", 0) == 0) {
    // Tapping a control on the list of combos just starts a new one with it.
    if (comboStep_ == 0) {
      if (comboBase().empty()) { comboClickGreyed("combo_new"); return; }
      if (combosHere().size() >= 6) { say("Six combos per game for now, because that's what fits. Remove one first."); return; }
      picked_.clear();
      comboStick_.clear();
      comboStep_ = 1;
    }
    if (comboStep_ != 1) return;
    // Touch first, because that's the point of this page. Then press, then off. A control can only do what it has. A stick
    // or pad also offers its directions underneath, so a push one way is one tap away rather than a long way round.
    const std::string path = id.substr(2);
    const Input* i = inputAt(*mine, path);
    if (!i) return;
    if (is2D(*i)) comboStick_ = path;
    const bool canPress = i->click || i->value || i->force;
    auto it = picked_.find(path);
    if (it == picked_.end()) picked_[path] = i->touch ? "touch" : "click";
    else if (it->second == "touch" && canPress) it->second = "click";
    else picked_.erase(it);
    return;
  }
  if (id.rfind("combo_idea_", 0) == 0) {
    const auto ideas = comboIdeas(*mine);
    const size_t at = size_t(std::atoi(id.c_str() + 11));
    if (at >= ideas.size()) return;
    picked_.clear();
    comboStick_.clear();
    for (const ComboInput& in : ideas[at].inputs) {
      picked_[in.path] = in.state;
      const Input* i = inputAt(*mine, in.path);
      if (i && is2D(*i)) comboStick_ = in.path;
    }
    return;
  }
  if (id == "combo_page_next") { ++comboPage_; return; }
  if (id == "combo_page_prev") { comboPage_ = std::max(0, comboPage_ - 1); return; }
  if (id == "combo_group_next" || id == "combo_group_prev") {
    const std::vector<GameAction> actions = namedActions(activeBinding(), *mine);
    std::vector<std::string> groups;
    for (const GameAction& a : actions)
      if (std::find(groups.begin(), groups.end(), a.group) == groups.end()) groups.push_back(a.group);
    if (groups.empty()) return;
    size_t at = 0;
    for (size_t i = 0; i < groups.size(); ++i) if (groups[i] == comboGroup_) at = i;
    comboGroup_ = groups[(at + (id == "combo_group_next" ? 1 : groups.size() - 1)) % groups.size()];
    comboPage_ = 0;
    return;
  }
  auto saved = [&](const std::string& what, bool ok) {
    if (ok) say(what + afterChange(), 1);
    else say("Couldn't write " + gameName_ + "'s binding file. It's in the log.", 2);
    savePrefs();
  };
  if (id.rfind("combo_pick_", 0) == 0) {
    const std::vector<GameAction> actions = namedActions(activeBinding(), *mine);
    const size_t at = size_t(std::atoi(id.c_str() + 11));
    if (at >= actions.size() || picked_.empty()) return;
    Combo c;
    for (const auto& [path, state] : picked_) c.inputs.push_back({path, state});
    c.output = actions[at].output;
    c.label = actions[at].label;
    // The same controls can only mean one thing, so doing it again replaces the old one.
    std::vector<Combo>& list = combos[choiceKey()];
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Combo& o) {
      if (o.inputs.size() != c.inputs.size()) return false;
      for (size_t i = 0; i < c.inputs.size(); ++i) if (o.inputs[i].path != c.inputs[i].path || o.inputs[i].state != c.inputs[i].state) return false;
      return true;
    }), list.end());
    const std::string what = comboText(c.inputs, *mine, stickAxes) + " does " + c.label + ".";
    list.push_back(c);
    picked_.clear();
    comboStick_.clear();
    comboStep_ = 0;
    saved(what, reapply());
    return;
  }
  if (id.rfind("combo_del_", 0) == 0) {
    std::vector<Combo>& list = combos[choiceKey()];
    const size_t at = size_t(std::atoi(id.c_str() + 10));
    if (at >= list.size()) return;
    list.erase(list.begin() + std::ptrdiff_t(at));
    if (list.empty()) combos.erase(choiceKey());
    saved("Removed.", reapply());
    return;
  }
}

// ---------------------------------------------------------------- changes that can work right away
void Page::noteLaunch() {
  launchKnown_ = true;
  launchFrom_ = effectiveFrom();
  launchWiring_ = launchFrom_.empty() ? Wiring{} : wiring(launchFrom_, yours_.type);
  launchCombos_ = combosHere();
  const Found f = activeBinding();
  launchJsonOk_ = f.ok;
  launchJson_ = f.ok ? f.json : Json::Value();
}

namespace {
// What one part of one of your controls sets off in a binding, as "hand|set|action|kind". Kinds: b on or off, t a toggle, g a grab,
// a how far, p a stick's movement, px and py one number of it, k<x>,<y> a set amount while pressed, x anything else.
using PartKey = std::pair<std::string, std::string>;  // "right/input/a", "click"
using Parts = std::map<PartKey, std::set<std::string>>;
}  // namespace

static Parts partsIn(const Json::Value& binding) {
  Parts out;
  if (!binding.isObject() || !binding["bindings"].isObject()) return out;
  for (const auto& setName : binding["bindings"].getMemberNames()) {
    const Json::Value& set = binding["bindings"][setName];
    if (!set.isMember("sources")) continue;
    for (const auto& src : set["sources"]) {
      const std::string path = after(src["path"].asString(), "/user/hand/");
      if (path.empty() || !src["inputs"].isObject()) continue;
      const std::string mode = src["mode"].asString();
      const Json::Value params = src.get("parameters", Json::Value(Json::objectValue));
      const Json::Value& in = src["inputs"];
      for (const auto& name : in.getMemberNames()) {
        if (!in[name].isObject() || !in[name].isMember("output")) continue;
        std::string part, kind = "b";
        if (mode == "button") { if (name == "click" || name == "touch") part = name; }
        else if (mode == "toggle_button") { if (name == "click" || name == "touch") { part = name; kind = "t"; } }
        else if (mode == "grab") { if (name == "grab") { part = "value"; kind = "g"; } }
        else if (mode == "trigger") {
          if (name == "click" || name == "touch") part = name;
          else if (name == "pull") { part = "value"; kind = "a"; }
        } else if (mode == "force_sensor") {
          if (name == "force") { part = "value"; kind = "a"; }
        } else if (mode == "scalar_constant") {
          if (name == "value") {
            part = "click";
            kind = "k" + params.get("on/x", "1").asString() + "," + params.get("on/y", "0").asString();
          }
        } else if (mode == "joystick" || mode == "trackpad") {
          if (name == "click" || name == "touch") part = name;
          else if (name == "position") { part = "move"; kind = "p"; }
          else if (name == "position_x") { part = "move"; kind = "px"; }
          else if (name == "position_y") { part = "move"; kind = "py"; }
        } else if (mode == "dpad" || mode == "dpad_click" || mode == "dpad_touch") {
          if (isDirection(name)) {
            part = name;
            if (mode == "dpad_click" || (mode == "dpad" && params.get("sub_mode", "touch").asString() == "click")) kind = "bc";
          }
        }
        if (part.empty()) { part = "?" + mode + "/" + name; kind = "x"; }
        // Which hand is part of the job: an OpenXR game asks for one action once per hand, so the same name on the left
        // stick and the right one are two different things to it.
        out[{path, part}].insert(path.substr(0, path.find('/')) + "|" + setName + "|" + lowered(in[name]["output"].asString()) + "|" + kind);
      }
    }
  }
  return out;
}

static std::string kindOf(const std::string& e) { return e.substr(e.rfind('|') + 1); }
static std::string jobOf(const std::string& e) { return e.substr(0, e.rfind('|')); }

// A part that did `had` at launch, doing `wanted` now: which part of it the driver works, and how far. False if it can't.
static bool reachFor(const std::string& had, const std::string& wanted, const std::string& carrierPart, std::string& toPart, float& amount) {
  amount = 1;
  if (had == wanted && had != "x") {
    toPart = carrierPart;
    return had != "bc";  // a stick pushed one way and pressed in: the driver only pushes
  }
  if (wanted.size() < 2 || wanted[0] != 'k') return false;
  const size_t comma = wanted.find(',');
  const double x = std::atof(wanted.substr(1, comma == std::string::npos ? std::string::npos : comma - 1).c_str());
  const double y = comma == std::string::npos ? 0 : std::atof(wanted.substr(comma + 1).c_str());
  auto way = [&](double across, double up) -> bool {
    if (across == 0 && up == 0) return false;
    if (across != 0 && up != 0) return false;
    toPart = up > 0 ? "north" : up < 0 ? "south" : across > 0 ? "east" : "west";
    return true;
  };
  if (had == "p") return way(x, y);
  if (had == "px") return way(x, 0);
  if (had == "py") return way(0, x);  // a constant on one number carries it in its x
  if (had == "a") { toPart = "value"; amount = float(x); return x > 0; }
  return false;
}

// Which part of yours can be worked to do which.
static bool sourceSuits(const std::string& part, const std::string& toPart, float amount) {
  if (part == "move" || toPart == "move") return part == toPart;
  if (part == "value") return toPart == "value" && amount == 1;
  return part.empty() || part[0] != '?';
}

std::vector<LiveLink> liveLinks(const Json::Value& launched, const Json::Value& now, bool* allOfIt) {
  bool all = true;
  std::vector<LiveLink> out;
  const Parts was = partsIn(launched), is = partsIn(now);
  std::set<PartKey> keys;
  for (const auto& kv : was) keys.insert(kv.first);
  for (const auto& kv : is) keys.insert(kv.first);
  static const std::set<std::string> none;
  for (const PartKey& key : keys) {
    auto w = was.find(key), n = is.find(key);
    const std::set<std::string>& before = w == was.end() ? none : w->second;
    const std::set<std::string>& wanted = n == is.end() ? none : n->second;
    if (before == wanted) continue;
    const std::string& part = key.second;
    if (part[0] == '?') { all = false; continue; }
    // Everything it should set off now, from parts of the launch binding that set off those and nothing else. Its own old
    // job first, then the parts that did the most.
    std::vector<std::pair<const PartKey*, const std::set<std::string>*>> carriers;
    for (const auto& kv : was)
      if (!kv.second.empty() && kv.first.second[0] != '?') carriers.push_back({&kv.first, &kv.second});
    std::stable_sort(carriers.begin(), carriers.end(), [&](const auto& a, const auto& b) {
      if ((*a.first == key) != (*b.first == key)) return *a.first == key;
      return a.second->size() > b.second->size();
    });
    std::set<std::string> left = wanted;
    bool self = false;
    std::vector<LiveLink> picks;
    for (const auto& [ck, sig] : carriers) {
      if (left.empty()) break;
      std::string toPart;
      float amount = 1;
      bool fits = true;
      std::vector<std::string> used, across, up;  // a stick's numbers that nothing here asks for, by which one they are
      for (const std::string& e : *sig) {
        std::string match;
        for (const std::string& l : left)
          if (jobOf(l) == jobOf(e)) { match = l; break; }
        if (match.empty() && (kindOf(e) == "px" || kindOf(e) == "py")) {
          (kindOf(e) == "px" ? across : up).push_back(e);
          continue;
        }
        std::string tp;
        float amt = 1;
        if (match.empty() || !reachFor(kindOf(e), kindOf(match), ck->second, tp, amt) || (!toPart.empty() && (tp != toPart || amt != amount))) { fits = false; break; }
        toPart = tp;
        amount = amt;
        used.push_back(match);
      }
      // Pushing a stick up leaves its across number alone, and pushing it sideways its up one. Anything else it did, it does.
      if (fits && (!across.empty() || !up.empty())) {
        const bool upDown = toPart == "north" || toPart == "south", sideways = toPart == "east" || toPart == "west";
        fits = !toPart.empty() && (across.empty() || upDown) && (up.empty() || sideways);
      }
      if (!fits || toPart.empty() || !sourceSuits(part, toPart, amount)) continue;
      for (const std::string& u : used) left.erase(u);
      if (*ck == key && toPart == part && amount == 1) self = true;
      else picks.push_back({key.first, part, ck->first, toPart, amount});
    }
    if (!left.empty()) {  // something no part did at launch
      if (std::getenv("UTMTFTM_LIVE_DEBUG")) {
        printf("    live: %s %s can't: wants", key.first.c_str(), part.c_str());
        for (const std::string& e : left) printf(" [%s]", e.c_str());
        printf("\n");
      }
      all = false;
      continue;
    }
    // A stick pushed one way can't stop doing its old job and keep the rest of the stick's: that waits.
    if (!before.empty() && !self && isDirection(part)) { all = false; continue; }
    if (self) {
      if (picks.empty()) continue;
      picks.push_back({key.first, part, key.first, part, 1});  // it keeps doing its own, as well
    }
    if (picks.empty()) {
      if (!before.empty()) out.push_back({key.first, part, "", "", 1});  // it does nothing now
      continue;
    }
    out.insert(out.end(), picks.begin(), picks.end());
  }
  if (allOfIt) *allOfIt = all;
  return out;
}

std::vector<LiveLink> Page::linksNow(bool* allOfIt) {
  if (allOfIt) *allOfIt = true;
  if (gameKey_.empty() || !launchKnown_ || !launchJsonOk_ || !lib_.profile(yours_.type)) return {};
  const Found f = activeBinding();
  if (!f.ok) {
    if (allOfIt) *allOfIt = false;
    return {};
  }
  return liveLinks(launchJson_, f.json, allOfIt);
}

// The game still has the bindings it launched with. If your A should now do what its B does, the driver can send your A's
// press as B's. If nothing on the controller does that action right now, there's no press to send, so that waits.
std::vector<Page::LiveRoute> Page::liveRoutes(bool* allOfIt) const {
  std::vector<LiveRoute> out;
  if (allOfIt) *allOfIt = true;
  if (gameKey_.empty() || !launchKnown_ || !lib_.profile(yours_.type)) return out;
  const std::string from = effectiveFrom();
  Wiring before;
  if (from == launchFrom_) {
    if (from.empty()) return out;  // on the developer's bindings, and nothing's been rewired
    before = launchWiring_;
  } else if (launchFrom_.empty() && from == yours_.type) {
    before = defaultWiring(lib_, from, yours_.type);  // the developer's bindings have every button doing its own job
  } else {
    if (allOfIt) *allOfIt = false;  // a whole other layout
    return out;
  }
  const Wiring now = wiring(from, yours_.type);
  for (const auto& [input, their] : before) {  // a part's own job taken away again: the game still has it until it launches
    auto still = now.find(input);
    if (input.find('#') != std::string::npos && (still == now.end() || still->second != their) && allOfIt) *allOfIt = false;
  }
  for (const auto& [input, their] : now) {
    auto was = before.find(input);
    if (was != before.end() && was->second == their) continue;
    // A stick direction, a press or a touch on its own isn't one button standing in for another, so it waits.
    if (input.find('#') != std::string::npos || their.find('#') != std::string::npos) {
      if (allOfIt) *allOfIt = false;
      continue;
    }
    if (their.empty()) { out.push_back({input, ""}); continue; }
    std::string carrier;
    for (const auto& [other, itsJob] : before) {
      if (itsJob == their && other.find('#') == std::string::npos) { carrier = other; break; }
    }
    if (carrier.empty()) {
      if (allOfIt) *allOfIt = false;
      continue;
    }
    if (carrier != input) out.push_back({input, carrier});
  }
  return out;
}

// A combo made since the game launched, whose action is on a button: hold the controls and that button gets pressed.
std::vector<Combo> Page::liveCombos(bool* allOfIt) {
  std::vector<Combo> out;
  if (allOfIt) *allOfIt = true;
  const Profile* mine = lib_.profile(yours_.type);
  if (gameKey_.empty() || !launchKnown_ || !mine) return out;
  std::vector<GameAction> actions;
  bool haveActions = false;
  for (const Combo& c : combosHere()) {
    const bool old = std::any_of(launchCombos_.begin(), launchCombos_.end(), [&](const Combo& o) {
      if (o.output != c.output || o.inputs.size() != c.inputs.size()) return false;
      for (size_t i = 0; i < c.inputs.size(); ++i) if (o.inputs[i].path != c.inputs[i].path || o.inputs[i].state != c.inputs[i].state) return false;
      return true;
    });
    if (old) continue;
    if (!haveActions) {
      if (linking()) {
        // The button that does it is wherever it was when the game launched: that's what the game has.
        Found launched = activeBinding();
        launched.json = launchJson_;
        actions = comboActions(launched, *mine);
      } else {
        bool routesAll = true;
        const bool sameLayout = effectiveFrom() == launchFrom_ && liveRoutes(&routesAll).empty() && routesAll;
        if (sameLayout) actions = comboActions(activeBinding(), *mine);
      }
      haveActions = true;
    }
    auto it = std::find_if(actions.begin(), actions.end(), [&](const GameAction& a) { return lowered(a.output) == lowered(c.output) && !a.row.empty(); });
    // An older driver's live combos know a touch and a press, not a stick pushed one way, so those wait for the next launch.
    const bool pushes = !(hookFeatures_ & fhlive::kFeatureWays) && std::any_of(c.inputs.begin(), c.inputs.end(), [](const ComboInput& in) { return isDirection(in.state); });
    if (it == actions.end() || pushes || c.inputs.size() > fhlive::kMaxChordInputs) {
      if (allOfIt) *allOfIt = false;
      continue;
    }
    Combo live = c;
    live.output = it->row;
    out.push_back(live);
  }
  // One taken away since launch is still in the game's binding, and nothing can lift it out until it launches again.
  for (const Combo& o : launchCombos_) {
    const bool kept = std::any_of(combosHere().begin(), combosHere().end(), [&](const Combo& c) {
      if (o.output != c.output || o.inputs.size() != c.inputs.size()) return false;
      for (size_t i = 0; i < c.inputs.size(); ++i) if (o.inputs[i].path != c.inputs[i].path || o.inputs[i].state != c.inputs[i].state) return false;
      return true;
    });
    if (!kept && allOfIt) *allOfIt = false;
  }
  return out;
}

std::string Page::afterChange() {
  if (gameKey_.empty()) return "";
  bool routes = true, chords = true;
  if (linking()) linksNow(&routes);
  else liveRoutes(&routes);
  liveCombos(&chords);
  if (hookOk_ && hookHeard_ && routes && chords) return " Live in the game now.";
  return " It takes effect the next time " + gameName_ + " launches.";
}

static bool wireHand(const std::string& path, int& hand, std::string& rest) {
  const size_t slash = path.find('/');
  if (slash == std::string::npos) return false;
  const std::string side = path.substr(0, slash);
  if (side != "left" && side != "right") return false;
  hand = side == "right" ? 1 : 0;
  rest = path.substr(slash);
  return true;
}

void Page::fillInputTable(fhlive::InputTable& t) {
  std::memset(&t, 0, sizeof t);
  if (gameKey_.empty() || yours_.type.empty()) return;
  auto mine = recorded.find(choiceKey());
  if (mine != recorded.end())
    for (const macro::Recorded& m : mine->second) {
      if (m.hand < 0 || m.path.empty() || t.macros >= fhlive::kMaxMacros) continue;
      macro::toWire(m, t.macro[t.macros++]);
    }
  if (!launchKnown_) return;  // nothing's changed since it launched, then
  if (linking()) {
    static const std::map<std::string, uint8_t> partNo = {{"click", fhlive::LinkClick}, {"touch", fhlive::LinkTouch}, {"value", fhlive::LinkValue}, {"move", fhlive::LinkMove},
                                                          {"north", fhlive::LinkNorth}, {"east", fhlive::LinkEast}, {"south", fhlive::LinkSouth}, {"west", fhlive::LinkWest}};
    for (const LiveLink& l : linksNow(nullptr)) {
      if (t.links >= fhlive::kMaxLinks) break;
      auto fp = partNo.find(l.fromPart);
      int hand = 0;
      std::string path;
      if (fp == partNo.end() || !wireHand(l.from, hand, path)) continue;
      fhlive::WireLink& w = t.link[t.links];
      w = fhlive::WireLink{};
      w.fromHand = uint8_t(hand);
      w.fromPart = fp->second;
      macro::copyName(w.from, sizeof w.from, path);
      w.toHand = fhlive::kNoHand;
      w.amount = l.amount;
      if (!l.to.empty()) {
        auto tp = partNo.find(l.toPart);
        if (tp == partNo.end() || !wireHand(l.to, hand, path)) continue;
        w.toHand = uint8_t(hand);
        w.toPart = tp->second;
        macro::copyName(w.to, sizeof w.to, path);
      }
      ++t.links;
    }
  }
  for (const LiveRoute& r : linking() ? std::vector<LiveRoute>{} : liveRoutes(nullptr)) {
    if (t.routes >= fhlive::kMaxRoutes) break;
    fhlive::WireRoute& w = t.route[t.routes];
    int hand = 0;
    std::string path;
    if (!wireHand(r.from, hand, path)) continue;
    w.fromHand = uint8_t(hand);
    macro::copyName(w.from, sizeof w.from, path);
    w.toHand = fhlive::kNoHand;
    if (!r.to.empty() && wireHand(r.to, hand, path)) {
      w.toHand = uint8_t(hand);
      macro::copyName(w.to, sizeof w.to, path);
    }
    ++t.routes;
  }
  for (const Combo& c : liveCombos(nullptr)) {
    if (t.chords >= fhlive::kMaxChords) break;
    fhlive::WireChord& w = t.chord[t.chords];
    int hand = 0;
    std::string path;
    if (!wireHand(c.output, hand, path)) continue;
    w.targetHand = uint8_t(hand);
    macro::copyName(w.target, sizeof w.target, path);
    uint8_t n = 0;
    for (const ComboInput& in : c.inputs) {
      if (!wireHand(in.path, hand, path)) continue;
      w.in[n].hand = uint8_t(hand);
      w.in[n].onTouch = in.state == "touch" ? 1 : in.state == "north" ? fhlive::ChordNorth : in.state == "east" ? fhlive::ChordEast : in.state == "south" ? fhlive::ChordSouth : in.state == "west" ? fhlive::ChordWest : 0;
      macro::copyName(w.in[n].path, sizeof w.in[n].path, path);
      ++n;
    }
    w.count = n;
    if (n) ++t.chords;
  }
}

// ---------------------------------------------------------------- macros
std::string Page::controlName(int hand, const std::string& path) const {
  const Profile* p = lib_.profile(yours_.type);
  const Input* i = p ? p->find(hand ? "right" : "left", path) : nullptr;
  return i ? i->name : titleCase(path.substr(path.rfind('/') + 1));
}

std::string Page::playsOn(const macro::Recorded& m) const {
  if (m.hand < 0) return "";
  const Profile* p = lib_.profile(yours_.type);
  const Input* i = p ? p->find(m.hand ? "right" : "left", m.path) : nullptr;
  std::string out = m.onTouch ? "Touch " : "";
  if (!i || i->side.empty()) out += m.hand ? "right " : "left ";
  return out + controlName(m.hand, m.path);
}

std::vector<RecordedTile> Page::tilesFor(const std::vector<macro::Step>& steps, bool select) const {
  std::vector<RecordedTile> out;
  const Profile* p = lib_.profile(yours_.type);
  const std::vector<macro::Tile> tiles = macro::tilesOf(steps);
  for (size_t i = 0; i < tiles.size(); ++i) {
    const macro::Step& s = steps[size_t(tiles[i].first)];
    RecordedTile t;
    if (select) t.id = "macro_tile_" + std::to_string(i);
    t.name = controlName(s.hand, s.path);
    const Input* in = p ? p->find(s.hand ? "right" : "left", s.path) : nullptr;
    if (!in || in->side.empty()) t.hand = s.hand ? "R" : "L";
    t.kind = s.kind;
    t.x = s.x;
    t.y = s.y;
    t.both = tiles[i].count == 2;
    out.push_back(t);
  }
  return out;
}

// A controller reports more than its controls. The Frame's raw hand sensor, capsense, looks like a stick that's pushed for as long
// as you're holding on, and no game can bind it. What SteamVR's description of the controller lists is what a person can press.
bool Page::isControl(int hand, const std::string& path, const std::string& part) const {
  const Profile* p = lib_.profile(yours_.type);
  const Input* i = p ? p->find(hand ? "right" : "left", path) : nullptr;
  if (!i) return false;
  if (part == "x" || part == "y") return i->kind == "joystick" || i->kind == "trackpad";
  return true;
}

std::string Page::feltNow() const {
  std::map<std::string, int> level;  // 1 touching it, 2 pressing it
  for (const auto& [key, v] : felt_) {
    const size_t cut = key.rfind('/');
    const std::string part = key.substr(cut + 1);
    int& l = level[key.substr(0, cut)];
    if (part == "touch") { if (v > 0.5f) l = std::max(l, 1); }
    else if (part == "click") { if (v > 0.5f) l = 2; }
    else if (part == "x" || part == "y") { if (std::fabs(v) > 0.6f) l = 2; }
    else if (v > 0.75f) l = 2;
    else if (v > 0.1f) l = std::max(l, 1);
  }
  std::string out;
  for (const auto& [control, l] : level) {
    if (!l) continue;
    if (!out.empty()) out += ", ";
    if (l == 1) out += "Touch ";
    out += controlName(control[0] - '0', control.substr(1));
  }
  return out;
}

static std::string defaultRecordedName(const std::vector<macro::Step>& steps, const std::function<std::string(int, const std::string&)>& nameOf) {
  std::vector<std::string> names;
  for (const macro::Step& s : steps) {
    if (s.kind != macro::Press && s.kind != macro::Push) continue;
    const std::string n = nameOf(s.hand, s.path);
    if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
  }
  std::string out;
  for (size_t i = 0; i < names.size() && i < 3; ++i) out += (i ? " + " : "") + names[i];
  if (names.size() > 3) out += " + more";
  return out.empty() ? "Touches" : out;
}

RecordedModel Page::recordedModel(const std::string& hover, const std::string& down) {
  RecordedModel m;
  m.hover = hover;
  m.down = down;
  m.game = gameName_;
  m.status = status;
  m.statusWarn = statusKind == 2;
  m.statusGood = statusKind == 1;
  const Profile* mine = lib_.profile(yours_.type);
  if (!hookOk_) m.why = "Restart SteamVR once to turn this on. The add-on has to be there from the start to hear your buttons.";
  else if (gameKey_.empty()) m.why = "A macro belongs to one game, because what a button does is different in each.";
  else if (!mine) m.why = yours_.type.empty() ? "Turn your controllers on." : "SteamVR has no description of this controller.";
  else if (hookLate_) m.why = "SteamVR set your controllers up before UTMTFTM loaded, so it can't tell which button is which. Try restarting SteamVR.";
  else if (!hookHeard_) m.why = "UTMTFTM hasn't heard a button from this controller yet. Press one. If this stays, restart SteamVR.";
  m.canRecord = m.why.empty();
  if (!m.canRecord && recStep_ != 0) { recStep_ = 0; editing_ = macro::Recorded{}; }
  m.step = recStep_;
  m.ask = recAsk_;
  m.interview = recGuided_;
  m.interviewOn = macroInterview;
  m.touchesOn = macroTouches;
  m.feeling = feltNow();
  m.countdown = recCountdown_;
  m.waitShut = recWaitShut_;

  auto saved = recorded.find(choiceKey());
  if (saved != recorded.end())
    for (const macro::Recorded& r : saved->second) {
      RecordedRow row;
      row.name = r.name;
      const std::string on = playsOn(r);
      row.plays = on.empty() ? "No button yet" : (r.onTouch ? on : "Press " + on) + " to play" + (r.repeat ? ", repeats while held" : "");
      row.tiles = tilesFor(macro::played(r), false);
      m.saved.push_back(row);
    }

  if (recStep_ == 2) {
    const std::vector<macro::Step>& soFar = recorder_.soFar();
    const bool anyPress = std::any_of(soFar.begin(), soFar.end(), [](const macro::Step& s) { return s.kind == macro::Press || s.kind == macro::Push; });
    m.tiles = tilesFor(macro::shown(soFar, !anyPress), false);
  }
  if (recStep_ == 3 || recStep_ == 4 || recStep_ == 5) {
    const std::vector<macro::Step> steps = macro::shown(editing_.steps, editing_.withTouches);
    m.tiles = tilesFor(steps, recStep_ == 3);
    m.selected = recSelected_ < int(m.tiles.size()) ? recSelected_ : -1;
    // Whichever page of tiles the picked one is on.
    if (m.selected >= 0) recFirstTile_ = m.selected / kRecordedTilesPerPage * kRecordedTilesPerPage;
    if (recFirstTile_ >= int(m.tiles.size())) recFirstTile_ = 0;
    m.tileFirst = recFirstTile_;
    m.name = editing_.name;
    m.plays = playsOn(editing_);
    m.onTouch = editing_.onTouch;
    m.repeat = editing_.repeat;
    m.asRecorded = editing_.asRecorded;
    m.withTouches = editing_.withTouches;
    m.hasBareTouches = (macroTouches || editing_.withTouches) && macro::hasBareTouches(editing_.steps) && std::any_of(editing_.steps.begin(), editing_.steps.end(), [](const macro::Step& s) { return s.kind == macro::Press || s.kind == macro::Push; });
    // With Capacitive touches on, the row's always there, so it's plain whether any were felt.
    const bool anyPress = std::any_of(editing_.steps.begin(), editing_.steps.end(), [](const macro::Step& s) { return s.kind == macro::Press || s.kind == macro::Push; });
    const bool anyTouch = std::any_of(editing_.steps.begin(), editing_.steps.end(), [](const macro::Step& s) { return s.kind == macro::Touch; });
    if (macroTouches || anyTouch) m.touches = m.hasBareTouches ? 2 : anyTouch && !anyPress ? 3 : 1;
    if (editing_.hand >= 0) {
      const std::string name = controlName(editing_.hand, editing_.path), row = std::string(editing_.hand ? "right" : "left") + editing_.path;
      std::string job;
      if (mine)
        for (const GameAction& a : comboActions(activeBinding(), *mine))
          if (a.row == row) { job = a.label; break; }
      m.note = (job.empty() ? name + " stops doing its usual job" : name + " stops being " + job) + " in " + gameName_ + " while this is saved.";
      m.noteWarn = true;
      if (editing_.onTouch) {
        m.note = "Resting on " + name + " plays this. Pressing it still does what it did.";
        m.noteWarn = false;
      }
    }
    if (macro::played(editing_).size() > fhlive::kMaxSteps) {
      m.note = "That's too long to play in full. Only the first " + std::to_string(fhlive::kMaxSteps / 2) + " presses fit, so the rest won't play.";
      m.noteWarn = true;
    }
  }
  return m;
}

void Page::recordedClickGreyed(const std::string& id) {
  if (id == "macro_record") {
    if (!hookOk_) say("Restart SteamVR once to turn this on.");
    else if (gameKey_.empty()) say("Start a game first. A macro belongs to one game.");
    else say("Turn your controllers on first.");
  } else if (id == "macro_save") say("Choose which button plays it first.");
  else say("That isn't available right now.");
}

void Page::recordedClick(const std::string& id) {
  const std::vector<macro::Step> showing = macro::shown(editing_.steps, editing_.withTouches);
  const auto startRecording = [&] {
    recStep_ = 1;
    recPhaseStarted_ = false;
    recWaitShut_ = recShutMode_;
    recCountdown_ = 3;
    recSelected_ = -1;
    recFirstTile_ = 0;
  };
  if (id == "macro_record") {
    auto it = recorded.find(choiceKey());
    if (it != recorded.end() && it->second.size() >= 5) { say("Five per game for now, because that's what fits. Remove one first."); return; }
    editing_ = macro::Recorded{};
    editingAt_ = -1;
    recGuided_ = macroInterview;
    recFirstTile_ = 0;
    if (recGuided_) {
      recStep_ = 5;
      recAsk_ = 0;
      return;
    }
    startRecording();
    return;
  }
  if (id == "macro_touch_on" || id == "macro_touch_off") { macroTouches = id == "macro_touch_on"; savePrefs(); return; }
  if (id == "macro_interview_on" || id == "macro_interview_off") { macroInterview = id == "macro_interview_on"; savePrefs(); return; }
  if (id == "macro_ask_start") { startRecording(); return; }
  if (id == "macro_ask_next") {
    if (recAsk_ == 2 && editing_.hand < 0) { recordedClickGreyed("macro_save"); return; }
    if (recAsk_ + 1 < kRecordedQuestions) ++recAsk_;
    return;
  }
  if (id == "macro_ask_back") { if (recAsk_ > 0) --recAsk_; return; }
  if (id == "macro_steps_prev") { recFirstTile_ = std::max(0, recFirstTile_ - kRecordedTilesPerPage); recSelected_ = -1; return; }
  if (id == "macro_steps_next") { recFirstTile_ += kRecordedTilesPerPage; recSelected_ = -1; return; }
  if (id == "macro_again") { editing_.steps.clear(); startRecording(); return; }
  if (id == "macro_leave") {  // tapped another page during the countdown, or while it waited for the dashboard to shut
    const bool waited = recWaitShut_;
    recWaitShut_ = false;
    if (recStep_ == 1) {
      if (editing_.steps.empty()) recStep_ = 0;
      else backFromListening();
    } else if (waited && recStep_ == 4) backFromListening();
    return;
  }
  if (id == "macro_abort") {  // the dashboard shut while it was listening
    if (recordedListening()) {
      if (editing_.steps.empty()) recStep_ = 0;
      else backFromListening();
    }
    return;
  }
  if (id == "macro_cancel") { recStep_ = 0; editing_ = macro::Recorded{}; recGuided_ = false; recWaitShut_ = false; return; }
  if (id.rfind("macro_edit_", 0) == 0) {
    auto it = recorded.find(choiceKey());
    const size_t at = size_t(std::atoi(id.c_str() + 11));
    if (it == recorded.end() || at >= it->second.size()) return;
    editing_ = it->second[at];
    recAutoName_.clear();
    editingAt_ = int(at);
    recStep_ = 3;
    recGuided_ = false;
    recSelected_ = -1;
    recFirstTile_ = 0;
    return;
  }
  if (id.rfind("macro_del_", 0) == 0) {
    auto it = recorded.find(choiceKey());
    const size_t at = size_t(std::atoi(id.c_str() + 10));
    if (it == recorded.end() || at >= it->second.size()) return;
    const std::string name = it->second[at].name;
    it->second.erase(it->second.begin() + std::ptrdiff_t(at));
    if (it->second.empty()) recorded.erase(it);
    savePrefs();
    say("Removed " + name + ". That button's back to its usual job.", 1);
    return;
  }
  if (id.rfind("macro_tile_", 0) == 0) {
    const int at = std::atoi(id.c_str() + 11);
    recSelected_ = recSelected_ == at ? -1 : at;
    return;
  }
  if (id == "macro_left" || id == "macro_right" || id == "macro_remove_step") {
    // These work on what's showing, so whatever's hidden is gone from here on.
    if (recSelected_ < 0 || recSelected_ >= int(macro::tilesOf(showing).size())) return;
    editing_.steps = showing;
    if (id == "macro_remove_step") {
      macro::removeTile(editing_.steps, recSelected_);
      recSelected_ = -1;
      if (editing_.steps.empty()) { recStep_ = 0; editing_ = macro::Recorded{}; say("That was the last step, so there's nothing left to save."); }
      return;
    }
    recSelected_ = macro::moveTile(editing_.steps, recSelected_, id == "macro_left" ? -1 : 1);
    return;
  }
  if (id == "macro_pick") {
    picker_ = macro::TriggerPicker{};
    recStep_ = 4;
    recPhaseStarted_ = false;
    recWaitShut_ = recShutMode_;
    return;
  }
  // In the interview these answer the question, so it moves on.
  const auto answered = [&] { if (recStep_ == 5 && recAsk_ + 1 < kRecordedQuestions) ++recAsk_; };
  if (id == "macro_once") { editing_.repeat = false; answered(); return; }
  if (id == "macro_repeat") { editing_.repeat = true; answered(); return; }
  if (id == "macro_quick") { editing_.asRecorded = false; answered(); return; }
  if (id == "macro_mine") { editing_.asRecorded = true; answered(); return; }
  if (id == "macro_press_only") { editing_.withTouches = false; return; }
  if (id == "macro_touches") { editing_.withTouches = true; return; }
  if (id == "macro_rename") { recWantsName_ = true; return; }
  if (id == "macro_save") {
    if (editing_.hand < 0) { recordedClickGreyed(id); return; }
    std::vector<macro::Recorded>& list = recorded[choiceKey()];
    macro::Recorded done = editing_;
    if (editingAt_ >= 0 && editingAt_ < int(list.size())) list.erase(list.begin() + editingAt_);
    // A button can only play one thing.
    const size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const macro::Recorded& o) { return o.hand == done.hand && o.path == done.path && o.onTouch == done.onTouch; }), list.end());
    const bool replaced = list.size() != before;
    list.push_back(done);
    savePrefs();
    recStep_ = 0;
    recGuided_ = false;
    editing_ = macro::Recorded{};
    say(done.name + " is on " + playsOn(done) + (replaced ? ", in place of what was there." : ".") + " Works as soon as you close the dashboard.", 1);
    return;
  }
}

// With the dashboard shut the game's in front of you, so the countdown holds your buttons back from it too.
bool Page::recordedListening() const {
  if (recWaitShut_) return false;
  return recStep_ == 2 || recStep_ == 4 || (recShutMode_ && recStep_ == 1);
}

void Page::recordedDashboard(bool up) {
  const auto phase = [&] { return recStep_ == 1 || recStep_ == 2 || recStep_ == 4; };
  if (!recShutMode_) {
    recWaitShut_ = false;
    if (recordedListening() && !up) recordedClick("macro_abort");  // the dashboard shut while it was listening
  } else if (recWaitShut_) {
    if (!phase()) recWaitShut_ = false;
    else if (!up) {  // shut: now it can start, and the sign takes over
      recWaitShut_ = false;
      recPhaseStarted_ = false;
    }
  } else if (phase() && up) stopListening();  // opened again part way through
  const bool shutPhase = recShutMode_ && !recWaitShut_ && phase();
  if (recWasShutPhase_ && !shutPhase && !up) {
    recReopen_ = true;
    // What the sign says while the dashboard opens again, in case SteamVR doesn't open it from here.
    if (recShutStep_ == 4) {
      const std::string on = playsOn(editing_);
      recReopenTitle_ = picker_.done() && !on.empty() ? "Got it" : "Nothing was pressed";
      recReopenSub_ = picker_.done() && !on.empty() ? on + " plays it. Save it in UTMTFTM on the dashboard." : "So no button's been chosen.";
    } else if (recShutStep_ == 2 && !recorder_.empty()) {
      recReopenTitle_ = "Got it";
      recReopenSub_ = "Your steps are in UTMTFTM on the dashboard.";
    } else {
      recReopenTitle_ = "Nothing was pressed";
      recReopenSub_ = "So nothing's been recorded.";
    }
  }
  if (shutPhase) recShutStep_ = recStep_;
  recWasShutPhase_ = shutPhase;
}

bool Page::recordedReopen(std::string* title, std::string* sub) {
  const bool r = recReopen_;
  recReopen_ = false;
  if (r && title) *title = recReopenTitle_;
  if (r && sub) *sub = recReopenSub_;
  return r;
}

bool Page::recordedSign(PanelModel& p) const {
  p = PanelModel{};
  if (!recShutMode_ || recWaitShut_) return false;
  if (recStep_ == 1) {
    p.header = "RECORDING A MACRO";
    p.chip = std::to_string(std::max(1, recCountdown_));
    p.text = "Let go of everything";
    p.status = "Then do your sequence. The game won't get your buttons while it records.";
    p.keys = {{"STEAM", "Stop"}};
    return true;
  }
  if (recStep_ == 2) {
    p.header = "RECORDING";
    p.text = "Do your sequence now";
    std::string heard;
    int presses = 0;
    for (const macro::Step& s : recorder_.soFar())
      if (s.kind == macro::Press || s.kind == macro::Push) {
        ++presses;
        if (presses <= 6) heard += (heard.empty() ? "" : ", ") + controlName(s.hand, s.path);
      }
    if (presses > 6) heard += " and " + std::to_string(presses - 6) + " more";
    p.status = presses ? "Heard " + heard + ". It stops 2 seconds after your last press." : "Nothing yet. It stops 2 seconds after your last press.";
    p.keys = {{"STEAM", "Stop"}};
    return true;
  }
  if (recStep_ == 4) {
    p.header = "CHOOSE THE BUTTON";
    p.text = macroTouches ? "Press or touch what should play this" : "Press what should play this";
    p.status = picker_.done() ? "Got " + controlName(picker_.hand(), picker_.path()) + ". Now let go of it." : "Any button, on either controller.";
    p.keys = {{"STEAM", "Cancel"}};
    return true;
  }
  return false;
}

void Page::stopListening() {
  if (recStep_ == 2 && !recorder_.empty()) { finishTake(); return; }  // keep what you did before opening it
  if (recStep_ == 4 && picker_.done()) { backFromListening(); return; }
  if (!recGuided_ && editing_.steps.empty() && recStep_ != 4) recStep_ = 0;
  else backFromListening();
}

void Page::finishTake() {
  editing_.steps = recorder_.steps();
  const bool anyPress = std::any_of(editing_.steps.begin(), editing_.steps.end(), [](const macro::Step& s) { return s.kind == macro::Press || s.kind == macro::Push; });
  editing_.withTouches = !anyPress;
  if (editing_.name.empty() || editing_.name == recAutoName_) {
    recAutoName_ = defaultRecordedName(editing_.steps, [this](int h, const std::string& p) { return controlName(h, p); });
    editing_.name = recAutoName_;
  }
  recAsk_ = 1;
  backFromListening();
  recSelected_ = -1;
}

void Page::backFromListening() {
  recStep_ = recGuided_ ? 5 : 3;
  recPhaseStarted_ = false;
}

void Page::recordedReport(const macro::Raw& r) {
  std::string path, part;
  if (!macro::splitName(r.name, path, part) || !isControl(r.hand, path, part)) return;
  felt_[std::to_string(r.hand) + path + "/" + part] = r.value;
  const bool counts = macroTouches || part != "touch";
  if (recStep_ == 2) {
    if (counts) recorder_.feed(r);
    return;
  }
  if (recStep_ != 4) return;
  const bool was = picker_.done();
  if (counts) picker_.feed(r);
  if (picker_.done() && !was) {
    editing_.hand = picker_.hand();
    editing_.path = picker_.path();
    editing_.onTouch = picker_.onTouch();
    if (picker_.onTouch()) backFromListening();
    return;
  }
  // The press stays with us until it's let go of, so the dashboard doesn't get half of it and click something.
  if (picker_.done() && !picker_.onTouch() && r.hand == picker_.hand() && path == picker_.path() && (part == "click" || part == "value") && r.value < 0.4f) backFromListening();
}

void Page::recordedTick(uint32_t clockMs, uint32_t driverMs) {
  if (recStep_ != 1 && recStep_ != 2 && recStep_ != 4) return;
  if (recWaitShut_) return;  // nothing starts until the dashboard's shut
  if (!recPhaseStarted_) {
    recPhaseStarted_ = true;
    recPhaseStart_ = clockMs;
    if (recStep_ == 4) {
      picker_.begin(driverMs);
      felt_.clear();
    }
  }
  const uint32_t elapsed = clockMs - recPhaseStart_;
  if (recStep_ == 1) {
    recCountdown_ = 3 - int(elapsed / 1000);
    if (elapsed >= 3000) {
      recStep_ = 2;
      recPhaseStart_ = clockMs;
      recorder_ = macro::Recorder{};
      recorder_.begin(driverMs);
      felt_.clear();
    }
    return;
  }
  if (recStep_ == 2) {
    const bool someDone = recorder_.finished(driverMs) || recorder_.full() || elapsed >= 60000;
    if (recorder_.empty() && elapsed >= 20000) {
      if (recGuided_) {
        recStep_ = 5;
        recAsk_ = editing_.steps.empty() ? 0 : 1;
      } else recStep_ = editing_.hand >= 0 ? 3 : 0;
      say("Nothing was pressed, so nothing's been recorded.", 2);
      return;
    }
    if (recorder_.empty() || !someDone) return;
    finishTake();
    return;
  }
  // Choosing the button. Fifteen seconds, or a second and a half more to let go of what was chosen.
  if (picker_.done() ? elapsed >= 16500 : elapsed >= 15000) {
    backFromListening();
    if (!picker_.done()) say("Nothing was pressed, so no button's been chosen.");
  }
}

bool Page::recordedWantsName(std::string* current) const {
  if (recWantsName_ && current) *current = editing_.name;
  return recWantsName_;
}

void Page::recordedSetName(const std::string& name) {
  recWantsName_ = false;
  if (!name.empty()) editing_.name = name.size() > 40 ? name.substr(0, 40) : name;
}

}  // namespace bind
}  // namespace fh

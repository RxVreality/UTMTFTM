#include "bindings.h"

#include <algorithm>
#include <cctype>
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
      if (foundFile(path, out)) { out.names = names; return out; }
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
  if (!newest.empty() && foundFile(newest, out)) { out.names = names; return out; }

  // SteamVR ships bindings of its own for some games.
  if (const Profile* p = lib.profile(type))
    for (const auto& db : p->defaultBindings)
      if (std::find(keys.begin(), keys.end(), db["app_key"].asString()) != keys.end() && foundFile(pathText(fs::u8path(p->dir) / fs::u8path(db["binding_url"].asString())), out)) {
        out.names = names;
        return out;
      }

  // The game's own action file lists one for each controller it knows.
  if (haveManifest && m["default_bindings"].isArray())
    for (const auto& db : m["default_bindings"])
      if (db.isObject() && db["controller_type"].isString() && db["binding_url"].isString() && db["controller_type"].asString() == type && foundFile(pathText(fs::u8path(manifest).parent_path() / fs::u8path(db["binding_url"].asString())), out)) {
        out.names = names;
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
    if (mode == "joystick" || mode == "trackpad") out["mode"] = to.kind;
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

Json::Value translate(const Json::Value& from, const Wiring& wiring, const Profile& theirs, const Profile& yours) {
  // Their input -> the ones of yours that act as it.
  std::map<std::string, std::vector<std::string>> onto;
  for (const auto& [mine, their] : wiring)
    if (!their.empty()) onto[their].push_back(mine);
  auto yourInput = [&](const std::string& id) -> const Input* {
    const size_t slash = id.find('/');
    return slash == std::string::npos ? nullptr : yours.find(id.substr(0, slash), id.substr(slash));
  };
  // Anything the wiring doesn't mention stays where it is, if your controller has it at all. That's the right hand's
  // system button on most of them, which profiles only list on one side.
  std::map<std::string, std::vector<std::string>> same;
  auto targets = [&](const std::string& path) -> const std::vector<std::string>& {
    const std::string row = theirs.rowOf(path);
    auto it = onto.find(row.empty() ? path : row);
    if (it != onto.end()) return it->second;
    std::vector<std::string>& s = same[path];
    if (s.empty() && yourInput(path) && !wiring.count(path)) s.push_back(path);
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
    if (set.isMember("sources")) nextSet["sources"] = sources;

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
        {"stick_clicks", "Stick click"}, {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}};
    auto u = unity.find(word);
    if (endsWith(n, "touched") || endsWith(n, "touch") || n == "gripforce" || endsWith(n, "ready")) hidden = true;
    if (u != unity.end()) { label = u->second; if (word != "primarybutton" && word != "secondarybutton") pass = true; }
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
  };
  auto it = table.find(key);
  return it == table.end() ? std::vector<std::string>{} : it->second;
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
  writeJson(pathText(fs::u8path(d_.data) / "bindings.json"), v);
}

void Page::load() {
  lib_.load(d_);
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
  Json::Value out = translate(f.json, wiring(from, yours_.type), *theirs, *mine);
  std::string layoutName = titleCase(from);
  for (const Headset& h : lib_.headsets) if (h.type == from) { layoutName = shortName(h); break; }
  out["name"] = from == yours_.type ? "Rewired by UTMTFTM" : layoutName + " layout, translated by UTMTFTM";
  out["description"] = "Made by UTMTFTM from this game's " + from + " binding. Switch it back on the UTMTFTM page.";
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

void Page::reapply() {
  if (gameKey_.empty()) return;
  found_.clear();
  const std::string from = effectiveFrom();
  if (from.empty()) revertNow();
  else if (binding(from).ok) applyNow(from);
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
  m.armed = armed_.empty() ? "" : "y:" + armed_;

  const Profile* yourP = lib_.profile(yours_.type);
  const Profile* theirP = lib_.profile(ref_.type);
  const std::string refShort = ref_.name.empty() ? "" : shortName(ref_);

  // Their side: the reference, with what this game has on it.
  std::vector<RowText> theirRows[2];
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
    for (auto& rows : theirRows)
      for (RowText& r : rows) r.unused = theirBinding && !r.bound;
  }
  m.theirs.title = ref_.name;
  bool anyNamed = false;
  for (const auto& rows : theirRows)
    for (const RowText& r : rows) anyNamed = anyNamed || !r.action.empty();
  if (!m.hasGame) m.theirs.note = "Where everything sits";
  else if (!theirBinding) m.theirs.note = gameName_ + " hasn't set this one up";
  else if (anyNamed) m.theirs.note = "What " + gameName_ + " has on it";
  else m.theirs.note = gameName_ + " uses these, but doesn't say for what";

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
  auto actsAs = [&](const RowText& mine) -> RowText {
    RowText r;
    auto it = wired.find(mine.id);
    if (it == wired.end() || it->second.empty()) { r.unused = true; return r; }
    for (const auto& rows : theirRows)
      for (const RowText& t : rows)
        if (t.id == it->second) {
          if (theirBinding && t.unused) { r.unused = true; return r; }
          if (!t.action.empty()) r.action = t.action;
          else if (squeezed(t.name) != squeezed(mine.name) || it->second.substr(0, 5) != mine.id.substr(0, 5)) r.action = refShort + " " + t.name;
          return r;
        }
    r.action = refShort + " " + titleCase(it->second.substr(it->second.rfind('/') + 1));
    return r;
  };

  if (yourP) {
    const bool showWiring = !m.hasGame || m.layoutOn;
    const Found* native = m.hasGame ? &binding(yours_.type) : nullptr;
    Found translated;
    // Nothing of its own for your controller: SteamVR translates from the best one it has, so show that.
    std::string autoFrom;
    if (m.hasGame && !native->ok && !m.layoutOn) {
      auto table = lib_.remaps.find(yours_.type);
      int best = -1;
      if (table != lib_.remaps.end())
        for (const auto& layout : table->second["layouts"]) {
          const std::string type = layout["from_controller_type"].asString();
          const int pr = layout.get("priority", 0).asInt();
          if (pr > best && lib_.profile(type) && binding(type).ok) { best = pr; autoFrom = type; }
        }
      if (!autoFrom.empty()) {
        translated = binding(autoFrom);
        translated.json = translate(translated.json, defaultWiring(lib_, autoFrom, yours_.type), *lib_.profile(autoFrom), *yourP);
      }
    }
    const Found* shown = !autoFrom.empty() ? &translated : native;
    for (int hand = 0; hand < 2; ++hand) {
      yourRows[hand] = rowsOn(*yourP, hand ? "right" : "left", showWiring ? nullptr : shown, group_);
      for (RowText& r : yourRows[hand]) {
        if (showWiring) {
          const RowText a = actsAs(r);
          r.action = a.action;
          r.unused = a.unused;
        } else r.unused = shown && shown->ok && !r.bound;
      }
    }
    if (!m.hasGame) m.yours.note = "How it translates to the " + ref_.name;
    else if (m.layoutOn) m.yours.note = ref_.type == yours_.type ? "Rewired by you" : "On the " + refShort + " layout";
    else if (m.layoutOther) {
      std::string otherName = titleCase(from);
      for (const Headset& h : lib_.headsets) if (h.type == from) { otherName = shortName(h); break; }
      m.yours.note = "On the " + otherName + " layout";
    }
    else if (native->ok) m.yours.note = "As the developer set it";
    else if (!autoFrom.empty()) {
      std::string otherName = titleCase(autoFrom);
      for (const Headset& h : lib_.headsets) if (h.type == autoFrom) { otherName = h.name; break; }
      m.yours.note = "SteamVR is translating the game's " + otherName + " bindings";
    } else m.yours.note = "No bindings found yet. Play it once";
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
    if (!sameAction(hover.substr(2), yourRows, theirRows, "t:")) {
      auto it = wired.find(hover.substr(2));
      if (it != wired.end() && !it->second.empty()) m.lit.insert("t:" + it->second);
    }
  } else if (hover.rfind("t:", 0) == 0) {
    m.lit.insert(hover);
    if (!sameAction(hover.substr(2), theirRows, yourRows, "y:"))
      for (const auto& [mine, their] : wired)
        if (their == hover.substr(2)) m.lit.insert("y:" + mine);
  }

  const bool changesAnything = ref_.type != yours_.type || m.wiringChanged;
  m.layoutOk = changesAnything && (!m.hasGame || theirBinding != nullptr);
  if (!armed_.empty() && yourP) {
    const Input* i = yourP->find(armed_.substr(0, armed_.find('/')), armed_.substr(armed_.find('/')));
    m.armedName = i ? i->name : armed_;
  }
  m.status = status;
  m.statusWarn = statusKind == 2;
  m.statusGood = statusKind == 1;
  return m;
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
  auto saved = [&](const std::string& what) {
    say(what + (gameKey_.empty() ? "" : " It takes effect the next time " + gameName_ + " launches."), 1);
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
  if (id == "bind_cancel") { armed_.clear(); return; }
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
    wiringChanges[ref_.type + ">" + yours_.type][armed_] = "";
    armed_.clear();
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
      revertNow();
      saved(gameName_ + " is back to the developer's bindings.");
    } else {
      defaults.erase(yours_.type);
      if (!gameKey_.empty()) { games[choiceKey()].explicitly = false; revertNow(); }
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
    armed_ = armed_ == id.substr(2) ? "" : id.substr(2);
    return;
  }
  if (id.rfind("t:", 0) == 0) {
    if (armed_.empty()) { say("To change what a button does, tap it on your own controller first."); return; }
    wiringChanges[ref_.type + ">" + yours_.type][armed_] = id.substr(2);
    armed_.clear();
    if (!gameKey_.empty() && binding(ref_.type).ok) {
      GameChoice& c = games[choiceKey()];
      c.from = ref_.type;
      c.explicitly = true;
      applyNow(ref_.type);
      saved("Rewired, and this game is on that layout now.");
    } else {
      reapply();
      saved("Rewired.");
    }
    return;
  }
}

}  // namespace bind
}  // namespace fh

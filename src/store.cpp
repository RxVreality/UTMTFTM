#include "store.h"

#include <json/json.h>

#include "decaf.h"
#include "live.h"
#include "platform.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace fh {

void logf(const char* fmt, ...) {
  static FILE* f = nullptr;
  if (!f) {
    fs::create_directories(Store::dir());
    f = fopen((Store::dir() + "/utmtftm.log").c_str(), "a");
  }
  char ts[32]; time_t t = time(nullptr); strftime(ts, sizeof ts, "%F %T", localtime(&t));
  va_list a; va_start(a, fmt);
  char buf[2048]; vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
  fprintf(stderr, "%s %s\n", ts, buf);
  if (f) { fprintf(f, "%s %s\n", ts, buf); fflush(f); }
}

std::string Store::dir() { return platform::dataDir(); }
std::string Store::bindingsDir() { return dir() + "/bindings"; }

static bool readJson(const std::string& path, Json::Value& out) {
  std::ifstream in(path);
  if (!in) return false;
  Json::CharReaderBuilder b; std::string errs;
  return Json::parseFromStream(b, in, &out, &errs);
}

static bool writeJson(const std::string& path, const Json::Value& v) {
  Json::StreamWriterBuilder w; w["indentation"] = "   ";
  std::string tmp = path + ".tmp";
  { std::ofstream o(tmp); if (!o) return false; o << Json::writeString(w, v) << "\n"; }
  fs::rename(tmp, path);
  return true;
}

static Json::Value offJ(const Offset& o) {
  Json::Value v;
  for (int i = 0; i < 3; ++i) { v["pos"].append(o.pos[i]); v["rot"].append(o.rot[i]); }
  return v;
}
static Offset offFrom(const Json::Value& v) {
  Offset o;
  for (int i = 0; i < 3; ++i) { o.pos[i] = v["pos"].get(i, 0.0).asFloat(); o.rot[i] = v["rot"].get(i, 0.0).asFloat(); }
  return o;
}
static Json::Value pairJ(const HandPair& p) { Json::Value v; v["left"] = offJ(p.left); v["right"] = offJ(p.right); return v; }
static HandPair pairFrom(const Json::Value& v) { return {offFrom(v["left"]), offFrom(v["right"])}; }

bool Store::load() {
  Json::Value v;
  if (!readJson(dir() + "/profiles.json", v)) return false;
  max_pos = std::clamp(v["limits"].get("max_pos_m", 0.15).asFloat(), 0.01f, 0.30f);
  max_rot = 180.f;  // never limit rotation (older profiles stored 60)
  global = pairFrom(v["global"]);
  // Version 1 profiles matched the ghost to the old hand mesh, so those matches no longer line up.
  if (v.get("version", 1).asInt() >= 2)
    for (int h = 0; h < 2; ++h) {
      const char* k = h ? "right" : "left";
      ghost[h] = offFrom(v["ghost"][k]);
      ghostMatched[h] = v["ghost"].get(std::string(k) + "_matched", false).asBool();
    }
  apps.clear();
  for (const auto& k : v["apps"].getMemberNames()) {
    const auto& a = v["apps"][k];
    AppProfile p;
    p.name = a.get("name", "").asString();
    p.override_global = a.get("override_global", false).asBool();
    p.hands = pairFrom(a["hands"]);
    p.original_url = a.get("original_url", "").asString();
    p.settings_key = a.get("settings_key", "").asString();
    p.managed = a.get("managed", false).asBool();
    p.live = a.get("live", false).asBool();
    p.decaf = std::clamp(a.get("decaf", -1).asInt(), -1, decaf::kLevels - 1);
    apps[k] = p;
  }
  decafAll = std::clamp(v.get("decaf_all", 0).asInt(), 0, decaf::kLevels - 1);
  return true;
}

bool Store::save() const {
  fs::create_directories(dir());
  Json::Value v;
  v["version"] = 2;
  v["limits"]["max_pos_m"] = max_pos;
  v["global"] = pairJ(global);
  for (int h = 0; h < 2; ++h) {
    const char* k = h ? "right" : "left";
    v["ghost"][k] = offJ(ghost[h]);
    v["ghost"][std::string(k) + "_matched"] = ghostMatched[h];
  }
  v["apps"] = Json::objectValue;
  for (const auto& [k, p] : apps) {
    Json::Value a;
    a["name"] = p.name;
    a["override_global"] = p.override_global;
    a["hands"] = pairJ(p.hands);
    a["original_url"] = p.original_url;
    a["settings_key"] = p.settings_key;
    a["managed"] = p.managed;
    a["live"] = p.live;
    if (p.decaf >= 0) a["decaf"] = p.decaf;
    v["apps"][k] = a;
  }
  v["decaf_all"] = decafAll;
  return writeJson(dir() + "/profiles.json", v);
}

HandPair Store::effective(const std::string& app) const {
  auto it = apps.find(app);
  if (it != apps.end() && it->second.override_global) return it->second.hands;
  return global;
}

// ---- SteamVR binding plumbing -------------------------------------------------------------

std::string Store::controllerType;
static const char* kCats[] = {"openxr", "steamvr_input", "legacy"};

// The setting spells it steamvrinput, the binding file steamvr_input.
static std::string keyFor(const std::string& cat) { return Store::controllerType + "_250820_CurrentURL_" + (cat == "steamvr_input" ? "steamvrinput" : cat); }

static std::string getSetting(const std::string& sec, const std::string& key) {
  char buf[4096] = {0};
  vr::EVRSettingsError e = vr::VRSettingsError_None;
  vr::VRSettings()->GetString(sec.c_str(), key.c_str(), buf, sizeof buf, &e);
  return e == vr::VRSettingsError_None ? std::string(buf) : std::string();
}

static std::string ourPath(const std::string& app) { return Store::bindingsDir() + "/" + app + "_" + Store::controllerType + ".json"; }

static std::string newestIn(const std::string& d) {
  std::string best; fs::file_time_type bt{};
  std::error_code ec;
  for (auto& e : fs::directory_iterator(d, ec)) {
    if (!e.is_regular_file()) continue;
    if (best.empty() || e.last_write_time() > bt) { best = e.path().generic_string(); bt = e.last_write_time(); }
  }
  return best;
}

// Last binding file vrserver reported loading for this app whose controller_type we can use.
static std::string fromLog(const std::string& app) {
  std::string logs = platform::steamDir() + "/logs/";
  std::string found;
  for (const char* f : {"vrserver.previous.txt", "vrserver.txt"}) {
    std::ifstream in(logs + f);
    std::string line, needle = "for app '" + app + "'";
    while (std::getline(in, line)) {
      if (line.find("Successfully loaded binding file '") == std::string::npos || line.find(needle) == std::string::npos) continue;
      auto a = line.find('\'') + 1, b = line.find('\'', a);
      std::string p = line.substr(a, b - a);
      if (p.find("_hmd") != std::string::npos) continue;
      Json::Value v;
      if (!readJson(p, v)) continue;
      std::string ct = v.get("controller_type", "").asString();
      if (ct == Store::controllerType || ct == "oculus_touch" || ct == "knuckles") found = p;  // prefer later lines
    }
  }
  return found;
}

static std::string resolveBase(const std::string& app, const std::string& url) {
  const std::string file = "file:///";
  if (url.rfind(file, 0) == 0) {
    std::string p = url.substr(file.size());
#ifndef _WIN32
    p = "/" + p;
#endif
    if (fs::exists(p)) return p;
  }
  const std::string ws = "vr-input-workshop://";
  if (url.rfind(ws, 0) == 0) {
    std::string d = platform::steamDir() + "/steamapps/workshop/content/250820/" + url.substr(ws.size());
    std::string p = newestIn(d);
    if (!p.empty()) return p;
  }
  std::string local = platform::documentsDir() + "/steamvr/input/" + app + "_" + Store::controllerType + ".json";
  if (fs::exists(local)) return local;
  return fromLog(app);
}

static Json::Value offsetsJ(const HandPair& h) {
  Json::Value arr(Json::arrayValue);
  auto add = [&](const char* role, const Offset& o) {
    if (o.zero()) return;
    Json::Value e;
    e["role"] = role;
    for (int i = 0; i < 3; ++i) { e["offset_position"].append(o.pos[i]); e["offset_rotation"].append(o.rot[i]); }
    arr.append(e);
  };
  add("left_hand", h.left);
  add("right_hand", h.right);
  return arr;
}

std::string Store::apply(const std::string& app, std::string* err) {
  AppProfile& p = apps[app];
  HandPair eff = effective(app);
  if (controllerType.empty()) {  // binding files are per controller type
    if (err) *err = "Turn your controllers on first.";
    return "";
  }

  // Find which CurrentURL key this app uses, and what it points at now.
  std::string key = p.settings_key, url;
  if (key.empty()) {
    for (const char* c : kCats) {
      std::string u = getSetting(app, keyFor(c));
      if (!u.empty()) { key = keyFor(c); url = u; break; }
    }
  } else {
    url = getSetting(app, key);
  }
  const std::string ours = platform::fileUrl(ourPath(app));
  bool pointsAtUs = (url == ours);

  if (eff.zero()) {
    if (!p.managed) return "no offsets";
    vr::EVRSettingsError e = vr::VRSettingsError_None;
    if (p.original_url.empty()) vr::VRSettings()->RemoveKeyInSection(app.c_str(), key.c_str(), &e);
    else vr::VRSettings()->SetString(app.c_str(), key.c_str(), p.original_url.c_str(), &e);
    p.managed = false;
    logf("apply %s: offsets zero, restored original binding '%s' (key %s, err %d)", app.c_str(), p.original_url.c_str(), key.c_str(), (int)e);
    return "original binding restored";
  }

  std::string base = resolveBase(app, url);
  Json::Value b;
  if (base.empty() || !readJson(base, b)) {
    if (err) *err = "couldn't find this game's controller binding yet. Play it once, then try again.";
    logf("apply %s: no base binding (url '%s')", app.c_str(), url.c_str());
    return "";
  }
  if (key.empty()) key = keyFor(b.get("category", "steamvr_input").asString().c_str());
  std::string ct = b.get("controller_type", "").asString();
  if (ct != controllerType) logf("apply %s: WARNING base binding is '%s', not %s (untested path): %s", app.c_str(), ct.c_str(), controllerType.c_str(), base.c_str());

  if (!pointsAtUs && !p.managed) p.original_url = url;  // remember what to restore
  std::string bn = b.get("name", "Binding").asString();
  if (bn.find("(UTMTFTM)") == std::string::npos) b["name"] = bn + " (UTMTFTM)";
  Json::Value arr = offsetsJ(eff);
  b["options"]["device_offsets"] = arr;
  b["app_key"] = app;

  fs::create_directories(bindingsDir());
  if (!writeJson(ourPath(app), b)) { if (err) *err = "couldn't write binding file"; return ""; }

  vr::EVRSettingsError e = vr::VRSettingsError_None;
  vr::VRSettings()->SetString(app.c_str(), key.c_str(), ours.c_str(), &e);
  if (e != vr::VRSettingsError_None) {
    if (err) *err = std::string("SteamVR refused the setting: ") + vr::VRSettings()->GetSettingsErrorNameFromEnum(e);
    return "";
  }
  p.settings_key = key;
  p.managed = true;
  logf("apply %s: base %s -> %s (key %s, was '%s')", app.c_str(), base.c_str(), ours.c_str(), key.c_str(), url.c_str());
  return "saved. Restart the game to apply";
}

void Store::restoreBinding(const std::string& app) {
  auto it = apps.find(app);
  if (it == apps.end() || !it->second.managed) return;
  AppProfile& p = it->second;
  vr::EVRSettingsError e = vr::VRSettingsError_None;
  if (p.original_url.empty()) vr::VRSettings()->RemoveKeyInSection(app.c_str(), p.settings_key.c_str(), &e);
  else vr::VRSettings()->SetString(app.c_str(), p.settings_key.c_str(), p.original_url.c_str(), &e);
  p.managed = false;
  logf("restoreBinding %s -> '%s' (err %d)", app.c_str(), p.original_url.c_str(), (int)e);
}

void Store::publishLive(const HandPair* drag) const {
  fhlive::Shared* m = platform::mapLive();
  if (!m) return;
  HandPair now = drag ? *drag : effective(sceneKey);
  fhlive::DriverChannel& d = m->driver;
  fhlive::seqWriteBegin(d.seq);
  d.magic = fhlive::kDriverMagic;
  for (int h = 0; h < 2; ++h) {
    const Offset& o = h ? now.right : now.left;
    // grip' = grip * O and grip = raw * C, so the device itself has to move by C * O * C^-1.
    // Only with a game running: with none there are no hands to fix, only SteamVR's own pointers to throw off. A scene
    // with no key isn't a game either. On a Frame streaming from a PC the scene is Steam Link (vrlink.client), and an offset
    // here would land on top of whatever the PC's SteamVR applies.
    const bool standAside = dashboardUp && h != dashLiveHand;
    M dev = (useDriver && !standAside && scenePid && !sceneKey.empty() && !o.zero()) ? mul(mul(gripFromRaw[h], fromOffset(o)), inv(gripFromRaw[h])) : ident();
    if (!finite(dev)) dev = ident();
    d.device[h] = device[h];
    for (int i = 0; i < 3; ++i) d.want[h].pos[i] = dev.m[i][3];
    toQuat(dev, d.want[h].rot);
  }
  fhlive::seqWriteEnd(d.seq);
  publishDecaf();  // it follows the game the same way, and does nothing if that hasn't changed
}

int Store::decafLevel(const std::string& app) const {
  auto it = apps.find(app);
  if (it != apps.end() && it->second.decaf >= 0) return it->second.decaf;
  return decafAll;
}

bool Store::decafOwn(const std::string& app) const {
  auto it = apps.find(app);
  return it != apps.end() && it->second.decaf >= 0;
}

void Store::publishDecaf() const {
  fhlive::Shared* m = platform::mapLive();
  if (!m) return;
  // Only with a game running, the same as offsets: with none there's nothing to steady but SteamVR's own pointers.
  const int level = (scenePid && !sceneKey.empty()) ? decafLevel(sceneKey) : 0;
  const decaf::Params p = decaf::preset(level);
  fhlive::DecafSettings s[2];
  for (int h = 0; h < 2; ++h) {
    const bool pointing = dashboardUp && h == dashPointHand;
    s[h] = pointing ? fhlive::DecafSettings{} : fhlive::DecafSettings{p.rotMinHz, p.rotBeta, p.posMinHz, p.posBeta, p.speedHz};
  }
  fhlive::DecafChannel& d = m->decaf;
  if (!memcmp(d.want, s, sizeof s)) return;
  fhlive::seqWriteBegin(d.seq);
  memcpy(d.want, s, sizeof s);
  fhlive::seqWriteEnd(d.seq);
  logf("decaffeinate: step %d for %s%s", level, sceneKey.empty() ? "(no game)" : sceneKey.c_str(),
       level && dashboardUp && dashPointHand >= 0 ? (dashPointHand ? ", not the right hand while it points" : ", not the left hand while it points") : "");
}

bool Store::decafReady() {
  fhlive::Shared* m = platform::mapLive();
  return m && m->decaf.magic.load(std::memory_order_acquire) == fhlive::kDecafMagic;
}

void Store::decafMeasure(bool on) {
  fhlive::Shared* m = platform::mapLive();
  if (m) m->decaf.measure.store(on ? 1 : 0, std::memory_order_relaxed);
}

bool Store::decafShake(int hand, float& rawDeg, float& outDeg) {
  fhlive::Shared* m = platform::mapLive();
  if (!m || hand < 0 || hand > 1) return false;
  const uint32_t r = m->decaf.rotRaw[hand].load(std::memory_order_relaxed), o = m->decaf.rotOut[hand].load(std::memory_order_relaxed);
  if (!r) return false;
  rawDeg = r / 1000.f;
  outDeg = o / 1000.f;
  return true;
}

uint64_t Store::decafSmoothed() {
  fhlive::Shared* m = platform::mapLive();
  return m ? m->decaf.smoothed[0].load(std::memory_order_relaxed) + m->decaf.smoothed[1].load(std::memory_order_relaxed) : 0;
}

uint64_t Store::driverPoseUpdates() {
  fhlive::Shared* m = platform::mapLive();
  return m ? m->driver.poseUpdates.load(std::memory_order_relaxed) : 0;
}

uint64_t Store::driverHandUpdates() {
  fhlive::Shared* m = platform::mapLive();
  return m ? m->driver.handUpdates[0].load(std::memory_order_relaxed) + m->driver.handUpdates[1].load(std::memory_order_relaxed) : 0;
}

M Store::appliedDeviceOffset(int hand) {
  fhlive::Shared* m = platform::mapLive();
  if (!m) return ident();
  for (int tries = 0; tries < 8; ++tries) {
    uint32_t s0 = m->driver.appliedSeq.load(std::memory_order_acquire);
    if (s0 & 1) continue;
    fhlive::DeviceOffset o = m->driver.applied[hand];
    std::atomic_thread_fence(std::memory_order_acquire);
    if (m->driver.appliedSeq.load(std::memory_order_acquire) == s0) return fromPosQuat(o.pos, o.rot);
  }
  return ident();
}

void Store::askTruePoses(bool on) {
  fhlive::Shared* m = platform::mapLive();
  if (m) m->driver.wantTrue.store(on ? 1 : 0, std::memory_order_relaxed);
}

bool Store::inputReady() {
  fhlive::Shared* m = platform::mapLive();
  return m && m->input.magic.load(std::memory_order_acquire) == fhlive::kInputMagic;
}

uint64_t Store::inputReports() {
  fhlive::Shared* m = platform::mapLive();
  return m ? m->input.reports.load(std::memory_order_relaxed) : 0;
}

uint64_t Store::inputUnknown() {
  fhlive::Shared* m = platform::mapLive();
  return m ? m->input.unknownReports.load(std::memory_order_relaxed) : 0;
}

uint32_t Store::inputNowMs() {
  fhlive::Shared* m = platform::mapLive();
  return m ? m->input.nowMs.load(std::memory_order_relaxed) : 0;
}

uint64_t Store::inputTicks() {
  fhlive::Shared* m = platform::mapLive();
  return m ? m->input.ticks.load(std::memory_order_relaxed) : 0;
}

uint64_t Store::inputPlayed() {
  fhlive::Shared* m = platform::mapLive();
  return m ? m->input.macrosPlayed.load(std::memory_order_relaxed) : 0;
}

void Store::publishInput(const fhlive::InputTable& t) {
  fhlive::Shared* m = platform::mapLive();
  if (!m) return;
  if (!memcmp(&m->input.table, &t, sizeof t)) return;
  fhlive::seqWriteBegin(m->input.tableSeq);
  memcpy(&m->input.table, &t, sizeof t);
  fhlive::seqWriteEnd(m->input.tableSeq);
}

void Store::inputEnabled(bool on) {
  fhlive::Shared* m = platform::mapLive();
  if (m) m->input.enabled.store(on ? 1 : 0, std::memory_order_relaxed);
}

void Store::keepRecording() {
  fhlive::Shared* m = platform::mapLive();
  if (!m) return;
  // Never 0, which means off. A second is plenty: this comes round every 11 ms.
  const uint32_t until = m->input.nowMs.load(std::memory_order_relaxed) + 1000;
  m->input.recordUntilMs.store(until ? until : 1, std::memory_order_relaxed);
}

void Store::stopRecording() {
  fhlive::Shared* m = platform::mapLive();
  if (m) m->input.recordUntilMs.store(0, std::memory_order_relaxed);
}

void Store::recordingFromHere(uint32_t& cursor) {
  fhlive::Shared* m = platform::mapLive();
  cursor = m ? m->input.evWrite.load(std::memory_order_acquire) : 0;
}

uint64_t Store::recordingLost = 0;

size_t Store::readRecording(uint32_t& cursor, std::vector<fhlive::InputEvent*>& out) {
  out.clear();
  fhlive::Shared* m = platform::mapLive();
  if (!m) return 0;
  const uint32_t end = m->input.evWrite.load(std::memory_order_acquire);
  if (end - cursor > fhlive::kEventRing) {
    recordingLost += end - cursor - fhlive::kEventRing;
    cursor = end - fhlive::kEventRing;
  }
  while (cursor != end) {
    fhlive::InputEvent& e = m->input.events[cursor % fhlive::kEventRing];
    if (e.seq.load(std::memory_order_acquire) != cursor + 1) break;  // still being written: next time
    out.push_back(&e);
    ++cursor;
  }
  return out.size();
}

bool Store::driverTruePose(int hand, M* out) {
  fhlive::Shared* m = platform::mapLive();
  if (!m) return false;
  for (int tries = 0; tries < 8; ++tries) {
    uint32_t s0 = m->driver.trueSeq[hand].load(std::memory_order_acquire);
    if (!s0) return false;
    if (s0 & 1) continue;
    fhlive::DeviceOffset o = m->driver.truePose[hand];
    std::atomic_thread_fence(std::memory_order_acquire);
    if (m->driver.trueSeq[hand].load(std::memory_order_acquire) == s0) { *out = fromPosQuat(o.pos, o.rot); return true; }
  }
  return false;
}

}  // namespace fh

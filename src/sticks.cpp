#include "sticks.h"

#include <json/json.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sticks {

namespace fs = std::filesystem;

constexpr double kRestSeconds = 4;     // step 1: left alone this long
constexpr float kEdgeEnough = 0.95f;   // step 2: this much of the edge traced
constexpr size_t kTrail = 40;          // dots in the picture's trail
constexpr double kTrailEvery = 0.05;   // one every 50 ms
constexpr float kLearnedMax = 0.25f;   // how far learning while you play may ever take the middle from where the check found it

Drift::Drift(std::string dataDir) : file_(std::move(dataDir) + "/drift.json") { load(); }

static fs::path u8(const std::string& s) { return fs::u8path(s); }

void Drift::load() {
  std::ifstream f(u8(file_), std::ios::binary);
  if (!f) return;
  Json::Value root;
  Json::CharReaderBuilder b;
  std::string err;
  if (!Json::parseFromStream(b, f, &root, &err) || !root.isObject()) return;
  on_ = root.get("on", true).asBool();
  const Json::Value& cs = root["controllers"];
  if (!cs.isObject()) return;
  for (const std::string& serial : cs.getMemberNames()) {
    const Json::Value& c = cs[serial];
    if (!c.isObject()) continue;
    Saved s;
    s.drifts = c.get("drifts", false).asBool();
    s.touch = c.get("touch", false).asBool();
    s.verdict = c.get("verdict", "").asString();
    s.when = c.get("checked", "").asString();
    for (const Json::Value& l : c["lines"]) s.lines.push_back(l.asString());
    s.checkedX = c["rest"].get(0u, 0.0).asFloat();
    s.checkedY = c["rest"].get(1u, 0.0).asFloat();
    drift::Params p;
    p.on = s.drifts ? 1 : 0;
    p.cx = c["middle"].get(0u, s.checkedX).asFloat();
    p.cy = c["middle"].get(1u, s.checkedY).asFloat();
    p.dead = c.get("dead", 0.0).asFloat();
    p.gate = c.get("gate", 0.0).asFloat();
    p.learnPerSec = c.get("learn_per_s", 0.0).asFloat();
    p.learnMax = c.get("learn_max", 0.0).asFloat();
    for (Json::ArrayIndex k = 0; k < drift::kDirs; ++k) p.reach[k] = c["reach"].get(k, 0.0).asFloat();
    s.p = drift::sane(p);
    s.p.on = s.drifts ? 1 : 0;
    saved_[serial] = s;
  }
}

void Drift::save() const {
  Json::Value root;
  root["on"] = on_;
  Json::Value cs(Json::objectValue);
  for (const auto& [serial, s] : saved_) {
    Json::Value c;
    c["drifts"] = s.drifts;
    c["touch"] = s.touch;
    c["verdict"] = s.verdict;
    c["checked"] = s.when;
    Json::Value lines(Json::arrayValue);
    for (const std::string& l : s.lines) lines.append(l);
    c["lines"] = lines;
    // Rounded to a ten-thousandth: plenty for a stick, and easy to read.
    const auto r = [](float v) { return std::round(double(v) * 10000) / 10000; };
    c["rest"].append(r(s.checkedX));
    c["rest"].append(r(s.checkedY));
    c["middle"].append(r(s.p.cx));
    c["middle"].append(r(s.p.cy));
    c["dead"] = r(s.p.dead);
    c["gate"] = r(s.p.gate);
    c["learn_per_s"] = r(s.p.learnPerSec);
    c["learn_max"] = r(s.p.learnMax);
    for (int k = 0; k < drift::kDirs; ++k) c["reach"].append(r(s.p.reach[k]));
    cs[serial] = c;
  }
  root["controllers"] = cs;
  Json::StreamWriterBuilder w;
  w["indentation"] = "  ";
  std::error_code ec;
  fs::create_directories(u8(file_).parent_path(), ec);
  const fs::path tmp = u8(file_ + ".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << Json::writeString(w, root) << "\n";
    if (!f) return;
  }
  fs::rename(tmp, u8(file_), ec);
}

void Drift::say(const std::string& s, int kind) {
  status = s;
  statusKind = kind;
  ++statusSerial;
}

bool Drift::hidden() const {
#if !UTMTFTM_STICK_DRIFT
  return true;  // not in this build: the Frame's TMR sticks don't wear like this (CMakeLists)
#endif
  bool frame = false, other = false;
  for (const Hand& h : hands_) {
    if (h.serial.empty()) continue;
    (h.frame ? frame : other) = true;
  }
  return frame && !other;
}

void Drift::readRing(fhlive::DriftChannel* dc, int h, bool keep) {
  Hand& hd = hands_[h];
  const uint32_t w = dc->write[h].load(std::memory_order_acquire);
  if (w - hd.read > fhlive::kDriftRing) hd.read = w - fhlive::kDriftRing;  // fell behind: the oldest have been written over
  for (; hd.read != w; ++hd.read) {
    const fhlive::DriftSample& e = dc->ring[h][hd.read % fhlive::kDriftRing];
    if (e.seq.load(std::memory_order_acquire) != hd.read + 1) continue;  // still being written, or already written over
    const drift::Sample s{e.x, e.y, e.touch == 2 ? -1 : int(e.touch), e.ms / 1000.0};
    const drift::Dot out{e.ox, e.oy};
    std::atomic_thread_fence(std::memory_order_acquire);
    if (e.seq.load(std::memory_order_relaxed) != hd.read + 1) continue;
    if (!std::isfinite(s.x) || !std::isfinite(s.y)) continue;
    hd.raw = {s.x, s.y};
    hd.out = out;
    hd.touched = s.touch == 1;
    if (keep) hd.step.push_back(s);
    if (s.t - hd.trailAt >= kTrailEvery || s.t < hd.trailAt) {
      hd.trailAt = s.t;
      hd.trail.push_back(hd.raw);
      if (hd.trail.size() > kTrail) hd.trail.erase(hd.trail.begin());
    }
  }
}

static std::string textOf(const drift::Params& p) {
  char b[400];
  snprintf(b, sizeof b, "%u %.5f %.5f %.5f %.5f %.5f %.5f", p.on, p.cx, p.cy, p.dead, p.gate, p.learnPerSec, p.learnMax);
  std::string s = b;
  for (float r : p.reach) { snprintf(b, sizeof b, " %.5f", r); s += b; }
  return s;
}

void Drift::tick(fhlive::DriftChannel* dc, const std::string serial[2], const bool frame[2], bool showing, double now) {
  ready_ = dc && dc->magic.load(std::memory_order_acquire) == fhlive::kDriftMagic;
  for (int h = 0; h < 2; ++h) {
    Hand& hd = hands_[h];
    hd.frame = frame[h];
    if (hd.serial != serial[h]) {
      hd.serial = serial[h];
      hd.trail.clear();
      republish_ = true;
      if (step_ >= 1 && step_ <= 3) {
        step_ = 0;
        say("A controller changed, so the check stopped. Nothing changed.", 2);
      }
    }
    hd.found = ready_ ? dc->found[h].load(std::memory_order_relaxed) : 0;
    hd.touchBad = ready_ && dc->touchBad[h].load(std::memory_order_relaxed) != 0;
  }
  const bool measure = showing && ready_;
  if (measure != measuring_) {
    measuring_ = measure;
    if (ready_) dc->measure.store(measure ? 1 : 0, std::memory_order_relaxed);
    if (measure)
      for (int h = 0; h < 2; ++h) hands_[h].read = dc->write[h].load(std::memory_order_acquire);  // only from now on
  }
  if (measuring_)
    for (int h = 0; h < 2; ++h) readRing(dc, h, step_ >= 1 && step_ <= 3);
  if (step_ >= 1 && step_ <= 3) advance(now);

  // Where the middle has got to by learning while you play, kept for next time, now and then.
  if (ready_ && on_ && now - learnedAt_ > 60) {
    learnedAt_ = now;
    bool changed = false;
    for (int h = 0; h < 2; ++h) {
      auto it = saved_.find(hands_[h].serial);
      if (it == saved_.end() || !it->second.drifts || !it->second.touch) continue;
      float l[2] = {0, 0};
      bool got = false;
      for (int tries = 0; tries < 8 && !got; ++tries) {
        const uint32_t s0 = dc->learnSeq[h].load(std::memory_order_acquire);
        if (s0 & 1) continue;
        l[0] = dc->learned[h][0];
        l[1] = dc->learned[h][1];
        std::atomic_thread_fence(std::memory_order_acquire);
        got = dc->learnSeq[h].load(std::memory_order_acquire) == s0 && s0 != 0;
      }
      Saved& s = it->second;
      if (!got || !std::isfinite(l[0]) || !std::isfinite(l[1]) || drift::len(l[0] - s.p.cx, l[1] - s.p.cy) < 0.01f) continue;
      float dx = l[0] - s.checkedX, dy = l[1] - s.checkedY;
      const float d = drift::len(dx, dy);
      if (d > kLearnedMax) { dx *= kLearnedMax / d; dy *= kLearnedMax / d; }
      s.p.cx = s.checkedX + dx;
      s.p.cy = s.checkedY + dy;
      // The driver's already there by itself: what's saved following it isn't a change to send.
      drift::Params sent = s.p;
      sent.on = 1;
      published_[h] = textOf(sent);
      changed = true;
    }
    if (changed) save();  // not sent: the driver is already there
  }
  if (ready_) publish(dc);
}

void Drift::publish(fhlive::DriftChannel* dc) {
  drift::Params want[2];
  std::string text[2];
  bool changed = republish_;
  for (int h = 0; h < 2; ++h) {
    auto it = saved_.find(hands_[h].serial);
    if (on_ && !hands_[h].serial.empty() && it != saved_.end() && it->second.drifts) {
      want[h] = it->second.p;
      want[h].on = 1;
    }
    text[h] = textOf(want[h]);
    changed = changed || text[h] != published_[h];
  }
  if (!changed) return;
  republish_ = false;
  fhlive::seqWriteBegin(dc->seq);
  for (int h = 0; h < 2; ++h) {
    static_assert(sizeof(drift::Params) == sizeof(fhlive::DriftSettings), "the same layout");
    memcpy(&dc->want[h], &want[h], sizeof want[h]);
    published_[h] = text[h];
  }
  fhlive::seqWriteEnd(dc->seq);
}

void Drift::advance(double now) {
  bool all = true;
  for (int h = 0; h < 2; ++h) {
    Hand& hd = hands_[h];
    if (!present(h)) { hd.done = true; continue; }
    if (step_ == 1) {
      const double dt = hd.lastT < 0 ? 0 : std::min(now - hd.lastT, 0.5);
      hd.lastT = now;
      const bool heard = !hd.step.empty();
      // Moved a long way from where it's been resting: somebody pushed it. Start this stick again.
      bool pushed = false;
      if (hd.step.size() >= 20) {
        const drift::Rest r = drift::restOf(hd.step);
        pushed = r.pushed;
      }
      if (hd.touched || pushed) {
        hd.restSeconds = 0;
        hd.step.clear();
        hd.done = false;
      } else if (heard && !hd.done) {
        hd.restSeconds += dt;
        if (hd.restSeconds >= kRestSeconds) {
          hd.rest = drift::restOf(hd.step);
          if (!hd.rest.ok && !hd.rest.pushed && !hd.rest.touched) {
            // A controller that only reports a change, and a stick that didn't change: where it last said is where it rests.
            hd.rest = drift::Rest{};
            hd.rest.ok = true;
            hd.rest.x = hd.raw.x;
            hd.rest.y = hd.raw.y;
          }
          hd.done = hd.rest.ok;
          if (!hd.done) { hd.restSeconds = 0; hd.step.clear(); }
        }
      }
    } else if (step_ == 2) {
      hd.reach = drift::edgeOf(hd.step, hd.rest.x, hd.rest.y);
      hd.done = drift::edgeDone(hd.reach) >= kEdgeEnough;
    } else if (step_ == 3) {
      hd.landed = drift::landingsOf(hd.step, hd.rest.x, hd.rest.y);
      hd.done = drift::landingsDone(hd.landed) >= 1.f;
    }
    all = all && hd.done;
  }
  if (!all) return;
  if (step_ == 1 || step_ == 2) {
    for (Hand& hd : hands_) {
      if (step_ == 2) hd.edge = hd.step;
      hd.step.clear();
      hd.done = false;
    }
    ++step_;
    stepStart_ = now;
  } else {
    finish();
  }
}

void Drift::finish() {
  char date[16] = "";
  const std::time_t t = std::time(nullptr);
  if (const std::tm* tm = std::localtime(&t)) std::strftime(date, sizeof date, "%Y-%m-%d", tm);
  int drifting = 0, checked = 0;
  for (int h = 0; h < 2; ++h) {
    Hand& hd = hands_[h];
    if (!present(h) || hd.serial.empty()) continue;
    // A touch sensor that misses: pushed most of the way out with no thumb felt, in a good share of step 2.
    int out = 0, unfelt = 0;
    for (const drift::Sample& s : hd.edge) {
      if (drift::len(s.x - hd.rest.x, s.y - hd.rest.y) < 0.6f) continue;
      ++out;
      unfelt += s.touch == 0;
    }
    const bool hasTouch = (hd.found & 2) != 0;
    const bool missed = hasTouch && out > 20 && unfelt * 5 > out;
    hd.found_ = drift::analyse(hd.rest, hd.edge, hd.landed, hasTouch, missed);
    Saved s;
    s.p = hd.found_.p;
    s.checkedX = hd.rest.x;
    s.checkedY = hd.rest.y;
    s.drifts = hd.found_.drifts;
    s.touch = hd.found_.touchUsed;
    s.verdict = hd.found_.verdict;
    s.lines = hd.found_.lines;
    if (missed) {
      s.lines.push_back("Its touch sensor missed your");
      s.lines.push_back("thumb, so touch isn't used.");
    }
    s.when = date;
    saved_[hd.serial] = s;
    ++checked;
    drifting += s.drifts;
  }
  save();
  republish_ = true;
  step_ = 4;
  if (!checked) say("No sticks to check.", 2);
  else if (!drifting) say("Checked. Nothing to fix.", 1);
  else if (on_) say("Checked and fixed. It's saved for these controllers.", 1);
  else say("Checked. Fix drift is off, so the game still gets the drift: switch it on to fix it.", 2);
}

bool Drift::click(const std::string& id, double now) {
  if (id == "drift_check" || id == "drift_again") {
    if (!ready_) { say("The add-on SteamVR has loaded is older. Restart SteamVR once and it's ready.", 2); return true; }
    if (!present(0) && !present(1)) { say("No sticks found. Switch your controllers on, then check again.", 2); return true; }
    for (Hand& hd : hands_) {
      hd.step.clear();
      hd.edge.clear();
      hd.reach.clear();
      hd.landed.clear();
      hd.restSeconds = 0;
      hd.lastT = -1;
      hd.done = false;
      hd.rest = drift::Rest{};
    }
    step_ = 1;
    stepStart_ = now;
    status.clear();
    ++statusSerial;
    return true;
  }
  if (id == "drift_cancel") {
    step_ = 0;
    say("Cancelled. Nothing changed.", 0);
    return true;
  }
  if (id == "drift_skip") {
    if (step_ == 2) {
      for (Hand& hd : hands_) { hd.edge.clear(); hd.reach.clear(); hd.step.clear(); hd.done = false; }
      step_ = 3;
      stepStart_ = now;
    } else if (step_ == 3) {
      finish();
    }
    return true;
  }
  if (id == "drift_done") {
    step_ = 0;
    return true;
  }
  if (id == "drift_on" || id == "drift_off") {
    on_ = id == "drift_on";
    save();
    republish_ = true;
    say(on_ ? "Fixing drift. The game sees the middle when you let go." : "Drift fix off. The game gets your sticks exactly as they are.", on_ ? 1 : 0);
    return true;
  }
  return false;
}

void Drift::leave() {
  if (step_ != 0) step_ = 0;
}

fh::DriftModel Drift::model() const {
  fh::DriftModel m;
  m.step = step_;
  m.fixOn = on_;
  m.ready = ready_;
  double least = kRestSeconds;
  for (int h = 0; h < 2; ++h) {
    const Hand& hd = hands_[h];
    fh::DriftStick& s = m.sticks[h];
    s.present = ready_ ? present(h) : !hd.serial.empty();
    s.hasTouch = (hd.found & 2) != 0;
    s.touched = hd.touched;
    s.raw = {hd.raw.x, hd.raw.y};
    s.out = {hd.out.x, hd.out.y};
    for (const drift::Dot& d : hd.trail) s.trail.push_back({d.x, d.y});
    auto it = saved_.find(hd.serial);
    if (it != saved_.end() && !hd.serial.empty()) {
      const Saved& sv = it->second;
      s.checked = true;
      s.drifts = sv.drifts;
      s.verdict = sv.verdict;
      s.verdictWarn = sv.drifts;
      s.lines = sv.lines;
      if (sv.drifts) {
        s.rest = {sv.p.cx, sv.p.cy};
        s.dead = sv.p.dead;
      }
      if (hd.touchBad) {
        s.lines.push_back("Its touch sensor missed your");
        s.lines.push_back("thumb, so touch isn't used now.");
      }
    }
    if (step_ >= 1 && step_ <= 3 && s.present) {
      if (step_ == 1) {
        s.progress = hd.done ? 1.f : float(std::min(1.0, hd.restSeconds / kRestSeconds));
        s.countdown = hd.done ? 0 : int(std::ceil(kRestSeconds - hd.restSeconds - 1e-6));
        least = std::min(least, hd.done ? kRestSeconds : hd.restSeconds);
        s.trail.clear();
        for (size_t i = hd.step.size() > 400 ? hd.step.size() - 400 : 0; i < hd.step.size(); i += 10) s.trail.push_back({hd.step[i].x, hd.step[i].y});
      } else if (step_ == 2) {
        s.reach = hd.reach.empty() ? std::vector<float>(drift::kSlices, 0.f) : hd.reach;
        s.progress = std::min(1.f, drift::edgeDone(s.reach) / kEdgeEnough);
        s.trail.clear();
      } else {
        for (const drift::Landing& l : hd.landed) s.landed.push_back({l.x, l.y});
        s.progress = drift::landingsDone(hd.landed);
      }
    }
    if (step_ == 4 && s.present) {
      for (const drift::Landing& l : hd.landed) s.landed.push_back({l.x, l.y});
      s.trail.clear();
    }
  }
  if (step_ == 1) m.countdown = int(std::ceil(kRestSeconds - least - 1e-6));
  return m;
}

}  // namespace sticks

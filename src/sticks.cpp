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
// Draft B (0.6.4): the stick's picture also marks where it jumps. False: draft A, the two lines of text only. J4 picked A.
constexpr bool kFlickMarks = false;
// Say how many times it jumped, not where from (where they start moves about from one go round to the next), and say along
// the bottom that the ring shows every jump and the solid dot is what games get. J4 picked this over the page as it was,
// 20 November ("New wording"). False: the page as it was.
constexpr bool kFlickWords = true;

std::vector<std::string> flickWords(const std::vector<std::string>& lines, int flicks) {
  std::vector<std::string> out;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    if (l.rfind("Jumps at ", 0) == 0) {
      if (l.back() == ':' && i + 1 < lines.size()) ++i;  // its list of ways, on the next line
      out.push_back("Jumps away and straight back,");
      out.push_back(flicks == 1 ? "once in this check." : std::to_string(flicks) + " times in this check.");
      continue;
    }
    out.push_back(l);
  }
  return out;
}

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
    // 0.6.4's flicks. A 0.6.3 file has none, and reads as a stick that doesn't flick.
    for (Json::ArrayIndex k = 0; k < drift::kDirs; ++k) s.reachSeen[k] = c["reach_seen"].get(k, 0.0).asFloat();
    const Json::Value& fm = c["flick_map"];
    if (fm.isObject()) {
      // 0.6.4's first test build saved a map of where they started instead of "hold": a stick it mapped has them held back
      // now, without a new check. How is the program's to say, not the file's.
      const bool hold = fm.isMember("hold") ? fm["hold"].asBool() : fm["cells"].isArray() && fm["cells"].size() > 0;
      if (hold && s.drifts) s.fl = drift::holdFlicks();
      s.flicks = fm.get("seen", 0).asInt();
      for (const Json::Value& d : fm["from"])
        if (d.isArray()) s.flickAt.push_back({d.get(0u, 0.0).asFloat(), d.get(1u, 0.0).asFloat()});
    }
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
    for (int k = 0; k < drift::kDirs; ++k) c["reach_seen"].append(r(s.reachSeen[k]));
    if (drift::any(s.fl) || s.flicks > 0) {
      // Whether its flicks are held back, how many the check saw, and where they started (for the picture).
      Json::Value fm, from(Json::arrayValue);
      fm["hold"] = drift::any(s.fl);
      fm["seen"] = s.flicks;
      for (const drift::Dot& d : s.flickAt) {
        Json::Value xy(Json::arrayValue);
        xy.append(r(d.x));
        xy.append(r(d.y));
        from.append(xy);
      }
      fm["from"] = from;
      c["flick_map"] = fm;
    }
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

static std::string textOf(const drift::Params& p, const drift::Flicks& f) {
  char b[400];
  snprintf(b, sizeof b, "%u %.5f %.5f %.5f %.5f %.5f %.5f", p.on, p.cx, p.cy, p.dead, p.gate, p.learnPerSec, p.learnMax);
  std::string s = b;
  for (float r : p.reach) { snprintf(b, sizeof b, " %.5f", r); s += b; }
  snprintf(b, sizeof b, " | %u %.4f %.4f %.3f %.4f %.4f", f.on, f.big, f.out, f.most, f.stay, f.back);
  return s + b;
}

void Drift::tick(fhlive::DriftChannel* dc, const std::string serial[2], const bool frame[2], bool showing, double now) {
  // 0.6.4's driver also holds flicks back. A 0.6.3 one still loaded can't: SteamVR needs a restart, like an older one.
  ready_ = dc && dc->magic.load(std::memory_order_acquire) == fhlive::kDriftMagic && dc->flickMagic.load(std::memory_order_acquire) == fhlive::kFlickMagic;
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
      published_[h] = textOf(sent, s.fl);
      changed = true;
    }
    if (changed) save();  // not sent: the driver is already there
  }
  if (ready_) publish(dc);
}

void Drift::publish(fhlive::DriftChannel* dc) {
  drift::Params want[2];
  drift::Flicks flick[2];
  std::string text[2];
  bool changed = republish_;
  for (int h = 0; h < 2; ++h) {
    auto it = saved_.find(hands_[h].serial);
    if (on_ && !hands_[h].serial.empty() && it != saved_.end() && it->second.drifts) {
      want[h] = it->second.p;
      want[h].on = 1;
      flick[h] = it->second.fl;
    }
    text[h] = textOf(want[h], flick[h]);
    changed = changed || text[h] != published_[h];
  }
  if (!changed) return;
  republish_ = false;
  fhlive::seqWriteBegin(dc->seq);
  for (int h = 0; h < 2; ++h) {
    static_assert(sizeof(drift::Params) == sizeof(fhlive::DriftSettings), "the same layout");
    static_assert(sizeof(drift::Flicks) == sizeof(fhlive::FlickSettings), "the same layout");
    memcpy(&dc->want[h], &want[h], sizeof want[h]);
    memcpy(&dc->flick[h], &flick[h], sizeof flick[h]);
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
      if (step_ == 1) hd.restSamples = hd.step;
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
    drift::CheckData d;
    d.rest = hd.rest;
    d.edge = hd.edge;
    d.flick = hd.step;  // step 3's, still there: nothing clears them till the next check
    d.landed = hd.landed;
    d.hasTouch = hasTouch;
    d.touchMissed = missed;
    // A shortfall the last check of this controller saw too counts as sure.
    if (auto was = saved_.find(hd.serial); was != saved_.end())
      for (int k = 0; k < drift::kDirs; ++k) d.prevReach[k] = was->second.reachSeen[k];
    hd.found_ = drift::analyse(d);
    Saved s;
    s.p = hd.found_.p;
    s.fl = hd.found_.fl;
    s.flicks = int(hd.found_.flicks.size());
    for (const drift::Flick& k : hd.found_.flicks)
      if (s.flickAt.size() < 24) s.flickAt.push_back({k.fromX, k.fromY});
    for (int k = 0; k < drift::kDirs; ++k) s.reachSeen[k] = hd.found_.reachSeen[k];
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
  char stamp[32] = "";
  if (const std::tm* tm = std::localtime(&t)) std::strftime(stamp, sizeof stamp, "%Y-%m-%d-%H%M%S", tm);
  writeSamples(stamp);
  save();
  republish_ = true;
  step_ = 4;
  if (!checked) say("No sticks to check.", 2);
  else if (!drifting) say("Checked. Nothing to fix.", 1);
  else if (on_) say("Checked and fixed. It's saved for these controllers.", 1);
  else say("Checked. Fix drift is off, so the game still gets the drift: switch it on to fix it.", 2);
}

// Everything the sticks said during the check, in a small file in the data folder (0.6.4, so a real worn stick gives real
// numbers to tune from). Only the check's own samples; the five newest checks are kept.
void Drift::writeSamples(const std::string& stamp) {
  constexpr size_t kMostLines = 150000;  // about 5 MB at most, even from a controller that reports a thousand times a second
  constexpr int kKeep = 5;
  std::error_code ec;
  const fs::path dir = u8(file_).parent_path() / "drift-checks";
  fs::create_directories(dir, ec);
  const fs::path path = dir / u8("check-" + stamp + ".csv");
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return;
  f << "# UTMTFTM " << UTMTFTM_VERSION << " stick check. step 1 left alone, 2 round the edge, 3 flick and let go. touch: 1 thumb on, 0 off, -1 can't tell\n";
  for (int h = 0; h < 2; ++h) {
    const Hand& hd = hands_[h];
    if (hd.serial.empty() || !present(h)) continue;
    const drift::Findings& fd = hd.found_;
    f << "# " << (h ? "right " : "left ") << hd.serial << ": " << fd.verdict << ", rests " << hd.rest.x << " " << hd.rest.y << ", flicks " << fd.flicks.size()
      << ", reports every " << int(std::lround(fd.gap * 1000)) << " ms\n";
    for (const drift::Flick& k : fd.flicks)
      f << "#   flick from " << k.fromX << " " << k.fromY << " to " << k.toX << " " << k.toY << ", " << k.size << " out, back in " << k.ms << " ms\n";
  }
  f << "hand,step,t,x,y,touch\n";
  size_t lines = 0;
  char b[96];
  for (int h = 0; h < 2; ++h) {
    const Hand& hd = hands_[h];
    const std::vector<drift::Sample>* steps[3] = {&hd.restSamples, &hd.edge, &hd.step};
    for (int st = 0; st < 3; ++st)
      for (const drift::Sample& sm : *steps[st]) {
        if (++lines > kMostLines) break;
        snprintf(b, sizeof b, "%c,%d,%.4f,%.5f,%.5f,%d\n", h ? 'R' : 'L', st + 1, sm.t, sm.x, sm.y, sm.touch);
        f << b;
      }
  }
  f.close();
  lastSamples = path.u8string();
  // Only the newest few.
  std::vector<fs::path> all;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.path().filename().u8string().rfind("check-", 0) == 0) all.push_back(e.path());
  std::sort(all.begin(), all.end());
  for (size_t i = 0; i + kKeep < all.size(); ++i) fs::remove(all[i], ec);
}

bool Drift::click(const std::string& id, double now) {
  if (id == "drift_check" || id == "drift_again") {
    if (!ready_) { say("The add-on SteamVR has loaded is older. Restart SteamVR once and it's ready.", 2); return true; }
    if (!present(0) && !present(1)) { say("No sticks found. Switch your controllers on, then check again.", 2); return true; }
    for (Hand& hd : hands_) {
      hd.step.clear();
      hd.restSamples.clear();
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
  m.flickMarks = kFlickMarks;
  m.explainJumps = kFlickWords;
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
      s.lines = kFlickWords && sv.flicks > 0 ? flickWords(sv.lines, sv.flicks) : sv.lines;
      if (sv.drifts) {
        s.rest = {sv.p.cx, sv.p.cy};
        s.dead = sv.p.dead;
        for (const drift::Dot& d : sv.flickAt) s.flickAt.push_back({d.x, d.y});
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

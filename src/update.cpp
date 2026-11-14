#include "update.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef UTMTFTM_VERSION
#define UTMTFTM_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;

namespace fh {
namespace update {

const char* thisVersion() { return UTMTFTM_VERSION; }

// ---------------------------------------------------------------- versions
static std::string bare(std::string v) {
  while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(v.begin());
  return v;
}

int compareVersions(const std::string& a, const std::string& b) {
  const std::string x = bare(a), y = bare(b);
  const size_t xd = x.find('-'), yd = y.find('-');
  const std::string xc = x.substr(0, xd), yc = y.substr(0, yd);
  const std::string xp = xd == std::string::npos ? "" : x.substr(xd + 1), yp = yd == std::string::npos ? "" : y.substr(yd + 1);
  std::istringstream xs(xc), ys(yc);
  for (int i = 0; i < 4; ++i) {
    std::string xn, yn;
    std::getline(xs, xn, '.');
    std::getline(ys, yn, '.');
    const long xv = xn.empty() ? 0 : std::strtol(xn.c_str(), nullptr, 10), yv = yn.empty() ? 0 : std::strtol(yn.c_str(), nullptr, 10);
    if (xv != yv) return xv < yv ? -1 : 1;
  }
  if (xp == yp) return 0;
  if (xp.empty()) return 1;  // a release comes after its own test builds
  if (yp.empty()) return -1;
  return xp < yp ? -1 : 1;
}

// ---------------------------------------------------------------- what GitHub says
static std::string trimmed(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return s.substr(i);
}

// The first paragraph, as words: no **, no `, one line, and no hidden comments.
static std::string firstParagraph(std::string body) {
  for (size_t at = body.find("<!--"); at != std::string::npos; at = body.find("<!--")) {
    const size_t end = body.find("-->", at);
    body.erase(at, end == std::string::npos ? std::string::npos : end + 3 - at);
  }
  std::string text;
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] == '\r') continue;
    text += body[i];
  }
  text = trimmed(text);
  const size_t para = text.find("\n\n");
  if (para != std::string::npos) text = text.substr(0, para);
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '*' || ch == '`' || ch == '#' || ch == '_') continue;
    out += ch == '\n' ? ' ' : ch;
  }
  return trimmed(out);
}

bool parseRelease(const std::string& json, Release& out) {
  Json::Value v;
  Json::CharReaderBuilder b;
  std::string errs;
  std::istringstream in(json);
  if (!Json::parseFromStream(b, in, &v, &errs) || !v.isObject() || !v.isMember("tag_name")) return false;
  if (v.get("draft", false).asBool() || v.get("prerelease", false).asBool()) return false;
  out = Release{};
  out.version = bare(v["tag_name"].asString());
  if (out.version.empty() || !std::isdigit(static_cast<unsigned char>(out.version[0]))) return false;
  out.title = v.get("name", "").asString();
  out.page = v.get("html_url", "").asString();
  const std::string body = v.get("body", "").asString();
  out.summary = firstParagraph(body);
  const std::string tag = "<!-- utmtftm-min-ok:";
  const size_t at = body.find(tag);
  if (at != std::string::npos) {
    const size_t end = body.find("-->", at);
    const std::string line = body.substr(at + tag.size(), end == std::string::npos ? std::string::npos : end - at - tag.size());
    const size_t bar = line.find('|');
    out.minOk = bare(trimmed(line.substr(0, bar)));
    if (bar != std::string::npos) out.why = trimmed(line.substr(bar + 1));
  }
  for (const Json::Value& a : v["assets"]) {
    Asset x;
    x.name = a.get("name", "").asString();
    x.url = a.get("browser_download_url", "").asString();
    x.size = a.get("size", 0).asUInt64();
    const std::string digest = a.get("digest", "").asString();
    if (digest.rfind("sha256:", 0) == 0) x.sha256 = digest.substr(7);
    std::transform(x.sha256.begin(), x.sha256.end(), x.sha256.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (!x.name.empty() && x.url.rfind("https://", 0) == 0) out.assets.push_back(x);
  }
  return true;
}

const Asset* assetFor(const Release& r, bool windows) {
  const std::string want = windows ? "UTMTFTM-Setup-" + r.version + ".exe" : "install-frame.sh";
  for (const Asset& a : r.assets)
    if (a.name == want) return &a;
  return nullptr;
}

// ---------------------------------------------------------------- SHA-256 (FIPS 180-4)
std::string sha256Hex(const std::string& bytes) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
      0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
      0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
      0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
      0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
      0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  std::string msg = bytes;
  const uint64_t bits = uint64_t(bytes.size()) * 8;
  msg += char(0x80);
  while (msg.size() % 64 != 56) msg += char(0);
  for (int i = 7; i >= 0; --i) msg += char((bits >> (i * 8)) & 0xff);
  for (size_t off = 0; off < msg.size(); off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(msg.data()) + off + i * 4;
      w[i] = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (uint32_t x : h)
    for (int i = 28; i >= 0; i -= 4) out += hex[(x >> i) & 0xf];
  return out;
}

// ---------------------------------------------------------------- the updater
static std::string readAll(const std::string& file) {
  std::ifstream in(fs::u8path(file), std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

constexpr uint64_t kEveryMs = 6ull * 60 * 60 * 1000;  // a long session still hears about a fix the same day

Updater::Updater(std::string dataDir) : dir_(std::move(dataDir)) {
#ifdef _WIN32
  windows = true;
#else
  windows = false;
#endif
  apiUrl = std::string("https://api.github.com/repos/") + kRepo + "/releases/latest";
  fetch = [](const std::string& url, const std::string& file) {
    const std::string curl = curlPath();
    if (curl.empty()) return false;
    std::error_code ec;
    fs::remove(fs::u8path(file), ec);
    const std::string agent = std::string("UTMTFTM/") + thisVersion();
    return runHidden({curl, "-fsSL", "--max-time", "120", "--retry", "2", "-A", agent, "-H", "Accept: application/vnd.github+json", "-o", file, url}, 150000) == 0;
  };
  start = [](const std::string& file, bool win) {
    // Windows: the installer puts the new files in beside the running ones (/UPDATE leaves SteamVR alone). The Frame: its
    // install script fetches and checks the build itself, then puts it in the same way (--update).
    if (win) return runHidden({file, "/S", "/UPDATE"}, 180000) == 0;
    return runHidden({"/bin/sh", file, "--update"}, 300000) == 0;
  };
  const char* off = std::getenv("UTMTFTM_NO_UPDATE_CHECK");
  if (off && off[0] && off[0] != '0') s_.state = State::Off;
}

Updater::~Updater() { join(); }

void Updater::join() {
  if (worker_.joinable()) worker_.join();
}

void Updater::set(const std::function<void(Status&)>& change) {
  std::lock_guard<std::mutex> l(m_);
  change(s_);
  ++s_.serial;
}

Status Updater::status() const {
  std::lock_guard<std::mutex> l(m_);
  return s_;
}

void Updater::tick(uint64_t nowMs) {
  if (status().state == State::Off || busy_) return;
  if (checked_ && nowMs - lastCheck_ < kEveryMs) return;
  checked_ = true;
  lastCheck_ = nowMs;
  checkNow();
}

void Updater::checkNow() {
  // Installed: there's nothing more to look for until SteamVR restarts with the new one.
  const State now = status().state;
  if (now == State::Off || now == State::Installed || busy_.exchange(true)) return;
  join();
  set([](Status& s) { s.state = State::Checking; s.error.clear(); });
  worker_ = std::thread([this] {
    std::error_code ec;
    fs::create_directories(fs::u8path(dir_) / "update", ec);
    const std::string file = (fs::u8path(dir_) / "update" / "latest.json").u8string();
    Release r;
    const bool got = fetch(apiUrl, file) && parseRelease(readAll(file), r);
    set([&](Status& s) {
      if (s.state == State::Installed) return;  // already done; a later check doesn't take that back
      if (!got) {
        s.state = State::Failed;
        s.error = "Couldn't reach GitHub to check for a newer version.";
        return;
      }
      s.latest = r;
      s.needed = !r.minOk.empty() && compareVersions(thisVersion(), r.minOk) < 0;
      s.state = compareVersions(thisVersion(), r.version) < 0 ? State::Available : State::UpToDate;
    });
    busy_ = false;
  });
}

void Updater::install() {
  const Status now = status();
  if (now.state != State::Available || busy_.exchange(true)) return;
  join();
  set([](Status& s) { s.state = State::Downloading; s.error.clear(); });
  worker_ = std::thread([this, now] {
    const auto fail = [&](const std::string& why) {
      set([&](Status& s) { s.state = State::Available; s.error = why; });
      busy_ = false;
    };
    const Asset* a = assetFor(now.latest, windows);
    if (!a) return fail("This release has no " + std::string(windows ? "Windows installer" : "Steam Frame build") + " yet. Try again in a few minutes.");
    std::error_code ec;
    fs::create_directories(fs::u8path(dir_) / "update", ec);
    const std::string file = (fs::u8path(dir_) / "update" / a->name).u8string();
    if (!fetch(a->url, file)) return fail("The download didn't finish. Check the connection and try again.");
    // Exactly the file GitHub lists, or nothing gets started.
    std::string want = a->sha256;
    if (want.empty())
      for (const Asset& s : now.latest.assets)
        if (s.name == a->name + ".sha256" && fetch(s.url, file + ".sha256")) want = readAll(file + ".sha256").substr(0, 64);
    const std::string got = sha256Hex(readAll(file));
    if (want.size() != 64 || got != want) {
      fs::remove(fs::u8path(file), ec);
      return fail("The download didn't match what GitHub lists, so it wasn't installed. Try again.");
    }
    set([](Status& s) { s.state = State::Installing; });
    if (!start(file, windows)) return fail("The installer stopped before it finished. Try again, or get it from the GitHub page.");
    set([](Status& s) { s.state = State::Installed; });
    busy_ = false;
  });
}

}  // namespace update
}  // namespace fh

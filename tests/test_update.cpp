// The updater, with no network: versions, GitHub's real answer for v0.6.0, the checksum, and a whole check and install
// against a pretend GitHub. UTMTFTM_TEST_NET=1 also asks the real GitHub, with the real curl.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "update.h"

namespace fs = std::filesystem;
using namespace fh::update;

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

static std::string readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}
static void writeAll(const fs::path& p, const std::string& s) {
  std::ofstream out(p, std::ios::binary);
  out << s;
}

static Status waitFor(Updater& u, State not1, State not2 = State::Off) {
  for (int i = 0; i < 400; ++i) {
    const Status s = u.status();
    if (s.state != not1 && s.state != not2) return s;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return u.status();
}

// A release answer with one Windows installer and one Frame script, whose contents and checksums are given.
static std::string fakeRelease(const std::string& version, const std::string& exeSha, const std::string& shSha, const std::string& notes) {
  std::string body = notes;
  for (size_t at = body.find('"'); at != std::string::npos; at = body.find('"', at + 2)) body.insert(at, "\\");
  return "{\"tag_name\": \"v" + version + "\", \"name\": \"UTMTFTM v" + version + "\", \"draft\": false, \"prerelease\": false, " +
         "\"html_url\": \"https://github.com/RxVreality/UTMTFTM/releases/tag/v" + version + "\", \"body\": \"" + body + "\", \"assets\": [" +
         "{\"name\": \"UTMTFTM-Setup-" + version + ".exe\", \"size\": 5, \"digest\": \"sha256:" + exeSha + "\", \"browser_download_url\": \"https://example.invalid/setup.exe\"}," +
         "{\"name\": \"install-frame.sh\", \"size\": 5, \"digest\": \"sha256:" + shSha + "\", \"browser_download_url\": \"https://example.invalid/install-frame.sh\"}]}";
}

int main() {
  // ---- versions
  check(compareVersions("0.6.0", "0.6.1") < 0 && compareVersions("v0.6.1", "0.6.1") == 0 && compareVersions("0.10.0", "0.9.9") > 0, "versions compare by number, v or not");
  check(compareVersions("0.6.1-test", "0.6.1") < 0 && compareVersions("0.6.1-test", "0.6.0") > 0, "a test build comes after the last release and before its own");
  check(compareVersions("1.0", "1.0.0") == 0, "a missing part is a nought");

  // ---- the checksum, against the standard's own examples
  check(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
  check(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
  check(sha256Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 of a million a's");
  check(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 across two blocks");

  // ---- GitHub's real answer for v0.6.0
  const std::string here = fs::path(__FILE__).parent_path().u8string();
  Release r;
  check(parseRelease(readAll(fs::u8path(here) / "data" / "github-release-0.6.0.json"), r), "GitHub's answer for v0.6.0 reads");
  check(r.version == "0.6.0" && r.title == "UTMTFTM v0.6.0" && r.page.find("/releases/tag/v0.6.0") != std::string::npos, "its version, title and page: " + r.version);
  check(r.summary.rfind("The Bindings and Combos tabs now start with a simple mode", 0) == 0 && r.summary.find("**") == std::string::npos, "its first paragraph, as words: " + r.summary.substr(0, 60));
  const Asset* win = assetFor(r, true);
  const Asset* frame = assetFor(r, false);
  check(win && win->name == "UTMTFTM-Setup-0.6.0.exe" && win->sha256 == "6cada15dfb6c6a4c0d09edd75792106269b13ca80909359ceeb257d9ee204bd8", "the Windows installer, with GitHub's checksum");
  check(frame && frame->name == "install-frame.sh" && frame->url.rfind("https://github.com/", 0) == 0, "and the Frame's install script");
  check(r.minOk.empty(), "0.6.0 doesn't say anything older is broken");

  // The hidden line that says older versions may not work.
  Release marked;
  check(parseRelease(fakeRelease("0.7.0", std::string(64, '0'), std::string(64, '0'), "New things.\n\nMore.\n<!-- utmtftm-min-ok: 0.6.9 | SteamVR changed how bindings reload -->"), marked), "a release with the line reads");
  check(marked.minOk == "0.6.9" && marked.why == "SteamVR changed how bindings reload", "and the line says from which version and why: " + marked.minOk + " / " + marked.why);
  check(marked.summary == "New things.", "the line isn't part of the summary");
  Release draft;
  check(!parseRelease("{\"tag_name\": \"v9.0.0\", \"draft\": true}", draft) && !parseRelease("not json", draft), "drafts and nonsense aren't releases");

  // ---- a whole check and install, against a pretend GitHub
  const fs::path work = fs::temp_directory_path() / "utmtftm-test-update";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  const std::string exe = "MZ new installer", sh = "#!/bin/sh\necho new\n";
  std::string answer = fakeRelease("9.0.0", sha256Hex(exe), sha256Hex(sh), "**Fixes** for PC VR.\n\nAnd more.");
  int asked = 0;
  std::string started;
  {
    Updater u(work.u8string());
    u.fetch = [&](const std::string& url, const std::string& file) {
      ++asked;
      if (url == u.apiUrl) writeAll(fs::u8path(file), answer);
      else if (url.find("setup.exe") != std::string::npos) writeAll(fs::u8path(file), exe);
      else if (url.find("install-frame.sh") != std::string::npos) writeAll(fs::u8path(file), sh);
      else return false;
      return true;
    };
    u.start = [&](const std::string& file, bool) { started = file; return true; };
    u.windows = true;
    u.tick(1000);
    Status s = waitFor(u, State::Idle, State::Checking);
    check(s.state == State::Available && s.latest.version == "9.0.0" && !s.needed, "a newer version is found");
    check(s.latest.summary == "Fixes for PC VR.", "with its first paragraph: " + s.latest.summary);
    u.tick(2000);
    check(asked == 1, "and it doesn't ask again straight away");
    u.install();
    s = waitFor(u, State::Available, State::Downloading);
    s = waitFor(u, State::Installing);
    check(s.state == State::Installed && fs::path(fs::u8path(started)).filename() == "UTMTFTM-Setup-9.0.0.exe", "it fetches the installer, checks it and starts it: " + started);
    u.tick(1000 + 7ull * 60 * 60 * 1000);
    s = waitFor(u, State::Checking);
    check(s.state == State::Installed, "a later check doesn't take back that it's installed");
  }
  {
    // A download that isn't what GitHub lists never starts.
    Updater u(work.u8string());
    started.clear();
    u.fetch = [&](const std::string& url, const std::string& file) {
      writeAll(fs::u8path(file), url == u.apiUrl ? answer : std::string("tampered"));
      return true;
    };
    u.start = [&](const std::string& file, bool) { started = file; return true; };
    u.windows = false;
    u.checkNow();
    waitFor(u, State::Idle, State::Checking);
    u.install();
    Status s = waitFor(u, State::Available);
    s = waitFor(u, State::Downloading, State::Installing);
    check(s.state == State::Available && started.empty() && s.error.find("didn't match") != std::string::npos, "a download that doesn't match isn't started: " + s.error);
  }
  {
    // Up to date, and a release that says this version may not work.
    Updater u(work.u8string());
    u.fetch = [&](const std::string&, const std::string& file) { writeAll(fs::u8path(file), fakeRelease(thisVersion(), std::string(64, '0'), std::string(64, '0'), "Same.")); return true; };
    u.checkNow();
    Status s = waitFor(u, State::Idle, State::Checking);
    check(s.state == State::UpToDate, std::string("this version, ") + thisVersion() + ", is up to date");
    u.fetch = [&](const std::string&, const std::string& file) {
      writeAll(fs::u8path(file), fakeRelease("99.0.0", std::string(64, '0'), std::string(64, '0'), "Big change.\n<!-- utmtftm-min-ok: 99.0.0 | SteamVR changed -->"));
      return true;
    };
    u.checkNow();
    s = waitFor(u, State::UpToDate, State::Checking);
    check(s.state == State::Available && s.needed && s.latest.why == "SteamVR changed", "one that says older versions may not work makes it needed");
    u.fetch = [&](const std::string&, const std::string&) { return false; };
    u.checkNow();
    s = waitFor(u, State::Available, State::Checking);
    check(s.state == State::Failed && !s.error.empty(), "no connection says so: " + s.error);
  }

  {
    // A check that couldn't reach GitHub tries again soon, because a Frame starts UTMTFTM before its Wi-Fi is back (J4's,
    // 14 November), then every six hours again once one gets through. Each tick waits for the check it started to finish.
    Updater u(work.u8string());
    int asked = 0;
    bool online = false;
    u.fetch = [&](const std::string&, const std::string& file) {
      ++asked;
      if (!online) return false;
      writeAll(fs::u8path(file), fakeRelease(thisVersion(), std::string(64, '0'), std::string(64, '0'), "Same."));
      return true;
    };
    const auto tickAt = [&](uint64_t ms) {
      u.tick(ms);
      for (int i = 0; i < 400 && u.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return u.status();
    };
    const uint64_t t0 = 1000, minute = 60000;
    Status s = tickAt(t0);
    check(s.state == State::Failed && asked == 1, "the first check, with no network yet, fails");
    tickAt(t0 + 30000);
    check(asked == 1, "it doesn't ask again within half a minute");
    s = tickAt(t0 + minute + 1);
    check(asked == 2 && s.state == State::Failed, "it asks again a minute later");
    tickAt(t0 + minute + 1 + 100000);
    check(asked == 2, "after two failures it waits two minutes");
    online = true;
    const uint64_t t1 = t0 + minute + 1 + 2 * minute + 1;
    s = tickAt(t1);
    check(asked == 3 && s.state == State::UpToDate, "and gets through once the network's up");
    for (uint64_t m = 1; m <= 60; ++m) tickAt(t1 + m * minute);
    check(asked == 3, "after that it doesn't retry every minute");
    tickAt(t1 + 6ull * 60 * minute + 1);
    check(asked == 4, "but checks again six hours later");
    // Offline for good from here: a minute, 2, 5, 15, then every 30 minutes. Over three hours of ticks a minute apart that's
    // asks at 0, 1, 3, 8, 23, 53, 83, 113, 143 and 173 minutes.
    online = false;
    const uint64_t t2 = t1 + 12ull * 60 * minute + 2;
    const int before = asked;
    for (uint64_t m = 0; m <= 180; ++m) tickAt(t2 + m * minute);
    check(asked - before == 10, "offline for three hours it asks 10 times, never more often than every half hour after the first few: " + std::to_string(asked - before));
  }

  // ---- the real thing, when asked
  const char* net = std::getenv("UTMTFTM_TEST_NET");
  if (net && net[0] == '1') {
    check(!curlPath().empty(), "this machine has curl: " + curlPath());
    Updater u(work.u8string());
    u.checkNow();
    Status s = waitFor(u, State::Idle, State::Checking);
    std::printf("real GitHub: state %d, latest %s, %s\n", int(s.state), s.latest.version.c_str(), s.error.c_str());
    check(s.state == State::Available || s.state == State::UpToDate, "the real GitHub answers");
    check(assetFor(s.latest, true) && assetFor(s.latest, false), "and its latest release has both installers");
    // The real installer, fetched by the real curl, checked by this checksum against what GitHub lists for it.
    if (const Asset* a = assetFor(s.latest, true)) {
      const std::string file = (work / a->name).u8string();
      check(u.fetch(a->url, file), "the real Windows installer downloads");
      const std::string got = sha256Hex(readAll(fs::u8path(file)));
      std::printf("real installer: %zu bytes, sha256 %s, GitHub says %s\n", readAll(fs::u8path(file)).size(), got.c_str(), a->sha256.c_str());
      check(got == a->sha256 && readAll(fs::u8path(file)).size() == a->size, "and it's exactly what GitHub lists");
    }
  }

  fs::remove_all(work, ec);
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

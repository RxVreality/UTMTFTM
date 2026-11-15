// Checking GitHub for a newer UTMTFTM, fetching it and starting its installer, so nobody has to go and download each
// version by hand (J4, 14 November). Everything that touches the network or starts a program runs on a thread of its own,
// through curl, which Windows 10 and later and SteamOS both have. The overlay never waits on it.
//
// What it sends: one request to GitHub's public API for the latest release, with no account and nothing about you, at
// start and then every few hours. A check that couldn't get through tries again sooner. UTMTFTM_NO_UPDATE_CHECK=1 turns it off.
//
// A release can say older versions may not work properly any more, for when a SteamVR update breaks something and the
// fix is out. It goes in the release notes, where nobody reading them sees it:
//   <!-- utmtftm-min-ok: 0.6.1 | Live bindings need 0.6.1 since SteamVR's last update -->
// Anything older than that version then shows a red warning instead of a plain "an update is ready".
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fh {
namespace update {

constexpr const char* kRepo = "RxVreality/UTMTFTM";
const char* thisVersion();  // this build's, "0.6.1"

// Below 0 if a is older than b. A leading v doesn't count, and a test or pre-release ("0.6.1-test") comes before its release.
int compareVersions(const std::string& a, const std::string& b);

struct Asset {
  std::string name, url;
  std::string sha256;  // as GitHub lists it, lower case hex. Empty if it doesn't
  uint64_t size = 0;
};
struct Release {
  std::string version;  // "0.6.2"
  std::string title;    // "UTMTFTM v0.6.2"
  std::string summary;  // the first paragraph of the notes, plain words
  std::string page;     // the release on github.com
  std::vector<Asset> assets;
  std::string minOk, why;  // from the notes' utmtftm-min-ok line
};
// GitHub's answer for /releases/latest. False if it isn't one.
bool parseRelease(const std::string& json, Release& out);
// The one file this platform installs from: the Windows installer, or the Frame's install script, which fetches and checks
// its own download.
const Asset* assetFor(const Release& r, bool windows);

std::string sha256Hex(const std::string& bytes);

enum class State { Off, Idle, Checking, UpToDate, Available, Downloading, Installing, Installed, Failed };
struct Status {
  State state = State::Idle;
  Release latest;
  bool needed = false;  // this version is older than the notes' min-ok, so things may not work until it updates
  std::string error;    // in words, when it failed
  uint32_t serial = 0;  // goes up with every change, for redrawing
};

// Runs a program and waits for it. Its arguments are passed as they are, with no shell. The exit code, or -1 if it didn't
// start or ran past the time. Windows and Linux each have one (platform_*.cpp).
int runHidden(const std::vector<std::string>& args, int timeoutMs);
// Starts a program and leaves it running. False if it didn't start.
bool startDetached(const std::vector<std::string>& args);
std::string curlPath();  // "" if this machine hasn't got it

class Updater {
 public:
  explicit Updater(std::string dataDir);
  ~Updater();
  // Looks for a newer version, unless it looked within the last few hours or one's already being fetched. now in ms.
  // After a check that couldn't reach GitHub it looks again in a minute, then 2, 5, 15 and every 30 minutes until one does.
  void tick(uint64_t nowMs);
  void checkNow();
  // Fetches the new version, checks it's exactly what GitHub lists, and starts its installer. On Windows that puts the new
  // files in next to the running ones, and SteamVR uses them from its next start. On the Frame the same.
  void install();
  Status status() const;
  bool busy() const { return busy_; }  // a check or an install is still running on its thread

  // For the tests: where to ask, and how to fetch a URL into a file. Real ones by default.
  std::string apiUrl;
  std::function<bool(const std::string& url, const std::string& file)> fetch;
  std::function<bool(const std::string& file, bool windows)> start;
  bool windows;

 private:
  void set(const std::function<void(Status&)>& change);
  void join();
  std::string dir_;
  mutable std::mutex m_;
  Status s_;
  std::thread worker_;
  std::atomic<bool> busy_{false};
  uint64_t lastCheck_ = 0;
  bool checked_ = false;
  std::atomic<int> failures_{0};  // checks in a row that couldn't reach GitHub
};

}  // namespace update
}  // namespace fh

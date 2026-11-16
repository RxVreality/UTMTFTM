// Checking GitHub for a newer UTMTFTM, so nobody has to go and look for each version by hand (J4, 14 November).
// Everything that touches the network runs on a thread of its own. The overlay never waits on it.
//
// Windows only asks. It asks through Windows' own web library (WinHTTP) inside this program, starts no other program, and
// never downloads or runs an installer: the Update page hands GitHub's link to your web browser and you open the
// installer yourself, as the first time (J4, 15 November). 0.6.1 asked through a hidden curl.exe and would have run the
// installer out of sight, which is what a Trojan downloader does, and MMeNDtal got a Trojan warning on it.
// The Steam Frame still fetches through curl and updates itself; there's no antivirus on SteamOS to alarm.
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

// Opened: Windows only, the link went to the web browser. Downloading, Installing, Installed: the Frame only.
enum class State { Off, Idle, Checking, UpToDate, Available, Downloading, Installing, Installed, Opened, Failed };
struct Status {
  State state = State::Idle;
  Release latest;
  bool needed = false;  // this version is older than the notes' min-ok, so things may not work until it updates
  std::string error;    // in words, when it failed
  std::string opened;   // Windows: the link the web browser was given
  uint32_t serial = 0;  // goes up with every change, for redrawing
};

// Fetches a URL into a file. False if it didn't get a whole answer. Windows: WinHTTP, in this program. Linux: curl.
bool fetchUrl(const std::string& url, const std::string& file);
// Only this project's own pages on GitHub are ever handed to the web browser.
inline bool ourLink(const std::string& url) {
  const std::string ours = std::string("https://github.com/") + kRepo + "/";
  if (url.rfind(ours, 0) != 0 || url.size() > 300) return false;
  for (const char ch : url)
    if (ch <= ' ' || ch == '"' || ch == '<' || ch == '>' || ch == '^' || ch == '`' || ch == '|' || ch == '\\' || ch == 127) return false;
  return true;
}

#ifdef _WIN32
// Hands a link to the web browser, the way clicking it would. False if Windows wouldn't.
bool openInBrowser(const std::string& url);
#else
// Runs a program and waits for it. Its arguments are passed as they are, with no shell. The exit code, or -1 if it didn't
// start or ran past the time.
int runHidden(const std::vector<std::string>& args, int timeoutMs);
std::string curlPath();  // "" if this machine hasn't got it
#endif

class Updater {
 public:
  explicit Updater(std::string dataDir);
  ~Updater();
  // Looks for a newer version, unless it looked within the last few hours or one's already being fetched. now in ms.
  // After a check that couldn't reach GitHub it looks again in a minute, then 2, 5, 15 and every 30 minutes until one does.
  void tick(uint64_t nowMs);
  void checkNow();
  // Windows: hands the installer's link to the web browser, and nothing more (the release's page if it has no installer).
  // The Frame: fetches the new version, checks it's exactly what GitHub lists, and starts its install script, which puts
  // the new files in next to the running ones. SteamVR uses them from its next start.
  void install();
  Status status() const;
  bool busy() const { return busy_; }  // a check or an install is still running on its thread

  // For the tests: where to ask, how to fetch a URL into a file, how to start the Frame's script and how to hand the web
  // browser a link. Real ones by default.
  std::string apiUrl;
  std::function<bool(const std::string& url, const std::string& file)> fetch;
  std::function<bool(const std::string& file, bool windows)> start;
  std::function<bool(const std::string& url)> open;
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

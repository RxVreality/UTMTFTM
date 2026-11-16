// The Linux side of platform.h, written for SteamOS on the Steam Frame.
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#include "platform.h"
#include "update.h"

namespace fs = std::filesystem;

namespace fh {
namespace platform {

#if defined(__aarch64__)
static const char* const kBinDir = "linuxarm64";
#else
static const char* const kBinDir = "linux64";
#endif

std::string dataDir() { return fhlive::linuxDataDir(); }

std::string exeDir() {
  char b[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
  if (n <= 0) return ".";
  std::string s(b, size_t(n));
  size_t at = s.rfind('/');
  return at == std::string::npos ? "." : s.substr(0, at);
}

std::string steamDir() { return fhlive::linuxHome() + "/.local/share/Steam"; }

std::string steamvrDir() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  std::string config = xdg && xdg[0] ? std::string(xdg) : fhlive::linuxHome() + "/.config";
  std::ifstream in(config + "/openvr/openvrpaths.vrpath");
  Json::Value v;
  Json::CharReaderBuilder b;
  std::string errs;
  if (in && Json::parseFromStream(b, in, &v, &errs))
    for (const auto& r : v["runtime"]) {
      std::error_code ec;
      if (fs::exists(r.asString(), ec)) return r.asString();
    }
  std::error_code ec;
  if (fs::exists("/opt/steamvr", ec)) return "/opt/steamvr";  // where the Frame keeps it
  return steamDir() + "/steamapps/common/SteamVR";
}

std::vector<std::string> configDirs() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  std::string config = xdg && xdg[0] ? std::string(xdg) : fhlive::linuxHome() + "/.config";
  std::vector<std::string> out;
  std::ifstream in(config + "/openvr/openvrpaths.vrpath");
  Json::Value v;
  Json::CharReaderBuilder b;
  std::string errs;
  if (in && Json::parseFromStream(b, in, &v, &errs))
    for (const auto& c : v["config"]) out.push_back(c.asString());
  const std::string steam = steamDir() + "/config";
  if (std::find(out.begin(), out.end(), steam) == out.end()) out.push_back(steam);
  return out;
}

// On Linux SteamVR keeps its per-user binding files in ~/steamvr, where Windows has them in Documents.
std::string documentsDir() { return fhlive::linuxHome(); }

void sleepMs(int ms) {
  timespec t{ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&t, &t) == -1 && errno == EINTR) {
  }
}

uint32_t currentPid() { return uint32_t(getpid()); }

static std::vector<std::string> nulSeparated(const std::string& s) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at < s.size()) {
    size_t end = s.find('\0', at);
    if (end == std::string::npos) end = s.size();
    if (end > at) out.push_back(s.substr(at, end - at));
    at = end + 1;
  }
  return out;
}

// "C:\Games\Thing.EXE" and "/usr/bin/thing" both come out as "thing".
static std::string plainName(std::string s) {
  size_t at = s.find_last_of("/\\");
  if (at != std::string::npos) s = s.substr(at + 1);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0) s.resize(s.size() - 4);
  return s;
}

std::string exeKeyFrom(const std::string& environ, const std::string& cmdline) {
  // Steam starts a game with its app id in the environment, whatever it runs under.
  for (const std::string& var : nulSeparated(environ)) {
    if (var.rfind("SteamAppId=", 0) != 0) continue;
    const std::string id = var.substr(11);
    if (!id.empty() && id != "0" && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); })) return "steam.app." + id;
  }
  // A Windows game on the Frame runs as FEX running Wine running the game: the name wanted is past those.
  static const char* const kRunners[] = {"fex", "fexinterpreter", "fexloader", "wine", "wine64", "wine-preloader", "wine64-preloader"};
  for (const std::string& arg : nulSeparated(cmdline)) {
    const std::string name = plainName(arg);
    if (name.empty() || std::any_of(std::begin(kRunners), std::end(kRunners), [&](const char* r) { return name == r; })) continue;
    return "exe." + name;
  }
  return "";
}

static std::string readAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

std::string processExeKey(uint32_t pid) {
  const std::string proc = "/proc/" + std::to_string(pid);
  return exeKeyFrom(readAll(proc + "/environ"), readAll(proc + "/cmdline"));
}

fhlive::Shared* mapLive() {
  static fhlive::Shared* m = nullptr;
  if (m) return m;
  std::error_code ec;
  fs::create_directories(dataDir(), ec);
  int f = open((dataDir() + "/live.shm").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (f < 0) return nullptr;
  struct stat st {};
  if (fstat(f, &st) != 0 || (st.st_size < off_t(sizeof(fhlive::Shared)) && ftruncate(f, sizeof(fhlive::Shared)) != 0)) {
    close(f);
    return nullptr;
  }
  void* v = mmap(nullptr, sizeof(fhlive::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
  close(f);
  if (v == MAP_FAILED) return nullptr;
  return m = static_cast<fhlive::Shared*>(v);
}

double threadCpuSeconds() {
  rusage u{};
  if (getrusage(RUSAGE_THREAD, &u) != 0) return 0;
  return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

bool systemCpu(double* busy, double* total) {
  std::ifstream f("/proc/stat");
  std::string cpu;
  unsigned long long v[8] = {};
  if (!(f >> cpu) || cpu != "cpu") return false;
  for (auto& x : v) f >> x;
  const double hz = double(sysconf(_SC_CLK_TCK));
  const unsigned long long all = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
  *total = double(all) / hz;
  *busy = double(all - v[3] - v[4]) / hz;  // less idle and waiting for the disk
  return true;
}

uint64_t pageFaults() {
  rusage u{};
  return getrusage(RUSAGE_SELF, &u) == 0 ? uint64_t(u.ru_minflt + u.ru_majflt) : 0;
}

std::string raisePriority() { return "processor priority unchanged (the Frame never needed it)"; }

double cpuSeconds() {
  rusage u{};
  if (getrusage(RUSAGE_SELF, &u) != 0) return 0;
  return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

bool setDriverRegistered(bool on, const std::string& driverDir) {
  const std::string tool = steamvrDir() + "/bin/" + kBinDir + "/vrpathreg";
  const char* verb = on ? "adddriver" : "removedriver";
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    int quiet = open("/dev/null", O_RDWR);
    if (quiet >= 0) {
      dup2(quiet, 1);
      dup2(quiet, 2);
    }
    execl(tool.c_str(), tool.c_str(), verb, driverDir.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  for (int waited = 0; waited < 20000; waited += 50) {
    pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (done < 0) return false;
    sleepMs(50);
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  return false;
}

}  // namespace platform

// ---------------------------------------------------------------- for the updater (update.h)
namespace update {

static pid_t spawn(const std::vector<std::string>& args) {
  if (args.empty()) return -1;
  std::vector<char*> argv;
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid != 0) return pid;
  setsid();  // its own session, so it isn't stopped along with the overlay
  int quiet = open("/dev/null", O_RDWR);
  if (quiet >= 0) {
    dup2(quiet, 0);
    dup2(quiet, 1);
    dup2(quiet, 2);
  }
  execv(argv[0], argv.data());
  _exit(127);
}

bool fetchUrl(const std::string& url, const std::string& file) {
  const std::string curl = curlPath();
  if (curl.empty()) return false;
  std::error_code ec;
  fs::remove(fs::u8path(file), ec);
  const std::string agent = std::string("UTMTFTM/") + thisVersion();
  return runHidden({curl, "-fsSL", "--max-time", "120", "--retry", "2", "-A", agent, "-H", "Accept: application/vnd.github+json", "-o", file, url}, 150000) == 0;
}

int runHidden(const std::vector<std::string>& args, int timeoutMs) {
  const pid_t pid = spawn(args);
  if (pid < 0) return -1;
  int status = 0;
  for (int waited = 0; waited < timeoutMs; waited += 50) {
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (done < 0) return -1;
    platform::sleepMs(50);
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  return -1;
}

bool startDetached(const std::vector<std::string>& args) {
  const pid_t pid = spawn(args);
  if (pid < 0) return false;
  signal(SIGCHLD, SIG_IGN);  // nobody waits for it, so don't leave it a zombie
  return true;
}

std::string curlPath() {
  for (const char* p : {"/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl"})
    if (access(p, X_OK) == 0) return p;
  return "";
}

}  // namespace update
}  // namespace fh

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <psapi.h>

#include <json/json.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "platform.h"
#include "update.h"

namespace fs = std::filesystem;

namespace fh {
namespace platform {

static std::string utf8(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string s(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}
static std::wstring wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return L"";
  std::wstring w(size_t(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}
static std::string slashes(std::string p) {
  std::replace(p.begin(), p.end(), '\\', '/');
  while (!p.empty() && p.back() == '/') p.pop_back();
  return p;
}

static std::string localAppData() {
  wchar_t b[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", b, MAX_PATH);
  return n && n < MAX_PATH ? slashes(utf8(b)) : ".";
}

std::string dataDir() { return localAppData() + "/UTMTFTM"; }

std::string exeDir() {
  wchar_t b[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, b, MAX_PATH);
  std::string s = slashes(utf8(b));
  size_t at = s.rfind('/');
  return at == std::string::npos ? "." : s.substr(0, at);
}

std::string steamDir() {
  wchar_t b[MAX_PATH] = {0};
  DWORD size = sizeof b;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, b, &size) == ERROR_SUCCESS)
    return slashes(utf8(b));
  return "C:/Program Files (x86)/Steam";
}

std::string steamvrDir() {
  std::ifstream in(wide(localAppData() + "/openvr/openvrpaths.vrpath"));
  Json::Value v;
  Json::CharReaderBuilder b;
  std::string errs;
  if (in && Json::parseFromStream(b, in, &v, &errs))
    for (const auto& r : v["runtime"]) {
      std::string p = slashes(r.asString());
      std::error_code ec;
      if (fs::exists(wide(p), ec)) return p;
    }
  return steamDir() + "/steamapps/common/SteamVR";
}

std::vector<std::string> configDirs() {
  std::vector<std::string> out;
  std::ifstream in(wide(localAppData() + "/openvr/openvrpaths.vrpath"));
  Json::Value v;
  Json::CharReaderBuilder b;
  std::string errs;
  if (in && Json::parseFromStream(b, in, &v, &errs))
    for (const auto& c : v["config"]) out.push_back(slashes(c.asString()));
  const std::string steam = steamDir() + "/config";
  if (std::find(out.begin(), out.end(), steam) == out.end()) out.push_back(steam);
  return out;
}

std::string documentsDir() {
  PWSTR p = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) out = slashes(utf8(p));
  CoTaskMemFree(p);
  return out;
}

void sleepMs(int ms) {
  if (ms >= 50) { Sleep(ms); return; }
  static HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!t) { Sleep(ms); return; }
  LARGE_INTEGER due;
  due.QuadPart = -10000LL * ms;
  if (SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(t, INFINITE);
  else Sleep(ms);
}

uint32_t currentPid() { return GetCurrentProcessId(); }

std::string processExeKey(uint32_t pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return "";
  wchar_t path[MAX_PATH] = {0};
  DWORD n = MAX_PATH;
  BOOL ok = QueryFullProcessImageNameW(h, 0, path, &n);
  CloseHandle(h);
  if (!ok) return "";
  std::wstring name = fs::path(path).stem().wstring();
  CharLowerW(name.data());
  return "exe." + utf8(name.c_str());
}

fhlive::Shared* mapLive() {
  static fhlive::Shared* m = nullptr;
  if (m) return m;
  std::error_code ec;
  fs::create_directories(wide(dataDir()), ec);
  HANDLE f = CreateFileW(wide(dataDir() + "/live.shm").c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return nullptr;
  HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, sizeof(fhlive::Shared), nullptr);
  CloseHandle(f);
  if (!map) return nullptr;
  void* v = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(fhlive::Shared));
  CloseHandle(map);
  return m = static_cast<fhlive::Shared*>(v);
}

static double fileSecs(const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; }

double threadCpuSeconds() {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  return fileSecs(kernel) + fileSecs(user);
}

bool systemCpu(double* busy, double* total) {
  FILETIME idle, kernel, user;
  if (!GetSystemTimes(&idle, &kernel, &user)) return false;
  *total = fileSecs(kernel) + fileSecs(user);  // kernel time includes idle time
  *busy = *total - fileSecs(idle);
  return true;
}

uint64_t pageFaults() {
  PROCESS_MEMORY_COUNTERS c{};
  c.cb = sizeof c;
  return GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof c) ? c.PageFaultCount : 0;
}

// J4, Pavlov over Virtual Desktop, 2026-11-14: drawing the bubble, all CPU work, took 18-54 ms instead of 6, and the program
// went up to 2.3 s between turns, while the same steps measured under 0.1 ms with SteamVR and a busy graphics card in the lab.
// A program with no window at normal priority waits behind a game that's in front and busy. SteamVR's own programs run above it.
std::string raisePriority() {
  std::string out;
  out += SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) ? "processor priority above normal" : "processor priority unchanged (refused)";
  PROCESS_POWER_THROTTLING_STATE pt{};
  pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  pt.StateMask = 0;  // neither: full speed, and the timer as asked
  out += SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt) ? ", no power throttling" : ", power throttling unchanged";
  return out;
}

double cpuSeconds() {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  const auto secs = [](const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
  return secs(kernel) + secs(user);
}

bool setDriverRegistered(bool on, const std::string& driverDir) {
  std::wstring exe = wide(steamvrDir() + "/bin/win64/vrpathreg.exe"), dir = wide(driverDir);
  std::replace(exe.begin(), exe.end(), L'/', L'\\');
  std::replace(dir.begin(), dir.end(), L'/', L'\\');
  std::wstring cmd = L"\"" + exe + L"\" " + (on ? L"adddriver" : L"removedriver") + L" \"" + dir + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
  DWORD code = 1;
  if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
}

}  // namespace platform

// ---------------------------------------------------------------- for the updater (update.h)
namespace update {

// One argument as CommandLineToArgvW reads it back: quoted, with quotes and the backslashes before them escaped.
static std::wstring argQuoted(const std::wstring& a) {
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t ch : a) {
    if (ch == L'\\') { ++slashes; continue; }
    if (ch == L'"') out.append(slashes * 2 + 1, L'\\');
    else out.append(slashes, L'\\');
    slashes = 0;
    out += ch;
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}

static bool launch(const std::vector<std::string>& args, DWORD flags, PROCESS_INFORMATION& pi) {
  if (args.empty()) return false;
  std::wstring exe = platform::wide(args[0]);
  std::replace(exe.begin(), exe.end(), L'/', L'\\');
  std::wstring cmd;
  for (size_t i = 0; i < args.size(); ++i) {
    std::wstring a = i == 0 ? exe : platform::wide(args[i]);
    if (i) cmd += L" ";
    cmd += argQuoted(a);
  }
  STARTUPINFOW si{};
  si.cb = sizeof si;
  pi = PROCESS_INFORMATION{};
  return CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi) != 0;
}

int runHidden(const std::vector<std::string>& args, int timeoutMs) {
  PROCESS_INFORMATION pi;
  if (!launch(args, CREATE_NO_WINDOW, pi)) return -1;
  DWORD code = DWORD(-1);
  if (WaitForSingleObject(pi.hProcess, DWORD(timeoutMs)) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  else TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return int(code);
}

bool startDetached(const std::vector<std::string>& args) {
  PROCESS_INFORMATION pi;
  if (!launch(args, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

// Windows 10 and 11 have had it since 2018.
std::string curlPath() {
  wchar_t sys[MAX_PATH];
  const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
  if (!n || n >= MAX_PATH) return "";
  const std::string path = platform::utf8(sys) + "\\curl.exe";
  std::error_code ec;
  return fs::exists(fs::u8path(path), ec) ? path : "";
}

}  // namespace update
}  // namespace fh

// A stand-in for SteamVR's client library, vrclient.so, so the real Frame program can run on a build machine with no
// headset. OpenVR's own loader finds it through openvrpaths.vrpath, exactly as it finds the real one, so everything from
// the program's first line down is the code that ships. tests/test_frame_run.sh drives it.
//
// It plays one scenario, picked by FAKE_STEAMVR_SCENARIO:
//   place      clicks Move on the dashboard, drags the left hand with the right, and checks what comes out of the
//              real driver, in the real poses a game would get
//   bypass     the controllers' poses never reach the driver's hook, and the program has to notice and not claim "Live"
//   addon_off  the add-on is switched off in SteamVR, and the program has to leave without showing anything
// Results go to FAKE_STEAMVR_LOG, and pictures of what it really sent as Vulkan textures go to FAKE_STEAMVR_SHOTS.
//
// The interfaces are C++ classes with hundreds of methods between them and only a few matter here, so each is a vtable
// where every slot returns zero (no error, false, nothing), with just the needed ones filled in. Slots come from the
// compiler via member pointers, so they follow the headers. ARM64 only: that's the only ABI this is written for.
#include <openvr.h>

#include <cairo.h>
#include <json/json.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fake_vrserver.h"
#include "platform.h"
#include "ui.h"

namespace {

using fakeserver::Pose;

// ---------------------------------------------------------------- results
std::mutex g_logLock;
std::atomic<int> g_failures{0};

void say(const char* fmt, ...) {
  const char* path = std::getenv("FAKE_STEAMVR_LOG");
  std::lock_guard<std::mutex> lock(g_logLock);
  FILE* f = path ? fopen(path, "a") : stderr;
  if (!f) return;
  va_list a;
  va_start(a, fmt);
  vfprintf(f, fmt, a);
  va_end(a);
  fputs("\n", f);
  if (path) fclose(f);
}

void check(bool ok, const char* what, double got = 0) {
  if (!ok) ++g_failures;
  say("%s %s (%.2f)", ok ? "ok  " : "FAIL", what, got);
}

// ---------------------------------------------------------------- what the simulated headset is doing
constexpr uint32_t kHmd = 0, kLeft = 3, kRight = 4;
constexpr uint64_t kOverlayDash = 1, kOverlayThumb = 2, kOverlayFirstOther = 3;
constexpr uint64_t kSourceLeft = 1001, kSourceRight = 1002;
constexpr uint32_t kGamePid = 4242;

struct World {
  std::mutex m;
  bool dashboardVisible = true;
  bool quit = false;
  uint32_t scenePid = 0;
  std::deque<vr::VREvent_t> dashEvents;
  bool grab[2] = {false, false}, done[2] = {false, false};
  std::map<std::string, uint64_t> handles;
  std::map<std::pair<uint64_t, uint64_t>, bool> lastDigital;
  std::map<std::string, uint64_t> overlays;
  uint64_t nextOverlay = kOverlayFirstOther;
} g;

std::atomic<int> g_dashUploads{0}, g_otherUploads{0}, g_rawUploads{0}, g_savedShots{0};
std::atomic<bool> g_manifestRead{false};
std::thread g_scenario;
std::string g_scenarioName;

// ---------------------------------------------------------------- vtables
void* zero() { return nullptr; }

struct Interface {
  void** vtable;  // where a C++ object keeps it
  void* table[256];
  Interface() {
    vtable = table;
    for (void*& f : table) f = reinterpret_cast<void*>(zero);
  }
};

// On ARM64 a pointer to a virtual member holds its byte offset into the vtable.
template <class R, class C, class... A>
size_t slotOf(R (C::*f)(A...)) {
  struct Raw {
    uintptr_t ptr;
    ptrdiff_t adj;
  };
  Raw raw;
  static_assert(sizeof f == sizeof raw, "unexpected member pointer");
  memcpy(&raw, &f, sizeof raw);
  return raw.ptr / sizeof(void*);
}
#define FILL(iface, cls, method, fn) iface.table[slotOf(&vr::cls::method)] = reinterpret_cast<void*>(fn)

Interface g_system, g_overlay, g_input, g_apps, g_settings, g_other;

vr::HmdMatrix34_t toMatrix(const Pose& p) {
  const double w = p.rot[0], x = p.rot[1], y = p.rot[2], z = p.rot[3];
  vr::HmdMatrix34_t m;
  m.m[0][0] = float(1 - 2 * (y * y + z * z)); m.m[0][1] = float(2 * (x * y - w * z)); m.m[0][2] = float(2 * (x * z + w * y)); m.m[0][3] = float(p.pos[0]);
  m.m[1][0] = float(2 * (x * y + w * z)); m.m[1][1] = float(1 - 2 * (x * x + z * z)); m.m[1][2] = float(2 * (y * z - w * x)); m.m[1][3] = float(p.pos[1]);
  m.m[2][0] = float(2 * (x * z - w * y)); m.m[2][1] = float(2 * (y * z + w * x)); m.m[2][2] = float(1 - 2 * (x * x + y * y)); m.m[2][3] = float(p.pos[2]);
  return m;
}
vr::TrackedDevicePose_t toTracked(const Pose& p) {
  vr::TrackedDevicePose_t t{};
  t.mDeviceToAbsoluteTracking = toMatrix(p);
  t.eTrackingResult = vr::TrackingResult_Running_OK;
  t.bPoseIsValid = true;
  t.bDeviceIsConnected = true;
  return t;
}

// ---------------------------------------------------------------- IVRSystem
uint32_t sysStringProp(void*, vr::TrackedDeviceIndex_t idx, vr::ETrackedDeviceProperty prop, char* out, uint32_t size, vr::ETrackedPropertyError* err) {
  std::string v;
  if (prop == vr::Prop_ControllerType_String) v = "frame_controller";
  else if (prop == vr::Prop_RenderModelName_String) v = idx == kLeft ? "frame_controller_left" : "frame_controller_right";
  if (err) *err = vr::TrackedProp_Success;
  if (out && size) snprintf(out, size, "%s", v.c_str());
  return uint32_t(v.size() + 1);
}
vr::TrackedDeviceIndex_t sysRoleDevice(void*, vr::ETrackedControllerRole role) {
  return role == vr::TrackedControllerRole_LeftHand ? kLeft : role == vr::TrackedControllerRole_RightHand ? kRight : vr::k_unTrackedDeviceIndexInvalid;
}
vr::HmdMatrix34_t sysIdentity(void*) {
  vr::HmdMatrix34_t m{};
  m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
  return m;
}
vr::HmdMatrix34_t sysEye(void*, vr::EVREye eye) {
  vr::HmdMatrix34_t m{};
  m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
  m.m[0][3] = eye == vr::Eye_Left ? -0.032f : 0.032f;
  return m;
}
void sysDevicePoses(void*, vr::ETrackingUniverseOrigin, float, vr::TrackedDevicePose_t* poses, uint32_t count) {
  if (count) poses[0] = toTracked(fakeserver::seenPose(kHmd));
}
bool sysPollEvent(void*, vr::VREvent_t* ev, uint32_t) {
  std::lock_guard<std::mutex> lock(g.m);
  if (!g.quit) return false;
  g.quit = false;
  memset(ev, 0, sizeof *ev);
  ev->eventType = vr::VREvent_Quit;
  return true;
}

// ---------------------------------------------------------------- IVROverlay
vr::EVROverlayError ovDashboard(void*, const char* key, const char*, vr::VROverlayHandle_t* main, vr::VROverlayHandle_t* thumb) {
  say("CreateDashboardOverlay %s", key);
  *main = kOverlayDash;
  *thumb = kOverlayThumb;
  return vr::VROverlayError_None;
}
vr::EVROverlayError ovCreate(void*, const char* key, const char*, vr::VROverlayHandle_t* h) {
  std::lock_guard<std::mutex> lock(g.m);
  uint64_t& id = g.overlays[key];
  if (!id) id = g.nextOverlay++;
  *h = id;
  return vr::VROverlayError_None;
}
const char* ovErrorName(void*, vr::EVROverlayError) { return "an overlay error"; }
bool ovDashVisible(void*) {
  std::lock_guard<std::mutex> lock(g.m);
  return g.dashboardVisible;
}
bool ovVisible(void*, vr::VROverlayHandle_t h) {
  std::lock_guard<std::mutex> lock(g.m);
  return h == kOverlayDash && g.dashboardVisible;
}
bool ovPollEvent(void*, vr::VROverlayHandle_t h, vr::VREvent_t* ev, uint32_t) {
  std::lock_guard<std::mutex> lock(g.m);
  if (h != kOverlayDash || g.dashEvents.empty()) return false;
  *ev = g.dashEvents.front();
  g.dashEvents.pop_front();
  return true;
}

// Copies the texture back out with Vulkan, the way the compositor would read it, and saves what's really in it.
void saveTexture(const vr::VRVulkanTextureData_t& t, const std::string& name) {
  const char* dir = std::getenv("FAKE_STEAMVR_SHOTS");
  if (!dir) return;
  auto dev = reinterpret_cast<VkDevice>(t.m_pDevice);
  auto queue = reinterpret_cast<VkQueue>(t.m_pQueue);
  auto pd = reinterpret_cast<VkPhysicalDevice>(t.m_pPhysicalDevice);
  const VkDeviceSize size = VkDeviceSize(t.m_nWidth) * t.m_nHeight * 4;
  VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bc.size = size;
  bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buf = VK_NULL_HANDLE;
  if (vkCreateBuffer(dev, &bc, nullptr, &buf) != VK_SUCCESS) return;
  VkMemoryRequirements r;
  vkGetBufferMemoryRequirements(dev, buf, &r);
  VkPhysicalDeviceMemoryProperties mem;
  vkGetPhysicalDeviceMemoryProperties(pd, &mem);
  uint32_t type = 0;
  const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
    if ((r.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = r.size;
  ai.memoryTypeIndex = type;
  VkDeviceMemory bm = VK_NULL_HANDLE;
  vkAllocateMemory(dev, &ai, nullptr, &bm);
  vkBindBufferMemory(dev, buf, bm, 0);

  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pc.queueFamilyIndex = t.m_nQueueFamilyIndex;
  VkCommandPool pool = VK_NULL_HANDLE;
  vkCreateCommandPool(dev, &pc, nullptr, &pool);
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = pool;
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(dev, &ca, &cmd);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(cmd, &bi);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {t.m_nWidth, t.m_nHeight, 1};
  vkCmdCopyImageToBuffer(cmd, reinterpret_cast<VkImage>(t.m_nImage), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
  vkEndCommandBuffer(cmd);
  VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(dev, &fc, nullptr, &fence);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(queue, 1, &si, fence);
  vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull);

  void* mapped = nullptr;
  vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, &mapped);
  cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(t.m_nWidth), int(t.m_nHeight));
  cairo_surface_flush(s);
  uint8_t* dst = cairo_image_surface_get_data(s);
  const int stride = cairo_image_surface_get_stride(s);
  const uint8_t* src = static_cast<const uint8_t*>(mapped);
  uint64_t inked = 0;
  for (uint32_t y = 0; y < t.m_nHeight; ++y)
    for (uint32_t x = 0; x < t.m_nWidth; ++x) {
      const uint8_t* p = src + (size_t(y) * t.m_nWidth + x) * 4;
      uint8_t* d = dst + size_t(y) * stride + x * 4;
      const unsigned a = p[3];
      d[0] = uint8_t(p[2] * a / 255); d[1] = uint8_t(p[1] * a / 255); d[2] = uint8_t(p[0] * a / 255); d[3] = uint8_t(a);
      if (a) ++inked;
    }
  cairo_surface_mark_dirty(s);
  cairo_surface_write_to_png(s, (std::string(dir) + "/" + name + ".png").c_str());
  cairo_surface_destroy(s);
  say("saved %s: %ux%u, %llu pixels with something in them", name.c_str(), t.m_nWidth, t.m_nHeight, (unsigned long long)inked);
  vkUnmapMemory(dev, bm);
  vkDestroyFence(dev, fence, nullptr);
  vkDestroyCommandPool(dev, pool, nullptr);
  vkDestroyBuffer(dev, buf, nullptr);
  vkFreeMemory(dev, bm, nullptr);
}

vr::EVROverlayError ovTexture(void*, vr::VROverlayHandle_t h, const vr::Texture_t* tex) {
  if (tex->eType != vr::TextureType_Vulkan) {
    ++g_failures;
    say("FAIL a texture that isn't Vulkan");
    return vr::VROverlayError_InvalidTexture;
  }
  const auto& vk = *static_cast<const vr::VRVulkanTextureData_t*>(tex->handle);
  if (vk.m_nFormat != VK_FORMAT_R8G8B8A8_UNORM || !vk.m_nImage || !vk.m_pDevice || !vk.m_pQueue) {
    ++g_failures;
    say("FAIL a Vulkan texture with something missing");
  }
  if (h == kOverlayDash) {
    const int n = ++g_dashUploads;
    if (g_savedShots < 40) { ++g_savedShots; saveTexture(vk, "dashboard-" + std::to_string(n)); }
  } else if (h != kOverlayThumb) {
    const int n = ++g_otherUploads;
    if (n <= 3 || n % 60 == 0) { if (g_savedShots < 40) { ++g_savedShots; saveTexture(vk, "placing-" + std::to_string(n)); } }
  }
  return vr::VROverlayError_None;
}
vr::EVROverlayError ovRaw(void*, vr::VROverlayHandle_t, void*, uint32_t, uint32_t, uint32_t) {
  ++g_rawUploads;
  return vr::VROverlayError_None;
}

// ---------------------------------------------------------------- IVRInput
vr::EVRInputError inManifest(void*, const char* path) {
  std::ifstream in(path);
  Json::Value v;
  Json::CharReaderBuilder b;
  std::string errs;
  const bool ok = in && Json::parseFromStream(b, in, &v, &errs) && v.isMember("actions");
  check(ok, "the action manifest SteamVR is told about exists and parses");
  g_manifestRead = true;
  say("SetActionManifestPath %s", path);
  return vr::VRInputError_None;
}
uint64_t handleOf(const char* name) {
  std::lock_guard<std::mutex> lock(g.m);
  uint64_t& id = g.handles[name];
  if (!id) id = g.handles.size();
  return id;
}
vr::EVRInputError inSetHandle(void*, const char* name, vr::VRActionSetHandle_t* h) { *h = handleOf(name); return vr::VRInputError_None; }
vr::EVRInputError inActionHandle(void*, const char* name, vr::VRActionHandle_t* h) { *h = handleOf(name); return vr::VRInputError_None; }
vr::EVRInputError inSource(void*, const char* name, vr::VRInputValueHandle_t* h) {
  *h = !strcmp(name, "/user/hand/left") ? kSourceLeft : kSourceRight;
  return vr::VRInputError_None;
}
vr::EVRInputError inDigital(void*, vr::VRActionHandle_t action, vr::InputDigitalActionData_t* d, uint32_t, vr::VRInputValueHandle_t source) {
  const int hand = source == kSourceLeft ? 0 : 1;
  const uint64_t aGrab = handleOf("/actions/fh/in/grab"), aDone = handleOf("/actions/fh/in/done");
  std::lock_guard<std::mutex> lock(g.m);
  const bool now = action == aGrab ? g.grab[hand] : action == aDone ? g.done[hand] : false;
  bool& last = g.lastDigital[{action, source}];
  memset(d, 0, sizeof *d);
  d->bActive = true;
  d->bState = now;
  d->bChanged = now != last;
  last = now;
  return vr::VRInputError_None;
}
vr::EVRInputError inPose(void*, vr::VRActionHandle_t, vr::ETrackingUniverseOrigin, vr::InputPoseActionData_t* d, uint32_t, vr::VRInputValueHandle_t source) {
  memset(d, 0, sizeof *d);
  d->bActive = true;
  d->pose = toTracked(fakeserver::seenPose(source == kSourceLeft ? kLeft : kRight));
  return vr::VRInputError_None;
}

// ---------------------------------------------------------------- IVRApplications
vr::EVRApplicationError appAddManifest(void*, const char* path, bool) {
  std::ifstream in(path);
  Json::Value v;
  Json::CharReaderBuilder b;
  std::string errs;
  const bool ok = in && Json::parseFromStream(b, in, &v, &errs);
  const Json::Value app = ok ? v["applications"][0] : Json::Value();
  const std::string binary = app["binary_path_linux_arm"].asString();
  check(ok && app["app_key"].asString() == "utmtftm.overlay", "the manifest SteamVR is given parses and names the overlay");
  check(!binary.empty() && binary[0] == '/' && access(binary.c_str(), X_OK) == 0, "the manifest names the installed program, by its full path");
  check(access(app["action_manifest_path"].asCString(), R_OK) == 0, "the manifest names the installed action file");
  check(access(app["image_path"].asCString(), R_OK) == 0, "the manifest names the installed icon");
  say("AddApplicationManifest %s", path);
  return vr::VRApplicationError_None;
}
vr::EVRApplicationError appRemoveManifest(void*, const char* path) {
  say("RemoveApplicationManifest %s", path);
  return vr::VRApplicationError_None;
}
vr::EVRApplicationError appAutoLaunch(void*, const char* key, bool on) {
  say("SetApplicationAutoLaunch %s %d", key, on ? 1 : 0);
  return vr::VRApplicationError_None;
}
bool appInstalled(void*, const char*) { return true; }
uint32_t appScenePid(void*) {
  std::lock_guard<std::mutex> lock(g.m);
  return g.scenePid;
}
vr::EVRApplicationError appKeyByPid(void*, uint32_t, char* out, uint32_t size) {
  snprintf(out, size, "steam.app.12345");
  return vr::VRApplicationError_None;
}
uint32_t appProperty(void*, const char*, vr::EVRApplicationProperty, char* out, uint32_t size, vr::EVRApplicationError* err) {
  if (err) *err = vr::VRApplicationError_None;
  if (out && size) snprintf(out, size, "Simulated Game");
  return 15;
}

// ---------------------------------------------------------------- IVRSettings
const char* setErrorName(void*, vr::EVRSettingsError) { return "a settings error"; }
bool setGetBool(void*, const char* section, const char* key, vr::EVRSettingsError* err) {
  if (g_scenarioName == "addon_off" && !strcmp(section, "driver_utmtftm") && !strcmp(key, vr::k_pch_Driver_Enable_Bool)) {
    if (err) *err = vr::VRSettingsError_None;
    return false;
  }
  if (err) *err = vr::VRSettingsError_UnsetSettingHasNoDefault;
  return false;
}
void setGetString(void*, const char*, const char*, char* out, uint32_t size, vr::EVRSettingsError* err) {
  if (out && size) out[0] = 0;
  if (err) *err = vr::VRSettingsError_UnsetSettingHasNoDefault;
}

void fillInterfaces() {
  FILL(g_system, IVRSystem, GetStringTrackedDeviceProperty, sysStringProp);
  FILL(g_system, IVRSystem, GetTrackedDeviceIndexForControllerRole, sysRoleDevice);
  FILL(g_system, IVRSystem, GetRawZeroPoseToStandingAbsoluteTrackingPose, sysIdentity);
  FILL(g_system, IVRSystem, GetEyeToHeadTransform, sysEye);
  FILL(g_system, IVRSystem, GetDeviceToAbsoluteTrackingPose, sysDevicePoses);
  FILL(g_system, IVRSystem, PollNextEvent, sysPollEvent);

  FILL(g_overlay, IVROverlay, CreateDashboardOverlay, ovDashboard);
  FILL(g_overlay, IVROverlay, CreateOverlay, ovCreate);
  FILL(g_overlay, IVROverlay, GetOverlayErrorNameFromEnum, ovErrorName);
  FILL(g_overlay, IVROverlay, IsDashboardVisible, ovDashVisible);
  FILL(g_overlay, IVROverlay, IsOverlayVisible, ovVisible);
  FILL(g_overlay, IVROverlay, PollNextOverlayEvent, ovPollEvent);
  FILL(g_overlay, IVROverlay, SetOverlayTexture, ovTexture);
  FILL(g_overlay, IVROverlay, SetOverlayRaw, ovRaw);

  FILL(g_input, IVRInput, SetActionManifestPath, inManifest);
  FILL(g_input, IVRInput, GetActionSetHandle, inSetHandle);
  FILL(g_input, IVRInput, GetActionHandle, inActionHandle);
  FILL(g_input, IVRInput, GetInputSourceHandle, inSource);
  FILL(g_input, IVRInput, GetDigitalActionData, inDigital);
  FILL(g_input, IVRInput, GetPoseActionDataForNextFrame, inPose);

  FILL(g_apps, IVRApplications, AddApplicationManifest, appAddManifest);
  FILL(g_apps, IVRApplications, RemoveApplicationManifest, appRemoveManifest);
  FILL(g_apps, IVRApplications, SetApplicationAutoLaunch, appAutoLaunch);
  FILL(g_apps, IVRApplications, IsApplicationInstalled, appInstalled);
  FILL(g_apps, IVRApplications, GetCurrentSceneProcessId, appScenePid);
  FILL(g_apps, IVRApplications, GetApplicationKeyByProcessId, appKeyByPid);
  FILL(g_apps, IVRApplications, GetApplicationPropertyString, appProperty);

  FILL(g_settings, IVRSettings, GetSettingsErrorNameFromEnum, setErrorName);
  FILL(g_settings, IVRSettings, GetBool, setGetBool);
  FILL(g_settings, IVRSettings, GetString, setGetString);
}

// ---------------------------------------------------------------- scenarios
void wait(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

double gap(const Pose& a, const Pose& b) {
  double s = 0;
  for (int i = 0; i < 3; ++i) s += (a.pos[i] - b.pos[i]) * (a.pos[i] - b.pos[i]);
  return std::sqrt(s);
}
double turn(const Pose& a, const Pose& b) {
  const double s = std::fabs(a.rot[0] * b.rot[0] + a.rot[1] * b.rot[1] + a.rot[2] * b.rot[2] + a.rot[3] * b.rot[3]);
  return 2 * std::acos(s > 1 ? 1 : s) * 57.29578;
}

void pushMouse(vr::EVREventType type, double x, double y) {
  vr::VREvent_t ev{};
  ev.eventType = type;
  ev.data.mouse.x = float(x);
  ev.data.mouse.y = float(760 - y);  // SteamVR counts up from the bottom
  std::lock_guard<std::mutex> lock(g.m);
  g.dashEvents.push_back(ev);
}

// The laser lands on a button and clicks it. Where the button is comes from the same drawing code the page uses.
void click(const std::string& id) {
  fh::Canvas cv(1200, 760);
  std::vector<fh::Button> buttons;
  fh::DashModel m;
  m.game = "Simulated Game";
  m.live = fh::LiveState::Live;
  fh::drawDashboard(cv, m, buttons);
  for (const fh::Button& b : buttons) {
    if (b.id != id) continue;
    const double x = b.x + b.w / 2, y = b.y + b.h / 2;
    pushMouse(vr::VREvent_MouseMove, x, y);
    wait(60);
    pushMouse(vr::VREvent_MouseButtonDown, x, y);
    wait(60);
    pushMouse(vr::VREvent_MouseButtonUp, x, y);
    say("clicked %s at %.0f, %.0f", id.c_str(), x, y);
    return;
  }
  check(false, ("there's no button called " + id).c_str());
}

void setDashboard(bool on) {
  std::lock_guard<std::mutex> lock(g.m);
  g.dashboardVisible = on;
}
void setScene(uint32_t pid) {
  std::lock_guard<std::mutex> lock(g.m);
  g.scenePid = pid;
}
void finish() {
  say("RESULT %s (%d failures)", g_failures ? "FAIL" : "PASS", g_failures.load());
  std::lock_guard<std::mutex> lock(g.m);
  g.quit = true;
}

std::string driverPath() { return fh::platform::exeDir() + "/driver/bin/linuxarm64/driver_utmtftm.so"; }

const Pose kHmdPose = {{0, 1.6, 0}, {1, 0, 0, 0}};
const Pose kLeftStart = {{0.20, 1.10, -0.30}, {std::cos(0.3926990817), 0, 0, std::sin(0.3926990817)}};
const Pose kRightStart = {{0.25, 1.12, -0.30}, {std::cos(0.2617993878), std::sin(0.2617993878), 0, 0}};

void startServer(bool hooked) {
  fakeserver::setDevicePose(kHmd, kHmdPose);
  fakeserver::setDevicePose(kLeft, kLeftStart);
  fakeserver::setDevicePose(kRight, kRightStart);
  check(fakeserver::start(driverPath().c_str(), hooked), "the real driver loads and takes the hook");
}

void scenarioPlace() {
  startServer(true);
  setScene(kGamePid);
  wait(3500);  // long enough for the program to see the game and the driver
  check(g_dashUploads > 0, "the dashboard page reached SteamVR as a Vulkan texture", g_dashUploads);
  check(gap(fakeserver::seenPose(kLeft), kLeftStart) < 1e-4, "nothing moves before anything's been placed", gap(fakeserver::seenPose(kLeft), kLeftStart) * 1000);

  click("place_left");
  wait(300);
  setDashboard(false);  // the player shuts the dashboard, which is what starts placing
  wait(600);

  {
    std::lock_guard<std::mutex> lock(g.m);
    g.grab[1] = true;  // the right hand squeezes grip on the left hand
  }
  wait(250);
  const double d[3] = {0.05, 0.03, -0.02};
  Pose right = kRightStart;
  Pose wanted = kLeftStart;
  for (int i = 0; i < 3; ++i) wanted.pos[i] += d[i];
  double worstWhileMoving = 0;
  for (int step = 1; step <= 60; ++step) {
    for (int i = 0; i < 3; ++i) right.pos[i] = kRightStart.pos[i] + d[i] * step / 60;
    fakeserver::setDevicePose(kRight, right);
    wait(16);
    Pose target = kLeftStart;
    for (int i = 0; i < 3; ++i) target.pos[i] += d[i] * step / 60;
    worstWhileMoving = std::max(worstWhileMoving, gap(fakeserver::seenPose(kLeft), target));
  }
  wait(300);
  say("while dragging, the left hand trailed by %.1f mm at worst", worstWhileMoving * 1000);
  check(worstWhileMoving < 0.03, "the hand follows the grab live (mm)", worstWhileMoving * 1000);
  check(gap(fakeserver::seenPose(kLeft), wanted) < 0.002, "the hand is where it was dragged to (mm)", gap(fakeserver::seenPose(kLeft), wanted) * 1000);
  {
    std::lock_guard<std::mutex> lock(g.m);
    g.grab[1] = false;
  }
  wait(300);
  check(gap(fakeserver::seenPose(kLeft), wanted) < 0.002, "letting go leaves it there (mm)", gap(fakeserver::seenPose(kLeft), wanted) * 1000);
  {
    std::lock_guard<std::mutex> lock(g.m);
    g.done[1] = true;  // trigger: lock it in
  }
  wait(200);
  {
    std::lock_guard<std::mutex> lock(g.m);
    g.done[1] = false;
  }
  wait(600);

  const Pose left = fakeserver::seenPose(kLeft);
  check(gap(left, wanted) < 0.002, "after saving, the game's left hand has moved by what was dragged (mm)", gap(left, wanted) * 1000);
  check(turn(left, kLeftStart) < 0.5, "and it hasn't turned (degrees)", turn(left, kLeftStart));
  check(gap(fakeserver::seenPose(kRight), right) < 1e-4, "the right hand wasn't touched (mm)", gap(fakeserver::seenPose(kRight), right) * 1000);
  check(gap(fakeserver::seenPose(kHmd), kHmdPose) < 1e-4 && turn(fakeserver::seenPose(kHmd), kHmdPose) < 0.01, "the headset wasn't touched");
  check(g_otherUploads > 0, "the bubble and the instruction panel reached SteamVR as Vulkan textures", g_otherUploads);
  check(g_rawUploads == 0, "nothing fell back to SetOverlayRaw", g_rawUploads);

  setDashboard(true);
  wait(500);
  check(gap(fakeserver::seenPose(kLeft), kLeftStart) < 1e-3, "with the dashboard open the offset stands aside, so its pointer is true (mm)", gap(fakeserver::seenPose(kLeft), kLeftStart) * 1000);
  setDashboard(false);
  wait(500);
  check(gap(fakeserver::seenPose(kLeft), wanted) < 0.002, "and comes back when it shuts (mm)", gap(fakeserver::seenPose(kLeft), wanted) * 1000);

  setDashboard(true);
  wait(300);
  click("reset_left");
  wait(400);
  setDashboard(false);
  wait(500);
  check(gap(fakeserver::seenPose(kLeft), kLeftStart) < 1e-3, "Reset puts the hand back (mm)", gap(fakeserver::seenPose(kLeft), kLeftStart) * 1000);

  // Place it once more and leave it, so there's something saved for the test script to find on disk.
  setDashboard(true);
  wait(300);
  click("place_left");
  wait(300);
  setDashboard(false);
  wait(600);
  {
    std::lock_guard<std::mutex> lock(g.m);
    g.grab[1] = true;
  }
  wait(250);
  fakeserver::setDevicePose(kRight, Pose{{kRightStart.pos[0] + 0.04, kRightStart.pos[1], kRightStart.pos[2]}, {kRightStart.rot[0], kRightStart.rot[1], kRightStart.rot[2], kRightStart.rot[3]}});
  wait(300);
  {
    std::lock_guard<std::mutex> lock(g.m);
    g.grab[1] = false;
  }
  wait(250);
  {
    std::lock_guard<std::mutex> lock(g.m);
    g.done[1] = true;
  }
  wait(200);
  {
    std::lock_guard<std::mutex> lock(g.m);
    g.done[1] = false;
  }
  wait(500);
  setDashboard(true);
  wait(500);  // a last picture of the page, with the saved offset on it
  finish();
}

void scenarioBypass() {
  startServer(false);
  setScene(kGamePid);
  wait(15000);  // the program waits 10 seconds before it gives up on hearing from the hands
  std::ifstream in(fh::platform::dataDir() + "/utmtftm.log");
  std::stringstream log;
  log << in.rdbuf();
  check(log.str().find("none from the controllers") != std::string::npos, "the program noticed the hook isn't seeing the controllers");
  finish();
}

// ---------------------------------------------------------------- the library SteamVR's loader asks for
class ClientCore {
 public:
  virtual vr::EVRInitError Init(vr::EVRApplicationType type, const char*) {
    say("Init, application type %d", int(type));
    g_scenarioName = std::getenv("FAKE_STEAMVR_SCENARIO") ? std::getenv("FAKE_STEAMVR_SCENARIO") : "";
    fillInterfaces();
    if (type == vr::VRApplication_Overlay && g_scenarioName == "place") g_scenario = std::thread(scenarioPlace);
    if (type == vr::VRApplication_Overlay && g_scenarioName == "bypass") g_scenario = std::thread(scenarioBypass);
    return vr::VRInitError_None;
  }
  virtual void Cleanup() {
    if (g_scenario.joinable()) g_scenario.join();
    fakeserver::stop();
    say("Cleanup");
  }
  virtual vr::EVRInitError IsInterfaceVersionValid(const char*) { return vr::VRInitError_None; }
  virtual void* GetGenericInterface(const char* name, vr::EVRInitError* err) {
    if (err) *err = vr::VRInitError_None;
    if (!strncmp(name, "IVRSystem_", 10)) return &g_system;
    if (!strncmp(name, "IVROverlay_", 11)) return &g_overlay;
    if (!strncmp(name, "IVRInput_", 9)) return &g_input;
    if (!strncmp(name, "IVRApplications_", 16)) return &g_apps;
    if (!strncmp(name, "IVRSettings_", 12)) return &g_settings;
    return &g_other;
  }
  virtual bool BIsHmdPresent() { return true; }
  virtual const char* GetEnglishStringForHmdError(vr::EVRInitError) { return "an error from the simulated SteamVR"; }
  virtual const char* GetIDForVRInitError(vr::EVRInitError) { return "VRInitError_Simulated"; }
};

ClientCore g_core;

}  // namespace

extern "C" __attribute__((visibility("default"))) void* VRClientCoreFactory(const char* name, int* code) {
  if (!strcmp(name, "IVRClientCore_003")) return &g_core;
  if (code) *code = vr::VRInitError_Init_InterfaceNotFound;
  return nullptr;
}

// A stand-in for SteamVR's client library, vrclient.so, so the real Frame program can run on a build machine with no
// headset. OpenVR's own loader finds it through openvrpaths.vrpath, exactly as it finds the real one, so everything from
// the program's first line down is the code that ships. tests/test_frame_run.sh drives it.
//
// It plays one scenario, picked by FAKE_STEAMVR_SCENARIO:
//   place      clicks Move on the dashboard, drags the left hand with the right, and checks what comes out of the
//              real driver, in the real poses a game would get
//   bypass     the controllers' poses never reach the driver's hook, and the program has to notice and not claim "Live"
//   addon_off  the add-on is switched off in SteamVR, and the program has to leave without showing anything
//   bindings   opens the bindings page, puts a game on another controller's layout, rewires a button and takes it all back,
//              checking what it tells SteamVR at each step
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

#include <algorithm>
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
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bindings.h"
#include "fake_vrserver.h"
#include "fixture_steamvr.h"
#include "model_files.h"
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
  uint32_t primaryDevice = kHmd;  // whichever controller the dashboard's laser is on. Nobody yet
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

Interface g_system, g_overlay, g_input, g_apps, g_settings, g_models, g_other;

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
bool sysBoolProp(void*, vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* err) {
  if (err) *err = vr::TrackedProp_Success;
  return prop == vr::Prop_HasCamera_Bool;  // a Frame says it has one
}
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
uint32_t ovPrimaryDevice(void*) {
  std::lock_guard<std::mutex> lock(g.m);
  return g.primaryDevice;
}
bool ovDashVisible(void*) {
  std::lock_guard<std::mutex> lock(g.m);
  return g.dashboardVisible;
}
std::atomic<int> g_showDashboards{0};
void ovShowDashboard(void*, const char*) {
  ++g_showDashboards;
  std::lock_guard<std::mutex> lock(g.m);
  g.dashboardVisible = true;
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
std::atomic<int> g_roomViewStyle{4};  // whatever you had: opaque
const char* setErrorName(void*, vr::EVRSettingsError) { return "a settings error"; }
bool setGetBool(void*, const char* section, const char* key, vr::EVRSettingsError* err) {
  if (g_scenarioName == "addon_off" && !strcmp(section, "driver_utmtftm") && !strcmp(key, vr::k_pch_Driver_Enable_Bool)) {
    if (err) *err = vr::VRSettingsError_None;
    return false;
  }
  if (err) *err = vr::VRSettingsError_UnsetSettingHasNoDefault;
  return false;
}
std::map<std::string, std::string> g_strings;
int32_t setGetInt32(void*, const char* section, const char* key, vr::EVRSettingsError* err) {
  if (err) *err = vr::VRSettingsError_None;
  return !strcmp(section, "camera") && !strcmp(key, "roomViewStyle") ? g_roomViewStyle.load() : 0;
}
void setSetInt32(void*, const char* section, const char* key, int32_t value, vr::EVRSettingsError* err) {
  if (!strcmp(section, "camera") && !strcmp(key, "roomViewStyle")) g_roomViewStyle = value;
  say("SetInt32 %s %s %d", section, key, value);
  if (err) *err = vr::VRSettingsError_None;
}
void setGetString(void*, const char* section, const char* key, char* out, uint32_t size, vr::EVRSettingsError* err) {
  std::lock_guard<std::mutex> lock(g.m);
  auto it = g_strings.find(std::string(section) + "/" + key);
  if (out && size) snprintf(out, size, "%s", it == g_strings.end() ? "" : it->second.c_str());
  if (err) *err = it == g_strings.end() ? vr::VRSettingsError_UnsetSettingHasNoDefault : vr::VRSettingsError_None;
}
void setSetString(void*, const char* section, const char* key, const char* value, vr::EVRSettingsError* err) {
  {
    std::lock_guard<std::mutex> lock(g.m);
    g_strings[std::string(section) + "/" + key] = value;
  }
  say("SetString %s %s %s", section, key, value);
  if (err) *err = vr::VRSettingsError_None;
}
void setRemoveKey(void*, const char* section, const char* key, vr::EVRSettingsError* err) {
  {
    std::lock_guard<std::mutex> lock(g.m);
    g_strings.erase(std::string(section) + "/" + key);
  }
  say("RemoveKey %s %s", section, key);
  if (err) *err = vr::VRSettingsError_None;
}

// ---------------------------------------------------------------- IVRRenderModels
// The made-up models from fixture_steamvr.h. A part's "file" is <model>|<part>.
uint32_t modelParts(void*, const char* model) { return uint32_t(fh::fixture::partsOf(model).size()); }
uint32_t modelPartName(void*, const char* model, uint32_t index, char* out, uint32_t size) {
  const auto parts = fh::fixture::partsOf(model);
  if (index >= parts.size()) return 0;
  if (out && size) snprintf(out, size, "%s", parts[index].name);
  return uint32_t(strlen(parts[index].name) + 1);
}
uint32_t modelPartFile(void*, const char* model, const char* part, char* out, uint32_t size) {
  const std::string file = std::string(model) + "|" + part;
  if (out && size) snprintf(out, size, "%s", file.c_str());
  return uint32_t(file.size() + 1);
}
bool modelPartState(void*, const char* model, const char* part, const vr::VRControllerState_t*, const vr::RenderModel_ControllerMode_State_t*, vr::RenderModel_ComponentState_t* st) {
  for (const auto& p : fh::fixture::partsOf(model)) {
    if (strcmp(p.name, part) != 0) continue;
    memset(st, 0, sizeof *st);
    st->mTrackingToComponentRenderModel = sysIdentity(nullptr);
    st->mTrackingToComponentLocal = sysIdentity(nullptr);
    st->uProperties = vr::VRComponentProperty_IsVisible;
    return true;
  }
  return false;
}
vr::EVRRenderModelError modelLoad(void*, const char* file, vr::RenderModel_t** out) {
  const std::string f = file;
  const size_t bar = f.find('|');
  if (bar == std::string::npos) return vr::VRRenderModelError_InvalidArg;
  for (const auto& p : fh::fixture::partsOf(f.substr(0, bar))) {
    if (f.substr(bar + 1) != p.name) continue;
    const fh::Mesh m = fh::fixture::boxMesh(p);
    auto* rm = new vr::RenderModel_t{};
    auto* v = new vr::RenderModel_Vertex_t[m.p.size()];
    auto* idx = new uint16_t[m.idx.size()];
    for (size_t i = 0; i < m.p.size(); ++i) {
      v[i] = {};
      v[i].vPosition.v[0] = m.p[i].x; v[i].vPosition.v[1] = m.p[i].y; v[i].vPosition.v[2] = m.p[i].z;
      v[i].vNormal.v[0] = m.n[i].x; v[i].vNormal.v[1] = m.n[i].y; v[i].vNormal.v[2] = m.n[i].z;
    }
    for (size_t i = 0; i < m.idx.size(); ++i) idx[i] = uint16_t(m.idx[i]);
    rm->rVertexData = v;
    rm->unVertexCount = uint32_t(m.p.size());
    rm->rIndexData = idx;
    rm->unTriangleCount = uint32_t(m.idx.size() / 3);
    *out = rm;
    return vr::VRRenderModelError_None;
  }
  return vr::VRRenderModelError_InvalidModel;
}
void modelFree(void*, vr::RenderModel_t* rm) {
  if (!rm) return;
  delete[] rm->rVertexData;
  delete[] rm->rIndexData;
  delete rm;
}

void fillInterfaces() {
  FILL(g_system, IVRSystem, GetStringTrackedDeviceProperty, sysStringProp);
  FILL(g_system, IVRSystem, GetBoolTrackedDeviceProperty, sysBoolProp);
  FILL(g_system, IVRSystem, GetTrackedDeviceIndexForControllerRole, sysRoleDevice);
  FILL(g_system, IVRSystem, GetRawZeroPoseToStandingAbsoluteTrackingPose, sysIdentity);
  FILL(g_system, IVRSystem, GetEyeToHeadTransform, sysEye);
  FILL(g_system, IVRSystem, GetDeviceToAbsoluteTrackingPose, sysDevicePoses);
  FILL(g_system, IVRSystem, PollNextEvent, sysPollEvent);

  FILL(g_overlay, IVROverlay, CreateDashboardOverlay, ovDashboard);
  FILL(g_overlay, IVROverlay, CreateOverlay, ovCreate);
  FILL(g_overlay, IVROverlay, GetOverlayErrorNameFromEnum, ovErrorName);
  FILL(g_overlay, IVROverlay, IsDashboardVisible, ovDashVisible);
  FILL(g_overlay, IVROverlay, GetPrimaryDashboardDevice, ovPrimaryDevice);
  FILL(g_overlay, IVROverlay, IsOverlayVisible, ovVisible);
  FILL(g_overlay, IVROverlay, ShowDashboard, ovShowDashboard);
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
  FILL(g_settings, IVRSettings, SetString, setSetString);
  FILL(g_settings, IVRSettings, GetInt32, setGetInt32);
  FILL(g_settings, IVRSettings, SetInt32, setSetInt32);
  FILL(g_settings, IVRSettings, RemoveKeyInSection, setRemoveKey);

  FILL(g_models, IVRRenderModels, GetComponentCount, modelParts);
  FILL(g_models, IVRRenderModels, GetComponentName, modelPartName);
  FILL(g_models, IVRRenderModels, GetComponentRenderModelName, modelPartFile);
  FILL(g_models, IVRRenderModels, GetComponentState, modelPartState);
  FILL(g_models, IVRRenderModels, LoadRenderModel_Async, modelLoad);
  FILL(g_models, IVRRenderModels, FreeRenderModel, modelFree);
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
bool g_fineArrows = false;  // the Hands page has its fine adjust arrows out
void click(const std::string& id) {
  fh::Canvas cv(1200, 760);
  std::vector<fh::Button> buttons;
  fh::DashModel m;
  m.game = "Simulated Game";
  m.live = fh::LiveState::Live;
  m.fine = g_fineArrows;
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

// The same, on the Decaf page.
void clickDecaf(const std::string& id) {
  fh::Canvas cv(1200, 760);
  std::vector<fh::Button> buttons;
  fh::DecafModel m;
  m.game = "Simulated Game";
  m.live = fh::LiveState::Live;
  fh::drawDecaf(cv, m, buttons);
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
  check(false, ("there's no button called " + id + " on the Decaf page").c_str());
}

// The same, on the bindings page. The buttons come from the page's own code, reading the same files the program does.
fh::bind::Page* g_pageForClicks = nullptr;
void clickBindings(const std::string& id) {
  fh::bind::Page& page = *g_pageForClicks;
  for (const std::string& name : page.modelsWanted()) {
    fh::Mesh mesh;
    std::vector<fh::ModelPart> parts;
    if (fh::modelFromFiles(fh::platform::steamvrDir(), name, mesh, parts)) page.give(name, mesh, parts);
  }
  fh::Canvas cv(1200, 760);
  std::vector<fh::Button> buttons;
  fh::drawBindings(cv, page.model("", ""), buttons);
  for (const fh::Button& b : buttons) {
    if (b.id != id) continue;
    const double x = b.x + b.w / 2, y = b.y + b.h / 2;
    pushMouse(vr::VREvent_MouseMove, x, y);
    wait(60);
    pushMouse(vr::VREvent_MouseButtonDown, x, y);
    wait(60);
    pushMouse(vr::VREvent_MouseButtonUp, x, y);
    say("clicked %s at %.0f, %.0f", id.c_str(), x, y);
    page.click(id);  // so this copy of the page has the same buttons as the real one next time
    return;
  }
  check(false, ("there's no button called " + id + " on the bindings page").c_str());
}

// And on a walkthrough, the same way.
void clickGuide(fh::DashPage tab, const std::string& id) {
  fh::bind::Page& page = *g_pageForClicks;
  for (const std::string& name : page.modelsWanted()) {
    fh::Mesh mesh;
    std::vector<fh::ModelPart> parts;
    if (fh::modelFromFiles(fh::platform::steamvrDir(), name, mesh, parts)) page.give(name, mesh, parts);
  }
  fh::Canvas cv(1200, 760);
  std::vector<fh::Button> buttons;
  fh::drawGuide(cv, page.guideModel(tab, "", "", 0), buttons);
  for (const fh::Button& b : buttons) {
    if (b.id != id) continue;
    const double x = b.x + b.w / 2, y = b.y + b.h / 2;
    pushMouse(vr::VREvent_MouseMove, x, y);
    wait(60);
    pushMouse(vr::VREvent_MouseButtonDown, x, y);
    wait(60);
    pushMouse(vr::VREvent_MouseButtonUp, x, y);
    say("clicked %s at %.0f, %.0f", id.c_str(), x, y);
    page.guideClick(tab, id);
    return;
  }
  check(false, ("there's no button called " + id + " on the walkthrough").c_str());
}

// And on the combos page, the same way.
void clickCombos(const std::string& id) {
  fh::bind::Page& page = *g_pageForClicks;
  for (const std::string& name : page.modelsWanted()) {
    fh::Mesh mesh;
    std::vector<fh::ModelPart> parts;
    if (fh::modelFromFiles(fh::platform::steamvrDir(), name, mesh, parts)) page.give(name, mesh, parts);
  }
  fh::Canvas cv(1200, 760);
  std::vector<fh::Button> buttons;
  fh::drawCombos(cv, page.comboModel("", ""), buttons);
  for (const fh::Button& b : buttons) {
    if (b.id != id) continue;
    const double x = b.x + b.w / 2, y = b.y + b.h / 2;
    pushMouse(vr::VREvent_MouseMove, x, y);
    wait(60);
    pushMouse(vr::VREvent_MouseButtonDown, x, y);
    wait(60);
    pushMouse(vr::VREvent_MouseButtonUp, x, y);
    say("clicked %s at %.0f, %.0f", id.c_str(), x, y);
    page.comboClick(id);
    return;
  }
  check(false, ("there's no button called " + id + " on the combos page").c_str());
}

// And on the macros page.
void clickRecorded(const std::string& id) {
  fh::bind::Page& page = *g_pageForClicks;
  fh::Canvas cv(1200, 760);
  std::vector<fh::Button> buttons;
  fh::drawRecorded(cv, page.recordedModel("", ""), buttons);
  for (const fh::Button& b : buttons) {
    if (b.id != id) continue;
    const double x = b.x + b.w / 2, y = b.y + b.h / 2;
    pushMouse(vr::VREvent_MouseMove, x, y);
    wait(60);
    pushMouse(vr::VREvent_MouseButtonDown, x, y);
    wait(60);
    pushMouse(vr::VREvent_MouseButtonUp, x, y);
    say("clicked %s at %.0f, %.0f", id.c_str(), x, y);
    page.recordedClick(id);
    return;
  }
  check(false, ("there's no button called " + id + " on the macros page").c_str());
}

void setDashboard(bool on) {
  std::lock_guard<std::mutex> lock(g.m);
  g.dashboardVisible = on;
}
void setPointing(uint32_t device) {
  std::lock_guard<std::mutex> lock(g.m);
  g.primaryDevice = device;
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
  check(fakeserver::start(driverPath().c_str(), hooked, kLeft, kRight), "the real driver loads and takes the hook");
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
  // The globes are as they were in 0.4.0: placing a hand leaves Room View and the Steam button alone.
  check(g_roomViewStyle == 4, "placing a hand leaves Room View's style as it was", g_roomViewStyle.load());
  {
    int presses = 0;
    for (const auto& b : fakeserver::heardButtons()) if (b.first == "R/input/system/click" && b.second == 1.f) ++presses;
    check(presses == 0, "and never presses the Steam button", presses);
  }

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

std::string settingNow(const std::string& name) {
  std::lock_guard<std::mutex> lock(g.m);
  auto it = g_strings.find(name);
  return it == g_strings.end() ? "" : it->second;
}

std::string logText() {
  std::ifstream in(fh::platform::dataDir() + "/utmtftm.log");
  std::stringstream log;
  log << in.rdbuf();
  return log.str();
}
// The log carries on from run to run, so a scenario can look at only what it wrote itself.
bool logHasSince(size_t from, const std::string& what) {
  const std::string all = logText();
  return all.size() > from && all.find(what, from) != std::string::npos;
}

bool logHas(const std::string& what) {
  std::ifstream in(fh::platform::dataDir() + "/utmtftm.log");
  std::stringstream log;
  log << in.rdbuf();
  return log.str().find(what) != std::string::npos;
}

void scenarioBindings() {
  // SteamVR's files for two controllers and two games, where the program will look for them.
  const std::string steamvr = fh::platform::steamvrDir(), config = fh::platform::configDirs()[0];
  fh::fixture::writeSteamVR(steamvr, config, fh::platform::dataDir() + "/../fake-games");
  fh::bind::Dirs dirs;
  dirs.steamvr = steamvr;
  dirs.steam = fh::platform::steamDir();
  dirs.data = fh::platform::dataDir() + "/../fake-page-data";
  dirs.config = fh::platform::configDirs();
  fh::bind::Page page(dirs, fh::bind::Settings{[](const std::string&, const std::string&) { return std::string(); }, [](const std::string&, const std::string&, const std::string&) { return true; }, [](const std::string&, const std::string&) {}});
  page.load();
  page.setYours("frame_controller", "frame_controller_left", "frame_controller_right");
  page.setGame("steam.app.12345", "steam.app.12345", "Simulated Game");
  g_pageForClicks = &page;

  startServer(true);
  setScene(kGamePid);
  wait(3500);
  const std::string setting = "steam.app.12345/frame_controller_250820_CurrentURL_openxr";
  const int beforePage = g_dashUploads;
  click("page_bindings");
  wait(1500);
  check(g_dashUploads > beforePage, "opening the page sends SteamVR a new picture", g_dashUploads - beforePage);
  check(logHas("read SteamVR's controller files"), "the program read the controller files");
  check(logHas("bindings page: frame_controller_left loaded, 7 parts"), "and loaded your left controller's model, in parts");
  check(logHas("bindings page: oculus_quest_plus_controller_right loaded, 7 parts"), "and the other controller's");
  check(settingNow(setting).empty(), "just looking changes nothing");

  // The tab opens on the walkthrough. Swapping two buttons there is done by the driver, live, without touching the game's
  // binding: with the dashboard shut, pressing A reaches the game as B. Undo puts A back.
  g_pageForClicks->setHookState(true, true);  // the program can see the hook by now, so this copy takes the same path
  clickGuide(fh::DashPage::Bindings, "g:right/input/a");
  wait(300);
  clickGuide(fh::DashPage::Bindings, "g:right/input/b");
  wait(600);
  check(settingNow(setting).empty(), "swapping A and B on the walkthrough leaves the game's binding alone");
  setDashboard(false);
  wait(800);
  fakeserver::clearHeardButtons();
  fakeserver::reportButton(1, "/input/a/click", 1);
  wait(100);
  fakeserver::reportButton(1, "/input/a/click", 0);
  wait(100);
  {
    bool b = false, a = false;
    for (const auto& [who, v] : fakeserver::heardButtons()) {
      b = b || (who == "R/input/b/click" && v == 1.f);
      a = a || (who == "R/input/a/click" && v == 1.f);
    }
    check(b && !a, "and with the dashboard shut, pressing A reaches the game as B");
  }
  setDashboard(true);
  wait(600);
  clickGuide(fh::DashPage::Bindings, "guide_bind_undo");
  wait(600);
  setDashboard(false);
  wait(800);
  fakeserver::clearHeardButtons();
  fakeserver::reportButton(1, "/input/a/click", 1);
  wait(100);
  fakeserver::reportButton(1, "/input/a/click", 0);
  wait(100);
  {
    bool a = false;
    for (const auto& [who, v] : fakeserver::heardButtons()) a = a || (who == "R/input/a/click" && v == 1.f);
    check(a, "and after Undo, A is A again");
  }
  setDashboard(true);
  wait(600);
  clickGuide(fh::DashPage::Bindings, "guide_advanced");
  wait(400);

  clickBindings("bind_layout");
  wait(600);
  const std::string url = settingNow(setting);
  check(url.rfind("file:///", 0) == 0, "putting the game on the Quest layout points its setting at a file");
  const std::string file = "/" + url.substr(8);
  {
    std::ifstream in(file);
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errs;
    check(in && Json::parseFromStream(b, in, &v, &errs), "and the file's there, at that path, and reads");
    check(v["controller_type"].asString() == "frame_controller", "it's for the Frame");
    check(v["interaction_profile"].asString() == "/interaction_profiles/oculus/touch_controller", "and the game goes on seeing a Quest controller");
  }

  clickBindings("y:right/input/x");
  wait(300);
  clickBindings("t:right/input/a");
  wait(600);
  {
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    check(text.str().find("/user/hand/right/input/x") != std::string::npos, "rewiring X rewrites the file");
  }
  clickBindings("bind_reset_wiring");
  wait(400);

  const int beforeHands = g_dashUploads;
  click("page_hands");
  wait(500);
  check(g_dashUploads > beforeHands, "the Hands tab goes back", g_dashUploads - beforeHands);
  check(settingNow(setting) == url, "and leaves the layout alone");
  click("page_bindings");
  wait(500);

  clickBindings("bind_dev");
  wait(500);
  check(settingNow(setting).empty(), "going back to the developer's takes the setting off");
  check(access(file.c_str(), F_OK) != 0, "and our file's gone");

  // The default for all games reaches a game the next time it starts.
  clickBindings("bind_scope_global");
  wait(200);
  clickBindings("bind_layout");
  wait(500);
  check(settingNow(setting).rfind("file:///", 0) == 0, "all games includes this one");
  clickBindings("bind_scope_game");
  wait(200);
  clickBindings("bind_dev");
  wait(500);
  check(settingNow(setting).empty(), "and this game can opt out");
  setScene(0);
  wait(1500);
  setScene(kGamePid);
  wait(1500);
  check(settingNow(setting).empty(), "it stays out when it starts again");
  check(g_rawUploads == 0, "nothing fell back to SetOverlayRaw", g_rawUploads);
  g_pageForClicks = nullptr;
  finish();
}

// Macros, through the real program and the real driver, from the Combos tab to a press in the game.
// Records a macro the way the program does it: with the dashboard shut and a sign in front of you. Steam Link runs the dashboard
// from the headset, and while it's open the PC hears no buttons at all (J4's PC, 8 November), and J4 liked it better this way
// on the Frame too. Steam Link also says every button again about 360 times a second, changed or not, and that flood is in here.
void scenarioRecorded() {
  const size_t logStart = logText().size();
  const std::string steamvr = fh::platform::steamvrDir(), config = fh::platform::configDirs()[0];
  fh::fixture::writeSteamVR(steamvr, config, fh::platform::dataDir() + "/../fake-games");
  fh::bind::Dirs dirs;
  dirs.steamvr = steamvr;
  dirs.steam = fh::platform::steamDir();
  dirs.data = fh::platform::dataDir() + "/../fake-page-data";
  dirs.config = fh::platform::configDirs();
  fh::bind::Page page(dirs, fh::bind::Settings{[](const std::string&, const std::string&) { return std::string(); }, [](const std::string&, const std::string&, const std::string&) { return true; }, [](const std::string&, const std::string&) {}});
  page.load();
  page.setYours("frame_controller", "frame_controller_left", "frame_controller_right");
  page.setGame("steam.app.12345", "steam.app.12345", "Simulated Game");
  page.setHookState(true, true);
  page.setRecordShut(true);
  g_pageForClicks = &page;

  startServer(true);
  setScene(kGamePid);
  wait(3500);
  using fakeserver::reportButton;
  const auto heard = [](const std::string& who, float v) {
    for (const auto& b : fakeserver::heardButtons()) if (b.first == who && b.second == v) return true;
    return false;
  };
  const auto where = [](const std::string& who, float v) {
    const auto all = fakeserver::heardButtons();
    for (size_t i = 0; i < all.size(); ++i) if (all[i].first == who && all[i].second == v) return int(i);
    return -1;
  };
  reportButton(1, "/input/thumbstick/touch", 1);
  reportButton(1, "/input/thumbstick/touch", 0);
  wait(1200);
  check(logHasSince(logStart, "button hook: ready"), "the program knows the driver can hear the buttons");

  click("page_combos");
  wait(800);
  clickGuide(fh::DashPage::Combos, "guide_advanced");
  wait(400);
  clickCombos("page_recorded");
  wait(800);

  uint32_t t = 1000;
  const auto copy = [&](int hand, const char* name, float v) { t += 60; fh::macro::Raw r; r.ms = t; r.hand = hand; r.name = name; r.value = v; page.recordedReport(r); };
  page.recordedDashboard(true);
  clickRecorded("macro_record");
  wait(1500);
  check(logHasSince(logStart, "macros: waiting for the dashboard to shut"), "Record new waits for the dashboard to shut");
  check(!logHasSince(logStart, "macros: listening"), "and isn't listening yet");
  fakeserver::clearHeardButtons();
  reportButton(1, "/input/a/click", 1);
  wait(50);
  reportButton(1, "/input/a/click", 0);
  check(heard("R/input/a/click", 1.f), "nothing's held back while it waits");

  const int signsBefore = g_otherUploads;
  setDashboard(false);
  page.recordedDashboard(false);
  page.recordedTick(0, 0);
  page.recordedTick(3000, 0);
  wait(800);
  check(logHasSince(logStart, "macros: listening, with the dashboard shut"), "shut, it listens");
  check(g_otherUploads > signsBefore, "and puts a sign up in front of you", g_otherUploads - signsBefore);
  wait(2800);  // the countdown

  // Steam Link's flood, around the real presses. A thousand reports at a time is four rings' worth.
  const char* same[] = {"/input/x/click", "/input/y/click", "/input/grip/touch", "/input/menu/click", "/input/thumbrest/touch"};
  const auto flood = [&] { for (int i = 0; i < 200; ++i) for (const char* n : same) reportButton(1, n, 0); };
  fakeserver::clearHeardButtons();
  flood();
  reportButton(1, "/input/a/touch", 1);
  reportButton(1, "/input/a/click", 1); copy(1, "/input/a/click", 1);
  flood();
  wait(150);
  reportButton(1, "/input/trigger/value", 1);
  reportButton(1, "/input/trigger/click", 1); copy(1, "/input/trigger/click", 1);
  flood();
  wait(150);
  reportButton(1, "/input/trigger/click", 0); copy(1, "/input/trigger/click", 0);
  reportButton(1, "/input/trigger/value", 0);
  flood();
  wait(150);
  reportButton(1, "/input/a/click", 0); copy(1, "/input/a/click", 0);
  reportButton(1, "/input/a/touch", 0);
  check(!heard("R/input/a/click", 1.f) && !heard("R/input/trigger/click", 1.f), "the game doesn't get any of it");
  wait(2600);
  page.recordedTick(4000, t + 5000);
  check(logHasSince(logStart, "macros: stopped listening"), "it stops by itself");
  check(logHasSince(logStart, " 0 lost,") && logHasSince(logStart, "R/input/trigger/click x"), "and lost nothing in the flood, trigger included");
  check(g_showDashboards >= 1 && logHasSince(logStart, "opening it again"), "then opens the dashboard again by itself", g_showDashboards);
  page.recordedDashboard(false);
  page.recordedDashboard(true);

  // Choosing the button, the same way.
  clickRecorded("macro_pick");
  wait(600);
  const int opened = g_showDashboards;
  setDashboard(false);
  page.recordedDashboard(false);
  page.recordedTick(0, 0);
  wait(600);
  reportButton(1, "/input/b/click", 1); copy(1, "/input/b/click", 1);
  wait(100);
  reportButton(1, "/input/b/click", 0); copy(1, "/input/b/click", 0);
  wait(600);
  check(g_showDashboards > opened, "after B's let go of, the dashboard opens again", g_showDashboards);
  page.recordedDashboard(false);
  page.recordedDashboard(true);
  clickRecorded("macro_save");
  wait(800);

  // In the game: B plays it.
  setDashboard(false);
  wait(600);
  fakeserver::clearHeardButtons();
  reportButton(1, "/input/b/click", 1);
  wait(500);
  reportButton(1, "/input/b/click", 0);
  wait(100);
  check(!heard("R/input/b/click", 1.f), "the game doesn't see B");
  const int aDown = where("R/input/a/click", 1.f), trigDown = where("R/input/trigger/click", 1.f), trigUp = where("R/input/trigger/click", 0.f), aUp = where("R/input/a/click", 0.f);
  check(aDown >= 0 && trigDown > aDown && trigUp > trigDown && aUp > trigUp, "it sees A, then the trigger, then the trigger up, then A up", aDown);
  check(g_rawUploads == 0, "nothing fell back to SetOverlayRaw", g_rawUploads);
  g_pageForClicks = nullptr;
  finish();
}

// Fine adjust with the dashboard open: every tap reaches the real driver, on the hand that isn't pointing.
void scenarioFine() {
  startServer(true);
  setScene(kGamePid);
  wait(3500);
  setPointing(kRight);
  wait(300);
  check(gap(fakeserver::seenPose(kLeft), kLeftStart) < 1e-4, "nothing's moved yet");

  click("fine_toggle");
  wait(400);
  g_fineArrows = true;
  click("fine_big");
  wait(300);
  click("nudge_0_0_p");  // the left hand, 1 cm forward
  wait(500);
  check(std::fabs(gap(fakeserver::seenPose(kLeft), kLeftStart) - 0.01) < 5e-4, "with the dashboard open, tapping moves the hand that isn't pointing (mm)", gap(fakeserver::seenPose(kLeft), kLeftStart) * 1000);
  check(gap(fakeserver::seenPose(kRight), kRightStart) < 1e-4, "and leaves the other alone");
  click("nudge_0_0_m");
  wait(500);
  check(gap(fakeserver::seenPose(kLeft), kLeftStart) < 1e-4, "tapping back puts it back (mm)", gap(fakeserver::seenPose(kLeft), kLeftStart) * 1000);

  click("fine_small");
  wait(300);
  for (int i = 0; i < 3; ++i) {
    click("nudge_0_4_p");  // 1 degree to the right
    wait(200);
  }
  wait(400);
  check(std::fabs(turn(fakeserver::seenPose(kLeft), kLeftStart) - 3) < 0.1, "three taps turn it three degrees", turn(fakeserver::seenPose(kLeft), kLeftStart));

  setPointing(kLeft);
  wait(500);
  check(turn(fakeserver::seenPose(kLeft), kLeftStart) < 0.05, "point with that hand and it stands aside, so the laser's true", turn(fakeserver::seenPose(kLeft), kLeftStart));
  setPointing(kRight);
  wait(500);
  check(std::fabs(turn(fakeserver::seenPose(kLeft), kLeftStart) - 3) < 0.1, "and it comes back when you point with the other", turn(fakeserver::seenPose(kLeft), kLeftStart));

  click("nudge_1_1_p");  // the right hand, the one pointing, 1 mm up
  wait(500);
  check(gap(fakeserver::seenPose(kRight), kRightStart) < 1e-4, "the hand that's pointing doesn't move yet", gap(fakeserver::seenPose(kRight), kRightStart) * 1000);
  setDashboard(false);
  wait(500);
  check(std::fabs(gap(fakeserver::seenPose(kRight), kRightStart) - 0.001) < 2e-4, "and does when the dashboard shuts (mm)", gap(fakeserver::seenPose(kRight), kRightStart) * 1000);
  check(std::fabs(turn(fakeserver::seenPose(kLeft), kLeftStart) - 3) < 0.1, "the other's still turned");

  setDashboard(true);
  wait(300);
  click("fine_toggle");
  g_fineArrows = false;
  wait(500);
  check(turn(fakeserver::seenPose(kLeft), kLeftStart) < 0.05 && gap(fakeserver::seenPose(kRight), kRightStart) < 1e-4, "out of fine adjust, the dashboard has both hands true again");
  check(logHas("fine adjust left") && logHas("fine adjust right"), "the log says what was done");
  check(g_rawUploads == 0, "nothing fell back to SetOverlayRaw", g_rawUploads);
  finish();
}

// Decaffeinate: tapped on its page in the real program, steadying both hands through the real driver.
// Both hands shake by about a third of a degree and a millimetre, the way tracking noise and tremor do; this measures how
// much of that reaches SteamVR, and so every game.
void scenarioDecaf() {
  startServer(true);
  setScene(kGamePid);
  wait(3500);
  std::mt19937 rng(5);
  std::normal_distribution<double> n01(0, 1);
  auto shaken = [&](const Pose& base) {
    Pose p = base;
    const double q[4] = {1, n01(rng) * 0.0015, n01(rng) * 0.0015, n01(rng) * 0.0015};  // half-angles: about 0.3 degrees all told
    double r[4] = {q[0] * base.rot[0] - q[1] * base.rot[1] - q[2] * base.rot[2] - q[3] * base.rot[3],
                   q[0] * base.rot[1] + q[1] * base.rot[0] + q[2] * base.rot[3] - q[3] * base.rot[2],
                   q[0] * base.rot[2] - q[1] * base.rot[3] + q[2] * base.rot[0] + q[3] * base.rot[1],
                   q[0] * base.rot[3] + q[1] * base.rot[2] - q[2] * base.rot[1] + q[3] * base.rot[0]};
    const double n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    for (int i = 0; i < 4; ++i) p.rot[i] = r[i] / n;
    for (int i = 0; i < 3; ++i) p.pos[i] += n01(rng) * 0.001;
    return p;
  };
  // Shakes both hands for a while, and says how far each one SteamVR sees strays from where it really is, typically.
  auto shake = [&](int ms, double& left, double& right) {
    double sl = 0, sr = 0;
    int n = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms / 3);
    while (std::chrono::steady_clock::now() < end) {
      fakeserver::setDevicePose(kLeft, shaken(kLeftStart));
      fakeserver::setDevicePose(kRight, shaken(kRightStart));
      wait(4);
      if (std::chrono::steady_clock::now() < settle) continue;
      const double l = turn(fakeserver::seenPose(kLeft), kLeftStart), r = turn(fakeserver::seenPose(kRight), kRightStart);
      sl += l * l; sr += r * r;
      ++n;
    }
    fakeserver::setDevicePose(kLeft, kLeftStart);
    fakeserver::setDevicePose(kRight, kRightStart);
    left = std::sqrt(sl / std::max(n, 1));
    right = std::sqrt(sr / std::max(n, 1));
  };

  setPointing(kRight);
  click("page_decaf");
  wait(400);
  double offL, offR;
  shake(1500, offL, offR);
  say("off: left hand shakes %.3f degrees, right %.3f", offL, offR);
  check(offL > 0.1 && offR > 0.1, "with it off, the shake gets through to the game as it is (degrees)", offL);

  clickDecaf("decaf_2");  // Strong, for this game
  wait(400);
  check(logHas("decaffeinate: step 2 for steam.app.12345"), "the program handed the driver the step for this game");
  double onL, onR;
  shake(2400, onL, onR);
  say("Strong with the dashboard open, the right hand pointing: left %.3f degrees, right %.3f", onL, onR);
  check(onL < offL / 3, "Strong takes at least two thirds of the left hand's shake out (degrees)", onL);
  check(onR > offR * 0.7, "the hand pointing at the dashboard is left alone, so its laser is true (degrees)", onR);

  setDashboard(false);
  wait(300);
  double shutL, shutR;
  shake(2400, shutL, shutR);
  say("Strong with the dashboard shut: left %.3f degrees, right %.3f", shutL, shutR);
  check(shutL < offL / 3 && shutR < offR / 3, "with the dashboard shut, both hands steady (degrees, right)", shutR);

  setDashboard(true);
  wait(300);
  clickDecaf("decaf_0");
  wait(400);
  double backL, backR;
  shake(1500, backL, backR);
  check(backL > offL * 0.7, "Off puts the hands back exactly as tracked (degrees)", backL);
  clickDecaf("decaf_3");  // left on Maximum, so the run can check it was saved
  wait(400);
  check(g_rawUploads == 0, "nothing fell back to SetOverlayRaw", g_rawUploads);
  finish();
}

// Making a combo on the page, in the real program, and what lands in SteamVR's setting and the binding file.
void scenarioCombos() {
  const std::string steamvr = fh::platform::steamvrDir(), config = fh::platform::configDirs()[0];
  fh::fixture::writeSteamVR(steamvr, config, fh::platform::dataDir() + "/../fake-games");
  fh::bind::Dirs dirs;
  dirs.steamvr = steamvr;
  dirs.steam = fh::platform::steamDir();
  dirs.data = fh::platform::dataDir() + "/../fake-page-data";
  dirs.config = fh::platform::configDirs();
  fh::bind::Page page(dirs, fh::bind::Settings{[](const std::string&, const std::string&) { return std::string(); }, [](const std::string&, const std::string&, const std::string&) { return true; }, [](const std::string&, const std::string&) {}});
  page.load();
  page.setYours("frame_controller", "frame_controller_left", "frame_controller_right");
  page.setGame("steam.app.12345", "steam.app.12345", "Simulated Game");
  g_pageForClicks = &page;

  startServer(true);
  setScene(kGamePid);
  wait(3500);
  const std::string setting = "steam.app.12345/frame_controller_250820_CurrentURL_openxr";
  const int before = g_dashUploads;
  click("page_combos");
  wait(1500);
  check(g_dashUploads > before, "opening the page sends SteamVR a new picture", g_dashUploads - before);
  check(settingNow(setting).empty(), "just looking changes nothing");
  clickGuide(fh::DashPage::Combos, "guide_advanced");
  wait(400);
  clickCombos("combo_new");
  wait(300);
  clickCombos("c:left/input/dpad_up");
  wait(200);
  clickCombos("c:left/input/dpad_down");
  wait(200);
  clickCombos("combo_next");
  wait(300);
  const fh::ComboModel choosing = page.comboModel("", "");
  check(!choosing.choices.empty(), "there's something to pick");
  std::string primary;
  for (const fh::ComboChoice& e : choosing.choices) if (e.name == "Primary") primary = e.id;
  check(!primary.empty(), "including Primary");
  clickCombos(primary);
  wait(600);
  const std::string url = settingNow(setting);
  check(url.rfind("file:///", 0) == 0, "saving it points the game's setting at a file");
  const std::string file = "/" + url.substr(8);
  {
    std::ifstream in(file);
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errs;
    check(in && Json::parseFromStream(b, in, &v, &errs), "and the file's there, at that path, and reads");
    const Json::Value& chords = v["bindings"]["/actions/oculustouchcontroller"]["chords"];
    check(chords.size() == 1 && chords[0]["output"].asString() == "/actions/oculustouchcontroller/in/primarybutton", "with the chord in it, set to what was picked");
    check(chords.size() == 1 && chords[0]["inputs"].size() == 2 && chords[0]["inputs"][0][1].asString() == "touch", "on the controls that were tapped, as touches");
  }
  const int beforeHands = g_dashUploads;
  click("page_hands");
  wait(500);
  check(g_dashUploads > beforeHands, "the Hands tab goes back", g_dashUploads - beforeHands);
  click("page_combos");
  wait(500);
  setScene(0);
  wait(1500);
  setScene(kGamePid);
  wait(1500);
  check(settingNow(setting) == url, "and it's still set after the game closes and starts again");
  clickCombos("combo_del_0");
  wait(600);
  check(settingNow(setting).empty(), "removing it takes the setting off");
  check(access(file.c_str(), F_OK) != 0, "and our file's gone");
  check(g_rawUploads == 0, "nothing fell back to SetOverlayRaw", g_rawUploads);
  g_pageForClicks = nullptr;
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
    if (type == vr::VRApplication_Overlay && g_scenarioName == "bindings") g_scenario = std::thread(scenarioBindings);
    if (type == vr::VRApplication_Overlay && g_scenarioName == "fine") g_scenario = std::thread(scenarioFine);
    if (type == vr::VRApplication_Overlay && g_scenarioName == "combos") g_scenario = std::thread(scenarioCombos);
    if (type == vr::VRApplication_Overlay && g_scenarioName == "recorded") g_scenario = std::thread(scenarioRecorded);
    if (type == vr::VRApplication_Overlay && g_scenarioName == "decaf") g_scenario = std::thread(scenarioDecaf);
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
    if (!strncmp(name, "IVRRenderModels_", 16)) return &g_models;
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

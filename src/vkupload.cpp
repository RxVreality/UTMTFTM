// The Linux side of overlay_upload.h: overlay images go to SteamVR as Vulkan textures. This came over from the first
// Steam Frame build, where it ran in the headset.
#include "overlay_upload.h"

#include <vulkan/vulkan.h>

#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace fh {

namespace {
constexpr int kRing = 3;  // images per overlay, so we never rewrite the one SteamVR was just given

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string w;
  while (in >> w) out.push_back(w);
  return out;
}
}  // namespace

struct OverlayUploader::Impl {
  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties mem{};

  struct Slot {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory imgMem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory bufMem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool initialised = false;  // the image has been through one upload (its layout is TRANSFER_SRC_OPTIMAL)
  };
  struct Target {
    uint32_t w = 0, h = 0;
    Slot slot[kRing];
    int next = 0;
  };
  std::map<vr::VROverlayHandle_t, Target> targets;

  int memType(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
  }

  void destroySlot(Slot& s) {
    if (s.fence) vkDestroyFence(dev, s.fence, nullptr);
    if (s.cmd) vkFreeCommandBuffers(dev, pool, 1, &s.cmd);
    if (s.mapped) vkUnmapMemory(dev, s.bufMem);
    if (s.buf) vkDestroyBuffer(dev, s.buf, nullptr);
    if (s.bufMem) vkFreeMemory(dev, s.bufMem, nullptr);
    if (s.img) vkDestroyImage(dev, s.img, nullptr);
    if (s.imgMem) vkFreeMemory(dev, s.imgMem, nullptr);
    s = Slot{};
  }

  bool createSlot(Slot& s, uint32_t w, uint32_t h) {
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = VK_FORMAT_R8G8B8A8_UNORM;
    ic.extent = {w, h, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ic, nullptr, &s.img) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, s.img, &r);
    int t = memType(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) t = memType(r.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (t < 0 || vkAllocateMemory(dev, &ai, nullptr, &s.imgMem) != VK_SUCCESS) return false;
    vkBindImageMemory(dev, s.img, s.imgMem, 0);

    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = VkDeviceSize(w) * h * 4;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bc, nullptr, &s.buf) != VK_SUCCESS) return false;
    vkGetBufferMemoryRequirements(dev, s.buf, &r);
    t = memType(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (t < 0 || vkAllocateMemory(dev, &ai, nullptr, &s.bufMem) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, s.buf, s.bufMem, 0);
    if (vkMapMemory(dev, s.bufMem, 0, VK_WHOLE_SIZE, 0, &s.mapped) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ca, &s.cmd) != VK_SUCCESS) return false;
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return vkCreateFence(dev, &fc, nullptr, &s.fence) == VK_SUCCESS;
  }

  void destroyTarget(Target& t) {
    for (auto& s : t.slot) destroySlot(s);
    t = Target{};
  }
};

OverlayUploader::OverlayUploader() : p(new Impl) {}
OverlayUploader::~OverlayUploader() { shutdown(); delete p; }
bool OverlayUploader::ready() const { return p->dev != VK_NULL_HANDLE; }

bool OverlayUploader::init(std::string* info) {
  auto fail = [&](const std::string& e) {
    if (info) *info = e;
    shutdown();
    return false;
  };
  auto C = vr::VRCompositor();
  if (!C) return fail("no IVRCompositor");

  // Instance, with the extensions SteamVR needs to take our images.
  std::string want;
  if (uint32_t n = C->GetVulkanInstanceExtensionsRequired(nullptr, 0)) {
    std::vector<char> b(n);
    C->GetVulkanInstanceExtensionsRequired(b.data(), n);
    want = b.data();
  }
  uint32_t n = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> have(n);
  vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
  std::set<std::string> haveSet;
  for (auto& e : have) haveSet.insert(e.extensionName);
  std::vector<std::string> instExt;
  for (auto& e : split(want)) if (haveSet.count(e)) instExt.push_back(e);
  std::vector<const char*> instPtr;
  for (auto& e : instExt) instPtr.push_back(e.c_str());

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "UTMTFTM";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ii.pApplicationInfo = &app;
  ii.enabledExtensionCount = (uint32_t)instPtr.size();
  ii.ppEnabledExtensionNames = instPtr.data();
  if (vkCreateInstance(&ii, nullptr, &p->inst) != VK_SUCCESS) return fail("vkCreateInstance failed");

  // The GPU the headset is driven by.
  uint64_t out = 0;
  vr::VRSystem()->GetOutputDevice(&out, vr::TextureType_Vulkan, (VkInstance_T*)p->inst);
  p->pd = (VkPhysicalDevice)out;
  if (!p->pd) {
    uint32_t c = 0;
    vkEnumeratePhysicalDevices(p->inst, &c, nullptr);
    std::vector<VkPhysicalDevice> v(c);
    vkEnumeratePhysicalDevices(p->inst, &c, v.data());
    if (v.empty()) return fail("no Vulkan device");
    p->pd = v[0];
  }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(p->pd, &props);
  vkGetPhysicalDeviceMemoryProperties(p->pd, &p->mem);

  want.clear();
  if (uint32_t m = C->GetVulkanDeviceExtensionsRequired((VkPhysicalDevice_T*)p->pd, nullptr, 0)) {
    std::vector<char> b(m);
    C->GetVulkanDeviceExtensionsRequired((VkPhysicalDevice_T*)p->pd, b.data(), m);
    want = b.data();
  }
  vkEnumerateDeviceExtensionProperties(p->pd, nullptr, &n, nullptr);
  have.resize(n);
  vkEnumerateDeviceExtensionProperties(p->pd, nullptr, &n, have.data());
  haveSet.clear();
  for (auto& e : have) haveSet.insert(e.extensionName);
  std::vector<std::string> devExt;
  for (auto& e : split(want)) if (haveSet.count(e)) devExt.push_back(e);
  std::vector<const char*> devPtr;
  for (auto& e : devExt) devPtr.push_back(e.c_str());

  uint32_t qc = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(p->pd, &qc, nullptr);
  std::vector<VkQueueFamilyProperties> q(qc);
  vkGetPhysicalDeviceQueueFamilyProperties(p->pd, &qc, q.data());
  bool found = false;
  for (uint32_t i = 0; i < qc && !found; ++i)
    if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { p->family = i; found = true; }
  if (!found) return fail("no graphics queue");

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = p->family;
  qi.queueCount = 1;
  qi.pQueuePriorities = &prio;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  di.enabledExtensionCount = (uint32_t)devPtr.size();
  di.ppEnabledExtensionNames = devPtr.data();
  if (vkCreateDevice(p->pd, &di, nullptr, &p->dev) != VK_SUCCESS) return fail("vkCreateDevice failed");
  vkGetDeviceQueue(p->dev, p->family, 0, &p->queue);

  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pc.queueFamilyIndex = p->family;
  if (vkCreateCommandPool(p->dev, &pc, nullptr, &p->pool) != VK_SUCCESS) return fail("vkCreateCommandPool failed");
  if (info) {
    *info = std::string(props.deviceName) + " (instance extensions:";
    for (auto& e : instExt) *info += " " + e;
    *info += "; device extensions:";
    for (auto& e : devExt) *info += " " + e;
    *info += ")";
  }
  return true;
}

vr::EVROverlayError OverlayUploader::upload(vr::VROverlayHandle_t o, const uint8_t* rgba, uint32_t w, uint32_t h) {
  if (!ready() || !w || !h) return vr::VROverlayError_RequestFailed;
  Impl::Target& t = p->targets[o];
  if (t.w != w || t.h != h) {
    vkQueueWaitIdle(p->queue);
    p->destroyTarget(t);
    for (auto& s : t.slot)
      if (!p->createSlot(s, w, h)) { p->destroyTarget(t); return vr::VROverlayError_RequestFailed; }
    t.w = w; t.h = h;
  }
  Impl::Slot& s = t.slot[t.next];
  t.next = (t.next + 1) % kRing;

  if (vkWaitForFences(p->dev, 1, &s.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) return vr::VROverlayError_RequestFailed;
  vkResetFences(p->dev, 1, &s.fence);
  std::memcpy(s.mapped, rgba, size_t(w) * h * 4);

  vkResetCommandBuffer(s.cmd, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(s.cmd, &bi);
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = s.img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  // Wait for anything SteamVR queued on this image (its copy runs on our queue), then overwrite it.
  b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.oldLayout = s.initialised ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  VkBufferImageCopy r{};
  r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  r.imageExtent = {w, h, 1};
  vkCmdCopyBufferToImage(s.cmd, s.buf, s.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
  // SteamVR wants images it's given in TRANSFER_SRC_OPTIMAL (openvr wiki, Vulkan).
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  vkEndCommandBuffer(s.cmd);

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &s.cmd;
  if (vkQueueSubmit(p->queue, 1, &si, s.fence) != VK_SUCCESS) return vr::VROverlayError_RequestFailed;
  // The image must hold the new pixels before SteamVR copies it.
  if (vkWaitForFences(p->dev, 1, &s.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) return vr::VROverlayError_RequestFailed;
  s.initialised = true;

  vr::VRVulkanTextureData_t vk{};
  vk.m_nImage = (uint64_t)s.img;
  vk.m_pDevice = (VkDevice_T*)p->dev;
  vk.m_pPhysicalDevice = (VkPhysicalDevice_T*)p->pd;
  vk.m_pInstance = (VkInstance_T*)p->inst;
  vk.m_pQueue = (VkQueue_T*)p->queue;
  vk.m_nQueueFamilyIndex = p->family;
  vk.m_nWidth = w;
  vk.m_nHeight = h;
  vk.m_nFormat = VK_FORMAT_R8G8B8A8_UNORM;
  vk.m_nSampleCount = 1;
  vr::Texture_t tex{&vk, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
  return vr::VROverlay()->SetOverlayTexture(o, &tex);
}

void OverlayUploader::shutdown() {
  if (p->dev) {
    vkDeviceWaitIdle(p->dev);
    for (auto& [o, t] : p->targets) p->destroyTarget(t);
    p->targets.clear();
    if (p->pool) vkDestroyCommandPool(p->dev, p->pool, nullptr);
    vkDestroyDevice(p->dev, nullptr);
  }
  if (p->inst) vkDestroyInstance(p->inst, nullptr);
  p->pool = VK_NULL_HANDLE;
  p->dev = VK_NULL_HANDLE;
  p->inst = VK_NULL_HANDLE;
  p->pd = VK_NULL_HANDLE;
}

}  // namespace fh

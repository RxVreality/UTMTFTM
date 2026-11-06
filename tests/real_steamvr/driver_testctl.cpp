// A pretend right-hand controller, loaded by the real SteamVR, for tests/real_steamvr/real_hook_check.cpp. It sets its buttons
// up and reports them through IVRDriverInput the way Valve's own drivers do, so the add-on's hook gets tried against the real
// vrserver with no headset. Not shipped. The check program tells it what to press with DriverDebugRequest: "/input/a/click 1".
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <openvr_driver.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {

vr::IVRDriverInput* g_input = nullptr;

struct Ctl : vr::ITrackedDeviceServerDriver {
  uint32_t id = vr::k_unTrackedDeviceIndexInvalid;
  std::map<std::string, vr::VRInputComponentHandle_t> bools, scalars;

  vr::EVRInitError Activate(uint32_t i) override {
    id = i;
    const auto c = vr::VRProperties()->TrackedDeviceToPropertyContainer(i);
    vr::VRProperties()->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_RightHand);
    vr::VRProperties()->SetStringProperty(c, vr::Prop_ControllerType_String, "knuckles");
    vr::VRProperties()->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{indexcontroller}/input/index_controller_profile.json");
    vr::VRProperties()->SetStringProperty(c, vr::Prop_ModelNumber_String, "UTMTFTM test controller");
    vr::VRProperties()->SetStringProperty(c, vr::Prop_RenderModelName_String, "{indexcontroller}valve_controller_knu_1_0_right");
    for (const char* n : {"/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch", "/input/trigger/click", "/input/trigger/touch", "/input/system/click", "/input/system/touch"}) {
      vr::VRInputComponentHandle_t h = 0;
      const auto e = g_input->CreateBooleanComponent(c, n, &h);
      if (e == vr::VRInputError_None) bools[n] = h;
    }
    for (const char* n : {"/input/trigger/value"}) {
      vr::VRInputComponentHandle_t h = 0;
      const auto e = g_input->CreateScalarComponent(c, n, &h, vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
      if (e == vr::VRInputError_None) scalars[n] = h;
    }
    return vr::VRInitError_None;
  }
  void Deactivate() override { id = vr::k_unTrackedDeviceIndexInvalid; }
  void EnterStandby() override {}
  void* GetComponent(const char*) override { return nullptr; }
  void DebugRequest(const char* req, char* out, uint32_t size) override {
    char name[64] = {0};
    float v = 0;
    if (sscanf(req, "%63s %f", name, &v) != 2) { snprintf(out, size, "bad request"); return; }
    vr::EVRInputError e = vr::VRInputError_NameNotFound;
    if (bools.count(name)) e = g_input->UpdateBooleanComponent(bools[name], v > 0.5f, 0);
    else if (scalars.count(name)) e = g_input->UpdateScalarComponent(scalars[name], v, 0);
    snprintf(out, size, "%d", int(e));
  }
  vr::DriverPose_t GetPose() override {
    vr::DriverPose_t p{};
    p.poseIsValid = true;
    p.deviceIsConnected = true;
    p.result = vr::TrackingResult_Running_OK;
    p.qWorldFromDriverRotation.w = 1;
    p.qDriverFromHeadRotation.w = 1;
    p.qRotation.w = 1;
    p.vecPosition[0] = 0.2;
    p.vecPosition[1] = 1.1;
    p.vecPosition[2] = -0.3;
    return p;
  }
};

Ctl g_ctl;

struct Provider : vr::IServerTrackedDeviceProvider {
  vr::EVRInitError Init(vr::IVRDriverContext* ctx) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
    // The version Valve's own drivers ask for. The SDK's header stops at _003.
    vr::EVRInitError e = vr::VRInitError_None;
    g_input = static_cast<vr::IVRDriverInput*>(ctx->GetGenericInterface("IVRDriverInput_005", &e));
    if (!g_input || e != vr::VRInitError_None) g_input = vr::VRDriverInput();
    vr::VRServerDriverHost()->TrackedDeviceAdded("utmtftm_test_right", vr::TrackedDeviceClass_Controller, &g_ctl);
    return vr::VRInitError_None;
  }
  void Cleanup() override { VR_CLEANUP_SERVER_DRIVER_CONTEXT(); }
  const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
  void RunFrame() override {
    if (g_ctl.id != vr::k_unTrackedDeviceIndexInvalid) {
      const vr::DriverPose_t p = g_ctl.GetPose();
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(g_ctl.id, p, sizeof p);
    }
    vr::VREvent_t ev;
    while (vr::VRServerDriverHost()->PollNextEvent(&ev, sizeof ev)) {}
  }
  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override {}
  void LeaveStandby() override {}
};

Provider g_provider;

}  // namespace

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* name, int* code) {
  if (!strcmp(name, vr::IServerTrackedDeviceProvider_Version)) return &g_provider;
  if (code) *code = vr::VRInitError_Init_InterfaceNotFound;
  return nullptr;
}

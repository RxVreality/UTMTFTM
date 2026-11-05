// What SteamVR knows about a controller, whoever made it. Everything goes by the model name SteamVR reports,
// so any controller it can draw works, including ones from add-on drivers.
#pragma once
#include <openvr.h>

#include <string>

#include "ghost3d.h"
#include "xform.h"

namespace fh {

// An add-on driver can report a name it can't load itself: Virtual Desktop's Quest 2 folder has no whole model in it.
// The same name without its "{driver}" prefix is SteamVR's own copy of that model.
inline std::string withoutDriver(const std::string& model) {
  size_t end = model.find('}');
  return !model.empty() && model[0] == '{' && end != std::string::npos ? model.substr(end + 1) : model;
}

// Where the game's grip sits on this model. Not every model has openxr_grip: Index and Vive wands have grip, and
// some only have handgrip. This is the order SteamVR itself looks for them in. Returns the one it found, or null.
// GetComponentStateForDevicePath would say no unless that controller is on right now. This one just reads the table.
inline const char* gripOnModel(const std::string& model, M& gripFromRaw) {
  for (const std::string& m : {model, withoutDriver(model)})
    for (const char* name : {"openxr_grip", "grip", "handgrip"}) {
      vr::RenderModel_ComponentState_t st{};
      vr::RenderModel_ControllerMode_State_t mode{};
      vr::VRControllerState_t state{};
      if (vr::VRRenderModels()->GetComponentState(m.c_str(), name, &state, &mode, &st) && finite(st.mTrackingToComponentLocal)) {
        gripFromRaw = st.mTrackingToComponentLocal;
        return name;
      }
    }
  return nullptr;
}

enum class ModelLoad { Loading, Done, Failed };

// One model's mesh, in the device's own frame. SteamVR loads in the background, so keep calling step() until it
// stops saying Loading. It's built from the model's parts, as SteamVR draws it. The whole-model files don't always
// match: Pico and Focus 3 won't load whole at all, and the right Quest 2's has a solid disc across the tracking ring.
class ModelLoader {
 public:
  Mesh mesh;
  std::vector<ModelPart> parts;

  explicit ModelLoader(const std::string& model = "") : model_(model) {}

  ModelLoad step() {
    auto R = vr::VRRenderModels();
    const uint32_t count = R->GetComponentCount(model_.c_str());
    while (next_ < count) {
      char part[128] = {0}, file[256] = {0};
      R->GetComponentName(model_.c_str(), next_, part, sizeof part);
      vr::RenderModel_ComponentState_t st{};
      vr::RenderModel_ControllerMode_State_t mode{};
      vr::VRControllerState_t state{};
      // Parts with no mesh are just named points on the controller, and hidden ones only show while they're touched.
      const bool drawn = R->GetComponentRenderModelName(model_.c_str(), part, file, sizeof file) && file[0] &&
                         R->GetComponentState(model_.c_str(), part, &state, &mode, &st) && (st.uProperties & vr::VRComponentProperty_IsVisible);
      if (drawn) {
        const uint32_t firstVertex = (uint32_t)mesh.p.size(), firstIndex = (uint32_t)mesh.idx.size();
        if (add(file, st.mTrackingToComponentRenderModel) == vr::VRRenderModelError_Loading) return ModelLoad::Loading;
        if (mesh.p.size() > firstVertex) {
          V3 sum{0, 0, 0};
          for (size_t i = firstVertex; i < mesh.p.size(); ++i) sum = sum + mesh.p[i];
          parts.push_back({part, firstIndex, (uint32_t)mesh.idx.size() - firstIndex, sum * (1.f / float(mesh.p.size() - firstVertex))});
        }
      }
      ++next_;
    }
    if (!mesh.p.empty()) return ModelLoad::Done;
    // No parts with anything to draw, so it has to be one piece.
    vr::EVRRenderModelError e = add(model_.c_str(), ident());
    if (e == vr::VRRenderModelError_Loading) return ModelLoad::Loading;
    if (e == vr::VRRenderModelError_None && !mesh.p.empty()) return ModelLoad::Done;
    const std::string plain = withoutDriver(model_);
    if (plain == model_) return ModelLoad::Failed;
    model_ = plain;
    next_ = 0;
    return ModelLoad::Loading;
  }

 private:
  vr::EVRRenderModelError add(const char* file, const M& place) {
    vr::RenderModel_t* rm = nullptr;
    vr::EVRRenderModelError e = vr::VRRenderModels()->LoadRenderModel_Async(file, &rm);
    if (e != vr::VRRenderModelError_None || !rm) return e;
    const uint32_t base = (uint32_t)mesh.p.size();
    for (uint32_t i = 0; i < rm->unVertexCount; ++i) {
      const vr::RenderModel_Vertex_t& v = rm->rVertexData[i];
      mesh.p.push_back(xfP(place, {v.vPosition.v[0], v.vPosition.v[1], v.vPosition.v[2]}));
      mesh.n.push_back(norm(xfN(place, {v.vNormal.v[0], v.vNormal.v[1], v.vNormal.v[2]})));
    }
    for (uint32_t i = 0; i < rm->unTriangleCount * 3; ++i) mesh.idx.push_back(base + rm->rIndexData[i]);
    vr::VRRenderModels()->FreeRenderModel(rm);
    return e;
  }

  std::string model_;
  uint32_t next_ = 0;
};

}  // namespace fh

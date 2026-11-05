// Review tool, needs SteamVR running: the bindings page drawn from what the real SteamVR says, through the same calls the
// overlay makes, for every controller it can draw and any games you name. It only reads: asked to change a setting, it
// prints what it would have done and doesn't.
// Usage: bind_sheet <out-dir> [app key ...]
#include <openvr.h>

#include <cstdio>
#include <string>
#include <vector>

#include "bindings.h"
#include "controller_vr.h"
#include "platform.h"
#include "ui.h"

using namespace fh;

static void loadModels(bind::Page& page) {
  for (const std::string& name : page.modelsWanted()) {
    ModelLoader loader(name);
    ModelLoad r = ModelLoad::Loading;
    for (int tries = 0; tries < 1500 && r == ModelLoad::Loading; ++tries) {
      r = loader.step();
      if (r == ModelLoad::Loading) platform::sleepMs(20);
    }
    printf("  model %-44s %s, %zu parts\n", name.c_str(), r == ModelLoad::Done ? "loaded" : "DID NOT LOAD", loader.parts.size());
    page.give(name, loader.mesh, loader.parts);
  }
}

static void save(const BindModel& m, const std::string& path) {
  Canvas cv(1200, 760);
  std::vector<Button> b;
  drawBindings(cv, m, b);
  cv.savePNG(path);
}

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : ".";
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Utility);
  if (e != vr::VRInitError_None) { printf("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e)); return 1; }

  bind::Dirs d;
  d.steamvr = platform::steamvrDir();
  d.steam = platform::steamDir();
  d.data = out + "/data";
  d.config = platform::configDirs();
  bind::Settings s;
  s.get = [](const std::string& section, const std::string& key) {
    char buf[4096] = {0};
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(section.c_str(), key.c_str(), buf, sizeof buf, &err);
    return err == vr::VRSettingsError_None ? std::string(buf) : std::string();
  };
  s.set = [](const std::string& section, const std::string& key, const std::string& value) {
    printf("  would set %s %s = %s\n", section.c_str(), key.c_str(), value.c_str());
    return true;
  };
  s.remove = [](const std::string& section, const std::string& key) { printf("  would remove %s %s\n", section.c_str(), key.c_str()); };

  bind::Page page(d, s);
  page.load();

  std::string type, model[2];
  auto S = vr::VRSystem();
  for (int h = 0; h < 2; ++h) {
    const auto idx = S->GetTrackedDeviceIndexForControllerRole(h ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
    if (idx == vr::k_unTrackedDeviceIndexInvalid) continue;
    char t[128] = {0}, m[256] = {0};
    S->GetStringTrackedDeviceProperty(idx, vr::Prop_ControllerType_String, t, sizeof t);
    S->GetStringTrackedDeviceProperty(idx, vr::Prop_RenderModelName_String, m, sizeof m);
    if (t[0]) type = t;
    model[h] = m;
  }
  if (type.empty()) {
    printf("no controllers on, so drawing it as if it were a Frame\n");
    type = "frame_controller";
    model[0] = "{frame_controller}frame_controller_left";
    model[1] = "{frame_controller}frame_controller_right";
  }
  printf("your controllers: %s, %s, %s\n", type.c_str(), model[0].c_str(), model[1].c_str());
  page.setYours(type, model[0], model[1]);

  int fails = 0;
  const BindModel first = page.model("", "");
  for (const std::string& ref : first.refs) {
    printf("%s\n", ref.c_str());
    page.setReference(ref);
    loadModels(page);
    const BindModel m = page.model("", "");
    for (const BindSide* side : {&m.theirs, &m.yours})
      for (int h = 0; h < 2; ++h)
        for (const BindRow& r : side->rows[h])
          if (r.x < 0) { printf("  FAIL %s has no spot on the picture\n", r.id.c_str()); ++fails; }
    std::string name = ref;
    for (char& ch : name) if (ch == ' ') ch = '_';
    save(m, out + "/ref_" + name + ".png");
  }

  page.setReference(first.refs.empty() ? "" : first.ref);
  std::vector<std::string> games;
  for (int i = 2; i < argc; ++i) games.push_back(argv[i]);
  if (const uint32_t pid = vr::VRApplications()->GetCurrentSceneProcessId()) {
    char key[256] = {0};
    if (vr::VRApplications()->GetApplicationKeyByProcessId(pid, key, sizeof key) == vr::VRApplicationError_None && key[0]) games.push_back(key);
  }
  for (const std::string& g : games) {
    char name[256] = {0};
    vr::EVRApplicationError err;
    vr::VRApplications()->GetApplicationPropertyString(g.c_str(), vr::VRApplicationProperty_Name_String, name, sizeof name, &err);
    printf("game %s (%s)\n", g.c_str(), name);
    page.setGame(g, g, name[0] ? name : g);
    loadModels(page);
    const BindModel m = page.model("", "");
    printf("  %s: %s\n  %s: %s\n", m.theirs.title.c_str(), m.theirs.note.c_str(), m.yours.title.c_str(), m.yours.note.c_str());
    std::string file = g;
    for (char& ch : file) if (ch == '.') ch = '_';
    save(m, out + "/game_" + file + ".png");
  }
  vr::VR_Shutdown();
  printf("%d controllers, %d rows without a spot\n", int(first.refs.size()), fails);
  return fails ? 1 : 0;
}

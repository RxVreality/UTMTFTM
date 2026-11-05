// A small made-up SteamVR on disk: two controllers, how one translates to the other, simple box models and two games'
// bindings. It's written for the tests to read, so they run the same on a machine with no SteamVR, and none of Valve's
// files end up in this repository. tests/test_bindings.cpp also runs against the real thing when it's installed.
#pragma once
#include <json/json.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "ghost3d.h"

namespace fh {
namespace fixture {

inline void writeFile(const std::filesystem::path& p, const std::string& text) {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream o(p, std::ios::binary);
  o << text;
}

// Where each part of a model sits, so the same numbers can be served as files or through the fake IVRRenderModels.
struct Part { const char* name; float x, y, z, half; };
inline std::vector<Part> partsOf(const std::string& model) {
  const bool left = model.find("left") != std::string::npos;
  const float s = left ? 1.f : -1.f;
  if (model.find("frame") != std::string::npos) {
    if (left)
      return {{"body", 0, -0.01f, 0.08f, 0.035f}, {"thumbstick", 0.024f, 0.012f, 0.048f, 0.010f}, {"dpad", -0.004f, 0.006f, 0.040f, 0.012f},
              {"button_view", 0.013f, 0.0f, 0.030f, 0.004f}, {"trigger", 0.006f, -0.040f, 0.052f, 0.012f}, {"bumper", 0.008f, -0.030f, 0.036f, 0.008f}, {"button_grip", 0.017f, -0.007f, 0.086f, 0.008f}};
    return {{"body", 0, -0.01f, 0.08f, 0.035f}, {"thumbstick", -0.024f, 0.012f, 0.048f, 0.010f}, {"button_a", 0.004f, 0.012f, 0.047f, 0.004f}, {"button_b", 0.012f, 0.007f, 0.039f, 0.004f},
            {"button_x", -0.005f, 0.006f, 0.042f, 0.004f}, {"button_y", 0.003f, 0.001f, 0.033f, 0.004f}, {"button_menu", -0.013f, 0.0f, 0.030f, 0.004f},
            {"trigger", -0.006f, -0.040f, 0.052f, 0.012f}, {"bumper", -0.008f, -0.030f, 0.036f, 0.008f}, {"button_grip", -0.017f, -0.007f, 0.086f, 0.008f}};
  }
  if (left)
    return {{"body", 0, -0.01f, 0.08f, 0.032f}, {"thumbstick", 0.005f * s, 0.014f, 0.04f, 0.009f}, {"button_x", -0.006f, 0.01f, 0.055f, 0.005f}, {"button_y", 0.016f, 0.006f, 0.044f, 0.005f},
            {"button_enter", -0.010f, 0.01f, 0.057f, 0.004f}, {"trigger", 0.013f, -0.03f, 0.047f, 0.012f}, {"button_grip", 0.012f, -0.003f, 0.089f, 0.010f}};
  return {{"body", 0, -0.01f, 0.08f, 0.032f}, {"thumbstick", 0.005f * s, 0.014f, 0.04f, 0.009f}, {"button_a", -0.006f, 0.01f, 0.055f, 0.005f}, {"button_b", -0.016f, 0.006f, 0.044f, 0.005f},
          {"button_enter", 0.010f, 0.01f, 0.057f, 0.004f}, {"trigger", -0.013f, -0.03f, 0.047f, 0.012f}, {"button_grip", -0.012f, -0.003f, 0.089f, 0.010f}};
}

inline Mesh boxMesh(const Part& p) {
  Mesh m;
  addEllipsoid(m, {p.x, p.y, p.z}, {p.half, p.half, p.name == std::string("body") ? p.half * 2.2f : p.half}, {0, 1, 0}, 6, 10);
  return m;
}

inline void writeModel(const std::filesystem::path& root, const std::string& model) {
  Json::Value doc;
  for (const Part& part : partsOf(model)) {
    const Mesh m = boxMesh(part);
    std::string obj;
    for (const V3& v : m.p) obj += "v " + std::to_string(v.x) + " " + std::to_string(v.y) + " " + std::to_string(v.z) + "\n";
    for (const V3& n : m.n) obj += "vn " + std::to_string(n.x) + " " + std::to_string(n.y) + " " + std::to_string(n.z) + "\n";
    for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
      const std::string a = std::to_string(m.idx[t] + 1), b = std::to_string(m.idx[t + 1] + 1), c = std::to_string(m.idx[t + 2] + 1);
      obj += "f " + a + "//" + a + " " + b + "//" + b + " " + c + "//" + c + "\n";
    }
    const std::string file = model + "_" + part.name + ".obj";
    writeFile(root / model / file, obj);
    doc["components"][part.name]["filename"] = file;
  }
  Json::StreamWriterBuilder w;
  writeFile(root / model / (model + ".json"), Json::writeString(w, doc));
}

inline const char* kFrameProfile = R"({
  "jsonid": "input_profile", "controller_type": "frame_controller",
  "mirror_mappings": [["/user/hand/left/input/view", "/user/hand/right/input/menu"], ["/user/hand/left/input/dpad_up", "/user/hand/right/input/y"],
                      ["/user/hand/left/input/dpad_right", "/user/hand/right/input/b"], ["/user/hand/left/input/dpad_down", "/user/hand/right/input/a"],
                      ["/user/hand/left/input/dpad_left", "/user/hand/right/input/x"]],
  "input_source": {
    "/input/system": {"type": "button", "click": true, "touch": true, "order": 1},
    "/input/menu": {"type": "button", "side": "right", "click": true, "touch": true, "order": 2},
    "/input/view": {"type": "button", "side": "left", "click": true, "touch": true, "order": 2},
    "/input/a": {"type": "button", "side": "right", "click": true, "touch": true, "order": 6},
    "/input/b": {"type": "button", "side": "right", "click": true, "touch": true, "order": 7},
    "/input/x": {"type": "button", "side": "right", "click": true, "touch": true, "order": 8},
    "/input/y": {"type": "button", "side": "right", "click": true, "touch": true, "order": 9},
    "/input/dpad_up": {"type": "button", "side": "left", "click": true, "touch": true, "order": 13},
    "/input/dpad_right": {"type": "button", "side": "left", "click": true, "touch": true, "order": 12},
    "/input/dpad_down": {"type": "button", "side": "left", "click": true, "touch": true, "order": 10},
    "/input/dpad_left": {"type": "button", "side": "left", "click": true, "touch": true, "order": 11},
    "/input/bumper": {"type": "button", "click": true, "touch": true, "order": 4},
    "/input/trigger": {"type": "trigger", "value": true, "click": true, "touch": true, "order": 3},
    "/input/grip": {"type": "trigger", "value": true, "click": true, "touch": true, "order": 14},
    "/input/thumbstick": {"type": "joystick", "click": true, "touch": true, "order": 5},
    "/input/thumbrest": {"type": "button", "click": false, "touch": true, "visibility": "InputValueVisibility_SteamVRInternal"},
    "/pose/raw": {"type": "pose"}, "/pose/openxr_grip": {"type": "pose"}, "/pose/openxr_aim": {"type": "pose"},
    "/input/skeleton/left": {"type": "skeleton", "side": "left"}, "/input/skeleton/right": {"type": "skeleton", "side": "right"},
    "/output/haptic": {"type": "vibration"}
  }
})";

inline const char* kTouchProfile = R"({
  "jsonid": "input_profile", "controller_type": "oculus_touch",
  "input_source": {
    "/input/joystick": {"type": "joystick", "click": true, "touch": true, "order": 2},
    "/input/trigger": {"type": "trigger", "value": true, "touch": true, "order": 1},
    "/input/grip": {"type": "trigger", "value": true, "order": 3},
    "/input/a": {"type": "button", "side": "right", "click": true, "touch": true, "order": 4},
    "/input/b": {"type": "button", "side": "right", "click": true, "touch": true, "order": 5},
    "/input/x": {"type": "button", "side": "left", "click": true, "touch": true, "order": 4},
    "/input/y": {"type": "button", "side": "left", "click": true, "touch": true, "order": 5},
    "/input/system": {"type": "button", "side": "left", "click": true, "order": 0},
    "/pose/raw": {"type": "pose"}, "/pose/openxr_grip": {"type": "pose"}, "/pose/openxr_aim": {"type": "pose"}
  }
})";

inline const char* kFrameRemap = R"({
  "to_controller_type": "frame_controller",
  "layouts": [{
    "priority": 95, "from_controller_type": "oculus_touch",
    "autoremappings": [
      {"from": "/user/hand/right/input/grip", "to": "/user/hand/right/input/grip"},
      {"from": "/user/hand/right/input/trigger", "to": ["/user/hand/right/input/trigger", "/user/hand/right/input/bumper"]},
      {"from": "/user/hand/right/input/joystick", "to": "/user/hand/right/input/thumbstick"},
      {"from": "/user/hand/left/input/x", "to": "/user/hand/left/input/dpad_down", "mirror": false},
      {"from": "/user/hand/left/input/y", "to": ["/user/hand/left/input/dpad_left", "/user/hand/left/input/dpad_right", "/user/hand/left/input/dpad_up"], "mirror": false},
      {"from": "/user/hand/right/input/a", "to": "/user/hand/right/input/a", "mirror": false},
      {"from": "/user/hand/right/input/b", "to": ["/user/hand/right/input/b", "/user/hand/right/input/x", "/user/hand/right/input/y"], "mirror": false}
    ],
    "remappings": [{"remapping_mode": "replace", "from": {"path": "/user/hand/left/input/menu"}, "to": {"path": "/user/hand/left/input/view"}}]
  }]
})";

// A Unity-style game: role names, one Quest binding, nothing for the Frame.
inline const char* kUnityBinding = R"({
  "category": "openxr", "controller_type": "oculus_touch", "interaction_profile": "/interaction_profiles/oculus/touch_controller", "name": "Bindings from OpenXR for oculus_touch",
  "bindings": {"/actions/oculustouchcontroller": {
    "chords": [], "eyetracking": [],
    "haptics": [{"output": "/actions/oculustouchcontroller/out/haptic", "path": "/user/hand/left/output/haptic"}, {"output": "/actions/oculustouchcontroller/out/haptic", "path": "/user/hand/right/output/haptic"}],
    "poses": [{"output": "/actions/oculustouchcontroller/in/devicepose", "path": "/user/hand/left/pose/openxr_grip"}, {"output": "/actions/oculustouchcontroller/in/devicepose", "path": "/user/hand/right/pose/openxr_grip"}],
    "sources": [
      {"inputs": {"position": {"output": "/actions/oculustouchcontroller/in/thumbstick"}}, "mode": "joystick", "path": "/user/hand/left/input/joystick"},
      {"inputs": {"position": {"output": "/actions/oculustouchcontroller/in/thumbstick"}}, "mode": "joystick", "path": "/user/hand/right/input/joystick"},
      {"inputs": {"pull": {"output": "/actions/oculustouchcontroller/in/grip"}}, "mode": "trigger", "path": "/user/hand/left/input/grip"},
      {"inputs": {"pull": {"output": "/actions/oculustouchcontroller/in/grip"}}, "mode": "trigger", "path": "/user/hand/right/input/grip"},
      {"inputs": {"click": {"output": "/actions/oculustouchcontroller/in/menu"}}, "mode": "button", "parameters": {"force_input": "click"}, "path": "/user/hand/left/input/menu"},
      {"inputs": {"click": {"output": "/actions/oculustouchcontroller/in/menu"}}, "mode": "button", "parameters": {"force_input": "click"}, "path": "/user/hand/right/input/system"},
      {"inputs": {"click": {"output": "/actions/oculustouchcontroller/in/primarybutton"}}, "mode": "button", "path": "/user/hand/left/input/x"},
      {"inputs": {"click": {"output": "/actions/oculustouchcontroller/in/primarybutton"}}, "mode": "button", "path": "/user/hand/right/input/a"},
      {"inputs": {"touch": {"output": "/actions/oculustouchcontroller/in/primarytouched"}}, "mode": "button", "path": "/user/hand/right/input/a"},
      {"inputs": {"click": {"output": "/actions/oculustouchcontroller/in/secondarybutton"}}, "mode": "button", "path": "/user/hand/left/input/y"},
      {"inputs": {"click": {"output": "/actions/oculustouchcontroller/in/secondarybutton"}}, "mode": "button", "path": "/user/hand/right/input/b"},
      {"inputs": {"pull": {"output": "/actions/oculustouchcontroller/in/trigger"}}, "mode": "trigger", "path": "/user/hand/left/input/trigger"},
      {"inputs": {"pull": {"output": "/actions/oculustouchcontroller/in/trigger"}}, "mode": "trigger", "path": "/user/hand/right/input/trigger"},
      {"inputs": {"click": {"output": "/actions/oculustouchcontroller/in/triggerpressed"}}, "mode": "button", "path": "/user/hand/left/input/trigger"},
      {"inputs": {"click": {"output": "/actions/oculustouchcontroller/in/triggerpressed"}}, "mode": "button", "path": "/user/hand/right/input/trigger"},
      {"inputs": {"click": {"output": "/actions/oculustouchcontroller/in/thumbstickclicked"}}, "mode": "button", "path": "/user/hand/left/input/joystick"},
      {"inputs": {"click": {"output": "/actions/oculustouchcontroller/in/thumbstickclicked"}}, "mode": "button", "path": "/user/hand/right/input/joystick"}
    ]
  }}
})";

// A SteamVR Input game with real action names, in its own folder.
inline const char* kNamedManifest = R"({
  "default_bindings": [{"controller_type": "oculus_touch", "binding_url": "bindings_touch.json"}, {"controller_type": "frame_controller", "binding_url": "bindings_frame.json"}],
  "actions": [{"name": "/actions/main/in/Fire", "type": "boolean"}, {"name": "/actions/main/in/Move", "type": "vector2"}, {"name": "/actions/main/in/Reload", "type": "boolean"}, {"name": "/actions/vehicle/in/Horn", "type": "boolean"}],
  "action_sets": [{"name": "/actions/main", "usage": "leftright"}, {"name": "/actions/vehicle", "usage": "leftright"}, {"name": "/actions/vehicle_right", "usage": "leftright"}],
  "localization": [{"language_tag": "en_US", "/actions/main": "On Foot", "/actions/vehicle": "Driving", "/actions/main/in/fire": "Fire the gun", "/actions/main/in/reload": "Reload"}]
})";
inline const char* kNamedTouch = R"({
  "category": "steamvr_input", "controller_type": "oculus_touch", "name": "Default",
  "bindings": {"/actions/main": {"sources": [
    {"mode": "button", "path": "/user/hand/right/input/trigger", "inputs": {"click": {"output": "/actions/main/in/Fire"}}},
    {"mode": "joystick", "path": "/user/hand/left/input/joystick", "inputs": {"position": {"output": "/actions/main/in/Move"}}},
    {"mode": "button", "path": "/user/hand/right/input/a", "inputs": {"click": {"output": "/actions/main/in/Reload"}}}
  ]}, "/actions/vehicle": {"sources": [
    {"mode": "button", "path": "/user/hand/right/input/a", "inputs": {"click": {"output": "/actions/vehicle/in/Horn"}}}
  ]}, "/actions/vehicle_right": {"sources": [
    {"mode": "button", "path": "/user/hand/right/input/b", "inputs": {"click": {"output": "/actions/vehicle/in/Horn"}}}
  ]}}
})";
inline const char* kNamedFrame = R"({
  "category": "steamvr_input", "controller_type": "frame_controller", "name": "Default",
  "bindings": {"/actions/main": {"sources": [
    {"mode": "button", "path": "/user/hand/right/input/bumper", "inputs": {"click": {"output": "/actions/main/in/Fire"}}},
    {"mode": "joystick", "path": "/user/hand/left/input/thumbstick", "inputs": {"position": {"output": "/actions/main/in/Move"}}},
    {"mode": "button", "path": "/user/hand/right/input/y", "inputs": {"click": {"output": "/actions/main/in/Reload"}}}
  ]}, "/actions/vehicle": {"sources": [
    {"mode": "button", "path": "/user/hand/right/input/a", "inputs": {"click": {"output": "/actions/vehicle/in/Horn"}}}
  ]}}
})";

// Lays out the made-up SteamVR. steamvr is its install folder, config its config folder, gamesDir somewhere for a game.
inline void writeSteamVR(const std::string& steamvr, const std::string& config, const std::string& gamesDir) {
  namespace fs = std::filesystem;
  const fs::path root = fs::u8path(steamvr), cfg = fs::u8path(config);
  writeFile(root / "drivers/frame_controller/resources/input/frame_controller_profile.json", kFrameProfile);
  writeFile(root / "drivers/frame_controller/resources/input/frame_controller_remapping.json", kFrameRemap);
  writeFile(root / "drivers/oculus/resources/input/touch_profile.json", kTouchProfile);
  for (const char* m : {"frame_controller_left", "frame_controller_right"}) writeModel(root / "drivers/frame_controller/resources/rendermodels", m);
  for (const char* m : {"oculus_quest_plus_controller_left", "oculus_quest_plus_controller_right", "oculus_quest2_controller_left", "oculus_quest2_controller_right"})
    writeModel(root / "resources/rendermodels", m);
  writeFile(cfg / "openxr/steam.app.12345_oculus_touch_0_binding.json", kUnityBinding);
  const fs::path game = fs::u8path(gamesDir) / "NamedGame";
  writeFile(game / "actions.json", kNamedManifest);
  writeFile(game / "bindings_touch.json", kNamedTouch);
  writeFile(game / "bindings_frame.json", kNamedFrame);
  Json::Value apps;
  Json::Value a;
  a["app_key"] = "steam.app.777";
  a["action_manifest_path"] = (game / "actions.json").generic_u8string();
  apps["applications"].append(a);
  Json::StreamWriterBuilder w;
  writeFile(cfg / "steamapps.vrmanifest", Json::writeString(w, apps));
}

}  // namespace fixture
}  // namespace fh

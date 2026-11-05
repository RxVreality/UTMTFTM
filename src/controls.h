// What each kind of controller calls the buttons the placing panel asks for. This has to agree with
// input/bindings_<type>.json, which test_controllers checks.
#pragma once
#include <string>

namespace fh {

struct ButtonNames { const char* reset; const char* cancel; };

inline ButtonNames buttonNames(const std::string& controllerType) {
  // A and B on the right controller, X and Y on the left.
  for (const char* t : {"oculus_touch", "pico_controller", "vive_focus3_controller", "hpmotioncontroller", "vive_cosmos_controller"})
    if (controllerType == t) return {"A / X", "B / Y"};
  // No face buttons.
  if (controllerType == "vive_controller" || controllerType == "holographic_controller") return {"Trackpad", "Menu"};
  // The Frame, Index, and anything SteamVR maps onto one of our bindings by itself.
  return {"A", "B"};
}

}  // namespace fh

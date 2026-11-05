// The hand UTMTFTM draws: SteamVR's own skinned glove, curled by SteamVR's own open-to-closed animation
// and seated in the OpenXR grip frame, which is defined the same way on every controller. Everything is read
// at run time from the user's SteamVR install, so none of Valve's data ships with this project.
// The Steam Frame's SteamVR has the skeleton and animation but no glove, so there the same pose gets the skin in handskin.h.
// If none of those files can be read, the hand falls back to the procedural one in ghost3d.h.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ghost3d.h"

namespace fh {

enum Finger { kThumb = 0, kIndex, kMiddle, kRing, kPinky, kFingerCount };
struct Curl { float f[kFingerCount]; };  // 0 open .. 1 closed; negative: the glove's bind pose

// How far each finger closes around the Steam Frame controller before it would push into it.
extern const Curl kGripCurl;

// The right glove posed with `c`, in skeleton space. vertexFinger (optional) gets, per vertex, the finger
// it mostly belongs to, or kFingerCount for the palm. false if SteamVR's files aren't readable.
// With a skeleton but no glove in this SteamVR, it fills in the joints and leaves the mesh empty.
bool posedRightGlove(const std::string& steamvrDir, const Curl& c, Mesh& out, V3 joints[31],
                     std::vector<uint8_t>* vertexFinger = nullptr);

// Right hand -> left hand, across the skeleton's X axis.
void mirrorHand(Mesh& m, V3 joints[31]);

// Where the hand skeleton sits in the OpenXR grip frame. For the left hand this applies after mirrorHand.
bool handInGrip(const std::string& steamvrDir, bool left, M& handInGrip);

// The hand to draw, in the OpenXR grip frame, built once per side.
const Mesh& gripHandMesh(bool left);
const V3* gripHandSkeleton(bool left);  // 31 joints

}  // namespace fh

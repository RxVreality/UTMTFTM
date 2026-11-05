#include "glove.h"

#include <json/json.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

#include "platform.h"

namespace fs = std::filesystem;

namespace fh {

// Measured by tests/glove_sheet.cpp on the Frame controller: each finger closes until it would sink into
// the handle, and the palm rests against it. The thumb stays a little open so it clears the tracking ring.
const Curl kGripCurl = {{0.25f, 0.40f, 0.40f, 0.35f, 0.35f}};
static constexpr float kPalmFromHandle = 0.046f;  // middle of the palm to the handle's centre line, metres
static constexpr float kPalmTowardWrist = 0.020f;  // moves the handle from mid-palm toward the fingers

namespace {

// ---------------------------------------------------------------- glTF binary
struct Glb {
  Json::Value j;
  std::vector<uint8_t> bin;
};

bool readGlb(const std::string& path, Glb& g) {
  std::ifstream in(fs::u8path(path), std::ios::binary);
  if (!in) return false;
  uint32_t hdr[3];
  in.read(reinterpret_cast<char*>(hdr), sizeof hdr);
  if (!in || hdr[0] != 0x46546C67) return false;  // "glTF"
  for (;;) {
    uint32_t chunk[2];
    in.read(reinterpret_cast<char*>(chunk), sizeof chunk);
    if (!in) break;
    std::vector<uint8_t> data(chunk[0]);
    in.read(reinterpret_cast<char*>(data.data()), chunk[0]);
    if (!in) return false;
    if (chunk[1] == 0x4E4F534A) {  // "JSON"
      Json::CharReaderBuilder b;
      std::string errs;
      std::unique_ptr<Json::CharReader> r(b.newCharReader());
      const char* s = reinterpret_cast<const char*>(data.data());
      if (!r->parse(s, s + data.size(), &g.j, &errs)) return false;
    } else if (chunk[1] == 0x004E4942) {  // "BIN"
      g.bin = std::move(data);
    }
  }
  return !g.j.isNull() && !g.bin.empty();
}

struct View {
  const uint8_t* p = nullptr;
  size_t stride = 0, count = 0;
  int comp = 0;
};

bool accessor(const Glb& g, int i, int comps, View& v) {
  if (i < 0 || (unsigned)i >= g.j["accessors"].size()) return false;
  const Json::Value& a = g.j["accessors"][i];
  const Json::Value& bv = g.j["bufferViews"][a["bufferView"].asInt()];
  size_t off = a.get("byteOffset", 0).asUInt64() + bv.get("byteOffset", 0).asUInt64();
  v.comp = a["componentType"].asInt();
  v.count = a["count"].asUInt64();
  size_t size = v.comp == 5126 || v.comp == 5125 ? 4 : v.comp == 5123 ? 2 : 1;
  v.stride = bv.get("byteStride", 0).asUInt64();
  if (!v.stride) v.stride = size * comps;
  if (!v.count || off + v.stride * (v.count - 1) + size * comps > g.bin.size()) return false;
  v.p = g.bin.data() + off;
  return true;
}
float readF(const View& v, size_t i, int c) {
  const uint8_t* e = v.p + v.stride * i;
  if (v.comp == 5126) { float f; memcpy(&f, e + 4 * c, 4); return f; }
  if (v.comp == 5123) { uint16_t u; memcpy(&u, e + 2 * c, 2); return u / 65535.f; }
  return e[c] / 255.f;
}
uint32_t readU(const View& v, size_t i, int c) {
  const uint8_t* e = v.p + v.stride * i;
  if (v.comp == 5125) { uint32_t u; memcpy(&u, e + 4 * c, 4); return u; }
  if (v.comp == 5123) { uint16_t u; memcpy(&u, e + 2 * c, 2); return u; }
  return e[c];
}

M trs(const float t[3], const float q[4]) {
  float x = q[0], y = q[1], z = q[2], w = q[3];
  M m{};
  m.m[0][0] = 1 - 2 * (y * y + z * z); m.m[0][1] = 2 * (x * y - z * w); m.m[0][2] = 2 * (x * z + y * w); m.m[0][3] = t[0];
  m.m[1][0] = 2 * (x * y + z * w); m.m[1][1] = 1 - 2 * (x * x + z * z); m.m[1][2] = 2 * (y * z - x * w); m.m[1][3] = t[1];
  m.m[2][0] = 2 * (x * z - y * w); m.m[2][1] = 2 * (y * z + x * w); m.m[2][2] = 1 - 2 * (x * x + y * y); m.m[2][3] = t[2];
  return m;
}

// ---------------------------------------------------------------- the right glove and its animation
struct Model {
  struct Node {
    std::string name;
    int parent = -1;
    float t[3] = {0, 0, 0}, r[4] = {0, 0, 0, 1};
    int finger = kFingerCount;  // which finger's curl drives this bone
  };
  struct Track {
    std::vector<std::array<float, 4>> r;
    std::vector<std::array<float, 3>> t;
  };
  std::vector<Node> nodes;
  std::vector<int> skin;   // node index per skin joint
  std::vector<M> ibm;
  std::vector<V3> p, n;
  std::vector<std::array<uint16_t, 4>> joints;
  std::vector<std::array<float, 4>> weights;
  std::vector<uint32_t> idx;
  std::map<int, Track> tracks;  // by node
  int slots[31];                // node per SteamVR skeleton bone
};

int fingerOf(const std::string& name) {
  if (name.find("thumb") != std::string::npos) return kThumb;
  if (name.find("index") != std::string::npos) return kIndex;
  if (name.find("middle") != std::string::npos) return kMiddle;
  if (name.find("ring") != std::string::npos) return kRing;
  if (name.find("pinky") != std::string::npos) return kPinky;
  return kFingerCount;
}

bool loadModel(const std::string& dir, Model& m) {
  Glb g;
  if (!readGlb(dir + "/resources/rendermodels/vr_glove/vr_glove_right_model_slim.glb", g)) return false;
  const Json::Value& nodes = g.j["nodes"];
  m.nodes.resize(nodes.size());
  std::map<std::string, int> byName;
  for (Json::ArrayIndex i = 0; i < nodes.size(); ++i) {
    Model::Node& n = m.nodes[i];
    n.name = nodes[i].get("name", "").asString();
    byName[n.name] = (int)i;
    n.finger = fingerOf(n.name);
    if (nodes[i].isMember("translation")) for (int k = 0; k < 3; ++k) n.t[k] = nodes[i]["translation"][k].asFloat();
    if (nodes[i].isMember("rotation")) for (int k = 0; k < 4; ++k) n.r[k] = nodes[i]["rotation"][k].asFloat();
    for (const auto& c : nodes[i]["children"]) m.nodes[c.asInt()].parent = (int)i;
  }
  const Json::Value& skin = g.j["skins"][0];
  for (const auto& j : skin["joints"]) m.skin.push_back(j.asInt());
  View ib;
  if (m.skin.empty() || !accessor(g, skin["inverseBindMatrices"].asInt(), 16, ib)) return false;
  for (size_t i = 0; i < m.skin.size(); ++i) {
    M a{};
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 3; ++r) a.m[r][c] = readF(ib, i, c * 4 + r);  // glTF is column-major
    m.ibm.push_back(a);
  }
  const Json::Value& prim = g.j["meshes"][0]["primitives"][0];
  const Json::Value& at = prim["attributes"];
  View vp, vn, vj, vw, vi;
  if (!accessor(g, at["POSITION"].asInt(), 3, vp) || !accessor(g, at["NORMAL"].asInt(), 3, vn) ||
      !accessor(g, at["JOINTS_0"].asInt(), 4, vj) || !accessor(g, at["WEIGHTS_0"].asInt(), 4, vw) ||
      !accessor(g, prim["indices"].asInt(), 1, vi))
    return false;
  m.p.resize(vp.count); m.n.resize(vp.count); m.joints.resize(vp.count); m.weights.resize(vp.count);
  for (size_t i = 0; i < vp.count; ++i) {
    m.p[i] = {readF(vp, i, 0), readF(vp, i, 1), readF(vp, i, 2)};
    m.n[i] = {readF(vn, i, 0), readF(vn, i, 1), readF(vn, i, 2)};
    // Valve's weights are stored with their squares summing to 1, so divide by the plain sum to blend them.
    float sum = 0;
    for (int k = 0; k < 4; ++k) { m.joints[i][k] = (uint16_t)readU(vj, i, k); m.weights[i][k] = readF(vw, i, k); sum += m.weights[i][k]; }
    if (sum > 0) for (int k = 0; k < 4; ++k) m.weights[i][k] /= sum;
  }
  m.idx.resize(vi.count);
  for (size_t i = 0; i < vi.count; ++i) m.idx[i] = readU(vi, i, 0);

  // SteamVR's skeleton order, so `slots[i]` lines up with the 31-bone hand everywhere else.
  static const char* const kBones[26] = {
      "Root", "wrist_r", "finger_thumb_0_r", "finger_thumb_1_r", "finger_thumb_2_r", "finger_thumb_r_end",
      "finger_index_meta_r", "finger_index_0_r", "finger_index_1_r", "finger_index_2_r", "finger_index_r_end",
      "finger_middle_meta_r", "finger_middle_0_r", "finger_middle_1_r", "finger_middle_2_r", "finger_middle_r_end",
      "finger_ring_meta_r", "finger_ring_0_r", "finger_ring_1_r", "finger_ring_2_r", "finger_ring_r_end",
      "finger_pinky_meta_r", "finger_pinky_0_r", "finger_pinky_1_r", "finger_pinky_2_r", "finger_pinky_r_end"};
  for (int i = 0; i < 26; ++i) {
    auto it = byName.find(kBones[i]);
    if (it == byName.end()) return false;
    m.slots[i] = it->second;
  }
  const int aux[5] = {4, 9, 14, 19, 24};  // the five aux bones sit on these joints in the rest pose
  for (int i = 0; i < 5; ++i) m.slots[26 + i] = m.slots[aux[i]];

  // The animation's bones are the same names with a "REF:" prefix. Without it the hand just stays open.
  Glb a;
  if (readGlb(dir + "/resources/anims/hand_right_closeanim.glb", a) && a.j["animations"].size()) {
    const Json::Value& anim = a.j["animations"][0];
    for (const auto& ch : anim["channels"]) {
      std::string name = a.j["nodes"][ch["target"]["node"].asInt()].get("name", "").asString();
      if (name.rfind("REF:", 0) == 0) name = name.substr(4);
      auto it = byName.find(name);
      if (it == byName.end()) continue;
      const std::string path = ch["target"]["path"].asString();
      View out;
      int comps = path == "rotation" ? 4 : 3;
      if (path != "rotation" && path != "translation") continue;
      if (!accessor(a, anim["samplers"][ch["sampler"].asInt()]["output"].asInt(), comps, out)) continue;
      Model::Track& tr = m.tracks[it->second];
      for (size_t k = 0; k < out.count; ++k) {
        if (comps == 4) tr.r.push_back({readF(out, k, 0), readF(out, k, 1), readF(out, k, 2), readF(out, k, 3)});
        else tr.t.push_back({readF(out, k, 0), readF(out, k, 1), readF(out, k, 2)});
      }
    }
  }
  return true;
}

const Model* model(const std::string& dir) {
  static std::map<std::string, std::unique_ptr<Model>> cache;
  auto it = cache.find(dir);
  if (it != cache.end()) return it->second.get();
  auto m = std::make_unique<Model>();
  if (!loadModel(dir, *m)) m.reset();
  return (cache[dir] = std::move(m)).get();
}

template <size_t N>
void sampleKeys(const std::vector<std::array<float, N>>& keys, float curl, float out[N]) {
  float f = curl * float(keys.size() - 1);
  size_t a = (size_t)f, b = std::min(a + 1, keys.size() - 1);
  float t = f - (float)a, sign = 1;
  if (N == 4) {  // shortest way round for quaternions
    float d = 0;
    for (size_t i = 0; i < N; ++i) d += keys[a][i] * keys[b][i];
    if (d < 0) sign = -1;
  }
  float len2 = 0;
  for (size_t i = 0; i < N; ++i) { out[i] = keys[a][i] * (1 - t) + sign * keys[b][i] * t; len2 += out[i] * out[i]; }
  if (N == 4 && len2 > 0) { float l = std::sqrt(len2); for (size_t i = 0; i < N; ++i) out[i] /= l; }
}

struct BuiltHand {
  Mesh mesh;
  V3 joints[31];
};

BuiltHand buildGripHand(bool left) {
  BuiltHand h;
  std::string dir = platform::steamvrDir();
  M seat;
  if (posedRightGlove(dir, kGripCurl, h.mesh, h.joints) && handInGrip(dir, left, seat)) {
    if (left) mirrorHand(h.mesh, h.joints);
    for (auto& p : h.mesh.p) p = xfP(seat, p);
    for (auto& n : h.mesh.n) n = norm(xfN(seat, n));
    for (auto& j : h.joints) j = xfP(seat, j);
    return h;
  }
  // No glove to read. The Steam Frame's SteamVR doesn't ship one. So it's our own hand, closed around the handle.
  gripHandJoints(left, h.joints);
  h.mesh = buildHand(left);
  return h;
}

const BuiltHand& gripHand(bool left) {
  static const BuiltHand hands[2] = {buildGripHand(true), buildGripHand(false)};
  return hands[left ? 0 : 1];
}

}  // namespace

bool posedRightGlove(const std::string& dir, const Curl& c, Mesh& out, V3 joints[31], std::vector<uint8_t>* vertexFinger) {
  const Model* mp = model(dir);
  if (!mp) return false;
  const Model& m = *mp;

  std::vector<M> world(m.nodes.size());
  std::vector<uint8_t> done(m.nodes.size(), 0);
  // Parents first, without assuming the file lists them in that order.
  auto eval = [&](auto&& self, int i) -> const M& {
    if (done[i]) return world[i];
    const Model::Node& n = m.nodes[i];
    float t[3] = {n.t[0], n.t[1], n.t[2]}, r[4] = {n.r[0], n.r[1], n.r[2], n.r[3]};
    // Fingers only: the animation's wrist is in another frame (and another scale).
    float curl = n.finger < kFingerCount ? c.f[n.finger] : -1.f;
    auto tr = m.tracks.find(i);
    if (tr != m.tracks.end() && curl >= 0) {
      if (!tr->second.r.empty()) sampleKeys<4>(tr->second.r, curl, r);
      if (!tr->second.t.empty()) sampleKeys<3>(tr->second.t, curl, t);
    }
    M local = trs(t, r);
    world[i] = n.parent >= 0 ? mul(self(self, n.parent), local) : local;
    done[i] = 1;
    return world[i];
  };
  std::vector<M> finals(m.skin.size());
  for (size_t j = 0; j < m.skin.size(); ++j) finals[j] = mul(eval(eval, m.skin[j]), m.ibm[j]);

  out = Mesh{};
  out.p.resize(m.p.size());
  out.n.resize(m.p.size());
  out.idx = m.idx;
  if (vertexFinger) vertexFinger->resize(m.p.size());
  for (size_t i = 0; i < m.p.size(); ++i) {
    V3 p{0, 0, 0}, n{0, 0, 0};
    float best = -1;
    int bestJoint = 0;
    for (int k = 0; k < 4; ++k) {
      float w = m.weights[i][k];
      if (w <= 0) continue;
      const M& f = finals[m.joints[i][k]];
      p = p + xfP(f, m.p[i]) * w;
      n = n + xfN(f, m.n[i]) * w;
      if (w > best) { best = w; bestJoint = m.joints[i][k]; }
    }
    out.p[i] = p;
    out.n[i] = norm(n);
    if (vertexFinger) {
      const Model::Node& b = m.nodes[m.skin[bestJoint]];
      (*vertexFinger)[i] = b.name.find("_meta") != std::string::npos ? (uint8_t)kFingerCount : (uint8_t)b.finger;  // metacarpals are palm
    }
  }
  for (int i = 0; i < 31; ++i) joints[i] = pos(eval(eval, m.slots[i]));
  return true;
}

void mirrorHand(Mesh& m, V3 joints[31]) {
  for (auto& p : m.p) p.x = -p.x;
  for (auto& n : m.n) n.x = -n.x;
  for (size_t t = 0; t + 2 < m.idx.size(); t += 3) std::swap(m.idx[t + 1], m.idx[t + 2]);
  for (int i = 0; i < 31; ++i) joints[i].x = -joints[i].x;
}

// The OpenXR grip definition: -Z runs little finger to thumb along the handle, the palm faces the handle
// (+X for the left hand), and an open hand's fingers point -Y, so they curl under and around it.
bool handInGrip(const std::string& dir, bool left, M& out) {
  V3 j[31];
  Mesh bind;
  if (!posedRightGlove(dir, Curl{{-1, -1, -1, -1, -1}}, bind, j)) return false;
  if (left) mirrorHand(bind, j);
  Frame3 from = handFrame(j);
  V3 a{0, 0, -1}, u{0, -1, 0}, n = cross(a, u);
  M r = ident();
  for (int row = 0; row < 3; ++row) {
    float ta = row == 0 ? a.x : row == 1 ? a.y : a.z, tu = row == 0 ? u.x : row == 1 ? u.y : u.z, tn = row == 0 ? n.x : row == 1 ? n.y : n.z;
    r.m[row][0] = ta * from.a.x + tu * from.u.x + tn * from.n.x;
    r.m[row][1] = ta * from.a.y + tu * from.u.y + tn * from.n.y;
    r.m[row][2] = ta * from.a.z + tu * from.u.z + tn * from.n.z;
    r.m[row][3] = 0;
  }
  V3 mid = xfP(r, (j[1] + j[12]) * 0.5f);
  V3 target{left ? -kPalmFromHandle : kPalmFromHandle, kPalmTowardWrist, 0};
  r.m[0][3] = target.x - mid.x; r.m[1][3] = target.y - mid.y; r.m[2][3] = target.z - mid.z;
  out = r;
  return true;
}

const Mesh& gripHandMesh(bool left) { return gripHand(left).mesh; }
const V3* gripHandSkeleton(bool left) { return gripHand(left).joints; }

}  // namespace fh

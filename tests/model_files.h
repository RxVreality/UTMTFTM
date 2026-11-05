// Reads a controller's model from SteamVR's files, the way the overlay gets it through SteamVR's API, for the tests that
// run with no SteamVR running. The parts are the ones SteamVR draws, in the controller's own frame.
#pragma once
#include <json/json.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ghost3d.h"
#include "objload.h"

namespace fh {

inline bool modelFromFiles(const std::string& steamvr, const std::string& fullName, Mesh& mesh, std::vector<ModelPart>& parts) {
  namespace fs = std::filesystem;
  const size_t brace = fullName.find('}');
  const std::string name = brace == std::string::npos ? fullName : fullName.substr(brace + 1);
  std::vector<fs::path> roots = {fs::u8path(steamvr) / "resources" / "rendermodels" / name};
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(fs::u8path(steamvr) / "drivers", ec))
    roots.push_back(d.path() / "resources" / "rendermodels" / name);
  for (const fs::path& dir : roots) {
    std::ifstream in(dir / (name + ".json"));
    if (!in) continue;
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errs;
    if (!Json::parseFromStream(b, in, &v, &errs)) return false;
    for (const auto& part : v["components"].getMemberNames()) {
      const Json::Value& c = v["components"][part];
      if (!c.isMember("filename") || !c.get("visibility", Json::objectValue).get("default", true).asBool()) continue;
      Mesh piece;
      if (!loadObj((dir / c["filename"].asString()).u8string(), piece)) continue;
      const uint32_t base = (uint32_t)mesh.p.size(), first = (uint32_t)mesh.idx.size();
      V3 sum{0, 0, 0};
      for (size_t i = 0; i < piece.p.size(); ++i) {
        mesh.p.push_back(piece.p[i]);
        mesh.n.push_back(i < piece.n.size() ? piece.n[i] : V3{0, 1, 0});
        sum = sum + piece.p[i];
      }
      for (uint32_t i : piece.idx) mesh.idx.push_back(base + i);
      parts.push_back({part, first, (uint32_t)piece.idx.size(), sum * (1.f / float(piece.p.size()))});
    }
    return !mesh.p.empty();
  }
  return false;
}

}  // namespace fh

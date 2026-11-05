// Minimal Wavefront OBJ reader (v / vn / f, triangles or polygons) for SteamVR render models.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

#include "ghost3d.h"

namespace fh {

// Loads positions + normals into a Mesh (corners with the same v/vn pair are shared). Faces with
// no normals get a flat normal. Returns false if the file can't be read or has no faces.
inline bool loadObj(const std::string& path, Mesh& out) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return false;
  std::vector<V3> P, N;
  std::map<std::pair<int, int>, uint32_t> corner;
  Mesh m;
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == 'v' && line[1] == ' ') {
      V3 v; if (sscanf(line + 2, "%f %f %f", &v.x, &v.y, &v.z) == 3) P.push_back(v);
    } else if (line[0] == 'v' && line[1] == 'n') {
      V3 v; if (sscanf(line + 3, "%f %f %f", &v.x, &v.y, &v.z) == 3) N.push_back(v);
    } else if (line[0] == 'f' && line[1] == ' ') {
      std::vector<uint32_t> poly;
      char* s = line + 2;
      while (*s) {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s || *s == '\n' || *s == '\r') break;
        int vi = (int)strtol(s, &s, 10), ni = 0;
        if (*s == '/') {
          ++s;
          if (*s != '/') strtol(s, &s, 10);  // texture coordinate: unused
          if (*s == '/') { ++s; ni = (int)strtol(s, &s, 10); }
        }
        while (*s && *s != ' ' && *s != '\t' && *s != '\n') ++s;
        if (vi < 0) vi = (int)P.size() + 1 + vi;
        if (ni < 0) ni = (int)N.size() + 1 + ni;
        if (vi < 1 || vi > (int)P.size()) continue;
        if (ni > (int)N.size()) ni = 0;
        auto key = std::make_pair(vi, ni);
        auto it = corner.find(key);
        if (it == corner.end()) {
          it = corner.emplace(key, (uint32_t)m.p.size()).first;
          m.p.push_back(P[vi - 1]);
          m.n.push_back(ni ? norm(N[ni - 1]) : V3{0, 0, 0});
        }
        poly.push_back(it->second);
      }
      for (size_t i = 2; i < poly.size(); ++i) {
        uint32_t a = poly[0], b = poly[i - 1], c = poly[i];
        m.idx.insert(m.idx.end(), {a, b, c});
        V3 fn = norm(cross(m.p[b] - m.p[a], m.p[c] - m.p[a]));
        for (uint32_t k : {a, b, c}) if (len(m.n[k]) < 0.5f) m.n[k] = fn;
      }
    }
  }
  fclose(f);
  if (m.idx.empty()) return false;
  out = std::move(m);
  return true;
}

}  // namespace fh

// The Sims 3 camera hook: a lot's low-detail model, its impostor (milestones 66-71).
//
// The game draws every lot outside the active ones (and every lot in the neighbourhood view) as EA's
// ready-made low-detail model from the world file: one draw per lot (VS 074cd28f, PS 9c84a6b7), a
// simplified house over a flat ground plate, painted from one small atlas stored LINEAR (s2: colour,
// alpha a cut-out; s3: window glow, alpha the ambient occlusion). Its vertices carry a class in the
// packed normal's alpha: 0 ordinary, 0.75 reflective (glass, water), 1 the ground plate. The town
// ground has a hole under every lot; the plate is the lot's only ground there.
//
// This module holds what the device needs to draw such a model in parts: the plate's top split from
// the house (baked into the terrain by the runtime's terrain baker, with the hook's own plate shader),
// and the windows that glow at night (the game's switch, c3.x) picked from the glow atlas, as a
// window-only copy of it on a layer of the glow triangles lifted off the wall. The device side
// (sims3LotPlateEntry, sims3LotGlowTexture, sims3LotModelDraw) is in d3d9_device.cpp.
#pragma once

#include "sims3_camera_hook.h"

#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sims3cam {

// A lot's low-detail model, its impostor (runs 173-175): a simplified house and a flat ground plate,
// one draw per lot with VS 074cd28f and PS 9c84a6b7; s2 the plain colour (alpha a cut-out), s3 the
// window glow (rgb) and the ambient occlusion (alpha).
inline constexpr uint64_t kLotImpostorVs = 0x074cd28fc5260474ull;

// The low-detail lot's ground plate as terrain (milestone 69): the town ground has a hole under every
// lot and the low-detail model's plate -- a fan of up-facing triangles at the lot's ground height,
// class 1 (its packed normal's alpha 255), plus side triangles hanging 1.5 m down -- is the only ground
// there. The plate's top goes to the runtime's terrain baker (the visible terrain marker at stage 0 and
// the hook's own pixel shader: the plate's colour from EA's linear atlas, encoded to sRGB as the bake
// expects), so it takes the terrain's material and light; the rest of the model (the house, the
// plate's sides) is drawn as before. terrainLotPlate 0 = the model as one object, as before.
inline bool terrainLotPlate() { static int s = -1; if (s < 0) s = hookOption("terrainLotPlate", 1) != 0 ? 1 : 0; return s == 1; }
struct PlateSplitStats { uint32_t in = 0, plate = 0, house = 0, outside = 0; };
// pos: x, y, z per vertex (model space, y up); cls: the class byte per vertex; idx: triangles as
// vertex numbers. A triangle is the plate's top when its three corners are class 1 (255) and it lies
// flat. Out: the house's and the plate's triangles, as the same vertex numbers.
inline void splitLotPlate(const std::vector<float>& pos, const std::vector<uint8_t>& cls, const std::vector<uint32_t>& idx,
                          std::vector<uint32_t>& house, std::vector<uint32_t>& plate, PlateSplitStats& st) {
  const size_t n = cls.size();
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
    ++st.in;
    if (a >= n || b >= n || c >= n || pos.size() < 3 * n) { ++st.outside; continue; }
    bool top = cls[a] == 255 && cls[b] == 255 && cls[c] == 255;
    if (top) {
      const float ux = pos[3 * b] - pos[3 * a], uy = pos[3 * b + 1] - pos[3 * a + 1], uz = pos[3 * b + 2] - pos[3 * a + 2];
      const float vx = pos[3 * c] - pos[3 * a], vy = pos[3 * c + 1] - pos[3 * a + 1], vz = pos[3 * c + 2] - pos[3 * a + 2];
      const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
      const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
      top = len > 0.f && std::fabs(ny) > 0.999f * len;
    }
    std::vector<uint32_t>& out = top ? plate : house;
    out.push_back(a); out.push_back(b); out.push_back(c);
    if (top) ++st.plate; else ++st.house;
  }
}
// The low-detail lot model's window glow (milestone 70): its pixel shader (9c84a6b7) adds s3.rgb -- a
// glow atlas on the same coordinates, stored linear -- times c3.x, the game's switch (0 by day, 1 at
// night). The hook draws the model's glowing triangles once more with s3 at stage 0, blended ONE / ONE
// (the runtime's emissive blend: s3 times the texture factor as light) and the factor set to c3.x.
// lotGlow 0 = no glow pass.
inline constexpr uint64_t kLotImpostorPs = 0x9c84a6b7017f33fcull;
inline constexpr int kLotGlowStage = 3, kLotGlowScaleReg = 3;
inline constexpr uint8_t kLotGlowThreshold = 40;   // a texel glows above 40 / 255 (run 176: 14% of the house texels, mean 174, 165, 125)
inline bool lotGlow() { static int s = -1; if (s < 0) s = hookOption("lotGlow", 1) != 0 ? 1 : 0; return s == 1; }
// Every level-0 texel as A8R8G8B8 (DXT1, DXT3 / DXT5 colour blocks with alpha 255, A8R8G8B8, X8R8G8B8).
inline bool decodeColour(uint32_t format, const uint8_t* data, size_t size, uint32_t w, uint32_t hgt, std::vector<uint32_t>& out) {
  if (!data || w == 0 || hgt == 0 || w > 4096 || hgt > 4096) return false;
  const bool dxt1 = format == (uint32_t) D3DFMT_DXT1, dxt35 = format == (uint32_t) D3DFMT_DXT3 || format == (uint32_t) D3DFMT_DXT5;
  out.assign((size_t) w * hgt, 0xFF000000u);
  if (dxt1 || dxt35) {
    const uint32_t bw = (w + 3) / 4, bh = (hgt + 3) / 4, bs = dxt1 ? 8u : 16u;
    if (size < (size_t) bw * bh * bs) return false;
    for (uint32_t by = 0; by < bh; ++by) for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint8_t* b = data + ((size_t) by * bw + bx) * bs + (dxt1 ? 0u : 8u);
      const uint32_t c0 = b[0] | ((uint32_t) b[1] << 8), c1 = b[2] | ((uint32_t) b[3] << 8);
      const uint32_t bits = b[4] | ((uint32_t) b[5] << 8) | ((uint32_t) b[6] << 16) | ((uint32_t) b[7] << 24);
      uint32_t rgb[4][3];
      for (int k = 0; k < 2; ++k) { const uint32_t c = k ? c1 : c0; rgb[k][0] = ((c >> 11) & 31u) * 255u / 31u; rgb[k][1] = ((c >> 5) & 63u) * 255u / 63u; rgb[k][2] = (c & 31u) * 255u / 31u; }
      const bool four = !dxt1 || c0 > c1;
      for (int q = 0; q < 3; ++q) {
        rgb[2][q] = four ? (2 * rgb[0][q] + rgb[1][q]) / 3 : (rgb[0][q] + rgb[1][q]) / 2;
        rgb[3][q] = four ? (rgb[0][q] + 2 * rgb[1][q]) / 3 : 0;
      }
      for (uint32_t py = 0; py < 4; ++py) for (uint32_t px = 0; px < 4; ++px) {
        const uint32_t x = bx * 4 + px, y = by * 4 + py;
        if (x >= w || y >= hgt) continue;
        const uint32_t* c = rgb[(bits >> (2 * (py * 4 + px))) & 3u];
        out[(size_t) y * w + x] = 0xFF000000u | (c[0] << 16) | (c[1] << 8) | c[2];
      }
    }
    return true;
  }
  if (format == (uint32_t) D3DFMT_A8R8G8B8 || format == (uint32_t) D3DFMT_X8R8G8B8) {
    if (size < (size_t) w * hgt * 4) return false;
    for (size_t i = 0; i < (size_t) w * hgt; ++i) out[i] = 0xFF000000u | ((uint32_t) data[i * 4 + 2] << 16) | ((uint32_t) data[i * 4 + 1] << 8) | data[i * 4];
    return true;
  }
  return false;
}
inline uint8_t maxChannel(uint32_t argb) { const uint8_t r = (uint8_t) (argb >> 16), g = (uint8_t) (argb >> 8), b = (uint8_t) argb; return r > g ? (r > b ? r : b) : (g > b ? g : b); }
// The brightest of r, g, b of every level-0 texel (the glow test).
inline bool decodeMaxChannel(uint32_t format, const uint8_t* data, size_t size, uint32_t w, uint32_t hgt, std::vector<uint8_t>& out) {
  std::vector<uint32_t> argb;
  if (!decodeColour(format, data, size, w, hgt, argb)) return false;
  out.resize(argb.size());
  for (size_t i = 0; i < argb.size(); ++i) out[i] = maxChannel(argb[i]);
  return true;
}
// The window-only glow (milestone 71): every texel at or below the threshold black, so only the windows
// emit -- and, kept in one full-size level, they stay windows at a distance (the atlas's smaller
// levels average their light over the whole wall).
inline void windowOnlyGlow(std::vector<uint32_t>& argb, uint8_t threshold) {
  for (uint32_t& c : argb) if (maxChannel(c) <= threshold) c = 0xFF000000u;
}
// The glow layer sits this far in front of the surface it lights (milestone 71; game units = metres):
// the house's own triangles in the same place gave the ray tracer two surfaces in one spot.
inline constexpr float kLotGlowLift = 0.05f;
// One triangle's corners (x, y, z three times) moved along its face normal, turned to the side of
// the vertex normal vn, by lift.
inline void liftTriangle(float* p, const float* vn, float lift) {
  const float ux = p[3] - p[0], uy = p[4] - p[1], uz = p[5] - p[2], vx = p[6] - p[0], vy = p[7] - p[1], vz = p[8] - p[2];
  float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
  const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (!(len > 0.f)) return;
  nx /= len; ny /= len; nz /= len;
  if (nx * vn[0] + ny * vn[1] + nz * vn[2] < 0.f) { nx = -nx; ny = -ny; nz = -nz; }
  for (int k = 0; k < 3; ++k) { p[3 * k] += nx * lift; p[3 * k + 1] += ny * lift; p[3 * k + 2] += nz * lift; }
}
// The triangles (vertex numbers) that cover a texel of the glow texture brighter than the threshold:
// every texel centre inside the triangle in texture space, or the texel under its centre when it covers none.
inline void selectGlowTriangles(const std::vector<float>& uv, const std::vector<uint32_t>& tris, const std::vector<uint8_t>& glow, uint32_t w, uint32_t hgt,
                                uint8_t threshold, std::vector<uint32_t>& out) {
  if (glow.size() < (size_t) w * hgt || w == 0 || hgt == 0) return;
  const size_t n = uv.size() / 2;
  for (size_t t = 0; t + 2 < tris.size(); t += 3) {
    const uint32_t i0 = tris[t], i1 = tris[t + 1], i2 = tris[t + 2];
    if (i0 >= n || i1 >= n || i2 >= n) continue;
    const float x0 = uv[2 * i0] * w, y0 = uv[2 * i0 + 1] * hgt, x1 = uv[2 * i1] * w, y1 = uv[2 * i1 + 1] * hgt, x2 = uv[2 * i2] * w, y2 = uv[2 * i2 + 1] * hgt;
    const float den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    bool lit = false, covered = false;
    if (std::fabs(den) > 1e-9f) {
      const int xa = (std::max)(0, (int) std::floor((std::min)(x0, (std::min)(x1, x2)))), xb = (std::min)((int) w - 1, (int) std::ceil((std::max)(x0, (std::max)(x1, x2))));
      const int ya = (std::max)(0, (int) std::floor((std::min)(y0, (std::min)(y1, y2)))), yb = (std::min)((int) hgt - 1, (int) std::ceil((std::max)(y0, (std::max)(y1, y2))));
      for (int y = ya; y <= yb && !lit; ++y) for (int x = xa; x <= xb && !lit; ++x) {
        const float px = x + 0.5f, py = y + 0.5f;
        const float a = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den, b = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den, c = 1.f - a - b;
        if (a < -1e-4f || b < -1e-4f || c < -1e-4f) continue;
        covered = true;
        if (glow[(size_t) y * w + x] > threshold) lit = true;
      }
    }
    if (!covered) {
      const int x = (std::min)((int) w - 1, (std::max)(0, (int) ((x0 + x1 + x2) / 3.f))), y = (std::min)((int) hgt - 1, (std::max)(0, (int) ((y0 + y1 + y2) / 3.f)));
      lit = glow[(size_t) y * w + x] > threshold;
    }
    if (lit) { out.push_back(i0); out.push_back(i1); out.push_back(i2); }
  }
}
// The plate's pixel shader (ps_3_0): texld r0, v0 (TEXCOORD0), s2 -- the model's colour atlas, stored
// linear and sampled raw as the game does -- then rgb ^ (1 / 2.2) into oC0.rgb and 1 into oC0.a.
inline constexpr DWORD kLotPlatePs[] = {
  0xFFFF0300u,                                                 // ps_3_0
  0x05000051u, 0xA00F0000u, 0x3EE8BA2Fu, 0x3F800000u, 0x00000000u, 0x00000000u,   // def c0, 0.4545454, 1, 0, 0
  0x0200001Fu, 0x80000005u, 0x90030000u,                       // dcl_texcoord v0.xy
  0x0200001Fu, 0x90000000u, 0xA00F0802u,                       // dcl_2d s2
  0x03000042u, 0x800F0000u, 0x90E40000u, 0xA0E40802u,          // texld r0, v0, s2
  0x03000020u, 0x80010001u, 0x80000000u, 0xA0000000u,          // pow r1.x, r0.x, c0.x
  0x03000020u, 0x80020001u, 0x80550000u, 0xA0000000u,          // pow r1.y, r0.y, c0.x
  0x03000020u, 0x80040001u, 0x80AA0000u, 0xA0000000u,          // pow r1.z, r0.z, c0.x
  0x02000001u, 0x80070800u, 0x80E40001u,                       // mov oC0.xyz, r1
  0x02000001u, 0x80080800u, 0xA0550000u,                       // mov oC0.w, c0.y
  0x0000FFFFu,
};

}  // namespace sims3cam

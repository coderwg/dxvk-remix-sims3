// The Sims 3 camera hook, milestone 13: window and door openings cut into the wall geometry.
//
// The game's walls are plain quads. Windows and doors are holes the PIXEL shader cuts: it
// samples a per-lot mask atlas (DXT1) at a coordinate built from the vertex data and discards
// the texel where the mask reads black. The ray tracer never runs that pixel shader -- it
// shades the wall's albedo over the captured triangles -- so every opening is walled up and
// no light passes through. This module cuts the openings into the triangles instead, and the
// device substitutes the cut geometry for the game's at the draw.
//
// What the wall vertex shaders compute (walls A f64835cc, B 84b06922, C 25812b76, D bd6dc15e,
// all alike; register names from walls A):
//   k = clamp(flag.x, cK.x, cK.y) [x 0.99 in C]   flag = stream-1 UBYTE4 TEXCOORD5, cK = c8 (c12 in B)
//   s = TEXCOORD2.x / 4096                           across the segment
//   t = lerp(TEXCOORD2.z, TEXCOORD2.y, k) / 4096     down the segment: 0 = top, 1 = floor; k = 1 raises the wall
//   y = lerp(POSITION.w, POSITION.y, k) / 256        (k = 0: the cut-away stub, k = 1: full height)
//   atlas u = (rect.x + s * rect.z) / 4096, v = (rect.y + t * rect.w) / 4096     rect = TEXCOORD4 (SHORT4)
//   z = max(0.5 * (1 - t), max(-rect.x * 0.75 / 4096, 0))
// and the pixel shader discards where mask.red + z < 0.5 (texkill; walls C writes mask.red + z - 0.5
// to alpha and the draw is alpha-tested), gated by flag.y > 0.5 (a colour input). A plain
// segment carries rect (-4096, -4096, 4096, 4096): z >= 0.75, nothing discarded.
//
// The cut works in (s, t): per cell (rect) the discarded texels are read from the mask with the
// same rule, grouped into horizontal runs and bands of equal runs -> rectangles in (s, t); each
// triangle of the segment is clipped against them (Sutherland-Hodgman, four pieces per
// rectangle: left, right, above, below) and re-triangulated. New vertices interpolate every
// declared element from the triangle's corners by barycentric weight, so positions, texture
// coordinates, normals and the flags are consistent with the game's own vertices.
#pragma once

#include "sims3_camera_hook.h"

#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sims3cam {

// ---- the mask: one byte per texel, the red channel the shaders read -------------------------
// (decodeColour's red, milestone 154)
inline bool decodeMaskRed(uint32_t format, const uint8_t* data, size_t size, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
  std::vector<uint32_t> argb;
  if (!decodeColour(format, data, size, w, h, argb)) return false;
  out.resize(argb.size());
  for (size_t i = 0; i < argb.size(); ++i) out[i] = (uint8_t) (argb[i] >> 16);
  return true;
}

// ---- vertex elements ------------------------------------------------------------------------
inline uint32_t declTypeBytes(uint8_t type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1: return 4;
  case D3DDECLTYPE_FLOAT2: return 8;
  case D3DDECLTYPE_FLOAT3: return 12;
  case D3DDECLTYPE_FLOAT4: return 16;
  case D3DDECLTYPE_D3DCOLOR: case D3DDECLTYPE_UBYTE4: case D3DDECLTYPE_SHORT2: case D3DDECLTYPE_UBYTE4N:
  case D3DDECLTYPE_SHORT2N: case D3DDECLTYPE_USHORT2N: case D3DDECLTYPE_UDEC3: case D3DDECLTYPE_DEC3N: case D3DDECLTYPE_FLOAT16_2: return 4;
  case D3DDECLTYPE_SHORT4: case D3DDECLTYPE_SHORT4N: case D3DDECLTYPE_USHORT4N: case D3DDECLTYPE_FLOAT16_4: return 8;
  default: return 0;
  }
}
// Components the hook interpolates (0: the element is copied from the dominant corner).
inline uint32_t declTypeFloats(uint8_t type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1: return 1;
  case D3DDECLTYPE_FLOAT2: return 2;
  case D3DDECLTYPE_FLOAT3: return 3;
  case D3DDECLTYPE_FLOAT4: return 4;
  case D3DDECLTYPE_D3DCOLOR: case D3DDECLTYPE_UBYTE4: case D3DDECLTYPE_UBYTE4N: return 4;
  case D3DDECLTYPE_SHORT2: case D3DDECLTYPE_SHORT2N: case D3DDECLTYPE_USHORT2N: return 2;
  case D3DDECLTYPE_SHORT4: case D3DDECLTYPE_SHORT4N: case D3DDECLTYPE_USHORT4N: return 4;
  default: return 0;
  }
}
// Integer elements decode to their raw integer values (normalisation is linear, so interpolating
// the raw values and rounding back is the same thing).
inline void declDecode(uint8_t type, const uint8_t* p, float* f) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1: case D3DDECLTYPE_FLOAT2: case D3DDECLTYPE_FLOAT3: case D3DDECLTYPE_FLOAT4:
    memcpy(f, p, declTypeBytes(type)); break;
  case D3DDECLTYPE_D3DCOLOR: case D3DDECLTYPE_UBYTE4: case D3DDECLTYPE_UBYTE4N:
    for (int i = 0; i < 4; ++i) f[i] = (float) p[i]; break;
  case D3DDECLTYPE_SHORT2: case D3DDECLTYPE_SHORT2N: { int16_t v[2]; memcpy(v, p, 4); f[0] = v[0]; f[1] = v[1]; } break;
  case D3DDECLTYPE_SHORT4: case D3DDECLTYPE_SHORT4N: { int16_t v[4]; memcpy(v, p, 8); for (int i = 0; i < 4; ++i) f[i] = v[i]; } break;
  case D3DDECLTYPE_USHORT2N: { uint16_t v[2]; memcpy(v, p, 4); f[0] = v[0]; f[1] = v[1]; } break;
  case D3DDECLTYPE_USHORT4N: { uint16_t v[4]; memcpy(v, p, 8); for (int i = 0; i < 4; ++i) f[i] = v[i]; } break;
  default: break;
  }
}
inline void declEncode(uint8_t type, const float* f, uint8_t* p) {
  auto r8 = [](float v) -> uint8_t { return (uint8_t) (std::min)((std::max)((long) lroundf(v), 0L), 255L); };
  auto r16 = [](float v) -> int16_t { return (int16_t) (std::min)((std::max)((long) lroundf(v), -32768L), 32767L); };
  auto u16 = [](float v) -> uint16_t { return (uint16_t) (std::min)((std::max)((long) lroundf(v), 0L), 65535L); };
  switch (type) {
  case D3DDECLTYPE_FLOAT1: case D3DDECLTYPE_FLOAT2: case D3DDECLTYPE_FLOAT3: case D3DDECLTYPE_FLOAT4:
    memcpy(p, f, declTypeBytes(type)); break;
  case D3DDECLTYPE_D3DCOLOR: case D3DDECLTYPE_UBYTE4: case D3DDECLTYPE_UBYTE4N:
    for (int i = 0; i < 4; ++i) p[i] = r8(f[i]); break;
  case D3DDECLTYPE_SHORT2: case D3DDECLTYPE_SHORT2N: { int16_t v[2] = { r16(f[0]), r16(f[1]) }; memcpy(p, v, 4); } break;
  case D3DDECLTYPE_SHORT4: case D3DDECLTYPE_SHORT4N: { int16_t v[4] = { r16(f[0]), r16(f[1]), r16(f[2]), r16(f[3]) }; memcpy(p, v, 8); } break;
  case D3DDECLTYPE_USHORT2N: { uint16_t v[2] = { u16(f[0]), u16(f[1]) }; memcpy(p, v, 4); } break;
  case D3DDECLTYPE_USHORT4N: { uint16_t v[4] = { u16(f[0]), u16(f[1]), u16(f[2]), u16(f[3]) }; memcpy(p, v, 8); } break;
  default: break;
  }
}

// ---- the wall vertex layout, from the bound declaration -------------------------------------
// Stream 0: SHORT4 POSITION, SHORT4 TEXCOORD2 (s, t up, t down, -), SHORT4 TEXCOORD4 (the cell:
// x, y, w, h in 1/4096 of the atlas); stream 1: UBYTE4 TEXCOORD5 (up-ness, gate, visible, -).
struct WallLayout {
  bool valid = false;
  struct Elem { uint8_t stream, type; uint16_t offset; };
  Elem elems[24];
  uint8_t elemCount = 0;
  int16_t posOff = -1, tc2Off = -1, tc4Off = -1, flagOff = -1;
  uint16_t minStride0 = 0, minStride1 = 0;   // bytes the elements need per stream
};

inline bool wallLayoutFromDecl(const D3DVERTEXELEMENT9* e, WallLayout& out) {
  out = WallLayout();
  if (!e) return false;
  for (int i = 0; e[i].Stream != 0xFF; ++i) {
    if (e[i].Type == D3DDECLTYPE_UNUSED) continue;
    const uint32_t bytes = declTypeBytes((uint8_t) e[i].Type);
    if (e[i].Stream > 1 || bytes == 0 || out.elemCount >= 24) return false;   // more than two streams, an element the cut cannot carry, or too many
    WallLayout::Elem& w = out.elems[out.elemCount++];
    w.stream = (uint8_t) e[i].Stream; w.type = (uint8_t) e[i].Type; w.offset = e[i].Offset;
    uint16_t& ms = e[i].Stream ? out.minStride1 : out.minStride0;
    ms = (uint16_t) std::max<uint32_t>(ms, e[i].Offset + bytes);
    if (e[i].Stream == 0 && e[i].Type == D3DDECLTYPE_SHORT4) {
      if (e[i].Usage == D3DDECLUSAGE_POSITION && e[i].UsageIndex == 0) out.posOff = (int16_t) e[i].Offset;
      else if (e[i].Usage == D3DDECLUSAGE_TEXCOORD && e[i].UsageIndex == 2) out.tc2Off = (int16_t) e[i].Offset;
      else if (e[i].Usage == D3DDECLUSAGE_TEXCOORD && e[i].UsageIndex == 4) out.tc4Off = (int16_t) e[i].Offset;
    } else if (e[i].Stream == 1 && e[i].Type == D3DDECLTYPE_UBYTE4 && e[i].Usage == D3DDECLUSAGE_TEXCOORD && e[i].UsageIndex == 5) {
      out.flagOff = (int16_t) e[i].Offset;
    }
  }
  out.valid = out.posOff >= 0 && out.tc2Off >= 0 && out.tc4Off >= 0 && out.flagOff >= 0;
  return out.valid;
}

// ---- the wall vertex shader: where its clamp of the up-ness flag lives ----------------------
// Found from the bytecode: inputs TEXCOORD2 / 4 / 5 declared, a MAX of the TEXCOORD5 input
// against a constant register (the clamp's lower bound), a MIN of that against the same
// register (the upper bound) -> k; a following MUL of k by a literal (walls C: 0.99) scales it.
struct WallVsInfo {
  bool valid = false;
  uint8_t clampReg = 0;
  float kScale = 1.f;
};

inline bool analyzeWallVertexShader(const DWORD* tokens, size_t count, WallVsInfo& out) {
  out = WallVsInfo();
  if (!dxsoIsVertexShader(tokens, count, 2)) return false;
  const uint32_t kTemp = 0u, kConst = 2u;
  int tc2 = -1, tc4 = -1, tc5 = -1;
  float defs[256][4]; bool defd[256] = {};
  int clampReg = -1, maxTemp = -1, kReg = -1; uint32_t kMask = 0; bool kDone = false;
  auto isInput = [&](uint32_t tok, int reg) { return reg >= 0 && dxsoRegType(tok) == kDxsoRegInput && (int) dxsoRegNum(tok) == reg; };
  auto isTemp = [&](uint32_t tok, int reg) { return reg >= 0 && dxsoRegType(tok) == kTemp && (int) dxsoRegNum(tok) == reg; };
  auto isClamp = [&](uint32_t tok) { return clampReg >= 0 && dxsoRegType(tok) == kConst && (int) dxsoRegNum(tok) == clampReg; };
  dxsoForEach(tokens, count, [&](size_t pos, uint32_t op, uint32_t len) {
    if (op == kDxsoOpDcl) {
      uint32_t usage, idx, reg;
      if (dxsoDcl(tokens, pos, len, kDxsoRegInput, usage, idx, reg) && usage == kUsageTexcoord) {
        if (idx == 2) tc2 = (int) reg; else if (idx == 4) tc4 = (int) reg; else if (idx == 5) tc5 = (int) reg;
      }
      return true;
    }
    if (op == 0x51u) {                                                     // DEF c#, x, y, z, w
      if (len >= 5 && dxsoRegType(tokens[pos + 1]) == kConst) { const uint32_t r = dxsoRegNum(tokens[pos + 1]); if (r < 256) { memcpy(defs[r], &tokens[pos + 2], 16); defd[r] = true; } }
      return true;
    }
    if (op == 0x52u || op == 0x53u || len < 1) return true;
    const uint32_t dest = tokens[pos + 1];
    if (clampReg < 0) {
      if (op == 0x0Bu && len >= 3) {                                       // MAX dest, flag, cK  (or cK, flag)
        const uint32_t a = tokens[pos + 2], b = tokens[pos + 3];
        if (isInput(a, tc5) && dxsoRegType(b) == kConst) clampReg = (int) dxsoRegNum(b);
        else if (isInput(b, tc5) && dxsoRegType(a) == kConst) clampReg = (int) dxsoRegNum(a);
        if (clampReg >= 0 && dxsoRegType(dest) == kTemp) maxTemp = (int) dxsoRegNum(dest);
      }
      return true;
    }
    if (kReg < 0) {
      if (op == 0x0Au && len >= 3) {                                       // MIN dest, max, cK.y -> k
        const uint32_t a = tokens[pos + 2], b = tokens[pos + 3];
        if (((isTemp(a, maxTemp) && isClamp(b)) || (isTemp(b, maxTemp) && isClamp(a))) && dxsoRegType(dest) == kTemp) { kReg = (int) dxsoRegNum(dest); kMask = (dest >> 16) & 0xFu; }
      }
      return true;
    }
    if (!kDone && dxsoRegType(dest) == kTemp && (int) dxsoRegNum(dest) == kReg && (((dest >> 16) & 0xFu) & kMask)) {
      // the first instruction writing k again: a scale by a literal, or the end of k's life
      if (op == 0x05u && len >= 3) {
        const uint32_t a = tokens[pos + 2], b = tokens[pos + 3];
        const uint32_t c = isTemp(a, kReg) ? b : isTemp(b, kReg) ? a : 0u;
        if (c && dxsoRegType(c) == kConst && dxsoRegNum(c) < 256 && defd[dxsoRegNum(c)]) out.kScale = defs[dxsoRegNum(c)][(c >> 16) & 3u];
      }
      kDone = true;
    }
    return true;
  });
  out.valid = tc2 >= 0 && tc4 >= 0 && tc5 >= 0 && clampReg >= 0 && clampReg < 256 && kReg >= 0;
  if (out.valid) out.clampReg = (uint8_t) clampReg;
  return out.valid;
}

// ---- the cut ----------------------------------------------------------------------------------
struct WallCutParams {
  float clampLo = 0.f, clampHi = 1.f;   // the shader's clamp of the per-vertex up-ness flag (cK.x, cK.y)
  float clampVis = 0.f;                 // cK.z: the lower bound of the visibility flag (1 = every vertex visible)
  float kScale = 1.f;
  float threshold = 0.5f;               // a texel is discarded where mask.red + z < threshold; below 0: no opening
                                        // test (the mask unknown or unreadable) -- the hidden triangles still go
};

struct WallCutInput {
  WallLayout layout;
  const uint8_t* vb0 = nullptr; size_t vb0Size = 0; uint32_t offset0 = 0, stride0 = 0;
  const uint8_t* vb1 = nullptr; size_t vb1Size = 0; uint32_t offset1 = 0, stride1 = 0;
  const uint8_t* ib = nullptr; size_t ibSize = 0; bool ib32 = false;
  int32_t baseVertex = 0; uint32_t startIndex = 0, primCount = 0;
  const uint8_t* mask = nullptr; uint32_t maskW = 0, maskH = 0;   // the red plane; null = black everywhere (unread below threshold 0)
  WallCutParams params;
};

struct WallCutStats {
  uint32_t triangles = 0, cut = 0, removed = 0, cells = 0, cellsWithOpenings = 0, holeRects = 0;
  uint32_t hidden = 0, hiddenMixed = 0, hiddenCorners = 0;   // triangles dropped for a hidden corner (mixed: not all three)
};

struct WallCutOutput {
  bool changed = false;               // false: nothing to cut, draw the game's geometry
  std::vector<uint8_t> vb0, vb1;      // the draw's vertices, both streams, from vertex 0
  std::vector<uint16_t> ib;
  uint32_t vertexCount = 0, triangleCount = 0;
  WallCutStats stats;
};

inline bool cutWallOpenings(const WallCutInput& in, WallCutOutput& out) {
  out = WallCutOutput();
  const WallLayout& L = in.layout;
  if (!L.valid || !in.vb0 || !in.vb1 || !in.ib || in.primCount == 0 || in.primCount > 65535) return false;
  if (in.stride0 < L.minStride0 || in.stride1 < L.minStride1 || in.stride0 > 256 || in.stride1 > 256) return false;
  const size_t idxBytes = in.ib32 ? 4 : 2;
  if (((size_t) in.startIndex + (size_t) in.primCount * 3) * idxBytes > in.ibSize) return false;
  const float thr = in.params.threshold;

  auto vertexIndex = [&](uint32_t i) -> int64_t {
    const size_t k = (size_t) in.startIndex + i;
    return (int64_t) readIndex((const uint8_t*) in.ib, k, in.ib32) + in.baseVertex;
  };
  // vertex v's record in a stream, null when it lies outside the buffer
  auto record = [](const uint8_t* buf, size_t size, uint32_t offset, uint32_t stride, int64_t v) -> const uint8_t* {
    if (v < 0) return nullptr;
    const uint64_t off = (uint64_t) offset + (uint64_t) v * stride;
    return off + stride <= size ? buf + off : nullptr;
  };
  auto bytes0 = [&](int64_t v) { return record(in.vb0, in.vb0Size, in.offset0, in.stride0, v); };
  auto bytes1 = [&](int64_t v) { return record(in.vb1, in.vb1Size, in.offset1, in.stride1, v); };

  // per-vertex facts, as the vertex shader computes them
  struct Facts { float s, t; bool gate, vis; int16_t rect[4]; };
  auto facts = [&](const uint8_t* p0, const uint8_t* p1, Facts& f) {
    int16_t tc2[4]; memcpy(tc2, p0 + L.tc2Off, 8); memcpy(f.rect, p0 + L.tc4Off, 8);
    const uint8_t* fl = p1 + L.flagOff;
    const float k = (std::min)((std::max)((float) fl[0], in.params.clampLo), in.params.clampHi) * in.params.kScale;
    f.s = tc2[0] / 4096.f;
    f.t = (tc2[2] + k * (tc2[1] - tc2[2])) / 4096.f;
    f.gate = fl[1] > 0;
    f.vis = (std::max)((float) fl[2], in.params.clampVis) >= 0.5f;   // the shader scales clip xyz by max(flag.z, cK.z)
  };

  // the openings of a cell, in (s, t) of its segment, and the rest of the plane as the solid
  // rectangles a cut triangle is clipped to (milestone 78); all triangles of a segment share the cell
  struct Hole { float s0, s1, t0, t1; };
  struct Cell { int16_t rect[4]; std::vector<Hole> holes, solids; };
  std::vector<Cell> cells;
  auto overlaps = [](float s0, float s1, float t0, float t1, const Hole& h) { return !(s1 <= h.s0 || s0 >= h.s1 || t1 <= h.t0 || t0 >= h.t1); };
  auto cellIndex = [&](const int16_t rect[4]) -> size_t {
    for (size_t i = 0; i < cells.size(); ++i) if (!memcmp(cells[i].rect, rect, 8)) return i;
    Cell c; memcpy(c.rect, rect, 8);
    // a plain segment (the negative marker rectangle; z >= 0.75 in the shader) is never cut: its
    // texel reads land outside the atlas, where the game's sampler and this code would differ
    const float zFloor = (std::max)(0.f, -rect[0] * (0.75f / 4096.f));
    if (rect[0] >= 0 && rect[1] >= 0 && zFloor < thr && rect[2] > 0 && rect[3] > 0) {
      const uint32_t W = in.mask ? in.maskW : 256u, H = in.mask ? in.maskH : 256u;
      const float x0 = rect[0] * (float) W / 4096.f, y0 = rect[1] * (float) H / 4096.f;
      const float cw = rect[2] * (float) W / 4096.f, ch = rect[3] * (float) H / 4096.f;
      const int px0 = (int) floorf(x0), px1 = (int) ceilf(x0 + cw), py0 = (int) floorf(y0), py1 = (int) ceilf(y0 + ch);
      auto sOf = [&](int px) { return (px * 4096.f / (float) W - rect[0]) / (float) rect[2]; };   // a texel column edge in s
      auto tOf = [&](int py) { return (py * 4096.f / (float) H - rect[1]) / (float) rect[3]; };   // a texel row edge in t
      struct Run { int x0, x1; };
      std::vector<Run> prev, cur; int bandY0 = py0;
      // a band of rows with the same openings gives its holes and its solid runs (the complement in s,
      // unbounded at both ends), each merged into the solid rectangle above it when their s extents match
      const float kFar = 1e6f;
      auto solidBand = [&](float t0, float t1, const std::vector<Run>& runs) {
        float s = -kFar;
        auto add = [&](float s0, float s1) {
          if (s1 <= s0) return;
          for (Hole& r : c.solids) if (r.t1 == t0 && r.s0 == s0 && r.s1 == s1) { r.t1 = t1; return; }
          c.solids.push_back({ s0, s1, t0, t1 });
        };
        for (const Run& r : runs) { add(s, sOf(r.x0)); s = sOf(r.x1); }
        add(s, kFar);
      };
      solidBand(-kFar, tOf(py0), {});
      auto flush = [&](int yEnd) {
        if (yEnd <= bandY0) return;
        for (const Run& r : prev) c.holes.push_back({ sOf(r.x0), sOf(r.x1), tOf(bandY0), tOf(yEnd) });
        solidBand(tOf(bandY0), tOf(yEnd), prev);
      };
      for (int py = py0; py < py1; ++py) {
        cur.clear();
        // the texel row's centre; a row straddling the cell's top edge counts as just inside it
        // (the shader keeps only the very top, t = 0)
        const float t = (std::max)((py + 0.5f - y0) / ch, 1e-4f);
        const float z = (std::max)(0.5f * (1.f - t), zFloor);
        if (z < thr) {
          const float need = (thr - z) * 255.f;                          // discarded where red < need
          int runStart = -1;
          for (int px = px0; px <= px1; ++px) {
            bool killed = false;
            if (px < px1) {
              float m = 0.f;
              if (in.mask) {
                const int cx = (std::min)((std::max)(px, 0), (int) W - 1), cy = (std::min)((std::max)(py, 0), (int) H - 1);
                m = (float) in.mask[(size_t) cy * W + cx];
              }
              killed = m < need;
            }
            if (killed && runStart < 0) runStart = px;
            else if (!killed && runStart >= 0) { cur.push_back({ runStart, px }); runStart = -1; }
          }
        }
        bool same = cur.size() == prev.size();
        for (size_t i = 0; same && i < cur.size(); ++i) same = cur[i].x0 == prev[i].x0 && cur[i].x1 == prev[i].x1;
        if (!same) { flush(py); prev = cur; bandY0 = py; }
      }
      flush(py1);
      solidBand(tOf(py1), kFar, {});
      if (c.holes.empty()) c.solids.clear();
    }
    ++out.stats.cells;
    if (!c.holes.empty()) { ++out.stats.cellsWithOpenings; out.stats.holeRects += (uint32_t) c.holes.size(); }
    cells.push_back(std::move(c));
    return cells.size() - 1;
  };

  // polygons in (s, t) with barycentric weights over the triangle's corners
  struct PV { float s, t, w0, w1, w2; };
  using Poly = std::vector<PV>;
  auto clipHalf = [](const Poly& p, bool axisT, float c, bool keepBelow, Poly& r) {
    r.clear();
    const size_t n = p.size();
    if (n < 3) return;
    auto d = [&](const PV& v) { const float x = axisT ? v.t : v.s; return keepBelow ? c - x : x - c; };   // >= 0: kept
    for (size_t i = 0; i < n; ++i) {
      const PV& a = p[i]; const PV& b = p[(i + 1) % n];
      const float da = d(a), db = d(b);
      if (da >= 0.f) r.push_back(a);
      if ((da >= 0.f) != (db >= 0.f)) {
        const float f = da / (da - db);
        r.push_back({ a.s + (b.s - a.s) * f, a.t + (b.t - a.t) * f, a.w0 + (b.w0 - a.w0) * f, a.w1 + (b.w1 - a.w1) * f, a.w2 + (b.w2 - a.w2) * f });
      }
    }
  };
  auto area2 = [](const Poly& p) { float a = 0.f; for (size_t i = 0, n = p.size(); i < n; ++i) { const PV& u = p[i]; const PV& v = p[(i + 1) % n]; a += u.s * v.t - v.s * u.t; } return a; };
  auto keep = [&](std::vector<Poly>& dst, Poly& p) { if (p.size() >= 3 && std::fabs(area2(p)) > 1e-9f) dst.push_back(std::move(p)); };

  // the output vertices: both streams' bytes per vertex, deduplicated
  const uint32_t rec = in.stride0 + in.stride1;
  std::vector<uint8_t> vbuf;
  std::unordered_map<uint64_t, std::vector<uint32_t>> vmap;
  auto emitBytes = [&](const uint8_t* b) -> int64_t {
    std::vector<uint32_t>& bucket = vmap[fnv1a64(b, rec)];
    for (uint32_t idx : bucket) if (!memcmp(&vbuf[(size_t) idx * rec], b, rec)) return idx;
    if (out.vertexCount >= 65535) return -1;
    const uint32_t idx = out.vertexCount++;
    vbuf.insert(vbuf.end(), b, b + rec);
    bucket.push_back(idx);
    return idx;
  };
  std::vector<uint8_t> tmp(rec);
  auto emitOriginal = [&](const uint8_t* p0, const uint8_t* p1) -> int64_t {
    memcpy(tmp.data(), p0, in.stride0); memcpy(tmp.data() + in.stride0, p1, in.stride1);
    return emitBytes(tmp.data());
  };
  auto emitBlend = [&](const PV& v, const uint8_t* const p0[3], const uint8_t* const p1[3]) -> int64_t {
    const float w[3] = { v.w0, v.w1, v.w2 };
    int dom = 0; if (w[1] > w[dom]) dom = 1; if (w[2] > w[dom]) dom = 2;
    memcpy(tmp.data(), p0[dom], in.stride0); memcpy(tmp.data() + in.stride0, p1[dom], in.stride1);
    for (uint32_t e = 0; e < L.elemCount; ++e) {
      const WallLayout::Elem& el = L.elems[e];
      const uint32_t nf = declTypeFloats(el.type);
      if (!nf) continue;
      float acc[4] = {}, f[4] = {};
      for (int i = 0; i < 3; ++i) {
        declDecode(el.type, (el.stream ? p1[i] : p0[i]) + el.offset, f);
        for (uint32_t c = 0; c < nf; ++c) acc[c] += w[i] * f[c];
      }
      declEncode(el.type, acc, tmp.data() + (el.stream ? in.stride0 : 0u) + el.offset);
    }
    return emitBytes(tmp.data());
  };

  std::vector<uint32_t> tris;
  tris.reserve((size_t) in.primCount * 3);
  std::vector<Poly> pieces;
  for (uint32_t tI = 0; tI < in.primCount; ++tI) {
    ++out.stats.triangles;
    const uint8_t* p0[3] = {}; const uint8_t* p1[3] = {}; Facts f[3] = {};
    for (int i = 0; i < 3; ++i) {
      const int64_t v = vertexIndex(tI * 3 + i);
      p0[i] = bytes0(v); p1[i] = bytes1(v);
      if (!p0[i] || !p1[i]) return false;                                 // a vertex outside the buffers: not ours to rewrite
      facts(p0[i], p1[i], f[i]);
    }
    // A hidden vertex (flag.z = 0 with cK.z = 0) is collapsed to the clip centre by the shader, and
    // the fan that makes of its triangle is hidden in-game by the mask the pixel shader reads; the
    // ray tracer, never running that pixel shader, showed the fans (the "popping walls" at close
    // zoom, runs 22-29). Nothing of such a triangle is visible in-game: dropped.
    const int hiddenCorners = (f[0].vis ? 0 : 1) + (f[1].vis ? 0 : 1) + (f[2].vis ? 0 : 1);
    if (hiddenCorners) {
      ++out.stats.hidden; out.stats.hiddenCorners += (uint32_t) hiddenCorners;
      if (hiddenCorners < 3) ++out.stats.hiddenMixed;
      continue;
    }
    bool cut = f[0].gate && f[1].gate && f[2].gate && !memcmp(f[0].rect, f[1].rect, 8) && !memcmp(f[0].rect, f[2].rect, 8);
    const Cell* cell = nullptr;
    if (cut) { cell = &cells[cellIndex(f[0].rect)]; cut = !cell->holes.empty(); }
    float s0 = 1e9f, s1 = -1e9f, t0 = 1e9f, t1 = -1e9f;
    for (int i = 0; i < 3; ++i) { s0 = (std::min)(s0, f[i].s); s1 = (std::max)(s1, f[i].s); t0 = (std::min)(t0, f[i].t); t1 = (std::max)(t1, f[i].t); }
    if (cut) {
      cut = false;
      for (const Hole& h : cell->holes) if (overlaps(s0, s1, t0, t1, h)) { cut = true; break; }
      // a triangle with no area in (s, t) -- an edge-on face whose texels lie on a line -- is left
      // whole: the subtraction would produce nothing and drop it
      const float area2 = (f[1].s - f[0].s) * (f[2].t - f[0].t) - (f[2].s - f[0].s) * (f[1].t - f[0].t);
      if (std::fabs(area2) < 1e-9f) cut = false;
    }
    if (!cut) {
      for (int i = 0; i < 3; ++i) { const int64_t idx = emitOriginal(p0[i], p1[i]); if (idx < 0) return false; tris.push_back((uint32_t) idx); }
      continue;
    }
    // the triangle clipped to each solid rectangle it overlaps (milestone 78): the pieces tile it minus
    // the openings exactly, however many rectangles the openings take (subtracting hole after hole
    // multiplied the pieces, so a cell of more than 64 used to be cut as their bounding box)
    pieces.clear();
    const Poly tri = { { f[0].s, f[0].t, 1.f, 0.f, 0.f }, { f[1].s, f[1].t, 0.f, 1.f, 0.f }, { f[2].s, f[2].t, 0.f, 0.f, 1.f } };
    Poly a, b;
    for (const Hole& r : cell->solids) {
      if (!overlaps(s0, s1, t0, t1, r)) continue;
      clipHalf(tri, false, r.s0, false, a); clipHalf(a, false, r.s1, true, b);
      clipHalf(b, true, r.t0, false, a); clipHalf(a, true, r.t1, true, b);
      keep(pieces, b);
    }
    if (pieces.empty()) { ++out.stats.removed; continue; }
    ++out.stats.cut;
    for (const Poly& p : pieces) {
      const int64_t i0 = emitBlend(p[0], p0, p1);
      if (i0 < 0) return false;
      for (size_t k = 1; k + 1 < p.size(); ++k) {
        const int64_t i1 = emitBlend(p[k], p0, p1), i2 = emitBlend(p[k + 1], p0, p1);
        if (i1 < 0 || i2 < 0) return false;
        if (i0 != i1 && i1 != i2 && i0 != i2) { tris.push_back((uint32_t) i0); tris.push_back((uint32_t) i1); tris.push_back((uint32_t) i2); }
      }
    }
  }
  out.changed = out.stats.cut > 0 || out.stats.removed > 0 || out.stats.hidden > 0;
  if (!out.changed) { out.vertexCount = 0; return true; }
  out.vb0.resize((size_t) out.vertexCount * in.stride0);
  out.vb1.resize((size_t) out.vertexCount * in.stride1);
  for (uint32_t v = 0; v < out.vertexCount; ++v) {
    memcpy(&out.vb0[(size_t) v * in.stride0], &vbuf[(size_t) v * rec], in.stride0);
    memcpy(&out.vb1[(size_t) v * in.stride1], &vbuf[(size_t) v * rec + in.stride0], in.stride1);
  }
  out.ib.resize(tris.size());
  for (size_t i = 0; i < tris.size(); ++i) out.ib[i] = (uint16_t) tris[i];
  out.triangleCount = (uint32_t) (tris.size() / 3);
  return true;
}

// ---- a zero-thickness wall's back side (milestones 97, 97c) ----------------------------------
// A pool's walls have no thickness: the wall object (one vertex buffer) carries the pool side and,
// on the very same planes and areas facing the other way, the outer side (runs 207-209: the tile
// side 344 triangles, then 104 + 240; texture hashes 73C9E3A9 / 254EA684 / 12B386FA; on every wall
// plane the tile side's count facing one way equals the outer pieces' facing the other, the outer
// squares cut along the other diagonal -- no triangle shares its corners -- and the vertex data the
// same at every camera). The game culls the side facing away. The runtime does not cull rays that
// have passed through a translucent surface (rtx.enableCullingInSecondaryRays changed nothing), so
// behind the water both sides came back at one distance and fought pixel by pixel -- the speckle on
// the pool walls. A wall piece whose triangles all lie on planes where an earlier piece of the same
// vertex buffer has triangles facing the other way is such a back side and is left out; a wall with a
// thickness has its two sides on different planes.
// A triangle's key: its plane (the integer normal of its stored positions, reduced, and the plane's
// offset), signed so the first nonzero normal component is positive, and the facing in the low bit --
// the plane faced the other way is the key ^ 1. Degenerate triangles have none (0).
inline uint64_t wallTriKey(const int16_t* a, const int16_t* b, const int16_t* c) {
  const int64_t u[3] = { (int64_t) b[0] - a[0], (int64_t) b[1] - a[1], (int64_t) b[2] - a[2] };
  const int64_t v[3] = { (int64_t) c[0] - a[0], (int64_t) c[1] - a[1], (int64_t) c[2] - a[2] };
  int64_t n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
  int64_t g = 0;
  for (int64_t x : n) { int64_t y = x < 0 ? -x : x; while (y) { const int64_t t = g % y; g = y; y = t; } }
  if (g == 0) return 0;
  for (int64_t& x : n) x /= g;
  const int64_t s = n[0] ? n[0] : n[1] ? n[1] : n[2];
  const bool facing = s > 0;
  if (!facing) for (int64_t& x : n) x = -x;
  const int64_t key[4] = { n[0], n[1], n[2], n[0] * a[0] + n[1] * a[1] + n[2] * a[2] };
  const uint64_t k = fnv1a64(key, sizeof key) << 1 | (facing ? 1u : 0u);
  return k ? k : 2u;
}
// Whether a piece (its triangles' keys) is the back side of the triangles kept so far: every one of
// its triangles reversed among them. Counts the reversed ones in matched.
template<typename Set>
inline bool isWallBackSide(const std::vector<uint64_t>& piece, const Set& kept, uint32_t& matched) {
  matched = 0; uint32_t n = 0;
  for (uint64_t k : piece) { if (!k) continue; ++n; if (kept.count(k ^ 1u)) ++matched; }
  return n > 0 && matched == n;
}

}  // namespace sims3cam

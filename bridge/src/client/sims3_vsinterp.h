#pragma once
// The Sims 3 camera hook: a vertex shader's position output, evaluated on the client for one vertex
// (milestone 119). Remix keeps no motion for an instance with a translucent material (its instance
// manager drops the per-vertex previous positions: "motion vectors on translucent surfaces cannot be
// trusted", TREX-634), and our draws are vertex-captured in world space with an identity WORLD
// transform, so a moving glass object is held still by the denoiser and trails. With
// rtx.useWorldMatricesForShaders the runtime takes D3DTS_WORLD as the instance's transform and puts
// the captured vertices into its space; a WORLD transform that follows the object gives the
// instance its motion back (the way the GTA IV compatibility mod gives Remix every object's world
// matrix). The object's place comes from running the draw's own vertex shader for one of its
// vertices: the clip position, back through the camera the runtime holds, is a world position.
//
// The evaluator covers what the game's vertex shaders use (the survey of 3210 shaders: mov, add,
// mul, mad, dp3, dp4, rcp, rsq, min, max, slt, sge, frc, exp, log, lit, lrp, pow, abs, nrm, sincos,
// mova, the matrix macros, if / if_cmp / else / endif); a loop, a call, a texture read, predication
// or a relative destination makes it give up (false), and the draw keeps the identity transform.
#include <cmath>
#include <cstdint>
#include <cstring>
#include "sims3_camera_hook.h"

namespace sims3cam {

struct VsConstants { const float* f = nullptr; const int* i = nullptr; const BOOL* b = nullptr; };   // 256 x 4 floats, 16 x 4 ints, 16 bools
struct VsRun { const char* failed = nullptr; };

// One vertex element as the shader reads it: (0, 0, 0, 1) where the element has no component.
inline bool vsDecodeElement(uint8_t type, const uint8_t* p, float out[4]) {
  out[0] = out[1] = out[2] = 0.f; out[3] = 1.f;
  auto half = [](uint16_t hv) -> float {
    const uint32_t s = (hv >> 15) & 1u, e = (hv >> 10) & 0x1Fu, m = hv & 0x3FFu;
    float v;
    if (e == 0) v = std::ldexp((float) m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float) (m | 0x400u), (int) e - 25);
    return s ? -v : v;
  };
  switch (type) {
  case D3DDECLTYPE_FLOAT1: memcpy(out, p, 4); break;
  case D3DDECLTYPE_FLOAT2: memcpy(out, p, 8); break;
  case D3DDECLTYPE_FLOAT3: memcpy(out, p, 12); break;
  case D3DDECLTYPE_FLOAT4: memcpy(out, p, 16); break;
  case D3DDECLTYPE_D3DCOLOR: out[0] = p[2] / 255.f; out[1] = p[1] / 255.f; out[2] = p[0] / 255.f; out[3] = p[3] / 255.f; break;   // B, G, R, A in memory
  case D3DDECLTYPE_UBYTE4: for (int i = 0; i < 4; ++i) out[i] = (float) p[i]; break;
  case D3DDECLTYPE_UBYTE4N: for (int i = 0; i < 4; ++i) out[i] = p[i] / 255.f; break;
  case D3DDECLTYPE_SHORT2: { int16_t v[2]; memcpy(v, p, 4); out[0] = v[0]; out[1] = v[1]; } break;
  case D3DDECLTYPE_SHORT4: { int16_t v[4]; memcpy(v, p, 8); for (int i = 0; i < 4; ++i) out[i] = v[i]; } break;
  case D3DDECLTYPE_SHORT2N: { int16_t v[2]; memcpy(v, p, 4); for (int i = 0; i < 2; ++i) out[i] = (std::max)(v[i] / 32767.f, -1.f); } break;
  case D3DDECLTYPE_SHORT4N: { int16_t v[4]; memcpy(v, p, 8); for (int i = 0; i < 4; ++i) out[i] = (std::max)(v[i] / 32767.f, -1.f); } break;
  case D3DDECLTYPE_USHORT2N: { uint16_t v[2]; memcpy(v, p, 4); out[0] = v[0] / 65535.f; out[1] = v[1] / 65535.f; } break;
  case D3DDECLTYPE_USHORT4N: { uint16_t v[4]; memcpy(v, p, 8); for (int i = 0; i < 4; ++i) out[i] = v[i] / 65535.f; } break;
  case D3DDECLTYPE_UDEC3: { uint32_t v; memcpy(&v, p, 4); out[0] = (float) (v & 1023u); out[1] = (float) ((v >> 10) & 1023u); out[2] = (float) ((v >> 20) & 1023u); } break;
  case D3DDECLTYPE_DEC3N: {
    uint32_t v; memcpy(&v, p, 4);
    for (int i = 0; i < 3; ++i) { int32_t x = (int32_t) ((v >> (10 * i)) & 1023u); if (x & 512) x -= 1024; out[i] = (std::max)(x / 511.f, -1.f); }
  } break;
  case D3DDECLTYPE_FLOAT16_2: { uint16_t v[2]; memcpy(v, p, 4); out[0] = half(v[0]); out[1] = half(v[1]); } break;
  case D3DDECLTYPE_FLOAT16_4: { uint16_t v[4]; memcpy(v, p, 8); for (int i = 0; i < 4; ++i) out[i] = half(v[i]); } break;
  default: return false;
  }
  return true;
}

// The input registers the shader declares: v# -> (usage, usage index); count of them.
struct VsInputDcl { uint8_t reg, usage, index; };
inline int vsInputDcls(const DWORD* tok, size_t count, VsInputDcl out[16]) {
  int n = 0;
  dxsoForEach(tok, count, [&](size_t pos, uint32_t op, uint32_t len) {
    uint32_t usage, idx, reg;
    if (op == kDxsoOpDcl && n < 16 && dxsoDcl(tok, pos, len, kDxsoRegInput, usage, idx, reg) && reg < 16) out[n++] = { (uint8_t) reg, (uint8_t) usage, (uint8_t) idx };
    return true;
  });
  return n;
}

// Runs the shader for one vertex; pos receives its position output (clip space).
inline bool evalVsPosition(const DWORD* tok, size_t count, const float in[16][4], const VsConstants& k, float pos[4], VsRun& run) {
  if (!dxsoIsVertexShader(tok, count, 2) || !k.f) { run.failed = "not a vs_2_0 / vs_3_0 shader"; return false; }
  const bool vs3 = dxsoIsVertexShader(tok, count, 3);
  static thread_local float c[256][4];
  memcpy(c, k.f, sizeof c);
  int ic[16][4] = {}; if (k.i) memcpy(ic, k.i, sizeof ic);
  BOOL bc[16] = {}; if (k.b) memcpy(bc, k.b, sizeof bc);
  float r[32][4] = {}, o[16][4] = {}, rast[3][4] = {}, dummy[4] = {};
  int a0[4] = {};
  int posReg = vs3 ? -1 : 0;
  // definitions and the position output's register
  dxsoForEach(tok, count, [&](size_t p, uint32_t op, uint32_t len) {
    if (op == 0x51u && len >= 5) { const uint32_t n = dxsoRegNum(tok[p + 1]); if (n < 256) memcpy(c[n], &tok[p + 2], 16); }
    else if (op == 0x30u && len >= 5) { const uint32_t n = dxsoRegNum(tok[p + 1]); if (n < 16) for (int i = 0; i < 4; ++i) ic[n][i] = (int) tok[p + 2 + i]; }
    else if (op == 0x2Fu && len >= 2) { const uint32_t n = dxsoRegNum(tok[p + 1]); if (n < 16) bc[n] = tok[p + 2] ? TRUE : FALSE; }
    else if (vs3 && op == kDxsoOpDcl) { uint32_t usage, idx, reg; if (dxsoDcl(tok, p, len, kDxsoRegOutput, usage, idx, reg) && usage == kUsagePosition && idx == 0 && reg < 16) posReg = (int) reg; }
    return true;
  });
  if (posReg < 0) { run.failed = "no position output"; return false; }

  // a source operand at tokens[q]; q advances past it (and its relative-address token)
  auto src = [&](size_t& q, float v[4]) -> bool {
    const uint32_t t = tok[q++];
    const uint32_t type = dxsoRegType(t), num = dxsoRegNum(t), swz = (t >> 16) & 0xFFu, mod = (t >> 24) & 0xFu;
    int idx = (int) num;
    if (t & (1u << 13)) {                       // relative addressing: the address token follows
      const uint32_t rt = tok[q++];
      const uint32_t comp = (rt >> 16) & 3u;
      if (dxsoRegType(rt) != 3u) { run.failed = "relative to a loop counter"; return false; }
      idx += a0[comp];
    }
    float raw[4];
    switch (type) {
    case 0: if (idx < 0 || idx >= 32) return false; memcpy(raw, r[idx], 16); break;
    case 1: if (idx < 0 || idx >= 16) return false; memcpy(raw, in[idx], 16); break;
    case 2: case 11: case 12: case 13: {
      const int base = type == 2 ? 0 : type == 11 ? 2048 : type == 12 ? 4096 : 6144;
      const int n = idx + base;
      if (n < 0 || n >= 256) { for (float& x : raw) x = 0.f; } else memcpy(raw, c[n], 16);   // outside the bank reads 0, as the hardware
    } break;
    case 3: for (int i = 0; i < 4; ++i) raw[i] = (float) a0[i]; break;
    case 7: if (idx < 0 || idx >= 16) return false; for (int i = 0; i < 4; ++i) raw[i] = (float) ic[idx][i]; break;
    default: run.failed = "an unsupported source register"; return false;
    }
    for (int i = 0; i < 4; ++i) v[i] = raw[(swz >> (2 * i)) & 3u];
    switch (mod) {
    case 0: break;
    case 1: for (int i = 0; i < 4; ++i) v[i] = -v[i]; break;
    case 2: for (int i = 0; i < 4; ++i) v[i] -= 0.5f; break;
    case 3: for (int i = 0; i < 4; ++i) v[i] = -(v[i] - 0.5f); break;
    case 4: for (int i = 0; i < 4; ++i) v[i] = 2.f * v[i] - 1.f; break;
    case 5: for (int i = 0; i < 4; ++i) v[i] = -(2.f * v[i] - 1.f); break;
    case 6: for (int i = 0; i < 4; ++i) v[i] = 1.f - v[i]; break;
    case 7: for (int i = 0; i < 4; ++i) v[i] *= 2.f; break;
    case 8: for (int i = 0; i < 4; ++i) v[i] *= -2.f; break;
    case 11: for (int i = 0; i < 4; ++i) v[i] = std::fabs(v[i]); break;
    case 12: for (int i = 0; i < 4; ++i) v[i] = -std::fabs(v[i]); break;
    default: run.failed = "an unsupported source modifier"; return false;
    }
    return true;
  };
  // the destination operand at tokens[q]: where to write, its mask, saturate
  struct Dst { float* reg; int* areg; uint32_t mask; bool sat; };
  auto dst = [&](size_t& q, Dst& d) -> bool {
    const uint32_t t = tok[q++];
    if (t & (1u << 13)) { run.failed = "a relative destination"; return false; }
    const uint32_t type = dxsoRegType(t), num = dxsoRegNum(t);
    d.mask = (t >> 16) & 0xFu; d.sat = ((t >> 20) & 1u) != 0; d.reg = nullptr; d.areg = nullptr;
    switch (type) {
    case 0: if (num >= 32) return false; d.reg = r[num]; break;
    case 3: d.areg = a0; break;
    case 4: if (num >= 3) return false; d.reg = rast[num]; break;
    case 5: d.reg = dummy; break;                                     // oD# (vs_2_0)
    case 6: if (num >= 16) return false; d.reg = o[num]; break;       // oT# (vs_2_0) / o# (vs_3_0)
    default: run.failed = "an unsupported destination register"; return false;
    }
    return true;
  };
  auto write = [&](const Dst& d, const float v[4]) {
    for (int i = 0; i < 4; ++i) {
      if (!(d.mask & (1u << i))) continue;
      float x = v[i];
      if (d.sat) x = x < 0.f ? 0.f : x > 1.f ? 1.f : x;
      if (d.areg) d.areg[i] = (int) std::floor(x + 0.5f);   // mova rounds to the nearest (vs_2_0 and up)
      else d.reg[i] = x;
    }
  };

  // flow control: a stack of (taken, any branch taken, enclosing active)
  struct Branch { bool active, outer; };
  Branch stack[16]; int depth = 0;
  bool active = true, ok = true;
  dxsoForEach(tok, count, [&](size_t p, uint32_t op, uint32_t len) {
    const uint32_t itok = tok[p];
    if (itok & (1u << 28)) { run.failed = "a predicated instruction"; ok = false; return false; }
    size_t q = p + 1;
    switch (op) {
    case 0x1Fu: case 0x51u: case 0x30u: case 0x2Fu: case 0u: return true;   // dcl, def, defi, defb, nop
    case 40u: {                                                              // if b#
      const uint32_t t = tok[q];
      const bool cond = dxsoRegType(t) == 14u && dxsoRegNum(t) < 16 && bc[dxsoRegNum(t)];
      if (depth >= 16) { ok = false; run.failed = "nesting"; return false; }
      stack[depth++] = { active && cond, active };
      active = active && cond;
      return true;
    }
    case 41u: {                                                              // if_cmp a, b
      float x[4], y[4];
      if (!src(q, x) || !src(q, y)) { ok = false; return false; }
      bool cond = false;
      switch ((itok >> 16) & 7u) { case 1: cond = x[0] > y[0]; break; case 2: cond = x[0] == y[0]; break; case 3: cond = x[0] >= y[0]; break;
                                   case 4: cond = x[0] < y[0]; break; case 5: cond = x[0] != y[0]; break; case 6: cond = x[0] <= y[0]; break; default: break; }
      if (depth >= 16) { ok = false; run.failed = "nesting"; return false; }
      stack[depth++] = { active && cond, active };
      active = active && cond;
      return true;
    }
    case 42u: if (depth <= 0) { ok = false; return false; } active = stack[depth - 1].outer && !stack[depth - 1].active; stack[depth - 1].active = active; return true;   // else
    case 43u: if (depth <= 0) { ok = false; return false; } active = stack[--depth].outer; return true;                                                            // endif
    default: break;
    }
    if (!active) return true;
    Dst d; float s0[4], s1[4], s2[4], v[4] = {};
    auto scalar = [](const float s[4]) { return s[0]; };   // replicate swizzles: every component the same
    switch (op) {
    case 1u:  if (!dst(q, d) || !src(q, s0)) break; write(d, s0); return true;                                                        // mov
    case 46u: if (!dst(q, d) || !src(q, s0)) break; write(d, s0); return true;                                                        // mova
    case 2u:  if (!dst(q, d) || !src(q, s0) || !src(q, s1)) break; for (int i = 0; i < 4; ++i) v[i] = s0[i] + s1[i]; write(d, v); return true;   // add
    case 3u:  if (!dst(q, d) || !src(q, s0) || !src(q, s1)) break; for (int i = 0; i < 4; ++i) v[i] = s0[i] - s1[i]; write(d, v); return true;   // sub
    case 5u:  if (!dst(q, d) || !src(q, s0) || !src(q, s1)) break; for (int i = 0; i < 4; ++i) v[i] = s0[i] * s1[i]; write(d, v); return true;   // mul
    case 4u:  if (!dst(q, d) || !src(q, s0) || !src(q, s1) || !src(q, s2)) break; for (int i = 0; i < 4; ++i) v[i] = s0[i] * s1[i] + s2[i]; write(d, v); return true;   // mad
    case 18u: if (!dst(q, d) || !src(q, s0) || !src(q, s1) || !src(q, s2)) break; for (int i = 0; i < 4; ++i) v[i] = s0[i] * (s1[i] - s2[i]) + s2[i]; write(d, v); return true;   // lrp
    case 6u:  if (!dst(q, d) || !src(q, s0)) break; { const float x = scalar(s0); for (float& e : v) e = x == 0.f ? INFINITY : 1.f / x; } write(d, v); return true;   // rcp
    case 7u:  if (!dst(q, d) || !src(q, s0)) break; { const float x = std::fabs(scalar(s0)); for (float& e : v) e = x == 0.f ? INFINITY : 1.f / std::sqrt(x); } write(d, v); return true;   // rsq
    case 8u:  if (!dst(q, d) || !src(q, s0) || !src(q, s1)) break; { const float x = s0[0] * s1[0] + s0[1] * s1[1] + s0[2] * s1[2]; for (float& e : v) e = x; } write(d, v); return true;   // dp3
    case 9u:  if (!dst(q, d) || !src(q, s0) || !src(q, s1)) break; { const float x = s0[0] * s1[0] + s0[1] * s1[1] + s0[2] * s1[2] + s0[3] * s1[3]; for (float& e : v) e = x; } write(d, v); return true;   // dp4
    case 10u: if (!dst(q, d) || !src(q, s0) || !src(q, s1)) break; for (int i = 0; i < 4; ++i) v[i] = (std::min)(s0[i], s1[i]); write(d, v); return true;   // min
    case 11u: if (!dst(q, d) || !src(q, s0) || !src(q, s1)) break; for (int i = 0; i < 4; ++i) v[i] = (std::max)(s0[i], s1[i]); write(d, v); return true;   // max
    case 12u: if (!dst(q, d) || !src(q, s0) || !src(q, s1)) break; for (int i = 0; i < 4; ++i) v[i] = s0[i] < s1[i] ? 1.f : 0.f; write(d, v); return true;   // slt
    case 13u: if (!dst(q, d) || !src(q, s0) || !src(q, s1)) break; for (int i = 0; i < 4; ++i) v[i] = s0[i] >= s1[i] ? 1.f : 0.f; write(d, v); return true;   // sge
    case 14u: if (!dst(q, d) || !src(q, s0)) break; { const float x = std::exp2(scalar(s0)); for (float& e : v) e = x; } write(d, v); return true;   // exp
    case 15u: if (!dst(q, d) || !src(q, s0)) break; { const float a = std::fabs(scalar(s0)); const float x = a == 0.f ? -INFINITY : std::log2(a); for (float& e : v) e = x; } write(d, v); return true;   // log
    case 16u: if (!dst(q, d) || !src(q, s0)) break;                                                                                // lit
      v[0] = 1.f; v[1] = (std::max)(s0[0], 0.f);
      v[2] = s0[0] > 0.f ? std::pow((std::max)(s0[1], 0.f), (std::max)(-128.f, (std::min)(128.f, s0[3]))) : 0.f; v[3] = 1.f;
      write(d, v); return true;
    case 19u: if (!dst(q, d) || !src(q, s0)) break; for (int i = 0; i < 4; ++i) v[i] = s0[i] - std::floor(s0[i]); write(d, v); return true;   // frc
    case 32u: if (!dst(q, d) || !src(q, s0) || !src(q, s1)) break; { const float x = std::pow(std::fabs(scalar(s0)), scalar(s1)); for (float& e : v) e = x; } write(d, v); return true;   // pow
    case 33u: if (!dst(q, d) || !src(q, s0) || !src(q, s1)) break;                                                                 // crs
      v[0] = s0[1] * s1[2] - s0[2] * s1[1]; v[1] = s0[2] * s1[0] - s0[0] * s1[2]; v[2] = s0[0] * s1[1] - s0[1] * s1[0]; write(d, v); return true;
    case 35u: if (!dst(q, d) || !src(q, s0)) break; for (int i = 0; i < 4; ++i) v[i] = std::fabs(s0[i]); write(d, v); return true;   // abs
    case 36u: if (!dst(q, d) || !src(q, s0)) break; {                                                                               // nrm
      const float l = std::sqrt(s0[0] * s0[0] + s0[1] * s0[1] + s0[2] * s0[2]);
      for (int i = 0; i < 4; ++i) v[i] = l == 0.f ? INFINITY : s0[i] / l;
    } write(d, v); return true;
    case 37u: if (!dst(q, d) || !src(q, s0)) break; v[0] = std::cos(scalar(s0)); v[1] = std::sin(scalar(s0)); write(d, v); return true;   // sincos (vs_2_0's two macro constants unread)
    case 20u: case 21u: case 22u: case 23u: case 24u: {                                                                             // m4x4, m4x3, m3x4, m3x3, m3x2
      const uint32_t rowsN = op == 20u ? 4 : op == 21u ? 3 : op == 22u ? 4 : op == 23u ? 3 : 2;
      const bool four = op == 20u || op == 21u;
      if (!dst(q, d) || !src(q, s0)) break;
      const uint32_t mt = tok[q];                       // the matrix: consecutive registers, one per row
      if (mt & (1u << 13)) { run.failed = "a relative matrix"; break; }
      const uint32_t mtype = dxsoRegType(mt), base = dxsoRegNum(mt);
      for (uint32_t row = 0; row < rowsN; ++row) {
        float m[4];
        if (mtype == 2u && base + row < 256) memcpy(m, c[base + row], 16);
        else if (mtype == 0u && base + row < 32) memcpy(m, r[base + row], 16);
        else { run.failed = "a matrix register"; ok = false; return false; }
        v[row] = s0[0] * m[0] + s0[1] * m[1] + s0[2] * m[2] + (four ? s0[3] * m[3] : 0.f);
      }
      write(d, v); return true;
    }
    default: run.failed = "an unsupported instruction"; ok = false; return false;
    }
    if (!run.failed) run.failed = "an operand";
    ok = false; return false;
  });
  if (!ok) return false;
  memcpy(pos, vs3 ? o[posReg] : rast[0], 16);
  return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]) && std::isfinite(pos[3]);
}

// A row-vector 4x4 inverse in double precision (Cramer); false when singular.
inline bool invert4d(const D3DMATRIX& A, double out[16]) {
  double m[16]; for (int i = 0; i < 16; ++i) m[i] = (&A.m[0][0])[i];
  double inv[16];
  inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
  inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
  inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
  inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
  inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
  inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
  inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
  inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
  inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
  inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
  inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
  inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
  inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
  inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
  inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
  inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
  const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (!(std::fabs(det) > 1e-30)) return false;
  for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
  return true;
}
// A clip position back to the world, as the runtime's vertex capture does it (dxso_compiler.cpp):
// clip x inverse(projection) -> view, divided by w; x inverse(view) -> world -- in double precision,
// and with a perspective projection the clip depth is rebuilt from w (z = w P22 / P23 + P32): far
// from the camera the shader's float clip z rounds coarsely (0.2 units off at 100 in a test), and a
// position that wavers with the camera would read as motion.
inline bool clipToWorld(const D3DMATRIX& view, const D3DMATRIX& proj, const float clip[4], float world[3]) {
  double ip[16], iv[16];
  if (!invert4d(proj, ip) || !invert4d(view, iv)) return false;
  double cl[4] = { clip[0], clip[1], clip[2], clip[3] };
  if (proj.m[3][3] == 0.f && proj.m[0][2] == 0.f && proj.m[1][2] == 0.f && std::fabs(proj.m[2][3]) > 1e-6f)
    cl[2] = cl[3] / proj.m[2][3] * proj.m[2][2] + proj.m[3][2];
  double vh[4];
  for (int c = 0; c < 4; ++c) vh[c] = cl[0] * ip[0 * 4 + c] + cl[1] * ip[1 * 4 + c] + cl[2] * ip[2 * 4 + c] + cl[3] * ip[3 * 4 + c];
  if (!(std::fabs(vh[3]) > 1e-20)) return false;
  const double v4[4] = { vh[0] / vh[3], vh[1] / vh[3], vh[2] / vh[3], 1.0 };
  for (int c = 0; c < 3; ++c) world[c] = (float) (v4[0] * iv[0 * 4 + c] + v4[1] * iv[1 * 4 + c] + v4[2] * iv[2 * 4 + c] + v4[3] * iv[3 * 4 + c]);
  return std::isfinite(world[0]) && std::isfinite(world[1]) && std::isfinite(world[2]);
}

}  // namespace sims3cam

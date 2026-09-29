#pragma once
/*
 * The Sims 3 camera hook for the RTX Remix bridge client (milestone 1b).
 *
 * The Sims 3 never calls IDirect3DDevice9::SetTransform. It uploads a fused
 * World*View*Projection matrix to vertex-shader constants c0..c3 (column-vector
 * convention, one register per matrix row), usually the object's World as a 3x4 at
 * c4..c6, and the camera's world-space eye position as a float4 somewhere in the same
 * block. DXVK-Remix derives its camera solely from D3DTS_VIEW / D3DTS_PROJECTION and
 * treats an identity projection as "no camera", so stock Remix captures nothing.
 *
 * This hook recovers View and Projection and forwards them via SetTransform; Remix's
 * vertex capture then places geometry from the vertex-shader output through
 * inverse(View*Projection), so no per-draw World is needed.
 *
 * Disambiguation (learned from run 1): a projection composed with any rigid World is
 * still a perfectly valid-looking camera, just in object space. The only way to tell
 * View*Projection from View*Projection*World is a world-space anchor, and the game
 * provides one: the eye position it uploads for specular lighting. A candidate is
 * accepted only if the camera position it implies equals a float4 in the same upload.
 *
 * Classification of a c0 upload:
 *   Main        - verified camera with a proper basis (det +1): the play camera, whichever
 *                 way it pitches                                  -> forward View/Projection
 *   OtherCamera - verified camera with a mirrored basis (det -1): a reflection pass (the
 *                 sea/pool pass, a wall mirror's stencil pass)    -> its draws are dropped
 *   None        - anything else (object-space fusions, unknown layouts, the UI's pixel-to-
 *                 clip scale) -> leave as is: those draws are still rendered by the main
 *                 camera and un-project correctly, or fail the per-draw 3D tests
 *
 * The ray tracer renders reflections itself, so nothing of a reflection pass is sent on: its
 * 3D draws are dropped on the client (recognised by the mirrored camera, or by the stencil
 * mirror's render states for draws whose own constants never reach the classifier). Any
 * other draw that is not captured keeps the runtime's default transforms (or the game's own,
 * if it ever set any), i.e. plain rasterization, so it looks exactly as it did without the hook.
 *
 * Toggle with the SIMS3_CAMERA_HOOK environment variable (unset/1 = on; 0, f or n = off).
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <d3d9.h>

namespace sims3cam {

struct M4 { float m[4][4]; };  // m[row][col], column-vector convention (M * v)

enum class Kind { None, Main, OtherCamera };

inline bool enabled() {
  static int s = -1;
  if (s < 0) {
    char e[8] = {};
    const DWORD n = GetEnvironmentVariableA("SIMS3_CAMERA_HOOK", e, sizeof e);
    s = (n > 0 && (e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' || e[0] == 'N')) ? 0 : 1;
  }
  return s == 1;
}

inline float len3(const float* v) { return std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); }
inline float dot3(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

// Inverse of an affine 3x4 [A|t] (rows w[0..2] = {a0,a1,a2,t}) as a 4x4. Handles scale.
inline bool invAffine(const float w[3][4], M4& out) {
  const float a = w[0][0], b = w[0][1], c = w[0][2];
  const float d = w[1][0], e = w[1][1], f = w[1][2];
  const float g = w[2][0], h = w[2][1], i = w[2][2];
  const float det = a*(e*i - f*h) - b*(d*i - f*g) + c*(d*h - e*g);
  if (std::fabs(det) < 1e-12f) return false;
  const float id = 1.f / det;
  const float r[3][3] = {
    { (e*i - f*h)*id, (c*h - b*i)*id, (b*f - c*e)*id },
    { (f*g - d*i)*id, (a*i - c*g)*id, (c*d - a*f)*id },
    { (d*h - e*g)*id, (b*g - a*h)*id, (a*e - b*d)*id } };
  for (int rr = 0; rr < 3; ++rr) {
    for (int cc = 0; cc < 3; ++cc) out.m[rr][cc] = r[rr][cc];
    out.m[rr][3] = -(r[rr][0]*w[0][3] + r[rr][1]*w[1][3] + r[rr][2]*w[2][3]);
  }
  out.m[3][0] = out.m[3][1] = out.m[3][2] = 0.f; out.m[3][3] = 1.f;
  return true;
}

inline void mul(const M4& A, const M4& B, M4& out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += A.m[r][k] * B.m[k][c];
      out.m[r][c] = s;
    }
}

struct Camera {
  D3DMATRIX view = {};      // D3D row-vector convention (transpose of the column-vector V)
  D3DMATRIX proj = {};      // D3D row-vector perspective; depth terms verbatim from the game
  float fovY = 0.f, aspect = 0.f, nearZ = 0.f;
  float pos[3] = {};        // camera position in the space the matrix maps from
  float fwd[3] = {};        // direction of increasing clip w, i.e. what the camera looks along
  bool mirrored = false;    // the basis is improper (det -1): the play camera reflected across a plane, i.e. a reflection pass
};

// Decompose a column-vector ViewProjection into View and Projection and validate it as a
// plausible perspective camera. Math validated against captured game data (vp_decompose.py,
// test_sims3cam.cpp): VP row 3 = -V row 2; row 2 = a*V row 2 + [0 0 0 b]; fx,fy = row norms.
inline bool decompose(const M4& VP, Camera& cam) {
  float V2[4] = { -VP.m[3][0], -VP.m[3][1], -VP.m[3][2], -VP.m[3][3] };
  const float n2 = len3(V2);
  if (!(n2 > 0.98f && n2 < 1.02f)) return false;                 // perspective row must be a unit direction
  int k = 0;                                                      // the largest component (>= 0.57 for a unit row)
  for (int i = 1; i < 3; ++i) if (std::fabs(V2[i]) > std::fabs(V2[k])) k = i;
  const float a = VP.m[2][k] / V2[k];                            // P[2][2]
  for (int i = 0; i < 3; ++i) {                                   // row 2 direction must be a * V2 direction
    const float mag = std::fabs(VP.m[2][i]);
    if (std::fabs(VP.m[2][i] - a*V2[i]) > 2e-3f * (mag > 1.f ? mag : 1.f)) return false;
  }
  const float b = VP.m[2][3] - a*V2[3];                          // P[2][3]
  if (!(a < -0.5f && a > -1.5f)) return false;                   // a = far/(near-far) ~ -1
  const float nearZ = b / a;
  if (!(nearZ > 1e-3f && nearZ < 1e4f)) return false;
  const float fx = len3(VP.m[0]), fy = len3(VP.m[1]);
  if (!(fx > 0.1f && fy > 0.1f)) return false;
  float V0[4], V1[4];
  for (int i = 0; i < 4; ++i) { V0[i] = VP.m[0][i] / fx; V1[i] = VP.m[1][i] / fy; }
  if (std::fabs(dot3(V0, V1)) > 0.02f || std::fabs(dot3(V0, V2)) > 0.02f || std::fabs(dot3(V1, V2)) > 0.02f) return false;
  // |det| must be 1 for an orthonormal basis (V0 and V1 are unit by construction). With the handedness convention assumed for the
  // perspective row and fy, the play camera's basis is a proper rotation (det +1, checked by
  // test_sims3cam.cpp); a camera reflected across a plane -- the sea/pool reflection pass, or a
  // wall mirror's stencil reflection pass at close zoom (run 66) -- comes out improper (det -1).
  const float det = V0[0]*(V1[1]*V2[2] - V1[2]*V2[1]) - V0[1]*(V1[0]*V2[2] - V1[2]*V2[0]) + V0[2]*(V1[0]*V2[1] - V1[1]*V2[0]);
  if (std::fabs(std::fabs(det) - 1.f) > 0.05f) return false;
  cam.mirrored = det < 0.f;
  const float aspect = fy / fx;                                    // width/height for square pixels
  const float fovY = 2.f * std::atan(1.f / fy);
  if (!(aspect > 1.2f && aspect < 2.5f)) return false;            // square 90-degree shadow/cube cameras
  if (!(fovY > 0.20f && fovY < 1.30f)) return false;              // ~11..75 degrees

  // Handedness: the play camera is right-handed (it looks along -Z of this basis, and the
  // basis is a proper rotation, det +1 -- checked by test_sims3cam.cpp). The sea-reflection
  // camera is its planar mirror and therefore improper; it is never forwarded. (A left-handed
  // pair -- view Z = +row 3, P34 = +1 -- reproduces the same VP but is the mirror of this
  // camera; tried after run 6 and rejected by the determinant test.)
  D3DMATRIX& v = cam.view; std::memset(&v, 0, sizeof v);
  v._11 = V0[0]; v._12 = V1[0]; v._13 = V2[0];
  v._21 = V0[1]; v._22 = V1[1]; v._23 = V2[1];
  v._31 = V0[2]; v._32 = V1[2]; v._33 = V2[2];
  v._41 = V0[3]; v._42 = V1[3]; v._43 = V2[3]; v._44 = 1.f;

  // Depth terms verbatim: inverse(View*Projection) must be the exact inverse of what the
  // vertex shader applied, because vertex capture un-projects the captured clip output.
  D3DMATRIX& p = cam.proj; std::memset(&p, 0, sizeof p);
  p._11 = fx; p._22 = fy; p._33 = a; p._34 = -1.f; p._43 = b;

  cam.fovY = fovY; cam.aspect = aspect; cam.nearZ = nearZ;
  for (int i = 0; i < 3; ++i) {
    cam.pos[i] = -(V0[i]*V0[3] + V1[i]*V1[3] + V2[i]*V2[3]);      // -(R^T t), convention independent
    cam.fwd[i] = -V2[i];                                           // = VP row 3 direction
  }
  return true;
}

// The game uploads the camera's own world position as a float4 (x, y, z, 1) in the same block.
// A candidate near the world origin is not a camera: its "eye" would be matched by any small
// float4 in the block -- a light direction, a colour (run 70: a once-per-frame block of
// VS 41a25ab3, with the play camera's lens but the view's translation stripped, matched a
// direction at the origin and was adopted as the main camera whenever the far view was in
// frame, i.e. at a horizon tilt).
inline bool eyePresent(const float* c, unsigned count, const float pos[3]) {
  if (len3(pos) < 2.f) return false;
  for (unsigned r = 4; r < count; ++r) {
    const float* v = c + r*4;
    if (std::fabs(v[3] - 1.f) > 0.01f) continue;                      // a position, not a direction or a colour
    if (std::fabs(v[0] - pos[0]) < 0.5f && std::fabs(v[1] - pos[1]) < 0.5f && std::fabs(v[2] - pos[2]) < 0.5f) return true;
  }
  return false;
}

// Two matrices within a relative tolerance (the same camera re-derived from another object's
// fused matrix differs in the last bits; re-sending it every draw only makes the runtime's
// camera jitter).
inline bool similarMatrix(const D3DMATRIX& a, const D3DMATRIX& b, float eps) {
  const float* pa = &a._11; const float* pb = &b._11;
  for (int i = 0; i < 16; ++i) if (std::fabs(pa[i] - pb[i]) > eps * (1.f + std::fabs(pb[i]))) return false;
  return true;
}

// A verified camera is the play camera unless it is a reflection: every reflection pass --
// the sea/pool pass rendered first each frame (the camera mirrored across the water plane,
// so it looks up), a wall mirror's stencil pass at close zoom (mirrored across a vertical
// plane, so it still looks down) -- has an improper basis (det -1). Nothing else tells them
// apart: the play camera pitches from a few degrees below the horizon to top-down, and at the
// horizon the earlier orientation test (forward.y < -0.05, runs 2-67) rejected it. The ray
// tracer renders reflections itself; the reflection passes' draws are dropped (sims3ApplyForDraw).
inline Kind kindOfVerified(const Camera& cam) { return cam.mirrored ? Kind::OtherCamera : Kind::Main; }

// c = constant floats beginning at register 0; count = number of float4 registers.
inline Kind classify(const float* c, unsigned count, Camera& cam) {
  if (count < 4) return Kind::None;
  M4 WVP;
  for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) WVP.m[r][col] = c[r*4 + col];
  if (count >= 7) {                                                // candidate A: World at c4..c6, VP = WVP * W^-1
    float w[3][4];
    for (int r = 0; r < 3; ++r) for (int col = 0; col < 4; ++col) w[r][col] = c[16 + r*4 + col];
    M4 Winv, VP;
    if (invAffine(w, Winv)) {
      mul(WVP, Winv, VP);
      if (decompose(VP, cam) && eyePresent(c, count, cam.pos)) return kindOfVerified(cam);
    }
  }
  if (decompose(WVP, cam) && eyePresent(c, count, cam.pos)) return kindOfVerified(cam);   // candidate B: identity World
  return Kind::None;
}

inline bool sameMatrix(const D3DMATRIX& a, const D3DMATRIX& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// What the runtime currently holds. Kind::None here means the transforms the runtime had
// before the hook: the game's own, if it ever set any, else identity.
struct Held { Kind kind = Kind::None; D3DMATRIX view = {}; D3DMATRIX proj = {}; };

// ---- draw-time decision (milestone 1d) ------------------------------------------------
// Run 3 showed that a correct camera is not enough: every draw that never uploads a matrix
// (post-processing full-screen quads, the bloom composite, UI batches) inherits the main
// camera and is un-projected into geometry right in front of it. In the traced frame the 3D
// pass is cleanly separated from those draws by two signals the client already tracks:
// depth testing (on for the whole 3D pass, off from the bloom chain to the end of frame) and
// the vertex position layout (three components for meshes, two for screen-space quads).
// A draw gets the main camera only if both say "3D"; otherwise identity (plain raster).

// Does this vertex declaration carry a 3-component (or wider) POSITION 0?
inline bool positionIs3D(const D3DVERTEXELEMENT9* e) {
  if (!e) return false;
  for (; e->Stream != 0xFF; ++e) {
    if (e->Usage == D3DDECLUSAGE_POSITIONT) return false;
    if (e->Usage != D3DDECLUSAGE_POSITION || e->UsageIndex != 0) continue;
    // The Sims 3 packs nearly every mesh position as SHORT4 (176 of 209 layouts in the
    // trace) and some as four bytes (D3DCOLOR / UBYTE4: compressed grid positions for
    // terrain and instanced foliage). A 4-byte position is never a screen-space quad.
    //
    // FLOAT4 counts as 3D: the game's one FLOAT4 layout carries w = 0 in the buffer, but
    // its shader rebuilds (x, y, z, 1) before projecting, so it produces proper points.
    // (What actually produces infinities is a shader pattern, not a layout: see
    // shaderCollapsesPosition below.)
    switch (e->Type) {
      case D3DDECLTYPE_FLOAT3: case D3DDECLTYPE_FLOAT4:
      case D3DDECLTYPE_SHORT4: case D3DDECLTYPE_SHORT4N: case D3DDECLTYPE_USHORT4N:
      case D3DDECLTYPE_UDEC3: case D3DDECLTYPE_DEC3N: case D3DDECLTYPE_FLOAT16_4:
      case D3DDECLTYPE_D3DCOLOR: case D3DDECLTYPE_UBYTE4: case D3DDECLTYPE_UBYTE4N:
        return true;
      default:
        return false;       // FLOAT1/FLOAT2/SHORT2: screen-space or procedural
    }
  }
  return false;             // no POSITION element at all
}

inline bool fvfIs3D(DWORD fvf) {
  const DWORD pos = fvf & D3DFVF_POSITION_MASK;
  return pos != 0 && pos != D3DFVF_XYZRHW;
}

// The decision itself, kept free of device types so it can be unit-tested. rtIsPrimary:
// render target 0 is the backbuffer-sized target, i.e. the 3D pass (the shadow and
// reflection passes render into small textures and must not be touched).
// The stencil-mirror pass (a wall mirror at close zoom, run 66): the scene is drawn again,
// reflected across the mirror's plane, into the backbuffer -- masked by the stencil, with the
// winding flipped (CCW) -- and the camera it uploads is caught by kindOfVerified. Draws whose
// own constants never reach the classifier (the terrain: shadow rows at c0) inherit whatever
// camera state came before them, so the pass is also recognised from its render states.
inline bool isMirrorPass(DWORD stencilEnable, DWORD cullMode) { return stencilEnable != 0 && cullMode == D3DCULL_CCW; }

// A 3D draw of a reflection pass (the ray tracer renders reflections itself): the last camera
// upload was a mirrored one, or the draw carries the stencil mirror's render states.
inline bool isReflectionDraw(bool cameraMirrored, bool declIs3D, DWORD zEnable, DWORD stencilEnable, DWORD cullMode) {
  return declIs3D && zEnable != D3DZB_FALSE && (cameraMirrored || isMirrorPass(stencilEnable, cullMode));
}

// A 3D draw of the main pass: a verified camera, a 3D position layout, depth testing on, the
// primary render target.
inline bool drawIs3D(bool cameraValid, bool declIs3D, DWORD zEnable, bool rtIsPrimary) {
  return cameraValid && declIs3D && zEnable != D3DZB_FALSE && rtIsPrimary;
}
inline bool drawWantsCamera(bool cameraValid, bool declIs3D, DWORD zEnable, bool rtIsPrimary, DWORD stencilEnable = 0, DWORD cullMode = D3DCULL_CW) {
  return drawIs3D(cameraValid, declIs3D, zEnable, rtIsPrimary) && !isMirrorPass(stencilEnable, cullMode);
}

// ---- draw-time texture remap (milestone 1g) -------------------------------------------
// Remix takes texture stage 0 as the material's albedo and drops any draw whose stage-0
// texture has no content hash. The Sims 3 binds an environment cube map at sampler 0 for
// most materials (walls, floors, objects, Sims) and the diffuse at sampler 1, so all of
// those draws were dropped (56 instances out of ~300 draws in run 8). For captured draws
// the client presents the first colour 2D texture as stage 0 for the duration of the draw
// and restores the binding afterwards. The game's own rasterization of that draw then
// samples the wrong texture, but Remix's image replaces it.
inline bool isColorFormat(D3DFORMAT f) {
  switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: case D3DFMT_A16B16G16R16F:
      return true;
    default:
      return false;   // L8, A8, V8U8, L16, depth formats, ...
  }
}

// The stage whose texture should be presented as stage 0 for a captured draw, or -1 when
// stage 0 is already usable (or unbound) or no colour 2D texture is bound at all.
// bound[k]: a texture is bound at stage k; color2D[k]: it is a 2D colour texture that is
// not a render target.
inline int pickAlbedoStage(const bool bound[16], const bool color2D[16]) {
  if (!bound[0] || color2D[0]) return -1;
  for (int k = 1; k < 16; ++k) if (bound[k] && color2D[k]) return k;
  return -1;
}

// One lookup for every hash-keyed table below.
template<typename T, size_t N>
inline const T* findByHash(const T (&table)[N], uint64_t hash) {
  if (hash == 0) return nullptr;
  for (const T& e : table) if (e.hash == hash) return &e;
  return nullptr;
}

// ---- per-shader constant patches (milestone 1f) ----------------------------------------
// The engine hides geometry inside its vertex shaders. Wall shaders multiply the projected
// x,y,z by a per-vertex visibility factor floored by a constant (the cut-away view); the lot
// terrain shader moves vertices outside a rectangle to a far clip position. The game's
// rasterizer draws nothing for such vertices, but Remix's vertex capture reconstructs them
// onto the focal axis at the near plane: triangles from real vertices to that point are the
// fans converging on the screen centre, and the giant sheets, seen in runs 1-5.
// Patching the constants makes every vertex a normal point: walls are always visible (the
// walls-down lowering is a separate lerp and still works) and terrain chunks are complete.
// Shaders are recognised by an FNV-1a-64 hash of their token stream; the hashes below were
// taken from the game's own shaders in the trace (shaders/hashes.txt).
struct ConstPatch { uint16_t reg; uint8_t comp; float value; };
struct ShaderPatch { uint64_t hash; const char* name; ConstPatch patches[4]; uint32_t count; };

// The lot terrain's vertex shader (its draws are also de-duplicated per frame, see LotCopies).
inline constexpr uint64_t kLotTerrainVs = 0x976b73dbd59842cdull;

inline const ShaderPatch kShaderPatches[] = {
  { kLotTerrainVs, "lot terrain: kill rectangle c7 = (0,0,0.5,0.5)", { { 7, 0, 0.f }, { 7, 1, 0.f }, { 7, 2, 0.5f }, { 7, 3, 0.5f } }, 4 },
};

// ---- vertex shader bytecode patch: promote the diffuse texcoord to TEXCOORD0 (milestone 2)
// Remix captures the texture coordinate from the vertex shader output declared TEXCOORD0.
// The Sims 3 shaders put other things there (cube-map direction, lighting coordinates) and
// emit the diffuse UV on another texcoord, so every remapped material sampled its texture
// with a near-constant coordinate and rendered as flat grey (run 9). Per shader, the pixel
// shader in the trace says which texcoord feeds the albedo sampler; the table below
// promotes it. The edit is in place on a copy of the token stream:
//   vs_3_0: swap the usage-index fields of the two `dcl_texcoord` output declarations
//           (TEXCOORD0 <-> TEXCOORDk), so the register carrying the UV becomes TEXCOORD0;
//   vs_2_x: renumber every oT0 <-> oTk parameter token (oT registers are their semantic).
// The game's own rasterization of those draws then feeds its pixel shader the wrong
// interpolants; the ray-traced image replaces it.
struct TexcoordPromote { uint64_t hash; const char* name; uint8_t texcoordIndex; };

// Built from the trace: for each (vertex shader, pixel shader) pair in the lot's main
// pass, the texcoord the pixel shader reads with the albedo sampler (stage 1 when stage 0
// is a cube map, else stage 0). Shaders whose UV already leaves on TEXCOORD0 (the main
// object shader, terrain, walls C) need nothing; three shaders keep the UV in .zw of
// TEXCOORD0 and are not handled yet. Names are the shader handles in the trace.
// The texcoord to promote is the one the pixel shader reads with the ALBEDO sampler
// (kAlbedoStages), not merely with the first colour texture: five earlier rules that
// pointed at normal or lighting maps (floor, walls B, two door/window families, 0xddc1880)
// were removed after reading the pixel shaders' arithmetic.
inline const TexcoordPromote kTexcoordPromotes[] = {
  { 0x0ba6ddb9aa01913cull, "objects 0x12d25080 vs_3_0 (diffuse s3 on TEXCOORD2)", 2 },
  { 0xc3af2a4a82d84e6eull, "objects 0x12d213c0 vs_3_0 (diffuse s3 on TEXCOORD2)", 2 },
  { 0x7d1bc3ce6acbd715ull, "objects 0x12d17440 vs_3_0 (diffuse s2 on TEXCOORD2)", 2 },
  // 0x10a51180 (floors): the pattern is on TEXCOORD0, no promotion (see kAlbedoStages)
  { 0x3c837e49bf748d99ull, "0x164c8ea0 vs_3_0 (diffuse s2 on TEXCOORD1)", 1 },
  // in-game variants (run-15 shader dump)
  { 0x1bd4405f8346ded4ull, "objects, skinned (diffuse s3 on TEXCOORD2)", 2 },
  { 0x4c1d851f37e3c3f2ull, "objects, skinned, 0x10a68220 family (diffuse s2 on TEXCOORD2)", 2 },
  { 0x4be4f1463f801b19ull, "0xd9c0fe0 family sibling (diffuse s1 on TEXCOORD2)", 2 },
  { 0x24ef09fb3303a9d0ull, "outer ground / water sibling (pattern tile s2 on TEXCOORD2)", 2 },
  { 0xe228d963a38f3e41ull, "vs_2_0 e228d963 (diffuse s1 on TEXCOORD2)", 2 },
  { 0x23072b72226654bdull, "0x164cc3c0 vs_3_0", 5 },
  { 0x9f227c82c758a989ull, "0x164d8920 vs_2_0", 1 },
  { 0x294ca59dd766bd6eull, "0x16b30160 vs_2_0", 1 },
  { 0xe0c97675a334022eull, "0x16b38500 vs_2_0", 1 },
  // 0x16b56360 (floor tiles): the colour texture is on TEXCOORD0, no promotion
  { 0x64154031c30a8800ull, "0xd9c0fe0 vs_3_0", 2 },
};

inline const TexcoordPromote* findTexcoordPromote(uint64_t hash) { return findByHash(kTexcoordPromotes, hash); }

// DXSO token helpers (D3D9 shader bytecode).
inline uint32_t dxsoRegType(uint32_t tok) { return ((tok >> 28) & 0x7u) | (((tok >> 11) & 0x3u) << 3); }
inline uint32_t dxsoRegNum(uint32_t tok) { return tok & 0x7FFu; }
inline uint32_t dxsoSetRegNum(uint32_t tok, uint32_t n) { return (tok & ~0x7FFu) | (n & 0x7FFu); }
inline constexpr uint32_t kDxsoEnd = 0x0000FFFFu;
// The definitions carry literal payloads, never register tokens: DEF (0x51), DEFB (0x2F), DEFI (0x30).
inline bool dxsoIsDef(uint32_t op) { return op == 0x51u || op == 0x2Fu || op == 0x30u; }

// Number of DWORD tokens from the version token through the END token, inclusive; 0 when no
// END token lies within `maxTokens` (a truncated or foreign stream).
inline size_t shaderTokenCount(const DWORD* tokens, size_t maxTokens) {
  if (!tokens) return 0;
  for (size_t n = 0; n < maxTokens; ++n) if (tokens[n] == kDxsoEnd) return n + 1;
  return 0;
}
inline size_t shaderTokenCount(const DWORD* tokens) { return shaderTokenCount(tokens, 65536); }

// A vertex shader of at least the given major version?
inline bool dxsoIsVertexShader(const DWORD* tokens, size_t count, unsigned minMajor) {
  if (!tokens || count < 2) return false;
  const uint32_t version = tokens[0];
  return (version & 0xFFFF0000u) == 0xFFFE0000u && ((version >> 8) & 0xFFu) >= minMajor;
}

// Walks the instructions after the version token -- comment blocks skipped, END stops --
// calling fn(pos, opcode, parameterTokens) for each; fn returning false stops the walk.
template<typename Fn>
inline void dxsoForEach(const DWORD* tokens, size_t count, Fn&& fn) {
  size_t pos = 1;
  while (pos < count) {
    const uint32_t t = tokens[pos];
    if (t == 0x0000FFFFu) break;
    const uint32_t op = t & 0xFFFFu;
    if (op == 0xFFFEu) { pos += 1 + ((t >> 16) & 0x7FFFu); continue; }
    const uint32_t len = (t >> 24) & 0xFu;
    if (pos + len >= count) break;
    if (!fn(pos, op, len)) break;
    pos += 1 + len;
  }
}

// A DCL instruction's usage, usage index and register number (dest register of the given type).
inline bool dxsoDcl(const DWORD* tokens, size_t pos, uint32_t len, uint32_t regType, uint32_t& usage, uint32_t& index, uint32_t& reg) {
  if (len < 2 || dxsoRegType(tokens[pos + 2]) != regType) return false;
  usage = tokens[pos + 1] & 0x1Fu; index = (tokens[pos + 1] >> 16) & 0xFu; reg = dxsoRegNum(tokens[pos + 2]);
  return true;
}
inline constexpr uint32_t kDxsoOpDcl = 0x1Fu, kDxsoRegTemp = 0u, kDxsoRegInput = 1u, kDxsoRegOutput = 6u;   // OUTPUT (vs_3_0) shares the code of TEXCRDOUT (vs_2_x)
inline constexpr uint32_t kUsagePosition = 0u, kUsageBlendWeight = 1u, kUsageBlendIndices = 2u, kUsageNormal = 3u, kUsagePointSize = 4u, kUsageTexcoord = 5u, kUsageColor = 10u, kUsageFog = 11u;
// An output register token o<reg> with the given write mask, and a DCL of it with the given semantic.
inline DWORD dxsoOutputToken(uint32_t reg, uint32_t mask) { return 0x80000000u | ((kDxsoRegOutput & 7u) << 28) | (((kDxsoRegOutput >> 3) & 3u) << 11) | ((mask & 0xFu) << 16) | (reg & 0x7FFu); }
inline void dxsoPushDcl(std::vector<DWORD>& r, uint32_t usage, uint32_t index, uint32_t reg, uint32_t mask) {
  r.push_back(0x0200001Fu);                                     // DCL, two parameter tokens
  r.push_back(0x80000000u | (usage & 0x1Fu) | ((index & 0xFu) << 16));
  r.push_back(dxsoOutputToken(reg, mask));
}

// Rewrites a vertex shader token stream in place so that texcoord `K` becomes TEXCOORD0.
// Returns the number of tokens changed (0 = nothing matched, stream left untouched).
inline uint32_t promoteTexcoord(DWORD* tokens, size_t count, uint8_t K) {
  if (!dxsoIsVertexShader(tokens, count, 2) || K == 0) return 0;         // vs_1_x tokens have no length field: not walkable
  const bool vs3 = dxsoIsVertexShader(tokens, count, 3);
  DWORD* usage0 = nullptr;
  DWORD* usageK = nullptr;
  uint32_t changed = 0;
  dxsoForEach(tokens, count, [&](size_t pos, uint32_t op, uint32_t len) {
    uint32_t usage, idx, reg;
    if (vs3) {
      if (op == kDxsoOpDcl && dxsoDcl(tokens, pos, len, kDxsoRegOutput, usage, idx, reg) && usage == kUsageTexcoord) {
        if (idx == 0) usage0 = &tokens[pos + 1];
        else if (idx == K) usageK = &tokens[pos + 1];
      }
    } else if (!dxsoIsDef(op)) {                                          // vs_2_x: renumber the oT# parameter tokens
      for (size_t i = 1; i <= len; ++i) {
        DWORD& p = tokens[pos + i];
        if ((p & 0x80000000u) && dxsoRegType(p) == kDxsoRegOutput /*oT#*/) {
          const uint32_t n = dxsoRegNum(p);
          if (n == 0) { p = dxsoSetRegNum(p, K); ++changed; }
          else if (n == K) { p = dxsoSetRegNum(p, 0); ++changed; }
        }
      }
    }
    return true;
  });
  if (vs3) {
    if (usage0 && usageK) {
      *usage0 = (*usage0 & ~0xF0000u) | ((uint32_t) K << 16);
      *usageK = (*usageK & ~0xF0000u);
      changed = 2;
    } else if (!usage0 && usageK) {                                       // no TEXCOORD0 declared: relabel K
      *usageK = (*usageK & ~0xF0000u);
      changed = 1;
    }
  }
  return changed;
}

// ---- the world-space normal from the vertex shader (milestone 11) -------------------------
// The runtime shades with the vertex shader's NORMAL-semantic output when one exists (its
// vertex capture copies that register and multiplies it by the WORLD transform, identity here);
// otherwise with the raw input normal, which The Sims 3 packs into bytes -- garbage as a
// direction, so captured normals stayed off and everything was shaded per triangle. Every
// shader with a normal input transforms it for its own lighting and hands the pixel shader the
// world-space result in a texture coordinate. A per-component taint pass finds that output: the
// TEXCOORD output whose x, y and z derive from the normal inputs (and the skinning weights) and
// from nothing else. The variant made from it (vs_3_0 only) declares one more output with the
// NORMAL semantic and repeats every instruction that writes the found register into it. A
// shader with a normal input but no such output gets the input's semantic renamed instead, so
// the runtime falls back to the triangle normal rather than the packed bytes.
struct VsNormalInfo {
  bool valid = false;
  bool hasNormalInput = false;
  uint8_t version = 0;            // vertex shader major version
  uint16_t inputTexcoords = 0;    // declared input TEXCOORD indices (bitmask)
  uint16_t candidates = 0;        // TEXCOORD indices whose output is the transformed normal (bitmask)
  uint8_t outReg[16];             // per TEXCOORD index: the output register (o# / oT#), 0xFF none
  VsNormalInfo() { for (uint8_t& r : outReg) r = 0xFF; }
};

inline bool analyzeVertexNormal(const DWORD* tokens, size_t count, VsNormalInfo& out) {
  out = VsNormalInfo();
  if (!dxsoIsVertexShader(tokens, count, 2)) return false;
  out.version = (uint8_t) ((tokens[0] >> 8) & 0xFFu);
  const bool vs3 = out.version >= 3;
  enum : uint8_t { kNormal0 = 1, kNormal1 = 2, kBlendW = 4, kBlendI = 8, kPosition = 16, kTexcoord = 32, kColor = 64, kOther = 128 };
  const uint32_t kTemp = kDxsoRegTemp, kInput = kDxsoRegInput, kOutput = kDxsoRegOutput;
  uint8_t tempTaint[32][4] = {}, inputTaint[16][4] = {}, outTaint[16][4] = {};
  bool outWritten[16][4] = {};
  int8_t outTexcoord[16];
  for (int i = 0; i < 16; ++i) outTexcoord[i] = vs3 ? (int8_t) -1 : (int8_t) i;   // vs_2_x: oT# is TEXCOORD#
  auto srcTaint = [&](uint32_t tok, uint32_t comp) -> uint8_t {
    const uint32_t ty = dxsoRegType(tok), n = dxsoRegNum(tok), sc = (tok >> (16 + 2 * comp)) & 3u;
    if (ty == kTemp && n < 32) return tempTaint[n][sc];
    if (ty == kInput && n < 16) return inputTaint[n][sc];
    return 0;
  };
  auto srcFirst = [&](uint32_t tok, uint32_t k) -> uint8_t { uint8_t all = 0; for (uint32_t c = 0; c < k; ++c) all |= srcTaint(tok, c); return all; };
  dxsoForEach(tokens, count, [&](size_t pos, uint32_t op, uint32_t len) {
    if (op == kDxsoOpDcl && len >= 2) {
      const uint32_t usage = tokens[pos + 1] & 0x1Fu, index = (tokens[pos + 1] >> 16) & 0xFu, dest = tokens[pos + 2];
      const uint32_t ty = dxsoRegType(dest), n = dxsoRegNum(dest);
      if (ty == kInput && n < 16) {
        uint8_t t = kOther;
        if (usage == kUsageNormal) { t = index == 0 ? (uint8_t) kNormal0 : (uint8_t) kNormal1; if (index == 0) out.hasNormalInput = true; }
        else if (usage == kUsageBlendWeight) t = kBlendW;
        else if (usage == kUsageBlendIndices) t = kBlendI;
        else if (usage == kUsagePosition) t = kPosition;
        else if (usage == kUsageTexcoord) { t = kTexcoord; out.inputTexcoords |= (uint16_t) (1u << index); }
        else if (usage == kUsageColor) t = kColor;
        for (uint32_t c = 0; c < 4; ++c) inputTaint[n][c] = t;
      } else if (vs3 && ty == kOutput && n < 16) {
        if (usage == kUsageTexcoord) outTexcoord[n] = (int8_t) index;
      }
      return true;
    }
    // no destination to follow: flow control (CALL .. LABEL, REP .. BREAKC), the address move,
    // and the definitions, whose parameters are literal payloads
    if (len < 1 || dxsoIsDef(op) || (op >= 0x19u && op <= 0x1Eu) || (op >= 0x26u && op <= 0x2Eu)) return true;
    const uint32_t dest = tokens[pos + 1], dty = dxsoRegType(dest), dn = dxsoRegNum(dest), mask = (dest >> 16) & 0xFu;
    // reductions feed every destination component from a fixed number of source components:
    // DP3 / NRM / CRS / M3xN three, DP4 / LIT / DST / M4xN four, DP2ADD two (its addend one)
    const uint32_t reduce = (op == 0x08u || op == 0x24u || op == 0x21u || op == 0x16u || op == 0x17u || op == 0x18u) ? 3u
                          : (op == 0x09u || op == 0x10u || op == 0x11u || op == 0x14u || op == 0x15u) ? 4u : op == 0x5Au ? 2u : 0u;
    uint8_t taint[4] = {};
    for (uint32_t i = 2; i <= len; ++i) {
      const uint32_t src = tokens[pos + i];
      if (!(src & 0x80000000u)) continue;
      if (reduce) { const uint8_t all = srcFirst(src, (op == 0x5Au && i == 4) ? 1u : reduce); for (uint32_t c = 0; c < 4; ++c) taint[c] |= all; }
      else for (uint32_t c = 0; c < 4; ++c) taint[c] |= srcTaint(src, c);
    }
    if (dty == kTemp && dn < 32) { for (uint32_t c = 0; c < 4; ++c) if (mask & (1u << c)) tempTaint[dn][c] = taint[c]; }
    else if (dty == kOutput && dn < 16) {
      for (uint32_t c = 0; c < 4; ++c) if (mask & (1u << c)) { outTaint[dn][c] = taint[c]; outWritten[dn][c] = true; }
    }
    return true;
  });
  for (uint32_t r = 0; r < 16; ++r) {
    if (outTexcoord[r] < 0) continue;
    if (!(outWritten[r][0] && outWritten[r][1] && outWritten[r][2])) continue;
    const uint8_t t = (uint8_t) (outTaint[r][0] | outTaint[r][1] | outTaint[r][2]);
    if (!(t & (kNormal0 | kNormal1)) || (t & (uint8_t) ~(kNormal0 | kNormal1 | kBlendW | kBlendI))) continue;
    out.candidates |= (uint16_t) (1u << outTexcoord[r]);
    out.outReg[outTexcoord[r]] = (uint8_t) r;
  }
  out.valid = true;
  return true;
}

// The candidate to use: the one the pixel shader itself treats as a normal when that is known,
// else the lowest. -1 when the shader has no candidate.
inline int chooseNormalTexcoord(const VsNormalInfo& v, uint16_t psNormalTexcoords) {
  if (!v.valid || !v.candidates) return -1;
  uint16_t pick = (uint16_t) (v.candidates & psNormalTexcoords);
  if (!pick) pick = v.candidates;
  int i = 0;
  while (!(pick & (1u << i))) ++i;                                // pick is non-zero here
  return i;
}

// A vs_3_0 variant with one more output, declared NORMAL, written by a repeat of every
// instruction that writes register `outReg` (its x, y, z). Returns the new register's number,
// 0xFF when it cannot be made (no free output register, nothing writes outReg); `t` is then
// left untouched.
inline uint32_t makeNormalVariant(std::vector<DWORD>& t, uint32_t outReg) {
  const size_t n = t.size();
  if (n < 2 || !dxsoIsVertexShader(t.data(), n, 3)) return 0xFFu;
  uint32_t maxReg = 0; size_t lastDcl = 0;
  dxsoForEach(t.data(), n, [&](size_t pos, uint32_t op, uint32_t len) {
    if (op == kDxsoOpDcl) { lastDcl = pos; if (len >= 2 && dxsoRegType(t[pos + 2]) == kDxsoRegOutput) maxReg = (std::max)(maxReg, dxsoRegNum(t[pos + 2])); return true; }
    if (dxsoIsDef(op)) return true;
    for (size_t i = 1; i <= len; ++i) { const uint32_t p = t[pos + i]; if ((p & 0x80000000u) && dxsoRegType(p) == kDxsoRegOutput) maxReg = (std::max)(maxReg, dxsoRegNum(p)); }
    return true;
  });
  const uint32_t N = maxReg + 1;
  if (N > 11 || lastDcl == 0) return 0xFFu;
  std::vector<DWORD> r; r.reserve(n + 64);
  r.push_back(t[0]);
  size_t pos = 1; bool inserted = false; uint32_t copies = 0;
  while (pos < n) {
    const uint32_t tok = t[pos];
    if (tok == kDxsoEnd) break;
    const uint32_t op = tok & 0xFFFFu;
    if (op == 0xFFFEu) { const size_t len = 1 + ((tok >> 16) & 0x7FFFu); if (pos + len > n) return 0xFFu; r.insert(r.end(), t.begin() + pos, t.begin() + pos + len); pos += len; continue; }
    const uint32_t len = (tok >> 24) & 0xFu;
    if (pos + len >= n) return 0xFFu;
    r.insert(r.end(), t.begin() + pos, t.begin() + pos + 1 + len);
    if (pos == lastDcl) {
      dxsoPushDcl(r, kUsageNormal, 0, N, 0x7u);                                     // dcl_normal o<N>.xyz
      inserted = true;
    } else if (op != kDxsoOpDcl && !dxsoIsDef(op) && len >= 1) {
      const uint32_t dest = t[pos + 1];
      if ((dest & 0x80000000u) && dxsoRegType(dest) == kDxsoRegOutput && dxsoRegNum(dest) == outReg && ((dest >> 16) & 0x7u)) {
        const size_t at = r.size();
        r.insert(r.end(), t.begin() + pos, t.begin() + pos + 1 + len);
        r[at + 1] = (dest & ~0x000F07FFu) | (((dest >> 16) & 0x7u) << 16) | N;     // the same write, x/y/z only, into o<N>
        ++copies;
      }
    }
    pos += 1 + len;
  }
  while (pos < n) r.push_back(t[pos++]);
  if (!inserted || copies == 0) return 0xFFu;
  t.swap(r);
  return N;
}

// The input normal's declaration renamed to an unused TEXCOORD index, so the runtime sees no
// normal at all (triangle normals) instead of the packed bytes. Returns false when nothing
// could be renamed.
inline bool hideNormalInput(std::vector<DWORD>& t, const VsNormalInfo& info) {
  if (!info.valid || !info.hasNormalInput) return false;
  int freeIdx = -1;
  for (int i = 15; i >= 0; --i) if (!(info.inputTexcoords & (1u << i))) { freeIdx = i; break; }
  if (freeIdx < 0) return false;
  bool done = false;
  dxsoForEach(t.data(), t.size(), [&](size_t pos, uint32_t op, uint32_t len) {
    if (op != kDxsoOpDcl || len < 2) return true;
    uint32_t usage, idx, reg;
    if (dxsoDcl(t.data(), pos, len, kDxsoRegInput, usage, idx, reg) && usage == kUsageNormal && idx == 0) {
      t[pos + 1] = (t[pos + 1] & ~0xF001Fu) | kUsageTexcoord | ((uint32_t) freeIdx << 16);
      done = true;
      return false;
    }
    return true;
  });
  return done;
}

// A vs_2_x stream rewritten as vs_3_0 (milestone 11b), so that the NORMAL output above can be
// declared on it too: the fixed output registers (oPos, oFog, oPts, oD#, oT#) become numbered
// outputs with declared semantics -- oT# keeps its number, the others follow the highest oT --
// and the version token is bumped; instruction encodings are shared between the two versions.
// Returns false (stream untouched) when the outputs would not fit in o0..o11.
inline bool convertVs2To3(std::vector<DWORD>& t) {
  const size_t n = t.size();
  if (n < 2 || !dxsoIsVertexShader(t.data(), n, 2) || dxsoIsVertexShader(t.data(), n, 3)) return false;
  const uint32_t kRastOut = 4u, kAttrOut = 5u;
  bool usedT[8] = {}, usedRast[3] = {}, usedAttr[2] = {};
  size_t lastDcl = 0; bool bad = false;
  dxsoForEach(t.data(), n, [&](size_t pos, uint32_t op, uint32_t len) {
    if (op == kDxsoOpDcl) { lastDcl = pos; return true; }
    if (dxsoIsDef(op)) return true;
    for (size_t i = 1; i <= len; ++i) {
      const uint32_t p = t[pos + i];
      if (!(p & 0x80000000u)) continue;
      const uint32_t ty = dxsoRegType(p), r = dxsoRegNum(p);
      if (ty == kRastOut) { if (r < 3) usedRast[r] = true; else bad = true; }
      else if (ty == kAttrOut) { if (r < 2) usedAttr[r] = true; else bad = true; }
      else if (ty == kDxsoRegOutput) { if (r < 8) usedT[r] = true; else bad = true; }
    }
    return true;
  });
  if (bad || lastDcl == 0 || !usedRast[0]) return false;
  uint32_t next = 0;
  for (uint32_t i = 0; i < 8; ++i) if (usedT[i]) next = i + 1;
  uint32_t regRast[3] = { 0xFFu, 0xFFu, 0xFFu }, regAttr[2] = { 0xFFu, 0xFFu };
  for (uint32_t i = 0; i < 3; ++i) if (usedRast[i]) regRast[i] = next++;
  for (uint32_t i = 0; i < 2; ++i) if (usedAttr[i]) regAttr[i] = next++;
  if (next > 12) return false;
  auto dcl = [&](std::vector<DWORD>& r, uint32_t usage, uint32_t index, uint32_t reg) { dxsoPushDcl(r, usage, index, reg, 0xFu); };
  std::vector<DWORD> r; r.reserve(n + 48);
  r.push_back(0xFFFE0300u);
  size_t pos = 1;
  while (pos < n) {
    const uint32_t tok = t[pos];
    if (tok == kDxsoEnd) break;
    const uint32_t op = tok & 0xFFFFu;
    if (op == 0xFFFEu) { const size_t len = 1 + ((tok >> 16) & 0x7FFFu); if (pos + len > n) return false; r.insert(r.end(), t.begin() + pos, t.begin() + pos + len); pos += len; continue; }
    const uint32_t len = (tok >> 24) & 0xFu;
    if (pos + len >= n) return false;
    const size_t at = r.size();
    r.insert(r.end(), t.begin() + pos, t.begin() + pos + 1 + len);
    if (op == kDxsoOpDcl) {
      if (pos == lastDcl) {
        dcl(r, kUsagePosition, 0, regRast[0]);
        for (uint32_t i = 0; i < 8; ++i) if (usedT[i]) dcl(r, kUsageTexcoord, i, i);
        for (uint32_t i = 0; i < 2; ++i) if (usedAttr[i]) dcl(r, kUsageColor, i, regAttr[i]);
        if (usedRast[1]) dcl(r, kUsageFog, 0, regRast[1]);
        if (usedRast[2]) dcl(r, kUsagePointSize, 0, regRast[2]);
      }
    } else if (!dxsoIsDef(op)) {
      for (size_t i = 1; i <= len; ++i) {
        DWORD& p = r[at + i];
        if (!(p & 0x80000000u)) continue;
        const uint32_t ty = dxsoRegType(p), rn = dxsoRegNum(p);
        const uint32_t reg = ty == kRastOut ? regRast[rn] : ty == kAttrOut ? regAttr[rn] : 0xFFu;
        if (reg != 0xFFu) p = (DWORD) ((p & ~(0x70000000u | 0x1800u | 0x7FFu)) | ((kDxsoRegOutput & 7u) << 28) | reg);
      }
    }
    pos += 1 + len;
  }
  r.push_back(kDxsoEnd);
  t.swap(r);
  return true;
}

// A dead read of one constant register appended before END (milestone 12): the runtime hashes a
// captured draw's constants only up to the highest register the shader reads (all of them once
// it addresses relatively), so a shader that takes its per-instance data from the instance
// stream never has the per-instance tag in c255 in its hash. `mov r0, c<reg>` as the last
// instruction changes nothing the shader outputs and extends that range to the register.
inline bool appendConstantRead(std::vector<DWORD>& t, uint32_t reg) {
  if (!dxsoIsVertexShader(t.data(), t.size(), 0) || reg > 255) return false;
  size_t end = t.size();
  while (end > 1 && t[end - 1] != kDxsoEnd) --end;
  if (end <= 1) return false;
  const DWORD ins[3] = { 0x02000001u, 0x800F0000u, 0xA0E40000u | reg };   // MOV r0.xyzw, c<reg>.xyzw
  t.insert(t.begin() + (end - 1), ins, ins + 3);
  return true;
}

// ---- per-pixel-shader albedo stage (milestone 2c) -----------------------------------------
// "The first colour 2D texture" is the wrong albedo for multi-texture materials: the object
// shaders keep an emissive map at s2 and the diffuse at s3, the floor shaders a normal map at
// s1 and the diffuse blend at s2. The stage below was read from each pixel shader's
// arithmetic (the sample multiplied by the summed light colour) and is keyed by the pixel
// shader's bytecode hash; it takes precedence over pickAlbedoStage for captured draws.
// The object shaders also carry the four-light rig at c0..c7 (rig, see SunVoter) and, the
// recolourable ones, the Create-A-Style tint at c8 (tint, see packTint).
struct AlbedoStage { uint64_t hash; const char* name; uint8_t stage; bool rig; bool tint; };
inline constexpr uint8_t kTintRegister = 8;

inline const AlbedoStage kAlbedoStages[] = {
  // objects: s2 the lot's light map (sampled with a world-space projection, rows c15/c16 of the VS; scaled by c12.y), s3 diffuse (TEXCOORD2), s4 specular mask; light rig and tint
  { 0x0c19795eb80e2e96ull, "object PS 0x10a68900", 3, true, true },
  { 0x8282d3a0d611b62eull, "object PS 0x10a69440", 3, true, true },
  { 0x54bea85dc05c7c35ull, "object PS 0x10a694e0", 3, true, true },
  { 0x470c140c802b7ec0ull, "object PS 0x1118d5e0", 3, true, true },
  { 0x5aee1186d554dbc4ull, "object PS 0x10a68220 (s2 diffuse, TEXCOORD2; 3 lights)", 2, true, false },
  // lot terrain paint (VS 0x15c787a0; thousands of triangles per draw): s1 normal map,
  // diffuse = mask blend of the paint layers s2/s3/s4 (TEXCOORD0). Not the floor tiles.
  { 0x17eabad58f650687ull, "terrain paint PS 0x13d1dd40", 2 },
  { 0x670dbe0fa52c4650ull, "terrain paint PS 0x13d1d5c0", 2 },
  { 0xd63bf505ec4a44a0ull, "terrain paint PS 0x13d1d8e0", 2 },
  { 0x98062e8d4d12af7dull, "terrain paint PS 0x13d1d980", 2 },
  // in-game variants (run-15 shader dump); the 0x10a68220 family carries the rig
  { 0x1458c67a2c009563ull, "object PS variant 1458c67a (s2 diffuse, TEXCOORD2)", 2, true, false },
  { 0xa3afadeeb6a034c6ull, "object PS variant a3afadee (s2 diffuse, TEXCOORD2)", 2, true, false },
  { 0x528502f128e81506ull, "PS 528502f1 (s1 on TEXCOORD2)", 1 },
  { 0xc0b8100612d70879ull, "PS c0b81006 (s1 on TEXCOORD1)", 1 },
  { 0x5e6fbac12103e50bull, "PS 5e6fbac1 (s1 on TEXCOORD1)", 1 },
  { 0x86f63bbb50a73dbcull, "PS 86f63bbb (s1 on TEXCOORD2)", 1 },
  { 0xd1478edc68b0d60cull, "outer ground PS d1478edc (pattern tile s2 on TEXCOORD2)", 2 },
  // doors / windows / trims: normal map at s1, diffuse blend at s3 or s4 (TEXCOORD0)
  { 0xced805afc5c51385ull, "PS 0x14592a60", 3 },
  { 0x0f79ca391d079af3ull, "PS 0x14592b00", 3 },
  { 0x9bb08cceb8d858c5ull, "PS 0x14594b80", 4 },
  { 0x77e55c68700be25full, "PS 0x14594ae0", 4 },
  { 0x1cbf4a03015f901cull, "PS 0x10a2afa0 (s3, TEXCOORD0)", 3 },
  { 0x920fb3f3573d30d4ull, "PS 0x10ea7000 (s3, TEXCOORD0)", 3 },
  { 0x9b8f4e2b9fbb9bb1ull, "PS 0x13d26d00 (s2, TEXCOORD1)", 2 },
  { 0x9ccd448262adfd4dull, "PS 0x105dbc60 (s2, TEXCOORD1)", 2 },
  { 0xd99d3c12905fd7c9ull, "PS 0xd7adc80 (s1, TEXCOORD2)", 1 },
  // layered ground decals: four layers on TEXCOORD0, first layer as albedo
  { 0x2d843890f7471cdfull, "PS 0xd7ad960 (s6..s9 layers)", 6 },
  { 0x2bc380a5a20143fcull, "PS 0x104068c0 (s7..s10 layers)", 7 },
  { 0x3ebb622c4fe0be4full, "floors PS 0x10a221a0 (s2 pattern on TEXCOORD0; s1 is the room lightmap)", 2 },
  { 0xa63ccabe650b1bc0ull, "floors PS 0x10a1f0e0 (s2 pattern on TEXCOORD0; s1 is the room lightmap)", 2 },
  // terrain and simple textured objects: stage 0 already
  { 0xe18ad53a96ff51ccull, "terrain PS 0x103f2f20", 0 },
  { 0x27ac2b7a8987e4d4ull, "terrain PS 0x13be1180", 0 },
  { 0x028ce2dde691b739ull, "terrain PS 0x13be0fa0", 0 },
  { 0x3608ab95ab50c8b4ull, "terrain PS 0x13be12c0", 0 },
  { 0xc30755d3de24af9aull, "terrain PS 0x13be1360", 0 },
  { 0xff72720db4324926ull, "PS 0x167e8be0 (s0, TEXCOORD1)", 0 },
  { 0xda37b5ef6f7a09a6ull, "floor tiles PS 0x167e3320 (s1 on TEXCOORD0; s0 is the lightmap)", 1 },
  { 0xdeeecbb3cdf04655ull, "PS 0xdbbbc40 (s1)", 1 },
  { 0xb0fb977be6b7bbccull, "PS 0x106fe840 (s1)", 1 },
  { 0xc3c0476375bf7797ull, "PS 0x16e98380 (s1)", 1 },
  { 0x78a22ef03769d4deull, "PS 0x16e95cc0 (s1)", 1 },
  { 0x6c1867be86538473ull, "PS 0x13d275c0 (s1, TEXCOORD5)", 1 },
};

inline const AlbedoStage* findAlbedoStage(uint64_t hash) { return findByHash(kAlbedoStages, hash); }

// ---- Create-A-Style tint via the fixed-function texture factor (milestone 2c) --------------
// The object pixel shaders multiply their diffuse sample by a per-object tint constant (c8)
// before lighting: for recoloured furniture the texture is a greyscale pattern and c8 is the
// colour. Remix's legacy material multiplies the albedo by D3DRS_TEXTUREFACTOR when stage 0's
// colour argument 2 is TFACTOR (d3d9_rtx_utils.cpp: materialData.tFactor = renderStates[
// D3DRS_TEXTUREFACTOR]), and the game's pixel shaders ignore that fixed-function state, so
// the client forwards c8 there for captured draws (white for shaders without a tint).
inline const AlbedoStage* findTintConst(uint64_t hash) { const AlbedoStage* a = findAlbedoStage(hash); return (a && a->tint) ? a : nullptr; }

inline uint32_t packTint(const float* rgb) {
  auto to8 = [](float v) -> uint32_t { if (!(v > 0.f)) return 0u; if (v > 1.f) return 255u; return (uint32_t) (v * 255.f + 0.5f); };
  return 0xFF000000u | (to8(rgb[0]) << 16) | (to8(rgb[1]) << 8) | to8(rgb[2]);   // D3DCOLOR ARGB
}

// ---- draws never to capture (milestone 2c; populated in 2f; blended-only entries in 16) ----
// Draws of the listed vertex shaders are left to rasterization (absent from the ray-traced
// image): passes the ray tracer cannot represent. An entry marked blendedOnly leaves only the
// shader's alpha-blended draws to rasterization and captures its opaque ones.
struct NeverCapture { uint64_t hash; const char* name; bool blendedOnly; };

inline const NeverCapture kNeverCapture[] = {
  // from the run-15 shader dump
  { 0xc79615c0181b5ef1ull, "drop-shadow decals (instanced quads multiplied over the ground)", false },
  // Close-range grass and flower sprites: camera-relative, faded by distance (hidden
  // instances collapse to the origin), one of four axis orientations per instance, two
  // wind-animated frames blended, alpha cut by texkill. Whole quads under capture -- the
  // "green walls" that pop up as the camera comes close.
  { 0x5a2deada1e077b44ull, "grass/flower detail sprites (distance-faded, wind-animated, alpha-cut)", false },
  // A skinned Sim pass whose pixel shader (11227d6d) writes pure black with the composite's
  // alpha times a constant: a darkening overlay coincident with the body. Under capture it is a
  // second surface on the body (black in the raw albedo view once culling favours it, run 57);
  // the ray tracer shades the body itself, so the overlay stays with the rasterizer.
  { 0xab38a73070378739ull, "Sim black overlay pass (skinned, pixel shader outputs black x composite alpha)", false },
  // The lot overlays drawn right after each lot ground patch (milestone 18c, run 109): three
  // families with NO texture at all (alpha-blended, depth write off), one draw per lot patch --
  // the game's shading / fade quads over the lot -- and their textured sibling that draws the
  // buildings' shadow shapes onto the ground. Under capture an untextured draw is an opaque white
  // plane over the lot (the "white pop", runs 85-109; it has a geometry hash and no texture to
  // pick) and the shadow shapes are black surfaces; the ray tracer shades and shadows the lot itself.
  { 0x8e7f4f65c455f509ull, "lot overlay quad, untextured (one per lot ground patch)", true },
  { 0xd55820f978e8826full, "lot overlay quad, untextured", true },
  { 0x1139e30a9396c3c3ull, "lot overlay quad, untextured (after the world terrain)", true },
  { 0x23072b72226654bdull, "building shadow shapes on the ground (blended decals, textured)", true },
  // run 112: drawn in the top-down view, one per frame each -- terrain-packed positions (1/256)
  // with a second height per vertex, placed 35 % / 80 % of the way between the two heights, a
  // constant colour (PS 8133bb57) and the alpha from the height difference; captured they are
  // translucent sheets over the lot that shade the ground beneath them (alpha-blended geometry
  // casts shadows in the runtime)
  { 0x53422654d95996b1ull, "lot-sized untextured translucent sheet (top-down view, 35 %)", true },
  { 0x19c4591d463ac2a6ull, "lot-sized untextured translucent sheet (top-down view, 80 %)", true },
  // (The terrain paint passes -- the lot's 0344bbc3 and the world terrain's blended layers of
  // dfaf82cf -- were listed here until milestone 17; they are now baked by the runtime's terrain
  // baker as hidden layer passes, see kTerrainShaders.)
};

inline const NeverCapture* findNeverCapture(uint64_t hash) { return findByHash(kNeverCapture, hash); }
// The mode a vertex shader carries from its table entry: 0 captured, 1 never captured, 2 not
// captured when alpha blending is on.
inline uint8_t neverCaptureMode(const NeverCapture* n) { return n ? (n->blendedOnly ? 2 : 1) : 0; }
inline bool neverCaptureDraw(uint8_t mode, DWORD alphaBlendEnable) { return mode == 1 || (mode == 2 && alphaBlendEnable != 0); }

// ---- the lot terrain drawn once per world chunk (milestone 16) ----------------------------
// A lot's ground mesh is drawn once for every 256-unit world chunk it overlaps, with that chunk's
// textures, each copy clipped to its chunk by the kill rectangle -- the clip kShaderPatches
// disables, since under vertex capture the clipped vertices became the fans. With the clip off
// every copy is the whole lot, so a lot on a chunk boundary was two coincident opaque surfaces
// (run 73: the terrain flicker in the lot). The frame's first draw of a mesh (same vertex
// buffer, same World rows c4..c6) is captured and the copies after it are dropped; they differ
// only in the chunk textures, which the ray tracer does not use.
inline uint64_t lotTerrainKey(uint64_t bufferId, const float rows[12]) {
  uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&](uint32_t v) { for (int i = 0; i < 4; ++i) { h ^= (v >> (8 * i)) & 0xFFu; h *= 0x100000001b3ull; } };
  mix((uint32_t) bufferId); mix((uint32_t) (bufferId >> 32));
  for (int i = 0; i < 12; ++i) { uint32_t bits; memcpy(&bits, &rows[i], 4); mix(bits); }
  return h;
}
struct LotCopies {
  static constexpr uint32_t kMax = 16;
  uint64_t keys[kMax] = {}; uint32_t count = 0;
  // Whether the key was seen this frame; a new key is remembered while there is room (past
  // kMax meshes in a frame further draws are simply kept).
  bool seen(uint64_t key) {
    for (uint32_t i = 0; i < count; ++i) if (keys[i] == key) return true;
    if (count < kMax) keys[count++] = key;
    return false;
  }
  void clear() { count = 0; }
};

// ---- terrain paint through the runtime's terrain baker (milestone 17) ----------------------
// The ground is painted in layers: each draw's pixel shader blends up to four tiling layer
// textures by per-chunk masks, and further layers come as alpha-blended passes over the same
// triangles. One texture per draw cannot carry that, so the draws are handed to the runtime's
// terrain baker: it re-renders every draw whose stage-0 texture is tagged rtx.terrainTextures
// with the game's own pixel shader, top-down, into a cascade of textures around the camera, and
// ray-traces the geometry with that texture (rtx_terrain_baker.cpp). The client's part:
//   * a terrain draw gets a hook-made MARKER texture at stage 0 (the runtime identifies a
//     material by the hash of its stage-0 texture) and the game's stage-0 texture moved to the
//     shader's detail stage through a pixel shader variant (psSwapSamplers, psDetailSampler), so
//     the shader still reads it and reads the marker as its detail (milestone 17k);
//   * a layer pass -- alpha-blended layers, the lot's paint composite, a lot mesh drawn again
//     for a further world chunk -- is baked with its blend state (alpha as the shader writes it)
//     but not ray-traced as its own geometry. The world's blended layers get the second marker,
//     tagged terrain AND hidden (rtx.hideInstanceTextures): separate geometry. A lot's
//     re-submissions -- its further chunk copies, its replays, its paint composite -- are hidden
//     too and issued as two half draws, so the runtime never takes one for the lot's
//     own visible instance (runs 85-89: white or flickering lots whenever it did);
//   * the lit lot-paint shaders get their final lighting multiply replaced by the albedo
//     (psUnlitOutput), so the bake carries no sun, shadow or fog.
// The markers' content is fixed, so their hashes are stable across runs: the hook sets the two
// runtime options itself when it can hash (xxhash), otherwise the two textures are tagged once
// in the runtime's menu.
// Hook options from `sims3hook.txt` next to this DLL (one `key = value` per line, integers;
// `#` comments), read once. Absent file or key = the default. Used for the diagnostics that
// are flipped between runs without a rebuild.
inline int hookOption(const char* key, int def) {
  struct Entry { char key[48]; int value; };
  static Entry entries[32]; static int count = -1;
  if (count < 0) {
    count = 0;
    char path[MAX_PATH] = {};
    HMODULE self = GetModuleHandleA("d3d9.dll");
    if (self && GetModuleFileNameA(self, path, MAX_PATH)) {
      if (char* slash = strrchr(path, '\\')) {
        snprintf(slash + 1, (size_t) (MAX_PATH - (slash + 1 - path)), "sims3hook.txt");
        if (FILE* f = fopen(path, "rb")) {
          char line[256];
          while (fgets(line, sizeof line, f) && count < 32) {
            char* p = line; while (*p == ' ' || *p == '\t') ++p;
            if (*p == '#' || *p == '\r' || *p == '\n' || !*p) continue;
            char* eq = strchr(p, '='); if (!eq) continue;
            char* end = eq; while (end > p && (end[-1] == ' ' || end[-1] == '\t')) --end;
            const size_t kl = (size_t) (end - p); if (kl == 0 || kl >= sizeof entries[0].key) continue;
            memcpy(entries[count].key, p, kl); entries[count].key[kl] = 0;
            entries[count].value = atoi(eq + 1);
            ++count;
          }
          fclose(f);
        }
      }
    }
  }
  for (int i = 0; i < count; ++i) if (strcmp(entries[i].key, key) == 0) return entries[i].value;
  return def;
}

// Toggle with SIMS3_TERRAIN (unset/1 = the baker path; 0, f or n = the pre-M17 capture), or
// `terrain = 0` in sims3hook.txt.
inline bool terrainEnabled() {
  static int s = -1;
  if (s < 0) {
    char e[8] = {};
    const DWORD n = GetEnvironmentVariableA("SIMS3_TERRAIN", e, sizeof e);
    s = (n > 0 && (e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' || e[0] == 'N')) ? 0 : (hookOption("terrain", 1) != 0 ? 1 : 0);
  }
  return s == 1;
}
// markKey (milestone 17y): the virtual-key code of the key that writes the rolling trace to the log
// (F9 and the backtick key always work too); default 45 = Insert.
inline int markKey() { static int s = -1; if (s < 0) { s = hookOption("markKey", 45); if (s < 1 || s > 254) s = 45; } return s; }
// ringTrace = 1 keeps the rolling trace of every draw and event (the last ~300 frames) that the
// mark key writes to the log (diagnostic; 0 = off).
inline int ringTrace() { static int s = -1; if (s < 0) s = hookOption("ringTrace", 0) != 0; return s; }
// Night from the sun (milestone 20d): skyFromSun = 1 drives the runtime's sky brightness
// (rtx.skyBrightness) and the ceiling of its auto-exposure (rtx.autoExposure.evMaxValue) from
// the luminance of the sun the hook holds, through the Remix API (the server loads the
// runtime's API only with exposeRemixApi = True in .trex\bridge.conf). skyDayLevel = the sun
// luminance of full day in thousandths, the reference (a higher one measured raises it);
// skyMinBrightness = the sky's floor in thousandths; dayEvMax / nightEvMax = the exposure
// ceiling in hundredths of an EV at full day and at the floor (the runtime's default is 5.0).
inline int skyFromSun() { static int s = -1; if (s < 0) s = hookOption("skyFromSun", 1) != 0; return s; }
inline float skyDayLevel() { static float s = -1.f; if (s < 0.f) { int v = hookOption("skyDayLevel", 900); if (v < 10) v = 10; s = (float) v / 1000.f; } return s; }
inline float skyMinBrightness() { static float s = -1.f; if (s < 0.f) { int v = hookOption("skyMinBrightness", 30); if (v < 0) v = 0; if (v > 1000) v = 1000; s = (float) v / 1000.f; } return s; }
inline float dayEvMax() { static float s = -99.f; if (s < -98.f) { int v = hookOption("dayEvMax", 500); if (v < -1000) v = -1000; if (v > 1000) v = 1000; s = (float) v / 100.f; } return s; }
inline float nightEvMax() { static float s = -99.f; if (s < -98.f) { int v = hookOption("nightEvMax", 100); if (v < -1000) v = -1000; if (v > 1000) v = 1000; s = (float) v / 100.f; } return s; }
// The lights go to the runtime through the Remix API (milestones 20b, 23): the sun as a distant
// light, the lamps as sphere lights, with explicit radiance and size. The API needs
// exposeRemixApi = True in .trex\bridge.conf; without it there are no lights and one warning.
// sunAngle = the sun's angular diameter, thousandths of a degree; sunRadiance = radiance per
// unit of rig colour, thousandths; lampRadius = the lamps' sphere radius in thousandths of a
// unit; lampRadiance = radiance per unit of colour, thousandths.
inline float sunAngle() { static float s = -1.f; if (s < 0.f) { int v = hookOption("sunAngle", 2000); if (v < 100) v = 100; if (v > 90000) v = 90000; s = (float) v / 1000.f; } return s; }
inline float sunRadiance() { static float s = -1.f; if (s < 0.f) { int v = hookOption("sunRadiance", 1000); if (v < 0) v = 0; s = (float) v / 1000.f; } return s; }
inline float lampRadius() { static float s = -1.f; if (s < 0.f) { int v = hookOption("lampRadius", 150); if (v < 20) v = 20; s = (float) v / 1000.f; } return s; }
inline float lampRadiance() { static float s = -1.f; if (s < 0.f) { int v = hookOption("lampRadiance", 40000); if (v < 0) v = 0; s = (float) v / 1000.f; } return s; }

// layerPass: every draw is a layer pass. lotFamily: a lot's ground and its paint composite --
// drawn in place, the first copy visible and every re-submission (further chunk copies, the
// composite's passes) hidden and split in two (milestones 16-19; the lot replay of milestones
// 17e-18i re-issued them after the world terrain, which run 119's paint test showed is not
// needed: nothing overwrites a lot's paint in the atlas). coverageAlpha: the base draws keep
// the shader's own alpha (its paint coverage) and bake with an alpha test; tried in run 77 and
// wrong for this game -- the world's base pass is black where only its blended layer passes paint,
// and the skipped texels showed stale bakes -- so no shader uses it.
struct TerrainShader { uint64_t hash; const char* name; bool layerPass; bool coverageAlpha; bool lotFamily; };

inline const TerrainShader kTerrainShaders[] = {
  { 0x55c99586fb17cd1cull, "lot-area terrain paint (lit, 3 layers + lot mask)", false, false, false },
  { 0xdfaf82cf9ec175b0ull, "world terrain (4 layers + chunk mask; alpha-blended draws = extra layers)", false, false, false },
  { kLotTerrainVs,         "lot terrain (3 layers + chunk mask, clipped per chunk by texkill)", false, false, true },
  { 0x0344bbc366f10954ull, "lot paint composite (unlit 4-layer blend over the lot terrain)", true, false, true },
};

// How a terrain variant treats alpha: 0 as the shader writes it (blended layer passes), 1 forced
// to 1 (base draws), 2 the shader's own coverage, baked with an alpha test (unused, run 77).
// A lot mesh's further chunk copies and its replays are opaque draws each clipped to its own
// world chunk by texkill, together covering the lot: each must write the opacity (alpha 1) for
// its part (milestone 17n; with only the first copy writing alpha, the rest of a lot kept stale
// atlas texels, which shift whenever the cascades re-centre on a moving camera -- run 87's
// flicker during camera movement). The blended composite keeps the shader's alpha.
inline uint8_t terrainAlphaMode(const TerrainShader* t, uint8_t kind, DWORD alphaBlendEnable) {
  if (kind == 1) return (t && t->coverageAlpha) ? 2 : 1;
  if (t && t->lotFamily && !t->layerPass && alphaBlendEnable == 0) return 1;
  return 0;
}
inline const TerrainShader* findTerrainShader(uint64_t hash) { return findByHash(kTerrainShaders, hash); }

// 0 not a terrain draw, 1 base terrain (baked and ray-traced), 2 layer pass (baked, hidden).
inline uint8_t terrainDrawKind(const TerrainShader* t, DWORD alphaBlendEnable, bool furtherLotCopy) {
  if (!t) return 0;
  return (t->layerPass || alphaBlendEnable != 0 || furtherLotCopy) ? 2 : 1;
}

// The lit lot-area paint pixel shaders: colour = albedo x light + fog, as one final
// `mad oC0.xyz, albedo, light, fog`; the unlit variant keeps the albedo (psUnlitOutput).
inline const uint64_t kUnlitPatches[] = { 0x17eabad58f650687ull, 0x670dbe0fa52c4650ull, 0x98062e8d4d12af7dull, 0xd63bf505ec4a44a0ull };
inline bool wantsUnlitPatch(uint64_t psHash) { for (uint64_t h : kUnlitPatches) if (h == psHash) return true; return false; }

inline constexpr uint32_t kDxsoRegSampler = 10u, kDxsoRegColorOut = 8u, kDxsoOpMov = 1u, kDxsoOpMad = 4u;

// The highest sampler a pixel shader declares or reads, or -1.
inline int psMaxSampler(const DWORD* t, size_t count) {
  int m = -1;
  dxsoForEach(t, count, [&](size_t pos, uint32_t op, uint32_t len) {
    if (dxsoIsDef(op)) return true;
    if (op == kDxsoOpDcl) { if (len >= 2 && dxsoRegType(t[pos + 2]) == kDxsoRegSampler) m = (std::max)(m, (int) dxsoRegNum(t[pos + 2])); return true; }
    for (size_t i = 1; i <= len; ++i) if ((t[pos + i] & 0x80000000u) && dxsoRegType(t[pos + i]) == kDxsoRegSampler) m = (std::max)(m, (int) dxsoRegNum(t[pos + i]));
    return true;
  });
  return m;
}

// Renumbers sampler a as b and b as a in every declaration and read; returns the tokens changed.
inline uint32_t psSwapSamplers(std::vector<DWORD>& t, uint32_t a, uint32_t b) {
  uint32_t changed = 0;
  auto swapTok = [&](DWORD& p) {
    if (!(p & 0x80000000u) || dxsoRegType(p) != kDxsoRegSampler) return;
    const uint32_t n = dxsoRegNum(p);
    if (n == a) { p = dxsoSetRegNum(p, b); ++changed; } else if (n == b) { p = dxsoSetRegNum(p, a); ++changed; }
  };
  dxsoForEach(t.data(), t.size(), [&](size_t pos, uint32_t op, uint32_t len) {
    if (dxsoIsDef(op)) return true;
    if (op == kDxsoOpDcl) { if (len >= 2) swapTok(t[pos + 2]); return true; }
    for (size_t i = 1; i <= len; ++i) swapTok(t[pos + i]);
    return true;
  });
  return changed;
}

// Replaces the last `mad oC0.xyz, a, b, c` by `mov oC0.xyz, a`; false when the last write of
// oC0's colour is anything else (the shader is then not one of kUnlitPatches' shape).
inline bool psUnlitOutput(std::vector<DWORD>& t) {
  size_t last = 0; uint32_t lastOp = 0, lastLen = 0;
  dxsoForEach(t.data(), t.size(), [&](size_t pos, uint32_t op, uint32_t len) {
    if (dxsoIsDef(op) || op == kDxsoOpDcl || len < 1) return true;
    const DWORD d = t[pos + 1];
    if ((d & 0x80000000u) && dxsoRegType(d) == kDxsoRegColorOut && dxsoRegNum(d) == 0 && ((d >> 16) & 0x7u) == 0x7u) { last = pos; lastOp = op; lastLen = len; }
    return true;
  });
  if (last == 0 || lastOp != kDxsoOpMad || lastLen != 4) return false;
  t[last] = (t[last] & ~0x0F00FFFFu) | kDxsoOpMov | (2u << 24);   // opcode and length; the control bits stay
  t.erase(t.begin() + (ptrdiff_t) last + 3, t.begin() + (ptrdiff_t) last + 5);
  return true;
}

// Makes the shader end with `mov oC0.w, 1`: the baked terrain's alpha is its opacity in the ray
// tracer (the runtime expects 1 there), while the game's terrain shaders write the paint weight
// or 0 and mostly mask alpha off. A constant register above every one the shader uses is
// defined as 1 (a DEF after the last declaration), and the write is appended before END.
// Applied to base terrain draws only: a layer pass's alpha is its blend weight.
inline bool psForceAlphaOne(std::vector<DWORD>& t) {
  if (t.size() < 2 || t.back() != kDxsoEnd) return false;
  int maxConst = -1; size_t lastDecl = 0;
  dxsoForEach(t.data(), t.size(), [&](size_t pos, uint32_t op, uint32_t len) {
    if (dxsoIsDef(op) || op == kDxsoOpDcl) { lastDecl = pos + 1 + len; }
    if (op == 0x51u && len >= 1 && dxsoRegType(t[pos + 1]) == 2u) maxConst = (std::max)(maxConst, (int) dxsoRegNum(t[pos + 1]));   // DEF c#
    else if (!dxsoIsDef(op) && op != kDxsoOpDcl) for (size_t i = 1; i <= len; ++i) if ((t[pos + i] & 0x80000000u) && dxsoRegType(t[pos + i]) == 2u) maxConst = (std::max)(maxConst, (int) dxsoRegNum(t[pos + i]));
    return true;
  });
  const int reg = maxConst + 1;
  if (reg > 223 || lastDecl == 0 || lastDecl >= t.size()) return false;
  const DWORD one = 0x3F800000u;
  const DWORD defTokens[6] = { 0x51u | (5u << 24), 0x80000000u | (2u << 28) | (0xFu << 16) | (DWORD) reg, one, one, one, one };   // def cN, 1, 1, 1, 1
  t.insert(t.begin() + (ptrdiff_t) lastDecl, defTokens, defTokens + 6);
  const DWORD movTokens[3] = { kDxsoOpMov | (2u << 24), 0x80000000u | (1u << 11) | (0x8u << 16), 0x80000000u | (2u << 28) | (0x00u << 16) | (DWORD) reg };   // mov oC0.w, cN.x
  t.insert(t.end() - 1, movTokens, movTokens + 3);
  return true;
}

// ---- the stage layer 0 is read from (milestone 17k) ------------------------------------------
// A texture the hook binds at a sampler stage the game's shader does not declare is not seen by
// the in-frame bake: the world was black with layer 0 moved to the first undeclared stage
// (runs 78-82), green with it moved to stage 1, a declared stage whose own texture was displaced
// for the draw (run 84), and green with no move at all (run 83, the marker read as layer 0); at
// Present an undeclared stage works (the lot replays, runs 78/80). The runtime's bookkeeping
// (SetStateTexture / UndirtyTextures / PrepareDraw / BindTexture, the compiler's per-sampler
// bound spec constant) shows nothing stage-specific, so the cause stays unknown; the way round
// it: layer 0 goes to a stage the shader declares AND the game binds for the draw, whose own
// texture the bake can do without -- the DETAIL texture. Every Sims 3 terrain pixel shader ends
// with `texld rN, v0.zwzw, sD` / `add rN.w, rN.x, rN.x` / colour x rN.w: a fine L8 grain sampled
// with the second half of TEXCOORD0, doubled. With s0 and sD swapped the variant reads layer 0
// from sD (the game's layer-0 texture bound there) and the "detail" from s0, where the marker
// sits: its red is 0x80, so the detail factor is 2 x 128/255 = 1.004 -- the bake loses the grain,
// nothing else. A shader without such a read (the lot paint composite) falls back to the first
// undeclared stage, which works for it because it is only ever replayed at Present.
inline constexpr uint32_t kDxsoOpTex = 0x42u, kDxsoSwizzleZwzw = 0xEEu;   // texld; a source's .zwzw swizzle

// The sampler of the shader's last `texld r, v0.zwzw, s` (its detail read), or -1.
inline int psDetailSampler(const DWORD* t, size_t count) {
  int m = -1;
  dxsoForEach(t, count, [&](size_t pos, uint32_t op, uint32_t len) {
    if (op != kDxsoOpTex || len < 3) return true;
    const DWORD src0 = t[pos + 2], src1 = t[pos + 3];
    if (!(src0 & 0x80000000u) || dxsoRegType(src0) != kDxsoRegInput || dxsoRegNum(src0) != 0 || ((src0 >> 16) & 0xFFu) != kDxsoSwizzleZwzw) return true;
    if (!(src1 & 0x80000000u) || dxsoRegType(src1) != kDxsoRegSampler) return true;
    m = (int) dxsoRegNum(src1);
    return true;
  });
  return m;
}

// ---- the lot paint composite (milestone 17l) -------------------------------------------------
// PS 99ee53ff (VS 0344bbc3) has no detail read: s0 = the lot's paint mask (v0), s1..s4 = four
// paint layers (v1); colour = s3 x mask.x + s1 x mask.z + s2 x mask.y + s4 x mask.w, alpha =
// mask.x, blended ONE / INVSRCALPHA under the game's alpha test. Its mask therefore goes to a
// LAYER's stage (bound by the game for the draw) and a BLACK marker takes stage 0, read as that
// layer: its term is 0. Two passes make the draw whole: pass 1 (mask at s4) paints layers 1..3
// with the game's blend; pass 2 (mask at s3, the black marker at s1 and s2 as well) adds
// layer 4 x mask.w alone, blended ONE / ONE with the same alpha test. The game's own draw of
// the composite is pass 1 (layer 4 missing) -- the replay after the world terrain redoes both.
inline constexpr uint64_t kLotCompositePs = 0x99ee53ff6ef1b0b6ull;
inline int lotCompositeStage(int pass) { return pass == 1 ? 4 : pass == 2 ? 3 : 0; }

// The marker textures: 32x32 A8R8G8B8, fixed and flat. 0 = terrain (mid grey; base draws;
// visible), 1 = the world's layer passes and a lot's lifted copies and replays (the same red,
// with blue; hidden), 2 = the composite's passes (black; hidden, lifted). The red is what matters
// for 0 and 1: the variant reads the marker as the shader's detail texture (psDetailSampler),
// doubled, and 0x80 makes that a factor of 1; the composite reads its marker as a paint layer,
// and black takes that layer out. Any fixed content fixes the hashes, which are what rtx.conf
// tags. D3DCOLOR (A R G B; memory order B, G, R, A on this machine).
// (Run 91 diagnostic: the terrain marker is DARK RED (0xFF800000), so a lot showing its marker --
// a draw whose bake did not replace its material -- is told apart from a lot showing alpha 0.)
inline constexpr uint32_t kTerrainMarkerSize = 32;
inline constexpr int kTerrainMarkers = 3;
inline constexpr uint32_t kTerrainMarkerColor[kTerrainMarkers] = { 0xFF800000u, 0xFF8000FFu, 0xFF000000u };
inline void terrainMarkerPixels(int kind, uint32_t* out) {
  const uint32_t c = kTerrainMarkerColor[kind < 0 || kind >= kTerrainMarkers ? 0 : kind];
  for (uint32_t i = 0; i < kTerrainMarkerSize * kTerrainMarkerSize; ++i) out[i] = c;
}

// ---- the sky dome (runs 70-72) ------------------------------------------------------------
// The game's sky is a camera-relative sphere (VS 41a25ab3: the position rotated by c4..c6 into a
// cube-map direction, and its clip z copied from w so it sits at the far plane), drawn only
// while the sky is in frame. Captured as geometry it is a 3 km sphere around the lot that blocks
// the sun; left to the rasterizer it is painted over by the ray-traced image, whose sky is then
// black (and the sun's glow, which the dome's pixel shader draws, is gone). The runtime renders
// a draw as its SKY -- the backdrop and the environment light -- when the draw's viewport has a
// minimum depth of 1.0 (rtx.skyMinZThreshold), provided the draw reaches its classification:
// with a camera, and with a hashable 2D texture at stage 0 (a cube map has no hash). The device
// therefore captures a sky dome draw like any other, presents its first 2D texture at stage 0
// and sets a depth-1 viewport around it.
//
// Recognised from the bytecode: a position input, and the output position's z copied from its w
// (`mov oPos, r.xyzz`, or oPos.z and oPos.w from one DP4 source and constant row). Shaders that
// collapse the position to a constant (the game's stubs, `mov oPos, c0.x`) have no position input.
inline bool isSkyDomeShader(const DWORD* tokens, size_t count) {
  if (!dxsoIsVertexShader(tokens, count, 2)) return false;
  const bool vs3 = dxsoIsVertexShader(tokens, count, 3);
  const uint32_t posType = vs3 ? kDxsoRegOutput : 4u;   // vs_3_0: the o# declared POSITION; vs_2_x: oPos (RASTOUT 0)
  int posReg = vs3 ? -1 : 0;
  bool sky = false, hasPosInput = false, zSet = false, wSet = false; uint32_t zSrc = 0, wSrc = 0, zRow = 0, wRow = 0;
  dxsoForEach(tokens, count, [&](size_t pos, uint32_t op, uint32_t len) {
    if (op == kDxsoOpDcl) {
      uint32_t u, i, r;
      if (dxsoDcl(tokens, pos, len, kDxsoRegInput, u, i, r) && u == kUsagePosition) hasPosInput = true;
      else if (vs3 && dxsoDcl(tokens, pos, len, kDxsoRegOutput, u, i, r) && u == kUsagePosition && i == 0) posReg = (int) r;
      return true;
    }
    if (dxsoIsDef(op) || len < 2) return true;
    const uint32_t dest = tokens[pos + 1];
    if (posReg < 0 || dxsoRegType(dest) != posType || (int) dxsoRegNum(dest) != posReg) return true;
    const uint32_t mask = (dest >> 16) & 0xFu;
    if (op == 0x01u && (mask & 0xCu) == 0xCu) {                        // MOV oPos, src: z and w from the same component?
      const uint32_t src = tokens[pos + 2];
      if (((src >> 20) & 3u) == ((src >> 22) & 3u)) sky = true;
    } else if (op == 0x09u && len >= 3) {                              // DP4 oPos.z / oPos.w with one source and one row
      const uint32_t a = tokens[pos + 2], b = tokens[pos + 3];
      if (mask & 0x4u) { zSet = true; zSrc = a; zRow = b; }
      if (mask & 0x8u) { wSet = true; wSrc = a; wRow = b; }
      if (zSet && wSet && zSrc == wSrc && zRow == wRow) sky = true;
    }
    return !sky;
  });
  return sky && hasPosInput;
}

// ---- the sun, from the per-object light rig (milestone 2b) --------------------------------
// The object pixel shaders light in world space with a four-light rig uploaded per draw:
// c0..c3 are unit directions TOWARD the lights, c4..c7 their colours (dp3 with the world
// normal, then colour * NdotL). The brightest light from above the horizon is the sun (or
// sky key); the rest are fills. Because the rig is per object (indoor objects carry lamp
// rigs), each upload casts a vote and Present takes the majority, which the client then
// forwards as a fixed-function directional light. Remix converts fixed-function lights
// into ray-traced ones and drops its fallback light once a real light exists.
inline const AlbedoStage* findLightRig(uint64_t hash) { const AlbedoStage* a = findAlbedoStage(hash); return (a && a->rig) ? a : nullptr; }

inline float luminance(const float* c) { return 0.2126f*c[0] + 0.7152f*c[1] + 0.0722f*c[2]; }

struct SunVote { float dir[3]; float col[3]; uint32_t count; };

struct SunVoter {
  SunVote votes[8];
  uint32_t n = 0;
  void clear() { n = 0; }
  // dirs: c0..c3 (4 float4), cols: c4..c7 (4 float4)
  void add(const float* dirs, const float* cols) {
    int best = -1; float bestLum = 0.f;
    for (int i = 0; i < 4; ++i) {
      const float* d = dirs + i*4; const float* c = cols + i*4;
      const float len = len3(d);
      if (len < 0.9f || len > 1.1f || d[1] / len < 0.15f) continue;   // a unit direction, from above the horizon
      const float lum = luminance(c);
      if (lum > bestLum) { bestLum = lum; best = i; }
    }
    if (best < 0 || bestLum < 0.05f) return;
    const float* d = dirs + best*4; const float* c = cols + best*4;
    const float len = len3(d); const float nd[3] = { d[0]/len, d[1]/len, d[2]/len };
    for (uint32_t k = 0; k < n; ++k) {
      SunVote& v = votes[k];
      if (dot3(v.dir, nd) > 0.999f && std::fabs(luminance(v.col) - bestLum) < 0.05f) { ++v.count; return; }
    }
    if (n < 8) { SunVote& v = votes[n++]; for (int j = 0; j < 3; ++j) { v.dir[j] = nd[j]; v.col[j] = c[j]; } v.count = 1; }
  }
  // The sun is the brightest candidate with real support (two rigs and a fifth of the votes):
  // a view of mostly indoor objects votes for the dim sky fill by majority, and that must
  // not dim the whole scene. Without a qualified candidate, the majority decides.
  bool best(SunVote& out) const {
    uint32_t total = 0;
    for (uint32_t k = 0; k < n; ++k) total += votes[k].count;
    int bi = -1;
    for (uint32_t k = 0; k < n; ++k) {
      if (votes[k].count < 2 || votes[k].count * 5 < total) continue;
      if (bi < 0 || luminance(votes[k].col) > luminance(votes[bi].col)) bi = (int) k;
    }
    if (bi < 0)
      for (uint32_t k = 0; k < n; ++k)
        if (bi < 0 || votes[k].count > votes[bi].count || (votes[k].count == votes[bi].count && luminance(votes[k].col) > luminance(votes[bi].col))) bi = (int) k;
    if (bi < 0) return false;
    out = votes[bi];
    return true;
  }
  // The brightest candidate whose direction matches the given axis (within ~5 degrees), if
  // any: indoor objects report the sun's direction with a heavily attenuated colour, so the
  // most-voted match dims with the view while the brightest is the sun itself.
  bool matching(const float* dir, SunVote& out) const {
    int bi = -1;
    for (uint32_t k = 0; k < n; ++k) {
      if (dot3(votes[k].dir, dir) < 0.995f) continue;
      if (bi < 0 || luminance(votes[k].col) > luminance(votes[bi].col)) bi = (int) k;
    }
    if (bi < 0) return false;
    out = votes[bi];
    return true;
  }
};

// ---- what a pixel shader does with its samplers, read from its bytecode (milestone 7) ------
// The hand tables above answer two questions per shader: which sampler is the albedo, and
// which texture coordinate feeds it. Every untabled permutation the game compiles (a new lot,
// a new outfit type, a detail setting) rendered grey until it was read by hand. This reads the
// same facts from the bytecode when the shader is created: a per-component taint pass follows
// every sampler's read through the arithmetic to the colour output, and remembers, for each
// sampler, the TEXCOORD index its coordinate came from (or that it was computed). At draw
// time the albedo is chosen among the samplers that reach the colour with a known coordinate,
// from what is bound (chooseAutoAlbedo). The hand tables remain overrides.
struct PsSamplerUse {
  bool cube = false;        // declared as a cube map
  bool read = false;        // a texld reads it
  bool dependent = false;   // read with a computed coordinate (a temp register)
  bool projective = false;  // read with texldp (shadow maps)
  int8_t texcoord = -1;     // TEXCOORD index of the coordinate input, or -1
  uint8_t colorChannels = 0;// how many of its r, g, b reach the colour output (an albedo: 3; a mask: 1 or 2)
  bool reachesColor() const { return colorChannels > 0; }
};
struct PsAnalysis {
  PsSamplerUse samplers[16];
  uint16_t normalTexcoords = 0;   // TEXCOORD inputs the shader treats as a normal: normalised, or dotted with a constant (a light direction)
  // The wall opening mask (milestone 13): the sampler whose red is added to its own coordinate's
  // z (the wall shaders' "mask.r + t.z" test), and whether that value feeds a texkill or the
  // alpha output (walls C: alpha-tested).
  int8_t maskSampler = -1;
  bool maskKill = false, maskAlpha = false;
  bool valid = false;
};

// Taint: for every register component, the set of (sampler, channel) pairs whose sample fed
// it, followed through swizzles and write masks; reductions (dot products) spread every
// source channel to every destination component. 64 bits: 16 samplers x 4 channels.
inline bool analyzePixelShader(const DWORD* tokens, size_t count, PsAnalysis& out) {
  out = PsAnalysis();
  if (!tokens || count < 2) return false;
  const uint32_t version = tokens[0];
  if ((version & 0xFFFF0000u) != 0xFFFF0000u) return false;         // not a pixel shader
  const unsigned major = (version >> 8) & 0xFFu;
  if (major < 2) return false;                                        // ps_1_x has no dcl/texld of this shape
  int8_t inputTexcoord[32]; for (int i = 0; i < 32; ++i) inputTexcoord[i] = -1;
  uint64_t tempTaint[32][4] = {}, colorTaint[4][4] = {};
  const uint32_t kTemp = 0u, kInput = 1u, kTexture = 3u, kColorOut = 8u, kSampler = 10u;
  auto bit = [](uint32_t sampler, uint32_t channel) -> uint64_t { return 1ull << (sampler * 4 + channel); };
  // per temp: the sampler and coordinate register of the last TEXLD into it (the mask test)
  int8_t texldSampler[32]; uint32_t texldCoord[32] = {};
  for (int i = 0; i < 32; ++i) texldSampler[i] = -1;
  auto regKey = [](uint32_t tok) -> uint32_t { return (dxsoRegType(tok) << 16) | dxsoRegNum(tok); };
  // the taint of one source component (after the source's swizzle)
  auto srcComp = [&](uint32_t tok, uint32_t destComp) -> uint64_t {
    if (dxsoRegType(tok) != kTemp || dxsoRegNum(tok) >= 32) return 0;
    const uint32_t sc = (tok >> (16 + 2 * destComp)) & 3u;
    return tempTaint[dxsoRegNum(tok)][sc];
  };
  // the union of the first n swizzled components of a source (what a reduction reads)
  auto srcFirst = [&](uint32_t tok, uint32_t n) -> uint64_t {
    if (dxsoRegType(tok) != kTemp || dxsoRegNum(tok) >= 32) return 0;
    uint64_t all = 0;
    for (uint32_t c = 0; c < n; ++c) all |= tempTaint[dxsoRegNum(tok)][(tok >> (16 + 2 * c)) & 3u];
    return all;
  };
  dxsoForEach(tokens, count, [&](size_t pos, uint32_t op, uint32_t len) {
    if (op == kDxsoOpDcl && len >= 2) {
      const uint32_t usage = tokens[pos + 1], dest = tokens[pos + 2];
      const uint32_t type = dxsoRegType(dest), n = dxsoRegNum(dest);
      if (type == kSampler && n < 16) out.samplers[n].cube = ((usage >> 27) & 0xFu) == 3u;   // texture type: 2 = 2D, 3 = cube, 4 = volume
      else if (type == kInput && n < 32 && major >= 3 && (usage & 0x1Fu) == kUsageTexcoord) inputTexcoord[n] = (int8_t) ((usage >> 16) & 0xFu);
      else if (type == kTexture && n < 32 && major < 3) inputTexcoord[n] = (int8_t) n;
      return true;
    }
    if (dxsoIsDef(op) || len < 1) return true;
    // which coordinate inputs the shader uses as a normal: NRM of an input, or DP3 of an input
    // with a constant register (the light directions of the rig)
    {
      auto inputTc = [&](uint32_t tok) -> int { const uint32_t ty = dxsoRegType(tok), n = dxsoRegNum(tok); return ((ty == kInput || ty == kTexture) && n < 32) ? (int) inputTexcoord[n] : -1; };
      if (op == 0x24u && len >= 2) {
        const int tc = inputTc(tokens[pos + 2]);
        if (tc >= 0) out.normalTexcoords |= (uint16_t) (1u << tc);
      } else if (op == 0x08u && len >= 3) {
        const uint32_t a = tokens[pos + 2], b = tokens[pos + 3];
        const int ta = inputTc(a), tb = inputTc(b);
        if (ta >= 0 && dxsoRegType(b) == 2u) out.normalTexcoords |= (uint16_t) (1u << ta);
        else if (tb >= 0 && dxsoRegType(a) == 2u) out.normalTexcoords |= (uint16_t) (1u << tb);
      }
    }
    const uint32_t dest = tokens[pos + 1];
    const uint32_t dtype = dxsoRegType(dest), dn = dxsoRegNum(dest);
    const uint32_t mask = (dest >> 16) & 0xFu;
    if (op == 0x41u) {                                                 // TEXKILL reg: what feeds the discard
      if (out.maskSampler >= 0 && dtype == kTemp && dn < 32) {
        for (uint32_t c = 0; c < 4; ++c) if (tempTaint[dn][c] & bit((uint32_t) out.maskSampler, 0)) out.maskKill = true;
      }
      return true;
    }
    uint64_t taint[4] = {};
    if (op == 0x42u && len >= 3) {                                     // TEXLD dest, coord, sampler
      const uint32_t coord = tokens[pos + 2], samp = tokens[pos + 3];
      if (dxsoRegType(samp) == kSampler && dxsoRegNum(samp) < 16) {
        const uint32_t s = dxsoRegNum(samp);
        PsSamplerUse& u = out.samplers[s];
        u.read = true;
        if (((tokens[pos] >> 16) & 0xFFu) & 1u) u.projective = true;   // D3DSI_TEXLD_PROJECT
        const uint32_t ctype = dxsoRegType(coord), cn = dxsoRegNum(coord);
        if ((ctype == kInput || ctype == kTexture) && cn < 32 && inputTexcoord[cn] >= 0) u.texcoord = inputTexcoord[cn];
        else if (ctype == kTemp) u.dependent = true;
        for (uint32_t c = 0; c < 4; ++c) taint[c] = bit(s, c);
        if (dtype == kTemp && dn < 32) { texldSampler[dn] = (int8_t) s; texldCoord[dn] = regKey(coord); }
      }
    } else {
      // the wall shaders' opening test: ADD x, sample.x, coord.z with the sample's own coordinate
      if (op == 0x02u && len >= 3 && out.maskSampler < 0) {
        const uint32_t a = tokens[pos + 2], b = tokens[pos + 3];
        auto pair = [&](uint32_t sample, uint32_t coord) -> bool {
          if (dxsoRegType(sample) != kTemp || dxsoRegNum(sample) >= 32 || ((sample >> 24) & 0xFu) /*a source modifier*/) return false;
          const uint32_t tn = dxsoRegNum(sample);
          return texldSampler[tn] >= 0 && ((sample >> 16) & 0xFFu) == 0x00u /*.x*/ && regKey(coord) == texldCoord[tn] && ((coord >> 16) & 0xFFu) == 0xAAu /*.z*/;
        };
        if (pair(a, b)) out.maskSampler = texldSampler[dxsoRegNum(a)];
        else if (pair(b, a)) out.maskSampler = texldSampler[dxsoRegNum(b)];
      }
      // reductions read a fixed number of components of each source and feed every destination
      // component: DP3 / NRM / CRS three, DP4 four, DP2ADD two (its addend one)
      const uint32_t reduce = (op == 0x08u || op == 0x24u || op == 0x21u) ? 3u : op == 0x09u ? 4u : op == 0x5Au ? 2u : 0u;
      for (uint32_t i = 2; i <= len; ++i) {
        const uint32_t src = tokens[pos + i];
        if (reduce) { const uint64_t all = srcFirst(src, (op == 0x5Au && i == 4) ? 1u : reduce); for (uint32_t c = 0; c < 4; ++c) taint[c] |= all; }
        else for (uint32_t c = 0; c < 4; ++c) taint[c] |= srcComp(src, c);
      }
    }
    if (dtype == kTemp && dn < 32) { for (uint32_t c = 0; c < 4; ++c) if (mask & (1u << c)) tempTaint[dn][c] = taint[c]; }
    else if (dtype == kColorOut && dn < 4) { for (uint32_t c = 0; c < 4; ++c) if (mask & (1u << c)) colorTaint[dn][c] = taint[c]; }
    return true;
  });
  const uint64_t color = colorTaint[0][0] | colorTaint[0][1] | colorTaint[0][2];   // oC0.rgb
  for (uint32_t s = 0; s < 16; ++s) {
    uint8_t n = 0;
    for (uint32_t c = 0; c < 3; ++c) if (color & bit(s, c)) ++n;
    out.samplers[s].colorChannels = n;
  }
  if (out.maskSampler >= 0 && !out.maskKill && (colorTaint[0][3] & bit((uint32_t) out.maskSampler, 0))) out.maskAlpha = true;
  out.valid = true;
  return true;
}

// The albedo for a draw of an untabled pixel shader: among the samplers that reach the colour
// with a coordinate taken straight from an input, and hold a bound 2D colour texture, the one
// scoring highest: larger textures, compressed formats (the game's albedos are DXT; lightmaps,
// ramps and render targets are not), lower coordinate indices. Returns false when none.
inline bool chooseAutoAlbedo(const PsAnalysis& a, const bool color2D[16], const uint32_t fmt[16], const uint16_t w[16], const uint16_t h[16], int& stage, int& texcoord) {
  float best = -1e9f; stage = -1; texcoord = -1;
  for (int s = 0; s < 16; ++s) {
    const PsSamplerUse& u = a.samplers[s];
    if (!u.read || !u.reachesColor() || u.dependent || u.projective || u.cube || u.texcoord < 0) continue;
    if (!color2D[s]) continue;                                    // a 2D colour texture that is not a render target
    const bool compressed = fmt[s] == (uint32_t) D3DFMT_DXT1 || fmt[s] == (uint32_t) D3DFMT_DXT2 || fmt[s] == (uint32_t) D3DFMT_DXT3 || fmt[s] == (uint32_t) D3DFMT_DXT4 || fmt[s] == (uint32_t) D3DFMT_DXT5;
    const float area = (float) w[s] * (float) h[s];
    // all three channels reaching the colour is the signature of an albedo; a mask or a
    // gloss map contributes one channel, and a tie between two candidates goes to the later
    // sampler (the game binds masks below their albedo)
    const float score = std::log2((std::max)(area, 1.f)) + (compressed ? 3.f : 0.f) - 2.f * u.texcoord + 2.f * u.colorChannels + 0.01f * s;
    if (score > best) { best = score; stage = s; texcoord = u.texcoord; }
  }
  return stage >= 0;
}

// ---- which texture coordinates the runtime samples with (milestone 3g) -------------------
// The 1.5.2 runtime takes a draw's texture coordinates from the vertex declaration element
// whose index equals stage 0's D3DTSS_TEXCOORDINDEX, and only when no such element exists
// does it use the vertex shader's captured TEXCOORD0 output. Objects and Sims pass their
// input set through nearly unchanged, so the raw input looks right; the wall shaders keep
// theirs in texel units and divide by 512 in the shader, so the runtime tiled every
// wallpaper five hundred times across a wall. Pointing stage 0's index at an unused set
// (the game never sets it) hides the input and the runtime samples with the shader's own
// output -- which is what the pixel shader samples with, by construction. Applied to the
// families whose TEXCOORD0 output is verified: the promoted ones, and these.
struct CapturedUv { uint64_t hash; const char* name; };

inline const CapturedUv kCapturedUv[] = {
  { 0x0fcdd50823cd0504ull, "floors (pattern UV computed in the shader)" },
  { 0x22e0b0fb83e51c5cull, "floor tiles" },
};

inline bool useCapturedUv(uint64_t hash) {
  return findTexcoordPromote(hash) != nullptr || findByHash(kCapturedUv, hash) != nullptr;
}

// ---- the sun's direction from the shadow map (milestone 3d) ------------------------------
// The rig vote cannot be trusted for the direction: an object's brightest light is a
// different key light indoors, so the majority flips with the view. But every shader that
// reads the 2048x2048 shadow map receives the light's view-projection as four constant
// rows applied to the world position, and the gradients of its x and y clip coordinates
// cross to the light axis -- whichever objects are drawn. The vote then only supplies the
// colour, from the candidate whose direction matches (the trace's sun matched to 1e-3).
// The rows sit at c0..c3 in every known shader.
struct ShadowSource { uint64_t hash; const char* name; };

inline const ShadowSource kShadowSources[] = {
  { 0x55c99586fb17cd1cull, "terrain paint VS 0x15c787a0" },
  { 0x7d1bc3ce6acbd715ull, "objects 0x12d17440" },
  { 0x64154031c30a8800ull, "0xd9c0fe0 vs_3_0" },
  { 0x3c837e49bf748d99ull, "0x164c8ea0 vs_3_0" },
};

// The register of the shader's shadow rows (0), or -1 when the shader has none.
inline int findShadowSource(uint64_t hash) { return findByHash(kShadowSources, hash) ? 0 : -1; }

// rows: the four shadow view-projection rows (16 floats). Returns the unit direction toward
// the light, or false when the rows are degenerate or the light sits at the horizon.
inline bool shadowLightDir(const float* rows, float* out) {
  const float* gx = rows; const float* gy = rows + 4;
  if (len3(gx) < 1e-7f || len3(gy) < 1e-7f) return false;
  const float c[3] = { gx[1]*gy[2]-gx[2]*gy[1], gx[2]*gy[0]-gx[0]*gy[2], gx[0]*gy[1]-gx[1]*gy[0] };
  const float n = len3(c);
  if (n < 1e-14f) return false;
  float d[3] = { c[0]/n, c[1]/n, c[2]/n };
  if (d[1] < 0.f) for (int q = 0; q < 3; ++q) d[q] = -d[q];
  if (d[1] < 0.05f) return false;
  for (int q = 0; q < 3; ++q) out[q] = d[q];
  return true;
}

inline bool sameSun(const SunVote& a, const SunVote& b) {
  return dot3(a.dir, b.dir) > 0.9995f && std::fabs(a.col[0] - b.col[0]) < 0.02f && std::fabs(a.col[1] - b.col[1]) < 0.02f && std::fabs(a.col[2] - b.col[2]) < 0.02f;
}

inline void makeSunLight(const SunVote& v, D3DLIGHT9& l) {
  std::memset(&l, 0, sizeof l);
  l.Type = D3DLIGHT_DIRECTIONAL;
  l.Diffuse.r = v.col[0] > 0.f ? v.col[0] : 0.f;
  l.Diffuse.g = v.col[1] > 0.f ? v.col[1] : 0.f;
  l.Diffuse.b = v.col[2] > 0.f ? v.col[2] : 0.f;
  l.Diffuse.a = 1.f;
  l.Specular = l.Diffuse;
  l.Direction.x = -v.dir[0]; l.Direction.y = -v.dir[1]; l.Direction.z = -v.dir[2];   // D3D: the direction the light travels
}

// ---- the game's own lamp lights (milestone 22) --------------------------------------------
// The game keeps every lamp model's lights in a LITE resource: type, position in the model's
// own space, colour, intensity. sims3/tools/lite_table.py reads the package files and writes
// sims3lights.txt next to this DLL: one line per mesh, keyed by the FNV-1a 64 hash of the
// model's index data as the game uploads it (run 133: the Direct3D index buffers match the
// package chunks byte for byte, differences decoded). At a draw of an object shader the index
// buffer's hash names the model, and the object's World rows carry each light to the world.
struct LiteLight { uint8_t type; float pos[3]; float col[3]; float intensity; };   // type 3 point, 4 spot, 5 lamp shade, 6 tube
struct LiteModel { uint64_t ibHash; uint64_t inst; uint8_t n; LiteLight lights[4]; };
struct LiteTable {
  static const int kMax = 1024;
  LiteModel models[kMax]; uint32_t n = 0; bool loaded = false; uint32_t lines = 0;
};
inline LiteTable& liteTable() {
  static LiteTable t;
  if (t.loaded) return t;
  t.loaded = true;
  char path[MAX_PATH] = {};
  HMODULE self = GetModuleHandleA("d3d9.dll");
  if (!self || !GetModuleFileNameA(self, path, MAX_PATH)) return t;
  char* slash = strrchr(path, '\\');
  if (!slash) return t;
  snprintf(slash + 1, (size_t) (MAX_PATH - (slash + 1 - path)), "sims3lights.txt");
  FILE* f = fopen(path, "rb");
  if (!f) return t;
  char line[1024];
  while (fgets(line, sizeof line, f) && t.n < (uint32_t) LiteTable::kMax) {
    ++t.lines;
    if (line[0] == '#' || line[0] == '\r' || line[0] == '\n' || !line[0]) continue;
    char* e = line;
    LiteModel m = {};
    m.ibHash = strtoull(e, &e, 16); m.inst = strtoull(e, &e, 16);
    const long cnt = strtol(e, &e, 10);
    if (cnt < 1) continue;
    for (long i = 0; i < cnt && m.n < 4; ++i) {
      LiteLight& L = m.lights[m.n];
      const long typ = strtol(e, &e, 10);
      float v[7]; for (int k = 0; k < 7; ++k) v[k] = (float) strtod(e, &e);
      if (typ < 3 || typ > 6) continue;
      L.type = (uint8_t) typ; L.pos[0] = v[0]; L.pos[1] = v[1]; L.pos[2] = v[2]; L.col[0] = v[3]; L.col[1] = v[4]; L.col[2] = v[5]; L.intensity = v[6];
      ++m.n;
    }
    if (m.n) t.models[t.n++] = m;
  }
  fclose(f);
  return t;
}
inline const LiteModel* findLiteModel(uint64_t ibHash) {
  LiteTable& t = liteTable();
  for (uint32_t k = 0; k < t.n; ++k) if (t.models[k].ibHash == ibHash) return &t.models[k];
  return nullptr;
}
// A model-space point through the World rows the object shaders carry (three rows of four:
// row . (x, y, z, 1), the translation in .w).
inline void worldPoint(const float* rows, const float* p, float* out) {
  for (int i = 0; i < 3; ++i) out[i] = rows[4*i] * p[0] + rows[4*i + 1] * p[1] + rows[4*i + 2] * p[2] + rows[4*i + 3];
}

// ---- lamps (milestones 3a, 21, 22, 23) ----------------------------------------------------
// A lamp is one of the game's own lights (the table above): its object's index buffer names the
// model, the model's definition names the light, the object's World rows carry it to the
// world. Whether it is ON comes from the per-object light rig the game uploads with every
// object-shader draw: c0..c3 are unit directions toward the four strongest lights at that
// object, c4..c7 their colours. An object right next to a lamp has that lamp among its
// strongest lights whenever it is on, and not otherwise; a far object's rig is full of other
// lights and proves nothing either way (runs 134-136). So only the objects drawn within two
// units of a lamp's light are its witnesses: one of them pointing at the light says on, and
// gives the colour the player set; one drawn and not pointing at it says off; none drawn
// holds the last state. The game keeps a lamp's own light out of that lamp's rig (run 136),
// so a lamp cannot vouch for itself.
//
// Milestone 25 read a state flag into the lamp's own draw (its s2 texture, its rig); run 139
// refuted it: s2 is the lot's LIGHT MAP, one texture for every object, whose content and hash
// change whenever the lighting changes. Milestone 27 reads that map instead (run 140): the game
// computes it on the CPU and uploads it (256x128 A8R8G8B8, a new version at every lighting
// change), the object shaders project world positions into it with the two constant rows
// after World, and at a lamp's base it reads 64-101 with the lamp on against 7-15 off, its
// surroundings unchanged. So the map's value at a lamp's base, bright and clearly above the
// points around it, says on; it is read for every known lamp whenever the map changes, drawn
// or not, and it overrides the witnesses, who remain the fallback without a readable map.
// Milestone 28 (run 141): the map's word STANDS per lamp until the map says otherwise -- the
// witnesses may no longer overrule it on the frames a lamp is not drawn (off screen, lamps lit
// up and went out by the rays alone) -- and the map is the texture most object draws of the
// frame carry at s2, not whatever the last lamp draw carried (a second texture displaced it
// 63 times in run 141 while the map itself changed 17 times, each time re-judging every lamp
// against the wrong texels). A lamp beyond the map's extent is not judged by it.
// Milestone 29 (run 142): the lot has MORE THAN ONE light map -- a 256x128 one and a 128x128
// one over the western half at the same texel size, both re-uploaded at every lighting change,
// each carried by its own objects -- so the frame's majority map was the wrong map for half the
// lamps half the time. Now each lamp is judged by the map its own draws carry (a shared texture,
// adopted at once or after three consecutive draws with it), every carried map is decoded and
// judges its lamps when its content changes. And the map is coloured: a lamp set to blue lit
// its base in blue and its red plane read dark, so the brightest channel is read, not red.
struct LampRay { float pos[3]; float dir[3]; float col[3]; };

struct Lamp {
  float pos[3];        // the light, in the world (from the game's definition through the object's World rows)
  float col[3];        // colour x brightness as forwarded
  float anchor[3];     // the object's origin
  uint32_t id;         // the object's origin quantised, with the light's index: the lamp's identity across frames (the API light's hash)
  uint8_t kind;        // the light type (3 point, 4 spot, 5 lamp shade, 6 tube)
  uint8_t light;       // which of the model's lights (milestone 24: a lamp is found again by its object's position and this, not by an exact id)
  float base;          // the game's brightness for it: intensity / 100 x the definition's colour luminance
  uint32_t support;    // witness rays pointing at the light this frame
  uint32_t missing;    // grows by two per frame of witnesses saying off
  uint32_t age;        // supported frames (forwarded once >= kConfirmFrames)
  uint32_t unseen;     // consecutive frames its object was not drawn
  uint32_t held;       // frames held for want of witnesses (statistics)
  bool drawn;          // the object was drawn this frame
  int8_t state;        // the light map's standing word for it (milestone 28): 1 on, 0 off, -1 not judged (beyond the map, or none read yet); it stands until the map says otherwise
  void* map;           // the light map texture its draws carry (milestone 29); the device judges it through that map
  void* mapCand;       // another map its draws have carried lately, adopted after kMapConfirm consecutive draws
  uint8_t mapCandN;
  bool confirmedOnce;  // forwarded at least once (the event log)
  void* api;           // the Remix API light handle (the device destroys it on drop)
  float sentPos[3], sentCol[3]; bool sent;   // what the runtime holds
};

// World register of the vertex shaders whose draws may contribute rays (the rig is only
// meaningful with a known object position).
struct WorldReg { uint64_t hash; int reg; };
inline const WorldReg kWorldRegs[] = {
  { 0x0ba6ddb9aa01913cull, 12 },   // objects 0x12d25080
  { 0xc3af2a4a82d84e6eull, 12 },   // objects 0x12d213c0
  { 0x7d1bc3ce6acbd715ull, 16 },   // objects 0x12d17440 (its fused matrix sits at c12..c15)
  { 0x1bd4405f8346ded4ull, 192 },  // objects, skinned (fused matrix at c188..c191)
  { 0x4c1d851f37e3c3f2ull, 196 },  // objects, skinned, 0x10a68220 family (fused matrix at c192..c195)
  { 0xc141e2d46a9df578ull, 16 },   // normal-mapped, alpha-cut object shader with the lot shadow fade (close-zoom sighting)
  { 0x64154031c30a8800ull, 16 },   // 0xd9c0fe0 family (same layout, scrolling UVs)
  { 0x4be4f1463f801b19ull, 16 },   // 0xd9c0fe0 family sibling
};
inline int findWorldReg(uint64_t hash) { const WorldReg* w = findByHash(kWorldRegs, hash); return w ? w->reg : -1; }

inline float lampDist(const float* a, const float* b) {
  const float d[3] = { a[0]-b[0], a[1]-b[1], a[2]-b[2] };
  return len3(d);
}

struct LampSolver {
  static const int kMaxRays = 768;
  static const int kMaxLamps = 48;
  static const int kMaxOrigins = 512;
  static const uint32_t kMissingLimit = 180;   // off after a second and a half of witnesses saying so (missing grows by two per frame)
  static const uint32_t kUnseenLimit = 600;    // released after ten seconds undrawn
  static const uint32_t kConfirmFrames = 3;    // supported frames before a lamp is forwarded
  static constexpr float kWitness = 2.f;       // a witness stands this close to the light in the ground plane, from 5 units below it to 3 above (a ceiling light hangs high)
  static constexpr float kClose = 1.f;         // this close, an object lists the lamp among its lights whenever it is on: its silence says off
  static constexpr float kAimCos = 0.85f;      // a witness ray points at the light when it aims within ~32 degrees of it (the rig is evaluated at the
                                               // object's centre, the ray traced from its origin: half a unit apart on a near object, milestone 24)
  LampRay rays[kMaxRays]; uint32_t nRays = 0;             // this frame's rig rays
  float origins[kMaxOrigins][3]; uint8_t originSlots[kMaxOrigins]; uint32_t nOrigins = 0;   // this frame's object origins (every object-shader draw) and how many of the rig's four slots their rig used
  Lamp lamps[kMaxLamps]; uint32_t nLamps = 0;
  uint32_t created = 0, dropped = 0;                      // statistics
  void* droppedApi[kMaxLamps]; uint32_t nDropped = 0;     // the API lights of the lamps dropped this frame, for the device to destroy
  struct Event { uint8_t kind; float anchor[3]; float y; float col[3]; uint32_t votes, age; };   // 1 lit (first forwarded), 2 dropped
  Event events[16]; uint32_t nEvents = 0;

  // An origin quantised to a quarter unit, hashed: the same object gives the same id.
  static uint32_t originId(const float* p) {
    const int32_t q[3] = { (int32_t) std::floor(p[0] * 4.f + 0.5f), (int32_t) std::floor(p[1] * 4.f + 0.5f), (int32_t) std::floor(p[2] * 4.f + 0.5f) };
    uint32_t h = 2166136261u;
    for (int i = 0; i < 3; ++i) for (int b = 0; b < 4; ++b) { h ^= (uint32_t) ((q[i] >> (8 * b)) & 0xFF); h *= 16777619u; }
    return h ? h : 1u;
  }

  // One captured draw of a rig shader: the object's world position and its rig (c0..c3
  // directions toward the lights, c4..c7 colours). sunDir (unit, may be null) is skipped.
  void add(const float* pos, const float* dirs, const float* cols, const float* sunDir) {
    bool known = false;   // the same object's further draws
    for (uint32_t k = nOrigins > 8 ? nOrigins - 8 : 0; k < nOrigins && !known; ++k) known = lampDist(origins[k], pos) < 1e-3f;
    uint8_t used = 0;
    for (int i = 0; i < 4; ++i) { const float* d = dirs + i*4; const float len = len3(d); if (len >= 0.9f && len <= 1.1f && luminance(cols + i*4) >= 0.02f) ++used; }
    if (!known && nOrigins < (uint32_t) kMaxOrigins) { for (int j = 0; j < 3; ++j) origins[nOrigins][j] = pos[j]; originSlots[nOrigins] = used; ++nOrigins; }
    for (int i = 0; i < 4; ++i) {
      const float* d = dirs + i*4; const float* c = cols + i*4;
      const float len = len3(d);
      if (len < 0.9f || len > 1.1f || luminance(c) < 0.02f) continue;
      const float nd[3] = { d[0]/len, d[1]/len, d[2]/len };
      if (sunDir && dot3(sunDir, nd) > 0.999f) continue;
      bool dup = false;   // the same object's earlier draws carry the same rig
      for (uint32_t k = nRays > 4 ? nRays - 4 : 0; k < nRays && !dup; ++k)
        dup = lampDist(rays[k].pos, pos) < 1e-3f && dot3(rays[k].dir, nd) > 0.9999f;
      if (dup) continue;
      if (nRays >= (uint32_t) kMaxRays) return;
      LampRay& r = rays[nRays++];
      for (int j = 0; j < 3; ++j) { r.pos[j] = pos[j]; r.dir[j] = nd[j]; r.col[j] = c[j]; }
    }
  }

  // A lamp from the game's definitions, once per draw of its object: the light's exact world
  // position; the definition's colour and intensity until the witnesses give the colour.
  // ...`state` is what the object's own draw says (milestone 25): 1 on, 0 off, -1 nothing readable this draw.
  static const uint8_t kMapConfirm = 3;   // consecutive draws with another map before a lamp changes map (a one-off texture cannot steal it)
  // The light map a lamp's draws carry (milestone 29): adopted at once when it has none, else only
  // after kMapConfirm consecutive draws with the same other map. True when the lamp's map changed
  // (from = the map it had).
  bool mapLamp(uint8_t light, const float* anchor, void* map, void** from) {
    for (uint32_t k = 0; k < nLamps; ++k) {
      Lamp& L = lamps[k];
      if (L.light != light || lampDist(L.anchor, anchor) > 0.5f) continue;
      if (L.map == map) { L.mapCand = nullptr; L.mapCandN = 0; return false; }
      if (L.map == nullptr || (L.mapCand == map && ++L.mapCandN >= kMapConfirm)) {
        if (from) *from = L.map;
        L.map = map; L.state = -1; L.mapCand = nullptr; L.mapCandN = 0;
        return true;
      }
      if (L.mapCand != map) { L.mapCand = map; L.mapCandN = 1; }
      return false;
    }
    return false;
  }
  const Lamp* find(uint8_t light, const float* anchor) const {
    for (uint32_t k = 0; k < nLamps; ++k) if (lamps[k].light == light && lampDist(lamps[k].anchor, anchor) <= 0.5f) return &lamps[k];
    return nullptr;
  }
  void addModelLamp(uint32_t id, uint8_t light, const float* anchor, const float* pos, const float* col, float intensity, uint8_t kind, int8_t state) {
    for (uint32_t k = 0; k < nLamps; ++k) {
      Lamp& L = lamps[k];
      if (L.light != light || lampDist(L.anchor, anchor) > 0.5f) continue;   // the same object (its position may jitter across the id's quantisation)
      for (int q = 0; q < 3; ++q) { L.anchor[q] = anchor[q]; L.pos[q] = pos[q]; }
      L.drawn = true; if (state >= 0) L.state = state;
      return;
    }
    if (nLamps >= (uint32_t) kMaxLamps) return;
    Lamp& L = lamps[nLamps++];
    std::memset(&L, 0, sizeof L);
    for (int q = 0; q < 3; ++q) { L.anchor[q] = anchor[q]; L.pos[q] = pos[q]; L.col[q] = col[q] * intensity / 100.f; }
    L.base = luminance(col) * intensity / 100.f;
    L.id = id; L.light = light; L.kind = kind; L.drawn = true; L.state = state;
    ++created;
  }

  // Is the object at o a witness of L: within kWitness of its light in the ground plane, from
  // 5 units below it to 3 above, and not the lamp's own object?
  static bool witness(const float* o, const Lamp& L) {
    const float dx = o[0] - L.pos[0], dy = o[1] - L.pos[1], dz = o[2] - L.pos[2];
    return dy > -5.f && dy < 3.f && dx*dx + dz*dz < kWitness * kWitness && lampDist(o, L.anchor) > 0.3f;
  }
  // Can the witness at o testify that L is off? Only if the lamp would surely be among its four
  // listed lights when on: it stands within kClose of the light, or its rig has a free slot.
  static bool canTestify(const float* o, uint8_t slots, const Lamp& L) {
    const float dx = o[0] - L.pos[0], dz = o[2] - L.pos[2];
    return slots < 4 || dx*dx + dz*dz < kClose * kClose;
  }
  // Does ray r aim at point P: within ~32 degrees of the direction to it?
  static bool pointsAt(const LampRay& r, const float* P) {
    const float v[3] = { P[0] - r.pos[0], P[1] - r.pos[1], P[2] - r.pos[2] };
    const float n = len3(v);
    return n > 0.2f && dot3(v, r.dir) / n > kAimCos;
  }
  void record(uint8_t kind, const Lamp& L, uint32_t votes) {
    if (nEvents >= 16) return;
    Event& e = events[nEvents++];
    e.kind = kind; for (int q = 0; q < 3; ++q) { e.anchor[q] = L.anchor[q]; e.col[q] = L.col[q]; }
    e.y = L.pos[1]; e.votes = votes; e.age = L.age;
  }

  // Frame end: the light map's standing word decides each judged lamp (the witnesses' rays give
  // the colour); a lamp the map has not judged goes by its witnesses. The lamps gone are handed
  // to the device.
  uint32_t solve() {
    nDropped = 0; nEvents = 0;
    for (uint32_t k = 0; k < nLamps; ++k) {
      Lamp& L = lamps[k];
      uint32_t pointing = 0; float sum[3] = {};
      for (uint32_t i = 0; i < nRays; ++i)
        if (witness(rays[i].pos, L) && pointsAt(rays[i], L.pos)) { ++pointing; for (int q = 0; q < 3; ++q) sum[q] += rays[i].col[q]; }
      L.support = pointing;
      uint32_t witnesses = 0;
      for (uint32_t o = 0; o < nOrigins; ++o) if (witness(origins[o], L) && canTestify(origins[o], originSlots[o], L)) ++witnesses;
      // the verdict: the map's standing word when it has one; else the witnesses -- one pointing
      // at the light says on, one able to testify and not pointing says off, none holds
      const int8_t verdict = L.state >= 0 ? L.state : (pointing ? (int8_t) 1 : (witnesses ? (int8_t) 0 : (int8_t) -1));
      if (verdict == 1) {
        L.missing = 0;
        if (L.age < 100000u) ++L.age;
        // the hue: the witnesses' rays (the player's colour choice); the brightness the game's
        if (luminance(sum) > 1e-6f) { const float hl = luminance(sum); const float sc = L.age <= 1 ? 1.f : 0.2f; for (int q = 0; q < 3; ++q) L.col[q] += sc * (sum[q] / hl * L.base - L.col[q]); }
        if (L.age == kConfirmFrames && !L.confirmedOnce) { L.confirmedOnce = true; record(1, L, pointing); }
      } else if (verdict == 0) {
        L.missing += L.state == 0 ? 6u : 2u;   // the map's word: out within half a second; the witnesses': a second and a half
      } else {
        ++L.held;         // nothing says either way: the last state holds
      }
      L.unseen = L.drawn ? 0 : L.unseen + 1;
      L.drawn = false;   // the map's word stands (milestone 28): undrawn, a judged lamp is not handed to the witnesses
    }
    for (uint32_t k = 0; k < nLamps; ) {
      Lamp& L = lamps[k];
      if (L.missing > kMissingLimit || (L.unseen > kUnseenLimit && L.state < 0)) {
        if (L.confirmedOnce) record(2, L, L.missing > kMissingLimit ? 1u : 0u);
        droppedApi[nDropped++] = L.api;
        L = lamps[nLamps - 1]; --nLamps; ++dropped;
      }
      else ++k;
    }
    nRays = 0; nOrigins = 0;
    return nLamps;
  }
};

inline uint64_t fnv1a64(const void* bytes, size_t len) {
  const unsigned char* p = static_cast<const unsigned char*>(bytes);
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
  return h;
}

// Hash a D3D9 shader token stream from its version token through the END token, inclusive
// (0 for a stream without an END token).
inline uint64_t shaderHash(const DWORD* tokens) {
  const size_t n = shaderTokenCount(tokens);
  return n ? fnv1a64(tokens, n * sizeof(DWORD)) : 0;
}

inline const ShaderPatch* findShaderPatch(uint64_t hash) { return findByHash(kShaderPatches, hash); }

inline bool patchIntersects(const ShaderPatch* p, UINT startRegister, UINT count) {
  if (!p) return false;
  for (uint32_t i = 0; i < p->count; ++i)
    if (p->patches[i].reg >= startRegister && p->patches[i].reg < startRegister + count) return true;
  return false;
}

// Rewrites the patched components inside an upload of `count` float4 registers starting at
// `startRegister`. Returns how many components were rewritten.
inline uint32_t applyPatches(const ShaderPatch* p, UINT startRegister, float* data, UINT count) {
  uint32_t n = 0;
  if (!p) return 0;
  for (uint32_t i = 0; i < p->count; ++i) {
    const ConstPatch& c = p->patches[i];
    if (c.reg < startRegister || c.reg >= startRegister + count) continue;
    data[(c.reg - startRegister) * 4 + c.comp] = c.value;
    ++n;
  }
  return n;
}

} // namespace sims3cam

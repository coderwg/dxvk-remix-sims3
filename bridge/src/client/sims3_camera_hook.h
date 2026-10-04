#pragma once
/*
 * The Sims 3 camera hook for the RTX Remix bridge client (milestones 1-153).
 *
 * The Sims 3 never calls IDirect3DDevice9::SetTransform (not once in the traced frames). Its vertex
 * shaders read a constant block: a fused World*View*Projection (four registers, column-vector
 * convention, one per matrix row), usually the object's World as a 3x4 right after it, and the
 * camera's world-space eye position as a float4 (x, y, z, 1) further on in the same block. Where
 * the fused matrix sits depends on the shader family, always a multiple of four registers from the
 * upload's start (the in-world trace): c0 (the terrain, much of the lot), c4 (the walls), c8 and
 * c12 (others), c180 and c188 (the Sims and the objects). DXVK-Remix derives its camera solely
 * from D3DTS_VIEW / D3DTS_PROJECTION and treats an identity projection as "no camera", so stock
 * Remix captures nothing.
 *
 * This hook recovers View and Projection from those uploads and forwards them via SetTransform;
 * Remix's vertex capture then places geometry from the vertex-shader output through
 * inverse(View*Projection). D3DTS_WORLD stays the identity, except for glass: its object's place
 * (milestone 119), so that a moving glass keeps its motion.
 *
 * Disambiguation (learned from run 1): a projection composed with any rigid World is
 * still a perfectly valid-looking camera, just in object space. The only way to tell
 * View*Projection from View*Projection*World is a world-space anchor, and the game
 * provides one: the eye position it uploads for specular lighting. A candidate is
 * accepted only if the camera position it implies equals a float4 in the same upload --
 * or, for blocks the game sends without the eye (the lot terrain's), with its World
 * un-multiplied and as the play camera continued (see continuesCamera).
 *
 * Classification of an upload:
 *   Main       - verified camera with a proper basis (det +1): the play camera, whichever
 *                way it pitches                                  -> forward View/Projection
 *   Reflection - verified camera with a mirrored basis (det -1): a reflection pass (the
 *                sea/pool pass, a wall mirror's stencil pass)    -> its draws are dropped
 *   None       - anything else (object-space fusions, unknown layouts, the UI's pixel-to-
 *                clip scale) -> leave as is: those draws are still rendered by the main
 *                camera and un-project correctly, or fail the per-draw 3D tests
 *
 * The ray tracer renders reflections itself, so nothing of a reflection pass is sent on: its
 * 3D draws are dropped on the client (recognised by the mirrored camera). The passes whose
 * result Remix never shows -- the shadow map, the sky's environment cube, the water's
 * reflection target -- are dropped whole (milestone 124, unshownPass), and so are the draws
 * named in kDropPs by their pixel shader (milestones 148, 149): the game's own fakes, the world
 * draws Remix must not have and the blended copies drawn in the world. Any other draw that is not
 * captured gets the identity
 * transforms: into another target, or after the interface, the runtime rasterizes it as the
 * game drew it. (A 3D draw on the screen before the interface the runtime does NOT rasterize:
 * it traces it with an unknown camera, rtx.skipObjectsWithUnknownCamera being off -- junk at the
 * world origin -- which is why such draws are not sent at all.)
 *
 * Toggle with the SIMS3_CAMERA_HOOK environment variable (unset/1 = on; 0, f or n = off).
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <d3d9.h>

namespace sims3cam {

struct M4 { float m[4][4]; };  // m[row][col], column-vector convention (M * v)

enum class Kind { None, Main, Reflection };

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
  bool continued = false;   // taken without its eye in the upload, as the play camera continued (milestone 123)
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
inline bool eyePresent(const float* c, unsigned count, const float pos[3], unsigned from = 4) {
  if (len3(pos) < 2.f) return false;
  for (unsigned r = from; r < count; ++r) {
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
inline Kind kindOfVerified(const Camera& cam) { return cam.mirrored ? Kind::Reflection : Kind::Main; }

// Candidate A at `m`: the fused matrix with the World in the three registers after it, un-multiplied
// (VP = WVP * W^-1).
inline bool unWorld(const float* m, M4& VP) {
  M4 WVP, Winv;
  float w[3][4];
  for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) WVP.m[r][col] = m[r*4 + col];
  for (int r = 0; r < 3; ++r) for (int col = 0; col < 4; ++col) w[r][col] = m[16 + r*4 + col];
  if (!invAffine(w, Winv)) return false;
  mul(WVP, Winv, VP);
  return true;
}

// The fused matrix at register `base` of an upload: candidate A with the World in the three registers
// after it, candidate B with an identity World. Either is a camera only if its eye is a float4
// further on in the same upload.
inline Kind classifyAt(const float* c, unsigned count, unsigned base, Camera& cam) {
  const float* m = c + base*4;
  M4 VP;
  if (base + 7 <= count && unWorld(m, VP) && decompose(VP, cam) && eyePresent(c, count, cam.pos, base + 4)) return kindOfVerified(cam);
  M4 WVP;
  for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) WVP.m[r][col] = m[r*4 + col];
  if (decompose(WVP, cam) && eyePresent(c, count, cam.pos, base + 4)) return kindOfVerified(cam);   // candidate B
  return Kind::None;
}

// A camera without its eye in the upload (milestone 123). Run 226: the lot terrain's block (VS
// 976b73db, c0..c10) carries the fused matrix and the World but no eye, and was drawn first after
// the water's reflection pass. Such a camera is taken only with its World un-multiplied (candidate
// A: not an object-space camera) and only as the play camera continued: a proper basis, the lens of
// the last camera verified by its eye, its eye within a frame's travel of that one's and looking
// the same way. Run 70's sky-dome phantom (candidate A, the play camera's lens, its eye at the
// world origin) fails the distance. In the in-world trace every candidate-A camera that passes the
// lens tests, with or without its eye, is the play camera or a reflection.
inline bool continuesCamera(const Camera& cam, const Camera& last) {
  const float d[3] = { cam.pos[0] - last.pos[0], cam.pos[1] - last.pos[1], cam.pos[2] - last.pos[2] };
  return !cam.mirrored && std::fabs(cam.fovY - last.fovY) < 1e-3f && std::fabs(cam.aspect - last.aspect) < 1e-3f
      && len3(d) < 50.f && dot3(cam.fwd, last.fwd) > 0.9f;
}

// c = the constant floats of one upload; count = its number of float4 registers. The fused matrix
// is tried at every fourth register (milestone 122): run 225's stale draws were walls drawn first
// after the water's reflection pass, their matrix at c4, which the c0-only reading never saw. Only
// when no place verifies by its eye, a candidate A continuing `last` (the last camera verified by
// its eye) is taken as the main camera (milestone 123).
inline Kind classify(const float* c, unsigned count, Camera& cam, const Camera* last = nullptr) {
  cam.continued = false;
  for (unsigned base = 0; base + 4 <= count; base += 4) {
    const Kind k = classifyAt(c, count, base, cam);
    if (k != Kind::None) return k;
  }
  if (last) {
    for (unsigned base = 0; base + 7 <= count; base += 4) {
      M4 VP;
      if (unWorld(c + base*4, VP) && decompose(VP, cam) && continuesCamera(cam, *last)) { cam.continued = true; return Kind::Main; }
    }
  }
  return Kind::None;
}

// What the runtime currently holds. Kind::None here means the identity transforms (the game sets
// none of its own).
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

// A 3D draw of a reflection pass (the ray tracer renders reflections itself): the last camera upload
// was a mirrored one -- the water's reflection, a wall mirror's pass at close zoom (run 66), caught by
// kindOfVerified. (Milestones 66 and 85 also told a mirror's pass by its render states -- the stencil
// with the winding flipped -- and ended it at a draw culling clockwise; with the camera read from
// every upload since milestones 122-123 neither ever acted, a close-zoom wall mirror included, run
// 248: removed in milestone 147.)
inline bool isReflectionDraw(bool cameraMirrored, bool declIs3D, DWORD zEnable) {
  return declIs3D && zEnable != D3DZB_FALSE && cameraMirrored;
}

// A pass whose result Remix never shows (milestone 124): the ray tracer makes its own shadows, sky
// light and reflections, so these draws are dropped whole. The in-world trace, per frame: the shadow
// map (a 2048x2048 target written with colour writes off: depth only) ~72 draws, the water's
// reflection (its own 512x512 target, the one a reflection camera draws into) ~53, one face of the
// sky's environment cube (64x64, a cube map's face) ~5-12; the screen ~226. Only offscreen targets.
// The game's shadow setting stays on: with it off the game draws its terrain with other shaders,
// the ones the sun is read from.
enum UnshownPass { kShadowMapPass = 0, kSkyCubePass = 1, kWaterReflectionPass = 2, kUnshownPasses = 3 };
inline int unshownPass(bool rtIsPrimary, unsigned rtW, unsigned rtH, DWORD colourWrites, bool rtCubeFace, bool rtReflection) {
  if (rtIsPrimary) return -1;
  if (rtCubeFace) return kSkyCubePass;
  if (rtReflection) return kWaterReflectionPass;
  if ((colourWrites & 0xF) == 0 && rtW == rtH && rtW >= 256) return kShadowMapPass;
  return -1;
}

// A 3D draw of the main pass: a verified camera, a 3D position layout, depth testing on, the
// primary render target, and a viewport over the whole of it. Run 237: the action menu's portrait
// of the active Sim is drawn after the interface into a 256x256 viewport of the screen, through a
// camera of its own; the runtime rasterizes it over the traced picture, so it stays exactly as the
// game draws it (milestone 133; as a world draw it got the hook's variants and textures: broken).
inline bool drawIs3D(bool cameraValid, bool declIs3D, DWORD zEnable, bool rtIsPrimary, bool fullViewport) {
  return cameraValid && declIs3D && zEnable != D3DZB_FALSE && rtIsPrimary && fullViewport;
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
// The engine hides geometry inside its vertex shaders: the lot terrain shader moves vertices
// outside a rectangle to a far clip position. The game's rasterizer draws nothing for such
// vertices, but Remix's vertex capture reconstructs them onto the focal axis at the near plane:
// triangles from real vertices to that point are the fans converging on the screen centre, and
// the giant sheets, seen in runs 1-5. Patching the constants makes every vertex a normal point
// and terrain chunks complete. (The wall shaders hide vertices the same way, by a per-vertex
// visibility factor -- the cut-away view; since milestone 13 the wall cut drops those triangles
// instead, sims3_walls.h.)
// Shaders are recognised by an FNV-1a-64 hash of their token stream; the hashes below were
// taken from the game's own shaders in the trace (shaders/hashes.txt).
struct ConstPatch { uint16_t reg; uint8_t comp; float value; };
struct ShaderPatch { uint64_t hash; const char* name; ConstPatch patches[4]; uint32_t count; };

// The lot terrain's vertex shader (a lot's further chunk copies are baked as hidden layer passes, see LotCopies).
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
// (kAlbedoStages), not merely with the first colour texture: six earlier rules that
// pointed at normal or lighting maps (floor, walls B, two door/window families, 0xddc1880,
// and 0x164d8920 = 9f227c82, whose TEXCOORD1 is its pixel shader's light map, milestone 76)
// were removed after reading the pixel shaders' arithmetic.
inline const TexcoordPromote kTexcoordPromotes[] = {
  { 0x0ba6ddb9aa01913cull, "objects 0x12d25080 vs_3_0 (diffuse s3 on TEXCOORD2)", 2 },
  { 0xc3af2a4a82d84e6eull, "objects 0x12d213c0 vs_3_0 (diffuse s3 on TEXCOORD2)", 2 },
  { 0x7d1bc3ce6acbd715ull, "objects 0x12d17440 vs_3_0 (diffuse s2 on TEXCOORD2)", 2 },
  // 0x10a51180 (floors): the pattern is on TEXCOORD0, no promotion (see kAlbedoStages)
  // in-game variants (run-15 shader dump)
  { 0x1bd4405f8346ded4ull, "objects, skinned (diffuse s3 on TEXCOORD2)", 2 },
  { 0x4c1d851f37e3c3f2ull, "objects, skinned, 0x10a68220 family (diffuse s2 on TEXCOORD2)", 2 },
  { 0x4be4f1463f801b19ull, "0xd9c0fe0 family sibling (diffuse s1 on TEXCOORD2)", 2 },
  { 0x24ef09fb3303a9d0ull, "outer ground / water sibling (pattern tile s2 on TEXCOORD2)", 2 },
  { 0xe228d963a38f3e41ull, "vs_2_0 e228d963 (diffuse s1 on TEXCOORD2)", 2 },
  { 0x23072b72226654bdull, "0x164cc3c0 vs_3_0", 5 },
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
// Only an output the pixel shader reads counts (milestone 90): the town's water VS 2a6edce6
// decodes its NORMAL input into TEXCOORD7, which its pixel shader never reads -- for water those
// bytes are no normal, and the runtime shaded the ponds with a near-sideways normal, an even white
// (runs 194-197). Of 154 shader pairs with a normal input in runs 183-197, four had such a dead
// output: the water, one wall pair (now triangle normals) and f10077e8, whose pixel shader reads
// its other candidate.
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

// The candidate to use, among those the pixel shader reads (psInputTexcoords; all when not known):
// the one it treats as a normal when that is known, else the lowest. -1 when none is left.
inline int chooseNormalTexcoord(const VsNormalInfo& v, uint16_t psNormalTexcoords, uint16_t psInputTexcoords = 0xFFFFu) {
  const uint16_t read = (uint16_t) (v.candidates & psInputTexcoords);
  if (!v.valid || !read) return -1;
  uint16_t pick = (uint16_t) (read & psNormalTexcoords);
  if (!pick) pick = read;
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

// ---- the constants a vertex shader hands its pixel shader unchanged (milestone 135) ---------
// Per interpolated component (semantic * 4 + component: TEXCOORD0-15 0-63, COLOR0-1 64-71), the
// float constant the vertex shader copies into it with a plain MOV, register * 4 + component, or
// -1. A relatively addressed copy (c2[a0.w].y: shader instancing, three registers per instance)
// names its base register -- the first instance's -- and is marked rel. SpeedTree hands each
// tree's fade over this way.
inline constexpr int kSemColor0 = 16;
struct VsConstantOutputs { int16_t c[72]; bool rel[72]; VsConstantOutputs() { for (int16_t& v : c) v = -1; for (bool& r : rel) r = false; } };

inline bool analyzeVertexConstantOutputs(const DWORD* tokens, size_t count, VsConstantOutputs& out) {
  out = VsConstantOutputs();
  if (!dxsoIsVertexShader(tokens, count, 2)) return false;
  const bool vs3 = dxsoIsVertexShader(tokens, count, 3);
  const uint32_t kAttrOut = 5u;                                         // vs_2_x oD#
  int8_t sem[16];
  for (int i = 0; i < 16; ++i) sem[i] = vs3 ? (int8_t) -1 : (int8_t) i;  // vs_2_x: oT# is TEXCOORD#
  bool flow = false;
  dxsoForEach(tokens, count, [&](size_t pos, uint32_t op, uint32_t len) {
    if (op == kDxsoOpDcl) {
      uint32_t usage, idx, reg;
      if (vs3 && dxsoDcl(tokens, pos, len, kDxsoRegOutput, usage, idx, reg) && reg < 16)
        sem[reg] = usage == kUsageTexcoord ? (int8_t) idx : (usage == kUsageColor && idx < 2) ? (int8_t) (kSemColor0 + idx) : (int8_t) -1;
      return true;
    }
    if (dxsoIsDef(op) || len < 1) return true;
    if ((op >= 0x19u && op <= 0x1Eu) || (op >= 0x26u && op <= 0x2Du) || op == 0x60u) { flow = true; return false; }   // call / loop / rep / if: not followed
    const uint32_t dest = tokens[pos + 1], ty = dxsoRegType(dest), n = dxsoRegNum(dest), mask = (dest >> 16) & 0xFu;
    int s = -1;
    if (ty == kDxsoRegOutput && n < 16) s = sem[n];
    else if (!vs3 && ty == kAttrOut && n < 2) s = kSemColor0 + (int) n;
    if (s < 0) return true;
    const uint32_t src = len >= 2 ? tokens[pos + 2] : 0u;
    const bool copy = op == 0x01u && len >= 2 && dxsoRegType(src) == 2u && ((src >> 24) & 0xFu) == 0u && ((dest >> 20) & 0xFu) == 0u;   // MOV o, c: no source or result modifier
    for (uint32_t c = 0; c < 4; ++c) if (mask & (1u << c)) {
      out.c[s * 4 + c] = copy ? (int16_t) (dxsoRegNum(src) * 4 + ((src >> (16 + 2 * c)) & 3u)) : (int16_t) -1;
      out.rel[s * 4 + c] = copy && (src & (1u << 13)) != 0u;
    }
    return true;
  });
  if (flow) out = VsConstantOutputs();
  return !flow;
}

// ---- leaf cards fixed in the world (milestone 136) -----------------------------------------
// SpeedTree's leaves are cards turned to the camera. The vertex shader takes each corner's offset
// in the card's own plane and turns it into the world with the camera's three axes -- one DP3 per
// world axis against three consecutive constants, each the camera's right, up and back for that
// axis (VS 799a26fa: dp3 r2.x / r2.y / r2.z, r1.xwyw, c121 / c122 / c123) -- then adds it to the
// leaf's centre (add r1.xyz, r2, r1). Traced, every leaf turns as the camera turns: its shadow
// and reflections move with the view and the denoiser smears the turning cards (run 240). The
// variant turns the offset with a basis of the leaf's own: the card faces outward from its tree
// -- its back axis the centre's direction from the tree's origin, its right axis level -- the same
// for the card's four corners, fixed in the world. The pattern is read from the bytecode
// (findCameraCard); at draw time the three constants must be the frame camera's axes
// (cardBasisIsCamera) before the variant is used.
struct CameraCard { bool valid = false; uint16_t basisReg = 0; };
struct CardSite {
  size_t dp3[3] = {}, consumer = 0;   // token positions: the DP3s writing x, y, z; the ADD that takes the offset
  uint32_t offsetSrc = 0;             // the DP3s' common source token: the corner's offset in the card's plane, swizzled
  uint32_t dst = 0, centre = 0;       // the offset's temp register; the centre's, the ADD's other operand
  uint16_t basisReg = 0;              // the first of the three constants
  uint32_t freeTemp[4] = {};          // four temps the shader never names (of vs_2_0's r0-r11)
  uint32_t freeConst = 0;             // a constant it never names (below c255, the hook's tag read)
};
inline bool findCameraCard(const DWORD* t, size_t n, CardSite& s) {
  s = CardSite();
  if (!dxsoIsVertexShader(t, n, 2)) return false;
  bool usedTemp[12] = {}, usedConst[256] = {}, flow = false;
  struct Dp3 { size_t pos; uint32_t dst, comp, src, c; };
  Dp3 dps[64]; uint32_t ndp = 0;
  auto plainSrc = [](uint32_t p) { return ((p >> 24) & 0xFu) == 0u && !(p & (1u << 13)); };   // no modifier, no relative address
  dxsoForEach(t, n, [&](size_t pos, uint32_t op, uint32_t len) {
    if (op == kDxsoOpDcl) return true;
    if (dxsoIsDef(op)) { if (op == 0x51u && len >= 1 && dxsoRegNum(t[pos + 1]) < 256) usedConst[dxsoRegNum(t[pos + 1])] = true; return true; }
    if ((op >= 0x19u && op <= 0x1Eu) || (op >= 0x26u && op <= 0x2Du) || op == 0x60u) flow = true;
    for (uint32_t i = 1; i <= len; ++i) {
      const uint32_t p = t[pos + i];
      if (!(p & 0x80000000u)) continue;
      const uint32_t ty = dxsoRegType(p), r = dxsoRegNum(p);
      if (ty == kDxsoRegTemp && r < 12) usedTemp[r] = true;
      else if (ty == 2u && r < 256) usedConst[r] = true;
    }
    if (op == 0x08u && len == 3 && ndp < 64) {                        // DP3 rD.<x|y|z>, rS.<swizzle>, c#
      const uint32_t d = t[pos + 1], a = t[pos + 2], b = t[pos + 3], m = (d >> 16) & 0xFu;
      if (dxsoRegType(d) == kDxsoRegTemp && dxsoRegNum(d) < 12 && ((d >> 20) & 0xFu) == 0u && (m == 1u || m == 2u || m == 4u)
          && dxsoRegType(a) == kDxsoRegTemp && plainSrc(a) && dxsoRegType(b) == 2u && plainSrc(b) && ((b >> 16) & 0xFFu) == 0xE4u)
        dps[ndp++] = { pos, dxsoRegNum(d), m == 1u ? 0u : m == 2u ? 1u : 2u, a, dxsoRegNum(b) };
    }
    return true;
  });
  if (flow) return false;
  // three DP3s into one temp's x, y, z, from one source, against c[B], c[B+1], c[B+2]
  int found[3] = { -1, -1, -1 };
  for (uint32_t i = 0; i < ndp && found[0] < 0; ++i) {
    if (dps[i].comp != 0u) continue;
    int f[3] = { (int) i, -1, -1 };
    for (uint32_t j = 0; j < ndp; ++j)
      for (uint32_t k = 1; k < 3; ++k)
        if (dps[j].comp == k && dps[j].dst == dps[i].dst && dps[j].src == dps[i].src && dps[j].c == dps[i].c + k && f[k] < 0) f[k] = (int) j;
    if (f[1] >= 0 && f[2] >= 0) { found[0] = f[0]; found[1] = f[1]; found[2] = f[2]; }
  }
  if (found[0] < 0) return false;
  for (int k = 0; k < 3; ++k) s.dp3[k] = dps[found[k]].pos;
  s.dst = dps[found[0]].dst; s.offsetSrc = dps[found[0]].src; s.basisReg = (uint16_t) dps[found[0]].c;
  const uint32_t srcReg = dxsoRegNum(s.offsetSrc);
  const size_t first = (std::min)(s.dp3[0], (std::min)(s.dp3[1], s.dp3[2])), last = (std::max)(s.dp3[0], (std::max)(s.dp3[1], s.dp3[2]));
  // between them the source is not written; until the ADD the offset's x, y, z are neither read nor written
  bool ok = true;
  auto readsXyz = [](uint32_t p) { for (uint32_t c = 0; c < 4; ++c) if (((p >> (16 + 2 * c)) & 3u) < 3u) return true; return false; };
  dxsoForEach(t, n, [&](size_t pos, uint32_t op, uint32_t len) {
    if (pos <= first || op == kDxsoOpDcl || dxsoIsDef(op) || len < 1) return true;
    if (pos == s.dp3[0] || pos == s.dp3[1] || pos == s.dp3[2]) return true;
    const uint32_t d = t[pos + 1];
    const bool writesTemp = (d & 0x80000000u) && dxsoRegType(d) == kDxsoRegTemp;
    if (pos < last && writesTemp && dxsoRegNum(d) == srcReg) { ok = false; return false; }
    bool reads = false;
    for (uint32_t i = 2; i <= len; ++i) { const uint32_t p = t[pos + i]; if ((p & 0x80000000u) && dxsoRegType(p) == kDxsoRegTemp && dxsoRegNum(p) == s.dst && readsXyz(p)) reads = true; }
    if (reads) {
      if (pos < last || op != 0x02u || len != 3) { ok = false; return false; }
      const uint32_t a = t[pos + 2], b = t[pos + 3];
      const uint32_t other = dxsoRegNum(a) == s.dst ? b : a, mine = dxsoRegNum(a) == s.dst ? a : b;
      if (!writesTemp || ((d >> 16) & 7u) != 7u || ((d >> 20) & 0xFu) != 0u || !plainSrc(mine) || ((mine >> 16) & 0xFFu) != 0xE4u
          || dxsoRegType(other) != kDxsoRegTemp || !plainSrc(other) || ((other >> 16) & 0xFFu) != 0xE4u || dxsoRegNum(other) == s.dst) { ok = false; return false; }
      s.consumer = pos; s.centre = dxsoRegNum(other);
      return false;
    }
    if (writesTemp && dxsoRegNum(d) == s.dst && (((d >> 16) & 7u) != 0u)) { ok = false; return false; }
    return true;
  });
  if (!ok || s.consumer == 0) return false;
  uint32_t nf = 0;
  for (int r = 11; r >= 0 && nf < 4; --r) if (!usedTemp[r]) s.freeTemp[nf++] = (uint32_t) r;
  if (nf < 4) return false;
  for (int c = 254; c >= 0; --c) if (!usedConst[c]) { s.freeConst = (uint32_t) c; return true; }
  return false;
}
inline bool analyzeCameraCard(const DWORD* t, size_t n, CameraCard& out) {
  out = CameraCard();
  CardSite s;
  if (!findCameraCard(t, n, s)) return false;
  out.valid = true; out.basisReg = s.basisReg;
  return true;
}
// Rewrites the stream so the card faces outward from its tree. The first DP3 keeps the offset
// (mov rT.xyz, rS.<swizzle>), the other two go; before the ADD: n = normalize(centre), right =
// normalize((n.z, 0, -n.x) + (1e-5, 0, 0)) (level; the small x settles a leaf straight above the
// origin), up = n x right, and the offset = right * T.x + up * T.y + n * T.z -- the camera's right,
// up and back replaced by the leaf's own. Returns false (stream untouched) when the pattern is not there.
inline bool makeOutwardCards(std::vector<DWORD>& t) {
  CardSite s;
  if (!findCameraCard(t.data(), t.size(), s)) return false;
  const uint32_t T = s.freeTemp[0], N = s.freeTemp[1], Rr = s.freeTemp[2], U = s.freeTemp[3], D = s.dst, C = s.centre, K = s.freeConst;
  auto dstT = [](uint32_t r, uint32_t mask) -> DWORD { return 0x80000000u | (mask << 16) | r; };
  auto srcT = [](uint32_t r, uint32_t swz) -> DWORD { return 0x80000000u | (swz << 16) | r; };
  auto srcC = [](uint32_t r, uint32_t swz) -> DWORD { return 0xA0000000u | (swz << 16) | r; };
  const size_t first = (std::min)(s.dp3[0], (std::min)(s.dp3[1], s.dp3[2]));
  std::vector<DWORD> r; r.reserve(t.size() + 48);
  r.push_back(t[0]);
  const float k[4] = { 1.f, 0.f, -1.f, 1e-5f };
  r.push_back(0x05000051u); r.push_back(0xA00F0000u | K);              // def c<K>, 1, 0, -1, 1e-5
  for (float f : k) { DWORD w; std::memcpy(&w, &f, 4); r.push_back(w); }
  const size_t n = t.size();
  size_t pos = 1;
  while (pos < n) {
    const uint32_t tok = t[pos];
    if (tok == kDxsoEnd) break;
    const uint32_t op = tok & 0xFFFFu;
    if (op == 0xFFFEu) { const size_t len = 1 + ((tok >> 16) & 0x7FFFu); if (pos + len > n) return false; r.insert(r.end(), t.begin() + pos, t.begin() + pos + len); pos += len; continue; }
    const uint32_t len = (tok >> 24) & 0xFu;
    if (pos + len >= n) return false;
    if (pos == first) {
      r.insert(r.end(), { 0x02000001u, dstT(T, 7u), s.offsetSrc });                         // mov rT.xyz, rS.<swizzle>
    } else if (pos == s.dp3[0] || pos == s.dp3[1] || pos == s.dp3[2]) {
      // the other two DP3s go
    } else {
      if (pos == s.consumer) {
        r.insert(r.end(), { 0x02000024u, dstT(N, 7u), srcT(C, 0xE4u) });                     // nrm rN.xyz, rC
        r.insert(r.end(), { 0x03000005u, dstT(D, 7u), srcT(N, 0xC6u), srcC(K, 0xE4u) });     // mul rD.xyz, rN.zyxw, cK  (n.z, 0, -n.x)
        r.insert(r.end(), { 0x03000002u, dstT(D, 1u), srcT(D, 0x00u), srcC(K, 0xFFu) });     // add rD.x, rD.x, cK.w
        r.insert(r.end(), { 0x02000024u, dstT(Rr, 7u), srcT(D, 0xE4u) });                    // nrm rR.xyz, rD     (right)
        r.insert(r.end(), { 0x03000021u, dstT(U, 7u), srcT(N, 0xE4u), srcT(Rr, 0xE4u) });    // crs rU.xyz, rN, rR (up)
        r.insert(r.end(), { 0x03000005u, dstT(D, 7u), srcT(Rr, 0xE4u), srcT(T, 0x00u) });    // mul rD.xyz, rR, rT.x
        r.insert(r.end(), { 0x04000004u, dstT(D, 7u), srcT(U, 0xE4u), srcT(T, 0x55u), srcT(D, 0xE4u) });   // mad rD.xyz, rU, rT.y, rD
        r.insert(r.end(), { 0x04000004u, dstT(D, 7u), srcT(N, 0xE4u), srcT(T, 0xAAu), srcT(D, 0xE4u) });   // mad rD.xyz, rN, rT.z, rD
      }
      r.insert(r.end(), t.begin() + pos, t.begin() + pos + 1 + len);
    }
    pos += 1 + len;
  }
  while (pos < n) r.push_back(t[pos++]);
  t.swap(r);
  return true;
}
// The three constants (12 floats from c[basisReg]) hold the camera's right, up and back as their
// columns: row i is row i of the camera's view rotation (Camera::view, the transpose of V).
inline bool cardBasisIsCamera(const float c[12], const Camera& cam) {
  for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) if (std::fabs(c[i * 4 + j] - cam.view.m[i][j]) > 0.02f) return false;
  return true;
}

// ---- per-pixel-shader albedo stage (milestone 2c) -----------------------------------------
// "The first colour 2D texture" is the wrong albedo for multi-texture materials: the object
// shaders keep an emissive map at s2 and the diffuse at s3, the floor shaders a normal map at
// s1 and the diffuse blend at s2. The stage below was read from each pixel shader's
// arithmetic (the sample multiplied by the summed light colour) and is keyed by the pixel
// shader's bytecode hash; it takes precedence over pickAlbedoStage for captured draws.
// The recolourable object shaders also carry the Create-A-Style tint at c8, and walls C its flat
// colour at c4 (tintReg, the pixel constant forwarded as the texture factor, see packTint; -1: none).
struct AlbedoStage { uint64_t hash; const char* name; uint8_t stage; int8_t tintReg = -1; };
inline constexpr int8_t kTintRegister = 8;

inline const AlbedoStage kAlbedoStages[] = {
  // objects: s2 the lot's light map (sampled with a world-space projection, rows c15/c16 of the VS; scaled by c12.y), s3 diffuse (TEXCOORD2), s4 specular mask; light rig and tint
  { 0x0c19795eb80e2e96ull, "object PS 0x10a68900", 3, kTintRegister },
  { 0x8282d3a0d611b62eull, "object PS 0x10a69440", 3, kTintRegister },
  { 0x54bea85dc05c7c35ull, "object PS 0x10a694e0", 3, kTintRegister },
  { 0x470c140c802b7ec0ull, "object PS 0x1118d5e0", 3, kTintRegister },
  { 0x5aee1186d554dbc4ull, "object PS 0x10a68220 (s2 diffuse, TEXCOORD2; 3 lights)", 2 },
  // lot terrain paint (VS 0x15c787a0; thousands of triangles per draw): s1 normal map,
  // diffuse = mask blend of the paint layers s2/s3/s4 (TEXCOORD0). Not the floor tiles.
  { 0x17eabad58f650687ull, "terrain paint PS 0x13d1dd40", 2 },
  { 0x670dbe0fa52c4650ull, "terrain paint PS 0x13d1d5c0", 2 },
  { 0xd63bf505ec4a44a0ull, "terrain paint PS 0x13d1d8e0", 2 },
  { 0x98062e8d4d12af7dull, "terrain paint PS 0x13d1d980", 2 },
  // in-game variants (run-15 shader dump)
  { 0x1458c67a2c009563ull, "object PS variant 1458c67a (s2 diffuse, TEXCOORD2)", 2 },
  { 0xa3afadeeb6a034c6ull, "object PS variant a3afadee (s2 diffuse, TEXCOORD2)", 2 },
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
  { 0x9ccd448262adfd4dull, "PS 0x105dbc60 (s2, TEXCOORD1)", 2 },
  { 0xd99d3c12905fd7c9ull, "PS 0xd7adc80 (s1, TEXCOORD2)", 1 },
  // layered ground decals: four layers on TEXCOORD0, first layer as albedo
  { 0x2d843890f7471cdfull, "PS 0xd7ad960 (s6..s9 layers)", 6 },
  { 0x2bc380a5a20143fcull, "PS 0x104068c0 (s7..s10 layers)", 7 },
  { 0x3ebb622c4fe0be4full, "floors PS 0x10a221a0 (s2 pattern on TEXCOORD0; s1 is the room lightmap)", 2 },
  { 0xa63ccabe650b1bc0ull, "floors PS 0x10a1f0e0 (s2 pattern on TEXCOORD0; s1 is the room lightmap)", 2 },
  // walls C (milestone 79): the wall's thickness -- tops, edges, the sides of openings, its own index
  // ranges of the wall buffer -- in one flat colour, c4, under the light; no colour texture. Its s1,
  // the opening mask (greyscale, white where the wall stands), is the albedo, tinted by c4.
  { 0x7d2cbb8e474dfaf5ull, "walls C PS 7d2cbb8e (flat colour c4; s1 the opening mask)", 1, 4 },
  // terrain and simple textured objects: stage 0 already
  { 0xe18ad53a96ff51ccull, "terrain PS 0x103f2f20", 0 },
  { 0x27ac2b7a8987e4d4ull, "terrain PS 0x13be1180", 0 },
  { 0x028ce2dde691b739ull, "terrain PS 0x13be0fa0", 0 },
  { 0x3608ab95ab50c8b4ull, "terrain PS 0x13be12c0", 0 },
  { 0xc30755d3de24af9aull, "terrain PS 0x13be1360", 0 },
  // PS 0x167e8be0 (ff72720d): colour = s1 (TEXCOORD0) x s0 (TEXCOORD1), the floor tiles' layout; its
  // entry here named s0, a room light map (256x128, or a 32x32 / 4x4 stand-in): left to the
  // chooser since milestone 76, which takes the compressed s1
  { 0xda37b5ef6f7a09a6ull, "floor tiles PS 0x167e3320 (s1 on TEXCOORD0; s0 is the lightmap)", 1 },
  { 0xdeeecbb3cdf04655ull, "PS 0xdbbbc40 (s1)", 1 },
  { 0xb0fb977be6b7bbccull, "PS 0x106fe840 (s1)", 1 },
  { 0xc3c0476375bf7797ull, "PS 0x16e98380 (s1)", 1 },
  { 0x78a22ef03769d4deull, "PS 0x16e95cc0 (s1)", 1 },
};

inline const AlbedoStage* findAlbedoStage(uint64_t hash) { return findByHash(kAlbedoStages, hash); }

// ---- Create-A-Style tint via the fixed-function texture factor (milestone 2c) --------------
// The object pixel shaders multiply their diffuse sample by a per-object tint constant (c8)
// before lighting: for recoloured furniture the texture is a greyscale pattern and c8 is the
// colour. Remix's legacy material multiplies the albedo by D3DRS_TEXTUREFACTOR when stage 0's
// colour argument 2 is TFACTOR (d3d9_rtx_utils.cpp: materialData.tFactor = renderStates[
// D3DRS_TEXTUREFACTOR]), and the game's pixel shaders ignore that fixed-function state, so
// the client forwards c8 there for captured draws (white for shaders without a tint), read from
// the device at the draw (milestone 79), as the shader reads it.

inline uint32_t packTint(const float* rgb) {
  auto to8 = [](float v) -> uint32_t { if (!(v > 0.f)) return 0u; if (v > 1.f) return 255u; return (uint32_t) (v * 255.f + 0.5f); };
  return 0xFF000000u | (to8(rgb[0]) << 16) | (to8(rgb[1]) << 8) | to8(rgb[2]);   // D3DCOLOR ARGB
}

// ---- draws never sent to the runtime, by pixel shader (milestones 130, 131, 133; one table since 148-149)
// What the pixel shader computes decides it. kGameFake: what the game paints because it cannot trace
// light -- its shadows, its fog, its glow and its tone curve -- which Remix makes itself: every draw of
// the shader is left out (run 234 counted what still reached the runtime uncaptured; the shaders
// disassembled; none of these is ever captured). Before the interface they cost the bridge and the
// runtime for nothing; the tone curve, after the interface, was laid over the ray-traced picture.
// kLeftOut: a surface the ray tracer must not have in the world -- one it cannot draw, or a second one
// over another: every world draw is left out (draws outside the world are rasterized as the game draws them).
// kBlendedCopy: the blended copy of an alpha-tested surface -- the same mesh drawn again for soft edges,
// or a faint effect over it: its blended world draws are left out (see-through, the runtime gives them
// no motion and drops their vertex colour); its opaque draws are captured, and its draws outside the
// world (the action menu's portrait, a partial viewport) are rasterized as the game draws them.
// (Until milestone 148 a second table, kNeverCapture, named some of these by vertex shader: its entries
// whose vertex shader only ever drew with a fake went in milestone 144, and the effect cards' vertex
// shader 6cb3b47f, which only ever draws with 7304aaea and 7865aa44, became those two here. Until 149
// the lot picture was named by its vertex shader 92337a18, which only ever draws with 4c59eb62, and the
// sound waves stood in a list of their own, both dropped once the draw had been taken for capture.)
enum : uint8_t { kGameFake = 0, kLeftOut = 1, kBlendedCopy = 2, kDropKinds = 3 };
struct DropPs { uint64_t hash; const char* name; uint8_t kind; };   // hash: the pixel shader's
inline const DropPs kDropPs[] = {
  // the screen copied in 256x256 tiles after the interface and drawn back through x (1 + c0) / (x + c0)
  { 0xbac911e069b2ee21ull, "the game's tone curve", kGameFake },
  // the glow: the scene's copy shrunk into 512x512, blurred back into 1024x1024, added onto the screen
  { 0x5b275a5c0a5eb82eull, "the game's glow (shrink)", kGameFake },
  { 0x596c432012619e82ull, "the game's glow (blur)", kGameFake },
  { 0x353ed3fb56cf16f1ull, "the game's glow (blur; added onto the screen)", kGameFake },
  // the Sims' soft shadows: each Sim's silhouette in a channel of a 512x512 target, blurred down to
  // 64x64 and back, laid over the ground as blobs (VS b7d550c6)
  { 0x06640070ab99ca77ull, "the Sims' shadow silhouettes", kGameFake },
  { 0x4e0b4fe50a3af4eaull, "the Sims' shadows (blur)", kGameFake },
  { 0x2db6dcdb34d42a11ull, "the Sims' shadows (box filter)", kGameFake },
  { 0xe602e4157960f7adull, "the Sims' shadows (blur)", kGameFake },
  { 0x6cc68b13e65b5a73ull, "the Sims' shadow blobs on the ground", kGameFake },
  { 0x92a300ed9b662005ull, "the Sims' shadow blobs on the ground (multiplied)", kGameFake },
  { 0x7f79fba967877d64ull, "a shadow blob on the ground", kGameFake },
  // shadows and light painted onto the ground, the lot and the Sims
  { 0x6c1867be86538473ull, "building shadows on the ground (the shadow map, four taps, multiplied)", kGameFake },
  { 0x9b8f4e2b9fbb9bb1ull, "the town ground's light (the shadow map, multiplied over the paint)", kGameFake },
  { 0x00d76427fc9ede0cull, "a shadow decal on the ground (multiplied)", kGameFake },
  { 0xff810e83c3f21f0bull, "drop-shadow decals (multiplied)", kGameFake },
  { 0x11227d6d7bba5802ull, "the Sims' darkening overlay (black by the composite's alpha)", kGameFake },
  { 0x8133bb57435ab6eeull, "the lot's shading sheets in the top-down view (a colour, alpha by height)", kGameFake },
  // the fog painted over the lot (the runtime's fog comes from the game's light record)
  { 0xd33629855723d2bcull, "the game's fog over the lot", kGameFake },
  { 0xd09db2967daf6d1full, "the game's fog over the lot", kGameFake },
  // The lot's ground as the neighbourhood view draws it (milestone 63): VS 92337a18 (the lot VS's decode
  // and per-chunk clip, plus a normal) with this pixel shader, which shows only a pre-baked 256x256 picture
  // of the lot's paint; two draws per frame, the active lot only (runs 168-171). A second surface over the
  // lot's ground there -- the low-detail model's plate, which fills the hole the town ground has under
  // every lot (run 176).
  { 0x4c59eb620412df33ull, "the neighbourhood view's lot picture (a second surface over the lot's ground)", kLeftOut },
  // A screen-shimmer effect (milestone 114): the game bends the scene behind through a bump map, tinted by
  // a colour texture and the vertex colour, cut out on its alpha -- the sound waves from speakers (run 219),
  // taken for bumpy glass by milestone 107's survey. The ray tracer cannot draw a shimmer (the user: "leave
  // the sound waves out for now").
  { 0xa9336d35a25143aeull, "the sound waves from speakers: ps_2_0, signed bump map s2 at TEXCOORD4, the scene behind s1, colour s3 (VS 648e326d)", kLeftOut },
  // The Sims' hair soft-edge pass (milestones 131, 133; found in run 171): each Sim's hair is drawn twice --
  // alpha-tested and opaque, then the same mesh blended (SRCALPHA / INVSRCALPHA, no depth write) for its
  // soft edges; the vertex shader differs from Sim to Sim, the pixel shader is the hair's: 57a5a049 and
  // 00e85de9 indoors, 45c7a7cd outdoors (with the sun's shadow map s5 and the fog; taken for a parked
  // car's windows until milestone 134). Captured, the see-through copy trailed behind a walking Sim (run 237).
  { 0x57a5a049ffa47770ull, "the Sims' hair soft-edge pass (the blended copy of the alpha-tested hair)", kBlendedCopy },
  { 0x00e85de9a42890fbull, "the Sims' hair soft-edge pass (the blended copy of the alpha-tested hair)", kBlendedCopy },
  { 0x45c7a7cd511b5233ull, "the Sims' hair soft-edge pass outdoors (with the sun's shadow map; the blended copy)", kBlendedCopy },
  // The effect cards (milestone 91, VS 6cb3b47f): texture x vertex colour, alpha = texture alpha x vertex
  // alpha, a constant-direction cube glint, no culling. The runtime drops the vertex colour, so a faint
  // blended effect became an unshaded white sheet: the light beams over the downtown lot (run 173) and a
  // pond's surface effect drawn through the water's stencil mask -- the "flat white" pond of runs 194-198.
  { 0x7304aaea6a75fb3full, "effect cards (blended: light beams, a pond's surface effect)", kBlendedCopy },
  { 0x7865aa449e279d1dull, "effect cards (blended; the stage-0 variant)", kBlendedCopy },
};
inline const DropPs* findDropPs(uint64_t psHash) { return findByHash(kDropPs, psHash); }
// Whether a draw of the bound pixel shader is left out (is3D: a world draw, see drawIs3D).
inline bool dropsDraw(const DropPs* d, bool is3D, DWORD alphaBlendEnable) {
  return d && (d->kind == kGameFake || (is3D && (d->kind == kLeftOut || alphaBlendEnable != 0)));
}

// ---- trees near the camera drawn solid (milestone 137) -------------------------------------
// When the camera comes close the game draws a whole tree blended instead of opaque: its own pixel
// shaders, the same textures, the alpha out the tree's opacity (c2.w, falling as the camera nears)
// and a texkill that thins the leaves as it falls (run 240: 594 blended draws against 152 opaque at
// close range). The runtime took them for translucent -- unshaded and see-through. The user's
// choice: a tree near the camera stays a solid tree. Blending off, and its leaves cut where the
// opaque tree's are -- alpha at least the tree's fade, which the vertex shader hands over in
// fadeInput (semantic * 4 + component, as PsAnalysis::fadeInput): VS 1b5224b7 writes c2.y to
// TEXCOORD1.w and 01729eeb to COLOR0.w, as their opaque counterparts 799a26fa and 854fd850 do.
// sampler: the leaf texture the cut reads, which must be the draw's albedo.
struct SolidFade { uint64_t hash; const char* name; int8_t sampler; int8_t fadeInput; };   // hash: the pixel shader's
inline const SolidFade kSolidFades[] = {
  { 0x714d5dae31da4378ull, "a tree's leaves near the camera (VS 1b5224b7)", 1, 1 * 4 + 3 },
  { 0xd3759d8d17c63b92ull, "a tree's branches near the camera (VS 01729eeb)", 1, kSemColor0 * 4 + 3 },
  { 0xca3faba5c3d2dbc2ull, "a tree's fronds near the camera (VS 01729eeb)", 1, kSemColor0 * 4 + 3 },
};
inline const SolidFade* findSolidFade(uint64_t psHash) { return findByHash(kSolidFades, psHash); }

// ---- the lot terrain drawn once per world chunk (milestone 16) ----------------------------
// A lot's ground mesh is drawn once for every 256-unit world chunk it overlaps, with that chunk's
// textures, each copy clipped to its chunk by the kill rectangle -- the clip kShaderPatches
// disables, since under vertex capture the clipped vertices became the fans. With the clip off
// every copy is the whole lot, so a lot on a chunk boundary was two coincident opaque surfaces
// (run 73: the terrain flicker in the lot). The frame's first draw of a mesh (same vertex
// buffer, same World rows c4..c6) is captured; the copies after it differ only in the chunk
// textures and are baked as hidden layer passes (lotFurtherCopy).
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
//     re-submissions -- its further chunk copies, its paint composite's passes -- are hidden
//     too and issued as two half draws, so the runtime never takes one for the lot's
//     own visible instance (runs 85-89: white or flickering lots whenever it did);
//   * the lit lot-paint shaders get their final lighting multiply replaced by the albedo
//     (psUnlitOutput), so the bake carries no sun, shadow or fog.
// The markers' content is fixed, so their hashes are stable across runs: the hook sets the two
// runtime options itself when it can hash (xxhash), otherwise the textures are tagged once in
// the runtime's menu.
// Hook options from `sims3hook.txt` next to this DLL (one `key = value` per line, integers;
// `#` comments), read once. Absent file or key = the default, and every default is the value the
// shipped sims3/config/sims3hook.txt carries, so the hook behaves the same without the file.
inline int hookOption(const char* key, int def) {
  struct Entry { char key[48]; int value; };
  static Entry entries[64]; static int count = -1;
  if (count < 0) {
    count = 0;
    char path[MAX_PATH] = {};
    HMODULE self = GetModuleHandleA("d3d9.dll");
    if (self && GetModuleFileNameA(self, path, MAX_PATH)) {
      if (char* slash = strrchr(path, '\\')) {
        snprintf(slash + 1, (size_t) (MAX_PATH - (slash + 1 - path)), "sims3hook.txt");
        if (FILE* f = fopen(path, "rb")) {
          char line[256];
          while (fgets(line, sizeof line, f) && count < 64) {
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

// markKey (milestone 17y): the virtual-key code of the mark key, which logs the lit lamps, the fog
// and the frame's glass once; default 220 = backslash.
inline int markKey() { static int s = -1; if (s < 0) { s = hookOption("markKey", 220); if (s < 1 || s > 254) s = 220; } return s; }
// The game's fog (milestone 56): the game's own fog (its colour and range, read beside its light
// record) goes to the runtime as D3D9 linear fog, which the runtime lays over the ray-traced
// picture (its composite fog, used while rtx.volumetrics.enable is off); fogColourScale =
// the fog's brightness in thousandths (1000: the game's fog colour as bright as the sky the game
// draws, lit as the scene is lit, milestone 57), sent as rtx.fogColorScale through the Remix API.
inline float fogColourScale() { static float s = -1.f; if (s < 0.f) { int v = hookOption("fogColourScale", 1000); if (v < 0) v = 0; if (v > 10000) v = 10000; s = (float) v / 1000.f; } return s; }
// The lights go to the runtime through the Remix API (milestones 20b, 23): the sun as a distant
// light, the lamps as sphere lights, with explicit radiance and size. The API needs
// exposeRemixApi = True in .trex\bridge.conf; without it there are no lights and one warning.
// sunAngle = the sun's angular diameter, thousandths of a degree; sunRadiance = radiance per
// unit of rig colour, thousandths; lampRadius = the lamps' sphere radius in thousandths of a
// unit; lampRadiance = radiance per unit of colour, thousandths.
inline float sunAngle() { static float s = -1.f; if (s < 0.f) { int v = hookOption("sunAngle", 2000); if (v < 100) v = 100; if (v > 90000) v = 90000; s = (float) v / 1000.f; } return s; }
// moonLight = the moon's share of the game's moonlight, in percent (100 = the game's own; 0 = none, the night stays dark).
inline float moonShare() { static float s = -1.f; if (s < 0.f) { int v = hookOption("moonLight", 2); if (v < 0) v = 0; if (v > 200) v = 200; s = (float) v / 100.f; } return s; }
// dawnMinutes = the sun's rise eased over this many game minutes after sunrise (the game's own rise is steep: nothing
// at 6 h, orange at 6.2 h): the game's light times the minutes since sunrise over this; 0 = the game's rise as it is.
inline float dawnHours() { static float s = -1.f; if (s < 0.f) { int v = hookOption("dawnMinutes", 60); if (v < 0) v = 0; if (v > 360) v = 360; s = (float) v / 60.f; } return s; }
// duskMinutes, duskLevel = the sun's afterglow: when its light falls below duskLevel (thousandths of full) during dusk it
// is held and faded out over duskMinutes by the game's clock (the game's own dusk ends with a drop from red to nothing in
// twelve minutes); 0 minutes = no afterglow.
inline float duskHours() { static float s = -1.f; if (s < 0.f) { int v = hookOption("duskMinutes", 60); if (v < 0) v = 0; if (v > 360) v = 360; s = (float) v / 60.f; } return s; }
inline float duskLevel() { static float s = -1.f; if (s < 0.f) { int v = hookOption("duskLevel", 100); if (v < 1) v = 1; if (v > 1000) v = 1000; s = (float) v / 1000.f; } return s; }
inline float sunRadiance() { static float s = -1.f; if (s < 0.f) { int v = hookOption("sunRadiance", 1000); if (v < 0) v = 0; s = (float) v / 1000.f; } return s; }
inline float lampRadius() { static float s = -1.f; if (s < 0.f) { int v = hookOption("lampRadius", 150); if (v < 20) v = 20; s = (float) v / 1000.f; } return s; }
// lampMax = the most lamps lit at once (the nearest to the camera's target first); the world lights
// (a street lamp's) are lit too, faded in and out by the game's own night switch.
inline uint32_t lampMax() { static int s = -1; if (s < 0) { s = hookOption("lampMax", 96); if (s < 1) s = 1; if (s > 96) s = 96; } return (uint32_t) s; }
inline float lampRadiance() { static float s = -1.f; if (s < 0.f) { int v = hookOption("lampRadiance", 40000); if (v < 0) v = 0; s = (float) v / 1000.f; } return s; }
// The lamps' shapes from the game's definitions (milestone 32): cones for spots and lamp shades, a
// shade's glow, a cylinder for a tube. lampConeScale scales
// every cone angle (thousandths; the definitions' angles are taken as half angles); lampConeSoftness
// the cone edge's softness (thousandths, 0..1000); lampShadeGlow scales the light through the shade.
inline float lampConeScale() { static float s = -1.f; if (s < 0.f) { int v = hookOption("lampConeScale", 1000); if (v < 100) v = 100; s = (float) v / 1000.f; } return s; }
inline float lampConeSoftness() { static float s = -1.f; if (s < 0.f) { int v = hookOption("lampConeSoftness", 300); if (v < 0) v = 0; if (v > 1000) v = 1000; s = (float) v / 1000.f; } return s; }
// ---- the lamps' own word and the game's clock (milestones 36, 39, 40) --------------------
// Nothing about a lamp is in what the game draws but its mesh. The game's scripts know it all,
// and the lamp reporter (sims3/scriptmod: a script mod) hands it over: for every lamp near the
// camera the keys of its object and of its model, its transform, whether it is on, its colour
// and its level, written a few times a second into a block of memory the hook finds by its
// signature (the block's head carries the block's own address, so nothing else in the process
// passes for it). The reporter's word is the lamp; what it does not name gives no light. With
// the lamps comes the game's clock (version 3).
inline constexpr uint32_t kLampMagic0 = 0x58523353u, kLampMagic1 = 0x504D414Cu, kLampMagic2 = 0x33303076u;   // 'S3RX' 'LAMP' 'v003'
inline constexpr uint32_t kLampHead = 64, kLampFloats = 24, kLampInts = 8, kLampCapacity = 512;
// The block's head, checked: the signature, the block's own address, the version, the record
// sizes, a lamp count within the capacity, the two areas where they can be. head = its first 64
// bytes as read.
struct LampHead { uint32_t lamps, sequence, floatsAt, intsAt; bool world; };
inline bool lampReportHead(const uint8_t* head, uint32_t address, LampHead& out) {
  uint32_t h[16]; std::memcpy(h, head, sizeof h);
  if (h[0] != kLampMagic0 || h[1] != kLampMagic1 || h[2] != kLampMagic2 || h[3] != address) return false;
  if (h[4] != 3u || h[7] != kLampFloats || h[11] != kLampInts || h[8] > kLampCapacity || h[6] > h[8]) return false;
  if (h[12] < kLampHead || h[12] > 4096u || h[13] < h[12] + (h[8] + 1u) * kLampFloats * 4u || h[13] > (1u << 20)) return false;
  out.lamps = h[6]; out.sequence = h[5]; out.world = h[10] != 0; out.floatsAt = h[12]; out.intsAt = h[13];
  return true;
}
// The game's clock, from the reporter's frame record: the hour of the day, the game's sunrise and
// sunset, and the game's own word for night (SimClock.IsNightTime: the hour lies between sunset
// and sunrise). known: the reporter could read it.
struct GameClock { float hour, sunrise, sunset; bool night, known; };
inline GameClock clockFromRecord(const float* frame) {
  GameClock c;
  c.hour = frame[8]; c.sunrise = frame[9]; c.sunset = frame[10]; c.night = frame[11] > 0.5f;
  c.known = c.hour >= 0.f && c.hour < 24.f && c.sunset > c.sunrise;
  return c;
}
// The hours of the game's one directional light (run 152): it is the sun's from sunrise until
// an hour after sunset, where it has faded to nothing, and the moon's from there until sunrise.
inline bool clockMoonTime(const GameClock& c) { return c.known && (c.hour >= c.sunset + 1.f || c.hour < c.sunrise); }
// Dusk: the two hours from sunset, in which the sun's light goes and the moon's comes.
inline bool clockDusk(const GameClock& c) { return c.known && c.hour >= c.sunset && c.hour < c.sunset + 2.f; }
// The dawn's ease (milestone 43, the hook's own): the factor on the sun's light for the first `hours`
// after sunrise, rising from nothing at sunrise to the game's own light at the end; 1 otherwise.
inline float dawnEase(const GameClock& c, float hours) {
  if (!c.known || hours <= 0.f || c.hour < c.sunrise || c.hour >= c.sunrise + hours) return 1.f;
  return (c.hour - c.sunrise) / hours;
}
// The dusk's cross-fade (milestone 46): -1 while the game's light is still the sun's, then, from
// the moment it becomes the moon's (an hour after sunset), rising from 0 to 1 over `hours`; 1
// once past, or without hours, or without the clock.
inline float duskFade(const GameClock& c, float hours) {
  if (!c.known) return 1.f;
  if (!clockMoonTime(c)) return -1.f;
  if (hours <= 0.f) return 1.f;
  float since = c.hour - (c.sunset + 1.f); if (since < 0.f) since += 24.f;
  return since / hours > 1.f ? 1.f : since / hours;
}
inline uint64_t lampId64(const int32_t* lowHigh) { return (uint64_t) (uint32_t) lowHigh[0] | ((uint64_t) (uint32_t) lowHigh[1] << 32); }
// A record's word for one light of a lamp: on or off, and the colour to send -- the player's colour
// (a preset's or a custom one; the definition's own when the lamp is left at its default, preset
// 13) times the definition's intensity times the level, relative to the game's normal level. The
// level is the engine's dimmer: switching a lamp on sets it to the lamp's intensity, off to 0.
struct LampWord { bool on; float col[3]; float level; };
inline LampWord lampWordFromRecord(const float* r, const float* defCol, float defIntensity, float normalLevel) {
  LampWord w;
  w.on = r[8] > 0.5f;
  float c[3] = { r[3], r[4], r[5] };
  if ((int) (r[9] + 0.5f) == 13) { c[0] = defCol[0]; c[1] = defCol[1]; c[2] = defCol[2]; }
  else if (c[0] > 1.5f || c[1] > 1.5f || c[2] > 1.5f) { c[0] /= 255.f; c[1] /= 255.f; c[2] /= 255.f; }
  const float applied = r[7] > 0.f ? r[7] : r[6];
  w.level = (normalLevel > 1e-4f && applied > 0.f) ? applied / normalLevel : 1.f;
  if (w.level > 4.f) w.level = 4.f;
  for (int q = 0; q < 3; ++q) w.col[q] = c[q] * defIntensity / 100.f * w.level;
  return w;
}
inline float lampShadeGlow() { static float s = -1.f; if (s < 0.f) { int v = hookOption("lampShadeGlow", 1000); if (v < 0) v = 0; s = (float) v / 1000.f; } return s; }

// layerPass: every draw is a layer pass. lotFamily: a lot's ground and its paint composite --
// drawn in place, the first copy visible and every re-submission (further chunk copies, the
// composite's passes) hidden and split in two (milestones 16-19; the lot replay of milestones
// 17e-18i re-issued them after the world terrain, which run 119's paint test showed is not
// needed: nothing overwrites a lot's paint in the atlas). (A base draw keeping the shader's own
// coverage alpha, baked with an alpha test, was tried in run 77 and is wrong for this game: the
// world's base pass is black where only its blended layer passes paint.)
struct TerrainShader { uint64_t hash; const char* name; bool layerPass; bool lotFamily; };

inline const TerrainShader kTerrainShaders[] = {
  { 0x55c99586fb17cd1cull, "lot-area terrain paint (lit, 3 layers + lot mask)", false, false },
  { 0xdfaf82cf9ec175b0ull, "world terrain (4 layers + chunk mask; alpha-blended draws = extra layers)", false, false },
  { kLotTerrainVs,         "lot terrain (3 layers + chunk mask, clipped per chunk by texkill)", false, true },
  { 0x0344bbc366f10954ull, "lot paint composite (unlit 4-layer blend over the lot terrain)", true, true },
  { 0x2a57449ad7d2c7eeull, "the town's coarse ground (the far ground and the neighbourhood view)", false, false },
};
// The town's coarse ground (milestones 61, 63): low 256-unit squares of the whole town, one draw and
// one vertex buffer per square (SHORT4, with 2-unit skirts on the square edges and around every
// flattened lot pad), lit by PS 072c2bbd from a single pre-baked colour map of the town (s3 at v1).
// The neighbourhood view's only ground; in the household view the game draws, square by square,
// either the detailed pieces or the coarse square, never both (run 170). It goes through the
// squares' merged path like the detailed squares. VS: position x c16.xyx + c16.zwz, rows c8..c10.
// (The lot's ground as the neighbourhood view draws it, VS 92337a18: left out by its pixel shader, kDropPs.)


// How a terrain variant treats alpha: 0 as the shader writes it (blended layer passes), 1 forced
// to 1 (base draws).
// A lot mesh's further chunk copies are opaque draws each clipped to its own
// world chunk by texkill, together covering the lot: each must write the opacity (alpha 1) for
// its part (milestone 17n; with only the first copy writing alpha, the rest of a lot kept stale
// atlas texels, which shift whenever the cascades re-centre on a moving camera -- run 87's
// flicker during camera movement). The blended composite keeps the shader's alpha.
inline uint8_t terrainAlphaMode(const TerrainShader* t, uint8_t kind, DWORD alphaBlendEnable) {
  if (kind == 1 || kind == 3) return 1;   // kind 3: a square's piece that only paints (milestone 60), opaque like a base draw
  if (t && t->lotFamily && !t->layerPass && alphaBlendEnable == 0) return 1;
  return 0;
}
inline const TerrainShader* findTerrainShader(uint64_t hash) { return findByHash(kTerrainShaders, hash); }

// 0 not a terrain draw, 1 base terrain (baked and ray-traced), 2 layer pass (baked, hidden); the
// device makes a square's opaque piece kind 3 (baked with alpha 1, hidden) once the square's
// merged shape is traced instead (milestone 60, design B).
inline uint8_t terrainDrawKind(const TerrainShader* t, DWORD alphaBlendEnable, bool furtherLotCopy) {
  if (!t) return 0;
  return (t->layerPass || alphaBlendEnable != 0 || furtherLotCopy) ? 2 : 1;
}

// The lit lot-area paint pixel shaders: colour = albedo x light + fog, as one final
// `mad oC0.xyz, albedo, light, fog`; the unlit variant keeps the albedo (psUnlitOutput).
inline const uint64_t kUnlitPatches[] = { 0x17eabad58f650687ull, 0x670dbe0fa52c4650ull, 0x98062e8d4d12af7dull, 0xd63bf505ec4a44a0ull,
                                         0x072c2bbd4fdeb89bull };   // the coarse ground's shader (milestone 61): the same final mad
inline bool wantsUnlitPatch(uint64_t psHash) { for (uint64_t h : kUnlitPatches) if (h == psHash) return true; return false; }

// ---- the town ground's squares (milestone 60, design B) ----------------------------------
// Each 256-unit square of the town ground goes to the ray tracer as ONE shape, the union of the
// game's opaque pieces without the skirts; the pieces themselves only paint. A piece not yet in a
// shape is traced as it comes (before milestone 60 every piece was).
// The ground's triangles of a square: a skirt hangs 2 units down from the ground's edge, so two of
// its corners stand on the same point of the ground (the same raw x and z: run 168 found every
// near-vertical triangle of the town ground to be such a 2.0-unit wall); a heightfield's own
// triangles never do. A triangle with no area seen from above (three corners on a line) is left out
// as well, and so is one whose index falls outside the vertices. x, z: the raw SHORT4 x and z of
// every vertex; idx: triangle lists of vertex numbers; out: the kept ones.
struct MergeStats { uint32_t in = 0, kept = 0, skirts = 0, flat = 0, outside = 0; };
inline void mergeGroundTriangles(const std::vector<int32_t>& x, const std::vector<int32_t>& z, const std::vector<uint32_t>& idx, std::vector<uint32_t>& out, MergeStats& st) {
  const size_t n = x.size();
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
    ++st.in;
    if (a >= n || b >= n || c >= n) { ++st.outside; continue; }
    if ((x[a] == x[b] && z[a] == z[b]) || (x[b] == x[c] && z[b] == z[c]) || (x[c] == x[a] && z[c] == z[a])) { ++st.skirts; continue; }
    const int64_t area2 = (int64_t) (x[b] - x[a]) * (z[c] - z[a]) - (int64_t) (z[b] - z[a]) * (x[c] - x[a]);
    if (area2 == 0) { ++st.flat; continue; }
    out.push_back(a); out.push_back(b); out.push_back(c);
    ++st.kept;
  }
}
// Two pieces' index ranges (start << 32 | triangle count) share indices.
inline bool rangesOverlap(uint64_t r, uint64_t q) {
  const uint64_t a0 = r >> 32, a1 = a0 + 3ull * (uint32_t) r, b0 = q >> 32, b1 = b0 + 3ull * (uint32_t) q;
  return a0 < b1 && b0 < a1;
}
// ---- the game's own light in its memory (milestones 49, 127, 128) -----------------------
// The game keeps the light it computes in a record of eight floats: the direction toward the
// light, 0, the colour, 1 (run 161: a record that kept moving with the clock in every view and
// agreed with the lit terrain's c0 / c1 to five decimals). The hook reaches it through the game's
// own pointers (kLightChains) and takes the sky's light, the night switch and the fog from it:
// one source, in every view, from the first frame of a live world.
// (Not-a-number and infinite floats are told by their bits: the compiler may assume none exist.)
inline bool finiteFloats(const float* q, int n) {
  for (int k = 0; k < n; ++k) { uint32_t u; std::memcpy(&u, q + k, 4); if ((u & 0x7f800000u) == 0x7f800000u) return false; }
  return true;
}
inline bool floatBits(const float* q, uint32_t bits) { uint32_t u; std::memcpy(&u, q, 4); return u == bits; }

// The game's night switch (runs 163, 164): the float 28 before its light record, 0 by day and 1 at
// night, taking about eight game minutes to change at either end (19 h and 5 h in Sunset Valley);
// the lit terrain's c7.x, the scale of its lamp light map. A value outside 0..1, or not
// a number, is not the switch (nothing is written then).
inline bool nightSwitchValue(const float* q, float* out) {
  if (!finiteFloats(q, 1) || !(*q >= 0.f && *q <= 1.f)) return false;
  *out = *q;
  return true;
}

// The game's fog (run 163, milestone 56). Its lit terrain shader fogs by distance d with c4 and c2:
// amount = pow(1 - saturate(d * c4.x + c4.y), c4.w), colour c2 (the fog colour 64 floats before the
// light record, c4 20 before it). c4.x = -1 / (end - start) and c4.y = end / (end - start): the
// runtime's D3D9 linear fog with the same start and end. A curve c4.w other than 1 (grey days) is
// matched at half fog by moving the start, the runtime's fog being linear. False when c4 is no fog.
inline bool fogRangeFromGame(const float* c4, float* start, float* end) {
  if (!finiteFloats(c4, 4) || !(c4[0] < 0.f) || !(c4[3] > 0.05f && c4[3] <= 20.f)) return false;
  const float e = -c4[1] / c4[0], s = (1.f - c4[1]) / c4[0];
  if (!(e > s) || !(e < 1.0e6f) || !(s > -1.0e6f)) return false;
  const float half = s + (e - s) * std::pow(0.5f, 1.f / c4[3]);
  *start = 2.f * half - e;
  *end = e;
  return true;
}
// The game's fog colour (c2: gamma, as its shaders blend it) in the runtime's linear light: an opaque
// D3DCOLOR with its brightest channel at 255 (the hue, kept precise however dark the night) and that
// channel's linear value in *bright (the brightness, sent as rtx.fogColorScale; milestone 57).
inline bool fogColourFromGame(const float* c2, uint32_t* out, float* bright) {
  if (!finiteFloats(c2, 3)) return false;
  float lin[3], top = 0.f;
  for (int k = 0; k < 3; ++k) {
    const float x = c2[k] < 0.f ? 0.f : (c2[k] > 1.f ? 1.f : c2[k]);
    lin[k] = x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    if (lin[k] > top) top = lin[k];
  }
  uint32_t v = 0xFF000000u;
  if (top > 0.f) for (int k = 0; k < 3; ++k) v |= (uint32_t) (lin[k] / top * 255.f + 0.5f) << (16 - 8 * k);
  *out = v;
  *bright = top;
  return true;
}
// The fog's light (milestone 57): the game's fog colour goes with the game's own light, while the hook
// sends the sun and the moon at other strengths (the moon's share, the dawn's ease, the afterglow).
// The fog is lit in the same proportion: the light of the sky as sent over the game's own (both as
// luminance), at most 1; 1 while the game's light is out and the hook's is not; 0 with no light sent.
inline float fogLightShare(float sent, float game) {
  if (!(sent > 0.f)) return 0.f;
  if (!(game > 0.f) || sent >= game) return 1.f;
  return sent / game;
}

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
// Present an undeclared stage worked (the lot replays of the time, runs 78/80). The runtime's bookkeeping
// (SetStateTexture / UndirtyTextures / PrepareDraw / BindTexture, the compiler's per-sampler
// bound spec constant) shows nothing stage-specific, so the cause stays unknown; the way round
// it: layer 0 goes to a stage the shader declares AND the game binds for the draw, whose own
// texture the bake can do without -- the DETAIL texture. Every Sims 3 terrain pixel shader ends
// with `texld rN, v0.zwzw, sD` / `add rN.w, rN.x, rN.x` / colour x rN.w: a fine L8 grain sampled
// with the second half of TEXCOORD0, doubled. With s0 and sD swapped the variant reads layer 0
// from sD (the game's layer-0 texture bound there) and the "detail" from s0, where the marker
// sits: its red is 0x80, so the detail factor is 2 x 128/255 = 1.004 -- the bake loses the grain,
// nothing else. The one shader without such a read, the lot paint composite, is given a paint
// layer's stage instead (lotCompositeStage).
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
// the composite is pass 1 (layer 4 missing); the hook issues pass 2 right after it (milestone 19).
inline constexpr uint64_t kLotCompositePs = 0x99ee53ff6ef1b0b6ull;
inline int lotCompositeStage(int pass) { return pass == 1 ? 4 : pass == 2 ? 3 : 0; }

// The marker textures: 32x32 A8R8G8B8, fixed and flat. 0 = terrain (dark red; base draws;
// visible), 1 = the world's layer passes and a lot's further chunk copies (the same red, with
// blue; hidden), 2 = the composite's passes (black; hidden). The red is what matters
// for 0 and 1: the variant reads the marker as the shader's detail texture (psDetailSampler),
// doubled, and 0x80 makes that a factor of 1; the composite reads its marker as a paint layer,
// and black takes that layer out. Any fixed content fixes the hashes, which are what rtx.conf
// tags. D3DCOLOR (A R G B; memory order B, G, R, A on this machine).
// The terrain marker is DARK RED (0xFF800000): its red 0x80 is the detail factor of 1, and a lot
// showing its marker -- a draw whose bake did not replace its material -- is told apart (run 91).
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

inline float luminance(const float* c) { return 0.2126f*c[0] + 0.7152f*c[1] + 0.0722f*c[2]; }

// ---- the sun and the moon: the game's light (milestones 41, 128) -------------------------
// The game's one directional light, its colour and the unit direction toward it, as its light
// record holds it (and as it hands it to the lit terrain shaders, c0 and c1). It is the sun by
// day and the moon by night. Run 152 (a whole game day): the colour follows the game's timeline by the hour
// (SunMoonLight of the sky's light file / 255, linear between its keys), it is zero at 19 h and
// at 6 h, the direction changes sides at 19 h and at 5 h and never stands lower than 45
// degrees. The hook sends it as a distant light, the sun's or the moon's (SkyLights below).
//
// (Milestones 2b to 40 took the colour from a vote over the objects' light rigs and the
// direction from the shadow map's rows. In run 152 the vote was wrong or absent most of an
// outdoor day -- the sun at a fifth of its brightness at noon, the night's light through the
// whole sunrise -- while the terrain's direction equalled the shadow map's in every sample.)
struct Sun { float dir[3]; float col[3]; };   // dir: unit, toward the light; col: the game's colour, 1 = full

// A colour and a direction toward the light -- the game's own light record -- as the sky's
// light. False when they are not a light's: the direction is not a
// unit vector from above, or a colour is not a number, negative or beyond any light's.
inline bool skyLightFrom(const float* c0, const float* c1, Sun& out) {
  const float n = len3(c1);
  if (!(n > 0.98f && n < 1.02f) || !(c1[1] > 0.05f)) return false;
  for (int q = 0; q < 3; ++q) if (!(c0[q] >= 0.f && c0[q] <= 16.f)) return false;
  for (int q = 0; q < 3; ++q) { out.dir[q] = c1[q] / n; out.col[q] = c0[q]; }
  return true;
}

// The game's light record reached through the game's own pointers (milestone 127): record =
// [[TS3.exe + base] + member] + offset. Found by the M126 diagnostic in runs 231-232: the same five
// chains in two fresh sessions while the record lay elsewhere each time (1DDA6C00, 30D3BF20). A
// second static (0xe46c54) holds the same object as 0xdda2d4, so its two chains only repeated
// these (milestone 130: one chain per path). Run 233: two chains of five agreed only from the
// first frame on; run 234: all five from the first to the last. Offsets of TS3.exe build 6707155c.
struct LightChain { uint32_t base, member, offset; };
inline constexpr LightChain kLightChains[] = {
  { 0xe2ad10, 0xd0, 0x5e0 }, { 0xdda2d4, 0x64, 0x7f0 }, { 0xdda2d4, 0x58, 0xa10 },
};
inline constexpr int kLightChainCount = (int) (sizeof kLightChains / sizeof kLightChains[0]);
// The address most chains lead to, if at least two agree (at[k] = 0: chain k unreadable or not a
// record); 0 otherwise. `votes` = how many agree.
inline uintptr_t lightChainVote(const uintptr_t* at, int n, int& votes) {
  uintptr_t best = 0; votes = 0;
  for (int i = 0; i < n; ++i) {
    if (!at[i]) continue;
    int c = 0;
    for (int j = 0; j < n; ++j) if (at[j] == at[i]) ++c;
    if (c > votes) { votes = c; best = at[i]; }
  }
  if (votes < 2) { votes = 0; return 0; }
  return best;
}

// Whether the runtime's light still stands for this one: within a fifth of a degree and half a
// hundredth of each colour (an API light cannot be changed, only made anew).
inline bool sameSun(const Sun& a, const Sun& b) {
  return dot3(a.dir, b.dir) > 0.999994f && std::fabs(a.col[0] - b.col[0]) < 0.005f && std::fabs(a.col[1] - b.col[1]) < 0.005f && std::fabs(a.col[2] - b.col[2]) < 0.005f;
}

// ---- the two lights of the sky: sun and moon cross-fading at dusk and at dawn (milestones 42 to 46)
// The game's one directional light is the sun's or the moon's by the game's clock (clockMoonTime);
// the hook sends them as two lights, the moon's scaled by the user's share (moonLight). The game's
// own changes are abrupt: at dusk a drop from red to nothing in twelve minutes (18.8 to 19 h), at
// dawn nothing at 6 h and orange twelve minutes later, and the moon raised over the hour after
// 19 h and faded to nothing over the two hours before 6 h. At the user's wish the two lights are
// pulled into each other in equal parts, the same way at both ends:
//   - DUSK: the sun's light, once below duskLevel, is held as an AFTERGLOW until the game's light
//     becomes the moon's; from that moment, over duskMinutes, the afterglow fades from its level
//     to nothing while the moon rises from nothing to its floor (its full share);
//   - NIGHT: the moon's light never falls below its floor, in the direction the game has it,
//     moving as the game moves it;
//   - DAWN: from sunrise, over dawnMinutes, the moon fades from its floor to nothing while the sun
//     rises from nothing to the game's own light (the caller eases the sun by dawnEase).
// Every colour and direction is one the game gave; the cross-fades are the hook's own. Without the
// clock, or without minutes, the light goes out as the game has it.
struct SkyLights {
  Sun shown[2] = {}; bool showing[2] = { false, false };   // what goes out: 0 the sun, 1 the moon
  Sun strong = {}; bool haveStrong = false;                // the sun's light when it last stood at the dusk level or above
  Sun glow = {}; bool glowing = false; float glowFade = 0.f;   // the afterglow, and how much of it is left
  float moonFull[3] = { 0.137f, 0.137f, 0.392f };          // the moon's colour at its full: the game's 35, 35, 100 of 255 until a brighter moon has been seen
  bool keeping = false; float keepFade = 0.f;              // the moon kept past sunrise, and how much of it is left
  int body = 0;                                            // whose the game's light is
  float level() const { return (showing[0] ? luminance(shown[0].col) : 0.f) + (showing[1] ? luminance(shown[1].col) : 0.f); }
  // game: the light as the game hands it over, the sun's already eased for the dawn; kDusk: duskFade;
  // kDawn: dawnEase; dusk: clockDusk; level: duskLevel. Returns 1 when the afterglow begins, 2 when
  // it ends, 0 otherwise.
  int step(const Sun& game, bool moonTime, float share, float kDusk, float kDawn, bool dusk, float level) {
    int what = 0;
    body = moonTime ? 1 : 0;
    Sun live = game;
    if (body == 1) for (int q = 0; q < 3; ++q) live.col[q] *= share;
    showing[0] = showing[1] = false;
    const float floorLum = share * luminance(moonFull);
    if (body == 0) {
      const float lum = luminance(live.col);
      if (lum >= level) {
        strong = live; haveStrong = true;
        if (glowing) { glowing = false; glowFade = 0.f; what = 2; }
        shown[0] = live;
      } else {
        if (!glowing && dusk && kDusk < 0.f && haveStrong) { glow = strong; glowing = true; glowFade = 1.f; what = 1; }
        if (glowing) {
          for (int q = 0; q < 3; ++q) glow.dir[q] = live.dir[q];   // the game's sun still moves: the afterglow follows it
          shown[0] = glow;                                          // held at its level until the moon comes
        } else shown[0] = live;
      }
      showing[0] = true;
      // the dawn: the moon fades from its floor as the sun rises, in equal parts
      if (keeping) {
        keepFade = 1.f - kDawn;
        if (keepFade <= 0.f) { keeping = false; keepFade = 0.f; }
        else { for (int q = 0; q < 3; ++q) { shown[1].col[q] = moonFull[q] * share * keepFade; shown[1].dir[q] = live.dir[q]; } showing[1] = true; }
      }
    } else {
      if (luminance(game.col) > luminance(moonFull)) for (int q = 0; q < 3; ++q) moonFull[q] = game.col[q];
      // the dusk: the moon rises to its floor as the afterglow fades, in equal parts; by night the floor stands
      const float k = kDusk < 0.f ? 0.f : kDusk;
      shown[1] = live;
      if (luminance(live.col) < floorLum * k) for (int q = 0; q < 3; ++q) shown[1].col[q] = moonFull[q] * share * k;   // in the game's direction
      showing[1] = true; keeping = share > 0.f; keepFade = 1.f;
      if (glowing) {
        glowFade = 1.f - k;
        if (glowFade <= 0.f) { glowing = false; glowFade = 0.f; what = 2; }
        else { shown[0] = glow; for (int q = 0; q < 3; ++q) shown[0].col[q] *= glowFade; showing[0] = true; }
      }
    }
    return what;
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
// A cut-out's value (milestone 68): a sum of up to three terms, each a literal times, when c >= 0,
// one float constant component (register * 4 + component), read at draw time.
struct CutTerm { float k = 0.f; int16_t c = -1; };
struct CutPoly {
  CutTerm t[3]; uint8_t n = 0;
  bool add(const CutPoly& o) { if (n + o.n > 3) return false; for (uint8_t i = 0; i < o.n; ++i) t[n++] = o.t[i]; return true; }
  void negate() { for (uint8_t i = 0; i < n; ++i) t[i].k = -t[i].k; }
  bool same(const CutPoly& o) const { if (n != o.n) return false; for (uint8_t i = 0; i < n; ++i) if (t[i].c != o.t[i].c || t[i].k != o.t[i].k) return false; return true; }
};
struct PsAnalysis {
  PsSamplerUse samplers[16];
  uint16_t normalTexcoords = 0;   // TEXCOORD inputs the shader treats as a normal: normalised, or dotted with a constant (a light direction)
  uint16_t inputTexcoords = 0;    // TEXCOORD inputs the shader declares (ps_3_0 dcl_texcoord#, ps_2_x t#)
  // The wall opening mask (milestone 13): the sampler whose red is added to its own coordinate's
  // z (the wall shaders' "mask.r + t.z" test), and whether that value feeds a texkill or the
  // alpha output (walls C: alpha-tested).
  int8_t maskSampler = -1;
  bool maskKill = false, maskAlpha = false;
  // The cut-out (milestone 68): the shader's one texkill, when the value it tests is a * alpha + b --
  // alpha one 2D sampler's alpha, a and b literals and float constants (cutA, cutB), straight-line
  // code -- the game's alpha test written as a discard, which the runtime never sees. -1: none, or
  // not of that form (a mask, a vertex value, two textures, a comparison, flow control).
  int8_t cutSampler = -1;
  CutPoly cutA, cutB;
  // The fade (milestone 135): the alpha output is an interpolated component minus one 2D sampler's
  // alpha -- SpeedTree's leaves and branches, "fade - alpha" under the game's alpha test LESS 1/255:
  // a texel shows where its alpha reaches the tree's fade. The runtime tests the texture's own
  // alpha, so the test is turned around at draw time (groupFades) with the fade the vertex
  // shader hands over (VsConstantOutputs). fadeSampler: that sampler, or -1; fadeInput: the
  // interpolated component, semantic * 4 + component as in VsConstantOutputs.
  int8_t fadeSampler = -1, fadeInput = -1;
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
  int8_t inputColor[32]; for (int i = 0; i < 32; ++i) inputColor[i] = (major < 3 && i < 2) ? (int8_t) i : (int8_t) -1;   // ps_2_x: v# is COLOR#
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
  // the cut-out (milestone 68): per temp component, a * alpha(sampler) + b, or unknown; b may also
  // add one interpolated component, in (semantic * 4 + component; the fade, milestone 135)
  struct Form { bool known = false; int8_t s = -1, in = -1; CutPoly a, b; };
  Form form[32][4], alphaOut;
  float defv[256][4] = {}; bool defd[256] = {};
  int kills = 0; bool killOk = false, flow = false; Form killed;
  auto srcForm = [&](uint32_t tok, uint32_t comp) -> Form {
    Form f;
    const uint32_t mod = (tok >> 24) & 0xFu;
    if ((mod != 0u && mod != 1u) || (tok & (1u << 13))) return f;   // plain or negated, no relative addressing
    const uint32_t ty = dxsoRegType(tok), n = dxsoRegNum(tok), sc = (tok >> (16 + 2 * comp)) & 3u;
    if (ty == kTemp && n < 32) f = form[n][sc];
    else if ((ty == kInput || ty == kTexture) && n < 32) {              // an interpolated component, added as it is
      const int sem = (ty == kTexture || major >= 3) && inputTexcoord[n] >= 0 ? inputTexcoord[n] : (ty == kInput && inputColor[n] >= 0) ? kSemColor0 + inputColor[n] : -1;
      if (sem < 0 || mod != 0u) return f;
      f.known = true; f.in = (int8_t) (sem * 4 + (int) sc);
      return f;
    } else if (ty == 2u && n < 256) {                                      // a float constant: a literal (DEF) or read at draw time
      f.known = true; f.b.n = 1;
      if (defd[n]) f.b.t[0].k = defv[n][sc]; else { f.b.t[0].k = 1.f; f.b.t[0].c = (int16_t) (n * 4 + sc); }
    } else return f;
    if (f.known && mod == 1u) { if (f.in >= 0) return Form(); f.a.negate(); f.b.negate(); }
    return f;
  };
  auto addForm = [](const Form& x, const Form& y) -> Form {
    if (!x.known || !y.known || (x.s >= 0 && y.s >= 0 && x.s != y.s) || (x.in >= 0 && y.in >= 0)) return Form();
    Form r = x; r.s = x.s >= 0 ? x.s : y.s; r.in = x.in >= 0 ? x.in : y.in;
    if (!r.a.add(y.a) || !r.b.add(y.b)) return Form();
    return r;
  };
  auto mulForm = [](const Form& x, const Form& y) -> Form {
    if (!x.known || !y.known || (x.s >= 0 && y.s >= 0) || x.in >= 0 || y.in >= 0) return Form();
    const Form& v = x.s >= 0 ? x : y; const Form& kf = x.s >= 0 ? y : x;   // kf: a constant
    if (kf.b.n != 1) return Form();
    const CutTerm kt = kf.b.t[0];
    Form r = v;
    auto scale = [&](CutPoly& p) -> bool {
      for (uint8_t i = 0; i < p.n; ++i) { p.t[i].k *= kt.k; if (kt.c >= 0) { if (p.t[i].c >= 0) return false; p.t[i].c = kt.c; } }
      return true;
    };
    if (!scale(r.a) || !scale(r.b)) return Form();
    return r;
  };
  dxsoForEach(tokens, count, [&](size_t pos, uint32_t op, uint32_t len) {
    if (op == kDxsoOpDcl && len >= 2) {
      const uint32_t usage = tokens[pos + 1], dest = tokens[pos + 2];
      const uint32_t type = dxsoRegType(dest), n = dxsoRegNum(dest);
      if (type == kSampler && n < 16) out.samplers[n].cube = ((usage >> 27) & 0xFu) == 3u;   // texture type: 2 = 2D, 3 = cube, 4 = volume
      else if (type == kInput && n < 32 && major >= 3 && (usage & 0x1Fu) == kUsageTexcoord) { inputTexcoord[n] = (int8_t) ((usage >> 16) & 0xFu); out.inputTexcoords |= (uint16_t) (1u << ((usage >> 16) & 0xFu)); }
      else if (type == kInput && n < 32 && major >= 3 && (usage & 0x1Fu) == kUsageColor && ((usage >> 16) & 0xFu) < 2u) inputColor[n] = (int8_t) ((usage >> 16) & 0xFu);
      else if (type == kTexture && n < 32 && major < 3) { inputTexcoord[n] = (int8_t) n; if (n < 16) out.inputTexcoords |= (uint16_t) (1u << n); }
      return true;
    }
    if (op == 0x51u && len >= 5) {                                     // DEF c#, four literals (the cut-out's constants)
      const uint32_t n = dxsoRegNum(tokens[pos + 1]);
      if (n < 256) { defd[n] = true; for (uint32_t c = 0; c < 4; ++c) std::memcpy(&defv[n][c], &tokens[pos + 2 + c], 4); }
      return true;
    }
    if (dxsoIsDef(op) || len < 1) return true;
    if ((op >= 0x19u && op <= 0x1Eu) || (op >= 0x26u && op <= 0x2Du) || op == 0x60u) flow = true;   // call / loop / rep / if / else / break
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
      // the cut-out: every tested component the same a * alpha(s) + b
      ++kills; killOk = false;
      if (dtype == kTemp && dn < 32 && !flow) {
        Form f0; bool same = true;
        for (uint32_t c = 0; c < 4 && same; ++c) {
          if (!(mask & (1u << c))) continue;
          const Form& f = form[dn][c];
          if (!f.known || f.s < 0 || f.in >= 0) same = false;
          else if (!f0.known) f0 = f;
          else if (f.s != f0.s || !f.a.same(f0.a) || !f.b.same(f0.b)) same = false;
        }
        if (same && f0.known) { killed = f0; killOk = true; }
      }
      return true;
    }
    // the cut-out's forms of the components this instruction writes (unknown unless plain arithmetic),
    // and the alpha output's (the fade)
    if ((dtype == kTemp && dn < 32) || (dtype == kColorOut && dn == 0)) {
      Form nf[4];
      const bool sat = ((dest >> 20) & 1u) != 0u, predicated = (tokens[pos] & (1u << 28)) != 0u;
      for (uint32_t c = 0; c < 4 && !sat && !predicated; ++c) {
        if (!(mask & (1u << c))) continue;
        if ((op == 0x42u || op == 0x5Fu || op == 0x5Du) && len >= 3) {  // TEXLD / TEXLDL / TEXLDD: the alpha of a 2D sampler
          const uint32_t samp = tokens[pos + 3];
          const bool proj = op == 0x42u && (((tokens[pos] >> 16) & 0xFFu) & 1u);
          if (!proj && dxsoRegType(samp) == kSampler && dxsoRegNum(samp) < 16 && ((samp >> (16 + 2 * c)) & 3u) == 3u) {
            nf[c].known = true; nf[c].s = (int8_t) dxsoRegNum(samp); nf[c].a.n = 1; nf[c].a.t[0].k = 1.f;
          }
        }
        else if (op == 0x01u && len >= 2) nf[c] = srcForm(tokens[pos + 2], c);
        else if (op == 0x02u && len >= 3) nf[c] = addForm(srcForm(tokens[pos + 2], c), srcForm(tokens[pos + 3], c));
        else if (op == 0x03u && len >= 3) { Form y = srcForm(tokens[pos + 3], c); if (y.in >= 0) y = Form(); if (y.known) { y.a.negate(); y.b.negate(); } nf[c] = addForm(srcForm(tokens[pos + 2], c), y); }
        else if (op == 0x05u && len >= 3) nf[c] = mulForm(srcForm(tokens[pos + 2], c), srcForm(tokens[pos + 3], c));
        else if (op == 0x04u && len >= 4) nf[c] = addForm(mulForm(srcForm(tokens[pos + 2], c), srcForm(tokens[pos + 3], c)), srcForm(tokens[pos + 4], c));
      }
      if (dtype == kTemp) { for (uint32_t c = 0; c < 4; ++c) if (mask & (1u << c)) form[dn][c] = nf[c]; }
      else if (mask & 8u) alphaOut = nf[3];
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
  if (kills == 1 && killOk && killed.s >= 0 && killed.s < 16 && !out.samplers[killed.s].cube) { out.cutSampler = killed.s; out.cutA = killed.a; out.cutB = killed.b; }
  // the fade (milestone 135): oC0.w = an interpolated component - alpha(s), nothing else
  if (!flow && alphaOut.known && alphaOut.s >= 0 && alphaOut.s < 16 && !out.samplers[alphaOut.s].cube && alphaOut.in >= 0
      && alphaOut.a.n == 1 && alphaOut.a.t[0].c < 0 && alphaOut.a.t[0].k == -1.f && alphaOut.b.n == 0) { out.fadeSampler = alphaOut.s; out.fadeInput = alphaOut.in; }
  out.valid = true;
  return true;
}

// A cut-out's a or b with the bound float constants (get(register, component)).
template<typename Get>
inline float cutEval(const CutPoly& p, Get&& get) {
  float v = 0.f;
  for (uint8_t i = 0; i < p.n; ++i) v += p.t[i].k * (p.t[i].c >= 0 ? get((uint32_t) p.t[i].c >> 2, (uint32_t) p.t[i].c & 3u) : 1.f);
  return v;
}
// The D3D alpha test, the one the runtime applies to the albedo, that keeps the texels where
// "a * x + c OP 0" -- x the texel's alpha in steps of 1/255 (0..255), OP one of the four ordered
// comparisons (milestone 151: one converter for the shaders' cut-outs, milestones 67-68, and the trees'
// fade, milestone 135). D3DCMP_GREATEREQUAL or D3DCMP_LESSEQUAL with ref -- every texel passing comes
// out as GREATEREQUAL 0 or LESSEQUAL 255 (alphaTestPassesAll) --, D3DCMP_NEVER when none passes, 0 when
// OP is not an ordered comparison.
inline uint32_t alphaTestFor(float a, float c, uint32_t op, uint32_t& ref) {
  ref = 0;
  const bool greater = op == D3DCMP_GREATER || op == D3DCMP_GREATEREQUAL;
  if (!greater && op != D3DCMP_LESS && op != D3DCMP_LESSEQUAL) return 0u;
  const bool strict = op == D3DCMP_GREATER || op == D3DCMP_LESS;
  if (a == 0.f) {   // a constant: every texel or none
    const bool pass = greater ? (strict ? c > 0.f : c >= 0.f) : (strict ? c < 0.f : c <= 0.f);
    return pass ? (uint32_t) D3DCMP_GREATEREQUAL : (uint32_t) D3DCMP_NEVER;
  }
  const float t = -c / a;   // the comparison with x, mirrored when a is negative
  if (greater == (a > 0.f)) {                                                   // x > t, or x >= t
    const float r = strict ? std::floor(t + 1e-3f) + 1.f : std::ceil(t - 1e-3f);
    if (r > 255.f) return (uint32_t) D3DCMP_NEVER;
    ref = r <= 0.f ? 0u : (uint32_t) r;
    return (uint32_t) D3DCMP_GREATEREQUAL;
  }
  const float r = strict ? std::ceil(t - 1e-3f) - 1.f : std::floor(t + 1e-3f);  // x < t, or x <= t
  if (r < 0.f) return (uint32_t) D3DCMP_NEVER;
  ref = r >= 255.f ? 255u : (uint32_t) r;
  return (uint32_t) D3DCMP_LESSEQUAL;
}
inline bool alphaTestPassesAll(uint32_t func, uint32_t ref) {
  return (func == D3DCMP_GREATEREQUAL && ref == 0u) || (func == D3DCMP_LESSEQUAL && ref == 255u);
}

// The plants of a SpeedTree draw grouped by their cut (milestone 139). SpeedTree draws up to eight
// plants at once, each with its own fade (shader instancing); each plant's fade turned into the
// runtime's test -- the game's "fade - alpha OP ref/255", times 255: -x + 255 * fade - ref OP 0
// (alphaTestFor) --, plants with the same reference form a group, and a plant faded
// out entirely (D3DCMP_NEVER) belongs to none (out). func: the comparison of every group -- 0 when
// the game's test cannot be turned, D3DCMP_NEVER when every plant is faded out.
struct FadeGroups {
  static constexpr uint32_t kPlants = 8;
  uint32_t func = 0, count = 0, ref[kPlants] = {};
  uint8_t members[kPlants] = {}, out = 0;
  bool split() const { return count > 1 || (count == 1 && out != 0); }   // the draw goes out once per group
};
inline FadeGroups groupFades(uint32_t gameFunc, uint32_t gameRef, const float* fades, uint32_t n) {
  FadeGroups g;
  for (uint32_t i = 0; i < n && i < FadeGroups::kPlants; ++i) {
    uint32_t r = 0;
    const uint32_t f = alphaTestFor(-1.f, 255.f * fades[i] - (float) gameRef, gameFunc, r);
    if (!f) return FadeGroups();
    if (f == (uint32_t) D3DCMP_NEVER) { g.out |= (uint8_t) (1u << i); continue; }
    g.func = f;
    uint32_t j = 0;
    while (j < g.count && g.ref[j] != r) ++j;
    if (j == g.count) g.ref[g.count++] = r;
    g.members[j] |= (uint8_t) (1u << i);
  }
  if (!g.func && g.out) g.func = (uint32_t) D3DCMP_NEVER;
  return g;
}

// The albedo for a draw of an untabled pixel shader: among the samplers that reach the colour
// with a coordinate taken straight from an input, and hold a bound 2D colour texture, a
// compressed one whenever there is one -- the game's colour textures are DXT; its light maps,
// which it makes itself, are not (milestone 76: the lot's 1024x512 light map outscored a 16x64
// wall trim by size, and objects' 256x128 sky-light maps their smaller colour textures, so the
// runtime lit baked light) -- and within that, the one scoring highest: larger textures, lower
// coordinate indices. Returns false when none.
inline bool chooseAutoAlbedo(const PsAnalysis& a, const bool color2D[16], const uint32_t fmt[16], const uint16_t w[16], const uint16_t h[16], int& stage, int& texcoord) {
  float best = -1e9f; bool bestCompressed = false; stage = -1; texcoord = -1;
  for (int s = 0; s < 16; ++s) {
    const PsSamplerUse& u = a.samplers[s];
    if (!u.read || !u.reachesColor() || u.dependent || u.projective || u.cube || u.texcoord < 0) continue;
    if (!color2D[s]) continue;                                    // a 2D colour texture that is not a render target
    const bool compressed = fmt[s] == (uint32_t) D3DFMT_DXT1 || fmt[s] == (uint32_t) D3DFMT_DXT2 || fmt[s] == (uint32_t) D3DFMT_DXT3 || fmt[s] == (uint32_t) D3DFMT_DXT4 || fmt[s] == (uint32_t) D3DFMT_DXT5;
    const float area = (float) w[s] * (float) h[s];
    // all three channels reaching the colour is the signature of an albedo; a mask or a
    // gloss map contributes one channel, and a tie between two candidates goes to the later
    // sampler (the game binds masks below their albedo)
    const float score = std::log2((std::max)(area, 1.f)) - 2.f * u.texcoord + 2.f * u.colorChannels + 0.01f * s;
    if (stage < 0 || (compressed && !bestCompressed) || (compressed == bestCompressed && score > best)) { best = score; bestCompressed = compressed; stage = s; texcoord = u.texcoord; }
  }
  return stage >= 0;
}

// ---- glass (milestones 80-107) -----------------------------------------------------------------
// The game's glass is one family of pixel shaders: the view reflected about the normal into an
// environment cube, sharp highlights from the four lights, a Fresnel term. It comes in three forms:
//  1. cube only, blended over what lies behind -- window panes, shower stalls, glass tables, a passing
//     car's windows (isGlassShader, milestone 80);
//  2. with the scene behind -- the 1024 render target read at the pixel, shifted by the normal --
//     drawn opaque: clear, a colour texture over the reflection, or a normal map bending the scene
//     behind (bumpy glass: the shower door);
//  3. glass passes of other shaders: a parked car's paint shader drawn blended for its windows, a
//     distant car's small glass shader with a constant alpha, a textured glass shader (a dome).
// The runtime drops a draw whose stage 0 is a cube map (no hash: the panes looked empty) and takes a
// 2D texture there as an opaque albedo, so every glass draw goes out with one of the hook's markers at
// stage 0 and blending off; the Sims3Glass mod (sims3/remix-mod/Sims3Glass/mod.usda) makes each
// marker's hash a glass (kGlassMaterial): clear (thin, IOR 1.5), car glass (tinted, about 75 % through)
// or the plumbob's (green, milestone 114; no glow since 115; thin since 116). Bumpy glass
// is the clear glass with the game's own bump map (milestone 109, the user's choice over the frosted
// glass of milestones 88-108): the bump map itself at stage 0, its hash naming its material in the
// Sims3GlassBumps mod (sims3GlassBump). Forms 2 and 3 share their samplers' signature with
// the Sims' hair and skin passes and with objects fading in (an object's own shader lerping the scene
// behind by a constant: e.g. 7b3cb6be, a8c64e11, the cars' 834b2191 -- not glass), so they are named,
// each read from its bytecode. The table is the survey of milestone 107: every pixel shader the logs
// recorded through run 217 (3210 dumped) that reads an environment cube and the scene behind or only
// cubes.
// (Not glass, though alike: the light-beam cards 7304aaea / 8d3a3a22 -- their cube lookup has a
// constant direction, no normal.)
inline bool isGlassShader(const PsAnalysis& a) {
  if (!a.valid) return false;
  int cubes = 0;
  for (const PsSamplerUse& u : a.samplers) { if (!u.read) continue; if (!u.cube) return false; ++cubes; }
  return cubes > 0;
}
// The glass materials (milestones 80, 105, 114): each its marker -- a flat 32x32 colour, what shows if
// the mod is not loaded -- whose XXH3-64 names its material in sims3/remix-mod/Sims3Glass/mod.usda.
struct GlassMaterial { const char* name; uint32_t colour; uint64_t hash; };
inline constexpr uint8_t kClearGlass = 0, kCarGlass = 1, kPlumbob = 2, kGlassMaterials = 3;
inline const GlassMaterial kGlassMaterial[kGlassMaterials] = {
  { "glass", 0xFFB8C8D0u, 0x5E30D0B82C246E6Cull },       // pale grey-blue: thin, clear
  { "car glass", 0xFFA0B4ACu, 0x8A5EDD7D16D8E741ull },   // grey-green: thin, tinted (0.72, 0.78, 0.75)
  { "plumbob", 0xFF40E060u, 0x55B2C95B88DA3E67ull },     // green: thin green glass (milestone 116; a solid gem trailed the Sim)
};
// bumpStage: the sampler of a bumpy glass's bump map; bumpScaleReg: the pixel shader constant whose x
// scales its slopes (-1: none). (A blendedPassOnly flag for "the parked car's windows", 45c7a7cd, went in
// milestone 134: that shader is the Sims' hair outdoors.)
struct NamedGlass { uint64_t hash; const char* name; uint8_t material; int8_t bumpStage = -1; int8_t bumpScaleReg = -1; };
inline const NamedGlass kNamedGlass[] = {
  // form 2, clear: the scene behind, no normal map
  { 0x2b1da1b45f51d3f9ull, "the plumbob over the active Sim (run 219), ps_2_0: reflection, highlights, Fresnel, the scene behind (s1) x c10 "
                           "-- its green (VS ddc6be9f; 966k draws in 155 runs) -> green glass (the user: milestone 114; no glow 'for now', 115)", kPlumbob },
  { 0x85e9c3381d5bf054ull, "glass, a decorative glass object on a table (run 218): reflection, Fresnel, the scene behind (s1) lerped to c10 "
                           "(VS b3e88e28, skinned; also VS d251510d; runs 201-218)", kClearGlass },
  { 0x8ff495765d26a6fdull, "glass: as 85e9c338 (VS 2 kinds; 21 runs)", kClearGlass },
  { 0x7eeb349a23cbefefull, "glass: as 85e9c338, a colour texture s2 x c11 over the reflection (4 runs)", kClearGlass },
  { 0x910a56f24813e248ull, "glass, an unplayable lot's windows, outside (run 218; VS c96f1465): as 572773cf, its colour s3 x c10 tinting the "
                           "scene behind -- the clear glass as the household windows, its bump map (slopes x 5) left out: the user's choice", kClearGlass },
  // form 2, bumpy: a normal map bends the scene behind -- its x in alpha, its y in blue, at TEXCOORD5,
  // the slopes scaled by c14.x, z rebuilt (n = s x T - s y B + z N, B = N x T times the tangent's w)
  { 0x572773cfbd618a3aull, "bumpy glass, a shower door: bump map s2, the scene behind (s1) read through it, colour s3 over the reflection "
                           "(VS b51f1577, skinned: the door swings; runs 192-217)", kClearGlass, 2, 14 },
  { 0x8fe3ce7c5fbc6234ull, "bumpy glass: as 572773cf, its colour s3 x c10 tinting the scene behind (16 runs)", kClearGlass, 2, 14 },
  // form 3
  { 0xac4184cee232ed04ull, "glass: colour texture s2 x c10, gloss s3, irradiance cube s1 (VS d7fede81; an unplayable lot's dome, runs 212, 218)", kClearGlass },
  { 0xd03ebab11453bca1ull, "car glass, a distant car's windows: ps_2_0 reflection, Fresnel, highlights, the car's atlas s1 at a decoded UV, "
                           "constant alpha c6.w (VS 4e9298de; runs 159, 205)", kCarGlass },
  // form 1, named for its material
  { 0x66516d5db94ab307ull, "car glass, a passing car's windshield and windows: cube only (VS e79a4bf1, skinned; run 215)", kCarGlass },
};
inline const NamedGlass* namedGlass(uint64_t hash) { return findByHash(kNamedGlass, hash); }
// (The glass survey, milestones 110 to 118, showed every glass shader not yet identified in a flat colour
// of its own -- pink at the end -- until the user came across it (runs 218-221 named ten). Retired in
// milestone 146: the four left, 8fe3ce7c, 8ff49576, d03ebab1 and 7eeb349a, go out as their named glass.
// A window's two sides are two draws: 3197bfde facing a room, 98e23f47 facing outdoors -- a window on an
// indoor wall is 3197bfde on both sides, one on a free-standing outdoor wall 98e23f47 on both, run 218.)
// The material hashes a Remix mod names (def Material "mat_<16 hex>"): which of the game's bump maps
// the Sims3GlassBumps mod has a bumpy glass for (milestone 109).
inline std::vector<uint64_t> modMaterialHashes(const std::string& usda) {
  std::vector<uint64_t> out;
  static const char kTag[] = "Material \"mat_";
  for (size_t at = usda.find(kTag); at != std::string::npos; at = usda.find(kTag, at + 1)) {
    const size_t p = at + sizeof kTag - 1;
    if (p + 16 > usda.size()) break;
    uint64_t v = 0; bool ok = true;
    for (size_t i = 0; i < 16 && ok; ++i) {
      const char ch = usda[p + i]; v <<= 4;
      if (ch >= '0' && ch <= '9') v |= (uint64_t) (ch - '0');
      else if (ch >= 'A' && ch <= 'F') v |= (uint64_t) (ch - 'A' + 10);
      else if (ch >= 'a' && ch <= 'f') v |= (uint64_t) (ch - 'a' + 10);
      else ok = false;
    }
    if (ok) out.push_back(v);
  }
  return out;
}
// ---- a glass sheet's back side (milestone 104) -------------------------------------------------
// A glass sheet the game models with both sides -- a passing car's windshield (all 36 triangles, run
// 215), the shower door's panel (8 of its 20, runs 215-216) -- has its two sides on one plane, facing
// opposite ways. The game culls the side turned away; the ray tracer meets both in the same place: the
// windshield clipped and grainy at angles (run 217 with one side: "a little noisy when moving the
// camera"; the door's frost still goes with distance, so not this). On a plane that carries both
// facings within one draw, the triangles of the facing met second are left out. Planes are compared
// to 1/32 in direction and 0.01 in offset (the mesh's own units); a pane with a thickness has its
// sides on two planes and keeps both.
inline uint64_t glassPlaneKey(const float* a, const float* b, const float* c, uint8_t& facing) {
  const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
  float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
  const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  facing = 0;
  if (!(len > 1e-12f)) return 0;   // degenerate: no plane
  for (float& x : n) x /= len;
  int32_t q[4] = { (int32_t) std::lround(n[0] * 32.f), (int32_t) std::lround(n[1] * 32.f), (int32_t) std::lround(n[2] * 32.f),
                   (int32_t) std::lround((n[0] * a[0] + n[1] * a[1] + n[2] * a[2]) / 0.01f) };
  facing = 1;
  const int32_t lead = q[0] ? q[0] : q[1] ? q[1] : q[2];
  if (lead < 0) { for (int32_t& x : q) x = -x; facing = 2; }
  return 1ull << 63 | (uint64_t) (uint8_t) (q[0] + 64) | ((uint64_t) (uint8_t) (q[1] + 64) << 8) | ((uint64_t) (uint8_t) (q[2] + 64) << 16) | ((uint64_t) (uint32_t) q[3] << 24);
}
// pos: nine floats per triangle (its corners); keep: one flag per triangle. Returns how many are left out.
inline uint32_t glassFrontTriangles(const float* pos, size_t triangles, std::vector<uint8_t>& keep) {
  keep.assign(triangles, 1);
  std::vector<uint64_t> keys(triangles); std::vector<uint8_t> facings(triangles);
  std::unordered_map<uint64_t, uint8_t> first;   // a plane -> the facing met first
  for (size_t t = 0; t < triangles; ++t) {
    keys[t] = glassPlaneKey(pos + t * 9, pos + t * 9 + 3, pos + t * 9 + 6, facings[t]);
    if (keys[t]) first.emplace(keys[t], facings[t]);
  }
  uint32_t dropped = 0;
  for (size_t t = 0; t < triangles; ++t) if (keys[t] && facings[t] != first[keys[t]]) { keep[t] = 0; ++dropped; }
  return dropped;
}
// ---- mirrors (milestone 101) -----------------------------------------------------------------
// A reflective sheet -- a cube-only pixel shader under the stencil test, PS 86dad57d (VS d79254da):
// the view reflected about the normal into an environment cube, fogged, its alpha from material
// constants -- is a mirror's face (run 212's survey: every sheet showed on a mirror). The game fills
// the mirror with a mirrored-camera pass (left out: the ray tracer reflects by itself) and lays this
// sheet over it; left out too since milestone 82, the mirrors were blank (the wall behind them). It
// goes out with the mirror marker, blending off: the Sims3Glass mod's opaque, fully metallic, smooth
// material names no albedo, so the marker's colour is the mirror's tint (mergeLegacyMaterial).
inline bool isReflectiveSheet(const PsAnalysis& a, DWORD stencilEnable) { return stencilEnable && isGlassShader(a); }
inline constexpr uint32_t kMirrorMarkerColour = 0xFFF0F2F2u;          // ARGB silver: the mirror's tint
inline constexpr uint64_t kMirrorMarkerHash = 0x27198C264F1C40A5ull;  // XXH3-64 of its level 0; the mod's material name
inline constexpr uint32_t kGlassMarkerSize = 32;

// ---- water (milestones 86, 89-93) --------------------------------------------------------------
// The game paints its water's look in the shader -- a reflection, bump maps, caustics, a colour or
// foam texture, the scene behind from a render target -- over waves made in the vertex shader; the
// runtime took one of those textures as an albedo (an opaque grey plane, run 193; a pool's rippled
// light-blue colour, run 196). A water draw is presented with one of the hook's water markers at
// stage 0, blending off; the hook's Remix mod makes each marker's hash a translucent water VOLUME,
// IOR 1.33 -- refraction, and an absorption that deepens with the depth to the bed (the runtime
// tracks whether a ray is inside the medium and turns the surface to face it, so the sheet's own
// facing does not matter). One material per look (milestone 99, kWaterMaterial): a lot pool, a pond,
// the sea (the town's water), an object's water -- each its own marker, tint and ripple map, in the
// hook's Sims3Water mod (the glass stays in Sims3Glass). The ground or basin under it is the game's
// own (a pond's bed, a lot pool's walls and floor: run 197). Ripples (milestone 94): each material's
// normal map is made from the game's own wave map (sims3/remix-mod/make_textures.py) and sampled with
// the draw's captured TEXCOORD0 -- the game's scrolling wave coordinate -- with the game's own
// sampler states for that wave map moved to stage 0, where the runtime takes them for the material
// (its wrap above all: one pool shader has a reflection cube at stage 0). Named, each read from its
// bytecode.
// (Not water: PS f45e6c60, VS 33017462 -- a lot pool's FLOOR, a floor tile s4 under caustics s3, its
// normal map, reflection and Fresnel; taken for the pool's surface in milestone 89, run 198 showed the
// pool's surface itself is 011ba470 below.)
// The water materials (milestone 99): each look's marker -- a flat 32x32 colour, what shows if the mod is
// not loaded -- whose XXH3-64 names its material in sims3/remix-mod/Sims3Water/mod.usda, and its ripple map
// there (made by make_textures.py, the strength baked in). Pool and pond keep the hashes of milestone 93's
// clear and natural water.
struct WaterMaterial { const char* name; uint32_t colour; uint64_t hash; const char* ripples; };
inline constexpr uint8_t kWaterPool = 0, kWaterPond = 1, kWaterSea = 2, kWaterObject = 3, kWaterMaterials = 4;
inline const WaterMaterial kWaterMaterial[kWaterMaterials] = {
  { "pool", 0xFF8CC4C8u, 0x2723DD62C28E1456ull, "water_pool_n.dds" },             // pale teal
  { "pond", 0xFF6E9C8Cu, 0x52B04DF3E566CA55ull, "water_pond_n.dds" },             // muted green-teal
  { "sea", 0xFF5A86A0u, 0xDDC84353084D6462ull, "water_sea_n.dds" },               // slate blue
  { "object water", 0xFFA0D0D8u, 0x109A6039DC3F71F1ull, "water_object_n.dds" },   // light aqua
};
struct WaterShader { uint64_t hash; const char* name; uint8_t material; };   // material: kWaterPool ... kWaterObject
inline const WaterShader kWaterPs[] = {
  { 0xf74b4657dbfd60bcull, "the town's water, ponds and sea (VS 2a6edce6: a plane at a set height, waves, refraction and reflection targets, "
                           "two bump maps; its NORMAL input is no normal here, see chooseNormalTexcoord)", kWaterSea },
  { 0x387e1a15c63c120aull, "water, instanced (VS 1a047c76: waves, refraction target, reflection cube, a two-sample bump map)", kWaterObject },
  // milestone 108 (the glass survey of milestone 107): until now sent opaque, a bump map or a ramp taken as the albedo
  { 0xd92d3913aba53da8ull, "an object's water, ps_3_0: as 387e1a15 -- the bump map s2 read at two coordinates, the scene behind s1 "
                           "shifted by it, reflection cube s0 (2 VS kinds; 28 runs)", kWaterObject },
  { 0xaf6cd85bfab9f36dull, "an object's water, ps_3_0: as d92d3913 (1 run)", kWaterObject },
  { 0xdb58e590608be737ull, "open water (VS defcc84d): as the town's water -- two scrolling signed wave maps s0 / s1 (Q8W8V8U8), the scene "
                           "behind s4 shifted by them, reflection cube s2, the glint ramp s3 (DXT1 512x4) -- with a shore mask s6 "
                           "(a cut-out) and the fog; 38 runs", kWaterSea },
  { 0xd40999e5838e8b05ull, "a lot pool's surface (VS 011ba470, a byte-packed grid): two scrolling wave normal maps s0 / s1, the scene behind "
                           "(render target s2) and the reflection (render target s3) read through them; captured, the wave map was its "
                           "albedo -- the slow-moving lavender 'normal map' of runs 194-198", kWaterPool },
  { 0x85c0a78a614b15d3ull, "a lot pool's surface, cube-reflected (VS 011ba470): wave maps s1 / s2, the scene behind s3, reflection cubes s0 / s4", kWaterPool },
  { 0x11a6bdfd3e77d03aull, "a pond's surface (VS 24bd4713, a byte-packed mesh of its own, no culling; drawn right after the town's water): "
                           "two scrolling signed wave maps s0 / s1 (Q8W8V8U8), reflection cubes s2 / s3, a sun-glint ramp s4 (DXT1 512x4), "
                           "the shadow map s5, the scene behind s6; captured, the glint ramp was its albedo -- the flat white pond of runs 194-199", kWaterPond },
};
inline bool isWaterPs(uint64_t hash) { return findByHash(kWaterPs, hash) != nullptr; }
inline int waterMaterial(uint64_t hash) { const WaterShader* w = findByHash(kWaterPs, hash); return w ? (int) w->material : -1; }
// The game's wave maps (milestone 93): a water draw's 2D textures in these formats are written to
// rtx-remix\logs\sims3-textures when missing, level 0 as stored -- the source of the mod's ripple maps.
inline bool isWaveMapFormat(uint32_t fmt) { return fmt == 21u /* A8R8G8B8 */ || fmt == 22u /* X8R8G8B8 */ || fmt == 63u /* Q8W8V8U8 */; }

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

// ---- the game's own lamp lights --------------------------------------------------------
// The game keeps every lamp model's lights in a LITE resource: type, position in the model's
// own space, colour, intensity, shape. sims3/tools/lite_table.py reads the package files and
// writes sims3lights.txt next to this DLL (format 3): the lights per model, and which object
// names which model. A lamp is looked up by the keys the lamp reporter gives for it; no mesh is
// involved (milestones 22 to 38 named a lamp by the mesh it was drawn with).
//
// The shape: at = the definition's direction, which points from the lit side BACK to the light
// (a picture light's points away from its wall, a street lamp's and a skylight's up, a
// fountain's underwater light's down): the light travels along -at. d by type: spot = cone
// angle, blur; lamp shade and world light = cone angle (around -at), shade multiplier, bottom
// angle (the cone around +at), shade r g b (the light through the shade); tube = length, blur.
struct LiteLight { uint8_t type; float pos[3]; float col[3]; float intensity; float at[3]; float d[6]; };   // type 3 point, 4 spot, 5 lamp shade, 6 tube, 11 world light
struct LiteModel { uint64_t inst; uint8_t n; LiteLight lights[4]; };
struct LiteTable {
  std::vector<LiteModel> models;
  std::unordered_map<uint64_t, uint32_t> byModel;   // a model's instance -> its place in models
  bool loaded = false; uint32_t lines = 0; int format = 0;
  // A lamp's definition by the catalog model key the reporter gives for it (milestone 39; the
  // object lines of the table -- an object naming its model -- never found one, milestone 145).
  const LiteModel* model(uint64_t inst) const {
    const auto it = byModel.find(inst);
    return it == byModel.end() ? nullptr : &models[it->second];
  }
};
// One line of sims3lights.txt. True when it gave the table something.
inline bool liteParseLine(LiteTable& t, const char* line) {
  if (line[0] == '#' || line[0] == '\r' || line[0] == '\n' || !line[0]) return false;
  if (!strncmp(line, "format", 6)) { t.format = (int) strtol(line + 6, nullptr, 10); return true; }
  if (t.format != 3) return false;   // an older table (keyed by meshes) names no lamp
  char* e = nullptr;
  if (!strncmp(line, "model ", 6)) {
    LiteModel m = {};
    m.inst = strtoull(line + 6, &e, 16);
    const long cnt = strtol(e, &e, 10);
    for (long i = 0; i < cnt && m.n < 4; ++i) {
      const long typ = strtol(e, &e, 10);
      float v[16]; for (int k = 0; k < 16; ++k) v[k] = (float) strtod(e, &e);
      if (!((typ >= 3 && typ <= 6) || typ == 11)) continue;
      LiteLight& L = m.lights[m.n++];
      L.type = (uint8_t) typ; L.pos[0] = v[0]; L.pos[1] = v[1]; L.pos[2] = v[2]; L.col[0] = v[3]; L.col[1] = v[4]; L.col[2] = v[5]; L.intensity = v[6];
      for (int k = 0; k < 3; ++k) L.at[k] = v[7 + k];
      for (int k = 0; k < 6; ++k) L.d[k] = v[10 + k];
    }
    if (!m.n) return false;
    t.byModel[m.inst] = (uint32_t) t.models.size(); t.models.push_back(m);
    return true;
  }
  return false;
}
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
  while (fgets(line, sizeof line, f)) { ++t.lines; liteParseLine(t, line); }
  fclose(f);
  return t;
}
// A model-space point through the World rows the object shaders carry (three rows of four:
// row . (x, y, z, 1), the translation in .w).
inline void worldPoint(const float* rows, const float* p, float* out) {
  for (int i = 0; i < 3; ++i) out[i] = rows[4*i] * p[0] + rows[4*i + 1] * p[1] + rows[4*i + 2] * p[2] + rows[4*i + 3];
}
// A model-space direction through the same rows (milestone 32): rotated, not translated, unit
// length. False (and zeros) for a zero vector, which is what a point light carries.
inline bool worldDir(const float* rows, const float* v, float* out) {
  for (int i = 0; i < 3; ++i) out[i] = rows[4*i] * v[0] + rows[4*i + 1] * v[1] + rows[4*i + 2] * v[2];
  const float n = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
  if (n < 1e-6f) { out[0] = out[1] = out[2] = 0.f; return false; }
  for (int i = 0; i < 3; ++i) out[i] /= n;
  return true;
}

// ---- lamps ---------------------------------------------------------------------------------
// A lamp is one of the game's own lights, and everything about it is the game's: WHAT it is
// (its lights and their shapes) from the game's light definitions, looked up by the keys of
// its object; WHERE it is from its object's transform; WHETHER it is on, in which colour and
// at which level from its state. The lamp reporter hands over the keys, the transform and the
// state (above); the table holds the definitions. Nothing is read from what is drawn, so a
// lamp gives its light whether or not it is in view.
//
// A spot is a sphere light shaped to its cone; a lamp shade (and a world light with a
// direction) is two cones -- the definition's angle around the way its light travels, its
// bottom angle the other way -- and an unshaped light for what comes through the shade; a
// tube is a cylinder; a point a sphere.
//
// The book below holds the lights that are lit, one entry per light of a lit lamp, by the
// object's id and the light's index. Each frame the device names the lit ones again; an entry
// not named is put out.
//
// (Milestones 21 to 35 inferred the state -- from the light rigs' rays, then from the lot's
// light maps -- and milestones 22 to 38 named a lamp by the mesh it was drawn with. Both were
// removed, with milestones 37 and 39; the run journal holds what was learnt.)
struct Lamp {
  uint64_t object;     // the object's id
  uint8_t light;       // which of the model's lights
  uint8_t kind;        // how it is sent: 3 a sphere, 4 a cone, 5 two cones and the shade's light, 6 a cylinder
  uint32_t id;         // the API light's hash, from the object's id and the light's index
  float pos[3];        // the light, in the world
  float col[3];        // colour x brightness as forwarded
  float dir[3];        // the way the main cone's light travels (unit; zeros = no direction)
  float angle;         // the main cone's angle from its axis in degrees (0 = none)
  float bottom;        // the opposite cone's angle (a lamp shade's bottom opening; 0 = none)
  float shade[3];      // the light through a lamp shade, as a factor of the lamp's colour (zeros = none)
  float tube;          // a tube light's length along dir (0 = none)
  bool seen;           // named this frame
  void* api;           // the Remix API lights: the main one, the opposite cone's, the shade's
  void* api2;
  void* api3;
  float sentPos[3], sentCol[3], sentDir[3]; bool sent;   // what the runtime holds
};

inline float lampDist(const float* a, const float* b) {
  const float d[3] = { a[0]-b[0], a[1]-b[1], a[2]-b[2] };
  return len3(d);
}

struct Lamps {
  static const int kMax = 256;                 // lights (a lamp has up to four)
  Lamp lamps[kMax]; uint32_t n = 0;
  uint32_t lit = 0, out = 0;                   // statistics: lights lit and put out
  void* gone[3 * kMax]; uint32_t nGone = 0;    // the API lights of the entries put out this frame, for the device to destroy

  static uint32_t lightId(uint64_t object, uint8_t light) {
    uint32_t h = 2166136261u;
    for (int b = 0; b < 8; ++b) { h ^= (uint32_t) ((object >> (8 * b)) & 0xFF); h *= 16777619u; }
    h ^= (uint32_t) light + 1u; h *= 16777619u;
    return h ? h : 1u;
  }
  Lamp* find(uint64_t object, uint8_t light) {
    for (uint32_t k = 0; k < n; ++k) if (lamps[k].object == object && lamps[k].light == light) return &lamps[k];
    return nullptr;
  }
  // Frame start: nothing named yet, nothing handed over.
  void begin() { nGone = 0; for (uint32_t k = 0; k < n; ++k) lamps[k].seen = false; }
  // A light of a lit lamp, named this frame: its entry, found or made (fresh). Null when the book is full.
  Lamp* name(uint64_t object, uint8_t light, bool& fresh) {
    Lamp* L = find(object, light);
    fresh = L == nullptr;
    if (!L) {
      if (n >= (uint32_t) kMax) return nullptr;
      L = &lamps[n++];
      std::memset(L, 0, sizeof *L);
      L->object = object; L->light = light; L->id = lightId(object, light);
      ++lit;
    }
    L->seen = true;
    return L;
  }
  // Its API lights to the device, to destroy.
  void handOver(Lamp& L) {
    void** hs[3] = { &L.api, &L.api2, &L.api3 };
    for (int q = 0; q < 3; ++q) if (*hs[q]) { if (nGone < (uint32_t) (3 * kMax)) gone[nGone++] = *hs[q]; *hs[q] = nullptr; }
    L.sent = false;
  }
  // Frame end: the entries not named are put out. Returns how many are left, all lit.
  uint32_t end() {
    for (uint32_t k = 0; k < n; ) {
      Lamp& L = lamps[k];
      if (!L.seen) { handOver(L); L = lamps[n - 1]; --n; ++out; }
      else ++k;
    }
    return n;
  }
};

// Index i of an index buffer's bytes, 16- or 32-bit.
inline uint32_t readIndex(const uint8_t* data, size_t i, bool ib32) {
  if (ib32) { uint32_t v; std::memcpy(&v, data + i * 4u, 4); return v; }
  uint16_t v; std::memcpy(&v, data + i * 2u, 2); return v;
}
inline uint64_t fnv1a64(const void* bytes, size_t len) {
  const unsigned char* p = static_cast<const unsigned char*>(bytes);
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
  return h;
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

// ---- the hook's own device calls (milestone 152) ------------------------------------------------------
// One guard and one undo log for what the hook does on the device itself. own > 0 while the hook calls
// the device: the setters' hooks leave the game's facts alone (the bound shaders, textures and layout,
// the stage-0 states, the terrain block's held states, the camera's constants), the shader-creation hooks
// make no tables and the draw hooks ignore a draw (the restore quad). Every state the hook changes for a
// draw is held in the log: the device's value saved the first time the current scope takes the state,
// only differences sent, and everything put back newest first at the end of the scope -- the draw's
// (sims3EndDraw), or a section's inside it (the low-detail lot's plate and glow, the restore quad) or
// around a re-issued draw (the composite's second pass). (Until milestone 152 eight flags guarded the
// setters, each some of them -- the restore quad's sampler calls came under none and cancelled the
// terrain block's sRGB hold -- and every change kept a saved value and a flag of its own, put back one by
// one in sims3EndDraw.) A full log leaves the state alone and counts it (full).
struct HookCalls {
  enum : uint8_t { kRs = 1, kSampler, kStage, kTexture, kPs, kVs, kViewport, kWorld };
  struct Entry { uint8_t kind = 0, stage = 0; uint16_t type = 0; DWORD value = 0; IUnknown* object = nullptr; };
  struct Scope { uint32_t base = 0, prev = 0; };
  struct Guard {   // the hook's own call, for its lifetime
    HookCalls& c;
    explicit Guard(HookCalls& x) : c(x) { ++c.own; }
    ~Guard() { --c.own; }
    Guard(const Guard&) = delete; Guard& operator=(const Guard&) = delete;
  };
  static constexpr uint32_t kEntries = 64;
  uint32_t own = 0;
  Entry log[kEntries]; uint32_t count = 0, scope = 0, full = 0;
  D3DVIEWPORT9 viewport = {}; D3DMATRIX world = {};   // the saved viewport and WORLD transform (the sky dome's, a glass draw's: one each)

  bool holds(uint8_t kind, uint32_t stage, uint32_t type) const {   // whether the current scope holds the state
    for (uint32_t i = scope; i < count; ++i) if (log[i].kind == kind && log[i].stage == stage && log[i].type == type) return true;
    return false;
  }
  bool holdsRs(D3DRENDERSTATETYPE s) const { return holds(kRs, 0, (uint32_t) s); }
  Scope open() { const Scope s = { count, scope }; scope = count; return s; }
  template<typename Dev> void close(Dev* dev, Scope s) { undoTo(dev, s.base); scope = s.prev; }
  // a device reset: the saved objects released, nothing set (the device is back to its defaults)
  void forget() {
    for (uint32_t i = 0; i < count; ++i) { if (log[i].object) log[i].object->Release(); log[i] = Entry(); }
    count = scope = 0; own = 0;
  }

  template<typename Dev> void holdRs(Dev* dev, D3DRENDERSTATETYPE s, DWORD v) {
    Guard g(*this);
    DWORD now = 0; dev->GetRenderState(s, &now);
    if (take(kRs, 0, (uint32_t) s, now, nullptr) && now != v) dev->SetRenderState(s, v);
  }
  template<typename Dev> void holdSampler(Dev* dev, DWORD stage, D3DSAMPLERSTATETYPE t, DWORD v) {
    Guard g(*this);
    DWORD now = 0; dev->GetSamplerState(stage, t, &now);
    if (take(kSampler, stage, (uint32_t) t, now, nullptr) && now != v) dev->SetSamplerState(stage, t, v);
  }
  template<typename Dev> void holdStage(Dev* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    Guard g(*this);
    DWORD now = 0; dev->GetTextureStageState(stage, t, &now);
    if (take(kStage, stage, (uint32_t) t, now, nullptr) && now != v) dev->SetTextureStageState(stage, t, v);
  }
  template<typename Dev> void holdTexture(Dev* dev, DWORD stage, IDirect3DBaseTexture9* t) {
    Guard g(*this);
    IDirect3DBaseTexture9* now = nullptr; dev->GetTexture(stage, &now);   // a reference, the entry's (or released by take)
    const bool same = now == t;
    if (take(kTexture, stage, 0, 0, now) && !same) dev->SetTexture(stage, t);
  }
  template<typename Dev> void holdPs(Dev* dev, IDirect3DPixelShader9* ps) {
    Guard g(*this);
    IDirect3DPixelShader9* now = nullptr; dev->GetPixelShader(&now);
    const bool same = now == ps;
    if (take(kPs, 0, 0, 0, now) && !same) dev->SetPixelShader(ps);
  }
  template<typename Dev> void holdVs(Dev* dev, IDirect3DVertexShader9* vs) {
    Guard g(*this);
    IDirect3DVertexShader9* now = nullptr; dev->GetVertexShader(&now);
    const bool same = now == vs;
    if (take(kVs, 0, 0, 0, now) && !same) dev->SetVertexShader(vs);
  }
  template<typename Dev> void holdViewport(Dev* dev, const D3DVIEWPORT9& vp) {
    Guard g(*this);
    D3DVIEWPORT9 now = {}; dev->GetViewport(&now);
    const bool had = holds(kViewport, 0, 0);
    if (!take(kViewport, 0, 0, 0, nullptr)) return;
    if (!had) viewport = now;
    dev->SetViewport(&vp);
  }
  template<typename Dev> void holdWorld(Dev* dev, const D3DMATRIX& m) {
    Guard g(*this);
    D3DMATRIX now = {}; dev->GetTransform(D3DTS_WORLD, &now);
    const bool had = holds(kWorld, 0, 0);
    if (!take(kWorld, 0, 0, 0, nullptr)) return;
    if (!had) world = now;
    dev->SetTransform(D3DTS_WORLD, &m);
  }
  // the log back to `to` entries, newest first: each state set back where it differs
  template<typename Dev> void undoTo(Dev* dev, uint32_t to) {
    Guard g(*this);
    while (count > to) {
      Entry& e = log[--count];
      switch (e.kind) {
        case kRs: { DWORD now = 0; dev->GetRenderState((D3DRENDERSTATETYPE) e.type, &now); if (now != e.value) dev->SetRenderState((D3DRENDERSTATETYPE) e.type, e.value); break; }
        case kSampler: { DWORD now = 0; dev->GetSamplerState(e.stage, (D3DSAMPLERSTATETYPE) e.type, &now); if (now != e.value) dev->SetSamplerState(e.stage, (D3DSAMPLERSTATETYPE) e.type, e.value); break; }
        case kStage: { DWORD now = 0; dev->GetTextureStageState(e.stage, (D3DTEXTURESTAGESTATETYPE) e.type, &now); if (now != e.value) dev->SetTextureStageState(e.stage, (D3DTEXTURESTAGESTATETYPE) e.type, e.value); break; }
        case kTexture: {
          IDirect3DBaseTexture9* saved = static_cast<IDirect3DBaseTexture9*>(e.object), *now = nullptr; dev->GetTexture(e.stage, &now);
          if (now != saved) dev->SetTexture(e.stage, saved);
          if (now) now->Release();
          break;
        }
        case kPs: {
          IDirect3DPixelShader9* saved = static_cast<IDirect3DPixelShader9*>(e.object), *now = nullptr; dev->GetPixelShader(&now);
          if (now != saved) dev->SetPixelShader(saved);
          if (now) now->Release();
          break;
        }
        case kVs: {
          IDirect3DVertexShader9* saved = static_cast<IDirect3DVertexShader9*>(e.object), *now = nullptr; dev->GetVertexShader(&now);
          if (now != saved) dev->SetVertexShader(saved);
          if (now) now->Release();
          break;
        }
        case kViewport: dev->SetViewport(&viewport); break;
        case kWorld: dev->SetTransform(D3DTS_WORLD, &world); break;
        default: break;
      }
      if (e.object) e.object->Release();
      e = Entry();
    }
  }

 private:
  // the state's entry in the current scope, made with the saved value when there is none: false when
  // the log is full (the state is left alone); an object passed in is the entry's, or released
  bool take(uint8_t kind, uint32_t stage, uint32_t type, DWORD value, IUnknown* object) {
    if (holds(kind, stage, type)) { if (object) object->Release(); return true; }
    if (count >= kEntries) { ++full; if (object) object->Release(); return false; }
    Entry& e = log[count++];
    e.kind = kind; e.stage = (uint8_t) stage; e.type = (uint16_t) type; e.value = value; e.object = object;
    return true;
  }
};
using OwnCall = HookCalls::Guard;

} // namespace sims3cam

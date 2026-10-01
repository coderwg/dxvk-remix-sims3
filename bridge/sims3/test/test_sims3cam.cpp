// Standalone test of sims3_camera_hook.h and sims3_walls.h against constants, shader bytecode and
// buffers captured from the game (native apitrace and the in-game shader dumps), plus constructed
// regression cases. Run: _test_sims3cam.cmd (writes _test_sims3cam.log). Data that is not on this
// machine is reported as SKIP, not as a failure.
//
// Orientation (settled by run 2's logs and the trace's draw counts): the world is Y-up; the
// play camera looks DOWN (forward.y < 0) from above the sea plane (y ~ 28); the reflection
// pass uses the same camera mirrored across that plane and looks UP. The captured calls
// 1277907 / 1278332 (eye y = -10.7, forward.y = +0.86) are therefore REFLECTION-pass draws.
#include "sims3_camera_hook.h"
#include "sims3_walls.h"
#define XXH_INLINE_ALL
#include "xxhash.h"   // the runtime's texture hash (milestone 17): the marker hashes the hook sends
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

using namespace sims3cam;
static int fails = 0, skips = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  PASS  "); } else { printf("  FAIL  "); ++fails; } printf(__VA_ARGS__); printf("\n"); } while (0)
#define SKIP(...) do { printf("  SKIP  "); ++skips; printf(__VA_ARGS__); printf("\n"); } while (0)
static const char* kindName(Kind k) { return k == Kind::Main ? "Main" : k == Kind::OtherCamera ? "OtherCamera" : "None"; }

// The in-game shader dumps (written by the hook, sims3DumpShader) and the captured buffers.
static const char* kDumpDir = "C:\\Program Files\\EA Games\\The Sims 3\\Game\\Bin\\rtx-remix\\logs\\sims3-shaders\\";
static bool loadBytes(const char* path, std::vector<uint8_t>& out) {
  FILE* f = fopen(path, "rb"); if (!f) return false;
  fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? (size_t) n : 0);
  const size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
  fclose(f);
  return !out.empty() && got == out.size();
}
// A shader dump by name, trimmed to its END token (false when absent or without one).
static bool loadShader(const char* name, std::vector<DWORD>& out) {
  char path[320]; snprintf(path, sizeof path, "%s%s.bin", kDumpDir, name);
  std::vector<uint8_t> bytes;
  if (!loadBytes(path, bytes) || bytes.size() < 8) return false;
  out.resize(bytes.size() / 4); memcpy(out.data(), bytes.data(), out.size() * 4);
  const size_t n = shaderTokenCount(out.data(), out.size());
  if (n < 2) return false;
  out.resize(n);
  return true;
}
// The highest output register a vs_3_0 stream declares or writes.
static uint32_t maxOutputReg(const std::vector<DWORD>& t) {
  uint32_t m = 0;
  dxsoForEach(t.data(), t.size(), [&](size_t pos, uint32_t op, uint32_t len) {
    if (dxsoIsDef(op)) return true;
    for (size_t i = 1; i <= len; ++i) if ((t[pos + i] & 0x80000000u) && dxsoRegType(t[pos + i]) == kDxsoRegOutput) m = (std::max)(m, dxsoRegNum(t[pos + i]));
    return true;
  });
  return m;
}

// call 1277907 (reflection pass): c0..c3 = fused WVP, c4..c6 = World (1152,0,896), c7 param, c8 = eye
static const float k1277907[12*4] = {
  -1.7050f, 0.0000f, 1.7092f, -275.6799f,
   2.6064f,-2.2062f, 2.6000f, -251.4135f,
   0.3639f, 0.8578f, 0.3631f,  -22.8789f,
   0.3639f, 0.8578f, 0.3630f,  -22.6270f,
   1,0,0,1152,   0,1,0,0,   0,0,1,896,   -0.0002f,1.0067f,5.0f,1.0f,
   1115.162f,-10.7015f,1020.544f,1.f,  0,0,0,0,  0,0,0,0,  0,0,0,0 };
// call 1278332 (reflection pass): identity World -> c0..c3 is the pure ViewProjection; c7 = eye
static const float k1278332[12*4] = {
  -1.7050f, 0.0000f, 1.7092f,  157.0656f,
   2.6064f,-2.2062f, 2.6000f,-5583.6380f,
   0.3639f, 0.8578f, 0.3631f, -767.4377f,
   0.3639f, 0.8578f, 0.3630f, -767.1238f,
   0.5662f,0.5576f,0.7744f,1.0f,  0,0,0,0,  -0.0002f,1.0067f,5.0f,1.0f,  1115.16f,-10.70f,1020.54f,1.0f,
   0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };
// call 1276557: 90-degree square shadow camera (rotation real; rejection is decided by fx == fy)
static const float kShadow[12*4] = {
   0.9397f,0,0.3420f,-1050.f,   0,1,0,-45.f,   -0.3426f,0,0.9412f,-1000.5f,   -0.3420f,0,0.9397f,-1000.f,
   0.9397f,0,0.342f,1113.957f,  0,1,0,45.889f,  -0.342f,0,0.9397f,1022.679f,  1125.28f,47.396f,1031.86f,1.0f,
   0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };
// call 1281189: the UI pass (pixel-to-clip scale 2/1920, -2/1080; offset; colour)
static const float kUi[5*4] = {
   0.001041667f,0,0,0,  0,-0.001851852f,0,0,  0,0,0,0,  -0.9890624f,-0.5842593f,0.5f,1,  0.1215686f,0.8509805f,0.1215686f,1 };

static void mulColumn(const float* M, const float p[4], float out[4]) {
  for (int r = 0; r < 4; ++r) out[r] = M[r*4+0]*p[0] + M[r*4+1]*p[1] + M[r*4+2]*p[2] + M[r*4+3]*p[3];
}
static void mulRow(const float p[4], const D3DMATRIX& M, float out[4]) {
  for (int c = 0; c < 4; ++c) out[c] = p[0]*M.m[0][c] + p[1]*M.m[1][c] + p[2]*M.m[2][c] + p[3]*M.m[3][c];
}
static M4 fromRows(const float* r) { M4 m; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) m.m[i][j] = r[i*4+j]; return m; }
static void toBlock(const M4& m, float* c) { for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) c[i*4+j] = m.m[i][j]; }
static bool near3(const float* p, float x, float y, float z) { return std::fabs(p[0]-x) < 1.f && std::fabs(p[1]-y) < 1.f && std::fabs(p[2]-z) < 1.f; }
static bool nearf(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
  printf("sims3 hook standalone test (sims3_camera_hook.h + sims3_walls.h)\n");
  CHECK(enabled(), "hook enabled by default (SIMS3_CAMERA_HOOK unset)");

  // --- captured reflection-pass draws: verified cameras, looking up -> OtherCamera
  Camera A = {}, B = {};
  const Kind kA = classify(k1277907, 12, A);
  CHECK(kA == Kind::OtherCamera, "call 1277907 (WVP + World at c4, eye at c8): %s (reflection camera, fwd.y=%.2f)", kindName(kA), A.fwd[1]);
  const Kind kB = classify(k1278332, 12, B);
  CHECK(kB == Kind::OtherCamera, "call 1278332 (identity World, eye at c7): %s", kindName(kB));
  if (kA != Kind::None && kB != Kind::None) {
    printf("         fovY=%.2f deg  aspect=%.3f  near=%.4f  eye=(%.2f, %.2f, %.2f)\n", A.fovY*57.2958f, A.aspect, A.nearZ, A.pos[0], A.pos[1], A.pos[2]);
    CHECK(A.fovY*57.2958f > 25.f && A.fovY*57.2958f < 27.5f && A.aspect > 1.75f && A.aspect < 1.81f, "lens: fovY ~26 deg, aspect ~16:9");
    float maxd = 0.f;
    for (int i = 0; i < 16; ++i) { float d = std::fabs(((const float*)A.view.m)[i] - ((const float*)B.view.m)[i]); if (d > maxd) maxd = d; }
    CHECK(maxd < 0.5f, "both draws yield the same View (max diff %.4f; 4-decimal inputs)", maxd);
    const float p[4] = { 1113.957f, 45.889f, 1022.679f, 1.f };
    float game[4], v[4], mine[4];
    mulColumn(k1278332, p, game); mulRow(p, B.view, v); mulRow(v, B.proj, mine);
    CHECK(std::fabs(game[0]-mine[0]) < 0.05f && std::fabs(game[1]-mine[1]) < 0.05f && std::fabs(game[3]-mine[3]) < 0.05f, "clip x, y, w round-trip through forwarded View*Projection");
    CHECK(std::fabs(game[2]-mine[2]) < 0.15f, "clip z round-trips within the trace data's precision (%.4f vs %.4f)", game[2], mine[2]);
    CHECK(near3(B.pos, 1115.16f, -10.70f, 1020.54f), "camera position equals the eye constant the game uploads");
  }

  // --- the play camera: the captured VP mirrored back across the sea plane y = 28 (y' = 56 - y).
  // Its eye (1115.16, 66.70, 1020.54) matches the eye the trace shows for main-pass draws (66.85).
  const M4 VPr = fromRows(k1278332);
  const float Mrows[16] = { 1,0,0,0,  0,-1,0,56,  0,0,1,0,  0,0,0,1 };
  M4 M = fromRows(Mrows), VP; mul(VPr, M, VP);
  float blockM[12*4] = {}; toBlock(VP, blockM);
  const float I34[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 }; for (int i = 0; i < 12; ++i) blockM[16+i] = I34[i];
  blockM[28] = 1115.16f; blockM[29] = 66.70f; blockM[30] = 1020.54f; blockM[31] = 1.f;      // own eye at c7
  Camera Mn = {}; const Kind kM = classify(blockM, 12, Mn);
  CHECK(kM == Kind::Main, "play camera (eye y=66.7, fwd.y=%.2f): %s -> forwarded", Mn.fwd[1], kindName(kM));
  if (kM == Kind::Main) {
    CHECK(near3(Mn.pos, 1115.16f, 66.70f, 1020.54f), "  its position is the main-pass eye");
    // The forwarded View must be a PROPER rotation (det +1) in D3D's left-handed convention,
    // otherwise Remix sees a mirrored world with reversed winding (run 6: black tops).
    const D3DMATRIX& V = Mn.view;
    const float det = V._11*(V._22*V._33 - V._23*V._32) - V._12*(V._21*V._33 - V._23*V._31) + V._13*(V._21*V._32 - V._22*V._31);
    CHECK(det > 0.99f && det < 1.01f, "  forwarded View is a proper rotation (det=%.4f) with P34=%.0f (right-handed; a left-handed pair gives det -1)", det, Mn.proj._34);
    // and the LH pair still reproduces the game's clip output exactly (x, y, w; z within data precision)
    const float q[4] = { 1113.957f, 45.889f, 1022.679f, 1.f };
    float gm[4], vv[4], mm[4]; float blockRows[16]; for (int i = 0; i < 16; ++i) blockRows[i] = blockM[i];
    mulColumn(blockRows, q, gm); mulRow(q, Mn.view, vv); mulRow(vv, Mn.proj, mm);
    CHECK(std::fabs(gm[0]-mm[0]) < 0.05f && std::fabs(gm[1]-mm[1]) < 0.05f && std::fabs(gm[3]-mm[3]) < 0.05f && std::fabs(gm[2]-mm[2]) < 0.15f, "  LH pair round-trips the play camera's clip output (w=%.3f vs %.3f)", gm[3], mm[3]);
  }

  // --- regression (run 1): VP * rigid World is a valid-looking camera in object space.
  const float cs = std::cos(30.f*3.14159265f/180.f), sn = std::sin(30.f*3.14159265f/180.f);
  const float Wrows[16] = { cs,0,sn,1200,  0,1,0,5,  -sn,0,cs,900,  0,0,0,1 };
  M4 W = fromRows(Wrows), WVP; mul(VP, W, WVP);
  float blockA[12*4] = {}; toBlock(WVP, blockA);
  for (int i = 0; i < 12; ++i) blockA[16+i] = Wrows[i];                                     // World at c4..c6
  blockA[32] = 1115.16f; blockA[33] = 66.70f; blockA[34] = 1020.54f; blockA[35] = 1.f;       // eye at c8
  Camera R1 = {}; const Kind k1 = classify(blockA, 12, R1);
  CHECK(k1 == Kind::Main, "WVP with rotated World at c4 + eye: %s (World un-multiplied)", kindName(k1));
  if (k1 == Kind::Main) CHECK(near3(R1.pos, 1115.16f, 66.70f, 1020.54f), "  and its position is the true eye");
  float blockB[12*4] = {}; toBlock(WVP, blockB);                                             // same WVP, no World, eye present
  blockB[32] = 1115.16f; blockB[33] = 66.70f; blockB[34] = 1020.54f; blockB[35] = 1.f;
  Camera R2 = {}; const Kind k2 = classify(blockB, 12, R2);
  CHECK(k2 == Kind::None, "same WVP without a World at c4: %s (object-space camera must NOT be forwarded)", kindName(k2));

  Camera U = {}; const Kind kU = classify(kUi, 5, U);
  CHECK(kU == Kind::None, "UI pass (2/1920, -2/1080 scale): %s -> not a camera (its draws fail the per-draw 3D tests)", kindName(kU));
  {
    // run 70: a block with the play camera's lens but the view's translation stripped decomposes to a
    // camera at the world origin; its "eye" was matched by a small float4 (a direction). A camera near
    // the origin is never adopted, and the eye must be a position (w = 1), not a direction (w = 0).
    float blockO[12*4] = {}; toBlock(VP, blockO);
    for (int r = 0; r < 4; ++r) blockO[r*4+3] = 0.f;                                          // no translation
    blockO[28] = 0.57f; blockO[29] = 0.77f; blockO[30] = 0.26f; blockO[31] = 0.f;              // a light direction at c7
    Camera O = {}; const Kind kO = classify(blockO, 12, O);
    CHECK(kO == Kind::None, "a camera at the world origin with a direction where the eye would be: %s -> not adopted", kindName(kO));
    const float eye[3] = { 1115.16f, 66.70f, 1020.54f };
    float blockE[8*4] = {}; blockE[28] = 1115.4f; blockE[29] = 66.9f; blockE[30] = 1020.3f; blockE[31] = 0.f;
    CHECK(!eyePresent(blockE, 8, eye), "eye: a float4 with w = 0 at the eye's position is not the eye");
    blockE[31] = 1.f;
    CHECK(eyePresent(blockE, 8, eye), "eye: (x, y, z, 1) within half a unit is");
    blockE[28] = 1116.f;
    CHECK(!eyePresent(blockE, 8, eye), "eye: a unit away is not");
    D3DMATRIX ma = Mn.view, mb = Mn.view; mb._41 *= 1.f + 5e-6f; mb._22 += 2e-6f;
    CHECK(similarMatrix(ma, mb, 1e-5f) && !similarMatrix(ma, mb, 1e-7f), "similarMatrix: last-bit differences of the same camera are the same camera (_41 %.3f); a tighter tolerance tells them apart", ma._41);
  }
  Camera S = {}; const Kind kS = classify(kShadow, 12, S);
  CHECK(kS == Kind::None, "90-degree square shadow camera: %s", kindName(kS));

  // --- draw-time decision (milestone 1d): position layout and depth test
  const D3DVERTEXELEMENT9 mesh[]   = { {0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0}, {0,12,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,0}, D3DDECL_END() };
  const D3DVERTEXELEMENT9 packed[] = { {0,0,D3DDECLTYPE_SHORT4,0,D3DDECLUSAGE_POSITION,0}, {0,8,D3DDECLTYPE_SHORT4,0,D3DDECLUSAGE_TEXCOORD,0}, D3DDECL_END() };
  const D3DVERTEXELEMENT9 quad[]   = { {0,0,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_POSITION,0}, {0,8,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,0}, D3DDECL_END() };
  const D3DVERTEXELEMENT9 pretr[]  = { {0,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITIONT,0}, D3DDECL_END() };
  const D3DVERTEXELEMENT9 nopos[]  = { {0,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_TEXCOORD,0}, {0,16,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_TEXCOORD,1}, D3DDECL_END() };
  CHECK(positionIs3D(mesh),    "decl FLOAT3 POSITION + NORMAL: 3D");
  CHECK(positionIs3D(packed),  "decl SHORT4 POSITION (packed terrain/decals): 3D");
  const D3DVERTEXELEMENT9 bytes[]  = { {0,0,D3DDECLTYPE_UBYTE4,0,D3DDECLUSAGE_POSITION,0}, {0,4,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,0}, D3DDECL_END() };
  CHECK(positionIs3D(bytes),   "decl UBYTE4 POSITION (compressed grid positions): 3D");
  CHECK(!positionIs3D(quad),   "decl FLOAT2 POSITION (UI / full-screen quad): not 3D");
  const D3DVERTEXELEMENT9 f4pos[]  = { {0,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,0}, D3DDECL_END() };
  CHECK(positionIs3D(f4pos),   "decl FLOAT4 POSITION: 3D (its shader rebuilds w = 1)");
  CHECK(!positionIs3D(pretr),  "decl POSITIONT (pre-transformed): not 3D");
  CHECK(!positionIs3D(nopos),  "decl with no POSITION (TEXCOORD-only): not 3D");
  CHECK(fvfIs3D(D3DFVF_XYZ | D3DFVF_DIFFUSE) && !fvfIs3D(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1), "FVF XYZ: 3D, XYZRHW: not 3D");
  CHECK(drawIs3D(true, true, D3DZB_TRUE, true),   "3D decl + depth on + camera + primary RT -> main camera");
  CHECK(!drawIs3D(true, true, D3DZB_FALSE, true), "3D decl + depth OFF                      -> identity (post/UI window)");
  CHECK(!drawIs3D(true, false, D3DZB_TRUE, true), "2D decl                                  -> identity");
  CHECK(!drawIs3D(false, true, D3DZB_TRUE, true), "no verified camera yet                   -> identity");
  CHECK(!drawIs3D(true, true, D3DZB_TRUE, false), "non-primary render target (shadow/reflection pass) -> identity");

  // --- draw-time texture remap (milestone 1g)
  CHECK(isColorFormat(D3DFMT_A8R8G8B8) && isColorFormat(D3DFMT_DXT5) && isColorFormat(D3DFMT_DXT1), "colour formats: A8R8G8B8, DXT1, DXT5");
  CHECK(!isColorFormat(D3DFMT_L8) && !isColorFormat(D3DFMT_V8U8) && !isColorFormat(D3DFMT_D24S8), "non-colour formats: L8, V8U8, D24S8");
  { bool b[16] = {}, c[16] = {};
    b[0] = true; c[0] = true; b[1] = true; c[1] = true;
    CHECK(pickAlbedoStage(b, c) == -1, "stage 0 already a colour texture -> no remap");
    c[0] = false;                                  // cube map at stage 0, diffuse at stage 1 (walls, floor, objects)
    CHECK(pickAlbedoStage(b, c) == 1, "cube map at stage 0, colour at stage 1 -> present stage 1");
    c[1] = false; b[2] = true; c[2] = true;        // stage 1 is an L8 mask, stage 2 colour
    CHECK(pickAlbedoStage(b, c) == 2, "cube at 0, L8 at 1, colour at 2 -> present stage 2");
    b[2] = false;                                  // nothing usable
    CHECK(pickAlbedoStage(b, c) == -1, "cube at 0 and no colour texture -> no remap (draw stays dropped)");
    b[0] = false;
    CHECK(pickAlbedoStage(b, c) == -1, "nothing bound at stage 0 -> no remap");
  }

  // --- per-shader constant patches (milestone 1f)
  CHECK(fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull, "FNV-1a-64 test vector");
  const DWORD toks[] = { 0xFFFE0200u, 0x0000FFFFu };
  CHECK(shaderTokenCount(toks) == 2 && shaderTokenCount(toks, 1) == 0, "shaderTokenCount counts version..END inclusive; a stream without END within the bound counts 0");
  const DWORD noEnd[] = { 0xFFFE0200u, 0x02000001u, 0x800F0000u, 0x90E40000u };
  CHECK(shaderTokenCount(noEnd, 4) == 0 && !dxsoIsVertexShader(noEnd, shaderTokenCount(noEnd, 4), 2), "a truncated stream (no END) is refused rather than read past its end");
  CHECK(dxsoIsDef(0x51u) && dxsoIsDef(0x2Fu) && dxsoIsDef(0x30u) && !dxsoIsDef(0x52u) && !dxsoIsDef(0x01u), "DEF / DEFB / DEFI are the literal definitions (0x51, 0x2F, 0x30); TEXREG2RGB (0x52) is not");
  CHECK(findShaderPatch(0xf64835ccff6bffd7ull) == nullptr && findShaderPatch(0x1234ull) == nullptr && findShaderPatch(0) == nullptr, "walls: no constant patch (the cut-away is handled in the geometry); unknown and zero hashes: no rule");
  const ShaderPatch* wa = findShaderPatch(0x976b73dbd59842cdull);
  CHECK(wa && wa->count == 4 && wa->patches[0].reg == 7, "lot terrain rule found by hash (c7)");
  float blk[12*4]; for (int i = 0; i < 48; ++i) blk[i] = (float) i;
  CHECK(patchIntersects(wa, 0, 12) && !patchIntersects(wa, 0, 7) && patchIntersects(wa, 7, 1), "range test: c0..c11 yes, c0..c6 no, c7..c7 yes");
  CHECK(applyPatches(wa, 0, blk, 12) == 4 && blk[28] == 0.f && blk[29] == 0.f && blk[30] == 0.5f && blk[31] == 0.5f && blk[27] == 27.f && blk[32] == 32.f, "lot terrain: c7 -> (0,0,0.5,0.5) in a c0..c11 upload, neighbours untouched");

  // --- texcoord promotion on the game's real shader bytecode (milestone 2)
  {
    auto countType6 = [](const std::vector<DWORD>& t, uint32_t num) {   // oT#/o# parameter tokens with that number
      int c = 0;
      dxsoForEach(t.data(), t.size(), [&](size_t pos, uint32_t op, uint32_t len) {
        if (!dxsoIsDef(op)) for (size_t i = 1; i <= len; ++i) if ((t[pos + i] & 0x80000000u) && dxsoRegType(t[pos + i]) == kDxsoRegOutput && dxsoRegNum(t[pos + i]) == num) ++c;
        return true;
      });
      return c;
    };
    auto dclIndexOf = [](const std::vector<DWORD>& t, uint32_t reg) -> int {   // usage index declared for output register `reg`, -1 if none
      int idx = -1;
      dxsoForEach(t.data(), t.size(), [&](size_t pos, uint32_t op, uint32_t len) {
        uint32_t u, i, r;
        if (op == kDxsoOpDcl && dxsoDcl(t.data(), pos, len, kDxsoRegOutput, u, i, r) && u == kUsageTexcoord && r == reg) { idx = (int) i; return false; }
        return true;
      });
      return idx;
    };
    std::vector<DWORD> wallsA, wallsB;
    if (!loadShader("vs_f64835ccff6bffd7", wallsA)) SKIP("walls A dump (vs_f64835ccff6bffd7) not found: promotion on vs_2_0 not tested");
    else {
      const size_t n = wallsA.size();
      CHECK(n == 326, "walls A token count through END = %zu", n);
      const int before0 = countType6(wallsA, 0), before1 = countType6(wallsA, 1);
      std::vector<DWORD> patched = wallsA;
      const uint32_t changed = promoteTexcoord(patched.data(), n, 1);
      CHECK(changed == (uint32_t) (before0 + before1) && changed > 0, "walls A (vs_2_0): oT0<->oT1 renumbered (%u tokens; had %d oT0 + %d oT1 writes)", changed, before0, before1);
      CHECK(countType6(patched, 0) == before1 && countType6(patched, 1) == before0, "  counts swapped, nothing else touched");
      CHECK(shaderTokenCount(patched.data()) == n && patched[0] == wallsA[0], "  stream length and version intact");
      DWORD vs1[4] = { 0xFFFE0101u, 0x0000FFFFu, 0, 0 };
      CHECK(promoteTexcoord(vs1, 2, 1) == 0, "  a vs_1_1 stream (no length fields) is left alone");
    }
    if (!loadShader("vs_84b06922c8593a48", wallsB)) SKIP("walls B dump (vs_84b06922c8593a48) not found: promotion on vs_3_0 not tested");
    else {
      const size_t n = wallsB.size();
      std::vector<DWORD> patched = wallsB;
      const int o1before = dclIndexOf(wallsB, 1), o2before = dclIndexOf(wallsB, 2);
      const uint32_t changed = promoteTexcoord(patched.data(), n, 1);
      CHECK(o1before == 0 && o2before == 1, "walls B (vs_3_0): o1 is TEXCOORD0 and o2 is TEXCOORD1 before the patch (%d, %d)", o1before, o2before);
      CHECK(changed == 2 && dclIndexOf(patched, 1) == 1 && dclIndexOf(patched, 2) == 0, "  after promote 1: o2 declared TEXCOORD0, o1 declared TEXCOORD1 (%u tokens changed)", changed);
      CHECK(promoteTexcoord(patched.data(), n, 0) == 0, "  K = 0 is a no-op");
    }
  }

  // --- the low-detail lot's window glow (milestone 70): decoding the glow atlas, picking the glowing triangles
  {
    // one DXT1 block 4x4: c0 = white, c1 = black, the first row index 0 (white), the rest index 1 (black)
    const uint8_t blk[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x55, 0x55, 0x55 };
    std::vector<uint8_t> lv;
    CHECK(decodeMaxChannel((uint32_t) D3DFMT_DXT1, blk, sizeof blk, 4, 4, lv) && lv.size() == 16 && lv[0] == 255 && lv[3] == 255 && lv[4] == 0 && lv[15] == 0,
          "glow: a DXT1 block decodes to its brightest channel (row 0 white, the rest black)");
    // a DXT5 block (alpha part ignored): colour c0 = pure red (0xF800), c1 = black, all index 2 -> 2/3 red = 170
    const uint8_t blk5[16] = { 0, 0, 0, 0, 0, 0, 0, 0,  0x00, 0xF8, 0x00, 0x00, 0xAA, 0xAA, 0xAA, 0xAA };
    CHECK(decodeMaxChannel((uint32_t) D3DFMT_DXT5, blk5, sizeof blk5, 4, 4, lv) && lv[0] == 170 && lv[15] == 170, "glow: DXT5 takes the colour block, 4-colour mode (2/3 red = %u)", (unsigned) lv[0]);
    // 4x4 texture: only texel (1, 1) bright (centre 1.5, 1.5 in texels); a triangle covering that centre is kept, one far from it is not, a tiny one inside the texel is kept by its centre
    std::vector<uint8_t> gl(16, 0); gl[1 * 4 + 1] = 200;
    const std::vector<float> uv = { 0.f, 0.f,  1.f, 0.f,  0.f, 1.f,   0.75f, 0.75f,  1.f, 0.75f,  0.75f, 1.f,   0.30f, 0.30f,  0.32f, 0.30f,  0.30f, 0.32f };
    const std::vector<uint32_t> tris = { 0, 1, 2,   3, 4, 5,   6, 7, 8 };
    std::vector<uint32_t> out;
    selectGlowTriangles(uv, tris, gl, 4, 4, kLotGlowThreshold, out);
    CHECK(out.size() == 6 && out[0] == 0 && out[3] == 6, "glow: the triangle over the bright texel and the tiny one inside it glow; the far one does not (%zu indices)", out.size());
  }

  // --- the window glow's layer (milestone 71): only the windows, lifted off the wall
  {
    const uint8_t blk[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x55, 0x55, 0x55 };   // DXT1: row 0 white, the rest black
    std::vector<uint32_t> c;
    CHECK(decodeColour((uint32_t) D3DFMT_DXT1, blk, sizeof blk, 4, 4, c) && c.size() == 16 && c[0] == 0xFFFFFFFFu && c[5] == 0xFF000000u, "glow: DXT1 decodes to A8R8G8B8 (white %08X, black %08X)", c[0], c[5]);
    std::vector<uint32_t> g = { 0xFFAEA57Du, 0xFF141414u, 0xFF292929u, 0xFF000000u };
    windowOnlyGlow(g, kLotGlowThreshold);
    CHECK(g[0] == 0xFFAEA57Du && g[1] == 0xFF000000u && g[2] == 0xFF292929u && g[3] == 0xFF000000u, "glow: texels at or below 40/255 go black, brighter ones stay (a window %08X kept, 20 dropped, 41 kept)", g[0]);
    float p[9] = { 1.f, 0.f, 0.f,   1.f, 0.f, 1.f,   1.f, 1.f, 0.f };   // a wall at x = 1
    const float out[3] = { 1.f, 0.f, 0.f }, in[3] = { -1.f, 0.f, 0.f };
    float q[9]; memcpy(q, p, sizeof q);
    liftTriangle(p, out, 0.05f); liftTriangle(q, in, 0.05f);
    CHECK(std::fabs(p[0] - 1.05f) < 1e-5f && std::fabs(p[6] - 1.05f) < 1e-5f && std::fabs(q[3] - 0.95f) < 1e-5f && p[1] == 0.f && p[5] == 1.f,
          "glow: a wall triangle moves 5 cm to the side its normal faces, whatever its winding (x %.3f out, %.3f in)", p[0], q[0]);
  }

  // --- the low-detail lot's plate (milestone 69): its flat class-1 triangles, and the plate shader
  {
    // a slab: top (4 triangles around the centre, y = 1, class 1), one side triangle (class 1, vertical), a house triangle (class 0)
    const std::vector<float> pos = { 0, 1, 0,   10, 1, 0,   10, 1, 10,   0, 1, 10,   5, 1, 5,   0, -0.5f, 0,   3, 1, 3,   3, 4, 3,   4, 1, 3 };
    const std::vector<uint8_t> cls = { 255, 255, 255, 255, 255, 255, 0, 0, 0 };
    const std::vector<uint32_t> idx = { 4, 0, 1,  4, 1, 2,  4, 2, 3,  4, 3, 0,   0, 1, 5,   6, 7, 8,   0, 1, 99 };
    std::vector<uint32_t> house, plate; PlateSplitStats st;
    splitLotPlate(pos, cls, idx, house, plate, st);
    CHECK(st.in == 7 && st.plate == 4 && st.house == 2 && st.outside == 1 && plate.size() == 12 && house.size() == 6,
          "plate: the slab's 4 flat class-1 triangles are the plate; its side and the house triangle stay with the house; an index past the vertices is left out (%u/%u/%u/%u)", st.plate, st.house, st.outside, st.in);
    const size_t n = shaderTokenCount(kLotPlatePs, sizeof kLotPlatePs / sizeof kLotPlatePs[0]);
    PsAnalysis a;
    CHECK(n == sizeof kLotPlatePs / sizeof kLotPlatePs[0] && analyzePixelShader(kLotPlatePs, n, a) && a.samplers[2].read && a.samplers[2].texcoord == 0 && a.samplers[2].colorChannels == 3 && a.cutSampler == -1,
          "plate shader: ps_3_0, %zu tokens, reads s2 at TEXCOORD0 into all three colour channels, no cut-out", n);
  }

  // --- the cut-out (milestone 68): a texkill on one sampler's alpha read as a * alpha + b
  {
    uint32_t ref = 0;
    CHECK(cutAlphaTest(1.f, -0.5f, ref) == D3DCMP_GREATEREQUAL && ref == 128, "cut: alpha - 0.5 keeps alpha >= 128/255 (ref %u)", ref);
    CHECK(cutAlphaTest(255.f, -128.f, ref) == D3DCMP_GREATEREQUAL && ref == 128, "cut: alpha * 255 - 128 keeps alpha >= 128 (ref %u)", ref);
    CHECK(cutAlphaTest(255.f, 0.f, ref) == 0u, "cut: alpha * 255 - 0 discards nothing: no test");
    CHECK(cutAlphaTest(1.f, 0.5f, ref) == 0u, "cut: alpha + 0.5 discards nothing");
    CHECK(cutAlphaTest(1.f, -1.5f, ref) == D3DCMP_NEVER, "cut: alpha - 1.5 discards everything");
    CHECK(cutAlphaTest(-1.f, 0.5f, ref) == D3DCMP_LESSEQUAL && ref == 127, "cut: 0.5 - alpha keeps alpha <= 127/255 (ref %u)", ref);
    CHECK(cutAlphaTest(0.f, -1.f, ref) == D3DCMP_NEVER && cutAlphaTest(0.f, 1.f, ref) == 0u, "cut: a constant value discards all or nothing");
    std::vector<DWORD> imp, obj, obj2, walls, two, vin;
    if (loadShader("ps_9c84a6b7017f33fc", imp)) {
      PsAnalysis a;
      CHECK(analyzePixelShader(imp.data(), imp.size(), a) && a.cutSampler == 2 && a.cutA.n == 1 && a.cutA.t[0].k == 1.f && a.cutA.t[0].c < 0 && a.cutB.n == 1 && a.cutB.t[0].k == -0.5f && a.cutB.t[0].c < 0,
            "cut: the low-detail houses' PS 9c84a6b7 -> 1 * alpha(s2) - 0.5 (sampler %d)", a.cutSampler);
    } else SKIP("ps_9c84a6b7017f33fc dump not found");
    if (loadShader("ps_00230c49e1b880b6", obj)) {
      PsAnalysis a;
      const bool ok = analyzePixelShader(obj.data(), obj.size(), a);
      CHECK(ok && a.cutSampler == 6 && a.cutA.n == 1 && a.cutA.t[0].k == 255.f && a.cutA.t[0].c < 0 && a.cutB.n == 1 && a.cutB.t[0].k == -1.f && a.cutB.t[0].c == 7 * 4,
            "cut: object PS 00230c49 -> 255 * alpha(s6) - c7.x, the game's alpha reference (sampler %d, b term c%d)", a.cutSampler, a.cutB.n ? a.cutB.t[0].c : -1);
      auto get = [](uint32_t reg, uint32_t comp) -> float { return (reg == 7 && comp == 0) ? 96.f : 0.f; };
      uint32_t r2 = 0;
      CHECK(cutAlphaTest(cutEval(a.cutA, get), cutEval(a.cutB, get), r2) == D3DCMP_GREATEREQUAL && r2 == 96, "  with c7.x = 96: alpha test >= 96 (ref %u)", r2);
    } else SKIP("ps_00230c49e1b880b6 dump not found");
    if (loadShader("ps_10c22e3b87e087a0", obj2)) {
      PsAnalysis a;
      CHECK(analyzePixelShader(obj2.data(), obj2.size(), a) && a.cutSampler == 9 && a.cutB.n == 1 && a.cutB.t[0].k == -0.5f, "cut: PS 10c22e3b -> alpha(s9) - 0.5 (sampler %d)", a.cutSampler);
    } else SKIP("ps_10c22e3b87e087a0 dump not found");
    if (loadShader("ps_936215cf2c1c56a7", walls) || loadShader("ps_0463ee5c7120f777", walls)) {
      PsAnalysis a;
      CHECK(analyzePixelShader(walls.data(), walls.size(), a) && a.cutSampler == -1, "cut: a shader whose texkill is no single alpha threshold reads none (sampler %d)", a.cutSampler);
    } else SKIP("no mask / two-texkill dump found");
    if (loadShader("ps_03d264329cbfcf64", vin)) {
      PsAnalysis a;
      CHECK(analyzePixelShader(vin.data(), vin.size(), a) && a.cutSampler == -1, "cut: PS 03d26432 discards by a vertex value: no cut-out");
    } else SKIP("ps_03d264329cbfcf64 dump not found");
  }

  // --- the world normal from the vertex shader (milestone 11), on the in-game shader dumps
  {
    std::vector<DWORD> obj, sim, wallsB3, roof, floors, psObj;
    const bool haveAll = loadShader("vs_0ba6ddb9aa01913c", obj) && loadShader("vs_f2eeef682061fb92", sim) && loadShader("vs_84b06922c8593a48", wallsB3)
                      && loadShader("vs_b077115faa5f77b4", roof) && loadShader("vs_0fcdd50823cd0504", floors) && loadShader("ps_0c19795eb80e2e96", psObj);
    if (!haveAll) SKIP("in-game shader dumps for the normal analysis not all found (vs_0ba6ddb9 / vs_f2eeef68 / vs_84b06922 / vs_b077115f / vs_0fcdd508 / ps_0c19795e)");
    if (haveAll) {
      VsNormalInfo a;
      CHECK(analyzeVertexNormal(obj.data(), obj.size(), a) && a.version == 3 && a.hasNormalInput && a.candidates == (1u << 1) && a.outReg[1] == 4,
            "object VS 0ba6ddb9: the world normal leaves in TEXCOORD1 = o4 (candidates %x, o%u)", a.candidates, a.outReg[1]);
      VsNormalInfo s;
      CHECK(analyzeVertexNormal(sim.data(), sim.size(), s) && s.hasNormalInput && (s.candidates & 1u) && s.outReg[0] == 1,
            "Sim VS f2eeef68 (skinned, morphed): TEXCOORD0 = o1 (candidates %x)", s.candidates);
      VsNormalInfo w;
      CHECK(analyzeVertexNormal(wallsB3.data(), wallsB3.size(), w) && w.candidates == ((1u << 2) | (1u << 6)),
            "walls B VS 84b06922: two normal-derived outputs, TEXCOORD2 and TEXCOORD6 (candidates %x)", w.candidates);
      CHECK(chooseNormalTexcoord(w, (uint16_t) (1u << 6)) == 6 && chooseNormalTexcoord(w, 0) == 2, "  the pixel shader's own use breaks the tie, else the lowest");
      VsNormalInfo r;
      CHECK(analyzeVertexNormal(roof.data(), roof.size(), r) && r.hasNormalInput && r.candidates == 0, "roof VS b077115f: normal mixed with the per-vertex pitch (POSITION2): no candidate");
      VsNormalInfo f;
      CHECK(analyzeVertexNormal(floors.data(), floors.size(), f) && !f.hasNormalInput && f.candidates == 0, "floors VS 0fcdd508: no normal input");
      PsAnalysis pa;
      CHECK(analyzePixelShader(psObj.data(), psObj.size(), pa) && (pa.normalTexcoords & (1u << 1)), "object PS 0c19795e treats TEXCOORD1 as a normal (mask %x)", pa.normalTexcoords);
      // the variant: one more output, NORMAL, written wherever o4 is
      std::vector<DWORD> v = obj;
      const size_t before = v.size();
      const uint32_t highest = maxOutputReg(obj);
      const uint32_t made = makeNormalVariant(v, 4);
      CHECK(made == highest + 1 && made <= 11, "object variant: NORMAL output o%u (highest used was o%u)", made, highest);
      int dclNormal = 0, writesNew = 0, writesOld = 0;
      dxsoForEach(v.data(), v.size(), [&](size_t pos, uint32_t op, uint32_t len) {
        if (op == kDxsoOpDcl && len >= 2) { uint32_t u, i, rg; if (dxsoDcl(v.data(), pos, len, kDxsoRegOutput, u, i, rg) && u == kUsageNormal && rg == made) ++dclNormal; return true; }
        if (dxsoIsDef(op) || len < 1) return true;
        const uint32_t d = v[pos + 1];
        if ((d & 0x80000000u) && dxsoRegType(d) == kDxsoRegOutput) { if (dxsoRegNum(d) == made) ++writesNew; else if (dxsoRegNum(d) == 4) ++writesOld; }
        return true;
      });
      CHECK(dclNormal == 1 && writesNew == writesOld && writesNew > 0 && v.size() > before + 3, "  declared once, every write of o4 repeated into o%u (%d writes), stream %zu -> %zu tokens", made, writesNew, before, v.size());
      CHECK(v.back() == kDxsoEnd && v[0] == obj[0], "  END kept, version kept");
      VsNormalInfo a2;
      CHECK(analyzeVertexNormal(v.data(), v.size(), a2) && a2.candidates == a.candidates && maxOutputReg(v) == made, "  the variant analyses the same, with the new register in use");
      // hiding: the input normal renamed to a free TEXCOORD index
      std::vector<DWORD> hcopy = roof;
      CHECK(hideNormalInput(hcopy, r), "roof variant: normal input hidden");
      VsNormalInfo r2;
      CHECK(analyzeVertexNormal(hcopy.data(), hcopy.size(), r2) && !r2.hasNormalInput, "  the hidden shader shows the runtime no normal input");
      CHECK(!hideNormalInput(hcopy, f), "  nothing to hide on a shader without a normal input");
      // the dead constant read for the per-instance tag (milestone 12)
      std::vector<DWORD> cr = obj;
      CHECK(appendConstantRead(cr, 255) && cr.size() == before + 3 && cr.back() == kDxsoEnd, "object VS: a read of c255 appended before END (%zu -> %zu tokens)", before, cr.size());
      const size_t n = cr.size();
      CHECK(cr[n - 4] == 0x02000001u && cr[n - 3] == 0x800F0000u && cr[n - 2] == 0xA0E400FFu, "  it is `mov r0, c255` (%08lX %08lX %08lX)", (unsigned long) cr[n - 4], (unsigned long) cr[n - 3], (unsigned long) cr[n - 2]);
      VsNormalInfo a5;
      CHECK(analyzeVertexNormal(cr.data(), cr.size(), a5) && a5.candidates == a.candidates && a5.outReg[1] == a.outReg[1], "  the normal analysis is unchanged by it");
    }

    // --- vs_2_0 rewritten as vs_3_0 (milestone 11b): walls A, then the NORMAL output on the result
    std::vector<DWORD> wallsA2;
    if (!loadShader("vs_f64835ccff6bffd7", wallsA2)) SKIP("walls A dump not found: the vs_2_0 -> vs_3_0 rewrite not tested");
    else {
      VsNormalInfo a2;
      CHECK(analyzeVertexNormal(wallsA2.data(), wallsA2.size(), a2) && a2.version == 2 && a2.hasNormalInput && a2.candidates == ((1u << 2) | (1u << 3) | (1u << 4)) && a2.outReg[2] == 2,
            "walls A (vs_2_0): normal-derived outputs oT2, oT3, oT4 (candidates %x)", a2.candidates);
      std::vector<DWORD> c = wallsA2;
      CHECK(convertVs2To3(c), "walls A rewritten as vs_3_0");
      CHECK(c[0] == 0xFFFE0300u && c.back() == 0x0000FFFFu && dxsoIsVertexShader(c.data(), c.size(), 3), "  version token vs_3_0, END kept");
      int rastAttr = 0, outDcls = 0, posDcl = 0, texDcls = 0, colDcls = 0; uint32_t maxOut = 0;
      dxsoForEach(c.data(), c.size(), [&](size_t pos, uint32_t op, uint32_t len) {
        if (op == kDxsoOpDcl && len >= 2) {
          uint32_t u, i, rg;
          if (dxsoDcl(c.data(), pos, len, kDxsoRegOutput, u, i, rg)) { ++outDcls; if (u == kUsagePosition) ++posDcl; else if (u == kUsageTexcoord) { ++texDcls; if (rg != i) ++rastAttr; } else if (u == kUsageColor) ++colDcls; }
          return true;
        }
        if (dxsoIsDef(op)) return true;
        for (size_t k = 1; k <= len; ++k) { const uint32_t p = c[pos + k]; if (!(p & 0x80000000u)) continue; const uint32_t ty = dxsoRegType(p); if (ty == 4u || ty == 5u) ++rastAttr; if (ty == kDxsoRegOutput) maxOut = (std::max)(maxOut, dxsoRegNum(p)); }
        return true;
      });
      CHECK(rastAttr == 0 && posDcl == 1 && texDcls == 7 && colDcls == 2 && outDcls == 10, "  no oPos/oD tokens left; declared: position, 7 texcoords (oT# -> o#), 2 colours (%d dcls, %d stray)", outDcls, rastAttr);
      CHECK(maxOut == 9, "  outputs end at o%u (oT0-6, then position, colour 0, colour 1)", maxOut);
      VsNormalInfo a3;
      CHECK(analyzeVertexNormal(c.data(), c.size(), a3) && a3.version == 3 && a3.candidates == a2.candidates && a3.outReg[2] == 2 && a3.outReg[3] == 3 && a3.outReg[4] == 4, "  the rewritten shader analyses the same (candidates %x)", a3.candidates);
      const uint32_t made = makeNormalVariant(c, a3.outReg[2]);
      CHECK(made == 10, "  NORMAL output o%u added on the rewritten shader", made);
      if (haveAll) { std::vector<DWORD> c3 = obj; CHECK(!convertVs2To3(c3) && c3 == obj, "a vs_3_0 shader is left alone"); }
    }
  }

  // --- the sun from the per-object light rig (milestone 2b), values from trace call 1279780
  {
    const AlbedoStage* ao = findAlbedoStage(0x0c19795eb80e2e96ull); const AlbedoStage* af = findAlbedoStage(0x17eabad58f650687ull);
    CHECK(ao && ao->stage == 3 && af && af->stage == 2 && findAlbedoStage(0x1234ull) == nullptr, "albedo stage: object PS -> 3, floor PS -> 2, unknown -> none");
    CHECK(findTexcoordPromote(0x0ba6ddb9aa01913cull) && findTexcoordPromote(0x0ba6ddb9aa01913cull)->texcoordIndex == 2 && findTexcoordPromote(0x55c99586fb17cd1cull) == nullptr, "promotions: objects promote 2; the terrain paint no longer promotes");
    CHECK(findNeverCapture(0xc79615c0181b5ef1ull) && findNeverCapture(0x5a2deada1e077b44ull), "never-capture: the drop-shadow decals and the grass sprites are left to the rasterizer");
    CHECK(findNeverCapture(0xab38a73070378739ull) != nullptr, "never-capture: the Sim black overlay pass (pixel shader 11227d6d outputs black) is left to the rasterizer");
    CHECK(findNeverCapture(0x41a25ab37bb2622cull) == nullptr, "never-capture: the sky dome is not in the table (it is presented as the runtime's sky instead)");
    {
      // the sky dome recognised from its bytecode: a position input, and the position's z pinned to w
      std::vector<DWORD> dome, obj2, wallsA3, stub;
      if (!loadShader("vs_41a25ab37bb2622c", dome)) SKIP("sky dome dump (vs_41a25ab37bb2622c) not found");
      else CHECK(isSkyDomeShader(dome.data(), dome.size()), "sky dome VS 41a25ab3 (mov oPos, r1.xyzz) recognised");
      if (loadShader("vs_0ba6ddb9aa01913c", obj2) && loadShader("vs_f64835ccff6bffd7", wallsA3))
        CHECK(!isSkyDomeShader(obj2.data(), obj2.size()) && !isSkyDomeShader(wallsA3.data(), wallsA3.size()), "  the object and wall shaders are not sky domes");
      if (loadShader("vs_e6648469a2dfcc8f", stub)) CHECK(!isSkyDomeShader(stub.data(), stub.size()), "  the game's stub shader (mov oPos, c0.x; no position input) is not a sky dome");
      // a vs_3_0 dome by DP4s: oPos.z and oPos.w from the same source and row
      const DWORD vs3dome[] = { 0xFFFE0300u, 0x0200001Fu, 0x80000000u, 0x900F0000u,                    // dcl_position v0
                                0x0200001Fu, 0x80000000u, 0xE00F0000u,                                 // dcl_position o0
                                0x03000009u, 0xE0010000u, 0x80E40000u, 0xA0E40000u,                    // dp4 o0.x, r0, c0
                                0x03000009u, 0xE0020000u, 0x80E40000u, 0xA0E40001u,                    // dp4 o0.y, r0, c1
                                0x03000009u, 0xE0040000u, 0x80E40000u, 0xA0E40003u,                    // dp4 o0.z, r0, c3
                                0x03000009u, 0xE0080000u, 0x80E40000u, 0xA0E40003u, 0x0000FFFFu };     // dp4 o0.w, r0, c3
      CHECK(isSkyDomeShader(vs3dome, sizeof vs3dome / 4), "a vs_3_0 dome writing o0.z and o0.w from the same row is recognised");
      DWORD vs3plain[sizeof vs3dome / 4]; memcpy(vs3plain, vs3dome, sizeof vs3dome); vs3plain[18] = 0xA0E40002u;   // z from c2
      CHECK(!isSkyDomeShader(vs3plain, sizeof vs3plain / 4), "  with z from its own row it is an ordinary shader");
    }
    CHECK(findAlbedoStage(0x3ebb622c4fe0be4full) && findAlbedoStage(0x3ebb622c4fe0be4full)->stage == 2 && findAlbedoStage(0xda37b5ef6f7a09a6ull) && findAlbedoStage(0xda37b5ef6f7a09a6ull)->stage == 1 && findAlbedoStage(0xf0d7af09599ed1bcull) == nullptr, "albedo stage: floors -> s2, floor tiles -> s1 (their small shared textures are lightmaps); the wall variant is stripped");
    CHECK(findTexcoordPromote(0x0fcdd50823cd0504ull) == nullptr && findTexcoordPromote(0x22e0b0fb83e51c5cull) == nullptr && findTexcoordPromote(0x1bd4405f8346ded4ull) && findTexcoordPromote(0x1bd4405f8346ded4ull)->texcoordIndex == 2, "promotions: floors no longer promote; the skinned object variant promotes 2");
    CHECK(useCapturedUv(0x0ba6ddb9aa01913cull) && !useCapturedUv(0x976b73dbd59842cdull) && !useCapturedUv(0xf64835ccff6bffd7ull) && !useCapturedUv(0x1234ull), "captured UVs: promoted families sample with the shader's output; terrain (no input set), walls (stripped) and unknown shaders keep the runtime's default");
    {
      // milestone 7: the pixel shader's samplers read from its bytecode, checked against the hand tables on the real dumps
      std::vector<DWORD> t; PsAnalysis a;
      if (loadShader("ps_7e9cd7f2d6bf8e67", t)) {
        CHECK(analyzePixelShader(t.data(), t.size(), a) && a.samplers[3].texcoord == 1 && a.samplers[3].colorChannels == 3 && a.samplers[6].texcoord == 2 && a.samplers[6].colorChannels == 3 && a.samplers[4].dependent && a.samplers[5].projective && a.samplers[2].colorChannels < 3,
              "auto: Sim body 7e9cd7f2 -> composite s3 on TEXCOORD1 (3 channels), skin s6 on TEXCOORD2, ramp s4 dependent, shadow s5 projective, mask s2 partial (%u channels)", a.samplers[2].colorChannels);
        // the composite-versus-mask tie: both DXT5 1024x512 at TEXCOORD1 -> the one whose three channels reach the colour
        bool color2D[16] = {}; uint32_t fmt[16] = {}; uint16_t w[16] = {}, h[16] = {};
        for (int s : { 2, 3, 6, 7 }) { color2D[s] = true; fmt[s] = s < 6 ? (uint32_t) D3DFMT_DXT5 : (uint32_t) D3DFMT_DXT1; w[s] = 1024; h[s] = s < 6 ? 512 : 1024; }
        color2D[4] = true; fmt[4] = (uint32_t) D3DFMT_A8R8G8B8; w[4] = h[4] = 64;
        int stage = -1, tc = -1;
        CHECK(chooseAutoAlbedo(a, color2D, fmt, w, h, stage, tc) && stage == 3 && tc == 1, "auto: with the Sim's textures bound, the albedo is s3 at TEXCOORD1 (chose s%d at TEXCOORD%d)", stage, tc);
      } else SKIP("auto: ps_7e9cd7f2d6bf8e67 not found");
      if (loadShader("ps_ff597f52868c21c6", t)) {
        CHECK(analyzePixelShader(t.data(), t.size(), a) && a.samplers[3].texcoord == 1 && a.samplers[3].colorChannels == 3 && a.samplers[4].texcoord == 2 && a.samplers[6].texcoord == 2, "auto: normal-mapped Sim ff597f52 -> composite s3 on TEXCOORD1, normal maps s4/s6 on TEXCOORD2");
      } else SKIP("auto: ps_ff597f52868c21c6 not found");
      if (loadShader("ps_11227d6d7bba5802", t)) {
        CHECK(analyzePixelShader(t.data(), t.size(), a) && a.samplers[0].texcoord == 0 && a.samplers[0].colorChannels == 0, "auto: the vs_2_0 Sim shader 11227d6d writes a constant black colour and takes only alpha from s0 (TEXCOORD0): no albedo candidate");
      } else SKIP("auto: ps_11227d6d7bba5802 not found");
      if (loadShader("ps_936215cf1e55a47e", t)) {
        CHECK(analyzePixelShader(t.data(), t.size(), a) && a.samplers[2].texcoord == 0 && a.samplers[2].colorChannels == 3 && a.samplers[3].texcoord == 6 && !a.samplers[3].reachesColor(), "auto: walls A -> wallpaper s2 on TEXCOORD0; the mask s3 (TEXCOORD6) feeds only the discard, not the colour");
      } else SKIP("auto: ps_936215cf1e55a47e not found");
      if (loadShader("ps_0c19795eb80e2e96", t)) {
        CHECK(analyzePixelShader(t.data(), t.size(), a) && a.samplers[3].texcoord == 2 && a.samplers[3].colorChannels == 3 && (a.normalTexcoords & (1u << 1)), "auto: object PS 0c19795e -> diffuse s3 on TEXCOORD2, as the hand table says; TEXCOORD1 used as a normal");
      } else SKIP("auto: ps_0c19795eb80e2e96 not found");
    }
    {
      // the light table (format 3): the lights per model, and the objects naming the models
      LiteTable table;
      CHECK(!liteParseLine(table, "model 0000000000f29289 1 5 0 1.51 0 1 0.975 0.85 60 0 1 0 30 3 50 0.2327 0.2327 0.2327\n") && table.models.empty(), "light table: nothing is taken before the format line (an older table names no lamp)");
      liteParseLine(table, "# a comment\n");
      liteParseLine(table, "format 3\n");
      const bool m1 = liteParseLine(table, "model 0000000000f29289 1 5 0.0000 1.5100 0.0000 1.0000 0.9750 0.8500 60.0000 0.0000 1.0000 0.0000 30.0000 3.0000 50.0000 0.2327 0.2327 0.2327\n");
      const bool m2 = liteParseLine(table, "model 000000000005a335 2 11 0 1.69 0.33 1 1 1 97 0 0 0 0 0 0 0 0 0 11 0 3.69 0.0111 0.8 0.8 0.76 40 0 1 0 35 2.2 20 0.2327 0.2327 0.2327\n");
      const bool m3 = liteParseLine(table, "model 0000000000000777 1 7 0 0 0 1 1 1 1 0 0 1 1 0 0 0 0 0\n");   // a window: no lamp
      const bool o1 = liteParseLine(table, "object 0000000000000604 0000000000f29289\n");
      CHECK(m1 && m2 && !m3 && o1 && table.models.size() == 2 && table.objects.size() == 1, "light table: two models with lamp lights and one object read; a model of windows alone is left out");
      const LiteModel* stand = table.model(0xf29289ull);
      CHECK(stand && stand->n == 1 && stand->lights[0].type == 5 && nearf(stand->lights[0].pos[1], 1.51f) && nearf(stand->lights[0].intensity, 60.f) && nearf(stand->lights[0].at[1], 1.f) && nearf(stand->lights[0].d[0], 30.f) && nearf(stand->lights[0].d[2], 50.f) && nearf(stand->lights[0].d[3], 0.2327f),
            "light table: the standing lamp's lamp shade with its direction, cone, bottom cone and shade");
      const LiteModel* street = table.model(0x5a335ull);
      CHECK(street && street->n == 2 && street->lights[0].type == 11 && street->lights[1].type == 11 && nearf(street->lights[1].pos[1], 3.69f) && nearf(street->lights[1].d[0], 35.f), "light table: a street lamp's two world lights");
      int how = -1;
      CHECK(table.find(0xf29289ull, 0x604ull, &how) == stand && how == 1 && table.find(0x123ull, 0x604ull, &how) == stand && how == 2 && table.find(0x604ull, 0x999ull, &how) == stand && how == 3 && table.find(0x123ull, 0x999ull, &how) == nullptr && how == 0,
            "light table: a lamp's definition by its model's key, by its object's key, by either in the other's place; none for a stranger");
      // the book of lit lights
      int h1 = 1, h2 = 2, h3 = 3;
      Lamps ls; bool fresh = false;
      ls.begin();
      Lamp* L = ls.name(0x1122334455667788ull, 0, fresh);
      CHECK(L && fresh && ls.n == 1 && ls.lit == 1 && L->seen && L->id == Lamps::lightId(0x1122334455667788ull, 0) && L->id != Lamps::lightId(0x1122334455667788ull, 1) && L->id != Lamps::lightId(0x1122334455667789ull, 0),
            "lamps: a lit light is entered by its object's id and its index; the id of its API light is its own");
      Lamp* second = ls.name(0x1122334455667788ull, 1, fresh);
      CHECK(second && second != L && fresh && ls.n == 2, "lamps: a second light of the same lamp is an entry of its own");
      L->api = &h1; L->api2 = &h2; L->api3 = &h3; L->sent = true;
      CHECK(ls.end() == 2 && ls.nGone == 0 && ls.out == 0, "lamps: named this frame, both stay");
      ls.begin();
      CHECK(ls.name(0x1122334455667788ull, 0, fresh) == ls.find(0x1122334455667788ull, 0) && !fresh && ls.lit == 2, "lamps: named again the next frame, it is the same entry");
      CHECK(ls.end() == 1 && ls.out == 1 && ls.nGone == 0 && ls.find(0x1122334455667788ull, 1) == nullptr, "lamps: the light not named any more is put out (it held no API light)");
      ls.begin();
      CHECK(ls.end() == 0 && ls.out == 2 && ls.nGone == 3 && ls.gone[0] == &h1 && ls.gone[1] == &h2 && ls.gone[2] == &h3, "lamps: put out, all three of its API lights go to the device (%u handed over)", ls.nGone);
      Lamps full; uint32_t made = 0;
      full.begin();
      for (int k = 0; k < Lamps::kMax + 5; ++k) if (full.name(1000ull + (uint64_t) k, 0, fresh)) ++made;
      CHECK(made == (uint32_t) Lamps::kMax && full.n == (uint32_t) Lamps::kMax, "lamps: the book holds %d lights and refuses the next", Lamps::kMax);
      {
        // the lamp reporter's block, version 3
        uint32_t head[16] = { kLampMagic0, kLampMagic1, kLampMagic2, 0x12345678u, 3u, 40u, 2u, kLampFloats, kLampCapacity, 7u, 1u, kLampInts, kLampHead, kLampHead + (kLampCapacity + 1u) * kLampFloats * 4u, 0u, 0u };
        LampHead lh = {};
        CHECK(lampReportHead((const uint8_t*) head, 0x12345678u, lh) && lh.lamps == 2 && lh.sequence == 40 && lh.world && lh.floatsAt == 64 && lh.intsAt == 64 + 513 * 24 * 4, "reporter: a whole head is taken: 2 lamps, sequence 40, a world loaded, the floats at 64, the ints after them");
        CHECK(!lampReportHead((const uint8_t*) head, 0x12345679u, lh), "reporter: a head that does not carry its own address is not the block (a copy of the signature elsewhere)");
        head[4] = 2u;
        CHECK(!lampReportHead((const uint8_t*) head, 0x12345678u, lh) && kLampMagic2 == 0x33303076u, "reporter: an older version's block is not read (the clock came with version 3)");
        head[4] = 3u; head[6] = kLampCapacity + 1u;
        CHECK(!lampReportHead((const uint8_t*) head, 0x12345678u, lh), "reporter: more lamps than the capacity is not a head to trust");
        head[6] = 2u; head[13] = kLampHead + 100u;
        CHECK(!lampReportHead((const uint8_t*) head, 0x12345678u, lh), "reporter: ints that would lie within the floats are not a head to trust");
        // the game's clock in the frame record
        float frameRec[24] = {}; frameRec[8] = 19.5f; frameRec[9] = 6.f; frameRec[10] = 18.f; frameRec[11] = 1.f;
        const GameClock evening = clockFromRecord(frameRec);
        CHECK(evening.known && evening.night && nearf(evening.hour, 19.5f) && nearf(evening.sunrise, 6.f) && nearf(evening.sunset, 18.f), "clock: half past seven in the evening, night by the game's word");
        frameRec[8] = 12.f; frameRec[11] = 0.f;
        CHECK(clockFromRecord(frameRec).known && !clockFromRecord(frameRec).night, "clock: noon is day");
        frameRec[8] = -1.f;
        CHECK(!clockFromRecord(frameRec).known, "clock: an hour the reporter could not read is not a clock");
        frameRec[8] = 12.f; frameRec[9] = 0.f; frameRec[10] = 0.f;
        CHECK(!clockFromRecord(frameRec).known, "clock: without sunrise and sunset it is not a clock");
        CHECK(isLitTerrainPs(0x17eabad58f650687ull) && isLitTerrainPs(0xd63bf505ec4a44a0ull) && !isLitTerrainPs(0x028ce2dde691b739ull) && !isLitTerrainPs(0x99ee53ff6ef1b0b6ull), "terrain light: the four lit terrain shaders carry it; the unlit world terrain and the composite do not");
        {
          // the game's light record as run 161 found it: direction toward the light, 0, colour, 1
          const float dir[3] = { -0.5065f, 0.8159f, 0.2790f }, col[3] = { 0.1373f, 0.1373f, 0.4674f };
          const float rec[8] = { -0.5065f, 0.8159f, 0.2790f, 0.f, 0.1373f, 0.1373f, 0.4674f, 1.f };
          float noZero[8], noOne[8], otherCol[8], opposite[8], moved[8];
          std::memcpy(noZero, rec, sizeof rec); noZero[3] = 1e-7f;
          std::memcpy(noOne, rec, sizeof rec); noOne[7] = 0.999f;
          std::memcpy(otherCol, rec, sizeof rec); otherCol[6] = 0.6f;
          std::memcpy(opposite, rec, sizeof rec); for (int q = 0; q < 3; ++q) opposite[q] = -rec[q];
          std::memcpy(moved, rec, sizeof rec); moved[0] += 0.01f;
          CHECK(lightRecord(rec, dir, col, 0.001f) && !lightRecord(noZero, dir, col, 0.001f) && !lightRecord(noOne, dir, col, 0.001f) && !lightRecord(otherCol, dir, col, 0.001f) && !lightRecord(opposite, dir, col, 0.02f),
                "the game's light record: the direction, exactly 0, the colour, exactly 1; another colour or the opposite direction is not it");
          CHECK(!lightRecord(moved, dir, col, 0.001f) && lightRecord(moved, dir, col, 0.02f), "the game's light record: a light that moved while the search ran is found within 0.02, and confirmed only within 0.001");
          float nan8[8]; std::memcpy(nan8, rec, sizeof rec); nan8[5] = std::nanf("");
          CHECK(!lightRecord(nan8, dir, col, 0.02f) && tripletMatch(opposite, dir, 0.02f) == -1, "the game's light record: a float that is not a number is no record; the opposite direction matches with sign -1");
          float v = -1.f;
          const float sw[5] = { 0.f, 1.f, 0.43f, 1.5f, -0.25f }; const float nanSw = std::nanf("");
          CHECK(nightSwitchValue(sw + 0, &v) && v == 0.f && nightSwitchValue(sw + 1, &v) && v == 1.f && nightSwitchValue(sw + 2, &v) && v == 0.43f,
                "the game's night switch: off, on, and part way through its fade");
          v = 7.f;
          CHECK(!nightSwitchValue(sw + 3, &v) && !nightSwitchValue(sw + 4, &v) && !nightSwitchValue(&nanSw, &v) && v == 7.f,
                "the game's night switch: outside 0..1 or not a number is not the switch, and nothing is written");
          float fs = 0.f, fe = 0.f;
          const float dayFog[4] = { -0.00022371f, 1.0067f, 5.f, 1.f }, nightFog[4] = { -0.00033445f, 1.0033f, 5.f, 1.f };
          CHECK(fogRangeFromGame(dayFog, &fs, &fe) && std::fabs(fs - 30.f) < 0.5f && std::fabs(fe - 4500.f) < 1.f && fogRangeFromGame(nightFog, &fs, &fe) && std::fabs(fs - 10.f) < 0.5f && std::fabs(fe - 3000.f) < 1.f,
                "the game's fog: the terrain's c4 by day (30 to 4500) and by night (10 to 3000), as run 163 read them");
          const float greyFog[4] = { -1.f / 850.f, 1000.f / 850.f, 5.f, 0.75f };
          CHECK(fogRangeFromGame(greyFog, &fs, &fe) && std::fabs(fe - 1000.f) < 0.5f && std::fabs(fs - (-25.4f)) < 1.f,
                "the game's fog: a grey day's curve 0.75 (150 to 1000) matched at half fog by a linear fog from -25 to 1000");
          const float noFog[4] = { 0.001f, 1.f, 5.f, 1.f }, noCurve[4] = { -0.001f, 1.f, 5.f, 0.f }, nanFog[4] = { std::nanf(""), 1.f, 5.f, 1.f };
          CHECK(!fogRangeFromGame(noFog, &fs, &fe) && !fogRangeFromGame(noCurve, &fs, &fe) && !fogRangeFromGame(nanFog, &fs, &fe), "the game's fog: a rising c4.x, no curve or not a number is no fog");
          uint32_t fc = 0; float fb = -1.f;
          const float black[3] = { 0.f, 0.f, 0.f }, white[3] = { 1.f, 1.f, 1.f }, grey[3] = { 0.5f, 0.5f, 0.5f }, wild[3] = { 1.5f, -1.f, 0.5f }, nanCol[3] = { 0.5f, std::nanf(""), 0.5f };
          const float tint[3] = { 0.5f, 0.25f, 1.f }, night[3] = { 0.06f, 0.06f, 0.25f };
          CHECK(fogColourFromGame(black, &fc, &fb) && fc == 0xFF000000u && fb == 0.f && fogColourFromGame(white, &fc, &fb) && fc == 0xFFFFFFFFu && fb == 1.f &&
                fogColourFromGame(grey, &fc, &fb) && fc == 0xFFFFFFFFu && std::fabs(fb - 0.2140f) < 0.001f,
                "the game's fog colour: gamma 0, 1 and 0.5 in linear light, the hue at full scale and the brightness apart (0.5 -> 0.214)");
          CHECK(fogColourFromGame(tint, &fc, &fb) && fc == 0xFF370DFFu && fb == 1.f && fogColourFromGame(night, &fc, &fb) && (fc & 0xFFu) == 0xFFu && ((fc >> 16) & 0xFFu) == 25u && std::fabs(fb - 0.0508f) < 0.001f,
                "the game's fog colour: a tint keeps its hue; the night's dark blue keeps it too (25 25 255 at brightness 0.051)");
          CHECK(fogColourFromGame(wild, &fc, &fb) && fc == 0xFFFF0037u && fb == 1.f && !fogColourFromGame(nanCol, &fc, &fb), "the game's fog colour: clamped to 0..1 per channel; not a number is no colour");
          {
            // a square's ground: 0..3 a quad of two triangles; 4, 5 skirt vertices under 0 and 1; 6 on the line through 0 and 1
            const std::vector<int32_t> gx = { 0, 64, 0, 64, 0, 64, 128 }, gz = { 0, 0, 64, 64, 0, 0, 0 };
            const std::vector<uint32_t> gi = { 0, 1, 2,  1, 3, 2,  0, 4, 5,  0, 5, 1,  0, 1, 6,  1, 2, 9 };
            std::vector<uint32_t> kept; MergeStats ms;
            mergeGroundTriangles(gx, gz, gi, kept, ms);
            CHECK(ms.in == 6 && ms.kept == 2 && ms.skirts == 2 && ms.flat == 1 && ms.outside == 1 && kept == std::vector<uint32_t>({ 0, 1, 2, 1, 3, 2 }),
                  "a square's shape: the ground's two triangles kept; the skirt's two, the one on a line and the one past the vertices left out");
            CHECK(rangesOverlap((0ull << 32) | 10u, (29ull << 32) | 1u) && !rangesOverlap((0ull << 32) | 10u, (30ull << 32) | 5u) && rangesOverlap((30ull << 32) | 5u, (40ull << 32) | 2u),
                  "a square's pieces: index ranges overlap when they share an index, not when one starts where the other ends");
          }
          CHECK(fogLightShare(1.f, 1.f) == 1.f && std::fabs(fogLightShare(0.0032f, 0.16f) - 0.02f) < 1e-4f && fogLightShare(0.1f, 0.f) == 1.f && fogLightShare(0.3f, 0.1f) == 1.f && fogLightShare(0.f, 0.16f) == 0.f,
                "the fog's light: by day 1, by night the moon's share (0.02), 1 while the game's light is out and the afterglow is not, never above 1, 0 with no light sent");
        }
        const int32_t ids[2] = { (int32_t) 0x55667788u, (int32_t) 0x11223344u };
        CHECK(lampId64(ids) == 0x1122334455667788ull, "reporter: an id from its low and high halves");
        //                      x      y      z     r    g    b   intens dimmer on  preset emits level
        const float dimBlueRec[12] = { 10.f, 45.9f, 20.f, 0.f, 0.f, 1.f, 0.6f, 0.6f, 1.f, 3.f,  1.f, 0.f };
        const float offRec[12]     = { 14.f, 46.7f, 22.f, 1.f, 1.f, 1.f, 1.f,  0.f,  0.f, 13.f, 0.f, 0.f };
        const float brightRec[12]  = { 14.f, 46.7f, 22.f, 1.f, 1.f, 1.f, 1.f,  1.7f, 1.f, 13.f, 1.f, 0.f };
        const float defCol[3] = { 1.f, 0.9f, 0.8f };
        const LampWord dimBlue = lampWordFromRecord(dimBlueRec, defCol, 60.f, 1.f), off = lampWordFromRecord(offRec, defCol, 60.f, 1.f), bright = lampWordFromRecord(brightRec, defCol, 60.f, 1.f);
        CHECK(dimBlue.on && nearf(dimBlue.level, 0.6f) && dimBlue.col[0] == 0.f && nearf(dimBlue.col[2], 0.36f), "reporter: a blue lamp at the dim level: on, blue, the definition's 60 at six tenths (%.2f %.2f %.2f)", dimBlue.col[0], dimBlue.col[1], dimBlue.col[2]);
        CHECK(!off.on && nearf(off.col[0], 0.6f) && nearf(off.col[2], 0.48f), "reporter: a lamp left at its default colour takes the definition's; switched off, it is off");
        CHECK(bright.on && nearf(bright.level, 1.7f) && nearf(bright.col[0], 1.02f), "reporter: the level is the engine's dimmer, whatever the lamp's intensity field says (bright: x%.2f)", bright.level);
        const float custom[12] = { 0.f, 0.f, 0.f, 255.f, 128.f, 0.f, 1.f, 1.f, 1.f, 9.f, 1.f, 0.f };
        const LampWord c = lampWordFromRecord(custom, defCol, 100.f, 1.f);
        CHECK(c.on && nearf(c.col[0], 1.f) && nearf(c.col[1], 128.f / 255.f), "reporter: a custom colour given in 0..255 is scaled to 0..1");
        // a point and a direction through an object's transform
        const float rowsY90[12] = { 0.f, 0.f, 2.f, 10.f,  0.f, 2.f, 0.f, 20.f,  -2.f, 0.f, 0.f, 30.f };   // a quarter turn about y, scaled by two, translated
        const float front[3] = { 0.f, 0.f, -1.f }, zero[3] = {}, up1[3] = { 0.f, 1.f, 0.f }; float wd[3], wp[3];
        worldPoint(rowsY90, up1, wp);
        CHECK(nearf(wp[0], 10.f) && nearf(wp[1], 22.f) && nearf(wp[2], 30.f), "lamps: a point of the model through the object's transform (%.1f %.1f %.1f)", wp[0], wp[1], wp[2]);
        CHECK(worldDir(rowsY90, front, wd) && nearf(wd[0], -1.f) && nearf(wd[1], 0.f) && nearf(wd[2], 0.f), "lamps: a direction through it is rotated, not translated, and of unit length (%.2f %.2f %.2f)", wd[0], wd[1], wd[2]);
        CHECK(!worldDir(rowsY90, zero, wd) && wd[0] == 0.f, "lamps: a point light's zero vector gives no direction");
      }
    }
    const AlbedoStage* tc = findAlbedoStage(0x0c19795eb80e2e96ull);
    const AlbedoStage* tn = findAlbedoStage(0x5aee1186d554dbc4ull);
    const float purple[3] = { 0.5f, 0.25f, 1.0f }, hot[3] = { 2.f, -1.f, 0.5f };
    CHECK(tc && tc->tint && kTintRegister == 8 && findAlbedoStage(0x1234ull) == nullptr && (!tn || !tn->tint),
          "tint: the recolourable object PS carries the tint at c8; the 3-light variant carries none; unknown -> none");
    CHECK(packTint(purple) == 0xFF8040FFu && packTint(hot) == 0xFFFF0080u, "packTint: ARGB with rounding and clamping (%08X, %08X)", packTint(purple), packTint(hot));
    {
      // the sun: the terrain's light as the game hands it over (values of run 152)
      const float noonCol[4] = { 1.f, 1.f, 0.995f, 0.f }, noonDir[4] = { 0.019f, 0.946f, 0.324f, 0.f };
      Sun noon = {};
      CHECK(skyLightFrom(noonCol, noonDir, noon) && nearf(noon.col[2], 0.995f) && nearf(len3(noon.dir), 1.f) && nearf(noon.dir[1], 0.946f), "sun: the terrain's light at noon, white from high up (%.3f %.3f %.3f)", noon.dir[0], noon.dir[1], noon.dir[2]);
      const float darkCol[4] = { 0.f, 0.f, 0.f, 0.f }, moonDir[4] = { 0.645f, 0.723f, 0.247f, 0.f };
      Sun dark = {};
      CHECK(skyLightFrom(darkCol, moonDir, dark) && luminance(dark.col) == 0.f, "sun: at 19 h the game's light is zero, and that is taken as it is");
      const float moonCol[4] = { 0.137f, 0.137f, 0.392f, 0.f };
      Sun moon = {};
      CHECK(skyLightFrom(moonCol, moonDir, moon) && nearf(luminance(moon.col), 0.1554f), "sun: by night it is the moon's blue (luminance %.4f)", luminance(moon.col));
      const float noDir[4] = { 0.f, 0.f, 0.f, 0.f }, longDir[4] = { 0.f, 2.f, 0.f, 0.f }, downDir[4] = { 0.f, -1.f, 0.f, 0.f }, badCol[4] = { -0.5f, 1.f, 1.f, 0.f }, hugeCol[4] = { 100.f, 1.f, 1.f, 0.f };
      Sun none = {};
      CHECK(!skyLightFrom(noonCol, noDir, none) && !skyLightFrom(noonCol, longDir, none) && !skyLightFrom(noonCol, downDir, none), "sun: constants whose direction is not a unit vector from above are not a light's");
      CHECK(!skyLightFrom(badCol, noonDir, none) && !skyLightFrom(hugeCol, noonDir, none), "sun: a negative colour or one beyond any light's is not a light's");
      Sun warmer = noon; warmer.col[1] -= 0.01f;
      Sun turned = noon; turned.dir[0] += 0.01f; { const float n = len3(turned.dir); for (int q = 0; q < 3; ++q) turned.dir[q] /= n; }
      Sun nearly = noon; nearly.col[0] -= 0.002f;
      CHECK(sameSun(noon, noon) && sameSun(noon, nearly) && !sameSun(noon, warmer) && !sameSun(noon, turned), "sun: made anew for a hundredth of colour or half a degree, not for less than half a hundredth");
      // dusk and dawn: a game day and a half by the game's own timeline (SunMoonLight / 255, linear between the keys),
      // the light from the east until noon and after 19 h, from the west until 19 h and after midnight until 5 h
      struct Key { float t, r, g, b; };
      static const Key keys[] = { { 0.f, 35, 35, 110 }, { 3.f, 35, 35, 120 }, { 4.f, 35, 35, 120 }, { 6.f, 0, 0, 0 }, { 6.2f, 200, 90, 50 }, { 7.f, 255, 255, 180 }, { 12.f, 255, 255, 255 }, { 17.f, 255, 245, 235 },
                                  { 18.8f, 120, 40, 40 }, { 19.f, 0, 0, 0 }, { 20.f, 35, 35, 100 }, { 21.f, 35, 35, 100 }, { 24.f, 35, 35, 110 } };
      auto gameLight = [&](float hour) {
        Sun s = {};
        for (size_t k = 0; k + 1 < sizeof keys / sizeof keys[0]; ++k) if (hour >= keys[k].t && hour <= keys[k + 1].t) {
          const float a = (hour - keys[k].t) / (keys[k + 1].t - keys[k].t);
          s.col[0] = (keys[k].r + a * (keys[k + 1].r - keys[k].r)) / 255.f; s.col[1] = (keys[k].g + a * (keys[k + 1].g - keys[k].g)) / 255.f; s.col[2] = (keys[k].b + a * (keys[k + 1].b - keys[k].b)) / 255.f;
          break;
        }
        const bool east = (hour >= 5.f && hour < 12.f) || hour >= 19.f;
        s.dir[0] = east ? 0.664f : -0.664f; s.dir[1] = 0.707f; s.dir[2] = 0.242f;
        return s;
      };
      auto clockAt = [](float hour) { GameClock c; c.hour = hour; c.sunrise = 6.f; c.sunset = 18.f; c.night = hour >= 18.f || hour <= 6.f; c.known = true; return c; };
      CHECK(!clockMoonTime(clockAt(18.5f)) && clockMoonTime(clockAt(19.f)) && clockMoonTime(clockAt(5.9f)) && !clockMoonTime(clockAt(6.f)) && !clockMoonTime(clockAt(12.f)), "clock: the light is the moon's from an hour after sunset until sunrise");
      GameClock unknownClock = {};
      CHECK(nearf(dawnEase(clockAt(6.f), 1.f), 0.f) && nearf(dawnEase(clockAt(6.5f), 1.f), 0.5f) && nearf(dawnEase(clockAt(7.f), 1.f), 1.f) && nearf(dawnEase(clockAt(5.9f), 1.f), 1.f) && nearf(dawnEase(clockAt(12.f), 1.f), 1.f),
            "dawn: eased over an hour after sunrise, nothing at sunrise, half at half past, the game's own from seven; untouched before sunrise and by day");
      CHECK(nearf(dawnEase(clockAt(6.5f), 0.f), 1.f) && nearf(dawnEase(unknownClock, 1.f), 1.f) && nearf(dawnEase(clockAt(6.5f), 2.f), 0.25f), "dawn: no ease without minutes or without the clock; two hours ease a quarter at half past six");
      CHECK(duskFade(clockAt(18.5f), 1.f) < 0.f && nearf(duskFade(clockAt(19.f), 1.f), 0.f) && nearf(duskFade(clockAt(19.5f), 1.f), 0.5f) && nearf(duskFade(clockAt(20.f), 1.f), 1.f) && nearf(duskFade(clockAt(1.f), 1.f), 1.f) && nearf(duskFade(clockAt(19.5f), 0.f), 1.f) && nearf(duskFade(unknownClock, 1.f), 1.f),
            "dusk: before the light is the moon's the fade is not begun; from an hour after sunset it rises over the hours given; past it, without hours or without the clock it is done");
      CHECK(clockDusk(clockAt(18.f)) && clockDusk(clockAt(19.9f)) && !clockDusk(clockAt(20.f)) && !clockDusk(clockAt(4.f)) && !clockDusk(clockAt(12.f)) && !clockDusk(unknownClock), "clock: dusk is the two hours from sunset");
      {
        // a game day from noon: the moon at a twentieth, the cross-fades an hour each: at dusk the afterglow (from a tenth
        // of full) against the rising moon, at dawn the moon against the eased sun
        SkyLights sky; int begins = 0, ends = 0;
        bool noonSun = false, glowHeld = false, duskBoth = false, glowGone = false, nightMoon = false, moonMoves = false, dawnSunAlone = false, morningSun = false, sunriseKept = false, dawnBoth = false; float glowAt = 0.f, endAt = 0.f, lowest = 9.f, lowestAt = 0.f;
        for (int i = 0; i <= 2000; ++i) {   // to eight the next morning, a hundredth of an hour a step
          const float t = 12.f + 0.01f * (float) i, hour = t >= 24.f ? t - 24.f : t;
          const GameClock c = clockAt(hour);
          const float kDawn = clockMoonTime(c) ? 1.f : dawnEase(c, 1.f);
          Sun g = gameLight(hour); if (kDawn < 1.f) for (int q = 0; q < 3; ++q) g.col[q] *= kDawn;   // as the device eases the sun
          const int what = sky.step(g, clockMoonTime(c), 0.05f, duskFade(c, 1.f), kDawn, clockDusk(c), 0.1f);
          if (what == 1) { ++begins; glowAt = hour; } if (what == 2) { ++ends; endAt = hour; }
          if (t >= 19.f && t <= 31.f && sky.level() < lowest) { lowest = sky.level(); lowestAt = hour; }
          if (i == 0) noonSun = sky.showing[0] && !sky.showing[1] && nearf(luminance(sky.shown[0].col), 1.f);
          if (nearf(t, 18.97f)) glowHeld = sky.glowing && sky.showing[0] && !sky.showing[1] && nearf(luminance(sky.shown[0].col), 0.1f) && sky.shown[0].dir[0] < 0.f;   // held at a tenth until the moon comes
          if (nearf(t, 19.5f)) duskBoth = sky.showing[0] && sky.showing[1] && sky.glowing && nearf(sky.glowFade, 0.5f) && nearf(luminance(sky.shown[0].col), 0.05f) && nearf(luminance(sky.shown[1].col), 0.5f * 0.05f * 0.1556f) && sky.shown[0].dir[0] < 0.f && sky.shown[1].dir[0] > 0.f;   // half way: half the afterglow, half the floor
          if (nearf(t, 20.1f)) glowGone = !sky.showing[0] && sky.showing[1] && !sky.glowing && nearf(luminance(sky.shown[1].col), 0.05f * 0.1556f);
          if (nearf(t, 25.f)) nightMoon = !sky.showing[0] && sky.showing[1] && nearf(luminance(sky.shown[1].col), 0.05f * luminance(gameLight(1.f).col));
          if (nearf(t, 29.5f)) moonMoves = sky.showing[1] && !sky.showing[0] && sky.shown[1].dir[0] > 0.f && nearf(luminance(sky.shown[1].col), 0.05f * 0.1613f);   // on the side the game has it at 5.5 h, at its floor while the game fades it
          if (nearf(t, 30.f)) sunriseKept = sky.showing[0] && sky.showing[1] && sky.keeping && nearf(luminance(sky.shown[1].col), 0.05f * 0.1613f) && nearf(luminance(sky.shown[0].col), 0.f);   // at sunrise the sun is nothing yet, the moon whole
          if (nearf(t, 30.5f)) dawnBoth = sky.showing[0] && sky.showing[1] && sky.keeping && nearf(sky.keepFade, 0.5f) && nearf(luminance(sky.shown[1].col), 0.5f * 0.05f * 0.1613f) && nearf(luminance(sky.shown[0].col), 0.5f * luminance(gameLight(6.5f).col));   // half way: half the moon, half the sun
          if (nearf(t, 31.f)) dawnSunAlone = sky.showing[0] && !sky.showing[1] && !sky.keeping && nearf(luminance(sky.shown[0].col), luminance(gameLight(7.f).col));
          if (nearf(t, 31.5f)) morningSun = sky.showing[0] && !sky.showing[1];
        }
        CHECK(noonSun && glowHeld && duskBoth && glowGone && nightMoon && moonMoves && sunriseKept && dawnBoth && dawnSunAlone && morningSun && begins == 1 && ends == 1,
              "sky: the sun alone at noon; the afterglow held at a tenth from %.2f h until the moon comes; at 19.5 h half the afterglow from the west and half the floor from the east; the moon alone from %.2f h, at its share by night, at its floor while the game fades it; at sunrise the moon whole and the sun nothing; at 6.5 h half of each; the sun alone from 7 h", glowAt, endAt);
        CHECK(lowest > 0.05f * 0.1556f - 0.0005f, "sky: from 19 h to the morning the light of the sky never falls below the moon's floor (lowest %.4f at %.2f h)", lowest, lowestAt);
        CHECK(glowAt > 18.9f && glowAt < 18.95f && endAt > 19.95f && endAt < 20.05f, "sky: the afterglow begins when the sun falls below a tenth (%.2f h) and is gone an hour after the moon came (%.2f h)", glowAt, endAt);
        SkyLights plain; const Sun g = gameLight(19.5f);
        plain.step(g, false, 0.5f, 1.f, 1.f, false, 0.1f);
        CHECK(plain.showing[0] && !plain.showing[1] && !plain.glowing && nearf(luminance(plain.shown[0].col), luminance(g.col)), "sky: without the clock the light goes out as the game has it");
        SkyLights late; const Sun d = gameLight(18.95f);   // the hook comes into the dusk: no light of the sun seen at the level
        const int what = late.step(d, false, 0.5f, -1.f, 1.f, true, 0.1f);
        CHECK(what == 0 && !late.glowing && late.showing[0] && nearf(luminance(late.shown[0].col), luminance(d.col)), "sky: coming into a dusk under way there is nothing to hold; the game's light as it is");
        SkyLights none; none.step(gameLight(12.f), false, 0.5f, -1.f, 1.f, false, 0.1f); none.step(gameLight(18.95f), false, 0.5f, -1.f, 1.f, true, 0.1f); none.step(gameLight(19.1f), true, 0.5f, 1.f, 1.f, true, 0.1f);
        CHECK(!none.glowing && !none.showing[0] && none.showing[1] && nearf(luminance(none.shown[1].col), 0.5f * 0.1556f), "sky: with no minutes the afterglow is gone the moment the moon comes, and the moon stands at its floor at once");
        SkyLights dark; dark.step(gameLight(23.f), true, 0.f, 1.f, 1.f, false, 0.1f); dark.step(gameLight(6.02f), false, 0.f, -1.f, 0.02f, false, 0.1f);
        CHECK(!dark.keeping && !dark.showing[1] && dark.showing[0], "sky: with no share for the moon there is no floor and nothing is kept");
      }
    }
  }

  // --- reflection passes (run 66): a mirrored camera is never the play camera, whichever way it looks
  {
    float rows[16]; memcpy(rows, k1277907, sizeof rows);            // the sea reflection pass: looks up, improper
    Camera sc = {};
    CHECK(decompose(fromRows(rows), sc) && sc.mirrored && sc.fwd[1] > 0.05f && kindOfVerified(sc) == Kind::OtherCamera, "camera: the sea reflection camera is improper (det -1) and looks up -> OtherCamera");
    for (int r = 0; r < 4; ++r) rows[r*4+1] = -rows[r*4+1];         // un-mirrored across the water: the play camera
    Camera mc = {};
    CHECK(decompose(fromRows(rows), mc) && !mc.mirrored && mc.fwd[1] < -0.05f && kindOfVerified(mc) == Kind::Main, "camera: the play camera is a proper rotation (det +1) looking down -> Main");
    for (int r = 0; r < 4; ++r) rows[r*4+0] = -rows[r*4+0];         // reflected across a vertical plane: a wall mirror's pass
    Camera wc = {};
    CHECK(decompose(fromRows(rows), wc) && wc.mirrored && wc.fwd[1] < -0.05f && kindOfVerified(wc) == Kind::OtherCamera, "camera: a wall mirror's reflection camera looks down too but is improper -> OtherCamera (the mirrored house and terrain popping at close zoom)");
    // the play camera tilted to the horizon (and a touch above it) is still the play camera: only the basis decides
    Camera hc = mc; hc.fwd[1] = 0.f;
    CHECK(kindOfVerified(hc) == Kind::Main, "camera: a proper camera at the horizon -> Main (the old pitch test rejected it and broke the far view)");
    hc.fwd[1] = 0.2f;
    CHECK(kindOfVerified(hc) == Kind::Main, "camera: a proper camera looking slightly up -> Main");
    CHECK(isMirrorPass(TRUE, D3DCULL_CCW) && !isMirrorPass(FALSE, D3DCULL_CCW) && !isMirrorPass(TRUE, D3DCULL_CW) && !isMirrorPass(0, D3DCULL_CW),
          "draw: stencil on with the winding flipped = the stencil-mirror pass -> not captured; either state alone, or the defaults, capture as before");
    CHECK(isReflectionDraw(true, true, D3DZB_TRUE, FALSE, D3DCULL_CW) && isReflectionDraw(false, true, D3DZB_TRUE, TRUE, D3DCULL_CCW) && !isReflectionDraw(false, true, D3DZB_TRUE, FALSE, D3DCULL_CW)
          && !isReflectionDraw(true, false, D3DZB_TRUE, FALSE, D3DCULL_CW) && !isReflectionDraw(true, true, D3DZB_FALSE, FALSE, D3DCULL_CW),
          "draw: a 3D draw under a mirrored camera, or with the stencil mirror's states, is a reflection pass's and is dropped; 2D or depth-off draws under it (the compositor, the UI) are not");
  }

  // --- wall openings (milestone 13): the cut on the captured wall buffers (native trace, call
  // 1278548 = walls A, base 48, start 96, 40 triangles; the mask atlas's second upload)
  {
    std::vector<uint8_t> vb0, vb1, ib, atlas;
    const bool have = loadBytes("walls-data/wall-vb0-1110320.bin", vb0) && loadBytes("walls-data/wall-vb1-1105054.bin", vb1)
                   && loadBytes("walls-data/wall-ib-1180214.bin", ib) && loadBytes("walls-data/mask-atlas-1110316.bin", atlas);
    if (!have) SKIP("walls: the captured wall buffers (walls-data/) not found");
    else CHECK(vb0.size() == 71280 && vb1.size() == 6480 && ib.size() == 6456 && atlas.size() == 32768, "walls: the captured wall buffers and mask atlas (walls-data/) have the traced sizes");
    if (have) {
      std::vector<uint8_t> red;
      CHECK(decodeMaskRed(D3DFMT_DXT1, atlas.data(), atlas.size(), 256, 256, red) && red.size() == 65536 && maskBytes(D3DFMT_DXT1, 256, 256) == 32768, "walls: DXT1 atlas decoded to a red plane");
      // the window cell (1792, 64, 1024, 2048) = texels 112..176 x 4..132; its black box at a plain 0.5 threshold
      int xmin = 999, xmax = -1, ymin = 999, ymax = -1, count = 0;
      for (int y = 4; y < 132; ++y) for (int x = 112; x < 176; ++x) if (red[y * 256 + x] < 128) { xmin = (std::min)(xmin, x); xmax = (std::max)(xmax, x); ymin = (std::min)(ymin, y); ymax = (std::max)(ymax, y); ++count; }
      CHECK(xmin == 119 && xmax == 168 && ymin == 24 && ymax == 76 && count == 2650, "walls: the window's black box in its cell is x 119..168, y 24..76, filled (%d,%d..%d,%d: %d texels)", xmin, ymin, xmax, ymax, count);
      const D3DVERTEXELEMENT9 decl[] = {
        { 0, 0, D3DDECLTYPE_SHORT4, 0, D3DDECLUSAGE_POSITION, 0 }, { 0, 8, D3DDECLTYPE_SHORT4, 0, D3DDECLUSAGE_TEXCOORD, 0 }, { 0, 16, D3DDECLTYPE_SHORT2, 0, D3DDECLUSAGE_TEXCOORD, 1 },
        { 0, 20, D3DDECLTYPE_SHORT4, 0, D3DDECLUSAGE_TEXCOORD, 2 }, { 0, 28, D3DDECLTYPE_SHORT2, 0, D3DDECLUSAGE_TEXCOORD, 3 }, { 0, 32, D3DDECLTYPE_SHORT4, 0, D3DDECLUSAGE_TEXCOORD, 4 },
        { 0, 40, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_NORMAL, 0 }, { 1, 0, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_TEXCOORD, 5 }, D3DDECL_END() };
      WallLayout L;
      CHECK(wallLayoutFromDecl(decl, L) && L.valid && L.posOff == 0 && L.tc2Off == 20 && L.tc4Off == 32 && L.flagOff == 0 && L.minStride0 == 44 && L.minStride1 == 4 && L.elemCount == 8,
            "walls: the trace's declaration -> the wall layout (stride 44 + 4)");
      const D3DVERTEXELEMENT9 plain[] = { { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, { 0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
      WallLayout L2;
      CHECK(!wallLayoutFromDecl(plain, L2) && !L2.valid, "walls: an ordinary declaration is not a wall layout");
      WallCutInput in; in.layout = L;
      in.vb0 = vb0.data(); in.vb0Size = vb0.size(); in.stride0 = 44; in.vb1 = vb1.data(); in.vb1Size = vb1.size(); in.stride1 = 4;
      in.ib = ib.data(); in.ibSize = ib.size(); in.baseVertex = 48; in.startIndex = 96; in.primCount = 40;
      in.mask = red.data(); in.maskW = in.maskH = 256;
      WallCutOutput o;
      // as captured (c8 = 0, 1 and every flag 0): cut-away stubs, the window sits above them
      in.params.clampLo = 0.f; in.params.clampHi = 1.f;
      CHECK(cutWallOpenings(in, o) && !o.changed && o.stats.triangles == 40 && o.stats.cells == 2 && o.stats.cellsWithOpenings == 1 && o.stats.cut == 0 && o.stats.removed == 0,
            "walls: stubs (k = 0): the window cell has openings but no triangle reaches them -> unchanged (%u cells, %u with openings, %u rectangles)", o.stats.cells, o.stats.cellsWithOpenings, o.stats.holeRects);
      // walls up (the clamp forces k = 1): the window segment's four triangles are cut
      in.params.clampLo = in.params.clampHi = 1.f;
      CHECK(cutWallOpenings(in, o) && o.changed && o.stats.cut == 4 && o.stats.removed == 0 && o.triangleCount > 40 && o.vertexCount > 60
            && o.ib.size() == o.triangleCount * 3 && o.vb0.size() == o.vertexCount * 44 && o.vb1.size() == o.vertexCount * 4,
            "walls: up (k = 1): 4 triangles cut -> %u triangles, %u vertices (%u rectangles)", o.triangleCount, o.vertexCount, o.stats.holeRects);
      {
        // the discarded texels of the cell by the shaders' rule at k = 1 (mask.red + max(0.5 (1 - t), 0) < 0.5)
        auto killed = [&](int x, int y) { const float t = (y + 0.5f - 4.f) / 128.f, need = (0.5f - 0.5f * (1.f - t)) * 255.f; return (float) red[y * 256 + x] < need; };
        double holeArea = 0; int holeTexels = 0, kyMin = 999, kyMax = -1;
        for (int y = 4; y < 132; ++y) for (int x = 112; x < 176; ++x) if (killed(x, y)) { ++holeTexels; holeArea += (1.0 / 64.0) * (1.0 / 128.0); kyMin = (std::min)(kyMin, y); kyMax = (std::max)(kyMax, y); }
        auto st = [&](const uint8_t* v, float& s, float& t) { int16_t tc2[4]; memcpy(tc2, v + 20, 8); s = tc2[0] / 4096.f; t = tc2[1] / 4096.f; };   // k = 1: t = TEXCOORD2.y
        auto rectX = [&](const uint8_t* v) { int16_t r[4]; memcpy(r, v + 32, 8); return r[0]; };
        auto triArea = [&](const uint8_t* const v[3]) { float s[3], t[3]; for (int i = 0; i < 3; ++i) st(v[i], s[i], t[i]); return std::fabs((s[1] - s[0]) * (t[2] - t[0]) - (s[2] - s[0]) * (t[1] - t[0])) / 2; };
        double areaBefore = 0, areaAfter = 0; int inside = 0, kept = 0, atTop = 0, atBottom = 0;
        for (int t = 0; t < 40; ++t) { const uint16_t* ix = (const uint16_t*) ib.data() + 96 + 3 * t; const uint8_t* v[3]; for (int i = 0; i < 3; ++i) v[i] = vb0.data() + (ix[i] + 48) * 44; if (rectX(v[0]) == 1792) areaBefore += triArea(v); }
        for (uint32_t t = 0; t < o.triangleCount; ++t) { const uint8_t* v[3]; for (int i = 0; i < 3; ++i) v[i] = o.vb0.data() + o.ib[3 * t + i] * 44; if (rectX(v[0]) == 1792) areaAfter += triArea(v); }
        const float bt0 = (kyMin * 16 - 64) / 2048.f, bt1 = ((kyMax + 1) * 16 - 64) / 2048.f;   // the opening's top and bottom rows in t
        for (uint32_t v = 0; v < o.vertexCount; ++v) {
          const uint8_t* p = o.vb0.data() + v * 44; if (rectX(p) != 1792) continue;
          float s, t; st(p, s, t);
          const float px = 112.f + s * 64.f, py = 4.f + t * 128.f, fx = px - floorf(px), fy = py - floorf(py);
          if (fx > 0.02f && fx < 0.98f && fy > 0.02f && fy < 0.98f && killed((int) floorf(px), (int) floorf(py))) ++inside;   // strictly inside a discarded texel
          int16_t pos[4]; memcpy(pos, p, 8);
          if (std::fabs(t - bt0) < 2e-3f && std::abs(pos[1] - (int) lroundf(767.f * (1.f - bt0))) <= 1) ++atTop;
          if (std::fabs(t - bt1) < 2e-3f && std::abs(pos[1] - (int) lroundf(767.f * (1.f - bt1))) <= 1) ++atBottom;
        }
        for (int v = 84; v <= 89; ++v) { const uint8_t* src = vb0.data() + v * 44; for (uint32_t w = 0; w < o.vertexCount; ++w) if (!memcmp(o.vb0.data() + w * 44, src, 44)) { ++kept; break; } }
        CHECK(std::fabs(areaBefore - 0.96997) < 1e-3 && std::fabs((areaBefore - holeArea) - areaAfter) < 3e-3,
              "walls: the window segment's area %.4f minus the discarded texels %.4f (%d texels, rows %d..%d) = %.4f after the cut", areaBefore, holeArea, holeTexels, kyMin, kyMax, areaAfter);
        CHECK(inside == 0 && kept == 6, "walls: no output vertex inside the opening (%d), all six corners of the segment kept (%d)", inside, kept);
        CHECK(atTop >= 2 && atBottom >= 2, "walls: vertices on the opening's top row (%d) and bottom row (%d) carry the interpolated height (y = 767 (1 - t))", atTop, atBottom);
      }
      in.primCount = 24;
      CHECK(cutWallOpenings(in, o) && !o.changed, "walls: the plain segments alone -> unchanged");
      in.primCount = 40; in.params.threshold = 0.5f + 64.f / 255.f;
      CHECK(cutWallOpenings(in, o) && o.changed && o.stats.cut == 4 && o.stats.removed == 0 && o.stats.cellsWithOpenings == 1, "walls: the alpha-tested threshold (walls C, reference 64) cuts the same four triangles and leaves the plain segments alone (%u cells with openings)", o.stats.cellsWithOpenings);
      in.params.threshold = 0.5f; in.mask = nullptr;
      CHECK(cutWallOpenings(in, o) && o.changed && o.stats.removed == 4 && o.stats.cut == 0 && o.triangleCount == 36, "walls: no mask bound (black): the window segment goes whole (%u triangles left)", o.triangleCount);
      in.mask = red.data(); in.startIndex = 3200;   // 3228 indices in the buffer: 3200 + 120 is past its end
      CHECK(!cutWallOpenings(in, o), "walls: an index range past the buffer is refused");
      in.startIndex = 96; in.stride0 = 40;
      CHECK(!cutWallOpenings(in, o), "walls: a stride too small for the layout is refused");
      in.stride0 = 44;
      // hidden vertices (flag.z = 0 while cK.z = 0): the shader collapses them to the clip centre and the
      // game's mask hides the fans; every triangle with a hidden corner is dropped
      {
        std::vector<uint8_t> flags = vb1;
        for (int v = 87; v <= 89; ++v) flags[v * 4 + 2] = 0;   // the window segment's top row hidden
        WallCutInput hi = in; hi.vb1 = flags.data(); hi.params.clampLo = hi.params.clampHi = 1.f; hi.params.clampVis = 0.f; hi.params.threshold = 0.5f;
        WallCutOutput ho;
        CHECK(cutWallOpenings(hi, ho) && ho.changed && ho.stats.hidden == 4 && ho.stats.hiddenMixed == 4 && ho.stats.hiddenCorners == 6 && ho.stats.cut == 0 && ho.triangleCount == 36,
              "walls: a hidden top row drops the segment's four mixed triangles (%u hidden, %u mixed, %u corners, %u triangles left)", ho.stats.hidden, ho.stats.hiddenMixed, ho.stats.hiddenCorners, ho.triangleCount);
        hi.params.clampVis = 1.f;   // the walls-up override (cK.z = 1) makes every vertex visible
        CHECK(cutWallOpenings(hi, ho) && ho.changed && ho.stats.hidden == 0 && ho.stats.cut == 4, "walls: cK.z = 1 overrides the hidden flag: the segment is cut, not dropped");
        for (int v = 84; v <= 89; ++v) flags[v * 4 + 2] = 0;
        hi.params.clampVis = 0.f;
        CHECK(cutWallOpenings(hi, ho) && ho.stats.hidden == 4 && ho.stats.hiddenMixed == 0 && ho.stats.hiddenCorners == 12 && ho.triangleCount == 36, "walls: a fully hidden segment: four triangles dropped, none mixed (%u corners)", ho.stats.hiddenCorners);
      }
      {
        // a segment edge-on in mask space (all corners on one s): the cut would leave nothing, so it is left whole
        std::vector<uint8_t> edge = vb0;
        for (int v = 84; v <= 89; ++v) { int16_t tc2[4]; memcpy(tc2, &edge[v * 44 + 20], 8); tc2[0] = 2048; memcpy(&edge[v * 44 + 20], tc2, 8); }
        WallCutInput ei = in; ei.vb0 = edge.data(); ei.params.clampLo = ei.params.clampHi = 1.f; ei.params.threshold = 0.5f;
        WallCutOutput eo;
        CHECK(cutWallOpenings(ei, eo) && !eo.changed && eo.stats.cut == 0 && eo.stats.removed == 0, "walls: a segment with no area in (s, t) is left whole rather than removed");
      }
      {
        uint8_t px[32]; for (int i = 0; i < 32; ++i) px[i] = 0xFF;
        std::vector<uint8_t> r16;
        CHECK(decodeMaskRed(D3DFMT_A1R5G5B5, px, sizeof px, 4, 4, r16) && r16.size() == 16 && r16[0] == 255 && r16[15] == 255 && maskBytes(D3DFMT_A1R5G5B5, 4, 4) == 32, "walls: the game's blank 4x4 A1R5G5B5 mask decodes white");
        uint8_t px565[2] = { 0x00, 0xF8 };   // red 31, green 0, blue 0
        CHECK(decodeMaskRed(D3DFMT_R5G6B5, px565, 2, 1, 1, r16) && r16[0] == 255 && maskBytes(D3DFMT_R5G6B5, 1, 1) == 2, "walls: R5G6B5 red decodes");
      }
      CHECK(wallCutEnabled(), "walls: the cut is on by default (SIMS3_WALL_CUT unset)");
    }
    // the shader facts, on the in-game dumps
    {
      std::vector<DWORD> vsA, vsB, vsC, vsD, psA, psB, psC, psD, psD2, obj, psObj;
      const bool haveAll = loadShader("vs_f64835ccff6bffd7", vsA) && loadShader("vs_84b06922c8593a48", vsB) && loadShader("vs_25812b76d378f9bf", vsC) && loadShader("vs_bd6dc15e1ae0dc79", vsD)
                        && loadShader("ps_936215cf1e55a47e", psA) && loadShader("ps_6535a7bb14726b00", psB) && loadShader("ps_7d2cbb8e474dfaf5", psC) && loadShader("ps_f0d7af09599ed1bc", psD)
                        && loadShader("ps_579ba93e4482bba9", psD2) && loadShader("vs_0ba6ddb9aa01913c", obj) && loadShader("ps_0c19795eb80e2e96", psObj);
      if (!haveAll) SKIP("walls: in-game wall shader dumps not all found (vs f64835cc / 84b06922 / 25812b76 / bd6dc15e, ps 936215cf / 6535a7bb / 7d2cbb8e / f0d7af09 / 579ba93e, vs 0ba6ddb9, ps 0c19795e)");
      if (haveAll) {
        WallVsInfo w;
        CHECK(analyzeWallVertexShader(vsA.data(), vsA.size(), w) && w.clampReg == 8 && nearf(w.kScale, 1.f), "walls A VS f64835cc: up-ness clamp at c8, scale 1");
        CHECK(analyzeWallVertexShader(vsB.data(), vsB.size(), w) && w.clampReg == 12 && nearf(w.kScale, 1.f), "walls B VS 84b06922 (vs_3_0): clamp at c12, scale 1");
        CHECK(analyzeWallVertexShader(vsC.data(), vsC.size(), w) && w.clampReg == 8 && nearf(w.kScale, 0.99f), "walls C VS 25812b76: clamp at c8, scale 0.99 (%.3f)", w.kScale);
        CHECK(analyzeWallVertexShader(vsD.data(), vsD.size(), w) && w.clampReg == 8 && nearf(w.kScale, 1.f), "walls D VS bd6dc15e: clamp at c8, scale 1");
        CHECK(!analyzeWallVertexShader(obj.data(), obj.size(), w) && !w.valid, "object VS 0ba6ddb9 is not a wall shader");
        PsAnalysis a;
        CHECK(analyzePixelShader(psA.data(), psA.size(), a) && a.maskSampler == 3 && a.maskKill && !a.maskAlpha, "walls A PS 936215cf: the mask at s3 feeds a texkill");
        CHECK(analyzePixelShader(psB.data(), psB.size(), a) && a.maskSampler == 4 && a.maskKill, "walls B PS 6535a7bb (ps_3_0): the mask at s4 feeds a texkill");
        CHECK(analyzePixelShader(psC.data(), psC.size(), a) && a.maskSampler == 1 && !a.maskKill && a.maskAlpha, "walls C PS 7d2cbb8e: the mask at s1 feeds the alpha output (alpha-tested draw)");
        CHECK(analyzePixelShader(psD.data(), psD.size(), a) && a.maskSampler == 3 && a.maskKill, "walls D PS f0d7af09: the mask at s3 feeds a texkill");
        CHECK(analyzePixelShader(psD2.data(), psD2.size(), a) && a.maskSampler == 2 && a.maskKill, "walls D PS 579ba93e: the mask at s2 feeds a texkill");
        CHECK(analyzePixelShader(psObj.data(), psObj.size(), a) && a.maskSampler < 0 && !a.maskKill && !a.maskAlpha, "object PS 0c19795e: no mask test");
      }
    }
  }

  // --- milestone 16: the lot terrain drawn once per world chunk, and the world terrain's blended layer passes
  {
    CHECK(!findNeverCapture(0xdfaf82cf9ec175b0ull) && !findNeverCapture(0x0344bbc366f10954ull) && findNeverCapture(0xc79615c0181b5ef1ull), "never-capture table: the terrain paint passes left it (baked as hidden layer passes, milestone 17); the drop-shadow decals remain");
    const NeverCapture n = { 1, "blended-only", true }, m = { 2, "always", false };
    CHECK(neverCaptureMode(&n) == 2 && neverCaptureMode(&m) == 1 && neverCaptureMode(nullptr) == 0, "neverCaptureMode: 2 for a blended-only entry, 1 otherwise, 0 without an entry");
    CHECK(!neverCaptureDraw(0, TRUE) && neverCaptureDraw(1, FALSE) && neverCaptureDraw(1, TRUE) && !neverCaptureDraw(2, FALSE) && neverCaptureDraw(2, TRUE), "neverCaptureDraw: mode 1 always, mode 2 only with alpha blending on");
    CHECK(kLotTerrainVs == kShaderPatches[0].hash && findShaderPatch(kLotTerrainVs) != nullptr, "the lot terrain's hash names the kill-rectangle patch");
    // the traced frame: lot A (World rows below, buffer 0x500c9e60) drawn for chunks (1152,1152) and (1152,896), lot B (0x500cbd40) likewise
    const float rowsA[12] = { 0.3420206f, 0.f, -0.9396924f, 1171.278f,  0.f, 1.f, 0.f, 45.89392f,  0.9396924f, 0.f, 0.3420206f, 1001.816f };
    const float rowsB[12] = { 0.9396916f, 0.f, 0.3420229f, 1113.957f,  0.f, 1.f, 0.f, 45.88892f,  -0.3420229f, 0.f, 0.9396916f, 1022.679f };
    const uint64_t a1 = lotTerrainKey(0x500c9e60ull, rowsA), a2 = lotTerrainKey(0x500c9e60ull, rowsA), b1 = lotTerrainKey(0x500cbd40ull, rowsB), aOther = lotTerrainKey(0x500cbd40ull, rowsA);
    CHECK(a1 == a2 && a1 != b1 && a1 != aOther && b1 != aOther, "lotTerrainKey: same buffer and rows -> same key; another buffer or other rows -> another key");
    LotCopies c;
    CHECK(!c.seen(a1) && !c.seen(b1) && c.seen(b1) && c.seen(a1) && c.count == 2, "LotCopies: the frame's first draw of each lot mesh is kept, the second copy is a duplicate (the traced frame's order A, B, B, A)");
    c.clear();
    CHECK(c.count == 0 && !c.seen(a1), "LotCopies: a new frame forgets the meshes");
    LotCopies d; bool overflowOk = true;
    for (uint32_t i = 0; i < LotCopies::kMax + 4; ++i) { const float r[12] = { (float) i }; overflowOk = overflowOk && !d.seen(lotTerrainKey(1, r)); }
    CHECK(overflowOk && d.count == LotCopies::kMax, "LotCopies: past %u meshes in a frame further draws are kept, nothing overflows", (unsigned) LotCopies::kMax);
  }

  // --- milestone 17: terrain paint through the runtime's terrain baker
  {
    const TerrainShader* w = findTerrainShader(0xdfaf82cf9ec175b0ull);
    const TerrainShader* p = findTerrainShader(0x0344bbc366f10954ull);
    const TerrainShader* l = findTerrainShader(kLotTerrainVs);
    CHECK(w && !w->layerPass && p && p->layerPass && l && !l->layerPass && findTerrainShader(0x55c99586fb17cd1cull) && !findTerrainShader(0x24ef09fb3303a9d0ull), "kTerrainShaders: world terrain, lot paint composite (layer), lot terrain, lot-area paint; the outer ground is not terrain");
    CHECK(terrainAlphaMode(w, 1, FALSE) == 1 && terrainAlphaMode(l, 1, FALSE) == 1 && terrainAlphaMode(w, 2, TRUE) == 0 && terrainAlphaMode(p, 2, TRUE) == 0 && terrainAlphaMode(nullptr, 1, FALSE) == 1, "terrainAlphaMode: every base draw forces alpha 1 (no shader keeps its coverage alpha, run 77), the world's blended layer passes and the composite leave alpha alone");
    CHECK(terrainAlphaMode(l, 2, FALSE) == 1 && terrainAlphaMode(l, 2, TRUE) == 0 && terrainAlphaMode(w, 2, FALSE) == 0 && terrainAlphaMode(p, 2, FALSE) == 0, "terrainAlphaMode: a lot mesh's opaque chunk copies and replays force alpha 1 too (each covers its own chunk), not the world's or the composite's");
    CHECK(l->lotFamily && p->lotFamily && !w->lotFamily && !findTerrainShader(0x55c99586fb17cd1cull)->lotFamily, "lotFamily: the lot terrain and its paint composite are the lot family (drawn in place, re-submissions hidden); the world terrain and lot-area paint are not");
    CHECK(markKey() == 220, "markKey: the mark key defaults to backslash (virtual key 220), as the shipped sims3hook.txt");
    CHECK(hookOption("noSuchKeyForTheTest", 7) == 7, "hook options: an absent key gives its default (no sims3hook.txt next to the test binary)");
    CHECK(terrainDrawKind(nullptr, TRUE, true) == 0 && terrainDrawKind(w, FALSE, false) == 1 && terrainDrawKind(w, TRUE, false) == 2 && terrainDrawKind(p, FALSE, false) == 2 && terrainDrawKind(l, FALSE, false) == 1 && terrainDrawKind(l, FALSE, true) == 2, "terrainDrawKind: base for opaque draws; layer pass for blended draws, the composite, and a lot mesh's further chunk copies");
    CHECK(wantsUnlitPatch(0x17eabad58f650687ull) && wantsUnlitPatch(0x670dbe0fa52c4650ull) && wantsUnlitPatch(0x98062e8d4d12af7dull) && wantsUnlitPatch(0xd63bf505ec4a44a0ull) && !wantsUnlitPatch(0x99ee53ff6ef1b0b6ull) && !wantsUnlitPatch(0x028ce2dde691b739ull), "unlit patch: the four lit lot-area paint shaders only (the composite's final mad is not albedo x light)");
    static uint32_t m0[kTerrainMarkerSize * kTerrainMarkerSize], m1[kTerrainMarkerSize * kTerrainMarkerSize], m2[kTerrainMarkerSize * kTerrainMarkerSize], m0b[kTerrainMarkerSize * kTerrainMarkerSize];
    terrainMarkerPixels(0, m0); terrainMarkerPixels(1, m1); terrainMarkerPixels(2, m2); terrainMarkerPixels(0, m0b);
    bool flat = true;
    for (uint32_t i = 0; i < kTerrainMarkerSize * kTerrainMarkerSize; ++i) flat = flat && m0[i] == kTerrainMarkerColor[0] && m1[i] == kTerrainMarkerColor[1] && m2[i] == kTerrainMarkerColor[2];
    CHECK(kTerrainMarkers == 3 && memcmp(m0, m0b, sizeof m0) == 0 && memcmp(m0, m1, sizeof m0) != 0 && memcmp(m1, m2, sizeof m1) != 0 && flat && ((m0[0] >> 16) & 0xFFu) == 0x80u && ((m1[0] >> 16) & 0xFFu) == 0x80u && (m2[0] & 0xFFFFFFu) == 0u, "markers: fixed content, distinct, flat; red 0x80 in the terrain (dark red) and layer-pass (purple) markers (read as the detail, doubled = 1), the composite's black");
    CHECK((uint64_t) XXH3_64bits("", 0) == 0x2D06800538D394C2ull, "xxhash: XXH3_64bits of the empty input is the reference value (the runtime's texture hash)");
    const uint64_t h0 = (uint64_t) XXH3_64bits(m0, sizeof m0), h1 = (uint64_t) XXH3_64bits(m1, sizeof m1), h2 = (uint64_t) XXH3_64bits(m2, sizeof m2);
    CHECK(h0 != 0 && h1 != 0 && h2 != 0 && h0 != h1 && h1 != h2, "marker hashes: terrain 0x%016llX, world layer pass 0x%016llX, lot composite 0x%016llX (rtx.terrainTextures = all three, rtx.hideInstanceTextures = the layer pass and the composite, never the terrain marker)", (unsigned long long) h0, (unsigned long long) h1, (unsigned long long) h2);
    CHECK(kLotCompositePs == 0x99ee53ff6ef1b0b6ull && lotCompositeStage(0) == 0 && lotCompositeStage(1) == 4 && lotCompositeStage(2) == 3, "lot composite: pass 1 reads the mask from s4 (layer 4 out), pass 2 from s3 (layer 3 out)");
    std::vector<DWORD> lit, world, layer, comp, lit2, lit3, lit4;
    const bool have = loadShader("ps_17eabad58f650687", lit) && loadShader("ps_028ce2dde691b739", world) && loadShader("ps_3608ab95ab50c8b4", layer) && loadShader("ps_99ee53ff6ef1b0b6", comp)
                   && loadShader("ps_98062e8d4d12af7d", lit2) && loadShader("ps_670dbe0fa52c4650", lit3) && loadShader("ps_d63bf505ec4a44a0", lit4);
    if (!have) SKIP("terrain: pixel shader dumps 17eabad5 / 028ce2dd / 3608ab95 / 99ee53ff / 98062e8d / 670dbe0f / d63bf505 not all found");
    if (have) {
      CHECK(psMaxSampler(lit.data(), lit.size()) == 8 && psMaxSampler(world.data(), world.size()) == 6 && psMaxSampler(layer.data(), layer.size()) == 3 && psMaxSampler(comp.data(), comp.size()) == 4 && psMaxSampler(lit2.data(), lit2.size()) == 9 && psMaxSampler(lit4.data(), lit4.size()) == 5,
            "psMaxSampler: 17eabad5 s8, 028ce2dd s6, 3608ab95 s3, 99ee53ff s4, 98062e8d s9, d63bf505 s5");
      // the detail read (milestone 17k): the last texld of v0.zwzw, whose stage layer 0 moves to
      CHECK(psDetailSampler(lit.data(), lit.size()) == 8 && psDetailSampler(world.data(), world.size()) == 6 && psDetailSampler(layer.data(), layer.size()) == 3 && psDetailSampler(comp.data(), comp.size()) == -1 && psDetailSampler(lit2.data(), lit2.size()) == 9 && psDetailSampler(lit3.data(), lit3.size()) == 7 && psDetailSampler(lit4.data(), lit4.size()) == 4,
            "psDetailSampler: the detail read (texld v0.zwzw) is s8 in 17eabad5, s6 in 028ce2dd, s3 in 3608ab95, s9 in 98062e8d, s7 in 670dbe0f, s4 in d63bf505; the composite 99ee53ff has none (-1)");
      std::vector<DWORD> lot, lot2, world3, layer1;
      if (loadShader("ps_e18ad53a96ff51cc", lot) && loadShader("ps_136392ca90c24a44", lot2) && loadShader("ps_27ac2b7a8987e4d4", world3) && loadShader("ps_c30755d3de24af9a", layer1))
        CHECK(psDetailSampler(lot.data(), lot.size()) == 4 && psDetailSampler(lot2.data(), lot2.size()) == 3 && psDetailSampler(world3.data(), world3.size()) == 4 && psDetailSampler(layer1.data(), layer1.size()) == 2, "psDetailSampler: s4 in e18ad53a (lot), s3 in 136392ca, s4 in 27ac2b7a (3-layer world), s2 in c30755d3");
      else SKIP("psDetailSampler: lot / 3-layer world / 1-layer pass shader dumps not all found");
      auto refs = [](const std::vector<DWORD>& s, uint32_t n) {
        uint32_t c = 0;
        dxsoForEach(s.data(), s.size(), [&](size_t pos, uint32_t op, uint32_t len) {
          if (dxsoIsDef(op)) return true;
          for (size_t i = 1; i <= len; ++i) if ((s[pos + i] & 0x80000000u) && dxsoRegType(s[pos + i]) == kDxsoRegSampler && dxsoRegNum(s[pos + i]) == n) ++c;
          return true;
        });
        return c;
      };
      std::vector<DWORD> t = lit;
      const uint32_t changed = psSwapSamplers(t, 0, 9);
      CHECK(changed == 2 && refs(lit, 0) == 2 && refs(t, 0) == 0 && refs(t, 9) == 2 && t.size() == lit.size(), "psSwapSamplers 17eabad5 s0<->s9: the cube's declaration and its one read now name s9, nothing names s0 (%u tokens changed)", changed);
      std::vector<DWORD> t2 = world;
      CHECK(psSwapSamplers(t2, 0, 7) == 2 && refs(t2, 7) == 2 && refs(t2, 0) == 0, "psSwapSamplers 028ce2dd s0<->s7: layer 0's declaration and read");
      std::vector<DWORD> t3 = world;
      CHECK(psSwapSamplers(t3, 0, 6) == 4 && refs(t3, 6) == 2 && refs(t3, 0) == 2 && psDetailSampler(t3.data(), t3.size()) == 0 && t3.size() == world.size(), "psSwapSamplers 028ce2dd s0<->s6 (its detail stage, the M17k swap): 4 tokens renumbered, the detail read now names s0 and layer 0's declaration and read s6");
      std::vector<DWORD> t4 = lit;
      CHECK(psSwapSamplers(t4, 0, 8) == 4 && refs(t4, 8) == 2 && refs(t4, 0) == 2 && psDetailSampler(t4.data(), t4.size()) == 0, "psSwapSamplers 17eabad5 s0<->s8 (its detail stage): the cube's declaration and read now name s8, the detail read s0");
      std::vector<DWORD> compA = comp, compB = comp;
      CHECK(psSwapSamplers(compA, 0, 4) == 4 && refs(compA, 4) == 2 && refs(compA, 0) == 2 && psSwapSamplers(compB, 0, 3) == 4 && refs(compB, 3) == 2 && refs(compB, 0) == 2 && psDetailSampler(compA.data(), compA.size()) == -1, "psSwapSamplers 99ee53ff (the composite) s0<->s4 and s0<->s3: the mask's declaration and read and one layer's swap, 4 tokens each");
      auto lastColourWrite = [](const std::vector<DWORD>& s, uint32_t& op, DWORD& src0) {
        op = 0; src0 = 0;
        dxsoForEach(s.data(), s.size(), [&](size_t pos, uint32_t o, uint32_t len) {
          if (!dxsoIsDef(o) && o != kDxsoOpDcl && len >= 2 && (s[pos + 1] & 0x80000000u) && dxsoRegType(s[pos + 1]) == kDxsoRegColorOut && dxsoRegNum(s[pos + 1]) == 0 && ((s[pos + 1] >> 16) & 7u) == 7u) { op = o; src0 = s[pos + 2]; }
          return true;
        });
      };
      for (int i = 0; i < 4; ++i) {
        const std::vector<DWORD>& src = i == 0 ? lit : i == 1 ? lit2 : i == 2 ? lit3 : lit4;
        const char* name = i == 0 ? "17eabad5" : i == 1 ? "98062e8d" : i == 2 ? "670dbe0f" : "d63bf505";
        std::vector<DWORD> u = src; const size_t before = u.size();
        uint32_t op = 0; DWORD s0 = 0;
        const bool ok = psUnlitOutput(u);
        lastColourWrite(u, op, s0);
        CHECK(ok && u.size() == before - 2 && op == kDxsoOpMov && dxsoRegType(s0) == kDxsoRegTemp && dxsoRegNum(s0) == 0 && u.back() == kDxsoEnd && shaderTokenCount(u.data(), u.size()) == u.size(),
              "psUnlitOutput %s: the final mad oC0.xyz becomes mov oC0.xyz, r0 (albedo x detail); 2 tokens fewer, END intact", name);
      }
      // alpha forced to 1 for base draws: a DEF of a fresh constant and a final mov oC0.w
      for (int i = 0; i < 3; ++i) {
        const std::vector<DWORD>& src = i == 0 ? lit : i == 1 ? world : comp;
        const char* name = i == 0 ? "17eabad5" : i == 1 ? "028ce2dd" : "99ee53ff";
        const int expectReg = i == 0 ? 16 : i == 1 ? 1 : 1;
        std::vector<DWORD> a = src; const size_t before = a.size();
        const bool ok = psForceAlphaOne(a);
        // the last instruction before END
        size_t lastPos = 0; uint32_t lastOp = 0, lastLen = 0; bool defSeen = false;
        dxsoForEach(a.data(), a.size(), [&](size_t pos, uint32_t op, uint32_t len) { if (op == 0x51u && dxsoRegNum(a[pos + 1]) == (uint32_t) expectReg && a[pos + 2] == 0x3F800000u) defSeen = true; lastPos = pos; lastOp = op; lastLen = len; return true; });
        const bool movOk = lastOp == kDxsoOpMov && lastLen == 2 && dxsoRegType(a[lastPos + 1]) == kDxsoRegColorOut && ((a[lastPos + 1] >> 16) & 0xFu) == 0x8u && dxsoRegType(a[lastPos + 2]) == 2u && dxsoRegNum(a[lastPos + 2]) == (uint32_t) expectReg;
        CHECK(ok && a.size() == before + 9 && defSeen && movOk && a.back() == kDxsoEnd && shaderTokenCount(a.data(), a.size()) == a.size(), "psForceAlphaOne %s: def c%d = 1 after the declarations, `mov oC0.w, c%d.x` before END (9 tokens more)", name, expectReg, expectReg);
      }
      std::vector<DWORD> w2 = world, c2 = comp;
      uint32_t opW = 0; DWORD sW = 0; lastColourWrite(world, opW, sW);
      CHECK(!psUnlitOutput(w2) && w2.size() == world.size() && opW == 5u /*MUL*/, "psUnlitOutput 028ce2dd: the world terrain's final mul is left alone (not a mad)");
      CHECK(psUnlitOutput(c2) && !wantsUnlitPatch(0x99ee53ff6ef1b0b6ull), "psUnlitOutput would rewrite the composite's final mad too, which is why the patch is applied by hash only");
    }
  }

  printf("%d failure(s), %d skipped\n", fails, skips);
  return fails;
}

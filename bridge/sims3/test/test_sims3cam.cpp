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
#include "sims3_lots.h"
#include "sims3_vsinterp.h"
#define XXH_INLINE_ALL
#include "xxhash.h"   // the runtime's texture hash (milestone 17): the marker hashes the hook sends
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

using namespace sims3cam;
static int fails = 0, skips = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  PASS  "); } else { printf("  FAIL  "); ++fails; } printf(__VA_ARGS__); printf("\n"); } while (0)
#define SKIP(...) do { printf("  SKIP  "); ++skips; printf(__VA_ARGS__); printf("\n"); } while (0)
// A device for the hook's own calls (milestone 152): the states as plain arrays, every set counted.
struct FakeDev {
  DWORD rs[256] = {}, ss[16][16] = {}, tss[8][40] = {}; D3DVIEWPORT9 vp = {}; D3DMATRIX world = {}; uint32_t sets = 0;
  HRESULT GetRenderState(D3DRENDERSTATETYPE s, DWORD* v) { *v = rs[s]; return S_OK; }
  HRESULT SetRenderState(D3DRENDERSTATETYPE s, DWORD v) { rs[s] = v; ++sets; return S_OK; }
  HRESULT GetSamplerState(DWORD st, D3DSAMPLERSTATETYPE t, DWORD* v) { *v = ss[st][t]; return S_OK; }
  HRESULT SetSamplerState(DWORD st, D3DSAMPLERSTATETYPE t, DWORD v) { ss[st][t] = v; ++sets; return S_OK; }
  HRESULT GetTextureStageState(DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD* v) { *v = tss[st][t]; return S_OK; }
  HRESULT SetTextureStageState(DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v) { tss[st][t] = v; ++sets; return S_OK; }
  HRESULT GetTexture(DWORD, IDirect3DBaseTexture9** t) { *t = nullptr; return S_OK; }
  HRESULT SetTexture(DWORD, IDirect3DBaseTexture9*) { ++sets; return S_OK; }
  HRESULT GetPixelShader(IDirect3DPixelShader9** p) { *p = nullptr; return S_OK; }
  HRESULT SetPixelShader(IDirect3DPixelShader9*) { ++sets; return S_OK; }
  HRESULT GetVertexShader(IDirect3DVertexShader9** p) { *p = nullptr; return S_OK; }
  HRESULT SetVertexShader(IDirect3DVertexShader9*) { ++sets; return S_OK; }
  HRESULT GetViewport(D3DVIEWPORT9* v) { *v = vp; return S_OK; }
  HRESULT SetViewport(const D3DVIEWPORT9* v) { vp = *v; ++sets; return S_OK; }
  HRESULT GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX* m) { *m = world; return S_OK; }
  HRESULT SetTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX* m) { world = *m; ++sets; return S_OK; }
};

static const char* kindName(Kind k) { return k == Kind::Main ? "Main" : k == Kind::Reflection ? "Reflection" : "None"; }

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
static void rowTimes(const float v[4], const D3DMATRIX& M, float out[4]) {
  for (int c = 0; c < 4; ++c) out[c] = v[0] * M.m[0][c] + v[1] * M.m[1][c] + v[2] * M.m[2][c] + v[3] * M.m[3][c];
}

int main() {
  printf("sims3 hook standalone test (sims3_camera_hook.h + sims3_walls.h)\n");
  CHECK(enabled(), "hook enabled by default (SIMS3_CAMERA_HOOK unset)");

  // --- captured reflection-pass draws: verified cameras, looking up -> Reflection
  Camera A = {}, B = {};
  const Kind kA = classify(k1277907, 12, A);
  CHECK(kA == Kind::Reflection, "call 1277907 (WVP + World at c4, eye at c8): %s (reflection camera, fwd.y=%.2f)", kindName(kA), A.fwd[1]);
  const Kind kB = classify(k1278332, 12, B);
  CHECK(kB == Kind::Reflection, "call 1278332 (identity World, eye at c7): %s", kindName(kB));
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
  {
    // milestone 122: the fused matrix elsewhere in the block (run 225: the walls' VS 84b06922 takes it at c4..c7, its World
    // at c8..c10, the eye after them; c0..c3 hold something else)
    const float other[16] = { 0.5f,0,0,0.5f,  0,-0.5f,0,0.5f,  0,0,1,0,  0,0,0,1 };
    float walls[16*4] = {};
    for (int i = 0; i < 16; ++i) walls[i] = other[i];
    toBlock(WVP, walls + 16);                                                                 // c4..c7
    for (int i = 0; i < 12; ++i) walls[32+i] = Wrows[i];                                      // c8..c10
    walls[48] = 1115.16f; walls[49] = 66.70f; walls[50] = 1020.54f; walls[51] = 1.f;           // eye at c12
    Camera Cw = {}; const Kind kw = classify(walls, 16, Cw);
    CHECK(kw == Kind::Main && near3(Cw.pos, 1115.16f, 66.70f, 1020.54f), "the walls' layout (fused matrix at c4, World c8..c10, eye at c12): %s at the true eye", kindName(kw));
    float noEye[16*4]; for (int i = 0; i < 64; ++i) noEye[i] = walls[i];
    noEye[51] = 0.f;
    Camera Cn = {}; const Kind kn = classify(noEye, 16, Cn);
    CHECK(kn == Kind::None, "the walls' layout without the eye: %s", kindName(kn));
    float noWorld[16*4] = {}; for (int i = 0; i < 32; ++i) noWorld[i] = walls[i];
    noWorld[48] = 1115.16f; noWorld[49] = 66.70f; noWorld[50] = 1020.54f; noWorld[51] = 1.f;
    Camera Co = {}; const Kind ko = classify(noWorld, 16, Co);
    CHECK(ko == Kind::None, "an object-space fused matrix at c4 without its World: %s (not forwarded, as at c0)", kindName(ko));
    // the water's reflection camera in the walls' layout: a reflection, so the pass's draws are dropped
    M4 WVPr; mul(VPr, W, WVPr);
    float wallsR[16*4]; for (int i = 0; i < 64; ++i) wallsR[i] = walls[i];
    toBlock(WVPr, wallsR + 16);
    wallsR[48] = 1115.16f; wallsR[49] = -10.70f; wallsR[50] = 1020.54f;
    Camera Cr = {}; const Kind kr = classify(wallsR, 16, Cr);
    CHECK(kr == Kind::Reflection, "the water's reflection camera in the walls' layout: %s", kindName(kr));
    // the Sims' block at c180: the fused matrix eight registers in (c188), World after it, the eye at c200
    float sims[24*4] = {};
    for (int i = 0; i < 16; ++i) sims[i] = other[i];
    toBlock(WVP, sims + 32);
    for (int i = 0; i < 12; ++i) sims[48+i] = Wrows[i];
    sims[80] = 1115.16f; sims[81] = 66.70f; sims[82] = 1020.54f; sims[83] = 1.f;
    Camera Cs = {}; const Kind ks = classify(sims, 24, Cs);
    CHECK(ks == Kind::Main && near3(Cs.pos, 1115.16f, 66.70f, 1020.54f), "a block with the fused matrix eight registers in, the eye twelve further: %s", kindName(ks));
    CHECK(!Cw.continued && !Cs.continued, "  both verified by their eye, not continued");
  }
  if (kM == Kind::Main) {
    // milestone 123: the lot terrain's block (VS 976b73db): fused matrix c0..c3, World c4..c6, c7..c10 its own, no eye
    float lot[11*4] = {};
    toBlock(WVP, lot);
    for (int i = 0; i < 12; ++i) lot[16+i] = Wrows[i];
    const float own[16] = { 0,0,0.5f,0.5f,  0.01f,0.01f,0,0,  0.02f,0.02f,0,0,  1100,0,1000,1 };
    for (int i = 0; i < 16; ++i) lot[28+i] = own[i];
    Camera L0 = {}; const Kind kl0 = classify(lot, 11, L0);
    CHECK(kl0 == Kind::None, "the lot terrain's block without its eye, no reference: %s", kindName(kl0));
    Camera L1 = {}; const Kind kl1 = classify(lot, 11, L1, &Mn);
    CHECK(kl1 == Kind::Main && L1.continued && near3(L1.pos, 1115.16f, 66.70f, 1020.54f), "  continuing the last camera verified by its eye: %s (continued %d) at the true eye", kindName(kl1), (int) L1.continued);
    Camera away = Mn; away.pos[0] += 60.f;
    Camera L2 = {}; const Kind kl2 = classify(lot, 11, L2, &away);
    CHECK(kl2 == Kind::None, "  the reference 60 units away: %s", kindName(kl2));
    Camera wide = Mn; wide.fovY += 0.01f;
    Camera L3 = {}; const Kind kl3 = classify(lot, 11, L3, &wide);
    CHECK(kl3 == Kind::None, "  another lens: %s", kindName(kl3));
    // the water's reflection camera in the same block: mirrored, never the play camera continued
    float lotR[11*4]; for (int i = 0; i < 44; ++i) lotR[i] = lot[i];
    M4 WVPr2; mul(VPr, W, WVPr2); toBlock(WVPr2, lotR);
    Camera L4 = {}; const Kind kl4 = classify(lotR, 11, L4, &Mn);
    CHECK(kl4 == Kind::None, "  the water's reflection camera without its eye: %s", kindName(kl4));
    // run 70's sky-dome phantom: the play camera's lens, the view's translation stripped, a rotation as its World
    float dome[11*4] = {};
    M4 VPo = VP; for (int r = 0; r < 4; ++r) VPo.m[r][3] = 0.f;
    M4 WVPo; mul(VPo, W, WVPo); toBlock(WVPo, dome);
    for (int i = 0; i < 12; ++i) dome[16+i] = Wrows[i];
    for (int i = 0; i < 3; ++i) dome[16 + i*4 + 3] = 0.f;                                      // a rotation only
    Camera L5 = {}; const Kind kl5 = classify(dome, 11, L5, &Mn);
    CHECK(kl5 == Kind::None, "  the sky dome's camera at the world origin: %s", kindName(kl5));
    // an object-space fused matrix with no World after it is never continued
    float objs[11*4] = {}; toBlock(WVP, objs);
    Camera L6 = {}; const Kind kl6 = classify(objs, 11, L6, &Mn);
    CHECK(kl6 == Kind::None, "  an object-space fused matrix without its World: %s", kindName(kl6));
  }

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

  // --- the game's light through its own pointers (milestone 127): at least two chains must agree
  {
    int v = 0;
    const uintptr_t all[5] = { 0x1DDA6C00, 0x1DDA6C00, 0x1DDA6C00, 0x1DDA6C00, 0x1DDA6C00 };
    CHECK(lightChainVote(all, 5, v) == 0x1DDA6C00 && v == 5, "light chains: all five agree -> the record (%d votes)", v);
    const uintptr_t two[5] = { 0, 0x30D3BF20, 0, 0x30D3BF20, 0 };
    CHECK(lightChainVote(two, 5, v) == 0x30D3BF20 && v == 2, "light chains: two agree, three unreadable -> the record");
    const uintptr_t one[5] = { 0, 0x30D3BF20, 0, 0, 0 };
    CHECK(lightChainVote(one, 5, v) == 0 && v == 0, "light chains: a single chain is not trusted");
    const uintptr_t split[5] = { 0x1000, 0x2000, 0, 0, 0 };
    CHECK(lightChainVote(split, 5, v) == 0, "light chains: two chains to different places -> none");
    const uintptr_t most[5] = { 0x1000, 0x2000, 0x2000, 0x1000, 0x2000 };
    CHECK(lightChainVote(most, 5, v) == 0x2000 && v == 3, "light chains: three against two -> the three");
    CHECK(kLightChainCount == 3, "light chains: one chain per path, three (TS3.exe build 6707155c)");
  }

  // --- the game's own fakes (milestone 130): dropped by pixel shader
  {
    auto fake = [](uint64_t ps) { const DropPs* d = findDropPs(ps); return d && d->kind == kGameFake; };
    auto copy = [](uint64_t ps) { const DropPs* d = findDropPs(ps); return d && d->kind == kBlendedCopy; };
    CHECK(fake(0xbac911e069b2ee21ull) && fake(0x353ed3fb56cf16f1ull) && fake(0x9b8f4e2b9fbb9bb1ull) && fake(0xd33629855723d2bcull) && fake(0xff810e83c3f21f0bull) && fake(0x11227d6d7bba5802ull)
          && fake(0x6cc68b13e65b5a73ull) && fake(0x92a300ed9b662005ull),
          "drop table: the game's fakes -- the tone curve, the glow's composite, the town ground's light, the fog over the lot, the drop-shadow decals, the Sims' darkening overlay, the Sims' shadow blobs");
    CHECK(copy(0x57a5a049ffa47770ull) && copy(0x00e85de9a42890fbull) && copy(0x45c7a7cd511b5233ull) && copy(0x7304aaea6a75fb3full) && copy(0x7865aa449e279d1dull),
          "drop table (M148): the blended copies -- the Sims' hair indoors and out, the effect cards by their two pixel shaders");
    CHECK(!findDropPs(0x5aee1186d554dbc4ull) && !findDropPs(0x7ba9f578d1a1ebb2ull) && !findDropPs(0x52d5e6267a915d34ull) && !findDropPs(0xe8daded2c199a6e2ull) && !findDropPs(0),
          "drop table: not the objects' shader, the interface, the cursor's depth pick, the grass sprites (sent since milestone 138)");
    const DropPs* f = findDropPs(0xbac911e069b2ee21ull); const DropPs* hair = findDropPs(0x57a5a049ffa47770ull); const DropPs* pic = findDropPs(0x4c59eb620412df33ull);
    CHECK(pic && pic->kind == kLeftOut && !findDropPs(0x92337a1805f17506ull),
          "drop table (M149): the neighbourhood view's lot picture by its pixel shader 4c59eb62, not its vertex shader 92337a18");
    CHECK(dropsDraw(f, false, FALSE) && dropsDraw(f, true, TRUE) && dropsDraw(hair, true, TRUE) && !dropsDraw(hair, true, FALSE) && !dropsDraw(hair, false, TRUE) && !dropsDraw(nullptr, true, TRUE)
          && dropsDraw(pic, true, FALSE) && dropsDraw(pic, true, TRUE) && !dropsDraw(pic, false, FALSE),
          "drop table: a fake goes in every draw; a surface left out in every world draw; a blended copy only as a blended world draw (its opaque pass captured, the menu portrait's rasterized)");
  }

  // --- the passes Remix never shows (milestone 124), from the in-world trace's targets
  CHECK(unshownPass(false, 2048, 2048, 0, false, false) == kShadowMapPass, "a 2048x2048 offscreen target written with colour writes off: the shadow map");
  CHECK(unshownPass(true, 1920, 1080, 0, false, false) == -1, "the screen with colour writes off (a stencil or depth pass of the main view): kept");
  CHECK(unshownPass(false, 2048, 2048, 0xF, false, false) == -1, "a square offscreen target written with colour: not the shadow map");
  CHECK(unshownPass(false, 128, 128, 0, false, false) == -1 && unshownPass(false, 128, 128, 1, false, false) == -1, "the 128x128 target (colour writes 1 in the trace): kept");
  CHECK(unshownPass(false, 1024, 512, 0, false, false) == -1, "a depth-only pass into a non-square offscreen target: kept");
  CHECK(unshownPass(false, 64, 64, 0xF, true, false) == kSkyCubePass, "a cube map's face: the sky's environment cube");
  CHECK(unshownPass(false, 512, 512, 0xF, false, true) == kWaterReflectionPass && unshownPass(false, 512, 512, 7, false, true) == kWaterReflectionPass, "the target a reflection camera drew into: the water's reflection, whatever its draws write");
  CHECK(unshownPass(true, 1920, 1080, 0xF, true, true) == -1, "nothing on the screen is ever an unshown pass");

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
  CHECK(drawIs3D(true, true, D3DZB_TRUE, true, true),   "3D decl + depth on + camera + primary RT -> main camera");
  CHECK(!drawIs3D(true, true, D3DZB_FALSE, true, true), "3D decl + depth OFF                      -> identity (post/UI window)");
  CHECK(!drawIs3D(true, false, D3DZB_TRUE, true, true), "2D decl                                  -> identity");
  CHECK(!drawIs3D(false, true, D3DZB_TRUE, true, true), "no verified camera yet                   -> identity");
  CHECK(!drawIs3D(true, true, D3DZB_TRUE, false, true), "non-primary render target (shadow/reflection pass) -> identity");
  CHECK(!drawIs3D(true, true, D3DZB_TRUE, true, false), "a viewport over part of the screen (the action menu's portrait, M133) -> as the game draws it");

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
    {
      const uint8_t l8[4] = { 0, 128, 255, 7 }; const uint8_t p565[2] = { 0x1F, 0xF8 }; const uint8_t p4444[2] = { 0x0F, 0xF0 };
      std::vector<uint32_t> o;
      CHECK(decodeColour((uint32_t) D3DFMT_L8, l8, sizeof l8, 2, 2, o) && o[1] == 0xFF808080u && o[2] == 0xFFFFFFFFu
            && decodeColour((uint32_t) D3DFMT_R5G6B5, p565, 2, 1, 1, o) && o[0] == 0xFFFF00FFu && decodeColour((uint32_t) D3DFMT_A4R4G4B4, p4444, 2, 1, 1, o) && o[0] == 0xFF0000FFu
            && !decodeColour((uint32_t) D3DFMT_DXT1, blk, 4, 4, 4, o) && !decodeColour((uint32_t) D3DFMT_V8U8, l8, sizeof l8, 1, 1, o) && level0Bytes(D3DFMT_DXT5, 6, 6) == 64,
            "one decoder (M154): L8 as grey, R5G6B5 and A4R4G4B4 in full colour; short data and other formats refused; DXT blocks rounded up");
    }
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
    // the hook's call (sims3BeginDraw): texkill keeps a * alpha + b >= 0, times 255
    auto cut = [](float a, float b, uint32_t& r) { return alphaTestFor(a, 255.f * b, D3DCMP_GREATEREQUAL, r); };
    auto cutNone = [&](float a, float b) { uint32_t r = 0; const uint32_t f = cut(a, b, r); return alphaTestPassesAll(f, r); };
    CHECK(cut(1.f, -0.5f, ref) == D3DCMP_GREATEREQUAL && ref == 128, "cut: alpha - 0.5 keeps alpha >= 128/255 (ref %u)", ref);
    CHECK(cut(255.f, -128.f, ref) == D3DCMP_GREATEREQUAL && ref == 128, "cut: alpha * 255 - 128 keeps alpha >= 128 (ref %u)", ref);
    CHECK(cutNone(255.f, 0.f), "cut: alpha * 255 - 0 discards nothing: no test");
    CHECK(cutNone(1.f, 0.5f), "cut: alpha + 0.5 discards nothing");
    CHECK(cut(1.f, -1.5f, ref) == D3DCMP_NEVER, "cut: alpha - 1.5 discards everything");
    CHECK(cut(-1.f, 0.5f, ref) == D3DCMP_LESSEQUAL && ref == 127 && !cutNone(-1.f, 0.5f) && cutNone(-1.f, 1.f), "cut: 0.5 - alpha keeps alpha <= 127/255 (ref %u); 1 - alpha discards nothing", ref);
    CHECK(cut(0.f, -1.f, ref) == D3DCMP_NEVER && cutNone(0.f, 1.f), "cut: a constant value discards all or nothing");
    CHECK(alphaTestFor(1.f, -10.f, D3DCMP_GREATER, ref) == D3DCMP_GREATEREQUAL && ref == 11 && alphaTestFor(1.f, -10.f, D3DCMP_LESS, ref) == D3DCMP_LESSEQUAL && ref == 9
          && alphaTestFor(1.f, -10.f, D3DCMP_EQUAL, ref) == 0u && alphaTestFor(0.f, 0.f, D3DCMP_GREATER, ref) == D3DCMP_NEVER,
          "alphaTestFor (M151): x - 10 > 0 keeps x >= 11, < 0 keeps x <= 9; EQUAL not turned; a constant 0 > 0 keeps nothing");
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
      CHECK(cut(cutEval(a.cutA, get), cutEval(a.cutB, get), r2) == D3DCMP_GREATEREQUAL && r2 == 96, "  with c7.x = 96: alpha test >= 96 (ref %u)", r2);
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
    // the hedges' PS 783b8225: its cut-out, texkill on s2's alpha
    std::vector<DWORD> hedge;
    if (loadShader("ps_783b82250ef7c14d", hedge)) {
      PsAnalysis a;
      const bool ok = analyzePixelShader(hedge.data(), hedge.size(), a);
      auto get = [](uint32_t reg, uint32_t comp) -> float { return (reg == 2 && comp == 3) ? 1.f : (reg == 9 && comp == 0) ? 128.f : 0.f; };
      uint32_t r = 0;
      const uint32_t f = ok ? cut(cutEval(a.cutA, get), cutEval(a.cutB, get), r) : 0u;
      CHECK(ok && a.cutSampler == 2 && f == D3DCMP_GREATEREQUAL && r == 128,
            "cut: the hedges' PS 783b8225 -> 255 * c2.w * alpha(s2) - c9.x (c2.w 1, c9.x 128: alpha test >= %u)", r);
    } else SKIP("ps_783b82250ef7c14d dump not found");
  }

  // --- the trees' fade (milestone 135): "fade - alpha" under the game's LESS 1, turned onto the alpha
  {
    // one plant: the game's test on "fade - alpha" turned by groupFades (alphaTestFor)
    auto one = [](uint32_t func, uint32_t gref, float f, uint32_t& r) { const FadeGroups g = groupFades(func, gref, &f, 1); r = g.count ? g.ref[0] : 0u; return g.func; };
    uint32_t ref = 0;
    CHECK(one(D3DCMP_LESS, 1, 0.3313726f, ref) == D3DCMP_GREATEREQUAL && ref == 84, "fade: a whole tree (0.3313726 - alpha < 1/255) keeps alpha >= 84 (ref %u)", ref);
    CHECK(one(D3DCMP_LESS, 1, 1.f, ref) == D3DCMP_GREATEREQUAL && ref == 255, "fade: faded out (1 - alpha < 1/255) keeps alpha 255 only (ref %u)", ref);
    CHECK(one(D3DCMP_LESS, 1, 0.f, ref) == D3DCMP_GREATEREQUAL && ref == 0, "fade: fade 0 keeps every texel (ref %u)", ref);
    CHECK(one(D3DCMP_LESSEQUAL, 0, 0.5f, ref) == D3DCMP_GREATEREQUAL && ref == 128, "fade: 0.5 - alpha <= 0 keeps alpha >= 128 (ref %u)", ref);
    CHECK(one(D3DCMP_GREATER, 0, 0.5f, ref) == D3DCMP_LESSEQUAL && ref == 127, "fade: 0.5 - alpha > 0 keeps alpha <= 127 (ref %u)", ref);
    CHECK(one(D3DCMP_LESS, 0, 0.f, ref) == D3DCMP_GREATEREQUAL && ref == 1, "fade: 0 - alpha < 0 keeps alpha above 0, >= 1 (ref %u)", ref);
    CHECK(one(D3DCMP_EQUAL, 1, 0.33f, ref) == 0u && one(D3DCMP_ALWAYS, 1, 0.33f, ref) == 0u, "fade: other comparisons left as the game's");
    // milestone 139: each plant its own cut -- traced batches (native2 trace) grouped
    {
      const float whole[4] = { 0.3313726f, 0.3313726f, 0.3313726f, 0.3313726f }, mixed[3] = { 0.3432f, 0.4654f, 0.4725f }, pair[3] = { 0.6031f, 0.4204f, 0.4204f }, gone[2] = { 0.3313726f, 1.2f }, allGone[2] = { 1.2f, 1.5f };
      const FadeGroups a = groupFades(D3DCMP_LESS, 1, whole, 4), b = groupFades(D3DCMP_LESS, 1, mixed, 3), c = groupFades(D3DCMP_LESS, 1, pair, 3);
      const FadeGroups d = groupFades(D3DCMP_LESS, 1, gone, 2), e = groupFades(D3DCMP_LESS, 1, allGone, 2), f = groupFades(D3DCMP_EQUAL, 1, whole, 4);
      CHECK(a.func == D3DCMP_GREATEREQUAL && a.count == 1 && a.ref[0] == 84 && a.members[0] == 0x0F && !a.split(), "plants: four whole plants are one group, >= 84, one draw");
      CHECK(b.count == 3 && b.ref[0] == 87 && b.ref[1] == 118 && b.ref[2] == 120 && b.split(), "plants: fades 0.3432 / 0.4654 / 0.4725 are three cuts (>= %u, %u, %u)", b.ref[0], b.ref[1], b.ref[2]);
      CHECK(c.count == 2 && c.members[0] == 0x01 && c.members[1] == 0x06 && c.split(), "plants: 0.6031 / 0.4204 / 0.4204 are two groups, the last two together");
      CHECK(d.count == 1 && d.out == 0x02 && d.members[0] == 0x01 && d.split(), "plants: a plant faded out entirely belongs to no group, and the draw is split to leave it out");
      CHECK(e.func == D3DCMP_NEVER && e.count == 0 && !e.split(), "plants: every plant faded out -> never drawn");
      CHECK(f.func == 0u && f.count == 0, "plants: a test that cannot be turned groups nothing");
    }
    std::vector<DWORD> leafPs, branchPs, leafVs, branchVs, lodPs, objPs;
    if (loadShader("ps_7e48acce64547cd0", leafPs) && loadShader("ps_2746661ff9d95c1d", branchPs) && loadShader("vs_799a26fa907d36d7", leafVs) && loadShader("vs_854fd850257ee36f", branchVs)) {
      PsAnalysis a, b;
      CHECK(analyzePixelShader(leafPs.data(), leafPs.size(), a) && a.fadeSampler == 1 && a.fadeInput == 1 * 4 + 3 && a.cutSampler == -1,
            "fade: leaves PS 7e48acce -> alpha = TEXCOORD1.w - alpha(s1) (sampler %d, input %d)", a.fadeSampler, a.fadeInput);
      CHECK(analyzePixelShader(branchPs.data(), branchPs.size(), b) && b.fadeSampler == 1 && b.fadeInput == kSemColor0 * 4 + 3 && b.cutSampler == -1,
            "fade: branches PS 2746661f -> alpha = COLOR0.w - alpha(s1) (sampler %d, input %d)", b.fadeSampler, b.fadeInput);
      VsConstantOutputs lv, bv;
      CHECK(analyzeVertexConstantOutputs(leafVs.data(), leafVs.size(), lv) && lv.c[1 * 4 + 3] == 2 * 4 + 1 && lv.rel[1 * 4 + 3] && lv.c[1 * 4 + 2] == -1,
            "fade: leaves VS 799a26fa hands c2[a0].y (the first instance's fade, read relatively) over in TEXCOORD1.w (c%d), its z computed", lv.c[7]);
      CHECK(analyzeVertexConstantOutputs(branchVs.data(), branchVs.size(), bv) && bv.c[kSemColor0 * 4 + 3] == 2 * 4 + 1,
            "fade: branches VS 854fd850 hands c2.y over in COLOR0.w (c%d)", bv.c[kSemColor0 * 4 + 3]);
    } else SKIP("SpeedTree shader dumps (ps_7e48acce / ps_2746661f / vs_799a26fa / vs_854fd850) not found");
    if (loadShader("ps_714d5dae31da4378", lodPs)) {
      PsAnalysis a;
      CHECK(analyzePixelShader(lodPs.data(), lodPs.size(), a) && a.fadeSampler == -1 && a.cutSampler == -1, "fade: the LOD fade's blended PS 714d5dae (alpha = TEXCOORD3.x, texkill on alpha + an interpolant) has neither");
    } else SKIP("ps_714d5dae31da4378 dump not found");
    if (loadShader("ps_00230c49e1b880b6", objPs)) {
      PsAnalysis a;
      CHECK(analyzePixelShader(objPs.data(), objPs.size(), a) && a.fadeSampler == -1 && a.cutSampler == 6, "fade: object PS 00230c49 keeps its cut-out and has no fade");
    } else SKIP("ps_00230c49e1b880b6 dump not found");
  }

  // --- leaf cards fixed in the world (milestone 136): the leaf shaders run for a card's four corners
  // under two cameras; the game's turn with the camera, the variant's stay put, flat, facing out from
  // the tree's origin, level, the card's size kept
  {
    // the camera of a traced frame (c117-c120 its view-projection, c121-c123 the leaves' axes) and a second one
    const float vp[16] = { -1.70502f, -3.597459e-08f, 1.709191f, 0.f, 2.875988f, 1.385067f, 2.868971f, 0.f,
                           0.2284907f, -0.9465755f, 0.2279332f, -0.2500208f, 0.2284716f, -0.9464967f, 0.2279142f, 0.f };
    const float camA[12] = { -0.7062426f, 0.6700911f, -0.2284716f, 1117.602f, 0.f, 0.3227137f, 0.9464967f, 75.21104f, 0.7079699f, 0.6684562f, -0.2279142f, 1022.205f };
    const float camB[12] = { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f };
    Camera traced = {};
    CHECK(decompose(fromRows(vp), traced) && cardBasisIsCamera(camA, traced) && !cardBasisIsCamera(camB, traced),
          "cards: the traced leaves' c121-c123 are the traced camera's axes; another basis is not");
    struct Leaf { const char* name; uint16_t basis; };
    const Leaf leaves[3] = { { "vs_799a26fa907d36d7", 121 }, { "vs_e0c97675a334022e", 134 }, { "vs_1b5224b7e9bee689", 121 } };
    for (const Leaf& L : leaves) {
      std::vector<DWORD> vs;
      if (!loadShader(L.name, vs)) { SKIP("%s dump not found", L.name); continue; }
      CameraCard cc;
      CHECK(analyzeCameraCard(vs.data(), vs.size(), cc) && cc.basisReg == L.basis, "cards: %s turns its leaves to the camera with c%u-c%u (found c%u)", L.name, (unsigned) L.basis, (unsigned) L.basis + 2u, (unsigned) cc.basisReg);
      std::vector<DWORD> out = vs;
      const bool made = makeOutwardCards(out);
      static float bank[256][4];
      // one corner of a leaf centred at (2, 5, 1) of a tree at (10, 0, 20), unturned, unscaled, no wind, no rocking
      auto run = [&](const std::vector<DWORD>& t, const float* cam, int corner, float p[4]) -> bool {
        memset(bank, 0, sizeof bank);
        const float c0[4] = { 10.f, 0.f, 20.f, 1.f }, c1[4] = { 1.f, 0.f, 0.f, 0.f }, c2[4] = { 1.f, 0.33f, 1.f, 1.f }, rock[4] = { 1.f, 0.f, 1.f, 0.f };
        memcpy(bank[0], c0, 16); memcpy(bank[1], c1, 16); memcpy(bank[2], c2, 16);
        for (int i = 0; i < 3; ++i) bank[81 + i][i] = 1.f;
        memcpy(bank[105], rock, 16); memcpy(bank[114], rock, 16);
        for (int i = 0; i < 4; ++i) { bank[117 + i][i] = 1.f; bank[130 + i][i] = 1.f; }
        memcpy(bank[L.basis], cam, 48);
        float in[16][4] = {};
        const float v0[4] = { 2.f, 5.f, 1.f, 1.f }, v1[4] = { 127.f, 127.f, 255.f, 127.f }, v3[4] = { 1.f, 1.f, 1.f, 1.f }, v4[4] = { 0.f, 0.f, 0.f, 1.f };
        memcpy(in[0], v0, 16); memcpy(in[1], v1, 16); memcpy(in[3], v3, 16); memcpy(in[4], v4, 16);
        in[5][2] = (float) corner;
        VsConstants kc; kc.f = &bank[0][0]; VsRun r;
        return evalVsPosition(t.data(), t.size(), in, kc, p, r);
      };
      const float centre[3] = { 12.f, 5.f, 21.f }, nl = std::sqrt(30.f), n[3] = { 2.f / nl, 5.f / nl, 1.f / nl };
      bool ran = true, turns = false, fixed = made, flat = made, sized = made;
      float corners[4][4] = {};
      for (int k = 0; k < 4 && ran; ++k) {
        float a[4], b[4], pa[4], pb[4];
        if (!run(vs, camA, k, a) || !run(vs, camB, k, b)) { ran = false; break; }
        if (std::fabs(a[0] - b[0]) + std::fabs(a[1] - b[1]) + std::fabs(a[2] - b[2]) > 1e-3f) turns = true;
        if (!made) continue;
        if (!run(out, camA, k, pa) || !run(out, camB, k, pb)) { ran = false; break; }
        if (std::fabs(pa[0] - pb[0]) + std::fabs(pa[1] - pb[1]) + std::fabs(pa[2] - pb[2]) > 1e-4f) fixed = false;
        const float d[3] = { pa[0] - centre[0], pa[1] - centre[1], pa[2] - centre[2] }, od[3] = { b[0] - centre[0], b[1] - centre[1], b[2] - centre[2] };
        if (std::fabs(d[0] * n[0] + d[1] * n[1] + d[2] * n[2]) > 1e-4f) flat = false;
        if (std::fabs(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - std::sqrt(od[0] * od[0] + od[1] * od[1] + od[2] * od[2])) > 1e-4f) sized = false;
        memcpy(corners[k], pa, sizeof pa);
      }
      const bool level = made && ran && std::fabs(corners[1][1] - corners[0][1]) < 1e-4f && std::fabs(corners[1][0] - corners[0][0]) + std::fabs(corners[1][2] - corners[0][2]) > 0.5f;
      CHECK(ran && turns, "cards: %s as the game has it turns its leaf with the camera", L.name);
      CHECK(made && ran && fixed && flat && sized && level,
            "cards: %s's variant holds the leaf still under both cameras (%d), flat across the centre's direction from the tree (%d), its top edge level (%d), its size kept (%d)", L.name, (int) fixed, (int) flat, (int) level, (int) sized);
    }
    // trees near the camera (milestone 137): the blended set's vertex shaders hand the fade over where the table says
    {
      std::vector<DWORD> leafFadeVs, branchFadeVs, ps;
      if (loadShader("vs_1b5224b7e9bee689", leafFadeVs) && loadShader("vs_01729eebdb3cafd4", branchFadeVs)) {
        VsConstantOutputs lv, bv;
        const SolidFade* leavesNear = findSolidFade(0x714d5dae31da4378ull); const SolidFade* branchesNear = findSolidFade(0xd3759d8d17c63b92ull); const SolidFade* frondsNear = findSolidFade(0xca3faba5c3d2dbc2ull);
        CHECK(leavesNear && branchesNear && frondsNear && analyzeVertexConstantOutputs(leafFadeVs.data(), leafFadeVs.size(), lv) && analyzeVertexConstantOutputs(branchFadeVs.data(), branchFadeVs.size(), bv)
              && lv.c[leavesNear->fadeInput] == 2 * 4 + 1 && bv.c[branchesNear->fadeInput] == 2 * 4 + 1 && bv.c[frondsNear->fadeInput] == 2 * 4 + 1 && !findSolidFade(0x7e48acce64547cd0ull)
              && lv.c[kSemColor0 * 4] == 126 * 4 && !lv.rel[kSemColor0 * 4] && lv.rel[leavesNear->fadeInput],
              "near trees: VS 1b5224b7 hands c2.y over in TEXCOORD1.w, 01729eeb in COLOR0.w -- the fade the table reads; the opaque leaves' PS is not in the table");
        bool samplersOk = true;
        for (const SolidFade& f : kSolidFades) {
          char name[32]; snprintf(name, sizeof name, "ps_%016llx", (unsigned long long) f.hash);
          PsAnalysis a;
          if (!loadShader(name, ps) || !analyzePixelShader(ps.data(), ps.size(), a) || !a.samplers[f.sampler].read || a.samplers[f.sampler].texcoord != 1 || !a.samplers[f.sampler].reachesColor()) samplersOk = false;
        }
        CHECK(samplersOk, "near trees: each table shader reads its leaf texture s1 with TEXCOORD1 into the colour");
      } else SKIP("vs_1b5224b7 / vs_01729eeb dumps not found");
    }
    std::vector<DWORD> branch, obj;
    if (loadShader("vs_854fd850257ee36f", branch) && loadShader("vs_0ba6ddb9aa01913c", obj)) {
      CameraCard a, b;
      CHECK(!analyzeCameraCard(branch.data(), branch.size(), a) && !analyzeCameraCard(obj.data(), obj.size(), b), "cards: the branches' VS 854fd850 and an object's VS 0ba6ddb9 build no camera-facing cards");
    } else SKIP("vs_854fd850 / vs_0ba6ddb9 dumps not found");
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
      CHECK(chooseNormalTexcoord(w, 0, (uint16_t) (1u << 6)) == 6 && chooseNormalTexcoord(w, 0, (uint16_t) (1u << 3)) == -1, "  (M90) only a candidate the pixel shader reads: the read one, or none");
      VsNormalInfo r;
      CHECK(analyzeVertexNormal(roof.data(), roof.size(), r) && r.hasNormalInput && r.candidates == 0, "roof VS b077115f: normal mixed with the per-vertex pitch (POSITION2): no candidate");
      VsNormalInfo f;
      CHECK(analyzeVertexNormal(floors.data(), floors.size(), f) && !f.hasNormalInput && f.candidates == 0, "floors VS 0fcdd508: no normal input");
      PsAnalysis pa;
      CHECK(analyzePixelShader(psObj.data(), psObj.size(), pa) && (pa.normalTexcoords & (1u << 1)), "object PS 0c19795e treats TEXCOORD1 as a normal (mask %x)", pa.normalTexcoords);
      // milestone 90: the town's water decodes its NORMAL input into TEXCOORD7, which its pixel shader never reads
      std::vector<DWORD> wvs, wps, fvs, fps;
      if (loadShader("vs_2a6edce65dc2afb5", wvs) && loadShader("ps_f74b4657dbfd60bc", wps) && loadShader("vs_f10077e8d85a0f0f", fvs) && loadShader("ps_9a371afbc53c8ed8", fps)) {
        VsNormalInfo wv, fv; PsAnalysis wp, fp;
        const bool okW = analyzeVertexNormal(wvs.data(), wvs.size(), wv) && analyzePixelShader(wps.data(), wps.size(), wp) && wv.candidates == (1u << 7) && wp.inputTexcoords == 0x3Fu
                         && chooseNormalTexcoord(wv, wp.normalTexcoords, wp.inputTexcoords) == -1 && chooseNormalTexcoord(wv, wp.normalTexcoords) == 7;
        std::vector<DWORD> hid = wvs; VsNormalInfo hv;
        const bool okHide = okW && hideNormalInput(hid, wv) && analyzeVertexNormal(hid.data(), hid.size(), hv) && !hv.hasNormalInput;
        CHECK(okW && okHide, "normal (M90): water VS 2a6edce6's candidate TEXCOORD7 is not read by PS f74b4657 (reads %x): no candidate, the packed input hidden (triangle normals)", wp.inputTexcoords);
        const bool okF = analyzeVertexNormal(fvs.data(), fvs.size(), fv) && analyzePixelShader(fps.data(), fps.size(), fp)
                         && chooseNormalTexcoord(fv, fp.normalTexcoords) == 0 && chooseNormalTexcoord(fv, fp.normalTexcoords, fp.inputTexcoords) == 4;
        CHECK(okF, "normal (M90): VS f10077e8 with PS 9a371afb: TEXCOORD4, the candidate the pixel shader reads (was TEXCOORD0, which it does not; candidates %x, reads %x)", fv.candidates, fp.inputTexcoords);
      } else SKIP("water / f10077e8 shader dumps not found: the M90 normal rule not tested on them");
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
      // milestone 76: a compressed candidate beats an uncompressed one (the game's light maps are not DXT)
      auto bindTex = [](bool* color2D, uint32_t* fmt, uint16_t* w, uint16_t* h, int s, D3DFORMAT f, uint16_t W, uint16_t H) { color2D[s] = true; fmt[s] = (uint32_t) f; w[s] = W; h[s] = H; };
      if (loadShader("ps_579ba93e4482bba9", t)) {
        bool color2D[16] = {}; uint32_t fmt[16] = {}; uint16_t w[16] = {}, h[16] = {}; int stage = -1, tc = -1;
        bindTex(color2D, fmt, w, h, 0, D3DFMT_A8R8G8B8, 1024, 512); bindTex(color2D, fmt, w, h, 1, D3DFMT_DXT1, 16, 64); bindTex(color2D, fmt, w, h, 2, D3DFMT_DXT1, 256, 256);
        CHECK(analyzePixelShader(t.data(), t.size(), a) && chooseAutoAlbedo(a, color2D, fmt, w, h, stage, tc) && stage == 1 && tc == 0,
              "auto (M76): walls-D trim 579ba93e -> the 16x64 DXT1 trim s1 at TEXCOORD0, not the lot's 1024x512 light map s0 (chose s%d at TEXCOORD%d)", stage, tc);
      } else SKIP("auto: ps_579ba93e4482bba9 not found");
      if (loadShader("ps_2ee776917a22577f", t)) {
        bool color2D[16] = {}; uint32_t fmt[16] = {}; uint16_t w[16] = {}, h[16] = {}; int stage = -1, tc = -1;
        bindTex(color2D, fmt, w, h, 1, D3DFMT_A8R8G8B8, 256, 128); bindTex(color2D, fmt, w, h, 2, D3DFMT_DXT1, 64, 64);
        CHECK(analyzePixelShader(t.data(), t.size(), a) && chooseAutoAlbedo(a, color2D, fmt, w, h, stage, tc) && stage == 2 && tc == 2,
              "auto (M76): object 2ee77691 -> its 64x64 DXT1 colour s2 at TEXCOORD2, not the 256x128 sky-light map s1 (chose s%d at TEXCOORD%d)", stage, tc);
        bool c2[16] = {}; uint32_t f2[16] = {}; uint16_t w2[16] = {}, h2[16] = {};
        bindTex(c2, f2, w2, h2, 1, D3DFMT_A8R8G8B8, 32, 32); bindTex(c2, f2, w2, h2, 2, D3DFMT_A1R5G5B5, 4, 4);
        CHECK(chooseAutoAlbedo(a, c2, f2, w2, h2, stage, tc) && stage == 1, "  with no compressed candidate the score decides as before (chose s%d)", stage);
      } else SKIP("auto: ps_2ee776917a22577f not found");
      if (loadShader("ps_ff72720db4324926", t)) {
        bool color2D[16] = {}; uint32_t fmt[16] = {}; uint16_t w[16] = {}, h[16] = {}; int stage = -1, tc = -1;
        bindTex(color2D, fmt, w, h, 0, D3DFMT_A8R8G8B8, 256, 128); bindTex(color2D, fmt, w, h, 1, D3DFMT_DXT1, 256, 256);
        CHECK(findAlbedoStage(0xff72720db4324926ull) == nullptr && findTexcoordPromote(0x9f227c82c758a989ull) == nullptr
              && analyzePixelShader(t.data(), t.size(), a) && chooseAutoAlbedo(a, color2D, fmt, w, h, stage, tc) && stage == 1 && tc == 0,
              "auto (M76): ff72720d (the floor tiles' layout) untabled, its vertex shader 9f227c82 unpromoted -> the DXT1 s1 at TEXCOORD0, not the room light map s0 (chose s%d at TEXCOORD%d)", stage, tc);
      } else SKIP("auto: ps_ff72720db4324926 not found");
    }
    {
      // the light table (format 3): the lights per model
      LiteTable table;
      CHECK(!liteParseLine(table, "model 0000000000f29289 1 5 0 1.51 0 1 0.975 0.85 60 0 1 0 30 3 50 0.2327 0.2327 0.2327\n") && table.models.empty(), "light table: nothing is taken before the format line (an older table names no lamp)");
      liteParseLine(table, "# a comment\n");
      liteParseLine(table, "format 3\n");
      const bool m1 = liteParseLine(table, "model 0000000000f29289 1 5 0.0000 1.5100 0.0000 1.0000 0.9750 0.8500 60.0000 0.0000 1.0000 0.0000 30.0000 3.0000 50.0000 0.2327 0.2327 0.2327\n");
      const bool m2 = liteParseLine(table, "model 000000000005a335 2 11 0 1.69 0.33 1 1 1 97 0 0 0 0 0 0 0 0 0 11 0 3.69 0.0111 0.8 0.8 0.76 40 0 1 0 35 2.2 20 0.2327 0.2327 0.2327\n");
      const bool m3 = liteParseLine(table, "model 0000000000000777 1 7 0 0 0 1 1 1 1 0 0 1 1 0 0 0 0 0\n");   // a window: no lamp
      const bool o1 = liteParseLine(table, "object 0000000000000604 0000000000f29289\n");   // an object line of an older tool: ignored (milestone 145)
      CHECK(m1 && m2 && !m3 && !o1 && table.models.size() == 2, "light table: two models with lamp lights read; a model of windows alone and an object line are left out");
      const LiteModel* stand = table.model(0xf29289ull);
      CHECK(stand && stand->n == 1 && stand->lights[0].type == 5 && nearf(stand->lights[0].pos[1], 1.51f) && nearf(stand->lights[0].intensity, 60.f) && nearf(stand->lights[0].at[1], 1.f) && nearf(stand->lights[0].d[0], 30.f) && nearf(stand->lights[0].d[2], 50.f) && nearf(stand->lights[0].d[3], 0.2327f),
            "light table: the standing lamp's lamp shade with its direction, cone, bottom cone and shade");
      const LiteModel* street = table.model(0x5a335ull);
      CHECK(street && street->n == 2 && street->lights[0].type == 11 && street->lights[1].type == 11 && nearf(street->lights[1].pos[1], 3.69f) && nearf(street->lights[1].d[0], 35.f), "light table: a street lamp's two world lights");
      CHECK(table.model(0xf29289ull) == stand && table.model(0x604ull) == nullptr && table.model(0x123ull) == nullptr, "light table: a lamp's definition by its catalog model key; none by an object's key or a stranger's");
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
        {
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
    CHECK(tc && tc->tintReg == 8 && kTintRegister == 8 && findAlbedoStage(0x1234ull) == nullptr && (!tn || tn->tintReg < 0),
          "tint: the recolourable object PS carries the tint at c8; the 3-light variant carries none; unknown -> none");
    {
      // milestone 79: walls C has no colour texture, its colour is the constant c4; the albedo is its
      // opening mask s1 (greyscale, white where the wall stands) tinted by c4
      const AlbedoStage* wc = findAlbedoStage(0x7d2cbb8e474dfaf5ull);
      std::vector<DWORD> t; PsAnalysis a;
      const bool have = loadShader("ps_7d2cbb8e474dfaf5", t) && analyzePixelShader(t.data(), t.size(), a);
      CHECK(wc && wc->stage == 1 && wc->tintReg == 4 && (!have || (a.maskSampler == 1 && a.maskAlpha && !a.samplers[1].reachesColor())),
            "tint (M79): walls C -> s1, the opening mask (alpha only in the shader: %d), tinted by c4", have ? (int) a.samplers[1].colorChannels : -1);
    }
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
    CHECK(decompose(fromRows(rows), sc) && sc.mirrored && sc.fwd[1] > 0.05f && kindOfVerified(sc) == Kind::Reflection, "camera: the sea reflection camera is improper (det -1) and looks up -> Reflection");
    for (int r = 0; r < 4; ++r) rows[r*4+1] = -rows[r*4+1];         // un-mirrored across the water: the play camera
    Camera mc = {};
    CHECK(decompose(fromRows(rows), mc) && !mc.mirrored && mc.fwd[1] < -0.05f && kindOfVerified(mc) == Kind::Main, "camera: the play camera is a proper rotation (det +1) looking down -> Main");
    for (int r = 0; r < 4; ++r) rows[r*4+0] = -rows[r*4+0];         // reflected across a vertical plane: a wall mirror's pass
    Camera wc = {};
    CHECK(decompose(fromRows(rows), wc) && wc.mirrored && wc.fwd[1] < -0.05f && kindOfVerified(wc) == Kind::Reflection, "camera: a wall mirror's reflection camera looks down too but is improper -> Reflection (the mirrored house and terrain popping at close zoom)");
    // the play camera tilted to the horizon (and a touch above it) is still the play camera: only the basis decides
    Camera hc = mc; hc.fwd[1] = 0.f;
    CHECK(kindOfVerified(hc) == Kind::Main, "camera: a proper camera at the horizon -> Main (the old pitch test rejected it and broke the far view)");
    hc.fwd[1] = 0.2f;
    CHECK(kindOfVerified(hc) == Kind::Main, "camera: a proper camera looking slightly up -> Main");
    CHECK(isReflectionDraw(true, true, D3DZB_TRUE) && !isReflectionDraw(false, true, D3DZB_TRUE) && !isReflectionDraw(true, false, D3DZB_TRUE) && !isReflectionDraw(true, true, D3DZB_FALSE),
          "draw: a 3D draw under a mirrored camera is a reflection pass's and is dropped; 2D or depth-off draws under it (the compositor, the UI) are not");
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
      CHECK(decodeMaskRed(D3DFMT_DXT1, atlas.data(), atlas.size(), 256, 256, red) && red.size() == 65536 && level0Bytes(D3DFMT_DXT1, 256, 256) == 32768, "walls: DXT1 atlas decoded to a red plane");
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
      {
        // milestone 78: six round windows (radius 11 texels) in the window cell -- more hole rectangles
        // than the 64 that used to collapse to their bounding box: the cut is still exact, and the
        // wall between them stays
        std::vector<uint8_t> round = red;
        for (int y = 4; y < 132; ++y) for (int x = 112; x < 176; ++x) round[y * 256 + x] = 255;
        const int cx[6] = { 128, 158, 128, 158, 128, 158 }, cy[6] = { 30, 30, 66, 66, 102, 102 };
        for (int k = 0; k < 6; ++k) for (int y = cy[k] - 11; y <= cy[k] + 11; ++y) for (int x = cx[k] - 11; x <= cx[k] + 11; ++x)
          if ((x - cx[k]) * (x - cx[k]) + (y - cy[k]) * (y - cy[k]) <= 121) round[y * 256 + x] = 0;
        WallCutInput ri = in; ri.mask = round.data();
        WallCutOutput ro;
        auto killedR = [&](int x, int y) { const float t = (y + 0.5f - 4.f) / 128.f, need = (0.5f - 0.5f * (1.f - t)) * 255.f; return (float) round[y * 256 + x] < need; };
        double holeArea = 0; for (int y = 4; y < 132; ++y) for (int x = 112; x < 176; ++x) if (killedR(x, y)) holeArea += (1.0 / 64.0) * (1.0 / 128.0);
        auto stR = [&](const uint8_t* v, float& s, float& t) { int16_t tc2[4]; memcpy(tc2, v + 20, 8); s = tc2[0] / 4096.f; t = tc2[1] / 4096.f; };
        auto rectXR = [&](const uint8_t* v) { int16_t r[4]; memcpy(r, v + 32, 8); return r[0]; };
        auto areaOf = [&](const uint8_t* const v[3]) { float s[3], t[3]; for (int i = 0; i < 3; ++i) stR(v[i], s[i], t[i]); return std::fabs((s[1] - s[0]) * (t[2] - t[0]) - (s[2] - s[0]) * (t[1] - t[0])) / 2; };
        // the wall between the windows: s over texels 141..146, t over rows 29..101 (the windows' extent)
        auto inGap = [&](float s, float t) { const float px = 112.f + s * 64.f, py = 4.f + t * 128.f; return px > 141.f && px < 146.f && py > 30.f && py < 100.f; };
        CHECK(cutWallOpenings(ri, ro) && ro.changed && ro.stats.holeRects > 64 && ro.stats.cut == 4, "walls (M78): six round windows make %u hole rectangles; the window segment's 4 triangles cut", ro.stats.holeRects);
        double before = 0, after = 0, gap = 0; int inside = 0;
        for (int t = 0; t < 40; ++t) { const uint16_t* ix = (const uint16_t*) ib.data() + 96 + 3 * t; const uint8_t* v[3]; for (int i = 0; i < 3; ++i) v[i] = vb0.data() + (ix[i] + 48) * 44; if (rectXR(v[0]) == 1792) before += areaOf(v); }
        for (uint32_t t = 0; t < ro.triangleCount; ++t) {
          const uint8_t* v[3]; for (int i = 0; i < 3; ++i) v[i] = ro.vb0.data() + ro.ib[3 * t + i] * 44;
          if (rectXR(v[0]) != 1792) continue;
          after += areaOf(v);
          float s[3], tt[3]; for (int i = 0; i < 3; ++i) stR(v[i], s[i], tt[i]);
          if (inGap((s[0] + s[1] + s[2]) / 3.f, (tt[0] + tt[1] + tt[2]) / 3.f)) gap += areaOf(v);
        }
        for (uint32_t v = 0; v < ro.vertexCount; ++v) {
          const uint8_t* p = ro.vb0.data() + v * 44; if (rectXR(p) != 1792) continue;
          float s, t; stR(p, s, t);
          const float px = 112.f + s * 64.f, py = 4.f + t * 128.f, fx = px - floorf(px), fy = py - floorf(py);
          if (fx > 0.02f && fx < 0.98f && fy > 0.02f && fy < 0.98f && killedR((int) floorf(px), (int) floorf(py))) ++inside;
        }
        CHECK(std::fabs((before - holeArea) - after) < 3e-3 && inside == 0, "walls (M78): the segment's area %.4f minus the six openings %.4f = %.4f after the cut; no vertex inside an opening (%d)", before, holeArea, after, inside);
        CHECK(gap > 0.01, "walls (M78): the wall between the round windows stays (%.4f of it in triangles; their bounding box would have removed it)", gap);
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
        // milestone 77: with no opening test (threshold below 0: the mask unknown or unreadable) the
        // hidden triangles still go, and nothing is cut -- not even with no mask at all
        hi.params.threshold = -1.f; hi.mask = nullptr;
        CHECK(cutWallOpenings(hi, ho) && ho.changed && ho.stats.hidden == 4 && ho.stats.cut == 0 && ho.stats.removed == 0 && ho.stats.cellsWithOpenings == 0 && ho.triangleCount == 36,
              "walls (M77): no opening test: the hidden segment's four triangles still dropped, nothing cut (%u hidden, %u cut, %u triangles left)", ho.stats.hidden, ho.stats.cut, ho.triangleCount);
        hi.vb1 = vb1.data();
        CHECK(cutWallOpenings(hi, ho) && !ho.changed && ho.stats.cut == 0 && ho.stats.removed == 0, "walls (M77): no opening test and nothing hidden -> unchanged (the game's draw goes out)");
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
        CHECK(decodeMaskRed(D3DFMT_A1R5G5B5, px, sizeof px, 4, 4, r16) && r16.size() == 16 && r16[0] == 255 && r16[15] == 255 && level0Bytes(D3DFMT_A1R5G5B5, 4, 4) == 32, "walls: the game's blank 4x4 A1R5G5B5 mask decodes white");
        uint8_t px565[2] = { 0x00, 0xF8 };   // red 31, green 0, blue 0
        CHECK(decodeMaskRed(D3DFMT_R5G6B5, px565, 2, 1, 1, r16) && r16[0] == 255 && level0Bytes(D3DFMT_R5G6B5, 1, 1) == 2, "walls: R5G6B5 red decodes");
      }
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
    {
      // milestone 80: the glass rule -- the game's glass shaders read only cube maps; the marker's hash is
      // the one the mod in the repo names
      const char* glassPs[5] = { "ps_98e23f47d947eb22", "ps_3197bfdef2330503", "ps_66516d5db94ab307", "ps_efdac7f048e21b1f", "ps_db28eb0c60fdb2fb" };
      int found = 0, glass = 0;
      for (const char* n : glassPs) { std::vector<DWORD> gt; PsAnalysis ga; if (loadShader(n, gt) && analyzePixelShader(gt.data(), gt.size(), ga)) { ++found; if (isGlassShader(ga)) ++glass; } }
      std::vector<DWORD> wt, ot; PsAnalysis wa, oa;
      const bool notWall = !loadShader("ps_936215cf1e55a47e", wt) || (analyzePixelShader(wt.data(), wt.size(), wa) && !isGlassShader(wa));
      const bool notObject = !loadShader("ps_0c19795eb80e2e96", ot) || (analyzePixelShader(ot.data(), ot.size(), oa) && !isGlassShader(oa));
      CHECK(found >= 1 && glass == found && notWall && notObject && !isGlassShader(PsAnalysis()),
            "glass (M80): the game's glass shaders read only cube maps (%d of %d dumps found); walls A, the object shader and an empty analysis are not glass", glass, found);
      // milestone 81: textured glass by name; the Sims' hair pass and the light-beam cards (cube + 2D, blended) are not glass
      std::vector<DWORD> tg, hair, beam; PsAnalysis tga, ha, ba;
      const bool tgOk = !loadShader("ps_29c6b22234617c1a", tg) || (analyzePixelShader(tg.data(), tg.size(), tga) && !isGlassShader(tga) && !namedGlass(0x29c6b22234617c1aull));
      const bool hairOk = !loadShader("ps_57a5a049ffa47770", hair) || (analyzePixelShader(hair.data(), hair.size(), ha) && !isGlassShader(ha) && !namedGlass(0x57a5a049ffa47770ull));
      const bool beamOk = !loadShader("ps_7304aaea6a75fb3f", beam) || (analyzePixelShader(beam.data(), beam.size(), ba) && !isGlassShader(ba) && !namedGlass(0x7304aaea6a75fb3full));
      const NamedGlass* door = namedGlass(0x572773cfbd618a3aull);
      CHECK(tgOk && hairOk && beamOk && namedGlass(0xac4184cee232ed04ull) && namedGlass(0xac4184cee232ed04ull)->material == kClearGlass && !namedGlass(0x3197bfdef2330503ull)
            && door && door->material == kClearGlass && door->bumpStage == 2,
            "glass (M81, M87, M100, M109, M117): named glass ac4184ce clear, the shower door 572773cf bumpy; the skill pill 29c6b222 not glass (its own colours), the hair pass 57a5a049, the light-beam card 7304aaea and the cube-only pane 3197bfde are not named");
      // milestones 102, 105, 107: the named forms 2 and 3 are not cube-only, so the name is what makes them glass
      bool farOk = true;
      for (uint64_t hsh : { 0x85e9c3381d5bf054ull, 0xd03ebab11453bca1ull, 0x2b1da1b45f51d3f9ull, 0x8ff495765d26a6fdull,
                            0x7eeb349a23cbefefull, 0x8fe3ce7c5fbc6234ull, 0x910a56f24813e248ull }) {
        char n[32]; snprintf(n, sizeof n, "ps_%016llx", (unsigned long long) hsh);
        std::vector<DWORD> sb; PsAnalysis sa;
        if (loadShader(n, sb) && (!analyzePixelShader(sb.data(), sb.size(), sa) || isGlassShader(sa))) farOk = false;
      }
      const NamedGlass* doorKin = namedGlass(0x85e9c3381d5bf054ull); const NamedGlass* carFar = namedGlass(0xd03ebab11453bca1ull);
      const NamedGlass* passing = namedGlass(0x66516d5db94ab307ull); const DropPs* hairOut = findDropPs(0x45c7a7cd511b5233ull);
      CHECK(farOk && doorKin && doorKin->material == kClearGlass && carFar && carFar->material == kCarGlass && passing && passing->material == kCarGlass && !namedGlass(0x0c2df3be933b2117ull)
            && !namedGlass(0x45c7a7cd511b5233ull) && hairOut && hairOut->kind == kBlendedCopy,
            "glass (M102, M105, M134): 85e9c338 clear glass; car glass: a passing car's 66516d5d, a distant car's d03ebab1; 0c2df3be not named; 45c7a7cd is the Sims' hair outdoors, not glass (its blended copy never sent); 85e9c338 / d03ebab1 not cube-only");
      // milestones 107, 109: the survey's glass -- clear; bumpy with a normal map; objects fading in are not glass
      auto glassMat = [](uint64_t hsh) { const NamedGlass* g = namedGlass(hsh); return g ? (int) g->material : -1; };
      CHECK(farOk && glassMat(0x2b1da1b45f51d3f9ull) == kPlumbob && glassMat(0x8ff495765d26a6fdull) == kClearGlass && glassMat(0x7eeb349a23cbefefull) == kClearGlass
            && glassMat(0x8fe3ce7c5fbc6234ull) == kClearGlass && glassMat(0x910a56f24813e248ull) == kClearGlass && glassMat(0xa9336d35a25143aeull) == -1 && findDropPs(0xa9336d35a25143aeull) && findDropPs(0xa9336d35a25143aeull)->kind == kLeftOut
            && glassMat(0x7b3cb6be7d73e3b4ull) == -1 && glassMat(0xa8c64e11b251a0cbull) == -1 && glassMat(0x834b21919e9f8d8aull) == -1 && glassMat(0x3661ea706449953cull) == -1,
            "glass (M107, M114): the survey's 8ff49576 / 7eeb349a / 8fe3ce7c / 910a56f2 clear glass, the plumbob 2b1da1b4 its own, none cube-only; the speakers' sound waves a9336d35 left out, not glass; the fading objects 7b3cb6be / a8c64e11 / 834b2191 / 3661ea70 not named");
      // milestone 109: bumpy glass -- the bump map's sampler and coordinate from the bytecode (the promotion needs a plain read),
      // the slope scale's register; nothing else has a bump stage; a mod's material names parsed
      {
        struct Bumpy { uint64_t hash; const char* dump; int texcoord; int scaleReg; };
        const Bumpy bumpy[] = { { 0x572773cfbd618a3aull, "ps_572773cfbd618a3a", 5, 14 }, { 0x8fe3ce7c5fbc6234ull, "ps_8fe3ce7c5fbc6234", 5, 14 } };
        bool bumpOk = true; int bumpRead = 0;
        for (const Bumpy& b : bumpy) {
          const NamedGlass* g = namedGlass(b.hash);
          if (!g || g->bumpStage != 2 || g->bumpScaleReg != b.scaleReg || g->material != kClearGlass) bumpOk = false;
          std::vector<DWORD> bt; PsAnalysis ba2;
          if (loadShader(b.dump, bt) && analyzePixelShader(bt.data(), bt.size(), ba2)) {
            ++bumpRead;
            const PsSamplerUse& su = ba2.samplers[2];
            if (!su.read || su.dependent || su.projective || su.cube || su.texcoord != b.texcoord) bumpOk = false;
          }
        }
        int others = 0; for (const NamedGlass& g : kNamedGlass) if (g.bumpStage >= 0) ++others;
        const std::vector<uint64_t> parsed = modMaterialHashes("def Material \"mat_0123456789ABCDEF\"\n{ }\ndef Material \"mat_FEDCBA9876543210\"\ndef Material \"mat_XYZ\"");
        CHECK(bumpOk && others == 2 && namedGlass(0x910a56f24813e248ull) && namedGlass(0x910a56f24813e248ull)->bumpStage < 0 && parsed.size() == 2 && parsed[0] == 0x0123456789ABCDEFull && parsed[1] == 0xFEDCBA9876543210ull,
              "bumpy glass (M109, M111, M114): 572773cf / 8fe3ce7c read their bump map at s2 plainly at TEXCOORD5, slopes x c14.x (%d of 2 dumps read); only they have a bump stage -- the unplayable lot's windows 910a56f2 are plain clear glass; a mod's material hashes parsed", bumpRead);
      }
      // milestone 104: a sheet with a back side keeps one facing per plane; a pane with a thickness keeps both sides
      {
        auto tri = [](std::vector<float>& v, std::initializer_list<float> p) { v.insert(v.end(), p); };
        std::vector<float> sheet;   // a 1 x 1 square in z = 0: front (+z) as two triangles, its back (-z) on the other diagonal
        tri(sheet, { 0,0,0, 1,0,0, 1,1,0 }); tri(sheet, { 0,0,0, 1,1,0, 0,1,0 });
        tri(sheet, { 0,0,0, 0,1,0, 1,0,0 }); tri(sheet, { 1,0,0, 0,1,0, 1,1,0 });
        std::vector<uint8_t> keep;
        const uint32_t d1 = glassFrontTriangles(sheet.data(), 4, keep);
        const bool sheetOk = d1 == 2 && keep[0] && keep[1] && !keep[2] && !keep[3];
        std::vector<float> pane = sheet;   // the back moved 2 cm behind: two planes, both kept
        for (size_t i = 18; i < pane.size(); i += 3) pane[i + 2] = -0.02f;
        const uint32_t d2 = glassFrontTriangles(pane.data(), 4, keep);
        std::vector<float> two;   // two separate squares on one plane, facing the same way: both kept
        tri(two, { 0,0,0, 1,0,0, 1,1,0 }); tri(two, { 5,0,0, 6,0,0, 6,1,0 });
        const uint32_t d3 = glassFrontTriangles(two.data(), 2, keep);
        std::vector<float> flat;   // a degenerate triangle has no plane and is kept
        tri(flat, { 0,0,0, 1,0,0, 2,0,0 });
        const uint32_t d4 = glassFrontTriangles(flat.data(), 1, keep);
        CHECK(sheetOk && d2 == 0 && d3 == 0 && d4 == 0 && keep.size() == 1 && keep[0],
              "glass (M104): a sheet's back side on the other diagonal is left out (%u of 4), a pane 2 cm thick keeps both sides (%u), coplanar sheets facing one way are kept (%u), a degenerate triangle is kept (%u)", d1, d2, d3, d4);
      }
      // milestones 82, 101: the reflective sheet's PS 86dad57d reads only a cube; under the stencil test it is a mirror's face
      std::vector<DWORD> mp; PsAnalysis ma;
      const bool mirrorCube = !loadShader("ps_86dad57d0dc73989", mp) || (analyzePixelShader(mp.data(), mp.size(), ma) && isGlassShader(ma));
      CHECK(mirrorCube && (!ma.valid || (isReflectiveSheet(ma, TRUE) && !isReflectiveSheet(ma, FALSE))), "mirror (M82, M101): the reflective sheet PS 86dad57d reads only a cube; with the stencil test on it is a mirror's face");
      // milestones 84, 131: the Sims' soft shadow blob -- dropped by its pixel shaders (game fakes), no longer listed by its VS
      // milestones 97, 97c: a zero-thickness wall's back side -- the same square in the plane x = 512, the back
      // cut along the other diagonal (run 209); a wall with a thickness has its other side on another plane
      {
        const int16_t a[4] = { 512, -770, 768, 0 }, b[4] = { 512, -770, 896, 0 }, c[4] = { 512, -2, 896, 0 }, d[4] = { 512, -2, 768, 0 };
        const int16_t e[4] = { 527, -770, 768, 0 }, f[4] = { 527, -770, 896, 0 }, g[4] = { 527, -2, 896, 0 };
        const std::vector<uint64_t> front = { wallTriKey(a, b, c), wallTriKey(a, c, d) };
        const std::vector<uint64_t> back = { wallTriKey(b, a, d), wallTriKey(b, d, c) };        // the other diagonal, facing the other way
        const std::vector<uint64_t> same = { wallTriKey(b, c, d), wallTriKey(b, d, a) };        // the other diagonal, the same facing
        const std::vector<uint64_t> thick = { wallTriKey(f, e, g) };                             // the other way, 15 units further out
        std::unordered_set<uint64_t> kept(front.begin(), front.end());
        uint32_t m1 = 0, m2 = 0, m3 = 0;
        const bool backIs = isWallBackSide(back, kept, m1), sameIs = isWallBackSide(same, kept, m2), thickIs = isWallBackSide(thick, kept, m3);
        CHECK(backIs && m1 == 2 && !sameIs && m2 == 0 && !thickIs && m3 == 0 && wallTriKey(a, a, b) == 0 && wallTriKey(a, b, c) == wallTriKey(b, c, a) && (wallTriKey(a, b, c) ^ 1u) == wallTriKey(c, b, a),
              "wall back side (M97c): the other diagonal facing the other way on the same plane is a back side; the same facing, or another plane, is not");
      }
      static uint32_t gm[kGlassMarkerSize * kGlassMarkerSize]; for (auto& p : gm) p = kGlassMaterial[kClearGlass].colour;
      const uint64_t gh = (uint64_t) XXH3_64bits(gm, sizeof gm);
      std::vector<uint8_t> usd; char name[32]; snprintf(name, sizeof name, "mat_%016llX", (unsigned long long) kGlassMaterial[kClearGlass].hash);
      const bool haveUsd = loadBytes("../remix-mod/Sims3Glass/mod.usda", usd);
      const std::string u(usd.begin(), usd.end());
      CHECK(gh == kGlassMaterial[kClearGlass].hash && haveUsd && u.find(name) != std::string::npos && u.find("AperturePBR_Translucent.mdl") != std::string::npos && u.find("/RootNode/Looks/") != std::string::npos,
            "glass (M80): the marker's level-0 hash 0x%016llX names the translucent material %s in sims3/remix-mod/Sims3Glass/mod.usda", (unsigned long long) gh, name);
      // milestone 101: the mirror marker's hash names an opaque, fully metallic, smooth material naming no albedo
      for (auto& p : gm) p = kMirrorMarkerColour;
      const uint64_t mh = (uint64_t) XXH3_64bits(gm, sizeof gm);
      char mirName[32]; snprintf(mirName, sizeof mirName, "mat_%016llX", (unsigned long long) kMirrorMarkerHash);
      const size_t mat = u.find(mirName), mend = mat == std::string::npos ? mat : u.find("token outputs:out", mat);
      CHECK(mh == kMirrorMarkerHash && mat != std::string::npos && mend != std::string::npos && u.find("AperturePBR_Opacity.mdl", mat) < mend && u.find("metallic_constant = 1", mat) < mend
            && u.find("reflection_roughness_constant", mat) < mend && u.find("diffuse_texture", mat) > mend && u.find("diffuse_color_constant", mat) > mend,
            "mirror (M101): the marker's level-0 hash 0x%016llX names the opaque metallic material %s in Sims3Glass/mod.usda (no albedo of its own)", (unsigned long long) mh, mirName);
      // milestones 86, 93, 99: each water material's marker hash names its material in the water mod -- a volume
      // with its own ripple map -- and no material of the glass mod; the water shaders by name
      std::vector<uint8_t> wusd; const bool haveWater = loadBytes("../remix-mod/Sims3Water/mod.usda", wusd);
      const std::string uw(wusd.begin(), wusd.end());
      for (int m = 0; m < kWaterMaterials; ++m) {
        const WaterMaterial& w = kWaterMaterial[m];
        for (auto& p : gm) p = w.colour;
        const uint64_t hm = (uint64_t) XXH3_64bits(gm, sizeof gm);
        char mname[32]; snprintf(mname, sizeof mname, "mat_%016llX", (unsigned long long) w.hash);
        char ripples[64]; snprintf(ripples, sizeof ripples, "@./textures/%s@", w.ripples);
        const size_t at = uw.find(mname), end = at == std::string::npos ? at : uw.find("token outputs:out", at);
        bool distinct = true; for (int o = 0; o < m; ++o) if (kWaterMaterial[o].hash == w.hash) distinct = false;
        CHECK(haveWater && hm == w.hash && at != std::string::npos && end != std::string::npos && uw.find("ior_constant = 1.33", at) < end && uw.find("thin_walled = 0", at) < end
              && uw.find(ripples, at) < end && u.find(mname) == std::string::npos && distinct && w.hash != kGlassMaterial[kClearGlass].hash,
              "water (M99): the %s marker's level-0 hash 0x%016llX names %s in Sims3Water/mod.usda (IOR 1.33, a volume, %s), not in the glass mod", w.name, (unsigned long long) hm, mname, w.ripples);
      }
      CHECK(waterMaterial(0xd40999e5838e8b05ull) == kWaterPool && waterMaterial(0x85c0a78a614b15d3ull) == kWaterPool && waterMaterial(0x11a6bdfd3e77d03aull) == kWaterPond
            && waterMaterial(0xf74b4657dbfd60bcull) == kWaterSea && waterMaterial(0x387e1a15c63c120aull) == kWaterObject
            && waterMaterial(0xd92d3913aba53da8ull) == kWaterObject && waterMaterial(0xaf6cd85bfab9f36dull) == kWaterObject && waterMaterial(0xdb58e590608be737ull) == kWaterSea
            && !namedGlass(0xd92d3913aba53da8ull) && !namedGlass(0xdb58e590608be737ull)
            && waterMaterial(0xf45e6c607bb94189ull) == -1 && waterMaterial(0x3197bfdef2330503ull) == -1 && isWaterPs(0x11a6bdfd3e77d03aull) && !isWaterPs(0xf45e6c607bb94189ull),
            "water (M86-M108): the pool's surfaces d40999e5 / 85c0a78a, the pond's 11a6bdfd, the town's water f74b4657 and the open water db58e590 (the sea), the instanced 387e1a15 and d92d3913 / af6cd85b (object water); not the pool floor f45e6c60 nor the glass");
      // milestone 105: the car glass marker's hash names the tinted thin glass
      for (auto& p : gm) p = kGlassMaterial[kCarGlass].colour;
      const uint64_t ch = (uint64_t) XXH3_64bits(gm, sizeof gm);
      char cname[32]; snprintf(cname, sizeof cname, "mat_%016llX", (unsigned long long) ch);
      const size_t cat = u.find(cname);
      const size_t cend = cat == std::string::npos ? cat : u.find("token outputs:out", cat);
      CHECK(ch == kGlassMaterial[kCarGlass].hash && cat != std::string::npos && cend != std::string::npos && u.find("thin_walled = 1", cat) < cend && u.find("use_diffuse_layer = 0", cat) < cend
            && u.find("transmittance_color = (0.72, 0.78, 0.75)", cat) < cend && u.find("thin_wall_thickness = 1\n", cat) < cend,
            "car glass (M105): the marker's level-0 hash 0x%016llX names the tinted thin glass %s in Sims3Glass/mod.usda", (unsigned long long) ch, cname);
      // milestones 114-116: the plumbob's marker names thin green glass, no glow; the three glass markers differ
      for (auto& p : gm) p = kGlassMaterial[kPlumbob].colour;
      const uint64_t ph = (uint64_t) XXH3_64bits(gm, sizeof gm);
      char pname[32]; snprintf(pname, sizeof pname, "mat_%016llX", (unsigned long long) ph);
      const size_t pat = u.find(pname);
      const size_t pend = pat == std::string::npos ? pat : u.find("token outputs:out", pat);
      CHECK(ph == kGlassMaterial[kPlumbob].hash && pat != std::string::npos && pend != std::string::npos && u.find("thin_walled = 1", pat) < pend && u.find("thin_wall_thickness = 1\n", pat) < pend && u.find("transmittance_color = (0.5, 0.92, 0.55)", pat) < pend
            && u.find("enable_emission", pat) > pend && kGlassMaterial[0].hash != kGlassMaterial[1].hash && kGlassMaterial[1].hash != kGlassMaterial[2].hash && kGlassMaterial[0].hash != kGlassMaterial[2].hash,
            "plumbob (M114-M116): the marker's level-0 hash 0x%016llX names the thin green glass %s in Sims3Glass/mod.usda (no glow)", (unsigned long long) ph, pname);
    }
    // milestone 119: a vertex shader's position evaluated on the client -- a hand-made shader, the shower door's
    // skinned shader with a known bone, every glass vertex shader in the dumps, and the way back from clip to world
    {
      std::vector<DWORD> t = { 0xFFFE0300u,
        0x0200001Fu, 0x80000000u, 0x900F0000u,                 // dcl_position v0
        0x0200001Fu, 0x80000000u, 0xE00F0000u };               // dcl_position o0
      for (DWORD row = 0; row < 4; ++row) { t.push_back(0x03000009u); t.push_back(0xE0000000u | (1u << (16 + row))); t.push_back(0x90E40000u); t.push_back(0xA0E40000u | row); }   // dp4 o0.<row>, v0, c<row>
      t.push_back(0x0000FFFFu);
      static float cf[256][4]; memset(cf, 0, sizeof cf);
      const float M[4][4] = { { 2, 0, 0, 1 }, { 0, 3, 0, 2 }, { 0, 0, 4, 3 }, { 0, 0, 0, 1 } };
      memcpy(cf, M, sizeof M);
      float in[16][4] = {}; in[0][0] = 1; in[0][1] = 2; in[0][2] = 3; in[0][3] = 1;
      VsConstants kc; kc.f = &cf[0][0]; VsRun run; float pos[4] = {};
      const bool handOk = evalVsPosition(t.data(), t.size(), in, kc, pos, run) && nearf(pos[0], 3.f) && nearf(pos[1], 8.f) && nearf(pos[2], 15.f) && nearf(pos[3], 1.f);
      // the shower door's VS b51f1577 family -- b3e88e28: bone 0 = identity plus (10, 20, 30); the camera rows c180-c183 the identity
      std::vector<DWORD> door; bool doorOk = false; float dp[4] = {};
      if (loadShader("vs_b3e88e28e856fac1", door)) {
        memset(cf, 0, sizeof cf);
        const float bone[3][4] = { { 1, 0, 0, 10 }, { 0, 1, 0, 20 }, { 0, 0, 1, 30 } };
        memcpy(cf, bone, sizeof bone);
        for (int i = 0; i < 4; ++i) cf[180 + i][i] = 1.f;
        float vin[16][4] = {};
        VsInputDcl dcls[16]; const int nd = vsInputDcls(door.data(), door.size(), dcls);
        for (int i = 0; i < nd; ++i) if (dcls[i].usage == kUsagePosition) { vin[dcls[i].reg][0] = 1; vin[dcls[i].reg][1] = 2; vin[dcls[i].reg][2] = 3; vin[dcls[i].reg][3] = 1; }
        VsRun dr;
        doorOk = evalVsPosition(door.data(), door.size(), vin, kc, dp, dr) && nearf(dp[0], 11.f) && nearf(dp[1], 22.f) && nearf(dp[2], 33.f) && nearf(dp[3], 1.f);
      }
      // every glass vertex shader: nothing the evaluator does not know
      int glassVs = 0, glassVsOk = 0; const char* firstFail = "";
      for (const char* vsName : { "vs_5126ba796dbf5622", "vs_34a201bbfafb6d8d", "vs_b51f157720f062d8", "vs_b3e88e28e856fac1", "vs_d251510d258367de", "vs_e79a4bf1a29d4470",
                                  "vs_a77613ea8f18457b", "vs_4e9298de4bffcede", "vs_6feaaa2aa6558225", "vs_6d4b6bccd89a560e", "vs_13b4ec4425c0cc7b", "vs_ddc6be9fd81a65f6",
                                  "vs_c96f14650fa756fa", "vs_6b921b44994c1feb", "vs_d7fede81e7ce2dda", "vs_4b9e80e93d8ace3d" }) {
        std::vector<DWORD> vt; if (!loadShader(vsName, vt)) continue;
        ++glassVs;
        for (int i = 0; i < 256; ++i) for (int j = 0; j < 4; ++j) cf[i][j] = (i % 4 == j) ? 1.f : 0.f;
        float vin[16][4] = {}; for (auto& v : vin) v[3] = 1.f;
        VsRun vr; float vp[4];
        evalVsPosition(vt.data(), vt.size(), vin, kc, vp, vr);
        if (!vr.failed) ++glassVsOk; else if (!*firstFail) firstFail = vsName;
      }
      // clip -> world: a world point through a view and a projection and back
      D3DMATRIX view = {}, proj = {};
      const float cy = std::cos(0.6f), sy = std::sin(0.6f);
      view.m[0][0] = cy; view.m[0][2] = -sy; view.m[1][1] = 1.f; view.m[2][0] = sy; view.m[2][2] = cy; view.m[3][0] = -40.f; view.m[3][1] = -5.f; view.m[3][2] = 120.f; view.m[3][3] = 1.f;
      proj.m[0][0] = 1.2f; proj.m[1][1] = 1.6f; proj.m[2][2] = 1.0001f; proj.m[2][3] = 1.f; proj.m[3][2] = -0.10001f;
      const float wp[4] = { 975.f, 42.9f, 906.f, 1.f }; float vv[4], clip[4], back[3] = {};
      rowTimes(wp, view, vv); rowTimes(vv, proj, clip);
      const bool backOk = clipToWorld(view, proj, clip, back) && std::fabs(back[0] - 975.f) < 0.05f && std::fabs(back[1] - 42.9f) < 0.05f && std::fabs(back[2] - 906.f) < 0.05f;
      CHECK(handOk && doorOk && glassVs >= 1 && glassVsOk == glassVs && backOk,
            "vertex shader position (M119): a hand-made dp4 shader (%.1f %.1f %.1f %.1f), the door family's skinned b3e88e28 at bone 0 + (10, 20, 30) -> (%.1f %.1f %.1f %.1f), %d of %d glass vertex shaders evaluated%s%s, clip back to the world (%.2f %.2f %.2f)",
            pos[0], pos[1], pos[2], pos[3], dp[0], dp[1], dp[2], dp[3], glassVsOk, glassVs, *firstFail ? " -- first failing: " : "", firstFail, back[0], back[1], back[2]);
    }
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

  // --- the hook's own calls (milestone 152): one guard, one undo log with scopes
  {
    FakeDev d; HookCalls c;
    d.rs[D3DRS_ALPHATESTENABLE] = FALSE; d.rs[D3DRS_ALPHAFUNC] = D3DCMP_LESS; d.rs[D3DRS_ALPHAREF] = 1; d.rs[D3DRS_ALPHABLENDENABLE] = TRUE;
    const HookCalls::Scope draw = c.open();
    c.holdRs(&d, D3DRS_ALPHATESTENABLE, TRUE); c.holdRs(&d, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL); c.holdRs(&d, D3DRS_ALPHAREF, 84);
    c.holdRs(&d, D3DRS_ALPHAREF, 120);   // a second group's reference: the game's value stays the saved one
    CHECK(c.count == 3 && d.rs[D3DRS_ALPHAREF] == 120 && c.holdsRs(D3DRS_ALPHAFUNC) && !c.holdsRs(D3DRS_ALPHABLENDENABLE) && c.own == 0,
          "hook calls: a state held once per scope, set again as often as wanted; the guard back to 0 after each call");
    const uint32_t before = d.sets;
    c.holdRs(&d, D3DRS_ZWRITEENABLE, 0);   // already 0 on the device
    CHECK(d.sets == before && c.holdsRs(D3DRS_ZWRITEENABLE), "hook calls: a state already at the value is held (taken) but not sent");
    {
      const HookCalls::Scope inner = c.open();   // a section inside the draw (the plate, the restore quad)
      c.holdRs(&d, D3DRS_ALPHAREF, 0); c.holdRs(&d, D3DRS_ALPHABLENDENABLE, FALSE);
      CHECK(!c.holdsRs(D3DRS_ALPHAFUNC) && c.holdsRs(D3DRS_ALPHAREF) && d.rs[D3DRS_ALPHAREF] == 0, "hook calls: a section's scope sees only its own holds");
      c.close(&d, inner);
      CHECK(d.rs[D3DRS_ALPHAREF] == 120 && d.rs[D3DRS_ALPHABLENDENABLE] == TRUE && c.count == 4 && c.holdsRs(D3DRS_ALPHAFUNC),
            "hook calls: a section's close puts back its own holds only -- the draw's alpha test still set");
    }
    d.ss[0][D3DSAMP_SRGBTEXTURE] = TRUE; d.tss[0][D3DTSS_COLOROP] = D3DTOP_MODULATE;
    c.holdSampler(&d, 0, D3DSAMP_SRGBTEXTURE, FALSE); c.holdStage(&d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    D3DVIEWPORT9 vp = { 0, 0, 1920, 1080, 0.f, 1.f }; d.vp = vp; D3DVIEWPORT9 sky = vp; sky.MinZ = sky.MaxZ = 1.f;
    c.holdViewport(&d, sky);
    D3DMATRIX id = {}; id.m[0][0] = id.m[1][1] = id.m[2][2] = id.m[3][3] = 1.f; d.world = id; D3DMATRIX w = id; w.m[3][0] = 5.f;
    c.holdWorld(&d, w);
    CHECK(d.ss[0][D3DSAMP_SRGBTEXTURE] == FALSE && d.tss[0][D3DTSS_COLOROP] == D3DTOP_SELECTARG1 && d.vp.MinZ == 1.f && d.world.m[3][0] == 5.f, "hook calls: sampler, stage, viewport and WORLD held");
    c.close(&d, draw);
    CHECK(c.count == 0 && c.scope == 0 && d.rs[D3DRS_ALPHATESTENABLE] == FALSE && d.rs[D3DRS_ALPHAFUNC] == D3DCMP_LESS && d.rs[D3DRS_ALPHAREF] == 1
          && d.ss[0][D3DSAMP_SRGBTEXTURE] == TRUE && d.tss[0][D3DTSS_COLOROP] == D3DTOP_MODULATE && d.vp.MinZ == 0.f && d.world.m[3][0] == 0.f && c.own == 0,
          "hook calls: the draw's close puts every state back to the game's value");
    // a dropped draw: its scope closes empty; a scope around a re-issued draw keeps its holds through the draw's own
    const HookCalls::Scope around = c.open();
    c.holdRs(&d, D3DRS_SRCBLEND, D3DBLEND_ONE);
    const HookCalls::Scope reissue = c.open();
    c.holdRs(&d, D3DRS_COLORWRITEENABLE, 0xF);
    c.close(&d, reissue);
    CHECK(d.rs[D3DRS_SRCBLEND] == D3DBLEND_ONE && d.rs[D3DRS_COLORWRITEENABLE] == 0 && c.count == 1, "hook calls: the re-issue's close leaves the composite's blend held around it");
    c.close(&d, around);
    CHECK(d.rs[D3DRS_SRCBLEND] == 0 && c.count == 0, "hook calls: ...which its own close puts back");
    {
      HookCalls f; FakeDev e;
      const HookCalls::Scope s = f.open();
      for (uint32_t i = 0; i < HookCalls::kEntries + 3; ++i) f.holdRs(&e, (D3DRENDERSTATETYPE) i, 7);
      CHECK(f.count == HookCalls::kEntries && f.full == 3 && e.rs[HookCalls::kEntries] == 0 && e.rs[HookCalls::kEntries - 1] == 7,
            "hook calls: a full log leaves the state alone and counts it (%u)", f.full);
      f.close(&e, s);
      CHECK(e.rs[0] == 0 && e.rs[HookCalls::kEntries - 1] == 0, "hook calls: ...and still puts back all it held");
    }
    { OwnCall g(c); CHECK(c.own == 1, "hook calls: the guard counts the hook's own call while it lives"); }
    CHECK(c.own == 0, "hook calls: ...and no longer after");
  }

  printf("%d failure(s), %d skipped\n", fails, skips);
  return fails;
}

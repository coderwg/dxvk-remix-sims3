// The Sims 3 camera hook, part of d3d9_device.cpp: a draw's capture: the transforms the runtime
// holds, the statistics notes, the albedo stage and its coordinate, the vertex shader variants and
// the albedo's sampler states. Included after sims3_device_state.inl; not a standalone header.

// The lot terrain drawn again for another world chunk (milestone 16): the frame's first draw of the
// mesh was the whole lot already; the copy is baked as a hidden layer pass (h.lotFurtherCopy, milestone
// 17), so its chunk's paint reaches the terrain texture.
template<typename Dev>
void sims3LotTerrainCopy(Sims3Hook& h, Dev* dev, bool is3D) {
  h.lotFurtherCopy = false;
  if (!is3D || h.vsHash != sims3cam::kLotTerrainVs) return;
  IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0;
  if (SUCCEEDED(dev->GetStreamSource(0, &vb, &vbOffset, &vbStride)) && vb) {
    const uint64_t key = sims3cam::lotTerrainKey((uint64_t) (uintptr_t) vb, h.rows4to6);
    vb->Release();
    if (h.lotCopies.seen(key)) { h.lotFurtherCopy = true; ++h.terrainLotCopyDraws; }
  }
}

// Returns whether this draw is captured with the main camera, and holds the runtime's transforms
// accordingly: the main camera for a captured draw, else the identity (a draw the runtime
// rasterizes: into another target, or after the interface).
template<typename Dev>
bool sims3ApplyForDraw(Sims3Hook& h, Dev* dev, const DWORD* rs) {
  static const D3DMATRIX kIdentity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
  const DWORD zEnable = rs[D3DRS_ZENABLE];
  // a pass whose result Remix never shows (milestone 124): the shadow map, the sky's cube, the water's reflection
  const int unshown = sims3cam::unshownPass(h.rtIsPrimary, h.rt0W, h.rt0H, rs[D3DRS_COLORWRITEENABLE], h.rtCubeFace, h.rt0Id != 0 && h.rt0Id == h.reflectionRtId);
  if (unshown >= 0) {
    h.drawDropped = true; ++h.unshownDrops[unshown];
    return false;
  }
  // a reflection pass's draw (runs 66-68): dropped before it reaches the runtime; the ray tracer
  // renders reflections itself. Nothing is changed on the device for it.
  h.drawDropped = sims3cam::isReflectionDraw(h.camMirrored, h.declIs3D, zEnable);
  if (h.drawDropped) {
    ++h.reflectionDrops;
    if (h.reflectionFrame != h.frames) { h.reflectionFrame = h.frames; ++h.reflectionFrames; }
    return false;
  }
  D3DVIEWPORT9 vp = {}; dev->GetViewport(&vp);
  const bool fullViewport = h.rt0W == 0 || (vp.X == 0 && vp.Y == 0 && vp.Width == h.rt0W && vp.Height == h.rt0H);   // milestone 133
  const bool is3D = sims3cam::drawIs3D(h.cameraValid, h.declIs3D, zEnable, h.rtIsPrimary, fullViewport);
  // a draw the runtime must not have (milestones 148, 149, kDropPs): the game's own fakes -- its shadows, fog,
  // glow and tone curve, which Remix makes itself -- the world surfaces left out and the blended copies drawn in the world
  if (sims3cam::dropsDraw(h.psDrop, is3D, rs[D3DRS_ALPHABLENDENABLE])) {
    h.drawDropped = true; ++h.dropped[h.psDrop->kind];
    return false;
  }
  sims3LotTerrainCopy(h, dev, is3D);   // a lot chunk copy: baked hidden (milestone 16)
  const bool want = is3D;
  if (want) {
    if (h.held.kind != sims3cam::Kind::Main || !sims3cam::similarMatrix(h.held.view, h.cam.view, 1e-5f) || !sims3cam::similarMatrix(h.held.proj, h.cam.proj, 1e-5f)) {
      h.held.kind = sims3cam::Kind::Main; h.held.view = h.cam.view; h.held.proj = h.cam.proj;
      if (!h.loggedDraw3D) { h.loggedDraw3D = true; Logger::info("Sims 3 camera hook: first 3D draw with the main camera (depth test on, 3-component position, primary target)"); }
      ++h.transformSends;
      dev->SetTransform(D3DTS_VIEW, &h.cam.view);
      dev->SetTransform(D3DTS_PROJECTION, &h.cam.proj);
    }
  } else if (h.held.kind != sims3cam::Kind::None) {
    // back to the identity transforms (the game sets no transforms of its own): a draw the runtime
    // rasterizes, into another target or after the interface
    h.held.kind = sims3cam::Kind::None;
    if (!h.loggedDraw2D) {
      h.loggedDraw2D = true;
      char msg[160];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: first draw without the main camera (cameraValid=%d, declIs3D=%d, zEnable=%lu, primaryRT=%d)", (int) h.cameraValid, (int) h.declIs3D, (unsigned long) zEnable, (int) h.rtIsPrimary);
      Logger::info(msg);
    }
    dev->SetTransform(D3DTS_VIEW, &kIdentity);
    dev->SetTransform(D3DTS_PROJECTION, &kIdentity);
  }
  return want;
}

// Diagnostics: count captured draws per (vertex shader, pixel shader) pair.
inline void sims3NoteStates(Sims3Hook::ShaderStat& s, const DWORD* rs) {
  s.cull = (uint8_t) rs[D3DRS_CULLMODE]; s.blend = (uint8_t) (rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0);
  s.src = (uint8_t) rs[D3DRS_SRCBLEND]; s.dst = (uint8_t) rs[D3DRS_DESTBLEND];
  s.zw = (uint8_t) (rs[D3DRS_ZWRITEENABLE] ? 1 : 0); s.zf = (uint8_t) rs[D3DRS_ZFUNC];
  s.atest = (uint8_t) (rs[D3DRS_ALPHATESTENABLE] ? 1 : 0); s.cw = (uint8_t) (rs[D3DRS_COLORWRITEENABLE] & 0xF);
  s.stencil = (uint8_t) (rs[D3DRS_STENCILENABLE] ? 1 : 0);
}
inline void sims3NoteInstancing(Sims3Hook::ShaderStat& s, UINT freq0) {
  if (freq0 & D3DSTREAMSOURCE_INDEXEDDATA) {
    const uint32_t n = freq0 & 0x3FFFFFFFu;
    if (n > 1) { ++s.instDraws; if (n > s.instMax) s.instMax = (uint16_t) (n > 0xFFFF ? 0xFFFF : n); }
  }
}
inline void sims3NoteCapture(Sims3Hook& h, int stage, const DWORD* rs, UINT freq0) {
  for (int i = 0; i < h.shaderStatCount; ++i) {
    if (h.shaderStats[i].vs == h.vsHash && h.shaderStats[i].ps == h.psHash) { ++h.shaderStats[i].draws; h.shaderStats[i].stage = stage; sims3NoteStates(h.shaderStats[i], rs); sims3NoteInstancing(h.shaderStats[i], freq0); return; }
  }
  if (h.shaderStatCount < Sims3Hook::kShaderStats) {
    Sims3Hook::ShaderStat& s = h.shaderStats[h.shaderStatCount++];
    s.vs = h.vsHash; s.ps = h.psHash; s.draws = 1u; s.stage = stage; s.instDraws = 0; s.instMax = 0;
    for (int i = 0; i < 8; ++i) { s.kind[i] = h.boundKind[i]; s.fmt[i] = h.boundFmt[i]; s.w[i] = h.boundW[i]; s.h[i] = h.boundH[i]; }
    sims3NoteStates(s, rs);
    sims3NoteInstancing(s, freq0);
  }
}

// Diagnostics: a D3DFORMAT as text (a FOURCC as its letters, else the enum's known names).
inline const char* sims3FormatName(uint32_t f, char* buf, size_t n) {
  if (f >= 0x20202020u) {
    buf[0] = (char) (f & 0xff); buf[1] = (char) ((f >> 8) & 0xff); buf[2] = (char) ((f >> 16) & 0xff); buf[3] = (char) ((f >> 24) & 0xff); buf[4] = 0;
    return buf;
  }
  switch (f) {
    case 21: return "A8R8G8B8"; case 22: return "X8R8G8B8"; case 23: return "R5G6B5"; case 25: return "A1R5G5B5"; case 26: return "A4R4G4B4";
    case 28: return "A8"; case 32: return "A8B8G8R8"; case 50: return "L8"; case 51: return "A8L8"; case 60: return "V8U8"; case 63: return "Q8W8V8U8";
    case 71: return "D32"; case 75: return "D24S8"; case 77: return "D24X8"; case 80: return "D16"; case 111: return "R16F"; case 112: return "G16R16F";
    case 113: return "A16B16G16R16F"; case 114: return "R32F"; case 115: return "G32R32F"; case 116: return "A32B32G32R32F";
    default: snprintf(buf, n, "fmt%u", f); return buf;
  }
}

// Diagnostics: keep the bytecode of every shader the game creates (once per hash) next to
// the bridge logs, so the in-game shader set can be disassembled and tabled offline.
// <exe dir>\rtx-remix\logs\sims3-shaders, created on first use.
inline const char* sims3DumpDir() {
  static char base[MAX_PATH] = {};
  if (!base[0]) {
    GetModuleFileNameA(nullptr, base, MAX_PATH);
    char* p = strrchr(base, '\\'); if (p) *p = 0;
    strncat_s(base, "\\rtx-remix\\logs\\sims3-shaders", _TRUNCATE);
    CreateDirectoryA(base, nullptr);
  }
  return base;
}
inline void sims3DumpShader(const char* kind, uint64_t hash, const DWORD* tokens, size_t count) {
  if (!tokens || count == 0) return;
  char path[MAX_PATH + 64];
  snprintf(path, sizeof path, "%s\\%s_%016llx.bin", sims3DumpDir(), kind, (unsigned long long) hash);
  if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f) return;
  fwrite(tokens, sizeof(DWORD), count, f);
  fclose(f);
}

// The coordinate for the albedo at stage k (milestone 7): tc as chosen by chooseAutoAlbedo, or
// (tc < 0) the coordinate input the pixel shader's sampler k reads, from the bytecode analysis --
// for a TABLED pixel shader on an untabled vertex shader (run 41: a Sim outfit's permutation,
// flat grey from the raw SHORT2 input). A coordinate other than TEXCOORD0 asks for a promoted
// variant of the vertex shader for the draw (h.pendingPromote, bound by sims3BindVariant); either
// way the captured TEXCOORD0 is then what the pixel shader samples with (h.autoCapturedUv).
inline void sims3AutoTexcoord(Sims3Hook& h, int k, int tc, bool tabledPs) {
  if (tc < 0) {
    const sims3cam::PsSamplerUse& u = h.psAuto->samplers[k];
    if (!u.read || u.dependent || u.projective || u.cube || u.texcoord < 0) return;
    tc = u.texcoord;
    ++h.autoTexcoordDraws;
  }
  // the shader's own coordinate output is what its pixel shader samples with; a promotion is
  // bound by sims3BindVariant, which clears autoCapturedUv again if the variant cannot be made
  h.autoCapturedUv = true;
  if (tc > 0) h.pendingPromote = (uint8_t) tc;
}

// The normal the runtime should shade with, for the bound shaders (milestone 11): the register
// of the vertex shader's world-normal output (chooseNormalTexcoord: one the pixel shader reads,
// milestone 90; its own use of a coordinate as a normal breaks ties), 0xFF to hide the packed
// input normal instead (vs_2_x, or no such output), 0xFE when the shader has no normal input at all.
inline uint8_t sims3NormalChoice(const Sims3Hook& h) {
  if (!h.vsNormal || !h.vsNormal->valid || !h.vsNormal->hasNormalInput) return 0xFE;
  const bool psKnown = h.psAuto && h.psAuto->valid;
  const uint16_t ps = psKnown ? h.psAuto->normalTexcoords : (uint16_t) 0;
  const int tc = sims3cam::chooseNormalTexcoord(*h.vsNormal, ps, psKnown ? h.psAuto->inputTexcoords : (uint16_t) 0xFFFFu);
  if (tc >= 0) return h.vsNormal->outReg[tc];   // a vs_2_x candidate is an oT#, which convertVs2To3 keeps as o#
  return 0xFF;
}

// The variant of the bound vertex shader for this draw -- the promoted coordinate, the normal
// output, the c255 read and/or leaf cards faced outward from their tree (milestone 136) -- made
// once per (shader, coordinate, normal, read, outward) through the
// device and re-bound to the game's shader after the draw (sims3EndDraw). The game's shader is
// held by a reference of the hook's own until then: binding the variant drops the device
// state's reference to it.
template<typename Dev>
void sims3BindVariant(Sims3Hook& h, Dev* dev, uint8_t tc, uint8_t normalOut, bool constRead, bool outward) {
  if (!h.vsBound || (tc == 0 && normalOut == 0xFE && !constRead && !outward)) return;
  Sims3Hook::VsVariant* v = nullptr;
  for (uint32_t i = 0; i < h.vsVariantCount; ++i)
    if (h.vsVariants[i].base == h.vsBound && h.vsVariants[i].hash == h.vsHash && h.vsVariants[i].texcoord == tc && h.vsVariants[i].normalOut == normalOut && h.vsVariants[i].constRead == constRead
        && h.vsVariants[i].outward == outward) { v = &h.vsVariants[i]; break; }
  if (!v) {
    if (h.vsVariantCount >= Sims3Hook::kVsVariants) { ++h.vsVariantsFull; }
    else {
      v = &h.vsVariants[h.vsVariantCount++];
      *v = { h.vsBound, h.vsHash, tc, normalOut, constRead, outward, nullptr };
      UINT size = 0;
      if (SUCCEEDED(h.vsBound->GetFunction(nullptr, &size)) && size >= 8) {
        std::vector<DWORD> t(size / 4 + 1);
        if (SUCCEEDED(h.vsBound->GetFunction(t.data(), &size))) {
          t.resize(sims3cam::shaderTokenCount(t.data(), t.size()));
          bool good = t.size() >= 2, hidden = false, converted = false; uint32_t made = 0xFEu;
          // leaf cards faced outward from their tree (milestone 136), on the game's own bytecode
          if (good && outward) good = sims3cam::makeOutwardCards(t);
          // a vs_2_x shader cannot declare a NORMAL output: rewritten as vs_3_0 first (the
          // promotion then takes the vs_3_0 path on it)
          if (good && normalOut < 0xFE && h.vsNormal && h.vsNormal->version < 3) { converted = sims3cam::convertVs2To3(t); if (converted) ++h.vsConverted; else ++h.vsConvertFailed; }
          if (good && tc > 0) good = sims3cam::promoteTexcoord(t.data(), t.size(), tc) > 0;
          if (good && normalOut != 0xFE && h.vsNormal) {
            if (normalOut != 0xFF) made = sims3cam::makeNormalVariant(t, normalOut);
            if (normalOut == 0xFF || made == 0xFFu) { hidden = sims3cam::hideNormalInput(t, *h.vsNormal); good = hidden; made = 0xFEu; }
          }
          // a split instanced draw: the per-instance tag in c255 must be within the constants the
          // runtime hashes, which a dead read of c255 guarantees (see appendConstantRead)
          if (good && constRead) good = sims3cam::appendConstantRead(t, 255);
          if (good) {
            IDirect3DVertexShader9* shader = nullptr;
            HRESULT hr;
            { sims3cam::OwnCall ownCall(h.calls); hr = dev->CreateVertexShader(t.data(), &shader); }
            if (SUCCEEDED(hr) && shader) {
              v->variant = shader; ++h.autoVariantsMade;
              if (made != 0xFEu || hidden) ++h.normalVariantsMade;
            }
          }
        }
      }
    }
  }
  if (v && v->variant) {
    h.calls.holdVs(dev, v->variant);   // the game's shader back in sims3EndDraw
    if (v->normalOut < 0xFE) ++h.normalDraws; else if (v->normalOut == 0xFF) ++h.normalHiddenDraws;
  } else if (tc > 0) {
    h.autoCapturedUv = false;   // no promoted variant: the draw samples as declared
  }
}

// The sampler states a terrain draw's moved stage-0 texture keeps at its new stage (milestone 17).
inline constexpr D3DSAMPLERSTATETYPE kSims3SamplerCopy[Sims3Hook::kSamplerCopies] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE, D3DSAMP_MAXANISOTROPY };
// A texture moved to stage 0 takes its own stage's sampler states with it (milestone 68): the runtime
// samples stage 0's texture with stage 0's states -- the sRGB flag picks the decode, the addressing
// and the filters the sampling. Held on stage 0 in the current scope (the draw's or a section's).
template<typename Dev>
void sims3SamplerStatesTo0(Sims3Hook& h, Dev* dev, DWORD from) {
  for (int i = 0; i < Sims3Hook::kSamplerCopies; ++i) {
    DWORD v = 0; dev->GetSamplerState(from, kSims3SamplerCopy[i], &v);
    h.calls.holdSampler(dev, 0, kSims3SamplerCopy[i], v);
  }
}

// An untabled pixel shader's draw: the albedo stage from its bytecode and what is bound
// (chooseAutoAlbedo), then its coordinate. Returns the stage to present, or -1 when the
// bytecode names no candidate.
inline int sims3AutoAlbedo(Sims3Hook& h) {
  int k = -1, tc = -1;
  if (!sims3cam::chooseAutoAlbedo(*h.psAuto, h.boundColor2D, h.boundFmt, h.boundW, h.boundH, k, tc)) { ++h.autoNoAlbedo; return -1; }
  ++h.autoDraws;
  sims3AutoTexcoord(h, k, tc, false);
  return k;
}

// A material marker (milestones 80, 86): a flat 32x32 texture of the hook's own, the stage-0 texture of
// every glass (or water) draw. Its hash names a material in the hook's Remix mod, which the runtime
// reads from rtx-remix/mods while replacement assets are on.
template<typename Dev>
bool sims3EnsureMarker(Dev* dev, IDirect3DTexture9*& marker, bool& failed, uint32_t colour, uint64_t modHash, const char* what) {
  if (marker) return true;
  if (failed) return false;
  char msg[320];
  static uint32_t pixels[sims3cam::kGlassMarkerSize * sims3cam::kGlassMarkerSize];
  for (auto& p : pixels) p = colour;
  IDirect3DTexture9* tex = sims3MakeTexture(dev, pixels, sims3cam::kGlassMarkerSize, sims3cam::kGlassMarkerSize);
  if (!tex) { failed = true; snprintf(msg, sizeof msg, "Sims 3 camera hook: %s marker texture could not be made; its draws go out as before", what); Logger::info(msg); return false; }
  marker = tex;
  const uint64_t hash = (uint64_t) XXH3_64bits(pixels, sizeof pixels);   // the runtime hashes level 0's bytes, rows packed
  snprintf(msg, sizeof msg, "Sims 3 camera hook: %s marker created, hash 0x%016llX%s (the hook's Remix mods name mat_%016llX; replacement assets must be on)",
           what, (unsigned long long) hash, !modHash ? " -- no material: its flat colour shows" : hash == modHash ? "" : " -- NOT the mod's", (unsigned long long) modHash);
  Logger::info(msg);
  return true;
}
template<typename Dev>
bool sims3EnsureGlassMarker(Sims3Hook& h, Dev* dev, int m) {
  const sims3cam::GlassMaterial& g = sims3cam::kGlassMaterial[m];
  return sims3EnsureMarker(dev, h.glassMarkers[m], h.glassMarkerFailed[m], g.colour, g.hash, g.name);
}
template<typename Dev>
bool sims3EnsureMirrorMarker(Sims3Hook& h, Dev* dev) { return sims3EnsureMarker(dev, h.mirrorMarker, h.mirrorMarkerFailed, sims3cam::kMirrorMarkerColour, sims3cam::kMirrorMarkerHash, "mirror"); }
template<typename Dev>
bool sims3EnsureWaterMarker(Sims3Hook& h, Dev* dev, int m) {
  const sims3cam::WaterMaterial& w = sims3cam::kWaterMaterial[m];
  return sims3EnsureMarker(dev, h.waterMarkers[m], h.waterMarkerFailed[m], w.colour, w.hash, w.name);
}


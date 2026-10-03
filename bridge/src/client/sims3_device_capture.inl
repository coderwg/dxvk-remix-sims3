// The Sims 3 camera hook, part of d3d9_device.cpp: a draw's capture: the transforms the runtime
// holds, the statistics notes, the albedo stage and its coordinate, the vertex shader variants and
// the albedo's sampler states. Included after sims3_device_state.inl; not a standalone header.

// The lot terrain drawn again for another world chunk (milestone 16): the frame's first draw of the
// mesh was the whole lot already. With the terrain markers in place (milestone 17) the copy is baked
// as a hidden layer pass (h.lotFurtherCopy), so its chunk's paint reaches the terrain texture;
// without them it is dropped like a reflection pass's draw (true).
template<typename Dev>
bool sims3LotTerrainCopy(Sims3Hook& h, Dev* dev, bool is3D) {
  h.lotFurtherCopy = false;
  if (!is3D || h.vsHash != sims3cam::kLotTerrainVs) return false;
  IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0;
  if (SUCCEEDED(dev->GetStreamSource(0, &vb, &vbOffset, &vbStride)) && vb) {
    const uint64_t key = sims3cam::lotTerrainKey((uint64_t) (uintptr_t) vb, h.rows4to6);
    vb->Release();
    if (h.lotCopies.seen(key)) {
      if (h.marker[1] != nullptr && !h.markerFailed) {
        h.lotFurtherCopy = true; ++h.terrainLotCopyDraws;
      } else {
        h.drawDropped = true; h.dropWhy = "lot copy";
        ++h.lotCopyDrops;
        if (h.lotCopyLogged < 4) {
          ++h.lotCopyLogged; char msg[240];
          snprintf(msg, sizeof msg, "Sims 3 camera hook: lot terrain drawn again for another world chunk at frame %u -> dropped (mesh %p, World translation %.1f, %.1f, %.1f)", h.frames + 1, (void*) vb, h.rows4to6[3], h.rows4to6[7], h.rows4to6[11]);
          Logger::info(msg);
        }
        return true;
      }
    }
  }
  return false;
}

// Returns whether this draw is captured with the main camera, and holds the runtime's transforms
// accordingly: the main camera for a captured draw, else the identity (plain rasterization).
template<typename Dev>
bool sims3ApplyForDraw(Sims3Hook& h, Dev* dev, const DWORD* rs) {
  static const D3DMATRIX kIdentity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
  const DWORD zEnable = rs[D3DRS_ZENABLE], stencil = rs[D3DRS_STENCILENABLE], cull = rs[D3DRS_CULLMODE];
  // a pass whose result Remix never shows (milestone 124): the shadow map, the sky's cube, the water's reflection
  const int unshown = sims3cam::unshownPass(h.rtIsPrimary, h.rt0W, h.rt0H, rs[D3DRS_COLORWRITEENABLE], h.rtCubeFace, h.rt0Id != 0 && h.rt0Id == h.reflectionRtId);
  if (unshown >= 0) {
    static const char* const kWhy[sims3cam::kUnshownPasses] = { "shadow map", "sky cube", "water reflection" };
    h.drawDropped = true; h.dropWhy = kWhy[unshown]; ++h.unshownDrops[unshown];
    return false;
  }
  // a mirrored camera's pass is over at the first draw culling clockwise (milestone 85): the main
  // camera, which a mirrored upload never replaces, holds again
  if (sims3cam::endsMirroredPass(h.camMirrored, cull)) { h.camMirrored = false; h.cameraValid = h.cameraValidBeforeMirror; ++h.mirrorPassEnds; }
  // a reflection pass's draw (runs 66-68): dropped before it reaches the runtime; the ray tracer
  // renders reflections itself. Nothing is changed on the device for it.
  h.drawDropped = sims3cam::isReflectionDraw(h.camMirrored, h.declIs3D, zEnable, stencil, cull);
  h.dropWhy = h.drawDropped ? "reflection pass" : "";
  if (h.drawDropped) {
    ++h.reflectionDrops;
    if (!h.camMirrored) ++h.reflectionDropsByStates;
    if (h.reflectionFrame != h.frames) { h.reflectionFrame = h.frames; ++h.reflectionFrames; }
    if (h.reflectionLogged < 6) {
      ++h.reflectionLogged; char msg[240];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: reflection-pass draw dropped at frame %u -> VS %016llx PS %016llx (camera mirrored %d, stencil %lu, cull %lu, %s target)", h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash, (int) h.camMirrored, (unsigned long) stencil, (unsigned long) cull, h.rtIsPrimary ? "primary" : "offscreen");
      Logger::info(msg);
    }
    return false;
  }
  const bool is3D = sims3cam::drawIs3D(h.cameraValid, h.declIs3D, zEnable, h.rtIsPrimary);
  if (sims3LotTerrainCopy(h, dev, is3D)) return false;   // a lot chunk copy without the baker's markers: dropped (milestone 16)
  const bool want = is3D && !sims3cam::neverCaptureDraw(h.vsNeverCapture, rs[D3DRS_ALPHABLENDENABLE]);
  if (want) {
    if (h.held.kind != sims3cam::Kind::Main || !sims3cam::similarMatrix(h.held.view, h.cam.view, 1e-5f) || !sims3cam::similarMatrix(h.held.proj, h.cam.proj, 1e-5f)) {
      h.held.kind = sims3cam::Kind::Main; h.held.view = h.cam.view; h.held.proj = h.cam.proj;
      if (!h.loggedDraw3D) { h.loggedDraw3D = true; Logger::info("Sims 3 camera hook: first 3D draw with the main camera (depth test on, 3-component position, primary target)"); }
      ++h.transformSends;
      h.ourState = true;
      dev->SetTransform(D3DTS_VIEW, &h.cam.view);
      dev->SetTransform(D3DTS_PROJECTION, &h.cam.proj);
      h.ourState = false;
    }
  } else if (h.held.kind != sims3cam::Kind::None) {
    // back to the identity transforms: plain rasterization (the game sets no transforms of its own)
    h.held.kind = sims3cam::Kind::None;
    if (!h.loggedDraw2D) {
      h.loggedDraw2D = true;
      char msg[160];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: first draw without the main camera (cameraValid=%d, declIs3D=%d, zEnable=%lu, primaryRT=%d)", (int) h.cameraValid, (int) h.declIs3D, (unsigned long) zEnable, (int) h.rtIsPrimary);
      Logger::info(msg);
    }
    h.ourState = true;
    dev->SetTransform(D3DTS_VIEW, &kIdentity);
    dev->SetTransform(D3DTS_PROJECTION, &kIdentity);
    h.ourState = false;
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
  if (tc > 0 && !h.vsTabled) h.pendingPromote = (uint8_t) tc;
  if (h.autoLogged < 24) {
    ++h.autoLogged;
    char fb[16], msg[256];
    snprintf(msg, sizeof msg, "Sims 3 camera hook: auto %s -> VS %016llx PS %016llx: stage %d (%s %ux%u) at TEXCOORD%d%s", tabledPs ? "coordinate (tabled albedo, untabled vertex shader)" : "albedo",
             (unsigned long long) h.vsHash, (unsigned long long) h.psHash, k,
             sims3FormatName(h.boundFmt[k], fb, sizeof fb), (unsigned) h.boundW[k], (unsigned) h.boundH[k], tc,
             (tc > 0 && !h.vsTabled) ? ", promoted variant for the draw" : (tc > 0 ? ", vertex shader tabled" : ", captured TEXCOORD0"));
    Logger::info(msg);
  }
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
// output and/or the c255 read -- made once per (shader, coordinate, normal, read) through the
// device and re-bound to the game's shader after the draw (sims3EndDraw). The game's shader is
// held by a reference of the hook's own until then: binding the variant drops the device
// state's reference to it.
template<typename Dev>
void sims3BindVariant(Sims3Hook& h, Dev* dev, uint8_t tc, uint8_t normalOut, bool constRead) {
  if (!h.vsBound || (tc == 0 && normalOut == 0xFE && !constRead)) return;
  Sims3Hook::VsVariant* v = nullptr;
  for (uint32_t i = 0; i < h.vsVariantCount; ++i)
    if (h.vsVariants[i].base == h.vsBound && h.vsVariants[i].hash == h.vsHash && h.vsVariants[i].texcoord == tc && h.vsVariants[i].normalOut == normalOut && h.vsVariants[i].constRead == constRead) { v = &h.vsVariants[i]; break; }
  if (!v) {
    if (h.vsVariantCount >= Sims3Hook::kVsVariants) { ++h.vsVariantsFull; }
    else {
      v = &h.vsVariants[h.vsVariantCount++];
      *v = { h.vsBound, h.vsHash, tc, normalOut, constRead, nullptr };
      UINT size = 0;
      if (SUCCEEDED(h.vsBound->GetFunction(nullptr, &size)) && size >= 8) {
        std::vector<DWORD> t(size / 4 + 1);
        if (SUCCEEDED(h.vsBound->GetFunction(t.data(), &size))) {
          t.resize(sims3cam::shaderTokenCount(t.data(), t.size()));
          bool good = t.size() >= 2, hidden = false, converted = false; uint32_t made = 0xFEu;
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
            h.creatingVariant = true;
            const HRESULT hr = dev->CreateVertexShader(t.data(), &shader);
            h.creatingVariant = false;
            if (SUCCEEDED(hr) && shader) {
              v->variant = shader; ++h.autoVariantsMade;
              if (made != 0xFEu || hidden) ++h.normalVariantsMade;
              if (h.normalLogged < 16) {
                ++h.normalLogged;
                char what[96], msg[224];
                if (made != 0xFEu) snprintf(what, sizeof what, "world normal (o%u) repeated into NORMAL output o%u", (unsigned) normalOut, (unsigned) made);
                else if (hidden) snprintf(what, sizeof what, "packed normal input hidden from the capture (triangle normals)");
                else snprintf(what, sizeof what, "no normal change");
                snprintf(msg, sizeof msg, "Sims 3 camera hook: shader variant for VS %016llx -> %s%s%s%s", (unsigned long long) h.vsHash, converted ? "vs_2_0 rewritten as vs_3_0, " : "", tc > 0 ? "texcoord promoted, " : "", what, constRead ? ", c255 read for the per-instance tag" : "");
                Logger::info(msg);
              }
            }
          }
        }
      }
    }
  }
  if (v && v->variant) {
    h.autoVsRestore = h.vsBound; h.autoVsRestore->AddRef();
    h.swappingVs = true; dev->SetVertexShader(v->variant); h.swappingVs = false;
    if (v->normalOut < 0xFE) ++h.normalDraws; else if (v->normalOut == 0xFF) ++h.normalHiddenDraws;
  } else if (tc > 0) {
    h.autoCapturedUv = false;   // no promoted variant: the draw samples as declared
  }
}

// The sampler states a terrain draw's moved stage-0 texture keeps at its new stage (milestone 17).
inline constexpr D3DSAMPLERSTATETYPE kSims3SamplerCopy[Sims3Hook::kSamplerCopies] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE, D3DSAMP_MAXANISOTROPY };
// A texture moved to stage 0 takes its own stage's sampler states with it (milestone 68): the runtime
// samples stage 0's texture with stage 0's states -- the sRGB flag picks the decode, the addressing
// and the filters the sampling. The states that differ are set on stage 0, their stage-0 values kept
// in saved; the mask returned says which, for sims3SamplerStatesBack.
template<typename Dev>
uint8_t sims3SamplerStatesTo0(Sims3Hook& h, Dev* dev, DWORD from, DWORD* saved) {
  uint8_t set = 0;
  h.ourSampler = true;
  for (int i = 0; i < Sims3Hook::kSamplerCopies; ++i) {
    DWORD v0 = 0, vf = 0;
    dev->GetSamplerState(0, kSims3SamplerCopy[i], &v0); dev->GetSamplerState(from, kSims3SamplerCopy[i], &vf);
    if (v0 == vf) continue;
    saved[i] = v0; set |= (uint8_t) (1u << i);
    dev->SetSamplerState(0, kSims3SamplerCopy[i], vf);
  }
  h.ourSampler = false;
  return set;
}
template<typename Dev>
void sims3SamplerStatesBack(Sims3Hook& h, Dev* dev, const DWORD* saved, uint8_t set) {
  if (!set) return;
  h.ourSampler = true;
  for (int i = 0; i < Sims3Hook::kSamplerCopies; ++i) if (set & (1u << i)) dev->SetSamplerState(0, kSims3SamplerCopy[i], saved[i]);
  h.ourSampler = false;
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
bool sims3EnsureMarker(Dev* dev, IDirect3DTexture9*& marker, uint64_t& hash, bool& failed, uint32_t colour, uint64_t modHash, const char* what) {
  if (marker) return true;
  if (failed) return false;
  char msg[320];
  IDirect3DTexture9* tex = nullptr;
  if (FAILED(dev->CreateTexture(sims3cam::kGlassMarkerSize, sims3cam::kGlassMarkerSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || tex == nullptr) {
    failed = true; snprintf(msg, sizeof msg, "Sims 3 camera hook: %s marker texture could not be created; its draws go out as before", what); Logger::info(msg); return false;
  }
  static uint32_t pixels[sims3cam::kGlassMarkerSize * sims3cam::kGlassMarkerSize];
  for (auto& p : pixels) p = colour;
  D3DLOCKED_RECT lr = {};
  if (FAILED(tex->LockRect(0, &lr, nullptr, 0)) || lr.pBits == nullptr) {
    tex->Release(); failed = true; snprintf(msg, sizeof msg, "Sims 3 camera hook: %s marker texture could not be written; its draws go out as before", what); Logger::info(msg); return false;
  }
  for (uint32_t y = 0; y < sims3cam::kGlassMarkerSize; ++y) memcpy((uint8_t*) lr.pBits + (size_t) y * lr.Pitch, pixels + y * sims3cam::kGlassMarkerSize, sims3cam::kGlassMarkerSize * 4);
  tex->UnlockRect(0);
  marker = tex;
#if SIMS3_HAVE_XXHASH
  hash = (uint64_t) XXH3_64bits(pixels, sizeof pixels);   // the runtime hashes level 0's bytes, rows packed
#endif
  snprintf(msg, sizeof msg, "Sims 3 camera hook: %s marker created, hash 0x%016llX%s (the hook's Remix mods name mat_%016llX; replacement assets must be on)",
           what, (unsigned long long) hash, !modHash ? " -- no material: its flat colour shows" : hash == modHash ? "" : " -- NOT the mod's", (unsigned long long) modHash);
  Logger::info(msg);
  return true;
}
template<typename Dev>
bool sims3EnsureGlassMarker(Sims3Hook& h, Dev* dev, int m) {
  const sims3cam::GlassMaterial& g = sims3cam::kGlassMaterial[m];
  return sims3EnsureMarker(dev, h.glassMarkers[m], h.glassMarkerHashes[m], h.glassMarkerFailed[m], g.colour, g.hash, g.name);
}
template<typename Dev>
bool sims3EnsureMirrorMarker(Sims3Hook& h, Dev* dev) { return sims3EnsureMarker(dev, h.mirrorMarker, h.mirrorMarkerHash, h.mirrorMarkerFailed, sims3cam::kMirrorMarkerColour, sims3cam::kMirrorMarkerHash, "mirror"); }
// The glass survey's marker for the bound pixel shader (milestone 110; one run), or nullptr when it is
// not surveyed; made on its first draw, the log naming its colour, shader and frame.
template<typename Dev>
IDirect3DTexture9* sims3SurveyMarker(Sims3Hook& h, Dev* dev) {
  const sims3cam::GlassSurvey* s = sims3cam::glassSurvey(h.psHash);
  if (!s) return nullptr;
  const size_t i = (size_t) (s - sims3cam::kGlassSurvey);
  if (!h.surveyMarkers[i] && !h.surveyFailed[i]) {
    char what[160];
    snprintf(what, sizeof what, "glass survey: %s = PS %016llx (first drawn at frame %u, VS %016llx);", s->colourName, (unsigned long long) h.psHash, h.frames + 1, (unsigned long long) h.vsHash);
    sims3EnsureMarker(dev, h.surveyMarkers[i], h.surveyHashes[i], h.surveyFailed[i], s->colour, 0, what);
  }
  if (h.surveyMarkers[i]) ++h.surveyDraws;
  return h.surveyMarkers[i];
}
template<typename Dev>
bool sims3EnsureWaterMarker(Sims3Hook& h, Dev* dev, int m) {
  const sims3cam::WaterMaterial& w = sims3cam::kWaterMaterial[m];
  return sims3EnsureMarker(dev, h.waterMarkers[m], h.waterMarkerHashes[m], h.waterMarkerFailed[m], w.colour, w.hash, w.name);
}


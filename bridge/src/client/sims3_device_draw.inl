// The Sims 3 camera hook, part of d3d9_device.cpp: the device reset, and the hook around every draw
// of the game (sims3BeginDraw, sims3EndDraw). Included after sims3_device_walls.inl, last in the
// first anonymous namespace; not a standalone header.

// The device reset (and its destruction): every object the hook made or holds a reference to is
// released, the bound-shader and bound-texture facts are forgotten (the device state is back to
// defaults; the game's shaders themselves survive a Reset), and the runtime is back to default state.
inline void sims3OnReset(Sims3Hook& h) {
  for (auto& s : h.squares) if (s.merged) s.merged->Release();   // the squares' merged shapes (milestone 60)
  h.squares.clear(); h.mergePending = -1;
  for (auto& e : h.plates) {   // the low-detail lots' split, glow layer and its textures (milestones 69-71)
    if (e.house) e.house->Release(); if (e.plate) e.plate->Release(); if (e.glow) e.glow->Release(); if (e.glowVb) e.glowVb->Release();
  }
  h.plates.clear();
  for (auto& g : h.glowTexs) if (g.tex) g.tex->Release();
  h.glowTexs.clear();
  if (h.platePs) { h.platePs->Release(); h.platePs = nullptr; }
  h.platePsFailed = false;
  for (uint32_t i = 0; i < h.wallCacheCount; ++i) sims3ReleaseWallEntry(h.wallCache[i]);
  h.wallCacheCount = 0; for (auto& m : h.masks) m = Sims3Hook::MaskEntry(); h.maskUse = 0;
  for (auto& e : h.bufHashes) e = Sims3Hook::HashEntry(); h.bufHashUse = 0;
  h.vsWall = nullptr; h.wallLayout = sims3cam::WallLayout(); h.wallDeclId = 0;
  for (uint32_t i = 0; i < h.vsVariantCount; ++i) if (h.vsVariants[i].variant) h.vsVariants[i].variant->Release();
  h.vsVariantCount = 0;
  if (h.autoVsRestore) { h.autoVsRestore->Release(); h.autoVsRestore = nullptr; }
  if (h.remapRestore) { h.remapRestore->Release(); h.remapRestore = nullptr; }
  // the terrain baker's markers and pixel shader variants (milestone 17)
  for (uint32_t i = 0; i < h.psVariantCount; ++i) if (h.psVariants[i].variant) h.psVariants[i].variant->Release();
  h.psVariantCount = 0;
  if (h.psRestore) { h.psRestore->Release(); h.psRestore = nullptr; }
  if (h.freeStageRestore) { h.freeStageRestore->Release(); h.freeStageRestore = nullptr; }
  for (int i = 0; i < sims3cam::kTerrainMarkers; ++i) { if (h.marker[i]) h.marker[i]->Release(); h.marker[i] = nullptr; h.markerHash[i] = 0; }
  h.markerFailed = false; h.markersConfigSent = false; h.terrainFreeStage = -1; h.tblockActive = false; h.tblockStage = -1; h.tblockSet = 0; h.tblockSrgb = 0; h.ourSampler = false; h.psBound = nullptr; h.vsTerrain = nullptr; h.lotFurtherCopy = false; h.swappingPs = false;
  h.cwOurs = false; h.atOurs = false; h.fogOurs = false;
  h.compositePass = 0; h.extraActive = false; h.splitDraw = false; h.ourConsts = false; h.reissue = false; h.reissueKind = 0; h.compositeSecond = false;
  for (int i = 0; i < 2; ++i) { if (h.extraRestore[i]) h.extraRestore[i]->Release(); h.extraRestore[i] = nullptr; }
  h.remapActive = false; h.maskEmu = 0; h.viewportOurs = false; h.vsSkyDome = false;
  h.vsBound = nullptr; h.vsTabled = false; h.vsNormal = nullptr; h.pendingPromote = 0; h.vsHash = 0;
  h.patch = nullptr; h.vsNeverCapture = 0; h.vsCapturedUv = false;
  h.lotCopies.clear();
  h.psAuto = nullptr; h.psAlbedoStage = -1; h.psTintReg = -1; h.psHash = 0;
  h.declIs3D = false; h.drawCaptured = false; h.autoCapturedUv = false;
  for (int i = 0; i < 16; ++i) { h.boundTex[i] = nullptr; h.boundColor2D[i] = false; h.boundKind[i] = 0; h.boundFmt[i] = 0; h.boundW[i] = h.boundH[i] = 0; }
  for (uint32_t i = 0; i < h.scratchCount; ++i) { if (h.scratch[i].surf) h.scratch[i].surf->Release(); if (h.scratch[i].tex) h.scratch[i].tex->Release(); h.scratch[i] = Sims3Hook::Scratch(); }
  h.scratchCount = 0; h.copyScratch = -1; h.ourDraw = false; h.rt0W = h.rt0H = 0; h.rt0Fmt = 0;
  h.held = sims3cam::Held(); h.cameraValid = false; h.camMirrored = false; h.drawDropped = false; h.rtIsPrimary = true;
  h.frameCamSet = false;
  h.tssOurs = 0; h.uvIndexHidden = false; h.factorOurs = false; h.sentFactor = 0xFFFFFFFFu; h.gameFactor = 0xFFFFFFFFu;
  h.gameTss0[0] = D3DTOP_MODULATE; h.gameTss0[1] = D3DTA_TEXTURE; h.gameTss0[2] = D3DTA_CURRENT; h.gameTss0[3] = 0;
  h.gameXformSet[0] = h.gameXformSet[1] = false;
  h.sunSet = false; h.moonSet = false; h.terrainSunDraws = 0;
  for (uint32_t k = 0; k < h.lamps.n; ++k) { sims3cam::Lamp& Lr = h.lamps.lamps[k]; Lr.sent = false; Lr.api = Lr.api2 = Lr.api3 = nullptr; }   // the runtime's lights are gone with the device; the lamps are re-sent
  for (bool& r : h.rsSet) r = false;
  Logger::info("Sims 3 camera hook: device reset -> the hook's objects released, held state cleared");
}

// Before every draw of the game (not the hook's own restore quad). A captured draw -- the main
// camera held, see sims3ApplyForDraw -- gets: its albedo presented as stage 0 when the game bound
// a cube map / render target there (Remix would drop the draw), the vertex shader variant for
// the draw (promoted coordinate, world normal, c255 read), the raw texcoord set hidden for the
// families whose shader output is verified, its tint as the texture factor, and its shader's
// cut-out as an alpha test (milestones 67-68). Any other draw gets the game's own state back, and a masked write
// into an offscreen target its emulation. Returns whether the draw is captured.
template<typename Dev>
bool sims3BeginDraw(Sims3Hook& h, Dev* dev, const DWORD* rs, UINT freq0) {
  if (h.mergePending >= 0 && !h.reissue) {   // a square's shape that was not sent after its piece (the piece took another way out): next piece then
    if ((size_t) h.mergePending < h.squares.size()) h.squares[(size_t) h.mergePending].mergedFrame = 0xFFFFFFFFu;
    h.mergePending = -1; ++h.mergeSkipped;
  }
  const bool want = sims3ApplyForDraw(h, dev, rs);
  h.drawCaptured = want;
  if (h.drawDropped) return false;                // a reflection pass's draw: the caller returns without drawing
  if (!want) {
    sims3TerrainBlockEndIfUsed(h, dev);      // an uncaptured draw follows the terrain block: ends it only if it samples a held stage (milestone 18h)
    sims3RestoreGameState(h, dev);
    sims3BeginMaskedWrite(h, dev, rs);
    return false;
  }
  // the neighbourhood view's lot picture (milestone 63): a second surface over the lot's ground, left out
  if (h.vsHash == sims3cam::kLotPictureVs) { h.drawDropped = true; h.drawCaptured = false; ++h.lotPictureDropped; return false; }
  int k = -1;
  ++h.capturedDraws;
  if (rs[D3DRS_FOGENABLE] && (rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE || rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE)) ++h.gameFogDraws;   // a fog state of the game's own (milestone 56: none expected)
  h.autoCapturedUv = false; h.pendingPromote = 0;
  // a terrain draw (milestone 17): handed to the runtime's terrain baker with the marker at
  // stage 0 and the game's pixel shader variant; no albedo stage, no vertex shader variant
  uint8_t terrainKind = sims3cam::terrainDrawKind(h.vsTerrain, rs[D3DRS_ALPHABLENDENABLE], h.lotFurtherCopy);
  const bool lotFamilyDraw = terrainKind != 0 && h.vsTerrain && h.vsTerrain->lotFamily;
  // the town ground's squares (milestones 60, 63; the detailed pieces and the coarse squares alike):
  // an opaque piece of a square whose merged shape is ready only paints (3); the shape follows it
  // (sims3MergedSquareDraw)
  if (terrainKind == 1 && !h.reissue && !lotFamilyDraw) terrainKind = sims3SquarePiece(h, dev);
  if (terrainKind != 0 && h.reissue) terrainKind = h.reissueKind ? h.reissueKind : 2;   // the hook's own re-issue: the composite's second pass (hidden), a square's merged shape (1)
  h.compositeSecond = false;
  // a lot's ground goes out in place (milestone 19): nothing overwrites its paint in the atlas (run 119's paint test)
  const bool terrain = terrainKind != 0 && sims3BeginTerrainDraw(h, dev, terrainKind);
  if (!terrain) sims3TerrainBlockEnd(h, dev);   // a captured non-terrain draw follows the terrain block (milestone 18g)
  // the game's fog for the runtime (milestone 56): on the frame's first base terrain draws (drawn in
  // every view; their pixel shaders are 3.0, which fixed-function fog leaves alone, so the bake is
  // untouched), as D3D9 linear fog; the game's states are put back after the draw (sims3EndDraw)
  if (terrain && terrainKind == 1 && h.fogReady && h.psMajor >= 3 && h.fogFrameDraws < 4u) {
    ++h.fogFrameDraws; ++h.fogDraws;
    DWORD start = 0, end = 0; std::memcpy(&start, &h.fogStart, 4); std::memcpy(&end, &h.fogEnd, 4);
    const DWORD ours[5] = { TRUE, D3DFOG_LINEAR, h.fogColour, start, end };
    for (int i = 0; i < 5; ++i) dev->GetRenderState(kSims3FogRs[i], &h.fogSaved[i]);
    h.ourState = true; for (int i = 0; i < 5; ++i) dev->SetRenderState(kSims3FogRs[i], ours[i]); h.ourState = false;
    h.fogOurs = true;
  }
  if (!terrain) {
    if (h.psAlbedoStage >= 0 && h.psAlbedoStage < 16 && h.boundTex[h.psAlbedoStage] != nullptr) {
      k = h.psAlbedoStage; ++h.overrideDraws;
      // on an untabled vertex shader (a Sim outfit's permutation, run 41) the coordinate the albedo
      // sampler reads, from the bytecode, decides the promotion and the captured UV
      if (!h.vsTabled && h.vsBound && h.psAuto && h.psAuto->valid) sims3AutoTexcoord(h, k, -1, true);
    } else {
      // an untabled pixel shader: the albedo from its bytecode (a promoted vertex-shader variant when needed)
      if (h.psAuto && h.psAuto->valid && h.vsBound) k = sims3AutoAlbedo(h);
      if (k < 0) { bool bound[16]; for (int i = 0; i < 16; ++i) bound[i] = h.boundTex[i] != nullptr; k = sims3cam::pickAlbedoStage(bound, h.boundColor2D); }
    }
    // a cut-out the runtime must see (milestones 67-68): the shader's texkill on its albedo's alpha,
    // a * alpha + b with the bound constants, as the D3D alpha test the runtime applies to the albedo
    if (k >= 0 && h.psAuto && h.psAuto->valid && h.psAuto->cutSampler == k && !rs[D3DRS_ALPHATESTENABLE] && !h.atOurs) {
      auto get = [&](uint32_t reg, uint32_t comp) -> float { float v[4] = {}; dev->GetPixelShaderConstantF(reg, v, 1); return v[comp & 3u]; };
      const float a = sims3cam::cutEval(h.psAuto->cutA, get), b = sims3cam::cutEval(h.psAuto->cutB, get);
      uint32_t ref = 0; const uint32_t func = sims3cam::cutAlphaTest(a, b, ref);
      if (func) {
        dev->GetRenderState(D3DRS_ALPHATESTENABLE, &h.atSaved[0]); dev->GetRenderState(D3DRS_ALPHAFUNC, &h.atSaved[1]); dev->GetRenderState(D3DRS_ALPHAREF, &h.atSaved[2]);
        h.atOurs = true; h.ourState = true;
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE); dev->SetRenderState(D3DRS_ALPHAFUNC, func); dev->SetRenderState(D3DRS_ALPHAREF, ref);
        h.ourState = false;
        ++h.alphaCutDraws;
      }
      bool seen = false; for (uint32_t i = 0; i < h.cutLogged; ++i) if (h.cutLoggedPs[i] == h.psHash) seen = true;
      if (!seen && h.cutLogged < 32) {
        h.cutLoggedPs[h.cutLogged++] = h.psHash; char msg[256];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: cut-out of PS %016llx (VS %016llx): texkill on the alpha of s%d, %.3f * alpha + %.3f -> %s %u",
                 (unsigned long long) h.psHash, (unsigned long long) h.vsHash, k, a, b,
                 func == D3DCMP_GREATEREQUAL ? "alpha test >=" : func == D3DCMP_LESSEQUAL ? "alpha test <=" : func == D3DCMP_NEVER ? "never drawn" : "nothing discarded, no test", ref);
        Logger::info(msg);
      }
    }
    // the vertex shader variant for the draw: the promoted coordinate and/or the world normal as a
    // NORMAL output, and on a hardware-instanced draw (split per instance) a read of c255 for the tag
    sims3BindVariant(h, dev, h.pendingPromote, sims3NormalChoice(h), (freq0 & D3DSTREAMSOURCE_INDEXEDDATA) && (freq0 & 0x3FFFFFFFu) > 1u);
  }
  if (k >= 0 && k < 16) ++h.remapCount[k];
  sims3NoteCapture(h, k, rs, freq0);
  // texture coordinates: hide the raw input set from the runtime (stage 0 index -> 7) for the
  // families whose shader output is verified, so it samples with the captured TEXCOORD0
  const bool wantCaptured = h.vsCapturedUv || h.autoCapturedUv;
  h.ourState = true;
  if (wantCaptured != h.uvIndexHidden) {
    h.uvIndexHidden = wantCaptured;
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, h.uvIndexHidden ? 7u : h.gameTss0[3]);
    if (h.uvIndexHidden && !h.loggedCapturedUv) { h.loggedCapturedUv = true; Logger::info("Sims 3 camera hook: first draw sampling with the shader's captured texture coordinates (stage 0 texcoord index 7 hides the raw input set)"); }
  }
  // Create-A-Style tint: albedo x TEXTUREFACTOR (white when the shader has no tint); each of the
  // three stage-0 states the game has written since is set again
  for (int i = 0; i < 3; ++i) if (!(h.tssOurs & (1u << i))) { h.tssOurs |= (uint8_t) (1u << i); dev->SetTextureStageState(0, kSims3Tss[i], kSims3TssOurs[i]); }
  const uint32_t factor = (h.psTintReg >= 0) ? sims3cam::packTint(h.tint) : 0xFFFFFFFFu;
  if (!h.factorOurs || factor != h.sentFactor) {
    h.sentFactor = factor; h.factorOurs = true;
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, factor);
    if (!h.loggedTint && factor != 0xFFFFFFFFu) { h.loggedTint = true; char msg[160]; snprintf(msg, sizeof msg, "Sims 3 camera hook: first non-white tint forwarded as texture factor: %08X", factor); Logger::info(msg); }
  }
  h.ourState = false;
  // the sky dome: any 2D texture at stage 0 (the runtime drops a draw whose stage-0 texture has
  // no hash, and a cube map has none), and a depth-1 viewport, which makes the draw the sky
  if (h.vsSkyDome) {
    if (k < 0 && (h.boundKind[0] & 0x7F) != 1) for (int s = 1; s < 16; ++s) if ((h.boundKind[s] & 0x7F) == 1 && h.boundTex[s]) { k = s; break; }
    dev->GetViewport(&h.gameViewport);
    D3DVIEWPORT9 vp = h.gameViewport; vp.MinZ = 1.f; vp.MaxZ = 1.f;
    h.ourState = true; dev->SetViewport(&vp); h.ourState = false;
    h.viewportOurs = true; ++h.skyDraws;
    if (h.skyLogged < 3) {
      ++h.skyLogged; char fb[16]; char msg[288];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: sky dome draw presented as the runtime's sky at frame %u -> VS %016llx PS %016llx, viewport depth 1, stage 0 %s (%s %ux%u)", h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash, k > 0 ? "presented from stage" : "as bound", k > 0 ? sims3FormatName(h.boundFmt[k], fb, sizeof fb) : sims3FormatName(h.boundFmt[0], fb, sizeof fb), (unsigned) h.boundW[k > 0 ? k : 0], (unsigned) h.boundH[k > 0 ? k : 0]);
      if (k > 0) { const size_t l = strlen(msg); snprintf(msg + l, sizeof msg - l, " s%d", k); }
      Logger::info(msg);
    }
  }
  if (k > 0) {
    if (!h.loggedRemap) { h.loggedRemap = true; char msg[192]; snprintf(msg, sizeof msg, "Sims 3 camera hook: first captured draw whose albedo is not at stage 0; presenting stage %d as the albedo for such draws", k); Logger::info(msg); }
    h.remapRestore = h.boundTex[0]; if (h.remapRestore) h.remapRestore->AddRef();   // held until sims3EndDraw: the remap drops the state's reference
    h.remapActive = true;
    h.inRemap = true; dev->SetTexture(0, h.boundTex[k]); h.inRemap = false;
    // its sampler states go with it (milestone 68); the game's stage 0 states come back in sims3EndDraw
    h.remapSamplerSet = sims3SamplerStatesTo0(h, dev, (DWORD) k, h.remapSamplerSaved);
    if (h.remapSamplerSet) {
      ++h.remapSamplerDraws; if (h.remapSamplerSet & (1u << 5)) ++h.remapSrgbDraws;
      bool seen = false; for (uint32_t i = 0; i < h.remapSamplerLogged; ++i) if (h.remapSamplerLoggedPs[i] == h.psHash) seen = true;
      if (!seen && h.remapSamplerLogged < 24) {
        h.remapSamplerLoggedPs[h.remapSamplerLogged++] = h.psHash;
        static const char* const kNames[Sims3Hook::kSamplerCopies] = { "addressU", "addressV", "mag", "min", "mip", "sRGB", "anisotropy" };
        char msg[512]; int n = snprintf(msg, sizeof msg, "Sims 3 camera hook: albedo at stage %d keeps its sampler states at stage 0 for VS %016llx PS %016llx:", k, (unsigned long long) h.vsHash, (unsigned long long) h.psHash);
        for (int i = 0; i < Sims3Hook::kSamplerCopies && n > 0 && n < (int) sizeof msg; ++i) {
          if (!(h.remapSamplerSet & (1u << i))) continue;
          DWORD vk = 0; dev->GetSamplerState(0, kSims3SamplerCopy[i], &vk);
          n += snprintf(msg + n, sizeof msg - n, " %s %lu (stage 0 had %lu)", kNames[i], (unsigned long) vk, (unsigned long) h.remapSamplerSaved[i]);
        }
        Logger::info(msg);
      }
    }
  }
  return true;
}

// After the draw's message has been queued: the game's stage-0 texture and vertex shader back,
// the masked-write emulation undone.
template<typename Dev>
void sims3EndDraw(Sims3Hook& h, Dev* dev) {
  if (h.fogOurs) { h.fogOurs = false; h.ourState = true; for (int i = 0; i < 5; ++i) dev->SetRenderState(kSims3FogRs[i], h.fogSaved[i]); h.ourState = false; }
  if (h.viewportOurs) { h.viewportOurs = false; h.ourState = true; dev->SetViewport(&h.gameViewport); h.ourState = false; }
  // a terrain draw's pixel shader variant, moved texture and sampler states (milestone 17)
  if (h.psRestore) {
    IDirect3DPixelShader9* ps = h.psRestore; h.psRestore = nullptr;
    h.swappingPs = true; dev->SetPixelShader(ps); h.swappingPs = false;
    ps->Release();
  }
  if (h.cwOurs) { h.cwOurs = false; h.ourState = true; dev->SetRenderState(D3DRS_COLORWRITEENABLE, h.cwRestore); h.ourState = false; }
  if (h.atOurs) {
    h.atOurs = false; h.ourState = true;
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, h.atSaved[0]); dev->SetRenderState(D3DRS_ALPHAFUNC, h.atSaved[1]); dev->SetRenderState(D3DRS_ALPHAREF, h.atSaved[2]);
    h.ourState = false;
  }
  if (h.terrainFreeStage >= 0) {
    h.inRemap = true; dev->SetTexture((DWORD) h.terrainFreeStage, h.freeStageRestore); h.inRemap = false;
    if (h.freeStageRestore) { h.freeStageRestore->Release(); h.freeStageRestore = nullptr; }
    h.terrainFreeStage = -1;
  }
  if (h.extraActive) {   // the composite's second pass: stages 1 and 2 back (milestone 17l)
    h.extraActive = false; h.inRemap = true;
    for (int s = 0; s < 2; ++s) { dev->SetTexture((DWORD) (s + 1), h.extraRestore[s]); if (h.extraRestore[s]) { h.extraRestore[s]->Release(); h.extraRestore[s] = nullptr; } }
    h.inRemap = false;
  }
  h.splitDraw = false;
  if (h.remapActive) {
    h.remapActive = false;
    h.inRemap = true; dev->SetTexture(0, h.remapRestore); h.inRemap = false;
    if (h.remapRestore) { h.remapRestore->Release(); h.remapRestore = nullptr; }
  }
  sims3SamplerStatesBack(h, dev, h.remapSamplerSaved, h.remapSamplerSet);   // the game's stage 0 sampler states back (milestone 68)
  h.remapSamplerSet = 0;
  if (h.autoVsRestore) {
    IDirect3DVertexShader9* base = h.autoVsRestore; h.autoVsRestore = nullptr;
    h.swappingVs = true; dev->SetVertexShader(base); h.swappingVs = false;
    base->Release();
  }
  if (h.maskEmu) {
    if (h.maskEmu == 3) sims3RestoreChannels(h, dev);
    h.ourState = true;
    for (int i = 0; i < 10; ++i) if (h.maskEmuBits & (1u << i)) dev->SetRenderState(kSims3MaskRs[i], h.maskSaved[i]);
    h.ourState = false; h.maskEmu = 0;
  }
}

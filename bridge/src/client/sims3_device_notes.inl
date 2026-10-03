// The Sims 3 camera hook, part of d3d9_device.cpp: the facts about the bound objects (milestone
// 17w), recorded by the setters. Included after sims3_device_lots.inl, last in the second anonymous
// namespace; not a standalone header.

// The hook's facts about the bound objects (milestone 17w): what the setters recorded inline before.
void sims3NoteVertexShader(Sims3Hook& h, IDirect3DVertexShader9* pShader) {
  auto* const pLssVertexShader = bridge_cast<Direct3DVertexShader9_LSS*>(pShader);
  h.patch = pLssVertexShader ? pLssVertexShader->sims3Patch : nullptr;
  h.vsNeverCapture = pLssVertexShader ? pLssVertexShader->sims3NeverCapture : 0;
  h.vsSkyDome = pLssVertexShader ? pLssVertexShader->sims3SkyDome : false;
  h.vsHash = pLssVertexShader ? pLssVertexShader->sims3Hash : 0;
  h.vsCapturedUv = pLssVertexShader ? sims3cam::useCapturedUv(pLssVertexShader->sims3Hash) : false;
  h.vsBound = pShader;
  h.vsNormal = pLssVertexShader ? &pLssVertexShader->sims3Normal : nullptr;
  h.vsConstOut = pLssVertexShader ? &pLssVertexShader->sims3ConstOut : nullptr;
  h.vsCard = pLssVertexShader ? &pLssVertexShader->sims3Card : nullptr;
  h.vsWall = pLssVertexShader ? &pLssVertexShader->sims3Wall : nullptr;
  h.vsTerrain = pLssVertexShader ? sims3cam::findTerrainShader(pLssVertexShader->sims3Hash) : nullptr;
  h.vsTabled = pLssVertexShader && (h.vsCapturedUv || h.vsNeverCapture == 1);   // a blended-only entry still gets its opaque draws' variants
}
void sims3NotePixelShader(Sims3Hook& h, IDirect3DPixelShader9* pShader) {
  Direct3DPixelShader9_LSS* pLssPixelShader = bridge_cast<Direct3DPixelShader9_LSS*>(pShader);
  h.psBound = pShader;
  h.psAlbedoStage = pLssPixelShader ? pLssPixelShader->sims3AlbedoStage : -1;
  h.psTintReg = pLssPixelShader ? pLssPixelShader->sims3TintReg : -1;
  h.psHash = pLssPixelShader ? pLssPixelShader->sims3Hash : 0;
  h.psNeverCapture = sims3cam::neverCaptureMode(sims3cam::findNeverCapture(h.psHash));   // milestone 133
  h.psAuto = pLssPixelShader ? &pLssPixelShader->sims3Auto : nullptr;
  h.psMajor = pLssPixelShader ? pLssPixelShader->sims3Major : (uint8_t) 0;
}
void sims3NoteTexture(Sims3Hook& h, DWORD Stage, IDirect3DBaseTexture9* pTexture) {
  h.boundTex[Stage] = pTexture;
  bool color2D = false;
  uint8_t kind = 0; uint32_t fmt = 0; uint16_t w = 0, hgt = 0;
  if (pTexture != nullptr) {
    const D3DRESOURCETYPE type = pTexture->GetType();
    if (type == D3DRTYPE_TEXTURE) {
      const D3DSURFACE_DESC d = bridge_cast<Direct3DTexture9_LSS*>(pTexture)->getLevelDesc(0);
      color2D = (d.Usage & D3DUSAGE_RENDERTARGET) == 0 && sims3cam::isColorFormat(d.Format);
      kind = (uint8_t) (1 | ((d.Usage & D3DUSAGE_RENDERTARGET) ? 0x80 : 0));
      fmt = (uint32_t) d.Format; w = (uint16_t) d.Width; hgt = (uint16_t) d.Height;
    } else if (type == D3DRTYPE_CUBETEXTURE) {
      kind = 2;
    } else if (type == D3DRTYPE_VOLUMETEXTURE) {
      kind = 3;
    }
  }
  h.boundColor2D[Stage] = color2D;
  h.boundKind[Stage] = kind; h.boundFmt[Stage] = fmt; h.boundW[Stage] = w; h.boundH[Stage] = hgt;
}
void sims3NoteDecl(Sims3Hook& h, IDirect3DVertexDeclaration9* pDecl) {
  auto* const pLssVtxDecl = bridge_cast<Direct3DVertexDeclaration9_LSS*>(pDecl);
  const UID id = (pLssVtxDecl) ? (UID) pLssVtxDecl->getId() : 0;
  h.declIs3D = pLssVtxDecl ? sims3cam::positionIs3D(pLssVtxDecl->sims3Elements()) : false;
  h.declElems = pLssVtxDecl ? pLssVtxDecl->sims3Elements() : nullptr;
  h.wallLayout = sims3cam::WallLayout(); h.wallDeclId = (uint32_t) id;
  if (pLssVtxDecl) sims3cam::wallLayoutFromDecl(pLssVtxDecl->sims3Elements(), h.wallLayout);
}

// The game's wave maps (milestone 93): a water draw's bound 2D textures in a wave-map format
// (sims3cam::isWaveMapFormat), level 0 as the client keeps it, to
// rtx-remix\logs\sims3-textures\water_<pixel shader>_s<stage>_<w>x<h>_<format>.raw -- the source of the
// Remix mod's ripple normal maps (sims3/remix-mod/make_textures.py --waves), which are game data and not in
// the repository. A file is written only when it is missing (milestone 98).
inline void sims3DumpWaveMaps(Sims3Hook& h) {
  for (DWORD s = 0; s < 16 && h.waveDumped < 32u; ++s) {
    if (h.boundKind[s] != 1 || !sims3cam::isWaveMapFormat(h.boundFmt[s]) || !h.boundTex[s]) continue;   // 2D, not a render target
    auto* tex = bridge_cast<Direct3DTexture9_LSS*>(h.boundTex[s]);
    if (!tex) continue;
    const uint32_t id = (uint32_t) tex->getId();
    bool done = false; for (uint32_t i = 0; i < h.waveDumped; ++i) if (h.waveDumpedIds[i] == id) done = true;
    if (done) continue;
    const uint8_t* data = tex->sims3Level0Data();
    const D3DSURFACE_DESC d = tex->getLevelDesc(0);
    h.waveDumpedIds[h.waveDumped++] = id;
    if (!data) { Logger::info(format_string("Sims 3 camera hook: wave map of PS %016llx at s%u: no level-0 data kept", (unsigned long long) h.psHash, (unsigned) s)); continue; }
    static char dir[MAX_PATH] = {};
    if (!dir[0]) {
      GetModuleFileNameA(nullptr, dir, MAX_PATH);
      char* p = strrchr(dir, '\\'); if (p) *p = 0;
      strncat_s(dir, "\\rtx-remix\\logs\\sims3-textures", _TRUNCATE);
      CreateDirectoryA(dir, nullptr);
    }
    char fb[16], path[MAX_PATH + 96];
    snprintf(path, sizeof path, "%s\\water_%016llx_s%u_%ux%u_%s.raw", dir, (unsigned long long) h.psHash, (unsigned) s, (unsigned) d.Width, (unsigned) d.Height, sims3FormatName(d.Format, fb, sizeof fb));
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) continue;   // already there
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) {
      fwrite(data, 1, bridge_util::calcTotalSizeOfRect(d.Width, d.Height, d.Format), f);
      fclose(f);
      Logger::info(format_string("Sims 3 camera hook: wave map written -> %s", path));
    }
  }
}


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
  h.vsWall = pLssVertexShader ? &pLssVertexShader->sims3Wall : nullptr;
  h.vsConstRegs = pLssVertexShader ? pLssVertexShader->sims3ConstRegs : (uint16_t) 0;
  h.vsTerrain = pLssVertexShader ? sims3cam::findTerrainShader(pLssVertexShader->sims3Hash) : nullptr;
  h.vsTabled = pLssVertexShader && (h.vsCapturedUv || h.vsNeverCapture == 1);   // a blended-only entry still gets its opaque draws' variants
}
void sims3NotePixelShader(Sims3Hook& h, IDirect3DPixelShader9* pShader) {
  Direct3DPixelShader9_LSS* pLssPixelShader = bridge_cast<Direct3DPixelShader9_LSS*>(pShader);
  h.psBound = pShader;
  h.psAlbedoStage = pLssPixelShader ? pLssPixelShader->sims3AlbedoStage : -1;
  h.psTintReg = pLssPixelShader ? pLssPixelShader->sims3TintReg : -1;
  h.psHash = pLssPixelShader ? pLssPixelShader->sims3Hash : 0;
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
  h.wallLayout = sims3cam::WallLayout(); h.wallDeclId = (uint32_t) id;
  if (pLssVtxDecl) sims3cam::wallLayoutFromDecl(pLssVtxDecl->sims3Elements(), h.wallLayout);
}

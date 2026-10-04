// The Sims 3 camera hook, part of d3d9_device.cpp: the town ground's squares (milestone 60): each
// square's shape as one draw of the hook's own. Included first, inside that file's second anonymous
// namespace (after the buffer headers); not a standalone header.

// ---- the town ground's squares (milestone 60, design B) -------------------------------------
// The game draws each 256-unit square of the town ground from one vertex buffer, in several
// pieces -- one per paint-layer mix, the unlit world shader dfaf82cf, or the lit lot-area shader
// 55c99586 near lots -- that tile the square exactly, and it hangs 2-unit skirts from the
// square's and the lots' edges (run 168's census). The ray tracer gets each square's SHAPE as one
// draw of the hook's own: the union of its opaque pieces without the skirts, sent right after the
// square's first piece of a frame with the state that piece set up, as a visible terrain draw;
// the game's pieces only PAINT (kind 3: baked with alpha 1, hidden). The shape's bake paints the
// first piece's paint over the whole square and the following pieces cover their parts with
// their own (the first piece's part gets the same paint twice: the same shader variant, and both
// markers' red is 0x80). A piece that is not in the shape yet (a new square, a re-cut piece) is
// traced as before for that frame; the shape is rebuilt at Present.
template<typename Dev>
uint8_t sims3SquarePiece(Sims3Hook& h, Dev* dev) {
  if (!h.drawIndexed || h.drawType != D3DPT_TRIANGLELIST || h.drawPrims == 0) return 1;
  IDirect3DVertexBuffer9* vb = nullptr; UINT off = 0, stride = 0;
  if (FAILED(dev->GetStreamSource(0, &vb, &off, &stride)) || !vb) return 1;
  vb->Release();   // the device state holds it
  IDirect3DIndexBuffer9* ib = nullptr;
  if (FAILED(dev->GetIndices(&ib)) || !ib) return 1;
  ib->Release();
  auto* lvb = bridge_cast<Direct3DVertexBuffer9_LSS*>(vb);
  auto* lib = bridge_cast<Direct3DIndexBuffer9_LSS*>(ib);
  if (!lvb || !lib || stride == 0) return 1;
  const uint32_t vbId = (uint32_t) lvb->getId();
  struct { uint64_t vb; uint32_t vbId, off, stride; } sk = { (uint64_t) (uintptr_t) vb, vbId, off, stride };
  Sims3Hook::Square* sp = h.squares.find(sims3cam::fnv1a64(&sk, sizeof sk));
  if (!sp) {
    // the position: SHORT4 x, height, z, morph (run 168); any other layout is not merged
    IDirect3DVertexDeclaration9* decl = nullptr; int posOffset = -1;
    if (SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl) {
      auto* ld = bridge_cast<Direct3DVertexDeclaration9_LSS*>(decl);
      const D3DVERTEXELEMENT9* e = ld ? ld->sims3Elements() : nullptr;
      for (int i = 0; e && i < 32 && e[i].Stream != 0xFF; ++i)
        if (e[i].Stream == 0 && e[i].Usage == D3DDECLUSAGE_POSITION && e[i].UsageIndex == 0 && e[i].Type == D3DDECLTYPE_SHORT4) posOffset = e[i].Offset;
      decl->Release();
    }
    if (posOffset < 0 || (UINT) posOffset + 8u > stride) return 1;
    Sims3Hook::Square& n = h.squares.add(sims3cam::fnv1a64(&sk, sizeof sk), [](Sims3Hook::Square& old) { old.release(); });
    n.vb = vb; n.vbId = vbId; n.offset = off; n.stride = stride; n.posOffset = (uint16_t) posOffset;
    sp = &n;
  }
  Sims3Hook::Square& s = *sp;
  if (s.frameSeen != h.frames) { s.frameSeen = h.frames; s.frameRanges.clear(); }
  s.vbVersion = lvb->sims3Version; s.ib = ib; s.ibId = (uint32_t) lib->getId(); s.ibVersion = lib->sims3Version; s.ib32 = lib->getDesc().Format == D3DFMT_INDEX32; s.base = h.drawBase;
  const uint64_t range = ((uint64_t) h.drawStart << 32) | (uint64_t) h.drawPrims;
  s.frameRanges.push_back(range);
  const bool inShape = s.ready && s.merged && s.builtVbVersion == s.vbVersion && s.builtIbId == s.ibId && s.builtIbVersion == s.ibVersion && s.builtBase == s.base &&
                       std::binary_search(s.ranges.begin(), s.ranges.end(), range);
  if (!inShape) { ++h.mergeFallbackPieces; return 1; }
  ++h.mergePaintPieces;
  if (s.mergedFrame != h.frames) { s.mergedFrame = h.frames; h.mergePending = &s; }
  return 3;
}
// The square's shape from the game's buffers as they are now (the square was drawn this frame).
template<typename Dev>
void sims3SquareBuild(Sims3Hook& h, Dev* dev, Sims3Hook::Square& s, const std::vector<uint64_t>& ranges) {
  ++h.mergeBuilds;
  if (s.merged) { s.merged->Release(); s.merged = nullptr; }
  s.ready = false;
  auto* lvb = bridge_cast<Direct3DVertexBuffer9_LSS*>(s.vb);
  auto* lib = bridge_cast<Direct3DIndexBuffer9_LSS*>(s.ib);
  const uint8_t* vd = lvb ? lvb->sims3Data() : nullptr;
  const uint8_t* id = lib ? lib->sims3Data() : nullptr;
  if (!vd || !id) { ++h.mergeBuildFailed; return; }
  const uint32_t vbSize = lvb->sims3Size(), ibSize = lib->sims3Size();
  const uint32_t count = vbSize > s.offset ? (vbSize - s.offset) / s.stride : 0;
  std::vector<int32_t> x(count), z(count);
  for (uint32_t v = 0; v < count; ++v) {
    int16_t c[4]; memcpy(c, vd + s.offset + (size_t) v * s.stride + s.posOffset, sizeof c);
    x[v] = c[0]; z[v] = c[2];
  }
  const uint32_t isz = s.ib32 ? 4u : 2u, ibCount = ibSize / isz;
  std::vector<uint32_t> idx;
  for (uint64_t r : ranges) {
    const uint32_t start = (uint32_t) (r >> 32);
    uint32_t n = 3u * (uint32_t) r;
    if (start >= ibCount) continue;
    if (start + n > ibCount) n = (ibCount - start) / 3u * 3u;
    for (uint32_t k = 0; k < n; ++k) {
      uint32_t i;
      i = sims3cam::readIndex(id, (size_t) start + k, s.ib32);
      const int64_t vtx = (int64_t) s.base + (int64_t) i;
      idx.push_back(vtx < 0 ? 0xFFFFFFFFu : (uint32_t) vtx);   // a vertex before the buffer: left out as outside
    }
  }
  std::vector<uint32_t> out; sims3cam::MergeStats st;
  sims3cam::mergeGroundTriangles(x, z, idx, out, st);
  s.kept = st.kept; s.skirts = st.skirts; s.flat = st.flat;
  s.ranges = ranges; s.builtVbVersion = s.vbVersion; s.builtIbId = s.ibId; s.builtIbVersion = s.ibVersion; s.builtBase = s.base;
  if (out.empty()) { ++h.mergeBuildFailed; return; }
  uint32_t lo = 0xFFFFFFFFu, hi = 0;
  for (uint32_t v : out) { if (v < lo) lo = v; if (v > hi) hi = v; }
  s.merged = sims3MakeIndexBuffer(dev, out);
  if (!s.merged) { ++h.mergeBuildFailed; return; }
  s.mergedPrims = (uint32_t) (out.size() / 3); s.minIndex = lo; s.numVertices = hi - lo + 1; s.ready = true;
}
// At Present: each square drawn this frame keeps its shape when the frame's pieces are in it;
// a new piece joins the shape unless it overlaps one already there (the pieces were re-cut:
// the frame's own pieces then); a written buffer means the frame's pieces from scratch.
template<typename Dev>
void sims3SquaresFrameEnd(Sims3Hook& h, Dev* dev) {
  h.mergePending = nullptr;
  const uint32_t drawn = h.frames - 1;   // Present has counted the frame already: its draws saw frames - 1
  for (auto& s : h.squares) {
    if (s.frameSeen != drawn || s.frameRanges.empty()) continue;
    std::sort(s.frameRanges.begin(), s.frameRanges.end());
    s.frameRanges.erase(std::unique(s.frameRanges.begin(), s.frameRanges.end()), s.frameRanges.end());
    const bool sameBuffers = s.builtVbVersion == s.vbVersion && s.builtIbId == s.ibId && s.builtIbVersion == s.ibVersion && s.builtBase == s.base && (s.ready || !s.ranges.empty());
    std::vector<uint64_t> shape;
    if (sameBuffers) {
      shape = s.ranges;
      bool recut = false;
      for (uint64_t r : s.frameRanges) {
        if (std::binary_search(s.ranges.begin(), s.ranges.end(), r)) continue;
        for (uint64_t q : shape) if (sims3cam::rangesOverlap(r, q)) { recut = true; break; }
        if (recut) break;
        shape.push_back(r);
      }
      if (recut) shape = s.frameRanges; else std::sort(shape.begin(), shape.end());
    } else {
      shape = s.frameRanges;
    }
    if (!sameBuffers || shape != s.ranges) sims3SquareBuild(h, dev, s, shape);
  }
}
// Right after a square's first piece of the frame: the square's shape, through the ordinary draw
// hooks as the hook's own re-issue (kind 1), from the piece's stream and state with the shape's
// index buffer; the game's index buffer back afterwards.
template<typename Dev>
void sims3MergedSquareDraw(Sims3Hook& h, Dev* dev) {
  Sims3Hook::Square* const sp = h.mergePending; h.mergePending = nullptr;
  if (!sp) return;
  Sims3Hook::Square& s = *sp;
  if (!s.ready || !s.merged) return;
  IDirect3DVertexBuffer9* vb = nullptr; UINT off = 0, stride = 0;
  if (FAILED(dev->GetStreamSource(0, &vb, &off, &stride)) || vb != s.vb || off != s.offset || stride != s.stride) { if (vb) vb->Release(); ++h.mergeSkipped; return; }
  vb->Release();
  IDirect3DIndexBuffer9* game = nullptr; dev->GetIndices(&game);
  dev->SetIndices(s.merged);
  h.reissue = true; h.reissueKind = 1;
  dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, s.minIndex, s.numVertices, 0, s.mergedPrims);
  h.reissue = false; h.reissueKind = 0;
  dev->SetIndices(game); if (game) game->Release();
  ++h.mergedDraws;
}

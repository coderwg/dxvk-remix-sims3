// The Sims 3 camera hook, part of d3d9_device.cpp: the wall openings (milestone 13): the mask atlas
// decoded, the content hashes and the cut geometry's cache. Included after sims3_device_masks.inl;
// not a standalone header.

// ---- wall openings (milestone 13) ----------------------------------------------------------
// The mask atlas decoded to one byte per texel, cached per (texture, upload). Null when the
// texture's level 0 was never locked on the client or its format is not one the decoder reads.
inline const uint8_t* sims3WallMask(Sims3Hook& h, uint32_t texId, uint32_t version, uint32_t fmt, uint32_t w, uint32_t hgt, const uint8_t* data, uint64_t& contentHash) {
  const uint64_t key = (uint64_t) texId << 32 | version;
  if (const Sims3Hook::MaskEntry* known = h.masks.find(key)) { contentHash = known->contentHash; return known->ok ? known->red.data() : nullptr; }
  Sims3Hook::MaskEntry& m = h.masks.add(key);
  m.ok = data != nullptr && sims3cam::decodeMaskRed(fmt, data, bridge_util::calcTotalSizeOfRect(w, hgt, (D3DFORMAT) fmt), w, hgt, m.red);
  if (m.ok) { ++h.wallMasksDecoded; m.contentHash = sims3cam::fnv1a64(m.red.data(), m.red.size()) ^ ((uint64_t) w << 48) ^ ((uint64_t) hgt << 32); }
  contentHash = m.contentHash;
  return m.ok ? m.red.data() : nullptr;
}

// The content hash of a buffer upload, computed once per (object, upload): the wall cut's cache
// key follows the bytes, so an identical re-upload rebuilds nothing.
inline uint64_t sims3ContentHash(Sims3Hook& h, uint32_t id, uint32_t version, const uint8_t* data, size_t size) {
  const uint64_t key = (uint64_t) id << 32 | version;
  if (const uint64_t* known = h.bufHashes.find(key)) return *known;
  return h.bufHashes.add(key) = sims3cam::fnv1a64(data, size) ^ ((uint64_t) size << 40);
}

// The cut geometry of a wall draw: from the cache by key, else built now (its buffers created
// on the device). An entry with changed == false says the draw has nothing to cut or drop, or
// that the cut refused it (counted: a vertex outside the buffers, more than 65,535 vertices).
template<typename Dev>
Sims3Hook::WallEntry* sims3WallEntry(Sims3Hook& h, Dev* dev, uint64_t key, const sims3cam::WallCutInput& in, sims3cam::WallCutStats& statsOut, bool& builtOut) {
  builtOut = false;
  if (Sims3Hook::WallEntry* known = h.wallCache.find(key)) return known;
  sims3cam::WallCutOutput res;
  const bool ok = sims3cam::cutWallOpenings(in, res);
  statsOut = res.stats; builtOut = true;
  Sims3Hook::WallEntry e;
  e.changed = ok && res.changed;
  ++h.wallBuilt;
  if (!ok) ++h.wallRefused;
  if (e.changed) {
    h.wallCutTriangles += res.stats.cut; h.wallRemovedTriangles += res.stats.removed; h.wallHiddenTriangles += res.stats.hidden;
    e.vb0 = sims3MakeVertexBuffer(dev, res.vb0); e.vb1 = sims3MakeVertexBuffer(dev, res.vb1); e.ib = sims3MakeIndexBuffer(dev, res.ib);
    e.vertexCount = res.vertexCount; e.triangleCount = res.triangleCount;
    if (!e.vb0 || !e.vb1 || !e.ib) { e.release(); ++h.wallBuildFailed; }
  }
  return &(h.wallCache.add(key, [](Sims3Hook::WallEntry& old) { old.release(); }) = e);
}

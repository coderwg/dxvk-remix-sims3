**Title:** client: zero-fill the shadow copy of a surface; a partly written fresh texture uploads heap leftovers

### Problem

`Direct3DSurface9_LSS::lock` (`src/client/d3d9_surface.cpp`) allocates the client's copy of a
surface with `new uint8_t[surfaceSize]`, uninitialised. The game writes into that copy, and
`unlock` uploads the locked rectangle from it, whole.

A game that writes only part of a fresh texture, and expects the rest to read as zero, gets
heap leftovers instead. Native Direct3D hands out fresh memory for a new texture, and the
Remix runtime zeroes a fresh mapping buffer (`D3D9DeviceEx::LockImage`: `if (alloced)
std::memset(physSlice.mapPtr, 0, physSlice.length)`), so the same game is fine without the
bridge. Through the bridge, the unwritten part is uploaded as texels.

### How it shows in The Sims 3

Terrain paint in Build mode. Each lot has a paint mask (A8R8G8B8, 128x128 or 128x256, one
channel per paint layer). When a stroke needs a new mask the game creates the texture, locks
the whole level with flags 0 and writes only the rows the lot covers: rows 0 to 120 of a
128x128 mask, rows 0 to 160 of a 128x256 mask, or just the rows of the stroke. The remaining
rows are never written (measured with a marker byte in the copy: 7 of 128, 95 of 256, up to
124 of 128 rows untouched at the first write).

The uploaded mask then carried heap garbage in those rows, and the lot's paint turned into a
dense blocky pattern over the whole lot on the stroke that created the mask. Undo rewrote the
mask whole and restored it. With ray tracing off the same pattern showed, so the texture data
itself was wrong. With the copy zero-filled the corruption is gone (two runs, every stroke
type). With the copy filled with a marker instead of zeros, the marker shows through as a band
across the lot, in the rows the game wrote only in part.

### Fix

`m_shadow.reset(new uint8_t[surfaceSize]())` -- value-initialised, so a fresh copy reads as
zeros. One line, no protocol change, no cost beyond the zeroing of a buffer that is allocated
once per surface.

### Notes

- Any game that relies on a fresh lockable texture reading as zero is affected the same way;
  the symptom is texture garbage that appears once and goes away on a full rewrite.
- The shared-heap path (`useSharedHeap = True`) allocates from the shared heap instead and was
  not checked.

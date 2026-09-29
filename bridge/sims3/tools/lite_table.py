"""The Sims 3 light definitions (LITE) per model, from the game's package files.

Reads every DBPF package under the game's GameData folders (and, after them, the user's mods
folder, whose overrides win), follows each visual proxy (VPXY) to its light definition (LITE)
and its model (MODL / MLOD), and writes one line per model that has lights:

    <index hash> <index bytes> <vertex hash> <vertex bytes> <model instance> <lights...>

The hashes are FNV-1a 64 of the mesh chunks' data, raw and with the index differences
decoded. The game uploads its chunks verbatim (run 133: 32 of 115 Direct3D index buffers
matched the decoded hashes exactly), so the decoded index hash identifies a drawn model.

    python lite_table.py "C:\\Program Files\\EA Games\\The Sims 3" out.txt [--hook sims3lights.txt]

--hook writes the compact table the camera hook reads from next to its DLL (milestone 22):
one line per mesh, `<decoded index hash> <model instance> <count> [<type> x y z r g b intensity]...`
with type 3 point, 4 spot, 5 lamp shade, 6 tube light; window, area and world lights are left
out (daylight portals, not lamps).
"""
import os, struct, sys, collections, time

T_LITE, T_MODL, T_MLOD, T_VPXY = 0x03B4C61D, 0x01661233, 0x01D10F34, 0x736884F1
C_VBUF, C_IBUF = 0x0229684B, 0x0229684F
LIGHT_TYPES = {1: 'Ambient', 2: 'Directional', 3: 'Point', 4: 'Spot', 5: 'LampShade', 6: 'TubeLight', 7: 'SquareWindow', 8: 'CircularWindow', 9: 'SquareAreaLight', 10: 'DiscAreaLight', 11: 'WorldLight'}

def fnv1a64(b):
    h = 0xcbf29ce484222325
    for x in b:
        h ^= x; h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

def refpack(src):
    flags = src[0]; p = 2; big = (flags & 0x80) != 0
    if flags & 0x01: p += 4 if big else 3
    n = 4 if big else 3; p += n
    out = bytearray()
    while p < len(src):
        b0 = src[p]
        if b0 < 0x80:
            b1 = src[p + 1]; p += 2; lit = b0 & 3; cnt = ((b0 & 0x1C) >> 2) + 3; off = ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1 = src[p + 1]; b2 = src[p + 2]; p += 3; lit = (b1 & 0xC0) >> 6; cnt = (b0 & 0x3F) + 4; off = ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1 = src[p + 1]; b2 = src[p + 2]; b3 = src[p + 3]; p += 4; lit = b0 & 3; cnt = ((b0 & 0x0C) << 6) + b3 + 5; off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            p += 1; lit = ((b0 & 0x1F) << 2) + 4; cnt = 0; off = 0
        else:
            p += 1; lit = b0 & 3; out += src[p:p + lit]; p += lit; break
        out += src[p:p + lit]; p += lit
        for k in range(cnt): out.append(out[-off])
    return bytes(out)

def read_index(path):
    with open(path, 'rb') as f:
        hdr = f.read(96)
        if hdr[:4] != b'DBPF': return []
        count = struct.unpack_from('<I', hdr, 0x24)[0]; idxSize = struct.unpack_from('<I', hdr, 0x2C)[0]; idxOff = struct.unpack_from('<I', hdr, 0x40)[0]
        f.seek(idxOff); idx = f.read(idxSize)
    pos = 0; indexType = struct.unpack_from('<I', idx, pos)[0]; pos += 4
    common = {}
    if indexType & 1: common['type'] = struct.unpack_from('<I', idx, pos)[0]; pos += 4
    if indexType & 2: common['group'] = struct.unpack_from('<I', idx, pos)[0]; pos += 4
    if indexType & 4: common['ihi'] = struct.unpack_from('<I', idx, pos)[0]; pos += 4
    out = []
    for i in range(count):
        t = common.get('type'); g = common.get('group'); ihi = common.get('ihi')
        if t is None: t = struct.unpack_from('<I', idx, pos)[0]; pos += 4
        if g is None: g = struct.unpack_from('<I', idx, pos)[0]; pos += 4
        if ihi is None: ihi = struct.unpack_from('<I', idx, pos)[0]; pos += 4
        ilo = struct.unpack_from('<I', idx, pos)[0]; pos += 4
        off, sz, msz, comp, unk = struct.unpack_from('<IIIHH', idx, pos); pos += 16
        out.append((t, g, (ihi << 32) | ilo, off, sz & 0x7FFFFFFF, comp))
    return out

def rcol(b):
    ver, pub, unused, nExt, nInt = struct.unpack_from('<IIIII', b, 0)
    p = 20; internal = []
    for i in range(nInt): inst, typ, grp = struct.unpack_from('<QII', b, p); internal.append((typ, grp, inst)); p += 16
    p += 16 * nExt
    chunks = [struct.unpack_from('<II', b, p + 8 * i) for i in range(nInt)]
    return internal, chunks

def lite_lights(b):
    i = b.find(b'LITE')
    if i < 0: return []
    n = b[i + 12]; p = i + 16; out = []
    for k in range(n):
        typ = struct.unpack_from('<I', b, p)[0]
        f = struct.unpack_from('<' + 'f' * 31, b, p + 4)
        out.append((LIGHT_TYPES.get(typ, str(typ)),) + tuple(f[:16]))   # position, colour, intensity, then the shape: a direction and six numbers by type
        p += 4 + 4 * 31
    return out

class Store:
    # Every copy of a resource is kept (milestone 30): a model's LOD meshes are MLODs of the SAME
    # instance under different groups, and a patch package may carry a second version -- keying
    # by type and instance alone lost all but one, and the game drew meshes the table never saw.
    def __init__(self): self.res = {}; self.files = []; self.copies = 0
    def add_package(self, path):
        try: idx = read_index(path)
        except Exception as ex: print('skip', path, ex); return
        fi = len(self.files); self.files.append(open(path, 'rb'))
        for (t, g, inst, off, sz, comp) in idx:
            if t in (T_LITE, T_MODL, T_MLOD, T_VPXY): self.res.setdefault((t, inst), []).append((fi, g, off, sz, comp)); self.copies += 1
    def get_all(self, t, inst):
        """Every copy of (t, inst), decompressed, in package order."""
        out = []
        for (fi, g, off, sz, comp) in self.res.get((t, inst), []):
            f = self.files[fi]; f.seek(off); raw = f.read(sz)
            out.append(refpack(raw) if comp == 0xFFFF else raw)
        return out
    def get(self, t, inst):
        all_ = self.get_all(t, inst)
        return all_[-1] if all_ else None
    def get_best(self, t, inst):
        """The copy the game itself uses (milestone 32, for the light definitions): a package of the
        Mods folder before the game's, a DeltaBuild or ContentPatch before a FullBuild."""
        best = None; rank = -1
        for (fi, g, off, sz, comp) in self.res.get((t, inst), []):
            path = self.files[fi].name.replace('\\', '/').lower(); name = path.rsplit('/', 1)[-1]
            r = 3 if '/mods/' in path else (2 if name.startswith('deltabuild') or name.startswith('contentpatch') else 1)
            if r >= rank: rank = r; best = (fi, off, sz, comp)
        if not best: return None
        fi, off, sz, comp = best
        f = self.files[fi]; f.seek(off); raw = f.read(sz)
        return refpack(raw) if comp == 0xFFFF else raw

def keys_in(b, store):
    found = []
    for p in range(0, len(b) - 15):   # the keys sit at any byte alignment
        typ, grp, inst = struct.unpack_from('<IIQ', b, p)
        if typ in (T_LITE, T_MODL, T_MLOD) and (typ, inst) in store.res: found.append((typ, inst))
        inst2, typ2, grp2 = struct.unpack_from('<QII', b, p)
        if typ2 in (T_LITE, T_MODL, T_MLOD) and (typ2, inst2) in store.res: found.append((typ2, inst2))
    return found

def mesh_hashes(store, t, inst):
    """The (index hash raw, index hash decoded, index bytes, vertex hash, vertex bytes, flags, displacement)
    of every IBUF/VBUF pair inside the RCOL of resource (t, inst): a MODL holds its first LOD's mesh
    chunks, an MLOD the further LODs'."""
    out = []
    for b in store.get_all(t, inst):   # every copy: each LOD group, each patched version
        if not b or len(b) < 20: continue
        internal, chunks = rcol(b)
        ibufs = []; vbufs = []
        for (tt, g, i), (pos, size) in zip(internal, chunks):
            tag = b[pos:pos + 4]
            if tag == b'IBUF': ibufs.append(b[pos:pos + size])
            elif tag == b'VBUF': vbufs.append(b[pos:pos + size])
        for k, ibuf in enumerate(ibufs):
            vbuf = vbufs[k] if k < len(vbufs) else None
            out += one_mesh(ibuf, vbuf)
    return out

def one_mesh(ibuf, vbuf):
    # IBUF: tag, version, flags, displacement, then 16-bit indices (flags bit 0: stored as differences)
    ver, flags, disp = struct.unpack_from('<IIi', ibuf, 4)
    raw = ibuf[16:]
    n = len(raw) // 2
    idx = list(struct.unpack_from('<' + 'h' * n, raw, 0))
    dec = []
    if flags & 1:
        acc = 0
        for d in idx: acc += d; dec.append(acc & 0xFFFF)
    else:
        dec = [x & 0xFFFF for x in idx]
    decoded = struct.pack('<' + 'H' * n, *dec)
    vraw = vbuf[16:] if vbuf else b''
    return [(fnv1a64(raw), fnv1a64(decoded), len(raw), fnv1a64(vraw), len(vraw), flags, disp)]

def model_meshes(store, modlInst):
    """The mesh chunks of a model: its MODL's own, then every MLOD the MODL refers to."""
    rows = mesh_hashes(store, T_MODL, modlInst)
    seen = set()
    for b in store.get_all(T_MODL, modlInst):
        for (tt, i) in keys_in(b, store):
            if tt == T_MLOD and i not in seen:
                seen.add(i); rows += mesh_hashes(store, T_MLOD, i)
    if modlInst not in seen: rows += mesh_hashes(store, T_MLOD, modlInst)   # the LOD meshes sit under the model's own instance
    uniq = []; had = set()
    for r in rows:
        if (r[1], r[2]) not in had: had.add((r[1], r[2])); uniq.append(r)
    return uniq

def main():
    if len(sys.argv) < 3: print(__doc__); return
    root, outPath = sys.argv[1], sys.argv[2]
    verbose = '--verbose' in sys.argv
    hookPath = sys.argv[sys.argv.index('--hook') + 1] if '--hook' in sys.argv else None
    hookRows = []; hookSeen = set()
    t0 = time.time()
    store = Store()
    pk = []
    for dp, dn, fn in os.walk(root):
        if os.path.basename(dp) == 'Packages' and 'GameData' in dp:
            for f in sorted(fn):
                if f.endswith('.package'): pk.append(os.path.join(dp, f))
    mods = os.path.join(os.path.expanduser('~'), 'Documents', 'Electronic Arts', 'The Sims 3', 'Mods', 'Packages')
    if os.path.isdir(mods):
        for dp, dn, fn in os.walk(mods):
            for f in sorted(fn):
                if f.endswith('.package'): pk.append(os.path.join(dp, f))
    for p in pk: store.add_package(p)
    print('%d packages, %d resources of interest in %d copies, %.1f s' % (len(pk), len(store.res), store.copies, time.time() - t0))
    vpxys = [k for k in store.res if k[0] == T_VPXY]
    rows = 0; withLite = 0; withMesh = 0
    with open(outPath, 'w') as out:
        for (t, inst) in vpxys:
            b = store.get(T_VPXY, inst)
            if not b: continue
            ks = keys_in(b, store)
            lites = [i for (tt, i) in ks if tt == T_LITE]
            if not lites: continue
            withLite += 1
            lights = []
            for li in lites:
                for L in lite_lights(store.get_best(T_LITE, li) or b''):
                    if L not in lights: lights.append(L)
            if not lights: continue
            modls = [i for (tt, i) in ks if tt == T_MODL] or [inst]
            meshes = []
            for mi in modls: meshes += model_meshes(store, mi)
            if meshes: withMesh += 1
            lampLights = [L for L in lights if L[0] in ('Point', 'Spot', 'LampShade', 'TubeLight')]
            for (ih, ihd, ilen, vh, vlen, flags, disp) in meshes:
                out.write('%016x %016x %d %016x %d %016x %d %d | %s\n' % (ih, ihd, ilen, vh, vlen, inst, flags, disp,
                          '; '.join('%s (%.2f,%.2f,%.2f) rgb %.2f,%.2f,%.2f i %.1f' % L[:8] for L in lights)))
                rows += 1
                if lampLights and (ihd, inst) not in hookSeen:
                    hookSeen.add((ihd, inst))
                    kinds = {'Point': 3, 'Spot': 4, 'LampShade': 5, 'TubeLight': 6}
                    hookRows.append('%016x %016x %d %s' % (ihd, inst, len(lampLights), ' '.join(('%d' + ' %.4f' * 16) % ((kinds[L[0]],) + L[1:17]) for L in lampLights)))
    print('%d visual proxies, %d with lights, %d of those with mesh chunks found; %d mesh rows written to %s in %.1f s' % (len(vpxys), withLite, withMesh, rows, outPath, time.time() - t0))
    if hookPath:
        # a mesh several models share (a generic quad, a small common part) names none of them (milestone 30)
        owners = {}
        for r in hookRows: owners.setdefault(r.split()[0], set()).add(r.split()[1])
        unique = [r for r in hookRows if len(owners[r.split()[0]]) == 1]
        with open(hookPath, 'w') as hk:
            hk.write('# The Sims 3 camera hook: the game\'s own lamp lights per mesh (milestone 22), written by sims3/tools/lite_table.py.\n')
            hk.write('# <decoded index hash> <model instance> <count> [<type> x y z r g b intensity ax ay az d0 d1 d2 d3 d4 d5]...  type 3 point, 4 spot, 5 lamp shade, 6 tube\n')
            hk.write('# a = the definition\'s direction (from the lit side back to the light); d by type: spot = cone angle, blur; lamp shade = cone angle, shade multiplier, bottom angle, shade r g b; tube = length, blur\n')
            hk.write('format 2\n')
            for r in unique: hk.write(r + '\n')
        print('%d mesh lines for the hook written to %s (%d shared meshes left out)' % (len(unique), hookPath, len(hookRows) - len(unique)))

if __name__ == '__main__': main()

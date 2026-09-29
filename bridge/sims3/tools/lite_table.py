"""The Sims 3 light definitions per model, for the camera hook (sims3lights.txt, format 3).

    python lite_table.py "C:\\Program Files\\EA Games\\The Sims 3" sims3lights.txt

Reads every package under the game's folders and the user's Mods folder. A lamp in the game is
an object (its key resource, OBJK) that names a model (its visual proxy, VPXY), and the model
names a light definition (LITE). The table carries both steps:

    model  <model instance> <count> [<type> x y z r g b intensity ax ay az d0 d1 d2 d3 d4 d5]...
    object <object instance> <model instance>

type 3 point, 4 spot, 5 lamp shade, 6 tube, 11 world light (a street lamp's); windows and area
lights are left out (daylight openings, not lamps). a = the definition's direction, which
points from the lit side back to the light; d by type: spot = cone angle, blur; lamp shade and
world light = cone angle, shade multiplier, bottom angle, shade r g b; tube = length, blur.

The lamp reporter script mod names each lamp by its keys, and the hook looks the lamp up here:
no mesh is involved (milestones 22 to 38 identified a lamp by the mesh it was drawn with, and
missed every lamp drawn with a mesh the table did not hold).

Where a definition exists in several packages the game's own choice is taken: a package of the
Mods folder before the game's, a DeltaBuild or ContentPatch before a FullBuild.
"""
import os, struct, sys, time

T_LITE, T_VPXY, T_OBJK = 0x03B4C61D, 0x736884F1, 0x02DC343F
LIGHT_TYPES = {1: 'Ambient', 2: 'Directional', 3: 'Point', 4: 'Spot', 5: 'LampShade', 6: 'TubeLight', 7: 'SquareWindow', 8: 'CircularWindow', 9: 'SquareAreaLight', 10: 'DiscAreaLight', 11: 'WorldLight'}
LAMP_TYPES = {'Point': 3, 'Spot': 4, 'LampShade': 5, 'TubeLight': 6, 'WorldLight': 11}

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

def lite_lights(b):
    """The lights of a LITE resource: (type name, x, y, z, r, g, b, intensity, ax, ay, az, d0..d5)."""
    i = b.find(b'LITE')
    if i < 0: return []
    n = b[i + 12]; p = i + 16; out = []
    for k in range(n):
        if p + 128 > len(b): break
        typ = struct.unpack_from('<I', b, p)[0]
        f = struct.unpack_from('<31f', b, p + 4)
        out.append((LIGHT_TYPES.get(typ, str(typ)),) + tuple(f[:16]))
        p += 128
    return out

class Store:
    """Every copy of the resources of interest, by (type, instance), in package order."""
    def __init__(self): self.res = {}; self.files = []; self.copies = 0
    def add_package(self, path):
        try: idx = read_index(path)
        except Exception as ex: print('skip', path, ex); return
        fi = len(self.files); self.files.append(open(path, 'rb'))
        for (t, g, inst, off, sz, comp) in idx:
            if t in (T_LITE, T_VPXY, T_OBJK): self.res.setdefault((t, inst), []).append((fi, g, off, sz, comp)); self.copies += 1
    @staticmethod
    def rank(path):
        path = path.replace('\\', '/').lower(); name = path.rsplit('/', 1)[-1]
        return 3 if '/mods/' in path else (2 if name.startswith('deltabuild') or name.startswith('contentpatch') else 1)
    def read(self, entry):
        fi, g, off, sz, comp = entry
        f = self.files[fi]; f.seek(off); raw = f.read(sz)
        return refpack(raw) if comp == 0xFFFF else raw
    def get_all(self, t, inst):
        return [self.read(e) for e in self.res.get((t, inst), [])]
    def get_best(self, t, inst):
        """The copy the game itself uses."""
        best = None; rank = -1
        for e in self.res.get((t, inst), []):
            r = self.rank(self.files[e[0]].name)
            if r >= rank: rank = r; best = e
        return self.read(best) if best else None

def keys_in(b, store, wanted):
    """The resources of the wanted types that the bytes name (a key sits at any byte alignment, in either field order)."""
    found = []
    for p in range(0, len(b) - 15):
        typ, grp, inst = struct.unpack_from('<IIQ', b, p)
        if typ in wanted and (typ, inst) in store.res: found.append((typ, inst))
        inst2, typ2, grp2 = struct.unpack_from('<QII', b, p)
        if typ2 in wanted and (typ2, inst2) in store.res: found.append((typ2, inst2))
    return found

def main():
    if len(sys.argv) < 3: print(__doc__); return 1
    root, outPath = sys.argv[1], sys.argv[2]
    t0 = time.time()
    store = Store()
    pk = []
    for dp, dn, fn in os.walk(root):
        if os.path.basename(dp) == 'Packages' and 'GameData' in dp:
            pk += [os.path.join(dp, f) for f in sorted(fn) if f.endswith('.package')]
    mods = os.path.join(os.path.expanduser('~'), 'Documents', 'Electronic Arts', 'The Sims 3', 'Mods', 'Packages')
    nMods = 0
    if os.path.isdir(mods):
        for dp, dn, fn in os.walk(mods):
            for f in sorted(fn):
                if f.endswith('.package'): pk.append(os.path.join(dp, f)); nMods += 1
    for p in pk: store.add_package(p)
    print('%d packages (%d of the Mods folder), %d resources of interest in %d copies, %.1f s' % (len(pk), nMods, len(store.res), store.copies, time.time() - t0))

    models = {}   # model instance -> its lamp lights
    for (t, inst) in [k for k in store.res if k[0] == T_VPXY]:
        lights = []
        for vb in store.get_all(T_VPXY, inst):
            for (tt, li) in keys_in(vb, store, (T_LITE,)):
                for L in lite_lights(store.get_best(T_LITE, li) or b''):
                    if L[0] in LAMP_TYPES and L not in lights: lights.append(L)
        if lights: models[inst] = lights
    objects = {}  # object instance -> model instance
    for (t, inst) in [k for k in store.res if k[0] == T_OBJK]:
        for ob in store.get_all(T_OBJK, inst):
            for (tt, mi) in keys_in(ob, store, (T_VPXY,)):
                if mi in models: objects[inst] = mi
    byType = {}
    for lights in models.values():
        for L in lights: byType[L[0]] = byType.get(L[0], 0) + 1
    with open(outPath, 'w') as out:
        out.write('# The Sims 3 camera hook: the game\'s lamp lights per model, written by sims3/tools/lite_table.py.\n')
        out.write('# model <model instance> <count> [<type> x y z r g b intensity ax ay az d0 d1 d2 d3 d4 d5]...  type 3 point, 4 spot, 5 lamp shade, 6 tube, 11 world light\n')
        out.write('# object <object instance> <model instance>: the object key of a lamp names its model\n')
        out.write('# a = the definition\'s direction (from the lit side back to the light); d by type: spot = cone angle, blur; lamp shade, world light = cone angle, shade multiplier, bottom angle, shade r g b; tube = length, blur\n')
        out.write('format 3\n')
        for inst in sorted(models):
            lights = models[inst][:4]
            out.write('model %016x %d %s\n' % (inst, len(lights), ' '.join(('%d' + ' %.4f' * 16) % ((LAMP_TYPES[L[0]],) + L[1:17]) for L in lights)))
        for inst in sorted(objects):
            out.write('object %016x %016x\n' % (inst, objects[inst]))
    print('%d models with lamp lights (%s), %d objects naming them; written to %s in %.1f s' % (len(models), ', '.join('%s %d' % kv for kv in sorted(byType.items())), len(objects), outPath, time.time() - t0))
    return 0

if __name__ == '__main__': sys.exit(main())

"""Reads the terrain census of the camera hook (milestone 59) and rebuilds the game's terrain draws.

The hook writes, for the frame after each mark, a log line per draw ("census M dN ...") and for every
draw of a terrain vertex shader a file census<M>_d<NNNN>.bin in rtx-remix\\logs\\sims3-terrain-census:
a packed header (version 1, 1532 bytes), the draw's stream 0 and stream 1 vertex bytes and its index
bytes. This tool decodes the files, runs the terrain vertex shaders on the CPU (the three the hook
knows, from their disassembly) to get world positions, and reports what covers what.

  python terrain_census.py <run folder>                 summary of every census (from the log)
  python terrain_census.py <run folder> draws M         every terrain draw of census M, decoded
  python terrain_census.py <run folder> overlap M [cell] the ground covered by more than one ray-traced draw

<run folder> holds bridge32.log and the sims3-terrain-census folder. Read only.
"""
import os, re, struct, sys, collections, math

HDR_FMT = '<4s11I2Q16I8I I 192s 128f 64f 384s 6I'
HDR_SIZE = struct.calcsize(HDR_FMT)
assert HDR_SIZE == 1532, HDR_SIZE
RS_NAMES = ['zenable', 'zwrite', 'zfunc', 'blend', 'src', 'dst', 'blendop', 'cull', 'cw', 'atest', 'afunc', 'aref', 'stencil', 'depthbias', 'slopebias', 'srgbwrite']
DECLTYPE = {0: ('f', 1), 1: ('f', 2), 2: ('f', 3), 3: ('f', 4), 4: ('B', 4), 5: ('B', 4), 6: ('h', 2), 7: ('h', 4), 8: ('B', 4), 9: ('h', 2), 10: ('h', 4),
            11: ('H', 2), 12: ('H', 4), 15: ('e', 2), 16: ('e', 4)}
USAGE = ['position', 'blendweight', 'blendindices', 'normal', 'psize', 'texcoord', 'tangent', 'binormal', 'tessfactor', 'positiont', 'color', 'fog', 'depth', 'sample']
VS_NAMES = {0x55c99586fb17cd1c: 'lot-area', 0xdfaf82cf9ec175b0: 'world', 0x976b73dbd59842cd: 'lot', 0x0344bbc366f10954: 'composite', 0x2a57449ad7d2c7ee: 'coarse'}

class Draw:
    pass

def read_file(path):
    b = open(path, 'rb').read()
    v = struct.unpack_from(HDR_FMT, b, 0)
    d = Draw()
    (d.magic, d.version, d.mark, d.frame, d.draw, d.indexed, d.primType, d.baseVertex, d.minIndex, d.numVertices, d.startIndex, d.primCount) = v[0:12]
    if d.baseVertex >= 2**31: d.baseVertex -= 2**32
    d.vsHash, d.psHash = v[12], v[13]
    d.rs = dict(zip(RS_NAMES, v[14:30]))
    d.streamOffset, d.streamStride, d.vbSize = v[30:32], v[32:34], v[34:36]
    d.ibFormat, d.ibSize = v[36], v[37]
    d.declCount = v[38]
    raw = v[39]
    d.decl = []
    for i in range(d.declCount):
        stream, offset, typ, method, usage, uidx = struct.unpack_from('<HHBBBB', raw, i * 8)
        d.decl.append((stream, offset, typ, usage, uidx))
    vc = v[40:40 + 128]; d.vsConst = [vc[i * 4:i * 4 + 4] for i in range(32)]
    pc = v[168:168 + 64]; d.psConst = [pc[i * 4:i * 4 + 4] for i in range(16)]
    traw = v[232]
    d.tex = [struct.unpack_from('<4IQ', traw, i * 24) for i in range(16)]
    d.vbFirst, d.vbBytes, d.ibFirst, d.ibBytes = v[233:235], v[235:237], v[237], v[238]
    pos = HDR_SIZE
    d.vb = []
    for s in range(2):
        d.vb.append(b[pos:pos + d.vbBytes[s]]); pos += d.vbBytes[s]
    d.ib = b[pos:pos + d.ibBytes]
    d.name = VS_NAMES.get(d.vsHash, '%016x' % d.vsHash)
    return d

def positions(d):
    """the POSITION element of each vertex in the draw's range, as floats"""
    el = [e for e in d.decl if e[3] == 0 and e[4] == 0]
    if not el: return []
    stream, offset, typ, usage, uidx = el[0]
    fmt, n = DECLTYPE.get(typ, (None, 0))
    if fmt is None: return []
    stride = d.streamStride[stream]
    data = d.vb[stream]
    out = []
    for i in range(len(data) // stride if stride else 0):
        vals = struct.unpack_from('<%d%s' % (n, fmt), data, i * stride + offset)
        vals = list(vals) + [0.0] * (4 - n)
        if n < 4: vals[3] = 1.0
        out.append([float(x) for x in vals])
    return out

def indices(d):
    if not d.ibBytes: return None
    if d.ibFormat == 102: return list(struct.unpack('<%dI' % (len(d.ib) // 4), d.ib))
    return list(struct.unpack('<%dH' % (len(d.ib) // 2), d.ib))

def dp4(a, c): return a[0] * c[0] + a[1] * c[1] + a[2] * c[2] + a[3] * c[3]

def world_vertex(d, v):
    """runs the draw's vertex shader on one vertex: (world x, y, z, morph or None, inside its chunk)"""
    c = d.vsConst
    if d.name in ('world', 'lot-area'):
        scale, cam, mor = (c[9], c[10], c[11]) if d.name == 'world' else (c[16], c[17], c[18])
        rows = (c[4], c[5], c[6]) if d.name == 'world' else (c[8], c[9], c[10])
        r0 = [v[0] * scale[0] + scale[2], v[1] * scale[1] + scale[3], v[2] * scale[0] + scale[2], v[3] * scale[1] + scale[2]]
        dx = abs(cam[0] - r0[0] - rows[0][3]) - mor[0]
        dz = abs(cam[2] - r0[2] - rows[2][3]) - mor[0]
        m = max(dx, dz) / mor[1] if mor[1] else 0.0
        m = min(max(m, 0.0), 1.0)
        p = [r0[0], r0[1] + m * r0[3], r0[2], 1.0]
        return (dp4(p, rows[0]), dp4(p, rows[1]), dp4(p, rows[2]), m, True)
    if d.name == 'coarse':
        s = c[16]
        p = [v[0] * s[0] + s[2], v[1] * s[1] + s[3], v[2] * s[0] + s[2], 1.0]
        return (dp4(p, c[8]), dp4(p, c[9]), dp4(p, c[10]), None, True)
    if d.name == 'lot':
        y = v[2] / 256.0 + v[3] - 128.0
        p = [v[0] * 0.5, y, v[1] * 0.5, 1.0]
        wx, wy, wz = dp4(p, c[4]), dp4(p, c[5]), dp4(p, c[6])
        lx, lz = wx - c[10][0], wz - c[10][2]
        r3x, r3y = lx * c[7][0] + c[7][2], lz * c[7][1] + c[7][3]
        out = max(0.0, -r3y) + max(0.0, -r3x) + max(0.0, r3x - 1.0) + max(0.0, r3y - 1.0) - 0.01
        return (wx, wy, wz, None, not (out > 0.0))
    return None

def triangles(d):
    """world-space triangles of the draw (triangle lists and strips), with the vertices' morph"""
    pos = positions(d)
    if not pos: return []
    wv = [world_vertex(d, v) for v in pos]
    if not wv or wv[0] is None: return []
    idx = indices(d)
    first = d.baseVertex + d.minIndex if d.indexed else 0
    def vert(i):
        k = (d.baseVertex + i - (d.baseVertex + d.minIndex)) if d.indexed else i
        return wv[k] if 0 <= k < len(wv) else None
    tris = []
    if d.indexed and idx is not None:
        if d.primType == 4:
            for t in range(len(idx) // 3): tris.append((vert(idx[3 * t]), vert(idx[3 * t + 1]), vert(idx[3 * t + 2])))
        elif d.primType == 5:
            for t in range(len(idx) - 2): tris.append((vert(idx[t]), vert(idx[t + 1]), vert(idx[t + 2])))
    else:
        if d.primType == 4:
            for t in range(len(wv) // 3): tris.append((wv[3 * t], wv[3 * t + 1], wv[3 * t + 2]))
        elif d.primType == 5:
            for t in range(len(wv) - 2): tris.append((wv[t], wv[t + 1], wv[t + 2]))
    return [t for t in tris if all(x is not None for x in t)]

LINE = re.compile(r'census (\d+) d(\d+) (DIP|DP) (\d+) prims (\d+) verts .*?\| VS ([0-9a-f]{16})(.*?) PS ([0-9a-f]{16}) \| z(\d+) w(\d+) f(\d+) b(\d+) (\d+)/(\d+) cull(\d+) cw(\w+) at(\d+) bias (\w+)/(\w+) \| t0 (.*?) \| file (\S+) \| hook: (\S)(.*)$')

def log_lines(folder):
    out = collections.defaultdict(dict)
    heads = {}
    for line in open(os.path.join(folder, 'bridge32.log'), encoding='latin-1'):
        m = LINE.search(line)
        if m:
            g = m.groups()
            out[int(g[0])][int(g[1])] = {'call': g[2], 'prims': int(g[3]), 'verts': int(g[4]), 'vs': g[5], 'vsname': g[6].strip(), 'ps': g[7],
                                         'z': int(g[8]), 'w': int(g[9]), 'f': int(g[10]), 'b': int(g[11]), 'src': int(g[12]), 'dst': int(g[13]), 'cull': int(g[14]),
                                         'cw': g[15], 'at': int(g[16]), 'bias': (g[17], g[18]), 't0': g[19], 'file': g[20], 'hook': g[21], 'rest': g[22].strip()}
            continue
        m2 = re.search(r'census (\d+) begins with frame (\d+): (.*)$', line)
        if m2: heads[int(m2.group(1))] = m2.group(3)
    return out, heads

def census_dir(folder): return os.path.join(folder, 'sims3-terrain-census')

def load(folder, mark):
    ds = []
    for f in sorted(os.listdir(census_dir(folder))):
        if f.startswith('census%d_' % mark) and f.endswith('.bin'): ds.append(read_file(os.path.join(census_dir(folder), f)))
    return ds

def cmd_summary(folder):
    lines, heads = log_lines(folder)
    for mark in sorted(lines):
        L = lines[mark]
        print('=== census %d: %s' % (mark, heads.get(mark, '')))
        print('    %d draws; by hook decision: %s' % (len(L), dict(collections.Counter(x['hook'] + (' ' + x['rest'].split(',')[0] if x['rest'].startswith('kind') else '') for x in L.values()))))
        by = collections.Counter()
        prims = collections.Counter()
        for x in L.values():
            key = (x['vs'], x['vsname'][:30], x['hook'], x['rest'].split(',')[0])
            by[key] += 1; prims[key] += x['prims']
        for key, n in by.most_common(40):
            print('    %4d draws %7d prims  VS %s %-30s hook %s %s' % (n, prims[key], key[0], key[1], key[2], key[3]))

def cmd_draws(folder, mark):
    lines, heads = log_lines(folder)
    L = lines.get(mark, {})
    print('=== census %d: %s' % (mark, heads.get(mark, '')))
    for d in load(folder, mark):
        x = L.get(d.draw, {})
        tris = triangles(d)
        xs = [p[0] for t in tris for p in t if p[4]]; ys = [p[1] for t in tris for p in t if p[4]]; zs = [p[2] for t in tris for p in t if p[4]]
        ms = [p[3] for t in tris for p in t if p[3] is not None]
        outside = sum(1 for t in tris for p in t if not p[4])
        edges = [math.dist((t[a][0], t[a][2]), (t[b][0], t[b][2])) for t in tris[:400] for a, b in ((0, 1), (1, 2), (2, 0))]
        tex = ' '.join('s%d:%016x' % (s, d.tex[s][4]) for s in range(16) if d.tex[s][0] & 0x7F == 1 and d.tex[s][4])
        print('d%04u %-9s PS %016x %s prim%d %5d tris %5d verts | blend %d %d/%d z%d w%d f%d cull%d bias %s | hook %s %s' % (
            d.draw, d.name, d.psHash, 'DIP' if d.indexed else 'DP', d.primType, d.primCount, d.numVertices, d.rs['blend'], d.rs['src'], d.rs['dst'], d.rs['zenable'], d.rs['zwrite'], d.rs['zfunc'], d.rs['cull'],
            '%08x/%08x' % (d.rs['depthbias'], d.rs['slopebias']), x.get('hook', '?'), x.get('rest', '').split(',')[0]))
        if xs:
            print('      x %.1f..%.1f z %.1f..%.1f y %.2f..%.2f | edge %.2f..%.2f | morph %s | %d corners outside the chunk | decl %s' % (
                min(xs), max(xs), min(zs), max(zs), min(ys), max(ys), min(edges) if edges else 0, max(edges) if edges else 0,
                ('%.2f..%.2f' % (min(ms), max(ms))) if ms else '-', outside, ' '.join('%s%d:t%d@%d' % (USAGE[e[3]] if e[3] < len(USAGE) else e[3], e[4], e[2], e[1]) for e in d.decl)))
        print('      textures %s' % tex)

def bary_height(t, x, z):
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = (t[0][:3], t[1][:3], t[2][:3])
    den = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2)
    if abs(den) < 1e-9: return None
    a = ((z1 - z2) * (x - x2) + (x2 - x1) * (z - z2)) / den
    b = ((z2 - z0) * (x - x2) + (x0 - x2) * (z - z2)) / den
    c = 1 - a - b
    if a < 1e-4 or b < 1e-4 or c < 1e-4: return None   # strictly inside: a point on a shared edge is not an overlap
    return a * y0 + b * y1 + c * y2

def cmd_overlap(folder, mark, cell=2.0):
    """samples every cell's centre: which ray-traced terrain draws cover it, at which heights"""
    lines, heads = log_lines(folder)
    L = lines.get(mark, {})
    print('=== census %d: %s' % (mark, heads.get(mark, '')))
    cover = collections.defaultdict(list)
    for d in load(folder, mark):
        x = L.get(d.draw, {})
        if x.get('hook') != 'C' or 'kind 1' not in x.get('rest', ''): continue
        for t in triangles(d):
            if not all(p[4] for p in t): continue
            xs = [p[0] for p in t]; zs = [p[2] for p in t]
            for i in range(int(math.floor(min(xs) / cell)), int(math.floor(max(xs) / cell)) + 1):
                for k in range(int(math.floor(min(zs) / cell)), int(math.floor(max(zs) / cell)) + 1):
                    h = bary_height(t, (i + 0.371) * cell, (k + 0.613) * cell)   # off the grid lines of the game's meshes
                    if h is not None: cover[(i, k)].append((d.draw, d.name, h))
    multi = {c: v for c, v in cover.items() if len(set(s[0] for s in v)) > 1}
    print('%d cells of %.0f units covered by ray-traced terrain; %d covered by more than one draw' % (len(cover), cell, len(multi)))
    pairs = collections.defaultdict(list)
    for c, v in multi.items():
        v = sorted(v, key=lambda s: s[0])
        for a in range(len(v)):
            for b in range(a + 1, len(v)):
                if v[a][0] != v[b][0]: pairs[(v[a][0], v[a][1], v[b][0], v[b][1])].append(abs(v[a][2] - v[b][2]))
    for key, dh in sorted(pairs.items(), key=lambda kv: -len(kv[1]))[:40]:
        dh.sort()
        print('  d%04u %-9s with d%04u %-9s: %6d cells, height apart: median %.3f, max %.3f, %d cells within 0.001' % (
            key[0], key[1], key[2], key[3], len(dh), dh[len(dh) // 2], dh[-1], sum(1 for x in dh if x < 0.001)))

if __name__ == '__main__':
    folder = sys.argv[1]
    what = sys.argv[2] if len(sys.argv) > 2 else 'summary'
    if what == 'summary': cmd_summary(folder)
    elif what == 'draws': cmd_draws(folder, int(sys.argv[3]))
    elif what == 'overlap': cmd_overlap(folder, int(sys.argv[3]), float(sys.argv[4]) if len(sys.argv) > 4 else 2.0)

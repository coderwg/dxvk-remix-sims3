"""The Sims 3 camera hook's Remix mod textures, made from the hook's dumps of the game's own textures
(rtx-remix/logs/sims3-textures): game data, made locally and not in the repository.

Sims3Water/textures/water_<material>_n.dds (milestones 94, 99): each water material's ripples, its strength
                     baked in -- pool: the game's own first wave map of the pool shader; pond, sea, object
                     water: the game's wave map of the town's water (the object water's own map was never
                     dumped). From the wave-map dumps (milestone 93): --waves <dump folder>.
Sims3GlassBumps/     (milestone 109): the bumpy glass -- one clear glass per bump map of the game's bumpy
                     glass shaders, named by the bump map's own runtime hash (the hook presents a bumpy glass
                     draw with its bump map at stage 0 once this mod names it), mod.usda and
                     textures/bump_<hash>_n.dds. From the bump-map dumps (bump_<hash>_<ps>_s<stage>_<w>x<h>_
                     <format>_k<slope scale x 1000>.raw): --bumps <dump folder>. A separate mod: generated.

The runtime samples a translucent material's normal map with the draw's TEXCOORD0 -- for the game's water
its scrolling wave coordinate, for the bumpy glass the bump map's own coordinate the hook promotes there --
and decodes it as an unsigned octahedral hemisphere direction in r, g (packing.slangh
unsignedOctahedralToHemisphereDirection); the game's maps are tangent-space with z up.

Uncompressed DDS, DX10 header, R8G8B8A8_UNORM, full mip chain: the runtime reads it as linear data. Run:
    python make_textures.py [--waves <folder with water_*.raw>] [--bumps <folder with bump_*.raw>]
"""
import argparse
import math
import os
import re
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
WATER_OUT = os.path.join(HERE, 'Sims3Water', 'textures')   # the ripples
BUMPS_MOD = os.path.join(HERE, 'Sims3GlassBumps')          # the bumpy glass

# the wave maps the normal maps are made from (the hook's dump names), and how much each is flattened:
# the game adds its two pool layers and +2 to their up component (PS d40999e5 / 85c0a78a), about halving
# the slopes; one layer at half slope comes close. The pool's: x 0.1 more, the user's choice by eye with
# the runtime's Translucent "Normal Strength" (run 202; first given as 0.65, a mistype), which scales the
# decoded slopes the same way
TOWN_WAVES = 'water_f74b4657dbfd60bc_s0_256x256_Q8W8V8U8.raw'
WAVES = {
    'water_pool_n.dds': ('water_85c0a78a614b15d3_s1_256x256_A8R8G8B8.raw', 0.5 * 0.1),
    'water_pond_n.dds': (TOWN_WAVES, 0.5),
    'water_sea_n.dds': (TOWN_WAVES, 0.5),
    'water_object_n.dds': (TOWN_WAVES, 0.5),
}
# the bumpy glass shaders (sims3cam::kNamedGlass entries with a bump stage): an older dump of another shader
# is not made into a material (910a56f2, an unplayable lot's windows: the clear glass without its bumps since
# milestone 111, the user's choice)
BUMPY_PS = {'572773cfbd618a3a', '8fe3ce7c5fbc6234', 'a9336d35a25143ae'}
BUMP_NAME = re.compile(r'^bump_([0-9A-F]{16})_([0-9a-f]{16})_s(\d+)_(\d+)x(\d+)_([A-Z0-9]+)_k(-?\d+)\.raw$')


def write_dds_rgba8(path, levels):
    """levels: [(width, height, bytes RGBA8 rows packed)], level 0 first."""
    w, h = levels[0][0], levels[0][1]
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000          # CAPS HEIGHT WIDTH PITCH PIXELFORMAT MIPMAPCOUNT
    pixfmt = struct.pack('<II4sIIIII', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)   # DDPF_FOURCC 'DX10'
    caps = 0x1000 | (0x400000 | 0x8 if len(levels) > 1 else 0)          # TEXTURE (| MIPMAP | COMPLEX)
    header = struct.pack('<IIIIIII', 124, flags, h, w, w * 4, 0, len(levels)) + b'\0' * 44 + pixfmt + struct.pack('<IIIII', caps, 0, 0, 0, 0)
    dx10 = struct.pack('<IIIII', 28, 3, 0, 1, 0)                         # DXGI_FORMAT_R8G8B8A8_UNORM, TEXTURE2D, array 1
    with open(path, 'wb') as f:
        f.write(b'DDS ' + header + dx10)
        for lw, lh, data in levels:
            assert len(data) == lw * lh * 4
            f.write(data)


def read_wave_map(path, w, h):
    """The game's wave map as unit tangent-space normals (z up), rows top to bottom."""
    data = open(path, 'rb').read()
    signed = 'Q8W8V8U8' in os.path.basename(path)
    out = []
    for i in range(w * h):
        if signed:                                   # U (x), V (y), W (z), Q: signed bytes / 127
            u, v, wz, _ = struct.unpack_from('<bbbb', data, i * 4)
            x, y, z = u / 127.0, v / 127.0, wz / 127.0
        else:                                        # A8R8G8B8 in memory B, G, R, A; 2t - 1
            b, g, r = data[i * 4], data[i * 4 + 1], data[i * 4 + 2]
            x, y, z = r / 127.5 - 1.0, g / 127.5 - 1.0, b / 127.5 - 1.0
        out.append((x, y, z))
    return out


def decode_dxt5(data, w, h):
    """BC3 / DXT5 to RGBA bytes per texel, rows top to bottom."""
    out = [None] * (w * h)
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * 16
            a0, a1 = data[o], data[o + 1]
            abits = int.from_bytes(data[o + 2:o + 8], 'little')
            alphas = [a0, a1] + ([((6 - i) * a0 + i * a1) // 7 for i in range(1, 7)] if a0 > a1 else
                                 [((4 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255])
            c0, c1, cbits = struct.unpack_from('<HHI', data, o + 8)
            def rgb(c): return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)
            p0, p1 = rgb(c0), rgb(c1)
            cols = [p0, p1, tuple((2 * p0[i] + p1[i]) // 3 for i in range(3)), tuple((p0[i] + 2 * p1[i]) // 3 for i in range(3))]
            for ty in range(4):
                for tx in range(4):
                    x, y = bx * 4 + tx, by * 4 + ty
                    if x >= w or y >= h:
                        continue
                    t = ty * 4 + tx
                    r, g, b = cols[cbits >> (2 * t) & 3]
                    out[y * w + x] = (r, g, b, alphas[abits >> (3 * t) & 7])
    return out


def read_bump_map(path, fmt, w, h, scale):
    """A bumpy glass's bump map as unit tangent-space normals, decoded as its pixel shader does: DXT5 / A8L8 --
    x in alpha, y in blue, each 2.00787401 t - 1.03937006, z = sqrt(1 - x^2 - y^2), the slopes scaled by the
    shader's constant (c14.x), and n = s x T - s y B + z N; Q8W8V8U8 -- signed as the game's wave maps."""
    data = open(path, 'rb').read()
    if fmt in ('DXT5', 'A8L8'):
        # A8L8 (a door's flat 4x4 stand-in, run 218): L, A per texel, sampled as (L, L, L, A)
        texels = decode_dxt5(data, w, h) if fmt == 'DXT5' else [(data[i * 2], data[i * 2], data[i * 2], data[i * 2 + 1]) for i in range(w * h)]
        out = []
        for r, g, b, a in texels:
            x, y = 2.00787401 * a / 255.0 - 1.03937006, 2.00787401 * b / 255.0 - 1.03937006
            z = math.sqrt(max(0.0, 1.0 - x * x - y * y))
            out.append((scale * x, -scale * y, z))
        return out
    if fmt in ('Q8W8V8U8', 'A8R8G8B8', 'X8R8G8B8'):
        return [(x * scale, y * scale, z) for x, y, z in read_wave_map(path, w, h)]
    raise ValueError('bump map format %s not decoded' % fmt)


def normalize(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) or 1.0
    return (v[0] / l, v[1] / l, v[2] / l)


def octahedral_rgba(n):
    """hemisphereDirectionToUnsignedOctahedral (packing.slangh), as bytes r, g; b 0, a 255."""
    x, y, z = n
    z = max(z, 0.0)
    s = abs(x) + abs(y) + z
    px, py = x / s, y / s
    ox, oy = px + py, px - py
    to8 = lambda t: max(0, min(255, int(round((t * 0.5 + 0.5) * 255.0))))
    return bytes((to8(ox), to8(oy), 0, 255))


def normal_levels(normals, w, h):
    levels = []
    cur = normals
    while True:
        levels.append((w, h, b''.join(octahedral_rgba(normalize(n)) for n in cur)))
        if w == 1 and h == 1:
            return levels
        nw, nh = max(1, w // 2), max(1, h // 2)
        nxt = []
        for y in range(nh):
            for x in range(nw):
                acc = [0.0, 0.0, 0.0]
                for dy in (0, 1):
                    for dx in (0, 1):
                        sx, sy = min(w - 1, 2 * x + dx), min(h - 1, 2 * y + dy)
                        n = cur[sy * w + sx]
                        acc[0] += n[0]; acc[1] += n[1]; acc[2] += n[2]
                nxt.append(normalize(acc))
        cur, w, h = nxt, nw, nh


BUMPS_DOC = '''The Sims 3 camera hook's bumpy glass (milestone 109), GENERATED by ../make_textures.py --bumps
from the hook's bump-map dumps: game data, made locally and not in the repository. One material per bump map
of the game's bumpy glass shaders (sims3cam::kNamedGlass entries with a bump stage), named by the bump map's
own runtime hash: the hook presents a bumpy glass draw with its bump map at stage 0 once this file names it
(it reads the names at start), otherwise with the clear glass marker. Each is the clear glass of Sims3Glass
-- thin, IOR 1.5 -- with the bump map, converted to the runtime's octahedral normals, as its normal map,
sampled with the bump map's own coordinate. Deployed to Game/Bin/rtx-remix/mods/Sims3GlassBumps/.'''


def bump_material(h):
    return '''        def Material "mat_{h}"
        {{
            token outputs:mdl:displacement.connect = </RootNode/Looks/mat_{h}/Shader.outputs:out>
            token outputs:mdl:surface.connect = </RootNode/Looks/mat_{h}/Shader.outputs:out>
            token outputs:mdl:volume.connect = </RootNode/Looks/mat_{h}/Shader.outputs:out>

            def Shader "Shader"
            {{
                uniform token info:implementationSource = "sourceAsset"
                uniform asset info:mdl:sourceAsset = @AperturePBR_Translucent.mdl@
                uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_Translucent"
                float inputs:ior_constant = 1.5
                color3f inputs:transmittance_color = (0.97, 0.98, 0.98)
                float inputs:transmittance_measurement_distance = 1
                bool inputs:thin_walled = 1
                float inputs:thin_wall_thickness = 0.005
                bool inputs:use_diffuse_layer = 0
                asset inputs:normalmap_texture = @./textures/bump_{h}_n.dds@
                token outputs:out
            }}
        }}
'''.format(h=h)


def make_bumps(folder):
    out_tex = os.path.join(BUMPS_MOD, 'textures')
    os.makedirs(out_tex, exist_ok=True)
    made = {}
    for name in sorted(os.listdir(folder)):
        m = BUMP_NAME.match(name)
        if not m:
            continue
        h, ps, stage, w, hh, fmt, k = m.groups()
        if h in made or ps not in BUMPY_PS:
            continue
        w, hh, scale = int(w), int(hh), int(k) / 1000.0
        normals = read_bump_map(os.path.join(folder, name), fmt, w, hh, scale)
        write_dds_rgba8(os.path.join(out_tex, 'bump_%s_n.dds' % h), normal_levels(normals, w, hh))
        made[h] = (ps, fmt, w, hh, scale)
        print('wrote bump_%s_n.dds from %s (PS %s, %s %dx%d, slope scale %.3f)' % (h, name, ps, fmt, w, hh, scale))
    body = ''.join(bump_material(h) + ('\n' if i + 1 < len(made) else '') for i, h in enumerate(made))
    usda = '#usda 1.0\n(\n    defaultPrim = "RootNode"\n    doc = """' + BUMPS_DOC + '"""\n    metersPerUnit = 1\n    upAxis = "Y"\n)\n\n' \
           'def Xform "RootNode"\n{\n    def Scope "Looks"\n    {\n' + body + '    }\n}\n'
    with open(os.path.join(BUMPS_MOD, 'mod.usda'), 'w', newline='\n') as f:
        f.write(usda)
    print('wrote', os.path.join(BUMPS_MOD, 'mod.usda'), 'with', len(made), 'materials')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--waves', help='folder with the hook\'s wave-map dumps (water_*.raw)')
    ap.add_argument('--bumps', help='folder with the hook\'s bump-map dumps (bump_*.raw)')
    args = ap.parse_args()
    if not args.waves and not args.bumps:
        ap.error('nothing to make: pass --waves and / or --bumps')
    if args.waves:
        os.makedirs(WATER_OUT, exist_ok=True)
        for name, (src, flatten) in WAVES.items():
            path = os.path.join(args.waves, src)
            w, h = 256, 256
            normals = [normalize((x * flatten, y * flatten, z)) for x, y, z in read_wave_map(path, w, h)]
            write_dds_rgba8(os.path.join(WATER_OUT, name), normal_levels(normals, w, h))
            print('wrote', os.path.join(WATER_OUT, name), 'from', src)
    if args.bumps:
        make_bumps(args.bumps)


if __name__ == '__main__':
    main()

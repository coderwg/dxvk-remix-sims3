"""The Sims 3 camera hook's Remix mod textures (milestones 93, 94): written next to mod.usda in textures/.

frost.dds            the shower door's frosting: the translucent material's diffuse layer takes its colour and
                     its opacity from transmittance_texture (rgb, alpha) -- without a texture the layer is off
                     (translucent_surface_material_interaction: diffuseOpacity 0), so M88's door stayed clear.
water_clear_n.dds    the ripples of clear water (lot pools): the game's own first wave map of the pool shader
water_natural_n.dds  the ripples of natural water (ponds, the sea): the game's wave map of the town's water
                     Both are made from the hook's wave-map dumps (rtx-remix/logs/sims3-textures, milestone 93):
                     game data, so they are not in the repository; pass the dump folder with --waves.
                     The runtime samples a translucent material's normal map with the draw's TEXCOORD0 -- for
                     the game's water its scrolling wave coordinate, so the game's motion drives the ripples --
                     and decodes it as an unsigned octahedral hemisphere direction in r, g (packing.slangh
                     unsignedOctahedralToHemisphereDirection); the game's maps are tangent-space with z up.

Uncompressed DDS, DX10 header, R8G8B8A8_UNORM, full mip chain: the runtime reads it as linear data. Run:
    python make_textures.py [--waves <folder with water_*.raw>]
"""
import argparse
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'Sims3Glass', 'textures')

# the wave maps the normal maps are made from (the hook's dump names), and how much each is flattened:
# the game adds its two pool layers and +2 to their up component (PS d40999e5 / 85c0a78a), about halving
# the slopes; one layer at half slope comes close
WAVES = {
    'water_clear_n.dds': ('water_85c0a78a614b15d3_s1_256x256_A8R8G8B8.raw', 0.5),
    'water_natural_n.dds': ('water_f74b4657dbfd60bc_s0_256x256_Q8W8V8U8.raw', 0.5),
}


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


def flat_levels(w, h, rgba):
    levels = []
    while True:
        levels.append((w, h, bytes(rgba) * (w * h)))
        if w == 1 and h == 1:
            return levels
        w, h = max(1, w // 2), max(1, h // 2)


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--waves', help='folder with the hook\'s wave-map dumps (water_*.raw)')
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    # very frosted (a test the user asked for, run 201: "clearly frosted"): a pale grey-white diffuse layer at 85 % opacity
    write_dds_rgba8(os.path.join(OUT, 'frost.dds'), flat_levels(4, 4, (224, 228, 232, 217)))
    print('wrote', os.path.join(OUT, 'frost.dds'))
    if not args.waves:
        print('no --waves: the water normal maps are not made')
        return
    for name, (src, flatten) in WAVES.items():
        path = os.path.join(args.waves, src)
        w, h = 256, 256
        normals = [normalize((x * flatten, y * flatten, z)) for x, y, z in read_wave_map(path, w, h)]
        write_dds_rgba8(os.path.join(OUT, name), normal_levels(normals, w, h))
        print('wrote', os.path.join(OUT, name), 'from', src)


if __name__ == '__main__':
    main()

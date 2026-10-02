"""The Sims 3 camera hook's Remix mod textures (milestone 93): written next to mod.usda in textures/.

frost.dds    the shower door's frosting: the translucent material's diffuse layer takes its colour and
             its opacity from transmittance_texture (rgb, alpha) -- without a texture the layer is off
             (translucent_surface_material_interaction: diffuseOpacity 0), so M88's door stayed clear.

Uncompressed DDS, DX10 header, R8G8B8A8_UNORM, full mip chain: the runtime reads it as linear data and
gamma-decodes colours itself. Run: python make_textures.py
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'Sims3Glass', 'textures')


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


def main():
    os.makedirs(OUT, exist_ok=True)
    # very frosted (a test the user asked for): a pale grey-white diffuse layer at 85 % opacity
    write_dds_rgba8(os.path.join(OUT, 'frost.dds'), flat_levels(4, 4, (224, 228, 232, 217)))
    print('wrote', os.path.join(OUT, 'frost.dds'))


if __name__ == '__main__':
    main()

"""Builds the lamp reporter script mod: compiles LampReporter.cs against the game's own script
assemblies and wraps the DLL into a package the game loads from Mods\\Packages.

    python build_scriptmod.py <folder with the game's script assemblies> [<output package>]

The assemblies (mscorlib, System, SimIFace, ScriptCore, Sims3GameplaySystems, Sims3GameplayObjects,
UI, Sims3Metadata) come out of the game's own packages (Game\\Bin\\simcore, scripts and
gameplay .package, resource type S3SA); they are the user's, and are not part of this repository.

The package holds two resources, as every pure script mod does:
  S3SA 0x073FAA07  the assembly (version 1, no checksum, an all-zero table: stored as it is)
  _XML 0x0333406C  the tuning of the class that starts the mod; its instance is the FNV-1 64 hash
                   of the lower-cased full class name, which is how the game finds the class
"""
import os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLASS = 'Sims3RtxHook.LampReporter'
ASSEMBLY = 'Sims3RtxLamps'
REFS = ['mscorlib', 'System', 'SimIFace', 'ScriptCore', 'Sims3GameplaySystems', 'Sims3GameplayObjects', 'UI', 'Sims3Metadata']
CSC = r'C:\Windows\Microsoft.NET\Framework\v3.5\csc.exe'

def fnv1_64(s):
    h = 0xcbf29ce484222325
    for c in s.lower().encode('utf-8'):
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
        h ^= c
    return h

def s3sa(dll):
    blocks = (len(dll) + 511) // 512
    return struct.pack('<BI', 1, 0x2BC4F79F) + bytes(64) + struct.pack('<H', blocks) + bytes(8 * blocks) + dll + bytes(blocks * 512 - len(dll))

def package(resources):
    """resources: [(type, group, instance, bytes)], stored uncompressed. DBPF 2.0, index version 3."""
    head = 96
    body = b''; rows = []
    for (t, g, inst, data) in resources:
        rows.append((t, g, inst, head + len(body), len(data)))
        body += data
    index = struct.pack('<I', 0)   # no field shared by the rows
    for (t, g, inst, off, size) in rows:
        index += struct.pack('<IIIIIIIHH', t, g, inst >> 32, inst & 0xFFFFFFFF, off, size | 0x80000000, size, 0, 1)
    h = bytearray(head)
    h[0:4] = b'DBPF'
    struct.pack_into('<II', h, 4, 2, 0)
    struct.pack_into('<I', h, 0x24, len(rows))
    struct.pack_into('<I', h, 0x2C, len(index))
    struct.pack_into('<I', h, 0x3C, 3)
    struct.pack_into('<I', h, 0x40, head + len(body))
    return bytes(h) + body + index

def main():
    if len(sys.argv) < 2: print(__doc__); return 1
    refs = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, ASSEMBLY + '.package')
    dll = os.path.join(HERE, 'out', ASSEMBLY + '.dll')
    os.makedirs(os.path.dirname(dll), exist_ok=True)
    cmd = [CSC, '/nologo', '/target:library', '/nostdlib+', '/noconfig', '/optimize+', '/debug-', '/warn:4', '/out:' + dll]
    cmd += ['/r:' + os.path.join(refs, r + '.dll') for r in REFS]
    cmd += [os.path.join(HERE, 'LampReporter.cs')]
    r = subprocess.run(cmd, capture_output=True, text=True)
    print((r.stdout + r.stderr).strip() or 'compiled without a message')
    if r.returncode != 0: print('COMPILE_FAILED'); return 2
    data = open(dll, 'rb').read()
    xml = ('<?xml version="1.0" encoding="utf-8"?>\r\n<base>\r\n  <Current_Tuning>\r\n'
           '    <!--Starts the lamp reporter of the RTX Remix camera hook. It reads the lamps; it changes nothing.-->\r\n'
           '    <kInstantiator value="True" />\r\n  </Current_Tuning>\r\n</base>\r\n').encode('utf-8')
    pkg = package([(0x0333406C, 0, fnv1_64(CLASS), xml), (0x073FAA07, 0, fnv1_64(ASSEMBLY), s3sa(data))])
    open(out, 'wb').write(pkg)
    print('%s: %d bytes (assembly %d bytes; tuning instance %016x for %s; assembly instance %016x)' % (out, len(pkg), len(data), fnv1_64(CLASS), CLASS, fnv1_64(ASSEMBLY)))
    print('BUILD_OK')
    return 0

if __name__ == '__main__': sys.exit(main())

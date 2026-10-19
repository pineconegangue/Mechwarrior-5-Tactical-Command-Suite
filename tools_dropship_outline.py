#!/usr/bin/env python3
"""
Build the dropship outlines HUDDisplay draws (assets/dropships/<name>.txt) from the game's own meshes.

WHY FROM THE MESHES. The maps only knew a dropship's turrets, which then floated in mid-air. The hull is a
static mesh in the pak (or, for the DLC3 Union, ~40 parts placed by its blueprint), so its real shape - in
the actor's own frame, the same frame the turrets are attached in - can be read once, offline, and shipped
as a few hundred points. Checked: a Leopard turret's attach offset (-6081, 0, 1421) lies 1.1 m from the hull.

Each file, cm in the actor's frame (X forward, Y right, Z up), one polygon per line:
    top   x y x y ...        the outline seen from above (LIDAR scope)
    slice z x y x y ...      the outline at height z (three of them: the 3D map / TacOps)
    side  x z x z ...        the profile along the centreline (the 3D map / TacOps)

Usage: tools_dropship_outline.py <pak> <outdir>     (needs numpy + opencv)
"""
import os, sys, re, math, struct, importlib.util
import numpy as np, cv2

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('pe', os.path.join(here, 'tools_paperdoll_extract.py'))
P = importlib.util.module_from_spec(spec); spec.loader.exec_module(P)

def get(f, tree, entries, path):
    d, fn = path.rsplit('/', 1)
    key = [k for k in tree if k.endswith(d + '/')]
    if not key or fn not in tree[key[0]]: raise KeyError(path)
    b = P.payload(f, P.decode_entry(entries, tree[key[0]][fn]))
    # tools_paperdoll_extract.payload leaves 4 bytes of the entry header on uncompressed entries
    if b[:4] == b'\0\0\0\0' and (b[4:8] == bytes.fromhex('c1832a9e') or fn.endswith('.uexp')): b = b[4:]
    return b

def positions(uexp):
    """LOD0's FPositionVertexBuffer: Stride 12, NumVertices, then the bulk array (12, N) and N float3s.
    Not 4-byte aligned in the cooked file, so every byte offset is tried."""
    d = uexp
    for o in range(0, len(d) - 16):
        if d[o] == 12 and d[o + 1:o + 4] == b'\0\0\0':
            a, n, b, m = struct.unpack_from('<iiii', d, o)
            if b == 12 and n == m and 3 < n < 2000000 and o + 16 + n * 12 <= len(d):
                return np.frombuffer(d[o + 16:o + 16 + n * 12], dtype='<f4').reshape(-1, 3).astype(float)
    raise RuntimeError('no position buffer')

# ---- a cooked blueprint's component placement (names, imports, exports, tagged properties) ----
class Pkg:
    def __init__(s, ua, ux):
        s.x = ux; d = ua; o = [0]
        def i32():
            v = struct.unpack_from('<i', d, o[0])[0]; o[0] += 4; return v
        def i64():
            v = struct.unpack_from('<q', d, o[0])[0]; o[0] += 8; return v
        def fstr():
            n = i32()
            if n == 0: return ''
            if n > 0: r = d[o[0]:o[0] + n - 1].decode('utf-8', 'replace'); o[0] += n; return r
            r = d[o[0]:o[0] - 2 * n - 2].decode('utf-16-le', 'replace'); o[0] += -2 * n; return r
        i32(); leg = i32()
        if leg != -4: i32()
        i32(); i32(); nc = i32(); o[0] += nc * 20
        s.total = i32(); fstr(); i32()
        ncount = i32(); noff = i32(); i32(); i32(); ec = i32(); eo = i32(); ic = i32(); io = i32()
        o[0] = noff; s.names = []
        for _ in range(ncount): s.names.append(fstr()); o[0] += 4
        def fname():
            a = i32(); b = i32(); return s.names[a] if b == 0 else '%s_%d' % (s.names[a], b - 1)
        o[0] = io; s.imports = []
        for _ in range(ic):
            fname(); cn = fname(); i32(); s.imports.append((cn, fname()))
        o[0] = eo; s.exports = []
        for _ in range(ec):
            st = o[0]; i32(); i32(); i32(); i32(); on = fname(); i32(); ss = i64(); so = i64(); o[0] = st + 104
            s.exports.append(dict(name=on, size=ss, off=so - s.total))
    def objname(s, i):
        return s.imports[-i - 1][1] if i < 0 else (s.exports[i - 1]['name'] if i > 0 else None)
    def props(s, e):
        d = s.x; o = e['off']; end = o + e['size']; out = {}
        def fn(at):
            a, b = struct.unpack_from('<ii', d, at); return s.names[a] if 0 <= a < len(s.names) else '?'
        while o < end - 8:
            name = fn(o); o += 8
            if name == 'None': break
            typ = fn(o); o += 8; size, _ = struct.unpack_from('<ii', d, o); o += 8; extra = None
            if typ == 'StructProperty': extra = fn(o); o += 24
            elif typ == 'BoolProperty': o += 1
            elif typ in ('ByteProperty', 'EnumProperty', 'ArrayProperty', 'SetProperty'): extra = fn(o); o += 8
            elif typ == 'MapProperty': o += 16
            hg = d[o]; o += 1
            if hg: o += 16
            val = d[o:o + size]; o += size
            if typ == 'StructProperty' and extra in ('Vector', 'Rotator') and size == 12: out[name] = struct.unpack('<3f', val)
            elif typ == 'ArrayProperty' and extra == 'ObjectProperty':
                n = struct.unpack_from('<i', val, 0)[0]
                out[name] = [s.objname(struct.unpack_from('<i', val, 4 + 4 * k)[0]) for k in range(n)]
            elif typ == 'ObjectProperty' and size == 4: out[name] = s.objname(struct.unpack('<i', val)[0])
        return out
    def export(s, name):
        for e in s.exports:
            if e['name'] == name: return e

def rotz(v, yaw_deg):
    a = math.radians(yaw_deg); c, sn = math.cos(a), math.sin(a)
    return v @ np.array([[c, -sn, 0], [sn, c, 0], [0, 0, 1]]).T

def union_assembled(f, tree, entries):
    """DLC3 UnionDropship_Destructible: DefaultSceneRoot > PivotPoint > ~40 parts (yaw-turned copies)."""
    base = 'MW5Mercs/Content/DLC3/Objects/Dropships/Union/'
    bp = Pkg(get(f, tree, entries, base + 'UnionDropship_Destructible.uasset'),
             get(f, tree, entries, base + 'UnionDropship_Destructible.uexp'))
    pivot = np.array(bp.props(bp.export('PivotPoint_GEN_VARIABLE')).get('RelativeLocation', (0, 0, 0)))
    cache, out = {}, []
    for e in bp.exports:
        m = re.match(r'(Union_[A-Za-z]+?)(?:_\d{3}|_Front|_Back|_Left|_Right)_GEN_VARIABLE$', e['name'])
        if not m: continue
        mesh = m.group(1) + '_DM0_STM'
        if mesh not in cache: cache[mesh] = positions(get(f, tree, entries, base + 'Models/' + mesh + '.uexp'))
        pr = bp.props(e)
        loc = np.array(pr.get('RelativeLocation', (0, 0, 0))); rot = pr.get('RelativeRotation', (0, 0, 0))
        out.append(rotz(cache[mesh], rot[1]) + loc + pivot)
    return np.vstack(out)

def outline(pts2, cell=50.0, close=15, eps=100.0):
    mn = pts2.min(0) - 500; ij = ((pts2 - mn) / cell).astype(int)
    W, H = ij.max(0) + 3
    img = np.zeros((H, W), np.uint8); img[ij[:, 1], ij[:, 0]] = 255
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (close, close))
    img = cv2.morphologyEx(img, cv2.MORPH_CLOSE, k)
    cs, _ = cv2.findContours(img, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    c = max(cs, key=cv2.contourArea)
    return cv2.approxPolyDP(c, eps / cell, True)[:, 0, :].astype(float) * cell + mn

def write(path, name, v):
    z0, z1 = v[:, 2].min(), v[:, 2].max()
    lines = ['# %s - generated by tools_dropship_outline.py from the game\'s mesh (%d vertices); cm, actor frame' % (name, len(v)),
             '# bounds %.0f %.0f %.0f  %.0f %.0f %.0f' % tuple(list(v.min(0)) + list(v.max(0)))]
    fmt = lambda p: ' '.join('%.0f %.0f' % (a, b) for a, b in p)
    lines.append('top ' + fmt(outline(v[:, :2], eps=80)))
    for a, b in ((0.0, 0.25), (0.25, 0.55), (0.55, 1.0)):
        lo, hi = z0 + a * (z1 - z0), z0 + b * (z1 - z0)
        m = (v[:, 2] >= lo) & (v[:, 2] <= hi)
        if m.sum() > 50: lines.append('slice %.0f %s' % ((lo + hi) / 2, fmt(outline(v[m][:, :2]))))
    mid = np.abs(v[:, 1]) < 0.1 * (v[:, 1].max() - v[:, 1].min())
    lines.append('side ' + fmt(outline(v[mid][:, [0, 2]])))
    open(path, 'w').write('\n'.join(lines) + '\n')
    print('wrote', path, len(lines) - 2, 'polygons')

def main():
    pak, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    f, tree, entries, _ = P.load_index(pak)
    write(os.path.join(outdir, 'leopard.txt'), 'Leopard',
          positions(get(f, tree, entries, 'MW5Mercs/Content/Objects/Dropships/Leopard/Models/Leopard_Exterior_STM.uexp')))
    write(os.path.join(outdir, 'union_destructible.txt'), 'Union (DLC3 destructible)', union_assembled(f, tree, entries))
    # The Union (2026-10-19 fix): Union_Base_STM is the hull; Union_Animated_SKM is only the animated legs / doors
    # ring (bind pose z 1181..5554, +-5450), a CHILD of the hull mesh. Checked against both blueprints:
    #   UnionDropship_Takeoff (DLC3): DefaultSceneRoot > StaticMesh (Union_Base_STM, at 0) > SkeletalMesh (z -6200);
    #     its thrusters, also children of the hull, sit at z -3850 (hull bottom -4699) and the decal locator at -5850,
    #     under the legs (-5019). The old file put the HULL at -6200 - 62 m too low in the 3D slices / profile.
    #   UnionDropship (base game): Scene > Union_Base_STM (z +5834) > Union_Animated_SKM (z -6322 from the hull);
    #     thrusters at hull -4515, the legs' capsules at z 481 under the feet (z 693), spawners on the hull.
    v = positions(get(f, tree, entries, 'MW5Mercs/Content/Objects/Dropships/Union/Model/Union_Base_STM.uexp'))
    legs = positions(get(f, tree, entries, 'MW5Mercs/Content/Objects/Dropships/Union/Model/Union_Animated_SKM.uexp'))
    write(os.path.join(outdir, 'union_takeoff.txt'), 'Union (takeoff)', np.vstack([v, legs + np.array([0, 0, -6200.0])]))
    hull = np.array([0, 0, 5833.8])
    write(os.path.join(outdir, 'union.txt'), 'Union (base game)', np.vstack([v + hull, legs + hull + np.array([0, 0, -6321.9])]))
    # DLC7 Broadsword (Broadsword_C < AbstractLeopard_C < MWDropship): Exterior = Broadsword_STM at the root, scale
    # 0.6 (its turret spawners carry 1/0.6 to undo it).
    b = positions(get(f, tree, entries, 'MW5Mercs/Content/DLC7/Objects/Ships/Broadsword/Meshes/Broadsword_STM.uexp'))
    write(os.path.join(outdir, 'broadsword.txt'), 'Broadsword (DLC7)', b * 0.6)

if __name__ == '__main__':
    main()

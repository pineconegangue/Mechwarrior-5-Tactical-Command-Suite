#!/usr/bin/env python3
"""
Extract MW5 per-chassis paperdoll textures from a UE4 .pak into greyscale PNG masks.

WHY MASKS AND NOT COLOUR IMAGES. The shipped textures are line art on black: the game
tints them per damage state rather than showing them as-is. So the useful output is a
single-channel intensity mask, which Direct2D can hand to FillOpacityMask with a solid
brush - exactly the tint-by-condition the HUD already does with flat rectangles.

WHY EVERY PART SHARES ONE CANVAS. Each part texture is a full-size canvas with that part
drawn in its true position, so the consumer composites by drawing them all at the same
rect. No layout table, no per-chassis offsets, nothing to get wrong.

Usage:
    paperdoll_extract.py survey  <pak> [more.pak ...]
    paperdoll_extract.py extract <outdir> <pak> [more.pak ...] [--only=catapult,atlas]
"""
import struct, os, sys, zlib, json, re

PAK_MAGIC = 0x5A6F12E1
PKG_TAG   = 0x9E2A83C1

# ---------------------------------------------------------------- pak index ----
def read_footer(f, size):
    f.seek(max(0, size - 4096)); tail = f.read(4096)
    for i in range(len(tail) - 4, -1, -1):
        if struct.unpack_from('<I', tail, i)[0] == PAK_MAGIC:
            ver = struct.unpack_from('<i', tail, i + 4)[0]
            io_, is_ = struct.unpack_from('<qq', tail, i + 8)
            enc = tail[i - 1] if i >= 1 else 0
            return ver, io_, is_, enc
    raise RuntimeError("pak footer magic not found")

def rdstr(b, o):
    n = struct.unpack_from('<i', b, o)[0]; o += 4
    if n == 0: return '', o
    if n > 0:  return b[o:o + n - 1].decode('utf-8', 'replace'), o + n
    n = -n;    return b[o:o + n * 2 - 2].decode('utf-16-le', 'replace'), o + n * 2

def load_index(path):
    f = open(path, 'rb'); size = os.path.getsize(path)
    ver, io_, is_, enc = read_footer(f, size)
    if enc:
        raise RuntimeError("%s: index is encrypted - not supported" % os.path.basename(path))
    f.seek(io_); idx = f.read(is_)
    o = 0
    mount, o = rdstr(idx, o)
    o += 4 + 8                              # NumEntries, PathHashSeed
    if struct.unpack_from('<i', idx, o)[0]: o += 4 + 36
    else:                                   o += 4
    has_fd = struct.unpack_from('<i', idx, o)[0]; o += 4
    if not has_fd:
        raise RuntimeError("%s: no full directory index" % os.path.basename(path))
    fdo, fds = struct.unpack_from('<qq', idx, o); o += 16 + 20
    enc_size = struct.unpack_from('<i', idx, o)[0]; o += 4
    entries = idx[o:o + enc_size]

    f.seek(fdo); fd = f.read(fds)
    o2 = 0; ndir = struct.unpack_from('<i', fd, o2)[0]; o2 += 4
    tree = {}
    for _ in range(ndir):
        d, o2 = rdstr(fd, o2)
        nf = struct.unpack_from('<i', fd, o2)[0]; o2 += 4
        files = {}
        for _ in range(nf):
            fn, o2 = rdstr(fd, o2)
            files[fn] = struct.unpack_from('<i', fd, o2)[0]; o2 += 4
        tree[d] = files
    return f, tree, entries, mount

def decode_entry(b, off):
    """FPakFile::DecodePakEntry - the compact per-file record the v8+ index stores."""
    V = struct.unpack_from('<I', b, off)[0]; off += 4
    o32 = (V >> 31) & 1; u32 = (V >> 30) & 1; s32 = (V >> 29) & 1
    cm  = (V >> 23) & 0x3f
    encrypted = (V >> 22) & 1
    nblk = (V >> 6) & 0xffff
    blocksize = (V & 0x3f) << 11 if nblk else 0
    offset = struct.unpack_from('<I' if o32 else '<Q', b, off)[0]; off += 4 if o32 else 8
    usize  = struct.unpack_from('<I' if u32 else '<Q', b, off)[0]; off += 4 if u32 else 8
    if cm == 0:
        size = usize
    else:
        size = struct.unpack_from('<I' if s32 else '<Q', b, off)[0]; off += 4 if s32 else 8
    blocks = []
    if nblk:
        if nblk == 1 and not encrypted:
            blocks = None                       # single implicit block
        else:
            for _ in range(nblk):
                s, e = struct.unpack_from('<qq', b, off); off += 16
                blocks.append((s, e))
    return dict(offset=offset, usize=usize, size=size, cm=cm,
                encrypted=encrypted, nblk=nblk, blocksize=blocksize, blocks=blocks)

# The FPakEntry struct is repeated immediately before each file's payload. Its serialized
# length varies with compression, so it is computed rather than assumed.
def payload(f, e):
    if e['encrypted']:
        raise RuntimeError("encrypted entry")
    hdr = 8 + 8 + 8 + 20 + 1 + 4            # offset,size,usize,hash,cm(as int32 in v8+),flags
    hdr = 8 + 8 + 8 + 20 + 1 + 4
    if e['cm'] != 0:
        nb = e['nblk'] if e['nblk'] else 1
        hdr += 4 + nb * 16 + 4              # block array + blocksize
    f.seek(e['offset'] + hdr)
    raw = f.read(e['size'])
    if e['cm'] == 0:
        return raw[:e['usize']]
    try:
        return zlib.decompress(raw)
    except zlib.error:
        return zlib.decompressobj().decompress(raw)

# ------------------------------------------------------------- texture parse ----
BLOCK = {'PF_DXT1': 8, 'PF_DXT5': 16, 'PF_BC5': 16, 'PF_BC7': 16, 'PF_DXT3': 16}

def mip_bytes(fmt, w, h):
    if fmt in BLOCK:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * BLOCK[fmt]
    if fmt in ('PF_B8G8R8A8', 'PF_R8G8B8A8'): return w * h * 4
    if fmt == 'PF_G8': return w * h
    return None

def parse_texture(uasset, uexp, ubulk=None):
    """Returns (fmt, w, h, nmips, top_mip_bytes).

    WHERE THE PIXELS ACTUALLY START. The mip's bulk header carries OffsetInFile, and that
    offset is relative to the START OF THE PACKAGE - i.e. to the .uasset, not the .uexp.
    So the payload begins at (OffsetInFile - len(uasset)) inside the .uexp. Guessing
    "the last mip-sized run of bytes" instead happens to look plausible on a single-mip
    texture and is silently wrong: it decoded a Catapult centre torso into an off-centre
    smear, which still read as a mech once eight of them were composited. The rule below
    is derived, not fitted, and it puts the top mip first for multi-mip textures too.
    """
    i = uexp.find(b'PF_')
    if i < 0: raise RuntimeError("no pixel format string")
    j = uexp.find(b'\x00', i)
    fmt = uexp[i:j].decode('ascii')
    w, h, _slices = struct.unpack_from('<iii', uexp, i - 4 - 12)
    if not (1 <= w <= 8192 and 1 <= h <= 8192):
        raise RuntimeError("implausible dimensions %dx%d" % (w, h))
    nmips = struct.unpack_from('<i', uexp, j + 1 + 4)[0]
    if not (1 <= nmips <= 16): nmips = 1

    o = j + 1 + 12                      # FirstMip, NumMips, (slices/strip) then bulk header
    flags, ec, sod = struct.unpack_from('<iii', uexp, o)
    off64 = struct.unpack_from('<q', uexp, o + 12)[0]
    # 2026-10-19 PAPERDOLL FIX: +4 - measured, 397/397 base-game textures; the DXT5 empty block
    # only lines up there. Reading 4 bytes early split every block across two (4x4 noise dolls).
    start = off64 - len(uasset) + 4
    need = mip_bytes(fmt, w, h)
    if need is not None and sod != need:
        raise RuntimeError("mip size %d != expected %d for %s %dx%d" % (sod, need, fmt, w, h))
    if 0 <= start and start + sod <= len(uexp):
        return fmt, w, h, nmips, uexp[start:start + sod]

    # NOT AN ERROR - THE PAYLOAD IS IN A SEPARATE FILE.
    #
    # A texture can keep its pixels in a sibling .ubulk instead of inline in the .uexp, and then
    # OffsetInFile is an offset into THAT file, not into the package. Computing start against the
    # .uexp gives a negative number, which is what 196 mod textures were failing on - including
    # every Clan chassis in YetAnotherLegendaryMech. The base game happens to ship everything
    # inline, which is why this went unnoticed until a Mist Lynx turned up in a mission.
    if ubulk:
        for cand in (off64, 0, len(ubulk) - sod):
            if 0 <= cand and cand + sod <= len(ubulk):
                return fmt, w, h, nmips, ubulk[cand:cand + sod]
    raise RuntimeError("payload %d..%d outside uexp (%d) and no usable ubulk (%d)"
                       % (start, start + sod, len(uexp), len(ubulk) if ubulk else 0))

# ------------------------------------------------------------------ decoders ----
def _bc_colours(c0, c1, opaque):
    rgb = lambda c: (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
    p = [rgb(c0), rgb(c1)]
    if c0 > c1 or opaque:
        p.append(tuple((2 * p[0][k] + p[1][k]) // 3 for k in range(3)))
        p.append(tuple((p[0][k] + 2 * p[1][k]) // 3 for k in range(3)))
    else:
        p.append(tuple((p[0][k] + p[1][k]) // 2 for k in range(3)))
        p.append((0, 0, 0))
    return p

def decode_luma(fmt, data, w, h):
    """Straight to an 8-bit intensity mask - colour is discarded, since the consumer tints."""
    out = bytearray(w * h)
    if fmt == 'PF_DXT5':
        # 2026-10-19 PAPERDOLL FIX: the shape is in the ALPHA block (colour is flat white).
        alp = bytearray(w * h)
        bi = 0
        for by in range(0, h, 4):
            for bx in range(0, w, 4):
                a0, a1 = data[bi], data[bi + 1]
                bits = int.from_bytes(data[bi + 2:bi + 8], 'little')
                if a0 > a1: pal = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
                else:       pal = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
                bi += 16
                for k in range(16):
                    px, py = bx + (k & 3), by + (k >> 2)
                    if px < w and py < h: alp[py * w + px] = pal[(bits >> (3 * k)) & 7]
        if max(alp) - min(alp) > 32:
            return bytes(alp)
    if fmt in ('PF_DXT1', 'PF_DXT5', 'PF_DXT3'):
        step = BLOCK[fmt]; coff = 8 if fmt != 'PF_DXT1' else 0
        bi = 0
        for by in range(0, h, 4):
            for bx in range(0, w, 4):
                c0, c1 = struct.unpack_from('<HH', data, bi + coff)
                cbits = struct.unpack_from('<I', data, bi + coff + 4)[0]
                pal = _bc_colours(c0, c1, fmt != 'PF_DXT1')
                bi += step
                for y in range(4):
                    for x in range(4):
                        px, py = bx + x, by + y
                        if px >= w or py >= h: continue
                        r, g, b = pal[(cbits >> (2 * (4 * y + x))) & 3]
                        out[py * w + px] = (r * 299 + g * 587 + b * 114) // 1000
        return bytes(out)
    if fmt in ('PF_B8G8R8A8', 'PF_R8G8B8A8'):
        # THE SHAPE IS IN ALPHA. THE COLOUR CHANNELS ARE A FLAT FILL.
        #
        # Every paperdoll texture the base game ships is PF_B8G8R8A8, and every one of them
        # draws the silhouette in the alpha channel over flat white or flat black RGB. Picking
        # "whichever channel varies more" looked safe and was not: a flat BLACK RGB field with a
        # white outline bleeding into it still has a spread of 255, so the Demolisher, Partisan
        # and Jedgar all chose luminance and decoded to an unreadable near-solid block. Spread
        # cannot tell a silhouette from a two-tone field.
        #
        # So alpha is the rule, and luminance is only the fallback for a texture whose alpha is
        # genuinely constant (fully opaque art, where the drawing must be in the colour).
        lum = bytearray(w * h)
        alp = bytearray(w * h)
        for i in range(w * h):
            b, g, r, a = data[i*4], data[i*4+1], data[i*4+2], data[i*4+3]
            if fmt == 'PF_R8G8B8A8': r, b = b, r
            lum[i] = (r * 299 + g * 587 + b * 114) // 1000
            alp[i] = a
        return bytes(alp if (max(alp) - min(alp)) > 32 else lum)
    if fmt == 'PF_G8':
        return bytes(data[:w * h])
    raise RuntimeError("no decoder for " + fmt)

def normalise_polarity(grey):
    """Masks must be bright where the part is, because the renderer paints colour THROUGH them.

    Most ship that way - dark field, bright line work. A handful of the vehicle masks are the
    other way round: a dark silhouette on a white field, which through an opacity mask paints the
    whole tile and leaves a hole where the vehicle is. Mean brightness tells them apart with no
    ambiguity, since a mask is mostly background by construction.
    """
    if not grey: return grey
    if sum(grey) // len(grey) > 127:
        return bytes(255 - v for v in grey)
    return grey

def write_png_grey(path, w, h, grey):
    grey = normalise_polarity(grey)
    raw = b''.join(b'\x00' + grey[y * w:(y + 1) * w] for y in range(h))
    ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as fp:
        fp.write(b'\x89PNG\r\n\x1a\n')
        fp.write(ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)))
        fp.write(ch(b'IDAT', zlib.compress(raw, 9)))
        fp.write(ch(b'IEND', b''))

# --------------------------------------------------------------------- driver ----

# Three naming conventions ship in the wild and the consumer must not have to know about
# any of them: stock mechs use "<part>_<layer>", newer stock ones "Front_Armor_<Part>",
# and mod chassis their own prefix ("GRN_ARM_CT"). All are folded to "<part>_<layer>"
# with the part names the contract already uses.
PART_ALIAS = {
    'hd': 'head', 'h': 'head', 'head': 'head', 'body': 'ct',
    'ct': 'ct', 'lt': 'lt', 'rt': 'rt', 'la': 'la', 'ra': 'ra', 'll': 'll', 'rl': 'rl',
    'rct': 'rct', 'rlt': 'rlt', 'rrt': 'rrt', 'ctr': 'rct', 'ltr': 'rlt', 'rtr': 'rrt',
    # Rear faces written as one glued token, and the cockpit/head-plate spellings some mods use.
    'ctrear': 'rct', 'ltrear': 'rlt', 'rtrear': 'rrt',
    'rearct': 'rct', 'rearlt': 'rlt', 'rearrt': 'rrt',
    'hp': 'head', 'cp': 'head', 'cockpit': 'head',
}
# Multi-token spellings: UrbanTest writes Devastator_Arm_Left_Armor, others Left_Torso.
PART_PAIRS = {
    ('arm', 'left'): 'la', ('left', 'arm'): 'la', ('l', 'arm'): 'la',
    ('arm', 'right'): 'ra', ('right', 'arm'): 'ra', ('r', 'arm'): 'ra',
    ('leg', 'left'): 'll', ('left', 'leg'): 'll', ('l', 'leg'): 'll',
    ('leg', 'right'): 'rl', ('right', 'leg'): 'rl', ('r', 'leg'): 'rl',
    ('left', 'torso'): 'lt', ('torso', 'left'): 'lt', ('l', 'shoulders'): 'lt',
    ('right', 'torso'): 'rt', ('torso', 'right'): 'rt', ('r', 'shoulders'): 'rt',
    ('center', 'torso'): 'ct', ('centre', 'torso'): 'ct', ('torso', 'center'): 'ct',
    ('center', 'body'): 'ct', ('front', 'body'): 'ct',
}
ARMOR_TOKENS    = {'armor', 'armour', 'arnor', 'ext', 'exterior'}
INTERNAL_TOKENS = {'internal', 'internals', 'int', 'interior'}
VULN_TOKENS     = {'vulnerability', 'vulnerable', 'vuln'}
NOISE_TOKENS    = {'paperdoll', 'target', 'texture', 'textures', 'hdtextures', 'yagdm',
                   'yaml', 'f', 'front', 'mech', 'battlemech', 'omnimech', 'dmg', 'damage'}

# Which files in a Target folder are paperdoll masks. Deliberately generous: it must cover the
# "Arnor" misspelling that ships in YAGM, and _ext/_int as well as the spelled-out words.
PAPERDOLL_FILE = re.compile(r'(paperdoll|_armor|_armour|_arnor|_internal|_internals|_ext|_int|'
                            r'body_side|body_top|_dd_\d+x\d+_)', re.I)

def canon_name(short, chassis=''):
    """<part>_<layer>, or the name unchanged when it is not a paperdoll mask.

    TOKENISED, NOT SUBSTRING. Matching "int" inside the raw string made YAGDM_CLINT_CT_Armor
    an INTERNAL, because "clint_" contains "int_" - so a chassis whose name happens to embed a
    layer word had its armour and internal masks collapse onto one file, and whichever was
    written second won. Splitting into tokens first removes that whole class of accident.
    """
    t = short.lower().replace(' ', '_').replace('-', '_')
    toks = [x for x in re.split(r'[^a-z0-9]+', t) if x]
    # Strip the chassis name's own tokens, then generic noise and bare numbers.
    chtoks = set(x for x in re.split(r'[^a-z0-9]+', chassis.lower()) if x)
    toks = [x for x in toks if x not in chtoks and x not in NOISE_TOKENS]
    stripped = [re.sub(r'\d+$', '', x) or x for x in toks]

    # A part letter can be GLUED to the chassis name with no separator: YAGM ships
    # "YAGDM_MongooseCT_TARGET_ARMOR_REAR", where CT is the part and Mongoose the chassis. Peel a
    # chassis token off the front of any token that still carries one.
    peeled = []
    for x in stripped:
        for c in sorted(chtoks, key=len, reverse=True):
            if len(c) > 2 and x.startswith(c) and len(x) > len(c):
                x = x[len(c):]
                break
        peeled.append(x)
    stripped = peeled

    layer = None
    for x in stripped:
        if x in VULN_TOKENS:     layer = 'vulnerability'; break
        if x in ARMOR_TOKENS:    layer = 'armor'; break
        if x in INTERNAL_TOKENS: layer = 'internal'; break

    part = None
    for i in range(len(stripped) - 1):
        p = PART_PAIRS.get((stripped[i], stripped[i + 1]))
        if p: part = p; break
    if not part:
        for x in stripped:
            if x in PART_ALIAS: part = PART_ALIAS[x]; break

    # "ARM" is ambiguous: Grendel writes GRN_ARM_CT for the ARMOUR of the centre torso, while
    # UrbanTest writes Devastator_Arm_Left for the left ARM. Only read it as armour when it did
    # not already supply the part and some other token did.
    if layer is None and 'arm' in stripped and part not in (None, 'la', 'ra'):
        layer = 'armor'

    if part and layer:
        if 'rear' in stripped and part in ('ct', 'lt', 'rt'):
            part = {'ct': 'rct', 'lt': 'rlt', 'rt': 'rrt'}[part]
        return '%s_%s' % (part, layer)
    if part == 'ct' and not layer and ('side' in stripped or 'top' in stripped):
        return None   # vehicle body view - handled by the caller
    return t

# PBR CHANNEL MAPS ARE NOT PAPERDOLLS. The loose "_int"/"_ext"/"_armor" spellings are a good
# filter inside a Target folder and a bad one everywhere else - they matched ship interiors,
# Leopard hull trim, holotable bakedowns and a quonset hut, each of which then got its own
# junk chassis folder. A paperdoll mask never carries a material-channel suffix, so these
# suffixes are a clean exclusion rather than a blocklist of the particular assets that leaked.
NOT_A_MASK = re.compile(r'(_clr|_mrs|_orm|_ems|_nrm|_msk|_bcr|_met|_dirt|_wear|_rgh|_ao|'
                        r'_builtdata|_bakedown|_lod\d|lod\d_)', re.I)

def find_textures(tree):
    """chassis -> asset stem -> {uasset, uexp, ubulk} entry offsets.

    WHY THIS NO LONGER LOOKS FOR A "Target" FOLDER. It used to require Target to appear as a
    path segment, which is the convention for mechs and for the four tanks that shipped with
    the base game. It is not the convention for everything else, and requiring it silently
    hid the Manticore, Riverjack, Schrek and Scorpion, the Elemental, the DLC1 Warrior engine
    pods - and the one generic TURRET paperdoll, which is the only turret art in the game and
    which I had previously reported as not existing. Three separate sweeps missed it because
    all three asked the same wrong question.

    So the rule is now about the FILE, not the folder: anything named like a paperdoll mask is
    one. The chassis is the nearest enclosing folder that is not a texture or Target wrapper.
    """
    TEXDIR = ('textures', 'texture', 'hdtextures', 'hdtexture', 'target', 'materials')
    out = {}
    for d, files in tree.items():
        norm = d.replace('\\', '/')
        seg = [x for x in norm.split('/') if x]
        # The UI folder holds paperdoll FRAMES and widgets, not per-unit masks.
        if any(x.lower() == 'ui' for x in seg): continue
        k = len(seg)
        while k > 0 and seg[k - 1].lower() in TEXDIR: k -= 1
        if k == 0: continue
        folder = seg[k - 1]

        # WIDENING THE FOLDER RULE MEANT TIGHTENING THE FILE RULE. The old regex leans on
        # "_int"/"_ext", which is fine inside a Target folder and catastrophic outside one:
        # it matched Door_ExtraLarge_Inner_STM and a pile of biome and material assets, and
        # the sweep created junk chassis folders for them. So the loose spellings are only
        # honoured under Target, which is where they were always meant to apply; anywhere
        # else the filename has to actually say "paperdoll".
        # 'Target' for a unit's own folder, 'Targets' for the shared objective-marker folders.
        under_target = any(x.lower() in ('target', 'targets') for x in seg)
        for fn, ei in files.items():
            if 'paperdoll' in fn.lower():
                pass
            elif under_target and PAPERDOLL_FILE.search(fn) and not NOT_A_MASK.search(fn):
                pass
            else:
                continue
            stem, ext = os.path.splitext(fn)
            if ext.lower() not in ('.uasset', '.uexp', '.ubulk'): continue

            # A shared "_common" folder names nothing, so take the identity from the file:
            # Paperdoll_Turret_Generic_Body_Side -> turret_generic.
            chassis = folder
            if folder.startswith('_'):
                t = stem.lower()
                if t.startswith('paperdoll_'): t = t[10:]
                for suf in ('_body_side', '_body_top', '_side', '_top', '_front', '_back'):
                    if t.endswith(suf): t = t[:-len(suf)]; break
                if t: chassis = t
            key = chassis.lower()
            slot = out.setdefault(key, {}).setdefault(stem, {})
            slot[ext.lower().lstrip('.')] = ei
    return out

MECH_PARTS = {'head', 'ct', 'lt', 'rt', 'la', 'ra', 'll', 'rl',
              'rct', 'rlt', 'rrt'}

# The single view a one-body unit is drawn with, best first. Side elevation reads as a vehicle
# at a glance where a plan view reads as a blob, so it wins; top is the last resort, and is all
# the Igor and the Warrior engine pods ever had.
BODY_PREFERENCE = ('_body_side', '_side_uix', '_side', '_front_uix', '_front',
                   '_body_top', '_top_uix', '_top', '_back')

def canonicalise_bodies(outdir):
    """Give every one-body unit a mask at a PREDICTABLE path, and stock paperdolls/_generic.

    WHY THE RENDERER MUST NOT DO THIS. The art is named six different ways - demolisher_body_side,
    igor_side, condor_side_uix, battlearmor_front_uix, and the MissileCarrier folder holds a file
    called lrm_carrier_body_side. Teaching the consumer all six spellings means an alias table in
    C++ that silently goes stale the next time a mod invents a seventh. Resolving it here instead
    costs one extra file per vehicle and leaves the renderer with a single rule: <chassis>/body.png.

    A folder holding mech part masks is left alone - those composite, and have no single body.
    """
    made, generic = 0, {}
    for chassis in sorted(os.listdir(outdir)):
        cd = os.path.join(outdir, chassis)
        if not os.path.isdir(cd) or chassis.startswith('_'): continue
        names = [f[:-4] for f in os.listdir(cd) if f.endswith('.png')]
        if not names: continue

        # A FOLDER CAN HOLD BOTH. "scorpion" is a mech AND a tank, and the two share a chassis
        # name and therefore a folder; an earlier version skipped any folder containing mech
        # part masks and so silently dropped the Scorpion tank's body - and the Battle Armor's,
        # which also carries a stray ct_armor. The two lookups do not collide (the mech doll
        # asks for ct_armor, a vehicle asks for body.png), so the right test is not "does this
        # folder look like a mech" but "is there a body view here at all".
        pick = None
        for suf in BODY_PREFERENCE:
            for n in sorted(names):
                if n.endswith(suf) and n.split('_')[0] not in MECH_PARTS:
                    pick = n; break
            if pick: break
        if not pick: continue
        with open(os.path.join(cd, pick + '.png'), 'rb') as fp: blob = fp.read()
        with open(os.path.join(cd, 'body.png'), 'wb') as fp: fp.write(blob)
        made += 1

    # Two objective structures ship their art in a shared "markers" folder, named by unit
    # rather than by folder, so neither is reachable by chassis name. Split them out. "fct" is
    # the front elevation, which is the view the other one-body units use.
    # WHICH TEXTURE BELONGS TO WHICH UNIT IS TAKEN FROM THE WIDGET, NOT FROM THE FILE NAME.
    # GeneratorPaperDoll references Generic_Generator_DD, RadarTowerPaperDoll references
    # Generic_ComsTower_DD, and SatelliteDishPaperDoll references Radar_fct_armor - so the file
    # called "radar" is the SATELLITE DISH, and the radar tower's art is the one called ComsTower.
    # Guessing from the names, as an earlier pass did, files the dish under the tower.
    for unit, src in (('artillery',  'markers/artillery_fct_armor.png'),
                      ('satellite',  'markers/radar_fct_armor.png'),
                      ('radartower', 'markers/generic_comstower_dd_130x132_side.png'),
                      ('generator',  'markers/generic_generator_dd_130x132_side.png')):
        sp = os.path.join(outdir, *src.split('/'))
        if not os.path.exists(sp): continue
        ud = os.path.join(outdir, unit); os.makedirs(ud, exist_ok=True)
        with open(sp, 'rb') as fp: blob = fp.read()
        with open(os.path.join(ud, 'body.png'), 'wb') as fp: fp.write(blob)
        made += 1

    # ONE GENERIC PER CONTRACT KIND. The consumer falls back to _generic/<kind>.png when a unit
    # has no art of its own, so every kind it can publish needs an entry - otherwise an
    # unrecognised VTOL gets a tank silhouette, which is worse than a box because it is a
    # confident lie. The representative for each is the game's own: its generic turret doll where
    # one exists, and otherwise the most neutral member of the type.
    GENERIC = {
        'tank':       'demolisher',
        'turret':     'turret_generic',
        'vtol':       'warrior',
        'vtoldual':   'igor',
        'artillery':  'artillery',
        'generator':  'generator',
        'radartower': 'radartower',
        'satellite':  'satellite',
    }
    gd = os.path.join(outdir, '_generic'); os.makedirs(gd, exist_ok=True)
    missing = []
    for kind, src in sorted(GENERIC.items()):
        sp = os.path.join(outdir, src, 'body.png')
        if not os.path.exists(sp): missing.append('%s (wanted %s)' % (kind, src)); continue
        with open(sp, 'rb') as fp: blob = fp.read()
        with open(os.path.join(gd, kind + '.png'), 'wb') as fp: fp.write(blob)
        generic[kind] = True
    if missing: print("  NO GENERIC for: %s" % ', '.join(missing))
    print("canonicalised %d unit bodies; _generic: %s" % (made, sorted(generic)))
    return made

def main():
    if len(sys.argv) < 3: print(__doc__); return 2
    mode = sys.argv[1]
    args = [a for a in sys.argv[2:] if not a.startswith('--')]
    only = None
    force = '--force' in sys.argv
    for a in sys.argv:
        if a.startswith('--only='): only = set(a.split('=', 1)[1].lower().split(','))
    outdir = None
    if mode == 'extract':
        outdir = args.pop(0); os.makedirs(outdir, exist_ok=True)
    elif mode == 'canon':
        return 0 if canonicalise_bodies(args[0]) >= 0 else 1

    stats, manifest, failures = {}, {}, []
    for pak in args:
        try:    f, tree, entries, _ = load_index(pak)
        except Exception as ex:
            print("SKIP %s: %s" % (os.path.basename(pak), ex)); continue
        tex = find_textures(tree)
        print("%s: %d chassis with paperdoll textures" % (os.path.basename(pak), len(tex)))
        for chassis in sorted(tex):
            if only and chassis not in only: continue
            for name, parts in sorted(tex[chassis].items()):
                try:
                    if 'uexp' not in parts or 'uasset' not in parts:
                        raise RuntimeError("missing uasset/uexp pair")
                    uexp   = payload(f, decode_entry(entries, parts['uexp']))
                    uasset = payload(f, decode_entry(entries, parts['uasset']))
                    ubulk  = payload(f, decode_entry(entries, parts['ubulk'])) if 'ubulk' in parts else None
                    fmt, w, h, nmips, mip = parse_texture(uasset, uexp, ubulk)
                    key = "%s %dx%d mips=%d" % (fmt, w, h, nmips)
                    stats[key] = stats.get(key, 0) + 1
                    if mode == 'extract':
                        cd = os.path.join(outdir, chassis); os.makedirs(cd, exist_ok=True)
                        short = name.lower()
                        for pre in (chassis + '_', 'paperdoll_'):
                            if short.startswith(pre): short = short[len(pre):]
                        short = canon_name(short, chassis) or short
                        dst = os.path.join(cd, short + '.png')
                        manifest.setdefault(chassis, []).append(short)
                        if os.path.exists(dst) and not force:   # resume: decoding is the slow part
                            continue
                        write_png_grey(dst, w, h, decode_luma(fmt, mip, w, h))
                except Exception as ex:
                    failures.append("%s/%s: %s" % (chassis, name, ex))
    print("\nformats seen:")
    for k in sorted(stats): print("   %-28s %d" % (k, stats[k]))
    if failures:
        print("\n%d failures:" % len(failures))
        for x in failures[:25]: print("   " + x)
    if mode == 'canon':
        return 0 if canonicalise_bodies(args[0]) >= 0 else 1
    if mode == 'extract':
        with open(os.path.join(outdir, 'manifest.json'), 'w') as fp:
            json.dump({k: sorted(v) for k, v in manifest.items()}, fp, indent=1, sort_keys=True)
        print("\nwrote %d chassis to %s" % (len(manifest), outdir))
    return 0

if __name__ == '__main__':
    sys.exit(main())

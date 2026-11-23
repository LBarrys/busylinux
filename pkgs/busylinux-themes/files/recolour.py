#!/usr/bin/env python3
"""Recolour and trim Breeze Light's cursors and Papirus's icons to Gruvbox.

recolour.py cursors SRC DEST   Breeze_Light's xcursors, in three sizes
recolour.py icons THEME...     Papirus copies, renamed *-Gruvbox, in place
"""
import os, re, shutil, struct, sys

# The GTK theme's palette (Colloid Green Dark Gruvbox).
FG, BG = (0xeb, 0xdb, 0xb2), (0x1d, 0x20, 0x21)

# --- cursors -----------------------------------------------------------------
CURSOR_SIZES = {24, 36, 48}          # 1x, 1.5x and 2x of dwl's 24
# Breeze's accents and the Gruvbox colour that takes each one's place.
CURSOR_ACCENTS = {
    0x46a7ac: 0x689d6a, 0x18c087: 0x689d6a,     # teal, mint -> aqua
    0xd4497f: 0xb16286,                         # pink -> purple
    0xed1515: 0xcc241d, 0xf67400: 0xd65d0e,     # red, orange
    0x3daee9: 0x458588, 0x11d116: 0x98971a,     # blue, green
}


def rgb(c):
    return (c >> 16) & 255, (c >> 8) & 255, c & 255


def hue(r, g, b):
    hi, lo = max(r, g, b), min(r, g, b)
    if hi == lo:
        return 0.0
    d = hi - lo
    if hi == r:
        h = (g - b) / d % 6
    elif hi == g:
        h = (b - r) / d + 2
    else:
        h = (r - g) / d + 4
    return h * 60


ACCENTS = [(hue(*rgb(k)), max(rgb(k)), rgb(v)) for k, v in CURSOR_ACCENTS.items()]


def recolour_pixel(v, cache={}):
    a = v >> 24
    if a == 0:
        return 0
    if v in cache:
        return cache[v]
    # xcursor pixels are premultiplied
    r, g, b = (min(255, c * 255 // a) for c in rgb(v))
    if max(r, g, b) - min(r, g, b) <= 40:
        # white body, black outline and shadow: onto the theme's text and background
        t = (r + g + b) / 765
        out = [BG[i] + (FG[i] - BG[i]) * t for i in range(3)]
    else:
        h = hue(r, g, b)
        _, kv, target = min(ACCENTS, key=lambda k: min(abs(k[0] - h), 360 - abs(k[0] - h)))
        out = [c * max(r, g, b) / kv for c in target]
    out = [min(255, round(c)) * a // 255 for c in out]
    cache[v] = res = a << 24 | out[0] << 16 | out[1] << 8 | out[2]
    return res


def xcursor(path):
    """Rewrite one xcursor file: only CURSOR_SIZES' images, recoloured."""
    b = open(path, 'rb').read()
    if b[:4] != b'Xcur':
        raise SystemExit(path + ': not an xcursor file')
    n = struct.unpack_from('<I', b, 12)[0]
    imgs = []
    for i in range(n):
        typ, size, pos = struct.unpack_from('<III', b, 16 + 12 * i)
        if typ != 0xfffd0002 or size not in CURSOR_SIZES:
            continue
        w, h, xh, yh, delay = struct.unpack_from('<5I', b, pos + 16)
        px = struct.unpack_from('<%dI' % (w * h), b, pos + 36)
        imgs.append((size, struct.pack('<9I', 36, typ, size, 1, w, h, xh, yh, delay) +
                     struct.pack('<%dI' % len(px), *map(recolour_pixel, px))))
    if not imgs:
        raise SystemExit(path + ': has none of the sizes kept')
    out = [struct.pack('<4sIII', b'Xcur', 16, 0x10000, len(imgs))]
    pos = 16 + 12 * len(imgs)
    for size, chunk in imgs:
        out.append(struct.pack('<III', 0xfffd0002, size, pos))
        pos += len(chunk)
    return b''.join(out + [c for _, c in imgs])


def cursors(src, dest):
    os.makedirs(os.path.join(dest, 'cursors'))
    for f in sorted(os.listdir(os.path.join(src, 'cursors'))):
        s, d = os.path.join(src, 'cursors', f), os.path.join(dest, 'cursors', f)
        if os.path.islink(s):
            os.symlink(os.readlink(s), d)
        else:
            with open(d, 'wb') as o:
                o.write(xcursor(s))
    # cursors_scalable (SVG cursors) is left out: wlroots and GTK read xcursors.
    with open(os.path.join(dest, 'index.theme'), 'w') as o:
        o.write('[Icon Theme]\nName=Breeze-Gruvbox\n'
                'Comment=Breeze Light by the KDE VDG, recoloured to Gruvbox for BusyLinux\n')


# --- icons -------------------------------------------------------------------
ICON_SIZES = {16, 22, 32, 48}
# Dropped: 8, 18, 42, 64, 84, 96 and 128, and 24, which is 22 with a 1px margin.
# papirus-folders' colour sets; blue, the default, stays and is recoloured.
FOLDER_SETS = ('adwaita black bluegrey breeze brown carmine cyan darkcyan deeporange green '
               'grey indigo magenta nordic orange palebrown paleorange pink red teal violet '
               'white yaru yellow').split()
FOLDER_SET = re.compile(r'^(folder|user)-(%s)([-.])' % '|'.join(FOLDER_SETS))
# Papirus's palette onto Gruvbox's neutral one.
ICON_COLOURS = {
    '5294e2': '98971a', '4877b1': '79740e',     # folder front, back
    '1d344f': '3c3836', '222c31': '282828',     # folder emblems
    'e4e4e4': 'ebdbb2', 'ffffff': 'fbf1c7',     # paper, highlights
    'dfdfdf': 'ebdbb2', '444444': '3c3836',     # symbolic text (dark, light)
    '3f3f3f': '3c3836', '4f4f4f': '504945', '323232': '282828', '4d4d4d': '504945',
    '8e8e8e': '928374', '607d8b': '928374', '4d646f': '665c54',
    'f44336': 'cc241d', '4caf50': '98971a', '4285f4': '458588', 'ff9800': 'd65d0e',
    '3a87e5': '458588', '3f51b5': '458588', 'fecd38': 'd79921',
}
HEX = re.compile(r'#([0-9a-fA-F]{6})\b')


def svg(path):
    with open(path, encoding='utf-8') as f:
        s = f.read()
    t = HEX.sub(lambda m: '#' + ICON_COLOURS.get(m.group(1).lower(), m.group(1)), s)
    if t != s:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(t)


def size_of(dirname):
    m = re.match(r'(\d+)x\d+', dirname)
    return int(m.group(1)) if m else None


def index(path, name):
    lines, out, skip = open(path, encoding='utf-8').read().split('\n'), [], False
    for line in lines:
        if line.startswith('['):
            sec = line[1:-1]
            skip = sec != 'Icon Theme' and size_of(sec) not in ICON_SIZES
        if skip:
            continue
        k, _, v = line.partition('=')
        if k == 'Name':
            line = 'Name=' + name
        elif k == 'Comment':
            line = 'Comment=Papirus, recoloured to Gruvbox and trimmed for BusyLinux'
        elif k == 'Inherits':
            line = 'Inherits=' + ('Papirus-Gruvbox,hicolor' if name.endswith('-Dark') else 'hicolor')
        elif k in ('Directories', 'ScaledDirectories'):
            line = k + '=' + ','.join(d for d in v.split(',') if size_of(d) in ICON_SIZES)
        elif k.endswith('Sizes'):
            line = k + '=' + ','.join(s for s in v.split(',') if int(s) in ICON_SIZES)
        elif k.endswith('Default') and v.isdigit() and int(v) not in ICON_SIZES:
            line = k + '=' + str(max(s for s in ICON_SIZES if s < int(v)))
        out.append(line)
    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out))


def icons(theme):
    name = os.path.basename(theme.rstrip('/'))
    for d in os.listdir(theme):
        p = os.path.join(theme, d)
        if size_of(d) is not None and size_of(d) not in ICON_SIZES:
            os.unlink(p) if os.path.islink(p) else shutil.rmtree(p)
    for root, dirs, files in os.walk(theme):
        for f in dirs + files:
            p = os.path.join(root, f)
            if os.path.islink(p):
                t = os.readlink(p)
                if '/Papirus/' in t:          # Papirus-Dark borrows Papirus's folders
                    os.unlink(p)
                    os.symlink(t.replace('/Papirus/', '/Papirus-Gruvbox/'), p)
            if f in files and FOLDER_SET.match(f):
                os.unlink(p)
            elif f in files and f.endswith('.svg') and not os.path.islink(p):
                svg(p)
    index(os.path.join(theme, 'index.theme'), name)


def dangling(top):
    """Remove links to removed icons, links to those, and empty folders."""
    while True:
        gone = 0
        for root, dirs, files in os.walk(top):
            for f in dirs + files:
                p = os.path.join(root, f)
                if os.path.islink(p) and not os.path.exists(p):
                    os.unlink(p)
                    gone += 1
        if not gone:
            break
    for root, dirs, files in os.walk(top, topdown=False):
        if not os.listdir(root):
            os.rmdir(root)


if __name__ == '__main__':
    if sys.argv[1:2] == ['cursors'] and len(sys.argv) == 4:
        cursors(sys.argv[2], sys.argv[3])
    elif sys.argv[1:2] == ['icons'] and len(sys.argv) > 2:
        for t in sys.argv[2:]:
            icons(t)
        dangling(os.path.dirname(os.path.abspath(sys.argv[2])))
    else:
        raise SystemExit(__doc__)

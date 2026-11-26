#!/usr/bin/env python3
"""Builds the Overheat file-icon font from the geometry below.

    python3 assets/icons/build_icons.py            # needs fontTools; Pillow for the preview

Writes, next to this script:
  OverheatIcons.ttf    the icon font (committed; the C++ build embeds it, no Python needed)
  preview.png          every icon in the dark and light palettes (only when Pillow is present)
and src/ui/file_icon_glyphs.h, the codepoint of every glyph.

Style: every icon is a monoline mark on a 16x16 grid (y down), drawn with round caps and
round joins and chamfered ("octagonal") corners instead of curves where a letter needs a
corner. File icons are a mark only: the app draws the tinted rounded tile behind it, so the
tile is pixel-snapped and stays crisp at any size. Folders are three layers: a tinted
silhouette, a full-colour outline and, for special folders, a small badge in the body.
See README.md.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

UPM = 1024
GRID = 16
UNIT = UPM / GRID
SW = 1.6  # default stroke width (grid units)


# ---------------------------------------------------------------- geometry
class Builder:
    """Collects contours in grid units. A transform (scale about a centre, then offset,
    optional rotation) maps local points to the grid; stroke widths are given in final
    units so a scaled-down badge keeps a legible stroke."""

    def __init__(self):
        self.contours = []  # (points, is_hole)
        self.tf = [(1.0, 0.0, 0.0, 0.0)]  # stack of (scale, dx, dy, rot_deg)
        self.wmap = lambda w: w  # stroke width -> final width (badges override it)

    # transform -------------------------------------------------------
    def push(self, scale=1.0, dx=0.0, dy=0.0, rot=0.0):
        self.tf.append((scale, dx, dy, rot))

    def pop(self):
        self.tf.pop()

    def map(self, p):
        x, y = p
        for scale, dx, dy, rot in reversed(self.tf):
            if rot:
                a = math.radians(rot)
                cx, cy = x - 8, y - 8
                x, y = 8 + cx * math.cos(a) - cy * math.sin(a), 8 + cx * math.sin(a) + cy * math.cos(a)
            x, y = 8 + (x - 8) * scale + dx, 8 + (y - 8) * scale + dy
        return (x, y)

    # raw contours ----------------------------------------------------
    def add(self, pts, hole=False, mapped=False):
        if not mapped:
            pts = [self.map(p) for p in pts]
        out = []
        for p in pts:
            if not out or abs(p[0] - out[-1][0]) > 1e-4 or abs(p[1] - out[-1][1]) > 1e-4:
                out.append(p)
        if len(out) > 2 and abs(out[0][0] - out[-1][0]) < 1e-4 and abs(out[0][1] - out[-1][1]) < 1e-4:
            out.pop()
        if len(out) >= 3:
            self.contours.append((out, hole))

    # filled shapes ---------------------------------------------------
    def poly(self, *pts, hole=False):
        self.add(list(pts), hole)

    def circle(self, cx, cy, r, hole=False):
        n = max(20, int(r * 14))
        self.add([(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n))
                  for i in range(n)], hole)

    def rrect(self, x, y, w, h, r, hole=False):
        r = min(r, w / 2, h / 2)
        pts = []
        for cx, cy, a0 in ((x + w - r, y + r, -90), (x + w - r, y + h - r, 0),
                           (x + r, y + h - r, 90), (x + r, y + r, 180)):
            for i in range(9):
                a = math.radians(a0 + 90 * i / 8)
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        self.add(pts, hole)

    def ring(self, cx, cy, r, w=SW):
        self.ellipse_ring(cx, cy, r, r, w)

    # strokes ---------------------------------------------------------
    def _stadium(self, a, b, w):
        r = w / 2
        dx, dy = b[0] - a[0], b[1] - a[1]
        ang = math.atan2(dy, dx)
        n = 10
        pts = []
        for i in range(n + 1):  # cap around b
            t = ang - math.pi / 2 + math.pi * i / n
            pts.append((b[0] + r * math.cos(t), b[1] + r * math.sin(t)))
        for i in range(n + 1):  # cap around a
            t = ang + math.pi / 2 + math.pi * i / n
            pts.append((a[0] + r * math.cos(t), a[1] + r * math.sin(t)))
        self.add(pts, mapped=True)

    def line(self, *pts, w=SW, closed=False):
        """Polyline with round caps and joins (a union of stadiums)."""
        w = self.wmap(w)
        m = [self.map(p) for p in pts]
        if closed:
            m.append(m[0])
        if len(m) == 1:
            self.add([(m[0][0] + w / 2 * math.cos(2 * math.pi * i / 16),
                       m[0][1] + w / 2 * math.sin(2 * math.pi * i / 16)) for i in range(16)], mapped=True)
        for a, b in zip(m, m[1:]):
            self._stadium(a, b, w)

    def curve(self, pts, w=SW, caps=True):
        """Smooth centreline (densely sampled) offset into one contour with round caps."""
        w = self.wmap(w)
        m = [self.map(p) for p in pts]
        r = w / 2
        left, right = [], []
        for i, p in enumerate(m):
            a = m[max(i - 1, 0)]
            b = m[min(i + 1, len(m) - 1)]
            dx, dy = b[0] - a[0], b[1] - a[1]
            ln = math.hypot(dx, dy) or 1
            nx, ny = -dy / ln, dx / ln
            left.append((p[0] + nx * r, p[1] + ny * r))
            right.append((p[0] - nx * r, p[1] - ny * r))
        pts_out = list(left)
        if caps:
            e, f = m[-1], m[-2]
            ang = math.atan2(e[1] - f[1], e[0] - f[0])
            for i in range(1, 10):
                t = ang + math.pi / 2 - math.pi * i / 10
                pts_out.append((e[0] + r * math.cos(t), e[1] + r * math.sin(t)))
        pts_out += right[::-1]
        if caps:
            s, g = m[0], m[1]
            ang = math.atan2(s[1] - g[1], s[0] - g[0])
            for i in range(1, 10):
                t = ang + math.pi / 2 - math.pi * i / 10
                pts_out.append((s[0] + r * math.cos(t), s[1] + r * math.sin(t)))
        self.add(pts_out, mapped=True)

    def arc(self, cx, cy, rx, ry, a0, a1, w=SW):
        """Elliptical arc, angles in degrees (0 = right, 90 = down)."""
        n = max(12, int(abs(a1 - a0) / 6))
        pts = [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
                cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]
        self.curve(pts, w)

    def ellipse_ring(self, cx, cy, rx, ry, w=SW):
        w = self.wmap(w)
        n = 64
        pts = [(cx + rx * math.cos(2 * math.pi * i / n), cy + ry * math.sin(2 * math.pi * i / n)) for i in range(n)]
        m = [self.map(p) for p in pts]
        # Offset the closed curve both ways: outer contour plus an inner hole.
        outer, inner = [], []
        for i, p in enumerate(m):
            a, b = m[i - 1], m[(i + 1) % n]
            dx, dy = b[0] - a[0], b[1] - a[1]
            ln = math.hypot(dx, dy) or 1
            nx, ny = dy / ln, -dx / ln
            outer.append((p[0] - nx * w / 2, p[1] - ny * w / 2))
            inner.append((p[0] + nx * w / 2, p[1] + ny * w / 2))
        self.add(outer, mapped=True)
        self.add(inner, hole=True, mapped=True)


# ---------------------------------------------------------------- letters
def letter(b, ch, x, y, w, h, sw=SW):
    """Chamfered monoline capitals in the box (x, y, w, h)."""
    x0, y0, x1, y1 = x, y, x + w, y + h
    xm, ym = x + w / 2, y + h / 2
    c = min(w, h) * 0.3  # chamfer
    L = lambda *p, **kw: b.line(*p, w=sw, **kw)
    if ch == "C":
        L((x1, y0), (x0 + c, y0), (x0, y0 + c), (x0, y1 - c), (x0 + c, y1), (x1, y1))
    elif ch == "O":
        L((x0 + c, y0), (x1 - c, y0), (x1, y0 + c), (x1, y1 - c), (x1 - c, y1), (x0 + c, y1),
          (x0, y1 - c), (x0, y0 + c), closed=True)
    elif ch == "G":
        L((x1, y0), (x0 + c, y0), (x0, y0 + c), (x0, y1 - c), (x0 + c, y1), (x1, y1), (x1, ym), (xm + w * 0.1, ym))
    elif ch == "H":
        L((x0, y0), (x0, y1)); L((x1, y0), (x1, y1)); L((x0, ym), (x1, ym))
    elif ch == "S":
        c = min(c, h * 0.22)
        L((x1, y0), (x0 + c, y0), (x0, y0 + c), (x0, ym - c), (x0 + c, ym), (x1 - c, ym), (x1, ym + c),
          (x1, y1 - c), (x1 - c, y1), (x0, y1))
    elif ch == "J":
        L((x0 + w * 0.25, y0), (x1, y0), (x1, y1 - c), (x1 - c, y1), (x0 + c, y1), (x0, y1 - c))
    elif ch == "T":
        L((x0, y0), (x1, y0)); L((xm, y0), (xm, y1))
    elif ch == "M":
        L((x0, y1), (x0, y0), (xm, ym + h * 0.05), (x1, y0), (x1, y1))
    elif ch == "Z":
        L((x0, y0), (x1, y0), (x0, y1), (x1, y1))
    elif ch == "K":
        L((x0, y0), (x0, y1)); L((x1, y0), (x0 + sw * 0.3, ym), (x1, y1))
    elif ch == "P":
        L((x0, y1), (x0, y0), (x1 - c, y0), (x1, y0 + c), (x1, ym - c * 0.6), (x1 - c, ym + c * 0.4), (x0, ym + c * 0.4))
    elif ch == "R":
        L((x0, y1), (x0, y0), (x1 - c, y0), (x1, y0 + c), (x1, ym - c), (x1 - c, ym), (x0, ym)); L((xm, ym), (x1, y1))
    elif ch == "A":
        L((x0, y1), (x0, y0 + c), (x0 + c, y0), (x1 - c, y0), (x1, y0 + c), (x1, y1)); L((x0, ym + h * 0.08), (x1, ym + h * 0.08))
    elif ch == "V":
        L((x0, y0), (xm, y1), (x1, y0))
    elif ch == "X":
        L((x0, y0), (x1, y1)); L((x1, y0), (x0, y1))
    elif ch == "L":
        L((x0, y0), (x0, y1), (x1, y1))
    elif ch == "0":
        L((x0 + c, y0), (x1 - c, y0), (x1, y0 + c), (x1, y1 - c), (x1 - c, y1), (x0 + c, y1),
          (x0, y1 - c), (x0, y0 + c), closed=True)
    elif ch == "1":
        L((x0, y0 + h * 0.22), (xm, y0), (xm, y1)); L((x0, y1), (x1, y1))
    elif ch == "#":
        d = w * 0.12
        L((x0 + w * 0.35 + d, y0), (x0 + w * 0.35 - d, y1)); L((x0 + w * 0.75 + d, y0), (x0 + w * 0.75 - d, y1))
        L((x0, y0 + h * 0.33), (x1, y0 + h * 0.33)); L((x0, y0 + h * 0.68), (x1, y0 + h * 0.68))
    elif ch == "+":
        L((x0, ym), (x1, ym)); L((xm, ym - w / 2), (xm, ym + w / 2))
    else:
        raise ValueError(ch)


def word(b, text, x, y, w, h, gap, sw=SW):
    for i, ch in enumerate(text):
        letter(b, ch, x + i * (w + gap), y, w, h, sw)


def chevrons(b, sw=SW, slash=False, inset=0.0):
    b.line((5.6 - inset, 4.6), (2.8, 8), (5.6 - inset, 11.4), w=sw)
    b.line((10.4 + inset, 4.6), (13.2, 8), (10.4 + inset, 11.4), w=sw)
    if slash:
        b.line((9.1, 4.2), (6.9, 11.8), w=sw)


# ---------------------------------------------------------------- marks
# Each mark draws into the 16x16 tile; keep it inside ~2.5..13.5 so the tile shows a margin.
MARKS = {}


def mark(name):
    def reg(fn):
        MARKS[name] = fn
        return fn
    return reg


@mark("doc")
def _(b):
    b.line((4.5, 5), (11.5, 5)); b.line((4.5, 8), (11.5, 8)); b.line((4.5, 11), (9, 11))


@mark("c")
def _(b):
    letter(b, "C", 4.6, 3.8, 6.8, 8.4, 1.8)


@mark("cpp")
def _(b):
    letter(b, "C", 2.8, 4, 5.4, 8, 1.7)
    for y in (5.7, 10.3):
        b.line((9.9, y), (13.3, y), w=1.5); b.line((11.6, y - 1.7), (11.6, y + 1.7), w=1.5)


@mark("h")
def _(b):
    letter(b, "H", 4.6, 3.8, 6.8, 8.4, 1.8)


@mark("hpp")
def _(b):
    letter(b, "H", 2.8, 4, 5.2, 8, 1.7)
    for y in (5.7, 10.3):
        b.line((9.9, y), (13.3, y), w=1.5); b.line((11.6, y - 1.7), (11.6, y + 1.7), w=1.5)


@mark("csharp")
def _(b):
    letter(b, "C", 2.8, 4, 5.2, 8, 1.7)
    letter(b, "#", 9.2, 4.6, 4.4, 6.8, 1.3)


@mark("rust")
def _(b):
    # Gear: a ring with eight square teeth and a solid hub.
    b.ring(8, 8, 3.7, 1.7)
    for i in range(8):
        b.push(rot=i * 45)
        b.rrect(7, 2.0, 2.0, 2.6, 0.4)
        b.pop()
    b.circle(8, 8, 1.3)


@mark("python")
def _(b):
    # Two interlocking hooks, one turned 180 degrees: the snakes of the Python mark.
    for r in (0, 180):
        b.push(rot=r)
        b.line((4.0, 10.8), (4.0, 4.8), (5.2, 3.6), (10.6, 3.6), w=1.7)
        b.line((4.0, 7.2), (7.8, 7.2), w=1.7)
        b.pop()


@mark("js")
def _(b):
    word(b, "JS", 2.9, 4.2, 4.4, 7.6, 1.5, 1.6)


@mark("ts")
def _(b):
    word(b, "TS", 2.9, 4.2, 4.4, 7.6, 1.5, 1.6)


@mark("react")
def _(b):
    for r in (0, 60, 120):
        b.push(rot=r)
        b.ellipse_ring(8, 8, 5.6, 2.1, 1.1)
        b.pop()
    b.circle(8, 8, 1.3)


@mark("json")
def _(b):
    b.line((6.6, 3.8), (5.6, 3.8), (4.8, 4.6), (4.8, 7.0), (3.6, 8.0), (4.8, 9.0), (4.8, 11.4), (5.6, 12.2), (6.6, 12.2), w=1.6)
    b.line((9.4, 3.8), (10.4, 3.8), (11.2, 4.6), (11.2, 7.0), (12.4, 8.0), (11.2, 9.0), (11.2, 11.4), (10.4, 12.2), (9.4, 12.2), w=1.6)


@mark("markdown")
def _(b):
    letter(b, "M", 2.8, 4.4, 5.8, 7.2, 1.6)
    b.line((11.6, 4.4), (11.6, 11.4), w=1.6)
    b.line((9.6, 9.4), (11.6, 11.4), (13.6, 9.4), w=1.6)


@mark("cmake")
def _(b):
    b.line((8, 3.6), (13, 12.2), (3, 12.2), closed=True, w=1.6)
    b.line((8, 7.4), (8, 12.2), w=1.6)


@mark("shell")
def _(b):
    b.line((3.8, 5), (7.2, 8), (3.8, 11), w=1.8)
    b.line((8.8, 11.2), (12.4, 11.2), w=1.8)


@mark("git")
def _(b):
    b.line((5.2, 4.2), (5.2, 11.8), w=1.6)
    b.line((10.8, 5.6), (10.8, 6.8), (8.8, 8.8), (6.4, 8.8), (5.2, 10.0), w=1.6)
    b.circle(5.2, 4.0, 1.7); b.circle(5.2, 12.0, 1.7); b.circle(10.8, 4.8, 1.7)


@mark("image")
def _(b):
    b.poly((2.8, 12.4), (6.8, 6.6), (9.2, 9.8), (10.6, 8.2), (13.2, 12.4))
    b.circle(10.8, 4.8, 1.5)


@mark("archive")
def _(b):
    # A box with a zip down the middle and its pull tab.
    b.line((5.6, 3.4), (3.4, 3.4), (3.4, 12.6), (12.6, 12.6), (12.6, 3.4), (10.4, 3.4), w=1.4)
    for i, y in enumerate((2.6, 4.4, 6.2)):
        b.rrect(6.6 if i % 2 == 0 else 8.0, y, 1.4, 1.3, 0.3)
    b.rrect(6.5, 8.0, 3.0, 3.4, 0.7)

@mark("config")
def _(b):
    for y, k in ((4.6, 10.4), (8.0, 5.6), (11.4, 9.0)):
        b.line((3.4, y), (12.6, y), w=1.3)
        b.circle(k, y, 1.75)


@mark("lock")
def _(b):
    b.arc(8, 7.2, 2.6, 2.9, 180, 360, 1.6)
    b.line((5.4, 7.2), (5.4, 8.0), w=1.6); b.line((10.6, 7.2), (10.6, 8.0), w=1.6)
    b.rrect(3.8, 7.6, 8.4, 5.6, 1.2)
    b.circle(8, 10.0, 0.95, hole=True)
    b.rrect(7.5, 10.2, 1.0, 1.8, 0.3, hole=True)


@mark("license")
def _(b):
    b.poly((5.4, 8.6), (4.2, 13.4), (6.0, 12.4), (7.2, 13.6), (8.0, 9.6))
    b.poly((10.6, 8.6), (11.8, 13.4), (10.0, 12.4), (8.8, 13.6), (8.0, 9.6))
    b.circle(8, 6.6, 4.0)
    b.circle(8, 6.6, 2.5, hole=True)
    b.circle(8, 6.6, 1.5)


@mark("readme")
def _(b):
    b.circle(8, 4.3, 1.25)
    b.line((6.4, 7.0), (8, 7.0), (8, 11.8), w=1.8)
    b.line((6.2, 11.9), (9.8, 11.9), w=1.8)


@mark("build")
def _(b):
    # Hammer: a head and a handle, at 45 degrees.
    b.push(rot=45)
    b.rrect(4.2, 3.0, 7.6, 3.2, 0.8)
    b.line((8, 6.2), (8, 13.4), w=1.9)
    b.pop()


@mark("docker")
def _(b):
    # A shipping container: frame and ribs.
    b.line((2.8, 4.4), (13.2, 4.4), (13.2, 11.6), (2.8, 11.6), closed=True, w=1.5)
    for x in (6.2, 8.0, 9.8):
        b.line((x, 6.8), (x, 9.2), w=1.2)
    b.line((4.4, 6.8), (4.4, 9.2), w=1.2); b.line((11.6, 6.8), (11.6, 9.2), w=1.2)

@mark("binary")
def _(b):
    letter(b, "0", 2.9, 4.2, 4.2, 7.6, 1.5)
    letter(b, "1", 9.0, 4.2, 4.2, 7.6, 1.5)


@mark("html")
def _(b):
    chevrons(b, 1.6, slash=True)


@mark("xml")
def _(b):
    chevrons(b, 1.6)
    b.circle(8, 8, 1.2)


@mark("css")
def _(b):
    letter(b, "#", 3.8, 3.6, 8.4, 8.8, 1.7)


@mark("go")
def _(b):
    word(b, "GO", 2.8, 4.2, 4.6, 7.6, 1.2, 1.6)


@mark("java")
def _(b):
    b.line((3.6, 7.0), (3.6, 10.6), (5.2, 12.2), (8.4, 12.2), (10.0, 10.6), (10.0, 7.0), closed=True, w=1.5)
    b.arc(10.4, 8.8, 1.9, 1.5, -90, 90, 1.3)
    b.curve([(5.4 + 0.6 * math.sin(i / 6 * math.pi * 2), 2.4 + 3.0 * i / 12) for i in range(13)], 1.1)
    b.curve([(8.2 + 0.6 * math.sin(i / 6 * math.pi * 2), 2.4 + 3.0 * i / 12) for i in range(13)], 1.1)


@mark("lua")
def _(b):
    b.circle(7.4, 8.6, 4.4)
    b.circle(9.0, 7.0, 1.4, hole=True)
    b.circle(12.6, 3.4, 1.3)


@mark("shader")
def _(b):
    # Four-point sparkle.
    pts = []
    for i in range(8):
        a = math.radians(-90 + 45 * i)
        r = 5.4 if i % 2 == 0 else 1.5
        pts.append((8 + r * math.cos(a), 8 + r * math.sin(a)))
    b.poly(*pts)
    b.circle(12.4, 3.6, 1.0)


@mark("svg")
def _(b):
    pts = []
    for i in range(25):
        t = i / 24
        p0, p1, p2, p3 = (3.4, 11.6), (4.4, 3.0), (11.6, 13.0), (12.6, 4.4)
        x = (1 - t) ** 3 * p0[0] + 3 * (1 - t) ** 2 * t * p1[0] + 3 * (1 - t) * t * t * p2[0] + t ** 3 * p3[0]
        y = (1 - t) ** 3 * p0[1] + 3 * (1 - t) ** 2 * t * p1[1] + 3 * (1 - t) * t * t * p2[1] + t ** 3 * p3[1]
        pts.append((x, y))
    b.curve(pts, 1.4)
    b.rrect(2.0, 10.2, 2.8, 2.8, 0.5)
    b.rrect(11.2, 3.0, 2.8, 2.8, 0.5)


@mark("table")
def _(b):
    b.line((3.4, 4.0), (12.6, 4.0), (12.6, 12.0), (3.4, 12.0), closed=True, w=1.4)
    b.line((3.4, 7.2), (12.6, 7.2), w=1.4)
    b.line((7.0, 4.0), (7.0, 12.0), w=1.4)


@mark("database")
def _(b):
    b.ellipse_ring(8, 4.6, 4.2, 1.6, 1.4)
    b.arc(8, 8.2, 4.2, 1.6, 0, 180, 1.4)
    b.arc(8, 11.4, 4.2, 1.6, 0, 180, 1.4)
    b.line((3.8, 4.6), (3.8, 11.4), w=1.4); b.line((12.2, 4.6), (12.2, 11.4), w=1.4)


@mark("diff")
def _(b):
    b.line((4.8, 5.6), (11.2, 5.6), w=1.8); b.line((8, 2.6), (8, 8.6), w=1.8)
    b.line((4.8, 11.6), (11.2, 11.6), w=1.8)


@mark("log")
def _(b):
    for y in (4.6, 8.0, 11.4):
        b.circle(4.0, y, 1.0)
        b.line((6.6, y), (12.4 if y < 11 else 10.0, y), w=1.4)


@mark("font")
def _(b):
    letter(b, "A", 4.2, 3.8, 7.6, 8.4, 1.8)


@mark("audio")
def _(b):
    b.line((7.0, 11.0), (7.0, 3.6), (12.0, 2.8), (12.0, 9.8), w=1.5)
    b.circle(5.4, 11.4, 2.0)
    b.circle(10.4, 10.2, 2.0)


@mark("video")
def _(b):
    b.poly((5.4, 3.6), (12.6, 8.0), (5.4, 12.4))


@mark("haskell")
def _(b):
    b.line((4.4, 3.6), (5.6, 3.6), (11.8, 12.4), w=1.8)
    b.line((8.6, 8.2), (4.2, 12.4), w=1.8)


@mark("zig")
def _(b):
    letter(b, "Z", 4.4, 3.8, 7.2, 8.4, 1.9)


@mark("kotlin")
def _(b):
    letter(b, "K", 4.8, 3.8, 6.4, 8.4, 1.9)


@mark("php")
def _(b):
    letter(b, "P", 5.0, 3.8, 6.2, 8.4, 1.9)


@mark("ruby")
def _(b):
    b.line((5.2, 4.0), (10.8, 4.0), (13.0, 6.8), (8.0, 12.6), (3.0, 6.8), closed=True, w=1.4)
    b.line((3.0, 6.8), (13.0, 6.8), w=1.2)
    b.line((6.4, 6.8), (8.0, 12.6), (9.6, 6.8), w=1.2)


@mark("nix")
def _(b):
    for r in (0, 60, 120):
        b.push(rot=r)
        b.line((8, 2.8), (8, 13.2), w=1.6)
        b.pop()


@mark("vim")
def _(b):
    letter(b, "V", 4.0, 3.8, 8.0, 8.4, 1.9)


@mark("key")
def _(b):
    b.ring(5.4, 8.0, 2.4, 1.6)
    b.line((7.8, 8.0), (13.0, 8.0), w=1.6)
    b.line((11.4, 8.0), (11.4, 10.4), w=1.5); b.line((13.0, 8.0), (13.0, 9.8), w=1.5)


@mark("clock")
def _(b):
    b.ring(8, 8, 4.6, 1.5)
    b.line((8, 5.2), (8, 8), (10.2, 9.4), w=1.5)


@mark("test")
def _(b):
    # Flask.
    b.line((6.4, 3.2), (9.6, 3.2), w=1.4)
    b.line((6.8, 3.2), (6.8, 6.6), (3.6, 11.6), (4.4, 12.8), (11.6, 12.8), (12.4, 11.6), (9.2, 6.6), (9.2, 3.2), w=1.4)
    b.poly((4.8, 10.0), (11.2, 10.0), (12.0, 11.4), (11.4, 12.2), (4.6, 12.2), (4.0, 11.4))


@mark("code")
def _(b):
    chevrons(b, 1.8, slash=True)


@mark("book")
def _(b):
    b.line((8, 4.6), (8, 12.2), w=1.4)
    b.line((8, 4.6), (6.0, 3.6), (2.8, 3.6), (2.8, 11.2), (6.0, 11.2), (8, 12.2), w=1.4)
    b.line((8, 4.6), (10.0, 3.6), (13.2, 3.6), (13.2, 11.2), (10.0, 11.2), (8, 12.2), w=1.4)


@mark("package")
def _(b):
    b.line((8, 2.8), (12.8, 5.2), (12.8, 10.8), (8, 13.2), (3.2, 10.8), (3.2, 5.2), closed=True, w=1.4)
    b.line((3.2, 5.2), (8, 7.6), (12.8, 5.2), w=1.4)
    b.line((8, 7.6), (8, 13.2), w=1.4)


@mark("gear")
def _(b):
    b.ring(8, 8, 3.2, 2.0)
    for i in range(6):
        b.push(rot=i * 60)
        b.rrect(6.8, 2.2, 2.4, 2.6, 0.5)
        b.pop()


# ---------------------------------------------------------------- folders
def folder_closed_shape(b):
    b.poly(*rounded_poly([(1.4, 3.0), (6.2, 3.0), (7.8, 4.6), (14.6, 4.6), (14.6, 13.0), (1.4, 13.0)], 1.0))


def folder_open_shape(b):
    # Back panel and the front flap tilted forward.
    b.poly(*rounded_poly([(1.4, 3.0), (6.2, 3.0), (7.8, 4.6), (13.2, 4.6), (13.2, 6.6), (1.4, 6.6)], 1.0))
    b.poly(*rounded_poly([(1.4, 6.0), (15.2, 6.6), (13.2, 13.0), (1.4, 13.0)], 0.9))


def rounded_poly(pts, r):
    """Polygon with every corner rounded by radius r (quarter-ish arcs, sampled)."""
    out = []
    n = len(pts)
    for i in range(n):
        p0, p1, p2 = pts[i - 1], pts[i], pts[(i + 1) % n]
        v1 = (p0[0] - p1[0], p0[1] - p1[1]); l1 = math.hypot(*v1)
        v2 = (p2[0] - p1[0], p2[1] - p1[1]); l2 = math.hypot(*v2)
        d = min(r, l1 / 2, l2 / 2)
        a = (p1[0] + v1[0] / l1 * d, p1[1] + v1[1] / l1 * d)
        c = (p1[0] + v2[0] / l2 * d, p1[1] + v2[1] / l2 * d)
        for k in range(7):  # quadratic bezier a -> p1 -> c
            t = k / 6
            out.append(((1 - t) ** 2 * a[0] + 2 * (1 - t) * t * p1[0] + t * t * c[0],
                        (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * p1[1] + t * t * c[1]))
    return out


def folder_closed_line(b):
    b.line((1.6, 12.2), (1.6, 3.8), (2.4, 3.0), (6.0, 3.0), (7.6, 4.6), (13.6, 4.6), (14.4, 5.4),
           (14.4, 12.2), (13.6, 13.0), (2.4, 13.0), closed=True, w=1.3)


def folder_open_line(b):
    b.line((1.6, 12.2), (1.6, 3.8), (2.4, 3.0), (6.0, 3.0), (7.6, 4.6), (12.4, 4.6), (13.2, 5.4), (13.2, 6.6), w=1.3)
    b.line((1.6, 12.2), (2.4, 13.0), (12.6, 13.0), (13.2, 12.4), (15.0, 7.6), (14.4, 6.6), (4.4, 6.6), (3.6, 7.4),
           (1.6, 12.2), w=1.3)


# Badges: a mark scaled into the folder body (stroke widths stay legible).
BADGE_SCALE = 0.68
BADGE_DX, BADGE_DY = 0.0, 0.85
OPEN_BADGE_SCALE = 0.58
OPEN_BADGE_DX, OPEN_BADGE_DY = 0.5, 1.9
BADGE_SW = 1.35

FOLDERS = ["code", "test", "book", "git", "build", "image", "shell", "config", "package", "binary",
           "gear", "font", "shader", "docker", "h", "archive"]


# ---------------------------------------------------------------- font
def glyph_list():
    """(name, draw function) in codepoint order; the order is the ABI of file_icon_glyphs.h."""
    out = [("folder_fill", folder_closed_shape), ("folder_line", folder_closed_line),
           ("folder_open_fill", folder_open_shape), ("folder_open_line", folder_open_line)]
    for name, fn in MARKS.items():
        out.append((name, fn))
    for name in FOLDERS:
        def badge(b, fn=MARKS[name]):
            b.push(BADGE_SCALE, BADGE_DX, BADGE_DY)
            b.wmap = lambda w: BADGE_SW * (w / SW) ** 0.5
            fn(b)
            b.pop()
        out.append(("badge_" + name, badge))
    for name in FOLDERS:
        def badge(b, fn=MARKS[name]):
            b.push(OPEN_BADGE_SCALE, OPEN_BADGE_DX, OPEN_BADGE_DY)
            b.wmap = lambda w: BADGE_SW * (w / SW) ** 0.5
            fn(b)
            b.pop()
        out.append(("badge_open_" + name, badge))
    return out


def area(pts):
    s = 0.0
    for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
        s += x0 * y1 - x1 * y0
    return s / 2


def build_font(glyphs, path):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.ttLib.tables._g_l_y_f import flagOverlapSimple

    order = [".notdef"] + [n for n, _ in glyphs]
    cmap, glyf, metrics, codes = {}, {}, {}, {}
    pen = TTGlyphPen(None)
    pen.moveTo((0, 0)); pen.lineTo((0, UPM)); pen.lineTo((UPM, UPM)); pen.lineTo((UPM, 0)); pen.closePath()
    glyf[".notdef"] = pen.glyph()
    metrics[".notdef"] = (UPM, 0)
    for i, (name, fn) in enumerate(glyphs):
        cp = 0xE000 + i
        codes[name] = cp
        cmap[cp] = name
        b = Builder()
        fn(b)
        pen = TTGlyphPen(None)
        xmin = UPM
        for pts, hole in b.contours:
            fpts = [(round(x * UNIT), round((GRID - y) * UNIT)) for x, y in pts]
            # Font space is y-up: filled contours clockwise (negative area), holes counter-clockwise.
            if (area(fpts) > 0) != hole:
                fpts.reverse()
            pen.moveTo(fpts[0])
            for p in fpts[1:]:
                pen.lineTo(p)
            pen.closePath()
            xmin = min(xmin, min(p[0] for p in fpts))
        g = pen.glyph()
        if g.numberOfContours > 0:
            g.flags[0] |= flagOverlapSimple  # stroked marks overlap; tell FreeType to supersample
        glyf[name] = g
        metrics[name] = (UPM, xmin if b.contours else 0)

    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyf)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=UPM, descent=0)
    fb.setupNameTable({"familyName": "Overheat Icons", "styleName": "Regular",
                       "copyright": "Overheat project, MIT licensed (see LICENSE)",
                       "uniqueFontIdentifier": "Overheat Icons Regular", "fullName": "Overheat Icons",
                       "psName": "OverheatIcons-Regular", "version": "Version 1.0"})
    fb.setupOS2(sTypoAscender=UPM, sTypoDescender=0, sTypoLineGap=0, usWinAscent=UPM, usWinDescent=0)
    fb.setupPost()
    fb.save(path)
    return codes


def write_header(codes, path):
    lines = ["// Generated by assets/icons/build_icons.py - do not edit.",
             "#pragma once", "#include <cstdint>", "", "namespace oh::icon_glyph {", ""]
    for name, cp in codes.items():
        lines.append(f"inline constexpr uint32_t {name} = 0x{cp:04X};")
    lines += ["", "} // namespace oh::icon_glyph", ""]
    open(path, "w").write("\n".join(lines))


def write_preview(ttf, codes, path):
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("Pillow not installed: skipping preview.png")
        return
    marks = [n for n in MARKS]
    folders = ["folder"] + FOLDERS
    cols = 12
    cell = 44
    size = 28
    rows_m = (len(marks) + cols - 1) // cols
    rows_f = (len(folders) + cols - 1) // cols
    themes = [("dark", (22, 23, 26), 0.20), ("light", (255, 255, 255), 0.14)]
    H = (rows_m + rows_f) * cell + 30
    img = Image.new("RGB", (cols * cell * 2 + 40, H), (128, 128, 128))
    font = ImageFont.truetype(ttf, size)
    pal = preview_palette()
    for ti, (tname, bg, tint) in enumerate(themes):
        ox = ti * (cols * cell + 40)
        layer = Image.new("RGBA", (cols * cell + 20, H), bg + (255,))
        d = ImageDraw.Draw(layer)
        hues = pal[tname]
        for i, n in enumerate(marks):
            x, y = 10 + (i % cols) * cell, 10 + (i // cols) * cell
            col = hues[i % len(hues)]
            tile = tuple(round(bg[k] + (col[k] - bg[k]) * tint) for k in range(3))
            d.rounded_rectangle((x, y, x + size - 1, y + size - 1), radius=round(size * 0.22), fill=tile)
            d.text((x, y + size), chr(codes[n]), font=font, fill=col, anchor="ls")
        for i, n in enumerate(folders):
            x, y = 10 + (i % cols) * cell, 20 + (rows_m + i // cols) * cell
            col = hues[(i * 3) % len(hues)]
            fill = tuple(round(bg[k] + (col[k] - bg[k]) * (tint + 0.12)) for k in range(3))
            opened = i % 2 == 1
            d.text((x, y + size), chr(codes["folder_open_fill" if opened else "folder_fill"]), font=font, fill=fill, anchor="ls")
            d.text((x, y + size), chr(codes["folder_open_line" if opened else "folder_line"]), font=font, fill=col, anchor="ls")
            if n != "folder":
                d.text((x, y + size), chr(codes[("badge_open_" if opened else "badge_") + n]), font=font, fill=col, anchor="ls")
        img.paste(layer, (ox, 0))
    img.save(path)


def preview_palette():
    # Mirrors kHues in src/ui/file_icons.cpp (dark, light).
    dark = [0x5b9cff, 0x8c84ff, 0xc38cf5, 0xf07ab4, 0xf26d6d, 0xf2924c, 0xe8c25a, 0x7ccf6c, 0x4fc7b0, 0x56c3e6, 0x9aa3b2, 0xc49a6c]
    light = [0x1f63d6, 0x5a4fd8, 0x8a3fc4, 0xc4357d, 0xc93636, 0xc4621c, 0x9a7a00, 0x2f8a2a, 0x13836f, 0x137fa6, 0x5f6878, 0x8a6236]
    rgb = lambda c: ((c >> 16) & 255, (c >> 8) & 255, c & 255)
    return {"dark": [rgb(c) for c in dark], "light": [rgb(c) for c in light]}


def main():
    glyphs = glyph_list()
    ttf = os.path.join(HERE, "OverheatIcons.ttf")
    codes = build_font(glyphs, ttf)
    write_header(codes, os.path.join(ROOT, "src", "ui", "file_icon_glyphs.h"))
    write_preview(ttf, codes, os.path.join(HERE, "preview.png"))
    print(f"{len(glyphs)} glyphs -> {ttf}")


if __name__ == "__main__":
    sys.exit(main())

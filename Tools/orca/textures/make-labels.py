#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Orca's Online menu labels: our own art, drawn in each game's style with open-licence fonts.

Writes the texture pack Orca loads in every session (Data/Sys/Orca/Textures/RSBE01, see ORCA.md
"Online menu" and "The character select's title"): Dolphin's hires texture names, so each PNG replaces one of the game's textures on
screen and nothing else (texture replacement never touches emulated memory). Brawl and Project+
both load textures as RSBE01; their labels hash differently, so one folder serves both.

  python3 Tools/orca/textures/make-labels.py [--out <dir>] [--sheet <preview.png>]

Needs Pillow. The fonts are next to this script, with their licences (SIL Open Font License 1.1):
Archivo Black for Brawl's heavy, wide labels, Anton for Project+'s condensed ones and for Brawl's
title bar (the game's own titles there are a light condensed sans: SELECT MODE, / WITH ANYONE).
Nothing here is derived from the game's art: the sizes and boxes below are numbers measured on the
game's own textures (where its letters sit), so ours sit in the same place.

Each label is drawn at 4x the game's size (Dolphin takes any integer multiple), with mip levels
down to the game's own size (<name>_mip1.png at 2x, _mip2.png at 1x): a custom texture's sampler
takes its mip levels even where the game's texture has none (TextureCacheBase::GetSamplerState
lifts the LOD clamp for custom textures), so a label drawn at 1x internal resolution is a clean 1x
image instead of the 4x one read every fourth texel, which breaks thin strokes (Anton's N).
"""

import argparse
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
FONTS = os.path.join(HERE, "fonts")
ARCHIVO = os.path.join(FONTS, "ArchivoBlack-Regular.ttf")
ANTON = os.path.join(FONTS, "Anton-Regular.ttf")
SCALE = 4

# One entry per replaced texture:
#   name: Dolphin's name for the game's texture (tex1_<w>x<h>_<XXH64 of its data>_<GX format>)
#   lines: the text, one string per line
#   box: where the game's own letters sit in its texture (x0, y0, x1, y1), native pixels
#   style: brawl (black, white rim and soft halo), pplus (black, white outline), title (white),
#          brawl-css and pplus-css (the character select's mode title: a gradient fill, black outline)
LABELS = [
    # Brawl's main menu: the Wi-Fi button's label (MenMainFrTop15, "NINTENDO Wi-Fi CONNECTION").
    dict(name="tex1_127x96_fdd5334e5af7357c_2", lines=["Play", "Online"], box=(7, 12, 119, 84),
         style="brawl"),
    # Brawl's page title and breadcrumb on the Online pages (MenMainTtlJ.03, "NINTENDO WFC"): its
    # letters span the slot at the height of every title's (rows 1-14); ours take that height.
    dict(name="tex1_120x16_a56825159146a723_0", lines=["ONLINE"], box=(3, 1, 117, 14),
         style="title"),
    # Brawl's With Anyone: Basic Brawl (MenMainWifi22) and Team Battle (MenMainWifi23).
    dict(name="tex1_144x88_115d82941c7412cd_2", lines=["Casual"], box=(7, 22, 138, 66),
         style="brawl"),
    # (The game's letters reach x 151; ours stop at 140, so RANKED's rim clears the handshake icon
    # as CASUAL's clears its own.)
    dict(name="tex1_160x80_e364b4db513567ba_2", lines=["Ranked"], box=(7, 18, 140, 62),
         style="brawl"),
    # Project+'s With Anyone: Basic Versus (MenMainWifi22) and Team Battle (MenMainWifi23).
    dict(name="tex1_144x88_ecd09d11be34045f_5", lines=["CASUAL"], box=(11, 16, 133, 71),
         style="pplus"),
    dict(name="tex1_160x80_5b8f52ac0127b2a8_5", lines=["RANKED"], box=(24, 14, 136, 66),
         style="pplus"),
    # The character select's mode title (MenSelchrPanelRule3.1: Brawl's BRAWL logo, Project+'s
    # VERSUS), as the game draws it with Orca's mark for a mode in its palette (UX/CssTitle.h: the
    # transparent entry the texture's background uses, a value nobody sees, so each mode hashes to
    # its own name). Brawl's in the gold of its other rule titles (Special Brawl, Winner Out), small
    # capitals like theirs; Project+'s in its mint like VERSUS, where VERSUS's letters sit.
    dict(name="tex1_132x48_44a1dd712ec999f0_159b390ed5eec04d_9", lines=["Casual"],
         box=(8, 12, 124, 36), style="brawl-css"),
    dict(name="tex1_132x48_44a1dd712ec999f0_4d236969243ec2d8_9", lines=["Ranked"],
         box=(8, 12, 124, 36), style="brawl-css"),
    dict(name="tex1_132x48_44a1dd712ec999f0_952be74b413d03a4_9", lines=["Friends"],
         box=(8, 12, 124, 36), style="brawl-css"),
    dict(name="tex1_128x56_57564f7b1ba3bc1c_96a987e9e0ec657e_9", lines=["CASUAL"],
         box=(16, 17, 113, 39), style="pplus-css"),
    dict(name="tex1_128x56_57564f7b1ba3bc1c_e2dffeab0d95d424_9", lines=["RANKED"],
         box=(16, 17, 113, 39), style="pplus-css"),
    dict(name="tex1_128x56_57564f7b1ba3bc1c_faaf2c3553d21273_9", lines=["FRIENDS"],
         box=(13, 17, 116, 39), style="pplus-css"),
]

# The character select titles' fills, top to bottom (position 0-1 down the letters, RGB): measured
# on the game's own rule titles, a gold with a white band through the middle (Brawl's) and a teal
# to white to green (Project+'s VERSUS).
GRADIENTS = {
    "brawl-css": [(0.0, (221, 204, 102)), (0.3, (221, 204, 110)), (0.42, (238, 221, 170)),
                  (0.5, (255, 246, 220)), (0.56, (255, 255, 255)), (0.62, (255, 246, 220)),
                  (0.7, (238, 221, 170)), (0.8, (221, 204, 112)), (1.0, (221, 204, 102))],
    "pplus-css": [(0.0, (65, 222, 189)), (0.25, (148, 230, 205)), (0.45, (222, 246, 230)),
                  (0.55, (222, 246, 230)), (0.75, (139, 230, 172)), (1.0, (74, 213, 123))],
}


def size_of(name):
    w, h = name.split("_")[1].split("x")
    return int(w), int(h)


def gradient(style, top, bottom, w, h):
    """An RGBA image of the style's fill, its stops spread from row `top` to row `bottom`."""
    stops = GRADIENTS[style]
    out = Image.new("RGBA", (w, h))
    px = out.load()
    for y in range(h):
        t = min(1.0, max(0.0, (y - top) / max(1, bottom - top)))
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                k = (t - t0) / (t1 - t0) if t1 > t0 else 0
                c = tuple(round(a + (b - a) * k) for a, b in zip(c0, c1))
                break
        for x in range(w):
            px[x, y] = c + (255,)
    return out


def small_caps_runs(word, font_path, size):
    """Brawl's label lettering: each word's first letter full size, the rest small capitals."""
    runs = []
    for i, word_part in enumerate(word.split(" ")):
        if i:
            runs.append((" ", ImageFont.truetype(font_path, int(size * 0.62))))
        runs.append((word_part[0].upper(), ImageFont.truetype(font_path, size)))
        if len(word_part) > 1:
            runs.append((word_part[1:].upper(), ImageFont.truetype(font_path, int(size * 0.74))))
    return runs


def line_mask(runs, tracking):
    """One line of text (runs of (text, font) sharing a baseline) as a tight L mask."""
    ascent = max(f.getmetrics()[0] for _, f in runs)
    descent = max(f.getmetrics()[1] for _, f in runs)
    width = 0
    for text, f in runs:
        width += sum(f.getlength(c) + tracking for c in text)
    mask = Image.new("L", (int(width) + 8, ascent + descent + 8), 0)
    d = ImageDraw.Draw(mask)
    x = 4.0
    for text, f in runs:
        a = f.getmetrics()[0]
        for c in text:
            d.text((x, 4 + ascent - a), c, font=f, fill=255)
            x += f.getlength(c) + tracking
    return mask.crop(mask.getbbox())


def text_mask(label, w, h):
    """The label's letters as an L mask of the full texture (4x), fitted into its box."""
    W, H = w * SCALE, h * SCALE
    x0, y0, x1, y1 = (v * SCALE for v in label["box"])
    bw, bh = x1 - x0, y1 - y0
    style = label["style"]
    big = 400
    lines = []
    for text in label["lines"]:
        if style in ("brawl", "brawl-css"):
            lines.append(line_mask(small_caps_runs(text, ARCHIVO, big), big * 0.01))
        elif style in ("pplus", "pplus-css"):
            lines.append(line_mask([(text, ImageFont.truetype(ANTON, big))], big * 0.02))
        else:
            # Anton as it comes (condensed, like the game's titles), with hardly any tracking.
            lines.append(line_mask([(text, ImageFont.truetype(ANTON, big))], big * 0.03))
    gap = int(big * 0.14)
    tw = max(m.width for m in lines)
    th = sum(m.height for m in lines) + gap * (len(lines) - 1)
    block = Image.new("L", (tw, th), 0)
    y = 0
    for m in lines:
        block.paste(m, ((tw - m.width) // 2, y))
        y += m.height + gap
    # The title bar's letters fill its height (centred, as the game's own short titles are); a label
    # keeps its aspect inside its box.
    k = min(bw / tw, bh / th)
    if style == "title":
        k = bh / th
    kx = k
    if style == "pplus-css":
        # Project+'s title letters are wider than Anton's: its full height, widened to the box.
        k = bh / th
        kx = min(bw / tw, k * 1.6)
    block = block.resize((max(1, int(tw * kx)), max(1, int(th * k))), Image.LANCZOS)
    out = Image.new("L", (W, H), 0)
    out.paste(block, (x0 + (bw - block.width) // 2, y0 + (bh - block.height) // 2))
    return out


def grow(mask, px):
    """The mask dilated by about `px` pixels, with round corners and a soft edge: a blur whose
    sigma is px / 2 reaches 2.3% of full (6/255) two sigmas out."""
    if px <= 0:
        return mask
    return mask.filter(ImageFilter.GaussianBlur(px / 2)).point(
        lambda v: 255 if v >= 6 else v * 42)


def draw(label):
    w, h = size_of(label["name"])
    W, H = w * SCALE, h * SCALE
    mask = text_mask(label, w, h)
    style = label["style"]
    if style == "title":
        # An I4 texture: the game draws intensity as both colour and alpha.
        return Image.merge("RGBA", (mask, mask, mask, mask))
    white = Image.new("RGBA", (W, H), (255, 255, 255, 255))
    black = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    out = Image.new("RGBA", (W, H), (255, 255, 255, 0))
    if style == "brawl":
        rim = grow(mask, 2.2 * SCALE)
        halo = grow(rim, 2.5 * SCALE).filter(ImageFilter.GaussianBlur(1.8 * SCALE))
        out = Image.composite(white, out, halo.point(lambda v: int(v * 0.6)))
        out = Image.composite(white, out, rim)
    elif style in GRADIENTS:
        # The letters in the style's fill, its stops down the letters' own height, inside a black
        # outline about two and a half of the game's pixels wide, as the game's titles have.
        box = mask.point(lambda v: 255 if v >= 128 else 0).getbbox()
        fill = gradient(style, box[1], box[3], W, H)
        outline = grow(mask, 2.4 * SCALE)
        out = Image.composite(black, out, outline)
        return Image.composite(fill, out, mask)
    else:
        rim = grow(mask, 2.6 * SCALE)
        edge = grow(rim, 0.8 * SCALE)
        out = Image.composite(Image.new("RGBA", (W, H), (60, 60, 60, 255)), out,
                              ImageChops.subtract(edge, rim).point(lambda v: int(v * 0.5)))
        out = Image.composite(white, out, rim)
    return Image.composite(black, out, mask)


def mips(label, im):
    """The label's mip levels, each half the last (box filter), down to the game's size. Pillow
    averages RGBA premultiplied; the title (I4: intensity is colour and alpha) halves its mask."""
    assert SCALE & (SCALE - 1) == 0, "mip levels halve down to the game's size: SCALE is 2^n"
    w, h = size_of(label["name"])
    out = []
    while im.width > w:
        size = (im.width // 2, im.height // 2)
        if label["style"] == "title":
            a = im.getchannel("A").resize(size, Image.BOX)
            im = Image.merge("RGBA", (a, a, a, a))
        else:
            im = im.resize(size, Image.BOX)
        out.append(im)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--out", default=os.path.join(REPO, "Data", "Sys", "Orca", "Textures", "RSBE01"))
    p.add_argument("--sheet", help="also write a preview of every label on the menus' colours")
    a = p.parse_args()
    os.makedirs(a.out, exist_ok=True)
    images = []
    for label in LABELS:
        im = draw(label)
        path = os.path.join(a.out, label["name"] + ".png")
        im.save(path, optimize=True)
        images.append((label, im))
        print(path, im.size)
        for level, mip in enumerate(mips(label, im), 1):
            path = os.path.join(a.out, f"{label['name']}_mip{level}.png")
            mip.save(path, optimize=True)
            print(path, mip.size)
    if a.sheet:
        cw = max(im.width for _, im in images) + 20
        sheet = Image.new("RGBA", (cw * 2, sum(im.height + 20 for _, im in images)), (0, 0, 0, 255))
        y = 0
        for label, im in images:
            for i, bg in enumerate([(236, 168, 196, 255), (40, 30, 70, 255)]):
                tile = Image.new("RGBA", (im.width, im.height), bg)
                tile.alpha_composite(im)
                sheet.paste(tile, (cw * i + 10, y + 10))
            y += im.height + 20
        sheet.save(a.sheet)
    return 0


if __name__ == "__main__":
    sys.exit(main())

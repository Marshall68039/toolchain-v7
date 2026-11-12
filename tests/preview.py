#!/usr/bin/env python3
"""Draw roughly what the calculator viewer shows for a page.

    ./test_core PAGE.bin > layout.txt
    python3 preview.py layout.txt SPRITE_DIR out.png [first_line [selected_link]]

SPRITE_DIR holds the NAME.bin sprite files (convert with html2ce.py --bin).
selected_link is a link number (as listed by test_core): it is drawn highlighted with the
arrow beside it, the way main.c does it.
The font is only an approximation of the calculator's; positions, sizes, colours
and image placement follow main.c.
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont



def build_palette():  # same fixed palette as html2ce.py and main.c
    pal = [(r * 51, g * 51, b * 51) for r in range(6) for g in range(6) for b in range(6)]
    return pal + [(((k + 1) * 255) // 40,) * 3 for k in range(40)]


PAL = build_palette()
C_BLACK, C_WHITE, C_LINK, C_ITAL, C_DEAD = 0, 255, 4, 108, 235
C_ARROW = C_ITAL
BAR_H, VIEW_TOP = 12, 14
VIEW_BOT = 240 - BAR_H
VIEW_W, MARGIN = 312, 4
SCALE = {0: (1, 1), 1: (2, 2), 2: (1, 2), 3: (1, 1)}

FONT = None
for cand in ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
             "C:/Windows/Fonts/consolab.ttf"):
    try:
        FONT = ImageFont.truetype(cand, 10)
        break
    except OSError:
        pass
FONT = FONT or ImageFont.load_default()


def glyph(ch, color, sx, sy, bold):
    cell = Image.new("L", (8, 8), 0)
    ImageDraw.Draw(cell).text((0, -1), ch, font=FONT, fill=255)
    if sx != 1 or sy != 1:
        cell = cell.resize((8 * sx, 8 * sy), Image.NEAREST)
    return cell


def parse(path):
    doc, lines, links = {}, [], {}
    target = None  # where S/I lines attach: the line, or the current cell of a row

    def blank(**kw):
        return {"segs": [], "img": None, "cells": [], "x": 0, "w": 0, **kw}

    for raw in Path(path).read_text().splitlines():
        if raw.startswith("DOC"):
            doc["head"] = raw
        elif raw.startswith("L "):
            f = dict(kv.split("=") for kv in raw.split()[2:])
            lines.append(blank(i=int(raw.split()[1]), **{k: int(v) for k, v in f.items()}))
            target = lines[-1]
        elif raw.startswith("C "):
            lines[-1]["cells"].append(blank(lines=[]))
            cell = lines[-1]["cells"][-1]
            target = cell
        elif raw.startswith("X "):
            _, x, w = raw.split()
            cell["x"], cell["w"] = int(x), int(w)
        elif raw.startswith("K "):
            _, kind, center, indent, height, yoff = raw.split()
            cell["lines"].append(blank(kind=int(kind), center=int(center), indent=int(indent),
                                       h=int(height), yoff=int(yoff)))
            target = cell["lines"][-1]
        elif raw.startswith("S "):
            _, st, ln, lid, *txt = raw.split(" ", 4)
            target["segs"].append((int(st), (txt[0] if txt else "")[:int(ln)].ljust(int(ln)),
                                   int(lid)))
        elif raw.startswith("I "):
            _, name, w, h = raw.split()
            target["img"] = (name, int(w), int(h))
        elif raw.startswith("LK "):
            f = raw.split()
            links[int(f[1])] = {"y": int(f[4][2:]), "h": int(f[5][2:])}
        elif raw.startswith("ERR"):
            print(raw)
    return doc, lines, links


def draw_arrow(img, x, gy, sy):
    """A small right-pointing triangle, 3 wide and 5 tall, in the gutter left of the line."""
    top = gy + (8 * sy - 5) // 2
    x = max(0, x - 4)
    for r, n in enumerate((1, 2, 3, 2, 1)):
        for k in range(n):
            img.putpixel((x + k, top + r), PAL[C_ARROW])


def draw_line(img, d, ln, y, x0, avail, sprites, sel, sel_y, abs_y):
    """Draw a text, rule or image line whose box starts at x0 and is `avail` px wide."""
    if ln["kind"] == 2:  # image
        name, w, h = ln["img"]
        data = (Path(sprites) / (name + ".bin")).read_bytes()
        sp = Image.new("RGB", (w, h))
        sp.putdata([PAL[b] for b in data[2:]])
        x = x0 + ln["indent"]
        if ln["center"] and w < avail - ln["indent"]:
            x += (avail - ln["indent"] - w) // 2
        img.paste(sp, (x, y + 2))
    elif y + ln["h"] <= VIEW_BOT:
        if ln["kind"] == 1:
            d.line((x0 + ln["indent"], y + 3, x0 + avail - 1, y + 3), fill=PAL[C_DEAD])
            return
        width = sum(len(t) * 8 * SCALE[(st >> 2) & 3][0] for st, t, _ in ln["segs"])
        x = x0 + ln["indent"]
        if ln["center"] and width < avail - ln["indent"]:
            x += (avail - ln["indent"] - width) // 2
        arrow_done = False
        for st, text, lid in ln["segs"]:
            head = (st >> 2) & 3
            sx, sy = SCALE[head]
            bold = bool(st & 1) or head != 0
            col = (C_LINK if st & 0x10 else C_DEAD if st & 0x20 else C_ITAL if st & 2 else C_BLACK)
            xs = x
            chosen = sel and lid == sel
            if chosen:
                col = C_WHITE
                d.rectangle((xs, y, xs + len(text) * 8 * sx - 1, y + ln["h"] - 1), fill=PAL[C_LINK])
                if not arrow_done and abs_y == sel_y:
                    draw_arrow(img, x0 + ln["indent"], y + ln["h"] - 1 - 8 * sy, sy)
                    arrow_done = True
            for ch in text:
                if ch != " ":
                    g = glyph(ch, col, sx, sy, bold)
                    gy = y + ln["h"] - 1 - 8 * sy
                    img.paste(PAL[col], (x, gy), g)
                    if bold:
                        img.paste(PAL[col], (x + 1, gy), g)
                x += 8 * sx
            if st & 0x30:
                d.line((xs, y + ln["h"] - 1, x - 1, y + ln["h"] - 1), fill=PAL[col])


def main():
    layout, sprites, out = sys.argv[1:4]
    top = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    sel = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    doc, lines, links = parse(layout)
    sel_y = links[sel]["y"] if sel in links else -1
    img = Image.new("RGB", (320, 240), PAL[C_WHITE])
    d = ImageDraw.Draw(img)
    y0 = lines[top]["y"] if lines else 0
    for ln in lines[top:]:
        y = VIEW_TOP + ln["y"] - y0
        if y >= VIEW_BOT:
            break
        if ln["kind"] == 3:  # side-by-side columns
            for cell in ln["cells"]:
                for sub in cell["lines"]:
                    if y + sub["yoff"] < VIEW_BOT:
                        draw_line(img, d, sub, y + sub["yoff"], MARGIN + cell["x"], cell["w"],
                                  sprites, sel, sel_y, ln["y"] + sub["yoff"])
        else:
            draw_line(img, d, ln, y, MARGIN, VIEW_W, sprites, sel, sel_y, ln["y"])
    for yy in (0, VIEW_BOT):
        d.rectangle((0, yy, 319, yy + BAR_H - 1), fill=PAL[C_BLACK])
    d.text((4, 1), doc.get("head", "").split('"')[1] if '"' in doc.get("head", "") else "", fill=(255, 255, 255), font=FONT)
    d.text((4, VIEW_BOT + 1), "line %d/%d" % (top + 1, len(lines)), fill=(255, 255, 255), font=FONT)
    img.resize((640, 480), Image.NEAREST).save(out)
    print("wrote", out)


main()

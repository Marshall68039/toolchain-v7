#!/usr/bin/env python3
"""Check a converted clip: decode it here with its own small decoder, compare every frame
with what the C code (test_video) unpacks, and draw the frames as a contact sheet.

    ./test_video CHUNK1.bin [CHUNK2.bin ...] > c.txt
    python3 check_video.py c.txt out.png CHUNK1.bin [CHUNK2.bin ...]
"""
import struct
import sys
from PIL import Image


def palette():
    pal = [(r * 51, g * 51, b * 51) for r in range(6) for g in range(6) for b in range(6)]
    return pal + [(((k + 1) * 255) // 40,) * 3 for k in range(40)]


def unrle(data, size):
    out, i = bytearray(), 0
    while i < len(data) and len(out) < size:
        c = data[i]; i += 1
        if c < 128:
            out += data[i:i + c + 1]; i += c + 1
        else:
            out += bytes([data[i]]) * (c - 126); i += 1
    return bytes(out[:size])


def main():
    ctxt, png, chunks = sys.argv[1], sys.argv[2], sys.argv[3:]
    frames, w, h = [], 0, 0
    for path in chunks:
        d = open(path, "rb").read()
        assert d[:2] == b"HV" and d[2] == 1, "not a video chunk"
        w, h, scale, fps = d[12], d[13], d[14], d[15]
        total, here = struct.unpack("<HH", d[16:20])
        pos = 20
        for _ in range(here):
            n = struct.unpack("<H", d[pos:pos + 2])[0]
            frames.append(unrle(d[pos + 2:pos + 2 + n], w * h)); pos += 2 + n
        assert pos == len(d), "%s: %d stray bytes" % (path, len(d) - pos)
    mine = []
    for f in frames:
        s = 0
        for b in f:
            s = (s * 31 + b) & 0xFFFFFFFF
        mine.append(s)
    theirs = [int(l.split()[2]) for l in open(ctxt) if l.startswith("F ")]
    bad = [i for i, (a, b) in enumerate(zip(mine, theirs)) if a != b]
    print("%d frames, %dx%d x%d at %d fps: C and Python agree on %d, differ on %d%s"
          % (len(frames), w, h, scale, fps, len(mine) - len(bad), len(bad),
             "" if len(mine) == len(theirs) else "  (frame counts differ!)"))
    pal = palette()
    pick = [frames[int(i * (len(frames) - 1) / 7)] for i in range(8)] if len(frames) > 8 else frames
    sheet = Image.new("RGB", (4 * (w + 4), ((len(pick) + 3) // 4) * (h + 4)), (90, 90, 90))
    for k, f in enumerate(pick):
        im = Image.new("RGB", (w, h)); im.putdata([pal[b] for b in f])
        sheet.paste(im, ((k % 4) * (w + 4), (k // 4) * (h + 4)))
    sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save(png)
    print("wrote", png)
    sys.exit(1 if bad or len(mine) != len(theirs) else 0)


main()

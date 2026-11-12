#!/usr/bin/env python3
"""Draw roughly what the calculator's video player shows for one frame of a clip.

    python3 preview_video.py FRAME_NUMBER out.png CHUNK1.bin [CHUNK2.bin ...]

Positions, scale and colours follow show_frame() in main.c; the font is an approximation.
"""
import struct
import sys
from PIL import Image, ImageDraw, ImageFont

BAR_H, VIEW_BOT = 12, 240 - 12


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
    want, out, chunks = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
    frames, name = [], ""
    for path in chunks:
        d = open(path, "rb").read()
        w, h, scale = d[12], d[13], d[14]
        pos = 20
        for _ in range(struct.unpack("<H", d[18:20])[0]):
            n = struct.unpack("<H", d[pos:pos + 2])[0]
            frames.append(unrle(d[pos + 2:pos + 2 + n], w * h)); pos += 2 + n
    n_frames, want = len(frames), min(want, len(frames) - 1)
    pal = palette()
    img = Image.new("RGB", (320, 240), (0, 0, 0))
    sprite = Image.new("RGB", (w, h)); sprite.putdata([pal[b] for b in frames[want]])
    sw, sh = w * scale, h * scale
    img.paste(sprite.resize((sw, sh), Image.NEAREST), ((320 - sw) // 2, BAR_H + (VIEW_BOT - BAR_H - sh) // 2))
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", 10)
    d.text((4, 1), "Video " + chunks[0].rsplit("/", 1)[-1][:-4], fill=(255, 255, 255), font=font)
    d.text((4, VIEW_BOT + 1), "%d/%d Enter:pause <>:seek Clear:exit" % (want + 1, n_frames), fill=(255, 255, 255), font=font)
    img.resize((640, 480), Image.NEAREST).save(out)
    print("wrote", out)


main()

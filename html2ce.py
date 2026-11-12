#!/usr/bin/env python3
"""
html2ce.py - convert HTML pages into TI-84 Plus CE AppVars for an on-calculator HTML reader.

Usage:
    python html2ce.py PAGE [PAGE ...] [-o OUTDIR] [-n NAME] [--dump] [--bin] [--page-size N]
                      [--no-images] [--image-max WxH] [--depth N] [--max-pages N] [--same-site]
                      [--list-all] [--videos ask|yes|no] [--video-size WxH] [--video-fps N]
                      [--video-seconds N] [--video-max-kb N] [--video-scale N]

PAGE is a local .html file or an http(s):// URL. Each page becomes NAME.8xv (an archived
AppVar you can send with TI Connect CE). Pages bigger than --page-size are split into
chained pages NAME, NAME02, NAME03, ... that point at each other.

Following links: --depth N (default 1) also converts the pages the given page links to
(depth 2 also the pages those link to, and so on; 0 = just the page itself). Links between
converted pages are rewritten to point at each other's AppVars; links to anything else are
kept as grey, non-selectable link text. --max-pages (default 40) stops a link-heavy page from
producing hundreds of files, and --same-site only follows links on the starting site.
You can also pass several pages at once; they are linked to each other the same way.

Videos: a <video> in a page (or a video file given as PAGE: .mp4 .webm .mov .mkv .avi
.gif) can be turned into a small silent clip the viewer plays. For every video the script
asks whether to convert it (--videos yes / no answers for all of them). Clips are low
quality on purpose: --video-size (default 80x60) drawn --video-scale times as big, at
--video-fps (default 5) for at most --video-seconds (default 15) and --video-max-kb
(default 400). Needs ffmpeg on the PATH (GIFs only need Pillow).

The viewer's menu lists only the pages you gave on the command line. The pages pulled in by
following links are still on the calculator (you reach them through the links) but stay out
of the menu, unless you add --list-all.

Side-by-side columns: a table row with 2 or 3 cells is shown as that many columns (widths
from the cells' width= attributes, else equal). Rows with 4 or more cells, and tables
nested inside a column, are flattened with ' | ' between cells.

Needs Python 3.7+. Pictures additionally need the Pillow package (pip install pillow);
without it, or with --no-images, each picture shows its alt text instead.

Pictures (<img>) are scaled to fit --image-max (default 240x160, never enlarged),
dithered to the calculator palette and stored as sprite AppVars named like
NAMEI01, NAMEI02, ... which the viewer loads while showing the page.

---------------------------------------------------------------------------------------
PAGE FORMAT (version 4) - this is what the calculator-side viewer reads
---------------------------------------------------------------------------------------
Header:
    2 bytes   magic "HR"
    1 byte    format version (5; the viewer also reads 3 and 4)
    1 byte    flags: bit 0 = list this page in the viewer's menu (version 3 pages have 0 here
              and are all listed)
    8 bytes   previous page AppVar name (NUL padded, all zero = none)
    8 bytes   next page AppVar name     (NUL padded, all zero = none)
    1 byte    title length (0-40)
    N bytes   title (ASCII)
Then a token stream. Any byte >= 0x20 is a plain ASCII text character. Bytes below 0x20
are commands:

    0x00           END of page
    0x01 n         HEADING style n (1 big, 2 medium, 3 small); lasts until the next 0x02
    0x02           PARAGRAPH break (blank line)
    0x03           LINE break
    0x04 v         CENTER on/off (v = 1 or 0)
    0x05 name[8]   LINK start; name = target AppVar, or 8 zero bytes if not on the calculator
    0x06           LINK end
    0x07 v         BOLD on/off
    0x08 v         ITALIC on/off
    0x09 name[8] w h len alt
                   IMAGE; name = sprite AppVar (8 zero bytes = no sprite, show the alt
                   text instead), w/h in pixels, then len bytes of alt text (0-40)
    0x0A n         INDENT level n (0-3)
    0x0B v         PREFORMATTED on/off (keep spaces, don't word-wrap)
    0x0C           HORIZONTAL RULE (occupies its own line)
    0x0D n w[n]    COLUMNS: start n (2 or 3) side-by-side columns; w = relative widths
    0x0E           next column
    0x0F           end of COLUMNS
    0x10 name[8] h16
                   LINK start, like 0x05, that points at an anchor inside the target page;
                   h16 = 2-byte little-endian anchor hash (see below)
    0x11 h16       ANCHOR: this spot can be jumped to by links carrying the same hash
    0x12 name[8] len alt
                   VIDEO: a clip (name = its first VIDEO APPVAR, 8 zero bytes = none); the
                   viewer shows a link "[> video: alt]" and plays the clip on Enter

Links: the viewer lets you step through the links that point at pages on the calculator
(grey links to pages that aren't there can't be selected). A link to "page#name" uses 0x10
and the 16-bit hash of "name" (see anchor_hash); the target spot is marked in its page by
0x11 with the same hash, from <a name=...> or any id=... attribute. If the anchor isn't
found (or is missing) the viewer shows the top of the target page. Links to a page that was
split into chained pages are followed along the chain by the viewer.

Each column starts in the plain state (bold, italic, centre, indent, heading, link all off).
Column geometry: avail = 312 - 8*(n-1); width_i = avail * w_i // sum(w); the last column
takes the remainder; columns start 8 pixels apart. The viewer builds rows from the columns:
each row takes the next line of every column and packs further lines into the shorter
columns, so the text beside a tall heading or picture stays tight.

On/off commands take an explicit value (not a toggle), and every page starts in the plain
state, so a page that was split from a longer one re-states whatever was active at the
split and can never get out of sync.

SPRITE APPVAR: 1 byte width, 1 byte height, then width*height bytes, one palette index
per pixel, row by row (the layout graphx's gfx_sprite_t uses). The palette is fixed:
indexes 0-215 are a 6x6x6 colour cube (index = r*36 + g*6 + b, each level 0-5 = 0,51,
...,255) and 216-255 are forty greys, v = (k+1)*255/40 for k = 0..39 (255 = white).

VIDEO APPVARS (see video.h in the viewer's sources for the exact layout): a clip is a
chain of AppVars with a 20-byte header (magic "HV", next chunk, stored frame size, scale,
fps, frame counts) followed by frames. Each frame is the picture's palette indexes, packed
with a simple run-length scheme. Clips are small on purpose: silent, 80x60 pixels drawn 3x
as big, 5 frames a second.

Text is plain ASCII only (32-126); "(c)", "--", "..." etc. replace symbols the calculator
font lacks. Words longer than the screen are the viewer's job to hard-wrap.
"""

import argparse
import base64
import io
import re
import shutil
import struct
import subprocess
import sys
import time
import unicodedata
from collections import namedtuple
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urldefrag, urljoin, urlparse
from urllib.request import Request, url2pathname, urlopen

try:  # pictures and videos are optional
    from PIL import Image, ImageSequence
except ImportError:  # pragma: no cover
    Image = ImageSequence = None

MAGIC = b"HR"
FORMAT_VERSION = 5
HEADER_RESERVE = 96  # bytes held back per page for the header and closing tokens

(END, HEAD, PARA, BR, CENTER, LINK, LINK_END, BOLD, ITALIC,
 IMAGE, INDENT, PRE, RULE, COLS, COL, COLS_END, LINKA, ANCHOR, VIDEO) = range(19)

NO_LINK = bytes(8)  # link target that isn't on the calculator


def anchor_hash(name):
    """16-bit hash of an anchor name (0 = no anchor). The viewer only compares hashes."""
    name = unquote(name or "").strip()
    if not name:
        return 0
    h = 2166136261
    for b in name.encode("utf-8", "replace"):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) or 1

# State the viewer is in at a given point in the stream.
St = namedtuple("St", "bold ital center pre indent head link cols",
                defaults=(False, False, False, False, 0, 0, None, None))

# ----------------------------------------------------------------------------------------
# Text -> ASCII
# ----------------------------------------------------------------------------------------

CHAR_MAP = {
    "\u00a0": " ", "\u2009": " ", "\u200b": "", "\ufeff": "",
    "\u00a9": "(c)", "\u00ae": "(R)", "\u2122": "(TM)",
    "\u2018": "'", "\u2019": "'", "\u201a": ",", "\u201c": '"', "\u201d": '"', "\u201e": '"',
    "\u2013": "-", "\u2014": "--", "\u2026": "...", "\u2022": "*", "\u00b7": "*",
    "\u00d7": "x", "\u00f7": "/", "\u00b0": "deg", "\u00b1": "+/-",
    "\u00bd": "1/2", "\u00bc": "1/4", "\u00be": "3/4",
    "\u2192": "->", "\u2190": "<-", "\u00ab": "<<", "\u00bb": ">>", "\u00df": "ss",
    "\u2264": "<=", "\u2265": ">=", "\u2260": "!=",
    "\u20ac": "EUR", "\u00a3": "GBP", "\u00a5": "JPY",
}


def asciify(s):
    s = s.replace("\r\n", "\n").replace("\r", "\n")
    out = []
    for ch in s:
        if ch in CHAR_MAP:
            out.append(CHAR_MAP[ch])
        elif ch in "\n\t" or 32 <= ord(ch) < 127:
            out.append(ch)
        elif ch.isspace():
            out.append(" ")
        elif ord(ch) < 32 or ord(ch) == 127:
            continue
        else:
            base = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore").decode()
            if base:
                out.append(base)
            elif not unicodedata.combining(ch):
                out.append("?")
    return "".join(out)


# ----------------------------------------------------------------------------------------
# HTML -> token items
# ----------------------------------------------------------------------------------------

HEAD_TAGS = {"title", "meta", "link", "style", "script", "base", "noscript"}
SKIP = {"script", "style", "noscript", "template", "svg", "iframe", "object", "select",
        "textarea"}
VOID = {"br", "hr", "img", "input", "meta", "link", "area", "base", "col", "embed",
        "param", "source", "track", "wbr"}
HEADINGS = {"h1": 1, "h2": 1, "h3": 2, "h4": 2, "h5": 3, "h6": 3}
BLOCK1 = {"div", "section", "article", "header", "footer", "main", "nav", "aside",
          "address", "figure", "figcaption", "form", "fieldset", "center", "dl",
          "details", "summary", "caption", "tr", "dt", "dd", "li"}

# Old-style HTML leaves <p>, <li>, <td>... unclosed. These tables say what a new start
# tag implicitly closes, and how far up the stack to look before giving up.
IMPLIED = {"p": {"p"}, "li": {"li"}, "td": {"td", "th"}, "th": {"td", "th"},
           "tr": {"tr", "td", "th"}, "dt": {"dt", "dd"}, "dd": {"dt", "dd"}}
SCOPE = {"p": {"div", "td", "th", "table", "li", "ul", "ol", "blockquote", "body"},
         "li": {"ul", "ol"}, "td": {"tr", "table"}, "th": {"tr", "table"},
         "tr": {"table"}, "dt": {"dl"}, "dd": {"dl"}}


def px_attr(v):
    """A width/height attribute as pixels, or None (percentages etc. don't count)."""
    m = re.match(r"\s*(\d+)\s*(px)?\s*$", v or "", re.I)
    return int(m.group(1)) if m else None


VIEW_W, COL_GUTTER, MIN_COL_W, MAX_COLS = 312, 8, 48, 3


def col_geometry(weights):
    """Column x offsets and widths in pixels (same formula as the viewer)."""
    n = len(weights)
    avail = VIEW_W - COL_GUTTER * (n - 1)
    tot = sum(weights) or 1
    ws = [avail * w // tot for w in weights]
    ws[-1] += avail - sum(ws)
    xs, x = [], 0
    for w in ws:
        xs.append(x)
        x += w + COL_GUTTER
    return xs, ws


def cell_weights(specs):
    """Relative column widths (1-255) from a row's width= attributes (None if absent)."""
    pct = []
    for spec in specs:
        m = re.match(r"\s*(\d+(?:\.\d+)?)\s*(%|px)?\s*$", spec or "", re.I)
        if not m:
            pct.append(None)
        else:
            v = float(m.group(1))
            pct.append(v if m.group(2) == "%" else v * 100 / 640)  # px: relative to a 640 page
    unknown = [i for i, v in enumerate(pct) if v is None]
    if unknown:
        known = sum(v for v in pct if v is not None)
        share = max(100 - known, 15 * len(unknown)) / len(unknown)
        pct = [share if v is None else v for v in pct]
    tot = sum(pct) or 1
    weights = [max(1, min(255, round(255 * v / tot))) for v in pct]
    if min(col_geometry(weights)[1]) < MIN_COL_W:  # too narrow to read: share equally
        weights = [1] * len(pct)
    return weights


class Converter(HTMLParser):
    """Walks the HTML and builds a list of (bytes, state_after) items."""

    def __init__(self, resolve, image_cb=None, rows=None, scan=False, video_cb=None):
        super().__init__(convert_charrefs=True)
        self.resolve = resolve          # href -> 8-byte AppVar name (or NO_LINK)
        self.image_cb = image_cb        # (src, width, height, max_width) -> (name8, w, h) or None
        self.video_cb = video_cb        # (src, label) -> (name8, label) or None
        self.video = None               # the <video> we are inside: its src and title
        self.scan = scan                # first pass: only collect table rows and links
        self.rows = rows if rows is not None else []  # per <tr>: width= of each cell
        self.hrefs = []                 # every link target seen (for following links)
        self.tr_count = 0
        self.tr_stack = []              # open <tr>: (index in self.stack, row number)
        self.col_row = None             # row currently shown as columns
        self.col_n = 0
        self.col_idx = -1
        self.col_weights = None
        self.col_widths = None
        self.items = []                 # (token bytes, St after emitting it)
        self.stack = []                 # open tags: (tag, desired state before it)
        self.lists = []                 # per open <ul>/<ol>: None or running count
        self.des = St()                 # state the markup asks for right now
        self.em = St()                  # state already written to the stream
        self.skip = 0
        self.in_head = False
        self.in_title = False
        self.title_buf = []
        self.started = False
        self.at_line_start = True
        self.last_break_idx = None
        self.pending_space = False
        self.pending_sep = False
        self.skip_nl = False

    # ---- emitting ------------------------------------------------------------------

    def push(self, b):
        self.items.append((b, self.em))

    def brk(self, level):
        """Request a line (1) or paragraph (2) break without doubling up."""
        if not self.started:
            return
        if self.at_line_start:
            i = self.last_break_idx
            if (level == 2 and i is not None and i == len(self.items) - 1
                    and self.items[i][0] == bytes([BR])):
                self.em = self.em._replace(head=0)
                self.items[i] = (bytes([PARA]), self.em)
            return
        self.sync_close()  # a link that has ended closes before the break, not after it
        if level == 2:
            self.em = self.em._replace(head=0)  # a paragraph break ends a heading
        self.push(bytes([PARA if level == 2 else BR]))
        self.at_line_start = True
        self.last_break_idx = len(self.items) - 1
        self.pending_space = False

    def force_break(self):
        if not self.started:
            return
        self.sync_close()
        self.push(bytes([BR]))
        self.at_line_start = True
        self.last_break_idx = len(self.items) - 1
        self.pending_space = False

    def sync_close(self):
        if self.em.link is not None and self.des.link != self.em.link:
            self.em = self.em._replace(link=None)
            self.push(bytes([LINK_END]))

    def sync_open(self):
        d = self.des
        for field, op in (("center", CENTER), ("bold", BOLD), ("ital", ITALIC), ("pre", PRE)):
            want = getattr(d, field)
            if want != getattr(self.em, field):
                self.em = self.em._replace(**{field: want})
                self.push(bytes([op, int(want)]))
        indent = min(d.indent, 3)
        if indent != self.em.indent:
            self.em = self.em._replace(indent=indent)
            self.push(bytes([INDENT, indent]))
        if d.head and d.head != self.em.head:
            self.em = self.em._replace(head=d.head)
            self.push(bytes([HEAD, d.head]))
        if d.link is not None and d.link != self.em.link:
            self.em = self.em._replace(link=d.link)
            self.push(bytes([LINKA if len(d.link) == 10 else LINK]) + d.link)

    def lead(self):
        """Everything that must precede visible content: link close, spacing, state."""
        self.sync_close()
        if self.pending_sep:
            if not self.at_line_start:
                self.push(b" | ")
            self.pending_sep = False
        elif self.pending_space and not self.at_line_start:
            self.push(b" ")
        self.pending_space = False
        self.sync_open()
        self.at_line_start = False
        self.started = True

    def emit(self, text):
        self.lead()
        self.push(text.encode("ascii"))

    # ---- tag handling --------------------------------------------------------------

    def find(self, tag):
        bounds = SCOPE.get(tag, ())
        for i in range(len(self.stack) - 1, -1, -1):
            t = self.stack[i][0]
            if t == tag:
                return i
            if t in bounds:
                return None
        return None

    def pop_to(self, i):
        for t, _ in self.stack[i:]:
            if t in ("ul", "ol") and self.lists:
                self.lists.pop()
        while self.tr_stack and self.tr_stack[-1][0] >= i:  # a row ended
            _, ridx = self.tr_stack.pop()
            if ridx == self.col_row:
                self.end_columns()
        self.des = self.stack[i][1]
        del self.stack[i:]

    # ---- side-by-side columns ----------------------------------------------------

    def cell_start(self, a):
        """A <td>/<th> begins: open or advance a column block, or fall back to ' | '."""
        row = self.tr_stack[-1][1] if self.tr_stack else None
        if row is not None and self.scan:
            self.rows[row].append(a.get("width"))
            return
        cells = self.rows[row] if row is not None and row < len(self.rows) else []
        if 2 <= len(cells) <= MAX_COLS and self.col_row in (None, row):
            if self.col_row is None:
                self.start_columns(row, cells)
            elif self.col_idx + 1 < self.col_n:
                self.next_column()
            return
        if not self.at_line_start:
            self.pending_sep = True

    def col_state(self):
        """The viewer starts every column in the plain state; mirror that here."""
        self.em = St(cols=(self.col_weights, self.col_idx))
        self.at_line_start = True
        self.last_break_idx = None
        self.pending_space = False
        self.pending_sep = False

    def start_columns(self, row, cells):
        self.col_row, self.col_n, self.col_idx = row, len(cells), 0
        self.col_weights = tuple(cell_weights(cells))
        self.col_widths = col_geometry(self.col_weights)[1]
        self.brk(1)
        self.col_state()
        self.started = True
        self.push(bytes([COLS, self.col_n]) + bytes(self.col_weights))

    def next_column(self):
        self.col_idx += 1
        self.col_state()
        self.push(bytes([COL]))

    def end_columns(self):
        self.col_row = None
        self.col_widths = None
        self.em = St()
        self.at_line_start = True
        self.last_break_idx = None
        self.pending_space = self.pending_sep = False
        self.push(bytes([COLS_END]))

    def finish(self):
        """Call after feeding all the HTML: closes a row left open at the end."""
        if self.col_row is not None:
            self.end_columns()

    def implied(self, tag):
        targets = IMPLIED.get(tag)
        if not targets:
            return
        bounds = SCOPE.get(tag, ())
        found = None
        for i in range(len(self.stack) - 1, -1, -1):
            t = self.stack[i][0]
            if t in targets:
                found = i
            if t in bounds:
                break
        if found is not None:
            self.pop_to(found)

    def begin(self, tag, a):
        old = new = self.des
        if tag in ("td", "th"):  # cells start left-aligned (headers centred), whatever
            new = new._replace(center=(tag == "th"))  # <center> surrounds the table
            if a.get("align", "").lower() in ("left", "right"):
                new = new._replace(center=False)
        if (tag == "center" or a.get("align", "").lower() == "center"
                or re.search(r"text-align\s*:\s*center", a.get("style", ""), re.I)):
            new = new._replace(center=True)
        if tag in ("b", "strong", "th", "dt"):
            new = new._replace(bold=True)
        elif tag in ("i", "em", "cite", "var", "dfn"):
            new = new._replace(ital=True)
        elif tag == "pre":
            new = new._replace(pre=True)
        elif tag in ("ul", "ol", "blockquote"):
            new = new._replace(indent=min(old.indent + 1, 3))
        elif tag in HEADINGS:
            new = new._replace(head=HEADINGS[tag])
        elif tag == "a":
            href = a.get("href", "").strip()
            if href and not re.match(r"(mailto|javascript|tel|data):", href, re.I):
                self.hrefs.append(href)
                new = new._replace(link=self.resolve(href))

        if tag in ("p", "blockquote", "table", "pre") or tag in HEADINGS:
            self.brk(2)
        elif tag in BLOCK1 or tag in ("ul", "ol"):
            self.brk(1)
        if tag == "pre":
            self.skip_nl = True
        if tag in ("td", "th"):
            self.cell_start(a)
        if tag in ("ul", "ol"):
            self.lists.append(0 if tag == "ol" else None)

        if tag == "tr":
            self.tr_stack.append((len(self.stack), self.tr_count))
            if self.scan:
                self.rows.append([])
            self.tr_count += 1

        self.stack.append((tag, old))
        self.des = new

        if tag == "li":
            if self.lists and self.lists[-1] is not None:
                self.lists[-1] += 1
                mark = "%d." % self.lists[-1]
            else:
                mark = "*"
            self.emit(mark)
            self.pending_space = True

    def mark_anchor(self, tag, a):
        """<a name=x> or id=x: a spot that links can jump to."""
        h = anchor_hash(a.get("id") or (a.get("name") if tag == "a" else ""))
        if h:
            self.push(bytes([ANCHOR]) + struct.pack("<H", h))

    def end_tag(self, tag):
        i = self.find(tag)
        if i is None:
            if tag == "p":
                self.brk(2)
            return
        self.pop_to(i)
        if tag in HEADINGS or tag in ("p", "pre", "table", "blockquote"):
            self.brk(2)
        elif tag in ("ul", "ol"):
            self.brk(2 if self.des.indent == 0 else 1)
        elif tag in BLOCK1:
            self.brk(1)

    def void(self, tag, a):
        if tag == "br":
            self.force_break()
        elif tag == "hr":
            self.brk(1)
            self.push(bytes([RULE]))
            self.started = True
            self.at_line_start = True
            self.last_break_idx = None
            self.pending_space = False
        elif tag == "img":
            w, h = px_attr(a.get("width")), px_attr(a.get("height"))
            if (w is not None and w <= 2) or (h is not None and h <= 2):
                return  # tracking pixel / spacer
            alt = re.sub(r"\s+", " ", asciify(a.get("alt", ""))).strip()[:40]
            b = alt.encode("ascii")
            maxw = self.col_widths[self.col_idx] if self.col_widths else None
            info = self.image_cb(a.get("src", ""), w, h, maxw) if self.image_cb else None
            name, iw, ih = info if info else (NO_LINK, 0, 0)
            self.lead()
            self.push(bytes([IMAGE]) + name + bytes([iw, ih, len(b)]) + b)
            if info:  # a real picture is a block of its own on the calculator
                self.at_line_start = True
                self.last_break_idx = None
                self.pending_space = False

    # ---- HTMLParser callbacks ------------------------------------------------------

    def handle_starttag(self, tag, attrs):
        a = {k: (v or "") for k, v in attrs}
        if tag == "title":
            self.in_title = True
            return
        if self.in_head and tag not in HEAD_TAGS:
            self.in_head = False
        if tag == "head":
            self.in_head = True
            return
        if tag == "body":
            self.skip = 0
        if tag == "video" and not self.skip and not self.in_head:
            self.video = {"src": a.get("src", "").strip(), "alt": a.get("title", "")}
            self.skip += 1  # whatever is inside is fallback text for old browsers
            return
        if tag == "source" and self.video is not None:
            if not self.video["src"]:
                self.video["src"] = a.get("src", "").strip()
            return
        if tag in SKIP:
            self.skip += 1
            return
        if self.skip:
            return
        if tag in VOID:
            self.void(tag, a)
            return
        self.implied(tag)
        self.begin(tag, a)
        self.mark_anchor(tag, a)

    def handle_endtag(self, tag):
        if tag == "title":
            self.in_title = False
            return
        if tag == "head":
            self.in_head = False
            return
        if tag == "video" and self.video is not None:
            self.skip = max(0, self.skip - 1)
            self.finish_video()
            return
        if tag in SKIP:
            self.skip = max(0, self.skip - 1)
            return
        if self.skip or tag in VOID:
            return
        self.end_tag(tag)

    def finish_video(self):
        """</video>: put a playable clip in the page, or its name if there isn't one."""
        v, self.video = self.video, None
        if not v["src"]:
            return
        label = re.sub(r"\s+", " ", asciify(v["alt"])).strip()[:30]
        if not label:
            label = asciify(unquote(Path(urlparse(v["src"]).path).stem))[:30] or "clip"
        info = self.video_cb(v["src"], label) if self.video_cb else None
        self.brk(1)
        if info:
            name, shown = info
            b = shown.encode("ascii")[:40]
            self.lead()
            self.push(bytes([VIDEO]) + name + bytes([len(b)]) + b)
        else:
            self.emit("[video: %s]" % label)
        self.brk(1)

    def handle_data(self, data):
        if self.in_title:
            self.title_buf.append(data)
            return
        if self.skip or self.in_head:
            return
        text = asciify(data)
        if self.des.pre:
            text = text.expandtabs(4)
            if self.skip_nl and text.startswith("\n"):
                text = text[1:]
            self.skip_nl = False
            for n, line in enumerate(text.split("\n")):
                if n:
                    self.force_break()
                if line:
                    self.emit(line)
            return
        for m in re.finditer(r"\s+|\S+", text):
            tok = m.group()
            if tok.isspace():
                self.pending_space = True
            else:
                self.emit(tok)

    def get_title(self):
        return re.sub(r"\s+", " ", asciify("".join(self.title_buf))).strip()[:40]


# ----------------------------------------------------------------------------------------
# Pagination and file output
# ----------------------------------------------------------------------------------------

def restore_tokens(st):
    """Commands that put the viewer back into state `st` at the start of a split page."""
    b = b""
    if st.cols:  # reopen the column block we were in (earlier columns stay empty)
        weights, idx = st.cols
        b += bytes([COLS, len(weights)]) + bytes(weights) + bytes([COL]) * idx
    if st.center:
        b += bytes([CENTER, 1])
    if st.bold:
        b += bytes([BOLD, 1])
    if st.ital:
        b += bytes([ITALIC, 1])
    if st.pre:
        b += bytes([PRE, 1])
    if st.indent:
        b += bytes([INDENT, st.indent])
    if st.head:
        b += bytes([HEAD, st.head])
    if st.link is not None:
        b += bytes([LINKA if len(st.link) == 10 else LINK]) + st.link
    return b


def is_split_point(item, top_level_only):
    ok = item[0] in (bytes([PARA]), bytes([COLS_END]))
    return ok and (not top_level_only or item[1].cols is None)


def paginate(items, limit):
    avail = limit - HEADER_RESERVE
    pages, n, i, prev_state = [], len(items), 0, St()
    while True:
        head = restore_tokens(prev_state)
        size, j = len(head), i
        while j < n and size + len(items[j][0]) <= avail:
            size += len(items[j][0])
            j += 1
        if j < n:  # must split: prefer a paragraph break outside any column block
            for top_level_only in (True, False):
                k = j
                while k > i and not is_split_point(items[k - 1], top_level_only):
                    k -= 1
                if k > i and sum(len(x[0]) for x in items[i:k]) >= avail // 2:
                    j = k
                    break
            if j == i:
                j = i + 1
        body = head + b"".join(x[0] for x in items[i:j])
        end_state = items[j - 1][1] if j > i else prev_state
        if end_state.link is not None:
            body += bytes([LINK_END])
        if end_state.cols:  # split inside a column block: close it on this page
            body += bytes([COLS_END])
        pages.append(body + bytes([END]))
        if j >= n:
            return pages
        prev_state, i = end_state, j


def pad8(name):
    return name.encode("ascii").ljust(8, b"\0") if name else bytes(8)


def build_page(tokens, title, prev, nxt, listed=True):
    t = title.encode("ascii")[:40]
    return (MAGIC + bytes([FORMAT_VERSION, 1 if listed else 0]) + pad8(prev) + pad8(nxt)
            + bytes([len(t)]) + t + tokens)


def make_8xv(name, content):
    """Wrap `content` in a TI-83+/84+ family archived AppVar file."""
    data = struct.pack("<H", len(content)) + content  # AppVar data starts with its size
    entry = (struct.pack("<HH", 13, len(data)) + bytes([0x15])
             + name.encode("ascii").ljust(8, b"\0") + bytes([0x00, 0x80])
             + struct.pack("<H", len(data)) + data)
    header = (b"**TI83F*" + bytes([0x1A, 0x0A, 0x00])
              + b"Created by html2ce".ljust(42, b"\0") + struct.pack("<H", len(entry)))
    return header + entry + struct.pack("<H", sum(entry) & 0xFFFF)


def dump_page(name, data):
    """Readable rendering of a page, so you can check the converter's output."""
    if data[:2] != MAGIC:
        return "(not an html2ce page)"
    prev = data[4:12].rstrip(b"\0").decode() or "-"
    nxt = data[12:20].rstrip(b"\0").decode() or "-"
    tl = data[20]
    title = data[21:21 + tl].decode()
    listed = bool(data[3] & 1) or data[2] == 3
    pos = 21 + tl
    out = ["== %s  title=%r  prev=%s  next=%s%s ==" % (name, title, prev, nxt,
                                                       "" if listed else "  (not in menu)")]
    line = []
    flags = {CENTER: "CENTER", BOLD: "B", ITALIC: "I", PRE: "PRE"}
    while pos < len(data):
        c = data[pos]
        if c >= 0x20:
            end = pos
            while end < len(data) and data[end] >= 0x20:
                end += 1
            line.append(data[pos:end].decode())
            pos = end
            continue
        pos += 1
        if c == END:
            line.append("{END}")
            break
        elif c == HEAD:
            line.append("{H%d}" % data[pos]); pos += 1
        elif c == PARA:
            line.append("{P}"); out.append("".join(line)); out.append(""); line = []
        elif c == BR:
            line.append("{BR}"); out.append("".join(line)); line = []
        elif c in flags:
            line.append("{%s:%d}" % (flags[c], data[pos])); pos += 1
        elif c == INDENT:
            line.append("{IND:%d}" % data[pos]); pos += 1
        elif c == LINK:
            nm = data[pos:pos + 8].rstrip(b"\0").decode() or "none"
            line.append("{LINK>%s}" % nm); pos += 8
        elif c == LINKA:
            nm = data[pos:pos + 8].rstrip(b"\0").decode() or "none"
            line.append("{LINK>%s#%04x}" % (nm, data[pos + 8] | data[pos + 9] << 8)); pos += 10
        elif c == ANCHOR:
            line.append("{ANCHOR %04x}" % (data[pos] | data[pos + 1] << 8)); pos += 2
        elif c == VIDEO:
            nm = data[pos:pos + 8].rstrip(b"\0").decode() or "none"
            n = data[pos + 8]
            line.append("{VIDEO:%s %s}" % (nm, data[pos + 9:pos + 9 + n].decode())); pos += 9 + n
        elif c == LINK_END:
            line.append("{/LINK}")
        elif c == IMAGE:
            nm = data[pos:pos + 8].rstrip(b"\0").decode() or "none"
            iw, ih, n = data[pos + 8], data[pos + 9], data[pos + 10]
            alt = data[pos + 11:pos + 11 + n].decode()
            line.append("{IMG:%s %dx%d %s}" % (nm, iw, ih, alt)); pos += 11 + n
        elif c == RULE:
            line.append("{HR}"); out.append("".join(line)); line = []
        elif c == COLS:
            n = data[pos]
            line.append("{COLS:%s}" % ",".join(str(x) for x in data[pos + 1:pos + 1 + n]))
            out.append("".join(line)); line = []; pos += 1 + n
        elif c == COL:
            line.append("{COL}"); out.append("".join(line)); line = []
        elif c == COLS_END:
            line.append("{/COLS}"); out.append("".join(line)); line = []
        else:
            line.append("{?0x%02x}" % c)
    out.append("".join(line))
    return "\n".join(out)


# ----------------------------------------------------------------------------------------
# Pictures -> sprite AppVars
# ----------------------------------------------------------------------------------------

def build_palette():
    """The fixed calculator palette (must match init_palette() in the viewer)."""
    pal = [(r * 51, g * 51, b * 51) for r in range(6) for g in range(6) for b in range(6)]
    pal += [(((k + 1) * 255) // 40,) * 3 for k in range(40)]
    return pal


def fetch_bytes(url):
    if url.startswith("data:"):
        head, _, payload = url.partition(",")
        if ";base64" in head:
            return base64.b64decode(payload)
        from urllib.parse import unquote_to_bytes
        return unquote_to_bytes(payload)
    if re.match(r"https?://", url, re.I):
        req = Request(url, headers={"User-Agent": "Mozilla/5.0 (html2ce)"})
        with urlopen(req, timeout=20) as r:
            return r.read(20_000_000)
    if url.startswith("file:"):
        return Path(url2pathname(urlparse(url).path)).read_bytes()
    raise ValueError("unsupported image address")


class ImageStore:
    """Turns pictures into 8-bit sprites in the fixed palette, one per distinct picture."""

    def __init__(self, enabled, max_w, max_h):
        self.max_w, self.max_h = max_w, max_h
        self.enabled = enabled
        self.sprites = {}  # AppVar name -> content bytes
        self.cache = {}    # (url, width, height) -> (name8, w, h) or None
        if enabled and Image is None:
            print("note: Pillow is not installed, so pictures will show their alt text "
                  "(pip install pillow)")
            self.enabled = False
        if self.enabled:
            flat = [c for rgb in build_palette() for c in rgb]
            self.pal_img = Image.new("P", (1, 1))
            self.pal_img.putpalette(flat)

    def to_sprite(self, raw, want_w, want_h, max_w=None):
        im = Image.open(io.BytesIO(raw))
        im.load()
        if im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info):
            im = im.convert("RGBA")
            bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
            bg.alpha_composite(im)
            im = bg
        im = im.convert("RGB")
        w, h = im.size
        if want_w and want_h:
            w, h = want_w, want_h
        elif want_w:
            w, h = want_w, max(1, round(h * want_w / w))
        elif want_h:
            w, h = max(1, round(w * want_h / h)), want_h
        limit_w = min(self.max_w, max_w) if max_w else self.max_w
        scale = min(1.0, limit_w / w, self.max_h / h)  # shrink to fit, never enlarge
        w, h = max(1, round(w * scale)), max(1, round(h * scale))
        if (w, h) != im.size:
            im = im.resize((w, h), getattr(Image, "Resampling", Image).LANCZOS)
        q = im.quantize(palette=self.pal_img)
        return bytes([w, h]) + q.tobytes(), w, h


# ----------------------------------------------------------------------------------------
# Videos -> HV AppVars
# ----------------------------------------------------------------------------------------

VIDEO_HEADER = 20           # bytes at the start of every chunk (see the docstring)
VIDEO_CHUNK = 60000         # most bytes of one chunk (an AppVar holds a bit under 65000)
VIDEO_PIX_MAX = 6000        # most pixels per stored frame (video.h)
VIDEO_FRAMES_MAX = 300      # most frames per clip (video.h)
VIDEO_EXT = {".mp4", ".m4v", ".mov", ".webm", ".mkv", ".avi", ".gif"}


def rle_encode(data):
    """Pack palette indexes: c < 128 = c+1 literal bytes, c >= 128 = next byte x (c-126)."""
    out, i, n = bytearray(), 0, len(data)
    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 129:
            run += 1
        if run >= 3:
            out += bytes([126 + run, data[i]])
            i += run
            continue
        j = i
        while j < n and j - i < 128:
            if j + 2 < n and data[j] == data[j + 1] == data[j + 2]:
                break
            j += 1
        j = max(j, i + 1)
        out.append(j - i - 1)
        out += data[i:j]
        i = j
    return bytes(out)


def rle_decode(data, size):
    out, i = bytearray(), 0
    while i < len(data) and len(out) < size:
        c = data[i]
        i += 1
        if c < 128:
            out += data[i:i + c + 1]
            i += c + 1
        else:
            out += bytes([data[i]]) * (c - 126)
            i += 1
    return bytes(out[:size])


class VideoStore:
    """Turns videos into small silent clips: a few frames a second, tiny pictures in the
    calculator palette (no dithering, so they pack well), run-length packed, in chunks."""

    def __init__(self, w, h, fps, seconds, max_kb, scale):
        self.w, self.h, self.fps, self.seconds, self.max_kb = w, h, fps, seconds, max_kb
        self.scale = scale or max(1, min(312 // w, 214 // h))
        self.max_frames = max(1, min(VIDEO_FRAMES_MAX, int(fps * seconds)))
        self.ffmpeg = shutil.which("ffmpeg")
        self.cache = {}   # url -> (name8, label) or None, so a video is only asked about once
        self.out = {}     # AppVar name -> content
        self.count = 0
        if Image is not None:
            flat = [c for rgb in build_palette() for c in rgb]
            self.pal_img = Image.new("P", (1, 1))
            self.pal_img.putpalette(flat)

    def problem(self, url):
        """Why this video can't be converted here, or None."""
        if Image is None:
            return "Pillow is not installed (pip install pillow)"
        if url.startswith("data:"):
            return "inline data: videos are not supported"
        if Path(urlparse(url).path).suffix.lower() != ".gif" and not self.ffmpeg:
            return "ffmpeg was not found (install it and put it on your PATH)"
        return None

    def fit(self, im):
        """A picture as w*h RGB bytes: shrunk to fit, black bars around it if needed."""
        im = im.convert("RGBA")
        bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
        bg.alpha_composite(im)
        im = bg.convert("RGB")
        k = min(self.w / im.width, self.h / im.height)
        nw, nh = max(1, round(im.width * k)), max(1, round(im.height * k))
        im = im.resize((nw, nh), getattr(Image, "Resampling", Image).LANCZOS)
        canvas = Image.new("RGB", (self.w, self.h), (0, 0, 0))
        canvas.paste(im, ((self.w - nw) // 2, (self.h - nh) // 2))
        return canvas.tobytes()

    def gif_frames(self, raw):
        im = Image.open(io.BytesIO(raw))
        shots, t = [], 0
        for fr in ImageSequence.Iterator(im):
            shots.append((t, self.fit(fr)))
            t += max(fr.info.get("duration", 100) or 100, 20)  # ms
        n = max(1, min(self.max_frames, int(t * self.fps / 1000)))
        out, k = [], 0
        for i in range(n):  # the picture that was showing at i/fps seconds
            while k + 1 < len(shots) and shots[k + 1][0] <= i * 1000 / self.fps:
                k += 1
            out.append(shots[k][1])
        return out

    def ffmpeg_frames(self, url):
        src = url2pathname(urlparse(url).path) if url.startswith("file:") else url
        vf = ("fps=%d,scale=%d:%d:force_original_aspect_ratio=decrease:flags=area,"
              "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black" % (self.fps, self.w, self.h, self.w, self.h))
        cmd = [self.ffmpeg, "-v", "error", "-nostdin", "-i", src, "-an", "-sn",
               "-frames:v", str(self.max_frames), "-vf", vf,
               "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
        if r.returncode:
            raise ValueError("ffmpeg failed: " + r.stderr.decode("utf-8", "replace").strip()[-200:])
        size = self.w * self.h * 3
        return [r.stdout[i:i + size] for i in range(0, len(r.stdout) - size + 1, size)]

    def convert(self, url):
        """Frame records ([length][packed frame]) for the video, and whether it was cut short."""
        if Path(urlparse(url).path).suffix.lower() == ".gif":
            raws = self.gif_frames(fetch_bytes(url))
        else:
            raws = self.ffmpeg_frames(url)
        if not raws:
            raise ValueError("no frames could be read")
        dither_none = getattr(Image, "Dither", Image).NONE
        records, total, cut = [], 0, False
        for raw in raws:
            q = Image.frombytes("RGB", (self.w, self.h), raw).quantize(
                palette=self.pal_img, dither=dither_none)
            data = rle_encode(q.tobytes())
            rec = struct.pack("<H", len(data)) + data
            if records and total + len(rec) > self.max_kb * 1024:
                cut = True
                break
            records.append(rec)
            total += len(rec)
        return records, cut

    def chunks(self, records, names_for):
        """Split frame records into AppVar contents. names_for(count) gives the chunk names."""
        groups, cur, size = [], [], VIDEO_HEADER
        for rec in records:
            if cur and size + len(rec) > VIDEO_CHUNK:
                groups.append(cur)
                cur, size = [], VIDEO_HEADER
            cur.append(rec)
            size += len(rec)
        groups.append(cur)
        names = names_for(len(groups))
        out = []
        for i, g in enumerate(groups):
            head = (b"HV" + bytes([1, 0]) + pad8(names[i + 1] if i + 1 < len(names) else None)
                    + bytes([self.w, self.h, self.scale, self.fps])
                    + struct.pack("<HH", len(records), len(g)))
            out.append((names[i], head + b"".join(g)))
        return out


# ----------------------------------------------------------------------------------------
# Loading pages, naming, links
# ----------------------------------------------------------------------------------------

def is_url(s):
    return re.match(r"https?://", s, re.I) is not None


def decode_bytes(raw, hint=None):
    sniff = re.search(rb"charset=[\"']?([\w-]+)", raw[:4096], re.I)
    for enc in (hint, sniff.group(1).decode("ascii") if sniff else None, "utf-8", "cp1252"):
        if not enc:
            continue
        try:
            return raw.decode(enc)
        except (LookupError, UnicodeDecodeError):
            continue
    return raw.decode("latin-1")


HTML_EXT = {".html", ".htm", ".xhtml", ".shtml", ""}


def load(src, strict=False):
    """Return (html text, base URL used for resolving that page's links).
    strict=True (for pages found by following links) refuses things that aren't HTML.
    A video file given as PAGE becomes a one-line page that holds the video."""
    path = urlparse(src).path if is_url(src) or src.startswith("file:") else src
    if not strict and Path(path).suffix.lower() in VIDEO_EXT:
        base = src if is_url(src) or src.startswith("file:") else Path(src).resolve().as_uri()
        fname = Path(urlparse(base).path).name
        esc = lambda t: t.replace("&", "&amp;").replace("<", "&lt;").replace('"', "&quot;")
        return ("<html><head><title>%s</title></head><body><video src=\"%s\"></video></body></html>"
                % (esc(unquote(Path(fname).stem)), esc(fname))), base
    if is_url(src):
        req = Request(src, headers={"User-Agent": "Mozilla/5.0 (html2ce)"})
        with urlopen(req, timeout=20) as r:
            ctype = r.headers.get_content_type()
            if strict and not (ctype.startswith("text/") or ctype == "application/xhtml+xml"):
                raise ValueError("not an HTML page (%s)" % ctype)
            raw = r.read(5_000_000)
            hint = r.headers.get_content_charset()
            base = r.geturl()
        return decode_bytes(raw, hint), base
    p = Path(url2pathname(urlparse(src).path)) if src.startswith("file:") else Path(src)
    if strict and p.suffix.lower() not in HTML_EXT:
        raise ValueError("not an HTML file")
    return decode_bytes(p.read_bytes()), p.resolve().as_uri()


def norm_key(url):
    """Identity of a page for link matching (ignores #fragment, scheme and www.)."""
    p = urlparse(urldefrag(url)[0])
    host = p.netloc.lower()
    if host.startswith("www."):
        host = host[4:]
    path = p.path or "/"
    if path.endswith("/"):
        path += "index.html"
    if p.scheme == "file":
        path = path.lower()
    return host + path + ("?" + p.query if p.query else "")


def saved_from(html):
    """Browsers that save a page add '<!-- saved from url=(0041)https://... -->'. Links in the
    saved copy still point at the original address, so that address names this page too."""
    m = re.search(r"<!--\s*saved from url=\(\d+\)\s*(https?://\S+?)\s*-->", html[:2000], re.I)
    return m.group(1) if m else None


def clean_name(s):
    name = re.sub(r"[^A-Za-z0-9]", "", s).upper()[:8] or "PAGE"
    return "P" + name[:7] if name[0].isdigit() else name


def derive_name(src):
    if is_url(src):
        p = urlparse(src)
        stem = Path(p.path).stem or p.netloc.replace("www.", "").split(".")[0]
    else:
        stem = Path(src).stem
    return clean_name(stem)


def unique_name(name, used):
    cand = name
    n = 0
    while cand in used:
        n += 1
        if n > 99:
            sys.exit("Could not find a free AppVar name for " + name)
        cand = name[:6] + "%02d" % n
    used.add(cand)
    return cand


def page_names(base, count):
    if count > 99:
        sys.exit("%s would need %d pages; try a larger --page-size" % (base, count))
    return [base] + [base[:6] + "%02d" % n for n in range(2, count + 1)]


SKIP_EXT = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".svg", ".ico", ".pdf", ".zip",
            ".gz", ".tar", ".7z", ".rar", ".exe", ".msi", ".dmg", ".iso", ".mp3", ".mp4",
            ".avi", ".mov", ".wav", ".css", ".js", ".json", ".xml", ".rss", ".doc", ".docx",
            ".xls", ".xlsx", ".ppt", ".pptx", ".8xp", ".8xv", ".8xg"}


def discover(base, href):
    """The address to fetch for a link, or None if it isn't a page worth following."""
    href = (href or "").strip()
    if not href or href.startswith("#") or re.match(r"(mailto|javascript|tel|data|ftp):", href, re.I):
        return None
    url = urldefrag(urljoin(base, href))[0]
    p = urlparse(url)
    if p.scheme not in ("http", "https", "file") or Path(p.path).suffix.lower() in SKIP_EXT:
        return None
    return url


def site_of(url):
    host = urlparse(url).netloc.lower()
    return host[4:] if host.startswith("www.") else host


def scan_page(html):
    """First pass over a page: finds its table rows and links without converting anything."""
    s = Converter(lambda href: NO_LINK, None, scan=True)
    s.feed(html)
    s.close()
    return s


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Convert HTML pages (files or URLs) into TI-84 Plus CE AppVars.")
    ap.add_argument("inputs", nargs="+", metavar="PAGE", help="HTML file or http(s) URL")
    ap.add_argument("-o", "--outdir", default=".", help="where to write files (default: .)")
    ap.add_argument("-n", "--name", help="AppVar name of the first page (max 8 letters/digits)")
    ap.add_argument("--depth", type=int, default=1, metavar="N",
                    help="levels of links to follow from each given page: 0 = only the page, "
                         "1 (default) = also every page it links to, 2 = also the pages "
                         "those link to, ...")
    ap.add_argument("--max-pages", type=int, default=40, metavar="N",
                    help="stop following links once this many pages are loaded (default 40)")
    ap.add_argument("--same-site", action="store_true",
                    help="only follow links to the same website as the first page")
    ap.add_argument("--list-all", action="store_true",
                    help="list every converted page in the viewer's menu, not just the pages "
                         "you gave (pages reached by following links are hidden by default)")
    ap.add_argument("--videos", choices=("ask", "yes", "no"), default="ask",
                    help="what to do with videos: ask about each one (default), convert all "
                         "of them, or none (needs ffmpeg; GIFs only need Pillow)")
    ap.add_argument("--video-size", default="80x60", metavar="WxH",
                    help="size of the stored video pictures (default 80x60, max 6000 pixels)")
    ap.add_argument("--video-scale", type=int, default=0, metavar="N",
                    help="draw each stored pixel N x N on the screen (default: as big as fits)")
    ap.add_argument("--video-fps", type=int, default=5, metavar="N",
                    help="frames per second of converted videos (default 5, 1-15)")
    ap.add_argument("--video-seconds", type=int, default=15, metavar="N",
                    help="longest video to convert, in seconds (default 15)")
    ap.add_argument("--video-max-kb", type=int, default=400, metavar="N",
                    help="most kilobytes one video may take on the calculator (default 400)")
    ap.add_argument("--dump", action="store_true", help="print a readable view of each page")
    ap.add_argument("--bin", action="store_true",
                    help="also write the raw page data as NAME.bin (for convbin)")
    ap.add_argument("--page-size", type=int, default=16000,
                    help="max bytes per AppVar page (default 16000, limit 65000; the viewer "
                         "keeps about 30000 bytes of laid-out text per page in RAM)")
    ap.add_argument("--no-images", action="store_true",
                    help="don't convert pictures; show their alt text instead")
    ap.add_argument("--image-max", default="240x160", metavar="WxH",
                    help="largest picture size in pixels (default 240x160; max 255x200)")
    args = ap.parse_args(argv)

    try:
        max_w, max_h = (int(x) for x in args.image_max.lower().split("x"))
    except ValueError:
        ap.error("--image-max must look like 240x160")
    if not (8 <= max_w <= 255 and 8 <= max_h <= 200):
        ap.error("--image-max must be between 8x8 and 255x200")
    if args.name and len(args.inputs) > 1:
        ap.error("-n/--name only works with a single input")
    if not 1000 <= args.page_size <= 65000:
        ap.error("--page-size must be between 1000 and 65000")
    if args.depth < 0 or args.max_pages < 1:
        ap.error("--depth must be 0 or more and --max-pages at least 1")
    try:
        vw, vh = (int(x) for x in args.video_size.lower().split("x"))
    except ValueError:
        ap.error("--video-size must look like 80x60")
    if not (4 <= vw <= 255 and 4 <= vh <= 255 and vw * vh <= VIDEO_PIX_MAX):
        ap.error("--video-size must be between 4x4 and 255x255, at most %d pixels" % VIDEO_PIX_MAX)
    if not 1 <= args.video_fps <= 15 or args.video_seconds < 1 or args.video_max_kb < 10:
        ap.error("--video-fps must be 1-15, --video-seconds at least 1, --video-max-kb at least 10")
    vstore = VideoStore(vw, vh, args.video_fps, args.video_seconds, args.video_max_kb,
                        args.video_scale)
    if vstore.scale < 1 or vw * vstore.scale > 320 or vh * vstore.scale > 214:
        ap.error("--video-scale makes the picture bigger than the screen (320x214)")
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    # ---- 1. load the given pages and follow their links -------------------------------------
    pages, seen, queued = [], set(), set()
    queue = [(src, 0, True) for src in args.inputs]  # (address, depth, typed by you)
    root_site, not_followed = None, 0
    while queue:
        src, depth, explicit = queue.pop(0)
        if not explicit and len(pages) >= args.max_pages:
            not_followed += 1
            continue
        if not explicit and is_url(src):
            time.sleep(0.3)  # be polite to the server
        try:
            html, base = load(src, strict=not explicit)
        except Exception as e:  # noqa: BLE001 - show any load problem plainly
            if explicit:
                sys.exit("Could not read %s: %s" % (src, e))
            print("  skipped %s: %s" % (src[:70], e))
            continue
        key = norm_key(base)
        if key in seen:
            continue
        seen.add(key)
        if is_url(src):
            seen.add(norm_key(src))
        alias = saved_from(html)
        if alias:
            seen.add(norm_key(alias))
        scan = scan_page(html)
        pages.append({"src": src, "html": html, "base": base, "rows": scan.rows,
                      "alias": alias, "main": explicit})
        print("loaded %s%s" % (src[:80], "" if depth == 0 else "  (followed link, depth %d)" % depth))
        if root_site is None:
            root_site = site_of(base)
        if depth < args.depth:
            for href in scan.hrefs:
                url = discover(base, href)
                if not url:
                    continue
                k = norm_key(url)
                if k in seen or k in queued:
                    continue
                if args.same_site and site_of(url) != root_site:
                    continue
                queued.add(k)
                queue.append((url, depth + 1, False))
    if not_followed:
        print("note: %d linked pages were not followed (limit --max-pages %d)"
              % (not_followed, args.max_pages))

    # ---- 2. name every page, and map addresses to names so links can be wired up --------------
    used = set()
    names = [unique_name(clean_name(args.name) if args.name and i == 0 else derive_name(p["src"]),
                         used)
             for i, p in enumerate(pages)]
    key_map = {}
    for p, name in zip(pages, names):
        key_map[norm_key(p["base"])] = name
        if is_url(p["src"]):
            key_map[norm_key(p["src"])] = name
        if p["alias"]:
            key_map[norm_key(p["alias"])] = name

    # ---- 3. convert --------------------------------------------------------------------------
    store = ImageStore(not args.no_images, max_w, max_h)
    written, total_bytes = set(), 0
    for p, name in zip(pages, names):
        src, html, base = p["src"], p["html"], p["base"]
        stats = {"ok": 0, "ext": 0}
        counter = [0]

        def image_cb(src_attr, w, h, maxw=None, base=base, name=name, counter=counter):
            src_attr = (src_attr or "").strip()
            if not store.enabled or not src_attr:
                return None
            url = urljoin(base, src_attr)
            key = (url, w, h, maxw)
            if key in store.cache:
                return store.cache[key]
            result = None
            try:
                content, iw, ih = store.to_sprite(fetch_bytes(url), w, h, maxw)
                counter[0] += 1
                if counter[0] > 99:
                    raise ValueError("more than 99 pictures on one page")
                iname = name[:5] + "I%02d" % counter[0]
                if iname in used or iname in store.sprites:
                    raise ValueError("AppVar name clash on " + iname)
                store.sprites[iname] = content
                result = (pad8(iname), iw, ih)
                print("  picture %s: %dx%d  <- %s" % (iname, iw, ih, src_attr[:60]))
            except Exception as e:  # noqa: BLE001 - a bad picture shouldn't stop the page
                print("  warning: picture skipped (%s): %s" % (src_attr[:60], e))
            store.cache[key] = result
            return result

        def video_cb(src_attr, label, base=base, name=name):
            url = urljoin(base, src_attr)
            if url in vstore.cache:
                return vstore.cache[url]
            vstore.cache[url] = None
            if args.videos == "no":
                return None
            why = vstore.problem(url)
            if why:
                print("  video skipped (%s): %s" % (why, src_attr[:60]))
                return None
            if args.videos == "ask":
                if not sys.stdin.isatty():
                    print("  video skipped (not a terminal, so I can't ask; use --videos yes): %s"
                          % src_attr[:60])
                    return None
                try:
                    ans = input('  Convert video "%s"? It becomes a silent clip of up to %d s, '
                                '%dx%d pixels drawn %dx as big, %d fps. [y/N] '
                                % (src_attr[:70], vstore.max_frames // vstore.fps or 1,
                                   vstore.w, vstore.h, vstore.scale, vstore.fps))
                except EOFError:
                    ans = ""
                if ans.strip().lower() not in ("y", "yes"):
                    print("  skipped.")
                    return None
            try:
                records, cut = vstore.convert(url)
                while True:  # a name no other file uses: PAGE-prefix + V + number
                    vstore.count += 1
                    if vstore.count > 99:
                        raise ValueError("more than 99 videos")
                    vbase = name[:3] + "V%02d" % vstore.count
                    if not any(n == vbase or n.startswith(vbase) and len(n) == len(vbase) + 2
                               for n in list(used) + list(store.sprites) + list(vstore.out)):
                        break
                made = vstore.chunks(
                    records, lambda k: [vbase] + [vbase + "%02d" % i for i in range(2, k + 1)])
                for cname, content in made:
                    vstore.out[cname] = content
                    used.add(cname)
            except Exception as e:  # noqa: BLE001 - a bad video shouldn't stop the page
                print("  warning: video skipped (%s): %s" % (src_attr[:60], " ".join(str(e).split())[:160]))
                return None
            kb = sum(len(c) for _, c in made) / 1024
            print("  video %s: %d frames (%.0f s) at %dx%d x%d, %d file%s, %.0f KB%s  <- %s"
                  % (vbase, len(records), len(records) / vstore.fps, vstore.w, vstore.h,
                     vstore.scale, len(made), "" if len(made) == 1 else "s", kb,
                     "  (cut short to fit --video-max-kb)" if cut else "", src_attr[:60]))
            vstore.cache[url] = (pad8(vbase), label)
            return vstore.cache[url]

        def resolve(href, base=base, stats=stats):
            full = urljoin(base, href)
            target = key_map.get(norm_key(full))
            if target:
                stats["ok"] += 1
                h = anchor_hash(urldefrag(full)[1])
                return pad8(target) + struct.pack("<H", h) if h else pad8(target)
            stats["ext"] += 1
            return NO_LINK

        conv = Converter(resolve, image_cb, rows=p["rows"], video_cb=video_cb)
        conv.feed(html)
        conv.close()
        conv.finish()
        title = conv.get_title() or name
        parts = paginate(conv.items, args.page_size)
        pnames = page_names(name, len(parts))

        print("%s -> %s  (%d page%s)" % (src[:60], name, len(parts), "" if len(parts) == 1 else "s"))
        for idx, (pn, tokens) in enumerate(zip(pnames, parts)):
            if pn in written:
                sys.exit("AppVar name clash on %s; use -n or rename your inputs" % pn)
            written.add(pn)
            content = build_page(
                tokens, title,
                pnames[idx - 1] if idx else None,
                pnames[idx + 1] if idx + 1 < len(pnames) else None,
                listed=args.list_all or p["main"])
            path = outdir / (pn + ".8xv")
            data = make_8xv(pn, content)
            path.write_bytes(data)
            total_bytes += len(data)
            if args.bin:
                (outdir / (pn + ".bin")).write_bytes(content)
            print("  wrote %s  (%d bytes of page data)" % (path, len(content)))
            if args.dump:
                print(dump_page(pn, content))
                print()
        if stats["ok"] or stats["ext"]:
            print("  links: %d point at converted pages, %d are not on the calculator"
                  % (stats["ok"], stats["ext"]))

    for iname, content in store.sprites.items():
        if iname in written:
            sys.exit("AppVar name clash on %s; use -n or rename your inputs" % iname)
        written.add(iname)
        path = outdir / (iname + ".8xv")
        data = make_8xv(iname, content)
        path.write_bytes(data)
        total_bytes += len(data)
        if args.bin:
            (outdir / (iname + ".bin")).write_bytes(content)
        print("wrote %s  (%dx%d sprite, %d bytes)" % (path, content[0], content[1], len(content)))

    for vname, content in vstore.out.items():
        if vname in written:
            sys.exit("AppVar name clash on %s; use -n or rename your inputs" % vname)
        written.add(vname)
        path = outdir / (vname + ".8xv")
        data = make_8xv(vname, content)
        path.write_bytes(data)
        total_bytes += len(data)
        if args.bin:
            (outdir / (vname + ".bin")).write_bytes(content)
        print("wrote %s  (video data, %d bytes)" % (path, len(content)))

    print("Done: %d page%s from %d starting page%s, %d files, %.0f KB to send."
          % (len(pages), "" if len(pages) == 1 else "s", len(args.inputs),
             "" if len(args.inputs) == 1 else "s", len(written), total_bytes / 1024))
    hidden = 0 if args.list_all else len(pages) - len(args.inputs)
    if hidden > 0:
        print("The menu lists the %d page%s you gave; %d linked page%s %s out of it "
              "(add --list-all to show them)."
              % (len(args.inputs), "" if len(args.inputs) == 1 else "s", hidden,
                 "" if hidden == 1 else "s", "stays" if hidden == 1 else "stay"))

if __name__ == "__main__":
    main()

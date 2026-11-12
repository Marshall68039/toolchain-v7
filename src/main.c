/*
 * HTMLRD - reader for pages converted with html2ce.py
 *
 * Menu:   Up/Down choose a page, Enter or 2nd opens it, Clear quits.
 *         Del deletes the chosen page with everything that belongs to it (all pages of a
 *         long document and its pictures), after asking for Enter to confirm.
 *         The menu shows the pages you gave html2ce.py; pages it pulled in through links
 *         are reached through the links (use --list-all there to list them too).
 * Reader: Up/Down scroll a line (hold to repeat); scrolling past the end or the start
 *         of a long document carries on into its next / previous page,
 *         2nd / Alpha page down / up,
 *         Left / Right select the previous / next link (an arrow shows beside it),
 *         Enter follows the selected link,
 *         Clear goes back to where you came from, and to the menu when there is no
 *         earlier page.
 *
 * Headings are drawn in three sizes, bold/italic/links get colours, images are
 * drawn from sprite AppVars made by html2ce.py. A video shows up in a page as a link
 * "[> video: ...]"; Enter on it opens the player:
 *         Enter / 2nd pause and play (again from the start after the end),
 *         Left / Right jump back / forward about two seconds, Clear stops.
 */

#include <fileioc.h>
#include <graphx.h>
#include <keypadc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <tice.h>

#include "core.h"
#include "video.h"

#define MARGIN_X 4
#define BAR_H    12
#define VIEW_TOP (BAR_H + 2)
#define VIEW_BOT (240 - BAR_H)           /* first row below the text area */
#define VIEW_H   (VIEW_BOT - VIEW_TOP)

#define LIST_H    10
#define LIST_ROWS ((240 - 2 * BAR_H - 4) / LIST_H)
#define MAX_FILES 48
#define HIST_CAP  8   /* pages you can go back through after following links */
#define MAX_CHAIN 40  /* pages of one long document searched for a link's anchor */
#define MAX_PAGES 99  /* pages in one long document */
#define SPR_CAP   48  /* pictures of one document that can be deleted along with it */
#define DEL_CAP   200 /* pages, pictures and video chunks of one document */

/* Palette: 0-215 a 6x6x6 colour cube (index = r*36 + g*6 + b, levels 0..5),
 * 216-255 forty greys ending in white. Must match html2ce.py. */
#define C_BLACK 0
#define C_WHITE 255
#define C_LINK  4   /* cube (0,0,4): dark blue  */
#define C_ITAL  108 /* cube (3,0,0): dark red   */
#define C_ARROW C_ITAL
#define C_DEAD  235 /* mid grey: link to a page that isn't on the calculator */

#define REPEAT_DELAY 12 /* frames before a held key starts repeating */
#define REPEAT_STEP  2

#ifndef gfx_RGBTo1555
#define gfx_RGBTo1555(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 7) | (((g) & 0xF8) << 2) | (((b) & 0xF8) >> 3) \
                | (((g) & 0x04) << 13)))
#endif

enum { K_UP = 1, K_DOWN = 2, K_LEFT = 4, K_RIGHT = 8,
       K_ENTER = 16, K_CLEAR = 32, K_2ND = 64, K_ALPHA = 128, K_DEL = 256 };
#define K_REPEAT (K_UP | K_DOWN | K_LEFT | K_RIGHT)

typedef struct {
    char name[9];
    char title[41];
} Entry;

typedef struct {
    char     name[9];
    uint16_t top;
    uint8_t  sel;
} Hist;

static Entry    entries[MAX_FILES];
static Hist     hist[HIST_CAP];
static uint8_t  nhist;
static Doc      doc;
static char     cur_name[9];
static uint16_t palette[256];

static uint16_t top, max_top;       /* first line shown, and the last useful first line */
static uint8_t  sel_link;           /* selected link number, 0 = none                   */
static uint16_t sel_y;              /* page y of the selected link's first line         */
static const char *doc_msg;         /* one-off message for the status bar               */

/* ---- input -------------------------------------------------------------------- */

static uint16_t poll_keys(void)
{
    uint16_t k = 0;

    kb_Scan();
    if (kb_Data[7] & kb_Up)    k |= K_UP;
    if (kb_Data[7] & kb_Down)  k |= K_DOWN;
    if (kb_Data[7] & kb_Left)  k |= K_LEFT;
    if (kb_Data[7] & kb_Right) k |= K_RIGHT;
    if (kb_Data[6] & kb_Enter) k |= K_ENTER;
    if (kb_Data[6] & kb_Clear) k |= K_CLEAR;
    if (kb_Data[1] & kb_2nd)   k |= K_2ND;
    if (kb_Data[1] & kb_Del)   k |= K_DEL;
    if (kb_Data[2] & kb_Alpha) k |= K_ALPHA;
    return k;
}

/* Keys that went down since last call, plus auto-repeat for the arrow keys. */
static uint16_t key_events(void)
{
    static uint16_t prev;
    static uint16_t hold;
    uint16_t now = poll_keys();
    uint16_t ev = now & (uint16_t)~prev;

    if ((now & K_REPEAT) && now == prev) {
        if (hold < 60000u)
            hold++;
        if (hold >= REPEAT_DELAY && (hold - REPEAT_DELAY) % REPEAT_STEP == 0)
            ev |= now & K_REPEAT;
    } else {
        hold = 0;
    }
    prev = now;
    return ev;
}

/* ---- files -------------------------------------------------------------------- */

/* Find every converted page that is the first page of its document. */
static uint8_t scan_pages(void)
{
    void   *pos = NULL;
    char   *name;
    uint8_t n = 0, h, i;

    while (n < MAX_FILES && (name = ti_Detect(&pos, "HR")) != NULL) {
        const uint8_t *p;
        uint16_t       size;

        h = ti_Open(name, "r");
        if (!h)
            continue;
        p = (const uint8_t *)ti_GetDataPtr(h);
        size = ti_GetSize(h);
        if (size >= 21 && p[2] >= 3 && p[2] <= 5 && p[20] <= 40
                && (p[2] == 3 || (p[3] & 1))) { /* version 3 pages are all listed */
            bool first = true;

            for (i = 4; i < 12; i++) /* skip continuation pages (they have a prev) */
                if (p[i])
                    first = false;
            if (first) {
                strncpy(entries[n].name, name, 8);
                entries[n].name[8] = 0;
                memcpy(entries[n].title, p + 21, p[20]);
                entries[n].title[p[20]] = 0;
                n++;
            }
        }
        ti_Close(h);
    }
    return n;
}

static bool load_page(const char *name)
{
    uint8_t h = ti_Open(name, "r");
    int     r;

    if (!h)
        return false;
    r = doc_layout(&doc, (const uint8_t *)ti_GetDataPtr(h), ti_GetSize(h));
    ti_Close(h);
    if (r != DOC_OK)
        return false;
    strncpy(cur_name, name, 8);
    cur_name[8] = 0;
    sel_link = 0;
    return true;
}

static uint16_t clamp_top(uint16_t t)
{
    return t > max_top ? max_top : t;
}

/* Open a page and start at its top (or its end). */
static bool open_page(const char *name, bool at_end)
{
    if (!load_page(name))
        return false;
    max_top = doc_max_top(&doc, VIEW_H);
    top = at_end ? max_top : 0;
    return true;
}

/* ---- video -------------------------------------------------------------------- */

static const uint8_t *vframes[VIDEO_FRAMES_MAX]; /* where each frame of the clip is */
static uint8_t        vbuf[2 + VIDEO_PIX_MAX];   /* one unpacked frame, as a sprite  */

static void draw_chrome(const char *top_text, const char *bottom);

static bool is_video(const char *name)
{
    const uint8_t *p;
    uint8_t        h = ti_Open(name, "r");
    bool           v;

    if (!h)
        return false;
    p = (const uint8_t *)ti_GetDataPtr(h);
    v = ti_GetSize(h) >= 2 && p[0] == 'H' && p[1] == 'V';
    ti_Close(h);
    return v;
}

/* Collect the frames of a clip (a chain of chunks). The frame data stays in the archive;
 * nothing is written while a clip plays, so the pointers stay good. */
static uint16_t index_video(const char *first, VideoChunk *head)
{
    VideoChunk     c;
    char           name[9];
    const uint8_t *p;
    uint16_t       size, n = 0;
    uint8_t        h, hops;

    strncpy(name, first, 8);
    name[8] = 0;
    for (hops = 0; name[0] && hops < 40; hops++) {
        h = ti_Open(name, "r");
        if (!h)
            break;
        p = (const uint8_t *)ti_GetDataPtr(h);
        size = ti_GetSize(h);
        if (!video_chunk(p, size, &c)) {
            ti_Close(h);
            break;
        }
        if (hops == 0)
            *head = c;
        n = video_index(p, size, &c, vframes, n, VIDEO_FRAMES_MAX);
        strcpy(name, c.next);
        ti_Close(h);
    }
    return n;
}

static void show_frame(const VideoChunk *v, uint16_t frame, uint16_t n, bool playing,
                       const char *title)
{
    char     status[48];
    uint16_t px = (uint16_t)(v->w * v->h), got;
    int      sw = v->w * v->scale, sh = v->h * v->scale;

    got = video_unrle(vframes[frame] + 2, video_frame_len(vframes[frame]), vbuf + 2, px);
    if (got < px) /* damaged frame: black for the rest */
        memset(vbuf + 2 + got, C_BLACK, px - got);
    gfx_FillScreen(C_BLACK);
    gfx_ScaledSprite_NoClip((gfx_sprite_t *)vbuf, (320 - sw) / 2,
                            BAR_H + (VIEW_BOT - BAR_H - sh) / 2, v->scale, v->scale);
    sprintf(status, "%u/%u %s <>:seek Clear:exit", (unsigned)(frame + 1), (unsigned)n,
            playing ? "Enter:pause" : frame + 1 >= n ? "Enter:replay" : "Enter:play");
    draw_chrome(title, status);
}

/* Play a clip. Returns NULL, or a message when it can't be played. */
static const char *play_video(const char *name)
{
    VideoChunk v;
    char       title[24];
    uint16_t   n, frame = 0, seek, t, step;
    uint16_t   ev;
    bool       playing = true, redraw = true;

    n = index_video(name, &v);
    if (!n)
        return "Could not play that video.";
    if (v.w * v.scale > 320 || v.h * v.scale > VIEW_BOT - BAR_H)
        return "Video is too big for the screen.";
    vbuf[0] = v.w;
    vbuf[1] = v.h;
    sprintf(title, "Video %s", name);
    seek = (uint16_t)(v.fps * 2 > 0 ? v.fps * 2 : 1);
    step = (uint16_t)(1000 / v.fps / 15); /* 15 ms ticks per frame; drawing time adds to it */
    if (step < 1)
        step = 1;
    for (;;) {
        if (redraw) {
            show_frame(&v, frame, n, playing, title);
            gfx_SwapDraw();
            redraw = false;
        }
        for (t = 0; t < step && !redraw; t++) {
            delay(15);
            ev = key_events();
            if (ev & K_CLEAR)
                return NULL;
            if (ev & (K_ENTER | K_2ND)) {
                if (!playing && frame + 1 >= n)
                    frame = 0; /* replay */
                playing = !playing;
                redraw = true;
            }
            if (ev & K_LEFT) {
                frame = frame > seek ? (uint16_t)(frame - seek) : 0;
                redraw = true;
            }
            if (ev & K_RIGHT) {
                frame = frame + seek < n ? (uint16_t)(frame + seek) : (uint16_t)(n - 1);
                redraw = true;
            }
        }
        if (!redraw && playing) {
            if (frame + 1 < n)
                frame++;
            else
                playing = false; /* end of the clip */
            redraw = true;
        }
    }
}

/* ---- links -------------------------------------------------------------------- */

static bool link_ok(uint8_t id)
{
    const LinkInfo *k = doc_link(&doc, id);

    return k && k->h;
}

static bool link_visible(const LinkInfo *k)
{
    uint16_t ty = doc_y(&doc, top);

    return k->y >= ty && k->y + k->h <= ty + VIEW_H;
}

/* The link after (dir = 1) or before (dir = -1) `from` in reading order, or 0. */
static uint8_t step_link(uint8_t from, int dir)
{
    int id = from;

    for (;;) {
        id += dir;
        if (id < 1 || id > doc.nlinks)
            return 0;
        if (link_ok((uint8_t)id))
            return (uint8_t)id;
    }
}

/* With nothing selected yet: the first (or last) link that is on screen, else the
 * first (or last) link of the page. */
static uint8_t start_link(int dir)
{
    uint8_t id, found = 0;

    for (id = 1; id <= doc.nlinks; id++)
        if (link_ok(id) && link_visible(doc_link(&doc, id))) {
            found = id;
            if (dir > 0)
                break;
        }
    if (found)
        return found;
    return dir > 0 ? step_link(0, 1) : step_link((uint8_t)(doc.nlinks + 1), -1);
}

/* Scroll just enough that the link is on screen. */
static void reveal_link(uint8_t id)
{
    const LinkInfo *k = doc_link(&doc, id);
    uint16_t        y;

    if (!k || link_visible(k))
        return;
    y = k->y > 3 * LINE_H ? (uint16_t)(k->y - 3 * LINE_H) : 0;
    top = clamp_top(doc_line_at(&doc, y));
}

static void hist_push(const char *name, uint16_t t, uint8_t s)
{
    if (nhist == HIST_CAP) {
        memmove(&hist[0], &hist[1], (HIST_CAP - 1) * sizeof hist[0]);
        nhist--;
    }
    strncpy(hist[nhist].name, name, 8);
    hist[nhist].name[8] = 0;
    hist[nhist].top = t;
    hist[nhist].sel = s;
    nhist++;
}

/* Open `name` and move to the spot an anchor marks. A long document is a chain of
 * pages, so the anchor may be a few pages on; if it isn't anywhere, show the top. */
static bool open_target(const char *name, uint16_t anchor)
{
    char     first[9], next[9];
    uint16_t line;
    uint8_t  hops;

    strncpy(first, name, 8);
    first[8] = 0;
    if (!open_page(first, false))
        return false;
    if (!anchor)
        return true;
    for (hops = 0; hops < MAX_CHAIN; hops++) {
        if (doc_find_anchor(&doc, anchor, &line)) {
            top = clamp_top(line);
            return true;
        }
        if (!doc.next[0])
            break;
        strcpy(next, doc.next);
        if (!load_page(next))
            break;
        max_top = doc_max_top(&doc, VIEW_H);
    }
    return open_page(first, false);
}

/* Enter on the selected link. */
static void follow_link(void)
{
    const LinkInfo *k = doc_link(&doc, sel_link);
    char     name[9], back[9];
    uint16_t anchor, back_top = top;
    uint8_t  back_sel = sel_link;

    if (!k)
        return;
    memcpy(name, k->name, 8); /* the link table is rebuilt by the next page */
    name[8] = 0;
    anchor = k->anchor;
    if (is_video(name)) { /* a clip, not a page: play it, then carry on where we were */
        doc_msg = play_video(name);
        return;
    }
    strcpy(back, cur_name);
    if (open_target(name, anchor)) {
        hist_push(back, back_top, back_sel);
    } else {
        open_page(back, false); /* put the page we were on back, whatever happened */
        top = clamp_top(back_top);
        sel_link = back_sel;
        doc_msg = "Could not open that page.";
    }
}

/* Clear: go back one page, or to the menu. Returns false when there's nothing to go back to. */
static bool go_back(void)
{
    Hist h;

    if (!nhist)
        return false;
    h = hist[--nhist];
    if (!open_page(h.name, false))
        return false;
    top = clamp_top(h.top);
    if (h.sel <= doc.nlinks)
        sel_link = h.sel;
    return true;
}

/* Scrolled off the end / start of a page of a long document: carry on. */
static void flow_page(bool forward)
{
    char name[9];

    strcpy(name, forward ? doc.next : doc.prev);
    if (!open_page(name, !forward))
        doc_msg = "Could not open that page.";
}

/* ---- drawing ------------------------------------------------------------------ */

static void init_palette(void)
{
    unsigned i, r, g, b, v;

    for (i = 0; i < 216; i++) {
        r = (i / 36) * 51;
        g = ((i / 6) % 6) * 51;
        b = (i % 6) * 51;
        palette[i] = gfx_RGBTo1555(r, g, b);
    }
    for (i = 216; i < 256; i++) {
        v = ((i - 216 + 1) * 255) / 40;
        palette[i] = gfx_RGBTo1555(v, v, v);
    }
    gfx_SetPalette(palette, sizeof palette, 0);
}

static void text_on_white(void)
{
    gfx_SetTextFGColor(C_BLACK);
    gfx_SetTextBGColor(C_WHITE);
    gfx_SetTextTransparentColor(C_WHITE);
}

static void text_on_black(void)
{
    gfx_SetTextFGColor(C_WHITE);
    gfx_SetTextBGColor(C_BLACK);
    gfx_SetTextTransparentColor(C_BLACK);
}

static void draw_chrome(const char *top_text, const char *bottom)
{
    gfx_SetColor(C_BLACK);
    gfx_FillRectangle(0, 0, 320, BAR_H);
    gfx_FillRectangle(0, VIEW_BOT, 320, BAR_H);
    text_on_black();
    gfx_SetTextScale(1, 1);
    gfx_SetTextXY(MARGIN_X, 2);
    gfx_PrintString(top_text);
    gfx_SetTextXY(MARGIN_X, VIEW_BOT + 2);
    gfx_PrintString(bottom);
}

/* One character cell, optionally double-struck for bold. */
static void put_char(int x, int y, char c, bool bold)
{
    gfx_SetTextXY(x, y);
    gfx_PrintChar(c);
    if (bold) {
        gfx_SetTextXY(x + 1, y);
        gfx_PrintChar(c);
    }
}

/* The selection arrow: a small right-pointing triangle, 3 wide and 5 tall, with its
 * top-left corner at (x, y). It sits in the margin to the left of the line. */
static void draw_arrow(int x, int y)
{
    static const uint8_t width[5] = {1, 2, 3, 2, 1};
    uint8_t i;

    gfx_SetColor(C_ARROW);
    for (i = 0; i < 5; i++)
        gfx_HorizLine(x, y + i, width[i]);
}

/* Draw one text line whose box starts at x0 and is `avail` pixels wide. The selected
 * link is drawn white on blue, and the line it starts on gets the arrow. */
static void draw_text_line(const LineView *v, int y, int x0, int avail)
{
    const uint8_t *p = v->p;
    uint8_t  nseg = *p++, s, len, st, head, k, lid;
    uint16_t width = 0;
    int      x, sx, sy, color;
    bool     bold, chosen, arrow = false;

    for (s = 0; s < nseg; s++) { /* first pass: total width, for centring */
        st = p[0];
        len = p[1];
        width += (uint16_t)(len * CHAR_W * head_sx(STYLE_HEAD(st)));
        p += 3 + len;
    }
    p = v->p + 1;
    x = x0 + v->indent;
    if (v->center && width < avail - v->indent)
        x += (avail - v->indent - width) / 2;

    for (s = 0; s < nseg; s++) {
        const uint8_t *text;
        int            xs = x;

        st = *p++;
        len = *p++;
        lid = *p++;
        text = p;
        p += len;
        head = STYLE_HEAD(st);
        sx = head_sx(head);
        sy = head_sy(head);
        bold = (st & S_BOLD) || head;
        chosen = sel_link && lid == sel_link;
        color = chosen ? C_WHITE
              : (st & S_LINK) ? C_LINK : (st & S_DEAD) ? C_DEAD
              : (st & S_ITAL) ? C_ITAL : C_BLACK;

        if (chosen) {
            gfx_SetColor(C_LINK);
            gfx_FillRectangle(xs, y, len * CHAR_W * sx, v->height);
            if (!arrow && v->y == sel_y) { /* only on the line the link starts on */
                int ax = x0 + v->indent - 4;

                draw_arrow(ax < 0 ? 0 : ax,
                           y + v->height - 1 - 8 * sy + (8 * sy - 5) / 2);
                arrow = true;
            }
        }
        if (chosen) { /* white letters on the blue box: blue is the see-through colour */
            gfx_SetTextBGColor(C_LINK);
            gfx_SetTextTransparentColor(C_LINK);
        }
        gfx_SetTextFGColor(color);
        gfx_SetTextScale(sx, sy);
        for (k = 0; k < len; k++, x += CHAR_W * sx)
            if (text[k] != ' ')
                put_char(x, y + v->height - 1 - 8 * sy, (char)text[k], bold);
        if (chosen) {
            gfx_SetTextBGColor(C_WHITE);
            gfx_SetTextTransparentColor(C_WHITE);
        }
        if (st & (S_LINK | S_DEAD)) { /* underline */
            gfx_SetColor(color);
            gfx_HorizLine(xs, y + v->height - 1, x - xs);
        }
    }
    gfx_SetTextScale(1, 1);
    gfx_SetTextFGColor(C_BLACK);
}

static void draw_image(const LineView *v, int y, int x0, int avail)
{
    char    name[9];
    int     x = x0 + v->indent, w = v->p[8], h = v->p[9];
    uint8_t slot;

    memcpy(name, v->p, 8);
    name[8] = 0;
    if (v->center && w < avail - v->indent)
        x += (avail - v->indent - w) / 2;
    slot = ti_Open(name, "r");
    if (slot) {
        gfx_Sprite((gfx_sprite_t *)ti_GetDataPtr(slot), x, y + 2);
        ti_Close(slot);
    } else { /* sprite AppVar missing: outline where it should be */
        gfx_SetColor(C_DEAD);
        gfx_Rectangle(x, y + 2, w, h);
    }
}

/* Draw a text, rule or image line (not a row) at screen row y. Lines that would
 * poke out of the bottom are skipped, except images, which the bottom bar covers. */
static void draw_line(const LineView *v, int y, int x0, int avail)
{
    if (v->kind == LK_IMAGE) {
        draw_image(v, y, x0, avail);
    } else if (y + v->height <= VIEW_BOT) {
        if (v->kind == LK_TEXT) {
            draw_text_line(v, y, x0, avail);
        } else if (v->kind == LK_RULE) {
            gfx_SetColor(C_DEAD);
            gfx_HorizLine(x0 + v->indent, y + 3, avail - v->indent);
        }
    }
}

static void draw_doc(void)
{
    char            status[56];
    const LinkInfo *sl = doc_link(&doc, sel_link);
    LineView        v, t, sub;
    uint16_t        i, cx, cw;
    uint8_t         c, k, cnt;
    int             y;

    sel_y = sl ? sl->y : 0;
    gfx_FillScreen(C_WHITE);
    text_on_white();
    if (doc.nlines == 0) {
        gfx_SetTextXY(MARGIN_X, VIEW_TOP);
        gfx_PrintString("(empty page)");
    } else {
        doc_line(&doc, top, &t);
        for (i = top; i < doc.nlines; i++) {
            doc_line(&doc, i, &v);
            y = VIEW_TOP + (int)(v.y - t.y);
            if (y >= VIEW_BOT)
                break;
            if (v.kind == LK_ROW) { /* side-by-side columns: each holds a stack of lines */
                for (c = 0; c < doc_row_cols(&v); c++) {
                    if (!doc_row_cell(&doc, &v, c, &sub, &cnt, &cx, &cw))
                        continue;
                    for (k = 0; k < cnt; k++) {
                        int sy = VIEW_TOP + (int)(sub.y - t.y);

                        if (sy < VIEW_BOT)
                            draw_line(&sub, sy, MARGIN_X + cx, cw);
                        if (k + 1 < cnt)
                            doc_cell_next(&sub);
                    }
                }
            } else {
                draw_line(&v, y, MARGIN_X, VIEW_W);
            }
        }
    }
    sprintf(status, "%s %u/%u%s%s%s", cur_name, (unsigned)(top + 1), (unsigned)doc.nlines,
            doc.prev[0] ? " <" : "", doc.next[0] ? " >" : "", doc.truncated ? " (cut)" : "");
    if (sl)
        sprintf(status + strlen(status), " #%u/%u", (unsigned)sel_link, (unsigned)doc.nlinks);
    draw_chrome(doc.title[0] ? doc.title : cur_name, doc_msg ? doc_msg : status);
}

static bool asking; /* waiting for Enter to confirm a delete */

static void draw_list(uint8_t count, uint8_t sel, uint8_t top_row, const char *msg)
{
    char    row[41];
    uint8_t i, k;
    const char *s;

    gfx_FillScreen(C_WHITE);
    text_on_white();
    if (!count) {
        gfx_PrintStringXY("No pages found.", MARGIN_X, VIEW_TOP);
        gfx_PrintStringXY("Convert HTML with html2ce.py,", MARGIN_X, VIEW_TOP + 2 * LINE_H);
        gfx_PrintStringXY("then send the .8xv files to", MARGIN_X, VIEW_TOP + 3 * LINE_H);
        gfx_PrintStringXY("the calculator.", MARGIN_X, VIEW_TOP + 4 * LINE_H);
    }
    for (i = 0; i < LIST_ROWS && top_row + i < count; i++) {
        const Entry *e = &entries[top_row + i];
        int y = BAR_H + 2 + i * LIST_H;

        k = 0;
        for (s = e->name; *s && k < 8; s++)
            row[k++] = *s;
        while (k < 9)
            row[k++] = ' ';
        for (s = e->title; *s && k < 39; s++)
            row[k++] = *s;
        row[k] = 0;
        if (top_row + i == sel) {
            gfx_SetColor(C_BLACK);
            gfx_FillRectangle(0, y - 1, 320, LIST_H);
            text_on_black();
        } else {
            text_on_white();
        }
        gfx_PrintStringXY(row, MARGIN_X, y);
    }
    if (msg) {
        text_on_white();
        gfx_PrintStringXY(msg, MARGIN_X, VIEW_BOT - LINE_H - 2);
    }
    draw_chrome("HTML Reader",
                asking ? "Enter: delete   Clear: cancel"
                       : count ? "Enter: open  Del: delete  Clear: quit" : "Clear: quit");
}

/* ---- deleting ----------------------------------------------------------------- */

static char    del_name[DEL_CAP][9]; /* everything that goes with the chosen document */
static uint8_t del_n;

static bool in_del(const char *name)
{
    uint8_t i;

    for (i = 0; i < del_n; i++)
        if (!strncmp(del_name[i], name, 8))
            return true;
    return false;
}

/* List every file that belongs to the document whose first page is `first`: all of its
 * pages, and the pictures and videos that no page of any other document still uses. */
static void prepare_delete(const char *first)
{
    static uint8_t spr[SPR_CAP][8];
    void          *pos = NULL;
    char          *nm, name[9];
    const uint8_t *p;
    uint16_t       size;
    uint8_t        h, i, nspr = 0;

    del_n = 0;
    strncpy(name, first, 8);
    name[8] = 0;
    while (name[0] && del_n < MAX_PAGES && !in_del(name)) { /* the chain of pages */
        h = ti_Open(name, "r");
        if (!h)
            break;
        p = (const uint8_t *)ti_GetDataPtr(h);
        size = ti_GetSize(h);
        nspr = doc_sprites(p, size, spr, nspr, SPR_CAP);
        strcpy(del_name[del_n++], name);
        if (size >= 21) { /* name of the next page */
            memcpy(name, p + 12, 8);
            name[8] = 0;
        } else {
            name[0] = 0;
        }
        ti_Close(h);
    }
    pos = NULL;
    while (nspr && (nm = ti_Detect(&pos, "HR")) != NULL) { /* sprites other pages share stay */
        if (in_del(nm))
            continue;
        h = ti_Open(nm, "r");
        if (!h)
            continue;
        p = (const uint8_t *)ti_GetDataPtr(h);
        size = ti_GetSize(h);
        for (i = 0; i < nspr;) {
            if (doc_uses_sprite(p, size, spr[i]))
                memcpy(spr[i], spr[--nspr], 8);
            else
                i++;
        }
        ti_Close(h);
    }
    for (i = 0; i < nspr; i++) { /* pictures and videos; a video is a chain of chunks */
        memcpy(name, spr[i], 8);
        name[8] = 0;
        while (name[0] && del_n < DEL_CAP && !in_del(name)) {
            h = ti_Open(name, "r"); /* only count what exists */
            if (!h)
                break;
            p = (const uint8_t *)ti_GetDataPtr(h);
            size = ti_GetSize(h);
            strcpy(del_name[del_n++], name);
            if (size >= VIDEO_HEADER && p[0] == 'H' && p[1] == 'V') {
                memcpy(name, p + 4, 8); /* next chunk */
                name[8] = 0;
            } else {
                name[0] = 0;
            }
            ti_Close(h);
        }
    }
}

static void do_delete(void)
{
    uint8_t i;

    for (i = 0; i < del_n; i++)
        ti_Delete(del_name[i]);
    del_n = 0;
}

/* ---- main --------------------------------------------------------------------- */

int main(void)
{
    enum { S_LIST, S_DOC } state = S_LIST;
    uint8_t     count, sel = 0, top_row = 0, id;
    uint16_t    t, ev;
    bool        dirty = true;
    const char *msg = NULL;
    static char del_msg[48];

    count = scan_pages();

    gfx_Begin();
    gfx_SetDrawBuffer();
    init_palette();
    gfx_SetMonospaceFont(8);

    for (;;) {
        ev = key_events();

        if (state == S_LIST && asking) { /* "delete this document?" */
            if (ev) {
                if (ev & K_ENTER) {
                    uint8_t before = count;

                    do_delete();
                    count = scan_pages();
                    if (sel >= count)
                        sel = count ? count - 1 : 0;
                    if (top_row > sel)
                        top_row = sel;
                    msg = count < before ? "Deleted." : "Could not delete.";
                } else {
                    msg = NULL;
                }
                asking = false;
                dirty = true;
            }
            if (dirty)
                draw_list(count, sel, top_row, msg);
        } else if (state == S_LIST) {
            if (ev && msg) { /* a message shows until the next key */
                msg = NULL;
                dirty = true;
            }
            if (ev & K_CLEAR)
                break;
            if (count && (ev & K_DEL)) {
                prepare_delete(entries[sel].name);
                sprintf(del_msg, "Delete %s (%u files)?", entries[sel].name, (unsigned)del_n);
                msg = del_msg;
                asking = true;
                dirty = true;
            }
            if (count) {
                if ((ev & K_DOWN) && sel + 1 < count) { sel++; dirty = true; }
                if ((ev & K_UP) && sel > 0)           { sel--; dirty = true; }
                if (sel < top_row)
                    top_row = sel;
                if (sel >= top_row + LIST_ROWS)
                    top_row = sel - LIST_ROWS + 1;
                if (!asking && (ev & (K_ENTER | K_2ND))) {
                    if (open_page(entries[sel].name, false)) {
                        state = S_DOC;
                        nhist = 0;
                        doc_msg = NULL;
                        msg = NULL;
                    } else {
                        msg = "Could not open that page.";
                    }
                    dirty = true;
                }
            }
            if (dirty)
                draw_list(count, sel, top_row, msg);
        } else {
            if (ev && doc_msg) { /* a message shows until the next key */
                doc_msg = NULL;
                dirty = true;
            }
            if (ev & K_DOWN) {
                if (top < max_top)
                    top++;
                else if (doc.next[0])
                    flow_page(true);
                dirty = true;
            }
            if (ev & K_UP) {
                if (top > 0)
                    top--;
                else if (doc.prev[0])
                    flow_page(false);
                dirty = true;
            }
            if (ev & K_2ND) {                    /* page down, keeping one line of overlap */
                if (top >= max_top && doc.next[0]) {
                    flow_page(true);
                } else {
                    t = clamp_top(doc_line_at(&doc, doc_y(&doc, top) + VIEW_H - LINE_H));
                    top = (t == top) ? clamp_top(top + 1) : t;
                }
                dirty = true;
            }
            if (ev & K_ALPHA) {                  /* page up */
                if (top == 0 && doc.prev[0]) {
                    flow_page(false);
                } else {
                    uint16_t y = doc_y(&doc, top);

                    t = doc_line_at(&doc, y > VIEW_H - LINE_H ? y - (VIEW_H - LINE_H) : 0);
                    top = (t >= top && top > 0) ? top - 1 : t;
                }
                dirty = true;
            }
            if ((ev & (K_LEFT | K_RIGHT)) && !((ev & K_LEFT) && (ev & K_RIGHT))) {
                int dir = (ev & K_RIGHT) ? 1 : -1;

                if (!doc.nlinks)
                    doc_msg = "No links on this page.";
                else if ((id = sel_link ? step_link(sel_link, dir) : start_link(dir)) != 0) {
                    sel_link = id;
                    reveal_link(id);
                }
                dirty = true;
            }
            if (ev & K_ENTER) {
                const LinkInfo *k = doc_link(&doc, sel_link);

                if (!k)
                    doc_msg = doc.nlinks ? "Left/Right: pick a link." : "No links on this page.";
                else if (!link_visible(k))
                    reveal_link(sel_link); /* bring it on screen first, then Enter again */
                else
                    follow_link();
                dirty = true;
            }
            if (ev & K_CLEAR) {
                if (!go_back())
                    state = S_LIST;
                doc_msg = NULL;
                dirty = true;
            }
            if (dirty) {
                if (state == S_DOC)
                    draw_doc();
                else
                    draw_list(count, sel, top_row, msg);
            }
        }

        if (dirty) {
            gfx_SwapDraw();
            dirty = false;
        }
        delay(15);
    }

    gfx_End();
    return 0;
}

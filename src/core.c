#include "core.h"

#include <string.h>

/* Token opcodes - must match html2ce.py */
enum {
    T_END, T_HEAD, T_PARA, T_BR, T_CENTER, T_LINK, T_LINK_END,
    T_BOLD, T_ITALIC, T_IMAGE, T_INDENT, T_PRE, T_RULE,
    T_COLS, T_COL, T_COLS_END, T_LINKA, T_ANCHOR, T_VIDEO
};

#define FORMAT_MIN     3
#define FORMAT_MAX     5
#define MAXC           48    /* more than VIEW_W / CHAR_W, so a line always fits */
#define Y_LIMIT        60000u
#define NO_OFF         0xFFFFu
#define MIN_COL_W      32    /* narrower columns than this fall back to equal widths */

typedef struct {
    uint16_t off; /* where the line starts in arena[] */
    uint16_t y;   /* pixel position of the line       */
} LineRec;

typedef struct {
    uint16_t x, w; /* column position and width inside the text area */
} ColGeom;

typedef struct {
    uint16_t hash;
    uint16_t line;
} AnchorRec;

/* Each line: [flags: bit0 centre, bits1-2 kind][indent px][height px][payload] */
static uint8_t   arena[ARENA_CAP];
static LineRec   recs[LINE_CAP];
static ColGeom   blocks[BLOCK_CAP][MAX_COLS];
static uint16_t  colrec[MAX_COLS][COLLINE_CAP]; /* arena offsets of a block's column lines */
static LinkInfo  links[LINK_CAP];
static AnchorRec anchors[ANCHOR_CAP];

typedef struct {
    Doc     *doc;
    uint16_t arena_len, nlines, y;
    uint16_t width;      /* width available to text: the column's, or VIEW_W */

    char     cur[MAXC];  /* line being built, one style byte per char */
    uint8_t  cst[MAXC];
    uint8_t  clid[MAXC]; /* and the link number of each char (0 = none) */
    uint8_t  clen;
    uint16_t cpx;        /* pixel width of cur                        */
    bool     has_text;
    bool     line_center;
    uint8_t  line_ind;

    char     word[MAXC]; /* word being collected                      */
    uint8_t  wst[MAXC];
    uint8_t  wlid[MAXC];
    uint8_t  wlen;
    uint16_t wpx;

    uint8_t  ind;        /* current indent in pixels                  */
    uint8_t  head;       /* current heading size 0-3                  */
    uint8_t  link;       /* 0 none, 1 live, 2 dead                    */
    uint8_t  link_id;    /* number of the live link we're inside      */
    uint8_t  nlinks;
    uint8_t  nanchors;
    bool     bold, ital, center, pre;
    bool     last_blank, full;

    bool     in_block;   /* collecting side-by-side columns           */
    uint8_t  ignored;    /* nesting depth of column markers we ignore */
    uint8_t  nblocks;
    uint16_t blk_line;   /* page line where the open block's rows begin */
    struct {
        uint8_t  n, cur, geom;
        uint16_t count[MAX_COLS];
    } blk;
} Layout;

/* ---- line storage ---------------------------------------------------------------- */

/* Reserve room for a line; returns where to write it, or NULL when full.
 * Inside a column block the line goes to that column's list instead of the page. */
static uint8_t *reserve(Layout *l, uint32_t bytes, uint8_t height)
{
    uint8_t *p;

    if (l->full)
        return NULL;
    if (l->in_block) {
        if (l->blk.count[l->blk.cur] >= COLLINE_CAP) {
            l->doc->truncated = true;
            return NULL;
        }
        if ((uint32_t)l->arena_len + bytes > ARENA_CAP) {
            l->full = true;
            l->doc->truncated = true;
            return NULL;
        }
        colrec[l->blk.cur][l->blk.count[l->blk.cur]++] = l->arena_len;
        p = arena + l->arena_len;
        l->arena_len += (uint16_t)bytes;
        return p;
    }
    if (l->nlines >= LINE_CAP || (uint32_t)l->arena_len + bytes > ARENA_CAP
            || (uint32_t)l->y + height > Y_LIMIT) {
        l->full = true;
        l->doc->truncated = true;
        return NULL;
    }
    recs[l->nlines].off = l->arena_len;
    recs[l->nlines].y = l->y;
    l->nlines++;
    l->y += height;
    p = arena + l->arena_len;
    l->arena_len += (uint16_t)bytes;
    return p;
}

static uint8_t cur_style(const Layout *l)
{
    return (uint8_t)((l->bold ? S_BOLD : 0) | (l->ital ? S_ITAL : 0)
                     | (l->head << 2)
                     | (l->link == 1 ? S_LINK : 0) | (l->link == 2 ? S_DEAD : 0));
}

static void commit_text(Layout *l)
{
    uint8_t  n = l->clen, i, j, nseg = 0, h = LINE_H, st, lid;
    uint32_t size = 4; /* header (3) + segment count (1) */
    uint8_t *p;

    while (n && l->cur[n - 1] == ' ')
        n--;
    for (i = 0; i < n; i++) {
        uint8_t hh = (uint8_t)(8 * head_sy(STYLE_HEAD(l->cst[i])) + 1);

        if (hh > h)
            h = hh;
        if (i == 0 || l->cst[i] != l->cst[i - 1] || l->clid[i] != l->clid[i - 1]) {
            nseg++;
            size += 3; /* style, length, link id */
        }
    }
    size += n;
    p = reserve(l, size, h);
    if (!p)
        return;
    *p++ = (uint8_t)((LK_TEXT << 1) | (n && l->line_center ? 1 : 0));
    *p++ = n ? l->line_ind : 0; /* an empty line has no indent */
    *p++ = h;
    *p++ = nseg;
    for (i = 0; i < n; i = j) {
        st = l->cst[i];
        lid = l->clid[i];
        for (j = i; j < n && l->cst[j] == st && l->clid[j] == lid; j++)
            ;
        *p++ = st;
        *p++ = (uint8_t)(j - i);
        *p++ = lid;
        memcpy(p, l->cur + i, j - i);
        p += j - i;
    }
    l->last_blank = (n == 0);
}

static void start_line(Layout *l)
{
    l->clen = 0;
    l->cpx = 0;
    l->has_text = false;
}

static void newline(Layout *l)
{
    commit_text(l);
    start_line(l);
}

static void emit_blank(Layout *l)
{
    uint8_t *p = reserve(l, 4, LINE_H);

    if (!p)
        return;
    p[0] = (uint8_t)(LK_TEXT << 1);
    p[1] = 0;
    p[2] = LINE_H;
    p[3] = 0;
    l->last_blank = true;
}

/* ---- words ----------------------------------------------------------------------- */

static uint16_t avail_px(const Layout *l)
{
    return (uint16_t)(l->width > (unsigned)(l->ind + 16) ? l->width - l->ind : 16);
}

static void flush_word(Layout *l)
{
    uint16_t avail = avail_px(l);
    uint16_t spx = 0;
    uint8_t  sst = 0, slid = 0;

    if (!l->wlen)
        return;
    if (l->has_text) {
        /* the space between two words: wide as the next word's size, and it
         * keeps bold/italic/link only when both neighbours have it (and a link
         * only when both are the same link) */
        sst = (uint8_t)((l->cst[l->clen - 1] & l->wst[0] & ~S_HEAD_MASK)
                        | (l->wst[0] & S_HEAD_MASK));
        if (sst & S_LINK) {
            if (l->clid[l->clen - 1] == l->wlid[0])
                slid = l->wlid[0];
            else
                sst &= (uint8_t)~S_LINK;
        }
        spx = (uint16_t)(CHAR_W * head_sx(STYLE_HEAD(sst)));
        if (l->cpx + spx + l->wpx > avail) {
            newline(l);
            spx = 0;
        }
    }
    if (!l->has_text) {
        l->line_center = l->center;
        l->line_ind = l->ind;
    }
    if (spx) {
        l->cur[l->clen] = ' ';
        l->cst[l->clen] = sst;
        l->clid[l->clen] = slid;
        l->clen++;
        l->cpx += spx;
    }
    memcpy(l->cur + l->clen, l->word, l->wlen);
    memcpy(l->cst + l->clen, l->wst, l->wlen);
    memcpy(l->clid + l->clen, l->wlid, l->wlen);
    l->clen += l->wlen;
    l->cpx += l->wpx;
    l->has_text = true;
    l->wlen = 0;
    l->wpx = 0;
}

static void add_char(Layout *l, char c)
{
    uint8_t  st = cur_style(l);
    uint8_t  lid = (uint8_t)(l->link == 1 ? l->link_id : 0);
    uint16_t cw = (uint16_t)(CHAR_W * head_sx(STYLE_HEAD(st)));
    uint16_t avail = avail_px(l);

    if (l->pre) { /* keep every space, wrap hard at the edge */
        if (l->cpx + cw > avail || l->clen >= MAXC)
            newline(l);
        if (!l->has_text) {
            l->line_center = l->center;
            l->line_ind = l->ind;
        }
        l->cur[l->clen] = c;
        l->cst[l->clen] = st;
        l->clid[l->clen] = lid;
        l->clen++;
        l->cpx += cw;
        l->has_text = true;
        return;
    }
    if (c == ' ') {
        flush_word(l);
        return;
    }
    if (l->wpx + cw > avail || l->wlen >= MAXC - 1) { /* one word wider than a line */
        flush_word(l);
        newline(l);
    }
    l->word[l->wlen] = c;
    l->wst[l->wlen] = st;
    l->wlid[l->wlen] = lid;
    l->wlen++;
    l->wpx += cw;
}

static void add_str(Layout *l, const char *s)
{
    while (*s)
        add_char(l, *s++);
}

/* ---- links and anchors ------------------------------------------------------------ */

static bool nonzero8(const uint8_t *p)
{
    uint8_t i;

    for (i = 0; i < 8; i++)
        if (p[i])
            return true;
    return false;
}

/* A link starts. Live links (target on the calculator) are numbered in reading order. */
static void begin_link(Layout *l, const uint8_t *name, uint16_t anchor)
{
    if (!nonzero8(name) || l->nlinks >= LINK_CAP) {
        l->link = 2;
        l->link_id = 0;
        return;
    }
    memcpy(links[l->nlinks].name, name, 8);
    links[l->nlinks].anchor = anchor;
    links[l->nlinks].y = 0;
    links[l->nlinks].h = 0;
    l->link = 1;
    l->link_id = ++l->nlinks;
}

static void end_link(Layout *l)
{
    l->link = 0;
    l->link_id = 0;
}

static void add_anchor(Layout *l, uint16_t hash)
{
    if (l->nanchors >= ANCHOR_CAP)
        return;
    anchors[l->nanchors].hash = hash;
    /* inside side-by-side columns the anchor points at the first row of the block */
    anchors[l->nanchors].line = l->in_block ? l->blk_line : l->nlines;
    l->nanchors++;
}

/* ---- breaks and blocks ----------------------------------------------------------- */

static void end_line(Layout *l) /* <br> */
{
    flush_word(l);
    if (l->nlines == 0 && !l->in_block && !l->has_text) { /* ignore leading blank lines */
        start_line(l);
        return;
    }
    newline(l);
}

static void end_para(Layout *l) /* finish the line, leave a blank one, end any heading */
{
    flush_word(l);
    if (l->has_text)
        newline(l);
    if ((l->nlines > 0 || l->in_block) && !l->last_blank)
        emit_blank(l);
    l->head = 0;
}

static void rule(Layout *l)
{
    uint8_t *p;

    flush_word(l);
    if (l->has_text)
        newline(l);
    p = reserve(l, 3, 7);
    if (!p)
        return;
    p[0] = (uint8_t)(LK_RULE << 1);
    p[1] = l->ind;
    p[2] = 7;
    l->last_blank = false;
}

static void image(Layout *l, const uint8_t *name, uint8_t w, uint8_t h)
{
    uint8_t *p;
    uint16_t hh = (uint16_t)(h + 4);

    flush_word(l);
    if (l->has_text)
        newline(l);
    p = reserve(l, 13, hh > 255 ? 255 : (uint8_t)hh);
    if (!p)
        return;
    p[0] = (uint8_t)((LK_IMAGE << 1) | (l->center ? 1 : 0));
    p[1] = l->ind;
    p[2] = hh > 255 ? 255 : (uint8_t)hh;
    memcpy(p + 3, name, 8);
    p[11] = w;
    p[12] = h;
    l->last_blank = false;
}

/* ---- side-by-side columns -------------------------------------------------------- */

static void reset_style(Layout *l)
{
    l->bold = l->ital = l->center = l->pre = false;
    l->head = 0;
    l->link = 0;
    l->link_id = 0;
    l->ind = 0;
}

/* Column positions: same formula as col_geometry() in html2ce.py. */
static void layout_cols(ColGeom *g, uint8_t n, const uint8_t *weights)
{
    uint32_t avail = VIEW_W - COL_GUTTER * (n - 1u), tot = 0, used = 0;
    uint8_t  i;
    uint16_t x = 0;

    for (i = 0; i < n; i++)
        tot += weights[i];
    if (!tot)
        tot = 1;
    for (i = 0; i < n; i++) {
        g[i].w = (uint16_t)(avail * weights[i] / tot);
        used += g[i].w;
    }
    g[n - 1].w = (uint16_t)(g[n - 1].w + (avail - used)); /* last column takes the remainder */
    for (i = 0; i < n; i++)
        if (g[i].w < MIN_COL_W) { /* absurd weights: share the space equally */
            for (i = 0; i < n; i++)
                g[i].w = (uint16_t)(avail / n);
            g[n - 1].w = (uint16_t)(g[n - 1].w + (avail - (avail / n) * n));
            break;
        }
    for (i = 0; i < n; i++) {
        g[i].x = x;
        x = (uint16_t)(x + g[i].w + COL_GUTTER);
    }
}

static void begin_cols(Layout *l, uint8_t n, const uint8_t *weights)
{
    if (l->in_block || l->ignored || n < 2 || n > MAX_COLS || l->nblocks >= BLOCK_CAP) {
        l->ignored++; /* nested or too many: let the content flow in one column */
        return;
    }
    flush_word(l);
    if (l->has_text)
        newline(l);
    reset_style(l);
    layout_cols(blocks[l->nblocks], n, weights);
    memset(&l->blk, 0, sizeof l->blk);
    l->blk.n = n;
    l->blk.geom = l->nblocks++;
    l->blk_line = l->nlines;
    l->in_block = true;
    l->width = blocks[l->blk.geom][0].w;
    l->last_blank = false;
    start_line(l);
}

static void next_col(Layout *l)
{
    if (!l->in_block || l->ignored) { /* stray separator: just start a new paragraph */
        end_para(l);
        return;
    }
    flush_word(l);
    if (l->has_text)
        newline(l);
    if (l->blk.cur + 1 < l->blk.n)
        l->blk.cur++;
    l->width = blocks[l->blk.geom][l->blk.cur].w;
    reset_style(l);
    l->last_blank = false;
    start_line(l);
}

static uint8_t col_line_h(uint8_t c, uint16_t i)
{
    return arena[colrec[c][i] + 2];
}

/* Join the columns into rows. Each row takes the next line of every column; the
 * tallest of those sets the row's height, and shorter columns pack in further lines
 * (while at least half of the next line fits) so their text stays tight. */
static void end_cols(Layout *l)
{
    uint16_t idx[MAX_COLS] = {0, 0, 0}, start[MAX_COLS], sum[MAX_COLS], rowh, tall;
    uint8_t  cnt[MAX_COLS], c, h, n = l->blk.n, g = l->blk.geom, *p;
    bool     more;

    flush_word(l);
    if (l->has_text)
        newline(l);
    l->in_block = false;
    for (;;) {
        more = false;
        tall = 0;
        for (c = 0; c < n; c++)
            if (idx[c] < l->blk.count[c]) {
                more = true;
                if (col_line_h(c, idx[c]) > tall)
                    tall = col_line_h(c, idx[c]);
            }
        if (!more)
            break;
        rowh = 0;
        for (c = 0; c < n; c++) {
            start[c] = idx[c];
            cnt[c] = 0;
            sum[c] = 0;
            while (idx[c] < l->blk.count[c]) {
                h = col_line_h(c, idx[c]);
                if (cnt[c] && (sum[c] + h / 2 > tall || sum[c] + h > 255))
                    break;
                sum[c] = (uint16_t)(sum[c] + h);
                cnt[c]++;
                idx[c]++;
            }
            if (sum[c] > rowh)
                rowh = sum[c];
        }
        p = reserve(l, 5u + 3u * n, (uint8_t)rowh);
        if (!p)
            break;
        p[0] = (uint8_t)(LK_ROW << 1);
        p[1] = 0;
        p[2] = (uint8_t)rowh;
        p[3] = g;
        p[4] = n;
        for (c = 0; c < n; c++) {
            uint16_t off = cnt[c] ? colrec[c][start[c]] : NO_OFF;

            p[5 + 3 * c] = (uint8_t)(off & 0xFF);
            p[6 + 3 * c] = (uint8_t)(off >> 8);
            p[7 + 3 * c] = cnt[c];
        }
    }
    l->width = VIEW_W;
    reset_style(l);
    l->last_blank = false;
    start_line(l);
}

static void cols_end(Layout *l)
{
    if (l->ignored)
        l->ignored--;
    else if (l->in_block)
        end_cols(l);
}

/* ---- public API ------------------------------------------------------------------ */

static void copy_name(char *dst, const uint8_t *src)
{
    uint8_t i;

    for (i = 0; i < 8 && src[i]; i++)
        dst[i] = (char)src[i];
    dst[i] = 0;
}

static void fill_view(const uint8_t *p, uint16_t y, LineView *v)
{
    v->y = y;
    v->center = p[0] & 1;
    v->kind = (p[0] >> 1) & 3;
    v->indent = p[1];
    v->height = p[2];
    v->p = p + 3;
}

bool doc_line(const Doc *d, uint16_t i, LineView *v)
{
    if (i >= d->nlines)
        return false;
    fill_view(arena + recs[i].off, recs[i].y, v);
    return true;
}

uint8_t doc_row_cols(const LineView *row)
{
    return row->p[1];
}

bool doc_row_cell(const Doc *d, const LineView *row, uint8_t col, LineView *first,
                  uint8_t *count, uint16_t *x, uint16_t *w)
{
    uint8_t        g = row->p[0], n = row->p[1];
    const uint8_t *q;
    uint16_t       off;

    (void)d;
    if (col >= n || g >= BLOCK_CAP)
        return false;
    q = row->p + 2 + 3 * col;
    off = (uint16_t)(q[0] | (q[1] << 8));
    *x = blocks[g][col].x;
    *w = blocks[g][col].w;
    *count = q[2];
    if (!q[2] || off == NO_OFF)
        return false;
    fill_view(arena + off, row->y, first);
    return true;
}

/* Size in bytes of a line's payload (what follows its 3-byte header). */
static uint16_t payload_size(const LineView *v)
{
    const uint8_t *q;
    uint8_t        s;

    switch (v->kind) {
    case LK_TEXT: /* [segment count] then [style][length][link id][chars] per segment */
        q = v->p + 1;
        for (s = 0; s < v->p[0]; s++)
            q += 3 + q[1];
        return (uint16_t)(q - v->p);
    case LK_IMAGE:
        return 10;
    default:
        return 0;
    }
}

void doc_cell_next(LineView *v)
{
    uint16_t size = payload_size(v);
    uint16_t y = (uint16_t)(v->y + v->height);

    fill_view(v->p + size, y, v);
}

const LinkInfo *doc_link(const Doc *d, uint8_t id)
{
    return (id && id <= d->nlinks) ? &links[id - 1] : NULL;
}

bool doc_find_anchor(const Doc *d, uint16_t hash, uint16_t *line)
{
    uint8_t i;

    for (i = 0; i < d->nanchors; i++)
        if (anchors[i].hash == hash) {
            *line = anchors[i].line;
            return true;
        }
    return false;
}

/* Remember where each link starts (its first line), looking at every text line,
 * including the lines inside side-by-side columns. */
static void note_links(const LineView *v)
{
    const uint8_t *p;
    uint8_t        nseg, s, id;

    if (v->kind != LK_TEXT)
        return;
    p = v->p;
    nseg = *p++;
    for (s = 0; s < nseg; s++) {
        id = p[2];
        if (id && links[id - 1].h == 0) {
            links[id - 1].y = v->y;
            links[id - 1].h = v->height;
        }
        p += 3 + p[1];
    }
}

static void index_links(const Doc *d)
{
    uint16_t i, x, w;
    uint8_t  c, k, cnt;
    LineView v, sub;

    if (!d->nlinks)
        return;
    for (i = 0; i < d->nlines; i++) {
        doc_line(d, i, &v);
        if (v.kind != LK_ROW) {
            note_links(&v);
            continue;
        }
        for (c = 0; c < doc_row_cols(&v); c++) {
            if (!doc_row_cell(d, &v, c, &sub, &cnt, &x, &w))
                continue;
            for (k = 0; k < cnt; k++) {
                note_links(&sub);
                if (k + 1 < cnt)
                    doc_cell_next(&sub);
            }
        }
    }
}

int doc_layout(Doc *d, const uint8_t *data, uint16_t size)
{
    static Layout l; /* big, so keep it off the stack */
    uint32_t pos;
    uint8_t  tl, c, n, w, h, i;

    memset(d, 0, sizeof *d);
    if (size < 21)
        return DOC_BAD_HEADER;
    if (data[0] != 'H' || data[1] != 'R')
        return DOC_BAD_MAGIC;
    if (data[2] < FORMAT_MIN || data[2] > FORMAT_MAX)
        return DOC_BAD_VERSION;
    tl = data[20];
    if (tl > 40 || 21u + tl > size)
        return DOC_BAD_HEADER;
    copy_name(d->prev, data + 4);
    copy_name(d->next, data + 12);
    memcpy(d->title, data + 21, tl);
    d->title[tl] = 0;
    pos = 21u + tl;

    memset(&l, 0, sizeof l);
    l.doc = d;
    l.width = VIEW_W;
    start_line(&l);

#define ARG() (pos < size ? data[pos++] : 0)

    while (pos < size && !l.full) {
        c = data[pos++];
        if (c >= 0x20) {
            add_char(&l, (char)c);
            continue;
        }
        switch (c) {
        case T_END:
            goto done;
        case T_HEAD:
            flush_word(&l);
            l.head = ARG() & 3;
            break;
        case T_PARA:
            end_para(&l);
            break;
        case T_BR:
            end_line(&l);
            break;
        case T_CENTER:
            l.center = ARG() != 0;
            break;
        case T_BOLD:
            l.bold = ARG() != 0;
            break;
        case T_ITALIC:
            l.ital = ARG() != 0;
            break;
        case T_LINK:
            if (pos + 8 > size) {
                pos = size;
                break;
            }
            begin_link(&l, data + pos, 0);
            pos += 8;
            break;
        case T_LINKA:
            if (pos + 10 > size) {
                pos = size;
                break;
            }
            begin_link(&l, data + pos, (uint16_t)(data[pos + 8] | (data[pos + 9] << 8)));
            pos += 10;
            break;
        case T_LINK_END:
            end_link(&l);
            break;
        case T_ANCHOR:
            if (pos + 2 > size) {
                pos = size;
                break;
            }
            add_anchor(&l, (uint16_t)(data[pos] | (data[pos + 1] << 8)));
            pos += 2;
            break;
        case T_VIDEO: /* name[8] len alt[len]: a link to the clip, "[> video: alt]" */
            if (pos + 9 > size || pos + 9u + data[pos + 8] > size) {
                pos = size;
                break;
            }
            n = data[pos + 8];
            if (nonzero8(data + pos))
                begin_link(&l, data + pos, 0);
            add_str(&l, "[> video");
            if (n)
                add_str(&l, ": ");
            for (i = 0; i < n; i++)
                add_char(&l, (char)data[pos + 9 + i]);
            add_str(&l, "]");
            end_link(&l);
            pos += 9u + n;
            break;
        case T_IMAGE:
            if (pos + 11 > size) {
                pos = size;
                break;
            }
            w = data[pos + 8];
            h = data[pos + 9];
            n = data[pos + 10];
            if (nonzero8(data + pos)) {
                image(&l, data + pos, w, h);
                pos += 11u + n;
            } else { /* no sprite: show the alt text instead */
                pos += 11;
                add_str(&l, n ? "[image: " : "[image]");
                for (i = 0; i < n && pos < size; i++)
                    add_char(&l, (char)data[pos++]);
                if (n)
                    add_str(&l, "]");
            }
            break;
        case T_INDENT:
            flush_word(&l);
            if (l.has_text) /* a new indent starts a new line */
                newline(&l);
            n = ARG();
            l.ind = (uint8_t)(16 * (n > 3 ? 3 : n));
            if (l.ind > l.width / 3)
                l.ind = (uint8_t)((l.width / 3 / 16) * 16);
            break;
        case T_PRE:
            flush_word(&l);
            l.pre = ARG() != 0;
            break;
        case T_RULE:
            rule(&l);
            break;
        case T_COLS:
            n = ARG();
            if (pos + n > size) {
                pos = size;
                break;
            }
            begin_cols(&l, n, data + pos);
            pos += n;
            break;
        case T_COL:
            next_col(&l);
            break;
        case T_COLS_END:
            cols_end(&l);
            break;
        default:
            break;
        }
    }
done:
    if (l.in_block)
        end_cols(&l);
    flush_word(&l);
    if (l.has_text)
        commit_text(&l);
    d->nlines = l.nlines;
    d->total_h = l.y;
    d->arena_used = l.arena_len;
    d->nlinks = l.nlinks;
    d->nanchors = l.nanchors;
    index_links(d);
    return DOC_OK;
}

/* Walk a page's token stream and call fn() with the name of every picture sprite and video.
 * Stops early when fn() returns true. */
static void walk_images(const uint8_t *data, uint16_t size,
                        bool (*fn)(const uint8_t *name8, void *ctx), void *ctx)
{
    uint32_t pos;
    uint8_t  c;

    if (size < 21 || data[0] != 'H' || data[1] != 'R')
        return;
    pos = 21u + data[20];
    while (pos < size) {
        c = data[pos++];
        if (c >= 0x20)
            continue;
        switch (c) {
        case T_END:
            return;
        case T_HEAD: case T_CENTER: case T_BOLD: case T_ITALIC: case T_INDENT: case T_PRE:
            pos += 1;
            break;
        case T_LINK:
            pos += 8;
            break;
        case T_LINKA:
            pos += 10;
            break;
        case T_ANCHOR:
            pos += 2;
            break;
        case T_COLS:
            if (pos < size)
                pos += 1u + data[pos];
            break;
        case T_IMAGE:
            if (pos + 11 > size)
                return;
            if (nonzero8(data + pos) && fn(data + pos, ctx))
                return;
            pos += 11u + data[pos + 10];
            break;
        case T_VIDEO:
            if (pos + 9 > size)
                return;
            if (nonzero8(data + pos) && fn(data + pos, ctx))
                return;
            pos += 9u + data[pos + 8];
            break;
        default:
            break;
        }
    }
}

typedef struct {
    uint8_t (*out)[8];
    uint8_t n, cap;
} SpriteList;

static bool add_sprite(const uint8_t *name8, void *ctx)
{
    SpriteList *s = ctx;
    uint8_t     i;

    for (i = 0; i < s->n; i++)
        if (!memcmp(s->out[i], name8, 8))
            return false;
    if (s->n < s->cap)
        memcpy(s->out[s->n++], name8, 8);
    return false;
}

typedef struct {
    const uint8_t *name8;
    bool           found;
} SpriteQuery;

static bool match_sprite(const uint8_t *name8, void *ctx)
{
    SpriteQuery *q = ctx;

    if (!memcmp(name8, q->name8, 8))
        q->found = true;
    return q->found; /* stop walking once it's been seen */
}

uint8_t doc_sprites(const uint8_t *data, uint16_t size, uint8_t (*out)[8], uint8_t n,
                    uint8_t cap)
{
    SpriteList s;

    s.out = out;
    s.n = n;
    s.cap = cap;
    walk_images(data, size, add_sprite, &s);
    return s.n;
}

bool doc_uses_sprite(const uint8_t *data, uint16_t size, const uint8_t *name8)
{
    SpriteQuery q;

    q.name8 = name8;
    q.found = false;
    walk_images(data, size, match_sprite, &q);
    return q.found;
}

uint16_t doc_y(const Doc *d, uint16_t i)
{
    return i < d->nlines ? recs[i].y : d->total_h;
}

uint16_t doc_line_at(const Doc *d, uint16_t y)
{
    uint16_t i;

    for (i = 0; i < d->nlines; i++)
        if (recs[i].y >= y)
            return i;
    return d->nlines ? (uint16_t)(d->nlines - 1) : 0;
}

uint16_t doc_max_top(const Doc *d, uint16_t view_height)
{
    uint16_t i;

    for (i = 0; i < d->nlines; i++)
        if (d->total_h - recs[i].y <= view_height)
            return i;
    return d->nlines ? (uint16_t)(d->nlines - 1) : 0;
}

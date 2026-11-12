/* Host test: lay out a page produced by `html2ce.py --bin`, check the layout rules,
 * and print it in a form that preview.py can draw.
 *   gcc -Wall -Wextra -I../src test_core.c ../src/core.c -o test_core
 *   ./test_core PAGE.bin > layout.txt
 */
#include <stdio.h>

#include "core.h"

static int errors;

/* Print one line (or one cell of a row) and check that it fits its width. */
static void print_line(const LineView *v, unsigned avail, const char *tag)
{
    if (v->kind == LK_TEXT) {
        const uint8_t *p = v->p;
        uint8_t nseg = *p++, s, k;
        unsigned width = 0;

        for (s = 0; s < nseg; s++) {
            uint8_t st = p[0], len = p[1], lid = p[2];

            printf("S %u %u %u ", st, len, lid);
            for (k = 0; k < len; k++)
                putchar(p[3 + k]);
            putchar('\n');
            width += (unsigned)len * CHAR_W * head_sx(STYLE_HEAD(st));
            p += 3 + len;
        }
        if (width + v->indent > avail) {
            printf("ERR %s is %u px wide + %u indent (max %u)\n", tag, width, v->indent, avail);
            errors++;
        }
    } else if (v->kind == LK_IMAGE) {
        printf("I %.8s %u %u\n", (const char *)v->p, v->p[8], v->p[9]);
    }
}

int main(int argc, char **argv)
{
    static uint8_t buf[70000];
    static Doc d;
    FILE *f;
    size_t n;
    int r;
    uint16_t i, expect_y = 0, line;
    uint8_t id;
    LineView v, sub;

    if (argc < 2 || !(f = fopen(argv[1], "rb"))) {
        fprintf(stderr, "usage: %s PAGE.bin\n", argv[0]);
        return 2;
    }
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    r = doc_layout(&d, buf, (uint16_t)n);
    if (r != DOC_OK) {
        printf("ERR doc_layout failed: %d\n", r);
        return 1;
    }
    printf("DOC \"%s\" prev=\"%s\" next=\"%s\" lines=%u total_h=%u truncated=%d arena=%u/%u "
           "links=%u anchors=%u\n",
           d.title, d.prev, d.next, d.nlines, d.total_h, d.truncated, d.arena_used, ARENA_CAP,
           d.nlinks, d.nanchors);
    for (i = 0; i < d.nlines; i++) {
        char tag[32];

        doc_line(&d, i, &v);
        printf("L %u y=%u h=%u kind=%u center=%u indent=%u\n", i, v.y, v.height, v.kind,
               v.center, v.indent);
        if (v.y != expect_y) { printf("ERR line %u y not contiguous\n", i); errors++; }
        expect_y = (uint16_t)(v.y + v.height);
        sprintf(tag, "line %u", i);
        if (v.kind == LK_ROW) {
            uint8_t c, k, cnt, nc = doc_row_cols(&v);

            for (c = 0; c < nc; c++) {
                uint16_t x, w, used = 0;

                printf("C %u\n", c);
                if (!doc_row_cell(&d, &v, c, &sub, &cnt, &x, &w)) {
                    printf("X 0 0\n");
                    continue;
                }
                printf("X %u %u\n", x, w);
                if (x + w > VIEW_W) { printf("ERR line %u col %u sticks out of the page\n", i, c); errors++; }
                for (k = 0; k < cnt; k++) {
                    printf("K %u %u %u %u %u\n", sub.kind, sub.center, sub.indent, sub.height,
                           sub.y - v.y);
                    sprintf(tag, "line %u col %u", i, c);
                    if (sub.kind > LK_IMAGE) { printf("ERR %s bad sub-line kind\n", tag); errors++; break; }
                    print_line(&sub, w, tag);
                    if (sub.y != v.y + used) { printf("ERR %s sub-line y\n", tag); errors++; }
                    used = (uint16_t)(used + sub.height);
                    if (k + 1 < cnt)
                        doc_cell_next(&sub);
                }
                if (used > v.height) { printf("ERR line %u col %u taller than its row\n", i, c); errors++; }
            }
        } else {
            print_line(&v, VIEW_W, tag);
        }
    }
    if (d.total_h != expect_y) { printf("ERR total_h mismatch\n"); errors++; }

    /* links: every one should have been placed, and anchors they point at should exist */
    for (id = 1; id <= d.nlinks; id++) {
        const LinkInfo *lk = doc_link(&d, id);
        int ok = 1;

        printf("LK %u %.8s %04x y=%u h=%u\n", id, (const char *)lk->name, lk->anchor, lk->y, lk->h);
        if (!lk->h) { printf("ERR link %u has no text\n", id); errors++; }
        else if (lk->y + lk->h > d.total_h) { printf("ERR link %u is off the page\n", id); errors++; }
        if (lk->anchor) {
            ok = doc_find_anchor(&d, lk->anchor, &line);
            printf("   anchor %04x -> %s line %u\n", lk->anchor, ok ? "found" : "NOT FOUND", ok ? line : 0);
        }
    }
    printf("END errors=%d\n", errors);
    return errors != 0;
}

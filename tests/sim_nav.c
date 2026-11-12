/* Drive the viewer's real main() on a PC with fake calculator libraries, to check
 * link selection, following, going back and page flow.
 *   gcc -Wall -Wextra -Istubs -I../src sim_nav.c ../src/core.c -o sim_nav
 *   ./sim_nav DIR KEYS      DIR holds the NAME.bin pages (html2ce.py --bin)
 * KEYS: r/l = Right/Left, u/d = Up/Down, e = Enter, c = Clear, 2 = 2nd, a = Alpha,
 *       x = Del (in the menu: asks, then Enter deletes, any other key cancels),
 *       . = let one 15 ms tick pass with no key (video playback is paced in ticks).
 * After every key it prints where the viewer is. A final run of Clear keys quits. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main htmlrd_main
#include "../src/main.c"
#undef main

volatile uint16_t kb_Data[8];

static struct { char name[9]; uint8_t *data; uint16_t size; } files[64];
static int nfiles;
static const char *keys;
static int  frame, quitting;

void delay(unsigned ms) { (void)ms; }
char *ti_Detect(void **vat, const char *s)
{
    long i = (long)*vat;

    for (; i < nfiles; i++) /* like the real call: only variables that start with s */
        if (files[i].size >= strlen(s) && !memcmp(files[i].data, s, strlen(s))) {
            *vat = (void *)(i + 1);
            return files[i].name;
        }
    *vat = (void *)(long)nfiles;
    return NULL;
}
uint8_t ti_Delete(const char *name)
{
    int i;

    for (i = 0; i < nfiles; i++)
        if (!strncmp(files[i].name, name, 8)) {
            free(files[i].data);
            memmove(&files[i], &files[i + 1], (size_t)(nfiles - i - 1) * sizeof files[0]);
            nfiles--;
            return 1;
        }
    return 0;
}
uint8_t ti_Open(const char *name, const char *mode)
{
    int i;

    (void)mode;
    for (i = 0; i < nfiles; i++)
        if (!strncmp(files[i].name, name, 8))
            return (uint8_t)(i + 1);
    return 0;
}
void *ti_GetDataPtr(uint8_t h) { return files[h - 1].data; }
uint16_t ti_GetSize(uint8_t h) { return files[h - 1].size; }
int ti_Close(uint8_t h) { (void)h; return 0; }

void gfx_Begin(void) {} void gfx_End(void) {}
void gfx_SetDrawBuffer(void) {} void gfx_SwapDraw(void) {}
void gfx_SetMonospaceFont(uint8_t s) { (void)s; }
void gfx_SetPalette(const void *p, unsigned s, uint8_t o) { (void)p; (void)s; (void)o; }
void gfx_FillScreen(uint8_t c) { (void)c; } void gfx_SetColor(uint8_t c) { (void)c; }
void gfx_FillRectangle(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void gfx_Rectangle(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void gfx_HorizLine(int x, int y, int l) { (void)x; (void)y; (void)l; }
void gfx_Sprite(const gfx_sprite_t *s, int x, int y) { (void)s; (void)x; (void)y; }
void gfx_ScaledSprite_NoClip(const gfx_sprite_t *s, int x, int y, uint8_t ws, uint8_t hs)
{
    uint32_t sum = 0;
    unsigned k;

    for (k = 0; k < (unsigned)s->width * s->height; k++)
        sum = sum * 31u + s->data[k];
    printf("  V draw %u x=%d y=%d scale=%ux%u of %ux%u\n", sum, x, y, ws, hs, s->width, s->height);
}
void gfx_SetTextFGColor(uint8_t c) { (void)c; } void gfx_SetTextBGColor(uint8_t c) { (void)c; }
void gfx_SetTextTransparentColor(uint8_t c) { (void)c; }
void gfx_SetTextScale(uint8_t a, uint8_t b) { (void)a; (void)b; }
void gfx_SetTextXY(int x, int y) { (void)x; (void)y; }
void gfx_PrintChar(const char c) { (void)c; }
void gfx_PrintString(const char *s) { (void)s; }
void gfx_PrintStringXY(const char *s, int x, int y) { (void)s; (void)x; (void)y; }

static void report_menu(char key)
{
    uint8_t n = scan_pages(), i;

    printf("%c -> menu:", key);
    for (i = 0; i < n; i++)
        printf(" %s", entries[i].name);
    printf("%s | on calculator:", n ? "" : " (empty)");
    for (i = 0; i < nfiles; i++)
        printf(" %s", files[i].name);
    printf("%s\n", asking ? "  [asking to delete]" : "");
}

static void report(char key)
{
    const LinkInfo *k = doc_link(&doc, sel_link);

    if (!cur_name[0] || (key == 'x' || asking || !doc.nlines)) { report_menu(key); return; }
    printf("%c -> %-8s top=%-4u/%-4u sel=%-3u", key, cur_name, top, max_top, sel_link);
    if (k) {
        printf(" link y=%u anchor=%04x", k->y, k->anchor);
        printf(" %s", link_visible(k) ? "visible" : "OFFSCREEN");
    }
    printf(" hist=%u%s%s\n", nhist, doc_msg ? " msg: " : "", doc_msg ? doc_msg : "");
}

void kb_Scan(void)
{
    static char last;
    char c;

    memset((void *)kb_Data, 0, sizeof kb_Data);
    if (frame & 1) { frame++; return; }  /* a released frame between key presses */
    if (*keys == '.') { keys++; return; } /* an idle tick */
    if (frame > 0)
        report(last);
    c = *keys ? *keys++ : 0;
    if (!c) {
        if (quitting++ > 12) { printf("(clear never quit)\n"); exit(1); }
        c = 'c';
    }
    last = c;
    frame++;
    switch (c) {
    case 'r': kb_Data[7] |= kb_Right; break;
    case 'l': kb_Data[7] |= kb_Left;  break;
    case 'u': kb_Data[7] |= kb_Up;    break;
    case 'd': kb_Data[7] |= kb_Down;  break;
    case 'e': kb_Data[6] |= kb_Enter; break;
    case 'c': kb_Data[6] |= kb_Clear; break;
    case '2': kb_Data[1] |= kb_2nd;   break;
    case 'a': kb_Data[2] |= kb_Alpha; break;
    case 'x': kb_Data[1] |= kb_Del;   break;
    }
}

int main(int argc, char **argv)
{
    DIR *dir;
    struct dirent *de;

    if (argc < 3) { fprintf(stderr, "usage: %s DIR KEYS\n", argv[0]); return 2; }
    keys = argv[2];
    dir = opendir(argv[1]);
    while (dir && (de = readdir(dir)) && nfiles < 64) {
        char path[512], *dot = strrchr(de->d_name, '.');
        FILE *f;

        if (!dot || strcmp(dot, ".bin") || strlen(de->d_name) > 12)
            continue;
        snprintf(path, sizeof path, "%s/%s", argv[1], de->d_name);
        f = fopen(path, "rb");
        if (!f)
            continue;
        files[nfiles].data = malloc(70000);
        files[nfiles].size = (uint16_t)fread(files[nfiles].data, 1, 70000, f);
        fclose(f);
        memset(files[nfiles].name, 0, 9);
        memcpy(files[nfiles].name, de->d_name, (size_t)(dot - de->d_name));
        nfiles++;
    }
    return htmlrd_main();
}

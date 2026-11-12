/* Host test for video.c: read the chunks of one clip (html2ce.py --bin), unpack every
 * frame and print a checksum of each, for check_video.py to compare.
 *   gcc -Wall -Wextra -I../src test_video.c ../src/video.c -o test_video
 *   ./test_video CHUNK1.bin [CHUNK2.bin ...]      (in chain order) */
#include <stdio.h>
#include <stdlib.h>

#include "video.h"

int main(int argc, char **argv)
{
    static uint8_t  buf[70000], pix[VIDEO_PIX_MAX];
    static const uint8_t *frames[VIDEO_FRAMES_MAX];
    VideoChunk c, first;
    uint16_t   n = 0, i, got;
    uint32_t   sum;
    int        a, errors = 0;

    for (a = 1; a < argc; a++) {
        FILE  *f = fopen(argv[a], "rb");
        size_t size;
        uint16_t before = n;
        uint8_t *copy;

        if (!f) { fprintf(stderr, "cannot read %s\n", argv[a]); return 2; }
        size = fread(buf, 1, sizeof buf, f);
        fclose(f);
        copy = malloc(size);                       /* frame pointers must outlive buf */
        for (i = 0; i < size; i++) copy[i] = buf[i];
        if (!video_chunk(copy, (uint16_t)size, &c)) { printf("ERR %s: bad chunk header\n", argv[a]); return 1; }
        if (a == 1) first = c;
        n = video_index(copy, (uint16_t)size, &c, frames, n, VIDEO_FRAMES_MAX);
        printf("CHUNK %s frames=%u (header says %u) next=\"%s\"\n", argv[a], n - before, c.here, c.next);
        if (n - before != c.here) { printf("ERR frame count differs from header\n"); errors++; }
        if (c.w != first.w || c.h != first.h || c.total != first.total) { printf("ERR chunks disagree\n"); errors++; }
    }
    printf("CLIP %ux%u scale=%u fps=%u total=%u indexed=%u\n", first.w, first.h, first.scale,
           first.fps, first.total, n);
    if (n != first.total) { printf("ERR indexed %u frames but the clip has %u\n", n, first.total); errors++; }
    for (i = 0; i < n; i++) {
        uint16_t px = (uint16_t)(first.w * first.h), k;

        got = video_unrle(frames[i] + 2, video_frame_len(frames[i]), pix, px);
        if (got != px) { printf("ERR frame %u unpacked to %u bytes, wanted %u\n", i, got, px); errors++; }
        for (sum = 0, k = 0; k < got; k++)
            sum = sum * 31u + pix[k];
        printf("F %u %u\n", i, sum);
    }
    printf("END errors=%d\n", errors);
    return errors != 0;
}

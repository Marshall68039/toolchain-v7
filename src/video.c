#include "video.h"

#include <string.h>

bool video_chunk(const uint8_t *data, uint16_t size, VideoChunk *c)
{
    uint8_t i;

    memset(c, 0, sizeof *c);
    if (size < VIDEO_HEADER || data[0] != 'H' || data[1] != 'V' || data[2] != 1)
        return false;
    for (i = 0; i < 8 && data[4 + i]; i++)
        c->next[i] = (char)data[4 + i];
    c->next[i] = 0;
    c->w = data[12];
    c->h = data[13];
    c->scale = data[14];
    c->fps = data[15];
    c->total = (uint16_t)(data[16] | (data[17] << 8));
    c->here = (uint16_t)(data[18] | (data[19] << 8));
    return c->w && c->h && c->scale && c->fps && (uint16_t)c->w * c->h <= VIDEO_PIX_MAX;
}

uint16_t video_index(const uint8_t *data, uint16_t size, const VideoChunk *c,
                     const uint8_t **frames, uint16_t n, uint16_t cap)
{
    uint32_t pos = VIDEO_HEADER;
    uint16_t i, len;

    for (i = 0; i < c->here && n < cap; i++) {
        if (pos + 2 > size)
            break;
        len = video_frame_len(data + pos);
        if (pos + 2u + len > size)
            break;
        frames[n++] = data + pos;
        pos += 2u + len;
    }
    return n;
}

uint16_t video_unrle(const uint8_t *rle, uint16_t len, uint8_t *dst, uint16_t cap)
{
    uint16_t in = 0, out = 0, n;
    uint8_t  c;

    while (in < len && out < cap) {
        c = rle[in++];
        if (c < 128) { /* c+1 literal bytes */
            n = (uint16_t)(c + 1);
            if (in + n > len)
                n = (uint16_t)(len - in);
            if (out + n > cap)
                n = (uint16_t)(cap - out);
            memcpy(dst + out, rle + in, n);
            in = (uint16_t)(in + n);
            out = (uint16_t)(out + n);
        } else { /* one byte repeated c-126 times */
            if (in >= len)
                break;
            n = (uint16_t)(c - 126);
            if (out + n > cap)
                n = (uint16_t)(cap - out);
            memset(dst + out, rle[in++], n);
            out = (uint16_t)(out + n);
        }
    }
    return out;
}

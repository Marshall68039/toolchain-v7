/*
 * video.h - small silent video clips for the HTML reader.
 *
 * Pure C with no calculator dependencies, so it can be tested on a PC.
 * html2ce.py turns a video into a chain of "HV" AppVars (chunks of up to ~60000 bytes).
 * Every chunk starts with a 20-byte header:
 *     2 bytes  magic "HV"
 *     1 byte   format version (1)
 *     1 byte   reserved (0)
 *     8 bytes  next chunk's AppVar name (NUL padded, all zero = last chunk)
 *     1 byte   frame width in pixels  (stored size, small)
 *     1 byte   frame height in pixels
 *     1 byte   scale: the viewer draws every stored pixel as scale x scale pixels
 *     1 byte   frames per second
 *     2 bytes  total frames in the whole clip (little endian)
 *     2 bytes  frames in this chunk
 * followed by this chunk's frames, each [length: 2 bytes LE][data], where data is the
 * frame's width*height palette indexes (the reader's fixed palette), row by row, packed
 * like this: a byte c < 128 is followed by c+1 literal bytes; a byte c >= 128 is followed
 * by one byte that is repeated c-126 times (2..129).
 */
#ifndef VIDEO_H
#define VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#define VIDEO_HEADER     20u
#define VIDEO_PIX_MAX    6000u  /* most pixels in one stored frame            */
#define VIDEO_FRAMES_MAX 300u   /* most frames in one clip                    */

typedef struct {
    uint8_t  w, h, scale, fps;
    uint16_t total; /* frames in the whole clip */
    uint16_t here;  /* frames in this chunk     */
    char     next[9];
} VideoChunk;

/* Read a chunk's header; false if it isn't a usable video chunk. */
bool video_chunk(const uint8_t *data, uint16_t size, VideoChunk *c);

/* Append pointers to this chunk's frames to frames[n..]; returns the new count.
 * A frame pointer addresses its 2-byte length. */
uint16_t video_index(const uint8_t *data, uint16_t size, const VideoChunk *c,
                     const uint8_t **frames, uint16_t n, uint16_t cap);

/* Unpack one frame (the bytes after its length) into dst; returns bytes written. */
uint16_t video_unrle(const uint8_t *rle, uint16_t len, uint8_t *dst, uint16_t cap);

static inline uint16_t video_frame_len(const uint8_t *frame)
{
    return (uint16_t)(frame[0] | (frame[1] << 8));
}

#endif

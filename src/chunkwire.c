#include "chunkwire.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  LEB128-style unsigned varint                                       */
/* ------------------------------------------------------------------ */
/* 7 data bits per byte, high bit = "more bytes follow". A count up to
 * CHUNKWIRE_COLUMN_BLOCKS (65536) needs at most 3 bytes. */

static size_t varint_size(size_t v) {
    size_t n = 1;
    while (v >= 0x80) { v >>= 7; n++; }
    return n;
}

/* Write a varint into out[off..out_cap). Returns bytes written, or 0 if it
 * wouldn't fit (caller treats 0 as failure). */
static size_t varint_write(uint8_t* out, size_t off, size_t out_cap, size_t v) {
    size_t need = varint_size(v);
    if (off + need > out_cap) return 0;
    for (size_t i = 0; i < need; i++) {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (i + 1 < need) byte |= 0x80;
        out[off + i] = byte;
    }
    return need;
}

/* Read a varint from in[*off..in_len). Returns true on success, advancing
 * *off; false on truncation or overlong (> size_t) encoding. */
static bool varint_read(const uint8_t* in, size_t in_len, size_t* off,
                        size_t* out_v) {
    size_t v = 0;
    int shift = 0;
    for (;;) {
        if (*off >= in_len) return false;            /* truncated */
        if (shift > 56) return false;                /* guards against overlong */
        uint8_t byte = in[(*off)++];
        v |= (size_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    *out_v = v;
    return true;
}

/* ------------------------------------------------------------------ */
/*  RLE                                                                */
/* ------------------------------------------------------------------ */

size_t chunkwire_rle_bound(size_t n) {
    /* Pathological: n single-element runs. Each run = varint(1) (1 byte) +
     * 1 block byte = 2 bytes. Add a little slack for safety. */
    return n * 2 + 8;
}

size_t chunkwire_rle_encode(const uint8_t* blocks, size_t n,
                            uint8_t* out, size_t out_cap) {
    size_t off = 0;
    size_t i = 0;
    while (i < n) {
        uint8_t b = blocks[i];
        size_t run = 1;
        while (i + run < n && blocks[i + run] == b) run++;

        size_t w = varint_write(out, off, out_cap, run);
        if (w == 0) return 0;
        off += w;
        if (off + 1 > out_cap) return 0;
        out[off++] = b;

        i += run;
    }
    /* n == 0 produces an empty (zero-byte) payload, which is valid and decodes
     * back to zero bytes. Engine columns are never empty, but keep it sane. */
    return off;
}

bool chunkwire_rle_decode(const uint8_t* in, size_t in_len,
                          uint8_t* out, size_t out_cap, size_t* out_n) {
    size_t in_off = 0;
    size_t out_off = 0;
    while (in_off < in_len) {
        size_t run = 0;
        if (!varint_read(in, in_len, &in_off, &run)) return false;
        if (run == 0) return false;                  /* invalid: zero-length run */
        if (in_off >= in_len) return false;          /* missing block byte */
        uint8_t b = in[in_off++];
        if (out_off + run > out_cap) return false;   /* would overrun output */
        memset(out + out_off, b, run);
        out_off += run;
    }
    if (out_n) *out_n = out_off;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Little-endian fixed-width helpers (self-contained)                 */
/* ------------------------------------------------------------------ */

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/*  Full chunk body                                                    */
/* ------------------------------------------------------------------ */

#define CHUNK_BODY_HEADER 12   /* cx(4) + cz(4) + rle_len(4) */

size_t chunkwire_encode_bound(void) {
    return CHUNK_BODY_HEADER + chunkwire_rle_bound(CHUNKWIRE_COLUMN_BLOCKS);
}

size_t chunkwire_encode_chunk(int32_t cx, int32_t cz, const uint8_t* blocks,
                              uint8_t* out, size_t out_cap) {
    if (out_cap < CHUNK_BODY_HEADER) return 0;
    size_t rle = chunkwire_rle_encode(blocks, CHUNKWIRE_COLUMN_BLOCKS,
                                      out + CHUNK_BODY_HEADER,
                                      out_cap - CHUNK_BODY_HEADER);
    if (rle == 0) return 0;
    put_u32(out + 0, (uint32_t)cx);
    put_u32(out + 4, (uint32_t)cz);
    put_u32(out + 8, (uint32_t)rle);
    return CHUNK_BODY_HEADER + rle;
}

bool chunkwire_decode_chunk(const uint8_t* in, size_t in_len,
                            int32_t* cx, int32_t* cz,
                            uint8_t* blocks, size_t blocks_cap) {
    if (in_len < CHUNK_BODY_HEADER) return false;
    if (blocks_cap < CHUNKWIRE_COLUMN_BLOCKS) return false;
    uint32_t rle_len = get_u32(in + 8);
    if ((size_t)rle_len > in_len - CHUNK_BODY_HEADER) return false;

    size_t out_n = 0;
    if (!chunkwire_rle_decode(in + CHUNK_BODY_HEADER, rle_len,
                              blocks, blocks_cap, &out_n))
        return false;
    if (out_n != CHUNKWIRE_COLUMN_BLOCKS) return false;

    if (cx) *cx = (int32_t)get_u32(in + 0);
    if (cz) *cz = (int32_t)get_u32(in + 4);
    return true;
}

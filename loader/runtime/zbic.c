// ZBIC: how the parts of some Switch 2 modules are packed (flag 0x80 of an NSO's header, from system version 22), written again
// as the Zstandard frame it stands for. tools/zbic.py has the story and is what this was written from, line for line: a ZBIC frame
// is a Zstandard frame with its first four bytes changed and each table of probabilities written another way (the running sums of
// the counts, by binary interpolative coding, as the digits of one long number). Nothing is unpacked here: the frame comes out the
// standard way, for the Zstandard library to unpack (game.c).
#ifdef NX2_LOADER
#include <string.h>

#include "nx2.h"

typedef struct {
    const u8 *data;
    u64 first, left, value;
} Digits;
static void fill(Digits *d) {
    while (d->left && d->value < (1ull << 40)) d->value = d->value << 8 | d->data[d->first + d->left - 1], d->left--;
}
static u64 digit(Digits *d, u64 radix) {
    const u64 v = d->value % radix;
    d->value /= radix;
    fill(d);
    return v;
}

// the sums' places in the order they are written: a range's middle, then its upper half's, then its lower half's
static int middles(u32 low, u32 high, u32 (*out)[3], int n) {
    if (high - low >= 2) {
        const u32 mid = (low + high) / 2;
        out[n][0] = low, out[n][1] = mid, out[n][2] = high, n++;
        n = middles(mid, high, out, n);
        n = middles(low, mid, out, n);
    }
    return n;
}

// A table as ZBIC writes it, at data[at..end): each symbol's count as Zstandard has them (-1: "less than one"), how many there are,
// the table's log, the bytes it took. 0: read; -1: not a table, or damaged.
static int table(const u8 *data, u64 at, u64 end, u32 most, s32 *counts, u32 *ncounts, u32 *log, u64 *took) {
    if (at >= end) return -1;
    const u32 more = data[at] & 0x7F, shifted = data[at] >> 7;
    if (at + 1 + more > end) return -1;
    Digits d = {data, at + 1, more, 0};
    fill(&d);
    const u32 last_symbol = (u32)digit(&d, 52) + 1;
    if (last_symbol > most) return -1;
    *log = (u32)digit(&d, 8) + 5;
    const u32 whole = 1u << *log;
    const u32 total = (u32)digit(&d, whole) + 1 + (shifted ? last_symbol + 1 : 0);
    u32 span = 2;
    while (span <= last_symbol) span *= 2;
    static u32 sums[513], order[512][3];
    if (span > 512) return -1;
    memset(sums, 0, sizeof sums);
    sums[span] = total;
    const int n = middles(0, span, order, 0);
    for (int k = 0; k < n; k++) {
        const u32 a = sums[order[k][0]], b = sums[order[k][2]];
        if (b < a) return -1;
        sums[order[k][1]] = a != b ? a + (u32)digit(&d, (u64)(b - a) + 1) : a;
    }
    if (sums[1]) digit(&d, (u64)sums[1] + 1);  // (one digit more is written, between nought and the first sum: it says nothing)
    u64 all = 0;
    for (u32 k = 0; k <= last_symbol; k++) {
        const s64 c = (s64)sums[k + 1] - (s64)sums[k] - (s64)shifted;
        if (c < -1) return -1;
        counts[k] = (s32)c, all += (u64)(c < 0 ? -c : c);
    }
    if (d.left || all != whole) return -1;
    *ncounts = last_symbol + 1, *took = more + 1;
    return 0;
}

// the same table as Zstandard writes it (RFC 8878, 4.1.1), to out: how many bytes, or -1 if there is no room
static s64 standard(const s32 *counts, u32 ncounts, u32 log, u8 *out, u64 room) {
    u64 used = 0;
    memset(out, 0, room < 512 ? room : 512);
#define PUT(value, n) \
    do { \
        const u64 v_ = (u64)(value); \
        for (u32 b_ = 0; b_ < (u32)(n); b_++, used++) { \
            if (used / 8 >= room || used / 8 >= 512) return -1; \
            if (v_ >> b_ & 1) out[used / 8] |= (u8)(1u << (used % 8)); \
        } \
    } while (0)
    PUT(log - 5, 4);
    s64 remaining = (1ll << log) + 1, threshold = 1ll << log;
    u32 width = log + 1, k = 0;
    while (remaining > 1 && k < ncounts) {
        const s32 count = counts[k++];
        const s64 value = (s64)count + 1, limit = 2 * threshold - 1 - remaining;
        if (value < limit) PUT(value, width - 1);
        else if (value < threshold) PUT(value, width);
        else PUT(value + limit, width);
        remaining -= count < 0 ? -count : count;
        while (remaining < threshold) width--, threshold >>= 1;
        if (count == 0) {  // (after a nought: how many more follow it, two bits at a time, a 3 meaning three and more to come)
            u32 run = 0;
            while (k + run < ncounts && counts[k + run] == 0) run++;
            k += run;
            for (; run >= 3; run -= 3) PUT(3, 2);
            PUT(run, 2);
        }
    }
#undef PUT
    return (s64)((used + 7) / 8);
}

typedef struct {
    u8 *out;
    u64 at, room;
    int over;
} Out;
static void put(Out *o, const u8 *bytes, u64 n) {
    if (o->at + n > o->room) {
        o->over = 1;
        return;
    }
    memcpy(o->out + o->at, bytes, n), o->at += n;
}
static void put_le(Out *o, u64 v, u32 n) {
    u8 b[8];
    for (u32 k = 0; k < n; k++) b[k] = (u8)(v >> (8 * k));
    put(o, b, n);
}
static u64 le(const u8 *p, u32 n) {
    u64 v = 0;
    for (u32 k = 0; k < n; k++) v |= (u64)p[k] << (8 * k);
    return v;
}

static const u8 size_head[4] = {3, 3, 4, 5}, size_bits[4] = {10, 10, 14, 18};  // a packed literals section's header

// a block's literals, written with their tree's table the standard way: where the sequences begin, or 0 for a damaged block
static u64 literals(const u8 *data, u64 at, u64 end, Out *o) {
    if (at >= end) return 0;
    const u32 kind = data[at] & 3;
    u32 form = data[at] >> 2 & 3;
    if (kind < 2) {  // as they are, or one byte repeated: nothing of ZBIC's in them
        const u32 head = (form == 0 || form == 2) ? 1 : form == 1 ? 2 : 3;
        if (at + head > end) return 0;
        const u64 size = le(data + at, head) >> (head == 1 ? 3 : 4), stop = at + head + (kind == 0 ? size : 1);
        if (stop > end) return 0;
        put(o, data + at, stop - at);
        return stop;
    }
    u32 head = size_head[form], bits = size_bits[form];
    if (at + head > end) return 0;
    const u64 v = le(data + at, head), unpacked = v >> 4 & ((1ull << bits) - 1), packed = v >> (4 + bits) & ((1ull << bits) - 1), stop = at + head + packed;
    if (stop > end) return 0;
    const u8 *body = data + at + head;
    u64 body_size = packed;
    static u8 rewritten[1 << 18];  // (a block is 128 KB at most)
    if (kind == 2 && body_size && body[0] < 128) {
        // the tree's weights, packed with a table of their own: the table, then their stream, which stays as it is
        const u32 n = body[0];
        static s32 counts[256];
        u32 ncounts, log;
        u64 took;
        if (1 + (u64)n > body_size || table(body, 1, 1 + n, 255, counts, &ncounts, &log, &took)) return 0;
        u8 weights[512];
        const s64 std = standard(counts, ncounts, log, weights, sizeof weights);
        const u64 stream = n - took;
        if (std < 0 || (u64)std + stream > 127 || 1 + (u64)std + stream + (body_size - 1 - n) > sizeof rewritten) return 0;
        rewritten[0] = (u8)((u64)std + stream);
        memcpy(rewritten + 1, weights, (u64)std);
        memcpy(rewritten + 1 + std, body + 1 + took, stream);
        memcpy(rewritten + 1 + std + stream, body + 1 + n, body_size - 1 - n);
        body_size = 1 + (u64)std + stream + (body_size - 1 - n), body = rewritten;
    }
    const u64 larger = unpacked > body_size ? unpacked : body_size;
    while (form && form < 3 && larger >> size_bits[form]) form++;
    head = size_head[form], bits = size_bits[form];
    if (larger >> bits) return 0;
    put_le(o, kind | form << 2 | unpacked << 4 | body_size << (4 + bits), head);
    put(o, body, body_size);
    return stop;
}

// a packed block, the standard way: 0, or -1 for a damaged one
static int block(const u8 *data, u64 at, u64 end, Out *o) {
    at = literals(data, at, end, o);
    if (!at || at >= end) return -1;
    const u32 n = data[at] < 128 ? 1 : data[at] < 255 ? 2 : 3;
    if (at + n > end) return -1;
    put(o, data + at, n);
    const u32 count = data[at];
    at += n;
    if (count) {
        if (at >= end) return -1;
        const u32 modes = data[at];
        put(o, data + at, 1);
        at++;
        static const u32 most[3] = {35, 31, 52};  // literal lengths, offsets, match lengths
        for (int k = 0; k < 3; k++) {
            const u32 mode = modes >> (6 - 2 * k) & 3;
            if (mode == 1) {  // one symbol always: its byte
                if (at >= end) return -1;
                put(o, data + at, 1);
                at++;
            } else if (mode == 2) {
                static s32 counts[64];
                u32 ncounts, log;
                u64 took;
                u8 std[512];
                if (table(data, at, end, most[k], counts, &ncounts, &log, &took)) return -1;
                const s64 len = standard(counts, ncounts, log, std, sizeof std);
                if (len < 0) return -1;
                put(o, std, (u64)len);
                at += took;
            }
        }
    }
    if (at > end) return -1;
    put(o, data + at, end - at);
    return 0;
}

// ZBIC frames as the Zstandard frames they stand for: how many bytes were written to out, or -1 (not ZBIC, damaged, or no room)
s64 nx2_zbic_to_zstd(const u8 *data, u64 size, u8 *out, u64 room) {
    Out o = {out, 0, room, 0};
    u64 at = 0;
    while (at < size) {
        if (at + 5 > size || memcmp(data + at, "ZBIC", 4)) return -1;
        const u32 flags = data[at + 4], one_segment = flags >> 5 & 1;
        static const u8 dict[4] = {0, 1, 2, 4}, content[4] = {0, 2, 4, 8};
        const u32 head = 1 + (one_segment ? 0 : 1) + dict[flags & 3] + ((flags >> 6) ? content[flags >> 6] : one_segment ? 1 : 0);
        if (at + 4 + head > size) return -1;
        static const u8 zstd[4] = {0x28, 0xB5, 0x2F, 0xFD};
        put(&o, zstd, 4), put(&o, data + at + 4, head);
        at += 4 + head;
        for (;;) {
            if (at + 3 > size) return -1;
            const u32 h = (u32)le(data + at, 3), last = h & 1, kind = h >> 1 & 3, len = h >> 3;
            const u64 n = kind == 1 ? 1 : len;
            if (kind == 3 || at + 3 + n > size) return -1;
            if (kind == 2) {
                const u64 header_at = o.at;
                put_le(&o, 0, 3);
                const u64 body_at = o.at;
                if (block(data, at + 3, at + 3 + len, &o) || o.over) return -1;
                const u64 body = o.at - body_at;
                if (body > (128u << 10)) return -1;
                const u32 again = (u32)(body << 3 | 2u << 1 | last);
                out[header_at] = (u8)again, out[header_at + 1] = (u8)(again >> 8), out[header_at + 2] = (u8)(again >> 16);
            } else {
                put(&o, data + at, 3 + n);
            }
            at += 3 + n;
            if (last) break;
        }
        if (flags & 4) {  // the sum of what is unpacked: the same either way
            if (at + 4 > size) return -1;
            put(&o, data + at, 4);
            at += 4;
        }
    }
    return o.over ? -1 : (s64)o.at;
}
#endif

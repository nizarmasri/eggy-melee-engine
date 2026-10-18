/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Desync resync, the pure part (src/pc/net_resync.h). */
#include "pc/net_resync.h"

#include <string.h>

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put64(uint8_t* p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t get64(const uint8_t* p) {
    return (uint64_t)get32(p) << 32 | get32(p + 4);
}

uint64_t resync_checksum(const uint8_t* p, size_t n) {
    uint64_t h = 14695981039346656037ull; /* FNV-1a 64 */
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static size_t header_size(uint32_t nregions) {
    return RESYNC_HEADER_FIXED + (size_t)nregions * RESYNC_REGION_BYTES;
}

static size_t body_size(const ResyncHeader* h) {
    size_t n = 0;
    for (uint32_t i = 0; i < h->nregions; i++) {
        n += h->region[i].len;
    }
    return n;
}

size_t resync_image_size(const ResyncHeader* h) {
    if (h->nregions > RESYNC_REGIONS_MAX) {
        return 0;
    }
    const size_t n = header_size(h->nregions) + body_size(h) + RESYNC_TRAILER;
    return n > RESYNC_BYTES_MAX ? 0 : n;
}

size_t resync_encode(const ResyncHeader* h, const uint8_t* body, uint8_t* out, size_t cap) {
    const size_t n = resync_image_size(h);
    if (n == 0 || n > cap) {
        return 0;
    }
    put32(out, RESYNC_MAGIC);
    put32(out + 4, RESYNC_VERSION);
    put32(out + 8, (uint32_t)h->frame);
    put32(out + 12, h->session);
    put32(out + 16, (uint32_t)h->scene);
    put64(out + 20, h->seed_addr);
    put32(out + 28, h->seed_val);
    put32(out + 32, h->nregions);
    size_t at = RESYNC_HEADER_FIXED;
    for (uint32_t i = 0; i < h->nregions; i++) {
        put64(out + at, h->region[i].addr);
        put32(out + at + 8, h->region[i].len);
        at += RESYNC_REGION_BYTES;
    }
    const size_t body_n = body_size(h);
    memcpy(out + at, body, body_n);
    at += body_n;
    put64(out + at, resync_checksum(out, at));
    return n;
}

bool resync_decode(const uint8_t* in, size_t len, ResyncHeader* h, const uint8_t** body) {
    if (in == NULL || len < RESYNC_HEADER_FIXED + RESYNC_TRAILER || len > RESYNC_BYTES_MAX ||
        get32(in) != RESYNC_MAGIC || get32(in + 4) != RESYNC_VERSION)
    {
        return false;
    }
    memset(h, 0, sizeof *h);
    h->frame = (int32_t)get32(in + 8);
    h->session = get32(in + 12);
    h->scene = (int32_t)get32(in + 16);
    h->seed_addr = get64(in + 20);
    h->seed_val = get32(in + 28);
    h->nregions = get32(in + 32);
    if (h->nregions > RESYNC_REGIONS_MAX || header_size(h->nregions) + RESYNC_TRAILER > len) {
        return false;
    }
    size_t at = RESYNC_HEADER_FIXED;
    for (uint32_t i = 0; i < h->nregions; i++) {
        h->region[i].addr = get64(in + at);
        h->region[i].len = get32(in + at + 8);
        at += RESYNC_REGION_BYTES;
    }
    const size_t body_n = body_size(h);
    if (body_n > len || at + body_n + RESYNC_TRAILER != len) {
        return false;
    }
    if (get64(in + at + body_n) != resync_checksum(in, at + body_n)) {
        return false;
    }
    *body = in + at;
    return true;
}

const char* resync_layout_check(
    const ResyncHeader* theirs, const ResyncRegion* mine, int nmine, int fixed) {
    if ((int)theirs->nregions != nmine) {
        return "another set of heaps";
    }
    for (int i = 0; i < nmine; i++) {
        if (theirs->region[i].addr != mine[i].addr) {
            return i < fixed ? "another memory layout (another engine build?)" :
                               "a heap starts elsewhere";
        }
        if (i < fixed && theirs->region[i].len != mine[i].len) {
            return "another memory layout (another engine build?)";
        }
    }
    return NULL;
}

bool resync_frame_final(int32_t frame, int32_t remote_have, int32_t rb_frame) {
    return frame >= 0 && (int64_t)remote_have >= (int64_t)frame - 1 && rb_frame < 0;
}

int resync_apply_when(int32_t s, int32_t now) {
    if (s < 0) {
        return RESYNC_TOO_LATE;
    }
    if (now == s) {
        return RESYNC_AT_FRESH;
    }
    if (now < s) {
        return RESYNC_LATER;
    }
    return (int64_t)now - s <= RESYNC_REPLAY_MAX ? RESYNC_ROLLBACK : RESYNC_TOO_LATE;
}

void resync_policy_init(ResyncPolicy* p) {
    memset(p, 0, sizeof *p);
    p->frame = -1;
    p->asked = -1;
}

bool resync_policy_want(ResyncPolicy* p, int32_t f) {
    if (p->state != RESYNC_IDLE || p->sent >= RESYNC_SENDS_MAX) {
        return false;
    }
    p->state = RESYNC_WANT;
    p->asked = f;
    return true;
}

void resync_policy_taken(ResyncPolicy* p, int32_t s, uint8_t receivers) {
    if (p->state != RESYNC_WANT) {
        return;
    }
    p->state = RESYNC_HOLD;
    p->sent++;
    p->frame = s;
    p->waiting = receivers;
    p->applied = 0;
}

void resync_policy_answer(ResyncPolicy* p, int port, int32_t s, bool ok) {
    if (p->state != RESYNC_HOLD || s != p->frame || port < 0 || port > 7) {
        return; /* an answer for an older snapshot, or nobody's */
    }
    const uint8_t bit = (uint8_t)(1u << port);
    p->waiting &= (uint8_t)~bit;
    if (ok) {
        p->applied |= bit;
    }
}

bool resync_policy_release(ResyncPolicy* p, uint8_t live, int32_t held_ms) {
    if (p->state != RESYNC_HOLD) {
        return true;
    }
    if ((p->waiting & live) != 0 && held_ms < RESYNC_HOLD_MS) {
        return false;
    }
    p->state = RESYNC_IDLE;
    p->asked = -1;
    return true;
}

void resync_msg_encode(uint8_t out[RESYNC_MSG_LEN], int32_t frame, uint32_t size, uint8_t nth) {
    put32(out, (uint32_t)frame);
    put32(out + 4, size);
    out[8] = nth;
}

bool resync_msg_decode(const uint8_t* in, int len, int32_t* frame, uint32_t* size, uint8_t* nth) {
    if (in == NULL || len != RESYNC_MSG_LEN) {
        return false;
    }
    *frame = (int32_t)get32(in);
    *size = get32(in + 4);
    *nth = in[8];
    return *frame >= 0;
}

void resync_answer_encode(uint8_t out[RESYNC_ANSWER_LEN], int32_t frame, uint8_t result) {
    put32(out, (uint32_t)frame);
    out[4] = result;
}

bool resync_answer_decode(const uint8_t* in, int len, int32_t* frame, uint8_t* result) {
    if (in == NULL || len != RESYNC_ANSWER_LEN) {
        return false;
    }
    *frame = (int32_t)get32(in);
    *result = in[4];
    return *frame >= 0;
}

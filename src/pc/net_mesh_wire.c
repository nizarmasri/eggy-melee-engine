/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The mesh session's wire format and rules (src/pc/net_mesh_wire.h). Pure. */
#include "pc/net_mesh_wire.h"

#include <string.h>

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get16(const uint8_t* p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void hdr_put(uint8_t* p, const MeshHdr* h) {
    p[0] = h->magic;
    p[1] = h->version;
    put32(p + 2, h->session);
    p[6] = h->from;
    p[7] = h->to;
}

bool mesh_hdr_decode(const uint8_t* in, size_t len, MeshHdr* h) {
    if (in == NULL || len < MESH_HDR_LEN) {
        return false;
    }
    h->magic = in[0];
    h->version = in[1];
    h->session = get32(in + 2);
    h->from = in[6];
    h->to = in[7];
    return h->from < MESH_PORTS && h->to < MESH_PORTS;
}

static void pad_put(uint8_t* p, const MeshPad* pad) {
    put16(p, pad->button);
    p[2] = (uint8_t)pad->stickX;
    p[3] = (uint8_t)pad->stickY;
    p[4] = (uint8_t)pad->substickX;
    p[5] = (uint8_t)pad->substickY;
    p[6] = pad->triggerLeft;
    p[7] = pad->triggerRight;
}

static void pad_get(const uint8_t* p, MeshPad* pad) {
    pad->button = get16(p);
    pad->stickX = (int8_t)p[2];
    pad->stickY = (int8_t)p[3];
    pad->substickX = (int8_t)p[4];
    pad->substickY = (int8_t)p[5];
    pad->triggerLeft = p[6];
    pad->triggerRight = p[7];
}

size_t mesh_input_len(const MeshInput* pk) {
    size_t n = MESH_INPUT_FIXED;
    for (int i = 0; i < pk->nsec && i < MESH_SECTIONS; i++) {
        n += MESH_SECTION_FIXED + (size_t)pk->sec[i].count * sizeof(MeshPad);
    }
    return n;
}

size_t mesh_input_encode(const MeshInput* pk, uint8_t* out, size_t cap) {
    if (pk->nsec > MESH_SECTIONS) {
        return 0;
    }
    for (int i = 0; i < pk->nsec; i++) {
        if (pk->sec[i].count > MESH_RED || pk->sec[i].port >= MESH_PORTS) {
            return 0;
        }
    }
    const size_t n = mesh_input_len(pk);
    if (n > cap || n > MESH_DGRAM_MAX) {
        return 0;
    }
    hdr_put(out, &pk->h);
    put16(out + 8, pk->seq);
    put16(out + 10, pk->echo);
    put16(out + 12, pk->held);
    out[14] = pk->flags;
    out[15] = (uint8_t)pk->adv;
    for (int p = 0; p < MESH_PORTS; p++) {
        put32(out + 16 + 4 * p, (uint32_t)pk->have[p]);
    }
    put32(out + 32, (uint32_t)pk->ck_frame);
    put32(out + 36, pk->ck);
    out[40] = pk->nsec;
    size_t at = MESH_INPUT_FIXED;
    for (int i = 0; i < pk->nsec; i++) {
        const MeshSection* s = &pk->sec[i];
        out[at] = s->port;
        put32(out + at + 1, (uint32_t)s->first);
        out[at + 5] = s->count;
        at += MESH_SECTION_FIXED;
        for (int f = 0; f < s->count; f++) {
            pad_put(out + at, &s->pads[f]);
            at += sizeof(MeshPad);
        }
    }
    return n;
}

bool mesh_input_decode(const uint8_t* in, size_t len, MeshInput* out) {
    if (!mesh_hdr_decode(in, len, &out->h) || out->h.magic != MESH_INPUT || len < MESH_INPUT_FIXED)
    {
        return false;
    }
    out->seq = get16(in + 8);
    out->echo = get16(in + 10);
    out->held = get16(in + 12);
    out->flags = in[14];
    out->adv = (int8_t)in[15];
    for (int p = 0; p < MESH_PORTS; p++) {
        out->have[p] = (int32_t)get32(in + 16 + 4 * p);
    }
    out->ck_frame = (int32_t)get32(in + 32);
    out->ck = get32(in + 36);
    out->nsec = in[40];
    if (out->nsec > MESH_SECTIONS) {
        return false;
    }
    size_t at = MESH_INPUT_FIXED;
    for (int i = 0; i < out->nsec; i++) {
        MeshSection* s = &out->sec[i];
        if (at + MESH_SECTION_FIXED > len) {
            return false;
        }
        s->port = in[at];
        s->first = (int32_t)get32(in + at + 1);
        s->count = in[at + 5];
        at += MESH_SECTION_FIXED;
        if (s->port >= MESH_PORTS || s->count > MESH_RED || s->first < 0 ||
            at + (size_t)s->count * sizeof(MeshPad) > len)
        {
            return false;
        }
        for (int f = 0; f < s->count; f++) {
            pad_get(in + at, &s->pads[f]);
            at += sizeof(MeshPad);
        }
    }
    return at == len;
}

size_t mesh_rel_encode(const MeshRel* r, uint8_t* out, size_t cap) {
    const size_t n = MESH_REL_FIXED + (size_t)r->len;
    if (r->len > MESH_REL_MAX || n > cap || n > MESH_DGRAM_MAX) {
        return 0;
    }
    hdr_put(out, &r->h);
    put16(out + 8, r->seq);
    out[10] = r->type;
    put16(out + 11, r->len);
    memcpy(out + MESH_REL_FIXED, r->payload, r->len);
    return n;
}

bool mesh_rel_decode(const uint8_t* in, size_t len, MeshRel* out) {
    if (!mesh_hdr_decode(in, len, &out->h) || out->h.magic != MESH_REL || len < MESH_REL_FIXED) {
        return false;
    }
    out->seq = get16(in + 8);
    out->type = in[10];
    out->len = get16(in + 11);
    if (out->len > MESH_REL_MAX || MESH_REL_FIXED + (size_t)out->len != len) {
        return false;
    }
    memcpy(out->payload, in + MESH_REL_FIXED, out->len);
    return true;
}

size_t mesh_small_encode(const MeshHdr* h, uint16_t value, uint8_t* out, size_t cap) {
    if (cap < MESH_HDR_LEN + 2) {
        return 0;
    }
    hdr_put(out, h);
    put16(out + MESH_HDR_LEN, value);
    return MESH_HDR_LEN + 2;
}

bool mesh_small_decode(const uint8_t* in, size_t len, MeshHdr* h, uint16_t* value) {
    if (len != MESH_HDR_LEN + 2 || !mesh_hdr_decode(in, len, h) ||
        (h->magic != MESH_ACK && h->magic != MESH_BYE))
    {
        return false;
    }
    *value = get16(in + MESH_HDR_LEN);
    return true;
}

size_t mesh_addr_wrap(uint8_t port, const uint8_t* dgram, size_t len, uint8_t* out, size_t cap) {
    if (port >= MESH_PORTS || len == 0 || len + 1 > MESH_SLOT_BYTES || len + 1 > cap) {
        return 0;
    }
    out[0] = port;
    memcpy(out + 1, dgram, len);
    return len + 1;
}

bool mesh_addr_unwrap(
    const uint8_t* in, size_t len, uint8_t* port, const uint8_t** dgram, size_t* dgram_len) {
    if (in == NULL || len < 2 || len > MESH_SLOT_BYTES || in[0] >= MESH_PORTS) {
        return false;
    }
    *port = in[0];
    *dgram = in + 1;
    *dgram_len = len - 1;
    return true;
}

bool mesh_relay_wanted(int32_t mine, int32_t theirs, bool settled, int32_t quiet_ms) {
    if (mine <= theirs) {
        return false;
    }
    return (int64_t)mine - theirs >= MESH_RELAY_LAG || settled || quiet_ms >= MESH_RELAY_QUIET_MS;
}

int mesh_away_accept(uint8_t settled, int self, int from, int port) {
    if (from < 0 || from >= MESH_PORTS || port < 0 || port >= MESH_PORTS || from == self ||
        (settled & (1u << from)) != 0)
    {
        return MESH_AWAY_IGNORE;
    }
    if (port == self) {
        return MESH_AWAY_EVICTED;
    }
    return (settled & (1u << port)) != 0 ? MESH_AWAY_IGNORE : MESH_AWAY_SETTLE;
}

int mesh_leader(uint8_t humans, uint8_t settled, uint8_t silent, int self) {
    uint8_t live = (uint8_t)(humans & ~settled & ~silent);
    if (self >= 0 && self < MESH_PORTS) {
        live |= (uint8_t)(1u << self);
    }
    for (int p = 0; p < MESH_PORTS; p++) {
        if (live & (1u << p)) {
            return p;
        }
    }
    return -1;
}

bool mesh_settle_ready(int32_t mine, const int32_t* reports, uint8_t live_mask, int32_t* frame) {
    int32_t most = mine;
    for (int q = 0; q < MESH_PORTS; q++) {
        if ((live_mask & (1u << q)) && reports[q] > most) {
            most = reports[q];
        }
    }
    if (mine < most) {
        return false; /* another live phone holds more: wait for its relay */
    }
    *frame = mine;
    return true;
}

int mesh_silence_limit_ms(int32_t its_newest, int32_t scene_first, int away_ms, int load_ms) {
    return its_newest < scene_first ? load_ms : away_ms;
}

void mesh_away_encode(uint8_t out[MESH_AWAY_LEN], uint8_t port, int32_t frame) {
    out[0] = port;
    put32(out + 1, (uint32_t)frame);
}

bool mesh_away_decode(const uint8_t* in, int len, uint8_t* port, int32_t* frame) {
    if (in == NULL || len != MESH_AWAY_LEN || in[0] >= MESH_PORTS) {
        return false;
    }
    *port = in[0];
    *frame = (int32_t)get32(in + 1);
    return *frame >= -1;
}

uint8_t mesh_silent_mask(const MeshAwayView* v) {
    uint8_t silent = 0;
    for (int p = 0; p < MESH_PORTS; p++) {
        const uint8_t bit = (uint8_t)(1u << p);
        if (p == v->self || (v->humans & bit) == 0 || (v->settled & bit) != 0) {
            continue;
        }
        const int32_t limit = v->loading[p] ? v->load_ms : v->away_ms;
        if (v->bye[p] || v->quiet_ms[p] >= limit) {
            silent |= bit;
        }
    }
    return silent;
}

int mesh_away_decide(const MeshAwayView* v, int32_t* frame) {
    const uint8_t silent = mesh_silent_mask(v);
    if (silent == 0 || mesh_leader(v->humans, v->settled, silent, v->self) != v->self) {
        return -1;
    }
    const uint8_t live = (uint8_t)(v->humans & ~v->settled & ~silent & ~(1u << v->self));
    for (int p = 0; p < MESH_PORTS; p++) {
        if ((silent & (1u << p)) == 0 || v->stable_ms[p] < v->stable_need_ms) {
            continue;
        }
        int32_t reports[MESH_PORTS];
        for (int q = 0; q < MESH_PORTS; q++) {
            reports[q] = v->reports[q][p];
        }
        if (mesh_settle_ready(v->have[p], reports, live, frame)) {
            return p;
        }
    }
    return -1;
}

void mesh_m2_probe(uint8_t out[MESH_M2_PROBE_LEN], uint32_t session, uint8_t player) {
    memset(out, 0, MESH_M2_PROBE_LEN);
    out[0] = MESH_BYE;
    out[1] = MESH_VERSION;
    put32(out + 2, session);
    out[6] = player;
    out[7] = 4; /* PC_NET_PEER_INCOMPATIBLE; the 8 bytes after are its (absent) MAC */
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The pure part of MELEE_EGGY netplay (src/pc/eggy/eggy_net.h): the hold's
 * rules and the barrier payload. No game or session state, so
 * tests/eggy/eggy_net_test.c builds it with the host compiler.
 */
#include "pc/eggy/eggy_net.h"

#include <string.h>

/* 'EGGY': what a 0 stands for, since net.c reads 0 as "none". */
#define EGGY_NONZERO 0x45474759u

static void be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void eggy_hello_encode(uint8_t out[EGGY_HELLO_LEN], uint32_t config_hash, uint32_t session) {
    out[0] = 'E';
    out[1] = 'G';
    out[2] = 'G';
    out[3] = 1;
    be32(out + 4, config_hash);
    be32(out + 8, session);
}

bool eggy_hello_matches(const uint8_t* in, int len, uint32_t config_hash, uint32_t session) {
    uint8_t want[EGGY_HELLO_LEN];
    eggy_hello_encode(want, config_hash, session);
    return in != NULL && len == EGGY_HELLO_LEN && memcmp(in, want, sizeof want) == 0;
}

uint32_t eggy_net_session_id(const EggyConfig* c) {
    return c->session != 0 ? c->session : EGGY_NONZERO;
}

uint32_t eggy_net_seed(const EggyConfig* c) {
    return c->seed != 0 ? c->seed : EGGY_NONZERO;
}

void eggy_hold_init(EggyHold* h, bool leader) {
    memset(h, 0, sizeof *h);
    h->leader = leader;
}

int eggy_hold_next(const EggyHold* h, bool go) {
    if (!h->sent_loaded) {
        return EGGY_HOLD_SEND_LOADED;
    }
    if (!h->reported) {
        return h->peer_loaded ? EGGY_HOLD_REPORT : EGGY_HOLD_WAIT;
    }
    if (h->leader) {
        if (h->sent_go) {
            return EGGY_HOLD_RELEASE;
        }
        return go && h->peer_armed ? EGGY_HOLD_SEND_GO : EGGY_HOLD_WAIT;
    }
    if (!h->sent_armed) {
        return go ? EGGY_HOLD_SEND_ARMED : EGGY_HOLD_WAIT;
    }
    return h->peer_go ? EGGY_HOLD_RELEASE : EGGY_HOLD_WAIT;
}

void eggy_hold_done(EggyHold* h, int action) {
    switch (action) {
    case EGGY_HOLD_SEND_LOADED:
        h->sent_loaded = true;
        break;
    case EGGY_HOLD_REPORT:
        h->reported = true;
        break;
    case EGGY_HOLD_SEND_ARMED:
        h->sent_armed = true;
        break;
    case EGGY_HOLD_SEND_GO:
        h->sent_go = true;
        break;
    default:
        break;
    }
}

void eggy_hold_receive(EggyHold* h, int type) {
    switch (type) {
    case EGGY_REL_LOADED:
        h->peer_loaded = true;
        break;
    case EGGY_REL_ARMED:
        if (h->leader) {
            h->peer_armed = true;
        }
        break;
    case EGGY_REL_GO:
        if (!h->leader) {
            h->peer_go = true;
        }
        break;
    default:
        break;
    }
}

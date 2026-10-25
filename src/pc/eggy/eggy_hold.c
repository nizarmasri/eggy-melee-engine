/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The pure part of MELEE_EGGY netplay (src/pc/eggy/eggy_net.h): the hold's
 * rules and the HELLO payload. No game or session state, so
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

void eggy_hold_init(EggyHold* h, uint8_t humans, int self) {
    memset(h, 0, sizeof *h);
    h->humans = humans;
    h->self = self;
}

int eggy_hold_next(const EggyHold* h, bool go, uint8_t away, int leader) {
    const uint8_t others = (uint8_t)(h->humans & ~away & ~(1u << h->self));
    if (!h->sent_loaded) {
        return EGGY_HOLD_SEND_LOADED;
    }
    if (!h->reported) {
        return (h->loaded & others) == others ? EGGY_HOLD_REPORT : EGGY_HOLD_WAIT;
    }
    if (!h->sent_armed) {
        return go ? EGGY_HOLD_SEND_ARMED : EGGY_HOLD_WAIT;
    }
    if (h->sent_go || h->got_go) {
        return EGGY_HOLD_RELEASE;
    }
    if (leader == h->self && (h->armed & others) == others) {
        return EGGY_HOLD_SEND_GO;
    }
    return EGGY_HOLD_WAIT;
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

void eggy_hold_receive(EggyHold* h, int type, int from) {
    if (from < 0 || from >= EGGY_PORTS || from == h->self || (h->humans & (1u << from)) == 0) {
        return;
    }
    const uint8_t bit = (uint8_t)(1u << from);
    switch (type) {
    case EGGY_REL_LOADED:
        h->loaded |= bit;
        break;
    case EGGY_REL_ARMED:
        h->armed |= bit;
        break;
    case EGGY_REL_GO:
        h->got_go = true;
        break;
    default:
        break;
    }
}

void eggy_hold_forget(EggyHold* h, int port) {
    if (port < 0 || port >= EGGY_PORTS || port == h->self) {
        return;
    }
    h->loaded &= (uint8_t)~(1u << port);
    h->armed &= (uint8_t)~(1u << port);
}

int eggy_hold_resend(const EggyHold* h, uint8_t out[2]) {
    int n = 0;
    if (h->sent_loaded) {
        out[n++] = EGGY_REL_LOADED;
    }
    if (h->sent_armed) {
        out[n++] = EGGY_REL_ARMED;
    }
    return n;
}

bool eggy_net_agreed(bool hello_sent, uint8_t hello_got, uint8_t others, uint8_t away) {
    return hello_sent && (others & (uint8_t)~away) != 0 && ((hello_got | away) & others) == others;
}

int eggy_go_hold_ms(const int32_t ping_ms[EGGY_PORTS], uint8_t ports) {
    int32_t worst = 0;
    for (int port = 0; port < EGGY_PORTS; port++) {
        if ((ports & (1u << port)) && ping_ms[port] > worst) {
            worst = ping_ms[port];
        }
    }
    const int32_t hold = (worst + 1) / 2;
    return hold > EGGY_GO_HOLD_MAX_MS ? EGGY_GO_HOLD_MAX_MS : (int)hold;
}

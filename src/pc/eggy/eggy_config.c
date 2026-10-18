/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The Eggy config buffer, layout v1 (src/pc/eggy/eggy.h), checked word by
 * word. Pure: no game state, so tests/eggy/eggy_config_test.c builds it with
 * the host compiler. The first invalid word is reported as its index + 1, so
 * an error code points at the exact word the page got wrong.
 */
#include "pc/eggy/eggy.h"

#include <stddef.h>

bool eggy_stage_valid(int32_t stkind) {
    /* Fountain of Dreams (2) .. Final Destination (32): every VS stage. 21 is
     * Akaneia, never finished, and 26 is the top of Icicle Mountain, which
     * only Adventure uses. */
    return stkind >= 2 && stkind <= 32 && stkind != 21 && stkind != 26;
}

static bool in_range(int32_t v, int32_t lo, int32_t hi) {
    return v >= lo && v <= hi;
}

static bool is_flag(int32_t v) {
    return v == 0 || v == 1;
}

uint32_t eggy_config_hash(const int32_t* w) {
    uint32_t h = 2166136261u; /* FNV-1a, each word's bytes low first */
    for (int i = 0; i < EGGY_CONFIG_WORDS; i++) {
        if ((i >= 12 && i <= 14) || i == 49) {
            continue; /* this device's sound, this phone's port */
        }
        const uint32_t v = (uint32_t)w[i];
        for (int b = 0; b < 4; b++) {
            h ^= (v >> (8 * b)) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h;
}

/* Words 48..63 with netplay on: two to four humans, one per phone. */
static int parse_netplay(const int32_t* w, EggyConfig* c) {
    uint8_t humans = 0;
    int n = 0, lowest = -1;
    for (int port = 0; port < EGGY_PORTS; port++) {
        if (c->ports[port].kind == EGGY_PORT_HUMAN) {
            humans |= (uint8_t)(1u << port);
            lowest = lowest < 0 ? port : lowest;
            n++;
        }
    }
    if (!in_range(w[49], 0, EGGY_PORTS - 1)) {
        return EGGY_ERR_FIELD(49);
    }
    if (!in_range(w[50], EGGY_NET_PHONES_MIN, EGGY_NET_PHONES_MAX)) {
        return EGGY_ERR_FIELD(50);
    }
    if (n < EGGY_NET_PHONES_MIN || n > EGGY_NET_PHONES_MAX) {
        return EGGY_ERR_NET_HUMANS;
    }
    if (w[50] != n) {
        return EGGY_ERR_FIELD(50); /* one phone per human */
    }
    if ((humans & (1u << w[49])) == 0) {
        return EGGY_ERR_FIELD(49); /* this phone's port must be one of the humans */
    }
    if (w[51] != lowest) {
        return EGGY_ERR_FIELD(51); /* the leader is the lowest human port */
    }
    if (!in_range(w[53], 0, 4)) {
        return EGGY_ERR_FIELD(53);
    }
    for (int i = 54; i < EGGY_CONFIG_WORDS; i++) {
        if (w[i] != 0) {
            return EGGY_ERR_FIELD(i);
        }
    }
    c->local_port = w[49];
    c->leader_port = lowest;
    c->humans = humans;
    c->phones = n;
    c->session = (uint32_t)w[52];
    c->input_delay = w[53];
    return 0;
}

int eggy_config_parse(const int32_t* w, const uint8_t* costumes, EggyConfig* out) {
    EggyConfig c = {0};
    int fighters = 0;
    unsigned teams_seen = 0;

    if (w[0] != EGGY_CONFIG_VERSION) {
        return EGGY_ERR_FIELD(0);
    }
    c.seed = (uint32_t)w[1];
    if (!eggy_stage_valid(w[2])) {
        return EGGY_ERR_FIELD(2);
    }
    c.stage = w[2];
    if (w[3] != EGGY_MATCH_STOCK && w[3] != EGGY_MATCH_TIME) {
        return EGGY_ERR_FIELD(3);
    }
    c.match_kind = w[3];
    if (!in_range(w[4], c.match_kind == EGGY_MATCH_STOCK ? 1 : 0, 99)) {
        return EGGY_ERR_FIELD(4);
    }
    c.stocks = w[4];
    if (!in_range(w[5], 0, 99)) {
        return EGGY_ERR_FIELD(5);
    }
    c.time_minutes = w[5];
    if (!in_range(w[6], -1, 4)) {
        return EGGY_ERR_FIELD(6);
    }
    c.item_freq = w[6];
    c.item_mask = (uint64_t)(uint32_t)w[8] << 32 | (uint32_t)w[7];
    for (int i = 9; i <= 12; i++) {
        if (!is_flag(w[i])) {
            return EGGY_ERR_FIELD(i);
        }
    }
    c.teams = w[9] != 0;
    c.friendly_fire = w[10] != 0;
    c.frozen_stadium = w[11] != 0;
    c.music = w[12] != 0;
    for (int i = 13; i <= 14; i++) {
        if (!in_range(w[i], 0, 127)) {
            return EGGY_ERR_FIELD(i);
        }
    }
    c.music_volume = w[13];
    c.sfx_volume = w[14];
    if (!in_range(w[15], 0, 600)) {
        return EGGY_ERR_FIELD(15);
    }
    c.results_seconds = w[15];

    for (int port = 0; port < EGGY_PORTS; port++) {
        const int base = EGGY_CONFIG_PORT_BASE + port * EGGY_CONFIG_PORT_WORDS;
        const int32_t* p = &w[base];
        EggyPort* dst = &c.ports[port];
        if (p[0] == EGGY_PORT_NONE) {
            continue; /* an empty port's other words are not read */
        }
        if (p[0] != EGGY_PORT_HUMAN && p[0] != EGGY_PORT_CPU) {
            return EGGY_ERR_FIELD(base);
        }
        if (!in_range(p[1], 0, EGGY_CKIND_COUNT - 1)) {
            return EGGY_ERR_FIELD(base + 1);
        }
        if (!in_range(p[2], 0, (int32_t)costumes[p[1]] - 1)) {
            return EGGY_ERR_FIELD(base + 2);
        }
        if (!in_range(p[3], 0, 2)) {
            return EGGY_ERR_FIELD(base + 3);
        }
        if (!in_range(p[4], p[0] == EGGY_PORT_CPU ? 1 : 0, 9)) {
            return EGGY_ERR_FIELD(base + 4);
        }
        if (!in_range(p[5], 0, 9)) {
            return EGGY_ERR_FIELD(base + 5);
        }
        for (int r = 6; r < EGGY_CONFIG_PORT_WORDS; r++) {
            if (p[r] != 0) {
                return EGGY_ERR_FIELD(base + r);
            }
        }
        dst->kind = p[0];
        dst->ckind = p[1];
        dst->costume = p[2];
        dst->team = p[3];
        dst->cpu_level = p[4];
        dst->handicap = p[5];
        fighters++;
        teams_seen |= 1u << p[3];
    }
    if (fighters < 2) {
        return EGGY_ERR_FIGHTERS;
    }
    if (c.teams && (teams_seen & (teams_seen - 1)) == 0) {
        return EGGY_ERR_TEAMS;
    }
    if (!is_flag(w[48])) {
        return EGGY_ERR_FIELD(48);
    }
    c.netplay = w[48] != 0;
    if (c.netplay) {
        const int err = parse_netplay(w, &c); /* solo: words 49..63 are not read */
        if (err != 0) {
            return err;
        }
    }
    c.hash = eggy_config_hash(w);
    *out = c;
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Eggy config buffer's validation (src/pc/eggy/eggy_config.c), built with
 * the host compiler by .github/workflows/ci.yml:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -Isrc tests/eggy/eggy_config_test.c \
 *       src/pc/eggy/eggy_config.c -o eggy_config_test && ./eggy_config_test
 */
#include "pc/eggy/eggy.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);               \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* Five costumes for everyone but a few, like the game's table. */
static uint8_t costumes[EGGY_CKIND_COUNT];

static void base(int32_t* w) {
    memset(w, 0, EGGY_CONFIG_WORDS * sizeof *w);
    w[0] = 1;
    w[1] = (int32_t)0xDEADBEEFu;
    w[2] = 32; /* Final Destination */
    w[3] = EGGY_MATCH_STOCK;
    w[4] = 4;
    w[5] = 8;
    w[6] = -1;
    w[7] = -1;
    w[8] = -1;
    w[10] = 1;
    w[11] = 1;
    w[12] = 1;
    w[13] = 127;
    w[14] = 100;
    w[15] = 0;
    int32_t* p0 = &w[EGGY_CONFIG_PORT_BASE];
    p0[0] = EGGY_PORT_HUMAN;
    p0[1] = 2; /* Fox */
    p0[2] = 1;
    p0[3] = 0;
    p0[4] = 0;
    int32_t* p1 = &w[EGGY_CONFIG_PORT_BASE + EGGY_CONFIG_PORT_WORDS];
    p1[0] = EGGY_PORT_CPU;
    p1[1] = 20; /* Falco */
    p1[2] = 3;
    p1[3] = 1;
    p1[4] = 9;
    p1[5] = 7;
}

static int parse(const int32_t* w, EggyConfig* out) {
    return eggy_config_parse(w, costumes, out);
}

/* Set word `index` to `value` on a valid config and expect `code`. */
static void expect(int index, int32_t value, int code) {
    int32_t w[EGGY_CONFIG_WORDS];
    EggyConfig c;
    EggyConfig before;
    memset(&c, 0x5A, sizeof c);
    memcpy(&before, &c, sizeof c);
    base(w);
    w[index] = value;
    const int got = parse(w, &c);
    if (got != code) {
        fprintf(stderr, "word %d = %d: expected %d, got %d\n", index, value, code, got);
        failures++;
    }
    if (code != 0) {
        CHECK(memcmp(&c, &before, sizeof c) == 0); /* *out untouched on error */
    }
}

static void test_valid(void) {
    int32_t w[EGGY_CONFIG_WORDS];
    EggyConfig c;
    base(w);
    CHECK(parse(w, &c) == 0);
    CHECK(c.seed == 0xDEADBEEFu);
    CHECK(c.stage == 32);
    CHECK(c.match_kind == EGGY_MATCH_STOCK);
    CHECK(c.stocks == 4);
    CHECK(c.time_minutes == 8);
    CHECK(c.item_freq == -1);
    CHECK(c.item_mask == UINT64_MAX);
    CHECK(!c.teams && c.friendly_fire && c.frozen_stadium && c.music);
    CHECK(c.music_volume == 127 && c.sfx_volume == 100 && c.results_seconds == 0);
    CHECK(c.ports[0].kind == EGGY_PORT_HUMAN && c.ports[0].ckind == 2 && c.ports[0].costume == 1);
    CHECK(c.ports[1].kind == EGGY_PORT_CPU && c.ports[1].ckind == 20 && c.ports[1].team == 1);
    CHECK(c.ports[1].cpu_level == 9 && c.ports[1].handicap == 7);
    CHECK(c.ports[2].kind == EGGY_PORT_NONE && c.ports[3].kind == EGGY_PORT_NONE);
}

static void test_fields(void) {
    expect(0, 2, EGGY_ERR_FIELD(0));
    expect(0, 0, EGGY_ERR_FIELD(0));
    /* Every VS stage, and nothing else. */
    for (int32_t st = -1; st <= 40; st++) {
        const bool vs = st >= 2 && st <= 32 && st != 21 && st != 26;
        CHECK(eggy_stage_valid(st) == vs);
        expect(2, st, vs ? 0 : EGGY_ERR_FIELD(2));
    }
    expect(3, 2, EGGY_ERR_FIELD(3));
    expect(3, -1, EGGY_ERR_FIELD(3));
    expect(4, 0, EGGY_ERR_FIELD(4)); /* a stock match needs a stock */
    expect(4, 99, 0);
    expect(4, 100, EGGY_ERR_FIELD(4));
    expect(5, 0, 0);
    expect(5, 99, 0);
    expect(5, 100, EGGY_ERR_FIELD(5));
    expect(5, -1, EGGY_ERR_FIELD(5));
    for (int32_t f = -3; f <= 6; f++) {
        expect(6, f, f >= -1 && f <= 4 ? 0 : EGGY_ERR_FIELD(6));
    }
    for (int i = 9; i <= 12; i++) {
        expect(i, 1, 0);
        expect(i, 2, EGGY_ERR_FIELD(i));
        expect(i, -1, EGGY_ERR_FIELD(i));
    }
    expect(13, 128, EGGY_ERR_FIELD(13));
    expect(14, -1, EGGY_ERR_FIELD(14));
    expect(15, 600, 0);
    expect(15, 601, EGGY_ERR_FIELD(15));
    expect(15, -1, EGGY_ERR_FIELD(15));
}

static void test_time_match(void) {
    int32_t w[EGGY_CONFIG_WORDS];
    EggyConfig c;
    base(w);
    w[3] = EGGY_MATCH_TIME;
    w[4] = 0; /* stocks are not read in a time match */
    w[5] = 0; /* and no clock at all is allowed */
    CHECK(parse(w, &c) == 0);
    CHECK(c.match_kind == EGGY_MATCH_TIME && c.time_minutes == 0);
}

static void test_item_mask(void) {
    int32_t w[EGGY_CONFIG_WORDS];
    EggyConfig c;
    base(w);
    w[7] = 1;
    w[8] = 2;
    CHECK(parse(w, &c) == 0);
    CHECK(c.item_mask == 0x0000000200000001ull);
    w[7] = (int32_t)0x80000000u;
    w[8] = 0;
    CHECK(parse(w, &c) == 0);
    CHECK(c.item_mask == 0x80000000ull);
}

static void test_ports(void) {
    const int p = EGGY_CONFIG_PORT_BASE; /* port 0 */
    expect(p, 2, EGGY_ERR_FIELD(p));
    expect(p, 4, EGGY_ERR_FIELD(p));
    expect(p + 1, -1, EGGY_ERR_FIELD(p + 1));
    expect(p + 1, 26, EGGY_ERR_FIELD(p + 1));
    expect(p + 1, 19, 0); /* Sheik */
    expect(p + 2, 4, 0);
    expect(p + 2, 5, EGGY_ERR_FIELD(p + 2));
    expect(p + 2, -1, EGGY_ERR_FIELD(p + 2));
    expect(p + 3, 2, 0);
    expect(p + 3, 3, EGGY_ERR_FIELD(p + 3));
    expect(p + 4, 9, 0); /* a human's CPU level is accepted and ignored */
    expect(p + 4, 10, EGGY_ERR_FIELD(p + 4));
    expect(p + 5, 9, 0);
    expect(p + 5, 10, EGGY_ERR_FIELD(p + 5));
    expect(p + 5, -1, EGGY_ERR_FIELD(p + 5));
    expect(p + 6, 1, EGGY_ERR_FIELD(p + 6));
    expect(p + 7, -1, EGGY_ERR_FIELD(p + 7));

    const int q = EGGY_CONFIG_PORT_BASE + EGGY_CONFIG_PORT_WORDS; /* port 1, a CPU */
    expect(q + 4, 0, EGGY_ERR_FIELD(q + 4));
    expect(q + 4, 1, 0);

    /* Costumes follow the character's own count. */
    expect(p + 2, 2, 0);
    {
        int32_t w[EGGY_CONFIG_WORDS];
        EggyConfig c;
        base(w);
        w[p + 1] = 25; /* three costumes in this table */
        w[p + 2] = 2;
        CHECK(parse(w, &c) == 0);
        w[p + 2] = 3;
        CHECK(parse(w, &c) == EGGY_ERR_FIELD(p + 2));
    }

    /* An empty port's words are never read. */
    {
        int32_t w[EGGY_CONFIG_WORDS];
        EggyConfig c;
        base(w);
        int32_t* empty = &w[EGGY_CONFIG_PORT_BASE + 3 * EGGY_CONFIG_PORT_WORDS];
        for (int i = 1; i < EGGY_CONFIG_PORT_WORDS; i++) {
            empty[i] = -12345;
        }
        CHECK(parse(w, &c) == 0);
        CHECK(c.ports[3].kind == EGGY_PORT_NONE && c.ports[3].ckind == 0);
    }
}

static void test_fighters_and_teams(void) {
    int32_t w[EGGY_CONFIG_WORDS];
    EggyConfig c;

    base(w);
    w[EGGY_CONFIG_PORT_BASE + EGGY_CONFIG_PORT_WORDS] = EGGY_PORT_NONE;
    CHECK(parse(w, &c) == EGGY_ERR_FIGHTERS);
    w[EGGY_CONFIG_PORT_BASE] = EGGY_PORT_NONE;
    CHECK(parse(w, &c) == EGGY_ERR_FIGHTERS);

    /* Four fighters, CPUs on ports 3 and 4. */
    base(w);
    for (int port = 2; port < 4; port++) {
        int32_t* p = &w[EGGY_CONFIG_PORT_BASE + port * EGGY_CONFIG_PORT_WORDS];
        p[0] = EGGY_PORT_CPU;
        p[1] = 9 + port;
        p[2] = 0;
        p[3] = 0;
        p[4] = 3;
    }
    CHECK(parse(w, &c) == 0);
    CHECK(c.ports[3].kind == EGGY_PORT_CPU && c.ports[3].ckind == 12 && c.ports[3].cpu_level == 3);

    /* Teams: two teams at least. */
    base(w);
    w[9] = 1;
    w[EGGY_CONFIG_PORT_BASE + EGGY_CONFIG_PORT_WORDS + 3] = 0;
    CHECK(parse(w, &c) == EGGY_ERR_TEAMS);
    w[EGGY_CONFIG_PORT_BASE + EGGY_CONFIG_PORT_WORDS + 3] = 2;
    CHECK(parse(w, &c) == 0);
    CHECK(c.teams);
    /* Without teams, one colour of team is fine. */
    w[9] = 0;
    w[EGGY_CONFIG_PORT_BASE + EGGY_CONFIG_PORT_WORDS + 3] = 0;
    CHECK(parse(w, &c) == 0);
}

/* Netplay: Fox on port 1 (this phone, the leader), the CPU Falco on port 2
 * and a human Captain Falcon on port 3, the other phone. */
static void net_base(int32_t* w) {
    base(w);
    int32_t* p2 = &w[EGGY_CONFIG_PORT_BASE + 2 * EGGY_CONFIG_PORT_WORDS];
    p2[0] = EGGY_PORT_HUMAN;
    p2[1] = 0;
    p2[2] = 2;
    p2[3] = 2;
    w[48] = 1;
    w[49] = 0;
    w[50] = 2;
    w[51] = 0;
    w[52] = (int32_t)0xC0FFEE01u;
    w[53] = 0;
}

static void net_expect(int index, int32_t value, int code) {
    int32_t w[EGGY_CONFIG_WORDS];
    EggyConfig c;
    net_base(w);
    w[index] = value;
    const int got = parse(w, &c);
    if (got != code) {
        fprintf(stderr, "netplay word %d = %d: expected %d, got %d\n", index, value, code, got);
        failures++;
    }
}

static void test_netplay(void) {
    int32_t w[EGGY_CONFIG_WORDS];
    EggyConfig c;
    net_base(w);
    CHECK(parse(w, &c) == 0);
    CHECK(c.netplay && c.local_port == 0 && c.leader_port == 0 && c.guest_port == 2);
    CHECK(c.session == 0xC0FFEE01u && c.input_delay == 0);
    w[49] = 2; /* the other phone's view of the same match */
    CHECK(parse(w, &c) == 0);
    CHECK(c.local_port == 2 && c.leader_port == 0 && c.guest_port == 2);

    net_expect(48, 2, EGGY_ERR_FIELD(48));
    net_expect(48, -1, EGGY_ERR_FIELD(48));
    net_expect(49, 1, EGGY_ERR_FIELD(49)); /* the CPU's port */
    net_expect(49, 3, EGGY_ERR_FIELD(49)); /* nobody's */
    net_expect(49, 4, EGGY_ERR_FIELD(49));
    net_expect(49, -1, EGGY_ERR_FIELD(49));
    net_expect(50, 1, EGGY_ERR_FIELD(50));
    net_expect(50, 3, EGGY_ERR_FIELD(50));
    net_expect(51, 2, EGGY_ERR_FIELD(51)); /* the leader is the lowest human */
    net_expect(51, 1, EGGY_ERR_FIELD(51));
    net_expect(52, 0, 0);
    net_expect(52, -1, 0);
    for (int32_t d = -1; d <= 5; d++) {
        net_expect(53, d, d >= 0 && d <= 4 ? 0 : EGGY_ERR_FIELD(53));
    }
    for (int i = 54; i < EGGY_CONFIG_WORDS; i++) {
        net_expect(i, 1, EGGY_ERR_FIELD(i));
    }

    /* Exactly two humans, one per phone. */
    net_base(w);
    w[EGGY_CONFIG_PORT_BASE + EGGY_CONFIG_PORT_WORDS] = EGGY_PORT_HUMAN; /* Falco too */
    w[EGGY_CONFIG_PORT_BASE + EGGY_CONFIG_PORT_WORDS + 4] = 0;
    CHECK(parse(w, &c) == EGGY_ERR_NET_HUMANS);
    net_base(w);
    w[EGGY_CONFIG_PORT_BASE + 2 * EGGY_CONFIG_PORT_WORDS] = EGGY_PORT_CPU;
    w[EGGY_CONFIG_PORT_BASE + 2 * EGGY_CONFIG_PORT_WORDS + 4] = 5;
    CHECK(parse(w, &c) == EGGY_ERR_NET_HUMANS);

    /* Solo never reads words 49..63. */
    base(w);
    for (int i = 49; i < EGGY_CONFIG_WORDS; i++) {
        w[i] = -7;
    }
    CHECK(parse(w, &c) == 0);
    CHECK(!c.netplay);
}

static void test_hash(void) {
    int32_t a[EGGY_CONFIG_WORDS], b[EGGY_CONFIG_WORDS];
    net_base(a);
    memcpy(b, a, sizeof a);
    const uint32_t h = eggy_config_hash(a);
    CHECK(h == eggy_config_hash(b));
    /* What each phone may set for itself. */
    b[12] = 0;
    b[13] = 3;
    b[14] = 99;
    b[49] = 2;
    CHECK(eggy_config_hash(b) == h);
    /* Every other word is the match. */
    for (int i = 0; i < EGGY_CONFIG_WORDS; i++) {
        if ((i >= 12 && i <= 14) || i == 49) {
            continue;
        }
        memcpy(b, a, sizeof a);
        b[i] ^= 0x100;
        if (eggy_config_hash(b) == h) {
            fprintf(stderr, "hash ignores word %d\n", i);
            failures++;
        }
    }
    /* FNV-1a over the bytes low first: one word of 1, the rest 0, is pinned
     * so both phones' engines (and any page-side check) agree on it. */
    int32_t one[EGGY_CONFIG_WORDS] = {1};
    uint32_t want = 2166136261u;
    for (int i = 0; i < EGGY_CONFIG_WORDS; i++) {
        if ((i >= 12 && i <= 14) || i == 49) {
            continue;
        }
        for (int byte = 0; byte < 4; byte++) {
            want ^= (uint32_t)((uint32_t)one[i] >> (8 * byte)) & 0xFFu;
            want *= 16777619u;
        }
    }
    CHECK(eggy_config_hash(one) == want);
    /* The parse carries it. */
    EggyConfig c;
    CHECK(parse(a, &c) == 0 && c.hash == h);
}

int main(void) {
    memset(costumes, 5, sizeof costumes);
    costumes[25] = 3;
    test_valid();
    test_fields();
    test_time_match();
    test_item_mask();
    test_ports();
    test_fighters_and_teams();
    test_netplay();
    test_hash();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("eggy_config: all passed");
    return 0;
}

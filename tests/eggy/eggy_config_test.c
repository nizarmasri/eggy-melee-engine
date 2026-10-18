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

int main(void) {
    memset(costumes, 5, sizeof costumes);
    costumes[25] = 3;
    test_valid();
    test_fields();
    test_time_match();
    test_item_mask();
    test_ports();
    test_fighters_and_teams();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("eggy_config: all passed");
    return 0;
}

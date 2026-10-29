/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The resync's pure part (src/pc/net_resync.c): the snapshot image's round
 * trip and integrity, the receiver's layout check, the frame the leader takes
 * it at and where each receiver applies it, and the leader's budget of three
 * snapshots a match. Built with the host compiler by .github/workflows/ci.yml:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -Isrc tests/eggy/eggy_resync_test.c \
 *       src/pc/net_resync.c -o eggy_resync_test && ./eggy_resync_test
 */
#include "pc/net_pick.h"
#include "pc/net_resync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);               \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ---- the image ------------------------------------------------------------ */

/* Six regions shaped like a snapshot's: four fixed, two heaps. */
static ResyncHeader header_of(int32_t frame) {
    ResyncHeader h;
    memset(&h, 0, sizeof h);
    h.frame = frame;
    h.session = 0xC0FFEE01u;
    h.scene = 2;
    h.seed_addr = 0x0004A0C0ull;
    h.seed_val = 0xDEADBEEFu;
    h.nregions = 6;
    static const uint32_t lens[6] = {96, 32, 4000, 12000, 30001, 7};
    uint64_t addr = 0x10000;
    for (int i = 0; i < 6; i++) {
        h.region[i].addr = addr;
        h.region[i].len = lens[i];
        addr += 0x100000;
    }
    return h;
}

static size_t body_len(const ResyncHeader* h) {
    size_t n = 0;
    for (uint32_t i = 0; i < h->nregions; i++) {
        n += h->region[i].len;
    }
    return n;
}

static uint8_t* body_of(const ResyncHeader* h) {
    const size_t n = body_len(h);
    uint8_t* b = malloc(n);
    for (size_t i = 0; i < n; i++) {
        b[i] = (uint8_t)(i * 31 + (i >> 8));
    }
    return b;
}

static void test_round_trip(void) {
    const ResyncHeader h = header_of(1234);
    uint8_t* body = body_of(&h);
    const size_t n = resync_image_size(&h);
    CHECK(n == RESYNC_HEADER_FIXED + 6 * RESYNC_REGION_BYTES + body_len(&h) + RESYNC_TRAILER);
    uint8_t* img = malloc(n);
    CHECK(resync_encode(&h, body, img, n - 1) == 0); /* does not fit */
    CHECK(resync_encode(&h, body, img, n) == n);
    CHECK(memcmp(img, "EGSN", 4) == 0);

    ResyncHeader d;
    const uint8_t* got = NULL;
    CHECK(resync_decode(img, n, &d, &got));
    CHECK(d.frame == 1234 && d.session == h.session && d.scene == 2);
    CHECK(d.seed_addr == h.seed_addr && d.seed_val == h.seed_val);
    CHECK(d.nregions == 6);
    for (int i = 0; i < 6; i++) {
        CHECK(d.region[i].addr == h.region[i].addr && d.region[i].len == h.region[i].len);
    }
    CHECK(got == img + RESYNC_HEADER_FIXED + 6 * RESYNC_REGION_BYTES);
    CHECK(got != NULL && memcmp(got, body, body_len(&h)) == 0);

    /* The checksum covers every byte: one flipped anywhere is refused. */
    const size_t at[] = {0, 5, 9, 14, 21, 33, 40, 200, n / 2, n - RESYNC_TRAILER - 1, n - 1};
    for (size_t k = 0; k < sizeof at / sizeof at[0]; k++) {
        img[at[k]] ^= 0x10;
        CHECK(!resync_decode(img, n, &d, &got));
        img[at[k]] ^= 0x10;
    }
    CHECK(resync_decode(img, n, &d, &got));

    /* Exactly one image: short, long, empty, absent. */
    CHECK(!resync_decode(img, n - 1, &d, &got));
    CHECK(!resync_decode(img, 0, &d, &got));
    CHECK(!resync_decode(img, RESYNC_HEADER_FIXED, &d, &got));
    CHECK(!resync_decode(NULL, n, &d, &got));
    uint8_t* longer = malloc(n + 1);
    memcpy(longer, img, n);
    longer[n] = 0;
    CHECK(!resync_decode(longer, n + 1, &d, &got));
    free(longer);
    free(img);
    free(body);
}

/* An image re-signed after a change, so only the change is under test. */
static void resign(uint8_t* img, size_t n) {
    const uint64_t c = resync_checksum(img, n - RESYNC_TRAILER);
    for (int i = 0; i < 8; i++) {
        img[n - RESYNC_TRAILER + i] = (uint8_t)(c >> (56 - 8 * i));
    }
}

static void test_bad_headers(void) {
    const ResyncHeader h = header_of(50);
    uint8_t* body = body_of(&h);
    const size_t n = resync_image_size(&h);
    uint8_t* img = malloc(n);
    CHECK(resync_encode(&h, body, img, n) == n);
    ResyncHeader d;
    const uint8_t* got;

    img[3] = 'X'; /* magic */
    resign(img, n);
    CHECK(!resync_decode(img, n, &d, &got));
    img[3] = 'N';
    img[7] = 2; /* a version this build does not read */
    resign(img, n);
    CHECK(!resync_decode(img, n, &d, &got));
    img[7] = 1;
    img[35] = RESYNC_REGIONS_MAX + 1; /* more regions than any snapshot has */
    resign(img, n);
    CHECK(!resync_decode(img, n, &d, &got));
    img[35] = 5; /* fewer than the bytes say: the lengths no longer add up */
    resign(img, n);
    CHECK(!resync_decode(img, n, &d, &got));
    img[35] = 6;
    resign(img, n);
    CHECK(resync_decode(img, n, &d, &got));
    /* A region claiming more bytes than there are. */
    const size_t len0 = RESYNC_HEADER_FIXED + 8;
    img[len0] ^= 0x01;
    resign(img, n);
    CHECK(!resync_decode(img, n, &d, &got));

    /* Too many regions cannot be encoded at all. */
    ResyncHeader big = h;
    big.nregions = RESYNC_REGIONS_MAX + 1;
    CHECK(resync_image_size(&big) == 0);
    CHECK(resync_encode(&big, body, img, n) == 0);
    /* Nor can more than the bound on any state. */
    ResyncHeader huge = h;
    huge.region[5].len = RESYNC_BYTES_MAX;
    CHECK(resync_image_size(&huge) == 0);
    free(img);
    free(body);
}

/* ---- the receiver's layout check --------------------------------------------- */

static void test_layout(void) {
    const ResyncHeader h = header_of(10);
    ResyncRegion mine[RESYNC_REGIONS_MAX];
    for (uint32_t i = 0; i < h.nregions; i++) {
        mine[i] = h.region[i];
    }
    const int fixed = 4;
    CHECK(resync_layout_check(&h, mine, 6, fixed) == NULL);
    /* A heap's live extent differs between phones: it is the leader's that
     * gets restored, free lists and all. */
    mine[4].len += 4096;
    mine[5].len = 16;
    CHECK(resync_layout_check(&h, mine, 6, fixed) == NULL);
    /* A heap starting elsewhere, or one more or fewer heaps, cannot take it. */
    mine[5].addr += 32;
    CHECK(resync_layout_check(&h, mine, 6, fixed) != NULL);
    mine[5].addr -= 32;
    CHECK(resync_layout_check(&h, mine, 5, fixed) != NULL);
    /* The statics and heap descriptors must match exactly: another build. */
    mine[2].len += 4;
    CHECK(resync_layout_check(&h, mine, 6, fixed) != NULL);
    mine[2].len -= 4;
    mine[3].addr += 8;
    CHECK(resync_layout_check(&h, mine, 6, fixed) != NULL);
    mine[3].addr -= 8;
    CHECK(resync_layout_check(&h, mine, 6, fixed) == NULL);
}

/* ---- the frame choice --------------------------------------------------------- */

/* The widest a fight predicts before a hard stall (net_pick.h): a narrower
 * window only stops every phone closer to S. */
#define WINDOW NET_WINDOW_MAX

static void test_frame_final(void) {
    /* Entering S is final once every input before S is real and nothing
     * waits to be rolled back. */
    CHECK(resync_frame_final(100, 99, -1));
    CHECK(resync_frame_final(100, 104, -1));
    CHECK(!resync_frame_final(100, 98, -1)); /* frame 99 still predicted */
    CHECK(!resync_frame_final(100, 99, 97)); /* a misprediction to roll back */
    CHECK(!resync_frame_final(-1, 99, -1));
    CHECK(resync_frame_final(0, -1, -1)); /* the first frame has nothing before it */

    /* The leader's choice over a run of frames after a desync at 200: the
     * first fresh frame where the inputs before it are in and no rollback
     * is pending (rollbacks resolve between ticks). */
    struct {
        int32_t frame, have, rb;
    } run[] = {{203, 199, -1}, {204, 202, 201}, {205, 204, -1}, {206, 205, -1}};
    int32_t s = -1;
    for (size_t i = 0; i < sizeof run / sizeof run[0] && s < 0; i++) {
        if (resync_frame_final(run[i].frame, run[i].have, run[i].rb)) {
            s = run[i].frame;
        }
    }
    CHECK(s == 205);
}

static void test_apply_when(void) {
    CHECK(resync_apply_when(300, 300) == RESYNC_AT_FRESH);
    CHECK(resync_apply_when(300, 290) == RESYNC_LATER);
    CHECK(resync_apply_when(300, 301) == RESYNC_ROLLBACK);
    CHECK(resync_apply_when(300, 300 + RESYNC_REPLAY_MAX) == RESYNC_ROLLBACK);
    CHECK(resync_apply_when(300, 301 + RESYNC_REPLAY_MAX) == RESYNC_TOO_LATE);
    CHECK(resync_apply_when(-1, 0) == RESYNC_TOO_LATE);
}

/* A mesh where the leader holds before S: how far each receiver can have got
 * when the bytes land, at every delay Eggy uses and well past it. A receiver
 * runs a fresh frame f once every remote input up to f - WINDOW is real; the
 * leader's are real up to S + delay (written at the start of its tick S,
 * before it held), and a receiver's own up to its frame + delay. So the receivers run
 * ahead until they wait on the leader, and the furthest is its next fresh
 * frame. The lowest is the one the leader waited for before taking S. Each
 * must land where it can apply, never past the rings. */
static void test_mesh_positions(void) {
    for (int delay = 1; delay <= 16; delay++) {
        for (int phones = 2; phones <= 4; phones++) {
            const int32_t s = 1000;
            int32_t next[4]; /* each receiver's next fresh frame */
            /* The leader took S once every input before it was real: each
             * receiver had written up to S - 1, i.e. was past S - 1 - delay. */
            for (int r = 1; r < phones; r++) {
                next[r] = s - delay + (r - 1);
            }
            /* Step them as far as the inputs allow. */
            for (int moved = 1; moved;) {
                moved = 0;
                for (int r = 1; r < phones; r++) {
                    int32_t have = s + delay; /* the leader's */
                    for (int o = 1; o < phones; o++) {
                        if (o != r && next[o] - 1 + delay < have) {
                            have = next[o] - 1 + delay;
                        }
                    }
                    if (next[r] - WINDOW <= have) {
                        next[r]++;
                        moved = 1;
                    }
                }
            }
            for (int r = 1; r < phones; r++) {
                CHECK(next[r] == s + delay + WINDOW + 1);
                const int when = resync_apply_when(s, next[r]);
                CHECK(when == RESYNC_ROLLBACK);
                /* Any receiver short of that (bytes in early) applies too. */
                for (int32_t at = s - delay; at <= next[r]; at++) {
                    CHECK(resync_apply_when(s, at) != RESYNC_TOO_LATE);
                }
            }
        }
    }
    /* RESYNC_REPLAY_MAX holds every delay up to 17 at the widest window, and a
     * fight's delay is at most NET_FIGHT_DELAY_MAX. */
    CHECK(resync_apply_when(1000, 1000 + 17 + WINDOW + 1) == RESYNC_ROLLBACK);
    CHECK(NET_FIGHT_DELAY_MAX <= 17);
}

/* ---- the leader's budget ------------------------------------------------------ */

static void test_policy(void) {
    ResyncPolicy p;
    resync_policy_init(&p);
    CHECK(p.state == RESYNC_IDLE && p.sent == 0 && p.frame == -1 && p.asked == -1);
    CHECK(resync_policy_release(&p, 0x0F, 0)); /* nothing held */

    /* One snapshot, two receivers: the hold ends when both have answered. */
    CHECK(resync_policy_want(&p, 500));
    CHECK(p.state == RESYNC_WANT && p.asked == 500);
    CHECK(!resync_policy_want(&p, 501)); /* one is coming already */
    resync_policy_taken(&p, 505, 0x06);
    CHECK(p.state == RESYNC_HOLD && p.sent == 1 && p.frame == 505 && p.waiting == 0x06);
    CHECK(!resync_policy_want(&p, 506)); /* nor while it is out */
    CHECK(!resync_policy_release(&p, 0x07, 100));
    resync_policy_answer(&p, 1, 505, true);
    resync_policy_answer(&p, 2, 400, true); /* an older snapshot's: ignored */
    CHECK(p.waiting == 0x04 && p.applied == 0x02);
    CHECK(!resync_policy_release(&p, 0x07, 100));
    resync_policy_answer(&p, 2, 505, false); /* refused: answered all the same */
    CHECK(p.waiting == 0 && p.applied == 0x02);
    CHECK(resync_policy_release(&p, 0x07, 120));
    CHECK(p.state == RESYNC_IDLE && p.asked == -1);
    resync_policy_answer(&p, 1, 505, true); /* late duplicate: nothing held */
    CHECK(p.state == RESYNC_IDLE);

    /* The second: one receiver settled away mid-hold no longer holds it. */
    CHECK(resync_policy_want(&p, 900));
    resync_policy_taken(&p, 903, 0x0E);
    resync_policy_answer(&p, 1, 903, true);
    resync_policy_answer(&p, 3, 903, true);
    CHECK(!resync_policy_release(&p, 0x0F, 50));
    CHECK(resync_policy_release(&p, 0x0B, 60)); /* port 2 left */
    CHECK(p.sent == 2);

    /* The third: nobody answers, and the hold ends at its bound. */
    CHECK(resync_policy_want(&p, 1500));
    resync_policy_taken(&p, 1502, 0x02);
    CHECK(!resync_policy_release(&p, 0x03, RESYNC_HOLD_MS - 1));
    CHECK(resync_policy_release(&p, 0x03, RESYNC_HOLD_MS));
    CHECK(p.sent == RESYNC_SENDS_MAX);

    /* A snapshot and two resends: a fourth desync gets none. */
    CHECK(!resync_policy_want(&p, 2000));
    CHECK(p.state == RESYNC_IDLE && p.sent == 3);
    resync_policy_taken(&p, 2001, 0x02); /* not wanted: no effect */
    CHECK(p.state == RESYNC_IDLE && p.sent == 3);

    /* Out-of-range ports are nobody's. */
    resync_policy_init(&p);
    CHECK(resync_policy_want(&p, 1));
    resync_policy_taken(&p, 2, 0x02);
    resync_policy_answer(&p, -1, 2, true);
    resync_policy_answer(&p, 8, 2, true);
    CHECK(p.waiting == 0x02 && p.applied == 0);
}

/* ---- the two messages ---------------------------------------------------------- */

static void test_messages(void) {
    uint8_t m[RESYNC_MSG_LEN];
    resync_msg_encode(m, 123456, 9876543u, 2);
    int32_t f = 0;
    uint32_t size = 0;
    uint8_t nth = 0;
    CHECK(resync_msg_decode(m, RESYNC_MSG_LEN, &f, &size, &nth));
    CHECK(f == 123456 && size == 9876543u && nth == 2);
    CHECK(!resync_msg_decode(m, RESYNC_MSG_LEN - 1, &f, &size, &nth));
    CHECK(!resync_msg_decode(NULL, RESYNC_MSG_LEN, &f, &size, &nth));
    resync_msg_encode(m, -5, 1, 1);
    CHECK(!resync_msg_decode(m, RESYNC_MSG_LEN, &f, &size, &nth));

    uint8_t a[RESYNC_ANSWER_LEN];
    resync_answer_encode(a, 77, 7);
    uint8_t result = 0;
    CHECK(resync_answer_decode(a, RESYNC_ANSWER_LEN, &f, &result));
    CHECK(f == 77 && result == 7);
    CHECK(!resync_answer_decode(a, RESYNC_ANSWER_LEN + 1, &f, &result));
    resync_answer_encode(a, -1, 0);
    CHECK(!resync_answer_decode(a, RESYNC_ANSWER_LEN, &f, &result));
}

int main(void) {
    test_round_trip();
    test_bad_headers();
    test_layout();
    test_frame_final();
    test_apply_when();
    test_mesh_positions();
    test_policy();
    test_messages();
    if (failures != 0) {
        fprintf(stderr, "eggy_resync_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("eggy_resync_test: ok\n");
    return 0;
}

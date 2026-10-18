/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MELEE_EGGY netplay's pure rules (src/pc/eggy/eggy_hold.c): the hold before
 * the first tick, for two to four phones, and the HELLO payload. Built with
 * the host compiler by .github/workflows/ci.yml:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -Isrc tests/eggy/eggy_net_test.c \
 *       src/pc/eggy/eggy_hold.c -o eggy_net_test && ./eggy_net_test
 */
#include "pc/eggy/eggy_net.h"

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

/* Phones' holds, with every message a phone sends delivered to every other
 * at the end of the frame (the lanes deliver in order, some frames later). */
typedef struct Phone {
    EggyHold h;
    int port;
    bool go;
    bool reported;
    bool released;
    int outbox[8];
    int n;
} Phone;

static void phone_frame(Phone* p, bool lane_full, uint8_t away, int leader) {
    for (;;) {
        const int a = eggy_hold_next(&p->h, p->go, away, leader);
        if (a == EGGY_HOLD_WAIT) {
            return;
        }
        if (a == EGGY_HOLD_RELEASE) {
            p->released = true;
            return;
        }
        if (a == EGGY_HOLD_REPORT) {
            p->reported = true;
        } else {
            if (lane_full) {
                return; /* refused: asked again next frame */
            }
            p->outbox[p->n++] = a == EGGY_HOLD_SEND_LOADED ? EGGY_REL_LOADED :
                                a == EGGY_HOLD_SEND_ARMED  ? EGGY_REL_ARMED :
                                                             EGGY_REL_GO;
        }
        eggy_hold_done(&p->h, a);
    }
}

/* Everything phone `from` sent, to every phone in `to` but itself. */
static void deliver(Phone* ph, int n, int from, uint8_t to) {
    for (int q = 0; q < n; q++) {
        if (q != from && (to & (1u << ph[q].port))) {
            for (int i = 0; i < ph[from].n; i++) {
                eggy_hold_receive(&ph[q].h, ph[from].outbox[i], ph[from].port);
            }
        }
    }
    ph[from].n = 0;
}

static void phones_init(Phone* ph, int n, const int* ports) {
    uint8_t humans = 0;
    for (int i = 0; i < n; i++) {
        humans |= (uint8_t)(1u << ports[i]);
    }
    memset(ph, 0, sizeof(Phone) * (size_t)n);
    for (int i = 0; i < n; i++) {
        ph[i].port = ports[i];
        eggy_hold_init(&ph[i].h, humans, ports[i]);
    }
}

static void test_hold(void) {
    /* Two phones, as M2 played them: ports 0 and 2, 0 leads. */
    Phone ph[2];
    phones_init(ph, 2, (const int[]){0, 2});
    Phone* leader = &ph[0];
    Phone* guest = &ph[1];

    /* The guest loads first: nothing is reported until both are loaded. */
    phone_frame(guest, false, 0, 0);
    deliver(ph, 2, 1, 0x0F);
    CHECK(!guest->reported);
    CHECK(eggy_hold_next(&guest->h, false, 0, 0) == EGGY_HOLD_WAIT);

    /* The leader loads, with its lane full the first time. */
    phone_frame(leader, true, 0, 0);
    CHECK(leader->n == 0 && !leader->h.sent_loaded);
    phone_frame(leader, false, 0, 0);
    CHECK(leader->reported); /* the guest's LOADED was already in */
    deliver(ph, 2, 0, 0x0F);
    phone_frame(guest, false, 0, 0);
    CHECK(guest->reported);

    /* The leader's go alone starts nothing: it arms. */
    leader->go = true;
    phone_frame(leader, false, 0, 0);
    CHECK(!leader->released && leader->n == 1 && leader->outbox[0] == EGGY_REL_ARMED);
    deliver(ph, 2, 0, 0x0F);

    /* The guest's go only arms the start. */
    guest->go = true;
    phone_frame(guest, false, 0, 0);
    CHECK(!guest->released && guest->n == 1 && guest->outbox[0] == EGGY_REL_ARMED);
    deliver(ph, 2, 1, 0x0F);

    /* The leader now sends GO and releases; the guest releases on GO. */
    phone_frame(leader, false, 0, 0);
    CHECK(leader->released && leader->n == 1 && leader->outbox[0] == EGGY_REL_GO);
    CHECK(!guest->released);
    deliver(ph, 2, 0, 0x0F);
    phone_frame(guest, false, 0, 0);
    CHECK(guest->released);
}

/* Three and four phones: loads and gos in any order, nobody released until
 * every phone has loaded and every page said go, and then all of them. */
static void test_hold_n(void) {
    const int ports3[] = {1, 2, 3}, ports4[] = {0, 1, 2, 3};
    for (int n = 3; n <= 4; n++) {
        Phone ph[4];
        phones_init(ph, n, n == 3 ? ports3 : ports4);
        const int leader = ph[0].port;
        /* Phones load one per frame, last first; each page says go two
         * frames after its phone has reported. */
        int reported_at[4] = {-1, -1, -1, -1};
        int released = 0;
        for (int frame = 0; frame < 40 && released < n; frame++) {
            for (int i = n - 1; i >= 0; i--) {
                if (frame < n - 1 - i) {
                    continue; /* not loaded yet */
                }
                if (reported_at[i] >= 0 && frame >= reported_at[i] + 2 + i) {
                    ph[i].go = true;
                }
                const bool was = ph[i].reported;
                phone_frame(&ph[i], false, 0, leader);
                if (!was && ph[i].reported) {
                    reported_at[i] = frame;
                    for (int q = 0; q < n; q++) {
                        CHECK(ph[q].h.sent_loaded); /* only once every phone has loaded */
                    }
                }
            }
            for (int i = 0; i < n; i++) {
                deliver(ph, n, i, 0x0F);
            }
            released = 0;
            for (int i = 0; i < n; i++) {
                if (ph[i].released) {
                    released++;
                    for (int q = 0; q < n; q++) {
                        CHECK(ph[q].h.sent_armed); /* every page said go first */
                    }
                }
            }
        }
        CHECK(released == n);
        for (int i = 0; i < n; i++) {
            CHECK(ph[i].h.sent_go == (ph[i].port == leader)); /* one GO, the leader's */
        }
    }
}

/* A phone settled away in the hold is not waited for; a leader settled away
 * before its GO is replaced by the next phone, which has every ARMED already. */
static void test_hold_away(void) {
    Phone ph[3];
    phones_init(ph, 3, (const int[]){0, 1, 3});
    /* Port 3 never loads: settled away, the others play. */
    for (int frame = 0; frame < 8; frame++) {
        for (int i = 0; i < 2; i++) {
            ph[i].go = ph[i].reported;
            phone_frame(&ph[i], false, frame >= 3 ? 0x08 : 0, 0);
        }
        CHECK(!ph[0].reported || frame >= 3);
        deliver(ph, 3, 0, 0x0F);
        deliver(ph, 3, 1, 0x0F);
    }
    CHECK(ph[0].released && ph[1].released);

    /* The leader loads and arms, then goes before sending GO. */
    phones_init(ph, 3, (const int[]){0, 1, 3});
    for (int frame = 0; frame < 4; frame++) {
        for (int i = 0; i < 3; i++) {
            ph[i].go = ph[i].reported && i != 0;
            phone_frame(&ph[i], false, 0, 0);
            deliver(ph, 3, i, 0x0F);
        }
    }
    CHECK(ph[0].reported && ph[1].h.sent_armed && ph[2].h.sent_armed);
    CHECK(!ph[1].released && !ph[2].released); /* the leader's page never said go */
    /* Port 0 is settled away: port 1 leads now, and needs no message again. */
    phone_frame(&ph[1], false, 0x01, 1);
    CHECK(ph[1].released && ph[1].n == 1 && ph[1].outbox[0] == EGGY_REL_GO);
    phone_frame(&ph[2], false, 0x01, 1);
    CHECK(!ph[2].released);
    deliver(ph, 3, 1, 0x0F);
    phone_frame(&ph[2], false, 0x01, 1);
    CHECK(ph[2].released);
}

static void test_roles(void) {
    EggyHold h;
    /* Ports 0 and 2, this phone 2 (not the leader). */
    eggy_hold_init(&h, 0x05, 2);
    h.sent_loaded = h.reported = true;
    h.loaded = 0x01;
    CHECK(eggy_hold_next(&h, false, 0, 0) == EGGY_HOLD_WAIT);
    CHECK(eggy_hold_next(&h, true, 0, 0) == EGGY_HOLD_SEND_ARMED);
    eggy_hold_done(&h, EGGY_HOLD_SEND_ARMED);
    eggy_hold_receive(&h, EGGY_REL_ARMED, 0);
    CHECK(eggy_hold_next(&h, true, 0, 0) == EGGY_HOLD_WAIT); /* not the leader: no GO */
    eggy_hold_receive(&h, EGGY_REL_GO, 0);
    CHECK(eggy_hold_next(&h, true, 0, 0) == EGGY_HOLD_RELEASE);

    /* The leader needs its own go and every other ARMED. */
    eggy_hold_init(&h, 0x05, 0);
    h.sent_loaded = h.reported = true;
    h.loaded = 0x04;
    eggy_hold_receive(&h, EGGY_REL_ARMED, 2);
    CHECK(eggy_hold_next(&h, false, 0, 0) == EGGY_HOLD_WAIT); /* its own page has not said go */
    CHECK(eggy_hold_next(&h, true, 0, 0) == EGGY_HOLD_SEND_ARMED);
    eggy_hold_done(&h, EGGY_HOLD_SEND_ARMED);
    CHECK(eggy_hold_next(&h, true, 0, 0) == EGGY_HOLD_SEND_GO);

    /* An early ARMED (the other's go came first) is kept; others are not ours. */
    eggy_hold_init(&h, 0x05, 0);
    eggy_hold_receive(&h, EGGY_REL_ARMED, 2);
    eggy_hold_receive(&h, EGGY_REL_LOADED, 2);
    eggy_hold_receive(&h, EGGY_REL_LOADED, 1); /* not a phone of the match */
    eggy_hold_receive(&h, EGGY_REL_LOADED, 0); /* ourselves */
    CHECK(h.loaded == 0x04 && h.armed == 0x04);
    CHECK(eggy_hold_next(&h, true, 0, 0) == EGGY_HOLD_SEND_LOADED);
    eggy_hold_done(&h, EGGY_HOLD_SEND_LOADED);
    CHECK(eggy_hold_next(&h, true, 0, 0) == EGGY_HOLD_REPORT);
    eggy_hold_done(&h, EGGY_HOLD_REPORT);
    CHECK(eggy_hold_next(&h, true, 0, 0) == EGGY_HOLD_SEND_ARMED);
    eggy_hold_done(&h, EGGY_HOLD_SEND_ARMED);
    eggy_hold_receive(&h, 0x55, 2); /* not a hold message */
    CHECK(eggy_hold_next(&h, true, 0, 0) == EGGY_HOLD_SEND_GO);
}

static void test_hello(void) {
    uint8_t b[EGGY_HELLO_LEN];
    eggy_hello_encode(b, 0x01020304u, 0xA0B0C0D0u);
    const uint8_t want[EGGY_HELLO_LEN] = {
        'E', 'G', 'G', 1, 0x01, 0x02, 0x03, 0x04, 0xA0, 0xB0, 0xC0, 0xD0};
    CHECK(memcmp(b, want, sizeof b) == 0);
    CHECK(eggy_hello_matches(b, sizeof b, 0x01020304u, 0xA0B0C0D0u));
    CHECK(!eggy_hello_matches(b, sizeof b, 0x01020305u, 0xA0B0C0D0u)); /* another match */
    CHECK(!eggy_hello_matches(b, sizeof b, 0x01020304u, 0xA0B0C0D1u)); /* another session */
    CHECK(!eggy_hello_matches(b, sizeof b - 1, 0x01020304u, 0xA0B0C0D0u));
    CHECK(!eggy_hello_matches(NULL, 0, 0, 0));
    /* An empty message is not one. */
    CHECK(!eggy_hello_matches(b, 0, 0x01020304u, 0xA0B0C0D0u));
}

static void test_ids(void) {
    EggyConfig c;
    memset(&c, 0, sizeof c);
    /* net.c reads 0 as "none": neither id may reach it as 0. */
    CHECK(eggy_net_session_id(&c) != 0 && eggy_net_seed(&c) != 0);
    c.session = 77;
    c.seed = 99;
    CHECK(eggy_net_session_id(&c) == 77 && eggy_net_seed(&c) == 99);
}

int main(void) {
    test_hold();
    test_hold_n();
    test_hold_away();
    test_roles();
    test_hello();
    test_ids();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("eggy_net: all passed");
    return 0;
}

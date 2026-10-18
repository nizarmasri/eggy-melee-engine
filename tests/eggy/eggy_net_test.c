/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MELEE_EGGY netplay's pure rules (src/pc/eggy/eggy_hold.c): the hold before
 * the first tick and the barrier payload. Built with the host compiler by
 * .github/workflows/ci.yml:
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

/* Two phones' holds, with messages delivered at the end of each frame (the
 * reliable lane delivers in order but some frames later). */
typedef struct Phone {
    EggyHold h;
    bool go;
    bool reported;
    bool released;
    int outbox[8];
    int n;
} Phone;

static void phone_frame(Phone* p, bool lane_full) {
    for (;;) {
        const int a = eggy_hold_next(&p->h, p->go);
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

static void deliver(Phone* from, Phone* to) {
    for (int i = 0; i < from->n; i++) {
        eggy_hold_receive(&to->h, from->outbox[i]);
    }
    from->n = 0;
}

static void test_hold(void) {
    Phone leader, guest;
    memset(&leader, 0, sizeof leader);
    memset(&guest, 0, sizeof guest);
    eggy_hold_init(&leader.h, true);
    eggy_hold_init(&guest.h, false);

    /* The guest loads first: nothing is reported until both are loaded. */
    phone_frame(&guest, false);
    deliver(&guest, &leader);
    CHECK(!guest.reported);
    CHECK(eggy_hold_next(&guest.h, false) == EGGY_HOLD_WAIT);

    /* The leader loads, with its lane full the first time. */
    phone_frame(&leader, true);
    CHECK(leader.n == 0 && !leader.h.sent_loaded);
    phone_frame(&leader, false);
    CHECK(leader.reported); /* the guest's LOADED was already in */
    deliver(&leader, &guest);
    phone_frame(&guest, false);
    CHECK(guest.reported);

    /* The leader's go alone starts nothing. */
    leader.go = true;
    phone_frame(&leader, false);
    CHECK(!leader.released && leader.n == 0);

    /* The guest's go only arms the start. */
    guest.go = true;
    phone_frame(&guest, false);
    CHECK(!guest.released && guest.n == 1 && guest.outbox[0] == EGGY_REL_ARMED);
    deliver(&guest, &leader);

    /* The leader now sends GO and releases; the guest releases on GO. */
    phone_frame(&leader, false);
    CHECK(leader.released && leader.n == 1 && leader.outbox[0] == EGGY_REL_GO);
    CHECK(!guest.released);
    deliver(&leader, &guest);
    phone_frame(&guest, false);
    CHECK(guest.released);
}

static void test_roles(void) {
    EggyHold h;
    /* A guest never releases on its own go, an ARMED is the leader's only. */
    eggy_hold_init(&h, false);
    h.sent_loaded = h.peer_loaded = h.reported = true;
    eggy_hold_receive(&h, EGGY_REL_ARMED);
    CHECK(!h.peer_armed);
    CHECK(eggy_hold_next(&h, true) == EGGY_HOLD_SEND_ARMED);
    eggy_hold_done(&h, EGGY_HOLD_SEND_ARMED);
    CHECK(eggy_hold_next(&h, true) == EGGY_HOLD_WAIT);
    eggy_hold_receive(&h, EGGY_REL_GO);
    CHECK(eggy_hold_next(&h, true) == EGGY_HOLD_RELEASE);

    /* A leader ignores a GO and needs the guest's ARMED. */
    eggy_hold_init(&h, true);
    h.sent_loaded = h.peer_loaded = h.reported = true;
    eggy_hold_receive(&h, EGGY_REL_GO);
    CHECK(eggy_hold_next(&h, true) == EGGY_HOLD_WAIT);
    eggy_hold_receive(&h, EGGY_REL_ARMED);
    CHECK(eggy_hold_next(&h, false) == EGGY_HOLD_WAIT); /* its own page has not said go */
    CHECK(eggy_hold_next(&h, true) == EGGY_HOLD_SEND_GO);

    /* An early ARMED (the guest's go came first) is kept. */
    eggy_hold_init(&h, true);
    eggy_hold_receive(&h, EGGY_REL_ARMED);
    eggy_hold_receive(&h, EGGY_REL_LOADED);
    CHECK(eggy_hold_next(&h, true) == EGGY_HOLD_SEND_LOADED);
    eggy_hold_done(&h, EGGY_HOLD_SEND_LOADED);
    CHECK(eggy_hold_next(&h, true) == EGGY_HOLD_REPORT);
    eggy_hold_done(&h, EGGY_HOLD_REPORT);
    CHECK(eggy_hold_next(&h, true) == EGGY_HOLD_SEND_GO);
    eggy_hold_receive(&h, 0x55); /* not ours */
    CHECK(eggy_hold_next(&h, true) == EGGY_HOLD_SEND_GO);
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
    /* The lobby's empty READY_BARRIER is not one. */
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

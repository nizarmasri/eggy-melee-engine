/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A fight's input delay and prediction window from its link
 * (src/pc/net_pick.c). Built with the host compiler by
 * tools/eggy/host_tests.py:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -Isrc tests/eggy/eggy_pick_test.c \
 *       src/pc/net_pick.c -o eggy_pick_test && ./eggy_pick_test
 */
#include "pc/net_pick.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);               \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define FRAME 16667

static void test_need(void) {
    CHECK(net_pick_need(0, 0, FRAME) == 0); /* unmeasured */
    CHECK(net_pick_need(20000, 1000, FRAME) == 1);
    CHECK(net_pick_need(100000, 0, FRAME) == 3);      /* 50 ms */
    CHECK(net_pick_need(100004, 0, FRAME) == 4);      /* just over three frames */
    CHECK(net_pick_need(436000, 13000, FRAME) == 14); /* the 2026-10-29 pair: 231 ms */
    CHECK(net_pick_need(436000, 13000, 0) == 0);
}

/* Up to six frames of trip, all but two (what shipped); past it, half. */
static void test_delay(void) {
    CHECK(net_pick_fight_delay(0, 2) == 2);
    CHECK(net_pick_fight_delay(1, 1) == 1); /* a LAN keeps its floor of one */
    CHECK(net_pick_fight_delay(3, 2) == 2);
    CHECK(net_pick_fight_delay(5, 2) == 3);
    CHECK(net_pick_fight_delay(6, 2) == 4);
    CHECK(net_pick_fight_delay(7, 2) == 4); /* no step down past six */
    CHECK(net_pick_fight_delay(8, 2) == 4);
    CHECK(net_pick_fight_delay(10, 2) == 5);
    CHECK(net_pick_fight_delay(15, 2) == 8);
    CHECK(net_pick_fight_delay(14, 2) == 7);
    CHECK(net_pick_fight_delay(40, 2) == NET_FIGHT_DELAY_MAX);
    /* It never falls as the trip grows. */
    for (int need = 1; need < 40; need++) {
        CHECK(net_pick_fight_delay(need + 1, 2) >= net_pick_fight_delay(need, 2));
    }
}

/* Delay plus window covers the trip with four frames to spare, up to the
 * most the ring holds. */
static void test_window(void) {
    CHECK(net_pick_window(0, 2) == NET_WINDOW_MIN);
    CHECK(net_pick_window(6, 4) == NET_WINDOW_MIN);
    CHECK(net_pick_window(14, 7) == 11);
    CHECK(net_pick_window(15, 8) == 11);
    CHECK(net_pick_window(30, 8) == NET_WINDOW_MAX);
    for (int need = 0; need <= 16; need++) {
        const int d = net_pick_fight_delay(need, 2);
        CHECK(d + net_pick_window(need, d) >= need + 4);
    }
}

int main(void) {
    test_need();
    test_delay();
    test_window();
    if (failures) {
        fprintf(stderr, "eggy_pick_test: %d failed\n", failures);
        return 1;
    }
    printf("eggy_pick_test: passed\n");
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MELEE_EGGY's frame report, its pure part (src/pc/eggy/eggy_report.c): the
 * eggy_perf words a page of any engine age reads, and the hitch ring's layout
 * and wraparound. Built with the host compiler by tools/eggy/host_tests.py,
 * which tools/browser/build.py runs first:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -Isrc tests/eggy/eggy_report_test.c \
 *       src/pc/eggy/eggy_report.c -o eggy_report_test && ./eggy_report_test
 */
#include "pc/eggy/eggy_report.h"

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

/* A window whose every total differs, so a word read from the wrong place shows. */
static EggyPerfWindow sample_window(void) {
    EggyPerfWindow w;
    memset(&w, 0, sizeof w);
    w.frames = 120;
    w.ticks = 121;
    w.reruns = 7;
    w.ms[EGGY_PERF_TICK] = 1.5;
    w.ms[EGGY_PERF_RERUN] = 2.5;
    w.ms[EGGY_PERF_DRAW] = 3.5;
    w.ms[EGGY_PERF_SUBMIT] = 4.5;
    w.ms[EGGY_PERF_GPU_WAIT] = 5.5;
    w.ms[EGGY_PERF_SPARE] = 6.5;
    w.ms[EGGY_PERF_YIELD] = 7.5;
    w.ms[EGGY_PERF_OTHER] = 8.5;
    w.ms[EGGY_PERF_WAIT] = 9.5;
    w.worst_ms = 41.25;
    w.late_20 = 3;
    w.late_34 = 2;
    w.draws = 400;
    w.merged = 90;
    w.created = 12;
    w.queued = 4;
    w.compiling = 2;
    w.window_ms = 2000.0;
    w.background_left = 181;
    w.vert_kib = 300;
    w.index_kib = 40;
    w.uniform_kib = 80;
    w.undrawn = 5;
    w.texture_us = 0;
    return w;
}

/* What every word up to [25] has meant since the page started reading 26. */
static void check_v26(const int32_t* words) {
    CHECK(words[0] == 120);
    CHECK(words[1] == 121);
    CHECK(words[2] == 7);
    CHECK(words[3] == 1500);
    CHECK(words[4] == 2500);
    CHECK(words[5] == 3500);
    CHECK(words[6] == 4500);
    CHECK(words[7] == 5500);
    CHECK(words[8] == 6500);
    CHECK(words[9] == 7500);
    CHECK(words[10] == 8500);
    CHECK(words[11] == 41250);
    CHECK(words[12] == 3);
    CHECK(words[13] == 2);
    CHECK(words[14] == 400);
    CHECK(words[15] == 90);
    CHECK(words[16] == 12);
    CHECK(words[17] == 4);
    CHECK(words[18] == 2);
    CHECK(words[19] == 2000000);
    CHECK(words[20] == 181);
    CHECK(words[21] == 300);
    CHECK(words[22] == 40);
    CHECK(words[23] == 80);
    CHECK(words[24] == 5);
    CHECK(words[25] == 9500);
}

static void test_perf_words(void) {
    /* Words are appended, never moved: one more than the 26 the page reads. */
    CHECK(EGGY_PERF_WORDS == 27);

    EggyPerfWindow w = sample_window();
    int32_t before[EGGY_PERF_WORDS];
    eggy_perf_pack(before, &w);
    check_v26(before);
    CHECK(before[26] == 0);

    /* Texture time lands in [26] and moves nothing a 26-word reader sees. */
    w.texture_us = 12345;
    int32_t words[EGGY_PERF_WORDS];
    eggy_perf_pack(words, &w);
    check_v26(words);
    CHECK(memcmp(before, words, 26 * sizeof(int32_t)) == 0);
    CHECK(words[26] == 12345);

    /* A wrapped difference past int32 is capped, never negative. */
    w.texture_us = 0xF0000000u;
    eggy_perf_pack(words, &w);
    CHECK(words[26] == INT32_MAX);

    CHECK(eggy_perf_us(-1.0) == 0);
    CHECK(eggy_perf_us(0.0) == 0);
    CHECK(eggy_perf_us(1.0) == 1000);
    CHECK(eggy_perf_us(3.0e6) == 2000000000);
}

static EggyFrame sample_frame(uint32_t retrace) {
    EggyFrame f;
    memset(&f, 0, sizeof f);
    f.retrace = retrace;
    f.state = EGGY_STATE_FIGHTING;
    f.match_frame = 1200;
    f.at_ms = 65432.75;
    f.frame_ms = 226.0;
    f.ms[EGGY_PERF_TICK] = 4.0;
    f.ms[EGGY_PERF_RERUN] = 8.0;
    f.ms[EGGY_PERF_DRAW] = 150.0;
    f.ms[EGGY_PERF_SUBMIT] = 30.0;
    f.ms[EGGY_PERF_GPU_WAIT] = 20.0;
    f.ms[EGGY_PERF_SPARE] = 1.0;
    f.ms[EGGY_PERF_YIELD] = 9.0;
    f.ms[EGGY_PERF_OTHER] = 3.0;
    f.ms[EGGY_PERF_WAIT] = 1.0;
    f.ticks = 2;
    f.reruns = 6;
    f.began.started = 10;
    f.began.settled = 9;
    f.began.uploads = 125;
    f.began.texture_us = 1000;
    f.began.skipped = 50;
    f.ended.started = 12;
    f.ended.settled = 10;
    f.ended.uploads = 126;
    f.ended.texture_us = 4500;
    f.ended.skipped = 58;
    f.compiling = 2;
    f.undrawn = false;
    return f;
}

static const int32_t* slot(const int32_t* ring, int i) {
    return &ring[EGGY_HITCH_HEADER + i * EGGY_HITCH_ENTRY_WORDS];
}

static void test_hitch_layout(void) {
    CHECK(EGGY_HITCH_HEADER == 3);
    CHECK(EGGY_HITCH_ENTRY_WORDS == 24);
    CHECK(EGGY_HITCH_CAPACITY == 8);
    CHECK(EGGY_HITCH_RING_WORDS == 3 + 8 * 24);

    int32_t ring[EGGY_HITCH_RING_WORDS];
    memset(ring, 0x7f, sizeof ring);
    eggy_hitches_init(ring);
    CHECK(ring[0] == 0);
    CHECK(ring[1] == EGGY_HITCH_ENTRY_WORDS);
    CHECK(ring[2] == EGGY_HITCH_CAPACITY);
    for (int i = 0; i < EGGY_HITCH_CAPACITY; i++) {
        CHECK(slot(ring, i)[0] == 0); /* empty */
    }

    const EggyFrame f = sample_frame(4321);
    eggy_hitch_record(ring, &f);
    CHECK(ring[0] == 1);
    CHECK(ring[1] == EGGY_HITCH_ENTRY_WORDS);
    CHECK(ring[2] == EGGY_HITCH_CAPACITY);
    const int32_t* e = slot(ring, 0);
    CHECK(e[0] == 1);
    CHECK(e[1] == 4321);
    CHECK(e[2] == EGGY_STATE_FIGHTING);
    CHECK(e[3] == 1200);
    CHECK(e[4] == 65432);
    CHECK(e[5] == 226000);
    /* eggy_perf's order: tick, re-run, draw, submit, GPU wait, spare, yield,
     * other, netplay wait. */
    CHECK(e[6] == 4000);
    CHECK(e[7] == 8000);
    CHECK(e[8] == 150000);
    CHECK(e[9] == 30000);
    CHECK(e[10] == 20000);
    CHECK(e[11] == 1000);
    CHECK(e[12] == 9000);
    CHECK(e[13] == 3000);
    CHECK(e[14] == 1000);
    int64_t sections = 0;
    for (int i = 6; i <= 14; i++) {
        sections += e[i];
    }
    CHECK(sections == e[5]); /* they add up to the frame */
    CHECK(e[15] == 2);
    CHECK(e[16] == 6);
    CHECK(e[17] == 2);
    CHECK(e[18] == 1);
    CHECK(e[19] == 2);
    CHECK(e[20] == 1);
    CHECK(e[21] == 3500);
    CHECK(e[22] == 8);
    CHECK(e[23] == 0);
    for (int i = 1; i < EGGY_HITCH_CAPACITY; i++) {
        CHECK(slot(ring, i)[0] == 0); /* only the first slot written */
    }
}

static void test_hitch_wraparound(void) {
    int32_t ring[EGGY_HITCH_RING_WORDS];
    eggy_hitches_init(ring);
    /* Eleven hitches: the ring keeps 4..11, the 9th to 11th in slots 0..2. */
    for (uint32_t n = 1; n <= 11; n++) {
        EggyFrame f = sample_frame(1000 + n);
        f.undrawn = (n % 2) == 0;
        eggy_hitch_record(ring, &f);
        CHECK(ring[0] == (int32_t)n);
    }
    CHECK(ring[1] == EGGY_HITCH_ENTRY_WORDS);
    CHECK(ring[2] == EGGY_HITCH_CAPACITY);
    for (int i = 0; i < EGGY_HITCH_CAPACITY; i++) {
        const int32_t* e = slot(ring, i);
        const int32_t n = i < 3 ? 9 + i : 1 + i;
        CHECK(e[0] == n);
        CHECK(e[1] == 1000 + n);
        CHECK(e[23] == ((n % 2) == 0 ? 1 : 0));
    }
    /* A reader walks n from the count down, eight of them, each where it is. */
    const int32_t count = ring[0];
    for (int32_t n = count; n > 0 && n > count - EGGY_HITCH_CAPACITY; n--) {
        const int32_t* e = slot(ring, (n - 1) % EGGY_HITCH_CAPACITY);
        CHECK(e[0] == n);
        CHECK(e[1] == 1000 + n);
    }
}

static void test_hitch_counters_wrap(void) {
    int32_t ring[EGGY_HITCH_RING_WORDS];
    eggy_hitches_init(ring);
    EggyFrame f = sample_frame(7);
    /* Counters since boot wrap; a frame's share is still their difference. */
    f.began.started = 0xFFFFFFF0u;
    f.ended.started = 5u;
    f.began.texture_us = 0xFFFFFF00u;
    f.ended.texture_us = 0x100u;
    f.began.skipped = 0xFFFFFFFFu;
    f.ended.skipped = 0u;
    eggy_hitch_record(ring, &f);
    const int32_t* e = slot(ring, 0);
    CHECK(e[17] == 21);
    CHECK(e[21] == 512);
    CHECK(e[22] == 1);
}

static void test_hitch_threshold(void) {
    CHECK(EGGY_HITCH_MS == 50);
    CHECK(!eggy_hitch_is(16.7));
    CHECK(!eggy_hitch_is(50.0));
    CHECK(eggy_hitch_is(50.001));
    CHECK(eggy_hitch_is(311.0));
}

static void test_hitch_clamps(void) {
    int32_t ring[EGGY_HITCH_RING_WORDS];
    eggy_hitches_init(ring);
    EggyFrame f = sample_frame(1);
    f.at_ms = -5.0;
    f.frame_ms = 5.0e6;
    eggy_hitch_record(ring, &f);
    CHECK(slot(ring, 0)[4] == 0);
    CHECK(slot(ring, 0)[5] == 2000000000);
    f.at_ms = 1.0e12;
    eggy_hitch_record(ring, &f);
    CHECK(slot(ring, 1)[4] == INT32_MAX);
}

int main(void) {
    test_perf_words();
    test_hitch_layout();
    test_hitch_wraparound();
    test_hitch_counters_wrap();
    test_hitch_threshold();
    test_hitch_clamps();
    if (failures != 0) {
        fprintf(stderr, "eggy_report_test: %d failed\n", failures);
        return 1;
    }
    printf("eggy_report_test: ok\n");
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The frame report's pure part (src/pc/eggy/eggy_report.h): eggy_perf's words
 * from a window's totals, and the hitch ring. Presentation only, like the
 * rest of the report: nothing here is read by the simulation.
 */
#include "pc/eggy/eggy_report.h"

#include <string.h>

/* The sections in eggy_perf's order ([3]..[10], then [25]), which a hitch
 * entry keeps too. */
static const int k_sections[EGGY_PERF_SECTIONS] = {
    EGGY_PERF_TICK,
    EGGY_PERF_RERUN,
    EGGY_PERF_DRAW,
    EGGY_PERF_SUBMIT,
    EGGY_PERF_GPU_WAIT,
    EGGY_PERF_SPARE,
    EGGY_PERF_YIELD,
    EGGY_PERF_OTHER,
    EGGY_PERF_WAIT,
};

int32_t eggy_perf_us(double ms) {
    return ms <= 0 ? 0 : ms >= 2.0e6 ? 2000000000 : (int32_t)(ms * 1000.0);
}

static int32_t capped(uint32_t v) {
    return v > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)v;
}

void eggy_perf_pack(int32_t words[EGGY_PERF_WORDS], const EggyPerfWindow* w) {
    memset(words, 0, sizeof(int32_t) * EGGY_PERF_WORDS);
    words[0] = w->frames;
    words[1] = w->ticks;
    words[2] = w->reruns;
    for (int i = 0; i < 8; i++) {
        words[3 + i] = eggy_perf_us(w->ms[k_sections[i]]);
    }
    words[11] = eggy_perf_us(w->worst_ms);
    words[12] = w->late_20;
    words[13] = w->late_34;
    words[14] = w->draws;
    words[15] = w->merged;
    words[16] = w->created;
    words[17] = w->queued;
    words[18] = w->compiling;
    words[19] = eggy_perf_us(w->window_ms);
    words[20] = w->background_left;
    words[21] = w->vert_kib;
    words[22] = w->index_kib;
    words[23] = w->uniform_kib;
    words[24] = w->undrawn;
    words[25] = eggy_perf_us(w->ms[EGGY_PERF_WAIT]);
    /* Appended: a reader of the first 26 words reads what it always did. */
    words[26] = capped(w->texture_us);
}

void eggy_hitches_init(int32_t ring[EGGY_HITCH_RING_WORDS]) {
    memset(ring, 0, sizeof(int32_t) * EGGY_HITCH_RING_WORDS);
    ring[1] = EGGY_HITCH_ENTRY_WORDS;
    ring[2] = EGGY_HITCH_CAPACITY;
}

bool eggy_hitch_is(double frame_ms) {
    return frame_ms > EGGY_HITCH_MS;
}

void eggy_hitch_record(int32_t ring[EGGY_HITCH_RING_WORDS], const EggyFrame* f) {
    const uint32_t seq = (uint32_t)ring[0] + 1u;
    ring[0] = (int32_t)seq;
    const uint32_t slot = (seq - 1u) % EGGY_HITCH_CAPACITY;
    int32_t* e = &ring[EGGY_HITCH_HEADER + (int32_t)slot * EGGY_HITCH_ENTRY_WORDS];
    memset(e, 0, sizeof(int32_t) * EGGY_HITCH_ENTRY_WORDS);
    e[0] = (int32_t)seq;
    e[1] = (int32_t)f->retrace;
    e[2] = f->state;
    e[3] = f->match_frame;
    e[4] = f->at_ms <= 0 ? 0 : f->at_ms >= (double)INT32_MAX ? INT32_MAX : (int32_t)f->at_ms;
    e[5] = eggy_perf_us(f->frame_ms);
    for (int i = 0; i < EGGY_PERF_SECTIONS; i++) {
        e[6 + i] = eggy_perf_us(f->ms[k_sections[i]]);
    }
    e[15] = f->ticks;
    e[16] = f->reruns;
    /* Unsigned differences: right across a counter's wrap. */
    e[17] = capped(f->ended.started - f->began.started);
    e[18] = capped(f->ended.settled - f->began.settled);
    e[19] = f->compiling;
    e[20] = capped(f->ended.uploads - f->began.uploads);
    e[21] = capped(f->ended.texture_us - f->began.texture_us);
    e[22] = capped(f->ended.skipped - f->began.skipped);
    e[23] = f->undrawn ? 1 : 0;
}

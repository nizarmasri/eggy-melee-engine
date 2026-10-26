/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The frame report's pure part (eggy.h "Frame report" and "Hitches"): a
 * window's totals packed into eggy_perf's words, and the ring of long frames
 * eggy_hitches() exports. No clock and no engine state, so
 * tests/eggy/eggy_report_test.c builds it with the host compiler; eggy_perf.c
 * keeps the books and calls it.
 */
#ifndef PC_EGGY_EGGY_REPORT_H
#define PC_EGGY_EGGY_REPORT_H

#include "pc/eggy/eggy.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A window's totals, as eggy_perf() packs them (eggy.h's words, in order). */
typedef struct EggyPerfWindow {
    int32_t frames;
    int32_t ticks;
    int32_t reruns;
    double ms[EGGY_PERF_SECTIONS];
    double worst_ms;
    int32_t late_20;
    int32_t late_34;
    int32_t draws;
    int32_t merged;
    int32_t created;
    int32_t queued;
    int32_t compiling;
    double window_ms;
    int32_t background_left;
    int32_t vert_kib;
    int32_t index_kib;
    int32_t uniform_kib;
    int32_t undrawn;
    uint32_t texture_us;
} EggyPerfWindow;

/* Monotonic counters (since boot, wrapping): a frame's events are the
 * difference between the boundaries that close it. */
typedef struct EggyFrameCounters {
    uint32_t started;    /* browser pipeline compiles started */
    uint32_t settled;    /* settled: compiled or refused */
    uint32_t uploads;    /* textures uploaded */
    uint32_t texture_us; /* converting and uploading them */
    uint32_t skipped;    /* draws skipped for a pipeline not ready */
} EggyFrameCounters;

/* One frame, boundary to boundary, as a hitch entry records it. */
typedef struct EggyFrame {
    uint32_t retrace;
    int32_t state;       /* HUD [0] */
    int32_t match_frame; /* HUD [1] */
    double at_ms;        /* when it ended, performance.now() */
    double frame_ms;
    double ms[EGGY_PERF_SECTIONS];
    int32_t ticks;
    int32_t reruns;
    EggyFrameCounters began;
    EggyFrameCounters ended;
    int32_t compiling; /* still compiling at its end */
    bool undrawn;
} EggyFrame;

/* Milliseconds as eggy_perf's microsecond words: 0 at most, capped at 2e9. */
int32_t eggy_perf_us(double ms);
void eggy_perf_pack(int32_t words[EGGY_PERF_WORDS], const EggyPerfWindow* w);

void eggy_hitches_init(int32_t ring[EGGY_HITCH_RING_WORDS]);
/* A frame long enough to be recorded: over EGGY_HITCH_MS. */
bool eggy_hitch_is(double frame_ms);
void eggy_hitch_record(int32_t ring[EGGY_HITCH_RING_WORDS], const EggyFrame* f);

#ifdef __cplusplus
}
#endif

#endif

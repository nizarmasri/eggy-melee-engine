/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Eggy host API: where a phone's frame time goes (eggy_perf, eggy.h). The page
 * reads it every few seconds of a match and reports it, so a phone that plays
 * slowly says whether the game's ticks, its drawing, the GPU or the browser
 * took the time.
 *
 * The clock is shared out between sections: at any moment exactly one is
 * running, and entering one stops the last. The frame boundary (src/pc/vi.c)
 * runs inside the game's render pass (HSD_VICopyXFBAsync waits there for a
 * free XFB), so a boundary's sections nest inside "draw" and hand the clock
 * back to it after: nothing is counted twice.
 *
 * Each frame, boundary to boundary, also keeps its own books: its sections,
 * its ticks, and what aurora's counters say happened in it. One longer than
 * EGGY_HITCH_MS goes into the hitch ring (eggy_hitches), so the page can tell
 * what a single long frame was, which a window's totals cannot. A few
 * counters read per boundary, nothing allocated, nothing logged.
 *
 * Presentation only: nothing here is read by the simulation.
 */
#include "pc/eggy/eggy.h"
#include "pc/eggy/eggy_report.h"

#include <aurora/gfx.h>
#include <dolphin/vi.h>
#include <emscripten.h>
#include <string.h>

extern uint32_t aurora_browser_pipelines_compiling(void);
extern uint32_t aurora_background_pipelines_left(void);
extern uint32_t aurora_browser_compiles_started(void);
extern uint32_t aurora_browser_compiles_settled(void);
extern uint32_t aurora_texture_uploads(void);
extern uint32_t aurora_texture_us(void);
extern uint32_t aurora_draws_skipped(void);

static int s_section = EGGY_PERF_OTHER;
static double s_since;
static double s_ms[EGGY_PERF_SECTIONS];
static double s_window_start;
static double s_last_boundary;
static double s_worst_ms;
static int32_t s_frames;
static int32_t s_late_20;
static int32_t s_late_34;
static int32_t s_ticks;
static int32_t s_reruns;
static int32_t s_undrawn;
static uint32_t s_texture_us_read; /* aurora_texture_us() at the last eggy_perf() */
static int32_t s_words[EGGY_PERF_WORDS];

/* The frame being played, since the last boundary. */
static double s_frame_ms[EGGY_PERF_SECTIONS];
static int32_t s_frame_ticks;
static int32_t s_frame_reruns;
static EggyFrameCounters s_frame_began;
static bool s_frame_judged; /* false after a parked boundary: that wait is no frame */
static int32_t s_hitches[EGGY_HITCH_RING_WORDS] = {0, EGGY_HITCH_ENTRY_WORDS, EGGY_HITCH_CAPACITY};

/* The running section's share up to `now`, to the window and the frame. */
static void account(double now) {
    if (s_since != 0) {
        const double ms = now - s_since;
        s_ms[s_section] += ms;
        s_frame_ms[s_section] += ms;
    } else {
        s_window_start = now;
    }
    s_since = now;
}

int pc_eggy_perf_enter(int section) {
    account(emscripten_get_now());
    const int previous = s_section;
    s_section = section;
    return previous;
}

void pc_eggy_perf_tick(bool rerun) {
    if (rerun) {
        s_reruns++;
        s_frame_reruns++;
    } else {
        s_ticks++;
        s_frame_ticks++;
    }
}

static void frame_counters(EggyFrameCounters* c) {
    c->started = aurora_browser_compiles_started();
    c->settled = aurora_browser_compiles_settled();
    c->uploads = aurora_texture_uploads();
    c->texture_us = aurora_texture_us();
    c->skipped = aurora_draws_skipped();
}

static void boundary(bool undrawn, bool idle) {
    const double now = emscripten_get_now();
    account(now); /* so the frame's sections add up to it */
    s_undrawn += undrawn;
    EggyFrameCounters counters;
    frame_counters(&counters);
    if (s_last_boundary != 0) {
        const double frame = now - s_last_boundary;
        if (frame > s_worst_ms) {
            s_worst_ms = frame;
        }
        s_late_20 += frame > 20.0;
        s_late_34 += frame > 34.0;
        if (s_frame_judged && !idle && eggy_hitch_is(frame)) {
            EggyFrame f;
            memset(&f, 0, sizeof f);
            f.retrace = VIGetRetraceCount();
            pc_eggy_frame_facts(&f.state, &f.match_frame);
            f.at_ms = now;
            f.frame_ms = frame;
            memcpy(f.ms, s_frame_ms, sizeof f.ms);
            f.ticks = s_frame_ticks;
            f.reruns = s_frame_reruns;
            f.began = s_frame_began;
            f.ended = counters;
            f.compiling = (int32_t)aurora_browser_pipelines_compiling();
            f.undrawn = undrawn;
            eggy_hitch_record(s_hitches, &f);
        }
    }
    s_frame_judged = !idle;
    memset(s_frame_ms, 0, sizeof s_frame_ms);
    s_frame_ticks = 0;
    s_frame_reruns = 0;
    s_frame_began = counters;
    s_last_boundary = now;
    s_frames++;
}

void pc_eggy_perf_boundary(bool undrawn) {
    boundary(undrawn, false);
}

void pc_eggy_perf_idle(void) {
    boundary(false, true);
}

EMSCRIPTEN_KEEPALIVE const int32_t* eggy_perf(void) {
    /* Close the running section's share up to now, then start a new window. */
    pc_eggy_perf_enter(s_section);
    const double now = s_since;
    EggyPerfWindow w;
    memset(&w, 0, sizeof w);
    w.frames = s_frames;
    w.ticks = s_ticks;
    w.reruns = s_reruns;
    memcpy(w.ms, s_ms, sizeof w.ms);
    w.worst_ms = s_worst_ms;
    w.late_20 = s_late_20;
    w.late_34 = s_late_34;
    const AuroraStats* stats = aurora_get_stats();
    if (stats != NULL) {
        w.draws = (int32_t)stats->drawCallCount;
        w.merged = (int32_t)stats->mergedDrawCallCount;
        w.created = (int32_t)stats->createdPipelines;
        w.queued = (int32_t)stats->queuedPipelines;
        w.vert_kib = (int32_t)(stats->lastVertSize / 1024);
        w.index_kib = (int32_t)(stats->lastIndexSize / 1024);
        w.uniform_kib = (int32_t)(stats->lastUniformSize / 1024);
    }
    w.compiling = (int32_t)aurora_browser_pipelines_compiling();
    w.window_ms = now - s_window_start;
    w.background_left = (int32_t)aurora_background_pipelines_left();
    w.undrawn = s_undrawn;
    const uint32_t texture_us = aurora_texture_us();
    w.texture_us = texture_us - s_texture_us_read;
    s_texture_us_read = texture_us;
    eggy_perf_pack(s_words, &w);
    memset(s_ms, 0, sizeof s_ms);
    s_window_start = now;
    s_worst_ms = 0;
    s_frames = s_late_20 = s_late_34 = s_ticks = s_reruns = s_undrawn = 0;
    return s_words;
}

EMSCRIPTEN_KEEPALIVE const int32_t* eggy_hitches(void) {
    return s_hitches;
}

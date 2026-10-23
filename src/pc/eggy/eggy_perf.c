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
 * Presentation only: nothing here is read by the simulation.
 */
#include "pc/eggy/eggy.h"

#include <aurora/gfx.h>
#include <emscripten.h>
#include <string.h>

extern uint32_t aurora_browser_pipelines_compiling(void);
extern uint32_t aurora_background_pipelines_left(void);

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
static int32_t s_words[EGGY_PERF_WORDS];

int pc_eggy_perf_enter(int section) {
    const double now = emscripten_get_now();
    if (s_since != 0) {
        s_ms[s_section] += now - s_since;
    } else {
        s_window_start = now;
    }
    s_since = now;
    const int previous = s_section;
    s_section = section;
    return previous;
}

void pc_eggy_perf_tick(bool rerun) {
    if (rerun) {
        s_reruns++;
    } else {
        s_ticks++;
    }
}

void pc_eggy_perf_boundary(void) {
    const double now = emscripten_get_now();
    if (s_last_boundary != 0) {
        const double frame = now - s_last_boundary;
        if (frame > s_worst_ms) {
            s_worst_ms = frame;
        }
        s_late_20 += frame > 20.0;
        s_late_34 += frame > 34.0;
    }
    s_last_boundary = now;
    s_frames++;
}

static int32_t us(double ms) {
    return ms <= 0 ? 0 : ms >= 2.0e6 ? 2000000000 : (int32_t)(ms * 1000.0);
}

EMSCRIPTEN_KEEPALIVE const int32_t* eggy_perf(void) {
    /* Close the running section's share up to now, then start a new window. */
    pc_eggy_perf_enter(s_section);
    const double now = s_since;
    memset(s_words, 0, sizeof s_words);
    s_words[0] = s_frames;
    s_words[1] = s_ticks;
    s_words[2] = s_reruns;
    s_words[3] = us(s_ms[EGGY_PERF_TICK]);
    s_words[4] = us(s_ms[EGGY_PERF_RERUN]);
    s_words[5] = us(s_ms[EGGY_PERF_DRAW]);
    s_words[6] = us(s_ms[EGGY_PERF_SUBMIT]);
    s_words[7] = us(s_ms[EGGY_PERF_GPU_WAIT]);
    s_words[8] = us(s_ms[EGGY_PERF_SPARE]);
    s_words[9] = us(s_ms[EGGY_PERF_YIELD]);
    s_words[10] = us(s_ms[EGGY_PERF_OTHER]);
    s_words[11] = us(s_worst_ms);
    s_words[12] = s_late_20;
    s_words[13] = s_late_34;
    const AuroraStats* stats = aurora_get_stats();
    if (stats != NULL) {
        s_words[14] = (int32_t)stats->drawCallCount;
        s_words[15] = (int32_t)stats->mergedDrawCallCount;
        s_words[16] = (int32_t)stats->createdPipelines;
        s_words[17] = (int32_t)stats->queuedPipelines;
        s_words[21] = (int32_t)(stats->lastVertSize / 1024);
        s_words[22] = (int32_t)(stats->lastIndexSize / 1024);
        s_words[23] = (int32_t)(stats->lastUniformSize / 1024);
    }
    s_words[18] = (int32_t)aurora_browser_pipelines_compiling();
    s_words[19] = us(now - s_window_start);
    s_words[20] = (int32_t)aurora_background_pipelines_left();
    memset(s_ms, 0, sizeof s_ms);
    s_window_start = now;
    s_worst_ms = 0;
    s_frames = s_late_20 = s_late_34 = s_ticks = s_reruns = 0;
    return s_words;
}

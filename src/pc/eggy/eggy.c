/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Eggy host API (src/pc/eggy/eggy.h, EGGY.md): the page loads a config, the
 * engine builds the StartMeleeData itself (onEnterDebugVs in gmvsmode.c is
 * the template), holds the loaded match at frame 0 until eggy_go(), feeds it
 * the page's pads, and parks again after GAME! (and Melee's results screen, if
 * asked) with the standings in eggy_results(). One engine instance plays every
 * match of a session: the loop below is Melee's own scene loop (the same heaps
 * torn down and rebuilt per scene as VS mode's CSS -> VS -> results cycle), so
 * nothing here allocates.
 *
 * GM_EGGY is a game mode of three states:
 *   VS (1)            on_enter parks until a valid config is loaded, then
 *                     builds the match; GS_VS loads it and holds at frame 0
 *                     (pc_eggy_scene_begin, gmscene.c) until eggy_go()
 *   sudden death (2)  only after a tie, as VS mode plays it (gmvsmelee.c)
 *   results (3)       Melee's results screen, ended after [15] seconds
 *
 * Netplay (word 48) is the same loop with melee-web's session around it, over
 * a mesh of two to four phones (src/pc/eggy/eggy_net.c): the park opens it,
 * the hold waits for every other phone too, and the park after the match
 * closes it.
 *
 * All of this runs on the game thread. The exports only latch requests the
 * game thread acts on at its next frame boundary or tick, so the page can call
 * them whenever the engine has yielded (between frames, or during a disc read).
 */
#include "pc/compat.h"
#include "pc/eggy/eggy.h"
#include "pc/eggy/eggy_mode.h"
#include "pc/eggy/eggy_net.h"
#include "pc/net.h"
#include "pc/net_internal.h"
#include "pc/pc.h"
#include "pc/widescreen.h"

#include <dolphin/pad.h>
#include <math.h>
#include <dolphin/vi.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wscalar-storage-order" /* disc-struct unions in lb/types.h */
#include <melee/ft/fighter.h>
#include <melee/ft/forward.h>
#include <melee/ft/ftlib.h>
#include <melee/ft/inlines.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gm_1B03.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/lb/forward.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/mn/types.h>
#include <melee/pl/forward.h>
#include <melee/pl/player.h>
#pragma GCC diagnostic pop
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/random.h>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdlib.h>
#include <string.h>

/* GameCube buttons: d-pad, Z, R, L, A, B, X, Y, Start. */
#define EGGY_PAD_BUTTONS 0x1F7Fu

enum {
    MODE_STATE_VS = 1,
    MODE_STATE_SUDDEN_DEATH = 2,
    MODE_STATE_RESULTS = 3,
};

/* onState names, by the index eggy_js_state takes. */
enum {
    JS_PARKED = 0,
    JS_LOADED = 1,
    JS_ENDED = 2,
    JS_ERROR = 3,
    JS_NET_ERROR = 4,
    JS_DESYNC = 5,
    JS_PEER_LOST = 6,
    JS_AWAY = 7,
    JS_SNAPSHOT = 8,
    JS_RESYNC = 9,
};

// clang-format off
EM_JS(void, eggy_js_state, (int kind, int a, int b), {
  const names = ['parked', 'loaded', 'ended', 'error', 'net-error', 'desync', 'peer-lost', 'away',
                 'snapshot', 'resync'];
  const eggy = Module['eggy'];
  if (eggy && typeof eggy.onState === 'function') {
    try {
      eggy.onState(names[kind], a, b);
    } catch (e) {
      err('eggy: onState(' + names[kind] + ') threw: ' + e);
    }
  }
});
// clang-format on

static int32_t s_config_words[EGGY_CONFIG_WORDS]; /* the page writes layout v1 here */
static int32_t s_hud[EGGY_HUD_WORDS];
static int32_t s_results[EGGY_RESULTS_WORDS];

static int s_state = EGGY_STATE_PARKED;
static EggyConfig s_pending; /* accepted by eggy_load, not yet taken by the park */
static bool s_pending_ok;
static int s_pending_serial;
static EggyConfig s_cfg; /* the match being played */
static int s_serial;
static bool s_go;
static bool s_abort;
static bool s_scene_live; /* inside GS_VS or GS_SUDDEN_DEATH, fighters exist */
static bool s_sudden_death;
static bool s_net_lost;      /* netplay: the session died mid-match, outcome 4 */
static bool s_net_failed;    /* netplay: it died in the hold, before frame 0 */
static bool s_net_closing;   /* netplay: the park is waiting for the other phones */
static int32_t s_frame_base; /* ticks of the scenes before this one (VS before sudden death) */
static int32_t s_end_frame;
static int32_t s_results_ticks;
static int s_matches;
static StartMeleeData s_start;            /* as built, for sudden death */
static PADStatus s_pads[EGGY_PORTS];      /* the page's, for the next pad samples */
static bool s_test_desync;                /* eggy_test_desync, waiting for a safe tick */
static PADStatus s_tick_pads[EGGY_PORTS]; /* as the last tick simulated them */

bool pc_eggy_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char* eggy = getenv("MELEE_EGGY");
        const char* scene = getenv("MELEE_BOOT_SCENE");
        on = eggy != NULL && eggy[0] != '\0' && eggy[0] != '0' && pc_vs_only() && scene != NULL &&
             strcmp(scene, "eggy") == 0;
    }
    return on != 0;
}

void pc_eggy_preinit(void) {
    /* The page owns the keyboard: SDL registers no key listener at all, so no
     * page keystroke is captured or default-prevented (SDL3 accepts "#none"
     * for this hint; it must be set before the window exists). */
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#none");
    /* The engine boots while people pick: nothing builds ahead until the page
     * says it may (eggy_background_builds), or the boot's compiles stall the
     * pick screen in the GPU process. */
    extern void aurora_set_background_builds(bool on);
    aurora_set_background_builds(false);
}

/* aurora's pipeline cache (extern/aurora/lib/gfx/pipeline_cache.cpp): every
 * browser compile goes through one capped queue, the ones a draw asked for
 * first, then the cache's known pipelines "in the background". */
extern void aurora_pump_pipelines(void);
extern void aurora_set_compile_cap(uint32_t n);
extern void aurora_set_background_builds(bool on);
extern uint32_t aurora_demanded_pipelines_pending(void);
extern uint32_t aurora_draws_skipped(void);
extern uint32_t aurora_texture_uploads(void);
extern void aurora_mark_go(void);
extern uint32_t aurora_demanded_after_go(void);
extern uint32_t aurora_demanded_after_go_known(void);
extern uint32_t aurora_demanded_after_go_new(void);
extern uint32_t aurora_pipelines_finished(void);
extern uint32_t aurora_background_pipelines_left(void);
extern uint32_t aurora_pipelines_failed(void);
extern uint32_t aurora_compile_ms_total(void);
extern int aurora_pipeline_cache_flush(void);
extern void aurora_set_fog_enabled(bool on);
extern void aurora_set_pipeline_tags(const uint32_t* tags, uint32_t count);
extern void aurora_prebuild_tagged(void);
/* src/pc/audio.c: nothing is mixed while held, and the release starts clean. */
extern void pc_audio_set_held(bool held);

/* The cache's known pipelines build only while parked or holding a loaded
 * match at frame 0, and only while the page allows it (eggy_background_builds:
 * off until it says, and it says no while people pick): a compile in the GPU
 * process slows every frame, and a loading or played match's own pipelines
 * come first. The hold is where most get built, behind the load screen: the
 * fight's effects are in the cache, and one compiled mid-fight is a hitch on
 * every phone, and an iPhone's GPU process at risk. */
static bool s_background_ok = false;
static bool s_audio_held;

static void background_builds_follow(void) {
    aurora_set_background_builds(
        s_background_ok && (s_state == EGGY_STATE_PARKED || s_state == EGGY_STATE_ERROR ||
                               s_state == EGGY_STATE_LOADED));
}

static void set_state(int state) {
    s_state = state;
    s_hud[0] = state;
    background_builds_follow();
    /* A loaded match's stage music and crowd are started by the VS scene; held
     * at frame 0 they stay silent, and start from the top at GO. */
    const bool held = state == EGGY_STATE_LOADED;
    if (held != s_audio_held) {
        s_audio_held = held;
        pc_audio_set_held(held);
    }
}

static bool fighting(void) {
    return s_scene_live && (s_state == EGGY_STATE_LOADED || s_state == EGGY_STATE_FIGHTING);
}

/* ---- pre-roll -----------------------------------------------------------
 * A loaded match waits at frame 0 (the holds below) for as long as the page,
 * or the slowest phone, takes. While it waits, the held frame is drawn: the
 * render pass only (gm_DrawFrame), no tick, no input, nothing the simulation
 * reads, so every pipeline, texture and buffer the fight's first frames need
 * is made behind the load screen and not at GO. VS's on_enter has already
 * built the camera, the fighters, the stage and the HUD the draw reads. The
 * seed is put back after, should any display callback draw a random number:
 * netplay re-seeds entering frame 0 anyway (net.c), and so a phone alone
 * starts the same match however long it pre-rolled.
 *
 * Ready (eggy_graphics_ready) once nothing a draw asked for is still queued
 * or compiling, the last PREROLL_CLEAN draws made nothing new and their frame
 * times are steady; or, should something keep moving (a pipeline that failed
 * is skipped on every draw), steady after PREROLL_SETTLE draws, or whatever
 * the times after PREROLL_GIVE_UP. Then the hold presents that picture again
 * (pc_eggy_hold_frame) and draws it once a second, and pre-rolls again when
 * that draw asks for a pipeline or uploads a texture, or the page's picture
 * size changes. */
#define PREROLL_CLEAN 30
#define PREROLL_SETTLE 300
#define PREROLL_GIVE_UP 600
#define PREROLL_REDRAW_EVERY 60

typedef struct Preroll {
    bool active; /* a hold is under way */
    bool ready;
    bool view_changed; /* the page's picture size changed since the last look */
    uint32_t seed;     /* the RNG as the hold found it */
    int32_t boundaries;
    int32_t draws;
    int32_t clean; /* draws in a row that made nothing new */
    bool uploaded; /* the last draw uploaded a texture */
    int32_t since_draw;
    uint64_t last_ns; /* the last draw's start, for the next one's frame time */
    uint32_t times_us[PREROLL_CLEAN];
    int32_t times_n;
    int32_t median_us;
    int32_t p90_us;
} Preroll;

static Preroll s_pre;
static int s_preroll_min; /* eggy_preroll_min */

bool pc_eggy_hold_frame(void) {
    /* Parked, the last picture (GAME! or the results screen) stays up. Not
     * while a match loads or holds: those frames clear to black, so frame 0
     * draws into a cleared colour and depth buffer, not over the last one.
     * Once the held frame is pre-rolled, that picture stays up instead. */
    return pc_eggy_enabled() &&
           (s_state == EGGY_STATE_PARKED || s_state == EGGY_STATE_ERROR || s_net_closing ||
               (s_state == EGGY_STATE_LOADED && s_pre.active && s_pre.ready));
}

bool pc_eggy_music_off(void) {
    return pc_eggy_enabled() && !s_cfg.music;
}

bool pc_eggy_frozen_stadium(void) {
    return s_cfg.frozen_stadium;
}

/* Presses for one simulated tick only (eggy_tap), whatever the frame rate: a
 * phone that renders a frame in two ticks would hold a one-frame press for
 * both, and a short hop needs jump up again before the jump squat ends (3
 * ticks at the quickest). Taken by the sample a tick simulates: the pad
 * alarm's alone, or in netplay the session's local sample (net.c
 * capture_local_sample, between pc_eggy_sampling(true) and (false)), never
 * the alarm's, whose samples the session overwrites. */
static u16 s_taps[EGGY_PORTS];
static bool s_sampling;

void pc_eggy_sampling(bool on) {
    s_sampling = on;
}

/* ---- input --------------------------------------------------------------
 * Every PADRead in the engine (the pad alarm's HSD_PadRenewRawStatus, netplay's
 * local sample, the chat overlay) goes through here: -Wl,--wrap=PADRead in
 * platforms/browser/CMakeLists.txt. With MELEE_EGGY the page's pads replace
 * aurora's whole merge of SDL keyboard, gamepads, touch and the GameCube
 * adapter, so none of those reach the game. The page's pads count only while a
 * match is held or running; otherwise all four ports read neutral, so a
 * results screen runs its full time and a held button never leaks into the
 * next match's setup.
 *
 * In a netplay session the sample net.c takes is port 0's (net.c
 * capture_local_sample: "physical port zero is the local controller"), and
 * the session writes every port of the frame itself (write_head), so port 0
 * carries this phone's own port and nothing else is read: the other phones'
 * pads come from the other phones. */
u32 __real_PADRead(PADStatus* status);
u32 __wrap_PADRead(PADStatus* status) {
    if (!pc_eggy_enabled()) {
        return __real_PADRead(status);
    }
    const bool live = s_state == EGGY_STATE_LOADED || s_state == EGGY_STATE_FIGHTING;
    const bool net = s_cfg.netplay && pc_net_active();
    const bool takes_taps = net ? s_sampling : true;
    for (int i = 0; i < EGGY_PORTS; i++) {
        const int from = net ? (i == 0 ? s_cfg.local_port : -1) : i;
        if (live && from >= 0) {
            status[i] = s_pads[from];
            if (takes_taps) {
                status[i].button |= s_taps[from];
                s_taps[from] = 0;
            }
        } else {
            memset(&status[i], 0, sizeof status[i]);
        }
        status[i].err = PAD_ERR_NONE;
    }
    if (!live) {
        memset(s_taps, 0, sizeof s_taps);
    }
    return 0; /* no rumble */
}

static int8_t clamp_s8(int v) {
    return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
}

static uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

EMSCRIPTEN_KEEPALIVE void eggy_pad(int port, int buttons, int stick_x, int stick_y, int substick_x,
    int substick_y, int trigger_l, int trigger_r) {
    if (port < 0 || port >= EGGY_PORTS) {
        return;
    }
    PADStatus* p = &s_pads[port];
    memset(p, 0, sizeof *p);
    p->button = (u16)((unsigned)buttons & EGGY_PAD_BUTTONS);
    p->stickX = clamp_s8(stick_x);
    p->stickY = clamp_s8(stick_y);
    p->substickX = clamp_s8(substick_x);
    p->substickY = clamp_s8(substick_y);
    p->triggerLeft = clamp_u8(trigger_l);
    p->triggerRight = clamp_u8(trigger_r);
    p->err = PAD_ERR_NONE;
}

/* ---- the picture --------------------------------------------------------
 * The page sizes the render to its screen: eggy_view(width, height), in
 * pixels, which is the picture's shape too. Widescreen follows the window
 * (park: mode 2), so a phone's 20:9 shows 20:9 of the stage, never a 16:9 cut
 * to fit or barred. Applied at the next frame boundary (vi.c), between two
 * presented frames. Presentation only: Melee's cameras and every query of them
 * keep GameCube semantics (cobj.c), so each phone of a match may have its own
 * shape and the simulations still agree. */
static int s_view_w;
static int s_view_h;

EMSCRIPTEN_KEEPALIVE int eggy_view(int width, int height) {
    if (width < 64 || height < 64 || width > 4096 || height > 4096) {
        return -1;
    }
    s_view_w = width;
    s_view_h = height;
    return 0;
}

/* ---- quality --------------------------------------------------------------
 * eggy_draw_every(n): while a match is played, one frame in n is drawn and the
 * others skip their render pass (gmscene.c); ticks are untouched. A count of
 * frames since the fight started picks which, so the pattern starts over with
 * every match. Presentation only, like the picture's size. */
static int s_draw_every = 1;
static uint32_t s_draw_count;

EMSCRIPTEN_KEEPALIVE int eggy_draw_every(int n) {
    if (n < 1 || n > 4) {
        return -1;
    }
    s_draw_every = n;
    return 0;
}

bool pc_eggy_skip_draw(void) {
    if (!pc_eggy_enabled() || s_state != EGGY_STATE_FIGHTING || s_draw_every <= 1) {
        s_draw_count = 0;
        return false;
    }
    return s_draw_count++ % (uint32_t)s_draw_every != 0;
}

/* eggy_lite (eggy.h), taken while parked for the matches after it: a phone's
 * own, like the picture's size. */
static int s_lite;

bool pc_eggy_lite_no_shadows(void) {
    return pc_eggy_enabled() && (s_lite & EGGY_LITE_NO_SHADOWS) != 0;
}

/* What drawing a match costs this phone (eggy_graphics_stats [13], [14]), so
 * lite's flags can be weighed on and off. */
static uint64_t s_draw_ns;
static int32_t s_drawn;

void pc_eggy_drew(uint64_t ns) {
    if (s_state == EGGY_STATE_FIGHTING) {
        s_draw_ns += ns;
        s_drawn++;
    }
}

/* A match on this phone alone runs one tick in each of its first passes after
 * GO (gmscene.c): however long the first frames take, the fight does not open
 * with ticks run at once and never drawn. Netplay keeps its session's pace. */
#define START_ONE_TICK_PASSES 3
static int s_start_passes = START_ONE_TICK_PASSES;

bool pc_eggy_start_one_tick(void) {
    if (!pc_eggy_enabled() || s_cfg.netplay || s_state != EGGY_STATE_FIGHTING ||
        s_start_passes >= START_ONE_TICK_PASSES)
    {
        return false;
    }
    s_start_passes++;
    return true;
}

void pc_eggy_frame_end(void) {
    if (!pc_eggy_enabled() || s_view_w == 0) {
        return;
    }
    const int w = s_view_w;
    const int h = s_view_h;
    s_view_w = s_view_h = 0;
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (windows != NULL && count > 0) {
        /* The canvas's own size, not SDL_GetWindowSize: SDL keeps the window
         * in CSS pixels, and a render the same size as the stage's CSS (360
         * lines on a stage 360 pixels tall) matched it while the canvas still
         * drew 480, so a phone's step down to 360 lines never took. */
        int now_w = 0;
        int now_h = 0;
        const char* canvas = SDL_GetStringProperty(
            SDL_GetWindowProperties(windows[0]), SDL_PROP_WINDOW_EMSCRIPTEN_CANVAS_ID_STRING, NULL);
        if (canvas == NULL ||
            emscripten_get_canvas_element_size(canvas, &now_w, &now_h) != EMSCRIPTEN_RESULT_SUCCESS)
        {
            now_w = now_h = 0;
        }
        if (now_w != w || now_h != h) {
            SDL_SetWindowSize(windows[0], w, h);
            s_pre.view_changed = true;
        }
    }
    SDL_free(windows);
}

/* ---- a fighter, as the page's tests read it ------------------------------
 * eggy_fighter(port): what the game made of the pads, so a test can drive the
 * real touch controls and check the fighter dashed rather than walked. Read
 * only, and never part of the simulation. A slow machine renders one frame in
 * several ticks, so what a page samples between frames misses ticks: every
 * motion state entered on any tick, and the highest the fighter rose, are
 * kept since the last read (watch_fighters, after each fresh tick). */
static int32_t s_fighter[EGGY_FIGHTER_WORDS];
static uint32_t s_seen[EGGY_PORTS][EGGY_FIGHTER_SEEN_WORDS];
static float s_peak[EGGY_PORTS];
static bool s_peaked[EGGY_PORTS];

static void watch_fighters(void) {
    if (!s_scene_live) {
        return;
    }
    for (int port = 0; port < EGGY_PORTS; port++) {
        HSD_GObj* gobj = Player_GetEntity(port);
        if (gobj == NULL || gobj->classifier != HSD_GOBJ_CLASS_FIGHTER) {
            continue;
        }
        const Fighter* fp = GET_FIGHTER(gobj);
        const int motion = fp->motion_id;
        if (motion >= 0 && motion < 32 * EGGY_FIGHTER_SEEN_WORDS) {
            s_seen[port][motion / 32] |= 1u << (motion % 32);
        }
        if (!s_peaked[port] || fp->cur_pos.y > s_peak[port]) {
            s_peak[port] = fp->cur_pos.y;
            s_peaked[port] = true;
        }
    }
}

EMSCRIPTEN_KEEPALIVE const int32_t* eggy_fighter(int port) {
    memset(s_fighter, 0, sizeof s_fighter);
    if (port < 0 || port >= EGGY_PORTS || !s_scene_live ||
        (s_state != EGGY_STATE_LOADED && s_state != EGGY_STATE_FIGHTING))
    {
        return s_fighter;
    }
    HSD_GObj* gobj = Player_GetEntity(port);
    if (gobj == NULL || gobj->classifier != HSD_GOBJ_CLASS_FIGHTER) {
        return s_fighter;
    }
    const Fighter* fp = GET_FIGHTER(gobj);
    s_fighter[0] = 1;
    s_fighter[1] = fp->motion_id;
    s_fighter[2] = fp->ground_or_air == GA_Air;
    s_fighter[3] = fp->facing_dir < 0 ? -1 : 1;
    s_fighter[4] = (int32_t)lroundf(fp->cur_pos.x * 1000);
    s_fighter[5] = (int32_t)lroundf(fp->cur_pos.y * 1000);
    s_fighter[6] = (int32_t)lroundf(fp->gr_vel * 1000);
    s_fighter[7] = (int32_t)lroundf(fp->input.lstick[0].x * 1000);
    s_fighter[8] = (int32_t)lroundf(fp->input.lstick[0].y * 1000);
    s_fighter[9] = fp->x1968_jumpsUsed;
    memcpy(&s_fighter[EGGY_FIGHTER_SEEN_AT], s_seen[port], sizeof s_seen[port]);
    s_fighter[EGGY_FIGHTER_SEEN_AT + EGGY_FIGHTER_SEEN_WORDS] =
        (int32_t)lroundf((s_peaked[port] ? s_peak[port] : fp->cur_pos.y) * 1000);
    memset(s_seen[port], 0, sizeof s_seen[port]);
    s_peaked[port] = false;
    return s_fighter;
}

EMSCRIPTEN_KEEPALIVE void eggy_tap(int port, int buttons) {
    if (port < 0 || port >= EGGY_PORTS) {
        return;
    }
    s_taps[port] |= (u16)((unsigned)buttons & EGGY_PAD_BUTTONS);
}

/* ---- HUD -------------------------------------------------------------- */

/* The fighter's CharacterKind now: Zelda and Sheik follow their changes. Only
 * while the scene's fighters exist: player slots keep stale entity pointers
 * between scenes (net_snapshot.c frame_checksum has the same guard). */
static int32_t current_ckind(int port) {
    const int32_t ckind = s_cfg.ports[port].ckind;
    if (ckind != CKind_Zelda && ckind != CKind_Seak) {
        return ckind;
    }
    HSD_GObj* gobj = Player_GetEntity(port);
    if (gobj == NULL || gobj->classifier != HSD_GOBJ_CLASS_FIGHTER) {
        return ckind;
    }
    return ftLib_GetKind(gobj) == Ft_Kind_Seak ? CKind_Seak : CKind_Zelda;
}

static void hud_update(void) {
    s_hud[0] = s_state;
    if (s_state == EGGY_STATE_ENDED) {
        /* Frozen at GAME!: the results screen reuses the player slots. */
        s_hud[1] = s_end_frame;
        s_hud[2] = -1;
        return;
    }
    if (s_state != EGGY_STATE_LOADED && s_state != EGGY_STATE_FIGHTING) {
        memset(&s_hud[1], 0, sizeof s_hud - sizeof s_hud[0]);
        s_hud[2] = -1;
        return;
    }
    if (!s_scene_live) {
        return; /* between the VS match and its sudden death: the last values */
    }
    s_hud[1] = s_frame_base + (int32_t)gm_801A4BA8();
    const StartMeleeRules* rules = gm_GetStartMeleeRules();
    s_hud[2] = rules->timer_enabled && !rules->timer_counts_up ? (int32_t)gm_8016AEEC() : -1;
    for (int port = 0; port < EGGY_PORTS; port++) {
        int32_t* h = &s_hud[EGGY_HUD_PORT_BASE + 4 * port];
        if (s_cfg.ports[port].kind == EGGY_PORT_NONE) {
            memset(h, 0, 4 * sizeof *h);
            continue;
        }
        h[0] = 1;
        h[1] = Player_GetStocks(port);
        h[2] = Player_GetDamage(port);
        h[3] = current_ckind(port);
    }
}

/* ---- the match -------------------------------------------------------- */

static void on_match_end(u8 outcome) {
    (void)outcome;
    /* GAME! or TIME!, in sudden death again if there is one. */
    s_end_frame = s_frame_base + (int32_t)gm_801A4BA8();
}

static u8 team_costume(int ckind, int team) {
    /* What character select dresses a team in (mncharsel.c). Sheik is not on
     * character select: she is Zelda held into the match, so Zelda's row. */
    const u8 k = (u8)(ckind == CKind_Seak ? CKind_Zelda : ckind);
    switch (team) {
    case 1:
        return gm_801692BC(k);
    case 2:
        return gm_80169290(k);
    default:
        return gm_80169264(k);
    }
}

static void build_match(StartMeleeData* start) {
    const EggyConfig* c = &s_cfg;
    StartMeleeRules* rules = &start->rules;

    gm_SetupRulesDefaults(rules);
    rules->stkind = (u16)c->stage;
    rules->is_vs = true; /* as gmVsMelee_EnterVs: teams' stock sharing */
    if (c->match_kind == EGGY_MATCH_STOCK) {
        rules->match_kind = MatchKind_Stock;
        rules->is_stock = true;
    } else {
        rules->match_kind = MatchKind_Time;
    }
    rules->timer_counts_up = false;
    if (c->time_minutes > 0) {
        rules->timer_enabled = true;
        rules->time_limit = (u32)c->time_minutes * 60;
    }
    rules->item_freq = (s8)c->item_freq;
    rules->x20 = c->item_mask;
    rules->is_teams = c->teams;
    rules->friendly_fire = c->friendly_fire;
    rules->sd_penalty = -1; /* Melee's default rule */
    rules->disable_pausing = true;
    rules->on_match_end = on_match_end;

    for (int i = 0; i < GM_MAX_PLAYERS; i++) {
        gm_SetupPlayerDefaults(&start->players[i]);
    }
    for (int port = 0; port < EGGY_PORTS; port++) {
        const EggyPort* src = &c->ports[port];
        PlayerInitData* p = &start->players[port];
        if (src->kind == EGGY_PORT_NONE) {
            continue;
        }
        p->slot_type = src->kind == EGGY_PORT_HUMAN ? Gm_PKind_Human : Gm_PKind_Cpu;
        p->ckind = (s8)src->ckind; /* Sheik is CKind_Seak: she loads as herself */
        p->color = c->teams ? team_costume(src->ckind, src->team) : (u8)src->costume;
        p->team = (u8)(c->teams ? src->team : 0);
        p->stocks = (s8)(c->match_kind == EGGY_MATCH_STOCK ? c->stocks : 0);
        if (src->kind == EGGY_PORT_CPU) {
            p->cpu_level = (u8)src->cpu_level;
            /* MELEE_EGGY_TEST=1: Melee's own Stay CPU, which keeps to its spot
             * and never attacks, so a test can drive the controls and read
             * what the fighter did without being hit. */
            const char* test = getenv("MELEE_EGGY_TEST");
            if (test != NULL && test[0] == '1') {
                p->cpu_kind = CpuKind_0;
            }
        }
        if (src->handicap > 0) {
            p->handicap = (s8)src->handicap;
            p->attack_ratio = fn_8016419C((u8)src->handicap);
            p->defense_ratio = fn_801641B4((u8)src->handicap);
        }
        p->rumble_enabled = false;
    }
    gm_SetupSubColors(start); /* teammates of one character get shades */
    s_start = *start;
}

/* The match's tags in aurora's pipeline cache (eggy.h "Pipeline tags"): what
 * it first needs is filed under them, and what is filed under any of them
 * already is built while it loads, ahead of the pre-roll that asks for it. */
static void pipeline_tags(void) {
    uint32_t tags[2 + 2 * EGGY_PORTS];
    uint32_t n = 0;
    tags[n++] = EGGY_TAG_STAGE | (uint32_t)s_cfg.stage;
    if (s_cfg.item_freq >= 0 && s_cfg.item_mask != 0) {
        tags[n++] = EGGY_TAG_ITEMS;
    }
    for (int port = 0; port < EGGY_PORTS; port++) {
        const EggyPort* p = &s_cfg.ports[port];
        if (p->kind == EGGY_PORT_NONE) {
            continue;
        }
        /* Zelda and Sheik are one fighter changing back and forth. */
        const int32_t also = p->ckind == CKind_Zelda ? CKind_Seak :
                             p->ckind == CKind_Seak  ? CKind_Zelda :
                                                       -1;
        const int32_t kinds[2] = {p->ckind, also};
        for (int k = 0; k < 2; k++) {
            if (kinds[k] < 0) {
                continue;
            }
            const uint32_t tag = EGGY_TAG_FIGHTER | (uint32_t)kinds[k];
            uint32_t i = 0;
            while (i < n && tags[i] != tag) {
                i++;
            }
            if (i == n) {
                tags[n++] = tag;
            }
        }
    }
    aurora_set_pipeline_tags(tags, n);
    aurora_prebuild_tagged();
}

static void preroll_begin(void) {
    memset(&s_pre, 0, sizeof s_pre);
    s_pre.active = true;
    s_pre.seed = *HSD_RandSeedPtr;
}

static int compare_u32(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a;
    const uint32_t y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

/* Whether the held frame is drawn and nothing for it is still being made. */
static bool preroll_judge(void) {
    if (s_pre.draws < 1 || s_pre.draws < s_preroll_min || aurora_demanded_pipelines_pending() != 0)
    {
        return false;
    }
    bool steady = false;
    if (s_pre.times_n >= PREROLL_CLEAN) {
        uint32_t sorted[PREROLL_CLEAN];
        memcpy(sorted, s_pre.times_us, sizeof sorted);
        qsort(sorted, PREROLL_CLEAN, sizeof sorted[0], compare_u32);
        s_pre.median_us = (int32_t)sorted[PREROLL_CLEAN / 2];
        s_pre.p90_us = (int32_t)sorted[PREROLL_CLEAN * 9 / 10];
        /* A frame of jitter at the 90th percentile is a phone's ordinary pace
         * (a vsync missed now and then), never a compile or an upload. */
        const int32_t half_again = s_pre.median_us + s_pre.median_us / 2;
        const int32_t one_more = s_pre.median_us + 17000;
        steady = s_pre.p90_us <= (half_again > one_more ? half_again : one_more);
    }
    if (steady && (s_pre.clean >= PREROLL_CLEAN || s_pre.draws >= PREROLL_SETTLE)) {
        return true;
    }
    return s_pre.draws >= PREROLL_GIVE_UP;
}

/* The held frame, drawn: the render pass and its XFB hand-off, then the
 * boundary when the hand-off did not meet one (the hold leaves every XFB
 * free), so each pass of a hold is one frame boundary. */
static void preroll_draw(void) {
    const uint64_t began = SDL_GetTicksNS();
    const uint32_t retraces = VIGetRetraceCount();
    const uint32_t skipped = aurora_draws_skipped();
    const uint32_t uploads = aurora_texture_uploads();
    gm_DrawFrame(false);
    if (VIGetRetraceCount() == retraces) {
        VIWaitForRetrace();
    }
    s_pre.draws++;
    s_pre.uploaded = aurora_texture_uploads() != uploads;
    const bool made = aurora_draws_skipped() != skipped || s_pre.uploaded;
    s_pre.clean = made ? 0 : s_pre.clean + 1;
    if (s_pre.last_ns != 0) {
        const uint64_t us = (began - s_pre.last_ns) / 1000u;
        s_pre.times_us[s_pre.times_n % PREROLL_CLEAN] = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
        s_pre.times_n++;
    }
    s_pre.last_ns = began;
}

/* One pass of a hold at frame 0: one frame boundary, the held frame drawn
 * until it is ready, then that picture kept up and drawn once a second. */
static void hold_frame(void) {
    if (s_pre.view_changed) {
        /* Another picture size: its frames are made, and weighed, again. */
        s_pre.view_changed = false;
        s_pre.ready = false;
        s_pre.clean = 0;
        s_pre.times_n = 0;
        s_pre.last_ns = 0;
    }
    if (s_pre.boundaries++ == 0) {
        VIWaitForRetrace(); /* the page's picture size first (pc_eggy_frame_end) */
        return;
    }
    if (s_pre.ready) {
        if (++s_pre.since_draw < PREROLL_REDRAW_EVERY) {
            VIWaitForRetrace();
            return;
        }
        s_pre.since_draw = 0;
        s_pre.last_ns = 0; /* a second since the last draw is no frame time */
        preroll_draw();
        /* A draw skipped for a pipeline that failed is skipped every time:
         * only new work (a pipeline asked for, a texture) undoes ready. */
        if (s_pre.uploaded || aurora_demanded_pipelines_pending() != 0) {
            s_pre.ready = false;
            s_pre.times_n = 0;
            pc_log_line("eggy: the held frame asked for more after %d pre-roll draws", s_pre.draws);
        }
        return;
    }
    preroll_draw();
    if (preroll_judge()) {
        s_pre.ready = true;
        s_pre.since_draw = 0;
        pc_log_line("eggy: graphics ready after %d pre-roll draws (%d made nothing new, frames "
                    "%.1f ms, 90%% under %.1f ms)",
            s_pre.draws, s_pre.clean, s_pre.median_us / 1000.0, s_pre.p90_us / 1000.0);
    }
}

static void preroll_end(void) {
    if (s_pre.draws > 0 && *HSD_RandSeedPtr != s_pre.seed) {
        pc_log_line("eggy: pre-roll drew random numbers (seed %08x -> %08x), put back",
            (unsigned)s_pre.seed, (unsigned)*HSD_RandSeedPtr);
        *HSD_RandSeedPtr = s_pre.seed;
    }
    s_pre.active = false;
}

static void graphics_log_go(void) {
    pc_log_line("eggy: go after %d pre-roll draws (%s), lite %d: %u pipelines waiting, %u "
                "compiled, %u failed, %u ms compiling, %u draws skipped, %u textures",
        s_pre.draws, s_pre.ready ? "ready" : "not ready", s_lite,
        (unsigned)aurora_demanded_pipelines_pending(), (unsigned)aurora_pipelines_finished(),
        (unsigned)aurora_pipelines_failed(), (unsigned)aurora_compile_ms_total(),
        (unsigned)aurora_draws_skipped(), (unsigned)aurora_texture_uploads());
}

static void graphics_log_match(void) {
    pc_log_line("eggy: the match drew %d frames at %.2f ms each (lite %d), and asked for %u "
                "pipelines after go (%u known but not built, %u new to this phone)",
        s_drawn, s_drawn > 0 ? (double)s_draw_ns / 1e6 / s_drawn : 0.0, s_lite,
        (unsigned)aurora_demanded_after_go(), (unsigned)aurora_demanded_after_go_known(),
        (unsigned)aurora_demanded_after_go_new());
}

static void net_resync_event(int event, int32_t a, int32_t b);

/* Netplay: open the session for the config just taken (eggy_net.c), the
 * game thread yielding every frame. 0 when it is up, -1 when the page
 * aborted, else the EggyNetError (the session is closed either way). */
static int net_open(void) {
    pc_net_set_resync_hook(net_resync_event);
    s_test_desync = false;
    eggy_net_begin(&s_cfg);
    for (;;) {
        if (s_abort) {
            eggy_net_drop(false);
            return -1;
        }
        const int r = eggy_net_connect_step();
        if (r > 0) {
            return 0;
        }
        if (r < 0) {
            return -r;
        }
        VIWaitForRetrace();
    }
}

/* Wait here, between scenes, presenting the last picture, until the page
 * loads a valid config (and, for netplay, the session is up). */
static void park(void) {
    static bool started;
    if (!started) {
        started = true;
        pc_widescreen_set_mode(2); /* the window's shape, which the page sets (eggy_view) */
    }
    if (s_matches > 0) {
        /* What a match end stops (gmvs.c fn_8016C7F0): the results screen's
         * music, and anything an abort left playing. */
        lbAudioAx_800236DC();
        lbAudioAx_80023694();
    }
    s_scene_live = false;
    if (s_cfg.netplay) {
        /* The last match's session: both phones leave its last scene on the
         * same frame, and this waits for the other one to say so. */
        s_net_closing = true;
        while (!eggy_net_close_step()) {
            VIWaitForRetrace();
        }
        s_net_closing = false;
    }
    for (;;) {
        s_abort = false;
        if (s_state != EGGY_STATE_ERROR) {
            set_state(EGGY_STATE_PARKED);
        }
        hud_update();
        eggy_js_state(JS_PARKED, s_serial, 0);
        /* Nothing is drawn here, so no frame is presented either: the canvas
         * keeps the last picture and the page its time (pc_frame_idle). */
        while (!s_pending_ok) {
            pc_frame_idle();
        }
        s_cfg = s_pending;
        s_serial = s_pending_serial;
        s_pending_ok = false;
        s_sudden_death = false;
        s_net_lost = false;
        s_net_failed = false;
        s_frame_base = 0;
        s_end_frame = 0;
        s_results_ticks = 0;
        memset(s_results, 0, sizeof s_results);
        memset(s_tick_pads, 0, sizeof s_tick_pads);
        pipeline_tags();
        set_state(EGGY_STATE_LOADING);
        hud_update();
        if (!s_cfg.netplay) {
            return;
        }
        const int err = net_open();
        if (err == 0) {
            return;
        }
        if (err > 0) {
            eggy_js_state(JS_NET_ERROR, err, s_serial);
        }
        /* parked again: 'parked' follows */
    }
}

static void enter_vs(GameModeState* state) {
    StartMeleeData* start = gm_GetGameModeStateEnterData(state);

    park();
    /* Identical unlock state on every device (what netplay pins too,
     * net_handshake.c): unlock predicates gate RNG draws. */
    pc_unlock_state_set(pc_unlock_state_all());
    pc_audio_set_music_volume((float)s_cfg.music_volume / 127.0f);
    pc_audio_set_sfx_volume((float)s_cfg.sfx_volume / 127.0f);
    build_match(start);
    gm_LoadAnnouncer();
    s_matches++;
    /* Last, so the stream starts with the scene: MELEE_SEED's effect. A
     * netplay session puts the same seed back entering frame 0 (net.c). */
    *HSD_RandSeedPtr = s_cfg.netplay ? eggy_net_seed(&s_cfg) : s_cfg.seed;
}

/* onState('away', port, frame) for every phone settled away since the last
 * look, this one's own included. */
static void net_away_report(void) {
    int32_t frame;
    for (int port; (port = eggy_net_next_away(&frame)) >= 0;) {
        pc_log_line("eggy: port %d settled away after frame %d", port, frame);
        eggy_js_state(JS_AWAY, port, frame);
    }
}

/* The netcode's resync events (net.c "resync"): the leader's snapshot is
 * ready for the page to send; this phone has applied the leader's; or, on the
 * leader, the hold for it is over. 'resync' says this phone plays on from the
 * leader's state of that frame. */
static void net_resync_event(int event, int32_t a, int32_t b) {
    if (event == PC_NET_RESYNC_SNAPSHOT) {
        eggy_js_state(JS_SNAPSHOT, a, b); /* size, frame */
    } else if (event == PC_NET_RESYNC_APPLIED || event == PC_NET_RESYNC_RELEASED) {
        eggy_js_state(JS_RESYNC, a, s_serial); /* frame */
    }
}

/* Netplay's hold: until every phone is loaded and every page said go
 * (eggy_net.c). A failure unwinds the loaded scene as an abort does. */
static void net_hold(void) {
    for (;;) {
        if (s_abort) {
            eggy_net_drop(false);
            return;
        }
        bool loaded = false;
        const int r = eggy_net_hold_step(s_go, &loaded);
        net_away_report();
        if (loaded) {
            eggy_js_state(JS_LOADED, s_serial, 0);
        }
        if (r > 0) {
            return;
        }
        if (r < 0) {
            s_net_failed = true;
            s_abort = true;
            eggy_js_state(JS_NET_ERROR, -r, s_serial);
            return;
        }
        hold_frame();
    }
}

/* Netplay, once per tick of the match: a desync is reported, a phone
 * settled away is reported (the rest play on), and a match that cannot go on
 * here before it was decided -- this phone settled away by the others, its
 * session gone, or no other phone left -- ends where it stands. */
static void net_tick(void) {
    int32_t frame = 0;
    const int event = eggy_net_tick(&frame);
    net_away_report();
    if (event > 0) {
        eggy_js_state(JS_DESYNC, frame, s_serial);
    } else if (event < 0 && s_state == EGGY_STATE_FIGHTING && s_scene_live && !s_abort &&
               !s_net_lost && gmVs_GetSceneController()->state.match_result == OUTCOME_NONE)
    {
        s_net_lost = true;
        frame = s_frame_base + (int32_t)gm_801A4BA8();
        const bool evicted = eggy_net_evicted();
        pc_log_line("eggy: %s at match frame %d (status %d)",
            evicted ? "this phone was settled away" : "no other phone is left", frame,
            pc_net_peer_status());
        if (!evicted) {
            eggy_js_state(JS_PEER_LOST, frame, s_serial); /* 'away' told this one already */
        }
        /* Melee's own early end: the match is scored as it stands (gmvs.c
         * gm_GetMatchOutcome, OUTCOME_TERMINATED), and GAME! closes it on
         * this phone alone. */
        gm_8016B328();
    }
}

void pc_eggy_scene_begin(int scene_kind) {
    if (!pc_eggy_enabled()) {
        return;
    }
    s_scene_live = scene_kind == GS_VS || scene_kind == GS_SUDDEN_DEATH;
    if (scene_kind != GS_VS || s_state != EGGY_STATE_LOADING) {
        return;
    }
    /* Loaded: the very first tick of the match is next. Held here, it is
     * pre-rolled (hold_frame). A go latched while it loaded releases it with
     * no pre-roll at all. */
    set_state(EGGY_STATE_LOADED);
    hud_update();
    preroll_begin();
    if (s_cfg.netplay) {
        net_hold();
    } else if (!s_abort) {
        eggy_js_state(JS_LOADED, s_serial, 0);
        while (!s_go && !s_abort) {
            hold_frame();
        }
    }
    preroll_end();
    s_go = false;
    s_start_passes = 0;
    s_draw_ns = 0;
    s_drawn = 0;
    aurora_mark_go();
    if (!s_abort) {
        graphics_log_go();
    }
    set_state(EGGY_STATE_FIGHTING);
}

/* eggy_test_desync (MELEE_EGGY_TEST=1): this phone's own fighter takes 10%
 * the other phones never simulate, on a tick no rollback can undo, so this
 * phone's match parts from theirs from here on: a real desync for the resync
 * to repair, on demand. */
static void test_desync_apply(void) {
    if (!s_test_desync || !s_scene_live || !pc_net_frame_final(pc_net_frame())) {
        return;
    }
    HSD_GObj* gobj = Player_GetEntity(s_cfg.local_port);
    if (gobj == NULL || gobj->classifier != HSD_GOBJ_CLASS_FIGHTER) {
        return;
    }
    Fighter* fp = GET_FIGHTER(gobj);
    fp->dmg.x1830_percent += 10.0f;
    s_test_desync = false;
    pc_log_line("eggy: test desync, port %d's percent moved on this phone alone at frame %d",
        s_cfg.local_port, pc_net_frame());
}

void pc_eggy_after_tick(void) {
    if (!pc_eggy_enabled()) {
        return;
    }
    /* A rollback re-runs ticks; what is decided here happens on fresh ones
     * only. (The HUD may follow a re-run: it reads the state as it stands.) */
    const bool fresh = !(s_cfg.netplay && pc_net_resim());
    if (s_state == EGGY_STATE_FIGHTING) {
        /* The slot this tick consumed (HSD_PadRenewMasterStatus), for
         * eggy_checksum: frame_checksum folds the pads as simulated. */
        const PadLibData* p = &HSD_PadLibData;
        if (p->queue != NULL && p->qnum != 0) {
            memcpy(
                s_tick_pads, p->queue[(p->qread + p->qnum - 1) % p->qnum].stat, sizeof s_tick_pads);
        }
        if (fresh) {
            watch_fighters();
        }
        if (s_cfg.netplay && fresh) {
            net_tick();
            test_desync_apply();
        }
        if (s_abort && fresh) {
            if (s_cfg.netplay) {
                eggy_net_drop(false); /* the other phone sees this one leave */
            }
            gm_801A4B74(); /* end the scene now, without drawing this tick */
        }
    } else if (s_state == EGGY_STATE_ENDED) {
        if (s_cfg.netplay && fresh) {
            int32_t frame;
            eggy_net_tick(&frame); /* keep the lane drained; the match is decided */
            net_away_report();
        }
        if (s_abort && fresh) {
            if (s_cfg.netplay) {
                eggy_net_drop(false);
            }
            gm_801A4B74();
        } else if (fresh && ++s_results_ticks >= s_cfg.results_seconds * 60) {
            gm_801A4B60(); /* the results screen's own end */
        }
    }
    hud_update();
}

static void fill_results(const MatchEnd* end, int outcome) {
    memset(s_results, 0, sizeof s_results);
    s_results[0] = s_end_frame;
    s_results[1] = outcome;
    for (int port = 0; port < EGGY_PORTS; port++) {
        const MatchPlayerData* p = &end->player_standings[port];
        int32_t* r = &s_results[EGGY_RESULTS_PORT_BASE + 8 * port];
        if (p->pkind == Gm_PKind_NA) {
            continue;
        }
        /* Placement as the results screen ranks it (gmresult.c): by score,
         * ties shared, a team's for each of its members. */
        const u8 rank =
            end->is_teams == 1 ? end->team_standings[p->team].is_big_loser : p->is_big_loser;
        r[0] = 1;
        r[1] = rank + 1;
        r[2] = p->stocks;
        r[3] = (int32_t)p->x20;   /* KOs (gm_80166378) */
        r[4] = p->x24;            /* falls */
        r[5] = p->self_destructs; /* self-destructs */
        r[6] = (int32_t)p->x40;   /* damage dealt (pl_80040B3C) */
        r[7] = (int32_t)p->x44;   /* damage taken (pl_80040B64) */
    }
}

/* The match is decided: standings out, then the results screen or the park. */
static void finish(const MatchEnd* end) {
    int outcome = s_net_lost                      ? EGGY_OUTCOME_DISCONNECTED :
                  s_sudden_death                  ? EGGY_OUTCOME_SUDDEN_DEATH :
                  end->outcome == OUTCOME_TIMEOUT ? EGGY_OUTCOME_TIME_OUT :
                                                    EGGY_OUTCOME_NORMAL;
    if (s_end_frame == 0) {
        s_end_frame = s_frame_base; /* no GAME! was called: the last tick */
    }
    graphics_log_match();
    fill_results(end, outcome);
    set_state(EGGY_STATE_ENDED);
    hud_update();
    eggy_js_state(JS_ENDED, outcome, s_serial);
    /* A lost session has no results screen: the other phones are not there. */
    gm_SetNextGameModeStateId(
        s_cfg.results_seconds > 0 && !s_net_lost ? MODE_STATE_RESULTS : MODE_STATE_VS);
}

static void finish_aborted(void) {
    graphics_log_match();
    memset(s_results, 0, sizeof s_results);
    s_results[0] = s_frame_base;
    s_results[1] = s_net_failed ? EGGY_OUTCOME_DISCONNECTED : EGGY_OUTCOME_ABORTED;
    gm_SetNextGameModeStateId(MODE_STATE_VS);
}

static void exit_vs(GameModeState* state) {
    MatchExitInfo* info = gm_GetGameModeStateExitData(state);
    s_scene_live = false;
    s_frame_base += (int32_t)gm_801A4BA8();
    if (s_abort) {
        finish_aborted();
    } else if (!s_net_lost && gm_MatchHasMultipleWinners(&info->match_end)) {
        s_sudden_death = true;
        gm_SetNextGameModeStateId(MODE_STATE_SUDDEN_DEATH);
    } else {
        finish(&info->match_end);
    }
}

static void enter_sudden_death(GameModeState* state) {
    /* gmVsMelee_EnterSuddenDeath, from the match as built rather than from
     * VS mode's saved data. */
    StartMeleeData* start = gm_GetGameModeStateEnterData(state);
    *start = s_start;
    /* VS mode sets these two on the VS scene's copy only (gmVsMelee_EnterVs),
     * so its sudden death runs without them. */
    start->rules.is_vs = false;
    start->rules.is_stock = false;
    gm_SetupSuddenDeath(start, &gmVsMelee_VsExitInfo.match_end);
}

static void exit_sudden_death(GameModeState* state) {
    s_scene_live = false;
    s_frame_base += (int32_t)gm_801A4BA8();
    if (s_abort) {
        finish_aborted();
        return;
    }
    gmVsMelee_ExitSuddenDeath(state); /* folds it into the VS match's standings */
    finish(&gmVsMelee_VsExitInfo.match_end);
}

static void enter_results(GameModeState* state) {
    gmVsMelee_EnterResults(state);
    s_results_ticks = 0;
}

static void exit_results(GameModeState* state) {
    (void)state;
    s_scene_live = false;
    gm_SetNextGameModeStateId(MODE_STATE_VS);
}

GameModeState gm_Mode_Eggy_States[] = {
    {
        MODE_STATE_VS,
        lbDvdPreload_2,
        0,
        enter_vs,
        exit_vs,
        {GS_VS, &gmVsMelee_StartData, &gmVsMelee_VsExitInfo},
    },
    {
        MODE_STATE_SUDDEN_DEATH,
        lbDvdPreload_2,
        0,
        enter_sudden_death,
        exit_sudden_death,
        {GS_SUDDEN_DEATH, &gmVsMelee_StartData, &gmVsMelee_SuddenDeathExitInfo},
    },
    {
        MODE_STATE_RESULTS,
        lbDvdPreload_2,
        0,
        enter_results,
        exit_results,
        {GS_RESULTS, &gmVsMelee_ResultsEnterData, NULL},
    },
    {GM_GAMEMODESTATE_TERMINATE},
};

/* ---- exports ---------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE int32_t* eggy_config_buffer(void) {
    return s_config_words;
}

EMSCRIPTEN_KEEPALIVE int eggy_costumes(int ckind) {
    if (ckind < 0 || ckind >= EGGY_CKIND_COUNT) {
        return 0;
    }
    return gm_GetNumCostumesForCKind((u8)ckind);
}

EMSCRIPTEN_KEEPALIVE int eggy_team_costume(int ckind, int team) {
    if (ckind < 0 || ckind >= EGGY_CKIND_COUNT || team < 0 || team > 2) {
        return -1;
    }
    return team_costume(ckind, team);
}

EMSCRIPTEN_KEEPALIVE int eggy_load(int serial) {
    if (!pc_eggy_enabled()) {
        return EGGY_ERR_DISABLED;
    }
    if (s_state != EGGY_STATE_PARKED && s_state != EGGY_STATE_ERROR) {
        return EGGY_ERR_BUSY;
    }
    uint8_t costumes[EGGY_CKIND_COUNT];
    for (int k = 0; k < EGGY_CKIND_COUNT; k++) {
        costumes[k] = (uint8_t)eggy_costumes(k);
    }
    EggyConfig cfg;
    const int err = eggy_config_parse(s_config_words, costumes, &cfg);
    if (err != 0) {
        s_pending_ok = false;
        set_state(EGGY_STATE_ERROR);
        pc_log_line("eggy: config refused, error %d (serial %d)", err, serial);
        eggy_js_state(JS_ERROR, err, serial);
        return err;
    }
    s_pending = cfg;
    s_pending_serial = serial;
    s_pending_ok = true;
    s_go = false;
    s_abort = false;
    memset(s_pads, 0, sizeof s_pads);
    return 0;
}

EMSCRIPTEN_KEEPALIVE int eggy_go(void) {
    /* Latched: a go that arrives while the match still loads releases it the
     * moment it is loaded ('loaded' is still reported first). */
    if (s_state == EGGY_STATE_LOADING || s_state == EGGY_STATE_LOADED ||
        ((s_state == EGGY_STATE_PARKED || s_state == EGGY_STATE_ERROR) && s_pending_ok))
    {
        s_go = true;
        return 1;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE int eggy_abort(void) {
    switch (s_state) {
    case EGGY_STATE_LOADING:
    case EGGY_STATE_LOADED:
    case EGGY_STATE_FIGHTING:
    case EGGY_STATE_ENDED:
        s_abort = true;
        if (s_cfg.netplay && !s_net_closing && pc_net_active()) {
            pc_net_leave(); /* not after the stall and resume windows, now */
        }
        return 1;
    default:
        if (s_pending_ok) {
            s_pending_ok = false; /* loaded, not yet started: forget it */
            return 1;
        }
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE const int32_t* eggy_hud(void) {
    if (pc_eggy_enabled()) {
        hud_update();
    }
    return s_hud;
}

EMSCRIPTEN_KEEPALIVE const int32_t* eggy_results(void) {
    return s_results;
}

EMSCRIPTEN_KEEPALIVE uint32_t eggy_checksum(void) {
    /* Netplay's desync checksum (net_snapshot.c): the pads as simulated, the
     * RNG seed and every fighter's position, facing, percent, action,
     * stocks and velocities. 0 outside a match. */
    if (!pc_eggy_enabled() || !fighting()) {
        return 0;
    }
    return frame_checksum(s_tick_pads);
}

EMSCRIPTEN_KEEPALIVE const int32_t* eggy_net_stats(void) {
    return eggy_net_stats_words();
}

/* ---- resync (EGGY.md "Resync") ---- */

static bool netplay_running(void) {
    return pc_eggy_enabled() && s_cfg.netplay && pc_net_active();
}

/* The leader: a snapshot as a desync would ask for one. 0 asked ('snapshot'
 * follows at the next frame that is final), -1 not the acting leader of a
 * running match, or the match's three are spent. */
EMSCRIPTEN_KEEPALIVE int eggy_snapshot_begin(void) {
    return netplay_running() ? pc_net_resync_request() : -1;
}

EMSCRIPTEN_KEEPALIVE int eggy_snapshot_size(void) {
    size_t len = 0;
    pc_net_resync_image(&len, NULL);
    return (int)len;
}

EMSCRIPTEN_KEEPALIVE const uint8_t* eggy_snapshot_buffer(void) {
    return pc_net_resync_image(NULL, NULL);
}

EMSCRIPTEN_KEEPALIVE int32_t eggy_snapshot_frame(void) {
    int32_t frame = -1;
    pc_net_resync_image(NULL, &frame);
    return frame;
}

/* A receiver: room for len bytes of the leader's image, NULL if none. */
EMSCRIPTEN_KEEPALIVE uint8_t* eggy_snapshot_reserve(int len) {
    return len > 0 ? pc_net_resync_reserve((size_t)len) : NULL;
}

/* A receiver: the reassembled image of `frame`. 0 taken ('resync' follows
 * once it is applied), or a PC_NET_RESYNC_ERR_* (EGGY.md). */
EMSCRIPTEN_KEEPALIVE int eggy_snapshot_apply(const uint8_t* ptr, int len, int32_t frame) {
    if (!netplay_running()) {
        return PC_NET_RESYNC_ERR_SESSION;
    }
    return len > 0 ? pc_net_resync_offer(ptr, (size_t)len, frame) : PC_NET_RESYNC_ERR_IMAGE;
}

/* MELEE_EGGY_TEST=1 only: make this phone desync (test_desync_apply). */
EMSCRIPTEN_KEEPALIVE int eggy_test_desync(void) {
    const char* on = getenv("MELEE_EGGY_TEST");
    if (on == NULL || on[0] != '1' || !netplay_running() || s_state != EGGY_STATE_FIGHTING) {
        return 0;
    }
    s_test_desync = true;
    return 1;
}

EMSCRIPTEN_KEEPALIVE void eggy_audio(int music_volume, int sfx_volume) {
    const int music = music_volume < 0 ? 0 : music_volume > 127 ? 127 : music_volume;
    const int sfx = sfx_volume < 0 ? 0 : sfx_volume > 127 ? 127 : sfx_volume;
    pc_audio_set_music_volume((float)music / 127.0f);
    pc_audio_set_sfx_volume((float)sfx / 127.0f);
}

/* ---- graphics (eggy.h "Pre-roll", "Graphics") ---- */

/* How many words eggy_perf() fills, for a page that reads more than one engine. */
EMSCRIPTEN_KEEPALIVE int eggy_perf_words(void) {
    return EGGY_PERF_WORDS;
}

static bool parked(void) {
    return s_state == EGGY_STATE_PARKED || s_state == EGGY_STATE_ERROR;
}

EMSCRIPTEN_KEEPALIVE int eggy_lite(int flags) {
    if (pc_eggy_enabled() && parked()) {
        const int lite = flags & (EGGY_LITE_NO_SHADOWS | EGGY_LITE_NO_FOG);
        if (lite != s_lite) {
            s_lite = lite;
            aurora_set_fog_enabled((lite & EGGY_LITE_NO_FOG) == 0);
            pc_log_line("eggy: lite %d (shadows %s, fog %s)", lite,
                lite & EGGY_LITE_NO_SHADOWS ? "off" : "on", lite & EGGY_LITE_NO_FOG ? "off" : "on");
        }
    }
    return s_lite;
}

EMSCRIPTEN_KEEPALIVE int eggy_graphics_ready(void) {
    return pc_eggy_enabled() && s_state == EGGY_STATE_LOADED && s_pre.active && s_pre.ready;
}

EMSCRIPTEN_KEEPALIVE int eggy_compile_cap(int n) {
    if (n < 0 || n > 16) {
        return -1;
    }
    aurora_set_compile_cap((uint32_t)n);
    return 0;
}

EMSCRIPTEN_KEEPALIVE int eggy_background_builds(int on) {
    s_background_ok = on != 0;
    background_builds_follow();
    return 0;
}

EMSCRIPTEN_KEEPALIVE int eggy_pipeline_cache_flush(void) {
    return aurora_pipeline_cache_flush();
}

EMSCRIPTEN_KEEPALIVE int eggy_preroll_min(int n) {
    if (n < 0 || n > 100000) {
        return -1;
    }
    s_preroll_min = n;
    return 0;
}

static int32_t s_graphics[EGGY_GRAPHICS_WORDS];

EMSCRIPTEN_KEEPALIVE const int32_t* eggy_graphics_stats(void) {
    memset(s_graphics, 0, sizeof s_graphics);
    s_graphics[0] = (int32_t)aurora_demanded_pipelines_pending();
    s_graphics[1] = (int32_t)aurora_draws_skipped();
    s_graphics[2] = (int32_t)aurora_texture_uploads();
    s_graphics[3] = (int32_t)aurora_demanded_after_go();
    s_graphics[4] = (int32_t)aurora_pipelines_finished();
    s_graphics[5] = (int32_t)aurora_pipelines_failed();
    s_graphics[6] = (int32_t)aurora_compile_ms_total();
    s_graphics[7] = s_pre.draws;
    s_graphics[8] = eggy_graphics_ready();
    s_graphics[9] = s_pre.clean;
    s_graphics[10] = s_pre.median_us;
    s_graphics[11] = s_pre.p90_us;
    s_graphics[12] = s_lite;
    s_graphics[13] = s_draw_ns / 1000u > INT32_MAX ? INT32_MAX : (int32_t)(s_draw_ns / 1000u);
    s_graphics[14] = s_drawn;
    s_graphics[15] = (int32_t)aurora_background_pipelines_left();
    s_graphics[16] = (int32_t)aurora_demanded_after_go_known();
    s_graphics[17] = (int32_t)aurora_demanded_after_go_new();
    return s_graphics;
}

/* How many words eggy_graphics_stats() fills: an engine without this fills 16. */
EMSCRIPTEN_KEEPALIVE int eggy_graphics_words(void) {
    return EGGY_GRAPHICS_WORDS;
}

void pc_eggy_frame_facts(int32_t* state, int32_t* match_frame) {
    *state = s_hud[0];
    *match_frame = s_hud[1];
}

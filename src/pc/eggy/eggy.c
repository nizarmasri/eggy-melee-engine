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
#include <dolphin/vi.h>
#include <SDL3/SDL_hints.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wscalar-storage-order" /* disc-struct unions in lb/types.h */
#include <melee/ft/forward.h>
#include <melee/ft/ftlib.h>
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
};

// clang-format off
EM_JS(void, eggy_js_state, (int kind, int a, int b), {
  const names = ['parked', 'loaded', 'ended', 'error', 'net-error', 'desync', 'peer-lost', 'away'];
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
}

static void set_state(int state) {
    s_state = state;
    s_hud[0] = state;
}

static bool fighting(void) {
    return s_scene_live && (s_state == EGGY_STATE_LOADED || s_state == EGGY_STATE_FIGHTING);
}

bool pc_eggy_hold_frame(void) {
    /* Parked, the last picture (GAME! or the results screen) stays up. Not
     * while a match loads or holds: those frames clear to black, so frame 0
     * draws into a cleared colour and depth buffer, not over the last one. */
    return pc_eggy_enabled() &&
           (s_state == EGGY_STATE_PARKED || s_state == EGGY_STATE_ERROR || s_net_closing);
}

bool pc_eggy_music_off(void) {
    return pc_eggy_enabled() && !s_cfg.music;
}

bool pc_eggy_frozen_stadium(void) {
    return s_cfg.frozen_stadium;
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
    for (int i = 0; i < EGGY_PORTS; i++) {
        const int from = net ? (i == 0 ? s_cfg.local_port : -1) : i;
        if (live && from >= 0) {
            status[i] = s_pads[from];
        } else {
            memset(&status[i], 0, sizeof status[i]);
        }
        status[i].err = PAD_ERR_NONE;
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

/* Netplay: open the session for the config just taken (eggy_net.c), the
 * game thread yielding every frame. 0 when it is up, -1 when the page
 * aborted, else the EggyNetError (the session is closed either way). */
static int net_open(void) {
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
        pc_widescreen_set_mode(1); /* fixed 16:9, never the window's shape */
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
        while (!s_pending_ok) {
            VIWaitForRetrace();
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
        VIWaitForRetrace();
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
    /* Loaded: the very first tick of the match is next. */
    set_state(EGGY_STATE_LOADED);
    hud_update();
    if (s_cfg.netplay) {
        net_hold();
    } else if (!s_abort) {
        eggy_js_state(JS_LOADED, s_serial, 0);
        while (!s_go && !s_abort) {
            VIWaitForRetrace();
        }
    }
    s_go = false;
    set_state(EGGY_STATE_FIGHTING);
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
        if (s_cfg.netplay && fresh) {
            net_tick();
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
    fill_results(end, outcome);
    set_state(EGGY_STATE_ENDED);
    hud_update();
    eggy_js_state(JS_ENDED, outcome, s_serial);
    /* A lost session has no results screen: the other phones are not there. */
    gm_SetNextGameModeStateId(
        s_cfg.results_seconds > 0 && !s_net_lost ? MODE_STATE_RESULTS : MODE_STATE_VS);
}

static void finish_aborted(void) {
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

EMSCRIPTEN_KEEPALIVE void eggy_audio(int music_volume, int sfx_volume) {
    const int music = music_volume < 0 ? 0 : music_volume > 127 ? 127 : music_volume;
    const int sfx = sfx_volume < 0 ? 0 : sfx_volume > 127 ? 127 : sfx_volume;
    pc_audio_set_music_volume((float)music / 127.0f);
    pc_audio_set_sfx_volume((float)sfx / 127.0f);
}

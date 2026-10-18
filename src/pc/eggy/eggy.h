/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Eggy host API: the page that embeds this engine (Eggy) configures, starts,
 * feeds and reads real VS matches through these exports, and the engine
 * reports back through Module.eggy.onState(name, a, b). EGGY.md at the repo
 * root has the call order and the list of upstream files this touches.
 *
 * On with MELEE_EGGY=1 together with MELEE_VS_ONLY=1 and MELEE_BOOT_SCENE=eggy.
 * Off, every hook below returns at once and the engine is melee-web's.
 *
 * Config buffer, layout v1 (int32 words at eggy_config_buffer()):
 *    [0] version, 1
 *    [1] seed (uint32): the game RNG is set to it as the match loads
 *    [2] stage, a StKind: 2..32 except 21 (Akaneia) and 26 (Icicle Mountain top)
 *    [3] match kind: 0 stock, 1 time
 *    [4] stocks, 1..99 (stock matches; time matches accept 0..99 and ignore it)
 *    [5] time limit in minutes, 0..99 (0: no clock)
 *    [6] item frequency: -1 off, 0..4 very low .. very high
 *    [7] item mask, low 32 bits, and [8] the high 32: bit n allows ItemKind n
 *        (StartMeleeRules.x20). Both -1 is every item.
 *    [9] teams 0/1, [10] friendly fire 0/1, [11] frozen Pokemon Stadium 0/1
 *   [12] music on this device 0/1 (0: no music stream is ever started, so no
 *        music file is read)
 *   [13] music volume 0..127, [14] sound effect volume 0..127
 *   [15] results screen seconds, 0..600: 0 parks right after GAME!, more shows
 *        Melee's results screen for that long
 *   [16 + 8 * port], port 0..3:
 *        +0 kind: 0 none, 1 human, 3 CPU
 *        +1 CharacterKind 0..25 (18 Zelda, 19 Sheik: each starts as itself)
 *        +2 costume, 0..eggy_costumes(ckind) - 1 (with teams on, the team's
 *           costume is worn instead: eggy_team_costume)
 *        +3 team: 0 red, 1 blue, 2 green
 *        +4 CPU level: 1..9 for a CPU, 0..9 (ignored) for a human
 *        +5 handicap: 0 off, 1..9 Melee's handicap levels
 *        +6, +7 reserved, 0
 * Two fighters at least; with teams on, two teams at least.
 *
 * HUD (int32 words at eggy_hud(), current at every tick):
 *   [0] state: 0 parked, 1 loading, 2 loaded (held at frame 0), 3 fighting,
 *       4 ended (results screen), 5 error (parked after an invalid config)
 *   [1] match frame: ticks simulated since frame 0, through sudden death
 *   [2] seconds left on the clock, -1 with no clock
 *   [3 + 4 * port]: present, stocks, percent (as the game shows it), the
 *       fighter's CharacterKind now (18/19 follow Zelda and Sheik's changes)
 *
 * Results (int32 words at eggy_results(), filled before onState('ended')):
 *   [0] end frame: the match frame on which GAME! or TIME! came
 *   [1] outcome: 0 normal, 1 time out, 2 sudden death then decided, 3 aborted
 *   [2 + 8 * port]: present, placement (1..4, teammates share it), stocks
 *       left, KOs, falls, self-destructs, damage dealt, damage taken
 *   From Melee's own match end (MatchEnd.player_standings).
 */
#ifndef PC_EGGY_EGGY_H
#define PC_EGGY_EGGY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EGGY_CONFIG_VERSION 1
#define EGGY_CONFIG_WORDS 48
#define EGGY_CONFIG_PORT_BASE 16
#define EGGY_CONFIG_PORT_WORDS 8
#define EGGY_PORTS 4
#define EGGY_HUD_WORDS (3 + 4 * EGGY_PORTS)
#define EGGY_HUD_PORT_BASE 3
#define EGGY_RESULTS_WORDS (2 + 8 * EGGY_PORTS)
#define EGGY_RESULTS_PORT_BASE 2

/* CharacterKind 0..25, Sheik included. */
#define EGGY_CKIND_COUNT 26

enum EggyPortKind {
    EGGY_PORT_NONE = 0,
    EGGY_PORT_HUMAN = 1,
    EGGY_PORT_CPU = 3,
};

enum EggyMatchKind {
    EGGY_MATCH_STOCK = 0,
    EGGY_MATCH_TIME = 1,
};

enum EggyState {
    EGGY_STATE_PARKED = 0,
    EGGY_STATE_LOADING = 1,
    EGGY_STATE_LOADED = 2,
    EGGY_STATE_FIGHTING = 3,
    EGGY_STATE_ENDED = 4,
    EGGY_STATE_ERROR = 5,
};

enum EggyOutcome {
    EGGY_OUTCOME_NORMAL = 0,
    EGGY_OUTCOME_TIME_OUT = 1,
    EGGY_OUTCOME_SUDDEN_DEATH = 2,
    EGGY_OUTCOME_ABORTED = 3,
};

/* eggy_load's errors (also onState('error', code, serial)): 1..48 name the
 * first invalid word, (index + 1); the rest are whole-config conditions. */
#define EGGY_ERR_FIELD(index) ((index) + 1)
#define EGGY_ERR_BUSY 100     /* not parked: a match is loading or running */
#define EGGY_ERR_FIGHTERS 101 /* fewer than two fighters */
#define EGGY_ERR_TEAMS 102    /* teams on, every fighter on one team */
#define EGGY_ERR_DISABLED 103 /* the engine was not started with MELEE_EGGY */

typedef struct EggyPort {
    int32_t kind; /* enum EggyPortKind */
    int32_t ckind;
    int32_t costume;
    int32_t team;
    int32_t cpu_level;
    int32_t handicap;
} EggyPort;

typedef struct EggyConfig {
    uint32_t seed;
    int32_t stage;
    int32_t match_kind; /* enum EggyMatchKind */
    int32_t stocks;
    int32_t time_minutes;
    int32_t item_freq;
    uint64_t item_mask;
    bool teams;
    bool friendly_fire;
    bool frozen_stadium;
    bool music;
    int32_t music_volume;
    int32_t sfx_volume;
    int32_t results_seconds;
    EggyPort ports[EGGY_PORTS];
} EggyConfig;

/* Validate a v1 config (src/pc/eggy/eggy_config.c; pure, no game state).
 * costumes[ckind] is how many costumes each CharacterKind has. Returns 0 and
 * fills *out, or an EGGY_ERR_* code and leaves *out alone. */
int eggy_config_parse(const int32_t* words, const uint8_t* costumes, EggyConfig* out);
/* A StKind VS matches can be played on. */
bool eggy_stage_valid(int32_t stkind);

/* ---- exports (Module._eggy_*; src/pc/eggy/eggy.c) ---------------------- */

int32_t* eggy_config_buffer(void);
int eggy_load(int serial);
int eggy_go(void);
void eggy_pad(int port, int buttons, int stick_x, int stick_y, int substick_x, int substick_y,
    int trigger_l, int trigger_r);
const int32_t* eggy_hud(void);
const int32_t* eggy_results(void);
int eggy_abort(void);
uint32_t eggy_checksum(void);
void eggy_audio(int music_volume, int sfx_volume);
int eggy_costumes(int ckind);
int eggy_team_costume(int ckind, int team);

/* ---- hooks the engine calls -------------------------------------------- */

/* MELEE_EGGY=1, MELEE_VS_ONLY=1 and MELEE_BOOT_SCENE=eggy, read once. */
bool pc_eggy_enabled(void);
/* platforms/browser/main.c, before aurora_initialize: SDL listens to no key. */
void pc_eggy_preinit(void);
/* src/pc/vi.c: parked, the frame boundary presents the last picture again
 * instead of clearing it. */
bool pc_eggy_hold_frame(void);
/* gmscene.c gm_801A4D34, before the first tick of a scene: a match being
 * loaded holds here, at frame 0, until eggy_go(). */
void pc_eggy_scene_begin(int scene_kind);
/* gmscene.c gm_RunSimTick, after every tick: HUD, abort, results timer. */
void pc_eggy_after_tick(void);
/* lbaudio_ax.c: no music stream may start on this device. */
bool pc_eggy_music_off(void);
/* platforms/browser/pc_stubs.c: the config's frozen Pokemon Stadium. */
bool pc_eggy_frozen_stadium(void);

#ifdef __cplusplus
}
#endif

#endif

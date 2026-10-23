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
 *   [48] netplay 0/1: 0 plays on this phone alone and words 49..63 are not
 *        read; 1 plays two to four phones over the page's link, one per human
 *        port (src/pc/eggy/eggy_net.c, src/pc/net_mesh.c)
 *   [49] this phone's port: one of the human ports
 *   [50] phones in the match: 2..4, the number of human ports
 *   [51] the leader's port: the lowest human port. Its phone releases the
 *        hold, announces the input delay and settles a silent phone away;
 *        if it goes silent itself, the next lowest phone still in does
 *   [52] session id (uint32), distinct per match: datagrams of any other
 *        session are dropped
 *   [53] input delay in frames: 0 auto (from ping), 1..4 fixed
 *   [54..63] reserved, 0
 * Two fighters at least; with teams on, two teams at least. Netplay: two to
 * four humans, one per phone; CPUs are simulated identically on every phone.
 * Every phone's config must agree on every word but 12..14 and 49
 * (eggy_config_hash), or the session is refused.
 *
 * Netplay stats (int32 words at eggy_net_stats(), kept after a session ends):
 *   [0] state: 0 none, 1 waiting for the page's links, 2 handshaking, 3 loading
 *       and holding, 4 playing, 5 closing, 6 closed, 7 failed
 *   [1] ping ms (the worst phone's), [2] input delay, [3] rollbacks,
 *   [4] deepest rollback, [5] desyncs reported (each resync re-arms it),
 *   [6] frames stalled on the other phones, [7] link quality 0 stable ..
 *   2 stalling
 *   [8 + port] that port's phone: 0 not a phone of the match, 1 this phone,
 *       2 not heard from yet, 3 playing, 4 silent (about to be settled away),
 *       5 settled away, 6 another engine build (net-error 7)
 *   [12 + port] ping ms to that port's phone, -1 none
 *   [16] resync: 0 none, 1 the leader waiting for a final frame, 2 the leader
 *        holding for the others, 3 waiting for the leader's snapshot
 *   [17] snapshots taken (the leader) or applied, [18] the last one's frame
 *        (-1 none), [19] reserved, 0
 *
 * HUD (int32 words at eggy_hud(), current at every tick):
 *   [0] state: 0 parked, 1 loading, 2 loaded (held at frame 0), 3 fighting,
 *       4 ended (results screen), 5 error (parked after an invalid config)
 *   [1] match frame: ticks simulated since frame 0, through sudden death
 *   [2] seconds left on the clock, -1 with no clock
 *   [3 + 4 * port]: present, stocks, percent (as the game shows it), the
 *       fighter's CharacterKind now (18/19 follow Zelda and Sheik's changes)
 *
 * Fighter (int32 words at eggy_fighter(port), read on demand; all 0 when the
 * port has no fighter right now). For the page's tests, never the simulation:
 *   [0] present, [1] motion state (ftCommon_MotionState: 14 Wait, 15..17
 *   walks, 20 Dash, 21 Run, 24 KneeBend...), [2] airborne, [3] facing -1/1,
 *   [4] x and [5] y position, [6] ground velocity, [7] stick x and [8] stick y
 *   as the fighter read them (after the dead zone), all x1000, [9] jumps used,
 *   [10..25] every motion state entered on a tick since the last read, as a
 *   512-bit set (bit m of word m / 32), [26] the highest y since then, x1000.
 *   A read starts the next set
 *
 * Picture: eggy_view(width, height) asks for a render of that many pixels,
 * which is its shape too (widescreen follows the window); taken at the next
 * frame boundary. 64..4096 each, else -1. Each phone may pick its own.
 *
 * Frame report (int32 words at eggy_perf(); totals since the last read, and
 * each read starts the next; src/pc/eggy/eggy_perf.c). Times in microseconds:
 *   [0] frames drawn (frame boundaries), [1] ticks simulated fresh, [2] ticks
 *   re-simulated (rollback, sync test)
 *   [3] in fresh ticks, [4] in re-run ticks, [5] drawing (the game's render
 *   pass and aurora turning it into GPU work), [6] submitting (aurora_end_frame:
 *   encode, upload, present), [7] waiting for the GPU (aurora_begin_frame),
 *   [8] spare (the pacing's sleep and the pad wait: ahead of time), [9]
 *   yielded with no time to spare (the browser's own work between frames),
 *   [10] everything else (audio, alarms, netplay, events)
 *   [11] the longest frame, boundary to boundary, [12] frames over 20 ms,
 *   [13] over 34 ms
 *   [14] draws in the last frame, [15] of them merged, [16] pipelines created,
 *   [17] queued, [18] the browser's pipeline compilations still running
 *   [19] the window these cover
 *   [20] cached pipelines still to build behind the frames
 *   (MELEE_PIPELINES_BACKGROUND), [21] vertex, [22] index and [23] uniform
 *   data of the last frame, in KiB
 *
 * Results (int32 words at eggy_results(), filled before onState('ended')):
 *   [0] end frame: the match frame on which GAME! or TIME! came
 *   [1] outcome: 0 normal, 1 time out, 2 sudden death then decided, 3 aborted,
 *       4 disconnected (the netplay peer was lost mid-match)
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
#define EGGY_CONFIG_WORDS 64
#define EGGY_CONFIG_PORT_BASE 16
#define EGGY_CONFIG_PORT_WORDS 8
#define EGGY_PORTS 4
#define EGGY_HUD_WORDS (3 + 4 * EGGY_PORTS)
#define EGGY_HUD_PORT_BASE 3
#define EGGY_RESULTS_WORDS (2 + 8 * EGGY_PORTS)
#define EGGY_RESULTS_PORT_BASE 2
#define EGGY_NET_STATS_WORDS 20
#define EGGY_FIGHTER_SEEN_AT 10
#define EGGY_FIGHTER_SEEN_WORDS 16
#define EGGY_FIGHTER_WORDS (EGGY_FIGHTER_SEEN_AT + EGGY_FIGHTER_SEEN_WORDS + 1)
#define EGGY_NET_PHONES_MIN 2
#define EGGY_NET_PHONES_MAX 4
#define EGGY_PERF_WORDS 24

/* Where the clock goes (pc_eggy_perf_enter). */
enum EggyPerfSection {
    EGGY_PERF_OTHER = 0,
    EGGY_PERF_TICK = 1,
    EGGY_PERF_RERUN = 2,
    EGGY_PERF_DRAW = 3,
    EGGY_PERF_SUBMIT = 4,
    EGGY_PERF_GPU_WAIT = 5,
    EGGY_PERF_SPARE = 6,
    EGGY_PERF_YIELD = 7,
    EGGY_PERF_SECTIONS = 8,
};

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
    EGGY_OUTCOME_DISCONNECTED = 4,
};

/* eggy_net_stats()[0]. */
enum EggyNetState {
    EGGY_NET_NONE = 0,
    EGGY_NET_LINK = 1,
    EGGY_NET_HANDSHAKE = 2,
    EGGY_NET_HOLD = 3,
    EGGY_NET_LIVE = 4,
    EGGY_NET_CLOSING = 5,
    EGGY_NET_CLOSED = 6,
    EGGY_NET_FAILED = 7,
};

/* onState('net-error', code, serial): the session could not be set up or was
 * lost before the match's first frame. The engine parks ('parked' follows). */
enum EggyNetError {
    EGGY_NET_ERR_LINK = 1,         /* the page's link did not open (status) */
    EGGY_NET_ERR_CONNECT = 2,      /* the session could not be opened */
    EGGY_NET_ERR_HANDSHAKE = 3,    /* RULES/READY failed or timed out */
    EGGY_NET_ERR_MISMATCH = 4,     /* another phone's config is another match */
    EGGY_NET_ERR_PEER_LEFT = 5,    /* every other phone left, or this one was settled away */
    EGGY_NET_ERR_TIMEOUT = 6,      /* a phone never answered or never finished loading */
    EGGY_NET_ERR_INCOMPATIBLE = 7, /* another wire protocol (engine build) */
};

/* eggy_load's errors (also onState('error', code, serial)): 1..64 name the
 * first invalid word, (index + 1); the rest are whole-config conditions. */
#define EGGY_ERR_FIELD(index) ((index) + 1)
#define EGGY_ERR_BUSY 100       /* not parked: a match is loading or running */
#define EGGY_ERR_FIGHTERS 101   /* fewer than two fighters */
#define EGGY_ERR_TEAMS 102      /* teams on, every fighter on one team */
#define EGGY_ERR_DISABLED 103   /* the engine was not started with MELEE_EGGY */
#define EGGY_ERR_NET_HUMANS 104 /* netplay humans not 2..4 */

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
    /* netplay, words 48..53 */
    bool netplay;
    int32_t local_port;
    int32_t leader_port; /* the lowest human port */
    uint8_t humans;      /* the human ports, one phone each */
    int32_t phones;      /* how many */
    uint32_t session;
    int32_t input_delay; /* 0 auto */
    uint32_t hash;       /* eggy_config_hash() of the words */
} EggyConfig;

/* Validate a v1 config (src/pc/eggy/eggy_config.c; pure, no game state).
 * costumes[ckind] is how many costumes each CharacterKind has. Returns 0 and
 * fills *out, or an EGGY_ERR_* code and leaves *out alone. */
int eggy_config_parse(const int32_t* words, const uint8_t* costumes, EggyConfig* out);
/* A StKind VS matches can be played on. */
bool eggy_stage_valid(int32_t stkind);
/* FNV-1a over every word both phones of a netplay match must agree on: all
 * 64 but 12..14 (this device's sound) and 49 (this phone's port). */
uint32_t eggy_config_hash(const int32_t* words);

/* ---- exports (Module._eggy_*; src/pc/eggy/eggy.c) ---------------------- */

int32_t* eggy_config_buffer(void);
int eggy_load(int serial);
int eggy_go(void);
void eggy_pad(int port, int buttons, int stick_x, int stick_y, int substick_x, int substick_y,
    int trigger_l, int trigger_r);
/* Buttons pressed for exactly one simulated tick, on top of eggy_pad's. */
void eggy_tap(int port, int buttons);
const int32_t* eggy_hud(void);
const int32_t* eggy_results(void);
int eggy_abort(void);
uint32_t eggy_checksum(void);
void eggy_audio(int music_volume, int sfx_volume);
int eggy_costumes(int ckind);
int eggy_team_costume(int ckind, int team);
const int32_t* eggy_net_stats(void);
int eggy_snapshot_begin(void);
int eggy_snapshot_size(void);
const uint8_t* eggy_snapshot_buffer(void);
int32_t eggy_snapshot_frame(void);
uint8_t* eggy_snapshot_reserve(int len);
int eggy_snapshot_apply(const uint8_t* ptr, int len, int32_t frame);
int eggy_test_desync(void);
int eggy_view(int width, int height);
const int32_t* eggy_fighter(int port);
const int32_t* eggy_perf(void);

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
/* src/pc/net.c capture_local_sample: the PADRead between true and false is
 * the session's local sample, which takes eggy_tap's presses. */
void pc_eggy_sampling(bool on);
/* src/pc/vi.c, after a frame is presented: a picture size the page asked for. */
void pc_eggy_frame_end(void);
/* platforms/browser/pc_stubs.c: the config's frozen Pokemon Stadium. */
bool pc_eggy_frozen_stadium(void);
/* The frame report (eggy_perf.c): the clock goes to `section` from now, and
 * the section it left is returned, to hand the clock back to. */
int pc_eggy_perf_enter(int section);
/* gmscene.c, per simulated tick; vi.c, per frame boundary. */
void pc_eggy_perf_tick(bool rerun);
void pc_eggy_perf_boundary(void);

#ifdef __cplusplus
}
#endif

#endif

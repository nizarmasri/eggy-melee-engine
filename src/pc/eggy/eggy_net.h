/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * MELEE_EGGY netplay, internal to src/pc/eggy (EGGY.md "Netplay").
 *
 * One session per match, on melee-web's own rollback netcode (src/pc/net*.c)
 * over a mesh of the match's two to four phones (src/pc/net_mesh.c, over the
 * page's link): the park opens it and every phone checks every other's
 * config (HELLO) before anything is simulated, the VS scene loads, every
 * phone holds before its first tick until all are loaded and every page said
 * go, and the match runs under rollback from session frame 0. A phone that
 * goes silent from the hold on is settled away by the others, who play on.
 * After the match's last scene every phone says DONE and closes.
 *
 * eggy_hold.c is the pure part (the hold's rules and the HELLO payload),
 * built with the host compiler by tests/eggy/eggy_net_test.c; eggy_net.c
 * drives the session on the game thread.
 */
#ifndef PC_EGGY_EGGY_NET_H
#define PC_EGGY_EGGY_NET_H

#include "pc/eggy/eggy.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reliable messages on the mesh's caller lane (>= 0x10), each sent to every
 * phone still in. */
#define EGGY_REL_HELLO 0x11
#define EGGY_REL_LOADED 0x30 /* this phone has the VS scene loaded */
#define EGGY_REL_ARMED 0x31  /* this phone's page said go */
#define EGGY_REL_GO 0x32     /* the acting leader: release the first tick */
#define EGGY_REL_DONE 0x33   /* this phone has left the match's last scene */

/* HELLO payload: "EGG", version 1, then the config hash and the session id,
 * both big-endian. */
#define EGGY_HELLO_LEN 12
void eggy_hello_encode(uint8_t out[EGGY_HELLO_LEN], uint32_t config_hash, uint32_t session);
bool eggy_hello_matches(const uint8_t* in, int len, uint32_t config_hash, uint32_t session);

/* The ids net.c is given: never 0, which means "none" there. */
uint32_t eggy_net_session_id(const EggyConfig* c);
uint32_t eggy_net_seed(const EggyConfig* c);

/* The hold before the first tick. Every phone sends LOADED once its scene
 * is loaded, and reports 'loaded' once it holds every other phone's; then
 * ARMED once its page has said go. The acting leader sends GO once it has
 * sent its own ARMED and holds every other phone's, and releases; the rest
 * release when a GO arrives. A phone settled away is not waited for, and
 * since every phone hears every ARMED, a leader that goes silent is replaced
 * by the next one (the mesh's rule) with nothing to send again. */
typedef struct EggyHold {
    int self;       /* this phone's port */
    uint8_t humans; /* every phone's port */
    uint8_t loaded; /* ports whose LOADED arrived */
    uint8_t armed;  /* ports whose ARMED arrived */
    bool sent_loaded, reported, sent_armed, sent_go, got_go;
} EggyHold;

enum EggyHoldAction {
    EGGY_HOLD_WAIT,
    EGGY_HOLD_SEND_LOADED,
    EGGY_HOLD_REPORT, /* onState('loaded') */
    EGGY_HOLD_SEND_ARMED,
    EGGY_HOLD_SEND_GO,
    EGGY_HOLD_RELEASE,
};

void eggy_hold_init(EggyHold* h, uint8_t humans, int self);
/* The next thing to do, given whether this phone's page has said go, the
 * ports settled away and the acting leader's port. The caller does it and
 * calls eggy_hold_done(); a send the lane refused is simply not marked done,
 * and comes back next frame. */
int eggy_hold_next(const EggyHold* h, bool go, uint8_t away, int leader);
void eggy_hold_done(EggyHold* h, int action);
/* A LOADED, ARMED or GO from the phone on port `from`; anything else, or
 * from no other phone, is ignored. */
void eggy_hold_receive(EggyHold* h, int type, int from);

/* ---- the session (eggy_net.c, game thread) ---------------------------- */

/* Start connecting for this match. */
void eggy_net_begin(const EggyConfig* c);
/* Once per frame while parked: 0 still connecting, 1 up (handshake and
 * barrier done), < 0 a -EggyNetError (the session is closed). */
int eggy_net_connect_step(void);
/* Once per frame while the loaded VS scene holds: 0 holding, 1 release the
 * first tick, < 0 a -EggyNetError (closed). *loaded is set on the frame both
 * phones are loaded. */
int eggy_net_hold_step(bool go, bool* loaded);
/* Once per tick of the match, never during a re-run: 1 a desync was seen
 * (*frame is its frame), -1 the match cannot go on here (the session is gone,
 * this phone was settled away, or no other phone is left), 0 nothing new. */
int eggy_net_tick(int32_t* frame);
/* The next port settled away not reported yet (-1 none), and *frame the last
 * frame of its input the match used. This phone's own port when the others
 * settled it. */
int eggy_net_next_away(int32_t* frame);
/* The others settled this phone away. */
bool eggy_net_evicted(void);
/* After the match's last scene: say DONE and wait for the other phone's, up
 * to a few seconds. True once closed. */
bool eggy_net_close_step(void);
/* Close now (page abort or failure). */
void eggy_net_drop(bool failed);
/* A session is open. */
bool eggy_net_open(void);
const int32_t* eggy_net_stats_words(void);

#ifdef __cplusplus
}
#endif

#endif

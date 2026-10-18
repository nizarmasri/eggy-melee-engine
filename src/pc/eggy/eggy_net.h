/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * MELEE_EGGY netplay, internal to src/pc/eggy (EGGY.md "Netplay").
 *
 * One session per match, on melee-web's own netcode (src/pc/net*.c, the
 * session net_match_web.c opens over the page's link): the park opens it and
 * runs the RULES/READY handshake and the ready barrier before anything is
 * simulated, the VS scene loads, both phones hold before its first tick until
 * both are loaded and both pages said go, and the match runs under rollback
 * from session frame 0. After the match's last scene both phones say DONE
 * and close.
 *
 * eggy_hold.c is the pure part (the hold's rules and the barrier payload),
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

/* Reliable messages on the session's caller lane (net_reliable.c, >= 0x10).
 * HELLO is the lobby's READY_BARRIER type (net_lan.h), carrying the match. */
#define EGGY_REL_HELLO 0x11
#define EGGY_REL_LOADED 0x30 /* this phone has the VS scene loaded */
#define EGGY_REL_ARMED 0x31  /* guest -> leader: the guest's page said go */
#define EGGY_REL_GO 0x32     /* leader -> guest: release the first tick */
#define EGGY_REL_DONE 0x33   /* this phone has left the match's last scene */

/* HELLO payload: "EGG", version 1, then the config hash and the session id,
 * both big-endian. */
#define EGGY_HELLO_LEN 12
void eggy_hello_encode(uint8_t out[EGGY_HELLO_LEN], uint32_t config_hash, uint32_t session);
bool eggy_hello_matches(const uint8_t* in, int len, uint32_t config_hash, uint32_t session);

/* The ids net.c is given: never 0, which means "none" there. */
uint32_t eggy_net_session_id(const EggyConfig* c);
uint32_t eggy_net_seed(const EggyConfig* c);

/* The hold before the first tick. Both phones send LOADED once their scene
 * is loaded and report 'loaded' once they hold the other's too. The guest's
 * go only arms the start (ARMED); the leader sends GO once it has its own go
 * and the guest's, and releases; the guest releases when GO arrives. */
typedef struct EggyHold {
    bool leader;
    bool sent_loaded, peer_loaded, reported;
    bool sent_armed, peer_armed; /* guest sends, leader receives */
    bool sent_go, peer_go;       /* leader sends, guest receives */
} EggyHold;

enum EggyHoldAction {
    EGGY_HOLD_WAIT,
    EGGY_HOLD_SEND_LOADED,
    EGGY_HOLD_REPORT, /* onState('loaded') */
    EGGY_HOLD_SEND_ARMED,
    EGGY_HOLD_SEND_GO,
    EGGY_HOLD_RELEASE,
};

void eggy_hold_init(EggyHold* h, bool leader);
/* The next thing to do, given whether this phone's page has said go. The
 * caller does it and calls eggy_hold_done(); a send the lane refused is
 * simply not marked done, and comes back next frame. */
int eggy_hold_next(const EggyHold* h, bool go);
void eggy_hold_done(EggyHold* h, int action);
/* A LOADED, ARMED or GO from the other phone; anything else, or one meant
 * for the other role, is ignored. */
void eggy_hold_receive(EggyHold* h, int type);

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
 * (*frame is its frame), -1 the session is gone, 0 nothing new. */
int eggy_net_tick(int32_t* frame);
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

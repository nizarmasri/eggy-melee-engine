/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What a fight over a link plays with: its input delay and how far it may
 * predict past the others before it stalls (net_pick.c). Pure: no game or
 * session state, so tests/eggy/eggy_pick_test.c builds it with the host
 * compiler. */
#ifndef PC_NET_PICK_H
#define PC_NET_PICK_H

#include <stdbool.h>
#include <stdint.h>

/* Predicted frames before a hard stall: the fewest any fight gets, and the
 * most. Each is one rollback snapshot (~6 MB) more in the ring. */
#define NET_WINDOW_MIN 7
#define NET_WINDOW_MAX 12
/* A fight's input delay: the most it is ever given. */
#define NET_FIGHT_DELAY_MAX 8

/* Frames of the one-way trip a peer's input has to cover: half the round
 * trip plus the jitter on it, rounded up. 0 while the link is unmeasured. */
int net_pick_need(uint32_t ping_us, uint32_t jitter_us, int32_t frame_us);

/* The fight's input delay for that trip. `lo` is the floor (1 on a LAN, 2
 * otherwise). Up to six frames of trip, all but two of it (the rollback
 * covers those two): the tuning every short link has played with. Past that
 * the trip is split between the hands and the rollback, half each, so a far
 * link neither waits on every frame nor rolls back further than it delays. */
int net_pick_fight_delay(int need, int lo);

/* How far a fight may predict for that trip: the part the delay leaves to the
 * rollback, with four frames to spare for jitter, never fewer than the window
 * every short link has, nor more than the ring holds. */
int net_pick_window(int need, int delay);

#endif

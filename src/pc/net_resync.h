/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Desync resync, the pure part (src/pc/net.c "resync" drives it): the wire
 * image a snapshot travels in between phones, the rule for the frame it is
 * taken at, where a receiver applies it, and the leader's budget. No game or
 * session state, so tests/eggy/eggy_resync_test.c builds it with the host
 * compiler.
 *
 * When the acting leader of a mesh sees a desync it serialises its own state
 * at a frame S that is final there, and holds before S while the page carries
 * the bytes to the other phones; each restores it and re-runs whatever frames
 * past S it had simulated from the inputs it holds. Every phone is then on the
 * leader's timeline from S on.
 *
 * The image, big-endian:
 *    0  'E' 'G' 'S' 'N'
 *    4  version (1)
 *    8  frame S: the state is the one entering S
 *   12  session id
 *   16  scene kind it was taken in
 *   20  address of the RNG seed (u64), 28 its value
 *   32  region count n (<= RESYNC_REGIONS_MAX)
 *   36  n x {address (u64), length (u32)}
 *       the regions' bytes, in order
 *       FNV-1a 64 of everything before it (u64)
 * Addresses are this build's linear-memory addresses: the same on every phone
 * running the same engine, which the receiver checks region by region.
 */
#ifndef PC_NET_RESYNC_H
#define PC_NET_RESYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RESYNC_MAGIC 0x4547534Eu /* 'EGSN' */
#define RESYNC_VERSION 1
#define RESYNC_REGIONS_MAX 16
#define RESYNC_HEADER_FIXED 36
#define RESYNC_REGION_BYTES 12
#define RESYNC_TRAILER 8
#define RESYNC_BYTES_MAX (64u << 20) /* far past any state; a bound on garbage */

typedef struct ResyncRegion {
    uint64_t addr;
    uint32_t len;
} ResyncRegion;

typedef struct ResyncHeader {
    int32_t frame;
    uint32_t session;
    int32_t scene;
    uint64_t seed_addr;
    uint32_t seed_val;
    uint32_t nregions;
    ResyncRegion region[RESYNC_REGIONS_MAX];
} ResyncHeader;

uint64_t resync_checksum(const uint8_t* p, size_t n);

/* Bytes the image of h takes, 0 when h is not one (too many regions). */
size_t resync_image_size(const ResyncHeader* h);
/* Write the image: the header, then `body`, which holds the regions' bytes
 * back to back (their lengths summed), then the checksum. 0 when it does not
 * fit `cap`. */
size_t resync_encode(const ResyncHeader* h, const uint8_t* body, uint8_t* out, size_t cap);
/* Read one: false unless it is exactly an image with a good checksum.
 * *body points at the regions' bytes inside `in`. */
bool resync_decode(const uint8_t* in, size_t len, ResyncHeader* h, const uint8_t** body);

/* Why this phone cannot take the leader's state, NULL when it can: the same
 * number of regions, the first `fixed` (statics, heap descriptors) at the same
 * address with the same length, and every heap starting where ours does. A
 * heap's length may differ: its live extent moves with allocation, and the
 * restore brings the leader's free lists with it. */
const char* resync_layout_check(
    const ResyncHeader* theirs, const ResyncRegion* mine, int nmine, int fixed);

/* The leader's state entering `frame` is final: every input before it is
 * real (remote_have, the newest frame every remote input is real up to) and
 * no misprediction is waiting to be rolled back (rb_frame < 0). */
bool resync_frame_final(int32_t frame, int32_t remote_have, int32_t rb_frame);

/* Where a receiver whose next fresh frame is `now` applies a snapshot of S:
 * now, at the point the leader took it (now == S); by rolling back to S and
 * re-running the frames since (S < now, while its rings still hold them);
 * once it gets there (now < S); or never (it is too far past). */
enum { RESYNC_AT_FRESH, RESYNC_ROLLBACK, RESYNC_LATER, RESYNC_TOO_LATE };
#define RESYNC_REPLAY_MAX 30 /* frames re-run at most: inside the input rings */
int resync_apply_when(int32_t s, int32_t now);

/* The leader's side of one match. */
enum { RESYNC_IDLE, RESYNC_WANT, RESYNC_HOLD };
#define RESYNC_SENDS_MAX 3   /* a snapshot, then at most two resends */
#define RESYNC_HOLD_MS 30000 /* how long the leader waits for the others */
typedef struct ResyncPolicy {
    int state;
    int sent;        /* snapshots taken this match */
    int32_t frame;   /* S of the one out */
    uint8_t waiting; /* ports that have not answered it */
    uint8_t applied; /* ports that applied it */
    int32_t asked;   /* frame of the desync that asked, -1 none */
} ResyncPolicy;

void resync_policy_init(ResyncPolicy* p);
/* A desync at frame f (or the page asking): true when a snapshot will be
 * taken, false when one is already coming or the budget is spent. */
bool resync_policy_want(ResyncPolicy* p, int32_t f);
/* The snapshot of S is out to `receivers`. */
void resync_policy_taken(ResyncPolicy* p, int32_t s, uint8_t receivers);
/* The phone on `port` answered for frame s (ok: applied it). */
void resync_policy_answer(ResyncPolicy* p, int port, int32_t s, bool ok);
/* Whether the hold ends: every receiver still in has answered, or the hold
 * has lasted RESYNC_HOLD_MS. `live` is the ports still in. Ends the hold. */
bool resync_policy_release(ResyncPolicy* p, uint8_t live, int32_t held_ms);

/* The RESYNC announcement (leader to every phone) and the answer. */
#define RESYNC_MSG_LEN 9
void resync_msg_encode(uint8_t out[RESYNC_MSG_LEN], int32_t frame, uint32_t size, uint8_t nth);
bool resync_msg_decode(const uint8_t* in, int len, int32_t* frame, uint32_t* size, uint8_t* nth);
#define RESYNC_ANSWER_LEN 5
void resync_answer_encode(uint8_t out[RESYNC_ANSWER_LEN], int32_t frame, uint8_t result);
bool resync_answer_decode(const uint8_t* in, int len, int32_t* frame, uint8_t* result);

#ifdef __cplusplus
}
#endif

#endif

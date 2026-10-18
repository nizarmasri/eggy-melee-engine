/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The mesh session's wire format and rules (src/pc/net_mesh.c), pure C with
 * no session or game state, so tests/eggy/eggy_mesh_test.c builds it with the
 * host compiler.
 *
 * A mesh is two to four instances, one per human port, all talking to all
 * (Slippi's doubles model): each sends its own port's inputs to every other,
 * and relays what it holds of a third port to a phone that lacks it. The page
 * carries the datagrams (src/pc/net_web.c); every one starts with a byte
 * naming the other end -- the destination port going out, the source port
 * coming in -- and the 512-byte slot includes that byte.
 *
 * Multi-byte fields are big-endian, like the rest of the netcode's wire.
 */
#ifndef PC_NET_MESH_WIRE_H
#define PC_NET_MESH_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One past melee-web's PC_NET_PROTO_VERSION (9): another engine build is
 * refused rather than misread. */
#define MESH_VERSION 10
#define MESH_PORTS 4
#define MESH_SLOT_BYTES 512                  /* net_web.c WEB_SLOT_BYTES */
#define MESH_DGRAM_MAX (MESH_SLOT_BYTES - 1) /* after the port byte */
#define MESH_RED 32                          /* frames one section carries at most */
#define MESH_SECTIONS 4                      /* own port plus relays */
#define MESH_REL_MAX 256                     /* reliable payload */

enum MeshMagic {
    MESH_INPUT = 'I',
    MESH_REL = 'R',
    MESH_ACK = 'K',
    MESH_BYE = 'B',
};

/* The same 8 bytes net.c's WirePad is. */
typedef struct MeshPad {
    uint16_t button;
    int8_t stickX, stickY, substickX, substickY;
    uint8_t triggerLeft, triggerRight;
} MeshPad;

typedef struct MeshHdr {
    uint8_t magic;
    uint8_t version; /* MESH_VERSION */
    uint32_t session;
    uint8_t from, to; /* engine ports */
} MeshHdr;
#define MESH_HDR_LEN 8

/* Frames first..first+count-1 of one port's input. */
typedef struct MeshSection {
    uint8_t port;
    int32_t first;
    uint8_t count;
    MeshPad pads[MESH_RED];
} MeshSection;

/* The input packet each instance sends every frame to every other. */
typedef struct MeshInput {
    MeshHdr h;
    uint16_t seq;             /* per destination */
    uint16_t echo;            /* the newest seq received from the destination */
    uint16_t held;            /* how long that one waited here, 0.1 ms units */
    uint8_t flags;            /* MESH_ECHO when echo is valid */
    int8_t adv;               /* frame advantage over the destination (net.c send_inputs) */
    int32_t have[MESH_PORTS]; /* newest contiguous frame held of each port, -1 none */
    int32_t ck_frame;         /* the sender's newest confirmed frame, -1 none */
    uint32_t ck;              /* its checksum */
    uint8_t nsec;
    MeshSection sec[MESH_SECTIONS];
} MeshInput;
#define MESH_ECHO 0x01
#define MESH_INPUT_FIXED 41
#define MESH_SECTION_FIXED 6

typedef struct MeshRel {
    MeshHdr h;
    uint16_t seq; /* per destination */
    uint8_t type;
    uint16_t len;
    uint8_t payload[MESH_REL_MAX];
} MeshRel;
#define MESH_REL_FIXED 13

/* Bytes an input packet with these sections takes. */
size_t mesh_input_len(const MeshInput* pk);
/* Encode into out (cap bytes); 0 when it does not fit. */
size_t mesh_input_encode(const MeshInput* pk, uint8_t* out, size_t cap);
bool mesh_input_decode(const uint8_t* in, size_t len, MeshInput* out);
size_t mesh_rel_encode(const MeshRel* r, uint8_t* out, size_t cap);
bool mesh_rel_decode(const uint8_t* in, size_t len, MeshRel* out);
/* 'K' acks every reliable seq below next; 'B' carries a reason. */
size_t mesh_small_encode(const MeshHdr* h, uint16_t value, uint8_t* out, size_t cap);
bool mesh_small_decode(const uint8_t* in, size_t len, MeshHdr* h, uint16_t* value);
/* Just the header, to route a datagram before its body is read. */
bool mesh_hdr_decode(const uint8_t* in, size_t len, MeshHdr* h);

/* The ring's leading port byte. wrap: out = port, dgram; 0 when it does not
 * fit a slot. unwrap: the port and the datagram behind it. */
size_t mesh_addr_wrap(uint8_t port, const uint8_t* dgram, size_t len, uint8_t* out, size_t cap);
bool mesh_addr_unwrap(
    const uint8_t* in, size_t len, uint8_t* port, const uint8_t** dgram, size_t* dgram_len);

/* ---- rules ------------------------------------------------------------ */

/* Whether a phone relays a third port's input to another: it holds more of
 * it than the other says it holds, and the other is MESH_RELAY_LAG frames
 * behind (so not getting it from that port itself), or that port has gone
 * quiet MESH_RELAY_QUIET_MS (its last frames must spread before it is
 * settled), or it is settled (every phone must reach its F). */
#define MESH_RELAY_LAG 4
#define MESH_RELAY_QUIET_MS 500
bool mesh_relay_wanted(int32_t mine, int32_t theirs, bool settled, int32_t quiet_ms);

/* What an AWAY(port, F) from the phone on `from` means here. A phone already
 * settled here has no say any more; one about this phone means it has been
 * let go. */
enum { MESH_AWAY_IGNORE, MESH_AWAY_SETTLE, MESH_AWAY_EVICTED };
int mesh_away_accept(uint8_t settled, int self, int from, int port);

/* The phone that leads: the lowest human port not settled away and not
 * silent to this phone. A phone never counts itself silent, so with every
 * other phone gone it leads itself. -1 only for an empty set. */
int mesh_leader(uint8_t humans, uint8_t settled, uint8_t silent, int self);

/* Whether this phone may act on its own view of who is silent: it hears a
 * strict majority of the phones still in, itself included. A phone that
 * stops receiving (a stall, a busy link) sees every other phone go silent at
 * once, and must not settle the ones that still hear each other. U is the
 * phones not settled and not gone (a BYE, or a link the page reported failed:
 * their own word, which no partition fakes), this one included; L is this one
 * and those of U it hears (not in `unheard`, mesh_unheard_mask). A quorum is
 * 2L > U, or U <= 2: between two phones neither side can be told from the
 * other, and each settles the other as before. A 2-2 split settles nobody. */
bool mesh_quorum(uint8_t humans, uint8_t settled, uint8_t unheard, uint8_t gone, int self);
/* mesh_leader as it may act: without a quorum this phone's silence view does
 * not count, and the lead is the lowest port still in, as if all were heard. */
int mesh_acting_leader(
    uint8_t humans, uint8_t settled, uint8_t silent, uint8_t unheard, uint8_t gone, int self);

/* The leader's settlement of a silent port: F is the newest frame of its
 * input any live phone holds. reports[q] is what phone q last said it holds
 * of that port (live_mask names the phones whose reports count: live, not
 * the silent port, not this phone), mine what this phone holds. Ready once
 * this phone holds all of it -- the others relay what it lacks -- and then
 * *frame is F. */
bool mesh_settle_ready(int32_t mine, const int32_t* reports, uint8_t live_mask, int32_t* frame);

/* What a silent phone gets before it is settled: away_ms in play, load_ms
 * while it has not reached the scene's first frames (it is loading it, and a
 * loading phone sends nothing). */
int mesh_silence_limit_ms(int32_t its_newest, int32_t scene_first, int away_ms, int load_ms);

/* AWAY payload: the port and F. */
#define MESH_AWAY_LEN 5
void mesh_away_encode(uint8_t out[MESH_AWAY_LEN], uint8_t port, int32_t frame);
bool mesh_away_decode(const uint8_t* in, int len, uint8_t* port, int32_t* frame);

/* What one phone knows when it decides whether to settle a silent port. */
typedef struct MeshAwayView {
    uint8_t humans;  /* human ports, one phone each */
    uint8_t settled; /* ports already settled away */
    int self;        /* this phone's port */
    bool bye[MESH_PORTS];
    /* Since this phone last heard from each port, or of it: a datagram from
     * it, or the newest frame of its input anyone is known to hold moving
     * (relayed to this phone, or in another phone's report). */
    int32_t quiet_ms[MESH_PORTS];
    /* Since that newest frame last moved. */
    int32_t stable_ms[MESH_PORTS];
    /* Still loading its scene: has not reached the scene's first frames. */
    bool loading[MESH_PORTS];
    int32_t have[MESH_PORTS];                 /* this phone's contiguous have of each */
    int32_t reports[MESH_PORTS][MESH_PORTS];  /* [q][p]: what phone q says it holds of p */
    int32_t away_ms, load_ms, stable_need_ms; /* the limits */
} MeshAwayView;

/* The ports this phone counts silent: gone (BYE), or quiet past the limit
 * that applies (load_ms while loading, away_ms otherwise). Never itself, never
 * one already settled. */
uint8_t mesh_silent_mask(const MeshAwayView* v);
/* The ports a quorum does not count as heard: the silent ones, and any quiet
 * for half the limit that applies. Phones cut off together cross the limit a
 * few milliseconds apart; counting the first as gone while the rest still
 * count as heard would let a minority settle them one by one. */
uint8_t mesh_unheard_mask(const MeshAwayView* v);

/* The port this phone settles now, -1 for none, and *frame its F. Only with
 * a quorum (mesh_quorum), only the acting leader settles, only a silent
 * port, only once the newest frame of it has stopped moving for
 * stable_need_ms and this phone holds it (mesh_settle_ready). One per call:
 * the caller applies it and asks again. */
int mesh_away_decide(const MeshAwayView* v, int32_t* frame);

/* A datagram an M2 engine (melee-web's two-peer wire, protocol 9) reads as a
 * BYE of another protocol version and refuses (net.c rx_datagram: shape,
 * player, session, then version): what makes an M2 phone fail with error 7
 * instead of waiting on a mesh it cannot read. `player` is the M2 side's
 * remote player, 0 or 1; send both. A mesh drops it (a 'B' is 10 bytes). */
#define MESH_M2_PROBE_LEN 16
void mesh_m2_probe(uint8_t out[MESH_M2_PROBE_LEN], uint32_t session, uint8_t player);

#ifdef __cplusplus
}
#endif

#endif

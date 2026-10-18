/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The mesh session's pure part (src/pc/net_mesh_wire.c): the wire codec, the
 * ring's port byte, and the rules a phone settles a silent one away by, run
 * over a simulated mesh of three and four phones. Built with the host
 * compiler by .github/workflows/ci.yml:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -Isrc tests/eggy/eggy_mesh_test.c \
 *       src/pc/net_mesh_wire.c -o eggy_mesh_test && ./eggy_mesh_test
 */
#include "pc/net_mesh_wire.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);               \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ---- the wire ------------------------------------------------------------ */

static MeshPad pad_of(int port, int32_t f) {
    MeshPad p;
    p.button = (uint16_t)(f * 7 + port);
    p.stickX = (int8_t)(f - 100);
    p.stickY = (int8_t)(port * 30 - 60);
    p.substickX = (int8_t)-f;
    p.substickY = 5;
    p.triggerLeft = (uint8_t)f;
    p.triggerRight = (uint8_t)(255 - port);
    return p;
}

static void section(MeshInput* pk, int port, int32_t first, int count) {
    MeshSection* s = &pk->sec[pk->nsec++];
    s->port = (uint8_t)port;
    s->first = first;
    s->count = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        s->pads[i] = pad_of(port, first + i);
    }
}

static void test_input_codec(void) {
    MeshInput pk;
    memset(&pk, 0, sizeof pk);
    pk.h = (MeshHdr){MESH_INPUT, MESH_VERSION, 0xC0FFEE01u, 1, 3};
    pk.seq = 65535;
    pk.echo = 12;
    pk.held = 40;
    pk.flags = MESH_ECHO;
    pk.adv = -3;
    pk.have[0] = 1000;
    pk.have[1] = 1002;
    pk.have[2] = -1;
    pk.have[3] = 998;
    pk.ck_frame = 990;
    pk.ck = 0xDEADBEEFu;
    section(&pk, 1, 971, MESH_RED); /* our own, a full window */
    section(&pk, 0, 990, 11);       /* a relay */
    section(&pk, 2, 5, 0);          /* nothing of it, just the port */

    uint8_t out[MESH_SLOT_BYTES];
    const size_t n = mesh_input_encode(&pk, out, sizeof out);
    CHECK(n == mesh_input_len(&pk));
    CHECK(n == MESH_INPUT_FIXED + 3 * MESH_SECTION_FIXED + (MESH_RED + 11) * 8);
    CHECK(n > 0 && n <= MESH_DGRAM_MAX);
    CHECK(out[0] == 'I' && out[1] == MESH_VERSION && out[6] == 1 && out[7] == 3);
    CHECK(out[2] == 0xC0 && out[3] == 0xFF && out[4] == 0xEE && out[5] == 0x01); /* big-endian */

    MeshInput back;
    CHECK(mesh_input_decode(out, n, &back));
    CHECK(
        back.h.magic == 'I' && back.h.session == 0xC0FFEE01u && back.h.from == 1 && back.h.to == 3);
    CHECK(back.seq == 65535 && back.echo == 12 && back.held == 40 && back.flags == MESH_ECHO);
    CHECK(back.adv == -3 && back.ck_frame == 990 && back.ck == 0xDEADBEEFu);
    CHECK(
        back.have[0] == 1000 && back.have[1] == 1002 && back.have[2] == -1 && back.have[3] == 998);
    CHECK(back.nsec == 3);
    for (int i = 0; i < 3; i++) {
        CHECK(back.sec[i].port == pk.sec[i].port && back.sec[i].first == pk.sec[i].first &&
              back.sec[i].count == pk.sec[i].count);
        CHECK(memcmp(back.sec[i].pads, pk.sec[i].pads, pk.sec[i].count * sizeof(MeshPad)) == 0);
    }

    /* One byte short, or one over, is not a packet. */
    CHECK(!mesh_input_decode(out, n - 1, &back));
    CHECK(!mesh_input_decode(out, n + 1, &back));
    /* Nor is another magic, a port past 3 or a section past its window. */
    out[0] = 'R';
    CHECK(!mesh_input_decode(out, n, &back));
    out[0] = 'I';
    out[7] = 4;
    CHECK(!mesh_input_decode(out, n, &back));
    out[7] = 3;
    out[MESH_INPUT_FIXED + 5] = MESH_RED + 1;
    CHECK(!mesh_input_decode(out, n, &back));

    /* A fourth full section does not fit the slot: refused, never cut. */
    MeshInput big = pk;
    big.nsec = 2;
    big.sec[1].count = MESH_RED;
    section(&big, 3, 900, MESH_RED);
    CHECK(mesh_input_len(&big) > MESH_DGRAM_MAX);
    CHECK(mesh_input_encode(&big, out, sizeof out) == 0);
    /* Nor does a section wider than the window, or a bad port. */
    MeshInput wide = pk;
    wide.sec[2].count = MESH_RED + 1;
    CHECK(mesh_input_encode(&wide, out, sizeof out) == 0);
    MeshInput port = pk;
    port.sec[1].port = 4;
    CHECK(mesh_input_encode(&port, out, sizeof out) == 0);
}

static void test_rel_codec(void) {
    MeshRel r;
    memset(&r, 0, sizeof r);
    r.h = (MeshHdr){MESH_REL, MESH_VERSION, 7, 0, 2};
    r.seq = 513;
    r.type = 0x30;
    r.len = 5;
    memcpy(r.payload, "\x01\x02\x03\x04\x05", 5);
    uint8_t out[MESH_SLOT_BYTES];
    const size_t n = mesh_rel_encode(&r, out, sizeof out);
    CHECK(n == MESH_REL_FIXED + 5);
    MeshRel back;
    CHECK(mesh_rel_decode(out, n, &back));
    CHECK(back.seq == 513 && back.type == 0x30 && back.len == 5 &&
          memcmp(back.payload, r.payload, 5) == 0 && back.h.from == 0 && back.h.to == 2);
    CHECK(!mesh_rel_decode(out, n - 1, &back)); /* its length is the datagram's */
    r.len = MESH_REL_MAX;
    CHECK(mesh_rel_encode(&r, out, sizeof out) == MESH_REL_FIXED + MESH_REL_MAX);
    r.len = MESH_REL_MAX + 1;
    CHECK(mesh_rel_encode(&r, out, sizeof out) == 0);

    /* 'K' and 'B' are ten bytes. */
    MeshHdr h = {MESH_ACK, MESH_VERSION, 7, 3, 1};
    CHECK(mesh_small_encode(&h, 40000, out, sizeof out) == MESH_HDR_LEN + 2);
    MeshHdr hb;
    uint16_t v = 0;
    CHECK(mesh_small_decode(out, MESH_HDR_LEN + 2, &hb, &v) && hb.magic == MESH_ACK && v == 40000 &&
          hb.from == 3 && hb.to == 1);
    out[0] = MESH_INPUT;
    CHECK(!mesh_small_decode(out, MESH_HDR_LEN + 2, &hb, &v));
    out[0] = MESH_BYE;
    CHECK(mesh_small_decode(out, MESH_HDR_LEN + 2, &hb, &v) && hb.magic == MESH_BYE);
    CHECK(!mesh_small_decode(out, MESH_HDR_LEN + 3, &hb, &v));

    /* AWAY: the port and its last frame. */
    uint8_t a[MESH_AWAY_LEN];
    mesh_away_encode(a, 2, 4321);
    uint8_t port = 0;
    int32_t frame = -2;
    CHECK(mesh_away_decode(a, sizeof a, &port, &frame) && port == 2 && frame == 4321);
    mesh_away_encode(a, 3, -1); /* it never sent a frame */
    CHECK(mesh_away_decode(a, sizeof a, &port, &frame) && port == 3 && frame == -1);
    a[0] = 4;
    CHECK(!mesh_away_decode(a, sizeof a, &port, &frame));
    CHECK(!mesh_away_decode(a, sizeof a - 1, &port, &frame));
}

static void test_addressing(void) {
    uint8_t dgram[MESH_SLOT_BYTES];
    for (int i = 0; i < MESH_SLOT_BYTES; i++) {
        dgram[i] = (uint8_t)i;
    }
    uint8_t slot[MESH_SLOT_BYTES];
    /* The port byte counts against the 512: 511 bytes of datagram fit. */
    CHECK(mesh_addr_wrap(2, dgram, MESH_DGRAM_MAX, slot, sizeof slot) == MESH_SLOT_BYTES);
    CHECK(slot[0] == 2 && memcmp(slot + 1, dgram, MESH_DGRAM_MAX) == 0);
    CHECK(mesh_addr_wrap(2, dgram, MESH_DGRAM_MAX + 1, slot, sizeof slot) == 0);
    CHECK(mesh_addr_wrap(4, dgram, 10, slot, sizeof slot) == 0); /* no port 4 */
    CHECK(mesh_addr_wrap(0, dgram, 0, slot, sizeof slot) == 0);  /* nothing to carry */
    CHECK(mesh_addr_wrap(1, dgram, 10, slot, 10) == 0);          /* no room */

    uint8_t from = 0;
    const uint8_t* d = NULL;
    size_t len = 0;
    CHECK(mesh_addr_wrap(3, dgram, 40, slot, sizeof slot) == 41);
    CHECK(mesh_addr_unwrap(slot, 41, &from, &d, &len) && from == 3 && len == 40 && d == slot + 1);
    CHECK(!mesh_addr_unwrap(slot, 1, &from, &d, &len)); /* a port byte alone */
    CHECK(!mesh_addr_unwrap(slot, MESH_SLOT_BYTES + 1, &from, &d, &len));
    slot[0] = 4;
    CHECK(!mesh_addr_unwrap(slot, 41, &from, &d, &len));

    /* Everything a mesh sends puts the version second, where an M2 engine
     * (protocol 9) reads its own: the two refuse each other. */
    CHECK(MESH_VERSION == 10);
    uint8_t probe[MESH_M2_PROBE_LEN];
    mesh_m2_probe(probe, 0xA1B2C3D4u, 1);
    const uint8_t want[MESH_M2_PROBE_LEN] = {
        'B', MESH_VERSION, 0xA1, 0xB2, 0xC3, 0xD4, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(memcmp(probe, want, sizeof want) == 0);
    /* A mesh reads the probe as nothing: a 'B' is ten bytes. */
    MeshHdr h;
    uint16_t v = 0;
    CHECK(!mesh_small_decode(probe, sizeof probe, &h, &v));
}

/* ---- the rules ----------------------------------------------------------- */

static void test_leader(void) {
    CHECK(mesh_leader(0x0F, 0, 0, 2) == 0);
    CHECK(mesh_leader(0x0F, 0, 0x01, 2) == 1); /* 0 silent to us: 1 leads */
    CHECK(mesh_leader(0x0F, 0x01, 0x02, 2) == 2);
    CHECK(mesh_leader(0x0F, 0x01, 0x02, 3) == 2);
    CHECK(mesh_leader(0x0A, 0, 0x02, 3) == 3); /* every other silent: we lead ourselves */
    CHECK(mesh_leader(0x0F, 0x0F, 0, 1) == 1); /* even settled by the others */
    CHECK(mesh_leader(0x05, 0, 0, 2) == 0);
    CHECK(mesh_leader(0, 0, 0, -1) == -1);
}

/* Settling needs a strict majority of the phones still in (gone by a BYE not
 * counted), except between two. */
static void test_quorum(void) {
    /* Three phones; this one, port 2, hears nobody (a stall, a deaf link):
     * the two that hear each other must not be settled by it. */
    CHECK(!mesh_quorum(0x07, 0, 0x03, 0, 2));
    CHECK(mesh_acting_leader(0x07, 0, 0x03, 0x03, 0, 2) == 0); /* it does not lead */
    /* It hears one of the two: two of three, it may settle the third. */
    CHECK(mesh_quorum(0x07, 0, 0x02, 0, 2));
    CHECK(mesh_quorum(0x07, 0, 0x04, 0, 0));
    CHECK(mesh_acting_leader(0x07, 0, 0x01, 0x01, 0, 2) == 1);
    /* Four phones split 2-2: neither half settles the other. */
    CHECK(!mesh_quorum(0x0F, 0, 0x0C, 0, 0));
    CHECK(!mesh_quorum(0x0F, 0, 0x03, 0, 3));
    CHECK(mesh_acting_leader(0x0F, 0, 0x03, 0x03, 0, 3) == 0);
    /* Four phones, one gone: three of four settle it. */
    CHECK(mesh_quorum(0x0F, 0, 0x08, 0, 0));
    CHECK(mesh_quorum(0x0F, 0, 0x01, 0, 1));
    CHECK(mesh_acting_leader(0x0F, 0, 0x01, 0x01, 0, 2) == 1);
    /* Two cut off together: the first to cross the limit is silent while
     * the second is only quiet. The quorum counts it as unheard already, so
     * neither is settled, and the lead does not move. */
    CHECK(!mesh_quorum(0x0F, 0, 0x0C, 0, 0));
    CHECK(mesh_acting_leader(0x0F, 0, 0x01, 0x05, 0, 1) == 0);
    /* ...and once it is settled, the three that are left are three. */
    CHECK(mesh_quorum(0x0F, 0x08, 0x04, 0, 0));
    CHECK(!mesh_quorum(0x0F, 0x08, 0x06, 0, 0));
    /* Two phones: no majority is possible, so each settles the other. */
    CHECK(mesh_quorum(0x03, 0, 0x02, 0, 0));
    CHECK(mesh_quorum(0x03, 0, 0x01, 0, 1));
    CHECK(mesh_acting_leader(0x03, 0, 0x01, 0x01, 0, 1) == 1);
    /* Three phones, as many as two once a third has left: a pair again. */
    CHECK(mesh_quorum(0x07, 0x04, 0x02, 0, 0));
    /* Phones that said BYE are out of the count: two of four leaving at once
     * leaves a pair, which settles them, and one of three leaving lets a
     * phone that hears nobody else settle the last. */
    CHECK(mesh_quorum(0x0F, 0, 0x05, 0x05, 1));
    CHECK(mesh_quorum(0x07, 0, 0x03, 0x01, 2));
    CHECK(!mesh_quorum(0x0F, 0, 0x07, 0x01, 3)); /* one BYE, two silent: 1 of 3 */
    CHECK(mesh_quorum(0x0F, 0x08, 0x07, 0x07, 3)); /* all the others gone */
}

static void test_settle_rules(void) {
    int32_t rep[MESH_PORTS] = {-1, 102, 99, -1};
    int32_t f = -9;
    CHECK(!mesh_settle_ready(100, rep, 0x02, &f)); /* port 1 holds two frames more */
    CHECK(mesh_settle_ready(100, rep, 0x04, &f) && f == 100);
    CHECK(mesh_settle_ready(102, rep, 0x06, &f) && f == 102);
    CHECK(mesh_settle_ready(-1, (int32_t[]){-1, -1, -1, -1}, 0x0E, &f) && f == -1);

    CHECK(mesh_relay_wanted(110, 100, false, 0));
    CHECK(!mesh_relay_wanted(103, 100, false, 0)); /* in flight from it directly */
    CHECK(mesh_relay_wanted(103, 100, false, MESH_RELAY_QUIET_MS));
    CHECK(mesh_relay_wanted(101, 100, true, 0)); /* settled: everyone to F */
    CHECK(!mesh_relay_wanted(100, 100, true, 100000));

    CHECK(mesh_away_accept(0x00, 1, 0, 2) == MESH_AWAY_SETTLE);
    CHECK(mesh_away_accept(0x00, 1, 0, 1) == MESH_AWAY_EVICTED);
    CHECK(mesh_away_accept(0x01, 1, 0, 2) == MESH_AWAY_IGNORE); /* from a settled phone */
    CHECK(mesh_away_accept(0x04, 1, 0, 2) == MESH_AWAY_IGNORE); /* already */
    CHECK(mesh_away_accept(0x00, 1, 1, 2) == MESH_AWAY_IGNORE); /* from ourselves */

    MeshAwayView v;
    memset(&v, 0, sizeof v);
    v.humans = 0x07;
    v.self = 0;
    v.away_ms = 1500;
    v.load_ms = 9000;
    v.stable_need_ms = 250;
    for (int q = 0; q < MESH_PORTS; q++) {
        for (int p = 0; p < MESH_PORTS; p++) {
            v.reports[q][p] = 50;
        }
        v.have[q] = 50;
    }
    v.quiet_ms[2] = 1499;
    v.stable_ms[2] = 1499;
    CHECK(mesh_silent_mask(&v) == 0);
    CHECK(mesh_away_decide(&v, &f) == -1);
    v.quiet_ms[2] = 1500;
    v.stable_ms[2] = 1500;
    CHECK(mesh_silent_mask(&v) == 0x04);
    CHECK(mesh_away_decide(&v, &f) == 2 && f == 50);
    v.loading[2] = true; /* a phone loading its scene gets load_ms */
    CHECK(mesh_silent_mask(&v) == 0 && mesh_away_decide(&v, &f) == -1);
    v.loading[2] = false;
    v.reports[1][2] = 52; /* port 1 holds more: wait for its relay */
    CHECK(mesh_away_decide(&v, &f) == -1);
    v.have[2] = 52;
    CHECK(mesh_away_decide(&v, &f) == 2 && f == 52);
    v.self = 1; /* port 0 leads: port 1 does nothing */
    CHECK(mesh_away_decide(&v, &f) == -1);
    v.quiet_ms[0] = 1500; /* port 0 silent too: port 1 hears nobody, one of */
    v.stable_ms[0] = 1500; /* three, and settles neither of the two */
    CHECK(mesh_silent_mask(&v) == 0x05 && mesh_away_decide(&v, &f) == -1);
    v.bye[0] = true; /* unless port 0 said BYE: then it is the last two */
    CHECK(mesh_away_decide(&v, &f) == 0 || mesh_away_decide(&v, &f) == 2);
    v.bye[0] = false;
    v.self = 0;
    v.quiet_ms[0] = 0;
    v.bye[1] = true; /* a BYE is silent now, once the frame stops moving */
    v.stable_ms[1] = 100;
    v.quiet_ms[2] = 0;
    CHECK(mesh_silent_mask(&v) == 0x02 && mesh_away_decide(&v, &f) == -1);
    v.stable_ms[1] = 250;
    CHECK(mesh_away_decide(&v, &f) == 1 && f == 50);
    v.settled = 0x02; /* never twice */
    CHECK(mesh_silent_mask(&v) == 0 && mesh_away_decide(&v, &f) == -1);
}

/* ---- a simulated mesh ------------------------------------------------------
 * Phones exchange input packets every 16 ms over links that can be cut, with
 * a few milliseconds of latency: what each says it holds, its own frames and
 * whatever third port's frames mesh_relay_wanted says to relay. Each runs the
 * real decision (mesh_away_decide over its own view, quiet measured as
 * net_mesh.c measures it) and the real acceptance of an AWAY, resent every
 * 200 ms until it lands. */

#define SIM_PHONES 4
#define SIM_LAT 6
#define SIM_EVENTS 4096

typedef struct SimPhone {
    bool alive, bye, evicted;
    int32_t wrote;            /* its own newest frame */
    int32_t have[MESH_PORTS]; /* what it holds of each port */
    int32_t rep[MESH_PORTS][MESH_PORTS];
    int64_t heard[MESH_PORTS]; /* ms, -1 never */
    int64_t moved[MESH_PORTS];
    int32_t last_max[MESH_PORTS];
    uint8_t settled;
    int32_t settle[MESH_PORTS];
    int64_t settled_at[MESH_PORTS];
    int settled_by[MESH_PORTS];
    int32_t evicted_frame;
    uint8_t away_owed[MESH_PORTS]; /* per destination: ports it still has to be told */
    int64_t last_decide, grace_until;
    /* A resync (net.c "resync", net_mesh.c mesh_resync_hold): the leader
     * holds, writing no frame, until every receiver has answered; each
     * receiver's window runs from the announcement until the leader's input
     * moves past rs_release. */
    bool hold, rs_on, answered;
    int rs_leader;
    int32_t rs_release;
    int64_t rs_since;
    uint8_t rs_owed;  /* the leader: receivers not yet told */
    bool answer_owed; /* a receiver: its answer not yet delivered */
    uint8_t answers;  /* the leader: receivers that answered */
} SimPhone;

typedef struct SimEvent {
    int64_t at;
    int from, to;
    bool away;
    int rs; /* 1 the resync announcement (frame: rs_release), 2 an answer */
    int port;      /* away */
    int32_t frame; /* away */
    int32_t have[MESH_PORTS];
    int32_t give[MESH_PORTS]; /* frames delivered of each port, -1 none */
} SimEvent;

static struct {
    int n;
    uint8_t humans;
    SimPhone ph[SIM_PHONES];
    bool up[SIM_PHONES][SIM_PHONES];
    SimEvent ev[SIM_EVENTS];
    int nev;
    int64_t now;
    int away_ms, load_ms;
    int32_t scene_lead; /* frames a phone needs before away_ms applies */
    int window;         /* frames a phone runs past what it holds of the others; 0 any */
    int64_t bytes_at;   /* when the snapshot's bytes reach the receivers, -1 never */
} sim;

static void sim_init(int n, int32_t start) {
    memset(&sim, 0, sizeof sim);
    sim.n = n;
    sim.humans = (uint8_t)((1u << n) - 1);
    sim.away_ms = 1500;
    sim.load_ms = 8000;
    sim.scene_lead = 12;
    sim.bytes_at = -1;
    for (int p = 0; p < n; p++) {
        SimPhone* ph = &sim.ph[p];
        ph->alive = true;
        ph->wrote = start;
        ph->last_decide = -1;
        for (int q = 0; q < MESH_PORTS; q++) {
            ph->have[q] = q == p ? start : -1;
            ph->heard[q] = -1;
            ph->moved[q] = -1;
            ph->last_max[q] = -1;
            ph->settle[q] = -2;
            ph->settled_at[q] = -1;
            ph->settled_by[q] = -1;
            for (int r = 0; r < MESH_PORTS; r++) {
                ph->rep[q][r] = -1;
            }
        }
        for (int q = 0; q < n; q++) {
            sim.up[p][q] = true;
        }
    }
}

static bool sim_settled(const SimPhone* ph, int p) {
    return (ph->settled >> p) & 1u;
}

static int32_t sim_quiet(const SimPhone* ph, int p) {
    int64_t t = ph->heard[p] > ph->moved[p] ? ph->heard[p] : ph->moved[p];
    return (int32_t)(sim.now - (t < 0 ? 0 : t));
}

static void sim_post(SimEvent e) {
    if (sim.nev < SIM_EVENTS && sim.up[e.from][e.to]) {
        e.at = sim.now + SIM_LAT;
        sim.ev[sim.nev++] = e;
    }
}

static void sim_settle(int self, int p, int32_t f, int by) {
    SimPhone* ph = &sim.ph[self];
    if (sim_settled(ph, p)) {
        return;
    }
    ph->settled |= (uint8_t)(1u << p);
    ph->settle[p] = f;
    ph->settled_at[p] = sim.now;
    ph->settled_by[p] = by;
    if (by == self) {
        for (int q = 0; q < sim.n; q++) {
            if (q != self) {
                ph->away_owed[q] |= (uint8_t)(1u << p);
            }
        }
    }
}

static void sim_send(int a) {
    SimPhone* pa = &sim.ph[a];
    for (int b = 0; b < sim.n; b++) {
        if (b == a || sim_settled(pa, b) || sim.ph[a].bye) {
            continue;
        }
        SimEvent e;
        memset(&e, 0, sizeof e);
        e.from = a;
        e.to = b;
        for (int p = 0; p < MESH_PORTS; p++) {
            e.have[p] = pa->have[p];
            e.give[p] = -1;
        }
        e.give[a] = pa->wrote;
        for (int p = 0; p < sim.n; p++) {
            if (p == a || p == b) {
                continue;
            }
            int32_t mine = pa->have[p];
            if (sim_settled(pa, p) && mine > pa->settle[p]) {
                mine = pa->settle[p];
            }
            if (mesh_relay_wanted(mine, pa->rep[b][p], sim_settled(pa, p), sim_quiet(pa, p))) {
                e.give[p] = mine;
            }
        }
        sim_post(e);
    }
}

static void sim_send_aways(int a) {
    SimPhone* pa = &sim.ph[a];
    for (int b = 0; b < sim.n; b++) {
        for (int p = 0; p < MESH_PORTS; p++) {
            if ((pa->away_owed[b] >> p) & 1u) {
                SimEvent e;
                memset(&e, 0, sizeof e);
                e.from = a;
                e.to = b;
                e.away = true;
                e.port = p;
                e.frame = pa->settle[p];
                sim_post(e); /* resent until it lands: the lane's ack */
            }
        }
    }
}

/* The resync's two reliable messages, resent every 200 ms until they land. */
static void sim_send_resync(int a) {
    SimPhone* pa = &sim.ph[a];
    for (int b = 0; b < sim.n; b++) {
        const bool announce = (pa->rs_owed >> b) & 1u;
        const bool answer = b == pa->rs_leader && pa->answer_owed;
        if (b == a || (!announce && !answer)) {
            continue;
        }
        SimEvent e;
        memset(&e, 0, sizeof e);
        e.from = a;
        e.to = b;
        e.rs = announce ? 1 : 2;
        e.frame = pa->rs_release;
        sim_post(e);
    }
}

static void sim_receive(const SimEvent* e) {
    SimPhone* pb = &sim.ph[e->to];
    if (!pb->alive) {
        return;
    }
    if (e->rs == 1) { /* net.c net_resync_announce -> mesh_resync_hold */
        pb->heard[e->from] = sim.now;
        sim.ph[e->from].rs_owed &= (uint8_t) ~(1u << e->to);
        if (!pb->rs_on || pb->rs_leader != e->from || pb->rs_release != e->frame) {
            pb->rs_on = true;
            pb->rs_leader = e->from;
            pb->rs_release = e->frame;
            pb->rs_since = sim.now;
        }
        return;
    }
    if (e->rs == 2) { /* net.c net_resync_answer */
        pb->heard[e->from] = sim.now;
        sim.ph[e->from].answer_owed = false;
        pb->answers |= (uint8_t)(1u << e->from);
        return;
    }
    if (e->away) {
        sim.ph[e->from].away_owed[e->to] &= (uint8_t) ~(1u << e->port);
        const int what = mesh_away_accept(pb->settled, e->to, e->from, e->port);
        if (what == MESH_AWAY_EVICTED && !pb->evicted) {
            pb->evicted = true;
            pb->evicted_frame = e->frame;
            pb->alive = false; /* it leaves */
        } else if (what == MESH_AWAY_SETTLE) {
            sim_settle(e->to, e->port, e->frame, e->from);
        }
        return;
    }
    pb->heard[e->from] = sim.now;
    for (int p = 0; p < MESH_PORTS; p++) {
        if (e->have[p] > pb->rep[e->from][p]) {
            pb->rep[e->from][p] = e->have[p];
        }
        if (p == e->to || e->give[p] < 0) {
            continue;
        }
        int32_t got = e->give[p];
        if (sim_settled(pb, p) && got > pb->settle[p]) {
            got = pb->settle[p]; /* net.c's slot_take clips at F */
        }
        if (got > pb->have[p]) {
            pb->have[p] = got;
        }
    }
}

static void sim_decide(int self) {
    SimPhone* ph = &sim.ph[self];
    /* net_mesh.c service(): after a gap of its own, a phone measures the
     * others' silence again from now. */
    if (ph->last_decide >= 0 && sim.now - ph->last_decide > 1000) {
        ph->grace_until = sim.now + sim.away_ms;
    }
    ph->last_decide = sim.now;
    /* net_mesh.c track_max */
    for (int p = 0; p < sim.n; p++) {
        if (p == self) {
            continue;
        }
        int32_t mx = ph->have[p];
        for (int q = 0; q < sim.n; q++) {
            if (q != self && q != p && ph->rep[q][p] > mx) {
                mx = ph->rep[q][p];
            }
        }
        if (mx != ph->last_max[p]) {
            ph->last_max[p] = mx;
            ph->moved[p] = sim.now;
        }
    }
    /* net_mesh.c service(): a resync's window ends once its leader has moved
     * on (or is gone), and every silence is measured afresh from there. */
    if (ph->rs_on) {
        const int lead = ph->rs_leader;
        const int32_t since = (int32_t)(sim.now - ph->rs_since);
        const bool in = !sim_settled(ph, lead) && !(sim.ph[lead].bye && ph->heard[lead] >= 0);
        if (lead == self ? since >= MESH_RESYNC_WINDOW_MS :
                           mesh_resync_over(in, ph->last_max[lead], ph->rs_release, since))
        {
            ph->rs_on = false;
            ph->grace_until = sim.now + sim.away_ms;
        }
    }
    if (sim.now < ph->grace_until) {
        return;
    }
    for (int i = 0; i < MESH_PORTS; i++) {
        MeshAwayView v;
        memset(&v, 0, sizeof v);
        v.humans = sim.humans;
        v.settled = ph->settled;
        v.self = self;
        v.away_ms = sim.away_ms;
        v.load_ms = sim.load_ms;
        v.stable_need_ms = 250;
        v.resync = ph->rs_on;
        for (int p = 0; p < MESH_PORTS; p++) {
            v.have[p] = ph->have[p];
            for (int q = 0; q < MESH_PORTS; q++) {
                v.reports[q][p] = ph->rep[q][p];
            }
            if (p >= sim.n || p == self) {
                continue;
            }
            v.bye[p] = sim.ph[p].bye && ph->heard[p] >= 0; /* its BYE reached us */
            v.quiet_ms[p] = sim_quiet(ph, p);
            v.stable_ms[p] = (int32_t)(sim.now - (ph->moved[p] < 0 ? 0 : ph->moved[p]));
            v.loading[p] = ph->have[p] < sim.scene_lead;
        }
        int32_t f = -2;
        const int p = mesh_away_decide(&v, &f);
        if (p < 0) {
            return;
        }
        sim_settle(self, p, f, self);
    }
}

/* With sim.window, a phone runs a frame only while it holds every other
 * phone still in to within that many frames: rollback's window, so a phone
 * that hears nothing of another stalls, as it does. */
static bool sim_may_run(int p) {
    const SimPhone* ph = &sim.ph[p];
    if (sim.window == 0) {
        return true;
    }
    for (int q = 0; q < sim.n; q++) {
        if (q != p && !sim_settled(ph, q) && ph->have[q] < ph->wrote + 1 - sim.window) {
            return false;
        }
    }
    return true;
}

/* The leader takes a snapshot and holds (net.c resync_point). */
static void sim_resync_begin(int lead) {
    SimPhone* ph = &sim.ph[lead];
    ph->hold = true;
    ph->rs_on = true;
    ph->rs_leader = lead;
    ph->rs_release = ph->wrote;
    ph->rs_since = sim.now;
    ph->rs_owed = (uint8_t)(sim.humans & ~ph->settled & ~(1u << lead));
    ph->answers = 0;
}

/* The resync's progress each ms: a receiver answers once the bytes are in;
 * the leader lets go once every phone still in has answered, or at 30 s. */
static void sim_resync_step(int p) {
    SimPhone* ph = &sim.ph[p];
    if (ph->rs_on && ph->rs_leader != p && !ph->answered && sim.bytes_at >= 0 &&
        sim.now >= sim.bytes_at)
    {
        ph->answered = true;
        ph->answer_owed = true;
    }
    if (ph->hold) {
        const uint8_t in = (uint8_t)(sim.humans & ~ph->settled & ~(1u << p));
        if ((in & ~ph->answers) == 0 || sim.now - ph->rs_since >= 30000) {
            ph->hold = false;
            ph->rs_on = false; /* mesh_resync_release */
            ph->grace_until = sim.now + sim.away_ms;
        }
    }
}

/* Run until `until` ms: phones make a frame and send every 16 ms. */
static void sim_run(int64_t until) {
    for (; sim.now < until; sim.now++) {
        for (int i = 0; i < sim.nev;) {
            if (sim.ev[i].at <= sim.now) {
                const SimEvent e = sim.ev[i];
                sim.ev[i] = sim.ev[--sim.nev];
                sim_receive(&e);
            } else {
                i++;
            }
        }
        for (int p = 0; p < sim.n; p++) {
            SimPhone* ph = &sim.ph[p];
            if (!ph->alive) {
                continue;
            }
            sim_resync_step(p);
            if (sim.now % 16 == p) { /* the phones' frames are not in phase */
                if (!ph->bye && !ph->hold && sim_may_run(p)) {
                    ph->wrote++;
                    ph->have[p] = ph->wrote;
                }
                sim_send(p); /* every 16 ms, held or stalled: keepalives */
            }
            if (sim.now % 200 == 0) {
                sim_send_aways(p);
                sim_send_resync(p);
            }
            sim_decide(p);
        }
    }
}

/* Every live phone has settled port p, all at the same frame, by `by`. */
static bool sim_agree(int p, int32_t* frame, int by) {
    int32_t f = -2;
    for (int q = 0; q < sim.n; q++) {
        const SimPhone* ph = &sim.ph[q];
        if (q == p || !ph->alive) {
            continue;
        }
        if (!sim_settled(ph, p) || (f != -2 && ph->settle[p] != f) ||
            (by >= 0 && ph->settled_by[p] != by && q != by && ph->settled_by[p] == q))
        {
            return false;
        }
        f = ph->settle[p];
    }
    *frame = f;
    return f != -2;
}

static uint8_t sim_settled_any(int q) {
    return sim.ph[q].settled;
}

static void test_sim_one_goes(void) {
    for (int n = 3; n <= 4; n++) {
        sim_init(n, 100);
        sim_run(2000);
        for (int q = 0; q < n; q++) {
            CHECK(sim_settled_any(q) == 0); /* all talking: nobody settled */
        }
        sim.ph[n - 1].alive = false; /* the last phone dies */
        const int32_t last = sim.ph[n - 1].wrote;
        sim_run(3400);
        CHECK(sim_settled_any(0) == 0); /* not before away_ms */
        sim_run(6000);
        int32_t f = -2;
        CHECK(sim_agree(n - 1, &f, 0));
        CHECK(f == last); /* every frame it sent is played, none it did not */
        CHECK(sim.ph[0].settled_by[n - 1] == 0);
        CHECK(sim.ph[0].settled_at[n - 1] >= 2000 - 16 + 1500); /* its last word, then away_ms */
        for (int q = 0; q < n - 1; q++) {
            CHECK(sim.ph[q].settled == (1u << (n - 1)));
            CHECK(sim.ph[q].have[n - 1] >= f); /* holds everything up to F */
        }
    }
}

/* Port 0 stops hearing port 3 directly, so 3's frames reach it relayed: that
 * is no silence. When 3 dies, its last frames are only at 1 and 2 -- the
 * leader waits for them before it settles, and F is theirs. */
static void test_sim_relay_first(void) {
    sim_init(4, 100);
    sim_run(1000);
    sim.up[3][0] = false;
    sim_run(4000);
    CHECK(sim_settled_any(0) == 0 && sim_settled_any(1) == 0 && sim_settled_any(2) == 0);
    CHECK(sim.ph[0].have[3] >= sim.ph[3].wrote - MESH_RELAY_LAG - 2); /* relayed along */
    sim.ph[3].alive = false;
    const int32_t last = sim.ph[3].wrote;
    sim_run(8000);
    int32_t f = -2;
    CHECK(sim_agree(3, &f, 0));
    CHECK(f == last);
    CHECK(sim.ph[0].have[3] == last);
    CHECK(sim.ph[1].settled == 0x08 && sim.ph[2].settled == 0x08);
}

/* The leader goes: the next lowest phone settles it, by the same rule, and
 * the one after it does not act on its own. */
static void test_sim_leader_goes(void) {
    for (int n = 3; n <= 4; n++) {
        sim_init(n, 100);
        sim_run(2000);
        sim.ph[0].alive = false;
        const int32_t last = sim.ph[0].wrote;
        sim_run(7000);
        int32_t f = -2;
        CHECK(sim_agree(0, &f, 1));
        CHECK(f == last);
        for (int q = 1; q < n; q++) {
            CHECK(sim.ph[q].settled == 0x01);
            CHECK(sim.ph[q].settled_by[0] == 1);
        }
    }
    /* The leader and another of four at once: from the two left that is a
     * 2-2 split, which settles nobody... */
    sim_init(4, 100);
    sim_run(2000);
    sim.ph[0].alive = false;
    sim.ph[2].alive = false;
    const int32_t l0 = sim.ph[0].wrote, l2 = sim.ph[2].wrote;
    sim_run(7000);
    CHECK(sim_settled_any(1) == 0 && sim_settled_any(3) == 0);
    /* ...until their links are reported failed (their BYE): then the pair
     * left settles both, the next one leading. */
    sim.ph[0].bye = true;
    sim.ph[2].bye = true;
    sim_run(9000);
    int32_t f0 = -2, f2 = -2;
    CHECK(sim_agree(0, &f0, 1) && f0 == l0);
    CHECK(sim_agree(2, &f2, 1) && f2 == l2);
    CHECK(sim.ph[1].settled == 0x05 && sim.ph[3].settled == 0x05);
    CHECK(sim.ph[3].settled_by[0] == 1 && sim.ph[3].settled_by[2] == 1);
}

/* The phone that hears nobody (Eggy's e2e: port 3's page stopped receiving
 * for more than away_ms, its game thread still servicing, so no freeze
 * grace): its own datagrams still land, the other two hear everyone, and it
 * settles nobody. Once it hears again the match goes on, all three in. */
static void test_sim_deaf_phone(void) {
    sim_init(3, 100);
    sim_run(2000);
    sim.up[0][2] = false;
    sim.up[1][2] = false;
    sim_run(9000);
    for (int q = 0; q < 3; q++) {
        CHECK(sim_settled_any(q) == 0);
        CHECK(sim.ph[q].alive && !sim.ph[q].evicted);
    }
    sim.up[0][2] = true;
    sim.up[1][2] = true;
    sim_run(12000);
    for (int q = 0; q < 3; q++) {
        CHECK(sim_settled_any(q) == 0);
        CHECK(sim.ph[q].alive && !sim.ph[q].evicted);
    }
    /* Deaf for good, and then really gone: the two that hear each other
     * settle it. */
    sim.ph[2].alive = false;
    const int32_t last = sim.ph[2].wrote;
    sim_run(16000);
    int32_t f = -2;
    CHECK(sim_agree(2, &f, 0) && f == last);
}

/* Four phones cut into two pairs: neither pair settles the other, and the
 * match is whole again when the links come back. */
static void test_sim_split(void) {
    sim_init(4, 100);
    sim_run(2000);
    for (int a = 0; a < 2; a++) {
        for (int b = 2; b < 4; b++) {
            sim.up[a][b] = false;
            sim.up[b][a] = false;
        }
    }
    sim_run(9000);
    for (int a = 0; a < 2; a++) {
        for (int b = 2; b < 4; b++) {
            sim.up[a][b] = true;
            sim.up[b][a] = true;
        }
    }
    sim_run(12000);
    for (int q = 0; q < 4; q++) {
        CHECK(sim_settled_any(q) == 0);
        CHECK(sim.ph[q].alive && !sim.ph[q].evicted);
    }
}

/* Cut every link of port p both ways, or mend them. */
static void sim_cut(int p, bool cut) {
    for (int q = 0; q < sim.n; q++) {
        if (q != p) {
            sim.up[p][q] = !cut;
            sim.up[q][p] = !cut;
        }
    }
}

/* Eggy's e2e (two phones, a starved Mac): the leader held for its snapshot
 * while the page moved 4 MB, its datagrams starved for seconds, and the
 * other phone settled it away 2.3 s in. Here the bytes take 10 s, and the
 * leader's links carry nothing for 4 s at a time meanwhile: nobody is
 * settled, the resync completes, and the match goes on. */
static void test_sim_resync_hold(void) {
    for (int n = 2; n <= 3; n++) {
        sim_init(n, 100);
        sim.window = 8;
        sim_run(2000);
        sim_resync_begin(0);
        const int32_t held = sim.ph[0].wrote;
        sim_run(2100);
        for (int r = 1; r < n; r++) {
            CHECK(sim.ph[r].rs_on && sim.ph[r].rs_leader == 0 && sim.ph[r].rs_release == held);
        }
        sim_cut(0, true);
        sim_run(6100);
        sim_cut(0, false);
        sim_run(6300);
        sim_cut(0, true);
        sim_run(10300);
        sim_cut(0, false);
        sim_run(10400);
        sim_cut(0, true);
        sim.bytes_at = 12000;
        sim_run(12000);
        sim_cut(0, false);
        for (int q = 0; q < n; q++) {
            CHECK(sim.ph[q].wrote <= held + sim.window); /* all held up meanwhile */
        }
        sim_run(18000);
        for (int q = 0; q < n; q++) {
            CHECK(sim.ph[q].alive && !sim.ph[q].evicted);
            CHECK(sim_settled_any(q) == 0);
            CHECK(!sim.ph[q].hold && !sim.ph[q].rs_on); /* the resync is over everywhere */
            CHECK(sim.ph[q].wrote > held + 200);         /* and the match goes on */
        }
        CHECK(sim.ph[0].answers == (uint8_t)(sim.humans & ~1u));
    }
}

/* A phone really gone during a resync is still settled: silent, once the
 * hold is over (the leader lets go at 30 s without its answer), or at once
 * on a BYE. */
static void test_sim_resync_gone(void) {
    sim_init(3, 100);
    sim.window = 8;
    sim_run(2000);
    sim_resync_begin(0);
    sim_run(2100);
    sim.ph[2].alive = false; /* dies mid-transfer, silently */
    sim_run(2000 + 30000 - 100);
    CHECK(sim_settled_any(0) == 0 && sim_settled_any(1) == 0);
    CHECK(sim.ph[0].hold);
    sim_run(2000 + 30000 + sim.away_ms + 1000);
    int32_t f = -2;
    CHECK(!sim.ph[0].hold);
    CHECK(sim_agree(2, &f, 0));
    CHECK(sim.ph[1].alive && !sim.ph[1].evicted && sim_settled_any(1) == 0x04);

    sim_init(3, 100);
    sim.window = 8;
    sim_run(2000);
    sim_resync_begin(0);
    sim_run(2100);
    sim.ph[2].bye = true; /* its link reported failed: settled once still */
    sim.ph[2].alive = false;
    sim_run(3000);
    CHECK(sim_agree(2, &f, 0));
}

/* Two phones: either side settles the other after away_ms, as before. */
static void test_sim_pair(void) {
    sim_init(2, 100);
    sim_run(2000);
    sim.ph[1].alive = false;
    const int32_t last = sim.ph[1].wrote;
    sim_run(5000);
    CHECK(sim.ph[0].settled == 0x02 && sim.ph[0].settle[1] == last);
}

/* A phone frozen past away_ms comes back to find itself settled: it leaves,
 * and whatever it decided about the others while it caught up is ignored. */
static void test_sim_evicted(void) {
    sim_init(3, 100);
    sim_run(2000);
    sim.ph[2].alive = false;
    sim_run(5000);
    sim.ph[2].alive = true; /* back, with everyone silent since 2000 to it */
    sim_run(6000);
    CHECK(sim.ph[2].evicted);
    CHECK(sim.ph[2].evicted_frame == sim.ph[0].settle[2]);
    CHECK(sim.ph[0].settled == 0x04 && sim.ph[1].settled == 0x04);
}

/* A BYE is settled as soon as its last frames have stopped moving. */
static void test_sim_bye(void) {
    sim_init(3, 100);
    sim_run(2000);
    sim.ph[1].bye = true; /* says so, and sends nothing more */
    sim.ph[1].alive = false;
    sim.ph[0].heard[1] = sim.now; /* its BYE landed */
    sim_run(2600);
    int32_t f = -2;
    CHECK(sim_agree(1, &f, 0));
    CHECK(sim.ph[0].settled_at[1] < 2000 + 1500);
}

/* A phone loading its scene sends nothing for a while: it gets load_ms. */
static void test_sim_loading(void) {
    sim_init(3, 1); /* frame 1: its scene's first frames are not reached yet */
    sim.ph[2].alive = false;
    sim_run(6000);
    CHECK(sim_settled_any(0) == 0 && sim_settled_any(1) == 0);
    sim_run(9000);
    int32_t f = -2;
    CHECK(sim_agree(2, &f, 0));
}

int main(void) {
    test_input_codec();
    test_rel_codec();
    test_addressing();
    test_leader();
    test_quorum();
    test_settle_rules();
    test_sim_one_goes();
    test_sim_relay_first();
    test_sim_leader_goes();
    test_sim_evicted();
    test_sim_bye();
    test_sim_loading();
    test_sim_deaf_phone();
    test_sim_split();
    test_sim_pair();
    test_sim_resync_hold();
    test_sim_resync_gone();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("eggy_mesh: all passed");
    return 0;
}

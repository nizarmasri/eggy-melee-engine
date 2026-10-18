/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A mesh of two to four instances, one per human port (src/pc/eggy, EGGY.md
 * "Netplay"): the transport net.c's rollback session runs on when net.mesh
 * is set, in place of its two-peer one. Slippi's doubles model: every phone
 * sends its own port's input to every other phone, and rollback covers every
 * remote port, each a slot of net.c's (net.c "remote slots").
 *
 * Everything here runs on the game thread, which in the browser is also the
 * page's: the page delivers datagrams into net_web.c's receive ring and calls
 * the pump (mesh_rx_now) on arrival, which only stamps and queues them -- it
 * may run while a tick is parked in a disc read, so nothing it does touches
 * the simulation -- and sends a keepalive when the game thread has been
 * quiet, so a phone loading a scene still answers. recv_inputs applies the
 * queue (mesh_recv). There is no receive thread and no transmit timer.
 *
 * On the link, every datagram is one of net_mesh_wire.h's, behind the ring's
 * leading port byte. What is per phone:
 *   - input: one packet per frame to every other phone. It carries this
 *     phone's own input from the first frame the receiver lacks (its report)
 *     up to MESH_RED frames, what this phone holds of every port (have), its
 *     confirmed checksum, its frame advantage over the receiver, and echoes
 *     the receiver's newest packet with how long it was held here, which
 *     times the round trip without acks. Where this phone holds a third
 *     port's input the receiver lacks, by a few frames or once that port has
 *     gone quiet, it relays it.
 *   - reliable: one ordered lane per phone pair, the whole queue resent every
 *     MESH_REL_RESEND_NS until a cumulative 'K' covers it. The caller's
 *     messages, net.c's scene hand-off and the auto delay, and AWAY.
 *
 * Away. Once armed (pc_net_mesh_arm: the match's hold), a phone that has
 * been silent for away_ms (load_ms while it is still loading its scene) is
 * settled by the acting leader: the lowest human port not settled and not
 * silent to this phone, so a silent leader is replaced by the next one by the
 * same rule. F, the last frame of its input the rest will ever use, is the
 * newest frame of it any live phone holds; the leader waits until it holds
 * that much itself (the others relay it) and the number has stopped moving,
 * then announces AWAY(port, F) to every phone. Every phone feeds that port
 * neutral input from F + 1 (net_slot_settle) and goes on without it. A phone
 * told it was settled itself leaves (net_peer_gone). Nothing is resumed.
 * net_mesh_wire.c holds the rules, so tests/eggy/eggy_mesh_test.c runs them.
 *
 * Time sync is net_sync.c's, fed with the pair of the phone this one runs
 * furthest ahead of; ping is the worst phone's; the delay announcer is the
 * acting leader. Checksums are compared with every phone.
 */
#include "compat.h"
#include "pc/net_internal.h"

#include <SDL3/SDL_timer.h>
#include <string.h>

#define MESH_AWAY_MS 1500
#define MESH_LOAD_MS 120000
#define MESH_STABLE_MS 250
#define MESH_LOAD_LEAD 10 /* frames into a scene before away_ms applies */
#define MESH_REL_Q 16     /* reliable messages queued per phone */
#define MESH_REL_RESEND_NS 200000000ull
#define MESH_RXQ 256
#define MESH_KEEPALIVE_NS 50000000ull /* the pump's, while the game thread is busy */
#define MESH_PROBE_NS 1000000000ull
#define MESH_FREEZE_NS 1000000000ull /* a gap this long is this phone's own freeze */
#define MESH_PHASE_NS 1000000000ull  /* a phase sample older than this is stale */
#define MESH_REL_AWAY 0x05

_Static_assert(sizeof(MeshPad) == sizeof(WirePad), "the pads are one wire image");
_Static_assert(MESH_REL_MAX == REL_MAX, "a reliable payload is the same size");

typedef struct MeshRelMsg {
    uint16_t seq;
    uint8_t type;
    uint16_t len;
    uint8_t payload[MESH_REL_MAX];
} MeshRelMsg;

typedef struct MeshPeer {
    int slot;          /* net.c's slot for this port */
    uint64_t heard_ns; /* its last datagram, 0 never */
    bool bye, incompatible;
    bool warned;
    int32_t have[MESH_PORTS]; /* what it says it holds of each port */
    /* input packets, and the round trip they time */
    uint16_t tx_seq;
    uint16_t tx_seqs[64];
    uint64_t tx_ns[64];
    bool rx_valid;
    uint16_t rx_seq; /* its newest packet, echoed back */
    uint64_t rx_ns;  /* when that arrived */
    uint32_t rtt_us; /* smoothed */
    /* phase: our frame advantage over it, less its over us */
    int32_t off;
    uint64_t off_ns; /* 0: no sample */
    /* reliable lane */
    MeshRelMsg tx[MESH_REL_Q];
    int tx_head, tx_n;
    uint16_t tx_next;
    uint64_t tx_sent_ns;
    uint16_t rx_expect;
    /* its checksums by frame */
    struct {
        int32_t frame;
        uint32_t ck;
    } ck[RING];
    int32_t ck_checked;
    uint64_t probe_ns;
} MeshPeer;

typedef struct MeshRx {
    uint64_t ns;
    uint16_t len;
    uint8_t from;
    uint8_t data[MESH_DGRAM_MAX];
} MeshRx;

typedef struct MeshCall {
    uint8_t from;
    uint8_t type;
    uint16_t len;
    uint8_t payload[MESH_REL_MAX];
} MeshCall;

static int s_away_ms = MESH_AWAY_MS, s_load_ms = MESH_LOAD_MS; /* pc_net_mesh_set_timing */

static struct {
    uint8_t humans;
    int self;
    bool open;
    bool armed;
    MeshPeer peer[MESH_PORTS]; /* by port: the other humans' */
    uint8_t settled;
    int32_t settle[MESH_PORTS];
    uint8_t silent; /* as the last service saw it */
    bool evicted;
    int32_t evicted_frame;
    int away_ms, load_ms;
    uint64_t open_ns, last_service_ns, grace_until_ns, last_send_ns;
    int32_t last_max[MESH_PORTS]; /* newest frame of each port anyone holds */
    uint64_t max_moved_ns[MESH_PORTS];
    MeshRx q[MESH_RXQ];
    int q_head, q_n;
    MeshCall call[MESH_REL_Q];
    int call_head, call_n;
    unsigned rx_bad, rx_full;
    bool warned_conflict;
} m;

/* ---- the page's link ----------------------------------------------------- */

static void link_send(int port, const uint8_t* dgram, size_t len) {
#ifdef __EMSCRIPTEN__
    uint8_t out[MESH_SLOT_BYTES];
    const size_t n = mesh_addr_wrap((uint8_t)port, dgram, len, out, sizeof out);
    if (n > 0 && net_web_send(out, n) == (int)n) {
        net.tx_pkts++;
    }
#else
    (void)port, (void)dgram, (void)len;
#endif
}

static int link_recv(uint8_t* buf, size_t cap) {
#ifdef __EMSCRIPTEN__
    return net_web_recv(buf, cap);
#else
    (void)buf, (void)cap;
    return -1;
#endif
}

/* ---- who is who ---------------------------------------------------------- */

static bool is_peer(int p) {
    return p >= 0 && p < MESH_PORTS && p != m.self && (m.humans & (1u << p)) != 0;
}

static bool settled(int p) {
    return (m.settled & (1u << p)) != 0;
}

/* Still in the match from here: sent to and waited for. */
static bool live(int p) {
    return is_peer(p) && !settled(p) && !m.peer[p].bye;
}

static int32_t have_of(int p) {
    if (p == m.self) {
        return net_local_wrote();
    }
    return is_peer(p) ? net_slot_have(m.peer[p].slot) : -1;
}

static int32_t ms_since(uint64_t now, uint64_t then) {
    if (now <= then) {
        return 0;
    }
    const uint64_t ms = (now - then) / 1000000ull;
    return ms > INT32_MAX ? INT32_MAX : (int32_t)ms;
}

/* Since the last sign of port p: a datagram from it, or the newest frame of
 * its input anyone holds moving. */
static int32_t quiet_ms(int p, uint64_t now) {
    uint64_t t = m.peer[p].heard_ns > m.max_moved_ns[p] ? m.peer[p].heard_ns : m.max_moved_ns[p];
    return ms_since(now, t != 0 ? t : m.open_ns);
}

static bool loading(int p) {
    return (int64_t)have_of(p) < (int64_t)net_scene_first() + net.delay + MESH_LOAD_LEAD;
}

static void away_view(MeshAwayView* v, uint64_t now) {
    memset(v, 0, sizeof *v);
    v->humans = m.humans;
    v->settled = m.settled;
    v->self = m.self;
    v->away_ms = m.away_ms;
    v->load_ms = m.load_ms;
    v->stable_need_ms = MESH_STABLE_MS;
    for (int p = 0; p < MESH_PORTS; p++) {
        v->have[p] = have_of(p);
        for (int q = 0; q < MESH_PORTS; q++) {
            v->reports[q][p] = is_peer(q) ? m.peer[q].have[p] : -1;
        }
        if (!is_peer(p)) {
            continue;
        }
        v->bye[p] = m.peer[p].bye;
        v->quiet_ms[p] = quiet_ms(p, now);
        v->stable_ms[p] = ms_since(now, m.max_moved_ns[p] != 0 ? m.max_moved_ns[p] : m.open_ns);
        v->loading[p] = loading(p);
    }
}

static int leader_now(void) {
    return mesh_leader(m.humans, m.settled, m.silent, m.self);
}

/* ---- reliable lane ------------------------------------------------------- */

static MeshHdr hdr_to(uint8_t magic, int port) {
    MeshHdr h = {magic, MESH_VERSION, net.session, (uint8_t)m.self, (uint8_t)port};
    return h;
}

static void lane_put(int port, const MeshRelMsg* msg) {
    MeshRel r;
    r.h = hdr_to(MESH_REL, port);
    r.seq = msg->seq;
    r.type = msg->type;
    r.len = msg->len;
    memcpy(r.payload, msg->payload, msg->len);
    uint8_t out[MESH_DGRAM_MAX];
    const size_t n = mesh_rel_encode(&r, out, sizeof out);
    if (n > 0) {
        link_send(port, out, n);
    }
}

static bool lane_queue(int port, uint8_t type, const void* payload, int len) {
    MeshPeer* q = &m.peer[port];
    if (q->tx_n == MESH_REL_Q || len < 0 || len > MESH_REL_MAX) {
        return false;
    }
    MeshRelMsg* msg = &q->tx[(q->tx_head + q->tx_n++) % MESH_REL_Q];
    msg->seq = q->tx_next++;
    msg->type = type;
    msg->len = (uint16_t)len;
    if (len > 0) {
        memcpy(msg->payload, payload, (size_t)len);
    }
    lane_put(port, msg); /* now; the resend timer covers the rest */
    if (q->tx_sent_ns == 0) {
        q->tx_sent_ns = SDL_GetTicksNS();
    }
    return true;
}

/* Everything unacked, again, once the resend interval has gone by. A phone
 * settled away is still owed its AWAY: if it was only frozen, it has to find
 * that out within the grace its own freeze gives it (service), before it
 * starts judging the rest of us by a silence that was its own. */
static void lane_service(uint64_t now) {
    for (int p = 0; p < MESH_PORTS; p++) {
        MeshPeer* q = &m.peer[p];
        if (!is_peer(p) || q->bye || q->tx_n == 0 || now - q->tx_sent_ns < MESH_REL_RESEND_NS) {
            continue;
        }
        for (int i = 0; i < q->tx_n; i++) {
            lane_put(p, &q->tx[(q->tx_head + i) % MESH_REL_Q]);
        }
        q->tx_sent_ns = now;
    }
}

static void send_small(int port, uint8_t magic, uint16_t value) {
    const MeshHdr h = hdr_to(magic, port);
    uint8_t out[MESH_HDR_LEN + 2];
    const size_t n = mesh_small_encode(&h, value, out, sizeof out);
    if (n > 0) {
        link_send(port, out, n);
    }
}

static void lane_ack(MeshPeer* q, uint16_t next) {
    bool moved = false;
    while (q->tx_n > 0 && (int16_t)(q->tx[q->tx_head].seq - next) < 0) {
        q->tx_head = (q->tx_head + 1) % MESH_REL_Q;
        q->tx_n--;
        moved = true;
    }
    if (moved) {
        q->tx_sent_ns = q->tx_n > 0 ? SDL_GetTicksNS() : 0;
    }
}

/* ---- away ---------------------------------------------------------------- */

static void away_broadcast(int port, int32_t frame) {
    uint8_t msg[MESH_AWAY_LEN];
    mesh_away_encode(msg, (uint8_t)port, frame);
    for (int p = 0; p < MESH_PORTS; p++) {
        if (live(p) || p == port) {
            lane_queue(p, MESH_REL_AWAY, msg, sizeof msg); /* the settled one too, so it leaves */
        }
    }
}

/* Port p is settled at F here: from now on its input after F is neutral. */
static void settle_apply(int p, int32_t frame, int by) {
    if (!is_peer(p)) {
        return;
    }
    if (settled(p)) {
        if (m.settle[p] != frame && !m.warned_conflict) {
            m.warned_conflict = true;
            pc_log_line("net: port %d settled at frame %d by port %d, already at %d here", p + 1,
                frame, by + 1, m.settle[p]);
        }
        return;
    }
    m.settled |= (uint8_t)(1u << p);
    m.settle[p] = frame;
    net_slot_settle(m.peer[p].slot, frame);
    pc_log_line("net: port %d settled away at frame %d by port %d (frame %d here, %s)", p + 1,
        frame, by + 1, net.frame, m.peer[p].bye ? "it left" : "silent");
    if (by == m.self) {
        away_broadcast(p, frame);
    }
}

static void on_away(int from, const uint8_t* payload, int len) {
    uint8_t port;
    int32_t frame;
    if (!mesh_away_decode(payload, len, &port, &frame)) {
        return;
    }
    const int what = mesh_away_accept(m.settled, m.self, from, port);
    if (what == MESH_AWAY_EVICTED) {
        if (!m.evicted) {
            m.evicted = true;
            m.evicted_frame = frame;
            pc_log_line("net: settled away by port %d at frame %d (frame %d here); leaving",
                from + 1, frame, net.frame);
            net_peer_gone(PC_NET_PEER_LEFT);
        }
    } else if (what == MESH_AWAY_SETTLE) {
        settle_apply(port, frame, from);
    }
}

/* The newest frame of each port anyone is known to hold, and when it moved:
 * the sign of life relays and reports carry. */
static void track_max(uint64_t now) {
    for (int p = 0; p < MESH_PORTS; p++) {
        if (!is_peer(p)) {
            continue;
        }
        int32_t mx = have_of(p);
        for (int q = 0; q < MESH_PORTS; q++) {
            if (is_peer(q) && q != p && m.peer[q].have[p] > mx) {
                mx = m.peer[q].have[p];
            }
        }
        if (mx != m.last_max[p]) {
            m.last_max[p] = mx;
            m.max_moved_ns[p] = now;
        }
    }
}

static void service(uint64_t now) {
    if (m.last_service_ns != 0 && now - m.last_service_ns > MESH_FREEZE_NS) {
        /* This phone was the one stopped (a load, the tab hidden): everyone's
         * silence is ours, so it is measured again from here. */
        m.grace_until_ns = now + (uint64_t)m.away_ms * 1000000ull;
    }
    m.last_service_ns = now;
    track_max(now);
    lane_service(now);
    MeshAwayView v;
    away_view(&v, now);
    m.silent = mesh_silent_mask(&v);
    if (!m.armed || m.evicted || now < m.grace_until_ns) {
        return;
    }
    for (int i = 0; i < MESH_PORTS; i++) {
        int32_t frame;
        const int p = mesh_away_decide(&v, &frame);
        if (p < 0) {
            break;
        }
        settle_apply(p, frame, m.self);
        away_view(&v, now);
        m.silent = mesh_silent_mask(&v);
    }
}

/* ---- input --------------------------------------------------------------- */

static void section_add(MeshInput* pk, int port, int64_t first, int count, bool own) {
    MeshSection* s = &pk->sec[pk->nsec++];
    s->port = (uint8_t)port;
    s->first = (int32_t)first;
    s->count = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        const int32_t f = (int32_t)(first + i);
        const WirePad* w = own ? net_local_pad(f) : net_slot_real(m.peer[port].slot, f);
        memcpy(&s->pads[i], w, sizeof s->pads[i]);
    }
}

static void send_input_to(int port, uint64_t now) {
    MeshPeer* q = &m.peer[port];
    MeshInput pk;
    memset(&pk, 0, sizeof pk);
    pk.h = hdr_to(MESH_INPUT, port);
    pk.seq = q->tx_seq++;
    q->tx_seqs[pk.seq & 63] = pk.seq;
    q->tx_ns[pk.seq & 63] = now;
    if (q->rx_valid) {
        const uint64_t held = (now - q->rx_ns) / 100000ull;
        pk.flags = MESH_ECHO;
        pk.echo = q->rx_seq;
        pk.held = (uint16_t)(held > 65535 ? 65535 : held);
    }
    const int32_t adv = net_slot_adv(q->slot);
    pk.adv = (int8_t)(adv == INT32_MIN ? 0 : adv < -127 ? -127 : adv > 127 ? 127 : adv);
    for (int p = 0; p < MESH_PORTS; p++) {
        pk.have[p] = (m.humans & (1u << p)) ? have_of(p) : -1;
    }
    pk.ck_frame = net_confirmed();
    pk.ck = pk.ck_frame >= 0 ? net_ck_at(pk.ck_frame) : 0;

    /* Our own input from the first frame it lacks. */
    const int32_t wrote = net_local_wrote();
    int64_t first = (int64_t)q->have[m.self] + 1;
    if (first < 0) {
        first = 0;
    }
    if (first <= (int64_t)wrote - RING) {
        first = (int64_t)wrote - RING + 1;
    }
    int64_t count = (int64_t)wrote - first + 1;
    if (count > MESH_RED) {
        count = MESH_RED;
    }
    if (count > 0) {
        section_add(&pk, m.self, first, (int)count, true);
    }
    /* What it lacks of a third port that we hold. */
    for (int p = 0; p < MESH_PORTS && pk.nsec < MESH_SECTIONS; p++) {
        if (!is_peer(p) || p == port) {
            continue;
        }
        int32_t mine = have_of(p);
        if (settled(p) && mine > m.settle[p]) {
            mine = m.settle[p];
        }
        const int32_t theirs = q->have[p];
        if (!mesh_relay_wanted(mine, theirs, settled(p), quiet_ms(p, now))) {
            continue;
        }
        const size_t used = mesh_input_len(&pk) + MESH_SECTION_FIXED;
        if (used + sizeof(MeshPad) > MESH_DGRAM_MAX) {
            break;
        }
        int room = (int)((MESH_DGRAM_MAX - used) / sizeof(MeshPad));
        room = room > MESH_RED ? MESH_RED : room;
        int64_t rf = (int64_t)theirs + 1;
        const int64_t oldest = (int64_t)(mine > net.frame ? mine : net.frame) - RING + 1;
        rf = rf < 0 ? 0 : rf < oldest ? oldest : rf; /* what the ring still holds as real */
        int64_t rc = (int64_t)mine - rf + 1;
        rc = rc > room ? room : rc;
        if (rc > 0) {
            section_add(&pk, p, rf, (int)rc, false);
        }
    }
    uint8_t out[MESH_DGRAM_MAX];
    const size_t n = mesh_input_encode(&pk, out, sizeof out);
    if (n > 0) {
        link_send(
            port, out, n); /* not net.tx_inputs: nothing acks them, so it would read as loss */
    }
}

/* An M2 engine cannot read a mesh: tell it in its own words, until the
 * phone answers in ours. */
static void probe(uint64_t now) {
    for (int p = 0; p < MESH_PORTS; p++) {
        MeshPeer* q = &m.peer[p];
        if (!live(p) || q->heard_ns != 0 || (q->probe_ns != 0 && now - q->probe_ns < MESH_PROBE_NS))
        {
            continue;
        }
        q->probe_ns = now;
        for (uint8_t player = 0; player < 2; player++) {
            uint8_t out[MESH_M2_PROBE_LEN];
            mesh_m2_probe(out, net.session, player);
            link_send(p, out, sizeof out);
        }
    }
}

void mesh_send_inputs(void) {
    if (!m.open) {
        return;
    }
    const uint64_t now = SDL_GetTicksNS();
    for (int p = 0; p < MESH_PORTS; p++) {
        if (live(p)) {
            send_input_to(p, now);
        }
    }
    lane_service(now);
    probe(now);
    m.last_send_ns = now;
}

/* The phase sample of the phone this one runs furthest ahead of: slowing
 * down for it is what keeps every phone inside every other's window. */
static void phase_note(int port, int remote_adv, uint64_t now) {
    const int32_t local = net_slot_adv(m.peer[port].slot);
    if (local == INT32_MIN) {
        return;
    }
    m.peer[port].off = local - remote_adv;
    m.peer[port].off_ns = now;
    int worst = -1;
    for (int p = 0; p < MESH_PORTS; p++) {
        const MeshPeer* q = &m.peer[p];
        if (live(p) && q->off_ns != 0 && now - q->off_ns < MESH_PHASE_NS &&
            (worst < 0 || q->off > m.peer[worst].off))
        {
            worst = p;
        }
    }
    if (worst == port) {
        adv_note(remote_adv, local);
    }
}

static void rtt_note(int port, uint32_t us) {
    MeshPeer* q = &m.peer[port];
    q->rtt_us = q->rtt_us == 0 ? us : (q->rtt_us * 3 + us) / 4;
    uint32_t worst = 0;
    int worst_port = -1;
    for (int p = 0; p < MESH_PORTS; p++) {
        if (live(p) && m.peer[p].rtt_us > worst) {
            worst = m.peer[p].rtt_us;
            worst_port = p;
        }
    }
    net.ping_us = worst;
    if (worst_port == port) {
        jitter_note(us); /* one phone's |dRTT|: mixing two would read as jitter */
    }
}

static void on_input(int port, const MeshInput* pk, uint64_t at) {
    MeshPeer* q = &m.peer[port];
    const bool newer = !q->rx_valid || (int16_t)(pk->seq - q->rx_seq) > 0;
    if (newer) {
        q->rx_valid = true;
        q->rx_seq = pk->seq;
        q->rx_ns = at;
    }
    if (pk->flags & MESH_ECHO) {
        const int i = pk->echo & 63;
        if (q->tx_seqs[i] == pk->echo && q->tx_ns[i] != 0 && at > q->tx_ns[i]) {
            const uint64_t rtt = at - q->tx_ns[i];
            const uint64_t held = (uint64_t)pk->held * 100000ull;
            q->tx_ns[i] = 0;
            if (rtt > held && rtt - held < 5000000000ull) {
                rtt_note(port, (uint32_t)((rtt - held) / 1000ull));
            }
        }
    }
    for (int p = 0; p < MESH_PORTS; p++) {
        if (pk->have[p] > q->have[p]) {
            q->have[p] = pk->have[p];
        }
    }
    if (pk->ck_frame >= 0) {
        q->ck[pk->ck_frame & (RING - 1)].frame = pk->ck_frame;
        q->ck[pk->ck_frame & (RING - 1)].ck = pk->ck;
    }
    for (int i = 0; i < pk->nsec; i++) {
        const MeshSection* s = &pk->sec[i];
        if (!is_peer(s->port) || s->count == 0) {
            continue; /* our own, or nobody's */
        }
        WirePad pads[MESH_RED];
        memcpy(pads, s->pads, (size_t)s->count * sizeof pads[0]);
        net_slot_take(m.peer[s->port].slot, s->first, s->count, pads);
    }
    if (newer) {
        phase_note(port, pk->adv, at);
    }
}

static bool deliver(int from, const MeshRel* r) {
    switch (r->type) {
    case MESH_REL_AWAY:
        on_away(from, r->payload, r->len);
        return true;
    case REL_DELAY:
        net_delay_rel(r->payload, r->len);
        return true;
    case REL_SCENE:
        net_scene_rel_from(from, r->payload, r->len);
        return true;
    case REL_CHAT:
        return true;
    default:
        break;
    }
    if (r->type < 0x10) {
        return true; /* nothing else of ours */
    }
    if (m.call_n == MESH_REL_Q) {
        return false; /* the caller is not draining: unacked, resent later */
    }
    MeshCall* c = &m.call[(m.call_head + m.call_n++) % MESH_REL_Q];
    c->from = (uint8_t)from;
    c->type = r->type;
    c->len = r->len;
    memcpy(c->payload, r->payload, r->len);
    return true;
}

static void lane_rx(int from, const MeshRel* r) {
    MeshPeer* q = &m.peer[from];
    const int16_t d = (int16_t)(r->seq - q->rx_expect);
    if (d > 0) {
        return; /* ahead of a gap: the sender resends the lane in order */
    }
    if (d == 0) {
        if (!deliver(from, r)) {
            return;
        }
        q->rx_expect++;
    }
    send_small(from, MESH_ACK, q->rx_expect);
}

static void rx_one(int from, const uint8_t* d, size_t n, uint64_t at) {
    if (!is_peer(from) || n < 2) {
        m.rx_bad++;
        return;
    }
    MeshPeer* q = &m.peer[from];
    if (d[1] != MESH_VERSION) {
        /* Another engine build: an M2 one speaks protocol 9 (pc/net.h). */
        if (!q->incompatible) {
            q->incompatible = true;
            pc_log_line("net: port %d speaks protocol %u, the mesh speaks %u", from + 1, d[1],
                MESH_VERSION);
        }
        return;
    }
    MeshHdr h;
    if (!mesh_hdr_decode(d, n, &h) || h.from != from || h.to != m.self || h.session != net.session)
    {
        m.rx_bad++;
        if (!q->warned) {
            q->warned = true;
            pc_log_line("net: dropped a datagram from port %d (magic %02x, %u bytes)", from + 1,
                d[0], (unsigned)n);
        }
        return;
    }
    switch (h.magic) {
    case MESH_INPUT: {
        MeshInput pk;
        if (!mesh_input_decode(d, n, &pk)) {
            m.rx_bad++;
            return;
        }
        q->heard_ns = at;
        on_input(from, &pk, at);
        break;
    }
    case MESH_REL: {
        MeshRel r;
        if (!mesh_rel_decode(d, n, &r)) {
            m.rx_bad++;
            return;
        }
        q->heard_ns = at;
        lane_rx(from, &r);
        break;
    }
    case MESH_ACK:
    case MESH_BYE: {
        uint16_t value;
        if (!mesh_small_decode(d, n, &h, &value)) {
            m.rx_bad++;
            return;
        }
        q->heard_ns = at;
        if (h.magic == MESH_ACK) {
            lane_ack(q, value);
        } else if (!q->bye) {
            q->bye = true;
            pc_log_line("net: port %d left (reason %u) at frame %d", from + 1, value, net.frame);
        }
        break;
    }
    default:
        m.rx_bad++;
        break;
    }
}

/* Off the page's ring into the queue, stamped now. */
static void drain_link(uint64_t now) {
    while (m.q_n < MESH_RXQ) {
        uint8_t buf[MESH_SLOT_BYTES];
        const int n = link_recv(buf, sizeof buf);
        if (n < 0) {
            break;
        }
        uint8_t from;
        const uint8_t* d;
        size_t len;
        if (!mesh_addr_unwrap(buf, (size_t)n, &from, &d, &len) || len > MESH_DGRAM_MAX) {
            m.rx_bad++;
            continue;
        }
        MeshRx* r = &m.q[(m.q_head + m.q_n++) % MESH_RXQ];
        r->ns = now;
        r->from = from;
        r->len = (uint16_t)len;
        memcpy(r->data, d, len);
    }
}

void mesh_rx_now(void) {
    if (!m.open) {
        return;
    }
    const uint64_t now = SDL_GetTicksNS();
    drain_link(now);
    if (now - m.last_send_ns >= MESH_KEEPALIVE_NS) {
        mesh_send_inputs(); /* the game thread is busy (a load): still answer */
    }
}

void mesh_recv(void) {
    if (!m.open) {
        return;
    }
    const uint64_t now = SDL_GetTicksNS();
    drain_link(now);
    while (m.q_n > 0) {
        const MeshRx* r = &m.q[m.q_head];
        rx_one(r->from, r->data, r->len, r->ns);
        m.q_head = (m.q_head + 1) % MESH_RXQ;
        m.q_n--;
    }
    service(now);
}

/* ---- the rest of net.c's session ----------------------------------------- */

void mesh_open(uint8_t humans, int local_port) {
    memset(&m, 0, sizeof m);
    m.humans = humans;
    m.self = local_port;
    m.away_ms = s_away_ms;
    m.load_ms = s_load_ms;
    m.open_ns = SDL_GetTicksNS();
    int slot = 0;
    for (int p = 0; p < MESH_PORTS; p++) {
        MeshPeer* q = &m.peer[p];
        for (int i = 0; i < MESH_PORTS; i++) {
            q->have[i] = -1;
        }
        for (int i = 0; i < RING; i++) {
            q->ck[i].frame = -1;
        }
        q->ck_checked = -1;
        m.settle[p] = -2;
        m.last_max[p] = -1;
        if (is_peer(p)) {
            q->slot = slot++; /* net_slots_setup's order: ascending ports */
        }
    }
    m.open = true;
}

void mesh_check_desync(void) {
    const int32_t upto = net_confirmed();
    for (int p = 0; p < MESH_PORTS; p++) {
        MeshPeer* q = &m.peer[p];
        if (!live(p)) {
            continue;
        }
        int32_t from = q->ck_checked + 1 > net.ck_from ? q->ck_checked + 1 : net.ck_from;
        if (from <= net.frame - RING) {
            from = net.frame - RING + 1;
        }
        for (int32_t f = from; f <= upto; f++) {
            const int i = f & (RING - 1);
            if (q->ck[i].frame != f) {
                continue;
            }
            q->ck_checked = f;
            if (net_ck_at(f) != q->ck[i].ck) {
                net_desync_found(f, net_ck_at(f), q->ck[i].ck, p);
                return;
            }
        }
    }
}

void mesh_bye(uint8_t reason) {
    for (int p = 0; p < MESH_PORTS; p++) {
        if (is_peer(p) && !m.peer[p].bye) {
            for (int i = 0; i < 3; i++) {
                send_small(p, MESH_BYE, reason);
            }
        }
    }
}

bool mesh_announcer(void) {
    return leader_now() == m.self;
}

uint8_t mesh_scene_peers(void) {
    uint8_t peers = 0;
    for (int p = 0; p < MESH_PORTS; p++) {
        if (live(p)) {
            peers |= (uint8_t)(1u << p);
        }
    }
    return peers;
}

int mesh_quiet_ms(void) {
    const uint64_t now = SDL_GetTicksNS();
    int worst = 0;
    for (int p = 0; p < MESH_PORTS; p++) {
        if (!live(p)) {
            continue;
        }
        if (m.peer[p].heard_ns == 0) {
            return -1;
        }
        const int32_t q = ms_since(now, m.peer[p].heard_ns);
        worst = q > worst ? q : worst;
    }
    return worst;
}

bool mesh_send_reliable(uint8_t type, const void* payload, int len) {
    if (len < 0 || len > MESH_REL_MAX) {
        return false;
    }
    for (int p = 0; p < MESH_PORTS; p++) {
        if (live(p) && m.peer[p].tx_n == MESH_REL_Q) {
            return false; /* all or none, so a retry reaches nobody twice */
        }
    }
    for (int p = 0; p < MESH_PORTS; p++) {
        if (live(p)) {
            lane_queue(p, type, payload, len);
        }
    }
    return true;
}

int mesh_recv_reliable(int* from, uint8_t* type, void* payload, int max) {
    if (m.call_n == 0 || from == NULL || type == NULL || payload == NULL || max < 0) {
        return -1;
    }
    const MeshCall* c = &m.call[m.call_head];
    const int n = c->len > max ? max : c->len;
    *from = c->from;
    *type = c->type;
    memcpy(payload, c->payload, (size_t)n);
    m.call_head = (m.call_head + 1) % MESH_REL_Q;
    m.call_n--;
    return n;
}

/* ---- public (pc/net.h) ------------------------------------------------- */

void pc_net_mesh_set_timing(int away_ms, int load_ms) {
    s_away_ms = away_ms > 0 ? away_ms : MESH_AWAY_MS;
    s_load_ms = load_ms > 0 ? load_ms : MESH_LOAD_MS;
}

void pc_net_mesh_arm(void) {
    if (m.open && !m.armed) {
        m.armed = true;
        m.grace_until_ns = 0;
    }
}

int pc_net_mesh_link(int port) {
    if (port < 0 || port >= MESH_PORTS || !m.open || (m.humans & (1u << port)) == 0) {
        return PC_NET_MESH_NONE;
    }
    if (port == m.self) {
        return PC_NET_MESH_SELF;
    }
    const MeshPeer* q = &m.peer[port];
    return q->incompatible         ? PC_NET_MESH_INCOMPATIBLE :
           settled(port)           ? PC_NET_MESH_AWAY :
           q->heard_ns == 0        ? PC_NET_MESH_WAITING :
           q->bye                  ? PC_NET_MESH_SILENT :
           (m.silent >> port) & 1u ? PC_NET_MESH_SILENT :
                                     PC_NET_MESH_LIVE;
}

int pc_net_mesh_ping_ms(int port) {
    if (!is_peer(port) || m.peer[port].rtt_us == 0) {
        return -1;
    }
    return (int)(m.peer[port].rtt_us / 1000);
}

bool pc_net_mesh_away(int port, int32_t* frame) {
    if (!m.open || port < 0 || port >= MESH_PORTS) {
        return false;
    }
    if (port == m.self) {
        if (m.evicted && frame != NULL) {
            *frame = m.evicted_frame;
        }
        return m.evicted;
    }
    if (settled(port) && frame != NULL) {
        *frame = m.settle[port];
    }
    return settled(port);
}

int pc_net_mesh_leader(void) {
    return m.open ? leader_now() : -1;
}

uint8_t pc_net_mesh_live(void) {
    if (!m.open) {
        return 0;
    }
    uint8_t mask = m.evicted ? 0 : (uint8_t)(1u << m.self);
    return (uint8_t)(mask | mesh_scene_peers());
}

void pc_net_mesh_gone(int port) {
    if (m.open && is_peer(port) && !m.peer[port].bye) {
        m.peer[port].bye = true; /* as its BYE: settled once its last frames stop moving */
        pc_log_line("net: the link to port %d failed at frame %d", port + 1, net.frame);
    }
}

int pc_net_mesh_recv(int* from, uint8_t* type, void* payload, int max) {
    return mesh_recv_reliable(from, type, payload, max);
}

bool pc_net_mesh_send_to(int port, uint8_t type, const void* payload, int len) {
    return net.active && net.mesh && live(port) && lane_queue(port, type, payload, len);
}

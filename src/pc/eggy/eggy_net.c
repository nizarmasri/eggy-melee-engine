/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * MELEE_EGGY netplay on the game thread (src/pc/eggy/eggy_net.h). The page
 * has already paired the match's phones and opened its links, so the session
 * is a mesh over those links (src/pc/net_mesh.c) and runs melee-web's
 * rollback session unchanged from there: rollback over every remote port,
 * time sync, desync checks.
 *
 * Nothing is simulated until the match. The mesh connects from the park and
 * every phone sends every other its HELLO (the config's hash): the match is
 * agreed when every phone has every other's and they all match, with no
 * RULES/READY handshake, since the config is the whole match and the seed and
 * first frame are in it. Then the VS scene loads and the hold (eggy_hold.c)
 * waits for every phone, so the scene's first tick is session frame 0 on all
 * of them and every frame of the match is checksum-compared.
 *
 * From the hold on, a phone that goes silent is settled away by the others
 * (the mesh's rule), who report it ('away') and play on. A phone the others
 * settled leaves; one left with no other phone ends the match where it
 * stands, as M2's last phone did.
 */
#include "pc/compat.h"
#include "pc/eggy/eggy_net.h"
#include "pc/net.h"
#include "pc/net_internal.h"
#include "pc/net_lan.h"
#include "pc/pc.h"

#include <SDL3/SDL_timer.h>
#include <emscripten.h>
#include <string.h>

/* The page's link states (platforms/browser/bundle/netplay.mjs LINK). */
#define LINK_OPEN 4
#define LINK_FAILED (-1)

/* The page's links must all be open by then. */
#define EGGY_LINK_MS 20000
/* Connected, for every other phone to answer (its page may call eggy_load a
 * little later) and then for every HELLO. */
#define EGGY_CONNECT_MS 30000
/* A phone silent this long mid-match is settled away. */
#define EGGY_AWAY_MS 1500
/* The other phones' loads, from ours: a cold phone compiling shaders is slow.
 * A phone still loading a scene is also given this long before it is
 * settled away. */
#define EGGY_LOADED_MS 120000
/* How long a finished phone waits for the others' DONE. */
#define EGGY_CLOSE_MS 5000
/* A refused match is reported by every phone: the refusal waits this long
 * after our HELLO went out, so the others have it before our BYE. */
#define EGGY_REFUSE_MS 600

// clang-format off
EM_JS(int, eggy_link_status, (int port), {
  const n = Module['meleeNetplay'];
  try {
    return n && typeof n.status === 'function' ? n.status(port) | 0 : -1;
  } catch (e) {
    return -1;
  }
});
// clang-format on

static struct {
    int state; /* enum EggyNetState */
    EggyConfig cfg;
    uint8_t others;    /* the other phones' ports */
    uint64_t t0;       /* when the current phase began, SDL ms */
    bool heard;        /* every other phone's session has answered */
    bool hello_sent;   /* our HELLO is queued */
    uint8_t hello_got; /* ports whose HELLO arrived and is our match */
    int refuse;        /* EGGY_NET_ERR_* waiting out EGGY_REFUSE_MS, 0 none */
    uint64_t refuse_at;
    EggyHold hold;
    bool hold_begun;
    uint64_t hold_t0;
    bool done_sent;
    uint8_t done_got;
    int32_t desync_told; /* the desync frame last reported, -1 none */
    int desyncs;         /* reported this match */
    uint8_t away_told;   /* ports whose 'away' was reported */
    int32_t stats[EGGY_NET_STATS_WORDS];
} s;

static uint64_t now_ms(void) {
    return SDL_GetTicks();
}

static void stats_refresh(void) {
    PcNetCounters c;
    if (pc_net_counters(&c)) {
        s.stats[1] = c.ping_ms;
        s.stats[2] = c.delay;
        s.stats[3] = (int32_t)c.rollbacks;
        s.stats[4] = c.rollback_depth_max;
        s.stats[5] = s.desyncs;
        s.stats[6] = (int32_t)c.stalls;
        s.stats[7] = c.quality;
    }
    s.stats[0] = s.state;
    for (int port = 0; port < EGGY_PORTS; port++) {
        s.stats[8 + port] = pc_net_mesh_link(port);
        s.stats[12 + port] = pc_net_mesh_ping_ms(port);
    }
    int state, count;
    int32_t last;
    pc_net_resync_counts(&state, &count, &last);
    s.stats[16] = state;
    s.stats[17] = count;
    s.stats[18] = last;
}

static void set_state(int state) {
    s.state = state;
    s.stats[0] = state;
}

/* Why the session ended, as a net-error. */
static int peer_error(void) {
    for (int port = 0; port < EGGY_PORTS; port++) {
        if (pc_net_mesh_link(port) == PC_NET_MESH_INCOMPATIBLE) {
            return EGGY_NET_ERR_INCOMPATIBLE;
        }
    }
    return pc_net_peer_status() == PC_NET_PEER_INCOMPATIBLE ? EGGY_NET_ERR_INCOMPATIBLE :
           pc_net_peer_status() == PC_NET_PEER_TIMEOUT      ? EGGY_NET_ERR_TIMEOUT :
                                                              EGGY_NET_ERR_PEER_LEFT;
}

static int fail(int code) {
    if (pc_net_active()) {
        stats_refresh();
        pc_net_disconnect();
    }
    set_state(EGGY_NET_FAILED);
    pc_log_line("eggy: netplay failed, error %d", code);
    return -code;
}

/* The other phones still in. */
static uint8_t others_live(void) {
    return (uint8_t)(pc_net_mesh_live() & s.others);
}

/* A link the page reports failed is that phone gone: no 1.5 s (or, while
 * it loads, 120 s) of silence to wait out first. */
static void links_check(void) {
    for (int port = 0; port < EGGY_PORTS; port++) {
        if ((s.others & (1u << port)) && eggy_link_status(port) == LINK_FAILED) {
            pc_net_mesh_gone(port);
        }
    }
}

static uint8_t away_mask(void) {
    uint8_t away = 0;
    for (int port = 0; port < EGGY_PORTS; port++) {
        if ((s.others & (1u << port)) && pc_net_mesh_away(port, NULL)) {
            away |= (uint8_t)(1u << port);
        }
    }
    return away;
}

/* What the other phones sent on the caller lane. Drained every frame and
 * every tick the session is up: each lane is ordered, so one message left
 * unread would hold back everything that phone sends after it. */
static void drain(void) {
    int from;
    uint8_t type;
    uint8_t buf[256];
    int n;
    while ((n = pc_net_mesh_recv(&from, &type, buf, sizeof buf)) >= 0) {
        if (from < 0 || from >= EGGY_PORTS || (s.others & (1u << from)) == 0) {
            continue;
        }
        switch (type) {
        case EGGY_REL_HELLO:
            if (eggy_hello_matches(buf, n, s.cfg.hash, eggy_net_session_id(&s.cfg))) {
                s.hello_got |= (uint8_t)(1u << from);
            } else if (s.refuse == 0) {
                pc_log_line(
                    "eggy: port %d's match differs (our config hash %08x)", from + 1, s.cfg.hash);
                s.refuse = EGGY_NET_ERR_MISMATCH;
                s.refuse_at = now_ms() + EGGY_REFUSE_MS;
            }
            break;
        case EGGY_REL_DONE:
            s.done_got |= (uint8_t)(1u << from);
            break;
        default:
            eggy_hold_receive(&s.hold, type, from);
            break;
        }
    }
}

void eggy_net_begin(const EggyConfig* c) {
    memset(&s, 0, sizeof s);
    s.cfg = *c;
    s.others = (uint8_t)(c->humans & ~(1u << c->local_port));
    s.t0 = now_ms();
    eggy_hold_init(&s.hold, c->humans, c->local_port);
    for (int port = 0; port < EGGY_PORTS; port++) {
        s.stats[12 + port] = -1;
    }
    s.stats[18] = -1;
    s.desync_told = -1;
    set_state(EGGY_NET_LINK);
    pc_log_line("eggy: netplay on port %d of ports %x (%d phones, leader %d), session %08x, "
                "delay %d",
        c->local_port, c->humans, c->phones, c->leader_port, eggy_net_session_id(c),
        c->input_delay);
}

int eggy_net_connect_step(void) {
    const uint64_t now = now_ms();
    if (s.state == EGGY_NET_LINK) {
        bool open = true;
        for (int port = 0; port < EGGY_PORTS; port++) {
            if ((s.others & (1u << port)) == 0) {
                continue;
            }
            const int link = eggy_link_status(port);
            if (link == LINK_FAILED) {
                return fail(EGGY_NET_ERR_LINK);
            }
            open = open && link == LINK_OPEN;
        }
        if (!open) {
            return now - s.t0 > EGGY_LINK_MS ? fail(EGGY_NET_ERR_LINK) : 0;
        }
        pc_net_set_input_delay(s.cfg.input_delay > 0 ? s.cfg.input_delay : -1);
        pc_net_mesh_set_timing(EGGY_AWAY_MS, EGGY_LOADED_MS);
        if (!pc_net_mesh_connect(s.cfg.humans, s.cfg.local_port, eggy_net_session_id(&s.cfg),
                eggy_net_seed(&s.cfg), 0))
        {
            return fail(EGGY_NET_ERR_CONNECT);
        }
        set_state(EGGY_NET_HANDSHAKE);
        s.t0 = now;
        return 0;
    }
    if (s.state != EGGY_NET_HANDSHAKE) {
        return fail(EGGY_NET_ERR_CONNECT);
    }
    pc_net_poll_parked();
    if (!pc_net_active()) {
        return fail(s.refuse != 0 ? s.refuse : peer_error());
    }
    drain();
    for (int port = 0; port < EGGY_PORTS; port++) {
        if (pc_net_mesh_link(port) == PC_NET_MESH_INCOMPATIBLE) {
            return fail(EGGY_NET_ERR_INCOMPATIBLE); /* it refuses ours too (the M2 probe) */
        }
    }
    if (!s.hello_sent) {
        uint8_t hello[EGGY_HELLO_LEN];
        eggy_hello_encode(hello, s.cfg.hash, eggy_net_session_id(&s.cfg));
        s.hello_sent = pc_net_send_reliable(EGGY_REL_HELLO, hello, sizeof hello);
    }
    if (!s.heard) {
        /* The clock below runs from the moment every phone has answered. */
        bool all = true;
        for (int port = 0; port < EGGY_PORTS; port++) {
            if ((s.others & (1u << port)) && pc_net_mesh_link(port) == PC_NET_MESH_WAITING) {
                all = false;
            }
        }
        if (!all) {
            return now - s.t0 > EGGY_CONNECT_MS ? fail(EGGY_NET_ERR_TIMEOUT) : 0;
        }
        s.heard = true;
        s.t0 = now;
    }
    if (s.refuse != 0) {
        /* Ours still goes out first, so the others refuse too. */
        return now >= s.refuse_at && s.hello_sent   ? fail(s.refuse) :
               now >= s.refuse_at + EGGY_CONNECT_MS ? fail(s.refuse) :
                                                      0;
    }
    for (int port = 0; port < EGGY_PORTS; port++) {
        if ((s.others & (1u << port)) && pc_net_mesh_link(port) == PC_NET_MESH_SILENT) {
            return fail(EGGY_NET_ERR_PEER_LEFT); /* it left before the match was agreed */
        }
    }
    if (s.hello_sent && s.hello_got == s.others) {
        set_state(EGGY_NET_HOLD);
        stats_refresh();
        pc_log_line("eggy: netplay session up with ports %x, ping %d ms", s.others, s.stats[1]);
        return 1;
    }
    if (now - s.t0 > EGGY_CONNECT_MS) {
        return fail(EGGY_NET_ERR_HANDSHAKE);
    }
    return 0;
}

static bool send_msg(uint8_t type) {
    return pc_net_send_reliable(type, NULL, 0);
}

int eggy_net_hold_step(bool go, bool* loaded) {
    const uint64_t now = now_ms();
    if (!s.hold_begun) {
        s.hold_begun = true;
        s.hold_t0 = now;
        pc_net_mesh_arm(); /* a phone silent from here on is settled away */
    }
    links_check();
    pc_net_poll_parked();
    if (!pc_net_active()) {
        return fail(peer_error());
    }
    drain();
    stats_refresh();
    if (others_live() == 0) {
        return fail(EGGY_NET_ERR_PEER_LEFT); /* every other phone is gone before frame 0 */
    }
    for (;;) {
        const int action = eggy_hold_next(&s.hold, go, away_mask(), pc_net_mesh_leader());
        switch (action) {
        case EGGY_HOLD_SEND_LOADED:
            if (!send_msg(EGGY_REL_LOADED)) {
                return 0;
            }
            break;
        case EGGY_HOLD_REPORT:
            *loaded = true;
            break;
        case EGGY_HOLD_SEND_ARMED:
            if (!send_msg(EGGY_REL_ARMED)) {
                return 0;
            }
            break;
        case EGGY_HOLD_SEND_GO:
            if (!send_msg(EGGY_REL_GO)) {
                return 0;
            }
            break;
        case EGGY_HOLD_RELEASE:
            set_state(EGGY_NET_LIVE);
            pc_log_line("eggy: netplay released, ping %d ms, delay %d", s.stats[1], s.stats[2]);
            return 1;
        default:
            if (!s.hold.reported && now - s.hold_t0 > EGGY_LOADED_MS) {
                return fail(EGGY_NET_ERR_TIMEOUT);
            }
            return 0;
        }
        eggy_hold_done(&s.hold, action);
    }
}

int eggy_net_tick(int32_t* frame) {
    if (s.state != EGGY_NET_LIVE) {
        return 0;
    }
    if (!pc_net_active()) {
        set_state(EGGY_NET_FAILED);
        return -1;
    }
    links_check();
    drain();
    stats_refresh();
    PcNetCounters c;
    /* Once per desync: a resync clears it, and a new one is reported again. */
    if (pc_net_counters(&c) && c.desync_frame >= 0 && c.desync_frame != s.desync_told) {
        s.desync_told = c.desync_frame;
        s.desyncs++;
        *frame = c.desync_frame;
        return 1;
    }
    return others_live() == 0 ? -1 : 0;
}

int eggy_net_next_away(int32_t* frame) {
    for (int port = 0; port < EGGY_PORTS; port++) {
        const uint8_t bit = (uint8_t)(1u << port);
        if ((s.cfg.humans & bit) && !(s.away_told & bit) && pc_net_mesh_away(port, frame)) {
            s.away_told |= bit;
            return port;
        }
    }
    return -1;
}

bool eggy_net_evicted(void) {
    return s.cfg.netplay && pc_net_mesh_away(s.cfg.local_port, NULL);
}

bool eggy_net_close_step(void) {
    if (!pc_net_active()) {
        if (s.state != EGGY_NET_FAILED && s.state != EGGY_NET_NONE) {
            set_state(EGGY_NET_CLOSED);
        }
        return true;
    }
    const uint64_t now = now_ms();
    if (s.state != EGGY_NET_CLOSING) {
        set_state(EGGY_NET_CLOSING);
        s.t0 = now;
    }
    if (!s.done_sent) {
        s.done_sent = send_msg(EGGY_REL_DONE);
    }
    /* Polling also resends whatever inputs the others lack, which they may
     * still need to finish the scene's last frames. */
    pc_net_poll_parked();
    if (!pc_net_active()) {
        set_state(EGGY_NET_CLOSED);
        return true;
    }
    drain();
    stats_refresh();
    const uint8_t live = others_live();
    if ((s.done_sent && (s.done_got & live) == live) || now - s.t0 > EGGY_CLOSE_MS) {
        pc_net_disconnect();
        set_state(EGGY_NET_CLOSED);
        return true;
    }
    return false;
}

void eggy_net_drop(bool failed) {
    if (pc_net_active()) {
        stats_refresh();
        pc_net_disconnect();
    }
    if (failed) {
        set_state(EGGY_NET_FAILED);
    } else if (s.state != EGGY_NET_NONE && s.state != EGGY_NET_FAILED) {
        set_state(EGGY_NET_CLOSED);
    }
}

bool eggy_net_open(void) {
    return pc_net_active();
}

const int32_t* eggy_net_stats_words(void) {
    if (s.state == EGGY_NET_LIVE || s.state == EGGY_NET_HOLD || s.state == EGGY_NET_CLOSING) {
        stats_refresh();
    }
    return s.stats;
}

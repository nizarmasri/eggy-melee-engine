/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * MELEE_EGGY netplay on the game thread (src/pc/eggy/eggy_net.h). This is
 * net_match_web.c's CONNECT phase without its matchmaking: the page has
 * already paired the two phones and opened its link, so the session is
 * handed that link (net_web.c) and runs the same steps -- connect, the
 * RULES/READY handshake, the ready barrier -- and from there everything is
 * melee-web's session unchanged: rollback, time sync, resume, desync checks.
 *
 * What differs is where the steps run. melee-web runs them in its lobby
 * scene, which ticks in lockstep from connect on, so the match starts on
 * whatever frame the lobby reached. Here nothing is simulated until the
 * match: the handshake runs from the park and the hold (no tick, the game
 * thread yielding every frame), names session frame 0 as the start, and the
 * VS scene's first tick is frame 0 on both phones. Every frame of the match
 * is therefore checksum-compared, and a phone that loads slower only makes
 * the other wait at the first tick.
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

/* The page's link must be open by then. */
#define EGGY_LINK_MS 20000
/* Connected, for the other phone's session to answer (its page may call
 * eggy_load a little later); then the handshake's own 15 s, and as long
 * again for the barrier. */
#define EGGY_CONNECT_MS 30000
/* Phones: iOS suspends a tab in the background, so a peer gets this long to
 * come back -- silent past the stall timeout, then resumed within the window
 * (net.c's reconnect phase). */
#define EGGY_STALL_MS 15000
#define EGGY_RECONNECT_MS 15000
/* The other phone's load, from ours: a cold phone compiling shaders is slow. */
#define EGGY_LOADED_MS 120000
/* How long a finished phone waits for the other's DONE. */
#define EGGY_CLOSE_MS 5000
/* A refused match is reported by both phones: the refusal waits this long
 * after our HELLO went out, so the other phone has it before our BYE. */
#define EGGY_REFUSE_MS 600

// clang-format off
EM_JS(int, eggy_link_status, (void), {
  const n = Module['meleeNetplay'];
  try {
    return n && typeof n.status === 'function' ? n.status() | 0 : -1;
  } catch (e) {
    return -1;
  }
});
// clang-format on

static struct {
    int state; /* enum EggyNetState */
    EggyConfig cfg;
    bool leader;
    uint64_t t0;     /* when the current phase began, SDL ms */
    bool heard;      /* the other phone's session has answered */
    bool hello_sent; /* our HELLO is queued */
    bool hello_got;  /* theirs arrived and is our match */
    int refuse;      /* EGGY_NET_ERR_* waiting out EGGY_REFUSE_MS, 0 none */
    uint64_t refuse_at;
    EggyHold hold;
    bool hold_begun;
    uint64_t hold_t0;
    bool done_sent, peer_done;
    bool desync_seen;
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
        s.stats[5] = c.desync_frame >= 0 ? 1 : 0;
        s.stats[6] = (int32_t)c.stalls;
        s.stats[7] = c.quality;
    }
    s.stats[0] = s.state;
}

static void set_state(int state) {
    s.state = state;
    s.stats[0] = state;
}

/* Why the session ended, as a net-error. */
static int peer_error(void) {
    return pc_net_peer_status() == PC_NET_PEER_INCOMPATIBLE ? EGGY_NET_ERR_INCOMPATIBLE :
                                                              EGGY_NET_ERR_PEER_LEFT;
}

static int fail(int code) {
    if (pc_net_active()) {
        pc_net_disconnect();
    }
    set_state(EGGY_NET_FAILED);
    pc_log_line("eggy: netplay failed, error %d", code);
    return -code;
}

/* What the other phone sent on the caller lane. Drained every frame and
 * every tick the session is up: the lane is stop-and-wait, so one message
 * left unread would hold back everything the peer sends after it. */
static void drain(void) {
    uint8_t type;
    uint8_t buf[256];
    int n;
    while ((n = pc_net_recv_reliable(&type, buf, sizeof buf)) >= 0) {
        switch (type) {
        case EGGY_REL_HELLO:
            if (eggy_hello_matches(buf, n, s.cfg.hash, eggy_net_session_id(&s.cfg))) {
                s.hello_got = true;
            } else if (s.refuse == 0) {
                pc_log_line("eggy: the other phone's match differs (config hash %08x)", s.cfg.hash);
                s.refuse = EGGY_NET_ERR_MISMATCH;
                s.refuse_at = now_ms() + EGGY_REFUSE_MS;
            }
            break;
        case EGGY_REL_DONE:
            s.peer_done = true;
            break;
        default:
            eggy_hold_receive(&s.hold, type);
            break;
        }
    }
}

void eggy_net_begin(const EggyConfig* c) {
    memset(&s, 0, sizeof s);
    s.cfg = *c;
    s.leader = c->local_port == c->leader_port;
    s.t0 = now_ms();
    eggy_hold_init(&s.hold, s.leader);
    set_state(EGGY_NET_LINK);
    pc_log_line("eggy: netplay %s on port %d with port %d, session %08x, delay %d",
        s.leader ? "leader" : "guest", c->local_port, s.leader ? c->guest_port : c->leader_port,
        eggy_net_session_id(c), c->input_delay);
}

int eggy_net_connect_step(void) {
    const uint64_t now = now_ms();
    if (s.state == EGGY_NET_LINK) {
        const int link = eggy_link_status();
        if (link == LINK_FAILED || (link != LINK_OPEN && now - s.t0 > EGGY_LINK_MS)) {
            return fail(EGGY_NET_ERR_LINK);
        }
        if (link != LINK_OPEN) {
            return 0;
        }
        /* net_match_web.c's connect, with the match's own seating: the leader
         * is player 0 (host), the guest player 1, each on its config port. */
        pc_net_set_input_delay(s.cfg.input_delay > 0 ? s.cfg.input_delay : -1);
        pc_net_set_ports(s.cfg.leader_port, s.cfg.guest_port);
        pc_net_set_session_id(eggy_net_session_id(&s.cfg));
        pc_net_set_timeouts(EGGY_STALL_MS, EGGY_RECONNECT_MS);
        pc_net_set_match_start(0);
        if (!pc_net_connect_socket(
                NET_WEB_SOCK, "web", 1, s.leader ? 0 : 1, s.leader ? eggy_net_seed(&s.cfg) : 0))
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
    if (!s.heard) {
        /* The handshake's 15 s run from its first call, so it is not called
         * before the other phone is there to answer. */
        if (pc_net_quiet_ms() < 0) {
            return now - s.t0 > EGGY_CONNECT_MS ? fail(EGGY_NET_ERR_TIMEOUT) : 0;
        }
        s.heard = true;
        s.t0 = now;
    }
    int32_t start = -1;
    uint32_t seed;
    const bool agreed = s.leader ? pc_net_host_match(eggy_net_seed(&s.cfg), &start) :
                                   pc_net_guest_wait_match(&seed, &start);
    if (pc_net_handshake_state() == HS_FAILED) {
        return fail(s.refuse != 0 ? s.refuse : EGGY_NET_ERR_HANDSHAKE);
    }
    if (agreed && !s.hello_sent) {
        uint8_t hello[EGGY_HELLO_LEN];
        eggy_hello_encode(hello, s.cfg.hash, eggy_net_session_id(&s.cfg));
        s.hello_sent = pc_net_send_reliable(EGGY_REL_HELLO, hello, sizeof hello);
    }
    if (s.refuse != 0) {
        /* Ours still goes out first, so the other phone refuses too. */
        return now >= s.refuse_at && s.hello_sent   ? fail(s.refuse) :
               now >= s.refuse_at + EGGY_CONNECT_MS ? fail(s.refuse) :
                                                      0;
    }
    if (s.hello_sent && s.hello_got) {
        set_state(EGGY_NET_HOLD);
        stats_refresh();
        pc_log_line("eggy: netplay session up, start frame %d, ping %d ms", start, s.stats[1]);
        return 1;
    }
    if (now - s.t0 > EGGY_CONNECT_MS) {
        return fail(agreed ? EGGY_NET_ERR_TIMEOUT : EGGY_NET_ERR_HANDSHAKE);
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
    }
    pc_net_poll_parked();
    if (!pc_net_active()) {
        return fail(peer_error());
    }
    drain();
    stats_refresh();
    /* Neither side ticks while it holds, so no wait_remote() is watching the
     * link: a peer that is gone past the stall and resume windows is called
     * gone here. */
    if (pc_net_quiet_ms() > EGGY_STALL_MS + EGGY_RECONNECT_MS) {
        return fail(EGGY_NET_ERR_PEER_LEFT);
    }
    for (;;) {
        const int action = eggy_hold_next(&s.hold, go);
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
    drain();
    stats_refresh();
    PcNetCounters c;
    if (!s.desync_seen && pc_net_counters(&c) && c.desync_frame >= 0) {
        s.desync_seen = true;
        *frame = c.desync_frame;
        return 1;
    }
    return 0;
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
    /* Polling also resends whatever inputs the other phone has not acked,
     * which it may still need to finish the scene's last frames. */
    pc_net_poll_parked();
    if (!pc_net_active()) {
        set_state(EGGY_NET_CLOSED);
        return true;
    }
    drain();
    stats_refresh();
    if ((s.done_sent && s.peer_done) || now - s.t0 > EGGY_CLOSE_MS) {
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

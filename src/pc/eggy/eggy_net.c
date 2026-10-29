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
 *
 * A phone whose page reloads before the hold releases comes back: the page
 * tells us when a link's epoch changes (eggy_net_peer_reset), the lane to it
 * starts over on both ends, and it is waited for again from its HELLO. Once
 * the match is under way nothing is resumed yet.
 */
#include "pc/compat.h"
#include "pc/eggy/eggy_net.h"
#include "pc/net.h"
#include "pc/net_internal.h"
#include "pc/net_lan.h"
#include "pc/pc.h"

#include <sysdolphin/baselib/random.h>

#include <SDL3/SDL_timer.h>
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

/* net_mesh.c, for eggy_net_peer_reset: only this file calls them. */
extern int pc_net_mesh_peer_reset(int port);
extern void pc_net_mesh_resend_away(int port);

_Static_assert(
    MAX_REGIONS <= EGGY_HEAP_LAYOUT_REGIONS, "eggy_heap_layout has a word for every region");

/* The page's link states (platforms/browser/bundle/netplay.mjs LINK). */
#define LINK_OPEN 4
#define LINK_FAILED (-1)

/* The page's links must all be open by then. */
#define EGGY_LINK_MS 20000
/* Connected, for every other phone to answer (its page may call eggy_load a
 * little later) and then for every HELLO. */
#define EGGY_CONNECT_MS 30000
/* A phone silent this long mid-match is settled away, and between two phones
 * that ends the match for good. 1.5 s ended matches over a TURN relay or
 * mobile data on the first hiccup of a second or two (the other phone's data
 * also lands a trip late there, ~250 ms of the limit gone); GGPO waits 5 s.
 * The others hold still meanwhile, so it stays short of that. */
#define EGGY_AWAY_MS 4000
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
    int32_t desync_told;    /* the desync frame last reported, -1 none */
    int desyncs;            /* reported this match */
    uint8_t away_told;      /* ports whose 'away' was reported */
    uint64_t go_release_at; /* the leader's own release, once its GO is on the way (SDL ms) */
    bool delay_picked;      /* the leader's fight delay, picked as its GO first went out */
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
    if ((away_mask() & s.others) == s.others) {
        return fail(
            EGGY_NET_ERR_PEER_LEFT); /* a page that started again: every other phone is out */
    }
    if (eggy_net_agreed(s.hello_sent, s.hello_got, s.others, away_mask())) {
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
            pc_net_fight_window_pick(); /* the load has measured the link by now */
            break;
        case EGGY_HOLD_SEND_ARMED:
            if (!send_msg(EGGY_REL_ARMED)) {
                return 0;
            }
            break;
        case EGGY_HOLD_SEND_GO: {
            if (!s.delay_picked) {
                s.delay_picked = true;
                pc_net_fight_delay_pick(); /* ahead of the GO, on the same lane */
            }
            if (!send_msg(EGGY_REL_GO)) {
                return 0;
            }
            /* The others release when the GO reaches them, half a trip from
             * now: the leader waits that out too, rather than starting the
             * match a trip ahead of everyone it plays. */
            int32_t ping[EGGY_PORTS];
            for (int port = 0; port < EGGY_PORTS; port++) {
                ping[port] = pc_net_mesh_ping_ms(port);
            }
            const int hold = eggy_go_hold_ms(ping, others_live());
            s.go_release_at = now + (uint64_t)hold;
            pc_log_line("eggy: GO sent; this phone releases in %d ms, with the others", hold);
            break;
        }
        case EGGY_HOLD_RELEASE:
            if (s.hold.sent_go && now < s.go_release_at) {
                return 0;
            }
            pc_net_hold_released();
            set_state(EGGY_NET_LIVE);
            pc_log_line("eggy: netplay released, ping %d ms, delay %d", s.stats[1], s.stats[2]);
            return 1;
        default:
            if (!s.hold.reported && now - s.hold_t0 > EGGY_LOADED_MS) {
                return fail(EGGY_NET_ERR_TIMEOUT);
            }
            if (s.hold.reported) {
                pc_net_hold_reserve(); /* the window's snapshots, a slot a held frame */
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

EMSCRIPTEN_KEEPALIVE int eggy_net_peer_reset(int port) {
    if (s.state == EGGY_NET_LIVE || s.state == EGGY_NET_CLOSING) {
        pc_log_line("eggy: port %d started again mid-match; nothing is resumed", port + 1);
        return EGGY_PEER_RESET_FIGHTING;
    }
    if (s.state != EGGY_NET_HANDSHAKE && s.state != EGGY_NET_HOLD) {
        pc_net_mesh_peer_reset(port); /* no mesh up: only the ring is emptied */
        return EGGY_PEER_RESET_NONE;
    }
    if (port < 0 || port >= EGGY_PORTS || (s.others & (1u << port)) == 0) {
        return EGGY_PEER_RESET_PORT;
    }
    const int reset = pc_net_mesh_peer_reset(port);
    if (reset < 0) {
        return reset == -3 ? EGGY_PEER_RESET_LEFT : EGGY_PEER_RESET_PORT;
    }
    const uint8_t bit = (uint8_t)(1u << port);
    s.hello_got &= (uint8_t)~bit;
    s.done_got &= (uint8_t)~bit;
    eggy_hold_forget(&s.hold, port);
    if (reset == 2) {
        pc_net_mesh_resend_away(port); /* its own AWAY among them: it leaves */
        return EGGY_PEER_RESET_TOLD;
    }
    /* Ours again, in the order they first went: HELLO is seq 0 on every lane,
     * then LOADED and ARMED as far as this phone got, then who is out. */
    if (s.hello_sent) {
        uint8_t hello[EGGY_HELLO_LEN];
        eggy_hello_encode(hello, s.cfg.hash, eggy_net_session_id(&s.cfg));
        pc_net_mesh_send_to(port, EGGY_REL_HELLO, hello, sizeof hello);
    }
    uint8_t again[2];
    const int n = eggy_hold_resend(&s.hold, again);
    for (int i = 0; i < n; i++) {
        pc_net_mesh_send_to(port, again[i], NULL, 0);
    }
    pc_net_mesh_resend_away(port);
    pc_log_line(
        "eggy: port %d started again before the match; waiting for it from its HELLO", port + 1);
    return EGGY_PEER_RESET_DONE;
}

EMSCRIPTEN_KEEPALIVE const uint32_t* eggy_heap_layout(void) {
    static uint32_t w[EGGY_HEAP_LAYOUT_WORDS];
    Region r[MAX_REGIONS];
    const int n = snapshot_regions_now(r);
    memset(w, 0, sizeof w);
    w[1] = (uint32_t)n;
    w[2] = (uint32_t)(uintptr_t)HSD_RandSeedPtr;
    /* FNV-1a over what resync_unusable compares, in its order. */
    uint32_t digest = 2166136261u;
#define EGGY_FOLD(v) (digest = (digest ^ (uint32_t)(v)) * 16777619u)
    EGGY_FOLD(w[2]);
    EGGY_FOLD(n);
    for (int i = 0; i < n; i++) {
        uint32_t* out = w + 3 + 3 * i;
        out[0] = i < SNAPSHOT_FIXED_REGIONS ? (uint32_t)i :
                                              16u + (uint32_t)strtoul(r[i].name + 4, NULL, 10);
        out[1] = (uint32_t)(uintptr_t)r[i].ptr;
        out[2] = (uint32_t)r[i].len;
        EGGY_FOLD(out[1]);
        if (i < SNAPSHOT_FIXED_REGIONS) {
            EGGY_FOLD(out[2]);
        }
    }
#undef EGGY_FOLD
    w[0] = digest;
    return w;
}

const int32_t* eggy_net_stats_words(void) {
    if (s.state == EGGY_NET_LIVE || s.state == EGGY_NET_HOLD || s.state == EGGY_NET_CLOSING) {
        stats_refresh();
    }
    return s.stats;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The browser's datagram link for netplay (docs/netcode-plan.md §6).
 *
 * A page has no UDP. The page owns the link to the peer
 * (platforms/browser/bundle/netplay.mjs): a WebRTC data channel opened
 * unordered with no retransmits, which is UDP's contract, or, when the two
 * networks cannot reach each other, a WebSocket relay through the Cloudflare
 * Worker that paired them. net.c's datagrams cross into it through two
 * single-producer, single-consumer rings in shared wasm memory:
 *
 *   tx  net.c (always under net.tx_lock) -> the page, woken by an Atomics
 *       notify on the head, which sends each slot as one message -- only
 *       from a worker thread; on the page's own thread a send goes straight
 *       into the channel (web_net_send_now);
 *   rx  the page -> net.c, which the page runs on arrival (net_web_pump)
 *       or a receive thread sleeping on the head drains.
 *
 * A full ring drops the datagram, as a full socket buffer would; the
 * protocol already resends everything that matters. Nothing here parses a
 * datagram: net.c's gate (rx_datagram) authenticates and checks each one
 * exactly as it does for a socket.
 *
 * A mesh session (net_mesh.c, MELEE_EGGY netplay) has two to four phones on
 * the same rings, so each of its datagrams carries one leading byte naming
 * the other end: the destination engine port in tx, the source engine port in
 * rx, inside the slot's 512 bytes. The page maps ports to its links.
 * Module.meleeNetLink.mesh is the mesh protocol this engine speaks (an M2
 * engine has none, and its rings never carry the byte).
 */
#ifdef __EMSCRIPTEN__
#include "compat.h"
#include "pc/net_internal.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define WEB_SLOTS 256 /* power of two */
#define WEB_SLOT_BYTES 512
_Static_assert(WEB_SLOT_BYTES == MESH_SLOT_BYTES, "a mesh datagram and its port byte fill a slot");

typedef struct WebRing {
    _Atomic uint32_t head; /* slots written, ever (producer) */
    _Atomic uint32_t tail; /* slots consumed, ever (consumer) */
    uint32_t len[WEB_SLOTS];
    uint8_t data[WEB_SLOTS][WEB_SLOT_BYTES];
} WebRing;

static WebRing s_tx, s_rx;
/* Bumped by net_web_reset: the page drops what it had queued for an older
 * session instead of delivering it into the next. */
static _Atomic uint32_t s_generation;

// clang-format off
EM_JS(void, web_net_register, (void* tx, void* rx, void* generation, int slots, int bytes, int mesh), {
  Module.meleeNetLink = { buffer: HEAPU8.buffer, tx, rx, generation, slots, bytes, mesh };
  Module.meleeNetLinkReady?.();
});
// clang-format on

__attribute__((constructor)) static void net_web_init(void) {
    web_net_register(&s_tx, &s_rx, &s_generation, WEB_SLOTS, WEB_SLOT_BYTES, MESH_VERSION);
}

// clang-format off
/* On the page's thread the datagram goes out at once: the data channel's
 * send() is synchronous, so nothing waits for the game thread to yield. */
EM_JS(int, web_net_send_now, (const void* buf, int len), {
  const n = Module.meleeNetplay;
  if (!n?.sendNow) return 0;
  n.sendNow(HEAPU8.slice(buf, buf + len));
  return 1;
});
// clang-format on

/* The page's receive notification (netplay.mjs): run net.c's receive gate on
 * what just arrived. */
EMSCRIPTEN_KEEPALIVE void net_web_pump(void) {
    extern void pc_net_web_rx_now(void);
    pc_net_web_rx_now();
}

int net_web_send(const void* buf, size_t len) {
    if (len > WEB_SLOT_BYTES) {
        errno = EMSGSIZE;
        return -1;
    }
    if (emscripten_is_main_browser_thread() && web_net_send_now(buf, (int)len)) {
        return (int)len;
    }
    uint32_t head = atomic_load_explicit(&s_tx.head, memory_order_relaxed);
    if (head - atomic_load_explicit(&s_tx.tail, memory_order_acquire) >= WEB_SLOTS) {
        errno = EAGAIN; /* the page is not keeping up: drop, as UDP would */
        return -1;
    }
    uint32_t slot = head & (WEB_SLOTS - 1);
    memcpy(s_tx.data[slot], buf, len);
    s_tx.len[slot] = (uint32_t)len;
    atomic_store_explicit(&s_tx.head, head + 1, memory_order_release);
    emscripten_futex_wake((void*)&s_tx.head, 1); /* the page's Atomics.waitAsync */
    return (int)len;
}

int net_web_recv(void* buf, size_t cap) {
    uint32_t tail = atomic_load_explicit(&s_rx.tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&s_rx.head, memory_order_acquire)) {
        errno = EAGAIN;
        return -1;
    }
    uint32_t slot = tail & (WEB_SLOTS - 1);
    uint32_t len = s_rx.len[slot];
    if (len > cap) {
        len = (uint32_t)cap;
    }
    memcpy(buf, s_rx.data[slot], len);
    atomic_store_explicit(&s_rx.tail, tail + 1, memory_order_release);
    return (int)len;
}

/* net.c's sock_wait. Only the receive thread blocks; the page's main thread
 * must never park here, or the page could not deliver what it waits for. */
void net_web_wait(int ms) {
    uint32_t tail = atomic_load_explicit(&s_rx.tail, memory_order_relaxed);
    if (emscripten_is_main_browser_thread() ||
        atomic_load_explicit(&s_rx.head, memory_order_acquire) != tail)
    {
        return;
    }
    emscripten_futex_wait((void*)&s_rx.head, tail, (double)ms);
}

/* The session closed: forget both directions. Only called from net.c's
 * disconnect, after its receive thread has been joined. */
void net_web_reset(void) {
    atomic_store(&s_rx.tail, atomic_load(&s_rx.head));
    atomic_store(&s_tx.tail, atomic_load(&s_tx.head));
    atomic_fetch_add(&s_generation, 1);
}
#endif

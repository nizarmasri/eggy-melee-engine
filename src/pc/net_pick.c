/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A fight's input delay and prediction window from its link (net_pick.h).
 *
 * The two together have to cover the one-way trip, or the fight stalls on
 * every frame and runs at (delay + window) / trip of full speed. Two phones on
 * different networks measured a 434-510 ms round trip over a TURN relay (14
 * frames each way with the jitter) against a delay capped at 4 and a window
 * of 7: 25-49 ticks a second, waiting on each other for most of the clock. */
#include "pc/net_pick.h"

int net_pick_need(uint32_t ping_us, uint32_t jitter_us, int32_t frame_us) {
    if (ping_us == 0 || frame_us <= 0) {
        return 0;
    }
    const uint64_t trip = (uint64_t)ping_us / 2 + jitter_us;
    return (int)((trip + (uint64_t)frame_us - 1) / (uint64_t)frame_us);
}

int net_pick_fight_delay(int need, int lo) {
    int d = need <= 6 ? need - 2 : (need + 1) / 2;
    if (need > 6 && d < 4) {
        d = 4;
    }
    return d < lo ? lo : d > NET_FIGHT_DELAY_MAX ? NET_FIGHT_DELAY_MAX : d;
}

int net_pick_window(int need, int delay) {
    const int w = need - delay + 4;
    return w < NET_WINDOW_MIN ? NET_WINDOW_MIN : w > NET_WINDOW_MAX ? NET_WINDOW_MAX : w;
}

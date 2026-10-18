/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Native-only pieces of src/pc that the browser platform replaces.
 *
 * The desktop launcher (launcher.cpp) and the archive file cache
 * (file_cache.cpp) have no browser counterpart: settings come from the hosting
 * page through the environment, and disc reads are cached by the page.
 * (gcadapter.c is compiled here, reading the adapter the page opens through
 * WebHID.) Everything else in src/pc is compiled unchanged.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "pc/eggy/eggy.h"
#include "pc/file_cache.h"
#include "pc/net.h"
#include "pc/pc.h"
#include "pc/slp.h"

/* Desktop launcher preferences; defaults match launcher_data.hpp, except that
 * the VS-only profile plays the competitive standard (Slippi's and 20XX's):
 * UCF on. MELEE_UCF overrides it, as in the launcher (launcher.cpp). */
bool pc_is_unlock_all_enabled(void) {
    return pc_vs_only();
}
/* Frozen Pokemon Stadium (no transformations): the competitive standard,
 * Slippi's online setting, and one less file loaded mid-match (the four
 * transformation archives). A session plays the host's value, carried in
 * RULES, as launcher.cpp does; MELEE_FROZEN_STADIUM=0 restores the
 * transformations. */
bool pc_is_frozen_stadium_enabled(void) {
    bool unlock_all, frozen;
    if (pc_eggy_enabled()) {
        return pc_eggy_frozen_stadium(); /* the page's config, word 11 */
    }
    if (pc_net_rules(&unlock_all, &frozen)) {
        return frozen;
    }
    const char* env = getenv("MELEE_FROZEN_STADIUM");
    return env != NULL ? env[0] != '0' : pc_vs_only();
}
bool pc_is_free_camera_enabled(void) {
    return false;
}
bool pc_is_ucf_enabled(void) {
    const char* env = getenv("MELEE_UCF");
    if (pc_net_deterministic() || pc_eggy_enabled()) {
        return true; /* both peers must agree; launcher.cpp forces it too */
    }
    return env != NULL ? env[0] != '0' : pc_vs_only();
}
/* MELEE_EGGY plays fixed 16:9 (src/pc/eggy), with the HUD spread to it. */
int pc_get_hud_mode(void) {
    return pc_eggy_enabled() ? 1 : 0;
}
bool pc_is_custom_textures_enabled(void) {
    return false;
}
/* Netplay lobby identity and Direct Connect settings (net_lan.c, net_match.c); the lobby itself is
 * unreachable here. */
const char* pc_get_net_name(void) {
    return "";
}
const char* pc_get_net_target(void) {
    return "";
}
void pc_set_net_target(const char* code) {
    (void)code;
}
int pc_get_net_port(void) {
    return 0;
}
uint64_t pc_install_id(void) {
    return 0;
}
const char* pc_app_rev(void) {
    return "browser";
}

/* Archive cache and background prewarm: the page owns disc caching. */
bool pc_file_cache_get(const char* filename, void* dst, size_t* size) {
    (void)filename;
    (void)dst;
    (void)size;
    return false;
}
bool pc_file_cache_get_size(const char* filename, size_t* size) {
    (void)filename;
    (void)size;
    return false;
}
void pc_file_cache_put(const char* filename, const void* data, size_t size) {
    (void)filename;
    (void)data;
    (void)size;
}
void pc_file_cache_start_prewarm(void) {}
bool pc_file_cache_require(const char* filename) {
    (void)filename;
    return false;
}

/* Replay recording writes to a native file; the browser has no recorder. */
void pc_slp_match_start(const struct StartMeleeData* data) {
    (void)data;
}
void pc_slp_match_end(void) {}
void pc_slp_tick_begin(void) {}
void pc_slp_tick_end(uint64_t proc_mask) {
    (void)proc_mask;
}
void pc_slp_pre_frame(struct HSD_GObj* gobj) {
    (void)gobj;
}

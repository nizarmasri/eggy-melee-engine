# SPDX-License-Identifier: GPL-3.0-or-later
# Static wasm link: position-independent objects only add GOT indirection.
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
set(AURORA_CACHE_USE_ZSTD OFF CACHE BOOL "" FORCE)
set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
add_compile_definitions(ENABLE_BACKEND_WEBGPU)
# -fsigned-char: wasm32's default, stated because the game assumes it (as the
# GameCube's compiler and upstream's native builds do).
add_compile_options(--use-port=emdawnwebgpu -pthread -fsigned-char)
# Paths baked into the binary (__FILE__ in asserts) relative to the checkout,
# so a published build does not carry the builder's directory layout.
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=)
add_link_options(--use-port=emdawnwebgpu -pthread -sALLOW_MEMORY_GROWTH=1 -sASYNCIFY=1)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/aurora ${CMAKE_BINARY_DIR}/aurora)
# The same floating-point contract as upstream's native builds, so netplay bits match.
set(MELEE_FP_FLAGS -ffp-contract=off -fno-fast-math)
target_compile_options(aurora_mtx PRIVATE ${MELEE_FP_FLAGS}
  -fno-builtin-sinf -fno-builtin-cosf -fno-builtin-tanf -fno-builtin-atanf
  -include ${CMAKE_SOURCE_DIR}/src/pc/libm/pc_trig.h)
target_compile_options(aurora_gx PRIVATE ${MELEE_FP_FLAGS})
target_compile_options(aurora_gd PRIVATE ${MELEE_FP_FLAGS})
# The PC layer (src/pc). This platform supplies its own main and disc access
# (main.c, dvd.c), stubs the native launcher and file cache (pc_stubs.c), and
# replaces net_match.c's DHT matchmaking with rooms on the netplay Worker
# (net_match_web.c, over the page's link, net_web.c). A new src/pc file the
# game needs goes in this list.
set(BROWSER_PC_SOURCES
 audio.c discfont.c gcadapter.c gx.c input_poll.c keyboard.c misc.c os.c profile.c region.c stb_vorbis.c thp.c touch.c vi.c widescreen.c
 eggy/eggy.c eggy/eggy_config.c eggy/eggy_hold.c eggy/eggy_net.c eggy/eggy_perf.c eggy/eggy_report.c
 android_compat.cpp music_stream.cpp textures.cpp
 net.c net_chat.c net_dht.c net_dht_item.c net_handshake.c net_identity.c net_lan.c net_match_web.c net_rank.c
 net_mesh.c net_mesh_wire.c net_pick.c net_rank_session.c net_resync.c net_rank_store.c net_reliable.c net_sfx.c net_sim.c net_snapshot.c net_sync.c net_watchdog.c net_web.c net_wire.c
 libm/pc_atanf.c libm/pc_cosdf.c libm/pc_cosf.c libm/pc_perturb.c libm/pc_rank_exp.c libm/pc_rank_sqrt.c
 libm/pc_rem_pio2_large.c libm/pc_rem_pio2f.c libm/pc_sindf.c libm/pc_sinf.c libm/pc_tandf.c libm/pc_tanf.c)
list(TRANSFORM BROWSER_PC_SOURCES PREPEND ${CMAKE_SOURCE_DIR}/src/pc/)
# Vendored netplay dependencies.
list(APPEND BROWSER_PC_SOURCES
 ${CMAKE_SOURCE_DIR}/extern/dht/dht.c ${CMAKE_SOURCE_DIR}/extern/dht/sha1.c
 ${CMAKE_SOURCE_DIR}/extern/monocypher/monocypher.c ${CMAKE_SOURCE_DIR}/extern/monocypher/monocypher-ed25519.c)
file(GLOB_RECURSE GAME_OBJECTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/build/browser/game/*.o")
set_source_files_properties(${GAME_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_executable(melee_browser ${GAME_OBJECTS}
 main.c dvd.c scheduler.c pc_stubs.c clock.c
 ${BROWSER_PC_SOURCES})
target_include_directories(melee_browser PRIVATE ${CMAKE_SOURCE_DIR}/extern/aurora/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/sdk_include ${CMAKE_SOURCE_DIR}/extern/monocypher)
target_link_libraries(melee_browser PRIVATE aurora::core aurora::gx aurora::gd aurora::vi aurora::pad aurora::si aurora::mtx aurora::os aurora::card aurora::thp)
# The Eggy host API (src/pc/eggy, EGGY.md): the page's calls (and HEAPU8/HEAP32,
# below, for its buffers), and every PADRead routed through src/pc/eggy/eggy.c
# so the page owns input under MELEE_EGGY.
set(EGGY_EXPORTS _eggy_config_buffer _eggy_load _eggy_go _eggy_pad _eggy_hud _eggy_results _eggy_abort
 _eggy_checksum _eggy_audio _eggy_costumes _eggy_team_costume _eggy_net_stats
 _eggy_snapshot_begin _eggy_snapshot_size _eggy_snapshot_buffer _eggy_snapshot_frame
 _eggy_snapshot_reserve _eggy_snapshot_apply _eggy_test_desync _eggy_view _eggy_fighter _eggy_tap _eggy_perf _eggy_draw_every
 _eggy_lite _eggy_graphics_ready _eggy_compile_cap _eggy_background_builds _eggy_pipeline_cache_flush
 _eggy_preroll_min _eggy_graphics_stats _eggy_perf_words _eggy_net_peer_reset _eggy_heap_layout
 _eggy_graphics_words _eggy_hitches)
list(JOIN EGGY_EXPORTS "," EGGY_EXPORTS)
# MELEE_STATE_SECTIONS: the netplay snapshot ranges (tools/browser/wasm_state_sections.py).
target_compile_definitions(melee_browser PRIVATE TARGET_PC=1 MELEE_PC=1 MELEE_USE_BUILTIN_SHA1 MELEE_STATE_SECTIONS=1)
target_compile_options(melee_browser PRIVATE -Wno-incompatible-pointer-types -Wno-int-conversion ${MELEE_FP_FLAGS})
# Asyncify instruments every function that might be on the stack when the game
# yields to the page (the frame boundary, disc polls and reads, the GPU wait at
# a frame's start), and every indirect call counts as might: three functions in
# four were instrumented, drawing included. asyncify-remove.json names what can
# never be on that stack -- GX and aurora's command processing and encoding, the
# HSD display tree, math, the allocator and its locks -- and they run plain.
# A function listed there must never reach browser_yield, emscripten_sleep or
# browser_disc_read: Asyncify cannot unwind through it.
target_link_options(melee_browser PRIVATE -O1 --profiling-funcs -sEMULATE_FUNCTION_POINTER_CASTS=1 -sSTACK_SIZE=8388608 -sINITIAL_MEMORY=268435456 -sMAXIMUM_MEMORY=2147483648 -sPTHREAD_POOL_SIZE=4 -sEXIT_RUNTIME=0 -sASSERTIONS=0 -lidbfs.js -sFORCE_FILESYSTEM=1 -Wl,--wrap=clock_gettime -Wl,--wrap=PADRead -sEXPORTED_FUNCTIONS=_main,_net_web_pump,${EGGY_EXPORTS} -sEXPORTED_RUNTIME_METHODS=callMain,FS,ENV,HEAPU8,HEAP32 -sINVOKE_RUN=0 -sASYNCIFY_IMPORTS=browser_disc_read -sASYNCIFY_REMOVE=@${CMAKE_CURRENT_SOURCE_DIR}/asyncify-remove.json -sASYNCIFY_STACK_SIZE=1048576 -sERROR_ON_UNDEFINED_SYMBOLS=1 -Wl,--error-limit=0)
set_target_properties(melee_browser PROPERTIES SUFFIX ".js")
# emcc's single-pass link fails validation at -O2/-O3 when --fpcast-emu runs
# ahead of the optimizer (direct calls into cast thunks keep the original
# arity), so link at -O1 and optimize the finished module as a second pass.
# Bound inlining after Asyncify: giant functions cause large Safari OMG JIT
# register-allocation spikes, even while the game heap stays at 256 MiB.
find_program(WASM_OPT wasm-opt HINTS "${EMSCRIPTEN_ROOT_PATH}/../bin" REQUIRED)
add_custom_command(TARGET melee_browser POST_BUILD
  COMMAND ${WASM_OPT} -O3 -g --inline-max-combined-binary-size=20000 --one-caller-inline-max-function-size=2000 --mvp-features --enable-threads --enable-bulk-memory --enable-bulk-memory-opt
    --enable-call-indirect-overlong --enable-multivalue --enable-mutable-globals --enable-nontrapping-float-to-int
    --enable-reference-types --enable-sign-ext --pass-arg=directize-initial-contents-immutable
    $<TARGET_FILE_DIR:melee_browser>/melee_browser.wasm -o $<TARGET_FILE_DIR:melee_browser>/melee_browser.wasm
  COMMENT "Optimizing melee_browser.wasm at -O3")

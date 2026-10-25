#ifdef __EMSCRIPTEN__
#include <cstdint>
#include <emscripten.h>
extern "C" void browser_yield(void);
extern "C" uint32_t aurora_browser_pipelines_compiling(void); /* gx/gx.cpp */
extern "C" uint32_t aurora_pipelines_finished(void);
extern "C" uint32_t aurora_pipelines_failed(void);
extern "C" uint32_t aurora_compile_ms_total(void);
EM_JS(void, browser_graphics_progress, (unsigned done, unsigned total), {
  Module.onGraphicsPreparation?.(done, total);
});
// The engine Safari and every iOS browser run, by the Eggy page's own test
// (webkitGpu): WebKit compiles each pipeline into Metal in its GPU process,
// about 50 ms apiece on an iPhone, and lost that process when hundreds were
// asked for at once.
EM_JS(int, browser_is_webkit, (), {
  const ua = navigator.userAgent || "";
  const webkit = /AppleWebKit/.test(ua) && !/Chrome|Chromium|Android|Edg/.test(ua);
  return webkit || /CriOS|FxiOS|EdgiOS/.test(ua) ? 1 : 0;
});
#endif
#include "pipeline_cache.hpp"

#include "clear.hpp"
#include "frame_packet.hpp"
#include "resources.hpp"
#include "hash.hpp"
#include "../gx/pipeline.hpp"
#include "../io.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../sqlite_utils.hpp"
#include "../webgpu/gpu.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <variant>
#include <thread>
#include <vector>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_thread.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <aurora/gfx.h>
#include <fmt/format.h>
#include <tracy/Tracy.hpp>

namespace aurora::gfx {
static Module Log("aurora::gfx::pipeline_cache");

constexpr int PipelineCacheSchema = 1;
constexpr const char* InitialPipelineCacheName = "initial_pipeline_cache.db";
constexpr const char* SdlVfsName = "aurora_pipeline_cache_sdl_vfs";

using NewPipelineCallback = std::function<wgpu::RenderPipeline()>;

struct CachedPipeline {
  wgpu::RenderPipeline pipeline;
};

struct PendingPipeline {
  PipelineRef hash;
  NewPipelineCallback create;
  bool fog = false; // a GX config with fog, held back while fog is off (aurora_set_fog_enabled)
};

struct PipelineCacheWrite {
  ShaderType type;
  PipelineRef hash;
  uint32_t configVersion = 0;
  ByteBuffer config;
  uint32_t firstFrameUsed = UINT32_MAX;
  bool deviceBuilt = false; // only marks (type, hash) in pipeline_device
  bool tagRow = false;      // only (hash, tag) in pipeline_tags
  uint32_t tag = 0;
};

using SavedPipelineConfig = std::variant<gx::PipelineConfig, clear::PipelineConfig
#ifdef AURORA_ENABLE_RMLUI
                                         ,
                                         rmlui::PipelineConfig
#endif
                                         >;

struct KnownPipeline {
  ShaderType type;
  SavedPipelineConfig config;
  uint32_t firstFrameUsed;
};

struct SdlVfsSqliteFile {
  sqlite3_file base;
  SDL_IOStream* io = nullptr;
};

static std::mutex g_pipelineMutex;
static bool g_hasPipelineThread = false;
static size_t g_pipelinesPerFrame = 0;
// For synchronous pipeline fallback (OpenGL)
#if defined(NDEBUG) && !defined(__ANDROID__)
constexpr size_t BuildPipelinesPerFrame = 5;
#else
constexpr size_t BuildPipelinesPerFrame = 1;
#endif
static std::vector<std::thread> g_pipelineThreads;
static std::atomic_bool g_pipelineThreadEnd = false;
static std::condition_variable g_pipelineQueueCv;
static std::condition_variable g_pipelineReadyCv;
static absl::flat_hash_map<PipelineRef, CachedPipeline> g_pipelines;
static absl::flat_hash_map<HashType, KnownPipeline> g_knownPipelines;
static std::optional<uint64_t> g_pipelineLayoutKey;
static std::deque<PendingPipeline> g_pipelineQueue;
static std::deque<PendingPipeline> g_backgroundPipelineQueue;
static absl::flat_hash_set<PipelineRef> g_pendingPipelines;
/* The refs in g_backgroundPipelineQueue. A draw of a queued pipeline promotes
 * it, which searched the whole background queue (every known config, twice) on
 * every draw of every queued pipeline, every frame. */
static absl::flat_hash_set<PipelineRef> g_backgroundPending;
static std::atomic_uint32_t g_drawsSkipped{0};
#ifdef __EMSCRIPTEN__
/* The browser compile gate. Every GX pipeline on the web is created by
 * browser_pump, the only caller of create for them, which keeps at most the cap
 * compiling at once, demanded work first. Before it, draws started compiles
 * inline (five a frame, three hundred a second) with nothing bounding how many
 * were in flight, and frame 0 of a match asked for hundreds at once: an
 * iPhone's GPU process, at about twenty a second, lost the device at GO. */
static uint32_t g_browserCompileCap = 0;  // aurora_set_compile_cap; 0 is the browser's default
static bool g_browserBackgroundOn = true; // aurora_set_background_builds
static std::deque<PendingPipeline> g_backgroundFogParked; // background fog configs while fog is off
/* Asked for by a draw and not compiled yet; the second set only those first
 * asked for after aurora_mark_go. */
static absl::flat_hash_set<PipelineRef> g_browserWanted;
static absl::flat_hash_set<PipelineRef> g_browserWantedAfterGo;
static bool g_browserAfterGo = false;
/* The device database keeps only configs this browser has compiled: a row is
 * held here until its first compile succeeds, and dropped if it is refused,
 * so a config that took the GPU process down is never asked for at boot. */
static absl::flat_hash_map<HashType, PipelineCacheWrite> g_browserUnbuiltRows;
static absl::flat_hash_set<HashType> g_browserBuiltKeys;
/* Which configs a match needed, by the host's tags for it (its fighters and
 * stage): pipeline_tags in the device database, and here by tag. The next
 * match sharing a tag builds them all before GO (aurora_prebuild_tagged), the
 * effects that only appear after GO with them. */
static std::vector<uint32_t> g_matchTags;
static absl::flat_hash_set<HashType> g_matchTagged;
static absl::flat_hash_map<uint32_t, absl::flat_hash_set<HashType>> g_tagConfigs;
#endif
static std::atomic_bool g_gpuCachePrunePending = false;
/* Android warms only pipelines this device has already built. The seed is
 * recorded on desktop GPUs and is not shipped in the APK
 * (tools/build_android.sh): an Adreno 750 driver (OnePlus Pad 2) fails one of
 * its configs with VK_ERROR_UNKNOWN, which aborted the app on every launch
 * once a worker walked the whole queue. A row is marked in pipeline_device
 * once a pipeline for it has been created here, so building it again at boot
 * asks nothing of the driver it has not already done; unmarked rows (a seed
 * merged by an older build, a config asked for but not built before exit) are
 * dropped at load. */
#if defined(__ANDROID__)
constexpr bool DeviceBuiltRowsOnly = true;
#else
constexpr bool DeviceBuiltRowsOnly = false;
#endif
static std::mutex g_deviceBuiltMutex;
static absl::flat_hash_set<HashType> g_deviceBuilt; // cache keys marked in pipeline_device

static bool env_flag(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && *v != '\0' && *v != '0';
}

/* MELEE_PIPELINE_SYNC=1: block the frame on a never-before-seen GX pipeline
 * (the issue #46 stutter) instead of dropping its draw until it compiles.
 * Kept for A/B; skipping is the default. */
static const bool g_pipelineSync = env_flag("MELEE_PIPELINE_SYNC");

/* MELEE_PIPELINE_JOBS=<n>: compile threads. The seed alone is ~12k configs,
 * so one thread takes minutes to turn it into PSOs; anything the game asks
 * for before then is skipped or (sync mode) stalled on. Threads run at low
 * priority so they only take CPU the game is not using. */
static unsigned pipeline_job_count() {
  if (const char* v = std::getenv("MELEE_PIPELINE_JOBS"); v != nullptr && *v != '\0') {
    return static_cast<unsigned>(std::clamp(std::strtoul(v, nullptr, 10), 1ul, 32ul));
  }
#if defined(__ANDROID__)
  /* One, not half the cores: several workers flooded mobile drivers (Adreno
   * libllvm-qglc.so) with concurrent vkCreateGraphicsPipelines until they
   * crashed or ran out of memory. */
  return 1;
#else
  return std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
#endif
}

static sqlite3* g_pipelineCacheDb = nullptr;
static sqlite3_stmt* g_pipelineCacheLoadStmt = nullptr;
static sqlite3_stmt* g_pipelineCacheUpsertStmt = nullptr;
static sqlite3_stmt* g_pipelineDeviceStmt = nullptr;
static sqlite3_stmt* g_pipelineTagStmt = nullptr;
static bool g_pipelineCacheBroken = false;
static std::thread g_pipelineCacheWriterThread;
static std::condition_variable g_pipelineCacheWriterCv;
static std::mutex g_pipelineCacheWriterMutex;
static std::deque<PipelineCacheWrite> g_pipelineCacheWriteQueue;
static bool g_pipelineCacheWriterStop = false;
// The writer holds a batch it has not committed yet (aurora_pipeline_cache_flush).
static bool g_pipelineCacheWriterBusy = false;
static std::condition_variable g_pipelineCacheWriterIdleCv;
static int g_sdlVfsRegisterResult = SQLITE_ERROR;

static SdlVfsSqliteFile* sdl_vfs_file(sqlite3_file* file) { return reinterpret_cast<SdlVfsSqliteFile*>(file); }

static sqlite3_vfs* default_vfs(sqlite3_vfs* vfs) { return static_cast<sqlite3_vfs*>(vfs->pAppData); }

static int sdl_vfs_close(sqlite3_file* file) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io != nullptr) {
    SDL_CloseIO(vfsFile->io);
    vfsFile->io = nullptr;
  }
  return SQLITE_OK;
}

static int sdl_vfs_read(sqlite3_file* file, void* buffer, int amount, sqlite3_int64 offset) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io == nullptr || offset < 0 || amount < 0) {
    return SQLITE_IOERR_READ;
  }
  if (SDL_SeekIO(vfsFile->io, offset, SDL_IO_SEEK_SET) < 0) {
    return SQLITE_IOERR_SEEK;
  }

  auto* dst = static_cast<uint8_t*>(buffer);
  int total = 0;
  while (total < amount) {
    const size_t read = SDL_ReadIO(vfsFile->io, dst + total, static_cast<size_t>(amount - total));
    if (read == 0) {
      if (SDL_GetIOStatus(vfsFile->io) == SDL_IO_STATUS_EOF) {
        std::memset(dst + total, 0, static_cast<size_t>(amount - total));
        return SQLITE_IOERR_SHORT_READ;
      }
      return SQLITE_IOERR_READ;
    }
    if (read > static_cast<size_t>(std::numeric_limits<int>::max() - total)) {
      return SQLITE_IOERR_READ;
    }
    total += static_cast<int>(read);
  }
  return SQLITE_OK;
}

static int sdl_vfs_write(sqlite3_file*, const void*, int, sqlite3_int64) { return SQLITE_READONLY; }

static int sdl_vfs_truncate(sqlite3_file*, sqlite3_int64) { return SQLITE_READONLY; }

static int sdl_vfs_sync(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_file_size(sqlite3_file* file, sqlite3_int64* size) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io == nullptr || size == nullptr) {
    return SQLITE_IOERR_FSTAT;
  }

  const auto ioSize = SDL_GetIOSize(vfsFile->io);
  if (ioSize < 0) {
    return SQLITE_IOERR_FSTAT;
  }
  *size = static_cast<sqlite3_int64>(ioSize);
  return SQLITE_OK;
}

static int sdl_vfs_lock(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_unlock(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_check_reserved_lock(sqlite3_file*, int* reserved) {
  if (reserved != nullptr) {
    *reserved = 0;
  }
  return SQLITE_OK;
}

static int sdl_vfs_file_control(sqlite3_file*, int op, void* arg) {
  switch (op) {
  case SQLITE_FCNTL_LOCKSTATE:
    *static_cast<int*>(arg) = SQLITE_LOCK_NONE;
    return SQLITE_OK;
  case SQLITE_FCNTL_HAS_MOVED:
    *static_cast<int*>(arg) = 0;
    return SQLITE_OK;
  case SQLITE_FCNTL_SIZE_HINT:
    return SQLITE_OK;
  default:
    return SQLITE_NOTFOUND;
  }
}

static int sdl_vfs_sector_size(sqlite3_file*) { return 4096; }

static int sdl_vfs_device_characteristics(sqlite3_file*) {
  return SQLITE_IOCAP_IMMUTABLE | SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN;
}

static constexpr sqlite3_io_methods SdlVfsIoMethods{
    .iVersion = 1,
    .xClose = sdl_vfs_close,
    .xRead = sdl_vfs_read,
    .xWrite = sdl_vfs_write,
    .xTruncate = sdl_vfs_truncate,
    .xSync = sdl_vfs_sync,
    .xFileSize = sdl_vfs_file_size,
    .xLock = sdl_vfs_lock,
    .xUnlock = sdl_vfs_unlock,
    .xCheckReservedLock = sdl_vfs_check_reserved_lock,
    .xFileControl = sdl_vfs_file_control,
    .xSectorSize = sdl_vfs_sector_size,
    .xDeviceCharacteristics = sdl_vfs_device_characteristics,
};

static int sdl_vfs_open(sqlite3_vfs*, const char* name, sqlite3_file* file, int flags, int* outFlags) {
  auto* vfsFile = sdl_vfs_file(file);
  vfsFile->base.pMethods = nullptr;
  vfsFile->io = nullptr;

  if (name == nullptr || (flags & SQLITE_OPEN_READWRITE) != 0 || (flags & SQLITE_OPEN_READONLY) == 0) {
    return SQLITE_CANTOPEN;
  }

  vfsFile->io = SDL_IOFromFile(name, "rb");
  if (vfsFile->io == nullptr) {
    return SQLITE_CANTOPEN;
  }

  vfsFile->base.pMethods = &SdlVfsIoMethods;
  if (outFlags != nullptr) {
    *outFlags = SQLITE_OPEN_READONLY;
  }
  return SQLITE_OK;
}

static int sdl_vfs_delete(sqlite3_vfs*, const char*, int) { return SQLITE_READONLY; }

static int sdl_vfs_access(sqlite3_vfs*, const char* name, int flags, int* result) {
  if (result == nullptr) {
    return SQLITE_IOERR_ACCESS;
  }
  if (name == nullptr || flags == SQLITE_ACCESS_READWRITE) {
    *result = 0;
    return SQLITE_OK;
  }

  auto* io = SDL_IOFromFile(name, "rb");
  *result = io != nullptr ? 1 : 0;
  if (io != nullptr) {
    SDL_CloseIO(io);
  }
  return SQLITE_OK;
}

static int sdl_vfs_full_pathname(sqlite3_vfs*, const char* name, int outSize, char* out) {
  if (name == nullptr || out == nullptr || outSize <= 0) {
    return SQLITE_CANTOPEN;
  }
  sqlite3_snprintf(outSize, out, "%s", name);
  return SQLITE_OK;
}

static void* sdl_vfs_dl_open(sqlite3_vfs*, const char*) { return nullptr; }

static void sdl_vfs_dl_error(sqlite3_vfs*, int bytes, char* message) {
  if (message != nullptr && bytes > 0) {
    sqlite3_snprintf(bytes, message, "%s", "Dynamic loading is unsupported");
  }
}

static void (*sdl_vfs_dl_sym(sqlite3_vfs*, void*, const char*))(void) { return nullptr; }

static void sdl_vfs_dl_close(sqlite3_vfs*, void*) {}

static int sdl_vfs_randomness(sqlite3_vfs* vfs, int bytes, char* out) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xRandomness != nullptr) {
    return base->xRandomness(base, bytes, out);
  }
  if (out != nullptr && bytes > 0) {
    std::memset(out, 0, static_cast<size_t>(bytes));
  }
  return bytes;
}

static int sdl_vfs_sleep(sqlite3_vfs* vfs, int microseconds) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xSleep != nullptr) {
    return base->xSleep(base, microseconds);
  }
  return microseconds;
}

static int sdl_vfs_current_time(sqlite3_vfs* vfs, double* time) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xCurrentTime != nullptr) {
    return base->xCurrentTime(base, time);
  }
  if (time != nullptr) {
    *time = 2440587.5;
  }
  return SQLITE_OK;
}

static int sdl_vfs_get_last_error(sqlite3_vfs*, int, char*) { return SQLITE_OK; }

static bool register_sdl_vfs() {
  static std::once_flag registerOnce;
  std::call_once(registerOnce, [] {
    auto* baseVfs = sqlite3_vfs_find(nullptr);
    static sqlite3_vfs sdlVfs{
        .iVersion = 1,
        .szOsFile = static_cast<int>(sizeof(SdlVfsSqliteFile)),
        .mxPathname = 4096,
        .pNext = nullptr,
        .zName = SdlVfsName,
        .pAppData = baseVfs,
        .xOpen = sdl_vfs_open,
        .xDelete = sdl_vfs_delete,
        .xAccess = sdl_vfs_access,
        .xFullPathname = sdl_vfs_full_pathname,
        .xDlOpen = sdl_vfs_dl_open,
        .xDlError = sdl_vfs_dl_error,
        .xDlSym = sdl_vfs_dl_sym,
        .xDlClose = sdl_vfs_dl_close,
        .xRandomness = sdl_vfs_randomness,
        .xSleep = sdl_vfs_sleep,
        .xCurrentTime = sdl_vfs_current_time,
        .xGetLastError = sdl_vfs_get_last_error,
    };
    g_sdlVfsRegisterResult = sqlite3_vfs_register(&sdlVfs, 0);
  });
  return g_sdlVfsRegisterResult == SQLITE_OK;
}

#if defined(__cpp_lib_atomic_ref)
static std::atomic_ref queuedPipelines{detail::resources().stats.queuedPipelines};
static std::atomic_ref createdPipelines{detail::resources().stats.createdPipelines};
#else
struct AtomicStatRef {
  uint32_t& ref;
  uint32_t operator++() { return __atomic_add_fetch(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator--() { return __atomic_sub_fetch(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator++(int) { return __atomic_fetch_add(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator--(int) { return __atomic_fetch_sub(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator=(uint32_t val) {
    __atomic_store_n(&ref, val, __ATOMIC_RELAXED);
    return val;
  }
};
static AtomicStatRef queuedPipelines{detail::resources().stats.queuedPipelines};
static AtomicStatRef createdPipelines{detail::resources().stats.createdPipelines};
#endif

/* melee-pc: what pipeline creation costs, logged by log_pipeline_stats.
 * Creation is Tint + vkCreateGraphicsPipelines (or the D3D12/Metal
 * equivalent); the Dawn blob cache can skip Tint and hand the driver a
 * VkPipelineCache, so the per-pipeline time is what tells a warm launch from
 * a cold one. "Needed by a draw" counts, once per pipeline, a draw that found
 * its pipeline not yet built: skipped, or on the inline path built while the
 * frame waited. That number is the stutter a player sees. */
static std::atomic_uint32_t g_statCreated{0};
static std::atomic_uint64_t g_statCreateNs{0};
static std::atomic_uint64_t g_statCreateMaxNs{0};
static absl::flat_hash_set<PipelineRef> g_demandedPipelines; // guarded by g_pipelineMutex

static wgpu::RenderPipeline create_timed(const NewPipelineCallback& create) {
#if defined(__ANDROID__)
  // SEARCH prewarm and GX FIFO draws may compile on different threads.
  static std::mutex createMutex;
  std::unique_lock lock{createMutex};
#endif
  const auto start = std::chrono::steady_clock::now();
  auto pipeline = create();
#if defined(__ANDROID__)
  lock.unlock();
#endif
  const uint64_t ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  ++g_statCreated;
  g_statCreateNs += ns;
  uint64_t max = g_statCreateMaxNs.load(std::memory_order_relaxed);
  while (ns > max && !g_statCreateMaxNs.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {
  }
  return pipeline;
}

static void log_pipeline_stats() {
  size_t demanded;
  {
    std::lock_guard lock{g_pipelineMutex};
    demanded = g_demandedPipelines.size();
  }
  const uint32_t created = g_statCreated.load();
  const double totalMs = static_cast<double>(g_statCreateNs.load()) / 1e6;
  const auto dawn = webgpu::cache_stats();
  Log.info("pipelines: {} created in {:.0f} ms (avg {:.1f} ms, max {:.1f} ms), {} needed by a draw before ready; "
           "Dawn cache: {} hits ({} KiB), {} misses, {} stores ({} KiB)",
           created, totalMs, created != 0 ? totalMs / created : 0.0,
           static_cast<double>(g_statCreateMaxNs.load()) / 1e6, demanded, dawn.hits, dawn.hitBytes / 1024,
           dawn.misses, dawn.stores, dawn.storeBytes / 1024);
#ifdef __EMSCRIPTEN__
  // "Created" above is a compile started; these are the browser's results.
  const uint32_t finished = aurora_pipelines_finished();
  Log.info("browser pipelines: {} compiled (avg {:.0f} ms each in the GPU process), {} refused, {} compiling, "
           "{} skipped draws",
           finished, finished != 0 ? static_cast<double>(aurora_compile_ms_total()) / finished : 0.0,
           aurora_pipelines_failed(), aurora_browser_pipelines_compiling(), g_drawsSkipped.load());
#endif
}

template <typename PipelineConfig>
static PipelineCacheWrite make_pipeline_cache_write(ShaderType type, PipelineRef hash, const PipelineConfig& config,
                                                    uint32_t firstFrameUsed) {
  static_assert(std::has_unique_object_representations_v<PipelineConfig>);

  PipelineCacheWrite write{
      .type = type,
      .hash = hash,
      .configVersion = config.version,
      .config = ByteBuffer(sizeof(config)),
      .firstFrameUsed = firstFrameUsed,
  };
  std::memcpy(write.config.data(), &config, sizeof(config));
  return write;
}

static void enqueue_pipeline_cache_write(PipelineCacheWrite write) {
  if (g_pipelineCacheBroken || g_pipelineCacheDb == nullptr) {
    return;
  }

  {
    std::lock_guard lock{g_pipelineCacheWriterMutex};
    g_pipelineCacheWriteQueue.emplace_back(std::move(write));
  }
  g_pipelineCacheWriterCv.notify_one();
}

/* Runs on whichever thread built the pipeline, sometimes under g_pipelineMutex
 * (inline builds), hence its own mutex. Aurora aborts on a pipeline the driver
 * rejects (immediate error handling), so reaching here means it was accepted. */
static void mark_device_built(ShaderType type, HashType cacheKey) {
  if constexpr (!DeviceBuiltRowsOnly) {
    return;
  }
  {
    std::lock_guard lock{g_deviceBuiltMutex};
    if (!g_deviceBuilt.insert(cacheKey).second) {
      return;
    }
  }
  enqueue_pipeline_cache_write(PipelineCacheWrite{.type = type, .hash = cacheKey, .deviceBuilt = true});
}

template <typename Queue>
static auto find_pending_pipeline(Queue& queue, PipelineRef hash) {
  return std::find_if(queue.begin(), queue.end(), [=](const PendingPipeline& pending) { return pending.hash == hash; });
}

// Both under g_pipelineMutex.
static void queue_background(PendingPipeline&& pending) {
  g_backgroundPending.insert(pending.hash);
  g_backgroundPipelineQueue.emplace_back(std::move(pending));
}

static PendingPipeline pop_background() {
  PendingPipeline pending = std::move(g_backgroundPipelineQueue.front());
  g_backgroundPipelineQueue.pop_front();
  g_backgroundPending.erase(pending.hash);
  return pending;
}

enum class PipelinePriority {
  Background, // cache warmup
  Normal,     // async skip draw
  Blocking,   // block until compiled
};

static void promote_pending_pipeline(PipelineRef hash, PipelinePriority priority) {
  if (priority == PipelinePriority::Background || !g_backgroundPending.contains(hash)) {
    return;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelineQueue, hash);
  if (backgroundIt == g_backgroundPipelineQueue.end()) {
    return;
  }
  if (priority == PipelinePriority::Blocking) {
    g_pipelineQueue.emplace_front(std::move(*backgroundIt));
  } else {
    g_pipelineQueue.emplace_back(std::move(*backgroundIt));
  }
  g_backgroundPipelineQueue.erase(backgroundIt);
  g_backgroundPending.erase(hash);
}

static std::optional<PendingPipeline> take_pending_pipeline(PipelineRef hash) {
  auto priorityIt = find_pending_pipeline(g_pipelineQueue, hash);
  if (priorityIt != g_pipelineQueue.end()) {
    PendingPipeline pending = std::move(*priorityIt);
    g_pipelineQueue.erase(priorityIt);
    g_pendingPipelines.erase(hash);
    return pending;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelineQueue, hash);
  if (backgroundIt != g_backgroundPipelineQueue.end()) {
    PendingPipeline pending = std::move(*backgroundIt);
    g_backgroundPipelineQueue.erase(backgroundIt);
    g_backgroundPending.erase(hash);
    g_pendingPipelines.erase(hash);
    return pending;
  }

  return std::nullopt;
}

static void notify_pipeline_ready(bool queued) {
  ++createdPipelines;
  if (queued && --queuedPipelines == 0 && g_gpuCachePrunePending.exchange(false, std::memory_order_acq_rel)) {
    // Prune GPU cache entries after fully loading the pipeline cache.
    webgpu::cache_prune();
  }
  g_pipelineReadyCv.notify_all();
}

template <typename Config>
static void remember_pipeline_config(ShaderType type, const Config& config, uint32_t firstFrameUsed, bool persist) {
  const auto cacheKey = xxh3_hash(config, static_cast<HashType>(type));
  bool changed = false;
  {
    std::lock_guard lock{g_pipelineMutex};
    auto [it, inserted] = g_knownPipelines.try_emplace(cacheKey, KnownPipeline{type, config, firstFrameUsed});
    changed = inserted || firstFrameUsed < it->second.firstFrameUsed;
    it->second.firstFrameUsed = std::min(it->second.firstFrameUsed, firstFrameUsed);
#ifdef __EMSCRIPTEN__
    if (persist && type == ShaderType::GX && !g_matchTags.empty() && g_matchTagged.insert(cacheKey).second) {
      for (const uint32_t tag : g_matchTags) {
        if (g_tagConfigs[tag].insert(cacheKey).second) {
          enqueue_pipeline_cache_write(PipelineCacheWrite{.type = type, .hash = cacheKey, .tagRow = true, .tag = tag});
        }
      }
    }
    // Written once a compile of it succeeds (browser_pipeline_settled).
    if (persist && changed && type == ShaderType::GX && !g_browserBuiltKeys.contains(cacheKey)) {
      if (!g_pipelineCacheBroken && g_pipelineCacheDb != nullptr) {
        g_browserUnbuiltRows.insert_or_assign(cacheKey,
                                              make_pipeline_cache_write(type, cacheKey, config, firstFrameUsed));
      }
      return;
    }
#endif
  }
  if (persist && changed) {
    enqueue_pipeline_cache_write(make_pipeline_cache_write(type, cacheKey, config, firstFrameUsed));
  }
}

#ifdef __EMSCRIPTEN__
static void browser_pump();

// Under g_pipelineMutex: a draw asked for a GX pipeline that is not compiled.
static void browser_want(PipelineRef ref) {
  if (g_browserWanted.insert(ref).second && g_browserAfterGo) {
    g_browserWantedAfterGo.insert(ref);
  }
}
#endif

// Under g_pipelineMutex: a draw found its pipeline not ready and was skipped.
static void note_skipped_draw(PipelineRef ref) {
#ifdef __EMSCRIPTEN__
  if (gx::browser_pipeline_failed(ref)) {
    return; // refused for good: not a draw still waiting on its pipeline
  }
  browser_want(ref);
#endif
  ++g_drawsSkipped;
}

/* gated: a GX pipeline, which the browser compiles asynchronously; those are
 * only ever queued there, for browser_pump. Clear pipelines are created
 * synchronously and stay inline, or the screen goes uncleared behind a queue. */
static PipelineRef find_pipeline_impl(PipelineRef runtimeKey, NewPipelineCallback&& cb, PipelinePriority priority,
                                      bool gated, bool fog) {
  ZoneScoped;

  const bool blocking = priority == PipelinePriority::Blocking;
#ifdef __EMSCRIPTEN__
  const bool queueOnly = gated && !blocking;
  const bool inlineAlways = !gated;
#else
  const bool queueOnly = false;
  const bool inlineAlways = false;
  (void)gated;
#endif
  bool notifyWorker = false;
  bool pipelineReady = false;
  bool createdPipeline = false;
  bool queued = false;
  {
    std::scoped_lock guard{g_pipelineMutex};
    auto pipelineIt = g_pipelines.find(runtimeKey);
    if (pipelineIt == g_pipelines.end() && priority != PipelinePriority::Background) {
      g_demandedPipelines.insert(runtimeKey);
#ifdef __EMSCRIPTEN__
      if (gated) {
        browser_want(runtimeKey);
      }
#endif
    }
    if (pipelineIt != g_pipelines.end()) {
      pipelineReady = true;
    } else if (g_pendingPipelines.contains(runtimeKey)) {
      if (blocking && !g_hasPipelineThread) {
        auto pending = take_pending_pipeline(runtimeKey);
        if (pending) {
          g_pipelines.try_emplace(runtimeKey, CachedPipeline{.pipeline = create_timed(pending->create)});
          pipelineReady = true;
          ++g_pipelinesPerFrame;
          createdPipeline = true;
          queued = true;
        }
      } else {
        promote_pending_pipeline(runtimeKey, priority);
        notifyWorker = priority != PipelinePriority::Background;
      }
    } else if (!queueOnly && !g_hasPipelineThread &&
               (blocking || inlineAlways || g_pipelinesPerFrame < BuildPipelinesPerFrame)) {
      g_pipelines.try_emplace(runtimeKey, CachedPipeline{.pipeline = create_timed(cb)});
      pipelineReady = true;
      ++g_pipelinesPerFrame;
      createdPipeline = true;
    } else {
      PendingPipeline pending{
          .hash = runtimeKey,
          .create = std::move(cb),
          .fog = fog,
      };
      switch (priority) {
      case PipelinePriority::Background:
        queue_background(std::move(pending));
        break;
      case PipelinePriority::Normal:
        g_pipelineQueue.emplace_back(std::move(pending));
        break;
      case PipelinePriority::Blocking:
        g_pipelineQueue.emplace_front(std::move(pending));
        break;
      }
      g_pendingPipelines.insert(runtimeKey);
      ++queuedPipelines;
      notifyWorker = true;
    }
  }

  if (createdPipeline) {
    notify_pipeline_ready(queued);
  }

  if (notifyWorker) {
#ifdef __EMSCRIPTEN__
    browser_pump();
#else
    g_pipelineQueueCv.notify_one();
#endif
  }

  if (blocking && !pipelineReady) {
    std::unique_lock lock{g_pipelineMutex};
    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(runtimeKey) || g_pipelineThreadEnd; });
  }
  return runtimeKey;
}

static PipelineRef resolve_pipeline(ShaderType type, const gx::PipelineConfig& config, const RenderTargetLayout& layout,
                                    PipelinePriority priority) {
  const auto cacheKey = xxh3_hash(config, static_cast<HashType>(type));
  return find_pipeline_impl(
      xxh3_hash(layout.key, cacheKey),
      [type, cacheKey, config, layout] {
        auto pipeline = create_pipeline(config, layout);
        mark_device_built(type, cacheKey);
        return pipeline;
      },
      priority, true, config.shaderConfig.fogType != GX_FOG_NONE);
}

static PipelineRef resolve_pipeline(ShaderType type, const clear::PipelineConfig& config,
                                    const RenderTargetLayout& layout, PipelinePriority priority) {
  const auto cacheKey = xxh3_hash(config, static_cast<HashType>(type));
  return find_pipeline_impl(
      xxh3_hash(layout.key, cacheKey),
      [type, cacheKey, config, layout] {
        auto pipeline = create_pipeline(config, layout);
        mark_device_built(type, cacheKey);
        return pipeline;
      },
      priority, false, false);
}

#ifdef AURORA_ENABLE_RMLUI
static PipelineRef resolve_pipeline(ShaderType type, const rmlui::PipelineConfig& config, const RenderTargetLayout&,
                                    PipelinePriority priority) {
  const auto cacheKey = xxh3_hash(config, static_cast<HashType>(type));
  return find_pipeline_impl(
      cacheKey,
      [type, cacheKey, config] {
        auto pipeline = rmlui::create_pipeline(config);
        mark_device_built(type, cacheKey);
        return pipeline;
      },
      priority, false, false);
}
#endif

static void pipeline_cache_abort() {
  g_pipelineCacheBroken = true;
  if (g_pipelineCacheLoadStmt != nullptr) {
    sqlite3_finalize(g_pipelineCacheLoadStmt);
    g_pipelineCacheLoadStmt = nullptr;
  }
  if (g_pipelineCacheUpsertStmt != nullptr) {
    sqlite3_finalize(g_pipelineCacheUpsertStmt);
    g_pipelineCacheUpsertStmt = nullptr;
  }
  if (g_pipelineDeviceStmt != nullptr) {
    sqlite3_finalize(g_pipelineDeviceStmt);
    g_pipelineDeviceStmt = nullptr;
  }
  if (g_pipelineTagStmt != nullptr) {
    sqlite3_finalize(g_pipelineTagStmt);
    g_pipelineTagStmt = nullptr;
  }
  if (g_pipelineCacheDb != nullptr) {
    sqlite3_close(g_pipelineCacheDb);
    g_pipelineCacheDb = nullptr;
  }
}

static bool write_pipeline_cache_record(const PipelineCacheWrite& write);

static std::string pipeline_cache_seed_path() {
  if (g_config.resourcesPath == nullptr || g_config.resourcesPath[0] == '\0') {
    return InitialPipelineCacheName;
  }

  std::string path{g_config.resourcesPath};
  if (path.back() != '/' && path.back() != '\\') {
    path += '/';
  }
  path += InitialPipelineCacheName;
  return path;
}

/* The seed has always been packaged beside the executable, which is the
 * directory aurora used before resourcesPath became configurable. A host that
 * points resourcesPath at a directory holding no seed -- melee-pc does, for
 * the launcher's own assets -- would otherwise find nothing and compile every
 * pipeline on first use, which players hit as a multi-second stutter. */
static std::string pipeline_cache_seed_beside_exe_path() {
  const char* base = SDL_GetBasePath();
  if (base == nullptr) {
    return InitialPipelineCacheName;
  }
  std::string path{base};
  if (path.empty() || (path.back() != '/' && path.back() != '\\')) {
    path += '/';
  }
  path += InitialPipelineCacheName;
  return path;
}

static sqlite3* open_pipeline_cache_seed_db(const std::string& path) {
  if (!register_sdl_vfs()) {
    Log.warn("Failed to register SDL pipeline cache seed VFS");
    return nullptr;
  }

  sqlite3* seedDb = nullptr;
  const auto ret = sqlite3_open_v2(path.c_str(), &seedDb, SQLITE_OPEN_READONLY | SQLITE_OPEN_PRIVATECACHE, SdlVfsName);
  if (ret != SQLITE_OK) {
    if (seedDb != nullptr) {
      sqlite3_close(seedDb);
    }
    return nullptr; /* the caller names every path it tried */
  }

  bool schemaMatch = false;
  const auto schemaQuery = fmt::format("SELECT 1 FROM aurora_schema WHERE value = {}", PipelineCacheSchema);
  const auto schemaRet =
      sqlite::exec(seedDb, schemaQuery.c_str(), [&schemaMatch](int, char**, char**) { schemaMatch = true; });
  if (schemaRet != SQLITE_OK) {
    Log.warn("Failed to read bundled pipeline cache schema from '{}': {}", path, sqlite3_errmsg(seedDb));
    sqlite3_close(seedDb);
    return nullptr;
  }
  if (!schemaMatch) {
    Log.warn("Bundled pipeline cache '{}' does not use schema version {}", path, PipelineCacheSchema);
    sqlite3_close(seedDb);
    return nullptr;
  }

  return seedDb;
}

#ifndef __EMSCRIPTEN__
static void seed_pipeline_cache() {
  if (g_pipelineCacheBroken || g_pipelineCacheDb == nullptr || g_pipelineCacheUpsertStmt == nullptr) {
    return;
  }

  // If the target database is already populated, skip re-seeding to accelerate startup
  bool alreadySeeded = false;
  sqlite::exec(g_pipelineCacheDb, "SELECT 1 FROM pipeline_cache LIMIT 1;",
               [&alreadySeeded](int, char**, char**) { alreadySeeded = true; });
  if (alreadySeeded) {
    return;
  }

  const auto seedPath = pipeline_cache_seed_path();
  const auto besideExe = pipeline_cache_seed_beside_exe_path();
  std::string usedPath = seedPath;
  sqlite3* seedDb = open_pipeline_cache_seed_db(seedPath);
  if (seedDb == nullptr && besideExe != seedPath) {
    seedDb = open_pipeline_cache_seed_db(besideExe);
    usedPath = besideExe;
  }
  if (seedDb == nullptr) {
    Log.info("No bundled initial pipeline cache found at '{}' or '{}'", seedPath, besideExe);
    return;
  }

  sqlite3_stmt* seedStmt = nullptr;
  auto closeSeed = [&] {
    if (seedStmt != nullptr) {
      sqlite3_finalize(seedStmt);
      seedStmt = nullptr;
    }
    sqlite3_close(seedDb);
  };

  auto ret = sqlite3_prepare_v3(seedDb,
                                "SELECT type, hash, config_version, config_size, config, first_frame_used "
                                "FROM pipeline_cache",
                                -1, 0, &seedStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.warn("Failed to read bundled pipeline cache rows from '{}': {}", usedPath, sqlite3_errmsg(seedDb));
    closeSeed();
    return;
  }

  bool writeFailed = false;
  bool readFailed = false;
  uint32_t mergedRows = 0;
  uint32_t skippedRows = 0;
  {
    sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
    if (!tx) {
      Log.error("Failed to begin pipeline cache seed transaction");
      closeSeed();
      pipeline_cache_abort();
      return;
    }

    while ((ret = sqlite3_step(seedStmt)) == SQLITE_ROW) {
      const auto typeValue = sqlite3_column_int(seedStmt, 0);
      const auto hashValue = sqlite3_column_int64(seedStmt, 1);
      const auto configVersionValue = sqlite3_column_int(seedStmt, 2);
      const auto configSizeValue = sqlite3_column_int(seedStmt, 3);
      const auto* configBlob = static_cast<const uint8_t*>(sqlite3_column_blob(seedStmt, 4));
      const auto configBlobSize = sqlite3_column_bytes(seedStmt, 4);
      const auto firstFrameUsedValue = sqlite3_column_int64(seedStmt, 5);
      constexpr auto MaxShaderTypeValue = std::numeric_limits<std::underlying_type_t<ShaderType>>::max();
      if (typeValue < 0 || typeValue > MaxShaderTypeValue || configVersionValue < 0 || configSizeValue < 0 ||
          configSizeValue != configBlobSize || (configBlobSize > 0 && configBlob == nullptr) ||
          firstFrameUsedValue < 0 || firstFrameUsedValue > std::numeric_limits<uint32_t>::max()) {
        ++skippedRows;
        continue;
      }

      PipelineCacheWrite write{
          .type = static_cast<ShaderType>(typeValue),
          .hash = static_cast<PipelineRef>(hashValue),
          .configVersion = static_cast<uint32_t>(configVersionValue),
          .config = ByteBuffer(static_cast<size_t>(configBlobSize)),
          .firstFrameUsed = static_cast<uint32_t>(firstFrameUsedValue),
      };
      if (configBlobSize > 0) {
        std::memcpy(write.config.data(), configBlob, static_cast<size_t>(configBlobSize));
      }

      if (!write_pipeline_cache_record(write)) {
        writeFailed = true;
        break;
      }
      ++mergedRows;
    }

    if (!writeFailed && ret != SQLITE_DONE) {
      Log.warn("Failed while reading bundled pipeline cache rows from '{}': {}", usedPath, sqlite3_errmsg(seedDb));
      readFailed = true;
    }

    if (!writeFailed && !readFailed) {
      tx.commit();
    }
  }

  closeSeed();

  if (writeFailed) {
    pipeline_cache_abort();
    return;
  }

  if (!readFailed) {
    Log.info("Seeded pipeline cache from '{}' ({} rows merged, {} rows skipped)", usedPath, mergedRows, skippedRows);
  }
}
#endif

static std::string pipeline_cache_db_path() {
  return io::fs_path_to_string(io::fs_path_from_string(g_config.cachePath) / "pipeline_cache.db");
}

#ifdef __EMSCRIPTEN__
/* The page persists /cache between visits (FS.syncfs after
 * aurora_pipeline_cache_flush). A database persisted torn, or cut short by the
 * browser, can open and fail later, so it is checked once at boot, and one
 * that is wrong in any way starts again empty instead of leaving the phone
 * with no cache for the session. The seed does not depend on it. */
static bool pipeline_cache_db_sound(const std::string& path) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }
  bool ok = false;
  const auto ret = sqlite::exec(db, "PRAGMA quick_check(1);", [&ok](int argc, char** argv, char**) {
    ok = argc > 0 && argv[0] != nullptr && std::strcmp(argv[0], "ok") == 0;
  });
  sqlite3_close(db);
  return ret == SQLITE_OK && ok;
}

static void remove_pipeline_cache_db(const char* why) {
  const auto path = pipeline_cache_db_path();
  Log.warn("Starting the pipeline cache database again: {}", why);
  std::remove(path.c_str());
  std::remove((path + "-journal").c_str());
}
#endif

static bool open_pipeline_cache_db() {
  if (g_pipelineCacheBroken) {
    return false;
  }
  if (g_pipelineCacheDb != nullptr) {
    return true;
  }

  const auto path = pipeline_cache_db_path();
#ifdef __EMSCRIPTEN__
  static bool checked = false;
  if (!checked) {
    checked = true;
    std::error_code ec;
    if (std::filesystem::exists(io::fs_path_from_string(path), ec) && !pipeline_cache_db_sound(path)) {
      remove_pipeline_cache_db("it failed its integrity check");
    }
  }
#endif
  auto ret = sqlite3_open(path.c_str(), &g_pipelineCacheDb);
  if (ret != SQLITE_OK) {
    Log.error("Failed to open pipeline cache database: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

#ifdef __EMSCRIPTEN__
  // MEMFS has no shared memory for the WAL index and the page is torn down
  // without a checkpoint, so WAL commits never reached the persisted database.
  ret = sqlite::exec(g_pipelineCacheDb, "PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF;");
#else
  ret = sqlite::exec(g_pipelineCacheDb, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
#endif
  if (ret != SQLITE_OK) {
    Log.error("Failed to set pipeline cache pragmas: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  bool schemaFailed = false;
  {
    sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
    if (!tx) {
      Log.error("Failed to begin pipeline cache schema transaction");
      schemaFailed = true;
    } else {
      ret = sqlite::exec(g_pipelineCacheDb, "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);");
      if (ret != SQLITE_OK) {
        Log.error("Failed to create pipeline cache schema table: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    bool schemaMatch = false;
    if (!schemaFailed) {
      const auto schemaQuery = fmt::format("SELECT 1 FROM aurora_schema WHERE value = {}", PipelineCacheSchema);
      ret = sqlite::exec(g_pipelineCacheDb, schemaQuery.c_str(),
                         [&schemaMatch](int, char**, char**) { schemaMatch = true; });
      if (ret != SQLITE_OK) {
        Log.error("Failed to read pipeline cache schema version: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    if (!schemaFailed && !schemaMatch) {
      const auto schemaSql = fmt::format(
          R"(DROP TABLE IF EXISTS pipeline_cache;
CREATE TABLE pipeline_cache (
  type INTEGER NOT NULL,
  hash INTEGER NOT NULL,
  config_version INTEGER NOT NULL,
  config_size INTEGER NOT NULL,
  config BLOB NOT NULL,
  first_frame_used INTEGER NOT NULL,
  PRIMARY KEY (type, hash)
);
CREATE INDEX pipeline_cache_load_order_idx
  ON pipeline_cache(type, config_version, first_frame_used);
DELETE FROM aurora_schema;
INSERT INTO aurora_schema VALUES ({});)",
          PipelineCacheSchema);
      ret = sqlite::exec(g_pipelineCacheDb, schemaSql.c_str());
      if (ret != SQLITE_OK) {
        Log.error("Failed to initialize pipeline cache schema: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    if (!schemaFailed) {
      tx.commit();
    }
  }

  if (schemaFailed) {
    pipeline_cache_abort();
    return false;
  }

  ret = sqlite3_prepare_v3(g_pipelineCacheDb,
                           "SELECT config, first_frame_used FROM pipeline_cache "
                           "WHERE type = ? AND config_version = ? "
                           "ORDER BY first_frame_used ASC, rowid ASC",
                           -1, SQLITE_PREPARE_PERSISTENT, &g_pipelineCacheLoadStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare pipeline cache load statement: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  ret = sqlite3_prepare_v3(
      g_pipelineCacheDb,
      "INSERT INTO pipeline_cache (type, hash, config_version, config_size, config, first_frame_used) "
      "VALUES (?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(type, hash) DO UPDATE SET "
      "config_version = excluded.config_version, "
      "config_size = excluded.config_size, "
      "config = excluded.config, "
      "first_frame_used = MIN(pipeline_cache.first_frame_used, excluded.first_frame_used)",
      -1, SQLITE_PREPARE_PERSISTENT, &g_pipelineCacheUpsertStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare pipeline cache upsert statement: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  if constexpr (DeviceBuiltRowsOnly) {
    ret = sqlite::exec(g_pipelineCacheDb,
                       "CREATE TABLE IF NOT EXISTS pipeline_device ("
                       "type INTEGER NOT NULL, hash INTEGER NOT NULL, PRIMARY KEY (type, hash)"
                       ") WITHOUT ROWID;");
    if (ret == SQLITE_OK) {
      ret = sqlite3_prepare_v3(g_pipelineCacheDb, "INSERT OR IGNORE INTO pipeline_device (type, hash) VALUES (?, ?)",
                               -1, SQLITE_PREPARE_PERSISTENT, &g_pipelineDeviceStmt, nullptr);
    }
    if (ret != SQLITE_OK) {
      Log.error("Failed to prepare pipeline_device table: {}", sqlite3_errmsg(g_pipelineCacheDb));
      pipeline_cache_abort();
      return false;
    }
  }

#ifdef __EMSCRIPTEN__
  ret = sqlite::exec(g_pipelineCacheDb, "CREATE TABLE IF NOT EXISTS pipeline_tags ("
                                        "hash INTEGER NOT NULL, tag INTEGER NOT NULL, PRIMARY KEY (hash, tag)"
                                        ") WITHOUT ROWID;");
  if (ret == SQLITE_OK) {
    ret = sqlite3_prepare_v3(g_pipelineCacheDb, "INSERT OR IGNORE INTO pipeline_tags (hash, tag) VALUES (?, ?)", -1,
                             SQLITE_PREPARE_PERSISTENT, &g_pipelineTagStmt, nullptr);
  }
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare pipeline_tags table: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }
#endif

#ifndef __EMSCRIPTEN__
  seed_pipeline_cache();
  if (g_pipelineCacheBroken) {
    return false;
  }
#endif

  return true;
}

#ifdef __EMSCRIPTEN__
// Once per boot: a database that failed to open or read starts again empty.
static bool recreate_pipeline_cache_db(const char* why) {
  static bool recreated = false;
  if (recreated) {
    return false;
  }
  recreated = true;
  pipeline_cache_abort();
  remove_pipeline_cache_db(why);
  g_pipelineCacheBroken = false;
  return open_pipeline_cache_db();
}
#endif

static bool prepare_pipeline_cache_db() {
  if (open_pipeline_cache_db()) {
    return true;
  }
#ifdef __EMSCRIPTEN__
  return recreate_pipeline_cache_db("it could not be opened");
#else
  return false;
#endif
}

static void prune_old_pipeline_cache_versions() {
  if (!prepare_pipeline_cache_db()) {
    return;
  }

  const auto clearDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                       underlying(ShaderType::Clear), clear::ClearPipelineConfigVersion);
  auto ret = sqlite::exec(g_pipelineCacheDb, clearDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune clear pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }

  const auto gxDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                    underlying(ShaderType::GX), gx::GXPipelineConfigVersion);
  ret = sqlite::exec(g_pipelineCacheDb, gxDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune GX pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }

#ifdef AURORA_ENABLE_RMLUI
  const auto rmlDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                     underlying(ShaderType::Rml), rmlui::RmlPipelineConfigVersion);
  ret = sqlite::exec(g_pipelineCacheDb, rmlDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune RmlUi pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }
#endif

  if constexpr (DeviceBuiltRowsOnly) {
    /* Marks whose row a version bump just deleted first, then every row
     * this device has not built. */
    ret = sqlite::exec(g_pipelineCacheDb,
                       "DELETE FROM pipeline_device WHERE NOT EXISTS (SELECT 1 FROM pipeline_cache c "
                       "WHERE c.type = pipeline_device.type AND c.hash = pipeline_device.hash);"
                       "DELETE FROM pipeline_cache WHERE NOT EXISTS (SELECT 1 FROM pipeline_device d "
                       "WHERE d.type = pipeline_cache.type AND d.hash = pipeline_cache.hash);");
    if (ret != SQLITE_OK) {
      Log.error("Failed to prune pipeline rows this device has not built: {}", sqlite3_errmsg(g_pipelineCacheDb));
      pipeline_cache_abort();
      return;
    }
    if (const auto dropped = sqlite3_changes(g_pipelineCacheDb); dropped > 0) {
      Log.info("Dropped {} pipeline cache rows this device has not built", dropped);
    }
  }
}

static bool write_pipeline_cache_record(const PipelineCacheWrite& write) {
  const auto fail = [&]() {
    sqlite3_reset(g_pipelineCacheUpsertStmt);
    sqlite3_clear_bindings(g_pipelineCacheUpsertStmt);
    return false;
  };

  auto ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 1, underlying(write.type));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache type: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int64(g_pipelineCacheUpsertStmt, 2, static_cast<sqlite3_int64>(write.hash));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache hash: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 3, static_cast<int>(write.configVersion));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config version: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 4, static_cast<int>(write.config.size()));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config size: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_blob64(g_pipelineCacheUpsertStmt, 5, write.config.data(), write.config.size(), SQLITE_TRANSIENT);
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config blob: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int64(g_pipelineCacheUpsertStmt, 6, static_cast<sqlite3_int64>(write.firstFrameUsed));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache first-frame-used: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  ret = sqlite3_step(g_pipelineCacheUpsertStmt);
  if (ret != SQLITE_DONE) {
    Log.error("Failed to upsert pipeline cache row: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  sqlite3_reset(g_pipelineCacheUpsertStmt);
  sqlite3_clear_bindings(g_pipelineCacheUpsertStmt);
  return true;
}

static bool write_device_built_record(const PipelineCacheWrite& write) {
  int ret = sqlite3_bind_int(g_pipelineDeviceStmt, 1, underlying(write.type));
  if (ret == SQLITE_OK) {
    ret = sqlite3_bind_int64(g_pipelineDeviceStmt, 2, static_cast<sqlite3_int64>(write.hash));
  }
  if (ret == SQLITE_OK) {
    ret = sqlite3_step(g_pipelineDeviceStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
  }
  if (ret != SQLITE_OK) {
    Log.error("Failed to mark pipeline as built on this device: {}", sqlite3_errmsg(g_pipelineCacheDb));
  }
  sqlite3_reset(g_pipelineDeviceStmt);
  sqlite3_clear_bindings(g_pipelineDeviceStmt);
  return ret == SQLITE_OK;
}

static bool write_tag_record(const PipelineCacheWrite& write) {
  int ret = sqlite3_bind_int64(g_pipelineTagStmt, 1, static_cast<sqlite3_int64>(write.hash));
  if (ret == SQLITE_OK) {
    ret = sqlite3_bind_int64(g_pipelineTagStmt, 2, static_cast<sqlite3_int64>(write.tag));
  }
  if (ret == SQLITE_OK) {
    ret = sqlite3_step(g_pipelineTagStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
  }
  if (ret != SQLITE_OK) {
    Log.error("Failed to tag a pipeline with its match: {}", sqlite3_errmsg(g_pipelineCacheDb));
  }
  sqlite3_reset(g_pipelineTagStmt);
  sqlite3_clear_bindings(g_pipelineTagStmt);
  return ret == SQLITE_OK;
}

static void pipeline_cache_writer() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline cache writer thread");
#endif

  while (true) {
    std::deque<PipelineCacheWrite> batch;
    {
      std::unique_lock lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterCv.wait(lock,
                                   [] { return g_pipelineCacheWriterStop || !g_pipelineCacheWriteQueue.empty(); });
      if (g_pipelineCacheWriterStop && g_pipelineCacheWriteQueue.empty()) {
        return;
      }
      batch.swap(g_pipelineCacheWriteQueue);
      g_pipelineCacheWriterBusy = true;
    }

    bool writeFailed = false;
    {
      sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
      if (!tx) {
        Log.error("Failed to begin pipeline cache write transaction");
        writeFailed = true;
      } else {
        for (const auto& write : batch) {
          const bool written = write.tagRow        ? write_tag_record(write)
                               : write.deviceBuilt ? write_device_built_record(write)
                                                   : write_pipeline_cache_record(write);
          if (!written) {
            writeFailed = true;
            break;
          }
        }

        if (!writeFailed) {
          tx.commit();
        }
      }
    }

    if (writeFailed) {
      pipeline_cache_abort();
    }
    {
      std::lock_guard lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterBusy = false;
    }
    g_pipelineCacheWriterIdleCv.notify_all();
    if (writeFailed) {
      return;
    }
  }
}

static void build_pending_pipeline(PendingPipeline& pending) {
  auto result = create_timed(pending.create);
  {
    std::lock_guard lock{g_pipelineMutex};
    g_pipelines.try_emplace(pending.hash, CachedPipeline{std::move(result)});
    g_pendingPipelines.erase(pending.hash);
  }
  notify_pipeline_ready(true);
}

static void pipeline_worker() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline compilation thread");
#endif
  if (g_hasPipelineThread && !g_pipelineSync) {
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
  }

  while (g_hasPipelineThread || g_pipelinesPerFrame < BuildPipelinesPerFrame) {
    PendingPipeline pending;
    {
      std::unique_lock lock{g_pipelineMutex};
      if (g_hasPipelineThread) {
        // Several workers share the queues, so always re-check under the lock
        // rather than trusting a "has more" observed before it was released.
        g_pipelineQueueCv.wait(
            lock, [] { return !g_pipelineQueue.empty() || !g_backgroundPipelineQueue.empty() || g_pipelineThreadEnd; });
      } else if (g_pipelineQueue.empty()) {
        // On platforms without a background compilation thread (Android, WebGPU),
        // only process pipelines actively queued by the current frame (g_pipelineQueue).
        // Never stall the presentation loop compiling background pipelines; those
        // are built by wait_pipelines when the host has time to spare (the launcher).
        return;
      }
      if (g_pipelineThreadEnd) {
        break;
      }
      if (!g_hasPipelineThread || !g_pipelineQueue.empty()) {
        pending = std::move(g_pipelineQueue.front());
        g_pipelineQueue.pop_front();
      } else {
        pending = pop_background();
      }
    }
    build_pending_pipeline(pending);
    if (!g_hasPipelineThread) {
      ++g_pipelinesPerFrame;
    }
  }
}

#ifdef __EMSCRIPTEN__
static uint32_t browser_compile_cap() {
  if (g_browserCompileCap != 0) {
    return g_browserCompileCap;
  }
  // MELEE_PIPELINES_INFLIGHT=<n> at boot, for measuring; otherwise by browser.
  static const uint32_t byBrowser = [] {
    if (const char* v = std::getenv("MELEE_PIPELINES_INFLIGHT"); v != nullptr && std::atoi(v) > 0) {
      return static_cast<uint32_t>(std::atoi(v));
    }
    return browser_is_webkit() != 0 ? 2u : 6u;
  }();
  return byBrowser;
}

/* Starts queued compiles while fewer than the cap are in flight: every
 * pipeline a draw asked for first, the known configs (seed and device rows)
 * only when nothing a draw wants is waiting, and only while the host allows
 * them. Cheap when nothing is queued or the cap is full. A lost device is
 * asked for nothing. */
static void browser_pump() {
  if (webgpu::device_lost()) {
    return;
  }
  const uint32_t cap = browser_compile_cap();
  while (aurora_browser_pipelines_compiling() < cap) {
    PendingPipeline pending;
    {
      std::lock_guard lock{g_pipelineMutex};
      if (!g_pipelineQueue.empty()) {
        pending = std::move(g_pipelineQueue.front());
        g_pipelineQueue.pop_front();
      } else {
        if (!g_browserBackgroundOn) {
          return;
        }
        while (!g_backgroundPipelineQueue.empty() && g_backgroundPipelineQueue.front().fog && !gx::fog_enabled()) {
          g_backgroundFogParked.emplace_back(pop_background());
        }
        if (g_backgroundPipelineQueue.empty()) {
          return;
        }
        pending = pop_background();
      }
    }
    build_pending_pipeline(pending);
  }
}
#endif

template <typename PipelineConfig>
static size_t load_pipeline_cache_entries(ShaderType type, uint32_t configVersion) {
  if (!prepare_pipeline_cache_db()) {
    return 0;
  }

  auto ret = sqlite3_bind_int(g_pipelineCacheLoadStmt, 1, underlying(type));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache load type: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return 0;
  }
  ret = sqlite3_bind_int(g_pipelineCacheLoadStmt, 2, static_cast<int>(configVersion));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache load config version: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return 0;
  }

  size_t acceptedRows = 0;
  while ((ret = sqlite3_step(g_pipelineCacheLoadStmt)) == SQLITE_ROW) {
    const auto* configBlob = static_cast<const uint8_t*>(sqlite3_column_blob(g_pipelineCacheLoadStmt, 0));
    const auto configSize = sqlite3_column_bytes(g_pipelineCacheLoadStmt, 0);
    const auto firstFrameUsed = static_cast<uint32_t>(sqlite3_column_int64(g_pipelineCacheLoadStmt, 1));
    if (configSize != static_cast<int>(sizeof(PipelineConfig)) || (configSize != 0 && configBlob == nullptr)) {
      continue;
    }

    PipelineConfig config;
    std::memcpy(&config, configBlob, sizeof(config));
    if (config.version != configVersion) {
      continue;
    }

    remember_pipeline_config(type, config, firstFrameUsed, false);
    if constexpr (DeviceBuiltRowsOnly) {
      // Every row left after the load-time prune is marked already.
      std::lock_guard lock{g_deviceBuiltMutex};
      g_deviceBuilt.insert(xxh3_hash(config, static_cast<HashType>(type)));
    }
    ++acceptedRows;
  }

  if (ret != SQLITE_DONE) {
    Log.error("Failed to read pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
  }

  sqlite3_reset(g_pipelineCacheLoadStmt);
  sqlite3_clear_bindings(g_pipelineCacheLoadStmt);
  return acceptedRows;
}

static size_t load_device_pipeline_cache() {
  if (!prepare_pipeline_cache_db()) {
    return 0;
  }
  prune_old_pipeline_cache_versions();
  if (g_pipelineCacheBroken) {
    return 0;
  }

  size_t acceptedRows = 0;
#ifdef AURORA_ENABLE_RMLUI
  acceptedRows += load_pipeline_cache_entries<rmlui::PipelineConfig>(ShaderType::Rml, rmlui::RmlPipelineConfigVersion);
#endif
  acceptedRows +=
      load_pipeline_cache_entries<clear::PipelineConfig>(ShaderType::Clear, clear::ClearPipelineConfigVersion);
  acceptedRows += load_pipeline_cache_entries<gx::PipelineConfig>(ShaderType::GX, gx::GXPipelineConfigVersion);
#ifdef __EMSCRIPTEN__
  if (g_pipelineCacheBroken) {
    return acceptedRows;
  }
  size_t tags = 0;
  const auto ret = sqlite::exec(g_pipelineCacheDb, "SELECT hash, tag FROM pipeline_tags;",
                                [&tags](int argc, char** argv, char**) {
                                  if (argc < 2 || argv[0] == nullptr || argv[1] == nullptr) {
                                    return;
                                  }
                                  const auto hash = static_cast<HashType>(std::strtoll(argv[0], nullptr, 10));
                                  const auto tag = static_cast<uint32_t>(std::strtoll(argv[1], nullptr, 10));
                                  g_tagConfigs[tag].insert(hash);
                                  ++tags;
                                });
  if (ret != SQLITE_OK) {
    Log.warn("Failed to read pipeline tags: {}", sqlite3_errmsg(g_pipelineCacheDb));
  } else if (tags != 0) {
    Log.info("Read {} pipeline tags for {} matches' fighters and stages", tags, g_tagConfigs.size());
  }
#endif
  return acceptedRows;
}

#ifdef __EMSCRIPTEN__
template <typename PipelineConfig>
static bool remember_seed_row(ShaderType type, uint32_t configVersion, const uint8_t* blob, int size,
                              uint32_t firstFrameUsed) {
  if (size != static_cast<int>(sizeof(PipelineConfig)) || blob == nullptr) {
    return false;
  }
  PipelineConfig config;
  std::memcpy(&config, blob, sizeof(config));
  if (config.version != configVersion) {
    return false;
  }
  remember_pipeline_config(type, config, firstFrameUsed, false);
  return true;
}

/* The browser reads the shipped seed on every boot, after this device's rows
 * and their prune, straight into the known configs. It is never merged into
 * the device database: the merge was skipped whenever the table had a row,
 * and it ran before the prune, so a config version bump would have left a
 * phone with neither. */
static size_t load_pipeline_cache_seed() {
  const auto seedPath = pipeline_cache_seed_path();
  const auto besideExe = pipeline_cache_seed_beside_exe_path();
  std::string usedPath = seedPath;
  sqlite3* seedDb = open_pipeline_cache_seed_db(seedPath);
  if (seedDb == nullptr && besideExe != seedPath) {
    seedDb = open_pipeline_cache_seed_db(besideExe);
    usedPath = besideExe;
  }
  if (seedDb == nullptr) {
    Log.info("No bundled initial pipeline cache found at '{}' or '{}'", seedPath, besideExe);
    return 0;
  }

  sqlite3_stmt* stmt = nullptr;
  size_t accepted = 0;
  size_t skipped = 0;
  auto ret = sqlite3_prepare_v3(seedDb,
                                "SELECT type, config_version, config, first_frame_used FROM pipeline_cache "
                                "ORDER BY first_frame_used ASC, rowid ASC",
                                -1, 0, &stmt, nullptr);
  if (ret == SQLITE_OK) {
    while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
      const int type = sqlite3_column_int(stmt, 0);
      const int version = sqlite3_column_int(stmt, 1);
      const auto* blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 2));
      const int size = sqlite3_column_bytes(stmt, 2);
      const auto used = std::clamp<sqlite3_int64>(sqlite3_column_int64(stmt, 3), 0, UINT32_MAX);
      bool ok = false;
      if (type == underlying(ShaderType::GX) && version == static_cast<int>(gx::GXPipelineConfigVersion)) {
        ok = remember_seed_row<gx::PipelineConfig>(ShaderType::GX, gx::GXPipelineConfigVersion, blob, size,
                                                   static_cast<uint32_t>(used));
      } else if (type == underlying(ShaderType::Clear) &&
                 version == static_cast<int>(clear::ClearPipelineConfigVersion)) {
        ok = remember_seed_row<clear::PipelineConfig>(ShaderType::Clear, clear::ClearPipelineConfigVersion, blob,
                                                      size, static_cast<uint32_t>(used));
      }
      ok ? ++accepted : ++skipped;
    }
  }
  if (ret != SQLITE_DONE) {
    Log.warn("Failed to read bundled pipeline cache rows from '{}': {}", usedPath, sqlite3_errmsg(seedDb));
  }
  if (stmt != nullptr) {
    sqlite3_finalize(stmt);
  }
  sqlite3_close(seedDb);
  Log.info("Read the pipeline cache seed '{}': {} rows, {} skipped", usedPath, accepted, skipped);
  return accepted;
}
#endif

static size_t load_pipeline_cache() {
  size_t acceptedRows = load_device_pipeline_cache();
#ifdef __EMSCRIPTEN__
  if (g_pipelineCacheBroken && recreate_pipeline_cache_db("its rows could not be read")) {
    acceptedRows += load_device_pipeline_cache();
  }
  acceptedRows += load_pipeline_cache_seed();
#endif
  return acceptedRows;
}

static void start_pipeline_cache_writer() {
  if (!prepare_pipeline_cache_db()) {
    return;
  }

  g_pipelineCacheWriterStop = false;
  g_pipelineCacheWriterThread = std::thread(pipeline_cache_writer);
}

static void stop_pipeline_cache_writer() {
  if (g_pipelineCacheWriterThread.joinable()) {
    {
      std::lock_guard lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterStop = true;
    }
    g_pipelineCacheWriterCv.notify_one();
    g_pipelineCacheWriterThread.join();
  } else {
    g_pipelineCacheWriterStop = false;
  }

  g_pipelineCacheWriteQueue.clear();
}

PipelineRef find_pipeline(const clear::PipelineConfig& config, const RenderTargetLayout& layout) {
  remember_pipeline_config(ShaderType::Clear, config, current_frame(), true);
  return resolve_pipeline(ShaderType::Clear, config, layout, PipelinePriority::Normal);
}

/* A GX draw whose pipeline is still compiling is skipped (Wiicompiled's
 * approach): the geometry is missing for the few frames the compile takes,
 * and the frame never stalls (issue #46). Once compiled it lands in
 * g_pipelines and the next frame binds it. This used to block instead, because
 * with a single low-throughput compile thread a short-lived screen (the
 * Classic team-intro splash, ten fresh variants in about a second) lost all
 * its tiles; the worker pool makes that window a few frames wide. MELEE_PIPELINE_SYNC
 * restores the blocking behaviour for A/B. */
PipelineRef find_pipeline(const gx::PipelineConfig& config, const RenderTargetLayout& layout) {
  remember_pipeline_config(ShaderType::GX, config, current_frame(), true);
  return resolve_pipeline(ShaderType::GX, config, layout,
                          g_pipelineSync ? PipelinePriority::Blocking : PipelinePriority::Normal);
}

#ifdef AURORA_ENABLE_RMLUI
PipelineRef find_pipeline(const rmlui::PipelineConfig& config) {
  remember_pipeline_config(ShaderType::Rml, config, 0, true);
  return resolve_pipeline(ShaderType::Rml, config, {}, PipelinePriority::Blocking);
}
#endif

void rebuild_pipeline_cache() {
  ZoneScoped;
  const auto scene = scene_render_target_layout();
  auto offscreen = scene;
  offscreen.colorAttachmentCount = 1;
  offscreen.sampleCount = 1;
  detail::finalize_render_target_layout(offscreen);
  g_pipelineLayoutKey = scene.key;

  std::vector<KnownPipeline> known;
  {
    std::lock_guard lock{g_pipelineMutex};
    known.reserve(g_knownPipelines.size());
    for (const auto& [hash, pipeline] : g_knownPipelines) {
      known.push_back(pipeline);
    }
  }
  std::ranges::sort(known, {}, &KnownPipeline::firstFrameUsed);
  /* Rare (startup, MSAA or normal-buffer changes) and it re-queues every
   * known config, so it belongs next to the build counts. */
  Log.info("Queueing {} known pipeline configs for render target layout {:016x}", known.size(), scene.key);
  for (const auto& pipeline : known) {
    std::visit(
        [&](const auto& config) {
          resolve_pipeline(pipeline.type, config, scene, PipelinePriority::Background);
          if (offscreen.key != scene.key) {
            resolve_pipeline(pipeline.type, config, offscreen, PipelinePriority::Background);
          }
        },
        pipeline.config);
  }
}

void initialize_pipeline_cache() {
  g_pipelineCacheBroken = false;
  g_pipelineCacheWriterStop = false;
  g_pipelineThreadEnd = false;
  g_gpuCachePrunePending = false;

#if defined(__ANDROID__)
  /* Some Android Vulkan drivers crash when pipelines are created off the
   * draw thread. Keep the v0.2 inline path for every GPU. */
  const bool inlineOnly = true;
#else
  const bool inlineOnly = false;
#endif
  if (webgpu::g_backendType == wgpu::BackendType::WebGPU || inlineOnly) {
    g_hasPipelineThread = false;
  } else {
    g_hasPipelineThread = true;
    const unsigned jobs = pipeline_job_count();
    Log.info("Compiling pipelines on {} thread(s){}", jobs, g_pipelineSync ? ", sync mode" : "");
    for (unsigned i = 0; i < jobs; ++i) {
      g_pipelineThreads.emplace_back(pipeline_worker);
    }
  }

  const size_t loadedCount = load_pipeline_cache();
  rebuild_pipeline_cache();
  if (!g_pipelineCacheBroken && loadedCount > 0) {
    g_gpuCachePrunePending = true;
  }

  if (!g_pipelineCacheBroken) {
    start_pipeline_cache_writer();
  }
}

void shutdown_pipeline_cache() {
  if (g_hasPipelineThread) {
    g_pipelineThreadEnd = true;
    g_pipelineQueueCv.notify_all();
    g_pipelineReadyCv.notify_all();
    for (auto& thread : g_pipelineThreads) {
      thread.join();
    }
    g_pipelineThreads.clear();
  }
  g_hasPipelineThread = false;
  log_pipeline_stats();

  stop_pipeline_cache_writer();
  pipeline_cache_abort();
  g_pipelineCacheBroken = false;
  g_pipelinesPerFrame = 0;
  g_gpuCachePrunePending = false;
  g_pipelines.clear();
  g_knownPipelines.clear();
  g_pipelineLayoutKey.reset();
  g_pipelineQueue.clear();
  g_backgroundPipelineQueue.clear();
  g_backgroundPending.clear();
  g_pendingPipelines.clear();
  g_deviceBuilt.clear();
#ifdef __EMSCRIPTEN__
  g_backgroundFogParked.clear();
  g_browserWanted.clear();
  g_browserWantedAfterGo.clear();
  g_browserAfterGo = false;
  g_browserUnbuiltRows.clear();
  g_browserBuiltKeys.clear();
  g_matchTags.clear();
  g_matchTagged.clear();
  g_tagConfigs.clear();
#endif
  g_drawsSkipped = 0;

  queuedPipelines = 0;
  createdPipelines = 0;
  g_statCreated = 0;
  g_statCreateNs = 0;
  g_statCreateMaxNs = 0;
  g_demandedPipelines.clear();
}

void begin_pipeline_frame() {
  if (!g_hasPipelineThread) {
    g_pipelinesPerFrame = 0;
  }
  if (g_pipelineLayoutKey != scene_render_target_layout().key) {
    rebuild_pipeline_cache();
  }
  /* Every ten seconds at 60 Hz, and only when something was built: a
   * scripted run's last line is its total even if it is killed. */
  static uint32_t frames = 0;
  static uint32_t loggedCreated = 0;
  if (++frames % 600 == 0 && g_statCreated.load() != loggedCreated) {
    loggedCreated = g_statCreated.load();
    log_pipeline_stats();
  }
}

#ifdef __EMSCRIPTEN__
// The legacy switch (MELEE_PIPELINES_BACKGROUND hosts): any number above 0 lets
// the known configs build behind the frames, at the shared cap.
extern "C" void aurora_background_pipelines(uint32_t max_in_flight) { g_browserBackgroundOn = max_in_flight != 0; }

extern "C" uint32_t aurora_background_pipelines_left() {
  std::lock_guard lock{g_pipelineMutex};
  return static_cast<uint32_t>(g_backgroundPipelineQueue.size());
}

/* The known configs (the seed and this device's rows) used to be built here
 * before simulation, in 16 ms slices that held the page: 47 s on an iPhone 11,
 * which lost its GPU process soon after, and a frozen pick screen on Chromium.
 * Now they only queue: browser_pump builds them behind the frames, at the cap,
 * while the host allows background builds, and a match's own pipelines always
 * go first. */
extern "C" void browser_prepare_graphics() {
  const uint32_t total = wait_pipelines(0);
  if (total != 0) {
    Log.info("Building {} cached browser pipelines in the background, {} at a time", total, browser_compile_cap());
  }
  browser_graphics_progress(total, total); // nothing to wait for
}
#endif

void end_pipeline_frame() {
  if (!g_hasPipelineThread) {
#ifdef __EMSCRIPTEN__
    browser_pump();
#else
    pipeline_worker();
#endif
  }
}

bool get_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline) {
  std::lock_guard guard{g_pipelineMutex};
  const auto it = g_pipelines.find(ref);
  if (it == g_pipelines.end()) {
    note_skipped_draw(ref);
    return false;
  }
#ifdef __EMSCRIPTEN__
  // Newly encountered effects compile off the GPU submission path: only their
  // unready draws are skipped.
  if (!it->second.pipeline && !gx::take_browser_pipeline(ref, it->second.pipeline)) {
    note_skipped_draw(ref);
    return false;
  }
#endif
  pipeline = it->second.pipeline;
  return true;
}

#ifdef __EMSCRIPTEN__
void browser_pipeline_settled(PipelineRef ref, HashType cacheKey, bool ok) {
  std::optional<PipelineCacheWrite> row;
  {
    std::lock_guard lock{g_pipelineMutex};
    g_browserWanted.erase(ref);
    if (ok) {
      g_browserBuiltKeys.insert(cacheKey);
    }
    if (const auto it = g_browserUnbuiltRows.find(cacheKey); it != g_browserUnbuiltRows.end()) {
      if (ok) {
        row = std::move(it->second);
      }
      g_browserUnbuiltRows.erase(it);
    }
  }
  if (row) {
    enqueue_pipeline_cache_write(std::move(*row));
  }
}
#endif

uint32_t wait_pipelines(uint32_t maxWaitMs) {
#ifdef __EMSCRIPTEN__
  /* The browser never compiles on this thread's time: the gate starts what the
   * cap allows and the count is what is still queued. */
  (void)maxWaitMs;
  browser_pump();
  std::lock_guard lock{g_pipelineMutex};
  return static_cast<uint32_t>(g_pendingPipelines.size());
#else
  if (!g_hasPipelineThread) {
    /* No worker pool: this thread is the only one that builds pipelines (some
     * Android drivers fail vkCreateGraphicsPipelines off the draw thread), and
     * nothing builds background rows during frames. So spend the caller's
     * idle time building them here, one at a time until the budget is gone;
     * the first always starts, so a build longer than the budget still
     * makes progress. */
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{maxWaitMs};
    while (maxWaitMs != 0 && std::chrono::steady_clock::now() < deadline) {
      PendingPipeline pending;
      {
        std::lock_guard lock{g_pipelineMutex};
        if (!g_pipelineQueue.empty()) {
          pending = std::move(g_pipelineQueue.front());
          g_pipelineQueue.pop_front();
        } else if (!g_backgroundPipelineQueue.empty()) {
          pending = pop_background();
        } else {
          break;
        }
      }
      build_pending_pipeline(pending);
    }
  }
  std::unique_lock lock{g_pipelineMutex};
  if (g_hasPipelineThread && maxWaitMs != 0) {
    g_pipelineReadyCv.wait_for(lock, std::chrono::milliseconds{maxWaitMs},
                               [] { return g_pendingPipelines.empty() || g_pipelineThreadEnd; });
  }
  return static_cast<uint32_t>(g_pendingPipelines.size());
#endif
}

} // namespace aurora::gfx

uint32_t aurora_wait_pipelines(uint32_t max_wait_ms) { return aurora::gfx::wait_pipelines(max_wait_ms); }

/* The host's handles on the compile gate (src/pc/eggy). Defined on every
 * platform; off the browser most are no-ops. The page calls the flush and
 * reads the counts itself, so those stay exported. */
using namespace aurora::gfx;

#ifdef __EMSCRIPTEN__
#define AURORA_PAGE_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define AURORA_PAGE_EXPORT
#endif

extern "C" void aurora_pump_pipelines(void) {
#ifdef __EMSCRIPTEN__
  browser_pump();
#endif
}

extern "C" void aurora_set_compile_cap(uint32_t n) {
#ifdef __EMSCRIPTEN__
  g_browserCompileCap = n;
  browser_pump();
#else
  (void)n;
#endif
}

extern "C" void aurora_set_background_builds(bool on) {
#ifdef __EMSCRIPTEN__
  g_browserBackgroundOn = on;
  if (on) {
    browser_pump();
  }
#else
  (void)on;
#endif
}

extern "C" AURORA_PAGE_EXPORT uint32_t aurora_demanded_pipelines_pending(void) {
#ifdef __EMSCRIPTEN__
  std::lock_guard lock{g_pipelineMutex};
  return static_cast<uint32_t>(g_browserWanted.size());
#else
  return 0;
#endif
}

extern "C" AURORA_PAGE_EXPORT uint32_t aurora_draws_skipped(void) { return g_drawsSkipped.load(); }

extern "C" void aurora_mark_go(void) {
#ifdef __EMSCRIPTEN__
  std::lock_guard lock{g_pipelineMutex};
  g_browserAfterGo = true;
  g_browserWantedAfterGo.clear();
#endif
}

extern "C" AURORA_PAGE_EXPORT uint32_t aurora_demanded_after_go(void) {
#ifdef __EMSCRIPTEN__
  std::lock_guard lock{g_pipelineMutex};
  return static_cast<uint32_t>(g_browserWantedAfterGo.size());
#else
  return 0;
#endif
}

extern "C" void aurora_set_fog_enabled(bool on) {
  aurora::gx::set_fog_enabled(on);
#ifdef __EMSCRIPTEN__
  if (on) {
    std::lock_guard lock{g_pipelineMutex};
    while (!g_backgroundFogParked.empty()) {
      queue_background(std::move(g_backgroundFogParked.front()));
      g_backgroundFogParked.pop_front();
    }
  }
#endif
}

/* Waits, up to half a second, until every row asked for has been committed,
 * so the page's FS.syncfs right after persists a whole database and never one
 * torn mid-transaction. 0: written; 1: still writing; -1: no database. */
extern "C" AURORA_PAGE_EXPORT int aurora_pipeline_cache_flush(void) {
  if (g_pipelineCacheBroken || g_pipelineCacheDb == nullptr || !g_pipelineCacheWriterThread.joinable()) {
    return -1;
  }
  std::unique_lock lock{g_pipelineCacheWriterMutex};
  const bool drained = g_pipelineCacheWriterIdleCv.wait_for(lock, std::chrono::milliseconds{500}, [] {
    return (g_pipelineCacheWriteQueue.empty() && !g_pipelineCacheWriterBusy) || g_pipelineCacheWriterStop;
  });
  if (g_pipelineCacheBroken) {
    return -1;
  }
  return drained ? 0 : 1;
}

/* This match's tags (opaque to aurora; the host's fighters and stage): every
 * GX config a draw asks for from now on is recorded under each of them. */
extern "C" void aurora_set_pipeline_tags(const uint32_t* tags, uint32_t count) {
#ifdef __EMSCRIPTEN__
  std::lock_guard lock{g_pipelineMutex};
  g_matchTags.assign(tags, tags + (tags != nullptr ? count : 0));
  g_matchTagged.clear();
#else
  (void)tags;
  (void)count;
#endif
}

/* Asks, as a draw would, for every known config an earlier match sharing a tag
 * with this one needed, so they compile ahead of the background builds and
 * count as wanted until they have: the host's graphics-ready waits for them. */
extern "C" void aurora_prebuild_tagged(void) {
#ifdef __EMSCRIPTEN__
  const auto scene = scene_render_target_layout();
  std::vector<aurora::gx::PipelineConfig> configs;
  {
    std::lock_guard lock{g_pipelineMutex};
    absl::flat_hash_set<aurora::HashType> seen;
    for (const uint32_t tag : g_matchTags) {
      const auto tagIt = g_tagConfigs.find(tag);
      if (tagIt == g_tagConfigs.end()) {
        continue;
      }
      for (const aurora::HashType key : tagIt->second) {
        const auto it = g_knownPipelines.find(key);
        if (!seen.insert(key).second || it == g_knownPipelines.end()) {
          continue;
        }
        if (const auto* config = std::get_if<aurora::gx::PipelineConfig>(&it->second.config);
            config != nullptr && (aurora::gx::fog_enabled() || config->shaderConfig.fogType == GX_FOG_NONE)) {
          configs.push_back(*config);
        }
      }
    }
  }
  for (const auto& config : configs) {
    resolve_pipeline(ShaderType::GX, config, scene, PipelinePriority::Normal);
  }
  if (!configs.empty()) {
    Log.info("Prebuilding {} pipelines matches with these fighters and stage needed before", configs.size());
  }
#endif
}

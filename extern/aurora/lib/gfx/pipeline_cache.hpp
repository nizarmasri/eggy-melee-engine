#pragma once

#include "types.hpp"

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::rmlui {
struct PipelineConfig;
} // namespace aurora::rmlui

namespace aurora::gfx {

enum class ShaderType : uint8_t {
  Clear = 0,
  GX = 1,
  Rml = 2,
};

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
void begin_pipeline_frame();
void end_pipeline_frame();
void rebuild_pipeline_cache();

PipelineRef find_pipeline(const gx::PipelineConfig& config, const RenderTargetLayout& layout);
PipelineRef find_pipeline(const clear::PipelineConfig& config, const RenderTargetLayout& layout);
PipelineRef find_pipeline(const rmlui::PipelineConfig& config);

bool get_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);
/* Blocks up to maxWaitMs while queued pipelines compile; returns how many are
 * still pending (queued or mid-compile). */
uint32_t wait_pipelines(uint32_t maxWaitMs);

#ifdef __EMSCRIPTEN__
/* A browser compilation finished (gx.cpp build_pipeline): ok, or refused. The
 * runtime ref is the cache's key for the compiled pipeline; cacheKey names its
 * config, the device database's row. */
void browser_pipeline_settled(PipelineRef ref, HashType cacheKey, bool ok);
#endif

} // namespace aurora::gfx

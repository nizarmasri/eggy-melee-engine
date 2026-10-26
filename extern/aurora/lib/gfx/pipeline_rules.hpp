#pragma once

/* The browser pipeline cache's pure rules (pipeline_cache.cpp): which configs
 * every match needs, the order the known ones build in behind the frames, what
 * the draws after GO asked for, and the pump that keeps the compile cap full.
 * No GPU, no database and no absl, so tests/eggy/eggy_pipeline_rules_test.cpp
 * builds it with the host compiler. */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace aurora::gfx::rules {

/* A match's tags (the host's: its stage, fighters and items), sorted, each
 * once. */
using TagSet = std::vector<uint32_t>;

inline TagSet tag_set(const uint32_t* tags, size_t count) {
  TagSet set;
  if (tags != nullptr) {
    set.assign(tags, tags + count);
  }
  std::sort(set.begin(), set.end());
  set.erase(std::unique(set.begin(), set.end()), set.end());
  return set;
}

/* What every match that needed a config had in common: the first match's tags,
 * and each match after keeps only the ones it shares. A match always has its
 * stage, so a meet left empty means two matches with nothing in common both
 * needed the config: it is common (hit sparks, shields, KO, READY/GO, the HUD),
 * and every match builds it before GO. One fighter's or one stage's own configs
 * keep that tag however many matches need them, so they never become common,
 * which "seen in two matches" alone would make of every fighter played twice. */
struct Meet {
  bool seen = false;
  TagSet tags;
};

/* One more match needed the config. Returns whether the meet changed (a row
 * to write). A match with no tags says nothing. */
inline bool fold(Meet& meet, const TagSet& match) {
  if (match.empty()) {
    return false;
  }
  if (!meet.seen) {
    meet.seen = true;
    meet.tags = match;
    return true;
  }
  TagSet kept;
  std::set_intersection(meet.tags.begin(), meet.tags.end(), match.begin(), match.end(), std::back_inserter(kept));
  if (kept.size() == meet.tags.size()) {
    return false;
  }
  meet.tags = std::move(kept);
  return true;
}

inline bool common(const Meet& meet) { return meet.seen && meet.tags.empty(); }

/* A meet's tags as the device database keeps them (pipeline_meet.tags): four
 * little-endian bytes each, none for a common config. */
constexpr size_t MaxMeetTags = 64;

inline std::vector<uint8_t> encode(const TagSet& tags) {
  std::vector<uint8_t> bytes;
  bytes.reserve(tags.size() * 4);
  for (const uint32_t tag : tags) {
    for (int shift = 0; shift < 32; shift += 8) {
      bytes.push_back(static_cast<uint8_t>(tag >> shift));
    }
  }
  return bytes;
}

/* False for bytes no meet was written as (the row is skipped, never misread). */
inline bool decode(const uint8_t* bytes, size_t size, TagSet& out) {
  out.clear();
  if (size % 4 != 0 || size > MaxMeetTags * 4 || (size != 0 && bytes == nullptr)) {
    return false;
  }
  for (size_t i = 0; i < size; i += 4) {
    out.push_back(static_cast<uint32_t>(bytes[i]) | (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                  (static_cast<uint32_t>(bytes[i + 2]) << 16) | (static_cast<uint32_t>(bytes[i + 3]) << 24));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return true;
}

/* The order the known configs build in behind the frames: the common ones
 * first, then each by the frame it was first needed in. */
inline bool builds_before(bool commonA, uint32_t firstA, bool commonB, uint32_t firstB) {
  if (commonA != commonB) {
    return commonA;
  }
  return firstA < firstB;
}

/* The pipelines draws asked for and the browser has not compiled yet, and of
 * those first asked for after a match's GO, how many were configs no cache
 * held at all (new to this phone) rather than known ones not built yet (the
 * seed's, this device's rows, or an earlier draw's). */
template <typename Set>
class WantBook {
public:
  using Key = typename Set::value_type;

  /* A draw asked for `ref`; `known`: its config was in a cache before the draw. */
  void want(Key ref, bool known) {
    if (!m_wanted.insert(ref).second || !m_afterGo) {
      return;
    }
    if (m_wantedAfterGo.insert(ref).second && !known) {
      ++m_newAfterGo;
    }
  }
  void settled(Key ref) { m_wanted.erase(ref); }
  void go() {
    m_afterGo = true;
    m_wantedAfterGo.clear();
    m_newAfterGo = 0;
  }
  void clear() {
    m_wanted.clear();
    m_wantedAfterGo.clear();
    m_newAfterGo = 0;
    m_afterGo = false;
  }

  uint32_t pending() const { return static_cast<uint32_t>(m_wanted.size()); }
  uint32_t after_go() const { return static_cast<uint32_t>(m_wantedAfterGo.size()); }
  uint32_t after_go_new() const { return m_newAfterGo; }
  uint32_t after_go_known() const { return after_go() - m_newAfterGo; }

private:
  Set m_wanted;
  Set m_wantedAfterGo;
  uint32_t m_newAfterGo = 0;
  bool m_afterGo = false;
};

/* Keeps the browser's compile cap full. `step` starts one queued compile and
 * says whether it did: false once the cap is full or nothing may start. A
 * settled compile pumps again, so the next one starts at once and not at the
 * next frame's end (a parked engine has ten boundaries a second). A compile
 * may settle inside its own start (CallbackMode::AllowSpontaneous: a lost
 * device or a refused descriptor can answer at once), and that settle's pump
 * finds this one running further up the stack and returns: this one's next
 * step sees the slot it freed. So the stack never holds two pumps, however
 * many settle at once, and nothing is started twice. */
class PumpGate {
public:
  template <typename Step>
  void run(Step&& step) {
    if (m_running) {
      return;
    }
    m_running = true;
    while (step()) {}
    m_running = false;
  }
  bool running() const { return m_running; }

private:
  bool m_running = false;
};

} // namespace aurora::gfx::rules

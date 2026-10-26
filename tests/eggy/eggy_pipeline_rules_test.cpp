/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The browser pipeline cache's pure rules (extern/aurora/lib/gfx/pipeline_rules.hpp):
 * when a config is common to every match, how its meet is stored, the order
 * the known configs build in, the split of what draws asked for after GO, and
 * the pump that keeps the compile cap full without ever nesting. Built with
 * the host compiler by tools/eggy/host_tests.py, which tools/browser/build.py
 * runs first:
 *
 *   g++ -std=c++20 -Wall -Wextra -Werror -Iextern/aurora/lib/gfx \
 *       tests/eggy/eggy_pipeline_rules_test.cpp -o eggy_pipeline_rules_test
 */
#include "pipeline_rules.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <unordered_set>
#include <vector>

using namespace aurora::gfx::rules;

static int failures;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                                    \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

/* eggy.h's tags, as the host sends them. */
constexpr uint32_t Stage = 1u << 24;
constexpr uint32_t Fighter = 2u << 24;
constexpr uint32_t Items = 3u << 24;
enum : uint32_t { Fox = 1, Falco = 22, Marth = 9, Peach = 12, Zelda = 18, Sheik = 19 };
enum : uint32_t { FD = 25, BF = 24, YS = 8 };

static TagSet match(std::vector<uint32_t> tags) { return tag_set(tags.data(), tags.size()); }

static void test_tag_set() {
  const TagSet set = match({Fighter | Fox, Stage | FD, Fighter | Fox, Items});
  CHECK(set.size() == 3);
  CHECK(std::is_sorted(set.begin(), set.end()));
  CHECK(tag_set(nullptr, 4).empty());
}

static void test_common_rule() {
  // A hit spark: needed by two matches that share nothing, so common.
  Meet spark;
  CHECK(!common(spark));
  CHECK(fold(spark, match({Stage | FD, Fighter | Fox, Fighter | Marth})));
  CHECK(!common(spark)); // one match proves nothing
  CHECK(fold(spark, match({Stage | BF, Fighter | Peach, Fighter | Zelda, Fighter | Sheik})));
  CHECK(common(spark));
  CHECK(!fold(spark, match({Stage | YS, Fighter | Falco}))); // common stays common, no row
  CHECK(common(spark));

  // Fox's own: every match it was in had Fox, so it keeps Fox and is never common,
  // which "seen in two matches with different tags" would have made it.
  Meet shine;
  CHECK(fold(shine, match({Stage | FD, Fighter | Fox, Fighter | Marth})));
  CHECK(fold(shine, match({Stage | BF, Fighter | Fox, Fighter | Peach})));
  CHECK(!common(shine));
  CHECK(shine.tags == match({Fighter | Fox}));
  CHECK(!fold(shine, match({Stage | YS, Fighter | Fox, Items}))); // unchanged: no row to write
  CHECK(shine.tags == match({Fighter | Fox}));

  // A stage's own keeps the stage.
  Meet stage;
  fold(stage, match({Stage | FD, Fighter | Fox, Fighter | Marth}));
  fold(stage, match({Stage | FD, Fighter | Peach, Fighter | Falco}));
  CHECK(stage.tags == match({Stage | FD}));
  CHECK(!common(stage));

  // Items on in both: the items' own keep the items tag.
  Meet bomb;
  fold(bomb, match({Stage | FD, Fighter | Fox, Items}));
  fold(bomb, match({Stage | BF, Fighter | Peach, Items}));
  CHECK(bomb.tags == match({Items}));
  CHECK(!common(bomb));

  // A match with no tags says nothing, not "shares nothing".
  Meet quiet;
  CHECK(!fold(quiet, TagSet{}));
  CHECK(!quiet.seen);
  fold(quiet, match({Stage | FD, Fighter | Fox}));
  CHECK(!fold(quiet, TagSet{}));
  CHECK(!common(quiet));
}

static void test_meet_rows() {
  const TagSet tags = match({Stage | FD, Fighter | Fox, Items, 0xDEADBEEFu});
  const auto bytes = encode(tags);
  CHECK(bytes.size() == tags.size() * 4);
  TagSet back;
  CHECK(decode(bytes.data(), bytes.size(), back));
  CHECK(back == tags);
  // Little-endian, four bytes a tag.
  const auto one = encode(TagSet{0x01020304u});
  CHECK(one.size() == 4 && one[0] == 0x04 && one[1] == 0x03 && one[2] == 0x02 && one[3] == 0x01);

  // A common config's row is empty, and reads back common.
  CHECK(encode(TagSet{}).empty());
  Meet read;
  read.seen = true;
  CHECK(decode(nullptr, 0, read.tags));
  CHECK(common(read));

  // Bytes no meet was written as are refused, never misread.
  const uint8_t odd[5] = {1, 2, 3, 4, 5};
  CHECK(!decode(odd, sizeof odd, back));
  CHECK(!decode(nullptr, 4, back));
  std::vector<uint8_t> huge((MaxMeetTags + 1) * 4, 0);
  CHECK(!decode(huge.data(), huge.size(), back));
  // Out of order or repeated on disk: read as the set it is.
  const uint8_t unsorted[12] = {9, 0, 0, 0, 1, 0, 0, 0, 9, 0, 0, 0};
  CHECK(decode(unsorted, sizeof unsorted, back));
  CHECK(back == (TagSet{1, 9}));
}

static void test_build_order() {
  // Common ones first, each part by the frame it was first needed in.
  CHECK(builds_before(true, 900, false, 1));
  CHECK(!builds_before(false, 1, true, 900));
  CHECK(builds_before(true, 1, true, 2));
  CHECK(builds_before(false, 1, false, 2));
  CHECK(!builds_before(false, 2, false, 2));
  struct Row {
    bool common;
    uint32_t first;
  };
  std::vector<Row> rows{{false, 5}, {true, 40}, {false, 1}, {true, 3}};
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return builds_before(a.common, a.first, b.common, b.first); });
  CHECK(rows[0].common && rows[0].first == 3);
  CHECK(rows[1].common && rows[1].first == 40);
  CHECK(!rows[2].common && rows[2].first == 1);
  CHECK(!rows[3].common && rows[3].first == 5);
}

static void test_after_go_split() {
  WantBook<std::unordered_set<uint64_t>> book;
  // Before any GO: wanted, never "after GO" (the hold's own, the prebuild's).
  book.want(1, true);
  book.want(2, false);
  CHECK(book.pending() == 2);
  CHECK(book.after_go() == 0);
  book.go();
  // Still pending from before GO: not first asked for after it.
  book.want(1, true);
  CHECK(book.after_go() == 0);
  // After GO: one known but not built, two new to every cache.
  book.want(10, true);
  book.want(11, false);
  book.want(12, false);
  book.want(11, false); // asked again by the next draw: counted once
  book.want(10, true);
  CHECK(book.pending() == 5);
  CHECK(book.after_go() == 3);
  CHECK(book.after_go_known() == 1);
  CHECK(book.after_go_new() == 2);
  CHECK(book.after_go_known() + book.after_go_new() == book.after_go());
  // Settling takes it from pending, not from what this match asked for.
  book.settled(11);
  book.settled(1);
  CHECK(book.pending() == 3);
  CHECK(book.after_go() == 3);
  // Asked for again after settling (refused, say): still counted once.
  book.want(11, true);
  CHECK(book.after_go() == 3);
  CHECK(book.after_go_new() == 2);
  // The next match's GO starts over.
  book.go();
  CHECK(book.after_go() == 0);
  CHECK(book.after_go_new() == 0);
  book.want(20, false);
  CHECK(book.after_go() == 1 && book.after_go_new() == 1 && book.after_go_known() == 0);
  book.clear();
  CHECK(book.pending() == 0 && book.after_go() == 0);
  book.want(30, false); // after clear(): before any GO again
  CHECK(book.after_go() == 0);
}

/* A browser with a compile cap: start() takes the queue's next and may settle
 * it at once, inside the start (as CallbackMode::AllowSpontaneous allows),
 * which pumps again from inside the pump. */
struct FakeBrowser {
  PumpGate gate;
  std::deque<int> queue;
  std::vector<int> inFlight;
  std::vector<int> started;
  uint32_t cap = 2;
  int depth = 0;
  int deepest = 0;
  int mostInFlight = 0;
  bool settleAtOnce = false;
  bool backgroundOn = true;

  void settle(int id) {
    std::erase(inFlight, id);
    pump(); // what browser_pipeline_settled does, the lock released
  }
  void pump() {
    ++depth;
    deepest = std::max(deepest, depth);
    gate.run([this] {
      if (inFlight.size() >= cap || queue.empty() || !backgroundOn) {
        return false;
      }
      const int id = queue.front();
      queue.pop_front();
      inFlight.push_back(id);
      started.push_back(id);
      mostInFlight = std::max(mostInFlight, static_cast<int>(inFlight.size()));
      if (settleAtOnce) {
        settle(id);
      }
      return true;
    });
    --depth;
  }
};

static void test_pump_gate() {
  // Asynchronous settles: each one starts the next at once, the cap stays full.
  FakeBrowser b;
  for (int i = 0; i < 10; ++i) {
    b.queue.push_back(i);
  }
  b.pump();
  CHECK(b.started.size() == 2);
  CHECK(b.inFlight.size() == 2);
  while (!b.inFlight.empty()) {
    b.settle(b.inFlight.front());
    CHECK(b.inFlight.size() == std::min<size_t>(2, b.queue.size() + b.inFlight.size()));
  }
  CHECK(b.started.size() == 10);
  CHECK(b.mostInFlight == 2);
  CHECK(!b.gate.running());
  for (int i = 0; i < 10; ++i) {
    CHECK(b.started[static_cast<size_t>(i)] == i); // in order, each once
  }

  // Every compile settles inside its own start (a lost device refuses at
  // once): the nested pumps return, the outer one starts everything, once.
  FakeBrowser now;
  now.settleAtOnce = true;
  for (int i = 0; i < 1000; ++i) {
    now.queue.push_back(i);
  }
  now.pump();
  CHECK(now.started.size() == 1000);
  CHECK(now.queue.empty());
  CHECK(now.inFlight.empty());
  CHECK(now.deepest == 2); // the outer pump and one nested call that returned at once
  CHECK(now.mostInFlight == 1);
  CHECK(!now.gate.running());

  // Background builds off: a settle starts nothing.
  FakeBrowser off;
  off.queue = {1, 2, 3};
  off.pump();
  off.backgroundOn = false;
  off.settle(off.inFlight.front());
  CHECK(off.started.size() == 2);
  CHECK(off.inFlight.size() == 1);
  off.backgroundOn = true;
  off.pump();
  CHECK(off.started.size() == 3);
}

int main() {
  test_tag_set();
  test_common_rule();
  test_meet_rows();
  test_build_order();
  test_after_go_split();
  test_pump_gate();
  if (failures != 0) {
    std::fprintf(stderr, "eggy_pipeline_rules_test: %d failed\n", failures);
    return 1;
  }
  std::printf("eggy_pipeline_rules_test: ok\n");
  return 0;
}

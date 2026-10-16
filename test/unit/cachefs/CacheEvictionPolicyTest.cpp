#include "test_utils.h"

#include <CacheEvictionPolicy.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

using cache_fs::eviction::Candidate;
using cache_fs::eviction::PassResult;

namespace {

constexpr size_t kKB = 1024;
constexpr size_t kMB = 1024 * kKB;

Candidate book(const std::string& name, uint32_t seq, bool orphan = false, bool isProtected = false) {
  Candidate c;
  c.name = name;
  c.openSeq = seq;
  c.orphan = orphan;
  c.isProtected = isProtected;
  return c;
}

// Free space grows by each removed book's size; records what was removed.
class FakeTarget final : public cache_fs::eviction::Target {
 public:
  explicit FakeTarget(size_t freeBytes) : free_(freeBytes) {}

  void setSize(const std::string& name, size_t bytes) { sizes_[name] = bytes; }
  void failRemoval(const std::string& name) { failing_[name] = true; }

  size_t freeBytes() override {
    ++measurements;
    return free_;
  }

  bool removeBook(const Candidate& candidate) override {
    removed.push_back(candidate.name);
    if (failing_[candidate.name]) return false;
    free_ += sizes_[candidate.name];
    return true;
  }

  std::vector<std::string> removed;
  int measurements = 0;

 private:
  size_t free_;
  std::map<std::string, size_t> sizes_;
  std::map<std::string, bool> failing_;
};

std::string joined(const std::vector<std::string>& names) {
  std::string out;
  for (const auto& name : names) {
    if (!out.empty()) out += ",";
    out += name;
  }
  return out;
}

std::string orderOf(const std::vector<Candidate>& candidates) {
  std::vector<std::string> names;
  for (const size_t i : cache_fs::eviction::evictionOrder(candidates)) names.push_back(candidates[i].name);
  return joined(names);
}

PassResult runDefault(const std::vector<Candidate>& candidates, FakeTarget& target) {
  return cache_fs::eviction::runPass(candidates, target, cache_fs::eviction::kReserveFreeBytes,
                                     cache_fs::eviction::kTargetFreeBytes);
}

void testNames(TestUtils::TestRunner& runner) {
  using cache_fs::eviction::dirNameHasKey;
  using cache_fs::eviction::isBookCacheDirName;
  runner.expectTrue(isBookCacheDirName("epub_123"), "name_epub");
  runner.expectTrue(isBookCacheDirName("fb2_9"), "name_fb2");
  runner.expectTrue(isBookCacheDirName("txt_18446744073709551615"), "name_txt_64bit_hash");
  runner.expectTrue(isBookCacheDirName("md_1") && isBookCacheDirName("html_1") && isBookCacheDirName("xtc_1"),
                    "name_md_html_xtc");
  runner.expectFalse(isBookCacheDirName("epub_"), "name_rejects_missing_hash");
  runner.expectFalse(isBookCacheDirName("epub_12a"), "name_rejects_non_digit_hash");
  runner.expectFalse(isBookCacheDirName("pdf_123"), "name_rejects_unknown_format");
  runner.expectFalse(isBookCacheDirName("epubx_123"), "name_rejects_prefix_extension");
  runner.expectFalse(isBookCacheDirName("open.seq"), "name_rejects_counter_file");
  runner.expectFalse(isBookCacheDirName("fonts"), "name_rejects_plain_dir");
  runner.expectFalse(isBookCacheDirName("txt_123456789012345678901"), "name_rejects_overlong_hash");

  const std::string key = cache_fs::eviction::bookCacheKey("/Books/a.epub");
  runner.expectEqual(std::to_string(std::hash<std::string>{}("/Books/a.epub")), key, "key_matches_format_cache_path");
  runner.expectTrue(dirNameHasKey("epub_" + key, key), "key_matches_own_dir");
  runner.expectTrue(dirNameHasKey("fb2_" + key, key), "key_matches_any_format");
  runner.expectFalse(dirNameHasKey("epub_1" + key, key), "key_needs_whole_hash");
  runner.expectFalse(dirNameHasKey("epub_123", ""), "empty_key_matches_nothing");
}

void testRecords(TestUtils::TestRunner& runner) {
  using cache_fs::eviction::decodeOpenRecord;
  using cache_fs::eviction::encodeOpenRecord;
  const auto bytes = encodeOpenRecord(42, "/Books/Война и мир.fb2");
  uint32_t seq = 0;
  std::string path;
  runner.expectTrue(decodeOpenRecord(bytes.data(), bytes.size(), &seq, &path), "record_round_trip");
  runner.expectEq(42u, seq, "record_seq");
  runner.expectEqual("/Books/Война и мир.fb2", path, "record_path_utf8");

  const auto counter = encodeOpenRecord(7, "");
  runner.expectTrue(decodeOpenRecord(counter.data(), counter.size(), &seq, &path) && seq == 7 && path.empty(),
                    "record_without_path");

  auto corrupt = bytes;
  corrupt[5] ^= 0x01;
  runner.expectFalse(decodeOpenRecord(corrupt.data(), corrupt.size(), &seq, &path), "record_rejects_bit_flip");
  runner.expectFalse(decodeOpenRecord(bytes.data(), bytes.size() - 1, &seq, &path), "record_rejects_truncation");
  auto badMagic = bytes;
  badMagic[0] = 'X';
  runner.expectFalse(decodeOpenRecord(badMagic.data(), badMagic.size(), &seq, &path), "record_rejects_magic");
  runner.expectFalse(decodeOpenRecord(nullptr, 0, &seq, &path), "record_rejects_empty");

  const std::string longPath(cache_fs::eviction::kMaxRecordPathBytes + 1, 'x');
  const auto truncated = encodeOpenRecord(9, longPath);
  runner.expectTrue(decodeOpenRecord(truncated.data(), truncated.size(), &seq, &path) && seq == 9 && path.empty(),
                    "record_drops_overlong_path");
}

void testOrder(TestUtils::TestRunner& runner) {
  const std::vector<Candidate> books = {book("epub_3", 30), book("fb2_1", 10), book("txt_2", 20)};
  runner.expectEqual("fb2_1,txt_2,epub_3", orderOf(books), "order_least_recently_opened_first");

  const std::vector<Candidate> legacy = {book("epub_3", 30), book("epub_9", 0), book("txt_2", 20)};
  runner.expectEqual("epub_9,txt_2,epub_3", orderOf(legacy), "order_unrecorded_dir_counts_as_oldest");

  const std::vector<Candidate> orphans = {book("epub_1", 1), book("epub_8", 80, true), book("md_5", 50, true)};
  runner.expectEqual("md_5,epub_8,epub_1", orderOf(orphans), "order_orphans_first");

  const std::vector<Candidate> guarded = {book("epub_1", 1, false, true), book("epub_2", 2),
                                          book("epub_3", 3, true, true)};
  runner.expectEqual("epub_2", orderOf(guarded), "order_skips_protected_even_when_oldest_or_orphan");

  const std::vector<Candidate> foreign = {book("fonts", 0), book("open.seq", 0), book("epub_4", 4)};
  runner.expectEqual("epub_4", orderOf(foreign), "order_skips_non_book_names");

  const std::vector<Candidate> ties = {book("txt_2", 5), book("epub_9", 5), book("fb2_1", 5)};
  runner.expectEqual("epub_9,fb2_1,txt_2", orderOf(ties), "order_ties_by_name");
}

void testPass(TestUtils::TestRunner& runner) {
  {
    // 1.5 MB free: under the 2 MB reserve.  Evict oldest until 3 MB is free.
    FakeTarget target(1536 * kKB);
    target.setSize("fb2_1", 600 * kKB);
    target.setSize("txt_2", 600 * kKB);
    target.setSize("epub_3", 600 * kKB);
    target.setSize("epub_4", 600 * kKB);
    const std::vector<Candidate> books = {book("epub_4", 40), book("epub_3", 30), book("txt_2", 20),
                                          book("fb2_1", 10), book("epub_5", 50, false, true)};
    const PassResult r = runDefault(books, target);
    runner.expectEqual("fb2_1,txt_2,epub_3", joined(target.removed), "pass_evicts_oldest_until_target");
    runner.expectEq(static_cast<uint16_t>(3), r.lruRemoved, "pass_counts_lru_removals");
    runner.expectEq(1536 * kKB, r.freeBefore, "pass_reports_free_before");
    runner.expectEq(3336 * kKB, r.freeAfter, "pass_reports_free_after");
    runner.expectFalse(r.aborted, "pass_not_aborted");
  }
  {
    // Plenty of room: an LRU pass does nothing, but orphans still go.
    FakeTarget target(4 * kMB);
    target.setSize("epub_8", 100 * kKB);
    const std::vector<Candidate> books = {book("fb2_1", 10), book("epub_8", 80, true), book("txt_2", 20)};
    const PassResult r = runDefault(books, target);
    runner.expectEqual("epub_8", joined(target.removed), "pass_removes_orphans_with_room");
    runner.expectEq(static_cast<uint16_t>(1), r.orphansRemoved, "pass_counts_orphans");
    runner.expectEq(static_cast<uint16_t>(0), r.lruRemoved, "pass_keeps_recent_books_with_room");
  }
  {
    // Between reserve and target: hysteresis, nothing is evicted.
    FakeTarget target(2500 * kKB);
    const std::vector<Candidate> books = {book("fb2_1", 10), book("txt_2", 20)};
    const PassResult r = runDefault(books, target);
    runner.expectTrue(target.removed.empty(), "pass_idle_above_reserve");
    runner.expectEq(1, target.measurements, "pass_idle_measures_once");
    runner.expectEq(2500 * kKB, r.freeAfter, "pass_idle_free_unchanged");
  }
  {
    // Low space: orphans first, then LRU, re-measuring between removals.
    FakeTarget target(1 * kMB);
    target.setSize("md_7", 1 * kMB);
    target.setSize("fb2_1", 1 * kMB);
    target.setSize("txt_2", 1 * kMB);
    const std::vector<Candidate> books = {book("txt_2", 20), book("fb2_1", 10), book("md_7", 70, true)};
    runDefault(books, target);
    runner.expectEqual("md_7,fb2_1", joined(target.removed), "pass_orphans_then_lru_until_target");
  }
  {
    // Only the protected book is left: nothing to remove, no crash.
    FakeTarget target(100 * kKB);
    const std::vector<Candidate> books = {book("epub_5", 50, false, true), book("fonts", 0)};
    const PassResult r = runDefault(books, target);
    runner.expectTrue(target.removed.empty(), "pass_never_removes_protected");
    runner.expectEq(static_cast<uint16_t>(0), static_cast<uint16_t>(r.lruRemoved + r.orphansRemoved),
                    "pass_reports_nothing_removed");
  }
  {
    // A removal that fails is counted and the pass moves on.
    FakeTarget target(1 * kMB);
    target.setSize("txt_2", 3 * kMB);
    target.failRemoval("fb2_1");
    const std::vector<Candidate> books = {book("txt_2", 20), book("fb2_1", 10)};
    const PassResult r = runDefault(books, target);
    runner.expectEqual("fb2_1,txt_2", joined(target.removed), "pass_continues_after_failed_removal");
    runner.expectEq(static_cast<uint16_t>(1), r.failed, "pass_counts_failed_removal");
    runner.expectEq(static_cast<uint16_t>(1), r.lruRemoved, "pass_counts_successful_removal");
  }
  {
    // Abort is honoured before each removal.
    FakeTarget target(0);
    target.setSize("fb2_1", 100 * kKB);
    target.setSize("txt_2", 100 * kKB);
    int checks = 0;
    const std::vector<Candidate> books = {book("txt_2", 20), book("fb2_1", 10)};
    const PassResult r = cache_fs::eviction::runPass(books, target, cache_fs::eviction::kReserveFreeBytes,
                                                     cache_fs::eviction::kTargetFreeBytes,
                                                     [&checks]() { return ++checks > 1; });
    runner.expectEqual("fb2_1", joined(target.removed), "pass_abort_stops_before_next_removal");
    runner.expectTrue(r.aborted, "pass_reports_abort");
    runner.expectEq(100 * kKB, r.freeAfter, "pass_abort_measures_what_was_freed");
  }
  {
    runner.expectTrue(cache_fs::eviction::kReserveFreeBytes < cache_fs::eviction::kTargetFreeBytes,
                      "limits_have_hysteresis");
  }
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("CacheEvictionPolicy");
  testNames(runner);
  testRecords(runner);
  testOrder(runner);
  testPass(runner);
  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}

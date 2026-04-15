// Tests for FsHelpers::normalisePath() and FsHelpers::isHiddenFsItem().

#include "test_utils.h"

#include <FsHelpers.h>

int main() {
  TestUtils::TestRunner runner("FsHelpersNormalise");

  // --- normalisePath ---

  runner.expectEqual(FsHelpers::normalisePath("books/fiction"),
                     std::string("books/fiction"), "simple path unchanged");

  runner.expectEqual(FsHelpers::normalisePath("a/b/../c"), std::string("a/c"),
                     "parent traversal");

  runner.expectEqual(FsHelpers::normalisePath("a/b/c/../../d"),
                     std::string("a/d"), "multiple parent traversals");

  runner.expectEqual(FsHelpers::normalisePath("../a"), std::string("a"),
                     ".. at root level skipped");

  runner.expectEqual(FsHelpers::normalisePath("a//b"), std::string("a/b"),
                     "double slashes collapsed");

  runner.expectEqual(FsHelpers::normalisePath("a/b/"), std::string("a/b"),
                     "trailing slash stripped");

  runner.expectEqual(FsHelpers::normalisePath("/a/b"), std::string("a/b"),
                     "leading slash not preserved");

  runner.expectEqual(FsHelpers::normalisePath(""), std::string(""),
                     "empty string");

  runner.expectEqual(FsHelpers::normalisePath("a/../../b"), std::string("b"),
                     "traversal beyond depth");

  runner.expectEqual(FsHelpers::normalisePath("a/./b"), std::string("a/b"),
                     "single dot collapsed");

  runner.expectEqual(FsHelpers::normalisePath("./a/b"), std::string("a/b"),
                     "leading dot collapsed");

  runner.expectEqual(FsHelpers::normalisePath("a/b/."), std::string("a/b"),
                     "trailing dot collapsed");

  runner.expectEqual(FsHelpers::normalisePath("a/b/.."), std::string("a"),
                     "trailing dotdot resolves");

  // --- percentDecode ---

  runner.expectEqual(FsHelpers::percentDecode("hello%20world"),
                     std::string("hello world"), "space decode");

  runner.expectEqual(FsHelpers::percentDecode("image%2F43.jpg"),
                     std::string("image/43.jpg"), "slash decode");

  runner.expectEqual(FsHelpers::percentDecode("43%2Ejpg"),
                     std::string("43.jpg"), "dot decode");

  runner.expectEqual(FsHelpers::percentDecode("%E4%B8%AD%E6%96%87"),
                     std::string("\xe4\xb8\xad\xe6\x96\x87"), "utf8 decode");

  runner.expectEqual(FsHelpers::percentDecode("no-encoding"),
                     std::string("no-encoding"), "plain string unchanged");

  runner.expectEqual(FsHelpers::percentDecode("bad%ZZseq"),
                     std::string("bad%ZZseq"), "invalid hex preserved");

  runner.expectEqual(FsHelpers::percentDecode("trail%2"),
                     std::string("trail%2"), "truncated percent preserved");

  runner.expectEqual(FsHelpers::percentDecode(""),
                     std::string(""), "empty percentDecode");

  runner.expectEqual(FsHelpers::percentDecode("%2e%2e/%2e%2e/secret"),
                     std::string("../../secret"), "lowercase hex decode");

  // --- stripQueryAndFragment ---

  runner.expectEqual(FsHelpers::stripQueryAndFragment("image.jpg#page=1"),
                     std::string("image.jpg"), "strip fragment");

  runner.expectEqual(FsHelpers::stripQueryAndFragment("image.jpg?v=2"),
                     std::string("image.jpg"), "strip query");

  runner.expectEqual(FsHelpers::stripQueryAndFragment("img.jpg?v=2#x"),
                     std::string("img.jpg"), "strip query before fragment");

  runner.expectEqual(FsHelpers::stripQueryAndFragment("img.jpg#x?v=2"),
                     std::string("img.jpg"), "strip fragment before query");

  runner.expectEqual(FsHelpers::stripQueryAndFragment("image.jpg"),
                     std::string("image.jpg"), "no query or fragment");

  runner.expectEqual(FsHelpers::stripQueryAndFragment("#only-frag"),
                     std::string(""), "fragment-only becomes empty");

  runner.expectEqual(FsHelpers::stripQueryAndFragment(""),
                     std::string(""), "empty stripQueryAndFragment");

  runner.expectEqual(FsHelpers::normalisePath("a///b///c"), std::string("a/b/c"),
                     "multiple consecutive slashes collapsed");

  runner.expectEqual(FsHelpers::normalisePath("a"), std::string("a"),
                     "single component");

  runner.expectEqual(FsHelpers::normalisePath("a/b/c/../../../d"),
                     std::string("d"),
                     "traverse all the way back then descend");

  // --- isHiddenFsItem ---

  runner.expectTrue(FsHelpers::isHiddenFsItem("System Volume Information"),
                    "hidden: System Volume Information");
  runner.expectTrue(FsHelpers::isHiddenFsItem("LOST.DIR"), "hidden: LOST.DIR");
  runner.expectTrue(FsHelpers::isHiddenFsItem("$RECYCLE.BIN"),
                    "hidden: $RECYCLE.BIN");
  runner.expectTrue(FsHelpers::isHiddenFsItem("config"), "hidden: config");
  runner.expectTrue(FsHelpers::isHiddenFsItem("XTCache"), "hidden: XTCache");
  runner.expectTrue(FsHelpers::isHiddenFsItem("sleep"), "hidden: sleep");

  runner.expectFalse(FsHelpers::isHiddenFsItem("Config"),
                     "case sensitive: Config");
  runner.expectFalse(FsHelpers::isHiddenFsItem("SYSTEM VOLUME INFORMATION"),
                     "case sensitive: uppercase");

  runner.expectFalse(FsHelpers::isHiddenFsItem("books"), "not hidden: books");
  runner.expectFalse(FsHelpers::isHiddenFsItem("README"), "not hidden: README");

  runner.expectFalse(FsHelpers::isHiddenFsItem("config.txt"),
                     "partial match: config.txt");
  runner.expectFalse(FsHelpers::isHiddenFsItem(""), "empty string not hidden");

  return runner.allPassed() ? 0 : 1;
}

#include "test_utils.h"

#include <SDCardManager.h>
#include <SdFat.h>

#include <string>

#include "drivers/Storage.h"

namespace {

std::string readAll(FsFile& file) {
  std::string result(file.size(), '\0');
  if (!result.empty()) {
    file.read(reinterpret_cast<uint8_t*>(&result[0]), result.size());
  }
  return result;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("StorageRecovery");
  auto& sd = SDCardManager::getInstance();
  sd.reset();

  snapix::drivers::Storage storage;
  runner.expectTrue(storage.init().ok(), "storage initializes");

  sd.setFileData("/settings.bin", "old");
  sd.setFileData("/settings.bin.tmp", "new");
  runner.expectTrue(storage.rename("/settings.bin.tmp", "/settings.bin"),
                    "replace commits through backup");
  FsFile file;
  runner.expectTrue(storage.openRead("/settings.bin", file).ok(), "committed file opens");
  runner.expectEq(std::string("new"), readAll(file), "replacement content is visible");
  file.close();
  runner.expectFalse(sd.exists("/settings.bin.bak"), "backup removed after successful commit");

  sd.reset();
  sd.setFileData("/settings.bin", "old");
  sd.setFileData("/settings.bin.tmp", "new");
  sd.setRenameFailSource("/settings.bin.tmp");
  runner.expectFalse(storage.rename("/settings.bin.tmp", "/settings.bin"),
                     "failed replacement reports failure");
  runner.expectTrue(storage.openRead("/settings.bin", file).ok(),
                    "failed replacement rolls the destination back");
  runner.expectEq(std::string("old"), readAll(file),
                  "rollback preserves the previous committed content");
  file.close();

  sd.reset();
  sd.setFileData("/progress.bin.bak", "known-good");
  sd.setFileData("/progress.bin.tmp", "uncommitted");
  runner.expectTrue(storage.openRead("/progress.bin", file).ok(),
                    "missing destination restores backup");
  runner.expectEq(std::string("known-good"), readAll(file),
                  "backup wins over an uncommitted temp file");
  file.close();

  sd.reset();
  sd.setFileData("/bookmark.bin.tmp", "first-save");
  runner.expectTrue(storage.openRead("/bookmark.bin", file).ok(),
                    "lone temp from first save is promoted");
  runner.expectEq(std::string("first-save"), readAll(file),
                  "promoted temp content is preserved");
  file.close();

  storage.shutdown();
  runner.expectFalse(storage.openRead("/bookmark.bin", file).ok(),
                     "unmounted storage rejects reads");

  sd.reset();
  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}

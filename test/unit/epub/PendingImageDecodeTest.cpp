#include <ImageConverter.h>
#include <LittleFS.h>
#include <PendingImageDecode.h>

#include <cassert>
#include <cstdint>
#include <functional>
#include <string>

namespace {

snapix::PendingImageDecode makeJob(const std::string& stem,
                                   bool& preparationCalled) {
  snapix::PendingImageDecode item;
  item.tempJpegPath = "/pending-test/" + stem + ".jpg";
  item.targetBmpPath = "/pending-test/" + stem + ".bmp";
  item.maxWidth = 480;
  item.maxHeight = 779;
  item.srcHash = 1;
  item.quickMode = true;
  item.logTag = "TEST";
  item.prepareInput =
      [&preparationCalled](const std::string& path,
                           const std::function<bool()>& abort) {
        preparationCalled = true;
        if (abort && abort()) return false;
        File out = LittleFS.open(path.c_str(), "w");
        if (!out) return false;
        static constexpr uint8_t kSource[] = {0xff, 0xd8, 0xff, 0xd9};
        const bool ok =
            out.write(kSource, sizeof(kSource)) == sizeof(kSource);
        out.close();
        return ok;
      };
  return item;
}

}  // namespace

int main() {
  LittleFS.clearFiles();
  ImageConverterFactory::conversionCalls = 0;
  (void)snapix::pendingImage::purgePrefix("/pending-test/");
  (void)snapix::pendingImage::consumeRefreshSignal();

  bool prepared = false;
  const auto first = makeJob("lazy", prepared);
  assert(snapix::pendingImage::enqueue(
      first, snapix::pendingImage::Priority::CurrentPage));

  // Enqueue must be I/O-free: preparation belongs to ReaderAsync drain.
  assert(!prepared);
  assert(!LittleFS.exists(first.tempJpegPath.c_str()));
  assert(snapix::pendingImage::pendingCount() == 1);

  assert(snapix::pendingImage::drainOne());
  assert(prepared);
  assert(ImageConverterFactory::conversionCalls == 1);
  assert(!LittleFS.exists(first.tempJpegPath.c_str()));
  assert(LittleFS.exists(first.targetBmpPath.c_str()));
  assert(snapix::pendingImage::pendingCount() == 0);
  assert(snapix::pendingImage::consumeRefreshSignal());

  // Cancellation before extraction must not touch storage or run conversion.
  bool cancelledPrepared = false;
  const auto cancelled = makeJob("cancelled", cancelledPrepared);
  assert(snapix::pendingImage::enqueue(
      cancelled, snapix::pendingImage::Priority::CurrentPage));
  assert(snapix::pendingImage::drainOne([]() { return true; }));
  assert(!cancelledPrepared);
  assert(ImageConverterFactory::conversionCalls == 1);
  assert(!LittleFS.exists(cancelled.tempJpegPath.c_str()));
  assert(!LittleFS.exists(cancelled.targetBmpPath.c_str()));

  return 0;
}

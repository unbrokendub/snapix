#include "StreamingChapterRenderer.h"

namespace snapix::smolport {

StreamingChapterRenderer::StreamingChapterRenderer(MarkerObserver& observer)
    : reader_(observer) {}

void StreamingChapterRenderer::resetForNextChapter() { reader_.reset(); }

bool StreamingChapterRenderer::finish() { return reader_.finish(); }

RenderStatus StreamingChapterRenderer::renderRange(ChapterReadFn readChunk, uint8_t* chunkBuf,
                                                    size_t chunkBufSize, const ChapterAbortFn& shouldAbort,
                                                    RenderStats* outStats) {
  if (!readChunk || chunkBuf == nullptr || chunkBufSize == 0) {
    if (outStats) {
      outStats->bytesConsumed = 0;
      outStats->chunksProcessed = 0;
      outStats->flags = 0;
    }
    return RenderStatus::ReadError;
  }

  uint32_t bytesRead = 0;
  uint16_t chunkCount = 0;
  uint8_t flags = 0;
  RenderStatus result = RenderStatus::EofClean;

  for (;;) {
    if (shouldAbort && shouldAbort()) {
      result = RenderStatus::Aborted;
      break;
    }

    const int got = readChunk(chunkBuf, chunkBufSize);
    if (got == 0) {
      // Clean EOF.  Caller should call finish() to fire onStreamEnd.
      flags |= 0x1;
      result = RenderStatus::EofClean;
      break;
    }
    if (got < 0) {
      result = RenderStatus::ReadError;
      break;
    }

    ++chunkCount;
    bytesRead += static_cast<uint32_t>(got);

    // feed() returns false if observer asked to Stop OR if a malformed
    // marker tripped the reader.  We can't easily distinguish here, so
    // treat both as observer-stopped semantics — the caller's next
    // renderRange() (after handling whatever the Stop signified) will
    // re-feed from the next chunk; if the stream is genuinely broken,
    // they'll see the same Stop again with no progress.
    if (!reader_.feed(chunkBuf, static_cast<size_t>(got))) {
      result = RenderStatus::ObserverStopped;
      break;
    }
  }

  if (outStats) {
    outStats->bytesConsumed = bytesRead;
    outStats->chunksProcessed = chunkCount;
    outStats->flags = flags;
  }
  return result;
}

}  // namespace snapix::smolport

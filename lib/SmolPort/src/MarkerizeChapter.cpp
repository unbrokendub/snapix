#include "MarkerizeChapter.h"

#include "MarkerizeStream.h"  // CallbackSink + runMarkerizeLoop (shared with TXT/MD)

namespace snapix::smolport {

// v3.7.0 — the write-coalescing CallbackSink and the read→feed→write loop
// were moved to MarkerizeStream.h so TxtStripper / MarkdownStripper can reuse
// them.  markerizeChapter() is now a thin HtmlStripper-specific wrapper around
// runMarkerizeLoop(); the emitted bytes are identical (verified by
// MarkerizeChapterTest), so markers / idx / page caches stay valid.

MarkerizeStatus markerizeChapter(HtmlStripper::Mode mode, MarkerizeReadFn readChunk,
                                  MarkerizeWriteFn writeChunk, uint8_t* chunkBuf, size_t chunkBufSize,
                                  const MarkerizeAbortFn& shouldAbort, MarkerizeStats* outStats) {
  if (!readChunk || !writeChunk || chunkBuf == nullptr || chunkBufSize == 0) {
    if (outStats) {
      outStats->inputBytes = 0;
      outStats->outputBytes = 0;
      outStats->chunksProcessed = 0;
    }
    return MarkerizeStatus::ReadError;  // misconfigured caller — treat as setup error
  }

  uint32_t inBytes = 0;
  uint32_t outBytes = 0;
  uint16_t chunkCount = 0;

  CallbackSink sink(writeChunk, shouldAbort, outBytes);
  HtmlStripper stripper(sink, mode);

  const MarkerizeStatus result = runMarkerizeLoop(stripper, sink, readChunk, chunkBuf, chunkBufSize,
                                                  shouldAbort, inBytes, chunkCount);

  if (outStats) {
    outStats->inputBytes = inBytes;
    outStats->outputBytes = outBytes;
    outStats->chunksProcessed = chunkCount;
  }
  return result;
}

}  // namespace snapix::smolport

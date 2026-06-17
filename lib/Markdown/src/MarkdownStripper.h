#pragma once

// =============================================================================
// MarkdownStripper — converts a Markdown byte stream into the SmolPort
// byte-marker styled-text format (see Markers.h), so Markdown joins EPUB/FB2 on
// the v3 streaming render pipeline instead of the legacy ParsedText block path.
//
// It reuses the same `md_parser` tokeniser the legacy MarkdownParser used, so
// syntax coverage is identical (headers, bold/italic, lists, blockquotes, code,
// hr, links, images).  The difference is the OUTPUT: instead of building
// ParsedText blocks it emits markers to an HtmlStripperSink:
//
//   MD_HEADER_START  → kHeadingOn + level digit      MD_BLOCKQUOTE_* → kQuote{On,Off}
//   MD_BOLD_*        → kBold{On,Off}                 MD_HR           → kBreak (ornament)
//   MD_ITALIC_*      → kItalic{On,Off}              MD_LIST_ITEM    → "• " / "N. " prefix
//   MD_CODE_INLINE   → italic span                   blank line      → kParagraphBreak
//   MD_CODE_BLOCK    → "[Code: … ]" italic           MD_IMAGE        → "[Image]"
//
// Parsing is line-oriented (md_parser is reset per line, like the legacy path):
// blank lines separate paragraphs, consecutive non-blank lines join with a
// space.  Two deliberate improvements over the legacy per-line reset: code
// fences are treated as a TOGGLE (so the closing ``` is detected), and open
// bold/italic are auto-closed at paragraph boundaries (no style bleed).
//
// Same shape as HtmlStripper (sink + feed/finish/reset) so it drives the shared
// runMarkerizeLoop() in MarkerizeStream.h.
// =============================================================================

#include <cstddef>
#include <cstdint>

#include <HtmlStripper.h>  // snapix::smolport::HtmlStripperSink

#include "md_parser.h"

namespace snapix::markdown {

class MarkdownStripper {
 public:
  explicit MarkdownStripper(snapix::smolport::HtmlStripperSink& sink);

  // Feed an arbitrary chunk.  Bytes are buffered into lines internally; each
  // complete line is tokenised + mapped to markers.  Returns `len` (the
  // markerize pass is not interrupted mid-chunk — the loop aborts per-chunk).
  size_t feed(const uint8_t* data, size_t len);

  // Flush the trailing partial line and close any dangling styles / code block.
  void finish();

  // Reset for a fresh document.
  void reset();

 private:
  static constexpr int kLineCap = 512;  // matches legacy LINE_BUFFER_SIZE

  // md_parser callback trampoline → onToken().
  static bool tokenTrampoline(const md_token_t* token, void* user);
  void onToken(const md_token_t* token);

  // Line processing.
  void processLine(const char* line, int len);
  static bool lineIsBlank(const char* line, int len);
  static bool lineIsFence(const char* line, int len);  // ``` (ignoring indent)

  // Emit helpers.
  void emitMarker(uint8_t tag);
  void emitRawByte(uint8_t b);
  void emitText(const char* data, int len);  // escapes 0x01, applies separators
  void startBlock();        // paragraph break before a block-level element
  void flushSeparators();   // pending paragraph break / continuation space before inline text
  void closeInline();       // close any open bold/italic
  void closeCode();         // close an open code block

  snapix::smolport::HtmlStripperSink& sink_;
  md_parser_t parser_;

  // Line buffer (chunk-boundary safe).
  char line_[kLineCap];
  int lineLen_ = 0;

  // Paragraph / inline state, persisted across lines.
  bool everEmitted_ = false;    // any output yet? (suppresses a leading break)
  bool pendingBreak_ = false;   // a paragraph break is pending (blank line / block end)
  bool needSpace_ = false;      // a continuation space is pending (line join)
  bool paraHasContent_ = false; // current paragraph has emitted content
  bool inBold_ = false;
  bool inItalic_ = false;
  bool inCodeBlock_ = false;
  bool prevLineBlank_ = true;
};

}  // namespace snapix::markdown

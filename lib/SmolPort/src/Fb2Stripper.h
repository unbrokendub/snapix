#pragma once

// =============================================================================
// SmolPort Fb2Stripper — converts an FB2 (FictionBook2) XML stream into the
// byte-marker styled-text format.
//
// FB2 is an XML-based ebook format popular in the Russian-language EPUB
// ecosystem.  Structure: single `.fb2` file with `<FictionBook>` root,
// `<description>` metadata block, `<body>` containing `<section>`s, and
// `<binary>` chunks holding base64-encoded inline images.
//
// Heritage: same one-pass stripper pattern as `HtmlStripper`.  Internally
// this class is a thin alias over `HtmlStripper(sink, Mode::Fb2)` — the
// XML lexer (state machine, entity decoder, attribute parser) is shared;
// only the tag-to-marker dispatch table, capturable-attribute rules, and
// raw-body suppression set differ.
//
// Tag → marker mapping (FB2 mode):
//   <emphasis>, </emphasis>             → kItalicOn / kItalicOff
//   <strong>,   </strong>               → kBoldOn   / kBoldOff
//   <title>,    </title>                → kHeadingOn'2' / kHeadingOff
//                                          (level fixed at '2' — nesting
//                                          tracking would distinguish book
//                                          title from section title, but
//                                          MVP picks one level)
//   <subtitle>, </subtitle>             → kHeadingOn'3' / kHeadingOff
//   <cite>,     </cite>                 → kQuoteOn / kQuoteOff
//   </p>                                 → kParagraphBreak (after text)
//   <empty-line/>                        → kLineBreak
//   <v>                                  → kLineBreak  (verse line)
//   <section>                            → kPageBreak  (new logical page)
//   <image l:href="…"/>                  → kImageRef + len-prefixed href
//   id="…" on any tag                    → kAnchor    + len-prefixed id
//   <binary>, <description>, <stylesheet> → body suppressed (no emit)
//   everything else                      → silently skipped (text kept)
//
// Entities (Phase 3c): same decoder as HtmlStripper — XML builtins +
// named + numeric.  FB2 uses the XML entity set; well-formed FB2 files
// always use proper `&amp;` / `&#NNN;` escapes.
// =============================================================================

#include "HtmlStripper.h"

namespace snapix::smolport {

class Fb2Stripper : public HtmlStripper {
 public:
  explicit Fb2Stripper(HtmlStripperSink& sink)
      : HtmlStripper(sink, HtmlStripper::Mode::Fb2) {}
};

}  // namespace snapix::smolport

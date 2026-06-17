#include "MarkdownStripper.h"

#include <Markers.h>

#include <cstdio>
#include <cstring>

namespace snapix::markdown {

using snapix::smolport::kBoldOff;
using snapix::smolport::kBoldOn;
using snapix::smolport::kBreak;
using snapix::smolport::kHeadingOff;
using snapix::smolport::kHeadingOn;
using snapix::smolport::kItalicOff;
using snapix::smolport::kItalicOn;
using snapix::smolport::kMarker;
using snapix::smolport::kParagraphBreak;
using snapix::smolport::kQuoteOff;
using snapix::smolport::kQuoteOn;

MarkdownStripper::MarkdownStripper(snapix::smolport::HtmlStripperSink& sink) : sink_(sink) {
  md_parser_init(&parser_, &MarkdownStripper::tokenTrampoline, this);
}

void MarkdownStripper::reset() {
  md_parser_init(&parser_, &MarkdownStripper::tokenTrampoline, this);
  lineLen_ = 0;
  everEmitted_ = false;
  pendingBreak_ = false;
  needSpace_ = false;
  paraHasContent_ = false;
  inBold_ = false;
  inItalic_ = false;
  inCodeBlock_ = false;
  prevLineBlank_ = true;
}

// ---------------------------------------------------------------------------
// Emit primitives.
// ---------------------------------------------------------------------------
void MarkdownStripper::emitMarker(uint8_t tag) {
  const uint8_t m[2] = {kMarker, tag};
  sink_.emit(m, sizeof(m));
  everEmitted_ = true;
}

void MarkdownStripper::emitRawByte(uint8_t b) {
  if (b == kMarker) {
    const uint8_t esc[2] = {kMarker, kMarker};
    sink_.emit(esc, sizeof(esc));
  } else {
    sink_.emit(&b, 1);
  }
  everEmitted_ = true;
}

void MarkdownStripper::closeInline() {
  if (inItalic_) {
    emitMarker(kItalicOff);
    inItalic_ = false;
  }
  if (inBold_) {
    emitMarker(kBoldOff);
    inBold_ = false;
  }
}

void MarkdownStripper::startBlock() {
  // Separate this block-level element from any preceding content.
  if (paraHasContent_ || pendingBreak_) {
    if (everEmitted_) {
      closeInline();
      emitMarker(kParagraphBreak);
    }
  }
  pendingBreak_ = false;
  needSpace_ = false;
  paraHasContent_ = false;
}

void MarkdownStripper::flushSeparators() {
  if (pendingBreak_) {
    if (everEmitted_) {
      closeInline();
      emitMarker(kParagraphBreak);
    }
    pendingBreak_ = false;
    needSpace_ = false;
    paraHasContent_ = false;
  } else if (needSpace_) {
    if (paraHasContent_) emitRawByte(' ');
    needSpace_ = false;
  }
}

void MarkdownStripper::emitText(const char* data, int len) {
  if (len <= 0) return;
  flushSeparators();
  // Bulk-emit, self-doubling any literal 0x01 (marker escape).
  int start = 0;
  for (int i = 0; i < len; ++i) {
    if (static_cast<uint8_t>(data[i]) == kMarker) {
      if (i > start) sink_.emit(reinterpret_cast<const uint8_t*>(data + start), i - start);
      const uint8_t esc[2] = {kMarker, kMarker};
      sink_.emit(esc, sizeof(esc));
      start = i + 1;
    }
  }
  if (len > start) sink_.emit(reinterpret_cast<const uint8_t*>(data + start), len - start);
  paraHasContent_ = true;
  everEmitted_ = true;
}

void MarkdownStripper::closeCode() {
  if (!inCodeBlock_) return;
  emitText("]", 1);
  if (inItalic_) {
    emitMarker(kItalicOff);
    inItalic_ = false;
  }
  inCodeBlock_ = false;
  pendingBreak_ = true;
  paraHasContent_ = false;
}

// ---------------------------------------------------------------------------
// Token mapping.
// ---------------------------------------------------------------------------
bool MarkdownStripper::tokenTrampoline(const md_token_t* token, void* user) {
  static_cast<MarkdownStripper*>(user)->onToken(token);
  return true;  // never abort mid-line
}

void MarkdownStripper::onToken(const md_token_t* token) {
  switch (token->type) {
    case MD_TEXT:
      emitText(token->text, token->length);
      break;

    case MD_HEADER_START: {
      startBlock();
      uint8_t lvl = token->data;
      if (lvl < 1) lvl = 1;
      if (lvl > 6) lvl = 6;
      emitMarker(kHeadingOn);
      emitRawByte(static_cast<uint8_t>('0' + lvl));
      break;
    }
    case MD_HEADER_END:
      closeInline();
      emitMarker(kHeadingOff);
      pendingBreak_ = true;
      paraHasContent_ = false;
      break;

    case MD_BOLD_START:
      if (!inBold_) {
        flushSeparators();
        emitMarker(kBoldOn);
        inBold_ = true;
      }
      break;
    case MD_BOLD_END:
      if (inBold_) {
        emitMarker(kBoldOff);
        inBold_ = false;
      }
      break;

    case MD_ITALIC_START:
      if (!inItalic_) {
        flushSeparators();
        emitMarker(kItalicOn);
        inItalic_ = true;
      }
      break;
    case MD_ITALIC_END:
      if (inItalic_) {
        emitMarker(kItalicOff);
        inItalic_ = false;
      }
      break;

    case MD_CODE_INLINE: {
      flushSeparators();
      const bool wasItalic = inItalic_;
      if (!inItalic_) {
        emitMarker(kItalicOn);
        inItalic_ = true;
      }
      emitText(token->text, token->length);
      if (!wasItalic) {
        emitMarker(kItalicOff);
        inItalic_ = false;
      }
      break;
    }

    case MD_CODE_BLOCK_START:
      // Per-line parser reset means the CLOSING fence also arrives as a
      // START; treat it as a toggle so code blocks open AND close correctly.
      if (!inCodeBlock_) {
        startBlock();
        if (!inItalic_) {
          emitMarker(kItalicOn);
          inItalic_ = true;
        }
        emitText("[Code: ", 7);
        inCodeBlock_ = true;
      } else {
        closeCode();
      }
      break;
    case MD_CODE_BLOCK_END:
      closeCode();
      break;

    case MD_LIST_ITEM_START: {
      startBlock();
      if (token->data > 0) {
        char num[12];
        const int n = snprintf(num, sizeof(num), "%d. ", static_cast<int>(token->data));
        if (n > 0) emitText(num, n);
      } else {
        emitText("\xE2\x80\xA2 ", 4);  // "• " (U+2022 + space)
      }
      break;
    }
    case MD_LIST_ITEM_END:
      pendingBreak_ = true;
      paraHasContent_ = false;
      break;

    case MD_BLOCKQUOTE_START:
      startBlock();
      emitMarker(kQuoteOn);
      break;
    case MD_BLOCKQUOTE_END:
      closeInline();
      emitMarker(kQuoteOff);
      pendingBreak_ = true;
      paraHasContent_ = false;
      break;

    case MD_HR:
      startBlock();
      emitMarker(kBreak);
      pendingBreak_ = true;
      paraHasContent_ = false;
      break;

    case MD_IMAGE_ALT_START:
      emitText("[Image]", 7);
      break;

    case MD_NEWLINE:
      needSpace_ = true;
      break;

    // Link markers carry no styling we render; the link text flows as MD_TEXT.
    case MD_LINK_TEXT_START:
    case MD_LINK_TEXT_END:
    case MD_LINK_URL:
    case MD_IMAGE_ALT_END:
    case MD_IMAGE_URL:
    case MD_STRIKE_START:
    case MD_STRIKE_END:
    case MD_PARAGRAPH_START:
    case MD_PARAGRAPH_END:
      break;
  }
}

// ---------------------------------------------------------------------------
// Line buffering + dispatch.
// ---------------------------------------------------------------------------
bool MarkdownStripper::lineIsBlank(const char* line, int len) {
  for (int i = 0; i < len; ++i) {
    const char c = line[i];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
  }
  return true;
}

bool MarkdownStripper::lineIsFence(const char* line, int len) {
  int i = 0;
  while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;  // skip indent
  return (len - i) >= 3 && line[i] == '`' && line[i + 1] == '`' && line[i + 2] == '`';
}

void MarkdownStripper::processLine(const char* line, int len) {
  // Strip a trailing CR (CRLF sources).
  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) --len;

  if (inCodeBlock_) {
    if (lineIsFence(line, len)) {
      closeCode();
    } else {
      if (paraHasContent_) needSpace_ = true;  // join code lines with a space
      emitText(line, len);
    }
    prevLineBlank_ = false;
    return;
  }

  if (lineIsBlank(line, len)) {
    if (!prevLineBlank_) pendingBreak_ = true;
    prevLineBlank_ = true;
    return;
  }

  // Continuation of an open paragraph → join the next text with a space.
  if (paraHasContent_ && !pendingBreak_) needSpace_ = true;

  md_parser_reset(&parser_);
  md_parse(&parser_, line, static_cast<size_t>(len));
  prevLineBlank_ = false;
}

size_t MarkdownStripper::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c == '\n') {
      processLine(line_, lineLen_);
      lineLen_ = 0;
      continue;
    }
    if (lineLen_ < kLineCap - 1) {
      line_[lineLen_++] = c;
    } else {
      // Pathological line longer than the buffer — flush what we have as a
      // line and keep going (matches legacy fgets truncation behaviour).
      line_[lineLen_] = '\0';
      processLine(line_, lineLen_);
      lineLen_ = 0;
      line_[lineLen_++] = c;
    }
  }
  return len;
}

void MarkdownStripper::finish() {
  if (lineLen_ > 0) {
    processLine(line_, lineLen_);
    lineLen_ = 0;
  }
  if (inCodeBlock_) closeCode();
  closeInline();
}

}  // namespace snapix::markdown

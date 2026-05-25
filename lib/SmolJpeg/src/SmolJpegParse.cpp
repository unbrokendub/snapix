#include "SmolJpegParse.h"

#include "SmolJpegHuffman.h"

#include <Logging.h>

#include <algorithm>

#define TAG "SMOL_JPEG"

namespace snapix::smoljpeg {

namespace {

// =============================================================================
// HeaderReader — streaming byte reader over an `InputStream`.
//
// v2.0.88 (post-alpha hardware bring-up): the previous implementation
// allocated a single 32 KB scratch buffer up front (`readHeaderBuffer`)
// and parsed from that.  On the Xteink X4 (ESP32-C3, ~145 KB heap) this
// failed `AllocFailed` when invoked from the cover-gen path after the
// rest of the system had used ~20 KB of contiguous DRAM — the heap was
// 90 KB free total but the largest free block had dropped below 32 KB
// from prior allocations.  SmolJpeg then bailed and the slow picojpeg
// fallback kicked in.
//
// Rewritten as a 256-byte sliding-window reader: we read whatever the
// parser asks for in chunks of up to 256 B and discard them as we go.
// Skipped segments (APPn, COM, EXIF thumbnails) just advance the
// absolute stream cursor and trigger a refill on the next read.  Total
// transient RAM during marker parse is now <300 B + JpegState.
// =============================================================================
class HeaderReader {
 public:
  explicit HeaderReader(InputStream& in)
      : in_(in),
        streamLen_(in.length()),
        pos_(0),
        bufStart_(0),
        bufLen_(0) {}

  bool readU8(uint8_t& v) {
    if (pos_ >= bufStart_ + bufLen_ && !refill()) return false;
    v = buf_[pos_ - bufStart_];
    ++pos_;
    return true;
  }

  bool readU16(uint16_t& v) {
    uint8_t b1, b2;
    if (!readU8(b1) || !readU8(b2)) return false;
    v = static_cast<uint16_t>((uint16_t{b1} << 8) | uint16_t{b2});
    return true;
  }

  // Read exactly `n` bytes into `out`.  Returns false on short read.
  bool readBytes(uint8_t* out, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
      if (!readU8(out[i])) return false;
    }
    return true;
  }

  // Advance the absolute stream cursor by `n` bytes without keeping the
  // bytes.  Used to jump over APPn / COM / EXIF segments without paying
  // their RAM cost.  The internal buffer is invalidated if the jump
  // lands outside it; the next read refills from the new position.
  bool skip(uint32_t n) {
    if (n == 0) return true;
    const uint32_t target = pos_ + n;
    if (target > streamLen_) return false;
    pos_ = target;
    if (pos_ < bufStart_ || pos_ >= bufStart_ + bufLen_) {
      bufLen_ = 0;
      bufStart_ = pos_;
    }
    return true;
  }

  // Absolute byte offset of the next byte to be read (== where the
  // entropy-coded scan starts when the caller stops at SOS).
  uint32_t position() const { return pos_; }

 private:
  bool refill() {
    if (pos_ >= streamLen_) return false;
    bufStart_ = pos_;
    const uint32_t want = std::min<uint32_t>(
        sizeof(buf_), streamLen_ - pos_);
    const int got = in_.read(pos_, buf_, want);
    if (got <= 0) {
      bufLen_ = 0;
      return false;
    }
    bufLen_ = static_cast<uint32_t>(got);
    return true;
  }

  InputStream& in_;
  uint32_t streamLen_;
  uint32_t pos_;        // absolute position in the underlying stream
  uint32_t bufStart_;   // stream offset of buf_[0]
  uint32_t bufLen_;     // valid bytes in buf_
  uint8_t  buf_[256];   // sliding window
};

// Read the next marker byte (skipping 0xFF padding).  Position must be
// just past the previous segment.
bool readMarker(HeaderReader& r, uint8_t& marker) {
  uint8_t b;
  if (!r.readU8(b)) return false;
  if (b != 0xFFu) return false;
  do {
    if (!r.readU8(b)) return false;
  } while (b == 0xFFu);
  marker = b;
  return true;
}

// Skip a length-prefixed segment whose 2-byte length field comes next.
bool skipSegment(HeaderReader& r) {
  uint16_t len;
  if (!r.readU16(len)) return false;
  if (len < 2) return false;
  return r.skip(len - 2u);
}

Status parseSof(HeaderReader& r, JpegState& state, bool progressive) {
  uint16_t segLen;
  if (!r.readU16(segLen)) return Status::InvalidJpeg;

  uint8_t precision;
  uint16_t h, w;
  uint8_t nf;
  if (!r.readU8(precision)) return Status::InvalidJpeg;
  if (!r.readU16(h)) return Status::InvalidJpeg;
  if (!r.readU16(w)) return Status::InvalidJpeg;
  if (!r.readU8(nf)) return Status::InvalidJpeg;

  if (precision != 8) return Status::UnsupportedFeature;
  if (nf == 0 || nf > kMaxComponents) return Status::UnsupportedFeature;
  if (h == 0 || w == 0) return Status::InvalidJpeg;
  if (static_cast<uint32_t>(w) * static_cast<uint32_t>(h) > kMaxPixels) {
    return Status::ExceedsPixelLimit;
  }
  if (segLen != static_cast<uint16_t>(8 + 3 * nf)) return Status::InvalidJpeg;

  state.width = w;
  state.height = h;
  state.numComp = nf;
  state.progressive = progressive;

  uint8_t maxH = 1, maxV = 1;
  for (uint8_t i = 0; i < nf; ++i) {
    uint8_t id, samp, qt;
    if (!r.readU8(id) || !r.readU8(samp) || !r.readU8(qt)) {
      return Status::InvalidJpeg;
    }
    Component& comp = state.comp[i];
    comp.id    = id;
    comp.hSamp = (samp >> 4) & 0x0F;
    comp.vSamp = samp & 0x0F;
    comp.qtIdx = qt;
    if (comp.hSamp < 1 || comp.hSamp > 4 ||
        comp.vSamp < 1 || comp.vSamp > 4) {
      return Status::UnsupportedFeature;
    }
    if (qt >= 4) return Status::InvalidJpeg;
    if (comp.hSamp > maxH) maxH = comp.hSamp;
    if (comp.vSamp > maxV) maxV = comp.vSamp;
  }
  state.maxH = maxH;
  state.maxV = maxV;
  return Status::Ok;
}

Status parseDqt(HeaderReader& r, JpegState& state) {
  uint16_t segLen;
  if (!r.readU16(segLen)) return Status::InvalidJpeg;
  if (segLen < 2) return Status::InvalidJpeg;
  uint32_t remaining = static_cast<uint32_t>(segLen - 2);

  while (remaining > 0) {
    uint8_t pq;
    if (!r.readU8(pq)) return Status::InvalidJpeg;
    --remaining;
    const uint8_t precision = pq >> 4;  // 0 = 8-bit, 1 = 16-bit
    const uint8_t tid       = pq & 0x0F;
    if (tid >= 4) return Status::InvalidJpeg;

    if (precision == 0) {
      if (remaining < 64) return Status::InvalidJpeg;
      for (int i = 0; i < 64; ++i) {
        uint8_t v;
        if (!r.readU8(v)) return Status::InvalidJpeg;
        state.qt[tid][i] = v;
      }
      remaining -= 64;
    } else if (precision == 1) {
      if (remaining < 128) return Status::InvalidJpeg;
      for (int i = 0; i < 64; ++i) {
        uint16_t v;
        if (!r.readU16(v)) return Status::InvalidJpeg;
        state.qt[tid][i] = v;
      }
      remaining -= 128;
    } else {
      return Status::InvalidJpeg;
    }
    state.qtOk[tid] = true;
  }
  return Status::Ok;
}

Status parseDht(HeaderReader& r, JpegState& state) {
  uint16_t segLen;
  if (!r.readU16(segLen)) return Status::InvalidJpeg;
  if (segLen < 2) return Status::InvalidJpeg;
  uint32_t remaining = static_cast<uint32_t>(segLen - 2);

  while (remaining > 0) {
    uint8_t tc;
    if (!r.readU8(tc)) return Status::InvalidJpeg;
    --remaining;
    const uint8_t cls = tc >> 4;  // 0 = DC, 1 = AC
    const uint8_t tid = tc & 0x0F;
    if (cls > 1 || tid >= 4) return Status::InvalidJpeg;

    uint8_t bits[16];
    if (remaining < 16) return Status::InvalidJpeg;
    if (!r.readBytes(bits, 16)) return Status::InvalidJpeg;
    remaining -= 16;

    uint32_t nVals = 0;
    for (int i = 0; i < 16; ++i) nVals += bits[i];
    if (nVals > 256) return Status::InvalidJpeg;
    if (remaining < nVals) return Status::InvalidJpeg;

    uint8_t vals[256];  // ≤256 by JPEG spec; stack-resident, tight bound.
    if (!r.readBytes(vals, nVals)) return Status::InvalidJpeg;
    remaining -= nVals;

    HuffTable& tbl = (cls == 0) ? state.dcHuff[tid] : state.acHuff[tid];
    if (!buildHuffTable(tbl, bits, vals, static_cast<uint16_t>(nVals))) {
      return Status::InvalidJpeg;
    }
    if (cls == 0) state.dcOk[tid] = true;
    else          state.acOk[tid] = true;
  }
  return Status::Ok;
}

Status parseDri(HeaderReader& r, JpegState& state) {
  uint16_t segLen, interval;
  if (!r.readU16(segLen)) return Status::InvalidJpeg;
  if (segLen != 4) return Status::InvalidJpeg;
  if (!r.readU16(interval)) return Status::InvalidJpeg;
  state.restartInterval = interval;
  return Status::Ok;
}

Status parseSos(HeaderReader& r, JpegState& state) {
  uint16_t segLen;
  if (!r.readU16(segLen)) return Status::InvalidJpeg;
  if (segLen < 6) return Status::InvalidJpeg;

  uint8_t ns;
  if (!r.readU8(ns)) return Status::InvalidJpeg;
  if (ns == 0 || ns > kMaxComponents) return Status::InvalidJpeg;
  if (segLen != static_cast<uint16_t>(6 + 2 * ns)) return Status::InvalidJpeg;

  state.scanNumComp = ns;
  for (uint8_t i = 0; i < ns; ++i) {
    uint8_t id, dcac;
    if (!r.readU8(id) || !r.readU8(dcac)) return Status::InvalidJpeg;

    int compIdx = -1;
    for (uint8_t j = 0; j < state.numComp; ++j) {
      if (state.comp[j].id == id) { compIdx = j; break; }
    }
    if (compIdx < 0) {
      LOG_INF(TAG, "parseSos compIdx<0: id=%u numComp=%u comp_ids=[%u,%u,%u,%u]",
              static_cast<unsigned>(id),
              static_cast<unsigned>(state.numComp),
              state.numComp > 0 ? static_cast<unsigned>(state.comp[0].id) : 0u,
              state.numComp > 1 ? static_cast<unsigned>(state.comp[1].id) : 0u,
              state.numComp > 2 ? static_cast<unsigned>(state.comp[2].id) : 0u,
              state.numComp > 3 ? static_cast<unsigned>(state.comp[3].id) : 0u);
      return Status::InvalidJpeg;
    }

    state.comp[compIdx].dcTbl = (dcac >> 4) & 0x0F;
    state.comp[compIdx].acTbl = dcac & 0x0F;
    if (state.comp[compIdx].dcTbl >= 4 ||
        state.comp[compIdx].acTbl >= 4) {
      LOG_INF(TAG, "parseSos table-idx-bad: comp[%d].dcTbl=%u acTbl=%u",
              compIdx, static_cast<unsigned>(state.comp[compIdx].dcTbl),
              static_cast<unsigned>(state.comp[compIdx].acTbl));
      return Status::InvalidJpeg;
    }
    state.scanOrder[i] = static_cast<uint8_t>(compIdx);
  }

  uint8_t ss, se, ahAl;
  if (!r.readU8(ss) || !r.readU8(se) || !r.readU8(ahAl)) {
    return Status::InvalidJpeg;
  }
  state.scanSs = ss;
  state.scanSe = se;
  state.scanAl = ahAl & 0x0F;
  // Ah (high nibble of ahAl) is for progressive successive-approximation
  // refinement.  We only support the first scan, where Ah is implicitly 0,
  // so it's harmless to ignore here.

  return Status::Ok;
}

}  // namespace

Status parseMarkers(InputStream& in, JpegState& state) {
  state = JpegState{};

  HeaderReader r(in);

  // SOI: 0xFF 0xD8
  uint8_t b1 = 0, b2 = 0;
  const bool soi1 = r.readU8(b1);
  const bool soi2 = soi1 && r.readU8(b2);
  if (!soi1 || !soi2 || b1 != 0xFFu || b2 != kMarkerSOI) {
    LOG_INF(TAG, "parseMarkers SOI fail: streamLen=%u soi1=%u soi2=%u b1=0x%02X b2=0x%02X (expected FF D8)",
            static_cast<unsigned>(in.length()), static_cast<unsigned>(soi1),
            static_cast<unsigned>(soi2), static_cast<unsigned>(b1),
            static_cast<unsigned>(b2));
    return Status::InvalidJpeg;
  }

  for (;;) {
    uint8_t marker;
    if (!readMarker(r, marker)) {
      LOG_INF(TAG, "parseMarkers readMarker fail at pos=%u", static_cast<unsigned>(r.position()));
      return Status::InvalidJpeg;
    }

    Status s = Status::Ok;
    switch (marker) {
      case kMarkerSOF0:
        s = parseSof(r, state, false);
        if (s != Status::Ok) {
          LOG_INF(TAG, "parseMarkers SOF0 fail rc=%d at pos=%u",
                  static_cast<int>(s), static_cast<unsigned>(r.position()));
        }
        break;
      case kMarkerSOF2:
        s = parseSof(r, state, true);
        if (s != Status::Ok) {
          LOG_INF(TAG, "parseMarkers SOF2 fail rc=%d at pos=%u",
                  static_cast<int>(s), static_cast<unsigned>(r.position()));
        }
        break;
      case kMarkerDQT:
        s = parseDqt(r, state);
        if (s != Status::Ok) {
          LOG_INF(TAG, "parseMarkers DQT fail rc=%d at pos=%u",
                  static_cast<int>(s), static_cast<unsigned>(r.position()));
        }
        break;
      case kMarkerDHT:
        s = parseDht(r, state);
        if (s != Status::Ok) {
          LOG_INF(TAG, "parseMarkers DHT fail rc=%d at pos=%u",
                  static_cast<int>(s), static_cast<unsigned>(r.position()));
        }
        break;
      case kMarkerDRI:
        s = parseDri(r, state);
        if (s != Status::Ok) {
          LOG_INF(TAG, "parseMarkers DRI fail rc=%d at pos=%u",
                  static_cast<int>(s), static_cast<unsigned>(r.position()));
        }
        break;
      case kMarkerSOS:
        s = parseSos(r, state);
        if (s == Status::Ok) {
          state.scanStart = r.position();
        } else {
          LOG_INF(TAG, "parseMarkers SOS fail rc=%d at pos=%u",
                  static_cast<int>(s), static_cast<unsigned>(r.position()));
        }
        return s;
      case kMarkerEOI:
        // EOI before SOS — malformed.
        LOG_INF(TAG, "parseMarkers EOI before SOS at pos=%u", static_cast<unsigned>(r.position()));
        return Status::InvalidJpeg;
      default:
        // Standalone markers (TEM=0x01, RSTn=0xD0..D7) have no payload.
        if (marker == 0x01u ||
            (marker >= kMarkerRST0 && marker <= kMarkerRST7)) {
          break;
        }
        // Everything else (APPn, COM, JPGn, DAC, …) — skip via length.
        if (!skipSegment(r)) {
          LOG_INF(TAG, "parseMarkers skipSegment fail marker=0x%02X pos=%u",
                  static_cast<unsigned>(marker), static_cast<unsigned>(r.position()));
          return Status::InvalidJpeg;
        }
        break;
    }
    if (s != Status::Ok) return s;
  }
}

Status scanSofDimensions(InputStream& in, uint16_t& outWidth,
                         uint16_t& outHeight) {
  outWidth = 0;
  outHeight = 0;

  HeaderReader r(in);

  uint8_t b1, b2;
  if (!r.readU8(b1) || !r.readU8(b2) || b1 != 0xFFu || b2 != kMarkerSOI) {
    return Status::InvalidJpeg;
  }

  for (;;) {
    uint8_t marker;
    if (!readMarker(r, marker)) return Status::InvalidJpeg;

    if (marker == kMarkerSOF0 || marker == kMarkerSOF2) {
      uint16_t segLen;
      uint8_t precision;
      uint16_t h, w;
      if (!r.readU16(segLen) || !r.readU8(precision) ||
          !r.readU16(h) || !r.readU16(w)) {
        return Status::InvalidJpeg;
      }
      if (precision != 8 || h == 0 || w == 0) {
        return Status::InvalidJpeg;
      }
      outWidth = w;
      outHeight = h;
      return Status::Ok;
    }
    if (marker == kMarkerSOS || marker == kMarkerEOI) {
      return Status::InvalidJpeg;
    }
    // Standalone markers with no payload.
    if (marker == 0x01u ||
        (marker >= kMarkerRST0 && marker <= kMarkerRST7)) {
      continue;
    }
    if (!skipSegment(r)) return Status::InvalidJpeg;
  }
}

}  // namespace snapix::smoljpeg

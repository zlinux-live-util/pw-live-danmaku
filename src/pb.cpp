// The protobuf wire reader. See pb.hpp for why this project reads these blobs without a schema.
#include "pb.hpp"

namespace dwm::pb {
namespace {

/** Reads one base-128 varint. False when the bytes run out or the value does not terminate in ten
 *  bytes, which is the longest a 64-bit varint can be. */
bool readVarint(std::string_view b, size_t& i, uint64_t& out) {
  uint64_t r = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (i >= b.size()) return false;
    const uint8_t c = static_cast<uint8_t>(b[i++]);
    r |= static_cast<uint64_t>(c & 0x7F) << shift;
    if (!(c & 0x80)) {
      out = r;
      return true;
    }
  }
  return false;
}

uint64_t littleEndian(std::string_view b, size_t i, size_t n) {
  uint64_t v = 0;
  for (size_t k = 0; k < n; ++k)
    v |= static_cast<uint64_t>(static_cast<uint8_t>(b[i + k])) << (8 * k);
  return v;
}

/** Whether these bytes decode as UTF-8.
 *
 *  Used only to tell a string field from a nested message, which is the one distinction this reader
 *  cannot get from the wire format: both are length-delimited. Rejecting overlong forms, surrogates
 *  and out-of-range code points is what keeps a random binary payload from being declared text --
 *  a wrong "yes" here would show raw bytes as a nickname. */
bool isUtf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    size_t extra = 0;
    uint32_t cp = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      extra = 1;
      cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      cp = c & 0x07u;
    } else {
      return false;
    }
    if (i + extra >= s.size()) return false;
    for (size_t k = 1; k <= extra; ++k) {
      const uint8_t cc = static_cast<uint8_t>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (extra == 1 && cp < 0x80) return false;
    if (extra == 2 && cp < 0x800) return false;
    if (extra == 3 && cp < 0x10000) return false;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += extra + 1;
  }
  return true;
}

}  // namespace

Message::Message(std::string_view bytes) : bytes_(bytes) {
  size_t i = 0;
  while (i < bytes.size() && fields_.size() < kMaxFields) {
    uint64_t key = 0;
    if (!readVarint(bytes, i, key)) return;
    const int number = static_cast<int>(key >> 3);
    const auto wire = static_cast<Wire>(key & 7);
    // Field number 0 is not legal, so a zero here means the framing is not what it looks like --
    // most likely because these bytes are not a message at all.
    if (number <= 0) return;
    switch (wire) {
      case Wire::Varint: {
        uint64_t v = 0;
        if (!readVarint(bytes, i, v)) return;
        fields_.push_back({number, wire, v, {}});
        break;
      }
      case Wire::Fixed64: {
        if (i + 8 > bytes.size()) return;
        fields_.push_back({number, wire, littleEndian(bytes, i, 8), {}});
        i += 8;
        break;
      }
      case Wire::Length: {
        uint64_t n = 0;
        if (!readVarint(bytes, i, n) || n > bytes.size() - i) return;
        fields_.push_back({number, wire, 0, bytes.substr(i, static_cast<size_t>(n))});
        i += static_cast<size_t>(n);
        break;
      }
      case Wire::Fixed32: {
        if (i + 4 > bytes.size()) return;
        fields_.push_back({number, wire, littleEndian(bytes, i, 4), {}});
        i += 4;
        break;
      }
      // The start/end group pair is protobuf's other encoding, retained for a schema style
      // deprecated for a decade and never read here. 6 and 7 are not assigned at all.
      case Wire::StartGroup:
      case Wire::EndGroup:
      default: return;
    }
  }
}

bool Message::has(int number) const {
  for (const Field& f : fields_)
    if (f.number == number) return true;
  return false;
}

uint64_t Message::num(int number) const {
  for (const Field& f : fields_)
    if (f.number == number) return f.wire == Wire::Varint ? f.value : 0;
  return 0;
}

std::string_view Message::bytes(int number) const {
  for (const Field& f : fields_)
    if (f.number == number) return f.wire == Wire::Length ? f.bytes : std::string_view();
  return {};
}

std::string_view Message::str(int number) const {
  const std::string_view b = bytes(number);
  return isUtf8(b) ? b : std::string_view();
}

Message Message::sub(int number) const { return Message(bytes(number)); }

}  // namespace dwm::pb

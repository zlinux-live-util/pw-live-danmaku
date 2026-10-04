#pragma once
// A schema-less reader for the protobuf blobs bilibili puts in `data.pb`.
//
// Why it exists at all: the entry and gift events stopped carrying JSON. Both now send one base64
// field that decodes to a protobuf message, and no schema for either is published -- so the field
// numbers this project reads were measured off the wire (room 21852, see docs/internals.md) and are
// pinned by tests/bili_test.cpp. A generated class would need a .proto that does not exist to
// generate from, which is the whole problem.
//
// Scope: read-only, no enums, no defaults, no unknown-field retention. Enough to pull a handful of
// numbers and strings out of a few hundred bytes, and nothing more. A field is decoded, never
// interpreted: the meaning of number 5 is something the caller knows from the measurement, not
// something this reader could know.
//
// Absence is the normal answer, not an error. The payloads come off the network, so a truncated or
// unknown-shaped blob is a fact to handle; a malformed field ends the scan and whatever was read
// before it is kept, which degrades to a message with less in it rather than to a dropped message.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dwm::pb {

/** One protobuf message, held as the bytes it arrived in.
 *
 *  Lookups are by field number and read the first occurrence: protobuf permits repeated fields and
 *  this project only reads scalars and submessages that bilibili sends once. Constructing one scans
 *  the bytes; the scan is O(fields) and happens on the site thread, never in the render path. */
class Message {
 public:
  /** The protobuf wire types, named by their numbers.
   *
   *  The numbers are spelled out because the reader casts the key's low three bits straight into
   *  this enum, and a plain sequential list would silently disagree with the wire format: an earlier
   *  revision declared Fixed32 fourth, which made it the value 3, so every message containing a
   *  32-bit field -- the gift's unit price among them -- stopped being scanned at that field, and
   *  everything after it read as absent. That looked like a field the platform had stopped sending
   *  rather than a parser that had given up half way. */
  enum class Wire {
    Varint = 0,
    Fixed64 = 1,
    Length = 2,
    StartGroup = 3,
    EndGroup = 4,
    Fixed32 = 5,
  };

  explicit Message(std::string_view bytes);

  /** Number of fields found before the scan stopped. */
  size_t size() const { return fields_.size(); }
  /** Whether a field with this number was found at all. */
  bool has(int number) const;

  /** The varint stored under `number`, or 0. A field of any other wire type reads as 0 rather than
   *  being reinterpreted, because reading a nested message's bytes as an integer would produce a
   *  plausible-looking wrong answer instead of an obviously missing one. */
  uint64_t num(int number) const;

  /** The length-delimited payload under `number`; empty when absent. Nested messages and strings
   *  are both length-delimited, so this is the raw form of either. */
  std::string_view bytes(int number) const;

  /** The payload under `number` read as text.
   *
   *  A payload that is not valid UTF-8 reads as empty. That is a cheap guard against handing raw
   *  bytes to pango as a nickname, and it is the *only* guard there is: without a schema, a string
   *  and a nested message are the same thing on the wire, and they cannot be told apart from the
   *  bytes alone. Measured: the gift's `gift_info` submessage is 229 bytes that are mostly URLs
   *  separated by small varints, and it passes as text -- it would be drawn as a row of noise.
   *
   *  What actually makes the calls correct is that the field numbers come from a capture, so each
   *  one is known to be the string it looks like. This accessor only stops a wrong field number
   *  from being catastrophic. */
  std::string_view str(int number) const;

  /** The payload under `number` read as a nested message. An absent field yields an empty message,
   *  whose own lookups all read as absent -- so a missing submessage costs a caller nothing. */
  Message sub(int number) const;

 private:
  struct Field {
    int number = 0;
    Wire wire = Wire::Varint;
    uint64_t value = 0;  // varint payload, or the little-endian bits of a fixed32/64
    std::string_view bytes;
  };

  /** Upper bound on fields kept. A field costs at least two bytes on the wire, so this only ever
   *  truncates a payload that is already far larger than the few hundred bytes these events send --
   *  and it bounds the vector for any input, which a network-facing parser should do regardless. */
  static constexpr size_t kMaxFields = 512;

  std::string_view bytes_;
  std::vector<Field> fields_;
};

}  // namespace dwm::pb

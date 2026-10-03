#pragma once
// Minimal read-only JSON reader.
//
// Scope: enough for the HTTP replies and the DANMU_MSG bodies of the bilibili live protocol.
// No serialisation, no streaming, no number formatting round-trip -- this is a parser only.
//
// Why not a library: the rest of the project deliberately depends on nothing outside the
// distribution's system libraries, and the only JSON it ever sees is one screenful of an API
// reply. A full parser would be most of the code in this file.
//
// Errors are reported by parse()'s return value and never by throwing: the payloads arrive from
// the network, so malformed input is a fact to handle, not an exceptional condition.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dwm {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;

  /** Parses one complete JSON value. Trailing whitespace is allowed; trailing data is not. */
  static Json parse(std::string_view text, std::string* error = nullptr);

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }

  /** String value, or an empty string for any other type. */
  const std::string& str() const;
  /** Number as int64. Truncates toward zero; returns 0 for any other type. */
  int64_t num() const;
  /** Number as double. Returns 0 for any other type. */
  double dbl() const;
  bool boolean() const { return bool_; }

  /** Number of elements (array) or members (object); 0 otherwise. */
  size_t size() const;

  /** Object member by key. Returns a static null Json when absent. */
  const Json& operator[](std::string_view key) const;
  /** Array element. Returns a static null Json when out of range. */
  const Json& operator[](size_t index) const;

  /** Element at index, or null. Same as operator[](size_t) but spelled out at call sites where the
   *  index is known to be numeric (bilibili's info[] arrays). */
  const Json& at(size_t index) const { return (*this)[index]; }

  /** Deep lookup by a chain of keys, e.g. path(Json, {"data", "token"}). Returns null when any
   *  step is missing. Saves the repetitive null checks at the call site. */
  static const Json& path(const Json& root, std::initializer_list<std::string_view> keys);

  /** Object member lookup by key. Null when absent or when this is not an object. */
  const Json& get(std::string_view key) const { return (*this)[key]; }

  bool has(std::string_view key) const { return !(*this)[key].isNull(); }

  /** Object members in document order. Needed to walk a keyed map -- bilibili's extra.emots maps
   *  an emote token such as "[热]" to its image record, and there is no other way to enumerate
   *  the keys. Returns an empty vector for any other type. */
  const std::vector<std::pair<std::string, Json>>& items() const { return obj_; }

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0.0;
  std::string str_;
  std::vector<Json> arr_;
  std::vector<std::pair<std::string, Json>> obj_;

  friend class JsonParser;
};

}  // namespace dwm
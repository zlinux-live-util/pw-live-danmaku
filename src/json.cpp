// Minimal read-only JSON reader. See json.hpp for the scope and for why this exists rather than a
// dependency.
//
// The two places that produce numbers for bilibili are timestamps (ms, fits int64) and the colour
// field, which is a decimal RGB integer -- strtod covers both without a separate integer path.
#include "json.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dwm {
namespace {

const Json& nullJson() {
  static const Json kNull;
  return kNull;
}

/** Appends a code point as UTF-8. Surrogate pairs are combined by the caller before reaching here
 *  (see parseString): a lone surrogate becomes U+FFFD rather than invalid bytes, because the
 *  alternative is emitting a malformed string that pango would reject or lay out wrongly. */
void appendUtf8(std::string& out, uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

/** Recursive-descent parser over a string_view. Depth is bounded so a hostile payload cannot
 *  blow the C stack: every level costs a Json object (roughly 100 bytes), and 200 levels is far
 *  beyond anything the bilibili protocol produces. */
class JsonParser {
 public:
  JsonParser(std::string_view t, std::string* err) : t_(t), err_(err) {}

  bool run(Json& out) {
    skipWs();
    if (!parseValue(out, 0)) return false;
    skipWs();
    if (p_ != t_.size()) return fail("trailing data");
    return true;
  }

 private:
  std::string_view t_;
  size_t p_ = 0;
  std::string* err_;

  bool fail(const char* what) {
    if (err_ && err_->empty()) {
      *err_ = what;
      char tail[64];
      const size_t at = p_ < t_.size() ? p_ : t_.size();
      std::snprintf(tail, sizeof(tail), " at offset %zu", at);
      *err_ += tail;
    }
    return false;
  }

  bool eof() const { return p_ >= t_.size(); }
  char peek() const { return t_[p_]; }

  void skipWs() {
    while (p_ < t_.size()) {
      const char c = t_[p_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++p_;
      } else {
        break;
      }
    }
  }

  bool literal(std::string_view lit) {
    if (t_.size() - p_ < lit.size() || t_.compare(p_, lit.size(), lit) != 0) return false;
    p_ += lit.size();
    return true;
  }

  bool parseValue(Json& out, int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    if (eof()) return fail("unexpected end of input");
    switch (peek()) {
      case '{': return parseObject(out, depth);
      case '[': return parseArray(out, depth);
      case '"': {
        out.type_ = Json::Type::String;
        return parseString(out.str_);
      }
      case 't':
        if (!literal("true")) return fail("bad literal");
        out.type_ = Json::Type::Bool;
        out.bool_ = true;
        return true;
      case 'f':
        if (!literal("false")) return fail("bad literal");
        out.type_ = Json::Type::Bool;
        out.bool_ = false;
        return true;
      case 'n':
        if (!literal("null")) return fail("bad literal");
        out.type_ = Json::Type::Null;
        return true;
      default: return parseNumber(out);
    }
  }

  bool parseObject(Json& out, int depth) {
    out.type_ = Json::Type::Object;
    ++p_;  // '{'
    skipWs();
    if (!eof() && peek() == '}') {
      ++p_;
      return true;
    }
    for (;;) {
      skipWs();
      if (eof() || peek() != '"') return fail("expected object key");
      std::string key;
      if (!parseString(key)) return false;
      skipWs();
      if (eof() || peek() != ':') return fail("expected ':'");
      ++p_;
      skipWs();
      Json value;
      if (!parseValue(value, depth + 1)) return false;
      out.obj_.emplace_back(std::move(key), std::move(value));
      skipWs();
      if (eof()) return fail("unterminated object");
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() == '}') {
        ++p_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool parseArray(Json& out, int depth) {
    out.type_ = Json::Type::Array;
    ++p_;  // '['
    skipWs();
    if (!eof() && peek() == ']') {
      ++p_;
      return true;
    }
    for (;;) {
      skipWs();
      Json value;
      if (!parseValue(value, depth + 1)) return false;
      out.arr_.push_back(std::move(value));
      skipWs();
      if (eof()) return fail("unterminated array");
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() == ']') {
        ++p_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parseString(std::string& out) {
    out.clear();
    ++p_;  // opening quote
    for (;;) {
      if (eof()) return fail("unterminated string");
      const char c = t_[p_++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);  // raw UTF-8 passes through unchanged
        continue;
      }
      if (eof()) return fail("unterminated escape");
      const char e = t_[p_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (t_.size() - p_ < 4) return fail("truncated \\u escape");
          uint32_t cp = 0;
          for (int i = 0; i < 4; ++i) {
            const int d = hexDigit(t_[p_ + i]);
            if (d < 0) return fail("bad \\u escape");
            cp = (cp << 4) | static_cast<uint32_t>(d);
          }
          p_ += 4;
          // A high surrogate must be followed by \uDC00-\uDFFF; anything else yields U+FFFD.
          if (cp >= 0xD800 && cp <= 0xDBFF && t_.size() - p_ >= 6 && t_[p_] == '\\' &&
              t_[p_ + 1] == 'u') {
            uint32_t lo = 0;
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
              const int d = hexDigit(t_[p_ + 2 + i]);
              if (d < 0) { ok = false; break; }
              lo = (lo << 4) | static_cast<uint32_t>(d);
            }
            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
              p_ += 6;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              cp = 0xFFFD;
            }
          }
          appendUtf8(out, cp);
          break;
        }
        default: return fail("bad escape");
      }
    }
  }

  bool parseNumber(Json& out) {
    const char* begin = t_.data() + p_;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(begin, &end);
    if (end == begin) return fail("bad number");
    if (errno == ERANGE && (v > 1e308 || v < -1e308)) return fail("number out of range");
    p_ += static_cast<size_t>(end - begin);
    out.type_ = Json::Type::Number;
    out.num_ = v;
    return true;
  }

  static constexpr int kMaxDepth = 200;
};

Json Json::parse(std::string_view text, std::string* error) {
  if (error) error->clear();
  Json root;
  JsonParser parser(text, error);
  if (!parser.run(root)) return Json();
  return root;
}

const std::string& Json::str() const {
  static const std::string kEmpty;
  return type_ == Type::String ? str_ : kEmpty;
}

int64_t Json::num() const {
  if (type_ != Type::Number) return 0;
  // Double cannot hold every int64 exactly, but the values crossing this boundary are none
  // (bilibili timestamps are ~1.8e12, well under 2^53), so the double is a faithful carrier.
  return static_cast<int64_t>(num_);
}

double Json::dbl() const { return type_ == Type::Number ? num_ : 0.0; }

size_t Json::size() const {
  if (type_ == Type::Array) return arr_.size();
  if (type_ == Type::Object) return obj_.size();
  return 0;
}

const Json& Json::operator[](std::string_view key) const {
  if (type_ != Type::Object) return nullJson();
  for (const auto& kv : obj_) {
    if (kv.first == key) return kv.second;
  }
  return nullJson();
}

const Json& Json::operator[](size_t index) const {
  if (type_ != Type::Array || index >= arr_.size()) return nullJson();
  return arr_[index];
}

const Json& Json::path(const Json& root, std::initializer_list<std::string_view> keys) {
  const Json* cur = &root;
  for (std::string_view k : keys) {
    cur = &(*cur)[k];
    if (cur->isNull()) return *cur;  // caller checks isNull(); bailing early avoids the rest
  }
  return *cur;
}

}  // namespace dwm
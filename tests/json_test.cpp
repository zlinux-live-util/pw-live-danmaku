// Unit tests for the JSON reader. Plain asserts, no framework: the build target is two lines in
// the Makefile and the alternative is a dependency this project does not otherwise have.
//
// What is tested is the behaviour the protocol layer depends on: escapes, surrogate pairs,
// malformed input being reported rather than thrown, and the member/element lookups returning a
// null Json instead of going out of range.
#include <cstdio>
#include <cstring>
#include <string>

#include "json.hpp"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

template <typename A, typename B>
void checkEq(const A& got, const B& want, const char* what) {
  if (!(got == want)) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

using dwm::Json;

void testScalars() {
  std::string err;
  const Json j = Json::parse(R"({"a":1,"b":2.5,"c":"x","d":true,"e":null,"f":[1,2,3]})", &err);
  check(err.empty(), "scalars: no parse error");
  checkEq(j.size(), size_t(6), "scalars: member count");
  checkEq(j["a"].num(), int64_t(1), "scalars: integer");
  checkEq(j["b"].dbl(), 2.5, "scalars: double");
  checkEq(j["c"].str(), std::string("x"), "scalars: string");
  checkEq(j["d"].boolean(), true, "scalars: bool");
  check(j["e"].isNull(), "scalars: null");
  checkEq(j["f"].size(), size_t(3), "scalars: array length");
  checkEq(j["f"].at(1).num(), int64_t(2), "scalars: array index");
}

void testEscapes() {
  std::string err;
  const Json j = Json::parse(R"({"s":"a\"b\\c\/d\ne\tfAé中"})", &err);
  check(err.empty(), "escapes: no parse error");
  // é as U+00E9 and 中 as U+4E2D are three bytes each in UTF-8.
  checkEq(j["s"].str(), std::string("a\"b\\c/d\ne\tfA\xC3\xA9\xE4\xB8\xAD"),
          "escapes: \\u00E9 and \\u4E2D become UTF-8");
}

void testSurrogatePair() {
  std::string err;
  // U+1F600 GRINNING FACE as a surrogate pair.
  const Json j = Json::parse(R"({"s":"😀"})", &err);
  check(err.empty(), "surrogate: no parse error");
  checkEq(j["s"].str().size(), size_t(4), "surrogate: four UTF-8 bytes");
  checkEq(j["s"].str(), std::string("\xF0\x9F\x98\x80"), "surrogate: correct code point");
}

void testRawUtf8PassesThrough() {
  // Danmaku bodies carry raw UTF-8, not \u escapes. It must survive untouched.
  // A normal string literal, not a raw one: \x escapes are not expanded inside R"(...)".
  const std::string raw = "{\"info\":[\"\xe4\xba\xba\xe9\x80\x9a\"]}";
  std::string err;
  const Json j = Json::parse(raw, &err);
  check(err.empty(), "raw utf8: no parse error");
  checkEq(j["info"].at(0).str(), std::string("\xE4\xBA\xBA\xE9\x80\x9A"), "raw utf8: preserved");
}

void testMissingAndMalformed() {
  std::string err;
  // A missing key must read as null, not crash and not throw.
  const Json j = Json::parse(R"({"a":1})", &err);
  check(j["nope"].isNull(), "missing key: reads as null");
  check(j["a"]["deeper"].isNull(), "indexing a non-container: reads as null");
  check(j.at(99).isNull(), "out-of-range index: reads as null");

  // Malformed input must be reported through the return value.
  for (const char* bad : {"{", "{\"a\":}", "[1,2", "tru", "{\"a\":1}}", "", "\"unterminated"}) {
    std::string e;
    const Json r = Json::parse(bad, &e);
    check(r.isNull() && !e.empty(), "malformed input is reported, not thrown");
  }
}

void testPath() {
  std::string err;
  const Json j =
      Json::parse(R"({"code":0,"data":{"host_list":[{"host":"h","wss_port":2245}]}})", &err);
  checkEq(Json::path(j, {"code"}).num(), int64_t(0), "path: top level");
  checkEq(Json::path(j, {"data", "host_list"}).at(0)["host"].str(), std::string("h"),
          "path: through an array");
  check(Json::path(j, {"data", "absent"}).isNull(), "path: missing step reads as null");
}

void testDeepNestingIsRejected() {
  // The parser recurses, so the depth bound is what stops a hostile payload from unwinding the
  // stack. 500 levels is past the bound and must be refused.
  std::string deep(500, '[');
  std::string err;
  Json::parse(deep, &err);
  check(!err.empty(), "deep nesting: refused with an error");
}

void testNumbers() {
  std::string err;
  // A bilibili danmaku timestamp: ~1.8e12, beyond 32 bits but exact in a double.
  const Json j = Json::parse(R"({"ts":1791046553806,"rgb":16777215,"neg":-5})", &err);
  checkEq(j["ts"].num(), int64_t(1791046553806), "numbers: large int64 exact");
  checkEq(j["rgb"].num(), int64_t(16777215), "numbers: decimal colour");
  checkEq(j["neg"].num(), int64_t(-5), "numbers: negative");
}

}  // namespace

int main() {
  testScalars();
  testEscapes();
  testSurrogatePair();
  testRawUtf8PassesThrough();
  testMissingAndMalformed();
  testPath();
  testDeepNestingIsRejected();
  testNumbers();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("json: all checks passed\n");
  return 0;
}
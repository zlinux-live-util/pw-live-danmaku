// Unit tests for the bilibili protocol helpers: WBI signing, packet framing, and DANMU_MSG parsing.
//
// The vectors here are measured values, not invented ones. The mixin key is a real pair of site
// keys and the expected output is what the web client computes from them; md5 and percent-encoding
// use the published test vectors. A regression in any of these would show up as an API refusing
// the request with code -352, which is expensive to debug from the outside, so it is pinned here.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bili.hpp"
#include "json.hpp"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

void checkEq(const std::string& got, const std::string& want, const char* what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  got:  %s\n  want: %s\n", what, got.c_str(), want.c_str());
    ++failures;
  }
}

/** Numeric counterpart. Deliberately a separate name rather than an overload: a template taking
 *  any type would also match const char* pairs and win over the string overload for literals. */
template <typename A, typename B>
void checkEqInt(A got, B want, const char* what) {
  if (!(got == want)) {
    std::fprintf(stderr, "FAIL: %s (got %lld, want %lld)\n", what,
                 static_cast<long long>(got), static_cast<long long>(want));
    ++failures;
  }
}

using namespace dwm;

// A real site key pair, and the mixin key derived from it. Measured: the web client turns these two
// into the string on the right, and getDanmuInfo answers code 0 with the signature built from it.
void testMixinKey() {
  const std::string img = "7cd084941338484aae1ad9425b84077c";
  const std::string sub = "4932caff0ff746eab6f01bf08b70ac45";
  checkEq(Bili::mixinKey(img, sub), "ea1db124af3c7062474693fa704f4ff8", "mixinKey: measured pair");
  checkEqInt(Bili::mixinKey(img, sub).size(), size_t(32), "mixinKey: 32 characters");
  // Short input must be refused rather than read out of bounds.
  check(Bili::mixinKey("abc", "def").empty(), "mixinKey: short input refused");
  check(Bili::mixinKey("", "").empty(), "mixinKey: empty input refused");
}

void testMd5() {
  checkEq(Bili::md5Hex(""), "d41d8cd98f00b204e9800998ecf8427e", "md5: empty string");
  checkEq(Bili::md5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72", "md5: abc");
}

void testPercentEncode() {
  checkEq(Bili::percentEncode("abcXYZ019"), "abcXYZ019", "percentEncode: unreserved kept");
  checkEq(Bili::percentEncode("a b"), "a%20b", "percentEncode: space");
  checkEq(Bili::percentEncode("/?&="), "%2F%3F%26%3D", "percentEncode: reserved escaped");
  checkEq(Bili::percentEncode("-_.~"), "-_.~", "percentEncode: the four unreserved marks");
}

/** Builds a packet the way the protocol does, so the tests read like the wire format. */
std::string makePacket(uint16_t proto, uint32_t type, const std::string& body,
                       uint16_t headerSize = 16) {
  std::string out;
  const uint32_t total = headerSize + static_cast<uint32_t>(body.size());
  auto be16 = [&](uint16_t v) {
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
  };
  auto be32 = [&](uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
  };
  be32(total);
  be16(headerSize);
  be16(proto);
  be32(type);
  be32(1);  // sequence
  out.append(body);
  return out;
}

void testPacketFraming() {
  const std::string doc1 = R"({"cmd":"DANMU_MSG"})";
  const std::string doc2 = R"({"cmd":"NOTICE_MSG"})";
  const std::string wire = makePacket(kProtoCommand, kTypeCommand, doc1) +
                           makePacket(kProtoCommand, kTypeCommand, doc2);

  std::string_view in(wire);
  PacketHeader h;
  std::string_view body;
  int seen = 0;
  while (Bili::nextPacket(in, h, body)) {
    checkEqInt(h.type, kTypeCommand, "framing: type field");
    checkEq(std::string(body), seen == 0 ? doc1 : doc2, "framing: body in order");
    ++seen;
  }
  checkEqInt(seen, 2, "framing: both packets walked");
  check(in.empty(), "framing: buffer fully consumed");
}

void testFramingResync() {
  // A truncated tail must stop the walk, not hand back a body read from the wrong offset.
  const std::string doc = R"({"cmd":"X"})";
  std::string wire = makePacket(kProtoCommand, kTypeCommand, doc);
  wire.resize(wire.size() - 4);  // chop the last four bytes

  std::string_view in(wire);
  PacketHeader h;
  std::string_view body;
  const bool got = Bili::nextPacket(in, h, body);
  // The first header now claims more bytes than remain, so it must be refused.
  check(!got, "framing: truncated packet refused");

  // A totalSize below the header is nonsense and must not be trusted.
  std::string bogus(16, '\0');
  bogus[3] = 4;  // totalSize = 4 < headerSize
  std::string_view bin(bogus);
  check(!Bili::nextPacket(bin, h, body), "framing: totalSize below headerSize refused");
}

// The authentication reply really does arrive as proto=3 with a plain JSON body. Trusting proto
// there is what made an early revision report "auth reply could not be decompressed".
void testAuthReplyShape() {
  const std::string wire = makePacket(kProtoCommandBrotli, kTypeAuthResp, R"({"code":0})");
  std::vector<std::string> docs;
  Bili::forEachJson(wire, [&](std::string_view doc) {
    docs.emplace_back(doc);
    return true;
  });
  checkEqInt(docs.size(), size_t(1), "auth reply: one document");
  if (!docs.empty()) checkEq(docs[0], R"({"code":0})", "auth reply: body passed through as JSON");
}

void testForEachJsonBatch() {
  // A decompressed command body is a run of framed packets; each must be visited separately.
  // forEachJson takes the whole message, so the inner run is wrapped in an outer frame. Using
  // proto 0 for the wrapper keeps the test free of a compression step while exercising exactly the
  // nested walk that brotli output goes through.
  const std::string inner = makePacket(kProtoCommand, kTypeCommand, R"({"cmd":"A"})") +
                            makePacket(kProtoCommand, kTypeCommand, R"({"cmd":"B"})");
  const std::string wire = makePacket(kProtoCommand, kTypeCommand, inner);

  std::vector<std::string> cmds;
  Bili::forEachJson(wire, [&](std::string_view doc) {
    cmds.push_back(Json::parse(doc)["cmd"].str());
    return true;
  });
  checkEqInt(cmds.size(), size_t(2), "batch: two documents");
  if (cmds.size() == 2) {
    checkEq(cmds[0], "A", "batch: first cmd");
    checkEq(cmds[1], "B", "batch: second cmd");
  }
}

void testForEachJsonStopsEarly() {
  const std::string inner = makePacket(kProtoCommand, kTypeCommand, R"({"cmd":"A"})") +
                            makePacket(kProtoCommand, kTypeCommand, R"({"cmd":"B"})");
  const std::string wire = makePacket(kProtoCommand, kTypeCommand, inner);
  int visited = 0;
  Bili::forEachJson(wire, [&](std::string_view) {
    ++visited;
    return false;  // stop
  });
  checkEqInt(visited, 1, "visitor: early stop honoured");
}

// A real DANMU_MSG body as it arrives on the wire (nickname masked, because the capture was made
// without a login cookie). One "info" key holding every element: info[0] is the mode array,
// info[1] the text, info[2] the sender, info[9] the user/extra object.
void testParseDanmaku() {
  const std::string body =
      R"({"cmd":"DANMU_MSG","dm_v2":"","info":[)"
      R"([0,1,25,16777215,1791046553806,-1688173992,0,"f9d986db",0,0,0,"",0,"{}","{}",)"
      R"({"extra":"{\"mode\":1,\"dm_type\":0}"}],)"
      R"("老丈人只为名",)"
      R"([0,"旱獭邮箱_",0,0,0,10000,1,""],)"
      R"([10,0,16777215,6406235,"{}",0],["",""],0,0,0,0,)"
      R"({"extra":"{\"mode\":1,\"dm_type\":0}","user":{"base":{"name":"旱獭邮箱_"}}}]})";

  Danmaku d;
  const bool parsed = Bili::parseDanmaku(Json::parse(body), d);
  check(parsed, "DANMU_MSG: recognised");
  if (!parsed) return;
  checkEq(d.text, "老丈人只为名", "DANMU_MSG: text from info[1]");
  checkEq(d.user, "旱獭邮箱_", "DANMU_MSG: masked nickname from info[2][1]");
  checkEqInt(d.mode, 1, "DANMU_MSG: scroll mode");
  checkEqInt(d.fontSize, 25, "DANMU_MSG: font size");
  checkEqInt(d.color, 0xFFFFFFu, "DANMU_MSG: colour");
  checkEq(std::to_string(d.timestampMs), "1791046553806", "DANMU_MSG: timestamp");
}

void testParseDanmakuEmote() {
  // Emote danmaku carry the literal token in the text, e.g. "[夏日热浪_想要]".
  const std::string body =
      R"({"cmd":"DANMU_MSG","info":[)"
      R"([0,1,25,16777215,1791046559000,1,0,"h",0,0,0,"",0,"{}","{}",)"
      R"({"extra":"{\"dm_type\":1}"}],)"
      R"("[夏日热浪_想要]",)"
      R"([0,"user",0,0,0,10000,1,""],[10,0,16777215,0,"{}",0],["",""],0,0,0,0,)"
      R"({"extra":"{\"dm_type\":1}","user":{"base":{"name":"user"}}}]})";
  Danmaku d;
  check(Bili::parseDanmaku(Json::parse(body), d), "emote danmaku: recognised");
  checkEq(d.text, "[夏日热浪_想要]", "emote danmaku: token preserved verbatim");
}

void testParseIgnoresOtherCmds() {
  for (const char* cmd : {"LOG_IN_NOTICE", "NOTICE_MSG", "WATCHED_CHANGE", "ONLINE_RANK_COUNT",
                          "STOP_LIVE_ROOM_LIST", "SEND_GIFT"}) {
    Danmaku d;
    check(!Bili::parseDanmaku(Json::parse(std::string(R"({"cmd":")") + cmd + R"("})"), d),
          cmd);
  }
}

void testAuthPacketLayout() {
  const std::string p = Bili::authPacket(12345, "TOKEN");
  check(p.size() > 16, "auth packet: has a body");
  checkEq(std::string(p.substr(8, 4)), std::string("\x00\x00\x00\x07", 4), "auth packet: type is 7");
  // The body must be JSON: that is the whole finding this milestone recorded.
  const std::string body = p.substr(16);
  checkEq(body.substr(0, 1), "{", "auth packet: JSON body");
  std::string err;
  const Json j = Json::parse(body, &err);
  check(err.empty(), "auth packet: body parses as JSON");
  checkEq(std::to_string(j["roomid"].num()), "12345", "auth packet: roomid");
  checkEq(j["key"].str(), "TOKEN", "auth packet: key");
  checkEq(std::to_string(j["protover"].num()), "3", "auth packet: protover");
}

}  // namespace

int main() {
  testMixinKey();
  testMd5();
  testPercentEncode();
  testPacketFraming();
  testFramingResync();
  testAuthReplyShape();
  testForEachJsonBatch();
  testForEachJsonStopsEarly();
  testParseDanmaku();
  testParseDanmakuEmote();
  testParseIgnoresOtherCmds();
  testAuthPacketLayout();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("bili: all checks passed\n");
  return 0;
}
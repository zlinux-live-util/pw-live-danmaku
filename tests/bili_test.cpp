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
#include "message.hpp"

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

// A DANMU_MSG body shaped like the one measured on the wire: one "info" array holding everything,
// with info[0][15] carrying both "extra" (a JSON string) and "user". Using the real shape matters
// here -- an earlier revision read the extras from info[9], which is a timestamp object, so every
// field it tried to read was silently absent and the tests passed against a fixture with the same
// mistake in it.
void testParseDanmaku() {
  // A custom raw-string delimiter (R"J(...)J") rather than the default: these fixtures contain
  // }] and ,) sequences, and the default R"( ... )" would terminate on the first )" it met.
  const std::string body = R"J({"cmd":"DANMU_MSG","dm_v2":"","info":[
   [0,1,25,16777215,1791046553806,-1688173992,0,"f9d986db",0,0,0,"",0,"{}","{}",
    {"mode":0,"show_player_type":0,
     "extra":"{\"mode\":1,\"dm_type\":0}",
     "user":{"base":{"face":"https://i1.hdslb.com/bfs/face/abc.jpg","name":"旱獭邮箱_",
                     "name_color":16711680},
             "medal":{"guard_level":3,"name":"德云色"}}}],
   "老丈人只为名",
   [0,"旱獭邮箱_",0,0,0,10000,1,""],
   [10,0,16777215,6406235,"{}",0],["",""],0,0,0,0,
   {"ct":"AFFF4206","ts":1791046553},0,0,null,null,0,1040,[49],null]})J";

  Message m;
  const bool parsed = Bili::parseMessage(Json::parse(body), m);
  check(parsed, "DANMU_MSG: recognised");
  if (!parsed) return;
  checkEq(m.plainText(), "老丈人只为名", "DANMU_MSG: text from info[1]");
  checkEq(m.user, "旱獭邮箱_", "DANMU_MSG: name");
  checkEq(m.avatarUrl, "https://i1.hdslb.com/bfs/face/abc.jpg", "DANMU_MSG: face from info[0][15]");
  checkEqInt(m.userColor, 16711680u, "DANMU_MSG: name colour");
  checkEqInt(static_cast<int>(m.type), static_cast<int>(UserType::Member),
            "DANMU_MSG: guard_level 3 becomes Member");
  checkEqInt(m.tsMs, 1791046553806LL, "DANMU_MSG: timestamp");
  checkEqInt(static_cast<int>(m.kind), static_cast<int>(MsgKind::Text), "DANMU_MSG: plain text kind");
  checkEqInt(m.parts.size(), size_t(1), "DANMU_MSG: no emotes, so one text fragment");
}

// Emote danmaku: the token is literal in the body and extra.emots maps it to an image.
void testParseDanmakuEmote() {
  const std::string body = R"J({"cmd":"DANMU_MSG","info":[
   [0,1,25,16777215,1791046559000,1,0,"h",0,0,0,"",0,"{}","{}",
    {"extra":"{\"mode\":1,\"dm_type\":1,\"emots\":{\"[夏日热浪_想要]\":{\"url\":\"https://i0.hdslb.com/bfs/live/x.png\",\"width\":20,\"height\":20}}}",
     "user":{"base":{"name":"user","face":""}}}],
   "白花300块[夏日热浪_想要]",
   [0,"user",0,0,0,10000,1,""],
   [10,0,0,0,"{}",0],["",""],0,0,0,0,
   {"ct":"x","ts":1},0,0,null,null,0,1,[1],null]})J";

  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "emote danmaku: recognised");
  // Three fragments: the leading text, the emote, and nothing trailing.
  checkEqInt(m.parts.size(), size_t(2), "emote danmaku: text split around the token");
  if (m.parts.size() == 2) {
    checkEq(m.parts[0].text, "白花300块", "emote danmaku: leading text run");
    checkEqInt(static_cast<int>(m.parts[1].kind), static_cast<int>(Fragment::Kind::Emote),
              "emote danmaku: second fragment is an emote");
    checkEq(m.parts[1].text, "[夏日热浪_想要]", "emote danmaku: token kept as typed");
    checkEq(m.parts[1].url, "https://i0.hdslb.com/bfs/live/x.png", "emote danmaku: image url");
    checkEqInt(m.parts[1].px, 20, "emote danmaku: advertised size");
  }
  // Brackets that the platform did not advertise stay literal text.
  checkEq(m.plainText(), "白花300块[夏日热浪_想要]", "emote danmaku: plain text round-trips");
}

// A streamer's own uploaded emote (主播自定义表情). Measured on the wire in room 545068, cut down to
// the fields this reads: info[0][12] is 1, info[0][13] carries the picture, and extra.emots -- the
// map the inline-token case uses -- is null. The body is the emote's name, without brackets.
void testParseDanmakuStreamerEmote() {
  const std::string body = R"J({"cmd":"DANMU_MSG","info":[
   [0,1,25,16777215,1791065364000,1,0,"h",0,0,0,"",
    1,
    {"bulge_display":1,"emoticon_unique":"room_545068_9780","height":162,"in_player_area":1,
     "is_dynamic":0,"url":"http://i0.hdslb.com/bfs/live/46ec99d09a2b8c84314f0c506a55660ee08a9069.png",
     "width":162},
    {},
    {"extra":"{\"mode\":0,\"dm_type\":1,\"emoticon_unique\":\"room_545068_9780\",\"emots\":null}",
     "user":{"base":{"name":"钱少奢侈硬享受","face":"https://i1.hdslb.com/bfs/face/a.jpg"}}}],
   "嘻嘻",
   [0,"钱少奢侈硬享受",0,0,0,10000,1,""],
   [10,0,0,0,"{}",0],["",""],0,0,0,0,
   {"ct":"x","ts":1},0,0,null,null,0,1,[1],null]})J";

  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "streamer emote: recognised");
  checkEqInt(m.parts.size(), size_t(1), "streamer emote: the whole body is the picture");
  if (m.parts.size() == 1) {
    checkEqInt(static_cast<int>(m.parts[0].kind), static_cast<int>(Fragment::Kind::Emote),
              "streamer emote: one emote fragment");
    checkEq(m.parts[0].url,
            "https://i0.hdslb.com/bfs/live/46ec99d09a2b8c84314f0c506a55660ee08a9069.png",
            "streamer emote: url from info[0][13], upgraded to https");
    checkEq(m.parts[0].text, "嘻嘻", "streamer emote: name kept as the text fallback");
    checkEqInt(m.parts[0].px, 162, "streamer emote: advertised size");
  }
  checkEq(m.plainText(), "嘻嘻", "streamer emote: plain text round-trips");
}

// The same emote with info[0][13] as a JSON string instead of a nested object, which is how some
// payloads spell it (info[0][14] in the fixture above is an object where the wire had "{}").
void testParseDanmakuEmoteOptionsAsString() {
  const std::string body = R"J({"cmd":"DANMU_MSG","info":[
   [0,1,25,16777215,1,1,0,"h",0,0,0,"",1,
    "{\"emoticon_unique\":\"official_13\",\"height\":60,\"width\":183,\"url\":\"https://i0.hdslb.com/bfs/live/a98e359.png\"}",
    "{}",
    {"extra":"{\"mode\":0,\"dm_type\":1,\"emots\":null}","user":{"base":{"name":"u","face":""}}}],
   "妙啊",
   [0,"u",0,0,0,10000,1,""],
   [10,0,0,0,"{}",0],["",""],0,0,0,0,
   {"ct":"x","ts":1},0,0,null,null,0,1,[1],null]})J";

  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "emote options as string: recognised");
  checkEqInt(m.parts.size(), size_t(1), "emote options as string: one emote fragment");
  if (m.parts.size() == 1) {
    checkEq(m.parts[0].url, "https://i0.hdslb.com/bfs/live/a98e359.png",
            "emote options as string: url read out of the string");
    checkEqInt(m.parts[0].px, 60, "emote options as string: advertised size");
  }
}

// A dm_type of 0 with no advertised emote must not be mistaken for one: the whole point of reading
// info[0][12] rather than trusting the body's shape.
void testParseDanmakuTextWithBrackets() {
  const std::string body = R"J({"cmd":"DANMU_MSG","info":[
   [0,1,25,16777215,1,1,0,"h",0,0,0,"",0,"{}","{}",
    {"extra":"{\"mode\":0,\"dm_type\":0,\"emots\":null}","user":{"base":{"name":"u","face":""}}}],
   "[嘻嘻]",
   [0,"u",0,0,0,10000,1,""],
   [10,0,0,0,"{}",0],["",""],0,0,0,0,
   {"ct":"x","ts":1},0,0,null,null,0,1,[1],null]})J";

  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "brackets without dm_type: recognised");
  checkEqInt(m.parts.size(), size_t(1), "brackets without dm_type: one text run");
  if (m.parts.size() == 1) {
    checkEqInt(static_cast<int>(m.parts[0].kind), static_cast<int>(Fragment::Kind::Text),
              "brackets without dm_type: stays text");
    checkEq(m.parts[0].text, "[嘻嘻]", "brackets without dm_type: brackets are literal");
  }
}

// A 收藏集 / 表情包 emote: same carrier as the streamer's own, but the body keeps its brackets and
// emoticon_unique is the literal string "upower_[token]". Measured in room 1921271623, whose chat is
// full of these; the token reported in the bug this fixed was "[Neuro sama收藏集_晚安]".
void testParseDanmakuBracketedEmoteToken() {
  const std::string body = R"J({"cmd":"DANMU_MSG","info":[
   [0,1,25,16777215,1791070000000,1,0,"h",0,0,0,"",1,
    {"bulge_display":1,"emoticon_unique":"upower_[Neuro sama收藏集_晚安]","height":20,
     "in_player_area":1,"is_dynamic":0,
     "url":"https://i0.hdslb.com/bfs/garb/bf7dc14be1fe85256bb2b7237802a29d443b5664.png",
     "width":20},
    {},
    {"extra":"{\"mode\":0,\"dm_type\":1,\"emoticon_unique\":\"upower_[Neuro sama收藏集_晚安]\",\"emots\":null}",
     "user":{"base":{"name":"帕金尼","face":"https://i1.hdslb.com/bfs/face/a.jpg"}}}],
   "[Neuro sama收藏集_晚安]",
   [0,"帕金尼",0,0,0,10000,1,""],
   [10,0,0,0,"{}",0],["",""],0,0,0,0,
   {"ct":"x","ts":1},0,0,null,null,0,1,[1],null]})J";

  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "bracketed emote token: recognised");
  // One fragment, and it is a picture: the brackets are part of the emote's name, not a token to
  // look up. Treating them as text is exactly what this used to do.
  checkEqInt(m.parts.size(), size_t(1), "bracketed emote token: the whole body is the picture");
  if (m.parts.size() == 1) {
    checkEqInt(static_cast<int>(m.parts[0].kind), static_cast<int>(Fragment::Kind::Emote),
              "bracketed emote token: one emote fragment");
    checkEq(m.parts[0].url,
            "https://i0.hdslb.com/bfs/garb/bf7dc14be1fe85256bb2b7237802a29d443b5664.png",
            "bracketed emote token: url from info[0][13]");
    checkEqInt(m.parts[0].px, 20, "bracketed emote token: advertised size");
  }
  // The name is kept so the row still reads if the picture never arrives.
  checkEq(m.plainText(), "[Neuro sama收藏集_晚安]", "bracketed emote token: plain text round-trips");
}

// The two card kinds follow the documented field tables but have not been observed on this
// machine, so these tests pin the mapping rather than a capture.
void testParsePaidCard() {
  const std::string body = R"J({"cmd":"SUPER_CHAT_MESSAGE","data":{"uname":"五条悟","face":"https://f/x.jpg","message":"已经没有什么可怕的了","price":30,"start_time":1791046553,"medal_info":{"guard_level":3}}})J";
  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "SUPER_CHAT_MESSAGE: recognised");
  checkEqInt(static_cast<int>(m.kind), static_cast<int>(MsgKind::Paid), "SC: kind is Paid");
  checkEq(m.user, "五条悟", "SC: user");
  checkEq(m.plainText(), "已经没有什么可怕的了", "SC: message");
  checkEq(m.amount, "CN¥30", "SC: price");
  checkEqInt(m.tsMs, 1791046553000LL, "SC: start_time is seconds, converted to ms");
}

void testParseMembershipCard() {
  // price is in gold, and 1000 gold is one yuan.
  const std::string body = R"J({"cmd":"GUARD_BUY","data":{"uid":1,"username":"xfgryujk","guard_level":3,"num":1,"price":198000,"start_time":1791046553}})J";
  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "GUARD_BUY: recognised");
  checkEqInt(static_cast<int>(m.kind), static_cast<int>(MsgKind::Membership), "GUARD: kind");
  checkEq(m.user, "xfgryujk", "GUARD: user");
  checkEq(m.amount, "舰长 · CN¥198", "GUARD: tier and price converted from gold");
  checkEqInt(static_cast<int>(m.type), static_cast<int>(UserType::Member), "GUARD: membership tier");
}

void testSplitFragments() {
  std::vector<std::pair<std::string, Fragment>> emotes;
  Fragment f;
  f.kind = Fragment::Kind::Emote;
  f.text = "[笑哭]";
  emotes.emplace_back("[笑哭]", f);

  std::vector<Fragment> out;
  // The bracketed run the platform did not advertise must not be swallowed.
  Bili::splitFragments("a[not an emote]b[笑哭]c", emotes, out);
  checkEqInt(out.size(), size_t(3), "split: text, emote, trailing text");
  if (out.size() == 3) {
    checkEq(out[0].text, "a[not an emote]b", "split: unknown brackets stay in the text run");
    checkEq(out[1].text, "[笑哭]", "split: the advertised token became a fragment");
    checkEq(out[2].text, "c", "split: trailing text");
  }

  Bili::splitFragments("", emotes, out);
  checkEqInt(out.size(), size_t(0), "split: empty text yields no fragments");
  Bili::splitFragments("plain", emotes, out);
  checkEqInt(out.size(), size_t(1), "split: no match yields one text run");
}

void testParseIgnoresOtherCmds() {
  for (const char* cmd : {"LOG_IN_NOTICE", "NOTICE_MSG", "WATCHED_CHANGE", "ONLINE_RANK_COUNT",
                          "STOP_LIVE_ROOM_LIST", "SEND_GIFT", "INTERACT_WORD", "SUPER_CHAT_MESSAGE"
                                                                              "_UNKNOWN"}) {
    Message m;
    check(!Bili::parseMessage(Json::parse(std::string(R"({"cmd":")") + cmd + R"("})"), m), cmd);
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
  testParseDanmakuStreamerEmote();
  testParseDanmakuBracketedEmoteToken();
  testParseDanmakuEmoteOptionsAsString();
  testParseDanmakuTextWithBrackets();
  testSplitFragments();
  testParsePaidCard();
  testParseMembershipCard();
  testParseIgnoresOtherCmds();
  testAuthPacketLayout();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("bili: all checks passed\n");
  return 0;
}
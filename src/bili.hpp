#pragma once
// Bilibili live chat: HTTP bootstrap and the websocket packet protocol.
//
// Two things are non-obvious here and both were established by measurement rather than by reading
// documentation; docs/internals.md carries the reproducible commands. In short:
//
//  * getDanmuInfo refuses to answer (code -352, bilibili's risk control) unless the request is
//    WBI-signed *and* carries a buvid3 cookie plus browser-like headers. The cookie can be the
//    anonymous one from /x/frontend/finger/spi; a logged-in cookie is only needed to see nicknames.
//  * The authentication packet body is JSON. The protobuf ClientVerifyReq that most open-source
//    clients send is answered by an immediate disconnect -- four schema variants were tried.
//
// Nothing here blocks the render thread: it is all called from the site thread.
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "json.hpp"
#include "message.hpp"

namespace dwm {

struct BiliConfig {
  /**
   * Raw cookie header from the user's browser, e.g. "SESSDATA=...; bili_jct=...; DedeUserID=...".
   * Optional: without it the connection is anonymous, which shows the danmaku but masks every
   * nickname. Never logged, and --cookie prints a warning that it stays visible in ps(1).
   */
  std::string cookie;
  /** Sent as User-Agent and Origin. A browser-like one is part of what the API checks. */
  std::string userAgent;
};

struct DanmuEndpoint {
  std::string host;
  int wssPort = 2245;
  std::string token;
};

/** Compression applied to a packet body, taken from the header's proto field. */
enum BiliProto : uint16_t {
  kProtoCommand = 0,      // body is plain JSON
  kProtoSpecial = 1,      // control packet; the body convention is not uniform
  kProtoCommandZlib = 2,  // body is a zlib stream of nested packets
  kProtoCommandBrotli = 3,
};

/** Packet kind, taken from the header's type field. */
enum BiliType : uint32_t {
  kTypeHeartbeat = 2,
  kTypeHeartbeatResp = 3,
  kTypeCommand = 5,
  kTypeAuth = 7,
  kTypeAuthResp = 8,
};

/** 16-byte header every bilibili packet starts with. All fields are big-endian, and headerSize is
 *  16 in practice but is read rather than assumed so a future variant cannot desynchronise the
 *  parser silently. */
struct PacketHeader {
  uint32_t totalSize = 0;
  uint16_t headerSize = 0;
  uint16_t proto = 0;
  uint32_t type = 0;
  uint32_t sequence = 0;
};

class Bili {
 public:
  explicit Bili(BiliConfig cfg);

  /** Resolves whatever the user typed -- a room id, or a live.bilibili.com URL containing one --
   *  into a real room id, which is what the API requires. A URL with no room number in the path
   *  (a b23.tv short link) is fetched and scanned for one. Throws std::runtime_error. */
  int64_t resolveRoom(const std::string& input) const;

  /** WBI-signed getDanmuInfo. Throws std::runtime_error on transport failure or a non-zero code. */
  DanmuEndpoint danmuEndpoint(int64_t realRoomId);

  /** The User-Agent used for both the HTTP calls and the websocket handshake. The two must agree:
   *  the endpoint checks one and the server checks the other. */
  const std::string& userAgent() const { return cfg_.userAgent; }

  /** buvid3 + the user's cookie, as a Cookie header value. Public because the websocket
   *  handshake needs the same cookies the HTTP bootstrap used. Never logged. The own buvid3 is
   *  only added when the user's cookie does not already carry one, so the header never ends up
   *  with two of them. */
  const std::string& cookieHeader() const;

  /** The account mid taken from DedeUserID in the user's cookie, or 0 when absent. Sent as the
   *  authentication packet's uid so the connection is authenticated rather than a guest's, which
   *  is what decides whether nicknames are masked. Parsed on first call, not on first
   *  cookieHeader(), so it does not depend on those happening in a particular order. */
  int64_t accountMid() const;

  /** The 16-byte-header packet that authenticates the connection.
   *
   *  uid must match the account whose credentials fetched the token, or the server drops the
   *  connection; 0 means "guest", which still works but is why nicknames arrive masked. */
  static std::string authPacket(int64_t realRoomId, const std::string& token, int64_t uid = 0);
  /** Heartbeat packet. The body is a placeholder: the server only checks that one arrived. */
  static std::string heartbeatPacket();

  /** Extracts the next packet from a websocket payload, advancing in. Returns false at the end. */
  static bool nextPacket(std::string_view& in, PacketHeader& header, std::string_view& body);

  /** Invokes fn once per JSON document inside one websocket message, and returns whether the
   *  message parsed at all.
   *
   *  This exists because the proto field is a hint the server does not honour consistently: the
   *  authentication reply arrives with proto=3 but carries plain JSON (measured, see
   *  docs/internals.md), while the danmaku batches at that same proto really are brotli. An
   *  uncompressed command is a single JSON document; a compressed one decompresses to a further
   *  sequence of framed packets. Sniffing the body keeps both shapes behind one call, so the
   *  caller never branches on a field that would silently drop traffic when it lies.
   *
   *  fn returns false to stop early (used for --count). */
  using JsonVisitor = std::function<bool(std::string_view)>;
  static bool forEachJson(std::string_view message, const JsonVisitor& fn);

  /** Decompresses a command body per header.proto, then returns the JSON bodies inside it. For
   *  proto 0 the result is the body itself; for zlib/brotli it is the nested packet sequence, so
   *  the caller loops with nextPacket() again. Returns false on a malformed or oversized payload. */
  static bool decompress(uint16_t proto, std::string_view body, std::string& out,
                         size_t maxOut = 64u * 1024u * 1024u);

  /** Turns one command document into a Message. Returns false for anything that is not a message
   *  meant for the chat panel, which is most of the stream: entry notices, rank changes, gift and
   *  membership events this milestone does not render yet. That is not an error.
   *
   *  DANMU_MSG is measured and verified; see docs/internals.md. The card kinds are read from the
   *  documented field tables but have not yet been observed on this machine, so every field there
   *  is optional and a document that does not match simply produces a message with less in it
   *  rather than being dropped. */
  static bool parseMessage(const Json& json, Message& out);

  /** Splits a body into text and emote fragments using the platform's advertised token map. */
  static void splitFragments(const std::string& text,
                             const std::vector<std::pair<std::string, Fragment>>& emotes,
                             std::vector<Fragment>& out);

  // Exposed for the tests and for docs/internals.md; not used by the render path.
  static std::string mixinKey(const std::string& imgKey, const std::string& subKey);
  static std::string md5Hex(std::string_view data);
  static std::string percentEncode(std::string_view s);

 private:
  /** GET with the browser-like headers and the assembled cookie. */
  std::string get(const std::string& url, size_t maxBytes = 4u * 1024u * 1024u) const;

  /** buvid3 for an anonymous session: without it getDanmuInfo answers -352. Cached for the
   *  process; it is stable for the lifetime of the run. Const with a mutable member because it is
   *  a lazily filled cache behind a const interface: get() is const and needs the cookie header. */
  const std::string& buvid() const;

  /** mixin_key derived from the site-wide wbi_img keys. Cached and refreshed once per run, since
   *  the keys rotate daily. Same lazy-cache shape as buvid(). */
  const std::string& wbiMixinKey() const;

  BiliConfig cfg_;
  mutable int64_t mid_ = 0;
  mutable bool midParsed_ = false;
  mutable std::string buvid_;
  mutable std::string mixinKey_;
  mutable std::string cookie_;
  mutable bool cookieBuilt_ = false;
};

}  // namespace dwm
// Unit tests for the outbound feed: the typed tab-separated line format with and without Resonite
// tags, and the loopback server that carries it -- handshake, frame encoding, ping/pong, and the
// two refusals a client can earn.
//
// The line format is the contract, so those tests split each record the way a client does -- on tabs
// -- and assert the whole field vector. The server side is tested over a real socket rather than
// against its own helpers. Frame encoding is exactly the kind of code where a test that reuses the
// encoder's idea of a frame agrees with the encoder and with nothing else; this the client side
// below re-derives from RFC 6455 instead. The handshake is checked against the example nonce and
// accept value in section 1.3 of the RFC, which is a fact about the RFC and not about this program.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "broadcast.hpp"

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

using dwm::Fragment;
using dwm::Message;
using dwm::MsgKind;
using dwm::UserType;

/** Splits a record into its fields. The format is the whole contract, so the tests read it the way a
 *  client does -- by the tabs -- rather than rebuilding it from the same pieces. */
std::vector<std::string> splitFields(const std::string& line) {
  std::vector<std::string> fields;
  size_t start = 0;
  for (;;) {
    const size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

std::string describe(const std::vector<std::string>& fields) {
  std::string out;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) out += " | ";
    out += fields[i];
  }
  return out;
}

/** Asserts the whole field layout: count, order, and every value. A frame that gains a field, loses a
 *  trailing empty one, or puts two of them in the other order is a different wire format. */
void checkFields(const std::string& line, const std::vector<std::string>& want, const char* what) {
  const std::vector<std::string> got = splitFields(line);
  bool ok = got.size() == want.size();
  if (ok) {
    for (size_t i = 0; i < want.size(); ++i) {
      if (got[i] != want[i]) {
        ok = false;
        break;
      }
    }
  }
  if (ok) return;
  std::fprintf(stderr, "FAIL: %s\n  got:  %s\n  want: %s\n", what, describe(got).c_str(),
               describe(want).c_str());
  ++failures;
}

Message chat() {
  Message m;
  m.user = "哈基米";
  m.type = UserType::Member;
  m.avatarUrl = "https://example.invalid/face.png";
  m.parts.push_back(Fragment{Fragment::Kind::Text, "你", "", 0});
  m.parts.push_back(Fragment{Fragment::Kind::Emote, "[热]", "https://example.invalid/e.png", 28});
  m.parts.push_back(Fragment{Fragment::Kind::Text, "好", "", 0});
  return m;
}

void testChatFrame() {
  const std::string avatar = "https://example.invalid/face.png";
  const Message m = chat();
  checkFields(dwm::broadcastLine(m, false), {"1", "哈基米", "你[热]好", avatar},
              "plain: chat fields");

  // A tab or a newline in any text field must not survive into a field, or a client that splits on
  // tabs reads the rest of the message as another field. An emote travels as the token the sender
  // typed, the same text the panel falls back to when the picture has not arrived.
  Message t = chat();
  t.parts.clear();
  t.parts.push_back(Fragment{Fragment::Kind::Text, "a\tb\nc\x7F", "", 0});
  checkFields(dwm::broadcastLine(t, false), {"1", "哈基米", "a b c ", avatar},
              "plain: control characters become spaces");
}

void testChatRtf() {
  const std::string avatar = "https://example.invalid/face.png";

  const Message m = chat();
  checkFields(dwm::broadcastLine(m, true),
              {"1", "<color=#0F9D58>哈基米</color>", "你[热]好", avatar},
              "rtf: member name in role colour, body untouched");

  // The colour the sender chose wins over the role colour, the same order the panel uses.
  Message coloured = chat();
  coloured.userColor = 0x123456;
  checkFields(dwm::broadcastLine(coloured, true),
              {"1", "<color=#123456>哈基米</color>", "你[热]好", avatar},
              "rtf: the sender's colour wins");

  // Chat text is data. A tag the viewer typed is shown as glyphs on the panel and must be shown as
  // glyphs here too -- which takes the wiki's counted noparse, because the closed form cannot wrap
  // text that might contain its own closing tag.
  Message evil;
  evil.user = "x";
  evil.parts.push_back(Fragment{Fragment::Kind::Text, "<b>bold</b>", "", 0});
  checkFields(dwm::broadcastLine(evil, true),
              {"1", "<color=#9AA0A6>x</color>", "<noparse=1><b>bold<noparse=1></b>", ""},
              "rtf: a typed tag is disarmed");
}

void testGiftFrame() {
  const std::string avatar = "https://example.invalid/gift.png";
  Message gift;
  gift.kind = MsgKind::Gift;
  gift.user = "氧***";
  gift.avatarUrl = avatar;
  gift.parts.push_back(Fragment{Fragment::Kind::Text, "投喂 ", "", 0, true});
  gift.parts.push_back(Fragment{Fragment::Kind::Emote, "人气票", "", 0});
  gift = gift.withCount(10);

  checkFields(dwm::broadcastLine(gift, false),
              {"2", "氧***", "投喂", "人气票", "10", avatar}, "plain: gift fields");
  checkFields(dwm::broadcastLine(gift, true),
              {"2", "<color=#9AA0A6>氧***</color>", "<color=#F5C542>投喂</color>",
               "<color=#FB7299>人气票</color>", "10", avatar},
              "rtf: gift verb and gift name keep the panel's accents");

  // A lone gift is still one; zero is the model's "not counted" sentinel. The rendered " ×10" is
  // not part of the gift's name: count is its own field.
  Message one = gift;
  one.count = 0;
  checkFields(dwm::broadcastLine(one, false),
              {"2", "氧***", "投喂", "人气票", "1", avatar},
              "plain: a gift with no count counts as one");

  // Every gift text field is sanitised, and the verb's layout space is trimmed after it becomes a
  // field of its own.
  Message dirty = gift;
  dirty.user = "a\tb";
  dirty.parts[0].text = "投\t喂 ";
  dirty.parts[1].text = "人\n气票";
  checkFields(dwm::broadcastLine(dirty, false),
              {"2", "a b", "投 喂", "人 气票", "10", avatar},
              "plain: tabs in a gift cannot add fields");
}

void testSuperChatFrame() {
  const std::string avatar = "https://example.invalid/face.png";
  Message sc;
  sc.kind = MsgKind::Paid;
  sc.type = UserType::Member;
  sc.user = "五条悟";
  sc.avatarUrl = avatar;
  sc.amountValue = 30;
  sc.durationMs = 60000;
  sc.parts.push_back(Fragment{Fragment::Kind::Text, "已经没有什么可怕的了", "", 0});

  checkFields(dwm::broadcastLine(sc, false),
              {"3", "五条悟", "60000", "3000", "已经没有什么可怕的了", avatar},
              "plain: super chat fields");
  checkFields(dwm::broadcastLine(sc, true),
              {"3", "<color=#0F9D58>五条悟</color>", "60000", "3000",
               "已经没有什么可怕的了", avatar},
              "rtf: super chat name is styled, its numbers stay raw");

  // A payload without user_info, price or time still yields a complete frame: the fields are zero
  // or empty, never absent, and an empty name does not become an empty colour tag.
  Message bare;
  bare.kind = MsgKind::Paid;
  checkFields(dwm::broadcastLine(bare, false), {"3", "", "0", "0", "", ""},
              "plain: an unknown super chat field is zero, not missing");
  checkFields(dwm::broadcastLine(bare, true), {"3", "", "0", "0", "", ""},
              "rtf: an empty name stays empty");
}

void testMembershipFrame() {
  Message m;
  m.kind = MsgKind::Membership;
  m.type = UserType::Member;
  m.user = "xfgryujk";
  m.guardLevel = 3;
  m.amountValue = 198;

  checkFields(dwm::broadcastLine(m, false), {"4", "xfgryujk", "3", "19800", ""},
              "plain: membership fields");
  checkFields(dwm::broadcastLine(m, true),
              {"4", "<color=#0F9D58>xfgryujk</color>", "3", "19800", ""},
              "rtf: membership name is styled, tier and price stay raw");
}

void testEntryFrame() {
  Message entry;
  entry.kind = MsgKind::Entry;
  entry.user = "bili_30704624353";
  entry.count = 9;
  entry.parts.push_back(Fragment{Fragment::Kind::Text, "等 9 人进入了直播间", "", 0});

  checkFields(dwm::broadcastLine(entry, false),
              {"5", "bili_30704624353", "9", "等 9 人进入了直播间", ""},
              "plain: entry fields");
  // The ambient kinds arrive an order of magnitude faster than chat; the panel dims them, and so
  // does the feed, on both text fields so neither is left at full strength.
  checkFields(dwm::broadcastLine(entry, true),
              {"5", "<alpha=#80><color=#9AA0A6>bili_30704624353</color></alpha>", "9",
               "<alpha=#80>等 9 人进入了直播间</alpha>", ""},
              "rtf: entry name and content dimmed");
}

void testLikeFrame() {
  const std::string avatar = "https://example.invalid/like.png";
  Message like;
  like.kind = MsgKind::Like;
  like.user = "strangeLex";
  like.avatarUrl = avatar;
  like.parts.push_back(Fragment{Fragment::Kind::Text, "为主播点赞了", "", 0});

  checkFields(dwm::broadcastLine(like, false),
              {"6", "strangeLex", "为主播点赞了", avatar}, "plain: like fields");
  checkFields(dwm::broadcastLine(like, true),
              {"6", "<alpha=#80><color=#9AA0A6>strangeLex</color></alpha>",
               "<alpha=#80>为主播点赞了</alpha>", avatar},
              "rtf: like name and content dimmed");
}

void testFollowAndShareFrames() {
  // Follow and share are the two interactions that used to be indistinguishable from an entry.
  // They share the entry/like shape: type, user, content, avatar, with both text fields dimmed.
  Message follow;
  follow.kind = MsgKind::Follow;
  follow.user = "l***";
  follow.parts.push_back(Fragment{Fragment::Kind::Text, "关注了主播", "", 0});
  checkFields(dwm::broadcastLine(follow, false), {"7", "l***", "关注了主播", ""},
              "plain: follow fields");
  checkFields(dwm::broadcastLine(follow, true),
              {"7", "<alpha=#80><color=#9AA0A6>l***</color></alpha>",
               "<alpha=#80>关注了主播</alpha>", ""},
              "rtf: follow name and content dimmed");

  Message share;
  share.kind = MsgKind::Share;
  share.user = "l***";
  share.parts.push_back(Fragment{Fragment::Kind::Text, "分享了直播间", "", 0});
  checkFields(dwm::broadcastLine(share, false), {"8", "l***", "分享了直播间", ""},
              "plain: share fields");
  checkFields(dwm::broadcastLine(share, true),
              {"8", "<alpha=#80><color=#9AA0A6>l***</color></alpha>",
               "<alpha=#80>分享了直播间</alpha>", ""},
              "rtf: share name and content dimmed");
}

// --- the server ---------------------------------------------------------------------------------

int connectTo(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool sendAll(int fd, const std::string& s) {
  size_t sent = 0;
  while (sent < s.size()) {
    const ssize_t n = ::send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

enum class Read { Data, Timeout, Eof };

/** Appends whatever is readable within the timeout. Timeout and EOF are told apart: a poll that
 *  times out is a reason to keep waiting, while EOF is a reason to stop, and folding them together
 *  makes a read with a long budget give up after the first short slice. */
Read readSome(int fd, std::string& buf, int timeoutMs) {
  struct pollfd pfd {};
  pfd.fd = fd;
  pfd.events = POLLIN;
  if (::poll(&pfd, 1, timeoutMs) <= 0) return Read::Timeout;
  char tmp[4096];
  const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
  if (n <= 0) return Read::Eof;
  buf.append(tmp, static_cast<size_t>(n));
  return Read::Data;
}

/** Pops one complete server-to-client frame from buf. A server frame is never masked, so the
 *  layout is header, extended length, payload. False means "not enough bytes yet". */
bool takeFrame(std::string& buf, uint8_t& opcode, std::string& payload) {
  if (buf.size() < 2) return false;
  opcode = static_cast<uint8_t>(buf[0]) & 0x0F;
  if (static_cast<uint8_t>(buf[1]) & 0x80) {
    check(false, "a server frame must not be masked");
    return false;
  }
  uint64_t len = static_cast<uint8_t>(buf[1]) & 0x7F;
  size_t pos = 2;
  if (len == 126) {
    if (buf.size() < pos + 2) return false;
    len = (static_cast<uint64_t>(static_cast<uint8_t>(buf[pos])) << 8) |
          static_cast<uint8_t>(buf[pos + 1]);
    pos += 2;
  } else if (len == 127) {
    if (buf.size() < pos + 8) return false;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(buf[pos + i]);
    pos += 8;
  }
  if (buf.size() < pos + len) return false;
  payload = buf.substr(pos, static_cast<size_t>(len));
  buf.erase(0, pos + static_cast<size_t>(len));
  return true;
}

bool waitForFrame(int fd, std::string& buf, uint8_t want, std::string& payload, int timeoutMs) {
  for (;;) {
    uint8_t opcode = 0;
    if (takeFrame(buf, opcode, payload)) return opcode == want;
    if (timeoutMs <= 0) return false;
    const int slice = timeoutMs < 100 ? timeoutMs : 100;
    if (readSome(fd, buf, slice) == Read::Eof) return false;
    timeoutMs -= slice;
  }
}

bool waitForStatus(int fd, std::string& buf, const char* status, int timeoutMs) {
  for (;;) {
    if (buf.find(status) != std::string::npos) return true;
    if (timeoutMs <= 0) return false;
    const int slice = timeoutMs < 100 ? timeoutMs : 100;
    if (readSome(fd, buf, slice) == Read::Eof) return false;
    timeoutMs -= slice;
  }
}

/** Reads the handshake response and consumes it whole, so what is left in buf starts at a frame
 *  boundary. Stopping at the status line would leave the tail of the headers there, and its ASCII
 *  reads as a frame header -- which desynchronises every read after it. The response and the first
 *  broadcast frame can arrive in one segment, so this must not over-read either: it stops at the
 *  first \r\n\r\n and leaves anything after it alone. */
bool readHandshake(int fd, std::string& buf, int timeoutMs, std::string* headOut = nullptr) {
  for (;;) {
    const size_t end = buf.find("\r\n\r\n");
    if (end != std::string::npos) {
      const std::string head = buf.substr(0, end);
      if (headOut) *headOut = head;
      buf.erase(0, end + 4);
      return head.find("101 Switching Protocols") != std::string::npos;
    }
    if (timeoutMs <= 0) return false;
    const int slice = timeoutMs < 100 ? timeoutMs : 100;
    if (readSome(fd, buf, slice) == Read::Eof) return false;
    timeoutMs -= slice;
  }
}

/** A masked client frame with a fixed key: short payloads only, which is all a client of this feed
 *  has any reason to send. */
std::string clientFrame(uint8_t opcode, const std::string& payload) {
  std::string f;
  f.push_back(static_cast<char>(0x80 | opcode));
  f.push_back(static_cast<char>(0x80 | payload.size()));
  const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
  f.append(reinterpret_cast<const char*>(mask), sizeof(mask));
  for (size_t i = 0; i < payload.size(); ++i)
    f.push_back(static_cast<char>(payload[i] ^ mask[i & 3]));
  return f;
}

constexpr const char* kUpgrade =
    "GET /sub HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"  // RFC 6455 section 1.3
    "Sec-WebSocket-Version: 13\r\n\r\n";

bool waitForClients(const dwm::WsServer& server, size_t want, int timeoutMs) {
  for (int waited = 0; waited < timeoutMs; waited += 20) {
    if (server.clients() == want) return true;
    struct timespec ts {0, 20 * 1000 * 1000};
    ::nanosleep(&ts, nullptr);
  }
  return false;
}

void testServer() {
  dwm::WsServer server;
  server.listen("127.0.0.1", 0);  // an ephemeral port, so the test cannot collide with a real run
  check(server.port() > 0, "server: an ephemeral port was reported");
  std::thread thread([&server] { server.run(); });

  // A normal client: upgrade, receive, ping, close.
  {
    const int fd = connectTo(server.port());
    check(fd >= 0, "server: connected");
    check(sendAll(fd, kUpgrade), "server: upgrade request sent");
    std::string buf, head;
    check(readHandshake(fd, buf, 2000, &head), "server: answered 101");
    // The RFC's own nonce and the accept value it must produce.
    check(head.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos,
          "server: RFC 6455 example accept value");
    check(buf.empty(), "server: the handshake left no bytes in the frame buffer");
    check(waitForClients(server, 1, 2000), "server: client counted after the handshake");

    server.broadcast("1\tname\tbody\tface.png");
    std::string payload;
    check(waitForFrame(fd, buf, 0x1, payload, 2000), "server: text frame arrived");
    checkEq(payload, std::string("1\tname\tbody\tface.png"),
            "server: payload survived the framing");

    // Several messages in a row, one of them past the 125-byte boundary where the length moves
    // into the extended field: each must arrive exactly once, in order. A queue that is re-flushed
    // from the front instead of advanced would repeat the earlier frames here, and a duplicated
    // danmaku is the kind of thing a screenshot never shows.
    const std::string longBody(300, 'x');
    for (int i = 0; i < 4; ++i) server.broadcast("message " + std::to_string(i));
    server.broadcast(longBody);
    for (int i = 0; i < 4; ++i) {
      check(waitForFrame(fd, buf, 0x1, payload, 2000), "server: repeated frame arrived");
      checkEq(payload, "message " + std::to_string(i), "server: repeated frame in order");
    }
    check(waitForFrame(fd, buf, 0x1, payload, 2000), "server: extended-length frame arrived");
    checkEq(payload, longBody, "server: extended-length payload intact");
    check(!waitForFrame(fd, buf, 0x1, payload, 300), "server: nothing was sent twice");

    // A second client gets its own copy, and neither gets a second copy of it. The broadcast loop
    // is per client, so this is where a copy-into-the-wrong-queue would show up.
    const int fd2 = connectTo(server.port());
    check(fd2 >= 0, "server: second client connected");
    check(sendAll(fd2, kUpgrade), "server: second upgrade sent");
    std::string buf2, head2;
    check(readHandshake(fd2, buf2, 2000, &head2), "server: second client answered 101");
    check(waitForClients(server, 2, 2000), "server: two clients counted");
    server.broadcast("to both");
    check(waitForFrame(fd, buf, 0x1, payload, 2000), "server: first client got it");
    checkEq(payload, std::string("to both"), "server: first client's copy");
    check(waitForFrame(fd2, buf2, 0x1, payload, 2000), "server: second client got it");
    checkEq(payload, std::string("to both"), "server: second client's copy");
    check(!waitForFrame(fd, buf, 0x1, payload, 300), "server: first client got no extra copy");
    check(!waitForFrame(fd2, buf2, 0x1, payload, 300), "server: second client got no extra copy");
    ::close(fd2);
    check(waitForClients(server, 1, 2000), "server: back to one client");

    check(sendAll(fd, clientFrame(0x9, "keepalive")), "server: ping sent");
    check(waitForFrame(fd, buf, 0xA, payload, 2000), "server: ping answered with a pong");
    checkEq(payload, std::string("keepalive"), "server: pong carries the ping's payload");

    check(sendAll(fd, clientFrame(0x8, std::string("\x03\xe8", 2))), "server: close sent");
    check(waitForFrame(fd, buf, 0x8, payload, 2000), "server: close echoed");
    ::close(fd);
    check(waitForClients(server, 0, 2000), "server: client forgotten after the close");
  }

  // A client that violates the framing: an unmasked frame is a protocol error, and the answer is
  // 1002 rather than silence, so the client can tell which side broke the connection.
  {
    const int fd = connectTo(server.port());
    check(fd >= 0, "refusal: connected");
    check(sendAll(fd, kUpgrade), "refusal: upgrade request sent");
    std::string buf;
    check(readHandshake(fd, buf, 2000), "refusal: answered 101");
    check(waitForClients(server, 1, 2000), "refusal: client counted");

    std::string unmasked;
    unmasked.push_back(static_cast<char>(0x81));  // FIN + text
    unmasked.push_back(static_cast<char>(0x01));  // unmasked, one byte
    unmasked.push_back('x');
    check(sendAll(fd, unmasked), "refusal: unmasked frame sent");
    std::string payload;
    check(waitForFrame(fd, buf, 0x8, payload, 2000), "refusal: closed with a close frame");
    checkEq(payload, std::string("\x03\xea", 2), "refusal: 1002 protocol error");
    ::close(fd);
    check(waitForClients(server, 0, 2000), "refusal: client forgotten");
  }

  // Something that is not a WebSocket at all: the answer is an HTTP error before the close, not a
  // socket that vanishes.
  {
    const int fd = connectTo(server.port());
    check(fd >= 0, "non-websocket: connected");
    check(sendAll(fd, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), "non-websocket: request sent");
    std::string buf;
    check(waitForStatus(fd, buf, "400 Bad Request", 2000), "non-websocket: answered 400");
    ::close(fd);
  }

  server.stop();
  thread.join();
}

/** A client that stops reading costs the feed a bounded amount of memory, and what it does get
 *  has to stay a stream of whole frames.
 *
 *  The backlog cap is the only place the server drops anything, and the natural move -- throw away
 *  the oldest queued frame, ring-buffer style -- is wrong: the oldest may already be half on the
 *  socket, and no later bytes can repair a half frame. This test stalls a real connection until the
 *  cap engages, then reads what was written and requires every frame to parse and every payload to
 *  be a message that was actually sent, in order. Dropping the front instead fails here, either as
 *  a payload nobody sent or as trailing bytes that are not a frame. */
void testStalledClient() {
  dwm::WsServer server;
  server.listen("127.0.0.1", 0);
  std::thread thread([&server] { server.run(); });

  const int fd = connectTo(server.port());
  check(fd >= 0, "stall: connected");
  const int small = 4096;  // a tiny window, so backpressure arrives quickly
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  check(sendAll(fd, kUpgrade), "stall: upgrade request sent");
  std::string buf, head;
  check(readHandshake(fd, buf, 2000, &head), "stall: answered 101");
  check(waitForClients(server, 1, 2000), "stall: client counted");

  // Big messages, more of them than any socket buffer will hold: tens of megabytes if they were
  // all queued for one stalled reader.
  const size_t kMessages = 2000;
  const std::string tail(8000, 'y');
  std::vector<std::string> sent;
  sent.reserve(kMessages);
  for (size_t i = 0; i < kMessages; ++i) {
    sent.push_back("msg-" + std::to_string(i) + "-" + tail);
    server.broadcast(sent.back());
  }
  check(server.dropped() > 0, "stall: the backlog cap engaged");

  // Read to the end of what the server wrote. Silence is the end: it has nothing left queued.
  std::string all = buf;
  while (readSome(fd, all, 300) == Read::Data) {
  }

  size_t frames = 0;
  bool ordered = true;
  for (;;) {
    uint8_t opcode = 0;
    std::string payload;
    if (!takeFrame(all, opcode, payload)) break;
    if (opcode != 0x1) continue;
    if (frames >= sent.size() || payload != sent[frames]) ordered = false;
    ++frames;
  }
  check(frames > 0, "stall: something was delivered");
  check(ordered, "stall: every frame is a message that was sent, in order");
  check(all.empty(), "stall: the stream ends on a frame boundary");
  ::close(fd);
  server.stop();
  thread.join();
}

}  // namespace

int main() {
  testChatFrame();
  testChatRtf();
  testGiftFrame();
  testSuperChatFrame();
  testMembershipFrame();
  testEntryFrame();
  testLikeFrame();
  testFollowAndShareFrames();
  testServer();
  testStalledClient();
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("broadcast: ok\n");
  return 0;
}

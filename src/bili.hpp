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
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

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
   *  meant for the chat panel, which is still most of the stream: rank changes, watched counters,
   *  the aggregate "N people are liking" toasts, the login notice. That is not an error.
   *
   *  Six commands are read. Three of them are measured on this machine and verified:
   *
   *    DANMU_MSG           a typed chat line
   *    INTERACT_WORD_V2    somebody entered the room   (protobuf, see below)
   *    SEND_GIFT_V2        somebody sent a gift         (protobuf, see below)
   *    LIKE_INFO_V3_CLICK  somebody liked the stream
   *
   *  and the two card kinds follow the documented field tables but have not yet been observed here:
   *    SUPER_CHAT_MESSAGE, GUARD_BUY
   *
   *  Every field is read optionally throughout, so a document that does not match yields a message
   *  with less in it rather than being dropped -- except where a row without it would have nothing
   *  on it to draw, which is noted at each site.
   *
   *  INTERACT_WORD_V2 and SEND_GIFT_V2 no longer carry JSON. Both now send a single base64 field,
   *  `data.pb`, that decodes to a protobuf message with no published schema; the field numbers read
   *  here were measured off the wire and are pinned by tests/bili_test.cpp. This is the one place in
   *  the project that decodes protobuf, and it does so through src/pb.hpp rather than a generated
   *  class, because there is no schema to generate one from. */
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

/** Folds the entry notices into one row per window.
 *
 *  Measured in room 21852, about a million watchers: 110 INTERACT_WORD_V2 in 70 s against 25
 *  DANMU_MSG. Entries outnumber chat better than four to one, so a row each does not add
 *  information to the panel -- it pushes four fifths of what was actually typed off the top. The
 *  notices received since the last row are therefore shown together, named for the first of them
 *  and counted, which is what the site's own entry panel does with the same stream.
 *
 *  A window of 0 turns this off, and then every notice gets its own row exactly as it arrives. That
 *  is kept as a real option rather than treated as the degenerate case, because it is the honest
 *  way to see the raw event rate, which is what the decision above was based on.
 *
 *  Clock-driven rather than count-driven on purpose: a count threshold would still let a busy
 *  stretch put a burst on screen, and the crowding is a matter of how fast rows arrive, not of how
 *  many there are in total. */
class EntryMerger {
 public:
  /** The window used when --entry-merge is not given. The single source of that number: the
   *  command line's own default reads it from here, so the two cannot drift apart the first time
   *  one of them is edited. */
  static constexpr int64_t kDefaultWindowMs = 5000;

  explicit EntryMerger(int64_t windowMs = kDefaultWindowMs) : windowMs_(windowMs) {}

  int64_t windowMs() const { return windowMs_; }

  /** Feeds one entry notice and asks whether a row is due. Fills out and returns true when it is.
   *
   *  Note that a true result does not mean *this* notice is what gets drawn: when a batch is
   *  pending, the row drawn is the summary of the batch, and this notice is counted into it. */
  bool push(const Message& entry, int64_t nowMs, Message& out) {
    if (pending_ == 0) head_ = entry;
    ++pending_;
    return flush(nowMs, out);
  }

  /** Publishes a batch whose window has elapsed, without waiting for another notice to arrive.
   *
   *  The read loop calls this on every wake-up, so a batch is never more than one read deadline
   *  late -- without it, the last few entries of a burst would stay uncounted until the next viewer
   *  walked in, which in a quiet room could be never. */
  bool flush(int64_t nowMs, Message& out);

  /** Publishes whatever is pending right now, window or no window.
   *
   *  For the end of the stream, where waiting is no longer an option: once the socket is gone no
   *  further notice will ever arrive to close the batch, and the people in it did walk in. Folding
   *  them away at that point would make the row count disagree with the number of viewers the
   *  panel actually saw, which is the one thing the row exists to be accurate about. */
  bool flushAll(Message& out);

  /** Notices received since the last row was drawn. Diagnostics and tests only. */
  int pending() const { return pending_; }

 private:
  int64_t windowMs_;
  /** Earliest time the next row may be drawn. Zero before the first one, which is in the past. */
  int64_t nextRowAtMs_ = 0;
  int pending_ = 0;
  /** The notice the next row is named after: the first of the batch. */
  Message head_;
};

/** Folds repeated gifts from the same viewer into one row per window.
 *
 *  The same problem EntryMerger solves, one level down. Entry notices are frequent and every one is
 *  a different viewer, so they are folded by the room. A gift is something *one* viewer may repeat:
 *  bilibili's gift buttons are taps, and a viewer who likes a gift and taps it ten times produces
 *  ten rows saying the same thing, each pushing a line of actual chat off the top. The count is
 *  already on screen in that case -- a single gift of six reads " ×6" -- so a merged row is the
 *  same row with the sum and the picture of the first of them.
 *
 *  Keyed by viewer *and* gift, not by viewer alone: two different gifts are two different things and
 *  each earns its own row. The viewer's face is part of the key because an anonymous connection
 *  masks every nickname into the same few shapes (`氧***`), and merging two people who happen to
 *  share a masked name would show one of them having sent a gift the other one sent.
 *
 *  The row is held for the window and not drawn-then-amended, because the panel draws a row once
 *  and afterwards only moves its pixels: there is nothing to amend. The price is that a lone gift
 *  appears up to windowMs late, which is the same trade EntryMerger makes and the reason a window
 *  of 0 is a real option rather than a degenerate one.
 *
 *  Several runs are held at once, so a flush can publish more than one row: two viewers sending
 *  different gifts in the same window are not each other's duplicate. flush() therefore appends to a
 *  vector, and the caller publishes whatever came out of it in order. */
class GiftMerger {
 public:
  /** The window used when --gift-merge is not given. Shorter than EntryMerger's on purpose: a
   *  folded entry notice costs nothing but a dimmed row, whereas this one delays a thank-you to the
   *  person who paid for it. It is still long enough to hold a burst of taps. The single source of
   *  that number -- the command line reads its own default from here. */
  static constexpr int64_t kDefaultWindowMs = 3000;

  explicit GiftMerger(int64_t windowMs = kDefaultWindowMs) : windowMs_(windowMs) {}

  int64_t windowMs() const { return windowMs_; }

  /** Feeds one gift in.
   *
   *  With a window the row it belongs to is held until the window ends, and nothing is appended to
   *  out; with no window, or for a row the site gave no merge key to, the message goes straight into
   *  out and is this viewer's row to draw. Appending rather than returning one message is what lets
   *  the unmerged path share this call with the merged one. */
  void push(const Message& gift, int64_t nowMs, std::vector<Message>& out) {
    // Two ways to have nothing to combine with: no window was asked for, or the site gave this row
    // no key. Both mean the row is its own, and it goes straight out rather than being held for a
    // window that could never have matched anything.
    if (windowMs_ <= 0 || gift.mergeKey.empty()) {
      out.push_back(gift);
      return;
    }

    Run& run = runs_[keyOf(gift)];
    const int64_t count = gift.count > 0 ? gift.count : 1;
    // total is the sentinel for "this run is open": a gift is always worth at least one, so zero can
    // only mean nothing has been counted yet. It is a field rather than a test for the key's
    // presence, so a run that is created and immediately closed leaves nothing behind.
    if (run.total == 0) {
      run.row = gift;
      // The window runs from the first gift of the run and is not extended by the ones after it.
      // Extending it would let a viewer who keeps tapping hold one row off the panel forever, which
      // is the flood this replaces -- only slower.
      run.dueAtMs = nowMs + windowMs_;
    }
    run.total += count;
    pending_ += count;
  }

  /** Appends every row whose window has elapsed, oldest run first.
   *
   *  Called on every wake-up of the read loop, so a run is never more than one read deadline late --
   *  the same reason EntryMerger needs its flush(): waiting for the next gift would leave the last
   *  few of a burst uncounted until somebody else sent one, which in a quiet room could be never. */
  bool flush(int64_t nowMs, std::vector<Message>& out, bool ignoreWindow = false) {
    if (runs_.empty()) return false;

    // Oldest run first. The map is keyed by viewer and gift, so it knows nothing about arrival
    // order, and the row that has been waiting longest is the one that should appear first when
    // several runs come due in the same wake-up.
    std::vector<RunIter> due;
    for (RunIter it = runs_.begin(); it != runs_.end(); ++it) {
      if (!ignoreWindow && nowMs < it->second.dueAtMs) continue;
      due.push_back(it);
    }
    std::stable_sort(due.begin(), due.end(), [](const RunIter& a, const RunIter& b) {
      return a->second.dueAtMs < b->second.dueAtMs;
    });

    for (const RunIter& it : due) {
      // The row carries the first gift's verb and picture; only the count is rewritten, which is
      // what keeps a merged row looking like one gift rather than a summary sentence.
      out.push_back(it->second.row.withCount(it->second.total));
      pending_ -= it->second.total;
      // Erased by iterator, not by keyOf(run->row). Erasing by key would be a second, separate
      // derivation of the same identity, and it would only hold as long as withCount() happened to
      // leave user, avatarUrl and mergeKey alone -- a silent coupling, because a key that stopped
      // matching would neither fail to compile nor throw: the run would just stay in the map,
      // pending_ would already have been credited back, and the same row would be published again
      // on the next flush. map::erase invalidates only the iterator it is given, so the runs still
      // to be visited stay valid while this loop empties the map underneath them.
      runs_.erase(it);
    }
    return !due.empty();
  }

  /** Appends everything held right now, window or no window.
   *
   *  For the end of the stream, where waiting is no longer an option: once the socket is gone no
   *  further gift will arrive to close the run, and these were really sent -- folding them away at
   *  that point would make the panel's account of who gave what disagree with what the stream
   *  carried.
   *
   *  A wrapper rather than a second walk: one pass over the runs, told to ignore their windows. */
  bool flushAll(std::vector<Message>& out) { return flush(0, out, true); }

  /** Gifts held back right now, over every run. Diagnostics and tests only. */
  int64_t pending() const { return pending_; }
  /** How many runs are open, which is how many rows a flush can produce. Diagnostics and tests. */
  size_t pendingRows() const { return runs_.size(); }

 private:
  /** One viewer sending one gift, inside one window. */
  struct Run {
    /** The first gift of the run: verb, icon, face and name -- everything the row shows that is not
     *  the count. Later gifts of the same run contribute their count and nothing else. */
    Message row;
    /** Gifts in the run, summed from Message::count rather than counted as events: a row for "×6"
     *  plus one more is "×7", not "×2". */
    int64_t total = 0;
    /** When this run's window ends, measured from its first gift so a burst cannot keep pushing the
     *  row out indefinitely. */
    int64_t dueAtMs = 0;
  };

  /** The open runs. A map rather than a list because a room with several people sending gifts at
   *  once opens several runs, and each has to be found by key on arrival. */
  using RunMap = std::map<std::string, Run>;
  /** One open run where it sits in that map: how a run is handed out and, in flush(), closed. */
  using RunIter = RunMap::iterator;

  /** What makes two gifts the same row: the viewer's name and face, and the gift's own key.
   *
   *  A NUL between the parts, because none of them can contain one and a separator that can appear
   *  inside its own fields is a key that can collide. */
  static std::string keyOf(const Message& gift) {
    return gift.user + std::string(1, '\0') + gift.avatarUrl + std::string(1, '\0') + gift.mergeKey;
  }

  int64_t windowMs_;
  /** Open runs, keyed by viewer and gift. */
  RunMap runs_;
  /** Gifts held across every open run, which is what pending() reports. */
  int64_t pending_ = 0;
};

}  // namespace dwm
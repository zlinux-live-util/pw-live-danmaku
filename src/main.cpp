// pw-live-danmaku (native) -- bilibili live chat as a PipeWire video node.
//
// Thread split, which is the part that is hardest to change later:
//   main thread    : pwvideo::VideoNode::run(); the frame callback renders from a snapshot
//   site thread    : HTTP bootstrap, websocket, protocol decode; appends to Shared::pending
//   avatar thread  : fetches and decodes face images, because AssetCache::get blocks
//
// The frame callback never touches the network and never waits on the site thread: it drains the
// pending queue under a short lock, hands the new messages to the panel, and copies the panel's
// static layer into the frame.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cairo/cairo.h>

#include "images.hpp"
#include "bili.hpp"
#include "cairo_util.hpp"
#include "message.hpp"
#include "panel.hpp"
#include "pwvideo.hpp"
#include "ws.hpp"

namespace {

using namespace dwm;

int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/** Font chain. The comma form is a pango fallback chain resolved per character, so latin glyphs
 *  can come from one family and CJK from another. */
constexpr const char* kFontChain = "Noto Sans CJK SC,Sarasa Mono CL,DejaVu Sans,sans-serif";

/** Bounds how much work one frame can be asked to do. A burst of messages arriving while nobody was
 *  consuming frames is drained over several frames rather than all at once, so a flood cannot make
 *  a single frame arbitrarily expensive. */
constexpr size_t kMaxPerFrame = 40;

/** Bounds the queue when no consumer is attached. Without this an overlay left running for hours
 *  would accumulate every message that arrived. */
constexpr size_t kPendingCap = 400;

/** How many messages --dump waits for when --count was not given: enough for the panel to show
 *  something worth looking at, few enough to stay quick. */
constexpr int kDumpMessages = 6;

PanelTokens defaultTokens() {
  PanelTokens t;
  t.font = kFontChain;
  return t;
}

void usage(std::FILE* out) {
  std::fprintf(
      out,
      "pw-live-danmaku (native)\n"
      "\n"
      "  --room ID|URL      Bilibili room, e.g. 545068 or https://live.bilibili.com/545068\n"
      "                     Required unless --demo is given. A b23.tv short link also works.\n"
      "  --cookie STR       Raw cookie header from the browser, e.g.\n"
      "                     \"SESSDATA=...; bili_jct=...; DedeUserID=...\".\n"
      "                     Optional: anonymous works but masks nicknames. Visible in ps(1),\n"
      "                     so prefer --cookie-file.\n"
      "  --cookie-file PATH Read the cookie from a file instead; recommended, since the file can\n"
      "                     be chmod 600 and the value never reaches the process arguments.\n"
      "  --font NAME[,...]  Font family chain for the panel, default a CJK-capable fallback chain\n"
      "  --font-file PATH   Register a font file (or a directory) with fontconfig at startup;\n"
      "                     repeatable. Without --font its own family name is used\n"
      "  --node NAME        PipeWire node name, default pw-live-danmaku\n"
      "  --desc TEXT        Node description (this is what the OBS dropdown shows), default\n"
      "                     \"Live Chat\"\n"
      "  --size WxH         Output size, default 480x1080. The OBS source size must match:\n"
      "                     a smaller negotiated size is clipped, not scaled.\n"
      "  --fps N            Frame-rate ceiling, default 30\n"
      "  --dump FILE        Render one sample frame to PNG and exit\n"
      "  --count N          Exit after receiving N messages (0 = never)\n"
      "  --seconds N        Exit after N seconds (0 = never)\n"
      "  --demo             Draw a fixed set of messages and no network at all, for tuning the\n"
      "                     layout without a live room. Overrides --room.\n"
      "  --verbose, -v      Log the protocol handshake and every parsed message\n"
      "  --help, -h\n");
}

/** Argument parsing has three outcomes, not two: a bad flag is not a request for help. Same
 *  reasoning as the sibling projects -- under Restart=always an unknown flag that exited 0 would
 *  be a silent restart loop reporting SUCCESS. */
enum class Args { Ok, Help, Error };

struct Options {
  std::string room, cookie, cookieFile, node = "pw-live-danmaku", desc = "Live Chat";
  std::string dump;
  std::vector<std::string> fontFiles;
  std::string font;
  int width = 480, height = 1080, fps = 30, count = 0, seconds = 0;
  bool verbose = false, demo = false;
};

Args parseArgs(int argc, char** argv, Options& o) {
  auto next = [&](int& i) -> std::string {
    if (i + 1 >= argc) throw std::runtime_error("missing argument value");
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return Args::Help;
    } else if (a == "--room") {
      o.room = next(i);
    } else if (a == "--cookie") {
      o.cookie = next(i);
    } else if (a == "--cookie-file") {
      o.cookieFile = next(i);
    } else if (a == "--font") {
      o.font = next(i);
    } else if (a == "--font-file") {
      o.fontFiles.push_back(next(i));
    } else if (a == "--node") {
      o.node = next(i);
    } else if (a == "--desc") {
      o.desc = next(i);
    } else if (a == "--size") {
      const std::string v = next(i);
      const size_t x = v.find_first_of("xX*");
      if (x == std::string::npos) {
        o.width = o.height = std::max(64, std::stoi(v));
      } else {
        o.width = std::max(64, std::stoi(v.substr(0, x)));
        o.height = std::max(64, std::stoi(v.substr(x + 1)));
      }
    } else if (a == "--fps") {
      o.fps = std::max(1, std::stoi(next(i)));
    } else if (a == "--dump") {
      o.dump = next(i);
    } else if (a == "--count") {
      o.count = std::max(0, std::stoi(next(i)));
    } else if (a == "--seconds") {
      o.seconds = std::max(0, std::stoi(next(i)));
    } else if (a == "--demo") {
      o.demo = true;
    } else if (a == "--verbose" || a == "-v") {
      o.verbose = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n\n", a.c_str());
      usage(stderr);
      return Args::Error;
    }
  }
  return Args::Ok;
}

/** State shared between the site thread and the frame callback. */
struct Shared {
  mutable std::mutex mu;
  std::string state = "starting";
  std::string roomLabel;
  std::string lastEvent;
  std::deque<Message> pending;
  uint64_t total = 0, dropped = 0;
  // Why a message is showing the placeholder disc. The fetch counters cannot answer this: a user
  // with no face URL is never requested, so it looks identical to a face that has not arrived yet.
  uint64_t avatarEmpty = 0, avatarHttps = 0, avatarHttp = 0, avatarProtoRel = 0, avatarOther = 0;

  /** Classifies an avatar URL by shape. The protocol-relative case matters: a bare "//host/path"
   *  is not a URL the HTTP client will accept, and it would fail in a way that looks like a network
   *  problem rather than a malformed field. */
  void noteAvatar(const std::string& url) {
    std::lock_guard<std::mutex> lk(mu);
    if (url.empty()) ++avatarEmpty;
    else if (url.rfind("https://", 0) == 0) ++avatarHttps;
    else if (url.rfind("http://", 0) == 0) ++avatarHttp;
    else if (url.rfind("//", 0) == 0) ++avatarProtoRel;
    else ++avatarOther;
  }

  void append(Message m) {
    std::lock_guard<std::mutex> lk(mu);
    ++total;
    pending.push_back(std::move(m));
    while (pending.size() > kPendingCap) {
      pending.pop_front();
      ++dropped;
    }
  }
  void note(const std::string& event) {
    std::lock_guard<std::mutex> lk(mu);
    lastEvent = event;
  }
  void setState(std::string s) {
    std::lock_guard<std::mutex> lk(mu);
    state = std::move(s);
  }
};

/** Moves at most kMaxPerFrame messages into out, so one frame's cost stays bounded however large
 *  the backlog is. The lock is held only for the moves. */
void drain(Shared& sh, std::vector<Message>& out) {
  out.clear();
  std::lock_guard<std::mutex> lk(sh.mu);
  while (!sh.pending.empty() && out.size() < kMaxPerFrame) {
    out.push_back(std::move(sh.pending.front()));
    sh.pending.pop_front();
  }
}

void siteLoop(Shared& sh, Bili& bili, ImageStore& avatars, ImageStore& emoteImages,
              const std::string& roomInput,
              bool verbose, std::atomic<bool>& stop, pwvideo::VideoNode* video,
              std::atomic<int>& received, int count) {
  try {
    sh.setState("resolving room");
    const int64_t roomId = bili.resolveRoom(roomInput);
    if (verbose)
      std::fprintf(stderr, "[site] real room id: %lld\n", static_cast<long long>(roomId));

    sh.setState("fetching endpoint");
    const DanmuEndpoint ep = bili.danmuEndpoint(roomId);
    if (verbose)
      std::fprintf(stderr, "[site] endpoint: %s:%d token=%zu bytes\n", ep.host.c_str(), ep.wssPort,
                   ep.token.size());

    WsOptions opt;
    opt.host = ep.host;
    opt.port = ep.wssPort;
    opt.path = "/sub";
    opt.headers.emplace_back("User-Agent", bili.userAgent());
    opt.headers.emplace_back("Origin", "https://live.bilibili.com");
    opt.headers.emplace_back("Cookie", bili.cookieHeader());

    WsClient ws;
    sh.setState("connecting");
    ws.connect(opt);
    sh.setState("authenticating");
    // The account mid comes from the cookie's DedeUserID. Sending 0 authenticates as a guest,
    // which connects fine but is exactly why nicknames arrive masked.
    const int64_t mid = bili.accountMid();
    if (verbose)
      std::fprintf(stderr, "[site] uid=%lld (%s)\n", static_cast<long long>(mid),
                   mid ? "authenticated" : "guest");
    if (!ws.sendBinary(Bili::authPacket(roomId, ep.token, mid))) {
      sh.setState("auth send failed: " + ws.lastError());
      return;
    }

    // Wait for the reply rather than assuming it: an unauthenticated socket still receives
    // danmaku, so "connected" alone would prove nothing.
    std::string msg;
    bool authed = false;
    for (int i = 0; i < 60 && !stop.load(); ++i) {
      const WsClient::Event ev = ws.next(msg, 500);
      if (ev == WsClient::Event::Timeout) continue;
      if (ev == WsClient::Event::Error || ev == WsClient::Event::Close) {
        sh.setState("connection lost during auth: " + ws.lastError());
        return;
      }
      std::string reply;
      Bili::forEachJson(msg, [&](std::string_view doc) {
        reply.assign(doc);
        return true;
      });
      if (reply.empty()) continue;
      std::string err;
      const Json root = Json::parse(reply, &err);
      if (err.empty() && root["code"].num() == 0) {
        authed = true;
        break;
      }
      // The reply is a short JSON blob from the server and carries no user data.
      sh.setState("auth rejected: " + reply);
      return;
    }
    if (!authed) {
      sh.setState("no auth reply within 30 s");
      return;
    }
    if (verbose) std::fprintf(stderr, "[site] authenticated\n");
    sh.setState("streaming");

    // The read deadline is what drives the heartbeat timer: one thread, no second timer.
    int64_t nextHeartbeat = steadyMs() + 30000;
    while (!stop.load()) {
      const WsClient::Event ev = ws.next(msg, 1000);
      const int64_t now = steadyMs();
      if (now >= nextHeartbeat) {
        nextHeartbeat = now + 30000;
        if (!ws.sendBinary(Bili::heartbeatPacket())) {
          sh.setState("heartbeat failed: " + ws.lastError());
          return;
        }
      }
      if (ev == WsClient::Event::Timeout) continue;
      if (ev == WsClient::Event::Error || ev == WsClient::Event::Close) {
        sh.setState(std::string(ev == WsClient::Event::Close ? "closed by peer" : "error") + ": " +
                    ws.lastError());
        return;
      }

      Bili::forEachJson(msg, [&](std::string_view doc) {
        std::string err;
        const Json json = Json::parse(doc, &err);
        if (!err.empty()) return true;
        Message m;
        if (!Bili::parseMessage(json, m)) {
          const std::string cmd = json["cmd"].str();
          if (!cmd.empty()) sh.note(cmd);
          return true;
        }
        // Face images are fetched by the avatar thread; this only records the wish.
        sh.noteAvatar(m.avatarUrl);
        if (!m.avatarUrl.empty()) {
          // A URL we already have means two users share one picture. When that picture is the
          // site's default, several different people appear with the same flat disc, which reads
          // as a rendering fault and is not one.
          if (verbose && avatars.isCached(m.avatarUrl))
            std::fprintf(stderr, "[avatar] shared pic: %s  (user %s)\n", m.avatarUrl.c_str(),
                         m.user.c_str());
          avatars.request(m.avatarUrl);
        // Emotes are fetched from the platform's own CDN URLs in extra.emots. They go in a separate
        // store because they are reused all evening by everyone, unlike faces which are one per
        // viewer, so they deserve their own cache budget and a bigger share of it.
        for (const Fragment& part : m.parts) {
          if (part.kind == Fragment::Kind::Emote && !part.url.empty())
            emoteImages.request(part.url);
        }
        }
        if (verbose) {
          std::fprintf(stderr, "[msg] %s%s: %s\n",
                       m.kind == MsgKind::Text ? "" : (m.kind == MsgKind::Paid ? "[paid] " : "[sub] "),
                       m.user.c_str(), m.plainText().c_str());
          // Emote image URLs, so a layout that looks wrong can be reproduced offline without
          // having to catch the room mid-emote again.
          for (const Fragment& part : m.parts) {
            if (part.kind == Fragment::Kind::Emote && !part.url.empty())
              std::fprintf(stderr, "[emote] %s -> %s\n", part.text.c_str(), part.url.c_str());
          }
        }
        sh.append(std::move(m));
        const int n = received.fetch_add(1) + 1;
        if (count > 0 && n >= count) {
          if (verbose) std::fprintf(stderr, "[site] reached --count %d\n", count);
          stop.store(true);
          if (video) video->quit();
          return false;  // stop the walk
        }
        return true;
      });
    }
  } catch (const std::exception& e) {
    sh.setState(std::string("failed: ") + e.what());
  }
}

/** A fixed set of messages covering every case the panel has to draw: latin and CJK names, an
 *  emoji, a line that wraps, each role colour, a paid card, a membership card, and two strings that
 *  must be rendered as literal text. The last two are the important ones: markup arriving in chat
 *  has to be drawn as glyphs, and cairo plus pango do that by construction because there is no
 *  markup interpretation anywhere in this path. */
/** Real CDN URLs for a few stock emotes, captured off a live stream. Used by --demo so the emote
 *  layout can be judged from a dump instead of having to catch a room mid-emote, which is how the
 *  baseline bug survived several rounds of looking. Only the pictures need the network; everything
 *  else in the demo is offline, and a missing picture degrades to the token as text. */
struct DemoEmote {
  const char* token;
  const char* url;
};
constexpr DemoEmote kDemoEmotes[] = {
    {"[花]", "http://i0.hdslb.com/bfs/live/7dd2ef03e13998575e4d8a803c6e12909f94e72b.png"},
    {"[委屈]", "http://i0.hdslb.com/bfs/live/69312e99a00d1db2de34ef2db9220c5686643a3f.png"},
    {"[笑哭]", "http://i0.hdslb.com/bfs/live/c5436c6806c32b28d471bb23d42f0f8f164a187a.png"},
    {"[哇]", "http://i0.hdslb.com/bfs/live/650c3e22c06edcbca9756365754d38952fc019c3.png"},
    {"[汤圆]", "http://i0.hdslb.com/bfs/live/23ae12d3a71b9d7a22c8773343969fcbb94b20d0.png"},
    {"[藏狐]", "http://i0.hdslb.com/bfs/live/05ef7849e7313e9c32887df922613a7c1ad27f12.png"},
};

Fragment emotePart(size_t i) {
  const DemoEmote& e = kDemoEmotes[i % (sizeof(kDemoEmotes) / sizeof(kDemoEmotes[0]))];
  Fragment f;
  f.kind = Fragment::Kind::Emote;
  f.text = e.token;
  f.url = e.url;
  f.px = 20;
  return f;
}

std::vector<Message> demoMessages() {
  struct Spec {
    const char* user;
    UserType type;
    const char* text;
  };
  const Spec plain[] = {
      {"博丽灵梦", UserType::Normal, "DU↗DU→DU↗DU↓ Max Verstappen"},
      {"Jim Hacker", UserType::Moderator, "Remember... no Russian"},
      {"Makarov", UserType::Normal, "🔨⚓ 我不做人了，JOJO"},
      {"Rick Astley", UserType::Member, "🎉 让我看看"},
      {"孙悟空", UserType::Normal, "🎉 23333"},
      {"哈基米", UserType::Normal, "⚓ <img src=1 onerror=\"alert('CHECK YOUR CODE')\">"},
      {"孙悟空", UserType::Normal, "你这猴儿，真令我欢喜"},
      {"Tifa Lockhart", UserType::Moderator, "🔨🎉 Remember... no Russian"},
      {"五条悟", UserType::Normal, "<script>alert(\"CHECK YOUR CODE\")</script>"},
      {"長崎そよ", UserType::Normal, "逃げるんだよ！"},
      {"柚木つばめ", UserType::Normal, "会員的"},
      {"御剑传奇", UserType::Normal, "無駄無駄無駄無駄無駄無駄無駄無駄無駄無駄"},
  };
  std::vector<Message> out;
  for (const Spec& s : plain) {
    Message m;
    m.user = s.user;
    m.type = s.type;
    m.parts.emplace_back(Fragment{Fragment::Kind::Text, s.text, "", 0});
    out.push_back(std::move(m));
  }

  Message paid;
  paid.kind = MsgKind::Paid;
  paid.user = "五条悟";
  paid.amount = "CN¥30.0";
  paid.parts.emplace_back(
      Fragment{Fragment::Kind::Text, "已经没有什么可怕的了", "", 0});
  out.push_back(paid);

  // A paid message that runs to several lines, which is the common case and the one that used to
  // be cut off after its first line.
  Message paidLong;
  paidLong.kind = MsgKind::Paid;
  paidLong.user = "ディオ・ブランドー";
  paidLong.amount = "CN¥50.0";
  paidLong.parts.emplace_back(Fragment{
      Fragment::Kind::Text,
      "主播今天讲的内容值得反复看，顺便问一下下一次直播大概是什么时候开始呀？我带朋友一起过来",
      "", 0});
  paidLong.parts.emplace_back(emotePart(2));
  out.push_back(paidLong);

  Message sub;
  sub.kind = MsgKind::Membership;
  sub.user = "xfgryujk";
  sub.amount = "新会员";
  out.push_back(sub);

  Message after;
  after.user = "友好的益生菌";
  after.parts.emplace_back(Fragment{Fragment::Kind::Text, "弹幕姬启动", "", 0});
  out.push_back(after);

  // Emote layout cases, the ones that were impossible to reproduce on demand before.
  Message e1;
  e1.user = "表情测试";
  e1.parts.emplace_back(Fragment{Fragment::Kind::Text, "前面有字", "", 0});
  e1.parts.emplace_back(emotePart(0));
  e1.parts.emplace_back(Fragment{Fragment::Kind::Text, "后面也有字", "", 0});
  out.push_back(e1);

  Message e2;
  e2.user = "开头表情";
  e2.parts.emplace_back(emotePart(1));
  e2.parts.emplace_back(Fragment{Fragment::Kind::Text, "紧跟一段文字", "", 0});
  out.push_back(e2);

  Message e3;  // a line that is nothing but an emote
  e3.user = "纯表情";
  e3.parts.emplace_back(emotePart(3));
  out.push_back(e3);

  Message e4;  // long enough that the emote lands on a wrapped line
  e4.user = "换行表情";
  e4.parts.emplace_back(Fragment{
      Fragment::Kind::Text,
      "这一段文字特别长，长到需要折行，于是后面的表情会被挤到第二行上去", "", 0});
  e4.parts.emplace_back(emotePart(4));
  e4.parts.emplace_back(Fragment{Fragment::Kind::Text, "尾巴", "", 0});
  out.push_back(e4);

  Message e5;  // two emotes in a row, to see the gap between them
  e5.user = "连续表情";
  e5.parts.emplace_back(emotePart(5));
  e5.parts.emplace_back(emotePart(2));
  e5.parts.emplace_back(emotePart(0));
  out.push_back(e5);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  try {
    switch (parseArgs(argc, argv, o)) {
      case Args::Help: return 0;
      case Args::Error: return 2;
      case Args::Ok: break;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "argument error: %s\n\n", e.what());
    usage(stderr);
    return 2;
  }

  if (o.room.empty() && !o.demo) {
    std::fprintf(stderr, "--room is required, or --demo for the offline layout\n");
    return 2;
  }
  if (!o.cookie.empty() && !o.cookieFile.empty()) {
    std::fprintf(stderr, "--cookie and --cookie-file are mutually exclusive\n");
    return 2;
  }
  if (!o.cookie.empty()) {
    // Not a refusal: the value is genuinely useful where writing a secret file is awkward. The
    // warning is about visibility to other processes, which the file option avoids.
    std::fprintf(stderr,
                 "warning: --cookie puts the session in the process arguments, where any local user\n"
                 "         can read it from ps(1). Prefer --cookie-file with a chmod 600 file.\n");
  }
  if (!o.cookieFile.empty()) {
    std::FILE* f = std::fopen(o.cookieFile.c_str(), "rb");
    if (!f) {
      std::fprintf(stderr, "cannot read cookie file: %s\n", o.cookieFile.c_str());
      return 1;
    }
    char buf[8192];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    o.cookie = buf;
    // A text file usually ends in a newline; it would otherwise become part of the header value.
    while (!o.cookie.empty() && (o.cookie.back() == '\n' || o.cookie.back() == '\r'))
      o.cookie.pop_back();
  }

  BiliConfig cfg;
  cfg.cookie = o.cookie;
  Bili bili(cfg);

  PanelTokens tokens = defaultTokens();
  if (!o.font.empty()) tokens.font = o.font;

  Shared sh;
  sh.roomLabel = o.demo ? std::string("demo") : o.room;
  // 48 px decoded into a 24 px box, so the face stays crisp on a hidpi canvas without storing four
  // times the pixels it needs. The capacity is separate and only bounds memory: a busy room shows
  // roughly 36 rows, and a visible row's avatar has to outlive the messages that push it off.
  ImageStore avatars(48, 256, bili.userAgent());
  // Emotes are a small closed set reused by everyone, so they are worth far more entries than a
  // face is; 48 px is enough at the 24 px box they are drawn into.
  ImageStore emoteImages(48, 512, bili.userAgent());
  Panel panel(tokens);
  panel.setImageStore(&avatars);
  panel.setEmoteStore(&emoteImages);
  panel.resize(o.width, o.height);
  pwvideo::CairoFrame frame(o.width, o.height);

  // Reused across frames so the render path does not allocate.
  std::vector<Message> fresh;

  auto drawOnce = [&](int64_t now) {
    drain(sh, fresh);
    panel.update(fresh, now);
    panel.render(frame.cr(), now);
  };

  /** Renders a short run and leaves the settled state in the frame.
   *
   *  The entrance animation lasts --animMs, so a single frame taken at the instant a message lands
   *  samples it at alpha 0 and shows nothing at all. A viewer never sees that; they see the row
   *  after the fade. This is the same reasoning the sibling project applies to its own stateful
   *  effects: advance the clock past the transient and keep the last frame. */
  auto settleForDump = [&](int frames = 12) {
    const int64_t t0 = steadyMs();
    for (int i = 0; i < frames; ++i) panel.render(frame.cr(), t0 + (i + 1) * 33);
  };

  if (o.demo) {
    // Everything is known up front, so paint the list in one go rather than row by row.
    sh.setState("demo");
    const std::vector<Message> msgs = demoMessages();
    panel.rebuildLayer(msgs);

    // Only the emote pictures need the network here, and only because their URLs are the site's
    // own CDN ones rather than something checked in. Everything else -- every message, both card
    // kinds, all the text -- is offline, and a picture that will not download degrades to its
    // token as text rather than breaking the layout.
    std::thread emoteThread([&emoteImages] { emoteImages.runWorker(); });
    for (const Message& m : msgs) {
      for (const Fragment& part : m.parts) {
        if (part.kind == Fragment::Kind::Emote && !part.url.empty()) emoteImages.request(part.url);
      }
    }
    const int64_t emoteUntil = steadyMs() + 8000;
    while (emoteImages.pending() > 0 && steadyMs() < emoteUntil)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    emoteImages.stop();
    if (emoteThread.joinable()) emoteThread.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // let the last one land

    if (!o.dump.empty()) {
      panel.render(frame.cr(), steadyMs());
      settleForDump();
      if (!frame.writePng(o.dump)) {
        std::fprintf(stderr, "PNG write failed: %s\n", o.dump.c_str());
        return 1;
      }
      std::printf("Wrote %s (%dx%d), %zu emotes cached, %llu failed\n", o.dump.c_str(), o.width,
                  o.height, emoteImages.cached(),
                  static_cast<unsigned long long>(emoteImages.failed()));
      return 0;
    }
  }

  std::atomic<bool> stop{false};
  std::atomic<int> received{0};

  if (o.dump.empty()) {
    try {
      pwvideo::Options opt;
      opt.width = o.width;
      opt.height = o.height;
      opt.fpsCap = o.fps;
      opt.nodeName = o.node;
      opt.nodeDescription = o.desc;
      opt.appName = "pw-live-danmaku";
      opt.verbose = o.verbose;

      pwvideo::VideoNode video(opt, [&](uint8_t* dst, int stride, int w, int h) {
        drawOnce(steadyMs());
        // The negotiated w/h may be smaller than the frame; blitTo clips, which is why the OBS
        // source size has to match --size rather than being free to differ.
        frame.blitTo(dst, stride, w, h);
      });
      video.start();

      std::printf(
          "pw-live-danmaku (native) started\n"
          "  PipeWire node: %s   [select it as a \"PipeWire Video\" source in OBS]\n"
          "  Size: %dx%d @ %d fps\n",
          o.node.c_str(), o.width, o.height, o.fps);
      if (o.demo)
        std::printf("  Mode:  demo (no network)\n");
      else
        std::printf("  Room:  %s\n  Auth:  %s\n", o.room.c_str(),
                    o.cookie.empty() ? "anonymous (nicknames masked)" : "cookie");
      std::fflush(stdout);

      std::thread avatarThread([&avatars] { avatars.runWorker(); });
      std::thread emoteThread([&emoteImages] { emoteImages.runWorker(); });

      std::thread site;
      if (!o.demo) {
        site = std::thread(siteLoop, std::ref(sh), std::ref(bili), std::ref(avatars),
                           std::ref(emoteImages), o.room, o.verbose, std::ref(stop), &video,
                           std::ref(received), o.count);
      }

      std::thread killer;
      if (o.seconds > 0) {
        killer = std::thread([&] {
          const int64_t until = steadyMs() + static_cast<int64_t>(o.seconds) * 1000LL;
          while (!stop.load() && steadyMs() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          if (!stop.load()) {
            std::fprintf(stderr, "[site] reached --seconds %d\n", o.seconds);
            stop.store(true);
            video.quit();
          }
        });
      }

      // Avatar bookkeeping is invisible from the picture: a message whose avatar has not arrived
      // yet looks exactly like one whose user has no picture. --verbose prints the counters and the
      // last failing URL so the two can be told apart.
      std::thread stats;
      if (o.verbose) {
        stats = std::thread([&] {
          while (!stop.load()) {
            for (int i = 0; i < 10 && !stop.load(); ++i)
              std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (stop.load()) break;
            const std::string err = avatars.lastError();
            std::fprintf(stderr,
                         "[avatar] cached=%zu pending=%zu fetched=%llu failed=%llu%s%s\n"
                         "[avatar] url shapes: empty=%llu https=%llu http=%llu proto-rel=%llu other=%llu\n",
                         avatars.cached(), avatars.pending(),
                         static_cast<unsigned long long>(avatars.fetched()),
                         static_cast<unsigned long long>(avatars.failed()),
                         err.empty() ? "" : "  last-fail: ", err.c_str(),
                         static_cast<unsigned long long>(sh.avatarEmpty),
                         static_cast<unsigned long long>(sh.avatarHttps),
                         static_cast<unsigned long long>(sh.avatarHttp),
                         static_cast<unsigned long long>(sh.avatarProtoRel),
                         static_cast<unsigned long long>(sh.avatarOther));
          }
        });
      }
      video.run();  // Blocks until SIGINT/SIGTERM, --count, --seconds, or the site loop gives up
      stop.store(true);
      if (site.joinable()) site.join();
      avatars.stop();
      emoteImages.stop();
      if (avatarThread.joinable()) avatarThread.join();
      if (emoteThread.joinable()) emoteThread.join();
      if (stats.joinable()) stats.join();
      if (killer.joinable()) killer.join();
      return 0;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "startup failed: %s\n", e.what());
      return 1;
    }
  }

  // --dump against a live room: no PipeWire node at all, because with no consumer attached the
  // render callback is never invoked and the PNG would come out blank. The avatar worker runs here
  // too, so the dump shows real faces instead of placeholder discs -- which is what makes it useful
  // for judging a layout rather than only for reading the connection state.
  std::thread avatarThread([&avatars] { avatars.runWorker(); });
  std::thread emoteThread([&emoteImages] { emoteImages.runWorker(); });
  std::thread site(siteLoop, std::ref(sh), std::ref(bili), std::ref(avatars), std::ref(emoteImages),
                   o.room, o.verbose, std::ref(stop), static_cast<pwvideo::VideoNode*>(nullptr),
                   std::ref(received), o.count > 0 ? o.count : kDumpMessages);
  const int64_t until = steadyMs() + 90000;
  // Wait for a few messages rather than just the first: one row is not enough to judge a layout,
  // and a panel with real avatars in it needs more than one row to show them.
  const int want = o.count > 0 ? o.count : kDumpMessages;
  while (!stop.load() && received.load() < want && steadyMs() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop.store(true);
  site.join();
  // Bounded wait for the fetches already queued; a slow CDN must not hang the dump. The condition is
  // "there is still a backlog", not "nothing has arrived yet" -- the latter returns after the
  // first avatar and leaves the rest as placeholder discs.
  const int64_t avatarUntil = steadyMs() + 8000;
  while ((avatars.pending() > 0 || emoteImages.pending() > 0) && steadyMs() < avatarUntil)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  avatars.stop();
  emoteImages.stop();
  if (avatarThread.joinable()) avatarThread.join();
  if (emoteThread.joinable()) emoteThread.join();
  // pending() clears when the worker picks a URL up, so allow the last in-flight one to land.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  drawOnce(steadyMs());
  settleForDump();
  if (!frame.writePng(o.dump)) {
    std::fprintf(stderr, "PNG write failed: %s\n", o.dump.c_str());
    return 1;
  }
  std::printf("Wrote %s (%dx%d), received %d messages\n", o.dump.c_str(), o.width, o.height,
              received.load());
  return 0;
}
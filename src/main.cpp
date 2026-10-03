// pw-live-danmaku (native) -- bilibili live chat as a PipeWire video node.
//
// M0 scope: prove the chain end to end. HTTP bootstrap -> websocket -> auth -> brotli -> DANMU_MSG,
// and the frames out through the video node. The picture is a diagnostic card, not the chat panel:
// the panel is M1. What is deliberately settled here is the part that is hardest to change later --
// the thread split, the handoff to the render callback, and the CLI surface.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cairo/cairo.h>

#include "bili.hpp"
#include "cairo_util.hpp"
#include "pwvideo.hpp"
#include "text.hpp"
#include "ws.hpp"

namespace {

using namespace dwm;

int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/** Font chain. The comma form is a pango fallback chain resolved per character, so latin glyphs can
 *  come from one family and CJK from another. Overridable from M1 onwards with --font. */
constexpr const char* kFontChain = "Noto Sans CJK SC,Noto Sans SC,DejaVu Sans,sans-serif";

constexpr size_t kRecentKeep = 12;  // rows the diagnostic card shows

void usage(std::FILE* out) {
  std::fprintf(
      out,
      "pw-live-danmaku (native)\n"
      "\n"
      "  --room ID|URL      Bilibili room, e.g. 545068 or https://live.bilibili.com/545068\n"
      "                     Required. A b23.tv short link also works.\n"
      "  --cookie STR       Raw cookie header from the browser, e.g.\n"
      "                     \"SESSDATA=...; bili_jct=...; DedeUserID=...\".\n"
      "                     Optional: anonymous works but masks nicknames. Visible in ps(1),\n"
      "                     so prefer --cookie-file.\n"
      "  --cookie-file PATH Read the cookie from a file instead; recommended, since the file can\n"
      "                     be chmod 600 and the value never reaches the process arguments.\n"
      "  --node NAME        PipeWire node name, default pw-live-danmaku\n"
      "  --desc TEXT        Node description (this is what the OBS dropdown shows), default\n"
      "                     \"Live Chat\"\n"
      "  --size WxH         Output size, default 480x900. The OBS source size must match:\n"
      "                     a smaller negotiated size is clipped, not scaled.\n"
      "  --fps N            Frame-rate ceiling, default 30\n"
      "  --dump FILE        Render one sample frame to PNG and exit\n"
      "  --count N          Exit after receiving N danmaku (0 = never)\n"
      "  --seconds N        Exit after N seconds (0 = never)\n"
      "  --verbose, -v      Log the protocol handshake and every parsed danmaku\n"
      "  --help, -h\n");
}

/** Argument parsing has three outcomes, not two: a bad flag is not a request for help. Same
 *  reasoning as the sibling projects -- under Restart=always an unknown flag that exited 0 would
 *  be a silent restart loop reporting SUCCESS. */
enum class Args { Ok, Help, Error };

Args parseArgs(int argc, char** argv, std::string& room, std::string& cookie,
               std::string& cookieFile, std::string& node, std::string& desc, int& width,
               int& height, int& fps, std::string& dump, int& count, int& seconds, bool& verbose) {
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
      room = next(i);
    } else if (a == "--cookie") {
      cookie = next(i);
    } else if (a == "--cookie-file") {
      cookieFile = next(i);
    } else if (a == "--node") {
      node = next(i);
    } else if (a == "--desc") {
      desc = next(i);
    } else if (a == "--size") {
      const std::string v = next(i);
      const size_t x = v.find_first_of("xX*");
      if (x == std::string::npos) {
        width = height = std::max(64, std::stoi(v));
      } else {
        width = std::max(64, std::stoi(v.substr(0, x)));
        height = std::max(64, std::stoi(v.substr(x + 1)));
      }
    } else if (a == "--fps") {
      fps = std::max(1, std::stoi(next(i)));
    } else if (a == "--dump") {
      dump = next(i);
    } else if (a == "--count") {
      count = std::max(0, std::stoi(next(i)));
    } else if (a == "--seconds") {
      seconds = std::max(0, std::stoi(next(i)));
    } else if (a == "--verbose" || a == "-v") {
      verbose = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n\n", a.c_str());
      usage(stderr);
      return Args::Error;
    }
  }
  return Args::Ok;
}

/** A copy of everything the renderer needs, taken under the lock in one go. The render callback
 *  runs on the PipeWire main-loop thread and may not block on the network, so it renders from a
 *  value rather than holding a reference into live state -- the same shape the sibling project uses
 *  for its cover art. */
struct Snapshot {
  std::string state;
  std::string roomLabel;
  std::string lastEvent;
  std::vector<Danmaku> recent;
  uint64_t total = 0;
  uint64_t dropped = 0;
};

struct Shared {
  // mutable because snapshot() is a logically-const read that still has to take the lock.
  mutable std::mutex mu;
  Snapshot live;

  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lk(mu);
    return live;
  }
  void push(const Danmaku& d) {
    std::lock_guard<std::mutex> lk(mu);
    ++live.total;
    if (live.recent.size() >= kRecentKeep) live.recent.erase(live.recent.begin());
    live.recent.push_back(d);
  }
  void note(const std::string& event) {
    std::lock_guard<std::mutex> lk(mu);
    live.lastEvent = event;
  }
  void setState(std::string s) {
    std::lock_guard<std::mutex> lk(mu);
    live.state = std::move(s);
  }
  void drop() {
    std::lock_guard<std::mutex> lk(mu);
    ++live.dropped;
  }
};

/** The site thread: bootstrap over HTTP, then hold the websocket open, decoding into Shared.
 *
 *  Reconnect is M1 work; M0 reports the failure and stops, because a silent reconnect loop would
 *  hide exactly the thing this milestone exists to observe. video may be null when running for
 *  --dump, in which case there is nothing to quit and the loop just runs until the stop flag. */
void siteLoop(Shared& sh, Bili& bili, const std::string& roomInput, bool verbose,
              std::atomic<bool>& stop, pwvideo::VideoNode* video, std::atomic<int>& received,
              int count) {
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
    if (!ws.sendBinary(Bili::authPacket(roomId, ep.token))) {
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
        break;  // leave the wait loop and fall through to the streaming loop
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
        if (verbose) std::fprintf(stderr, "[site] heartbeat\n");
      }
      if (ev == WsClient::Event::Timeout) continue;
      if (ev == WsClient::Event::Error || ev == WsClient::Event::Close) {
        sh.setState(std::string(ev == WsClient::Event::Close ? "closed by peer" : "error") + ": " +
                    ws.lastError());
        return;
      }

      // The same walk handles both shapes a command message can take: a bare JSON document, and a
      // brotli batch of framed packets.
      Bili::forEachJson(msg, [&](std::string_view doc) {
        std::string err;
        const Json json = Json::parse(doc, &err);
        if (!err.empty()) {
          sh.drop();
          return true;
        }
        Danmaku d;
        if (!Bili::parseDanmaku(json, d)) {
          const std::string cmd = json["cmd"].str();
          if (!cmd.empty()) sh.note(cmd);
          return true;
        }
        sh.push(d);
        const int n = received.fetch_add(1) + 1;
        if (verbose)
          std::fprintf(stderr, "[dm] mode=%d fs=%d color=%06x %s: %s\n", d.mode, d.fontSize, d.color,
                       d.user.c_str(), d.text.c_str());
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

/** The diagnostic card. Replaced by the chat panel in M1; what it does prove is that the node
 *  carries pixels at the negotiated size, with alpha, to whatever consumer attaches. */
void drawDiagnostic(cairo_t* cr, const Snapshot& snap, int w) {
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.30);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  pwvideo::TextRenderer text;
  const double pad = 14.0;
  double y = pad + 18.0;

  auto line = [&](const std::string& s, double size, const pwvideo::Rgba& color, bool bold) {
    pwvideo::LabelSpec spec;
    spec.sizePx = size;
    spec.bold = bold;
    spec.center = false;
    spec.widthPx = w - 2 * pad;
    spec.maxLines = 1;
    spec.family = kFontChain;
    PangoLayout* l = text.layout(cr, s, spec);
    text.outline(cr, l, pad, y, 1.5, pwvideo::Rgba{0, 0, 0, 0.85});
    text.fill(cr, l, pad, y, color);
    const pwvideo::LabelMetrics m = pwvideo::TextRenderer::measure(l);
    y += m.height + 6.0;
  };

  const pwvideo::Rgba white{1, 1, 1, 1};
  const pwvideo::Rgba dim{0.78, 0.83, 0.88, 1};
  const pwvideo::Rgba bad{1.0, 0.45, 0.45, 1};

  line("pw-live-danmaku M0", 22.0, white, true);
  line("room " + snap.roomLabel, 14.0, dim, false);
  line(snap.state, 14.0, snap.state.rfind("failed", 0) == 0 ? bad : dim, false);
  char counts[128];
  std::snprintf(counts, sizeof(counts), "received %llu   dropped %llu",
                static_cast<unsigned long long>(snap.total),
                static_cast<unsigned long long>(snap.dropped));
  line(counts, 14.0, dim, false);
  if (!snap.lastEvent.empty()) line("last event: " + snap.lastEvent, 13.0, dim, false);
  y += 8.0;
  line("--- danmaku ---", 14.0, white, true);

  for (const Danmaku& d : snap.recent) {
    pwvideo::Rgba col{((d.color >> 16) & 0xFF) / 255.0, ((d.color >> 8) & 0xFF) / 255.0,
                      (d.color & 0xFF) / 255.0, 1.0};
    line(d.user + ": " + d.text, 14.0, col, false);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string room, cookie, cookieFile, node = "pw-live-danmaku", desc = "Live Chat";
  std::string dump;
  int width = 480, height = 900, fps = 30, count = 0, seconds = 0;
  bool verbose = false;

  try {
    switch (parseArgs(argc, argv, room, cookie, cookieFile, node, desc, width, height, fps, dump,
                      count, seconds, verbose)) {
      case Args::Help: return 0;
      case Args::Error: return 2;
      case Args::Ok: break;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "argument error: %s\n\n", e.what());
    usage(stderr);
    return 2;
  }

  if (room.empty()) {
    std::fprintf(stderr, "--room is required (see --help)\n");
    return 2;
  }
  if (!cookie.empty() && !cookieFile.empty()) {
    std::fprintf(stderr, "--cookie and --cookie-file are mutually exclusive\n");
    return 2;
  }
  if (!cookie.empty()) {
    // Not a refusal: the value is genuinely useful on a machine where writing a secret file is
    // awkward. The warning is about visibility to other processes, which the file option avoids.
    std::fprintf(stderr,
                 "warning: --cookie puts the session in the process arguments, where any local user\n"
                 "         can read it from ps(1). Prefer --cookie-file with a chmod 600 file.\n");
  }
  if (!cookieFile.empty()) {
    std::FILE* f = std::fopen(cookieFile.c_str(), "rb");
    if (!f) {
      std::fprintf(stderr, "cannot read cookie file: %s\n", cookieFile.c_str());
      return 1;
    }
    char buf[8192];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    cookie = buf;
    // A text file usually ends in a newline; it would otherwise become part of the header value.
    while (!cookie.empty() && (cookie.back() == '\n' || cookie.back() == '\r')) cookie.pop_back();
  }

  BiliConfig cfg;
  cfg.cookie = cookie;
  Bili bili(cfg);

  Shared sh;
  sh.live.roomLabel = room;
  pwvideo::CairoFrame frame(width, height);
  std::atomic<bool> stop{false};
  std::atomic<int> received{0};

  // --dump renders one sample frame and exits, so it must not start a PipeWire node at all: with
  // no consumer attached the render callback is never invoked, and the PNG would come out blank.
  if (!dump.empty()) {
    std::thread site(siteLoop, std::ref(sh), std::ref(bili), room, verbose, std::ref(stop),
                     static_cast<pwvideo::VideoNode*>(nullptr), std::ref(received), count);
    // Bounded: --dump must terminate even in a quiet room, so it gives up rather than hanging.
    const int64_t until = steadyMs() + 90000;
    while (!stop.load() && received.load() == 0 && steadyMs() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    site.join();

    drawDiagnostic(frame.cr(), sh.snapshot(), frame.width());
    if (!frame.writePng(dump)) {
      std::fprintf(stderr, "PNG write failed: %s\n", dump.c_str());
      return 1;
    }
    std::printf("Wrote %s (%dx%d), received %d danmaku\n", dump.c_str(), width, height,
                received.load());
    return 0;
  }

  try {
    pwvideo::Options opt;
    opt.width = width;
    opt.height = height;
    opt.fpsCap = fps;
    opt.nodeName = node;
    opt.nodeDescription = desc;
    opt.appName = "pw-live-danmaku";
    opt.verbose = verbose;

    pwvideo::VideoNode video(opt, [&](uint8_t* dst, int stride, int w, int h) {
      // The negotiated w/h may be smaller than the frame; blitTo clips, which is why the OBS
      // source size has to match --size rather than be free to differ.
      drawDiagnostic(frame.cr(), sh.snapshot(), frame.width());
      frame.blitTo(dst, stride, w, h);
    });
    video.start();

    std::printf(
        "pw-live-danmaku (native) started\n"
        "  PipeWire node: %s   [select it as a \"PipeWire Video\" source in OBS]\n"
        "  Size: %dx%d @ %d fps\n"
        "  Room: %s\n",
        node.c_str(), width, height, fps, room.c_str());
    std::printf("  Auth:  %s\n", cookie.empty() ? "anonymous (nicknames masked)" : "cookie");
    std::fflush(stdout);

    std::thread site(siteLoop, std::ref(sh), std::ref(bili), room, verbose, std::ref(stop),
                     &video, std::ref(received), count);

    std::thread killer;
    if (seconds > 0) {
      killer = std::thread([&] {
        const int64_t until = steadyMs() + static_cast<int64_t>(seconds) * 1000LL;
        while (!stop.load() && steadyMs() < until)
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!stop.load()) {
          std::fprintf(stderr, "[site] reached --seconds %d\n", seconds);
          stop.store(true);
          video.quit();
        }
      });
    }
    video.run();  // Blocks until SIGINT/SIGTERM, --count, --seconds, or the site loop gives up
    stop.store(true);
    if (site.joinable()) site.join();
    if (killer.joinable()) killer.join();
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "startup failed: %s\n", e.what());
    return 1;
  }
}
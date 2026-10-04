// The on-disk side of the chat history. See history.hpp for why the format, the write throttle and
// the failure policy are the way they are.
#include "history.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "json.hpp"

namespace dwm {
namespace {

/** Appends `s` as a JSON string, quotes included.
 *
 *  Only what JSON requires: the quote, the backslash and everything below 0x20. Danmaku bodies carry
 *  raw UTF-8 and pass through untouched -- \u-escaping them would make the file unreadable to a
 *  person, which is the whole reason this format is JSON rather than a length-prefixed blob. */
void appendJsonString(std::string& out, const std::string& s) {
  out += '"';
  for (const char raw : s) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          static const char* kHex = "0123456789abcdef";
          out += "\\u00";
          out += kHex[c >> 4];
          out += kHex[c & 0x0F];
        } else {
          out += raw;
        }
    }
  }
  out += '"';
}

/** A field, cut to kMaxFieldBytes. Cutting rather than dropping: a message too long to keep whole
 *  still says what it was about, whereas a skipped one leaves a hole in the window. */
std::string clip(const std::string& s) {
  return s.size() <= HistoryStore::kMaxFieldBytes ? s : s.substr(0, HistoryStore::kMaxFieldBytes);
}

// Kinds and types are written as names, not ordinals: an ordinal that moves between versions turns
// every saved row into a row of the wrong kind, and a name that is not recognised degrades to plain
// chat rather than to nonsense.
const char* kindName(MsgKind k) {
  switch (k) {
    case MsgKind::Paid: return "paid";
    case MsgKind::Membership: return "membership";
    case MsgKind::Gift: return "gift";
    case MsgKind::Entry: return "entry";
    case MsgKind::Like: return "like";
    case MsgKind::Text: break;
  }
  return "text";
}

MsgKind kindFromName(const std::string& s) {
  if (s == "paid") return MsgKind::Paid;
  if (s == "membership") return MsgKind::Membership;
  if (s == "gift") return MsgKind::Gift;
  if (s == "entry") return MsgKind::Entry;
  if (s == "like") return MsgKind::Like;
  return MsgKind::Text;
}

const char* typeName(UserType t) {
  switch (t) {
    case UserType::Member: return "member";
    case UserType::Moderator: return "moderator";
    case UserType::Owner: return "owner";
    case UserType::Normal: break;
  }
  return "normal";
}

UserType typeFromName(const std::string& s) {
  if (s == "member") return UserType::Member;
  if (s == "moderator") return UserType::Moderator;
  if (s == "owner") return UserType::Owner;
  return UserType::Normal;
}

void writeMessage(std::string& out, const Message& m) {
  out += "{\"kind\":";
  appendJsonString(out, kindName(m.kind));
  out += ",\"type\":";
  appendJsonString(out, typeName(m.type));
  out += ",\"user\":";
  appendJsonString(out, clip(m.user));
  out += ",\"color\":" + std::to_string(m.userColor);
  out += ",\"avatar\":";
  appendJsonString(out, m.avatarUrl);
  out += ",\"ts\":" + std::to_string(m.tsMs);
  out += ",\"amount\":";
  appendJsonString(out, m.amount);
  out += ",\"amountValue\":" + std::to_string(m.amountValue);
  out += ",\"count\":" + std::to_string(m.count);
  out += ",\"mergeKey\":";
  appendJsonString(out, m.mergeKey);
  out += ",\"parts\":[";
  for (size_t i = 0; i < m.parts.size(); ++i) {
    const Fragment& f = m.parts[i];
    if (i > 0) out += ',';
    out += "{\"emote\":";
    out += f.kind == Fragment::Kind::Emote ? '1' : '0';
    out += ",\"text\":";
    appendJsonString(out, clip(f.text));
    out += ",\"url\":";
    appendJsonString(out, f.url);
    out += ",\"px\":" + std::to_string(f.px);
    out += ",\"verb\":";
    out += f.verb ? "true" : "false";
    out += '}';
  }
  out += "]}";
}

Message readMessage(const Json& j) {
  Message m;
  m.kind = kindFromName(j.get("kind").str());
  m.type = typeFromName(j.get("type").str());
  m.user = j.get("user").str();
  m.userColor = static_cast<uint32_t>(j.get("color").num());
  m.avatarUrl = j.get("avatar").str();
  m.tsMs = j.get("ts").num();
  m.amount = j.get("amount").str();
  m.amountValue = j.get("amountValue").num();
  m.count = j.get("count").num();
  m.mergeKey = j.get("mergeKey").str();
  const Json& parts = j.get("parts");
  for (size_t i = 0; i < parts.size(); ++i) {
    const Json& p = parts[i];
    Fragment f;
    f.kind = p.get("emote").num() ? Fragment::Kind::Emote : Fragment::Kind::Text;
    f.text = p.get("text").str();
    f.url = p.get("url").str();
    f.px = static_cast<int>(p.get("px").num());
    f.verb = p.get("verb").boolean();
    m.parts.push_back(std::move(f));
  }
  return m;
}

/** The whole document, as one string. Built in memory and written in one go: the file is bounded by
 *  the window size, so there is nothing a streaming writer would buy here. */
std::string buildDocument(const std::string& room, const std::deque<Message>& items) {
  std::string out;
  out.reserve(256 + items.size() * 160);
  out += "{\"format\":" + std::to_string(HistoryStore::kFormat);
  out += ",\"saved\":";
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  char stamp[32] = {0};
  if (now != static_cast<std::time_t>(-1) && gmtime_r(&now, &tm))
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &tm);
  appendJsonString(out, stamp);
  out += ",\"room\":";
  appendJsonString(out, room);
  out += ",\"messages\":[";
  bool first = true;
  for (const Message& m : items) {
    if (!first) out += ',';
    first = false;
    writeMessage(out, m);
  }
  out += "]}\n";
  return out;
}

}  // namespace

std::string HistoryStore::defaultPath() {
  std::string base;
  // A relative XDG_STATE_HOME is ignored, per the XDG base directory spec: resolving it against the
  // working directory would put the file somewhere that depends on where the unit was started from.
  const char* state = std::getenv("XDG_STATE_HOME");
  if (state && state[0] == '/')
    base = state;
  else {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return std::string();
    base = std::string(home) + "/.local/state";
  }
  return base + "/pw-live-danmaku/history.json";
}

bool HistoryStore::add(const Message& m, int64_t nowMs) {
  if (!enabled_) return false;
  items_.push_back(m);
  while (items_.size() > capacity_) items_.pop_front();
  ++unsaved_;

  // The first row of a run is due straight away. Without that, a process killed before the eighth
  // message would leave nothing on disk at all, and history is the one feature whose absence is
  // invisible until the moment it is wanted.
  //
  //  unsaved_ is deliberately *not* cleared here: only save() clears it, so a caller that ignores
  // the return value merely delays the write instead of losing the rows at the exit save.
  if (unsaved_ >= kFlushRows || lastFlushMs_ == 0 || nowMs - lastFlushMs_ >= kFlushMs) {
    lastFlushMs_ = nowMs;
    return true;
  }
  return false;
}

bool HistoryStore::save() {
  if (!enabled_ || unsaved_ == 0) return false;

  // One warning, then off for the rest of the run. errno is read where it is set, before anything
  // else can overwrite it.
  const auto bail = [&](const std::string& what, int err) {
    std::fprintf(stderr, "[history] %s: %s\n[history] history is off for the rest of this run\n",
                 what.c_str(), std::strerror(err));
    enabled_ = false;
    return false;
  };

  const std::filesystem::path p(path_);
  if (p.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    // ec is also set when the directory already existed and is perfectly fine, so the test is
    // whether it is a directory rather than whether an error was reported.
    if (ec && !std::filesystem::is_directory(p.parent_path()))
      return bail("cannot create " + p.parent_path().string(), ec.value());
  }

  const std::string doc = buildDocument(room_, items_);
  // Same directory as the target, so rename() stays within one filesystem and is therefore atomic.
  const std::string tmp = path_ + ".tmp";

  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return bail("cannot write " + tmp, errno);
  const size_t written = std::fwrite(doc.data(), 1, doc.size(), f);
  const int closed = std::fclose(f);
  if (written != doc.size() || closed != 0) {
    const int err = errno;
    std::remove(tmp.c_str());
    return bail("cannot write " + tmp, err);
  }
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
    const int err = errno;
    std::remove(tmp.c_str());
    return bail("cannot replace " + path_, err);
  }

  unsaved_ = 0;
  return true;
}

std::vector<Message> HistoryStore::restore() {
  std::vector<Message> out;
  if (!enabled_) return out;

  std::FILE* f = std::fopen(path_.c_str(), "rb");
  if (!f) return out;  // A first run has no file, and that is not a problem worth a line on stderr.

  std::string text;
  bool tooBig = false;
  char buf[8192];
  for (size_t n = std::fread(buf, 1, sizeof buf, f); n > 0; n = std::fread(buf, 1, sizeof buf, f)) {
    text.append(buf, n);
    if (text.size() > kMaxFileBytes) {
      tooBig = true;
      break;
    }
  }
  std::fclose(f);
  if (tooBig) {
    std::fprintf(stderr, "[history] %s is larger than %zu bytes; ignoring it\n", path_.c_str(),
                 kMaxFileBytes);
    return out;
  }

  std::string err;
  const Json doc = Json::parse(text, &err);
  if (doc.isNull()) {
    std::fprintf(stderr, "[history] ignoring %s: %s\n", path_.c_str(), err.c_str());
    return out;
  }
  if (doc.get("format").num() != kFormat) {
    std::fprintf(stderr, "[history] ignoring %s: format %lld is not version %d\n", path_.c_str(),
                 static_cast<long long>(doc.get("format").num()), kFormat);
    return out;
  }
  const std::string& room = doc.get("room").str();
  if (!room.empty() && !room_.empty() && room != room_) {
    std::fprintf(stderr, "[history] ignoring %s: it holds room %s, this run is %s\n", path_.c_str(),
                 room.c_str(), room_.c_str());
    return out;
  }

  const Json& msgs = doc.get("messages");
  out.reserve(std::min(msgs.size(), capacity_));
  for (size_t i = 0; i < msgs.size(); ++i) out.push_back(readMessage(msgs[i]));
  // The file may be larger than this run's window -- --history N shrank, or the file was written by
  // a run with a larger one. Keep the tail: these are the rows that were on screen when it ended.
  if (out.size() > capacity_) out.erase(out.begin(), out.begin() + (out.size() - capacity_));

  // Seeded at the front of the window, and not marked unsaved: these rows are already in the file,
  // so a save() before anything new arrives would only rewrite what is there.
  items_.clear();
  for (const Message& m : out) items_.push_back(m);
  while (items_.size() > capacity_) items_.pop_front();
  return out;
}

}  // namespace dwm

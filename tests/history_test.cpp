// Unit tests for the history store: the JSON round trip, the bounded window, the write throttle and
// what happens to a file that cannot be used. Plain asserts, like the other two suites -- the build
// target is two lines in the Makefile and the alternative is a dependency this project does not
// otherwise have.
//
// The round trip is what everything else rests on. A field that is not written, or written under a
// name that is not read back, is a message that comes back subtly wrong -- a gift drawn as chat, an
// emote drawn as its token -- and none of that is visible in a diff, only on the overlay.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "history.hpp"

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

using dwm::HistoryStore;
using dwm::Message;

std::filesystem::path gDir;

std::string path(const char* name) { return (gDir / name).string(); }

/** A message with every field set to something distinguishable, so a round trip that drops one is a
 *  round trip that fails rather than one that looks fine. */
Message sample(dwm::MsgKind kind, const std::string& user, const std::string& body) {
  Message m;
  m.kind = kind;
  m.type = dwm::UserType::Moderator;
  m.user = user;
  m.userColor = 0x123456;
  m.avatarUrl = "https://example.invalid/face.jpg";
  m.amount = "CN¥30";
  m.amountValue = 30;
  m.count = 6;
  m.mergeKey = "gift:1:2";
  m.tsMs = 1791046553806;
  m.parts.emplace_back(dwm::Fragment{dwm::Fragment::Kind::Text, body, "", 0, true});
  m.parts.emplace_back(
      dwm::Fragment{dwm::Fragment::Kind::Emote, "[热]", "https://example.invalid/e.png", 28, false});
  return m;
}

void checkSame(const Message& got, const Message& want, const char* what) {
  check(got.kind == want.kind, what);
  check(got.type == want.type, what);
  checkEq(got.user, want.user, what);
  checkEq(got.userColor, want.userColor, what);
  checkEq(got.avatarUrl, want.avatarUrl, what);
  checkEq(got.amount, want.amount, what);
  checkEq(got.amountValue, want.amountValue, what);
  checkEq(got.count, want.count, what);
  checkEq(got.mergeKey, want.mergeKey, what);
  checkEq(got.tsMs, want.tsMs, what);
  checkEq(got.parts.size(), want.parts.size(), what);
  if (got.parts.size() != want.parts.size()) return;
  for (size_t i = 0; i < want.parts.size(); ++i) {
    check(got.parts[i].kind == want.parts[i].kind, what);
    checkEq(got.parts[i].text, want.parts[i].text, what);
    checkEq(got.parts[i].url, want.parts[i].url, what);
    checkEq(got.parts[i].px, want.parts[i].px, what);
    check(got.parts[i].verb == want.parts[i].verb, what);
  }
}

void testRoundTrip() {
  const std::string p = path("round-trip.json");
  HistoryStore store(p, 16);
  store.setRoom("545068");
  const std::vector<Message> want = {
      sample(dwm::MsgKind::Text, "观众甲", "你好"),
      sample(dwm::MsgKind::Gift, "观众乙", "投喂"),
      sample(dwm::MsgKind::Membership, "观众丙", "舰长"),
      sample(dwm::MsgKind::Entry, "观众丁", "进入"),
      sample(dwm::MsgKind::Like, "观众戊", "点赞"),
  };
  int64_t t = 1000;
  for (const Message& m : want) store.add(m, t += 10000);  // each add is due, so every one saves
  check(store.save(), "round trip: something was written");

  const std::vector<Message> got = store.restore();
  checkEq(got.size(), want.size(), "round trip: row count");
  for (size_t i = 0; i < got.size() && i < want.size(); ++i) checkSame(got[i], want[i], "round trip");

  // plainText() is derived from the parts, so it is the cheapest way to see that the runs came back
  // in the order they went in.
  checkEq(got[0].plainText(), want[0].plainText(), "round trip: body text");
  std::remove(p.c_str());
}

void testKindsSurviveAsNames() {
  // Written as names rather than ordinals: an ordinal that moves between builds turns every saved
  // row into a row of the wrong kind. An unrecognised name must degrade to plain chat, not throw.
  const std::string p = path("kinds.json");
  std::FILE* f = std::fopen(p.c_str(), "wb");
  std::fprintf(f,
               R"({"format":1,"saved":"2026-01-01T00:00:00Z","messages":[)"
               R"({"kind":"gift","type":"owner","user":"u","parts":[]},)"
               R"({"kind":"from-the-future","type":"from-the-future","user":"v","parts":[]}]})");
  std::fclose(f);
  HistoryStore store(p, 8);
  const std::vector<Message> got = store.restore();
  checkEq(got.size(), size_t(2), "kinds: both rows read");
  check(got[0].kind == dwm::MsgKind::Gift, "kinds: gift stays a gift");
  check(got[0].type == dwm::UserType::Owner, "kinds: owner stays owner");
  check(got[1].kind == dwm::MsgKind::Text, "kinds: an unknown kind is read as chat");
  check(got[1].type == dwm::UserType::Normal, "kinds: an unknown type is read as normal");
  std::remove(p.c_str());
}

void testWindowBound() {
  const std::string p = path("window.json");
  HistoryStore store(p, 3);
  int64_t t = 0;
  for (int i = 0; i < 5; ++i) {
    Message m;
    m.user = "u" + std::to_string(i);
    m.parts.emplace_back(dwm::Fragment{dwm::Fragment::Kind::Text, "row", "", 0, false});
    store.add(m, t += 10000);
  }
  checkEq(store.size(), size_t(3), "window: the deque is bounded");
  check(store.save(), "window: written");
  const std::vector<Message> got = store.restore();
  checkEq(got.size(), size_t(3), "window: the file holds the window, not the run");
  // The tail, in order: these are the rows that were on screen when the run ended, so the oldest
  // are the ones to go.
  checkEq(got[0].user, std::string("u2"), "window: oldest kept row");
  checkEq(got[2].user, std::string("u4"), "window: newest kept row");
  std::remove(p.c_str());
}

void testLoadCutsToThisRunsWindow() {
  // A file written by a run with a larger window, read by one with a smaller: the tail again.
  const std::string p = path("shrink.json");
  {
    HistoryStore wide(p, 10);
    int64_t t = 0;
    for (int i = 0; i < 8; ++i) {
      Message m;
      m.user = "u" + std::to_string(i);
      m.parts.emplace_back(dwm::Fragment{dwm::Fragment::Kind::Text, "row", "", 0, false});
      wide.add(m, t += 10000);
    }
    wide.save();
  }
  HistoryStore narrow(p, 2);
  const std::vector<Message> got = narrow.restore();
  checkEq(got.size(), size_t(2), "shrink: cut to this run's window");
  checkEq(got[0].user, std::string("u6"), "shrink: the newest rows are the ones kept");
  std::remove(p.c_str());
}

void testRestoreSeedsTheWindow() {
  // The point of seeding: a restart that saw two rows must leave a file that still holds the whole
  // window, not a file that has shrunk to the two rows this run happened to see.
  const std::string p = path("seed.json");
  {
    HistoryStore first(p, 10);
    int64_t t = 0;
    for (int i = 0; i < 6; ++i) {
      Message m;
      m.user = "old" + std::to_string(i);
      m.parts.emplace_back(dwm::Fragment{dwm::Fragment::Kind::Text, "row", "", 0, false});
      first.add(m, t += 10000);
    }
    first.save();
  }

  HistoryStore second(p, 10);
  const std::vector<Message> restored = second.restore();
  checkEq(restored.size(), size_t(6), "seed: the previous window came back");
  checkEq(second.size(), size_t(6), "seed: the window is seeded with it, not emptied");
  check(!second.hasUnsaved(), "seed: restoring is not a change, so nothing to write");

  Message m;
  m.user = "new";
  m.parts.emplace_back(dwm::Fragment{dwm::Fragment::Kind::Text, "row", "", 0, false});
  second.add(m, 100000);
  second.save();

  const std::vector<Message> got = second.restore();
  checkEq(got.size(), size_t(7), "seed: a short run keeps the history it inherited");
  checkEq(got.back().user, std::string("new"), "seed: the newest row is last");
  std::remove(p.c_str());
}

void testEscapes() {
  const std::string p = path("escapes.json");
  HistoryStore store(p, 4);
  Message m;
  // A quote, a backslash, a newline, a tab, a raw control byte and some multi-byte UTF-8: the
  // shapes a danmaku body can actually contain, none of which may come back changed.
  m.user = "a\"b\\c";
  m.parts.emplace_back(dwm::Fragment{
      dwm::Fragment::Kind::Text, "line1\nline2\ttab\x01" "中😀", "", 0, false});
  store.add(m, 1);
  check(store.save(), "escapes: written");

  const std::vector<Message> got = store.restore();
  checkEq(got.size(), size_t(1), "escapes: one row back");
  if (got.size() == 1) {
    checkEq(got[0].user, std::string("a\"b\\c"), "escapes: quotes and backslashes");
    checkEq(got[0].parts.at(0).text, m.parts.at(0).text, "escapes: control bytes and UTF-8");
  }
  std::remove(p.c_str());
}

void testLongFieldIsCut() {
  const std::string p = path("long.json");
  HistoryStore store(p, 2);
  Message m;
  m.parts.emplace_back(dwm::Fragment{
      dwm::Fragment::Kind::Text, std::string(9000, 'x'), "", 0, false});
  store.add(m, 1);
  store.save();
  const std::vector<Message> got = store.restore();
  checkEq(got.size(), size_t(1), "long field: the row is kept");
  if (got.size() == 1)
    checkEq(got[0].parts.at(0).text.size(), HistoryStore::kMaxFieldBytes,
            "long field: cut to the bound rather than dropped");
  std::remove(p.c_str());
}

void testThrottle() {
  const std::string p = path("throttle.json");
  HistoryStore store(p, 100);
  Message m;
  m.parts.emplace_back(dwm::Fragment{dwm::Fragment::Kind::Text, "row", "", 0, false});

  // The first row of a run is due immediately: a process killed before the eighth message would
  // otherwise leave nothing behind at all.
  check(store.add(m, 1000), "throttle: the first row is due at once");
  check(store.save(), "throttle: it was written");
  check(!store.save(), "throttle: nothing to write the second time");

  for (int i = 1; i < static_cast<int>(HistoryStore::kFlushRows); ++i)
    check(!store.add(m, 1000 + i), "throttle: a burst is batched");
  check(store.add(m, 1000 + HistoryStore::kFlushRows), "throttle: due at the row bound");
  check(store.save(), "throttle: the batch was written");

  // And by age, for a room too quiet to reach the row bound.
  check(!store.add(m, 2000), "throttle: not due yet");
  check(store.add(m, 2000 + HistoryStore::kFlushMs), "throttle: due at the age bound");
  std::remove(p.c_str());
}

void testUnusableFiles() {
  // A history is a nicety. None of these may throw, and none may stop the overlay from starting.
  const std::vector<const char*> broken = {
      "",                                    // empty
      "{",                                   // truncated
      "{\"format\":1,\"messages\":[{\"kind\"}",  // truncated inside a row
      "[]",                                  // valid JSON, wrong shape
      "{\"format\":99,\"messages\":[]}",      // a format this build does not know
  };
  for (size_t i = 0; i < broken.size(); ++i) {
    const std::string p = path("broken.json");
    std::FILE* f = std::fopen(p.c_str(), "wb");
    std::fputs(broken[i], f);
    std::fclose(f);
    HistoryStore store(p, 8);
    checkEq(store.restore().size(), size_t(0), "broken file: loads as no history");
    std::remove(p.c_str());
  }

  // Missing is not broken: a first run has no file at all.
  HistoryStore missing(path("never-written.json"), 8);
  checkEq(missing.restore().size(), size_t(0), "missing file: loads as no history");

  // A file belonging to another room must not be mixed in.
  const std::string p = path("room.json");
  {
    HistoryStore a(p, 8);
    a.setRoom("545068");
    Message m;
    m.parts.emplace_back(dwm::Fragment{dwm::Fragment::Kind::Text, "row", "", 0, false});
    a.add(m, 1);
    a.save();
  }
  HistoryStore b(p, 8);
  b.setRoom("1746707149");
  checkEq(b.restore().size(), size_t(0), "room: another room's file is not read");
  HistoryStore c(p, 8);  // no room asked for: take what is there
  checkEq(c.restore().size(), size_t(1), "room: no room asked for reads anything");
  std::remove(p.c_str());
}

void testAtomicReplace() {
  const std::string p = path("atomic.json");
  HistoryStore store(p, 8);
  Message m;
  m.user = "first";
  m.parts.emplace_back(dwm::Fragment{dwm::Fragment::Kind::Text, "row", "", 0, false});
  store.add(m, 1);
  store.save();

  Message second = m;
  second.user = "second";
  store.add(second, 2);
  store.save();

  // The temporary file must not survive a successful write, and the target must hold the newer
  // document rather than both.
  check(!std::filesystem::exists(p + ".tmp"), "atomic: no temporary file left behind");
  const std::vector<Message> got = store.restore();
  checkEq(got.size(), size_t(2), "atomic: the target holds the whole window");
  checkEq(got[1].user, std::string("second"), "atomic: the newest write is the one on disk");
  std::remove(p.c_str());
}

void testDisabledStore() {
  // --history 0, or a path that could not be resolved: the store is off and stays quiet.
  HistoryStore off(path("off.json"), 0);
  check(!off.enabled(), "disabled: capacity 0 is off");
  Message m;
  m.parts.emplace_back(dwm::Fragment{dwm::Fragment::Kind::Text, "row", "", 0, false});
  check(!off.add(m, 1), "disabled: add() does nothing");
  check(!off.save(), "disabled: save() does nothing");
  checkEq(off.restore().size(), size_t(0), "disabled: restore() is empty");

  HistoryStore nowhere("", 10);
  check(!nowhere.enabled(), "disabled: no path is off");
  check(!nowhere.add(m, 1), "disabled: add() does nothing");
  check(!std::filesystem::exists("history.json"), "disabled: nothing was written");
}

void testCreatesItsDirectory() {
  // The default path is under $XDG_STATE_HOME, which does not exist on a first run.
  const std::string p = (gDir / "new" / "deep" / "history.json").string();
  HistoryStore store(p, 4);
  Message m;
  m.parts.emplace_back(dwm::Fragment{dwm::Fragment::Kind::Text, "row", "", 0, false});
  store.add(m, 1);
  check(store.save(), "directory: the parent is created and the file written");
  check(std::filesystem::exists(p), "directory: the file is where it was asked to be");
  checkEq(store.restore().size(), size_t(1), "directory: and it reads back");
  std::filesystem::remove_all(gDir / "new");
}

}  // namespace

int main() {
  gDir = std::filesystem::temp_directory_path() / "pw-live-danmaku-history-test";
  std::filesystem::remove_all(gDir);
  std::filesystem::create_directories(gDir);

  testRoundTrip();
  testKindsSurviveAsNames();
  testWindowBound();
  testLoadCutsToThisRunsWindow();
  testRestoreSeedsTheWindow();
  testEscapes();
  testLongFieldIsCut();
  testThrottle();
  testUnusableFiles();
  testAtomicReplace();
  testDisabledStore();
  testCreatesItsDirectory();

  std::filesystem::remove_all(gDir);
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("history: all checks passed\n");
  return 0;
}

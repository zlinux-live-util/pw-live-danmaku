#pragma once
// The chat history that survives a restart.
//
// The panel is a window, not a log: it holds the last few rows and forgets everything older, so a
// stream that is switched on while the room is already talking opens on an empty column even
// though the room is busy. This module closes that gap by keeping the same bounded window on disk,
// so the next start paints the rows it already had -- above a rule, because yesterday's messages
// and today's must not read as one conversation.
//
// The decisions that shaped it:
//
//   * It holds Messages, which is the boundary the rest of the project is arranged around, so
//     nothing here knows what a room is. A file written by one site is readable by another, and
//     the file is JSON rather than a binary dump so a person can read it when it goes wrong.
//   * Writes are throttled by the *caller*, not here: add() does no I/O at all and only reports
//     when a write has become due. The site thread appends under the lock of its pending queue, and
//     a file write under that lock would stall the websocket read loop behind it.
//   * Saving goes to a temporary file in the same directory and is then renamed into place. A crash
//     part-way through a write therefore leaves the previous file intact rather than a truncated
//     one -- and a truncated file is a file that parses as nothing.
//   * A file that is missing, unreadable or malformed yields an empty history and no error. History
//     is a nicety, and an overlay that refuses to start because last night's file went bad is worse
//     than one that starts with an empty column.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "message.hpp"

namespace dwm {

class HistoryStore {
 public:
  /** A write is due when either bound is reached: the row count so a burst costs one write rather
   *  than one per message, and the age so a quiet room still gets its rows onto disk instead of
   *  sitting in memory until something bigger happens. */
  static constexpr size_t kFlushRows = 8;
  static constexpr int64_t kFlushMs = 2000;

  /** Longest single string kept in the file, and the largest file read back. Chat bodies are short;
   *  these bounds exist so one pathological message, or one hand-edited file, cannot make the store
   *  grow without limit or the load allocate without limit. A body that arrives longer than this is
   *  cut rather than dropped, since a truncated message still says what it was about. */
  static constexpr size_t kMaxFieldBytes = 4096;
  static constexpr size_t kMaxFileBytes = 4u << 20;  // 4 MiB, around forty thousand rows

  /** File format version. Written on save and checked on load, so a file from a future build is
   *  skipped rather than half-understood. */
  static constexpr int kFormat = 1;

  /** A store with an empty path or a zero capacity is off: add() and save() do nothing, load()
   *  returns nothing, and nothing is ever written. */
  HistoryStore(std::string path, size_t capacity)
      : path_(std::move(path)), capacity_(capacity), enabled_(capacity > 0 && !path_.empty()) {}

  bool enabled() const { return enabled_; }
  const std::string& path() const { return path_; }
  size_t capacity() const { return capacity_; }
  size_t size() const { return items_.size(); }
  /** Whether a message is held in memory but not yet on disk. */
  bool hasUnsaved() const { return unsaved_ > 0; }

  /** Which room this file belongs to. Written into the file and checked on load, so pointing two
   *  rooms at one history file does not interleave two conversations on the overlay.
   *
   *  An empty label means "do not care", which is also what a file without the field is read as: a
   *  hand-written file stays usable rather than being refused over a missing field. */
  void setRoom(std::string room) { room_ = std::move(room); }

  /** Appends one message, dropping the oldest when the window is full, and reports whether the
   *  caller should now call save(). No I/O happens here, so this is safe to call under a lock.
   *
   *  Ignoring the answer is not fatal -- the rows are still written by the next save(), the exit one
   *  included -- it only means the write happens later than it should. */
  bool add(const Message& m, int64_t nowMs);

  /** Writes the window to disk if anything has been added since the last write. Returns whether a
   *  file was actually written.
   *
   *  A failure disables the store and says so on stderr rather than being retried: an unwritable
   *  path is not going to become writable on its own, and a chat overlay that spams the log once
   *  per message is worse than one that quietly carries on without history. */
  bool save();

  /** Reads the previous run's rows and puts them at the front of this run's window, returning them
   *  so the caller can paint them. Empty on a first run, and empty on a file that cannot be used --
   *  both of which are ordinary, not failures.
   *
   *  Seeding rather than merely reading is what keeps the window the same size across a restart: a
   *  short run that saw four rows would otherwise leave a file of four rows, and every restart would
   *  shorten the history further until there was nothing left to show.
   *
   *  Nothing is marked unsaved. The file already says what these rows are, and rewriting it before
   *  a single new row has arrived would be a write with no reason behind it. */
  std::vector<Message> restore();

  /** $XDG_STATE_HOME/pw-live-danmaku/history.json, falling back to ~/.local/state when the XDG
   *  variable is unset or relative (the spec says a relative path must be ignored). Empty when
   *  neither it nor $HOME is set: with nothing to resolve against, off is the honest answer. */
  static std::string defaultPath();

 private:
  std::string path_;
  std::string room_;
  std::deque<Message> items_;
  size_t capacity_;
  bool enabled_ = false;
  size_t unsaved_ = 0;
  int64_t lastFlushMs_ = 0;
};

}  // namespace dwm

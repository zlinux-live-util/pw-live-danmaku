#pragma once
// The pinned layer: paid messages, held at the top of the panel instead of scrolling away with the
// chat.
//
// A paid message is not a chat line. It is addressed to the room rather than to the next reader, it
// is read once rather than scrolled past, and it has a dwell time the sender paid for. So it does not
// go into the scrolling list at all: the list would bury it under whatever arrived next, and it
// would have no way to leave on its own. Here each one is measured, drawn and retired on a clock of
// its own, and several can be up at once.
//
// What this is *not*: a second static surface. The chat panel caches its picture because its content
// is unbounded and mostly unchanging; this content is a handful of cards whose opacity changes every
// frame while they fade. Caching would buy nothing and would make per-notice alpha impossible, since
// a shared surface can only be composited as one thing. So it is painted straight into the frame,
// which is cheap precisely because the set is small and bounded.
//
// Tokens come from PanelTokens because both layers are drawn from the same stylesheet. Only the
// fields a card uses are read: padX, rowGap, the three card font sizes, and the outline pair.

#include <cstdint>
#include <string>
#include <vector>

#include <cairo/cairo.h>

#include "message.hpp"
#include "panel.hpp"
#include "text.hpp"

namespace dwm {

/** One paid message being held, with the clock that decides when it leaves. */
struct PinnedNotice {
  Message msg;
  double y = 0.0;         // top edge, from the top of the panel
  double h = 0.0;         // full height, padding included
  int64_t shownAtMs = 0;  // when it arrived
  int64_t dwellMs = 0;    // how long it stays at full opacity
  int64_t fadeMs = 0;     // how long the fade afterwards takes

  /** Opacity at nowMs: 1 until the dwell is up, then down to 0 across the fade. Clamped rather than
   *  allowed to go negative, because a negative alpha is an error to cairo, not a smaller number to
   *  draw with. */
  double alpha(int64_t nowMs) const;
  /** Whether the fade has run out and the notice can be dropped. */
  bool expired(int64_t nowMs) const { return nowMs - shownAtMs >= dwellMs + fadeMs; }
};

class PinnedLayer {
 public:
  explicit PinnedLayer(PanelTokens tokens) : tok_(std::move(tokens)) {}

  void resize(int width, int height);
  /** Adds paid messages, newest first in the stack. Anything that is not MsgKind::Paid is ignored,
   *  so the caller can hand over a whole batch without filtering it twice.
   *
   *  cr is only used to measure text into; nothing is drawn into it here. */
  void update(cairo_t* cr, const std::vector<Message>& fresh, int64_t nowMs);

  /** Draws every notice still alive, newest at the top. Called after the chat panel has been drawn,
   *  so the notices sit over it. */
  void render(cairo_t* cr, int64_t nowMs);

  void clear() {
    notices_.clear();
    contentH_ = 0.0;
    layoutW_ = 0.0;
  }

  bool empty() const { return notices_.empty(); }
  size_t size() const { return notices_.size(); }
  /** Total height the stack occupies, which can exceed the panel when several are up at once. */
  double contentHeight() const { return contentH_; }

  /** How long a paid message of this value stays up, in milliseconds.
   *
   *  The platform's price table, in whole currency units: the band a message falls in decides its
   *  dwell, so a larger payment is still on screen when a smaller one has gone. Anything below the
   *  smallest band gets that band's time rather than zero -- a notice that appeared and vanished
   *  inside one frame would look like a glitch, not like a feature. */
  static int64_t dwellFor(int64_t amountValue);

 private:
  /** One visual line of a notice, as measured. */
  struct Line {
    std::string text;
    double size = 0.0;
    /** pango's own height at this size, which for the body is every line of it together. */
    double height = 0.0;
    /** Whether this line is allowed to wrap. The body is; a name and an amount are not, and a name
     *  that wrapped would push the amount and the message down a line each. */
    bool wraps = false;
  };

  /** Lays a notice out and returns its height, padding included. The painted card and the height the
   *  stack reserves both come out of this one call, so they cannot disagree about how tall it is --
   *  which is what puts a card's last line outside its own background. */
  double layout(const Message& m, cairo_t* cr, double w, std::vector<Line>* out) const;
  /** Paints one notice at y, at full opacity. The caller has already applied the fade. */
  void paint(const Message& m, cairo_t* cr, double y, double w, double h) const;

  /** Re-measures and re-stacks from the top edge. Called when the set changes and when the width it
   *  was measured at no longer holds; a fade never calls it, because alpha moves nothing. */
  void restack(cairo_t* cr, double w);

  PanelTokens tok_;
  int width_ = 0;
  int height_ = 0;

  /** Newest first, so index 0 is the one against the top edge. */
  std::vector<PinnedNotice> notices_;
  double contentH_ = 0.0;
  /** Width the heights in notices_ were measured at, so a resize re-measures instead of stretching
   *  text that was fitted to the old width. 0 when nothing is up. */
  double layoutW_ = 0.0;

  // Own renderer rather than a borrowed one: TextRenderer holds a single PangoLayout that every
  // layout() call rebinds, and sharing it with the chat panel would mean measuring here and painting
  // there over each other's line lists. The existing code has already been bitten by that once.
  // Mutable for the same reason Panel's is: layout() is not const because it rebinds, and measuring is
  // logically a read.
  mutable pwvideo::TextRenderer text_;
};

}  // namespace dwm
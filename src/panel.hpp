#pragma once
// The chat panel: messages in, pixels out. Knows nothing about any platform.
//
// Design tokens mirror the reference stylesheet, one field per custom property, so the mapping can
// be checked against it without reading any code. What is *not* in the stylesheet but is needed
// here is the vertical rhythm between messages and the two card kinds; those are marked below.
//
// Two performance decisions shape the implementation:
//
//   * Text is static once laid out. Re-laying out the whole list on every incoming message costs a
//     pango pass per visible row, which at a busy message rate is the dominant cost. Instead the
//     newest message is the only thing that changes, so the accumulated picture is kept in a
//     static layer surface and each new message is written into it after shifting the existing
//     contents up by one row. That makes the work per message proportional to its own height
//     rather than to the panel.
//   * The static layer therefore holds everything except the message currently fading in. The fade
//     is drawn on top per frame; once it settles, that row is baked in and the per-frame work goes
//     back to a single copy.

#include <cstdint>
#include <string>
#include <vector>

#include <cairo/cairo.h>

#include "images.hpp"
#include "cairo_util.hpp"
#include "message.hpp"
#include "text.hpp"

namespace dwm {

/** One field per custom property in the reference stylesheet. */
struct PanelTokens {
  double avatar = 24.0;      // --avatar-size
  double avatarGap = 10.0;   // avatar margin-right
  double emote = 24.0;       // --emote-size; inline emotes are drawn at this box
  double fontUser = 20.0;    // --username-size
  double fontBody = 20.0;    // --text-content-size
  double fontCardName = 22.0;   // --paid-msg-line-1-size
  double fontCardAmount = 20.0; // --paid-msg-line-2-size
  double lineHeight = 1.2;   // --line-height, a multiple of the font size

  double padX = 20.0;        // padding-inline start
  double padRight = 4.0;     // padding-inline end
  double rowGap = 4.0;       // margin between messages; the stylesheet gives cards 4px, and plain
                             // lines are spaced the same so the column reads as one rhythm

  // The left colour bar: position absolute at left 8px, inset 4px top and bottom, 2px wide,
  // radius 2px.
  double barX = 8.0;
  double barWidth = 2.0;
  double barInset = 4.0;

  double outline = 1.5;      // text-shadow 0 0 2px, expressed as the stroke width we use
  double animMs = 200.0;     // animation duration
  double animShift = -16.0;  // the translate the animation starts from

  // --text-msg-bg-color, used as the bar colour
  uint32_t barNormal = 0xFFFFFF;
  uint32_t barMember = 0x0F9D58;
  uint32_t barModerator = 0x5E84F1;
  uint32_t barOwner = 0xFFD600;

  // --username-color, per author type
  uint32_t nameNormal = 0xEAEAEA;
  uint32_t nameMember = 0x0F9D58;
  uint32_t nameModerator = 0x5E84F1;
  uint32_t nameOwner = 0xFFD600;

  uint32_t body = 0xFFFFFF;         // --text-content-color
  uint32_t outlineColor = 0x000000; // --outline-color

  // --membership-msg-bg-color is opaque; the paid-message card uses the same family at 0.55 so a
  // stack of cards does not become an opaque slab. Bilibili's paid card is cyan, which no
  // token in the stylesheet provides, so it is set here rather than in a site implementation.
  uint32_t cardMembership = 0x0F9D58;
  double cardPaidAlpha = 0.55;

  std::string font;
};

/** Geometry of one laid-out message, in content space.
 *
 *  avatarUrl is carried here rather than baked into the picture because avatars are drawn every
 *  frame instead. The reason is correctness, not cost: a face that arrives after its row has been
 *  laid out would otherwise stay a placeholder disc forever, because nothing ever repaints a
 *  settled row. Drawing the little discs live means a late arrival shows up on the next frame. */
struct Row {
  size_t index = 0;   // running total, for identity only
  double y = 0.0;     // top edge, in content space measured from the top of the content
  double h = 0.0;     // full height including the gap below
  bool card = false;
  std::string avatarUrl;
};

class Panel {
 public:
  explicit Panel(PanelTokens tokens);

  void resize(int width, int height);
  /** The image stores are borrowed, not owned, and are read only through lookup(). Avatars and emotes
   *  are kept apart because they are fetched at different sizes and have very different reuse: one
   *  face per viewer versus the same handful of emotes all evening. */
  void setImageStore(ImageStore* store) { avatars_ = store; }
  void setEmoteStore(ImageStore* store) { emotes_ = store; }

  /** Appends messages that arrived since the last call, and repaints only what changed. nowMs is the
   *  clock the entrance animation is timed against. Returns true when anything was redrawn.
   *
   *  Only the *new* messages are passed in, and only the newest one is held out for its fade. The
   *  panel deliberately does not retain the messages it has already drawn: their pixels are in the
   *  static layer, and keeping the bodies would mean either re-measuring them to repaint or
   *  copying them across a lock on every frame. */
  bool update(const std::vector<Message>& fresh, int64_t nowMs);

  /** Draws the current frame. Only the fading row is painted here when it is mid-animation. */
  void render(cairo_t* cr, int64_t nowMs);

  bool empty() const { return rows_.empty(); }
  size_t rowCount() const { return rows_.size(); }
  /** Total messages laid out so far, including those whose rows have since scrolled out. */
  size_t count() const { return count_; }
  /** Total content height, which can exceed the panel; the view shows its tail. */
  double contentHeight() const { return contentH_; }
  /** Whether a message is currently fading in. */
  bool animating() const { return animating_; }
  /** Drops all history and clears the picture. */
  void clear();
  /** Paints a whole list at once, anchored to the bottom. The offline --demo path and any future
   *  "replay this file" path go through here: when every message is known up front there is nothing
   *  to gain from the incremental append, and it avoids the entrance animation, which is meant for
   *  a message that just arrived rather than for history. */
  void rebuildLayer(const std::vector<Message>& msgs);

 private:
  double lineAdvance() const {
    // The stylesheet's line-height is 1.2em, which is 24px at a 20px font -- exactly the emote box,
    // leaving no slack. An emote sits on the baseline and occupies the same 24px, so it ends up
    // touching the row above and pushing into the row below. Give the line whatever it needs to
    // hold both, with a little breathing room.
    return std::max(tok_.fontBody * tok_.lineHeight, tok_.emote + 2.0);
  }

  double measureRow(const Message& m, cairo_t* cr, bool* isCard) const;
  double paintRow(cairo_t* cr, const Message& m, double x, double y, double w) const;
  /** Moves the accumulated picture up by h and clears the strip that opens at the bottom, without
   *  painting into it. The newest message reserves its slot through this, so the rows below it move
   *  up on the frame it arrives rather than when its animation ends. */
  void reserveBand(double h);
  /** reserveBand followed by painting m into the strip it opened, for a row that is already settled
   *  and needs no animation. Row bookkeeping belongs to the caller. */
  void shiftAndPaint(const Message& m, double h);
  /** Fills the reserved strip with the row that has finished fading in. No shift: the strip was
   *  reserved on arrival, and shifting again would move the list a second time. */
  void bakeAnimating();
  /** Drops rows that have scrolled out of the panel, so the vector cannot grow without bound. */
  void prune();
  void drawAvatar(cairo_t* cr, const std::string& url, double cx, double cy, double d) const;
  /** Paints the avatars for every row currently on screen. Separate from paintRow so that a face
   *  arriving after its row was laid out appears without the row having to be redrawn. */
  void drawAvatars(cairo_t* cr) const;
/** Draws one inline emote into a box whose top edge is boxTop, or the token as text when the picture
 *  has not arrived yet or the platform gave no URL for it.
 *
 *  ascent/descent are the body font's metrics in pixels, needed to centre the fallback text in the
 *  box; the caller measures them once rather than per emote. */
void drawEmote(cairo_t* cr, const Fragment& f, double x, double boxTop, double ascent,
               double descent) const;
  pwvideo::Rgba rgb(uint32_t c, double alpha = 1.0) const;
  uint32_t barColor(UserType t) const;
  uint32_t nameColor(UserType t) const;

  PanelTokens tok_;
  ImageStore* avatars_ = nullptr;
  ImageStore* emotes_ = nullptr;

  int width_ = 0;
  int height_ = 0;
  pwvideo::SurfacePtr layer_;  // ARGB32, width_ x height_
  // Bound once to the layer, so measuring and painting the accumulated picture needs no context
  // argument threaded through, and so the frame context is only ever touched by render().
  pwvideo::ContextPtr layerCr_;

  // Geometry of what is on screen. Rows are pruned as they scroll out, so this stays small
  // however long the overlay runs; count_ is the running total.
  std::vector<Row> rows_;
  size_t count_ = 0;
  double contentH_ = 0.0;

  // The newest message, held out of the layer while it fades in.
  Message anim_;
  bool animating_ = false;
  int64_t animStart_ = 0;
  double animRowH_ = 0.0;

  // mutable because the const measurement and painting paths use it as scratch space: it owns a
  // single PangoLayout that is re-bound to whatever context is passed in, which is exactly the
  // reuse the submodule intends, and layout() is not const for that reason.
  mutable pwvideo::TextRenderer text_;
};

}  // namespace dwm
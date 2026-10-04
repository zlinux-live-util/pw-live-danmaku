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
#include <map>
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
  double avatar = 24.0;      // fallback avatar box, used only before the first measurement; the box
                             // itself is one line of body text as pango lays it out
  double avatarGap = 10.0;   // avatar margin-right
  double emote = 28.0;       // box an inline emote is drawn into. Tracks --font-size (main.cpp keeps
                             // them equal), because a picture in the text flow that is smaller than
                             // the text beside it reads as a mistake rather than as an emote
  double fontUser = 28.0;    // --username-size
  double fontBody = 28.0;    // --text-content-size
  double fontCardName = 30.0;   // --paid-msg-line-1-size
  double fontCardAmount = 28.0; // --paid-msg-line-2-size
  double lineHeight = 1.2;   // --line-height, a multiple of the font size

  double padX = 20.0;        // padding-inline start
  double padRight = 4.0;     // padding-inline end
  double nameBodyGap = 6.0;  // space between "name:" and the body on the line they share
  double rowGap = 4.0;       // margin between messages; the stylesheet gives cards 4px, and plain
                             // lines are spaced the same so the column reads as one rhythm

  // The left colour bar: position absolute flush against the panel edge (left 0), square corners,
  // and as tall as the text block of its row -- barInset trims that height from each end if wanted.
  double barX = 0.0;
  double barWidth = 8.0;
  double barInset = 0.0;

  double outline = 1.5;      // text-shadow 0 0 2px, expressed as the stroke width we use
  double animMs = 200.0;     // animation duration
  double animShift = -16.0;  // the translate the animation starts from

  // --text-msg-bg-color, used as the bar colour
  uint32_t barNormal = 0xFFFFFF;
  uint32_t barMember = 0x0F9D58;
  uint32_t barModerator = 0x5E84F1;
  uint32_t barOwner = 0xFFD600;
  // A gift is not one of the four author types, so its bar has no --username-color to come from.
  // Bilibili's gift accent is a warm gold; the reference stylesheet has no token for it, so it is
  // set here rather than in the site implementation, the same way the paid card's cyan is.
  uint32_t barGift = 0xF5C542;

  /** How far down the two ambient kinds -- entry notices and likes -- are drawn. They have the same
   *  shape as a chat row and the same outline, so without this they compete with what was actually
   *  typed for the same attention. Their arrival rates differ from chat's by more than an order of
   *  magnitude, so the visual weight has to differ by more than a shade. */
  double dimAlpha = 0.5;

  // --username-color, per author type. Ordinary names sit well below the body white: the name
  // repeats on every row, so at body brightness the column reads as solid text.
  uint32_t nameNormal = 0x9AA0A6;
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
  /** Demo-only: faces drawn in place of fetched ones, keyed by the URL the demo message carries.
   *  Empty in a live run, where the image stores are the only source. Consulted after the store
   *  misses, so an installed demo table cannot shadow a face that really was fetched. */
  void setDemoFaces(std::map<std::string, pwvideo::SurfacePtr> faces) {
    demoFaces_ = std::move(faces);
  }

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
  /** Side of the square the face is drawn in: one line of body text, as pango lays it out. Measured
   *  once per resize, since it is a property of the font and the size, not of any message. */
  double avatarBox() const;
  /** Paints a whole list at once, anchored to the bottom. The offline --demo path and any future
   *  "replay this file" path go through here: when every message is known up front there is nothing
   *  to gain from the incremental append, and it avoids the entrance animation, which is meant for
   *  a message that just arrived rather than for history. */
  void rebuildLayer(const std::vector<Message>& msgs);

 private:
  /** Distance from one line's baseline to the next: the stylesheet's line-height, but never less than
   *  an emote box plus a little air.
   *
   *  An inline emote sits on the baseline and reaches up by its own height, so a line advance of
   *  exactly the emote size would let it touch the line above and push into the line below. Both
   *  measureRow() and paintRow() take their advance from here, so the height a row is allocated and
   *  the height it is drawn at cannot drift apart. */
  double lineAdvance() const {
    return std::max(tok_.fontBody * tok_.lineHeight, tok_.emote + 2.0);
  }

  /** One line of a card, laid out and measured. */
  struct CardLine {
    std::string text;
    double size = 0.0;
    /** pango's own height for a line of this text at this size. Not size*lineHeight: a CJK face
     *  carries ascent+descent well past 1.0em, so the arithmetic figure came out shorter than the
     *  glyphs it was supposed to hold -- which is how a card's last line ended up outside it. */
    double height = 0.0;
  };

  /** The lines a card draws, each measured. The row height and the background behind the text both
   *  come out of this one call, so they cannot disagree about how tall the card is. */
  std::vector<CardLine> cardLines(const Message& m, cairo_t* cr, double w) const;
  /** Height a card occupies in the list: its text plus padding, plus the gap below it. */
  double cardHeight(const Message& m, cairo_t* cr, double w) const;
  double measureRow(const Message& m, cairo_t* cr, bool* isCard) const;
  /** Height pango gives one line of text at this size. Not sizePx*lineHeight: a CJK face carries
   *  ascent+descent well past 1.0em, so the arithmetic figure is shorter than the glyphs it would
   *  have to hold. */
  double measureLineHeight(cairo_t* cr, double sizePx) const;
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
  /** Draws one inline emote into a box on the current line, or the token as text when the picture
   *  has not arrived yet or the platform gave no URL for it. Returns how far the line advanced.
   *
   *  lineTop is the top of the line's box, the same value every text run on the line is drawn at.
   *  baseline is how far below that top pango puts the baseline, which depends on the font that
   *  answered and is measured rather than derived; the emote's bottom edge is aligned to the
   *  baseline, the way vertical-align: baseline aligns an inline image. alpha is the row's own
   *  alpha, so a picture on a dimmed row dims with the words beside it.
   *
   *  The advance is the picture's box when there is a picture, and the measured width of the token
   *  when there is not -- a token can be much wider than the box, and returning the box either way
   *  drew whatever came next on top of it. */
  double drawEmote(cairo_t* cr, const Fragment& f, double x, double lineTop, double baseline,
                   double alpha) const;
  pwvideo::Rgba rgb(uint32_t c, double alpha = 1.0) const;
  uint32_t barColor(UserType t) const;
  uint32_t nameColor(UserType t) const;

  /** Whether a row leaves room for an avatar.
   *
   *  Entry notices do not: they arrive several times a second and every one is a different viewer,
   *  so the column would become a strip of single-use faces that the avatar cache can never keep,
   *  and most of them would be the site's default picture besides. */
  static bool hasAvatarBox(MsgKind k) { return k != MsgKind::Entry; }

  /** Whether a row is drawn with the coloured bar at the panel edge. The two ambient kinds are not:
   *  the bar is the loudest thing in a row, and these rows exist to sit under the chat rather than
   *  in it. A gift keeps one, because it is something a viewer paid for. */
  static bool hasBar(MsgKind k) { return k != MsgKind::Entry && k != MsgKind::Like; }

  /** Alpha the name and body of this kind are drawn at: dimAlpha for the ambient kinds, 1.0 for the
   *  rest. Applied to the name as well as the body, or a dimmed row would still show a full-bright
   *  nickname column, and to the text halo as well, or a dimmed row would still show a full-strength
   *  black edge around half-bright glyphs. */
  double rowAlpha(MsgKind k) const;

  /** Left edge of the text in a row: past the avatar box when the row has one, at the padding when
   *  it does not. measureRow() and paintRow() both go through here, because a disagreement between
   *  them about where the text starts is exactly how a row ends up allocated short and overwritten. */
  double textX(MsgKind k) const;

  /** The bar colour for a row: the gift accent for a gift, the author type's colour otherwise. */
  uint32_t barColor(const Message& m) const;

  PanelTokens tok_;
  ImageStore* avatars_ = nullptr;
  ImageStore* emotes_ = nullptr;
  // Offline demo pictures, keyed by the URL the demo message carries. Empty otherwise.
  std::map<std::string, pwvideo::SurfacePtr> demoFaces_;
  // Measured in resize() and read by the layout and by the per-frame avatar pass alike, so the
  // avatar cannot drift from the text it belongs to when the font size changes.
  double avatarBox_ = 0.0;

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
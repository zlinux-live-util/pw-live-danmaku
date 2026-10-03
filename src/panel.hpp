#pragma once
// The chat panel: messages in, pixels out. Knows nothing about any platform.
//
// Design tokens mirror the reference stylesheet, one field per custom property, so the mapping can
// be checked against it without reading code. What is *not* in the stylesheet but is needed here is
// the vertical rhythm between messages and the two card kinds; those are marked below.
//
// ---------------------------------------------------------------------------
// Two invariants this file depends on, both learned the hard way:
//
//  1. TextRenderer::fill() takes a *baseline*, not a line top. It does a move_to and then
//     pango_cairo_show_layout, whose origin is the first line's baseline. Everything here speaks
//     line tops, and baselineOf() is the single place that converts. Mixing the two silently
//     puts every row's text above its own row box.
//
//  2. TextRenderer owns ONE PangoLayout and rebinds it on every layout() call. A PangoLayout* held
//     across two calls points at whatever the second call laid out -- that is how a username once
//     rendered as the metrics probe's "Ag", and how wrapped lines lost their continuations. So no
//     layout pointer escapes a helper here: callers work from byte spans and re-lay-out per run.
//
// Performance: text is static once laid out, so the accumulated picture lives in an ARGB32 static
// layer, and each arriving message is painted into the strip that opens at the bottom after a
// self-overlap blit -- work proportional to the new row, not to the panel. Avatars, emotes and the
// row mid-animation are drawn per frame: they are small, and more importantly a picture that
// arrives after its row was laid out must not be frozen out as a placeholder.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <cairo/cairo.h>

#include "images.hpp"
#include "message.hpp"
#include "text.hpp"

namespace dwm {

/** One field per custom property in the reference stylesheet. */
struct PanelTokens {
  double avatar = 24.0;      // --avatar-size
  double avatarGap = 10.0;   // avatar margin-right
  double emote = 24.0;       // --emote-size
  double fontUser = 20.0;    // --username-size
  double fontBody = 20.0;    // --text-content-size
  double fontCardName = 22.0;   // --paid-msg-line-1-size
  double fontCardAmount = 20.0; // --paid-msg-line-2-size
  double lineHeight = 1.2;   // --line-height, a multiple of the font size

  double padX = 20.0;        // padding-inline start
  double padRight = 4.0;     // padding-inline end
  double rowGap = 4.0;       // margin between messages

  // The left colour bar: absolute at left 8px, inset 4px top and bottom, 2px wide, radius 2px.
  double barX = 8.0;
  double barWidth = 2.0;
  double barInset = 4.0;

  double outline = 1.5;      // text-shadow 0 0 2px, as a stroke width
  double animMs = 200.0;     // animation duration
  double animShift = -16.0;  // the translate the animation starts from

  uint32_t barNormal = 0xFFFFFF;    // --text-msg-bg-color, used as the bar colour
  uint32_t barMember = 0x0F9D58;
  uint32_t barModerator = 0x5E84F1;
  uint32_t barOwner = 0xFFD600;

  uint32_t nameNormal = 0xEAEAEA;  // --username-color per author type
  uint32_t nameMember = 0x0F9D58;
  uint32_t nameModerator = 0x5E84F1;
  uint32_t nameOwner = 0xFFD600;

  uint32_t body = 0xFFFFFF;         // --text-content-color
  uint32_t outlineColor = 0x000000; // --outline-color

  // --membership-msg-bg-color is opaque; the paid-message card uses the same family lighter so a
  // stack of cards does not become a slab. Bilibili's paid-card cyan is not in the stylesheet, so
  // it lives here rather than in a site implementation.
  uint32_t cardMembership = 0x0F9D58;
  double cardPaidAlpha = 0.55;

  std::string font;
};

/** An emote positioned inside a row's text flow, stored relative to the row's own top. Kept out of
 *  the baked picture so a picture that arrives late still appears. */
struct EmoteSlot {
  double x = 0.0;
  double y = 0.0;
  std::string token;
  std::string url;
};

struct Row {
  size_t seq = 0;      // running total, for identity only
  double y = 0.0;      // top edge in content space
  double h = 0.0;      // full height including the gap below
  bool card = false;
  std::string avatarUrl;
  std::vector<EmoteSlot> emotes;
};

class Panel {
 public:
  explicit Panel(PanelTokens tokens);

  void resize(int width, int height);
  /** Both image stores are borrowed. Avatars and emotes are kept apart: faces are one per viewer,
   *  emotes are a small set everyone reuses. */
  void setImageStore(ImageStore* store) { avatars_ = store; }
  void setEmoteStore(ImageStore* store) { emotes_ = store; }

  /** Appends messages that arrived since the last call. nowMs clocks the entrance animation. */
  bool update(const std::vector<Message>& fresh, int64_t nowMs);
  /** Draws the current frame. */
  void render(cairo_t* cr, int64_t nowMs);

  bool empty() const { return rows_.empty(); }
  size_t rowCount() const { return rows_.size(); }
  size_t count() const { return count_; }
  double contentHeight() const { return contentH_; }
  bool animating() const { return animating_; }
  void clear();
  /** Paints a whole list at once, anchored to the bottom: the --demo path and any future replay
   *  path. When every message is known up front there is nothing to gain from the incremental
   *  append, and it avoids the entrance animation, which is for an arriving message. */
  void rebuildLayer(const std::vector<Message>& msgs);

 private:
  // --- text plumbing; see the invariants at the top of this file --------------------
  /** Measures the body font once and caches it. Runs before any other layout call, because the
   *  probe rebinds TextRenderer's single layout. */
  void probeMetrics(cairo_t* cr);
  /** The one place line top becomes baseline. */
  double baselineOf(double lineTop) const { return lineTop + ascent_; }
  /** Distance between successive line tops. Never below the font's own line box: the stylesheet's
   *  1.2em is 24 px at a 20 px font, while this font's natural box is 30, so honouring 1.2 exactly
   *  would overlap lines -- and an emote is 24 px tall and needs the room. */
  double lineAdvance() const;
  /** Width of a run, without drawing it. */
  double measureRun(cairo_t* cr, const std::string& s, const pwvideo::LabelSpec& spec) const;
  /** Draws one run with its top at lineTop; returns its width. The layout is local to this call. */
  double drawRun(cairo_t* cr, const std::string& s, const pwvideo::LabelSpec& spec, double x,
                 double lineTop, const pwvideo::Rgba& col);
  /** Byte span of each visual line of `body`. The layout does not escape: the caller gets plain
   *  data and re-lays-out per run when it draws. */
  std::vector<std::pair<size_t, size_t>> wrapSpans(cairo_t* cr, const std::string& body,
                                                   const pwvideo::LabelSpec& spec) const;

  // --- layout ------------------------------------------------------------------
  pwvideo::LabelSpec specFor(double sizePx, bool wrap, double widthPx, int maxLines) const;
  double measureRow(const Message& m, cairo_t* cr, bool* isCard);
  double cardHeight(cairo_t* cr, const Message& m, int* bodyLines);
  double paintRow(cairo_t* cr, const Message& m, double x, double y, double w,
                  std::vector<EmoteSlot>* slots);
  double cardInnerWidth() const { return width_ - 2.0 * tok_.padX; }

  // --- painting ----------------------------------------------------------------
  void shiftAndPaint(const Message& m, double h);
  void reserveBand(double h);
  void bakeAnimating();
  void prune();
  void drawAvatar(cairo_t* cr, const std::string& url, double cx, double cy, double d) const;
  void drawAvatars(cairo_t* cr) const;
  void drawEmote(cairo_t* cr, const EmoteSlot& slot) const;
  void drawEmotes(cairo_t* cr) const;

  pwvideo::Rgba rgb(uint32_t c, double alpha = 1.0) const;
  uint32_t barColor(UserType t) const;
  uint32_t nameColor(UserType t) const;

  PanelTokens tok_;
  ImageStore* avatars_ = nullptr;
  ImageStore* emotes_ = nullptr;

  int width_ = 0;
  int height_ = 0;
  pwvideo::SurfacePtr layer_;   // ARGB32, width_ x height_
  pwvideo::ContextPtr layerCr_; // bound once to the layer

  std::vector<Row> rows_;
  size_t count_ = 0;
  double contentH_ = 0.0;

  Message anim_;
  bool animating_ = false;
  int64_t animStart_ = 0;
  double animRowH_ = 0.0;

  // Font metrics, in pixels, measured once. Everything vertical derives from these.
  double ascent_ = 0.0;
  double descent_ = 0.0;
  bool metricsDone_ = false;

  // Scratch, so neither the frame path nor the row path allocates per row.
  mutable std::vector<EmoteSlot> slotScratch_;
  mutable std::vector<EmoteSlot> animSlots_;

  // mutable because the const measurement paths use it as scratch: it owns a single PangoLayout
  // that is re-bound to whatever context is passed in, and layout() is not const for that reason.
  mutable pwvideo::TextRenderer text_;
};

}  // namespace dwm
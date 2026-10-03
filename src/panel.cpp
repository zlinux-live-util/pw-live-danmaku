// The chat panel. See panel.hpp for the two invariants this file is built around: fill() takes a
// baseline, and TextRenderer's single PangoLayout must never be held across a layout() call.
#include "panel.hpp"

#include <algorithm>
#include <cstring>

#include <pango/pangocairo.h>

namespace dwm {
namespace {

constexpr double kPi = 3.14159265358979323846;

/** U+FFFC OBJECT REPLACEMENT CHARACTER, as UTF-8. Stands in for an emote inside the string handed
 *  to pango. */
constexpr const char* kEmoteChar = "\xEF\xBF\xBC";
constexpr size_t kEmoteCharLen = 3;

/** A card's body wraps; its other two lines are single-line by construction. */
constexpr int kCardBodyMaxLines = 4;

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/** A message body prepared for layout: text runs joined, every emote collapsed to one
 *  object-replacement character, plus where each placeholder sits and which fragment it is.
 *
 *  Substituting before layout means pango's own line breaking decides where the emotes land.
 *  Wrapping the fragments by hand would mean reimplementing break opportunities for mixed CJK and
 *  latin text, which is the part that is hard to get right. The placeholder is never drawn. */
struct InlineBody {
  std::string text;
  std::vector<std::pair<size_t, size_t>> emotes;  // (byte offset, index into parts)
};

InlineBody buildBody(const Message& m) {
  InlineBody b;
  for (size_t i = 0; i < m.parts.size(); ++i) {
    const Fragment& f = m.parts[i];
    if (f.kind == Fragment::Kind::Text) {
      b.text += f.text;
    } else {
      b.emotes.emplace_back(b.text.size(), i);
      b.text += kEmoteChar;
    }
  }
  return b;
}

}  // namespace

Panel::Panel(PanelTokens tokens) : tok_(std::move(tokens)) {}

void Panel::resize(int width, int height) {
  if (width == width_ && height == height_) return;
  width_ = std::max(1, width);
  height_ = std::max(1, height);
  // The deleter is explicit, as the submodule's CairoFrame does: cairo_surface_t is opaque, so a
  // shared_ptr that ever default-deleted one would need its size.
  layer_.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width_, height_),
               pwvideo::CairoSurfaceDeleter{});
  layerCr_ = pwvideo::ContextPtr(cairo_create(layer_.get()));
  metricsDone_ = false;
  clear();
}

void Panel::clear() {
  rows_.clear();
  count_ = 0;
  contentH_ = 0.0;
  animating_ = false;
  animRowH_ = 0.0;
  anim_ = Message();
  slotScratch_.clear();
  animSlots_.clear();
  if (!layer_ || !layerCr_) return;
  cairo_t* lc = layerCr_.get();
  cairo_set_operator(lc, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(lc, 0, 0, 0, 0);
  cairo_paint(lc);
  cairo_set_operator(lc, CAIRO_OPERATOR_OVER);
}

/* ---------------------------------------------------------------- text plumbing */

void Panel::probeMetrics(cairo_t* cr) {
  if (metricsDone_ || !cr) return;
  pwvideo::LabelSpec spec;
  spec.family = tok_.font;
  spec.bold = true;
  spec.sizePx = tok_.fontBody;
  spec.widthPx = 0.0;  // natural width: the probe must stay a single line
  spec.maxLines = 1;
  PangoLayout* l = text_.layout(cr, "Ag", spec);
  int pw = 0, ph = 0;
  pango_layout_get_pixel_size(l, &pw, &ph);
  const int asc = pango_layout_get_baseline(l) / PANGO_SCALE;
  if (asc > 0 && ph > asc) {
    ascent_ = asc;
    descent_ = ph - asc;
  } else {
    ascent_ = tok_.fontBody * 0.8;
    descent_ = tok_.fontBody * 0.2;
  }
  metricsDone_ = true;
}

double Panel::lineAdvance() const {
  return std::max(tok_.fontBody * tok_.lineHeight, ascent_ + descent_);
}

pwvideo::LabelSpec Panel::specFor(double sizePx, bool wrap, double widthPx, int maxLines) const {
  pwvideo::LabelSpec s;
  s.family = tok_.font;
  s.center = false;
  s.bold = true;
  s.sizePx = sizePx;
  s.widthPx = wrap ? widthPx : 0.0;
  s.maxLines = maxLines;
  return s;
}

double Panel::measureRun(cairo_t* cr, const std::string& s,
                         const pwvideo::LabelSpec& spec) const {
  if (s.empty()) return 0.0;
  PangoLayout* l = text_.layout(cr, s, spec);
  int w = 0, h = 0;
  pango_layout_get_pixel_size(l, &w, &h);
  return w;
}

double Panel::drawRun(cairo_t* cr, const std::string& s, const pwvideo::LabelSpec& spec, double x,
                      double lineTop, const pwvideo::Rgba& col) {
  if (s.empty()) return 0.0;
  // Lay out, draw, measure -- all inside this call. The layout pointer dies here, which is the
  // whole point: callers must never be able to hold one across another layout() call.
  PangoLayout* l = text_.layout(cr, s, spec);
  const double baseline = baselineOf(lineTop);
  const pwvideo::Rgba outline = rgb(tok_.outlineColor, 0.85);
  pwvideo::TextRenderer::outline(cr, l, x, baseline, tok_.outline, outline);
  pwvideo::TextRenderer::fill(cr, l, x, baseline, col);
  int w = 0, h = 0;
  pango_layout_get_pixel_size(l, &w, &h);
  return w;
}

std::vector<std::pair<size_t, size_t>> Panel::wrapSpans(cairo_t* cr, const std::string& body,
                                                       const pwvideo::LabelSpec& spec) const {
  std::vector<std::pair<size_t, size_t>> spans;
  if (body.empty()) return spans;
  PangoLayout* l = text_.layout(cr, body, spec);
  const int n = std::max(1, pango_layout_get_line_count(l));
  spans.reserve(static_cast<size_t>(std::min(n, spec.maxLines > 0 ? spec.maxLines : n)));
  for (int i = 0; i < n; ++i) {
    if (spec.maxLines > 0 && i >= spec.maxLines) break;
    PangoLayoutLine* pl = pango_layout_get_line_readonly(l, i);
    if (pl) spans.emplace_back(static_cast<size_t>(pl->start_index), static_cast<size_t>(pl->length));
  }
  return spans;
}

/* ---------------------------------------------------------------- measurement */

double Panel::cardHeight(cairo_t* cr, const Message& m, int* bodyLines) {
  double lines = 1.0;                                     // the name
  if (!m.amount.empty()) lines += 1.0;                    // the price
  int n = 0;
  if (!m.parts.empty()) {
    const pwvideo::LabelSpec bs = specFor(tok_.fontBody, true, cardInnerWidth(), kCardBodyMaxLines);
    n = static_cast<int>(wrapSpans(cr, m.plainText(), bs).size());
    lines += std::max(1, n);
  }
  if (bodyLines) *bodyLines = n;
  return tok_.rowGap * 2.0 + lines * lineAdvance();
}

double Panel::measureRow(const Message& m, cairo_t* cr, bool* isCard) {
  probeMetrics(cr);
  const bool card = m.kind != MsgKind::Text;
  if (isCard) *isCard = card;
  if (card) return cardHeight(cr, m, nullptr);

  // The name shares the first line with the body, so the body wraps at the reduced width. Measuring
  // it any wider here and narrower in paintRow is how a row ends up a line short and loses its
  // continuation to whatever is drawn next.
  const pwvideo::LabelSpec ns = specFor(tok_.fontUser, false, 0.0, 1);
  const double nameW = measureRun(cr, m.user + ":", ns);

  const double textX = tok_.padX + tok_.avatar + tok_.avatarGap;
  const double avail = static_cast<double>(width_) - textX - tok_.padRight;
  const pwvideo::LabelSpec bs = specFor(tok_.fontBody, true, std::max(20.0, avail - nameW), 8);
  const size_t lines = std::max<size_t>(1, wrapSpans(cr, buildBody(m).text, bs).size());

  return std::max(static_cast<double>(lines) * lineAdvance(), tok_.avatar) + tok_.rowGap;
}

/* ---------------------------------------------------------------- painting */

pwvideo::Rgba Panel::rgb(uint32_t c, double alpha) const {
  return pwvideo::Rgba{((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0,
                       alpha};
}

uint32_t Panel::barColor(UserType t) const {
  switch (t) {
    case UserType::Owner: return tok_.barOwner;
    case UserType::Moderator: return tok_.barModerator;
    case UserType::Member: return tok_.barMember;
    default: return tok_.barNormal;
  }
}

uint32_t Panel::nameColor(UserType t) const {
  switch (t) {
    case UserType::Owner: return tok_.nameOwner;
    case UserType::Moderator: return tok_.nameModerator;
    case UserType::Member: return tok_.nameMember;
    default: return tok_.nameNormal;
  }
}

void Panel::drawAvatar(cairo_t* cr, const std::string& url, double cx, double cy, double d) const {
  const double r = d / 2.0;
  cairo_save(cr);
  cairo_arc(cr, cx, cy, r, 0.0, 2.0 * kPi);
  cairo_clip(cr);

  pwvideo::SurfacePtr surf;
  if (avatars_) surf = avatars_->lookup(url);
  if (surf) {
    // Scale the picture into the box rather than assuming it is already the right size: cairo
    // anchors a source surface at its top-left corner, so a larger surface clipped to this circle
    // would show one corner of the face instead of the whole thing.
    const int sw = cairo_image_surface_get_width(surf.get());
    const int sh = cairo_image_surface_get_height(surf.get());
    if (sw > 0 && sh > 0) {
      cairo_translate(cr, cx - r, cy - r);
      cairo_scale(cr, d / static_cast<double>(sw), d / static_cast<double>(sh));
      cairo_set_source_surface(cr, surf.get(), 0.0, 0.0);
    } else {
      cairo_set_source_rgba(cr, 0.55, 0.58, 0.62, 1.0);
    }
  } else {
    cairo_set_source_rgba(cr, 0.55, 0.58, 0.62, 1.0);
  }
  cairo_paint(cr);
  cairo_restore(cr);
}

void Panel::drawEmote(cairo_t* cr, const EmoteSlot& slot) const {
  pwvideo::SurfacePtr surf;
  if (emotes_ && !slot.url.empty()) surf = emotes_->lookup(slot.url);

  if (!surf) {
    // Not fetched yet, or the platform advertised a token with no picture. The message still has to
    // read, so the token is drawn as text in the place the picture would be.
    const pwvideo::LabelSpec ls = specFor(tok_.fontBody, false, 0.0, 1);
    const double baseline = slot.y + (tok_.emote - (ascent_ + descent_)) / 2.0 + ascent_;
    PangoLayout* l = text_.layout(cr, slot.token, ls);
    pwvideo::TextRenderer::outline(cr, l, slot.x, baseline, tok_.outline, rgb(tok_.outlineColor, 0.85));
    pwvideo::TextRenderer::fill(cr, l, slot.x, baseline, rgb(tok_.body));
    return;
  }

  cairo_save(cr);
  pwvideo::roundedRect(cr, slot.x, slot.y, tok_.emote, tok_.emote, 4.0);
  cairo_clip(cr);
  const int sw = cairo_image_surface_get_width(surf.get());
  const int sh = cairo_image_surface_get_height(surf.get());
  if (sw > 0 && sh > 0) {
    cairo_translate(cr, slot.x, slot.y);
    cairo_scale(cr, tok_.emote / static_cast<double>(sw), tok_.emote / static_cast<double>(sh));
    cairo_set_source_surface(cr, surf.get(), 0.0, 0.0);
  } else {
    cairo_set_source_rgba(cr, 0.45, 0.45, 0.5, 1.0);
  }
  cairo_paint(cr);
  cairo_restore(cr);
}

double Panel::paintRow(cairo_t* cr, const Message& m, double x, double y, double w,
                       std::vector<EmoteSlot>* slots) {
  probeMetrics(cr);
  std::vector<EmoteSlot>& out = slots ? *slots : slotScratch_;
  out.clear();

  if (m.kind != MsgKind::Text) {
    // Membership card opaque, paid card lighter. Bilibili's cyan is not in the stylesheet.
    const pwvideo::Rgba bg = m.kind == MsgKind::Membership ? rgb(tok_.cardMembership, 1.0)
                                                           : rgb(0x00B8D4, tok_.cardPaidAlpha);
    int bodyLines = 0;
    // cardHeight already includes rowGap above and below, and the lines are laid out from
    // y + rowGap, so the box is the full height: subtracting the padding again made the last line
    // hang outside the card.
    const double h = cardHeight(cr, m, &bodyLines);
    pwvideo::roundedRect(cr, x, y, w, h, 6.0);
    cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
    cairo_fill(cr);

    // Cards speak the same line-top language as everything else.
    double lineTop = y + tok_.rowGap;
    const double px = x + tok_.padX;
    const pwvideo::Rgba fg = rgb(tok_.body);
    auto header = [&](const std::string& s, double size) {
      if (s.empty()) return;
      const pwvideo::LabelSpec ls = specFor(size, true, cardInnerWidth(), 1);
      drawRun(cr, s, ls, px, lineTop, fg);
      lineTop += lineAdvance();
    };
    header(m.user, tok_.fontCardName);
    header(m.amount, tok_.fontCardAmount);

    // The body wraps. It used to share maxLines = 1 with the two headers, so a paid message --
    // which is usually nothing but body -- was ellipsised to its first line and the rest of what
    // the viewer paid to send was never drawn.
    if (bodyLines > 0) {
      const std::string body = m.plainText();
      const pwvideo::LabelSpec bs = specFor(tok_.fontBody, true, cardInnerWidth(), kCardBodyMaxLines);
      const auto spans = wrapSpans(cr, body, bs);
      for (const auto& sp : spans) {
        std::string slice = body.substr(sp.first, sp.second);
        while (!slice.empty() && (slice.back() == '\n' || slice.back() == '\r')) slice.pop_back();
        if (!slice.empty()) {
          const pwvideo::LabelSpec ls = specFor(tok_.fontBody, false, 0.0, 1);
          drawRun(cr, slice, ls, px, lineTop, fg);
        }
        lineTop += lineAdvance();
      }
    }
    return lineTop - y;
  }

  // Plain line: the role-coloured bar, then the avatar's gap, then "name: body".
  const pwvideo::Rgba bar = rgb(barColor(m.type), m.type == UserType::Normal ? 0.5 : 1.0);
  cairo_set_source_rgba(cr, bar.r, bar.g, bar.b, bar.a);
  pwvideo::roundedRect(cr, x + tok_.barX, y + tok_.barInset, tok_.barWidth,
                       tok_.avatar - tok_.barInset * 2.0, 1.0);
  cairo_fill(cr);
  // The avatar itself is painted per frame by drawAvatars().

  const double textX = x + tok_.padX + tok_.avatar + tok_.avatarGap;
  const double avail = w - textX - tok_.padRight;

  const pwvideo::LabelSpec ns = specFor(tok_.fontUser, false, 0.0, 1);
  const std::string name = m.user + ":";
  // The name is measured, not assumed: latin and CJK widths differ enough that a fixed offset
  // would visibly misalign the body.
  const double nameW = measureRun(cr, name, ns);
  drawRun(cr, name, ns, textX, y, rgb(m.userColor != 0 ? m.userColor : nameColor(m.type)));

  const InlineBody body = buildBody(m);
  const pwvideo::LabelSpec bs = specFor(tok_.fontBody, true, std::max(20.0, avail - nameW), 8);
  const auto spans = wrapSpans(cr, body.text, bs);
  const pwvideo::Rgba fg = rgb(tok_.body);

  // The first body line sits on the same line top as the name, which is why the body was wrapped
  // at the reduced width; later lines advance from there.
  for (size_t li = 0; li < spans.size(); ++li) {
    const double lineTop = y + static_cast<double>(li) * lineAdvance();
    size_t len = spans[li].second;
    const size_t start = spans[li].first;
    while (len > 0 &&
           (body.text[start + len - 1] == '\n' || body.text[start + len - 1] == '\r')) {
      --len;
    }
    const size_t end = start + len;

    double cx = textX + nameW;  // continuation lines keep the body's left edge, not the name's
    if (li > 0) cx = textX + nameW;
    size_t cursor = start;
    for (const auto& em : body.emotes) {
      if (em.first < start) continue;
      if (em.first >= end) break;
      if (em.first > cursor) {
        const pwvideo::LabelSpec ls = specFor(tok_.fontBody, false, 0.0, 1);
        cx += drawRun(cr, body.text.substr(cursor, em.first - cursor), ls, cx, lineTop, fg);
      }
      // Inline image alignment: the box bottom rests on the descender line, which is how an inline image
      // aligns with text, so the box top is baseline + descent_ - box. The baseline comes from
      // baselineOf(lineTop), NOT from lineTop: using the line top here placed every emote exactly
      // one ascent too high, which is what made them appear to belong to the row above.
      // Stored relative to the row's own top: the list scrolls underneath these rows afterwards,
      // and rebuildLayer and the incremental path paint a row at different heights, so one relative
      // formula has to serve both.
      out.push_back(EmoteSlot{cx, baselineOf(lineTop) + descent_ - tok_.emote - y,
                              m.parts[em.second].text, m.parts[em.second].url});
      cx += tok_.emote;
      cursor = em.first + kEmoteCharLen;  // step over the placeholder itself
    }
    if (cursor < end) {
      const pwvideo::LabelSpec ls = specFor(tok_.fontBody, false, 0.0, 1);
      drawRun(cr, body.text.substr(cursor, end - cursor), ls, cx, lineTop, fg);
    }
  }
  return std::max(static_cast<double>(spans.size()) * lineAdvance(), tok_.avatar) + tok_.rowGap;
}

/* ---------------------------------------------------------------- static layer */

void Panel::reserveBand(double h) {
  if (!layer_ || !layerCr_ || h <= 0.0) return;
  cairo_t* lc = layerCr_.get();
  // Painting the surface onto itself one row up is a self-overlapping blit; pixman resolves the
  // overlap, so this behaves like a memmove rather than smearing.
  cairo_set_operator(lc, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(lc, layer_.get(), 0.0, -h);
  cairo_paint(lc);
  cairo_set_source_rgba(lc, 0, 0, 0, 0);
  cairo_rectangle(lc, 0, height_ - h, width_, h);
  cairo_fill(lc);
  cairo_set_operator(lc, CAIRO_OPERATOR_OVER);
}

void Panel::shiftAndPaint(const Message& m, double h) {
  if (h <= 0.0) return;
  reserveBand(h);
  paintRow(layerCr_.get(), m, 0.0, height_ - h, static_cast<double>(width_), nullptr);
}

void Panel::bakeAnimating() {
  if (!animating_ || !layer_ || !layerCr_) return;
  // The slot was reserved when the row arrived, so this only fills it. Shifting again would move
  // the list a second time.
  paintRow(layerCr_.get(), anim_, 0.0, height_ - animRowH_, static_cast<double>(width_), nullptr);
  animating_ = false;
  animRowH_ = 0.0;
  anim_ = Message();
}

void Panel::prune() {
  const double limit = contentH_ - height_;
  size_t drop = 0;
  while (drop < rows_.size() && rows_[drop].y + rows_[drop].h < limit) ++drop;
  if (drop > 0) rows_.erase(rows_.begin(), rows_.begin() + static_cast<ptrdiff_t>(drop));
}

void Panel::rebuildLayer(const std::vector<Message>& msgs) {
  clear();
  if (!layer_ || !layerCr_) return;
  cairo_t* lc = layerCr_.get();

  std::vector<double> hs;
  std::vector<std::vector<EmoteSlot>> es;
  hs.reserve(msgs.size());
  es.reserve(msgs.size());
  for (const Message& m : msgs) {
    bool card = false;
    hs.push_back(measureRow(m, lc, &card));
    es.push_back(slotScratch_);
  }

  double total = 0.0;
  for (double h : hs) total += h;
  // Oldest at the top, newest against the bottom, the whole block anchored to the bottom when it is
  // shorter than the panel. This has to match the incremental path, where each arriving message
  // is painted at the bottom and pushes the rest up.
  double y = static_cast<double>(height_) - total;
  for (size_t i = 0; i < msgs.size(); ++i) {
    Row r{count_++, contentH_, hs[i], msgs[i].kind != MsgKind::Text, msgs[i].avatarUrl, {}};
    // Re-paint to capture the emote slots at the row's final position. Painting twice is the price
    // of not letting a slot escape this file's helpers.
    paintRow(lc, msgs[i], 0.0, y, static_cast<double>(width_), nullptr);
    r.emotes = slotScratch_;
    rows_.push_back(std::move(r));
    contentH_ += hs[i];
    y += hs[i];
  }
}

bool Panel::update(const std::vector<Message>& fresh, int64_t nowMs) {
  if (!layer_ || !layerCr_ || fresh.empty()) return false;
  cairo_t* lc = layerCr_.get();

  bakeAnimating();

  for (size_t i = 0; i + 1 < fresh.size(); ++i) {
    bool card = false;
    const double h = measureRow(fresh[i], lc, &card);
    shiftAndPaint(fresh[i], h);
    Row r{count_++, contentH_, h, card, fresh[i].avatarUrl, {}};
    r.emotes = slotScratch_;
    rows_.push_back(std::move(r));
    contentH_ += h;
  }

  // The newest message reserves its slot immediately -- the rows below move up on the frame it
  // arrives, not when its animation ends -- and is held out of the layer only so render() can play
  // the slide-in into that reserved strip.
  const Message& last = fresh.back();
  bool card = false;
  const double h = measureRow(last, lc, &card);
  reserveBand(h);
  Row r{count_++, contentH_, h, card, last.avatarUrl, {}};
  rows_.push_back(std::move(r));
  contentH_ += h;
  anim_ = last;
  animRowH_ = h;
  animating_ = true;
  animStart_ = nowMs;
  prune();
  return true;
}

void Panel::drawAvatars(cairo_t* cr) const {
  const double top = contentH_ - static_cast<double>(height_);
  const double cx = tok_.padX + tok_.avatar / 2.0;
  const size_t n = rows_.size() - (animating_ && !rows_.empty() ? 1 : 0);
  for (size_t i = 0; i < n; ++i) {
    const Row& r = rows_[i];
    if (r.card || r.avatarUrl.empty()) continue;
    if (r.y + r.h <= top) continue;  // scrolled out of view
    const double screenTop = static_cast<double>(height_) - (contentH_ - r.y);
    drawAvatar(cr, r.avatarUrl, cx, screenTop + tok_.avatar / 2.0, tok_.avatar);
  }
}

void Panel::drawEmotes(cairo_t* cr) const {
  const double top = contentH_ - static_cast<double>(height_);
  const size_t n = rows_.size() - (animating_ && !rows_.empty() ? 1 : 0);
  for (size_t i = 0; i < n; ++i) {
    const Row& r = rows_[i];
    if (r.card || r.emotes.empty()) continue;
    if (r.y + r.h <= top) continue;
    const double screenTop = static_cast<double>(height_) - (contentH_ - r.y);
    for (const EmoteSlot& slot : r.emotes) {
      EmoteSlot moved = slot;
      moved.y += screenTop;  // slots are stored relative to the row's own top
      drawEmote(cr, moved);
    }
  }
}

void Panel::render(cairo_t* cr, int64_t nowMs) {
  if (!layer_) return;

  if (animating_ && nowMs - animStart_ >= static_cast<int64_t>(tok_.animMs)) bakeAnimating();

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, layer_.get(), 0.0, 0.0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  // Drawn live rather than baked, so a picture that lands after its row was laid out still shows.
  drawAvatars(cr);
  drawEmotes(cr);

  if (!animating_) return;

  // The fading row goes into a group so the opacity applies to the row as a whole. translate() is
  // what produces the slide-in.
  const double t = clamp01(static_cast<double>(nowMs - animStart_) / tok_.animMs);
  cairo_save(cr);
  cairo_translate(cr, tok_.animShift * (1.0 - t), 0.0);
  cairo_push_group(cr);
  paintRow(cr, anim_, 0.0, height_ - animRowH_, static_cast<double>(width_), &animSlots_);
  if (!anim_.avatarUrl.empty())
    drawAvatar(cr, anim_.avatarUrl, tok_.padX + tok_.avatar / 2.0,
               height_ - animRowH_ + tok_.avatar / 2.0, tok_.avatar);
  for (const EmoteSlot& slot : animSlots_) drawEmote(cr, slot);
  cairo_pop_group_to_source(cr);
  cairo_paint_with_alpha(cr, t);
  cairo_restore(cr);
}

}  // namespace dwm
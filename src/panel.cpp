// The chat panel. See panel.hpp for the performance reasoning behind the static layer and for the
// mapping from the reference stylesheet's custom properties to PanelTokens.
#include "panel.hpp"

#include <algorithm>
#include <cstring>

#include <pango/pangocairo.h>

namespace dwm {
namespace {

constexpr double kPi = 3.14159265358979323846;

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/** U+FFFC OBJECT REPLACEMENT CHARACTER, as UTF-8. Stands in for an emote inside the string handed
 *  to pango. */
constexpr const char* kEmoteChar = "\xEF\xBF\xBC";
constexpr size_t kEmoteCharLen = 3;

/** A message body prepared for layout: the text runs joined, with every emote collapsed to one
 *  object-replacement character, plus where each placeholder sits and which fragment it stands for.
 *
 *  Doing the substitution *before* layout means pango's own line breaking decides where the emotes
 *  land. Wrapping the fragments by hand instead would mean reimplementing break opportunities for
 *  mixed CJK and latin text, which is exactly the part that is hard to get right. The placeholder
 *  itself is never drawn -- paintRow splits each line at it and puts the picture there. */
struct InlineBody {
  std::string text;
  std::vector<std::pair<size_t, size_t>> emotes;  // (byte offset in text, index into parts)
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

/** Card height, from arithmetic alone. Deliberately not measured with pango: paintRow needs the
 *  height while it already holds the single TextRenderer layout, and a nested layout call would
 *  clobber it. */
double cardHeight(const Message& m, double nameSize, double amountSize, double bodySize,
                  double lineHeight, double rowGap) {
  double h = nameSize * lineHeight;
  if (!m.amount.empty()) h += amountSize * lineHeight;
  if (!m.parts.empty()) h += bodySize * lineHeight;
  return h + rowGap * 2.0;
}

}  // namespace

Panel::Panel(PanelTokens tokens) : tok_(std::move(tokens)) {}

void Panel::resize(int width, int height) {
  if (width == width_ && height == height_) return;
  width_ = std::max(1, width);
  height_ = std::max(1, height);
  // The deleter is supplied explicitly, as the submodule's CairoFrame does: cairo_surface_t is an
  // opaque type, so a shared_ptr that ever default-deleted one would need its size.
  layer_.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width_, height_),
               pwvideo::CairoSurfaceDeleter{});
  layerCr_ = pwvideo::ContextPtr(cairo_create(layer_.get()));
  clear();
}

void Panel::clear() {
  rows_.clear();
  contentH_ = 0.0;
  count_ = 0;
  animating_ = false;
  animRowH_ = 0.0;
  anim_ = Message();
  if (!layer_ || !layerCr_) return;
  cairo_t* lc = layerCr_.get();
  cairo_set_operator(lc, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(lc, 0, 0, 0, 0);
  cairo_paint(lc);
  cairo_set_operator(lc, CAIRO_OPERATOR_OVER);
}

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

double Panel::measureRow(const Message& m, cairo_t* cr, bool* isCard) const {
  const bool card = m.kind != MsgKind::Text;
  if (isCard) *isCard = card;
  if (card) {
    return cardHeight(m, tok_.fontCardName, tok_.fontCardAmount, tok_.fontBody, tok_.lineHeight,
                      tok_.rowGap);
  }

  // Must agree with paintRow about where the body wraps, or a row is allocated too short and its
  // last line is overwritten by whatever is drawn next. The name sits on the first line, so the
  // body wraps at (available minus the name) for that line; the name width has to be measured
  // here for the same reason paintRow measures it.
  pwvideo::LabelSpec spec;
  spec.family = tok_.font;
  spec.center = false;
  spec.bold = true;

  pwvideo::LabelSpec ns = spec;
  ns.sizePx = tok_.fontUser;
  ns.maxLines = 1;
  int nw = 0, nh = 0;
  pango_layout_get_pixel_size(text_.layout(cr, m.user + ":", ns), &nw, &nh);

  const double textX = tok_.padX + tok_.avatar + tok_.avatarGap;
  const double avail = static_cast<double>(width_) - textX - tok_.padRight;

  spec.sizePx = tok_.fontBody;
  spec.widthPx = std::max(20.0, avail - nw);
  spec.maxLines = 8;
  // Count lines on the laid-out string, emotes included: a line of nothing but an emote still takes
  // a line, and plainText() would measure a different string than the one actually drawn.
  const InlineBody body = buildBody(m);
  PangoLayout* l = text_.layout(cr, body.text, spec);
  const int lines = std::max(1, pango_layout_get_line_count(l));
  const double textH = static_cast<double>(lines) * lineAdvance();
  return std::max(textH, tok_.avatar) + tok_.rowGap;
}

void Panel::drawAvatar(cairo_t* cr, const std::string& url, double cx, double cy, double d) const {
  const double r = d / 2.0;
  cairo_save(cr);
  // Circular clip, so a square avatar is trimmed to a disc. Set before the transform below, so it
  // stays in the panel's coordinates.
  cairo_arc(cr, cx, cy, r, 0.0, 2.0 * kPi);
  cairo_clip(cr);

  pwvideo::SurfacePtr surf;
  if (avatars_) surf = avatars_->lookup(url);
  if (surf) {
    // Scale the picture into the box instead of assuming it is already the right size. cairo
    // anchors a source surface at its top-left corner, so a surface larger than the box would be
    // clipped down to that corner -- which is exactly how an avatar ends up showing one small square
    // of itself rather than the whole face.
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
    // Placeholder disc. Because avatars are drawn live rather than baked, this is genuinely
    // transient: the next frame after the picture lands shows the picture. It is still worth
    // distinguishing, since the site default avatar is itself a flat grey figure.
    cairo_set_source_rgba(cr, 0.55, 0.58, 0.62, 1.0);
  }
  cairo_paint(cr);
  cairo_restore(cr);
}

void Panel::drawAvatars(cairo_t* cr) const {
  // Rows are positioned from their content-space geometry rather than tracked incrementally, so
  // this stays correct however many times the list has been shifted.
  //
  // The row that is currently animating is deliberately skipped: update() pushes it onto rows_ like
  // any other, but render() draws its avatar inside the sliding group so the picture moves with the
  // text. Drawing it here as well would paint it a second time at the untranslated position, so the
  // avatar would sit still while the line beside it slid in.
  const double top = contentH_ - static_cast<double>(height_);
  const double cx = tok_.padX + tok_.avatar / 2.0;
  const size_t skipTail = animating_ && !rows_.empty() ? 1 : 0;
  const size_t n = rows_.size() - skipTail;
  for (size_t i = 0; i < n; ++i) {
    const Row& r = rows_[i];
    if (r.card || r.avatarUrl.empty()) continue;
    if (r.y + r.h <= top) continue;  // scrolled out of view
    // The visible window is the last height_ pixels of the content, so a row's top in content
    // space maps to the screen by subtracting where the window starts. This has to use r.y, not the
    // row's bottom: using the bottom puts every avatar one row-height below the text it belongs to,
    // while the row being animated is placed from height_ - animRowH_ and is therefore correct --
    // which is what makes the sliding avatar look like it does not line up with the rest.
    const double screenTop = static_cast<double>(height_) - (contentH_ - r.y);
    drawAvatar(cr, r.avatarUrl, cx, screenTop + tok_.avatar / 2.0, tok_.avatar);
  }
}

void Panel::drawEmote(cairo_t* cr, const Fragment& f, double x, double boxTop, double ascent,
                      double descent) const {
  const double d = tok_.emote;

  pwvideo::SurfacePtr surf;
  if (emotes_ && !f.url.empty()) surf = emotes_->lookup(f.url);

  if (!surf) {
    // Not fetched yet, or the platform advertised a token with no picture. Either way the message
    // still has to read, so the token is drawn as text in the same place the picture would be.
    pwvideo::LabelSpec ls;
    ls.family = tok_.font;
    ls.center = false;
    ls.bold = true;
    ls.sizePx = tok_.fontBody;
    ls.maxLines = 1;
    // fill() takes a baseline, and the glyph box spans from baseline-ascent to baseline+descent,
    // so centring it in the picture box works out like this.
    const double baseline = boxTop + (d - (ascent - descent)) / 2.0 + ascent;
    PangoLayout* l = text_.layout(cr, f.text, ls);
    pwvideo::TextRenderer::outline(cr, l, x, baseline, tok_.outline, rgb(tok_.outlineColor, 0.85));
    pwvideo::TextRenderer::fill(cr, l, x, baseline, rgb(tok_.body));
    return;
  }

  cairo_save(cr);
  // Slightly rounded, the way the reference stylesheet's inline images sit in the text flow.
  pwvideo::roundedRect(cr, x, boxTop, d, d, 4.0);
  cairo_clip(cr);
  const int sw = cairo_image_surface_get_width(surf.get());
  const int sh = cairo_image_surface_get_height(surf.get());
  if (sw > 0 && sh > 0) {
    cairo_translate(cr, x, boxTop);
    cairo_scale(cr, d / static_cast<double>(sw), d / static_cast<double>(sh));
    cairo_set_source_surface(cr, surf.get(), 0.0, 0.0);
  } else {
    cairo_set_source_rgba(cr, 0.45, 0.45, 0.5, 1.0);
  }
  cairo_paint(cr);
  cairo_restore(cr);
}

double Panel::paintRow(cairo_t* cr, const Message& m, double x, double y, double w) const {
  pwvideo::LabelSpec spec;
  spec.family = tok_.font;
  spec.center = false;
  spec.bold = true;

  const auto outlined = [&](PangoLayout* l, double lx, double ly, const pwvideo::Rgba& col) {
    pwvideo::TextRenderer::outline(cr, l, lx, ly, tok_.outline, rgb(tok_.outlineColor, 0.85));
    pwvideo::TextRenderer::fill(cr, l, lx, ly, col);
  };

  if (m.kind != MsgKind::Text) {
    // Membership card opaque, paid card lighter, so a stack of them does not become a slab.
    // Bilibili's cyan is not in the reference stylesheet, hence here rather than in the tokens.
    const pwvideo::Rgba bg = m.kind == MsgKind::Membership
                                 ? rgb(tok_.cardMembership, 1.0)
                                 : rgb(0x00B8D4, tok_.cardPaidAlpha);
    const double h = cardHeight(m, tok_.fontCardName, tok_.fontCardAmount, tok_.fontBody,
                                tok_.lineHeight, tok_.rowGap) -
                     tok_.rowGap * 2.0;
    pwvideo::roundedRect(cr, x, y, w, h, 6.0);
    cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
    cairo_fill(cr);

    double ly = y + tok_.rowGap;
    const double px = x + tok_.padX;
    auto line = [&](const std::string& s, double size) {
      if (s.empty()) return;
      spec.sizePx = size;
      spec.widthPx = w - 2 * tok_.padX;
      spec.maxLines = 1;
      outlined(text_.layout(cr, s, spec), px, ly, rgb(tok_.body));
      ly += size * tok_.lineHeight;
    };
    line(m.user, tok_.fontCardName);
    line(m.amount, tok_.fontCardAmount);
    line(m.plainText(), tok_.fontBody);
    return ly - y;
  }

  // Plain line: the role-coloured bar at the far left, then a gap for the avatar, then "name: body".
  const pwvideo::Rgba bar = rgb(barColor(m.type), m.type == UserType::Normal ? 0.5 : 1.0);
  cairo_set_source_rgba(cr, bar.r, bar.g, bar.b, bar.a);
  pwvideo::roundedRect(cr, x + tok_.barX, y + tok_.barInset, tok_.barWidth,
                       tok_.avatar - tok_.barInset * 2.0, 1.0);
  cairo_fill(cr);

  // The avatar is not drawn here. It is painted per frame by drawAvatars(), so that a face which
  // arrives after this row has been laid out still appears; baking it here would freeze whatever
  // was in the cache at that instant, which is usually the placeholder disc.

  const double textX = x + tok_.padX + tok_.avatar + tok_.avatarGap;
  const double avail = w - textX - tok_.padRight;

  // The name is measured rather than assumed: it can be latin or CJK, and the widths differ enough
  // that a fixed offset would visibly misalign the body.
  const uint32_t nc = m.userColor != 0 ? m.userColor : nameColor(m.type);
  pwvideo::LabelSpec ns = spec;
  ns.sizePx = tok_.fontUser;
  ns.maxLines = 1;
  PangoLayout* nl = text_.layout(cr, m.user + ":", ns);
  int nw = 0, nh = 0;
  pango_layout_get_pixel_size(nl, &nw, &nh);

  double ty = y;
  outlined(nl, textX, ty, rgb(nc));

  const double bodyX = textX + nw;
  // Emotes become object-replacement characters before layout, so pango does the line breaking and
  // places the placeholders. Each visual line is then split back at them and drawn as text runs
  // and pictures in a single pass.
  const InlineBody body = buildBody(m);
  pwvideo::LabelSpec bs = spec;
  bs.sizePx = tok_.fontBody;
  bs.widthPx = std::max(20.0, avail - nw);  // the first line shares the row with the name
  bs.maxLines = 8;
  PangoLayout* bl = text_.layout(cr, body.text, bs);

  const int nLines = std::max(1, pango_layout_get_line_count(bl));
  const double lh = lineAdvance();

  // Collect the line boundaries before drawing anything. TextRenderer owns a single PangoLayout,
  // so laying out a run inside this loop rebinds that same layout and the line list being walked
  // would be replaced underfoot -- which is exactly how continuation lines went missing.
  std::vector<std::pair<size_t, size_t>> spans;
  spans.reserve(static_cast<size_t>(nLines));
  for (int i = 0; i < nLines; ++i) {
    PangoLayoutLine* pl = pango_layout_get_line_readonly(bl, i);
    if (pl) spans.emplace_back(static_cast<size_t>(pl->start_index), static_cast<size_t>(pl->length));
  }
  if (spans.empty()) spans.emplace_back(0, body.text.size());

  // TextRenderer::fill() takes a *baseline*, not a line top: it does a move_to and then
  // pango_cairo_show_layout, whose origin is the first line's baseline. So an inline emote has to be
  // placed against that baseline using the font's own ascent and descent, or it hangs entirely
  // below the text instead of sitting on it.
  //
  // Measured with a probe layout here, after the spans above have been read and before the loop
  // below starts rebinding TextRenderer's single layout -- doing it any earlier would lose the
  // wrapped line list, doing it per emote would cost a layout each time.
  double ascent = tok_.fontBody * 0.8;
  double descent = tok_.fontBody * 0.2;
  {
    pwvideo::LabelSpec ps = bs;
    ps.widthPx = 0.0;  // natural width, so the probe is one line
    ps.maxLines = 1;
    PangoLayout* probe = text_.layout(cr, "Ag", ps);
    int pw = 0, ph = 0;
    pango_layout_get_pixel_size(probe, &pw, &ph);
    const int asc = pango_layout_get_baseline(probe) / PANGO_SCALE;
    if (asc > 0 && ph > asc) {
      ascent = asc;
      descent = ph - asc;
    }
  }

  for (const auto& span : spans) {
    // Pango keeps the newline at the end of a line's text run; drop it before measuring.
    size_t len = span.second;
    while (len > 0 && (body.text[span.first + len - 1] == '\n' ||
                       body.text[span.first + len - 1] == '\r')) {
      --len;
    }
    const size_t start = span.first;
    const size_t end = start + len;

    // Walk the line, alternating text runs and emotes, accumulating x by the measured width of
    // each run. Positions come out consistent with the wrapping above because both used pango.
    double x = bodyX;
    size_t cursor = start;
    for (const auto& em : body.emotes) {
      if (em.first < start) continue;
      if (em.first >= end) break;
      if (em.first > cursor) {
        const std::string run = body.text.substr(cursor, em.first - cursor);
        pwvideo::LabelSpec ls = spec;
        ls.sizePx = tok_.fontBody;
        ls.maxLines = 1;
        PangoLayout* rl = text_.layout(cr, run, ls);
        int rw = 0, rh = 0;
        pango_layout_get_pixel_size(rl, &rw, &rh);
        outlined(rl, x, ty, rgb(tok_.body));
        x += rw;
      }
      // The emote's bottom edge sits on the descender line, which is how an inline image aligns with
      // text. Its top therefore rises a little above the cap height, which is what the reference
      // stylesheet's 24px emoji against 20px text looks like.
      drawEmote(cr, m.parts[em.second], x, ty + descent - tok_.emote, ascent, descent);
      x += tok_.emote;
      cursor = em.first + kEmoteCharLen;  // step over the placeholder itself
    }
    if (cursor < end) {
      const std::string run = body.text.substr(cursor, end - cursor);
      pwvideo::LabelSpec ls = spec;
      ls.sizePx = tok_.fontBody;
      ls.maxLines = 1;
      outlined(text_.layout(cr, run, ls), x, ty, rgb(tok_.body));
    }
    ty += lh;
  }
  return ty - y;
}

/** Moves the accumulated picture up by h and clears the strip that opens at the bottom, without
 *  painting anything into it. The strip stays empty until the caller fills it, which is what lets
 *  the newest message reserve its slot the instant it arrives rather than 200 ms later. */
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

/** reserveBand followed by painting m into the strip it opened, for a row that is already settled
 *  and does not need animating. */
void Panel::shiftAndPaint(const Message& m, double h) {
  if (h <= 0.0) return;
  reserveBand(h);
  paintRow(layerCr_.get(), m, 0.0, height_ - h, static_cast<double>(width_));
}

/** Paints a whole list at once. Used for --demo and after a reset, where every message is known up
 *  front and the incremental path would only be paying for the same work one row at a time. */
void Panel::rebuildLayer(const std::vector<Message>& msgs) {
  clear();
  if (!layer_ || !layerCr_ || msgs.empty()) return;
  cairo_t* lc = layerCr_.get();

  std::vector<double> hs;
  hs.reserve(msgs.size());
  for (const Message& m : msgs) hs.push_back(measureRow(m, lc, nullptr));

  // Oldest at the top, newest against the bottom edge, and the whole block anchored to the
  // bottom when it is shorter than the panel. This has to match the incremental path, where each
  // arriving message is painted at the bottom and pushes the rest up; painting the list the other
  // way round would show the newest message at the top.
  double total = 0.0;
  for (double h : hs) total += h;
  double y = static_cast<double>(height_) - total;
  for (size_t i = 0; i < msgs.size(); ++i) {
    paintRow(lc, msgs[i], 0.0, y, static_cast<double>(width_));
    rows_.push_back(Row{count_++, contentH_, hs[i], msgs[i].kind != MsgKind::Text, msgs[i].avatarUrl});
    contentH_ += hs[i];
    y += hs[i];
  }
}

/** Settles the row that was fading in. The slot was already reserved when it arrived, so this only
 *  fills it; there is deliberately no shift here, or the rest of the list would jump a second
 *  time. The row was counted when it was held out, so this adds no bookkeeping. */
void Panel::bakeAnimating() {
  if (!animating_ || !layer_ || !layerCr_) return;
  paintRow(layerCr_.get(), anim_, 0.0, height_ - animRowH_, static_cast<double>(width_));
  animating_ = false;
  animRowH_ = 0.0;
  anim_ = Message();
}

void Panel::prune() {
  // Rows that have scrolled entirely out of the panel are dropped: their pixels are gone, and the
  // vector would otherwise grow for as long as the overlay runs.
  const double limit = contentH_ - height_;
  size_t drop = 0;
  while (drop < rows_.size() && rows_[drop].y + rows_[drop].h < limit) ++drop;
  if (drop > 0) rows_.erase(rows_.begin(), rows_.begin() + static_cast<ptrdiff_t>(drop));
}

bool Panel::update(const std::vector<Message>& fresh, int64_t nowMs) {
  if (!layer_ || !layerCr_ || fresh.empty()) return false;
  cairo_t* lc = layerCr_.get();

  // An animation in flight has to be settled first, otherwise its row would end up out of order
  // relative to the rows about to be appended below it.
  bakeAnimating();

  for (size_t i = 0; i + 1 < fresh.size(); ++i) {
    bool card = false;
    const double h = measureRow(fresh[i], lc, &card);
    shiftAndPaint(fresh[i], h);
    rows_.push_back(Row{count_++, contentH_, h, card, fresh[i].avatarUrl});
    contentH_ += h;
  }

  // The newest message reserves its slot immediately -- the existing rows move up on the very frame
  // it arrives, not when its animation ends -- and is held out of the layer only so render() can
  // play the slide-in into that reserved strip.
  const Message& last = fresh.back();
  bool card = false;
  const double h = measureRow(last, lc, &card);
  reserveBand(h);
  rows_.push_back(Row{count_++, contentH_, h, card, last.avatarUrl});
  contentH_ += h;
  anim_ = last;
  animRowH_ = h;
  animating_ = true;
  animStart_ = nowMs;
  prune();
  return true;
}

void Panel::render(cairo_t* cr, int64_t nowMs) {
  if (!layer_) return;

  // A finished fade is folded into the layer before it is copied, so from here on the frame is a
  // single copy again.
  if (animating_ && nowMs - animStart_ >= static_cast<int64_t>(tok_.animMs)) bakeAnimating();

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, layer_.get(), 0.0, 0.0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  // Avatars for every settled row, painted live rather than baked into the layer. They are small
  // (a 24 px disc, a few dozen of them) and this is what makes a face that arrives late still show
  // up: nothing has to be repainted, it is simply there on the next frame.
  drawAvatars(cr);

  if (!animating_) return;

  // The fading row goes into a group so its opacity applies to the row as a whole, outline
  // included. translate() is what produces the slide-in.
  const double t = clamp01(static_cast<double>(nowMs - animStart_) / tok_.animMs);
  cairo_save(cr);
  cairo_translate(cr, tok_.animShift * (1.0 - t), 0.0);
  cairo_push_group(cr);
  paintRow(cr, anim_, 0.0, height_ - animRowH_, static_cast<double>(width_));
  // Its own avatar goes inside the group too, so it slides and fades with the row instead of
  // standing still while the text beside it moves.
  if (!anim_.avatarUrl.empty())
    drawAvatar(cr, anim_.avatarUrl, tok_.padX + tok_.avatar / 2.0,
               height_ - animRowH_ + tok_.avatar / 2.0, tok_.avatar);
  cairo_pop_group_to_source(cr);
  cairo_paint_with_alpha(cr, t);
  cairo_restore(cr);
}

}  // namespace dwm
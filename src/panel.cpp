// The chat panel. See panel.hpp for the performance reasoning behind the static layer and for the
// mapping from the reference stylesheet's custom properties to PanelTokens.
#include "panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <pango/pangocairo.h>

namespace dwm {
namespace {

constexpr double kPi = 3.14159265358979323846;

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

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
  PangoLayout* l = text_.layout(cr, m.plainText(), spec);
  const int lines = std::max(1, pango_layout_get_line_count(l));
  const double body = static_cast<double>(lines) * tok_.fontBody * tok_.lineHeight;
  return std::max(body, tok_.avatar) + tok_.rowGap;
}

void Panel::drawAvatar(cairo_t* cr, const Message& m, double cx, double cy, double d) const {
  const double r = d / 2.0;
  cairo_save(cr);
  // Circular clip, so a square avatar is trimmed to a disc.
  cairo_arc(cr, cx, cy, r, 0.0, 2.0 * kPi);
  cairo_clip(cr);

  pwvideo::SurfacePtr surf;
  if (avatars_) surf = avatars_->lookup(m.avatarUrl);
  if (surf) {
    cairo_set_source_surface(cr, surf.get(), cx - r, cy - r);
  } else {
    // Placeholder disc. Bilibili hands out face URLs without a login, so a miss is normally
    // "not fetched yet" rather than "not available".
    cairo_set_source_rgba(cr, 0.55, 0.58, 0.62, 1.0);
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

  // Plain line: the role-coloured bar at the far left, then the avatar, then "name: body".
  const pwvideo::Rgba bar = rgb(barColor(m.type), m.type == UserType::Normal ? 0.5 : 1.0);
  cairo_set_source_rgba(cr, bar.r, bar.g, bar.b, bar.a);
  pwvideo::roundedRect(cr, x + tok_.barX, y + tok_.barInset, tok_.barWidth,
                       tok_.avatar - tok_.barInset * 2.0, 1.0);
  cairo_fill(cr);

  drawAvatar(cr, m, x + tok_.padX + tok_.avatar / 2.0, y + tok_.avatar / 2.0, tok_.avatar);

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
  const std::string body = m.plainText();
  pwvideo::LabelSpec bs = spec;
  bs.sizePx = tok_.fontBody;
  bs.widthPx = std::max(20.0, avail - nw);  // the first line shares the row with the name
  bs.maxLines = 8;
  PangoLayout* bl = text_.layout(cr, body, bs);

  // Each visual line is filled separately: that is what lets the name carry its own colour while
  // the body still wraps at the right width and continues under itself rather than under the name.
  const int nLines = std::max(1, pango_layout_get_line_count(bl));
  const double lh = tok_.fontBody * tok_.lineHeight;

  // Collect the line boundaries before drawing anything. TextRenderer owns a single PangoLayout,
  // so laying out a slice inside this loop rebinds that same layout and the line list being walked
  // would be replaced underfoot -- which is exactly how continuation lines went missing.
  std::vector<std::pair<size_t, size_t>> spans;
  spans.reserve(static_cast<size_t>(nLines));
  for (int i = 0; i < nLines; ++i) {
    PangoLayoutLine* pl = pango_layout_get_line_readonly(bl, i);
    if (pl) spans.emplace_back(static_cast<size_t>(pl->start_index), static_cast<size_t>(pl->length));
  }
  if (spans.empty()) spans.emplace_back(0, body.size());

  for (const auto& span : spans) {
    std::string slice = body.substr(span.first, span.second);
    // Pango keeps the newline at the end of a line's text run.
    while (!slice.empty() && (slice.back() == '\n' || slice.back() == '\r')) slice.pop_back();
    if (!slice.empty()) {
      pwvideo::LabelSpec ls = spec;
      ls.sizePx = tok_.fontBody;
      ls.maxLines = 1;
      outlined(text_.layout(cr, slice, ls), bodyX, ty, rgb(tok_.body));
    }
    ty += lh;
  }
  return ty - y;
}

/** Moves the accumulated picture up by h and paints m into the strip that opens at the bottom. */
void Panel::shiftAndPaint(const Message& m, double h) {
  if (!layer_ || !layerCr_ || h <= 0.0) return;
  cairo_t* lc = layerCr_.get();
  // Painting the surface onto itself one row up is a self-overlapping blit; pixman resolves the
  // overlap, so this behaves like a memmove rather than smearing.
  cairo_set_operator(lc, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(lc, layer_.get(), 0.0, -h);
  cairo_paint(lc);
  // The strip exposed at the bottom is stale; clear it before drawing into it.
  cairo_set_source_rgba(lc, 0, 0, 0, 0);
  cairo_rectangle(lc, 0, height_ - h, width_, h);
  cairo_fill(lc);
  cairo_set_operator(lc, CAIRO_OPERATOR_OVER);
  paintRow(lc, m, 0.0, height_ - h, static_cast<double>(width_));
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
    rows_.push_back(Row{count_++, contentH_, hs[i], msgs[i].kind != MsgKind::Text});
    contentH_ += hs[i];
    y += hs[i];
  }
}

/** Folds a finished entrance animation into the static layer. The row was already counted when it
 *  was held out, so this only moves its pixels; it must not add a row. */
void Panel::bakeAnimating() {
  if (!animating_) return;
  shiftAndPaint(anim_, animRowH_);
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
    rows_.push_back(Row{count_++, contentH_, h, card});
    contentH_ += h;
  }

  // The newest message is held out of the layer so it can fade in.
  const Message& last = fresh.back();
  bool card = false;
  const double h = measureRow(last, lc, &card);
  rows_.push_back(Row{count_++, contentH_, h, card});
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

  if (!animating_) return;

  // The fading row goes into a group so its opacity applies to the row as a whole, outline
  // included. translate() is what produces the slide-in.
  const double t = clamp01(static_cast<double>(nowMs - animStart_) / tok_.animMs);
  cairo_save(cr);
  cairo_translate(cr, tok_.animShift * (1.0 - t), 0.0);
  cairo_push_group(cr);
  paintRow(cr, anim_, 0.0, height_ - animRowH_, static_cast<double>(width_));
  cairo_pop_group_to_source(cr);
  cairo_paint_with_alpha(cr, t);
  cairo_restore(cr);
}

}  // namespace dwm
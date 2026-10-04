#include "notice.hpp"

#include <algorithm>
#include <utility>

#include "cairo_util.hpp"

namespace dwm {
namespace {

constexpr double kCardRadius = 6.0;

/** The fade at the end of a notice's dwell. Long enough to read as a fade rather than a cut, short
 *  enough that a notice does not linger as a ghost after it should have gone. */
constexpr int64_t kFadeMs = 600;

/** LabelSpec::maxLines for "wrap as much as it takes".
 *
 *  TextRenderer hands maxLines to pango_layout_set_height() negated, and pango reads the sign to mean
 *  two different things: negative is a maximum number of lines, positive is a *minimum*. So 0 is the
 *  unconstrained case, and -1 would ask for a layout at least one line tall -- which ellipsizes a
 *  long body back down to a single line and throws away exactly the text this layer exists to show. */
constexpr int kUnlimitedLines = 0;

}  // namespace

double PinnedNotice::alpha(int64_t nowMs) const {
  const int64_t held = nowMs - shownAtMs;
  if (held <= dwellMs) return 1.0;
  if (fadeMs <= 0) return 0.0;
  const double t = static_cast<double>(held - dwellMs) / static_cast<double>(fadeMs);
  return std::clamp(1.0 - t, 0.0, 1.0);
}

int64_t PinnedLayer::dwellFor(int64_t amountValue) {
  // Whole currency units, cheapest band last. Each figure is the dwell the platform sells at that
  // price, so the boundaries are the prices themselves rather than round numbers.
  if (amountValue >= 2000) return 2 * 60 * 60 * 1000;
  if (amountValue >= 1000) return 60 * 60 * 1000;
  if (amountValue >= 500) return 30 * 60 * 1000;
  if (amountValue >= 100) return 5 * 60 * 1000;
  if (amountValue >= 50) return 2 * 60 * 1000;
  return 60 * 1000;
}

void PinnedLayer::resize(int width, int height) {
  width_ = std::max(1, width);
  height_ = std::max(1, height);
  // The notices are laid out against the width, so this invalidates every height. They are kept and
  // re-measured on the next frame rather than dropped: a resize should not make the room go quiet
  // for the next two hours. restack() runs from render() when it notices layoutW_ no longer holds.
}

double PinnedLayer::layout(const Message& m, cairo_t* cr, double w,
                           std::vector<Line>* out) const {
  pwvideo::LabelSpec spec;
  spec.family = tok_.font;
  spec.center = false;
  spec.bold = true;
  spec.widthPx = std::max(20.0, w - 2 * tok_.padX);

  std::vector<Line> lines;

  // The two header lines first, in the order the card shows them.
  spec.maxLines = 1;
  spec.ellipsize = true;
  if (!m.user.empty()) {
    spec.sizePx = tok_.fontCardName;
    lines.push_back(Line{m.user, tok_.fontCardName,
                         static_cast<double>(pwvideo::TextRenderer::measure(
                             text_.layout(cr, m.user, spec)).height),
                         false});
  }
  if (!m.amount.empty()) {
    spec.sizePx = tok_.fontCardAmount;
    lines.push_back(Line{m.amount, tok_.fontCardAmount,
                         static_cast<double>(pwvideo::TextRenderer::measure(
                             text_.layout(cr, m.amount, spec)).height),
                         false});
  }

  // Then the body, which wraps and is never ellipsized. This is the one thing that separates a pinned
  // notice from the membership card: a card body is one line by design, but a paid message is the
  // thing the sender is paying to be read, and truncating it would discard the only part they wrote.
  // pango decides where it breaks, so the lines painted are exactly the lines that were measured.
  const std::string body = m.plainText();
  if (!body.empty()) {
    spec.sizePx = tok_.fontBody;
    spec.maxLines = kUnlimitedLines;
    spec.ellipsize = false;
    lines.push_back(Line{body, tok_.fontBody,
                         static_cast<double>(pwvideo::TextRenderer::measure(
                             text_.layout(cr, body, spec)).height),
                         true});
  }

  double content = 0.0;
  for (const Line& l : lines) content += l.height;
  if (out) *out = std::move(lines);
  return content + tok_.rowGap * 2.0;  // padding above and below, closing the card
}

void PinnedLayer::paint(const Message& m, cairo_t* cr, double y, double w, double h) const {
  // Opaque, where the chat panel's paid card is 0.55: there is never a stack of these behind it to
  // see through, and a pinned notice is meant to be what the eye lands on. Bilibili's cyan, which no
  // token in the stylesheet provides.
  pwvideo::roundedRect(cr, 0.0, y, w, h, kCardRadius);
  cairo_set_source_rgba(cr, 0.0, 0xB8 / 255.0, 0xD4 / 255.0, 1.0);
  cairo_fill(cr);

  // Measured again here rather than carried in from restack(): TextRenderer owns a single layout, so
  // laying out the lines below would otherwise destroy the list this came from.
  std::vector<Line> lines;
  layout(m, cr, w, &lines);

  const pwvideo::Rgba body{1.0, 1.0, 1.0, 1.0};
  const pwvideo::Rgba shadow{0.0, 0.0, 0.0, 0.85};
  const double pad = tok_.rowGap;
  double ly = y + pad;
  for (const Line& l : lines) {
    pwvideo::LabelSpec cs;
    cs.family = tok_.font;
    cs.center = false;
    cs.bold = true;
    cs.sizePx = l.size;
    cs.widthPx = std::max(20.0, w - 2 * tok_.padX);
    // A wrapped body has to be allowed to occupy every line it measured here, or pango ellipsizes it
    // back to one at draw time and the card is taller than the text in it.
    cs.maxLines = l.wraps ? kUnlimitedLines : 1;
    cs.ellipsize = !l.wraps;
    PangoLayout* lay = text_.layout(cr, l.text, cs);
    pwvideo::TextRenderer::outline(cr, lay, tok_.padX, ly, tok_.outline, shadow);
    pwvideo::TextRenderer::fill(cr, lay, tok_.padX, ly, body);
    // The same figure layout() arrived at, so the painted stack ends where the reserved height says.
    ly += l.height;
  }
}

void PinnedLayer::restack(cairo_t* cr, double w) {
  double y = 0.0;
  for (size_t i = 0; i < notices_.size(); ++i) {
    PinnedNotice& n = notices_[i];
    n.h = layout(n.msg, cr, w, nullptr);
    n.y = y;
    // The gap goes between cards and never above the first one. layout() already returns the card's
    // own padding inside its height, so without this the opaque cards sit flush against each other
    // and a stack of them reads as one tall slab rather than as separate messages. Above the first
    // card there is nothing to separate it from: the stack is top-edge aligned, so the newest notice
    // has to be flush against the top of the panel.
    y += n.h;
    if (i + 1 < notices_.size()) y += tok_.rowGap;
  }
  contentH_ = y;
  layoutW_ = w;
}

void PinnedLayer::update(cairo_t* cr, const std::vector<Message>& fresh, int64_t nowMs) {
  bool added = false;
  for (const Message& m : fresh) {
    if (m.kind != MsgKind::Paid) continue;
    PinnedNotice n;
    n.msg = m;
    n.shownAtMs = nowMs;
    n.dwellMs = dwellFor(m.amountValue);
    n.fadeMs = kFadeMs;
    // Newest first: the message that just arrived belongs at the top edge, where it is the most
    // prominent thing on the panel, and whatever is already up gets pushed down under it.
    notices_.insert(notices_.begin(), std::move(n));
    added = true;
  }
  if (added) restack(cr, static_cast<double>(width_));
}

void PinnedLayer::render(cairo_t* cr, int64_t nowMs) {
  if (notices_.empty()) return;

  // The surface being drawn on may be the negotiated size rather than the configured one, so the
  // width the text is fitted to is the narrower of the two: fitting to the wider one would run the
  // last words off the edge of a smaller consumer.
  cairo_surface_t* target = cairo_get_target(cr);
  // cairo_image_surface_get_width asserts on anything that is not an image surface, and the target
  // here is whatever the caller passed -- the frame today, but nothing forces it to stay that. An
  // unknown target is treated as full width rather than as zero, so a card laid out at width 0 would
  // wrap one character per line.
  double surfaceW = static_cast<double>(width_);
  if (target && cairo_surface_get_type(target) == CAIRO_SURFACE_TYPE_IMAGE)
    surfaceW = static_cast<double>(cairo_image_surface_get_width(target));
  const double w = std::min(static_cast<double>(width_), surfaceW);

  // Heights belong to the width they were measured at. A resize changes that, and a notice fitted to
  // the old one would have to be re-measured or its words would overhang.
  if (layoutW_ != w) restack(cr, w);

  // Drop the ones whose fade has run out, then close the gap. This is what lets the clock run on its
  // own: nothing has to arrive for an expired notice to leave.
  //
  // The scan is separate from the rebuild on purpose. Filtering into a second vector moves out of
  // every element it keeps, so doing that unconditionally would leave notices_ holding moved-from
  // messages whenever nothing had expired -- the layer would measure them correctly and then paint
  // empty cards, which is exactly what it did.
  bool expired = false;
  for (const PinnedNotice& n : notices_) {
    if (n.expired(nowMs)) {
      expired = true;
      break;
    }
  }
  if (expired) {
    std::vector<PinnedNotice> kept;
    kept.reserve(notices_.size());
    for (PinnedNotice& n : notices_) {
      if (!n.expired(nowMs)) kept.push_back(std::move(n));
    }
    notices_ = std::move(kept);
    restack(cr, w);
  }

  for (const PinnedNotice& n : notices_) {
    if (n.y >= static_cast<double>(height_)) break;  // below the panel; none of it is visible
    const double a = n.alpha(nowMs);
    if (a <= 0.0) continue;
    cairo_save(cr);
    // The whole card goes into a group so the opacity covers its background and its outlined text
    // together; fading the two separately would leave the outline holding on after the fill.
    cairo_push_group(cr);
    paint(n.msg, cr, n.y, w, n.h);
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, a);
    cairo_restore(cr);
  }
}

}  // namespace dwm
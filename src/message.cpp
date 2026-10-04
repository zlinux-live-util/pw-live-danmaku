// The platform-independent message model. See message.hpp for why each piece exists.
#include "message.hpp"

namespace dwm {

namespace {
/** The text a counted row's count is written with.
 *
 *  One definition of it, because Message::withCount both writes the count and recognises the one
 *  it is replacing. Two spellings of " ×" would mean a rebuilt row could keep the count it was
 *  meant to replace and read " ×6 ×12". */
constexpr const char* kCountText = " ×";

/** Whether this fragment is a count, by the only two things a count is: a text run, and text that
 *  begins with the marker above. */
bool isCount(const Fragment& f) {
  return f.kind == Fragment::Kind::Text && f.text.rfind(kCountText, 0) == 0;
}
}  // namespace

Message Message::withCount(int64_t total) const {
  Message out = *this;
  out.count = total;
  // Trailing runs only, so a row keeps everything else in the order it had. Popping rather than
  // rewriting in place keeps this idempotent: a row whose count was already replaced is recognised
  // and replaced again.
  while (!out.parts.empty() && isCount(out.parts.back())) out.parts.pop_back();
  if (total > 1)
    out.parts.push_back(
        Fragment{Fragment::Kind::Text, std::string(kCountText) + std::to_string(total), "", 0});
  return out;
}

std::string Message::plainText() const {
  std::string out;
  for (const Fragment& f : parts) {
    if (f.kind == Fragment::Kind::Text) {
      out += f.text;
    } else if (!f.text.empty()) {
      // An emote with no image still needs to occupy its place in the flow, so the token is kept
      // as typed. Rendering substitutes the picture later; the layout sees the token either way.
      out += f.text;
    }
  }
  return out;
}

void MessageList::push(Message m) {
  items_.push_back(std::move(m));
  // Bounded, oldest-first: a long-running overlay in a busy room would otherwise grow without
  // limit. Dropping the oldest is the right end to lose -- a chat panel is a window onto the
  // recent past, and the newest message is the one the streamer is looking at.
  while (items_.size() > capacity_) items_.pop_front();
  ++generation_;
}

}  // namespace dwm
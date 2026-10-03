// The platform-independent message model. See message.hpp for why each piece exists.
#include "message.hpp"

namespace dwm {

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
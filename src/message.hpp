#pragma once
// One chat message, in a form that does not mention any platform.
//
// This is the boundary the rest of the project is arranged around. A site implementation turns its
// wire events into Messages; the panel turns Messages into pixels and knows nothing about where
// they came from. That is what makes a second site a matter of writing one more translator rather
// than touching the renderer.
//
// The pieces that exist because of the *second* site, and which would otherwise look like
// unnecessary generality:
//   * Fragment: a message body is a run of text and inline images, not one string. Twitch emotes
//     are images at known character offsets; bilibili emote danmaku carry a "[token]" that has to
//     be substituted. Both are the same shape once normalised.
//   * UserType: twitch supplies moderator/subscriber/broadcaster badges. bilibili cannot supply
//     equivalent data anonymously, so it uses Normal, but the slot has to exist.
//   * avatarUrl: bilibili does supply faces, and twitch does too.

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace dwm {

/** Role colouring, in the reference stylesheet's order of precedence. */
enum class UserType { Normal, Member, Moderator, Owner };

/** What a message is, which decides both where it is drawn and how loud it is.
 *
 *  The two paid kinds are cards; the three at the bottom are ordinary rows in the chat column that
 *  carry an event instead of a typed body. They are here rather than being folded into Text because
 *  a viewer reads them differently -- "somebody said this" versus "somebody did this" -- and the
 *  panel dims the ambient ones so they do not crowd out what was actually typed. */
enum class MsgKind {
  /** An ordinary chat line. */
  Text,
  /** A paid message (billibili SUPER_CHAT / twitch cheer). */
  Paid,
  /** A membership purchase (billibili GUARD_BUY / twitch sub). */
  Membership,
  /** A gift (billibili SEND_GIFT_V2 / twitch cheer). Drawn like chat, with its own bar colour. */
  Gift,
  /** Somebody entered the room (billibili INTERACT_WORD_V2). Ambient: dimmed, no bar, no avatar. */
  Entry,
  /** Somebody liked the stream (billibili LIKE_INFO_V3_CLICK). Ambient, but the face is kept: the
   *  site's own like notice shows one, and a like is deliberate rather than incidental. */
  Like,
};

/** Whether a kind is a card: the two paid kinds, and only those.
 *
 *  A free function rather than a member because an enum class cannot carry one. It also keeps the
 *  question explicit at every call site -- "is this row a card" is exactly the kind of test where an
 *  earlier `kind != Text` quietly turned three new kinds into cards. */
constexpr bool isCard(MsgKind k) { return k == MsgKind::Paid || k == MsgKind::Membership; }

struct Fragment {
  enum class Kind { Text, Emote };
  Kind kind = Kind::Text;
  /** Text run, or for an emote the token as the sender typed it, e.g. "[热]". */
  std::string text;
  /** Emote image URL. Empty for a text run, and also empty when an emote token had no image to
   *  point at, in which case the renderer falls back to showing the token literally. */
  std::string url;
  /** Intrinsic emote size in pixels as advertised by the platform; 0 when unknown. */
  int px = 0;
};

struct Message {
  MsgKind kind = MsgKind::Text;
  UserType type = UserType::Normal;
  std::string user;
  /** Username colour as 0xRRGGBB. 0 means "use the palette colour for type", which is what a site
   *  with no per-user colour sends. */
  uint32_t userColor = 0;
  std::string avatarUrl;
  std::vector<Fragment> parts;

  /** Secondary line for a card: a price such as "CN¥30.0", or a membership label. Empty for a
   *  plain chat line. */
  std::string amount;

  /** The same price as a number, in whole units of the site's currency; 0 when there is none.
   *
   *  Carried beside the formatted `amount` because presentation needs the magnitude, not the
   *  rendering: a pinned paid message stays up for a time that depends on what was paid, and
   *  recovering that from "CN¥1980" would mean parsing a display string back into a number. A site
   *  that bills in a different unit sets it to what its own table is denominated in. */
  int64_t amountValue = 0;

  int64_t tsMs = 0;

  /** The body as plain text, concatenating the text runs and keeping emote tokens as typed. Used
   *  for layout fallback and for diagnostics. */
  std::string plainText() const;
};

/** A bounded, oldest-first message history. The bound is what keeps memory flat in a busy room:
 *  without it a long-running overlay grows without limit. */
class MessageList {
 public:
  explicit MessageList(size_t capacity = 200) : capacity_(capacity) {}

  void push(Message m);
  const std::deque<Message>& all() const { return items_; }
  size_t size() const { return items_.size(); }
  bool empty() const { return items_.empty(); }
  size_t capacity() const { return capacity_; }
  void clear() { items_.clear(); ++generation_; }

  /** Bumped by every mutation. The panel compares it against the generation it last laid out, so a
   *  re-layout happens exactly when the contents changed. */
  uint64_t generation() const { return generation_; }

 private:
  std::deque<Message> items_;
  size_t capacity_;
  uint64_t generation_ = 1;
};

}  // namespace dwm
#pragma once
// Avatar cache that the render callback may read without ever blocking.
//
// The submodule's AssetCache does the fetching and decoding, but its get() blocks on the network.
// Calling that from a frame callback would stall the PipeWire graph, so the split is:
//
//   worker thread : takes URLs off a pending queue, calls AssetCache::get (blocking is fine here)
//   render thread : looks a URL up in this store, which is a lock and a shared_ptr copy
//
// A miss is normal, not an error: the panel draws a neutral placeholder until the worker gets round
// to it, and nothing is ever fetched on demand from the frame path.
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "assetcache.hpp"
#include "cairo_util.hpp"

namespace dwm {

class AvatarStore {
 public:
  /** pixelSize is the box the fetched image is decoded and cropped to; capacity is how many are
   *  kept. They are separate because conflating them is how the first version of this ended up
   *  decoding every face at whatever the LRU limit happened to be and then showing its top-left
   *  corner: a 96x96 surface clipped into a 24px circle does not show the whole avatar. */
  AvatarStore(int pixelSize, size_t capacity, std::string userAgent);
  ~AvatarStore();

  /** Records a URL as wanted. Safe from the site thread; does nothing if already known or pending. */
  void request(const std::string& url);

  /** Number of distinct URLs still to be fetched. */
  size_t pending() const;

  /** Looks up a decoded avatar. Returns null when it is not cached yet. Never blocks and never
   *  touches the network. This is the only call the render path makes. */
  pwvideo::SurfacePtr lookup(const std::string& url) const;

  /** Whether the URL is already decoded and cached. Used only for diagnostics: a user with no
   *  picture and a user sharing the site default avatar look identical on screen, and the fetch
   *  counters cannot tell them apart because neither of them is a fetch. */
  bool isCached(const std::string& url) const;

  /** Resolves blocking get() calls until stop() is called or the queue empties. */
  void runWorker();
  void stop();

  size_t cached() const;
  uint64_t fetched() const;
  uint64_t failed() const;
  /** Description of the most recent failure, empty when the last fetch succeeded. The AssetCache
   *  does not report why a fetch failed, so this is captured from the fetcher's own counters plus
   *  the URL, which is enough to tell "no avatar for this user" from "the CDN refused us". */
  std::string lastError() const;

 private:
  struct Slot {
    pwvideo::SurfacePtr surf;
    uint64_t used = 0;  // for the LRU: bumped on every hit
  };

  int size_;
  size_t capacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  /** URLs already queued, so a chatty sender asking repeatedly does not queue the same fetch many
   *  times. Erased again when the fetch fails, which lets a later message retry it. */
  std::unordered_set<std::string> pendingUrls_;
  // mutable because lookup() is logically a read that still refreshes the LRU clock: which avatar
  // was used most recently is bookkeeping, not observable state.
  mutable std::unordered_map<std::string, Slot> cache_;
  mutable uint64_t clock_ = 0;
  bool stop_ = false;
  mutable std::string lastError_;
  std::unique_ptr<pwvideo::AssetCache> fetcher_;
};

}  // namespace dwm
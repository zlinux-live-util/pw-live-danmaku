// Avatar cache with a non-blocking read side. See avatars.hpp for why the fetching and the looking
// up are split across two threads.
#include "avatars.hpp"

#include <memory>
#include <utility>

namespace dwm {

AvatarStore::AvatarStore(int pixelSize, size_t capacity, std::string userAgent)
    : size_(std::max(1, pixelSize)), capacity_(std::max<size_t>(1, capacity)) {
  pwvideo::AssetCache::Options opt;
  // The fetcher keeps its own small cache purely to reuse the connection; this class decides what
  // stays, because it also has to know what to evict.
  opt.capacity = 8;
  opt.userAgent = std::move(userAgent);
  // Held by pointer because AssetCache is neither copyable nor movable, so it cannot be
  // constructed in a member initialiser from an Options built in the body.
  fetcher_ = std::make_unique<pwvideo::AssetCache>(opt);
}

AvatarStore::~AvatarStore() { stop(); }

void AvatarStore::request(const std::string& url) {
  if (url.empty()) return;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (cache_.count(url) || pendingUrls_.count(url)) return;
    pendingUrls_.insert(url);
    queue_.push_back(url);
  }
  cv_.notify_one();
}

size_t AvatarStore::pending() const {
  std::lock_guard<std::mutex> lk(mu_);
  return queue_.size();
}

pwvideo::SurfacePtr AvatarStore::lookup(const std::string& url) const {
  if (url.empty()) return nullptr;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = cache_.find(url);
  if (it == cache_.end()) return nullptr;
  it->second.used = ++clock_;  // the LRU clock advances on hits, so a popular avatar stays
  return it->second.surf;
}

bool AvatarStore::isCached(const std::string& url) const {
  if (url.empty()) return false;
  std::lock_guard<std::mutex> lk(mu_);
  return cache_.count(url) != 0;
}

size_t AvatarStore::cached() const {
  std::lock_guard<std::mutex> lk(mu_);
  return cache_.size();
}

uint64_t AvatarStore::fetched() const { return fetcher_->fetched(); }
uint64_t AvatarStore::failed() const { return fetcher_->failed(); }

std::string AvatarStore::lastError() const {
  std::lock_guard<std::mutex> lk(mu_);
  return lastError_;
}

void AvatarStore::runWorker() {
  for (;;) {
    std::string url;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty()) return;
      url = queue_.front();
      queue_.pop_front();
    }

    // Blocking by design: this is the worker, not the frame path.
    pwvideo::SurfacePtr surf = fetcher_->get(url, size_);

    std::lock_guard<std::mutex> lk(mu_);
    if (!surf) {
      pendingUrls_.erase(url);  // failed: allow a later message to try again
      lastError_ = url;
      continue;
    }
    cache_[url] = Slot{std::move(surf), ++clock_};

    // Evict the least recently used entry. Dropping a surface here can run cairo's finaliser, so
    // it happens under the lock but on a surface nobody is holding a reference to.
    while (cache_.size() > capacity_) {
      auto oldest = cache_.begin();
      for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if (it->second.used < oldest->second.used) oldest = it;
      }
      cache_.erase(oldest);
    }
  }
}

void AvatarStore::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
}

}  // namespace dwm
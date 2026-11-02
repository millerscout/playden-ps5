#pragma once

#include "library/model.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace playden {
class Storage;
}

namespace playden::steam {

// One app from IStoreBrowseService/GetItems: user tag names, screenshot URLs, and the
// highlight trailer's thumbnail and file. Public store data, not tied to an account.
struct StoreBrowseItem {
  std::uint32_t app = 0;
  bool tags_known = false;
  std::vector<std::string> tags;
  std::vector<std::string> screenshots;
  std::string trailer_thumbnail;
  std::string trailer_url;
  std::uint64_t fetched_at = 0;
  // True once GetItems or appdetails has answered for screenshots and the trailer, even if both are empty.
  bool media_known = false;
  std::string developer;
  std::string release_date;
  // From GetItems basic_info / controller categories, or filled later from appdetails.
  bool description_known = false;
  std::string description;
  std::string controller_support;  // "full", "partial", or empty
  // Genre names are not in GetItems. appdetails writes them onto this record.
  bool genres_known = false;
  std::vector<std::string> genres;
};

// A saved record is reused for this long, the same age as the cover-art cache.
constexpr std::uint64_t kStoreBrowseTtlSeconds = 14ull * 24ull * 3600ull;

std::string StoreTagListUrl();
std::string StoreBrowseItemsUrl(const std::vector<std::uint32_t>& apps);

// Names for tag ids, from IStoreService/GetTagList. Unknown ids are absent.
std::map<std::uint32_t, std::string> ParseStoreTagList(std::string_view body);
// Items in response order, or nothing when the body is not a GetItems response.
// Tag names come from `tags`; a missing dictionary leaves tags_known false.
std::optional<std::vector<StoreBrowseItem>> ParseStoreBrowseItems(std::string_view body,
                                                                  const std::map<std::uint32_t, std::string>& tags);

// A GetItems batch is larger than one appdetails page. The browse fetch may use this many bytes.
constexpr std::size_t kStoreBrowseResponseBytes = 8u * 1024u * 1024u;

// Fetches store browse data in batches. The game on screen is the next batch's first app.
// A record younger than kStoreBrowseTtlSeconds is answered from the local file and sends nothing.
class StoreBrowseService {
 public:
  using Fetch = std::function<std::optional<std::string>(const std::string& url, const std::atomic<bool>& cancel)>;

  explicit StoreBrowseService(Fetch fetch, std::chrono::milliseconds pause = std::chrono::milliseconds(350));
  ~StoreBrowseService();
  StoreBrowseService(const StoreBrowseService&) = delete;
  StoreBrowseService& operator=(const StoreBrowseService&) = delete;

  // The file that holds every app's record. Set before the first request.
  void SetStorage(const Storage* storage);
  // Unix seconds for the cache age. The system clock when unset.
  void SetNow(std::function<std::uint64_t()> now);

  void Request(GameId id);
  // The open game: next after whatever batch is already downloading.
  void RequestFirst(GameId id);
  // Ids the worker asks for, in order, when the queue has room. Replaces any walk already set.
  void SetWalk(std::vector<GameId> ids);
  // Called on the worker after a fetched batch, so the library file is not written on the UI thread.
  void SetTagSave(std::function<void(std::uint64_t account, std::vector<std::pair<GameId, std::vector<std::string>>>)> save);
  void SetTagAccount(std::uint64_t account);
  // True when the background queue already holds kMaxPending games.
  bool Full();
  // The saved record while it is under kStoreBrowseTtlSeconds, including one that has no description yet.
  std::optional<StoreBrowseItem> Cached(GameId id);
  // Writes genres onto the saved record. Description, controller support, developer and release date fill in
  // only when absent. With include_media, empty screenshot and trailer fields take the store page's URLs.
  void RememberDetails(GameId id, std::string description, std::vector<std::string> genres,
                       std::string controller, std::vector<std::string> screenshots = {},
                       std::string trailer_thumbnail = {}, std::string trailer_url = {},
                       std::string developer = {}, std::string release_date = {}, bool include_media = false);
  std::vector<StoreBrowseItem> Pump(std::size_t maximum = 8);
  void Shutdown();

  static constexpr std::size_t kMaxPending = 256;
  static constexpr std::size_t kBatch = 20;

 private:
  void Run();
  // Stores a parsed batch. A missing body is split in half and fetched again, down to one app.
  void FetchItems(const std::vector<std::uint32_t>& batch, const std::map<std::uint32_t, std::string>& tag_names);
  void EnsureLoaded();
  void SaveLocked(std::unique_lock<std::mutex>& lock);
  std::uint64_t Now() const;
  bool Current(const StoreBrowseItem& item, std::uint64_t now) const;
  bool Fresh(const StoreBrowseItem& item, std::uint64_t now) const;
  void Enqueue(GameId id, bool first);
  // Moves the next walk ids into the queue. The lock is held. Cache hits become results.
  void FillWalkLocked(std::uint64_t now);

  Fetch fetch_;
  std::chrono::milliseconds pause_;
  const Storage* storage_ = nullptr;
  std::function<std::uint64_t()> now_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<GameId> pending_;
  std::vector<GameId> walk_;
  std::size_t walk_at_ = 0;
  std::function<void(std::uint64_t, std::vector<std::pair<GameId, std::vector<std::string>>>)> tag_save_;
  std::atomic<std::uint64_t> tag_account_{0};
  std::set<GameId> requested_;
  std::deque<StoreBrowseItem> results_;
  std::map<std::uint32_t, StoreBrowseItem> cached_;
  std::map<std::uint32_t, std::string> tag_names_;
  std::uint64_t tag_names_at_ = 0;
  bool cache_loaded_ = false;
  std::atomic<bool> cancel_{false};
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace playden::steam

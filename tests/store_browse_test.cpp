// store_browse_test.cpp - batched Steam store tags, screenshots and trailers.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "platform/storage.h"
#include "steam/store_browse_service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using playden::GameId;

using namespace playden::steam;

#define CHECK(x) \
  do { \
    if (!(x)) { \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
      std::exit(1); \
    } \
  } while (0)

const char* kTags = R"({"response":{"tags":[{"tagid":21,"name":"Adventure"},{"tagid":4182,"name":"Singleplayer"}]}})";

const char* kItems = R"({"response":{"store_items":[{
  "appid": 620,
  "tags": [{"tagid": 21, "weight": 10}, {"tagid": 4182, "weight": 9}, {"tagid": 999999, "weight": 1}],
  "screenshots": {"all_ages_screenshots": [
    {"filename": "steam/apps/620/ss_one.jpg?t=1", "ordinal": 1},
    {"filename": "steam/apps/620/ss_two.jpg?t=1", "ordinal": 2},
    {"filename": "steam/apps/620/ss_sized.1920x1080.jpg?t=1", "ordinal": 3}
  ]},
  "trailers": {"highlights": [{
    "trailer_url_format": "steam/apps/${FILENAME}?t=5",
    "screenshot_medium": "5787/movie.293x165.jpg",
    "microtrailer": [
      {"filename": "620/3746/clip.webm", "type": "video/webm"},
      {"filename": "620/3746/clip.mp4", "type": "video/mp4"}
    ]
  }]},
  "basic_info": {"short_description": "A puzzle-platformer."},
  "categories": {"controller_categoryids": [28, 18]}
}]}})";

std::string ItemsFor(const std::string& url) {
  std::string items = "{\"response\":{\"store_items\":[";
  bool any = false;
  for (const char* id : {"11", "12", "13", "14", "620"}) {
    const std::string key = std::string("appid%22%3A") + id;
    if (url.find(key) == std::string::npos) continue;
    if (any) items += ',';
    any = true;
    items += std::string("{\"appid\":") + id + ",\"tags\":[{\"tagid\":21}]}";
  }
  items += "]}}";
  return items;
}

void ParsedFixtureNamesShotsAndTrailer() {
  const auto tags = ParseStoreTagList(kTags);
  CHECK(tags.at(21) == "Adventure" && tags.at(4182) == "Singleplayer");
  const auto items = ParseStoreBrowseItems(kItems, tags);
  CHECK(items && items->size() == 1 && (*items)[0].app == 620 && (*items)[0].tags_known);
  CHECK((*items)[0].tags.size() == 2 && (*items)[0].tags[0] == "Adventure" && (*items)[0].tags[1] == "Singleplayer");
  CHECK((*items)[0].screenshots.size() == 3);
  CHECK((*items)[0].screenshots[0] ==
        "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/620/ss_one.600x338.jpg?t=1");
  CHECK((*items)[0].screenshots[1] ==
        "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/620/ss_two.600x338.jpg?t=1");
  CHECK((*items)[0].screenshots[2] ==
        "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/620/ss_sized.1920x1080.jpg?t=1");
  CHECK((*items)[0].trailer_thumbnail ==
        "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/5787/movie.293x165.jpg?t=5");
  CHECK((*items)[0].trailer_url ==
        "https://video.akamai.steamstatic.com/store_trailers/620/3746/clip.mp4?t=5");
  CHECK((*items)[0].description_known && (*items)[0].description == "A puzzle-platformer.");
  CHECK((*items)[0].controller_support == "full");
  CHECK(!(*items)[0].genres_known);
  const char* partial = R"({"response":{"store_items":[{"appid":1,"categories":{"controller_categoryids":[18]}}]}})";
  const auto partial_items = ParseStoreBrowseItems(partial, tags);
  CHECK(partial_items && partial_items->size() == 1 && (*partial_items)[0].controller_support == "partial" &&
        !(*partial_items)[0].description_known);
  const auto unnamed = ParseStoreBrowseItems(kItems, {});
  CHECK(unnamed && unnamed->size() == 1 && !(*unnamed)[0].tags_known);
  CHECK(!ParseStoreBrowseItems("not json", tags));
}

struct Script {
  std::mutex mutex;
  std::vector<std::string> items;
  std::atomic<int> item_calls{0};
  std::atomic<bool> release{false};
  std::optional<std::string> operator()(const std::string& url, const std::atomic<bool>& cancel) {
    if (url.find("GetTagList") != std::string::npos) return std::string(kTags);
    if (url.find("GetItems") == std::string::npos) return std::nullopt;
    const int call = ++item_calls;
    {
      std::lock_guard<std::mutex> lock(mutex);
      items.push_back(url);
    }
    if (call == 1) {
      while (!release.load() && !cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ItemsFor(url);
  }
};

void WaitItems(Script& script, int count) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (script.item_calls.load() < count && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(script.item_calls.load() >= count);
}

std::vector<StoreBrowseItem> WaitResults(StoreBrowseService& service, std::size_t count) {
  std::vector<StoreBrowseItem> got;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (got.size() < count && std::chrono::steady_clock::now() < deadline) {
    for (auto& item : service.Pump(8)) got.push_back(std::move(item));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(got.size() >= count);
  return got;
}

void OpenGameIsNextAndAFreshFileSendsNothing() {
  const auto root = std::filesystem::temp_directory_path() / "playden-store-browse-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  playden::Storage storage(root.string());
  CHECK(storage.EnsureRootDirectory().ok());
  Script script;
  const auto now = std::make_shared<std::atomic<std::uint64_t>>(1700000000);
  {
    StoreBrowseService service([&](const std::string& url, const std::atomic<bool>& cancel) { return script(url, cancel); },
                               std::chrono::milliseconds(0));
    service.SetStorage(&storage);
    service.SetNow([now] { return now->load(); });
    service.Request(GameId::Steam(11));
    WaitItems(script, 1);
    service.Request(GameId::Steam(12));
    service.Request(GameId::Steam(13));
    service.RequestFirst(GameId::Steam(14));
    script.release = true;
    const auto got = WaitResults(service, 4);
    CHECK(got[0].app == 11 && got[1].app == 14 && got[2].app == 12 && got[3].app == 13);
    CHECK(got[0].tags_known && got[0].tags.size() == 1 && got[0].tags[0] == "Adventure");
    std::lock_guard<std::mutex> lock(script.mutex);
    CHECK(script.items.size() >= 2);
    const auto first = script.items[1].find("appid%22%3A14");
    const auto second = script.items[1].find("appid%22%3A12");
    CHECK(first != std::string::npos && second != std::string::npos && first < second);
  }
  Script quiet;
  {
    StoreBrowseService service([&](const std::string& url, const std::atomic<bool>& cancel) { return quiet(url, cancel); },
                               std::chrono::milliseconds(0));
    service.SetStorage(&storage);
    service.SetNow([now] { return now->load(); });
    service.RequestFirst(GameId::Steam(14));
    const auto got = WaitResults(service, 1);
    CHECK(got[0].app == 14 && got[0].tags_known && quiet.item_calls.load() == 0);
    now->store(1700000000ull + kStoreBrowseTtlSeconds);
    service.Request(GameId::Steam(11));
    WaitItems(quiet, 1);
  }
  std::filesystem::remove_all(root);
}

void AppendNumber(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    out.push_back(static_cast<std::uint8_t>(value));
    value >>= 8;
  }
}

void AppendString(std::vector<std::uint8_t>& out, const std::string& value) {
  AppendNumber(out, value.size(), 2);
  out.insert(out.end(), value.begin(), value.end());
}

std::uint32_t Checksum(const std::vector<std::uint8_t>& bytes) {
  std::uint32_t crc = 0xffffffffu;
  for (const auto byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// A version 1 record: tags and a trailer slot, and no description or genres.
std::vector<std::uint8_t> VersionOneRecord() {
  std::vector<std::uint8_t> out = {'P', 'D', 'N', 'S', 'T', 'B', 0, 1};
  AppendNumber(out, 1700000000, 8);
  AppendNumber(out, 1, 4);
  AppendNumber(out, 21, 4);
  AppendString(out, "Adventure");
  AppendNumber(out, 1, 4);
  AppendNumber(out, 42, 4);
  AppendNumber(out, 1700000000, 8);
  AppendNumber(out, 1, 1);
  AppendNumber(out, 1, 1);
  AppendString(out, "Adventure");
  AppendNumber(out, 0, 1);
  AppendString(out, "");
  AppendString(out, "");
  AppendNumber(out, Checksum(out), 4);
  return out;
}

void VersionOneStaysFreshUntilGenresArePatched() {
  const auto root = std::filesystem::temp_directory_path() / "playden-store-browse-details";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  playden::Storage storage(root.string());
  CHECK(storage.EnsureRootDirectory().ok());
  CHECK(storage.WriteAtomic("store-browse.cache", VersionOneRecord()).ok());
  Script quiet;
  const auto now = std::make_shared<std::atomic<std::uint64_t>>(1700000100);
  {
    StoreBrowseService service([&](const std::string& url, const std::atomic<bool>& cancel) { return quiet(url, cancel); },
                               std::chrono::milliseconds(0));
    service.SetStorage(&storage);
    service.SetNow([now] { return now->load(); });
    service.Request(GameId::Steam(42));
    const auto got = WaitResults(service, 1);
    CHECK(got[0].app == 42 && got[0].tags_known && got[0].tags.size() == 1 && got[0].tags[0] == "Adventure");
    CHECK(!got[0].description_known && !got[0].genres_known && quiet.item_calls.load() == 0);
    service.RememberDetails(GameId::Steam(42), "A short description.", {"Puzzle"}, "partial");
    const auto patched = service.Cached(GameId::Steam(42));
    CHECK(patched && patched->description == "A short description." && patched->controller_support == "partial");
    CHECK(patched->genres_known && patched->genres.size() == 1 && patched->genres[0] == "Puzzle");
    CHECK(patched->tags_known && patched->tags.size() == 1 && patched->tags[0] == "Adventure");
    CHECK(quiet.item_calls.load() == 0);
  }
  Script again;
  {
    StoreBrowseService service([&](const std::string& url, const std::atomic<bool>& cancel) { return again(url, cancel); },
                               std::chrono::milliseconds(0));
    service.SetStorage(&storage);
    service.SetNow([now] { return now->load(); });
    const auto cached = service.Cached(GameId::Steam(42));
    CHECK(cached && cached->description_known && cached->genres_known);
    CHECK(cached->description == "A short description." && cached->genres.size() == 1 && cached->genres[0] == "Puzzle");
    CHECK(cached->tags.size() == 1 && cached->tags[0] == "Adventure");
    service.Request(GameId::Steam(42));
    const auto got = WaitResults(service, 1);
    CHECK(got[0].description == "A short description." && got[0].genres_known && got[0].genres.size() == 1);
    CHECK(again.item_calls.load() == 0);
  }
  std::filesystem::remove_all(root);
}

std::vector<std::uint8_t> VersionThreeShot(bool sized) {
  std::vector<std::uint8_t> out = {'P', 'D', 'N', 'S', 'T', 'B', 0, 3};
  AppendNumber(out, 1700000000, 8);
  AppendNumber(out, 0, 4);
  AppendNumber(out, 1, 4);
  AppendNumber(out, 77, 4);
  AppendNumber(out, 1700000000, 8);
  AppendNumber(out, 1, 1);
  AppendNumber(out, 0, 1);
  AppendNumber(out, 1, 1);
  AppendString(out, sized ? "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/77/ss.600x338.jpg"
                          : "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/77/ss.jpg");
  AppendString(out, "");
  AppendString(out, "");
  AppendNumber(out, 1, 1);
  AppendString(out, "Kept.");
  AppendString(out, "");
  AppendNumber(out, 1, 1);
  AppendNumber(out, 0, 1);
  AppendNumber(out, Checksum(out), 4);
  return out;
}

void UnsizedShotsAreFetchedAgain() {
  const auto now = std::make_shared<std::atomic<std::uint64_t>>(1700000100);
  const auto check = [&](bool sized, bool fetches) {
    const auto root = std::filesystem::temp_directory_path() / (sized ? "playden-store-browse-sized" : "playden-store-browse-unsized");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    playden::Storage storage(root.string());
    CHECK(storage.EnsureRootDirectory().ok());
    CHECK(storage.WriteAtomic("store-browse.cache", VersionThreeShot(sized)).ok());
    Script quiet;
    StoreBrowseService service([&](const std::string& url, const std::atomic<bool>& cancel) { return quiet(url, cancel); },
                               std::chrono::milliseconds(0));
    service.SetStorage(&storage);
    service.SetNow([now] { return now->load(); });
    service.Request(GameId::Steam(77));
    if (fetches) {
      WaitItems(quiet, 1);
      CHECK(quiet.item_calls.load() >= 1);
    } else {
      const auto got = WaitResults(service, 1);
      CHECK(got[0].app == 77 && got[0].description == "Kept." && quiet.item_calls.load() == 0);
      CHECK(got[0].screenshots.size() == 1 && got[0].screenshots[0].find("600x338") != std::string::npos);
    }
    std::filesystem::remove_all(root);
  };
  check(false, true);
  check(true, false);
}

void TwentyTagsAreKept() {
  std::string tags = R"({"response":{"tags":[)";
  std::string item = R"({"response":{"store_items":[{"appid":7,"tags":[)";
  for (int id = 1; id <= 20; ++id) {
    if (id > 1) {
      tags += ',';
      item += ',';
    }
    tags += "{\"tagid\":" + std::to_string(id) + ",\"name\":\"Tag" + std::to_string(id) + "\"}";
    item += "{\"tagid\":" + std::to_string(id) + "}";
  }
  tags += "]}}";
  item += "]}]}}";
  const auto names = ParseStoreTagList(tags);
  const auto items = ParseStoreBrowseItems(item, names);
  CHECK(items && items->size() == 1 && (*items)[0].tags.size() == 20 && (*items)[0].tags.back() == "Tag20");
}

bool AppInUrl(const std::string& url, const char* id) {
  return url.find(std::string("appid%22%3A") + id + "%7D") != std::string::npos;
}

void AFailedBatchSplitsAndAnOmittedAppIsStored() {
  const auto root = std::filesystem::temp_directory_path() / "playden-store-browse-chunks";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  playden::Storage storage(root.string());
  CHECK(storage.EnsureRootDirectory().ok());
  std::mutex mutex;
  std::vector<std::string> items;
  std::atomic<int> calls{0};
  std::atomic<bool> hold_tags{true};
  std::atomic<bool> tags_seen{false};
  const auto now = std::make_shared<std::atomic<std::uint64_t>>(1700000000);
  {
    StoreBrowseService service(
        [&](const std::string& url, const std::atomic<bool>& cancel) -> std::optional<std::string> {
          if (url.find("GetTagList") != std::string::npos) {
            tags_seen = true;
            while (hold_tags.load() && !cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::string(kTags);
          }
          if (url.find("GetItems") == std::string::npos) return std::nullopt;
          ++calls;
          {
            std::lock_guard<std::mutex> lock(mutex);
            items.push_back(url);
          }
          if (AppInUrl(url, "31") && AppInUrl(url, "32")) return std::nullopt;
          if (AppInUrl(url, "41") && AppInUrl(url, "42"))
            return std::string(R"({"response":{"store_items":[{"appid":41,"tags":[{"tagid":21}]}]}})");
          if (AppInUrl(url, "31")) return std::string(R"({"response":{"store_items":[{"appid":31,"tags":[{"tagid":21}]}]}})");
          if (AppInUrl(url, "32")) return std::string(R"({"response":{"store_items":[{"appid":32,"tags":[{"tagid":21}]}]}})");
          return ItemsFor(url);
        },
        std::chrono::milliseconds(0));
    service.SetStorage(&storage);
    service.SetNow([now] { return now->load(); });
    service.Request(GameId::Steam(11));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!tags_seen.load() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(tags_seen.load());
    service.Request(GameId::Steam(31));
    service.Request(GameId::Steam(32));
    service.Request(GameId::Steam(41));
    service.Request(GameId::Steam(42));
    hold_tags = false;
    std::vector<StoreBrowseItem> got;
    const auto results_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < results_deadline) {
      for (auto& item : service.Pump(8)) got.push_back(std::move(item));
      if (std::any_of(got.begin(), got.end(), [](const StoreBrowseItem& item) { return item.app == 42; })) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto find_app = [&](std::uint32_t app) {
      return std::find_if(got.begin(), got.end(), [app](const StoreBrowseItem& item) { return item.app == app; });
    };
    const auto app31 = find_app(31);
    const auto app32 = find_app(32);
    const auto app42 = find_app(42);
    CHECK(app31 != got.end() && app31->tags_known && app31->tags.size() == 1 && app31->tags[0] == "Adventure");
    CHECK(app32 != got.end() && app32->tags_known && app32->tags.size() == 1);
    CHECK(app42 != got.end() && app42->tags_known && app42->tags.empty());
    std::lock_guard<std::mutex> lock(mutex);
    const auto both = std::find_if(items.begin(), items.end(), [](const std::string& url) {
      return AppInUrl(url, "31") && AppInUrl(url, "32");
    });
    CHECK(both != items.end());
    const auto alone = std::find_if(both + 1, items.end(), [](const std::string& url) {
      return AppInUrl(url, "31") && !AppInUrl(url, "32");
    });
    CHECK(alone != items.end());
  }
  const int before = calls.load();
  {
    StoreBrowseService service(
        [&](const std::string& url, const std::atomic<bool>&) -> std::optional<std::string> {
          if (url.find("GetTagList") != std::string::npos) return std::string(kTags);
          if (url.find("GetItems") == std::string::npos) return std::nullopt;
          ++calls;
          return ItemsFor(url);
        },
        std::chrono::milliseconds(0));
    service.SetStorage(&storage);
    service.SetNow([now] { return now->load(); });
    service.Request(GameId::Steam(42));
    const auto got = WaitResults(service, 1);
    CHECK(got[0].app == 42 && got[0].tags_known && got[0].tags.empty());
    CHECK(calls.load() == before);
  }
  std::filesystem::remove_all(root);
}

void AFullQueueDropsTheNextRequest() {
  std::atomic<bool> hold{true};
  std::atomic<bool> seen{false};
  std::mutex mutex;
  std::vector<std::string> urls;
  StoreBrowseService service(
      [&](const std::string& url, const std::atomic<bool>& cancel) -> std::optional<std::string> {
        if (url.find("GetTagList") != std::string::npos) {
          seen = true;
          while (hold.load() && !cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
          return std::string(kTags);
        }
        if (url.find("GetItems") == std::string::npos) return std::nullopt;
        {
          std::lock_guard<std::mutex> lock(mutex);
          urls.push_back(url);
        }
        return std::string(R"({"response":{"store_items":[]}})");
      },
      std::chrono::milliseconds(0));
  service.Request(GameId::Steam(11));
  const auto started = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!seen.load() && std::chrono::steady_clock::now() < started)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(seen.load());
  CHECK(!service.Full());
  for (std::uint32_t app = 2000; app < 2000 + StoreBrowseService::kMaxPending; ++app)
    service.Request(GameId::Steam(app));
  CHECK(service.Full());
  service.Request(GameId::Steam(29999));
  CHECK(service.Full());
  hold = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool saw_queued = false;
  while (std::chrono::steady_clock::now() < deadline && !saw_queued) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      saw_queued = std::any_of(urls.begin(), urls.end(), [](const std::string& url) { return AppInUrl(url, "2000"); });
    }
    if (!saw_queued) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::lock_guard<std::mutex> lock(mutex);
  CHECK(saw_queued);
  CHECK(std::none_of(urls.begin(), urls.end(), [](const std::string& url) { return AppInUrl(url, "29999"); }));
}

int main() {
  ParsedFixtureNamesShotsAndTrailer();
  OpenGameIsNextAndAFreshFileSendsNothing();
  VersionOneStaysFreshUntilGenresArePatched();
  UnsizedShotsAreFetchedAgain();
  TwentyTagsAreKept();
  AFailedBatchSplitsAndAnOmittedAppIsStored();
  AFullQueueDropsTheNextRequest();
  std::puts("store browse tests passed");
  return 0;
}

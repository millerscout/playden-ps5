// browse.hpp - Pure catalog projection and controller navigation helpers.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "homebrew/simulation.hpp"

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::homebrew {

// Continue playing shows at most this many games; the rest are a "More" cell that opens the Library.
inline constexpr std::size_t kShelfLimit = 20;

struct Shelf {
    std::string key;
    std::string title;
    std::vector<GameId> game_ids;
    int more = 0;  // games left out; above 0, a "More" cell follows the last game
};

// Focusable cells in a shelf: its games, plus the "More" cell when games were left out.
[[nodiscard]] inline int shelf_cells(const Shelf& shelf) noexcept {
    return static_cast<int>(shelf.game_ids.size()) + (shelf.more > 0 ? 1 : 0);
}
[[nodiscard]] inline std::string shelf_more_key(const Shelf& shelf) { return "shelf-" + shelf.key + "-more"; }

struct ShelfWindow {
    int first = 0;
    int count = 0;
};

[[nodiscard]] std::vector<const Game*> filtered_games(const State& state);
[[nodiscard]] std::vector<Shelf> shelves(const State& state);
[[nodiscard]] ShelfWindow shelf_window(int item_count, int focused_item,
                                       int capacity = 7) noexcept;
[[nodiscard]] std::vector<std::string> library_rail_scopes(const State& state);
// The library has games imported from a drive or the drop folder: the rail shows "Imported".
[[nodiscard]] bool has_imported_games(const State& state);

// Moves only Home and Library browse focus. Returning true means the route
// consumed the direction even when focus was already against an edge.
bool move_browser(State& state, int delta_x, int delta_y);

// Details shows this many tags, genres first. The rest are a "+N" chip that lists them all.
inline constexpr std::size_t kDetailTagPreview = 6;

// One still on the game page: the trailer poster first, then up to four screenshots.
struct DetailStill {
    std::string url;
    bool trailer = false;
};

[[nodiscard]] inline std::vector<DetailStill> detail_stills(const Game& game) {
    std::vector<DetailStill> stills;
    if (!game.store_trailer_thumbnail.empty()) stills.push_back({game.store_trailer_thumbnail, true});
    std::size_t shots = 0;
    for (const auto& url : game.store_screenshots) {
        if (url.empty() || shots >= 4) continue;
        stills.push_back({url, false});
        ++shots;
    }
    return stills;
}

// A Steam CDN mp4. The same hosts as store stills; anything else is not fetched.
[[nodiscard]] inline bool SteamTrailerUrl(std::string_view source) noexcept {
    constexpr std::string_view kHttps = "https://";
    if (source.size() <= kHttps.size() || source.substr(0, kHttps.size()) != kHttps) return false;
    if (source.find('\\') != std::string_view::npos || source.find('@') != std::string_view::npos ||
        source.find('#') != std::string_view::npos)
        return false;
    const auto path_at = source.find('/', kHttps.size());
    if (path_at == std::string_view::npos || path_at == kHttps.size()) return false;
    const auto host = source.substr(kHttps.size(), path_at - kHttps.size());
    bool host_ok = false;
    const bool video_host = host == "video.akamai.steamstatic.com";
    for (const char* name : {"cdn.cloudflare.steamstatic.com", "shared.cloudflare.steamstatic.com",
                             "cdn.akamai.steamstatic.com", "shared.akamai.steamstatic.com",
                             "steamcdn-a.akamaihd.net", "video.akamai.steamstatic.com"})
        if (host == name) host_ok = true;
    if (!host_ok || host.find(':') != std::string_view::npos) return false;
    const auto rest = source.substr(path_at);
    const auto query_at = rest.find('?');
    const auto path = rest.substr(0, query_at);
    if (query_at != std::string_view::npos) {
        const auto query = rest.substr(query_at);
        if (query.size() < 4 || query.substr(0, 3) != "?t=") return false;
        for (std::size_t index = 3; index < query.size(); ++index)
            if (query[index] < '0' || query[index] > '9') return false;
    }
    if (path.find("..") != std::string_view::npos || path.find("//") != std::string_view::npos) return false;
    constexpr std::string_view kAssets = "/store_item_assets/";
    constexpr std::string_view kApps = "/steam/apps/";
    constexpr std::string_view kTrailers = "/store_trailers/";
    const bool assets = path.size() > kAssets.size() && path.substr(0, kAssets.size()) == kAssets;
    const bool apps = path.size() > kApps.size() && path.substr(0, kApps.size()) == kApps;
    const bool trailers = path.size() > kTrailers.size() && path.substr(0, kTrailers.size()) == kTrailers;
    if (video_host) {
        if (!trailers) return false;
    } else if (!assets && !apps) {
        return false;
    }
    return path.size() >= 4 && path.substr(path.size() - 4) == ".mp4";
}

// Older pages stored the microtrailer on the image CDN, where that file 404s.
// The same path on the trailer host is the file Steam serves.
[[nodiscard]] inline std::string PlayableTrailerUrl(std::string_view source) {
    constexpr std::string_view kImageBase = "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/";
    constexpr std::string_view kVideoBase = "https://video.akamai.steamstatic.com/store_trailers/";
    std::string url(source);
    if (url.rfind(kImageBase, 0) == 0 && url.find(".mp4") != std::string::npos)
        url = std::string(kVideoBase) + url.substr(kImageBase.size());
    return SteamTrailerUrl(url) ? url : std::string();
}

[[nodiscard]] inline std::optional<std::size_t> DetailMediaIndex(std::string_view key) noexcept {
    constexpr std::string_view kPrefix = "details-media-";
    if (key.size() <= kPrefix.size() || key.compare(0, kPrefix.size(), kPrefix) != 0) return std::nullopt;
    std::size_t index = 0;
    const auto parsed = std::from_chars(key.data() + kPrefix.size(), key.data() + key.size(), index);
    if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size()) return std::nullopt;
    return index;
}

// A filmstrip cell. The index is its place in detail_stills.

[[nodiscard]] inline bool DetailMediaKey(std::string_view key) noexcept {
    constexpr std::string_view kPrefix = "details-media-";
    if (key.size() <= kPrefix.size() || key.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    for (std::size_t index = kPrefix.size(); index < key.size(); ++index)
        if (key[index] < '0' || key[index] > '9') return false;
    return true;
}

// A focusable chip on a game's page: one tag, or the "+N" that opens the rest.
[[nodiscard]] inline bool DetailChipKey(std::string_view key) noexcept {
    constexpr std::string_view kPrefix = "detail-tag-";
    return key == "detail-tags-more" ||
           (key.size() > kPrefix.size() && key.compare(0, kPrefix.size(), kPrefix) == 0);
}

// A ticked library tag, drawn as a chip. The index follows the saved set's order.
[[nodiscard]] inline std::string SelectedTagKey(std::size_t index) {
    return "selected-tag-" + std::to_string(index);
}
[[nodiscard]] inline bool SelectedTagIndex(std::string_view key, std::size_t& index) noexcept {
    constexpr std::string_view kPrefix = "selected-tag-";
    if (key.size() <= kPrefix.size() || key.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    const char* const begin = key.data() + kPrefix.size();
    const auto parsed = std::from_chars(begin, key.data() + key.size(), index);
    return parsed.ec == std::errc{} && parsed.ptr == key.data() + key.size();
}

[[nodiscard]] std::size_t previous_caret(std::string_view utf8, std::size_t caret) noexcept;
[[nodiscard]] std::size_t next_caret(std::string_view utf8, std::size_t caret) noexcept;

} // namespace playden::homebrew

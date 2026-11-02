#include "steam/store_details_service.h"

#include "steam/backend/json.h"

#include <algorithm>
#include <utility>

namespace playden::steam {
namespace {

constexpr std::size_t kMaximumTags = 32;
constexpr std::size_t kMaximumTagBytes = 64;
constexpr std::size_t kMaximumShots = 4;
constexpr std::size_t kMaximumUrlBytes = 512;
constexpr std::size_t kMaximumDeveloperBytes = 128;
constexpr std::size_t kMaximumReleaseBytes = 64;

bool ValidUrl(std::string_view url) {
  return url.size() <= kMaximumUrlBytes && url.rfind("https://", 0) == 0 && url.find('\0') == std::string_view::npos;
}

bool HasSizeToken(std::string_view url) {
  const auto query = url.find('?');
  const auto path = url.substr(0, query);
  for (std::size_t start = 0; start < path.size(); ++start) {
    if (path[start] < '0' || path[start] > '9') continue;
    std::size_t width = start;
    while (width < path.size() && path[width] >= '0' && path[width] <= '9') ++width;
    if (width == start || width >= path.size() || path[width] != 'x') continue;
    std::size_t height = width + 1;
    if (height >= path.size() || path[height] < '0' || path[height] > '9') continue;
    while (height < path.size() && path[height] >= '0' && path[height] <= '9') ++height;
    if (height > width + 1) return true;
  }
  return false;
}

bool JpegUrl(std::string_view url) {
  const auto query = url.find('?');
  const auto path = url.substr(0, query);
  auto ends = [&](std::string_view extension) {
    if (path.size() < extension.size()) return false;
    for (std::size_t index = 0; index < extension.size(); ++index) {
      char letter = path[path.size() - extension.size() + index];
      if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter - 'A' + 'a');
      if (letter != extension[index]) return false;
    }
    return true;
  };
  return ends(".jpg") || ends(".jpeg");
}

std::string KeepUrl(std::string url) {
  return ValidUrl(url) ? std::move(url) : std::string();
}

bool ValidTag(std::string_view tag) {
  return !tag.empty() && tag.size() <= kMaximumTagBytes && tag.find('\0') == std::string_view::npos;
}

// The store writes HTML: tags go, common entities become their characters, and runs
// of whitespace collapse, as the macOS app shows it.
std::string PlainText(std::string_view html) {
  std::string text;
  text.reserve(html.size());
  bool tag = false;
  for (const char c : html) {
    if (c == '<') { tag = true; text += ' '; continue; }
    if (c == '>') { tag = false; continue; }
    if (!tag) text += c;
  }
  static const std::pair<const char*, const char*> kEntities[] = {
      {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}, {"&amp;", "&"}};
  for (const auto& [entity, value] : kEntities) {
    const std::string_view from(entity);
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + 1)) text.replace(at, from.size(), value);
  }
  std::string collapsed;
  collapsed.reserve(text.size());
  for (const char c : text) {
    const bool space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
    if (space) {
      if (!collapsed.empty() && collapsed.back() != ' ') collapsed += ' ';
    } else {
      collapsed += c;
    }
  }
  while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
  return collapsed;
}

std::string ShortIdentity(std::string text, std::size_t maximum) {
  text = PlainText(text);
  if (text.find('\0') != std::string::npos || text.size() > maximum) return {};
  return text;
}

void AppendTag(std::vector<std::string>& tags, std::string value) {
  if (!ValidTag(value)) return;
  value = PlainText(value);
  if (!ValidTag(value)) return;
  if (tags.size() >= kMaximumTags) return;
  if (std::find(tags.begin(), tags.end(), value) != tags.end()) return;
  tags.push_back(std::move(value));
}

void ParseTagsField(const json::Value* field, std::vector<std::string>& tags) {
  if (!field) return;
  if (field->is_array()) {
    for (const auto& entry : field->array()) {
      if (entry.is_string()) AppendTag(tags, entry.text());
      else if (const auto* name = entry.Find("description"); name && name->is_string()) AppendTag(tags, name->text());
      else if (const auto* name = entry.Find("localizetag"); name && name->is_string()) AppendTag(tags, name->text());
    }
    return;
  }
  if (field->is_string()) AppendTag(tags, field->text());
}

}  // namespace

std::string StoreDetailsUrl(std::uint32_t app) {
  return "https://store.steampowered.com/api/appdetails?appids=" + std::to_string(app) + "&l=english";
}

std::optional<StoreDetails> ParseStoreDetails(std::uint32_t id, std::string_view body) {
  const auto root = json::Parse(body, {2 * 1024 * 1024, 32, 1 << 17, 512 * 1024});
  if (!root || !root->is_object()) return std::nullopt;
  const auto* entry = root->Find(std::to_string(id));
  if (!entry || !entry->is_object()) return std::nullopt;
  const auto* success = entry->Find("success");
  const auto* data = entry->Find("data");
  if (!success || !success->is_bool() || !success->boolean() || !data || !data->is_object()) return std::nullopt;
  const auto* app = data->Find("steam_appid");
  if (!app || app->AsUint64() != std::optional<std::uint64_t>(id)) return std::nullopt;
  StoreDetails details;
  details.id = GameId::Steam(id);
  if (const auto* text = data->Find("short_description"); text && text->is_string())
    details.description = PlainText(text->text());
  if (const auto* genres = data->Find("genres"); genres && genres->is_array())
    for (const auto& genre : genres->array())
      if (const auto* name = genre.Find("description"); name && name->is_string() && !name->text().empty())
        details.genres.push_back(PlainText(name->text()));
  if (const auto* support = data->Find("controller_support"); support && support->is_string())
    details.controller_support = support->text();
  ParseTagsField(data->Find("tags"), details.tags);
  ParseTagsField(data->Find("tag"), details.tags);
  if (const auto* shots = data->Find("screenshots"); shots && shots->is_array()) {
    for (const auto& shot : shots->array()) {
      if (details.screenshots.size() >= kMaximumShots) break;
      std::string url;
      if (const auto* thumb = shot.Find("path_thumbnail"); thumb && thumb->is_string()) url = KeepUrl(thumb->text());
      if (url.empty()) {
        if (const auto* full = shot.Find("path_full"); full && full->is_string() && JpegUrl(full->text()) && HasSizeToken(full->text()))
          url = KeepUrl(full->text());
      }
      if (!url.empty()) details.screenshots.push_back(std::move(url));
    }
  }
  const json::Value* movie = nullptr;
  if (const auto* movies = data->Find("movies"); movies && movies->is_array()) {
    for (const auto& entry : movies->array()) {
      const auto* highlight = entry.Find("highlight");
      if (highlight && highlight->is_bool() && highlight->boolean()) {
        movie = &entry;
        break;
      }
      if (!movie) movie = &entry;
    }
  }
  if (movie) {
    if (const auto* thumb = movie->Find("thumbnail"); thumb && thumb->is_string())
      details.trailer_thumbnail = KeepUrl(thumb->text());
    const auto* mp4 = movie->Find("mp4");
    const auto* webm = movie->Find("webm");
    const json::Value* file = nullptr;
    if (mp4 && mp4->is_object()) file = mp4->Find("480");
    if ((!file || !file->is_string()) && webm && webm->is_object()) file = webm->Find("480");
    if (file && file->is_string()) details.trailer_url = KeepUrl(file->text());
  }
  if (const auto* developers = data->Find("developers"); developers && developers->is_array() && !developers->array().empty()) {
    if (const auto& first = developers->array().front(); first.is_string())
      details.developer = ShortIdentity(first.text(), kMaximumDeveloperBytes);
  }
  if (const auto* released = data->Find("release_date"); released && released->is_object()) {
    if (const auto* date = released->Find("date"); date && date->is_string())
      details.release_date = ShortIdentity(date->text(), kMaximumReleaseBytes);
  }
  return details;
}

StoreDetailsService::StoreDetailsService(Fetch fetch) : fetch_(std::move(fetch)) {
  worker_ = std::thread([this] { Run(); });
}

StoreDetailsService::~StoreDetailsService() { Shutdown(); }

void StoreDetailsService::Request(GameId id) {
  if (!id.steam_app()) return;  // Steam games only
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || pending_.size() >= kMaxPending || !requested_.insert(id).second) return;
    pending_.push_back(id);
  }
  wake_.notify_one();
}

std::vector<StoreDetails> StoreDetailsService::Pump() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StoreDetails> out(std::make_move_iterator(results_.begin()), std::make_move_iterator(results_.end()));
  results_.clear();
  return out;
}

void StoreDetailsService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cancel_.store(true);
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void StoreDetailsService::Run() {
  for (;;) {
    GameId id;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (stopping_) return;
      id = pending_.front();
      pending_.pop_front();
    }
    const auto app = *id.steam_app();
    const auto body = fetch_ ? fetch_(StoreDetailsUrl(app), cancel_) : std::nullopt;
    auto details = body ? ParseStoreDetails(app, *body) : std::nullopt;
    if (!details) continue;
    std::lock_guard<std::mutex> lock(mutex_);
    results_.push_back(std::move(*details));
  }
}

}  // namespace playden::steam

#include "homebrew/artwork_backend.hpp"

#include "platform/managed_directory.h"
#include "platform/ps5/title_allow_list.h"
#include "steam/backend/http_client.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cerrno>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct DecodeBudget {
  std::size_t limit = 0;
  std::size_t outstanding = 0;
};

struct alignas(std::max_align_t) AllocationHeader {
  std::size_t size = 0;
  DecodeBudget* owner = nullptr;
};

thread_local DecodeBudget* g_decode_budget = nullptr;

void* ArtworkAllocate(std::size_t size) {
  DecodeBudget* budget = g_decode_budget;
  if (!budget || size > budget->limit -
                            std::min(budget->outstanding, budget->limit) ||
      size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader))
    return nullptr;
  auto* header = static_cast<AllocationHeader*>(
      std::malloc(sizeof(AllocationHeader) + size));
  if (!header) return nullptr;
  header->size = size;
  header->owner = budget;
  budget->outstanding += size;
  return header + 1;
}

void ArtworkFree(void* allocation) {
  if (!allocation) return;
  auto* header = static_cast<AllocationHeader*>(allocation) - 1;
  if (header->owner)
    header->owner->outstanding -=
        std::min(header->size, header->owner->outstanding);
  std::free(header);
}

void* ArtworkReallocate(void* allocation, std::size_t size) {
  if (!allocation) return ArtworkAllocate(size);
  if (size == 0) {
    ArtworkFree(allocation);
    return nullptr;
  }
  auto* old_header = static_cast<AllocationHeader*>(allocation) - 1;
  DecodeBudget* budget = old_header->owner;
  if (!budget ||
      (size > old_header->size &&
       size - old_header->size >
           budget->limit - std::min(budget->outstanding, budget->limit)) ||
      size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader))
    return nullptr;
  auto* header = static_cast<AllocationHeader*>(
      std::realloc(old_header, sizeof(AllocationHeader) + size));
  if (!header) return nullptr;
  if (size >= header->size) budget->outstanding += size - header->size;
  else budget->outstanding -= header->size - size;
  header->size = size;
  header->owner = budget;
  return header + 1;
}

class DecodeBudgetScope {
 public:
  explicit DecodeBudgetScope(std::size_t limit)
      : prior_(g_decode_budget), budget_{limit, 0} {
    g_decode_budget = &budget_;
  }
  ~DecodeBudgetScope() { g_decode_budget = prior_; }

 private:
  DecodeBudget* prior_;
  DecodeBudget budget_;
};

}  // namespace

#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MALLOC(size) ArtworkAllocate(size)
#define STBI_REALLOC(pointer, size) ArtworkReallocate(pointer, size)
#define STBI_FREE(pointer) ArtworkFree(pointer)
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace playden::homebrew {
namespace {

constexpr std::uint32_t kArtworkTimeoutMs = 10'000;
// Cached artwork expires after 14 days so updated Steam covers refresh.
// Cover JPEG/PNG bytes only; store tags live in the library cache (schema 8), not here.
constexpr std::time_t kArtworkCacheTtlSeconds = 14 * 24 * 3600;

bool CheckedImageBytes(int width, int height, std::size_t& bytes) {
  if (width <= 0 || height <= 0 ||
      width > static_cast<int>(kMaxArtworkDimension) ||
      height > static_cast<int>(kMaxArtworkDimension)) return false;
  const std::uint64_t value = static_cast<std::uint64_t>(width) *
                              static_cast<std::uint64_t>(height) * 4u;
  if (value > kMaxArtworkImageBytes ||
      value > std::numeric_limits<std::size_t>::max()) return false;
  bytes = static_cast<std::size_t>(value);
  return true;
}

bool SteamStaticHost(std::string_view host) {
  for (const char* name : {"cdn.cloudflare.steamstatic.com", "shared.cloudflare.steamstatic.com",
                           "cdn.akamai.steamstatic.com", "shared.akamai.steamstatic.com",
                           "steamcdn-a.akamaihd.net"})
    if (host == name) return true;
  return false;
}

// A store screenshot or trailer poster. Cover, hero, logo, and header stay on the exact-match path.
bool StoreStillUrl(std::string_view source) {
  constexpr std::string_view kHttps = "https://";
  if (source.size() <= kHttps.size() || source.substr(0, kHttps.size()) != kHttps) return false;
  if (source.find('\\') != std::string_view::npos || source.find('@') != std::string_view::npos ||
      source.find('#') != std::string_view::npos)
    return false;
  const auto path_at = source.find('/', kHttps.size());
  if (path_at == std::string_view::npos || path_at == kHttps.size()) return false;
  const auto host = source.substr(kHttps.size(), path_at - kHttps.size());
  if (host.find(':') != std::string_view::npos || !SteamStaticHost(host)) return false;

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
  const bool assets = path.size() > kAssets.size() && path.substr(0, kAssets.size()) == kAssets;
  const bool apps = path.size() > kApps.size() && path.substr(0, kApps.size()) == kApps;
  if (!assets && !apps) return false;
  if (apps) {
    std::size_t index = kApps.size();
    if (path[index] < '0' || path[index] > '9') return false;
    while (index < path.size() && path[index] >= '0' && path[index] <= '9') ++index;
    if (index >= path.size() || path[index] != '/') return false;
  }

  const auto slash = path.rfind('/');
  const auto file = slash == std::string_view::npos ? path : path.substr(slash + 1);
  if (file == "library_600x900.jpg" || file == "library_hero.jpg" || file == "logo.png" ||
      file == "header.jpg")
    return false;
  auto extension = [&](std::string_view suffix) {
    return file.size() > suffix.size() && file.substr(file.size() - suffix.size()) == suffix;
  };
  return extension(".jpg") || extension(".jpeg") || extension(".png");
}

std::string StillCacheName(const ArtworkKey& key) {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char byte : key.source) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
  const auto query = key.source.find('?');
  const auto path = query == std::string::npos ? std::string_view(key.source)
                                                : std::string_view(key.source).substr(0, query);
  const char* extension = ".jpg";
  if (path.size() >= 5 && path.substr(path.size() - 5) == ".jpeg") extension = ".jpeg";
  else if (path.size() >= 4 && path.substr(path.size() - 4) == ".png") extension = ".png";
  return ToKey(key.game_id) + "_" + hex + extension;
}

// The game's cover, hero, and logo, plus Steam CDN screenshots and trailer posters.
bool CanonicalKey(const ArtworkKey& key) {
  if (key.account_generation == 0 || !key.game_id.valid() ||
      key.source.size() > kMaxArtworkSourceBytes) return false;
  const std::string base = "https://cdn.cloudflare.steamstatic.com/steam/apps/" +
                           ToKey(key.game_id) + "/";
  for (const char* name : {"library_600x900.jpg", "library_hero.jpg", "logo.png"})
    if (key.source == base + name) return true;
  return StoreStillUrl(key.source);
}

std::string CacheFileName(const ArtworkKey& key) {
  if (key.account_generation == 0 || !key.game_id.valid() || key.source.empty()) return {};
  const std::string id = ToKey(key.game_id);
  if (id.empty()) return {};
  if (StoreStillUrl(key.source)) return StillCacheName(key);
  if (key.source.find("library_600x900.jpg") != std::string::npos) return id + "_600x900.jpg";
  if (key.source.find("library_hero.jpg") != std::string::npos) return id + "_hero.jpg";
  if (key.source.find("logo.png") != std::string::npos) return id + "_logo.png";
  return {};
}

ArtworkImage FrameToPortrait(const ArtworkImage& src, std::uint32_t target_w = 600, std::uint32_t target_h = 900) {
  if (src.width == 0 || src.height == 0 || src.width <= src.height) return src;
  ArtworkImage out;
  out.width = target_w;
  out.height = target_h;
  out.rgba.resize(target_w * target_h * 4, 0);

  // At least one row, so a very wide image still shows its banner.
  const int banner_h = std::max(1, static_cast<int>(target_w * src.height / src.width));
  const int offset_y = static_cast<int>((target_h - banner_h) / 2);
  const int bottom_margin = static_cast<int>(target_h) - (offset_y + banner_h);

  for (std::uint32_t y = 0; y < target_h; ++y) {
    for (std::uint32_t x = 0; x < target_w; ++x) {
      const std::size_t out_idx = (y * target_w + x) * 4;
      if (static_cast<int>(y) >= offset_y && static_cast<int>(y) < offset_y + banner_h) {
        const std::uint32_t sx = std::min(x * src.width / target_w, src.width - 1);
        const std::uint32_t sy = std::min((y - offset_y) * src.height / banner_h, src.height - 1);
        const std::size_t in_idx = (sy * src.width + sx) * 4;
        out.rgba[out_idx] = src.rgba[in_idx];
        out.rgba[out_idx + 1] = src.rgba[in_idx + 1];
        out.rgba[out_idx + 2] = src.rgba[in_idx + 2];
        out.rgba[out_idx + 3] = 255;
      } else {
        const std::uint32_t sx = std::min(x * src.width / target_w, src.width - 1);
        const std::uint32_t sy = (static_cast<int>(y) < offset_y) ? 0 : (src.height - 1);
        const std::size_t in_idx = (sy * src.width + sx) * 4;
        const float factor = (static_cast<int>(y) < offset_y)
            ? (offset_y > 0 ? (0.4f * static_cast<float>(y) / static_cast<float>(offset_y)) : 0.0f)
            : (bottom_margin > 0 ? (0.4f * static_cast<float>(target_h - 1 - y) / static_cast<float>(bottom_margin)) : 0.0f);
        out.rgba[out_idx] = static_cast<std::uint8_t>(src.rgba[in_idx] * factor);
        out.rgba[out_idx + 1] = static_cast<std::uint8_t>(src.rgba[in_idx + 1] * factor);
        out.rgba[out_idx + 2] = static_cast<std::uint8_t>(src.rgba[in_idx + 2] * factor);
        out.rgba[out_idx + 3] = 255;
      }
    }
  }
  return out;
}

}  // namespace

struct HttpArtworkBackend::Cache {
  std::mutex mutex;
  std::optional<platform::ManagedDirectory> dir;

  // Only a save creates the folder, while it holds the effective UID.
  bool EnsureDir(const std::string& path, bool create) {
    if (dir && dir->Validate()) return true;
    if (path.empty()) return false;
    const std::filesystem::path target = std::filesystem::path(path).lexically_normal();
    dir = platform::ManagedDirectory::Open(target, platform::DirectoryPolicy::kPrivate);
    if (dir || !create) return dir.has_value();

    const auto parent_path = target.parent_path();
    const auto child_name = target.filename().string();
    auto parent = platform::ManagedDirectory::Open(parent_path, platform::DirectoryPolicy::kApprovedTree);
    if (!parent) parent = platform::ManagedDirectory::Open(parent_path, platform::DirectoryPolicy::kPrivateTraversable);
    if (!parent) parent = platform::ManagedDirectory::Open(parent_path, platform::DirectoryPolicy::kPrivate);
    if (parent) {
      dir = parent->OpenChild(child_name, platform::DirectoryPolicy::kPrivate, /*create=*/true, 0700);
      if (dir) return true;
    }
    return false;
  }

  // The cached file's bytes, and whether it is younger than the TTL.
  std::optional<std::vector<std::uint8_t>> Read(const std::string& path, const std::string& name, bool* fresh) {
    *fresh = false;
    if (!EnsureDir(path, false)) return std::nullopt;
    // Size and age come from the opened file: another backend may rename a new copy over it.
    const int fd = dir->OpenRegularFile(name);
    if (fd < 0) return std::nullopt;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        static_cast<std::size_t>(st.st_size) > kMaxArtworkTransferBytes) {
      ::close(fd);
      return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(st.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      offset += static_cast<std::size_t>(count);
    }
    ::close(fd);
    if (offset != bytes.size()) return std::nullopt;
    const std::time_t now = std::time(nullptr);
    *fresh = now < st.st_mtime || now - st.st_mtime < kArtworkCacheTtlSeconds;
    return bytes;
  }

  // Writes a unique temporary file and renames it over the cached one. The title-icon builder
  // has its own backend, so two writers can save the same file at once. Another thread's root raise
  // would leave a root-owned folder or file that the app user can never use again, so a save that
  // can't hold the effective UID right away is skipped; the next load downloads the cover again.
  void Save(const std::string& path, const std::string& name, const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() > kMaxArtworkTransferBytes) return;
    const auto uid = ps5::TryHoldEffectiveUid();
    if (!uid || !EnsureDir(path, true)) return;
    static std::atomic<std::uint64_t> sequence{0};
    const std::string temp_name = "." + name + "." + std::to_string(::getpid()) + "." +
                                  std::to_string(sequence.fetch_add(1)) + ".tmp";
    const int fd = dir->CreateExclusiveFile(temp_name, 0600);
    if (fd < 0) return;
    std::size_t offset = 0;
    bool written = true;
    while (offset < bytes.size()) {
      const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { written = false; break; }
      offset += static_cast<std::size_t>(count);
    }
    const bool synced = written && ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (synced && closed && dir->RenameChild(temp_name, *dir, name, /*replace_existing=*/true)) {
      dir->Sync();
      return;
    }
    dir->RemoveRegularFile(temp_name);
  }
};

std::optional<ArtworkImage> artwork_internal::DecodeArtworkWithAllocationBudget(
    const std::vector<std::uint8_t>& encoded, std::size_t allocation_limit) {
  if (encoded.empty() || encoded.size() > kMaxArtworkTransferBytes ||
      encoded.size() > static_cast<std::size_t>(INT_MAX) || allocation_limit == 0 ||
      allocation_limit > kMaxArtworkDecodeAllocationBytes) return std::nullopt;
  DecodeBudgetScope budget(allocation_limit);

  int width = 0;
  int height = 0;
  int components = 0;
  if (!stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                             &width, &height, &components)) return std::nullopt;
  std::size_t bytes = 0;
  if (!CheckedImageBytes(width, height, bytes)) return std::nullopt;

  int decoded_width = 0;
  int decoded_height = 0;
  int decoded_components = 0;
  stbi_uc* decoded = stbi_load_from_memory(
      encoded.data(), static_cast<int>(encoded.size()), &decoded_width,
      &decoded_height, &decoded_components, 4);
  if (!decoded) return std::nullopt;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(decoded,
                                                              stbi_image_free);
  std::size_t decoded_bytes = 0;
  if (decoded_width != width || decoded_height != height ||
      !CheckedImageBytes(decoded_width, decoded_height, decoded_bytes) ||
      decoded_bytes != bytes) return std::nullopt;

  ArtworkImage image;
  image.width = static_cast<std::uint32_t>(decoded_width);
  image.height = static_cast<std::uint32_t>(decoded_height);
  image.rgba.assign(pixels.get(), pixels.get() + decoded_bytes);
  return image;
}

std::optional<ArtworkImage> DecodeArtwork(
    const std::vector<std::uint8_t>& encoded) {
  return artwork_internal::DecodeArtworkWithAllocationBudget(
      encoded, kMaxArtworkDecodeAllocationBytes);
}

HttpArtworkBackend::HttpArtworkBackend(std::string ca_bundle, std::string cache_dir)
    : cache_dir_(std::move(cache_dir)),
      http_(std::make_unique<steam::HttpClient>(std::move(ca_bundle))),
      perform_([client = http_.get()](const steam::HttpRequest& request, const std::atomic<bool>& cancel) {
        return client->Perform(request, cancel);
      }),
      cache_(cache_dir_.empty() ? nullptr : std::make_unique<Cache>()) {}

HttpArtworkBackend::HttpArtworkBackend(Perform perform, std::string cache_dir)
    : cache_dir_(std::move(cache_dir)),
      perform_(std::move(perform)),
      cache_(cache_dir_.empty() ? nullptr : std::make_unique<Cache>()) {}

HttpArtworkBackend::~HttpArtworkBackend() = default;

std::optional<ArtworkImage> HttpArtworkBackend::Load(
    const ArtworkKey& key, const std::atomic<bool>& cancel) {
  const bool still = StoreStillUrl(key.source);
  const auto note = [&](const std::string& reason) {
    if (!log_ || !still) return;
    log_("still " + reason + " app=" + ToKey(key.game_id) + " url=" + key.source);
  };
  if (!CanonicalKey(key) || cancel.load(std::memory_order_acquire)) {
    if (!CanonicalKey(key)) {
      if (log_ && (still || key.source.find("store_item_assets") != std::string::npos))
        log_("still rejected app=" + ToKey(key.game_id) + " url=" + key.source);
    } else if (still) {
      note("canceled");
    }
    return std::nullopt;
  }

  const bool cover = key.source.find("library_600x900.jpg") != std::string::npos;
  // A wide image in the cover slot is the header.jpg fallback: frame it as a portrait card.
  const auto framed = [cover](std::optional<ArtworkImage> image) {
    if (image && cover && image->width > image->height) image = FrameToPortrait(*image);
    return image;
  };
  const std::string file_name = cache_ ? CacheFileName(key) : std::string{};

  // A fresh cached copy is used without asking the CDN.
  std::optional<std::vector<std::uint8_t>> cached;
  bool fresh = false;
  if (!file_name.empty()) {
    std::lock_guard<std::mutex> lock(cache_->mutex);
    cached = cache_->Read(cache_dir_, file_name, &fresh);
  }
  if (cached && fresh)
    if (auto image = DecodeArtwork(*cached)) {
      note("cache " + std::to_string(image->width) + "x" + std::to_string(image->height));
      return framed(std::move(image));
    }
  if (cancel.load(std::memory_order_acquire)) {
    note("canceled");
    return std::nullopt;
  }

  steam::HttpRequest request;
  request.method = steam::HttpMethod::kGet;
  request.url = key.source;
  request.timeout_ms = kArtworkTimeoutMs;
  request.max_response_bytes = kMaxArtworkTransferBytes;
  auto response = perform_(request, cancel);

  // A game with no portrait cover (a real 404, not a timeout or a 5xx) uses its header.jpg banner.
  if (cover && response.ok() && response.status_code == 404 && !cancel.load(std::memory_order_acquire)) {
    request.url = "https://cdn.cloudflare.steamstatic.com/steam/apps/" + ToKey(key.game_id) + "/header.jpg";
    response = perform_(request, cancel);
  }

  // Only bytes that decode replace the cached copy, so a bad response never overwrites a good cover.
  if (response.ok() && response.status_code == 200) {
    if (auto image = DecodeArtwork(response.body)) {
      if (!file_name.empty()) {
        std::lock_guard<std::mutex> lock(cache_->mutex);
        cache_->Save(cache_dir_, file_name, response.body);
      }
      note("ok " + std::to_string(image->width) + "x" + std::to_string(image->height) +
           " bytes=" + std::to_string(response.body.size()));
      return framed(std::move(image));
    }
    note("decode bytes=" + std::to_string(response.body.size()));
  } else if (!response.ok()) {
    note(std::string("error=") + steam::HttpErrorName(response.error) +
         " bytes=" + std::to_string(response.body.size()));
  } else {
    note("http=" + std::to_string(response.status_code) + " bytes=" + std::to_string(response.body.size()));
  }

  // Offline, or the CDN failed: an expired cached copy is better than none.
  if (cached && !fresh)
    if (auto image = framed(DecodeArtwork(*cached))) {
      note("stale " + std::to_string(image->width) + "x" + std::to_string(image->height));
      return image;
    }
  return std::nullopt;
}

}  // namespace playden::homebrew

#pragma once
#include "library/repository.h"
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace playden {
class LibraryCache final : public LibraryRepository {
 public:
  explicit LibraryCache(Storage& storage) : storage_(storage) {}
  Result<LibrarySnapshot> Load(std::uint64_t account_id) const override;
  Result<void> Save(std::uint64_t account_id, const LibrarySnapshot& snapshot) const override;
  Result<void> Clear(std::uint64_t account_id) const override;
  Result<LibrarySnapshot> Load(const std::string& account_id) const;
  Result<void> Save(const std::string& account_id, const LibrarySnapshot& snapshot) const;
  Result<void> Clear(const std::string& account_id) const;
  Result<void> MergeGameTags(const std::string& account_id, const GameId& id,
                             std::vector<std::string> tags) const;
  Result<void> MergeGameTags(std::uint64_t account_id, const GameId& id,
                             std::vector<std::string> tags) const;
  // Patches the snapshot kept for this account. The file is read only when that snapshot is missing,
  // and written when a game's tags differ. A missing game, or tags that are not valid, is skipped.
  Result<void> MergeGameTags(std::uint64_t account_id,
                             const std::vector<std::pair<GameId, std::vector<std::string>>>& updates) const;

 private:
  Result<void> Persist(const std::string& account, const LibrarySnapshot& snapshot) const;
  Result<void> MergeResident(
      const std::string& account,
      const std::vector<std::pair<GameId, std::vector<std::string>>>& updates, bool strict) const;

  Storage& storage_;
  mutable std::mutex snapshot_mutex_;
  mutable std::optional<LibrarySnapshot> resident_;
  mutable std::string resident_account_;
};
}  // namespace playden

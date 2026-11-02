#pragma once
// kit_catalog.h - Which installed kit a pin names, and which parked copies to keep.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "update/release.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// Preferences and per-game settings store this when the player chose Latest.
inline constexpr const char* kLatestKitPin = "latest";

// A version the player saved before folder names ("0.17", "0.13-dev.20261023.1"). "latest" and empty are not one.
inline bool IsConcreteKitPin(std::string_view pin) { return update::ParseKitVersion(pin).has_value(); }

// One directory name under the kits root: "my-kit", "playden-kit-bceed2d". Not "latest", not a path.
inline bool IsKitFolderPin(std::string_view pin) {
  if (pin.empty() || pin.size() > 96 || pin.front() == '.' || pin == kLatestKitPin) return false;
  for (unsigned char c : pin)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return false;
  return true;
}

// What is written to preferences and game settings: "latest", or a folder name. Anything else is Latest.
inline std::string NormalizedKitPin(std::string_view pin) {
  if (pin == kLatestKitPin || IsKitFolderPin(pin)) return std::string(pin);
  return kLatestKitPin;
}

// The folder a replaced kit is parked in, beside the Latest slot: "<kit_id>-<git hash>".
inline std::string ParkedKitFolder(std::string_view kit_id, std::string_view git_hash) {
  return std::string(kit_id) + "-" + std::string(git_hash);
}

// `name` is an older parked folder, "<kit_id>-<version>". Writes the version when it is.
inline bool ParkedKitVersion(std::string_view kit_id, std::string_view name, std::string* version) {
  const std::string prefix = std::string(kit_id) + "-";
  if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return false;
  const auto parsed = name.substr(prefix.size());
  if (!IsConcreteKitPin(parsed)) return false;
  if (version) *version = std::string(parsed);
  return true;
}

// A folder the updater used to park as "<kit_id>-<version>". A git-hash folder stays, like a custom playkit:
// the player cannot have pinned that name before the update created it.
inline bool UpdaterParkedFolder(std::string_view kit_id, std::string_view name) {
  const std::string prefix = std::string(kit_id) + "-";
  if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return false;
  return IsConcreteKitPin(name.substr(prefix.size()));
}

enum class ResolvedKitKind {
  kUnavailable,  // nothing to run this game with
  kLatestSlot,   // the kit at <kits>/<kit_id>
  kFolder,       // a kit in another directory
};

struct ResolvedKit {
  ResolvedKitKind kind = ResolvedKitKind::kUnavailable;
  std::string folder;  // the directory name in use, when one was chosen
  bool fell_back = false;  // a saved folder was not available, so Latest is in use
};

// `saved` is empty, "latest", or a folder name. `folders` are the other kits that admit.
inline ResolvedKit ResolveKitPin(std::string_view saved, bool latest_admits, const std::vector<std::string>& folders) {
  if (IsKitFolderPin(saved)) {
    for (const auto& folder : folders)
      if (folder == saved) return {ResolvedKitKind::kFolder, std::string(saved), false};
    if (latest_admits) return {ResolvedKitKind::kLatestSlot, {}, true};
    return {ResolvedKitKind::kUnavailable, {}, true};
  }
  if (latest_admits) return {ResolvedKitKind::kLatestSlot, {}, false};
  return {ResolvedKitKind::kUnavailable, {}, false};
}

// A game pin wins when it is set. Empty follows the default pin (itself empty or "latest" for the slot).
inline ResolvedKit ResolveGameKit(std::string_view game_pin, std::string_view default_pin, bool latest_admits,
                                  const std::vector<std::string>& folders) {
  return ResolveKitPin(game_pin.empty() ? default_pin : game_pin, latest_admits, folders);
}

// Deletes unpinned legacy "<kit_id>-<version>" folders that admit. The Latest slot, a git-hash folder, a
// custom playkit, dot-folders, and a folder that does not admit are left alone. `admits` is the folder path.
// Returns the folders removed. An old version pin ("0.17") still protects "<kit_id>-0.17"; the full folder
// name does too.
std::vector<std::string> PruneUnpinnedKits(const std::string& kits_root, std::string_view kit_id,
                                           const std::vector<std::string>& pinned,
                                           const std::function<bool(const std::string& path)>& admits);

}  // namespace playden::runtime

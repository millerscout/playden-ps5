// runtime_kit_catalog_test.cpp - Which installed kit a pin names, and which parked copies stay.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/kit_catalog.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using playden::runtime::IsConcreteKitPin;
using playden::runtime::IsKitFolderPin;
using playden::runtime::NormalizedKitPin;
using playden::runtime::ParkedKitVersion;
using playden::runtime::PruneUnpinnedKits;
using playden::runtime::ResolveGameKit;
using playden::runtime::ResolveKitPin;
using playden::runtime::ResolvedKitKind;
using playden::runtime::UpdaterParkedFolder;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::exit(1); } } while (false)

int main() {
  const std::vector<std::string> parked = {"my-kit", "playden-kit-bceed2d"};
  const auto saved = ResolveKitPin("my-kit", true, parked);
  CHECK(saved.kind == ResolvedKitKind::kFolder && saved.folder == "my-kit" && !saved.fell_back);
  const auto missing = ResolveKitPin("other-kit", true, parked);
  CHECK(missing.kind == ResolvedKitKind::kLatestSlot && missing.folder.empty() && missing.fell_back);
  const auto follows = ResolveGameKit("", "playden-kit-bceed2d", true, parked);
  CHECK(follows.kind == ResolvedKitKind::kFolder && follows.folder == "playden-kit-bceed2d" && !follows.fell_back);
  const auto latest = ResolveKitPin("latest", true, parked);
  CHECK(latest.kind == ResolvedKitKind::kLatestSlot && latest.folder.empty() && !latest.fell_back);
  const auto empty = ResolveKitPin("", true, parked);
  CHECK(empty.kind == ResolvedKitKind::kLatestSlot && !empty.fell_back);
  const auto old_version = ResolveKitPin("0.17", true, parked);
  CHECK(old_version.kind == ResolvedKitKind::kLatestSlot && old_version.fell_back);
  const auto parked_only = ResolveKitPin("my-kit", false, parked);
  CHECK(parked_only.kind == ResolvedKitKind::kFolder && parked_only.folder == "my-kit" && !parked_only.fell_back);
  const auto neither = ResolveKitPin("other-kit", false, parked);
  CHECK(neither.kind == ResolvedKitKind::kUnavailable && neither.fell_back);
  const auto no_latest = ResolveKitPin("latest", false, parked);
  CHECK(no_latest.kind == ResolvedKitKind::kUnavailable && !no_latest.fell_back);
  CHECK(IsKitFolderPin("my-kit") && IsKitFolderPin("playden-kit-bceed2d") && IsKitFolderPin("0.17"));
  CHECK(!IsKitFolderPin("latest") && !IsKitFolderPin("") && !IsKitFolderPin(".hidden") && !IsKitFolderPin("a/b"));
  CHECK(NormalizedKitPin("my-kit") == "my-kit" && NormalizedKitPin("latest") == "latest");
  CHECK(NormalizedKitPin("../x") == "latest" && NormalizedKitPin(".hidden") == "latest");
  CHECK(IsConcreteKitPin("0.13-dev.20261023.1") && !IsConcreteKitPin("latest") && !IsConcreteKitPin("my-kit"));
  std::string version;
  CHECK(ParkedKitVersion("playden-kit", "playden-kit-0.16", &version) && version == "0.16");
  CHECK(!ParkedKitVersion("playden-kit", "playden-kit", &version));
  CHECK(!ParkedKitVersion("playden-kit", "playden-kit-bceed2d", &version));
  CHECK(!ParkedKitVersion("playden-kit", ".replaced-playden-kit-1", &version));
  CHECK(!UpdaterParkedFolder("playden-kit", "playden-kit-bceed2d"));
  CHECK(!UpdaterParkedFolder("playden-kit", "playden-kit-abc"));
  CHECK(UpdaterParkedFolder("playden-kit", "playden-kit-0.16"));
  CHECK(!UpdaterParkedFolder("playden-kit", "my-kit"));
  CHECK(!UpdaterParkedFolder("playden-kit", "playden-kit-h-r9"));

  char pattern[] = "/tmp/playden-kit-catalog-XXXXXX";
  const auto made = mkdtemp(pattern);
  CHECK(made != nullptr);
  const std::filesystem::path root = made;
  std::filesystem::create_directories(root / "playden-kit");
  std::filesystem::create_directories(root / "playden-kit-bceed2d");
  std::filesystem::create_directories(root / "playden-kit-abc");
  std::filesystem::create_directories(root / "playden-kit-0.16");
  std::filesystem::create_directories(root / "playden-kit-0.15");
  std::filesystem::create_directories(root / "my-kit");
  std::filesystem::create_directories(root / ".replaced-old");
  const auto removed = PruneUnpinnedKits(root.string(), "playden-kit", {"0.16", "playden-kit-bceed2d"},
                                         [](const std::string&) { return true; });
  CHECK(removed.size() == 1 && removed[0] == (root / "playden-kit-0.15").string());
  CHECK(std::filesystem::exists(root / "playden-kit"));
  CHECK(std::filesystem::exists(root / "playden-kit-bceed2d"));
  CHECK(std::filesystem::exists(root / "playden-kit-abc"));
  CHECK(std::filesystem::exists(root / "playden-kit-0.16"));
  CHECK(std::filesystem::exists(root / "my-kit"));
  CHECK(std::filesystem::exists(root / ".replaced-old"));
  CHECK(!std::filesystem::exists(root / "playden-kit-0.15"));
  std::filesystem::remove_all(root);
  return 0;
}

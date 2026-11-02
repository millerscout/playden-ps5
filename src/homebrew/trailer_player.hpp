#pragma once
// trailer_player.hpp - Plays one Steam trailer mp4 into RGBA frames.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace playden::homebrew {

// One trailer at a time. Play and Stop and TakeFrame are called from the UI thread.
// The Mac build decodes with VideoToolbox. Other builds accept Play and then fail,
// so the page keeps the poster.
class TrailerPlayer {
 public:
  TrailerPlayer();
  ~TrailerPlayer();
  TrailerPlayer(const TrailerPlayer&) = delete;
  TrailerPlayer& operator=(const TrailerPlayer&) = delete;

  void Play(const std::string& url);
  void Stop();
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] bool failed() const noexcept;

  // Copies the newest frame when `serial` differs from the last one published.
  bool TakeFrame(std::vector<std::uint8_t>& rgba, std::uint32_t& width, std::uint32_t& height,
                 std::uint64_t& serial);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playden::homebrew

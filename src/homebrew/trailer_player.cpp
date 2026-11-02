#include "homebrew/trailer_player.hpp"

namespace playden::homebrew {

struct TrailerPlayer::Impl {
  bool failed = false;
};

TrailerPlayer::TrailerPlayer() : impl_(std::make_unique<Impl>()) {}
TrailerPlayer::~TrailerPlayer() = default;

void TrailerPlayer::Play(const std::string&) { impl_->failed = true; }
void TrailerPlayer::Stop() { impl_->failed = false; }
bool TrailerPlayer::running() const noexcept { return false; }
bool TrailerPlayer::failed() const noexcept { return impl_->failed; }

bool TrailerPlayer::TakeFrame(std::vector<std::uint8_t>&, std::uint32_t&, std::uint32_t&,
                              std::uint64_t&) {
  return false;
}

}  // namespace playden::homebrew

//
// Created for the reactor / IO layer (columnar/network N0).
//

#include "Io/Poller.h"

#include <glog/logging.h>

#include <cerrno>

#include "Io/Channel.h"

namespace dbplay {

Poller::Poller(EventLoop *loop) : loop_(loop) {}

Poller::~Poller() = default;

void Poller::Poll(int timeout_ms, ChannelList *active_channels) {
  int num_events = ::poll(pollfds_.data(), pollfds_.size(), timeout_ms);
  if (num_events > 0) {
    FillActiveChannels(num_events, active_channels);
  } else if (num_events < 0 && errno != EINTR) {
    PLOG(ERROR) << "Poller::Poll";
  }
}

void Poller::FillActiveChannels(int num_events, ChannelList *active_channels) const {
  for (const auto &pfd : pollfds_) {
    if (num_events <= 0) break;
    if (pfd.revents > 0) {
      --num_events;
      auto it = channels_.find(pfd.fd);
      DBPLAYGROUND_ASSERT(it != channels_.end(), "Poller: revents for unknown fd");
      Channel *channel = it->second;
      channel->SetRevents(pfd.revents);
      active_channels->push_back(channel);
    }
  }
}

void Poller::UpdateChannel(Channel *channel) {
  if (channel->IndexInPoller() < 0) {
    // A brand new channel: append a pollfd for it.
    DBPLAYGROUND_ASSERT(channels_.find(channel->Fd()) == channels_.end(),
                        "Poller: fd already registered");
    struct pollfd pfd;
    pfd.fd = channel->Fd();
    pfd.events = static_cast<short>(channel->Events());
    pfd.revents = 0;
    pollfds_.push_back(pfd);
    int index = static_cast<int>(pollfds_.size()) - 1;
    channel->SetIndexInPoller(index);
    channels_[channel->Fd()] = channel;
  } else {
    // Existing channel: refresh the watched events in place.
    int index = channel->IndexInPoller();
    DBPLAYGROUND_ASSERT(channels_.find(channel->Fd()) != channels_.end(),
                        "Poller: updating unregistered fd");
    struct pollfd &pfd = pollfds_[index];
    pfd.events = static_cast<short>(channel->Events());
    pfd.revents = 0;
    if (channel->IsNoneEvent()) {
      // Negate the fd so ::poll ignores it while keeping the slot; the one's
      // complement (-fd-1) also handles fd == 0 correctly.
      pfd.fd = -channel->Fd() - 1;
    } else {
      pfd.fd = channel->Fd();
    }
  }
}

void Poller::RemoveChannel(Channel *channel) {
  int index = channel->IndexInPoller();
  DBPLAYGROUND_ASSERT(channels_.find(channel->Fd()) != channels_.end(),
                      "Poller: removing unregistered fd");
  channels_.erase(channel->Fd());

  auto last_index = static_cast<int>(pollfds_.size()) - 1;
  if (index == last_index) {
    pollfds_.pop_back();
  } else {
    // Swap-remove: move the last pollfd into this slot and fix up its channel's
    // recorded index. The stored fd may be negated (none-event), so undo that
    // before looking the channel up.
    int fd_at_end = pollfds_.back().fd;
    if (fd_at_end < 0) fd_at_end = -fd_at_end - 1;
    std::swap(pollfds_[index], pollfds_.back());
    channels_[fd_at_end]->SetIndexInPoller(index);
    pollfds_.pop_back();
  }
  channel->SetIndexInPoller(-1);
}

bool Poller::HasChannel(Channel *channel) const {
  auto it = channels_.find(channel->Fd());
  return it != channels_.end() && it->second == channel;
}

}  // namespace dbplay

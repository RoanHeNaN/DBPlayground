//
// Poller: the IO multiplexing backend for an EventLoop. This implementation is
// built on ::poll(), which works identically on Linux and macOS, so the whole
// reactor is portable. epoll/kqueue/io_uring can later replace this class
// without touching Channel or EventLoop.
//

#pragma once

#include <poll.h>

#include <map>
#include <vector>

#include "Common/Macros.h"

namespace dbplay {

class Channel;
class EventLoop;

class Poller {
 public:
  using ChannelList = std::vector<Channel *>;

  explicit Poller(EventLoop *loop);
  ~Poller();
  DISALLOW_COPY_AND_MOVE(Poller);

  // Block up to timeout_ms waiting for IO, then append every channel whose fd
  // fired to active_channels.
  void Poll(int timeout_ms, ChannelList *active_channels);

  // Register a new channel or update the watched events of an existing one.
  void UpdateChannel(Channel *channel);
  // Drop a channel entirely; must be called before its fd is closed.
  void RemoveChannel(Channel *channel);

  bool HasChannel(Channel *channel) const;

 private:
  void FillActiveChannels(int num_events, ChannelList *active_channels) const;

  EventLoop *loop_;
  std::vector<struct pollfd> pollfds_;
  std::map<int, Channel *> channels_;  // fd -> Channel
};

}  // namespace dbplay

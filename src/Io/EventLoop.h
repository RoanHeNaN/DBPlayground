//
// EventLoop: the reactor. One EventLoop belongs to exactly one thread (the
// thread that constructed it) and runs a poll -> dispatch loop. This is the
// shared runtime seam the network layer and, later, the storage IO layer will
// both post work onto (via runInLoop/queueInLoop, added in N1).
//

#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "Common/Macros.h"

namespace dbplay {

class Channel;
class Poller;

class EventLoop {
 public:
  EventLoop();
  ~EventLoop();
  DISALLOW_COPY_AND_MOVE(EventLoop);

  // Run the poll -> dispatch loop until Quit() is called. Must run in the
  // thread that constructed this EventLoop.
  void Loop();

  // Stop the loop. (In N0 this is only safe to call from the loop thread, e.g.
  // inside a channel callback; cross-thread Quit arrives with N1's wakeup.)
  void Quit();

  // Channel registration, forwarded to the Poller. Loop-thread only.
  void UpdateChannel(Channel *channel);
  void RemoveChannel(Channel *channel);
  bool HasChannel(Channel *channel);

  bool IsInLoopThread() const { return thread_id_ == std::this_thread::get_id(); }
  void AssertInLoopThread() const;

 private:
  using ChannelList = std::vector<Channel *>;

  std::atomic<bool> looping_;
  std::atomic<bool> quit_;
  const std::thread::id thread_id_;
  std::unique_ptr<Poller> poller_;
  ChannelList active_channels_;
};

}  // namespace dbplay

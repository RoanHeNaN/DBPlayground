//
// Channel: wraps a single fd plus the events we care about and the callbacks to
// run when they fire. A Channel does NOT own the fd; it only tells its owning
// EventLoop which events to watch and dispatches revents back to callbacks.
//

#pragma once

#include "Common/Macros.h"
#include "Io/Callbacks.h"

namespace dbplay {

class EventLoop;

class Channel {
 public:
  Channel(EventLoop *loop, int fd);
  ~Channel();
  DISALLOW_COPY_AND_MOVE(Channel);

  // Called by the EventLoop after Poller reports revents for this fd; fans out
  // to the registered callbacks based on which events fired.
  void HandleEvent();

  void SetReadCallback(EventCallback cb) { read_callback_ = std::move(cb); }
  void SetWriteCallback(EventCallback cb) { write_callback_ = std::move(cb); }
  void SetCloseCallback(EventCallback cb) { close_callback_ = std::move(cb); }
  void SetErrorCallback(EventCallback cb) { error_callback_ = std::move(cb); }

  int Fd() const { return fd_; }
  int Events() const { return events_; }
  void SetRevents(int revt) { revents_ = revt; }

  void EnableReading() {
    events_ |= kReadEvent;
    Update();
  }
  void DisableReading() {
    events_ &= ~kReadEvent;
    Update();
  }
  void EnableWriting() {
    events_ |= kWriteEvent;
    Update();
  }
  void DisableWriting() {
    events_ &= ~kWriteEvent;
    Update();
  }
  void DisableAll() {
    events_ = kNoneEvent;
    Update();
  }

  bool IsReading() const { return (events_ & kReadEvent) != 0; }
  bool IsWriting() const { return (events_ & kWriteEvent) != 0; }
  bool IsNoneEvent() const { return events_ == kNoneEvent; }

  // Poller bookkeeping: index of this channel's pollfd in the Poller's array,
  // or -1 when the channel is not yet registered.
  int IndexInPoller() const { return index_; }
  void SetIndexInPoller(int index) { index_ = index; }

  EventLoop *OwnerLoop() const { return loop_; }
  // Remove this channel from its EventLoop's Poller.
  void Remove();

 private:
  // Push the current events_ mask down to the Poller via the EventLoop.
  void Update();

  static const int kNoneEvent;
  static const int kReadEvent;
  static const int kWriteEvent;

  EventLoop *const loop_;
  const int fd_;
  int events_;   // events we ask the Poller to watch
  int revents_;  // events the Poller reported for the last poll
  int index_;    // position in Poller's pollfd array, -1 if not registered

  EventCallback read_callback_;
  EventCallback write_callback_;
  EventCallback close_callback_;
  EventCallback error_callback_;
};

}  // namespace dbplay

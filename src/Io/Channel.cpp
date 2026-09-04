//
// Created for the reactor / IO layer (columnar/network N0).
//

#include "Io/Channel.h"

#include <poll.h>

#include "Io/EventLoop.h"

namespace dbplay {

const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = POLLIN | POLLPRI;
const int Channel::kWriteEvent = POLLOUT;

Channel::Channel(EventLoop *loop, int fd) : loop_(loop), fd_(fd), events_(kNoneEvent), revents_(0), index_(-1) {}

Channel::~Channel() = default;

void Channel::Update() { loop_->UpdateChannel(this); }

void Channel::Remove() { loop_->RemoveChannel(this); }

void Channel::HandleEvent() {
  // Peer hung up with no more data to read: treat as close.
  if ((revents_ & POLLHUP) && !(revents_ & POLLIN)) {
    if (close_callback_) close_callback_();
  }
  if (revents_ & (POLLERR | POLLNVAL)) {
    if (error_callback_) error_callback_();
  }
  if (revents_ & (POLLIN | POLLPRI)) {
    if (read_callback_) read_callback_();
  }
  if (revents_ & POLLOUT) {
    if (write_callback_) write_callback_();
  }
}

}  // namespace dbplay

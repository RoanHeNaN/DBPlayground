//
// Created for the reactor / IO layer (columnar/network N0).
//

#include "Io/EventLoop.h"

#include <glog/logging.h>

#include <sstream>

#include "Io/Channel.h"
#include "Io/Poller.h"

namespace dbplay {

namespace {
// Wake up at least this often so the loop can notice quit_ even without IO. N1
// replaces this coarse polling with an explicit cross-thread wakeup fd.
constexpr int kPollTimeoutMs = 10000;

std::string ThreadIdString(const std::thread::id &id) {
  std::ostringstream oss;
  oss << id;
  return oss.str();
}
}  // namespace

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      thread_id_(std::this_thread::get_id()),
      poller_(std::make_unique<Poller>(this)) {}

EventLoop::~EventLoop() {
  DBPLAYGROUND_ASSERT(!looping_, "EventLoop destroyed while still looping");
}

void EventLoop::Loop() {
  DBPLAYGROUND_ASSERT(!looping_, "EventLoop::Loop called re-entrantly");
  AssertInLoopThread();
  looping_ = true;
  quit_ = false;

  while (!quit_) {
    active_channels_.clear();
    poller_->Poll(kPollTimeoutMs, &active_channels_);
    for (Channel *channel : active_channels_) {
      channel->HandleEvent();
    }
  }

  looping_ = false;
}

void EventLoop::Quit() { quit_ = true; }

void EventLoop::UpdateChannel(Channel *channel) {
  AssertInLoopThread();
  poller_->UpdateChannel(channel);
}

void EventLoop::RemoveChannel(Channel *channel) {
  AssertInLoopThread();
  poller_->RemoveChannel(channel);
}

bool EventLoop::HasChannel(Channel *channel) {
  AssertInLoopThread();
  return poller_->HasChannel(channel);
}

void EventLoop::AssertInLoopThread() const {
  if (!IsInLoopThread()) {
    LOG(FATAL) << "EventLoop was created in thread " << ThreadIdString(thread_id_)
               << " but accessed from thread " << ThreadIdString(std::this_thread::get_id());
  }
}

}  // namespace dbplay

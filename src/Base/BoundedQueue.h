//
// Created by 何智强 on 2021/10/29.
//

#ifndef DBPLAYGROUND_BOUNDEDQUEUE_H
#define DBPLAYGROUND_BOUNDEDQUEUE_H

#include <condition_variable>
#include <iostream>
#include <list>
#include <mutex>

namespace dbplay {

template <typename T>
class BoundedQueue {
 public:
  BoundedQueue() = default;
  BoundedQueue(const BoundedQueue &) = delete;
  explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

  bool PushBack(const T &element) {
    std::unique_lock<std::mutex> queue_lock(queue_mutex_);
    while (queue_.size() >= capacity_) queue_condition_.wait(queue_lock);
    queue_.emplace_back(element);
    queue_condition_.notify_one();
    return true;
  }

  bool Pop(T &element_ret) {
    std::unique_lock<std::mutex> queue_lock(queue_mutex_);
    while (queue_.empty()) queue_condition_.wait(queue_lock);

    queue_.pop_back();
    queue_condition_.notify_one();
    return true;
  }

  bool IsFull() {
    std::unique_lock<std::mutex> queue_lock(queue_mutex_);
    return queue_.size() >= capacity_;
  }

 private:
  std::list<T> queue_;
  size_t capacity_;
  std::mutex queue_mutex_;
  std::condition_variable queue_condition_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_BOUNDEDQUEUE_H

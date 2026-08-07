//
// Created by 何智强 on 2021/10/3.
//

#ifndef DBPLAYGROUND_READERWRITERLATCH_H
#define DBPLAYGROUND_READERWRITERLATCH_H

/**
 * Reader-Writer latch backed by std::mutex.
 */

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace dbplay {

class ReaderWriterLatch {
  using Mutex = std::mutex;
  using ConditionVariable = std::condition_variable;
  static const uint32_t MAX_READERS = UINT32_MAX;

 public:
  ReaderWriterLatch() = default;
  ~ReaderWriterLatch() { std::lock_guard<Mutex> guard(mutex_); }

  //    DISALLOW_COPY(ReaderWriterLatch);

  /**
   * Acquire a write latch.
   */
  void WLock() {
    std::unique_lock<Mutex> latch(mutex_);
    while (writer_entered_) {
      reader_.wait(latch);
    }
    writer_entered_ = true;
    while (reader_count_ > 0) {
      writer_.wait(latch);
    }
  }

  /**
   * Release a write latch.
   */
  void WUnlock() {
    std::lock_guard<Mutex> guard(mutex_);
    writer_entered_ = false;
    reader_.notify_all();
  }

  /**
   * Acquire a read latch.
   */
  void RLock() {
    std::unique_lock<Mutex> latch(mutex_);
    while (writer_entered_ || reader_count_ == MAX_READERS) {
      reader_.wait(latch);
    }
    reader_count_++;
  }

  /**
   * Release a read latch.
   */
  void RUnlock() {
    std::lock_guard<Mutex> guard(mutex_);
    reader_count_--;
    if (writer_entered_) {
      if (reader_count_ == 0) {
        writer_.notify_one();
      }
    } else {
      if (reader_count_ == MAX_READERS - 1) {
        reader_.notify_one();
      }
    }
  }

 private:
  Mutex mutex_;
  ConditionVariable writer_;
  ConditionVariable reader_;
  uint32_t reader_count_{0};
  bool writer_entered_{false};
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_READERWRITERLATCH_H

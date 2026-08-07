//
// Created by 何智强 on 2021/10/5.
//

#ifndef DBPLAYGROUND_TRANSACTION_H
#define DBPLAYGROUND_TRANSACTION_H

//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// transaction.h
//
// Identification: src/include/concurrency/transaction.h
//
// Copyright (c) 2015-2019, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "Common/Macros.h"
#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <thread>  // NOLINT
#include <unordered_set>

#include "Common/Config.h"
#include "Storage/Page/Page.h"

namespace dbplay {

/**
 * Type of write operation.
 */
enum class WType { INSERT = 0, DELETE, UPDATE };

/**
 * Transaction tracks information related to a transaction.
 */
class Transaction {
 public:
  explicit Transaction(txn_id_t txn_id) : thread_id_(std::this_thread::get_id()), txn_id_(txn_id) {
    page_set_ = std::make_shared<std::deque<std::shared_ptr<Page>>>();
    deleted_page_set_ = std::make_shared<std::unordered_set<page_id_t>>();
  }

  ~Transaction() = default;

  DISALLOW_COPY(Transaction);

  /** @return the id of the thread running the transaction */
  inline std::thread::id GetThreadId() const { return thread_id_; }

  /** @return the id of this transaction */
  inline txn_id_t GetTransactionId() const { return txn_id_; }

  /** @return the page set */
  inline std::shared_ptr<std::deque<std::shared_ptr<Page>>> GetPageSet() { return page_set_; }

  /**
   * Adds a page into the page set.
   * @param page page to be added
   */
  inline void AddIntoPageSet(std::shared_ptr<Page> page) { page_set_->push_back(page); }

  /** @return the deleted page set */
  inline std::shared_ptr<std::unordered_set<page_id_t>> GetDeletedPageSet() { return deleted_page_set_; }

  /**
   * Adds a page to the deleted page set.
   * @param page_id id of the page to be marked as deleted
   */
  inline void AddIntoDeletedPageSet(page_id_t page_id) { deleted_page_set_->insert(page_id); }

 private:
  /** The thread ID, used in single-threaded transactions. */
  std::thread::id thread_id_;
  /** The ID of this transaction. */
  txn_id_t txn_id_;

  /** Concurrent index: the pages that were latched during index operation. */
  std::shared_ptr<std::deque<std::shared_ptr<Page>>> page_set_;
  /** Concurrent index: the page IDs that were deleted during index operation.*/
  std::shared_ptr<std::unordered_set<page_id_t>> deleted_page_set_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TRANSACTION_H

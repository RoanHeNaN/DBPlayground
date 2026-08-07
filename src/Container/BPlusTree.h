//
// Created by 何智强 on 2021/10/5.
//

#ifndef DBPLAYGROUND_BPLUSTREE_H
#define DBPLAYGROUND_BPLUSTREE_H

//===----------------------------------------------------------------------===//
//
//                         CMU-DB Project (15-445/645)
//                         ***DO NO SHARE PUBLICLY***
//
// Identification: src/include/index/b_plus_tree.h
//
// Copyright (c) 2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//
#pragma once

#include <deque>
#include <queue>
#include <string>
#include <vector>

#include "Common/Config.h"
#include "Concurrency/Transaction.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Page/BPlusTreeInternalPage.h"
#include "Storage/Page/BPlusTreeLeafPage.h"
#include "Storage/Page/BPlusTreePage.h"

namespace dbplay {

#define BPLUSTREE BPlusTree<KeyType, ValueType>

/**
 * Main class providing the API for the Interactive B+ Tree.
 *
 * Implementation of simple b+ tree data structure where internal pages direct
 * the search and leaf pages contain actual data.
 * (1) We only support unique key
 * (2) support insert & remove
 * (3) The structure should shrink and grow dynamically
 * (4) Implement index iterator for range scan
 */
INDEX_TEMPLATE_ARGUMENTS
class BPlusTree {
  using MappingType = std::pair<KeyType, ValueType>;
  using InternalPage = BPlusTreeInternalPage<KeyType, page_id_t>;
  using LeafPage = BPlusTreeLeafPage<KeyType, ValueType>;

  enum class OpType { Read, Insert, Remove };

 public:
  explicit BPlusTree(std::shared_ptr<BufferPoolManager> buffer_pool_manager, size_t leaf_max_size = LEAF_PAGE_SIZE,
                     size_t internal_max_size = INTERNAL_PAGE_SIZE);

  // Returns true if this B+ tree has no keys and values.
  bool IsEmpty() const;

  // Insert a key-value pair into this B+ tree.
  bool Insert(const KeyType &key, const ValueType &value, Transaction *transaction = nullptr);

  // Remove a key and its value from this B+ tree.
  void Remove(const KeyType &key, Transaction *transaction = nullptr);

  // return the value associated with a given key
  bool GetValue(const KeyType &key, ValueType &value, Transaction *transaction = nullptr);

  //        void Draw(std::shared_ptr<BufferPoolManager> bpm, const std::string &outf) {
  //            std::ofstream out(outf);
  //            out << "digraph G {" << std::endl;
  //            ToGraph(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(root_page_id_)->GetData()), bpm, out);
  //            out << "}" << std::endl;
  //            out.close();
  //        }

  // read data from file and insert one by one
  void InsertFromFile(const std::string &file_name, Transaction *transaction = nullptr);

  // read data from file and remove one by one
  void RemoveFromFile(const std::string &file_name, Transaction *transaction = nullptr);

  // Helper function to print the B+ tree for debugging. It's a thin wrapper around SafeToString().
  void PrintTree() const;

 private:
  // expose for test purpose
  std::shared_ptr<Page> FindLeafPage(const KeyType &key, bool left_most = false);

  void StartNewTree(const KeyType &key, const ValueType &value);

  bool InsertIntoLeaf(const KeyType &key, const ValueType &value, Transaction *transaction = nullptr);

  void InsertIntoParent(BPlusTreePage *old_node, const KeyType &key, BPlusTreePage *new_node,
                        Transaction *transaction = nullptr);

  template <typename N>
  N *Split(N *node);

  template <typename N>
  bool CoalesceOrRedistribute(N *node, Transaction *txn, const KeyType &key);

  template <typename N>
  bool Coalesce(N **neighbor_node, N **node, BPlusTreeInternalPage<KeyType, page_id_t> **parent, int index,
                Transaction *transaction = nullptr);

  template <typename N>
  void Redistribute(N *neighbor_node, N *node, int index);

  bool AdjustRoot(BPlusTreePage *node);

  void UpdateRootPageId(int insert_record = 0);

  //        void ToGraph(BPlusTreePage *page, std::shared_ptr<BufferPoolManager> bpm, std::ofstream &out) const;

  //        void ToString(BPlusTreePage *page, std::shared_ptr<BufferPoolManager> bpm) const;

  // Safe ToString, without affecting the behavior of the buffer pool manager.
  //        void SafeToString(BPlusTreePage *page, std::shared_ptr<BufferPoolManager> bpm) const;

  // Similar to FindLeafPage, but with concurrency control
  std::shared_ptr<Page> FindLeafPageRW(const KeyType &key, bool left_most, enum OpType op, Transaction *transaction);

  template <typename N>
  bool FitOne(N *node1, N *node2);

  template <typename N>
  int MinSize(N *node);

  template <typename N>
  int MaxSize(N *node);

  template <typename N>
  bool IsSafe(N *node, enum OpType op);

  void UnlatchAndUnpin(enum OpType op, Transaction *transaction) const;

  page_id_t root_page_id_;  // acquire root_mutex_ before r/w root_page_id
  std::mutex root_mutex_;   // protect root_page_id
  std::shared_ptr<BufferPoolManager> buffer_pool_manager_;
  size_t leaf_max_size_;
  size_t internal_max_size_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_BPLUSTREE_H

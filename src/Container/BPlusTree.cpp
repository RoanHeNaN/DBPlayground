//
// Created by 何智强 on 2021/10/5.
//

#include "Container/BPlusTree.h"

#include <iostream>
#include <string>

#include "Common/EncodedKey.h"
#include "Common/RID.h"
#include "Storage/Page/Page.h"

namespace dbplay {
INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE::BPlusTree(std::shared_ptr<BufferPoolManager> buffer_pool_manager, size_t leaf_max_size,
                     size_t internal_max_size)
    : root_page_id_(INVALID_PAGE_ID),
      buffer_pool_manager_(buffer_pool_manager),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {}

// This function doesn't provide concurrency control for accessing root_page_id.
// The caller should acquire root_mutex_ throughout the call.
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE::IsEmpty() const { return root_page_id_ == INVALID_PAGE_ID; }

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE::GetValue(const KeyType &key, ValueType &value, Transaction *transaction) {
  if (root_page_id_ == INVALID_PAGE_ID) {
    return false;
  }

  bool allocated = false;
  if (transaction == nullptr) {
    transaction = new Transaction(0);
    allocated = true;
  }

  std::unique_lock root_lock(root_mutex_);  // locked, guaranteed unlock before return

  auto page = FindLeafPageRW(key, false, OpType::Read, transaction);  // pinned, latched
  bool root_page_safe = transaction->GetPageSet()->front()->GetPageId() != root_page_id_;
  if (root_page_safe) {
    root_lock.unlock();
  }

  LeafPage *leaf_node = reinterpret_cast<LeafPage *>(page->GetData());

  bool exists = leaf_node->Lookup(key, &value);

  UnlatchAndUnpin(OpType::Read, transaction);  // unlatch and unpin

  if (allocated) {
    delete transaction;
  }
  return exists;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE::Insert(const KeyType &key, const ValueType &value, Transaction *transaction) {
  // If necessary, split is performed.
  return InsertIntoLeaf(key, value, transaction);
}

/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 *
 * The caller should hold root_mutex_ throughout the call.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE::StartNewTree(const KeyType &key, const ValueType &value) {
  // Ask for new page from buffer pool manager
  auto new_page = buffer_pool_manager_->NewPage();  // pinned

  if (new_page == nullptr) {
    throw std::runtime_error("out of memory");
  }

  root_page_id_ = new_page->GetPageId();

  LeafPage *leaf_page = reinterpret_cast<LeafPage *>(new_page->GetData());
  leaf_page->Init(root_page_id_, INVALID_PAGE_ID, leaf_max_size_);

  // Update root page id in the header page.
  // We are inserting a new root page id, so insert_record = 1.
  UpdateRootPageId(1);

  // Insert entry directly into leaf page.
  // For a new B+ tree, the root page IS the leaf page.
  leaf_page->Insert(key, value);

  LOG(INFO) << "Created a new BPLUS Tree, ENTRY_SIZE " << sizeof(MappingType) << " LEAF_MAX_SIZE " << leaf_max_size_
            << " INTERNAL_MAX_SIZE " << internal_max_size_ << std::endl;

  buffer_pool_manager_->UnpinPage(root_page_id_, true);  // unpin
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immediately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE::InsertIntoLeaf(const KeyType &key, const ValueType &value, Transaction *transaction) {
  std::unique_lock root_lock(root_mutex_);  // locked, guaranteed unlock before return
  if (IsEmpty()) {
    StartNewTree(key, value);  // root_mutex_ held throughout the call
    return true;
  }

  bool allocated = false;
  if (transaction == nullptr) {
    transaction = new Transaction(0);
    allocated = true;
  }

  // root_mutex_ already held
  auto leaf_page = FindLeafPageRW(key, false, OpType::Insert, transaction);  // leaf_page pinned, page(s) latched
  bool root_page_safe = transaction->GetPageSet()->front()->GetPageId() != root_page_id_;
  if (root_page_safe) {
    root_lock.unlock();
  }

  auto leaf_node = reinterpret_cast<LeafPage *>(leaf_page->GetData());

  // duplicate, return immediately
  if (leaf_node->Lookup(key, nullptr)) {
    UnlatchAndUnpin(OpType::Insert, transaction);
    return false;
  }

  // no duplicate: insert
  leaf_node->Insert(key, value);

  // Split if necessary. When size=leaf_max_size, split. See SplitTest.
  if (leaf_node->GetSize() == MaxSize(leaf_node) + 1) {
    // If we enter this branch, leaf_node is not safe, so parent must have been latched.

    // Split: 1. Redistribute evenly; 2. Copy up middle key.
    // No need to wlatch the new page. No other thread can access it simultaneously, as parent is wlatched.
    LeafPage *new_leaf_page = Split(leaf_node);  // new_leaf_page pinned
    InsertIntoParent(leaf_node, new_leaf_page->KeyAt(0), new_leaf_page, transaction);

    buffer_pool_manager_->UnpinPage(new_leaf_page->GetPageId(), true);  // new_leaf_page unpin
  }

  UnlatchAndUnpin(OpType::Insert, transaction);

  if (allocated) {
    delete transaction;
  }

  //        LOG(INFO) << "Entry " << key << " : " << value << " inserted\n";
  return true;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page.
 *
 * The new page is pinned. (NOT wlatched)
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
N *BPLUSTREE::Split(N *node) {
  // Allocate new page
  auto new_page = buffer_pool_manager_->NewPage();  // pinned
  if (new_page == nullptr) {
    throw std::runtime_error("out of memory");
  }
  page_id_t new_page_id = new_page->GetPageId();
  // Move suffix to the new node
  N *new_node;
  if (node->IsLeafPage()) {
    LeafPage *leaf_node = reinterpret_cast<LeafPage *>(node);

    LeafPage *new_leaf_node = reinterpret_cast<LeafPage *>(new_page->GetData());
    new_leaf_node->Init(new_page_id, leaf_node->GetParentPageId(), leaf_max_size_);
    leaf_node->MoveHalfTo(new_leaf_node);

    new_leaf_node->SetNextPageId(leaf_node->GetNextPageId());  // [Attention] when leaf_node already has a right sibling
    leaf_node->SetNextPageId(new_page_id);

    new_node = reinterpret_cast<N *>(new_leaf_node);
  } else {
    InternalPage *internal_page = reinterpret_cast<InternalPage *>(node);
    InternalPage *new_internal_page = reinterpret_cast<InternalPage *>(new_page->GetData());
    new_internal_page->Init(new_page_id, internal_page->GetParentPageId(), internal_max_size_);
    internal_page->MoveHalfTo(new_internal_page, buffer_pool_manager_);

    new_node = reinterpret_cast<N *>(new_internal_page);
  }

  // New page pinned
  return new_node;
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method (assume to be pinned)
 * @param   key
 * @param   new_node      returned page from split() method (assume to be pinned)
 *
 * The caller should handling unpin old_node and new_node.
 *
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 *
 * Only old_node is latched, new_node is not latched. If the parent node already exists, it's already latched.
 * Otherwise, a new root will be created, and the root page will be latched.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE::InsertIntoParent(BPlusTreePage *old_node, const KeyType &key, BPlusTreePage *new_node,
                                 Transaction *transaction) {
  // Parent page must have been latched, as old_node cannot be safe. Root_mutex might not be held.

  std::shared_ptr<Page> parent_page;
  if (old_node->IsRootPage()) {
    // Splitting original root page, B+ tree height will grow by one
    parent_page = buffer_pool_manager_->NewPage();  // new root pinned
    if (parent_page == nullptr) {
      throw std::runtime_error("out of memory");
    }

    page_id_t parent_page_id = parent_page->GetPageId();
    root_page_id_ = parent_page_id;
    UpdateRootPageId(0);

    InternalPage *parent_node = reinterpret_cast<InternalPage *>(parent_page->GetData());
    parent_node->Init(parent_page_id, INVALID_PAGE_ID, internal_max_size_);

    parent_node->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());

    old_node->SetParentPageId(parent_page_id);
    new_node->SetParentPageId(parent_page_id);

    buffer_pool_manager_->UnpinPage(parent_page_id, true);  // new root unpin
  } else {
    // This else branch handles both copy-up and push-up behavior.
    // - When splitting a leaf node, do copy-up, when splitting an internal node, do push-up.
    // - Split() distributes key&value pairs. Half key&value pairs are moved to the new node. For internal page, new
    //   page's 0-th element's key is filled (and effectively ignored), this is correct! This key can be considered
    //   removed the B+ tree (because this key is not used for lookup).
    // - InsertNodeAfter() adds middle_key to the parent. Both copy-up and push-up achieved.

    page_id_t parent_page_id = old_node->GetParentPageId();
    parent_page = buffer_pool_manager_->FetchPage(parent_page_id);  // parent pinned
    InternalPage *parent_node = reinterpret_cast<InternalPage *>(parent_page->GetData());

    // <key, old node's page id> is one entry in the parent page, Insert <new key, new node's page id> after that.
    //    <key, old node> ...   ->   <key, old node> <new key, new node> ...
    // where new key is the minimum key of the new node.
    parent_node->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());

    if (parent_node->GetSize() == MaxSize(parent_node) + 1) {
      // no need to wlatch new_parent_node
      InternalPage *new_parent_node = Split(parent_node);  // new_parent_node pinned

      // The parent of parent_node must have been latched (or needs to be created), as parent_node cannot be safe.
      KeyType middle_key = new_parent_node->KeyAt(0);
      InsertIntoParent(parent_node, middle_key, new_parent_node);

      buffer_pool_manager_->UnpinPage(new_parent_node->GetPageId(), true);  // unpin new parent
    }

    buffer_pool_manager_->UnpinPage(parent_node->GetPageId(), true);  // unpin parent
  }
}

//*****************************************************************************
//* REMOVE
//*****************************************************************************

/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE::Remove(const KeyType &key, Transaction *transaction) {
  if (IsEmpty()) {
    throw std::runtime_error("Empty tree");
  }

  bool allocated = false;
  if (transaction == nullptr) {
    transaction = new Transaction(0);
    allocated = true;
  }

  std::unique_lock root_lock(root_mutex_);                                   // locked, guaranteed unlock before return
  auto leaf_page = FindLeafPageRW(key, false, OpType::Remove, transaction);  // leaf_page pinned, page(s) latched

  bool root_page_safe = transaction->GetPageSet()->front()->GetPageId() != root_page_id_;
  if (root_page_safe) {
    root_lock.unlock();
  }

  LeafPage *leaf_node = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  page_id_t leaf_page_id = leaf_node->GetPageId();

  leaf_node->RemoveAndDeleteRecord(key);

  if (leaf_node->GetSize() < MinSize(leaf_node)) {
    bool delete_leaf = CoalesceOrRedistribute(leaf_node, transaction, key);  // leaf_page will be unpinned
    if (delete_leaf) {
      buffer_pool_manager_->DeletePage(leaf_page_id);
    }
  }

  UnlatchAndUnpin(OpType::Remove, transaction);

  if (allocated) {
    delete transaction;
  }
}

/*
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no deletion happens
 *
 * node will be unpinned if coalesce/redistribute succeeds.
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
bool BPLUSTREE::CoalesceOrRedistribute(N *node, Transaction *txn, const KeyType &key) {
  if (node->IsRootPage()) {
    if (node->GetSize() <= 1) {
      // The root page might change if node's size <= 1:
      // when root is internal page, when size=1, its only child is promoted as the new root;
      // when root is leaf page and size=1, do nothing;
      // when root is leaf page and size=0, delete the only page.
      BPlusTreePage *old_root = reinterpret_cast<BPlusTreePage *>(node);

      // If root is not safe, root_mutex_ is already held
      bool del_root = AdjustRoot(old_root);

      if (del_root) {
        buffer_pool_manager_->UnpinPage(old_root->GetPageId(), true);
        buffer_pool_manager_->DeletePage(old_root->GetPageId());
      }
    }

    return false;
  }

  // parent is already latched
  page_id_t parent_page_id = node->GetParentPageId();
  auto parent_page = buffer_pool_manager_->FetchPage(parent_page_id);  // parent_page pinned
  InternalPage *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());

  int index_in_parent = parent->ValueIndex(node->GetPageId());
  int left_sib_index = index_in_parent - 1;
  int right_sib_index = index_in_parent + 1;

  // Coalesce wth left sibling
  if (left_sib_index >= 0) {
    int left_sib_page_id = parent->ValueAt(left_sib_index);
    auto left_sib_page = buffer_pool_manager_->FetchPage(left_sib_page_id);  // left_sib_page pinned
    left_sib_page->WLatch();
    N *left_sib = reinterpret_cast<N *>(left_sib_page->GetData());

    if (FitOne(left_sib, node)) {
      bool del_parent = Coalesce(&left_sib, &node, &parent, index_in_parent, txn);  // node unpinned and deleted

      left_sib_page->WUnlatch();
      buffer_pool_manager_->UnpinPage(parent_page_id, true);    // parent_page unpinned
      buffer_pool_manager_->UnpinPage(left_sib_page_id, true);  // left_sib_page unpinned
      if (del_parent) {
        buffer_pool_manager_->DeletePage(parent_page_id);
      }

      return false;  // node is merged into left_sib, and already deleted
    }
    left_sib_page->WUnlatch();

    buffer_pool_manager_->UnpinPage(left_sib_page_id, false);  // left_sib_page unpinned
  }

  // Coalesce with right sibling
  if (right_sib_index < parent->GetSize()) {
    int right_sib_page_id = parent->ValueAt(right_sib_index);
    auto right_sib_page = buffer_pool_manager_->FetchPage(right_sib_page_id);  // right_sib_page pinned
    right_sib_page->WLatch();
    N *right_sib = reinterpret_cast<N *>(right_sib_page->GetData());

    if (FitOne(right_sib, node)) {
      bool del_parent = Coalesce(&node, &right_sib, &parent, right_sib_index, txn);  // right_sib unpinned and deleted

      right_sib_page->WUnlatch();
      buffer_pool_manager_->UnpinPage(parent_page_id, true);     // parent_page unpinned
      buffer_pool_manager_->UnpinPage(node->GetPageId(), true);  // node unpinned
      if (del_parent) {
        buffer_pool_manager_->DeletePage(parent_page_id);
      }

      return false;  // right_sib is deleted, and node shouldn't be deleted
    }

    right_sib_page->WUnlatch();
    buffer_pool_manager_->UnpinPage(right_sib_page_id, false);  // right_sib_page unpinned
  }

  // Redistribute from left sibling
  if (left_sib_index >= 0) {
    int left_sib_page_id = parent->ValueAt(left_sib_index);
    auto left_sib_page = buffer_pool_manager_->FetchPage(left_sib_page_id);  // left_sib_page pinned
    left_sib_page->WLatch();
    N *left_sib = reinterpret_cast<N *>(left_sib_page->GetData());

    if (left_sib->GetSize() > MinSize(left_sib)) {
      // std::cout << strf("-(%d): borrow from page %d (l) to page %d\n", key, left_sib->GetPageId(),
      // node->GetPageId());
      Redistribute(left_sib, node, -1);  // Move left_sib's last key&value pair to the head of node

      left_sib_page->WUnlatch();
      buffer_pool_manager_->UnpinPage(parent_page_id, true);     // parent_page unpin
      buffer_pool_manager_->UnpinPage(node->GetPageId(), true);  // node unpin
      buffer_pool_manager_->UnpinPage(left_sib_page_id, true);   // left_sib_page unpin
      return false;
    }

    left_sib_page->WUnlatch();
    buffer_pool_manager_->UnpinPage(left_sib_page_id, false);  // left_sib_page unpinned
  }

  // Redistribute from right sibling
  if (right_sib_index < parent->GetSize()) {
    int right_sib_page_id = parent->ValueAt(right_sib_index);
    auto right_sib_page = buffer_pool_manager_->FetchPage(right_sib_page_id);  // right_sib_page pinned
    right_sib_page->WLatch();
    N *right_sib = reinterpret_cast<N *>(right_sib_page->GetData());

    if (right_sib->GetSize() > MinSize(right_sib)) {
      // std::cout << strf("-(%d): borrow from page %d (r) to page %d\n",key, right_sib->GetPageId(),
      // node->GetPageId());
      Redistribute(right_sib, node, 0);  // Move right_sib's first key&value pair to the head of node

      right_sib_page->WUnlatch();
      buffer_pool_manager_->UnpinPage(parent_page_id, true);     // parent_page unpin
      buffer_pool_manager_->UnpinPage(node->GetPageId(), true);  // node unpin
      buffer_pool_manager_->UnpinPage(right_sib_page_id, true);  // right_sib_page unpin
      return false;
    }

    right_sib_page->WUnlatch();
    buffer_pool_manager_->UnpinPage(right_sib_page_id, false);  // right_sib_page unpinned
  }

  throw std::runtime_error("Not root, cannot merge, cannot redistribute? This shouldn't happen");
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method CoalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @para    index              index of key in parent that should be removed.
 * @return  true means parent node should be deleted, false means no deletion happens
 *
 * neighbor_node, node, and parent are assumed to be pinned.
 * Node will be merged into neighbor_node, so node will be unpinned and deleted.
 * This function doesn't operate on the buffer of neighbor_node and parent.
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
bool BPLUSTREE::Coalesce(N **neighbor_node, N **node, BPlusTreeInternalPage<KeyType, page_id_t> **parent, int index,
                         Transaction *transaction) {
  // Assume that *neighbor_node is the left sibling of *node

  // Move entries from node to neighbor_node
  if ((*node)->IsLeafPage()) {
    LeafPage *leaf_node = reinterpret_cast<LeafPage *>(*node);
    LeafPage *leaf_neighbor = reinterpret_cast<LeafPage *>(*neighbor_node);

    leaf_node->MoveAllTo(leaf_neighbor);
    leaf_neighbor->SetNextPageId(leaf_node->GetNextPageId());
  } else {
    InternalPage *internal_node = reinterpret_cast<InternalPage *>(*node);
    InternalPage *internal_neighbor = reinterpret_cast<InternalPage *>(*neighbor_node);

    internal_node->MoveAllTo(internal_neighbor, (*parent)->KeyAt(index), buffer_pool_manager_);
  }

  buffer_pool_manager_->UnpinPage((*node)->GetPageId(), true);
  buffer_pool_manager_->DeletePage((*node)->GetPageId());

  (*parent)->Remove(index);

  // If parent is underfull, recursive operation
  if ((*parent)->GetSize() < MinSize(*parent)) {
    return CoalesceOrRedistribute(*parent, transaction, KeyType{});
  }

  return false;
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method CoalesceOrRedistribute()
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
void BPLUSTREE::Redistribute(N *neighbor_node, N *node, int index) {
  // Assume neighbor_node and node are pinned
  page_id_t parent_page_id = node->GetParentPageId();
  auto parent_page = buffer_pool_manager_->FetchPage(parent_page_id);

  if (node->IsLeafPage()) {
    LeafPage *leaf_node = reinterpret_cast<LeafPage *>(node);
    LeafPage *leaf_neighbor = reinterpret_cast<LeafPage *>(neighbor_node);
    InternalPage *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());

    if (index == 0) {  // neighbor_node is the right sibling
      leaf_neighbor->MoveFirstToEndOf(leaf_node);

      int update_index = parent->ValueIndex(leaf_neighbor->GetPageId());
      parent->SetKeyAt(update_index, leaf_neighbor->KeyAt(0));
    } else {  // neighbor_node is the left sibling
      leaf_neighbor->MoveLastToFrontOf(leaf_node);

      int update_index = parent->ValueIndex(leaf_node->GetPageId());
      parent->SetKeyAt(update_index, leaf_node->KeyAt(0));
    }
  } else {
    InternalPage *internal_node = reinterpret_cast<InternalPage *>(node);
    InternalPage *internal_neighbor = reinterpret_cast<InternalPage *>(neighbor_node);
    InternalPage *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());

    if (index == 0) {  // neighbor_node is the right sibling
      KeyType middle_key = parent->KeyAt(parent->ValueIndex(internal_neighbor->GetPageId()));
      internal_neighbor->MoveFirstToEndOf(internal_node, middle_key, buffer_pool_manager_);

      int update_index = parent->ValueIndex(internal_neighbor->GetPageId());
      parent->SetKeyAt(update_index, internal_neighbor->KeyAt(0));
    } else {  // neighbor_node is the left sibling
      KeyType middle_key = parent->KeyAt(parent->ValueIndex(internal_node->GetPageId()));
      internal_neighbor->MoveLastToFrontOf(internal_node, middle_key, buffer_pool_manager_);

      int update_index = parent->ValueIndex(internal_node->GetPageId());
      parent->SetKeyAt(update_index, internal_node->KeyAt(0));
    }
  }
}

/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within CoalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happens
 *
 * The caller should hold root_mutex_ throughout the call.
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE::AdjustRoot(BPlusTreePage *old_root_node) {
  // for case 1, old_root_node (internal node)  has size 1
  // for case 2, old_root_node (leaf node) has size 0
  // In other case, old_root_node should be deleted

  // case 1: old_root_node has only one pointer, to the only leaf page. Make the leaf page as the new root.
  if (!old_root_node->IsLeafPage() && old_root_node->GetSize() == 1) {
    InternalPage *internal_root = reinterpret_cast<InternalPage *>(old_root_node);
    page_id_t new_root_page_id = internal_root->RemoveAndReturnOnlyChild();

    root_page_id_ = new_root_page_id;
    UpdateRootPageId(0);

    auto new_root_page = buffer_pool_manager_->FetchPage(new_root_page_id);
    BPlusTreePage *new_root = reinterpret_cast<BPlusTreePage *>(new_root_page->GetData());
    new_root->SetParentPageId(INVALID_PAGE_ID);
    buffer_pool_manager_->UnpinPage(new_root_page_id, true);

    return true;
  }

  // case 2: all elements deleted from the B+ tree
  if (old_root_node->IsLeafPage() && old_root_node->GetSize() == 0) {
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId(0);
    return true;
  }

  return false;
}

/*
 * Checks if entries in node1 and node2 fits into one leaf node. N represents both internal node and leaf node.
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
bool BPLUSTREE::FitOne(N *node1, N *node2) {
  if (node1->IsLeafPage()) {
    return node1->GetSize() + node2->GetSize() <= leaf_max_size_ - 1;
  }

  return node1->GetSize() + node2->GetSize() <= internal_max_size_;
}

/*
 * If the node is safe, in the context of concurrency control.
 * This should only be called for Insert or Remove.
 *
 * @param op  Can only be OpType::Insert or OpType:Remove
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
bool BPLUSTREE::IsSafe(N *node, enum OpType op) {
  // insert
  if (op == OpType::Insert) {
    return node->GetSize() < MaxSize(node);
  }

  // remove
  if (node->IsRootPage()) {
    // If root is a leaf node, no constraint
    // If root is an internal node, it must have at least two pointers;
    if (node->IsLeafPage()) {
      return true;
    }

    return node->GetSize() > 2;
  }

  return node->GetSize() > MinSize(node);
}

/*
 * Find leaf page containing particular key.
 * It's similar to FindLeafPage, but with concurrency control for read/write operations.
 *
 * Index iterator is not tested for concurrent access, so they use FindLeafPage.
 * Other read/write operations to the B+ tree should use this function, with concurrency control.
 *
 * root_mutex_ is held throughout the call, to avoid deadlock.
 *
 * @param latched latched page ids. This is not needed if read_only = true.
 */
INDEX_TEMPLATE_ARGUMENTS
std::shared_ptr<Page> BPLUSTREE::FindLeafPageRW(const KeyType &key, bool left_most, enum OpType op,
                                                Transaction *transaction) {
  for (auto page = buffer_pool_manager_->FetchPage(root_page_id_);;) {
    if (page == nullptr) {
      throw std::runtime_error("FetchPage returns nullptr");
    }

    BPlusTreePage *node = reinterpret_cast<BPlusTreePage *>(page->GetData());

    if (op == OpType::Read) {
      page->RLatch();
      UnlatchAndUnpin(op, transaction);
    } else {
      page->WLatch();
      if (IsSafe(node, op)) {
        UnlatchAndUnpin(op, transaction);
      }
    }
    transaction->AddIntoPageSet(page);

    if (node->IsLeafPage()) {
      return page;  // pinned, latched
    }

    InternalPage *internal = reinterpret_cast<InternalPage *>(node);
    page_id_t child_page_id = left_most ? internal->ValueAt(0) : internal->Lookup(key);
    page = buffer_pool_manager_->FetchPage(child_page_id);
  }
}

// Unlatch and unpin all pages in the PageSet of a transaction, according to the operation type.
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE::UnlatchAndUnpin(enum OpType op, Transaction *transaction) const {
  if (transaction == nullptr) {
    return;
  }

  auto pages = transaction->GetPageSet();
  for (auto page : *pages) {
    page_id_t page_id = page->GetPageId();

    if (op == OpType::Read) {
      page->RUnlatch();
      buffer_pool_manager_->UnpinPage(page_id, false);
    } else {
      page->WUnlatch();
      buffer_pool_manager_->UnpinPage(page_id, true);
    }
  }
  pages->clear();
}

/*
 * Returns the minimum size for B+ tree page. N represents both internal and leaf pages.
 * It's ok for a B+ tree node's size to be equal to the returned value.
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
int BPLUSTREE::MinSize(N *node) {
  // leaf_page: ceil((n-1)/2) = floor(n/2), internal_page: ceil(n/2) = floor((n+1)/2)
  return node->IsLeafPage() ? leaf_max_size_ / 2 : (internal_max_size_ + 1) / 2;
}

/*
 * Returns the maximum size for B+ tree page. N represents both internal and leaf pages.
 * It's ok for a B+ tree node's size to be equal to the returned value.
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
int BPLUSTREE::MaxSize(N *node) {
  return node->IsLeafPage() ? leaf_max_size_ - 1 : internal_max_size_;
}

//****************************************************************************
//* UTILITIES AND DEBUG
//****************************************************************************

/*
 * Find leaf page containing particular key, if left_most flag == true, find
 * the left most leaf page.
 *
 * This is implemented for test purpose. You should use FindLeafPageRW.
 */
INDEX_TEMPLATE_ARGUMENTS
std::shared_ptr<Page> BPLUSTREE::FindLeafPage(const KeyType &key, bool left_most) {
  auto page = buffer_pool_manager_->FetchPage(root_page_id_);
  BPlusTreePage *node = reinterpret_cast<BPlusTreePage *>(page->GetData());

  while (!node->IsLeafPage()) {
    InternalPage *internal_node = reinterpret_cast<InternalPage *>(node);
    page_id_t next_page_id = left_most ? internal_node->ValueAt(0) : internal_node->Lookup(key);

    auto next_page = buffer_pool_manager_->FetchPage(next_page_id);  // next_level_page pinned
    BPlusTreePage *next_node = reinterpret_cast<BPlusTreePage *>(next_page->GetData());

    buffer_pool_manager_->UnpinPage(node->GetPageId(), false);  // curr_node unpinned

    page = next_page;
    node = next_node;
  }

  return page;  // pinned
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE::UpdateRootPageId(int insert_record) {}

template class BPlusTree<key_t, value_t>;
// Phase 2: the tree as a pure index — EncodedKey keys, RID values pointing into
// the TupleStore. Internal pages use page_id_t child pointers (see BPlusTree.h).
template class BPlusTree<EncodedKey, RID>;
}  // namespace dbplay

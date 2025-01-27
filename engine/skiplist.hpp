/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#pragma once

#include <algorithm>
#include <assert.h>
#include <cstdint>

#include "hash_table.hpp"
#include "kvdk/engine.hpp"
#include "structures.hpp"
#include "utils.hpp"

namespace KVDK_NAMESPACE {
static const int kMaxHeight = MAX_SKIPLIST_LEVEL;
static const uint16_t kCacheLevel = 3;

/* Format:
 * next pointers | DataEntry on pmem | level | cached key size | cached key
 * We only cache key if level > kCache level or there are enough space in
 * the end of malloced space to cache the key (4B here).
 * */
struct SkiplistNode {
public:
  std::atomic<SkiplistNode *> next[0];
  DLDataEntry *data_entry; // data entry on pmem
  // TODO: save memory
  uint16_t height;
  uint16_t cached_key_size;
  char cached_key[0];

  static void DeleteNode(SkiplistNode *node) { free(node->heap_space_start()); }

  static SkiplistNode *NewNode(const Slice &key, DLDataEntry *entry_on_pmem,
                               uint16_t l) {
    size_t size;
    if (l >= kCacheLevel && key.size() > 4) {
      size = sizeof(SkiplistNode) + 8 * l + key.size() - 4;
    } else {
      size = sizeof(SkiplistNode) + 8 * l;
    }
    void *space = malloc(size);
    SkiplistNode *node = (SkiplistNode *)((char *)space + 8 * l);
    if (node != nullptr) {
      node->data_entry = entry_on_pmem;
      node->height = l;
      node->MaybeCacheKey(key);
    }
    return node;
  }

  uint16_t Height() { return height; }

  Slice Key();

  SkiplistNode *Next(int l) { return next[-l].load(std::memory_order_acquire); }

  bool CASNext(int l, SkiplistNode *expected, SkiplistNode *x) {
    assert(l > 0);
    return (next[-l].compare_exchange_strong(expected, x));
  }

  SkiplistNode *RelaxedNext(int l) {
    assert(l > 0);
    return next[-l].load(std::memory_order_relaxed);
  }

  void SetNext(int l, SkiplistNode *x) {
    assert(l > 0);
    next[-l].store(x, std::memory_order_release);
  }

  void RelaxedSetNext(int l, SkiplistNode *x) {
    assert(l > 0);
    next[-l].store(x, std::memory_order_relaxed);
  }

private:
  SkiplistNode() {}

  void MaybeCacheKey(const Slice &key) {
    if (height >= kCacheLevel || key.size() <= 4) {
      cached_key_size = key.size();
      memcpy(cached_key, key.data(), key.size());
    } else {
      cached_key_size = 0;
    }
  }

  void *heap_space_start() { return (char *)this - height * 8; }
};

class Skiplist : public PersistentList {
public:
  Skiplist(DLDataEntry *h, const std::string &n, uint64_t i,
           const std::shared_ptr<PMEMAllocator> &pmem_allocator,
           std::shared_ptr<HashTable> hash_table)
      : name_(n), id_(i), pmem_allocator_(pmem_allocator),
        hash_table_(hash_table) {
    header_ = SkiplistNode::NewNode(n, h, kMaxHeight);
    for (int i = 1; i <= kMaxHeight; i++) {
      header_->RelaxedSetNext(i, nullptr);
    }
  }

  ~Skiplist() {
    if (header_) {
      SkiplistNode *to_delete = header_;
      while (to_delete) {
        SkiplistNode *next = to_delete->Next(1);
        SkiplistNode::DeleteNode(to_delete);
        to_delete = next;
      }
    }
  }

  uint64_t id() override { return id_; }

  const std::string &name() { return name_; }

  SkiplistNode *header() { return header_; }

  static int RandomHeight() {
    int height = 0;
    while (height < kMaxHeight && fast_random() & 1) {
      height++;
    }

    return height;
  }

  inline static Slice UserKey(const Slice &skiplist_key) {
    return Slice(skiplist_key.data() + 8, skiplist_key.size() - 8);
  }

  struct Splice {
    SkiplistNode *nexts[kMaxHeight + 1];
    SkiplistNode *prevs[kMaxHeight + 1];
    DLDataEntry *prev_data_entry;
    DLDataEntry *next_data_entry;

    void Recompute(const Slice &key, int l) {
      while (1) {
        SkiplistNode *tmp = prevs[l]->Next(l);
        if (tmp == nullptr) {
          nexts[l] = nullptr;
          break;
        }

        int cmp = Slice::compare(key, tmp->Key());

        if (cmp > 0) {
          prevs[l] = tmp;
        } else {
          nexts[l] = tmp;
          break;
        }
      }
    }
  };

  void Seek(const Slice &key, Splice *splice);

  Status Rebuild();

  bool FindAndLockWritePos(Splice *splice, const Slice &insert_key,
                           const HashTable::KeyHashHint &hint,
                           std::vector<SpinMutex *> &spins,
                           DLDataEntry *updated_data_entry);

  void *InsertDataEntry(Splice *insert_splice, DLDataEntry *inserting_entry,
                        const Slice &inserting_key, SkiplistNode *node);

  void DeleteDataEntry(Splice *delete_splice, const Slice &deleting_key,
                       SkiplistNode *node);

private:
  SkiplistNode *header_;
  std::string name_;
  uint64_t id_;
  std::shared_ptr<HashTable> hash_table_;
  std::shared_ptr<PMEMAllocator> pmem_allocator_;
};

class SortedIterator : public Iterator {
public:
  SortedIterator(Skiplist *skiplist,
                 const std::shared_ptr<PMEMAllocator> &pmem_allocator)
      : skiplist_(skiplist), pmem_allocator_(pmem_allocator), current(nullptr) {
  }

  virtual void Seek(const std::string &key) override {
    assert(skiplist_);
    Skiplist::Splice splice;
    skiplist_->Seek(key, &splice);
    current = splice.next_data_entry;
  }

  virtual void SeekToFirst() override {
    uint64_t first = skiplist_->header()->data_entry->next;
    current = (DLDataEntry *)pmem_allocator_->offset2addr(first);
  }

  virtual bool Valid() override { return current != nullptr; }

  virtual bool Next() override {
    if (!Valid()) {
      return false;
    }
    do {
      current = (DLDataEntry *)pmem_allocator_->offset2addr(current->next);
    } while (current && current->type == SORTED_DELETE_RECORD);
    return current != nullptr;
  }

  virtual bool Prev() override {
    if (!Valid()) {
      return false;
    }

    do {
      current = (DLDataEntry *)(pmem_allocator_->offset2addr(current->prev));
    } while (current->type == SORTED_DELETE_RECORD);

    if (current == skiplist_->header()->data_entry) {
      current = nullptr;
      return false;
    }

    return true;
  }

  virtual std::string Key() override {
    if (!Valid())
      return "";
    return Skiplist::UserKey(current->Key()).to_string();
  }

  virtual std::string Value() override {
    if (!Valid())
      return "";
    return current->Value().to_string();
  }

private:
  Skiplist *skiplist_;
  std::shared_ptr<PMEMAllocator> pmem_allocator_;
  DLDataEntry *current;
};
} // namespace KVDK_NAMESPACE
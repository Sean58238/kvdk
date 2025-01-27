/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#pragma once

#include <cstdio>
#include <vector>

#include "data_entry.hpp"
#include "dram_allocator.hpp"
#include "kvdk/engine.hpp"
#include "pmem_allocator.hpp"
#include "structures.hpp"

namespace KVDK_NAMESPACE {

class HashTable {
public:
  struct KeyHashHint {
    uint64_t key_hash_value;
    uint32_t bucket;
    uint32_t slot;
    SpinMutex *spin;
  };

  HashTable(uint64_t hash_bucket_num, uint32_t hash_bucket_size,
            uint32_t slot_grain,
            const std::shared_ptr<PMEMAllocator> &pmem_allocator,
            uint32_t write_threads)
      : num_hash_buckets_(hash_bucket_num), slot_grain_(slot_grain),
        hash_bucket_size_(hash_bucket_size),
        dram_allocator_(new DRAMAllocator(write_threads)),
        pmem_allocator_(pmem_allocator),
        num_entries_per_bucket_((hash_bucket_size_ - 8 /* next pointer */) /
                                sizeof(HashEntry)) {
    main_buckets_ = dram_allocator_->offset2addr(
        (dram_allocator_->Allocate(hash_bucket_size * hash_bucket_num)
             .space_entry.offset));
    //    memset(main_buckets_, 0, hash_bucket_size * hash_bucket_num);
    slots_.resize(hash_bucket_num / slot_grain);
    hash_bucket_entries_.resize(hash_bucket_num, 0);
  }

  KeyHashHint GetHint(const Slice &key) {
    KeyHashHint hint;
    hint.key_hash_value = hash_str(key.data(), key.size());
    hint.bucket = get_bucket_num(hint.key_hash_value);
    hint.slot = get_slot_num(hint.bucket);
    hint.spin = &slots_[hint.slot].spin;
    return hint;
  }

  Status Search(const KeyHashHint &hint, const Slice &key, uint16_t type_mask,
                HashEntry *hash_entry, DataEntry *data_entry,
                HashEntry **entry_base, bool search_for_write);

  void Insert(const KeyHashHint &hint, HashEntry *entry_base, uint16_t type,
              uint64_t offset, bool is_update);

private:
  inline uint32_t get_bucket_num(uint64_t key_hash_value) {
    return key_hash_value & (num_hash_buckets_ - 1);
  }

  inline uint32_t get_slot_num(uint32_t bucket) { return bucket / slot_grain_; }

  bool MatchHashEntry(const Slice &key, uint32_t hash_k_prefix,
                      uint16_t target_type, const HashEntry *hash_entry,
                      void *data_entry);

  std::vector<uint64_t> hash_bucket_entries_;
  const uint64_t num_hash_buckets_;
  const uint32_t slot_grain_;
  const uint32_t hash_bucket_size_;
  const uint64_t num_entries_per_bucket_;
  std::vector<Slot> slots_;
  std::shared_ptr<PMEMAllocator> pmem_allocator_;
  std::unique_ptr<DRAMAllocator> dram_allocator_;
  char *main_buckets_;
};
} // namespace KVDK_NAMESPACE
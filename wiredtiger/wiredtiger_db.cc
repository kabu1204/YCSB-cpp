// Copyright 2023 Chengye YU <yuchengye2013 AT outlook.com>.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <sys/stat.h>
#if defined(_MSC_VER)
#include "direct.h"
#define mkdir(x, y) _mkdir(x)
#endif

#include "core/core_workload.h"
#include "core/db_factory.h"
#include "core/properties.h"
#include "core/utils.h"

#include "wiredtiger_db.h"

#define WT_PREFIX "wiredtiger"
#define STR(x) #x

#define error_check(x)                                                                                 \
  {                                                                                                    \
    if ((x) != 0) {                                                                                    \
      throw utils::Exception(std::string("[" WT_PREFIX "] " __FILE__ ":") + std::to_string(__LINE__)); \
    }                                                                                                  \
  }


namespace {
  const std::string PROP_HOME = WT_PREFIX ".home";
  const std::string PROP_HOME_DEFAULT = "";

  const std::string PROP_FORMAT = WT_PREFIX ".format";
  const std::string PROP_FORMAT_DEFAULT = "single";

  const std::string PROP_CACHE_SIZE = WT_PREFIX ".cache_size";
  const std::string PROP_CACHE_SIZE_DEFAULT = "100MB";

  const std::string PROP_DIRECT_IO = WT_PREFIX ".direct_io";
  const std::string PROP_DIRECT_IO_DEFAULT = "[]";

  const std::string PROP_IN_MEMORY = WT_PREFIX ".in_memory";
  const std::string PROP_IN_MEMORY_DEFAULT = "false";

  const std::string PROP_LSM_MGR_MERGE = WT_PREFIX ".lsm_mgr.merge";
  const std::string PROP_LSM_MGR_MERGE_DEFAULT = "true";

  const std::string PROP_LSM_MGR_MAX_WORKERS = WT_PREFIX ".lsm_mgr.max_workers";
  const std::string PROP_LSM_MGR_MAX_WORKERS_DEFAULT = "4";

  const std::string PROP_BLK_MGR_ALLOCATION_SIZE = WT_PREFIX ".blk_mgr.allocation_size";
  const std::string PROP_BLK_MGR_ALLOCATION_SIZE_DEFAULT = "4KB";

  const std::string PROP_BLK_MGR_BLOOM_BIT_COUNT = WT_PREFIX ".blk_mgr.bloom_bit_count";
  const std::string PROP_BLK_MGR_BLOOM_BIT_COUNT_DEFAULT = "16";

  const std::string PROP_BLK_MGR_BLOOM_HASH_COUNT = WT_PREFIX ".blk_mgr.bloom_hash_count";
  const std::string PROP_BLK_MGR_BLOOM_HASH_COUNT_DEFAULT = "8";

  const std::string PROP_BLK_MGR_CHUNK_MAX = WT_PREFIX ".blk_mgr.chunk_max";
  const std::string PROP_BLK_MGR_CHUNK_MAX_DEFAULT = "5GB";

  const std::string PROP_BLK_MGR_CHUNK_SIZE = WT_PREFIX ".blk_mgr.chunk_size";
  const std::string PROP_BLK_MGR_CHUNK_SIZE_DEFAULT = "10MB";

  const std::string PROP_BLK_MGR_COMPRESSOR = WT_PREFIX ".blk_mgr.compressor";
  const std::string PROP_BLK_MGR_COMPRESSOR_DEFAULT = "snappy";

  const std::string PROP_BLK_MGR_BTREE_INTERNAL_PAGE_MAX = WT_PREFIX ".blk_mgr.btree.internal_page_max";
  const std::string PROP_BLK_MGR_BTREE_INTERNAL_PAGE_MAX_DEFAULT = "4KB";

  const std::string PROP_BLK_MGR_BTREE_LEAF_KEY_MAX = WT_PREFIX ".blk_mgr.btree.leaf_key_max";
  const std::string PROP_BLK_MGR_BTREE_LEAF_KEY_MAX_DEFAULT = "0";

  const std::string PROP_BLK_MGR_BTREE_LEAF_VALUE_MAX = WT_PREFIX ".blk_mgr.btree.leaf_value_max";
  const std::string PROP_BLK_MGR_BTREE_LEAF_VALUE_MAX_DEFAULT = "0";

  const std::string PROP_BLK_MGR_BTREE_LEAF_PAGE_MAX = WT_PREFIX ".blk_mgr.btree.leaf_page_max";
  const std::string PROP_BLK_MGR_BTREE_LEAF_PAGE_MAX_DEFAULT = "32KB";
}// namespace

namespace ycsbc {

  WT_CONNECTION *WTDB::conn_ = nullptr;
  int WTDB::ref_cnt_ = 0;
  std::mutex WTDB::mu_;
  std::atomic<uint64_t> WTDB::total_user_write_ = 0;
  std::atomic<uint64_t> WTDB::last_user_read_ = 0;
  std::atomic<uint64_t> WTDB::total_user_read_ = 0;
  std::atomic<uint64_t> WTDB::total_cache_write_ = 0;
  std::atomic<uint64_t> WTDB::total_cache_read_ = 0;
  std::atomic<uint64_t> WTDB::total_fs_write_ = 0;
  std::atomic<uint64_t> WTDB::total_fs_read_ = 0;
  std::atomic<uint64_t> WTDB::total_leaf_splits_ = 0;
  std::atomic<uint64_t> WTDB::total_internal_splits = 0;

  void WTDB::Init() {
    const std::lock_guard<std::mutex> lock(mu_);

    const utils::Properties &props = *props_;
    const std::string &format = props.GetProperty(PROP_FORMAT, PROP_FORMAT_DEFAULT);
    fieldcount_ = std::stoi(props.GetProperty(CoreWorkload::FIELD_COUNT_PROPERTY,
                                              CoreWorkload::FIELD_COUNT_DEFAULT));

    if (format == "single") {
      method_read_ = &WTDB::ReadSingleEntry;
      method_scan_ = &WTDB::ScanSingleEntry;
      method_update_ = &WTDB::UpdateSingleEntry;
      method_insert_ = &WTDB::InsertSingleEntry;
      method_delete_ = &WTDB::DeleteSingleEntry;
    } else {
      throw utils::Exception("single ONLY");
    }

    ref_cnt_++;
    if (conn_) {
      error_check(conn_->open_session(conn_, NULL, NULL, &session_));
      error_check(session_->open_cursor(session_, "table:ycsbc", NULL, "overwrite=true", &cursor_));
      return;
    }

    // Open connection (once, per process)
    {
      // 1. Setup wiredtiger home directory
      const std::string &home = props.GetProperty(PROP_HOME, PROP_HOME_DEFAULT);
      if (home.empty()) {
        throw utils::Exception(WT_PREFIX " home is missing");
      }
      int ret = mkdir(home.c_str(), 0775);
      if (ret && errno != EEXIST) {
        throw utils::Exception(std::string("Init mkdir: ") + strerror(errno));
      }

      // 2. Setup db config
      std::string db_config("create,");
      {// 2.1 General
        const std::string &cache_size = props.GetProperty(PROP_CACHE_SIZE, PROP_CACHE_SIZE_DEFAULT);
        const std::string &direct_io = props.GetProperty(PROP_DIRECT_IO, PROP_DIRECT_IO_DEFAULT);
        const std::string &in_memory = props.GetProperty(PROP_IN_MEMORY, PROP_IN_MEMORY_DEFAULT);
        if (!cache_size.empty()) db_config += "cache_size=" + cache_size + ",";
        if (!direct_io.empty()) db_config += "direct_io=" + direct_io + ",";
        if (!in_memory.empty()) db_config += "in_memory=" + in_memory + ",";
      }
      {// 2.2 LSM Manager
        std::string lsm_config;
        const std::string &lsm_merge = props.GetProperty(PROP_LSM_MGR_MERGE, PROP_LSM_MGR_MERGE_DEFAULT);
        const std::string &lsm_max_workers = props.GetProperty(PROP_LSM_MGR_MAX_WORKERS, PROP_LSM_MGR_MAX_WORKERS_DEFAULT);
        if (!lsm_merge.empty()) lsm_config += "merge=" + lsm_merge + ",";
        if (!lsm_max_workers.empty()) lsm_config += "worker_thread_max=" + lsm_max_workers;

        if (!lsm_config.empty()) db_config += "lsm_manager=(" + lsm_config + ")";
      }
//     db_config += ",block_cache=(enabled=true,hashsize=10240,size=64MB,system_ram=64MB,type=DRAM)";
#if defined(ENABLE_STAT)
      db_config += ",statistics=(fast,clear)";
#endif
      std::cout << "db config: " << db_config << std::endl;
      error_check(wiredtiger_open(home.c_str(), NULL, db_config.c_str(), &conn_));
    }

    // Open session (per thread)
    error_check(conn_->open_session(conn_, NULL, NULL, &session_));

    // Create table (once)
    {// 1. Setup block manager
      std::string table_config("key_format=u,value_format=u,");
      {// 1.1 General
        const std::string &alloc_size = props.GetProperty(PROP_BLK_MGR_ALLOCATION_SIZE, PROP_BLK_MGR_ALLOCATION_SIZE_DEFAULT);
        const std::string &compressor = props.GetProperty(PROP_BLK_MGR_COMPRESSOR, PROP_BLK_MGR_COMPRESSOR_DEFAULT);
        if (!alloc_size.empty()) table_config += "allocation_size=" + alloc_size + ",";
        if (!compressor.empty() && !std::set<std::string>{"snappy", "lz4", "zlib", "zstd"}.count(compressor)) {
          throw utils::Exception("unknown compressor name");
        } else
          table_config += "block_compressor=" + compressor + ",";
      }
      {// 1.2 LSM relevant
        std::string lsm_config;
        const std::string &bloom_bit_count = props.GetProperty(PROP_BLK_MGR_BLOOM_BIT_COUNT, PROP_BLK_MGR_BLOOM_BIT_COUNT_DEFAULT);
        const std::string &bloom_hash_count = props.GetProperty(PROP_BLK_MGR_BLOOM_HASH_COUNT, PROP_BLK_MGR_BLOOM_HASH_COUNT_DEFAULT);
        const std::string &chunk_max = props.GetProperty(PROP_BLK_MGR_CHUNK_MAX, PROP_BLK_MGR_CHUNK_MAX_DEFAULT);
        const std::string &chunk_size = props.GetProperty(PROP_BLK_MGR_CHUNK_SIZE, PROP_BLK_MGR_CHUNK_SIZE_DEFAULT);
        if (!bloom_bit_count.empty()) lsm_config += "bloom_bit_count=" + bloom_bit_count + ",";
        if (!bloom_hash_count.empty()) lsm_config += "bloom_hash_count=" + bloom_hash_count + ",";
        if (!chunk_max.empty()) lsm_config += "chunk_max=" + chunk_max + ",";
        if (!chunk_size.empty()) lsm_config += "chunk_size=" + chunk_size;

        if (!lsm_config.empty()) table_config += "lsm=(" + lsm_config + "),";
      }
      {// 1.3 BTree nodes
        const std::string &internal_page_max = props.GetProperty(PROP_BLK_MGR_BTREE_INTERNAL_PAGE_MAX, PROP_BLK_MGR_BTREE_INTERNAL_PAGE_MAX_DEFAULT);
        const std::string &leaf_key_max = props.GetProperty(PROP_BLK_MGR_BTREE_LEAF_KEY_MAX, PROP_BLK_MGR_BTREE_LEAF_KEY_MAX_DEFAULT);
        const std::string &leaf_value_max = props.GetProperty(PROP_BLK_MGR_BTREE_LEAF_VALUE_MAX, PROP_BLK_MGR_BTREE_LEAF_VALUE_MAX_DEFAULT);
        const std::string &leaf_page_max = props.GetProperty(PROP_BLK_MGR_BTREE_LEAF_PAGE_MAX, PROP_BLK_MGR_BTREE_LEAF_PAGE_MAX_DEFAULT);
        if (!internal_page_max.empty()) table_config += "internal_page_max=" + internal_page_max + ",";
        if (!leaf_key_max.empty()) table_config += "leaf_key_max=" + leaf_key_max + ",";
        if (!leaf_value_max.empty()) table_config += "leaf_value_max=" + leaf_value_max + ",";
        if (!leaf_page_max.empty()) table_config += "leaf_page_max=" + leaf_page_max;
      }
      std::cout << "table config: " << table_config << std::endl;
      error_check(session_->create(session_, "table:ycsbc", table_config.c_str()));
    }

    // Open cursor (per thread)
    error_check(session_->open_cursor(session_, "table:ycsbc", NULL, "overwrite=true", &cursor_));
  }

  void WTDB::Cleanup() {
    const std::lock_guard<std::mutex> lock(mu_);
    cursor_->close(cursor_);
    if (stat_cursor_) {
      stat_cursor_->close(stat_cursor_);
    }
    if (conn_cursor_) {
      conn_cursor_->close(conn_cursor_);
    }
    error_check(session_->close(session_, NULL));
    if (--ref_cnt_) {
      return;
    }
    error_check(conn_->close(conn_, NULL));
  }

  DB::Status WTDB::ReadSingleEntry(const std::string &table, const std::string &key,
                                   const std::vector<std::string> *fields,
                                   std::vector<Field> &result) {
    WT_ITEM k = {key.data(), key.size()};
    WT_ITEM v;
    int ret;
    cursor_->set_key(cursor_, &k);
    ret = cursor_->search(cursor_);
    if (ret == WT_NOTFOUND) {
      return kNotFound;
    } else if (ret != 0) {
      throw utils::Exception(WT_PREFIX " search error");
    }
    error_check(cursor_->get_value(cursor_, &v));
    if (fields != nullptr) {
      DeserializeRowFilter(&result, (const char *) v.data, v.size, *fields);
    } else {
      DeserializeRow(&result, (const char *) v.data, v.size);
    }
#if defined(ENABLE_STAT)
    last_user_read_ += v.size;
#endif
    return kOK;
  }

  DB::Status WTDB::ScanSingleEntry(const std::string &table, const std::string &key, int len,
                                   const std::vector<std::string> *fields,
                                   std::vector<std::vector<Field>> &result) {
    WT_ITEM k = {key.data(), key.size()};
    WT_ITEM v;
    int ret = 0, exact;

    cursor_->set_key(cursor_, &k);
    error_check(cursor_->search_near(cursor_, &exact));
    if (exact < 0) {
      ret = cursor_->next(cursor_);
    }
    for (int i = 0; !ret && i < len; ++i) {
      error_check(cursor_->get_value(cursor_, &v));
      result.emplace_back(std::vector<Field>());
      if (fields != nullptr) {
        DeserializeRowFilter(&result.back(), (const char *) v.data, v.size, *fields);
      } else {
        DeserializeRow(&result.back(), (const char *) v.data, v.size);
      }
#if defined(ENABLE_STAT)
      last_user_read_ += v.size;
#endif
    }
    return kOK;
  }

  DB::Status WTDB::UpdateSingleEntry(const std::string &table, const std::string &key,
                                     std::vector<Field> &values) {
    std::vector<Field> current_values;
    WT_ITEM k = {key.data(), key.size()};
    WT_ITEM v;
    int ret;

    cursor_->set_key(cursor_, &k);
    ret = cursor_->search(cursor_);
    if (ret == WT_NOTFOUND) {
      return kNotFound;
    } else if (ret != 0) {
      throw utils::Exception(WT_PREFIX " search error");
    }
    error_check(cursor_->get_value(cursor_, &v));
    DeserializeRow(&current_values, (const char *) v.data, v.size);
    for (Field &new_field: values) {
      bool found MAYBE_UNUSED = false;
      for (Field &cur_field: current_values) {
        if (cur_field.name == new_field.name) {
          found = true;
          cur_field.value = new_field.value;
          break;
        }
      }
      assert(found);
    }

    std::string data;
    SerializeRow(current_values, &data);
    v.data = data.data();
    v.size = data.size();
    cursor_->set_value(cursor_, &v);
    ret = cursor_->update(cursor_);
    if (ret == WT_NOTFOUND) {
      return kNotFound;
    } else if (ret != 0) {
      throw utils::Exception(WT_PREFIX " update error");
    }
    return kOK;
  }

  DB::Status WTDB::InsertSingleEntry(const std::string &table, const std::string &key,
                                     std::vector<Field> &values) {
    std::string data;
    WT_ITEM k = {key.data(), key.size()}, v;

    cursor_->set_key(cursor_, &k);
    SerializeRow(values, &data);
    v.data = data.data();
    v.size = data.size();
    cursor_->set_value(cursor_, &v);
    error_check(cursor_->insert(cursor_));
    // TODO: cursor reset?
    return kOK;
  }
  DB::Status WTDB::DeleteSingleEntry(const std::string &table, const std::string &key) {
    WT_ITEM k = {key.data(), key.size()};
    cursor_->set_key(cursor_, &k);
    error_check(cursor_->remove(cursor_));
    return kOK;
  }

  void WTDB::SerializeRow(const std::vector<Field> &values, std::string *data) {
    for (const Field &field: values) {
      uint32_t len = field.name.size();
      data->append(reinterpret_cast<char *>(&len), sizeof(uint32_t));// 4B
      data->append(field.name.data(), field.name.size());            // len(name)
      len = field.value.size();
      data->append(reinterpret_cast<char *>(&len), sizeof(uint32_t));// 4B
      data->append(field.value.data(), field.value.size());          // len(value)
    }
  }

  void WTDB::DeserializeRow(std::vector<Field> *values, const char *data_ptr, size_t data_len) {
    const char *p = data_ptr;
    const char *lim = p + data_len;
    while (p != lim) {
      assert(p < lim);
      uint32_t len = *reinterpret_cast<const uint32_t *>(p);
      p += sizeof(uint32_t);
      std::string field(p, static_cast<const size_t>(len));
      p += len;
      len = *reinterpret_cast<const uint32_t *>(p);
      p += sizeof(uint32_t);
      std::string value(p, static_cast<const size_t>(len));
      p += len;
      values->push_back({field, value});
    }
    assert(values->size() == fieldcount_);
  }

  void WTDB::DeserializeRowFilter(std::vector<Field> *values, const char *data_ptr, size_t data_len,
                                  const std::vector<std::string> &fields) {
    const char *p = data_ptr;
    const char *lim = p + data_len;
    std::vector<std::string>::const_iterator filter_iter = fields.begin();
    while (p != lim && filter_iter != fields.end()) {
      assert(p < lim);
      uint32_t len = *reinterpret_cast<const uint32_t *>(p);
      p += sizeof(uint32_t);
      std::string field(p, static_cast<const size_t>(len));
      p += len;
      len = *reinterpret_cast<const uint32_t *>(p);
      p += sizeof(uint32_t);
      std::string value(p, static_cast<const size_t>(len));
      p += len;
      if (*filter_iter == field) {
        values->push_back({field, value});
        filter_iter++;
      }
    }
    assert(values->size() == fields.size());
  }

  void get_stat(WT_CURSOR *c, int stat_field, int64_t *valuep) {
    const char *desc, *pvalue;

    c->set_key(c, stat_field);
    error_check(c->search(c));
    error_check(c->get_value(c, &desc, &pvalue, valuep));
  }

  static std::string Bytes2Size(uint64_t size) {
    char buf[16];
    if (size < (1 << 10)) {
      snprintf(buf, sizeof(buf), "%llu B", size);
      return {buf};
    } else if (size < (1 << 20)) {
      snprintf(buf, sizeof(buf), "%.2f KB", double(size) / 1024.0);
    } else if (size < (1 << 30)) {
      snprintf(buf, sizeof(buf), "%.2f MB", double(size) / (1024.0 * 1024.0));
    } else if (size < (1ull << 40)) {
      snprintf(buf, sizeof(buf), "%.2f GB",
               double(size) / (1024.0 * 1024.0 * 1024.0));
    } else {
      snprintf(buf, sizeof(buf), "%.2f TB",
               double(size) / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    }
    return buf;
  }

  void WTDB::PrintStat() {
    error_check(stat_cursor_->reset(stat_cursor_));
    error_check(conn_cursor_->reset(conn_cursor_));

    {
      int64_t app_insert, app_remove, app_update, cache_writes, blk_writes;
      uint64_t interval_user_write;

      get_stat(stat_cursor_, WT_STAT_DSRC_CURSOR_INSERT_BYTES, &app_insert);
      get_stat(stat_cursor_, WT_STAT_DSRC_CURSOR_REMOVE_BYTES, &app_remove);
      get_stat(stat_cursor_, WT_STAT_DSRC_CURSOR_UPDATE_BYTES, &app_update);

      get_stat(stat_cursor_, WT_STAT_DSRC_CACHE_BYTES_WRITE, &cache_writes);
      get_stat(conn_cursor_, WT_STAT_CONN_BLOCK_BYTE_WRITE, &blk_writes);

      interval_user_write = app_insert + app_remove + app_update;
      if ((interval_user_write) != 0) {
        printf("[Interval] Write amplification is %.2lf\n",
               (double) blk_writes / interval_user_write);
      }
      total_user_write_ += interval_user_write;
      total_cache_write_ += cache_writes;
      total_fs_write_ += blk_writes;
      printf("[Interval] USER_WRITE = %llu (%llu + %llu + %llu) (%s)\n",
             interval_user_write,
             app_insert, app_remove, app_update,
             Bytes2Size(interval_user_write).c_str());
      printf("[Interval] CACHE_BYTES_WRITE = %llu (%s)\n", cache_writes,
             Bytes2Size(cache_writes).c_str());
      printf("[Interval] BLOCK_BYTES_WRITE = %llu (%s)\n", blk_writes,
             Bytes2Size(blk_writes).c_str());
      printf("[Cumulative] USER_WRITE = %llu (%s)\n", total_user_write_.load(),
             Bytes2Size(total_user_write_.load()).c_str());
      printf("[Cumulative] CACHE_BYTES_WRITE = %llu (%s)\n", total_cache_write_.load(),
             Bytes2Size(total_cache_write_.load()).c_str());
      printf("[Cumulative] BLOCK_BYTES_WRITE = %llu (%s)\n", total_fs_write_.load(),
             Bytes2Size(total_fs_write_.load()).c_str());
    }
    {
      int64_t cache_read, app_read, blk_read;
      get_stat(stat_cursor_, WT_STAT_DSRC_CACHE_BYTES_READ, &cache_read);
      get_stat(conn_cursor_, WT_STAT_CONN_BLOCK_BYTE_READ, &blk_read);
      app_read = last_user_read_.exchange(0);
      if (app_read != 0) {
        printf("[Interval] Read amplification is %.2lf\n", (double) blk_read / app_read);
      }
      total_user_read_ += app_read;
      total_cache_read_ += cache_read;
      total_fs_read_ += blk_read;
      printf("[Interval] USER_READ = %llu (%s)\n", app_read,
             Bytes2Size(app_read).c_str());
      printf("[Interval] CACHE_BYTES_READ = %llu (%s)\n", cache_read,
             Bytes2Size(cache_read).c_str());
      printf("[Interval] BLOCK_BYTES_READ = %llu (%s)\n", blk_read,
             Bytes2Size(blk_read).c_str());
      printf("[Cumulative] USER_READ = %llu (%s)\n", total_user_read_.load(),
             Bytes2Size(total_user_read_.load()).c_str());
      printf("[Cumulative] CACHE_BYTES_READ = %llu (%s)\n", total_cache_read_.load(),
             Bytes2Size(total_cache_read_.load()).c_str());
      printf("[Cumulative] BLOCK_BYTES_READ = %llu (%s)\n", total_fs_read_.load(),
             Bytes2Size(total_fs_read_.load()).c_str());
    }
    {
      int64_t max_internal_page_size, max_leaf_page_size;
      get_stat(stat_cursor_, WT_STAT_DSRC_BTREE_MAXINTLPAGE, &max_internal_page_size);
      get_stat(stat_cursor_, WT_STAT_DSRC_BTREE_MAXLEAFPAGE, &max_leaf_page_size);
      printf("maximum internal page size(bytes): %lld\n", max_internal_page_size);
      printf("maximum leaf page size(bytes): %lld\n", max_leaf_page_size);
    }
    {
      int64_t internal_page_splits, leaf_page_splits, inmem_page_splits;
      get_stat(stat_cursor_, WT_STAT_DSRC_CACHE_INMEM_SPLIT, &inmem_page_splits);
      get_stat(stat_cursor_, WT_STAT_DSRC_CACHE_EVICTION_SPLIT_INTERNAL, &internal_page_splits);
      get_stat(stat_cursor_, WT_STAT_DSRC_CACHE_EVICTION_SPLIT_LEAF, &leaf_page_splits);
      total_internal_splits += internal_page_splits;
      total_leaf_splits_ += leaf_page_splits;

      printf("[Interval] in-memory page splits: %lld\n", inmem_page_splits);
      printf("[Interval] internal pages split during eviction: %lld\n", internal_page_splits);
      printf("[Interval] leaf pages split during eviction: %lld\n", leaf_page_splits);
      printf("[Cumulative] internal pages split during eviction: %lld\n", total_internal_splits.load());
      printf("[Cumulative] leaf pages split during eviction: %lld\n", total_leaf_splits_.load());
    }
  }
  void WTDB::InitStat() {
    error_check(session_->open_cursor(session_, "statistics:table:ycsbc", NULL, NULL, &stat_cursor_));
    error_check(session_->open_cursor(session_, "statistics:", NULL, NULL, &conn_cursor_));
  }

  DB *NewWTDB() {
    return new WTDB;
  }

  const bool registered = DBFactory::RegisterDB("wiredtiger", NewWTDB);

}// namespace ycsbc
// Copyright 2023 Chengye YU <yuchengye2013 AT outlook.com>.
// SPDX-License-Identifier: Apache-2.0

#ifndef _WIREDTIGER_DB_H
#define _WIREDTIGER_DB_H

#if defined(ENABLE_STAT)
enum WT_CUSTOM_STAT_ITEM {
    WT_CUSTOM_STAT_READ_BYTES = 0,

    WT_CUSTOM_STAT_NUM = 1
};
#endif

#include <string>
#include <mutex>

#include "core/db.h"
#include "core/properties.h"

#include "wiredtiger.h"
#include "wiredtiger_ext.h"

namespace ycsbc {

class WTDB : public DB {
 public:
  WTDB() {}
  ~WTDB() {}

  void Init();

  void Cleanup();

  Status Read(const std::string &table, const std::string &key,
              const std::vector<std::string> *fields, std::vector<Field> &result) {
    return (this->*(method_read_))(table, key, fields, result);
  }

  Status Scan(const std::string &table, const std::string &key, int len,
              const std::vector<std::string> *fields, std::vector<std::vector<Field>> &result) {
    return (this->*(method_scan_))(table, key, len, fields, result);
  }

  Status Update(const std::string &table, const std::string &key, std::vector<Field> &values) {
    return (this->*(method_update_))(table, key, values);
  }

  Status Insert(const std::string &table, const std::string &key, std::vector<Field> &values) {
    return (this->*(method_insert_))(table, key, values);
  }

  Status Delete(const std::string &table, const std::string &key) {
    return (this->*(method_delete_))(table, key);
  }

  void PrintStat() override;

  void InitStat() override;

 private:

  Status ReadSingleEntry(const std::string &table, const std::string &key,
                         const std::vector<std::string> *fields, std::vector<Field> &result);
  Status ScanSingleEntry(const std::string &table, const std::string &key, int len,
                         const std::vector<std::string> *fields,
                         std::vector<std::vector<Field>> &result);
  Status UpdateSingleEntry(const std::string &table, const std::string &key,
                           std::vector<Field> &values);
  Status InsertSingleEntry(const std::string &table, const std::string &key,
                           std::vector<Field> &values);
  Status DeleteSingleEntry(const std::string &table, const std::string &key);

  void SerializeRow(const std::vector<Field> &values, std::string *data);
  void DeserializeRow(std::vector<Field> *values, const char *data_ptr, size_t data_len);
  void DeserializeRowFilter(std::vector<Field> *values, const char *data_ptr, size_t data_len, const std::vector<std::string> &fields);

  Status (WTDB::*method_read_)(const std::string &, const std:: string &,
                                    const std::vector<std::string> *, std::vector<Field> &);
  Status (WTDB::*method_scan_)(const std::string &, const std::string &, int,
                                    const std::vector<std::string> *,
                                    std::vector<std::vector<Field>> &);
  Status (WTDB::*method_update_)(const std::string &, const std::string &,
                                      std::vector<Field> &);
  Status (WTDB::*method_insert_)(const std::string &, const std::string &,
                                      std::vector<Field> &);
  Status (WTDB::*method_delete_)(const std::string &, const std::string &);
  
  unsigned fieldcount_;

  static WT_CONNECTION *conn_;
  WT_SESSION *session_{nullptr};
  WT_CURSOR *cursor_{nullptr};
  WT_CURSOR *stat_cursor_{nullptr};
  WT_CURSOR *conn_cursor_{nullptr};
  /*
   * cursor is binded with session. And one session can be used only for one thread.
   * So stat_cursor cannot be used cross threads.
   */

  static int ref_cnt_;
  static std::mutex mu_;
  static std::atomic<uint64_t> total_user_write_;
  static std::atomic<uint64_t> last_user_read_;
  static std::atomic<uint64_t> total_user_read_;
  static std::atomic<uint64_t> total_cache_write_;
  static std::atomic<uint64_t> total_cache_read_;
  static std::atomic<uint64_t> total_fs_write_;
  static std::atomic<uint64_t> total_fs_read_;
  static std::atomic<uint64_t> total_leaf_splits_;
  static std::atomic<uint64_t> total_internal_splits;
};

DB *NewRocksdbDB();

} // namespace ycsbc

#endif
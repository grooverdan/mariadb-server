/*
  Copyright (c) 2026, MariaDB Foundation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#pragma once

#include "fiber_context.h"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"

struct THD;
struct Item;
template <class T> class List;

namespace myduck
{

/**
  Custom select_result_interceptor that writes result rows into
  a DuckDB DataChunk buffer and yields to the caller fiber when
  the chunk is full.

  Lifecycle:
    1. Created by the fiber function before mysql_execute_command().
    2. MariaDB executor calls send_data() once per row.
    3. When the DataChunk is full (STANDARD_VECTOR_SIZE rows),
       send_data() yields back to the DuckDB scan function.
    4. When the query finishes, executor calls send_eof();
       the finished flag is set.
    5. On error, abort_result_set() sets the error flag.
*/
class select_to_duckdb : public select_result_interceptor
{
public:
  select_to_duckdb(THD *thd_arg,
                   struct fiber_context *ctx,
                   duckdb::DataChunk *buffer,
                   const duckdb::vector<duckdb::LogicalType> *types);

  int send_data(List<Item> &items) override;
  bool send_eof() override;
  void abort_result_set() override;

  bool is_finished() const { return finished_; }
  bool has_error() const { return error_; }
  duckdb::idx_t row_count() const { return row_count_; }

private:
  struct fiber_context *ctx_;
  duckdb::DataChunk *buffer_;
  const duckdb::vector<duckdb::LogicalType> *types_;
  duckdb::idx_t row_count_;
  bool finished_;
  bool error_;
};

duckdb::Value item_to_duckdb_value(Item *item,
                                   const duckdb::LogicalType &type);

} /* namespace myduck */

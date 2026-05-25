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

#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_class.h"
#include "item.h"

#undef UNKNOWN

#include "fiber_scan.h"

namespace myduck
{

/* ----------------------------------------------------------------
   Item → duckdb::Value conversion
   ---------------------------------------------------------------- */

duckdb::Value item_to_duckdb_value(Item *item, const duckdb::LogicalType &type)
{
  if (item->is_null())
    return duckdb::Value();

  switch (type.id())
  {
  case duckdb::LogicalTypeId::TINYINT:
    return duckdb::Value::TINYINT(static_cast<int8_t>(item->val_int()));
  case duckdb::LogicalTypeId::SMALLINT:
    return duckdb::Value::SMALLINT(static_cast<int16_t>(item->val_int()));
  case duckdb::LogicalTypeId::INTEGER:
    return duckdb::Value::INTEGER(static_cast<int32_t>(item->val_int()));
  case duckdb::LogicalTypeId::BIGINT:
    return duckdb::Value::BIGINT(item->val_int());
  case duckdb::LogicalTypeId::UTINYINT:
    return duckdb::Value::UTINYINT(static_cast<uint8_t>(item->val_uint()));
  case duckdb::LogicalTypeId::USMALLINT:
    return duckdb::Value::USMALLINT(static_cast<uint16_t>(item->val_uint()));
  case duckdb::LogicalTypeId::UINTEGER:
    return duckdb::Value::UINTEGER(static_cast<uint32_t>(item->val_uint()));
  case duckdb::LogicalTypeId::UBIGINT:
    return duckdb::Value::UBIGINT(item->val_uint());
  case duckdb::LogicalTypeId::FLOAT:
    return duckdb::Value::FLOAT(static_cast<float>(item->val_real()));
  case duckdb::LogicalTypeId::DOUBLE:
    return duckdb::Value::DOUBLE(item->val_real());
  case duckdb::LogicalTypeId::BLOB: {
    String buf;
    String *s= item->val_str(&buf);
    if (!s)
      return duckdb::Value();
    return duckdb::Value::BLOB(std::string(s->ptr(), s->length()));
  }
  default: {
    String buf;
    String *s= item->val_str(&buf);
    if (!s)
      return duckdb::Value();
    return duckdb::Value(std::string(s->ptr(), s->length()));
  }
  }
}

/* ----------------------------------------------------------------
   select_to_duckdb implementation
   ---------------------------------------------------------------- */

select_to_duckdb::select_to_duckdb(THD *thd_arg,
                                   struct fiber_context *ctx,
                                   duckdb::DataChunk *buffer,
                                   const duckdb::vector<duckdb::LogicalType> *types)
  : select_result_interceptor(thd_arg),
    ctx_(ctx),
    buffer_(buffer),
    types_(types),
    row_count_(0),
    finished_(false),
    error_(false)
{}

int select_to_duckdb::send_data(List<Item> &items)
{
  List_iterator_fast<Item> it(items);
  Item *item;
  duckdb::idx_t col= 0;

  while ((item= it++))
  {
    if (col < types_->size())
    {
      duckdb::Value val= item_to_duckdb_value(item, (*types_)[col]);
      buffer_->data[col].SetValue(row_count_, val);
    }
    col++;
  }
  row_count_++;

  if (row_count_ >= STANDARD_VECTOR_SIZE)
  {
    buffer_->SetCardinality(row_count_);
    row_count_= 0;
    fiber_context_yield(ctx_);
  }

  return thd->killed ? -1 : 0;
}

bool select_to_duckdb::send_eof()
{
  if (row_count_ > 0)
  {
    buffer_->SetCardinality(row_count_);
    row_count_= 0;
  }
  finished_= true;
  return false;
}

void select_to_duckdb::abort_result_set()
{
  error_= true;
  finished_= true;
}

} /* namespace myduck */

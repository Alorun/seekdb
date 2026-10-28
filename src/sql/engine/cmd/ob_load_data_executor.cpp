/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX  SQL_ENG

#include "sql/engine/cmd/ob_load_data_executor.h"

#include "sql/engine/cmd/ob_load_data_impl.h"
#include "sql/ob_sql_trans_control.h"
#include "sql/engine/dml/ai_column_dml.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/rc/ob_server_runtime.h"

namespace oceanbase
{
namespace sql
{

int ObLoadDataExecutor::execute(ObExecContext &ctx, ObLoadDataStmt &stmt)
{
  int ret = OB_SUCCESS;
  bool command_started = false;
  ObLoadDataBase *load_impl = NULL;
  if (!stmt.get_load_arguments().is_csv_format_) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_ISNULL(load_impl = OB_NEWx(ObLoadDataSPImpl, (&ctx.get_allocator())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  if (OB_SUCC(ret) && stmt.ai_transactional_load_) {
    share::schema::ObSchemaGetterGuard guard;
    const share::schema::ObTableSchema *table = nullptr;
    common::ObSEArray<common::ObObjectID, 1> partitions;
    if (OB_FAIL(ObSqlTransControl::begin_command_dml(ctx))) {
    } else {
      command_started = true;
      if (OB_FAIL(ObSqlTransControl::lock_table(ctx, stmt.get_load_arguments().table_id_, partitions,
                 transaction::tablelock::ROW_EXCLUSIVE, -1))) {
      } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
      } else if (OB_FAIL(guard.get_table_schema(stmt.get_load_arguments().table_id_, table))) {
      } else if (table == nullptr || table->get_schema_version() != stmt.ai_schema_version_) {
        ret = OB_SCHEMA_EAGAIN;
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(load_impl->execute(ctx, stmt))) {
    } else {
    }
  }
  if (load_impl != nullptr) { load_impl->~ObLoadDataBase(); }
  if (command_started) {
    ctx.set_errcode(ret);
    const int end_ret = ObSqlTransControl::end_command_dml(ctx, ret != OB_SUCCESS);
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  ai_column::release_load_schema_barrier(ctx);
  return ret;
}

} // sql
} // oceanbase

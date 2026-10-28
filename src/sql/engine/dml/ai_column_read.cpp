// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/ob_physical_plan_ctx.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_del_upd_stmt.h"
#include "sql/resolver/dml/ob_stmt_expr_visitor.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/rc/ob_server_runtime.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "data_plane/access/ob_table_scan_param.h"
#include "data_plane/access/ob_table_param.h"
#include "data_plane/transaction/ob_tx_desc_access.h"
#include "storage/access/ob_table_scan_iterator.h"

namespace oceanbase { namespace sql { namespace ai_column {
using namespace common;
using namespace share::schema;
namespace {
class ReadRefCollector {
public:
  ReadRefCollector(ObSchemaGetterGuard &guard, ObPhysicalPlan &plan) : guard_(guard), plan_(plan) {}
  int statement(ObDMLStmt &stmt, bool projection, int64_t depth)
  {
    int ret = ancestors_.push_back(&stmt);
    if (OB_SUCC(ret)) {
      ret = statement_impl(stmt, projection, depth);
      ancestors_.pop_back();
    }
    return ret;
  }
  int statement_impl(ObDMLStmt &stmt, bool projection, int64_t depth)
  {
    int ret = OB_SUCCESS;
    if (depth > 128) { return OB_SIZE_OVERFLOW; }
    ObStmtExprGetter visitor;
    visitor.set_relation_scope();
    visitor.remove_scope(SCOPE_DMLINFOS);
    visitor.remove_scope(SCOPE_DML_COLUMN);
    visitor.remove_scope(SCOPE_DML_VALUE);
    visitor.remove_scope(SCOPE_DML_CONSTRAINT);
    visitor.remove_scope(SCOPE_INSERT_VECTOR);
    if (!projection) { visitor.remove_scope(SCOPE_SELECT); }
    ObSEArray<ObRawExpr *, 16> exprs;
    if (OB_FAIL(stmt.get_relation_exprs(exprs, visitor))) { return ret; }
    if (ObDelUpdStmt *write = dynamic_cast<ObDelUpdStmt *>(&stmt)) {
      ObSEArray<ObDmlTableInfo *, 2> infos;
      if (OB_FAIL(write->get_dml_table_infos(infos))) { return ret; }
      for (int64_t i = 0; OB_SUCC(ret) && i < infos.count(); ++i) {
        ObIArray<ObAssignment> *assigns = nullptr;
        if (ObUpdateTableInfo *update = dynamic_cast<ObUpdateTableInfo *>(infos.at(i))) { assigns = &update->assignments_; }
        else if (ObInsertTableInfo *insert = dynamic_cast<ObInsertTableInfo *>(infos.at(i))) {
          assigns = &insert->assignments_;
          for (int64_t v = 0; OB_SUCC(ret) && v < insert->column_conv_exprs_.count(); ++v) {
            ret = exprs.push_back(insert->column_conv_exprs_.at(v));
          }
        }
        if (assigns != nullptr) {
          for (int64_t j = 0; OB_SUCC(ret) && j < assigns->count(); ++j) {
            if (!assigns->at(j).is_implicit_) { ret = exprs.push_back(assigns->at(j).expr_); }
          }
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < exprs.count(); ++i) { ret = expression(stmt, exprs.at(i), depth + 1); }
    // View/derived-table filters and joins execute even when an unused output
    // column is pruned. Their unreferenced projection expressions do not.
    for (int64_t i = 0; OB_SUCC(ret) && i < stmt.get_table_items().count(); ++i) {
      TableItem *table = stmt.get_table_items().at(i);
      if (table != nullptr && table->is_generated_table() && table->ref_query_ != nullptr) {
        ret = statement(*table->ref_query_, false, depth + 1);
      }
    }
    return ret;
  }
private:
  int expression(ObDMLStmt &stmt, ObRawExpr *expr, int64_t depth)
  {
    int ret = OB_SUCCESS;
    if (depth > 128 || expr == nullptr) { return depth > 128 ? OB_SIZE_OVERFLOW : OB_ERR_UNEXPECTED; }
    if (expr->is_column_ref_expr()) {
      const ObColumnRefRawExpr &column = *static_cast<ObColumnRefRawExpr *>(expr);
      const TableItem *item = stmt.get_table_item_by_id(column.get_table_id());
      if (item != nullptr && item->is_basic_table()) {
        const ObTableSchema *table = nullptr;
        if (OB_FAIL(guard_.get_table_schema(item->ref_id_, table))) {
        } else if (table == nullptr) { ret = OB_TABLE_NOT_EXIST; }
        else {
          const ObColumnSchemaV2 *schema = table->get_column_schema(column.get_column_id());
          if (schema != nullptr && schema->is_ai_column()) {
            bool found = false;
            for (int64_t i = 0; i < plan_.ai_read_refs_.count(); i += 2) {
              found |= plan_.ai_read_refs_.at(i) == table->get_table_id()
                    && plan_.ai_read_refs_.at(i + 1) == schema->get_column_id();
            }
            if (!found) {
              ObSchemaObjVersion dependency(table->get_table_id(), table->get_schema_version(), DEPENDENCY_TABLE);
              if (OB_FAIL(stmt.add_global_dependency_table(dependency))) {
              } else if (OB_FAIL(plan_.ai_read_refs_.push_back(table->get_table_id()))) {
              } else { ret = plan_.ai_read_refs_.push_back(schema->get_column_id()); }
            }
          }
        }
      } else if (item != nullptr && item->is_generated_table() && item->ref_query_ != nullptr) {
        const int64_t idx = column.get_column_id() - OB_APP_MIN_COLUMN_ID;
        if (idx < 0 || idx >= item->ref_query_->get_select_item_size()) { ret = OB_ERR_UNEXPECTED; }
        else { ret = expression(*item->ref_query_, item->ref_query_->get_select_item(idx).expr_, depth + 1); }
      } else {
        // Correlated references belong to the parent namespace.
        for (int64_t i = ancestors_.count() - 1; OB_SUCC(ret) && i >= 0; --i) {
          ObDMLStmt *parent = ancestors_.at(i);
          if (parent != &stmt && parent->get_table_item_by_id(column.get_table_id()) != nullptr) {
            ret = expression(*parent, expr, depth + 1);
            break;
          }
        }
      }
    } else if (expr->is_query_ref_expr()) {
      ObSelectStmt *query = static_cast<ObQueryRefRawExpr *>(expr)->get_ref_stmt();
      if (query == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else { ret = statement(*query, true, depth + 1); }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
        ret = expression(stmt, expr->get_param_expr(i), depth + 1);
      }
    }
    return ret;
  }
  ObSchemaGetterGuard &guard_;
  ObPhysicalPlan &plan_;
  ObSEArray<ObDMLStmt *, 8> ancestors_;
};
} // anonymous namespace

int collect_read_refs(ObDMLStmt &stmt, ObSchemaGetterGuard &guard, ObPhysicalPlan &plan)
{
  ReadRefCollector collector(guard, plan);
  return collector.statement(stmt, true, 0);
}

int strict_read_check(ObExecContext &ctx)
{
  int ret = OB_SUCCESS;
  const ObPhysicalPlan *plan = ctx.get_physical_plan_ctx() == nullptr ? nullptr
      : ctx.get_physical_plan_ctx()->get_phy_plan();
  if (plan == nullptr || plan->ai_read_refs_.empty()) { return ret; }
  ObObj enabled;
  ObSQLSessionInfo *session = ctx.get_my_session();
  if (session == nullptr) { return OB_ERR_UNEXPECTED; }
  if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, enabled))) { return ret; }
  if (enabled.get_int() == 0) { return ret; }
  ObITabletScan *service = share::server_service<ObITabletScan>();
  if (service == nullptr || ctx.get_sql_ctx() == nullptr || ctx.get_sql_ctx()->schema_guard_ == nullptr) { return OB_NOT_INIT; }
  // This is the same native tablet-scan interface used by DAS. No nested SQL,
  // transaction creation or snapshot acquisition occurs in the checker.
  const auto &snapshot = ctx.get_das_ctx().get_snapshot();
  if (!snapshot.valid_) { return OB_ERR_UNEXPECTED; }
  for (int64_t i = 0; OB_SUCC(ret) && i < plan->ai_read_refs_.count(); i += 2) {
    const uint64_t tid = plan->ai_read_refs_.at(i), cid = plan->ai_read_refs_.at(i + 1);
    const ObTableSchema *table = nullptr;
    ObArenaAllocator allocator("AiStrictRead");
    ObSEArray<ObTabletID, 4> tablets;
    if (OB_FAIL(ctx.get_sql_ctx()->schema_guard_->get_table_schema(tid, table))) {
    } else if (table == nullptr) { ret = OB_TABLE_NOT_EXIST; }
    else if (OB_FAIL(table->get_tablet_ids(tablets))) {
    }
    const ObColumnSchemaV2 *column = table == nullptr ? nullptr : table->get_column_schema(cid);
    if (OB_SUCC(ret) && (column == nullptr || !column->is_ai_column())) { ret = OB_SCHEMA_EAGAIN; }
    for (int64_t t = 0; OB_SUCC(ret) && t < tablets.count(); ++t) {
      storage::ObTableScanParam param;
      ObTableParam table_param(allocator);
      ObStoragePushdownFlag pushdown;
      ObNewRange range;
      ObNewRowIterator *iterator = nullptr;
      param.tablet_id_ = tablets.at(t);
      param.index_id_ = tid;
      param.schema_version_ = table->get_schema_version();
      param.runtime_schema_version_ = table->get_schema_version();
      param.timeout_ = ctx.get_physical_plan_ctx()->get_timeout_timestamp();
      param.allocator_ = &allocator; param.scan_allocator_ = &allocator;
      param.trans_desc_ = session->get_tx_desc();
      param.tx_id_ = data_plane::tx_desc_id(session->get_tx_desc());
      param.tx_seq_base_ = data_plane::tx_desc_seq_base(session->get_tx_desc());
      param.sql_mode_ = session->get_sql_mode();
      param.limit_param_.limit_ = -1;
      param.limit_param_.offset_ = 0;
      param.reserved_cell_count_ = 1;
      range.table_id_ = tid; range.set_whole_range();
      if (OB_FAIL(param.snapshot_.assign(snapshot))) {
      } else if (OB_FAIL(param.column_ids_.push_back(column->get_ai_state_column_id()))) {
      } else if (OB_FAIL(param.key_ranges_.push_back(range))) {
      } else if (OB_FAIL(table_param.convert(*table, param.column_ids_, pushdown))) {
      } else {
        param.table_param_ = &table_param;
        if (OB_FAIL(service->table_scan(param, iterator))) {
        } else if (iterator == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else {
          blocksstable::ObDatumRow *row = nullptr;
          auto *scan = static_cast<storage::ObTableScanIterator *>(iterator);
          int next = OB_SUCCESS;
          while (OB_SUCC(ret) && OB_SUCCESS == (next = scan->get_next_row(row))) {
            if (row == nullptr || row->get_column_count() != 1) { ret = OB_ERR_UNEXPECTED; }
            else {
              const auto &state = row->storage_datums_[0];
              if (state.is_null() || state.get_int() != 1) {
                ret = OB_AI_COLUMN_NOT_READY;
                OB_LOGGER.log_user_message(ObLogger::USER_ERROR, OB_AI_COLUMN_NOT_READY,
                    "AI column has unfinished tasks for table_id=%lu, column_id=%lu", tid, cid);
              }
            }
          }
          if (OB_SUCC(ret) && next != OB_ITER_END) { ret = next; }
        }
      }
      if (iterator != nullptr) {
        const int close_ret = service->revert_scan_iter(iterator);
        if (OB_SUCC(ret)) { ret = close_ret; }
      }
    }
  }
  return ret;
}
} } } // namespace oceanbase::sql::ai_column

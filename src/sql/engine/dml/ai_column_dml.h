// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lib/container/ob_se_array.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/ai_column/ai_column_types.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"

namespace oceanbase { namespace share { namespace schema { class ObTableSchema; class ObSchemaGetterGuard; } } namespace sql {
class ObExecContext;
class ObEvalCtx;
class ObTableModifyOp;
struct ObDMLBaseCtDef;
class ObDMLResolver;
class ObDMLStmt;
class ObPhysicalPlan;
namespace ai_column {
enum class WriteKind { USER, RETRY, PUBLISH, BACKFILL };
struct WriteCapability {
  WriteKind kind_ = WriteKind::USER;
  uint64_t table_id_ = OB_INVALID_ID;
  const common::ObIArray<uint64_t> *columns_ = nullptr;
  common::ObIAllocator *progress_allocator_ = nullptr;
  common::ObString *last_rowkey_ = nullptr;
  bool allows(uint64_t table_id, uint64_t column_id) const;
};
struct PreparedCall {
  OB_UNIS_VERSION(1);
public:
  common::ObString model_;
  common::ObString input_;
  common::ObString config_;
  common::ObString system_;
  int64_t operation_ = 0;
  int64_t dimension_ = 0;
  bool input_null_ = false;
};
int append_hex(common::ObSqlString &sql, const common::ObString &bytes);
int append_identifier(common::ObSqlString &sql, const common::ObString &name);
int append_literal(common::ObSqlString &sql, const common::ObObj &value,
                   common::ObIAllocator &allocator, ObSQLSessionInfo *session);
int decode_objects(const common::ObString &bytes, common::ObIArray<common::ObObj> &out);
int make_rowkey_predicate(const share::schema::ObTableSchema &schema, const common::ObString &key,
                         common::ObSqlString &sql, common::ObIAllocator &allocator, ObSQLSessionInfo *session);
int make_rowkey_after(const share::schema::ObTableSchema &schema, const common::ObString &key,
                      common::ObSqlString &sql, common::ObIAllocator &allocator, ObSQLSessionInfo *session);
// Extends the established FK nested-session contract without borrowing its flags.
class TxnSqlGuard {
public:
  explicit TxnSqlGuard(ObExecContext &ctx);
  ~TxnSqlGuard();
  int open(const WriteCapability *capability = nullptr);
  int read(const common::ObString &sql, common::ObISQLClient::ReadResult &result);
  int write(const common::ObString &sql, int64_t &affected);
  int close();
private:
  ObExecContext &ctx_;
  common::sqlclient::ObISQLConnectionGuard guard_;
  ObIInnerSQLConnection *connection_;
  ObSQLSessionInfo::StmtSavedValue saved_session_;
  ObIInnerSQLConnection::SavedValue saved_connection_;
  transaction::ObTxDesc *tx_;
  const WriteCapability *previous_next_;
  const WriteCapability *capability_;
  bool opened_;
  bool old_internal_;
  bool old_iter_uncommitted_;
  bool old_retry_inited_;
};

class StatementContext {
public:
  explicit StatementContext(ObExecContext &ctx);
  ~StatementContext();
  int init();
  int collect(const common::ObIArray<common::ObObj> &fields);
  int collect_key(const common::ObIArray<common::ObObj> &fields);
  int finish();
private:
  ObExecContext &ctx_;
  ObChunkDatumStore rows_;
  ObChunkDatumStore keys_;
  bool inited_;
};

int schema_fence(ObExecContext &ctx);
int hold_load_schema_barrier(ObExecContext &ctx);
void release_load_schema_barrier(ObExecContext &ctx);
bool try_hold_ddl_schema_barrier();
void release_ddl_schema_barrier();
int collect_read_refs(ObDMLStmt &stmt, share::schema::ObSchemaGetterGuard &guard, ObPhysicalPlan &plan);
int strict_read_check(ObExecContext &ctx);
int prepare(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op);
int collect(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op, bool deleted = false);
int materialize(const ObDMLBaseCtDef &ctdef, ObExecContext &ctx, ObEvalCtx &eval);
int finish(ObExecContext &ctx, bool rollback);
bool user_write_allowed(ObSQLSessionInfo *session, uint64_t table_id, uint64_t column_id);
} } } // namespace oceanbase::sql::ai_column

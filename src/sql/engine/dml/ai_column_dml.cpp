// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/ob_sql_utils.h"
#include "sql/ob_sql_trans_control.h"
#include "sql/session/ob_local_session_var.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "query/engine/expr/ob_expr_lob_utils.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_ai_model_mgr.h"
#include "data_plane/tablelock/ob_table_lock_mode.h"
#include "data_plane/tmp_file/ob_tmp_file.h"
#include "lib/lock/ob_spin_rwlock.h"

namespace oceanbase { namespace sql { namespace ai_column {
using namespace common;
using namespace share;
using namespace share::schema;
OB_SERIALIZE_MEMBER(PreparedCall, model_, input_, config_, dimension_, input_null_, system_, operation_);

bool WriteCapability::allows(uint64_t table_id, uint64_t column_id) const
{
  bool found = false;
  if (kind_ != WriteKind::USER && table_id_ == table_id && columns_ != nullptr) {
    for (int64_t i = 0; i < columns_->count(); ++i) { found = found || columns_->at(i) == column_id; }
  }
  return found;
}
bool user_write_allowed(ObSQLSessionInfo *session, uint64_t table_id, uint64_t column_id)
{
  ObExecContext *ctx = session == nullptr ? nullptr : session->get_cur_exec_ctx();
  return ctx != nullptr && ctx->ai_column_capability() != nullptr
      && ctx->ai_column_capability()->allows(table_id, column_id);
}
TxnSqlGuard::TxnSqlGuard(ObExecContext &ctx)
  : ctx_(ctx), connection_(nullptr), tx_(nullptr), previous_next_(nullptr), capability_(nullptr),
    opened_(false), old_internal_(false), old_iter_uncommitted_(false), old_retry_inited_(false) {}
TxnSqlGuard::~TxnSqlGuard() { const int ignored = close(); UNUSED(ignored); }
int TxnSqlGuard::open(const WriteCapability *capability)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  if (opened_ || session == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, guard_))) {
  } else if (nullptr == (connection_ = as_inner_sql_connection(guard_.get_ptr()))) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(connection_->begin_nested_session(saved_session_, saved_connection_, false))) {
  } else {
    tx_ = session->get_tx_desc();
    old_retry_inited_ = session->get_retry_info().is_inited();
    old_internal_ = ctx_.ai_column_internal_sql();
    old_iter_uncommitted_ = ctx_.get_das_ctx().iter_uncommitted_row_;
    previous_next_ = ctx_.ai_column_next_capability();
    capability_ = capability;
    ctx_.set_ai_column_internal_sql(true);
    ctx_.get_das_ctx().iter_uncommitted_row_ = true;
    opened_ = true;
  }
  return ret;
}
int TxnSqlGuard::read(const ObString &sql, ObISQLClient::ReadResult &result)
{
  int ret = OB_SUCCESS;
  if (!opened_) { ret = OB_NOT_INIT; }
  else {
    ctx_.ai_column_next_capability() = capability_;
    ret = connection_->execute_read(sql, result);
    ctx_.ai_column_next_capability() = nullptr;
    if (tx_ != ctx_.get_my_session()->get_tx_desc()) { ret = OB_ERR_UNEXPECTED; }
  }
  return ret;
}
int TxnSqlGuard::write(const ObString &sql, int64_t &affected)
{
  int ret = OB_SUCCESS;
  if (!opened_) { ret = OB_NOT_INIT; }
  else {
    ctx_.ai_column_next_capability() = capability_;
    ret = connection_->execute_write(sql, affected);
    ctx_.ai_column_next_capability() = nullptr;
    if (tx_ != ctx_.get_my_session()->get_tx_desc()) { ret = OB_ERR_UNEXPECTED; }
  }
  return ret;
}
int TxnSqlGuard::close()
{
  int ret = OB_SUCCESS;
  if (opened_) {
    // finish() can run from ObInnerSQLResult::close(), after the owning inner
    // query has reset its retry context. External-session nested SQL initializes
    // that same context but does not reset it. Restore the uninitialized state
    // so the next statement on the worker transaction is allowed to execute.
    // A business query's active retry context must remain owned by that query.
    if (!old_retry_inited_) { ctx_.get_my_session()->get_retry_info_for_update().reset(); }
    ctx_.set_ai_column_internal_sql(old_internal_);
    ctx_.get_das_ctx().iter_uncommitted_row_ = old_iter_uncommitted_;
    ctx_.ai_column_next_capability() = previous_next_;
    ret = connection_->end_nested_session(saved_session_, saved_connection_);
    opened_ = false;
  }
  return ret;
}
int append_hex(ObSqlString &sql, const ObString &bytes)
{
  int ret = sql.append("X'");
  static const char digits[] = "0123456789abcdef";
  for (int64_t i = 0; OB_SUCC(ret) && i < bytes.length(); ++i) {
    const unsigned char c = bytes.ptr()[i];
    char pair[] = {digits[c >> 4], digits[c & 15]};
    ret = sql.append(pair, 2);
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}
int append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { ret = sql.append("``"); }
    else { ret = sql.append(name.ptr() + i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}
int append_literal(ObSqlString &sql, const ObObj &value, ObIAllocator &allocator, ObSQLSessionInfo *session)
{
  int ret = OB_SUCCESS;
  const int64_t size = 4 * value.get_serialize_size() + 4096;
  char *buffer = static_cast<char *>(allocator.alloc(size));
  int64_t pos = 0;
  ObObjPrintParams params = session == nullptr ? ObObjPrintParams() : session->create_obj_print_params();
  params.character_hex_safe_represent_ = true;
  params.binary_string_print_hex_ = true;
  if (buffer == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else if (OB_FAIL(value.print_sql_literal(buffer, size, pos, params))) {
  } else { ret = sql.append(buffer, pos); }
  return ret;
}
namespace {
// The SQL command LOAD DATA may implicitly commit an ordinary table's preceding
// transaction. This process-local barrier bridges that commit, while normal AI
// DML continues to use the transaction-owned table intents. AI metadata DDL
// retains the writer through its DDL transaction object's lifetime.
SpinRWLock ai_metadata_barrier;
enum Field { TABLE, COLUMN, REQUEST, SPEC, KEY, PREDICATE, INCARNATION, GENERATION,
             DEFINITION, RETRIES, DATABASE, CALL, TARGET, OLD_REQUEST, STATE, ERROR,
             MODEL_ID, MODEL_NAME, OPERATION, FIELD_COUNT };
int encode_objects(const ObIArray<ObObj> &values, ObIAllocator &allocator, ObString &bytes)
{
  int ret = OB_SUCCESS;
  int64_t size = serialization::encoded_length_i64(values.count());
  for (int64_t i = 0; i < values.count(); ++i) { size += values.at(i).get_serialize_size(); }
  char *data = static_cast<char *>(allocator.alloc(size));
  int64_t pos = 0;
  if (data == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else if (OB_FAIL(serialization::encode_i64(data, size, pos, values.count()))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < values.count(); ++i) { ret = values.at(i).serialize(data, size, pos); }
  if (OB_SUCC(ret)) { bytes.assign_ptr(data, pos); }
  return ret;
}
int uuid(ObIAllocator &allocator, ObString &text, char *binary)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(allocator.alloc(UuidCommon::LENGTH_UUID));
  if (buf == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else if (OB_FAIL(ObExprUuid::gen_server_uuid(buf, UuidCommon::LENGTH_UUID))) {
  } else {
    text.assign_ptr(buf, UuidCommon::LENGTH_UUID);
    int n = 0;
    int high = -1;
    for (int i = 0; i < text.length(); ++i) {
      const char c = text.ptr()[i];
      if (c == '-') { continue; }
      const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (v < 0) { ret = OB_INVALID_DATA; break; }
      if (high < 0) { high = v; }
      else if (n >= 16) { ret = OB_INVALID_DATA; break; }
      else { binary[n++] = static_cast<char>(high * 16 + v); high = -1; }
    }
    if (OB_SUCC(ret) && (n != 16 || high >= 0)) { ret = OB_INVALID_DATA; }
  }
  return ret;
}
int row_objects(const ExprFixedArray &row, ObEvalCtx &eval, ObIArray<ObObj> &out)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < row.count(); ++i) {
    ObDatum *datum = nullptr;
    ObObj value;
    if (OB_FAIL(row.at(i)->eval(eval, datum))) {
    } else if (OB_FAIL(datum->to_obj(value, row.at(i)->obj_meta_, row.at(i)->obj_datum_map_))) {
    } else { ret = out.push_back(value); }
  }
  return ret;
}
int64_t position(const ObDMLBaseCtDef &ctdef, uint64_t column)
{
  int64_t pos = -1;
  for (int64_t i = 0; pos < 0 && i < ctdef.column_ids_.count(); ++i) {
    if (ctdef.column_ids_.at(i) == column) { pos = i; }
  }
  return pos;
}
int get_schema(ObExecContext &ctx, uint64_t tid, const ObTableSchema *&schema)
{
  int ret = OB_SUCCESS;
  if (ctx.get_sql_ctx() == nullptr || ctx.get_sql_ctx()->schema_guard_ == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(ctx.get_sql_ctx()->schema_guard_->get_table_schema(tid, schema))) {
  } else if (schema == nullptr) { ret = OB_TABLE_NOT_EXIST; }
  return ret;
}
int table_name(ObExecContext &ctx, const ObTableSchema &schema, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *db = nullptr;
  if (OB_FAIL(ctx.get_sql_ctx()->schema_guard_->get_database_schema(schema.get_database_id(), db))) {
  } else if (db == nullptr) { ret = OB_ERR_BAD_DATABASE; }
  else if (OB_FAIL(append_identifier(sql, db->get_database_name_str()))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else { ret = append_identifier(sql, schema.get_table_name_str()); }
  return ret;
}
int make_key(const ObDMLBaseCtDef &ctdef, const ObTableSchema &schema, const ObIArray<ObObj> &row,
             ObIAllocator &allocator, ObString &key)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObObj, 8> fields;
  for (int64_t k = 0; OB_SUCC(ret) && k < schema.get_rowkey_column_num(); ++k) {
    const uint64_t cid = schema.get_rowkey_info().get_column(k)->column_id_;
    const int64_t idx = position(ctdef, cid);
    ObObj id; id.set_uint64(cid);
    if (idx < 0 || idx >= row.count()) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(fields.push_back(id))) {
    } else if (OB_FAIL(fields.push_back(row.at(idx)))) {
    }
  }
  if (OB_SUCC(ret)) { ret = encode_objects(fields, allocator, key); }
  return ret;
}
int differs(ObExecContext &ctx, ObIAllocator &allocator, const ObObj &a, const ObObj &b, bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  if (a.is_null() || b.is_null()) { changed = a.is_null() != b.is_null(); }
  else if (a.is_string_type() || a.is_lob() || a.is_json()) {
    ObString left, right;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx, &allocator, a, left))) {
    } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx, &allocator, b, right))) {
    } else { changed = left != right; }
  } else {
    ObSEArray<ObObj, 1> one, two;
    ObString left, right;
    if (OB_FAIL(one.push_back(a))) {
    } else if (OB_FAIL(two.push_back(b))) {
    } else if (OB_FAIL(encode_objects(one, allocator, left))) {
    } else if (OB_FAIL(encode_objects(two, allocator, right))) {
    } else { changed = left != right; }
  }
  return ret;
}
int tree_from_obj(ObExecContext &ctx, ObIAllocator &allocator, const ObObj &obj, ObIJsonBase *&tree)
{
  int ret = OB_SUCCESS;
  ObString bytes;
  if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx, &allocator, obj, bytes))) {
  } else { ret = ObJsonBaseFactory::get_json_base(&allocator, bytes,
                  obj.is_json() ? ObJsonInType::JSON_BIN : ObJsonInType::JSON_TREE,
                  ObJsonInType::JSON_TREE, tree); }
  return ret;
}
int prepare_call(ObExecContext &ctx, const ObDMLBaseCtDef &ctdef, const ObTableSchema &table,
                 const ObColumnSchemaV2 &column, const ObIArray<ObObj> &row,
                 ObIAllocator &allocator, PreparedCall &call, int64_t &model_id)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx.get_my_session();
  ObLocalSessionVar saved(&allocator);
  ObRawExprFactory factory(allocator);
  ObRawExpr *root = nullptr;
  ObSEArray<ObQualifiedName, 4> names;
  ObObj model_value, input, option;
  ObString normalized = column.get_ai_normalized_expr();
  ParamStore params;
  if (OB_FAIL(ObLocalSessionVarHelper::load_session_vars(session, saved))) { return ret; }
  if (OB_FAIL(ObLocalSessionVarHelper::update_session_vars_with_local(column.get_local_session_var(), *session))) {
  } else if (OB_FAIL(ObSQLUtils::convert_sql_text_from_schema_for_resolve(allocator, session->get_dtc_params(), normalized))) {
  } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(normalized,
            factory, *session, root, names, &table, nullptr, nullptr, ObResolverUtils::DISABLE_CHECK))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    const ObColumnSchemaV2 *dependency = table.get_column_schema(names.at(i).col_name_);
    const int64_t idx = dependency == nullptr ? -1 : position(ctdef, dependency->get_column_id());
    ObConstRawExpr *value = nullptr;
    if (idx < 0 || idx >= row.count()) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(ObRawExprUtils::build_const_obj_expr(factory, row.at(idx), value))) {
    } else { ret = ObRawExprUtils::replace_ref_column(root, names.at(i).ref_expr_, value); }
  }
  if (OB_SUCC(ret) && (root == nullptr || root->get_param_count() != 3)) { ret = OB_INVALID_ARGUMENT; }
  if (OB_SUCC(ret)) { call.operation_ = root->get_expr_type() == T_FUN_SYS_AI_EMBED ? 1 : 0; }
  if (OB_SUCC(ret) && OB_FAIL(root->formalize(session))) {
  }
  // Only ordinary parameter subtrees enter expression evaluation. The AI root
  // is deliberately never handed to code generation or eval.
  // Formalization may wrap even the constant model name in a charset cast.
  // Evaluate that subtree rather than assuming it remains a ConstRawExpr.
  if (OB_SUCC(ret) && OB_FAIL(ObSQLUtils::calc_const_expr(ctx, root->get_param_expr(0), model_value, allocator, params))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(ObSQLUtils::calc_const_expr(ctx, root->get_param_expr(1), input, allocator, params))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(ObSQLUtils::calc_const_expr(ctx, root->get_param_expr(2), option, allocator, params))) {
  }
  if (OB_SUCC(ret)) {
    if (model_value.is_null() || !model_value.is_string_type()) { ret = OB_INVALID_ARGUMENT; }
    else { call.model_ = model_value.get_string(); }
    const ObAiModelSchema *model = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ctx.get_sql_ctx()->schema_guard_->get_ai_model_schema(call.model_, model))) {
    } else if (model == nullptr) { ret = OB_INVALID_ARGUMENT; }
    else { model_id = model->get_ai_model_id(); }
    call.input_null_ = input.is_null();
    if (OB_SUCC(ret) && !input.is_null()) {
      if (input.is_json()) {
        ObIJsonBase *tree = nullptr;
        if (OB_FAIL(tree_from_obj(ctx, allocator, input, tree))) {
        } else if (tree->json_type() != ObJsonNodeType::J_OBJECT ||
                   !ObAIFuncPromptObjectUtils::is_valid_prompt_object(static_cast<ObJsonObject *>(tree))) {
          ret = OB_INVALID_ARGUMENT;
        } else { ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator,
                               static_cast<ObJsonObject *>(tree), call.input_); }
      } else { ret = ObTextStringHelper::read_real_string_data(ctx, &allocator, input, call.input_); }
    }
    if (OB_SUCC(ret) && !call.input_null_ && call.input_.empty()) { ret = OB_INVALID_ARGUMENT; }
    if (OB_SUCC(ret) && root->get_expr_type() == T_FUN_SYS_AI_EMBED) {
      call.dimension_ = option.get_int();
    } else if (OB_SUCC(ret) && !option.is_null()) {
      ObIJsonBase *config = nullptr;
      if (OB_FAIL(tree_from_obj(ctx, allocator, option, config))) {
      } else if (config->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_ARGUMENT; }
      else {
        ObJsonObject *object = static_cast<ObJsonObject *>(config);
        if (object->get_value("__seekdb_ai_column_result_prompt") != nullptr) {
          ObJsonNode *hint = object->get_value("__seekdb_ai_column_result_prompt");
          if (hint->json_type() != ObJsonNodeType::J_STRING) { ret = OB_INVALID_ARGUMENT; }
          else {
            ret = ob_write_string(allocator, static_cast<ObJsonString *>(hint)->get_str(), call.system_);
            if (OB_SUCC(ret)) { ret = object->remove(ObString("__seekdb_ai_column_result_prompt")); }
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(ObAIFuncUtils::validate_complete_config(allocator, object))) {
        } else if (OB_SUCC(ret)) { ret = ObAIFuncJsonUtils::print_json_to_str(allocator, object, call.config_); }
      }
    }
  }
  if (OB_SUCC(ret) && call.operation_ == 0 && call.system_.empty()) {
    // SQL NULL config is the ordinary AI_COMPLETE default. It still needs an
    // independent type hint; neither the user's input nor config is rewritten.
    const char *hint = column.is_json() ? "Return exactly one valid JSON value. Do not use markdown fences."
        : ob_is_integer_type(column.get_data_type()) ? "Return exactly one base-10 integer. Do not include other text."
        : (ob_is_numeric_type(column.get_data_type()) || ob_is_decimal_int(column.get_data_type()))
          ? "Return exactly one numeric value. Do not include other text."
          : "Return only the text value. Do not use markdown fences.";
    ret = ob_write_string(allocator, ObString(hint), call.system_);
  }
  const int restore = ObLocalSessionVarHelper::update_session_vars_with_local(saved, *session);
  if (OB_SUCC(ret)) { ret = restore; }
  return ret;
}
StatementContext *context(ObExecContext &ctx)
{
  if (ctx.ai_column_context() == nullptr) {
    void *mem = ctx.get_allocator().alloc(sizeof(StatementContext));
    if (mem != nullptr) { ctx.ai_column_context() = new(mem) StatementContext(ctx); }
  }
  return ctx.ai_column_context();
}
} // anonymous namespace

int decode_objects(const ObString &bytes, ObIArray<ObObj> &out)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0, count = 0;
  out.reuse();
  if (OB_FAIL(serialization::decode_i64(bytes.ptr(), bytes.length(), pos, &count))) {
  } else if (count < 0 || count > 4096) { ret = OB_INVALID_DATA; }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    ObObj value;
    if (OB_FAIL(value.deserialize(bytes.ptr(), bytes.length(), pos))) {
    } else { ret = out.push_back(value); }
  }
  if (OB_SUCC(ret) && pos != bytes.length()) { ret = OB_INVALID_DATA; }
  return ret;
}
int make_rowkey_predicate(const ObTableSchema &schema, const ObString &key, ObSqlString &sql,
                         ObIAllocator &allocator, ObSQLSessionInfo *session)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObObj, 8> fields;
  if (OB_FAIL(decode_objects(key, fields))) {
  } else if (fields.count() != 2 * schema.get_rowkey_column_num()) { ret = OB_INVALID_DATA; }
  for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); i += 2) {
    const ObColumnSchemaV2 *column = schema.get_column_schema(fields.at(i).get_uint64());
    if (column == nullptr) { ret = OB_SCHEMA_EAGAIN; }
    else if (i != 0 && OB_FAIL(sql.append(" AND "))) {
    } else if (OB_FAIL(append_identifier(sql, column->get_column_name_str()))) {
    } else if (OB_FAIL(sql.append(" = "))) {
    } else { ret = append_literal(sql, fields.at(i + 1), allocator, session); }
  }
  return ret;
}
int make_rowkey_after(const ObTableSchema &schema, const ObString &key, ObSqlString &sql,
                      ObIAllocator &allocator, ObSQLSessionInfo *session)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObObj, 8> fields;
  if (OB_FAIL(decode_objects(key, fields))) {
  } else if (fields.count() != 2 * schema.get_rowkey_column_num()) { ret = OB_INVALID_DATA; }
  else if (OB_FAIL(sql.append("("))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); i += 2) {
    const auto *column = schema.get_column_schema(fields.at(i).get_uint64());
    if (column == nullptr) { ret = OB_SCHEMA_EAGAIN; }
    else if (i > 0 && OB_FAIL(sql.append(","))) {
    } else { ret = append_identifier(sql, column->get_column_name_str()); }
  }
  if (OB_SUCC(ret)) { ret = sql.append(") > ("); }
  for (int64_t i = 1; OB_SUCC(ret) && i < fields.count(); i += 2) {
    if (i > 1 && OB_FAIL(sql.append(","))) {
    } else { ret = append_literal(sql, fields.at(i), allocator, session); }
  }
  if (OB_SUCC(ret)) { ret = sql.append(")"); }
  return ret;
}
StatementContext::StatementContext(ObExecContext &ctx) : ctx_(ctx), rows_("AiChanges"), keys_("AiFinalKeys"), inited_(false) {}
StatementContext::~StatementContext() { rows_.reset(); keys_.reset(); }
int StatementContext::init()
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = rows_.init(8 * 1024 * 1024, ObCtxIds::DEFAULT_CTX_ID, "AiChanges", true);
    if (OB_SUCC(ret)) { ret = keys_.init(8 * 1024 * 1024, ObCtxIds::DEFAULT_CTX_ID, "AiFinalKeys", true); }
    if (OB_SUCC(ret)) { ret = rows_.alloc_dir_id(); }
    if (OB_SUCC(ret)) { ret = keys_.alloc_dir_id(); }
    if (OB_SUCC(ret)) { inited_ = true; }
  }
  return ret;
}
int StatementContext::collect(const ObIArray<ObObj> &fields)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiRowCollect");
  ObString bytes;
  ObDatum datum;
  if (OB_FAIL(init())) {
  } else if (OB_FAIL(encode_objects(fields, allocator, bytes))) {
  } else {
    datum.set_string(bytes);
    ret = rows_.add_row(&datum, 1, 0, nullptr);
  }
  return ret;
}

int StatementContext::collect_key(const ObIArray<ObObj> &fields)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiKeyCollect");
  ObString bytes;
  ObDatum datum;
  if (OB_FAIL(init())) {
  } else if (OB_FAIL(encode_objects(fields, allocator, bytes))) {
  } else {
    datum.set_string(bytes);
    ret = keys_.add_row(&datum, 1, 0, nullptr);
  }
  return ret;
}

namespace {
int set_value(ObExpr *expr, ObEvalCtx &eval, const ObObj &value)
{
  int ret = OB_SUCCESS;
  ObDatum &datum = expr->locate_datum_for_write(eval);
  if (value.is_null()) { datum.set_null(); }
  else if (value.is_string_type()) {
    const ObString &str = value.get_string();
    char *data = expr->get_str_res_mem(eval, str.length());
    if (data == nullptr && str.length() != 0) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else { MEMCPY(data, str.ptr(), str.length()); datum.set_string(data, str.length()); }
  } else { ret = datum.from_obj(value); }
  if (OB_SUCC(ret)) {
    expr->get_eval_info(eval).evaluated_ = true;
    // Related-index generated expressions may already have been evaluated by
    // row materialization. Invalidate their caches before DAS copies the row.
    ObSEArray<ObExpr *, 16> parents;
    for (uint32_t i = 0; OB_SUCC(ret) && i < expr->parent_cnt_; ++i) { ret = parents.push_back(expr->parents_[i]); }
    for (int64_t i = 0; OB_SUCC(ret) && i < parents.count(); ++i) {
      ObExpr *parent = parents.at(i);
      if (parent != nullptr && parent->get_eval_info(eval).evaluated_) {
        parent->get_eval_info(eval).evaluated_ = false;
        for (uint32_t j = 0; OB_SUCC(ret) && j < parent->parent_cnt_; ++j) { ret = parents.push_back(parent->parents_[j]); }
      }
    }
  }
  return ret;
}
int set_column(const ObDMLBaseCtDef &ctdef, ObEvalCtx &eval, uint64_t cid, const ObObj &value)
{
  const int64_t idx = position(ctdef, cid);
  return idx < 0 || idx >= ctdef.new_row_.count() ? OB_ERR_UNEXPECTED
      : set_value(ctdef.new_row_.at(idx), eval, value);
}
int serialize_call(const PreparedCall &call, ObIAllocator &allocator, ObString &bytes)
{
  int ret = OB_SUCCESS;
  const int64_t size = call.get_serialize_size();
  int64_t pos = 0;
  char *data = static_cast<char *>(allocator.alloc(size));
  if (data == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else if (OB_FAIL(call.serialize(data, size, pos))) {
  } else { bytes.assign_ptr(data, pos); }
  return ret;
}
int serialize_target(const ObColumnSchemaV2 &column, ObIAllocator &allocator, ObString &bytes)
{
  int ret = OB_SUCCESS;
  const int64_t size = column.get_serialize_size();
  int64_t pos = 0;
  char *data = static_cast<char *>(allocator.alloc(size));
  if (data == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else if (OB_FAIL(column.serialize(data, size, pos))) {
  } else { bytes.assign_ptr(data, pos); }
  return ret;
}
bool local_input_error(int error)
{
  return error == OB_INVALID_ARGUMENT || error == OB_ERR_INVALID_JSON_TEXT
      || error == OB_ERR_NULL_VALUE;
}
int supersede(TxnSqlGuard &guard, uint64_t tid, uint64_t cid,
              const ObString &encoded_spec, const ObString &keep_request)
{
  int ret = OB_SUCCESS;
  AiColumnSpec spec;
  if (OB_FAIL(spec.decode(encoded_spec))) {
  } else {
    ObSqlString sql;
    int64_t affected = 0;
    if (OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=6,claim_token=NULL,lease_until=NULL "
                              "WHERE table_id=%lu AND column_id=%lu AND row_incarnation=", tid, cid))) {
    } else if (OB_FAIL(append_hex(sql, ObString(16, spec.incarnation_)))) {
    }
    if (OB_SUCC(ret) && !keep_request.empty()) {
      if (OB_FAIL(sql.append(" AND request_id<>"))) {
      } else { ret = append_hex(sql, keep_request); }
    }
    if (OB_SUCC(ret)) { ret = guard.write(sql.string(), affected); }
    // A group with no remaining live member has no publication work. Remove
    // it in T1 (not after commit), including DELETE of an uncommitted insert.
    // Partially replaced groups retain their original member barrier.
    sql.reset();
    if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(
        "DELETE FROM oceanbase.__all_ai_column_group WHERE table_id=%lu AND row_incarnation=", tid))) {
    } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, ObString(16, spec.incarnation_)))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append(
        " AND NOT EXISTS (SELECT 1 FROM oceanbase.__all_ai_column_task t WHERE "
        "t.table_id=__all_ai_column_group.table_id AND t.request_id=__all_ai_column_group.request_id AND t.state<>6)"))) {
    } else if (OB_SUCC(ret)) { ret = guard.write(sql.string(), affected); }
    sql.reset();
    if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(
        "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND row_incarnation=", tid))) {
    } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, ObString(16, spec.incarnation_)))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append(
        " AND state=6 AND NOT EXISTS (SELECT 1 FROM oceanbase.__all_ai_column_group g WHERE "
        "g.table_id=__all_ai_column_task.table_id AND g.request_id=__all_ai_column_task.request_id)"))) {
    } else if (OB_SUCC(ret)) { ret = guard.write(sql.string(), affected); }
  }
  return ret;
}
} // anonymous namespace

int prepare(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op)
{
  int ret = OB_SUCCESS;
  ObExecContext &ctx = op.get_exec_ctx();
  ObEvalCtx &eval = op.get_eval_ctx();
  if (!ctdef.is_primary_index_ || !ctdef.has_ai_columns_) { return ret; }
  const WriteCapability *cap = ctx.ai_column_capability();
  // A cached internal plan carries no authority. Every execution must receive
  // its own short-lived capability from the owning statement.
  if (ctdef.ai_private_write_) {
    if (cap == nullptr || cap->table_id_ != ctdef.das_base_ctdef_.index_tid_
        || static_cast<int64_t>(cap->kind_) != ctdef.ai_private_kind_) { return OB_ERR_NO_PRIVILEGE; }
    for (int64_t i = 0; i < ctdef.ai_write_columns_.count(); ++i) {
      if (!cap->allows(ctdef.das_base_ctdef_.index_tid_, ctdef.ai_write_columns_.at(i))) { return OB_ERR_NO_PRIVILEGE; }
    }
  }
  if (cap != nullptr && cap->kind_ == WriteKind::PUBLISH) { return ret; }
  const ObTableSchema *table = nullptr;
  ObArenaAllocator allocator("AiRowPrepare");
  ObSEArray<ObObj, 32> old_row, new_row;
  const bool insert = ctdef.dml_type_ == DAS_OP_TABLE_INSERT;
  bool key_changed = false;
  ObString request, incarnation_text;
  char incarnation[16], request_binary[16];
  StatementContext *statement = context(ctx);
  if (statement == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else if (OB_FAIL(get_schema(ctx, ctdef.das_base_ctdef_.index_tid_, table))) {
  } else if (OB_FAIL(row_objects(ctdef.new_row_, eval, new_row))) {
  } else if (!insert && OB_FAIL(row_objects(ctdef.old_row_, eval, old_row))) {
  }
  for (int64_t k = 0; OB_SUCC(ret) && !insert && k < table->get_rowkey_column_num(); ++k) {
    const int64_t idx = position(ctdef, table->get_rowkey_info().get_column(k)->column_id_);
    bool different = false;
    if (idx < 0 || idx >= old_row.count() || idx >= new_row.count()) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(differs(ctx, allocator, old_row.at(idx), new_row.at(idx), different))) {
    } else { key_changed |= different; }
  }
  // One identity per INSERT, shared by every AI member of the row. Legacy ADD
  // rows acquire an identity from the first initialized member when available.
  bool found_incarnation = false;
  for (int64_t i = 0; OB_SUCC(ret) && !insert && !found_incarnation && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 *col = table->get_column_schema_by_idx(i);
    if (col != nullptr && col->is_ai_column()) {
      const int64_t idx = position(ctdef, col->get_ai_result_spec_column_id());
      AiColumnSpec spec;
      if (idx >= 0 && idx < old_row.count() && !old_row.at(idx).is_null()) {
        if (OB_FAIL(spec.decode(old_row.at(idx).get_string()))) {
        } else { MEMCPY(incarnation, spec.incarnation_, 16); found_incarnation = true; }
      }
    }
  }
  if (OB_SUCC(ret) && !found_incarnation && OB_FAIL(uuid(allocator, incarnation_text, incarnation))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(i);
    if (column == nullptr || !column->is_ai_column()) { continue; }
    const uint64_t cid = column->get_column_id();
    bool changed = insert || key_changed;
    const int64_t old_spec_pos = position(ctdef, column->get_ai_result_spec_column_id());
    const bool missing = !insert && old_spec_pos >= 0 && old_row.at(old_spec_pos).is_null();
    const bool forced = cap != nullptr && cap->allows(table->get_table_id(), cid)
        && (cap->kind_ == WriteKind::RETRY || (cap->kind_ == WriteKind::BACKFILL && missing));
    changed |= forced || ctdef.ai_retry_column_id_ == cid;
    ObSEArray<uint64_t, 8> dependencies;
    if (OB_FAIL(column->get_ai_dependencies(dependencies))) { break; }
    for (int64_t d = 0; OB_SUCC(ret) && !changed && d < dependencies.count(); ++d) {
      const int64_t idx = position(ctdef, dependencies.at(d));
      if (idx < 0 || idx >= old_row.count() || idx >= new_row.count()) { ret = OB_ERR_UNEXPECTED; }
      else { ret = differs(ctx, allocator, old_row.at(idx), new_row.at(idx), changed); }
    }
    if (OB_FAIL(ret) || !changed) { continue; }
    AiColumnSpec spec;
    const int64_t spec_pos = position(ctdef, column->get_ai_result_spec_column_id());
    const int64_t req_pos = position(ctdef, column->get_ai_request_column_id());
    if (spec_pos < 0 || req_pos < 0) { ret = OB_ERR_UNEXPECTED; break; }
    if (!insert && !old_row.at(spec_pos).is_null()) {
      ret = spec.decode(old_row.at(spec_pos).get_string());
    }
    MEMCPY(spec.incarnation_, incarnation, 16);
    spec.definition_version_ = column->get_ai_definition_version();
    ObString spec_bytes, call_bytes, target_bytes;
    PreparedCall call;
    int64_t model_id = 0;
    int call_error = OB_SUCCESS;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(spec.advance())) {
    } else if (OB_FAIL(spec.encode(allocator, spec_bytes))) {
    } else if (request.empty() && OB_FAIL(uuid(allocator, request, request_binary))) {
    } else {
      call_error = prepare_call(ctx, ctdef, *table, *column, new_row, allocator, call, model_id);
      if (call_error != OB_SUCCESS && !local_input_error(call_error)) { ret = call_error; }
      else if (OB_FAIL(serialize_call(call, allocator, call_bytes))) {
      } else { ret = serialize_target(*column, allocator, target_bytes); }
    }
    ObObj value;
    if (OB_SUCC(ret)) { value.set_null(); ret = set_column(ctdef, eval, cid, value); }
    if (OB_SUCC(ret)) { value.set_tinyint(0); ret = set_column(ctdef, eval, column->get_ai_state_column_id(), value); }
    if (OB_SUCC(ret)) { value.set_varchar(request); ret = set_column(ctdef, eval, column->get_ai_request_column_id(), value); }
    if (OB_SUCC(ret)) { value.set_varchar(spec_bytes); ret = set_column(ctdef, eval, column->get_ai_result_spec_column_id(), value); }
    ObObj fields[FIELD_COUNT];
    fields[TABLE].set_uint64(table->get_table_id()); fields[COLUMN].set_uint64(cid);
    fields[REQUEST].set_varchar(request); fields[SPEC].set_varchar(spec_bytes);
    fields[INCARNATION].set_varchar(ObString(16, incarnation));
    fields[GENERATION].set_uint64(spec.generation_); fields[DEFINITION].set_int(spec.definition_version_);
    fields[RETRIES].set_int(column->get_ai_max_retries()); fields[DATABASE].set_uint64(table->get_database_id());
    fields[CALL].set_varchar(call_bytes); fields[TARGET].set_varchar(target_bytes);
    fields[OLD_REQUEST] = insert ? ObObj() : old_row.at(req_pos);
    const bool embed = call.operation_ == 1;
    fields[STATE].set_int(call_error != OB_SUCCESS || (call.input_null_ && !embed)
        ? static_cast<int64_t>(AiTaskState::STAGED_FAILED)
        : call.input_null_ && embed ? static_cast<int64_t>(AiTaskState::STAGED_SUCCESS)
        : static_cast<int64_t>(AiTaskState::QUEUED));
    fields[ERROR].set_int(call_error != OB_SUCCESS ? call_error : call.input_null_ && !embed ? OB_INVALID_ARGUMENT : 0);
    fields[MODEL_ID].set_int(model_id); fields[MODEL_NAME].set_varchar(call.model_);
    fields[OPERATION].set_int(embed ? 1 : 0);
    if (OB_SUCC(ret)) {
      ObSEArray<ObObj, FIELD_COUNT> record;
      for (int64_t f = 0; OB_SUCC(ret) && f < FIELD_COUNT; ++f) { ret = record.push_back(fields[f]); }
      if (OB_SUCC(ret)) { ret = statement->collect(record); }
    }
  }
  return ret;
}

int materialize(const ObDMLBaseCtDef &ctdef, ObExecContext &ctx, ObEvalCtx &eval)
{
  int ret = OB_SUCCESS;
  if (!ctdef.is_primary_index_ || !ctdef.has_ai_columns_ || ctx.ai_column_context() == nullptr) { return ret; }
  const WriteCapability *cap = ctx.ai_column_capability();
  if (cap != nullptr && cap->kind_ == WriteKind::PUBLISH) { return ret; }
  ObArenaAllocator allocator("AiFinalKey");
  const ObTableSchema *table = nullptr;
  ObSEArray<ObObj, 32> row;
  ObString key;
  if (OB_FAIL(get_schema(ctx, ctdef.das_base_ctdef_.index_tid_, table))) {
  } else if (OB_FAIL(row_objects(ctdef.new_row_, eval, row))) {
  } else if (OB_FAIL(make_key(ctdef, *table, row, allocator, key))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(i);
    if (column == nullptr || !column->is_ai_column()) { continue; }
    const int64_t rp = position(ctdef, column->get_ai_request_column_id());
    const int64_t sp = position(ctdef, column->get_ai_result_spec_column_id());
    if (rp < 0 || sp < 0) { ret = OB_ERR_UNEXPECTED; }
    else if (!row.at(rp).is_null() && !row.at(sp).is_null()) {
      ObObj fields[4];
      fields[0].set_varchar(key); fields[1] = row.at(rp);
      fields[2].set_uint64(column->get_column_id()); fields[3] = row.at(sp);
      ObSEArray<ObObj, 4> record;
      for (int64_t f = 0; OB_SUCC(ret) && f < 4; ++f) { ret = record.push_back(fields[f]); }
      if (OB_SUCC(ret)) { ret = ctx.ai_column_context()->collect_key(record); }
    }
  }
  return ret;
}

int collect(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op, bool deleted)
{
  if (!deleted) { return materialize(ctdef, op.get_exec_ctx(), op.get_eval_ctx()); }
  int ret = OB_SUCCESS;
  if (!ctdef.is_primary_index_ || !ctdef.has_ai_columns_) { return ret; }
  ObExecContext &ctx = op.get_exec_ctx();
  ObArenaAllocator allocator("AiDeleteRow");
  const ObTableSchema *table = nullptr;
  ObSEArray<ObObj, 32> row;
  ObString key;
  StatementContext *statement = context(ctx);
  if (statement == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else if (OB_FAIL(get_schema(ctx, ctdef.das_base_ctdef_.index_tid_, table))) {
  } else if (OB_FAIL(row_objects(ctdef.old_row_, op.get_eval_ctx(), row))) {
  } else if (OB_FAIL(make_key(ctdef, *table, row, allocator, key))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(i);
    if (column == nullptr || !column->is_ai_column()) { continue; }
    const int64_t rp = position(ctdef, column->get_ai_request_column_id());
    const int64_t sp = position(ctdef, column->get_ai_result_spec_column_id());
    if (rp < 0 || sp < 0) { ret = OB_ERR_UNEXPECTED; }
    else if (!row.at(rp).is_null() && !row.at(sp).is_null()) {
      ObObj fields[FIELD_COUNT];
      fields[TABLE].set_uint64(table->get_table_id()); fields[COLUMN].set_uint64(column->get_column_id());
      fields[KEY].set_varchar(key); fields[SPEC] = row.at(sp); fields[REQUEST] = row.at(rp);
      fields[OLD_REQUEST] = row.at(rp); fields[STATE].set_int(-1);
      ObSEArray<ObObj, FIELD_COUNT> record;
      for (int64_t f = 0; OB_SUCC(ret) && f < FIELD_COUNT; ++f) { ret = record.push_back(fields[f]); }
      if (OB_SUCC(ret)) { ret = statement->collect(record); }
    }
  }
  return ret;
}

int StatementContext::finish()
{
  int ret = OB_SUCCESS;
  TxnSqlGuard guard(ctx_);
  ObChunkDatumStore::Iterator iterator;
  const ObChunkDatumStore::StoredRow *stored = nullptr;
  if (!inited_) { return ret; }
  if (OB_FAIL(rows_.finish_add_row())) {
  } else if (OB_FAIL(keys_.finish_add_row())) {
  } else if (OB_FAIL(guard.open())) {
  } else if (OB_FAIL(rows_.begin(iterator))) {
  }
  while (OB_SUCC(ret) && OB_SUCC(iterator.get_next_row(stored))) {
    ObArenaAllocator allocator("AiEnqueue");
    ObSEArray<ObObj, FIELD_COUNT> record;
    const ObTableSchema *table = nullptr;
    ObString key;
    if (OB_FAIL(decode_objects(stored->cells()[0].get_string(), record))) {
    } else if (record.count() != FIELD_COUNT) { ret = OB_INVALID_DATA; }
    else if (OB_FAIL(get_schema(ctx_, record.at(TABLE).get_uint64(), table))) {
    }
    if (OB_FAIL(ret)) { break; }
    const ObColumnSchemaV2 *column = table->get_column_schema(record.at(COLUMN).get_uint64());
    if (column == nullptr || !column->is_ai_column()) { ret = OB_SCHEMA_EAGAIN; break; }
    const bool deleted = record.at(STATE).get_int() == -1;
    bool found = deleted;
    if (deleted) { key = record.at(KEY).get_string(); }
    else {
      // Keys are recorded at final DAS materialization, after heap-key
      // allocation. Both stores can spill; no execution-frame address survives.
      ObChunkDatumStore::Iterator keys;
      const ObChunkDatumStore::StoredRow *binding = nullptr;
      if (OB_FAIL(keys_.begin(keys))) { break; }
      int key_ret = OB_SUCCESS;
      while (OB_SUCC(ret) && OB_SUCCESS == (key_ret = keys.get_next_row(binding))) {
        ObSEArray<ObObj, 4> fields;
        if (OB_FAIL(decode_objects(binding->cells()[0].get_string(), fields))) {
        } else if (fields.count() != 4) { ret = OB_INVALID_DATA; }
        else if (fields.at(1).get_string() == record.at(REQUEST).get_string()
                 && fields.at(2).get_uint64() == column->get_column_id()
                 && fields.at(3).get_string() == record.at(SPEC).get_string()) {
          ret = ob_write_string(allocator, fields.at(0).get_string(), key);
          found = OB_SUCC(ret);
        }
      }
      if (OB_SUCC(ret) && key_ret != OB_ITER_END) { ret = key_ret; }
    }
    if (OB_FAIL(ret) || !found) { continue; }
    ObSqlString qualified, predicate, check;
    bool current = false;
    if (OB_FAIL(table_name(ctx_, *table, qualified))) {
    } else if (OB_FAIL(make_rowkey_predicate(*table, key, predicate, allocator, ctx_.get_my_session()))) {
    } else if (OB_FAIL(check.append("SELECT 1 FROM "))) {
    } else if (OB_FAIL(check.append(qualified.string()))) {
    } else if (OB_FAIL(check.append(" WHERE "))) {
    } else if (OB_FAIL(check.append(predicate.string()))) {
    } else if (OB_FAIL(check.append(" AND "))) {
    } else if (OB_FAIL(append_identifier(check, column->get_ai_request_column_id() == OB_INVALID_ID
                          ? ObString() : table->get_column_schema(column->get_ai_request_column_id())->get_column_name_str()))) {
    } else if (OB_FAIL(check.append("="))) {
    } else if (OB_FAIL(append_hex(check, record.at(REQUEST).get_string()))) {
    } else if (OB_FAIL(check.append(" AND "))) {
    } else if (OB_FAIL(append_identifier(check, table->get_column_schema(column->get_ai_result_spec_column_id())->get_column_name_str()))) {
    } else if (OB_FAIL(check.append("="))) {
    } else if (OB_FAIL(append_hex(check, record.at(SPEC).get_string()))) {
    }
    if (OB_SUCC(ret)) {
      SMART_VAR(ObISQLClient::ReadResult, result) {
        if (OB_FAIL(guard.read(check.string(), result))) {
        } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else {
          const int next = result.get_result()->next();
          if (next == OB_SUCCESS) { current = true; }
          else if (next != OB_ITER_END) { ret = next; }
        }
      }
    }
    if (OB_FAIL(ret)) { break; }
    if (deleted ? !current : current) {
      ret = supersede(guard, table->get_table_id(), column->get_column_id(), record.at(SPEC).get_string(),
                      deleted ? ObString() : record.at(REQUEST).get_string());
    }
    if (OB_FAIL(ret) || deleted || !current) { continue; }
    ObSqlString sql;
    int64_t affected = 0;
    if (OB_FAIL(sql.append_fmt("INSERT INTO oceanbase.__all_ai_column_group"
              "(table_id,request_id,database_id,row_incarnation,rowkey,member_count,state) VALUES(%lu,",
              table->get_table_id()))) {
    } else if (OB_FAIL(append_hex(sql, record.at(REQUEST).get_string()))) {
    } else if (OB_FAIL(sql.append_fmt(",%lu,", table->get_database_id()))) {
    } else if (OB_FAIL(append_hex(sql, record.at(INCARNATION).get_string()))) {
    } else if (OB_FAIL(sql.append(","))) {
    } else if (OB_FAIL(append_hex(sql, key))) {
    } else if (OB_FAIL(sql.append(",0,0) ON DUPLICATE KEY UPDATE request_id=request_id"))) {
    } else { ret = guard.write(sql.string(), affected); }
    sql.reset();
    if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt("INSERT INTO oceanbase.__all_ai_column_task"
        "(table_id,request_id,column_id,database_id,row_incarnation,rowkey,rowkey_predicate,"
        "generation,definition_version,state,eligible_ts,max_retries,model_id,model_name,operation,"
        "prepared_call,result_type_spec,error_code) VALUES(%lu,", table->get_table_id()))) {
    }
    if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, record.at(REQUEST).get_string()))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(",%lu,%lu,", column->get_column_id(), table->get_database_id()))) {
    } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, record.at(INCARNATION).get_string()))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append(","))) {
    } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, key))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append(","))) {
    } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, predicate.string()))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(",%lu,%ld,%ld,NOW(6),%ld,%ld,",
        record.at(GENERATION).get_uint64(), record.at(DEFINITION).get_int(), record.at(STATE).get_int(),
        record.at(RETRIES).get_int(), record.at(MODEL_ID).get_int()))) {
    } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, record.at(MODEL_NAME).get_string()))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(",%ld,", record.at(OPERATION).get_int()))) {
    } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, record.at(CALL).get_string()))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append(","))) {
    } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, record.at(TARGET).get_string()))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(",%ld)", record.at(ERROR).get_int()))) {
    } else if (OB_SUCC(ret)) { ret = guard.write(sql.string(), affected); }
    // Membership is finalized entirely within T1 and is immutable afterwards.
    if (OB_SUCC(ret) && affected == 1) {
      sql.reset();
      if (OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_group SET member_count=member_count+1 "
                                "WHERE table_id=%lu AND request_id=", table->get_table_id()))) {
      } else if (OB_FAIL(append_hex(sql, record.at(REQUEST).get_string()))) {
      } else { ret = guard.write(sql.string(), affected); }
    }
    const WriteCapability *cap = ctx_.ai_column_capability();
    if (OB_SUCC(ret) && cap != nullptr && cap->kind_ == WriteKind::BACKFILL
        && cap->last_rowkey_ != nullptr && cap->progress_allocator_ != nullptr) {
      ret = ob_write_string(*cap->progress_allocator_, key, *cap->last_rowkey_);
    }
  }
  if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  const int close_ret = guard.close();
  if (OB_SUCC(ret)) { ret = close_ret; }
  return ret;
}

int finish(ObExecContext &ctx, bool rollback)
{
  int ret = OB_SUCCESS;
  StatementContext *statement = ctx.ai_column_context();
  if (statement != nullptr) {
    if (!rollback) { ret = statement->finish(); }
    statement->~StatementContext();
    ctx.ai_column_context() = nullptr;
  }
  return ret;
}

int schema_fence(ObExecContext &ctx)
{
  int ret = OB_SUCCESS;
  const ObPhysicalPlan *plan = ctx.get_physical_plan_ctx() == nullptr ? nullptr
      : ctx.get_physical_plan_ctx()->get_phy_plan();
  if (plan == nullptr || !plan->is_dml_write_stmt()) { return ret; }
  ObSEArray<uint64_t, 8> ids;
  const auto &dependencies = plan->get_dependency_table();
  for (int64_t i = 0; OB_SUCC(ret) && i < dependencies.count(); ++i) {
    const auto &dependency = dependencies.at(i);
    if (dependency.get_schema_type() != TABLE_SCHEMA) { continue; }
    const ObTableSchema *table = nullptr;
    if (OB_FAIL(get_schema(ctx, dependency.object_id_, table))) {
    } else if (table->is_user_table() && !has_exist_in_array(ids, table->get_table_id())) {
      ret = ids.push_back(table->get_table_id());
    }
  }
  std::sort(ids.begin(), ids.end());
  ObSEArray<ObObjectID, 1> partitions;
  ObSchemaGetterGuard latest;
  for (int64_t i = 0; OB_SUCC(ret) && i < ids.count(); ++i) {
    ret = ObSqlTransControl::lock_table(ctx, ids.at(i), partitions,
                                      transaction::tablelock::ROW_EXCLUSIVE, -1);
  }
  // Take the schema guard only after all intents have been acquired. A guard
  // captured before waiting for ADD could otherwise validate an obsolete plan.
  if (OB_SUCC(ret) && OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(latest))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ids.count(); ++i) {
    const ObTableSchema *compiled = nullptr, *current = nullptr;
    if (OB_FAIL(get_schema(ctx, ids.at(i), compiled))) {
    } else if (OB_FAIL(latest.get_table_schema(ids.at(i), current))) {
    } else if (current == nullptr || current->get_schema_version() != compiled->get_schema_version()) {
      ret = OB_SCHEMA_EAGAIN;
    }
  }
  return ret;
}
int hold_load_schema_barrier(ObExecContext &ctx)
{
  int ret = OB_SUCCESS;
  if (!ctx.ai_load_schema_barrier()) {
    if (ctx.get_my_session() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(ai_metadata_barrier.rdlock(ctx.get_my_session()->get_query_timeout_ts()))) {
    } else { ctx.set_ai_load_schema_barrier(true); }
  }
  return ret;
}
void release_load_schema_barrier(ObExecContext &ctx)
{
  if (ctx.ai_load_schema_barrier()) {
    ctx.set_ai_load_schema_barrier(false);
    ai_metadata_barrier.rdunlock();
  }
}
bool try_hold_ddl_schema_barrier() { return ai_metadata_barrier.try_wrlock(); }
void release_ddl_schema_barrier() { ai_metadata_barrier.wrunlock(); }
} } } // namespace oceanbase::sql::ai_column

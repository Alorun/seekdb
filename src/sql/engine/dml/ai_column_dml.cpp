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

namespace oceanbase { namespace sql { namespace ai_column {
using namespace common;
using namespace share;
using namespace share::schema;
OB_SERIALIZE_MEMBER(PreparedCall, model_, input_, config_, dimension_, input_null_);

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
    opened_(false), old_internal_(false), old_iter_uncommitted_(false) {}
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
  } else if (db == nullptr) { ret = OB_BAD_DATABASE; }
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
  ObObj input, option;
  ParamStore params;
  if (OB_FAIL(ObLocalSessionVarHelper::load_session_vars(session, saved))) { return ret; }
  if (OB_FAIL(ObLocalSessionVarHelper::update_session_vars_with_local(column.get_local_session_var(), *session))) {
  } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(column.get_ai_normalized_expr(),
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
  if (OB_SUCC(ret) && OB_FAIL(root->formalize(session))) {
  }
  // Only ordinary parameter subtrees enter expression evaluation. The AI root
  // is deliberately never handed to code generation or eval.
  if (OB_SUCC(ret) && OB_FAIL(ObSQLUtils::calc_const_expr(ctx, root->get_param_expr(1), input, allocator, params))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(ObSQLUtils::calc_const_expr(ctx, root->get_param_expr(2), option, allocator, params))) {
  }
  if (OB_SUCC(ret)) {
    call.model_ = static_cast<ObConstRawExpr *>(root->get_param_expr(0))->get_value().get_string();
    const ObAiModelSchema *model = nullptr;
    if (OB_FAIL(ctx.get_sql_ctx()->schema_guard_->get_ai_model_schema(call.model_, model))) {
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
    if (OB_SUCC(ret) && root->get_expr_type() == T_FUN_SYS_AI_EMBED) {
      call.dimension_ = option.get_int();
    } else if (OB_SUCC(ret) && !option.is_null()) {
      ObIJsonBase *config = nullptr;
      if (OB_FAIL(tree_from_obj(ctx, allocator, option, config))) {
      } else if (config->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_ARGUMENT; }
      else {
        ObJsonObject *object = static_cast<ObJsonObject *>(config);
        if (object->get_value("__seekdb_ai_column_result_prompt") != nullptr) {
          ret = object->remove(ObString("__seekdb_ai_column_result_prompt"));
        }
        if (OB_SUCC(ret) && OB_FAIL(ObAIFuncUtils::validate_complete_config(allocator, object))) {
        } else if (OB_SUCC(ret)) { ret = ObAIFuncJsonUtils::print_json_to_str(allocator, object, call.config_); }
      }
    }
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
StatementContext::StatementContext(ObExecContext &ctx) : ctx_(ctx), rows_(), inited_(false) {}
StatementContext::~StatementContext() { rows_.reset(); }
int StatementContext::init()
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = rows_.init(8 * 1024 * 1024, ObCtxIds::DEFAULT_CTX_ID, "AiChanges", true);
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

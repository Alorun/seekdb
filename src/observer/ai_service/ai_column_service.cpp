// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.
#define USING_LOG_PREFIX SERVER
#include "observer/ai_service/ai_column_service.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_column_conv.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/ob_sql_utils.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "query/engine/expr/ob_expr_lob_utils.h"
#include "share/ob_lob_access_utils.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_ai_model_mgr.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/tablelock/ob_lock_inner_connection_util.h"

namespace oceanbase { namespace observer {
using namespace common;
using namespace share;
using namespace share::schema;
using namespace sql;
using namespace sql::ai_column;
namespace {
// Process counters reset on restart; durable per-task intents remain the
// retry-budget authority. Neither counter claims endpoint exactly-once.
std::atomic<uint64_t> dispatch_intents{0};
std::atomic<uint64_t> http_dispatches{0};
struct Task {
  uint64_t table_ = OB_INVALID_ID;
  uint64_t column_ = OB_INVALID_ID;
  int64_t model_id_ = 0;
  int64_t state_ = 0;
  int64_t intents_ = 0;
  int64_t retries_ = 0;
  int64_t budget_ = 0;
  ObString request_, token_, call_, target_, raw_, attempt_;
};
struct Attempt {
  OB_UNIS_VERSION(1);
public:
  ObString provider_;
  int64_t http_code_ = 0;
  int64_t transport_error_ = 0;
};
OB_SERIALIZE_MEMBER(Attempt, provider_, http_code_, transport_error_);

int unhex(const ObString &text, ObIAllocator &allocator, ObString &value)
{
  int ret = OB_SUCCESS;
  if (text.length() % 2 != 0) { return OB_INVALID_DATA; }
  char *bytes = static_cast<char *>(allocator.alloc(MAX(1, text.length() / 2)));
  if (bytes == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  auto digit = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0'
      : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  for (int64_t i = 0; OB_SUCC(ret) && i < text.length(); i += 2) {
    const int a = digit(text.ptr()[i]), b = digit(text.ptr()[i + 1]);
    if (a < 0 || b < 0) { ret = OB_INVALID_DATA; }
    else { bytes[i / 2] = static_cast<char>(a * 16 + b); }
  }
  if (OB_SUCC(ret)) { value.assign_ptr(bytes, text.length() / 2); }
  return ret;
}
int get_id(sqlclient::ObMySQLResult &result, const char *name, uint64_t &id)
{
  int64_t value = 0;
  const int ret = result.get_int(name, value);
  if (ret != OB_SUCCESS) { return ret; }
  if (value < 0) { return OB_INVALID_DATA; }
  id = static_cast<uint64_t>(value);
  return OB_SUCCESS;
}
int get_bytes(sqlclient::ObMySQLResult &result, const char *name, ObIAllocator &allocator, ObString &bytes)
{
  int ret = OB_SUCCESS;
  ObObj object;
  ObString hex;
  ObILobReadService *reader = server_service<ObILobReadService>();
  if (reader == nullptr) { ret = OB_NOT_INIT; }
  else if (OB_FAIL(result.get_obj(name, object))) {
  } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                 ObLobReadOptions(*reader, THIS_WORKER.get_timeout_ts()), &allocator, object, hex))) {
  } else { ret = unhex(hex, allocator, bytes); }
  return ret;
}
template <class T> int pack(const T &object, ObIAllocator &allocator, ObString &bytes)
{
  int ret = OB_SUCCESS;
  const int64_t size = object.get_serialize_size();
  int64_t pos = 0;
  char *data = static_cast<char *>(allocator.alloc(size));
  if (data == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else if (OB_FAIL(object.serialize(data, size, pos))) {
  } else { bytes.assign_ptr(data, pos); }
  return ret;
}
template <class T> int unpack(const ObString &bytes, T &object)
{
  int64_t pos = 0;
  const int ret = object.deserialize(bytes.ptr(), bytes.length(), pos);
  return ret != OB_SUCCESS ? ret : pos != bytes.length() ? OB_INVALID_DATA : OB_SUCCESS;
}
int task_key(ObSqlString &sql, const Task &task)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append_fmt("table_id=%lu AND column_id=%lu AND request_id=", task.table_, task.column_))) {
  } else { ret = append_hex(sql, task.request_); }
  return ret;
}
int owner(ObSqlString &sql, const Task &task, int64_t state)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(task_key(sql, task))) {
  } else if (OB_FAIL(sql.append_fmt(" AND state=%ld AND claim_token=", state))) {
  } else if (OB_FAIL(append_hex(sql, task.token_))) {
  } else { ret = sql.append(" AND lease_until>NOW(6)"); }
  return ret;
}
int new_token(ObIAllocator &allocator, ObString &token)
{
  int ret = OB_SUCCESS;
  char uuid[UuidCommon::LENGTH_UUID];
  ObSqlString hex;
  if (OB_FAIL(ObExprUuid::gen_server_uuid(uuid, sizeof(uuid)))) { return ret; }
  for (int64_t i = 0; OB_SUCC(ret) && i < sizeof(uuid); ++i) {
    if (uuid[i] != '-') { ret = hex.append(uuid + i, 1); }
  }
  if (OB_SUCC(ret)) { ret = unhex(hex.string(), allocator, token); }
  return ret;
}
int claim(ObMySQLProxy &proxy, ObIAllocator &allocator, Task &task)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction tx;
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_FAIL(tx.start(&proxy))) { return ret; }
  SMART_VAR(ObISQLClient::ReadResult, result) {
    if (OB_FAIL(tx.read(result, "SELECT table_id,request_id,column_id,model_id,state,attempt_intents,retry_count,max_retries,"
        "HEX(prepared_call) AS args,HEX(result_type_spec) AS target,HEX(IFNULL(raw_response,'')) AS raw,"
        "HEX(IFNULL(attempt_context,'')) AS attempt FROM oceanbase.__all_ai_column_task "
        "WHERE ((state IN (1,3) AND eligible_ts<=NOW(6)) OR "
        "(state=2 AND lease_until<=NOW(6)) OR (state=7 AND (lease_until IS NULL OR lease_until<=NOW(6)))) "
        "ORDER BY eligible_ts,table_id,request_id,column_id LIMIT 1"))) {
    } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else {
      sqlclient::ObMySQLResult &r = *result.get_result();
      ObString request;
      if (OB_FAIL(r.next())) {
      } else if (OB_FAIL(get_id(r, "table_id", task.table_))) {
      } else if (OB_FAIL(get_id(r, "column_id", task.column_))) {
      } else if (OB_FAIL(r.get_varchar("request_id", request))) {
      } else if (OB_FAIL(ob_write_string(allocator, request, task.request_))) {
      } else if (OB_FAIL(r.get_int("model_id", task.model_id_))) {
      } else if (OB_FAIL(r.get_int("state", task.state_))) {
      } else if (OB_FAIL(r.get_int("attempt_intents", task.intents_))) {
      } else if (OB_FAIL(r.get_int("retry_count", task.retries_))) {
      } else if (OB_FAIL(r.get_int("max_retries", task.budget_))) {
      } else if (OB_FAIL(get_bytes(r, "args", allocator, task.call_))) {
      } else if (OB_FAIL(get_bytes(r, "target", allocator, task.target_))) {
      } else if (OB_FAIL(get_bytes(r, "raw", allocator, task.raw_))) {
      } else { ret = get_bytes(r, "attempt", allocator, task.attempt_); }
    }
  }
  const int read_close = result.close();
  result.reset(); // Releases the read context's connection reference too.
  if (OB_SUCC(ret)) { ret = read_close; }
  const bool exhausted = task.state_ != 7 && task.intents_ > task.budget_;
  if (OB_SUCC(ret) && OB_FAIL(new_token(allocator, task.token_))) {
  } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=%d,claim_token=",
                                          exhausted ? 5 : task.state_ == 7 ? 7 : 2))) {
  } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, task.token_))) {
  } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(",lease_until=DATE_ADD(NOW(6),INTERVAL 90 SECOND),"
        "attempt_intents=attempt_intents+%d,error_code=%d WHERE ", task.state_ == 7 || exhausted ? 0 : 1,
        exhausted ? OB_TIMEOUT : 0))) {
  } else if (OB_SUCC(ret) && OB_FAIL(task_key(sql, task))) {
  } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(" AND state=%ld AND attempt_intents=%ld AND "
        "((state IN (1,3) AND eligible_ts<=NOW(6)) OR lease_until IS NULL OR lease_until<=NOW(6))",
        task.state_, task.intents_))) {
  } else if (OB_SUCC(ret) && OB_FAIL(tx.write(sql.ptr(), affected))) {
  } else if (OB_SUCC(ret) && affected != 1) { ret = OB_EAGAIN; }
  const int commit = tx.end(OB_SUCC(ret));
  // Only a positively acknowledged claim may cause an external dispatch.
  // Unknown commit outcomes are recovered from the persisted lease on a later scan.
  if (OB_SUCC(ret) && commit != OB_SUCCESS && !exhausted) {
    sql.reset();
    if (OB_FAIL(sql.append("SELECT 1 FROM oceanbase.__all_ai_column_task WHERE "))) {
    } else if (OB_FAIL(owner(sql, task, task.state_ == 7 ? 7 : 2))) {
    } else {
      SMART_VAR(ObISQLClient::ReadResult, result) {
        if (OB_FAIL(proxy.read(result, sql.ptr()))) {
        } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else { ret = result.get_result()->next(); }
      }
    }
  } else if (OB_SUCC(ret)) { ret = commit; }
  if (OB_SUCC(ret) && exhausted) { ret = OB_EAGAIN; }
  else if (OB_SUCC(ret) && task.state_ != 7) { ++task.intents_; task.state_ = 2; }
  return ret;
}
int build_request(ObIAllocator &allocator, const PreparedCall &call,
                  const ObAiModelSchema &model, const ObAiModelEndpointInfo &endpoint,
                  ObArray<ObString> &headers, ObJsonObject *&body)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo info(allocator, call.operation_ == 1 ? T_FUN_SYS_AI_EMBED : T_FUN_SYS_AI_COMPLETE);
  info.name_ = model.get_name(); info.model_ = model.get_model_name(); info.type_ = model.get_type();
  ObJsonObject *config = nullptr;
  ObIJsonBase *base = nullptr;
  if (!call.config_.empty()) {
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, call.config_, ObJsonInType::JSON_TREE,
                                               ObJsonInType::JSON_TREE, base))) {
    } else if (base->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_DATA; }
    else { config = static_cast<ObJsonObject *>(base); }
  }
  if (OB_SUCC(ret) && OB_FAIL(ObAIFuncUtils::get_header(allocator, info, endpoint, headers))) {
  } else if (OB_SUCC(ret) && call.operation_ == 1) {
    ObArray<ObString> inputs;
    ObJsonInt *dimensions = nullptr;
    if (call.dimension_ <= 0) { ret = OB_INVALID_DATA; }
    else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, call.dimension_, dimensions))) {
    } else if (OB_FAIL(config->add("dimensions", dimensions))) {
    } else if (OB_FAIL(inputs.push_back(call.input_))) {
    } else { ret = ObAIFuncUtils::get_embed_body(allocator, info, endpoint, inputs, config, body); }
  } else if (OB_SUCC(ret)) {
    ObString system = call.system_, input = call.input_;
    ret = ObAIFuncUtils::get_complete_body(allocator, info, endpoint, system, input, config, body);
  }
  return ret;
}
int parse_result(ObIAllocator &allocator, const Task &task, ObObj &typed)
{
  int ret = OB_SUCCESS;
  PreparedCall call;
  Attempt attempt;
  ObColumnSchemaV2 target(&allocator);
  ObIJsonBase *response = nullptr, *parsed = nullptr;
  if (OB_FAIL(unpack(task.call_, call))) {
  } else if (OB_FAIL(unpack(task.attempt_, attempt))) {
  } else if (OB_FAIL(unpack(task.target_, target))) {
  } else if (attempt.transport_error_ != OB_SUCCESS) { ret = attempt.transport_error_; }
  else if (attempt.http_code_ / 100 != 2) { ret = OB_CURL_ERROR; }
  else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, task.raw_, ObJsonInType::JSON_TREE,
                                                  ObJsonInType::JSON_TREE, response))) {
  } else if (response->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_DATA; }
  else if (call.operation_ == 1) {
    ObAIFuncIEmbed *provider = nullptr;
    if (OB_FAIL(ObAIFuncUtils::get_embed_provider(allocator, attempt.provider_, provider))) {
    } else if (OB_FAIL(provider->parse_output(allocator, static_cast<ObJsonObject *>(response), parsed))) {
    } else if (OB_FAIL(ObAIFuncUtils::validate_embeddings(allocator, parsed, 1, call.dimension_))) {
    } else {
      ObString canonical;
      if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(allocator,
            static_cast<ObJsonArray *>(parsed)->get_value(0), canonical))) {
      } else { typed.set_varchar(canonical); typed.set_collation_type(CS_TYPE_UTF8MB4_BIN); }
    }
  } else {
    ObAIFuncIComplete *provider = nullptr;
    ObString text;
    if (OB_FAIL(ObAIFuncUtils::get_complete_provider(allocator, attempt.provider_, provider))) {
    } else if (OB_FAIL(provider->parse_output(allocator, static_cast<ObJsonObject *>(response), parsed))) {
    } else if (parsed == nullptr || parsed->json_type() != ObJsonNodeType::J_STRING) { ret = OB_INVALID_DATA; }
    else {
      text = static_cast<ObJsonString *>(parsed)->get_str();
      if (target.get_data_type() == ObJsonType) {
        ObIJsonBase *document = nullptr;
        ObString canonical;
        if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, text, ObJsonInType::JSON_TREE,
                                                    ObJsonInType::JSON_TREE, document))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(allocator, document, canonical))) {
        } else { typed.set_varchar(canonical); typed.set_collation_type(CS_TYPE_UTF8MB4_BIN); }
      } else {
        ObObj source;
        source.set_varchar(text); source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        ObExprResType type;
        type.set_meta(target.get_meta_type()); type.set_accuracy(target.get_accuracy());
        // Persist a self-contained scalar, never a LOB locator created by a
        // conversion. The destination column's complete type remains in target.
        if (ob_is_text_tc(target.get_data_type())) { type.set_type(ObVarcharType); }
        ObCastCtx cast(&allocator, nullptr, CM_STRICT_MODE | CM_COLUMN_CONVERT | CM_ERROR_ON_SCALE_OVER,
                       target.get_collation_type());
        if (OB_FAIL(ObExprColumnConv::convert_skip_null_check(typed, source, type, true, cast))) {
        } else if (cast.warning_ != OB_SUCCESS) { ret = cast.warning_; }
      }
    }
  }
  return ret;
}

int stage(ObMySQLProxy &proxy, ObIAllocator &allocator, const Task &task, int parse_error, const ObObj &typed)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObString result;
  const bool retry = parse_error != OB_SUCCESS && task.intents_ <= task.budget_;
  if (OB_FAIL(pack(typed, allocator, result))) {
  } else if (OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=%d,typed_result=",
                           parse_error == OB_SUCCESS ? 4 : retry ? 3 : 5))) {
  } else if (OB_FAIL(append_hex(sql, result))) {
  } else if (OB_FAIL(sql.append_fmt(",error_code=%d,retry_count=%ld,eligible_ts=DATE_ADD(NOW(6),INTERVAL %ld SECOND),"
        "claim_token=NULL,lease_until=NULL WHERE ", parse_error,
        retry ? task.intents_ : task.retries_, retry ? MIN(60L, 1L << MIN(MAX(0L, task.intents_ - 1), 6L)) : 0))) {
  } else if (OB_FAIL(owner(sql, task, 7))) {
  } else {
    int64_t affected = 0;
    ret = proxy.write(sql.ptr(), affected);
    if (OB_SUCC(ret) && affected != 1) { ret = OB_EAGAIN; }
  }
  return ret;
}
} // anonymous namespace

int AiColumnService::start(ObMySQLProxy &proxy, ObMultiVersionSchemaService &schema_service)
{
  int ret = OB_SUCCESS;
  if (started_) { return OB_INIT_TWICE; }
  proxy_ = &proxy;
  schema_service_ = &schema_service;
  cancel_.store(false);
  if (OB_FAIL(ObServerThreadHelper::create("AiColumn", 4))) {
  } else if (OB_FAIL(ObServerThreadHelper::start())) {
    cancel_.store(true);
    ObServerThreadHelper::destroy();
  } else { started_ = true; }
  return ret;
}
void AiColumnService::stop()
{
  cancel_.store(true);
  if (started_) { ObServerThreadHelper::stop(); }
}
void AiColumnService::wait()
{
  if (started_) {
    ObServerThreadHelper::wait();
    ObServerThreadHelper::destroy();
    started_ = false;
    FLOG_INFO("AI column worker counters", "dispatch_intents", dispatch_intents.load(),
              "http_dispatches", http_dispatches.load());
  }
}
void AiColumnService::do_work()
{
  int ret = OB_SUCCESS;
  // ThreadHelper's native stack is intentionally small. SQL resolution and
  // nested execution need the same protected extension used by SMART_CALL.
  void *stack = nullptr;
  const int64_t size = STACK_PER_EXTEND;
  const int allocated = alloc_stack(size, stack);
  if (allocated != OB_SUCCESS) {
    FLOG_ERROR("cannot allocate AI worker SQL stack", "error", allocated);
    return;
  }
  auto work = [&]() -> int {
    while (!cancel_.load()) {
      const int executed = execute_one();
      const int published = publish_one();
      const int backfilled = backfill_one();
      if (executed != OB_SUCCESS && executed != OB_ITER_END && executed != OB_EAGAIN) {
        LOG_WARN("AI column task deferred", "error", executed);
      }
      if (published != OB_SUCCESS && published != OB_ITER_END && published != OB_EAGAIN) {
        LOG_WARN("AI column publication deferred", "error", published);
      }
      if (backfilled != OB_SUCCESS && backfilled != OB_ITER_END && backfilled != OB_EAGAIN) {
        LOG_WARN("AI column backfill deferred", "error", backfilled);
      }
      if (executed != OB_SUCCESS && published != OB_SUCCESS && backfilled != OB_SUCCESS) { idle(100000); }
    }
    return OB_SUCCESS;
  };
  const int executed = CALL_WITH_NEW_STACK(work(), stack, size);
  dealloc_stack(stack, size);
  if (executed != OB_SUCCESS) { FLOG_ERROR("AI worker SQL stack execution failed", "error", executed); }
}
int AiColumnService::execute_one()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiWorker");
  Task task;
  if (OB_FAIL(claim(*proxy_, allocator, task))) { return ret; }
  if (task.state_ != 7) {
    const uint64_t count = dispatch_intents.fetch_add(1, std::memory_order_relaxed) + 1;
    LOG_INFO("AI column dispatch intent", K(task.table_), K(task.column_), K(task.intents_), "process_count", count);
    PreparedCall call;
    Attempt attempt;
    ObSchemaGetterGuard guard;
    const ObAiModelSchema *model = nullptr;
    ObAiModelEndpointInfo endpoint;
    query::ObIAiEndpointResolver *resolver = server_service<query::ObIAiEndpointResolver>();
    ObArray<ObString> headers;
    ObJsonObject *body = nullptr;
    if (OB_FAIL(unpack(task.call_, call))) {
    } else if (OB_FAIL(schema_service_->get_runtime_schema_guard(guard))) {
    } else if (OB_FAIL(guard.get_ai_model_schema(static_cast<uint64_t>(task.model_id_), model))) {
    } else if (model == nullptr || (call.operation_ == 1 ? model->get_type() != EndpointType::DENSE_EMBEDDING
                                          : model->get_type() != EndpointType::COMPLETION)) { ret = OB_INVALID_ARGUMENT; }
    else if (resolver == nullptr) { ret = OB_NOT_INIT; }
    else if (OB_FAIL(resolver->resolve_by_model_name(model->get_name(), allocator, endpoint, false))) {
    } else if (OB_FAIL(ob_write_string(allocator, endpoint.get_provider(), attempt.provider_))) {
    } else if (OB_FAIL(build_request(allocator, call, *model, endpoint, headers, body))) {
    } else {
      // Claim is committed. There is no transaction or business lock here.
      ObAIFuncClient client;
      client.set_timeout_sec(60);
      bool dispatched = false;
      ret = client.send_post_raw(allocator, endpoint.get_url(), headers, body, task.raw_,
                                 attempt.http_code_, dispatched, &cancel_);
      if (dispatched) {
        const uint64_t count = http_dispatches.fetch_add(1, std::memory_order_relaxed) + 1;
        LOG_INFO("AI column HTTP dispatch", K(task.table_), K(task.column_), K(task.intents_), "process_count", count);
      }
    }
    attempt.transport_error_ = ret;
    // Never log or store endpoint credentials. Provider and transport outcome
    // suffice to parse this attempt after endpoint reconfiguration or restart.
    ObString attempt_bytes;
    ret = pack(attempt, allocator, attempt_bytes);
    if (OB_SUCC(ret)) { task.attempt_ = attempt_bytes; }
    ObSqlString sql;
    if (OB_SUCC(ret) && OB_FAIL(sql.append("UPDATE oceanbase.__all_ai_column_task SET state=7,raw_response="))) {
    } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, task.raw_))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append(",attempt_context="))) {
    } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, task.attempt_))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append(" WHERE "))) {
    } else if (OB_SUCC(ret) && OB_FAIL(owner(sql, task, 2))) {
    }
    if (OB_FAIL(ret)) { return ret; }
    int64_t affected = 0;
    // A transient database failure retains the response in memory and retries
    // persistence. It never returns to the HTTP dispatch path in this attempt.
    const int64_t until = ObTimeUtility::current_time() + 20000000;
    do {
      ret = proxy_->write(sql.ptr(), affected);
      if (OB_SUCC(ret) || cancel_.load()) { break; }
      idle(10000);
    } while (ObTimeUtility::current_time() < until);
    if (OB_FAIL(ret)) { return ret; }
    if (affected != 1) {
      // An earlier retry may have committed RAW_READY while its response was
      // lost. Confirm the persisted owner before parsing the local response.
      sql.reset();
      if (OB_FAIL(sql.append("SELECT 1 FROM oceanbase.__all_ai_column_task WHERE "))) {
      } else if (OB_FAIL(owner(sql, task, 7))) {
      } else {
        SMART_VAR(ObISQLClient::ReadResult, result) {
          if (OB_FAIL(proxy_->read(result, sql.ptr()))) {
          } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
          else { ret = result.get_result()->next(); }
        }
      }
      if (OB_FAIL(ret)) { return ret == OB_ITER_END ? OB_EAGAIN : ret; }
    }
    task.state_ = 7;
  }
  ObObj typed;
  const int parsed = parse_result(allocator, task, typed);
  if (parsed == OB_ALLOCATE_MEMORY_FAILED || parsed == OB_EAGAIN || parsed == OB_SCHEMA_EAGAIN) { return parsed; }
  return stage(*proxy_, allocator, task, parsed, typed);
}

int AiColumnService::publish_one()
{
  int ret = OB_SUCCESS;
  uint64_t table_id = OB_INVALID_ID;
  ObString request, key;
  ObArenaAllocator allocator("AiPublish");
  // This optimistic scan takes no task lock. The barrier is checked again
  // under row -> group -> task locks in the publication transaction below.
  SMART_VAR(ObISQLClient::ReadResult, result) {
    if (OB_FAIL(proxy_->read(result, "SELECT g.table_id,g.request_id,HEX(g.rowkey) AS row_key "
        "FROM oceanbase.__all_ai_column_group g WHERE g.member_count>0 AND "
        "g.member_count=(SELECT COUNT(*) FROM oceanbase.__all_ai_column_task t "
        "WHERE t.table_id=g.table_id AND t.request_id=g.request_id AND t.state IN (4,5,6)) LIMIT 1"))) {
    } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else {
      sqlclient::ObMySQLResult &r = *result.get_result();
      ObString tmp;
      if (OB_FAIL(r.next())) {
      } else if (OB_FAIL(get_id(r, "table_id", table_id))) {
      } else if (OB_FAIL(r.get_varchar("request_id", tmp))) {
      } else if (OB_FAIL(ob_write_string(allocator, tmp, request))) {
      } else { ret = get_bytes(r, "row_key", allocator, key); }
    }
  }
  const int read_close = result.close();
  result.reset();
  if (OB_SUCC(ret)) { ret = read_close; }
  if (OB_FAIL(ret)) { return ret; }
  ObMySQLTransaction tx;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  if (OB_FAIL(tx.start(proxy_))) { return ret; }
  if (OB_FAIL(transaction::tablelock::ObInnerConnectionLockUtil::lock_table(table_id,
           transaction::tablelock::ROW_EXCLUSIVE, 1000000, tx.get_connection()))) {
    if (ret == OB_TABLE_NOT_EXIST) { ret = OB_SUCCESS; }
  } else if (OB_FAIL(schema_service_->get_runtime_schema_guard(schema_guard))) {
  } else if (OB_FAIL(schema_guard.get_table_schema(table_id, table))) {
    if (ret == OB_TABLE_NOT_EXIST) { ret = OB_SUCCESS; table = nullptr; }
  } else if (table != nullptr && OB_FAIL(schema_guard.get_database_schema(table->get_database_id(), database))) {
    if (ret == OB_ERR_BAD_DATABASE) { ret = OB_SUCCESS; database = nullptr; }
  }
  if (table != nullptr && table->is_in_recyclebin()) { table = nullptr; }
  if (database != nullptr && database->is_in_recyclebin()) { database = nullptr; }
  ObSqlString group_key, qualified, predicate, sql, assignments, cas;
  int64_t member_count = 0, actual_members = 0, live_members = 0, affected = 0;
  bool row_exists = false;
  ObSEArray<uint64_t, 16> columns;
  ObSEArray<ObObj, 32> current;
  ObSQLSessionInfo *session = query::ObInnerSQLConnectionAccess::get_session(tx.get_connection());
  if (OB_SUCC(ret) && OB_FAIL(group_key.append_fmt("table_id=%lu AND request_id=", table_id))) {
  } else if (OB_SUCC(ret) && OB_FAIL(append_hex(group_key, request))) {
  }
  if (OB_SUCC(ret) && table != nullptr && database != nullptr) {
    if (OB_FAIL(append_identifier(qualified, database->get_database_name_str()))) {
    } else if (OB_FAIL(qualified.append("."))) {
    } else if (OB_FAIL(append_identifier(qualified, table->get_table_name_str()))) {
    } else if (OB_FAIL(make_rowkey_predicate(*table, key, predicate, allocator, session))) {
    } else if (OB_FAIL(sql.append("SELECT "))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
      const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(i);
      if (column == nullptr || !column->is_ai_column()) { continue; }
      if (!columns.empty()) { ret = sql.append(","); }
      if (OB_SUCC(ret) && OB_FAIL(columns.push_back(column->get_column_id()))) {
      } else if (OB_SUCC(ret) && OB_FAIL(append_identifier(sql, table->get_column_schema(column->get_ai_request_column_id())->get_column_name_str()))) {
      } else if (OB_SUCC(ret) && OB_FAIL(sql.append(","))) {
      } else if (OB_SUCC(ret)) { ret = append_identifier(sql, table->get_column_schema(column->get_ai_result_spec_column_id())->get_column_name_str()); }
    }
    if (OB_SUCC(ret) && !columns.empty()) {
      if (OB_FAIL(sql.append_fmt(" FROM %s WHERE %s FOR UPDATE", qualified.ptr(), predicate.ptr()))) {
      } else {
        SMART_VAR(ObISQLClient::ReadResult, result) {
          if (OB_FAIL(tx.read(result, sql.ptr()))) {
          } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
          else {
            const int next = result.get_result()->next();
            if (next == OB_SUCCESS) {
              row_exists = true;
              for (int64_t i = 0; OB_SUCC(ret) && i < 2 * columns.count(); ++i) {
                ObString bytes;
                ObObj value;
                const int fetched = result.get_result()->get_varchar(i, bytes);
                if (fetched == OB_SUCCESS) {
                  ObString copy;
                  if (OB_FAIL(ob_write_string(allocator, bytes, copy))) {
                  } else { value.set_varchar(copy); }
                } else if (fetched != OB_ERR_NULL_VALUE) { ret = fetched; }
                if (OB_SUCC(ret)) { ret = current.push_back(value); }
              }
            } else if (next != OB_ITER_END) { ret = next; }
          }
        }
      }
    }
  }
  sql.reset();
  if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt("SELECT member_count FROM oceanbase.__all_ai_column_group WHERE %s FOR UPDATE", group_key.ptr()))) {
  } else if (OB_SUCC(ret)) {
    SMART_VAR(ObISQLClient::ReadResult, result) {
      if (OB_FAIL(tx.read(result, sql.ptr()))) {
      } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(result.get_result()->next())) {
      } else { ret = result.get_result()->get_int("member_count", member_count); }
    }
  }
  sql.reset();
  if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt("SELECT column_id,state,generation,definition_version,HEX(row_incarnation) AS incarnation,"
        "HEX(IFNULL(typed_result,'')) AS result FROM oceanbase.__all_ai_column_task WHERE %s ORDER BY column_id FOR UPDATE", group_key.ptr()))) {
  } else if (OB_SUCC(ret)) {
    SMART_VAR(ObISQLClient::ReadResult, result) {
      if (OB_FAIL(tx.read(result, sql.ptr()))) {
      } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        sqlclient::ObMySQLResult &r = *result.get_result();
        int next = OB_SUCCESS;
        while (OB_SUCC(ret) && OB_SUCCESS == (next = r.next())) {
          uint64_t cid = 0, generation = 0;
          int64_t state = 0, definition = 0;
          ObString incarnation, result_bytes;
          if (OB_FAIL(get_id(r, "column_id", cid))) {
          } else if (OB_FAIL(r.get_int("state", state))) {
          } else if (OB_FAIL(r.get_uint("generation", generation))) {
          } else if (OB_FAIL(r.get_int("definition_version", definition))) {
          } else if (OB_FAIL(get_bytes(r, "incarnation", allocator, incarnation))) {
          } else if (OB_FAIL(get_bytes(r, "result", allocator, result_bytes))) {
          } else if (!ai_task_terminal(static_cast<AiTaskState>(state))) { ret = OB_EAGAIN; }
          else {
            ++actual_members;
            int64_t idx = -1;
            for (int64_t i = 0; i < columns.count(); ++i) { if (columns.at(i) == cid) { idx = i; } }
            const ObColumnSchemaV2 *column = table == nullptr ? nullptr : table->get_column_schema(cid);
            if (state == 6 || !row_exists || idx < 0 || column == nullptr || !column->is_ai_column()
                || current.at(2 * idx).is_null() || current.at(2 * idx + 1).is_null()
                || current.at(2 * idx).get_string() != request) { continue; }
            AiColumnSpec spec;
            if (OB_FAIL(spec.decode(current.at(2 * idx + 1).get_string()))) { break; }
            if (incarnation.length() != 16 || MEMCMP(spec.incarnation_, incarnation.ptr(), 16) != 0
                || spec.generation_ != generation || spec.definition_version_ != definition
                || column->get_ai_definition_version() != definition) { continue; }
            ObObj value;
            // NULL embedding input is an already-staged SQL NULL with no HTTP.
            if (state == 4 && !result_bytes.empty() && OB_FAIL(unpack(result_bytes, value))) { break; }
            if (live_members > 0 && OB_FAIL(assignments.append(","))) {
            } else if (OB_FAIL(append_identifier(assignments, column->get_column_name_str()))) {
            } else if (OB_FAIL(assignments.append("="))) {
            } else if (OB_FAIL(append_literal(assignments, value, allocator, session))) {
            } else if (OB_FAIL(assignments.append(","))) {
            } else if (OB_FAIL(append_identifier(assignments, table->get_column_schema(column->get_ai_state_column_id())->get_column_name_str()))) {
            } else if (OB_FAIL(assignments.append_fmt("=%d", state == 4 ? 1 : 2))) {
            } else if (OB_FAIL(cas.append(" AND "))) {
            } else if (OB_FAIL(append_identifier(cas, table->get_column_schema(column->get_ai_request_column_id())->get_column_name_str()))) {
            } else if (OB_FAIL(cas.append("="))) {
            } else if (OB_FAIL(append_hex(cas, request))) {
            } else if (OB_FAIL(cas.append(" AND "))) {
            } else if (OB_FAIL(append_identifier(cas, table->get_column_schema(column->get_ai_result_spec_column_id())->get_column_name_str()))) {
            } else if (OB_FAIL(cas.append("="))) {
            } else { ret = append_hex(cas, current.at(2 * idx + 1).get_string()); }
            ++live_members;
          }
        }
        if (OB_SUCC(ret) && next != OB_ITER_END) { ret = next; }
      }
    }
  }
  if (OB_SUCC(ret) && (actual_members != member_count || member_count <= 0)) { ret = OB_EAGAIN; }
  if (OB_SUCC(ret) && live_members > 0) {
    sql.reset();
    if (session == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(sql.append_fmt("UPDATE %s SET %s WHERE %s%s", qualified.ptr(), assignments.ptr(), predicate.ptr(), cas.ptr()))) {
    } else {
      WriteCapability capability;
      capability.kind_ = WriteKind::PUBLISH; capability.table_id_ = table_id; capability.columns_ = &columns;
      ObExecContext context(allocator);
      context.set_my_session(session);
      context.set_ai_column_internal_sql(true);
      LinkExecCtxGuard link(*session, context);
      context.ai_column_next_capability() = &capability;
      ret = tx.write(sql.ptr(), affected);
      context.ai_column_next_capability() = nullptr;
      if (OB_SUCC(ret) && affected != 1) { ret = OB_EAGAIN; }
    }
  }
  for (int i = 0; OB_SUCC(ret) && i < 2; ++i) {
    sql.reset();
    if (OB_FAIL(sql.append_fmt("DELETE FROM oceanbase.%s WHERE %s",
                              i == 0 ? "__all_ai_column_task" : "__all_ai_column_group", group_key.ptr()))) {
    } else { ret = tx.write(sql.ptr(), affected); }
  }
  const int commit = tx.end(OB_SUCC(ret));
  if (OB_SUCC(ret)) { ret = commit; }
  return ret;
}

int AiColumnService::backfill_one()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiBackfill");
  uint64_t tid = OB_INVALID_ID;
  int64_t ddl = 0, affected = 0;
  ObString token, cursor, last_key;
  ObSqlString control, sql;
  SMART_VAR(ObISQLClient::ReadResult, result) {
    if (OB_FAIL(proxy_->read(result, "SELECT table_id,ddl_schema_version FROM oceanbase.__all_ai_column_control "
        "WHERE kind=1 AND state=0 AND (lease_until IS NULL OR lease_until<=NOW(6)) "
        "AND eligible_ts<=NOW(6) ORDER BY eligible_ts,table_id LIMIT 1"))) {
    } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(result.get_result()->next())) {
    } else if (OB_FAIL(get_id(*result.get_result(), "table_id", tid))) {
    } else { ret = result.get_result()->get_int("ddl_schema_version", ddl); }
  }
  const int read_close = result.close();
  result.reset();
  if (OB_SUCC(ret)) { ret = read_close; }
  if (OB_FAIL(ret)) { return ret; }
  if (OB_FAIL(new_token(allocator, token))) {
  } else if (OB_FAIL(control.append_fmt("table_id=%lu AND ddl_schema_version=%ld AND kind=1 AND state=0", tid, ddl))) {
  } else if (OB_FAIL(sql.append("UPDATE oceanbase.__all_ai_column_control SET claim_token="))) {
  } else if (OB_FAIL(append_hex(sql, token))) {
  } else if (OB_FAIL(sql.append_fmt(",lease_until=DATE_ADD(NOW(6),INTERVAL 90 SECOND) WHERE %s "
                                  "AND (lease_until IS NULL OR lease_until<=NOW(6))", control.ptr()))) {
  } else if (OB_FAIL(proxy_->write(sql.ptr(), affected))) {
  } else if (affected == 0) { ret = OB_EAGAIN; }
  if (OB_FAIL(ret)) { return ret; }
  ObMySQLTransaction tx;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  ObSEArray<uint64_t, 8> columns;
  ObSqlString qualified, assignments, missing, order;
  int64_t controlled_members = 0;
  if (OB_FAIL(tx.start(proxy_))) { return ret; }
  if (OB_FAIL(transaction::tablelock::ObInnerConnectionLockUtil::lock_table(tid,
              transaction::tablelock::ROW_EXCLUSIVE, 1000000, tx.get_connection()))) {
    if (ret == OB_TABLE_NOT_EXIST) { ret = OB_SUCCESS; }
  } else if (OB_FAIL(schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(tid, table))) {
    if (ret == OB_TABLE_NOT_EXIST) { ret = OB_SUCCESS; table = nullptr; }
  } else if (table != nullptr && OB_FAIL(guard.get_database_schema(table->get_database_id(), database))) {
    if (ret == OB_ERR_BAD_DATABASE) { ret = OB_SUCCESS; database = nullptr; }
  }
  if (table != nullptr && table->is_in_recyclebin()) { table = nullptr; }
  if (database != nullptr && database->is_in_recyclebin()) { database = nullptr; }
  sql.reset();
  if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt("SELECT column_id,definition_version,HEX(IFNULL(cursor_rowkey,'')) AS cursor_key "
         "FROM oceanbase.__all_ai_column_control WHERE %s AND claim_token=", control.ptr()))) {
  } else if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, token))) {
  } else if (OB_SUCC(ret)) {
    SMART_VAR(ObISQLClient::ReadResult, result) {
      if (OB_FAIL(tx.read(result, sql.ptr()))) {
      } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        int next = OB_SUCCESS;
        while (OB_SUCC(ret) && OB_SUCCESS == (next = result.get_result()->next())) {
          uint64_t cid = 0;
          int64_t definition = 0;
          ObString progress;
          if (OB_FAIL(get_id(*result.get_result(), "column_id", cid))) {
          } else if (OB_FAIL(result.get_result()->get_int("definition_version", definition))) {
          } else if (OB_FAIL(get_bytes(*result.get_result(), "cursor_key", allocator, progress))) {
          } else if (controlled_members > 0 && cursor != progress) { ret = OB_INVALID_DATA; }
          else {
            cursor = progress;
            ++controlled_members;
            const ObColumnSchemaV2 *column = table == nullptr ? nullptr : table->get_column_schema(cid);
            if (column == nullptr || !column->is_ai_column() || column->get_ai_definition_version() != definition) { continue; }
            if (!columns.empty() && OB_FAIL(assignments.append(","))) {
            } else if (!columns.empty() && OB_FAIL(missing.append(" OR "))) {
            } else if (OB_FAIL(append_identifier(assignments, column->get_column_name_str()))) {
            } else if (OB_FAIL(assignments.append("="))) {
            } else if (OB_FAIL(append_identifier(assignments, column->get_column_name_str()))) {
            } else if (OB_FAIL(append_identifier(missing, table->get_column_schema(column->get_ai_result_spec_column_id())->get_column_name_str()))) {
            } else if (OB_FAIL(missing.append(" IS NULL"))) {
            } else { ret = columns.push_back(cid); }
          }
        }
        if (OB_SUCC(ret) && next != OB_ITER_END) { ret = next; }
      }
    }
  }
  bool complete = columns.empty() || table == nullptr || database == nullptr;
  if (OB_SUCC(ret) && !complete) {
    if (OB_FAIL(append_identifier(qualified, database->get_database_name_str()))) {
    } else if (OB_FAIL(qualified.append("."))) {
    } else if (OB_FAIL(append_identifier(qualified, table->get_table_name_str()))) {
    }
    sql.reset();
    ObSQLSessionInfo *session = query::ObInnerSQLConnectionAccess::get_session(tx.get_connection());
    if (session == nullptr) { ret = OB_ERR_UNEXPECTED; }
    if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt("UPDATE %s SET %s WHERE (%s)",
                                           qualified.ptr(), assignments.ptr(), missing.ptr()))) {
    } else if (OB_SUCC(ret) && !cursor.empty()) {
      if (OB_FAIL(sql.append(" AND "))) {
      } else { ret = make_rowkey_after(*table, cursor, sql, allocator, session); }
    }
    for (int64_t k = 0; OB_SUCC(ret) && k < table->get_rowkey_column_num(); ++k) {
      if (k > 0 && OB_FAIL(order.append(","))) {
      } else {
        const auto *key_column = table->get_column_schema(table->get_rowkey_info().get_column(k)->column_id_);
        if (key_column == nullptr) { ret = OB_SCHEMA_EAGAIN; }
        else { ret = append_identifier(order, key_column->get_column_name_str()); }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(" ORDER BY %s LIMIT 64", order.ptr()))) {
    } else if (OB_SUCC(ret)) {
      WriteCapability capability;
      capability.kind_ = WriteKind::BACKFILL; capability.table_id_ = tid; capability.columns_ = &columns;
      capability.progress_allocator_ = &allocator;
      capability.last_rowkey_ = &last_key;
      ObExecContext context(allocator);
      context.set_my_session(session);
      context.set_ai_column_internal_sql(true);
      LinkExecCtxGuard link(*session, context);
      context.ai_column_next_capability() = &capability;
      ret = tx.write(sql.ptr(), affected);
      context.ai_column_next_capability() = nullptr;
      complete = OB_SUCC(ret) && affected == 0 && cursor.empty();
      if (OB_SUCC(ret) && affected > 0 && last_key.empty()) { ret = OB_ERR_UNEXPECTED; }
    }
  }
  // Exhausting a nonempty cursor restarts from the beginning for an omission
  // check. Only an empty full pass completes this ADD. Progress and row writes
  // commit together; a crash or expired owner cannot advance the cursor alone.
  sql.reset();
  const bool discarded = table == nullptr || database == nullptr;
  if (OB_SUCC(ret) && discarded) {
    ret = sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_control WHERE %s AND claim_token=", control.ptr());
  } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_control SET state=%d,"
          "cursor_rowkey=", complete ? 1 : 0))) {
  } else if (OB_SUCC(ret) && !last_key.empty()) { ret = append_hex(sql, last_key); }
  else if (OB_SUCC(ret)) { ret = sql.append("NULL"); }
  if (OB_SUCC(ret) && !discarded) {
    ret = sql.append_fmt(",claim_token=NULL,lease_until=NULL WHERE %s AND claim_token=", control.ptr());
  }
  if (OB_SUCC(ret) && OB_FAIL(append_hex(sql, token))) {
  } else if (OB_SUCC(ret) && OB_FAIL(sql.append(" AND lease_until>NOW(6)"))) {
  } else if (OB_SUCC(ret) && OB_FAIL(tx.write(sql.ptr(), affected))) {
  } else if (OB_SUCC(ret) && affected != controlled_members) { ret = OB_EAGAIN; }
  const int commit = tx.end(OB_SUCC(ret));
  if (OB_SUCC(ret)) { ret = commit; }
  return ret;
}
} } // namespace oceanbase::observer

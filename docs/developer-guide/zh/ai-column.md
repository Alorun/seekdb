# 异步 AI COLUMN：实现记录

## 范围与仓库调查

仅 MySQL、`sync_mode='async'`，不支持 AI 列之间的依赖。题目实际列出五个功能模块，共 50 分；重复的最后一个标题按向量列与索引维护处理。

当前分支 `async-ai-column`。开始时存在文件 SQL 功能的未提交修改，本实现保留这些修改。没有发现已有 AI COLUMN 实现。

| 链路 | 已核对的代码及复用点 |
| --- | --- |
| 同步推理 | `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.cpp`、`ob_expr_ai_embed.cpp`：参数类型推导；`ob_ai_func_utils.cpp`：模型、provider、请求及响应；`ob_ai_func_client.cpp`：curl、超时及 HTTP 重试 |
| 模型与端点 | `src/share/schema/ob_ai_model_mgr.h`；`src/observer/ai_service/ob_ai_service_executor.cpp`、`ob_ai_service_proxy.cpp`；query 接口在 `src/query/api/query/ai/` |
| DDL | `src/sql/parser/sql_parser_mysql_mode.y`；`src/sql/resolver/ddl/ob_ddl_resolver.cpp`、`ob_create_table_resolver.cpp`、`ob_alter_table_resolver.cpp`；Rootserver 的 `src/rootserver/ob_ddl_service.cpp` |
| Schema | `src/share/schema/ob_column_schema.{h,cpp}`、`ob_table_sql_service.cpp`；读取是 `src/observer/schema/ob_schema_retrieve_utils.ipp`；内表源定义是 `src/share/inner_table/ob_inner_table_schema_def.py`；SHOW CREATE 是 `src/sql/printer/ob_schema_printer.cpp` |
| 写入与事务 | `src/sql/engine/dml/ob_dml_service.cpp` 的公共行处理入口；物理写入经 DAS；提交、回滚入口是 `src/sql/ob_sql_trans_control.cpp`，不能把提交回调当成任务存储 |
| 后台机制 | `src/observer/dbms_scheduler/ob_dbms_sched_service.cpp` 的生命周期及任务扫描；`ob_dbms_sched_table_operator.cpp` 的条件领取；`src/rootserver/ddl_task/ob_ddl_task.cpp` 的持久化记录、`SELECT FOR UPDATE` 及执行版本；还没有可直接使用的 AI 任务租约和分组发布机制 |
| 引用与会话 | `src/share/system_variable/gen_ob_sys_variables.py`；`src/sql/resolver/dml/ob_dml_stmt.h` 的 `get_relation_exprs()`、列引用；必须递归进入子查询、连接、SET 右侧和条件 |
| 向量 | `src/observer/vector_index/ob_vector_index_util.cpp` 的 VECTOR 维度；`src/sql/das/ob_das_dml_vec_iter.cpp` 及现有 DML 索引路径；`ob_plugin_vector_index_adaptor.cpp` 的 SCN／位图可见性 |

五组比赛 SQL 和 Mock 实际在 `tools/deploy/mysql_test/test_suite/ai_column/`，不在题面的 `ai_function/`。端口分别为 19120（DDL 不启动 Mock）、19121（DML）、19122（worker）、19124（retry/strict）、19123（vector）。开始时题面的三个 C++ 测试以及整个 `tools/ai_gateway/` 均不存在。现有 SQL 没有 MODIFY 或 SHOW AI COLUMNS 用例。

## 整体设计及事务不变量

以下运行期设计用于后续阶段，尚不代表这些机制已经实现。

1. AI 列是可索引的真实存储结果列，不是同步 generated column，也不通过 SELECT 包装隐藏旧值。默认值为 NULL。状态独立于结果，不能用 NULL 判断 READY。
2. T1：业务行的变化、AI 结果实际置 NULL、隐藏元数据更新、旧任务失效、新任务和任务组创建，共用原业务事务。失败时回滚全部变化；不在 T1 发 HTTP。不相关列更新不创建任务。INSERT SELECT、批量 INSERT、UPDATE、重复键更新和其他 SQL 写入最终必须经过同一维护入口。
3. 后台只用独立事务读取已提交的任务。领取使用持久化条件更新／行锁及全新所有权令牌，提交后才调用模型。数据库重启后通过扫描恢复，不依赖内存队列或提交回调。
4. T2 领取事务、无事务的模型调用、T3 暂存事务、T4 发布事务相互分离。模型调用不持有业务行锁。结果暂存必须再次检查任务所有权；过期执行者不能修改暂存结果。
5. T4 在同一个事务中锁定业务行、核对组及每个当前成员、写结果和状态、维护索引并清理任务。统一按业务行、组、列的顺序加锁；不能在事务外检查版本后无条件 UPDATE。
6. 同一行同次变更触发的列共享组请求 ID。各列可独立推理和暂存；全部成员到终态后一次发布。成功列写结果及 READY，最终失败列写 NULL 及 FAILED。其他组和未触发列保持其各自版本。
7. 持久化定位包括：表／列 ID、带类型的行键、行插入身份、每列生成版本、组请求 UUID、定义版本、任务所有权令牌、租约截止、重试计数／下次执行时间、暂存值及错误。`rowkey_predicate` 仅作兼容测试和诊断展示，不能替代身份及版本校验。
8. 行插入身份在 DELETE 后失效；同主键重新插入分配新身份。主键变化更新定位并重新生成。无论新一代是 READY 还是 PENDING，旧请求都不能命中新一代的请求 UUID／生成版本。定义变化、删除及 ID 重映射也必须经过同样的校验。
9. 手动 RETRY 只推进指定列命中行的生成版本。被替代的旧成员成为 SUPERSEDED 终态，旧组只可发布仍归属于它的成员；不能覆盖新的重试组，也不能让旧组永久等待已被替代的成员。
10. SQL 写保护需要不可由用户设置的内核发布上下文；不能使用用户可 SET 的 session 开关绕过保护。

行状态：`PENDING → READY / FAILED`，依赖变化或 RETRY 可从任意状态回到新的 PENDING 版本。

任务状态：`QUEUED → RUNNING → STAGED_SUCCESS / STAGED_FAILED`；可重试错误进入带退避时间的 `RETRY_WAIT`；租约过期后更换所有权令牌重新领取。删除或新版本替代产生 SUPERSEDED。终态组发布或取消后清理任务。

当前 HTTP 客户端自身也会重试；后续 worker 必须明确两层重试预算，逐次记录真实 dispatch，不能把一次任务执行等同一次模型请求。崩溃可能发生在远端已接收而本地未记录之间，本地状态不能保证外部 exactly-once。压测的实际请求数以 Mock／评测端点观测为准；只有端点协议明确支持时才发送幂等键。

Strict Read 必须在语句开始写入之前，对实际引用的各 AI 列做全列状态检查，且检查与数据访问使用同一语句快照。不能在最终输出行上检查，也不能扫描所有未引用的 AI 列。UPDATE／DELETE 使用较新锁定读版本时，要遵循引擎的冲突重试规则，不能把另一个内层 SQL 连接的快照当成外层语句快照。该接入和并发验证留在阶段 4。

RETRY 条件走 Parser、名称绑定和表达式执行：只有未加反引号的 `status` 标识符绑定到目标列状态，反引号的业务字段仍正常绑定，不做 SQL 字符串替换。

向量失效及发布都走现有事务 DML 的旧值／新值索引维护，验证目标是旧值在同一事务内退出 ANN、回滚后恢复、提交后新值与索引同步可见。

## 阶段 1 已接入的代码

- 新 Parser 节点保存表达式和选项 AST，复用原 CREATE／ALTER ADD 列语法；不改写为同步 generated column。
- `ObColumnSchemaV2` 保存原表达式、规范执行表达式、依赖 ID、重试配置、定义版本及三个隐藏列 ID。补充 reset、深拷贝、内存大小计算、RPC 序列化／反序列化和列 ID 重映射。
- `__all_column` 和其历史表保存这些字段。`__all_ai_column` 是同源只读视图，列／表 DDL 不需要另维护一份定义记录；过滤已隐藏／删除的列。
- 完整表列名和类型存在后再绑定 CREATE 的表达式，能识别前向 AI 依赖并拒绝。ALTER ADD 在 Resolver 校验，Rootserver 分配最终隐藏列 ID，并用已有 DDL 事务写结果列及隐藏列。
- 仅允许外层 AI_COMPLETE／AI_EMBED，拒绝嵌套 AI、非法模型参数、非纯输入、未知／隐藏／AI／generated 依赖、非法选项、非空约束／主键／显式默认值。实际 Resolver 有 SchemaGuard 时校验模型存在及模型类型；不请求 Endpoint。
- AI_EMBED 要求 VECTOR，校验显式维度，省略时将目标维度写入规范表达式。AI_COMPLETE 校验文本、JSON、数值目标类型。类型提示保存到规范表达式的内部 JSON 配置，保留原业务输入；后续阶段负责消费该配置并进行严格结果校验，提示本身不是校验。
- SHOW CREATE 输出原 AI 定义和显式 async 配置。依赖列的 DROP／MODIFY／重命名目前拒绝，避免表达式失去绑定；MODIFY／CHANGE AI 定义目前明确拒绝，待定义变化的任务失效事务实现后开放。SHOW AI COLUMNS 目前未接入，元数据查询可用 `__all_ai_column`。

阶段 1 不提供任务生成、worker、结果发布、RETRY、Strict Read 或 ANN 维护保证；这些能力按阶段 2～5实现，不能将当前代码作为完整比赛实现。

## 验证与后续顺序

新增测试位于题面指定位置：

- `unittest/sql/parser/test_ai_column_parser.cpp`：AST 保留、CREATE／ALTER 语法、普通 SQL、非法声明。
- `unittest/share/schema/test_ai_column_schema.cpp`：深拷贝、序列化往返、ID 重映射、损坏依赖编码。
- `unittest/sql/resolver/test_ai_column_resolver.cpp`：实际表达式绑定、去重依赖、函数／类型／维度限制、隐藏列。

正常完整构建后运行：

```sh
./build.sh release
make -C build_release -j2 seekdb sql_tests share_tests
GTEST_FILTER='AiColumn*' ./build_release/unittest/run_tests.sh \
  -R '^(sql|share)_tests_shard_[0-9]+$' --output-on-failure
```

本次先以生产编译参数在 `/tmp/seekdb-ai-column-units/` 单独重编新增测试及相应 Parser／Schema／Resolver 生产对象，链接已有运行库验证局部代码；这不是完整重编译后的服务器集成验证。本地实际命令为 `python3 /tmp/seekdb-ai-unit.py parser`、`python3 /tmp/seekdb-ai-unit.py schema`、`python3 /tmp/seekdb-ai-unit.py resolver`。Parser 3、Schema 3、Resolver 6 个用例均通过。DDL Resolver、CREATE／ALTER Resolver 和 Rootserver 做了生产参数编译检查；Schema SQL 写入、读取及 SHOW CREATE 所在的生产 Unity 单元做了编译检查。内表生成器及构建源清单生成器也通过。共享 `build_release` 当时另有文件 SQL 构建和运行实例，未终止或替换它们。现有 DDL mysqltest 未执行，其他四组 SQL、恢复／故障注入和性能测试也尚未执行。

SQL 集成验证必须使用完整重编后的隔离实例：

```sh
cd tools/deploy
/path/to/mysqltest --host=127.0.0.1 --port=PORT --user=root \
  --test-file=mysql_test/test_suite/ai_column/t/ai_column_ddl.test \
  --result-file=mysql_test/test_suite/ai_column/r/mysql/ai_column_ddl.result
```

阶段 2 先完成所有 DML 写入口的保护、实际失效及同事务入队；阶段 3 完成模型系统提示消费、类型转换／JSON／向量检查、持久化领取、暂存和组发布；阶段 4 加入有限重试、SQL 条件 RETRY 和同快照 Strict Read；阶段 5 用屏障及故障注入验证向量、重启、所有权过期与乱序，并测业务写入延迟、提交到可见延迟、完成吞吐及实际 HTTP 请求数。

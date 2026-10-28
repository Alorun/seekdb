# 异步 AI COLUMN：代码依据与实施决策

## 0. 阅读约定、代码基线与本轮范围

调查日期：2026-10-27。本轮只更新本文，不实现运行期功能，不修改竞赛 SQL／结果，不解决其他工作的合并冲突，不停止共享构建或数据库。

本文用 **题面**、**测试**、**代码**、**决策**、**待验**区分要求来源。下文所有新 `AiColumn*` 类型、方法和表均是拟新增接口，不能当作仓库已有 API；其他类／函数名已经阅读实现和调用方。代码定位采用 `452be98ed` 的行号；公共链路在 `5bce9e06d` 中也存在。AI 函数增强随后合入 `c899c2054`，相关行号及函数体已变化，下一轮应按函数名定位，保留增强实现。

调查起点是 `async-ai-column`、HEAD `452be98ed`，存在七个文件 SQL 工作区修改。调查期间共享工作区先变为 `2026-competition`／`990771625`（七个修改已入该提交），再短暂回到 `5bce9e06d`，最终按用户指定在 **`seekDB-basic-questions`** 更新本文。没有执行 checkout、reset、stash、cherry-pick 或冲突解决。写文档初稿时 HEAD 为 `990771625`，AI 阶段 1 仍在；当时另有 AI 函数增强的暂存修改及四个 AA 冲突：`unittest/BUILD.bazel`、`unittest/CMakeLists.txt`、`unittest/all_tests_main.cpp`、`unittest/sql/BUILD.bazel`。这些都不是本轮改动。最终复核时用户已完成合入，HEAD 为 **`c899c2054`**，冲突已消失，工作区仅本文有本轮修改。

### 已核实的实际进度

| 能力 | 当前事实及关键位置（452be98ed） |
| --- | --- |
| AST | `src/sql/parser/sql_parser_mysql_mode.y:6146` 起保存 AI 表达式及选项；不是 generated column 的执行改写 |
| Resolver | `src/sql/resolver/ddl/ob_ddl_resolver.cpp:2066` `resolve_ai_column_attribute`；`:2111` `resolve_ai_column_expr`；依赖绑定、纯表达式、函数／模型类型、配置和 VECTOR 维度校验已有 |
| 列 Schema | `src/share/schema/ob_column_schema.h:150`、`:422`；表达式、规范表达式、依赖 ID、重试、定义版本、三个隐藏列 ID；cpp 有 reset／assign／深拷贝／序列化／ID 重映射 |
| 系统元数据 | `src/share/inner_table/ob_inner_table_schema_def.py:346` 的 column／history 字段；`:13839` `__all_ai_column` 是同源只读视图，**不是任务表**；写入 `src/share/schema/ob_table_sql_service.cpp:3214`，读取 `src/observer/schema/ob_schema_retrieve_utils.ipp:1039` |
| CREATE／ADD | `ob_create_table_resolver.cpp:954` 完整列集合后绑定；`ob_alter_table_resolver.cpp:3429`；`src/rootserver/ob_ddl_service.cpp:7779`、`:7835` 分配并保存隐藏列 |
| 隐藏列 | `ob_ddl_resolver.cpp:2291` `add_ai_column_hidden_columns`：每个 AI 列各一个 nullable TINYINT state、VARCHAR(128) binary request、VARCHAR(256) binary result_spec；目前没有运行期初始化、编码或状态契约 |
| 展示及限制 | `src/sql/printer/ob_schema_printer.cpp:211` SHOW CREATE；`ob_alter_table_resolver.cpp:3540` 明确拒绝 MODIFY／CHANGE AI，`:4371` 保护依赖重命名；`ob_ddl_resolver.cpp:2274` 依赖保护 |
| 尚未实现 | 同事务失效／入队、持久化任务表、worker、类型化暂存和发布、RETRY、strict 变量／检查、SHOW AI COLUMNS、ADD 旧行回填、AI 专属 ANN 验证 |

与上一轮记录的功能进度基本一致；差异是已有实现已提交、当前分支已变化、共享 AI 函数增强已在调查期间合入。规范表达式中的 `__seekdb_ai_column_result_prompt` 目前仅被存储，尚未有 worker 消费；不能据此声称系统提示已发送。旧文档“没有 AI 实现”“题面三个 C++ 文件不存在”描述的是阶段 1 开始前，现已不适用于当前代码。

## 1. 精确要求、测试约定与缺失信息

题面实际有五个模块，共 50 分；最后重复标题按 AI 向量列与索引维护理解。仅 MySQL、async、不支持 AI 列链式依赖。业务事务不能发送模型请求；结果是存储列，PENDING／FAILED 实际为 SQL NULL，状态独立保存。

真实测试目录是 `tools/deploy/mysql_test/test_suite/ai_column/`，不是题面 `ai_function/`。五个 `.test`、对应 `r/mysql/*.result` 及 `include/ai_column_mock.py` 均存在；`tools/ai_gateway/README.md` 和 `test_gateway.py` 仍不存在。

| 项目 | 题面／测试／代码的区别与本次决策 |
| --- | --- |
| 语法与默认值 | 题面示例有 WITH，两项配置；当前 AST 允许省略选项。代码默认 `max_retries=2`，接受 0..INT32_MAX，选项大小写不敏感，拒绝重复／未知项及非 async（`ob_ddl_resolver.cpp:2077`）。题面、现有 SQL 没有规定缺省值；保留现有默认 async、2，不宣称是题面要求 |
| 重试预算 | 决策：`max_retries` 是初次调用之外的最多额外 dispatch 次数；0 只允许一次。HTTP 客户端原有最多 3 次额外重试（client.cpp:32、:148），异步 worker 必须单独关闭这层重试，普通 AI 函数默认不变 |
| HTTP 400 | Mock `include/ai_column_mock.py:96` 起将 retry-once 首次返回 **400**、下一次成功；`:211` 验证两次调用。不能只重试 429／5xx。决策：模型调用和结果校验错误均按有限预算重试；永久失效的定义／行直接取消，不发 HTTP |
| 可观察任务 | `ai_column_t1_dml.test` 在业务事务内查询任务，要求立即 `eligible_ts <= now(6)`、最终主键的 `rowkey_predicate`；`ai_column_worker.test:42` 要求同次变更两个成员共享 request_id；`:101` 要求第一次失败后 `state=3,retry_count=1`；完成后任务行清理。必须兼容这些字段，不能只做内存队列 |
| 隐藏状态 | 题面没有数值编码；决策为 0=PENDING、1=READY、2=FAILED，NULL 的遗留／ADD 行按 PENDING。READY 的 JSON literal null 是合法 JSON 值，不能混同 SQL NULL 或 FAILED |
| 条件 RETRY | 题面给出 WHERE；测试还要求不带 WHERE、READY 行重试、零行无动作、只改一个 AI 列、普通目标列错误 1235、与其他 ALTER 动作混用错误 1235；不强制隐式提交 |
| strict | 默认 OFF；测试要求错误 11120/HY000，消息含 `table_id=..., column_id=...`，覆盖 SELECT、UPDATE、DELETE、子查询和 WHERE 过滤外的未就绪行。`src/share/ob_errno.def:1884` 起已有 AI 错误但没有 11120；需要新增及生成 |
| MODIFY／SHOW | 现有五组 SQL 没有 MODIFY 或 SHOW AI COLUMNS 用例，题面没有后者的完整语法／结果字段。决策：完整首版继续明确拒绝 AI 定义 MODIFY／CHANGE，支持 CREATE／ADD／DROP／SHOW CREATE；另实现 `SHOW AI COLUMNS FROM [db.]tbl [LIKE ... | WHERE ...]`，列为 Field、Type、Expression、Dependencies、Sync_mode、Max_retries、Definition_version。此 SHOW 形式是推荐接口，不能伪称测试已规定 |
| 并发／限流 | AI COLUMN 题面未给并发或 QPS 选项。现有 endpoint 结构／`ObAiService` 无通用并发限流器；其他套件 `ai_function/t/ai_model_endpoint_ddl.test:45,85` 期待 max_concurrency，但当前解析结构不支持，AI 增强文档也报告该差异。首版使用有界 worker 池，不新增 AI COLUMN 选项，不假装该测试已通过 |
| Mock 能力 | typed-prompts 验证 system hint、用户输入保持原样、鉴权；slow-old 有约 1.2 秒延迟。事件在响应构造后记录，不足以精确覆盖请求中断时的远端实际接收次数；现有用例不是可控崩溃屏障，也没有恢复／性能证据 |

### 必须维持的运行期不变量

- 一个业务语句产生的结果失效、隐藏字段、旧成员失效、新组和任务，都在外层事务和外层语句保存点内；没有有效任务则业务变化也不能提交。
- 每行每 AI 列只有一个当前 `(incarnation,generation,request_id,definition_version)`；主键仅是定位条件。
- 推理无业务行锁、无打开的数据库事务。领取、响应暂存、发布分别是短事务。
- 同组全部原始成员达到终态后，一次事务发布仍归属它的全部成员；成功 READY，最终失败 NULL/FAILED。
- worker 只扫描持久化已提交记录；提交唤醒、内存队列都只是加速。取消或重复执行不能产生错误发布。
- strict 以同一语句快照中整列的状态为准；DML 修改前失败；检查不能被优化器裁剪、过滤条件或计划缓存绕过。
- AI 向量失效和发布必须进入索引 DML，不能只在 SELECT 输出层掩盖旧值。

## 决策总表

| 问题 | 最终选择 | 接入点／依据 | 限制或成本 |
| --- | --- | --- | --- |
| 同事务任务写入 | external-session nested SQL＋语句结束前 finish | ResultSet.close/end_stmt、FK 内层 SQL／外层保存点；§2.2 | 新增 AI nested 标记和可溢写收集器，不能只调用 SQL proxy |
| 批量／冲突候选 | 最终 token 核对后只入队存活成员 | INSERT/IODKU/REPLACE/DAS IGNORE 实现；§2.1 | 每存活行一次同事务点读，可批量优化 |
| 行身份／版本 | 既有三隐藏列复用；incarnation＋逐列 generation＋request＋def | ColumnSchema 隐藏 ID、UUID helper；§3 | 新二进制 spec codec，遗留行需回填 |
| 部分组替代 | SUPERSEDED 算屏障终态，剩余成员继续并一起发布 | 不重调未受影响列和单次触发原子性；§4 | 题面未明说该交错，已选定此解释 |
| 输入快照 | T1 只求普通参数并保存类型化参数包 | Expr.eval、generated 表达式固化环境；§5 | 参数持久化带来写放大，禁止根 AI eval |
| 推理复用 | 原 provider/client，新增 call options 与 transport/parse 分拆 | ObAIFuncModel／已合入增强；§5 | 默认普通 AI 行为保持；异步关闭隐藏重试 |
| 领取／恢复 | 独立 RC 短事务、租约 token CAS、有界 ThreadHelper 池 | scheduler 生命周期和条件领取实例；§6 | 没有可拿来即用的 AI worker，需新 service/表操作器 |
| 暂存／发布 | RAW_READY→typed STAGED；业务行锁内原子 CAS 和正常 UPDATE | SQL/DAS同 tx_desc；§6流程C | 远端响应未落盘就崩溃仍可能重复调用 |
| RETRY | 专用 AST 绑定，降低为 UPDATE，不走元数据 DDL提交 | lexer quoted 位、UpdateResolver；§7 | 新内部能力；混合 ALTER 1235 |
| strict | 优化前 refs，执行前 direct DAS整列扫描同 S | ResultSet.start_stmt 后、DAS snapshot、MVCC冲突；§7 | 首版整表扫描成本，PS每次执行仍检查 |
| DDL并发 | 所有业务写表先 table intent，再核对编译AI元数据；DDL排他 | SqlTransControl.lock_table＋DDLLock；§8 | 普通DML多一次schema缓存核对／表intent；不能只信旧计划 |
| 旧库／ADD | 新安装器＋DDL同事务control，分批回填 | Bootstrap/DDLOperator真实事务接口；§8 | 安装器是新增代码，旧库必须单独smoke |
| ANN | 正常索引DML＋query-local MVCC可见性 | related CtDef／delta D-I／SCN位图；§8 | 插件内存修改本身不提供回滚保证，必须动态验证 |

## 2. DML：接入点与同事务契约

### 2.1 为什么不能只改一个 process 函数

| 入口／代码证据 | 接入决策及陷阱 |
| --- | --- |
| `src/sql/engine/dml/ob_dml_service.cpp:598` process_insert、`:740` process_update | BEFORE trigger 后取得最终普通列值；update 的 null／changed／constraint 检查在 :796 之后。新增 `AiColumnDmlService::prepare_row` 在此阶段计算输入和准备隐藏值，**不能**执行 AI 根函数或写任务。普通行值／AI 赋值槽必须已在 Resolver／CG 中加入 |
| `ob_table_insert_op.cpp:220` | process_insert 后 :225 才 `set_heap_table_hidden_pk`，:228 才 insert；不能在 process_insert 把尚未生成的隐藏主键保存为任务键。INSERT SELECT、多行 VALUES 都经 child 拉行／该算子 |
| `ob_table_update_op.cpp:327`、`ob_table_delete_op.cpp:244`；DMLService update :999 | UPDATE 共用该链；主键变更 :1042 分成 DELETE／INSERT；无变化 :1020 只锁行。AI 依赖和主键标识由 CtDef 编译保存，新旧 StoredRow 保留类型和最终键 |
| `ob_table_insert_up_op.cpp:350` 起 | 重复键候选在 conflict checker 内多次合并；:404 process_update、:405 补 heap key；:1139 do_update 最后可走低层 delete／insert。不能按最初 INSERT 候选就入队 |
| `ob_table_replace_op.cpp:386` final_insert_row_to_das | 有直接写路径；批量路径 :669 先存行，:706 才 process_insert，必须在 AI prepare 后刷新 StoredRow；重复行删除经 :687。REPLACE 是新 incarnation，不延续被删除行身份 |
| `src/sql/das/ob_das_insert_op.cpp:130`、`:282` | INSERT IGNORE／唯一键重复可能在 DAS 转为成功并丢弃候选；现有 after-row 节点没有逐行 accepted 标记。aggregate affected_rows 或 aggregate is_duplicated 不能用来判断每行任务 |
| `ob_dml_service.cpp:1706` write_row_to_das_op | :1763 把行存入 DAS 缓冲；此时不是最终落盘成功。新增候选快照必须在最终行物化时深拷贝，不能保存 Expr／Datum 指针 |
| `ob_table_modify_op.cpp:1051` submit_all_dml_task | :1057 execute、:1058 close、:1059 after-row、:1062 reuse，已有外键／触发器使用此边界（DMLService :2126）。候选记录必须活到语句结束，不能随着约 6MB 的 DAS 批次复用丢失 |
| `src/sql/ob_result_set.cpp:678` do_close_plan | :701 exec_result close、:710 executor close、:728 end_stmt。**最终决策**：在成功执行并关闭所有写算子后、end_stmt 前调用新 `AiColumnStatementService::finish(ctx)`，入队失败转外层语句失败／回滚；失败语句丢弃收集器 |

收集器属于 `ObExecContext` 的**一次语句执行尝试**，使用可溢写 row store 限制批量内存；记录输入、typed old/new key、旧／新 token、逻辑操作，不是提交后可靠任务源。算子最终 StoredRow 物化处记录候选；IODKU／REPLACE 所有特殊写路径都接入。`finish` 在外层事务中点读最终业务行，只有当前隐藏 token 等于候选新 token 的存活代际入队；被 IGNORE、后续候选覆盖、临时插入后删除的候选不生成有效任务。每个受影响 `(incarnation,column_id)` 只处理最终状态；旧 incarnation 被确实删除时清理其任务。

多次候选变化可能使中间组成员丢失：收集器必须按最终业务行里的 **每列 request_id** 归并存活成员；一个成员已被后续候选替代时不能仍计入组 member_count。输入和 token 在 prepare 时一次生成并随 StoredRow 复制，finish 不再读正文重算。触发器／外键级联的嵌套语句在自己的外层保存点内 finish，重入深度和收集器分离；内部任务表写不启用收集器。

**依赖判定**：CtDef 存依赖 ID 与潜在赋值交集；运行时比较 BEFORE trigger 后新旧依赖的实际存储值（NULL 安全、字符按原始字节／类型比较，不能用忽略大小写排序规则吞掉输入变化）。无关列不重算；实际无变化的自赋值不重算；RETRY 无条件推进指定列，主键移动推进所有 AI 列。BEFORE trigger 可能改依赖，存在时将所有 AI 列列为潜在受影响项，运行时细判。

### 2.2 已有同事务内层 SQL 的真实接口

- 公共入口：`src/query/api/query/session/ob_inner_sql_connection_access.h:54` `create_connection_with_external_session(ObSQLSessionInfo*, ObISQLConnectionGuard&)`；`:58` SPI 版本。Observer 实现 `src/observer/ob_inner_sql_connection.cpp:162` 起传入 session，`:418` init_session 保存 `extern_session_`。本仓库是单 server runtime，接口**没有 tenant_id 参数**；不要沿用其他版本 API 虚构租户入参。用 session/current database、全局 table_id 隔离任务。
- 直接调用实例：`src/sql/engine/dml/ob_table_modify_op.cpp:914` open_inner_conn、`:970` begin_nested_session、`:990` execute_write；外键校验 :229 开始 nested、execute_read、关闭结果后 end；级联写也用 execute_write。这是外层业务事务内的实例。
- 新 `AiColumnTxnSqlGuard(ctx)` 包装同样步骤：创建／复用 external-session connection，`ObIInnerSQLConnection::begin_nested_session(ObSQLSessionInfo::StmtSavedValue&, ObIInnerSQLConnection::SavedValue&, bool skip_current_statement_tables)`，执行任务表 SQL、释放 ReadResult，再 end_nested_session；全路径 RAII 恢复。保存并断言 `session->get_tx_desc()` 前后一致。`src/sql/session/ob_inner_sql_connection.h:32` 是该契约接口。
- `ObBasicSessionInfo::base_save_session` `src/sql/session/ob_basic_session_info.cpp:4542` 在 :4577 将内层 autocommit 设 false；stmt_save/restore 合并 tx_result，保留 tx_desc。`:4654` 的 trans_save_session 才会隔离 tx_desc，属于 autonomous 路径，**禁止用于 T1**。
- `src/sql/ob_result_set.cpp:853` auto_end_plan_trans 仅在隐式事务且 autocommit 时提交，nested autocommit=false 不独立提交。禁止 T1 使用自行 start/end 的 `ObMySQLTransaction`；`src/observer/vector_index/ob_vector_refresh_transaction.cpp:108` 的独立刷新事务仅可参考 T2/T4，不能照搬入队。
- `ObSqlTransControl::start_stmt` `src/sql/ob_sql_trans_control.cpp:498` 建立外层隐式保存点；`:793` create_implicit_savepoint，`:905` end_stmt、`:954` rollback_to_implicit_savepoint。finish 的 SQL 位于此外层保存点之后，所以多行失败、语句回滚、显式事务回滚、autocommit 提交统一处理，不需要“回滚时补删队列”。
- **最小必要补充**：现有 `ObSQLUtils::is_nested_sql`（`src/sql/ob_sql_utils.cpp:3374`）只识别 PL、FK cascading、online-stats；普通 begin_nested_session 不会自动让这个分类返回 true。新增 parent ExecCtx 的短作用域 `ai_column_internal_sql` 标记，并纳入 is_nested_sql，不能冒充 FK 标记。`ObResultSet::do_close` :754 已有 `LinkExecCtxGuard`，内层关联父 ctx 和 nested_level 的实现见 sql_utils.cpp:3379/:3393；AiColumnTxnSqlGuard 在此期间开启标记，finally 恢复。该分类影响 start_stmt 的 reserved snapshot 和 end_stmt 的 session 标记清理（trans_control.cpp:552/:976），也禁止内层 plain-insert snapshot 优化（:751）。这一步与 shared tx_desc、nested autocommit=false 一起构成正确接口，不能省略。
- finish 点读本语句新写行需要同语句 read-your-writes。沿用外键 `ob_table_modify_op.cpp:230` 的 parent DAS `iter_uncommitted_row_` RAII guard；`src/sql/ob_sql_utils.cpp:3363` 读取 parent 标志。这里不是读取其他事务未提交任务。

`ObISQLConnection::execute_write(sql,affected_rows,is_user_sql=false)`／execute_read 是真实接口（`src/oblib/common/mysqlclient/ob_isql_connection.h:90` 附近）；基础 prepare/bind 默认 NOT_SUPPORTED，不能假装可直接参数绑定。首版任务 BLOB 使用 hex SQL literal，typed rowkey 用 `ObObj::print_sql_literal`（object/ob_object.h:1918），`ObObjPrintParams.character_hex_safe_represent_=true`、binary hex、目标精度／collation／时区；字符串实现见 `ob_obj_funcs.h:1146`。列／表标识符从 Schema 引用并正规转义，SQL 值不拼接原始输入；`rowkey_predicate` 用相同类型化打印仅供诊断，简单 BIGINT 单键输出测试要求的 `` `id` = 2 ``。更强的绑定封装可通过已有 `ObISQLConnection::execute(ObIExecutor&)`＋`ObSql::stmt_prepare/stmt_execute(ParamStore)` 新增，当前没有拿到即用的 helper，首版不依赖它。

### 2.3 写保护及索引规划

在 `src/sql/resolver/dml/ob_insert_resolver.cpp` 的目标列／VALUES／SELECT 映射中拒绝显式赋值 AI 列（含 NULL、DEFAULT、LOAD DATA／MERGE 等存在的赋值入口）；无列名 INSERT 的隐式目标集合排除 AI 列，类似 generated column 的“可省略”行为，但不把 AI 标记为同步 generated。`ob_del_upd_resolver.cpp:73` resolve_assignments 拒绝 UPDATE、IODKU、触发器 NEW 赋值等用户目标；运行时再断言写集合。

新增不可由 SQL SET 的 `AiWriteCapability` 放在内核 ExecCtx／嵌套执行 guard 中，类别 USER、RETRY、PUBLISH、BACKFILL，绑定 table/column IDs、预期 token；不得仅凭“inner connection”或“root”跳过保护。PUBLISH 允许精确写指定结果及隐藏列，不触发新任务；内部任务表自身无 AI 列，禁止用户业务 SQL 直接写内表。触发器也不能借内部 session 获取能力。

新增 `AiColumnDmlCtDef`，包含结果／三个隐藏列 expr 槽、依赖 ID→new/old expr、typed rowkey、定义及输入参数 expr。**在 Resolver 的 additional assignments 之前**加入潜在受影响结果列／隐藏列赋值；`ob_del_upd_resolver.cpp:435` 的向量更新识别需要看到它们。CG `src/sql/code_generator/ob_dml_cg_service.cpp:301` 在 :348 生成 related index CtDefs，:355 才生成 assignment info；只在运行时把向量改 NULL 会漏索引，不能采用。

### 2.4 并行 DML 与 LOAD DATA：不能遗漏的旁路

`src/sql/engine/pdml/static/ob_px_multi_part_insert_op.cpp:113`、update_op.cpp:128 虽也调用 process_insert/update，但执行上下文可能在 PX 工作线程，当前主语句收集器不能自动收到其全部候选。首版对含 AI 目标表在 `src/sql/optimizer/ob_optimizer.cpp:421` check_pdml_enabled 及 :359 附近 parallel-DAS-DML 特征判定中关闭并行**写**计划，回退普通本地 DML；并行 SELECT 输入仍可在 coordinator 汇聚。已有 batch_params 分支 :441 和不支持特征分支 :457 是同类回退实例。不能只关一个 PDML 开关却让 parallel-DAS-DML 或直接 SSTable 写绕过收集器；普通不含 AI 表保持原规划。首版性能报告须注明此限制。

LOAD DATA 有明确旁路：`src/sql/engine/cmd/ob_load_data_executor.cpp:28` 选择 SPImpl；`ob_load_data_impl.cpp:1284` 解析／分批，:1175 execute_insert_task 同步调用 exec_insert，**:939 使用 GCTX.sql_proxy_->write 新连接**。只给普通 INSERT 加 outbox 不能声称 LOAD DATA 与调用方事务一致。`src/sql/resolver/cmd/ob_load_data_resolver.cpp:341` 默认列列表、:353 SET 赋值还要排除／拒绝 AI 目标。`src/sql/resolver/cmd/ob_load_data_stmt.h:246` 的 cause_implicit_commit 当前固定 true，ResultSet.on_cmd_execute（:258）会先提交已有事务，这个前置提交也必须处理。

决策：保留普通表 LOAD DATA 路径；Resolver 在 ObLoadDataStmt 固定 `ai_transactional_load` 标记，AI 表的 cause_implicit_commit 返回 false，防止执行器接手前就提交用户事务；该标记需要 stmt 拷贝／缓存依赖，并在执行前核对 Schema。AI 表在 LoadDataExecutor 选择同 session 的受控路径，复用现有文件解析／批 SQL 构建，将 exec_insert 的 proxy.write 换为 §2.2 external-session nested execute_write，逐批仍走普通 INSERT／REPLACE／IGNORE 和 outbox。整个 LOAD DATA 使用一个外层命令保存点；有显式事务时不独立提交，autocommit 时全部成功后统一提交，失败回滚该保存点。不能每批提交后仍声称外层可回滚。

现有 ResultSet.start_stmt 要求物理计划（`ob_result_set.cpp:281`），LOAD DATA 由 CmdExecutor :431 调度，没有可直接套用的同等 guard。最小新增 `ObSqlTransControl::begin_command_dml/end_command_dml(ctx,CommandDmlState&)`，从 start_stmt/end_stmt 抽取既有 acquire_tx、`build_tx_param(session,tx_param)`（公开接口 h:123）、`create_implicit_savepoint`、`get_read_snapshot(session,plan_ctx,snapshot)`（h:163）及 rollback-to-savepoint／隐式结束事务逻辑，**不新建 ObMySQLTransaction 或 autonomous session**。新 `AiColumnCommandTxnGuard` 只包 AI LOAD DATA，记录原 autocommit／显式事务属性、外层保存点和 tx_desc；内层 SQL 的 tx_result 合并进外层。此 guard 与普通结果集生命周期公用事务原语，不能用用户可见 savepoint 名称模拟，也不能把“LOAD DATA 当前会分批独立写”当作本题允许事务提前发请求的豁免。新旧 batch 错误／IGNORE 语义按原解析器区分可忽略行错误与整语句错误，后者无部分 AI 业务修改。

## 3. 持久化结构与身份校验

### 3.1 业务行及定义

| 位置 | 字段／类型 | 职责 |
| --- | --- | --- |
| 每 AI 列已有 state | TINYINT nullable | 0/1/2 独立状态；新写显式初始化；遗留 NULL=PENDING，默认结果仍须实际为 NULL |
| 每 AI 列已有 request | VARCHAR(128) binary | 当前组 request_id（36 字节 UUID）；一个组中同列仅一任务，因此 request_id 同时充当 member request 标识 |
| 每 AI 列已有 result_spec | VARCHAR(256) binary | 固定有界二进制封包：format_version:u8、incarnation:UUID16、generation:u64、definition_version:i64；总长远小于 256。不是模型输入／响应；解析必须校验长度／版本 |
| 每行逻辑 incarnation | UUID16，复制在该行每 AI 列 spec | INSERT／REPLACE 新分配，DELETE 后永不复用；PK 移动保留。ADD 首个 AI 列时在行锁下生成，后加列从已有合法 spec 复制；并发回填／更新必须锁行后取值 |
| 每列 generation | u64 | 仅该列失效／手动 RETRY 时 +1；PK 移动所有 AI 列 +1；溢出报错，不回绕。不同列不能共用一个行 generation，否则 RETRY tags 会误废弃 emb |
| 列 Schema definition_version | 非零 i64 | 创建／ADD 时固定保存，不能每次用最新 table schema_version 重算；无关 DDL 不失效任务。当前 getter 对 0 的 fallback 仅作兼容，运行期前应固化 |
| 列 Schema local_session_vars | 已有 ObLocalSessionVar | 复用普通 generated 表达式固化机制保存定义解析的 SQL mode／charset／collation 等；AI 当前漏接持久化条件，下一轮补齐，不再新增平行的 ai_sql_mode 字段 |

UUID 复用 `ObExprUuid::gen_server_uuid(char*,UuidCommon::LENGTH_UUID)`（`src/sql/engine/expr/ob_expr_uuid.h:92`、cpp:483），不是主键／Mock 派生值。该函数生成标准 UUID 的缓冲长度按现有 LENGTH_UUID 常量，不假定带 NUL；存储时解码 UUID16 或保存规范字符。request_id、incarnation、claim_token 分别独立生成，不能互相替代。

### 3.2 新增内表（决策；尚未实现）

在 `src/share/inner_table/ob_inner_table_schema_def.py` 定义并用原生成链生成 C++。已核对空闲 ID **1131 task、1132 group、1135 control**；1133/1134 是 objauth/history，不能占用。落地前生成器再次验证全局唯一。保留现有 `__all_ai_column` 视图。

`__all_ai_column_task`：

| 字段 | SQL 类型／约束 | 说明 |
| --- | --- | --- |
| table_id, request_id, column_id | BIGINT, VARCHAR(36) binary, BIGINT；联合 PK | table_id 前缀隔离，组成员唯一 |
| database_id, row_incarnation | BIGINT, VARBINARY(16) NOT NULL | 恢复定位、生命周期验证 |
| rowkey, rowkey_predicate | LONGBLOB, LONGTEXT | rowkey 是带版本的列 ID／meta／ObObj 值序列（含 heap／复合／分区键）；predicate 仅兼容展示，不作权威键 |
| generation, definition_version | BIGINT UNSIGNED, BIGINT NOT NULL | 入队时快照；与业务 spec／当前 Schema 同时比对 |
| state, eligible_ts | TINYINT, TIMESTAMP(6) NOT NULL | 状态编码见下；立即可领时间用 DB 当前时间 |
| claim_token, lease_until | VARBINARY(16), TIMESTAMP(6) nullable | 每次领取换 token；暂存必须 token 一致且 lease 未过期 |
| attempt_intents, retry_count, max_retries | BIGINT NOT NULL | attempt_intents 为持久化派发意图数；retry_count 为已安排的额外尝试数；max_retries 入队快照 |
| model_id, model_name, operation | BIGINT, VARCHAR(128), TINYINT | 防模型同名删除重建；COMPLETE／EMBED |
| prepared_call, result_type_spec, eval_context | LONGBLOB 三列 | 版本化参数包／稳定目标类型及精度、字符集、维度／本次已求值数据的转换上下文；均无密码及进程内地址 |
| attempt_context | LONGBLOB nullable | 本次实际调用的 provider／协议版本、request_model、响应解析／变换标识；与 raw_response 同次暂存，无密钥；恢复必须按它解析旧响应，不能改用当前 Endpoint 的 provider |
| raw_response, typed_result | LONGBLOB nullable | 响应先持久化；typed_result 含格式版本、ObObjMeta、NULL 标志及内联值，VECTOR 使用稳定 VECTOR(n) 描述和有限 float 数据，不保存 subschema ID／LOB locator |
| error_code, error_class, error_message | BIGINT, TINYINT, VARCHAR(2048) | 有界、脱敏；模型错误与 DB 重试分开 |
| gmt_create, gmt_modified | TIMESTAMP(6) | 清理和诊断 |

必要索引：`(state,eligible_ts,table_id,request_id,column_id)`、`(state,lease_until,table_id,request_id,column_id)`、`(table_id,row_incarnation)`；组内访问走 PK 前缀。大模型参数／结果必须允许 LOB，不能拿 result_spec 的 256 字节存结果。

`__all_ai_column_group`：PK `(table_id BIGINT,request_id VARCHAR(36) binary)`；database_id、row_incarnation、typed rowkey、**不可变 member_count**、state TINYINT（0 OPEN／1 PUBLISHED／2 CANCELLED）、gmt_create/modified。首版不维护易失配的 ready_count，以锁定的成员状态计算屏障。业务行只存当前每列 request，不存一份“全行当前组”，因为一个行可以同时有 tags 新组和 emb 旧组。

`__all_ai_column_control`：PK `(table_id BIGINT,column_id BIGINT,definition_version BIGINT)`；kind（FEATURE_MIGRATION／ADD_BACKFILL／RECONCILE）、ddl_schema_version BIGINT（同次 ADD 成员集合）、state、cursor_rowkey LONGBLOB、claim_token、lease_until、eligible_ts、format_version、gmt_create/modified。保留 `(0,0,format_version)` 特征迁移哨兵；ADD 控制记录与 DDL 元数据同事务写入，回填每批进度与行／任务同事务推进。主键变更可能越过 cursor，结束前做一次 spec 缺失全表校验；普通 DML 也初始化缺失 spec，不能仅凭 cursor 宣告完成。

已发布组和 task 在同一 T4 中删除，业务 token 保留作最终栅栏；全失效／DELETE 组在 T1 或清理事务删除。已暂存但仍 OPEN 的行不按简单 TTL 删除，不能留业务列永久 PENDING。周期 reconciliation 检查缺任务的 PENDING（仅用于旧安装／升级异常），在行锁和 Schema 版本校验下重新建组；不以猜测远端调用是否发生决定安全性。

### 3.3 不能省略或合并的校验

- `request_id == group_id` 可以合并；成员身份为 `(request_id,column_id)`，不再另建相同用途 UUID。
- incarnation 与 generation 不可合并：前者防删除重插，后者允许各列独立推进；UUID request 即使足以防碰撞，也保留 generation 用于审计、严格 CAS 和迁移。
- definition_version 与 table schema_version 不可合并；claim_token 与 request_id 不可合并，租约重新领取仍是同一个业务请求。
- 每次阶段提交校验 task 状态和 claim_token；T4 在业务行锁内校验 table/column 仍存在、当前定义、incarnation、逐列 generation/request。不能只按主键，不接受“新版本仍 PENDING 所以旧结果可写”。

## 4. 任务／组状态及部分替代规则

任务数值编码固定：1 QUEUED、2 RUNNING、**3 RETRY_WAIT**、4 STAGED_SUCCESS、5 STAGED_FAILED、6 SUPERSEDED、7 RAW_READY。

```text
QUEUED / 到期 RETRY_WAIT --T2 领取并提交--> RUNNING(token,lease,attempt_intents+1)
RUNNING --T3 所有权及期限 CAS--> RAW_READY(raw_response)
RAW_READY --本地类型化及短事务--> STAGED_SUCCESS(typed_result)
RUNNING / RAW_READY --调用或校验失败且有预算--> RETRY_WAIT(retry_count+1,next_ts)
RUNNING / RAW_READY --失败且预算用尽--> STAGED_FAILED
RUNNING --租约到期且有预算--> RETRY_WAIT；无预算 -> STAGED_FAILED
非终态或暂存终态 --T1 新代际取代--> SUPERSEDED
所有原成员属于 {STAGED_SUCCESS,STAGED_FAILED,SUPERSEDED} --> 组可发布
组 OPEN --T4--> PUBLISHED + 删除组/成员；无存活成员 -> CANCELLED + 删除
```

RAW_READY 是已有完整响应的持久状态，不再占网络 lease；使用短本地 validation lease/token 或行锁推进，重启后只重做解析／类型校验。STAGED 状态不可被网络重新领取。DB 暂存失败但响应仍在内存时只重试暂存；不再发 HTTP；若提交结果未知，按 token 查询确认。响应尚未持久化就崩溃仍可能再调用，这是无法消除的窗口。

**最终选择：不可变原始成员屏障＋逐成员 SUPERSEDED 退出。** 题面没有明确定义部分替代组的行为；这是本次解释和实施规则。SUPERSEDED 算取消终态，不再等待其外部调用，原组只发布仍归属于它的成员，绝不修改新组的列。

| 时序 | 明确结果 |
| --- | --- |
| G1={tags,emb}，随后 RETRY tags 建 G2={tags} | T1 将 G1.tags 标 SUPERSEDED，推进 tags token 并置 NULL；G1.emb 继续原调用／重试。G1 所有原成员终态后只发布 emb；G2 可独立发布 tags；两者不能互改 |
| G1 同上，仅 tags 的依赖变化 | 与 RETRY 相同；不重新调用 emb，也不把 emb 转入新组 |
| G1.tags 已成功暂存，emb 仍重试，tags 被替代 | 丢弃旧 tags 暂存的发布资格，保留旧 emb；G1 不等待新 tags。若被替代的是 emb，则保留已暂存 tags，不重新推理 |
| G2 先完成，G1 后完成 | G1 发布时 tags request 不匹配，明确跳过；不能以组一次 UPDATE 覆盖全部 AI 列 |
| RETRY 与 G1 发布并发 | 都先锁同一业务行。G1 先获得锁：先发布，RETRY 随后使所选列回到新 PENDING；RETRY 先获得锁：旧成员失效，G1 只发布剩余成员 |
| 删除／主键移动 | DELETE 取消该 incarnation 的组，允许立即清理任务；PK 移动把旧位置所有当前成员废弃，在新键上以同一 incarnation、全列新 generation 建新组 |

每次触发的组自身仍满足“全部成员终态后单事务可见”。跨两次触发的新旧组不要求同一时刻发布。取消整个 G1 并重算 emb 会违反“未受影响列不重调”；保留已替代成员为 PENDING 会永久阻塞；将新 tags 拉入旧组会把两次触发混为一组，均不采用。

锁序：普通 T1／T4 先业务行（多行按语句原有锁序，冲突沿用数据库回滚重试），再涉及 group 按 request_id 排序，再 task 按 column_id 排序。T2 只锁 task；T3 只锁 task 后即提交，不在持 task 锁时再等业务行／group。发布候选扫描无锁读，真正 T4 才按上述顺序重查。发生死锁／快照冲突重试数据库事务，不消耗模型预算。

## 5. 输入求值、AI 复用、结果类型与 Endpoint

### 输入：选择事务内计算普通参数，保存参数快照

不保存依赖镜像给后台重新解释表达式，也不让 worker 读取当前正文。理由是 T1 已有最终新行、正常 SQL 表达式求值器和语句上下文；参数包能避免正文更新、SQL mode／时区变化及 LOB 生命周期造成旧任务输入漂移。

编译时复用 `ObRawExprUtils::build_generated_column_expr` 做绑定／formalize，取 AI 根节点参数表达式，经 `ObStaticEngineExprCG` 生成 CtDef 参数槽；运行时只对这些普通 `ObExpr` 调用 `eval(ObEvalCtx&,ObDatum*&)`，映射到 final new_row。**不生成／调用 AI 根节点 eval，禁止优化器预计算根 AI 表达式**。现有 `ob_dml_resolver.cpp:6153` 合并固化 SQL mode／collation、`:6289` 注册 local_session_vars 是调用依据；新 AI 路径复用它的环境机制，不复用同步 generated 的写值行为。

AI 定义解析环境用已有 `ObLocalSessionVar` 固化（`src/share/session/ob_local_session_var.h:59` add_local_var）；`ob_table_sql_service.cpp:3184`、retrieve_utils.ipp:1080 当前只为 generated 写／读 local_session_vars，需扩为 generated 或 AI。保存 SQL mode、connection charset/collation、时区等会影响允许表达式的变量；对无法枚举环境依赖的函数继续拒绝。类型检查时参数计算类型及 collation 已 formalize，T1 解引用 LOB／JSON，保存实际字符编码、SQL NULL 标志和 config 对象；不保存 Datum 内存或 LOB locator。当前 Resolver 已拒绝 RAND／UUID／NOW 等非纯输入及子查询／变量／AI 嵌套，首版不开放非确定性；字符集／排序规则按正常表达式规则，业务 DML 的时区／SQL mode 用于普通赋值，AI 定义自己的固化环境用于 AI 参数计算。

NULL 沿用实际 AI 函数语义：COMPLETE 的 SQL NULL prompt 当前非法，形成本地 STAGED_FAILED（无 HTTP）；EMBED 在已合入的增强中 NULL 输入直接返回 NULL，AI 列应 READY/SQL NULL 且无 HTTP，不得统一当 FAILED。config 的 SQL NULL／未传表示无 config，JSON null／非对象按增强校验拒绝；JSON literal null 的结果是 READY。此差异必须跟随增强接口，不能依赖旧函数曾报错的行为。输入求值／config 的确定性值错误保存为该成员的本地终态失败、无 HTTP；SQL NULL EMBED 保存为本地成功的 typed NULL，仍参与组屏障，不能提前把该列标 READY。分配失败、事务错误、无法可靠编码输入等系统错误使 T1 失败回滚，不能提交没有可恢复参数的任务。

### 复用及需要拆分的边界

- `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.cpp` 参数／LOB／JSON prompt/config 准备，与 `ob_expr_ai_embed.cpp` 的维度准备抽为纯 `prepare_call`；普通函数和 AI worker 共用，不复制 provider 协议。已合入的 AI 增强新增 `parse_embedding_array`、`validate_embeddings`、`validate_complete_config`，优先使用其最终实现。
- `ob_ai_func_utils.cpp:1283` `ObAIFuncModel::call_completion` 已支持 provider，但 :1291 system prompt 为空；新增可选 `AiCallOptions{system_prompt,deadline,transport_retries,dispatch_observer}`，普通调用使用旧默认值。worker 取内部 result hint，放单独 system 内容、从 provider config 移除内部 key，保持用户输入原样；OpenAI provider 现有 get_body 接受 system/user 两份 prompt（:1302），不能把类型指令拼进原始 content。
- 嵌入复用 `call_dense_embedding_vector_v2`；调用返回后再按目标 VECTOR(n) 做严格验证，不认为发送 dimensions 就已保证正确。
- `ob_ai_func_client.cpp:79` 由 THIS_WORKER 剩余时间覆盖 timeout；异步选项应取 worker deadline 与明确 request deadline 的最小值，并设置 curl 总时限／连接时限和取消回调。异步 `transport_retries=0`，一次任务尝试至多一个 HTTP dispatch；调用计数放在真实 curl perform 前。
- 基线 client :186 未检查 JSON 根类型、embed :1469/1478 强转；共享 AI 增强已修复根类型、向量元素／维度／index 校验、响应缓冲生命周期、鉴权头日志等。保留其修复，下一轮只补 call options、system hint 和异步持久化拆分，不再次覆盖这些函数。

### 类型化暂存与无必要重调

HTTP 完整响应先持久化 RAW_READY，再用同一 provider 解析器提取模型内容并严格转换；因此需要把现有 `call_completion` 的 transport 与 parse_output 分成可单独调用的步骤，旧 API 用这两步封装，普通行为不变。不得等所有 DB 写入成功后才决定是否“本地已有响应”。

| 目标 | 实际接口及处理 |
| --- | --- |
| 文本／数值 | `src/share/object/ob_obj_cast.h:412` `ObObjCaster::to_type`；`ObCastCtx` 用 CM_NONE、目标 collation／accuracy，不启用 WARN／截断放行；`:393` obj_accuracy_check。已有调用 `ob_ddl_resolver.cpp:4187` 起使用 CM_NONE、目标精度并解 LOB |
| JSON | `ObJsonBaseFactory::get_json_base` 完整解析；有效根可以是对象、数组、标量或 null；转换为稳定二进制 JSON，不能只检查首字符／system hint |
| VECTOR | `src/share/object/ob_array_cast.h:89` `ObArrayCastUtils::string_cast_vector`，cpp:930；`ObVectorDataCast::cast` 在 :37 附近检查维度。另复用增强 validate_embeddings 检查数量、重排、每元素 finite、n 恰好匹配；持久化稳定类型描述和 float 数组，发布重建目标 subschema |

typed_result 有格式和 ObObjMeta、精度／扩展类型描述；所有 LOB 扁平化。STAGED_SUCCESS 的后续发布失败只重试 T4。RAW_READY 重启重做解析；非法 JSON、超长／溢出、向量维度错误进入有限模型重试，最终 NULL/FAILED，不崩溃、不截断为成功。DB 断连／死锁／暂存失败不当作模型失败，不消耗新的模型尝试。

Endpoint 选择：每次实际尝试通过 `ObIAiEndpointResolver::resolve_by_model_name(model,allocator,endpoint,check_access)` 获取**当前** Endpoint；已有实现 `src/observer/omt/ob_ai_service.cpp:134`。`__all_ai_model_endpoint` 虽有 version（def.py:2444），`ObAiModelEndpointInfo`／proxy 当前未把它作为可用版本快照返回，不能声称已经 pin endpoint version。任务固化 model_id/type，删除同名重建不复用旧 ID；Endpoint 修改影响下一次尝试，已发出的旧配置响应只要业务版本仍匹配就有效。T3 将本次 provider／协议／响应解析标识的无密钥 attempt_context 与原响应一起持久化，RAW_READY 恢复按该上下文解析，不能拿新的 provider 解释旧协议响应。Endpoint 删除／禁用／暂时不可用按预算失败；任务不存 access_key，执行时解密当前配置，不把凭据写错误消息或日志。后台使用有审计身份的内部会话，权限在定义／业务入口校验，内部能力不能被业务 SQL 取得。

## 6. Worker 生命周期、领取、恢复与发布

### 已有机制及选择

`src/observer/dbms_scheduler/ob_dbms_sched_service.h:30` 继承 `ObServerThreadHelper`；cpp:55 create、:70 start、:77 do_work、:93 stop、:104 wait、:117 destroy 是完整实例。`src/rootserver/ob_server_thread_helper.h:28` 和 cpp:27/47 提供 create(name,count)、run2/do_work、idle/wakeup、stop/wait。`ob_dbms_sched_table_operator.cpp:60` 用有条件 UPDATE 领取，但没有 AI 所需租约令牌和组屏障，不能直接塞入 job 的 this_date 字段。

决策：新增托管 `AiColumnService` 复用 ThreadHelper 生命周期；首版 4 个有界执行线程，轮询有界批次＋条件唤醒，进程停止时停止领取、设置取消标志、等待网络 deadline 内退出，不用 detached thread。现有 `ObAiService` 只是配置查询服务，不是 worker 池；不把 job_queue_processes 偷当 AI 开关，不新增题面未要求的 SQL 限流配置。

注册／销毁放 `src/observer/omt/ob_server_runtime_controller.{h,cpp}`：modules 分配 :1213/1224、服务绑定、init :1420/1462、start :1517/1521、stop :1530、wait :1574、destroy :1619、解绑相关位置；API 若对外只暴露 wakeup，在 `src/query/api/query/ai/` 放小接口，SQL 层不依赖 Observer 私有实现。当前为单 runtime，不新造多租户 worker 语义。线程可随 module 创建，但仅在 Schema／迁移 ready 后扫描；SQL proxy 和 session manager 销毁前必须 stop/wait。

后台 SQL 会话真实实例：`ob_dbms_sched_job_executor.cpp:163` 用 session_mgr.create_sessid/create_session；`:307` `ObWorkerSessionGuard`，init_env、SPI external-session connection；`:181` 成对释放。新 service 使用独立受控 session，每个 T2/T3/T4 明确 start/end，网络期间事务已提交且 connection 释放。不能借用业务 session 或它的 deadline、当前数据库、未提交 snapshot。

### 领取、预算及异常分类

T2 使用独立 READ COMMITTED 事务，扫描到期候选（无锁），按 PK 点锁／条件 UPDATE：仅 QUEUED、到期 RETRY_WAIT 或过期 RUNNING 能领取；同时换 claim_token、设置 lease、持久化 attempt_intents。多个执行者 UPDATE affected_rows=1 才可发送。T2 提交后 HTTP；提交不确定时查询自己的 token，不以本地返回值猜测。

初始请求硬超时 60 秒，lease 至少 deadline+30 秒 DB 余量（默认 90 秒），按领取时 DB 时间写入；可用轻量续租，但不能超过硬调用截止，首版无需无界续租。T3 检查 DB 当前时间 `< lease_until` 且 token 匹配；超时即使尚无人接管也不接受旧响应。RAW_READY／STAGED 已不依赖网络 lease，不因原 deadline 到期丢弃持久结果。

`attempt_intents` 在发送前消费，崩溃发生在领取后、HTTP 前也占一次预算；租约恢复在剩余预算内重试，否则 STAGED_FAILED，避免无限重启导致无限推理。`retry_count` 表示安排过的额外尝试：首个失败立即写 state=3/retry_count=1；第二次派发不再次增加同一计数。退避首轮约 2 秒，`min(30s,2^retry_count 秒)` 加小抖动，用 persisted eligible_ts，不固定 sleep 占线程。配置为极大重试数时计算必须饱和，不能移位溢出。

HTTP 400/429/5xx、超时／断连、协议／类型错误消耗模型预算；schema/table/column 消失、token 被替代是取消，不重试模型。数据库冲突、暂存／发布失败只重试对应 DB 事务。运行者崩溃后回收 lease；RAW_READY 继续解析，STAGED 直接尝试组发布。T4 在提交后返回丢失则重读行 token/组是否已清理，重复发布 no-op。清理服务不持 task 锁等待业务行。

模型实际调用数与本地 intent 数分别统计。运行期 curl dispatch counter 是实际发起次数，Mock/评测端点计数是远端观测；崩溃窗口可能造成统计差异和重复推理，不能宣称远端 exactly-once。只有 Endpoint 协议明确支持才发送幂等键，不强加 provider 不识别的请求字段。清理后的性能报表使用累计指标／端点日志，不把剩余 task 行数当调用量。

### 四条接口级流程（所有 AiColumn* 均为待新增）

#### A. INSERT／依赖更新

```text
Resolver:
  reject_user_ai_targets(); collect_original_read_refs();
  add_ai_slots_before_additional_assignments_and_index_planning();
CG:
  AiColumnDmlCtDef{deps, final rowkey exprs, ordinary AI arg exprs, state/request/spec slots};
ResultSet.do_open_plan:
  start_stmt() -> outer savepoint + snapshot S; AiColumnSchemaFence.begin(all_write_tables);
  strict_check_if_enabled(S); execute_plan();
DML.prepare_row after BEFORE trigger:
  changed = INSERT ? all_AI : binary_changed_dependencies; PK move => all_AI;
  inc = new UUID for INSERT/REPLACE, otherwise old row inc (initialize legacy under lock);
  for changed columns: ordinary_args.eval(new_row, definition_env); deep_copy_payload();
  one new request_id per logical row change; per-column generation++;
  set real result=NULL,state=PENDING,request,new spec; build final new StoredRow;
  after final heap key assignment, collect candidate key/tokens/args;
  normal DAS business/index writes (no HTTP);
ResultSet.do_close_plan after all operators close, before end_stmt:
  if success:
    AiColumnStatementService.finish(ctx):
      external-session nested guard, assert same tx_desc, parent read-own-writes guard;
      compare each final row's current spec/request with collected candidates;
      keep only surviving generations; supersede exactly their old members;
      INSERT groups/member_count and task rows(eligible_ts=DB NOW) in this transaction;
      local NULL/validation outcomes enter STAGED_SUCCESS/FAILED, other members QUEUED;
  failure in any step => outer end_stmt(rollback=true); reset attempt collector;
  outer autocommit / explicit COMMIT later commits both business rows and task rows;
  optional wakeup; committed scan remains the recovery source.
```

#### B. DELETE／主键更新／条件 RETRY

```text
DELETE:
  preserve old typed key + incarnation/tokens in collector; execute normal DAS delete/index;
  finish: verify old incarnation no longer survives; lock its groups and cancel/delete tasks;
  outer rollback restores business/index/task rows together.
PK UPDATE:
  read old inc; compute final typed new key; bump all AI generations, result NULL;
  normal existing delete+insert path preserves inc; collect final new key;
  cancel old-location current requests, create all-AI group at new key in same T1.
RETRY Parser -> Resolver -> UPDATE plan (stmt::T_UPDATE, NOT generic DDL executor):
  resolve target existing AI column else 1235; reject mixed ALTER action with 1235;
  bind original WHERE AST; bare unquoted status -> target state CASE;
  normal column names including `status` bind through ordinary SQL resolver;
  internal RETRY capability adds only target's assignments; original WHERE evaluates normally;
  matched rows lock + bump target generation even if READY; cancel only old target member;
  reuse prepare/collect/finish above; zero matched => no group/task;
  any WHERE/binding/execution error => ordinary statement rollback, no partial retry.
```

#### C. 领取、推理、暂存、原子发布

```text
recover_expired (short transaction):
  expired RUNNING -> RETRY_WAIT(retry_count+1,eligible_ts=backoff,clear token),
                     or STAGED_FAILED if no dispatch budget; CAS old token/lease; COMMIT;
T2 BEGIN RC;
  UPDATE task SET state=RUNNING,claim_token=:fresh,lease_until=DB_NOW+:lease,
                  attempt_intents=attempt_intents+1
  WHERE PK=:key AND dispatch_budget_available
    AND (state=QUEUED OR state=RETRY_WAIT AND eligible_ts<=DB_NOW);
  affected_rows != 1 => skip; COMMIT; confirm ownership if commit outcome unknown;
HTTP without DB transaction/row locks:
  check current model identity; resolve current Endpoint; dispatch captured args once;
T3 BEGIN;
  UPDATE task SET raw_response=:bytes,attempt_context=:captured_protocol,state=RAW_READY
    WHERE PK=:key AND state=RUNNING AND claim_token=:token AND lease_until>DB_NOW;
  COMMIT; 0 rows => discard late response;
local parse/strict cast -> short owned task transaction STAGED_SUCCESS or retry/final failure;
if group may be terminal, schedule T4; polling also discovers it after restart;
T4 BEGIN RC;
  acquire DML table intent / validate current Schema;
  SELECT business hidden tokens by typed final key FOR UPDATE;
  SELECT group FOR UPDATE; lock task members ordered by column_id;
  recheck membership count, all terminal; missing/corrupt member => error/reconcile, never partial publish;
  for each surviving member verify table/column exists, def version, inc, generation, request;
  mismatch => mark SUPERSEDED; no surviving members => cancel/clean group;
  publish exact surviving set in ONE normal UPDATE with private PUBLISH capability:
    SET successful result=:typed, state=READY;
        final-failed result=NULL, state=FAILED;
    WHERE typed_key AND hidden tokens/spec = expected; require affected row=1;
  normal SQL/DAS maintains all related scalar/vector indexes using same tx_desc;
  DELETE all task members + group; COMMIT;
conflict/deadlock => rollback and retry T4 with stored results, no inference;
```

Schema 获取必须在 table intent 持有后检查，并让正常 DAS 的 schema_version 检查生效；DDL 也锁同一物理表，不能在 Schema 检查后让 DROP／ADD 穿过。业务 token 比较和 UPDATE 都在同一 T4 行锁事务内，WHERE 再带 CAS，不能拆成“先查然后另一次事务写”。

#### D. Strict Read 与业务执行

```text
Resolve before rewrite:
  AiReadRefCollector(original bound AST, including reachable children/views)
    -> unique {base_table_id,AI_column_id,state_column_id,definition_version};
  attach refs and schema dependencies to physical plan even when runtime strict=OFF;
Execute each statement attempt:
  initialize transaction/snapshot S (disable lazy snapshot if refs exist and strict ON);
  DML: AiColumnSchemaFence.begin(all_write_tables), verify compiled AI metadata under table intent;
  if session.ai_column_strict_read:
    AiColumnStrictReadChecker builds direct DAS base-table scans with tx_desc and snapshot S;
    project ONLY referenced hidden state columns; scan ALL rows/partitions, no user's WHERE;
    state != READY or legacy NULL => error11120 with IDs BEFORE opening main plan;
  execute original SELECT/UPDATE/DELETE using same S and tx_desc;
  write snapshot conflict => ordinary engine statement retry/rollback, rerun check with new S;
  finish AI outbox writes then end_stmt/commit as in A;
```

## 7. RETRY、Strict Read：绑定与快照依据

### RETRY 不走普通 DDL 提交边界

新增独立 `T_AI_COLUMN_RETRY(table,column,where_ast)`，解析入口在 mysql grammar 的 ALTER 分支；在 `src/sql/resolver/ob_resolver.cpp` 分派专用 Retry resolver，构造 `ObUpdateStmt`／stmt::T_UPDATE，保留原命令文本及内部标记。不能把它作为 Rootserver ALTER TABLE 元数据操作执行，否则有 DDL 隐式提交、条件行表达式执行和事务合并问题。已有 `ObUpdateResolver`／`ObDMLResolver` 名称绑定及执行计划处理 WHERE；只新增伪列绑定与目标能力。

`src/sql/parser/sql_parser_mysql_mode.l:614` 起反引号标识符在 :629 设置 `is_input_quoted_=1`；不是仅双引号才有此信息。Retry resolver 在绑定 T_COLUMN_REF 时，仅 **无表限定且未引用**的 status 生成目标隐藏状态的 CASE 字符串表达式（0 PENDING／1 READY／2 FAILED，NULL PENDING）；带反引号 `status` 及建议的 `t.status` 作为业务列正常绑定。必须在 AST 层保存 quoted 标记进入绑定，不能事后按列名字符串替换。派生 status 表达式不是用户读取全部 AI 结果，strict 不因内部目标赋值／隐藏状态绑定而检查目标列；用户 WHERE 若显式引用其他 AI 结果，则正常 strict。

WHERE 未命中无动作；未知字段等沿用正常 SQL 错误（如 1054）、类型错误沿用表达式规则，坏语法 1064；`status='不存在的状态'` 是合法字符串比较并可零行，除非后续题面明确要求报错。不得通过词法字符串替换兼容测试。

### 引用收集及并发论证

- `src/sql/resolver/dml/ob_dml_stmt.h:939` `get_relation_exprs()`；`:921` get_child_stmts；ColumnItem 的 base_tid/base_cid（:390）给基表映射。在名称绑定后、rewrite／谓词下推／投影裁剪之前收集用户表达式列引用，递归 SELECT 投影、WHERE、JOIN ON、GROUP/HAVING、ORDER/window、子查询、UPDATE RHS／条件、DELETE 条件及适用 RETURNING。`WHERE 0 AND ai_col...` 仍算语句引用，不能优化后消失。
- 视图／派生表要按实际引用输出展开至基列，同时包含视图自身过滤／连接等必需表达式；不能盲扫视图所有未使用投影，导致仅选普通列也被 AI 列阻塞。别名、关联子查询用已绑定 base IDs 去重；SELECT * 展开后包含的 AI 结果算引用；COUNT(*) 本身不读取 AI 值。
- ref 集合序列化进 `ObPhysicalPlan`，添加相应 Schema dependency；运行时读 session 开关，不能把 SET 变量值编译成永远开启／关闭。Prepared Statement、plan cache 命中均执行检查；EXPLAIN 不执行扫描；每次 statement retry 清空已检查标记。
- 检查位置是 `ObResultSet::do_open_plan` **start_stmt(:464) 之后、executor.execute_plan(:479) 之前**。在普通父算子 inner_open 中检查太晚，子 DML 可能已经打开并修改。
- `src/sql/ob_sql_trans_control.cpp:645` stmt_setup_snapshot_，:683 使用 `get_read_snapshot(*tx_desc,isolation,...,snapshot)`；INSERT 的 lazy snapshot :675 必须在 strict 有引用时禁止。new checker 直接用 `ObDASRef`／DAS scan，`src/sql/das/ob_das_ref.cpp:497` create_das_task 在 :508 注入 session tx_desc、:509 注入外层 DAS snapshot；不能使用重新分配快照的普通内层 SELECT 来证明一致。
- 对新 checker 最小新增接口是 `check(ObExecContext&, const AiReadRefs&, const ObTxReadSnapshot&)`，自行构建状态扫描 CtDef／RtDef、分区路由和 schema deps，复用 DAS table scan 及 `ObDASCtx::set_snapshot`，不调用外部 AI。strict 有 ref 时确保 plan.need_trans 和 snapshot 已初始化，即使优化器把主计划变成空／常量计划。
- 并发语义选定**语句快照**：检查在 S 全列 READY，后续 SELECT 读 S；S 后其他事务把另一行置 PENDING，不追溯使该语句失败。若 UPDATE／DELETE 试图改被并发更新过的行，`src/storage/memtable/mvcc/ob_mvcc_row.cpp:1053` mvcc_write 在 :1061、:1075 两次检查 `max_trans_version > snapshot_version`，返回 TRANSACTION_SET_VIOLATION 并撤销节点；重试必须重新检查。RR 使用事务快照，own writes 包含本事务先前失效状态。无需长时间锁整列；代价是每个引用列的整表状态扫描（可合并同表投影，一次扫描检查多个列）。

## 8. DDL 生命周期、旧安装与向量索引

### DDL 与回填

CREATE／ADD 只有 Schema，不发模型请求。新增 AI 定义的原始文本、依赖 ID、规范表达式、local_session_vars、固定非零 def version、三个隐藏 ID 一起保存／拷贝／重映射。

ADD 在 `ObDDLService::alter_table` 的同一 `ObDDLSQLTransaction` 中加入 control 记录，DDL 提交后回填服务按批扫描旧行；结果原本为 NULL，缺状态视为 PENDING，strict 在回填前也不能放行。每批短事务锁行、读当前数据、判断目标 spec 是否已由新 DML 初始化、纯参数求值、置 NULL/PENDING、入队、推进 cursor 一起提交。其他 AI 列不改；ADD 多列属于同次 DDL，按行把这批新增列组成一个组，不把既有列重新拉入。

为封住 ADD 与旧计划写入、DROP 与 T4 的交错，AI 元数据变更使用已有 `src/storage/ddl/ob_ddl_lock.cpp:648` `ObDDLLock::lock_table_in_trans(schema,EXCLUSIVE,trans)`；普通 common DDL 的 ROW_EXCLUSIVE 锁不足以声明排他。存储 DML `src/storage/tx_storage/ob_access_service.cpp:628` check_write_allowed_ 在 :642 使用 ROW_EXCLUSIVE、IN_TRANS_DML_LOCK，:679 携带 dml_param.schema_version、:692 `ls->lock`；schema 检查／锁和 DDL 同表／tablet 锁层级阻止旧写穿过。但携带 schema_version 的锁本身不等于“必定拒绝所有 ADD 之前的旧计划”，不能据此省略 SQL 层栅栏。

最小补充选定新增 `AiColumnSchemaFence::begin(ctx,all_write_table_ids)`，在 start_stmt 后、主计划执行前按 table_id 顺序为**所有业务 DML 目标表**（含编译时尚无 AI 列的表）取得事务 ROW_EXCLUSIVE table intent，再读取最新 Schema，对比计划中的 AI 列集合／依赖／定义版本／隐藏 ID。现成接口 `ObSqlTransControl::lock_table(ctx,table_id,empty_part_ids,ROW_EXCLUSIVE,-1)`（`ob_sql_trans_control.cpp:1160`，:1205 转 data_plane::lock_table，复用同一 tx_desc／tx_param／超时）；Rootserver EXCLUSIVE 是同一物理 table 锁。首次 ADD 若发生在 guard 之前，旧计划检查失败；若发生在 guard 之后，DDL 等事务释放 intent。失败返回已有 `OB_SCHEMA_EAGAIN` 触发 Schema 刷新／重新取计划（`src/sql/ob_query_retry_ctrl.cpp:287`、`:835`），重试重新编译 AI CtDef。只检查“计划已标有 AI”的表会漏首次 ADD，不能采用。代价是普通 DML 目标表多一次缓存 Schema 对比及事务 table intent；首版接受并测量，后续优化必须保持同一栅栏契约。worker 发布／回填也先取得同样 intent 再检查定义。

DROP AI 列／表在原 DDL 事务中按 table/column ID 取消相关任务、标旧组相应成员 SUPERSEDED；列的三隐藏列同步删除／标 unused，不能污染剩余列组。`ObDDLService::drop_column_online`（`src/rootserver/ob_ddl_service.cpp:22492`）及 table drop 路径接入；metadata view 自然不再展示。业务行持有 table intent 后才发布，避免 Schema 先查后删的窗口。DROP DATABASE／TRUNCATE／REPLACE TABLE 等改变行生命周期的 DDL 也取消旧 table/incarnation；TRUNCATE 即使复用 table_id，也在其 DDL 事务清空旧组／任务；后续 INSERT 分配全新 incarnation，迟到响应在 task/token/identity 任一处被拒绝，不额外引入未定义的 table epoch 字段。

首版明确拒绝 AI 定义 MODIFY／CHANGE、结果类型改变、依赖列 DROP／MODIFY／rename（已有检查保留）。题面没有要求必须开放这些 DDL；旧定义响应保护仍用持久化 def version，DROP+ADD 的新 column ID/def 使旧任务无效。若以后开放 MODIFY，必须增加正常 DML 全行失效／索引维护与新定义切换的原子 barrier，不能仅改 expr/version 再后台慢慢把旧向量置 NULL；本轮不假装这个数据重写接口已经存在。普通不影响依赖的 DDL允许，定义版本不随 table version 无故改变。表／列纯 rename 是否支持按当前依赖限制处理，不悄悄引入新支持范围。

### 系统内表安装和迁移

新库已有 bootstrap：`src/rootserver/ob_bootstrap.cpp:548` 构造生成的内表 Schema；`:558` publish_sys_schema；`:571` batch_create_schema 使用 `ObDDLSQLTransaction`，`:594` `ObDDLOperator::create_table(table,trans,...)`。新内表和索引纳入该生成链。

没有找到可直接复用的“把本特征所有新列／内表升级进旧实例”入口。`src/rootserver/ob_local_management_service.cpp:2627` force_create_sys_table **直接 NOT_SUPPORTED**，不可把它列为已支持的升级 API。最小新增 `AiColumnInnerSchemaInstaller::ensure_ready(ObDDLService&)`：在 `ObServer::start`（`src/observer/ob_server.cpp:1322`）wait_metadata_ready 成功后、:1328 net_frame.start 之前，通过 `local_management_service_.get_ddl_service()`（`src/rootserver/ob_local_management_service.h:291`）调用安装器，检查持久 Schema 与生成的目标定义，在受控 `ObDDLSQLTransaction` 中用 DDLOperator 的 create_table／insert_single_column 及 TableSqlService 版本更新添加缺失内表、__all_column/history 字段；写 control 格式哨兵，提交后广播 Schema，再设置 AiColumnService 的 ready 标记和开放 listener／DDL。安装失败返回启动错误，不能先对外接收会写新系统字段的 DDL。按固定 table/column IDs 幂等检测，不用用户 SQL 打开 allow_sys_table_ddl。普通列 SQL writer 也会写新字段，故不能仅在首次 AI CREATE 时才补系统列。

这属于**必要新增安装器**，不是现成复用接口；实现时逐步编译并用旧数据目录升级 smoke 验证。格式解码有版本、旧 metadata 字段缺失读取有默认；对旧 AI spec 缺失行启动持久 reconciliation。在安装成功前不执行 AI 任务；迁移错误要清楚报出，不创建半套可写 AI 表。旧二进制读新格式和滚动降级不在本题支持范围，不能以新增尾字段序列化就声称实现了通用升级兼容。

### 向量：证据、决策与尚须执行的验证

1. `ob_del_upd_resolver.cpp:435` 检查 update assignments 是否更新索引向量／分区键；`ob_dml_cg_service.cpp:348` 生成 related update CtDef，`:1043` `add_vec_idx_col_projector` 将 new VID 映射到 VEC_VID 表达式。AI NULL 失效必须在这之前进入 assignment 集合，发布也通过正常内部 UPDATE。
2. `src/sql/das/ob_das_update_op.cpp:374` 起：vector delta buffer 使用同事务 insert_rows(:384)，其他 domain index delete+insert(:390)，普通索引 update(:399)；open :425 传 tx_desc(:435)、snapshot(:436)、related defs(:440)。这证明是正常 DML 的索引路径，不能由单独插件 add_index 替代。
3. `src/sql/das/ob_das_dml_vec_iter.cpp:34` update 生成两条 domain 行；`:138` generate_vec_delta_buff_row 的旧行是 D(:180)，新行是 I(:183)，旧向量可置 NULL(:200)。`ObPluginVectorIndexAdaptor::insert_rows`（`src/observer/vector_index/ob_plugin_vector_index_adaptor.cpp:1458`）收集 D 的 del_vid；空向量 :1461 收到 null_vid，不添加 embedding；维度检查 :1463。query 补数 :2236 跳过 NULL／空向量。
4. 不能据此说插件事务化：`src/storage/ls/ob_ls_tablet_service.cpp:3204` insert_vector_index_rows 在 :3256 调 adaptor，`:3261` 存储向量置 NULL；adaptor :1512 add_index、:1537 位图修改均可能发生在提交前。查到的可见性权威是 MVCC 辅助表＋查询快照，而不是内存 VSAG 更新本身可自动回滚。
5. query 侧 adaptor :2406 同步路径只在 `read_scn <= bitmap_scn <= query_scn` 用共享位图，否则 :2425 从绑定快照的 row_iter 重建；`:2855` 异步增量路径共享位图比 query_scn 新时从空视图开始；`:2908` 过滤超快照 SCN，`:2934` 合并删除；`:3236` query-local bitmap ready 后取 I-D。`src/sql/das/iter/ob_das_vec_scan_utils.cpp:333` init_scan_param 在 :374 赋外层 snapshot/tx_desc。存储 index-id 更新 :3376 对 sync／async 采用不同处理，不能把向量索引的 async 与 AI 列 async 混同。

**实现决策**：失效、成功发布、失败 NULL、DELETE 全部只用上述正常 DML 和辅助表日志；为 AI 向量查询提供明确的 query-local MVCC 可见性契约，必要时在 `ObDasVecScanUtils`／adaptor 现有重建路径加 `force_snapshot_bitmap`，禁止在事务有 own writes 或共享缓存新于／不能覆盖快照时回退到裸共享位图。候选有效性必须在 ANN top-k 合并前按同一快照排除旧 VID／NULL，不能在最终 SELECT 才过滤而漏掉正确邻居。hidden-PK 的 VID 优化（`ob_del_upd_resolver.cpp:2498`）也要走同一契约，不以换一个 VID 为所有模式的前提。

**待验而非已保证**：当前没有运行 AI 失效／发布或普通向量 NULL＋回滚 SQL；静态代码尚不能证明所有 HNSW／VSAG、heap VID 优化、own-write ANN 和 async-cache 分支都满足回滚可见性。下一轮先做一个普通 VECTOR 的独立事务 smoke，若失败，在上述查询位图／辅助表边界修复后再接 AI；不能把库本身的缺陷归因于 AI 并忽略，也不能宣称现有 ANN 代码已经通过本题。此为明确的实施验收关卡，不需重新选择 outbox／发布架构。

## 9. 下一轮实施顺序、真实文件和必要新增接口

以下按依赖编译，先产生一个可运行闭环，再扩展入口。不要一次生成全部未编译 worker。

| 顺序 | 修改位置 | 复用／新增与完成标准 |
| --- | --- | --- |
| 0 基线复核 | `c899c2054` 中用户已完成的 AI 函数增强合入 | 当前无合并冲突；保留阶段 1 三组 AI COLUMN 测试及增强的 provider 校验，重新构建才能继续声称当前源码有效 |
| 1 元数据可运行 | `src/share/inner_table/ob_inner_table_schema_def.py`、生成目标；`src/observer/ob_server.cpp`、`src/rootserver/ob_bootstrap.cpp`、`ob_ddl_service.cpp`；`ob_column_schema.{h,cpp}`、TableSqlService、retrieve_utils.ipp | 新 task/group/control，安装器、固定 def version、spec codec、local vars；新增 `src/share/ai_column/` 共享结构、`AiColumnInnerSchemaInstaller`；验证新库及旧库重启元数据 |
| 2 DML 编译与保护 | `src/sql/resolver/dml/ob_insert_resolver.cpp`、`ob_del_upd_resolver.cpp`、`ob_update_resolver.cpp`、`ob_dml_resolver.cpp`、`src/sql/optimizer/ob_optimizer.cpp`；`src/sql/code_generator/ob_dml_cg_service.cpp`、`src/sql/engine/dml/ob_dml_ctx_define.{h,cpp}` | `AiColumnDmlCtDef`、纯参数槽、用户写保护；注入 assignments 后重用向量 additional assignments／related defs；先确认普通向量 NULL/ROLLBACK smoke |
| 3 T1 闭环 | `src/sql/engine/dml/ob_dml_service.cpp`、`ob_table_insert_op.cpp`、`ob_table_update_op.cpp`、`ob_table_delete_op.cpp`、`ob_table_insert_up_op.cpp`、`ob_table_replace_op.cpp`；`src/sql/engine/ob_exec_context.{h,cpp}`、`src/sql/ob_result_set.cpp`、`src/sql/ob_sql_utils.{h,cpp}` | 新 `AiColumnDmlService`、可溢写 `AiChangeCollector`、`AiColumnTxnSqlGuard`、`AiColumnSchemaFence::begin`、`AiColumnStatementService::finish`。并补 `src/sql/resolver/cmd/ob_load_data_resolver.cpp`／`ob_load_data_stmt.{h,cpp}`、`src/sql/engine/cmd/ob_load_data_executor.cpp`／`ob_load_data_impl.cpp` 的 AI LOAD 同session路径及 `ObSqlTransControl::begin_command_dml/end_command_dml`。覆盖 IGNORE/IODKU/REPLACE/SELECT/LOAD，提交／保存点／多行失败时 outbox 与数据统一 |
| 4 推理和发布 | `src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.{h,cpp}`、`ob_ai_func_client.{h,cpp}`、complete/embed 参数准备；新增 `src/observer/ai_column/` 服务／表操作器／发布器；runtime_controller.{h,cpp} 和构建清单 | `PreparedAiCall`、`AiCallOptions`、transport/parse 分拆、类型化封包；托管 service、T2/T3/T4、lease fencing、分组屏障和清理；先跑 worker SQL，再故障屏障 |
| 5 RETRY | parser item types（query/objit 对齐）、mysql y/l、`src/sql/resolver/ob_resolver.cpp`；新增 Retry resolver；UPDATE plan 的内核标记 | AST 条件绑定到 UPDATE、quoted status 区分、复用 T1、零行/READY/非法目标/混合动作 |
| 6 strict | `src/share/system_variable/gen_ob_sys_variables.py` 及生成文件、`src/share/ob_errno.def` 及生成文件；`ob_dml_stmt`／resolver 完成点、`src/sql/engine/ob_physical_plan.{h,cpp}`、`ob_result_set.cpp`、`ob_sql_trans_control.cpp` | ref collector、plan refs/deps、runtime session flag、11120、direct DAS checker；整列检查与语句相同 S，PS/缓存切变量/重试均检查 |
| 7 DDL／恢复／ANN | `ob_alter_table_resolver.cpp`、`src/sql/resolver/cmd/ob_show_resolver.cpp`、grammar、`ob_ddl_service.cpp`；`ob_das_vec_scan_utils.cpp`／`ob_plugin_vector_index_adaptor.cpp` 视 smoke 结果 | SHOW SQL 生成与现有视图绑定；ADD 持久 backfill、DROP/TRUNCATE 取消；query-local 可见性契约、own-write/回滚、崩溃恢复 |

SHOW 现成实例是 `ObShowResolver` `ob_show_resolver.cpp:366` T_SHOW_COLUMNS，经 resolve_show_from_table(:381) 做名称／权限再用 SHOW SQL 模板(:402) 生成 SELECT，并绑定 LIKE/WHERE AST；新增 SHOW AI 分支复用它查询 `__all_ai_column`（加 Schema 类型/依赖显示），不做 HTTP，也不走自制表扫描。

## 10. 已运行检查、最小端到端验收与覆盖

### 本轮实际检查

1. 读取 AGENTS、状态／历史、既有文档、五组 SQL／结果／Mock，以及上文实现和调用方。未运行 reset、未改功能文件／SQL期望、未终止任何实例。
2. 在共享工作区切换和增强冲突出现**之前**运行：

```sh
./build_release/unittest/sql/sql_tests --gtest_filter='AiColumn*'
/tmp/seekdb-ai-column-units/schema_tests --gtest_filter='AiColumn*'
```

前者通过 Parser 3、Resolver 6，共 9；后者通过 Schema 3。后者是上一轮生产参数局部链接的临时二进制，时间戳 13:07，不是本轮重建 share_tests；前者／运行库时间戳 18:18，覆盖调查起点的阶段 1。**不能将这 12 例视为合入后的 c899c2054 源码或完整服务器／SQL集成通过。** `build_release/unittest/share/share_tests` 当时不存在。

3. 最终 `git diff --check -- docs/developer-guide/zh/ai-column.md` 通过；重新清点三个 C++、五组 SQL/result 和 Mock 共 14/14 文件存在；Mock 使用 Python 内存 compile 语法检查通过，没有启动服务或生成 pycache。

4. `/usr/bin/mysql`、`/usr/bin/mysqltest` 可用；发现过共享 seekdb 实例，没有向其写入 AI 测试数据或替换二进制。完整构建、五组 SQL、普通向量事务 smoke、崩溃恢复、吞吐／延迟未运行。调查／写文档期间构建清单一度存在 AA 冲突，最终已由用户合入消除；本轮按调查范围没有重建完整服务器，阶段 2 之后的功能本来尚未实现。

### 下一轮最小复现（不改测试期望）

使用用户已完成合并的确定源码基线和**独立构建目录／数据目录／端口**；不要重用或停止共享实例。以下正常构建命令适用于已协调可用的 build_release，否则用独立目录配置等价 target：

```sh
./build.sh release
cmake --build build_release --target seekdb sql_tests share_tests -- -j2
./build_release/unittest/sql/sql_tests --gtest_filter='AiColumn*'
./build_release/unittest/share/share_tests --gtest_filter='AiColumn*'
```

先为隔离实例创建 base-dir；按 `seekdb --help` 启动其单 runtime，分配空闲数据库端口（不是 Mock 的 19120..19124）、独立数据盘／日志盘。本轮不指定替换现有实例的命令，不复用包含真实业务数据的目录。新库与一个保留旧 Schema 的独立目录都验证安装器及重启。

SQL 在 `tools/deploy` 为 cwd（脚本相对路径要求）；`CASE` 依次为 ai_column_ddl、ai_column_t1_dml、ai_column_worker、ai_column_retry_strict、ai_column_vector_index：

```sh
cd tools/deploy
mysqltest --no-defaults --host=127.0.0.1 --port=PORT --user=root \
  --test-file=mysql_test/test_suite/ai_column/t/CASE.test \
  --result-file=mysql_test/test_suite/ai_column/r/mysql/CASE.result
```

先 DDL（19120 是定义地址，不启动 Mock，不需模型凭据）；其余脚本自行 start/stop Mock：19121、19122、19124、19123，串行运行并确认端口所属，不能 kill 别人的 listener。DDL 后先做普通 VECTOR NULL/ROLLBACK/ANN，再接 AI 运行期。现有测试入口缺失时直接报告，不创建假的 tools/ai_gateway。

最小补验使用已有 suite 内的少量 SQL 或 `/tmp` 控制脚本：两连接与 Mock 控制屏障，带截止的状态轮询；复用内核 ERRSIM 在 T2提交后／HTTP后T3前／T4提交前后中断**隔离实例**，并测试过期 token 的旧执行者恢复。现有 Mock 的固定延迟可跑基础乱序，但恢复／原子可见性补验应增加可控屏障，不能用长 sleep 代替。无需新增大量专项文件。

| 评分模块 | 现有证据 | 完整实现后最小验收（当前未通过的项目） |
| --- | --- | --- |
| DDL／Schema | 12 个局部 C++ 例，SHOW/持久化静态调查 | 完整构建＋ddl.test；DDL HTTP零调用，新增／拷贝／重映射／旧库升级，ADD旧行回填，SHOW |
| DML／事务 | 仅真实接口／保存点／DAS路径证据 | t1_dml.test；回滚远端零调用、无关更新零新增、INSERT SELECT、多行失败、IGNORE/IODKU/REPLACE、PK移动、删除重插 |
| worker／一致性 | 仅设计及现有 Mock | worker.test；同组不部分可见、一成功一失败原子发布、旧响应对新PENDING无效、部分组被替代、过期执行者、四个崩溃边界恢复、429/5xx/断连/坏JSON/维度错误 |
| RETRY／strict | 仅 AST引号标志、DAS快照和冲突实现证据 | retry_strict.test；quoted status、READY／零行、条件SQL语义、where外未就绪、join/view/subquery、PS与开关、并发快照／写冲突、失败无部分DML |
| AI向量／索引 | 普通DML与MVCC辅助表/位图代码依据，未动态验证 | vector_index.test＋普通vector smoke；失效/失败/DELETE/发布、事务内ANN、回滚恢复、旧快照/新缓存、heap及同步/异步索引、重启后查询 |

性能最后测：业务 INSERT/UPDATE 到提交的延迟；提交到**组结果业务可见**的延迟；单位时间完成组／成员吞吐；curl实际dispatch与Mock端点计数。固定输入、模型、重试预算及一致性；同时报批大小、并发和失败率；不能用入队吞吐代替完成吞吐。本轮没有性能数字、恢复通过率或官方得分结论。

### 阻塞及实施边界

需求层没有必须再次询问的阻塞：默认值、部分替代组、RETRY事务属性、NULL／Endpoint变更、MODIFY拒绝范围已明确为本次决策。架构接口已定位，可按第9节直接实施，无需再进行同等范围调查。

最终复核工作树已经没有合并冲突，本轮只留下本文修改；**没有当前需求／环境阻塞，已具备按本方案直接实施的条件**。普通向量 NULL／回滚／own-write ANN、系统表安装器和跨入口 finish 收集器是下一轮优先运行的实现验证关卡，均未被描述为已有保证。若阶段 1 所在提交再次不在工作树中，应由用户保留／整合其提交，不能由协作者破坏性恢复整棵树。

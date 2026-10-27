# AI 函数增强：实现与验证

本次开发分支为 `AI-function-enhancement`，基于本地 `2026-competition` 的
`5bce9e06d`。没有修改竞赛测试或预期结果，没有提交或推送。

## 实际调用链

SQL 表达式的 `calc_result_typeN` 决定参数计算类型和字符集，`cg_expr` 注册
标量或批量求值函数。求值阶段从 Datum 取值；TEXT/LONGTEXT 通过
`ObTextStringHelper::read_real_string_data` 解引用 LOB，JSON 通过
`ObJsonExprHelper::get_json_doc` 转为树结构。

AI_PROMPT 返回 `{"template": ..., "args": [...]}`，不会在构造时替换模板。
AI_COMPLETE 消费这个结构时展开模板。模型信息由 `get_ai_func_info` 读取，
Endpoint 由 `ObIAiEndpointResolver::resolve_by_model_name` 解析；
`ObAIFuncModel` 选择 Provider 构造 JSON，再经 `ObAIFuncClient` 的 libcurl
客户端发送。Provider 解析协议结构，共享层验证向量或排序结果，最后通过
表达式结果分配器复制到 SQL 返回缓冲区。

## 六阶段实现

| 阶段 | 原缺陷与修改 | 主要文件和入口 |
| --- | --- | --- |
| AI_PROMPT | 拒绝 TEXT/JSON；直接读取字符串无法处理 LOB；重复占位符的预分配空间不足。保留文本类型和 JSON 结构，使用 LOB 读取器，动态追加每次替换，检查数字下标溢出和结果长度。嵌套 Prompt 保留 JSON 信封，在共享展开器中递归展开，直接嵌套和存入 JSON 列后的行为一致。 | `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.cpp`；`ob_ai_func_utils.cpp` 的 `replace_all_str_args_in_template` |
| AI_EMBED 单行 | NULL 文本曾报错；dimension 转换可能丢失无符号范围；响应强转和维度/元素检查不足。NULL 文本返回 NULL；整数保留原类型并检查正值、范围；先验证响应数量、结构、下标，再检查非空、等维度、有限数字。 | `ob_expr_ai_embed.cpp` 的 `get_dimension`、`eval_ai_embed`；共享层 `parse_embedding_array`、`validate_embeddings` |
| AI_COMPLETE | SQL NULL config 曾被当作字符串读取，二进制 JSON config 处理不正确，返回内容缺少非空检查。SQL NULL/未传配置不添加配置；JSON null/非对象拒绝；对象配置按 JSON 读取；验证 temperature 数值和 max_tokens 正整数，保留额外配置传递；内容必须为非空字符串。 | `ob_expr_ai_complete.cpp`；OpenAI 和共享 Completion 请求/响应逻辑 |
| Ollama | 原生适配代码未接入 Provider 分派，DDL 不接受 Ollama，空 key 不允许。加入大小写兼容的管理和分派；仅 Ollama 可空 key；生成使用 prompt、stream:false，参数转入 options；Embedding 使用 input 和 embeddings。修复只改 Provider 时空 key 被持久化为 SQL NULL 的问题。 | `src/share/ai_service/ob_ai_service_struct.cpp`；`src/observer/ai_service/ob_ai_service_proxy.cpp`；`ObOllamaUtils` |
| AI_RERANK | NULL 文本字段参数被当作对象模式；旧合并假定服务已排序，存在数量和索引检查不足。字符串/对象请求共用每批 20 条限制，每批完整验证，批内 index 转全局 index，合并后按分数降序、原始位置升序排序。文本从原输入重建；对象结果复制原对象并添加 `_model_score`，保留 ID 和其他字段。 | `ob_expr_ai_rerank.cpp`；共享 `call_rerank`、`sort_rerank_result` |
| AI_EMBED 跨行 | 没有表达式批量回调。新增 `eval_ai_embed_batch`，合并兼容的相邻有效行，保留 SQL 行号，跳过 NULL/skip/已求值行；响应先恢复 index 顺序，再写回原行。一次回调的所有请求成功后才发布结果。JSON_LENGTH 原来逐行拉取子表达式，现增加严格参数批量求值，使竞赛里的嵌套调用也能真正合并。 | `ob_expr_ai_embed.cpp`；`ob_expr_json_length.cpp` |

共享 HTTP 修复包括：每次重试清空响应、检查 JSON 根类型、使用有长度的错误消息、
让 curl 拥有请求体副本、保留分配器直到异步缓冲区释放完成，以及不记录鉴权头。
这些逻辑供多个阶段复用。

## 以当前源码和竞赛测试确定的语义

- 占位符从 `{0}` 开始。合法数字占位符越界或整数溢出报错；`{x}`、`{}`、
  `{-1}`、缺右括号等继续按原实现作为字面量保留。构造 Prompt 时仍不要求
  所有占位符都有参数，消费模板时才检查；原测试有这种用法。
- AI_PROMPT 的 SQL NULL 参数保持错误语义；空参数字符串合法，空模板非法。
  JSON Array/Object 保存在 args 中，普通 JSON 值由现有 JSON 打印器序列化，
  合法的 Prompt 信封递归展开。
- 模型及 Endpoint 没有配置默认向量维度的字段，因此未显式指定 dimension 时
  不发送人为固定维度，由服务使用模型默认值；随后检查返回的维度有效且一致。
  竞赛测试和回归 Mock 的默认响应为 768 维，生产代码没有写死 768。
- OpenAI 的带 index 响应检查完整性并恢复顺序；兼容旧的不带 index 响应，
  但拒绝混合有/无 index。Ollama 原生响应没有 index，保持协议数组顺序。
  Dashscope 的 Embedding 使用其 text_index。
- AI_RERANK 的第四参数是对象文本字段名，不是 top_n。现有 SQL 接口和调用路径
  没有全局 top_n 配置，所以要求每个请求完整返回全部文档，不新增 top_n 语义。
- URL 使用 Endpoint 中的完整地址。竞赛约定 Ollama 的两个路径分别是
  `/api/generate` 和 `/api/embed`。表达式每次解析当前 Endpoint，没有复用
  旧 Provider 的计划缓存。
- 批量请求受执行引擎批大小约束，并参考会话 max_allowed_packet 拆分合并输入；
  各请求使用独立临时 arena，SQL 返回值复制到表达式结果内存中。
  不合并不同模型名或不同 dimension 的行，不跨执行器批次收集输入。

## 验证结果

- Release 源码构建成功。
- 17 个 C++ 回归测试全部通过，覆盖模板、JSON/Unicode/转义、重复长参数、嵌套信封、下标边界、
  dimension 类型和值边界、响应结构与元素、乱序恢复、请求多输入字段、
  OpenAI/Ollama/Dashscope 协议、Rerank 数量和稳定排序，以及 skip/已求值/NULL 行。
- 原始 `ai_prompt.test`、`ai_function_enhancement.test` 通过，测试文件及 result 未改。
- `ai_model_ddl.test` 在提供 OBD 通常注入的连接变量和测试 admin 后通过。
- 新增 SQL 回归通过：超过 64KB 的 LOB、直接和存储后的嵌套 Prompt、NULL/JSON
  配置、Endpoint 请求模型名覆盖、HTTP 错误/超时/连接失败/畸形 JSON/错误根节点、默认和显式维度、动态
  dimension、NULL、过滤、LIMIT、乱序响应、重复 index、中间请求失败后恢复、
  跨请求 Rerank 身份，以及 Provider 修改后的原生协议。
- 513 行中 74 行 NULL，实际请求输入数量为 **219、219、1**，合计 439；
  `JSON_LENGTH(AI_EMBED(...))` 也得到相同记录。Mock 倒序返回 OpenAI 结果，
  SQL 向量仍逐行对应正确。独立覆盖 `[A,NULL,B,C]` → `[A,B,C]` → 倒序 index
  响应 → 原 SQL 行映射。

### 未通过或未提供的测试

`ai_model_endpoint_ddl.test` 在创建含 `max_concurrency` 的 Endpoint 时返回 11115。
本分支原有 Endpoint 结构、JSON 解析和管理视图均没有这个字段，属于现有测试与
源码约定不一致；没有修改预期或扩大本次六项功能范围来绕过它。

指定的 `ob_expr_ai_prompt_test.cpp`、`test_ob_openai_utils.cpp` 在本分支不存在；
新增测试位于 `unittest/sql/engine/expr/test_ai_*.cpp`，通过 sql_tests 执行。
本分支也未提供组委会 Mock 的实现和启动方式，已有 AI Column Mock 不覆盖全部
原生协议。此次网络验证使用新增的本地协议回归 Mock；仍需在组委会服务上复验，
不能据此宣称已经获得官方 60 分。未运行 Sanity/ASan/Valgrind。

## 复现

本地测试使用独立的宿主机进程，数据库端口 12881、Mock 端口 8000；
原有 Docker 竞赛实例未更换。数据库访问 Mock 使用 `127.0.0.1`，不把容器中的
localhost 当作宿主机。竞赛用例中的 `0.0.0.0` 在本次 Linux 宿主机进程上可达；
若改用容器运行，必须换成从数据库容器可达的宿主机地址。

```sh
./build.sh release
make -C build_release -j2 seekdb sql_tests
./build_release/unittest/run_tests.sh -R '^sql_tests_shard_' --output-on-failure

python3 unittest/sql/engine/expr/ai_function_mock.py \
  --port 8000 --events /tmp/seekdb-ai-function-events.jsonl
```

另一个终端启动独立数据库，首次启动前创建 base-dir。显式 NO_PROXY 保证本地
请求不经环境中的 HTTP 代理：

```sh
mkdir -p build_release/ai-regression-instance
NO_PROXY=127.0.0.1,localhost,0.0.0.0 no_proxy=127.0.0.1,localhost,0.0.0.0 \
  build_release/src/observer/seekdb --nodaemon \
  --base-dir="$PWD/build_release/ai-regression-instance" --port=12881 \
  --parameter memory_budget=2G --parameter datafile_size=512M \
  --parameter datafile_maxsize=2G --parameter log_disk_size=2G
```

数据库就绪后：

```sh
python3 unittest/sql/engine/expr/ai_function_regression.py \
  --events /tmp/seekdb-ai-function-events.jsonl

mysql --no-defaults -h127.0.0.1 -P12881 -uroot \
  -e 'create database if not exists test'
mkdir -p /tmp/seekdb-ai-mysqltest
```

对 ai_prompt 和 ai_function_enhancement 分别执行以下命令，替换 CASE 名：

```sh
deps/3rd/u01/obclient/bin/mysqltest --no-defaults \
  --host=127.0.0.1 --port=12881 --user=root --database=test \
  --logdir=/tmp/seekdb-ai-mysqltest \
  --test-file=tools/deploy/mysql_test/test_suite/ai_function/t/CASE.test \
  --result-file=tools/deploy/mysql_test/test_suite/ai_function/r/mysql/CASE.result
```

运行 `ai_model_ddl` 还需要测试 admin 和 prologue 中的 OBMYSQL_MS0、OBMYSQL_PORT、
OBMYSQL_PWD，参照 OBD 测试环境设置；不要修改现有用例的连接语句或预期结果。

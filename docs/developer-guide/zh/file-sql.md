# File SQL 开发与验证记录

## 源码盘点（2026-10-26）

初始分支 `the-file-is-database`，工作区干净。2026-10-27 继续时当前分支为 `async-ai-column`；保留该分支和已有工作区，按当前测试宏补齐 File SQL 的 CMake/Bazel 测试入口。工作期间检测到外部提交 `452be98ed`，其包含此前工作区的 File SQL 实现；保留该提交，继续修复，没有由本任务执行提交或推送。公开 mysqltest、数据和部署/清理 include 已存在；`unittest/sql/parser/test_file_sql_parser.cpp`、`unittest/sql/engine/basic/test_file_scan_utils.cpp` 原先不存在。初始源码没有 read_*、glob、COPY 或 enable_file_sql 实现。

已有机制及本次接入点：

- `sql_parser_mysql_mode.y` → `ObDMLResolver::resolve_file_table_item`：文件函数复用函数表解析节点，在绑定前流式推断动态列。
- `ObResolverUtils::get_all_function_table_column_names`、`resolve_function_table_column_item`：动态列参与原有名字空间、别名、星号展开和列类型检查。
- `ObJoinOrder::generate_function_table_paths` → `ObLogFunctionTable` → `ObStaticEngineCG::generate_spec` → `ObFunctionTableOp`：只扩展已有函数表算子；过滤、排序、聚合、JOIN 均由原生计划执行。
- `file_scan_utils`：只读文件访问、身份校验、受限元数据、两遍 CSV/JSONL 扫描及流式 glob。
- `rust/file-sql`：Apache Parquet/Arrow 格式解码，无 SQL 引擎；接收 C++ 校验后复制的 fd，批量读取并按物理列裁剪。
- COPY → SELECT 的 INTO 节点 → `ObSelectIntoOp` → `CsvExport`：原生查询流式输出，同目录临时文件成功关闭后通过 `linkat` 无覆盖发布。

评分表为 7、10、7、6、6、6、4、4，共 50 分；COPY 段落写“30 分”与总表冲突。本实现保留 COPY；仓库未提供八项正式名称和完整赛题说明，不据此宣称得分。

## 接口与语义

```sql
SET SESSION enable_file_sql = ON; -- 默认 OFF
SELECT * FROM read_csv('/allowed/input.csv', header = true, sample_size = -1);
SELECT * FROM read_json('/allowed/input.jsonl', format = 'newline_delimited');
SELECT * FROM read_ndjson('/allowed/input.jsonl');
SELECT * FROM read_parquet('/allowed/input.parquet');
SELECT file FROM glob('/allowed/*.csv');
DESCRIBE SELECT * FROM read_csv('/allowed/input.csv');
COPY (SELECT * FROM read_csv('/allowed/input.csv') ORDER BY id)
  TO '/allowed/export.csv' (format csv, header, nullstr 'NULL');
COPY my_table TO '/allowed/table.csv' (format csv, header);
```

CSV 默认有表头，分隔符逗号，引号和转义均为双引号，NULL 文本为空。支持 header、sample_size、delimiter/delim、quote、escape、nullstr；COPY 的对应选项采用上例语法，默认不写表头。别名不能绕过重复参数检查。其他参数和 sample_size 非 -1 会报错。JSONL 接受 format 与 sample_size；Parquet 和 glob 不接受附加选项。

以下为公开测试未明确部分的实现选择，不代表官方规范：

- 全量推断只保留列状态，不保留数据行。类型格为 NULL → BOOLEAN/BIGINT/DOUBLE/DATE/DATETIME/VARCHAR；BOOL 与整数合并为 BIGINT，数值合并为 DOUBLE，DATE 与 DATETIME 合并为 DATETIME，其余冲突为 VARCHAR。
- 超出 BIGINT、DOUBLE 溢出/下溢的 CSV 数字保留为文本。若 DOUBLE 列出现绝对值超过 2^53 的整数，则整列为 VARCHAR，防止整数被无提示舍入。
- CSV 未加引号的 nullstr 是 NULL；加引号的空字符串和 nullstr 保持字符串。加引号的数值仍可推断为数值。
- JSONL 的 JSON 字符串保持字符串，仅合法 ISO 日期/时间字符串推断为 DATE/DATETIME。JSON 数字不通过 double 中转整数文本。缺失字段和 JSON null 均为 SQL NULL，字符串 `"null"` 仍为文本。
- JSON 字段按首次出现顺序并集。同一对象中按 seekdb 原生 UTF8MB4_GENERAL_CI 名称比较等价的键报错（包括大小写、重音等价）；跨记录的等价键归入首次出现的列。CSV 等价/重复表头按 `name__2` 等顺序消歧；空名称生成 `column0` 等。列引用采用 seekdb 原有大小写规则。
- 全 NULL 列为可空 VARCHAR。空 CSV/JSONL 无可推断列时明确报错；只有 CSV 表头时有零数据行且列为可空 VARCHAR。空行视为记录，JSONL 空行报错。
- 日期使用严格 ISO 日历，检查闰年；不推断时区。不合法日期保留为文本。DATETIME 支持空格/T 分隔和最多六位小数。
- glob 仅支持最后一个路径分量中的模式；逐个发现并验证普通文件，直接目录项跳过；越界链接和特殊文件报错，不静默返回越界结果。

## 安全、一致性与资源

只支持 Linux 本地普通输入文件。遵守既有 secure_file_priv 语义：空值/NULL 拒绝，`/` 允许全路径，其余为目录边界。生产环境每次校验均从全局 Schema 读取 secure_file_priv，避免沿用旧连接中的值；修改此全局变量仍遵守原有 Unix socket 限制。每次打开校验 realpath、打开后的 `/proc/self/fd` 路径及 inode；输入 O_RDONLY、O_NONBLOCK、O_NOFOLLOW，特殊文件在读取前拒绝。功能开关和当前权限在推断、扫描、重扫及 COPY 开始/发布时检查。

记录 dev、inode、size、纳秒级 mtime/ctime；推断前后、扫描打开、每批/128 行、EOF、关闭、rescan 校验原路径和打开文件。能检测通常的追加、截断、替换和原地修改。这不是快照隔离：仍有检查间隔以及元数据被特权修改的边界，不能保证外部写入期间绝无混合字节。检测变化会以失败结束查询。

文件查询禁用计划缓存，保留原有函数表并行禁用机制。文件路径和选项要求 SQL 字面量，prepared 路径参数明确拒绝；带字面量的 prepared 执行重新绑定并运行时校验文件身份。二进制 prepared 实测在两次执行之间替换同路径文件，能读到重新推断后的值；返回列数变化时，libmysqlclient 明确返回 2057，需要客户端重新 prepare，不能继续使用旧的结果绑定。

资源上限：列 1024，列名 256 字节，单记录/字段 8 MiB，Schema 编码 1 MiB，读取缓冲 64 KiB，每批最多 128 行且约 8 MiB；Parquet Footer 8 MiB，Row Group 4096 个，页压缩/解压 16 MiB，单 Row Group 压缩总量、解码总量各 64 MiB，每组最多 65,536 页，每页值数 100 万。受限 STL/Rust 缓冲尚未全部纳入 seekdb 租户内存统计。取消检查在推断每条记录和扫描批次运行；Parquet 页头预检每 128 页及每个 Row Group 检查原生查询状态；单页解码期间仍以页/批次为取消边界。

Parquet 支持平面 BOOLEAN、有符号整数及至 UINT32、FLOAT/DOUBLE、UTF8、DATE32、无时区秒/毫秒/微秒 TIMESTAMP。拒绝嵌套、UINT64、DECIMAL、INT96 等未绑定类型；允许 PLAIN、PLAIN_DICTIONARY、RLE、RLE_DICTIONARY、BIT_PACKED、BYTE_STREAM_SPLIT，压缩启用 Snappy、gzip、zstd、lz4。当前明确拒绝 Delta 系列编码：库中 Delta 字符串解码器会在检查页值数之前依据页内计数分配数组，仍需受限解码支持。先检查 Footer、每页长度/复杂度及数据页计数与 Footer 行数的一致性，再调用格式库；零投影 COUNT(*) 同样核验页计数，不整文件解压。含字符串投影时 Arrow 每次解码一行，防止字典长值重复展开成大数组；固定宽度和空投影为 128 行，seekdb 扫描器继续按行数和字节上限组批。

坏页实测暴露了 Apache 解码路径的 panic。格式 FFI 的打开、初始化、批次读取及单元格转换均捕获 panic 并返回明确错误；不跨入 C++。由于 Rust 静态库共享 panic runtime，Release 与 cmake-debug 统一使用 unwind，File SQL 在内部捕获，其他 C ABI/cxx 入口仍在 panic 逃逸时终止，保持禁止 Rust panic 进入 C++ 的边界。File SQL 在 panic=abort 的 Linux 构建上有编译期拒绝，防止误链接不能捕获错误的解码库。一次对基础 Snappy 样本的 1,782 个逐字节翻转/截断检查发现原先 36 个进程异常退出，修复后为 0；这不是完整格式模糊测试或所有编码的安全证明。

COPY 全部非 NULL 字段加引号并转义，NULL 文本不加引号。目标已存在（含链接）拒绝，临时文件在允许目录中使用 O_TMPFILE 匿名创建（当前要求文件系统支持），成功 fsync 和关闭写句柄后，通过固定 inode 的只读句柄发布；`linkat` 原子拒绝并发目标。失败关闭句柄即回收匿名文件，不存在临时路径替换或清理误删；不覆盖、删除已有目标。断电后的目录持久性不在当前保证范围。

## 构建与测试入口

```sh
./build.sh release
make -C build_release -j2 sql_tests seekdb
GTEST_FILTER='FileSqlParser.*:FileScanUtils.*:FileSqlTypes.*' \
  ./build_release/unittest/run_tests.sh -R '^sql_tests_shard_0$' --output-on-failure
cargo test --manifest-path rust/Cargo.toml -p file-sql
```

公开 mysqltest 的正式部署入口：

```sh
cd tools/deploy
./obd.sh mysqltest -n <隔离部署名称> --suite file_sql
```

该公开脚本通过 SSH/SCP 在 observer 工作目录部署数据，并且使用 admin 账户；本地直连运行须先配置对应测试账户、环境变量和 SSH，不能把删掉公开测试步骤的派生脚本称为原测试通过。

最终结果见下节。跨平台和 Bazel 构建未运行，不由 Release 编译通过推断为通过。

## 最终构建与测试结果（2026-10-27）

按用户最后要求减少验证，收尾仅运行最终版本的 File SQL 单元测试、一次成功的端到端回归和原始 mysqltest；未继续扩展专项检查。

| 检查 | 实际命令或入口 | 结果 |
| --- | --- | --- |
| Release 配置 | `./build.sh release '-DTHIN_LTO_CONCURRENCY_LINK=-Wl,--threads=1,--thinlto-jobs=1'` | 成功 |
| 最终构建 | `make -C build_release -j1 seekdb sql_tests` | 成功；此前并行链接共享库被系统 Killed，改为单作业、单线程链接后成功 |
| Parser / 文件工具 | `GTEST_FILTER='FileSqlParser.*:FileScanUtils.*:FileSqlTypes.*' ./build_release/unittest/run_tests.sh -R '^sql_tests_shard_0$' -V` | 12 / 12 通过 |
| 原生 SQL | 下方 `test_file_sql_integration.py` 命令 | 66 项 SQL 检查通过（含准备和清理）；覆盖坏页错误及服务存活、投影/过滤/聚合/HAVING、排序/LIMIT、JOIN、强制重扫、COPY、普通 SQL 等 |
| 原始公开 mysqltest | 在 `tools/deploy` 执行 `../../deps/3rd/u01/obclient/bin/mysqltest --host=127.0.0.1 --port=2887 --user=root --database=test --ssl=0 --test-file=/tmp/file_sql_public_final.test --result-file=mysql_test/test_suite/file_sql/r/mysql/file_sql_basic.result` | `ok`；wrapper 仅设置部署变量并 source 原始 `.test`，原始部署/清理 include 和结果文件均未修改 |

公开测试使用本任务创建的回环 SSH 服务、临时密钥及测试账户，不修改用户 SSH 配置；结束后删除临时账户和密钥并停止服务。wrapper 中含临时测试密码，完成后删除。正式部署仍可使用前述 `obd.sh mysqltest` 入口。

最终测试实例为 `/tmp/seekdb-file-sql-final-mc5eyh1f`。旧实例目录 `/tmp/seekdb-file-sql-instance-lssC2d` 缺少并行 AI Column 改动新增的 `ai_expr` 内表字段，最终端到端首次在创建普通表时失败；保留旧目录，改用新建实例后全套成功，没有修改测试预期或绕过该普通表检查。两个测试实例均在结束后停止，目录保留。

大文件检查读取 134,610,955 字节、131,072 行 CSV，查询输出正确，用时约 0.83 秒；该轮每 10 ms 采样的 RSS 增量为 0。RSS 采样不是精确峰值，也不能单独证明没有物化；流式约束还由读取状态、记录/批次上限及代码调用链保证。

收尾前已完成的相关验证：Rust `cargo test --manifest-path rust/Cargo.toml -p file-sql` 的 2 个测试、Clippy `--all-targets -- -D warnings`、`cargo fmt --check` 均通过；工具及 parser 测试的 12 项 ASan/UBSan 检查通过（仅这些 C++ 源文件插桩，底层 Release 库未全量插桩，关闭 LeakSanitizer，不能视为全库内存安全证明）。二进制 prepared 的同路径值/类型变化重新绑定已实测，列数变化返回客户端 2057。未运行全库 Sanitizer、Bazel、跨平台、所有编码/压缩组合及长期压力测试。

最后一次 `git diff --check` 通过。没有执行提交、推送或覆盖外部 AI Column 工作。

## 功能对照

下表按八个功能面整理交付证据，不冒充缺失的正式评分项名称，也不把给定八个分值自行映射到功能面。

| 功能面 | 实现与验证范围 | 仍有的边界 |
| --- | --- | --- |
| CSV | 状态机、多行引号、CRLF、转义、NULL/空字符串、重复表头、全文件推断、边界及坏数据用例 | 单字节 ASCII 控制字符；单记录 8 MiB |
| JSONL | read_json/read_ndjson、全文件字段并集、稳定顺序、缺失 NULL、晚出现字段/类型冲突、重复键/嵌套拒绝 | 仅逐行扁平对象，空行拒绝 |
| Parquet | Apache 解码、基础类型、列裁剪、空投影 COUNT、Snappy、多 Row Group、NULL、损坏元数据与压缩大小限制 | Delta 系列未实现；其他压缩未逐个运行；不支持类型见上文 |
| 绑定与元数据 | 命名参数、别名、列归属、星号、Unicode 等价名、DESCRIBE、glob | 只接受字面量；glob 仅最终路径分量 |
| 原生 SQL | 表达式、WHERE、排序/LIMIT、聚合/HAVING、空结果、INNER/LEFT/跨格式 JOIN、EXPLAIN | 未单独覆盖每种优化器计划组合 |
| 扫描生命周期 | 流式推断/扫描、批量取数、字符串有效期、强制无物化嵌套循环重扫、LIMIT/取消/失败关闭 | 文件一致性依据元数据检查，不是不可变快照 |
| 安全与资源 | 默认关闭、全局 secure_file_priv、目录边界、符号链接、特殊文件、身份变化、资源上限、句柄回收 | Linux；部分受限缓冲未接入租户内存统计 |
| 兼容性与缓存 | 普通表/聚合/UNION、TABLE(generator)、同名 read_csv 标识符、文字和二进制 prepared；文件计划不入缓存 | Bazel、跨平台、全库 Sanitizer 未运行 |

COPY 单独完成：表或原生查询流式输出、CSV 选项、NULL/空字符串、特殊字符、UNION、读回、不覆盖已有/并发目标、取消与失败清理；要求允许目录所在文件系统支持 O_TMPFILE。整行大小限制在读取原生结果时即检查，发布前再次检查查询状态。

## 本地复现与清理

在隔离实例上，以下脚本自动准备数据、恢复原权限设置，并清理本次测试文件与随机测试数据库：

```sh
python3 unittest/sql/engine/basic/test_file_sql_integration.py \
  --port 2887 --socket /tmp/seekdb-file-sql-final-mc5eyh1f/run/sql.sock \
  --large-rows 131072 \
  --server-pid "$(cat /tmp/seekdb-file-sql-final-mc5eyh1f/run/seekdb.pid)"
```

本次实例已停止；复用该目录可先运行（只操作本次创建的测试目录）：

```sh
./build_release/src/observer/seekdb --nodaemon \
  --base-dir /tmp/seekdb-file-sql-final-mc5eyh1f --port 2887 --log-level WARN
```

新建实例时使用新的空目录，并增加本次实际使用的初始化参数：`--parameter memory_limit=2G --parameter log_disk_size=2G --parameter cpu_count=2 --parameter datafile_size=1G --parameter datafile_maxsize=2G`。完成后向该目录 `run/seekdb.pid` 中记录的进程发送 SIGTERM，等待退出后可删除自己创建的隔离目录。不要将旧内表结构的目录当作新版本的空实例。

最小 SQL 例子（先通过 Unix socket 将 secure_file_priv 设置为已存在的测试目录；结束后恢复原值）：

```sh
task_dir=$(mktemp -d /tmp/seekdb-file-sql-demo.XXXXXX)
printf 'id,value\n1,"a,b"\n2,\n' > "$task_dir/input.csv"
printf '%s\n' "$task_dir"
```

将下列 `/tmp/seekdb-file-sql-demo.XXXXXX` 换成上一步输出的目录：

```sql
SET GLOBAL secure_file_priv = '/tmp/seekdb-file-sql-demo.XXXXXX';
SET SESSION enable_file_sql = ON;
SELECT id, value FROM read_csv('/tmp/seekdb-file-sql-demo.XXXXXX/input.csv') ORDER BY id;
EXPLAIN SELECT COUNT(*) FROM read_csv('/tmp/seekdb-file-sql-demo.XXXXXX/input.csv');
COPY (SELECT * FROM read_csv('/tmp/seekdb-file-sql-demo.XXXXXX/input.csv'))
  TO '/tmp/seekdb-file-sql-demo.XXXXXX/output.csv' (HEADER, NULLSTR 'NULL');
SELECT * FROM read_csv('/tmp/seekdb-file-sql-demo.XXXXXX/output.csv', nullstr='NULL');
-- 恢复原来的 secure_file_priv，关闭会话或将 enable_file_sql 设回 OFF。
```

```sh
rm -f -- "$task_dir/input.csv" "$task_dir/output.csv"
rmdir -- "$task_dir"
```

Parquet 扩展测试数据的生成方式见 `unittest/sql/engine/basic/data/README.md`，使用同一 Apache 格式库生成，不依赖外部查询引擎。

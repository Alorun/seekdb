# FTS 65%+ Performance Optimization Plan

Status: design only. No implementation, test, or benchmark changes are included in this document.

## Objective

Pursue at least 65% mean latency reduction against the fixed benchmark baseline, with an internal design target of 70% to leave room for platform noise. The work is aimed at the three equally weighted score classes: index build, TOKENIZE, and MATCH query.

The scoring rule is:

  overall improvement = (build class + tokenize class + query class) / 3

Each class is itself the arithmetic mean of its listed metrics. A 65% overall improvement therefore requires the three class improvements to sum to at least 195 percentage points. A large improvement in only index building cannot meet the objective by itself.

For reference, if every individual metric reached 65% improvement against the checked-in baseline, the target times would be:

| Metric | Checked-in baseline | Time at 65% improvement |
| --- | ---: | ---: |
| build_ik_all_sec | 35.2836 s | 12.3493 s |
| build_ik_content_sec | 28.3764 s | 9.9317 s |
| build_beng_en_sec | 14.7578 s | 5.1652 s |
| tokenize_ik_avg_ms | 0.76478 ms | 0.26767 ms |
| tokenize_beng_avg_ms | 0.42262 ms | 0.14792 ms |
| query_cn_avg_ms | 16.66282 ms | 5.83200 ms |
| query_beng_avg_ms | 24.30424 ms | 8.50648 ms |
| query_mixed_avg_ms | 17.55928 ms | 6.14575 ms |
| query_limit_avg_ms | 16.23338 ms | 5.68168 ms |

These are planning thresholds, not evidence that the current branch or a local machine meets them. The previously shared JSON's fts_mean_improvement field does not reconcile with its metric values when scored against the checked-in baseline. Platform acceptance must use the platform's per-metric report or a controlled run against the same baseline and hardware.

## Guardrails

- Preserve complete TOKENIZE output, token order, parser options, stopword behavior, and IK SMART/MAX_WORD arbitration.
- Preserve MATCH result sets, relevance scores when requested, duplicate elimination, transaction snapshot visibility, and DML maintenance.
- Keep the persisted inverted and FTS_DOC_WORD table schemas and row encodings unchanged.
- Do not add SQL syntax, cache input text, cache query results, change benchmark SQL/data/score files, or bypass visibility checks.
- Keep the ordinary non-FTS index build path unchanged.
- Use bounded memory and explicit backpressure. A faster path that grows memory with total document or posting count is not acceptable.

## Current Code Path

The parent FTS DDL task creates the inverted-index auxiliary table and FTS_DOC_WORD auxiliary table as separate child builds in ObFtsIndexBuildTask::prepare_aux_index_tables. It records their task IDs and waits for both in prepare_aux_index_tables / wait_aux_table_complement. The build arguments carry the parent snapshot.

During each child build, the FTS table scan reads a source row and its text value, resolves LOB data, then calls ObFTIndexRowCache::segment. ObDASDomainUtils::generate_fulltext_word_rows calls the parser into a per-document word-frequency map, allocates one datum row per unique word, and arranges fields in one of two orders:

- Inverted index: WORD, DOC_ID, WORD_COUNT, DOC_LENGTH.
- FTS_DOC_WORD: DOC_ID, WORD, WORD_COUNT, DOC_LENGTH.

The generic index build path generates an INSERT SELECT statement in ObDDLTaskUtil::generate_build_replica_sql. That statement uses parallel DML and currently sets enable_newsort=false.

On the query path, ObDASIterUtils::create_text_retrieval_sub_tree composes text retrieval with lookup and optional sort. For unscored retrieval with lookup and no sort, it calls set_limit_after_lookup so the posting merge does not truncate candidates before row visibility is known. The existing no-score path already avoids relevance calculation; this plan targets work after candidate generation and safe stop behavior after lookup.

The current branch also contains a BENG ASCII fast path and an IK candidate automaton. The tokenizer phases below extend those paths rather than reimplementing completed work.

## Target Architecture

For each FTS DDL build, use one tablet-parallel source producer:

  source scan and LOB read
      -> one DOC_ID evaluation
      -> one parser invocation per indexed text value
      -> per-document term aggregation
      -> bounded logical posting batches
          -> inverted-key sorter and writer
          -> doc-word-key sorter and writer
      -> both targets complete
      -> existing DDL catch-up and publish transitions

Each posting's term bytes and DOC_ID are owned by a bounded batch until both consumers have accepted the posting. The two sinks may consume at different rates; a full sink applies backpressure to the producer. Sorting and writes remain independent because the target key orders differ.

The shared producer must use one DOC_ID value for both sinks. Existing idempotent DOC_ID generation uses DDL slice and row position, so retries must preserve those inputs or replay the paired tablet unit. Snapshot handling, online DML catch-up, checksums, cleanup, retry whitelist behavior, and final publication remain under the existing parent DDL state machine.

## Implementation Phases

### Phase 1: Shared FTS scan and segmentation

1. In src/rootserver/ddl_task/ob_fts_index_build_task.cpp, split auxiliary schema creation from data-build submission. Today prepare_aux_table creates an auxiliary target and submits its build immediately. Create both target schemas first, then submit one FTS-specific paired build task with both target IDs, the parent snapshot, parser settings, and parallelism.
2. In src/rootserver/ddl_task/ob_fts_index_build_task.h and .cpp, add paired-build task identity and durable progress to the FTS task state. Update serialize_params_to_message, deserialize_params_from_message, child completion tracking, and wait_aux_table_complement. Do not change generic DDL task state layouts.
3. In src/rootserver/ddl_task/ob_ddl_scheduler.cpp, integrate the paired task with the existing FTS task factory/scheduling path. If the current local management service boundary is required, update src/rootserver/ob_local_management_service.cpp only for the new internal FTS build request; preserve the ordinary create-index request path.
4. In src/rootserver/ddl_task/ob_ddl_common_rs_impl.cpp, keep generate_build_replica_sql unchanged for ordinary indexes. Add an FTS-only build plan or request path that scans the source once and targets both auxiliary tables. The source snapshot must remain the parent's snapshot_version_.
5. In src/sql/das/ob_das_domain_utils.cpp and ob_das_domain_utils.h, separate logical posting generation from target-row layout. Replace the target-specific row materialization in generate_fulltext_word_rows with a logical posting batch containing term, DOC_ID, term frequency, and document length. Keep the parser's grouping and stopword rules unchanged.
6. In src/sql/engine/table/ob_table_scan_op.cpp, refactor fetch_next_fts_index_rows so one source row and one parsed result feed the paired producer. Keep LOB ownership valid until both sinks consume the batch. fill_generated_fts_cols remains available for the legacy single-target path until the paired path is fully proven.
7. In src/sql/engine/table/ob_table_scan_op.h and a focused DAS/DDL definition header, add the bounded batch/context interfaces. Define explicit maximum bytes and postings per batch; do not size buffers by total table size.
8. Preserve paired retry semantics. If either destination fails before tablet completion, clear or replace both partial outputs for that tablet and replay the shared input unit, or persist independent idempotent sink checkpoints. Never publish one completed side with a stale or incomplete other side.
9. Keep current DDL completion, online change catch-up, checksum, and publish ordering. The paired task reports success only after both destination writes and validations finish.

Primary implementation files:

- src/rootserver/ddl_task/ob_fts_index_build_task.cpp and .h
- src/rootserver/ddl_task/ob_ddl_scheduler.cpp
- src/rootserver/ob_local_management_service.cpp, only if the task RPC needs extension
- src/rootserver/ddl_task/ob_ddl_common_rs_impl.cpp
- src/sql/das/ob_das_domain_utils.cpp and .h
- src/sql/engine/table/ob_table_scan_op.cpp and .h

### Phase 2: FTS batch sort and write

1. Add two FTS sink adapters over the logical posting batch. Each adapter forms its existing key order without copying term bytes when the batch remains alive.
2. Implement tablet-bounded sorted runs and a k-way merge for each destination. Make sort state independent per target so backpressure in one sink cannot corrupt the other.
3. In src/rootserver/ddl_task/ob_ddl_common_rs_impl.cpp, keep the current generic DDL SQL path for non-FTS indexes. Compare an FTS-only path against the existing INSERT SELECT path. The current enable_newsort=false hint is a research point; do not remove it globally.
4. Inspect the generated FTS physical plan first. If its sort is SQL-engine sort, the candidate integration points are src/sql/engine/sort/ob_sort_op_impl.cpp and ob_sort_op.cpp. If the DDL path sorts in DAS instead, integrate at src/sql/das/iter/ob_das_sort_iter.cpp. Do not maintain two sort implementations unless the physical plan requires both.
5. Optimize FTS sort records using precomputed term comparison metadata or a compact temporary representation. Temporary encoding must preserve byte/collation ordering exactly and must be decoded before writing the existing table row format.
6. Bound run memory by bytes, not only row count. Reuse batch buffers after both sinks advance. Track bytes read, postings emitted, run count, spilled bytes, merge passes, and per-target flush time in temporary diagnostics only.
7. Keep sort spill state task-local and disposable on retry. Do not change SSTable/index row encoding or add benchmark-only files under tools.

### Phase 3: Tokenizer hot paths

#### BENG ASCII

1. In src/storage/fts/ob_beng_ft_parser.cpp, extend ObBEngFTParser::segment and the ASCII token iterator. Target UTF8MB4_BIN and UTF8MB4_GENERAL_CI only, using the same character-classification rules as the existing parser.
2. Replace per-character repeated classification and normalization with a byte-class table and block-wise scanning. Evaluate SIMD only behind supported architecture/compiler guards; keep a portable scalar implementation.
3. Use a document-scoped stable token buffer with offsets so TOKENIZE, query parsing, and DDL indexing share the same tokenization implementation.
4. If any byte is NUL or non-ASCII, discard only uncommitted fast-path state and run the existing parser over the full input. Do not emit a partial ASCII prefix before fallback.
5. In src/storage/fts/ob_fts_parser_helper.cpp, preserve parser reuse and route the fast path through the common iterator contract. Keep non-ASCII, unsupported collations, and parser properties on the existing path.

#### IK candidate and arbitration

1. Keep src/storage/fts/ik/ob_ik_candidate_automaton.cpp as the immutable dictionary candidate source with its current memory budget and fallback behavior.
2. In src/storage/fts/ik/ob_ik_cjk_processor.cpp, replace per-hit linked candidate emission with a compact document-local candidate buffer. Record start offset, byte length, character count, and candidate type without copying dictionary words.
3. In src/storage/fts/ik/ob_ik_arbitrator.cpp and ob_ik_token.cpp/.h, implement arbitration over compact intervals while reproducing current SMART and MAX_WORD selection, stable output order, quantifier handling, stopwords, and CJK boundaries.
4. Reuse candidate and output buffers from the parser runtime. On allocation failure or unsupported configuration, fall back to the current arbitration implementation rather than silently changing output.
5. Do not change dictionary contents, parser configuration semantics, or the on-disk word representation.

#### TOKENIZE result construction

1. In src/sql/engine/expr/ob_expr_tokenize.cpp, inspect the cost of converting parser output to JSON separately from parser time.
2. If JSON shaping is material, write tokens directly to a reusable result buffer in the exact existing array/detail order and escaping format. Avoid a second token container only when output equivalence can be proved.
3. Do not cache input strings, TOKENIZE outputs, or MATCH query results between executions.

### Phase 4: No-score COUNT and post-lookup LIMIT

1. In src/sql/das/iter/ob_das_iter_utils.cpp, keep the current iterator composition and identify the exact physical-plan conditions for a simple global COUNT(*) with unscored MATCH. Exclude DISTINCT, grouping, score projection/filtering, ordering, and expressions that need each row.
2. Carry a count-only output mode through the relevant DAS definition/runtime interface in src/sql/das/ob_das_ir_define.h and the iterator construction code. The mode must be selected from the resolved physical plan, not SQL text matching.
3. In src/sql/das/iter/ob_das_text_retrieval_merge_iter.cpp, continue to merge and deduplicate candidate DOC_IDs. Do not count at posting-hit time because multiple terms can match the same document.
4. Accumulate the count only after the existing auxiliary/domain lookup and base-row visibility checks accept a document. Candidate merging must still use the same snapshot and match semantics.
5. Reuse the existing batched lookup path in src/sql/das/iter/ob_das_functional_lookup_iter.cpp and the lookup subtree created from ob_das_iter_utils.cpp. Add a count sink at the first point where a batch is known to be unique and visible; avoid materializing full projected rows when the plan needs only the count.
6. For LIMIT/OFFSET, preserve the current rule in ObDASIterUtils::create_text_retrieval_sub_tree: do not pass the limit down to posting merge before lookup. Add accepted-row accounting after visibility checks, stop when offset + limit valid unique rows have been accepted, and propagate end-of-stream through lookup and scan iterators.
7. Keep normal MATCH row output and relevance scoring on their existing paths. A count-only branch must not alter scores or candidate order in score-sensitive queries.
8. Include stale/deleted rows, duplicate term hits, empty queries, stopword-only queries, partitioned tables, offsets, and concurrent DML in correctness coverage.

### Phase 5: Tablet scheduling and conditional LOB batching

1. In the paired FTS build task, schedule work by estimated text bytes/posting count rather than only equal tablet count. Keep a fixed global memory budget and let sink backpressure throttle source readers.
2. Preserve deterministic DDL slice and row-index inputs for DOC_ID generation. Dynamic reassignment may move only work whose idempotent identity is stable.
3. Add LOB prefetch only if profiling of the benchmark/target workload shows external LOB reads are a significant fraction. Candidate implementation points are the FTS branch around ObTextStringHelper::read_real_string_data in src/sql/engine/table/ob_table_scan_op.cpp and the LOB read buffer lifecycle.
4. Do not broaden the change into general table-scan scheduling or ordinary index DDL.

## Correctness Coverage

Add tests to existing suites under tools/deploy/mysql_test/test_suite/fts_index/t and corresponding result files; do not create a separate test-only tools directory.

- Token output and fallback: extend beng_ascii.test, tokenize_function.test, parser_properties.test, cn_word.test, and ik_candidate_automaton.test. Compare the complete TOKENIZE result and order, including mixed ASCII/UTF-8, NUL, punctuation, case folding, empty input, stopwords, overlapping CJK words, and SMART/MAX_WORD.
- Build and paired-target equality: extend create_fts_index_afterward.test, offline_ddl_with_fts_index.test, online_ddl_on_table_with_fts_index.test, and partitioned_simple_query.test. Validate both auxiliary tables represent the same logical postings with the correct key order.
- DML and visibility: extend basic_dml.test and basic_dml_sequel.test. Cover insert/update/delete after build, deleted candidates, NULL/empty text, snapshot visibility, and index use after publication.
- Query semantics: extend simple_query.test, partitioned_simple_query.test, and no_score_count.test. Check complete document sets, score values where requested, COUNT equivalence, LIMIT/OFFSET results, duplicate terms, Boolean mode, and empty/stopword-only query behavior.
- Failure/retry: use the existing FTS DDL error-simulation/debug-sync facilities in the FTS build task to fail the inverted sink and doc-word sink independently. Verify retry leaves no duplicate or one-sided published result and that a second retry completes correctly.

## Performance Acceptance

1. Use the original benchmark SQL and exact baseline parameters: 20,000 rows, batch 500, 3,000 TOKENIZE rounds, 200 query rounds, 3 samples, warmup 30, SKIP_LOAD=0.
2. For each phase, first run only the affected existing FTS correctness suites. Then compare only affected original SQL with original-candidate-original on the same machine, build, resources, and data. If original drift exceeds 5%, repeat that comparison.
3. Record all nine scored metrics and compute class averages with the checked-in scoring rule. Do not accept a candidate because one metric improves if its class or overall mean regresses materially.
4. Keep the implementation path only when correctness passes and affected metrics show a stable positive direction. Revert a phase that fails correctness or has no positive effect before starting the next phase.
5. Run one complete benchmark after all accepted phases. The release gate is at least 65% overall mean improvement; the engineering target is at least 70%. Report category means and per-metric timings so platform variance is visible.
6. Do not change tools/benchmark/fts_large_bench.sh, fts_large_bench_gen.py, fts_large_bench_setup.sql, fts_large_bench_score.py, or fts_large_bench_baseline.json.

## Risks and Decisions

- The paired build is the highest-potential change, but it replaces a mature one-target DDL execution boundary. Land it only with paired retry, snapshot, DML catch-up, and publish semantics defined first.
- Building one auxiliary side from the other is an alternative prototype, not an additional stage. Compare its serialized critical path against the paired producer before choosing it.
- A custom FTS sorter can remove generic row overhead but can also lose parallelism or increase spill traffic. Keep independent sort sinks and benchmark sort/write separately.
- The checked-in baseline is an average of five CI runs. Local timings are not platform evidence unless the baseline environment is comparable; the previously shared metrics and mean-improvement field were inconsistent with this baseline.
- The 65% objective is a target, not a source-level guarantee. The plan is intentionally broad because the score weights build, tokenize, and query equally.

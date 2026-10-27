/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <cstddef>
#include <cstdint>
extern "C"
{
  void *seekdb_parquet_open(int fd, int (*check_status)(void *), void *context, char *error, size_t cap);
  void seekdb_parquet_close(void *scan);
  size_t seekdb_parquet_columns(void *scan);
  int64_t seekdb_parquet_rows(void *scan);
  const char *seekdb_parquet_column(void *scan, size_t column, int32_t *kind, bool *nullable);
  int seekdb_parquet_start(void *scan, const int64_t *columns, size_t count, char *error, size_t cap);
  int seekdb_parquet_next(void *scan, char *error, size_t cap);
  const char *seekdb_parquet_cell(void *scan, size_t column, bool *is_null, char *error, size_t cap);
}

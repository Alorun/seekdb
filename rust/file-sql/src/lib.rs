// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
//! Format decoding only. SQL binding, expressions and joins remain in seekdb.
#[cfg(all(target_os = "linux", panic = "abort"))]
compile_error!("File SQL requires guarded panic=unwind; build with rust/build.py");
#[cfg(target_os = "linux")]
mod linux {
    use arrow_array::{Array, RecordBatch};
    use arrow_cast::display::{ArrayFormatter, FormatOptions};
    use arrow_schema::{DataType, TimeUnit};
    use parquet::arrow::arrow_reader::{
        ArrowReaderOptions, ParquetRecordBatchReader, ParquetRecordBatchReaderBuilder,
    };
    use parquet::arrow::ProjectionMask;
    use parquet::format::{PageHeader, PageType};
    use parquet::thrift::TSerializable;
    use std::ffi::{c_char, CString};
    use std::fs::File;
    use std::io::{Cursor, Read, Seek, SeekFrom};
    use std::os::fd::FromRawFd;
    use std::ptr;
    use thrift::protocol::TCompactInputProtocol;

    const MAX_META: usize = 8 << 20;
    const MAX_PAGE: i32 = 16 << 20;
    const MAX_GROUP: i64 = 64 << 20;
    type Result<T> = std::result::Result<T, String>;

    fn guarded<T>(call: impl FnOnce() -> Result<T>) -> Result<T> {
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(call)).unwrap_or_else(|_| {
            Err("Parquet decoder rejected corrupt data (panic contained)".into())
        })
    }

    // Bound Thrift lengths/depth before handing metadata to the library. In
    // particular, a corrupt list length must not trigger an enormous allocation.
    struct Compact<'a> {
        b: &'a [u8],
        p: usize,
        work: usize,
    }
    impl Compact<'_> {
        fn byte(&mut self) -> Result<u8> {
            let v = *self.b.get(self.p).ok_or("truncated compact metadata")?;
            self.p += 1;
            Ok(v)
        }
        fn var(&mut self) -> Result<u64> {
            let mut v = 0;
            for i in 0..10 {
                let b = self.byte()?;
                if i == 9 && b > 1 {
                    return Err("invalid varint".into());
                }
                v |= ((b & 127) as u64) << (7 * i);
                if b < 128 {
                    return Ok(v);
                }
            }
            Err("overlong varint".into())
        }
        fn skip(&mut self, n: usize) -> Result<()> {
            self.p = self
                .p
                .checked_add(n)
                .filter(|v| *v <= self.b.len())
                .ok_or("truncated compact value")?;
            Ok(())
        }
        fn count(&mut self, n: u64) -> Result<usize> {
            if n > 65536 {
                return Err("metadata collection limit exceeded".into());
            }
            Ok(n as usize)
        }
        fn value(&mut self, t: u8, depth: usize, field: bool) -> Result<()> {
            self.work += 1;
            if depth > 32 || self.work > 1_000_000 {
                return Err("metadata complexity limit exceeded".into());
            }
            match t {
                1 | 2 => {
                    if !field {
                        self.byte()?;
                    }
                }
                3 => self.skip(1)?,
                4..=6 => {
                    self.var()?;
                }
                7 => self.skip(8)?,
                8 => {
                    let n = self.var()?;
                    if n > MAX_META as u64 {
                        return Err("metadata string limit exceeded".into());
                    }
                    self.skip(n as usize)?;
                }
                9 | 10 => {
                    let h = self.byte()?;
                    let n = if h >> 4 == 15 {
                        self.var()?
                    } else {
                        (h >> 4) as u64
                    };
                    let n = self.count(n)?;
                    for _ in 0..n {
                        self.value(h & 15, depth + 1, false)?;
                    }
                }
                11 => {
                    let n = self.var()?;
                    let n = self.count(n)?;
                    if n != 0 {
                        let h = self.byte()?;
                        for _ in 0..n {
                            self.value(h >> 4, depth + 1, false)?;
                            self.value(h & 15, depth + 1, false)?;
                        }
                    }
                }
                12 => loop {
                    let h = self.byte()?;
                    if h == 0 {
                        break;
                    }
                    if h >> 4 == 0 {
                        self.var()?;
                    }
                    self.value(h & 15, depth + 1, true)?;
                },
                _ => return Err("invalid compact type".into()),
            }
            Ok(())
        }
    }
    fn validate(bytes: &[u8]) -> Result<usize> {
        let mut c = Compact {
            b: bytes,
            p: 0,
            work: 0,
        };
        c.value(12, 0, true)?;
        Ok(c.p)
    }
    fn kind(t: &DataType) -> Result<i32> {
        Ok(match t {
            DataType::Null => 0,
            DataType::Int8
            | DataType::Int16
            | DataType::Int32
            | DataType::Int64
            | DataType::UInt8
            | DataType::UInt16
            | DataType::UInt32 => 1,
            DataType::Float32 | DataType::Float64 => 2,
            DataType::Boolean => 3,
            DataType::Utf8 | DataType::LargeUtf8 => 4,
            DataType::Date32 => 5,
            DataType::Timestamp(
                TimeUnit::Second | TimeUnit::Millisecond | TimeUnit::Microsecond,
                None,
            ) => 6,
            _ => return Err(format!("unsupported Parquet type: {t}")),
        })
    }
    fn check_encoding(encoding: parquet::format::Encoding) -> Result<()> {
        use parquet::format::Encoding as E;
        // Delta byte-array decoders allocate from embedded counts before
        // checking page counts. Reject them until those allocations are bounded.
        if matches!(
            encoding,
            E::PLAIN
                | E::PLAIN_DICTIONARY
                | E::RLE
                | E::RLE_DICTIONARY
                | E::BIT_PACKED
                | E::BYTE_STREAM_SPLIT
        ) {
            Ok(())
        } else {
            Err("unsupported Parquet encoding (delta encodings require bounded buffers)".into())
        }
    }
    struct Scan {
        file: File,
        names: Vec<CString>,
        kinds: Vec<i32>,
        nullable: Vec<bool>,
        rows: i64,
        reader: Option<ParquetRecordBatchReader>,
        batch: Option<RecordBatch>,
        row: usize,
        projection: Vec<usize>,
        text: CString,
    }
    fn open_file(mut file: File, check_status: &dyn Fn() -> Result<()>) -> Result<Scan> {
        check_status()?;
        let size = file.metadata().map_err(|e| e.to_string())?.len();
        if size < 12 {
            return Err("truncated Parquet file".into());
        }
        let mut magic = [0; 4];
        file.read_exact(&mut magic).map_err(|e| e.to_string())?;
        if &magic != b"PAR1" {
            return Err("invalid Parquet magic".into());
        }
        file.seek(SeekFrom::End(-8)).map_err(|e| e.to_string())?;
        let mut tail = [0; 8];
        file.read_exact(&mut tail).map_err(|e| e.to_string())?;
        if &tail[4..] != b"PAR1" {
            return Err("invalid Parquet footer magic".into());
        }
        let len = u32::from_le_bytes(tail[..4].try_into().unwrap()) as usize;
        if len > MAX_META || len as u64 > size - 12 {
            return Err("Parquet footer resource limit or invalid length".into());
        }
        file.seek(SeekFrom::End(-8 - len as i64))
            .map_err(|e| e.to_string())?;
        let mut footer = vec![0; len];
        file.read_exact(&mut footer).map_err(|e| e.to_string())?;
        validate(&footer)?;
        let builder = ParquetRecordBatchReaderBuilder::try_new_with_options(
            file.try_clone().map_err(|e| e.to_string())?,
            ArrowReaderOptions::new().with_skip_arrow_metadata(true),
        )
        .map_err(|e| e.to_string())?;
        let meta = builder.metadata();
        let schema = builder.schema();
        if schema.fields().is_empty()
            || schema.fields().len() > 1024
            || meta.num_row_groups() > 4096
        {
            return Err("Parquet schema or row-group resource limit".into());
        }
        if builder.parquet_schema().num_columns() != schema.fields().len() {
            return Err("nested Parquet schema unsupported".into());
        }
        let mut names = Vec::new();
        let mut kinds = Vec::new();
        let mut nullable = Vec::new();
        let mut used = std::collections::HashSet::new();
        for f in schema.fields() {
            if f.name().is_empty()
                || f.name().len() > 256
                || !used.insert(f.name().to_ascii_lowercase())
            {
                return Err("invalid or duplicate Parquet column name".into());
            }
            names.push(CString::new(f.name().as_str()).map_err(|_| "NUL in Parquet column name")?);
            kinds.push(kind(f.data_type())?);
            nullable.push(f.is_nullable());
        }
        // Validate page headers and ranges without decoding payloads. The actual
        // encodings/compression are implemented by Apache parquet, not this guard.
        let payload_end = size - 8 - len as u64;
        for rg in meta.row_groups() {
            check_status()?;
            if rg.num_rows() < 0 || rg.total_byte_size() < 0 || rg.total_byte_size() > MAX_GROUP {
                return Err("Parquet row group exceeds 64 MiB".into());
            }
            let mut decoded = 0i64;
            let mut compressed = 0i64;
            let mut pages = 0usize;
            for col in rg.columns() {
                if col.file_path().is_some_and(|p| !p.is_empty()) {
                    return Err("external Parquet column files unsupported".into());
                }
                if col.uncompressed_size() < 0 || col.uncompressed_size() > MAX_GROUP {
                    return Err("invalid Parquet column size".into());
                }
                // Arrow may fetch a complete column chunk before decoding its
                // pages. Bound compressed bytes independently of decoded bytes.
                compressed = compressed
                    .checked_add(col.compressed_size())
                    .ok_or("Parquet compressed size overflow")?;
                if col.compressed_size() < 0 || compressed > MAX_GROUP {
                    return Err("Parquet row group compressed size exceeds 64 MiB".into());
                }
                if col.data_page_offset() < 4 || col.dictionary_page_offset().is_some_and(|v| v < 4)
                {
                    return Err("invalid Parquet column offsets".into());
                }
                let (mut offset, bytes) = col.byte_range();
                let end = offset
                    .checked_add(bytes)
                    .filter(|v| *v <= payload_end)
                    .ok_or("invalid Parquet column range")?;
                let mut column_values = 0i64;
                while offset < end {
                    pages += 1;
                    if pages > 65536 {
                        return Err("Parquet row group page-count limit".into());
                    }
                    if pages.is_multiple_of(128) {
                        check_status()?;
                    }
                    file.seek(SeekFrom::Start(offset))
                        .map_err(|e| e.to_string())?;
                    let mut header = vec![0; ((end - offset) as usize).min(65536)];
                    file.read_exact(&mut header).map_err(|e| e.to_string())?;
                    let n = validate(&header)?;
                    let mut protocol = TCompactInputProtocol::new(Cursor::new(&header[..n]));
                    let h = PageHeader::read_from_in_protocol(&mut protocol)
                        .map_err(|e| e.to_string())?;
                    for encoding in [
                        h.data_page_header.as_ref().map(|p| p.encoding),
                        h.data_page_header_v2.as_ref().map(|p| p.encoding),
                        h.dictionary_page_header.as_ref().map(|p| p.encoding),
                    ]
                    .into_iter()
                    .flatten()
                    {
                        check_encoding(encoding)?;
                    }
                    if h.compressed_page_size < 0
                        || h.compressed_page_size > MAX_PAGE
                        || h.uncompressed_page_size < 0
                        || h.uncompressed_page_size > MAX_PAGE
                    {
                        return Err("Parquet page exceeds 16 MiB or has invalid length".into());
                    }
                    let values = h
                        .data_page_header
                        .as_ref()
                        .map(|p| p.num_values)
                        .or_else(|| h.data_page_header_v2.as_ref().map(|p| p.num_values))
                        .or_else(|| h.dictionary_page_header.as_ref().map(|p| p.num_values));
                    if values.is_some_and(|v| !(0..=1_000_000).contains(&v)) {
                        return Err("Parquet page value count limit".into());
                    }
                    let data_values = if h.type_ == PageType::DATA_PAGE {
                        h.data_page_header
                            .as_ref()
                            .ok_or("missing data page header")?
                            .num_values
                    } else if h.type_ == PageType::DATA_PAGE_V2 {
                        let p = h
                            .data_page_header_v2
                            .as_ref()
                            .ok_or("missing V2 data page header")?;
                        if p.num_rows != p.num_values
                            || p.num_nulls < 0
                            || p.num_nulls > p.num_values
                            || p.definition_levels_byte_length < 0
                            || p.repetition_levels_byte_length < 0
                            || i64::from(p.definition_levels_byte_length)
                                + i64::from(p.repetition_levels_byte_length)
                                > i64::from(h.uncompressed_page_size)
                        {
                            return Err("invalid flat V2 page counts or level lengths".into());
                        }
                        p.num_values
                    } else if h.type_ == PageType::DICTIONARY_PAGE
                        || h.type_ == PageType::INDEX_PAGE
                    {
                        0
                    } else {
                        return Err("unsupported Parquet page type".into());
                    };
                    column_values = column_values
                        .checked_add(i64::from(data_values))
                        .ok_or("Parquet page value count overflow")?;
                    decoded += h.uncompressed_page_size as i64;
                    if decoded > MAX_GROUP {
                        return Err("Parquet row group decoded size exceeds 64 MiB".into());
                    }
                    offset = offset
                        .checked_add(n as u64 + h.compressed_page_size as u64)
                        .filter(|v| *v <= end)
                        .ok_or("truncated Parquet page")?;
                }
                if column_values != rg.num_rows() {
                    return Err("Parquet page counts disagree with row-group footer".into());
                }
            }
        }
        check_status()?;
        let rows = meta.file_metadata().num_rows();
        if rows < 0
            || meta
                .row_groups()
                .iter()
                .try_fold(0i64, |sum, g| sum.checked_add(g.num_rows()))
                != Some(rows)
        {
            return Err("inconsistent Parquet row count".into());
        }
        Ok(Scan {
            file,
            names,
            kinds,
            nullable,
            rows,
            reader: None,
            batch: None,
            row: 0,
            projection: Vec::new(),
            text: CString::default(),
        })
    }
    fn message(buffer: *mut c_char, cap: usize, text: &str) {
        if cap == 0 || buffer.is_null() {
            return;
        }
        let n = text.len().min(cap - 1);
        unsafe {
            ptr::copy_nonoverlapping(text.as_ptr(), buffer.cast::<u8>(), n);
            *buffer.add(n) = 0;
        }
    }
    /// Takes ownership of a duplicated read-only regular-file descriptor.
    /// # Safety
    /// The caller must provide valid writable output buffers and a live handle from
    /// `seekdb_parquet_open`. Handles must not be used concurrently or after close.
    /// The open function takes ownership of its fd; close consumes its handle once.
    #[no_mangle]
    pub unsafe extern "C" fn seekdb_parquet_open(
        fd: i32,
        check_status: Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> i32>,
        context: *mut std::ffi::c_void,
        error: *mut c_char,
        cap: usize,
    ) -> *mut std::ffi::c_void {
        let check = || {
            if let Some(callback) = check_status {
                if callback(context) != 0 {
                    return Err("Parquet validation cancelled by query status".into());
                }
            }
            Ok(())
        };
        match guarded(|| open_file(File::from_raw_fd(fd), &check)) {
            Ok(s) => Box::into_raw(Box::new(s)).cast(),
            Err(e) => {
                message(error, cap, &e);
                ptr::null_mut()
            }
        }
    }
    /// # Safety
    /// The caller must provide valid writable output buffers and a live handle from
    /// `seekdb_parquet_open`. Handles must not be used concurrently or after close.
    /// The open function takes ownership of its fd; close consumes its handle once.
    #[no_mangle]
    pub unsafe extern "C" fn seekdb_parquet_close(s: *mut std::ffi::c_void) {
        if !s.is_null() {
            drop(Box::from_raw(s.cast::<Scan>()));
        }
    }
    /// # Safety
    /// The caller must provide valid writable output buffers and a live handle from
    /// `seekdb_parquet_open`. Handles must not be used concurrently or after close.
    /// The open function takes ownership of its fd; close consumes its handle once.
    #[no_mangle]
    pub unsafe extern "C" fn seekdb_parquet_columns(s: *mut std::ffi::c_void) -> usize {
        (*s.cast::<Scan>()).names.len()
    }
    /// # Safety
    /// The caller must provide valid writable output buffers and a live handle from
    /// `seekdb_parquet_open`. Handles must not be used concurrently or after close.
    /// The open function takes ownership of its fd; close consumes its handle once.
    #[no_mangle]
    pub unsafe extern "C" fn seekdb_parquet_rows(s: *mut std::ffi::c_void) -> i64 {
        (*s.cast::<Scan>()).rows
    }
    /// # Safety
    /// The caller must provide valid writable output buffers and a live handle from
    /// `seekdb_parquet_open`. Handles must not be used concurrently or after close.
    /// The open function takes ownership of its fd; close consumes its handle once.
    #[no_mangle]
    pub unsafe extern "C" fn seekdb_parquet_column(
        s: *mut std::ffi::c_void,
        i: usize,
        kind: *mut i32,
        nullable: *mut bool,
    ) -> *const c_char {
        let s = &*s.cast::<Scan>();
        if i >= s.names.len() {
            return ptr::null();
        }
        *kind = s.kinds[i];
        *nullable = s.nullable[i];
        s.names[i].as_ptr()
    }
    /// # Safety
    /// The caller must provide valid writable output buffers and a live handle from
    /// `seekdb_parquet_open`. Handles must not be used concurrently or after close.
    /// The open function takes ownership of its fd; close consumes its handle once.
    #[no_mangle]
    pub unsafe extern "C" fn seekdb_parquet_start(
        s: *mut std::ffi::c_void,
        columns: *const i64,
        count: usize,
        error: *mut c_char,
        cap: usize,
    ) -> i32 {
        let s = &mut *s.cast::<Scan>();
        let result = guarded(|| -> Result<()> {
            s.projection = if count == 0 {
                Vec::new()
            } else {
                std::slice::from_raw_parts(columns, count)
                    .iter()
                    .map(|i| *i as usize)
                    .collect()
            };
            s.projection.sort_unstable();
            s.projection.dedup();
            if s.projection.iter().any(|i| *i >= s.names.len()) {
                return Err("invalid Parquet projection".into());
            }
            let b = ParquetRecordBatchReaderBuilder::try_new_with_options(
                s.file.try_clone().map_err(|e| e.to_string())?,
                ArrowReaderOptions::new().with_skip_arrow_metadata(true),
            )
            .map_err(|e| e.to_string())?;
            let mask = ProjectionMask::leaves(b.parquet_schema(), s.projection.clone());
            // A tiny dictionary can expand to huge repeated strings. Decode
            // string projections one row at a time; the native scanner still
            // batches rows under its byte limit. Fixed-width/count scans use 128.
            let batch_size = if s.projection.iter().any(|i| s.kinds[*i] == 4) {
                1
            } else {
                128
            };
            s.reader = Some(
                b.with_batch_size(batch_size)
                    .with_projection(mask)
                    .build()
                    .map_err(|e| e.to_string())?,
            );
            s.batch = None;
            s.row = 0;
            Ok(())
        });
        match result {
            Ok(()) => 0,
            Err(e) => {
                message(error, cap, &e);
                -1
            }
        }
    }
    /// # Safety
    /// The caller must provide valid writable output buffers and a live handle from
    /// `seekdb_parquet_open`. Handles must not be used concurrently or after close.
    /// The open function takes ownership of its fd; close consumes its handle once.
    #[no_mangle]
    pub unsafe extern "C" fn seekdb_parquet_next(
        s: *mut std::ffi::c_void,
        error: *mut c_char,
        cap: usize,
    ) -> i32 {
        let s = &mut *s.cast::<Scan>();
        let result = guarded(|| {
            if s.batch.as_ref().is_some_and(|b| s.row + 1 < b.num_rows()) {
                s.row += 1;
                return Ok(0);
            }
            loop {
                match s.reader.as_mut().and_then(Iterator::next) {
                    Some(Ok(b)) => {
                        if b.num_rows() == 0 {
                            continue;
                        }
                        s.batch = Some(b);
                        s.row = 0;
                        return Ok(0);
                    }
                    Some(Err(e)) => return Err(e.to_string()),
                    None => {
                        s.batch = None;
                        return Ok(1);
                    }
                }
            }
        });
        match result {
            Ok(status) => status,
            Err(e) => {
                message(error, cap, &e);
                -1
            }
        }
    }
    /// # Safety
    /// The caller must provide valid writable output buffers and a live handle from
    /// `seekdb_parquet_open`. Handles must not be used concurrently or after close.
    /// The open function takes ownership of its fd; close consumes its handle once.
    #[no_mangle]
    pub unsafe extern "C" fn seekdb_parquet_cell(
        s: *mut std::ffi::c_void,
        column: usize,
        is_null: *mut bool,
        error: *mut c_char,
        cap: usize,
    ) -> *const c_char {
        let s = &mut *s.cast::<Scan>();
        let result = guarded(|| -> Result<Option<CString>> {
            let Some(projected) = s.projection.iter().position(|i| *i == column) else {
                return Ok(None);
            };
            let b = s.batch.as_ref().ok_or("no Parquet batch")?;
            let array = b.column(projected);
            if array.is_null(s.row) {
                return Ok(None);
            }
            if array.data_type() == &DataType::Float32 {
                let value = array
                    .as_any()
                    .downcast_ref::<arrow_array::Float32Array>()
                    .ok_or("invalid FLOAT array")?
                    .value(s.row);
                return Ok(Some(
                    CString::new((value as f64).to_string()).map_err(|_| "invalid FLOAT text")?,
                ));
            }
            let options = FormatOptions::default().with_display_error(false);
            let formatter =
                ArrayFormatter::try_new(array.as_ref(), &options).map_err(|e| e.to_string())?;
            let mut text = String::new();
            formatter
                .value(s.row)
                .write(&mut text)
                .map_err(|e| e.to_string())?;
            if text.len() > 8 << 20 {
                return Err("Parquet field exceeds 8 MiB".into());
            }
            Ok(Some(CString::new(text).map_err(|_| "NUL in Parquet text")?))
        });
        match result {
            Ok(None) => {
                *is_null = true;
                ptr::null()
            }
            Ok(Some(v)) => {
                s.text = v;
                *is_null = false;
                s.text.as_ptr()
            }
            Err(e) => {
                message(error, cap, &e);
                ptr::null()
            }
        }
    }

    #[cfg(test)]
    mod tests {
        use super::*;
        use arrow_array::{Int64Array, StringArray};
        use arrow_schema::{Field, Schema};
        use parquet::arrow::ArrowWriter;
        use parquet::basic::Compression;
        use parquet::file::properties::WriterProperties;
        use std::sync::Arc;

        #[test]
        fn snappy_multiple_groups_projection_nulls_and_count() {
            let path =
                std::env::temp_dir().join(format!("seekdb-parquet-{}.parquet", std::process::id()));
            let schema = Arc::new(Schema::new(vec![
                Field::new("id", DataType::Int64, true),
                Field::new("name", DataType::Utf8, true),
            ]));
            let props = WriterProperties::builder()
                .set_compression(Compression::SNAPPY)
                .set_max_row_group_size(2)
                .build();
            let mut writer =
                ArrowWriter::try_new(File::create(&path).unwrap(), schema.clone(), Some(props))
                    .unwrap();
            let batch = RecordBatch::try_new(
                schema,
                vec![
                    Arc::new(Int64Array::from(vec![
                        Some(1),
                        None,
                        Some(3),
                        Some(4),
                        Some(5),
                    ])),
                    Arc::new(StringArray::from(vec![
                        Some("one"),
                        Some("two"),
                        None,
                        Some("four"),
                        Some("five"),
                    ])),
                ],
            )
            .unwrap();
            writer.write(&batch).unwrap();
            writer.close().unwrap();
            if let Some(fixture) = std::env::var_os("SEEKDB_FILE_SQL_PARQUET_FIXTURE") {
                std::fs::copy(&path, fixture).unwrap();
            }
            let scan = open_file(File::open(&path).unwrap(), &|| Ok(())).unwrap();
            assert_eq!(scan.rows, 5);
            assert_eq!(scan.names.len(), 2);
            let handle = Box::into_raw(Box::new(scan)).cast();
            let mut error = [0i8; 512];
            unsafe {
                let projection = [0i64];
                assert_eq!(
                    seekdb_parquet_start(handle, projection.as_ptr(), 1, error.as_mut_ptr(), 512),
                    0
                );
                let mut rows = 0;
                let mut nulls = 0;
                while seekdb_parquet_next(handle, error.as_mut_ptr(), 512) == 0 {
                    let mut null = false;
                    let v = seekdb_parquet_cell(handle, 0, &mut null, error.as_mut_ptr(), 512);
                    if null {
                        nulls += 1;
                    } else {
                        assert!(!v.is_null());
                    }
                    rows += 1;
                }
                assert_eq!(rows, 5);
                assert_eq!(nulls, 1);
                assert_eq!(
                    seekdb_parquet_start(handle, ptr::null(), 0, error.as_mut_ptr(), 512),
                    0
                );
                let mut rows = 0;
                while seekdb_parquet_next(handle, error.as_mut_ptr(), 512) == 0 {
                    rows += 1;
                }
                assert_eq!(rows, 5);
                let projection = [1i64];
                assert_eq!(
                    seekdb_parquet_start(handle, projection.as_ptr(), 1, error.as_mut_ptr(), 512),
                    0
                );
                let mut strings = Vec::new();
                while seekdb_parquet_next(handle, error.as_mut_ptr(), 512) == 0 {
                    assert_eq!(
                        (*handle.cast::<Scan>()).batch.as_ref().unwrap().num_rows(),
                        1
                    );
                    let mut null = false;
                    let value = seekdb_parquet_cell(handle, 1, &mut null, error.as_mut_ptr(), 512);
                    strings.push(if null {
                        None
                    } else {
                        Some(std::ffi::CStr::from_ptr(value).to_str().unwrap().to_owned())
                    });
                }
                assert_eq!(
                    strings,
                    [
                        Some("one".into()),
                        Some("two".into()),
                        None,
                        Some("four".into()),
                        Some("five".into())
                    ]
                );
                seekdb_parquet_close(handle);
            }
            // A coordinated footer row-count change must also fail COUNT(*) with
            // an empty projection; column data-page counts are independent evidence.
            let original = std::fs::read(&path).unwrap();
            // This invalid level length used to panic in Bytes::slice. Exercise
            // the actual C boundary, which must return an error and stay alive.
            let mut damaged = original.clone();
            damaged[47] ^= 255;
            std::fs::write(&path, damaged).unwrap();
            unsafe {
                use std::os::fd::IntoRawFd;
                let handle = seekdb_parquet_open(
                    File::open(&path).unwrap().into_raw_fd(),
                    None,
                    ptr::null_mut(),
                    error.as_mut_ptr(),
                    error.len(),
                );
                assert!(!handle.is_null());
                let projection = [0i64, 1];
                let mut status = seekdb_parquet_start(
                    handle,
                    projection.as_ptr(),
                    projection.len(),
                    error.as_mut_ptr(),
                    error.len(),
                );
                while status == 0 {
                    status = seekdb_parquet_next(handle, error.as_mut_ptr(), error.len());
                }
                assert_eq!(status, -1);
                seekdb_parquet_close(handle);
            }
            let len = u32::from_le_bytes(
                original[original.len() - 8..original.len() - 4]
                    .try_into()
                    .unwrap(),
            ) as usize;
            let start = original.len() - 8 - len;
            let mut protocol =
                TCompactInputProtocol::new(Cursor::new(&original[start..original.len() - 8]));
            let mut metadata =
                parquet::format::FileMetaData::read_from_in_protocol(&mut protocol).unwrap();
            let write_metadata = |metadata: &parquet::format::FileMetaData| {
                let mut footer = Vec::new();
                let mut protocol = parquet::thrift::TCompactOutputProtocol::new(&mut footer);
                metadata.write_to_out_protocol(&mut protocol).unwrap();
                let mut corrupt = original[..start].to_vec();
                corrupt.extend(&footer);
                corrupt.extend((footer.len() as u32).to_le_bytes());
                corrupt.extend(b"PAR1");
                std::fs::write(&path, corrupt).unwrap();
            };
            let size = metadata.row_groups[0].columns[0]
                .meta_data
                .as_ref()
                .unwrap()
                .total_compressed_size;
            metadata.row_groups[0].columns[0]
                .meta_data
                .as_mut()
                .unwrap()
                .total_compressed_size = MAX_GROUP + 1;
            write_metadata(&metadata);
            assert!(open_file(File::open(&path).unwrap(), &|| Ok(()))
                .err()
                .unwrap()
                .contains("compressed size"));
            metadata.row_groups[0].columns[0]
                .meta_data
                .as_mut()
                .unwrap()
                .total_compressed_size = size;
            metadata.num_rows += 1;
            metadata.row_groups[0].num_rows += 1;
            write_metadata(&metadata);
            assert!(open_file(File::open(&path).unwrap(), &|| Ok(())).is_err());
            assert!(open_file(File::open(&path).unwrap(), &|| Err("cancelled".into())).is_err());
            std::fs::remove_file(path).unwrap();
        }
        #[test]
        fn rejects_unbounded_or_truncated_compact_lengths() {
            assert!(validate(&[0x19, 0xf5, 0xff, 0xff, 0xff, 0xff, 0x07]).is_err());
            assert!(validate(&[0x18, 0xff, 0xff, 0xff, 0xff, 0x07]).is_err());
            assert!(validate(&[0x1c, 0x1c]).is_err());
            assert_eq!(validate(&[0]).unwrap(), 1);
            assert!(check_encoding(parquet::format::Encoding::DELTA_BYTE_ARRAY).is_err());
            assert!(check_encoding(parquet::format::Encoding::DELTA_LENGTH_BYTE_ARRAY).is_err());
        }
    }
}

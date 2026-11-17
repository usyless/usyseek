//! usyseek native bridge: Qdrant Edge embedded engine + HuggingFace tokenizer + resvg, exposed over a small C ABI.
//! All structured data crosses the boundary as UTF-8 JSON; bulk numeric data (vectors, ids, pixels) as raw slices.

mod edge_common;
mod svg;

use edge_common::{create_edge_shard, sanitize_collection_name, shard_exists_on_disk};

use std::collections::HashMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::ptr;
use std::slice;
use std::sync::{Arc, Mutex};

use qdrant_edge::{
    AnyVariants, Condition, CountRequestBuilder, Direction, EdgeShard, FacetRequestBuilder,
    FacetValue, FieldCondition, Filter, JsonPath, Match, NamedQuery, OrderBy, OrderByInterface,
    Payload, PayloadOps, PointId, PointInsertOperations, PointOperations, PointStruct,
    PointStructPersisted, QueryEnum, QueryRequestBuilder, RetrieveRequestBuilder, ScoringQuery,
    ScrollRequestBuilder, SearchParams, SetPayloadOp, UpdateOperation, ValueVariants,
    VectorInternal, VectorStructInternal, WithPayloadInterface, WithVector, DEFAULT_VECTOR_NAME,
};
use serde_json::{Map as JsonMap, Value as Json};
use tokenizers::{PaddingParams, PaddingStrategy, Tokenizer, TruncationParams};

// ---------------------------------------------------------------------------------------------
// ABI helpers
// ---------------------------------------------------------------------------------------------

#[repr(C)]
pub struct UsyStr {
    pub ptr: *const u8,
    pub len: usize,
}

impl UsyStr {
    fn as_str(&self) -> &str {
        if self.ptr.is_null() || self.len == 0 {
            return "";
        }
        std::str::from_utf8(unsafe { slice::from_raw_parts(self.ptr, self.len) }).unwrap_or("")
    }
}

/// Owned buffer handed to C++; must be released with `usy_buf_free`.
#[repr(C)]
pub struct UsyBuf {
    pub ptr: *mut u8,
    pub len: usize,
    pub cap: usize,
}

impl UsyBuf {
    fn empty() -> Self {
        Self { ptr: ptr::null_mut(), len: 0, cap: 0 }
    }
    fn from_string(s: String) -> Self {
        Self::from_bytes(s.into_bytes())
    }
    fn from_bytes(mut v: Vec<u8>) -> Self {
        let out = Self { ptr: v.as_mut_ptr(), len: v.len(), cap: v.capacity() };
        std::mem::forget(v);
        out
    }
}

unsafe fn slice_from<'a, T>(ptr: *const T, len: usize) -> Result<&'a [T], String> {
    if len == 0 {
        Ok(&[])
    } else if ptr.is_null() {
        Err("null slice pointer with non-zero length".into())
    } else {
        Ok(unsafe { slice::from_raw_parts(ptr, len) })
    }
}

unsafe fn qdrant_ref<'a>(q: *mut UsyQdrant) -> Result<&'a UsyQdrant, String> {
    if q.is_null() {
        Err("null qdrant handle".into())
    } else {
        Ok(unsafe { &*q })
    }
}

/// Frees a buffer previously allocated by Rust and returned via `UsyBuf`.
///
/// # Safety
///
/// `b` must either have `b.ptr == null` or originate from a Rust `UsyBuf` returned by this library and not yet freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_buf_free(b: UsyBuf) {
    if !b.ptr.is_null() {
        drop(unsafe { Vec::from_raw_parts(b.ptr, b.len, b.cap) });
    }
}

unsafe fn write_out(out: *mut UsyBuf, s: String) {
    if !out.is_null() {
        unsafe { *out = UsyBuf::from_string(s) };
    }
}

unsafe fn write_bytes(out: *mut UsyBuf, v: Vec<u8>) {
    if !out.is_null() {
        unsafe { *out = UsyBuf::from_bytes(v) };
    }
}

/// Runs `f`, writing either its JSON result or the error message into `out`. Returns 0 on success.
fn run<F>(out: *mut UsyBuf, f: F) -> i32
where
    F: FnOnce() -> Result<String, String>,
{
    match f() {
        Ok(s) => {
            unsafe { write_out(out, s) };
            0
        }
        Err(e) => {
            unsafe { write_out(out, e) };
            1
        }
    }
}

/// Runs `f`, writing either raw JSON bytes or the error message into `out`. Returns 0 on success.
fn run_bytes<F>(out: *mut UsyBuf, f: F) -> i32
where
    F: FnOnce() -> Result<Vec<u8>, String>,
{
    match f() {
        Ok(v) => {
            unsafe { write_bytes(out, v) };
            0
        }
        Err(e) => {
            unsafe { write_out(out, e) };
            1
        }
    }
}

fn err<E: std::fmt::Display>(e: E) -> String {
    e.to_string()
}

// ---------------------------------------------------------------------------------------------
// Qdrant Edge
// ---------------------------------------------------------------------------------------------

pub struct UsyQdrant {
    root: PathBuf,
    shards: Mutex<HashMap<String, Arc<EdgeShard>>>,
}

const F_NAME: &str = "n";
const F_ANCESTORS: &str = "a";
const F_TAGS: &str = "t";
const F_KIND: &str = "k";
const F_MTIME: &str = "m";

impl UsyQdrant {
    fn collection_dir(&self, name: &str) -> Result<PathBuf, String> {
        Ok(self.root.join(sanitize_collection_name(name)?.as_ref()))
    }

    fn get_or_load_shard(&self, name: &str) -> Result<Arc<EdgeShard>, String> {
        let dir = self.collection_dir(name)?;
        let mut shards = self.shards.lock().map_err(|_| "shard lock poisoned".to_string())?;
        if let Some(shard) = shards.get(name) {
            return Ok(Arc::clone(shard));
        }
        if !shard_exists_on_disk(&dir) {
            return Err(format!("collection '{name}' does not exist"));
        }
        // Keep at most the active model's shard mapped so switching models doesn't hold stale mmaps open.
        shards.clear();
        let shard = Arc::new(EdgeShard::load(&dir, None).map_err(err)?);
        shards.insert(name.to_owned(), Arc::clone(&shard));
        Ok(shard)
    }

    fn flush_target(&self, name: &str) -> Result<(), String> {
        let targets: Vec<Arc<EdgeShard>> = {
            let shards = self.shards.lock().map_err(|_| "shard lock poisoned".to_string())?;
            if name.is_empty() {
                shards.values().cloned().collect()
            } else if let Some(s) = shards.get(name) {
                vec![Arc::clone(s)]
            } else {
                Vec::new()
            }
        };
        for shard in targets {
            shard.flush().map_err(err)?;
        }
        Ok(())
    }

    fn close_all(&self) -> Result<(), String> {
        let drained: Vec<(String, Arc<EdgeShard>)> = {
            let mut shards = self.shards.lock().map_err(|_| "shard lock poisoned".to_string())?;
            shards.drain().collect()
        };
        let mut first_err: Option<String> = None;
        for (_, shard) in drained {
            if let Err(e) = shard.flush()
                && first_err.is_none()
            {
                first_err = Some(e.to_string());
            }
            drop(shard);
        }
        if let Some(e) = first_err {
            return Err(e);
        }
        Ok(())
    }
}

fn write_payload_json(buf: &mut Vec<u8>, p: Option<Payload>) -> Result<(), String> {
    match p {
        Some(p) => serde_json::to_writer(buf, &p.0).map_err(err),
        None => {
            buf.extend_from_slice(b"{}");
            Ok(())
        }
    }
}

fn id_to_u64(id: PointId) -> u64 {
    match id {
        PointId::NumId(n) => n,
        PointId::Uuid(_) => 0,
    }
}

fn json_path(s: &str) -> Result<JsonPath, String> {
    s.parse::<JsonPath>().map_err(|_| format!("invalid json path '{s}'"))
}

fn field_match(field: &str, m: Match) -> Result<Condition, String> {
    Ok(Condition::Field(FieldCondition::new_match(json_path(field)?, m)))
}

/// Filter description:
/// { "kind": 0|1, "tags_all": [..], "tags_none": [..], "dirs": [..] (any), "names": [..] (all, prefix text) }
fn build_filter(f: Option<&Json>) -> Result<Option<Filter>, String> {
    let Some(f) = f else { return Ok(None) };
    let mut must: Vec<Condition> = Vec::new();
    let mut must_not: Vec<Condition> = Vec::new();

    if let Some(k) = f.get("kind").and_then(|k| k.as_i64()) {
        must.push(field_match(F_KIND, Match::new_value(ValueVariants::Integer(k)))?);
    }
    if let Some(arr) = f.get("tags_all").and_then(|x| x.as_array()) {
        for t in arr.iter().filter_map(|s| s.as_str()) {
            must.push(field_match(F_TAGS, Match::new_value(ValueVariants::String(t.to_owned())))?);
        }
    }
    if let Some(arr) = f.get("tags_none").and_then(|x| x.as_array())
        && arr.iter().any(|s| s.as_str().is_some())
    {
        must_not.push(field_match(
            F_TAGS,
            Match::new_any(AnyVariants::Strings(
                arr.iter().filter_map(|s| s.as_str().map(str::to_owned)).collect(),
            )),
        )?);
    }
    if let Some(arr) = f.get("dirs").and_then(|x| x.as_array())
        && arr.iter().any(|s| s.as_str().is_some())
    {
        must.push(field_match(
            F_ANCESTORS,
            Match::new_any(AnyVariants::Strings(
                arr.iter().filter_map(|s| s.as_str().map(str::to_owned)).collect(),
            )),
        )?);
    }
    if let Some(arr) = f.get("names").and_then(|x| x.as_array()) {
        for n in arr.iter().filter_map(|s| s.as_str()) {
            must.push(field_match(F_NAME, Match::new_text(n))?);
        }
    }

    if must.is_empty() && must_not.is_empty() {
        return Ok(None);
    }
    Ok(Some(Filter {
        should: None,
        min_should: None,
        must: (!must.is_empty()).then_some(must),
        must_not: (!must_not.is_empty()).then_some(must_not),
    }))
}

fn payload_selector(fields: Option<&Json>) -> Result<WithPayloadInterface, String> {
    let Some(arr) = fields.and_then(|x| x.as_array()) else {
        return Ok(WithPayloadInterface::Bool(true));
    };
    let paths: Vec<JsonPath> = arr
        .iter()
        .filter_map(|s| s.as_str())
        .map(json_path)
        .collect::<Result<Vec<_>, _>>()?;
    if paths.is_empty() {
        Ok(WithPayloadInterface::Bool(true))
    } else {
        Ok(WithPayloadInterface::Fields(paths))
    }
}

/// Opens a local Qdrant Edge storage root directory and returns an owned handle, or null with error text in `out`.
///
/// # Safety
///
/// `dir` (when non-empty) must point to valid readable bytes; `out` may be null or a valid writable `UsyBuf`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_open(dir: UsyStr, out: *mut UsyBuf) -> *mut UsyQdrant {
    let dir = dir.as_str();
    if dir.is_empty() {
        unsafe { write_out(out, "empty database directory".into()) };
        return ptr::null_mut();
    }
    let root = PathBuf::from(dir);
    if let Err(e) = fs::create_dir_all(&root) {
        unsafe { write_out(out, e.to_string()) };
        return ptr::null_mut();
    }
    Box::into_raw(Box::new(UsyQdrant {
        root,
        shards: Mutex::new(HashMap::new()),
    }))
}

/// Flushes and closes all open edge shards, releasing file locks without freeing the `UsyQdrant` handle itself.
///
/// # Safety
///
/// `q` must be null or valid; `out` must be null or uniquely writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_close_all(q: *mut UsyQdrant, out: *mut UsyBuf) -> i32 {
    run(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        q.close_all()?;
        Ok("ok".to_owned())
    })
}

/// Frees a `UsyQdrant` handle returned by `usy_qd_open` (flushing any still-open shards first).
///
/// # Safety
///
/// `q` must be null or a live pointer returned by `usy_qd_open` that has not yet been freed or used concurrently.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_free(q: *mut UsyQdrant) {
    if !q.is_null() {
        let boxed = unsafe { Box::from_raw(q) };
        let _ = boxed.close_all();
        drop(boxed);
    }
}

/// Flushes WAL and segments to disk for `name` (or all open shards when `name` is empty).
///
/// # Safety
///
/// `q` must be null or valid; `name`/`out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_flush(q: *mut UsyQdrant, name: UsyStr, out: *mut UsyBuf) -> i32 {
    let name = name.as_str();
    run(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        q.flush_target(name)?;
        Ok("ok".to_owned())
    })
}

/// Synchronously runs Qdrant Edge optimizers and flushes the shard for `name`.
///
/// # Safety
///
/// `q` must be null or valid; `name`/`out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_optimize(q: *mut UsyQdrant, name: UsyStr, out: *mut UsyBuf) -> i32 {
    let name = name.as_str();
    run(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let dir = q.collection_dir(name)?;
        if !shard_exists_on_disk(&dir) {
            return Ok("ok".to_owned());
        }
        let shard = q.get_or_load_shard(name)?;
        shard.optimize().map_err(err)?;
        shard.flush().map_err(err)?;
        Ok("ok".to_owned())
    })
}

/// Ensures `name` exists. When `dim == 0` and the collection does not exist, returns `2` without creating it.
///
/// # Safety
///
/// `q` must be null or valid; `name` (when non-empty) must point to valid readable bytes; `out` must be null or uniquely writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_ensure_collection(q: *mut UsyQdrant, name: UsyStr, dim: u64, out: *mut UsyBuf) -> i32 {
    let Ok(q) = (unsafe { qdrant_ref(q) }) else {
        unsafe { write_out(out, "null qdrant handle".into()) };
        return 1;
    };
    let name = name.as_str();
    let res: Result<String, (i32, String)> = (|| {
        let dir = q.collection_dir(name).map_err(|e| (1, e))?;
        let mut shards = q.shards.lock().map_err(|_| (1, "shard lock poisoned".to_owned()))?;
        if shards.contains_key(name) {
            return Ok("exists".to_owned());
        }
        if shard_exists_on_disk(&dir) {
            shards.clear();
            let shard = Arc::new(EdgeShard::load(&dir, None).map_err(|e| (1, err(e)))?);
            shards.insert(name.to_owned(), shard);
            return Ok("exists".to_owned());
        }
        if dim == 0 {
            return Err((2, "collection does not exist and requires dims".to_owned()));
        }

        shards.clear();
        let shard = create_edge_shard(&dir, dim as usize).map_err(|e| (1, e))?;

        shards.insert(name.to_owned(), Arc::new(shard));
        Ok("created".to_owned())
    })();
    match res {
        Ok(s) => {
            unsafe { write_out(out, s) };
            0
        }
        Err((code, msg)) => {
            unsafe { write_out(out, msg) };
            code
        }
    }
}

/// Verifies that collection `name` either does not exist on disk yet or loads cleanly into memory.
///
/// # Safety
///
/// `q` must be null or valid; `name`/`out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_check_collection(q: *mut UsyQdrant, name: UsyStr, out: *mut UsyBuf) -> i32 {
    let name = name.as_str();
    run(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let dir = q.collection_dir(name)?;
        if !shard_exists_on_disk(&dir) {
            return Ok("missing".to_owned());
        }
        let _ = q.get_or_load_shard(name)?;
        Ok("ok".to_owned())
    })
}

/// payloads: JSON array (one object per point, same order as ids).
///
/// # Safety
///
/// `q` must be null or valid; when `count > 0`, `ids` must point to `count` `u64`s and `vectors` to `count * dim` `f32`s; `name`/`payloads`/`out` must be valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_upsert(
    q: *mut UsyQdrant,
    name: UsyStr,
    ids: *const u64,
    count: usize,
    vectors: *const f32,
    dim: usize,
    payloads: UsyStr,
    out: *mut UsyBuf,
) -> i32 {
    let name = name.as_str();
    let payloads = payloads.as_str();
    run(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let total_floats = count.checked_mul(dim).ok_or_else(|| "vector slice size overflow".to_string())?;
        let ids = unsafe { slice_from(ids, count) }?;
        let vectors = unsafe { slice_from(vectors, total_floats) }?;
        if count == 0 {
            return Ok("ok".to_owned());
        }
        let arr: Vec<Json> = serde_json::from_str(payloads).map_err(err)?;
        if arr.len() != count {
            return Err("payload count mismatch".into());
        }
        let shard = q.get_or_load_shard(name)?;
        let points: Vec<PointStructPersisted> = arr
            .into_iter()
            .enumerate()
            .map(|(i, p)| {
                let obj = if p.is_object() { p } else { Json::Object(JsonMap::new()) };
                PointStruct::new(ids[i], vectors[i * dim..(i + 1) * dim].to_vec(), obj).into()
            })
            .collect();
        shard
            .update(UpdateOperation::PointOperation(PointOperations::UpsertPoints(
                PointInsertOperations::PointsList(points),
            )))
            .map_err(err)?;
        Ok("ok".to_owned())
    })
}

/// request: { "limit", "offset", "threshold", "filter", "fields": [..], "id": u64 (query by stored point) }
/// vector/dim: optional query vector (dim == 0 -> use "id").
///
/// # Safety
///
/// `q` must be null or valid; when `dim > 0` and `vector` is non-null, `vector` must point to `dim` `f32`s; `name`/`request`/`out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_query(
    q: *mut UsyQdrant,
    name: UsyStr,
    vector: *const f32,
    dim: usize,
    request: UsyStr,
    out: *mut UsyBuf,
) -> i32 {
    let name = name.as_str();
    let request = request.as_str();
    run_bytes(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let mut vector: Vec<f32> = if dim > 0 && !vector.is_null() {
            unsafe { slice_from(vector, dim) }?.to_vec()
        } else {
            Vec::new()
        };
        let req: Json = serde_json::from_str(request).map_err(err)?;
        let limit = req.get("limit").and_then(|v| v.as_u64()).unwrap_or(100) as usize;
        let offset = req.get("offset").and_then(|v| v.as_u64()).unwrap_or(0) as usize;

        let shard = q.get_or_load_shard(name)?;
        if vector.is_empty() {
            if let Some(id) = req.get("id").and_then(|v| v.as_u64()) {
                let records = shard
                    .retrieve(
                        RetrieveRequestBuilder::new(vec![PointId::NumId(id)])
                            .with_payload(WithPayloadInterface::Bool(false))
                            .with_vector(WithVector::Bool(true))
                            .build(),
                    )
                    .map_err(err)?;
                let rec = records.into_iter().next().ok_or_else(|| "query point not found".to_string())?;
                vector = match rec.vector {
                    Some(VectorStructInternal::Single(v)) => v,
                    Some(VectorStructInternal::Named(mut m)) => match m.remove(DEFAULT_VECTOR_NAME) {
                        Some(VectorInternal::Dense(v)) => v,
                        _ => return Err("query point has no dense vector".into()),
                    },
                    _ => return Err("query point has no dense vector".into()),
                };
            } else {
                return Err("no query vector".into());
            }
        }

        let mut b = QueryRequestBuilder::new(limit)
            .offset(offset)
            .query(ScoringQuery::Vector(QueryEnum::Nearest(NamedQuery::default_dense(vector))))
            .with_payload(payload_selector(req.get("fields"))?)
            .with_vector(WithVector::Bool(false))
            .params(SearchParams { exact: true, ..Default::default() });
        if let Some(t) = req.get("threshold").and_then(|v| v.as_f64()) {
            b = b.score_threshold(t as f32);
        }
        if let Some(f) = build_filter(req.get("filter"))? {
            b = b.filter(f);
        }
        let res = shard.query(b.build()).map_err(err)?;
        let mut buf = Vec::with_capacity(16 + res.len() * 128);
        buf.extend_from_slice(b"{\"points\":[");
        for (i, p) in res.into_iter().enumerate() {
            if i > 0 {
                buf.push(b',');
            }
            let _ = write!(buf, "{{\"id\":{},\"s\":", id_to_u64(p.id));
            serde_json::to_writer(&mut buf, &p.score).map_err(err)?;
            buf.extend_from_slice(b",\"p\":");
            write_payload_json(&mut buf, p.payload)?;
            buf.push(b'}');
        }
        buf.extend_from_slice(b"]}");
        Ok(buf)
    })
}

/// request: { "limit", "filter", "fields", "offset": u64 id cursor, "order_mtime": bool }
///
/// # Safety
///
/// `q` must be null or valid; `name`/`request`/`out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_scroll(q: *mut UsyQdrant, name: UsyStr, request: UsyStr, out: *mut UsyBuf) -> i32 {
    let name = name.as_str();
    let request = request.as_str();
    run_bytes(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let req: Json = serde_json::from_str(request).map_err(err)?;
        let limit = req.get("limit").and_then(|v| v.as_u64()).unwrap_or(100) as usize;
        let mut b = ScrollRequestBuilder::new()
            .limit(limit)
            .with_payload(payload_selector(req.get("fields"))?)
            .with_vector(WithVector::Bool(false));
        if let Some(f) = build_filter(req.get("filter"))? {
            b = b.filter(f);
        }
        if req.get("order_mtime").and_then(|v| v.as_bool()).unwrap_or(false) {
            b = b.order_by(OrderByInterface::Struct(OrderBy {
                key: json_path(F_MTIME)?,
                direction: Some(Direction::Desc),
                start_from: None,
            }));
        } else if let Some(off) = req.get("offset").and_then(|v| v.as_u64()) {
            b = b.offset(PointId::NumId(off));
        }
        let shard = q.get_or_load_shard(name)?;
        let (records, next_id) = shard.scroll(b.build()).map_err(err)?;
        let mut buf = Vec::with_capacity(32 + records.len() * 128);
        buf.extend_from_slice(b"{\"points\":[");
        for (i, p) in records.into_iter().enumerate() {
            if i > 0 {
                buf.push(b',');
            }
            let _ = write!(buf, "{{\"id\":{},\"p\":", id_to_u64(p.id));
            write_payload_json(&mut buf, p.payload)?;
            buf.push(b'}');
        }
        match next_id.map(id_to_u64) {
            Some(n) => {
                let _ = write!(buf, "],\"next\":{n}}}");
            }
            None => buf.extend_from_slice(b"],\"next\":null}"),
        }
        Ok(buf)
    })
}

/// request: { "fields": [..] }
///
/// # Safety
///
/// `q` must be null or valid; when `count > 0`, `ids` must point to `count` readable `u64`s; `name`/`request`/`out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_retrieve(
    q: *mut UsyQdrant,
    name: UsyStr,
    ids: *const u64,
    count: usize,
    request: UsyStr,
    out: *mut UsyBuf,
) -> i32 {
    let name = name.as_str();
    let request = request.as_str();
    run_bytes(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let ids: Vec<PointId> = unsafe { slice_from(ids, count) }?.iter().map(|&i| PointId::NumId(i)).collect();
        if ids.is_empty() {
            return Ok(br#"{"points":[]}"#.to_vec());
        }
        let req: Json = serde_json::from_str(request).unwrap_or(Json::Null);
        let shard = q.get_or_load_shard(name)?;
        let records = shard
            .retrieve(
                RetrieveRequestBuilder::new(ids)
                    .with_payload(payload_selector(req.get("fields"))?)
                    .with_vector(WithVector::Bool(false))
                    .build(),
            )
            .map_err(err)?;
        let mut buf = Vec::with_capacity(16 + records.len() * 128);
        buf.extend_from_slice(b"{\"points\":[");
        for (i, p) in records.into_iter().enumerate() {
            if i > 0 {
                buf.push(b',');
            }
            let _ = write!(buf, "{{\"id\":{},\"p\":", id_to_u64(p.id));
            write_payload_json(&mut buf, p.payload)?;
            buf.push(b'}');
        }
        buf.extend_from_slice(b"]}");
        Ok(buf)
    })
}

/// Merges `payload` (JSON object) into the given points.
///
/// # Safety
///
/// `q` must be null or valid; when `count > 0`, `ids` must point to `count` readable `u64`s; `name`/`payload`/`out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_set_payload(
    q: *mut UsyQdrant,
    name: UsyStr,
    ids: *const u64,
    count: usize,
    payload: UsyStr,
    out: *mut UsyBuf,
) -> i32 {
    let name = name.as_str();
    let payload = payload.as_str();
    run(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let ids: Vec<PointId> = unsafe { slice_from(ids, count) }?.iter().map(|&i| PointId::NumId(i)).collect();
        if ids.is_empty() {
            return Ok("ok".to_owned());
        }
        let p: Json = serde_json::from_str(payload).map_err(err)?;
        let Json::Object(map) = p else {
            return Err("payload must be a JSON object".into());
        };
        let shard = q.get_or_load_shard(name)?;
        shard
            .update(UpdateOperation::PayloadOperation(PayloadOps::SetPayload(SetPayloadOp {
                payload: Payload(map.into_iter().collect()),
                points: Some(ids),
                filter: None,
                key: None,
            })))
            .map_err(err)?;
        Ok("ok".to_owned())
    })
}

/// Deletes points with the given `ids` from `name`.
///
/// # Safety
///
/// `q` must be null or valid; when `count > 0`, `ids` must point to `count` readable `u64`s; `name`/`out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_delete(q: *mut UsyQdrant, name: UsyStr, ids: *const u64, count: usize, out: *mut UsyBuf) -> i32 {
    let name = name.as_str();
    run(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let ids: Vec<PointId> = unsafe { slice_from(ids, count) }?.iter().map(|&i| PointId::NumId(i)).collect();
        if ids.is_empty() {
            return Ok("ok".to_owned());
        }
        let shard = q.get_or_load_shard(name)?;
        shard
            .update(UpdateOperation::PointOperation(PointOperations::DeletePoints { ids }))
            .map_err(err)?;
        Ok("ok".to_owned())
    })
}

/// Drops collection `name` if it exists.
///
/// # Safety
///
/// `q` must be null or valid; `name`/`out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_drop_collection(q: *mut UsyQdrant, name: UsyStr, out: *mut UsyBuf) -> i32 {
    let name = name.as_str();
    run(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let dir = q.collection_dir(name)?;
        {
            let mut shards = q.shards.lock().map_err(|_| "shard lock poisoned".to_string())?;
            if let Some(shard) = shards.remove(name) {
                drop(shard);
            }
            if dir.exists() {
                fs::remove_dir_all(&dir).map_err(err)?;
            }
        }
        Ok("dropped".to_owned())
    })
}

fn dir_bytes(root: &Path) -> u64 {
    let mut total: u64 = 0;
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let Ok(ft) = entry.file_type() else {
                continue;
            };
            if ft.is_symlink() {
                continue;
            }
            if ft.is_dir() {
                stack.push(entry.path());
            } else if ft.is_file()
                && let Ok(meta) = entry.metadata()
            {
                total = total.saturating_add(meta.len());
            }
        }
    }
    total
}

fn cmp_case_insensitive(a: &str, b: &str) -> std::cmp::Ordering {
    let la = a.chars().flat_map(char::to_lowercase);
    let lb = b.chars().flat_map(char::to_lowercase);
    la.cmp(lb).then_with(|| a.cmp(b))
}

/// Lists Qdrant Edge collections on disk with exact point count and disk usage:
/// `[{ "name": string, "count": u64 | null, "bytes": u64 }]`.
///
/// # Safety
///
/// `q` must be null or valid; `out` must be null or uniquely writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_list_collections(q: *mut UsyQdrant, out: *mut UsyBuf) -> i32 {
    run_bytes(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let mut entries: Vec<(String, PathBuf)> = Vec::new();
        let read = fs::read_dir(&q.root).map_err(err)?;
        for item in read.flatten() {
            let Ok(ft) = item.file_type() else {
                continue;
            };
            if !ft.is_dir() || ft.is_symlink() {
                continue;
            }
            let path = item.path();
            if !shard_exists_on_disk(&path) {
                continue;
            }
            let name = item.file_name().to_string_lossy().into_owned();
            if name.is_empty() || name == "." || name == ".." {
                continue;
            }
            entries.push((name, path));
        }
        entries.sort_by(|a, b| cmp_case_insensitive(&a.0, &b.0));

        let mut buf = Vec::with_capacity(16 + entries.len() * 64);
        buf.push(b'[');
        for (i, (name, path)) in entries.iter().enumerate() {
            let bytes = dir_bytes(path);
            let loaded = {
                let shards = q.shards.lock().map_err(|_| "shard lock poisoned".to_string())?;
                shards.get(name).cloned()
            };
            let count = if let Some(shard) = loaded {
                shard.count(CountRequestBuilder::new().exact(true).build()).ok()
            } else if let Ok(shard) = EdgeShard::load(path, None) {
                shard.count(CountRequestBuilder::new().exact(true).build()).ok()
            } else {
                None
            };
            if i > 0 {
                buf.push(b',');
            }
            buf.extend_from_slice(b"{\"name\":");
            serde_json::to_writer(&mut buf, name).map_err(err)?;
            match count {
                Some(c) => {
                    let _ = write!(buf, ",\"count\":{c},\"bytes\":{bytes}}}");
                }
                None => {
                    let _ = write!(buf, ",\"count\":null,\"bytes\":{bytes}}}");
                }
            }
        }
        buf.push(b']');
        Ok(buf)
    })
}

/// Exact count; result is the number as a string. Missing collection counts as 0.
///
/// # Safety
///
/// `q` must be null or valid; `name`/`out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_count(q: *mut UsyQdrant, name: UsyStr, out: *mut UsyBuf) -> i32 {
    let name = name.as_str();
    run(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let dir = q.collection_dir(name)?;
        if !shard_exists_on_disk(&dir) {
            return Ok("0".to_owned());
        }
        let shard = q.get_or_load_shard(name)?;
        let count = shard.count(CountRequestBuilder::new().exact(true).build()).map_err(err)?;
        Ok(count.to_string())
    })
}

/// Tag facet counts: [{ "v": tag, "c": count }]
///
/// # Safety
///
/// `q` must be null or valid; `name`/`out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_facet_tags(q: *mut UsyQdrant, name: UsyStr, limit: u64, out: *mut UsyBuf) -> i32 {
    let name = name.as_str();
    run_bytes(out, || {
        let q = unsafe { qdrant_ref(q) }?;
        let dir = q.collection_dir(name)?;
        if !shard_exists_on_disk(&dir) {
            return Ok(b"[]".to_vec());
        }
        let shard = q.get_or_load_shard(name)?;
        let res = shard
            .facet(FacetRequestBuilder::new(json_path(F_TAGS)?).limit(limit as usize).exact(true).build())
            .map_err(err)?;
        let mut buf = Vec::with_capacity(8 + res.hits.len() * 32);
        buf.push(b'[');
        let mut first = true;
        for h in res.hits {
            if let FacetValue::Keyword(s) = h.value {
                if !first {
                    buf.push(b',');
                }
                first = false;
                buf.extend_from_slice(b"{\"v\":");
                serde_json::to_writer(&mut buf, &s).map_err(err)?;
                let _ = write!(buf, ",\"c\":{}}}", h.count);
            }
        }
        buf.push(b']');
        Ok(buf)
    })
}

// ---------------------------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------------------------

pub struct UsyTokenizer {
    tok: Tokenizer,
    ctx: usize,
}

/// Loads a HuggingFace tokenizer JSON file and configures fixed truncation/padding to `context_length`.
///
/// # Safety
///
/// `path` (when non-empty) must point to readable bytes; `out` must be null or uniquely writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_tok_load(path: UsyStr, context_length: usize, out: *mut UsyBuf) -> *mut UsyTokenizer {
    let mut tok = match Tokenizer::from_file(path.as_str()) {
        Ok(t) => t,
        Err(e) => {
            unsafe { write_out(out, e.to_string()) };
            return ptr::null_mut();
        }
    };
    if let Err(e) = tok.with_truncation(Some(TruncationParams { max_length: context_length, ..Default::default() })) {
        unsafe { write_out(out, e.to_string()) };
        return ptr::null_mut();
    }
    tok.with_padding(Some(PaddingParams {
        strategy: PaddingStrategy::Fixed(context_length),
        pad_id: 0,
        ..Default::default()
    }));
    Box::into_raw(Box::new(UsyTokenizer { tok, ctx: context_length }))
}

/// Frees a `UsyTokenizer` handle returned by `usy_tok_load`.
///
/// # Safety
///
/// `t` must be null or a live pointer returned by `usy_tok_load` not yet freed or used concurrently.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_tok_free(t: *mut UsyTokenizer) {
    if !t.is_null() {
        drop(unsafe { Box::from_raw(t) });
    }
}

/// Writes exactly `context_length` ids into `ids`. Returns the count written, 0 on failure.
///
/// # Safety
///
/// `t` must be null or valid; `text` must be valid; `ids` must be null or point to `context_length` writable `i64` elements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_tok_encode(t: *mut UsyTokenizer, text: UsyStr, ids: *mut i64) -> usize {
    if t.is_null() || ids.is_null() {
        return 0;
    }
    let t = unsafe { &*t };
    let Ok(enc) = t.tok.encode(text.as_str(), true) else { return 0 };
    if t.ctx == 0 {
        return 0;
    }
    let out = unsafe { slice::from_raw_parts_mut(ids, t.ctx) };
    out.fill(0);
    for (o, &i) in out.iter_mut().zip(enc.get_ids()) {
        *o = i as i64;
    }
    t.ctx
}

#[unsafe(no_mangle)]
pub extern "C" fn usy_buf_empty() -> UsyBuf {
    UsyBuf::empty()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn null_qdrant_and_empty_slices_fail_safely() {
        let empty = || UsyStr { ptr: ptr::null(), len: 0 };
        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_flush(ptr::null_mut(), empty(), &mut buf) }, 1);
        unsafe { usy_buf_free(buf) };

        let mut buf2 = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_retrieve(ptr::null_mut(), empty(), ptr::null(), 0, empty(), &mut buf2) }, 1);
        unsafe { usy_buf_free(buf2) };

        let mut buf3 = usy_buf_empty();
        assert_eq!(
            unsafe { usy_qd_upsert(ptr::null_mut(), empty(), ptr::null(), usize::MAX, ptr::null(), 4, empty(), &mut buf3) },
            1
        );
        unsafe { usy_buf_free(buf3) };

        assert_eq!(unsafe { slice_from::<u64>(ptr::null(), 0).unwrap().len() }, 0);
        assert!(unsafe { slice_from::<u64>(ptr::null(), 2) }.is_err());
        assert_eq!(unsafe { usy_tok_encode(ptr::null_mut(), empty(), ptr::null_mut()) }, 0);
    }

    #[test]
    fn edge_shard_roundtrip_crud_query_scroll_facet() {
        let root = std::env::temp_dir().join(format!("usyseek_edge_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let root_str = root.to_string_lossy().into_owned();
        let ustr = |s: &str| UsyStr { ptr: s.as_ptr(), len: s.len() };

        let mut buf = usy_buf_empty();
        let q = unsafe { usy_qd_open(ustr(&root_str), &mut buf) };
        unsafe { usy_buf_free(buf) };
        assert!(!q.is_null());

        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_ensure_collection(q, ustr("test_model"), 0, &mut buf) }, 2);
        unsafe { usy_buf_free(buf) };

        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_ensure_collection(q, ustr("test_model"), 4, &mut buf) }, 0);
        unsafe { usy_buf_free(buf) };

        let ids = [10u64, 20u64];
        let vecs = [1.0f32, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0];
        let payloads = r#"[{"p":"C:/a.jpg","n":"alpha_cat.jpg","k":0,"m":100,"s":10,"w":1,"h":1,"d":0,"t":["fav"],"a":["C:"]},{"p":"C:/b.mp4","n":"beta_dog.mp4","k":1,"m":200,"s":20,"w":1,"h":1,"d":500,"t":["fav","clip"],"a":["C:"]}]"#;
        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_upsert(q, ustr("test_model"), ids.as_ptr(), 2, vecs.as_ptr(), 4, ustr(payloads), &mut buf) }, 0);
        unsafe { usy_buf_free(buf) };

        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_optimize(q, ustr("test_model"), &mut buf) }, 0);
        unsafe { usy_buf_free(buf) };

        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_count(q, ustr("test_model"), &mut buf) }, 0);
        let cnt = unsafe { std::str::from_utf8(slice::from_raw_parts(buf.ptr, buf.len)).unwrap().to_owned() };
        unsafe { usy_buf_free(buf) };
        assert_eq!(cnt, "2");

        // Query by vector + prefix name filter
        let qvec = [0.9f32, 0.1, 0.0, 0.0];
        let qreq = r#"{"limit":10,"offset":0,"filter":{"names":["alpha"]},"fields":["p","n","t"]}"#;
        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_query(q, ustr("test_model"), qvec.as_ptr(), 4, ustr(qreq), &mut buf) }, 0);
        let qjson: Json = serde_json::from_slice(unsafe { slice::from_raw_parts(buf.ptr, buf.len) }).unwrap();
        unsafe { usy_buf_free(buf) };
        assert_eq!(qjson["points"].as_array().unwrap().len(), 1);
        assert_eq!(qjson["points"][0]["id"], 10);

        // Query by stored point ID
        let idreq = r#"{"limit":10,"offset":0,"id":20,"fields":["p"]}"#;
        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_query(q, ustr("test_model"), ptr::null(), 0, ustr(idreq), &mut buf) }, 0);
        let idjson: Json = serde_json::from_slice(unsafe { slice::from_raw_parts(buf.ptr, buf.len) }).unwrap();
        unsafe { usy_buf_free(buf) };
        assert_eq!(idjson["points"][0]["id"], 20);

        // Scroll ordered by mtime desc
        let sreq = r#"{"limit":10,"order_mtime":true,"fields":["m"]}"#;
        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_scroll(q, ustr("test_model"), ustr(sreq), &mut buf) }, 0);
        let sjson: Json = serde_json::from_slice(unsafe { slice::from_raw_parts(buf.ptr, buf.len) }).unwrap();
        unsafe { usy_buf_free(buf) };
        assert_eq!(sjson["points"][0]["id"], 20);
        assert_eq!(sjson["points"][1]["id"], 10);

        // Facet tags
        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_facet_tags(q, ustr("test_model"), 100, &mut buf) }, 0);
        let fjson: Json = serde_json::from_slice(unsafe { slice::from_raw_parts(buf.ptr, buf.len) }).unwrap();
        unsafe { usy_buf_free(buf) };
        assert_eq!(fjson.as_array().unwrap().len(), 2);

        // Close and reopen to verify persistence + lock release
        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_close_all(q, &mut buf) }, 0);
        unsafe { usy_buf_free(buf) };
        unsafe { usy_qd_free(q) };

        let mut buf = usy_buf_empty();
        let q2 = unsafe { usy_qd_open(ustr(&root_str), &mut buf) };
        unsafe { usy_buf_free(buf) };
        assert!(!q2.is_null());

        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_count(q2, ustr("test_model"), &mut buf) }, 0);
        let cnt2 = unsafe { std::str::from_utf8(slice::from_raw_parts(buf.ptr, buf.len)).unwrap().to_owned() };
        unsafe { usy_buf_free(buf) };
        assert_eq!(cnt2, "2");

        // Second collection for list_collections test
        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_ensure_collection(q2, ustr("alpha_model"), 4, &mut buf) }, 0);
        unsafe { usy_buf_free(buf) };

        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_list_collections(q2, &mut buf) }, 0);
        let cols: Json = serde_json::from_slice(unsafe { slice::from_raw_parts(buf.ptr, buf.len) }).unwrap();
        unsafe { usy_buf_free(buf) };
        let arr = cols.as_array().unwrap();
        assert_eq!(arr.len(), 2);
        assert_eq!(arr[0]["name"], "alpha_model");
        assert_eq!(arr[0]["count"], 0);
        assert!(arr[0]["bytes"].as_u64().unwrap() > 0);
        assert_eq!(arr[1]["name"], "test_model");
        assert_eq!(arr[1]["count"], 2);
        assert!(arr[1]["bytes"].as_u64().unwrap() > 0);

        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_drop_collection(q2, ustr("alpha_model"), &mut buf) }, 0);
        unsafe { usy_buf_free(buf) };

        let mut buf = usy_buf_empty();
        assert_eq!(unsafe { usy_qd_drop_collection(q2, ustr("test_model"), &mut buf) }, 0);
        unsafe { usy_buf_free(buf) };
        unsafe { usy_qd_free(q2) };
        let _ = fs::remove_dir_all(&root);
    }
}

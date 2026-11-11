//! usyseek native bridge: qdrant gRPC client + HuggingFace tokenizer + resvg, exposed over a small C ABI.
//! All structured data crosses the boundary as UTF-8 JSON; bulk numeric data (vectors, ids, pixels) as raw slices.

mod svg;

use std::collections::HashMap;
use std::ptr;
use std::slice;
use std::sync::OnceLock;
use std::time::Duration;

use qdrant_client::qdrant::{
    point_id::PointIdOptions, value::Kind, Condition, CountPointsBuilder, CreateCollectionBuilder,
    CreateFieldIndexCollectionBuilder, Datatype, DeletePointsBuilder, Direction, Distance, FacetCountsBuilder,
    FieldType, Filter, GetPointsBuilder, HnswConfigDiffBuilder, OrderByBuilder, PointId, PointStruct,
    PointsIdsList, QueryPointsBuilder, ScrollPointsBuilder, SearchParamsBuilder, SetPayloadPointsBuilder,
    TextIndexParamsBuilder, TokenizerType, UpsertPointsBuilder, Value, VectorInput, VectorParamsBuilder,
    PayloadIncludeSelector, Query,
};
use qdrant_client::qdrant::with_payload_selector::SelectorOptions;
use qdrant_client::{Payload, Qdrant};
use serde_json::{json, Map as JsonMap, Value as Json};
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

fn runtime() -> &'static tokio::runtime::Runtime {
    static RT: OnceLock<tokio::runtime::Runtime> = OnceLock::new();
    RT.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .thread_name("usy-qdrant")
            .enable_all()
            .build()
            .expect("tokio runtime")
    })
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

fn err<E: std::fmt::Display>(e: E) -> String {
    e.to_string()
}

// ---------------------------------------------------------------------------------------------
// Qdrant
// ---------------------------------------------------------------------------------------------

pub struct UsyQdrant {
    client: Qdrant,
}

const F_PATH: &str = "p";
const F_NAME: &str = "n";
const F_ANCESTORS: &str = "a";
const F_TAGS: &str = "t";
const F_KIND: &str = "k";
const F_MTIME: &str = "m";

fn qvalue_to_json(v: Value) -> Json {
    match v.kind {
        None | Some(Kind::NullValue(_)) => Json::Null,
        Some(Kind::BoolValue(b)) => Json::Bool(b),
        Some(Kind::IntegerValue(i)) => json!(i),
        Some(Kind::DoubleValue(d)) => json!(d),
        Some(Kind::StringValue(s)) => Json::String(s),
        Some(Kind::ListValue(l)) => Json::Array(l.values.into_iter().map(qvalue_to_json).collect()),
        Some(Kind::StructValue(s)) => {
            Json::Object(s.fields.into_iter().map(|(k, v)| (k, qvalue_to_json(v))).collect())
        }
    }
}

fn payload_to_json(p: HashMap<String, Value>) -> Json {
    Json::Object(p.into_iter().map(|(k, v)| (k, qvalue_to_json(v))).collect::<JsonMap<_, _>>())
}

fn id_to_u64(id: Option<PointId>) -> u64 {
    match id.and_then(|i| i.point_id_options) {
        Some(PointIdOptions::Num(n)) => n,
        _ => 0,
    }
}

fn str_list(v: Option<&Json>) -> Vec<String> {
    v.and_then(|x| x.as_array())
        .map(|a| a.iter().filter_map(|s| s.as_str().map(str::to_owned)).collect())
        .unwrap_or_default()
}

/// Filter description:
/// { "kind": 0|1, "tags_all": [..], "tags_none": [..], "dirs": [..] (any), "names": [..] (all, prefix text) }
fn build_filter(f: Option<&Json>) -> Option<Filter> {
    let f = f?;
    let mut must: Vec<Condition> = Vec::new();
    let mut must_not: Vec<Condition> = Vec::new();

    if let Some(k) = f.get("kind").and_then(|k| k.as_i64()) {
        must.push(Condition::matches(F_KIND, k));
    }
    for t in str_list(f.get("tags_all")) {
        must.push(Condition::matches(F_TAGS, t));
    }
    let none = str_list(f.get("tags_none"));
    if !none.is_empty() {
        must_not.push(Condition::matches(F_TAGS, none));
    }
    let dirs = str_list(f.get("dirs"));
    if !dirs.is_empty() {
        must.push(Condition::matches(F_ANCESTORS, dirs));
    }
    for n in str_list(f.get("names")) {
        must.push(Condition::matches_text(F_NAME, n));
    }

    if must.is_empty() && must_not.is_empty() {
        return None;
    }
    let mut filter = Filter::must(must);
    filter.must_not = must_not;
    Some(filter)
}

fn payload_selector(fields: &[String]) -> SelectorOptions {
    if fields.is_empty() {
        SelectorOptions::Enable(true)
    } else {
        SelectorOptions::Include(PayloadIncludeSelector { fields: fields.to_vec() })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_connect(url: UsyStr, out: *mut UsyBuf) -> *mut UsyQdrant {
    let url = url.as_str().to_owned();
    let res = Qdrant::from_url(&url)
        .timeout(Duration::from_secs(60))
        .connect_timeout(Duration::from_secs(2))
        .skip_compatibility_check()
        .build();
    match res {
        Ok(client) => Box::into_raw(Box::new(UsyQdrant { client })),
        Err(e) => {
            unsafe { write_out(out, e.to_string()) };
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_free(q: *mut UsyQdrant) {
    if !q.is_null() {
        drop(unsafe { Box::from_raw(q) });
    }
}

/// Returns 0 when the server answers a health check.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_health(q: *mut UsyQdrant, out: *mut UsyBuf) -> i32 {
    let q = unsafe { &*q };
    run(out, || {
        runtime()
            .block_on(q.client.health_check())
            .map(|r| r.version)
            .map_err(err)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_ensure_collection(q: *mut UsyQdrant, name: UsyStr, dim: u64, out: *mut UsyBuf) -> i32 {
    let q = unsafe { &*q };
    let name = name.as_str().to_owned();
    run(out, || {
        runtime().block_on(async {
            if q.client.collection_exists(&name).await.map_err(err)? {
                return Ok("exists".to_owned());
            }
            q.client
                .create_collection(
                    CreateCollectionBuilder::new(&name)
                        .vectors_config(
                            VectorParamsBuilder::new(dim, Distance::Cosine)
                                .on_disk(true)
                                .datatype(Datatype::Float16),
                        )
                        .hnsw_config(HnswConfigDiffBuilder::default().m(0))
                        .on_disk_payload(true),
                )
                .await
                .map_err(err)?;

            let idx = |field: &str, ty: FieldType| CreateFieldIndexCollectionBuilder::new(&name, field, ty).wait(true);
            q.client.create_field_index(idx(F_TAGS, FieldType::Keyword)).await.map_err(err)?;
            q.client.create_field_index(idx(F_ANCESTORS, FieldType::Keyword)).await.map_err(err)?;
            q.client.create_field_index(idx(F_KIND, FieldType::Integer)).await.map_err(err)?;
            q.client.create_field_index(idx(F_MTIME, FieldType::Integer)).await.map_err(err)?;
            q.client
                .create_field_index(
                    idx(F_NAME, FieldType::Text).field_index_params(
                        TextIndexParamsBuilder::new(TokenizerType::Prefix)
                            .lowercase(true)
                            .min_token_len(1)
                            .max_token_len(32),
                    ),
                )
                .await
                .map_err(err)?;
            let _ = F_PATH;
            Ok("created".to_owned())
        })
    })
}

/// payloads: JSON array (one object per point, same order as ids).
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
    let q = unsafe { &*q };
    let name = name.as_str().to_owned();
    let ids = unsafe { slice::from_raw_parts(ids, count) };
    let vectors = unsafe { slice::from_raw_parts(vectors, count * dim) };
    let payloads = payloads.as_str();
    run(out, || {
        let arr: Vec<Json> = serde_json::from_str(payloads).map_err(err)?;
        if arr.len() != count {
            return Err("payload count mismatch".into());
        }
        let points: Vec<PointStruct> = arr
            .into_iter()
            .enumerate()
            .map(|(i, p)| {
                let payload = Payload::try_from(p).unwrap_or_default();
                PointStruct::new(ids[i], vectors[i * dim..(i + 1) * dim].to_vec(), payload)
            })
            .collect();
        runtime()
            .block_on(q.client.upsert_points(UpsertPointsBuilder::new(&name, points).wait(true)))
            .map(|_| "ok".to_owned())
            .map_err(err)
    })
}

/// request: { "limit", "offset", "threshold", "filter", "fields": [..], "id": u64 (query by stored point) }
/// vector/dim: optional query vector (dim == 0 -> use "id").
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_query(
    q: *mut UsyQdrant,
    name: UsyStr,
    vector: *const f32,
    dim: usize,
    request: UsyStr,
    out: *mut UsyBuf,
) -> i32 {
    let q = unsafe { &*q };
    let name = name.as_str().to_owned();
    let vector: Vec<f32> = if dim > 0 && !vector.is_null() {
        unsafe { slice::from_raw_parts(vector, dim) }.to_vec()
    } else {
        Vec::new()
    };
    let request = request.as_str();
    run(out, || {
        let req: Json = serde_json::from_str(request).map_err(err)?;
        let limit = req.get("limit").and_then(|v| v.as_u64()).unwrap_or(100);
        let offset = req.get("offset").and_then(|v| v.as_u64()).unwrap_or(0);
        let fields = str_list(req.get("fields"));

        let query = if !vector.is_empty() {
            Query::new_nearest(vector)
        } else if let Some(id) = req.get("id").and_then(|v| v.as_u64()) {
            Query::new_nearest(VectorInput::new_id(id))
        } else {
            return Err("no query vector".into());
        };

        let mut b = QueryPointsBuilder::new(&name)
            .query(query)
            .limit(limit)
            .offset(offset)
            .with_payload(payload_selector(&fields))
            .with_vectors(false)
            .params(SearchParamsBuilder::default().exact(true));
        if let Some(t) = req.get("threshold").and_then(|v| v.as_f64()) {
            b = b.score_threshold(t as f32);
        }
        if let Some(f) = build_filter(req.get("filter")) {
            b = b.filter(f);
        }
        let res = runtime().block_on(q.client.query(b)).map_err(err)?;
        let pts: Vec<Json> = res
            .result
            .into_iter()
            .map(|p| json!({ "id": id_to_u64(p.id), "s": p.score, "p": payload_to_json(p.payload) }))
            .collect();
        Ok(json!({ "points": pts }).to_string())
    })
}

/// request: { "limit", "filter", "fields", "offset": u64 id cursor, "order_mtime": bool }
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_scroll(q: *mut UsyQdrant, name: UsyStr, request: UsyStr, out: *mut UsyBuf) -> i32 {
    let q = unsafe { &*q };
    let name = name.as_str().to_owned();
    let request = request.as_str();
    run(out, || {
        let req: Json = serde_json::from_str(request).map_err(err)?;
        let limit = req.get("limit").and_then(|v| v.as_u64()).unwrap_or(100) as u32;
        let fields = str_list(req.get("fields"));
        let mut b = ScrollPointsBuilder::new(&name)
            .limit(limit)
            .with_payload(payload_selector(&fields))
            .with_vectors(false);
        if let Some(f) = build_filter(req.get("filter")) {
            b = b.filter(f);
        }
        if req.get("order_mtime").and_then(|v| v.as_bool()).unwrap_or(false) {
            b = b.order_by(OrderByBuilder::new(F_MTIME).direction(Direction::Desc as i32));
        } else if let Some(off) = req.get("offset").and_then(|v| v.as_u64()) {
            b = b.offset(off);
        }
        let res = runtime().block_on(q.client.scroll(b)).map_err(err)?;
        let pts: Vec<Json> = res
            .result
            .into_iter()
            .map(|p| json!({ "id": id_to_u64(p.id), "p": payload_to_json(p.payload) }))
            .collect();
        let next = res.next_page_offset.map(|n| id_to_u64(Some(n)));
        Ok(json!({ "points": pts, "next": next }).to_string())
    })
}

/// request: { "fields": [..] }
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_retrieve(
    q: *mut UsyQdrant,
    name: UsyStr,
    ids: *const u64,
    count: usize,
    request: UsyStr,
    out: *mut UsyBuf,
) -> i32 {
    let q = unsafe { &*q };
    let name = name.as_str().to_owned();
    let ids: Vec<PointId> = unsafe { slice::from_raw_parts(ids, count) }.iter().map(|&i| i.into()).collect();
    let request = request.as_str();
    run(out, || {
        let req: Json = serde_json::from_str(request).unwrap_or(Json::Null);
        let fields = str_list(req.get("fields"));
        let res = runtime()
            .block_on(q.client.get_points(
                GetPointsBuilder::new(&name, ids)
                    .with_payload(payload_selector(&fields))
                    .with_vectors(false),
            ))
            .map_err(err)?;
        let pts: Vec<Json> = res
            .result
            .into_iter()
            .map(|p| json!({ "id": id_to_u64(p.id), "p": payload_to_json(p.payload) }))
            .collect();
        Ok(json!({ "points": pts }).to_string())
    })
}

/// Merges `payload` (JSON object) into the given points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_set_payload(
    q: *mut UsyQdrant,
    name: UsyStr,
    ids: *const u64,
    count: usize,
    payload: UsyStr,
    out: *mut UsyBuf,
) -> i32 {
    let q = unsafe { &*q };
    let name = name.as_str().to_owned();
    let ids: Vec<PointId> = unsafe { slice::from_raw_parts(ids, count) }.iter().map(|&i| i.into()).collect();
    let payload = payload.as_str();
    run(out, || {
        let p: Json = serde_json::from_str(payload).map_err(err)?;
        let p = Payload::try_from(p).map_err(err)?;
        runtime()
            .block_on(q.client.set_payload(
                SetPayloadPointsBuilder::new(&name, p)
                    .points_selector(PointsIdsList { ids })
                    .wait(true),
            ))
            .map(|_| "ok".to_owned())
            .map_err(err)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_delete(q: *mut UsyQdrant, name: UsyStr, ids: *const u64, count: usize, out: *mut UsyBuf) -> i32 {
    let q = unsafe { &*q };
    let name = name.as_str().to_owned();
    let ids: Vec<PointId> = unsafe { slice::from_raw_parts(ids, count) }.iter().map(|&i| i.into()).collect();
    run(out, || {
        runtime()
            .block_on(q.client.delete_points(
                DeletePointsBuilder::new(&name).points(PointsIdsList { ids }).wait(true),
            ))
            .map(|_| "ok".to_owned())
            .map_err(err)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_drop_collection(q: *mut UsyQdrant, name: UsyStr, out: *mut UsyBuf) -> i32 {
    let q = unsafe { &*q };
    let name = name.as_str().to_owned();
    run(out, || {
        runtime().block_on(async {
            if q.client.collection_exists(&name).await.map_err(err)? {
                q.client.delete_collection(&name).await.map_err(err)?;
            }
            Ok("dropped".to_owned())
        })
    })
}

/// Exact count; result is the number as a string. Missing collection counts as 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_count(q: *mut UsyQdrant, name: UsyStr, out: *mut UsyBuf) -> i32 {
    let q = unsafe { &*q };
    let name = name.as_str().to_owned();
    run(out, || {
        runtime().block_on(async {
            if !q.client.collection_exists(&name).await.map_err(err)? {
                return Ok("0".to_owned());
            }
            let r = q.client.count(CountPointsBuilder::new(&name).exact(true)).await.map_err(err)?;
            Ok(r.result.map(|c| c.count).unwrap_or(0).to_string())
        })
    })
}

/// Tag facet counts: [{ "v": tag, "c": count }]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_qd_facet_tags(q: *mut UsyQdrant, name: UsyStr, limit: u64, out: *mut UsyBuf) -> i32 {
    let q = unsafe { &*q };
    let name = name.as_str().to_owned();
    run(out, || {
        let res = runtime()
            .block_on(q.client.facet(FacetCountsBuilder::new(&name, F_TAGS).limit(limit).exact(true)))
            .map_err(err)?;
        let hits: Vec<Json> = res
            .hits
            .into_iter()
            .filter_map(|h| {
                use qdrant_client::qdrant::facet_value::Variant;
                match h.value.and_then(|v| v.variant) {
                    Some(Variant::StringValue(s)) => Some(json!({ "v": s, "c": h.count })),
                    _ => None,
                }
            })
            .collect();
        Ok(Json::Array(hits).to_string())
    })
}

// ---------------------------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------------------------

pub struct UsyTokenizer {
    tok: Tokenizer,
    ctx: usize,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_tok_load(path: UsyStr, context_length: usize, out: *mut UsyBuf) -> *mut UsyTokenizer {
    let path = path.as_str().to_owned();
    let mut tok = match Tokenizer::from_file(&path) {
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

#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_tok_free(t: *mut UsyTokenizer) {
    if !t.is_null() {
        drop(unsafe { Box::from_raw(t) });
    }
}

/// Writes exactly `context_length` ids into `ids`. Returns the count written, 0 on failure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_tok_encode(t: *mut UsyTokenizer, text: UsyStr, ids: *mut i64) -> usize {
    let t = unsafe { &*t };
    let Ok(enc) = t.tok.encode(text.as_str(), true) else { return 0 };
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

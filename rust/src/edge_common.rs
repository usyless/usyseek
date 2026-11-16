use std::path::Path;

use qdrant_edge::{
    CreateIndex, DEFAULT_VECTOR_NAME, Distance, EdgeConfigBuilder, EdgeOptimizersConfig, EdgeShard,
    EdgeVectorParamsBuilder, FieldIndexOperations, HnswIndexConfig, JsonPath, Memory,
    PayloadFieldSchema, PayloadSchemaParams, PayloadSchemaType, TextIndexParamsBuilder,
    TokenizerType, UpdateOperation, VectorStorageDatatype,
};

pub fn sanitize_collection_name(name: &str) -> Result<String, String> {
    if name.is_empty() || name == "." || name == ".." {
        return Err("invalid collection name".into());
    }
    let clean: String = name
        .chars()
        .map(|c| match c {
            '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|' => '_',
            c if c.is_control() => '_',
            c => c,
        })
        .collect();
    if clean.is_empty() || clean == "." || clean == ".." {
        return Err("invalid collection name".into());
    }
    Ok(clean)
}

pub fn shard_exists_on_disk(dir: &Path) -> bool {
    dir.join("edge_config.json").is_file() || dir.join("segments").is_dir()
}

pub fn create_edge_shard(dir: &Path, dim: usize) -> Result<EdgeShard, String> {
    let cfg = EdgeConfigBuilder::new()
        .vector(
            DEFAULT_VECTOR_NAME,
            EdgeVectorParamsBuilder::new(dim, Distance::Cosine)
                .memory(Memory::Cold)
                .datatype(VectorStorageDatatype::Float16)
                .build(),
        )
        .payload_memory(Memory::Cold)
        .hnsw_config({
            #[allow(deprecated)]
            HnswIndexConfig {
                m: 0,
                ef_construct: 100,
                full_scan_threshold: 10000,
                max_indexing_threads: 0,
                on_disk: None,
                memory: Some(Memory::Cold),
                payload_m: None,
                inline_storage: None,
            }
        })
        .optimizers(EdgeOptimizersConfig {
            deleted_threshold: Some(0.2),
            vacuum_min_vector_number: Some(1000),
            default_segment_number: Some(0),
            max_segment_size: None,
            indexing_threshold: Some(10000),
            prevent_unoptimized: None,
        })
        .build();

    let shard = EdgeShard::new(dir, cfg).map_err(|e| e.to_string())?;
    let create_idx = |field: &str, schema: PayloadFieldSchema| -> Result<(), String> {
        let field_name = field
            .parse::<JsonPath>()
            .map_err(|_| format!("invalid json path '{field}'"))?;
        shard
            .update(UpdateOperation::FieldIndexOperation(FieldIndexOperations::CreateIndex(
                CreateIndex { field_name, field_schema: Some(schema) },
            )))
            .map_err(|e| e.to_string())
    };
    create_idx("t", PayloadFieldSchema::FieldType(PayloadSchemaType::Keyword))?;
    create_idx("a", PayloadFieldSchema::FieldType(PayloadSchemaType::Keyword))?;
    create_idx("k", PayloadFieldSchema::FieldType(PayloadSchemaType::Integer))?;
    create_idx("m", PayloadFieldSchema::FieldType(PayloadSchemaType::Integer))?;
    create_idx(
        "n",
        PayloadFieldSchema::FieldParams(PayloadSchemaParams::Text(
            TextIndexParamsBuilder::new()
                .tokenizer(TokenizerType::Prefix)
                .lowercase(true)
                .min_token_len(1)
                .max_token_len(32)
                .build(),
        )),
    )?;
    shard.flush().map_err(|e| e.to_string())?;
    Ok(shard)
}

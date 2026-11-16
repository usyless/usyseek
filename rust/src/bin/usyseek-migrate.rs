//! Temporary one-shot migration tool: exports embeddings & payloads from the old
//! `data/qdrant/storage` collections into `data/qdrant_edge/<collection>` and cleans up old Qdrant files.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

#[path = "../edge_common.rs"]
mod edge_common;

use edge_common::{create_edge_shard, sanitize_collection_name, shard_exists_on_disk};
use qdrant_edge::{
    CountRequestBuilder, EdgeShard, PointInsertOperations, PointOperations, PointStructPersisted,
    ScrollRequestBuilder, UpdateOperation, WithPayloadInterface, WithVector,
};
use serde_json::Value as Json;

const BATCH_SIZE: usize = 512;
const FLUSH_INTERVAL_BATCHES: usize = 8;

struct Options {
    base_dir: Option<PathBuf>,
    force: bool,
    keep_old: bool,
}

fn parse_args() -> Result<Options, String> {
    let mut opts = Options { base_dir: None, force: false, keep_old: false };
    let mut args = std::env::args_os().skip(1);
    while let Some(arg) = args.next() {
        let s = arg.to_string_lossy();
        match s.as_ref() {
            "--dir" => {
                let p = args.next().ok_or("--dir requires a path argument")?;
                opts.base_dir = Some(PathBuf::from(p));
            }
            "--force" => opts.force = true,
            "--keep-old" => opts.keep_old = true,
            "-h" | "--help" => {
                println!(
                    "Usage: usyseek-migrate [--dir <usyseek_exe_dir>] [--force] [--keep-old]\n\
                     Migrates collections from <dir>/data/qdrant/storage/collections into <dir>/data/qdrant_edge\n\
                     and deletes old Qdrant files after verifying point counts."
                );
                std::process::exit(0);
            }
            other => return Err(format!("unknown argument: {other}")),
        }
    }
    Ok(opts)
}

fn resolve_base_dir(explicit: Option<PathBuf>) -> Result<PathBuf, String> {
    if let Some(dir) = explicit {
        return Ok(dir);
    }
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf));
    if let Some(ref d) = exe_dir
        && d.join("data").join("qdrant").exists()
    {
        return Ok(d.clone());
    }
    let cwd = std::env::current_dir().map_err(|e| e.to_string())?;
    if cwd.join("data").join("qdrant").exists() {
        return Ok(cwd);
    }
    Ok(exe_dir.unwrap_or(cwd))
}

fn read_vector_dim_from_config(coll_dir: &Path) -> Result<usize, String> {
    let cfg_path = coll_dir.join("config.json");
    let text = fs::read_to_string(&cfg_path)
        .map_err(|e| format!("failed to read {}: {e}", cfg_path.display()))?;
    let v: Json = serde_json::from_str(&text)
        .map_err(|e| format!("failed to parse {}: {e}", cfg_path.display()))?;
    if let Some(size) = v.pointer("/params/vectors/size").and_then(|x| x.as_u64()) {
        return usize::try_from(size).map_err(|_| "vector size overflow".to_string());
    }
    if let Some(obj) = v.pointer("/params/vectors").and_then(|x| x.as_object()) {
        for val in obj.values() {
            if let Some(size) = val.get("size").and_then(|x| x.as_u64()) {
                return usize::try_from(size).map_err(|_| "vector size overflow".to_string());
            }
        }
    }
    Err(format!("could not find vector size in {}", cfg_path.display()))
}

fn migrate_collection(coll_dir: &Path, edge_root: &Path, force: bool) -> Result<(), String> {
    let name = coll_dir
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or_else(|| format!("invalid collection directory name: {}", coll_dir.display()))?;
    let old_shard_dir = coll_dir.join("0");
    if !old_shard_dir.is_dir() {
        return Err(format!("missing shard directory {}", old_shard_dir.display()));
    }

    let dim = read_vector_dim_from_config(coll_dir)?;
    let clean_name = sanitize_collection_name(name)?;
    let dest_dir = edge_root.join(&clean_name);

    if dest_dir.exists() && shard_exists_on_disk(&dest_dir) {
        if !force {
            return Err(format!(
                "destination shard {} already exists (pass --force to replace it)",
                dest_dir.display()
            ));
        }
        fs::remove_dir_all(&dest_dir)
            .map_err(|e| format!("failed to remove existing {}: {e}", dest_dir.display()))?;
    }

    println!("Migrating collection '{name}' (dim={dim})...");
    let src_shard = EdgeShard::load(&old_shard_dir, None).map_err(|e| {
        format!(
            "failed to open old shard {} (make sure usyseek / qdrant.exe are closed): {e}",
            old_shard_dir.display()
        )
    })?;
    let expected = src_shard
        .count(CountRequestBuilder::new().exact(true).build())
        .map_err(|e| format!("failed to count old shard '{name}': {e}"))?;
    println!("  Source points: {expected}");

    let dest_shard = create_edge_shard(&dest_dir, dim)?;

    let mut offset = None;
    let mut migrated = 0usize;
    let mut batches = 0usize;
    loop {
        let mut req = ScrollRequestBuilder::new()
            .limit(BATCH_SIZE)
            .with_payload(WithPayloadInterface::Bool(true))
            .with_vector(WithVector::Bool(true));
        if let Some(off) = offset {
            req = req.offset(off);
        }
        let (records, next) = src_shard
            .scroll(req.build())
            .map_err(|e| format!("scroll failed on '{name}': {e}"))?;
        if records.is_empty() {
            break;
        }

        let mut points = Vec::with_capacity(records.len());
        for rec in records {
            let id = rec.id;
            match PointStructPersisted::try_from(rec) {
                Ok(pt) => points.push(pt),
                Err(e) => return Err(format!("point {id:?} missing vector: {e}")),
            }
        }
        let batch_len = points.len();
        dest_shard
            .update(UpdateOperation::PointOperation(PointOperations::UpsertPoints(
                PointInsertOperations::PointsList(points),
            )))
            .map_err(|e| format!("upsert failed on '{name}': {e}"))?;

        migrated += batch_len;
        batches += 1;
        if batches.is_multiple_of(FLUSH_INTERVAL_BATCHES) {
            dest_shard
                .flush()
                .map_err(|e| format!("flush failed on '{name}': {e}"))?;
        }
        if expected > 0 {
            print!("\r  Exported {migrated} / {expected} points...");
            let _ = std::io::Write::flush(&mut std::io::stdout());
        }

        match next {
            Some(n) => offset = Some(n),
            None => break,
        }
    }
    if expected > 0 {
        println!();
    }

    println!("  Optimizing and flushing '{clean_name}'...");
    dest_shard
        .optimize()
        .map_err(|e| format!("optimize failed on '{name}': {e}"))?;
    dest_shard
        .flush()
        .map_err(|e| format!("final flush failed on '{name}': {e}"))?;

    let actual = dest_shard
        .count(CountRequestBuilder::new().exact(true).build())
        .map_err(|e| format!("failed to count migrated shard '{clean_name}': {e}"))?;
    if actual != expected {
        return Err(format!(
            "count mismatch for '{name}': source has {expected} points, migrated shard has {actual}"
        ));
    }

    drop(dest_shard);
    drop(src_shard);
    println!("  Verified {actual} points in {}", dest_dir.display());
    Ok(())
}

fn remove_if_exists(path: &Path) -> Result<(), String> {
    if !path.exists() {
        return Ok(());
    }
    if path.is_dir() {
        fs::remove_dir_all(path)
            .map_err(|e| format!("failed to delete directory {}: {e}", path.display()))?;
    } else {
        fs::remove_file(path)
            .map_err(|e| format!("failed to delete file {}: {e}", path.display()))?;
    }
    println!("  Removed {}", path.display());
    Ok(())
}

fn cleanup_old_qdrant(base_dir: &Path) -> Result<(), String> {
    println!("Cleaning up old Qdrant files...");
    let data_dir = base_dir.join("data");
    remove_if_exists(&data_dir.join("qdrant"))?;
    remove_if_exists(&data_dir.join("qdrant.log"))?;
    remove_if_exists(&base_dir.join("qdrant"))?;

    let cfg_dir = base_dir.join("config");
    remove_if_exists(&cfg_dir.join("qdrant.yaml"))?;
    if cfg_dir.is_dir()
        && fs::read_dir(&cfg_dir)
            .map(|mut it| it.next().is_none())
            .unwrap_or(false)
    {
        remove_if_exists(&cfg_dir)?;
    }
    Ok(())
}

fn run(opts: Options) -> Result<(), String> {
    let base_dir = resolve_base_dir(opts.base_dir)?;
    let old_qdrant_dir = base_dir.join("data").join("qdrant");
    if !old_qdrant_dir.exists() {
        return Err(format!(
            "no old Qdrant directory found at {} (run from the usyseek.exe directory or pass --dir)",
            old_qdrant_dir.display()
        ));
    }

    let collections_dir = old_qdrant_dir.join("storage").join("collections");
    let edge_root = base_dir.join("data").join("qdrant_edge");
    fs::create_dir_all(&edge_root)
        .map_err(|e| format!("failed to create {}: {e}", edge_root.display()))?;

    let mut collections = Vec::new();
    if collections_dir.is_dir() {
        for entry in fs::read_dir(&collections_dir)
            .map_err(|e| format!("failed to read {}: {e}", collections_dir.display()))?
        {
            let entry = entry.map_err(|e| e.to_string())?;
            if entry.path().is_dir() {
                collections.push(entry.path());
            }
        }
    }
    collections.sort();

    if collections.is_empty() {
        println!("No old collections found under {}.", collections_dir.display());
    } else {
        for coll_dir in &collections {
            migrate_collection(coll_dir, &edge_root, opts.force)?;
        }
    }

    if !opts.keep_old {
        cleanup_old_qdrant(&base_dir)?;
    } else {
        println!("Skipped cleanup (--keep-old specified).");
    }

    println!("Migration complete.");
    Ok(())
}

fn main() -> ExitCode {
    match parse_args().and_then(run) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("Error: {e}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use qdrant_edge::{
        Distance, EdgeConfigBuilder, EdgeVectorParamsBuilder, HnswIndexConfig, Memory, PointStruct,
        VectorStorageDatatype, DEFAULT_VECTOR_NAME,
    };

    #[test]
    fn migrates_old_collection_and_cleans_up() {
        let root = std::env::temp_dir().join(format!("usyseek_migrate_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);

        let coll_dir = root.join("data/qdrant/storage/collections/test-model");
        let old_shard_dir = coll_dir.join("0");
        fs::create_dir_all(&old_shard_dir).unwrap();
        fs::create_dir_all(root.join("qdrant")).unwrap();
        fs::create_dir_all(root.join("config")).unwrap();
        fs::write(root.join("data/qdrant.log"), b"old log").unwrap();
        fs::write(root.join("qdrant/qdrant.exe"), b"old exe").unwrap();
        fs::write(root.join("config/qdrant.yaml"), b"log_level: WARN\n").unwrap();
        fs::write(
            coll_dir.join("config.json"),
            r#"{"params":{"vectors":{"size":4,"distance":"Cosine","on_disk":true,"datatype":"float16"},"on_disk_payload":true}}"#,
        )
        .unwrap();

        {
            #[allow(deprecated)]
            let cfg = EdgeConfigBuilder::new()
                .vector(
                    DEFAULT_VECTOR_NAME,
                    EdgeVectorParamsBuilder::new(4, Distance::Cosine)
                        .memory(Memory::Cold)
                        .datatype(VectorStorageDatatype::Float16)
                        .build(),
                )
                .payload_memory(Memory::Cold)
                .hnsw_config(HnswIndexConfig { m: 0, ..Default::default() })
                .build();
            let shard = EdgeShard::new(&old_shard_dir, cfg).unwrap();
            let pts: Vec<PointStructPersisted> = (1u64..=3)
                .map(|i| {
                    PointStruct::new(
                        i,
                        vec![i as f32, 0.0, 0.0, 0.0],
                        serde_json::json!({
                            "p": format!("C:/img_{i}.png"),
                            "n": format!("img_{i}.png"),
                            "k": 0,
                            "m": 100 + i,
                            "s": 42,
                            "w": 10,
                            "h": 10,
                            "d": 0,
                            "t": ["keep_tag"],
                            "a": ["c:"]
                        }),
                    )
                    .into()
                })
                .collect();
            shard
                .update(UpdateOperation::PointOperation(PointOperations::UpsertPoints(
                    PointInsertOperations::PointsList(pts),
                )))
                .unwrap();
            shard.flush().unwrap();
        }

        run(Options { base_dir: Some(root.clone()), force: false, keep_old: false }).unwrap();

        assert!(!root.join("data/qdrant").exists());
        assert!(!root.join("data/qdrant.log").exists());
        assert!(!root.join("qdrant").exists());
        assert!(!root.join("config/qdrant.yaml").exists());

        let migrated_dir = root.join("data/qdrant_edge/test-model");
        let shard = EdgeShard::load(&migrated_dir, None).unwrap();
        assert_eq!(shard.count(CountRequestBuilder::new().exact(true).build()).unwrap(), 3);
        drop(shard);
        let _ = fs::remove_dir_all(&root);
    }
}

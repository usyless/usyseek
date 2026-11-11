//! SVG rasterisation through resvg, shared by the indexer (embedding input) and the UI (thumbnails).

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;
use std::sync::{Arc, OnceLock};

use resvg::{tiny_skia, usvg};

use crate::{err, write_bytes, write_out, UsyBuf, UsyStr};

/// Larger documents are rejected rather than parsed (usvg keeps the whole tree in memory).
const MAX_SVG_BYTES: u64 = 64 << 20;
const MAX_SIDE: u32 = 8192;

const FIT_STRETCH: u32 = 0;
const FIT_COVER: u32 = 1;

#[repr(C)]
pub struct UsySvgInfo {
    /// Size of the returned bitmap in pixels.
    pub width: u32,
    pub height: u32,
    /// Intrinsic size of the document, rounded to whole pixels.
    pub src_width: u32,
    pub src_height: u32,
}

/// System fonts are enumerated once per process, on the first SVG that is rendered.
fn fontdb() -> Arc<usvg::fontdb::Database> {
    static DB: OnceLock<Arc<usvg::fontdb::Database>> = OnceLock::new();
    DB.get_or_init(|| {
        let mut db = usvg::fontdb::Database::new();
        db.load_system_fonts();
        Arc::new(db)
    })
    .clone()
}

fn render(path: &str, box_w: u32, box_h: u32, fit: u32, background: u32) -> Result<(UsySvgInfo, Vec<u8>), String> {
    if box_w == 0 || box_h == 0 || box_w > MAX_SIDE || box_h > MAX_SIDE {
        return Err("invalid target size".into());
    }
    let path = Path::new(path);
    if std::fs::metadata(path).map_err(err)?.len() > MAX_SVG_BYTES {
        return Err("SVG file too large".into());
    }
    let data = std::fs::read(path).map_err(err)?;

    let opt = usvg::Options {
        resources_dir: path.parent().map(Path::to_path_buf),
        fontdb: fontdb(),
        ..Default::default()
    };
    let tree = usvg::Tree::from_data(&data, &opt).map_err(err)?;
    let (sw, sh) = (tree.size().width(), tree.size().height());
    if !(sw > 0.0 && sh > 0.0) {
        return Err("SVG has no size".into());
    }

    let (bw, bh) = (box_w as f32, box_h as f32);
    let (transform, out_w, out_h) = match fit {
        FIT_STRETCH => (tiny_skia::Transform::from_scale(bw / sw, bh / sh), box_w, box_h),
        FIT_COVER => {
            let s = (bw / sw).max(bh / sh);
            let t = tiny_skia::Transform::from_scale(s, s).post_translate((bw - sw * s) / 2.0, (bh - sh * s) / 2.0);
            (t, box_w, box_h)
        }
        _ => {
            let s = (bw / sw).min(bh / sh);
            let ow = ((sw * s).round() as u32).clamp(1, box_w);
            let oh = ((sh * s).round() as u32).clamp(1, box_h);
            (tiny_skia::Transform::from_scale(ow as f32 / sw, oh as f32 / sh), ow, oh)
        }
    };

    let mut pixmap = tiny_skia::Pixmap::new(out_w, out_h).ok_or("could not allocate bitmap")?;
    if background >> 24 != 0 {
        let [a, r, g, b] = background.to_be_bytes();
        pixmap.fill(tiny_skia::Color::from_rgba8(r, g, b, a));
    }
    resvg::render(&tree, transform, &mut pixmap.as_mut());

    let info = UsySvgInfo {
        width: out_w,
        height: out_h,
        src_width: sw.round().max(1.0) as u32,
        src_height: sh.round().max(1.0) as u32,
    };
    Ok((info, pixmap.take()))
}

/// Rasterises the SVG (or gzip-compressed SVGZ) at `path` into premultiplied RGBA8.
///
/// `fit`: 0 = stretch to exactly `box_w` x `box_h`, 1 = cover the box (centre-cropped),
/// anything else = contain (aspect preserved, the bitmap is at most the box).
/// `background`: 0xAARRGGBB painted under the document, alpha 0 = keep transparency.
///
/// Returns 0 on success with `info.width * info.height * 4` bytes in `out`; otherwise `out` holds the error message.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usy_svg_render(
    path: UsyStr,
    box_w: u32,
    box_h: u32,
    fit: u32,
    background: u32,
    info: *mut UsySvgInfo,
    out: *mut UsyBuf,
) -> i32 {
    let path = path.as_str();
    // A malformed document must fail that one file, not take the whole indexer down with it
    let result = catch_unwind(AssertUnwindSafe(|| render(path, box_w, box_h, fit, background)))
        .unwrap_or_else(|_| Err("SVG renderer panicked".into()));
    match result {
        Ok((i, pixels)) => {
            if !info.is_null() {
                unsafe { *info = i };
            }
            unsafe { write_bytes(out, pixels) };
            0
        }
        Err(e) => {
            unsafe { write_out(out, e) };
            1
        }
    }
}

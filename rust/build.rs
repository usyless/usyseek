use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let resources_dir = manifest_dir.join("..").join("resources");
    let app_rc = resources_dir.join("app.rc");
    let resource_h = resources_dir.join("resource.h");

    println!("cargo:rerun-if-changed={}", app_rc.display());
    println!("cargo:rerun-if-changed={}", resource_h.display());

    let major = env::var("CARGO_PKG_VERSION_MAJOR").unwrap();
    let minor = env::var("CARGO_PKG_VERSION_MINOR").unwrap();
    let patch = env::var("CARGO_PKG_VERSION_PATCH").unwrap();
    let version = env::var("CARGO_PKG_VERSION").unwrap();

    let version_h = out_dir.join("usyseek_version.h");
    fs::write(
        &version_h,
        format!(
            "#pragma once\n\
             #define USYSEEK_VERSION_MAJOR {major}\n\
             #define USYSEEK_VERSION_MINOR {minor}\n\
             #define USYSEEK_VERSION_PATCH {patch}\n\
             #define USYSEEK_VERSION_TWEAK 0\n\
             #define USYSEEK_VERSION_STR   \"{version}\"\n\
             #define USYSEEK_VERSION_WSTR  L\"{version}\"\n"
        ),
    )
    .expect("failed to write usyseek_version.h");

    let res_path = out_dir.join("usyseek_rust.res");
    let mut cmd = Command::new("rc.exe");
    cmd.arg("/nologo")
        .arg("/DUSYSEEK_TARGET_RUST")
        .arg("/DUSYSEEK_RC_SKIP_ICON");

    if env::var("PROFILE").as_deref() == Ok("debug") {
        cmd.arg("/D_DEBUG");
    }

    let status = cmd
        .arg("/I")
        .arg(&out_dir)
        .arg("/fo")
        .arg(&res_path)
        .arg(&app_rc)
        .status()
        .expect("failed to execute rc.exe (ensure MSVC / Windows SDK tools are in PATH)");

    assert!(status.success(), "rc.exe failed with status: {status}");

    println!("cargo:rustc-link-arg-cdylib={}", res_path.display());
}

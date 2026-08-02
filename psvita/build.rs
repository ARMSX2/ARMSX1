use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-env-changed=VITASDK");
    println!("cargo:rerun-if-env-changed=ARMSX_VITA_NATIVE_DIR");
    println!("cargo:rerun-if-env-changed=ARMSX_VITA_FSUI_DIR");
    println!("cargo:rerun-if-env-changed=ARMSX_VITA_LIBCHDR_DIR");
    println!("cargo:rerun-if-env-changed=USE_CHD");

    let target = env::var("TARGET").unwrap_or_default();
    if !target.contains("vita") {
        return;
    }

    println!("cargo:rustc-check-cfg=cfg(vita_target)");
    println!("cargo:rustc-cfg=vita_target");

    let vitasdk = env::var("VITASDK").expect("VITASDK environment variable must be set");
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR must be set"));
    let repo_root = manifest_dir.parent().expect("psvita crate must live under the repo root");
    let native_dir = env::var_os("ARMSX_VITA_NATIVE_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| repo_root.join("bin/psvita"));
    let fsui_dir = env::var_os("ARMSX_VITA_FSUI_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| repo_root.join("build/fsui/psvita"));

    let native_archive = require_file(native_dir.join("libarmsx_vita.a"));
    let fsui_archives = [
        "libfsui-donor.a",
        "libfsui-backend-sdl.a",
        "libfsui-platform-sdl2.a",
        "libfsui-renderer-opengl.a",
        "libfsui-renderer-sdl2.a",
        "libfsui-renderer-sdl2surface.a",
        "libfsui-core.a",
        "libfsui_imgui.a",
        "libfsui_resources.a",
        "libfsui_glad.a",
    ]
        .into_iter()
        .map(|name| require_file(fsui_dir.join(name)))
        .collect::<Vec<_>>();
    let use_chd = env::var("USE_CHD").map(|value| value == "1").unwrap_or(false);
    let libchdr_archives = if use_chd {
        let libchdr_dir = env::var_os("ARMSX_VITA_LIBCHDR_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| repo_root.join("build/libchdr/psvita"));

        [
            "libchdr-static.a",
            "deps/lzma-25.01/libchdr-lzma.a",
            "deps/miniz-3.1.1/libminiz.a",
            "deps/zstd-1.5.7/libzstd.a",
        ]
        .into_iter()
        .map(|name| require_file(libchdr_dir.join(name)))
        .collect::<Vec<_>>()
    } else {
        Vec::new()
    };

    println!("cargo:rustc-link-search=native={}/arm-vita-eabi/lib", vitasdk);
    println!("cargo:rustc-link-lib=stdc++");
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    println!("cargo:rustc-link-arg={}", native_archive.display());
    if use_chd {
        for archive in &libchdr_archives {
            println!("cargo:rustc-link-arg={}", archive.display());
        }
    }
    for archive in &fsui_archives {
        println!("cargo:rustc-link-arg={}", archive.display());
    }
    println!("cargo:rustc-link-arg=-Wl,--end-group");

    emit_sdl2_link_flags(&vitasdk);
}

fn require_file(path: PathBuf) -> PathBuf {
    assert!(path.is_file(), "required Vita build artifact is missing: {}", path.display());
    path
}

fn emit_sdl2_link_flags(vitasdk: &str) {
    let pkg_config = Path::new(vitasdk).join("bin/arm-vita-eabi-pkg-config");
    let output = Command::new(&pkg_config)
        .args(["--libs", "sdl2"])
        .output()
        .expect("failed to execute arm-vita-eabi-pkg-config");

    if !output.status.success() {
        panic!(
            "arm-vita-eabi-pkg-config --libs sdl2 failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
    }

    let libs = String::from_utf8(output.stdout).expect("pkg-config output must be UTF-8");
    for token in libs.split_whitespace() {
        if let Some(path) = token.strip_prefix("-L") {
            println!("cargo:rustc-link-search=native={path}");
        } else if let Some(lib) = token.strip_prefix("-l") {
            println!("cargo:rustc-link-lib={lib}");
        } else if token.starts_with("-Wl,") {
            println!("cargo:rustc-link-arg={token}");
        }
    }
}

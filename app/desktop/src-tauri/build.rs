#[cfg(target_os = "macos")]
fn main() {
    tauri_build::build()
}

#[cfg(not(target_os = "macos"))]
fn main() {}

#[cfg(target_os = "macos")]
use easy_codex_host::health::{HEALTH_SOCKET_NAME, HealthError, HealthSnapshot, query_health};
#[cfg(target_os = "macos")]
use easy_codex_host::paths::AppPaths;
#[cfg(target_os = "macos")]
use serde::Serialize;

#[cfg(target_os = "macos")]
#[derive(Debug, Serialize)]
#[serde(tag = "connection", rename_all = "snake_case")]
enum HostProbe {
    Healthy { health: HealthSnapshot },
    Offline { reason: &'static str },
    ProtocolError { reason: &'static str },
}

#[cfg(target_os = "macos")]
#[tauri::command]
fn host_health() -> HostProbe {
    let Some(home) = std::env::var_os("HOME") else {
        return HostProbe::Offline {
            reason: "home_unavailable",
        };
    };
    let paths = AppPaths::from_home(std::path::Path::new(&home));
    match query_health(&paths.runtime_directory.join(HEALTH_SOCKET_NAME)) {
        Ok(health) => HostProbe::Healthy { health },
        Err(HealthError::Io(error))
            if matches!(
                error.kind(),
                std::io::ErrorKind::NotFound
                    | std::io::ErrorKind::ConnectionRefused
                    | std::io::ErrorKind::TimedOut
            ) =>
        {
            HostProbe::Offline {
                reason: "host_unreachable",
            }
        }
        Err(_) => HostProbe::ProtocolError {
            reason: "health_invalid",
        },
    }
}

#[cfg(target_os = "macos")]
fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![host_health])
        .run(tauri::generate_context!())
        .expect("Codex Keyboard desktop runtime failed");
}

#[cfg(not(target_os = "macos"))]
fn main() {
    println!("Codex Keyboard desktop requires macOS");
}

#[cfg(target_os = "macos")]
use easy_codex_host::health::{
    DashboardSnapshot, HEALTH_SOCKET_NAME, HealthError, HealthSnapshot, bind_dashboard_slot,
    query_dashboard, query_health,
};
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
#[derive(Debug, Serialize)]
#[serde(tag = "connection", rename_all = "snake_case")]
enum DashboardProbe {
    Healthy { dashboard: DashboardSnapshot },
    Offline { reason: &'static str },
    ProtocolError { reason: &'static str },
}

#[cfg(target_os = "macos")]
fn app_paths() -> Option<AppPaths> {
    std::env::var_os("HOME").map(|home| AppPaths::from_home(std::path::Path::new(&home)))
}

#[cfg(target_os = "macos")]
fn is_offline(error: &HealthError) -> bool {
    matches!(
        error,
        HealthError::Io(source)
            if matches!(
                source.kind(),
                std::io::ErrorKind::NotFound
                    | std::io::ErrorKind::ConnectionRefused
                    | std::io::ErrorKind::TimedOut
            )
    )
}

#[cfg(target_os = "macos")]
#[tauri::command]
fn host_health() -> HostProbe {
    let Some(paths) = app_paths() else {
        return HostProbe::Offline {
            reason: "home_unavailable",
        };
    };
    match query_health(&paths.runtime_directory.join(HEALTH_SOCKET_NAME)) {
        Ok(health) => HostProbe::Healthy { health },
        Err(error) if is_offline(&error) => HostProbe::Offline {
            reason: "host_unreachable",
        },
        Err(_) => HostProbe::ProtocolError {
            reason: "health_invalid",
        },
    }
}

#[cfg(target_os = "macos")]
#[tauri::command]
fn host_dashboard() -> DashboardProbe {
    let Some(paths) = app_paths() else {
        return DashboardProbe::Offline {
            reason: "home_unavailable",
        };
    };
    match query_dashboard(&paths.runtime_directory.join(HEALTH_SOCKET_NAME)) {
        Ok(dashboard) => DashboardProbe::Healthy { dashboard },
        Err(error) if is_offline(&error) => DashboardProbe::Offline {
            reason: "host_unreachable",
        },
        Err(_) => DashboardProbe::ProtocolError {
            reason: "dashboard_invalid",
        },
    }
}

#[cfg(target_os = "macos")]
#[tauri::command]
fn bind_slot(
    slot: u8,
    task_id: String,
    expected_generation: Option<u64>,
) -> Result<DashboardSnapshot, &'static str> {
    let Some(paths) = app_paths() else {
        return Err("home_unavailable");
    };
    bind_dashboard_slot(
        &paths.runtime_directory.join(HEALTH_SOCKET_NAME),
        slot,
        &task_id,
        expected_generation,
    )
    .map_err(|error| match error {
        HealthError::Rejected(_) => "binding_rejected",
        error if is_offline(&error) => "host_unreachable",
        _ => "dashboard_invalid",
    })
}

#[cfg(target_os = "macos")]
fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            host_health,
            host_dashboard,
            bind_slot
        ])
        .run(tauri::generate_context!())
        .expect("Codex Keyboard desktop runtime failed");
}

#[cfg(not(target_os = "macos"))]
fn main() {
    println!("Codex Keyboard desktop requires macOS");
}

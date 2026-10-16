#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

#[cfg(any(target_os = "macos", target_os = "windows"))]
use easy_codex_host::health::{
    DashboardSnapshot, HEALTH_SOCKET_NAME, HealthError, HealthSnapshot, bind_dashboard_slot,
    query_dashboard, query_health,
};
#[cfg(any(target_os = "macos", target_os = "windows"))]
use easy_codex_host::paths::AppPaths;
#[cfg(any(target_os = "macos", target_os = "windows"))]
use serde::Serialize;

#[cfg(any(target_os = "macos", target_os = "windows"))]
#[derive(Debug, Serialize)]
#[serde(tag = "connection", rename_all = "snake_case")]
enum HostProbe {
    Healthy { health: HealthSnapshot },
    Offline { reason: &'static str },
    ProtocolError { reason: &'static str },
}

#[cfg(any(target_os = "macos", target_os = "windows"))]
#[derive(Debug, Serialize)]
#[serde(tag = "connection", rename_all = "snake_case")]
enum DashboardProbe {
    Healthy { dashboard: DashboardSnapshot },
    Offline { reason: &'static str },
    ProtocolError { reason: &'static str },
}

#[cfg(any(target_os = "macos", target_os = "windows"))]
fn app_paths() -> Option<AppPaths> {
    std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(|home| AppPaths::from_home(std::path::Path::new(&home)))
}

#[cfg(any(target_os = "macos", target_os = "windows"))]
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

#[cfg(any(target_os = "macos", target_os = "windows"))]
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

#[cfg(any(target_os = "macos", target_os = "windows"))]
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
        Err(error) => {
            eprintln!("dashboard_probe_failed error={error}");
            DashboardProbe::ProtocolError {
                reason: "dashboard_invalid",
            }
        }
    }
}

#[cfg(any(target_os = "macos", target_os = "windows"))]
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

#[cfg(any(target_os = "macos", target_os = "windows"))]
fn main() {
    let mut context = tauri::generate_context!();
    let builder = tauri::Builder::default().invoke_handler(tauri::generate_handler![
        host_health,
        host_dashboard,
        bind_slot
    ]);
    #[cfg(target_os = "windows")]
    let builder = {
        // Set the data directory on the builder: the pinned runtime does not
        // copy WindowConfig.data_directory into WebviewAttributes.
        let windows = context.config().app.windows.clone();
        for window in &mut context.config_mut().app.windows {
            window.create = false;
        }
        builder.setup(move |app| {
            // Keep browser writes inside the existing writable Host support
            // root. LocalAppData can be read-only under the Windows token
            // used by the original installation.
            let paths = app_paths().ok_or_else(|| {
                std::io::Error::new(std::io::ErrorKind::NotFound, "home unavailable")
            })?;
            let directory = paths.root.join("desktop-webview-v2");
            for window in windows.iter().filter(|window| window.create) {
                tauri::WebviewWindowBuilder::from_config(app, window)?
                    .data_directory(directory.clone())
                    .build()?;
            }
            Ok(())
        })
    };
    builder
        .run(context)
        .expect("Codex Keyboard desktop runtime failed");
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
fn main() {
    println!("Codex Keyboard desktop requires macOS or Windows");
}

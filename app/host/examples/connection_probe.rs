//! Read-only local diagnostics. Never prints task IDs, titles, or speech.
use easy_codex_host::health::{HEALTH_SOCKET_NAME, query_dashboard, query_health};
use easy_codex_host::paths::AppPaths;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let verify_cloud = match std::env::args().nth(1).as_deref() {
        None => false,
        Some("--verify-cloud") => true,
        Some(_) => return Err("usage: connection_probe [--verify-cloud]".into()),
    };
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .ok_or("home unavailable")?;
    let paths = AppPaths::from_home(std::path::Path::new(&home));
    let socket = paths.runtime_directory.join(HEALTH_SOCKET_NAME);
    let health = query_health(&socket)?;
    println!("host=ready pid={}", health.pid);
    let dashboard = query_dashboard(&socket)?;
    println!("dashscope_configured={}", dashboard.provider.configured);
    println!("catalog_tasks={}", dashboard.tasks.len());
    if let Some(lan) = dashboard.lan {
        println!(
            "udp_port={} datagrams={} authenticated_heartbeats={} heartbeat_age_ms={:?}",
            lan.listening_port,
            lan.received_datagrams,
            lan.authenticated_heartbeats,
            lan.last_heartbeat_age_ms
        );
    }
    for slot in dashboard.slots {
        println!(
            "slot={} bound={} queued={} unread={}",
            slot.slot,
            slot.task_id.is_some(),
            slot.pending_jobs,
            slot.unread_generation.is_some()
        );
    }
    if verify_cloud {
        use easy_codex_host::secrets::{
            CredentialVerifier, DashScopeEnvStore, ImportLock, KeychainAccounts, SecretStore,
        };
        let lock = ImportLock::acquire(&paths.runtime_directory.join("key-import.lock"))?;
        let accounts = KeychainAccounts::load_or_create(&paths.installation_id, &lock)?;
        let environment = DashScopeEnvStore::new(paths.dashscope_env, &accounts);
        let secret = environment
            .get(&accounts.dashscope)?
            .ok_or("credential missing")?;
        drop(lock);
        easy_codex_host::dashscope::DashScopeHandshake::default().verify(&secret)?;
        println!("dashscope_cloud_handshake=verified");
    }
    Ok(())
}

use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;

use easy_codex_host::bindings::BindingService;
#[cfg(target_os = "macos")]
use easy_codex_host::cache::{CacheLimits, CacheStore};
use easy_codex_host::codex_catalog::CodexTaskCatalog;
#[cfg(target_os = "macos")]
use easy_codex_host::dashscope::DashScopeTtsClient;
use easy_codex_host::dashscope::{ASR_MODEL, DashScopeAsrClient, DashScopeHandshake};
use easy_codex_host::health::{HEALTH_SOCKET_NAME, HostDaemon, query_health};
use easy_codex_host::lan_voice::{WhisperCppConfig, import_default_whisper_model};
use easy_codex_host::launch_agent::{
    DeploymentMode, deploy_current_executable, is_loaded, uninstall,
};
use easy_codex_host::paths::AppPaths;
use easy_codex_host::provisioning::{LanProvisioning, load_or_create_device_secret, provision_lan};
#[cfg(target_os = "macos")]
use easy_codex_host::secrets::MacKeychainStore;
#[cfg(target_os = "macos")]
use easy_codex_host::secrets::remove_legacy_dashscope_items;
use easy_codex_host::secrets::{
    DashScopeEnvStore, ImportLock, KeychainAccounts, LocalCacheSecretStore, SecretStore,
    configure_dashscope_env, dashscope_key_is_installed,
};
#[cfg(target_os = "macos")]
use easy_codex_host::spark_runner::{SparkRunner, SparkRunnerConfig};
use easy_codex_host::store::StateStore;
#[cfg(target_os = "macos")]
use easy_codex_host::summary_orchestrator::{
    DashScopeSummarySynthesizer, SummaryOrchestrator, SummaryRunOutcome,
};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = std::env::args().skip(1);
    if let Some(raw) = arguments.next() {
        match raw.as_str() {
            "import-dashscope-key" => {
                return import_dashscope(arguments.next(), arguments.next());
            }
            "dashscope-key-status" => {
                require_no_more_arguments(&mut arguments, "dashscope-key-status")?;
                return dashscope_status();
            }
            "daemon" => {
                require_no_more_arguments(&mut arguments, "daemon")?;
                return run_daemon();
            }
            "health" => {
                require_no_more_arguments(&mut arguments, "health")?;
                return print_health();
            }
            "codex-catalog-status" => {
                require_no_more_arguments(&mut arguments, "codex-catalog-status")?;
                return print_codex_catalog_status();
            }
            "bind-slot" => {
                let slot = arguments.next().ok_or("missing slot")?.parse()?;
                let task_id = arguments.next().ok_or("missing task id")?;
                require_no_more_arguments(&mut arguments, "bind-slot")?;
                return bind_slot(slot, &task_id);
            }
            "provision-lan" => {
                let ssid = arguments.next().ok_or("missing Wi-Fi SSID")?;
                let host = arguments
                    .next()
                    .ok_or("missing Mac LAN IPv4 address")?
                    .parse()?;
                let port = arguments
                    .next()
                    .map(|value| value.parse())
                    .transpose()?
                    .unwrap_or(easy_codex_host::lan_voice::LAN_AUDIO_PORT);
                require_no_more_arguments(&mut arguments, "provision-lan")?;
                return provision_keyboard_lan(ssid, host, port);
            }
            "asr-import-model" => {
                let source = arguments.next().ok_or("missing model source path")?;
                require_no_more_arguments(&mut arguments, "asr-import-model")?;
                return import_asr_model(Path::new(&source));
            }
            "asr-status" => {
                require_no_more_arguments(&mut arguments, "asr-status")?;
                return print_asr_status();
            }
            "asr-transcribe-file" => {
                let source = arguments.next().ok_or("missing WAV source path")?;
                require_no_more_arguments(&mut arguments, "asr-transcribe-file")?;
                return transcribe_asr_file(Path::new(&source));
            }
            "summarize-slot" => {
                let slot = arguments.next().ok_or("missing slot")?.parse()?;
                require_no_more_arguments(&mut arguments, "summarize-slot")?;
                return summarize_slot(slot);
            }
            "summary-abandon-slot" => {
                let slot = arguments.next().ok_or("missing slot")?.parse()?;
                require_no_more_arguments(&mut arguments, "summary-abandon-slot")?;
                return abandon_slot_summary(slot);
            }
            "runner-supervisor" => {
                let parent = arguments.next().ok_or("missing parent pid")?.parse()?;
                let executable = arguments.next().ok_or("missing Codex executable")?;
                let task_id = arguments.next().ok_or("missing task id")?;
                let control_fd = arguments.next().ok_or("missing control fd")?.parse()?;
                require_no_more_arguments(&mut arguments, "runner-supervisor")?;
                let code = easy_codex_host::codex_runner::run_supervisor(
                    parent,
                    Path::new(&executable),
                    &task_id,
                    control_fd,
                )
                .unwrap_or(easy_codex_host::codex_runner::SUPERVISOR_IO_EXIT);
                std::process::exit(code);
            }
            "spark-supervisor" => {
                let parent = arguments.next().ok_or("missing parent pid")?.parse()?;
                let control_fd = arguments.next().ok_or("missing control fd")?.parse()?;
                let owner_lock_fd = arguments.next().ok_or("missing owner lock fd")?.parse()?;
                let task_lock_fd = arguments.next().ok_or("missing task lock fd")?.parse()?;
                require_no_more_arguments(&mut arguments, "spark-supervisor")?;
                let executable = required_os_environment("ECI_SPARK_EXECUTABLE")?;
                let codex_home = required_os_environment("ECI_SPARK_CODEX_HOME")?;
                let workdir = required_os_environment("ECI_SPARK_WORKDIR")?;
                let schema = required_os_environment("ECI_SPARK_SCHEMA")?;
                let output = required_os_environment("ECI_SPARK_OUTPUT")?;
                let tmpdir = required_os_environment("ECI_SPARK_TMPDIR")?;
                let owner_guardian = required_os_environment("ECI_SPARK_OWNER_GUARDIAN")?;
                let task_guardian = required_os_environment("ECI_SPARK_TASK_GUARDIAN")?;
                let code = easy_codex_host::spark_runner::run_spark_supervisor(
                    parent,
                    Path::new(&executable),
                    control_fd,
                    owner_lock_fd,
                    task_lock_fd,
                    Path::new(&codex_home),
                    Path::new(&workdir),
                    Path::new(&schema),
                    Path::new(&output),
                    Path::new(&tmpdir),
                    Path::new(&owner_guardian),
                    Path::new(&task_guardian),
                )
                .unwrap_or(easy_codex_host::codex_runner::SUPERVISOR_IO_EXIT);
                std::process::exit(code);
            }
            "launch-agent-install" => {
                require_no_more_arguments(&mut arguments, "launch-agent-install")?;
                return deploy_launch_agent(DeploymentMode::Install);
            }
            "launch-agent-upgrade" => {
                require_no_more_arguments(&mut arguments, "launch-agent-upgrade")?;
                return deploy_launch_agent(DeploymentMode::Upgrade);
            }
            "launch-agent-uninstall" => {
                require_no_more_arguments(&mut arguments, "launch-agent-uninstall")?;
                return uninstall_launch_agent();
            }
            "launch-agent-status" => {
                require_no_more_arguments(&mut arguments, "launch-agent-status")?;
                return print_launch_agent_status();
            }
            _ => return Err(format!("unknown command: {raw}").into()),
        }
    } else {
        println!("codex-keyboard local Wi-Fi Host ready");
    }
    Ok(())
}

fn required_os_environment(name: &str) -> Result<std::ffi::OsString, Box<dyn std::error::Error>> {
    std::env::var_os(name).ok_or_else(|| format!("missing {name}").into())
}

fn require_no_more_arguments(
    arguments: &mut impl Iterator<Item = String>,
    command: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    if arguments.next().is_some() {
        return Err(format!("{command} accepts no arguments").into());
    }
    Ok(())
}

fn home_paths() -> Result<AppPaths, Box<dyn std::error::Error>> {
    let home = std::env::var_os("HOME").ok_or("HOME is unavailable")?;
    Ok(AppPaths::from_home(Path::new(&home)))
}

fn run_daemon() -> Result<(), Box<dyn std::error::Error>> {
    let paths = home_paths()?;
    let daemon = HostDaemon::open(&paths)?;
    let shutdown = Arc::new(AtomicBool::new(false));
    signal_hook::flag::register(signal_hook::consts::SIGINT, Arc::clone(&shutdown))?;
    signal_hook::flag::register(signal_hook::consts::SIGTERM, Arc::clone(&shutdown))?;
    println!("status=ready");
    daemon.serve_until(shutdown)?;
    Ok(())
}

fn print_health() -> Result<(), Box<dyn std::error::Error>> {
    let paths = home_paths()?;
    let health = query_health(&paths.runtime_directory.join(HEALTH_SOCKET_NAME))?;
    println!("{}", serde_json::to_string(&health)?);
    Ok(())
}

fn print_codex_catalog_status() -> Result<(), Box<dyn std::error::Error>> {
    let tasks = CodexTaskCatalog::from_environment()?.list_tasks()?;
    let pinned = tasks.iter().filter(|task| task.pinned).count();
    println!("status=ready");
    println!("tasks={}", tasks.len());
    println!("pinned={pinned}");
    println!("recent={}", tasks.len() - pinned);
    Ok(())
}

fn bind_slot(slot: u8, task_id: &str) -> Result<(), Box<dyn std::error::Error>> {
    let paths = home_paths()?;
    paths.prepare()?;
    let catalog = CodexTaskCatalog::from_environment()?;
    let service = BindingService::new(&catalog);
    let mut store = StateStore::open(&paths.state_database)?;
    let expected_generation = store.binding(slot)?.map(|binding| binding.generation);
    let binding = service.bind(&mut store, slot, expected_generation, task_id)?;
    let generation = binding
        .or_else(|| store.binding(slot).ok().flatten())
        .ok_or("binding was not published")?
        .generation;
    println!("status=bound");
    println!("slot={slot}");
    println!("generation={generation}");
    Ok(())
}

fn provision_keyboard_lan(
    ssid: String,
    host: std::net::IpAddr,
    port: u16,
) -> Result<(), Box<dyn std::error::Error>> {
    use std::io::Read;

    let mut password = String::new();
    std::io::stdin().take(65).read_to_string(&mut password)?;
    while matches!(password.as_bytes().last(), Some(b'\n' | b'\r')) {
        password.pop();
    }
    let paths = home_paths()?;
    let device_secret = load_or_create_device_secret(&paths)?;
    let config = LanProvisioning::new(ssid, password, host, port, device_secret)?;
    let receipt = provision_lan(&config)?;
    println!("status=provisioned");
    println!("transport={}", receipt.transport);
    println!("product={}", receipt.product);
    println!("payload_bytes={}", receipt.payload_bytes);
    println!("chunks={}", receipt.chunks);
    println!("crc16={}", receipt.crc16);
    Ok(())
}

fn import_asr_model(source: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let paths = home_paths()?;
    paths.prepare()?;
    let receipt = import_default_whisper_model(&paths, source)?;
    println!("status=installed");
    println!("bytes={}", receipt.bytes);
    println!("sha1={}", receipt.sha1);
    println!("destination=private_app_support");
    Ok(())
}

fn print_asr_status() -> Result<(), Box<dyn std::error::Error>> {
    let paths = home_paths()?;
    paths.prepare()?;
    let import_lock = ImportLock::acquire(&paths.runtime_directory.join("key-import.lock"))?;
    let accounts = KeychainAccounts::load_or_create(&paths.installation_id, &import_lock)?;
    let env = DashScopeEnvStore::new(paths.dashscope_env.clone(), &accounts);
    let qwen_ready = dashscope_key_is_installed(&env, &accounts)?;
    let config = WhisperCppConfig::from_paths(&paths);
    println!("status={}", if qwen_ready { "ready" } else { "missing" });
    println!("provider=dashscope");
    println!("model={ASR_MODEL}");
    println!("region=cn-beijing");
    println!(
        "fallback_executable_present={}",
        config.executable.is_file()
    );
    println!("fallback_model_present={}", config.model.is_file());
    Ok(())
}

fn transcribe_asr_file(source: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let metadata = std::fs::symlink_metadata(source)?;
    if !metadata.is_file() || metadata.len() > 16_000 * 2 * 90 + 44 {
        return Err("WAV source must be a regular file within the 90 second PCM limit".into());
    }
    let wav = std::fs::read(source)?;
    let paths = home_paths()?;
    paths.prepare()?;
    let import_lock = ImportLock::acquire(&paths.runtime_directory.join("key-import.lock"))?;
    let accounts = KeychainAccounts::load_or_create(&paths.installation_id, &import_lock)?;
    let env = DashScopeEnvStore::new(paths.dashscope_env, &accounts);
    let secret = env
        .get(&accounts.dashscope)?
        .ok_or("DashScope API key is not configured")?;
    let transcript = DashScopeAsrClient::default().transcribe_wav(&secret, &wav)?;
    println!("status=transcribed");
    println!("model={}", transcript.model);
    println!("transport={}", transcript.transport);
    println!("text={}", transcript.text);
    Ok(())
}

#[cfg(target_os = "macos")]
fn summarize_slot(slot: u8) -> Result<(), Box<dyn std::error::Error>> {
    let paths = home_paths()?;
    paths.prepare()?;
    let mut state = StateStore::open(&paths.state_database)?;
    let binding = state.binding(slot)?.ok_or("slot is not bound")?;
    let import_lock = ImportLock::acquire(&paths.runtime_directory.join("key-import.lock"))?;
    let accounts = KeychainAccounts::load_or_create(&paths.installation_id, &import_lock)?;
    let cache_secrets = LocalCacheSecretStore::new(paths.cache_secret.clone(), &accounts);
    let cache = CacheStore::initialize(
        &paths.cache_directory,
        &cache_secrets,
        &accounts,
        CacheLimits::default(),
    )?;
    let dashscope_env = DashScopeEnvStore::new(paths.dashscope_env.clone(), &accounts);
    if !dashscope_key_is_installed(&dashscope_env, &accounts)? {
        return Err("DashScope API key is not configured".into());
    }
    let spark = SparkRunner::new(SparkRunnerConfig::default());
    let tts_client = DashScopeTtsClient::default();
    let tts = DashScopeSummarySynthesizer {
        client: &tts_client,
        secrets: &dashscope_env,
        accounts: &accounts,
    };
    let request_id = format!("manual-{}", uuid::Uuid::new_v4());
    let outcome = {
        let mut orchestrator = SummaryOrchestrator::new(&mut state, &cache, &spark, &tts);
        match orchestrator.resume(&binding.task_id)? {
            SummaryRunOutcome::Idle => orchestrator.run(&binding.task_id, &request_id)?,
            resumed => resumed,
        }
    };
    match outcome {
        SummaryRunOutcome::Idle => println!("status=idle"),
        SummaryRunOutcome::AlreadyPublished { generation } => {
            println!("status=already_published");
            println!("slot={slot}");
            println!("generation={generation}");
        }
        SummaryRunOutcome::Published {
            unread,
            audio,
            recovered_from_cache,
        } => {
            println!("status=published");
            println!("slot={slot}");
            println!("generation={}", unread.generation);
            println!("coverage_count={}", unread.coverage_count);
            println!("samples={}", audio.samples);
            println!("recovered_from_cache={recovered_from_cache}");
        }
        SummaryRunOutcome::ManualTtsReconciliationRequired { generation } => {
            println!("status=manual_tts_reconciliation_required");
            println!("slot={slot}");
            println!("generation={generation}");
        }
    }
    Ok(())
}

#[cfg(not(target_os = "macos"))]
fn summarize_slot(_slot: u8) -> Result<(), Box<dyn std::error::Error>> {
    Err("summarize-slot requires the macOS Host runtime".into())
}

fn abandon_slot_summary(slot: u8) -> Result<(), Box<dyn std::error::Error>> {
    let paths = home_paths()?;
    paths.prepare()?;
    let mut state = StateStore::open(&paths.state_database)?;
    let binding = state.binding(slot)?.ok_or("slot is not bound")?;
    let generation = state
        .abandon_interrupted_summary_for_task(&binding.task_id)?
        .ok_or("slot has no interrupted summary")?;
    println!("status=abandoned");
    println!("slot={slot}");
    println!("generation={generation}");
    Ok(())
}

fn deploy_launch_agent(mode: DeploymentMode) -> Result<(), Box<dyn std::error::Error>> {
    let home = std::env::var_os("HOME").ok_or("HOME is unavailable")?;
    deploy_current_executable(Path::new(&home), mode)?;
    println!(
        "status={}",
        match mode {
            DeploymentMode::Install => "installed",
            DeploymentMode::Upgrade => "upgraded",
        }
    );
    Ok(())
}

fn uninstall_launch_agent() -> Result<(), Box<dyn std::error::Error>> {
    let home = std::env::var_os("HOME").ok_or("HOME is unavailable")?;
    uninstall(Path::new(&home))?;
    println!("status=uninstalled");
    Ok(())
}

fn print_launch_agent_status() -> Result<(), Box<dyn std::error::Error>> {
    let loaded = is_loaded()?;
    let paths = home_paths()?;
    let healthy = loaded && query_health(&paths.runtime_directory.join(HEALTH_SOCKET_NAME)).is_ok();
    println!("loaded={loaded}");
    println!("healthy={healthy}");
    Ok(())
}

fn import_dashscope(
    source: Option<String>,
    unexpected: Option<String>,
) -> Result<(), Box<dyn std::error::Error>> {
    if unexpected.is_some() {
        return Err("import-dashscope-key accepts exactly one source file".into());
    }
    let source =
        source.ok_or("import-dashscope-key requires an explicitly selected source file")?;
    let home = std::env::var_os("HOME").ok_or("HOME is unavailable")?;
    let paths = AppPaths::from_home(std::path::Path::new(&home));
    paths.prepare()?;
    let import_lock = ImportLock::acquire(&paths.runtime_directory.join("key-import.lock"))?;
    let receipt = configure_dashscope_env(
        std::path::Path::new(&source),
        &paths.dashscope_env,
        &DashScopeHandshake::default(),
        &import_lock,
    )?;
    #[cfg(target_os = "macos")]
    {
        let accounts = KeychainAccounts::load_or_create(&paths.installation_id, &import_lock)?;
        remove_legacy_dashscope_items(&MacKeychainStore::background(), &accounts)?;
    }
    println!("status=configured_verified");
    println!("event_id={}", receipt.event_id);
    println!("region={}", receipt.region);
    println!("model={}", receipt.model);
    println!("server_event={}", receipt.server_event);
    println!("transport={}", receipt.transport);
    Ok(())
}

fn dashscope_status() -> Result<(), Box<dyn std::error::Error>> {
    let home = std::env::var_os("HOME").ok_or("HOME is unavailable")?;
    let paths = AppPaths::from_home(std::path::Path::new(&home));
    paths.prepare()?;
    let import_lock = ImportLock::acquire(&paths.runtime_directory.join("key-import.lock"))?;
    let accounts = KeychainAccounts::load_or_create(&paths.installation_id, &import_lock)?;
    let env = DashScopeEnvStore::new(paths.dashscope_env, &accounts);
    let installed = dashscope_key_is_installed(&env, &accounts)?;
    println!(
        "status={}",
        if installed { "configured" } else { "missing" }
    );
    Ok(())
}

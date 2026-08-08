#![cfg(target_os = "macos")]

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

#[cfg(not(test))]
use crate::cache::CacheLimits;
use crate::cache::CacheStore;
use crate::dashscope::DashScopeTtsClient;
use crate::paths::AppPaths;
#[cfg(not(test))]
use crate::secrets::LocalCacheSecretStore;
use crate::secrets::{DashScopeEnvStore, ImportLock, KeychainAccounts, dashscope_key_is_installed};
use crate::spark_runner::{SparkRunner, SparkRunnerConfig};
use crate::store::StateStore;
use crate::summary_orchestrator::{
    DashScopeSummarySynthesizer, SummaryOrchestrator, SummaryRunOutcome,
};

const POLL_INTERVAL: Duration = Duration::from_millis(250);
const FAILURE_BACKOFF: Duration = Duration::from_secs(30);
const MANUAL_BACKOFF: Duration = Duration::from_secs(60 * 60);

pub fn run(
    paths: AppPaths,
    mut store: StateStore,
    cache: Arc<CacheStore>,
    shutdown: Arc<AtomicBool>,
) {
    let mut retry_after = HashMap::<String, Instant>::new();
    while !shutdown.load(Ordering::Acquire) {
        match Runtime::open(&paths, Arc::clone(&cache)) {
            Ok(runtime) => {
                run_ready(&mut store, &runtime, &shutdown, &mut retry_after);
                return;
            }
            Err(error) => {
                eprintln!("summary_runtime=unavailable error={error}");
                sleep_until_shutdown(&shutdown, FAILURE_BACKOFF);
            }
        }
    }
}

struct Runtime {
    cache: Arc<CacheStore>,
    spark: SparkRunner,
    tts_client: DashScopeTtsClient,
    dashscope_env: DashScopeEnvStore,
    accounts: KeychainAccounts,
}

impl Runtime {
    fn open(paths: &AppPaths, cache: Arc<CacheStore>) -> Result<Self, String> {
        let lock = ImportLock::acquire(&paths.runtime_directory.join("key-import.lock"))
            .map_err(|error| error.to_string())?;
        let accounts = KeychainAccounts::load_or_create(&paths.installation_id, &lock)
            .map_err(|error| error.to_string())?;
        let dashscope_env = DashScopeEnvStore::new(paths.dashscope_env.clone(), &accounts);
        if !dashscope_key_is_installed(&dashscope_env, &accounts)
            .map_err(|error| error.to_string())?
        {
            return Err("dashscope credential is missing".into());
        }
        Ok(Self {
            cache,
            spark: SparkRunner::new(SparkRunnerConfig::default()),
            tts_client: DashScopeTtsClient::default(),
            dashscope_env,
            accounts,
        })
    }
}

#[cfg(all(target_os = "macos", not(test)))]
pub(crate) fn open_shared_cache(paths: &AppPaths) -> Result<CacheStore, String> {
    let lock = ImportLock::acquire(&paths.runtime_directory.join("key-import.lock"))
        .map_err(|error| error.to_string())?;
    let accounts = KeychainAccounts::load_or_create(&paths.installation_id, &lock)
        .map_err(|error| error.to_string())?;
    let cache_secrets = LocalCacheSecretStore::new(paths.cache_secret.clone(), &accounts);
    CacheStore::initialize(
        &paths.cache_directory,
        &cache_secrets,
        &accounts,
        CacheLimits::default(),
    )
    .map_err(|error| error.to_string())
}

fn run_ready(
    store: &mut StateStore,
    runtime: &Runtime,
    shutdown: &AtomicBool,
    retry_after: &mut HashMap<String, Instant>,
) {
    let mut scan_after = None::<String>;
    while !shutdown.load(Ordering::Acquire) {
        let tasks = match store.summary_work_tasks_after(scan_after.as_deref()) {
            Ok(tasks) => tasks,
            Err(error) => {
                eprintln!("summary_worker=store_error error={error}");
                sleep_until_shutdown(shutdown, FAILURE_BACKOFF);
                continue;
            }
        };
        if tasks.is_empty() {
            scan_after = None;
            sleep_until_shutdown(shutdown, POLL_INTERVAL);
            continue;
        }
        scan_after = tasks.last().cloned();
        for task_id in tasks {
            if shutdown.load(Ordering::Acquire) {
                break;
            }
            if retry_after
                .get(&task_id)
                .is_some_and(|deadline| *deadline > Instant::now())
            {
                continue;
            }
            let tts = DashScopeSummarySynthesizer {
                client: &runtime.tts_client,
                secrets: &runtime.dashscope_env,
                accounts: &runtime.accounts,
            };
            let request_id = format!("auto-{}", uuid::Uuid::new_v4());
            let result = {
                let mut orchestrator =
                    SummaryOrchestrator::new(store, runtime.cache.as_ref(), &runtime.spark, &tts);
                match orchestrator.resume(&task_id) {
                    Ok(SummaryRunOutcome::Idle) => orchestrator.run(&task_id, &request_id),
                    other => other,
                }
            };
            match result {
                Ok(SummaryRunOutcome::Published { unread, .. }) => {
                    retry_after.remove(&task_id);
                    eprintln!(
                        "summary_worker=published generation={} coverage={}",
                        unread.generation, unread.coverage_count
                    );
                }
                Ok(SummaryRunOutcome::AlreadyPublished { generation }) => {
                    retry_after.remove(&task_id);
                    eprintln!("summary_worker=already_published generation={generation}");
                }
                Ok(SummaryRunOutcome::Idle) => {
                    retry_after.remove(&task_id);
                }
                Ok(SummaryRunOutcome::ManualTtsReconciliationRequired { generation }) => {
                    retry_after.insert(task_id, Instant::now() + MANUAL_BACKOFF);
                    eprintln!("summary_worker=manual_reconciliation generation={generation}");
                }
                Err(error) => {
                    retry_after.insert(task_id, Instant::now() + FAILURE_BACKOFF);
                    eprintln!("summary_worker=failed error={error}");
                }
            }
        }
        sleep_until_shutdown(shutdown, POLL_INTERVAL);
    }
}

fn sleep_until_shutdown(shutdown: &AtomicBool, duration: Duration) {
    let deadline = Instant::now() + duration;
    while !shutdown.load(Ordering::Acquire) && Instant::now() < deadline {
        thread::sleep(
            Duration::from_millis(100).min(deadline.saturating_duration_since(Instant::now())),
        );
    }
}

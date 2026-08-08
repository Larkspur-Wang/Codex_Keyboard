use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use easy_codex_host::bindings::{BindingError, BindingService};
use easy_codex_host::codex_catalog::{CatalogError, CodexTaskCatalog};
use easy_codex_host::codex_runner::{CodexRunner, CodexRunnerConfig};
use easy_codex_host::paths::{AppPaths, open_private_file, secure_directory};
use easy_codex_host::prompt_queue::{DurablePromptScheduler, PromptQueueError, PromptQueueService};
use easy_codex_host::rollout_observer::{ObserverTick, RolloutObserver};
use easy_codex_host::store::{Binding, EnqueueOutcome, StateStore};
use fs2::FileExt;
use rusqlite::{Connection, OpenFlags, OptionalExtension};
use serde::Serialize;
use uuid::Uuid;

const APPROVAL_ENV: &str = "ECI_M3_GATE_APPROVED";
const TASK_ENV: &str = "ECI_M3_TEST_TASK_ID";
const CLI_ENV: &str = "EASY_CODEX_CLI";
const SUPERVISOR_ENV: &str = "ECI_M3_SUPERVISOR";
const MANUAL_TOKEN: &str = "ECI_M3_GATE_OK";
const QUEUED_TOKEN: &str = "ECI_M3_QUEUE_OK";
const MAX_PROMPT_BYTES: u64 = 32 * 1024;
const POLL_DELAY: Duration = Duration::from_millis(100);
const OBSERVER_TIMEOUT: Duration = Duration::from_secs(90);
const RUNNER_TIMEOUT: Duration = Duration::from_secs(5 * 60);
const SCHEDULER_TIMEOUT: Duration = Duration::from_secs(6 * 60);
const GATE_STATE_DIRECTORY: &str = "m3-runtime-gates";
const GATE_LOCK_FILE: &str = "gate.lock";
const GATE_OWNER_FILE: &str = ".owner";
const GATE_OWNER_TEMP_FILE: &str = ".owner.pending";
const GATE_OWNER_CONTENT: &[u8] = b"easy-codex-m3-runtime-v1\n";
const GATE_RUN_PREFIX: &str = "run-";
const MAX_MANAGED_RUNS: usize = 64;

#[derive(Debug, Clone, Copy)]
enum GateFailure {
    ApprovalMissing,
    Configuration,
    Input,
    Catalog,
    Binding,
    Store,
    QueueUnbound,
    QueueStore,
    Runner,
    Observer,
    Timeout,
    Interrupted,
    Assertion,
}

impl GateFailure {
    fn category(self) -> &'static str {
        match self {
            Self::ApprovalMissing => "approval_missing",
            Self::Configuration => "configuration",
            Self::Input => "input",
            Self::Catalog => "catalog",
            Self::Binding => "binding",
            Self::Store => "store",
            Self::QueueUnbound => "queue_unbound",
            Self::QueueStore => "queue_store",
            Self::Runner => "runner",
            Self::Observer => "observer",
            Self::Timeout => "timeout",
            Self::Interrupted => "interrupted",
            Self::Assertion => "assertion",
        }
    }
}

#[derive(Serialize)]
struct GateReport {
    schema: u8,
    status: &'static str,
    isolated_state: bool,
    catalog_allowlisted: bool,
    binding_slot: u8,
    binding_generation: u64,
    manual_completion_inserted: usize,
    queued_completion_inserted: usize,
    restart_duplicate_inserted: usize,
    restart_replay_reported: usize,
    catalog_transient_failures: usize,
    observer_transient_failures: usize,
    total_completion_count: u32,
    distinct_completion_count: u32,
    job_state: &'static str,
    job_task_matches: bool,
    prompt_matches: bool,
    manual_token_matches_once: bool,
    queued_token_matches_once: bool,
    private_identifiers_emitted: bool,
    desktop_visibility: &'static str,
}

#[derive(Debug)]
struct PersistedFacts {
    completion_count: u32,
    distinct_completion_count: u32,
    job_state: String,
    job_task_matches: bool,
    prompt_matches: bool,
    binding_matches: bool,
    manual_token_count: u32,
    queued_token_count: u32,
}

struct ManagedGateState {
    run_path: PathBuf,
    _lock: GateLock,
}

struct GateLock(File);

impl Drop for GateLock {
    fn drop(&mut self) {
        // Make the guard lifetime authoritative across platform lock semantics.
        let _ = FileExt::unlock(&self.0);
    }
}

struct ProvisionalRun {
    path: PathBuf,
    device: u64,
    inode: u64,
    armed: bool,
}

impl ProvisionalRun {
    fn disarm(mut self) {
        self.armed = false;
    }
}

impl Drop for ProvisionalRun {
    fn drop(&mut self) {
        if !self.armed {
            return;
        }
        let Ok(metadata) = fs::symlink_metadata(&self.path) else {
            return;
        };
        if metadata.is_dir()
            && metadata.uid() == unsafe { libc::geteuid() }
            && metadata.dev() == self.device
            && metadata.ino() == self.inode
        {
            let _ = fs::remove_dir_all(&self.path);
        }
    }
}

impl ManagedGateState {
    fn create() -> Result<Self, GateFailure> {
        let home = PathBuf::from(env::var_os("HOME").ok_or(GateFailure::Configuration)?);
        let paths = AppPaths::from_home(&home);
        paths.prepare().map_err(|_| GateFailure::Store)?;
        Self::create_in(&paths.runtime_directory.join(GATE_STATE_DIRECTORY))
    }

    fn create_in(root: &Path) -> Result<Self, GateFailure> {
        Self::create_in_with(root, |_| Ok(()))
    }

    fn create_in_with(
        root: &Path,
        mut checkpoint: impl FnMut(&'static str) -> Result<(), GateFailure>,
    ) -> Result<Self, GateFailure> {
        secure_directory(root).map_err(|_| GateFailure::Store)?;
        let lock = open_private_file(&root.join(GATE_LOCK_FILE)).map_err(|_| GateFailure::Store)?;
        lock.try_lock_exclusive().map_err(|_| GateFailure::Store)?;
        let lock = GateLock(lock);
        cleanup_stale_runs(root)?;

        let run_path = root.join(format!("{GATE_RUN_PREFIX}{}", Uuid::new_v4()));
        fs::create_dir(&run_path).map_err(|_| GateFailure::Store)?;
        let metadata = fs::symlink_metadata(&run_path).map_err(|_| GateFailure::Store)?;
        let provisional = ProvisionalRun {
            path: run_path.clone(),
            device: metadata.dev(),
            inode: metadata.ino(),
            armed: true,
        };
        checkpoint("directory_created")?;
        fs::set_permissions(&run_path, fs::Permissions::from_mode(0o700))
            .map_err(|_| GateFailure::Store)?;
        checkpoint("permissions_set")?;
        let owner_temp = run_path.join(GATE_OWNER_TEMP_FILE);
        let mut owner = open_private_file(&owner_temp).map_err(|_| GateFailure::Store)?;
        checkpoint("marker_temp_opened")?;
        owner.set_len(0).map_err(|_| GateFailure::Store)?;
        checkpoint("marker_temp_truncated")?;
        use std::io::Write;
        owner
            .write_all(GATE_OWNER_CONTENT)
            .map_err(|_| GateFailure::Store)?;
        checkpoint("marker_temp_written")?;
        owner.sync_all().map_err(|_| GateFailure::Store)?;
        checkpoint("marker_temp_synced")?;
        fs::rename(&owner_temp, run_path.join(GATE_OWNER_FILE)).map_err(|_| GateFailure::Store)?;
        checkpoint("marker_published")?;
        File::open(&run_path)
            .and_then(|directory| directory.sync_all())
            .map_err(|_| GateFailure::Store)?;
        checkpoint("directory_synced")?;
        provisional.disarm();
        Ok(Self {
            run_path,
            _lock: lock,
        })
    }

    fn state_path(&self) -> PathBuf {
        self.run_path.join("state.sqlite3")
    }

    fn close(self) -> Result<(), GateFailure> {
        remove_managed_run(&self.run_path)
    }
}

impl Drop for ManagedGateState {
    fn drop(&mut self) {
        let _ = remove_managed_run(&self.run_path);
    }
}

fn main() -> ExitCode {
    match run() {
        Ok(report) => match serde_json::to_string(&report) {
            Ok(serialized) => {
                println!("{serialized}");
                ExitCode::SUCCESS
            }
            Err(_) => fail(GateFailure::Assertion),
        },
        Err(error) => fail(error),
    }
}

fn fail(error: GateFailure) -> ExitCode {
    eprintln!(
        "{{\"schema\":1,\"status\":\"fail\",\"failure\":\"{}\"}}",
        error.category()
    );
    ExitCode::FAILURE
}

fn run() -> Result<GateReport, GateFailure> {
    if env::var(APPROVAL_ENV).ok().as_deref() != Some("1") {
        return Err(GateFailure::ApprovalMissing);
    }
    let task_id = required_uuid(TASK_ENV)?;
    let codex = required_absolute_file(CLI_ENV)?;
    let supervisor = required_absolute_file(SUPERVISOR_ENV)?;
    let prompt = read_prompt()?;
    if prompt.matches(QUEUED_TOKEN).count() != 1 || prompt.contains(MANUAL_TOKEN) {
        return Err(GateFailure::Input);
    }
    let request_id = Uuid::new_v4().to_string();
    let shutdown = Arc::new(AtomicBool::new(false));
    signal_hook::flag::register(signal_hook::consts::SIGINT, Arc::clone(&shutdown))
        .map_err(|_| GateFailure::Configuration)?;
    signal_hook::flag::register(signal_hook::consts::SIGTERM, Arc::clone(&shutdown))
        .map_err(|_| GateFailure::Configuration)?;

    let catalog = CodexTaskCatalog::from_environment().map_err(|_| GateFailure::Catalog)?;
    let catalog_failures = wait_for_allowlisted(&catalog, &task_id, &shutdown)?;
    let managed = ManagedGateState::create()?;
    let state_path = managed.state_path();

    let mut store = StateStore::open(&state_path).map_err(|_| GateFailure::Store)?;
    let (binding, binding_catalog_failures) =
        bind_with_retry(&catalog, &mut store, &task_id, &shutdown)?;
    if binding.slot != 1 || binding.generation != 1 || binding.task_id != task_id {
        return Err(GateFailure::Assertion);
    }

    let mut observer = RolloutObserver::new(catalog.clone());
    let baseline = wait_for_insertions(&mut observer, &mut store, 1, OBSERVER_TIMEOUT, &shutdown)?;
    if baseline.inserted != 1 || baseline.replayed != 0 {
        return Err(GateFailure::Assertion);
    }
    let baseline_facts = baseline_facts(&state_path, &task_id)?;
    if baseline_facts != (1, 1, 1, 1, 0) {
        return Err(GateFailure::Assertion);
    }

    drop(observer);
    drop(store);
    let mut store = StateStore::open(&state_path).map_err(|_| GateFailure::Store)?;
    let mut observer = RolloutObserver::new(catalog.clone());
    let first_restart = poll_quiet(&mut observer, &mut store, 5, &shutdown)?;
    if first_restart.inserted != 0 || first_restart.replayed != 0 {
        return Err(GateFailure::Assertion);
    }

    let queue = PromptQueueService::new(BindingService::new(&catalog));
    let (enqueue_outcome, enqueue_catalog_failures) =
        enqueue_with_retry(&queue, &mut store, &request_id, &prompt, &shutdown)?;
    if enqueue_outcome != EnqueueOutcome::Inserted {
        return Err(GateFailure::Assertion);
    }

    let runner = CodexRunner::new(CodexRunnerConfig {
        executable: codex,
        supervisor_executable: Some(supervisor),
        timeout: RUNNER_TIMEOUT,
        max_stdout_line_bytes: 256 * 1024,
        max_stdout_total_bytes: 4 * 1024 * 1024,
        max_stderr_bytes: 64 * 1024,
    });
    let mut scheduler = DurablePromptScheduler::new(runner);
    drive_scheduler(
        &mut scheduler,
        &mut store,
        &task_id,
        SCHEDULER_TIMEOUT,
        &shutdown,
    )?;
    let state = job_state(&state_path, &request_id)?;
    if state.as_deref() != Some("completed") {
        return Err(GateFailure::Runner);
    }

    let queued = wait_for_insertions(&mut observer, &mut store, 1, OBSERVER_TIMEOUT, &shutdown)?;
    if queued.inserted != 1 || queued.replayed != 0 {
        return Err(GateFailure::Assertion);
    }

    drop(scheduler);
    drop(observer);
    drop(store);
    let mut store = StateStore::open(&state_path).map_err(|_| GateFailure::Store)?;
    let mut observer = RolloutObserver::new(catalog);
    let second_restart = poll_quiet(&mut observer, &mut store, 5, &shutdown)?;
    check_interrupted(&shutdown)?;
    let facts = persisted_facts(&state_path, &task_id, &request_id, &prompt)?;
    if second_restart.inserted != 0
        || second_restart.replayed != 0
        || facts.completion_count != 2
        || facts.distinct_completion_count != 2
        || facts.job_state != "completed"
        || !facts.job_task_matches
        || !facts.prompt_matches
        || !facts.binding_matches
        || facts.manual_token_count != 1
        || facts.queued_token_count != 1
    {
        return Err(GateFailure::Assertion);
    }
    drop(observer);
    drop(store);
    check_interrupted(&shutdown)?;
    managed.close()?;

    Ok(GateReport {
        schema: 1,
        status: "pass",
        isolated_state: true,
        catalog_allowlisted: true,
        binding_slot: binding.slot,
        binding_generation: binding.generation,
        manual_completion_inserted: baseline.inserted,
        queued_completion_inserted: queued.inserted,
        restart_duplicate_inserted: first_restart.inserted + second_restart.inserted,
        restart_replay_reported: first_restart.replayed + second_restart.replayed,
        catalog_transient_failures: catalog_failures
            + binding_catalog_failures
            + enqueue_catalog_failures,
        observer_transient_failures: baseline.failed_tasks
            + first_restart.failed_tasks
            + queued.failed_tasks
            + second_restart.failed_tasks,
        total_completion_count: facts.completion_count,
        distinct_completion_count: facts.distinct_completion_count,
        job_state: "completed",
        job_task_matches: facts.job_task_matches,
        prompt_matches: facts.prompt_matches,
        manual_token_matches_once: facts.manual_token_count == 1,
        queued_token_matches_once: facts.queued_token_count == 1,
        private_identifiers_emitted: false,
        desktop_visibility: "external_check_required",
    })
}

fn required_uuid(name: &'static str) -> Result<String, GateFailure> {
    let value = env::var(name).map_err(|_| GateFailure::Configuration)?;
    Uuid::parse_str(&value).map_err(|_| GateFailure::Configuration)?;
    Ok(value)
}

fn required_absolute_file(name: &'static str) -> Result<PathBuf, GateFailure> {
    let path = PathBuf::from(env::var_os(name).ok_or(GateFailure::Configuration)?);
    if !path.is_absolute() || !path.is_file() {
        return Err(GateFailure::Configuration);
    }
    Ok(path)
}

fn read_prompt() -> Result<String, GateFailure> {
    let mut bytes = Vec::new();
    io::stdin()
        .take(MAX_PROMPT_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| GateFailure::Input)?;
    if bytes.is_empty() || bytes.len() as u64 > MAX_PROMPT_BYTES {
        return Err(GateFailure::Input);
    }
    String::from_utf8(bytes).map_err(|_| GateFailure::Input)
}

fn wait_for_allowlisted(
    catalog: &CodexTaskCatalog,
    task_id: &str,
    shutdown: &AtomicBool,
) -> Result<usize, GateFailure> {
    retry_catalog(shutdown, || match catalog.allowlisted(task_id) {
        Ok(_) => Ok(Some(())),
        Err(CatalogError::NotAllowlisted) => Err(GateFailure::Catalog),
        Err(_) => Ok(None),
    })
    .map(|(_, failures)| failures)
}

fn bind_with_retry(
    catalog: &CodexTaskCatalog,
    store: &mut StateStore,
    task_id: &str,
    shutdown: &AtomicBool,
) -> Result<(Binding, usize), GateFailure> {
    let service = BindingService::new(catalog);
    retry_catalog(shutdown, || match service.bind(store, 1, None, task_id) {
        Ok(Some(binding)) => Ok(Some(binding)),
        Ok(None) => Err(GateFailure::Binding),
        Err(BindingError::Catalog(CatalogError::NotAllowlisted)) => Err(GateFailure::Catalog),
        Err(BindingError::Catalog(_)) => Ok(None),
        Err(BindingError::Store(_)) => Err(GateFailure::Store),
    })
}

fn enqueue_with_retry(
    queue: &PromptQueueService<'_>,
    store: &mut StateStore,
    request_id: &str,
    prompt: &str,
    shutdown: &AtomicBool,
) -> Result<(EnqueueOutcome, usize), GateFailure> {
    retry_catalog(shutdown, || {
        match queue.enqueue_slot(store, 1, request_id, prompt) {
            Ok(outcome) => Ok(Some(outcome)),
            Err(PromptQueueError::UnboundSlot) => Err(GateFailure::QueueUnbound),
            Err(PromptQueueError::Binding(BindingError::Catalog(CatalogError::NotAllowlisted))) => {
                Err(GateFailure::Catalog)
            }
            Err(PromptQueueError::Binding(BindingError::Catalog(_))) => Ok(None),
            Err(PromptQueueError::Binding(BindingError::Store(_)))
            | Err(PromptQueueError::Store(_)) => Err(GateFailure::QueueStore),
        }
    })
}

fn retry_catalog<T>(
    shutdown: &AtomicBool,
    mut operation: impl FnMut() -> Result<Option<T>, GateFailure>,
) -> Result<(T, usize), GateFailure> {
    let deadline = Instant::now() + OBSERVER_TIMEOUT;
    let mut failures = 0_usize;
    loop {
        check_interrupted(shutdown)?;
        if let Some(value) = operation()? {
            return Ok((value, failures));
        }
        failures = failures.checked_add(1).ok_or(GateFailure::Assertion)?;
        if Instant::now() >= deadline {
            return Err(GateFailure::Timeout);
        }
        thread::sleep(POLL_DELAY);
    }
}

fn wait_for_insertions(
    observer: &mut RolloutObserver,
    store: &mut StateStore,
    expected: usize,
    timeout: Duration,
    shutdown: &AtomicBool,
) -> Result<ObserverTick, GateFailure> {
    let deadline = Instant::now() + timeout;
    let mut total = ObserverTick::default();
    let mut quiet_after_expected = 0_u8;
    loop {
        check_interrupted(shutdown)?;
        let tick = observer
            .poll_bound_tasks(store)
            .map_err(|_| GateFailure::Observer)?;
        if tick.failed_tasks != 0 {
            total.failed_tasks = total
                .failed_tasks
                .checked_add(tick.failed_tasks)
                .ok_or(GateFailure::Assertion)?;
            quiet_after_expected = 0;
            if Instant::now() >= deadline {
                return Err(GateFailure::Timeout);
            }
            thread::sleep(POLL_DELAY);
            continue;
        }
        total.inserted = total
            .inserted
            .checked_add(tick.inserted)
            .ok_or(GateFailure::Assertion)?;
        total.replayed = total
            .replayed
            .checked_add(tick.replayed)
            .ok_or(GateFailure::Assertion)?;
        if total.inserted > expected || total.replayed != 0 {
            return Err(GateFailure::Assertion);
        }
        if total.inserted == expected && tick.inserted == 0 && tick.replayed == 0 {
            quiet_after_expected += 1;
            if quiet_after_expected >= 3 {
                return Ok(total);
            }
        } else {
            quiet_after_expected = 0;
        }
        if Instant::now() >= deadline {
            return Err(GateFailure::Timeout);
        }
        thread::sleep(POLL_DELAY);
    }
}

fn poll_quiet(
    observer: &mut RolloutObserver,
    store: &mut StateStore,
    attempts: usize,
    shutdown: &AtomicBool,
) -> Result<ObserverTick, GateFailure> {
    let deadline = Instant::now() + OBSERVER_TIMEOUT;
    let mut total = ObserverTick::default();
    let mut successful = 0_usize;
    while successful < attempts {
        check_interrupted(shutdown)?;
        let tick = observer
            .poll_bound_tasks(store)
            .map_err(|_| GateFailure::Observer)?;
        if tick.failed_tasks != 0 {
            total.failed_tasks = total
                .failed_tasks
                .checked_add(tick.failed_tasks)
                .ok_or(GateFailure::Assertion)?;
            successful = 0;
            if Instant::now() >= deadline {
                return Err(GateFailure::Timeout);
            }
            thread::sleep(POLL_DELAY);
            continue;
        }
        total.inserted += tick.inserted;
        total.replayed += tick.replayed;
        successful += 1;
        thread::sleep(POLL_DELAY);
    }
    Ok(total)
}

fn drive_scheduler(
    scheduler: &mut DurablePromptScheduler,
    store: &mut StateStore,
    task_id: &str,
    timeout: Duration,
    shutdown: &AtomicBool,
) -> Result<(), GateFailure> {
    let deadline = Instant::now() + timeout;
    loop {
        if shutdown.load(Ordering::Acquire) {
            scheduler.shutdown_without_acknowledging();
            return Err(GateFailure::Interrupted);
        }
        scheduler.tick(store).map_err(|_| GateFailure::Store)?;
        if store
            .pending_count(task_id)
            .map_err(|_| GateFailure::Store)?
            == 0
            && scheduler.in_flight() == 0
        {
            return Ok(());
        }
        if Instant::now() >= deadline {
            scheduler.shutdown_without_acknowledging();
            return Err(GateFailure::Timeout);
        }
        thread::sleep(POLL_DELAY);
    }
}

fn check_interrupted(shutdown: &AtomicBool) -> Result<(), GateFailure> {
    if shutdown.load(Ordering::Acquire) {
        Err(GateFailure::Interrupted)
    } else {
        Ok(())
    }
}

fn read_only_connection(path: &Path) -> Result<Connection, GateFailure> {
    Connection::open_with_flags(
        path,
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )
    .map_err(|_| GateFailure::Store)
}

fn job_state(path: &Path, request_id: &str) -> Result<Option<String>, GateFailure> {
    read_only_connection(path)?
        .query_row(
            "SELECT state FROM jobs WHERE request_id = ?1",
            [request_id],
            |row| row.get(0),
        )
        .optional()
        .map_err(|_| GateFailure::Store)
}

fn persisted_facts(
    path: &Path,
    task_id: &str,
    request_id: &str,
    prompt: &str,
) -> Result<PersistedFacts, GateFailure> {
    let connection = read_only_connection(path)?;
    let (completion_count, distinct_completion_count, matching_task_count): (u32, u32, u32) =
        connection
            .query_row(
                "SELECT COUNT(*), COUNT(DISTINCT completion_id),
                        SUM(CASE WHEN task_id = ?1 THEN 1 ELSE 0 END)
                 FROM completion_ledger",
                [task_id],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .map_err(|_| GateFailure::Store)?;
    let (job_state, job_task, job_prompt): (String, String, String) = connection
        .query_row(
            "SELECT state, task_id, prompt FROM jobs WHERE request_id = ?1",
            [request_id],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
        )
        .map_err(|_| GateFailure::Store)?;
    let binding_matches: bool = connection
        .query_row(
            "SELECT EXISTS(
               SELECT 1 FROM bindings
               WHERE slot = 1 AND task_id = ?1 AND generation = 1
             )",
            [task_id],
            |row| row.get(0),
        )
        .map_err(|_| GateFailure::Store)?;
    let manual_token_count = token_count(&connection, MANUAL_TOKEN)?;
    let queued_token_count = token_count(&connection, QUEUED_TOKEN)?;
    Ok(PersistedFacts {
        completion_count,
        distinct_completion_count,
        job_state,
        job_task_matches: job_task == task_id && matching_task_count == completion_count,
        prompt_matches: job_prompt == prompt,
        binding_matches,
        manual_token_count,
        queued_token_count,
    })
}

fn baseline_facts(path: &Path, task_id: &str) -> Result<(u32, u32, u32, u32, u32), GateFailure> {
    let connection = read_only_connection(path)?;
    let (completion_count, distinct_completion_count, matching_task_count): (u32, u32, u32) =
        connection
            .query_row(
                "SELECT COUNT(*), COUNT(DISTINCT completion_id),
                        SUM(CASE WHEN task_id = ?1 THEN 1 ELSE 0 END)
                 FROM completion_ledger",
                [task_id],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .map_err(|_| GateFailure::Store)?;
    Ok((
        completion_count,
        distinct_completion_count,
        matching_task_count,
        token_count(&connection, MANUAL_TOKEN)?,
        token_count(&connection, QUEUED_TOKEN)?,
    ))
}

fn token_count(connection: &Connection, token: &str) -> Result<u32, GateFailure> {
    connection
        .query_row(
            "SELECT COUNT(*) FROM completion_ledger WHERE instr(turn_pack, ?1) > 0",
            [token],
            |row| row.get(0),
        )
        .map_err(|_| GateFailure::Store)
}

fn cleanup_stale_runs(root: &Path) -> Result<(), GateFailure> {
    let mut seen = 0_usize;
    for entry in fs::read_dir(root).map_err(|_| GateFailure::Store)? {
        let entry = entry.map_err(|_| GateFailure::Store)?;
        let Some(name) = entry.file_name().to_str().map(str::to_owned) else {
            continue;
        };
        let Some(raw_id) = name.strip_prefix(GATE_RUN_PREFIX) else {
            continue;
        };
        if Uuid::parse_str(raw_id).is_err() {
            continue;
        }
        seen = seen.checked_add(1).ok_or(GateFailure::Store)?;
        if seen > MAX_MANAGED_RUNS {
            return Err(GateFailure::Store);
        }
        let path = entry.path();
        validate_managed_run(&path)?;
        remove_managed_run(&path)?;
    }
    Ok(())
}

fn validate_managed_run(path: &Path) -> Result<(), GateFailure> {
    let metadata = fs::symlink_metadata(path).map_err(|_| GateFailure::Store)?;
    if !metadata.is_dir()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.permissions().mode() & 0o077 != 0
    {
        return Err(GateFailure::Store);
    }
    let marker_path = path.join(GATE_OWNER_FILE);
    let marker_metadata = match fs::symlink_metadata(&marker_path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            return if valid_partial_create_directory(path)? {
                Ok(())
            } else {
                Err(GateFailure::Store)
            };
        }
        Err(_) => return Err(GateFailure::Store),
    };
    if !marker_metadata.is_file()
        || marker_metadata.uid() != unsafe { libc::geteuid() }
        || marker_metadata.permissions().mode() & 0o077 != 0
        || marker_metadata.len() != GATE_OWNER_CONTENT.len() as u64
    {
        return Err(GateFailure::Store);
    }
    let marker = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(marker_path)
        .map_err(|_| GateFailure::Store)?;
    let mut contents = Vec::with_capacity(GATE_OWNER_CONTENT.len() + 1);
    marker
        .take(GATE_OWNER_CONTENT.len() as u64 + 1)
        .read_to_end(&mut contents)
        .map_err(|_| GateFailure::Store)?;
    if contents != GATE_OWNER_CONTENT {
        return Err(GateFailure::Store);
    }
    Ok(())
}

fn valid_partial_create_directory(path: &Path) -> Result<bool, GateFailure> {
    let entries = fs::read_dir(path)
        .map_err(|_| GateFailure::Store)?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| GateFailure::Store)?;
    if entries.is_empty() {
        return Ok(true);
    }
    if entries.len() != 1 || entries[0].file_name() != GATE_OWNER_TEMP_FILE {
        return Ok(false);
    }
    let metadata = entries[0].metadata().map_err(|_| GateFailure::Store)?;
    if !metadata.is_file()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.permissions().mode() & 0o077 != 0
        || metadata.len() > GATE_OWNER_CONTENT.len() as u64
    {
        return Ok(false);
    }
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(entries[0].path())
        .map_err(|_| GateFailure::Store)?;
    let mut contents = Vec::with_capacity(GATE_OWNER_CONTENT.len() + 1);
    file.take(GATE_OWNER_CONTENT.len() as u64 + 1)
        .read_to_end(&mut contents)
        .map_err(|_| GateFailure::Store)?;
    Ok(GATE_OWNER_CONTENT.starts_with(&contents))
}

fn remove_managed_run(path: &Path) -> Result<(), GateFailure> {
    match fs::symlink_metadata(path) {
        Ok(_) => {
            validate_managed_run(path)?;
            fs::remove_dir_all(path).map_err(|_| GateFailure::Store)
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(_) => Err(GateFailure::Store),
    }
}

#[cfg(test)]
mod tests {
    use std::process::{Command, Stdio};

    use super::*;

    const HELPER_MODE_ENV: &str = "ECI_M3_MANAGED_STATE_HELPER";
    const HELPER_ROOT_ENV: &str = "ECI_M3_MANAGED_STATE_ROOT";
    const HELPER_READY_ENV: &str = "ECI_M3_MANAGED_STATE_READY";

    #[test]
    fn managed_state_signal_helper() {
        let Ok(mode) = env::var(HELPER_MODE_ENV) else {
            return;
        };
        let root = PathBuf::from(env::var_os(HELPER_ROOT_ENV).unwrap());
        let ready = PathBuf::from(env::var_os(HELPER_READY_ENV).unwrap());
        let state = ManagedGateState::create_in(&root).unwrap();
        let mut private = open_private_file(&state.state_path()).unwrap();
        use std::io::Write;
        private.write_all(b"private gate fixture").unwrap();
        private.sync_all().unwrap();
        let shutdown = Arc::new(AtomicBool::new(false));
        if mode == "sigterm" {
            signal_hook::flag::register(signal_hook::consts::SIGTERM, Arc::clone(&shutdown))
                .unwrap();
        }
        fs::write(ready, b"ready").unwrap();

        if mode == "sigterm" {
            while !shutdown.load(Ordering::Acquire) {
                thread::sleep(Duration::from_millis(10));
            }
            drop(state);
            return;
        }
        loop {
            thread::sleep(Duration::from_secs(1));
        }
    }

    #[test]
    fn sigterm_cleans_current_state_and_sigkill_is_swept_on_next_start() {
        for mode in ["sigterm", "sigkill"] {
            let temp = tempfile::tempdir().unwrap();
            let root = temp.path().join("managed");
            let ready = temp.path().join("ready");
            let mut child = Command::new(env::current_exe().unwrap())
                .args([
                    "--exact",
                    "tests::managed_state_signal_helper",
                    "--nocapture",
                ])
                .env(HELPER_MODE_ENV, mode)
                .env(HELPER_ROOT_ENV, &root)
                .env(HELPER_READY_ENV, &ready)
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .spawn()
                .unwrap();
            let deadline = Instant::now() + Duration::from_secs(5);
            while !ready.is_file() {
                assert!(
                    Instant::now() < deadline,
                    "managed-state helper did not start"
                );
                thread::sleep(Duration::from_millis(10));
            }

            let signal = if mode == "sigterm" {
                libc::SIGTERM
            } else {
                libc::SIGKILL
            };
            assert_eq!(unsafe { libc::kill(child.id() as i32, signal) }, 0);
            let status = child.wait().unwrap();
            if mode == "sigterm" {
                assert!(status.success());
                assert_eq!(managed_run_count(&root), 0);
            } else {
                assert!(!status.success());
                assert_eq!(managed_run_count(&root), 1);
                let replacement = ManagedGateState::create_in(&root).unwrap();
                assert_eq!(managed_run_count(&root), 1);
                drop(replacement);
                assert_eq!(managed_run_count(&root), 0);
            }
        }
    }

    #[test]
    fn every_partial_create_checkpoint_is_cleaned_and_does_not_poison_next_start() {
        for failed_checkpoint in [
            "directory_created",
            "permissions_set",
            "marker_temp_opened",
            "marker_temp_truncated",
            "marker_temp_written",
            "marker_temp_synced",
            "marker_published",
            "directory_synced",
        ] {
            let temp = tempfile::tempdir().unwrap();
            let root = temp.path().join("managed");
            let result = ManagedGateState::create_in_with(&root, |checkpoint| {
                if checkpoint == failed_checkpoint {
                    Err(GateFailure::Store)
                } else {
                    Ok(())
                }
            });
            assert!(result.is_err());
            assert_eq!(managed_run_count(&root), 0);

            let next = ManagedGateState::create_in(&root).unwrap_or_else(|error| {
                panic!("next start failed after {failed_checkpoint}: {error:?}")
            });
            assert_eq!(managed_run_count(&root), 1);
            drop(next);
            assert_eq!(managed_run_count(&root), 0);
        }
    }

    #[test]
    fn partial_owner_publication_from_sigkill_is_swept_but_unknown_content_fails_closed() {
        for prefix_len in [0, 1, GATE_OWNER_CONTENT.len() / 2, GATE_OWNER_CONTENT.len()] {
            let temp = tempfile::tempdir().unwrap();
            let root = temp.path().join("managed");
            secure_directory(&root).unwrap();
            let stale = root.join(format!("{GATE_RUN_PREFIX}{}", Uuid::new_v4()));
            secure_directory(&stale).unwrap();
            let mut marker = open_private_file(&stale.join(GATE_OWNER_TEMP_FILE)).unwrap();
            use std::io::Write;
            marker.write_all(&GATE_OWNER_CONTENT[..prefix_len]).unwrap();
            marker.sync_all().unwrap();
            drop(marker);

            let next = ManagedGateState::create_in(&root).unwrap();
            assert_eq!(managed_run_count(&root), 1);
            drop(next);
            assert_eq!(managed_run_count(&root), 0);
        }

        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("managed");
        secure_directory(&root).unwrap();
        let stale = root.join(format!("{GATE_RUN_PREFIX}{}", Uuid::new_v4()));
        secure_directory(&stale).unwrap();
        fs::write(stale.join(GATE_OWNER_TEMP_FILE), b"not-our-marker").unwrap();
        assert!(ManagedGateState::create_in(&root).is_err());
        assert_eq!(managed_run_count(&root), 1);
    }

    fn managed_run_count(root: &Path) -> usize {
        fs::read_dir(root)
            .unwrap()
            .filter_map(Result::ok)
            .filter(|entry| {
                entry
                    .file_name()
                    .to_str()
                    .is_some_and(|name| name.starts_with(GATE_RUN_PREFIX))
            })
            .count()
    }
}

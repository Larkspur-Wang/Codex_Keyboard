use std::collections::HashSet;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
#[cfg(target_os = "macos")]
use std::os::fd::FromRawFd;
use std::os::unix::fs::{FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt};
#[cfg(target_os = "macos")]
use std::os::unix::net::UnixListener;
use std::os::unix::net::UnixStream;
use std::os::unix::process::{CommandExt, ExitStatusExt};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU8, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Mutex, OnceLock};
use std::thread;
use std::time::{Duration, Instant};

use fs2::FileExt;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use tempfile::{Builder, TempDir};
use thiserror::Error;
use zeroize::{Zeroize, Zeroizing};

use crate::codex_runner::{
    SUPERVISOR_CLI_MISSING_EXIT, SUPERVISOR_IO_EXIT, discover_codex_executable,
};
use crate::paths::{
    AppPaths, ExplicitFileLock, open_owned_directory_chain, open_private_file, secure_directory,
};
use crate::rollout_observer::{TurnPack, redact_sensitive_text};
use crate::store::{MAX_SUMMARY_COMPLETIONS_PER_CLAIM, PendingSummaryCompletion, SummaryClaim};
use crate::summary::{
    MAX_SUMMARY_DOCUMENT_BYTES, SummaryDocument, required_source_evidence_quote,
    source_evidence_quote_budget,
};

pub const SPARK_MODEL: &str = "gpt-5.3-codex-spark";
const DISABLED_SPARK_FEATURES: &[&str] = &[
    "plugins",
    "remote_plugin",
    "plugin_sharing",
    "skill_search",
    "skill_mcp_dependency_install",
    "memories",
    "goals",
    "hooks",
    "apps",
    "enable_mcp_apps",
    "shell_snapshot",
    "workspace_dependencies",
    "multi_agent",
    "multi_agent_v2",
    "image_generation",
    "browser_use",
    "browser_use_external",
    "browser_use_full_cdp_access",
    "computer_use",
    "in_app_browser",
    "artifact",
    "code_mode",
    "code_mode_host",
    "tool_suggest",
    "auth_elicitation",
    "request_permissions_tool",
    "external_agent_memory_import",
    "shell_tool",
    "unified_exec",
];
const MAX_AUTH_BYTES: u64 = 64 * 1024;
const MAX_TURN_PACK_BYTES: usize = 64 * 1024;
const MAX_TURN_PACK_TOTAL_BYTES: usize = 1024 * 1024;
const MAX_PROMPT_BYTES: usize = 2 * 1024 * 1024;
const MAX_CHILD_FILE_BYTES: libc::rlim_t = 16 * 1024 * 1024;
pub const SPARK_MAX_WORKSPACE_BYTES: u64 = 8 * 1024 * 1024;
pub const SPARK_MAX_WORKSPACE_NODES: usize = 32;
const MAX_RUNTIME_DATABASE_BYTES: u64 = 2 * 1024 * 1024;
const MAX_RUNTIME_WAL_BYTES: u64 = 4 * 1024 * 1024;
const MAX_RUNTIME_SHM_BYTES: u64 = 256 * 1024;
const MAX_MODEL_CACHE_BYTES: u64 = 2 * 1024 * 1024;
const MAX_STDOUT_BYTES: usize = 4 * 1024 * 1024;
const MAX_STDERR_BYTES: usize = 64 * 1024;
const OUTPUT_OK: u8 = 0;
const OUTPUT_TOO_LARGE: u8 = 1;
const OUTPUT_IO: u8 = 2;
const SPARK_SUPERVISOR_READY: u8 = 0xfe;

#[derive(Default)]
struct SparkProcessState {
    live_runs: HashSet<PathBuf>,
    live_tasks: HashSet<PathBuf>,
}

fn spark_process_state() -> &'static Mutex<SparkProcessState> {
    static STATE: OnceLock<Mutex<SparkProcessState>> = OnceLock::new();
    STATE.get_or_init(|| Mutex::new(SparkProcessState::default()))
}

#[derive(Debug, Error, Clone, Copy, PartialEq, Eq)]
pub enum SparkError {
    #[error("Codex CLI is unavailable")]
    CliMissing,
    #[error("Codex authentication is missing or unsafe")]
    Authentication,
    #[error("Spark temporary workspace is unsafe")]
    UnsafeWorkspace,
    #[error("Spark input is invalid or exceeds a fixed bound")]
    InvalidInput,
    #[error("Spark output is invalid")]
    InvalidOutput,
    #[error("Spark output exceeds a fixed bound")]
    OutputTooLarge,
    #[error("Spark invocation timed out")]
    Timeout,
    #[error("Spark invocation was cancelled")]
    Cancelled,
    #[error("Spark summary generation is already running for this task")]
    Busy,
    #[error("Spark process I/O failed")]
    ProcessIo,
    #[error("Spark provider rate limit was reached")]
    RateLimited,
    #[error("Spark provider or transport is temporarily unavailable")]
    ServiceUnavailable,
    #[error("Spark child hit its per-file resource limit")]
    ResourceLimit,
    #[error("requested Spark model is unavailable for this account or client")]
    ModelUnavailable,
    #[error("Spark local runtime initialization failed")]
    LocalRuntime,
    #[error("Spark process failed without a diagnostic on stderr")]
    SilentExitFailure,
    #[error("Spark process exited unsuccessfully")]
    ExitFailure,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SparkIsolationReport {
    pub workspace_max_nodes: usize,
    pub workspace_max_bytes: u64,
    pub system_skill_files: usize,
    pub plugin_files: usize,
    pub persistent_runtime_rows: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SparkRunOutcome {
    pub document: SummaryDocument,
    pub isolation: SparkIsolationReport,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SparkPromptIsolationReport {
    pub prompt_items: usize,
    pub skills_instruction_blocks: usize,
    pub skill_path_mentions: usize,
    pub workspace_max_nodes: usize,
    pub workspace_max_bytes: u64,
}

#[derive(Debug, Clone)]
pub struct SparkRunnerConfig {
    pub executable: PathBuf,
    pub supervisor_executable: Option<PathBuf>,
    pub auth_path: PathBuf,
    pub temp_root: PathBuf,
    pub timeout: Duration,
    pub max_stdout_bytes: usize,
    pub max_stderr_bytes: usize,
}

impl Default for SparkRunnerConfig {
    fn default() -> Self {
        let home = std::env::var_os("HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("/nonexistent"));
        let codex_home = std::env::var_os("CODEX_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| home.join(".codex"));
        Self {
            executable: discover_codex_executable(),
            supervisor_executable: std::env::current_exe().ok(),
            auth_path: codex_home.join("auth.json"),
            temp_root: AppPaths::from_home(&home).runtime_directory.join("spark"),
            timeout: Duration::from_secs(5 * 60),
            max_stdout_bytes: MAX_STDOUT_BYTES,
            max_stderr_bytes: MAX_STDERR_BYTES,
        }
    }
}

#[derive(Debug, Clone)]
pub struct SparkRunner {
    config: SparkRunnerConfig,
}

impl SparkRunner {
    pub fn new(config: SparkRunnerConfig) -> Self {
        Self { config }
    }

    pub fn run(
        &self,
        claim: &SummaryClaim,
        previous_unheard: Option<&SummaryDocument>,
    ) -> Result<SummaryDocument, SparkError> {
        self.run_with_report(claim, previous_unheard)
            .map(|outcome| outcome.document)
    }

    pub fn run_with_report(
        &self,
        claim: &SummaryClaim,
        previous_unheard: Option<&SummaryDocument>,
    ) -> Result<SparkRunOutcome, SparkError> {
        self.run_with_cancel_report(claim, previous_unheard, &AtomicBool::new(false))
    }

    pub fn run_with_cancel(
        &self,
        claim: &SummaryClaim,
        previous_unheard: Option<&SummaryDocument>,
        cancel: &AtomicBool,
    ) -> Result<SummaryDocument, SparkError> {
        self.run_with_cancel_report(claim, previous_unheard, cancel)
            .map(|outcome| outcome.document)
    }

    fn run_with_cancel_report(
        &self,
        claim: &SummaryClaim,
        previous_unheard: Option<&SummaryDocument>,
        cancel: &AtomicBool,
    ) -> Result<SparkRunOutcome, SparkError> {
        validate_claim_input(claim, previous_unheard)?;
        let expected_covers = claim
            .completions
            .iter()
            .map(|completion| completion.completion_id.clone())
            .collect::<Vec<_>>();
        let prompt = build_prompt(claim, previous_unheard)?;
        let mut auth = AuthSnapshot::load(&self.config.auth_path)?;
        secure_directory(&self.config.temp_root).map_err(|_| SparkError::UnsafeWorkspace)?;
        let run = SparkRunFiles::create(
            &self.config.temp_root,
            auth.bytes.as_slice(),
            &claim.task_id,
        )?;
        auth.bytes.zeroize();
        auth.bytes.clear();
        auth.verify_source(&self.config.auth_path)?;
        let output_monitor = OutputFileMonitor::new(&run.output, &run.output_file)?;
        let auth_monitor = AuthCopyMonitor::new(&run.codex_home.join("auth.json"), &run.auth_file)?;
        let workspace_monitor = SparkWorkspaceMonitor::new(
            &run.codex_home,
            &run.auth_file,
            &run.sandbox_marker_file,
            &run.skills_dir,
            &run.home_tmpdir,
            &run.home_arg0dir,
            &run.tmpdir,
            &self.config.executable,
        )?;

        let supervise = cfg!(target_os = "macos") && self.config.supervisor_executable.is_some();
        let (mut parent_control, child_control) = if supervise {
            let (parent, child) = UnixStream::pair().map_err(|_| SparkError::ProcessIo)?;
            (Some(parent), Some(child))
        } else {
            (None, None)
        };
        let control_fd = child_control.as_ref().map(AsRawFd::as_raw_fd);
        let mut command = if supervise {
            let mut command = Command::new(self.config.supervisor_executable.as_ref().unwrap());
            command.args([
                "spark-supervisor",
                &std::process::id().to_string(),
                &control_fd.unwrap().to_string(),
                &run.owner_lock_fd().to_string(),
                &run.task_lock_fd().to_string(),
            ]);
            command.env_clear();
            command.env("ECI_SPARK_EXECUTABLE", &self.config.executable);
            command.env("ECI_SPARK_CODEX_HOME", &run.codex_home);
            command.env("ECI_SPARK_WORKDIR", &run.workdir);
            command.env("ECI_SPARK_SCHEMA", &run.schema);
            command.env("ECI_SPARK_OUTPUT", &run.output);
            command.env("ECI_SPARK_TMPDIR", &run.tmpdir);
            command.env("ECI_SPARK_OWNER_GUARDIAN", run.owner_guardian_path());
            command.env("ECI_SPARK_TASK_GUARDIAN", run.task_guardian_path());
            inherit_proxy_environment(&mut command);
            command
        } else {
            spark_command(
                &self.config.executable,
                &run.codex_home,
                &run.workdir,
                &run.schema,
                &run.output,
                &run.tmpdir,
            )
        };
        let owner_lock_fd = run.owner_lock_fd();
        let task_lock_fd = run.task_lock_fd();
        command
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        unsafe {
            command.pre_exec(move || {
                if libc::setpgid(0, 0) != 0 {
                    return Err(std::io::Error::last_os_error());
                }
                if let Some(control_fd) = control_fd
                    && libc::fcntl(control_fd, libc::F_SETFD, 0) != 0
                {
                    return Err(std::io::Error::last_os_error());
                }
                for lock_fd in [owner_lock_fd, task_lock_fd] {
                    if libc::fcntl(lock_fd, libc::F_SETFD, 0) != 0 {
                        return Err(std::io::Error::last_os_error());
                    }
                }
                let file_limit = libc::rlimit {
                    rlim_cur: MAX_CHILD_FILE_BYTES,
                    rlim_max: MAX_CHILD_FILE_BYTES,
                };
                if libc::setrlimit(libc::RLIMIT_FSIZE, &file_limit) != 0 {
                    return Err(std::io::Error::last_os_error());
                }
                #[cfg(target_os = "linux")]
                {
                    if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL) != 0 {
                        return Err(std::io::Error::last_os_error());
                    }
                    if libc::getppid() == 1 {
                        return Err(std::io::Error::from(std::io::ErrorKind::BrokenPipe));
                    }
                }
                Ok(())
            });
        }
        if cancel.load(Ordering::Acquire) {
            return Err(SparkError::Cancelled);
        }
        let deadline = Instant::now() + self.config.timeout;
        let mut child = command.spawn().map_err(|error| {
            if error.kind() == std::io::ErrorKind::NotFound {
                SparkError::CliMissing
            } else {
                SparkError::ProcessIo
            }
        })?;
        let process_group = child.id() as i32;
        let mut startup_guard = ChildGuard::new(child, process_group);
        drop(child_control);
        if let Some(control) = parent_control.as_mut() {
            control
                .set_nonblocking(true)
                .map_err(|_| SparkError::ProcessIo)?;
            let mut ready = [0_u8; 1];
            loop {
                match control.read(&mut ready) {
                    Ok(1) if ready[0] == SPARK_SUPERVISOR_READY => break,
                    Ok(1) | Ok(0) => {
                        startup_guard.kill_and_wait();
                        return Err(SparkError::ProcessIo);
                    }
                    Ok(_) => unreachable!(),
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {}
                    Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
                    Err(_) => {
                        startup_guard.kill_and_wait();
                        return Err(SparkError::ProcessIo);
                    }
                }
                match startup_guard.child_mut().try_wait() {
                    Ok(Some(_)) | Err(_) => {
                        startup_guard.kill_and_wait();
                        return Err(SparkError::ProcessIo);
                    }
                    Ok(None) => {}
                }
                if let Err(error) = workspace_monitor.check() {
                    startup_guard.kill_and_wait();
                    return Err(error);
                }
                if cancel.load(Ordering::Acquire) {
                    startup_guard.kill_and_wait();
                    return Err(SparkError::Cancelled);
                }
                if Instant::now() >= deadline {
                    startup_guard.kill_and_wait();
                    return Err(SparkError::Timeout);
                }
                thread::sleep(Duration::from_millis(10));
            }
        }
        child = startup_guard.into_child();
        self.drive_child(
            child,
            parent_control,
            prompt,
            &output_monitor,
            &auth_monitor,
            &workspace_monitor,
            cancel,
            deadline,
        )?;
        workspace_monitor.check()?;
        validate_runtime_databases(&run.codex_home)?;
        auth.verify_source(&self.config.auth_path)?;
        let bytes = read_bounded_output(&run.output, output_monitor.identity)?;
        let mut document = SummaryDocument::parse(&bytes).map_err(|_| SparkError::InvalidOutput)?;
        document
            .validate_expected_covers(&expected_covers)
            .map_err(|_| SparkError::InvalidOutput)?;
        redact_document(&mut document);
        document
            .validate_expected_covers(&expected_covers)
            .map_err(|_| SparkError::InvalidOutput)?;
        Ok(SparkRunOutcome {
            document,
            isolation: workspace_monitor.report(),
        })
    }

    #[allow(clippy::too_many_arguments)]
    fn drive_child(
        &self,
        mut child: Child,
        mut control: Option<UnixStream>,
        prompt: Zeroizing<Vec<u8>>,
        output_monitor: &OutputFileMonitor,
        auth_monitor: &AuthCopyMonitor,
        workspace_monitor: &SparkWorkspaceMonitor,
        cancel: &AtomicBool,
        deadline: Instant,
    ) -> Result<(), SparkError> {
        let mut stdin = child.stdin.take().ok_or(SparkError::ProcessIo)?;
        let stdout = child.stdout.take().ok_or(SparkError::ProcessIo)?;
        let stderr = child.stderr.take().ok_or(SparkError::ProcessIo)?;
        let process_group = child.id() as i32;
        let mut guard = ChildGuard::new(child, process_group);

        let output_state = Arc::new(AtomicU8::new(OUTPUT_OK));
        let stdout_state = Arc::clone(&output_state);
        let stdout_limit = self.config.max_stdout_bytes;
        let stdout_reader =
            thread::spawn(move || drain_bounded(stdout, stdout_limit, stdout_state));
        let stderr_limit = self.config.max_stderr_bytes;
        let stderr_reader = thread::spawn(move || read_stderr(stderr, stderr_limit));
        let stdin_writer = thread::spawn(move || {
            stdin
                .write_all(prompt.as_slice())
                .and_then(|_| stdin.flush())
                .is_ok()
        });
        let supervised = control.is_some();
        let (status, supervisor_code) = loop {
            if let Err(error) = auth_monitor.check() {
                guard.kill_and_wait();
                join_io_threads(stdout_reader, stderr_reader, stdin_writer);
                return Err(error);
            }
            if let Err(error) = output_monitor.check() {
                guard.kill_and_wait();
                join_io_threads(stdout_reader, stderr_reader, stdin_writer);
                return Err(error);
            }
            if let Err(error) = workspace_monitor.check() {
                guard.kill_and_wait();
                join_io_threads(stdout_reader, stderr_reader, stdin_writer);
                return Err(error);
            }
            if let Some(control) = control.as_mut() {
                let mut code = [0_u8; 1];
                match control.read(&mut code) {
                    Ok(1) => {
                        guard.kill_and_wait();
                        break (None, Some(code[0]));
                    }
                    Ok(0) => {}
                    Ok(_) => unreachable!(),
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {}
                    Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
                    Err(_) => {
                        guard.kill_and_wait();
                        join_io_threads(stdout_reader, stderr_reader, stdin_writer);
                        return Err(SparkError::ProcessIo);
                    }
                }
            }
            match guard.child_mut().try_wait() {
                Ok(Some(status)) => break (Some(status), None),
                Ok(None) => {}
                Err(_) => {
                    guard.kill_and_wait();
                    join_io_threads(stdout_reader, stderr_reader, stdin_writer);
                    return Err(SparkError::ProcessIo);
                }
            }
            match output_state.load(Ordering::Acquire) {
                OUTPUT_TOO_LARGE => {
                    guard.kill_and_wait();
                    join_io_threads(stdout_reader, stderr_reader, stdin_writer);
                    return Err(SparkError::OutputTooLarge);
                }
                OUTPUT_IO => {
                    guard.kill_and_wait();
                    join_io_threads(stdout_reader, stderr_reader, stdin_writer);
                    return Err(SparkError::ProcessIo);
                }
                _ => {}
            }
            if Instant::now() >= deadline {
                guard.kill_and_wait();
                join_io_threads(stdout_reader, stderr_reader, stdin_writer);
                return Err(SparkError::Timeout);
            }
            if cancel.load(Ordering::Acquire) {
                guard.kill_and_wait();
                join_io_threads(stdout_reader, stderr_reader, stdin_writer);
                return Err(SparkError::Cancelled);
            }
            thread::sleep(Duration::from_millis(10));
        };
        if status.is_some() {
            guard.disarm();
        }
        let stdout_result = stdout_reader.join().unwrap_or(OUTPUT_IO);
        let stderr = stderr_reader
            .join()
            .unwrap_or_else(|_| Zeroizing::new(Vec::new()));
        let stdin_succeeded = stdin_writer.join().unwrap_or(false);
        if supervisor_code == Some(SUPERVISOR_CLI_MISSING_EXIT as u8)
            || (supervised
                && status.as_ref().and_then(|status| status.code())
                    == Some(SUPERVISOR_CLI_MISSING_EXIT))
        {
            return Err(SparkError::CliMissing);
        }
        if supervisor_code == Some(SUPERVISOR_IO_EXIT as u8)
            || (supervised
                && status.as_ref().and_then(|status| status.code()) == Some(SUPERVISOR_IO_EXIT))
        {
            return Err(SparkError::ProcessIo);
        }
        if stdout_result == OUTPUT_TOO_LARGE {
            return Err(SparkError::OutputTooLarge);
        }
        if stdout_result != OUTPUT_OK || !stdin_succeeded {
            return Err(SparkError::ProcessIo);
        }
        let succeeded = supervisor_code == Some(0)
            || status
                .as_ref()
                .is_some_and(std::process::ExitStatus::success);
        if !succeeded {
            if status.as_ref().and_then(|status| status.signal()).is_some() {
                return Err(SparkError::ExitFailure);
            }
            return Err(classify_stderr(&stderr));
        }
        Ok(())
    }
}

#[derive(Serialize)]
struct PromptInput<'a> {
    schema: u8,
    previous_unheard: Option<&'a SummaryDocument>,
    new_completions: &'a [AssistantCompletion<'a>],
}

#[derive(Serialize)]
struct AssistantCompletion<'a> {
    completion_id: &'a str,
    assistant_final: String,
    required_evidence_quote: String,
}

fn validate_claim_input(
    claim: &SummaryClaim,
    previous_unheard: Option<&SummaryDocument>,
) -> Result<(), SparkError> {
    if uuid::Uuid::parse_str(&claim.task_id).is_err()
        || claim.generation == 0
        || claim.completions.is_empty()
        || claim.completions.len() > MAX_SUMMARY_COMPLETIONS_PER_CLAIM
        || claim.previous_unread.is_some() != previous_unheard.is_some()
        || claim
            .previous_unread
            .as_ref()
            .is_some_and(|previous| previous.task_id != claim.task_id)
    {
        return Err(SparkError::InvalidInput);
    }
    let mut total = 0_usize;
    for completion in &claim.completions {
        total = total
            .checked_add(completion.turn_pack.len())
            .ok_or(SparkError::InvalidInput)?;
        if uuid::Uuid::parse_str(&completion.completion_id).is_err()
            || completion.turn_pack.is_empty()
            || completion.turn_pack.len() > MAX_TURN_PACK_BYTES
            || total > MAX_TURN_PACK_TOTAL_BYTES
            || !valid_assistant_turn_pack(completion)
        {
            return Err(SparkError::InvalidInput);
        }
    }
    if previous_unheard.is_some_and(|document| document.validate().is_err()) {
        return Err(SparkError::InvalidInput);
    }
    Ok(())
}

fn build_prompt(
    claim: &SummaryClaim,
    previous_unheard: Option<&SummaryDocument>,
) -> Result<Zeroizing<Vec<u8>>, SparkError> {
    const INSTRUCTIONS: &[u8] = b"Create a concrete cumulative unread task summary from the JSON input below. Each new completion contains only the authoritative final assistant reply from one completed task turn. Summarize only what those assistant_final fields reported: the user-visible result, still-relevant next work, and explicit decisions. Do not invent or reconstruct user messages, tool calls, intermediate progress, tests, logs, hidden reasoning, or implementation details that are not useful to the user. For every new completion, source_evidence must contain exactly one item in the same order: copy completion_id and copy required_evidence_quote exactly into exact_quote. source_evidence is private audit metadata, not narration: do not paste required_evidence_quote into spoken_text merely to satisfy validation. When previous_unheard is present, it is required cumulative context: preserve its facts, pending, and decisions in the matching output arrays, then compress and naturally re-summarize only the still-relevant old unread content together with every new completion. Do not copy previous spoken_text verbatim and do not narrate a chronological history. Write spoken_text as a natural Simplified Chinese briefing of at most 480 letters, digits, or Han characters. Lead immediately with the newest concrete user-visible result, prioritizing the last new completion, then briefly fold in other material unread outcomes or decisions. End with a next action only when the source contains a real actionable next step; never add a generic closing such as saying work can continue. For an ordinary batch of one to five completions, aim for roughly 120 to 180 Chinese characters, but extend when necessary rather than dropping materially useful meaning. It must be self-contained and say what was actually completed, what result matters, and any relevant next step or decision; do not merely say that a task is done. Omit test commands, validation mechanics, and implementation detail unless the user must act on them. The TTS reads spoken_text exactly, so never include schema labels, evidence excerpts, validation notes, section headings, or boilerplate that a person should not hear. Return only the output-schema JSON. covers_new_completions must exactly equal the ordered completion_id values in new_completions. Never emit credentials, hidden reasoning, or local absolute paths.\n";
    let completions = assistant_completions(claim)?;
    let mut prompt = BoundedSensitiveWriter::new(MAX_PROMPT_BYTES);
    prompt
        .write_all(INSTRUCTIONS)
        .map_err(|_| SparkError::InvalidInput)?;
    serde_json::to_writer(
        &mut prompt,
        &PromptInput {
            schema: 1,
            previous_unheard,
            new_completions: &completions,
        },
    )
    .map_err(|_| SparkError::InvalidInput)?;
    Ok(prompt.into_bytes())
}

fn valid_assistant_turn_pack(completion: &PendingSummaryCompletion) -> bool {
    serde_json::from_str::<TurnPack>(&completion.turn_pack).is_ok_and(|pack| {
        pack.v == 1
            && pack.turn_id == completion.completion_id
            && pack.assistant.len() == 1
            && !pack.assistant[0].trim().is_empty()
    })
}

fn assistant_completions(claim: &SummaryClaim) -> Result<Vec<AssistantCompletion<'_>>, SparkError> {
    let quote_budget = source_evidence_quote_budget(claim.completions.len());
    claim
        .completions
        .iter()
        .map(|completion| {
            let pack: TurnPack = serde_json::from_str(&completion.turn_pack)
                .map_err(|_| SparkError::InvalidInput)?;
            if pack.v != 1 || pack.turn_id != completion.completion_id {
                return Err(SparkError::InvalidInput);
            }
            if pack.assistant.len() != 1 || pack.assistant[0].trim().is_empty() {
                return Err(SparkError::InvalidInput);
            }
            let assistant_final = pack.assistant.into_iter().next().unwrap();
            let required_evidence_quote =
                required_source_evidence_quote(&assistant_final, quote_budget)
                    .ok_or(SparkError::InvalidInput)?;
            Ok(AssistantCompletion {
                completion_id: &completion.completion_id,
                assistant_final,
                required_evidence_quote,
            })
        })
        .collect()
}

fn redact_document(document: &mut SummaryDocument) {
    for value in document
        .facts
        .iter_mut()
        .chain(document.pending.iter_mut())
        .chain(document.decisions.iter_mut())
    {
        let redacted = redact_sensitive_text(value);
        value.zeroize();
        *value = redacted;
    }
    for evidence in &mut document.source_evidence {
        let redacted = redact_sensitive_text(&evidence.exact_quote);
        evidence.exact_quote.zeroize();
        evidence.exact_quote = redacted;
    }
    let spoken_text = redact_sensitive_text(&document.spoken_text);
    document.spoken_text.zeroize();
    document.spoken_text = spoken_text;
}

struct BoundedSensitiveWriter {
    bytes: Zeroizing<Vec<u8>>,
    limit: usize,
}

impl BoundedSensitiveWriter {
    fn new(limit: usize) -> Self {
        Self {
            bytes: Zeroizing::new(Vec::with_capacity(limit)),
            limit,
        }
    }

    fn into_bytes(self) -> Zeroizing<Vec<u8>> {
        self.bytes
    }
}

impl Write for BoundedSensitiveWriter {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        let next = self
            .bytes
            .len()
            .checked_add(bytes.len())
            .filter(|next| *next <= self.limit)
            .ok_or_else(|| std::io::Error::other("sensitive buffer limit exceeded"))?;
        self.bytes.extend_from_slice(bytes);
        debug_assert_eq!(self.bytes.len(), next);
        Ok(bytes.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

struct SparkRunFiles {
    root: Option<TempDir>,
    owner_lock: ExplicitFileLock,
    task_lock: ExplicitFileLock,
    auth_file: File,
    sandbox_marker_file: File,
    output_file: File,
    owner_guardian: PathBuf,
    task_guardian: PathBuf,
    registry_task: PathBuf,
    codex_home: PathBuf,
    skills_dir: PathBuf,
    home_tmpdir: PathBuf,
    home_arg0dir: PathBuf,
    workdir: PathBuf,
    schema: PathBuf,
    output: PathBuf,
    tmpdir: PathBuf,
}

struct PrivateTempDir(Option<TempDir>);

impl PrivateTempDir {
    fn new(directory: TempDir) -> Self {
        Self(Some(directory))
    }

    fn path(&self) -> &Path {
        self.0.as_ref().expect("private tempdir is live").path()
    }
}

impl Drop for PrivateTempDir {
    fn drop(&mut self) {
        if let Some(directory) = self.0.take() {
            make_private_tree_writable_best_effort(directory.path());
            let _ = directory.close();
        }
    }
}

impl SparkRunFiles {
    fn create(temp_root: &Path, auth: &[u8], task_id: &str) -> Result<Self, SparkError> {
        let mut process_state = spark_process_state()
            .lock()
            .map_err(|_| SparkError::UnsafeWorkspace)?;
        let sweep_lock = open_private_file(&temp_root.join(".sweep.lock"))
            .map_err(|_| SparkError::UnsafeWorkspace)?;
        sweep_lock
            .lock_exclusive()
            .map_err(|_| SparkError::UnsafeWorkspace)?;
        let sweep_lock = ExplicitFileLock::from_locked(sweep_lock);
        let task_lock_path = temp_root.join(task_lock_name(task_id));
        if process_state.live_tasks.contains(&task_lock_path) {
            return Err(SparkError::Busy);
        }
        let task_guardian_path = temp_root.join(task_guardian_socket_name(task_id));
        remove_stale_guardian_or_report_busy(&task_guardian_path)?;
        let task_lock =
            open_private_file(&task_lock_path).map_err(|_| SparkError::UnsafeWorkspace)?;
        match task_lock.try_lock_exclusive() {
            Ok(()) => {}
            Err(error) if error.kind() == fs2::lock_contended_error().kind() => {
                return Err(SparkError::Busy);
            }
            Err(_) => return Err(SparkError::UnsafeWorkspace),
        }
        let task_lock = ExplicitFileLock::from_locked(task_lock);
        sweep_stale_runs(temp_root, &process_state.live_runs)?;
        let root = Builder::new()
            .prefix("spark-")
            .tempdir_in(temp_root)
            .map_err(|_| SparkError::UnsafeWorkspace)?;
        fs::set_permissions(root.path(), fs::Permissions::from_mode(0o700))
            .map_err(|_| SparkError::UnsafeWorkspace)?;
        let owner_lock = open_private_file(&root.path().join("owner.lock"))
            .map_err(|_| SparkError::UnsafeWorkspace)?;
        owner_lock
            .lock_exclusive()
            .map_err(|_| SparkError::UnsafeWorkspace)?;
        let owner_lock = ExplicitFileLock::from_locked(owner_lock);
        let owner_guardian = root.path().join("guardian.sock");
        let codex_home = root.path().join("home");
        let workdir = root.path().join("work");
        let tmpdir = root.path().join("tmp");
        for directory in [&codex_home, &workdir, &tmpdir] {
            fs::create_dir(directory).map_err(|_| SparkError::UnsafeWorkspace)?;
            fs::set_permissions(directory, fs::Permissions::from_mode(0o700))
                .map_err(|_| SparkError::UnsafeWorkspace)?;
        }
        let auth_file = write_new_file(&codex_home.join("auth.json"), auth, 0o400)?;
        let sandbox_marker_file =
            write_new_file(&codex_home.join(".sandbox_migration"), b"v1\n", 0o400)?;
        let skills_dir = codex_home.join("skills");
        let home_tmpdir = codex_home.join("tmp");
        for directory in [&skills_dir, &home_tmpdir] {
            fs::create_dir(directory).map_err(|_| SparkError::UnsafeWorkspace)?;
        }
        let home_arg0dir = home_tmpdir.join("arg0");
        fs::create_dir(&home_arg0dir).map_err(|_| SparkError::UnsafeWorkspace)?;
        fs::set_permissions(&home_arg0dir, fs::Permissions::from_mode(0o700))
            .map_err(|_| SparkError::UnsafeWorkspace)?;
        for directory in [&skills_dir, &home_tmpdir] {
            fs::set_permissions(directory, fs::Permissions::from_mode(0o500))
                .map_err(|_| SparkError::UnsafeWorkspace)?;
        }
        let schema = root.path().join("summary.schema.json");
        let output = root.path().join("summary.output.json");
        drop(write_new_file(&schema, output_schema().as_bytes(), 0o400)?);
        let output_file = write_new_file(&output, b"", 0o600)?;
        fs::set_permissions(&workdir, fs::Permissions::from_mode(0o500))
            .map_err(|_| SparkError::UnsafeWorkspace)?;
        drop(sweep_lock);
        let registry_run = root.path().to_path_buf();
        process_state.live_runs.insert(registry_run);
        process_state.live_tasks.insert(task_lock_path.clone());
        Ok(Self {
            root: Some(root),
            owner_lock,
            task_lock,
            auth_file,
            sandbox_marker_file,
            output_file,
            owner_guardian,
            task_guardian: task_guardian_path,
            registry_task: task_lock_path,
            codex_home,
            skills_dir,
            home_tmpdir,
            home_arg0dir,
            workdir,
            schema,
            output,
            tmpdir,
        })
    }

    fn owner_lock_fd(&self) -> i32 {
        self.owner_lock.as_raw_fd()
    }

    fn task_lock_fd(&self) -> i32 {
        self.task_lock.as_raw_fd()
    }

    fn owner_guardian_path(&self) -> &Path {
        &self.owner_guardian
    }

    fn task_guardian_path(&self) -> &Path {
        &self.task_guardian
    }
}

fn task_lock_name(task_id: &str) -> String {
    format!(".task-{}.lock", hex_digest(task_id.as_bytes()))
}

fn task_guardian_socket_name(task_id: &str) -> String {
    let digest = hex_digest(task_id.as_bytes());
    format!(".g-{}.sock", &digest[..20])
}

fn hex_digest(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    let mut encoded = String::with_capacity(64);
    for byte in digest {
        use std::fmt::Write as _;
        write!(&mut encoded, "{byte:02x}").expect("writing to String cannot fail");
    }
    encoded
}

fn valid_task_lock_name(name: &str) -> bool {
    if name
        .strip_prefix(".g-")
        .and_then(|name| name.strip_suffix(".sock"))
        .is_some_and(|digest| {
            digest.len() == 20 && digest.bytes().all(|byte| byte.is_ascii_hexdigit())
        })
    {
        return true;
    }
    let digest = name
        .strip_prefix(".task-")
        .and_then(|name| name.strip_suffix(".lock"));
    digest.is_some_and(|digest| {
        digest.len() == 64 && digest.bytes().all(|byte| byte.is_ascii_hexdigit())
    })
}

fn remove_stale_guardian_or_report_busy(path: &Path) -> Result<(), SparkError> {
    let result = UnixStream::connect(path);
    match result {
        Ok(_) => return Err(SparkError::Busy),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(error)
            if matches!(
                error.kind(),
                std::io::ErrorKind::ConnectionRefused | std::io::ErrorKind::ConnectionReset
            ) => {}
        Err(_) => return Err(SparkError::UnsafeWorkspace),
    }
    let metadata = fs::symlink_metadata(path).map_err(|_| SparkError::UnsafeWorkspace)?;
    if !metadata.file_type().is_socket() || metadata.uid() != unsafe { libc::geteuid() } {
        return Err(SparkError::UnsafeWorkspace);
    }
    fs::remove_file(path).map_err(|_| SparkError::UnsafeWorkspace)
}

fn sweep_stale_runs(
    temp_root: &Path,
    process_live_runs: &HashSet<PathBuf>,
) -> Result<(), SparkError> {
    let entries = fs::read_dir(temp_root).map_err(|_| SparkError::UnsafeWorkspace)?;
    let mut run_count = 0_usize;
    let mut trash = Vec::new();
    for entry in entries {
        let entry = entry.map_err(|_| SparkError::UnsafeWorkspace)?;
        let name = entry.file_name();
        let name = name.to_str().ok_or(SparkError::UnsafeWorkspace)?;
        if name == ".sweep.lock" || valid_task_lock_name(name) {
            continue;
        }
        if name.starts_with(".trash-") {
            trash.push(entry.path());
            continue;
        }
        if process_live_runs.contains(&entry.path()) {
            continue;
        }
        run_count += 1;
        if run_count > 64 {
            return Err(SparkError::UnsafeWorkspace);
        }
        if !name.starts_with("spark-") {
            return Err(SparkError::UnsafeWorkspace);
        }
        let metadata =
            fs::symlink_metadata(entry.path()).map_err(|_| SparkError::UnsafeWorkspace)?;
        if !metadata.file_type().is_dir() || metadata.uid() != unsafe { libc::geteuid() } {
            return Err(SparkError::UnsafeWorkspace);
        }
        let owner_path = entry.path().join("owner.lock");
        let owner = match OpenOptions::new()
            .read(true)
            .write(true)
            .custom_flags(libc::O_CLOEXEC | libc::O_NOFOLLOW)
            .open(&owner_path)
        {
            Ok(owner) => Some(owner),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => None,
            Err(_) => return Err(SparkError::UnsafeWorkspace),
        };
        if let Some(owner) = owner {
            match owner.try_lock_exclusive() {
                Ok(()) => {}
                Err(error) if error.kind() == fs2::lock_contended_error().kind() => continue,
                Err(_) => return Err(SparkError::UnsafeWorkspace),
            }
        }
        let guardian_path = entry.path().join("guardian.sock");
        match UnixStream::connect(&guardian_path) {
            Ok(_) => continue,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error)
                if matches!(
                    error.kind(),
                    std::io::ErrorKind::ConnectionRefused | std::io::ErrorKind::ConnectionReset
                ) =>
            {
                let metadata = fs::symlink_metadata(&guardian_path)
                    .map_err(|_| SparkError::UnsafeWorkspace)?;
                if !metadata.file_type().is_socket() || metadata.uid() != unsafe { libc::geteuid() }
                {
                    return Err(SparkError::UnsafeWorkspace);
                }
            }
            Err(_) => return Err(SparkError::UnsafeWorkspace),
        }
        let quarantine = temp_root.join(format!(".trash-{}", uuid::Uuid::new_v4()));
        fs::rename(entry.path(), &quarantine).map_err(|_| SparkError::UnsafeWorkspace)?;
        trash.push(quarantine);
    }
    for path in trash {
        remove_private_tree_best_effort(&path);
    }
    Ok(())
}

fn remove_private_tree_best_effort(path: &Path) {
    make_private_tree_writable_best_effort(path);
    let _ = fs::remove_dir_all(path);
}

fn make_private_tree_writable_best_effort(path: &Path) {
    let mut directories = vec![path.to_path_buf()];
    while let Some(directory) = directories.pop() {
        let Ok(metadata) = fs::symlink_metadata(&directory) else {
            continue;
        };
        if !metadata.file_type().is_dir() || metadata.uid() != unsafe { libc::geteuid() } {
            continue;
        }
        let _ = fs::set_permissions(&directory, fs::Permissions::from_mode(0o700));
        if let Ok(entries) = fs::read_dir(&directory) {
            for entry in entries.flatten() {
                if fs::symlink_metadata(entry.path())
                    .is_ok_and(|metadata| metadata.file_type().is_dir())
                {
                    directories.push(entry.path());
                }
            }
        }
    }
}

impl Drop for SparkRunFiles {
    fn drop(&mut self) {
        let mut process_state = match spark_process_state().lock() {
            Ok(state) => state,
            Err(poisoned) => poisoned.into_inner(),
        };
        let registry_run = self.root.as_ref().map(|root| root.path().to_path_buf());
        let sweep_lock = self.root.as_ref().and_then(|root| {
            let parent = root.path().parent()?;
            let file = open_private_file(&parent.join(".sweep.lock")).ok()?;
            file.lock_exclusive().ok()?;
            Some(ExplicitFileLock::from_locked(file))
        });
        let _ = fs::set_permissions(&self.workdir, fs::Permissions::from_mode(0o700));
        truncate_sensitive_file_handle(&mut self.auth_file);
        truncate_sensitive_file_handle(&mut self.output_file);
        if let Some(root) = self.root.take() {
            make_private_tree_writable_best_effort(root.path());
            let _ = root.close();
        }
        remove_owned_socket_best_effort(&self.task_guardian);
        if let Some(registry_run) = registry_run {
            process_state.live_runs.remove(&registry_run);
        }
        process_state.live_tasks.remove(&self.registry_task);
        drop(sweep_lock);
    }
}

fn remove_owned_socket_best_effort(path: &Path) {
    if fs::symlink_metadata(path).is_ok_and(|metadata| {
        metadata.file_type().is_socket() && metadata.uid() == unsafe { libc::geteuid() }
    }) {
        let _ = fs::remove_file(path);
    }
}

fn truncate_sensitive_file_handle(file: &mut File) {
    let _ = file.set_len(0);
}

fn output_schema() -> String {
    serde_json::json!({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "schema": {"type": "integer", "const": 1},
            "facts": {"type": "array", "maxItems": 32, "items": {"type": "string"}},
            "pending": {"type": "array", "maxItems": 32, "items": {"type": "string"}},
            "decisions": {"type": "array", "maxItems": 32, "items": {"type": "string"}},
            "spoken_text": {"type": "string"},
            "source_evidence": {
                "type": "array",
                "maxItems": 32,
                "items": {
                    "type": "object",
                    "additionalProperties": false,
                    "properties": {
                        "completion_id": {"type": "string"},
                        "exact_quote": {"type": "string"}
                    },
                    "required": ["completion_id", "exact_quote"]
                }
            },
            "covers_new_completions": {
                "type": "array",
                "maxItems": 32,
                "items": {"type": "string"}
            }
        },
        "required": [
            "schema", "facts", "pending", "decisions", "spoken_text", "source_evidence",
            "covers_new_completions"
        ]
    })
    .to_string()
}

fn write_new_file(path: &Path, bytes: &[u8], mode: u32) -> Result<File, SparkError> {
    let mut file = OpenOptions::new()
        .read(true)
        .write(true)
        .create_new(true)
        .mode(mode)
        .custom_flags(libc::O_CLOEXEC | libc::O_NOFOLLOW)
        .open(path)
        .map_err(|_| SparkError::UnsafeWorkspace)?;
    file.write_all(bytes)
        .and_then(|_| file.sync_all())
        .map_err(|_| SparkError::UnsafeWorkspace)?;
    Ok(file)
}

fn open_auth(path: &Path) -> Result<File, SparkError> {
    if !path.is_absolute() {
        return Err(SparkError::Authentication);
    }
    let parent = path.parent().ok_or(SparkError::Authentication)?;
    open_owned_directory_chain(parent, false).map_err(|_| SparkError::Authentication)?;
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_CLOEXEC | libc::O_NOFOLLOW)
        .open(path)
        .map_err(|_| SparkError::Authentication)?;
    let metadata = file.metadata().map_err(|_| SparkError::Authentication)?;
    if !metadata.file_type().is_file()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.mode() & 0o077 != 0
        || metadata.len() == 0
        || metadata.len() > MAX_AUTH_BYTES
    {
        return Err(SparkError::Authentication);
    }
    Ok(file)
}

struct AuthSnapshot {
    bytes: Zeroizing<Vec<u8>>,
    identity: FileIdentity,
    digest: [u8; 32],
}

impl AuthSnapshot {
    fn load(path: &Path) -> Result<Self, SparkError> {
        let mut file = open_auth(path)?;
        let identity = FileIdentity::from_file(&file).map_err(|_| SparkError::Authentication)?;
        let bytes = read_auth_bytes(&mut file)?;
        let digest = Sha256::digest(bytes.as_slice()).into();
        Ok(Self {
            bytes,
            identity,
            digest,
        })
    }

    fn verify_source(&self, path: &Path) -> Result<(), SparkError> {
        let mut file = open_auth(path)?;
        let identity = FileIdentity::from_file(&file).map_err(|_| SparkError::Authentication)?;
        if identity.device != self.identity.device || identity.inode != self.identity.inode {
            return Err(SparkError::Authentication);
        }
        let bytes = read_auth_bytes(&mut file)?;
        let digest: [u8; 32] = Sha256::digest(bytes.as_slice()).into();
        if digest == self.digest {
            Ok(())
        } else {
            Err(SparkError::Authentication)
        }
    }
}

fn read_auth_bytes(file: &mut File) -> Result<Zeroizing<Vec<u8>>, SparkError> {
    let size = file
        .metadata()
        .map_err(|_| SparkError::Authentication)?
        .len() as usize;
    let mut bytes = Zeroizing::new(Vec::with_capacity(size));
    file.read_to_end(&mut bytes)
        .map_err(|_| SparkError::Authentication)?;
    let first = bytes.iter().find(|byte| !byte.is_ascii_whitespace());
    let last = bytes.iter().rfind(|byte| !byte.is_ascii_whitespace());
    if bytes.is_empty()
        || bytes.len() > MAX_AUTH_BYTES as usize
        || first != Some(&b'{')
        || last != Some(&b'}')
    {
        return Err(SparkError::Authentication);
    }
    let mut deserializer = serde_json::Deserializer::from_slice(&bytes);
    serde::de::IgnoredAny::deserialize(&mut deserializer)
        .and_then(|_| deserializer.end())
        .map_err(|_| SparkError::Authentication)?;
    Ok(bytes)
}

#[derive(Clone, Copy)]
struct FileIdentity {
    device: u64,
    inode: u64,
}

impl FileIdentity {
    fn from_file(file: &File) -> std::io::Result<Self> {
        let metadata = file.metadata()?;
        Ok(Self {
            device: metadata.dev(),
            inode: metadata.ino(),
        })
    }

    fn matches(&self, metadata: &fs::Metadata) -> bool {
        metadata.dev() == self.device && metadata.ino() == self.inode
    }
}

struct OutputFileMonitor {
    path: PathBuf,
    identity: FileIdentity,
}

impl OutputFileMonitor {
    fn new(path: &Path, file: &File) -> Result<Self, SparkError> {
        let identity = FileIdentity::from_file(file).map_err(|_| SparkError::InvalidOutput)?;
        Ok(Self {
            path: path.to_path_buf(),
            identity,
        })
    }

    fn check(&self) -> Result<(), SparkError> {
        let file = open_output_for_read(&self.path)?;
        let metadata = file.metadata().map_err(|_| SparkError::InvalidOutput)?;
        if !self.identity.matches(&metadata) {
            return Err(SparkError::InvalidOutput);
        }
        if metadata.len() > MAX_SUMMARY_DOCUMENT_BYTES as u64 {
            return Err(SparkError::OutputTooLarge);
        }
        Ok(())
    }
}

struct AuthCopyMonitor {
    path: PathBuf,
    identity: FileIdentity,
}

impl AuthCopyMonitor {
    fn new(path: &Path, file: &File) -> Result<Self, SparkError> {
        let identity = FileIdentity::from_file(file).map_err(|_| SparkError::Authentication)?;
        Ok(Self {
            path: path.to_path_buf(),
            identity,
        })
    }

    fn check(&self) -> Result<(), SparkError> {
        let file = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_CLOEXEC | libc::O_NOFOLLOW)
            .open(&self.path)
            .map_err(|_| SparkError::Authentication)?;
        let metadata = file.metadata().map_err(|_| SparkError::Authentication)?;
        if !metadata.file_type().is_file()
            || metadata.uid() != unsafe { libc::geteuid() }
            || metadata.mode() & 0o777 != 0o400
            || !self.identity.matches(&metadata)
            || metadata.len() == 0
            || metadata.len() > MAX_AUTH_BYTES
        {
            return Err(SparkError::Authentication);
        }
        Ok(())
    }
}

struct SparkWorkspaceMonitor {
    root: PathBuf,
    root_identity: FileIdentity,
    auth_identity: Option<FileIdentity>,
    sandbox_marker_identity: FileIdentity,
    skills_identity: FileIdentity,
    tmp_identity: FileIdentity,
    arg0_identity: FileIdentity,
    process_tmp: PathBuf,
    process_tmp_identity: FileIdentity,
    alias_target: PathBuf,
    max_nodes: AtomicUsize,
    max_bytes: AtomicU64,
}

impl SparkWorkspaceMonitor {
    #[allow(clippy::too_many_arguments)]
    fn new(
        root: &Path,
        auth: &File,
        sandbox_marker: &File,
        skills: &Path,
        tmp: &Path,
        arg0: &Path,
        process_tmp: &Path,
        executable: &Path,
    ) -> Result<Self, SparkError> {
        let root_identity = private_directory_identity(root, 0o700)?;
        let skills_identity = private_directory_identity(skills, 0o500)?;
        let tmp_identity = private_directory_identity(tmp, 0o500)?;
        let arg0_identity = private_directory_identity(arg0, 0o700)?;
        let process_tmp_identity = private_directory_identity(process_tmp, 0o700)?;
        let auth_identity =
            Some(FileIdentity::from_file(auth).map_err(|_| SparkError::UnsafeWorkspace)?);
        let sandbox_marker_identity =
            FileIdentity::from_file(sandbox_marker).map_err(|_| SparkError::UnsafeWorkspace)?;
        let alias_target = trusted_executable_path(executable)?;
        let monitor = Self {
            root: root.to_path_buf(),
            root_identity,
            auth_identity,
            sandbox_marker_identity,
            skills_identity,
            tmp_identity,
            arg0_identity,
            process_tmp: process_tmp.to_path_buf(),
            process_tmp_identity,
            alias_target,
            max_nodes: AtomicUsize::new(0),
            max_bytes: AtomicU64::new(0),
        };
        monitor.check()?;
        Ok(monitor)
    }

    fn new_without_auth(
        root: &Path,
        sandbox_marker: &File,
        skills: &Path,
        tmp: &Path,
        arg0: &Path,
        process_tmp: &Path,
        executable: &Path,
    ) -> Result<Self, SparkError> {
        let root_identity = private_directory_identity(root, 0o700)?;
        let skills_identity = private_directory_identity(skills, 0o500)?;
        let tmp_identity = private_directory_identity(tmp, 0o500)?;
        let arg0_identity = private_directory_identity(arg0, 0o700)?;
        let process_tmp_identity = private_directory_identity(process_tmp, 0o700)?;
        let sandbox_marker_identity =
            FileIdentity::from_file(sandbox_marker).map_err(|_| SparkError::UnsafeWorkspace)?;
        let alias_target = trusted_executable_path(executable)?;
        let monitor = Self {
            root: root.to_path_buf(),
            root_identity,
            auth_identity: None,
            sandbox_marker_identity,
            skills_identity,
            tmp_identity,
            arg0_identity,
            process_tmp: process_tmp.to_path_buf(),
            process_tmp_identity,
            alias_target,
            max_nodes: AtomicUsize::new(0),
            max_bytes: AtomicU64::new(0),
        };
        monitor.check()?;
        Ok(monitor)
    }

    fn check(&self) -> Result<(), SparkError> {
        for attempt in 0..3 {
            match self.audit() {
                Ok((nodes, bytes)) => {
                    self.max_nodes.fetch_max(nodes, Ordering::Relaxed);
                    self.max_bytes.fetch_max(bytes, Ordering::Relaxed);
                    return Ok(());
                }
                Err(error) if attempt == 2 => return Err(error),
                Err(_) => thread::sleep(Duration::from_millis(1)),
            }
        }
        unreachable!()
    }

    fn audit(&self) -> Result<(usize, u64), SparkError> {
        let metadata = fs::symlink_metadata(&self.root).map_err(|_| SparkError::UnsafeWorkspace)?;
        if !private_directory_matches(&metadata, 0o700, self.root_identity) {
            return Err(SparkError::UnsafeWorkspace);
        }

        let mut nodes = 1_usize;
        let mut bytes = 0_u64;
        for entry in fs::read_dir(&self.root).map_err(|_| SparkError::UnsafeWorkspace)? {
            let entry = entry.map_err(|_| SparkError::UnsafeWorkspace)?;
            let name = entry
                .file_name()
                .into_string()
                .map_err(|_| SparkError::UnsafeWorkspace)?;
            let metadata =
                fs::symlink_metadata(entry.path()).map_err(|_| SparkError::UnsafeWorkspace)?;
            let is_auth = name == "auth.json" && self.auth_identity.is_some();
            nodes = nodes.checked_add(1).ok_or(SparkError::UnsafeWorkspace)?;
            if nodes > SPARK_MAX_WORKSPACE_NODES {
                return Err(SparkError::UnsafeWorkspace);
            }
            if metadata.uid() != unsafe { libc::geteuid() } {
                return Err(if is_auth {
                    SparkError::Authentication
                } else {
                    SparkError::UnsafeWorkspace
                });
            }
            if is_auth && !metadata.file_type().is_file() {
                return Err(SparkError::Authentication);
            }

            if metadata.file_type().is_dir() {
                if name == "skills" {
                    if !private_directory_matches(&metadata, 0o500, self.skills_identity)
                        || fs::read_dir(entry.path())
                            .map_err(|_| SparkError::UnsafeWorkspace)?
                            .next()
                            .is_some()
                    {
                        return Err(SparkError::UnsafeWorkspace);
                    }
                } else if name == "tmp" {
                    if !private_directory_matches(&metadata, 0o500, self.tmp_identity) {
                        return Err(SparkError::UnsafeWorkspace);
                    }
                    let mut children =
                        fs::read_dir(entry.path()).map_err(|_| SparkError::UnsafeWorkspace)?;
                    let child = children
                        .next()
                        .ok_or(SparkError::UnsafeWorkspace)?
                        .map_err(|_| SparkError::UnsafeWorkspace)?;
                    if children.next().is_some()
                        || child.file_name() != "arg0"
                        || !fs::symlink_metadata(child.path()).is_ok_and(|metadata| {
                            private_directory_matches(&metadata, 0o700, self.arg0_identity)
                        })
                    {
                        return Err(SparkError::UnsafeWorkspace);
                    }
                    let (arg0_nodes, arg0_bytes) =
                        audit_arg0_scratch(&child.path(), &self.alias_target)?;
                    nodes = nodes
                        .checked_add(1 + arg0_nodes)
                        .ok_or(SparkError::UnsafeWorkspace)?;
                    bytes = bytes
                        .checked_add(arg0_bytes)
                        .filter(|bytes| *bytes <= SPARK_MAX_WORKSPACE_BYTES)
                        .ok_or(SparkError::UnsafeWorkspace)?;
                    if nodes > SPARK_MAX_WORKSPACE_NODES {
                        return Err(SparkError::UnsafeWorkspace);
                    }
                } else {
                    return Err(SparkError::UnsafeWorkspace);
                }
                continue;
            }

            if !metadata.file_type().is_file() || metadata.nlink() != 1 {
                return Err(if is_auth {
                    SparkError::Authentication
                } else {
                    SparkError::UnsafeWorkspace
                });
            }
            let mode = metadata.mode() & 0o777;
            let limit = if name == "auth.json" {
                if !self
                    .auth_identity
                    .is_some_and(|identity| identity.matches(&metadata))
                    || mode != 0o400
                {
                    return Err(if is_auth {
                        SparkError::Authentication
                    } else {
                        SparkError::UnsafeWorkspace
                    });
                }
                if metadata.len() == 0 || metadata.len() > MAX_AUTH_BYTES {
                    return Err(if is_auth {
                        SparkError::Authentication
                    } else {
                        SparkError::UnsafeWorkspace
                    });
                }
                MAX_AUTH_BYTES
            } else if name == ".sandbox_migration" {
                if !self.sandbox_marker_identity.matches(&metadata)
                    || mode != 0o400
                    || metadata.len() != 3
                {
                    return Err(SparkError::UnsafeWorkspace);
                }
                3
            } else {
                if mode & 0o133 != 0 {
                    return Err(SparkError::UnsafeWorkspace);
                }
                runtime_file_limit(&name).ok_or(SparkError::UnsafeWorkspace)?
            };
            if metadata.len() > limit {
                return Err(SparkError::UnsafeWorkspace);
            }
            bytes = bytes
                .checked_add(metadata.len())
                .filter(|bytes| *bytes <= SPARK_MAX_WORKSPACE_BYTES)
                .ok_or(SparkError::UnsafeWorkspace)?;
        }
        let metadata =
            fs::symlink_metadata(&self.process_tmp).map_err(|_| SparkError::UnsafeWorkspace)?;
        if !private_directory_matches(&metadata, 0o700, self.process_tmp_identity)
            || fs::read_dir(&self.process_tmp)
                .map_err(|_| SparkError::UnsafeWorkspace)?
                .next()
                .is_some()
        {
            return Err(SparkError::UnsafeWorkspace);
        }
        nodes = nodes.checked_add(1).ok_or(SparkError::UnsafeWorkspace)?;
        if nodes > SPARK_MAX_WORKSPACE_NODES {
            return Err(SparkError::UnsafeWorkspace);
        }
        Ok((nodes, bytes))
    }

    fn report(&self) -> SparkIsolationReport {
        SparkIsolationReport {
            workspace_max_nodes: self.max_nodes.load(Ordering::Relaxed),
            workspace_max_bytes: self.max_bytes.load(Ordering::Relaxed),
            system_skill_files: 0,
            plugin_files: 0,
            persistent_runtime_rows: 0,
        }
    }
}

fn audit_arg0_scratch(arg0: &Path, alias_target: &Path) -> Result<(usize, u64), SparkError> {
    let mut entries = fs::read_dir(arg0).map_err(|_| SparkError::UnsafeWorkspace)?;
    let Some(entry) = entries.next() else {
        return Ok((0, 0));
    };
    let entry = entry.map_err(|_| SparkError::UnsafeWorkspace)?;
    if entries.next().is_some() {
        return Err(SparkError::UnsafeWorkspace);
    }
    let name = entry
        .file_name()
        .into_string()
        .map_err(|_| SparkError::UnsafeWorkspace)?;
    let suffix = name
        .strip_prefix("codex-arg0")
        .ok_or(SparkError::UnsafeWorkspace)?;
    if suffix.len() != 6 || !suffix.bytes().all(|byte| byte.is_ascii_alphanumeric()) {
        return Err(SparkError::UnsafeWorkspace);
    }
    let metadata = fs::symlink_metadata(entry.path()).map_err(|_| SparkError::UnsafeWorkspace)?;
    if !metadata.file_type().is_dir()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.mode() & 0o777 != 0o755
    {
        return Err(SparkError::UnsafeWorkspace);
    }

    let mut nodes = 1_usize;
    let mut bytes = 0_u64;
    let mut seen = HashSet::new();
    for child in fs::read_dir(entry.path()).map_err(|_| SparkError::UnsafeWorkspace)? {
        let child = child.map_err(|_| SparkError::UnsafeWorkspace)?;
        let child_name = child
            .file_name()
            .into_string()
            .map_err(|_| SparkError::UnsafeWorkspace)?;
        if !seen.insert(child_name.clone()) {
            return Err(SparkError::UnsafeWorkspace);
        }
        let metadata =
            fs::symlink_metadata(child.path()).map_err(|_| SparkError::UnsafeWorkspace)?;
        if metadata.uid() != unsafe { libc::geteuid() } || metadata.nlink() != 1 {
            return Err(SparkError::UnsafeWorkspace);
        }
        if child_name == ".lock" {
            if !metadata.file_type().is_file()
                || metadata.mode() & 0o777 != 0o644
                || metadata.len() != 0
            {
                return Err(SparkError::UnsafeWorkspace);
            }
        } else if matches!(
            child_name.as_str(),
            "apply_patch" | "applypatch" | "codex-execve-wrapper"
        ) {
            if !metadata.file_type().is_symlink() || metadata.len() > 4096 {
                return Err(SparkError::UnsafeWorkspace);
            }
            let target = fs::read_link(child.path()).map_err(|_| SparkError::UnsafeWorkspace)?;
            if target != alias_target {
                return Err(SparkError::UnsafeWorkspace);
            }
        } else {
            return Err(SparkError::UnsafeWorkspace);
        }
        nodes = nodes.checked_add(1).ok_or(SparkError::UnsafeWorkspace)?;
        bytes = bytes
            .checked_add(metadata.len())
            .ok_or(SparkError::UnsafeWorkspace)?;
    }
    Ok((nodes, bytes))
}

fn trusted_executable_path(executable: &Path) -> Result<PathBuf, SparkError> {
    let path = fs::canonicalize(executable).map_err(|_| SparkError::CliMissing)?;
    let metadata = fs::symlink_metadata(&path).map_err(|_| SparkError::CliMissing)?;
    let owner = metadata.uid();
    if !metadata.file_type().is_file()
        || (owner != unsafe { libc::geteuid() } && owner != 0)
        || metadata.mode() & 0o022 != 0
        || metadata.mode() & 0o111 == 0
    {
        return Err(SparkError::CliMissing);
    }
    Ok(path)
}

fn private_directory_identity(path: &Path, mode: u32) -> Result<FileIdentity, SparkError> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_DIRECTORY)
        .open(path)
        .map_err(|_| SparkError::UnsafeWorkspace)?;
    let metadata = file.metadata().map_err(|_| SparkError::UnsafeWorkspace)?;
    if !private_directory_matches(
        &metadata,
        mode,
        FileIdentity::from_file(&file).map_err(|_| SparkError::UnsafeWorkspace)?,
    ) {
        return Err(SparkError::UnsafeWorkspace);
    }
    FileIdentity::from_file(&file).map_err(|_| SparkError::UnsafeWorkspace)
}

fn private_directory_matches(metadata: &fs::Metadata, mode: u32, identity: FileIdentity) -> bool {
    metadata.file_type().is_dir()
        && metadata.uid() == unsafe { libc::geteuid() }
        && metadata.mode() & 0o777 == mode
        && identity.matches(metadata)
}

fn runtime_file_limit(name: &str) -> Option<u64> {
    match name {
        "installation_id" => Some(128),
        "models_cache.json" => Some(MAX_MODEL_CACHE_BYTES),
        "goals_1.sqlite" | "memories_1.sqlite" | "logs_2.sqlite" | "state_5.sqlite" => {
            Some(MAX_RUNTIME_DATABASE_BYTES)
        }
        "goals_1.sqlite-wal"
        | "memories_1.sqlite-wal"
        | "logs_2.sqlite-wal"
        | "state_5.sqlite-wal" => Some(MAX_RUNTIME_WAL_BYTES),
        "goals_1.sqlite-shm"
        | "memories_1.sqlite-shm"
        | "logs_2.sqlite-shm"
        | "state_5.sqlite-shm" => Some(MAX_RUNTIME_SHM_BYTES),
        _ => None,
    }
}

fn validate_runtime_databases(codex_home: &Path) -> Result<(), SparkError> {
    const DATABASE_TABLES: &[(&str, &[&str])] = &[
        (
            "goals_1.sqlite",
            &["thread_goals", "thread_goal_continuation_deferrals"],
        ),
        ("memories_1.sqlite", &["jobs", "stage1_outputs"]),
        ("logs_2.sqlite", &["logs"]),
        (
            "state_5.sqlite",
            &[
                "external_agent_config_imports",
                "remote_control_enrollments",
                "thread_dynamic_tools",
                "thread_spawn_edges",
                "threads",
            ],
        ),
    ];
    for (database, tables) in DATABASE_TABLES {
        let path = codex_home.join(database);
        if !path.exists() {
            continue;
        }
        let connection = rusqlite::Connection::open_with_flags(
            path,
            rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY | rusqlite::OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )
        .map_err(|_| SparkError::UnsafeWorkspace)?;
        for table in *tables {
            let query = format!("SELECT COUNT(*) FROM \"{table}\"");
            let rows: i64 = connection
                .query_row(&query, [], |row| row.get(0))
                .map_err(|_| SparkError::UnsafeWorkspace)?;
            if rows != 0 {
                return Err(SparkError::UnsafeWorkspace);
            }
        }
    }
    Ok(())
}

fn open_output_for_read(path: &Path) -> Result<File, SparkError> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_CLOEXEC | libc::O_NOFOLLOW)
        .open(path)
        .map_err(|_| SparkError::InvalidOutput)?;
    let metadata = file.metadata().map_err(|_| SparkError::InvalidOutput)?;
    if !metadata.file_type().is_file() || metadata.uid() != unsafe { libc::geteuid() } {
        return Err(SparkError::InvalidOutput);
    }
    Ok(file)
}

fn read_bounded_output(
    path: &Path,
    expected_identity: FileIdentity,
) -> Result<Zeroizing<Vec<u8>>, SparkError> {
    let mut file = open_output_for_read(path)?;
    let metadata = file.metadata().map_err(|_| SparkError::InvalidOutput)?;
    if !expected_identity.matches(&metadata) || metadata.len() == 0 {
        return Err(SparkError::InvalidOutput);
    }
    if metadata.len() > MAX_SUMMARY_DOCUMENT_BYTES as u64 {
        return Err(SparkError::OutputTooLarge);
    }
    let mut bytes = Zeroizing::new(Vec::with_capacity(metadata.len() as usize));
    file.read_to_end(&mut bytes)
        .map_err(|_| SparkError::InvalidOutput)?;
    if bytes.len() > MAX_SUMMARY_DOCUMENT_BYTES {
        Err(SparkError::OutputTooLarge)
    } else {
        Ok(bytes)
    }
}

fn spark_command(
    executable: &Path,
    codex_home: &Path,
    workdir: &Path,
    schema: &Path,
    output: &Path,
    tmpdir: &Path,
) -> Command {
    let mut command = Command::new(executable);
    command.args([
        "exec",
        "--model",
        SPARK_MODEL,
        "--sandbox",
        "read-only",
        "--ephemeral",
        "--ignore-user-config",
        "--ignore-rules",
        "--skip-git-repo-check",
        "--config",
        "approval_policy=\"never\"",
        "--output-schema",
    ]);
    command.arg(schema);
    for feature in DISABLED_SPARK_FEATURES {
        command.args(["--disable", feature]);
    }
    command.arg("--output-last-message");
    command.arg(output);
    command.args(["--color", "never", "--cd"]);
    command.arg(workdir);
    command.arg("-");
    command.env_clear();
    command.env("CODEX_HOME", codex_home);
    command.env("HOME", codex_home);
    command.env("TMPDIR", tmpdir);
    inherit_proxy_environment(&mut command);
    command
}

pub fn verify_spark_prompt_isolation(
    executable: &Path,
    temp_root: &Path,
) -> Result<SparkPromptIsolationReport, SparkError> {
    secure_directory(temp_root).map_err(|_| SparkError::UnsafeWorkspace)?;
    let root = PrivateTempDir::new(
        Builder::new()
            .prefix("prompt-gate-")
            .tempdir_in(temp_root)
            .map_err(|_| SparkError::UnsafeWorkspace)?,
    );
    fs::set_permissions(root.path(), fs::Permissions::from_mode(0o700))
        .map_err(|_| SparkError::UnsafeWorkspace)?;
    let home = root.path().join("home");
    let work = root.path().join("work");
    let process_tmp = root.path().join("process-tmp");
    for directory in [&home, &work, &process_tmp] {
        fs::create_dir(directory).map_err(|_| SparkError::UnsafeWorkspace)?;
        fs::set_permissions(directory, fs::Permissions::from_mode(0o700))
            .map_err(|_| SparkError::UnsafeWorkspace)?;
    }
    let skills = home.join("skills");
    let home_tmp = home.join("tmp");
    for directory in [&skills, &home_tmp] {
        fs::create_dir(directory).map_err(|_| SparkError::UnsafeWorkspace)?;
    }
    let home_arg0 = home_tmp.join("arg0");
    fs::create_dir(&home_arg0).map_err(|_| SparkError::UnsafeWorkspace)?;
    fs::set_permissions(&home_arg0, fs::Permissions::from_mode(0o700))
        .map_err(|_| SparkError::UnsafeWorkspace)?;
    for directory in [&skills, &home_tmp] {
        fs::set_permissions(directory, fs::Permissions::from_mode(0o500))
            .map_err(|_| SparkError::UnsafeWorkspace)?;
    }
    let sandbox_marker = write_new_file(&home.join(".sandbox_migration"), b"v1\n", 0o400)?;
    fs::set_permissions(&work, fs::Permissions::from_mode(0o500))
        .map_err(|_| SparkError::UnsafeWorkspace)?;
    let monitor = SparkWorkspaceMonitor::new_without_auth(
        &home,
        &sandbox_marker,
        &skills,
        &home_tmp,
        &home_arg0,
        &process_tmp,
        executable,
    )?;

    let mut command = Command::new(executable);
    command.args(["debug", "prompt-input"]);
    for feature in DISABLED_SPARK_FEATURES {
        command.args(["--disable", feature]);
    }
    command.arg("Public Spark isolation gate.");
    command
        .env_clear()
        .env("CODEX_HOME", &home)
        .env("HOME", &home)
        .env("TMPDIR", &process_tmp)
        .current_dir(&work)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    unsafe {
        command.pre_exec(|| {
            if libc::setpgid(0, 0) != 0 {
                return Err(std::io::Error::last_os_error());
            }
            let file_limit = libc::rlimit {
                rlim_cur: MAX_CHILD_FILE_BYTES,
                rlim_max: MAX_CHILD_FILE_BYTES,
            };
            if libc::setrlimit(libc::RLIMIT_FSIZE, &file_limit) != 0 {
                return Err(std::io::Error::last_os_error());
            }
            #[cfg(target_os = "linux")]
            {
                if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL) != 0 {
                    return Err(std::io::Error::last_os_error());
                }
                if libc::getppid() == 1 {
                    return Err(std::io::Error::from(std::io::ErrorKind::BrokenPipe));
                }
            }
            Ok(())
        });
    }
    let mut child = command.spawn().map_err(|error| {
        if error.kind() == std::io::ErrorKind::NotFound {
            SparkError::CliMissing
        } else {
            SparkError::ProcessIo
        }
    })?;
    let stdout = child.stdout.take().ok_or(SparkError::ProcessIo)?;
    let stderr = child.stderr.take().ok_or(SparkError::ProcessIo)?;
    let process_group = child.id() as i32;
    let mut guard = ChildGuard::new(child, process_group);
    let output_state = Arc::new(AtomicU8::new(OUTPUT_OK));
    let output_reader = {
        let state = Arc::clone(&output_state);
        thread::spawn(move || read_bounded_pipe(stdout, MAX_STDOUT_BYTES, state))
    };
    let stderr_reader = thread::spawn(move || read_stderr(stderr, MAX_STDERR_BYTES));
    let deadline = Instant::now() + Duration::from_secs(30);
    let status = loop {
        if let Err(error) = monitor.check() {
            guard.kill_and_wait();
            let _ = output_reader.join();
            let _ = stderr_reader.join();
            return Err(error);
        }
        match output_state.load(Ordering::Acquire) {
            OUTPUT_TOO_LARGE => {
                guard.kill_and_wait();
                let _ = output_reader.join();
                let _ = stderr_reader.join();
                return Err(SparkError::OutputTooLarge);
            }
            OUTPUT_IO => {
                guard.kill_and_wait();
                let _ = output_reader.join();
                let _ = stderr_reader.join();
                return Err(SparkError::ProcessIo);
            }
            _ => {}
        }
        match guard.child_mut().try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) => {}
            Err(_) => {
                guard.kill_and_wait();
                let _ = output_reader.join();
                let _ = stderr_reader.join();
                return Err(SparkError::ProcessIo);
            }
        }
        if Instant::now() >= deadline {
            guard.kill_and_wait();
            let _ = output_reader.join();
            let _ = stderr_reader.join();
            return Err(SparkError::Timeout);
        }
        thread::sleep(Duration::from_millis(10));
    };
    guard.disarm();
    let output = output_reader
        .join()
        .unwrap_or_else(|_| Zeroizing::new(Vec::new()));
    let stderr = stderr_reader
        .join()
        .unwrap_or_else(|_| Zeroizing::new(Vec::new()));
    if output_state.load(Ordering::Acquire) == OUTPUT_TOO_LARGE {
        return Err(SparkError::OutputTooLarge);
    }
    if !status.success() {
        if status.signal().is_some() {
            return Err(SparkError::ExitFailure);
        }
        return Err(classify_stderr(&stderr));
    }
    monitor.check()?;
    validate_runtime_databases(&home)?;
    let value: serde_json::Value =
        serde_json::from_slice(&output).map_err(|_| SparkError::InvalidOutput)?;
    let prompt_items = value.as_array().map_or(1, Vec::len);
    let skills_instruction_blocks = count_json_string_matches(&value, "<skills_instructions>");
    let skill_path_mentions = count_json_string_matches(&value, "SKILL.md");
    if skills_instruction_blocks != 0 || skill_path_mentions != 0 {
        return Err(SparkError::UnsafeWorkspace);
    }
    let report = monitor.report();
    Ok(SparkPromptIsolationReport {
        prompt_items,
        skills_instruction_blocks,
        skill_path_mentions,
        workspace_max_nodes: report.workspace_max_nodes,
        workspace_max_bytes: report.workspace_max_bytes,
    })
}

fn count_json_string_matches(value: &serde_json::Value, needle: &str) -> usize {
    match value {
        serde_json::Value::Array(values) => values
            .iter()
            .map(|value| count_json_string_matches(value, needle))
            .sum(),
        serde_json::Value::Object(values) => values
            .values()
            .map(|value| count_json_string_matches(value, needle))
            .sum(),
        serde_json::Value::String(value) => usize::from(value.contains(needle)),
        _ => 0,
    }
}

fn inherit_proxy_environment(command: &mut Command) {
    for name in [
        "HTTPS_PROXY",
        "HTTP_PROXY",
        "ALL_PROXY",
        "NO_PROXY",
        "https_proxy",
        "http_proxy",
        "all_proxy",
        "no_proxy",
    ] {
        if let Some(value) = std::env::var_os(name) {
            command.env(name, value);
        }
    }
}

fn drain_bounded(mut reader: impl Read, limit: usize, state: Arc<AtomicU8>) -> u8 {
    let mut total = 0_usize;
    let mut buffer = Zeroizing::new([0_u8; 8192]);
    loop {
        match reader.read(&mut buffer[..]) {
            Ok(0) => return OUTPUT_OK,
            Ok(read) => {
                total = match total.checked_add(read) {
                    Some(total) if total <= limit => total,
                    _ => {
                        state.store(OUTPUT_TOO_LARGE, Ordering::Release);
                        return OUTPUT_TOO_LARGE;
                    }
                };
            }
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
            Err(_) => {
                state.store(OUTPUT_IO, Ordering::Release);
                return OUTPUT_IO;
            }
        }
    }
}

fn read_bounded_pipe(
    mut reader: impl Read,
    limit: usize,
    state: Arc<AtomicU8>,
) -> Zeroizing<Vec<u8>> {
    let mut output = Zeroizing::new(Vec::with_capacity(limit.min(4096)));
    let mut buffer = Zeroizing::new([0_u8; 8192]);
    loop {
        match reader.read(&mut buffer[..]) {
            Ok(0) => return output,
            Ok(read) => {
                let remaining = limit.saturating_sub(output.len());
                output.extend_from_slice(&buffer[..read.min(remaining)]);
                if read > remaining {
                    state.store(OUTPUT_TOO_LARGE, Ordering::Release);
                    return output;
                }
            }
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
            Err(_) => {
                state.store(OUTPUT_IO, Ordering::Release);
                return output;
            }
        }
    }
}

fn read_stderr(mut reader: impl Read, limit: usize) -> Zeroizing<Vec<u8>> {
    let mut output = Zeroizing::new(Vec::with_capacity(limit.min(4096)));
    let mut buffer = Zeroizing::new([0_u8; 4096]);
    loop {
        match reader.read(&mut buffer[..]) {
            Ok(0) => break,
            Ok(read) => {
                let remaining = limit.saturating_sub(output.len());
                output.extend_from_slice(&buffer[..read.min(remaining)]);
            }
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
            Err(_) => break,
        }
    }
    output
}

fn classify_stderr(stderr: &[u8]) -> SparkError {
    if contains_ascii_case_insensitive(stderr, b"not logged in")
        || contains_ascii_case_insensitive(stderr, b"authentication")
        || contains_ascii_case_insensitive(stderr, b"unauthorized")
        || stderr.windows(3).any(|window| window == b"401")
    {
        SparkError::Authentication
    } else if stderr.windows(3).any(|window| window == b"429")
        || contains_ascii_case_insensitive(stderr, b"rate limit")
        || contains_ascii_case_insensitive(stderr, b"usage limit")
    {
        SparkError::RateLimited
    } else if contains_ascii_case_insensitive(stderr, b"file size limit")
        || contains_ascii_case_insensitive(stderr, b"sigxfsz")
        || contains_ascii_case_insensitive(stderr, b"signal: 25")
    {
        SparkError::ResourceLimit
    } else if contains_ascii_case_insensitive(stderr, b"model")
        && (contains_ascii_case_insensitive(stderr, b"not found")
            || contains_ascii_case_insensitive(stderr, b"not supported")
            || contains_ascii_case_insensitive(stderr, b"unavailable")
            || contains_ascii_case_insensitive(stderr, b"does not exist")
            || contains_ascii_case_insensitive(stderr, b"no access"))
    {
        SparkError::ModelUnavailable
    } else if contains_ascii_case_insensitive(stderr, b"permission denied")
        || contains_ascii_case_insensitive(stderr, b"failed to initialize")
        || contains_ascii_case_insensitive(stderr, b"no space left")
    {
        SparkError::LocalRuntime
    } else if [b"500".as_slice(), b"502", b"503", b"504"]
        .iter()
        .any(|code| stderr.windows(code.len()).any(|window| window == *code))
        || contains_ascii_case_insensitive(stderr, b"service unavailable")
        || contains_ascii_case_insensitive(stderr, b"server error")
        || contains_ascii_case_insensitive(stderr, b"stream disconnected")
        || contains_ascii_case_insensitive(stderr, b"error sending request")
        || contains_ascii_case_insensitive(stderr, b"connection reset")
    {
        SparkError::ServiceUnavailable
    } else if stderr.is_empty() {
        SparkError::SilentExitFailure
    } else {
        SparkError::ExitFailure
    }
}

fn contains_ascii_case_insensitive(haystack: &[u8], needle: &[u8]) -> bool {
    haystack.windows(needle.len()).any(|window| {
        window
            .iter()
            .zip(needle)
            .all(|(left, right)| left.eq_ignore_ascii_case(right))
    })
}

fn join_io_threads(
    stdout: thread::JoinHandle<u8>,
    stderr: thread::JoinHandle<Zeroizing<Vec<u8>>>,
    stdin: thread::JoinHandle<bool>,
) {
    let _ = stdout.join();
    let _ = stderr.join();
    let _ = stdin.join();
}

struct ChildGuard {
    child: Option<Child>,
    process_group: i32,
}

impl ChildGuard {
    fn new(child: Child, process_group: i32) -> Self {
        Self {
            child: Some(child),
            process_group,
        }
    }

    fn child_mut(&mut self) -> &mut Child {
        self.child.as_mut().expect("child is armed")
    }

    fn kill_and_wait(&mut self) {
        if let Some(mut child) = self.child.take() {
            unsafe {
                libc::kill(-self.process_group, libc::SIGKILL);
            }
            let _ = child.kill();
            let _ = child.wait();
        }
    }

    fn disarm(&mut self) {
        unsafe {
            libc::kill(-self.process_group, libc::SIGKILL);
        }
        self.child.take();
    }

    fn into_child(mut self) -> Child {
        self.child.take().expect("child is armed")
    }
}

impl Drop for ChildGuard {
    fn drop(&mut self) {
        self.kill_and_wait();
    }
}

#[cfg(target_os = "macos")]
#[allow(clippy::too_many_arguments)]
pub fn run_spark_supervisor(
    parent: i32,
    executable: &Path,
    control_fd: i32,
    owner_lock_fd: i32,
    task_lock_fd: i32,
    codex_home: &Path,
    workdir: &Path,
    schema: &Path,
    output: &Path,
    tmpdir: &Path,
    owner_guardian: &Path,
    task_guardian: &Path,
) -> Result<i32, std::io::Error> {
    if parent <= 1
        || control_fd < 0
        || owner_lock_fd < 0
        || task_lock_fd < 0
        || !executable.is_absolute()
        || [
            codex_home,
            workdir,
            schema,
            output,
            tmpdir,
            owner_guardian,
            task_guardian,
        ]
        .iter()
        .any(|path| !path.is_absolute())
    {
        return Err(std::io::Error::from(std::io::ErrorKind::InvalidInput));
    }
    let queue = unsafe { libc::kqueue() };
    if queue < 0 {
        return Err(std::io::Error::last_os_error());
    }
    let _queue = unsafe { File::from_raw_fd(queue) };
    let mut control = unsafe { UnixStream::from_raw_fd(control_fd) };
    let _owner_lock = unsafe { File::from_raw_fd(owner_lock_fd) };
    let _task_lock = unsafe { File::from_raw_fd(task_lock_fd) };
    for fd in [control_fd, owner_lock_fd, task_lock_fd, queue] {
        if unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC) } != 0 {
            return Err(std::io::Error::last_os_error());
        }
    }
    let owner_guardian = UnixListener::bind(owner_guardian)?;
    owner_guardian.set_nonblocking(true)?;
    let task_guardian = UnixListener::bind(task_guardian)?;
    task_guardian.set_nonblocking(true)?;
    let change = libc::kevent {
        ident: parent as usize,
        filter: libc::EVFILT_PROC,
        flags: libc::EV_ADD | libc::EV_ENABLE | libc::EV_CLEAR,
        fflags: libc::NOTE_EXIT,
        data: 0,
        udata: std::ptr::null_mut(),
    };
    let registered =
        unsafe { libc::kevent(queue, &change, 1, std::ptr::null_mut(), 0, std::ptr::null()) };
    if registered != 0 {
        return Err(std::io::Error::last_os_error());
    }
    control.write_all(&[SPARK_SUPERVISOR_READY])?;
    let mut child = match spark_command(executable, codex_home, workdir, schema, output, tmpdir)
        .stdin(Stdio::inherit())
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .spawn()
    {
        Ok(child) => child,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return spark_supervisor_hold(
                queue,
                &mut control,
                SUPERVISOR_CLI_MISSING_EXIT as u8,
                [&owner_guardian, &task_guardian],
            );
        }
        Err(_) => {
            return spark_supervisor_hold(
                queue,
                &mut control,
                SUPERVISOR_IO_EXIT as u8,
                [&owner_guardian, &task_guardian],
            );
        }
    };
    loop {
        if let Err(error) = drain_guardian_listener(&owner_guardian) {
            return terminate_supervisor_group(error);
        }
        if let Err(error) = drain_guardian_listener(&task_guardian) {
            return terminate_supervisor_group(error);
        }
        let mut event = unsafe { std::mem::zeroed::<libc::kevent>() };
        let timeout = libc::timespec {
            tv_sec: 0,
            tv_nsec: 10_000_000,
        };
        let observed = unsafe { libc::kevent(queue, std::ptr::null(), 0, &mut event, 1, &timeout) };
        if observed < 0 {
            return terminate_supervisor_group(std::io::Error::last_os_error());
        }
        if observed > 0 {
            wait_for_test_parent_death_gate();
            unsafe {
                libc::kill(-libc::getpgrp(), libc::SIGKILL);
            }
            return Ok(1);
        }
        let status = match child.try_wait() {
            Ok(status) => status,
            Err(error) => return terminate_supervisor_group(error),
        };
        if let Some(status) = status {
            return spark_supervisor_hold(
                queue,
                &mut control,
                u8::from(!status.success()),
                [&owner_guardian, &task_guardian],
            );
        }
    }
}

#[cfg(target_os = "macos")]
fn spark_supervisor_hold(
    queue: i32,
    control: &mut UnixStream,
    code: u8,
    guardians: [&UnixListener; 2],
) -> Result<i32, std::io::Error> {
    if control.write_all(&[code]).is_err() {
        unsafe {
            libc::kill(-libc::getpgrp(), libc::SIGKILL);
        }
        return Ok(1);
    }
    loop {
        for guardian in guardians {
            if let Err(error) = drain_guardian_listener(guardian) {
                return terminate_supervisor_group(error);
            }
        }
        let mut event = unsafe { std::mem::zeroed::<libc::kevent>() };
        let observed =
            unsafe { libc::kevent(queue, std::ptr::null(), 0, &mut event, 1, std::ptr::null()) };
        if observed > 0 {
            wait_for_test_parent_death_gate();
            unsafe {
                libc::kill(-libc::getpgrp(), libc::SIGKILL);
            }
            return Ok(1);
        }
        if observed < 0 {
            return terminate_supervisor_group(std::io::Error::last_os_error());
        }
    }
}

#[cfg(target_os = "macos")]
fn terminate_supervisor_group(error: std::io::Error) -> Result<i32, std::io::Error> {
    unsafe {
        libc::kill(-libc::getpgrp(), libc::SIGKILL);
    }
    Err(error)
}

#[cfg(target_os = "macos")]
fn drain_guardian_listener(listener: &UnixListener) -> Result<(), std::io::Error> {
    loop {
        match listener.accept() {
            Ok((_stream, _address)) => {}
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => return Ok(()),
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error),
        }
    }
}

#[cfg(target_os = "macos")]
fn wait_for_test_parent_death_gate() {
    let Some(path) = std::env::var_os("ECI_SPARK_TEST_PARENT_DEATH_GATE").map(PathBuf::from) else {
        return;
    };
    if !path.is_absolute() {
        return;
    }
    while !path.exists() {
        thread::sleep(Duration::from_millis(10));
    }
}

#[cfg(not(target_os = "macos"))]
#[allow(clippy::too_many_arguments)]
pub fn run_spark_supervisor(
    _parent: i32,
    _executable: &Path,
    _control_fd: i32,
    _owner_lock_fd: i32,
    _task_lock_fd: i32,
    _codex_home: &Path,
    _workdir: &Path,
    _schema: &Path,
    _output: &Path,
    _tmpdir: &Path,
    _owner_guardian: &Path,
    _task_guardian: &Path,
) -> Result<i32, std::io::Error> {
    Err(std::io::Error::from(std::io::ErrorKind::Unsupported))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::ffi::OsStrExt;
    use std::os::unix::fs::PermissionsExt;
    use tempfile::tempdir;

    const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
    const COMPLETION: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";

    fn claim() -> SummaryClaim {
        SummaryClaim {
            outcome: crate::store::SummaryClaimOutcome::Inserted,
            request_id: "summary-request".into(),
            task_id: TASK.into(),
            generation: 1,
            previous_unread: None,
            completions: vec![PendingSummaryCompletion {
                completion_id: COMPLETION.into(),
                turn_pack: format!(
                    r#"{{"v":1,"turn_id":"{COMPLETION}","user":["discard-me-user"],"assistant":["final assistant result"],"tools":[{{"name":"discard-me-tool","status":"completed"}}]}}"#
                ),
            }],
        }
    }

    fn previous_document() -> SummaryDocument {
        SummaryDocument {
            schema: 1,
            facts: vec!["old fact".into()],
            pending: vec!["old pending".into()],
            decisions: vec![],
            spoken_text: "old spoken summary".into(),
            source_evidence: vec![],
            covers_new_completions: vec!["019fa972-5cfa-75e1-9008-0b17ade9a346".into()],
        }
    }

    #[test]
    fn summary_prompt_requires_simplified_chinese_spoken_text() {
        let prompt = build_prompt(&claim(), None).unwrap();
        let prompt = std::str::from_utf8(prompt.as_slice()).unwrap();
        assert!(prompt.contains("natural Simplified Chinese"));
        assert!(prompt.contains("required cumulative context"));
        assert!(prompt.contains("Do not copy previous spoken_text verbatim"));
        assert!(prompt.contains("authoritative final assistant reply"));
        assert!(prompt.contains("final assistant result"));
        assert!(prompt.contains("required_evidence_quote"));
        assert!(prompt.contains("private audit metadata, not narration"));
        assert!(prompt.contains("TTS reads spoken_text exactly"));
        assert!(prompt.contains("Lead immediately with the newest concrete user-visible result"));
        assert!(prompt.contains("prioritizing the last new completion"));
        assert!(prompt.contains("do not narrate a chronological history"));
        assert!(prompt.contains("End with a next action only when"));
        assert!(prompt.contains("aim for roughly 120 to 180 Chinese characters"));
        assert!(!prompt.contains("discard-me-user"));
        assert!(!prompt.contains("discard-me-tool"));
    }

    #[test]
    fn summary_prompt_includes_the_complete_previous_unheard_document() {
        let previous = previous_document();
        let prompt = build_prompt(&claim(), Some(&previous)).unwrap();
        let value: serde_json::Value =
            serde_json::from_slice(prompt.split(|byte| *byte == b'\n').nth(1).unwrap()).unwrap();
        assert_eq!(value["previous_unheard"]["facts"][0], "old fact");
        assert_eq!(
            value["previous_unheard"]["spoken_text"],
            "old spoken summary"
        );
    }

    fn auth(directory: &Path) -> PathBuf {
        let path = directory.join("auth.json");
        fs::write(&path, "{\"tokens\":\"fixture\"}").unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        path
    }

    fn script(directory: &Path, body: &str) -> PathBuf {
        let path = directory.join(format!("fake-codex-{}", uuid::Uuid::new_v4()));
        fs::write(&path, format!("#!/bin/sh\nset -eu\n{body}\n")).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
        path
    }

    fn config(root: &Path, executable: PathBuf) -> SparkRunnerConfig {
        SparkRunnerConfig {
            executable,
            supervisor_executable: None,
            auth_path: auth(root),
            temp_root: root.join("runs"),
            timeout: Duration::from_secs(15),
            max_stdout_bytes: 4096,
            max_stderr_bytes: 1024,
        }
    }

    fn success_script(root: &Path, extra: &str) -> PathBuf {
        script(
            root,
            &format!(
                r#"
output=''
schema=''
printf '%s\n' "$@" > "{args}"
env | LC_ALL=C sort > "{env}"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --output-last-message) output="$2"; shift 2 ;;
    --output-schema) schema="$2"; shift 2 ;;
    *) shift ;;
  esac
done
test -r "$CODEX_HOME/auth.json"
test ! -e "$CODEX_HOME/config.toml"
test ! -w "$CODEX_HOME/skills"
test ! -n "$(find "$CODEX_HOME/skills" -mindepth 1 -print -quit)"
test ! -w "$CODEX_HOME/tmp"
test -w "$CODEX_HOME/tmp/arg0"
test ! -n "$(find "$CODEX_HOME/tmp/arg0" -mindepth 1 -print -quit)"
test -r "$schema"
cat > "{stdin}"
printf '%s' '{{"schema":1,"facts":["done"],"pending":[],"decisions":[],"spoken_text":"done","covers_new_completions":["{completion}"]}}' > "$output"
{extra}
"#,
                args = root.join("args").display(),
                env = root.join("env").display(),
                stdin = root.join("stdin").display(),
                completion = COMPLETION,
            ),
        )
    }

    #[test]
    fn exact_isolated_argv_stdin_auth_snapshot_and_cleanup() {
        let root = tempdir().unwrap();
        let args = root.path().join("args");
        let env = root.path().join("env");
        let executable = success_script(root.path(), "");
        let runner = SparkRunner::new(config(root.path(), executable));
        let mut cumulative_claim = claim();
        cumulative_claim.previous_unread = Some(crate::store::UnreadSummary {
            task_id: TASK.into(),
            generation: 1,
            cache_object: "cache/previous".into(),
            coverage_count: 1,
        });
        cumulative_claim.generation = 2;
        let previous = previous_document();
        let outcome = runner
            .run_with_report(&cumulative_claim, Some(&previous))
            .unwrap();
        let document = outcome.document;
        assert_eq!(document.covers_new_completions, vec![COMPLETION]);
        assert_eq!(outcome.isolation.system_skill_files, 0);
        assert_eq!(outcome.isolation.plugin_files, 0);
        assert_eq!(outcome.isolation.persistent_runtime_rows, 0);
        assert!(outcome.isolation.workspace_max_nodes >= 4);
        assert!(outcome.isolation.workspace_max_bytes <= SPARK_MAX_WORKSPACE_BYTES);
        let prompt = fs::read_to_string(root.path().join("stdin")).unwrap();
        assert!(prompt.contains(r#""completion_id":"019fa972"#));
        assert!(prompt.contains("old fact"));
        assert!(!prompt.contains("summary-request"));
        let arguments = fs::read_to_string(args).unwrap();
        for required in [
            "exec",
            "--model",
            SPARK_MODEL,
            "--sandbox",
            "read-only",
            "--ephemeral",
            "--ignore-user-config",
            "--ignore-rules",
            "approval_policy=\"never\"",
            "--output-schema",
            "--output-last-message",
            "-",
        ] {
            assert!(arguments.lines().any(|line| line == required));
        }
        let argument_lines = arguments.lines().collect::<Vec<_>>();
        for feature in DISABLED_SPARK_FEATURES {
            assert!(
                argument_lines
                    .windows(2)
                    .any(|pair| pair == ["--disable", *feature])
            );
        }
        let environment = fs::read_to_string(env).unwrap();
        assert!(environment.contains("CODEX_HOME="));
        assert!(environment.contains("HOME="));
        assert!(environment.contains("TMPDIR="));
        assert!(!environment.contains("DASHSCOPE_API_KEY"));
        let leftovers = fs::read_dir(root.path().join("runs"))
            .unwrap()
            .filter_map(Result::ok)
            .filter(|entry| entry.file_name().to_string_lossy().starts_with("spark-"))
            .count();
        assert_eq!(leftovers, 0);
        assert_eq!(
            runner.run(&claim(), Some(&previous)),
            Err(SparkError::InvalidInput)
        );
    }

    #[test]
    fn rejects_missing_unsafe_or_replaced_auth() {
        let root = tempdir().unwrap();
        let executable = success_script(root.path(), "");
        let mut missing = config(root.path(), executable.clone());
        missing.auth_path = root.path().join("missing-auth");
        assert_eq!(
            SparkRunner::new(missing).run(&claim(), None),
            Err(SparkError::Authentication)
        );

        let unsafe_auth = root.path().join("unsafe-auth.json");
        fs::write(&unsafe_auth, "{}").unwrap();
        fs::set_permissions(&unsafe_auth, fs::Permissions::from_mode(0o644)).unwrap();
        let mut unsafe_config = config(root.path(), executable);
        unsafe_config.auth_path = unsafe_auth;
        assert_eq!(
            SparkRunner::new(unsafe_config).run(&claim(), None),
            Err(SparkError::Authentication)
        );
    }

    #[test]
    fn forbidden_or_aggregate_workspace_materialization_is_fail_closed() {
        for extra in [
            "mkdir -p \"$CODEX_HOME/plugins/cache\"; printf x > \"$CODEX_HOME/plugins/cache/payload\"; sleep 1",
            "chmod 700 \"$CODEX_HOME/skills\"; printf x > \"$CODEX_HOME/skills/evil\"; sleep 1",
            "printf x > \"$CODEX_HOME/memory.md\"; sleep 1",
            "dd if=/dev/zero of=\"$CODEX_HOME/models_cache.json\" bs=1048576 count=9 2>/dev/null; sleep 1",
            "printf x > \"$TMPDIR/unknown\"; sleep 1",
        ] {
            let root = tempdir().unwrap();
            let executable = success_script(root.path(), extra);
            assert_eq!(
                SparkRunner::new(config(root.path(), executable)).run(&claim(), None),
                Err(SparkError::UnsafeWorkspace)
            );
        }
    }

    #[test]
    fn nonempty_runtime_memory_goal_log_or_thread_tables_are_rejected() {
        let root = tempdir().unwrap();
        let cases = [
            (
                "goals_1.sqlite",
                "CREATE TABLE thread_goals(value TEXT); INSERT INTO thread_goals VALUES ('x'); CREATE TABLE thread_goal_continuation_deferrals(value TEXT);",
            ),
            (
                "memories_1.sqlite",
                "CREATE TABLE jobs(value TEXT); INSERT INTO jobs VALUES ('x'); CREATE TABLE stage1_outputs(value TEXT);",
            ),
            (
                "logs_2.sqlite",
                "CREATE TABLE logs(value TEXT); INSERT INTO logs VALUES ('x');",
            ),
            (
                "state_5.sqlite",
                "CREATE TABLE external_agent_config_imports(value TEXT); CREATE TABLE remote_control_enrollments(value TEXT); CREATE TABLE thread_dynamic_tools(value TEXT); CREATE TABLE thread_spawn_edges(value TEXT); CREATE TABLE threads(value TEXT); INSERT INTO threads VALUES ('x');",
            ),
        ];
        for (database, schema) in cases {
            let path = root.path().join(database);
            let connection = rusqlite::Connection::open(&path).unwrap();
            connection.execute_batch(schema).unwrap();
            drop(connection);
            assert_eq!(
                validate_runtime_databases(root.path()),
                Err(SparkError::UnsafeWorkspace)
            );
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn arg0_scratch_only_accepts_exact_codex_alias_shape_and_target() {
        let root = tempdir().unwrap();
        let executable = script(root.path(), "exit 0");
        let executable = fs::canonicalize(executable).unwrap();
        let arg0 = root.path().join("arg0");
        let aliases = arg0.join("codex-arg0Ab12Cd");
        fs::create_dir_all(&aliases).unwrap();
        fs::set_permissions(&aliases, fs::Permissions::from_mode(0o755)).unwrap();
        let lock = aliases.join(".lock");
        fs::write(&lock, b"").unwrap();
        fs::set_permissions(&lock, fs::Permissions::from_mode(0o644)).unwrap();
        for name in ["apply_patch", "applypatch", "codex-execve-wrapper"] {
            std::os::unix::fs::symlink(&executable, aliases.join(name)).unwrap();
        }
        let (nodes, _) = audit_arg0_scratch(&arg0, &executable).unwrap();
        assert_eq!(nodes, 5);

        fs::write(aliases.join("unexpected"), b"").unwrap();
        assert_eq!(
            audit_arg0_scratch(&arg0, &executable),
            Err(SparkError::UnsafeWorkspace)
        );
        fs::remove_file(aliases.join("unexpected")).unwrap();
        fs::remove_file(aliases.join("apply_patch")).unwrap();
        std::os::unix::fs::symlink(root.path().join("other"), aliases.join("apply_patch")).unwrap();
        assert_eq!(
            audit_arg0_scratch(&arg0, &executable),
            Err(SparkError::UnsafeWorkspace)
        );
    }

    #[test]
    fn prompt_input_gate_rejects_model_visible_skills_and_cleans_private_home() {
        let root = tempdir().unwrap();
        let clean = script(root.path(), "printf '[]'");
        let gate_root = root.path().join("clean-gates");
        let report = verify_spark_prompt_isolation(&clean, &gate_root).unwrap();
        assert_eq!(report.skills_instruction_blocks, 0);
        assert_eq!(report.skill_path_mentions, 0);
        assert_eq!(
            fs::read_dir(&gate_root)
                .unwrap()
                .filter_map(Result::ok)
                .count(),
            0
        );

        let injected = script(
            root.path(),
            r#"printf '["<skills_instructions>","fixture/SKILL.md"]'"#,
        );
        assert_eq!(
            verify_spark_prompt_isolation(&injected, &root.path().join("bad-gates")),
            Err(SparkError::UnsafeWorkspace)
        );
    }

    #[test]
    fn next_run_sweeps_crash_leftovers_and_rejects_unknown_root_content() {
        let root = tempdir().unwrap();
        let runs = root.path().join("runs");
        fs::create_dir(&runs).unwrap();
        fs::set_permissions(&runs, fs::Permissions::from_mode(0o700)).unwrap();
        let stale = runs.join("spark-stale");
        let stale_home = stale.join("home");
        fs::create_dir_all(&stale_home).unwrap();
        fs::write(stale_home.join("summary.output.json"), "private fixture").unwrap();
        for index in 0..40 {
            fs::write(stale_home.join(format!("entry-{index}")), b"fixture").unwrap();
        }
        let deep = stale.join("a/b/c/d/e/f");
        fs::create_dir_all(&deep).unwrap();
        std::os::unix::net::UnixListener::bind(deep.join("socket")).unwrap();
        let fifo_path = deep.join("fifo");
        let fifo = std::ffi::CString::new(fifo_path.as_os_str().as_bytes()).unwrap();
        assert_eq!(unsafe { libc::mkfifo(fifo.as_ptr(), 0o600) }, 0);
        fs::set_permissions(&stale_home, fs::Permissions::from_mode(0o500)).unwrap();
        let executable = success_script(root.path(), "");
        SparkRunner::new(config(root.path(), executable))
            .run(&claim(), None)
            .unwrap();
        assert!(!stale.exists());

        fs::write(runs.join("unknown"), "do not delete").unwrap();
        let executable = success_script(root.path(), "");
        assert_eq!(
            SparkRunner::new(config(root.path(), executable)).run(&claim(), None),
            Err(SparkError::UnsafeWorkspace)
        );
        assert!(runs.join("unknown").exists());
    }

    #[test]
    fn concurrent_runs_share_sweep_root_without_removing_live_workspaces() {
        let root = tempdir().unwrap();
        let auth_path = auth(root.path());
        let executable_a = success_script(root.path(), "sleep 0.05");
        let executable_b = success_script(root.path(), "sleep 0.05");
        let temp_root = root.path().join("runs");
        let config_a = SparkRunnerConfig {
            executable: executable_a,
            supervisor_executable: None,
            auth_path: auth_path.clone(),
            temp_root: temp_root.clone(),
            timeout: Duration::from_secs(5),
            max_stdout_bytes: 4096,
            max_stderr_bytes: 1024,
        };
        let config_b = SparkRunnerConfig {
            executable: executable_b,
            supervisor_executable: None,
            auth_path,
            temp_root,
            timeout: Duration::from_secs(5),
            max_stdout_bytes: 4096,
            max_stderr_bytes: 1024,
        };
        let first_claim = claim();
        let mut second_claim = claim();
        second_claim.task_id = "019fa972-5cfa-75e1-9008-0b17ade9a349".into();
        let first = std::thread::spawn(move || SparkRunner::new(config_a).run(&first_claim, None));
        let second =
            std::thread::spawn(move || SparkRunner::new(config_b).run(&second_claim, None));
        assert!(first.join().unwrap().is_ok());
        assert!(second.join().unwrap().is_ok());
        let leftovers = fs::read_dir(root.path().join("runs"))
            .unwrap()
            .filter_map(Result::ok)
            .filter(|entry| entry.file_name().to_string_lossy().starts_with("spark-"))
            .count();
        assert_eq!(leftovers, 0);
    }

    #[test]
    fn same_task_is_single_flight() {
        let root = tempdir().unwrap();
        let marker = root.path().join("first-running");
        let executable = script(
            root.path(),
            &format!(
                r#"
touch '{}'
output=''
while [ "$#" -gt 0 ]; do
  if [ "$1" = "--output-last-message" ]; then output="$2"; shift 2; else shift; fi
done
cat >/dev/null
printf '%s' '{{"schema":1,"facts":["done"],"pending":[],"decisions":[],"spoken_text":"done","covers_new_completions":["{COMPLETION}"]}}' > "$output"
sleep 0.2
"#,
                marker.display()
            ),
        );
        let runner = SparkRunner::new(config(root.path(), executable));
        let first_runner = runner.clone();
        let first = std::thread::spawn(move || first_runner.run(&claim(), None));
        let deadline = Instant::now() + Duration::from_secs(5);
        while !marker.exists() && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(5));
        }
        assert!(marker.exists());
        assert_eq!(runner.run(&claim(), None), Err(SparkError::Busy));
        assert!(first.join().unwrap().is_ok());
    }

    #[test]
    fn prompt_and_stdout_pipes_cannot_block_deadline_or_drain() {
        let root = tempdir().unwrap();
        let mut large_claim = claim();
        large_claim.completions[0].turn_pack = format!(
            r#"{{"v":1,"turn_id":"{COMPLETION}","user":[],"assistant":["{}"],"tools":[]}}"#,
            "x".repeat(MAX_TURN_PACK_BYTES / 2)
        );

        let never_reads = script(root.path(), "sleep 1");
        let mut timeout_config = config(root.path(), never_reads);
        timeout_config.timeout = Duration::from_millis(75);
        assert_eq!(
            SparkRunner::new(timeout_config).run(&large_claim, None),
            Err(SparkError::Timeout)
        );

        let stdout_first = script(
            root.path(),
            &format!(
                r#"
output=''
while [ "$#" -gt 0 ]; do
  if [ "$1" = "--output-last-message" ]; then output="$2"; shift 2; else shift; fi
done
(exec >/dev/null 2>&1; sleep 3; kill -KILL $$) &
dd if=/dev/zero bs=1024 count=512 2>/dev/null
cat >/dev/null
printf '%s' '{{"schema":1,"facts":["done"],"pending":[],"decisions":[],"spoken_text":"done","covers_new_completions":["{COMPLETION}"]}}' > "$output"
"#,
            ),
        );
        let mut stdout_config = config(root.path(), stdout_first);
        stdout_config.max_stdout_bytes = 1024 * 1024;
        assert!(
            SparkRunner::new(stdout_config)
                .run(&large_claim, None)
                .is_ok()
        );
    }

    #[test]
    fn growing_or_replaced_output_is_stopped_while_child_is_live() {
        let root = tempdir().unwrap();
        let growing = success_script(
            root.path(),
            "while :; do dd if=/dev/zero bs=4096 count=8 2>/dev/null >> \"$output\"; done",
        );
        let mut growing_config = config(root.path(), growing);
        growing_config.timeout = Duration::from_secs(10);
        assert_eq!(
            SparkRunner::new(growing_config).run(&claim(), None),
            Err(SparkError::OutputTooLarge)
        );

        let replaced = success_script(
            root.path(),
            "rm \"$output\"; printf '%s' '{\"schema\":1}' > \"$output\"; sleep 1",
        );
        assert_eq!(
            SparkRunner::new(config(root.path(), replaced)).run(&claim(), None),
            Err(SparkError::InvalidOutput)
        );

        let sentinel = root.path().join("sentinel");
        fs::write(&sentinel, b"must survive cleanup").unwrap();
        fs::set_permissions(&sentinel, fs::Permissions::from_mode(0o600)).unwrap();
        let output_hardlink = success_script(
            root.path(),
            &format!(
                "rm \"$output\"; ln '{}' \"$output\"; sleep 1",
                sentinel.display()
            ),
        );
        assert_eq!(
            SparkRunner::new(config(root.path(), output_hardlink)).run(&claim(), None),
            Err(SparkError::InvalidOutput)
        );
        assert_eq!(fs::read(&sentinel).unwrap(), b"must survive cleanup");

        let auth_hardlink = script(
            root.path(),
            &format!(
                "rm \"$CODEX_HOME/auth.json\"; ln '{}' \"$CODEX_HOME/auth.json\"; sleep 1",
                sentinel.display()
            ),
        );
        assert_eq!(
            SparkRunner::new(config(root.path(), auth_hardlink)).run(&claim(), None),
            Err(SparkError::Authentication)
        );
        assert_eq!(fs::read(&sentinel).unwrap(), b"must survive cleanup");
    }

    #[test]
    fn workspace_monitor_classifies_auth_size_hardlink_and_type_as_authentication() {
        let root = tempdir().unwrap();
        let temp_root = root.path().join("runs");
        secure_directory(&temp_root).unwrap();
        let executable = script(root.path(), "exit 0");
        let run = SparkRunFiles::create(&temp_root, b"fixture-auth", TASK).unwrap();
        let monitor = SparkWorkspaceMonitor::new(
            &run.codex_home,
            &run.auth_file,
            &run.sandbox_marker_file,
            &run.skills_dir,
            &run.home_tmpdir,
            &run.home_arg0dir,
            &run.tmpdir,
            &executable,
        )
        .unwrap();

        run.auth_file.set_len(MAX_AUTH_BYTES + 1).unwrap();
        assert_eq!(monitor.check(), Err(SparkError::Authentication));
        run.auth_file.set_len(b"fixture-auth".len() as u64).unwrap();

        let sentinel = root.path().join("auth-sentinel");
        fs::write(&sentinel, b"fixture-auth").unwrap();
        fs::set_permissions(&sentinel, fs::Permissions::from_mode(0o400)).unwrap();
        fs::remove_file(run.codex_home.join("auth.json")).unwrap();
        fs::hard_link(&sentinel, run.codex_home.join("auth.json")).unwrap();
        assert_eq!(monitor.check(), Err(SparkError::Authentication));
        assert_eq!(fs::read(&sentinel).unwrap(), b"fixture-auth");

        fs::remove_file(run.codex_home.join("auth.json")).unwrap();
        fs::create_dir(run.codex_home.join("auth.json")).unwrap();
        assert_eq!(monitor.check(), Err(SparkError::Authentication));
    }

    #[test]
    fn malformed_wrong_coverage_oversized_and_secret_output_fail_or_redact() {
        let root = tempdir().unwrap();
        for (body, expected) in [
            ("printf 'not-json' > \"$output\"", SparkError::InvalidOutput),
            (
                "printf '%s' '{\"schema\":1,\"facts\":[\"done\"],\"pending\":[],\"decisions\":[],\"spoken_text\":\"done\",\"covers_new_completions\":[\"019fa972-5cfa-75e1-9008-0b17ade9a349\"]}' > \"$output\"",
                SparkError::InvalidOutput,
            ),
            (
                "dd if=/dev/zero of=\"$output\" bs=1024 count=65 2>/dev/null",
                SparkError::OutputTooLarge,
            ),
        ] {
            let executable = success_script(root.path(), body);
            assert_eq!(
                SparkRunner::new(config(root.path(), executable)).run(&claim(), None),
                Err(expected)
            );
        }

        let token = "ghp_abcdefghijklmnopqrstuvwxyz1234567890";
        let private_path = "/Users/alice/private/project";
        let extra = format!(
            "printf '%s' '{{\"schema\":1,\"facts\":[\"{token}\",\"{private_path}\"],\"pending\":[],\"decisions\":[],\"spoken_text\":\"{token}\",\"covers_new_completions\":[\"{COMPLETION}\"]}}' > \"$output\""
        );
        let executable = success_script(root.path(), &extra);
        let document = SparkRunner::new(config(root.path(), executable))
            .run(&claim(), None)
            .unwrap();
        assert!(
            !document
                .canonical_json()
                .unwrap()
                .windows(token.len())
                .any(|w| w == token.as_bytes())
        );
        assert!(
            !document
                .facts
                .iter()
                .any(|value| value.contains(private_path))
        );
    }

    #[test]
    fn missing_cli_timeout_cancel_and_stderr_are_stable_categories() {
        let root = tempdir().unwrap();
        let missing = config(root.path(), root.path().join("missing"));
        assert_eq!(
            SparkRunner::new(missing).run(&claim(), None),
            Err(SparkError::CliMissing)
        );

        let sleepy = script(root.path(), "sleep 5");
        let mut sleepy_config = config(root.path(), sleepy);
        sleepy_config.timeout = Duration::from_millis(50);
        assert_eq!(
            SparkRunner::new(sleepy_config).run(&claim(), None),
            Err(SparkError::Timeout)
        );

        let auth_error = script(
            root.path(),
            "cat >/dev/null; printf 'not logged in: private' >&2; exit 1",
        );
        assert_eq!(
            SparkRunner::new(config(root.path(), auth_error)).run(&claim(), None),
            Err(SparkError::Authentication)
        );

        let signalled = script(root.path(), "cat >/dev/null; kill -TERM $$");
        assert_eq!(
            SparkRunner::new(config(root.path(), signalled)).run(&claim(), None),
            Err(SparkError::ExitFailure)
        );

        let noisy = script(
            root.path(),
            "cat >/dev/null; dd if=/dev/zero bs=1024 count=5 2>/dev/null",
        );
        assert_eq!(
            SparkRunner::new(config(root.path(), noisy)).run(&claim(), None),
            Err(SparkError::OutputTooLarge)
        );

        let cancelled = script(root.path(), "sleep 5");
        let cancel = AtomicBool::new(true);
        assert_eq!(
            SparkRunner::new(config(root.path(), cancelled)).run_with_cancel(
                &claim(),
                None,
                &cancel,
            ),
            Err(SparkError::Cancelled)
        );
    }
}

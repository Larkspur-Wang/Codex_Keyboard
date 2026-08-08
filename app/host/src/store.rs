use std::collections::BTreeSet;
use std::fs::{self, File};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use fs2::FileExt;
use rusqlite::{Connection, OpenFlags, OptionalExtension, TransactionBehavior, params};
use thiserror::Error;

use crate::cache::CacheId;
use crate::paths::{ExplicitFileLock, open_private_file};

pub const SCHEMA_VERSION: i64 = 6;
pub const MAX_PENDING_JOBS_PER_TASK: u32 = 12;
pub const MAX_GLOBAL_RUNNING_JOBS: u32 = 2;
pub const MAX_SUMMARY_COMPLETIONS_PER_CLAIM: usize = 32;
const MAX_REQUEST_ID_BYTES: usize = 128;
const MAX_TASK_ID_BYTES: usize = 64;
const MAX_PROMPT_BYTES: usize = 32 * 1024;
const MAX_CWD_BYTES: usize = 4 * 1024;
const MAX_ROLLOUT_PATH_BYTES: usize = 4 * 1024;
const MAX_TURN_PACK_BYTES: usize = 64 * 1024;
const MAX_SUMMARY_TURN_PACK_BYTES: usize = 1024 * 1024;

#[derive(Debug, Error)]
pub enum StoreError {
    #[error("state database I/O failed")]
    Io(#[from] std::io::Error),
    #[error("state database operation failed")]
    Sqlite(#[from] rusqlite::Error),
    #[error("unsupported schema version {0}")]
    UnsupportedSchema(i64),
    #[error("slot must be in 1..=4")]
    InvalidSlot,
    #[error("generation exceeds SQLite integer range")]
    GenerationOutOfRange,
    #[error("claim generation exceeds SQLite integer range")]
    ClaimGenerationOutOfRange,
    #[error("another Host process already owns this state database")]
    AlreadyRunning,
    #[error("request id was reused with a different immutable payload")]
    RequestIdConflict,
    #[error("job payload is invalid or exceeds a fixed bound")]
    InvalidJob,
    #[error("task already has the maximum number of queued or running jobs")]
    TaskQueueFull,
    #[error("state database path changed while it was being opened")]
    StatePathChanged,
    #[error("rollout cursor changed before the observation could be committed")]
    RolloutCursorChanged,
    #[error("task binding changed before the observation could be committed")]
    BindingChanged,
    #[error("completion id was reused for another task")]
    CompletionConflict,
    #[error("rollout cursor or completion payload is invalid")]
    InvalidCompletion,
    #[error("summary request is invalid")]
    InvalidSummaryRequest,
    #[error("another summary generation is already running for this task")]
    SummaryBusy,
    #[error("summary request id was reused for another task or generation")]
    SummaryRequestConflict,
    #[error("summary request was explicitly abandoned and cannot be reused")]
    SummaryRequestAbandoned,
    #[error("summary generation exceeds SQLite integer range")]
    SummaryGenerationOutOfRange,
    #[error("summary claim or current unread generation changed before publication")]
    SummaryClaimChanged,
    #[error("summary state is corrupt or violates its bounded contract")]
    InvalidSummaryState,
    #[error("summary cache reference does not match the claimed task and generation")]
    InvalidSummaryCacheReference,
    #[error("summary TTS attempt changed or no longer belongs to the active claim")]
    SummaryTtsAttemptChanged,
    #[error("playback lease payload is invalid")]
    InvalidPlaybackLease,
    #[error("another playback lease already owns this summary generation")]
    PlaybackBusy,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NewJob<'a> {
    pub request_id: &'a str,
    pub task_id: &'a str,
    pub slot: u8,
    pub generation: u64,
    pub prompt: &'a str,
    pub cwd: &'a Path,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Job {
    pub request_id: String,
    pub task_id: String,
    pub slot: u8,
    pub generation: u64,
    pub prompt: String,
    pub cwd: PathBuf,
    pub recovery_count: u32,
    pub claim_generation: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JobFailureKind {
    CliMissing,
    Authentication,
    TaskArchived,
    ActiveSession,
    Timeout,
    InvalidOutput,
    OutputTooLarge,
    UnsafeWorkingDirectory,
    ProcessIo,
    ExitFailure,
}

impl JobFailureKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::CliMissing => "cli_missing",
            Self::Authentication => "authentication",
            Self::TaskArchived => "task_archived",
            Self::ActiveSession => "active_session",
            Self::Timeout => "timeout",
            Self::InvalidOutput => "invalid_output",
            Self::OutputTooLarge => "output_too_large",
            Self::UnsafeWorkingDirectory => "unsafe_working_directory",
            Self::ProcessIo => "process_io",
            Self::ExitFailure => "exit_failure",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EnqueueOutcome {
    Inserted,
    Replay,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Binding {
    pub slot: u8,
    pub task_id: String,
    pub generation: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RolloutCursor {
    pub task_id: String,
    pub rollout_path: PathBuf,
    pub device: u64,
    pub inode: u64,
    pub offset: u64,
    pub generation: u64,
    pub anchor: [u8; 32],
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompletionOutcome {
    Inserted,
    Replay,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SummaryClaimOutcome {
    Inserted,
    Replay,
}

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct PendingSummaryCompletion {
    pub completion_id: String,
    pub turn_pack: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UnreadSummary {
    pub task_id: String,
    pub generation: u64,
    pub cache_object: String,
    pub coverage_count: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SummaryPlaybackLease {
    pub lease: u64,
    pub slot: u8,
    pub task_id: String,
    pub summary_generation: u64,
    pub request_generation: u32,
    pub connection_generation: u32,
    pub cache_object: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SummaryClaim {
    pub outcome: SummaryClaimOutcome,
    pub request_id: String,
    pub task_id: String,
    pub generation: u64,
    pub previous_unread: Option<UnreadSummary>,
    pub completions: Vec<PendingSummaryCompletion>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SummaryClaimResult {
    Claimed(SummaryClaim),
    Published { task_id: String, generation: u64 },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SummaryTtsAttemptState {
    Started,
    Ambiguous,
}

pub struct StateStore {
    connection: Connection,
    path: PathBuf,
    recovered_jobs: u64,
    recovered_summaries: u64,
    _instance_lock: Arc<ExplicitFileLock>,
    _database_guard: File,
}

pub struct ObserverStateStore {
    connection: Connection,
    path: PathBuf,
    _database_guard: File,
}

impl StateStore {
    pub fn open(path: &Path) -> Result<Self, StoreError> {
        if let Some(parent) = path.parent() {
            crate::paths::secure_directory(parent)?;
        }
        let lock_path = lock_path(path);
        let instance_lock = open_private_file(&lock_path)?;
        if let Err(error) = instance_lock.try_lock_exclusive() {
            if error.kind() == fs2::lock_contended_error().kind() {
                return Err(StoreError::AlreadyRunning);
            }
            return Err(StoreError::Io(error));
        }
        let instance_lock = Arc::new(ExplicitFileLock::from_locked(instance_lock));
        let database_guard = open_private_file(path)?;
        ensure_file_identity(&database_guard, path)?;
        let sidecar_guards = secure_existing_sidecars(path)?;
        let mut connection = Connection::open_with_flags(
            path,
            OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )?;
        ensure_file_identity(&database_guard, path)?;
        ensure_guarded_sidecars(&sidecar_guards)?;
        connection.busy_timeout(Duration::from_secs(5))?;
        ensure_supported_schema(&connection)?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        connection.pragma_update(None, "journal_mode", "DELETE")?;
        connection.pragma_update(None, "synchronous", "FULL")?;
        migrate(&mut connection)?;
        let recovered_jobs = recover_interrupted_jobs(&mut connection)?;
        let recovered_summaries = recover_interrupted_summaries(&mut connection)?;
        recover_interrupted_playback_leases(&mut connection)?;
        ensure_file_identity(&database_guard, path)?;
        ensure_guarded_sidecars(&sidecar_guards)?;
        drop(secure_existing_sidecars(path)?);
        Ok(Self {
            connection,
            path: path.to_path_buf(),
            recovered_jobs,
            recovered_summaries,
            _instance_lock: instance_lock,
            _database_guard: database_guard,
        })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn schema_version(&self) -> Result<i64, StoreError> {
        if has_jobs_column(&self.connection, "cwd")?
            && has_jobs_column(&self.connection, "failure_kind")?
            && has_table(&self.connection, "rollout_cursors")?
            && has_column(&self.connection, "completion_ledger", "turn_pack")?
            && has_column(&self.connection, "rollout_cursors", "anchor")?
            && has_column(&self.connection, "summary_ledger", "claim_id")?
            && has_column(&self.connection, "summary_ledger", "request_id")?
            && has_column(&self.connection, "summary_ledger", "previous_generation")?
            && has_column(&self.connection, "summary_ledger", "superseded_by")?
            && has_table(&self.connection, "summary_tts_attempts")?
            && has_table(&self.connection, "summary_playback_leases")?
        {
            Ok(SCHEMA_VERSION)
        } else if has_jobs_column(&self.connection, "cwd")?
            && has_jobs_column(&self.connection, "failure_kind")?
        {
            Ok(2)
        } else {
            Ok(self
                .connection
                .pragma_query_value(None, "user_version", |row| row.get(0))?)
        }
    }

    pub fn recovered_jobs_on_open(&self) -> u64 {
        self.recovered_jobs
    }

    pub fn recovered_summaries_on_open(&self) -> u64 {
        self.recovered_summaries
    }

    pub fn open_observer_store(&self) -> Result<ObserverStateStore, StoreError> {
        ensure_file_identity(&self._database_guard, &self.path)?;
        let database_guard = open_private_file(&self.path)?;
        ensure_file_identity(&self._database_guard, &self.path)?;
        ensure_file_identity(&database_guard, &self.path)?;
        let connection = Connection::open_with_flags(
            &self.path,
            OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )?;
        connection.busy_timeout(Duration::from_secs(5))?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        ensure_file_identity(&self._database_guard, &self.path)?;
        ensure_file_identity(&database_guard, &self.path)?;
        Ok(ObserverStateStore {
            connection,
            path: self.path.clone(),
            _database_guard: database_guard,
        })
    }

    pub fn open_worker_store(&self) -> Result<StateStore, StoreError> {
        ensure_file_identity(&self._database_guard, &self.path)?;
        let database_guard = open_private_file(&self.path)?;
        ensure_file_identity(&self._database_guard, &self.path)?;
        ensure_file_identity(&database_guard, &self.path)?;
        let connection = Connection::open_with_flags(
            &self.path,
            OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )?;
        connection.busy_timeout(Duration::from_secs(5))?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        ensure_file_identity(&self._database_guard, &self.path)?;
        ensure_file_identity(&database_guard, &self.path)?;
        Ok(StateStore {
            connection,
            path: self.path.clone(),
            recovered_jobs: 0,
            recovered_summaries: 0,
            _instance_lock: Arc::clone(&self._instance_lock),
            _database_guard: database_guard,
        })
    }

    pub fn enqueue(&mut self, job: &NewJob<'_>) -> Result<EnqueueOutcome, StoreError> {
        validate_slot(job.slot)?;
        validate_job(job)?;
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let existing: Option<(String, u8, i64, String, Vec<u8>)> = transaction
            .query_row(
                "SELECT task_id, slot, generation, prompt, cwd FROM jobs WHERE request_id = ?1",
                [job.request_id],
                |row| {
                    Ok((
                        row.get(0)?,
                        row.get(1)?,
                        row.get(2)?,
                        row.get(3)?,
                        row.get(4)?,
                    ))
                },
            )
            .optional()?;
        if let Some(existing) = existing {
            let exact_replay = existing.0 == job.task_id
                && existing.1 == job.slot
                && existing.2 == to_i64(job.generation)?
                && existing.3 == job.prompt
                && existing.4 == job.cwd.as_os_str().as_bytes();
            transaction.rollback()?;
            return if exact_replay {
                Ok(EnqueueOutcome::Replay)
            } else {
                Err(StoreError::RequestIdConflict)
            };
        }

        let pending: u32 = transaction.query_row(
            "SELECT COUNT(*) FROM jobs
             WHERE task_id = ?1 AND state IN ('queued', 'running')",
            [job.task_id],
            |row| row.get(0),
        )?;
        if pending >= MAX_PENDING_JOBS_PER_TASK {
            transaction.rollback()?;
            return Err(StoreError::TaskQueueFull);
        }
        transaction.execute(
            "INSERT INTO jobs
             (request_id, task_id, slot, generation, prompt, cwd, state)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'queued')",
            params![
                job.request_id,
                job.task_id,
                job.slot,
                to_i64(job.generation)?,
                job.prompt,
                job.cwd.as_os_str().as_bytes()
            ],
        )?;
        transaction.commit()?;
        Ok(EnqueueOutcome::Inserted)
    }

    #[cfg(test)]
    fn claim_next(&mut self, task_id: &str) -> Result<Option<Job>, StoreError> {
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let candidate: Option<(String, i64)> = transaction
            .query_row(
                "SELECT request_id, claim_generation FROM jobs
                 WHERE task_id = ?1 AND state = 'queued'
                   AND NOT EXISTS (
                     SELECT 1 FROM jobs running
                     WHERE running.task_id = ?1 AND running.state = 'running'
                   )
                 ORDER BY sequence ASC LIMIT 1",
                [task_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?;
        let Some((request_id, claim_generation)) = candidate else {
            transaction.commit()?;
            return Ok(None);
        };
        let next_claim_generation = claim_generation
            .checked_add(1)
            .ok_or(StoreError::ClaimGenerationOutOfRange)?;
        let changed = transaction.execute(
            "UPDATE jobs SET
               state = 'running',
               claim_generation = ?2,
               updated_at = unixepoch()
             WHERE request_id = ?1 AND state = 'queued'",
            params![request_id, next_claim_generation],
        )?;
        if changed != 1 {
            transaction.rollback()?;
            return Ok(None);
        }
        let job = transaction.query_row(
            "SELECT request_id, task_id, slot, generation, prompt, cwd, recovery_count,
                    claim_generation
             FROM jobs WHERE request_id = ?1",
            [&request_id],
            map_job,
        )?;
        transaction.commit()?;
        Ok(Some(job))
    }

    pub fn claim_next_runnable(&mut self) -> Result<Option<Job>, StoreError> {
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let running: u32 = transaction.query_row(
            "SELECT COUNT(*) FROM jobs WHERE state = 'running'",
            [],
            |row| row.get(0),
        )?;
        if running >= MAX_GLOBAL_RUNNING_JOBS {
            transaction.commit()?;
            return Ok(None);
        }
        let candidate: Option<(String, i64)> = transaction
            .query_row(
                "SELECT queued.request_id, queued.claim_generation
                 FROM jobs queued
                 WHERE queued.state = 'queued'
                   AND NOT EXISTS (
                     SELECT 1 FROM jobs running
                     WHERE running.task_id = queued.task_id AND running.state = 'running'
                   )
                   AND NOT EXISTS (
                     SELECT 1 FROM jobs earlier
                     WHERE earlier.task_id = queued.task_id AND earlier.state = 'queued'
                       AND earlier.sequence < queued.sequence
                   )
                 ORDER BY queued.sequence ASC LIMIT 1",
                [],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?;
        let Some((request_id, claim_generation)) = candidate else {
            transaction.commit()?;
            return Ok(None);
        };
        let next_claim_generation = claim_generation
            .checked_add(1)
            .ok_or(StoreError::ClaimGenerationOutOfRange)?;
        let changed = transaction.execute(
            "UPDATE jobs SET state = 'running', claim_generation = ?2,
                    failure_kind = NULL, updated_at = unixepoch()
             WHERE request_id = ?1 AND state = 'queued'",
            params![request_id, next_claim_generation],
        )?;
        if changed != 1 {
            transaction.rollback()?;
            return Ok(None);
        }
        let job = transaction.query_row(
            "SELECT request_id, task_id, slot, generation, prompt, cwd, recovery_count,
                    claim_generation FROM jobs WHERE request_id = ?1",
            [&request_id],
            map_job,
        )?;
        transaction.commit()?;
        Ok(Some(job))
    }

    pub fn set_binding(
        &mut self,
        slot: u8,
        expected_generation: Option<u64>,
        task_id: &str,
    ) -> Result<Option<Binding>, StoreError> {
        validate_slot(slot)?;
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let current: Option<(String, i64)> = transaction
            .query_row(
                "SELECT task_id, generation FROM bindings WHERE slot = ?1",
                [slot],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?;
        let current_generation = current.as_ref().map(|(_, generation)| *generation as u64);
        if current_generation != expected_generation {
            transaction.rollback()?;
            return Ok(None);
        }
        let generation = match expected_generation {
            Some(value) => value
                .checked_add(1)
                .ok_or(StoreError::GenerationOutOfRange)?,
            None => 1,
        };
        let generation_i64 = to_i64(generation)?;
        transaction.execute(
            "INSERT INTO bindings (slot, task_id, generation, updated_at)
             VALUES (?1, ?2, ?3, unixepoch())
             ON CONFLICT(slot) DO UPDATE SET
               task_id = excluded.task_id,
               generation = excluded.generation,
               updated_at = excluded.updated_at",
            params![slot, task_id, generation_i64],
        )?;
        transaction.commit()?;
        Ok(Some(Binding {
            slot,
            task_id: task_id.to_owned(),
            generation,
        }))
    }

    pub fn binding(&self, slot: u8) -> Result<Option<Binding>, StoreError> {
        validate_slot(slot)?;
        Ok(self
            .connection
            .query_row(
                "SELECT slot, task_id, generation FROM bindings WHERE slot = ?1",
                [slot],
                |row| {
                    Ok(Binding {
                        slot: row.get(0)?,
                        task_id: row.get(1)?,
                        generation: row.get::<_, i64>(2)? as u64,
                    })
                },
            )
            .optional()?)
    }

    pub fn bindings(&self) -> Result<Vec<Binding>, StoreError> {
        query_bindings(&self.connection)
    }

    pub fn mark_completed(
        &mut self,
        request_id: &str,
        claim_generation: u64,
    ) -> Result<bool, StoreError> {
        Ok(self.connection.execute(
            "UPDATE jobs SET state = 'completed', updated_at = unixepoch()
             WHERE request_id = ?1 AND state = 'running' AND claim_generation = ?2",
            params![request_id, to_i64(claim_generation)?],
        )? == 1)
    }

    pub fn mark_failed(
        &mut self,
        request_id: &str,
        claim_generation: u64,
        failure: JobFailureKind,
    ) -> Result<bool, StoreError> {
        Ok(self.connection.execute(
            "UPDATE jobs SET state = 'failed', failure_kind = ?3, updated_at = unixepoch()
             WHERE request_id = ?1 AND state = 'running' AND claim_generation = ?2",
            params![request_id, to_i64(claim_generation)?, failure.as_str()],
        )? == 1)
    }

    pub fn pending_count(&self, task_id: &str) -> Result<u32, StoreError> {
        Ok(self.connection.query_row(
            "SELECT COUNT(*) FROM jobs
             WHERE task_id = ?1 AND state IN ('queued', 'running')",
            [task_id],
            |row| row.get(0),
        )?)
    }

    pub fn rollout_cursor(&self, task_id: &str) -> Result<Option<RolloutCursor>, StoreError> {
        query_rollout_cursor(&self.connection, task_id)
    }

    pub fn commit_rollout_completion(
        &mut self,
        expected: Option<&RolloutCursor>,
        next: &RolloutCursor,
        completion_id: &str,
        turn_pack: &str,
    ) -> Result<CompletionOutcome, StoreError> {
        commit_rollout_completion_on(
            &mut self.connection,
            None,
            expected,
            next,
            completion_id,
            turn_pack,
        )
    }

    pub fn commit_bound_rollout_completion(
        &mut self,
        binding: &Binding,
        expected: Option<&RolloutCursor>,
        next: &RolloutCursor,
        completion_id: &str,
        turn_pack: &str,
    ) -> Result<CompletionOutcome, StoreError> {
        commit_rollout_completion_on(
            &mut self.connection,
            Some(binding),
            expected,
            next,
            completion_id,
            turn_pack,
        )
    }

    pub fn claim_summary(
        &mut self,
        task_id: &str,
        request_id: &str,
    ) -> Result<Option<SummaryClaimResult>, StoreError> {
        validate_summary_request(task_id, request_id)?;
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let prior_request: Option<(String, i64, String, Option<i64>, String)> = transaction
            .query_row(
                "SELECT task_id, generation, state, previous_generation, covers_completions
                 FROM summary_ledger WHERE request_id = ?1",
                [request_id],
                |row| {
                    Ok((
                        row.get(0)?,
                        row.get(1)?,
                        row.get(2)?,
                        row.get(3)?,
                        row.get(4)?,
                    ))
                },
            )
            .optional()?;
        if let Some((prior_task, generation, state, previous_generation, covers)) = prior_request {
            if prior_task != task_id {
                transaction.rollback()?;
                return Err(StoreError::SummaryRequestConflict);
            }
            match state.as_str() {
                "unheard" | "leased" | "heard" | "superseded" => {
                    transaction.commit()?;
                    return Ok(Some(SummaryClaimResult::Published {
                        task_id: task_id.to_owned(),
                        generation: generation as u64,
                    }));
                }
                "abandoned" => {
                    transaction.rollback()?;
                    return Err(StoreError::SummaryRequestAbandoned);
                }
                "interrupted" => {
                    let changed = transaction.execute(
                        "UPDATE summary_ledger SET state = 'generating', updated_at = unixepoch()
                         WHERE request_id = ?1 AND task_id = ?2 AND generation = ?3
                           AND state = 'interrupted' AND claim_id = ?1",
                        params![request_id, task_id, generation],
                    )?;
                    if changed != 1 {
                        transaction.rollback()?;
                        return Err(StoreError::SummaryClaimChanged);
                    }
                }
                "generating" => {}
                _ => {
                    transaction.rollback()?;
                    return Err(StoreError::InvalidSummaryState);
                }
            }
            let claim = load_summary_claim(
                &transaction,
                SummaryClaimOutcome::Replay,
                request_id,
                task_id,
                generation,
                previous_generation,
                &covers,
            )?;
            transaction.commit()?;
            return Ok(Some(SummaryClaimResult::Claimed(claim)));
        }
        let existing: Option<(String, i64, Option<i64>, String)> = transaction
            .query_row(
                "SELECT claim_id, generation, previous_generation, covers_completions
                 FROM summary_ledger
                 WHERE task_id = ?1 AND state IN ('generating', 'interrupted')",
                [task_id],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
            )
            .optional()?;
        if let Some((claim_id, generation, previous_generation, covers)) = existing {
            if claim_id != request_id {
                transaction.rollback()?;
                return Err(StoreError::SummaryBusy);
            }
            let claim = load_summary_claim(
                &transaction,
                SummaryClaimOutcome::Replay,
                request_id,
                task_id,
                generation,
                previous_generation,
                &covers,
            )?;
            transaction.commit()?;
            return Ok(Some(SummaryClaimResult::Claimed(claim)));
        }

        let previous_unread = query_current_unread(&transaction, task_id)?;
        let completions = query_pending_summary_completions(&transaction, task_id)?;
        if completions.is_empty() {
            transaction.commit()?;
            return Ok(None);
        }
        let generation: i64 = transaction.query_row(
            "SELECT COALESCE(MAX(generation), 0) FROM summary_ledger WHERE task_id = ?1",
            [task_id],
            |row| row.get(0),
        )?;
        let generation = generation
            .checked_add(1)
            .ok_or(StoreError::SummaryGenerationOutOfRange)?;
        let completion_ids = completions
            .iter()
            .map(|completion| completion.completion_id.as_str())
            .collect::<Vec<_>>();
        let covers =
            serde_json::to_string(&completion_ids).map_err(|_| StoreError::InvalidSummaryState)?;
        transaction.execute(
            "INSERT INTO summary_ledger
             (task_id, generation, state, covers_completions, cache_object, request_id, claim_id,
              previous_generation, superseded_by, created_at, updated_at)
             VALUES (?1, ?2, 'generating', ?3, NULL, ?4, ?4, ?5, NULL, unixepoch(), unixepoch())",
            params![
                task_id,
                generation,
                covers,
                request_id,
                previous_unread
                    .as_ref()
                    .map(|summary| to_i64(summary.generation))
                    .transpose()?
            ],
        )?;
        transaction.commit()?;
        Ok(Some(SummaryClaimResult::Claimed(SummaryClaim {
            outcome: SummaryClaimOutcome::Inserted,
            request_id: request_id.to_owned(),
            task_id: task_id.to_owned(),
            generation: generation as u64,
            previous_unread,
            completions,
        })))
    }

    pub fn abandon_summary_claim(&mut self, claim: &SummaryClaim) -> Result<bool, StoreError> {
        validate_summary_claim(claim)?;
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let changed = transaction.execute(
            "UPDATE summary_ledger
             SET state = 'abandoned', claim_id = NULL, updated_at = unixepoch()
             WHERE task_id = ?1 AND generation = ?2
               AND state IN ('generating', 'interrupted') AND claim_id = ?3",
            params![claim.task_id, to_i64(claim.generation)?, claim.request_id],
        )?;
        if changed == 1 {
            transaction.execute(
                "DELETE FROM summary_tts_attempts WHERE task_id = ?1 AND generation = ?2",
                params![claim.task_id, to_i64(claim.generation)?],
            )?;
        }
        transaction.commit()?;
        Ok(changed == 1)
    }

    pub fn summary_tts_attempt(
        &self,
        claim: &SummaryClaim,
    ) -> Result<Option<SummaryTtsAttemptState>, StoreError> {
        validate_summary_claim(claim)?;
        let state: Option<String> = self
            .connection
            .query_row(
                "SELECT attempt.state FROM summary_tts_attempts attempt
                 JOIN summary_ledger summary
                   ON summary.task_id = attempt.task_id
                  AND summary.generation = attempt.generation
                 WHERE attempt.task_id = ?1 AND attempt.generation = ?2
                   AND summary.request_id = ?3
                   AND summary.state IN ('generating', 'interrupted')",
                params![claim.task_id, to_i64(claim.generation)?, claim.request_id],
                |row| row.get(0),
            )
            .optional()?;
        state
            .map(|value| match value.as_str() {
                "started" => Ok(SummaryTtsAttemptState::Started),
                "ambiguous" => Ok(SummaryTtsAttemptState::Ambiguous),
                _ => Err(StoreError::InvalidSummaryState),
            })
            .transpose()
    }

    pub fn begin_summary_tts_attempt(
        &mut self,
        claim: &SummaryClaim,
    ) -> Result<SummaryTtsAttemptState, StoreError> {
        validate_summary_claim(claim)?;
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let active: bool = transaction.query_row(
            "SELECT EXISTS(
               SELECT 1 FROM summary_ledger
               WHERE task_id = ?1 AND generation = ?2 AND request_id = ?3
                 AND claim_id = ?3 AND state IN ('generating', 'interrupted')
             )",
            params![claim.task_id, to_i64(claim.generation)?, claim.request_id],
            |row| row.get(0),
        )?;
        if !active {
            transaction.rollback()?;
            return Err(StoreError::SummaryTtsAttemptChanged);
        }
        transaction.execute(
            "INSERT INTO summary_tts_attempts
             (task_id, generation, state, created_at, updated_at)
             VALUES (?1, ?2, 'started', unixepoch(), unixepoch())
             ON CONFLICT(task_id, generation) DO NOTHING",
            params![claim.task_id, to_i64(claim.generation)?],
        )?;
        let state: String = transaction.query_row(
            "SELECT state FROM summary_tts_attempts WHERE task_id = ?1 AND generation = ?2",
            params![claim.task_id, to_i64(claim.generation)?],
            |row| row.get(0),
        )?;
        transaction.commit()?;
        match state.as_str() {
            "started" => Ok(SummaryTtsAttemptState::Started),
            "ambiguous" => Ok(SummaryTtsAttemptState::Ambiguous),
            _ => Err(StoreError::InvalidSummaryState),
        }
    }

    pub fn mark_summary_tts_ambiguous(&mut self, claim: &SummaryClaim) -> Result<(), StoreError> {
        validate_summary_claim(claim)?;
        let changed = self.connection.execute(
            "UPDATE summary_tts_attempts SET state = 'ambiguous', updated_at = unixepoch()
             WHERE task_id = ?1 AND generation = ?2
               AND EXISTS(
                 SELECT 1 FROM summary_ledger
                 WHERE task_id = ?1 AND generation = ?2 AND request_id = ?3
                   AND claim_id = ?3 AND state IN ('generating', 'interrupted')
               )",
            params![claim.task_id, to_i64(claim.generation)?, claim.request_id],
        )?;
        if changed == 1 {
            Ok(())
        } else {
            Err(StoreError::SummaryTtsAttemptChanged)
        }
    }

    pub fn resume_interrupted_summary(
        &mut self,
        task_id: &str,
    ) -> Result<Option<SummaryClaim>, StoreError> {
        if uuid::Uuid::parse_str(task_id).is_err() {
            return Err(StoreError::InvalidSummaryRequest);
        }
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let interrupted: Option<(String, i64, Option<i64>, String)> = transaction
            .query_row(
                "SELECT claim_id, generation, previous_generation, covers_completions
                 FROM summary_ledger WHERE task_id = ?1 AND state = 'interrupted'",
                [task_id],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
            )
            .optional()?;
        let Some((request_id, generation, previous_generation, covers)) = interrupted else {
            transaction.commit()?;
            return Ok(None);
        };
        let changed = transaction.execute(
            "UPDATE summary_ledger SET state = 'generating', updated_at = unixepoch()
             WHERE task_id = ?1 AND generation = ?2 AND state = 'interrupted'
               AND claim_id = ?3",
            params![task_id, generation, request_id],
        )?;
        if changed != 1 {
            transaction.rollback()?;
            return Err(StoreError::SummaryClaimChanged);
        }
        let claim = load_summary_claim(
            &transaction,
            SummaryClaimOutcome::Replay,
            &request_id,
            task_id,
            generation,
            previous_generation,
            &covers,
        )?;
        transaction.commit()?;
        Ok(Some(claim))
    }

    pub fn publish_summary(
        &mut self,
        claim: &SummaryClaim,
        cache_object: &str,
    ) -> Result<UnreadSummary, StoreError> {
        validate_summary_claim(claim)?;
        let expected_cache = CacheId::for_task(&claim.task_id, claim.generation)
            .map_err(|_| StoreError::InvalidSummaryCacheReference)?
            .reference();
        if cache_object != expected_cache {
            return Err(StoreError::InvalidSummaryCacheReference);
        }
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let stored: Option<(Option<i64>, String)> = transaction
            .query_row(
                "SELECT previous_generation, covers_completions FROM summary_ledger
                 WHERE task_id = ?1 AND generation = ?2 AND state = 'generating'
                   AND claim_id = ?3",
                params![claim.task_id, to_i64(claim.generation)?, claim.request_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?;
        let Some((stored_previous, stored_covers)) = stored else {
            transaction.rollback()?;
            return Err(StoreError::SummaryClaimChanged);
        };
        let expected_previous = claim
            .previous_unread
            .as_ref()
            .map(|summary| to_i64(summary.generation))
            .transpose()?;
        let expected_ids = claim
            .completions
            .iter()
            .map(|completion| completion.completion_id.as_str())
            .collect::<Vec<_>>();
        let expected_covers =
            serde_json::to_string(&expected_ids).map_err(|_| StoreError::InvalidSummaryState)?;
        if stored_previous != expected_previous || stored_covers != expected_covers {
            transaction.rollback()?;
            return Err(StoreError::SummaryClaimChanged);
        }

        let mut previous_coverage = 0_u32;
        if let Some(previous) = &claim.previous_unread {
            let current = query_current_unread(&transaction, &claim.task_id)?;
            if current.as_ref() != Some(previous) {
                transaction.rollback()?;
                return Err(StoreError::SummaryClaimChanged);
            }
            let changed = transaction.execute(
                "UPDATE summary_ledger
                 SET state = CASE WHEN state = 'unheard' THEN 'superseded' ELSE state END,
                     superseded_by = ?3, updated_at = unixepoch()
                 WHERE task_id = ?1 AND generation = ?2
                   AND state IN ('unheard', 'leased') AND superseded_by IS NULL",
                params![
                    claim.task_id,
                    to_i64(previous.generation)?,
                    to_i64(claim.generation)?
                ],
            )?;
            if changed != 1 {
                transaction.rollback()?;
                return Err(StoreError::SummaryClaimChanged);
            }
            previous_coverage = transaction.execute(
                "UPDATE completion_ledger SET summarized_generation = ?3
                 WHERE task_id = ?1 AND summarized_generation = ?2",
                params![
                    claim.task_id,
                    to_i64(previous.generation)?,
                    to_i64(claim.generation)?
                ],
            )? as u32;
            if previous_coverage != previous.coverage_count {
                transaction.rollback()?;
                return Err(StoreError::SummaryClaimChanged);
            }
        } else if query_current_unread(&transaction, &claim.task_id)?.is_some() {
            transaction.rollback()?;
            return Err(StoreError::SummaryClaimChanged);
        }

        for completion in &claim.completions {
            let changed = transaction.execute(
                "UPDATE completion_ledger SET summarized_generation = ?4
                 WHERE completion_id = ?1 AND task_id = ?2 AND turn_pack = ?3
                   AND summarized_generation IS NULL",
                params![
                    completion.completion_id,
                    claim.task_id,
                    completion.turn_pack,
                    to_i64(claim.generation)?
                ],
            )?;
            if changed != 1 {
                transaction.rollback()?;
                return Err(StoreError::SummaryClaimChanged);
            }
        }
        let changed = transaction.execute(
            "UPDATE summary_ledger SET state = 'unheard', cache_object = ?4,
                    claim_id = NULL, updated_at = unixepoch()
             WHERE task_id = ?1 AND generation = ?2 AND state = 'generating'
               AND claim_id = ?3",
            params![
                claim.task_id,
                to_i64(claim.generation)?,
                claim.request_id,
                cache_object
            ],
        )?;
        if changed != 1 {
            transaction.rollback()?;
            return Err(StoreError::SummaryClaimChanged);
        }
        transaction.execute(
            "DELETE FROM summary_tts_attempts WHERE task_id = ?1 AND generation = ?2",
            params![claim.task_id, to_i64(claim.generation)?],
        )?;
        let coverage_count = previous_coverage
            .checked_add(claim.completions.len() as u32)
            .ok_or(StoreError::InvalidSummaryState)?;
        transaction.commit()?;
        Ok(UnreadSummary {
            task_id: claim.task_id.clone(),
            generation: claim.generation,
            cache_object: cache_object.to_owned(),
            coverage_count,
        })
    }

    pub fn current_unread_summary(
        &self,
        task_id: &str,
    ) -> Result<Option<UnreadSummary>, StoreError> {
        if uuid::Uuid::parse_str(task_id).is_err() {
            return Err(StoreError::InvalidSummaryRequest);
        }
        query_current_unread(&self.connection, task_id)
    }

    pub fn acquire_summary_playback(
        &mut self,
        slot: u8,
        request_generation: u32,
        connection_generation: u32,
        lease: u64,
    ) -> Result<Option<SummaryPlaybackLease>, StoreError> {
        validate_slot(slot)?;
        if request_generation == 0
            || connection_generation == 0
            || lease == 0
            || lease > i64::MAX as u64
        {
            return Err(StoreError::InvalidPlaybackLease);
        }
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let task_id: Option<String> = transaction
            .query_row(
                "SELECT task_id FROM bindings WHERE slot = ?1",
                [slot],
                |row| row.get(0),
            )
            .optional()?;
        let Some(task_id) = task_id else {
            transaction.commit()?;
            return Ok(None);
        };
        let summary: Option<(i64, String, String)> = transaction
            .query_row(
                "SELECT generation, cache_object, state FROM summary_ledger
                 WHERE task_id = ?1 AND state IN ('unheard', 'leased')
                   AND superseded_by IS NULL",
                [&task_id],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .optional()?;
        let Some((generation, cache_object, state)) = summary else {
            transaction.commit()?;
            return Ok(None);
        };
        let existing: Option<(i64, u8, u32, u32)> = transaction
            .query_row(
                "SELECT lease_id, slot, request_generation, connection_generation
                 FROM summary_playback_leases WHERE task_id = ?1 AND summary_generation = ?2",
                params![task_id, generation],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
            )
            .optional()?;
        if let Some((existing_lease, existing_slot, existing_request, existing_connection)) =
            existing
        {
            transaction.commit()?;
            if existing_lease == lease as i64
                && existing_slot == slot
                && existing_request == request_generation
                && existing_connection == connection_generation
            {
                return Ok(Some(SummaryPlaybackLease {
                    lease,
                    slot,
                    task_id,
                    summary_generation: generation as u64,
                    request_generation,
                    connection_generation,
                    cache_object,
                }));
            }
            return Err(StoreError::PlaybackBusy);
        }
        if state != "unheard" {
            transaction.rollback()?;
            return Err(StoreError::PlaybackBusy);
        }
        let changed = transaction.execute(
            "UPDATE summary_ledger SET state = 'leased', updated_at = unixepoch()
             WHERE task_id = ?1 AND generation = ?2 AND state = 'unheard'
               AND superseded_by IS NULL",
            params![task_id, generation],
        )?;
        if changed != 1 {
            transaction.rollback()?;
            return Err(StoreError::PlaybackBusy);
        }
        transaction.execute(
            "INSERT INTO summary_playback_leases
             (lease_id, task_id, summary_generation, slot, request_generation,
              connection_generation, created_at, updated_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, unixepoch(), unixepoch())",
            params![
                lease as i64,
                task_id,
                generation,
                slot,
                request_generation,
                connection_generation
            ],
        )?;
        transaction.commit()?;
        Ok(Some(SummaryPlaybackLease {
            lease,
            slot,
            task_id,
            summary_generation: generation as u64,
            request_generation,
            connection_generation,
            cache_object,
        }))
    }

    pub fn cancel_summary_playback(
        &mut self,
        lease: &SummaryPlaybackLease,
    ) -> Result<bool, StoreError> {
        validate_playback_lease(lease)?;
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let removed = transaction.execute(
            "DELETE FROM summary_playback_leases
             WHERE lease_id = ?1 AND task_id = ?2 AND summary_generation = ?3
               AND slot = ?4 AND request_generation = ?5 AND connection_generation = ?6",
            params![
                to_i64(lease.lease)?,
                lease.task_id,
                to_i64(lease.summary_generation)?,
                lease.slot,
                lease.request_generation,
                lease.connection_generation
            ],
        )?;
        if removed == 1 {
            transaction.execute(
                "UPDATE summary_ledger
                 SET state = CASE WHEN superseded_by IS NULL THEN 'unheard' ELSE 'superseded' END,
                     updated_at = unixepoch()
                 WHERE task_id = ?1 AND generation = ?2 AND state = 'leased'",
                params![lease.task_id, to_i64(lease.summary_generation)?],
            )?;
        }
        transaction.commit()?;
        Ok(removed == 1)
    }

    pub fn finish_summary_playback(
        &mut self,
        lease: &SummaryPlaybackLease,
    ) -> Result<bool, StoreError> {
        validate_playback_lease(lease)?;
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let exact: bool = transaction.query_row(
            "SELECT EXISTS(
               SELECT 1 FROM summary_playback_leases
               WHERE lease_id = ?1 AND task_id = ?2 AND summary_generation = ?3
                 AND slot = ?4 AND request_generation = ?5 AND connection_generation = ?6
             )",
            params![
                to_i64(lease.lease)?,
                lease.task_id,
                to_i64(lease.summary_generation)?,
                lease.slot,
                lease.request_generation,
                lease.connection_generation
            ],
            |row| row.get(0),
        )?;
        if !exact {
            transaction.commit()?;
            return Ok(false);
        }
        let changed = transaction.execute(
            "UPDATE summary_ledger SET state = 'heard', updated_at = unixepoch()
             WHERE task_id = ?1 AND generation = ?2 AND state = 'leased'
               AND cache_object = ?3",
            params![
                lease.task_id,
                to_i64(lease.summary_generation)?,
                lease.cache_object
            ],
        )?;
        if changed != 1 {
            transaction.rollback()?;
            return Err(StoreError::InvalidSummaryState);
        }
        transaction.execute(
            "DELETE FROM summary_playback_leases WHERE lease_id = ?1",
            [to_i64(lease.lease)?],
        )?;
        transaction.commit()?;
        Ok(true)
    }

    pub fn retained_summary_cache_references(&self) -> Result<BTreeSet<String>, StoreError> {
        let mut statement = self.connection.prepare(
            "SELECT cache_object FROM summary_ledger
             WHERE state IN ('unheard', 'leased') AND cache_object IS NOT NULL",
        )?;
        let rows = statement.query_map([], |row| row.get::<_, String>(0))?;
        let mut references = BTreeSet::new();
        for reference in rows {
            references.insert(reference?);
        }
        Ok(references)
    }

    pub fn pending_summary_completion_count(&self, task_id: &str) -> Result<u32, StoreError> {
        if uuid::Uuid::parse_str(task_id).is_err() {
            return Err(StoreError::InvalidSummaryRequest);
        }
        Ok(self.connection.query_row(
            "SELECT COUNT(*) FROM completion_ledger
             WHERE task_id = ?1 AND summarized_generation IS NULL",
            [task_id],
            |row| row.get(0),
        )?)
    }

    pub fn summary_work_tasks_after(
        &self,
        after_task_id: Option<&str>,
    ) -> Result<Vec<String>, StoreError> {
        let mut statement = self.connection.prepare(
            "SELECT task_id FROM (
               SELECT task_id FROM summary_ledger WHERE state = 'interrupted'
               UNION
               SELECT task_id FROM completion_ledger WHERE summarized_generation IS NULL
             ) WHERE (?1 IS NULL OR task_id > ?1)
             ORDER BY task_id LIMIT 32",
        )?;
        let rows = statement.query_map([after_task_id], |row| row.get::<_, String>(0))?;
        let mut tasks = Vec::new();
        for row in rows {
            let task = row?;
            if uuid::Uuid::parse_str(&task).is_err() {
                return Err(StoreError::InvalidSummaryState);
            }
            tasks.push(task);
        }
        Ok(tasks)
    }

    #[cfg(test)]
    pub(crate) fn completion_count(&self, task_id: &str) -> Result<u32, StoreError> {
        Ok(self.connection.query_row(
            "SELECT COUNT(*) FROM completion_ledger WHERE task_id = ?1",
            [task_id],
            |row| row.get(0),
        )?)
    }

    #[cfg(test)]
    pub(crate) fn completion_turn_pack(&self, completion_id: &str) -> Result<String, StoreError> {
        Ok(self.connection.query_row(
            "SELECT turn_pack FROM completion_ledger WHERE completion_id = ?1",
            [completion_id],
            |row| row.get(0),
        )?)
    }

    #[cfg(test)]
    fn job_state(&self, request_id: &str) -> Result<(String, u32), StoreError> {
        Ok(self.connection.query_row(
            "SELECT state, recovery_count FROM jobs WHERE request_id = ?1",
            [request_id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )?)
    }
}

impl ObserverStateStore {
    pub fn bindings(&self) -> Result<Vec<Binding>, StoreError> {
        ensure_file_identity(&self._database_guard, &self.path)?;
        query_bindings(&self.connection)
    }

    pub fn rollout_cursor(&self, task_id: &str) -> Result<Option<RolloutCursor>, StoreError> {
        ensure_file_identity(&self._database_guard, &self.path)?;
        query_rollout_cursor(&self.connection, task_id)
    }

    pub fn commit_rollout_completion(
        &mut self,
        expected: Option<&RolloutCursor>,
        next: &RolloutCursor,
        completion_id: &str,
        turn_pack: &str,
    ) -> Result<CompletionOutcome, StoreError> {
        ensure_file_identity(&self._database_guard, &self.path)?;
        let outcome = commit_rollout_completion_on(
            &mut self.connection,
            None,
            expected,
            next,
            completion_id,
            turn_pack,
        )?;
        ensure_file_identity(&self._database_guard, &self.path)?;
        Ok(outcome)
    }

    pub fn commit_bound_rollout_completion(
        &mut self,
        binding: &Binding,
        expected: Option<&RolloutCursor>,
        next: &RolloutCursor,
        completion_id: &str,
        turn_pack: &str,
    ) -> Result<CompletionOutcome, StoreError> {
        ensure_file_identity(&self._database_guard, &self.path)?;
        let outcome = commit_rollout_completion_on(
            &mut self.connection,
            Some(binding),
            expected,
            next,
            completion_id,
            turn_pack,
        )?;
        ensure_file_identity(&self._database_guard, &self.path)?;
        Ok(outcome)
    }
}

fn query_bindings(connection: &Connection) -> Result<Vec<Binding>, StoreError> {
    let mut statement = connection
        .prepare("SELECT slot, task_id, generation FROM bindings ORDER BY slot ASC LIMIT 4")?;
    let rows = statement.query_map([], |row| {
        Ok(Binding {
            slot: row.get(0)?,
            task_id: row.get(1)?,
            generation: row.get::<_, i64>(2)? as u64,
        })
    })?;
    rows.collect::<Result<Vec<_>, _>>()
        .map_err(StoreError::from)
}

fn query_rollout_cursor(
    connection: &Connection,
    task_id: &str,
) -> Result<Option<RolloutCursor>, StoreError> {
    connection
        .query_row(
            "SELECT task_id, rollout_path, device, inode, offset, generation, anchor
             FROM rollout_cursors WHERE task_id = ?1",
            [task_id],
            map_rollout_cursor,
        )
        .optional()
        .map_err(StoreError::from)
}

fn query_current_unread(
    connection: &Connection,
    task_id: &str,
) -> Result<Option<UnreadSummary>, StoreError> {
    let row: Option<(i64, Option<String>, i64)> = connection
        .query_row(
            "SELECT summary.generation, summary.cache_object,
                    (SELECT COUNT(*) FROM completion_ledger completion
                     WHERE completion.task_id = summary.task_id
                       AND completion.summarized_generation = summary.generation)
             FROM summary_ledger summary
             WHERE summary.task_id = ?1 AND summary.state IN ('unheard', 'leased')
               AND summary.superseded_by IS NULL",
            [task_id],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
        )
        .optional()?;
    match row {
        Some((generation, Some(cache_object), coverage_count))
            if generation > 0 && coverage_count > 0 =>
        {
            Ok(Some(UnreadSummary {
                task_id: task_id.to_owned(),
                generation: generation as u64,
                cache_object,
                coverage_count: u32::try_from(coverage_count)
                    .map_err(|_| StoreError::InvalidSummaryState)?,
            }))
        }
        Some(_) => Err(StoreError::InvalidSummaryState),
        None => Ok(None),
    }
}

fn query_pending_summary_completions(
    connection: &Connection,
    task_id: &str,
) -> Result<Vec<PendingSummaryCompletion>, StoreError> {
    let mut statement = connection.prepare(
        "SELECT completion_id, turn_pack FROM completion_ledger
         WHERE task_id = ?1 AND summarized_generation IS NULL
         ORDER BY observed_at ASC, rowid ASC LIMIT ?2",
    )?;
    let mut rows = statement.query(params![
        task_id,
        (MAX_SUMMARY_COMPLETIONS_PER_CLAIM + 1) as i64
    ])?;
    let mut completions = Vec::new();
    let mut total_bytes = 0_usize;
    while let Some(row) = rows.next()? {
        if completions.len() == MAX_SUMMARY_COMPLETIONS_PER_CLAIM {
            break;
        }
        let completion_id: String = row.get(0)?;
        let turn_pack: String = row.get(1)?;
        if uuid::Uuid::parse_str(&completion_id).is_err()
            || turn_pack.is_empty()
            || turn_pack.len() > MAX_TURN_PACK_BYTES
        {
            return Err(StoreError::InvalidSummaryState);
        }
        let next_total = total_bytes
            .checked_add(turn_pack.len())
            .ok_or(StoreError::InvalidSummaryState)?;
        if !completions.is_empty() && next_total > MAX_SUMMARY_TURN_PACK_BYTES {
            break;
        }
        total_bytes = next_total;
        completions.push(PendingSummaryCompletion {
            completion_id,
            turn_pack,
        });
    }
    Ok(completions)
}

fn load_summary_claim(
    connection: &Connection,
    outcome: SummaryClaimOutcome,
    request_id: &str,
    task_id: &str,
    generation: i64,
    previous_generation: Option<i64>,
    covers: &str,
) -> Result<SummaryClaim, StoreError> {
    if generation <= 0 || previous_generation.is_some_and(|value| value <= 0) {
        return Err(StoreError::InvalidSummaryState);
    }
    let completion_ids: Vec<String> =
        serde_json::from_str(covers).map_err(|_| StoreError::InvalidSummaryState)?;
    if completion_ids.is_empty() || completion_ids.len() > MAX_SUMMARY_COMPLETIONS_PER_CLAIM {
        return Err(StoreError::InvalidSummaryState);
    }
    let mut completions = Vec::with_capacity(completion_ids.len());
    let mut total_bytes = 0_usize;
    for completion_id in completion_ids {
        let completion: Option<(String, Option<i64>)> = connection
            .query_row(
                "SELECT turn_pack, summarized_generation FROM completion_ledger
                 WHERE completion_id = ?1 AND task_id = ?2",
                params![completion_id, task_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?;
        let Some((turn_pack, None)) = completion else {
            return Err(StoreError::InvalidSummaryState);
        };
        total_bytes = total_bytes
            .checked_add(turn_pack.len())
            .ok_or(StoreError::InvalidSummaryState)?;
        if turn_pack.is_empty()
            || turn_pack.len() > MAX_TURN_PACK_BYTES
            || total_bytes > MAX_SUMMARY_TURN_PACK_BYTES
        {
            return Err(StoreError::InvalidSummaryState);
        }
        completions.push(PendingSummaryCompletion {
            completion_id,
            turn_pack,
        });
    }
    let previous_unread = match previous_generation {
        Some(expected) => {
            let current = query_current_unread(connection, task_id)?;
            match current {
                Some(summary) if summary.generation == expected as u64 => Some(summary),
                _ => return Err(StoreError::InvalidSummaryState),
            }
        }
        None => None,
    };
    Ok(SummaryClaim {
        outcome,
        request_id: request_id.to_owned(),
        task_id: task_id.to_owned(),
        generation: generation as u64,
        previous_unread,
        completions,
    })
}

fn validate_summary_request(task_id: &str, request_id: &str) -> Result<(), StoreError> {
    let request_valid = !request_id.is_empty()
        && request_id.len() <= MAX_REQUEST_ID_BYTES
        && !request_id.bytes().any(|byte| byte.is_ascii_control());
    if uuid::Uuid::parse_str(task_id).is_ok() && request_valid {
        Ok(())
    } else {
        Err(StoreError::InvalidSummaryRequest)
    }
}

fn validate_summary_claim(claim: &SummaryClaim) -> Result<(), StoreError> {
    validate_summary_request(&claim.task_id, &claim.request_id)?;
    if claim.generation == 0
        || claim.completions.is_empty()
        || claim.completions.len() > MAX_SUMMARY_COMPLETIONS_PER_CLAIM
        || claim
            .previous_unread
            .as_ref()
            .is_some_and(|previous| previous.task_id != claim.task_id)
    {
        return Err(StoreError::InvalidSummaryRequest);
    }
    let mut total_bytes = 0_usize;
    for completion in &claim.completions {
        total_bytes = total_bytes
            .checked_add(completion.turn_pack.len())
            .ok_or(StoreError::InvalidSummaryRequest)?;
        if uuid::Uuid::parse_str(&completion.completion_id).is_err()
            || completion.turn_pack.is_empty()
            || completion.turn_pack.len() > MAX_TURN_PACK_BYTES
            || total_bytes > MAX_SUMMARY_TURN_PACK_BYTES
        {
            return Err(StoreError::InvalidSummaryRequest);
        }
    }
    Ok(())
}

fn validate_playback_lease(lease: &SummaryPlaybackLease) -> Result<(), StoreError> {
    validate_slot(lease.slot)?;
    let cache_matches = CacheId::from_reference(&lease.cache_object)
        .ok()
        .zip(CacheId::for_task(&lease.task_id, lease.summary_generation).ok())
        .is_some_and(|(actual, expected)| actual == expected);
    if lease.lease == 0
        || lease.lease > i64::MAX as u64
        || lease.summary_generation == 0
        || lease.summary_generation > i64::MAX as u64
        || lease.request_generation == 0
        || lease.connection_generation == 0
        || uuid::Uuid::parse_str(&lease.task_id).is_err()
        || !cache_matches
    {
        return Err(StoreError::InvalidPlaybackLease);
    }
    Ok(())
}

fn commit_rollout_completion_on(
    connection: &mut Connection,
    binding: Option<&Binding>,
    expected: Option<&RolloutCursor>,
    next: &RolloutCursor,
    completion_id: &str,
    turn_pack: &str,
) -> Result<CompletionOutcome, StoreError> {
    validate_rollout_completion(expected, next, completion_id, turn_pack)?;
    let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
    if let Some(binding) = binding {
        let still_bound = transaction.query_row(
            "SELECT EXISTS(
               SELECT 1 FROM bindings
               WHERE slot = ?1 AND task_id = ?2 AND generation = ?3
             )",
            params![binding.slot, binding.task_id, to_i64(binding.generation)?],
            |row| row.get::<_, bool>(0),
        )?;
        if !still_bound {
            transaction.rollback()?;
            return Err(StoreError::BindingChanged);
        }
    }
    let current = transaction
        .query_row(
            "SELECT task_id, rollout_path, device, inode, offset, generation, anchor
             FROM rollout_cursors WHERE task_id = ?1",
            [&next.task_id],
            map_rollout_cursor,
        )
        .optional()?;
    if current.as_ref() != expected {
        transaction.rollback()?;
        return Err(StoreError::RolloutCursorChanged);
    }

    let existing_task: Option<String> = transaction
        .query_row(
            "SELECT task_id FROM completion_ledger WHERE completion_id = ?1",
            [completion_id],
            |row| row.get(0),
        )
        .optional()?;
    let outcome = match existing_task {
        Some(task_id) if task_id == next.task_id => CompletionOutcome::Replay,
        Some(_) => {
            transaction.rollback()?;
            return Err(StoreError::CompletionConflict);
        }
        None => {
            transaction.execute(
                "INSERT INTO completion_ledger
                 (completion_id, task_id, rollout_cursor, observed_at, turn_pack)
                 VALUES (?1, ?2, ?3, unixepoch(), ?4)",
                params![
                    completion_id,
                    next.task_id,
                    format!("{}:{}", next.generation, next.offset),
                    turn_pack
                ],
            )?;
            CompletionOutcome::Inserted
        }
    };
    transaction.execute(
        "INSERT INTO rollout_cursors
         (task_id, rollout_path, device, inode, offset, generation, anchor, updated_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, unixepoch())
         ON CONFLICT(task_id) DO UPDATE SET
           rollout_path = excluded.rollout_path,
           device = excluded.device,
           inode = excluded.inode,
           offset = excluded.offset,
           generation = excluded.generation,
           anchor = excluded.anchor,
           updated_at = excluded.updated_at",
        params![
            next.task_id,
            next.rollout_path.as_os_str().as_bytes(),
            to_sql_i64(next.device)?,
            to_sql_i64(next.inode)?,
            to_sql_i64(next.offset)?,
            to_sql_i64(next.generation)?,
            next.anchor.as_slice()
        ],
    )?;
    transaction.commit()?;
    Ok(outcome)
}

fn migrate(connection: &mut Connection) -> Result<(), StoreError> {
    let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
    let current: i64 = transaction.pragma_query_value(None, "user_version", |row| row.get(0))?;
    if current > SCHEMA_VERSION {
        return Err(StoreError::UnsupportedSchema(current));
    }
    if current == SCHEMA_VERSION {
        transaction.commit()?;
        return Ok(());
    }
    if current == 0 {
        transaction.execute_batch(
            "CREATE TABLE bindings (
           slot INTEGER PRIMARY KEY CHECK(slot BETWEEN 1 AND 4),
           task_id TEXT NOT NULL,
           generation INTEGER NOT NULL CHECK(generation > 0),
           updated_at INTEGER NOT NULL
         );
         CREATE TABLE jobs (
           sequence INTEGER PRIMARY KEY AUTOINCREMENT,
           request_id TEXT NOT NULL UNIQUE,
           task_id TEXT NOT NULL,
           slot INTEGER NOT NULL CHECK(slot BETWEEN 1 AND 4),
           generation INTEGER NOT NULL CHECK(generation >= 0),
           prompt TEXT NOT NULL,
           cwd BLOB NOT NULL DEFAULT X'',
           state TEXT NOT NULL CHECK(state IN ('queued', 'running', 'completed', 'failed')),
           recovery_count INTEGER NOT NULL DEFAULT 0
             CHECK(recovery_count BETWEEN 0 AND 4294967295),
           claim_generation INTEGER NOT NULL DEFAULT 0 CHECK(claim_generation >= 0),
           failure_kind TEXT,
           created_at INTEGER NOT NULL DEFAULT (unixepoch()),
           updated_at INTEGER NOT NULL DEFAULT (unixepoch())
         );
         CREATE INDEX jobs_task_fifo ON jobs(task_id, state, sequence);
         CREATE UNIQUE INDEX jobs_one_running_per_task
           ON jobs(task_id) WHERE state = 'running';
         CREATE TABLE completion_ledger (
           completion_id TEXT PRIMARY KEY,
           task_id TEXT NOT NULL,
           rollout_cursor TEXT NOT NULL,
           observed_at INTEGER NOT NULL,
           summarized_generation INTEGER,
           turn_pack TEXT NOT NULL DEFAULT '{}'
         );
         CREATE TABLE rollout_cursors (
           task_id TEXT PRIMARY KEY,
           rollout_path BLOB NOT NULL,
           device INTEGER NOT NULL CHECK(device >= 0),
           inode INTEGER NOT NULL CHECK(inode >= 0),
           offset INTEGER NOT NULL CHECK(offset >= 0),
           generation INTEGER NOT NULL CHECK(generation > 0),
           anchor BLOB NOT NULL CHECK(length(anchor) = 32),
           updated_at INTEGER NOT NULL
         );
         CREATE TABLE summary_ledger (
           task_id TEXT NOT NULL,
           generation INTEGER NOT NULL CHECK(generation > 0),
           state TEXT NOT NULL CHECK(state IN
             ('generating', 'interrupted', 'abandoned', 'unheard', 'leased', 'heard', 'superseded')),
           covers_completions TEXT NOT NULL,
           cache_object TEXT,
           request_id TEXT,
           claim_id TEXT,
           previous_generation INTEGER CHECK(previous_generation IS NULL OR previous_generation > 0),
           superseded_by INTEGER CHECK(superseded_by IS NULL OR superseded_by > generation),
           created_at INTEGER NOT NULL,
           updated_at INTEGER NOT NULL,
           CHECK((state IN ('generating', 'interrupted')
                    AND claim_id IS NOT NULL AND cache_object IS NULL)
              OR (state = 'abandoned' AND claim_id IS NULL AND cache_object IS NULL)
              OR (state IN ('unheard', 'leased', 'heard', 'superseded') AND claim_id IS NULL)),
           PRIMARY KEY(task_id, generation)
         );
         CREATE UNIQUE INDEX summary_one_active_claim_per_task
           ON summary_ledger(task_id) WHERE state IN ('generating', 'interrupted');
         CREATE UNIQUE INDEX summary_request_id_unique
           ON summary_ledger(request_id) WHERE request_id IS NOT NULL;
         CREATE UNIQUE INDEX summary_one_current_unread_per_task
           ON summary_ledger(task_id)
           WHERE state IN ('unheard', 'leased') AND superseded_by IS NULL;
         CREATE TABLE summary_tts_attempts (
           task_id TEXT NOT NULL,
           generation INTEGER NOT NULL CHECK(generation > 0),
           state TEXT NOT NULL CHECK(state IN ('started', 'ambiguous')),
           created_at INTEGER NOT NULL,
           updated_at INTEGER NOT NULL,
           PRIMARY KEY(task_id, generation),
           FOREIGN KEY(task_id, generation) REFERENCES summary_ledger(task_id, generation)
         );
         PRAGMA user_version = 1;",
        )?;
    } else {
        if !has_jobs_column(&transaction, "cwd")? {
            transaction.execute(
                "ALTER TABLE jobs ADD COLUMN cwd BLOB NOT NULL DEFAULT X''",
                [],
            )?;
        }
        if !has_jobs_column(&transaction, "failure_kind")? {
            transaction.execute("ALTER TABLE jobs ADD COLUMN failure_kind TEXT", [])?;
            transaction.execute(
                "UPDATE jobs SET state = 'failed', failure_kind = 'unsafe_working_directory',
                        updated_at = unixepoch()
                  WHERE state IN ('queued', 'running')",
                [],
            )?;
        }
        transaction.execute_batch(
            "CREATE TABLE IF NOT EXISTS completion_ledger (
               completion_id TEXT PRIMARY KEY,
               task_id TEXT NOT NULL,
               rollout_cursor TEXT NOT NULL,
               observed_at INTEGER NOT NULL,
               summarized_generation INTEGER,
               turn_pack TEXT NOT NULL DEFAULT '{}'
             );",
        )?;
        if !has_column(&transaction, "completion_ledger", "turn_pack")? {
            transaction.execute(
                "ALTER TABLE completion_ledger
                 ADD COLUMN turn_pack TEXT NOT NULL DEFAULT '{}'",
                [],
            )?;
        }
        transaction.execute_batch(
            "CREATE TABLE IF NOT EXISTS rollout_cursors (
               task_id TEXT PRIMARY KEY,
               rollout_path BLOB NOT NULL,
               device INTEGER NOT NULL CHECK(device >= 0),
               inode INTEGER NOT NULL CHECK(inode >= 0),
               offset INTEGER NOT NULL CHECK(offset >= 0),
               generation INTEGER NOT NULL CHECK(generation > 0),
               anchor BLOB NOT NULL CHECK(length(anchor) = 32),
               updated_at INTEGER NOT NULL
             );",
        )?;
        if !has_column(&transaction, "rollout_cursors", "anchor")? {
            transaction.execute(
                "ALTER TABLE rollout_cursors ADD COLUMN anchor BLOB NOT NULL
                 DEFAULT X'0000000000000000000000000000000000000000000000000000000000000000'",
                [],
            )?;
        }
        migrate_summary_ledger(&transaction)?;
        create_summary_tts_attempts(&transaction)?;
    }
    create_summary_playback_leases(&transaction)?;
    transaction.commit()?;
    Ok(())
}

fn migrate_summary_ledger(connection: &Connection) -> Result<(), StoreError> {
    if !has_table(connection, "summary_ledger")? {
        create_summary_ledger(connection)?;
        return Ok(());
    }
    let has_claim_fields = has_column(connection, "summary_ledger", "claim_id")?
        && has_column(connection, "summary_ledger", "request_id")?
        && has_column(connection, "summary_ledger", "previous_generation")?
        && has_column(connection, "summary_ledger", "superseded_by")?
        && has_column(connection, "summary_ledger", "updated_at")?;
    if !has_claim_fields {
        connection.execute_batch("ALTER TABLE summary_ledger RENAME TO summary_ledger_legacy;")?;
        create_summary_ledger(connection)?;
        connection.execute_batch(
            "INSERT INTO summary_ledger
             (task_id, generation, state, covers_completions, cache_object, request_id, claim_id,
              previous_generation, superseded_by, created_at, updated_at)
             SELECT task_id, generation, state, covers_completions, cache_object, NULL, NULL,
                    NULL, NULL, created_at, created_at
             FROM summary_ledger_legacy WHERE state != 'generating';
             DROP TABLE summary_ledger_legacy;",
        )?;
        create_summary_indexes(connection)?;
    } else if !summary_schema_supports_interrupted(connection)? {
        connection.execute_batch("ALTER TABLE summary_ledger RENAME TO summary_ledger_legacy;")?;
        create_summary_ledger(connection)?;
        connection.execute_batch(
            "INSERT INTO summary_ledger
             (task_id, generation, state, covers_completions, cache_object, request_id, claim_id,
              previous_generation, superseded_by, created_at, updated_at)
             SELECT task_id, generation,
                    CASE WHEN state = 'generating' THEN 'interrupted' ELSE state END,
                    covers_completions, cache_object, request_id,
                    CASE WHEN state = 'generating' THEN claim_id ELSE NULL END,
                    previous_generation, superseded_by, created_at, updated_at
             FROM summary_ledger_legacy
             WHERE state != 'generating' OR (request_id IS NOT NULL AND claim_id IS NOT NULL);
             DROP TABLE summary_ledger_legacy;",
        )?;
        create_summary_indexes(connection)?;
    } else {
        create_summary_indexes(connection)?;
    }
    Ok(())
}

fn create_summary_ledger(connection: &Connection) -> Result<(), StoreError> {
    connection.execute_batch(
        "CREATE TABLE summary_ledger (
           task_id TEXT NOT NULL,
           generation INTEGER NOT NULL CHECK(generation > 0),
           state TEXT NOT NULL CHECK(state IN
             ('generating', 'interrupted', 'abandoned', 'unheard', 'leased', 'heard', 'superseded')),
           covers_completions TEXT NOT NULL,
           cache_object TEXT,
           request_id TEXT,
           claim_id TEXT,
           previous_generation INTEGER CHECK(previous_generation IS NULL OR previous_generation > 0),
           superseded_by INTEGER CHECK(superseded_by IS NULL OR superseded_by > generation),
           created_at INTEGER NOT NULL,
           updated_at INTEGER NOT NULL,
           CHECK((state IN ('generating', 'interrupted')
                    AND claim_id IS NOT NULL AND cache_object IS NULL)
              OR (state = 'abandoned' AND claim_id IS NULL AND cache_object IS NULL)
              OR (state IN ('unheard', 'leased', 'heard', 'superseded') AND claim_id IS NULL)),
           PRIMARY KEY(task_id, generation)
         );",
    )?;
    create_summary_indexes(connection)
}

fn create_summary_indexes(connection: &Connection) -> Result<(), StoreError> {
    connection.execute_batch(
        "DROP INDEX IF EXISTS summary_one_generating_per_task;
         CREATE UNIQUE INDEX IF NOT EXISTS summary_one_active_claim_per_task
           ON summary_ledger(task_id) WHERE state IN ('generating', 'interrupted');
         CREATE UNIQUE INDEX IF NOT EXISTS summary_request_id_unique
           ON summary_ledger(request_id) WHERE request_id IS NOT NULL;
         CREATE UNIQUE INDEX IF NOT EXISTS summary_one_current_unread_per_task
           ON summary_ledger(task_id)
           WHERE state IN ('unheard', 'leased') AND superseded_by IS NULL;",
    )?;
    Ok(())
}

fn create_summary_tts_attempts(connection: &Connection) -> Result<(), StoreError> {
    connection.execute_batch(
        "CREATE TABLE IF NOT EXISTS summary_tts_attempts (
           task_id TEXT NOT NULL,
           generation INTEGER NOT NULL CHECK(generation > 0),
           state TEXT NOT NULL CHECK(state IN ('started', 'ambiguous')),
           created_at INTEGER NOT NULL,
           updated_at INTEGER NOT NULL,
           PRIMARY KEY(task_id, generation),
           FOREIGN KEY(task_id, generation) REFERENCES summary_ledger(task_id, generation)
         );",
    )?;
    Ok(())
}

fn create_summary_playback_leases(connection: &Connection) -> Result<(), StoreError> {
    connection.execute_batch(
        "CREATE TABLE IF NOT EXISTS summary_playback_leases (
           lease_id INTEGER PRIMARY KEY CHECK(lease_id > 0),
           task_id TEXT NOT NULL,
           summary_generation INTEGER NOT NULL CHECK(summary_generation > 0),
           slot INTEGER NOT NULL CHECK(slot BETWEEN 1 AND 4),
           request_generation INTEGER NOT NULL CHECK(request_generation > 0),
           connection_generation INTEGER NOT NULL CHECK(connection_generation > 0),
           created_at INTEGER NOT NULL,
           updated_at INTEGER NOT NULL,
           UNIQUE(task_id, summary_generation),
           FOREIGN KEY(task_id, summary_generation)
             REFERENCES summary_ledger(task_id, generation)
         );",
    )?;
    Ok(())
}

fn has_jobs_column(connection: &Connection, expected: &str) -> Result<bool, StoreError> {
    has_column(connection, "jobs", expected)
}

fn has_column(connection: &Connection, table: &str, expected: &str) -> Result<bool, StoreError> {
    let mut statement = connection.prepare(&format!("PRAGMA table_info({table})"))?;
    let mut rows = statement.query([])?;
    while let Some(row) = rows.next()? {
        if row.get::<_, String>(1)? == expected {
            return Ok(true);
        }
    }
    Ok(false)
}

fn has_table(connection: &Connection, expected: &str) -> Result<bool, StoreError> {
    Ok(connection
        .query_row(
            "SELECT 1 FROM sqlite_schema WHERE type = 'table' AND name = ?1",
            [expected],
            |_| Ok(()),
        )
        .optional()?
        .is_some())
}

fn summary_schema_supports_interrupted(connection: &Connection) -> Result<bool, StoreError> {
    let sql: Option<String> = connection
        .query_row(
            "SELECT sql FROM sqlite_schema WHERE type = 'table' AND name = 'summary_ledger'",
            [],
            |row| row.get(0),
        )
        .optional()?;
    Ok(sql.is_some_and(|value| value.contains("'interrupted'")))
}

fn ensure_supported_schema(connection: &Connection) -> Result<(), StoreError> {
    let current: i64 = connection.pragma_query_value(None, "user_version", |row| row.get(0))?;
    if current > SCHEMA_VERSION {
        Err(StoreError::UnsupportedSchema(current))
    } else {
        Ok(())
    }
}

fn recover_interrupted_jobs(connection: &mut Connection) -> Result<u64, StoreError> {
    let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
    let recovered = transaction.execute(
        "UPDATE jobs SET
           state = 'queued',
           recovery_count = recovery_count + 1,
           updated_at = unixepoch()
         WHERE state = 'running'",
        [],
    )? as u64;
    transaction.commit()?;
    Ok(recovered)
}

fn recover_interrupted_summaries(connection: &mut Connection) -> Result<u64, StoreError> {
    let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
    let recovered = transaction.execute(
        "UPDATE summary_ledger SET state = 'interrupted', updated_at = unixepoch()
         WHERE state = 'generating'",
        [],
    )? as u64;
    transaction.commit()?;
    Ok(recovered)
}

fn recover_interrupted_playback_leases(connection: &mut Connection) -> Result<(), StoreError> {
    let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
    transaction.execute(
        "UPDATE summary_ledger
         SET state = CASE WHEN superseded_by IS NULL THEN 'unheard' ELSE 'superseded' END,
             updated_at = unixepoch()
         WHERE state = 'leased'",
        [],
    )?;
    transaction.execute("DELETE FROM summary_playback_leases", [])?;
    transaction.commit()?;
    Ok(())
}

fn map_job(row: &rusqlite::Row<'_>) -> rusqlite::Result<Job> {
    use std::os::unix::ffi::OsStringExt;

    Ok(Job {
        request_id: row.get(0)?,
        task_id: row.get(1)?,
        slot: row.get(2)?,
        generation: row.get::<_, i64>(3)? as u64,
        prompt: row.get(4)?,
        cwd: PathBuf::from(std::ffi::OsString::from_vec(row.get(5)?)),
        recovery_count: row.get(6)?,
        claim_generation: row.get::<_, i64>(7)? as u64,
    })
}

fn map_rollout_cursor(row: &rusqlite::Row<'_>) -> rusqlite::Result<RolloutCursor> {
    use std::os::unix::ffi::OsStringExt;

    let anchor = row
        .get::<_, Vec<u8>>(6)?
        .try_into()
        .map_err(|_value: Vec<u8>| {
            rusqlite::Error::FromSqlConversionFailure(
                6,
                rusqlite::types::Type::Blob,
                Box::new(std::io::Error::new(
                    std::io::ErrorKind::InvalidData,
                    "rollout anchor must contain 32 bytes",
                )),
            )
        })?;
    Ok(RolloutCursor {
        task_id: row.get(0)?,
        rollout_path: PathBuf::from(std::ffi::OsString::from_vec(row.get(1)?)),
        device: row.get::<_, i64>(2)? as u64,
        inode: row.get::<_, i64>(3)? as u64,
        offset: row.get::<_, i64>(4)? as u64,
        generation: row.get::<_, i64>(5)? as u64,
        anchor,
    })
}

fn validate_rollout_completion(
    expected: Option<&RolloutCursor>,
    next: &RolloutCursor,
    completion_id: &str,
    turn_pack: &str,
) -> Result<(), StoreError> {
    let path = next.rollout_path.as_os_str().as_bytes();
    let base_valid = uuid::Uuid::parse_str(&next.task_id).is_ok()
        && uuid::Uuid::parse_str(completion_id).is_ok()
        && next.rollout_path.is_absolute()
        && !path.is_empty()
        && path.len() <= MAX_ROLLOUT_PATH_BYTES
        && !path.contains(&0)
        && next.generation > 0
        && next.anchor != [0; 32]
        && !turn_pack.is_empty()
        && turn_pack.len() <= MAX_TURN_PACK_BYTES
        && serde_json::from_str::<serde_json::Value>(turn_pack)
            .is_ok_and(|value| value.is_object());
    let transition_valid = match expected {
        None => next.generation == 1,
        Some(previous) => {
            previous.task_id == next.task_id
                && ((previous.rollout_path == next.rollout_path
                    && previous.device == next.device
                    && previous.inode == next.inode
                    && previous.generation == next.generation
                    && next.offset >= previous.offset)
                    || (next.generation == previous.generation.saturating_add(1)))
        }
    };
    if base_valid && transition_valid {
        Ok(())
    } else {
        Err(StoreError::InvalidCompletion)
    }
}

fn validate_slot(slot: u8) -> Result<(), StoreError> {
    if (1..=4).contains(&slot) {
        Ok(())
    } else {
        Err(StoreError::InvalidSlot)
    }
}

fn validate_job(job: &NewJob<'_>) -> Result<(), StoreError> {
    let request_ok = !job.request_id.is_empty()
        && job.request_id.len() <= MAX_REQUEST_ID_BYTES
        && !job.request_id.bytes().any(|byte| byte.is_ascii_control());
    let task_ok = !job.task_id.is_empty()
        && job.task_id.len() <= MAX_TASK_ID_BYTES
        && !job.task_id.bytes().any(|byte| byte.is_ascii_control());
    let prompt_ok = !job.prompt.trim().is_empty() && job.prompt.len() <= MAX_PROMPT_BYTES;
    let cwd = job.cwd.as_os_str().as_bytes();
    let cwd_ok =
        job.cwd.is_absolute() && !cwd.is_empty() && cwd.len() <= MAX_CWD_BYTES && !cwd.contains(&0);
    if request_ok && task_ok && prompt_ok && cwd_ok {
        Ok(())
    } else {
        Err(StoreError::InvalidJob)
    }
}

fn to_i64(value: u64) -> Result<i64, StoreError> {
    i64::try_from(value).map_err(|_| StoreError::GenerationOutOfRange)
}

fn to_sql_i64(value: u64) -> Result<i64, StoreError> {
    i64::try_from(value).map_err(|_| StoreError::InvalidCompletion)
}

fn lock_path(database: &Path) -> PathBuf {
    let mut name = database.as_os_str().to_owned();
    name.push(".lock");
    PathBuf::from(name)
}

fn sidecar_path(database: &Path, suffix: &str) -> PathBuf {
    let mut name = database.as_os_str().to_owned();
    name.push(suffix);
    PathBuf::from(name)
}

fn secure_existing_sidecars(database: &Path) -> Result<Vec<(PathBuf, File)>, StoreError> {
    let mut guards = Vec::new();
    for suffix in ["-journal", "-wal", "-shm"] {
        let path = sidecar_path(database, suffix);
        match fs::symlink_metadata(&path) {
            Ok(_) => {
                let guard = open_private_file(&path)?;
                ensure_file_identity(&guard, &path)?;
                guards.push((path, guard));
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(StoreError::Io(error)),
        }
    }
    Ok(guards)
}

fn ensure_guarded_sidecars(guards: &[(PathBuf, File)]) -> Result<(), StoreError> {
    for (path, guard) in guards {
        match fs::symlink_metadata(path) {
            Ok(_) => ensure_file_identity(guard, path)?,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(StoreError::Io(error)),
        }
    }
    Ok(())
}

fn ensure_file_identity(file: &File, path: &Path) -> Result<(), StoreError> {
    let opened = file.metadata()?;
    let current = fs::symlink_metadata(path)?;
    if current.file_type().is_symlink()
        || opened.dev() != current.dev()
        || opened.ino() != current.ino()
    {
        return Err(StoreError::StatePathChanged);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io::Write;
    use std::os::unix::fs::PermissionsExt;
    use std::os::unix::process::ExitStatusExt;
    use std::process::{Command, Stdio};
    use std::time::Instant;

    use super::*;
    use tempfile::tempdir;

    fn job<'a>(request_id: &'a str, task_id: &'a str, prompt: &'a str) -> NewJob<'a> {
        NewJob {
            request_id,
            task_id,
            slot: 1,
            generation: 1,
            prompt,
            cwd: Path::new("/work"),
        }
    }

    fn insert_summary_completion(
        store: &StateStore,
        task_id: &str,
        completion_id: &str,
        turn_pack: &str,
    ) {
        store
            .connection
            .execute(
                "INSERT INTO completion_ledger
                 (completion_id, task_id, rollout_cursor, observed_at, turn_pack)
                 VALUES (?1, ?2, 'fixture', unixepoch(), ?3)",
                params![completion_id, task_id, turn_pack],
            )
            .unwrap();
    }

    fn claimed(result: Option<SummaryClaimResult>) -> SummaryClaim {
        match result.unwrap() {
            SummaryClaimResult::Claimed(claim) => claim,
            SummaryClaimResult::Published { .. } => panic!("expected active claim"),
        }
    }

    #[test]
    fn migration_is_idempotent_and_database_is_private() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("app").join("state.sqlite3");

        let store = StateStore::open(&path).unwrap();
        assert_eq!(store.schema_version().unwrap(), SCHEMA_VERSION);
        drop(store);
        let reopened = StateStore::open(&path).unwrap();

        assert_eq!(reopened.schema_version().unwrap(), SCHEMA_VERSION);
        assert_eq!(
            fs::metadata(path).unwrap().permissions().mode() & 0o777,
            0o600
        );
        assert_eq!(
            fs::metadata(temp.path().join("app"))
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o700
        );
    }

    #[test]
    fn version_one_jobs_are_failed_closed_because_they_have_no_catalog_cwd_snapshot() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let connection = Connection::open(&path).unwrap();
        connection
            .execute_batch(
                "CREATE TABLE jobs (
                   sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                   request_id TEXT NOT NULL UNIQUE,
                   task_id TEXT NOT NULL,
                   slot INTEGER NOT NULL,
                   generation INTEGER NOT NULL,
                   prompt TEXT NOT NULL,
                   state TEXT NOT NULL,
                   recovery_count INTEGER NOT NULL DEFAULT 0,
                   claim_generation INTEGER NOT NULL DEFAULT 0,
                   created_at INTEGER NOT NULL DEFAULT (unixepoch()),
                   updated_at INTEGER NOT NULL DEFAULT (unixepoch())
                 );
                 INSERT INTO jobs (request_id, task_id, slot, generation, prompt, state)
                   VALUES ('legacy', 'task-a', 1, 1, 'private', 'running');
                 PRAGMA user_version = 1;",
            )
            .unwrap();
        drop(connection);

        let store = StateStore::open(&path).unwrap();
        assert_eq!(store.schema_version().unwrap(), SCHEMA_VERSION);
        let rollback_compatible_version: i64 = store
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(rollback_compatible_version, 1);
        assert_eq!(store.recovered_jobs_on_open(), 0);
        let state: (String, String) = store
            .connection
            .query_row(
                "SELECT state, failure_kind FROM jobs WHERE request_id = 'legacy'",
                [],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .unwrap();
        assert_eq!(state, ("failed".into(), "unsafe_working_directory".into()));
        store
            .connection
            .execute(
                "INSERT INTO jobs
                 (request_id, task_id, slot, generation, prompt, state)
                 VALUES ('old-binary-write', 'task-b', 1, 1, 'prompt', 'queued')",
                [],
            )
            .unwrap();
    }

    #[test]
    fn task_capacity_is_transactional_and_exact_replay_stays_idempotent() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let mut store = StateStore::open(&path).unwrap();
        for index in 0..MAX_PENDING_JOBS_PER_TASK {
            store
                .enqueue(&job(&format!("r{index}"), "task-a", "prompt"))
                .unwrap();
        }
        assert_eq!(store.pending_count("task-a").unwrap(), 12);
        assert_eq!(
            store.enqueue(&job("r0", "task-a", "prompt")).unwrap(),
            EnqueueOutcome::Replay
        );
        assert!(matches!(
            store.enqueue(&job("overflow", "task-a", "prompt")),
            Err(StoreError::TaskQueueFull)
        ));

        let claimed = store.claim_next("task-a").unwrap().unwrap();
        assert!(
            store
                .mark_completed(&claimed.request_id, claimed.claim_generation)
                .unwrap()
        );
        assert_eq!(
            store
                .enqueue(&job("after-complete", "task-a", "prompt"))
                .unwrap(),
            EnqueueOutcome::Inserted
        );
    }

    #[test]
    fn global_claim_limit_and_task_heads_enforce_fifo() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let mut store = StateStore::open(&path).unwrap();
        store.enqueue(&job("a1", "task-a", "1")).unwrap();
        store.enqueue(&job("a2", "task-a", "2")).unwrap();
        store.enqueue(&job("b1", "task-b", "3")).unwrap();
        store.enqueue(&job("c1", "task-c", "4")).unwrap();

        let first = store.claim_next_runnable().unwrap().unwrap();
        let second = store.claim_next_runnable().unwrap().unwrap();
        assert_eq!(first.request_id, "a1");
        assert_eq!(second.request_id, "b1");
        assert!(store.claim_next_runnable().unwrap().is_none());
        assert!(
            store
                .mark_completed(&first.request_id, first.claim_generation)
                .unwrap()
        );
        assert_eq!(
            store.claim_next_runnable().unwrap().unwrap().request_id,
            "a2"
        );
    }

    #[test]
    fn one_hundred_jobs_preserve_fifo_in_bounded_batches() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let mut store = StateStore::open(&path).unwrap();
        let mut next_enqueue = 0_u32;
        let mut next_claim = 0_u32;
        while next_claim < 100 {
            while next_enqueue < 100 && store.pending_count("task-a").unwrap() < 12 {
                let id = format!("r{next_enqueue:03}");
                store.enqueue(&job(&id, "task-a", &id)).unwrap();
                next_enqueue += 1;
            }
            let claimed = store.claim_next("task-a").unwrap().unwrap();
            assert_eq!(claimed.request_id, format!("r{next_claim:03}"));
            assert!(
                store
                    .mark_completed(&claimed.request_id, claimed.claim_generation)
                    .unwrap()
            );
            next_claim += 1;
        }
    }

    #[test]
    fn live_rollback_journal_inherits_private_database_permissions() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let mut store = StateStore::open(&path).unwrap();
        let transaction = store
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .unwrap();
        transaction
            .execute(
                "INSERT INTO jobs
                 (request_id, task_id, slot, generation, prompt, cwd, state)
                 VALUES ('journal-test', 'task-a', 1, 1, 'private', X'2f776f726b', 'queued')",
                [],
            )
            .unwrap();

        let journal = sidecar_path(&path, "-journal");
        assert_eq!(
            fs::metadata(journal).unwrap().permissions().mode() & 0o777,
            0o600
        );
        transaction.rollback().unwrap();
    }

    #[test]
    fn guarded_file_detects_path_replacement() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let guard = open_private_file(&path).unwrap();
        let replacement = temp.path().join("replacement.sqlite3");
        fs::write(&replacement, b"replacement").unwrap();
        fs::rename(&replacement, &path).unwrap();

        assert!(matches!(
            ensure_file_identity(&guard, &path),
            Err(StoreError::StatePathChanged)
        ));
    }

    #[test]
    fn future_schema_is_rejected_without_changing_journal_mode() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        {
            let connection = Connection::open(&path).unwrap();
            connection
                .pragma_update(None, "journal_mode", "WAL")
                .unwrap();
            connection
                .pragma_update(None, "user_version", SCHEMA_VERSION + 1)
                .unwrap();
        }

        assert!(matches!(
            StateStore::open(&path),
            Err(StoreError::UnsupportedSchema(version)) if version == SCHEMA_VERSION + 1
        ));
        let connection = Connection::open(&path).unwrap();
        let journal_mode: String = connection
            .pragma_query_value(None, "journal_mode", |row| row.get(0))
            .unwrap();
        assert_eq!(journal_mode.to_ascii_lowercase(), "wal");
    }

    #[test]
    fn duplicate_request_is_idempotent_and_fifo_is_per_task() {
        let temp = tempdir().unwrap();
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        assert_eq!(
            store.enqueue(&job("r1", "task-a", "first")).unwrap(),
            EnqueueOutcome::Inserted
        );
        assert_eq!(
            store.enqueue(&job("r1", "task-a", "first")).unwrap(),
            EnqueueOutcome::Replay
        );
        assert!(matches!(
            store.enqueue(&job("r1", "task-a", "different")),
            Err(StoreError::RequestIdConflict)
        ));
        assert_eq!(
            store.enqueue(&job("r2", "task-b", "other")).unwrap(),
            EnqueueOutcome::Inserted
        );
        assert_eq!(
            store.enqueue(&job("r3", "task-a", "second")).unwrap(),
            EnqueueOutcome::Inserted
        );

        let first = store.claim_next("task-a").unwrap().unwrap();
        assert_eq!(first.request_id, "r1");
        assert!(store.claim_next("task-a").unwrap().is_none());
        assert!(
            !store
                .mark_completed("r1", first.claim_generation + 1)
                .unwrap()
        );
        assert!(store.mark_completed("r1", first.claim_generation).unwrap());
        assert_eq!(
            store.claim_next("task-a").unwrap().unwrap().request_id,
            "r3"
        );
        assert_eq!(
            store.claim_next("task-b").unwrap().unwrap().request_id,
            "r2"
        );
        assert!(store.claim_next("missing").unwrap().is_none());
    }

    #[test]
    fn binding_compare_and_swap_never_overwrites_on_stale_generation() {
        let temp = tempdir().unwrap();
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        let first = store.set_binding(2, None, "task-a").unwrap().unwrap();
        assert_eq!(first.generation, 1);

        assert!(store.set_binding(2, None, "task-stale").unwrap().is_none());
        let second = store
            .set_binding(2, Some(first.generation), "task-b")
            .unwrap()
            .unwrap();

        assert_eq!(second.generation, 2);
        assert_eq!(store.binding(2).unwrap(), Some(second));
    }

    #[test]
    fn binding_and_claim_generations_fail_instead_of_saturating() {
        let temp = tempdir().unwrap();
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        store.set_binding(1, None, "task-a").unwrap();
        store
            .connection
            .execute(
                "UPDATE bindings SET generation = ?1 WHERE slot = 1",
                [i64::MAX],
            )
            .unwrap();
        assert!(matches!(
            store.set_binding(1, Some(i64::MAX as u64), "task-b"),
            Err(StoreError::GenerationOutOfRange)
        ));
        assert_eq!(store.binding(1).unwrap().unwrap().task_id, "task-a");

        store.enqueue(&job("r1", "task-a", "prompt")).unwrap();
        store
            .connection
            .execute(
                "UPDATE jobs SET claim_generation = ?1 WHERE request_id = 'r1'",
                [i64::MAX],
            )
            .unwrap();
        assert!(matches!(
            store.claim_next("task-a"),
            Err(StoreError::ClaimGenerationOutOfRange)
        ));
        assert_eq!(store.job_state("r1").unwrap(), ("queued".into(), 0));
    }

    #[test]
    fn running_job_recovers_exactly_once_per_interrupted_claim() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let mut store = StateStore::open(&path).unwrap();
        store.enqueue(&job("r1", "task-a", "prompt")).unwrap();
        store.claim_next("task-a").unwrap().unwrap();
        drop(store);

        let mut reopened = StateStore::open(&path).unwrap();
        assert_eq!(reopened.recovered_jobs_on_open(), 1);
        assert_eq!(reopened.job_state("r1").unwrap(), ("queued".into(), 1));
        let claimed = reopened.claim_next("task-a").unwrap().unwrap();
        assert!(
            reopened
                .mark_completed("r1", claimed.claim_generation)
                .unwrap()
        );
        drop(reopened);

        let final_store = StateStore::open(&path).unwrap();
        assert_eq!(final_store.recovered_jobs_on_open(), 0);
        assert_eq!(
            final_store.job_state("r1").unwrap(),
            ("completed".into(), 1)
        );
    }

    #[test]
    fn one_hundred_reopen_cycles_preserve_one_job_and_monotonic_recovery_count() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let mut store = StateStore::open(&path).unwrap();
        store.enqueue(&job("r1", "task-a", "prompt")).unwrap();
        drop(store);

        for expected_recovery in 0..100 {
            let mut reopened = StateStore::open(&path).unwrap();
            assert_eq!(
                reopened.recovered_jobs_on_open(),
                u64::from(expected_recovery > 0)
            );
            let claimed = reopened.claim_next("task-a").unwrap().unwrap();
            assert_eq!(claimed.request_id, "r1");
            assert_eq!(claimed.recovery_count, expected_recovery);
        }

        let final_store = StateStore::open(&path).unwrap();
        assert_eq!(final_store.recovered_jobs_on_open(), 1);
        assert_eq!(final_store.job_state("r1").unwrap(), ("queued".into(), 100));
    }

    fn publish_kill_ready_and_wait(path: &Path) -> ! {
        let mut ready = File::create(path).unwrap();
        ready.write_all(b"ready").unwrap();
        ready.sync_all().unwrap();
        loop {
            std::thread::park();
        }
    }

    #[test]
    fn sqlite_kill_subprocess_helper() {
        let Some(path) = std::env::var_os("ECI_STORE_KILL_PATH") else {
            return;
        };
        let ready = PathBuf::from(std::env::var_os("ECI_STORE_KILL_READY").unwrap());
        let stage = std::env::var("ECI_STORE_KILL_STAGE").unwrap();
        let mut store = StateStore::open(&PathBuf::from(path)).unwrap();

        match stage.as_str() {
            "enqueue_before_commit" => {
                let transaction = store
                    .connection
                    .transaction_with_behavior(TransactionBehavior::Immediate)
                    .unwrap();
                assert_eq!(
                    transaction
                        .execute(
                            "INSERT INTO jobs
                             (request_id, task_id, slot, generation, prompt, cwd, state)
                             VALUES ('candidate', 'task-a', 1, 1, 'candidate', X'2f776f726b', 'queued')",
                            [],
                        )
                        .unwrap(),
                    1
                );
                publish_kill_ready_and_wait(&ready);
            }
            "enqueue_after_commit" => {
                assert!(matches!(
                    store.enqueue(&job("candidate", "task-a", "candidate")),
                    Ok(EnqueueOutcome::Inserted)
                ));
                publish_kill_ready_and_wait(&ready);
            }
            "claim_before_commit" => {
                let transaction = store
                    .connection
                    .transaction_with_behavior(TransactionBehavior::Immediate)
                    .unwrap();
                assert_eq!(
                    transaction
                        .execute(
                            "UPDATE jobs SET state = 'running', claim_generation = 1
                             WHERE request_id = 'base' AND state = 'queued'",
                            [],
                        )
                        .unwrap(),
                    1
                );
                publish_kill_ready_and_wait(&ready);
            }
            "claim_after_commit" => {
                assert_eq!(
                    store.claim_next("task-a").unwrap().unwrap().request_id,
                    "base"
                );
                publish_kill_ready_and_wait(&ready);
            }
            _ => panic!("unknown SQLite kill stage"),
        }
    }

    #[test]
    fn summary_kill_subprocess_helper() {
        let Some(path) = std::env::var_os("ECI_SUMMARY_KILL_PATH") else {
            return;
        };
        let ready = PathBuf::from(std::env::var_os("ECI_SUMMARY_KILL_READY").unwrap());
        let stage = std::env::var("ECI_SUMMARY_KILL_STAGE").unwrap();
        let mut store = StateStore::open(&PathBuf::from(path)).unwrap();
        match stage.as_str() {
            "claim_before_commit" => {
                let transaction = store
                    .connection
                    .transaction_with_behavior(TransactionBehavior::Immediate)
                    .unwrap();
                transaction
                    .execute(
                        "INSERT INTO summary_ledger
                         (task_id, generation, state, covers_completions, cache_object,
                          request_id, claim_id, previous_generation, superseded_by,
                          created_at, updated_at)
                         VALUES (?1, 1, 'generating', ?2, NULL, 'kill-request',
                                 'kill-request', NULL, NULL, unixepoch(), unixepoch())",
                        params![
                            "019fa972-5cfa-75e1-9008-0b17ade9a347",
                            r#"["019fa972-5cfa-75e1-9008-0b17ade9a348"]"#
                        ],
                    )
                    .unwrap();
                publish_kill_ready_and_wait(&ready);
            }
            "claim_after_commit" => {
                assert!(matches!(
                    store
                        .claim_summary("019fa972-5cfa-75e1-9008-0b17ade9a347", "kill-request")
                        .unwrap(),
                    Some(SummaryClaimResult::Claimed(_))
                ));
                publish_kill_ready_and_wait(&ready);
            }
            _ => panic!("unknown summary kill stage"),
        }
    }

    #[test]
    fn summary_claim_sigkill_boundaries_preserve_pending_and_recover_generation() {
        const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const TURN: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        for stage in ["claim_before_commit", "claim_after_commit"] {
            let temp = tempdir().unwrap();
            let path = temp.path().join("state.sqlite3");
            let ready = temp.path().join("summary-kill-ready");
            let initial = StateStore::open(&path).unwrap();
            insert_summary_completion(&initial, TASK, TURN, r#"{"turn":1}"#);
            drop(initial);

            let mut child = Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "store::tests::summary_kill_subprocess_helper"])
                .env("ECI_SUMMARY_KILL_PATH", &path)
                .env("ECI_SUMMARY_KILL_READY", &ready)
                .env("ECI_SUMMARY_KILL_STAGE", stage)
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .spawn()
                .unwrap();
            let deadline = Instant::now() + std::time::Duration::from_secs(5);
            while !ready.exists() {
                if let Some(status) = child.try_wait().unwrap() {
                    panic!("summary kill helper exited before checkpoint: {status}");
                }
                if Instant::now() >= deadline {
                    child.kill().unwrap();
                    panic!("summary kill helper did not reach checkpoint");
                }
                std::thread::sleep(std::time::Duration::from_millis(5));
            }
            assert_eq!(unsafe { libc::kill(child.id() as i32, libc::SIGKILL) }, 0);
            assert_eq!(child.wait().unwrap().signal(), Some(libc::SIGKILL));

            let mut reopened = StateStore::open(&path).unwrap();
            assert_eq!(reopened.pending_summary_completion_count(TASK).unwrap(), 1);
            assert_eq!(
                reopened.recovered_summaries_on_open(),
                u64::from(stage == "claim_after_commit")
            );
            if stage == "claim_after_commit" {
                insert_summary_completion(
                    &reopened,
                    TASK,
                    "019fa972-5cfa-75e1-9008-0b17ade9a349",
                    r#"{"turn":2}"#,
                );
            }
            let claim = if stage == "claim_after_commit" {
                assert!(matches!(
                    reopened.claim_summary(TASK, "unknown-after-restart"),
                    Err(StoreError::SummaryBusy)
                ));
                reopened.resume_interrupted_summary(TASK).unwrap().unwrap()
            } else {
                assert!(reopened.resume_interrupted_summary(TASK).unwrap().is_none());
                claimed(reopened.claim_summary(TASK, "kill-request").unwrap())
            };
            assert_eq!(claim.generation, 1);
            assert_eq!(
                claim.outcome,
                if stage == "claim_after_commit" {
                    SummaryClaimOutcome::Replay
                } else {
                    SummaryClaimOutcome::Inserted
                }
            );
            assert_eq!(claim.completions.len(), 1);
            assert_eq!(claim.completions[0].completion_id, TURN);
        }
    }

    #[test]
    fn one_hundred_sigkill_cycles_preserve_sqlite_commit_boundaries_and_fifo() {
        let stages = [
            "enqueue_before_commit",
            "enqueue_after_commit",
            "claim_before_commit",
            "claim_after_commit",
        ];

        for cycle in 0..100 {
            let temp = tempdir().unwrap();
            let path = temp.path().join("state.sqlite3");
            let ready = temp.path().join("kill-ready");
            let stage = stages[cycle % stages.len()];
            let mut initial = StateStore::open(&path).unwrap();
            initial.enqueue(&job("base", "task-a", "base")).unwrap();
            drop(initial);

            let mut child = Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "store::tests::sqlite_kill_subprocess_helper"])
                .env("ECI_STORE_KILL_PATH", &path)
                .env("ECI_STORE_KILL_READY", &ready)
                .env("ECI_STORE_KILL_STAGE", stage)
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .spawn()
                .unwrap();
            let deadline = Instant::now() + std::time::Duration::from_secs(5);
            while !ready.exists() {
                if let Some(status) = child.try_wait().unwrap() {
                    panic!("SQLite kill helper exited before checkpoint: {status}");
                }
                if Instant::now() >= deadline {
                    child.kill().unwrap();
                    panic!("SQLite kill helper did not reach checkpoint");
                }
                std::thread::sleep(std::time::Duration::from_millis(5));
            }
            assert_eq!(unsafe { libc::kill(child.id() as i32, libc::SIGKILL) }, 0);
            let status = child.wait().unwrap();
            assert_eq!(status.signal(), Some(libc::SIGKILL));

            let reopened = StateStore::open(&path).unwrap();
            let job_count: i64 = reopened
                .connection
                .query_row("SELECT COUNT(*) FROM jobs", [], |row| row.get(0))
                .unwrap();
            let candidate_count: i64 = reopened
                .connection
                .query_row(
                    "SELECT COUNT(*) FROM jobs WHERE request_id = 'candidate'",
                    [],
                    |row| row.get(0),
                )
                .unwrap();

            match stage {
                "enqueue_before_commit" => {
                    assert_eq!(job_count, 1);
                    assert_eq!(candidate_count, 0);
                    assert_eq!(reopened.job_state("base").unwrap(), ("queued".into(), 0));
                }
                "enqueue_after_commit" => {
                    assert_eq!(job_count, 2);
                    assert_eq!(candidate_count, 1);
                    assert_eq!(
                        reopened.job_state("candidate").unwrap(),
                        ("queued".into(), 0)
                    );
                }
                "claim_before_commit" => {
                    assert_eq!(job_count, 1);
                    assert_eq!(reopened.job_state("base").unwrap(), ("queued".into(), 0));
                }
                "claim_after_commit" => {
                    assert_eq!(job_count, 1);
                    assert_eq!(reopened.recovered_jobs_on_open(), 1);
                    assert_eq!(reopened.job_state("base").unwrap(), ("queued".into(), 1));
                }
                _ => unreachable!(),
            }
        }
    }

    #[test]
    fn second_store_cannot_recover_live_process_jobs() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let mut owner = StateStore::open(&path).unwrap();
        owner.enqueue(&job("r1", "task-a", "prompt")).unwrap();
        owner.claim_next("task-a").unwrap().unwrap();

        assert!(matches!(
            StateStore::open(&path),
            Err(StoreError::AlreadyRunning)
        ));
        assert_eq!(owner.job_state("r1").unwrap(), ("running".into(), 0));
    }

    #[test]
    fn instance_lock_is_released_even_while_a_duplicate_descriptor_survives() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let owner = StateStore::open(&path).unwrap();
        let inherited_descriptor = owner._instance_lock.file().try_clone().unwrap();

        drop(owner);
        let reopened = StateStore::open(&path).unwrap();

        drop(reopened);
        drop(inherited_descriptor);
    }

    #[test]
    fn completion_and_cursor_commit_atomically_with_replay_and_stale_cas() {
        const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const TURN: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let rollout = temp.path().join("rollout.jsonl");
        let mut store = StateStore::open(&path).unwrap();
        let first = RolloutCursor {
            task_id: TASK.into(),
            rollout_path: rollout.clone(),
            device: 1,
            inode: 2,
            offset: 100,
            generation: 1,
            anchor: [1; 32],
        };

        assert_eq!(
            store
                .commit_rollout_completion(None, &first, TURN, r#"{"v":1}"#)
                .unwrap(),
            CompletionOutcome::Inserted
        );
        let second = RolloutCursor {
            offset: 200,
            ..first.clone()
        };
        assert_eq!(
            store
                .commit_rollout_completion(Some(&first), &second, TURN, r#"{"v":1}"#)
                .unwrap(),
            CompletionOutcome::Replay
        );
        assert_eq!(store.completion_count(TASK).unwrap(), 1);
        assert_eq!(store.rollout_cursor(TASK).unwrap(), Some(second.clone()));

        let stale_next = RolloutCursor {
            offset: 300,
            ..second.clone()
        };
        assert!(matches!(
            store.commit_rollout_completion(Some(&first), &stale_next, TURN, r#"{"v":1}"#),
            Err(StoreError::RolloutCursorChanged)
        ));
        assert_eq!(store.rollout_cursor(TASK).unwrap(), Some(second));
    }

    #[test]
    fn cursor_write_failure_rolls_back_the_completion_ledger_insert() {
        const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const TURN: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        let temp = tempdir().unwrap();
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER reject_cursor BEFORE INSERT ON rollout_cursors
                 BEGIN SELECT RAISE(ABORT, 'injected cursor failure'); END;",
            )
            .unwrap();
        let cursor = RolloutCursor {
            task_id: TASK.into(),
            rollout_path: temp.path().join("rollout.jsonl"),
            device: 1,
            inode: 2,
            offset: 100,
            generation: 1,
            anchor: [1; 32],
        };

        assert!(
            store
                .commit_rollout_completion(None, &cursor, TURN, r#"{"v":1}"#)
                .is_err()
        );
        assert_eq!(store.completion_count(TASK).unwrap(), 0);
        assert!(store.rollout_cursor(TASK).unwrap().is_none());
    }

    #[test]
    fn completion_id_collision_across_tasks_fails_without_creating_a_cursor() {
        const TASK_A: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const TASK_B: &str = "019fa972-5cfa-75e1-9008-0b17ade9a349";
        const TURN: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        let temp = tempdir().unwrap();
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        let cursor = |task_id: &str| RolloutCursor {
            task_id: task_id.into(),
            rollout_path: temp.path().join(format!("{task_id}.jsonl")),
            device: 1,
            inode: if task_id == TASK_A { 2 } else { 3 },
            offset: 100,
            generation: 1,
            anchor: [1; 32],
        };
        store
            .commit_rollout_completion(None, &cursor(TASK_A), TURN, r#"{"v":1}"#)
            .unwrap();

        assert!(matches!(
            store.commit_rollout_completion(None, &cursor(TASK_B), TURN, r#"{"v":1}"#),
            Err(StoreError::CompletionConflict)
        ));
        assert!(store.rollout_cursor(TASK_B).unwrap().is_none());
        assert_eq!(store.completion_count(TASK_A).unwrap(), 1);
    }

    #[test]
    fn observer_connection_shares_state_without_sharing_the_process_owner_lock() {
        const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const TURN: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let mut store = StateStore::open(&path).unwrap();
        let mut observer = store.open_observer_store().unwrap();
        store.set_binding(1, None, TASK).unwrap();
        assert_eq!(observer.bindings().unwrap(), store.bindings().unwrap());
        assert!(matches!(
            StateStore::open(&path),
            Err(StoreError::AlreadyRunning)
        ));
        let cursor = RolloutCursor {
            task_id: TASK.into(),
            rollout_path: temp.path().join("rollout.jsonl"),
            device: 1,
            inode: 2,
            offset: 100,
            generation: 1,
            anchor: [1; 32],
        };

        observer
            .commit_rollout_completion(None, &cursor, TURN, r#"{"v":1}"#)
            .unwrap();

        assert_eq!(store.rollout_cursor(TASK).unwrap(), Some(cursor));
        assert_eq!(store.completion_count(TASK).unwrap(), 1);
    }

    #[test]
    fn summary_worker_connection_shares_owner_lock_and_discovers_each_task_once() {
        const TASK_A: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const TASK_B: &str = "019fa972-5cfa-75e1-9008-0b17ade9a349";
        const TURN_A: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        const TURN_B: &str = "019fa972-5cfa-75e1-9008-0b17ade9a350";
        const TURN_C: &str = "019fa972-5cfa-75e1-9008-0b17ade9a351";
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let owner = StateStore::open(&path).unwrap();
        let mut worker = owner.open_worker_store().unwrap();
        insert_summary_completion(&owner, TASK_B, TURN_B, r#"{"v":1}"#);
        insert_summary_completion(&owner, TASK_A, TURN_A, r#"{"v":1}"#);
        insert_summary_completion(&owner, TASK_A, TURN_C, r#"{"v":1}"#);

        assert_eq!(
            worker.summary_work_tasks_after(None).unwrap(),
            vec![TASK_A.to_owned(), TASK_B.to_owned()]
        );
        claimed(worker.claim_summary(TASK_A, "worker-request").unwrap());
        drop(owner);
        assert!(matches!(
            StateStore::open(&path),
            Err(StoreError::AlreadyRunning)
        ));
        drop(worker);

        let reopened = StateStore::open(&path).unwrap();
        assert_eq!(reopened.recovered_summaries_on_open(), 1);
        assert_eq!(
            reopened.summary_work_tasks_after(None).unwrap(),
            vec![TASK_A.to_owned(), TASK_B.to_owned()]
        );
    }

    #[test]
    fn summary_worker_task_scan_pages_past_thirty_two_backed_off_candidates() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let store = StateStore::open(&path).unwrap();
        let mut expected = Vec::new();
        for index in 0_u128..35 {
            let task =
                uuid::Uuid::from_u128(0x019fa9725cfa75e19008000000000000 + index).to_string();
            let completion =
                uuid::Uuid::from_u128(0x019fa9725cfa75e19009000000000000 + index).to_string();
            insert_summary_completion(&store, &task, &completion, r#"{"v":1}"#);
            expected.push(task);
        }
        expected.sort();

        let first = store.summary_work_tasks_after(None).unwrap();
        assert_eq!(first, expected[..32]);
        let second = store
            .summary_work_tasks_after(first.last().map(String::as_str))
            .unwrap();
        assert_eq!(second, expected[32..]);
        assert!(
            store
                .summary_work_tasks_after(second.last().map(String::as_str))
                .unwrap()
                .is_empty()
        );
    }

    #[test]
    fn bound_completion_rechecks_the_exact_binding_inside_its_transaction() {
        const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const TURN: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        let temp = tempdir().unwrap();
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        let observed = store.set_binding(1, None, TASK).unwrap().unwrap();
        store
            .set_binding(1, Some(observed.generation), "replacement-task")
            .unwrap()
            .unwrap();
        let cursor = RolloutCursor {
            task_id: TASK.into(),
            rollout_path: temp.path().join("rollout.jsonl"),
            device: 1,
            inode: 2,
            offset: 100,
            generation: 1,
            anchor: [1; 32],
        };

        assert!(matches!(
            store.commit_bound_rollout_completion(&observed, None, &cursor, TURN, r#"{"v":1}"#),
            Err(StoreError::BindingChanged)
        ));
        assert_eq!(store.completion_count(TASK).unwrap(), 0);
        assert!(store.rollout_cursor(TASK).unwrap().is_none());
    }

    #[test]
    fn summary_claim_publish_and_cumulative_replacement_are_transactional() {
        const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const FIRST: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        const SECOND: &str = "019fa972-5cfa-75e1-9008-0b17ade9a349";
        const THIRD: &str = "019fa972-5cfa-75e1-9008-0b17ade9a350";
        let temp = tempdir().unwrap();
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        insert_summary_completion(&store, TASK, FIRST, r#"{"turn":1}"#);

        let first = claimed(store.claim_summary(TASK, "summary-request-1").unwrap());
        assert_eq!(first.outcome, SummaryClaimOutcome::Inserted);
        assert_eq!(first.generation, 1);
        assert!(first.previous_unread.is_none());
        assert_eq!(first.completions.len(), 1);
        let replay = claimed(store.claim_summary(TASK, "summary-request-1").unwrap());
        assert_eq!(replay.outcome, SummaryClaimOutcome::Replay);
        assert_eq!(replay.completions, first.completions);
        assert!(matches!(
            store.claim_summary(TASK, "summary-request-busy"),
            Err(StoreError::SummaryBusy)
        ));
        assert!(matches!(
            store.publish_summary(&first, "wrong/reference"),
            Err(StoreError::InvalidSummaryCacheReference)
        ));

        let first_cache = CacheId::for_task(TASK, 1).unwrap().reference();
        let first_unread = store.publish_summary(&first, &first_cache).unwrap();
        assert_eq!(first_unread.coverage_count, 1);
        assert_eq!(
            store.current_unread_summary(TASK).unwrap(),
            Some(first_unread)
        );
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 0);
        assert_eq!(
            store.claim_summary(TASK, "summary-request-1").unwrap(),
            Some(SummaryClaimResult::Published {
                task_id: TASK.into(),
                generation: 1
            })
        );

        insert_summary_completion(&store, TASK, SECOND, r#"{"turn":2}"#);
        insert_summary_completion(&store, TASK, THIRD, r#"{"turn":3}"#);
        let second = claimed(store.claim_summary(TASK, "summary-request-2").unwrap());
        assert_eq!(second.generation, 2);
        assert_eq!(second.previous_unread.as_ref().unwrap().coverage_count, 1);
        assert_eq!(second.completions.len(), 2);
        let second_cache = CacheId::for_task(TASK, 2).unwrap().reference();
        let second_unread = store.publish_summary(&second, &second_cache).unwrap();
        assert_eq!(second_unread.coverage_count, 3);
        assert_eq!(
            store.current_unread_summary(TASK).unwrap(),
            Some(second_unread)
        );
        let first_state: (String, i64) = store
            .connection
            .query_row(
                "SELECT state, superseded_by FROM summary_ledger
                 WHERE task_id = ?1 AND generation = 1",
                [TASK],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .unwrap();
        assert_eq!(first_state, ("superseded".into(), 2));
        let covered: (u32, u32) = store
            .connection
            .query_row(
                "SELECT COUNT(*), COUNT(DISTINCT summarized_generation)
                 FROM completion_ledger WHERE task_id = ?1 AND summarized_generation = 2",
                [TASK],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .unwrap();
        assert_eq!(covered, (3, 1));
    }

    #[test]
    fn summary_failure_restart_and_stale_claim_preserve_old_unread_and_pending() {
        const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const FIRST: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        const SECOND: &str = "019fa972-5cfa-75e1-9008-0b17ade9a349";
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let mut store = StateStore::open(&path).unwrap();
        insert_summary_completion(&store, TASK, FIRST, r#"{"turn":1}"#);
        let first = claimed(store.claim_summary(TASK, "request-1").unwrap());
        let first_cache = CacheId::for_task(TASK, 1).unwrap().reference();
        let old_unread = store.publish_summary(&first, &first_cache).unwrap();

        insert_summary_completion(&store, TASK, SECOND, r#"{"turn":2}"#);
        let second = claimed(store.claim_summary(TASK, "request-2").unwrap());
        let mut stale = second.clone();
        stale.request_id = "stale-request".into();
        assert!(!store.abandon_summary_claim(&stale).unwrap());
        assert_eq!(
            store.current_unread_summary(TASK).unwrap(),
            Some(old_unread.clone())
        );
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 1);
        drop(store);

        let mut reopened = StateStore::open(&path).unwrap();
        assert_eq!(reopened.recovered_summaries_on_open(), 1);
        assert_eq!(
            reopened.current_unread_summary(TASK).unwrap(),
            Some(old_unread)
        );
        assert_eq!(reopened.pending_summary_completion_count(TASK).unwrap(), 1);
        let third = "019fa972-5cfa-75e1-9008-0b17ade9a350";
        insert_summary_completion(&reopened, TASK, third, r#"{"turn":3}"#);
        assert!(matches!(
            reopened.claim_summary(TASK, "new-request-after-restart"),
            Err(StoreError::SummaryBusy)
        ));
        let recovered = reopened.resume_interrupted_summary(TASK).unwrap().unwrap();
        assert_eq!(recovered.outcome, SummaryClaimOutcome::Replay);
        assert_eq!(recovered.request_id, "request-2");
        assert_eq!(recovered.generation, 2);
        assert_eq!(recovered.completions.len(), 1);
        assert_eq!(recovered.completions[0].completion_id, SECOND);
        assert!(reopened.abandon_summary_claim(&recovered).unwrap());
        assert_eq!(reopened.pending_summary_completion_count(TASK).unwrap(), 2);
        assert!(matches!(
            reopened.claim_summary(TASK, "request-2"),
            Err(StoreError::SummaryRequestAbandoned)
        ));
    }

    #[test]
    fn summary_publish_failure_rolls_back_coverage_and_current_generation() {
        const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const TURN: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        let temp = tempdir().unwrap();
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        insert_summary_completion(&store, TASK, TURN, r#"{"turn":1}"#);
        let claim = claimed(store.claim_summary(TASK, "request-1").unwrap());
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER reject_summary_coverage BEFORE UPDATE OF summarized_generation
                 ON completion_ledger
                 WHEN NEW.summarized_generation IS NOT NULL
                 BEGIN SELECT RAISE(ABORT, 'injected summary failure'); END;",
            )
            .unwrap();
        let cache = CacheId::for_task(TASK, 1).unwrap().reference();
        assert!(store.publish_summary(&claim, &cache).is_err());
        assert!(store.current_unread_summary(TASK).unwrap().is_none());
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 1);
        let state: String = store
            .connection
            .query_row(
                "SELECT state FROM summary_ledger WHERE task_id = ?1 AND generation = 1",
                [TASK],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(state, "generating");
    }

    #[test]
    fn summary_claim_is_bounded_and_leaves_later_completions_pending() {
        const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        let temp = tempdir().unwrap();
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        for index in 0..40_u128 {
            let completion_id =
                uuid::Uuid::from_u128(0x019f_a972_5cfa_75e1_9008_0000_0000_0000_u128 + index + 1)
                    .to_string();
            insert_summary_completion(
                &store,
                TASK,
                &completion_id,
                &format!(r#"{{"turn":{index}}}"#),
            );
        }

        let first = claimed(store.claim_summary(TASK, "request-1").unwrap());
        assert_eq!(first.completions.len(), MAX_SUMMARY_COMPLETIONS_PER_CLAIM);
        let cache = CacheId::for_task(TASK, 1).unwrap().reference();
        assert_eq!(
            store
                .publish_summary(&first, &cache)
                .unwrap()
                .coverage_count,
            32
        );
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 8);
        let second = claimed(store.claim_summary(TASK, "request-2").unwrap());
        assert_eq!(second.completions.len(), 8);
        assert_eq!(second.previous_unread.unwrap().coverage_count, 32);
    }

    #[test]
    fn leased_generation_can_be_superseded_without_being_consumed() {
        const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const FIRST: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        const SECOND: &str = "019fa972-5cfa-75e1-9008-0b17ade9a349";
        let temp = tempdir().unwrap();
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        insert_summary_completion(&store, TASK, FIRST, r#"{"turn":1}"#);
        let first = claimed(store.claim_summary(TASK, "request-1").unwrap());
        store
            .publish_summary(&first, &CacheId::for_task(TASK, 1).unwrap().reference())
            .unwrap();
        store
            .connection
            .execute(
                "UPDATE summary_ledger SET state = 'leased'
                 WHERE task_id = ?1 AND generation = 1",
                [TASK],
            )
            .unwrap();

        insert_summary_completion(&store, TASK, SECOND, r#"{"turn":2}"#);
        let second = claimed(store.claim_summary(TASK, "request-2").unwrap());
        assert_eq!(second.previous_unread.as_ref().unwrap().generation, 1);
        store
            .publish_summary(&second, &CacheId::for_task(TASK, 2).unwrap().reference())
            .unwrap();

        let old: (String, i64) = store
            .connection
            .query_row(
                "SELECT state, superseded_by FROM summary_ledger
                 WHERE task_id = ?1 AND generation = 1",
                [TASK],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .unwrap();
        assert_eq!(old, ("leased".into(), 2));
        assert_eq!(
            store
                .current_unread_summary(TASK)
                .unwrap()
                .unwrap()
                .generation,
            2
        );
    }

    #[test]
    fn playback_lease_is_exact_recovers_unread_and_only_finish_consumes() {
        const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const TURN: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        let temp = tempdir().unwrap();
        let path = temp.path().join("state.sqlite3");
        let mut store = StateStore::open(&path).unwrap();
        store.set_binding(2, None, TASK).unwrap().unwrap();
        insert_summary_completion(&store, TASK, TURN, r#"{"turn":1}"#);
        let claim = claimed(store.claim_summary(TASK, "request-1").unwrap());
        let cache = CacheId::for_task(TASK, 1).unwrap().reference();
        store.publish_summary(&claim, &cache).unwrap();

        let lease = store
            .acquire_summary_playback(2, 11, 19, 41)
            .unwrap()
            .unwrap();
        assert_eq!(lease.summary_generation, 1);
        assert_eq!(
            store.acquire_summary_playback(2, 11, 19, 41).unwrap(),
            Some(lease.clone())
        );
        assert!(matches!(
            store.acquire_summary_playback(2, 12, 19, 42),
            Err(StoreError::PlaybackBusy)
        ));
        let mut stale = lease.clone();
        stale.lease = 42;
        assert!(!store.finish_summary_playback(&stale).unwrap());
        assert!(store.cancel_summary_playback(&lease).unwrap());
        assert_eq!(
            store
                .current_unread_summary(TASK)
                .unwrap()
                .unwrap()
                .generation,
            1
        );

        let lease = store
            .acquire_summary_playback(2, 13, 19, 43)
            .unwrap()
            .unwrap();
        drop(store);
        let mut reopened = StateStore::open(&path).unwrap();
        assert_eq!(
            reopened
                .current_unread_summary(TASK)
                .unwrap()
                .unwrap()
                .generation,
            1
        );
        assert!(!reopened.finish_summary_playback(&lease).unwrap());
        let lease = reopened
            .acquire_summary_playback(2, 14, 20, 44)
            .unwrap()
            .unwrap();
        assert!(reopened.finish_summary_playback(&lease).unwrap());
        assert!(reopened.current_unread_summary(TASK).unwrap().is_none());
        assert!(
            reopened
                .retained_summary_cache_references()
                .unwrap()
                .is_empty()
        );
        assert!(!reopened.finish_summary_playback(&lease).unwrap());
    }

    #[test]
    fn summary_request_ids_are_global_and_generation_overflow_fails_closed() {
        const TASK_A: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
        const TASK_B: &str = "019fa972-5cfa-75e1-9008-0b17ade9a349";
        const TURN_A: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
        const TURN_B: &str = "019fa972-5cfa-75e1-9008-0b17ade9a350";
        let temp = tempdir().unwrap();
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        insert_summary_completion(&store, TASK_A, TURN_A, r#"{"turn":1}"#);
        insert_summary_completion(&store, TASK_B, TURN_B, r#"{"turn":2}"#);
        let first = claimed(store.claim_summary(TASK_A, "global-request").unwrap());
        assert!(matches!(
            store.claim_summary(TASK_B, "global-request"),
            Err(StoreError::SummaryRequestConflict)
        ));
        assert!(store.abandon_summary_claim(&first).unwrap());
        assert!(matches!(
            store.claim_summary(TASK_A, "global-request"),
            Err(StoreError::SummaryRequestAbandoned)
        ));
        assert!(matches!(
            store.claim_summary(TASK_B, "global-request"),
            Err(StoreError::SummaryRequestConflict)
        ));

        store
            .connection
            .execute(
                "INSERT INTO summary_ledger
                 (task_id, generation, state, covers_completions, cache_object, request_id,
                  claim_id, previous_generation, superseded_by, created_at, updated_at)
                 VALUES (?1, ?2, 'heard', '[]', NULL, 'overflow-seed', NULL, NULL, NULL,
                         unixepoch(), unixepoch())",
                params![TASK_A, i64::MAX],
            )
            .unwrap();
        assert!(matches!(
            store.claim_summary(TASK_A, "overflow-request"),
            Err(StoreError::SummaryGenerationOutOfRange)
        ));
        assert_eq!(store.pending_summary_completion_count(TASK_A).unwrap(), 1);
    }

    #[test]
    fn legacy_summary_table_migrates_without_resurrecting_generating_rows() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection
            .execute_batch(
                "CREATE TABLE summary_ledger (
                   task_id TEXT NOT NULL,
                   generation INTEGER NOT NULL,
                   state TEXT NOT NULL,
                   covers_completions TEXT NOT NULL,
                   cache_object TEXT,
                   created_at INTEGER NOT NULL,
                   PRIMARY KEY(task_id, generation)
                 );
                 INSERT INTO summary_ledger VALUES
                   ('task-a', 1, 'unheard', '[\"one\"]', 'object/1', 1),
                   ('task-b', 1, 'generating', '[\"two\"]', NULL, 1);",
            )
            .unwrap();

        let transaction = connection
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .unwrap();
        migrate_summary_ledger(&transaction).unwrap();
        transaction.commit().unwrap();

        assert!(has_column(&connection, "summary_ledger", "request_id").unwrap());
        let rows: u32 = connection
            .query_row("SELECT COUNT(*) FROM summary_ledger", [], |row| row.get(0))
            .unwrap();
        assert_eq!(rows, 1);
        let state: String = connection
            .query_row("SELECT state FROM summary_ledger", [], |row| row.get(0))
            .unwrap();
        assert_eq!(state, "unheard");
    }

    #[test]
    fn pre_interrupted_summary_schema_preserves_immutable_claim_and_rebuilds_indexes() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection
            .execute_batch(
                "CREATE TABLE summary_ledger (
                   task_id TEXT NOT NULL,
                   generation INTEGER NOT NULL CHECK(generation > 0),
                   state TEXT NOT NULL CHECK(state IN
                     ('generating', 'unheard', 'leased', 'heard', 'superseded')),
                   covers_completions TEXT NOT NULL,
                   cache_object TEXT,
                   request_id TEXT,
                   claim_id TEXT,
                   previous_generation INTEGER,
                   superseded_by INTEGER,
                   created_at INTEGER NOT NULL,
                   updated_at INTEGER NOT NULL,
                   PRIMARY KEY(task_id, generation)
                 );
                 CREATE UNIQUE INDEX summary_one_generating_per_task
                   ON summary_ledger(task_id) WHERE state = 'generating';
                 CREATE UNIQUE INDEX summary_request_id_unique
                   ON summary_ledger(request_id) WHERE request_id IS NOT NULL;
                 CREATE UNIQUE INDEX summary_one_current_unread_per_task
                   ON summary_ledger(task_id)
                   WHERE state IN ('unheard', 'leased') AND superseded_by IS NULL;
                 INSERT INTO summary_ledger VALUES
                   ('task-a', 2, 'generating', '[\"turn-a\"]', NULL,
                    'request-a', 'request-a', 1, NULL, 10, 11);",
            )
            .unwrap();

        let transaction = connection
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .unwrap();
        migrate_summary_ledger(&transaction).unwrap();
        transaction.commit().unwrap();

        let migrated: (String, String, String, i64, String) = connection
            .query_row(
                "SELECT state, request_id, claim_id, previous_generation, covers_completions
                 FROM summary_ledger",
                [],
                |row| {
                    Ok((
                        row.get(0)?,
                        row.get(1)?,
                        row.get(2)?,
                        row.get(3)?,
                        row.get(4)?,
                    ))
                },
            )
            .unwrap();
        assert_eq!(
            migrated,
            (
                "interrupted".into(),
                "request-a".into(),
                "request-a".into(),
                1,
                r#"["turn-a"]"#.into()
            )
        );
        let index_count: u32 = connection
            .query_row(
                "SELECT COUNT(*) FROM sqlite_schema
                 WHERE type = 'index' AND tbl_name = 'summary_ledger'
                   AND name IN ('summary_one_active_claim_per_task',
                                'summary_request_id_unique',
                                'summary_one_current_unread_per_task')",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(index_count, 3);
    }
}

use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File, OpenOptions};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::os::unix::fs::{FileExt as UnixFileExt, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};
use std::time::{Duration, Instant};

use fs2::FileExt;
use rusqlite::limits::Limit;
use rusqlite::{Connection, OpenFlags, OptionalExtension};
use serde::Deserialize;
use sha2::{Digest, Sha256};
use thiserror::Error;
use uuid::Uuid;

const MAX_GLOBAL_STATE_BYTES: u64 = 4 * 1024 * 1024;
const MAX_SESSION_INDEX_BYTES: u64 = 8 * 1024 * 1024;
const MAX_SESSION_INDEX_LINES: usize = 2_048;
const MAX_SESSION_INDEX_LINE_BYTES: usize = 64 * 1024;
const MAX_DATABASE_BYTES: u64 = 256 * 1024 * 1024;
const MAX_DATABASE_WAL_BYTES: u64 = 64 * 1024 * 1024;
const MAX_DATABASE_SHM_BYTES: u64 = 8 * 1024 * 1024;
const MAX_PINNED_CANDIDATES: usize = 64;
const MAX_RECENT_CANDIDATES: usize = 64;
// SQLite must be able to inspect and reject an oversized source column before `substr` caps what
// crosses into Rust. This bounds SQLite's record allocation while keeping the Rust surface tighter.
const MAX_DATABASE_RECORD_BYTES: i32 = 1024 * 1024;
const QUERY_PROGRESS_INTERVAL: i32 = 1_000;
const MAX_QUERY_PROGRESS_CHECKS: usize = 2_000;
const MAX_QUERY_DURATION: Duration = Duration::from_millis(150);
pub const MAX_PINNED_TASKS: usize = 16;
pub const MAX_RECENT_TASKS: usize = 8;
pub const MAX_TASK_NAME_CHARS: usize = 96;
pub const MAX_PROJECT_NAME_CHARS: usize = 64;

const THREAD_COLUMNS: &str = "
id,
substr(COALESCE(name, ''), 1, 4096),
substr(COALESCE(title, ''), 1, 4096),
substr(cwd, 1, 4096),
substr(rollout_path, 1, 4096),
MAX(COALESCE(recency_at_ms, 0), COALESCE(updated_at_ms, 0),
    COALESCE(updated_at, 0) * 1000),
substr(COALESCE(source, ''), 1, 8192),
substr(COALESCE(thread_source, ''), 1, 64),
substr(COALESCE(agent_role, ''), 1, 256)
";

const BOUNDED_THREAD_PREDICATE: &str = "
archived = 0
AND length(id) BETWEEN 1 AND 36
AND length(COALESCE(name, '')) <= 4096
AND length(COALESCE(title, '')) <= 4096
AND length(cwd) BETWEEN 1 AND 4096
AND length(rollout_path) BETWEEN 1 AND 4096
AND length(COALESCE(source, '')) <= 8192
AND length(COALESCE(thread_source, '')) <= 64
AND length(COALESCE(agent_role, '')) <= 256
";

static SNAPSHOT_PROCESS_MUTEX: Mutex<()> = Mutex::new(());

#[derive(Debug, Error)]
pub enum CatalogError {
    #[error("Codex index I/O failed")]
    Io(#[from] io::Error),
    #[error("Codex state database read failed")]
    Sqlite(#[from] rusqlite::Error),
    #[error("Codex index is malformed or unsupported")]
    InvalidIndex,
    #[error("Codex index exceeds its read limit")]
    IndexTooLarge,
    #[error("Codex index path is unsafe or changed while being read")]
    UnsafePath,
    #[error("task is not in the current bounded Codex allow-list")]
    NotAllowlisted,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodexTask {
    pub task_id: String,
    pub name: String,
    pub project: String,
    pub cwd: PathBuf,
    pub rollout_path: PathBuf,
    pub updated_at_ms: u64,
    pub pinned: bool,
}

#[derive(Debug, Clone)]
pub struct CodexTaskCatalog {
    codex_home: PathBuf,
    snapshot_root: PathBuf,
}

impl CodexTaskCatalog {
    pub fn from_paths(codex_home: PathBuf, snapshot_root: PathBuf) -> Self {
        Self {
            codex_home,
            snapshot_root,
        }
    }

    pub fn from_environment() -> Result<Self, CatalogError> {
        let home = PathBuf::from(std::env::var_os("HOME").ok_or(CatalogError::InvalidIndex)?);
        let codex_home = match std::env::var_os("CODEX_HOME") {
            Some(value) => PathBuf::from(value),
            None => home.join(".codex"),
        };
        let snapshot_root = crate::paths::AppPaths::from_home(&home)
            .runtime_directory
            .join("catalog-snapshots");
        Ok(Self::from_paths(codex_home, snapshot_root))
    }

    pub fn list_tasks(&self) -> Result<Vec<CodexTask>, CatalogError> {
        let global = read_global_state(&self.codex_home.join(".codex-global-state.json"))?;
        let index = read_session_index(&self.codex_home.join("session_index.jsonl"))?;
        let pinned_ids = pinned_ids(&global)?;
        let database = read_database(
            &self.codex_home.join("state_5.sqlite"),
            &pinned_ids,
            &self.snapshot_root,
        )?;
        let project_labels = project_labels(&global)?;
        let mut tasks = Vec::new();
        let mut seen = BTreeSet::new();

        for task_id in pinned_ids {
            let Some(row) = database.pinned.get(&task_id) else {
                continue;
            };
            if let Some(task) =
                build_task(&task_id, row, index.get(&task_id), &project_labels, true)
            {
                seen.insert(task_id);
                tasks.push(task);
                if tasks.len() == MAX_PINNED_TASKS {
                    break;
                }
            }
        }

        let recent = database
            .recent
            .iter()
            .filter(|row| !seen.contains(&row.task_id))
            .filter_map(|row| {
                build_task(
                    &row.task_id,
                    &row.task,
                    index.get(&row.task_id),
                    &project_labels,
                    false,
                )
            })
            .collect::<Vec<_>>();
        tasks.extend(recent.into_iter().take(MAX_RECENT_TASKS));
        Ok(tasks)
    }

    pub fn allowlisted(&self, task_id: &str) -> Result<CodexTask, CatalogError> {
        if Uuid::parse_str(task_id).is_err() {
            return Err(CatalogError::NotAllowlisted);
        }
        self.list_tasks()?
            .into_iter()
            .find(|task| task.task_id == task_id)
            .ok_or(CatalogError::NotAllowlisted)
    }
}

#[derive(Debug, Deserialize)]
struct SessionIndexRow {
    id: String,
    thread_name: String,
}

#[derive(Debug)]
struct DatabaseTask {
    name: String,
    title: String,
    cwd: PathBuf,
    rollout_path: PathBuf,
    updated_at_ms: u64,
}

#[derive(Debug)]
struct DatabaseTaskRow {
    task_id: String,
    task: DatabaseTask,
}

#[derive(Debug)]
struct DatabaseCatalog {
    pinned: BTreeMap<String, DatabaseTask>,
    recent: Vec<DatabaseTaskRow>,
}

fn read_global_state(path: &Path) -> Result<serde_json::Value, CatalogError> {
    let bytes = read_bounded_regular_file(path, MAX_GLOBAL_STATE_BYTES)?;
    let value = serde_json::from_slice::<serde_json::Value>(&bytes)
        .map_err(|_| CatalogError::InvalidIndex)?;
    if !value.is_object() {
        return Err(CatalogError::InvalidIndex);
    }
    Ok(value)
}

fn read_session_index(path: &Path) -> Result<BTreeMap<String, String>, CatalogError> {
    let file = open_guarded_regular_file(path, MAX_SESSION_INDEX_BYTES)?;
    let identity = file_identity(&file)?;
    let mut result = BTreeMap::new();
    let mut reader = BufReader::new(&file);
    let mut line = Vec::new();
    for _ in 0..MAX_SESSION_INDEX_LINES {
        line.clear();
        let read = reader.read_until(b'\n', &mut line)?;
        if read == 0 {
            ensure_path_identity(path, identity)?;
            return Ok(result);
        }
        if line.len() > MAX_SESSION_INDEX_LINE_BYTES {
            return Err(CatalogError::IndexTooLarge);
        }
        let row = serde_json::from_slice::<SessionIndexRow>(&line)
            .map_err(|_| CatalogError::InvalidIndex)?;
        if Uuid::parse_str(&row.id).is_err() || row.thread_name.trim().is_empty() {
            return Err(CatalogError::InvalidIndex);
        }
        result.insert(row.id, clean_text(&row.thread_name, MAX_TASK_NAME_CHARS));
    }
    line.clear();
    if reader.read_until(b'\n', &mut line)? != 0 {
        return Err(CatalogError::IndexTooLarge);
    }
    ensure_path_identity(path, identity)?;
    Ok(result)
}

fn read_database(
    path: &Path,
    pinned_ids: &[String],
    snapshot_root: &Path,
) -> Result<DatabaseCatalog, CatalogError> {
    read_database_with_budget(
        path,
        pinned_ids,
        snapshot_root,
        MAX_QUERY_PROGRESS_CHECKS,
        MAX_QUERY_DURATION,
    )
}

fn read_database_with_budget(
    path: &Path,
    pinned_ids: &[String],
    snapshot_root: &Path,
    max_progress_checks: usize,
    max_duration: Duration,
) -> Result<DatabaseCatalog, CatalogError> {
    let snapshot_manager = ManagedSnapshotRoot::acquire(snapshot_root)?;
    let guards = DatabaseFileGuards::open(path)?;
    let snapshot = guards.snapshot(&snapshot_manager)?;
    let connection = Connection::open_with_flags(
        snapshot.database_path(),
        OpenFlags::SQLITE_OPEN_READ_ONLY
            | OpenFlags::SQLITE_OPEN_NO_MUTEX
            | OpenFlags::SQLITE_OPEN_URI,
    )?;
    connection.busy_timeout(Duration::from_millis(250))?;
    connection.pragma_update(None, "query_only", "ON")?;
    connection.set_limit(Limit::SQLITE_LIMIT_LENGTH, MAX_DATABASE_RECORD_BYTES)?;
    let started = Instant::now();
    let mut checks = 0_usize;
    connection.progress_handler(
        QUERY_PROGRESS_INTERVAL,
        Some(move || {
            checks += 1;
            checks > max_progress_checks || started.elapsed() > max_duration
        }),
    )?;

    let home = path.parent().ok_or(CatalogError::UnsafePath)?;
    let mut pinned = BTreeMap::new();
    let pinned_query = format!(
        "SELECT {THREAD_COLUMNS} FROM threads WHERE id = ?1 AND {BOUNDED_THREAD_PREDICATE} LIMIT 1"
    );
    let mut pinned_statement = connection
        .prepare(&pinned_query)
        .map_err(|_| CatalogError::InvalidIndex)?;
    for task_id in pinned_ids.iter().take(MAX_PINNED_CANDIDATES) {
        let row = pinned_statement
            .query_row([task_id], database_row_from_sql)
            .optional()
            .map_err(|_| CatalogError::InvalidIndex)?;
        if let Some(row) = row
            && let Some(row) = validate_database_row(home, row)?
        {
            pinned.insert(row.task_id, row.task);
        }
    }
    drop(pinned_statement);

    let recent_query = format!(
        "SELECT {THREAD_COLUMNS} FROM threads
         WHERE recency_at_ms IS NOT NULL AND {BOUNDED_THREAD_PREDICATE}
         ORDER BY recency_at_ms DESC, id DESC LIMIT {MAX_RECENT_CANDIDATES}"
    );
    let mut recent_statement = connection
        .prepare(&recent_query)
        .map_err(|_| CatalogError::InvalidIndex)?;
    let rows = recent_statement
        .query_map([], database_row_from_sql)
        .map_err(|_| CatalogError::InvalidIndex)?;
    let mut recent = Vec::new();
    for row in rows {
        let row = row.map_err(|_| CatalogError::InvalidIndex)?;
        if let Some(row) = validate_database_row(home, row)? {
            recent.push(row);
        }
    }
    drop(recent_statement);
    connection.progress_handler(0, None::<fn() -> bool>)?;
    drop(connection);
    guards.verify_snapshot(&snapshot)?;
    Ok(DatabaseCatalog { pinned, recent })
}

type RawDatabaseRow = (
    String,
    String,
    String,
    String,
    String,
    i64,
    String,
    String,
    String,
);

fn database_row_from_sql(row: &rusqlite::Row<'_>) -> rusqlite::Result<RawDatabaseRow> {
    Ok((
        row.get(0)?,
        row.get(1)?,
        row.get(2)?,
        row.get(3)?,
        row.get(4)?,
        row.get(5)?,
        row.get(6)?,
        row.get(7)?,
        row.get(8)?,
    ))
}

fn validate_database_row(
    codex_home: &Path,
    row: RawDatabaseRow,
) -> Result<Option<DatabaseTaskRow>, CatalogError> {
    let (task_id, name, title, cwd, rollout_path, updated_at_ms, source, thread_source, agent_role) =
        row;
    if Uuid::parse_str(&task_id).is_err()
        || updated_at_ms < 0
        || !is_user_owned_thread(&source, &thread_source, &agent_role)
    {
        return Ok(None);
    }
    let rollout_path = PathBuf::from(rollout_path);
    if !validate_rollout_path(codex_home, &rollout_path)? {
        return Ok(None);
    }
    Ok(Some(DatabaseTaskRow {
        task_id,
        task: DatabaseTask {
            name,
            title,
            cwd: PathBuf::from(cwd),
            rollout_path,
            updated_at_ms: updated_at_ms as u64,
        },
    }))
}

fn is_user_owned_thread(source: &str, thread_source: &str, agent_role: &str) -> bool {
    if !agent_role.trim().is_empty() || thread_source.trim().eq_ignore_ascii_case("subagent") {
        return false;
    }
    let source = source.trim();
    if source.eq_ignore_ascii_case("subagent") {
        return false;
    }
    if source.starts_with('{') {
        let Ok(value) = serde_json::from_str::<serde_json::Value>(source) else {
            return false;
        };
        if value
            .as_object()
            .is_some_and(|object| object.contains_key("subagent"))
        {
            return false;
        }
    }
    true
}

fn pinned_ids(global: &serde_json::Value) -> Result<Vec<String>, CatalogError> {
    let object = global.as_object().ok_or(CatalogError::InvalidIndex)?;
    let values = object
        .get("pinned-thread-ids")
        .and_then(serde_json::Value::as_array)
        .ok_or(CatalogError::InvalidIndex)?;
    if values.len() > MAX_PINNED_CANDIDATES {
        return Err(CatalogError::IndexTooLarge);
    }
    let mut pinned = Vec::new();
    for value in values {
        let task_id = value.as_str().ok_or(CatalogError::InvalidIndex)?;
        if Uuid::parse_str(task_id).is_err() {
            return Err(CatalogError::InvalidIndex);
        }
        if !pinned.iter().any(|existing| existing == task_id) {
            pinned.push(task_id.to_owned());
        }
    }
    let pinned_set = pinned.iter().cloned().collect::<BTreeSet<_>>();
    let Some(order) = object
        .get("electron-persisted-atom-state")
        .and_then(serde_json::Value::as_object)
        .and_then(|atoms| atoms.get("unified-sidebar-pinned-order-v1"))
        .and_then(serde_json::Value::as_array)
    else {
        return Ok(pinned);
    };
    let prefix = "codex:thread:local:";
    let mut ordered = Vec::new();
    for value in order {
        let Some(value) = value.as_str() else {
            return Err(CatalogError::InvalidIndex);
        };
        let Some(task_id) = value.strip_prefix(prefix) else {
            continue;
        };
        if Uuid::parse_str(task_id).is_ok()
            && pinned_set.contains(task_id)
            && !ordered.iter().any(|existing| existing == task_id)
        {
            ordered.push(task_id.to_owned());
        }
    }
    for task_id in pinned {
        if !ordered.contains(&task_id) {
            ordered.push(task_id);
        }
    }
    Ok(ordered)
}

fn project_labels(global: &serde_json::Value) -> Result<BTreeMap<PathBuf, String>, CatalogError> {
    let Some(labels) = global
        .get("electron-workspace-root-labels")
        .and_then(serde_json::Value::as_object)
    else {
        return Ok(BTreeMap::new());
    };
    let mut result = BTreeMap::new();
    for (path, value) in labels {
        let label = value.as_str().ok_or(CatalogError::InvalidIndex)?;
        if !label.trim().is_empty() {
            result.insert(
                PathBuf::from(path),
                clean_text(label, MAX_PROJECT_NAME_CHARS),
            );
        }
    }
    Ok(result)
}

fn build_task(
    task_id: &str,
    database: &DatabaseTask,
    index_name: Option<&String>,
    project_labels: &BTreeMap<PathBuf, String>,
    pinned: bool,
) -> Option<CodexTask> {
    let fallback = database
        .cwd
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or(&task_id[..8]);
    let name = [
        index_name.map(String::as_str),
        Some(database.name.as_str()),
        Some(database.title.as_str()),
        Some(fallback),
    ]
    .into_iter()
    .flatten()
    .map(|value| clean_text(value, MAX_TASK_NAME_CHARS))
    .find(|value| !value.is_empty())?;
    if is_internal_task_name(&name) {
        return None;
    }
    let project = project_labels
        .get(&database.cwd)
        .cloned()
        .or_else(|| {
            database
                .cwd
                .file_name()
                .and_then(|value| value.to_str())
                .map(|value| clean_text(value, MAX_PROJECT_NAME_CHARS))
        })
        .unwrap_or_default();
    Some(CodexTask {
        task_id: task_id.to_owned(),
        name,
        project,
        cwd: database.cwd.clone(),
        rollout_path: database.rollout_path.clone(),
        updated_at_ms: database.updated_at_ms,
        pinned,
    })
}

fn clean_text(value: &str, max_chars: usize) -> String {
    value
        .replace('\0', "")
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(max_chars)
        .collect()
}

fn is_internal_task_name(value: &str) -> bool {
    value
        .to_lowercase()
        .starts_with("the following is the codex agent history whose request action")
}

fn validate_rollout_path(codex_home: &Path, path: &Path) -> Result<bool, CatalogError> {
    let sessions = codex_home.join("sessions");
    if !path.is_absolute()
        || !path.starts_with(&sessions)
        || path
            .components()
            .any(|component| matches!(component, std::path::Component::ParentDir))
    {
        return Err(CatalogError::UnsafePath);
    }
    let parent = path.parent().ok_or(CatalogError::UnsafePath)?;
    let name = path.file_name().ok_or(CatalogError::UnsafePath)?;
    let directory = match crate::paths::open_owned_directory_chain(parent, false) {
        Ok(directory) => directory,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(false),
        Err(_) => return Err(CatalogError::UnsafePath),
    };
    let file = match crate::paths::open_file_at(&directory, name, false) {
        Ok(file) => file,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(false),
        Err(_) => return Err(CatalogError::UnsafePath),
    };
    let metadata = file.metadata()?;
    if !metadata.is_file() || metadata.uid() != unsafe { libc::geteuid() } {
        return Err(CatalogError::UnsafePath);
    }
    Ok(true)
}

fn read_bounded_regular_file(path: &Path, max_bytes: u64) -> Result<Vec<u8>, CatalogError> {
    let mut file = open_guarded_regular_file(path, max_bytes)?;
    let identity = file_identity(&file)?;
    let mut bytes = Vec::with_capacity(file.metadata()?.len() as usize);
    file.read_to_end(&mut bytes)?;
    if bytes.len() as u64 > max_bytes {
        return Err(CatalogError::IndexTooLarge);
    }
    ensure_path_identity(path, identity)?;
    Ok(bytes)
}

struct GuardedDatabaseFile {
    path: PathBuf,
    file: File,
    identity: (u64, u64),
    max_bytes: u64,
}

impl GuardedDatabaseFile {
    fn open(path: PathBuf, max_bytes: u64) -> Result<Self, CatalogError> {
        let file = open_guarded_regular_file(&path, max_bytes)?;
        let identity = file_identity(&file)?;
        Ok(Self {
            path,
            file,
            identity,
            max_bytes,
        })
    }

    fn verify(&self) -> Result<(), CatalogError> {
        let opened = self.file.metadata()?;
        if opened.len() > self.max_bytes {
            return Err(CatalogError::IndexTooLarge);
        }
        ensure_path_identity_bounded(&self.path, self.identity, self.max_bytes)
    }

    fn copy_to(&self, destination: &Path) -> Result<FileDigest, CatalogError> {
        self.verify()?;
        let mut output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(destination)?;
        let digest = hash_guarded_file(&self.file, self.max_bytes, Some(&mut output))?;
        output.sync_all()?;
        Ok(digest)
    }

    fn verify_digest(&self, expected: &FileDigest) -> Result<(), CatalogError> {
        self.verify()?;
        if &hash_guarded_file(&self.file, self.max_bytes, None)? != expected {
            return Err(CatalogError::UnsafePath);
        }
        Ok(())
    }
}

enum OptionalDatabaseFile {
    Missing(PathBuf),
    Present(GuardedDatabaseFile),
}

impl OptionalDatabaseFile {
    fn open(path: PathBuf, max_bytes: u64) -> Result<Self, CatalogError> {
        match fs::symlink_metadata(&path) {
            Ok(_) => Ok(Self::Present(GuardedDatabaseFile::open(path, max_bytes)?)),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(Self::Missing(path)),
            Err(error) => Err(error.into()),
        }
    }

    fn verify(&self) -> Result<(), CatalogError> {
        match self {
            Self::Present(file) => file.verify(),
            Self::Missing(path) => match fs::symlink_metadata(path) {
                Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
                Ok(_) => Err(CatalogError::UnsafePath),
                Err(error) => Err(error.into()),
            },
        }
    }

    fn copy_to(&self, destination: &Path) -> Result<Option<FileDigest>, CatalogError> {
        match self {
            Self::Present(file) => file.copy_to(destination).map(Some),
            Self::Missing(_) => {
                self.verify()?;
                Ok(None)
            }
        }
    }

    fn verify_digest(&self, expected: Option<&FileDigest>) -> Result<(), CatalogError> {
        match (self, expected) {
            (Self::Present(file), Some(expected)) => file.verify_digest(expected),
            (Self::Missing(_), None) => self.verify(),
            _ => Err(CatalogError::UnsafePath),
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
struct FileDigest {
    bytes: u64,
    sha256: [u8; 32],
}

struct DatabaseSnapshot {
    directory: PathBuf,
    main_digest: FileDigest,
    wal_digest: Option<FileDigest>,
}

impl DatabaseSnapshot {
    fn database_path(&self) -> PathBuf {
        self.directory.join("state.sqlite")
    }
}

impl Drop for DatabaseSnapshot {
    fn drop(&mut self) {
        let _ = remove_managed_snapshot_directory(&self.directory);
    }
}

struct ManagedSnapshotRoot {
    path: PathBuf,
    _process_lock: MutexGuard<'static, ()>,
    _lock: crate::paths::ExplicitFileLock,
}

impl ManagedSnapshotRoot {
    fn acquire(path: &Path) -> Result<Self, CatalogError> {
        let process_lock = SNAPSHOT_PROCESS_MUTEX
            .lock()
            .map_err(|_| CatalogError::UnsafePath)?;
        crate::paths::secure_directory(path)?;
        let lock_file = crate::paths::open_private_file(&path.join(".lock"))?;
        lock_file.lock_exclusive()?;
        let lock = crate::paths::ExplicitFileLock::from_locked(lock_file);
        let mut marker = crate::paths::open_private_file(&path.join(".owner-v1"))?;
        let mut marker_bytes = Vec::new();
        Read::by_ref(&mut marker)
            .take(64)
            .read_to_end(&mut marker_bytes)?;
        if marker.metadata()?.len() > 64 {
            return Err(CatalogError::IndexTooLarge);
        }
        if marker_bytes.is_empty() {
            marker.write_all(b"easy-codex-input-catalog-v1\n")?;
            marker.sync_all()?;
        } else if marker_bytes != b"easy-codex-input-catalog-v1\n" {
            return Err(CatalogError::UnsafePath);
        }
        let manager = Self {
            path: path.to_path_buf(),
            _process_lock: process_lock,
            _lock: lock,
        };
        manager.reconcile()?;
        Ok(manager)
    }

    fn create_snapshot_directory(&self) -> Result<PathBuf, CatalogError> {
        for _ in 0..8 {
            let path = self.path.join(format!("snapshot-{}", Uuid::new_v4()));
            match fs::create_dir(&path) {
                Ok(()) => {
                    fs::set_permissions(&path, fs::Permissions::from_mode(0o700))?;
                    let metadata = fs::symlink_metadata(&path)?;
                    if !metadata.is_dir()
                        || metadata.uid() != unsafe { libc::geteuid() }
                        || metadata.permissions().mode() & 0o777 != 0o700
                    {
                        return Err(CatalogError::UnsafePath);
                    }
                    return Ok(path);
                }
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error.into()),
            }
        }
        Err(CatalogError::UnsafePath)
    }

    fn reconcile(&self) -> Result<(), CatalogError> {
        for entry in fs::read_dir(&self.path)? {
            let entry = entry?;
            let name = entry.file_name();
            if name == ".lock" || name == ".owner-v1" {
                continue;
            }
            let name_text = name.to_str().ok_or(CatalogError::UnsafePath)?;
            let id = name_text
                .strip_prefix("snapshot-")
                .ok_or(CatalogError::UnsafePath)?;
            if Uuid::parse_str(id).is_err() {
                return Err(CatalogError::UnsafePath);
            }
            remove_managed_snapshot_directory(&entry.path())?;
        }
        Ok(())
    }
}

fn remove_managed_snapshot_directory(path: &Path) -> Result<(), CatalogError> {
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.is_dir() || metadata.uid() != unsafe { libc::geteuid() } {
        return Err(CatalogError::UnsafePath);
    }
    for entry in fs::read_dir(path)? {
        let entry = entry?;
        let name = entry.file_name();
        if !matches!(
            name.to_str(),
            Some("state.sqlite" | "state.sqlite-wal" | "state.sqlite-shm")
        ) {
            return Err(CatalogError::UnsafePath);
        }
        let metadata = fs::symlink_metadata(entry.path())?;
        if metadata.file_type().is_symlink()
            || !metadata.is_file()
            || metadata.uid() != unsafe { libc::geteuid() }
        {
            return Err(CatalogError::UnsafePath);
        }
        fs::remove_file(entry.path())?;
    }
    fs::remove_dir(path)?;
    Ok(())
}

struct DatabaseFileGuards {
    main: GuardedDatabaseFile,
    wal: OptionalDatabaseFile,
    shm: OptionalDatabaseFile,
}

impl DatabaseFileGuards {
    fn open(path: &Path) -> Result<Self, CatalogError> {
        let path_text = path.to_str().ok_or(CatalogError::UnsafePath)?;
        Ok(Self {
            main: GuardedDatabaseFile::open(path.to_path_buf(), MAX_DATABASE_BYTES)?,
            wal: OptionalDatabaseFile::open(
                PathBuf::from(format!("{path_text}-wal")),
                MAX_DATABASE_WAL_BYTES,
            )?,
            shm: OptionalDatabaseFile::open(
                PathBuf::from(format!("{path_text}-shm")),
                MAX_DATABASE_SHM_BYTES,
            )?,
        })
    }

    fn verify(&self) -> Result<(), CatalogError> {
        self.main.verify()?;
        self.wal.verify()?;
        self.shm.verify()
    }

    fn snapshot(&self, manager: &ManagedSnapshotRoot) -> Result<DatabaseSnapshot, CatalogError> {
        self.verify()?;
        let directory = manager.create_snapshot_directory()?;
        let main_digest = self.main.copy_to(&directory.join("state.sqlite"))?;
        let wal_digest = self.wal.copy_to(&directory.join("state.sqlite-wal"))?;
        let snapshot = DatabaseSnapshot {
            directory,
            main_digest,
            wal_digest,
        };
        self.verify_snapshot(&snapshot)?;
        Ok(snapshot)
    }

    fn verify_snapshot(&self, snapshot: &DatabaseSnapshot) -> Result<(), CatalogError> {
        self.main.verify_digest(&snapshot.main_digest)?;
        self.wal.verify_digest(snapshot.wal_digest.as_ref())?;
        self.shm.verify()
    }
}

fn hash_guarded_file(
    file: &File,
    max_bytes: u64,
    mut destination: Option<&mut File>,
) -> Result<FileDigest, CatalogError> {
    let mut hasher = Sha256::new();
    let mut buffer = [0_u8; 64 * 1024];
    let mut offset = 0_u64;
    loop {
        let read = file.read_at(&mut buffer, offset)?;
        if read == 0 {
            break;
        }
        offset = offset
            .checked_add(read as u64)
            .ok_or(CatalogError::IndexTooLarge)?;
        if offset > max_bytes {
            return Err(CatalogError::IndexTooLarge);
        }
        hasher.update(&buffer[..read]);
        if let Some(output) = destination.as_deref_mut() {
            output.write_all(&buffer[..read])?;
        }
        #[cfg(test)]
        if destination.is_some()
            && offset >= 8 * 1024 * 1024
            && let Some(ready) = std::env::var_os("ECI_CATALOG_KILL_READY")
        {
            let mut ready = File::create(ready)?;
            ready.write_all(b"ready")?;
            ready.sync_all()?;
            loop {
                std::thread::park();
            }
        }
    }
    Ok(FileDigest {
        bytes: offset,
        sha256: hasher.finalize().into(),
    })
}

fn open_guarded_regular_file(path: &Path, max_bytes: u64) -> Result<File, CatalogError> {
    let metadata = fs::symlink_metadata(path)?;
    if metadata.file_type().is_symlink()
        || !metadata.is_file()
        || metadata.uid() != unsafe { libc::geteuid() }
    {
        return Err(CatalogError::UnsafePath);
    }
    if metadata.len() > max_bytes {
        return Err(CatalogError::IndexTooLarge);
    }
    let file = File::open(path)?;
    let opened = file.metadata()?;
    if !opened.is_file()
        || opened.uid() != unsafe { libc::geteuid() }
        || (opened.dev(), opened.ino()) != (metadata.dev(), metadata.ino())
    {
        return Err(CatalogError::UnsafePath);
    }
    Ok(file)
}

fn file_identity(file: &File) -> Result<(u64, u64), CatalogError> {
    let metadata = file.metadata()?;
    Ok((metadata.dev(), metadata.ino()))
}

fn ensure_path_identity(path: &Path, expected: (u64, u64)) -> Result<(), CatalogError> {
    ensure_path_identity_bounded(path, expected, u64::MAX)
}

fn ensure_path_identity_bounded(
    path: &Path,
    expected: (u64, u64),
    max_bytes: u64,
) -> Result<(), CatalogError> {
    let metadata = fs::symlink_metadata(path)?;
    if metadata.file_type().is_symlink()
        || !metadata.is_file()
        || metadata.uid() != unsafe { libc::geteuid() }
        || (metadata.dev(), metadata.ino()) != expected
    {
        return Err(CatalogError::UnsafePath);
    }
    if metadata.len() > max_bytes {
        return Err(CatalogError::IndexTooLarge);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::os::unix::fs::symlink;
    use std::os::unix::process::ExitStatusExt;
    use std::process::{Command, Stdio};

    use rusqlite::params;
    use serde_json::json;
    use tempfile::tempdir;

    use super::*;

    const PINNED_A: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
    const PINNED_B: &str = "019ebc56-375d-7f21-be14-14108703a18c";
    const RECENT_A: &str = "019f99c9-e12e-7e42-a261-2e17df6d65d3";
    const RECENT_B: &str = "019f99ad-c6d9-7f60-a9e6-fe6640f7fd2a";
    const ARCHIVED: &str = "019f1111-1111-7111-8111-111111111111";
    const INTERNAL: &str = "019fa68f-0000-7000-8000-000000000000";

    fn test_catalog(home: &Path) -> CodexTaskCatalog {
        let snapshot_root = home.join(".test-catalog-snapshots");
        crate::paths::secure_directory(&snapshot_root).unwrap();
        CodexTaskCatalog::from_paths(home.to_path_buf(), snapshot_root)
    }

    fn write_fixture(home: &Path) {
        fs::create_dir_all(home.join("sessions")).unwrap();
        fs::write(
            home.join(".codex-global-state.json"),
            serde_json::to_vec(&json!({
                "pinned-thread-ids": [PINNED_B, PINNED_A],
                "electron-workspace-root-labels": {"/work/heard": "Heard Fork"},
                "electron-persisted-atom-state": {
                    "unified-sidebar-pinned-order-v1": [
                        format!("codex:thread:local:{PINNED_A}"),
                        "codex:thread:local:client-new-thread:ignored",
                        format!("codex:thread:local:{PINNED_B}")
                    ]
                },
                "unrelated": {"secret": "must-not-be-returned"}
            }))
            .unwrap(),
        )
        .unwrap();
        let index = [
            json!({"id": PINNED_A, "thread_name": "Lark Heard"}),
            json!({"id": PINNED_B, "thread_name": "Lark CoSpace"}),
            json!({"id": RECENT_A, "thread_name": "Digital Service"}),
        ]
        .into_iter()
        .map(|row| serde_json::to_string(&row).unwrap())
        .collect::<Vec<_>>()
        .join("\n")
            + "\n";
        fs::write(home.join("session_index.jsonl"), index).unwrap();
        let connection = Connection::open(home.join("state_5.sqlite")).unwrap();
        connection
            .execute_batch(
                "CREATE TABLE threads (
                    id TEXT PRIMARY KEY,
                    name TEXT,
                    title TEXT NOT NULL,
                    cwd TEXT NOT NULL,
                    rollout_path TEXT NOT NULL,
                    updated_at INTEGER NOT NULL,
                    updated_at_ms INTEGER,
                    recency_at_ms INTEGER NOT NULL,
                    archived INTEGER NOT NULL,
                    source TEXT,
                    thread_source TEXT,
                    agent_role TEXT
                 );",
            )
            .unwrap();
        for (id, name, title, cwd, updated, archived) in [
            (PINNED_A, "db pinned a", "", "/work/heard", 100_i64, 0_i64),
            (PINNED_B, "db pinned b", "", "/work/cospace", 200, 0),
            (RECENT_A, "db recent a", "", "/work/digital", 400, 0),
            (RECENT_B, "", "Recent B", "/work/glasses", 300, 0),
            (ARCHIVED, "archived", "", "/work/archived", 900, 1),
            (
                INTERNAL,
                "The following is the Codex agent history whose request action you are assessing.",
                "",
                "/work/internal",
                1_000,
                0,
            ),
        ] {
            let rollout_path = home.join("sessions").join(format!("{id}.jsonl"));
            fs::write(&rollout_path, b"").unwrap();
            connection
                .execute(
                    "INSERT INTO threads
                     (id, name, title, cwd, rollout_path, updated_at, updated_at_ms,
                      recency_at_ms, archived, source, thread_source, agent_role)
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?7, ?8, 'vscode', 'user', NULL)",
                    params![
                        id,
                        name,
                        title,
                        cwd,
                        rollout_path.to_string_lossy(),
                        updated / 1_000,
                        updated,
                        archived
                    ],
                )
                .unwrap();
        }
    }

    #[test]
    fn pinned_desktop_order_precedes_bounded_recent_and_filters_archived_internal() {
        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let catalog = test_catalog(temp.path());

        let tasks = catalog.list_tasks().unwrap();

        assert_eq!(
            tasks
                .iter()
                .map(|task| task.task_id.as_str())
                .collect::<Vec<_>>(),
            [PINNED_A, PINNED_B, RECENT_A, RECENT_B]
        );
        assert_eq!(tasks[0].name, "Lark Heard");
        assert_eq!(tasks[0].project, "Heard Fork");
        assert_eq!(tasks[3].name, "Recent B");
        assert_eq!(
            tasks.iter().map(|task| task.pinned).collect::<Vec<_>>(),
            [true, true, false, false]
        );
        assert!(catalog.allowlisted(ARCHIVED).is_err());
        assert!(catalog.allowlisted(INTERNAL).is_err());
    }

    #[test]
    fn catalog_read_does_not_mutate_or_create_codex_index_files() {
        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let paths = [
            temp.path().join(".codex-global-state.json"),
            temp.path().join("session_index.jsonl"),
            temp.path().join("state_5.sqlite"),
        ];
        let catalog = test_catalog(temp.path());
        let before = paths
            .iter()
            .map(|path| fs::read(path).unwrap())
            .collect::<Vec<_>>();
        let entries_before = directory_entry_names(temp.path());

        catalog.list_tasks().unwrap();

        let after = paths
            .iter()
            .map(|path| fs::read(path).unwrap())
            .collect::<Vec<_>>();
        assert_eq!(after, before);
        assert_eq!(directory_entry_names(temp.path()), entries_before);
    }

    #[test]
    fn explicit_paths_keep_fixture_snapshots_out_of_an_unrelated_runtime() {
        let temp = tempdir().unwrap();
        let codex_home = temp.path().join("codex");
        let snapshot_root = temp.path().join("fixture-snapshots");
        let unrelated_runtime = temp.path().join("production-runtime-decoy");
        write_fixture(&codex_home);
        crate::paths::secure_directory(&snapshot_root).unwrap();
        crate::paths::secure_directory(&unrelated_runtime).unwrap();
        fs::write(unrelated_runtime.join("sentinel"), b"unchanged").unwrap();
        let before = directory_entry_names(&unrelated_runtime);

        CodexTaskCatalog::from_paths(codex_home, snapshot_root.clone())
            .list_tasks()
            .unwrap();

        assert_eq!(directory_entry_names(&unrelated_runtime), before);
        assert_eq!(
            directory_entry_names(&snapshot_root),
            BTreeSet::from([".lock".to_owned(), ".owner-v1".to_owned()])
        );
    }

    #[test]
    fn wal_catalog_read_never_changes_source_main_wal_or_shm() {
        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let connection = Connection::open(temp.path().join("state_5.sqlite")).unwrap();
        connection
            .execute_batch("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;")
            .unwrap();
        connection
            .execute(
                "UPDATE threads SET updated_at_ms = updated_at_ms + 1 WHERE id = ?1",
                [RECENT_A],
            )
            .unwrap();
        let paths = [
            temp.path().join("state_5.sqlite"),
            temp.path().join("state_5.sqlite-wal"),
            temp.path().join("state_5.sqlite-shm"),
        ];
        assert!(paths.iter().all(|path| path.is_file()));
        let catalog = test_catalog(temp.path());
        let before = paths
            .iter()
            .map(|path| fs::read(path).unwrap())
            .collect::<Vec<_>>();
        let entries_before = directory_entry_names(temp.path());

        catalog.list_tasks().unwrap();

        let after = paths
            .iter()
            .map(|path| fs::read(path).unwrap())
            .collect::<Vec<_>>();
        assert_eq!(after, before);
        assert_eq!(directory_entry_names(temp.path()), entries_before);
        drop(connection);
    }

    #[test]
    fn wal_without_source_shm_is_queried_via_snapshot_without_creating_one() {
        let origin = tempdir().unwrap();
        write_fixture(origin.path());
        let connection = Connection::open(origin.path().join("state_5.sqlite")).unwrap();
        connection
            .execute_batch("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;")
            .unwrap();
        connection
            .execute("UPDATE threads SET archived = 1", [])
            .unwrap();

        let target = tempdir().unwrap();
        write_fixture(target.path());
        fs::copy(
            origin.path().join("state_5.sqlite"),
            target.path().join("state_5.sqlite"),
        )
        .unwrap();
        fs::copy(
            origin.path().join("state_5.sqlite-wal"),
            target.path().join("state_5.sqlite-wal"),
        )
        .unwrap();
        let source_shm = target.path().join("state_5.sqlite-shm");
        assert!(!source_shm.exists());
        let catalog = test_catalog(target.path());
        let entries_before = directory_entry_names(target.path());
        let main_before = fs::read(target.path().join("state_5.sqlite")).unwrap();
        let wal_before = fs::read(target.path().join("state_5.sqlite-wal")).unwrap();

        let tasks = catalog.list_tasks().unwrap();

        assert!(tasks.is_empty());
        assert!(!source_shm.exists());
        assert_eq!(
            fs::read(target.path().join("state_5.sqlite")).unwrap(),
            main_before
        );
        assert_eq!(
            fs::read(target.path().join("state_5.sqlite-wal")).unwrap(),
            wal_before
        );
        assert_eq!(directory_entry_names(target.path()), entries_before);
        drop(connection);
    }

    #[test]
    fn database_snapshot_is_private_and_contains_only_main_and_optional_wal() {
        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let guards = DatabaseFileGuards::open(&temp.path().join("state_5.sqlite")).unwrap();
        let snapshot_root = temp.path().join("managed-snapshots");
        let manager = ManagedSnapshotRoot::acquire(&snapshot_root).unwrap();

        let snapshot = guards.snapshot(&manager).unwrap();

        assert_eq!(
            snapshot.directory.metadata().unwrap().permissions().mode() & 0o777,
            0o700
        );
        assert_eq!(
            snapshot
                .database_path()
                .metadata()
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o600
        );
        assert_eq!(
            directory_entry_names(&snapshot.directory),
            BTreeSet::from(["state.sqlite".to_owned()])
        );
        let snapshot_path = snapshot.directory.clone();
        drop(snapshot);
        assert!(!snapshot_path.exists());
    }

    #[test]
    fn snapshot_sigkill_subprocess_helper() {
        let Some(database) = std::env::var_os("ECI_CATALOG_KILL_DATABASE") else {
            return;
        };
        let snapshot_root =
            PathBuf::from(std::env::var_os("ECI_CATALOG_KILL_SNAPSHOT_ROOT").unwrap());
        let manager = ManagedSnapshotRoot::acquire(&snapshot_root).unwrap();
        let guards = DatabaseFileGuards::open(Path::new(&database)).unwrap();
        let _snapshot = guards.snapshot(&manager).unwrap();
    }

    #[test]
    fn next_catalog_start_removes_a_snapshot_interrupted_by_sigkill() {
        let temp = tempdir().unwrap();
        let database = temp.path().join("state_5.sqlite");
        let snapshot_root = temp.path().join("managed-snapshots");
        let ready = temp.path().join("kill-ready");
        File::create(&database)
            .unwrap()
            .set_len(16 * 1024 * 1024)
            .unwrap();
        let mut child = Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "codex_catalog::tests::snapshot_sigkill_subprocess_helper",
            ])
            .env("ECI_CATALOG_KILL_DATABASE", &database)
            .env("ECI_CATALOG_KILL_SNAPSHOT_ROOT", &snapshot_root)
            .env("ECI_CATALOG_KILL_READY", &ready)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(5);
        while !ready.exists() {
            if let Some(status) = child.try_wait().unwrap() {
                panic!("catalog kill helper exited before checkpoint: {status}");
            }
            if Instant::now() >= deadline {
                child.kill().unwrap();
                panic!("catalog kill helper did not reach copy checkpoint");
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        assert!(
            fs::read_dir(&snapshot_root)
                .unwrap()
                .filter_map(Result::ok)
                .any(|entry| entry.file_name().to_string_lossy().starts_with("snapshot-"))
        );
        assert_eq!(unsafe { libc::kill(child.id() as i32, libc::SIGKILL) }, 0);
        assert_eq!(child.wait().unwrap().signal(), Some(libc::SIGKILL));

        let manager = ManagedSnapshotRoot::acquire(&snapshot_root).unwrap();
        assert_eq!(
            directory_entry_names(&snapshot_root),
            BTreeSet::from([".lock".to_owned(), ".owner-v1".to_owned()])
        );
        drop(manager);
    }

    #[test]
    fn catalog_caps_pinned_and_recent_results() {
        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let connection = Connection::open(temp.path().join("state_5.sqlite")).unwrap();
        let mut pinned = Vec::new();
        let mut index = String::new();
        for position in 0..40_u128 {
            let task_id =
                Uuid::from_u128(0x1000_0000_0000_4000_8000_0000_0000_0000 + position).to_string();
            let rollout = temp
                .path()
                .join("sessions")
                .join(format!("{task_id}.jsonl"));
            fs::write(&rollout, b"").unwrap();
            connection
                .execute(
                    "INSERT INTO threads
                     (id, name, title, cwd, rollout_path, updated_at, updated_at_ms,
                      recency_at_ms, archived, source, thread_source, agent_role)
                     VALUES (?1, ?2, '', '/work/bounded', ?3, 1, ?4, ?4, 0,
                             'vscode', 'user', NULL)",
                    params![
                        task_id,
                        format!("Task {position}"),
                        rollout.to_string_lossy(),
                        10_000_i64 + position as i64
                    ],
                )
                .unwrap();
            index.push_str(
                &serde_json::to_string(&json!({
                    "id": task_id,
                    "thread_name": format!("Task {position}")
                }))
                .unwrap(),
            );
            index.push('\n');
            if position < 20 {
                pinned.push(task_id);
            }
        }
        drop(connection);
        fs::write(temp.path().join("session_index.jsonl"), index).unwrap();
        fs::write(
            temp.path().join(".codex-global-state.json"),
            serde_json::to_vec(&json!({
                "pinned-thread-ids": pinned,
                "electron-persisted-atom-state": {}
            }))
            .unwrap(),
        )
        .unwrap();

        let tasks = test_catalog(temp.path()).list_tasks().unwrap();

        assert_eq!(tasks.len(), MAX_PINNED_TASKS + MAX_RECENT_TASKS);
        assert_eq!(
            tasks.iter().filter(|task| task.pinned).count(),
            MAX_PINNED_TASKS
        );
        assert_eq!(
            tasks.iter().filter(|task| !task.pinned).count(),
            MAX_RECENT_TASKS
        );
    }

    #[test]
    fn pinned_lookup_is_independent_of_recent_window_and_invalid_pins_do_not_consume_quota() {
        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let connection = Connection::open(temp.path().join("state_5.sqlite")).unwrap();
        let missing = "019f0000-0000-7000-8000-000000000001";
        let mut pinned = vec![missing.to_owned(), INTERNAL.to_owned()];
        for position in 0..(MAX_PINNED_TASKS + MAX_RECENT_CANDIDATES + 4) {
            let task_id =
                Uuid::from_u128(0x2000_0000_0000_4000_8000_0000_0000_0000 + position as u128)
                    .to_string();
            let rollout = temp
                .path()
                .join("sessions")
                .join(format!("{task_id}.jsonl"));
            fs::write(&rollout, b"").unwrap();
            connection
                .execute(
                    "INSERT INTO threads
                     (id, name, title, cwd, rollout_path, updated_at, updated_at_ms,
                      recency_at_ms, archived, source, thread_source, agent_role)
                     VALUES (?1, ?2, '', '/work/pinned', ?3, 1, ?4, ?4, 0,
                             'vscode', 'user', NULL)",
                    params![
                        task_id,
                        format!("Pinned {position}"),
                        rollout.to_string_lossy(),
                        100_000_i64 + position as i64
                    ],
                )
                .unwrap();
            if position < MAX_PINNED_TASKS {
                pinned.push(task_id);
            }
        }
        drop(connection);
        fs::write(
            temp.path().join(".codex-global-state.json"),
            serde_json::to_vec(&json!({
                "pinned-thread-ids": pinned,
                "electron-persisted-atom-state": {}
            }))
            .unwrap(),
        )
        .unwrap();

        let tasks = test_catalog(temp.path()).list_tasks().unwrap();

        assert_eq!(
            tasks.iter().filter(|task| task.pinned).count(),
            MAX_PINNED_TASKS
        );
        assert!(!tasks.iter().any(|task| task.task_id == missing));
        assert!(!tasks.iter().any(|task| task.task_id == INTERNAL));
        assert!(
            tasks[..MAX_PINNED_TASKS]
                .iter()
                .all(|task| task.pinned && task.name.starts_with("Pinned "))
        );
    }

    #[test]
    fn structurally_identified_subagents_are_excluded_even_with_normal_titles() {
        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let connection = Connection::open(temp.path().join("state_5.sqlite")).unwrap();
        for (offset, source, thread_source, agent_role) in [
            (0_u128, "vscode", "subagent", ""),
            (1, r#"{"subagent":{"role":"worker"}}"#, "user", ""),
            (2, "vscode", "user", "reviewer"),
        ] {
            let task_id =
                Uuid::from_u128(0x3000_0000_0000_4000_8000_0000_0000_0000 + offset).to_string();
            let rollout = temp
                .path()
                .join("sessions")
                .join(format!("{task_id}.jsonl"));
            fs::write(&rollout, b"").unwrap();
            connection
                .execute(
                    "INSERT INTO threads
                     (id, name, title, cwd, rollout_path, updated_at, updated_at_ms,
                      recency_at_ms, archived, source, thread_source, agent_role)
                     VALUES (?1, 'Ordinary task title', '', '/work/user', ?2, 1,
                             ?3, ?3, 0, ?4, ?5, ?6)",
                    params![
                        task_id,
                        rollout.to_string_lossy(),
                        200_000_i64 + offset as i64,
                        source,
                        thread_source,
                        agent_role
                    ],
                )
                .unwrap();
        }
        drop(connection);

        let tasks = test_catalog(temp.path()).list_tasks().unwrap();

        assert!(!tasks.iter().any(|task| task.name == "Ordinary task title"));
    }

    #[test]
    fn oversized_database_files_and_sidecar_symlinks_fail_closed() {
        for suffix in ["", "-wal", "-shm"] {
            let temp = tempdir().unwrap();
            write_fixture(temp.path());
            let path = temp.path().join(format!("state_5.sqlite{suffix}"));
            let limit = match suffix {
                "" => MAX_DATABASE_BYTES,
                "-wal" => MAX_DATABASE_WAL_BYTES,
                "-shm" => MAX_DATABASE_SHM_BYTES,
                _ => unreachable!(),
            };
            File::create(&path).unwrap().set_len(limit + 1).unwrap();
            assert!(matches!(
                test_catalog(temp.path()).list_tasks(),
                Err(CatalogError::IndexTooLarge)
            ));
        }

        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let target = temp.path().join("other-wal");
        fs::write(&target, b"").unwrap();
        symlink(&target, temp.path().join("state_5.sqlite-wal")).unwrap();
        assert!(matches!(
            test_catalog(temp.path()).list_tasks(),
            Err(CatalogError::UnsafePath)
        ));
    }

    #[test]
    fn database_guard_detects_sidecar_creation_and_replacement() {
        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let database = temp.path().join("state_5.sqlite");
        let guards = DatabaseFileGuards::open(&database).unwrap();
        fs::write(temp.path().join("state_5.sqlite-wal"), b"late").unwrap();
        assert!(matches!(guards.verify(), Err(CatalogError::UnsafePath)));

        fs::remove_file(temp.path().join("state_5.sqlite-wal")).unwrap();
        fs::write(temp.path().join("state_5.sqlite-wal"), b"first").unwrap();
        let guards = DatabaseFileGuards::open(&database).unwrap();
        fs::remove_file(temp.path().join("state_5.sqlite-wal")).unwrap();
        fs::write(temp.path().join("state_5.sqlite-wal"), b"replacement").unwrap();
        assert!(matches!(guards.verify(), Err(CatalogError::UnsafePath)));
    }

    #[test]
    fn oversized_database_fields_are_not_materialized_into_catalog() {
        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let connection = Connection::open(temp.path().join("state_5.sqlite")).unwrap();
        connection
            .execute(
                "UPDATE threads SET source = ?1 WHERE id = ?2",
                params!["x".repeat(8 * 1024 + 1), RECENT_A],
            )
            .unwrap();
        connection
            .execute(
                "UPDATE threads SET name = ?1, title = ?1 WHERE id = ?2",
                params!["n".repeat(4 * 1024 + 1), RECENT_B],
            )
            .unwrap();
        drop(connection);

        let tasks = test_catalog(temp.path()).list_tasks().unwrap();

        assert!(!tasks.iter().any(|task| task.task_id == RECENT_A));
        assert!(!tasks.iter().any(|task| task.task_id == RECENT_B));
    }

    #[test]
    fn database_query_budget_interrupts_an_unindexed_large_fixture() {
        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let mut connection = Connection::open(temp.path().join("state_5.sqlite")).unwrap();
        let transaction = connection.transaction().unwrap();
        for position in 0..3_000_u128 {
            let task_id =
                Uuid::from_u128(0x4000_0000_0000_4000_8000_0000_0000_0000 + position).to_string();
            transaction
                .execute(
                    "INSERT INTO threads
                     (id, name, title, cwd, rollout_path, updated_at, updated_at_ms,
                      recency_at_ms, archived, source, thread_source, agent_role)
                     VALUES (?1, 'Load row', '', '/work/load', '/missing', 1, ?2, ?2, 0,
                             'vscode', 'user', NULL)",
                    params![task_id, position as i64],
                )
                .unwrap();
        }
        transaction.commit().unwrap();
        drop(connection);

        assert!(matches!(
            read_database_with_budget(
                &temp.path().join("state_5.sqlite"),
                &[],
                &temp.path().join("managed-snapshots"),
                0,
                Duration::from_secs(10)
            ),
            Err(CatalogError::InvalidIndex)
        ));
    }

    #[test]
    fn malformed_oversized_unknown_schema_and_symlink_indexes_fail_closed() {
        let temp = tempdir().unwrap();
        write_fixture(temp.path());
        let catalog = test_catalog(temp.path());
        fs::write(temp.path().join("session_index.jsonl"), b"not-json\n").unwrap();
        assert!(matches!(
            catalog.list_tasks(),
            Err(CatalogError::InvalidIndex)
        ));

        write_fixture_fresh_index(temp.path());
        fs::write(
            temp.path().join(".codex-global-state.json"),
            vec![b'x'; MAX_GLOBAL_STATE_BYTES as usize + 1],
        )
        .unwrap();
        assert!(matches!(
            catalog.list_tasks(),
            Err(CatalogError::IndexTooLarge)
        ));

        let other = temp.path().join("other-state.json");
        fs::write(&other, b"{}").unwrap();
        fs::remove_file(temp.path().join(".codex-global-state.json")).unwrap();
        symlink(other, temp.path().join(".codex-global-state.json")).unwrap();
        assert!(matches!(
            catalog.list_tasks(),
            Err(CatalogError::UnsafePath)
        ));

        fs::remove_file(temp.path().join(".codex-global-state.json")).unwrap();
        write_global_state(temp.path());
        let connection = Connection::open(temp.path().join("state_5.sqlite")).unwrap();
        connection
            .execute("ALTER TABLE threads RENAME TO incompatible", [])
            .unwrap();
        drop(connection);
        assert!(matches!(
            catalog.list_tasks(),
            Err(CatalogError::InvalidIndex)
        ));
    }

    fn write_fixture_fresh_index(home: &Path) {
        fs::write(
            home.join("session_index.jsonl"),
            format!("{{\"id\":\"{PINNED_A}\",\"thread_name\":\"Pinned\"}}\n"),
        )
        .unwrap();
    }

    fn write_global_state(home: &Path) {
        fs::write(
            home.join(".codex-global-state.json"),
            serde_json::to_vec(&json!({
                "pinned-thread-ids": [],
                "electron-persisted-atom-state": {}
            }))
            .unwrap(),
        )
        .unwrap();
    }

    fn directory_entry_names(path: &Path) -> BTreeSet<String> {
        fs::read_dir(path)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .collect()
    }
}

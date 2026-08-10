use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::ffi::OsString;
use std::fs::File;
use std::io::{self, BufRead, BufReader, Seek, SeekFrom};
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use thiserror::Error;
use uuid::Uuid;
use zeroize::Zeroizing;

use crate::codex_catalog::{CatalogError, CodexTask, CodexTaskCatalog};
use crate::paths::{open_file_at, open_owned_directory_chain};
use crate::store::{
    Binding, CompletionOutcome, ObserverStateStore, RolloutCursor, StateStore, StoreError,
};

const MAX_ROLLOUT_LINE_BYTES: usize = 256 * 1024;
const MAX_SCAN_BYTES_PER_POLL: usize = 4 * 1024 * 1024;
const ROLLOUT_ANCHOR_BYTES: usize = 4 * 1024;
const MAX_TEXT_BYTES: usize = 12 * 1024;
const MAX_TEXT_BYTES_PER_ROLE: usize = 22 * 1024;
const MAX_RAW_MESSAGES_PER_ROLE: usize = 32;
const MAX_MESSAGES_PER_ROLE: usize = 8;
const MAX_TOOLS: usize = 64;
const MAX_TOOL_NAME_BYTES: usize = 128;
const MAX_TOOL_STATUS_BYTES: usize = 64;
const MAX_SERIALIZED_TURN_PACK_BYTES: usize = 64 * 1024;
const DEFAULT_POLL_INTERVAL: Duration = Duration::from_secs(1);

#[derive(Debug, Error)]
pub enum ObserverError {
    #[error("Codex catalog failed")]
    Catalog(#[from] CatalogError),
    #[error("Host state failed")]
    Store(#[from] StoreError),
    #[error("rollout I/O failed")]
    Io(#[from] io::Error),
    #[error("rollout path is unsafe or changed while being read")]
    UnsafePath,
    #[error("rollout is malformed or belongs to another task")]
    InvalidRollout,
    #[error("rollout line or poll window exceeded its fixed bound")]
    RolloutTooLarge,
    #[error("turn pack exceeded its fixed bound")]
    TurnPackTooLarge,
    #[error("rollout generation overflowed")]
    GenerationOverflow,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ToolFact {
    pub name: String,
    pub status: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TurnPack {
    pub v: u8,
    pub turn_id: String,
    pub user: Vec<String>,
    pub assistant: Vec<String>,
    pub tools: Vec<ToolFact>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ObservedCompletion {
    pub completion_id: String,
    pub outcome: CompletionOutcome,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ObserverTick {
    pub inserted: usize,
    pub replayed: usize,
    pub failed_tasks: usize,
}

pub struct RolloutObserver {
    catalog: CodexTaskCatalog,
    poll_interval: Duration,
    next_poll: Instant,
    pending: BTreeMap<String, PendingScan>,
    #[cfg(test)]
    before_trust_check: Option<TrustCheckHook>,
    #[cfg(test)]
    before_catalog_poll: Option<Box<dyn FnOnce() + Send>>,
    #[cfg(test)]
    verification_read_sizes: Vec<usize>,
}

#[cfg(test)]
type TrustCheckHook = Box<dyn FnOnce(&Path) + Send>;

struct PendingScan {
    expected: Option<RolloutCursor>,
    rollout_path: PathBuf,
    device: u64,
    inode: u64,
    generation: u64,
    offset: u64,
    anchor: [u8; 32],
    trusted_offset: u64,
    trusted_anchor: [u8; 32],
    trusted_prefix_len: usize,
    trusted_prefix_anchor: [u8; 32],
    trusted_range_digest: Sha256,
    active: Option<TurnBuilder>,
    streaming_line: Option<StreamingLine>,
    verification: Option<CompletionVerification>,
}

struct StreamingLine {
    start_offset: u64,
    parser: StreamingRecordParser,
}

struct CompletionVerification {
    start_offset: u64,
    end_offset: u64,
    offset: u64,
    digest: Sha256,
    expected_digest: [u8; 32],
    completion_id: String,
    turn_pack: String,
}

impl PendingScan {
    fn validate_trusted_range(&self, rollout: &GuardedRollout) -> Result<(), ObserverError> {
        if rollout.anchor_at(self.trusted_offset)? != self.trusted_anchor
            || rollout.hash_range(self.trusted_offset, self.trusted_prefix_len)?
                != self.trusted_prefix_anchor
        {
            return Err(ObserverError::InvalidRollout);
        }
        Ok(())
    }

    fn trust_from(&mut self, rollout: &GuardedRollout, offset: u64) -> Result<(), ObserverError> {
        let size = rollout.file.metadata()?.len();
        self.trusted_offset = offset;
        self.trusted_anchor = rollout.anchor_at(offset)?;
        self.trusted_prefix_len =
            usize::try_from(size.saturating_sub(offset).min(ROLLOUT_ANCHOR_BYTES as u64))
                .map_err(|_| ObserverError::RolloutTooLarge)?;
        self.trusted_prefix_anchor = rollout.hash_range(offset, self.trusted_prefix_len)?;
        self.trusted_range_digest = Sha256::new();
        Ok(())
    }

    fn start_verification(
        &mut self,
        offset: u64,
        completion_id: String,
        turn_pack: String,
    ) -> Result<(), ObserverError> {
        if offset < self.trusted_offset || self.verification.is_some() {
            return Err(ObserverError::InvalidRollout);
        }
        self.verification = Some(CompletionVerification {
            start_offset: self.trusted_offset,
            end_offset: offset,
            offset: self.trusted_offset,
            digest: Sha256::new(),
            expected_digest: self.trusted_range_digest.clone().finalize().into(),
            completion_id,
            turn_pack,
        });
        Ok(())
    }
}

impl CompletionVerification {
    fn advance(&mut self, rollout: &GuardedRollout, budget: usize) -> Result<usize, ObserverError> {
        use std::os::unix::fs::FileExt;

        if self.offset < self.start_offset || self.offset > self.end_offset {
            return Err(ObserverError::InvalidRollout);
        }
        let remaining = usize::try_from(self.end_offset - self.offset)
            .map_err(|_| ObserverError::RolloutTooLarge)?;
        let wanted = remaining.min(budget);
        let mut buffer = [0_u8; ROLLOUT_ANCHOR_BYTES];
        let mut read = 0_usize;
        while read < wanted {
            let chunk = (wanted - read).min(buffer.len());
            let count = rollout
                .file
                .read_at(&mut buffer[..chunk], self.offset + read as u64)?;
            if count == 0 {
                return Err(ObserverError::InvalidRollout);
            }
            self.digest.update(&buffer[..count]);
            read += count;
        }
        self.offset = self
            .offset
            .checked_add(read as u64)
            .ok_or(ObserverError::RolloutTooLarge)?;
        Ok(read)
    }

    fn is_complete(&self) -> bool {
        self.offset == self.end_offset
    }

    fn validate_digest(&self) -> Result<(), ObserverError> {
        if !self.is_complete()
            || <[u8; 32]>::from(self.digest.clone().finalize()) != self.expected_digest
        {
            return Err(ObserverError::InvalidRollout);
        }
        Ok(())
    }
}

trait RolloutStateAccess {
    fn bindings(&self) -> Result<Vec<Binding>, StoreError>;
    fn rollout_cursor(&self, task_id: &str) -> Result<Option<RolloutCursor>, StoreError>;
    fn commit_rollout_completion(
        &mut self,
        binding: Option<&Binding>,
        expected: Option<&RolloutCursor>,
        next: &RolloutCursor,
        completion_id: &str,
        turn_pack: &str,
    ) -> Result<CompletionOutcome, StoreError>;
}

impl RolloutStateAccess for StateStore {
    fn bindings(&self) -> Result<Vec<Binding>, StoreError> {
        StateStore::bindings(self)
    }

    fn rollout_cursor(&self, task_id: &str) -> Result<Option<RolloutCursor>, StoreError> {
        StateStore::rollout_cursor(self, task_id)
    }

    fn commit_rollout_completion(
        &mut self,
        binding: Option<&Binding>,
        expected: Option<&RolloutCursor>,
        next: &RolloutCursor,
        completion_id: &str,
        turn_pack: &str,
    ) -> Result<CompletionOutcome, StoreError> {
        match binding {
            Some(binding) => StateStore::commit_bound_rollout_completion(
                self,
                binding,
                expected,
                next,
                completion_id,
                turn_pack,
            ),
            None => StateStore::commit_rollout_completion(
                self,
                expected,
                next,
                completion_id,
                turn_pack,
            ),
        }
    }
}

impl RolloutStateAccess for ObserverStateStore {
    fn bindings(&self) -> Result<Vec<Binding>, StoreError> {
        ObserverStateStore::bindings(self)
    }

    fn rollout_cursor(&self, task_id: &str) -> Result<Option<RolloutCursor>, StoreError> {
        ObserverStateStore::rollout_cursor(self, task_id)
    }

    fn commit_rollout_completion(
        &mut self,
        binding: Option<&Binding>,
        expected: Option<&RolloutCursor>,
        next: &RolloutCursor,
        completion_id: &str,
        turn_pack: &str,
    ) -> Result<CompletionOutcome, StoreError> {
        match binding {
            Some(binding) => ObserverStateStore::commit_bound_rollout_completion(
                self,
                binding,
                expected,
                next,
                completion_id,
                turn_pack,
            ),
            None => ObserverStateStore::commit_rollout_completion(
                self,
                expected,
                next,
                completion_id,
                turn_pack,
            ),
        }
    }
}

impl RolloutObserver {
    pub fn new(catalog: CodexTaskCatalog) -> Self {
        Self {
            catalog,
            poll_interval: DEFAULT_POLL_INTERVAL,
            next_poll: Instant::now(),
            pending: BTreeMap::new(),
            #[cfg(test)]
            before_trust_check: None,
            #[cfg(test)]
            before_catalog_poll: None,
            #[cfg(test)]
            verification_read_sizes: Vec::new(),
        }
    }

    pub fn from_environment() -> Result<Self, ObserverError> {
        Ok(Self::new(CodexTaskCatalog::from_environment()?))
    }

    #[cfg(test)]
    pub(crate) fn set_before_catalog_poll(&mut self, hook: Box<dyn FnOnce() + Send>) {
        self.before_catalog_poll = Some(hook);
    }

    pub fn tick_due(&mut self, store: &mut StateStore) -> Result<ObserverTick, StoreError> {
        self.tick_due_with(store)
    }

    pub(crate) fn tick_due_worker(
        &mut self,
        store: &mut ObserverStateStore,
    ) -> Result<ObserverTick, StoreError> {
        self.tick_due_with(store)
    }

    fn tick_due_with<S: RolloutStateAccess>(
        &mut self,
        store: &mut S,
    ) -> Result<ObserverTick, StoreError> {
        if Instant::now() < self.next_poll {
            return Ok(ObserverTick::default());
        }
        self.next_poll = Instant::now() + self.poll_interval;
        self.poll_bound_tasks_with(store)
    }

    pub fn poll_bound_tasks(&mut self, store: &mut StateStore) -> Result<ObserverTick, StoreError> {
        self.poll_bound_tasks_with(store)
    }

    fn poll_bound_tasks_with<S: RolloutStateAccess>(
        &mut self,
        store: &mut S,
    ) -> Result<ObserverTick, StoreError> {
        let bindings = store.bindings()?;
        let bound_tasks = bindings
            .iter()
            .map(|binding| binding.task_id.as_str())
            .collect::<BTreeSet<_>>();
        self.pending
            .retain(|task_id, _| bound_tasks.contains(task_id.as_str()));
        if bindings.is_empty() {
            return Ok(ObserverTick::default());
        }
        #[cfg(test)]
        if let Some(hook) = self.before_catalog_poll.take() {
            hook();
        }
        let tasks = match self.catalog.list_tasks() {
            Ok(tasks) => tasks,
            Err(_) => {
                return Ok(ObserverTick {
                    failed_tasks: bindings
                        .iter()
                        .map(|binding| &binding.task_id)
                        .collect::<BTreeSet<_>>()
                        .len(),
                    ..ObserverTick::default()
                });
            }
        };
        let mut seen = BTreeSet::new();
        let mut tick = ObserverTick::default();
        for binding in bindings {
            if !seen.insert(binding.task_id.clone()) {
                continue;
            }
            let Some(task) = tasks.iter().find(|task| task.task_id == binding.task_id) else {
                tick.failed_tasks += 1;
                continue;
            };
            match self.poll_task_with(store, task, Some(&binding)) {
                Ok(completions) => {
                    for completion in completions {
                        match completion.outcome {
                            CompletionOutcome::Inserted => tick.inserted += 1,
                            CompletionOutcome::Replay => tick.replayed += 1,
                        }
                    }
                }
                Err(_) => tick.failed_tasks += 1,
            }
        }
        Ok(tick)
    }

    pub fn poll_task(
        &mut self,
        store: &mut StateStore,
        task: &CodexTask,
    ) -> Result<Vec<ObservedCompletion>, ObserverError> {
        self.poll_task_with(store, task, None)
    }

    fn poll_task_with<S: RolloutStateAccess>(
        &mut self,
        store: &mut S,
        task: &CodexTask,
        binding: Option<&Binding>,
    ) -> Result<Vec<ObservedCompletion>, ObserverError> {
        if Uuid::parse_str(&task.task_id).is_err() {
            return Err(ObserverError::InvalidRollout);
        }
        let rollout = GuardedRollout::open(&task.rollout_path)?;
        rollout.validate_session_meta(&task.task_id)?;
        let stored = store.rollout_cursor(&task.task_id)?;
        let pending = self.pending.remove(&task.task_id);
        let mut scan = if let Some(pending) = pending {
            let reusable = pending.expected == stored
                && pending.rollout_path == task.rollout_path
                && pending.device == rollout.device
                && pending.inode == rollout.inode
                && pending.offset <= rollout.size
                && (pending.streaming_line.is_some()
                    || pending.verification.is_some()
                    || rollout.validate_line_boundary(pending.offset).is_ok())
                && rollout.anchor_at(pending.offset)? == pending.anchor
                && pending.validate_trusted_range(&rollout).is_ok();
            if reusable {
                pending
            } else {
                fresh_scan(&rollout, task, stored.clone())?
            }
        } else {
            fresh_scan(&rollout, task, stored.clone())?
        };

        let mut completions = Vec::new();
        let mut scanned = 0_usize;
        if let Some(verification) = scan.verification.as_mut() {
            let read = verification.advance(&rollout, MAX_SCAN_BYTES_PER_POLL)?;
            scanned = scanned
                .checked_add(read)
                .ok_or(ObserverError::RolloutTooLarge)?;
            #[cfg(test)]
            self.verification_read_sizes.push(read);
            if verification.is_complete() {
                scan.validate_trusted_range(&rollout)?;
                rollout.verify_path_identity()?;
                completions.push(commit_verified_completion(
                    store, binding, &mut scan, &rollout, task,
                )?);
            } else {
                scan.validate_trusted_range(&rollout)?;
                rollout.verify_path_identity()?;
                self.pending.insert(task.task_id.clone(), scan);
                return Ok(completions);
            }
        }

        let mut reader = BufReader::new(&rollout.file);
        reader.seek(SeekFrom::Start(scan.offset))?;
        let mut offset = scan.offset;
        let mut line = Vec::new();
        let mut line_start = scan
            .streaming_line
            .as_ref()
            .map_or(offset, |streaming| streaming.start_offset);
        let mut streaming_line = scan.streaming_line.take();
        let mut active = scan.active.take();
        let mut incomplete_line = false;
        while scanned < MAX_SCAN_BYTES_PER_POLL {
            let available = reader.fill_buf()?;
            if available.is_empty() {
                incomplete_line = streaming_line.is_some() || !line.is_empty();
                break;
            }
            let remaining = MAX_SCAN_BYTES_PER_POLL - scanned;
            let available = &available[..available.len().min(remaining)];
            let newline = available.iter().position(|byte| *byte == b'\n');
            let consumed = newline.map_or(available.len(), |position| position + 1);
            let record_bytes = &available[..newline.unwrap_or(consumed)];
            if let Some(streaming) = streaming_line.as_mut() {
                streaming.parser.feed(record_bytes)?;
                scan.trusted_range_digest.update(record_bytes);
            } else if line.len().saturating_add(record_bytes.len()) <= MAX_ROLLOUT_LINE_BYTES {
                line.extend_from_slice(record_bytes);
            } else {
                let mut parser = StreamingRecordParser::new();
                parser.feed(&line)?;
                parser.feed(record_bytes)?;
                scan.trusted_range_digest.update(&line);
                scan.trusted_range_digest.update(record_bytes);
                line.clear();
                streaming_line = Some(StreamingLine {
                    start_offset: line_start,
                    parser,
                });
            }
            reader.consume(consumed);
            scanned = scanned
                .checked_add(consumed)
                .ok_or(ObserverError::RolloutTooLarge)?;
            offset = offset
                .checked_add(consumed as u64)
                .ok_or(ObserverError::RolloutTooLarge)?;
            let Some(_) = newline else {
                if scanned == MAX_SCAN_BYTES_PER_POLL {
                    incomplete_line = true;
                    break;
                }
                continue;
            };
            if streaming_line.is_some() {
                scan.trusted_range_digest.update(b"\n");
            } else {
                scan.trusted_range_digest.update(&line);
                scan.trusted_range_digest.update(b"\n");
            }
            let value = if let Some(streaming) = streaming_line.take() {
                streaming.parser.finish()?
            } else {
                if line.last() == Some(&b'\r') {
                    line.pop();
                }
                if line.is_empty() {
                    return Err(ObserverError::InvalidRollout);
                }
                serde_json::from_slice(&line).map_err(|_| ObserverError::InvalidRollout)?
            };
            line.clear();
            line_start = offset;
            if let Some((turn_id, serialized)) = apply_rollout_value(value, &mut active)? {
                rollout.verify_path_identity()?;
                #[cfg(test)]
                if let Some(hook) = self.before_trust_check.take() {
                    hook(&task.rollout_path);
                }
                scan.validate_trusted_range(&rollout)?;
                scan.start_verification(offset, turn_id, serialized)?;
                let remaining = MAX_SCAN_BYTES_PER_POLL - scanned;
                let verification = scan
                    .verification
                    .as_mut()
                    .expect("completion starts verification");
                let read = verification.advance(&rollout, remaining)?;
                scanned = scanned
                    .checked_add(read)
                    .ok_or(ObserverError::RolloutTooLarge)?;
                #[cfg(test)]
                self.verification_read_sizes.push(read);
                if verification.is_complete() {
                    completions.push(commit_verified_completion(
                        store, binding, &mut scan, &rollout, task,
                    )?);
                } else {
                    scan.offset = offset;
                    scan.anchor = rollout.anchor_at(offset)?;
                    scan.active = active;
                    scan.streaming_line = streaming_line;
                    scan.validate_trusted_range(&rollout)?;
                    rollout.verify_path_identity()?;
                    self.pending.insert(task.task_id.clone(), scan);
                    return Ok(completions);
                }
            }
        }
        if incomplete_line && streaming_line.is_none() {
            offset = line_start;
        }
        rollout.verify_path_identity()?;
        let committed_offset = scan
            .expected
            .as_ref()
            .filter(|cursor| {
                cursor.rollout_path == task.rollout_path
                    && cursor.device == rollout.device
                    && cursor.inode == rollout.inode
                    && cursor.generation == scan.generation
            })
            .map_or(0, |cursor| cursor.offset);
        if active.is_some()
            || offset != committed_offset
            || incomplete_line
            || streaming_line.is_some()
            || scan.verification.is_some()
        {
            scan.validate_trusted_range(&rollout)?;
            scan.offset = offset;
            scan.anchor = rollout.anchor_at(offset)?;
            scan.active = active;
            scan.streaming_line = streaming_line;
            self.pending.insert(task.task_id.clone(), scan);
        }
        Ok(completions)
    }
}

fn commit_verified_completion<S: RolloutStateAccess>(
    store: &mut S,
    binding: Option<&Binding>,
    scan: &mut PendingScan,
    rollout: &GuardedRollout,
    task: &CodexTask,
) -> Result<ObservedCompletion, ObserverError> {
    let verification = scan
        .verification
        .take()
        .ok_or(ObserverError::InvalidRollout)?;
    verification.validate_digest()?;
    let offset = verification.end_offset;
    let next = RolloutCursor {
        task_id: task.task_id.clone(),
        rollout_path: task.rollout_path.clone(),
        device: rollout.device,
        inode: rollout.inode,
        offset,
        generation: scan.generation,
        anchor: rollout.anchor_at(offset)?,
    };
    let outcome = store.commit_rollout_completion(
        binding,
        scan.expected.as_ref(),
        &next,
        &verification.completion_id,
        &verification.turn_pack,
    )?;
    scan.expected = Some(next);
    scan.offset = offset;
    scan.anchor = rollout.anchor_at(offset)?;
    scan.trust_from(rollout, offset)?;
    Ok(ObservedCompletion {
        completion_id: verification.completion_id,
        outcome,
    })
}

fn apply_rollout_value(
    value: serde_json::Value,
    active: &mut Option<TurnBuilder>,
) -> Result<Option<(String, String)>, ObserverError> {
    let object = value.as_object().ok_or(ObserverError::InvalidRollout)?;
    match object.get("type").and_then(serde_json::Value::as_str) {
        Some("event_msg") => {
            let payload = object
                .get("payload")
                .and_then(serde_json::Value::as_object)
                .ok_or(ObserverError::InvalidRollout)?;
            match payload.get("type").and_then(serde_json::Value::as_str) {
                Some("task_started") => {
                    let turn_id = required_turn_id(payload)?;
                    // A later authoritative turn may supersede an abandoned incomplete one.
                    *active = Some(TurnBuilder::new(turn_id));
                }
                Some("task_complete") => {
                    let turn_id = required_turn_id(payload)?;
                    let mut builder = active.take().ok_or(ObserverError::InvalidRollout)?;
                    if builder.turn_id != turn_id {
                        return Err(ObserverError::InvalidRollout);
                    }
                    let last = payload
                        .get("last_agent_message")
                        .and_then(serde_json::Value::as_str)
                        .filter(|last| !last.is_empty())
                        .ok_or(ObserverError::InvalidRollout)?;
                    // task_complete carries the authoritative final answer.
                    // Discard commentary/progress messages observed earlier in the turn.
                    builder.assistant.clear();
                    builder.push_message("assistant", last)?;
                    let serialized = serde_json::to_string(&builder.finish()?)
                        .map_err(|_| ObserverError::InvalidRollout)?;
                    if serialized.len() > MAX_SERIALIZED_TURN_PACK_BYTES {
                        return Err(ObserverError::TurnPackTooLarge);
                    }
                    return Ok(Some((turn_id, serialized)));
                }
                _ => {}
            }
        }
        Some("response_item") => {
            if let Some(builder) = active.as_mut() {
                builder.observe_response_item(
                    object.get("payload").ok_or(ObserverError::InvalidRollout)?,
                )?;
            }
        }
        _ => {}
    }
    Ok(None)
}

fn fresh_scan(
    rollout: &GuardedRollout,
    task: &CodexTask,
    stored: Option<RolloutCursor>,
) -> Result<PendingScan, ObserverError> {
    let same_identity = stored.as_ref().is_some_and(|cursor| {
        cursor.rollout_path == task.rollout_path
            && cursor.device == rollout.device
            && cursor.inode == rollout.inode
            && cursor.offset <= rollout.size
    });
    let same_stream = if same_identity {
        let cursor = stored.as_ref().expect("same identity requires a cursor");
        rollout.validate_line_boundary(cursor.offset).is_ok()
            && rollout.anchor_at(cursor.offset)? == cursor.anchor
    } else {
        false
    };
    let (offset, generation) = if same_stream {
        let cursor = stored.as_ref().expect("same stream requires a cursor");
        (cursor.offset, cursor.generation)
    } else {
        let generation = match stored.as_ref() {
            Some(cursor) => cursor
                .generation
                .checked_add(1)
                .ok_or(ObserverError::GenerationOverflow)?,
            None => 1,
        };
        (0, generation)
    };
    let mut scan = PendingScan {
        expected: stored,
        rollout_path: task.rollout_path.clone(),
        device: rollout.device,
        inode: rollout.inode,
        generation,
        offset,
        anchor: rollout.anchor_at(offset)?,
        trusted_offset: offset,
        trusted_anchor: rollout.anchor_at(offset)?,
        trusted_prefix_len: 0,
        trusted_prefix_anchor: Sha256::digest([]).into(),
        trusted_range_digest: Sha256::new(),
        active: None,
        streaming_line: None,
        verification: None,
    };
    scan.trust_from(rollout, offset)?;
    Ok(scan)
}

struct GuardedRollout {
    parent: File,
    name: OsString,
    file: File,
    device: u64,
    inode: u64,
    size: u64,
}

impl GuardedRollout {
    fn open(path: &Path) -> Result<Self, ObserverError> {
        if !path.is_absolute()
            || path
                .components()
                .any(|component| matches!(component, std::path::Component::ParentDir))
        {
            return Err(ObserverError::UnsafePath);
        }
        let parent_path = path.parent().ok_or(ObserverError::UnsafePath)?;
        let name = path
            .file_name()
            .filter(|name| !name.is_empty())
            .ok_or(ObserverError::UnsafePath)?
            .to_owned();
        let parent = open_owned_directory_chain(parent_path, false)
            .map_err(|_| ObserverError::UnsafePath)?;
        let file = open_file_at(&parent, &name, false).map_err(|_| ObserverError::UnsafePath)?;
        let metadata = file.metadata()?;
        if !metadata.is_file() || metadata.uid() != unsafe { libc::geteuid() } {
            return Err(ObserverError::UnsafePath);
        }
        Ok(Self {
            parent,
            name,
            device: metadata.dev(),
            inode: metadata.ino(),
            size: metadata.len(),
            file,
        })
    }

    fn verify_path_identity(&self) -> Result<(), ObserverError> {
        let current =
            open_file_at(&self.parent, &self.name, false).map_err(|_| ObserverError::UnsafePath)?;
        let metadata = current.metadata()?;
        if !metadata.is_file()
            || metadata.uid() != unsafe { libc::geteuid() }
            || metadata.dev() != self.device
            || metadata.ino() != self.inode
        {
            return Err(ObserverError::UnsafePath);
        }
        Ok(())
    }

    fn validate_session_meta(&self, task_id: &str) -> Result<(), ObserverError> {
        let file =
            open_file_at(&self.parent, &self.name, false).map_err(|_| ObserverError::UnsafePath)?;
        let metadata = file.metadata()?;
        if metadata.dev() != self.device || metadata.ino() != self.inode {
            return Err(ObserverError::UnsafePath);
        }
        let mut reader = BufReader::new(file);
        let mut line = Vec::new();
        let consumed = read_bounded_line(&mut reader, &mut line, MAX_ROLLOUT_LINE_BYTES)?;
        if consumed == 0 || line.last() != Some(&b'\n') {
            return Err(ObserverError::InvalidRollout);
        }
        line.pop();
        if line.last() == Some(&b'\r') {
            line.pop();
        }
        let value: serde_json::Value =
            serde_json::from_slice(&line).map_err(|_| ObserverError::InvalidRollout)?;
        let valid = value.get("type").and_then(serde_json::Value::as_str) == Some("session_meta")
            && value
                .get("payload")
                .and_then(|payload| payload.get("id"))
                .and_then(serde_json::Value::as_str)
                == Some(task_id);
        if !valid {
            return Err(ObserverError::InvalidRollout);
        }
        self.verify_path_identity()
    }

    fn validate_line_boundary(&self, offset: u64) -> Result<(), ObserverError> {
        if offset == 0 {
            return Ok(());
        }
        use std::os::unix::fs::FileExt;

        let mut byte = [0_u8; 1];
        if self.file.read_at(&mut byte, offset - 1)? != 1 || byte[0] != b'\n' {
            return Err(ObserverError::InvalidRollout);
        }
        Ok(())
    }

    fn anchor_at(&self, offset: u64) -> Result<[u8; 32], ObserverError> {
        use std::os::unix::fs::FileExt;

        let metadata = self.file.metadata()?;
        if offset > metadata.len() {
            return Err(ObserverError::InvalidRollout);
        }
        let start = offset.saturating_sub(ROLLOUT_ANCHOR_BYTES as u64);
        let mut bytes = vec![0_u8; (offset - start) as usize];
        let mut read = 0_usize;
        while read < bytes.len() {
            let count = self.file.read_at(&mut bytes[read..], start + read as u64)?;
            if count == 0 {
                return Err(ObserverError::InvalidRollout);
            }
            read += count;
        }
        Ok(Sha256::digest(&bytes).into())
    }

    fn hash_range(&self, offset: u64, length: usize) -> Result<[u8; 32], ObserverError> {
        use std::os::unix::fs::FileExt;

        let end = offset
            .checked_add(length as u64)
            .ok_or(ObserverError::RolloutTooLarge)?;
        if end > self.file.metadata()?.len() {
            return Err(ObserverError::InvalidRollout);
        }
        let mut hasher = Sha256::new();
        let mut buffer = [0_u8; ROLLOUT_ANCHOR_BYTES];
        let mut read = 0_usize;
        while read < length {
            let wanted = (length - read).min(buffer.len());
            let count = self
                .file
                .read_at(&mut buffer[..wanted], offset + read as u64)?;
            if count == 0 {
                return Err(ObserverError::InvalidRollout);
            }
            hasher.update(&buffer[..count]);
            read += count;
        }
        Ok(hasher.finalize().into())
    }
}

struct TurnBuilder {
    turn_id: String,
    user: Vec<String>,
    assistant: Vec<String>,
    tools: Vec<ToolFact>,
    tool_calls: BTreeMap<String, usize>,
}

impl TurnBuilder {
    fn new(turn_id: String) -> Self {
        Self {
            turn_id,
            user: Vec::new(),
            assistant: Vec::new(),
            tools: Vec::new(),
            tool_calls: BTreeMap::new(),
        }
    }

    fn observe_response_item(&mut self, payload: &serde_json::Value) -> Result<(), ObserverError> {
        let object = payload.as_object().ok_or(ObserverError::InvalidRollout)?;
        if let Some(turn_id) = response_turn_id(object)
            && turn_id != self.turn_id
        {
            return Ok(());
        }
        match object.get("type").and_then(serde_json::Value::as_str) {
            Some("message") => {
                let Some(role) = object.get("role").and_then(serde_json::Value::as_str) else {
                    return Err(ObserverError::InvalidRollout);
                };
                if role != "user" && role != "assistant" {
                    return Ok(());
                }
                let content = object
                    .get("content")
                    .and_then(serde_json::Value::as_array)
                    .ok_or(ObserverError::InvalidRollout)?;
                for part in content {
                    let Some(part) = part.as_object() else {
                        return Err(ObserverError::InvalidRollout);
                    };
                    if matches!(
                        part.get("type").and_then(serde_json::Value::as_str),
                        Some("input_text" | "output_text")
                    ) && let Some(text) = part.get("text").and_then(serde_json::Value::as_str)
                    {
                        self.push_message(role, text)?;
                    }
                }
            }
            Some("function_call" | "custom_tool_call") => {
                let name = object
                    .get("name")
                    .and_then(serde_json::Value::as_str)
                    .ok_or(ObserverError::InvalidRollout)?;
                let status = object
                    .get("status")
                    .and_then(serde_json::Value::as_str)
                    .unwrap_or("requested");
                if name.is_empty() {
                    return Ok(());
                }
                if self.tools.len() >= MAX_TOOLS {
                    self.tools.remove(0);
                    self.tool_calls.retain(|_, index| {
                        if *index == 0 {
                            false
                        } else {
                            *index -= 1;
                            true
                        }
                    });
                }
                let index = self.tools.len();
                let name = redact_sensitive_text(name);
                let status = redact_sensitive_text(status);
                self.tools.push(ToolFact {
                    name: compact_text(&name, MAX_TOOL_NAME_BYTES),
                    status: compact_text(&status, MAX_TOOL_STATUS_BYTES),
                });
                if let Some(call_id) = object.get("call_id").and_then(serde_json::Value::as_str)
                    && !call_id.is_empty()
                    && call_id.len() <= MAX_TOOL_NAME_BYTES
                    && !call_id.bytes().any(|byte| byte.is_ascii_control())
                {
                    self.tool_calls.insert(call_id.to_owned(), index);
                }
            }
            Some("function_call_output" | "custom_tool_call_output") => {
                if let Some(call_id) = object.get("call_id").and_then(serde_json::Value::as_str)
                    && let Some(index) = self.tool_calls.get(call_id).copied()
                {
                    self.tools[index].status = "completed".into();
                }
            }
            _ => {}
        }
        Ok(())
    }

    fn push_message(&mut self, role: &str, text: &str) -> Result<(), ObserverError> {
        let text = redact_sensitive_text(text);
        if text.is_empty() {
            return Ok(());
        }
        let target = if role == "user" {
            &mut self.user
        } else {
            &mut self.assistant
        };
        if target.len() >= MAX_RAW_MESSAGES_PER_ROLE {
            target.remove(0);
        }
        target.push(compact_text(&text, MAX_TEXT_BYTES));
        Ok(())
    }

    fn finish(mut self) -> Result<TurnPack, ObserverError> {
        if Uuid::parse_str(&self.turn_id).is_err() {
            return Err(ObserverError::InvalidRollout);
        }
        self.user = compact_messages(self.user);
        self.assistant = compact_messages(self.assistant);
        Ok(TurnPack {
            v: 1,
            turn_id: self.turn_id,
            user: self.user,
            assistant: self.assistant,
            tools: self.tools,
        })
    }
}

fn compact_messages(messages: Vec<String>) -> Vec<String> {
    let mut remaining = MAX_TEXT_BYTES_PER_ROLE;
    let mut compacted = Vec::new();
    for message in messages.into_iter().rev().take(MAX_MESSAGES_PER_ROLE) {
        if remaining == 0 {
            break;
        }
        let message = compact_text(&message, remaining.min(MAX_TEXT_BYTES));
        remaining = remaining.saturating_sub(message.len());
        compacted.push(message);
    }
    compacted.reverse();
    compacted
}

fn compact_text(text: &str, max_bytes: usize) -> String {
    const MARKER: &str = "\n[truncated]\n";

    if text.len() <= max_bytes {
        return text.to_owned();
    }
    if max_bytes <= MARKER.len() {
        return MARKER[..max_bytes].to_owned();
    }
    let available = max_bytes - MARKER.len();
    let head_budget = available / 3;
    let tail_budget = available - head_budget;
    let head_end = utf8_floor(text, head_budget);
    let tail_start = utf8_ceil(text, text.len().saturating_sub(tail_budget));
    format!("{}{}{}", &text[..head_end], MARKER, &text[tail_start..])
}

fn utf8_floor(text: &str, mut index: usize) -> usize {
    index = index.min(text.len());
    while index > 0 && !text.is_char_boundary(index) {
        index -= 1;
    }
    index
}

fn utf8_ceil(text: &str, mut index: usize) -> usize {
    index = index.min(text.len());
    while index < text.len() && !text.is_char_boundary(index) {
        index += 1;
    }
    index
}

fn required_turn_id(
    payload: &serde_json::Map<String, serde_json::Value>,
) -> Result<String, ObserverError> {
    let turn_id = payload
        .get("turn_id")
        .and_then(serde_json::Value::as_str)
        .ok_or(ObserverError::InvalidRollout)?;
    if Uuid::parse_str(turn_id).is_err() {
        return Err(ObserverError::InvalidRollout);
    }
    Ok(turn_id.to_owned())
}

fn response_turn_id(payload: &serde_json::Map<String, serde_json::Value>) -> Option<&str> {
    payload
        .get("turn_id")
        .and_then(serde_json::Value::as_str)
        .or_else(|| {
            payload
                .get("internal_chat_message_metadata_passthrough")
                .and_then(serde_json::Value::as_object)
                .and_then(|metadata| metadata.get("turn_id"))
                .and_then(serde_json::Value::as_str)
        })
        .or_else(|| {
            payload
                .get("metadata")
                .and_then(serde_json::Value::as_object)
                .and_then(|metadata| metadata.get("turn_id"))
                .and_then(serde_json::Value::as_str)
        })
}

const MAX_STREAM_JSON_DEPTH: usize = 64;
const MAX_STREAM_CONTENT_PARTS: usize = 32;
const MAX_STREAM_SMALL_FIELD_BYTES: usize = 256;

#[derive(Default)]
struct StreamedRecord {
    payload_object: bool,
    top_type: Option<String>,
    payload_type: Option<String>,
    role: Option<String>,
    turn_id: Option<String>,
    internal_turn_id: Option<String>,
    metadata_turn_id: Option<String>,
    name: Option<String>,
    status: Option<String>,
    call_id: Option<String>,
    last_agent_message: Option<String>,
    parts: Vec<StreamedPart>,
}

#[derive(Default)]
struct StreamedPart {
    kind: Option<String>,
    text: Option<String>,
}

impl StreamedRecord {
    fn clear_payload(&mut self) {
        self.payload_object = false;
        self.payload_type = None;
        self.role = None;
        self.turn_id = None;
        self.internal_turn_id = None;
        self.metadata_turn_id = None;
        self.name = None;
        self.status = None;
        self.call_id = None;
        self.last_agent_message = None;
        self.parts.clear();
    }

    fn into_value(self) -> serde_json::Value {
        let mut root = serde_json::Map::new();
        if let Some(value) = self.top_type {
            root.insert("type".into(), value.into());
        }
        let mut payload = serde_json::Map::new();
        for (key, value) in [
            ("type", self.payload_type),
            ("role", self.role),
            ("turn_id", self.turn_id),
            ("name", self.name),
            ("status", self.status),
            ("call_id", self.call_id),
            ("last_agent_message", self.last_agent_message),
        ] {
            if let Some(value) = value {
                payload.insert(key.into(), value.into());
            }
        }
        if let Some(turn_id) = self.metadata_turn_id {
            payload.insert("metadata".into(), serde_json::json!({"turn_id": turn_id}));
        }
        if let Some(turn_id) = self.internal_turn_id {
            payload.insert(
                "internal_chat_message_metadata_passthrough".into(),
                serde_json::json!({"turn_id": turn_id}),
            );
        }
        if !self.parts.is_empty() {
            payload.insert(
                "content".into(),
                self.parts
                    .into_iter()
                    .map(|part| {
                        let mut value = serde_json::Map::new();
                        if let Some(kind) = part.kind {
                            value.insert("type".into(), kind.into());
                        }
                        if let Some(text) = part.text {
                            value.insert("text".into(), text.into());
                        }
                        serde_json::Value::Object(value)
                    })
                    .collect::<Vec<_>>()
                    .into(),
            );
        }
        if self.payload_object {
            root.insert("payload".into(), serde_json::Value::Object(payload));
        }
        serde_json::Value::Object(root)
    }
}

#[derive(Clone, Copy)]
enum JsonContext {
    Root,
    Payload,
    ContentArray,
    ContentPart(usize),
    Metadata,
    InternalMetadata,
    Other,
}

enum JsonFrame {
    Object {
        context: JsonContext,
        state: ObjectState,
        key: Option<String>,
    },
    Array {
        context: JsonContext,
        state: ArrayState,
    },
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ObjectState {
    FirstKeyOrEnd,
    Key,
    Colon,
    Value,
    CommaOrEnd,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ArrayState {
    FirstValueOrEnd,
    Value,
    CommaOrEnd,
}

#[derive(Clone, Copy)]
enum FieldTarget {
    TopType,
    PayloadType,
    Role,
    TurnId,
    InternalTurnId,
    MetadataTurnId,
    Name,
    Status,
    CallId,
    LastAgentMessage,
    PartType(usize),
    PartText(usize),
    Ignore,
}

enum JsonLex {
    String(JsonString),
    Atom(Vec<u8>),
}

struct JsonString {
    key: bool,
    target: FieldTarget,
    capture: BoundedCapture,
    escape: StringEscape,
    raw_utf8: Vec<u8>,
}

enum StringEscape {
    None,
    Escaped,
    Unicode {
        value: u16,
        digits: u8,
        high: Option<u16>,
    },
    LowBackslash(u16),
    LowU(u16),
}

enum BoundedCapture {
    None,
    Small {
        bytes: Vec<u8>,
        overflowed: bool,
    },
    Text {
        head: Vec<u8>,
        tail: VecDeque<Vec<u8>>,
        tail_bytes: usize,
        total: usize,
    },
}

impl BoundedCapture {
    fn small() -> Self {
        Self::Small {
            bytes: Vec::new(),
            overflowed: false,
        }
    }

    fn text() -> Self {
        Self::Text {
            head: Vec::new(),
            tail: VecDeque::new(),
            tail_bytes: 0,
            total: 0,
        }
    }

    fn push(&mut self, bytes: &[u8]) {
        match self {
            Self::None => {}
            Self::Small {
                bytes: captured,
                overflowed,
            } => {
                let remaining = MAX_STREAM_SMALL_FIELD_BYTES.saturating_sub(captured.len());
                captured.extend_from_slice(&bytes[..bytes.len().min(remaining)]);
                *overflowed |= bytes.len() > remaining;
            }
            Self::Text {
                head,
                tail,
                tail_bytes,
                total,
            } => {
                let head_limit = (MAX_TEXT_BYTES - "\n[truncated]\n".len()) / 3;
                let tail_limit = MAX_TEXT_BYTES - "\n[truncated]\n".len() - head_limit;
                *total = total.saturating_add(bytes.len());
                if head.len().saturating_add(bytes.len()) <= head_limit {
                    head.extend_from_slice(bytes);
                } else {
                    tail.push_back(bytes.to_vec());
                    *tail_bytes = tail_bytes.saturating_add(bytes.len());
                    while *tail_bytes > tail_limit {
                        let removed = tail.pop_front().expect("tail size is nonzero");
                        *tail_bytes -= removed.len();
                    }
                }
            }
        }
    }

    fn finish(self) -> Result<String, ObserverError> {
        match self {
            Self::None => Ok(String::new()),
            Self::Small { bytes, overflowed } => {
                if overflowed {
                    return Err(ObserverError::InvalidRollout);
                }
                String::from_utf8(bytes).map_err(|_| ObserverError::InvalidRollout)
            }
            Self::Text {
                head,
                tail,
                tail_bytes: _,
                total,
            } => {
                let tail = tail.into_iter().flatten().collect::<Vec<_>>();
                let retained = head.len() + tail.len();
                let mut value =
                    String::from_utf8(head).map_err(|_| ObserverError::InvalidRollout)?;
                if total > retained {
                    value.push_str("\n[truncated]\n");
                }
                value
                    .push_str(&String::from_utf8(tail).map_err(|_| ObserverError::InvalidRollout)?);
                Ok(value)
            }
        }
    }
}

impl JsonString {
    fn new(key: bool, target: FieldTarget) -> Self {
        let capture = if key {
            BoundedCapture::small()
        } else {
            match target {
                FieldTarget::PartText(_) | FieldTarget::LastAgentMessage => BoundedCapture::text(),
                FieldTarget::Ignore => BoundedCapture::None,
                _ => BoundedCapture::small(),
            }
        };
        Self {
            key,
            target,
            capture,
            escape: StringEscape::None,
            raw_utf8: Vec::with_capacity(4),
        }
    }

    fn feed(&mut self, byte: u8) -> Result<bool, ObserverError> {
        let state = std::mem::replace(&mut self.escape, StringEscape::None);
        match state {
            StringEscape::None => match byte {
                b'"' if self.raw_utf8.is_empty() => return Ok(true),
                b'\\' if self.raw_utf8.is_empty() => self.escape = StringEscape::Escaped,
                0x00..=0x1f => return Err(ObserverError::InvalidRollout),
                _ => self.feed_raw_utf8(byte)?,
            },
            StringEscape::Escaped => match byte {
                b'"' | b'\\' | b'/' => self.capture.push(&[byte]),
                b'b' => self.capture.push(&[0x08]),
                b'f' => self.capture.push(&[0x0c]),
                b'n' => self.capture.push(b"\n"),
                b'r' => self.capture.push(b"\r"),
                b't' => self.capture.push(b"\t"),
                b'u' => {
                    self.escape = StringEscape::Unicode {
                        value: 0,
                        digits: 0,
                        high: None,
                    }
                }
                _ => return Err(ObserverError::InvalidRollout),
            },
            StringEscape::Unicode {
                mut value,
                mut digits,
                high,
            } => {
                let digit = hex_value(byte)? as u16;
                value = value
                    .checked_mul(16)
                    .and_then(|value| value.checked_add(digit))
                    .ok_or(ObserverError::InvalidRollout)?;
                digits += 1;
                if digits < 4 {
                    self.escape = StringEscape::Unicode {
                        value,
                        digits,
                        high,
                    };
                } else if let Some(high) = high {
                    if !(0xdc00..=0xdfff).contains(&value) {
                        return Err(ObserverError::InvalidRollout);
                    }
                    let scalar =
                        0x1_0000 + (((high as u32 - 0xd800) << 10) | (value as u32 - 0xdc00));
                    push_scalar(&mut self.capture, scalar)?;
                } else if (0xd800..=0xdbff).contains(&value) {
                    self.escape = StringEscape::LowBackslash(value);
                } else if (0xdc00..=0xdfff).contains(&value) {
                    return Err(ObserverError::InvalidRollout);
                } else {
                    push_scalar(&mut self.capture, value as u32)?;
                }
            }
            StringEscape::LowBackslash(high) => {
                if byte != b'\\' {
                    return Err(ObserverError::InvalidRollout);
                }
                self.escape = StringEscape::LowU(high);
            }
            StringEscape::LowU(high) => {
                if byte != b'u' {
                    return Err(ObserverError::InvalidRollout);
                }
                self.escape = StringEscape::Unicode {
                    value: 0,
                    digits: 0,
                    high: Some(high),
                };
            }
        }
        Ok(false)
    }

    fn feed_raw_utf8(&mut self, byte: u8) -> Result<(), ObserverError> {
        if self.raw_utf8.is_empty() {
            if byte.is_ascii() {
                self.capture.push(&[byte]);
                return Ok(());
            }
            if !matches!(byte, 0xc2..=0xf4) {
                return Err(ObserverError::InvalidRollout);
            }
            self.raw_utf8.push(byte);
            return Ok(());
        }
        if !matches!(byte, 0x80..=0xbf) {
            return Err(ObserverError::InvalidRollout);
        }
        self.raw_utf8.push(byte);
        let expected = match self.raw_utf8[0] {
            0xc2..=0xdf => 2,
            0xe0..=0xef => 3,
            0xf0..=0xf4 => 4,
            _ => return Err(ObserverError::InvalidRollout),
        };
        if self.raw_utf8.len() == expected {
            std::str::from_utf8(&self.raw_utf8).map_err(|_| ObserverError::InvalidRollout)?;
            self.capture.push(&self.raw_utf8);
            self.raw_utf8.clear();
        } else if self.raw_utf8.len() > expected {
            return Err(ObserverError::InvalidRollout);
        }
        Ok(())
    }
}

fn hex_value(byte: u8) -> Result<u8, ObserverError> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        b'A'..=b'F' => Ok(byte - b'A' + 10),
        _ => Err(ObserverError::InvalidRollout),
    }
}

fn push_scalar(capture: &mut BoundedCapture, scalar: u32) -> Result<(), ObserverError> {
    let character = char::from_u32(scalar).ok_or(ObserverError::InvalidRollout)?;
    let mut bytes = [0_u8; 4];
    capture.push(character.encode_utf8(&mut bytes).as_bytes());
    Ok(())
}

struct StreamingRecordParser {
    stack: Vec<JsonFrame>,
    lex: Option<JsonLex>,
    record: StreamedRecord,
    complete: bool,
}

impl StreamingRecordParser {
    fn new() -> Self {
        Self {
            stack: Vec::new(),
            lex: None,
            record: StreamedRecord::default(),
            complete: false,
        }
    }

    fn feed(&mut self, bytes: &[u8]) -> Result<(), ObserverError> {
        for byte in bytes {
            self.feed_byte(*byte)?;
        }
        Ok(())
    }

    fn feed_byte(&mut self, byte: u8) -> Result<(), ObserverError> {
        if let Some(JsonLex::String(string)) = self.lex.as_mut() {
            if string.feed(byte)? {
                let JsonLex::String(string) = self.lex.take().expect("string lexer is active")
                else {
                    unreachable!();
                };
                self.finish_string(string)?;
            }
            return Ok(());
        }
        if let Some(JsonLex::Atom(atom)) = self.lex.as_mut() {
            if is_json_whitespace(byte) || matches!(byte, b',' | b']' | b'}') {
                let JsonLex::Atom(atom) = self.lex.take().expect("atom lexer is active") else {
                    unreachable!();
                };
                serde_json::from_slice::<serde_json::Value>(&atom)
                    .map_err(|_| ObserverError::InvalidRollout)?;
                self.finish_scalar()?;
                return self.feed_byte(byte);
            }
            if atom.len() >= 64 {
                return Err(ObserverError::InvalidRollout);
            }
            atom.push(byte);
            return Ok(());
        }
        if self.complete {
            return if is_json_whitespace(byte) {
                Ok(())
            } else {
                Err(ObserverError::InvalidRollout)
            };
        }
        if self.stack.is_empty() {
            if is_json_whitespace(byte) {
                return Ok(());
            }
            if byte != b'{' {
                return Err(ObserverError::InvalidRollout);
            }
            self.stack.push(JsonFrame::Object {
                context: JsonContext::Root,
                state: ObjectState::FirstKeyOrEnd,
                key: None,
            });
            return Ok(());
        }
        match self.stack.last().expect("stack is not empty") {
            JsonFrame::Object { state, .. } => match state {
                ObjectState::FirstKeyOrEnd => match byte {
                    b if is_json_whitespace(b) => Ok(()),
                    b'}' => self.close_container(),
                    b'"' => {
                        self.lex =
                            Some(JsonLex::String(JsonString::new(true, FieldTarget::Ignore)));
                        Ok(())
                    }
                    _ => Err(ObserverError::InvalidRollout),
                },
                ObjectState::Key => match byte {
                    b if is_json_whitespace(b) => Ok(()),
                    b'"' => {
                        self.lex =
                            Some(JsonLex::String(JsonString::new(true, FieldTarget::Ignore)));
                        Ok(())
                    }
                    _ => Err(ObserverError::InvalidRollout),
                },
                ObjectState::Colon => {
                    if is_json_whitespace(byte) {
                        Ok(())
                    } else if byte == b':' {
                        self.set_object_state(ObjectState::Value)
                    } else {
                        Err(ObserverError::InvalidRollout)
                    }
                }
                ObjectState::Value => self.start_value(byte),
                ObjectState::CommaOrEnd => match byte {
                    b if is_json_whitespace(b) => Ok(()),
                    b',' => self.set_object_state(ObjectState::Key),
                    b'}' => self.close_container(),
                    _ => Err(ObserverError::InvalidRollout),
                },
            },
            JsonFrame::Array { state, .. } => match state {
                ArrayState::FirstValueOrEnd => {
                    if is_json_whitespace(byte) {
                        Ok(())
                    } else if byte == b']' {
                        self.close_container()
                    } else {
                        self.start_value(byte)
                    }
                }
                ArrayState::Value => {
                    if is_json_whitespace(byte) {
                        Ok(())
                    } else {
                        self.start_value(byte)
                    }
                }
                ArrayState::CommaOrEnd => match byte {
                    b if is_json_whitespace(b) => Ok(()),
                    b',' => self.set_array_state(ArrayState::Value),
                    b']' => self.close_container(),
                    _ => Err(ObserverError::InvalidRollout),
                },
            },
        }
    }

    fn start_value(&mut self, byte: u8) -> Result<(), ObserverError> {
        if is_json_whitespace(byte) {
            return Ok(());
        }
        self.prepare_field_value();
        let target = self.field_target();
        match byte {
            b'"' => {
                self.lex = Some(JsonLex::String(JsonString::new(false, target)));
                Ok(())
            }
            b'{' | b'[' => {
                let context = self.child_context(byte)?;
                self.finish_scalar()?;
                if self.stack.len() >= MAX_STREAM_JSON_DEPTH {
                    return Err(ObserverError::InvalidRollout);
                }
                if byte == b'{' {
                    self.stack.push(JsonFrame::Object {
                        context,
                        state: ObjectState::FirstKeyOrEnd,
                        key: None,
                    });
                } else {
                    self.stack.push(JsonFrame::Array {
                        context,
                        state: ArrayState::FirstValueOrEnd,
                    });
                }
                Ok(())
            }
            b'-' | b'0'..=b'9' | b't' | b'f' | b'n' => {
                self.lex = Some(JsonLex::Atom(vec![byte]));
                Ok(())
            }
            _ => Err(ObserverError::InvalidRollout),
        }
    }

    fn prepare_field_value(&mut self) {
        let Some(JsonFrame::Object { context, key, .. }) = self.stack.last() else {
            return;
        };
        let context = *context;
        let key = key.as_deref();
        match (context, key) {
            (JsonContext::Root, Some("type")) => self.record.top_type = None,
            (JsonContext::Root, Some("payload")) => self.record.clear_payload(),
            (JsonContext::Payload, Some("type")) => self.record.payload_type = None,
            (JsonContext::Payload, Some("role")) => self.record.role = None,
            (JsonContext::Payload, Some("turn_id")) => self.record.turn_id = None,
            (JsonContext::Payload, Some("name")) => self.record.name = None,
            (JsonContext::Payload, Some("status")) => self.record.status = None,
            (JsonContext::Payload, Some("call_id")) => self.record.call_id = None,
            (JsonContext::Payload, Some("last_agent_message")) => {
                self.record.last_agent_message = None;
            }
            (JsonContext::Payload, Some("content")) => self.record.parts.clear(),
            (JsonContext::Payload, Some("metadata")) => self.record.metadata_turn_id = None,
            (JsonContext::Payload, Some("internal_chat_message_metadata_passthrough")) => {
                self.record.internal_turn_id = None
            }
            (JsonContext::Metadata, Some("turn_id")) => self.record.metadata_turn_id = None,
            (JsonContext::InternalMetadata, Some("turn_id")) => {
                self.record.internal_turn_id = None;
            }
            (JsonContext::ContentPart(index), Some("type")) => {
                if let Some(part) = self.record.parts.get_mut(index) {
                    part.kind = None;
                }
            }
            (JsonContext::ContentPart(index), Some("text")) => {
                if let Some(part) = self.record.parts.get_mut(index) {
                    part.text = None;
                }
            }
            _ => {}
        }
    }

    fn child_context(&mut self, byte: u8) -> Result<JsonContext, ObserverError> {
        let (context, key) = match self.stack.last() {
            Some(JsonFrame::Object { context, key, .. }) => (*context, key.as_deref()),
            Some(JsonFrame::Array { context, .. }) => (*context, None),
            None => return Err(ObserverError::InvalidRollout),
        };
        Ok(match (context, key, byte) {
            (JsonContext::Root, Some("payload"), b'{') => {
                self.record.payload_object = true;
                JsonContext::Payload
            }
            (JsonContext::Payload, Some("content"), b'[') => JsonContext::ContentArray,
            (JsonContext::Payload, Some("metadata"), b'{') => JsonContext::Metadata,
            (JsonContext::Payload, Some("internal_chat_message_metadata_passthrough"), b'{') => {
                JsonContext::InternalMetadata
            }
            (JsonContext::ContentArray, _, b'{')
                if self.record.parts.len() < MAX_STREAM_CONTENT_PARTS =>
            {
                let index = self.record.parts.len();
                self.record.parts.push(StreamedPart::default());
                JsonContext::ContentPart(index)
            }
            _ => JsonContext::Other,
        })
    }

    fn field_target(&self) -> FieldTarget {
        let Some(JsonFrame::Object { context, key, .. }) = self.stack.last() else {
            return FieldTarget::Ignore;
        };
        match (*context, key.as_deref()) {
            (JsonContext::Root, Some("type")) => FieldTarget::TopType,
            (JsonContext::Payload, Some("type")) => FieldTarget::PayloadType,
            (JsonContext::Payload, Some("role")) => FieldTarget::Role,
            (JsonContext::Payload, Some("turn_id")) => FieldTarget::TurnId,
            (JsonContext::Payload, Some("name")) => FieldTarget::Name,
            (JsonContext::Payload, Some("status")) => FieldTarget::Status,
            (JsonContext::Payload, Some("call_id")) => FieldTarget::CallId,
            (JsonContext::Payload, Some("last_agent_message")) => FieldTarget::LastAgentMessage,
            (JsonContext::Metadata, Some("turn_id")) => FieldTarget::MetadataTurnId,
            (JsonContext::InternalMetadata, Some("turn_id")) => FieldTarget::InternalTurnId,
            (JsonContext::ContentPart(index), Some("type")) => FieldTarget::PartType(index),
            (JsonContext::ContentPart(index), Some("text")) => FieldTarget::PartText(index),
            _ => FieldTarget::Ignore,
        }
    }

    fn finish_string(&mut self, string: JsonString) -> Result<(), ObserverError> {
        let value = string.capture.finish()?;
        if string.key {
            let Some(JsonFrame::Object { state, key, .. }) = self.stack.last_mut() else {
                return Err(ObserverError::InvalidRollout);
            };
            if !matches!(state, ObjectState::FirstKeyOrEnd | ObjectState::Key) {
                return Err(ObserverError::InvalidRollout);
            }
            *key = Some(value);
            *state = ObjectState::Colon;
            return Ok(());
        }
        self.record_field(string.target, value);
        self.finish_scalar()
    }

    fn record_field(&mut self, target: FieldTarget, value: String) {
        match target {
            FieldTarget::TopType => self.record.top_type = Some(value),
            FieldTarget::PayloadType => self.record.payload_type = Some(value),
            FieldTarget::Role => self.record.role = Some(value),
            FieldTarget::TurnId => self.record.turn_id = Some(value),
            FieldTarget::InternalTurnId => self.record.internal_turn_id = Some(value),
            FieldTarget::MetadataTurnId => self.record.metadata_turn_id = Some(value),
            FieldTarget::Name => self.record.name = Some(value),
            FieldTarget::Status => self.record.status = Some(value),
            FieldTarget::CallId => self.record.call_id = Some(value),
            FieldTarget::LastAgentMessage => self.record.last_agent_message = Some(value),
            FieldTarget::PartType(index) => {
                if let Some(part) = self.record.parts.get_mut(index) {
                    part.kind = Some(value);
                }
            }
            FieldTarget::PartText(index) => {
                if let Some(part) = self.record.parts.get_mut(index) {
                    part.text = Some(value);
                }
            }
            FieldTarget::Ignore => {}
        }
    }

    fn finish_scalar(&mut self) -> Result<(), ObserverError> {
        match self.stack.last_mut() {
            Some(JsonFrame::Object { state, key, .. }) if *state == ObjectState::Value => {
                *state = ObjectState::CommaOrEnd;
                *key = None;
                Ok(())
            }
            Some(JsonFrame::Array { state, .. })
                if matches!(state, ArrayState::FirstValueOrEnd | ArrayState::Value) =>
            {
                *state = ArrayState::CommaOrEnd;
                Ok(())
            }
            _ => Err(ObserverError::InvalidRollout),
        }
    }

    fn close_container(&mut self) -> Result<(), ObserverError> {
        let valid = match self.stack.last() {
            Some(JsonFrame::Object { state, .. }) => {
                matches!(state, ObjectState::FirstKeyOrEnd | ObjectState::CommaOrEnd)
            }
            Some(JsonFrame::Array { state, .. }) => {
                matches!(state, ArrayState::FirstValueOrEnd | ArrayState::CommaOrEnd)
            }
            None => false,
        };
        if !valid {
            return Err(ObserverError::InvalidRollout);
        }
        self.stack.pop();
        if self.stack.is_empty() {
            self.complete = true;
        }
        Ok(())
    }

    fn set_object_state(&mut self, next: ObjectState) -> Result<(), ObserverError> {
        let Some(JsonFrame::Object { state, .. }) = self.stack.last_mut() else {
            return Err(ObserverError::InvalidRollout);
        };
        *state = next;
        Ok(())
    }

    fn set_array_state(&mut self, next: ArrayState) -> Result<(), ObserverError> {
        let Some(JsonFrame::Array { state, .. }) = self.stack.last_mut() else {
            return Err(ObserverError::InvalidRollout);
        };
        *state = next;
        Ok(())
    }

    fn finish(self) -> Result<serde_json::Value, ObserverError> {
        if !self.complete || self.lex.is_some() || !self.stack.is_empty() {
            return Err(ObserverError::InvalidRollout);
        }
        Ok(self.record.into_value())
    }
}

fn is_json_whitespace(byte: u8) -> bool {
    matches!(byte, b' ' | b'\t' | b'\n' | b'\r')
}

fn read_bounded_line(
    reader: &mut impl BufRead,
    output: &mut Vec<u8>,
    max_bytes: usize,
) -> Result<usize, ObserverError> {
    let mut consumed = 0_usize;
    loop {
        let available = reader.fill_buf()?;
        if available.is_empty() {
            return Ok(consumed);
        }
        let take = available
            .iter()
            .position(|byte| *byte == b'\n')
            .map_or(available.len(), |position| position + 1);
        if output.len().saturating_add(take) > max_bytes {
            return Err(ObserverError::RolloutTooLarge);
        }
        output.extend_from_slice(&available[..take]);
        reader.consume(take);
        consumed += take;
        if output.last() == Some(&b'\n') {
            return Ok(consumed);
        }
    }
}

pub(crate) fn redact_sensitive_text(text: &str) -> String {
    let sanitized = Zeroizing::new(text.replace('\0', ""));
    let mut private_key = false;
    let mut joined = Zeroizing::new(String::with_capacity(sanitized.len().min(MAX_TEXT_BYTES)));
    for (index, line) in sanitized.lines().enumerate() {
        if index > 0 {
            joined.push('\n');
        }
        let lower = Zeroizing::new(line.to_ascii_lowercase());
        let starts_private_key = lower.contains("-----begin ") && lower.contains("private key");
        let ends_private_key = lower.contains("-----end ") && lower.contains("private key");
        if private_key || starts_private_key {
            private_key = !ends_private_key;
            joined.push_str("[redacted-sensitive-line]");
            continue;
        }
        if lower.contains("bearer ") || contains_sensitive_assignment(&lower) {
            joined.push_str("[redacted-sensitive-line]");
            continue;
        }
        let userinfo = redact_url_userinfo(line);
        let paths = redact_embedded_paths(&userinfo);
        let tokens = redact_embedded_tokens(&paths);
        joined.push_str(&tokens);
    }
    joined.trim().to_owned()
}

fn contains_sensitive_assignment(line: &str) -> bool {
    const KEYS: &[&str] = &[
        "api_key",
        "api-key",
        "apikey",
        "authorization",
        "access_token",
        "access-token",
        "refresh_token",
        "refresh-token",
        "token",
        "secret",
        "password",
        "cookie",
        "set-cookie",
        "client_secret",
        "client-secret",
        "aws_secret_access_key",
        "aws-secret-access-key",
        "credential",
        "credentials",
        "private_key",
    ];
    KEYS.iter().any(|key| {
        line.match_indices(key).any(|(start, _)| {
            let before = &line[..start];
            if before
                .chars()
                .next_back()
                .is_some_and(|character| character.is_ascii_alphanumeric())
            {
                return false;
            }
            let mut suffix = &line[start + key.len()..];
            if suffix.starts_with(['"', '\'']) {
                suffix = &suffix[1..];
            }
            suffix = suffix.trim_start_matches(|character: char| character.is_ascii_whitespace());
            suffix.starts_with([':', '='])
        })
    })
}

fn redact_url_userinfo(line: &str) -> Zeroizing<String> {
    let lower = Zeroizing::new(line.to_ascii_lowercase());
    let mut output = Zeroizing::new(String::with_capacity(line.len().min(MAX_TEXT_BYTES)));
    let mut cursor = 0_usize;
    while cursor < line.len() {
        let scheme = ["https://", "http://", "wss://", "ws://"]
            .iter()
            .filter_map(|scheme| lower[cursor..].find(scheme).map(|offset| (offset, *scheme)))
            .min_by_key(|(offset, _)| *offset);
        let Some((relative, scheme)) = scheme else {
            output.push_str(&line[cursor..]);
            break;
        };
        let start = cursor + relative;
        let authority_start = start + scheme.len();
        output.push_str(&line[cursor..authority_start]);
        let authority_end = line[authority_start..]
            .find(|character: char| {
                character.is_whitespace()
                    || matches!(character, '/' | '?' | '#' | '"' | '`' | '<' | '>')
            })
            .map_or(line.len(), |relative| authority_start + relative);
        let authority = &line[authority_start..authority_end];
        if let Some(at) = authority.rfind('@') {
            output.push_str("[redacted-credential]@");
            output.push_str(&authority[at + 1..]);
        } else {
            output.push_str(authority);
        }
        cursor = authority_end;
    }
    output
}

fn redact_embedded_tokens(line: &str) -> Zeroizing<String> {
    let mut output = Zeroizing::new(String::with_capacity(line.len().min(MAX_TEXT_BYTES)));
    let mut cursor = 0_usize;
    while cursor < line.len() {
        let Some(start_relative) = line[cursor..]
            .find(|character: char| character.is_ascii() && is_token_character(character as u8))
        else {
            output.push_str(&line[cursor..]);
            break;
        };
        let start = cursor + start_relative;
        output.push_str(&line[cursor..start]);
        let end = line[start..]
            .find(|character: char| !character.is_ascii() || !is_token_character(character as u8))
            .map_or(line.len(), |relative| start + relative);
        let token = &line[start..end];
        if looks_like_secret_token(token) {
            output.push_str("[redacted-token]");
        } else {
            output.push_str(token);
        }
        cursor = end;
    }
    output
}

fn is_token_character(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.' | b'/' | b'+' | b'=')
}

fn redact_embedded_paths(line: &str) -> Zeroizing<String> {
    let mut output = Zeroizing::new(String::with_capacity(line.len().min(MAX_TEXT_BYTES)));
    let mut cursor = 0_usize;
    while cursor < line.len() {
        let Some(relative) = line[cursor..].find(['/', '~', ':']) else {
            output.push_str(&line[cursor..]);
            break;
        };
        let candidate = cursor + relative;
        let start = local_path_start(line, candidate);
        let Some(start) = start else {
            let next = candidate + line[candidate..].chars().next().unwrap().len_utf8();
            output.push_str(&line[cursor..next]);
            cursor = next;
            continue;
        };
        output.push_str(&line[cursor..start]);
        let end = line[start..]
            .find(|character: char| {
                character.is_whitespace()
                    || matches!(
                        character,
                        '"' | '\'' | '`' | ')' | ']' | '}' | '>' | ',' | ';'
                    )
            })
            .map_or(line.len(), |relative| start + relative);
        output.push_str("[redacted-path]");
        cursor = end.max(start + 1);
    }
    output
}

fn local_path_start(line: &str, index: usize) -> Option<usize> {
    let suffix = &line[index..];
    if suffix.starts_with("://") {
        let scheme_start = line[..index]
            .rfind(|character: char| !character.is_ascii_alphanumeric())
            .map_or(0, |position| position + 1);
        if line[scheme_start..index].eq_ignore_ascii_case("file") {
            return Some(scheme_start);
        }
        return None;
    }
    if suffix.starts_with('/') && line[..index].ends_with(':') {
        let scheme_end = index - 1;
        let scheme_start = line[..scheme_end]
            .rfind(|character: char| !character.is_ascii_alphanumeric())
            .map_or(0, |position| position + 1);
        match line[scheme_start..scheme_end].to_ascii_lowercase().as_str() {
            "file" => return Some(scheme_start),
            "http" | "https" | "ws" | "wss" => return None,
            _ => {}
        }
    }
    if suffix.starts_with(':')
        && suffix
            .as_bytes()
            .get(1)
            .is_some_and(|byte| matches!(byte, b'/' | b'\\'))
        && index > 0
        && line.as_bytes()[index - 1].is_ascii_alphabetic()
        && (index == 1
            || line[..index - 1]
                .chars()
                .next_back()
                .is_some_and(|character| {
                    character.is_whitespace()
                        || matches!(
                            character,
                            '(' | '[' | '{' | '<' | '=' | ':' | '"' | '\'' | '`'
                        )
                }))
    {
        return Some(index - 1);
    }
    let boundary = index == 0
        || line[..index].chars().next_back().is_some_and(|character| {
            character.is_whitespace()
                || matches!(
                    character,
                    '(' | '[' | '{' | '<' | '=' | ':' | '"' | '\'' | '`'
                )
        });
    if !boundary {
        return None;
    }
    if suffix.starts_with("file://") || suffix.starts_with("~/") || suffix.starts_with('/') {
        return Some(index);
    }
    if suffix.len() >= 3 {
        let bytes = suffix.as_bytes();
        if bytes[0].is_ascii_alphabetic() && bytes[1] == b':' && matches!(bytes[2], b'/' | b'\\') {
            return Some(index);
        }
    }
    None
}

fn looks_like_secret_token(token: &str) -> bool {
    let token = token.trim_matches(|character: char| {
        matches!(character, '`' | '"' | '\'' | ',' | ';' | ')' | ']' | '}')
    });
    let token = token.rsplit_once('=').map_or(token, |(_, value)| value);
    let known_prefix = [
        "sk-",
        "ghp_",
        "github_pat_",
        "xoxb-",
        "xoxp-",
        "AKIA",
        "ASIA",
        "AIza",
    ]
    .iter()
    .any(|prefix| token.starts_with(prefix) && token.len() >= prefix.len() + 8);
    let jwt = token.starts_with("eyJ")
        && token.split('.').count() == 3
        && token.split('.').all(|segment| segment.len() >= 8);
    let git_sha = token.len() == 40 && token.bytes().all(|byte| byte.is_ascii_hexdigit());
    let high_entropy = !git_sha
        && token.len() >= 32
        && token.bytes().any(|byte| byte.is_ascii_alphabetic())
        && token.bytes().any(|byte| byte.is_ascii_digit())
        && token
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'+' | b'='));
    known_prefix || jwt || high_entropy
}

#[cfg(test)]
mod tests {
    use std::fs::{self, OpenOptions};
    use std::io::Write;
    use std::os::unix::fs::MetadataExt;

    use rusqlite::{Connection, params};
    use serde_json::json;
    use tempfile::tempdir;

    use super::*;

    const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
    const TURN_A: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
    const TURN_B: &str = "019fa972-5cfa-75e1-9008-0b17ade9a349";

    fn task(path: &Path) -> CodexTask {
        CodexTask {
            task_id: TASK.to_owned(),
            name: "Fixture".into(),
            project: "fixture".into(),
            cwd: path.parent().unwrap().to_path_buf(),
            rollout_path: path.to_path_buf(),
            updated_at_ms: 1,
            pinned: true,
        }
    }

    fn observer(root: &Path) -> RolloutObserver {
        RolloutObserver::new(CodexTaskCatalog::from_paths(
            root.join("unused-codex"),
            root.join("unused-snapshots"),
        ))
    }

    fn line(value: serde_json::Value) -> String {
        format!("{}\n", serde_json::to_string(&value).unwrap())
    }

    fn meta() -> String {
        line(json!({"type":"session_meta","payload":{"id":TASK}}))
    }

    fn started(turn: &str) -> String {
        line(json!({"type":"event_msg","payload":{"type":"task_started","turn_id":turn}}))
    }

    fn completed(turn: &str, last: &str) -> String {
        line(json!({"type":"event_msg","payload":{
            "type":"task_complete","turn_id":turn,"last_agent_message":last
        }}))
    }

    fn message(turn: &str, role: &str, text: &str) -> String {
        line(json!({"type":"response_item","payload":{
            "type":"message","role":role,
            "content":[{"type":if role == "assistant" {"output_text"} else {"input_text"},"text":text}],
            "internal_chat_message_metadata_passthrough":{"turn_id":turn}
        }}))
    }

    fn store(root: &Path) -> StateStore {
        StateStore::open(&root.join("state/state.sqlite3")).unwrap()
    }

    fn write_catalog(home: &Path, rollout: &Path) {
        fs::create_dir_all(home.join("sessions")).unwrap();
        fs::write(
            home.join(".codex-global-state.json"),
            serde_json::to_vec(&json!({
                "pinned-thread-ids": [TASK],
                "electron-persisted-atom-state": {}
            }))
            .unwrap(),
        )
        .unwrap();
        fs::write(
            home.join("session_index.jsonl"),
            format!("{{\"id\":\"{TASK}\",\"thread_name\":\"Fixture\"}}\n"),
        )
        .unwrap();
        let connection = Connection::open(home.join("state_5.sqlite")).unwrap();
        connection
            .execute_batch(
                "CREATE TABLE threads (
                    id TEXT PRIMARY KEY, name TEXT, title TEXT NOT NULL, cwd TEXT NOT NULL,
                    rollout_path TEXT NOT NULL, updated_at INTEGER NOT NULL,
                    updated_at_ms INTEGER, recency_at_ms INTEGER NOT NULL,
                    archived INTEGER NOT NULL, source TEXT, thread_source TEXT,
                    agent_role TEXT
                 );",
            )
            .unwrap();
        connection
            .execute(
                "INSERT INTO threads VALUES
                 (?1, 'Fixture', '', ?2, ?3, 1, 1000, 1000, 0, 'vscode', 'user', NULL)",
                params![TASK, home.to_string_lossy(), rollout.to_string_lossy()],
            )
            .unwrap();
    }

    #[test]
    fn only_matching_task_complete_commits_a_redacted_bounded_turn_pack() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        let contents = [
            meta(),
            started(TURN_A),
            message(TURN_A, "developer", "hidden developer content"),
            message(
                TURN_A,
                "user",
                &format!("please run tests\nDASHSCOPE_{}=never-store", "API_KEY"),
            ),
            line(json!({"type":"response_item","payload":{
                "type":"reasoning","encrypted_content":"hidden reasoning"
            }})),
            line(json!({"type":"response_item","payload":{
                "type":"function_call","name":"exec_command","call_id":"call-1",
                "arguments":"secret tool input",
                "metadata":{"turn_id":TURN_A}
            }})),
            line(json!({"type":"response_item","payload":{
                "type":"function_call_output","call_id":"call-1","output":"secret tool output"
            }})),
            message(TURN_A, "assistant", "tests passed at commit abc123"),
            completed(TURN_A, "duplicate fallback"),
        ]
        .concat();
        fs::write(&rollout, contents.as_bytes()).unwrap();
        let mut store = store(temp.path());

        let completions = observer(temp.path())
            .poll_task(&mut store, &task(&rollout))
            .unwrap();

        assert_eq!(completions.len(), 1);
        assert_eq!(completions[0].outcome, CompletionOutcome::Inserted);
        let cursor = store.rollout_cursor(TASK).unwrap().unwrap();
        assert_eq!(cursor.offset, contents.len() as u64);
        let pack = store.completion_turn_pack(TURN_A).unwrap();
        assert!(pack.contains("please run tests"));
        assert!(pack.contains("[redacted-sensitive-line]"));
        assert!(pack.contains("duplicate fallback"));
        assert!(pack.contains("exec_command"));
        assert!(pack.contains("\"status\":\"completed\""));
        for excluded in [
            "never-store",
            "hidden developer",
            "hidden reasoning",
            "secret tool input",
            "secret tool output",
            "tests passed at commit abc123",
        ] {
            assert!(!pack.contains(excluded));
        }
    }

    #[test]
    fn incomplete_turn_does_not_advance_cursor_and_restart_rebuilds_it() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        fs::write(
            &rollout,
            [meta(), started(TURN_A), message(TURN_A, "user", "work")].concat(),
        )
        .unwrap();
        let mut store = store(temp.path());
        let mut observer = observer(temp.path());

        assert!(
            observer
                .poll_task(&mut store, &task(&rollout))
                .unwrap()
                .is_empty()
        );
        assert!(store.rollout_cursor(TASK).unwrap().is_none());

        let mut file = OpenOptions::new().append(true).open(&rollout).unwrap();
        file.write_all(message(TURN_A, "assistant", "done").as_bytes())
            .unwrap();
        file.write_all(completed(TURN_A, "done").as_bytes())
            .unwrap();
        file.sync_all().unwrap();
        assert_eq!(
            observer
                .poll_task(&mut store, &task(&rollout))
                .unwrap()
                .len(),
            1
        );
        assert_eq!(store.completion_count(TASK).unwrap(), 1);
    }

    #[test]
    fn replacement_revalidates_session_and_replays_ledger_without_duplication() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        let first = [meta(), started(TURN_A), completed(TURN_A, "a")].concat();
        fs::write(&rollout, first).unwrap();
        let mut store = store(temp.path());
        let mut observer = observer(temp.path());
        observer.poll_task(&mut store, &task(&rollout)).unwrap();
        let old = store.rollout_cursor(TASK).unwrap().unwrap();

        fs::rename(&rollout, temp.path().join("old.jsonl")).unwrap();
        fs::write(
            &rollout,
            [
                meta(),
                started(TURN_A),
                completed(TURN_A, "a"),
                started(TURN_B),
                completed(TURN_B, "b"),
            ]
            .concat(),
        )
        .unwrap();
        let completions = observer.poll_task(&mut store, &task(&rollout)).unwrap();

        assert_eq!(completions.len(), 2);
        assert_eq!(completions[0].outcome, CompletionOutcome::Replay);
        assert_eq!(completions[1].outcome, CompletionOutcome::Inserted);
        assert_eq!(store.completion_count(TASK).unwrap(), 2);
        let current = store.rollout_cursor(TASK).unwrap().unwrap();
        assert_eq!(current.generation, old.generation + 1);
        assert_ne!((current.device, current.inode), (old.device, old.inode));
    }

    #[test]
    fn same_inode_truncation_restarts_from_zero_and_keeps_completion_idempotent() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        fs::write(
            &rollout,
            [
                meta(),
                started(TURN_A),
                message(TURN_A, "assistant", &"x".repeat(2_000)),
                completed(TURN_A, "a"),
            ]
            .concat(),
        )
        .unwrap();
        let inode = fs::metadata(&rollout).unwrap().ino();
        let mut store = store(temp.path());
        let mut observer = observer(temp.path());
        observer.poll_task(&mut store, &task(&rollout)).unwrap();
        let old = store.rollout_cursor(TASK).unwrap().unwrap();

        fs::write(
            &rollout,
            [meta(), started(TURN_A), completed(TURN_A, "a")].concat(),
        )
        .unwrap();
        assert_eq!(fs::metadata(&rollout).unwrap().ino(), inode);
        let completions = observer.poll_task(&mut store, &task(&rollout)).unwrap();

        assert_eq!(completions[0].outcome, CompletionOutcome::Replay);
        assert_eq!(store.completion_count(TASK).unwrap(), 1);
        assert_eq!(
            store.rollout_cursor(TASK).unwrap().unwrap().generation,
            old.generation + 1
        );
    }

    #[test]
    fn same_inode_rewrite_regrown_to_the_old_offset_is_detected_by_anchor() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        let first = [meta(), started(TURN_A), completed(TURN_A, "a")].concat();
        fs::write(&rollout, &first).unwrap();
        let inode = fs::metadata(&rollout).unwrap().ino();
        let mut store = store(temp.path());
        let mut observer = observer(temp.path());
        observer.poll_task(&mut store, &task(&rollout)).unwrap();
        let old = store.rollout_cursor(TASK).unwrap().unwrap();
        assert_eq!(old.offset, first.len() as u64);

        let rewritten = [meta(), started(TURN_B), completed(TURN_B, "b")].concat();
        assert_eq!(rewritten.len(), first.len());
        fs::write(&rollout, rewritten).unwrap();
        assert_eq!(fs::metadata(&rollout).unwrap().ino(), inode);

        let completions = observer.poll_task(&mut store, &task(&rollout)).unwrap();

        assert_eq!(completions.len(), 1);
        assert_eq!(completions[0].completion_id, TURN_B);
        assert_eq!(completions[0].outcome, CompletionOutcome::Inserted);
        assert_eq!(store.completion_count(TASK).unwrap(), 2);
        assert_eq!(
            store.rollout_cursor(TASK).unwrap().unwrap().generation,
            old.generation + 1
        );
    }

    #[test]
    fn concurrent_same_inode_rewrite_before_commit_is_rejected_and_replayed() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        let common = [
            meta(),
            started(TURN_A),
            message(TURN_A, "user", &"same-prefix ".repeat(800)),
        ]
        .concat();
        assert!(common.len() > ROLLOUT_ANCHOR_BYTES);
        let first = [common.clone(), completed(TURN_A, "a")].concat();
        let rewritten = [
            common,
            started(TURN_B),
            completed(TURN_B, "b"),
            line(json!({"type":"ignored","padding":"x".repeat(1_000)})),
        ]
        .concat();
        assert!(rewritten.len() >= first.len());
        fs::write(&rollout, first).unwrap();
        let inode = fs::metadata(&rollout).unwrap().ino();
        let replacement = rewritten.clone();
        let mut observer = observer(temp.path());
        observer.before_trust_check = Some(Box::new(move |path| {
            fs::write(path, &replacement).unwrap();
        }));
        let mut store = store(temp.path());

        assert!(matches!(
            observer.poll_task(&mut store, &task(&rollout)),
            Err(ObserverError::InvalidRollout)
        ));
        assert_eq!(fs::metadata(&rollout).unwrap().ino(), inode);
        assert_eq!(store.completion_count(TASK).unwrap(), 0);
        assert!(store.rollout_cursor(TASK).unwrap().is_none());

        let completions = observer.poll_task(&mut store, &task(&rollout)).unwrap();
        assert_eq!(completions.len(), 1);
        assert_eq!(completions[0].completion_id, TURN_B);
        assert_eq!(store.completion_count(TASK).unwrap(), 1);
    }

    #[test]
    fn real_shape_large_turn_is_compacted_and_completed_across_poll_windows() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        let mut contents = [meta(), started(TURN_A)].concat();
        let large_user = format!("USER_HEAD{}USER_TAIL", "u".repeat(73_000));
        contents.push_str(&message(TURN_A, "user", &large_user));
        for index in 0..806 {
            contents.push_str(&message(
                TURN_A,
                "assistant",
                &format!("assistant-{index:04} {}", "a ".repeat(3_000)),
            ));
        }
        for index in 0..100 {
            contents.push_str(&line(json!({"type":"response_item","payload":{
                "type":"function_call","name":format!("tool-{index:03}"),
                "call_id":format!("call-{index:03}"),"status":"requested",
                "metadata":{"turn_id":TURN_A}
            }})));
        }
        contents.push_str(&completed(TURN_A, "latest assistant fallback"));
        assert!(contents.len() > MAX_SCAN_BYTES_PER_POLL);
        fs::write(&rollout, contents).unwrap();
        let mut store = store(temp.path());
        let mut observer = observer(temp.path());

        let first = observer.poll_task(&mut store, &task(&rollout)).unwrap();
        assert!(first.is_empty());
        assert!(store.rollout_cursor(TASK).unwrap().is_none());

        let mut completion = Vec::new();
        for _ in 0..3 {
            completion = observer.poll_task(&mut store, &task(&rollout)).unwrap();
            if !completion.is_empty() {
                break;
            }
        }
        assert_eq!(completion.len(), 1);
        let serialized = store.completion_turn_pack(TURN_A).unwrap();
        assert!(serialized.len() <= MAX_SERIALIZED_TURN_PACK_BYTES);
        let pack: TurnPack = serde_json::from_str(&serialized).unwrap();
        assert_eq!(pack.user.len(), 1);
        assert!(pack.user[0].contains("USER_HEAD"));
        assert!(pack.user[0].contains("USER_TAIL"));
        assert!(pack.user[0].contains("[truncated]"));
        assert_eq!(pack.assistant, ["latest assistant fallback"]);
        assert_eq!(pack.tools.len(), MAX_TOOLS);
        assert_eq!(pack.tools.first().unwrap().name, "tool-036");
        assert_eq!(pack.tools.last().unwrap().name, "tool-099");
    }

    #[test]
    fn multi_megabyte_tool_and_user_lines_stream_without_blocking_completion() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        let large_tool_output = "HUGE_TOOL_SENTINEL ".repeat(280_000);
        let large_user = format!("LARGE_USER_HEAD {} LARGE_USER_TAIL", "u ".repeat(520_000));
        let contents = [
            meta(),
            started(TURN_A),
            line(json!({"type":"response_item","payload":{
                "type":"function_call_output","call_id":"large-call",
                "output":large_tool_output
            }})),
            message(TURN_A, "user", &large_user),
            message(TURN_A, "assistant", "streamed completion"),
            completed(TURN_A, "streamed completion"),
        ]
        .concat();
        assert!(contents.lines().map(str::len).max().unwrap() > 5 * 1024 * 1024);
        fs::write(&rollout, contents).unwrap();
        let mut store = store(temp.path());
        let mut observer = observer(temp.path());

        let first = observer.poll_task(&mut store, &task(&rollout)).unwrap();
        assert!(first.is_empty());
        assert!(store.rollout_cursor(TASK).unwrap().is_none());

        let mut completions = Vec::new();
        for _ in 0..4 {
            completions = observer.poll_task(&mut store, &task(&rollout)).unwrap();
            if !completions.is_empty() {
                break;
            }
        }
        assert_eq!(completions.len(), 1);
        let pack = store.completion_turn_pack(TURN_A).unwrap();
        assert!(pack.contains("LARGE_USER_HEAD"));
        assert!(pack.contains("LARGE_USER_TAIL"));
        assert!(pack.contains("[truncated]"));
        assert!(pack.contains("streamed completion"));
        assert!(!pack.contains("HUGE_TOOL_SENTINEL"));
        assert!(pack.len() <= MAX_SERIALIZED_TURN_PACK_BYTES);
        assert!(
            observer
                .verification_read_sizes
                .iter()
                .all(|bytes| *bytes <= MAX_SCAN_BYTES_PER_POLL)
        );
        assert!(observer.verification_read_sizes.len() >= 2);
        assert_eq!(
            observer.verification_read_sizes.iter().sum::<usize>() as u64,
            store.rollout_cursor(TASK).unwrap().unwrap().offset
        );
    }

    #[test]
    fn redaction_removes_absolute_paths_and_common_credential_shapes() {
        let unix_path = format!("{}/Users/alice/private.txt", "");
        let home_path = format!("{}docs/private.txt", "~/");
        let file_path = format!("{}tmp/private.txt", "file:///");
        let windows_path = format!("{}:\\Users\\alice\\private.txt", "C");
        let markdown_path = format!("[store.rs]({}/Users/alice/project/store.rs:42)", "");
        let json_path = format!(r#"{{"path":"{}/Users/alice/project/state.db"}}"#, "");
        let public_url = "https://github.com/example/project/commit/abc123";
        let github = format!("{}{}", ["gh", "p_"].concat(), "a1".repeat(20));
        let embedded_credential = format!(r#"{{"credential":"{github}"}}"#);
        let slack = format!("{}{}", ["xo", "xb-"].concat(), "a1".repeat(20));
        let aws = format!("{}{}", ["AK", "IA"].concat(), "A1".repeat(8));
        let google = format!("{}{}", ["AI", "za"].concat(), "A1".repeat(18));
        let jwt = format!("{}.{}.{}", "eyJabc123", "body1234", "sign1234");
        let entropy = "AbG2".repeat(10);
        let private_key = [
            &["-----BEGIN ", "PRIVATE KEY-----"].concat(),
            "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=",
            &["-----END ", "PRIVATE KEY-----"].concat(),
        ]
        .join("\n");
        let input = format!(
            "paths {unix_path} {home_path} {file_path} {windows_path} {markdown_path} {json_path}\n\
             public {public_url}\n\
             credentials {github} {slack} {aws} {google} {jwt} {entropy} {embedded_credential}\n\
             token=plain-value\n{private_key}"
        );

        let redacted = redact_sensitive_text(&input);

        for secret in [
            &unix_path,
            &home_path,
            &file_path,
            &windows_path,
            &markdown_path,
            &json_path,
            &github,
            &embedded_credential,
            &slack,
            &aws,
            &google,
            &jwt,
            &entropy,
        ] {
            assert!(!redacted.contains(secret));
        }
        assert_eq!(redacted.matches("[redacted-path]").count(), 6);
        assert!(redacted.contains(public_url));
        let commit = "0123456789abcdef0123456789abcdef01234567";
        assert!(redact_sensitive_text(commit).contains(commit));
        assert!(redacted.matches("[redacted-sensitive-line]").count() >= 4);
        assert!(redact_sensitive_text(&format!("value {entropy}")).contains("[redacted-token]"));

        for credential in [
            r#"{"password":"hunter2"}"#,
            r#"{"secret" : "short"}"#,
            r#"{"token":"tiny"}"#,
            r#"{"authorization":"Basic abc"}"#,
            r#"{"cookie":"sid=abc"}"#,
            r#"{"client_secret":"brief"}"#,
        ] {
            assert_eq!(
                redact_sensitive_text(credential),
                "[redacted-sensitive-line]"
            );
        }
        let credential_url = "https://alice:s3%20cret@example.com/api";
        let redacted_url = redact_sensitive_text(credential_url);
        assert_eq!(
            redacted_url,
            "https://[redacted-credential]@example.com/api"
        );
        assert!(!redacted_url.contains("alice"));
        assert!(!redacted_url.contains("s3%20cret"));
    }

    #[test]
    fn streaming_parser_handles_reordered_fields_escapes_and_small_chunks() {
        let raw = format!(
            r#"{{"payload":{{"content":[{{"text":"head\n\u4f60\u597d tail","type":"input_text"}}],"metadata":{{"turn_id":"{TURN_A}"}},"role":"user","type":"message"}},"type":"response_item"}}"#
        );
        let mut parser = StreamingRecordParser::new();
        for chunk in raw.as_bytes().chunks(3) {
            parser.feed(chunk).unwrap();
        }

        let value = parser.finish().unwrap();

        assert_eq!(value["type"], "response_item");
        assert_eq!(value["payload"]["type"], "message");
        assert_eq!(value["payload"]["role"], "user");
        assert_eq!(value["payload"]["metadata"]["turn_id"], TURN_A);
        assert_eq!(value["payload"]["content"][0]["type"], "input_text");
        assert_eq!(value["payload"]["content"][0]["text"], "head\n你好 tail");

        for invalid in [br#"{"type":"response_item",}"#.as_slice(), br#"{"x":[1,]}"#] {
            let mut parser = StreamingRecordParser::new();
            let outcome = parser.feed(invalid).and_then(|()| parser.finish());
            assert!(outcome.is_err());
        }
    }

    #[test]
    fn streaming_parser_matches_serde_for_json_whitespace_and_malformed_corpus() {
        let corpus: &[&[u8]] = &[
            br#"{"type":"ignored","payload":{"value":null}}"#,
            b" \t\r\n{\"type\":\"ignored\",\"payload\":[true,false,1]} \r\n",
            br#"{"type":"ignored",}"#,
            br#"{"x":[1,]}"#,
            b"{\x0b\"type\":\"ignored\"}",
            b"{\x0c\"type\":\"ignored\"}",
            br#"{"x":"\uD800"}"#,
            b"{\"x\":\"\xf0\x9f\x92\xa9\"}",
        ];
        for raw in corpus {
            let serde_accepts = serde_json::from_slice::<serde_json::Value>(raw).is_ok();
            let mut parser = StreamingRecordParser::new();
            let stream_accepts = raw
                .chunks(2)
                .try_for_each(|chunk| parser.feed(chunk))
                .and_then(|()| parser.finish())
                .is_ok();
            assert_eq!(
                stream_accepts, serde_accepts,
                "parser disagreement for {raw:?}"
            );
        }
    }

    #[test]
    fn streamed_duplicate_payload_uses_the_last_json_value_like_serde() {
        fn duplicate_payload(padding: usize) -> Vec<u8> {
            format!(
                r#"{{"type":"event_msg","payload":{{"type":"task_complete","turn_id":"{TURN_A}","last_agent_message":"must-not-commit"}},"pad":"{}","payload":null}}"#,
                "x".repeat(padding)
            )
            .into_bytes()
        }

        let small = duplicate_payload(8);
        let small_value: serde_json::Value = serde_json::from_slice(&small).unwrap();
        let mut small_active = Some(TurnBuilder::new(TURN_A.into()));
        assert!(matches!(
            apply_rollout_value(small_value, &mut small_active),
            Err(ObserverError::InvalidRollout)
        ));

        let oversized = duplicate_payload(MAX_ROLLOUT_LINE_BYTES + 1);
        let mut parser = StreamingRecordParser::new();
        for chunk in oversized.chunks(11) {
            parser.feed(chunk).unwrap();
        }
        let streamed = parser.finish().unwrap();
        assert!(streamed.get("payload").is_none());
        let mut streamed_active = Some(TurnBuilder::new(TURN_A.into()));
        assert!(matches!(
            apply_rollout_value(streamed, &mut streamed_active),
            Err(ObserverError::InvalidRollout)
        ));
    }

    #[test]
    fn streamed_turn_id_priority_is_direct_then_internal_then_metadata_in_any_key_order() {
        for metadata_first in [false, true] {
            let ids = if metadata_first {
                format!(
                    r#""metadata":{{"turn_id":"{TURN_A}"}},"internal_chat_message_metadata_passthrough":{{"turn_id":"{TURN_B}"}}"#
                )
            } else {
                format!(
                    r#""internal_chat_message_metadata_passthrough":{{"turn_id":"{TURN_B}"}},"metadata":{{"turn_id":"{TURN_A}"}}"#
                )
            };
            let raw = format!(
                r#"{{"type":"response_item","payload":{{"type":"message","role":"user","content":[{{"type":"input_text","text":"priority text"}}],"pad":"{}",{ids}}}}}"#,
                "x".repeat(MAX_ROLLOUT_LINE_BYTES + 1)
            );
            let serde_value: serde_json::Value = serde_json::from_str(&raw).unwrap();
            assert_eq!(
                response_turn_id(serde_value["payload"].as_object().unwrap()),
                Some(TURN_B)
            );

            let mut parser = StreamingRecordParser::new();
            for chunk in raw.as_bytes().chunks(13) {
                parser.feed(chunk).unwrap();
            }
            let streamed = parser.finish().unwrap();
            assert_eq!(
                response_turn_id(streamed["payload"].as_object().unwrap()),
                Some(TURN_B)
            );
            let mut active = Some(TurnBuilder::new(TURN_B.into()));
            assert!(
                apply_rollout_value(streamed, &mut active)
                    .unwrap()
                    .is_none()
            );
            assert_eq!(active.unwrap().user, vec!["priority text"]);
        }
    }

    #[test]
    fn malformed_utf8_is_rejected_in_streamed_public_and_ignored_strings() {
        fn malformed_record(prefix: &[u8], suffix: &[u8], padding: usize) -> Vec<u8> {
            let mut raw = prefix.to_vec();
            raw.extend(std::iter::repeat_n(b'x', padding));
            raw.push(0xff);
            raw.extend_from_slice(suffix);
            raw
        }

        let fixtures = [
            (
                br#"{"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":""#
                    .as_slice(),
                format!(r#""}}],"metadata":{{"turn_id":"{TURN_A}"}}}}}}"#).into_bytes(),
            ),
            (
                br#"{"type":"ignored","padding":""#.as_slice(),
                br#""}"#.to_vec(),
            ),
        ];
        for (prefix, suffix) in fixtures {
            let small = malformed_record(prefix, &suffix, 8);
            assert!(serde_json::from_slice::<serde_json::Value>(&small).is_err());

            let oversized = malformed_record(prefix, &suffix, MAX_ROLLOUT_LINE_BYTES + 1);
            let mut parser = StreamingRecordParser::new();
            assert!(
                oversized
                    .chunks(3)
                    .try_for_each(|chunk| parser.feed(chunk))
                    .and_then(|()| parser.finish())
                    .is_err()
            );
        }
    }

    #[test]
    fn streamed_text_truncation_keeps_complete_utf8_scalars() {
        let text = "你好🙂".repeat(MAX_TEXT_BYTES);
        let raw = serde_json::to_vec(&json!({"type":"response_item","payload":{
            "type":"message","role":"user","content":[{"type":"input_text","text":text}],
            "metadata":{"turn_id":TURN_A}
        }}))
        .unwrap();
        let mut parser = StreamingRecordParser::new();
        for chunk in raw.chunks(7) {
            parser.feed(chunk).unwrap();
        }
        let value = parser.finish().unwrap();
        let retained = value["payload"]["content"][0]["text"].as_str().unwrap();
        assert!(retained.contains("[truncated]"));
        assert!(!retained.contains('\u{fffd}'));
        assert!(retained.len() <= MAX_TEXT_BYTES);
    }

    #[test]
    fn malformed_oversized_partial_and_wrong_session_rollouts_never_advance() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        let mut store = store(temp.path());
        let mut observer = observer(temp.path());

        fs::write(
            &rollout,
            line(json!({"type":"session_meta","payload":{"id":TURN_A}})),
        )
        .unwrap();
        assert!(observer.poll_task(&mut store, &task(&rollout)).is_err());
        assert!(store.rollout_cursor(TASK).unwrap().is_none());

        fs::write(&rollout, [meta(), "{".repeat(300_000)].concat()).unwrap();
        assert!(observer.poll_task(&mut store, &task(&rollout)).is_err());
        assert!(store.rollout_cursor(TASK).unwrap().is_none());

        fs::write(
            &rollout,
            [meta(), started(TURN_A), "{\"type\":".into()].concat(),
        )
        .unwrap();
        assert!(
            observer
                .poll_task(&mut store, &task(&rollout))
                .unwrap()
                .is_empty()
        );
        assert!(store.rollout_cursor(TASK).unwrap().is_none());
    }

    #[test]
    fn commentary_tool_end_and_mismatched_complete_are_not_authoritative() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        fs::write(
            &rollout,
            [
                meta(),
                started(TURN_A),
                line(
                    json!({"type":"event_msg","payload":{"type":"agent_message","message":"done"}}),
                ),
                line(json!({"type":"response_item","payload":{
                    "type":"function_call_output","output":"done"
                }})),
                completed(TURN_B, "wrong"),
            ]
            .concat(),
        )
        .unwrap();
        let mut store = store(temp.path());

        assert!(matches!(
            observer(temp.path()).poll_task(&mut store, &task(&rollout)),
            Err(ObserverError::InvalidRollout)
        ));
        assert_eq!(store.completion_count(TASK).unwrap(), 0);
        assert!(store.rollout_cursor(TASK).unwrap().is_none());
    }

    #[test]
    fn bound_task_poll_revalidates_the_catalog_and_deduplicates_slots() {
        let temp = tempdir().unwrap();
        let codex_home = temp.path().join("codex");
        let rollout = codex_home.join("sessions/rollout.jsonl");
        fs::create_dir_all(rollout.parent().unwrap()).unwrap();
        fs::write(
            &rollout,
            [meta(), started(TURN_A), completed(TURN_A, "done")].concat(),
        )
        .unwrap();
        write_catalog(&codex_home, &rollout);
        let snapshot_root = temp.path().join("snapshots");
        crate::paths::secure_directory(&snapshot_root).unwrap();
        let mut observer =
            RolloutObserver::new(CodexTaskCatalog::from_paths(codex_home, snapshot_root));
        let mut store = store(temp.path());
        store.set_binding(1, None, TASK).unwrap().unwrap();
        store.set_binding(2, None, TASK).unwrap().unwrap();

        let tick = observer.poll_bound_tasks(&mut store).unwrap();

        assert_eq!(tick.inserted, 1);
        assert_eq!(tick.replayed, 0);
        assert_eq!(tick.failed_tasks, 0);
        assert_eq!(store.completion_count(TASK).unwrap(), 1);
    }

    #[test]
    fn bound_poll_rejects_a_binding_changed_after_scan_before_commit() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        fs::write(
            &rollout,
            [meta(), started(TURN_A), completed(TURN_A, "done")].concat(),
        )
        .unwrap();
        let state_path = temp.path().join("state.sqlite3");
        let mut store = StateStore::open(&state_path).unwrap();
        let observed = store.set_binding(1, None, TASK).unwrap().unwrap();
        let replacement_path = state_path.clone();
        let mut observer = observer(temp.path());
        observer.before_trust_check = Some(Box::new(move |_| {
            let connection = Connection::open(&replacement_path).unwrap();
            connection
                .execute(
                    "UPDATE bindings
                     SET task_id = 'replacement-task', generation = generation + 1
                     WHERE slot = 1",
                    [],
                )
                .unwrap();
        }));

        assert!(matches!(
            observer.poll_task_with(&mut store, &task(&rollout), Some(&observed)),
            Err(ObserverError::Store(StoreError::BindingChanged))
        ));
        assert_eq!(store.completion_count(TASK).unwrap(), 0);
        assert!(store.rollout_cursor(TASK).unwrap().is_none());
    }

    #[test]
    fn a_later_started_turn_supersedes_an_incomplete_turn_without_advancing_early() {
        let temp = tempdir().unwrap();
        let rollout = temp.path().join("rollout.jsonl");
        fs::write(
            &rollout,
            [
                meta(),
                started(TURN_A),
                message(TURN_A, "user", "abandoned"),
                started(TURN_B),
                message(TURN_B, "assistant", "finished"),
                completed(TURN_B, "finished"),
            ]
            .concat(),
        )
        .unwrap();
        let mut store = store(temp.path());

        let completions = observer(temp.path())
            .poll_task(&mut store, &task(&rollout))
            .unwrap();

        assert_eq!(completions.len(), 1);
        assert_eq!(completions[0].completion_id, TURN_B);
        assert_eq!(store.completion_count(TASK).unwrap(), 1);
        assert!(
            !store
                .completion_turn_pack(TURN_B)
                .unwrap()
                .contains("abandoned")
        );
    }
}

#[cfg(all(target_os = "macos", not(test)))]
use std::collections::BTreeMap;
use std::ffi::CString;
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::mem;
use std::os::fd::{AsRawFd, FromRawFd, RawFd};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{FileTypeExt, MetadataExt, PermissionsExt};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use thiserror::Error;

#[cfg(all(target_os = "macos", not(test)))]
use crate::audio::inspect_eiad;
use crate::bindings::BindingService;
#[cfg(all(target_os = "macos", not(test)))]
use crate::cache::{CacheId, CacheStore};
use crate::codex_catalog::{CatalogError, CodexTaskCatalog};
use crate::codex_runner::{CodexRunner, CodexRunnerConfig};
#[cfg(all(target_os = "macos", not(test)))]
use crate::lan_playback::{PLAYBACK_CHUNK_BYTES, PlaybackBegin, PlaybackIdentity};
#[cfg(all(target_os = "macos", not(test)))]
use crate::lan_voice::{LanPlaybackEvent, LanPlaybackRequest};
use crate::lan_voice::{LanVoiceConfig, LanVoiceError, LanVoiceIngress};
use crate::paths::{AppPaths, open_owned_directory_chain};
use crate::prompt_queue::{DurablePromptScheduler, PromptQueueService};
use crate::rollout_observer::{ObserverError, RolloutObserver};
#[cfg(all(target_os = "macos", not(test)))]
use crate::store::SummaryPlaybackLease;
use crate::store::{StateStore, StoreError};

pub const HEALTH_PROTOCOL_VERSION: u8 = 1;
pub const HEALTH_SOCKET_NAME: &str = "host.sock";
const MAX_REQUEST_BYTES: usize = 4 * 1024;
const MAX_RESPONSE_BYTES: usize = 16 * 1024;
const MAX_ACTIVE_CLIENTS: usize = 16;
const CLIENT_IO_TIMEOUT: Duration = Duration::from_secs(2);
const MIN_SOCKET_TIMEOUT: Duration = Duration::from_millis(1);
const CONNECT_TIMEOUT: Duration = Duration::from_millis(750);

#[derive(Debug, Error)]
pub enum HealthError {
    #[error("Host health I/O failed")]
    Io(#[from] io::Error),
    #[error("Host state failed")]
    Store(#[from] StoreError),
    #[error("Codex rollout observer failed")]
    Observer(#[from] ObserverError),
    #[error("Codex task catalog failed")]
    Catalog(#[from] CatalogError),
    #[error("LAN voice ingress failed")]
    LanVoice(#[from] LanVoiceError),
    #[error("Host shared cache failed: {0}")]
    Cache(String),
    #[error("another Host health endpoint is already active")]
    AlreadyRunning,
    #[error("Host health socket is not a private current-user socket")]
    UnsafeSocket,
    #[error("Host health socket path is too long")]
    SocketPathTooLong,
    #[error("Host health request exceeded its size limit")]
    RequestTooLarge,
    #[error("Host health response exceeded its size limit")]
    ResponseTooLarge,
    #[error("Host health peer returned an invalid response")]
    InvalidResponse,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HealthSnapshot {
    pub v: u8,
    pub status: HealthState,
    pub host_version: String,
    pub pid: u32,
    pub started_at_unix_ms: u64,
    pub socket: String,
    pub database_schema: i64,
    pub recovered_jobs_on_start: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum HealthState {
    Ready,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct HealthRequest {
    v: u8,
    op: HealthOperation,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
enum HealthOperation {
    Health,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ErrorReply {
    v: u8,
    ok: bool,
    error: &'static str,
}

pub struct HostDaemon {
    #[cfg(all(target_os = "macos", not(test)))]
    paths: AppPaths,
    socket_path: PathBuf,
    listener: UnixListener,
    socket_identity: (u64, u64),
    snapshot: Arc<HealthSnapshot>,
    scheduler: DurablePromptScheduler,
    catalog: CodexTaskCatalog,
    lan_voice: LanVoiceIngress,
    observer: Option<RolloutObserver>,
    store: StateStore,
}

impl HostDaemon {
    pub fn open(paths: &AppPaths) -> Result<Self, HealthError> {
        paths.prepare()?;
        let store = StateStore::open(&paths.state_database)?;
        let database_schema = store.schema_version()?;
        let recovered_jobs_on_start = store.recovered_jobs_on_open();
        let observer = RolloutObserver::from_environment()?;
        let catalog = CodexTaskCatalog::from_environment()?;
        let lan_voice_config = LanVoiceConfig::from_paths(paths);
        #[cfg(test)]
        let lan_voice_config = LanVoiceConfig {
            bind_port: 0,
            ..lan_voice_config
        };
        let lan_voice = LanVoiceIngress::start(lan_voice_config)?;
        let socket_path = paths.runtime_directory.join(HEALTH_SOCKET_NAME);
        let (listener, socket_identity) = bind_private_socket(&socket_path)?;
        let started_at_unix_ms = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis()
            .try_into()
            .unwrap_or(u64::MAX);
        let snapshot = Arc::new(HealthSnapshot {
            v: HEALTH_PROTOCOL_VERSION,
            status: HealthState::Ready,
            host_version: env!("CARGO_PKG_VERSION").to_owned(),
            pid: std::process::id(),
            started_at_unix_ms,
            socket: format!("run/{HEALTH_SOCKET_NAME}"),
            database_schema,
            recovered_jobs_on_start,
        });
        Ok(Self {
            #[cfg(all(target_os = "macos", not(test)))]
            paths: paths.clone(),
            socket_path,
            listener,
            socket_identity,
            snapshot,
            scheduler: DurablePromptScheduler::new(CodexRunner::new(CodexRunnerConfig::default())),
            catalog,
            lan_voice,
            observer: Some(observer),
            store,
        })
    }

    pub fn snapshot(&self) -> &HealthSnapshot {
        &self.snapshot
    }

    pub fn serve_until(mut self, shutdown: Arc<AtomicBool>) -> Result<(), HealthError> {
        self.listener.set_nonblocking(true)?;
        let active_clients = Arc::new(AtomicUsize::new(0));
        let mut observer = self
            .observer
            .take()
            .expect("observer is present at startup");
        let mut observer_store = self.store.open_observer_store()?;
        let observer_shutdown = Arc::clone(&shutdown);
        let observer_worker = thread::spawn(move || -> Result<(), StoreError> {
            while !observer_shutdown.load(Ordering::Acquire) {
                let _ = observer.tick_due_worker(&mut observer_store)?;
                thread::sleep(Duration::from_millis(20));
            }
            Ok(())
        });
        #[cfg(all(target_os = "macos", not(test)))]
        let (summary_worker, shared_cache) = {
            let paths = self.paths.clone();
            let summary_store = self.store.open_worker_store()?;
            let summary_shutdown = Arc::clone(&shutdown);
            let cache = Arc::new(
                crate::summary_worker::open_shared_cache(&paths).map_err(HealthError::Cache)?,
            );
            let worker_cache = Arc::clone(&cache);
            let worker = thread::spawn(move || {
                crate::summary_worker::run(paths, summary_store, worker_cache, summary_shutdown)
            });
            (worker, cache)
        };
        #[cfg(all(target_os = "macos", not(test)))]
        let mut active_playbacks = BTreeMap::<u64, ActiveHostPlayback>::new();
        let mut result = Ok(());
        let mut observer_stopped_unexpectedly = false;
        while !shutdown.load(Ordering::Acquire) {
            if observer_worker.is_finished() {
                observer_stopped_unexpectedly = true;
                break;
            }
            #[cfg(all(target_os = "macos", not(test)))]
            if summary_worker.is_finished() {
                result = Err(io::Error::other("summary worker stopped unexpectedly").into());
                break;
            }
            for _ in 0..8 {
                let Some(prompt) = self.lan_voice.try_recv() else {
                    break;
                };
                let queue = PromptQueueService::new(BindingService::new(&self.catalog));
                if let Err(error) = queue.enqueue_slot(
                    &mut self.store,
                    prompt.slot,
                    &prompt.request_id,
                    &prompt.transcript,
                ) {
                    eprintln!("lan_voice_enqueue_rejected={error}");
                }
            }
            #[cfg(all(target_os = "macos", not(test)))]
            for _ in 0..8 {
                let Some(event) = self.lan_voice.try_recv_playback() else {
                    break;
                };
                handle_lan_playback_event(
                    event,
                    &mut self.store,
                    &self.lan_voice,
                    shared_cache.as_ref(),
                    &mut active_playbacks,
                );
            }
            if let Err(error) = self.scheduler.tick(&mut self.store) {
                result = Err(error.into());
                break;
            }
            match self.listener.accept() {
                Ok((stream, _)) => {
                    if reserve_client(&active_clients) {
                        let snapshot = Arc::clone(&self.snapshot);
                        let active_clients = Arc::clone(&active_clients);
                        thread::spawn(move || {
                            let _reservation = ClientReservation(active_clients);
                            let _ = handle_client(stream, &snapshot);
                        });
                    }
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                    thread::sleep(Duration::from_millis(10));
                }
                Err(error) => {
                    result = Err(error.into());
                    break;
                }
            }
        }
        shutdown.store(true, Ordering::Release);
        self.scheduler.shutdown_without_acknowledging();
        match observer_worker.join() {
            Ok(Ok(())) if observer_stopped_unexpectedly && result.is_ok() => {
                result = Err(io::Error::other("rollout observer stopped unexpectedly").into())
            }
            Ok(Ok(())) => {}
            Ok(Err(error)) if result.is_ok() => result = Err(error.into()),
            Ok(Err(_)) => {}
            Err(_) if result.is_ok() => {
                result = Err(io::Error::other("rollout observer thread panicked").into())
            }
            Err(_) => {}
        }
        #[cfg(all(target_os = "macos", not(test)))]
        if summary_worker.is_finished() {
            let _ = summary_worker.join();
        }
        result
    }
}

impl Drop for HostDaemon {
    fn drop(&mut self) {
        let _ = remove_socket_if_identity(&self.socket_path, self.socket_identity, || {});
    }
}

#[cfg(all(target_os = "macos", not(test)))]
struct ActiveHostPlayback {
    lease: SummaryPlaybackLease,
    identity: PlaybackIdentity,
    total_samples: u64,
    heard_committed: bool,
}

#[cfg(all(target_os = "macos", not(test)))]
fn handle_lan_playback_event(
    event: LanPlaybackEvent,
    store: &mut StateStore,
    ingress: &LanVoiceIngress,
    cache: &CacheStore,
    active: &mut BTreeMap<u64, ActiveHostPlayback>,
) {
    match event {
        LanPlaybackEvent::Request(request) => {
            if let Err(error) = begin_lan_playback(request, store, ingress, cache, active) {
                eprintln!("lan_playback=request_rejected error={error}");
            }
        }
        LanPlaybackEvent::Finished(finished) => {
            let Some(playback) = active.get_mut(&finished.identity.lease) else {
                return;
            };
            if playback.identity != finished.identity
                || playback.total_samples != finished.played_samples
            {
                return;
            }
            if !playback.heard_committed {
                match store.finish_summary_playback(&playback.lease) {
                    Ok(true) => playback.heard_committed = true,
                    Ok(false) => return,
                    Err(error) => {
                        eprintln!("lan_playback=finish_rejected error={error}");
                        return;
                    }
                }
            }
            match cache.reconcile_with(|| store.retained_summary_cache_references()) {
                Ok(Ok(_)) => {}
                Ok(Err(error)) => {
                    eprintln!("lan_playback=cleanup_deferred error={error}");
                    return;
                }
                Err(error) => {
                    eprintln!("lan_playback=cleanup_deferred error={error}");
                    return;
                }
            }
            if ingress.acknowledge_playback_finished(finished.identity) {
                active.remove(&finished.identity.lease);
                eprintln!(
                    "lan_playback=heard slot={} generation={}",
                    finished.identity.slot, finished.identity.summary_generation
                );
            }
        }
        LanPlaybackEvent::Cancelled(identity) => {
            let Some(playback) = active.remove(&identity.lease) else {
                return;
            };
            if playback.identity != identity || playback.heard_committed {
                return;
            }
            if let Err(error) = store.cancel_summary_playback(&playback.lease) {
                eprintln!("lan_playback=cancel_failed error={error}");
            }
        }
    }
}

#[cfg(all(target_os = "macos", not(test)))]
fn begin_lan_playback(
    request: LanPlaybackRequest,
    store: &mut StateStore,
    ingress: &LanVoiceIngress,
    cache: &CacheStore,
    active: &mut BTreeMap<u64, ActiveHostPlayback>,
) -> Result<(), String> {
    if active.values().any(|playback| {
        playback.identity.slot == request.request.slot
            && playback.identity.request_generation == request.request.request_generation
            && playback.identity.connection_generation == request.request.connection_generation
    }) {
        return Ok(());
    }
    let superseded = active
        .iter()
        .map(|(lease_id, playback)| {
            (
                *lease_id,
                playback.identity,
                playback.lease.clone(),
                playback.heard_committed,
            )
        })
        .collect::<Vec<_>>();
    for (lease_id, identity, lease, heard_committed) in superseded {
        let _ = ingress.cancel_playback(identity);
        if !heard_committed {
            store
                .cancel_summary_playback(&lease)
                .map_err(|error| error.to_string())?;
        }
        active.remove(&lease_id);
    }
    let mut acquired = None;
    for _ in 0..4 {
        let lease_id = (uuid::Uuid::new_v4().as_u128() as u64) & i64::MAX as u64;
        if lease_id == 0 {
            continue;
        }
        match store.acquire_summary_playback(
            request.request.slot,
            request.request.request_generation,
            request.request.connection_generation,
            lease_id,
        ) {
            Ok(Some(lease)) => {
                acquired = Some(lease);
                break;
            }
            Ok(None) => return Ok(()),
            Err(StoreError::Sqlite(rusqlite::Error::SqliteFailure(error, _)))
                if error.code == rusqlite::ErrorCode::ConstraintViolation => {}
            Err(error) => return Err(error.to_string()),
        }
    }
    let lease = acquired.ok_or_else(|| "could not allocate playback lease".to_owned())?;
    let loaded = (|| {
        let id = CacheId::from_reference(&lease.cache_object).map_err(|error| error.to_string())?;
        let bundle = cache.read(&id).map_err(|error| error.to_string())?;
        let info =
            inspect_eiad(bundle.device_eiad.as_slice()).map_err(|error| error.to_string())?;
        if bundle.device_eiad.as_slice().len() > u32::MAX as usize {
            return Err("EIAD exceeds wire size".to_owned());
        }
        Ok::<_, String>((
            zeroize::Zeroizing::new(bundle.device_eiad.as_slice().to_vec()),
            info.samples,
        ))
    })();
    let (eiad, total_samples) = match loaded {
        Ok(loaded) => loaded,
        Err(error) => {
            let _ = store.cancel_summary_playback(&lease);
            return Err(error);
        }
    };
    let identity = PlaybackIdentity {
        slot: lease.slot,
        request_generation: lease.request_generation,
        connection_generation: lease.connection_generation,
        summary_generation: lease.summary_generation,
        lease: lease.lease,
    };
    let begin = PlaybackBegin {
        identity,
        total_bytes: eiad.len() as u32,
        total_samples,
        chunk_bytes: PLAYBACK_CHUNK_BYTES as u16,
        request_nonce: request.request.nonce,
    };
    if !ingress.start_playback(begin, request.source, eiad) {
        let _ = store.cancel_summary_playback(&lease);
        return Err("playback transport is busy".to_owned());
    }
    active.insert(
        lease.lease,
        ActiveHostPlayback {
            lease,
            identity,
            total_samples,
            heard_committed: false,
        },
    );
    Ok(())
}

struct ClientReservation(Arc<AtomicUsize>);

impl Drop for ClientReservation {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::AcqRel);
    }
}

fn reserve_client(active: &AtomicUsize) -> bool {
    active
        .fetch_update(Ordering::AcqRel, Ordering::Acquire, |count| {
            (count < MAX_ACTIVE_CLIENTS).then_some(count + 1)
        })
        .is_ok()
}

fn handle_client(mut stream: UnixStream, snapshot: &HealthSnapshot) -> Result<(), HealthError> {
    let deadline = Instant::now() + CLIENT_IO_TIMEOUT;
    let request_bytes = read_bounded(&mut stream, MAX_REQUEST_BYTES, LimitKind::Request, deadline)?;
    let request = serde_json::from_slice::<HealthRequest>(&request_bytes);
    match request {
        Ok(HealthRequest {
            v: HEALTH_PROTOCOL_VERSION,
            op: HealthOperation::Health,
        }) => write_json_line(&mut stream, snapshot, deadline),
        _ => write_json_line(
            &mut stream,
            &ErrorReply {
                v: HEALTH_PROTOCOL_VERSION,
                ok: false,
                error: "invalid_request",
            },
            deadline,
        ),
    }
}

pub fn query_health(socket_path: &Path) -> Result<HealthSnapshot, HealthError> {
    let socket_identity = validate_private_socket(socket_path)?;
    let mut stream = connect_with_deadline(socket_path, CONNECT_TIMEOUT)
        .map_err(|error| health_context(error, "connect"))?;
    let connected_identity = validate_private_socket(socket_path)?;
    if socket_identity != connected_identity {
        return Err(HealthError::UnsafeSocket);
    }
    let deadline = Instant::now() + CLIENT_IO_TIMEOUT;
    write_json_line(
        &mut stream,
        &HealthRequest {
            v: HEALTH_PROTOCOL_VERSION,
            op: HealthOperation::Health,
        },
        deadline,
    )
    .map_err(|error| health_context(error, "write request"))?;
    let _ = stream.shutdown(std::net::Shutdown::Write);
    let response = read_bounded(
        &mut stream,
        MAX_RESPONSE_BYTES,
        LimitKind::Response,
        deadline,
    )
    .map_err(|error| health_context(error, "read response"))?;
    let health: HealthSnapshot =
        serde_json::from_slice(&response).map_err(|_| HealthError::InvalidResponse)?;
    if health.v != HEALTH_PROTOCOL_VERSION
        || health.socket != format!("run/{HEALTH_SOCKET_NAME}")
        || health.host_version.is_empty()
        || health.pid == 0
    {
        return Err(HealthError::InvalidResponse);
    }
    Ok(health)
}

fn health_context(error: HealthError, operation: &'static str) -> HealthError {
    match error {
        HealthError::Io(source) => HealthError::Io(io::Error::new(
            source.kind(),
            format!("{operation}: {source}"),
        )),
        other => other,
    }
}

fn write_json_line(
    stream: &mut UnixStream,
    value: &impl Serialize,
    deadline: Instant,
) -> Result<(), HealthError> {
    let mut bytes = serde_json::to_vec(value).map_err(|_| HealthError::InvalidResponse)?;
    bytes.push(b'\n');
    let mut offset = 0;
    while offset < bytes.len() {
        stream.set_write_timeout(Some(remaining(deadline)?))?;
        match stream.write(&bytes[offset..]) {
            Ok(0) => return Err(io::Error::from(io::ErrorKind::WriteZero).into()),
            Ok(written) => offset += written,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error.into()),
        }
    }
    Ok(())
}

enum LimitKind {
    Request,
    Response,
}

fn read_bounded(
    stream: &mut UnixStream,
    limit: usize,
    kind: LimitKind,
    deadline: Instant,
) -> Result<Vec<u8>, HealthError> {
    let mut bytes = Vec::with_capacity(limit.min(1024));
    let mut chunk = [0_u8; 1024];
    loop {
        stream.set_read_timeout(Some(remaining(deadline)?))?;
        match stream.read(&mut chunk) {
            Ok(0) => return Err(HealthError::InvalidResponse),
            Ok(read) => {
                bytes.extend_from_slice(&chunk[..read]);
                if bytes.len() > limit {
                    return Err(match kind {
                        LimitKind::Request => HealthError::RequestTooLarge,
                        LimitKind::Response => HealthError::ResponseTooLarge,
                    });
                }
                if let Some(newline) = bytes.iter().position(|byte| *byte == b'\n') {
                    if bytes[newline + 1..]
                        .iter()
                        .any(|byte| !byte.is_ascii_whitespace())
                    {
                        return Err(HealthError::InvalidResponse);
                    }
                    bytes.truncate(newline);
                    return Ok(bytes);
                }
            }
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error.into()),
        }
    }
}

fn remaining(deadline: Instant) -> Result<Duration, HealthError> {
    deadline
        .checked_duration_since(Instant::now())
        .filter(|remaining| *remaining >= MIN_SOCKET_TIMEOUT)
        .ok_or_else(|| {
            io::Error::new(io::ErrorKind::TimedOut, "health I/O deadline exceeded").into()
        })
}

fn bind_private_socket(path: &Path) -> Result<(UnixListener, (u64, u64)), HealthError> {
    bind_private_socket_with_hook(path, || {})
}

fn bind_private_socket_with_hook(
    path: &Path,
    before_publish: impl FnOnce(),
) -> Result<(UnixListener, (u64, u64)), HealthError> {
    validate_socket_path_length(path)?;
    match fs::symlink_metadata(path) {
        Ok(before) => {
            validate_socket_metadata(&before)?;
            match connect_with_deadline(path, CONNECT_TIMEOUT) {
                Ok(_) => return Err(HealthError::AlreadyRunning),
                Err(HealthError::Io(error))
                    if matches!(
                        error.kind(),
                        io::ErrorKind::ConnectionRefused | io::ErrorKind::NotFound
                    ) => {}
                Err(error) => return Err(error),
            }
            remove_socket_if_identity(path, (before.dev(), before.ino()), || {})?;
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error.into()),
    }
    let parent_path = path.parent().ok_or(HealthError::UnsafeSocket)?;
    let file_name = path.file_name().ok_or(HealthError::UnsafeSocket)?;
    let parent = open_owned_directory_chain(parent_path, false)?;
    let destination = CString::new(file_name.as_bytes()).map_err(|_| HealthError::UnsafeSocket)?;
    let capacity = unsafe { mem::zeroed::<libc::sockaddr_un>() }.sun_path.len();
    let available_name_bytes = capacity
        .checked_sub(parent_path.as_os_str().as_bytes().len() + 2)
        .ok_or(HealthError::SocketPathTooLong)?;
    let random = uuid::Uuid::new_v4().simple().to_string();
    let random_bytes = available_name_bytes.saturating_sub(2).min(random.len());
    if random_bytes < 16 {
        return Err(HealthError::SocketPathTooLong);
    }
    let temporary_name = format!(".h{}", &random[..random_bytes]);
    let temporary =
        CString::new(temporary_name.as_bytes()).map_err(|_| HealthError::UnsafeSocket)?;
    let temporary_path = parent_path.join(&temporary_name);
    validate_socket_path_length(&temporary_path)?;

    let listener = UnixListener::bind(&temporary_path)?;
    let listener_identity = socket_identity_at_with_mode(&parent, &temporary, false)?;
    let publish = (|| {
        chmod_socket_nofollow_at(&parent, &temporary)?;
        if socket_identity_at(&parent, &temporary)? != listener_identity {
            return Err(HealthError::UnsafeSocket);
        }
        before_publish();
        if let Err(error) = rename_noreplace(&parent, &temporary, &destination) {
            return Err(if error.kind() == io::ErrorKind::AlreadyExists {
                HealthError::UnsafeSocket
            } else {
                error.into()
            });
        }
        if socket_identity_at(&parent, &destination)? != listener_identity {
            return Err(HealthError::UnsafeSocket);
        }
        parent.sync_all()?;
        Ok(())
    })();
    if let Err(error) = publish {
        let _ = remove_socket_if_identity(&temporary_path, listener_identity, || {});
        let _ = remove_socket_if_identity(path, listener_identity, || {});
        return Err(error);
    }
    Ok((listener, listener_identity))
}

fn chmod_socket_nofollow_at(parent: &File, name: &CString) -> Result<(), HealthError> {
    if unsafe {
        libc::fchmodat(
            parent.as_raw_fd(),
            name.as_ptr(),
            0o600,
            libc::AT_SYMLINK_NOFOLLOW,
        )
    } != 0
    {
        return Err(io::Error::last_os_error().into());
    }
    Ok(())
}

fn remove_socket_if_identity(
    path: &Path,
    expected: (u64, u64),
    before_rename: impl FnOnce(),
) -> Result<(), HealthError> {
    let parent_path = path.parent().ok_or(HealthError::UnsafeSocket)?;
    let file_name = path.file_name().ok_or(HealthError::UnsafeSocket)?;
    let parent = open_owned_directory_chain(parent_path, false)?;
    let source = CString::new(file_name.as_bytes()).map_err(|_| HealthError::UnsafeSocket)?;
    let retired_name = format!(".{HEALTH_SOCKET_NAME}.{}.retired", uuid::Uuid::new_v4());
    let retired = CString::new(retired_name).map_err(|_| HealthError::UnsafeSocket)?;

    before_rename();
    rename_noreplace(&parent, &source, &retired)?;

    let moved_identity = socket_identity_at(&parent, &retired);
    if !matches!(moved_identity, Ok(identity) if identity == expected) {
        let _ = rename_noreplace(&parent, &retired, &source);
        return Err(HealthError::UnsafeSocket);
    }
    if unsafe { libc::unlinkat(parent.as_raw_fd(), retired.as_ptr(), 0) } != 0 {
        let error = io::Error::last_os_error();
        let _ = rename_noreplace(&parent, &retired, &source);
        return Err(error.into());
    }
    parent.sync_all()?;
    Ok(())
}

fn socket_identity_at(parent: &File, name: &CString) -> Result<(u64, u64), HealthError> {
    socket_identity_at_with_mode(parent, name, true)
}

fn socket_identity_at_with_mode(
    parent: &File,
    name: &CString,
    require_private_mode: bool,
) -> Result<(u64, u64), HealthError> {
    let mut status: libc::stat = unsafe { mem::zeroed() };
    if unsafe {
        libc::fstatat(
            parent.as_raw_fd(),
            name.as_ptr(),
            &raw mut status,
            libc::AT_SYMLINK_NOFOLLOW,
        )
    } != 0
    {
        return Err(io::Error::last_os_error().into());
    }
    socket_identity_from_status(&status, require_private_mode)
}

fn socket_identity_from_status(
    status: &libc::stat,
    require_private_mode: bool,
) -> Result<(u64, u64), HealthError> {
    let uid = unsafe { libc::geteuid() };
    let file_type = status.st_mode & libc::S_IFMT;
    if file_type != libc::S_IFSOCK
        || status.st_uid != uid
        || (require_private_mode && status.st_mode & 0o077 != 0)
    {
        return Err(HealthError::UnsafeSocket);
    }
    #[cfg(target_os = "macos")]
    let identity = (status.st_dev as u64, status.st_ino);
    #[cfg(target_os = "linux")]
    let identity = (status.st_dev, status.st_ino);
    #[cfg(not(any(target_os = "macos", target_os = "linux")))]
    let identity = (status.st_dev as u64, status.st_ino as u64);
    Ok(identity)
}

#[cfg(target_os = "macos")]
fn rename_noreplace(parent: &File, source: &CString, destination: &CString) -> io::Result<()> {
    if unsafe {
        libc::renameatx_np(
            parent.as_raw_fd(),
            source.as_ptr(),
            parent.as_raw_fd(),
            destination.as_ptr(),
            libc::RENAME_EXCL,
        )
    } == 0
    {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

#[cfg(target_os = "linux")]
fn rename_noreplace(parent: &File, source: &CString, destination: &CString) -> io::Result<()> {
    if unsafe {
        libc::renameat2(
            parent.as_raw_fd(),
            source.as_ptr(),
            parent.as_raw_fd(),
            destination.as_ptr(),
            libc::RENAME_NOREPLACE,
        )
    } == 0
    {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn rename_noreplace(_parent: &File, _source: &CString, _destination: &CString) -> io::Result<()> {
    Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "atomic no-replace rename is unavailable",
    ))
}

fn validate_private_socket(path: &Path) -> Result<(u64, u64), HealthError> {
    validate_socket_path_length(path)?;
    let metadata = fs::symlink_metadata(path)?;
    validate_socket_metadata(&metadata)?;
    Ok((metadata.dev(), metadata.ino()))
}

fn validate_socket_metadata(metadata: &fs::Metadata) -> Result<(), HealthError> {
    let uid = unsafe { libc::geteuid() };
    if !metadata.file_type().is_socket()
        || metadata.uid() != uid
        || metadata.permissions().mode() & 0o077 != 0
    {
        return Err(HealthError::UnsafeSocket);
    }
    Ok(())
}

fn validate_socket_path_length(path: &Path) -> Result<(), HealthError> {
    let bytes = path.as_os_str().as_bytes();
    let capacity = unsafe { mem::zeroed::<libc::sockaddr_un>() }.sun_path.len();
    if bytes.is_empty() || bytes.contains(&0) || bytes.len() >= capacity {
        return Err(HealthError::SocketPathTooLong);
    }
    Ok(())
}

fn connect_with_deadline(path: &Path, timeout: Duration) -> Result<UnixStream, HealthError> {
    validate_socket_path_length(path)?;
    let descriptor = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM, 0) };
    if descriptor < 0 {
        return Err(io::Error::last_os_error().into());
    }
    let result = connect_descriptor(descriptor, path, timeout);
    if result.is_err() {
        unsafe { libc::close(descriptor) };
    }
    result.map(|()| unsafe { UnixStream::from_raw_fd(descriptor) })
}

fn connect_descriptor(
    descriptor: RawFd,
    path: &Path,
    timeout: Duration,
) -> Result<(), HealthError> {
    let old_flags = unsafe { libc::fcntl(descriptor, libc::F_GETFL) };
    if old_flags < 0
        || unsafe { libc::fcntl(descriptor, libc::F_SETFL, old_flags | libc::O_NONBLOCK) } < 0
        || unsafe { libc::fcntl(descriptor, libc::F_SETFD, libc::FD_CLOEXEC) } < 0
    {
        return Err(io::Error::last_os_error().into());
    }
    let mut address: libc::sockaddr_un = unsafe { mem::zeroed() };
    address.sun_family = libc::AF_UNIX as libc::sa_family_t;
    let bytes = path.as_os_str().as_bytes();
    for (target, source) in address.sun_path.iter_mut().zip(bytes) {
        *target = *source as libc::c_char;
    }
    let address_length = (mem::offset_of!(libc::sockaddr_un, sun_path) + bytes.len() + 1)
        .try_into()
        .map_err(|_| HealthError::SocketPathTooLong)?;
    #[cfg(target_os = "macos")]
    {
        address.sun_len =
            u8::try_from(address_length).map_err(|_| HealthError::SocketPathTooLong)?;
    }
    let connected = unsafe {
        libc::connect(
            descriptor,
            (&raw const address).cast::<libc::sockaddr>(),
            address_length,
        )
    };
    if connected != 0 {
        let error = io::Error::last_os_error();
        if error.raw_os_error() != Some(libc::EINPROGRESS) {
            return Err(error.into());
        }
        let mut poll_descriptor = libc::pollfd {
            fd: descriptor,
            events: libc::POLLOUT,
            revents: 0,
        };
        let timeout_ms = timeout.as_millis().min(i32::MAX as u128) as i32;
        let ready = unsafe { libc::poll(&mut poll_descriptor, 1, timeout_ms) };
        if ready == 0 {
            return Err(
                io::Error::new(io::ErrorKind::TimedOut, "connect deadline exceeded").into(),
            );
        }
        if ready < 0 {
            return Err(io::Error::last_os_error().into());
        }
        let mut socket_error = 0;
        let mut socket_error_length = mem::size_of::<libc::c_int>() as libc::socklen_t;
        if unsafe {
            libc::getsockopt(
                descriptor,
                libc::SOL_SOCKET,
                libc::SO_ERROR,
                (&raw mut socket_error).cast(),
                &mut socket_error_length,
            )
        } != 0
        {
            return Err(io::Error::last_os_error().into());
        }
        if socket_error != 0 {
            return Err(io::Error::from_raw_os_error(socket_error).into());
        }
    }
    if unsafe { libc::fcntl(descriptor, libc::F_SETFL, old_flags) } < 0 {
        return Err(io::Error::last_os_error().into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::symlink;
    use std::sync::Barrier;
    use tempfile::tempdir;

    fn start_daemon() -> (
        tempfile::TempDir,
        AppPaths,
        Arc<AtomicBool>,
        thread::JoinHandle<()>,
    ) {
        let temp = tempdir().unwrap();
        let paths = AppPaths::from_root(temp.path().join("state"));
        let daemon = HostDaemon::open(&paths).unwrap();
        let shutdown = Arc::new(AtomicBool::new(false));
        let worker_shutdown = Arc::clone(&shutdown);
        let worker = thread::spawn(move || daemon.serve_until(worker_shutdown).unwrap());
        (temp, paths, shutdown, worker)
    }

    fn stop_daemon(shutdown: Arc<AtomicBool>, worker: thread::JoinHandle<()>) {
        shutdown.store(true, Ordering::Release);
        worker.join().unwrap();
    }

    #[test]
    fn health_round_trip_is_private_and_bounded() {
        let (_temp, paths, shutdown, worker) = start_daemon();
        let socket = paths.runtime_directory.join(HEALTH_SOCKET_NAME);
        let health = query_health(&socket).unwrap();

        assert_eq!(health.status, HealthState::Ready);
        assert_eq!(health.database_schema, crate::store::SCHEMA_VERSION);
        assert_eq!(health.socket, "run/host.sock");
        assert_eq!(
            fs::symlink_metadata(&socket).unwrap().permissions().mode() & 0o777,
            0o600
        );

        stop_daemon(shutdown, worker);
        assert!(!socket.exists());
    }

    #[test]
    fn health_remains_responsive_while_the_observer_catalog_is_busy() {
        let temp = tempdir().unwrap();
        let paths = AppPaths::from_root(temp.path().join("state"));
        let mut daemon = HostDaemon::open(&paths).unwrap();
        daemon
            .store
            .set_binding(1, None, "019fa972-5cfa-75e1-9008-0b17ade9a347")
            .unwrap();
        let observer_busy = Arc::new(Barrier::new(2));
        let hook_barrier = Arc::clone(&observer_busy);
        let mut observer =
            RolloutObserver::new(crate::codex_catalog::CodexTaskCatalog::from_paths(
                temp.path().join("fixture-codex"),
                temp.path().join("fixture-snapshots"),
            ));
        observer.set_before_catalog_poll(Box::new(move || {
            hook_barrier.wait();
            thread::sleep(Duration::from_millis(1_000));
        }));
        daemon.observer = Some(observer);

        let shutdown = Arc::new(AtomicBool::new(false));
        let worker_shutdown = Arc::clone(&shutdown);
        let worker = thread::spawn(move || daemon.serve_until(worker_shutdown).unwrap());
        observer_busy.wait();
        thread::sleep(Duration::from_millis(20));
        let started = Instant::now();
        let health = query_health(&paths.runtime_directory.join(HEALTH_SOCKET_NAME)).unwrap();

        assert_eq!(health.status, HealthState::Ready);
        assert!(started.elapsed() < CONNECT_TIMEOUT);
        stop_daemon(shutdown, worker);
    }

    #[test]
    fn active_instance_and_unsafe_stale_paths_fail_closed() {
        let temp = tempdir().unwrap();
        let paths = AppPaths::from_root(temp.path().join("state"));
        let owner = HostDaemon::open(&paths).unwrap();
        assert!(matches!(
            HostDaemon::open(&paths),
            Err(HealthError::Store(StoreError::AlreadyRunning))
        ));
        drop(owner);

        let socket = paths.runtime_directory.join(HEALTH_SOCKET_NAME);
        fs::write(&socket, b"not a socket").unwrap();
        assert!(matches!(
            HostDaemon::open(&paths),
            Err(HealthError::UnsafeSocket)
        ));
        fs::remove_file(&socket).unwrap();
        let target = paths.runtime_directory.join("target");
        fs::write(&target, b"target").unwrap();
        symlink(&target, &socket).unwrap();
        assert!(matches!(
            HostDaemon::open(&paths),
            Err(HealthError::UnsafeSocket)
        ));
    }

    #[test]
    fn stale_socket_is_reclaimed_only_after_store_lock_is_owned() {
        let temp = tempdir().unwrap();
        let paths = AppPaths::from_root(temp.path().join("state"));
        paths.prepare().unwrap();
        let socket = paths.runtime_directory.join(HEALTH_SOCKET_NAME);
        drop(UnixListener::bind(&socket).unwrap());
        fs::set_permissions(&socket, fs::Permissions::from_mode(0o600)).unwrap();

        let daemon = HostDaemon::open(&paths).unwrap();
        assert_eq!(daemon.snapshot().status, HealthState::Ready);
    }

    #[test]
    fn malformed_oversize_and_slow_clients_do_not_block_health() {
        let (_temp, paths, shutdown, worker) = start_daemon();
        let socket = paths.runtime_directory.join(HEALTH_SOCKET_NAME);

        let mut malformed = UnixStream::connect(&socket).unwrap();
        malformed
            .write_all(b"{\"v\":1,\"op\":\"unknown\"}\n")
            .unwrap();
        malformed.shutdown(std::net::Shutdown::Write).unwrap();
        let mut reply = String::new();
        malformed.read_to_string(&mut reply).unwrap();
        assert!(reply.contains("invalid_request"));

        let mut oversized = UnixStream::connect(&socket).unwrap();
        oversized
            .write_all(&vec![b'x'; MAX_REQUEST_BYTES + 1])
            .unwrap();
        oversized.shutdown(std::net::Shutdown::Write).unwrap();

        let _slow = UnixStream::connect(&socket).unwrap();
        let started = std::time::Instant::now();
        assert!(query_health(&socket).is_ok());
        assert!(started.elapsed() < Duration::from_millis(500));

        stop_daemon(shutdown, worker);
    }

    #[test]
    fn drip_clients_cannot_extend_the_absolute_deadline_or_exhaust_slots() {
        let (_temp, paths, shutdown, worker) = start_daemon();
        let socket = paths.runtime_directory.join(HEALTH_SOCKET_NAME);
        let barrier = Arc::new(Barrier::new(MAX_ACTIVE_CLIENTS + 1));
        let mut clients = Vec::new();
        for _ in 0..MAX_ACTIVE_CLIENTS {
            let socket = socket.clone();
            let barrier = Arc::clone(&barrier);
            clients.push(thread::spawn(move || {
                let mut stream = UnixStream::connect(socket).unwrap();
                barrier.wait();
                for _ in 0..40 {
                    if stream.write_all(b"{").is_err() {
                        break;
                    }
                    thread::sleep(Duration::from_millis(100));
                }
            }));
        }
        barrier.wait();
        thread::sleep(CLIENT_IO_TIMEOUT + Duration::from_millis(200));

        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            if query_health(&socket).is_ok() {
                break;
            }
            assert!(Instant::now() < deadline, "health slots did not recover");
            thread::sleep(Duration::from_millis(25));
        }

        for client in clients {
            client.join().unwrap();
        }
        stop_daemon(shutdown, worker);
    }

    #[test]
    fn concurrent_clients_receive_the_same_authoritative_snapshot() {
        let (_temp, paths, shutdown, worker) = start_daemon();
        let socket = paths.runtime_directory.join(HEALTH_SOCKET_NAME);
        let barrier = Arc::new(Barrier::new(9));
        let mut clients = Vec::new();
        for _ in 0..8 {
            let barrier = Arc::clone(&barrier);
            let socket = socket.clone();
            clients.push(thread::spawn(move || {
                barrier.wait();
                query_health(&socket).unwrap()
            }));
        }
        barrier.wait();
        let snapshots: Vec<_> = clients
            .into_iter()
            .map(|client| client.join().unwrap())
            .collect();
        assert!(snapshots.windows(2).all(|pair| pair[0] == pair[1]));
        stop_daemon(shutdown, worker);
    }

    #[test]
    fn drop_does_not_unlink_a_replacement_path() {
        let temp = tempdir().unwrap();
        let paths = AppPaths::from_root(temp.path().join("state"));
        let daemon = HostDaemon::open(&paths).unwrap();
        let socket = paths.runtime_directory.join(HEALTH_SOCKET_NAME);
        fs::remove_file(&socket).unwrap();
        fs::write(&socket, b"replacement").unwrap();

        drop(daemon);
        assert_eq!(fs::read(&socket).unwrap(), b"replacement");
    }

    #[test]
    fn conditional_cleanup_preserves_a_path_replaced_during_the_race_window() {
        let temp = tempdir().unwrap();
        let paths = AppPaths::from_root(temp.path().join("state"));
        paths.prepare().unwrap();
        let socket = paths.runtime_directory.join(HEALTH_SOCKET_NAME);
        drop(UnixListener::bind(&socket).unwrap());
        fs::set_permissions(&socket, fs::Permissions::from_mode(0o600)).unwrap();
        let metadata = fs::symlink_metadata(&socket).unwrap();
        let expected = (metadata.dev(), metadata.ino());

        let result = remove_socket_if_identity(&socket, expected, || {
            fs::remove_file(&socket).unwrap();
            fs::write(&socket, b"replacement").unwrap();
        });

        assert!(matches!(result, Err(HealthError::UnsafeSocket)));
        assert_eq!(fs::read(&socket).unwrap(), b"replacement");
    }

    #[test]
    fn socket_publication_never_adopts_or_removes_a_racing_final_path() {
        let temp = tempdir().unwrap();
        let paths = AppPaths::from_root(temp.path().join("state"));
        paths.prepare().unwrap();
        let socket = paths.runtime_directory.join(HEALTH_SOCKET_NAME);

        let result = bind_private_socket_with_hook(&socket, || {
            fs::write(&socket, b"replacement").unwrap();
        });

        assert!(matches!(result, Err(HealthError::UnsafeSocket)));
        assert_eq!(fs::read(&socket).unwrap(), b"replacement");
        let mut entries: Vec<_> = fs::read_dir(&paths.runtime_directory)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        entries.sort();
        assert_eq!(entries, vec![HEALTH_SOCKET_NAME]);
    }
}

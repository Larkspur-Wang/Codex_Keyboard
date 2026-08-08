use std::collections::{BTreeMap, BTreeSet};
use std::ffi::{CStr, CString, OsStr, OsString};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};

use aes_gcm::aead::rand_core::{OsRng, RngCore};
use aes_gcm::aead::{Aead, KeyInit, Payload};
use aes_gcm::{Aes256Gcm, Nonce};
use fs2::FileExt;
use hkdf::Hkdf;
use sha2::{Digest, Sha256};
use thiserror::Error;
use zeroize::Zeroizing;

use crate::paths::{ExplicitFileLock, open_file_at, open_owned_directory_chain, secure_directory};
use crate::secrets::{KeychainAccounts, SecretBytes, SecretStore, SecretStoreError};

const CACHE_MAGIC: &[u8; 8] = b"ECICACHE";
const CACHE_VERSION: u8 = 1;
const HEADER_BYTES: usize = 48;
const TAG_BYTES: u64 = 16;
const CACHE_KEY_BYTES: usize = 32;
const SALT_BYTES: usize = 16;
const NONCE_BYTES: usize = 12;
const TASK_HASH_HEX_BYTES: usize = 64;
const TEMP_PREFIX: &str = ".tmp-";
const CACHE_LOCK_FILE: &str = ".cache.lock";
const KEY_MARKER_FILE: &str = ".key-check.enc";
const KEY_MARKER_TEMP_PREFIX: &str = ".key-check.tmp-";
const KEY_MARKER_PLAINTEXT: &[u8] = b"easy-codex-input-cache-key-v1";

pub const DEFAULT_MAX_CACHE_BYTES: u64 = 512 * 1024 * 1024;
pub const DEFAULT_MAX_GENERATIONS: usize = 512;
pub const MAX_GENERATION_PLAINTEXT_BYTES: u64 = 32 * 1024 * 1024;
const MAX_MANIFEST_BYTES: u64 = 1024 * 1024;
const MAX_AUDIO_OBJECT_BYTES: u64 = 16 * 1024 * 1024;

#[derive(Debug, Error)]
pub enum CacheError {
    #[error("cache I/O operation failed")]
    Io(#[from] std::io::Error),
    #[error("cache key is missing or has an invalid length")]
    InvalidKey,
    #[error("cache generation must be greater than zero")]
    InvalidGeneration,
    #[error("cache object exceeds its size limit")]
    ObjectTooLarge,
    #[error("cache generation exceeds its size limit")]
    GenerationTooLarge,
    #[error("cache capacity limit would be exceeded")]
    CapacityExceeded,
    #[error("cache generation is already published")]
    AlreadyPublished,
    #[error("another process already owns the cache")]
    AlreadyOpen,
    #[error("cache object is missing")]
    MissingObject,
    #[error("cache reference is invalid")]
    InvalidReference,
    #[error("a referenced cache generation is missing")]
    MissingReference,
    #[error("cache layout or object authentication is invalid")]
    Corrupt,
    #[error("cache encryption operation failed")]
    Crypto,
    #[error("cache operation lock is unavailable")]
    LockUnavailable,
    #[error(transparent)]
    SecretStore(#[from] SecretStoreError),
    #[cfg(test)]
    #[error("injected cache publication fault")]
    InjectedFault,
}

pub struct CacheKey(Zeroizing<[u8; CACHE_KEY_BYTES]>);

impl CacheKey {
    fn from_secret(secret: &SecretBytes) -> Result<Self, CacheError> {
        let bytes: [u8; CACHE_KEY_BYTES] = secret
            .as_slice()
            .try_into()
            .map_err(|_| CacheError::InvalidKey)?;
        Ok(Self(Zeroizing::new(bytes)))
    }

    fn load<S: SecretStore>(store: &S, accounts: &KeychainAccounts) -> Result<Self, CacheError> {
        let secret = store
            .get(&accounts.cache_key)?
            .ok_or(CacheError::InvalidKey)?;
        Self::from_secret(&secret)
    }

    fn initialize<S: SecretStore>(
        store: &S,
        accounts: &KeychainAccounts,
    ) -> Result<Self, CacheError> {
        if let Some(secret) = store.get(&accounts.cache_key)? {
            return Self::from_secret(&secret);
        }
        let mut candidate = Zeroizing::new([0_u8; CACHE_KEY_BYTES]);
        OsRng.fill_bytes(candidate.as_mut());
        match store.create(&accounts.cache_key, candidate.as_slice()) {
            Ok(()) | Err(SecretStoreError::AlreadyExists) => {}
            Err(error) => return Err(error.into()),
        }
        drop(candidate);

        let stored = store
            .get(&accounts.cache_key)?
            .ok_or(CacheError::InvalidKey)?;
        Self::from_secret(&stored)
    }

    #[cfg(test)]
    fn from_bytes(bytes: [u8; CACHE_KEY_BYTES]) -> Self {
        Self(Zeroizing::new(bytes))
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct CacheId {
    task_hash: [u8; 32],
    generation: u64,
}

impl CacheId {
    pub fn for_task(task_id: &str, generation: u64) -> Result<Self, CacheError> {
        if generation == 0 {
            return Err(CacheError::InvalidGeneration);
        }
        Ok(Self {
            task_hash: Sha256::digest(task_id.as_bytes()).into(),
            generation,
        })
    }

    pub fn from_reference(reference: &str) -> Result<Self, CacheError> {
        let mut components = reference.split('/');
        let task_hash = components.next().ok_or(CacheError::InvalidReference)?;
        let generation = components.next().ok_or(CacheError::InvalidReference)?;
        if components.next().is_some()
            || !is_task_hash(task_hash)
            || !is_canonical_generation(generation)
        {
            return Err(CacheError::InvalidReference);
        }
        let generation = generation
            .parse::<u64>()
            .map_err(|_| CacheError::InvalidReference)?;
        if generation == 0 {
            return Err(CacheError::InvalidReference);
        }
        let mut decoded = [0_u8; 32];
        for (index, pair) in task_hash.as_bytes().chunks_exact(2).enumerate() {
            decoded[index] = decode_hex_pair(pair).ok_or(CacheError::InvalidReference)?;
        }
        Ok(Self {
            task_hash: decoded,
            generation,
        })
    }

    pub fn reference(&self) -> String {
        format!("{}/{}", encode_hex(&self.task_hash), self.generation)
    }

    fn task_directory_name(&self) -> String {
        encode_hex(&self.task_hash)
    }

    fn generation_name(&self) -> String {
        self.generation.to_string()
    }
}

#[derive(Clone, Copy)]
pub struct CacheBundle<'a> {
    pub manifest_json: &'a [u8],
    pub qwen_wav: &'a [u8],
    pub device_eiad: &'a [u8],
}

pub struct CachePlaintext(Zeroizing<Vec<u8>>);

impl CachePlaintext {
    pub fn as_slice(&self) -> &[u8] {
        self.0.as_slice()
    }
}

pub struct DecryptedCacheBundle {
    pub manifest_json: CachePlaintext,
    pub qwen_wav: CachePlaintext,
    pub device_eiad: CachePlaintext,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CacheLimits {
    pub max_total_bytes: u64,
    pub max_generations: usize,
}

impl Default for CacheLimits {
    fn default() -> Self {
        Self {
            max_total_bytes: DEFAULT_MAX_CACHE_BYTES,
            max_generations: DEFAULT_MAX_GENERATIONS,
        }
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct CacheAudit {
    pub finalized_generations: usize,
    pub temporary_generations: usize,
    pub encrypted_bytes: u64,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct CleanupReport {
    pub temporary_generations_removed: usize,
    pub orphan_generations_removed: usize,
    pub encrypted_bytes_remaining: u64,
}

pub struct CacheStore {
    root: PathBuf,
    root_directory: File,
    key: CacheKey,
    limits: CacheLimits,
    operation_gate: Mutex<()>,
    _process_lock: ExplicitFileLock,
}

impl CacheStore {
    pub fn initialize<S: SecretStore>(
        root: &Path,
        store: &S,
        accounts: &KeychainAccounts,
        limits: CacheLimits,
    ) -> Result<Self, CacheError> {
        let (root_directory, process_lock) = prepare_root_and_lock(root, limits)?;
        cleanup_key_marker_temporaries(&root_directory)?;
        let marker_exists = entry_exists(&root_directory, OsStr::new(KEY_MARKER_FILE))?;
        let key = if marker_exists {
            CacheKey::load(store, accounts)?
        } else {
            ensure_uninitialized_root_is_empty(&root_directory)?;
            let key = CacheKey::initialize(store, accounts)?;
            write_key_marker(&root_directory, &key)?;
            key
        };
        verify_key_marker(&root_directory, &key)?;
        Ok(Self::build(root, root_directory, process_lock, key, limits))
    }

    pub fn open_existing<S: SecretStore>(
        root: &Path,
        store: &S,
        accounts: &KeychainAccounts,
        limits: CacheLimits,
    ) -> Result<Self, CacheError> {
        if !root.exists() {
            return Err(CacheError::InvalidKey);
        }
        let (root_directory, process_lock) = prepare_root_and_lock(root, limits)?;
        cleanup_key_marker_temporaries(&root_directory)?;
        let key = CacheKey::load(store, accounts)?;
        verify_key_marker(&root_directory, &key)?;
        Ok(Self::build(root, root_directory, process_lock, key, limits))
    }

    fn build(
        root: &Path,
        root_directory: File,
        process_lock: ExplicitFileLock,
        key: CacheKey,
        limits: CacheLimits,
    ) -> Self {
        Self {
            root: root.to_path_buf(),
            root_directory,
            key,
            limits,
            operation_gate: Mutex::new(()),
            _process_lock: process_lock,
        }
    }

    #[cfg(test)]
    fn open_with_key_for_test(
        root: &Path,
        key: CacheKey,
        limits: CacheLimits,
    ) -> Result<Self, CacheError> {
        let (root_directory, process_lock) = prepare_root_and_lock(root, limits)?;
        cleanup_key_marker_temporaries(&root_directory)?;
        if entry_exists(&root_directory, OsStr::new(KEY_MARKER_FILE))? {
            verify_key_marker(&root_directory, &key)?;
        } else {
            ensure_uninitialized_root_is_empty(&root_directory)?;
            write_key_marker(&root_directory, &key)?;
        }
        Ok(Self::build(root, root_directory, process_lock, key, limits))
    }

    #[cfg(test)]
    pub(crate) fn open_with_raw_key_for_test(
        root: &Path,
        key: [u8; CACHE_KEY_BYTES],
        limits: CacheLimits,
    ) -> Result<Self, CacheError> {
        Self::open_with_key_for_test(root, CacheKey::from_bytes(key), limits)
    }

    fn gate(&self) -> Result<MutexGuard<'_, ()>, CacheError> {
        self.operation_gate
            .lock()
            .map_err(|_| CacheError::LockUnavailable)
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn publish(&self, id: &CacheId, bundle: CacheBundle<'_>) -> Result<(), CacheError> {
        let _gate = self.gate()?;
        self.publish_observed(id, bundle, true, |_| Ok(()))
    }

    pub fn publish_or_verify(
        &self,
        id: &CacheId,
        bundle: CacheBundle<'_>,
    ) -> Result<(), CacheError> {
        let _gate = self.gate()?;
        self.publish_or_verify_unlocked(id, bundle)
    }

    pub(crate) fn publish_or_verify_with<R, E, F>(
        &self,
        id: &CacheId,
        bundle: CacheBundle<'_>,
        after_authenticated_read: F,
    ) -> Result<Result<R, E>, CacheError>
    where
        F: FnOnce(&DecryptedCacheBundle) -> Result<R, E>,
    {
        let _gate = self.gate()?;
        self.publish_or_verify_unlocked(id, bundle)?;
        let authenticated = self.read_unlocked(id)?;
        Ok(after_authenticated_read(&authenticated))
    }

    fn publish_or_verify_unlocked(
        &self,
        id: &CacheId,
        bundle: CacheBundle<'_>,
    ) -> Result<(), CacheError> {
        match self.read_unlocked(id) {
            Ok(existing) => {
                if existing.manifest_json.as_slice() == bundle.manifest_json
                    && existing.qwen_wav.as_slice() == bundle.qwen_wav
                    && existing.device_eiad.as_slice() == bundle.device_eiad
                {
                    Ok(())
                } else {
                    Err(CacheError::AlreadyPublished)
                }
            }
            Err(CacheError::MissingObject) => {
                self.publish_observed(id, bundle, true, |_| Ok(()))?;
                let existing = self.read_unlocked(id)?;
                if existing.manifest_json.as_slice() == bundle.manifest_json
                    && existing.qwen_wav.as_slice() == bundle.qwen_wav
                    && existing.device_eiad.as_slice() == bundle.device_eiad
                {
                    Ok(())
                } else {
                    Err(CacheError::Corrupt)
                }
            }
            Err(error) => Err(error),
        }
    }

    pub fn read(&self, id: &CacheId) -> Result<DecryptedCacheBundle, CacheError> {
        let _gate = self.gate()?;
        self.read_unlocked(id)
    }

    pub(crate) fn read_with<R, E, F>(
        &self,
        id: &CacheId,
        after_authenticated_read: F,
    ) -> Result<Result<R, E>, CacheError>
    where
        F: FnOnce(&DecryptedCacheBundle) -> Result<R, E>,
    {
        let _gate = self.gate()?;
        let authenticated = self.read_unlocked(id)?;
        Ok(after_authenticated_read(&authenticated))
    }

    fn read_unlocked(&self, id: &CacheId) -> Result<DecryptedCacheBundle, CacheError> {
        let task_directory =
            match open_directory_at(&self.root_directory, OsStr::new(&id.task_directory_name())) {
                Ok(directory) => directory,
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                    return Err(CacheError::MissingObject);
                }
                Err(error) => return Err(CacheError::Io(error)),
            };
        let directory = match open_directory_at(&task_directory, OsStr::new(&id.generation_name()))
        {
            Ok(directory) => directory,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                return Err(CacheError::MissingObject);
            }
            Err(error) => return Err(CacheError::Io(error)),
        };
        let manifest_json = self.read_object(&directory, id, ObjectKind::Manifest)?;
        let qwen_wav = self.read_object(&directory, id, ObjectKind::QwenWav)?;
        let device_eiad = self.read_object(&directory, id, ObjectKind::DeviceEiad)?;
        ensure_no_extra_entries(&directory)?;
        Ok(DecryptedCacheBundle {
            manifest_json,
            qwen_wav,
            device_eiad,
        })
    }

    pub fn audit(&self) -> Result<CacheAudit, CacheError> {
        let _gate = self.gate()?;
        Ok(self.scan_unlocked()?.audit)
    }

    pub fn reconcile(&self, references: &BTreeSet<String>) -> Result<CleanupReport, CacheError> {
        let _gate = self.gate()?;
        self.reconcile_unlocked(references)
    }

    pub(crate) fn reconcile_with<E, F>(
        &self,
        references: F,
    ) -> Result<Result<CleanupReport, E>, CacheError>
    where
        F: FnOnce() -> Result<BTreeSet<String>, E>,
    {
        let _gate = self.gate()?;
        let references = match references() {
            Ok(references) => references,
            Err(error) => return Ok(Err(error)),
        };
        self.reconcile_unlocked(&references).map(Ok)
    }

    fn reconcile_unlocked(
        &self,
        references: &BTreeSet<String>,
    ) -> Result<CleanupReport, CacheError> {
        let expected = references
            .iter()
            .map(|reference| CacheId::from_reference(reference))
            .collect::<Result<BTreeSet<_>, _>>()?;
        let scan = self.scan_unlocked()?;
        if !expected.is_subset(&scan.finalized) {
            return Err(CacheError::MissingReference);
        }

        // Authenticate every retained object before mutating anything on disk.
        for id in &expected {
            drop(self.read_unlocked(id)?);
        }

        let mut report = CleanupReport::default();
        for temporary in &scan.temporaries {
            self.remove_temporary(temporary)?;
            report.temporary_generations_removed += 1;
        }
        for id in scan.finalized.difference(&expected) {
            let identity = scan
                .task_identities
                .get(&id.task_directory_name())
                .ok_or(CacheError::Corrupt)?;
            self.remove_orphan_atomically(id, *identity)?;
            report.orphan_generations_removed += 1;
        }
        self.sync_all_task_directories()?;
        report.encrypted_bytes_remaining = self.scan_unlocked()?.audit.encrypted_bytes;
        Ok(report)
    }

    fn publish_observed<F>(
        &self,
        id: &CacheId,
        bundle: CacheBundle<'_>,
        cleanup_on_error: bool,
        mut checkpoint: F,
    ) -> Result<(), CacheError>
    where
        F: FnMut(PublishCheckpoint) -> Result<(), CacheError>,
    {
        validate_bundle(&bundle)?;
        let predicted_bytes = encoded_bundle_bytes(&bundle)?;
        let current = self.scan_unlocked()?.audit;
        if current.finalized_generations >= self.limits.max_generations
            || current
                .encrypted_bytes
                .checked_add(predicted_bytes)
                .is_none_or(|total| total > self.limits.max_total_bytes)
        {
            return Err(CacheError::CapacityExceeded);
        }

        let task_name = id.task_directory_name();
        let (task_handle, created) =
            open_or_create_directory_at(&self.root_directory, OsStr::new(&task_name))?;
        if created {
            self.root_directory.sync_all()?;
        }
        if entry_exists(&task_handle, OsStr::new(&id.generation_name()))? {
            return Err(CacheError::AlreadyPublished);
        }

        let temporary_name = format!("{TEMP_PREFIX}{}", uuid::Uuid::new_v4());
        let temporary_handle = create_directory_at(&task_handle, OsStr::new(&temporary_name))?;
        let mut renamed = false;
        let result = (|| {
            self.write_object(
                &temporary_handle,
                id,
                ObjectKind::Manifest,
                bundle.manifest_json,
            )?;
            checkpoint(PublishCheckpoint::ManifestSynced)?;
            self.write_object(&temporary_handle, id, ObjectKind::QwenWav, bundle.qwen_wav)?;
            checkpoint(PublishCheckpoint::QwenWavSynced)?;
            self.write_object(
                &temporary_handle,
                id,
                ObjectKind::DeviceEiad,
                bundle.device_eiad,
            )?;
            checkpoint(PublishCheckpoint::DeviceEiadSynced)?;
            temporary_handle.sync_all()?;
            checkpoint(PublishCheckpoint::TemporaryDirectorySynced)?;

            rename_noreplace(
                &task_handle,
                OsStr::new(&temporary_name),
                &task_handle,
                OsStr::new(&id.generation_name()),
            )?;
            renamed = true;
            checkpoint(PublishCheckpoint::Renamed)?;
            task_handle.sync_all()?;
            checkpoint(PublishCheckpoint::ParentDirectorySynced)
        })();

        if result.is_err() && !renamed && cleanup_on_error {
            let _ = remove_flat_directory_at(&task_handle, OsStr::new(&temporary_name));
        }
        result
    }

    fn write_object(
        &self,
        directory: &File,
        id: &CacheId,
        kind: ObjectKind,
        plaintext: &[u8],
    ) -> Result<(), CacheError> {
        let encrypted = encrypt_object(&self.key, id, kind, plaintext)?;
        let name = CString::new(kind.file_name()).map_err(|_| CacheError::Corrupt)?;
        let descriptor = unsafe {
            libc::openat(
                directory.as_raw_fd(),
                name.as_ptr(),
                libc::O_CREAT | libc::O_EXCL | libc::O_WRONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                0o600,
            )
        };
        if descriptor < 0 {
            return Err(std::io::Error::last_os_error().into());
        }
        let mut file = unsafe { File::from_raw_fd(descriptor) };
        set_file_mode(&file, 0o600)?;
        file.write_all(&encrypted)?;
        file.sync_all()?;
        Ok(())
    }

    fn read_object(
        &self,
        directory: &File,
        id: &CacheId,
        kind: ObjectKind,
    ) -> Result<CachePlaintext, CacheError> {
        let mut file =
            open_file_at(directory, OsStr::new(kind.file_name()), false).map_err(|error| {
                match error.kind() {
                    std::io::ErrorKind::NotFound => CacheError::MissingObject,
                    _ => CacheError::Io(error),
                }
            })?;
        let metadata = file.metadata()?;
        validate_file(&metadata)?;
        let max_encoded = kind
            .max_plaintext_bytes()
            .checked_add(HEADER_BYTES as u64 + TAG_BYTES)
            .ok_or(CacheError::ObjectTooLarge)?;
        if metadata.len() > max_encoded {
            return Err(CacheError::ObjectTooLarge);
        }
        let mut bytes = Vec::with_capacity(metadata.len() as usize);
        file.read_to_end(&mut bytes)?;
        if bytes.len() as u64 != metadata.len() {
            return Err(CacheError::Corrupt);
        }
        decrypt_object(&self.key, id, kind, &bytes)
    }

    fn scan_unlocked(&self) -> Result<CacheScan, CacheError> {
        validate_directory(&self.root_directory.metadata()?)?;
        let mut audit = CacheAudit::default();
        let mut finalized = BTreeSet::new();
        let mut temporaries = Vec::new();
        let mut task_identities = BTreeMap::new();

        for task_name in directory_names(&self.root_directory)? {
            let task_name = task_name.to_str().ok_or(CacheError::Corrupt)?;
            if task_name == CACHE_LOCK_FILE {
                let file = open_file_at(&self.root_directory, OsStr::new(CACHE_LOCK_FILE), false)?;
                validate_file(&file.metadata()?)?;
                continue;
            }
            if task_name == KEY_MARKER_FILE {
                let file = open_file_at(&self.root_directory, OsStr::new(KEY_MARKER_FILE), false)?;
                validate_file(&file.metadata()?)?;
                audit.encrypted_bytes = audit
                    .encrypted_bytes
                    .checked_add(file.metadata()?.len())
                    .ok_or(CacheError::CapacityExceeded)?;
                continue;
            }
            if !is_task_hash(task_name) {
                return Err(CacheError::Corrupt);
            }
            let task_directory = open_directory_at(&self.root_directory, OsStr::new(task_name))
                .map_err(|_| CacheError::Corrupt)?;
            let task_metadata = task_directory.metadata()?;
            validate_directory(&task_metadata)?;
            let task_identity = (task_metadata.dev(), task_metadata.ino());
            if task_identities
                .insert(task_name.to_owned(), task_identity)
                .is_some()
            {
                return Err(CacheError::Corrupt);
            }
            for generation_name in directory_names(&task_directory)? {
                let generation_name = generation_name.to_str().ok_or(CacheError::Corrupt)?;
                let generation_directory =
                    open_directory_at(&task_directory, OsStr::new(generation_name))
                        .map_err(|_| CacheError::Corrupt)?;
                validate_directory(&generation_directory.metadata()?)?;
                if let Some(uuid) = generation_name.strip_prefix(TEMP_PREFIX) {
                    if uuid::Uuid::parse_str(uuid).is_err() {
                        return Err(CacheError::Corrupt);
                    }
                    audit.temporary_generations = audit
                        .temporary_generations
                        .checked_add(1)
                        .ok_or(CacheError::CapacityExceeded)?;
                    audit.encrypted_bytes = audit
                        .encrypted_bytes
                        .checked_add(inspect_temporary_directory(&generation_directory)?)
                        .ok_or(CacheError::CapacityExceeded)?;
                    temporaries.push(TemporaryGeneration {
                        task_name: task_name.to_owned(),
                        temporary_name: generation_name.to_owned(),
                        task_identity,
                    });
                    continue;
                }
                let reference = format!("{task_name}/{generation_name}");
                let id = CacheId::from_reference(&reference).map_err(|_| CacheError::Corrupt)?;
                let bytes = inspect_final_directory(&generation_directory)?;
                if !finalized.insert(id) {
                    return Err(CacheError::Corrupt);
                }
                audit.finalized_generations = audit
                    .finalized_generations
                    .checked_add(1)
                    .ok_or(CacheError::CapacityExceeded)?;
                audit.encrypted_bytes = audit
                    .encrypted_bytes
                    .checked_add(bytes)
                    .ok_or(CacheError::CapacityExceeded)?;
            }
        }
        Ok(CacheScan {
            audit,
            finalized,
            temporaries,
            task_identities,
        })
    }

    #[cfg(test)]
    fn generation_path(&self, id: &CacheId) -> PathBuf {
        self.root
            .join(id.task_directory_name())
            .join(id.generation_name())
    }

    fn remove_orphan_atomically(
        &self,
        id: &CacheId,
        expected_task_identity: (u64, u64),
    ) -> Result<(), CacheError> {
        let task_handle =
            open_directory_at(&self.root_directory, OsStr::new(&id.task_directory_name()))?;
        validate_identity(&task_handle.metadata()?, expected_task_identity)?;
        let temporary_name = format!("{TEMP_PREFIX}{}", uuid::Uuid::new_v4());
        rename_noreplace(
            &task_handle,
            OsStr::new(&id.generation_name()),
            &task_handle,
            OsStr::new(&temporary_name),
        )?;
        task_handle.sync_all()?;
        remove_flat_directory_at(&task_handle, OsStr::new(&temporary_name))?;
        task_handle.sync_all()?;
        Ok(())
    }

    fn remove_temporary(&self, temporary: &TemporaryGeneration) -> Result<(), CacheError> {
        let task_handle =
            open_directory_at(&self.root_directory, OsStr::new(&temporary.task_name))?;
        validate_identity(&task_handle.metadata()?, temporary.task_identity)?;
        remove_flat_directory_at(&task_handle, OsStr::new(&temporary.temporary_name))?;
        task_handle.sync_all()?;
        Ok(())
    }

    fn sync_all_task_directories(&self) -> Result<(), CacheError> {
        for task_name in directory_names(&self.root_directory)? {
            let task_name = task_name.to_str().ok_or(CacheError::Corrupt)?;
            if is_task_hash(task_name) {
                open_directory_at(&self.root_directory, OsStr::new(task_name))?.sync_all()?;
            }
        }
        self.root_directory.sync_all()?;
        Ok(())
    }

    #[cfg(test)]
    fn publish_with_fault(
        &self,
        id: &CacheId,
        bundle: CacheBundle<'_>,
        fail_at: PublishCheckpoint,
    ) -> Result<(), CacheError> {
        let _gate = self.gate()?;
        self.publish_observed(id, bundle, false, |stage| {
            if stage == fail_at {
                Err(CacheError::InjectedFault)
            } else {
                Ok(())
            }
        })
    }
}

struct CacheScan {
    audit: CacheAudit,
    finalized: BTreeSet<CacheId>,
    temporaries: Vec<TemporaryGeneration>,
    task_identities: BTreeMap<String, (u64, u64)>,
}

struct TemporaryGeneration {
    task_name: String,
    temporary_name: String,
    task_identity: (u64, u64),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ObjectKind {
    KeyCheck = 0,
    Manifest = 1,
    QwenWav = 2,
    DeviceEiad = 3,
}

impl ObjectKind {
    const ALL: [Self; 3] = [Self::Manifest, Self::QwenWav, Self::DeviceEiad];

    fn file_name(self) -> &'static str {
        match self {
            Self::KeyCheck => KEY_MARKER_FILE,
            Self::Manifest => "manifest.json.enc",
            Self::QwenWav => "qwen.wav.enc",
            Self::DeviceEiad => "device.eiad.enc",
        }
    }

    fn max_plaintext_bytes(self) -> u64 {
        match self {
            Self::KeyCheck => KEY_MARKER_PLAINTEXT.len() as u64,
            Self::Manifest => MAX_MANIFEST_BYTES,
            Self::QwenWav | Self::DeviceEiad => MAX_AUDIO_OBJECT_BYTES,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PublishCheckpoint {
    ManifestSynced,
    QwenWavSynced,
    DeviceEiadSynced,
    TemporaryDirectorySynced,
    Renamed,
    ParentDirectorySynced,
}

fn validate_bundle(bundle: &CacheBundle<'_>) -> Result<(), CacheError> {
    for (kind, value) in [
        (ObjectKind::Manifest, bundle.manifest_json),
        (ObjectKind::QwenWav, bundle.qwen_wav),
        (ObjectKind::DeviceEiad, bundle.device_eiad),
    ] {
        if value.len() as u64 > kind.max_plaintext_bytes() {
            return Err(CacheError::ObjectTooLarge);
        }
    }
    let total = (bundle.manifest_json.len() as u64)
        .checked_add(bundle.qwen_wav.len() as u64)
        .and_then(|value| value.checked_add(bundle.device_eiad.len() as u64))
        .ok_or(CacheError::GenerationTooLarge)?;
    if total > MAX_GENERATION_PLAINTEXT_BYTES {
        return Err(CacheError::GenerationTooLarge);
    }
    Ok(())
}

fn encoded_bundle_bytes(bundle: &CacheBundle<'_>) -> Result<u64, CacheError> {
    let overhead = (HEADER_BYTES as u64 + TAG_BYTES)
        .checked_mul(ObjectKind::ALL.len() as u64)
        .ok_or(CacheError::CapacityExceeded)?;
    (bundle.manifest_json.len() as u64)
        .checked_add(bundle.qwen_wav.len() as u64)
        .and_then(|value| value.checked_add(bundle.device_eiad.len() as u64))
        .and_then(|value| value.checked_add(overhead))
        .ok_or(CacheError::CapacityExceeded)
}

fn encrypt_object(
    root_key: &CacheKey,
    id: &CacheId,
    kind: ObjectKind,
    plaintext: &[u8],
) -> Result<Vec<u8>, CacheError> {
    let mut salt = [0_u8; SALT_BYTES];
    let mut nonce = [0_u8; NONCE_BYTES];
    OsRng.fill_bytes(&mut salt);
    OsRng.fill_bytes(&mut nonce);
    let header = encode_header(kind, plaintext.len() as u64, &salt, &nonce);
    let aad = object_aad(&header, id);
    let object_key = derive_object_key(root_key, id, kind, &salt)?;
    let cipher =
        Aes256Gcm::new_from_slice(object_key.as_slice()).map_err(|_| CacheError::Crypto)?;
    let ciphertext = cipher
        .encrypt(
            Nonce::from_slice(&nonce),
            Payload {
                msg: plaintext,
                aad: &aad,
            },
        )
        .map_err(|_| CacheError::Crypto)?;
    let mut encoded = Vec::with_capacity(HEADER_BYTES + ciphertext.len());
    encoded.extend_from_slice(&header);
    encoded.extend_from_slice(&ciphertext);
    Ok(encoded)
}

fn decrypt_object(
    root_key: &CacheKey,
    id: &CacheId,
    expected_kind: ObjectKind,
    encoded: &[u8],
) -> Result<CachePlaintext, CacheError> {
    if encoded.len() < HEADER_BYTES + TAG_BYTES as usize {
        return Err(CacheError::Corrupt);
    }
    let header: &[u8; HEADER_BYTES] = encoded[..HEADER_BYTES]
        .try_into()
        .map_err(|_| CacheError::Corrupt)?;
    let (kind, plaintext_bytes, salt, nonce) = decode_header(header)?;
    if kind != expected_kind || plaintext_bytes > expected_kind.max_plaintext_bytes() {
        return Err(CacheError::Corrupt);
    }
    let expected_encoded = (HEADER_BYTES as u64)
        .checked_add(plaintext_bytes)
        .and_then(|value| value.checked_add(TAG_BYTES))
        .ok_or(CacheError::Corrupt)?;
    if encoded.len() as u64 != expected_encoded {
        return Err(CacheError::Corrupt);
    }
    let aad = object_aad(header, id);
    let object_key = derive_object_key(root_key, id, kind, &salt)?;
    let cipher =
        Aes256Gcm::new_from_slice(object_key.as_slice()).map_err(|_| CacheError::Crypto)?;
    let plaintext = cipher
        .decrypt(
            Nonce::from_slice(&nonce),
            Payload {
                msg: &encoded[HEADER_BYTES..],
                aad: &aad,
            },
        )
        .map_err(|_| CacheError::Corrupt)?;
    if plaintext.len() as u64 != plaintext_bytes {
        return Err(CacheError::Corrupt);
    }
    Ok(CachePlaintext(Zeroizing::new(plaintext)))
}

fn encode_header(
    kind: ObjectKind,
    plaintext_bytes: u64,
    salt: &[u8; SALT_BYTES],
    nonce: &[u8; NONCE_BYTES],
) -> [u8; HEADER_BYTES] {
    let mut header = [0_u8; HEADER_BYTES];
    header[..8].copy_from_slice(CACHE_MAGIC);
    header[8] = CACHE_VERSION;
    header[9] = kind as u8;
    header[12..20].copy_from_slice(&plaintext_bytes.to_be_bytes());
    header[20..36].copy_from_slice(salt);
    header[36..48].copy_from_slice(nonce);
    header
}

fn decode_header(
    header: &[u8; HEADER_BYTES],
) -> Result<(ObjectKind, u64, [u8; SALT_BYTES], [u8; NONCE_BYTES]), CacheError> {
    if &header[..8] != CACHE_MAGIC
        || header[8] != CACHE_VERSION
        || header[10] != 0
        || header[11] != 0
    {
        return Err(CacheError::Corrupt);
    }
    let kind = match header[9] {
        0 => ObjectKind::KeyCheck,
        1 => ObjectKind::Manifest,
        2 => ObjectKind::QwenWav,
        3 => ObjectKind::DeviceEiad,
        _ => return Err(CacheError::Corrupt),
    };
    let plaintext_bytes =
        u64::from_be_bytes(header[12..20].try_into().map_err(|_| CacheError::Corrupt)?);
    let salt = header[20..36].try_into().map_err(|_| CacheError::Corrupt)?;
    let nonce = header[36..48].try_into().map_err(|_| CacheError::Corrupt)?;
    Ok((kind, plaintext_bytes, salt, nonce))
}

fn object_aad(header: &[u8; HEADER_BYTES], id: &CacheId) -> Vec<u8> {
    let mut aad = Vec::with_capacity(HEADER_BYTES + id.task_hash.len() + 8);
    aad.extend_from_slice(header);
    aad.extend_from_slice(&id.task_hash);
    aad.extend_from_slice(&id.generation.to_be_bytes());
    aad
}

fn derive_object_key(
    root_key: &CacheKey,
    id: &CacheId,
    kind: ObjectKind,
    salt: &[u8; SALT_BYTES],
) -> Result<Zeroizing<[u8; CACHE_KEY_BYTES]>, CacheError> {
    let mut info = Vec::with_capacity(72);
    info.extend_from_slice(b"easy-codex-input/cache/object-key/v1");
    info.extend_from_slice(&id.task_hash);
    info.extend_from_slice(&id.generation.to_be_bytes());
    info.push(kind as u8);
    let hkdf = Hkdf::<Sha256>::new(Some(salt), root_key.0.as_slice());
    let mut output = Zeroizing::new([0_u8; CACHE_KEY_BYTES]);
    hkdf.expand(&info, output.as_mut())
        .map_err(|_| CacheError::Crypto)?;
    Ok(output)
}

fn prepare_root_and_lock(
    root: &Path,
    limits: CacheLimits,
) -> Result<(File, ExplicitFileLock), CacheError> {
    if limits.max_total_bytes == 0 || limits.max_generations == 0 {
        return Err(CacheError::CapacityExceeded);
    }
    secure_directory(root)?;
    let root_directory = open_owned_directory_chain(root, false)?;
    set_file_mode(&root_directory, 0o700)?;
    validate_directory(&root_directory.metadata()?)?;
    let process_lock = open_file_at(&root_directory, OsStr::new(CACHE_LOCK_FILE), true)?;
    set_file_mode(&process_lock, 0o600)?;
    validate_file(&process_lock.metadata()?)?;
    process_lock.try_lock_exclusive().map_err(|error| {
        if error.kind() == fs2::lock_contended_error().kind() {
            CacheError::AlreadyOpen
        } else {
            CacheError::Io(error)
        }
    })?;
    let process_lock = ExplicitFileLock::from_locked(process_lock);
    process_lock.set_len(0)?;
    process_lock.sync_all()?;
    Ok((root_directory, process_lock))
}

fn ensure_uninitialized_root_is_empty(root: &File) -> Result<(), CacheError> {
    for name in directory_names(root)? {
        if name != OsStr::new(CACHE_LOCK_FILE) {
            return Err(CacheError::InvalidKey);
        }
    }
    Ok(())
}

fn key_marker_id() -> CacheId {
    CacheId {
        task_hash: Sha256::digest(b"easy-codex-input/cache-key-check/v1").into(),
        generation: 1,
    }
}

fn write_key_marker(root: &File, key: &CacheKey) -> Result<(), CacheError> {
    let encoded = encrypt_object(
        key,
        &key_marker_id(),
        ObjectKind::KeyCheck,
        KEY_MARKER_PLAINTEXT,
    )?;
    let temporary_name = format!("{KEY_MARKER_TEMP_PREFIX}{}", uuid::Uuid::new_v4());
    let result = (|| {
        write_create_only_file(root, OsStr::new(&temporary_name), &encoded)?;
        root.sync_all()?;
        rename_noreplace(
            root,
            OsStr::new(&temporary_name),
            root,
            OsStr::new(KEY_MARKER_FILE),
        )?;
        root.sync_all()?;
        Ok(())
    })();
    if result.is_err() {
        let _ = unlink_file_at(root, OsStr::new(&temporary_name));
    }
    result
}

fn verify_key_marker(root: &File, key: &CacheKey) -> Result<(), CacheError> {
    let mut marker = open_file_at(root, OsStr::new(KEY_MARKER_FILE), false)
        .map_err(|_| CacheError::InvalidKey)?;
    let metadata = marker.metadata()?;
    validate_file(&metadata)?;
    let expected_bytes = HEADER_BYTES as u64 + KEY_MARKER_PLAINTEXT.len() as u64 + TAG_BYTES;
    if metadata.len() != expected_bytes {
        return Err(CacheError::InvalidKey);
    }
    let mut encoded = Vec::with_capacity(expected_bytes as usize);
    marker.read_to_end(&mut encoded)?;
    let plaintext = decrypt_object(key, &key_marker_id(), ObjectKind::KeyCheck, &encoded)
        .map_err(|_| CacheError::InvalidKey)?;
    if plaintext.as_slice() != KEY_MARKER_PLAINTEXT {
        return Err(CacheError::InvalidKey);
    }
    Ok(())
}

fn cleanup_key_marker_temporaries(root: &File) -> Result<(), CacheError> {
    let mut removed = false;
    for name in directory_names(root)? {
        let Some(name_text) = name.to_str() else {
            continue;
        };
        let Some(uuid) = name_text.strip_prefix(KEY_MARKER_TEMP_PREFIX) else {
            continue;
        };
        if uuid::Uuid::parse_str(uuid).is_err() {
            return Err(CacheError::Corrupt);
        }
        let metadata = metadata_at(root, &name)?;
        if !stat_is_regular(&metadata) || metadata.st_uid != unsafe { libc::geteuid() } {
            return Err(CacheError::Corrupt);
        }
        unlink_file_at(root, &name)?;
        removed = true;
    }
    if removed {
        root.sync_all()?;
    }
    Ok(())
}

fn write_create_only_file(
    directory: &File,
    name: &OsStr,
    contents: &[u8],
) -> Result<(), CacheError> {
    let name = CString::new(name.as_bytes()).map_err(|_| CacheError::Corrupt)?;
    let descriptor = unsafe {
        libc::openat(
            directory.as_raw_fd(),
            name.as_ptr(),
            libc::O_CREAT | libc::O_EXCL | libc::O_WRONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            0o600,
        )
    };
    if descriptor < 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    let mut file = unsafe { File::from_raw_fd(descriptor) };
    set_file_mode(&file, 0o600)?;
    file.write_all(contents)?;
    file.sync_all()?;
    Ok(())
}

fn set_file_mode(file: &File, mode: u32) -> Result<(), CacheError> {
    file.set_permissions(fs::Permissions::from_mode(mode))?;
    Ok(())
}

fn open_directory_at(parent: &File, name: &OsStr) -> std::io::Result<File> {
    let name = CString::new(name.as_bytes())
        .map_err(|_| std::io::Error::new(std::io::ErrorKind::InvalidInput, "invalid name"))?;
    let descriptor = unsafe {
        libc::openat(
            parent.as_raw_fd(),
            name.as_ptr(),
            libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
        )
    };
    if descriptor < 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(unsafe { File::from_raw_fd(descriptor) })
    }
}

fn open_or_create_directory_at(parent: &File, name: &OsStr) -> Result<(File, bool), CacheError> {
    match open_directory_at(parent, name) {
        Ok(directory) => {
            validate_directory(&directory.metadata()?)?;
            Ok((directory, false))
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            Ok((create_directory_at(parent, name)?, true))
        }
        Err(error) => Err(error.into()),
    }
}

fn create_directory_at(parent: &File, name: &OsStr) -> Result<File, CacheError> {
    let name_c = CString::new(name.as_bytes()).map_err(|_| CacheError::Corrupt)?;
    if unsafe { libc::mkdirat(parent.as_raw_fd(), name_c.as_ptr(), 0o700) } != 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    if unsafe { libc::fchmodat(parent.as_raw_fd(), name_c.as_ptr(), 0o700, 0) } != 0 {
        let error = std::io::Error::last_os_error();
        unsafe {
            libc::unlinkat(parent.as_raw_fd(), name_c.as_ptr(), libc::AT_REMOVEDIR);
        }
        return Err(error.into());
    }
    let directory = open_directory_at(parent, name)?;
    set_file_mode(&directory, 0o700)?;
    validate_directory(&directory.metadata()?)?;
    Ok(directory)
}

fn directory_names(directory: &File) -> Result<Vec<OsString>, CacheError> {
    let current = c".";
    let duplicated = unsafe {
        libc::openat(
            directory.as_raw_fd(),
            current.as_ptr(),
            libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC,
        )
    };
    if duplicated < 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    let stream = unsafe { libc::fdopendir(duplicated) };
    if stream.is_null() {
        let error = std::io::Error::last_os_error();
        unsafe {
            libc::close(duplicated);
        }
        return Err(error.into());
    }
    let mut names = Vec::new();
    loop {
        set_errno(0);
        let entry = unsafe { libc::readdir(stream) };
        if entry.is_null() {
            let error = get_errno();
            unsafe {
                libc::closedir(stream);
            }
            if error == 0 {
                break;
            }
            return Err(std::io::Error::from_raw_os_error(error).into());
        }
        let name = unsafe { CStr::from_ptr((*entry).d_name.as_ptr()) }.to_bytes();
        if name != b"." && name != b".." {
            names.push(OsString::from_vec(name.to_vec()));
        }
    }
    Ok(names)
}

#[cfg(target_os = "macos")]
fn errno_pointer() -> *mut libc::c_int {
    unsafe { libc::__error() }
}

#[cfg(target_os = "linux")]
fn errno_pointer() -> *mut libc::c_int {
    unsafe { libc::__errno_location() }
}

fn set_errno(value: libc::c_int) {
    unsafe {
        *errno_pointer() = value;
    }
}

fn get_errno() -> libc::c_int {
    unsafe { *errno_pointer() }
}

fn entry_exists(directory: &File, name: &OsStr) -> Result<bool, CacheError> {
    match metadata_at(directory, name) {
        Ok(_) => Ok(true),
        Err(CacheError::Io(error)) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error),
    }
}

fn metadata_at(directory: &File, name: &OsStr) -> Result<libc::stat, CacheError> {
    let name = CString::new(name.as_bytes()).map_err(|_| CacheError::Corrupt)?;
    let mut metadata = std::mem::MaybeUninit::<libc::stat>::uninit();
    if unsafe {
        libc::fstatat(
            directory.as_raw_fd(),
            name.as_ptr(),
            metadata.as_mut_ptr(),
            libc::AT_SYMLINK_NOFOLLOW,
        )
    } != 0
    {
        return Err(std::io::Error::last_os_error().into());
    }
    Ok(unsafe { metadata.assume_init() })
}

fn unlink_file_at(directory: &File, name: &OsStr) -> Result<(), CacheError> {
    let name = CString::new(name.as_bytes()).map_err(|_| CacheError::Corrupt)?;
    if unsafe { libc::unlinkat(directory.as_raw_fd(), name.as_ptr(), 0) } != 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    Ok(())
}

fn remove_flat_directory_at(parent: &File, name: &OsStr) -> Result<(), CacheError> {
    let directory = open_directory_at(parent, name)?;
    let metadata = directory.metadata()?;
    if !metadata.is_dir() || metadata.uid() != unsafe { libc::geteuid() } {
        return Err(CacheError::Corrupt);
    }
    for child in directory_names(&directory)? {
        let metadata = metadata_at(&directory, &child)?;
        if !stat_is_regular(&metadata) || metadata.st_uid != unsafe { libc::geteuid() } {
            return Err(CacheError::Corrupt);
        }
        unlink_file_at(&directory, &child)?;
    }
    directory.sync_all()?;
    let name = CString::new(name.as_bytes()).map_err(|_| CacheError::Corrupt)?;
    if unsafe { libc::unlinkat(parent.as_raw_fd(), name.as_ptr(), libc::AT_REMOVEDIR) } != 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    Ok(())
}

fn stat_is_regular(metadata: &libc::stat) -> bool {
    metadata.st_mode & libc::S_IFMT == libc::S_IFREG
}

fn inspect_final_directory(directory: &File) -> Result<u64, CacheError> {
    let mut sizes = BTreeMap::new();
    for name in directory_names(directory)? {
        let name = name.to_str().ok_or(CacheError::Corrupt)?;
        let kind = ObjectKind::ALL
            .into_iter()
            .find(|kind| kind.file_name() == name)
            .ok_or(CacheError::Corrupt)?;
        let file = open_file_at(directory, OsStr::new(name), false)?;
        let metadata = file.metadata()?;
        validate_file(&metadata)?;
        let max_encoded = kind.max_plaintext_bytes() + HEADER_BYTES as u64 + TAG_BYTES;
        if metadata.len() < HEADER_BYTES as u64 + TAG_BYTES || metadata.len() > max_encoded {
            return Err(CacheError::Corrupt);
        }
        if sizes.insert(kind as u8, metadata.len()).is_some() {
            return Err(CacheError::Corrupt);
        }
    }
    if sizes.len() != ObjectKind::ALL.len() {
        return Err(CacheError::Corrupt);
    }
    sizes.values().try_fold(0_u64, |total, value| {
        total
            .checked_add(*value)
            .ok_or(CacheError::CapacityExceeded)
    })
}

fn inspect_temporary_directory(directory: &File) -> Result<u64, CacheError> {
    let mut total = 0_u64;
    let mut names = BTreeSet::new();
    for name in directory_names(directory)? {
        let name = name.to_str().ok_or(CacheError::Corrupt)?;
        let kind = ObjectKind::ALL
            .into_iter()
            .find(|kind| kind.file_name() == name)
            .ok_or(CacheError::Corrupt)?;
        if !names.insert(name.to_owned()) {
            return Err(CacheError::Corrupt);
        }
        let file = open_file_at(directory, OsStr::new(name), false)?;
        let metadata = file.metadata()?;
        validate_file(&metadata)?;
        let max_encoded = kind.max_plaintext_bytes() + HEADER_BYTES as u64 + TAG_BYTES;
        if metadata.len() > max_encoded {
            return Err(CacheError::CapacityExceeded);
        }
        total = total
            .checked_add(metadata.len())
            .ok_or(CacheError::CapacityExceeded)?;
    }
    Ok(total)
}

fn ensure_no_extra_entries(directory: &File) -> Result<(), CacheError> {
    inspect_final_directory(directory).map(|_| ())
}

fn validate_directory(metadata: &fs::Metadata) -> Result<(), CacheError> {
    if !metadata.is_dir()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.permissions().mode() & 0o777 != 0o700
    {
        return Err(CacheError::Corrupt);
    }
    Ok(())
}

fn validate_file(metadata: &fs::Metadata) -> Result<(), CacheError> {
    if !metadata.is_file()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.permissions().mode() & 0o777 != 0o600
    {
        return Err(CacheError::Corrupt);
    }
    Ok(())
}

fn validate_identity(metadata: &fs::Metadata, expected: (u64, u64)) -> Result<(), CacheError> {
    validate_directory(metadata)?;
    if (metadata.dev(), metadata.ino()) != expected {
        return Err(CacheError::Corrupt);
    }
    Ok(())
}

fn is_task_hash(value: &str) -> bool {
    value.len() == TASK_HASH_HEX_BYTES
        && value
            .as_bytes()
            .iter()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(byte))
}

fn is_canonical_generation(value: &str) -> bool {
    value
        .as_bytes()
        .first()
        .is_some_and(|first| (b'1'..=b'9').contains(first))
        && value.as_bytes().iter().all(u8::is_ascii_digit)
}

fn encode_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        encoded.push(HEX[(byte >> 4) as usize] as char);
        encoded.push(HEX[(byte & 0x0f) as usize] as char);
    }
    encoded
}

fn decode_hex_pair(pair: &[u8]) -> Option<u8> {
    fn digit(value: u8) -> Option<u8> {
        match value {
            b'0'..=b'9' => Some(value - b'0'),
            b'a'..=b'f' => Some(value - b'a' + 10),
            _ => None,
        }
    }
    Some(digit(*pair.first()?)? << 4 | digit(*pair.get(1)?)?)
}

#[cfg(target_os = "macos")]
fn rename_noreplace(
    old_directory: &File,
    old_name: &OsStr,
    new_directory: &File,
    new_name: &OsStr,
) -> Result<(), CacheError> {
    let old_name = CString::new(old_name.as_bytes()).map_err(|_| CacheError::Corrupt)?;
    let new_name = CString::new(new_name.as_bytes()).map_err(|_| CacheError::Corrupt)?;
    let result = unsafe {
        libc::renameatx_np(
            old_directory.as_raw_fd(),
            old_name.as_ptr(),
            new_directory.as_raw_fd(),
            new_name.as_ptr(),
            libc::RENAME_EXCL,
        )
    };
    map_rename_result(result)
}

#[cfg(target_os = "linux")]
fn rename_noreplace(
    old_directory: &File,
    old_name: &OsStr,
    new_directory: &File,
    new_name: &OsStr,
) -> Result<(), CacheError> {
    let old_name = CString::new(old_name.as_bytes()).map_err(|_| CacheError::Corrupt)?;
    let new_name = CString::new(new_name.as_bytes()).map_err(|_| CacheError::Corrupt)?;
    let result = unsafe {
        libc::renameat2(
            old_directory.as_raw_fd(),
            old_name.as_ptr(),
            new_directory.as_raw_fd(),
            new_name.as_ptr(),
            libc::RENAME_NOREPLACE,
        )
    };
    map_rename_result(result)
}

fn map_rename_result(result: libc::c_int) -> Result<(), CacheError> {
    if result == 0 {
        return Ok(());
    }
    let error = std::io::Error::last_os_error();
    if error.kind() == std::io::ErrorKind::AlreadyExists {
        Err(CacheError::AlreadyPublished)
    } else {
        Err(CacheError::Io(error))
    }
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::os::unix::fs::{DirBuilderExt, symlink};
    use std::process::{Command, Stdio};
    use std::sync::{Arc, Barrier, Mutex, mpsc};

    use super::*;
    use tempfile::tempdir;

    #[derive(Default)]
    struct MemoryStore(RefCell<BTreeMap<String, Vec<u8>>>);

    impl SecretStore for MemoryStore {
        fn get(&self, account: &str) -> Result<Option<SecretBytes>, SecretStoreError> {
            Ok(self.0.borrow().get(account).cloned().map(SecretBytes::new))
        }

        fn create(&self, account: &str, secret: &[u8]) -> Result<(), SecretStoreError> {
            let mut values = self.0.borrow_mut();
            if values.contains_key(account) {
                return Err(SecretStoreError::AlreadyExists);
            }
            values.insert(account.to_owned(), secret.to_vec());
            Ok(())
        }

        fn delete(&self, account: &str) -> Result<(), SecretStoreError> {
            self.0.borrow_mut().remove(account);
            Ok(())
        }
    }

    fn accounts() -> KeychainAccounts {
        KeychainAccounts {
            dashscope: "dashscope-test".into(),
            dashscope_verified: "dashscope-marker-test".into(),
            cache_key: "cache-test".into(),
            device_secret: "device-test".into(),
        }
    }

    fn bundle() -> CacheBundle<'static> {
        CacheBundle {
            manifest_json: br#"{"schema":1,"spoken_samples":4}"#,
            qwen_wav: b"RIFF-test-wave-data",
            device_eiad: b"EIAD-test-device-data",
        }
    }

    fn store(root: &Path) -> CacheStore {
        CacheStore::open_with_key_for_test(
            root,
            CacheKey::from_bytes([7_u8; CACHE_KEY_BYTES]),
            CacheLimits::default(),
        )
        .unwrap()
    }

    fn object_path(root: &Path, id: &CacheId, kind: ObjectKind) -> PathBuf {
        root.join(id.task_directory_name())
            .join(id.generation_name())
            .join(kind.file_name())
    }

    #[test]
    fn key_marker_prevents_silent_rotation_when_keychain_item_is_missing_or_corrupt() {
        let temp = tempdir().unwrap();
        let root = temp.path().join("tts");
        let keychain = MemoryStore::default();
        let accounts = accounts();
        let cache =
            CacheStore::initialize(&root, &keychain, &accounts, CacheLimits::default()).unwrap();
        let id = CacheId::for_task("task-key-loss", 1).unwrap();
        cache.publish(&id, bundle()).unwrap();
        drop(cache);
        let original = keychain.0.borrow()[&accounts.cache_key].clone();

        keychain.0.borrow_mut().remove(&accounts.cache_key);
        assert!(matches!(
            CacheStore::initialize(&root, &keychain, &accounts, CacheLimits::default()),
            Err(CacheError::InvalidKey)
        ));
        assert!(!keychain.0.borrow().contains_key(&accounts.cache_key));

        keychain
            .0
            .borrow_mut()
            .insert(accounts.cache_key.clone(), vec![1_u8; 31]);
        assert!(matches!(
            CacheStore::open_existing(&root, &keychain, &accounts, CacheLimits::default()),
            Err(CacheError::InvalidKey)
        ));
        assert_eq!(keychain.0.borrow()[&accounts.cache_key].len(), 31);

        keychain
            .0
            .borrow_mut()
            .insert(accounts.cache_key.clone(), original);
        let reopened =
            CacheStore::open_existing(&root, &keychain, &accounts, CacheLimits::default()).unwrap();
        assert_eq!(
            reopened.read(&id).unwrap().qwen_wav.as_slice(),
            bundle().qwen_wav
        );
        drop(reopened);

        fs::remove_file(root.join(KEY_MARKER_FILE)).unwrap();
        keychain.0.borrow_mut().remove(&accounts.cache_key);
        assert!(matches!(
            CacheStore::initialize(&root, &keychain, &accounts, CacheLimits::default()),
            Err(CacheError::InvalidKey)
        ));
        assert!(!keychain.0.borrow().contains_key(&accounts.cache_key));
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn cache_key_roundtrips_through_isolated_background_keychain_item() {
        use crate::secrets::MacKeychainStore;

        let mut accounts = accounts();
        accounts.cache_key = format!("cache-test.{}", uuid::Uuid::new_v4());
        let keychain = MacKeychainStore::background();
        keychain.delete(&accounts.cache_key).unwrap();
        let temp = tempdir().unwrap();
        let cache = CacheStore::initialize(
            &temp.path().join("tts"),
            &keychain,
            &accounts,
            CacheLimits::default(),
        )
        .unwrap();
        drop(cache);
        let reopened = CacheStore::open_existing(
            &temp.path().join("tts"),
            &keychain,
            &accounts,
            CacheLimits::default(),
        )
        .unwrap();
        drop(reopened);
        keychain.delete(&accounts.cache_key).unwrap();
    }

    #[test]
    fn publishes_reads_and_audits_private_authenticated_bundle() {
        let temp = tempdir().unwrap();
        let cache = store(&temp.path().join("tts"));
        let id = CacheId::for_task("task-fixture-a", 9).unwrap();

        cache.publish(&id, bundle()).unwrap();
        let loaded = cache.read(&id).unwrap();
        assert_eq!(loaded.manifest_json.as_slice(), bundle().manifest_json);
        assert_eq!(loaded.qwen_wav.as_slice(), bundle().qwen_wav);
        assert_eq!(loaded.device_eiad.as_slice(), bundle().device_eiad);

        let audit = cache.audit().unwrap();
        assert_eq!(audit.finalized_generations, 1);
        assert_eq!(audit.temporary_generations, 0);
        assert!(audit.encrypted_bytes > encoded_bundle_bytes(&bundle()).unwrap() - 1);
        for (kind, plaintext) in [
            (ObjectKind::Manifest, bundle().manifest_json),
            (ObjectKind::QwenWav, bundle().qwen_wav),
            (ObjectKind::DeviceEiad, bundle().device_eiad),
        ] {
            let path = object_path(&cache.root, &id, kind);
            assert_eq!(
                fs::metadata(&path).unwrap().permissions().mode() & 0o777,
                0o600
            );
            let encrypted = fs::read(path).unwrap();
            assert!(
                !encrypted
                    .windows(plaintext.len())
                    .any(|window| window == plaintext)
            );
        }
        assert!(!id.reference().contains("task-fixture-a"));
    }

    #[test]
    fn duplicate_generation_never_replaces_published_ciphertext() {
        let temp = tempdir().unwrap();
        let cache = store(&temp.path().join("tts"));
        let id = CacheId::for_task("task-fixture-a", 1).unwrap();
        cache.publish(&id, bundle()).unwrap();
        let before = fs::read(object_path(&cache.root, &id, ObjectKind::QwenWav)).unwrap();

        assert!(matches!(
            cache.publish(&id, bundle()),
            Err(CacheError::AlreadyPublished)
        ));
        assert_eq!(
            fs::read(object_path(&cache.root, &id, ObjectKind::QwenWav)).unwrap(),
            before
        );
    }

    #[test]
    fn authentication_binds_key_task_generation_kind_and_ciphertext() {
        let temp = tempdir().unwrap();
        let cache = store(&temp.path().join("tts"));
        let id = CacheId::for_task("task-fixture-a", 1).unwrap();
        cache.publish(&id, bundle()).unwrap();
        let root = cache.root.clone();
        drop(cache);

        assert!(matches!(
            CacheStore::open_with_key_for_test(
                &root,
                CacheKey::from_bytes([8_u8; CACHE_KEY_BYTES]),
                CacheLimits::default(),
            ),
            Err(CacheError::InvalidKey)
        ));
        let cache = CacheStore::open_with_key_for_test(
            &root,
            CacheKey::from_bytes([7_u8; CACHE_KEY_BYTES]),
            CacheLimits::default(),
        )
        .unwrap();
        let qwen = object_path(&root, &id, ObjectKind::QwenWav);
        let device = object_path(&root, &id, ObjectKind::DeviceEiad);
        let qwen_bytes = fs::read(&qwen).unwrap();
        let device_bytes = fs::read(&device).unwrap();
        fs::write(&qwen, &device_bytes).unwrap();
        fs::write(&device, &qwen_bytes).unwrap();
        assert!(matches!(cache.read(&id), Err(CacheError::Corrupt)));

        fs::write(&qwen, &qwen_bytes[..qwen_bytes.len() - 1]).unwrap();
        assert!(matches!(cache.read(&id), Err(CacheError::Corrupt)));
    }

    #[test]
    fn capacity_is_checked_before_publication() {
        let temp = tempdir().unwrap();
        let root = temp.path().join("tts");
        let bootstrap = store(&root);
        let marker_bytes = bootstrap.audit().unwrap().encrypted_bytes;
        drop(bootstrap);
        let predicted = encoded_bundle_bytes(&bundle()).unwrap();
        let cache = CacheStore::open_with_key_for_test(
            &root,
            CacheKey::from_bytes([8_u8; CACHE_KEY_BYTES]),
            CacheLimits {
                max_total_bytes: marker_bytes + predicted - 1,
                max_generations: 1,
            },
        );
        assert!(matches!(cache, Err(CacheError::InvalidKey)));
        let cache = CacheStore::open_with_key_for_test(
            &root,
            CacheKey::from_bytes([7_u8; CACHE_KEY_BYTES]),
            CacheLimits {
                max_total_bytes: marker_bytes + predicted - 1,
                max_generations: 1,
            },
        )
        .unwrap();
        let id = CacheId::for_task("task-fixture-a", 1).unwrap();
        assert!(matches!(
            cache.publish(&id, bundle()),
            Err(CacheError::CapacityExceeded)
        ));
        assert_eq!(cache.audit().unwrap().finalized_generations, 0);
    }

    #[test]
    fn concurrent_publications_share_capacity_gate_and_over_limit_state_can_reconcile() {
        let temp = tempdir().unwrap();
        let root = temp.path().join("tts");
        let bootstrap = store(&root);
        let marker_bytes = bootstrap.audit().unwrap().encrypted_bytes;
        drop(bootstrap);
        let per_generation = encoded_bundle_bytes(&bundle()).unwrap();
        let cache = Arc::new(
            CacheStore::open_with_key_for_test(
                &root,
                CacheKey::from_bytes([7_u8; CACHE_KEY_BYTES]),
                CacheLimits {
                    max_total_bytes: marker_bytes + per_generation * 2,
                    max_generations: 1,
                },
            )
            .unwrap(),
        );
        let barrier = Arc::new(Barrier::new(3));
        let mut workers = Vec::new();
        for task in ["task-concurrent-a", "task-concurrent-b"] {
            let cache = Arc::clone(&cache);
            let barrier = Arc::clone(&barrier);
            workers.push(std::thread::spawn(move || {
                let id = CacheId::for_task(task, 1).unwrap();
                barrier.wait();
                cache.publish(&id, bundle())
            }));
        }
        barrier.wait();
        let outcomes = workers
            .into_iter()
            .map(|worker| worker.join().unwrap())
            .collect::<Vec<_>>();
        assert_eq!(outcomes.iter().filter(|result| result.is_ok()).count(), 1);
        assert_eq!(
            outcomes
                .iter()
                .filter(|result| matches!(result, Err(CacheError::CapacityExceeded)))
                .count(),
            1
        );
        assert_eq!(cache.audit().unwrap().finalized_generations, 1);
        drop(cache);

        let wide = CacheStore::open_with_key_for_test(
            &root,
            CacheKey::from_bytes([7_u8; CACHE_KEY_BYTES]),
            CacheLimits {
                max_total_bytes: marker_bytes + per_generation * 3,
                max_generations: 2,
            },
        )
        .unwrap();
        wide.publish(&CacheId::for_task("task-over-limit", 1).unwrap(), bundle())
            .unwrap();
        drop(wide);
        let constrained = CacheStore::open_with_key_for_test(
            &root,
            CacheKey::from_bytes([7_u8; CACHE_KEY_BYTES]),
            CacheLimits {
                max_total_bytes: marker_bytes + per_generation,
                max_generations: 1,
            },
        )
        .unwrap();
        assert!(constrained.audit().unwrap().finalized_generations > 1);
        let report = constrained.reconcile(&BTreeSet::new()).unwrap();
        assert!(report.orphan_generations_removed >= 2);
        assert_eq!(constrained.audit().unwrap().finalized_generations, 0);
    }

    #[test]
    fn reconcile_reference_snapshot_is_taken_inside_the_publication_gate() {
        let temp = tempdir().unwrap();
        let cache = Arc::new(store(&temp.path().join("tts")));
        let first = CacheId::for_task("task-gate-a", 1).unwrap();
        let second = CacheId::for_task("task-gate-b", 1).unwrap();
        cache.publish(&first, bundle()).unwrap();
        let references = Arc::new(Mutex::new(BTreeSet::from([first.reference()])));
        let (published_sender, published_receiver) = mpsc::channel();
        let (commit_sender, commit_receiver) = mpsc::channel();

        let publishing_cache = Arc::clone(&cache);
        let publishing_references = Arc::clone(&references);
        let second_for_publish = second.clone();
        let publisher = std::thread::spawn(move || {
            publishing_cache
                .publish_or_verify_with(&second_for_publish, bundle(), |_| -> Result<(), ()> {
                    published_sender.send(()).unwrap();
                    commit_receiver.recv().unwrap();
                    publishing_references
                        .lock()
                        .unwrap()
                        .insert(second_for_publish.reference());
                    Ok(())
                })
                .unwrap()
                .unwrap();
        });
        published_receiver.recv().unwrap();

        let reconciling_cache = Arc::clone(&cache);
        let reconciling_references = Arc::clone(&references);
        let reconciler = std::thread::spawn(move || {
            reconciling_cache
                .reconcile_with(|| Ok::<_, ()>(reconciling_references.lock().unwrap().clone()))
                .unwrap()
                .unwrap()
        });
        commit_sender.send(()).unwrap();
        publisher.join().unwrap();
        let report = reconciler.join().unwrap();
        assert_eq!(report.orphan_generations_removed, 0);
        assert_eq!(
            cache.read(&second).unwrap().device_eiad.as_slice(),
            bundle().device_eiad
        );
    }

    #[test]
    fn process_lock_rejects_a_second_store_for_the_same_root() {
        let temp = tempdir().unwrap();
        let root = temp.path().join("tts");
        let _owner = store(&root);
        assert!(matches!(
            CacheStore::open_with_key_for_test(
                &root,
                CacheKey::from_bytes([7_u8; CACHE_KEY_BYTES]),
                CacheLimits::default(),
            ),
            Err(CacheError::AlreadyOpen)
        ));
    }

    #[test]
    fn process_lock_is_released_even_while_a_duplicate_descriptor_survives() {
        let temp = tempdir().unwrap();
        let root = temp.path().join("tts");
        let owner = store(&root);
        let inherited_descriptor = owner._process_lock.file().try_clone().unwrap();

        drop(owner);
        let reopened = store(&root);

        drop(reopened);
        drop(inherited_descriptor);
    }

    #[test]
    fn reconcile_validates_everything_before_removing_temporary_and_orphan_data() {
        let temp = tempdir().unwrap();
        let cache = store(&temp.path().join("tts"));
        let kept = CacheId::for_task("task-fixture-a", 1).unwrap();
        let orphan = CacheId::for_task("task-fixture-a", 2).unwrap();
        cache.publish(&kept, bundle()).unwrap();
        cache.publish(&orphan, bundle()).unwrap();
        let temporary = cache
            .root
            .join(kept.task_directory_name())
            .join(format!("{TEMP_PREFIX}{}", uuid::Uuid::new_v4()));
        fs::DirBuilder::new()
            .mode(0o700)
            .create(&temporary)
            .unwrap();
        fs::write(temporary.join(ObjectKind::Manifest.file_name()), b"partial").unwrap();
        fs::set_permissions(
            temporary.join(ObjectKind::Manifest.file_name()),
            fs::Permissions::from_mode(0o600),
        )
        .unwrap();

        let missing = CacheId::for_task("task-fixture-missing", 1).unwrap();
        assert!(matches!(
            cache.reconcile(&BTreeSet::from([missing.reference()])),
            Err(CacheError::MissingReference)
        ));
        assert!(temporary.exists());
        assert!(cache.generation_path(&orphan).exists());

        let report = cache
            .reconcile(&BTreeSet::from([kept.reference()]))
            .unwrap();
        assert_eq!(report.temporary_generations_removed, 1);
        assert_eq!(report.orphan_generations_removed, 1);
        assert!(cache.generation_path(&kept).exists());
        assert!(!cache.generation_path(&orphan).exists());
        assert!(!temporary.exists());
        assert_eq!(cache.audit().unwrap().finalized_generations, 1);
    }

    #[test]
    fn reconcile_refuses_corrupt_referenced_ciphertext_before_deleting_orphans() {
        let temp = tempdir().unwrap();
        let cache = store(&temp.path().join("tts"));
        let kept = CacheId::for_task("task-fixture-a", 1).unwrap();
        let orphan = CacheId::for_task("task-fixture-b", 1).unwrap();
        cache.publish(&kept, bundle()).unwrap();
        cache.publish(&orphan, bundle()).unwrap();
        let path = object_path(&cache.root, &kept, ObjectKind::QwenWav);
        let mut bytes = fs::read(&path).unwrap();
        *bytes.last_mut().unwrap() ^= 0x80;
        fs::write(path, bytes).unwrap();

        assert!(matches!(
            cache.reconcile(&BTreeSet::from([kept.reference()])),
            Err(CacheError::Corrupt)
        ));
        assert!(cache.generation_path(&orphan).exists());
    }

    #[test]
    fn layout_symlinks_unknown_files_and_invalid_references_fail_closed() {
        let temp = tempdir().unwrap();
        let cache = store(&temp.path().join("tts"));
        let id = CacheId::for_task("task-fixture-a", 1).unwrap();
        cache.publish(&id, bundle()).unwrap();
        fs::write(cache.generation_path(&id).join("unexpected"), b"x").unwrap();
        assert!(matches!(cache.audit(), Err(CacheError::Corrupt)));

        fs::remove_file(cache.generation_path(&id).join("unexpected")).unwrap();
        let noncanonical = cache.root.join(id.task_directory_name()).join("+1");
        fs::create_dir(&noncanonical).unwrap();
        assert!(matches!(cache.audit(), Err(CacheError::Corrupt)));
        fs::remove_dir(noncanonical).unwrap();
        let outside = temp.path().join("outside");
        fs::create_dir(&outside).unwrap();
        symlink(&outside, cache.root.join("a".repeat(64))).unwrap();
        assert!(matches!(cache.audit(), Err(CacheError::Corrupt)));
        assert!(matches!(
            cache.reconcile(&BTreeSet::from(["../outside/1".into()])),
            Err(CacheError::InvalidReference)
        ));
        let hash = "b".repeat(64);
        for invalid in ["+1", "01", "-1", " 1", "1 ", "１"] {
            assert!(matches!(
                CacheId::from_reference(&format!("{hash}/{invalid}")),
                Err(CacheError::InvalidReference)
            ));
        }
    }

    #[test]
    fn descriptor_anchored_cleanup_cannot_follow_a_replaced_root_path() {
        let temp = tempdir().unwrap();
        let root = temp.path().join("tts");
        let moved = temp.path().join("moved-cache");
        let outside = temp.path().join("outside");
        let cache = store(&root);
        let id = CacheId::for_task("task-fixture-a", 1).unwrap();
        cache.publish(&id, bundle()).unwrap();
        fs::rename(&root, &moved).unwrap();
        fs::create_dir(&outside).unwrap();
        fs::write(outside.join("sentinel"), b"keep").unwrap();
        symlink(&outside, &root).unwrap();

        assert_eq!(cache.audit().unwrap().finalized_generations, 1);
        cache.reconcile(&BTreeSet::new()).unwrap();
        assert_eq!(fs::read(outside.join("sentinel")).unwrap(), b"keep");
        assert!(
            moved
                .join(id.task_directory_name())
                .read_dir()
                .unwrap()
                .next()
                .is_none()
        );
    }

    #[test]
    fn umask_subprocess_helper() {
        let Some(root) = std::env::var_os("ECI_CACHE_UMASK_ROOT") else {
            return;
        };
        unsafe {
            libc::umask(0o777);
        }
        let root = PathBuf::from(root);
        let cache = store(&root);
        let id = CacheId::for_task("task-umask", 1).unwrap();
        cache.publish(&id, bundle()).unwrap();
        assert_eq!(
            fs::metadata(&root).unwrap().permissions().mode() & 0o777,
            0o700
        );
        for path in [root.join(CACHE_LOCK_FILE), root.join(KEY_MARKER_FILE)] {
            assert_eq!(
                fs::metadata(path).unwrap().permissions().mode() & 0o777,
                0o600
            );
        }
        assert_eq!(
            fs::metadata(root.join(id.task_directory_name()))
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o700
        );
        assert_eq!(
            fs::metadata(
                root.join(id.task_directory_name())
                    .join(id.generation_name())
            )
            .unwrap()
            .permissions()
            .mode()
                & 0o777,
            0o700
        );
        for kind in ObjectKind::ALL {
            assert_eq!(
                fs::metadata(object_path(&root, &id, kind))
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o600
            );
        }
    }

    #[test]
    fn exact_modes_survive_an_owner_masking_umask() {
        let temp = tempdir().unwrap();
        let status = Command::new(std::env::current_exe().unwrap())
            .args(["--exact", "cache::tests::umask_subprocess_helper"])
            .env("ECI_CACHE_UMASK_ROOT", temp.path().join("masked-cache"))
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .unwrap();
        assert!(status.success());
    }

    #[test]
    fn crash_subprocess_helper() {
        let Some(root) = std::env::var_os("ECI_CACHE_CRASH_ROOT") else {
            return;
        };
        let stage = std::env::var("ECI_CACHE_CRASH_STAGE")
            .unwrap()
            .parse::<usize>()
            .unwrap();
        let generation = std::env::var("ECI_CACHE_CRASH_GENERATION")
            .unwrap()
            .parse::<u64>()
            .unwrap();
        let checkpoints = [
            PublishCheckpoint::ManifestSynced,
            PublishCheckpoint::QwenWavSynced,
            PublishCheckpoint::DeviceEiadSynced,
            PublishCheckpoint::TemporaryDirectorySynced,
            PublishCheckpoint::Renamed,
            PublishCheckpoint::ParentDirectorySynced,
        ];
        let cache = store(&PathBuf::from(root));
        let id = CacheId::for_task("task-fixture-fault", generation).unwrap();
        assert!(matches!(
            cache.publish_with_fault(&id, bundle(), checkpoints[stage]),
            Err(CacheError::InjectedFault)
        ));
        std::process::exit(85);
    }

    #[test]
    fn crash_mode_at_every_publication_boundary_recovers_one_hundred_cycles() {
        let checkpoints = [
            PublishCheckpoint::ManifestSynced,
            PublishCheckpoint::QwenWavSynced,
            PublishCheckpoint::DeviceEiadSynced,
            PublishCheckpoint::TemporaryDirectorySynced,
            PublishCheckpoint::Renamed,
            PublishCheckpoint::ParentDirectorySynced,
        ];
        for cycle in 1..=100_u64 {
            let temp = tempdir().unwrap();
            let root = temp.path().join("tts");
            let id = CacheId::for_task("task-fixture-fault", cycle).unwrap();
            let stage = checkpoints[(cycle as usize - 1) % checkpoints.len()];
            let status = Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "cache::tests::crash_subprocess_helper"])
                .env("ECI_CACHE_CRASH_ROOT", &root)
                .env(
                    "ECI_CACHE_CRASH_STAGE",
                    ((cycle as usize - 1) % checkpoints.len()).to_string(),
                )
                .env("ECI_CACHE_CRASH_GENERATION", cycle.to_string())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .status()
                .unwrap();
            assert_eq!(status.code(), Some(85));
            let reopened = store(&root);
            let references = if matches!(
                stage,
                PublishCheckpoint::Renamed | PublishCheckpoint::ParentDirectorySynced
            ) {
                BTreeSet::from([id.reference()])
            } else {
                BTreeSet::new()
            };
            reopened.reconcile(&references).unwrap();
            if references.is_empty() {
                assert_eq!(reopened.audit().unwrap().finalized_generations, 0);
            } else {
                assert_eq!(
                    reopened.read(&id).unwrap().qwen_wav.as_slice(),
                    bundle().qwen_wav
                );
            }
        }
    }
}

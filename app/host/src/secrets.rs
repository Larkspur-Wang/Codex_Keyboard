use std::fs::{self, File};
use std::io::Read;
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};

use fs2::FileExt;
use thiserror::Error;
use zeroize::Zeroizing;

use crate::paths::ExplicitFileLock;

pub const KEYCHAIN_SERVICE: &str = "com.larkspur.easy-codex-input";
const DASHSCOPE_VARIABLE: &[u8] = b"DASHSCOPE_API_KEY";
const MAX_IMPORT_BYTES: u64 = 1024 * 1024;
const MAX_STALE_ENV_TEMPORARIES: usize = 16;
const VERIFIED_MARKER: &[u8] = b"verified-v1";

pub struct DashScopeEnvStore {
    path: PathBuf,
    dashscope_account: String,
    verified_account: String,
}

impl DashScopeEnvStore {
    pub fn new(path: PathBuf, accounts: &KeychainAccounts) -> Self {
        Self {
            path,
            dashscope_account: accounts.dashscope.clone(),
            verified_account: accounts.dashscope_verified.clone(),
        }
    }
}

pub struct LocalCacheSecretStore {
    path: PathBuf,
    account: String,
}

impl LocalCacheSecretStore {
    pub fn new(path: PathBuf, accounts: &KeychainAccounts) -> Self {
        Self {
            path,
            account: accounts.cache_key.clone(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct KeychainAccounts {
    pub dashscope: String,
    pub dashscope_verified: String,
    pub cache_key: String,
    pub device_secret: String,
}

impl KeychainAccounts {
    pub fn load_or_create(path: &Path, _lock: &ImportLock) -> Result<Self, ImportError> {
        let mut file = crate::paths::open_private_file(path).map_err(|_| ImportError::SourceIo)?;
        let metadata = file.metadata().map_err(|_| ImportError::SourceIo)?;
        if metadata.len() > 64 {
            return Err(ImportError::InvalidInstallationIdentity);
        }
        let mut value = String::new();
        file.read_to_string(&mut value)
            .map_err(|_| ImportError::InvalidInstallationIdentity)?;
        let installation_id = if value.is_empty() {
            let generated = uuid::Uuid::new_v4().to_string();
            drop(file);
            crate::paths::replace_private_file(path, generated.as_bytes())
                .map_err(|_| ImportError::SourceIo)?;
            generated
        } else {
            let parsed = uuid::Uuid::parse_str(&value)
                .map_err(|_| ImportError::InvalidInstallationIdentity)?;
            if parsed.to_string() != value {
                return Err(ImportError::InvalidInstallationIdentity);
            }
            value
        };
        Ok(Self::for_installation(&installation_id))
    }

    fn for_installation(installation_id: &str) -> Self {
        Self {
            dashscope: format!("dashscope-api-key.{installation_id}"),
            dashscope_verified: format!("dashscope-api-key-verified.{installation_id}"),
            cache_key: format!("cache-encryption-key.{installation_id}"),
            device_secret: format!("device-secret.{installation_id}"),
        }
    }
}

pub struct SecretBytes(Zeroizing<Vec<u8>>);

impl SecretBytes {
    pub fn new(bytes: Vec<u8>) -> Self {
        Self(Zeroizing::new(bytes))
    }

    pub fn as_slice(&self) -> &[u8] {
        self.0.as_slice()
    }
}

impl AsRef<[u8]> for SecretBytes {
    fn as_ref(&self) -> &[u8] {
        self.as_slice()
    }
}

#[derive(Debug, Error)]
pub enum SecretStoreError {
    #[error("secret storage operation failed")]
    Backend,
    #[error("secret storage item already exists")]
    AlreadyExists,
}

pub trait SecretStore {
    fn get(&self, account: &str) -> Result<Option<SecretBytes>, SecretStoreError>;
    fn exists(&self, account: &str) -> Result<bool, SecretStoreError> {
        self.get(account).map(|value| value.is_some())
    }
    fn create(&self, account: &str, secret: &[u8]) -> Result<(), SecretStoreError>;
    fn delete(&self, account: &str) -> Result<(), SecretStoreError>;
}

#[cfg(target_os = "macos")]
pub struct MacKeychainStore {
    allow_user_interaction: bool,
}

#[cfg(target_os = "macos")]
static KEYCHAIN_INTERACTION_MUTEX: std::sync::Mutex<()> = std::sync::Mutex::new(());

#[cfg(target_os = "macos")]
impl MacKeychainStore {
    pub const fn interactive() -> Self {
        Self {
            allow_user_interaction: true,
        }
    }

    pub const fn background() -> Self {
        Self {
            allow_user_interaction: false,
        }
    }

    fn interaction_guard(
        &self,
    ) -> Result<
        Option<security_framework::os::macos::keychain::KeychainUserInteractionLock>,
        SecretStoreError,
    > {
        if self.allow_user_interaction {
            Ok(None)
        } else {
            security_framework::os::macos::keychain::SecKeychain::disable_user_interaction()
                .map(Some)
                .map_err(|_| SecretStoreError::Backend)
        }
    }
}

#[cfg(target_os = "macos")]
impl SecretStore for MacKeychainStore {
    fn get(&self, account: &str) -> Result<Option<SecretBytes>, SecretStoreError> {
        use security_framework::passwords::{PasswordOptions, generic_password};
        use security_framework_sys::base::errSecItemNotFound;

        let _exclusive = KEYCHAIN_INTERACTION_MUTEX
            .lock()
            .map_err(|_| SecretStoreError::Backend)?;
        let _guard = self.interaction_guard()?;
        let mut options = PasswordOptions::new_generic_password(KEYCHAIN_SERVICE, account);
        options.set_access_synchronized(Some(false));
        match generic_password(options) {
            Ok(secret) => Ok(Some(SecretBytes::new(secret))),
            Err(error) if error.code() == errSecItemNotFound => Ok(None),
            Err(_) => Err(SecretStoreError::Backend),
        }
    }

    fn exists(&self, account: &str) -> Result<bool, SecretStoreError> {
        use core_foundation::base::{CFType, TCFType};
        use core_foundation::dictionary::CFDictionary;
        use core_foundation::string::CFString;
        use security_framework::passwords::PasswordOptions;
        use security_framework_sys::base::{errSecItemNotFound, errSecSuccess};
        use security_framework_sys::keychain_item::SecItemCopyMatching;

        let _exclusive = KEYCHAIN_INTERACTION_MUTEX
            .lock()
            .map_err(|_| SecretStoreError::Backend)?;
        let _guard = self.interaction_guard()?;
        let mut options = PasswordOptions::new_generic_password(KEYCHAIN_SERVICE, account);
        options.set_access_synchronized(Some(false));
        #[allow(deprecated)]
        let query = options.query;
        let parameters = CFDictionary::<CFString, CFType>::from_CFType_pairs(&query);
        let status =
            unsafe { SecItemCopyMatching(parameters.as_concrete_TypeRef(), std::ptr::null_mut()) };
        if status == errSecSuccess {
            Ok(true)
        } else if status == errSecItemNotFound {
            Ok(false)
        } else {
            Err(SecretStoreError::Backend)
        }
    }

    fn create(&self, account: &str, secret: &[u8]) -> Result<(), SecretStoreError> {
        use core_foundation::base::{CFType, TCFType};
        use core_foundation::data::CFData;
        use core_foundation::dictionary::CFDictionary;
        use core_foundation::string::CFString;
        use security_framework::passwords::PasswordOptions;
        use security_framework_sys::base::{errSecDuplicateItem, errSecSuccess};
        use security_framework_sys::item::kSecValueData;
        use security_framework_sys::keychain_item::SecItemAdd;

        let _exclusive = KEYCHAIN_INTERACTION_MUTEX
            .lock()
            .map_err(|_| SecretStoreError::Backend)?;
        let _guard = self.interaction_guard()?;
        let mut options = PasswordOptions::new_generic_password(KEYCHAIN_SERVICE, account);
        options.set_access_synchronized(Some(false));
        #[allow(deprecated)]
        let mut query = options.query;
        let secret_data = std::sync::Arc::new(SecretBytes::new(secret.to_vec()));
        query.push((
            unsafe { CFString::wrap_under_get_rule(kSecValueData) },
            CFData::from_arc(secret_data).into_CFType(),
        ));
        let parameters = CFDictionary::<CFString, CFType>::from_CFType_pairs(&query);
        let status = unsafe { SecItemAdd(parameters.as_concrete_TypeRef(), std::ptr::null_mut()) };
        if status == errSecSuccess {
            Ok(())
        } else if status == errSecDuplicateItem {
            Err(SecretStoreError::AlreadyExists)
        } else {
            Err(SecretStoreError::Backend)
        }
    }

    fn delete(&self, account: &str) -> Result<(), SecretStoreError> {
        use security_framework::passwords::{PasswordOptions, delete_generic_password_options};
        use security_framework_sys::base::errSecItemNotFound;

        let _exclusive = KEYCHAIN_INTERACTION_MUTEX
            .lock()
            .map_err(|_| SecretStoreError::Backend)?;
        let _guard = self.interaction_guard()?;
        let mut options = PasswordOptions::new_generic_password(KEYCHAIN_SERVICE, account);
        options.set_access_synchronized(Some(false));
        match delete_generic_password_options(options) {
            Ok(()) => Ok(()),
            Err(error) if error.code() == errSecItemNotFound => Ok(()),
            Err(_) => Err(SecretStoreError::Backend),
        }
    }
}

impl SecretStore for DashScopeEnvStore {
    fn get(&self, account: &str) -> Result<Option<SecretBytes>, SecretStoreError> {
        if account == self.dashscope_account {
            return load_private_target_secret(&self.path)
                .map(Some)
                .map_err(|_| SecretStoreError::Backend);
        }
        if account == self.verified_account {
            load_private_target_secret(&self.path).map_err(|_| SecretStoreError::Backend)?;
            return Ok(Some(SecretBytes::new(VERIFIED_MARKER.to_vec())));
        }
        Ok(None)
    }

    fn exists(&self, account: &str) -> Result<bool, SecretStoreError> {
        if account != self.dashscope_account && account != self.verified_account {
            return Ok(false);
        }
        match fs::symlink_metadata(&self.path) {
            Ok(_) => load_private_target_secret(&self.path)
                .map(|_| true)
                .map_err(|_| SecretStoreError::Backend),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
            Err(_) => Err(SecretStoreError::Backend),
        }
    }

    fn create(&self, _account: &str, _secret: &[u8]) -> Result<(), SecretStoreError> {
        Err(SecretStoreError::Backend)
    }

    fn delete(&self, _account: &str) -> Result<(), SecretStoreError> {
        Err(SecretStoreError::Backend)
    }
}

impl SecretStore for LocalCacheSecretStore {
    fn get(&self, account: &str) -> Result<Option<SecretBytes>, SecretStoreError> {
        if account != self.account {
            return Ok(None);
        }
        let metadata = match fs::symlink_metadata(&self.path) {
            Ok(metadata) => metadata,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(_) => return Err(SecretStoreError::Backend),
        };
        if !metadata.is_file()
            || metadata.uid() != unsafe { libc::geteuid() }
            || metadata.nlink() != 1
            || metadata.permissions().mode() & 0o077 != 0
            || metadata.len() > 65
        {
            return Err(SecretStoreError::Backend);
        }
        let mut file =
            crate::paths::open_private_file(&self.path).map_err(|_| SecretStoreError::Backend)?;
        let mut encoded = Zeroizing::new(String::new());
        file.read_to_string(&mut encoded)
            .map_err(|_| SecretStoreError::Backend)?;
        let encoded = encoded.trim_end_matches(['\r', '\n']);
        if encoded.len() != 64 {
            return Err(SecretStoreError::Backend);
        }
        let mut decoded = Vec::with_capacity(32);
        for pair in encoded.as_bytes().chunks_exact(2) {
            let high = decode_hex_digit(pair[0]).ok_or(SecretStoreError::Backend)?;
            let low = decode_hex_digit(pair[1]).ok_or(SecretStoreError::Backend)?;
            decoded.push((high << 4) | low);
        }
        Ok(Some(SecretBytes::new(decoded)))
    }

    fn create(&self, account: &str, secret: &[u8]) -> Result<(), SecretStoreError> {
        if account != self.account || secret.len() != 32 {
            return Err(SecretStoreError::Backend);
        }
        if self.get(account)?.is_some() {
            return Err(SecretStoreError::AlreadyExists);
        }
        let mut encoded = Zeroizing::new(String::with_capacity(65));
        for byte in secret {
            use std::fmt::Write as _;
            write!(&mut encoded, "{byte:02x}").map_err(|_| SecretStoreError::Backend)?;
        }
        encoded.push('\n');
        crate::paths::replace_private_file(&self.path, encoded.as_bytes())
            .map_err(|_| SecretStoreError::Backend)
    }

    fn delete(&self, _account: &str) -> Result<(), SecretStoreError> {
        Err(SecretStoreError::Backend)
    }
}

fn decode_hex_digit(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

pub(crate) fn load_dashscope_env_secret(path: &Path) -> Result<SecretBytes, SecretStoreError> {
    load_private_target_secret(path).map_err(|_| SecretStoreError::Backend)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VerificationReceipt {
    pub event_id: String,
    pub region: &'static str,
    pub model: &'static str,
    pub server_event: &'static str,
    pub transport: &'static str,
}

#[derive(Debug, Clone, Error)]
pub enum VerificationError {
    #[error("credential was rejected by the remote service")]
    Rejected,
    #[error("credential verification could not reach the remote service")]
    Unavailable,
    #[error("credential verification returned an unexpected protocol response")]
    Protocol,
}

pub trait CredentialVerifier {
    fn verify(&self, secret: &SecretBytes) -> Result<VerificationReceipt, VerificationError>;
}

#[derive(Debug, Error)]
pub enum ImportError {
    #[error("secret import source could not be read")]
    SourceIo,
    #[error("secret import source must be a regular file owned by the current user")]
    UnsafeSource,
    #[error("secret import source is too large")]
    SourceTooLarge,
    #[error("target variable is missing, duplicated or malformed")]
    InvalidTargetVariable,
    #[error("imported credential has an invalid byte representation")]
    InvalidCredential,
    #[error(transparent)]
    Store(#[from] SecretStoreError),
    #[error(transparent)]
    Verify(#[from] VerificationError),
    #[error("another credential import is already running")]
    AlreadyImporting,
    #[error("a DashScope credential is already installed")]
    AlreadyImported,
    #[error("the installation identity is invalid")]
    InvalidInstallationIdentity,
}

pub struct ImportLock {
    _file: ExplicitFileLock,
}

impl ImportLock {
    pub fn acquire(path: &Path) -> Result<Self, ImportError> {
        let file = crate::paths::open_private_file(path).map_err(|_| ImportError::SourceIo)?;
        file.try_lock_exclusive().map_err(|error| {
            if error.kind() == fs2::lock_contended_error().kind() {
                ImportError::AlreadyImporting
            } else {
                ImportError::SourceIo
            }
        })?;
        Ok(Self {
            _file: ExplicitFileLock::from_locked(file),
        })
    }
}

pub fn import_dashscope_key<S: SecretStore, R: SecretStore, V: CredentialVerifier>(
    source: &Path,
    store: &S,
    readback_store: &R,
    verifier: &V,
    accounts: &KeychainAccounts,
    _lock: &ImportLock,
) -> Result<VerificationReceipt, ImportError> {
    let active_exists = store.exists(&accounts.dashscope)?;
    let marker = store.get(&accounts.dashscope_verified)?;
    let marker_exists = marker.is_some();
    let marker_is_current = marker
        .as_ref()
        .is_some_and(|value| value.as_slice() == VERIFIED_MARKER);
    drop(marker);
    if active_exists && marker_is_current {
        return Err(ImportError::AlreadyImported);
    }
    if marker_exists {
        store.delete(&accounts.dashscope_verified)?;
    }
    if active_exists {
        store.delete(&accounts.dashscope)?;
    }

    let candidate = load_target_secret(source)?;
    store.create(&accounts.dashscope, candidate.as_slice())?;
    drop(candidate);

    let stored = match readback_store.get(&accounts.dashscope) {
        Ok(Some(value)) => value,
        Ok(None) => {
            store.delete(&accounts.dashscope)?;
            return Err(SecretStoreError::Backend.into());
        }
        Err(error) => {
            store.delete(&accounts.dashscope)?;
            return Err(error.into());
        }
    };
    let receipt = match verifier.verify(&stored) {
        Ok(receipt) => receipt,
        Err(error) => {
            drop(stored);
            store.delete(&accounts.dashscope)?;
            return Err(error.into());
        }
    };
    drop(stored);
    if let Err(error) = store.create(&accounts.dashscope_verified, VERIFIED_MARKER) {
        store.delete(&accounts.dashscope)?;
        return Err(error.into());
    }
    Ok(receipt)
}

pub fn configure_dashscope_env<V: CredentialVerifier>(
    source: &Path,
    target: &Path,
    verifier: &V,
    _lock: &ImportLock,
) -> Result<VerificationReceipt, ImportError> {
    cleanup_dashscope_env_temporaries(target)?;
    let candidate = load_target_secret(source)?;
    let receipt = verifier.verify(&candidate)?;
    let mut encoded = Zeroizing::new(Vec::with_capacity(
        DASHSCOPE_VARIABLE.len() + candidate.as_slice().len() + 2,
    ));
    encoded.extend_from_slice(DASHSCOPE_VARIABLE);
    encoded.push(b'=');
    encoded.extend_from_slice(candidate.as_slice());
    encoded.push(b'\n');
    if crate::paths::replace_private_file(target, encoded.as_slice()).is_err() {
        let reconciled = load_private_target_secret(target)
            .is_ok_and(|current| current.as_slice() == candidate.as_slice());
        if !reconciled {
            return Err(ImportError::SourceIo);
        }
    } else {
        let readback = load_private_target_secret(target)?;
        if readback.as_slice() != candidate.as_slice() {
            return Err(ImportError::SourceIo);
        }
    }
    Ok(receipt)
}

fn cleanup_dashscope_env_temporaries(target: &Path) -> Result<(), ImportError> {
    let parent = target.parent().ok_or(ImportError::UnsafeSource)?;
    let target_name = target
        .file_name()
        .and_then(|name| name.to_str())
        .filter(|name| !name.is_empty())
        .ok_or(ImportError::UnsafeSource)?;
    let directory = crate::paths::open_owned_directory_chain(parent, false)
        .map_err(|_| ImportError::UnsafeSource)?;
    let prefix = format!(".{target_name}.");
    let effective_uid = unsafe { libc::geteuid() };
    let mut matched = 0_usize;
    for name in crate::paths::directory_names_at(&directory).map_err(|_| ImportError::SourceIo)? {
        let Some(name_text) = name.to_str() else {
            continue;
        };
        let Some(identifier) = name_text
            .strip_prefix(&prefix)
            .and_then(|name| name.strip_suffix(".tmp"))
        else {
            continue;
        };
        if uuid::Uuid::parse_str(identifier).is_err() {
            continue;
        }
        matched = matched.checked_add(1).ok_or(ImportError::UnsafeSource)?;
        if matched > MAX_STALE_ENV_TEMPORARIES {
            return Err(ImportError::UnsafeSource);
        }
        let metadata =
            crate::paths::metadata_at(&directory, &name).map_err(|_| ImportError::SourceIo)?;
        if metadata.st_mode & libc::S_IFMT != libc::S_IFREG
            || metadata.st_uid != effective_uid
            || metadata.st_nlink != 1
            || metadata.st_mode & 0o077 != 0
            || metadata.st_size < 0
            || metadata.st_size as u64 > MAX_IMPORT_BYTES
        {
            return Err(ImportError::UnsafeSource);
        }
        crate::paths::unlink_file_at(&directory, &name).map_err(|_| ImportError::SourceIo)?;
    }
    if matched > 0 {
        directory.sync_all().map_err(|_| ImportError::SourceIo)?;
    }
    Ok(())
}

pub fn remove_legacy_dashscope_items<S: SecretStore>(
    store: &S,
    accounts: &KeychainAccounts,
) -> Result<(), SecretStoreError> {
    store.delete(&accounts.dashscope)?;
    store.delete(&accounts.dashscope_verified)
}

pub fn dashscope_key_is_installed<S: SecretStore>(
    store: &S,
    accounts: &KeychainAccounts,
) -> Result<bool, SecretStoreError> {
    if !store.exists(&accounts.dashscope)? {
        return Ok(false);
    }
    let marker = store.get(&accounts.dashscope_verified)?;
    Ok(marker
        .as_ref()
        .is_some_and(|value| value.as_slice() == VERIFIED_MARKER))
}

fn load_target_secret(source: &Path) -> Result<SecretBytes, ImportError> {
    load_target_secret_with_permissions(source, false)
}

fn load_private_target_secret(source: &Path) -> Result<SecretBytes, ImportError> {
    load_target_secret_with_permissions(source, true)
}

fn load_target_secret_with_permissions(
    source: &Path,
    require_private: bool,
) -> Result<SecretBytes, ImportError> {
    let file = open_import_source(source, require_private)?;
    let before = file.metadata().map_err(|_| ImportError::SourceIo)?;
    if before.len() > MAX_IMPORT_BYTES {
        return Err(ImportError::SourceTooLarge);
    }
    let mut buffer = Zeroizing::new(Vec::with_capacity((MAX_IMPORT_BYTES + 1) as usize));
    (&file)
        .take(MAX_IMPORT_BYTES + 1)
        .read_to_end(&mut buffer)
        .map_err(|_| ImportError::SourceIo)?;
    if buffer.len() as u64 > MAX_IMPORT_BYTES {
        return Err(ImportError::SourceTooLarge);
    }
    let after = file.metadata().map_err(|_| ImportError::SourceIo)?;
    if before.len() != after.len()
        || before.mtime() != after.mtime()
        || before.mtime_nsec() != after.mtime_nsec()
        || before.ctime() != after.ctime()
        || before.ctime_nsec() != after.ctime_nsec()
    {
        return Err(ImportError::UnsafeSource);
    }
    let value = parse_target_value(&buffer)?;
    validate_credential(value.as_slice())?;
    Ok(value)
}

fn open_import_source(source: &Path, require_private: bool) -> Result<File, ImportError> {
    if !source.is_absolute() {
        return Err(ImportError::UnsafeSource);
    }
    let parent = source.parent().ok_or(ImportError::UnsafeSource)?;
    let file_name = match source.file_name() {
        Some(name) if !name.is_empty() => name,
        _ => return Err(ImportError::UnsafeSource),
    };
    let directory = crate::paths::open_owned_directory_chain(parent, false)
        .map_err(|_| ImportError::UnsafeSource)?;
    let file = crate::paths::open_file_at(&directory, file_name, false)
        .map_err(|_| ImportError::UnsafeSource)?;
    let effective_uid = unsafe { libc::geteuid() };
    let opened = file.metadata().map_err(|_| ImportError::SourceIo)?;
    if !opened.is_file()
        || opened.uid() != effective_uid
        || opened.mode() & 0o022 != 0
        || (require_private && opened.mode() & 0o077 != 0)
    {
        return Err(ImportError::UnsafeSource);
    }
    file.try_lock_shared()
        .map_err(|_| ImportError::UnsafeSource)?;
    Ok(file)
}

fn parse_target_value(buffer: &[u8]) -> Result<SecretBytes, ImportError> {
    let mut target = None;
    for raw_line in buffer.split(|byte| *byte == b'\n') {
        let mut line = trim_ascii(raw_line);
        if let Some(rest) = line.strip_prefix(b"export")
            && rest.first().is_some_and(u8::is_ascii_whitespace)
        {
            line = trim_ascii(rest);
        }
        let Some(equals) = line.iter().position(|byte| *byte == b'=') else {
            let name = line
                .split(|byte| byte.is_ascii_whitespace())
                .next()
                .unwrap_or_default();
            if name == DASHSCOPE_VARIABLE {
                return Err(ImportError::InvalidTargetVariable);
            }
            continue;
        };
        let name = trim_ascii(&line[..equals]);
        if name != DASHSCOPE_VARIABLE {
            continue;
        }
        let value = &line[equals + 1..];
        if target.is_some() {
            return Err(ImportError::InvalidTargetVariable);
        }
        target = Some(parse_value(trim_ascii(value))?);
    }
    target.ok_or(ImportError::InvalidTargetVariable)
}

fn parse_value(value: &[u8]) -> Result<SecretBytes, ImportError> {
    if value.len() >= 2
        && ((value[0] == b'\'' && value[value.len() - 1] == b'\'')
            || (value[0] == b'"' && value[value.len() - 1] == b'"'))
    {
        let inner = &value[1..value.len() - 1];
        if inner.contains(&b'\\') || inner.contains(&value[0]) {
            return Err(ImportError::InvalidTargetVariable);
        }
        Ok(SecretBytes::new(inner.to_vec()))
    } else if value.iter().any(u8::is_ascii_whitespace) || value.contains(&b'#') {
        Err(ImportError::InvalidTargetVariable)
    } else {
        Ok(SecretBytes::new(value.to_vec()))
    }
}

fn validate_credential(value: &[u8]) -> Result<(), ImportError> {
    if !(16..=4096).contains(&value.len())
        || value
            .iter()
            .any(|byte| !byte.is_ascii_graphic() || *byte == b'"' || *byte == b'\'')
    {
        return Err(ImportError::InvalidCredential);
    }
    Ok(())
}

fn trim_ascii(value: &[u8]) -> &[u8] {
    trim_ascii_start(value).trim_ascii_end()
}

fn trim_ascii_start(value: &[u8]) -> &[u8] {
    value.trim_ascii_start()
}

#[cfg(test)]
mod tests {
    use std::cell::{Cell, RefCell};
    use std::collections::BTreeMap;
    use std::os::unix::fs::PermissionsExt;

    use super::*;
    use tempfile::tempdir;

    #[test]
    fn local_cache_secret_is_private_stable_and_account_scoped() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("cache-secret.hex");
        let accounts = KeychainAccounts::for_installation("019fa972-5cfa-75e1-9008-0b17ade9a347");
        let store = LocalCacheSecretStore::new(path.clone(), &accounts);
        assert!(store.get(&accounts.cache_key).unwrap().is_none());
        assert!(store.get("wrong-account").unwrap().is_none());
        store.create(&accounts.cache_key, &[41; 32]).unwrap();
        assert_eq!(
            store.get(&accounts.cache_key).unwrap().unwrap().as_slice(),
            &[41; 32]
        );
        assert_eq!(
            fs::metadata(&path).unwrap().permissions().mode() & 0o777,
            0o600
        );
        assert!(matches!(
            store.create(&accounts.cache_key, &[42; 32]),
            Err(SecretStoreError::AlreadyExists)
        ));
        assert!(matches!(
            LocalCacheSecretStore::new(temp.path().join("other"), &accounts)
                .create(&accounts.cache_key, &[1; 31]),
            Err(SecretStoreError::Backend)
        ));
    }

    #[derive(Default)]
    struct MemoryStore {
        values: RefCell<BTreeMap<String, Vec<u8>>>,
        fail_next_get: Cell<bool>,
        fail_create_account: RefCell<Option<String>>,
        fail_next_delete: Cell<bool>,
    }

    impl SecretStore for MemoryStore {
        fn get(&self, account: &str) -> Result<Option<SecretBytes>, SecretStoreError> {
            if self.fail_next_get.replace(false) {
                return Err(SecretStoreError::Backend);
            }
            Ok(self
                .values
                .borrow()
                .get(account)
                .cloned()
                .map(SecretBytes::new))
        }

        fn create(&self, account: &str, secret: &[u8]) -> Result<(), SecretStoreError> {
            if self.fail_create_account.borrow().as_deref() == Some(account) {
                self.fail_create_account.borrow_mut().take();
                return Err(SecretStoreError::Backend);
            }
            let mut values = self.values.borrow_mut();
            if values.contains_key(account) {
                return Err(SecretStoreError::AlreadyExists);
            }
            values.insert(account.to_owned(), secret.to_vec());
            Ok(())
        }

        fn delete(&self, account: &str) -> Result<(), SecretStoreError> {
            if self.fail_next_delete.replace(false) {
                return Err(SecretStoreError::Backend);
            }
            self.values.borrow_mut().remove(account);
            Ok(())
        }
    }

    struct Verifier(Result<VerificationReceipt, VerificationError>);

    impl CredentialVerifier for Verifier {
        fn verify(&self, _secret: &SecretBytes) -> Result<VerificationReceipt, VerificationError> {
            self.0.clone()
        }
    }

    struct ExpectingVerifier<'a> {
        expected: &'a [u8],
    }

    impl CredentialVerifier for ExpectingVerifier<'_> {
        fn verify(&self, secret: &SecretBytes) -> Result<VerificationReceipt, VerificationError> {
            assert_eq!(secret.as_slice(), self.expected);
            Ok(receipt())
        }
    }

    struct StatusStore;

    impl SecretStore for StatusStore {
        fn get(&self, account: &str) -> Result<Option<SecretBytes>, SecretStoreError> {
            let accounts = accounts();
            assert_eq!(account, accounts.dashscope_verified);
            Ok(Some(SecretBytes::new(VERIFIED_MARKER.to_vec())))
        }

        fn exists(&self, account: &str) -> Result<bool, SecretStoreError> {
            assert_eq!(account, accounts().dashscope);
            Ok(true)
        }

        fn create(&self, _account: &str, _secret: &[u8]) -> Result<(), SecretStoreError> {
            unreachable!()
        }

        fn delete(&self, _account: &str) -> Result<(), SecretStoreError> {
            unreachable!()
        }
    }

    fn receipt() -> VerificationReceipt {
        VerificationReceipt {
            event_id: "event-test".into(),
            region: "cn-beijing",
            model: "model-test",
            server_event: "session.created",
            transport: "mock",
        }
    }

    fn source_line(value: &str) -> String {
        format!(
            "{}={value}\n",
            std::str::from_utf8(DASHSCOPE_VARIABLE).unwrap()
        )
    }

    fn lock(temp: &tempfile::TempDir) -> ImportLock {
        ImportLock::acquire(&temp.path().join("import.lock")).unwrap()
    }

    fn accounts() -> KeychainAccounts {
        KeychainAccounts::for_installation("7c764a58-1bd4-45a7-bd62-d9318beca940")
    }

    #[test]
    fn imports_only_exact_target_and_returns_no_source_or_secret() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        let variable = std::str::from_utf8(DASHSCOPE_VARIABLE).unwrap();
        fs::write(
            &source,
            format!("OTHER=value\nexport {variable}='candidate-secret-value'\nIGNORED=other\n"),
        )
        .unwrap();
        let store = MemoryStore::default();
        let accounts = accounts();

        let imported = import_dashscope_key(
            &source,
            &store,
            &store,
            &Verifier(Ok(receipt())),
            &accounts,
            &lock(&temp),
        )
        .unwrap();

        assert_eq!(imported, receipt());
        assert_eq!(
            store.get(&accounts.dashscope).unwrap().unwrap().as_slice(),
            b"candidate-secret-value"
        );
        assert!(!format!("{imported:?}").contains("selected.env"));
        assert!(!format!("{imported:?}").contains("candidate-secret-value"));
        assert!(dashscope_key_is_installed(&store, &accounts).unwrap());
    }

    #[test]
    fn installed_status_reads_marker_but_not_active_secret() {
        assert!(dashscope_key_is_installed(&StatusStore, &accounts()).unwrap());
    }

    #[test]
    fn corrupt_or_future_marker_is_not_installed_and_is_repaired() {
        for marker in [b"corrupt".as_slice(), b"verified-v2".as_slice()] {
            let temp = tempdir().unwrap();
            let source = temp.path().join("selected.env");
            fs::write(&source, source_line("candidate-secret-value")).unwrap();
            let store = MemoryStore::default();
            let accounts = accounts();
            store
                .create(&accounts.dashscope, b"stale-secret-value")
                .unwrap();
            store.create(&accounts.dashscope_verified, marker).unwrap();

            assert!(!dashscope_key_is_installed(&store, &accounts).unwrap());
            import_dashscope_key(
                &source,
                &store,
                &store,
                &Verifier(Ok(receipt())),
                &accounts,
                &lock(&temp),
            )
            .unwrap();
            assert_eq!(
                store.get(&accounts.dashscope).unwrap().unwrap().as_slice(),
                b"candidate-secret-value"
            );
            assert!(dashscope_key_is_installed(&store, &accounts).unwrap());
        }
    }

    #[test]
    fn ignores_near_prefix_variables_and_imports_the_exact_name() {
        let source = format!(
            "DASHSCOPE_API_KEY_BACKUP=backup-secret-value\n{}",
            source_line("candidate-secret-value")
        );

        assert_eq!(
            parse_target_value(source.as_bytes()).unwrap().as_slice(),
            b"candidate-secret-value"
        );
    }

    #[test]
    fn rejects_duplicate_malformed_symlink_and_oversized_sources() {
        use std::os::unix::fs::symlink;

        let temp = tempdir().unwrap();
        let duplicate = temp.path().join("duplicate.env");
        fs::write(
            &duplicate,
            format!(
                "{}{}",
                source_line("first-secret-value"),
                source_line("second-secret-value")
            ),
        )
        .unwrap();
        assert!(matches!(
            load_target_secret(&duplicate),
            Err(ImportError::InvalidTargetVariable)
        ));

        let malformed = temp.path().join("malformed.env");
        fs::write(&malformed, source_line("value with spaces")).unwrap();
        assert!(matches!(
            load_target_secret(&malformed),
            Err(ImportError::InvalidTargetVariable)
        ));

        let missing_equals = temp.path().join("missing-equals.env");
        let variable = std::str::from_utf8(DASHSCOPE_VARIABLE).unwrap();
        fs::write(
            &missing_equals,
            format!(
                "{variable} candidate-secret-value\n{}",
                source_line("valid-secret-value")
            ),
        )
        .unwrap();
        assert!(matches!(
            load_target_secret(&missing_equals),
            Err(ImportError::InvalidTargetVariable)
        ));

        let linked = temp.path().join("linked.env");
        symlink(&duplicate, &linked).unwrap();
        assert!(matches!(
            load_target_secret(&linked),
            Err(ImportError::UnsafeSource)
        ));

        let oversized = temp.path().join("oversized.env");
        fs::write(&oversized, vec![b'x'; MAX_IMPORT_BYTES as usize + 1]).unwrap();
        assert!(matches!(
            load_target_secret(&oversized),
            Err(ImportError::SourceTooLarge)
        ));

        let writable = temp.path().join("group-writable.env");
        fs::write(&writable, source_line("candidate-secret-value")).unwrap();
        fs::set_permissions(&writable, fs::Permissions::from_mode(0o660)).unwrap();
        assert!(matches!(
            load_target_secret(&writable),
            Err(ImportError::UnsafeSource)
        ));

        let writable_parent = temp.path().join("group-writable-parent");
        fs::create_dir(&writable_parent).unwrap();
        fs::set_permissions(&writable_parent, fs::Permissions::from_mode(0o770)).unwrap();
        let nested = writable_parent.join("selected.env");
        fs::write(&nested, source_line("candidate-secret-value")).unwrap();
        assert!(matches!(
            load_target_secret(&nested),
            Err(ImportError::UnsafeSource)
        ));
    }

    #[test]
    fn existing_key_blocks_reimport_without_mutation() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        fs::write(&source, source_line("candidate-secret-value")).unwrap();
        let store = MemoryStore::default();
        let accounts = accounts();
        store
            .create(&accounts.dashscope, b"previous-secret-value")
            .unwrap();
        store
            .create(&accounts.dashscope_verified, VERIFIED_MARKER)
            .unwrap();

        assert!(matches!(
            import_dashscope_key(
                &source,
                &store,
                &store,
                &Verifier(Err(VerificationError::Rejected)),
                &accounts,
                &lock(&temp)
            ),
            Err(ImportError::AlreadyImported)
        ));
        assert_eq!(
            store.get(&accounts.dashscope).unwrap().unwrap().as_slice(),
            b"previous-secret-value"
        );
    }

    #[test]
    fn failed_first_import_removes_unverified_key() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        fs::write(&source, source_line("candidate-secret-value")).unwrap();
        let store = MemoryStore::default();
        let accounts = accounts();

        assert!(
            import_dashscope_key(
                &source,
                &store,
                &store,
                &Verifier(Err(VerificationError::Unavailable)),
                &accounts,
                &lock(&temp)
            )
            .is_err()
        );
        assert!(store.get(&accounts.dashscope).unwrap().is_none());
        assert!(!dashscope_key_is_installed(&store, &accounts).unwrap());
    }

    #[test]
    fn verification_uses_readback_store_not_source_buffer() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        fs::write(&source, source_line("candidate-secret-value")).unwrap();
        let store = MemoryStore::default();
        let readback = MemoryStore::default();
        let accounts = accounts();
        readback
            .create(&accounts.dashscope, b"keychain-readback-value")
            .unwrap();

        import_dashscope_key(
            &source,
            &store,
            &readback,
            &ExpectingVerifier {
                expected: b"keychain-readback-value",
            },
            &accounts,
            &lock(&temp),
        )
        .unwrap();
        assert!(dashscope_key_is_installed(&store, &accounts).unwrap());
    }

    #[test]
    fn readback_failure_removes_unverified_active_item() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        fs::write(&source, source_line("candidate-secret-value")).unwrap();
        let store = MemoryStore::default();
        let readback = MemoryStore::default();
        let accounts = accounts();
        readback.fail_next_get.set(true);

        assert!(matches!(
            import_dashscope_key(
                &source,
                &store,
                &readback,
                &Verifier(Ok(receipt())),
                &accounts,
                &lock(&temp)
            ),
            Err(ImportError::Store(SecretStoreError::Backend))
        ));
        assert!(!store.exists(&accounts.dashscope).unwrap());
        assert!(!store.exists(&accounts.dashscope_verified).unwrap());
    }

    #[test]
    fn marker_create_failure_rolls_back_active_item() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        fs::write(&source, source_line("candidate-secret-value")).unwrap();
        let store = MemoryStore::default();
        let accounts = accounts();
        *store.fail_create_account.borrow_mut() = Some(accounts.dashscope_verified.clone());

        assert!(matches!(
            import_dashscope_key(
                &source,
                &store,
                &store,
                &Verifier(Ok(receipt())),
                &accounts,
                &lock(&temp)
            ),
            Err(ImportError::Store(SecretStoreError::Backend))
        ));
        assert!(!store.exists(&accounts.dashscope).unwrap());
        assert!(!store.exists(&accounts.dashscope_verified).unwrap());
    }

    #[test]
    fn rollback_failure_leaves_recoverable_active_only_state() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        fs::write(&source, source_line("candidate-secret-value")).unwrap();
        let store = MemoryStore::default();
        let accounts = accounts();
        store.fail_next_delete.set(true);

        assert!(matches!(
            import_dashscope_key(
                &source,
                &store,
                &store,
                &Verifier(Err(VerificationError::Rejected)),
                &accounts,
                &lock(&temp)
            ),
            Err(ImportError::Store(SecretStoreError::Backend))
        ));
        assert!(store.exists(&accounts.dashscope).unwrap());
        assert!(!store.exists(&accounts.dashscope_verified).unwrap());

        import_dashscope_key(
            &source,
            &store,
            &store,
            &Verifier(Ok(receipt())),
            &accounts,
            &lock(&temp),
        )
        .unwrap();
        assert!(dashscope_key_is_installed(&store, &accounts).unwrap());
    }

    #[test]
    fn next_import_reconciles_marker_only_crash_state() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        fs::write(&source, source_line("candidate-secret-value")).unwrap();
        let store = MemoryStore::default();
        let accounts = accounts();
        store
            .create(&accounts.dashscope_verified, VERIFIED_MARKER)
            .unwrap();
        assert!(!dashscope_key_is_installed(&store, &accounts).unwrap());

        import_dashscope_key(
            &source,
            &store,
            &store,
            &Verifier(Ok(receipt())),
            &accounts,
            &lock(&temp),
        )
        .unwrap();
        assert!(dashscope_key_is_installed(&store, &accounts).unwrap());
    }

    #[test]
    fn configures_private_env_with_only_the_target_variable() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        let target = temp.path().join("runtime.env");
        fs::write(
            &source,
            format!(
                "OTHER=must-not-copy\n{}IGNORED=also-must-not-copy\n",
                source_line("candidate-secret-value")
            ),
        )
        .unwrap();

        let configured = configure_dashscope_env(
            &source,
            &target,
            &ExpectingVerifier {
                expected: b"candidate-secret-value",
            },
            &lock(&temp),
        )
        .unwrap();

        assert_eq!(configured, receipt());
        assert_eq!(
            fs::metadata(&target).unwrap().permissions().mode() & 0o777,
            0o600
        );
        assert_eq!(
            fs::read(&target).unwrap(),
            source_line("candidate-secret-value").as_bytes()
        );
        let accounts = accounts();
        let store = DashScopeEnvStore::new(target, &accounts);
        assert!(dashscope_key_is_installed(&store, &accounts).unwrap());
        assert_eq!(
            store.get(&accounts.dashscope).unwrap().unwrap().as_slice(),
            b"candidate-secret-value"
        );
        assert!(store.get("unrelated-account").unwrap().is_none());
    }

    #[test]
    fn env_store_rejects_public_or_linked_runtime_files() {
        use std::os::unix::fs::symlink;

        let temp = tempdir().unwrap();
        let target = temp.path().join("runtime.env");
        fs::write(&target, source_line("candidate-secret-value")).unwrap();
        fs::set_permissions(&target, fs::Permissions::from_mode(0o644)).unwrap();
        let accounts = accounts();
        let store = DashScopeEnvStore::new(target.clone(), &accounts);
        assert!(matches!(
            store.get(&accounts.dashscope),
            Err(SecretStoreError::Backend)
        ));

        fs::set_permissions(&target, fs::Permissions::from_mode(0o600)).unwrap();
        let linked = temp.path().join("linked.env");
        symlink(&target, &linked).unwrap();
        let linked_store = DashScopeEnvStore::new(linked, &accounts);
        assert!(matches!(
            linked_store.get(&accounts.dashscope),
            Err(SecretStoreError::Backend)
        ));
    }

    #[test]
    fn failed_env_verification_preserves_the_previous_file() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        let target = temp.path().join("runtime.env");
        fs::write(&source, source_line("candidate-secret-value")).unwrap();
        fs::write(&target, source_line("previous-secret-value")).unwrap();
        fs::set_permissions(&target, fs::Permissions::from_mode(0o600)).unwrap();

        assert!(matches!(
            configure_dashscope_env(
                &source,
                &target,
                &Verifier(Err(VerificationError::Rejected)),
                &lock(&temp)
            ),
            Err(ImportError::Verify(VerificationError::Rejected))
        ));
        assert_eq!(
            fs::read(&target).unwrap(),
            source_line("previous-secret-value").as_bytes()
        );
    }

    #[test]
    fn next_env_configuration_removes_bounded_private_crash_temporary() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        let target = temp.path().join("runtime.env");
        let stale = temp
            .path()
            .join(format!(".runtime.env.{}.tmp", uuid::Uuid::new_v4()));
        fs::write(&source, source_line("candidate-secret-value")).unwrap();
        fs::write(&stale, source_line("stale-private-secret")).unwrap();
        fs::set_permissions(&stale, fs::Permissions::from_mode(0o600)).unwrap();

        configure_dashscope_env(&source, &target, &Verifier(Ok(receipt())), &lock(&temp)).unwrap();

        assert!(!stale.exists());
        assert_eq!(
            fs::read(&target).unwrap(),
            source_line("candidate-secret-value").as_bytes()
        );
    }

    #[test]
    fn legacy_cleanup_removes_only_dashscope_keychain_accounts() {
        let store = MemoryStore::default();
        let accounts = accounts();
        for account in [
            &accounts.dashscope,
            &accounts.dashscope_verified,
            &accounts.cache_key,
            &accounts.device_secret,
        ] {
            store.create(account, b"private-test-value").unwrap();
        }

        remove_legacy_dashscope_items(&store, &accounts).unwrap();

        assert!(!store.exists(&accounts.dashscope).unwrap());
        assert!(!store.exists(&accounts.dashscope_verified).unwrap());
        assert!(store.exists(&accounts.cache_key).unwrap());
        assert!(store.exists(&accounts.device_secret).unwrap());
    }

    #[test]
    fn private_env_umask_subprocess_helper() {
        let (Some(source), Some(target)) = (
            std::env::var_os("ECI_ENV_UMASK_SOURCE"),
            std::env::var_os("ECI_ENV_UMASK_TARGET"),
        ) else {
            return;
        };
        unsafe {
            libc::umask(0o777);
        }
        let source = PathBuf::from(source);
        let target = PathBuf::from(target);
        let directory = target.parent().unwrap();
        configure_dashscope_env(
            &source,
            &target,
            &ExpectingVerifier {
                expected: b"candidate-secret-value",
            },
            &ImportLock::acquire(&directory.join("masked-import.lock")).unwrap(),
        )
        .unwrap();
        assert_eq!(
            fs::metadata(&target).unwrap().permissions().mode() & 0o777,
            0o600
        );
    }

    #[test]
    fn env_replace_survives_an_owner_masking_umask_and_replaces_old_content() {
        use std::process::{Command, Stdio};

        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        let target = temp.path().join("runtime.env");
        fs::write(&source, source_line("candidate-secret-value")).unwrap();
        fs::write(&target, source_line("previous-secret-value")).unwrap();
        fs::set_permissions(&target, fs::Permissions::from_mode(0o600)).unwrap();

        let status = Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "secrets::tests::private_env_umask_subprocess_helper",
            ])
            .env("ECI_ENV_UMASK_SOURCE", &source)
            .env("ECI_ENV_UMASK_TARGET", &target)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .unwrap();
        assert!(status.success());
        assert_eq!(
            fs::read(&target).unwrap(),
            source_line("candidate-secret-value").as_bytes()
        );
        assert_eq!(
            fs::metadata(&target).unwrap().permissions().mode() & 0o777,
            0o600
        );
    }

    #[test]
    fn installation_identity_is_stable_private_and_fail_closed() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("installation-id");
        let import_lock = lock(&temp);
        let first = KeychainAccounts::load_or_create(&path, &import_lock).unwrap();
        let second = KeychainAccounts::load_or_create(&path, &import_lock).unwrap();
        assert_eq!(first, second);
        assert_eq!(
            fs::metadata(&path).unwrap().permissions().mode() & 0o777,
            0o600
        );

        fs::write(&path, b"not-a-valid-installation-id").unwrap();
        assert!(matches!(
            KeychainAccounts::load_or_create(&path, &import_lock),
            Err(ImportError::InvalidInstallationIdentity)
        ));
    }

    #[test]
    fn import_lock_is_process_exclusive() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("import.lock");
        let _owner = ImportLock::acquire(&path).unwrap();
        assert!(matches!(
            ImportLock::acquire(&path),
            Err(ImportError::AlreadyImporting)
        ));
    }

    #[test]
    fn import_lock_is_released_even_while_a_duplicate_descriptor_survives() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("import.lock");
        let owner = ImportLock::acquire(&path).unwrap();
        let inherited_descriptor = owner._file.file().try_clone().unwrap();

        drop(owner);
        let reopened = ImportLock::acquire(&path).unwrap();

        drop(reopened);
        drop(inherited_descriptor);
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn mac_keychain_background_roundtrip_uses_isolated_account_without_ui() {
        let account = format!("test.{}", uuid::Uuid::new_v4());
        let store = MacKeychainStore::background();
        store.delete(&account).unwrap();
        store.create(&account, b"first-test-secret").unwrap();
        assert!(matches!(
            store.create(&account, b"must-not-overwrite"),
            Err(SecretStoreError::AlreadyExists)
        ));
        assert_eq!(
            store.get(&account).unwrap().unwrap().as_slice(),
            b"first-test-secret"
        );
        assert_eq!(
            store.get(&account).unwrap().unwrap().as_slice(),
            b"first-test-secret"
        );
        store.delete(&account).unwrap();
        assert!(store.get(&account).unwrap().is_none());
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn mac_keychain_full_import_and_status_use_isolated_accounts() {
        let temp = tempdir().unwrap();
        let source = temp.path().join("selected.env");
        fs::write(&source, source_line("candidate-secret-value")).unwrap();
        let accounts = KeychainAccounts::for_installation(&uuid::Uuid::new_v4().to_string());
        let store = MacKeychainStore::background();
        store.delete(&accounts.dashscope).unwrap();
        store.delete(&accounts.dashscope_verified).unwrap();

        let imported = import_dashscope_key(
            &source,
            &store,
            &MacKeychainStore::background(),
            &Verifier(Ok(receipt())),
            &accounts,
            &lock(&temp),
        )
        .unwrap();
        assert_eq!(imported, receipt());
        assert_eq!(
            store.get(&accounts.dashscope).unwrap().unwrap().as_slice(),
            b"candidate-secret-value"
        );
        assert!(dashscope_key_is_installed(&MacKeychainStore::background(), &accounts).unwrap());
        store.delete(&accounts.dashscope).unwrap();
        store.delete(&accounts.dashscope_verified).unwrap();
    }
}

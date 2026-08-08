#[cfg(any(target_os = "macos", test))]
use std::ffi::CString;
#[cfg(target_os = "macos")]
use std::ffi::OsStr;
#[cfg(any(target_os = "macos", test))]
use std::fs::{self, File, OpenOptions};
use std::io;
#[cfg(any(target_os = "macos", test))]
use std::io::{Read, Write};
#[cfg(any(target_os = "macos", test))]
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::ffi::OsStrExt;
#[cfg(any(target_os = "macos", test))]
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Component, Path, PathBuf};
#[cfg(target_os = "macos")]
use std::process::{Command, Stdio};
#[cfg(any(target_os = "macos", test))]
use std::time::Duration;
#[cfg(target_os = "macos")]
use std::time::Instant;

#[cfg(any(target_os = "macos", test))]
use fs2::FileExt;
use thiserror::Error;

use crate::health::HEALTH_SOCKET_NAME;
#[cfg(any(target_os = "macos", test))]
use crate::health::HealthSnapshot;
#[cfg(test)]
use crate::health::HealthState;
#[cfg(target_os = "macos")]
use crate::health::{HealthError, query_health};
use crate::paths::AppPaths;
#[cfg(any(target_os = "macos", test))]
use crate::paths::{
    ExplicitFileLock, open_owned_directory_chain, open_private_file, secure_directory,
};

pub const LAUNCH_AGENT_LABEL: &str = "com.larkspur.easy-codex-input.host";
const INSTALLED_BINARY_NAME: &str = "easy-codex-host";
const PLIST_NAME: &str = "com.larkspur.easy-codex-input.host.plist";
#[cfg(any(target_os = "macos", test))]
const STOP_TIMEOUT: Duration = Duration::from_secs(25);
#[cfg(target_os = "macos")]
const STOP_STABLE_WINDOW: Duration = Duration::from_millis(250);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeploymentMode {
    Install,
    Upgrade,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LaunchAgentPaths {
    pub app: AppPaths,
    pub installed_binary: PathBuf,
    pub plist: PathBuf,
    pub log: PathBuf,
    pub management_lock: PathBuf,
}

impl LaunchAgentPaths {
    pub fn from_home(home: &Path) -> Result<Self, LaunchAgentError> {
        validate_absolute_path(home)?;
        let app = AppPaths::from_home(home);
        Ok(Self {
            installed_binary: app.root.join("bin").join(INSTALLED_BINARY_NAME),
            log: app.root.join("logs").join("host.log"),
            management_lock: app.runtime_directory.join("launch-agent.lock"),
            plist: home.join("Library").join("LaunchAgents").join(PLIST_NAME),
            app,
        })
    }

    pub fn health_socket(&self) -> PathBuf {
        self.app.runtime_directory.join(HEALTH_SOCKET_NAME)
    }
}

#[derive(Debug, Error)]
pub enum LaunchAgentError {
    #[error("LaunchAgent I/O failed")]
    Io(#[from] io::Error),
    #[error("LaunchAgent paths must be absolute, normalized UTF-8 paths")]
    InvalidPath,
    #[error("LaunchAgent source is not a regular executable")]
    InvalidSource,
    #[error("LaunchAgent target is not a current-user regular file")]
    UnsafeTarget,
    #[error("another LaunchAgent management operation is active")]
    AlreadyManaging,
    #[error("LaunchAgent is not installed and cannot be upgraded")]
    NotInstalled,
    #[error("launchctl could not activate the Host")]
    ActivationFailed,
    #[error("LaunchAgent rollback could not restore the previous Host")]
    RollbackFailed,
    #[error("launchctl could not stop the Host")]
    StopFailed,
}

pub fn render_plist(paths: &LaunchAgentPaths) -> Result<Vec<u8>, LaunchAgentError> {
    let binary = xml_path(&paths.installed_binary)?;
    let log = xml_path(&paths.log)?;
    let working_directory = xml_path(&paths.app.root)?;
    let plist = format!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \
\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n\
<plist version=\"1.0\">\n\
<dict>\n\
  <key>Label</key>\n  <string>{LAUNCH_AGENT_LABEL}</string>\n\
  <key>ProgramArguments</key>\n  <array>\n    <string>{binary}</string>\n    <string>daemon</string>\n  </array>\n\
  <key>WorkingDirectory</key>\n  <string>{working_directory}</string>\n\
  <key>RunAtLoad</key>\n  <true/>\n\
  <key>KeepAlive</key>\n  <true/>\n\
  <key>ProcessType</key>\n  <string>Background</string>\n\
  <key>ThrottleInterval</key>\n  <integer>5</integer>\n\
  <key>ExitTimeOut</key>\n  <integer>20</integer>\n\
  <key>StandardOutPath</key>\n  <string>{log}</string>\n\
  <key>StandardErrorPath</key>\n  <string>{log}</string>\n\
</dict>\n\
</plist>\n"
    );
    Ok(plist.into_bytes())
}

#[cfg(target_os = "macos")]
pub fn deploy_current_executable(
    home: &Path,
    mode: DeploymentMode,
) -> Result<LaunchAgentPaths, LaunchAgentError> {
    let source = std::env::current_exe()?;
    let paths = LaunchAgentPaths::from_home(home)?;
    let controller = MacLaunchctl::current_user();
    deploy(&paths, &source, mode, &controller)?;
    Ok(paths)
}

#[cfg(not(target_os = "macos"))]
pub fn deploy_current_executable(
    _home: &Path,
    _mode: DeploymentMode,
) -> Result<LaunchAgentPaths, LaunchAgentError> {
    Err(LaunchAgentError::ActivationFailed)
}

#[cfg(target_os = "macos")]
pub fn uninstall(home: &Path) -> Result<LaunchAgentPaths, LaunchAgentError> {
    let paths = LaunchAgentPaths::from_home(home)?;
    prepare_directories(&paths)?;
    let _lock = acquire_management_lock(&paths)?;
    let controller = MacLaunchctl::current_user();
    if controller.is_loaded()? {
        controller.bootout()?;
    }
    remove_owned_regular_if_present(&paths.plist)?;
    remove_owned_regular_if_present(&paths.installed_binary)?;
    wait_for_socket_removal(&paths.health_socket(), Duration::from_secs(5))?;
    Ok(paths)
}

#[cfg(not(target_os = "macos"))]
pub fn uninstall(_home: &Path) -> Result<LaunchAgentPaths, LaunchAgentError> {
    Err(LaunchAgentError::StopFailed)
}

#[cfg(target_os = "macos")]
pub fn is_loaded() -> Result<bool, LaunchAgentError> {
    MacLaunchctl::current_user().is_loaded()
}

#[cfg(not(target_os = "macos"))]
pub fn is_loaded() -> Result<bool, LaunchAgentError> {
    Ok(false)
}

#[cfg(any(target_os = "macos", test))]
trait ServiceController {
    fn is_loaded(&self) -> Result<bool, LaunchAgentError>;
    fn bootout(&self) -> Result<(), LaunchAgentError>;
    fn wait_stopped(&self, socket: &Path, timeout: Duration) -> Result<(), LaunchAgentError>;
    fn bootstrap(&self, plist: &Path) -> Result<(), LaunchAgentError>;
    fn kickstart(&self) -> Result<(), LaunchAgentError>;
    fn current_health(&self, socket: &Path) -> Result<Option<HealthSnapshot>, LaunchAgentError>;
    fn wait_healthy(
        &self,
        socket: &Path,
        expected_version: &str,
        previous_instance: Option<(u32, u64)>,
        timeout: Duration,
    ) -> Result<(), LaunchAgentError>;
}

#[cfg(target_os = "macos")]
struct MacLaunchctl {
    domain: String,
    service: String,
}

#[cfg(target_os = "macos")]
impl MacLaunchctl {
    fn current_user() -> Self {
        let uid = unsafe { libc::getuid() };
        let domain = format!("gui/{uid}");
        Self {
            service: format!("{domain}/{LAUNCH_AGENT_LABEL}"),
            domain,
        }
    }

    fn run(&self, arguments: &[&OsStr]) -> Result<bool, LaunchAgentError> {
        let status = Command::new("/bin/launchctl")
            .args(arguments)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()?;
        Ok(status.success())
    }

    fn service_pid(&self) -> Result<Option<u32>, LaunchAgentError> {
        let output = Command::new("/bin/launchctl")
            .args([OsStr::new("print"), OsStr::new(&self.service)])
            .stdin(Stdio::null())
            .stderr(Stdio::null())
            .output()?;
        if !output.status.success() {
            return Ok(None);
        }
        let text =
            std::str::from_utf8(&output.stdout).map_err(|_| LaunchAgentError::ActivationFailed)?;
        for line in text.lines() {
            if let Some(value) = line.trim().strip_prefix("pid = ") {
                return value
                    .parse::<u32>()
                    .ok()
                    .filter(|pid| *pid != 0)
                    .map(Some)
                    .ok_or(LaunchAgentError::ActivationFailed);
            }
        }
        Ok(None)
    }
}

#[cfg(target_os = "macos")]
impl ServiceController for MacLaunchctl {
    fn is_loaded(&self) -> Result<bool, LaunchAgentError> {
        self.run(&[OsStr::new("print"), OsStr::new(&self.service)])
    }

    fn bootout(&self) -> Result<(), LaunchAgentError> {
        if self.run(&[OsStr::new("bootout"), OsStr::new(&self.service)])? {
            Ok(())
        } else {
            Err(LaunchAgentError::StopFailed)
        }
    }

    fn wait_stopped(&self, socket: &Path, timeout: Duration) -> Result<(), LaunchAgentError> {
        let deadline = Instant::now() + timeout;
        let mut absent_since = None;
        loop {
            let unloaded = !self.is_loaded()?;
            let socket_absent = match fs::symlink_metadata(socket) {
                Ok(_) => false,
                Err(error) if error.kind() == io::ErrorKind::NotFound => true,
                Err(error) => return Err(error.into()),
            };
            if unloaded && socket_absent {
                let first_absent = absent_since.get_or_insert_with(Instant::now);
                if first_absent.elapsed() >= STOP_STABLE_WINDOW {
                    return Ok(());
                }
            } else {
                absent_since = None;
            }
            if Instant::now() >= deadline {
                return Err(LaunchAgentError::StopFailed);
            }
            std::thread::sleep(Duration::from_millis(25));
        }
    }

    fn bootstrap(&self, plist: &Path) -> Result<(), LaunchAgentError> {
        if self.run(&[
            OsStr::new("bootstrap"),
            OsStr::new(&self.domain),
            plist.as_os_str(),
        ])? {
            Ok(())
        } else {
            Err(LaunchAgentError::ActivationFailed)
        }
    }

    fn kickstart(&self) -> Result<(), LaunchAgentError> {
        if self.run(&[
            OsStr::new("kickstart"),
            OsStr::new("-k"),
            OsStr::new(&self.service),
        ])? {
            Ok(())
        } else {
            Err(LaunchAgentError::ActivationFailed)
        }
    }

    fn current_health(&self, socket: &Path) -> Result<Option<HealthSnapshot>, LaunchAgentError> {
        match query_health(socket) {
            Ok(snapshot) => Ok(Some(snapshot)),
            Err(error) if health_is_temporarily_unavailable(&error) => Ok(None),
            Err(_) => Err(LaunchAgentError::ActivationFailed),
        }
    }

    fn wait_healthy(
        &self,
        socket: &Path,
        expected_version: &str,
        previous_instance: Option<(u32, u64)>,
        timeout: Duration,
    ) -> Result<(), LaunchAgentError> {
        let deadline = Instant::now() + timeout;
        loop {
            match query_health(socket) {
                Ok(snapshot)
                    if snapshot.host_version == expected_version
                        && previous_instance
                            != Some((snapshot.pid, snapshot.started_at_unix_ms))
                        && self.service_pid()? == Some(snapshot.pid) =>
                {
                    return Ok(());
                }
                Ok(_) => {}
                Err(error) if health_is_temporarily_unavailable(&error) => {}
                Err(_) => return Err(LaunchAgentError::ActivationFailed),
            }
            if Instant::now() >= deadline {
                return Err(LaunchAgentError::ActivationFailed);
            }
            std::thread::sleep(Duration::from_millis(50));
        }
    }
}

#[cfg(target_os = "macos")]
fn health_is_temporarily_unavailable(error: &HealthError) -> bool {
    matches!(error, HealthError::InvalidResponse)
        || matches!(
            error,
            HealthError::Io(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::NotFound
                        | io::ErrorKind::ConnectionRefused
                        | io::ErrorKind::ConnectionReset
                        | io::ErrorKind::BrokenPipe
                        | io::ErrorKind::UnexpectedEof
                        | io::ErrorKind::TimedOut
                        | io::ErrorKind::WouldBlock
                )
        )
}

#[cfg(any(target_os = "macos", test))]
fn deploy(
    paths: &LaunchAgentPaths,
    source: &Path,
    mode: DeploymentMode,
    controller: &impl ServiceController,
) -> Result<(), LaunchAgentError> {
    prepare_directories(paths)?;
    let _lock = acquire_management_lock(paths)?;
    let previous_binary = read_owned_regular_if_present(&paths.installed_binary)?;
    let previous_plist = read_owned_regular_if_present(&paths.plist)?;
    if mode == DeploymentMode::Upgrade && (previous_binary.is_none() || previous_plist.is_none()) {
        return Err(LaunchAgentError::NotInstalled);
    }
    let candidate_binary = read_source_executable(source)?;
    let candidate_plist = render_plist(paths)?;
    let was_loaded = controller.is_loaded()?;
    let previous_health = controller.current_health(&paths.health_socket())?;
    if !was_loaded && previous_health.is_some() {
        return Err(LaunchAgentError::ActivationFailed);
    }
    if mode == DeploymentMode::Upgrade && was_loaded && previous_health.is_none() {
        return Err(LaunchAgentError::ActivationFailed);
    }
    let activation = (|| {
        if was_loaded {
            controller.bootout()?;
            controller.wait_stopped(&paths.health_socket(), STOP_TIMEOUT)?;
        }
        write_atomic_owned(&paths.installed_binary, &candidate_binary, 0o700)?;
        write_atomic_owned(&paths.plist, &candidate_plist, 0o600)?;
        controller.bootstrap(&paths.plist)?;
        controller.kickstart()?;
        controller.wait_healthy(
            &paths.health_socket(),
            env!("CARGO_PKG_VERSION"),
            previous_health
                .as_ref()
                .map(|health| (health.pid, health.started_at_unix_ms)),
            Duration::from_secs(10),
        )
    })();
    if activation.is_ok() {
        return Ok(());
    }

    let stopped = if controller.is_loaded().unwrap_or(false) {
        controller
            .bootout()
            .and_then(|()| controller.wait_stopped(&paths.health_socket(), STOP_TIMEOUT))
    } else {
        controller.wait_stopped(&paths.health_socket(), STOP_TIMEOUT)
    };
    if stopped.is_err() {
        return Err(LaunchAgentError::RollbackFailed);
    }
    restore_file(&paths.installed_binary, previous_binary.as_deref(), 0o700)?;
    restore_file(&paths.plist, previous_plist.as_deref(), 0o600)?;
    if was_loaded {
        let Some(previous_health) = previous_health else {
            return Err(LaunchAgentError::RollbackFailed);
        };
        if previous_plist.is_none()
            || controller.bootstrap(&paths.plist).is_err()
            || controller.kickstart().is_err()
            || controller
                .wait_healthy(
                    &paths.health_socket(),
                    &previous_health.host_version,
                    Some((previous_health.pid, previous_health.started_at_unix_ms)),
                    Duration::from_secs(10),
                )
                .is_err()
        {
            return Err(LaunchAgentError::RollbackFailed);
        }
    }
    Err(LaunchAgentError::ActivationFailed)
}

#[cfg(any(target_os = "macos", test))]
fn prepare_directories(paths: &LaunchAgentPaths) -> Result<(), LaunchAgentError> {
    paths.app.prepare()?;
    secure_directory(&paths.app.root.join("bin"))?;
    secure_directory(&paths.app.root.join("logs"))?;
    drop(open_owned_directory_chain(
        paths.plist.parent().ok_or(LaunchAgentError::InvalidPath)?,
        true,
    )?);
    drop(open_private_file(&paths.log)?);
    Ok(())
}

#[cfg(any(target_os = "macos", test))]
fn acquire_management_lock(paths: &LaunchAgentPaths) -> Result<ExplicitFileLock, LaunchAgentError> {
    let file = open_private_file(&paths.management_lock)?;
    file.try_lock_exclusive()
        .map_err(|error| match error.kind() {
            io::ErrorKind::WouldBlock => LaunchAgentError::AlreadyManaging,
            _ => LaunchAgentError::Io(error),
        })?;
    Ok(ExplicitFileLock::from_locked(file))
}

#[cfg(any(target_os = "macos", test))]
fn read_source_executable(path: &Path) -> Result<Vec<u8>, LaunchAgentError> {
    validate_absolute_path(path)?;
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.is_file() || metadata.permissions().mode() & 0o111 == 0 {
        return Err(LaunchAgentError::InvalidSource);
    }
    let mut file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(path)?;
    let opened = file.metadata()?;
    if !opened.is_file()
        || opened.permissions().mode() & 0o111 == 0
        || (metadata.dev(), metadata.ino()) != (opened.dev(), opened.ino())
    {
        return Err(LaunchAgentError::InvalidSource);
    }
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes)?;
    if bytes.is_empty() {
        return Err(LaunchAgentError::InvalidSource);
    }
    Ok(bytes)
}

#[cfg(any(target_os = "macos", test))]
fn read_owned_regular_if_present(path: &Path) -> Result<Option<Vec<u8>>, LaunchAgentError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) => {
            let uid = unsafe { libc::geteuid() };
            if !metadata.is_file() || metadata.uid() != uid {
                return Err(LaunchAgentError::UnsafeTarget);
            }
            let mut file = OpenOptions::new()
                .read(true)
                .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
                .open(path)?;
            let opened = file.metadata()?;
            if (metadata.dev(), metadata.ino()) != (opened.dev(), opened.ino()) {
                return Err(LaunchAgentError::UnsafeTarget);
            }
            let mut bytes = Vec::new();
            file.read_to_end(&mut bytes)?;
            Ok(Some(bytes))
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error.into()),
    }
}

#[cfg(any(target_os = "macos", test))]
fn write_atomic_owned(path: &Path, bytes: &[u8], mode: u32) -> Result<(), LaunchAgentError> {
    if fs::symlink_metadata(path).is_ok() {
        let metadata = fs::symlink_metadata(path)?;
        let uid = unsafe { libc::geteuid() };
        if !metadata.is_file() || metadata.uid() != uid {
            return Err(LaunchAgentError::UnsafeTarget);
        }
    }
    let parent_path = path.parent().ok_or(LaunchAgentError::InvalidPath)?;
    let file_name = path.file_name().ok_or(LaunchAgentError::InvalidPath)?;
    let parent = open_owned_directory_chain(parent_path, false)?;
    let temporary_name = format!(
        ".{}.{}.tmp",
        file_name.to_string_lossy(),
        uuid::Uuid::new_v4()
    );
    let temporary =
        CString::new(temporary_name.as_bytes()).map_err(|_| LaunchAgentError::InvalidPath)?;
    let destination =
        CString::new(file_name.as_bytes()).map_err(|_| LaunchAgentError::InvalidPath)?;
    let descriptor = unsafe {
        libc::openat(
            parent.as_raw_fd(),
            temporary.as_ptr(),
            libc::O_CREAT | libc::O_EXCL | libc::O_WRONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            mode,
        )
    };
    if descriptor < 0 {
        return Err(io::Error::last_os_error().into());
    }
    let mut file = unsafe { File::from_raw_fd(descriptor) };
    let result = (|| {
        file.set_permissions(fs::Permissions::from_mode(mode))?;
        file.write_all(bytes)?;
        file.sync_all()?;
        if unsafe {
            libc::renameat(
                parent.as_raw_fd(),
                temporary.as_ptr(),
                parent.as_raw_fd(),
                destination.as_ptr(),
            )
        } != 0
        {
            return Err(io::Error::last_os_error());
        }
        parent.sync_all()
    })();
    if result.is_err() {
        unsafe { libc::unlinkat(parent.as_raw_fd(), temporary.as_ptr(), 0) };
    }
    result.map_err(LaunchAgentError::Io)
}

#[cfg(any(target_os = "macos", test))]
fn restore_file(path: &Path, previous: Option<&[u8]>, mode: u32) -> Result<(), LaunchAgentError> {
    if let Some(bytes) = previous {
        write_atomic_owned(path, bytes, mode)
    } else {
        remove_owned_regular_if_present(path)
    }
}

#[cfg(any(target_os = "macos", test))]
fn remove_owned_regular_if_present(path: &Path) -> Result<(), LaunchAgentError> {
    remove_owned_regular_if_present_with_hook(path, || {})
}

#[cfg(any(target_os = "macos", test))]
fn remove_owned_regular_if_present_with_hook(
    path: &Path,
    before_rename: impl FnOnce(),
) -> Result<(), LaunchAgentError> {
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error.into()),
    };
    let uid = unsafe { libc::geteuid() };
    if !metadata.is_file() || metadata.uid() != uid {
        return Err(LaunchAgentError::UnsafeTarget);
    }
    let guard = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(path)?;
    let opened = guard.metadata()?;
    if !opened.is_file()
        || opened.uid() != uid
        || (metadata.dev(), metadata.ino()) != (opened.dev(), opened.ino())
    {
        return Err(LaunchAgentError::UnsafeTarget);
    }
    let parent_path = path.parent().ok_or(LaunchAgentError::InvalidPath)?;
    let file_name = path.file_name().ok_or(LaunchAgentError::InvalidPath)?;
    let parent = open_owned_directory_chain(parent_path, false)?;
    let source = CString::new(file_name.as_bytes()).map_err(|_| LaunchAgentError::InvalidPath)?;
    let retired_name = format!(
        ".{}.{}.retired",
        file_name.to_string_lossy(),
        uuid::Uuid::new_v4()
    );
    let retired =
        CString::new(retired_name.as_bytes()).map_err(|_| LaunchAgentError::InvalidPath)?;
    before_rename();
    rename_noreplace(&parent, &source, &retired)?;

    let moved = regular_identity_at(&parent, &retired);
    if !matches!(moved, Ok(identity) if identity == (opened.dev(), opened.ino())) {
        let _ = rename_noreplace(&parent, &retired, &source);
        return Err(LaunchAgentError::UnsafeTarget);
    }
    if unsafe { libc::unlinkat(parent.as_raw_fd(), retired.as_ptr(), 0) } != 0 {
        let error = io::Error::last_os_error();
        let _ = rename_noreplace(&parent, &retired, &source);
        return Err(error.into());
    }
    parent.sync_all()?;
    Ok(())
}

#[cfg(any(target_os = "macos", test))]
fn regular_identity_at(parent: &File, name: &CString) -> Result<(u64, u64), LaunchAgentError> {
    let mut status: libc::stat = unsafe { std::mem::zeroed() };
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
    if status.st_uid != unsafe { libc::geteuid() } || status.st_mode & libc::S_IFMT != libc::S_IFREG
    {
        return Err(LaunchAgentError::UnsafeTarget);
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

#[cfg(all(test, target_os = "linux"))]
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

#[cfg(target_os = "macos")]
fn wait_for_socket_removal(path: &Path, timeout: Duration) -> Result<(), LaunchAgentError> {
    let deadline = Instant::now() + timeout;
    while path.exists() && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(25));
    }
    if path.exists() {
        Err(LaunchAgentError::StopFailed)
    } else {
        Ok(())
    }
}

fn validate_absolute_path(path: &Path) -> Result<(), LaunchAgentError> {
    if !path.is_absolute()
        || path.as_os_str().as_bytes().contains(&0)
        || path.to_str().is_none()
        || path.components().any(|component| {
            matches!(
                component,
                Component::ParentDir | Component::CurDir | Component::Prefix(_)
            )
        })
    {
        return Err(LaunchAgentError::InvalidPath);
    }
    Ok(())
}

fn xml_path(path: &Path) -> Result<String, LaunchAgentError> {
    validate_absolute_path(path)?;
    Ok(xml_escape(
        path.to_str().ok_or(LaunchAgentError::InvalidPath)?,
    ))
}

fn xml_escape(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&apos;")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};
    use tempfile::tempdir;

    struct FakeController {
        loaded: Cell<bool>,
        pid: Cell<u32>,
        started_at_unix_ms: Cell<u64>,
        reuse_pid: Cell<bool>,
        preexisting_health: Cell<bool>,
        fail_bootstrap: Cell<bool>,
        fail_health: Cell<bool>,
        fail_stop_wait: Cell<bool>,
        calls: RefCell<Vec<&'static str>>,
    }

    impl FakeController {
        fn new(loaded: bool) -> Self {
            Self {
                loaded: Cell::new(loaded),
                pid: Cell::new(if loaded { 41 } else { 0 }),
                started_at_unix_ms: Cell::new(1),
                reuse_pid: Cell::new(false),
                preexisting_health: Cell::new(false),
                fail_bootstrap: Cell::new(false),
                fail_health: Cell::new(false),
                fail_stop_wait: Cell::new(false),
                calls: RefCell::new(Vec::new()),
            }
        }
    }

    impl ServiceController for FakeController {
        fn is_loaded(&self) -> Result<bool, LaunchAgentError> {
            self.calls.borrow_mut().push("is_loaded");
            Ok(self.loaded.get())
        }

        fn bootout(&self) -> Result<(), LaunchAgentError> {
            self.calls.borrow_mut().push("bootout");
            self.loaded.set(false);
            Ok(())
        }

        fn wait_stopped(&self, _socket: &Path, _timeout: Duration) -> Result<(), LaunchAgentError> {
            self.calls.borrow_mut().push("wait_stopped");
            if self.fail_stop_wait.replace(false) || self.loaded.get() {
                Err(LaunchAgentError::StopFailed)
            } else {
                Ok(())
            }
        }

        fn bootstrap(&self, _plist: &Path) -> Result<(), LaunchAgentError> {
            self.calls.borrow_mut().push("bootstrap");
            if self.fail_bootstrap.replace(false) {
                Err(LaunchAgentError::ActivationFailed)
            } else {
                self.loaded.set(true);
                if !self.reuse_pid.get() {
                    self.pid.set(self.pid.get().saturating_add(1).max(42));
                }
                self.started_at_unix_ms
                    .set(self.started_at_unix_ms.get().saturating_add(1));
                Ok(())
            }
        }

        fn kickstart(&self) -> Result<(), LaunchAgentError> {
            self.calls.borrow_mut().push("kickstart");
            Ok(())
        }

        fn current_health(
            &self,
            _socket: &Path,
        ) -> Result<Option<HealthSnapshot>, LaunchAgentError> {
            self.calls.borrow_mut().push("current_health");
            Ok(
                (self.loaded.get() || self.preexisting_health.get()).then(|| HealthSnapshot {
                    v: 1,
                    status: HealthState::Ready,
                    host_version: "0.0.9".to_owned(),
                    pid: self.pid.get().max(41),
                    started_at_unix_ms: self.started_at_unix_ms.get(),
                    socket: "run/host.sock".to_owned(),
                    database_schema: 1,
                    recovered_jobs_on_start: 0,
                }),
            )
        }

        fn wait_healthy(
            &self,
            _socket: &Path,
            expected_version: &str,
            previous_instance: Option<(u32, u64)>,
            _timeout: Duration,
        ) -> Result<(), LaunchAgentError> {
            self.calls.borrow_mut().push("wait_healthy");
            if self.fail_health.replace(false) {
                Err(LaunchAgentError::ActivationFailed)
            } else if self.loaded.get()
                && !expected_version.is_empty()
                && previous_instance != Some((self.pid.get(), self.started_at_unix_ms.get()))
            {
                Ok(())
            } else {
                Err(LaunchAgentError::ActivationFailed)
            }
        }
    }

    fn fixture() -> (tempfile::TempDir, LaunchAgentPaths, PathBuf) {
        let temp = tempdir().unwrap();
        let home = temp.path().join("home");
        fs::create_dir(&home).unwrap();
        let paths = LaunchAgentPaths::from_home(&home).unwrap();
        let source = home.join("candidate");
        fs::write(&source, b"candidate-v2").unwrap();
        fs::set_permissions(&source, fs::Permissions::from_mode(0o700)).unwrap();
        (temp, paths, source)
    }

    #[test]
    fn plist_has_launchd_lifecycle_and_escapes_paths() {
        let temp = tempdir().unwrap();
        let home = temp.path().join("home & <private>");
        fs::create_dir(&home).unwrap();
        let paths = LaunchAgentPaths::from_home(&home).unwrap();
        let plist = String::from_utf8(render_plist(&paths).unwrap()).unwrap();

        assert!(plist.contains("<key>RunAtLoad</key>\n  <true/>"));
        assert!(plist.contains("<key>KeepAlive</key>\n  <true/>"));
        assert!(plist.contains("<string>daemon</string>"));
        assert!(plist.contains("home &amp; &lt;private&gt;"));
        assert!(!plist.contains("home & <private>"));
    }

    #[test]
    fn install_writes_private_files_and_activates_service() {
        let (_temp, paths, source) = fixture();
        let controller = FakeController::new(false);
        deploy(&paths, &source, DeploymentMode::Install, &controller).unwrap();

        assert_eq!(fs::read(&paths.installed_binary).unwrap(), b"candidate-v2");
        assert!(
            String::from_utf8(fs::read(&paths.plist).unwrap())
                .unwrap()
                .contains(LAUNCH_AGENT_LABEL)
        );
        assert_eq!(
            fs::metadata(&paths.installed_binary)
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o700
        );
        assert_eq!(
            fs::metadata(&paths.plist).unwrap().permissions().mode() & 0o777,
            0o600
        );
        assert!(controller.loaded.get());
    }

    #[test]
    fn failed_upgrade_restores_previous_binary_plist_and_service() {
        let (_temp, paths, source) = fixture();
        prepare_directories(&paths).unwrap();
        write_atomic_owned(&paths.installed_binary, b"candidate-v1", 0o700).unwrap();
        write_atomic_owned(&paths.plist, b"old-plist", 0o600).unwrap();
        let controller = FakeController::new(true);
        controller.fail_bootstrap.set(true);

        assert!(matches!(
            deploy(&paths, &source, DeploymentMode::Upgrade, &controller),
            Err(LaunchAgentError::ActivationFailed)
        ));
        assert_eq!(fs::read(&paths.installed_binary).unwrap(), b"candidate-v1");
        assert_eq!(fs::read(&paths.plist).unwrap(), b"old-plist");
        assert!(controller.loaded.get());
        assert_eq!(
            controller
                .calls
                .borrow()
                .iter()
                .filter(|call| **call == "wait_stopped")
                .count(),
            2
        );
    }

    #[test]
    fn failed_stop_wait_reactivates_the_previous_install() {
        let (_temp, paths, source) = fixture();
        prepare_directories(&paths).unwrap();
        write_atomic_owned(&paths.installed_binary, b"candidate-v1", 0o700).unwrap();
        write_atomic_owned(&paths.plist, b"old-plist", 0o600).unwrap();
        let controller = FakeController::new(true);
        controller.fail_stop_wait.set(true);

        assert!(matches!(
            deploy(&paths, &source, DeploymentMode::Upgrade, &controller),
            Err(LaunchAgentError::ActivationFailed)
        ));
        assert_eq!(fs::read(&paths.installed_binary).unwrap(), b"candidate-v1");
        assert_eq!(fs::read(&paths.plist).unwrap(), b"old-plist");
        assert!(controller.loaded.get());
        assert_eq!(
            controller
                .calls
                .borrow()
                .iter()
                .filter(|call| **call == "wait_stopped")
                .count(),
            2
        );
    }

    #[test]
    fn upgrade_waits_for_previous_process_before_replacing_and_bootstrapping() {
        let (_temp, paths, source) = fixture();
        prepare_directories(&paths).unwrap();
        write_atomic_owned(&paths.installed_binary, b"candidate-v1", 0o700).unwrap();
        write_atomic_owned(&paths.plist, b"old-plist", 0o600).unwrap();
        let controller = FakeController::new(true);

        deploy(&paths, &source, DeploymentMode::Upgrade, &controller).unwrap();

        let calls = controller.calls.borrow();
        let bootout = calls.iter().position(|call| *call == "bootout").unwrap();
        let stopped = calls
            .iter()
            .position(|call| *call == "wait_stopped")
            .unwrap();
        let bootstrap = calls.iter().position(|call| *call == "bootstrap").unwrap();
        assert!(bootout < stopped && stopped < bootstrap);
    }

    #[test]
    fn reused_pid_with_a_new_start_time_is_a_new_healthy_instance() {
        let (_temp, paths, source) = fixture();
        prepare_directories(&paths).unwrap();
        write_atomic_owned(&paths.installed_binary, b"candidate-v1", 0o700).unwrap();
        write_atomic_owned(&paths.plist, b"old-plist", 0o600).unwrap();
        let controller = FakeController::new(true);
        controller.reuse_pid.set(true);

        deploy(&paths, &source, DeploymentMode::Upgrade, &controller).unwrap();

        assert_eq!(controller.pid.get(), 41);
        assert_eq!(controller.started_at_unix_ms.get(), 2);
        assert!(controller.loaded.get());
    }

    #[test]
    fn launch_transition_health_failures_are_retryable_but_unsafe_sockets_are_not() {
        assert!(health_is_temporarily_unavailable(
            &HealthError::InvalidResponse
        ));
        assert!(health_is_temporarily_unavailable(&HealthError::Io(
            io::Error::from(io::ErrorKind::ConnectionReset)
        )));
        assert!(!health_is_temporarily_unavailable(
            &HealthError::UnsafeSocket
        ));
    }

    #[test]
    fn successful_launchctl_without_healthy_candidate_rolls_back() {
        let (_temp, paths, source) = fixture();
        prepare_directories(&paths).unwrap();
        write_atomic_owned(&paths.installed_binary, b"candidate-v1", 0o700).unwrap();
        write_atomic_owned(&paths.plist, b"old-plist", 0o600).unwrap();
        let controller = FakeController::new(true);
        controller.fail_health.set(true);

        assert!(matches!(
            deploy(&paths, &source, DeploymentMode::Upgrade, &controller),
            Err(LaunchAgentError::ActivationFailed)
        ));
        assert_eq!(fs::read(&paths.installed_binary).unwrap(), b"candidate-v1");
        assert_eq!(fs::read(&paths.plist).unwrap(), b"old-plist");
        assert!(controller.loaded.get());
        assert_eq!(
            controller
                .calls
                .borrow()
                .iter()
                .filter(|call| **call == "wait_healthy")
                .count(),
            2
        );
    }

    #[test]
    fn upgrade_requires_a_complete_prior_install() {
        let (_temp, paths, source) = fixture();
        let controller = FakeController::new(false);
        assert!(matches!(
            deploy(&paths, &source, DeploymentMode::Upgrade, &controller),
            Err(LaunchAgentError::NotInstalled)
        ));
        assert!(controller.calls.borrow().is_empty());
    }

    #[test]
    fn unloaded_install_rejects_a_preexisting_same_version_health_endpoint() {
        let (_temp, paths, source) = fixture();
        let controller = FakeController::new(false);
        controller.preexisting_health.set(true);

        assert!(matches!(
            deploy(&paths, &source, DeploymentMode::Install, &controller),
            Err(LaunchAgentError::ActivationFailed)
        ));
        assert!(!paths.installed_binary.exists());
        assert!(!paths.plist.exists());
        assert_eq!(
            controller.calls.borrow().as_slice(),
            &["is_loaded", "current_health"]
        );
    }

    #[test]
    fn unsafe_target_and_ambiguous_paths_fail_closed() {
        let (_temp, paths, source) = fixture();
        prepare_directories(&paths).unwrap();
        fs::create_dir(&paths.installed_binary).unwrap();
        let controller = FakeController::new(false);
        assert!(matches!(
            deploy(&paths, &source, DeploymentMode::Install, &controller),
            Err(LaunchAgentError::UnsafeTarget)
        ));
        assert!(LaunchAgentPaths::from_home(Path::new("relative")).is_err());
        assert!(LaunchAgentPaths::from_home(Path::new("/tmp/a/../b")).is_err());
    }

    #[test]
    fn conditional_uninstall_cleanup_preserves_a_racing_replacement() {
        let (_temp, paths, _source) = fixture();
        prepare_directories(&paths).unwrap();
        write_atomic_owned(&paths.installed_binary, b"installed", 0o700).unwrap();

        let result = remove_owned_regular_if_present_with_hook(&paths.installed_binary, || {
            fs::remove_file(&paths.installed_binary).unwrap();
            fs::write(&paths.installed_binary, b"replacement").unwrap();
        });

        assert!(matches!(result, Err(LaunchAgentError::UnsafeTarget)));
        assert_eq!(fs::read(&paths.installed_binary).unwrap(), b"replacement");
    }
}

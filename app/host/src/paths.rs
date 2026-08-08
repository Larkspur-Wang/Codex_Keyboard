use std::ffi::{CStr, CString, OsStr, OsString};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Seek, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Component, Path, PathBuf};

use fs2::FileExt;
use zeroize::Zeroizing;

pub const APP_SUPPORT_DIRECTORY: &str = "EasyCodexInput";

pub(crate) struct ExplicitFileLock(File);

impl ExplicitFileLock {
    pub(crate) fn from_locked(file: File) -> Self {
        Self(file)
    }

    pub(crate) fn set_len(&self, size: u64) -> io::Result<()> {
        self.0.set_len(size)
    }

    pub(crate) fn sync_all(&self) -> io::Result<()> {
        self.0.sync_all()
    }

    pub(crate) fn as_raw_fd(&self) -> i32 {
        self.0.as_raw_fd()
    }

    #[cfg(test)]
    pub(crate) fn file(&self) -> &File {
        &self.0
    }
}

impl Drop for ExplicitFileLock {
    fn drop(&mut self) {
        // A forked child can briefly retain a duplicate descriptor until exec.
        // Explicit unlock makes the guard lifetime authoritative on every OS.
        let _ = FileExt::unlock(&self.0);
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AppPaths {
    pub root: PathBuf,
    pub dashscope_env: PathBuf,
    pub state_database: PathBuf,
    pub installation_id: PathBuf,
    pub device_secret: PathBuf,
    pub cache_secret: PathBuf,
    pub cache_directory: PathBuf,
    pub runtime_directory: PathBuf,
}

impl AppPaths {
    pub fn from_home(home: &Path) -> Self {
        let root = home
            .join("Library")
            .join("Application Support")
            .join(APP_SUPPORT_DIRECTORY);
        Self::from_root(root)
    }

    pub fn from_root(root: PathBuf) -> Self {
        Self {
            dashscope_env: root.join(".env"),
            state_database: root.join("state.sqlite3"),
            installation_id: root.join("installation-id"),
            device_secret: root.join("device-secret.hex"),
            cache_secret: root.join("cache-secret.hex"),
            cache_directory: root.join("cache").join("tts"),
            runtime_directory: root.join("run"),
            root,
        }
    }

    pub fn prepare(&self) -> io::Result<()> {
        secure_directory(&self.root)?;
        secure_directory(&self.cache_directory)?;
        secure_directory(&self.runtime_directory)
    }
}

pub fn secure_directory(path: &Path) -> io::Result<()> {
    let directory = open_owned_directory_chain(path, true)?;
    directory.set_permissions(fs::Permissions::from_mode(0o700))
}

pub fn open_private_file(path: &Path) -> io::Result<File> {
    let parent = path
        .parent()
        .ok_or_else(|| invalid_path("private file has no parent directory"))?;
    let file_name = path
        .file_name()
        .filter(|name| !name.is_empty())
        .ok_or_else(|| invalid_path("private file has no file name"))?;
    let parent = open_owned_directory_chain(parent, false)?;
    let file = open_file_at(&parent, file_name, true)?;
    let metadata = file.metadata()?;
    validate_owned_type(&metadata, false)?;
    file.set_permissions(fs::Permissions::from_mode(0o600))?;
    Ok(file)
}

pub(crate) fn replace_private_file(path: &Path, contents: &[u8]) -> io::Result<()> {
    let parent_path = path
        .parent()
        .ok_or_else(|| invalid_path("private file has no parent directory"))?;
    let file_name = path
        .file_name()
        .filter(|name| !name.is_empty())
        .ok_or_else(|| invalid_path("private file has no file name"))?;
    let parent = open_owned_directory_chain(parent_path, false)?;
    let destination =
        CString::new(file_name.as_bytes()).map_err(|_| invalid_path("file name contains NUL"))?;
    let temporary_name = OsString::from(format!(
        ".{}.{}.tmp",
        file_name.to_string_lossy(),
        uuid::Uuid::new_v4()
    ));
    let temporary = CString::new(temporary_name.as_bytes())
        .map_err(|_| invalid_path("temporary file name contains NUL"))?;
    let descriptor = unsafe {
        libc::openat(
            parent.as_raw_fd(),
            temporary.as_ptr(),
            libc::O_CREAT | libc::O_EXCL | libc::O_RDWR | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            0o600,
        )
    };
    if descriptor < 0 {
        return Err(io::Error::last_os_error());
    }
    let mut file = unsafe { File::from_raw_fd(descriptor) };
    let result = (|| {
        file.set_permissions(fs::Permissions::from_mode(0o600))?;
        file.write_all(contents)?;
        file.sync_all()?;
        file.rewind()?;
        let mut readback = Zeroizing::new(Vec::with_capacity(contents.len().saturating_add(1)));
        (&file)
            .take(contents.len().saturating_add(1) as u64)
            .read_to_end(&mut readback)?;
        if readback.as_slice() != contents {
            return Err(invalid_path("private temporary file readback mismatch"));
        }
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
        unsafe {
            libc::unlinkat(parent.as_raw_fd(), temporary.as_ptr(), 0);
        }
    }
    result
}

pub(crate) fn open_file_at(
    directory: &File,
    name: &std::ffi::OsStr,
    create: bool,
) -> io::Result<File> {
    let name = CString::new(name.as_bytes()).map_err(|_| invalid_path("file name contains NUL"))?;
    let access = if create {
        libc::O_CREAT | libc::O_RDWR
    } else {
        libc::O_RDONLY
    };
    let descriptor = unsafe {
        libc::openat(
            directory.as_raw_fd(),
            name.as_ptr(),
            access | libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK,
            0o600,
        )
    };
    if descriptor < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(unsafe { File::from_raw_fd(descriptor) })
    }
}

pub(crate) fn directory_names_at(directory: &File) -> io::Result<Vec<OsString>> {
    let duplicated = unsafe {
        libc::openat(
            directory.as_raw_fd(),
            c".".as_ptr(),
            libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC,
        )
    };
    if duplicated < 0 {
        return Err(io::Error::last_os_error());
    }
    let stream = unsafe { libc::fdopendir(duplicated) };
    if stream.is_null() {
        let error = io::Error::last_os_error();
        unsafe {
            libc::close(duplicated);
        }
        return Err(error);
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
            return Err(io::Error::from_raw_os_error(error));
        }
        let name = unsafe { CStr::from_ptr((*entry).d_name.as_ptr()) }.to_bytes();
        if name != b"." && name != b".." {
            names.push(OsString::from_vec(name.to_vec()));
        }
    }
    Ok(names)
}

pub(crate) fn metadata_at(directory: &File, name: &OsStr) -> io::Result<libc::stat> {
    let name = CString::new(name.as_bytes()).map_err(|_| invalid_path("file name contains NUL"))?;
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
        return Err(io::Error::last_os_error());
    }
    Ok(unsafe { metadata.assume_init() })
}

pub(crate) fn unlink_file_at(directory: &File, name: &OsStr) -> io::Result<()> {
    let name = CString::new(name.as_bytes()).map_err(|_| invalid_path("file name contains NUL"))?;
    if unsafe { libc::unlinkat(directory.as_raw_fd(), name.as_ptr(), 0) } != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
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

pub(crate) fn open_owned_directory_chain(path: &Path, create_missing: bool) -> io::Result<File> {
    if !path.is_absolute() {
        return Err(invalid_path("private paths must be absolute"));
    }
    let resolved = resolve_root_owned_symlink_prefix(path)?;
    let effective_uid = unsafe { libc::geteuid() };
    let mut directory = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_DIRECTORY | libc::O_CLOEXEC)
        .open("/")?;
    let mut entered_owned_tree = false;

    for component in resolved.components() {
        let name = match component {
            Component::RootDir => continue,
            Component::Normal(name) => name,
            Component::ParentDir => return Err(invalid_path("parent path is not allowed")),
            Component::CurDir => continue,
            Component::Prefix(_) => return Err(invalid_path("unsupported path prefix")),
        };
        let name = CString::new(name.as_bytes())
            .map_err(|_| invalid_path("directory name contains NUL"))?;
        let mut descriptor = unsafe {
            libc::openat(
                directory.as_raw_fd(),
                name.as_ptr(),
                libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            )
        };
        if descriptor < 0 {
            let error = io::Error::last_os_error();
            if error.kind() != io::ErrorKind::NotFound || !create_missing || !entered_owned_tree {
                return Err(error);
            }
            let created =
                unsafe { libc::mkdirat(directory.as_raw_fd(), name.as_ptr(), 0o700) } == 0;
            if !created {
                let mkdir_error = io::Error::last_os_error();
                if mkdir_error.kind() != io::ErrorKind::AlreadyExists {
                    return Err(mkdir_error);
                }
            }
            if created
                && unsafe { libc::fchmodat(directory.as_raw_fd(), name.as_ptr(), 0o700, 0) } != 0
            {
                return Err(io::Error::last_os_error());
            }
            descriptor = unsafe {
                libc::openat(
                    directory.as_raw_fd(),
                    name.as_ptr(),
                    libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                )
            };
            if descriptor < 0 {
                return Err(io::Error::last_os_error());
            }
        }
        let next = unsafe { File::from_raw_fd(descriptor) };
        let metadata = next.metadata()?;
        if !metadata.is_dir() {
            return Err(invalid_path("expected a directory"));
        }
        if metadata.uid() == effective_uid {
            entered_owned_tree = true;
            if metadata.mode() & 0o022 != 0 {
                return Err(invalid_path(
                    "user-owned directory is group or world writable",
                ));
            }
        } else if entered_owned_tree
            || metadata.uid() != 0
            || (metadata.mode() & 0o022 != 0 && metadata.mode() & sticky_directory_bit() == 0)
        {
            return Err(invalid_path("untrusted directory component"));
        }
        directory = next;
    }

    if entered_owned_tree {
        Ok(directory)
    } else {
        Err(invalid_path(
            "path has no existing directory owned by the current user",
        ))
    }
}

#[cfg(target_os = "macos")]
fn sticky_directory_bit() -> u32 {
    u32::from(libc::S_ISVTX)
}

#[cfg(not(target_os = "macos"))]
fn sticky_directory_bit() -> u32 {
    libc::S_ISVTX
}

fn resolve_root_owned_symlink_prefix(path: &Path) -> io::Result<PathBuf> {
    let effective_uid = unsafe { libc::geteuid() };
    let mut resolved = PathBuf::from("/");
    let mut entered_owned_tree = false;

    for component in path.components() {
        let name = match component {
            Component::RootDir => continue,
            Component::Normal(name) => name,
            Component::ParentDir => return Err(invalid_path("parent path is not allowed")),
            Component::CurDir => continue,
            Component::Prefix(_) => return Err(invalid_path("unsupported path prefix")),
        };
        let candidate = resolved.join(name);
        match fs::symlink_metadata(&candidate) {
            Ok(metadata) if metadata.file_type().is_symlink() => {
                if entered_owned_tree || metadata.uid() != 0 {
                    return Err(invalid_path("symlink component is not allowed"));
                }
                resolved = fs::canonicalize(&candidate)?;
            }
            Ok(metadata) => {
                if metadata.uid() == effective_uid {
                    entered_owned_tree = true;
                }
                resolved.push(name);
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => resolved.push(name),
            Err(error) => return Err(error),
        }
    }
    Ok(resolved)
}

fn validate_owned_type(metadata: &fs::Metadata, directory: bool) -> io::Result<()> {
    let correct_type = if directory {
        metadata.is_dir()
    } else {
        metadata.is_file()
    };
    if !correct_type {
        return Err(invalid_path(if directory {
            "expected a directory"
        } else {
            "expected a regular file"
        }));
    }
    // SAFETY: geteuid has no preconditions and does not dereference pointers.
    let effective_uid = unsafe { libc::geteuid() };
    if metadata.uid() != effective_uid {
        return Err(invalid_path("path is not owned by the current user"));
    }
    Ok(())
}

fn invalid_path(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::tempdir;

    #[test]
    fn prepares_private_application_directories() {
        let temp = tempdir().unwrap();
        let paths = AppPaths::from_root(temp.path().join("state"));

        paths.prepare().unwrap();

        for path in [
            &paths.root,
            &paths.cache_directory,
            &paths.runtime_directory,
        ] {
            assert_eq!(
                fs::metadata(path).unwrap().permissions().mode() & 0o777,
                0o700
            );
        }
    }

    #[test]
    fn private_file_is_never_left_world_readable() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("private");

        drop(open_private_file(&path).unwrap());

        assert_eq!(
            fs::metadata(path).unwrap().permissions().mode() & 0o777,
            0o600
        );
    }

    #[test]
    fn explicit_lock_releases_on_early_error_while_a_duplicate_descriptor_survives() {
        fn fail_after_wrapping(file: File) -> io::Result<()> {
            let _lock = ExplicitFileLock::from_locked(file);
            Err(io::Error::other("injected post-lock failure"))
        }

        let temp = tempdir().unwrap();
        let path = temp.path().join("process.lock");
        let owner = open_private_file(&path).unwrap();
        owner.try_lock_exclusive().unwrap();
        let inherited_descriptor = owner.try_clone().unwrap();

        assert!(fail_after_wrapping(owner).is_err());
        let contender = open_private_file(&path).unwrap();
        contender.try_lock_exclusive().unwrap();

        drop(contender);
        drop(inherited_descriptor);
    }

    #[test]
    fn rejects_symlink_directory_and_file_targets() {
        use std::os::unix::fs::symlink;

        let temp = tempdir().unwrap();
        let real_directory = temp.path().join("real");
        fs::create_dir(&real_directory).unwrap();
        let linked_directory = temp.path().join("linked");
        symlink(&real_directory, &linked_directory).unwrap();
        assert!(secure_directory(&linked_directory.join("nested")).is_err());
        fs::create_dir(real_directory.join("existing")).unwrap();
        assert!(secure_directory(&linked_directory.join("existing")).is_err());

        let real_file = temp.path().join("real-file");
        fs::write(&real_file, b"state").unwrap();
        let linked_file = temp.path().join("linked-file");
        symlink(&real_file, &linked_file).unwrap();
        assert!(open_private_file(&linked_file).is_err());
    }

    #[test]
    fn rejects_relative_private_paths() {
        assert!(secure_directory(Path::new("relative/private")).is_err());
        assert!(open_private_file(Path::new("relative/private")).is_err());
    }

    #[test]
    fn rejects_parent_components_instead_of_opening_a_decoy() {
        let temp = tempdir().unwrap();
        let parent = temp.path().join("parent");
        let nested = parent.join("nested");
        fs::create_dir_all(&nested).unwrap();
        fs::write(parent.join("private"), b"intended").unwrap();
        fs::write(nested.join("private"), b"decoy").unwrap();

        let ambiguous = nested.join("..").join("private");
        assert!(open_private_file(&ambiguous).is_err());
        assert_eq!(fs::read(parent.join("private")).unwrap(), b"intended");
        assert_eq!(fs::read(nested.join("private")).unwrap(), b"decoy");
    }

    #[test]
    fn directory_enumeration_and_unlink_stay_anchored_after_path_replacement() {
        use std::os::unix::fs::symlink;

        let temp = tempdir().unwrap();
        let root = temp.path().join("private-root");
        let moved = temp.path().join("moved-root");
        let outside = temp.path().join("outside");
        fs::create_dir(&root).unwrap();
        fs::write(root.join("stale.tmp"), b"private").unwrap();
        let directory = open_owned_directory_chain(&root, false).unwrap();

        fs::rename(&root, &moved).unwrap();
        fs::create_dir(&outside).unwrap();
        fs::write(outside.join("stale.tmp"), b"outside").unwrap();
        symlink(&outside, &root).unwrap();

        let names = directory_names_at(&directory).unwrap();
        assert_eq!(names, [OsString::from("stale.tmp")]);
        let metadata = metadata_at(&directory, &names[0]).unwrap();
        assert_eq!(metadata.st_size, 7);
        unlink_file_at(&directory, &names[0]).unwrap();

        assert!(!moved.join("stale.tmp").exists());
        assert_eq!(fs::read(outside.join("stale.tmp")).unwrap(), b"outside");
    }
}

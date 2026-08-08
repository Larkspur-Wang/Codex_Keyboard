fn host_command() -> std::process::Command {
    let mut command = std::process::Command::new(env!("CARGO_BIN_EXE_easy-codex-host"));
    command.env("ECI_LAN_AUDIO_PORT", "0");
    command
}

fn wait_for_path(path: &std::path::Path, present: bool) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while path.exists() != present && std::time::Instant::now() < deadline {
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
    assert_eq!(path.exists(), present);
}

fn wait_for_exit(child: &mut std::process::Child) -> std::process::ExitStatus {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    loop {
        if let Some(status) = child.try_wait().expect("poll Host child") {
            return status;
        }
        if std::time::Instant::now() >= deadline {
            let _ = child.kill();
            panic!("Host child did not exit before deadline");
        }
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
}

fn short_home() -> tempfile::TempDir {
    tempfile::Builder::new()
        .prefix("eci-")
        .tempdir_in("/tmp")
        .expect("create short HOME fixture")
}

fn wait_for_health(
    home: &std::path::Path,
    expected_pid: u32,
) -> easy_codex_host::health::HealthSnapshot {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    loop {
        let output = host_command()
            .arg("health")
            .env("HOME", home)
            .output()
            .unwrap();
        if output.status.success()
            && let Ok(snapshot) =
                serde_json::from_slice::<easy_codex_host::health::HealthSnapshot>(&output.stdout)
            && snapshot.pid == expected_pid
        {
            return snapshot;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "Host health did not become ready"
        );
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
}

#[test]
fn no_argument_startup_identifies_the_local_wifi_host() {
    let output = host_command().output().expect("run easy-codex-host");

    assert!(output.status.success());
    assert_eq!(
        String::from_utf8(output.stdout).unwrap(),
        "codex-keyboard local Wi-Fi Host ready\n"
    );
    assert!(output.stderr.is_empty());
}

#[test]
fn unknown_commands_fail_closed() {
    let output = host_command().arg("relay-connect").output().unwrap();

    assert!(!output.status.success());
    assert!(output.stdout.is_empty());
    assert!(
        String::from_utf8(output.stderr)
            .unwrap()
            .contains("unknown command")
    );
}

#[test]
fn daemon_health_second_instance_and_graceful_shutdown_are_real_processes() {
    let home = short_home();
    let paths = easy_codex_host::paths::AppPaths::from_home(home.path());
    let socket = paths.runtime_directory.join("host.sock");
    let mut daemon = host_command()
        .arg("daemon")
        .env("HOME", home.path())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .expect("spawn daemon");
    wait_for_path(&socket, true);

    let snapshot = wait_for_health(home.path(), daemon.id());
    assert_eq!(snapshot.pid, daemon.id());
    assert_eq!(snapshot.socket, "run/host.sock");

    let second = host_command()
        .arg("daemon")
        .env("HOME", home.path())
        .output()
        .expect("run second daemon");
    assert!(!second.status.success());
    assert!(
        String::from_utf8(second.stderr)
            .unwrap()
            .contains("AlreadyRunning")
    );

    assert_eq!(unsafe { libc::kill(daemon.id() as i32, libc::SIGTERM) }, 0);
    assert!(wait_for_exit(&mut daemon).success());
    wait_for_path(&socket, false);
}

#[test]
fn daemon_recovers_a_socket_left_by_sigkill() {
    let home = short_home();
    let paths = easy_codex_host::paths::AppPaths::from_home(home.path());
    let socket = paths.runtime_directory.join("host.sock");
    let mut first = host_command()
        .arg("daemon")
        .env("HOME", home.path())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .expect("spawn first daemon");
    wait_for_path(&socket, true);
    first.kill().unwrap();
    assert!(!wait_for_exit(&mut first).success());
    assert!(socket.exists());

    let mut recovered = host_command()
        .arg("daemon")
        .env("HOME", home.path())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .expect("spawn recovered daemon");
    let snapshot = wait_for_health(home.path(), recovered.id());
    assert_eq!(snapshot.pid, recovered.id());

    assert_eq!(
        unsafe { libc::kill(recovered.id() as i32, libc::SIGTERM) },
        0
    );
    assert!(wait_for_exit(&mut recovered).success());
}

#[test]
fn dashscope_status_reads_the_private_local_env_contract() {
    let home = short_home();
    let output = host_command()
        .arg("dashscope-key-status")
        .env("HOME", home.path())
        .output()
        .expect("run easy-codex-host");

    assert!(output.status.success());
    assert_eq!(
        String::from_utf8(output.stdout).unwrap(),
        "status=missing\n"
    );
}

#[test]
fn dashscope_import_requires_an_explicit_readable_source() {
    let home = short_home();
    let output = host_command()
        .args(["import-dashscope-key", "/explicit/source.env"])
        .env("HOME", home.path())
        .output()
        .expect("run easy-codex-host");

    assert!(!output.status.success());
    let stderr = String::from_utf8(output.stderr).expect("stderr is UTF-8");
    assert!(stderr.contains("UnsafeSource"));
}

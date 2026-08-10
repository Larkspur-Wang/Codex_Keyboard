#![cfg(target_os = "macos")]

use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::process::ExitStatusExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use easy_codex_host::codex_runner::{CodexRunner, CodexRunnerConfig};
use easy_codex_host::spark_runner::{SparkError, SparkRunner, SparkRunnerConfig};
use easy_codex_host::store::{
    Job, JobFailureKind, PendingSummaryCompletion, SummaryClaim, SummaryClaimOutcome,
};
use tempfile::tempdir;

#[test]
fn parent_sigkill_helper() {
    let Some(root) = std::env::var_os("ECI_PARENT_DEATH_ROOT") else {
        return;
    };
    let root = PathBuf::from(root);
    let executable = root.join("fake-codex");
    let supervisor_executable = std::env::var_os("ECI_SUPERVISOR_EXECUTABLE")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_BIN_EXE_easy-codex-host")));
    let runner = CodexRunner::new(CodexRunnerConfig {
        executable,
        supervisor_executable: Some(supervisor_executable),
        timeout: Duration::from_secs(60),
        max_stdout_line_bytes: 1024,
        max_stdout_total_bytes: 4096,
        max_stderr_bytes: 1024,
    });
    let job = Job {
        request_id: "parent-death".into(),
        task_id: "00000000-0000-4000-8000-000000000001".into(),
        slot: 1,
        generation: 1,
        prompt: "start".into(),
        cwd: root,
        recovery_count: 0,
        claim_generation: 1,
    };
    let _ = runner.run(&job);
}

#[test]
fn spark_parent_sigkill_helper() {
    let Some(root) = std::env::var_os("ECI_SPARK_PARENT_DEATH_ROOT") else {
        return;
    };
    let root = PathBuf::from(root);
    let supervisor_executable = std::env::var_os("ECI_SUPERVISOR_EXECUTABLE")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_BIN_EXE_easy-codex-host")));
    let runner = SparkRunner::new(SparkRunnerConfig {
        executable: root.join("fake-codex"),
        supervisor_executable: Some(supervisor_executable),
        auth_path: root.join("auth.json"),
        temp_root: root.join("runs"),
        timeout: Duration::from_secs(60),
        max_stdout_bytes: 4096,
        max_stderr_bytes: 1024,
    });
    let claim = SummaryClaim {
        outcome: SummaryClaimOutcome::Inserted,
        request_id: "spark-parent-death".into(),
        task_id: "00000000-0000-4000-8000-000000000001".into(),
        generation: 1,
        previous_unread: None,
        completions: vec![PendingSummaryCompletion {
            completion_id: "00000000-0000-4000-8000-000000000002".into(),
            turn_pack: r#"{"v":1,"turn_id":"00000000-0000-4000-8000-000000000002","user":[],"assistant":["start"],"tools":[]}"#.into(),
        }],
    };
    let _ = runner.run(&claim, None);
}

#[test]
fn macos_supervisor_kills_codex_group_after_host_sigkill() {
    let temp = tempdir().unwrap();
    let pid_file = temp.path().join("child.pid");
    let descendant_file = temp.path().join("descendant.pid");
    let args_file = temp.path().join("child.args");
    let prompt_file = temp.path().join("child.stdin");
    let leaked_socket = temp.path().join("leaked-control-fd");
    let control_fd_file = temp.path().join("control.fd");
    let supervisor = temp.path().join("supervisor-wrapper");
    fs::write(
        &supervisor,
        format!(
            "#!/bin/sh\nset -eu\nprintf '%s' \"$5\" > '{}'\nexec '{}' \"$@\"\n",
            control_fd_file.display(),
            env!("CARGO_BIN_EXE_easy-codex-host")
        ),
    )
    .unwrap();
    fs::set_permissions(&supervisor, fs::Permissions::from_mode(0o700)).unwrap();
    write_script(
        temp.path(),
        &format!(
            "control_fd=$(cat '{}'); [ ! -S /dev/fd/$control_fd ] || echo $control_fd > '{}'; printf '%s\\n' \"$@\" > '{}'; cat > '{}'; sleep 30 & echo $! > '{}'; echo $$ > '{}'; wait; printf '{{}}\\n'",
            control_fd_file.display(),
            leaked_socket.display(),
            args_file.display(),
            prompt_file.display(),
            descendant_file.display(),
            pid_file.display(),
        ),
    );
    let mut parent = Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "parent_sigkill_helper", "--nocapture"])
        .env("ECI_PARENT_DEATH_ROOT", temp.path())
        .env("ECI_SUPERVISOR_EXECUTABLE", supervisor)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    wait_for_file(&pid_file, &mut parent);
    wait_for_file(&descendant_file, &mut parent);
    assert_eq!(
        fs::read_to_string(&args_file).unwrap(),
        "exec\nresume\n--json\n00000000-0000-4000-8000-000000000001\n-\n"
    );
    assert_eq!(fs::read_to_string(&prompt_file).unwrap(), "start");
    assert!(!leaked_socket.exists(), "control socket leaked into Codex");
    let child_pid: i32 = fs::read_to_string(&pid_file)
        .unwrap()
        .trim()
        .parse()
        .unwrap();
    let descendant_pid: i32 = fs::read_to_string(&descendant_file)
        .unwrap()
        .trim()
        .parse()
        .unwrap();

    assert_eq!(unsafe { libc::kill(parent.id() as i32, libc::SIGKILL) }, 0);
    assert_eq!(parent.wait().unwrap().signal(), Some(libc::SIGKILL));

    let deadline = Instant::now() + Duration::from_secs(2);
    while (process_exists(child_pid) || process_exists(descendant_pid)) && Instant::now() < deadline
    {
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(
        !process_exists(child_pid),
        "Codex child survived Host SIGKILL"
    );
    assert!(
        !process_exists(descendant_pid),
        "Codex descendant survived Host SIGKILL"
    );
}

#[test]
fn spark_supervisor_kills_cli_group_after_host_sigkill() {
    let temp = tempdir().unwrap();
    let pid_file = temp.path().join("spark-child.pid");
    let descendant_file = temp.path().join("spark-descendant.pid");
    let prompt_file = temp.path().join("spark.stdin");
    let control_fd_file = temp.path().join("spark-control.fd");
    let owner_fd_file = temp.path().join("spark-owner.fd");
    let task_fd_file = temp.path().join("spark-task.fd");
    let supervisor_pid_file = temp.path().join("spark-supervisor.pid");
    let cleanup_gate = temp.path().join("allow-supervisor-cleanup");
    let supervisor = temp.path().join("spark-supervisor-wrapper");
    fs::write(
        &supervisor,
        format!(
            "#!/bin/sh\nset -eu\nprintf '%s' \"$3\" > '{}'\nprintf '%s' \"$4\" > '{}'\nprintf '%s' \"$5\" > '{}'\nprintf '%s' \"$$\" > '{}'\nexport ECI_SPARK_TEST_PARENT_DEATH_GATE='{}'\nexec '{}' \"$@\"\n",
            control_fd_file.display(),
            owner_fd_file.display(),
            task_fd_file.display(),
            supervisor_pid_file.display(),
            cleanup_gate.display(),
            env!("CARGO_BIN_EXE_easy-codex-host")
        ),
    )
    .unwrap();
    fs::set_permissions(&supervisor, fs::Permissions::from_mode(0o700)).unwrap();
    fs::write(temp.path().join("auth.json"), b"{\"fixture\":true}").unwrap();
    fs::set_permissions(
        temp.path().join("auth.json"),
        fs::Permissions::from_mode(0o600),
    )
    .unwrap();
    write_script(
        temp.path(),
        &format!(
            "control_fd=$(cat '{}'); owner_fd=$(cat '{}'); task_fd=$(cat '{}'); [ ! -e /dev/fd/$control_fd ]; [ ! -e /dev/fd/$owner_fd ]; [ ! -e /dev/fd/$task_fd ]; test -r \"$CODEX_HOME/auth.json\"; cat > '{}'; sleep 30 & echo $! > '{}'; echo $$ > '{}'; wait",
            control_fd_file.display(),
            owner_fd_file.display(),
            task_fd_file.display(),
            prompt_file.display(),
            descendant_file.display(),
            pid_file.display(),
        ),
    );
    let mut parent = Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "spark_parent_sigkill_helper", "--nocapture"])
        .env("ECI_SPARK_PARENT_DEATH_ROOT", temp.path())
        .env("ECI_SUPERVISOR_EXECUTABLE", supervisor)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    wait_for_file(&pid_file, &mut parent);
    wait_for_file(&descendant_file, &mut parent);
    wait_for_file(&supervisor_pid_file, &mut parent);
    assert!(
        fs::read_to_string(prompt_file)
            .unwrap()
            .contains("new_completions")
    );
    let child_pid: i32 = fs::read_to_string(pid_file)
        .unwrap()
        .trim()
        .parse()
        .unwrap();
    let descendant_pid: i32 = fs::read_to_string(descendant_file)
        .unwrap()
        .trim()
        .parse()
        .unwrap();
    let supervisor_pid: i32 = fs::read_to_string(supervisor_pid_file)
        .unwrap()
        .trim()
        .parse()
        .unwrap();
    assert_eq!(unsafe { libc::kill(parent.id() as i32, libc::SIGKILL) }, 0);
    assert_eq!(parent.wait().unwrap().signal(), Some(libc::SIGKILL));

    let busy_runner = SparkRunner::new(SparkRunnerConfig {
        executable: temp.path().join("fake-codex"),
        supervisor_executable: None,
        auth_path: temp.path().join("auth.json"),
        temp_root: temp.path().join("runs"),
        timeout: Duration::from_secs(2),
        max_stdout_bytes: 4096,
        max_stderr_bytes: 1024,
    });
    let claim = SummaryClaim {
        outcome: SummaryClaimOutcome::Inserted,
        request_id: "spark-recovery".into(),
        task_id: "00000000-0000-4000-8000-000000000001".into(),
        generation: 1,
        previous_unread: None,
        completions: vec![PendingSummaryCompletion {
            completion_id: "00000000-0000-4000-8000-000000000002".into(),
            turn_pack: r#"{"v":1,"turn_id":"00000000-0000-4000-8000-000000000002","user":[],"assistant":["recover"],"tools":[]}"#.into(),
        }],
    };
    assert_eq!(busy_runner.run(&claim, None), Err(SparkError::Busy));
    let live_runs = fs::read_dir(temp.path().join("runs"))
        .unwrap()
        .filter_map(Result::ok)
        .filter(|entry| entry.file_name().to_string_lossy().starts_with("spark-"))
        .count();
    assert_eq!(live_runs, 1);
    fs::write(cleanup_gate, b"continue").unwrap();
    let deadline = Instant::now() + Duration::from_secs(2);
    while (process_exists(child_pid)
        || process_exists(descendant_pid)
        || process_exists(supervisor_pid))
        && Instant::now() < deadline
    {
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(!process_exists(child_pid));
    assert!(!process_exists(descendant_pid));
    assert!(!process_exists(supervisor_pid));

    write_script(
        temp.path(),
        "output=''; while [ \"$#\" -gt 0 ]; do if [ \"$1\" = '--output-last-message' ]; then output=\"$2\"; shift 2; else shift; fi; done; cat >/dev/null; printf '%s' '{\"schema\":1,\"facts\":[\"recovered\"],\"pending\":[],\"decisions\":[],\"spoken_text\":\"recovered\",\"covers_new_completions\":[\"00000000-0000-4000-8000-000000000002\"]}' > \"$output\"",
    );
    let runner = SparkRunner::new(SparkRunnerConfig {
        executable: temp.path().join("fake-codex"),
        supervisor_executable: None,
        auth_path: temp.path().join("auth.json"),
        temp_root: temp.path().join("runs"),
        timeout: Duration::from_secs(2),
        max_stdout_bytes: 4096,
        max_stderr_bytes: 1024,
    });
    runner.run(&claim, None).unwrap();
    let leaked_runs = fs::read_dir(temp.path().join("runs"))
        .unwrap()
        .filter_map(Result::ok)
        .filter(|entry| entry.file_name().to_string_lossy().starts_with("spark-"))
        .count();
    assert_eq!(leaked_runs, 0);
}

#[test]
fn supervised_missing_cli_keeps_the_sanitized_category() {
    let temp = tempdir().unwrap();
    let runner = CodexRunner::new(CodexRunnerConfig {
        executable: temp.path().join("missing-codex"),
        supervisor_executable: Some(PathBuf::from(env!("CARGO_BIN_EXE_easy-codex-host"))),
        timeout: Duration::from_secs(2),
        max_stdout_line_bytes: 1024,
        max_stdout_total_bytes: 4096,
        max_stderr_bytes: 1024,
    });
    let job = Job {
        request_id: "missing".into(),
        task_id: "00000000-0000-4000-8000-000000000001".into(),
        slot: 1,
        generation: 1,
        prompt: "prompt".into(),
        cwd: temp.path().to_path_buf(),
        recovery_count: 0,
        claim_generation: 1,
    };
    assert_eq!(runner.run(&job), Err(JobFailureKind::CliMissing));
}

#[test]
fn supervisor_keeps_guarding_after_leader_exit_until_host_cleans_group() {
    let temp = tempdir().unwrap();
    let leader_file = temp.path().join("child.pid");
    let descendant_file = temp.path().join("descendant.pid");
    let gate = temp.path().join("leader.exit");
    write_script(
        temp.path(),
        &format!(
            "cat >/dev/null; sleep 30 & echo $! > '{}'; echo $$ > '{}'; while [ ! -e '{}' ]; do sleep 0.01; done; exit 0",
            descendant_file.display(),
            leader_file.display(),
            gate.display(),
        ),
    );
    let mut parent = Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "parent_sigkill_helper", "--nocapture"])
        .env("ECI_PARENT_DEATH_ROOT", temp.path())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    wait_for_file(&leader_file, &mut parent);
    wait_for_file(&descendant_file, &mut parent);
    let leader: i32 = fs::read_to_string(leader_file)
        .unwrap()
        .trim()
        .parse()
        .unwrap();
    let descendant: i32 = fs::read_to_string(descendant_file)
        .unwrap()
        .trim()
        .parse()
        .unwrap();
    assert_eq!(unsafe { libc::kill(parent.id() as i32, libc::SIGSTOP) }, 0);
    fs::write(gate, b"exit").unwrap();
    let leader_deadline = Instant::now() + Duration::from_secs(2);
    while process_exists(leader) && Instant::now() < leader_deadline {
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(!process_exists(leader));
    assert!(process_exists(descendant));

    assert_eq!(unsafe { libc::kill(parent.id() as i32, libc::SIGKILL) }, 0);
    assert_eq!(parent.wait().unwrap().signal(), Some(libc::SIGKILL));
    let deadline = Instant::now() + Duration::from_secs(2);
    while process_exists(descendant) && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(!process_exists(descendant));
}

fn write_script(directory: &Path, body: &str) {
    let path = directory.join("fake-codex");
    fs::write(&path, format!("#!/bin/sh\nset -eu\n{body}\n")).unwrap();
    fs::set_permissions(path, fs::Permissions::from_mode(0o700)).unwrap();
}

fn wait_for_file(path: &Path, child: &mut std::process::Child) {
    let deadline = Instant::now() + Duration::from_secs(5);
    while !path.exists() {
        if let Some(status) = child.try_wait().unwrap() {
            panic!("parent helper exited before child checkpoint: {status}");
        }
        assert!(
            Instant::now() < deadline,
            "parent helper did not start child"
        );
        std::thread::sleep(Duration::from_millis(10));
    }
}

fn process_exists(pid: i32) -> bool {
    unsafe { libc::kill(pid, 0) == 0 }
}

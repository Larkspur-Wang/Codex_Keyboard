//! Local DeepSeek adapter. Prompts and replies never enter argv or logs.
use std::io::{Read, Write};
use std::path::Path;
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use crate::store::JobFailureKind;

pub(crate) fn run(
    launcher: &Path,
    cwd: &Path,
    prompt: &str,
    cancel: &AtomicBool,
    timeout: Duration,
) -> Result<String, JobFailureKind> {
    #[cfg(windows)]
    let mut command = {
        let root = launcher
            .ancestors()
            .nth(5)
            .ok_or(JobFailureKind::CliMissing)?;
        let executable = root.join("DeepSeek Harness.exe");
        if !executable.is_file() {
            return Err(JobFailureKind::CliMissing);
        }
        let mut command = Command::new(executable);
        command.env("ELECTRON_RUN_AS_NODE", "1").args([
            std::ffi::OsStr::new("--expose-internals"),
            root.join(
                "resources/app.asar/dsh/node_modules/@deepseek-ai/dsh-desktop-host/lib/cli.js",
            )
            .as_os_str(),
        ]);
        command
    };
    #[cfg(not(windows))]
    let mut command = Command::new(launcher);
    command
        .args(["--profile", "headless", "--json", "-"])
        .current_dir(cwd);
    drive(command, prompt, cancel, timeout)
}

fn bounded_read(mut pipe: impl Read, limit: usize) -> Result<Vec<u8>, JobFailureKind> {
    let mut output = Vec::new();
    let mut buffer = [0; 4096];
    let mut exceeded = false;
    loop {
        let count = pipe
            .read(&mut buffer)
            .map_err(|_| JobFailureKind::ProcessIo)?;
        if count == 0 {
            break;
        }
        if !exceeded && output.len().saturating_add(count) <= limit {
            output.extend_from_slice(&buffer[..count]);
        } else {
            exceeded = true;
        }
    }
    if exceeded {
        Err(JobFailureKind::OutputTooLarge)
    } else {
        Ok(output)
    }
}

fn drive(
    mut command: Command,
    prompt: &str,
    cancel: &AtomicBool,
    timeout: Duration,
) -> Result<String, JobFailureKind> {
    if cancel.load(Ordering::Acquire) {
        return Err(JobFailureKind::ProcessIo);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    let mut child = command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|_| JobFailureKind::ProcessIo)?;
    let stdout = child.stdout.take().ok_or(JobFailureKind::ProcessIo)?;
    let stderr = child.stderr.take().ok_or(JobFailureKind::ProcessIo)?;
    let out = thread::spawn(move || bounded_read(stdout, 4 * 1024 * 1024));
    let err = thread::spawn(move || bounded_read(stderr, 256 * 1024));
    // Close stdin after writing: headless '-' reads until EOF.
    let mut input = child.stdin.take().ok_or(JobFailureKind::ProcessIo)?;
    let prompt = zeroize::Zeroizing::new(prompt.as_bytes().to_vec());
    let write_failed = std::sync::Arc::new(AtomicBool::new(false));
    let writer_failed = std::sync::Arc::clone(&write_failed);
    let writer = thread::spawn(move || {
        let result = input
            .write_all(&prompt)
            .map_err(|_| JobFailureKind::ProcessIo);
        writer_failed.store(result.is_err(), Ordering::Release);
        result
    });
    let deadline = Instant::now() + timeout;
    let status = loop {
        if write_failed.load(Ordering::Acquire)
            || cancel.load(Ordering::Acquire)
            || Instant::now() >= deadline
        {
            #[cfg(windows)]
            {
                use std::os::windows::process::CommandExt;
                let _ = Command::new("taskkill.exe")
                    .args(["/T", "/F", "/PID", &child.id().to_string()])
                    .creation_flags(0x0800_0000)
                    .stdout(Stdio::null())
                    .stderr(Stdio::null())
                    .status();
            }
            let _ = child.kill();
            let _ = child.wait();
            break Err(if Instant::now() >= deadline {
                JobFailureKind::Timeout
            } else {
                JobFailureKind::ProcessIo
            });
        }
        match child.try_wait() {
            Ok(Some(status)) => break Ok(status),
            Ok(None) => thread::sleep(Duration::from_millis(20)),
            Err(_) => {
                let _ = child.kill();
                let _ = child.wait();
                break Err(JobFailureKind::ProcessIo);
            }
        }
    };
    let output = out.join().map_err(|_| JobFailureKind::ProcessIo)?;
    let diagnostics = err.join().map_err(|_| JobFailureKind::ProcessIo)?;
    let written = writer.join().map_err(|_| JobFailureKind::ProcessIo)?;
    let status = status?;
    written?;
    if !status.success() {
        let diagnostic = diagnostics
            .as_ref()
            .ok()
            .map(|bytes| String::from_utf8_lossy(bytes).to_ascii_lowercase())
            .unwrap_or_default();
        let category = if ["eacces", "eperm", "permission denied", "access is denied"]
            .iter()
            .any(|text| diagnostic.contains(text))
        {
            "permission"
        } else if [
            "credential",
            "api_key",
            "unauthorized",
            "login",
            "authentication",
        ]
        .iter()
        .any(|text| diagnostic.contains(text))
        {
            "authentication"
        } else if diagnostic.contains("profile") {
            "profile"
        } else {
            "other"
        };
        eprintln!(
            "deepseek_harness=failed exit_code={:?} stdout_bytes={} stderr_bytes={} category={category}",
            status.code(),
            output.as_ref().map_or(0, Vec::len),
            diagnostics.as_ref().map_or(0, Vec::len)
        );
        return Err(JobFailureKind::ExitFailure);
    }
    diagnostics?;
    parse_final(&output?)
}

fn parse_final(output: &[u8]) -> Result<String, JobFailureKind> {
    let mut final_text = None;
    for line in output
        .split(|byte| *byte == b'\n')
        .filter(|line| !line.iter().all(u8::is_ascii_whitespace))
    {
        if line.len() > 256 * 1024 {
            return Err(JobFailureKind::OutputTooLarge);
        }
        let event: serde_json::Value =
            serde_json::from_slice(line).map_err(|_| JobFailureKind::InvalidOutput)?;
        match event.get("type").and_then(|value| value.as_str()) {
            Some("error") => return Err(JobFailureKind::ExitFailure),
            Some("final") => {
                if final_text.is_some() {
                    return Err(JobFailureKind::InvalidOutput);
                }
                let text = event
                    .get("text")
                    .and_then(|value| value.as_str())
                    .filter(|text| !text.trim().is_empty())
                    .ok_or(JobFailureKind::InvalidOutput)?;
                final_text = Some(text.to_owned());
            }
            _ => {}
        }
    }
    final_text.ok_or(JobFailureKind::InvalidOutput)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_a_single_nonempty_final_is_a_completion() {
        assert_eq!(
            parse_final(br#"{"type":"final","text":"actual answer"}"#).unwrap(),
            "actual answer"
        );
        assert!(parse_final(br#"{"type":"text","text":"partial"}"#).is_err());
        assert!(parse_final(br#"{"type":"final","text":""}"#).is_err());
        assert!(parse_final(b"{\"type\":\"final\",\"text\":\"x\"}\n{\"type\":\"error\"}").is_err());
        assert!(
            parse_final(
                b"{\"type\":\"final\",\"text\":\"x\"}\n{\"type\":\"final\",\"text\":\"y\"}"
            )
            .is_err()
        );
    }

    #[cfg(windows)]
    fn powershell(body: &str) -> Command {
        let mut command = Command::new("powershell.exe");
        command.args(["-NoProfile", "-NonInteractive", "-Command", body]);
        command
    }

    #[cfg(windows)]
    #[test]
    fn stdin_eof_and_concurrent_stderr_drain_are_required() {
        let command = powershell(
            "$p=[Console]::In.ReadToEnd(); [Console]::Error.Write(('x'*100000)); if ($p -ne 'hello & %PATH%') { exit 2 }; [Console]::Out.WriteLine('{\"type\":\"final\",\"text\":\"answer\"}')",
        );
        assert_eq!(
            drive(
                command,
                "hello & %PATH%",
                &AtomicBool::new(false),
                Duration::from_secs(10)
            )
            .unwrap(),
            "answer"
        );
    }

    #[cfg(windows)]
    #[test]
    fn nonzero_exit_and_timeout_never_complete() {
        let command = powershell(
            "[Console]::Out.WriteLine('{\"type\":\"final\",\"text\":\"answer\"}'); exit 2",
        );
        assert_eq!(
            drive(
                command,
                "",
                &AtomicBool::new(false),
                Duration::from_secs(10)
            ),
            Err(JobFailureKind::ExitFailure)
        );
        assert_eq!(
            drive(
                powershell("Start-Sleep 20"),
                &"blocked input".repeat(10000),
                &AtomicBool::new(false),
                Duration::from_millis(200)
            ),
            Err(JobFailureKind::Timeout)
        );
        assert_eq!(
            drive(
                powershell("Start-Sleep 20"),
                "",
                &AtomicBool::new(true),
                Duration::from_secs(10)
            ),
            Err(JobFailureKind::ProcessIo)
        );
    }
}

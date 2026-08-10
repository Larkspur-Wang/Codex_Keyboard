use std::collections::BTreeMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::thread::{self, JoinHandle};

use thiserror::Error;

use crate::bindings::{BindingError, BindingService};
use crate::codex_runner::CodexRunner;
use crate::store::{
    EnqueueOutcome, Job, JobFailureKind, MAX_GLOBAL_RUNNING_JOBS, NewJob, StateStore, StoreError,
};

#[derive(Debug, Error)]
pub enum PromptQueueError {
    #[error("slot has no current binding")]
    UnboundSlot,
    #[error("binding lookup failed: {0}")]
    Binding(#[from] BindingError),
    #[error("Host state failed")]
    Store(#[from] StoreError),
}

pub struct PromptQueueService<'a> {
    bindings: BindingService<'a>,
}

impl<'a> PromptQueueService<'a> {
    pub fn new(bindings: BindingService<'a>) -> Self {
        Self { bindings }
    }

    pub fn enqueue_slot(
        &self,
        store: &mut StateStore,
        slot: u8,
        request_id: &str,
        prompt: &str,
    ) -> Result<EnqueueOutcome, PromptQueueError> {
        let Some((binding, task)) = self.bindings.resolve(store, slot)? else {
            return Err(PromptQueueError::UnboundSlot);
        };
        Ok(store.enqueue(&NewJob {
            request_id,
            task_id: &binding.task_id,
            slot,
            generation: binding.generation,
            prompt,
            cwd: &task.cwd,
        })?)
    }
}

struct WorkerResult {
    request_id: String,
    claim_generation: u64,
    result: Result<(), JobFailureKind>,
}

struct Worker {
    cancel: Arc<AtomicBool>,
    handle: JoinHandle<()>,
}

pub struct DurablePromptScheduler {
    runner: CodexRunner,
    sender: Sender<WorkerResult>,
    receiver: Receiver<WorkerResult>,
    workers: BTreeMap<String, Worker>,
}

impl DurablePromptScheduler {
    pub fn new(runner: CodexRunner) -> Self {
        let (sender, receiver) = mpsc::channel();
        Self {
            runner,
            sender,
            receiver,
            workers: BTreeMap::new(),
        }
    }

    pub fn tick(&mut self, store: &mut StateStore) -> Result<(), StoreError> {
        while let Ok(outcome) = self.receiver.try_recv() {
            if let Some(worker) = self.workers.remove(&outcome.request_id) {
                let _ = worker.handle.join();
            }
            match outcome.result {
                Ok(()) => {
                    store.mark_completed(&outcome.request_id, outcome.claim_generation)?;
                }
                Err(failure) => {
                    store.mark_failed(&outcome.request_id, outcome.claim_generation, failure)?;
                }
            }
        }

        while self.workers.len() < MAX_GLOBAL_RUNNING_JOBS as usize {
            let Some(job) = store.claim_next_runnable()? else {
                break;
            };
            self.start(job);
        }
        Ok(())
    }

    pub fn in_flight(&self) -> usize {
        self.workers.len()
    }

    pub fn shutdown_without_acknowledging(&mut self) {
        for worker in self.workers.values() {
            worker.cancel.store(true, Ordering::Release);
        }
        for (_, worker) in std::mem::take(&mut self.workers) {
            let _ = worker.handle.join();
        }
        while self.receiver.try_recv().is_ok() {}
    }

    fn start(&mut self, job: Job) {
        let request_id = job.request_id.clone();
        let result_id = request_id.clone();
        let claim_generation = job.claim_generation;
        let runner = self.runner.clone();
        let sender = self.sender.clone();
        let cancel = Arc::new(AtomicBool::new(false));
        let worker_cancel = Arc::clone(&cancel);
        let handle = thread::spawn(move || {
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                runner.run_with_cancel(&job, &worker_cancel)
            }))
            .unwrap_or(Err(JobFailureKind::ProcessIo));
            let _ = sender.send(WorkerResult {
                request_id: result_id,
                claim_generation,
                result,
            });
        });
        self.workers.insert(request_id, Worker { cancel, handle });
    }
}

impl Drop for DurablePromptScheduler {
    fn drop(&mut self) {
        self.shutdown_without_acknowledging();
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::os::unix::fs::PermissionsExt;
    use std::path::{Path, PathBuf};
    use std::time::{Duration, Instant};

    use tempfile::tempdir;

    use super::*;
    use crate::codex_runner::CodexRunnerConfig;

    fn write_script(directory: &Path, body: &str) -> PathBuf {
        let path = directory.join("fake-codex");
        fs::write(&path, format!("#!/bin/sh\nset -eu\n{body}\n")).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
        path
    }

    fn runner(executable: PathBuf) -> CodexRunner {
        CodexRunner::new(CodexRunnerConfig {
            executable,
            supervisor_executable: None,
            timeout: Duration::from_secs(3),
            max_stdout_line_bytes: 1024,
            max_stdout_total_bytes: 4096,
            max_stderr_bytes: 1024,
        })
    }

    fn enqueue(store: &mut StateStore, request: &str, task: &str, prompt: &str, cwd: &Path) {
        store
            .enqueue(&NewJob {
                request_id: request,
                task_id: task,
                slot: 1,
                generation: 1,
                prompt,
                cwd,
            })
            .unwrap();
    }

    fn drive_until_idle(
        scheduler: &mut DurablePromptScheduler,
        store: &mut StateStore,
        tasks: &[&str],
    ) {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            scheduler.tick(store).unwrap();
            let pending = tasks
                .iter()
                .map(|task| store.pending_count(task).unwrap())
                .sum::<u32>();
            if pending == 0 && scheduler.in_flight() == 0 {
                break;
            }
            assert!(Instant::now() < deadline, "scheduler did not become idle");
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    #[test]
    fn four_different_tasks_run_concurrently() {
        let temp = tempdir().unwrap();
        let first = temp.path().join("first.started");
        let second = temp.path().join("second.started");
        let third = temp.path().join("third.started");
        let fourth = temp.path().join("fourth.started");
        let order = temp.path().join("order");
        let executable = write_script(
            temp.path(),
            &format!(
                "p=$(cat); touch '{}/'$p.started; i=0; while [ ! -e '{}' ] || [ ! -e '{}' ] || [ ! -e '{}' ] || [ ! -e '{}' ]; do i=$((i+1)); [ $i -lt 200 ] || exit 7; sleep 0.01; done; printf '%s\\n' \"$p\" >> '{}'; printf '{{}}\\n'",
                temp.path().display(),
                first.display(),
                second.display(),
                third.display(),
                fourth.display(),
                order.display()
            ),
        );
        let mut scheduler = DurablePromptScheduler::new(runner(executable));
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        enqueue(&mut store, "r1", "task-a", "first", temp.path());
        enqueue(&mut store, "r2", "task-b", "second", temp.path());
        enqueue(&mut store, "r3", "task-c", "third", temp.path());
        enqueue(&mut store, "r4", "task-d", "fourth", temp.path());

        scheduler.tick(&mut store).unwrap();
        assert_eq!(scheduler.in_flight(), 4);
        drive_until_idle(
            &mut scheduler,
            &mut store,
            &["task-a", "task-b", "task-c", "task-d"],
        );
        let lines = fs::read_to_string(order).unwrap();
        assert!(lines.contains("first\n"));
        assert!(lines.contains("second\n"));
        assert!(lines.contains("third\n"));
        assert!(lines.contains("fourth\n"));
    }

    #[test]
    fn scheduler_never_overlaps_two_jobs_for_one_task() {
        let temp = tempdir().unwrap();
        let active = temp.path().join("active");
        let order = temp.path().join("order");
        let executable = write_script(
            temp.path(),
            &format!(
                "p=$(cat); [ ! -e '{}' ] || exit 9; touch '{}'; printf '%s\\n' \"$p\" >> '{}'; sleep 0.05; rm '{}'; printf '{{}}\\n'",
                active.display(),
                active.display(),
                order.display(),
                active.display()
            ),
        );
        let mut scheduler = DurablePromptScheduler::new(runner(executable));
        let mut store = StateStore::open(&temp.path().join("state.sqlite3")).unwrap();
        enqueue(&mut store, "r1", "task-a", "first", temp.path());
        enqueue(&mut store, "r2", "task-a", "second", temp.path());

        scheduler.tick(&mut store).unwrap();
        assert_eq!(scheduler.in_flight(), 1);
        drive_until_idle(&mut scheduler, &mut store, &["task-a"]);
        assert_eq!(fs::read_to_string(order).unwrap(), "first\nsecond\n");
    }
}

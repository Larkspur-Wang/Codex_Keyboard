#[cfg(target_os = "macos")]
mod macos_gate {
    use std::cell::RefCell;
    use std::collections::{BTreeMap, BTreeSet};
    use std::io::Write;
    use std::path::{Path, PathBuf};
    use std::time::Instant;

    use easy_codex_host::cache::{CacheId, CacheLimits, CacheStore};
    use easy_codex_host::dashscope::{DashScopeTtsClient, TTS_MODEL, TtsAudio};
    use easy_codex_host::paths::AppPaths;
    use easy_codex_host::secrets::{
        DashScopeEnvStore, ImportLock, KeychainAccounts, SecretBytes, SecretStore,
        SecretStoreError, dashscope_key_is_installed,
    };
    use easy_codex_host::spark_runner::{SparkRunner, SparkRunnerConfig};
    use easy_codex_host::store::{RolloutCursor, StateStore};
    use easy_codex_host::summary_orchestrator::{
        DashScopeSummarySynthesizer, SUMMARY_TTS_INSTRUCTIONS, SUMMARY_TTS_VOICE,
        SummaryOrchestrator, SummaryRunOutcome, SummarySynthesizer,
    };
    use easy_codex_host::tts_cache::load_tts_summary;
    use serde::Serialize;
    use zeroize::Zeroizing;

    const TASK: &str = "00000000-0000-4000-8000-000000000041";
    const COMPLETION: &str = "00000000-0000-4000-8000-000000000042";
    const REAL_TTS_CALLS: usize = 20;

    #[derive(Serialize)]
    struct CallMetric {
        index: usize,
        latency_ms: u128,
        samples: u64,
        billed_characters: Option<u64>,
        attempts: u8,
        transport: String,
        served_model: String,
    }

    #[derive(Default)]
    struct EphemeralSecretStore(RefCell<BTreeMap<String, Zeroizing<Vec<u8>>>>);

    impl SecretStore for EphemeralSecretStore {
        fn get(&self, account: &str) -> Result<Option<SecretBytes>, SecretStoreError> {
            Ok(self
                .0
                .borrow()
                .get(account)
                .map(|value| SecretBytes::new(value.as_slice().to_vec())))
        }

        fn create(&self, account: &str, secret: &[u8]) -> Result<(), SecretStoreError> {
            let mut values = self.0.borrow_mut();
            if values.contains_key(account) {
                return Err(SecretStoreError::AlreadyExists);
            }
            values.insert(account.to_owned(), Zeroizing::new(secret.to_vec()));
            Ok(())
        }

        fn delete(&self, account: &str) -> Result<(), SecretStoreError> {
            self.0.borrow_mut().remove(account);
            Ok(())
        }
    }

    struct RecordingSynthesizer<T> {
        inner: T,
        metrics: RefCell<Vec<CallMetric>>,
    }

    impl<T: SummarySynthesizer> SummarySynthesizer for RecordingSynthesizer<T> {
        fn synthesize(
            &self,
            text: &str,
            voice: &str,
            instructions: &str,
        ) -> Result<TtsAudio, easy_codex_host::dashscope::TtsError> {
            let started = Instant::now();
            let audio = self.inner.synthesize(text, voice, instructions)?;
            let index = self.metrics.borrow().len() + 1;
            self.metrics
                .borrow_mut()
                .push(metric(index, started.elapsed().as_millis(), &audio));
            Ok(audio)
        }
    }

    pub fn run() -> Result<(), Box<dyn std::error::Error>> {
        if std::env::args_os().nth(1).is_some() {
            return Err("m4_live_gate accepts no arguments".into());
        }
        let home = std::env::var_os("HOME").ok_or("HOME is unavailable")?;
        let product_paths = AppPaths::from_home(Path::new(&home));
        product_paths.prepare()?;
        let product_lock =
            ImportLock::acquire(&product_paths.runtime_directory.join("key-import.lock"))?;
        let product_accounts =
            KeychainAccounts::load_or_create(&product_paths.installation_id, &product_lock)?;
        let dashscope_env =
            DashScopeEnvStore::new(product_paths.dashscope_env.clone(), &product_accounts);
        if !dashscope_key_is_installed(&dashscope_env, &product_accounts)? {
            return Err("private DashScope .env is unavailable".into());
        }

        let isolated_root = tempfile::Builder::new()
            .prefix("m4d-live-")
            .tempdir_in(&product_paths.runtime_directory)?;
        let isolated_paths = AppPaths::from_root(isolated_root.path().join("host"));
        isolated_paths.prepare()?;
        let isolated_lock =
            ImportLock::acquire(&isolated_paths.runtime_directory.join("key-import.lock"))?;
        let isolated_accounts =
            KeychainAccounts::load_or_create(&isolated_paths.installation_id, &isolated_lock)?;
        let ephemeral_secrets = EphemeralSecretStore::default();
        let cache = CacheStore::initialize(
            &isolated_paths.cache_directory,
            &ephemeral_secrets,
            &isolated_accounts,
            CacheLimits::default(),
        )?;
        let mut state = StateStore::open(&isolated_paths.state_database)?;
        let cursor = RolloutCursor {
            task_id: TASK.into(),
            rollout_path: isolated_root.path().join("fixture-rollout.jsonl"),
            device: 1,
            inode: 1,
            offset: 1,
            generation: 1,
            anchor: [41; 32],
        };
        state.commit_rollout_completion(
            None,
            &cursor,
            COMPLETION,
            r#"{"user":["Summarize the completed integration work."],"assistant":["The transaction and recovery checks passed."],"tools":[{"name":"eval","status":"passed"}]}"#,
        )?;

        // Keep the synthetic ledger/cache isolated, but exercise Spark through the
        // same private runtime root used by the product and the standalone live gate.
        let spark = SparkRunner::new(SparkRunnerConfig {
            supervisor_executable: None,
            ..SparkRunnerConfig::default()
        });
        let tts_client = DashScopeTtsClient::default();
        let tts = RecordingSynthesizer {
            inner: DashScopeSummarySynthesizer {
                client: &tts_client,
                secrets: &dashscope_env,
                accounts: &product_accounts,
            },
            metrics: RefCell::new(Vec::with_capacity(REAL_TTS_CALLS)),
        };
        let started = Instant::now();
        let first = SummaryOrchestrator::new(&mut state, &cache, &spark, &tts)
            .run(TASK, "m4d-real-composed-request")?;
        let SummaryRunOutcome::Published { unread, audio, .. } = first else {
            return Err("composed live generation did not publish".into());
        };
        if unread.coverage_count != 1 || state.pending_summary_completion_count(TASK)? != 0 {
            return Err("composed live generation did not commit exact coverage".into());
        }
        let composed_latency_ms = started.elapsed().as_millis();
        let summary = load_tts_summary(&cache, TASK, unread.generation)?;
        let cached = cache.read(&CacheId::for_task(TASK, unread.generation)?)?;
        let preview = write_preview(&product_paths.runtime_directory, cached.qwen_wav.as_slice())?;

        if audio.samples == 0 {
            return Err("composed live generation returned empty audio".into());
        }
        for _ in 2..=REAL_TTS_CALLS {
            tts.synthesize(
                &summary.spoken_text,
                SUMMARY_TTS_VOICE,
                SUMMARY_TTS_INSTRUCTIONS,
            )?;
        }
        let metrics = tts.metrics.into_inner();
        if metrics.len() != REAL_TTS_CALLS {
            return Err("live TTS call count changed".into());
        }
        let models = metrics
            .iter()
            .map(|metric| metric.served_model.as_str())
            .collect::<BTreeSet<_>>();
        let minimum = metrics
            .iter()
            .map(|metric| metric.latency_ms)
            .min()
            .unwrap_or(0);
        let maximum = metrics
            .iter()
            .map(|metric| metric.latency_ms)
            .max()
            .unwrap_or(0);
        let total: u128 = metrics.iter().map(|metric| metric.latency_ms).sum();
        println!(
            "{}",
            serde_json::json!({
                "status": "pass",
                "calls": REAL_TTS_CALLS,
                "region": "cn-beijing",
                "requested_model": TTS_MODEL,
                "stable_model": TTS_MODEL,
                "served_models": models,
                "voice": SUMMARY_TTS_VOICE,
                "composed_latency_ms": composed_latency_ms,
                "latency_ms_min": minimum,
                "latency_ms_mean": total / REAL_TTS_CALLS as u128,
                "latency_ms_max": maximum,
                "preview_file": preview.file_name().and_then(|name| name.to_str()),
                "summary_text_emitted": false,
                "audio_emitted_to_stdout": false,
                "credential_emitted": false,
                "metrics": metrics,
            })
        );
        Ok(())
    }

    fn metric(index: usize, latency_ms: u128, audio: &TtsAudio) -> CallMetric {
        CallMetric {
            index,
            latency_ms,
            samples: audio.receipt().samples,
            billed_characters: audio.receipt().characters,
            attempts: audio.receipt().attempts,
            transport: audio.receipt().transport.to_owned(),
            served_model: audio.receipt().model.to_owned(),
        }
    }

    fn write_preview(directory: &Path, wav: &[u8]) -> std::io::Result<PathBuf> {
        let mut preview = tempfile::Builder::new()
            .prefix("m4d-cherry-preview-")
            .suffix(".wav")
            .tempfile_in(directory)?;
        preview.write_all(wav)?;
        preview.as_file().sync_all()?;
        let (_, path) = preview.keep().map_err(|error| error.error)?;
        Ok(path)
    }
}

#[cfg(target_os = "macos")]
fn main() -> Result<(), Box<dyn std::error::Error>> {
    macos_gate::run()
}

#[cfg(not(target_os = "macos"))]
fn main() -> Result<(), Box<dyn std::error::Error>> {
    Err("m4_live_gate requires the macOS Host runtime".into())
}

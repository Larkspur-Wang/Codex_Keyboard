use thiserror::Error;

use crate::cache::{CacheError, CacheStore};
use crate::dashscope::{DashScopeTtsClient, TtsAudio, TtsError, TtsRequest};
use crate::rollout_observer::TurnPack;
use crate::secrets::{KeychainAccounts, SecretStore};
use crate::spark_runner::{SparkError, SparkRunner};
use crate::store::{
    StateStore, StoreError, SummaryClaim, SummaryClaimResult, SummaryTtsAttemptState, UnreadSummary,
};
use crate::summary::{
    SummaryDocument, SummaryDocumentError, contains_han_text, incorporates_new_completion,
    meaningful_for_speech, preserves_previous_unheard, required_source_evidence_quote,
    source_evidence_quote_budget,
};
use crate::tts_cache::{
    PublishedTtsGeneration, TtsCacheError, load_tts_generation_with, load_tts_summary,
    publish_tts_generation_with,
};

pub const SUMMARY_TTS_VOICE: &str = "Cherry";
pub const SUMMARY_TTS_INSTRUCTIONS: &str = "请使用自然、清晰、沉稳的普通话女声，语速适中，像可信赖的工作助理播报任务摘要；避免夸张情绪、撒娇和广告腔。";

pub trait SummaryGenerator {
    fn generate(
        &self,
        claim: &SummaryClaim,
        previous_unheard: Option<&SummaryDocument>,
    ) -> Result<SummaryDocument, SparkError>;
}

impl SummaryGenerator for SparkRunner {
    fn generate(
        &self,
        claim: &SummaryClaim,
        previous_unheard: Option<&SummaryDocument>,
    ) -> Result<SummaryDocument, SparkError> {
        self.run(claim, previous_unheard)
    }
}

pub trait SummarySynthesizer {
    fn synthesize(&self, text: &str, voice: &str, instructions: &str)
    -> Result<TtsAudio, TtsError>;
}

pub struct DashScopeSummarySynthesizer<'a, S> {
    pub client: &'a DashScopeTtsClient,
    pub secrets: &'a S,
    pub accounts: &'a KeychainAccounts,
}

impl<S: SecretStore> SummarySynthesizer for DashScopeSummarySynthesizer<'_, S> {
    fn synthesize(
        &self,
        text: &str,
        voice: &str,
        instructions: &str,
    ) -> Result<TtsAudio, TtsError> {
        self.client.synthesize_chunked(
            self.secrets,
            self.accounts,
            TtsRequest {
                text,
                voice,
                instructions,
            },
        )
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SummaryCheckpoint {
    ClaimAcquired,
    PreviousUnreadLoaded,
    SparkGenerated,
    TtsAttemptStarted,
    TtsGenerated,
    CacheAuthenticated,
    LedgerPublished,
}

pub trait SummaryCheckpointObserver {
    fn reached(&mut self, checkpoint: SummaryCheckpoint);
}

struct NoopObserver;

impl SummaryCheckpointObserver for NoopObserver {
    fn reached(&mut self, _checkpoint: SummaryCheckpoint) {}
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SummaryRunOutcome {
    Idle,
    AlreadyPublished {
        generation: u64,
    },
    Published {
        unread: UnreadSummary,
        audio: PublishedTtsGeneration,
        recovered_from_cache: bool,
    },
    ManualTtsReconciliationRequired {
        generation: u64,
    },
}

#[derive(Debug, Error)]
pub enum SummaryOrchestratorError {
    #[error(transparent)]
    Store(#[from] StoreError),
    #[error(transparent)]
    Summary(#[from] SummaryDocumentError),
    #[error("previous unread cache could not be authenticated")]
    PreviousCache(#[source] TtsCacheError),
    #[error(transparent)]
    Spark(#[from] SparkError),
    #[error(transparent)]
    Tts(#[from] TtsError),
    #[error(transparent)]
    Cache(#[from] TtsCacheError),
}

pub struct SummaryOrchestrator<'a, G, T> {
    store: &'a mut StateStore,
    cache: &'a CacheStore,
    generator: &'a G,
    synthesizer: &'a T,
}

impl<'a, G, T> SummaryOrchestrator<'a, G, T>
where
    G: SummaryGenerator,
    T: SummarySynthesizer,
{
    pub fn new(
        store: &'a mut StateStore,
        cache: &'a CacheStore,
        generator: &'a G,
        synthesizer: &'a T,
    ) -> Self {
        Self {
            store,
            cache,
            generator,
            synthesizer,
        }
    }

    pub fn run(
        &mut self,
        task_id: &str,
        request_id: &str,
    ) -> Result<SummaryRunOutcome, SummaryOrchestratorError> {
        self.run_with_observer(task_id, request_id, &mut NoopObserver)
    }

    pub fn resume(&mut self, task_id: &str) -> Result<SummaryRunOutcome, SummaryOrchestratorError> {
        let Some(claim) = self.store.resume_interrupted_summary(task_id)? else {
            return Ok(SummaryRunOutcome::Idle);
        };
        self.execute_claim(claim, &mut NoopObserver, true)
    }

    pub fn run_with_observer<O: SummaryCheckpointObserver>(
        &mut self,
        task_id: &str,
        request_id: &str,
        observer: &mut O,
    ) -> Result<SummaryRunOutcome, SummaryOrchestratorError> {
        let Some(claim) = self.store.claim_summary(task_id, request_id)? else {
            return Ok(SummaryRunOutcome::Idle);
        };
        match claim {
            SummaryClaimResult::Published { generation, .. } => {
                Ok(SummaryRunOutcome::AlreadyPublished { generation })
            }
            SummaryClaimResult::Claimed(claim) => self.execute_claim(claim, observer, false),
        }
    }

    fn execute_claim<O: SummaryCheckpointObserver>(
        &mut self,
        claim: SummaryClaim,
        observer: &mut O,
        resumed_after_process_restart: bool,
    ) -> Result<SummaryRunOutcome, SummaryOrchestratorError> {
        observer.reached(SummaryCheckpoint::ClaimAcquired);

        match self.recover_authenticated_cache(&claim, observer) {
            Ok(Some(outcome)) => return Ok(outcome),
            Ok(None) => {}
            Err(error) => return Err(error),
        }

        if let Some(attempt) = self.store.summary_tts_attempt(&claim)? {
            self.abandon(&claim)?;
            eprintln!(
                "summary=abandoned_unpublished_tts_attempt state={attempt:?} generation={} resumed={}",
                claim.generation, resumed_after_process_restart
            );
            return Ok(SummaryRunOutcome::Idle);
        }

        let previous = match &claim.previous_unread {
            Some(previous) => {
                match load_tts_summary(self.cache, &claim.task_id, previous.generation) {
                    Ok(summary) => Some(summary),
                    Err(error) => {
                        self.abandon(&claim)?;
                        return Err(SummaryOrchestratorError::PreviousCache(error));
                    }
                }
            }
            None => None,
        };
        observer.reached(SummaryCheckpoint::PreviousUnreadLoaded);

        let mut summary = match self.generator.generate(&claim, previous.as_ref()) {
            Ok(summary) => summary,
            Err(error) => {
                self.abandon(&claim)?;
                return Err(error.into());
            }
        };
        let expected = claim
            .completions
            .iter()
            .map(|completion| completion.completion_id.clone())
            .collect::<Vec<_>>();
        if let Err(error) = summary.validate_expected_covers(&expected) {
            self.abandon(&claim)?;
            return Err(error.into());
        }
        if !contains_han_text(&summary.spoken_text)
            || !meaningful_for_speech(&summary)
            || !preserves_previous_unheard(previous.as_ref(), &summary)
            || !incorporates_new_completion(previous.as_ref(), &summary)
            || validate_claim_source_evidence(&claim, &summary).is_err()
        {
            eprintln!(
                "summary=spoken_text_quality_retry generation={}",
                claim.generation
            );
            summary = match self.generator.generate(&claim, previous.as_ref()) {
                Ok(summary) => summary,
                Err(error) => {
                    self.abandon(&claim)?;
                    return Err(error.into());
                }
            };
            if let Err(error) = summary.validate_expected_covers(&expected) {
                self.abandon(&claim)?;
                return Err(error.into());
            }
            if !contains_han_text(&summary.spoken_text)
                || !meaningful_for_speech(&summary)
                || !preserves_previous_unheard(previous.as_ref(), &summary)
                || !incorporates_new_completion(previous.as_ref(), &summary)
                || validate_claim_source_evidence(&claim, &summary).is_err()
            {
                self.abandon(&claim)?;
                return Err(SummaryDocumentError::Text.into());
            }
        }
        observer.reached(SummaryCheckpoint::SparkGenerated);

        if self.store.begin_summary_tts_attempt(&claim)? != SummaryTtsAttemptState::Started {
            self.abandon(&claim)?;
            eprintln!(
                "summary=abandoned_unpublished_tts_attempt state=attempt_changed generation={}",
                claim.generation
            );
            return Ok(SummaryRunOutcome::Idle);
        }
        observer.reached(SummaryCheckpoint::TtsAttemptStarted);

        eprintln!(
            "summary=tts_start generation={} characters={}",
            claim.generation,
            summary.spoken_text.chars().count()
        );
        let tts = match self.synthesizer.synthesize(
            &summary.spoken_text,
            SUMMARY_TTS_VOICE,
            SUMMARY_TTS_INSTRUCTIONS,
        ) {
            Ok(tts) => tts,
            Err(TtsError::AmbiguousAfterCommit) => {
                self.abandon(&claim)?;
                eprintln!(
                    "summary=abandoned_unpublished_tts_attempt state=tts_ambiguous generation={}",
                    claim.generation
                );
                return Ok(SummaryRunOutcome::Idle);
            }
            Err(error) => {
                self.abandon(&claim)?;
                return Err(error.into());
            }
        };
        if tts.receipt().voice != SUMMARY_TTS_VOICE {
            self.abandon(&claim)?;
            eprintln!(
                "summary=abandoned_unpublished_tts_attempt state=voice_mismatch generation={}",
                claim.generation
            );
            return Ok(SummaryRunOutcome::Idle);
        }
        observer.reached(SummaryCheckpoint::TtsGenerated);

        let committed = publish_tts_generation_with(
            self.cache,
            &claim.task_id,
            claim.generation,
            summary,
            &tts,
            |audio| {
                observer.reached(SummaryCheckpoint::CacheAuthenticated);
                self.store
                    .publish_summary(&claim, &audio.cache_reference)
                    .map(|unread| (unread, audio))
            },
        );
        let (unread, audio) = match committed {
            Ok(Ok(published)) => published,
            Ok(Err(error)) => return Err(error.into()),
            Err(_) => match self.recover_authenticated_cache(&claim, observer) {
                Ok(Some(outcome)) => return Ok(outcome),
                Ok(None) => {
                    self.abandon(&claim)?;
                    eprintln!(
                        "summary=abandoned_unpublished_tts_attempt state=cache_commit generation={}",
                        claim.generation
                    );
                    return Ok(SummaryRunOutcome::Idle);
                }
                Err(error) => {
                    self.abandon(&claim)?;
                    return Err(error);
                }
            },
        };
        observer.reached(SummaryCheckpoint::LedgerPublished);
        Ok(SummaryRunOutcome::Published {
            unread,
            audio,
            recovered_from_cache: false,
        })
    }

    fn recover_authenticated_cache<O: SummaryCheckpointObserver>(
        &mut self,
        claim: &SummaryClaim,
        observer: &mut O,
    ) -> Result<Option<SummaryRunOutcome>, SummaryOrchestratorError> {
        let expected = claim
            .completions
            .iter()
            .map(|completion| completion.completion_id.clone())
            .collect::<Vec<_>>();
        let loaded =
            load_tts_generation_with(self.cache, &claim.task_id, claim.generation, |cached| {
                cached
                    .summary
                    .validate_expected_covers(&expected)
                    .map_err(RecoveryCommitError::Summary)?;
                validate_claim_source_evidence(claim, &cached.summary)
                    .map_err(RecoveryCommitError::Summary)?;
                observer.reached(SummaryCheckpoint::CacheAuthenticated);
                self.store
                    .publish_summary(claim, &cached.publication.cache_reference)
                    .map(|unread| (unread, cached.publication))
                    .map_err(RecoveryCommitError::Store)
            });
        match loaded {
            Ok(Ok((unread, audio))) => {
                observer.reached(SummaryCheckpoint::LedgerPublished);
                Ok(Some(SummaryRunOutcome::Published {
                    unread,
                    audio,
                    recovered_from_cache: true,
                }))
            }
            Ok(Err(RecoveryCommitError::Store(error))) => Err(error.into()),
            Ok(Err(RecoveryCommitError::Summary(error))) => Err(error.into()),
            Err(TtsCacheError::Cache(CacheError::MissingObject)) => Ok(None),
            Err(error) => Err(error.into()),
        }
    }

    fn abandon(&mut self, claim: &SummaryClaim) -> Result<(), StoreError> {
        if self.store.abandon_summary_claim(claim)? {
            Ok(())
        } else {
            Err(StoreError::SummaryClaimChanged)
        }
    }
}

fn validate_claim_source_evidence(
    claim: &SummaryClaim,
    summary: &SummaryDocument,
) -> Result<(), SummaryDocumentError> {
    let quote_budget = source_evidence_quote_budget(claim.completions.len());
    let sources = claim
        .completions
        .iter()
        .map(|completion| {
            let pack: TurnPack = serde_json::from_str(&completion.turn_pack)
                .map_err(|_| SummaryDocumentError::Evidence)?;
            if pack.v != 1 || pack.turn_id != completion.completion_id {
                return Err(SummaryDocumentError::Evidence);
            }
            if pack.assistant.len() != 1 || pack.assistant[0].trim().is_empty() {
                return Err(SummaryDocumentError::Evidence);
            }
            let assistant_final = &pack.assistant[0];
            let required_quote = required_source_evidence_quote(assistant_final, quote_budget)
                .ok_or(SummaryDocumentError::Evidence)?;
            Ok((
                completion.completion_id.clone(),
                assistant_final.clone(),
                required_quote,
            ))
        })
        .collect::<Result<Vec<_>, _>>()?;
    let source_refs = sources
        .iter()
        .map(|(completion_id, assistant_final, required_quote)| {
            (
                completion_id.as_str(),
                assistant_final.as_str(),
                required_quote.as_str(),
            )
        })
        .collect::<Vec<_>>();
    summary.validate_source_evidence(&source_refs)
}

enum RecoveryCommitError {
    Store(StoreError),
    Summary(SummaryDocumentError),
}

#[cfg(test)]
mod tests {
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::{Arc, Mutex};

    use super::*;
    use crate::audio::TTS_SAMPLE_RATE;
    use crate::cache::{CacheId, CacheLimits};
    use crate::dashscope::{TTS_MODEL, TtsReceipt};
    use crate::store::{RolloutCursor, SummaryClaimOutcome};
    use crate::summary::{SUMMARY_SCHEMA_VERSION, SummarySourceEvidence};

    const TASK: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";
    const FIRST: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
    const SECOND: &str = "019fa972-5cfa-75e1-9008-0b17ade9a349";
    const GENERIC_SOURCE_QUOTE: &str = "助手最终回复已经完成目标功能";
    const SOURCE_QUOTE: &str = "固件已把四个槽位映射到左起四颗灯并保持第五颗灯熄灭";

    fn source_evidence(claim: &SummaryClaim) -> Vec<SummarySourceEvidence> {
        claim
            .completions
            .iter()
            .map(|completion| SummarySourceEvidence {
                completion_id: completion.completion_id.clone(),
                exact_quote: SOURCE_QUOTE.into(),
            })
            .collect()
    }

    struct FakeGenerator {
        calls: Arc<AtomicUsize>,
        previous_facts: Arc<Mutex<Vec<usize>>>,
    }

    struct LanguageRepairGenerator {
        calls: Arc<AtomicUsize>,
        repair_succeeds: bool,
    }

    struct DropsPreviousGenerator {
        calls: Arc<AtomicUsize>,
    }

    struct GenericWithoutEvidenceGenerator {
        calls: Arc<AtomicUsize>,
    }

    struct GenericWithWeakEvidenceGenerator {
        calls: Arc<AtomicUsize>,
    }

    struct ReplaysPreviousGenerator {
        calls: Arc<AtomicUsize>,
    }

    impl SummaryGenerator for FakeGenerator {
        fn generate(
            &self,
            claim: &SummaryClaim,
            previous_unheard: Option<&SummaryDocument>,
        ) -> Result<SummaryDocument, SparkError> {
            self.calls.fetch_add(1, Ordering::SeqCst);
            self.previous_facts
                .lock()
                .unwrap()
                .push(previous_unheard.map_or(0, |document| document.facts.len()));
            let mut facts = previous_unheard
                .map(|document| document.facts.clone())
                .unwrap_or_default();
            facts.push(format!("fact-generation-{}", claim.generation));
            if !facts.iter().any(|item| item == SOURCE_QUOTE) {
                facts.push(SOURCE_QUOTE.into());
            }
            let mut pending = previous_unheard
                .map(|document| document.pending.clone())
                .unwrap_or_default();
            if !pending.iter().any(|item| item == "pending-work") {
                pending.push("pending-work".into());
            }
            let mut decisions = previous_unheard
                .map(|document| document.decisions.clone())
                .unwrap_or_default();
            if !decisions
                .iter()
                .any(|item| item == "use-transactional-publication")
            {
                decisions.push("use-transactional-publication".into());
            }
            let current_spoken = format!(
                "第 {} 代信箱更新。事实：{}。待办：{}。决策：{}。",
                claim.generation,
                facts.join("；"),
                pending.join("；"),
                decisions.join("；")
            );
            Ok(SummaryDocument {
                schema: SUMMARY_SCHEMA_VERSION,
                facts,
                pending,
                decisions,
                spoken_text: previous_unheard.map_or(current_spoken.clone(), |document| {
                    format!("{} {}", document.spoken_text, current_spoken)
                }),
                source_evidence: source_evidence(claim),
                covers_new_completions: claim
                    .completions
                    .iter()
                    .map(|completion| completion.completion_id.clone())
                    .collect(),
            })
        }
    }

    impl SummaryGenerator for LanguageRepairGenerator {
        fn generate(
            &self,
            claim: &SummaryClaim,
            _previous_unheard: Option<&SummaryDocument>,
        ) -> Result<SummaryDocument, SparkError> {
            let call = self.calls.fetch_add(1, Ordering::SeqCst);
            Ok(SummaryDocument {
                schema: SUMMARY_SCHEMA_VERSION,
                facts: vec!["已经完成本地语音信箱状态同步".into(), SOURCE_QUOTE.into()],
                pending: Vec::new(),
                decisions: Vec::new(),
                spoken_text: if call > 0 && self.repair_succeeds {
                    format!(
                        "已经完成本地语音信箱状态同步。任务已经完成，{}，具体结果和后续工作都已整理，可以播放。",
                        SOURCE_QUOTE,
                    )
                } else {
                    "The task is complete and ready to play.".into()
                },
                source_evidence: source_evidence(claim),
                covers_new_completions: claim
                    .completions
                    .iter()
                    .map(|completion| completion.completion_id.clone())
                    .collect(),
            })
        }
    }

    impl SummaryGenerator for DropsPreviousGenerator {
        fn generate(
            &self,
            claim: &SummaryClaim,
            _previous_unheard: Option<&SummaryDocument>,
        ) -> Result<SummaryDocument, SparkError> {
            self.calls.fetch_add(1, Ordering::SeqCst);
            Ok(SummaryDocument {
                schema: SUMMARY_SCHEMA_VERSION,
                facts: vec!["新完成事项已经通过真机验证".into(), SOURCE_QUOTE.into()],
                pending: Vec::new(),
                decisions: Vec::new(),
                spoken_text: format!(
                    "新完成事项已经通过真机验证，{}，旧信箱内容没有保留。",
                    SOURCE_QUOTE
                ),
                source_evidence: source_evidence(claim),
                covers_new_completions: claim
                    .completions
                    .iter()
                    .map(|completion| completion.completion_id.clone())
                    .collect(),
            })
        }
    }

    impl SummaryGenerator for GenericWithoutEvidenceGenerator {
        fn generate(
            &self,
            claim: &SummaryClaim,
            _previous_unheard: Option<&SummaryDocument>,
        ) -> Result<SummaryDocument, SparkError> {
            self.calls.fetch_add(1, Ordering::SeqCst);
            Ok(SummaryDocument {
                schema: SUMMARY_SCHEMA_VERSION,
                facts: vec!["任务已经顺利完成没有其他问题".into()],
                pending: Vec::new(),
                decisions: Vec::new(),
                spoken_text: "任务已经顺利完成，没有其他问题，可以继续后续工作。".into(),
                source_evidence: Vec::new(),
                covers_new_completions: claim
                    .completions
                    .iter()
                    .map(|completion| completion.completion_id.clone())
                    .collect(),
            })
        }
    }

    impl SummaryGenerator for GenericWithWeakEvidenceGenerator {
        fn generate(
            &self,
            claim: &SummaryClaim,
            _previous_unheard: Option<&SummaryDocument>,
        ) -> Result<SummaryDocument, SparkError> {
            self.calls.fetch_add(1, Ordering::SeqCst);
            Ok(SummaryDocument {
                schema: SUMMARY_SCHEMA_VERSION,
                facts: vec![GENERIC_SOURCE_QUOTE.into()],
                pending: Vec::new(),
                decisions: Vec::new(),
                spoken_text: format!(
                    "任务已经顺利完成，{}，可以继续后续工作。",
                    GENERIC_SOURCE_QUOTE
                ),
                source_evidence: claim
                    .completions
                    .iter()
                    .map(|completion| SummarySourceEvidence {
                        completion_id: completion.completion_id.clone(),
                        exact_quote: GENERIC_SOURCE_QUOTE.into(),
                    })
                    .collect(),
                covers_new_completions: claim
                    .completions
                    .iter()
                    .map(|completion| completion.completion_id.clone())
                    .collect(),
            })
        }
    }

    impl SummaryGenerator for ReplaysPreviousGenerator {
        fn generate(
            &self,
            claim: &SummaryClaim,
            previous_unheard: Option<&SummaryDocument>,
        ) -> Result<SummaryDocument, SparkError> {
            self.calls.fetch_add(1, Ordering::SeqCst);
            let mut replay = previous_unheard.cloned().ok_or(SparkError::InvalidInput)?;
            replay.source_evidence = source_evidence(claim);
            replay.covers_new_completions = claim
                .completions
                .iter()
                .map(|completion| completion.completion_id.clone())
                .collect();
            Ok(replay)
        }
    }

    #[derive(Clone, Copy)]
    enum FakeTtsMode {
        Success,
        SafeFailure,
        Ambiguous,
        WrongVoice,
    }

    struct FakeSynthesizer {
        calls: Arc<AtomicUsize>,
        mode: FakeTtsMode,
    }

    impl SummarySynthesizer for FakeSynthesizer {
        fn synthesize(
            &self,
            text: &str,
            voice: &str,
            instructions: &str,
        ) -> Result<TtsAudio, TtsError> {
            self.calls.fetch_add(1, Ordering::SeqCst);
            assert_eq!(voice, SUMMARY_TTS_VOICE);
            assert_eq!(instructions, SUMMARY_TTS_INSTRUCTIONS);
            match self.mode {
                FakeTtsMode::SafeFailure => Err(TtsError::Unavailable),
                FakeTtsMode::Ambiguous => Err(TtsError::AmbiguousAfterCommit),
                FakeTtsMode::Success | FakeTtsMode::WrongVoice => {
                    let samples = text.len().max(1) * 32;
                    let pcm = (0..samples)
                        .flat_map(|sample| ((sample % 511) as i16).to_le_bytes())
                        .collect::<Vec<_>>();
                    Ok(TtsAudio::from_test(
                        pcm,
                        TtsReceipt {
                            model: TTS_MODEL,
                            voice: if matches!(self.mode, FakeTtsMode::WrongVoice) {
                                "WrongVoice".into()
                            } else {
                                voice.into()
                            },
                            sample_rate: TTS_SAMPLE_RATE,
                            samples: samples as u64,
                            characters: Some(text.chars().count() as u64),
                            transport: "fake",
                            attempts: 1,
                        },
                    ))
                }
            }
        }
    }

    fn generator() -> FakeGenerator {
        FakeGenerator {
            calls: Arc::new(AtomicUsize::new(0)),
            previous_facts: Arc::new(Mutex::new(Vec::new())),
        }
    }

    fn synthesizer(mode: FakeTtsMode) -> FakeSynthesizer {
        FakeSynthesizer {
            calls: Arc::new(AtomicUsize::new(0)),
            mode,
        }
    }

    fn insert_completion(store: &mut StateStore, completion_id: &str, ordinal: u64) {
        let expected = store.rollout_cursor(TASK).unwrap();
        let next = RolloutCursor {
            task_id: TASK.into(),
            rollout_path: PathBuf::from("/tmp/easy-codex-input-m4d-fixture.jsonl"),
            device: 7,
            inode: 11,
            offset: ordinal,
            generation: 1,
            anchor: [ordinal as u8 + 1; 32],
        };
        store
            .commit_rollout_completion(
                expected.as_ref(),
                &next,
                completion_id,
                &format!(
                    r#"{{"v":1,"turn_id":"{completion_id}","user":[],"assistant":["{GENERIC_SOURCE_QUOTE}。{SOURCE_QUOTE}。第{ordinal}次。"],"tools":[]}}"#
                ),
            )
            .unwrap();
    }

    fn open_cache(root: &std::path::Path) -> CacheStore {
        open_cache_with_limits(root, CacheLimits::default())
    }

    fn open_cache_with_limits(root: &std::path::Path, limits: CacheLimits) -> CacheStore {
        CacheStore::open_with_raw_key_for_test(root, [31; 32], limits).unwrap()
    }

    fn run_success(store: &mut StateStore, cache: &CacheStore, request: &str) -> SummaryRunOutcome {
        let generator = generator();
        let tts = synthesizer(FakeTtsMode::Success);
        SummaryOrchestrator::new(store, cache, &generator, &tts)
            .run(TASK, request)
            .unwrap()
    }

    #[test]
    fn cumulative_generation_includes_previous_unread_and_replaces_only_after_cache_auth() {
        let temporary = tempfile::tempdir().unwrap();
        let mut store = StateStore::open(&temporary.path().join("state.sqlite3")).unwrap();
        let cache = open_cache(&temporary.path().join("cache"));
        insert_completion(&mut store, FIRST, 1);
        assert!(matches!(
            run_success(&mut store, &cache, "request-1"),
            SummaryRunOutcome::Published {
                recovered_from_cache: false,
                ..
            }
        ));

        insert_completion(&mut store, SECOND, 2);
        let generator = generator();
        let tts = synthesizer(FakeTtsMode::Success);
        let outcome = SummaryOrchestrator::new(&mut store, &cache, &generator, &tts)
            .run(TASK, "request-2")
            .unwrap();
        assert!(matches!(
            outcome,
            SummaryRunOutcome::Published {
                recovered_from_cache: false,
                ..
            }
        ));
        assert_eq!(generator.previous_facts.lock().unwrap().as_slice(), &[2]);
        assert_eq!(
            store
                .current_unread_summary(TASK)
                .unwrap()
                .unwrap()
                .generation,
            2
        );
        assert_eq!(
            store
                .current_unread_summary(TASK)
                .unwrap()
                .unwrap()
                .coverage_count,
            2
        );
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 0);
        let summary = load_tts_summary(&cache, TASK, 2).unwrap();
        assert_eq!(summary.facts.len(), 3);
    }

    #[test]
    fn non_chinese_spoken_text_gets_one_bounded_repair_before_tts() {
        let temporary = tempfile::tempdir().unwrap();
        let mut store = StateStore::open(&temporary.path().join("state.sqlite3")).unwrap();
        let cache = open_cache(&temporary.path().join("cache"));
        insert_completion(&mut store, FIRST, 1);
        let generator = LanguageRepairGenerator {
            calls: Arc::new(AtomicUsize::new(0)),
            repair_succeeds: true,
        };
        let tts = synthesizer(FakeTtsMode::Success);

        let outcome = SummaryOrchestrator::new(&mut store, &cache, &generator, &tts)
            .run(TASK, "language-repair")
            .unwrap();

        assert!(matches!(outcome, SummaryRunOutcome::Published { .. }));
        assert_eq!(generator.calls.load(Ordering::SeqCst), 2);
        assert_eq!(tts.calls.load(Ordering::SeqCst), 1);
        assert!(contains_han_text(
            &load_tts_summary(&cache, TASK, 1).unwrap().spoken_text
        ));
    }

    #[test]
    fn repeated_non_chinese_spoken_text_never_reaches_tts() {
        let temporary = tempfile::tempdir().unwrap();
        let mut store = StateStore::open(&temporary.path().join("state.sqlite3")).unwrap();
        let cache = open_cache(&temporary.path().join("cache"));
        insert_completion(&mut store, FIRST, 1);
        let generator = LanguageRepairGenerator {
            calls: Arc::new(AtomicUsize::new(0)),
            repair_succeeds: false,
        };
        let tts = synthesizer(FakeTtsMode::Success);

        let error = SummaryOrchestrator::new(&mut store, &cache, &generator, &tts)
            .run(TASK, "language-reject")
            .unwrap_err();

        assert!(matches!(
            error,
            SummaryOrchestratorError::Summary(SummaryDocumentError::Text)
        ));
        assert_eq!(generator.calls.load(Ordering::SeqCst), 2);
        assert_eq!(tts.calls.load(Ordering::SeqCst), 0);
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 1);
    }

    #[test]
    fn first_generation_generic_summary_without_exact_source_evidence_is_rejected() {
        let temporary = tempfile::tempdir().unwrap();
        let mut store = StateStore::open(&temporary.path().join("state.sqlite3")).unwrap();
        let cache = open_cache(&temporary.path().join("cache"));
        insert_completion(&mut store, FIRST, 1);
        let generator = GenericWithoutEvidenceGenerator {
            calls: Arc::new(AtomicUsize::new(0)),
        };
        let tts = synthesizer(FakeTtsMode::Success);

        let error = SummaryOrchestrator::new(&mut store, &cache, &generator, &tts)
            .run(TASK, "generic-first-generation")
            .unwrap_err();

        assert!(matches!(
            error,
            SummaryOrchestratorError::Summary(SummaryDocumentError::Text)
        ));
        assert_eq!(generator.calls.load(Ordering::SeqCst), 2);
        assert_eq!(tts.calls.load(Ordering::SeqCst), 0);
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 1);
        assert!(store.current_unread_summary(TASK).unwrap().is_none());
    }

    #[test]
    fn first_generation_generic_summary_cannot_select_a_weak_source_quote() {
        let temporary = tempfile::tempdir().unwrap();
        let mut store = StateStore::open(&temporary.path().join("state.sqlite3")).unwrap();
        let cache = open_cache(&temporary.path().join("cache"));
        insert_completion(&mut store, FIRST, 1);
        let generator = GenericWithWeakEvidenceGenerator {
            calls: Arc::new(AtomicUsize::new(0)),
        };
        let tts = synthesizer(FakeTtsMode::Success);

        let error = SummaryOrchestrator::new(&mut store, &cache, &generator, &tts)
            .run(TASK, "generic-weak-evidence")
            .unwrap_err();

        assert!(matches!(
            error,
            SummaryOrchestratorError::Summary(SummaryDocumentError::Text)
        ));
        assert_eq!(generator.calls.load(Ordering::SeqCst), 2);
        assert_eq!(tts.calls.load(Ordering::SeqCst), 0);
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 1);
        assert!(store.current_unread_summary(TASK).unwrap().is_none());
    }

    #[test]
    fn repeated_previous_unheard_omission_keeps_old_generation_and_skips_tts() {
        let temporary = tempfile::tempdir().unwrap();
        let mut store = StateStore::open(&temporary.path().join("state.sqlite3")).unwrap();
        let cache = open_cache(&temporary.path().join("cache"));
        insert_completion(&mut store, FIRST, 1);
        run_success(&mut store, &cache, "request-1");
        let old = store.current_unread_summary(TASK).unwrap().unwrap();
        insert_completion(&mut store, SECOND, 2);
        let generator = DropsPreviousGenerator {
            calls: Arc::new(AtomicUsize::new(0)),
        };
        let tts = synthesizer(FakeTtsMode::Success);

        let error = SummaryOrchestrator::new(&mut store, &cache, &generator, &tts)
            .run(TASK, "drop-previous")
            .unwrap_err();

        assert!(matches!(
            error,
            SummaryOrchestratorError::Summary(SummaryDocumentError::Text)
        ));
        assert_eq!(generator.calls.load(Ordering::SeqCst), 2);
        assert_eq!(tts.calls.load(Ordering::SeqCst), 0);
        assert_eq!(store.current_unread_summary(TASK).unwrap(), Some(old));
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 1);
    }

    #[test]
    fn unchanged_previous_replay_cannot_consume_a_new_completion() {
        let temporary = tempfile::tempdir().unwrap();
        let mut store = StateStore::open(&temporary.path().join("state.sqlite3")).unwrap();
        let cache = open_cache(&temporary.path().join("cache"));
        insert_completion(&mut store, FIRST, 1);
        run_success(&mut store, &cache, "request-1");
        let old = store.current_unread_summary(TASK).unwrap().unwrap();
        insert_completion(&mut store, SECOND, 2);
        let generator = ReplaysPreviousGenerator {
            calls: Arc::new(AtomicUsize::new(0)),
        };
        let tts = synthesizer(FakeTtsMode::Success);

        let error = SummaryOrchestrator::new(&mut store, &cache, &generator, &tts)
            .run(TASK, "replay-previous")
            .unwrap_err();

        assert!(matches!(
            error,
            SummaryOrchestratorError::Summary(SummaryDocumentError::Text)
        ));
        assert_eq!(generator.calls.load(Ordering::SeqCst), 2);
        assert_eq!(tts.calls.load(Ordering::SeqCst), 0);
        assert_eq!(store.current_unread_summary(TASK).unwrap(), Some(old));
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 1);
    }

    #[test]
    fn safe_tts_failure_releases_claim_without_replacing_old_unread() {
        let temporary = tempfile::tempdir().unwrap();
        let mut store = StateStore::open(&temporary.path().join("state.sqlite3")).unwrap();
        let cache = open_cache(&temporary.path().join("cache"));
        insert_completion(&mut store, FIRST, 1);
        run_success(&mut store, &cache, "request-1");
        let old = store.current_unread_summary(TASK).unwrap().unwrap();
        insert_completion(&mut store, SECOND, 2);

        let generator = generator();
        let tts = synthesizer(FakeTtsMode::SafeFailure);
        let error = SummaryOrchestrator::new(&mut store, &cache, &generator, &tts)
            .run(TASK, "request-2")
            .unwrap_err();
        assert!(matches!(
            error,
            SummaryOrchestratorError::Tts(TtsError::Unavailable)
        ));
        assert_eq!(store.current_unread_summary(TASK).unwrap(), Some(old));
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 1);
        assert!(matches!(
            store.claim_summary(TASK, "request-2"),
            Err(StoreError::SummaryRequestAbandoned)
        ));
    }

    #[test]
    fn ambiguous_tts_without_authenticated_cache_releases_the_slot() {
        let temporary = tempfile::tempdir().unwrap();
        let state_path = temporary.path().join("state.sqlite3");
        let cache_path = temporary.path().join("cache");
        let mut store = StateStore::open(&state_path).unwrap();
        let cache = open_cache(&cache_path);
        insert_completion(&mut store, FIRST, 1);
        run_success(&mut store, &cache, "request-1");
        let old = store.current_unread_summary(TASK).unwrap().unwrap();
        insert_completion(&mut store, SECOND, 2);

        let interrupted = match store.claim_summary(TASK, "request-2").unwrap().unwrap() {
            SummaryClaimResult::Claimed(claim) => claim,
            SummaryClaimResult::Published { .. } => panic!("unexpected publication"),
        };
        assert_eq!(
            store.begin_summary_tts_attempt(&interrupted).unwrap(),
            SummaryTtsAttemptState::Started
        );
        store.mark_summary_tts_ambiguous(&interrupted).unwrap();
        drop(cache);
        drop(store);

        let mut reopened = StateStore::open(&state_path).unwrap();
        let cache = open_cache(&cache_path);
        let generator = generator();
        let never_called = synthesizer(FakeTtsMode::Success);
        let outcome = SummaryOrchestrator::new(&mut reopened, &cache, &generator, &never_called)
            .resume(TASK)
            .unwrap();
        assert_eq!(outcome, SummaryRunOutcome::Idle);
        assert_eq!(never_called.calls.load(Ordering::SeqCst), 0);
        assert_eq!(reopened.current_unread_summary(TASK).unwrap(), Some(old));
        assert_eq!(reopened.pending_summary_completion_count(TASK).unwrap(), 1);
        let replacement = reopened.claim_summary(TASK, "request-3").unwrap().unwrap();
        assert!(matches!(
            replacement,
            SummaryClaimResult::Claimed(SummaryClaim { generation: 3, .. })
        ));
    }

    #[test]
    fn ambiguous_tts_response_releases_the_slot_without_restart() {
        let temporary = tempfile::tempdir().unwrap();
        let mut store = StateStore::open(&temporary.path().join("state.sqlite3")).unwrap();
        let cache = open_cache(&temporary.path().join("cache"));
        insert_completion(&mut store, FIRST, 1);
        let generator = generator();
        let ambiguous = synthesizer(FakeTtsMode::Ambiguous);

        let outcome = SummaryOrchestrator::new(&mut store, &cache, &generator, &ambiguous)
            .run(TASK, "request-1")
            .unwrap();

        assert_eq!(outcome, SummaryRunOutcome::Idle);
        assert_eq!(ambiguous.calls.load(Ordering::SeqCst), 1);
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 1);
        assert!(matches!(
            store.claim_summary(TASK, "request-2").unwrap(),
            Some(SummaryClaimResult::Claimed(SummaryClaim {
                generation: 2,
                ..
            }))
        ));
    }

    #[test]
    fn process_restart_after_started_tts_abandons_stale_claim_and_publishes_replacement() {
        let temporary = tempfile::tempdir().unwrap();
        let state_path = temporary.path().join("state.sqlite3");
        let cache_path = temporary.path().join("cache");
        let mut store = StateStore::open(&state_path).unwrap();
        let cache = open_cache(&cache_path);
        insert_completion(&mut store, FIRST, 1);
        run_success(&mut store, &cache, "request-1");
        insert_completion(&mut store, SECOND, 2);
        let interrupted = match store.claim_summary(TASK, "request-2").unwrap().unwrap() {
            SummaryClaimResult::Claimed(claim) => claim,
            SummaryClaimResult::Published { .. } => panic!("unexpected publication"),
        };
        assert_eq!(
            store.begin_summary_tts_attempt(&interrupted).unwrap(),
            SummaryTtsAttemptState::Started
        );
        drop(cache);
        drop(store);

        let mut reopened = StateStore::open(&state_path).unwrap();
        let cache = open_cache(&cache_path);
        let generator = generator();
        let tts = synthesizer(FakeTtsMode::Success);
        let mut orchestrator = SummaryOrchestrator::new(&mut reopened, &cache, &generator, &tts);
        assert_eq!(orchestrator.resume(TASK).unwrap(), SummaryRunOutcome::Idle);
        let replacement = orchestrator.run(TASK, "request-3").unwrap();

        assert!(matches!(
            replacement,
            SummaryRunOutcome::Published {
                unread: UnreadSummary {
                    generation: 3,
                    coverage_count: 2,
                    ..
                },
                recovered_from_cache: false,
                ..
            }
        ));
        assert_eq!(tts.calls.load(Ordering::SeqCst), 1);
        assert_eq!(generator.previous_facts.lock().unwrap().as_slice(), &[2]);
        assert_eq!(reopened.pending_summary_completion_count(TASK).unwrap(), 0);
    }

    #[test]
    fn cache_failure_without_authenticated_generation_releases_the_slot() {
        let temporary = tempfile::tempdir().unwrap();
        let state_path = temporary.path().join("state.sqlite3");
        let cache_path = temporary.path().join("cache");
        let limits = CacheLimits {
            max_total_bytes: u64::MAX,
            max_generations: 1,
        };
        let mut store = StateStore::open(&state_path).unwrap();
        let cache = open_cache_with_limits(&cache_path, limits);
        insert_completion(&mut store, FIRST, 1);
        run_success(&mut store, &cache, "request-1");
        let old = store.current_unread_summary(TASK).unwrap().unwrap();
        insert_completion(&mut store, SECOND, 2);

        let generator = generator();
        let successful_tts = synthesizer(FakeTtsMode::Success);
        let outcome = SummaryOrchestrator::new(&mut store, &cache, &generator, &successful_tts)
            .run(TASK, "request-2")
            .unwrap();
        assert_eq!(outcome, SummaryRunOutcome::Idle);
        assert_eq!(successful_tts.calls.load(Ordering::SeqCst), 1);
        assert_eq!(store.current_unread_summary(TASK).unwrap(), Some(old));
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 1);
        assert!(matches!(
            store.claim_summary(TASK, "request-3").unwrap(),
            Some(SummaryClaimResult::Claimed(SummaryClaim {
                generation: 3,
                ..
            }))
        ));
    }

    #[test]
    fn successful_tts_with_wrong_voice_releases_the_slot() {
        let temporary = tempfile::tempdir().unwrap();
        let state_path = temporary.path().join("state.sqlite3");
        let cache_path = temporary.path().join("cache");
        let mut store = StateStore::open(&state_path).unwrap();
        let cache = open_cache(&cache_path);
        insert_completion(&mut store, FIRST, 1);
        run_success(&mut store, &cache, "request-1");
        let old = store.current_unread_summary(TASK).unwrap().unwrap();
        insert_completion(&mut store, SECOND, 2);

        let generator = generator();
        let wrong_voice = synthesizer(FakeTtsMode::WrongVoice);
        let outcome = SummaryOrchestrator::new(&mut store, &cache, &generator, &wrong_voice)
            .run(TASK, "request-2")
            .unwrap();
        assert_eq!(outcome, SummaryRunOutcome::Idle);
        assert_eq!(wrong_voice.calls.load(Ordering::SeqCst), 1);
        assert_eq!(store.current_unread_summary(TASK).unwrap(), Some(old));
        assert_eq!(store.pending_summary_completion_count(TASK).unwrap(), 1);
        assert!(matches!(
            store.claim_summary(TASK, "request-3").unwrap(),
            Some(SummaryClaimResult::Claimed(SummaryClaim {
                generation: 3,
                ..
            }))
        ));
    }

    #[test]
    fn authenticated_orphan_with_wrong_claim_coverage_never_publishes_or_calls_tts() {
        let temporary = tempfile::tempdir().unwrap();
        let state_path = temporary.path().join("state.sqlite3");
        let cache_path = temporary.path().join("cache");
        let mut store = StateStore::open(&state_path).unwrap();
        let cache = open_cache(&cache_path);
        insert_completion(&mut store, FIRST, 1);
        run_success(&mut store, &cache, "request-1");
        let old = store.current_unread_summary(TASK).unwrap().unwrap();
        insert_completion(&mut store, SECOND, 2);
        let claim = match store.claim_summary(TASK, "request-2").unwrap().unwrap() {
            SummaryClaimResult::Claimed(claim) => claim,
            SummaryClaimResult::Published { .. } => panic!("unexpected publication"),
        };
        let wrong_summary = SummaryDocument {
            schema: SUMMARY_SCHEMA_VERSION,
            facts: vec!["wrong-coverage".into()],
            pending: vec!["pending-work".into()],
            decisions: vec!["fail-closed".into()],
            spoken_text: "wrong coverage summary".into(),
            source_evidence: vec![],
            covers_new_completions: vec![FIRST.into()],
        };
        let fake_tts = synthesizer(FakeTtsMode::Success)
            .synthesize(
                &wrong_summary.spoken_text,
                SUMMARY_TTS_VOICE,
                SUMMARY_TTS_INSTRUCTIONS,
            )
            .unwrap();
        crate::tts_cache::publish_tts_generation(
            &cache,
            TASK,
            claim.generation,
            wrong_summary,
            &fake_tts,
        )
        .unwrap();
        drop(cache);
        drop(store);

        let mut reopened = StateStore::open(&state_path).unwrap();
        let cache = open_cache(&cache_path);
        let generator = generator();
        let retry_tts = synthesizer(FakeTtsMode::Success);
        let error = SummaryOrchestrator::new(&mut reopened, &cache, &generator, &retry_tts)
            .resume(TASK)
            .unwrap_err();
        assert!(matches!(
            error,
            SummaryOrchestratorError::Summary(SummaryDocumentError::Coverage)
        ));
        assert_eq!(retry_tts.calls.load(Ordering::SeqCst), 0);
        assert_eq!(reopened.current_unread_summary(TASK).unwrap(), Some(old));
        assert_eq!(reopened.pending_summary_completion_count(TASK).unwrap(), 1);
    }

    struct KillObserver {
        target: SummaryCheckpoint,
        ready: PathBuf,
    }

    impl SummaryCheckpointObserver for KillObserver {
        fn reached(&mut self, checkpoint: SummaryCheckpoint) {
            if checkpoint == self.target {
                std::fs::write(&self.ready, b"ready").unwrap();
                loop {
                    std::thread::sleep(std::time::Duration::from_secs(60));
                }
            }
        }
    }

    fn checkpoint_name(checkpoint: SummaryCheckpoint) -> &'static str {
        match checkpoint {
            SummaryCheckpoint::ClaimAcquired => "claim",
            SummaryCheckpoint::PreviousUnreadLoaded => "previous",
            SummaryCheckpoint::SparkGenerated => "spark",
            SummaryCheckpoint::TtsAttemptStarted => "tts-started",
            SummaryCheckpoint::TtsGenerated => "tts-generated",
            SummaryCheckpoint::CacheAuthenticated => "cache",
            SummaryCheckpoint::LedgerPublished => "ledger",
        }
    }

    fn parse_checkpoint(value: &str) -> SummaryCheckpoint {
        match value {
            "claim" => SummaryCheckpoint::ClaimAcquired,
            "previous" => SummaryCheckpoint::PreviousUnreadLoaded,
            "spark" => SummaryCheckpoint::SparkGenerated,
            "tts-started" => SummaryCheckpoint::TtsAttemptStarted,
            "tts-generated" => SummaryCheckpoint::TtsGenerated,
            "cache" => SummaryCheckpoint::CacheAuthenticated,
            "ledger" => SummaryCheckpoint::LedgerPublished,
            _ => panic!("invalid checkpoint"),
        }
    }

    #[test]
    fn checkpoint_sigkill_helper() {
        let Some(state_path) = std::env::var_os("ECI_M4D_KILL_STATE") else {
            return;
        };
        let cache_path = PathBuf::from(std::env::var_os("ECI_M4D_KILL_CACHE").unwrap());
        let ready = PathBuf::from(std::env::var_os("ECI_M4D_KILL_READY").unwrap());
        let target = parse_checkpoint(&std::env::var("ECI_M4D_KILL_CHECKPOINT").unwrap());
        let mut store = StateStore::open(PathBuf::from(state_path).as_path()).unwrap();
        let cache = open_cache(&cache_path);
        let generator = generator();
        let tts = synthesizer(FakeTtsMode::Success);
        SummaryOrchestrator::new(&mut store, &cache, &generator, &tts)
            .run_with_observer(TASK, "request-2", &mut KillObserver { target, ready })
            .unwrap();
        panic!("kill checkpoint was not reached");
    }

    #[test]
    fn every_checkpoint_sigkill_recovers_without_losing_old_unread_or_completion_coverage() {
        use std::os::unix::process::ExitStatusExt;
        use std::process::Command;

        let checkpoints = [
            SummaryCheckpoint::ClaimAcquired,
            SummaryCheckpoint::PreviousUnreadLoaded,
            SummaryCheckpoint::SparkGenerated,
            SummaryCheckpoint::TtsAttemptStarted,
            SummaryCheckpoint::TtsGenerated,
            SummaryCheckpoint::CacheAuthenticated,
            SummaryCheckpoint::LedgerPublished,
        ];
        for checkpoint in checkpoints {
            let temporary = tempfile::tempdir().unwrap();
            let state_path = temporary.path().join("state.sqlite3");
            let cache_path = temporary.path().join("cache");
            let mut store = StateStore::open(&state_path).unwrap();
            let cache = open_cache(&cache_path);
            insert_completion(&mut store, FIRST, 1);
            run_success(&mut store, &cache, "request-1");
            let old = store.current_unread_summary(TASK).unwrap().unwrap();
            insert_completion(&mut store, SECOND, 2);
            let ready = temporary.path().join("ready");
            drop(cache);
            drop(store);

            let mut child = Command::new(std::env::current_exe().unwrap())
                .args([
                    "--exact",
                    "summary_orchestrator::tests::checkpoint_sigkill_helper",
                    "--nocapture",
                ])
                .env("ECI_M4D_KILL_STATE", &state_path)
                .env("ECI_M4D_KILL_CACHE", &cache_path)
                .env("ECI_M4D_KILL_READY", &ready)
                .env("ECI_M4D_KILL_CHECKPOINT", checkpoint_name(checkpoint))
                .spawn()
                .unwrap();
            for _ in 0..500 {
                if ready.exists() {
                    break;
                }
                if let Some(status) = child.try_wait().unwrap() {
                    panic!("kill helper exited before {checkpoint:?}: {status}");
                }
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
            assert!(ready.exists(), "kill helper did not reach {checkpoint:?}");
            assert_eq!(unsafe { libc::kill(child.id() as i32, libc::SIGKILL) }, 0);
            assert_eq!(child.wait().unwrap().signal(), Some(libc::SIGKILL));

            let mut reopened = StateStore::open(&state_path).unwrap();
            let cache = open_cache(&cache_path);
            let retry_generator = generator();
            let retry_tts = synthesizer(FakeTtsMode::Success);
            let recovered =
                SummaryOrchestrator::new(&mut reopened, &cache, &retry_generator, &retry_tts)
                    .resume(TASK)
                    .unwrap();
            match checkpoint {
                SummaryCheckpoint::TtsAttemptStarted | SummaryCheckpoint::TtsGenerated => {
                    assert_eq!(recovered, SummaryRunOutcome::Idle);
                    assert_eq!(retry_tts.calls.load(Ordering::SeqCst), 0);
                    assert_eq!(reopened.current_unread_summary(TASK).unwrap(), Some(old));
                    assert_eq!(reopened.pending_summary_completion_count(TASK).unwrap(), 1);
                    let replacement = SummaryOrchestrator::new(
                        &mut reopened,
                        &cache,
                        &retry_generator,
                        &retry_tts,
                    )
                    .run(TASK, "request-3")
                    .unwrap();
                    assert!(matches!(
                        replacement,
                        SummaryRunOutcome::Published {
                            unread: UnreadSummary {
                                generation: 3,
                                coverage_count: 2,
                                ..
                            },
                            ..
                        }
                    ));
                    assert_eq!(retry_tts.calls.load(Ordering::SeqCst), 1);
                    assert_eq!(reopened.pending_summary_completion_count(TASK).unwrap(), 0);
                }
                SummaryCheckpoint::LedgerPublished => {
                    assert_eq!(recovered, SummaryRunOutcome::Idle);
                    assert_eq!(
                        reopened
                            .current_unread_summary(TASK)
                            .unwrap()
                            .unwrap()
                            .generation,
                        2
                    );
                    assert_eq!(reopened.pending_summary_completion_count(TASK).unwrap(), 0);
                }
                SummaryCheckpoint::CacheAuthenticated => {
                    assert!(matches!(
                        recovered,
                        SummaryRunOutcome::Published {
                            recovered_from_cache: true,
                            ..
                        }
                    ));
                    assert_eq!(retry_tts.calls.load(Ordering::SeqCst), 0);
                    assert_eq!(reopened.pending_summary_completion_count(TASK).unwrap(), 0);
                }
                _ => {
                    assert!(matches!(
                        recovered,
                        SummaryRunOutcome::Published {
                            recovered_from_cache: false,
                            ..
                        }
                    ));
                    assert_eq!(reopened.pending_summary_completion_count(TASK).unwrap(), 0);
                }
            }
            assert!(CacheId::for_task(TASK, 1).is_ok());
        }
    }

    #[test]
    fn tts_attempt_schema_migrates_and_attempt_state_is_bound_to_claim() {
        let temporary = tempfile::tempdir().unwrap();
        let mut store = StateStore::open(&temporary.path().join("state.sqlite3")).unwrap();
        assert_eq!(
            store.schema_version().unwrap(),
            crate::store::SCHEMA_VERSION
        );
        insert_completion(&mut store, FIRST, 1);
        let claim = match store.claim_summary(TASK, "request-1").unwrap().unwrap() {
            SummaryClaimResult::Claimed(claim) => claim,
            SummaryClaimResult::Published { .. } => panic!("unexpected publication"),
        };
        assert_eq!(claim.outcome, SummaryClaimOutcome::Inserted);
        assert_eq!(store.summary_tts_attempt(&claim).unwrap(), None);
        assert_eq!(
            store.begin_summary_tts_attempt(&claim).unwrap(),
            SummaryTtsAttemptState::Started
        );
        store.mark_summary_tts_ambiguous(&claim).unwrap();
        assert_eq!(
            store.summary_tts_attempt(&claim).unwrap(),
            Some(SummaryTtsAttemptState::Ambiguous)
        );
        assert!(store.abandon_summary_claim(&claim).unwrap());
        assert!(matches!(store.summary_tts_attempt(&claim), Ok(None)));
    }

    #[test]
    fn existing_schema_without_tts_attempt_table_is_upgraded_in_place() {
        let temporary = tempfile::tempdir().unwrap();
        let path = temporary.path().join("state.sqlite3");
        let store = StateStore::open(&path).unwrap();
        drop(store);
        let connection = rusqlite::Connection::open(&path).unwrap();
        connection
            .execute_batch("DROP TABLE summary_tts_attempts; PRAGMA user_version = 1;")
            .unwrap();
        drop(connection);

        let store = StateStore::open(&path).unwrap();
        assert_eq!(
            store.schema_version().unwrap(),
            crate::store::SCHEMA_VERSION
        );
    }
}

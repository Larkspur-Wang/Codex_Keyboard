use serde::{Deserialize, Serialize};
use thiserror::Error;
use zeroize::Zeroizing;

use crate::audio::{AudioError, TTS_SAMPLE_RATE, decode_wav, encode_tts_audio, inspect_eiad};
use crate::cache::{CacheBundle, CacheError, CacheId, CacheStore};
use crate::dashscope::{TTS_MODEL, TTS_MODEL_SNAPSHOT, TtsAudio};
use crate::summary::{SummaryDocument, SummaryDocumentError};

pub const TTS_CACHE_MANIFEST_SCHEMA: u8 = 1;

#[derive(Debug, Error)]
pub enum TtsCacheError {
    #[error(transparent)]
    Audio(#[from] AudioError),
    #[error(transparent)]
    Cache(#[from] CacheError),
    #[error(transparent)]
    Summary(#[from] SummaryDocumentError),
    #[error("TTS cache manifest is malformed or inconsistent")]
    Manifest,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct TtsCacheManifest {
    schema: u8,
    requested_model: String,
    served_model: String,
    voice: String,
    response_format: String,
    sample_rate: u32,
    channels: u8,
    pcm_bits_per_sample: u8,
    samples: u64,
    duration_ms: u64,
    eiad_frames: u32,
    billed_characters: Option<u64>,
    summary: SummaryDocument,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PublishedTtsGeneration {
    pub cache_reference: String,
    pub samples: u64,
    pub duration_ms: u64,
    pub eiad_frames: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CachedTtsGeneration {
    pub summary: SummaryDocument,
    pub publication: PublishedTtsGeneration,
}

pub fn publish_tts_generation(
    cache: &CacheStore,
    task_id: &str,
    generation: u64,
    summary: SummaryDocument,
    tts: &TtsAudio,
) -> Result<PublishedTtsGeneration, TtsCacheError> {
    match publish_tts_generation_with(
        cache,
        task_id,
        generation,
        summary,
        tts,
        Ok::<PublishedTtsGeneration, std::convert::Infallible>,
    )? {
        Ok(published) => Ok(published),
        Err(never) => match never {},
    }
}

pub fn publish_tts_generation_with<R, E, F>(
    cache: &CacheStore,
    task_id: &str,
    generation: u64,
    summary: SummaryDocument,
    tts: &TtsAudio,
    after_authenticated_read: F,
) -> Result<Result<R, E>, TtsCacheError>
where
    F: FnOnce(PublishedTtsGeneration) -> Result<R, E>,
{
    drop(Zeroizing::new(summary.canonical_json()?));
    if tts.receipt().sample_rate != TTS_SAMPLE_RATE
        || tts.receipt().samples != (tts.pcm().len() / 2) as u64
    {
        return Err(TtsCacheError::Manifest);
    }
    let artifacts = encode_tts_audio(tts.pcm())?;
    let manifest = TtsCacheManifest {
        schema: TTS_CACHE_MANIFEST_SCHEMA,
        requested_model: TTS_MODEL.to_owned(),
        served_model: tts.receipt().model.to_owned(),
        voice: tts.receipt().voice.clone(),
        response_format: "pcm".to_owned(),
        sample_rate: TTS_SAMPLE_RATE,
        channels: 1,
        pcm_bits_per_sample: 16,
        samples: artifacts.samples(),
        duration_ms: artifacts.duration_ms(),
        eiad_frames: artifacts.frames(),
        billed_characters: tts.receipt().characters,
        summary,
    };
    let manifest_json =
        Zeroizing::new(serde_json::to_vec(&manifest).map_err(|_| TtsCacheError::Manifest)?);
    let id = CacheId::for_task(task_id, generation)?;
    let committed: Result<R, CallbackError<E>> = cache.publish_or_verify_with(
        &id,
        CacheBundle {
            manifest_json: &manifest_json,
            qwen_wav: artifacts.wav(),
            device_eiad: artifacts.eiad(),
        },
        |published| -> Result<R, CallbackError<E>> {
            let decoded_manifest: TtsCacheManifest =
                serde_json::from_slice(published.manifest_json.as_slice())
                    .map_err(|_| TtsCacheError::Manifest)?;
            validate_manifest(&decoded_manifest, tts)?;
            let wav_pcm = decode_wav(published.qwen_wav.as_slice()).map_err(TtsCacheError::from)?;
            if wav_pcm.as_slice() != tts.pcm() {
                return Err(TtsCacheError::Manifest.into());
            }
            let eiad =
                inspect_eiad(published.device_eiad.as_slice()).map_err(TtsCacheError::from)?;
            if eiad.samples != decoded_manifest.samples
                || eiad.frames != decoded_manifest.eiad_frames
                || eiad.sample_rate != decoded_manifest.sample_rate
            {
                return Err(TtsCacheError::Manifest.into());
            }
            let generation = PublishedTtsGeneration {
                cache_reference: id.reference(),
                samples: decoded_manifest.samples,
                duration_ms: decoded_manifest.duration_ms,
                eiad_frames: decoded_manifest.eiad_frames,
            };
            after_authenticated_read(generation).map_err(CallbackError::Callback)
        },
    )?;
    match committed {
        Ok(value) => Ok(Ok(value)),
        Err(CallbackError::Cache(error)) => Err(error),
        Err(CallbackError::Callback(error)) => Ok(Err(error)),
    }
}

enum CallbackError<E> {
    Cache(TtsCacheError),
    Callback(E),
}

impl<E> From<TtsCacheError> for CallbackError<E> {
    fn from(error: TtsCacheError) -> Self {
        Self::Cache(error)
    }
}

pub fn load_tts_generation_with<R, E, F>(
    cache: &CacheStore,
    task_id: &str,
    generation: u64,
    after_authenticated_read: F,
) -> Result<Result<R, E>, TtsCacheError>
where
    F: FnOnce(CachedTtsGeneration) -> Result<R, E>,
{
    let id = CacheId::for_task(task_id, generation)?;
    let loaded: Result<R, CallbackError<E>> = cache.read_with(&id, |published| {
        let manifest: TtsCacheManifest = serde_json::from_slice(published.manifest_json.as_slice())
            .map_err(|_| TtsCacheError::Manifest)?;
        validate_cached_manifest(&manifest, published)?;
        let publication = PublishedTtsGeneration {
            cache_reference: id.reference(),
            samples: manifest.samples,
            duration_ms: manifest.duration_ms,
            eiad_frames: manifest.eiad_frames,
        };
        after_authenticated_read(CachedTtsGeneration {
            summary: manifest.summary,
            publication,
        })
        .map_err(CallbackError::Callback)
    })?;
    match loaded {
        Ok(value) => Ok(Ok(value)),
        Err(CallbackError::Cache(error)) => Err(error),
        Err(CallbackError::Callback(error)) => Ok(Err(error)),
    }
}

pub fn load_tts_summary(
    cache: &CacheStore,
    task_id: &str,
    generation: u64,
) -> Result<SummaryDocument, TtsCacheError> {
    match load_tts_generation_with(cache, task_id, generation, |loaded| {
        Ok::<SummaryDocument, std::convert::Infallible>(loaded.summary)
    })? {
        Ok(summary) => Ok(summary),
        Err(never) => match never {},
    }
}

fn validate_cached_manifest(
    manifest: &TtsCacheManifest,
    published: &crate::cache::DecryptedCacheBundle,
) -> Result<(), TtsCacheError> {
    drop(Zeroizing::new(manifest.summary.canonical_json()?));
    let model_valid = matches!(
        manifest.served_model.as_str(),
        TTS_MODEL | TTS_MODEL_SNAPSHOT
    );
    if manifest.schema != TTS_CACHE_MANIFEST_SCHEMA
        || manifest.requested_model != TTS_MODEL
        || !model_valid
        || manifest.voice.is_empty()
        || manifest.response_format != "pcm"
        || manifest.sample_rate != TTS_SAMPLE_RATE
        || manifest.channels != 1
        || manifest.pcm_bits_per_sample != 16
        || manifest.samples == 0
        || manifest.duration_ms
            != manifest.samples.saturating_mul(1_000) / u64::from(TTS_SAMPLE_RATE)
        || manifest.eiad_frames == 0
    {
        return Err(TtsCacheError::Manifest);
    }
    let wav_pcm = decode_wav(published.qwen_wav.as_slice()).map_err(TtsCacheError::from)?;
    if (wav_pcm.len() / 2) as u64 != manifest.samples {
        return Err(TtsCacheError::Manifest);
    }
    let eiad = inspect_eiad(published.device_eiad.as_slice()).map_err(TtsCacheError::from)?;
    if eiad.samples != manifest.samples
        || eiad.frames != manifest.eiad_frames
        || eiad.sample_rate != manifest.sample_rate
    {
        return Err(TtsCacheError::Manifest);
    }
    Ok(())
}

fn validate_manifest(manifest: &TtsCacheManifest, tts: &TtsAudio) -> Result<(), TtsCacheError> {
    drop(Zeroizing::new(manifest.summary.canonical_json()?));
    if manifest.schema != TTS_CACHE_MANIFEST_SCHEMA
        || manifest.requested_model != TTS_MODEL
        || manifest.served_model != tts.receipt().model
        || manifest.voice != tts.receipt().voice
        || manifest.response_format != "pcm"
        || manifest.sample_rate != TTS_SAMPLE_RATE
        || manifest.channels != 1
        || manifest.pcm_bits_per_sample != 16
        || manifest.samples != tts.receipt().samples
        || manifest.duration_ms
            != manifest.samples.saturating_mul(1_000) / u64::from(TTS_SAMPLE_RATE)
        || manifest.eiad_frames == 0
        || manifest.billed_characters != tts.receipt().characters
    {
        Err(TtsCacheError::Manifest)
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audio::{EIAD_FRAME_SAMPLES, decode_eiad};
    use crate::cache::CacheLimits;
    use crate::dashscope::TtsReceipt;
    use crate::summary::SUMMARY_SCHEMA_VERSION;

    fn summary() -> SummaryDocument {
        SummaryDocument {
            schema: SUMMARY_SCHEMA_VERSION,
            facts: vec!["The realtime client completed.".into()],
            pending: vec!["Run the composed live gate.".into()],
            decisions: vec!["Keep plaintext inside encrypted cache objects.".into()],
            spoken_text: "The realtime client and audio codecs are ready.".into(),
            covers_new_completions: vec!["e2747337-dff1-4d99-b5af-4cb4ba09c557".into()],
        }
    }

    fn pcm(samples: usize) -> Vec<u8> {
        (0..samples)
            .flat_map(|sample| ((sample as i32 % 1_000) as i16).to_le_bytes())
            .collect()
    }

    fn tts(pcm: Vec<u8>) -> TtsAudio {
        let samples = (pcm.len() / 2) as u64;
        TtsAudio::from_test(
            pcm,
            TtsReceipt {
                model: TTS_MODEL,
                voice: "Cherry".into(),
                sample_rate: TTS_SAMPLE_RATE,
                samples,
                characters: Some(48),
                transport: "direct",
                attempts: 1,
            },
        )
    }

    #[test]
    fn generation_publishes_three_authenticated_objects_and_is_idempotent() {
        let temporary = tempfile::tempdir().unwrap();
        let cache = CacheStore::open_with_raw_key_for_test(
            &temporary.path().join("tts"),
            [19; 32],
            CacheLimits::default(),
        )
        .unwrap();
        let audio = tts(pcm(EIAD_FRAME_SAMPLES + 7));
        let first = publish_tts_generation(&cache, "task-a", 1, summary(), &audio).unwrap();
        let second = publish_tts_generation(&cache, "task-a", 1, summary(), &audio).unwrap();
        assert_eq!(first, second);
        assert_eq!(first.eiad_frames, 2);
        let id = CacheId::from_reference(&first.cache_reference).unwrap();
        let loaded = cache.read(&id).unwrap();
        assert_eq!(
            decode_wav(loaded.qwen_wav.as_slice()).unwrap().as_slice(),
            audio.pcm()
        );
        assert_eq!(
            decode_eiad(loaded.device_eiad.as_slice()).unwrap().len(),
            audio.pcm().len()
        );
        assert_eq!(cache.audit().unwrap().finalized_generations, 1);
    }

    #[test]
    fn same_generation_with_different_plaintext_cannot_replace_published_audio() {
        let temporary = tempfile::tempdir().unwrap();
        let cache = CacheStore::open_with_raw_key_for_test(
            &temporary.path().join("tts"),
            [23; 32],
            CacheLimits::default(),
        )
        .unwrap();
        let original = tts(pcm(100));
        publish_tts_generation(&cache, "task-a", 1, summary(), &original).unwrap();
        let replacement = tts(pcm(101));
        assert!(matches!(
            publish_tts_generation(&cache, "task-a", 1, summary(), &replacement),
            Err(TtsCacheError::Cache(CacheError::AlreadyPublished))
        ));
        let id = CacheId::for_task("task-a", 1).unwrap();
        assert_eq!(
            decode_wav(cache.read(&id).unwrap().qwen_wav.as_slice())
                .unwrap()
                .as_slice(),
            original.pcm()
        );
    }
}

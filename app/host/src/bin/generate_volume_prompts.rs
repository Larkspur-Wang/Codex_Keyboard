use std::fs;
use std::path::{Path, PathBuf};

use easy_codex_host::audio::{decode_wav, encode_tts_audio, transcode_eiad_for_device};
use easy_codex_host::dashscope::{DashScopeTtsClient, TTS_MODEL, TtsRequest};
use easy_codex_host::paths::AppPaths;
use easy_codex_host::secrets::{DashScopeEnvStore, ImportLock, KeychainAccounts};
use easy_codex_host::summary_orchestrator::SUMMARY_TTS_VOICE;
use serde::Serialize;
use sha2::{Digest, Sha256};

const INSTRUCTIONS: &str =
    "请用自然、清晰、简短的普通话女声播报设备当前音量。语速中等，不要添加原文以外的内容。";

#[derive(Serialize)]
struct PromptReceipt {
    level: u8,
    text: String,
    model: &'static str,
    voice: &'static str,
    sample_rate: u32,
    samples: u64,
    encoded_bytes: usize,
    sha256: String,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let output = std::env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .ok_or("usage: generate_volume_prompts <output-directory> [wav-source-directory]")?;
    let wav_source = std::env::args_os().nth(2).map(PathBuf::from);
    fs::create_dir_all(&output)?;

    let runtime = if wav_source.is_none() {
        let home = std::env::var_os("HOME")
            .map(PathBuf::from)
            .ok_or("HOME is not set")?;
        let paths = AppPaths::from_home(&home);
        paths.prepare()?;
        let lock = ImportLock::acquire(&paths.runtime_directory.join("key-import.lock"))?;
        let accounts = KeychainAccounts::load_or_create(&paths.installation_id, &lock)?;
        let secrets = DashScopeEnvStore::new(paths.dashscope_env, &accounts);
        Some((lock, accounts, secrets, DashScopeTtsClient::default()))
    } else {
        None
    };
    let mut receipts = Vec::new();

    for level in 1_u8..=10 {
        let percent = u16::from(level) * 10;
        let text = format!("当前音量百分之{percent}。");
        let pcm = if let Some(source) = wav_source.as_ref() {
            let wav = fs::read(source.join(format!("volume_{percent}.wav")))?;
            decode_wav(&wav)?
        } else {
            eprintln!("generating level={level} text={text}");
            let (_, accounts, secrets, client) = runtime.as_ref().expect("runtime exists");
            client
                .synthesize(
                    secrets,
                    accounts,
                    TtsRequest {
                        text: &text,
                        voice: SUMMARY_TTS_VOICE,
                        instructions: INSTRUCTIONS,
                    },
                )?
                .pcm()
                .to_vec()
                .into()
        };
        let host = encode_tts_audio(pcm.as_ref())?;
        let device = transcode_eiad_for_device(host.eiad())?;
        let path = output.join(format!("volume_{percent}.eiad"));
        write_new(&path, device.as_slice())?;
        receipts.push(PromptReceipt {
            level,
            text,
            model: TTS_MODEL,
            voice: SUMMARY_TTS_VOICE,
            sample_rate: 48_000,
            samples: u64::try_from(pcm.len() / 2)?,
            encoded_bytes: device.len(),
            sha256: hex_digest(device.as_slice()),
        });
    }

    let manifest = serde_json::to_vec_pretty(&receipts)?;
    write_new(&output.join("manifest.json"), &manifest)?;
    Ok(())
}

fn write_new(path: &Path, bytes: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
    if path.exists() {
        return Err(format!("refusing to overwrite {}", path.display()).into());
    }
    fs::write(path, bytes)?;
    Ok(())
}

fn hex_digest(bytes: &[u8]) -> String {
    Sha256::digest(bytes)
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect()
}

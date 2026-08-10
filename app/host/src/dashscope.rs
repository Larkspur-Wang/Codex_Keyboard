use std::collections::BTreeSet;
use std::env;
use std::ffi::OsString;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::{Arc, mpsc};
use std::time::{Duration, Instant};

use base64::Engine;
use base64::engine::general_purpose::STANDARD as BASE64;
use rustls::pki_types::ServerName;
use rustls::{ClientConfig, ClientConnection, RootCertStore, StreamOwned};
use sha1::{Digest, Sha1};
use thiserror::Error;
use uuid::Uuid;
use zeroize::{Zeroize, Zeroizing};

use crate::audio::{MAX_TTS_SECONDS, TTS_SAMPLE_RATE};
use crate::secrets::{
    CredentialVerifier, KeychainAccounts, SecretBytes, SecretStore, SecretStoreError,
    VerificationError, VerificationReceipt, dashscope_key_is_installed,
};

pub const BEIJING_REALTIME_ENDPOINT: &str =
    "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-tts-instruct-flash-realtime";
pub const TTS_MODEL: &str = "qwen3-tts-instruct-flash-realtime";
pub const TTS_MODEL_SNAPSHOT: &str = "qwen3-tts-instruct-flash-realtime-2026-01-22";
pub const ASR_MODEL: &str = "qwen3-asr-flash";
pub const ASR_MODEL_SNAPSHOT: &str = "qwen3-asr-flash-2026-02-10";
const HOST: &str = "dashscope.aliyuncs.com";
const PORT: u16 = 443;
const REQUEST_TARGET: &str = "/api-ws/v1/realtime?model=qwen3-tts-instruct-flash-realtime";
const ASR_REQUEST_TARGET: &str = "/compatible-mode/v1/chat/completions";
const MAX_HTTP_HEADER_BYTES: usize = 16 * 1024;
const MAX_PROXY_HEADER_BYTES: usize = 8 * 1024;
const MAX_SERVER_EVENT_BYTES: usize = 64 * 1024;
const MAX_TTS_TEXT_BYTES: usize = 64 * 1024;
const MAX_TTS_INSTRUCTIONS_BYTES: usize = 16 * 1024;
const MAX_TTS_PCM_BYTES: usize = TTS_SAMPLE_RATE as usize * MAX_TTS_SECONDS as usize * 2;
const MAX_TTS_ATTEMPTS: u8 = 3;
const MAX_TTS_CHUNK_CHARS: usize = 40;
const MAX_TTS_SERVER_EVENTS: usize = 32_768;
const MAX_TTS_AUDIO_DELTAS: usize = 16_384;
const MAX_ASR_WAV_BYTES: usize = 16_000 * 2 * 90 + 44;
const MAX_ASR_RESPONSE_BYTES: usize = 128 * 1024;
const MAX_ASR_TRANSCRIPT_BYTES: usize = 32 * 1024;

pub struct DashScopeHandshake {
    timeout: Duration,
    #[cfg(test)]
    test_platform: Option<TestPlatform>,
}

impl Default for DashScopeHandshake {
    fn default() -> Self {
        Self {
            timeout: Duration::from_secs(12),
            #[cfg(test)]
            test_platform: None,
        }
    }
}

#[derive(Clone)]
struct HandshakeTarget {
    host: String,
    port: u16,
    request_target: String,
}

impl HandshakeTarget {
    fn production() -> Self {
        Self {
            host: HOST.to_owned(),
            port: PORT,
            request_target: REQUEST_TARGET.to_owned(),
        }
    }
}

struct PreparedPlatform {
    target: HandshakeTarget,
    tls_config: Arc<ClientConfig>,
    proxy: Option<ProxyEndpoint>,
}

#[cfg(test)]
#[derive(Clone)]
struct TestPlatform {
    target: HandshakeTarget,
    tls_config: Arc<ClientConfig>,
    proxy: Option<ProxyEndpoint>,
    setup_delay: Duration,
}

impl CredentialVerifier for DashScopeHandshake {
    fn verify(&self, secret: &SecretBytes) -> Result<VerificationReceipt, VerificationError> {
        let deadline = Instant::now() + self.timeout;
        let platform = self.prepare_platform(deadline)?;
        let (tcp, transport) = connect_transport(deadline, &platform.target, platform.proxy)?;
        let mut stream = start_tls(tcp, deadline, platform.tls_config, &platform.target.host)?;
        let mut pending = authorize_websocket(&mut stream, secret, &platform.target)?;
        let mut fragmented_text = None;

        loop {
            let frame = read_server_frame(&mut stream, &mut pending)?;
            match frame.opcode {
                0x0 | 0x1 => {
                    if let Some(payload) = assemble_text_frame(frame, &mut fragmented_text)? {
                        match classify_server_event(&payload)? {
                            ServerEvent::Created { event_id, model } => {
                                write_masked_close(&mut stream, &1000_u16.to_be_bytes())?;
                                let receipt = VerificationReceipt {
                                    event_id,
                                    region: "cn-beijing",
                                    model,
                                    server_event: "session.created",
                                    transport: transport.as_str(),
                                };
                                await_peer_close(&mut stream, &mut pending)?;
                                return Ok(receipt);
                            }
                            ServerEvent::Other => {}
                        }
                    }
                }
                0x8 => {
                    validate_peer_close_payload(&frame.payload)?;
                    write_masked_close(&mut stream, &frame.payload)?;
                    return Err(VerificationError::Protocol);
                }
                0x9 => write_masked_pong(&mut stream, &frame.payload)?,
                0xA => {}
                _ => return Err(VerificationError::Protocol),
            }
        }
    }
}

impl DashScopeHandshake {
    fn prepare_platform(&self, deadline: Instant) -> Result<PreparedPlatform, VerificationError> {
        #[cfg(test)]
        if let Some(platform) = self.test_platform.clone() {
            return run_blocking_with_deadline(deadline, move || {
                std::thread::sleep(platform.setup_delay);
                Ok(PreparedPlatform {
                    target: platform.target,
                    tls_config: platform.tls_config,
                    proxy: platform.proxy,
                })
            });
        }
        prepare_platform(deadline)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TtsRequest<'a> {
    pub text: &'a str,
    pub voice: &'a str,
    pub instructions: &'a str,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TtsReceipt {
    pub model: &'static str,
    pub voice: String,
    pub sample_rate: u32,
    pub samples: u64,
    pub characters: Option<u64>,
    pub transport: &'static str,
    pub attempts: u8,
}

pub struct TtsAudio {
    pcm: Zeroizing<Vec<u8>>,
    receipt: TtsReceipt,
}

impl TtsAudio {
    pub fn pcm(&self) -> &[u8] {
        self.pcm.as_slice()
    }

    pub const fn receipt(&self) -> &TtsReceipt {
        &self.receipt
    }

    #[cfg(test)]
    pub(crate) fn from_test(pcm: Vec<u8>, receipt: TtsReceipt) -> Self {
        Self {
            pcm: Zeroizing::new(pcm),
            receipt,
        }
    }
}

#[derive(Debug, Error, Clone, Copy, PartialEq, Eq)]
pub enum AsrError {
    #[error("ASR audio is empty, malformed, or exceeds its limit")]
    InvalidAudio,
    #[error("DashScope rejected the credential")]
    Rejected,
    #[error("DashScope rate limited the request")]
    RateLimited,
    #[error("DashScope or its transport is unavailable")]
    Unavailable,
    #[error("DashScope returned an invalid ASR response")]
    Protocol,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AsrTranscript {
    pub text: String,
    pub model: &'static str,
    pub transport: &'static str,
}

pub struct DashScopeAsrClient {
    timeout: Duration,
    #[cfg(test)]
    test_platform: Option<TestPlatform>,
}

impl Default for DashScopeAsrClient {
    fn default() -> Self {
        Self {
            timeout: Duration::from_secs(45),
            #[cfg(test)]
            test_platform: None,
        }
    }
}

#[derive(serde::Serialize)]
struct AsrRequest<'a> {
    model: &'static str,
    messages: [AsrMessage<'a>; 1],
    stream: bool,
    asr_options: AsrOptions,
}

#[derive(serde::Serialize)]
struct AsrMessage<'a> {
    role: &'static str,
    content: [AsrContent<'a>; 1],
}

#[derive(serde::Serialize)]
struct AsrContent<'a> {
    r#type: &'static str,
    input_audio: AsrAudio<'a>,
}

#[derive(serde::Serialize)]
struct AsrAudio<'a> {
    data: &'a str,
}

#[derive(serde::Serialize)]
struct AsrOptions {
    language: &'static str,
    enable_itn: bool,
}

impl DashScopeAsrClient {
    pub fn transcribe_wav(
        &self,
        secret: &SecretBytes,
        wav: &[u8],
    ) -> Result<AsrTranscript, AsrError> {
        if wav.len() < 44
            || wav.len() > MAX_ASR_WAV_BYTES
            || &wav[..4] != b"RIFF"
            || &wav[8..12] != b"WAVE"
        {
            return Err(AsrError::InvalidAudio);
        }
        let mut data_uri = Zeroizing::new(String::with_capacity(24 + wav.len() * 4 / 3 + 4));
        data_uri.push_str("data:audio/wav;base64,");
        BASE64.encode_string(wav, &mut data_uri);
        let request = AsrRequest {
            model: ASR_MODEL,
            messages: [AsrMessage {
                role: "user",
                content: [AsrContent {
                    r#type: "input_audio",
                    input_audio: AsrAudio { data: &data_uri },
                }],
            }],
            stream: false,
            asr_options: AsrOptions {
                language: "zh",
                enable_itn: true,
            },
        };
        let body = Zeroizing::new(serde_json::to_vec(&request).map_err(|_| AsrError::Protocol)?);
        let deadline = Instant::now() + self.timeout;
        let platform = self.prepare_asr_platform(deadline)?;
        let (tcp, transport) = connect_transport(deadline, &platform.target, platform.proxy)
            .map_err(map_asr_transport_error)?;
        let mut stream = start_tls(tcp, deadline, platform.tls_config, &platform.target.host)
            .map_err(map_asr_transport_error)?;
        let response = post_asr_request(&mut stream, secret, &platform.target, &body)?;
        let value: serde_json::Value =
            serde_json::from_slice(&response).map_err(|_| AsrError::Protocol)?;
        let text = value
            .pointer("/choices/0/message/content")
            .and_then(serde_json::Value::as_str)
            .map(str::trim)
            .filter(|text| {
                !text.is_empty()
                    && text.len() <= MAX_ASR_TRANSCRIPT_BYTES
                    && !text.chars().any(|character| {
                        character.is_control() && !matches!(character, '\n' | '\r' | '\t')
                    })
            })
            .ok_or(AsrError::Protocol)?
            .split_whitespace()
            .collect::<Vec<_>>()
            .join(" ");
        if text.is_empty() {
            return Err(AsrError::Protocol);
        }
        Ok(AsrTranscript {
            text,
            model: ASR_MODEL,
            transport: transport.as_str(),
        })
    }

    fn prepare_asr_platform(&self, deadline: Instant) -> Result<PreparedPlatform, AsrError> {
        #[cfg(test)]
        if let Some(platform) = self.test_platform.clone() {
            return run_blocking_with_deadline(deadline, move || {
                std::thread::sleep(platform.setup_delay);
                Ok(PreparedPlatform {
                    target: platform.target,
                    tls_config: platform.tls_config,
                    proxy: platform.proxy,
                })
            })
            .map_err(map_asr_transport_error);
        }
        run_blocking_with_deadline(deadline, || {
            Ok(PreparedPlatform {
                target: HandshakeTarget {
                    host: HOST.to_owned(),
                    port: PORT,
                    request_target: ASR_REQUEST_TARGET.to_owned(),
                },
                tls_config: build_tls_config()?,
                proxy: proxy_from_environment()?,
            })
        })
        .map_err(map_asr_transport_error)
    }
}

fn map_asr_transport_error(error: VerificationError) -> AsrError {
    match error {
        VerificationError::Rejected => AsrError::Rejected,
        VerificationError::Unavailable => AsrError::Unavailable,
        VerificationError::Protocol => AsrError::Protocol,
    }
}

fn post_asr_request(
    stream: &mut DeadlineTls,
    secret: &SecretBytes,
    target: &HandshakeTarget,
    body: &[u8],
) -> Result<Zeroizing<Vec<u8>>, AsrError> {
    let mut request = Zeroizing::new(Vec::with_capacity(
        512 + secret.as_slice().len() + body.len(),
    ));
    request.extend_from_slice(b"POST ");
    request.extend_from_slice(target.request_target.as_bytes());
    request.extend_from_slice(b" HTTP/1.1\r\nHost: ");
    request.extend_from_slice(target.host.as_bytes());
    if target.port != 443 {
        request.extend_from_slice(b":");
        request.extend_from_slice(target.port.to_string().as_bytes());
    }
    request.extend_from_slice(b"\r\nAuthorization: Bearer ");
    request.extend_from_slice(secret.as_slice());
    request.extend_from_slice(b"\r\nContent-Type: application/json\r\nConnection: close\r\n");
    request.extend_from_slice(b"User-Agent: easy-codex-input/0.1\r\nContent-Length: ");
    request.extend_from_slice(body.len().to_string().as_bytes());
    request.extend_from_slice(b"\r\n\r\n");
    request.extend_from_slice(body);
    stream
        .write_all(&request)
        .and_then(|()| stream.flush())
        .map_err(|_| AsrError::Unavailable)?;

    let mut response = Zeroizing::new(Vec::with_capacity(4096));
    let header_end = loop {
        match scan_http_header(&response, MAX_HTTP_HEADER_BYTES) {
            HeaderScan::Complete(end) => break end,
            HeaderScan::TooLarge => return Err(AsrError::Protocol),
            HeaderScan::NeedMore => {}
        }
        let mut chunk = [0_u8; 4096];
        let read = stream.read(&mut chunk).map_err(|_| AsrError::Unavailable)?;
        if read == 0 {
            return Err(AsrError::Protocol);
        }
        response.extend_from_slice(&chunk[..read]);
    };
    let body_length = validate_asr_http_header(&response[..header_end])?;
    if body_length > MAX_ASR_RESPONSE_BYTES {
        return Err(AsrError::Protocol);
    }
    while response.len() < header_end + body_length {
        let remaining = header_end + body_length - response.len();
        let mut chunk = [0_u8; 4096];
        let read_limit = remaining.min(chunk.len());
        let read = stream
            .read(&mut chunk[..read_limit])
            .map_err(|_| AsrError::Unavailable)?;
        if read == 0 {
            return Err(AsrError::Protocol);
        }
        response.extend_from_slice(&chunk[..read]);
    }
    if response.len() != header_end + body_length {
        return Err(AsrError::Protocol);
    }
    Ok(Zeroizing::new(response[header_end..].to_vec()))
}

fn validate_asr_http_header(header: &[u8]) -> Result<usize, AsrError> {
    let mut headers = [httparse::EMPTY_HEADER; 32];
    let mut parsed = httparse::Response::new(&mut headers);
    if !parsed
        .parse(header)
        .map_err(|_| AsrError::Protocol)?
        .is_complete()
        || parsed.version != Some(1)
    {
        return Err(AsrError::Protocol);
    }
    match parsed.code {
        Some(200) => {}
        Some(401 | 403) => return Err(AsrError::Rejected),
        Some(429) => return Err(AsrError::RateLimited),
        Some(500..=599) => return Err(AsrError::Unavailable),
        _ => return Err(AsrError::Protocol),
    }
    if unique_header_value(&parsed, "transfer-encoding")
        .map_err(|_| AsrError::Protocol)?
        .is_some()
    {
        return Err(AsrError::Protocol);
    }
    let length = unique_header_value(&parsed, "content-length")
        .map_err(|_| AsrError::Protocol)?
        .ok_or(AsrError::Protocol)?;
    let length = std::str::from_utf8(length)
        .ok()
        .and_then(|value| value.parse::<usize>().ok())
        .ok_or(AsrError::Protocol)?;
    Ok(length)
}

#[derive(Debug, Error, Clone, Copy, PartialEq, Eq)]
pub enum TtsError {
    #[error("DashScope credential is not installed or could not be read")]
    Credential,
    #[error("TTS request is empty, malformed, or exceeds its limit")]
    InvalidRequest,
    #[error("DashScope rejected the credential")]
    Rejected,
    #[error("DashScope rate limited the request")]
    RateLimited,
    #[error("DashScope or its transport is unavailable")]
    Unavailable,
    #[error("TTS submission may have been accepted; automatic retry is unsafe")]
    AmbiguousAfterCommit,
    #[error("DashScope returned an invalid or out-of-order realtime event")]
    Protocol,
    #[error("DashScope audio exceeded the configured duration limit")]
    AudioLimit,
}

pub struct DashScopeTtsClient {
    timeout: Duration,
    max_attempts: u8,
    retry_backoff: Duration,
    #[cfg(test)]
    test_platform: Option<TestPlatform>,
}

impl Default for DashScopeTtsClient {
    fn default() -> Self {
        Self {
            timeout: Duration::from_secs(120),
            max_attempts: MAX_TTS_ATTEMPTS,
            retry_backoff: Duration::from_millis(250),
            #[cfg(test)]
            test_platform: None,
        }
    }
}

impl DashScopeTtsClient {
    pub fn synthesize<S: SecretStore>(
        &self,
        store: &S,
        accounts: &KeychainAccounts,
        request: TtsRequest<'_>,
    ) -> Result<TtsAudio, TtsError> {
        validate_tts_request(&request)?;
        let installed = dashscope_key_is_installed(store, accounts).map_err(map_secret_error)?;
        if !installed {
            return Err(TtsError::Credential);
        }
        let secret = store
            .get(&accounts.dashscope)
            .map_err(map_secret_error)?
            .ok_or(TtsError::Credential)?;

        let attempts = self.max_attempts.clamp(1, MAX_TTS_ATTEMPTS);
        for attempt in 1..=attempts {
            match self.synthesize_once(&secret, &request) {
                Ok(mut audio) => {
                    audio.receipt.attempts = attempt;
                    return Ok(audio);
                }
                Err(failure) if failure.retryable && attempt < attempts => {
                    let multiplier = u32::from(attempt);
                    std::thread::sleep(self.retry_backoff.saturating_mul(multiplier));
                }
                Err(failure) => return Err(failure.error),
            }
        }
        Err(TtsError::Unavailable)
    }

    pub fn synthesize_chunked<S: SecretStore>(
        &self,
        store: &S,
        accounts: &KeychainAccounts,
        request: TtsRequest<'_>,
    ) -> Result<TtsAudio, TtsError> {
        let chunks = split_tts_text(request.text, MAX_TTS_CHUNK_CHARS);
        let mut synthesized = Vec::with_capacity(chunks.len());
        for chunk in &chunks {
            synthesized.push(self.synthesize(
                store,
                accounts,
                TtsRequest {
                    text: chunk,
                    voice: request.voice,
                    instructions: request.instructions,
                },
            )?);
        }
        combine_tts_audio(synthesized)
    }

    fn synthesize_once(
        &self,
        secret: &SecretBytes,
        request: &TtsRequest<'_>,
    ) -> Result<TtsAudio, AttemptFailure> {
        let deadline = Instant::now() + self.timeout;
        let platform = self.prepare_tts_platform(deadline).map_err(|error| {
            if matches!(error, TtsError::Unavailable | TtsError::RateLimited) {
                AttemptFailure::retryable(error)
            } else {
                AttemptFailure::terminal(error)
            }
        })?;
        let (tcp, transport) = connect_transport(deadline, &platform.target, platform.proxy)
            .map_err(AttemptFailure::transport)?;
        let mut stream = start_tls(tcp, deadline, platform.tls_config, &platform.target.host)
            .map_err(AttemptFailure::transport)?;
        let mut pending = authorize_tts_websocket(&mut stream, secret, &platform.target)?;
        run_tts_session(&mut stream, &mut pending, request, transport)
    }

    fn prepare_tts_platform(&self, deadline: Instant) -> Result<PreparedPlatform, TtsError> {
        #[cfg(test)]
        if let Some(platform) = self.test_platform.clone() {
            return run_blocking_with_deadline(deadline, move || {
                std::thread::sleep(platform.setup_delay);
                Ok(PreparedPlatform {
                    target: platform.target,
                    tls_config: platform.tls_config,
                    proxy: platform.proxy,
                })
            })
            .map_err(map_verification_error);
        }
        prepare_platform(deadline).map_err(map_verification_error)
    }
}

fn split_tts_text(text: &str, max_chars: usize) -> Vec<String> {
    debug_assert!(max_chars > 0);
    let mut chunks = Vec::new();
    let mut current = String::new();
    let mut count = 0_usize;
    for character in text.chars() {
        current.push(character);
        count += 1;
        let natural_boundary = matches!(character, '。' | '！' | '？' | '；' | '\n');
        if count >= max_chars || (natural_boundary && count >= max_chars / 2) {
            chunks.push(std::mem::take(&mut current));
            count = 0;
        }
    }
    if !current.is_empty() {
        chunks.push(current);
    }
    chunks
}

fn combine_tts_audio(mut chunks: Vec<TtsAudio>) -> Result<TtsAudio, TtsError> {
    if chunks.is_empty() {
        return Err(TtsError::InvalidRequest);
    }
    let first = chunks.remove(0);
    let mut pcm = Zeroizing::new(Vec::with_capacity(first.pcm.len()));
    pcm.extend_from_slice(&first.pcm);
    let mut receipt = first.receipt;
    for chunk in chunks {
        if chunk.receipt.model != receipt.model
            || chunk.receipt.voice != receipt.voice
            || chunk.receipt.sample_rate != receipt.sample_rate
            || chunk.receipt.transport != receipt.transport
        {
            return Err(TtsError::Protocol);
        }
        let combined_length = pcm
            .len()
            .checked_add(chunk.pcm.len())
            .ok_or(TtsError::AudioLimit)?;
        if combined_length > MAX_TTS_PCM_BYTES {
            return Err(TtsError::AudioLimit);
        }
        pcm.extend_from_slice(&chunk.pcm);
        receipt.samples = receipt
            .samples
            .checked_add(chunk.receipt.samples)
            .ok_or(TtsError::AudioLimit)?;
        receipt.characters = receipt
            .characters
            .zip(chunk.receipt.characters)
            .map(|(left, right)| left.saturating_add(right));
        receipt.attempts = receipt.attempts.saturating_add(chunk.receipt.attempts);
    }
    Ok(TtsAudio { pcm, receipt })
}

fn map_secret_error(_: SecretStoreError) -> TtsError {
    TtsError::Credential
}

fn map_verification_error(error: VerificationError) -> TtsError {
    match error {
        VerificationError::Rejected => TtsError::Rejected,
        VerificationError::Unavailable => TtsError::Unavailable,
        VerificationError::Protocol => TtsError::Protocol,
    }
}

struct AttemptFailure {
    error: TtsError,
    retryable: bool,
}

impl AttemptFailure {
    fn terminal(error: TtsError) -> Self {
        Self {
            error,
            retryable: false,
        }
    }

    fn retryable(error: TtsError) -> Self {
        Self {
            error,
            retryable: true,
        }
    }

    fn transport(error: VerificationError) -> Self {
        match error {
            VerificationError::Rejected => Self::terminal(TtsError::Rejected),
            VerificationError::Unavailable => Self::retryable(TtsError::Unavailable),
            VerificationError::Protocol => Self::terminal(TtsError::Protocol),
        }
    }
}

fn validate_tts_request(request: &TtsRequest<'_>) -> Result<(), TtsError> {
    let valid_text = !request.text.is_empty()
        && request.text.len() <= MAX_TTS_TEXT_BYTES
        && !request.text.chars().any(char::is_control);
    let valid_voice = !request.voice.is_empty()
        && request.voice.len() <= 64
        && request
            .voice
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b' ' | b'-' | b'_'));
    let valid_instructions = request.instructions.len() <= MAX_TTS_INSTRUCTIONS_BYTES
        && !request.instructions.chars().any(char::is_control);
    if valid_text && valid_voice && valid_instructions {
        Ok(())
    } else {
        Err(TtsError::InvalidRequest)
    }
}

fn prepare_platform(deadline: Instant) -> Result<PreparedPlatform, VerificationError> {
    run_blocking_with_deadline(deadline, || {
        Ok(PreparedPlatform {
            target: HandshakeTarget::production(),
            tls_config: build_tls_config()?,
            proxy: proxy_from_environment()?,
        })
    })
}

fn run_blocking_with_deadline<T, F>(deadline: Instant, operation: F) -> Result<T, VerificationError>
where
    T: Send + 'static,
    F: FnOnce() -> Result<T, VerificationError> + Send + 'static,
{
    let remaining = deadline
        .checked_duration_since(Instant::now())
        .ok_or(VerificationError::Unavailable)?;
    let (sender, receiver) = mpsc::sync_channel(1);
    std::thread::spawn(move || {
        let _ = sender.send(operation());
    });
    receiver
        .recv_timeout(remaining)
        .map_err(|_| VerificationError::Unavailable)?
}

fn connect_transport(
    deadline: Instant,
    target: &HandshakeTarget,
    proxy: Option<ProxyEndpoint>,
) -> Result<(TcpStream, TransportRoute), VerificationError> {
    let (host, port) = proxy
        .as_ref()
        .map_or((target.host.clone(), target.port), |proxy| {
            (proxy.host.clone(), proxy.port)
        });
    let addresses = resolve_with_deadline(host, port, deadline)?;
    let stream = connect_addresses(&addresses, deadline)?;
    let route = if proxy.is_some() {
        establish_proxy_tunnel(&stream, deadline, target)?;
        TransportRoute::HttpsProxy
    } else {
        TransportRoute::Direct
    };
    Ok((stream, route))
}

#[derive(Debug, Clone, Copy)]
enum TransportRoute {
    Direct,
    HttpsProxy,
}

impl TransportRoute {
    const fn as_str(self) -> &'static str {
        match self {
            Self::Direct => "direct",
            Self::HttpsProxy => "https_proxy",
        }
    }
}

fn resolve_with_deadline(
    host: String,
    port: u16,
    deadline: Instant,
) -> Result<Vec<SocketAddr>, VerificationError> {
    let remaining = deadline
        .checked_duration_since(Instant::now())
        .ok_or(VerificationError::Unavailable)?;
    let (sender, receiver) = mpsc::sync_channel(1);
    std::thread::spawn(move || {
        let result = (host.as_str(), port)
            .to_socket_addrs()
            .map(|addresses| addresses.collect::<Vec<_>>());
        let _ = sender.send(result);
    });
    receiver
        .recv_timeout(remaining)
        .map_err(|_| VerificationError::Unavailable)?
        .map_err(|_| VerificationError::Unavailable)
}

fn connect_addresses(
    addresses: &[SocketAddr],
    deadline: Instant,
) -> Result<TcpStream, VerificationError> {
    for address in addresses {
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .ok_or(VerificationError::Unavailable)?;
        let attempt = remaining.min(Duration::from_secs(4));
        if let Ok(stream) = TcpStream::connect_timeout(address, attempt) {
            set_socket_deadline(&stream, deadline)?;
            return Ok(stream);
        }
    }
    Err(VerificationError::Unavailable)
}

fn build_tls_config() -> Result<std::sync::Arc<ClientConfig>, VerificationError> {
    let certificate_result = rustls_native_certs::load_native_certs();
    if certificate_result.certs.is_empty() {
        return Err(VerificationError::Unavailable);
    }
    let mut roots = RootCertStore::empty();
    let (added, _) = roots.add_parsable_certificates(certificate_result.certs);
    if added == 0 {
        return Err(VerificationError::Unavailable);
    }
    let config = ClientConfig::builder()
        .with_root_certificates(roots)
        .with_no_client_auth();
    Ok(std::sync::Arc::new(config))
}

fn start_tls(
    stream: TcpStream,
    deadline: Instant,
    config: std::sync::Arc<ClientConfig>,
    host: &str,
) -> Result<DeadlineTls, VerificationError> {
    let server_name =
        ServerName::try_from(host.to_owned()).map_err(|_| VerificationError::Protocol)?;
    let connection =
        ClientConnection::new(config, server_name).map_err(|_| VerificationError::Unavailable)?;
    let socket = DeadlineSocket::new(stream, deadline)?;
    Ok(DeadlineTls {
        inner: StreamOwned::new(connection, socket),
    })
}

fn authorize_websocket(
    stream: &mut DeadlineTls,
    secret: &SecretBytes,
    target: &HandshakeTarget,
) -> Result<Vec<u8>, VerificationError> {
    let websocket_key = Zeroizing::new(BASE64.encode(Uuid::new_v4().as_bytes()));
    let mut request = Zeroizing::new(Vec::with_capacity(512 + secret.as_slice().len()));
    request.extend_from_slice(b"GET ");
    request.extend_from_slice(target.request_target.as_bytes());
    request.extend_from_slice(b" HTTP/1.1\r\nHost: ");
    request.extend_from_slice(target.host.as_bytes());
    if target.port != 443 {
        request.extend_from_slice(b":");
        request.extend_from_slice(target.port.to_string().as_bytes());
    }
    request.extend_from_slice(b"\r\n");
    request.extend_from_slice(b"Connection: Upgrade\r\nUpgrade: websocket\r\n");
    request.extend_from_slice(b"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ");
    request.extend_from_slice(websocket_key.as_bytes());
    request.extend_from_slice(b"\r\nAuthorization: Bearer ");
    request.extend_from_slice(secret.as_slice());
    request.extend_from_slice(b"\r\nUser-Agent: easy-codex-input/0.1\r\n\r\n");

    stream
        .write_all(&request)
        .and_then(|()| stream.flush())
        .map_err(|_| VerificationError::Unavailable)?;

    let mut response = Zeroizing::new(Vec::with_capacity(2048));
    let header_end = loop {
        match scan_http_header(&response, MAX_HTTP_HEADER_BYTES) {
            HeaderScan::Complete(end) => break end,
            HeaderScan::TooLarge => return Err(VerificationError::Protocol),
            HeaderScan::NeedMore => {}
        }
        let mut chunk = [0_u8; 1024];
        let read = stream
            .read(&mut chunk)
            .map_err(|_| VerificationError::Unavailable)?;
        if read == 0 {
            return Err(VerificationError::Protocol);
        }
        response.extend_from_slice(&chunk[..read]);
    };
    validate_handshake_response(&response[..header_end], &websocket_key)?;
    Ok(response[header_end..].to_vec())
}

fn authorize_tts_websocket(
    stream: &mut DeadlineTls,
    secret: &SecretBytes,
    target: &HandshakeTarget,
) -> Result<Vec<u8>, AttemptFailure> {
    let websocket_key = Zeroizing::new(BASE64.encode(Uuid::new_v4().as_bytes()));
    let mut request = Zeroizing::new(Vec::with_capacity(512 + secret.as_slice().len()));
    request.extend_from_slice(b"GET ");
    request.extend_from_slice(target.request_target.as_bytes());
    request.extend_from_slice(b" HTTP/1.1\r\nHost: ");
    request.extend_from_slice(target.host.as_bytes());
    if target.port != 443 {
        request.extend_from_slice(b":");
        request.extend_from_slice(target.port.to_string().as_bytes());
    }
    request.extend_from_slice(b"\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n");
    request.extend_from_slice(b"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ");
    request.extend_from_slice(websocket_key.as_bytes());
    request.extend_from_slice(b"\r\nAuthorization: Bearer ");
    request.extend_from_slice(secret.as_slice());
    request.extend_from_slice(b"\r\nUser-Agent: easy-codex-input/0.1\r\n\r\n");
    stream
        .write_all(&request)
        .and_then(|()| stream.flush())
        .map_err(|_| AttemptFailure::retryable(TtsError::Unavailable))?;

    let mut response = Zeroizing::new(Vec::with_capacity(2048));
    let header_end = loop {
        match scan_http_header(&response, MAX_HTTP_HEADER_BYTES) {
            HeaderScan::Complete(end) => break end,
            HeaderScan::TooLarge => return Err(AttemptFailure::terminal(TtsError::Protocol)),
            HeaderScan::NeedMore => {}
        }
        let mut chunk = [0_u8; 1024];
        let read = stream
            .read(&mut chunk)
            .map_err(|_| AttemptFailure::retryable(TtsError::Unavailable))?;
        if read == 0 {
            return Err(AttemptFailure::retryable(TtsError::Unavailable));
        }
        response.extend_from_slice(&chunk[..read]);
    };
    validate_tts_handshake_response(&response[..header_end], &websocket_key)?;
    Ok(response[header_end..].to_vec())
}

fn validate_tts_handshake_response(
    response: &[u8],
    websocket_key: &str,
) -> Result<(), AttemptFailure> {
    let mut headers = [httparse::EMPTY_HEADER; 32];
    let mut parsed = httparse::Response::new(&mut headers);
    if !parsed
        .parse(response)
        .map_err(|_| AttemptFailure::terminal(TtsError::Protocol))?
        .is_complete()
        || parsed.version != Some(1)
    {
        return Err(AttemptFailure::terminal(TtsError::Protocol));
    }
    match parsed.code {
        Some(101) => {}
        Some(401 | 403) => return Err(AttemptFailure::terminal(TtsError::Rejected)),
        Some(429) => return Err(AttemptFailure::retryable(TtsError::RateLimited)),
        Some(500..=599) => return Err(AttemptFailure::retryable(TtsError::Unavailable)),
        _ => return Err(AttemptFailure::terminal(TtsError::Protocol)),
    }
    let upgrade = unique_header_value(&parsed, "upgrade")
        .map_err(|_| AttemptFailure::terminal(TtsError::Protocol))?
        .is_some_and(|value| value.eq_ignore_ascii_case(b"websocket"));
    let connection = unique_header_value(&parsed, "connection")
        .map_err(|_| AttemptFailure::terminal(TtsError::Protocol))?
        .is_some_and(|value| {
            value
                .split(|byte| *byte == b',')
                .any(|token| token.trim_ascii().eq_ignore_ascii_case(b"upgrade"))
        });
    let expected_accept = websocket_accept(websocket_key);
    let accept = unique_header_value(&parsed, "sec-websocket-accept")
        .map_err(|_| AttemptFailure::terminal(TtsError::Protocol))?
        .is_some_and(|value| value == expected_accept.as_bytes());
    let unsolicited_negotiation = parsed.headers.iter().any(|header| {
        header.name.eq_ignore_ascii_case("sec-websocket-extensions")
            || header.name.eq_ignore_ascii_case("sec-websocket-protocol")
    });
    if upgrade && connection && accept && !unsolicited_negotiation {
        Ok(())
    } else {
        Err(AttemptFailure::terminal(TtsError::Protocol))
    }
}

struct TtsSessionState {
    model: &'static str,
    session_id: String,
    response_id: Option<String>,
    item_id: Option<String>,
    committed: bool,
    request_submitted: bool,
    output_item_added: bool,
    content_part_added: bool,
    audio_done: bool,
    content_part_done: bool,
    output_item_done: bool,
    response_done: bool,
    received_audio: bool,
    characters: Option<u64>,
    server_event_ids: BTreeSet<String>,
    server_events: usize,
    audio_deltas: usize,
    pcm: Zeroizing<Vec<u8>>,
}

fn run_tts_session<S: Read + Write>(
    stream: &mut S,
    pending: &mut Vec<u8>,
    request: &TtsRequest<'_>,
    transport: TransportRoute,
) -> Result<TtsAudio, AttemptFailure> {
    let mut fragmented = None;
    let created = read_tts_event(stream, pending, &mut fragmented, false)?;
    let (model, session_id, event_id) = parse_created(&created)?;
    let mut state = TtsSessionState {
        model,
        session_id,
        response_id: None,
        item_id: None,
        committed: false,
        request_submitted: false,
        output_item_added: false,
        content_part_added: false,
        audio_done: false,
        content_part_done: false,
        output_item_done: false,
        response_done: false,
        received_audio: false,
        characters: None,
        server_event_ids: BTreeSet::from([event_id]),
        server_events: 1,
        audio_deltas: 0,
        pcm: Zeroizing::new(Vec::new()),
    };

    send_client_event(
        stream,
        serde_json::json!({
            "event_id": client_event_id(),
            "type": "session.update",
            "session": {
                "voice": request.voice,
                "mode": "commit",
                "language_type": "Auto",
                "response_format": "pcm",
                "sample_rate": TTS_SAMPLE_RATE,
                "instructions": request.instructions,
                "optimize_instructions": false
            }
        }),
    )
    .map_err(retry_before_commit)?;
    let updated = read_tts_event(stream, pending, &mut fragmented, false)?;
    register_event(&mut state, &updated)?;
    validate_updated(&updated, &state, request.voice)?;

    send_client_event(
        stream,
        serde_json::json!({
            "event_id": client_event_id(),
            "type": "input_text_buffer.append",
            "text": request.text
        }),
    )
    .map_err(retry_before_commit)?;
    // A failed commit write may still have reached the service. From this point onward the
    // caller must reconcile rather than retry and risk duplicate synthesis or billing.
    state.request_submitted = true;
    send_client_event(
        stream,
        serde_json::json!({
            "event_id": client_event_id(),
            "type": "input_text_buffer.commit"
        }),
    )
    .map_err(|_| {
        eprintln!("tts_session=failed phase=commit_write");
        ambiguous_after_commit()
    })?;

    while !state.response_done {
        // After commit, an ambiguous disconnect must not retry and risk duplicate synthesis/billing.
        let mut event = read_tts_event(stream, pending, &mut fragmented, true)
            .map_err(|error| trace_tts_failure("response_read", &state, error))?;
        register_event(&mut state, &event)
            .map_err(|error| trace_tts_failure("event_registration", &state, error))?;
        let event_type = event
            .get("type")
            .and_then(serde_json::Value::as_str)
            .ok_or_else(|| trace_tts_failure("event_type", &state, protocol_failure(true)))?
            .to_owned();
        let handled = match event_type.as_str() {
            "input_text_buffer.committed" => {
                if state.committed || state.response_id.is_some() {
                    Err(protocol_failure(state.request_submitted))
                } else {
                    require_service_id(event.get("item_id"), true, state.request_submitted)?;
                    state.committed = true;
                    Ok(())
                }
            }
            "response.created" => parse_response_created(&event, &mut state, request.voice),
            "response.output_item.added"
            | "response.content_part.added"
            | "response.content_part.done"
            | "response.output_item.done" => validate_response_scaffold(&event, &mut state),
            "response.audio.delta" => append_audio_delta(&mut event, &mut state),
            "response.audio.done" => mark_audio_done(&event, &mut state),
            "response.done" => mark_response_done(&event, &mut state),
            "error" => Err(classify_tts_service_error(&event, state.request_submitted)),
            _ => Err(protocol_failure(state.request_submitted)),
        };
        handled.map_err(|error| trace_tts_failure(&event_type, &state, error))?;
    }

    // response.done already proves the complete PCM and usage were received. Session teardown is
    // best-effort so a proxy closing the completed WebSocket cannot strand a valid synthesis.
    let finish_sent = send_client_event(
        stream,
        serde_json::json!({
            "event_id": client_event_id(),
            "type": "session.finish"
        }),
    )
    .is_ok();
    if finish_sent && let Ok(finished) = read_tts_event(stream, pending, &mut fragmented, true) {
        let _ = register_event(&mut state, &finished);
    }
    let _ = write_masked_close(stream, &1000_u16.to_be_bytes());
    let _ = await_peer_close(stream, pending);

    let samples = u64::try_from(state.pcm.len() / 2).map_err(|_| ambiguous_after_commit())?;
    Ok(TtsAudio {
        pcm: state.pcm,
        receipt: TtsReceipt {
            model: state.model,
            voice: request.voice.to_owned(),
            sample_rate: TTS_SAMPLE_RATE,
            samples,
            characters: state.characters,
            transport: transport.as_str(),
            attempts: 1,
        },
    })
}

fn trace_tts_failure(
    phase: &str,
    state: &TtsSessionState,
    failure: AttemptFailure,
) -> AttemptFailure {
    eprintln!(
        "tts_session=failed phase={phase} committed={} response_created={} audio_received={} audio_done={} content_done={} item_done={} response_done={} deltas={} pcm_bytes={}",
        state.committed,
        state.response_id.is_some(),
        state.received_audio,
        state.audio_done,
        state.content_part_done,
        state.output_item_done,
        state.response_done,
        state.audio_deltas,
        state.pcm.len()
    );
    failure
}

fn read_tts_event<S: Read + Write>(
    stream: &mut S,
    pending: &mut Vec<u8>,
    fragmented: &mut Option<Vec<u8>>,
    request_submitted: bool,
) -> Result<SensitiveJson, AttemptFailure> {
    loop {
        let frame = read_server_frame(stream, pending).map_err(|error| match error {
            VerificationError::Unavailable if !request_submitted => {
                AttemptFailure::retryable(TtsError::Unavailable)
            }
            VerificationError::Unavailable => ambiguous_after_commit(),
            _ => protocol_failure(request_submitted),
        })?;
        match frame.opcode {
            0x0 | 0x1 => {
                if let Some(payload) = assemble_text_frame(frame, fragmented)
                    .map_err(|_| protocol_failure(request_submitted))?
                {
                    let payload = Zeroizing::new(payload);
                    return serde_json::from_slice(&payload)
                        .map(SensitiveJson)
                        .map_err(|_| protocol_failure(request_submitted));
                }
            }
            0x8 => {
                let close_code = if frame.payload.len() >= 2 {
                    u16::from_be_bytes([frame.payload[0], frame.payload[1]])
                } else {
                    0
                };
                eprintln!("tts_session=peer_close code={close_code}");
                return Err(protocol_failure(request_submitted));
            }
            0x9 => write_masked_pong(stream, &frame.payload).map_err(|_| {
                if request_submitted {
                    ambiguous_after_commit()
                } else {
                    AttemptFailure::retryable(TtsError::Unavailable)
                }
            })?,
            0xA => {}
            _ => return Err(protocol_failure(request_submitted)),
        }
    }
}

fn client_event_id() -> String {
    format!("event_{}", Uuid::new_v4().simple())
}

fn send_client_event<S: Write>(
    stream: &mut S,
    mut event: serde_json::Value,
) -> Result<(), AttemptFailure> {
    let encoded = serde_json::to_vec(&event);
    zeroize_json(&mut event);
    let payload =
        Zeroizing::new(encoded.map_err(|_| AttemptFailure::terminal(TtsError::Protocol))?);
    if payload.len() > MAX_SERVER_EVENT_BYTES {
        return Err(AttemptFailure::terminal(TtsError::InvalidRequest));
    }
    write_masked_text(stream, &payload)
}

fn write_masked_text<W: Write>(writer: &mut W, payload: &[u8]) -> Result<(), AttemptFailure> {
    let length = payload.len();
    let mut frame = Zeroizing::new(Vec::with_capacity(length.saturating_add(14)));
    frame.push(0x81);
    if length < 126 {
        frame.push(0x80 | length as u8);
    } else if let Ok(length) = u16::try_from(length) {
        frame.push(0x80 | 126);
        frame.extend_from_slice(&length.to_be_bytes());
    } else {
        frame.push(0x80 | 127);
        frame.extend_from_slice(&(length as u64).to_be_bytes());
    }
    let mask_source = Uuid::new_v4();
    let mask = &mask_source.as_bytes()[..4];
    frame.extend_from_slice(mask);
    frame.extend(
        payload
            .iter()
            .enumerate()
            .map(|(index, byte)| byte ^ mask[index % 4]),
    );
    writer
        .write_all(&frame)
        .and_then(|()| writer.flush())
        .map_err(|_| AttemptFailure::terminal(TtsError::Unavailable))
}

struct SensitiveJson(serde_json::Value);

impl std::ops::Deref for SensitiveJson {
    type Target = serde_json::Value;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl std::ops::DerefMut for SensitiveJson {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}

impl Drop for SensitiveJson {
    fn drop(&mut self) {
        zeroize_json(&mut self.0);
    }
}

fn zeroize_json(value: &mut serde_json::Value) {
    match value {
        serde_json::Value::String(value) => value.zeroize(),
        serde_json::Value::Array(values) => values.iter_mut().for_each(zeroize_json),
        serde_json::Value::Object(values) => values.values_mut().for_each(zeroize_json),
        serde_json::Value::Null | serde_json::Value::Bool(_) | serde_json::Value::Number(_) => {}
    }
}

fn parse_created(
    event: &serde_json::Value,
) -> Result<(&'static str, String, String), AttemptFailure> {
    if event.get("type").and_then(serde_json::Value::as_str) != Some("session.created") {
        return Err(AttemptFailure::terminal(TtsError::Protocol));
    }
    let event_id = service_id(event.get("event_id"), false)?;
    let model = match event
        .pointer("/session/model")
        .and_then(serde_json::Value::as_str)
    {
        Some(TTS_MODEL) => TTS_MODEL,
        Some(TTS_MODEL_SNAPSHOT) => TTS_MODEL_SNAPSHOT,
        _ => return Err(AttemptFailure::terminal(TtsError::Protocol)),
    };
    let session_id = service_id(event.pointer("/session/id"), false)?;
    Ok((model, session_id, event_id))
}

fn register_event(
    state: &mut TtsSessionState,
    event: &serde_json::Value,
) -> Result<(), AttemptFailure> {
    if state.server_events >= MAX_TTS_SERVER_EVENTS {
        return Err(protocol_failure(state.request_submitted));
    }
    let event_id = service_id(event.get("event_id"), state.request_submitted)?;
    if !state.server_event_ids.insert(event_id) {
        return Err(protocol_failure(state.request_submitted));
    }
    state.server_events += 1;
    Ok(())
}

fn validate_updated(
    event: &serde_json::Value,
    state: &TtsSessionState,
    voice: &str,
) -> Result<(), AttemptFailure> {
    let session = event
        .get("session")
        .ok_or_else(|| protocol_failure(false))?;
    let model = session.get("model").and_then(serde_json::Value::as_str);
    if event.get("type").and_then(serde_json::Value::as_str) != Some("session.updated")
        || session.get("id").and_then(serde_json::Value::as_str) != Some(&state.session_id)
        || !matches!(model, Some(value) if value == state.model)
        || session.get("voice").and_then(serde_json::Value::as_str) != Some(voice)
        || session.get("mode").and_then(serde_json::Value::as_str) != Some("commit")
        || session
            .get("response_format")
            .and_then(serde_json::Value::as_str)
            != Some("pcm")
        || session
            .get("sample_rate")
            .and_then(serde_json::Value::as_u64)
            != Some(u64::from(TTS_SAMPLE_RATE))
    {
        return Err(protocol_failure(false));
    }
    Ok(())
}

fn parse_response_created(
    event: &serde_json::Value,
    state: &mut TtsSessionState,
    voice: &str,
) -> Result<(), AttemptFailure> {
    if !state.committed || state.response_id.is_some() {
        return Err(protocol_failure(state.request_submitted));
    }
    let response = event
        .get("response")
        .ok_or_else(|| protocol_failure(state.request_submitted))?;
    let response_id = service_id(response.get("id"), state.request_submitted)?;
    if response.get("object").and_then(serde_json::Value::as_str) != Some("realtime.response")
        || response.get("status").and_then(serde_json::Value::as_str) != Some("in_progress")
        || response.get("voice").and_then(serde_json::Value::as_str) != Some(voice)
        || !response
            .get("output")
            .and_then(serde_json::Value::as_array)
            .is_some_and(Vec::is_empty)
    {
        return Err(protocol_failure(state.request_submitted));
    }
    state.response_id = Some(response_id);
    Ok(())
}

fn validate_response_scaffold(
    event: &serde_json::Value,
    state: &mut TtsSessionState,
) -> Result<(), AttemptFailure> {
    let response_id = state
        .response_id
        .as_deref()
        .ok_or_else(|| protocol_failure(state.request_submitted))?;
    if event.get("response_id").and_then(serde_json::Value::as_str) != Some(response_id) {
        return Err(protocol_failure(state.request_submitted));
    }
    if event
        .get("output_index")
        .and_then(serde_json::Value::as_u64)
        != Some(0)
    {
        return Err(protocol_failure(state.request_submitted));
    }
    match event.get("type").and_then(serde_json::Value::as_str) {
        Some(event_type @ ("response.content_part.added" | "response.content_part.done")) => {
            let done = event_type == "response.content_part.done";
            let lifecycle_valid = if done {
                state.output_item_added
                    && state.content_part_added
                    && state.audio_done
                    && !state.content_part_done
                    && !state.output_item_done
            } else {
                state.output_item_added
                    && !state.content_part_added
                    && !state.received_audio
                    && !state.audio_done
                    && !state.content_part_done
                    && !state.output_item_done
            };
            if !lifecycle_valid {
                return Err(protocol_failure(state.request_submitted));
            }
            let item_id = service_id(event.get("item_id"), state.request_submitted)?;
            bind_item_id(state, &item_id)?;
            if event
                .get("content_index")
                .and_then(serde_json::Value::as_u64)
                != Some(0)
                || event
                    .pointer("/part/type")
                    .and_then(serde_json::Value::as_str)
                    != Some("audio")
            {
                return Err(protocol_failure(state.request_submitted));
            }
            if done {
                state.content_part_done = true;
            } else {
                state.content_part_added = true;
            }
        }
        Some(event_type @ ("response.output_item.added" | "response.output_item.done")) => {
            let done = event_type == "response.output_item.done";
            let lifecycle_valid = if done {
                state.output_item_added && state.content_part_done && !state.output_item_done
            } else {
                !state.output_item_added
                    && !state.content_part_added
                    && !state.received_audio
                    && !state.audio_done
                    && !state.content_part_done
                    && !state.output_item_done
            };
            if !lifecycle_valid {
                return Err(protocol_failure(state.request_submitted));
            }
            let item_id = service_id(event.pointer("/item/id"), state.request_submitted)?;
            bind_item_id(state, &item_id)?;
            let expected_status = if done { "completed" } else { "in_progress" };
            if event
                .pointer("/item/object")
                .and_then(serde_json::Value::as_str)
                != Some("realtime.item")
                || event
                    .pointer("/item/type")
                    .and_then(serde_json::Value::as_str)
                    != Some("message")
                || event
                    .pointer("/item/role")
                    .and_then(serde_json::Value::as_str)
                    != Some("assistant")
                || event
                    .pointer("/item/status")
                    .and_then(serde_json::Value::as_str)
                    != Some(expected_status)
            {
                return Err(protocol_failure(state.request_submitted));
            }
            if done {
                state.output_item_done = true;
            } else {
                state.output_item_added = true;
            }
        }
        _ => return Err(protocol_failure(state.request_submitted)),
    }
    Ok(())
}

fn append_audio_delta(
    event: &mut serde_json::Value,
    state: &mut TtsSessionState,
) -> Result<(), AttemptFailure> {
    validate_audio_identity(event, state)?;
    if !state.output_item_added || !state.content_part_added || state.audio_done {
        return Err(protocol_failure(state.request_submitted));
    }
    if state.audio_deltas >= MAX_TTS_AUDIO_DELTAS {
        return Err(protocol_failure(state.request_submitted));
    }
    state.audio_deltas += 1;
    let delta_value = event
        .get_mut("delta")
        .map(serde_json::Value::take)
        .ok_or_else(|| protocol_failure(state.request_submitted))?;
    let delta = match delta_value {
        serde_json::Value::String(delta) => Zeroizing::new(delta),
        mut value => {
            zeroize_json(&mut value);
            return Err(protocol_failure(state.request_submitted));
        }
    };
    let decoded = Zeroizing::new(
        BASE64
            .decode(delta.as_bytes())
            .map_err(|_| protocol_failure(state.request_submitted))?,
    );
    if decoded.is_empty() || !decoded.len().is_multiple_of(2) {
        return Err(protocol_failure(state.request_submitted));
    }
    let new_length = state
        .pcm
        .len()
        .checked_add(decoded.len())
        .ok_or_else(ambiguous_after_commit)?;
    if new_length > MAX_TTS_PCM_BYTES {
        return Err(ambiguous_after_commit());
    }
    state.pcm.extend_from_slice(&decoded);
    state.received_audio = true;
    Ok(())
}

fn validate_audio_identity(
    event: &serde_json::Value,
    state: &mut TtsSessionState,
) -> Result<(), AttemptFailure> {
    let response_id = state
        .response_id
        .as_deref()
        .ok_or_else(|| protocol_failure(state.request_submitted))?;
    let item_id = service_id(event.get("item_id"), state.request_submitted)?;
    if event.get("response_id").and_then(serde_json::Value::as_str) != Some(response_id)
        || event
            .get("output_index")
            .and_then(serde_json::Value::as_u64)
            != Some(0)
        || event
            .get("content_index")
            .and_then(serde_json::Value::as_u64)
            != Some(0)
    {
        return Err(protocol_failure(state.request_submitted));
    }
    bind_item_id(state, &item_id)
}

fn bind_item_id(state: &mut TtsSessionState, item_id: &str) -> Result<(), AttemptFailure> {
    match state.item_id.as_deref() {
        Some(existing) if existing != item_id => Err(protocol_failure(state.request_submitted)),
        Some(_) => Ok(()),
        None => {
            state.item_id = Some(item_id.to_owned());
            Ok(())
        }
    }
}

fn mark_audio_done(
    event: &serde_json::Value,
    state: &mut TtsSessionState,
) -> Result<(), AttemptFailure> {
    validate_audio_identity(event, state)?;
    if !state.output_item_added
        || !state.content_part_added
        || state.audio_done
        || !state.received_audio
        || state.content_part_done
        || state.output_item_done
    {
        return Err(protocol_failure(state.request_submitted));
    }
    state.audio_done = true;
    Ok(())
}

fn mark_response_done(
    event: &serde_json::Value,
    state: &mut TtsSessionState,
) -> Result<(), AttemptFailure> {
    if !state.audio_done
        || !state.content_part_done
        || !state.output_item_done
        || state.response_done
    {
        return Err(protocol_failure(state.request_submitted));
    }
    let response = event
        .get("response")
        .ok_or_else(|| protocol_failure(state.request_submitted))?;
    if response.get("id").and_then(serde_json::Value::as_str) != state.response_id.as_deref()
        || response.get("object").and_then(serde_json::Value::as_str) != Some("realtime.response")
        || response.get("status").and_then(serde_json::Value::as_str) != Some("completed")
    {
        return Err(protocol_failure(state.request_submitted));
    }
    state.characters = response
        .pointer("/usage/characters")
        .and_then(serde_json::Value::as_u64);
    state.response_done = true;
    Ok(())
}

fn classify_tts_service_error(
    event: &serde_json::Value,
    request_submitted: bool,
) -> AttemptFailure {
    let code = event
        .pointer("/error/code")
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .to_ascii_lowercase();
    let kind = if code.contains("rate") || code.contains("throttl") || code.contains("429") {
        TtsError::RateLimited
    } else if code.contains("auth") || code.contains("credential") || code.contains("401") {
        TtsError::Rejected
    } else if code.contains("server") || code.contains("internal") || code.contains("unavailable") {
        TtsError::Unavailable
    } else {
        TtsError::Protocol
    };
    if request_submitted {
        ambiguous_after_commit()
    } else if matches!(kind, TtsError::RateLimited | TtsError::Unavailable) {
        AttemptFailure::retryable(kind)
    } else {
        AttemptFailure::terminal(kind)
    }
}

fn ambiguous_after_commit() -> AttemptFailure {
    AttemptFailure::terminal(TtsError::AmbiguousAfterCommit)
}

fn retry_before_commit(failure: AttemptFailure) -> AttemptFailure {
    if failure.error == TtsError::Unavailable {
        AttemptFailure::retryable(TtsError::Unavailable)
    } else {
        failure
    }
}

fn protocol_failure(request_submitted: bool) -> AttemptFailure {
    if request_submitted {
        ambiguous_after_commit()
    } else {
        AttemptFailure::terminal(TtsError::Protocol)
    }
}

fn service_id(
    value: Option<&serde_json::Value>,
    request_submitted: bool,
) -> Result<String, AttemptFailure> {
    let value = value
        .and_then(serde_json::Value::as_str)
        .filter(|value| valid_service_event_id(value))
        .ok_or_else(|| protocol_failure(request_submitted))?;
    Ok(value.to_owned())
}

fn require_service_id(
    value: Option<&serde_json::Value>,
    allow_empty: bool,
    request_submitted: bool,
) -> Result<(), AttemptFailure> {
    let value = value
        .and_then(serde_json::Value::as_str)
        .ok_or_else(|| protocol_failure(request_submitted))?;
    if (allow_empty && value.is_empty()) || valid_service_event_id(value) {
        Ok(())
    } else {
        Err(protocol_failure(request_submitted))
    }
}

struct DeadlineSocket {
    inner: TcpStream,
    deadline: Instant,
}

impl DeadlineSocket {
    fn new(inner: TcpStream, deadline: Instant) -> Result<Self, VerificationError> {
        inner
            .set_nonblocking(true)
            .map_err(|_| VerificationError::Unavailable)?;
        Ok(Self { inner, deadline })
    }

    fn wait(&self, events: i16) -> std::io::Result<()> {
        loop {
            let remaining = self
                .deadline
                .checked_duration_since(Instant::now())
                .ok_or_else(|| {
                    std::io::Error::new(std::io::ErrorKind::TimedOut, "deadline elapsed")
                })?;
            let timeout_ms = remaining.as_millis().clamp(1, i32::MAX as u128) as i32;
            let mut descriptor = libc::pollfd {
                fd: std::os::fd::AsRawFd::as_raw_fd(&self.inner),
                events,
                revents: 0,
            };
            let result = unsafe { libc::poll(&mut descriptor, 1, timeout_ms) };
            if result > 0 {
                return Ok(());
            }
            if result == 0 {
                return Err(std::io::Error::new(
                    std::io::ErrorKind::TimedOut,
                    "deadline elapsed",
                ));
            }
            let error = std::io::Error::last_os_error();
            if error.kind() != std::io::ErrorKind::Interrupted {
                return Err(error);
            }
        }
    }
}

impl Read for DeadlineSocket {
    fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
        loop {
            if Instant::now() >= self.deadline {
                return Err(std::io::Error::new(
                    std::io::ErrorKind::TimedOut,
                    "deadline elapsed",
                ));
            }
            match self.inner.read(buffer) {
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                    self.wait(libc::POLLIN)?;
                }
                Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
                result => return result,
            }
        }
    }
}

impl Write for DeadlineSocket {
    fn write(&mut self, buffer: &[u8]) -> std::io::Result<usize> {
        loop {
            if Instant::now() >= self.deadline {
                return Err(std::io::Error::new(
                    std::io::ErrorKind::TimedOut,
                    "deadline elapsed",
                ));
            }
            match self.inner.write(buffer) {
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                    self.wait(libc::POLLOUT)?;
                }
                Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
                result => return result,
            }
        }
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.inner.flush()
    }
}

struct DeadlineTls {
    inner: StreamOwned<ClientConnection, DeadlineSocket>,
}

impl Read for DeadlineTls {
    fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
        self.inner.read(buffer)
    }
}

impl Write for DeadlineTls {
    fn write(&mut self, buffer: &[u8]) -> std::io::Result<usize> {
        self.inner.write(buffer)
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.inner.flush()
    }
}

fn validate_handshake_response(
    response: &[u8],
    websocket_key: &str,
) -> Result<(), VerificationError> {
    let mut headers = [httparse::EMPTY_HEADER; 32];
    let mut parsed = httparse::Response::new(&mut headers);
    if !parsed
        .parse(response)
        .map_err(|_| VerificationError::Protocol)?
        .is_complete()
    {
        return Err(VerificationError::Protocol);
    }
    if parsed.version != Some(1) {
        return Err(VerificationError::Protocol);
    }
    match parsed.code {
        Some(101) => {}
        Some(401 | 403) => return Err(VerificationError::Rejected),
        _ => return Err(VerificationError::Protocol),
    }
    let upgrade = unique_header_value(&parsed, "upgrade")?
        .is_some_and(|value| value.eq_ignore_ascii_case(b"websocket"));
    let connection = unique_header_value(&parsed, "connection")?.is_some_and(|value| {
        value
            .split(|byte| *byte == b',')
            .any(|token| token.trim_ascii().eq_ignore_ascii_case(b"upgrade"))
    });
    let expected_accept = websocket_accept(websocket_key);
    let accept = unique_header_value(&parsed, "sec-websocket-accept")?
        .is_some_and(|value| value == expected_accept.as_bytes());
    let unsolicited_negotiation = parsed.headers.iter().any(|header| {
        header.name.eq_ignore_ascii_case("sec-websocket-extensions")
            || header.name.eq_ignore_ascii_case("sec-websocket-protocol")
    });
    if upgrade && connection && accept && !unsolicited_negotiation {
        Ok(())
    } else {
        Err(VerificationError::Protocol)
    }
}

fn unique_header_value<'a>(
    response: &'a httparse::Response<'a, 'a>,
    name: &str,
) -> Result<Option<&'a [u8]>, VerificationError> {
    let mut matches = response
        .headers
        .iter()
        .filter(|header| header.name.eq_ignore_ascii_case(name));
    let value = matches.next().map(|header| header.value);
    if matches.next().is_some() {
        Err(VerificationError::Protocol)
    } else {
        Ok(value)
    }
}

fn websocket_accept(key: &str) -> String {
    let mut digest = Sha1::new();
    digest.update(key.as_bytes());
    digest.update(b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    BASE64.encode(digest.finalize())
}

struct ServerFrame {
    fin: bool,
    opcode: u8,
    payload: Vec<u8>,
}

fn read_server_frame<R: Read>(
    reader: &mut R,
    pending: &mut Vec<u8>,
) -> Result<ServerFrame, VerificationError> {
    let mut first = [0_u8; 2];
    read_exact_prefixed(reader, pending, &mut first)?;
    let fin = first[0] & 0x80 != 0;
    let reserved = first[0] & 0x70;
    let opcode = first[0] & 0x0f;
    let masked = first[1] & 0x80 != 0;
    if reserved != 0 || masked || !matches!(opcode, 0x0 | 0x1 | 0x2 | 0x8 | 0x9 | 0xA) {
        return Err(VerificationError::Protocol);
    }
    let length_code = first[1] & 0x7f;
    if opcode >= 0x8 && (!fin || length_code >= 126) {
        return Err(VerificationError::Protocol);
    }
    let mut length = u64::from(length_code);
    if length == 126 {
        let mut extended = [0_u8; 2];
        read_exact_prefixed(reader, pending, &mut extended)?;
        length = u64::from(u16::from_be_bytes(extended));
        if length < 126 {
            return Err(VerificationError::Protocol);
        }
    } else if length == 127 {
        let mut extended = [0_u8; 8];
        read_exact_prefixed(reader, pending, &mut extended)?;
        length = u64::from_be_bytes(extended);
        if length < 65_536 || length & (1_u64 << 63) != 0 {
            return Err(VerificationError::Protocol);
        }
    }
    let length = usize::try_from(length).map_err(|_| VerificationError::Protocol)?;
    if length > MAX_SERVER_EVENT_BYTES {
        return Err(VerificationError::Protocol);
    }
    let mut payload = vec![0_u8; length];
    read_exact_prefixed(reader, pending, &mut payload)?;
    Ok(ServerFrame {
        fin,
        opcode,
        payload,
    })
}

fn assemble_text_frame(
    frame: ServerFrame,
    fragmented: &mut Option<Vec<u8>>,
) -> Result<Option<Vec<u8>>, VerificationError> {
    match frame.opcode {
        0x1 if fragmented.is_none() && frame.fin => Ok(Some(frame.payload)),
        0x1 if fragmented.is_none() => {
            *fragmented = Some(frame.payload);
            Ok(None)
        }
        0x0 => {
            let payload = fragmented.as_mut().ok_or(VerificationError::Protocol)?;
            let total = payload
                .len()
                .checked_add(frame.payload.len())
                .ok_or(VerificationError::Protocol)?;
            if total > MAX_SERVER_EVENT_BYTES {
                return Err(VerificationError::Protocol);
            }
            payload.extend_from_slice(&frame.payload);
            if frame.fin {
                Ok(fragmented.take())
            } else {
                Ok(None)
            }
        }
        _ => Err(VerificationError::Protocol),
    }
}

fn read_exact_prefixed<R: Read>(
    reader: &mut R,
    pending: &mut Vec<u8>,
    output: &mut [u8],
) -> Result<(), VerificationError> {
    let from_pending = pending.len().min(output.len());
    output[..from_pending].copy_from_slice(&pending[..from_pending]);
    pending.drain(..from_pending);
    reader
        .read_exact(&mut output[from_pending..])
        .map_err(|_| VerificationError::Unavailable)
}

fn write_masked_pong<W: Write>(writer: &mut W, payload: &[u8]) -> Result<(), VerificationError> {
    write_masked_control_frame(writer, 0xA, payload)
}

fn write_masked_close<W: Write>(writer: &mut W, payload: &[u8]) -> Result<(), VerificationError> {
    write_masked_control_frame(writer, 0x8, payload)
}

fn await_peer_close<S: Read + Write>(
    stream: &mut S,
    pending: &mut Vec<u8>,
) -> Result<(), VerificationError> {
    loop {
        let frame = read_server_frame(stream, pending)?;
        match frame.opcode {
            0x8 => {
                validate_peer_close_payload(&frame.payload)?;
                return Ok(());
            }
            0x9 => write_masked_pong(stream, &frame.payload)?,
            0xA => {}
            _ => return Err(VerificationError::Protocol),
        }
    }
}

fn write_masked_control_frame<W: Write>(
    writer: &mut W,
    opcode: u8,
    payload: &[u8],
) -> Result<(), VerificationError> {
    if payload.len() > 125 {
        return Err(VerificationError::Protocol);
    }
    let mask_source = Uuid::new_v4();
    let mask = &mask_source.as_bytes()[..4];
    let mut frame = Vec::with_capacity(6 + payload.len());
    frame.push(0x80 | opcode);
    frame.push(0x80 | payload.len() as u8);
    frame.extend_from_slice(mask);
    frame.extend(
        payload
            .iter()
            .enumerate()
            .map(|(index, byte)| byte ^ mask[index % 4]),
    );
    writer
        .write_all(&frame)
        .and_then(|()| writer.flush())
        .map_err(|_| VerificationError::Unavailable)
}

fn validate_peer_close_payload(payload: &[u8]) -> Result<(), VerificationError> {
    if payload.len() == 1 {
        return Err(VerificationError::Protocol);
    }
    if payload.len() >= 2 {
        let status = u16::from_be_bytes([payload[0], payload[1]]);
        let valid_status = matches!(
            status,
            1000 | 1001 | 1002 | 1003 | 1007 | 1008 | 1009 | 1011 | 1012 | 1013 | 1014
        ) || matches!(status, 3000..=4999);
        if !valid_status || std::str::from_utf8(&payload[2..]).is_err() {
            return Err(VerificationError::Protocol);
        }
    }
    Ok(())
}

enum ServerEvent {
    Created {
        event_id: String,
        model: &'static str,
    },
    Other,
}

fn classify_server_event(payload: &[u8]) -> Result<ServerEvent, VerificationError> {
    let event: serde_json::Value =
        serde_json::from_slice(payload).map_err(|_| VerificationError::Protocol)?;
    match event.get("type").and_then(serde_json::Value::as_str) {
        Some("session.created") => {
            let event_id = event
                .get("event_id")
                .and_then(serde_json::Value::as_str)
                .filter(|value| valid_service_event_id(value))
                .ok_or(VerificationError::Protocol)?;
            let model = event
                .pointer("/session/model")
                .and_then(serde_json::Value::as_str)
                .ok_or(VerificationError::Protocol)?;
            let model = match model {
                TTS_MODEL => TTS_MODEL,
                TTS_MODEL_SNAPSHOT => TTS_MODEL_SNAPSHOT,
                _ => return Err(VerificationError::Protocol),
            };
            Ok(ServerEvent::Created {
                event_id: event_id.to_owned(),
                model,
            })
        }
        Some("error") => Err(VerificationError::Protocol),
        Some(_) => Ok(ServerEvent::Other),
        None => Err(VerificationError::Protocol),
    }
}

fn valid_service_event_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 256
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.' | b':'))
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct ProxyEndpoint {
    host: String,
    port: u16,
}

#[cfg(any(target_os = "macos", test))]
#[derive(Debug, Default, Clone, PartialEq, Eq)]
struct SystemProxySettings {
    exceptions: Vec<String>,
    pac_enabled: bool,
    auto_discovery_enabled: bool,
    https_enabled: bool,
    https_host: Option<String>,
    https_port: Option<i64>,
    requires_authentication: bool,
}

#[cfg(any(target_os = "macos", test))]
fn select_system_proxy(
    settings: &SystemProxySettings,
) -> Result<Option<ProxyEndpoint>, VerificationError> {
    if bypasses_proxy(&settings.exceptions.join(","), HOST, PORT) {
        return Ok(None);
    }
    if settings.pac_enabled || settings.auto_discovery_enabled {
        return Err(VerificationError::Unavailable);
    }
    if !settings.https_enabled {
        return Ok(None);
    }
    if settings.requires_authentication {
        return Err(VerificationError::Unavailable);
    }
    let host = settings
        .https_host
        .as_deref()
        .filter(|host| !host.is_empty())
        .ok_or(VerificationError::Unavailable)?;
    let raw_port = settings.https_port.ok_or(VerificationError::Unavailable)?;
    let port = u16::try_from(raw_port).map_err(|_| VerificationError::Unavailable)?;
    if port == 0 {
        return Err(VerificationError::Unavailable);
    }
    Ok(Some(ProxyEndpoint {
        host: host.to_owned(),
        port,
    }))
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum EnvironmentValue {
    Missing,
    Unicode(String),
    InvalidUnicode,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum EnvironmentProxySelection {
    Direct,
    Proxy(ProxyEndpoint),
    System,
}

fn environment_value(value: Option<OsString>) -> EnvironmentValue {
    match value {
        None => EnvironmentValue::Missing,
        Some(value) => value
            .into_string()
            .map(EnvironmentValue::Unicode)
            .unwrap_or(EnvironmentValue::InvalidUnicode),
    }
}

fn preferred_environment_value<'a>(
    uppercase: &'a EnvironmentValue,
    lowercase: &'a EnvironmentValue,
) -> &'a EnvironmentValue {
    if matches!(uppercase, EnvironmentValue::Missing) {
        lowercase
    } else {
        uppercase
    }
}

fn select_environment_proxy(
    no_proxy_uppercase: &EnvironmentValue,
    no_proxy_lowercase: &EnvironmentValue,
    https_proxy_uppercase: &EnvironmentValue,
    https_proxy_lowercase: &EnvironmentValue,
) -> Result<EnvironmentProxySelection, VerificationError> {
    match preferred_environment_value(no_proxy_uppercase, no_proxy_lowercase) {
        EnvironmentValue::InvalidUnicode => return Err(VerificationError::Unavailable),
        EnvironmentValue::Unicode(value) if bypasses_proxy(value, HOST, PORT) => {
            return Ok(EnvironmentProxySelection::Direct);
        }
        EnvironmentValue::Missing | EnvironmentValue::Unicode(_) => {}
    }

    match preferred_environment_value(https_proxy_uppercase, https_proxy_lowercase) {
        EnvironmentValue::InvalidUnicode => Err(VerificationError::Unavailable),
        EnvironmentValue::Unicode(value) => parse_proxy(Some(value)).map(|proxy| {
            proxy.map_or(
                EnvironmentProxySelection::Direct,
                EnvironmentProxySelection::Proxy,
            )
        }),
        EnvironmentValue::Missing => Ok(EnvironmentProxySelection::System),
    }
}

fn proxy_from_environment() -> Result<Option<ProxyEndpoint>, VerificationError> {
    let selection = select_environment_proxy(
        &environment_value(env::var_os("NO_PROXY")),
        &environment_value(env::var_os("no_proxy")),
        &environment_value(env::var_os("HTTPS_PROXY")),
        &environment_value(env::var_os("https_proxy")),
    )?;
    match selection {
        EnvironmentProxySelection::Direct => Ok(None),
        EnvironmentProxySelection::Proxy(proxy) => Ok(Some(proxy)),
        EnvironmentProxySelection::System => {
            #[cfg(target_os = "macos")]
            {
                macos_system_proxy()
            }
            #[cfg(not(target_os = "macos"))]
            Ok(None)
        }
    }
}

#[cfg(target_os = "macos")]
fn macos_system_proxy() -> Result<Option<ProxyEndpoint>, VerificationError> {
    use core::ffi::c_void;
    use core_foundation::array::CFArray;
    use core_foundation::base::{CFGetTypeID, CFType, TCFType};
    use core_foundation::dictionary::{CFDictionary, CFDictionaryRef};
    use core_foundation::number::CFNumber;
    use core_foundation::string::CFString;

    #[link(name = "SystemConfiguration", kind = "framework")]
    unsafe extern "C" {
        fn SCDynamicStoreCopyProxies(store: *const c_void) -> CFDictionaryRef;
    }

    fn number(dictionary: &CFDictionary<CFString, CFType>, key: &str) -> Option<i64> {
        dictionary
            .find(CFString::new(key))?
            .downcast::<CFNumber>()?
            .to_i64()
    }

    fn string(dictionary: &CFDictionary<CFString, CFType>, key: &str) -> Option<String> {
        Some(
            dictionary
                .find(CFString::new(key))?
                .downcast::<CFString>()?
                .to_string(),
        )
    }

    fn strings(dictionary: &CFDictionary<CFString, CFType>, key: &str) -> Vec<String> {
        let Some(array) = dictionary
            .find(CFString::new(key))
            .and_then(|value| value.downcast::<CFArray>())
        else {
            return Vec::new();
        };
        array
            .iter()
            .filter_map(|raw| {
                let raw = *raw;
                if raw.is_null()
                    || unsafe { CFGetTypeID(raw.cast()) }
                        != unsafe { core_foundation::string::CFStringGetTypeID() }
                {
                    return None;
                }
                Some(unsafe { CFString::wrap_under_get_rule(raw.cast()) }.to_string())
            })
            .collect()
    }

    let raw = unsafe { SCDynamicStoreCopyProxies(std::ptr::null()) };
    if raw.is_null() {
        return Err(VerificationError::Unavailable);
    }
    let settings = unsafe { CFDictionary::<CFString, CFType>::wrap_under_create_rule(raw) };
    select_system_proxy(&SystemProxySettings {
        exceptions: strings(&settings, "ExceptionsList"),
        pac_enabled: number(&settings, "ProxyAutoConfigEnable") == Some(1),
        auto_discovery_enabled: number(&settings, "ProxyAutoDiscoveryEnable") == Some(1),
        https_enabled: number(&settings, "HTTPSEnable") == Some(1),
        https_host: string(&settings, "HTTPSProxy"),
        https_port: number(&settings, "HTTPSPort"),
        requires_authentication: number(&settings, "HTTPSProxyAuthenticated") == Some(1)
            || string(&settings, "HTTPSProxyUsername").is_some()
            || string(&settings, "HTTPSProxyPassword").is_some(),
    })
}

fn parse_proxy(value: Option<&str>) -> Result<Option<ProxyEndpoint>, VerificationError> {
    let Some(value) = value else {
        return Ok(None);
    };
    let mut authority = value
        .strip_prefix("http://")
        .ok_or(VerificationError::Unavailable)?;
    if let Some(without_slash) = authority.strip_suffix('/') {
        authority = without_slash;
    }
    if authority.contains('@') || authority.contains('/') {
        return Err(VerificationError::Unavailable);
    }
    let (host, port) = if let Some((host, raw_port)) = authority.rsplit_once(':') {
        let port = raw_port
            .parse()
            .map_err(|_| VerificationError::Unavailable)?;
        (host, port)
    } else {
        (authority, 80)
    };
    if host.is_empty() {
        return Err(VerificationError::Unavailable);
    }
    Ok(Some(ProxyEndpoint {
        host: host.to_owned(),
        port,
    }))
}

fn bypasses_proxy(no_proxy: &str, host: &str, port: u16) -> bool {
    no_proxy.split(',').any(|entry| {
        let mut entry = entry.trim();
        if entry == "*" {
            return true;
        }
        if let Some((without_port, raw_port)) = entry.rsplit_once(':')
            && let Ok(entry_port) = raw_port.parse::<u16>()
        {
            if entry_port != port {
                return false;
            }
            entry = without_port;
        }
        let domain = entry
            .trim_start_matches("*.")
            .trim_start_matches('.')
            .trim_end_matches('.')
            .to_ascii_lowercase();
        let host = host.to_ascii_lowercase();
        !domain.is_empty()
            && (host == domain
                || host
                    .strip_suffix(&domain)
                    .is_some_and(|prefix| prefix.ends_with('.')))
    })
}

fn establish_proxy_tunnel(
    stream: &TcpStream,
    deadline: Instant,
    target: &HandshakeTarget,
) -> Result<(), VerificationError> {
    set_socket_deadline(stream, deadline)?;
    let mut stream = stream;
    let authority = format!("{}:{}", target.host, target.port);
    let mut request = Vec::with_capacity(128 + authority.len() * 2);
    request.extend_from_slice(b"CONNECT ");
    request.extend_from_slice(authority.as_bytes());
    request.extend_from_slice(b" HTTP/1.1\r\nHost: ");
    request.extend_from_slice(authority.as_bytes());
    request.extend_from_slice(b"\r\n\r\n");
    stream
        .write_all(&request)
        .map_err(|_| VerificationError::Unavailable)?;
    read_proxy_connect_response(stream, deadline)
}

fn read_proxy_connect_response(
    mut stream: &TcpStream,
    deadline: Instant,
) -> Result<(), VerificationError> {
    let mut response = Vec::with_capacity(512);
    loop {
        set_socket_deadline(stream, deadline)?;
        match scan_http_header(&response, MAX_PROXY_HEADER_BYTES) {
            HeaderScan::Complete(end) => {
                let mut headers = [httparse::EMPTY_HEADER; 16];
                let mut parsed = httparse::Response::new(&mut headers);
                let complete = parsed
                    .parse(&response[..end])
                    .map_err(|_| VerificationError::Unavailable)?
                    .is_complete();
                return if complete && parsed.version.is_some() && parsed.code == Some(200) {
                    Ok(())
                } else {
                    Err(VerificationError::Unavailable)
                };
            }
            HeaderScan::TooLarge => return Err(VerificationError::Unavailable),
            HeaderScan::NeedMore => {}
        }
        let mut chunk = [0_u8; 512];
        let read = stream
            .read(&mut chunk)
            .map_err(|_| VerificationError::Unavailable)?;
        if read == 0 {
            return Err(VerificationError::Unavailable);
        }
        response.extend_from_slice(&chunk[..read]);
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum HeaderScan {
    NeedMore,
    Complete(usize),
    TooLarge,
}

fn scan_http_header(response: &[u8], maximum: usize) -> HeaderScan {
    if let Some(index) = response.windows(4).position(|window| window == b"\r\n\r\n") {
        let end = index + 4;
        if end <= maximum {
            HeaderScan::Complete(end)
        } else {
            HeaderScan::TooLarge
        }
    } else if response.len() >= maximum {
        HeaderScan::TooLarge
    } else {
        HeaderScan::NeedMore
    }
}

fn set_socket_deadline(stream: &TcpStream, deadline: Instant) -> Result<(), VerificationError> {
    let remaining = deadline
        .checked_duration_since(Instant::now())
        .ok_or(VerificationError::Unavailable)?;
    stream
        .set_read_timeout(Some(remaining))
        .and_then(|()| stream.set_write_timeout(Some(remaining)))
        .map_err(|_| VerificationError::Unavailable)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::collections::HashMap;
    use std::io::Cursor;
    use std::net::TcpListener;
    use std::thread::JoinHandle;

    use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
    use rustls::{ServerConfig, ServerConnection};

    const TEST_CERT_DER_BASE64: &str = "MIIBuDCCAV2gAwIBAgIUXTf3Qlkj11iUY1TXWg9OsxTq3iUwCgYIKoZIzj0EAwIwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDgwMjE4MzgyOVoXDTM2MDczMDE4MzgyOVowFDESMBAGA1UEAwwJbG9jYWxob3N0MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEurHxjHKLuMvbmtT2uXmqYzWK4nPrkZnVF34+RPlpj2K5qS4xLjyYdUE+gzuKFLG1JH81gIxVxseXbUsxXLw6hKOBjDCBiTAdBgNVHQ4EFgQUKw06epdOGlwiKm9zNGW9y751UwUwHwYDVR0jBBgwFoAUKw06epdOGlwiKm9zNGW9y751UwUwFAYDVR0RBA0wC4IJbG9jYWxob3N0MAwGA1UdEwEB/wQCMAAwDgYDVR0PAQH/BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMBMAoGCCqGSM49BAMCA0kAMEYCIQDnr6YpV6nSQeGDl90oIldrdocgoP0B64E1eEwDS7tBfAIhAIAvCiizfmZu+jfwYDr1GK30+9zuU5XBPCUomJUu/pHc";
    const TEST_KEY_DER_BASE64: &str = "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg+WNf0SCqjrK+p7RGY8uBitAtmUb/hsJeQP1h8ZYP2b6hRANCAAS6sfGMcou4y9ua1Pa5eapjNYric+uRmdUXfj5E+WmPYrmpLjEuPJh1QT6DO4oUsbUkfzWAjFXGx5dtSzFcvDqE";

    #[derive(Clone, Copy)]
    enum ServerScript {
        Success,
        Http(u16),
        Proxy407,
        InvalidClose,
        MissingClose,
    }

    fn test_tls_configs() -> (Arc<ClientConfig>, Arc<ServerConfig>) {
        let certificate = CertificateDer::from(BASE64.decode(TEST_CERT_DER_BASE64).unwrap());
        let private_key = PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(
            BASE64.decode(TEST_KEY_DER_BASE64).unwrap(),
        ));
        let mut roots = RootCertStore::empty();
        roots.add(certificate.clone()).unwrap();
        let client = ClientConfig::builder()
            .with_root_certificates(roots)
            .with_no_client_auth();
        let server = ServerConfig::builder()
            .with_no_client_auth()
            .with_single_cert(vec![certificate], private_key)
            .unwrap();
        (Arc::new(client), Arc::new(server))
    }

    fn read_headers<R: Read>(reader: &mut R, maximum: usize) -> Vec<u8> {
        let mut request = Vec::new();
        loop {
            match scan_http_header(&request, maximum) {
                HeaderScan::Complete(end) => {
                    request.truncate(end);
                    return request;
                }
                HeaderScan::TooLarge => panic!("test peer sent an oversized header"),
                HeaderScan::NeedMore => {}
            }
            let mut chunk = [0_u8; 512];
            let count = reader.read(&mut chunk).unwrap();
            assert!(count > 0);
            request.extend_from_slice(&chunk[..count]);
        }
    }

    fn write_server_frame<W: Write>(writer: &mut W, opcode: u8, fin: bool, payload: &[u8]) {
        writer
            .write_all(&[if fin { 0x80 | opcode } else { opcode }])
            .unwrap();
        if payload.len() < 126 {
            writer.write_all(&[payload.len() as u8]).unwrap();
        } else {
            writer.write_all(&[126]).unwrap();
            writer
                .write_all(&(payload.len() as u16).to_be_bytes())
                .unwrap();
        }
        writer.write_all(payload).unwrap();
        writer.flush().unwrap();
    }

    fn read_client_close<R: Read>(reader: &mut R) {
        loop {
            let mut header = [0_u8; 2];
            reader.read_exact(&mut header).unwrap();
            assert_ne!(header[1] & 0x80, 0);
            let length = usize::from(header[1] & 0x7f);
            assert!(length <= 125);
            let mut mask = [0_u8; 4];
            reader.read_exact(&mut mask).unwrap();
            let mut payload = vec![0_u8; length];
            reader.read_exact(&mut payload).unwrap();
            for (index, byte) in payload.iter_mut().enumerate() {
                *byte ^= mask[index % 4];
            }
            match header[0] {
                0x8A => assert_eq!(payload, b"probe"),
                0x88 => {
                    assert_eq!(payload, 1000_u16.to_be_bytes());
                    return;
                }
                _ => panic!("unexpected client control frame"),
            }
        }
    }

    fn read_client_pong<R: Read>(reader: &mut R, expected: &[u8]) {
        let mut header = [0_u8; 2];
        reader.read_exact(&mut header).unwrap();
        assert_eq!(header[0], 0x8A);
        assert_ne!(header[1] & 0x80, 0);
        let length = usize::from(header[1] & 0x7f);
        assert!(length <= 125);
        let mut mask = [0_u8; 4];
        reader.read_exact(&mut mask).unwrap();
        let mut payload = vec![0_u8; length];
        reader.read_exact(&mut payload).unwrap();
        for (index, byte) in payload.iter_mut().enumerate() {
            *byte ^= mask[index % 4];
        }
        assert_eq!(payload, expected);
    }

    fn read_client_text<R: Read>(reader: &mut R) -> serde_json::Value {
        let mut header = [0_u8; 2];
        reader.read_exact(&mut header).unwrap();
        assert_eq!(header[0], 0x81);
        assert_ne!(header[1] & 0x80, 0);
        let mut length = u64::from(header[1] & 0x7f);
        if length == 126 {
            let mut extended = [0_u8; 2];
            reader.read_exact(&mut extended).unwrap();
            length = u64::from(u16::from_be_bytes(extended));
        } else if length == 127 {
            let mut extended = [0_u8; 8];
            reader.read_exact(&mut extended).unwrap();
            length = u64::from_be_bytes(extended);
        }
        let mut mask = [0_u8; 4];
        reader.read_exact(&mut mask).unwrap();
        let mut payload = vec![0_u8; usize::try_from(length).unwrap()];
        reader.read_exact(&mut payload).unwrap();
        for (index, byte) in payload.iter_mut().enumerate() {
            *byte ^= mask[index % 4];
        }
        serde_json::from_slice(&payload).unwrap()
    }

    #[derive(Clone, Copy)]
    enum TtsServerScript {
        Success,
        InvalidBase64,
        AudioDoneBeforeDelta,
        MissingAudioDone,
        DuplicateEventId,
        DuplicateOutputItemAdded,
        ContentPartBeforeOutputItem,
        MissingDoneScaffold,
        DisconnectAfterCommit,
        DisconnectAfterResponseDone,
        StallAfterDelta,
        Http(u16),
    }

    fn tts_event(sequence: u32, event_type: &str, body: serde_json::Value) -> Vec<u8> {
        let mut event = serde_json::json!({
            "event_id": format!("event-server-{sequence}"),
            "type": event_type
        });
        event
            .as_object_mut()
            .unwrap()
            .extend(body.as_object().cloned().unwrap_or_default());
        serde_json::to_vec(&event).unwrap()
    }

    fn serve_tts_connection(stream: TcpStream, tls: Arc<ServerConfig>, script: TtsServerScript) {
        let connection = ServerConnection::new(tls).unwrap();
        let mut stream = StreamOwned::new(connection, stream);
        let request = read_headers(&mut stream, MAX_HTTP_HEADER_BYTES);
        let request = std::str::from_utf8(&request).unwrap();
        assert!(request.contains("Authorization: Bearer test-tts-credential"));
        if let TtsServerScript::Http(status) = script {
            write!(
                stream,
                "HTTP/1.1 {status} Rejected\r\nContent-Length: 0\r\n\r\n"
            )
            .unwrap();
            stream.flush().unwrap();
            return;
        }
        let websocket_key = request
            .lines()
            .find_map(|line| line.strip_prefix("Sec-WebSocket-Key: "))
            .unwrap();
        write!(
            stream,
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
            websocket_accept(websocket_key)
        )
        .unwrap();
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(
                1,
                "session.created",
                serde_json::json!({"session": {
                    "id": "sess-test",
                    "model": TTS_MODEL,
                    "mode": "server_commit",
                    "voice": "Cherry",
                    "response_format": "pcm",
                    "sample_rate": 24000
                }}),
            ),
        );
        let update = read_client_text(&mut stream);
        assert_eq!(update["type"], "session.update");
        assert_eq!(update["session"]["voice"], "Cherry");
        assert_eq!(update["session"]["mode"], "commit");
        assert_eq!(update["session"]["response_format"], "pcm");
        assert_eq!(update["session"]["sample_rate"], 48_000);
        assert_eq!(update["session"]["instructions"], "calm and concise");
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(
                2,
                "session.updated",
                serde_json::json!({"session": {
                    "id": "sess-test",
                    "model": TTS_MODEL,
                    "voice": "Cherry",
                    "mode": "commit",
                    "response_format": "pcm",
                    "sample_rate": 48000
                }}),
            ),
        );
        let append = read_client_text(&mut stream);
        assert_eq!(append["type"], "input_text_buffer.append");
        assert_eq!(append["text"], "test summary");
        assert_eq!(
            read_client_text(&mut stream)["type"],
            "input_text_buffer.commit"
        );
        if matches!(script, TtsServerScript::DisconnectAfterCommit) {
            return;
        }
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(
                3,
                "input_text_buffer.committed",
                serde_json::json!({"item_id": ""}),
            ),
        );
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(
                4,
                "response.created",
                serde_json::json!({"response": {
                    "id": "resp-test",
                    "object": "realtime.response",
                    "status": "in_progress",
                    "voice": "Cherry",
                    "output": []
                }}),
            ),
        );
        let output_item = |status: &str| {
            serde_json::json!({
                "response_id": "resp-test",
                "output_index": 0,
                "item": {
                    "id": "item-test",
                    "object": "realtime.item",
                    "type": "message",
                    "role": "assistant",
                    "status": status
                }
            })
        };
        let content_part = serde_json::json!({
            "response_id": "resp-test",
            "item_id": "item-test",
            "output_index": 0,
            "content_index": 0,
            "part": {"type": "audio"}
        });
        if matches!(script, TtsServerScript::ContentPartBeforeOutputItem) {
            write_server_frame(
                &mut stream,
                0x1,
                true,
                &tts_event(5, "response.content_part.added", content_part),
            );
            return;
        }
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(5, "response.output_item.added", output_item("in_progress")),
        );
        if matches!(script, TtsServerScript::DuplicateOutputItemAdded) {
            write_server_frame(
                &mut stream,
                0x1,
                true,
                &tts_event(6, "response.output_item.added", output_item("in_progress")),
            );
            return;
        }
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(6, "response.content_part.added", content_part.clone()),
        );
        let audio_identity = serde_json::json!({
            "response_id": "resp-test",
            "item_id": "item-test",
            "output_index": 0,
            "content_index": 0
        });
        if matches!(script, TtsServerScript::AudioDoneBeforeDelta) {
            write_server_frame(
                &mut stream,
                0x1,
                true,
                &tts_event(7, "response.audio.done", audio_identity),
            );
            return;
        }
        let delta = if matches!(script, TtsServerScript::InvalidBase64) {
            "%%%".to_owned()
        } else {
            BASE64.encode([0_u8, 0, 1, 0, 2, 0, 3, 0])
        };
        let mut delta_body = audio_identity.as_object().unwrap().clone();
        delta_body.insert("delta".into(), serde_json::Value::String(delta));
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(
                7,
                "response.audio.delta",
                serde_json::Value::Object(delta_body),
            ),
        );
        if matches!(script, TtsServerScript::InvalidBase64) {
            return;
        }
        if matches!(script, TtsServerScript::StallAfterDelta) {
            write_server_frame(&mut stream, 0x9, true, b"after-audio");
            read_client_pong(&mut stream, b"after-audio");
            std::thread::sleep(Duration::from_secs(3));
            return;
        }
        if matches!(script, TtsServerScript::MissingAudioDone) {
            write_server_frame(
                &mut stream,
                0x1,
                true,
                &tts_event(
                    8,
                    "response.done",
                    serde_json::json!({"response": {
                        "id": "resp-test",
                        "object": "realtime.response",
                        "status": "completed",
                        "usage": {"characters": 12}
                    }}),
                ),
            );
            return;
        }
        let done_sequence = if matches!(script, TtsServerScript::DuplicateEventId) {
            7
        } else {
            8
        };
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(done_sequence, "response.audio.done", audio_identity),
        );
        if matches!(script, TtsServerScript::DuplicateEventId) {
            return;
        }
        if matches!(script, TtsServerScript::MissingDoneScaffold) {
            write_server_frame(
                &mut stream,
                0x1,
                true,
                &tts_event(
                    9,
                    "response.done",
                    serde_json::json!({"response": {
                        "id": "resp-test",
                        "object": "realtime.response",
                        "status": "completed",
                        "usage": {"characters": 12}
                    }}),
                ),
            );
            return;
        }
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(9, "response.content_part.done", content_part),
        );
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(10, "response.output_item.done", output_item("completed")),
        );
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(
                11,
                "response.done",
                serde_json::json!({"response": {
                    "id": "resp-test",
                    "object": "realtime.response",
                    "status": "completed",
                    "usage": {"characters": 12}
                }}),
            ),
        );
        if matches!(script, TtsServerScript::DisconnectAfterResponseDone) {
            return;
        }
        assert_eq!(read_client_text(&mut stream)["type"], "session.finish");
        write_server_frame(
            &mut stream,
            0x1,
            true,
            &tts_event(12, "session.finished", serde_json::json!({})),
        );
        read_client_close(&mut stream);
        write_server_frame(&mut stream, 0x8, true, &1000_u16.to_be_bytes());
    }

    fn spawn_tts_peer(scripts: Vec<TtsServerScript>) -> (u16, Arc<ClientConfig>, JoinHandle<()>) {
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = listener.local_addr().unwrap().port();
        let (client, server) = test_tls_configs();
        let handle = std::thread::spawn(move || {
            for script in scripts {
                let (stream, _) = listener.accept().unwrap();
                serve_tts_connection(stream, Arc::clone(&server), script);
            }
        });
        (port, client, handle)
    }

    fn test_tts_client(port: u16, tls_config: Arc<ClientConfig>) -> DashScopeTtsClient {
        DashScopeTtsClient {
            timeout: Duration::from_secs(2),
            max_attempts: 3,
            retry_backoff: Duration::from_millis(1),
            test_platform: Some(TestPlatform {
                target: HandshakeTarget {
                    host: "localhost".into(),
                    port,
                    request_target: REQUEST_TARGET.into(),
                },
                tls_config,
                proxy: None,
                setup_delay: Duration::ZERO,
            }),
        }
    }

    fn spawn_asr_peer(
        status: u16,
        response_body: serde_json::Value,
    ) -> (u16, Arc<ClientConfig>, JoinHandle<()>) {
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = listener.local_addr().unwrap().port();
        let (client, server) = test_tls_configs();
        let handle = std::thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            let connection = ServerConnection::new(server).unwrap();
            let mut stream = StreamOwned::new(connection, stream);
            let headers = read_headers(&mut stream, MAX_HTTP_HEADER_BYTES);
            let headers = std::str::from_utf8(&headers).unwrap();
            assert!(headers.starts_with("POST /compatible-mode/v1/chat/completions HTTP/1.1\r\n"));
            assert!(headers.contains("Authorization: Bearer test-asr-credential\r\n"));
            assert!(headers.contains("Content-Type: application/json\r\n"));
            let body = serde_json::to_vec(&response_body).unwrap();
            write!(
                stream,
                "HTTP/1.1 {status} Test\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                body.len()
            )
            .unwrap();
            stream.write_all(&body).unwrap();
            stream.flush().unwrap();
        });
        (port, client, handle)
    }

    fn test_asr_client(port: u16, tls_config: Arc<ClientConfig>) -> DashScopeAsrClient {
        DashScopeAsrClient {
            timeout: Duration::from_secs(2),
            test_platform: Some(TestPlatform {
                target: HandshakeTarget {
                    host: "localhost".into(),
                    port,
                    request_target: ASR_REQUEST_TARGET.into(),
                },
                tls_config,
                proxy: None,
                setup_delay: Duration::ZERO,
            }),
        }
    }

    fn minimal_wav() -> Vec<u8> {
        let mut wav = vec![0_u8; 44];
        wav[..4].copy_from_slice(b"RIFF");
        wav[8..12].copy_from_slice(b"WAVE");
        wav
    }

    #[derive(Default)]
    struct MemorySecrets(RefCell<HashMap<String, Vec<u8>>>);

    impl SecretStore for MemorySecrets {
        fn get(&self, account: &str) -> Result<Option<SecretBytes>, SecretStoreError> {
            Ok(self.0.borrow().get(account).cloned().map(SecretBytes::new))
        }

        fn create(&self, account: &str, secret: &[u8]) -> Result<(), SecretStoreError> {
            self.0.borrow_mut().insert(account.into(), secret.to_vec());
            Ok(())
        }

        fn delete(&self, account: &str) -> Result<(), SecretStoreError> {
            self.0.borrow_mut().remove(account);
            Ok(())
        }
    }

    fn tts_accounts_and_store() -> (KeychainAccounts, MemorySecrets) {
        let accounts = KeychainAccounts {
            dashscope: "dashscope-test".into(),
            dashscope_verified: "dashscope-marker-test".into(),
            cache_key: "cache-test".into(),
            device_secret: "device-test".into(),
        };
        let store = MemorySecrets::default();
        store
            .create(&accounts.dashscope, b"test-tts-credential")
            .unwrap();
        store
            .create(&accounts.dashscope_verified, b"verified-v1")
            .unwrap();
        (accounts, store)
    }

    fn request() -> TtsRequest<'static> {
        TtsRequest {
            text: "test summary",
            voice: "Cherry",
            instructions: "calm and concise",
        }
    }

    fn serve_websocket(stream: TcpStream, tls: Arc<ServerConfig>, script: ServerScript) {
        let connection = ServerConnection::new(tls).unwrap();
        let mut stream = StreamOwned::new(connection, stream);
        let request = read_headers(&mut stream, MAX_HTTP_HEADER_BYTES);
        let request = std::str::from_utf8(&request).unwrap();
        assert!(request.contains("Authorization: Bearer test-credential-1"));

        if let ServerScript::Http(status) = script {
            write!(
                stream,
                "HTTP/1.1 {status} Rejected\r\nContent-Length: 0\r\n\r\n"
            )
            .unwrap();
            stream.flush().unwrap();
            return;
        }

        let websocket_key = request
            .lines()
            .find_map(|line| line.strip_prefix("Sec-WebSocket-Key: "))
            .unwrap();
        write!(
            stream,
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
            websocket_accept(websocket_key)
        )
        .unwrap();
        let event = format!(
            r#"{{"event_id":"event-integration","type":"session.created","session":{{"model":"{TTS_MODEL}"}}}}"#
        );
        let split = event.len() / 2;
        write_server_frame(&mut stream, 0x1, false, &event.as_bytes()[..split]);
        write_server_frame(&mut stream, 0x9, true, b"probe");
        write_server_frame(&mut stream, 0x0, true, &event.as_bytes()[split..]);
        read_client_close(&mut stream);
        match script {
            ServerScript::Success => {
                write_server_frame(&mut stream, 0x8, true, &1000_u16.to_be_bytes())
            }
            ServerScript::InvalidClose => {
                write_server_frame(&mut stream, 0x8, true, &2999_u16.to_be_bytes())
            }
            ServerScript::MissingClose => {}
            ServerScript::Http(_) | ServerScript::Proxy407 => unreachable!(),
        }
    }

    fn spawn_test_peer(
        script: ServerScript,
        behind_proxy: bool,
    ) -> (u16, Arc<ClientConfig>, JoinHandle<()>) {
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = listener.local_addr().unwrap().port();
        let (client, server) = test_tls_configs();
        let handle = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            if behind_proxy {
                let request = read_headers(&mut stream, MAX_PROXY_HEADER_BYTES);
                assert!(request.starts_with(b"CONNECT localhost:443 HTTP/1.1\r\n"));
                if matches!(script, ServerScript::Proxy407) {
                    stream
                        .write_all(b"HTTP/1.1 407 Proxy Authentication Required\r\n\r\n")
                        .unwrap();
                    return;
                }
                stream
                    .write_all(b"HTTP/1.1 200 Connection Established\r\n\r\n")
                    .unwrap();
                stream.flush().unwrap();
            }
            serve_websocket(stream, server, script);
        });
        (port, client, handle)
    }

    fn test_handshake(
        port: u16,
        tls_config: Arc<ClientConfig>,
        behind_proxy: bool,
        timeout: Duration,
        setup_delay: Duration,
    ) -> DashScopeHandshake {
        DashScopeHandshake {
            timeout,
            test_platform: Some(TestPlatform {
                target: HandshakeTarget {
                    host: "localhost".into(),
                    port: if behind_proxy { 443 } else { port },
                    request_target: REQUEST_TARGET.into(),
                },
                tls_config,
                proxy: behind_proxy.then_some(ProxyEndpoint {
                    host: "localhost".into(),
                    port,
                }),
                setup_delay,
            }),
        }
    }

    struct ScriptedStream {
        input: Cursor<Vec<u8>>,
        output: Vec<u8>,
    }

    impl ScriptedStream {
        fn new(input: Vec<u8>) -> Self {
            Self {
                input: Cursor::new(input),
                output: Vec::new(),
            }
        }
    }

    impl Read for ScriptedStream {
        fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
            self.input.read(buffer)
        }
    }

    impl Write for ScriptedStream {
        fn write(&mut self, buffer: &[u8]) -> std::io::Result<usize> {
            self.output.extend_from_slice(buffer);
            Ok(buffer.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn endpoint_is_beijing_wss_and_pins_requested_model() {
        assert!(BEIJING_REALTIME_ENDPOINT.starts_with("wss://dashscope.aliyuncs.com/"));
        assert!(BEIJING_REALTIME_ENDPOINT.ends_with(&format!("model={TTS_MODEL}")));
        assert_eq!(
            TTS_MODEL_SNAPSHOT,
            "qwen3-tts-instruct-flash-realtime-2026-01-22"
        );
        assert!(build_tls_config().is_ok());
    }

    #[test]
    fn realtime_tts_reads_keychain_runs_commit_state_machine_and_returns_pcm() {
        let (port, tls, peer) = spawn_tts_peer(vec![TtsServerScript::Success]);
        let client = test_tts_client(port, tls);
        let (accounts, store) = tts_accounts_and_store();
        let audio = client.synthesize(&store, &accounts, request()).unwrap();
        assert_eq!(audio.pcm(), [0, 0, 1, 0, 2, 0, 3, 0]);
        assert_eq!(audio.receipt().model, TTS_MODEL);
        assert_eq!(audio.receipt().sample_rate, 48_000);
        assert_eq!(audio.receipt().samples, 4);
        assert_eq!(audio.receipt().characters, Some(12));
        assert_eq!(audio.receipt().attempts, 1);
        peer.join().unwrap();
    }

    #[test]
    fn pre_audio_429_and_5xx_retry_but_success_is_not_duplicated() {
        let (port, tls, peer) = spawn_tts_peer(vec![
            TtsServerScript::Http(429),
            TtsServerScript::Http(503),
            TtsServerScript::Success,
        ]);
        let client = test_tts_client(port, tls);
        let (accounts, store) = tts_accounts_and_store();
        let audio = client.synthesize(&store, &accounts, request()).unwrap();
        assert_eq!(audio.receipt().attempts, 3);
        assert_eq!(audio.pcm().len(), 8);
        peer.join().unwrap();
    }

    #[test]
    fn long_tts_text_is_split_without_loss_and_combines_complete_pcm() {
        let text = format!(
            "{}。{}。{}",
            "甲".repeat(MAX_TTS_CHUNK_CHARS - 1),
            "乙".repeat(MAX_TTS_CHUNK_CHARS - 1),
            "丙".repeat(9)
        );
        let chunks = split_tts_text(&text, MAX_TTS_CHUNK_CHARS);
        assert_eq!(chunks.concat(), text);
        assert!(
            chunks
                .iter()
                .all(|chunk| chunk.chars().count() <= MAX_TTS_CHUNK_CHARS)
        );

        let audio = combine_tts_audio(
            chunks
                .iter()
                .map(|chunk| {
                    TtsAudio::from_test(
                        vec![0, 0, 1, 0, 2, 0, 3, 0],
                        TtsReceipt {
                            model: TTS_MODEL,
                            voice: "Cherry".into(),
                            sample_rate: TTS_SAMPLE_RATE,
                            samples: 4,
                            characters: Some(chunk.chars().count() as u64),
                            transport: "fake",
                            attempts: 1,
                        },
                    )
                })
                .collect(),
        )
        .unwrap();
        assert_eq!(audio.pcm().len(), chunks.len() * 8);
        assert_eq!(audio.receipt().attempts, chunks.len() as u8);
        assert_eq!(
            audio.receipt().characters,
            Some(text.chars().count() as u64)
        );
    }

    #[test]
    fn tts_http_auth_rejection_is_terminal_and_not_retried() {
        let (port, tls, peer) = spawn_tts_peer(vec![TtsServerScript::Http(403)]);
        let client = test_tts_client(port, tls);
        let (accounts, store) = tts_accounts_and_store();
        assert!(matches!(
            client.synthesize(&store, &accounts, request()),
            Err(TtsError::Rejected)
        ));
        peer.join().unwrap();
    }

    #[test]
    fn ambiguous_disconnect_after_commit_is_never_retried() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = listener.local_addr().unwrap().port();
        let (tls, server_tls) = test_tls_configs();
        let peer = std::thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            serve_tts_connection(stream, server_tls, TtsServerScript::DisconnectAfterCommit);
            listener.set_nonblocking(true).unwrap();
            let deadline = Instant::now() + Duration::from_millis(100);
            while Instant::now() < deadline {
                if listener.accept().is_ok() {
                    return true;
                }
                std::thread::sleep(Duration::from_millis(2));
            }
            false
        });
        let client = test_tts_client(port, tls);
        let (accounts, store) = tts_accounts_and_store();
        assert!(matches!(
            client.synthesize(&store, &accounts, request()),
            Err(TtsError::AmbiguousAfterCommit)
        ));
        assert!(
            !peer.join().unwrap(),
            "request retried after ambiguous commit"
        );
    }

    #[test]
    fn stalled_session_after_audio_obeys_absolute_deadline() {
        let (port, tls, peer) = spawn_tts_peer(vec![TtsServerScript::StallAfterDelta]);
        let mut client = test_tts_client(port, tls);
        client.timeout = Duration::from_secs(2);
        client.max_attempts = 1;
        let (accounts, store) = tts_accounts_and_store();
        let started = Instant::now();
        assert!(matches!(
            client.synthesize(&store, &accounts, request()),
            Err(TtsError::AmbiguousAfterCommit)
        ));
        assert!(started.elapsed() < Duration::from_millis(2_500));
        peer.join().unwrap();
    }

    #[test]
    fn malformed_audio_order_base64_and_duplicate_events_fail_closed() {
        for script in [
            TtsServerScript::InvalidBase64,
            TtsServerScript::AudioDoneBeforeDelta,
            TtsServerScript::MissingAudioDone,
            TtsServerScript::DuplicateEventId,
            TtsServerScript::DuplicateOutputItemAdded,
            TtsServerScript::ContentPartBeforeOutputItem,
            TtsServerScript::MissingDoneScaffold,
        ] {
            let (port, tls, peer) = spawn_tts_peer(vec![script]);
            let mut client = test_tts_client(port, tls);
            client.max_attempts = 1;
            let (accounts, store) = tts_accounts_and_store();
            assert!(matches!(
                client.synthesize(&store, &accounts, request()),
                Err(TtsError::AmbiguousAfterCommit)
            ));
            peer.join().unwrap();
        }
    }

    #[test]
    fn complete_response_is_accepted_when_only_session_cleanup_disconnects() {
        let (port, tls, peer) = spawn_tts_peer(vec![TtsServerScript::DisconnectAfterResponseDone]);
        let client = test_tts_client(port, tls);
        let (accounts, store) = tts_accounts_and_store();
        let audio = client.synthesize(&store, &accounts, request()).unwrap();
        assert_eq!(audio.pcm().len(), 8);
        assert_eq!(audio.receipt().voice, "Cherry");
        peer.join().unwrap();
    }

    #[test]
    fn session_event_and_audio_delta_counts_are_bounded() {
        let mut state = TtsSessionState {
            model: TTS_MODEL,
            session_id: "sess-test".into(),
            response_id: Some("resp-test".into()),
            item_id: Some("item-test".into()),
            committed: true,
            request_submitted: true,
            output_item_added: true,
            content_part_added: true,
            audio_done: false,
            content_part_done: false,
            output_item_done: false,
            response_done: false,
            received_audio: false,
            characters: None,
            server_event_ids: BTreeSet::new(),
            server_events: MAX_TTS_SERVER_EVENTS,
            audio_deltas: MAX_TTS_AUDIO_DELTAS,
            pcm: Zeroizing::new(Vec::new()),
        };
        let server_event = serde_json::json!({"event_id": "event-over-limit"});
        assert!(matches!(
            register_event(&mut state, &server_event),
            Err(AttemptFailure {
                error: TtsError::AmbiguousAfterCommit,
                retryable: false
            })
        ));
        let mut delta = serde_json::json!({
            "response_id": "resp-test",
            "item_id": "item-test",
            "output_index": 0,
            "content_index": 0,
            "delta": BASE64.encode([0_u8, 0])
        });
        assert!(matches!(
            append_audio_delta(&mut delta, &mut state),
            Err(AttemptFailure {
                error: TtsError::AmbiguousAfterCommit,
                retryable: false
            })
        ));
        let service_error = serde_json::json!({"error": {"code": "rate_limit"}});
        assert!(matches!(
            classify_tts_service_error(&service_error, true),
            AttemptFailure {
                error: TtsError::AmbiguousAfterCommit,
                retryable: false
            }
        ));
    }

    #[test]
    fn credential_marker_and_bounded_request_are_required_before_network() {
        let accounts = KeychainAccounts {
            dashscope: "dashscope-test".into(),
            dashscope_verified: "dashscope-marker-test".into(),
            cache_key: "cache-test".into(),
            device_secret: "device-test".into(),
        };
        let store = MemorySecrets::default();
        let client = DashScopeTtsClient::default();
        assert!(matches!(
            client.synthesize(&store, &accounts, request()),
            Err(TtsError::Credential)
        ));
        let invalid = TtsRequest {
            text: "",
            ..request()
        };
        assert!(matches!(
            client.synthesize(&store, &accounts, invalid),
            Err(TtsError::InvalidRequest)
        ));
        let invalid = TtsRequest {
            voice: "../voice",
            ..request()
        };
        assert!(matches!(
            client.synthesize(&store, &accounts, invalid),
            Err(TtsError::InvalidRequest)
        ));
    }

    #[test]
    fn verify_runs_direct_and_proxy_dns_connect_tls_upgrade_frames_and_close() {
        for behind_proxy in [false, true] {
            let (port, client, peer) = spawn_test_peer(ServerScript::Success, behind_proxy);
            let verifier = test_handshake(
                port,
                client,
                behind_proxy,
                Duration::from_secs(2),
                Duration::ZERO,
            );
            let receipt = verifier
                .verify(&SecretBytes::new(b"test-credential-1".to_vec()))
                .unwrap();
            assert_eq!(receipt.event_id, "event-integration");
            assert_eq!(receipt.model, TTS_MODEL);
            assert_eq!(
                receipt.transport,
                if behind_proxy {
                    "https_proxy"
                } else {
                    "direct"
                }
            );
            peer.join().unwrap();
        }
    }

    #[test]
    fn verify_maps_http_rejection_and_fails_proxy_auth_closed() {
        for status in [401, 403] {
            let (port, client, peer) = spawn_test_peer(ServerScript::Http(status), false);
            let result =
                test_handshake(port, client, false, Duration::from_secs(2), Duration::ZERO)
                    .verify(&SecretBytes::new(b"test-credential-1".to_vec()));
            assert!(matches!(result, Err(VerificationError::Rejected)));
            peer.join().unwrap();
        }

        let (port, client, peer) = spawn_test_peer(ServerScript::Proxy407, true);
        let result = test_handshake(port, client, true, Duration::from_secs(2), Duration::ZERO)
            .verify(&SecretBytes::new(b"test-credential-1".to_vec()));
        assert!(matches!(result, Err(VerificationError::Unavailable)));
        peer.join().unwrap();
    }

    #[test]
    fn verify_rejects_invalid_or_missing_peer_close() {
        for script in [ServerScript::InvalidClose, ServerScript::MissingClose] {
            let (port, client, peer) = spawn_test_peer(script, false);
            let result =
                test_handshake(port, client, false, Duration::from_secs(2), Duration::ZERO)
                    .verify(&SecretBytes::new(b"test-credential-1".to_vec()));
            assert!(matches!(
                result,
                Err(VerificationError::Protocol | VerificationError::Unavailable)
            ));
            peer.join().unwrap();
        }
    }

    #[test]
    fn verify_absolute_deadline_covers_platform_setup_and_connect_stall() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = listener.local_addr().unwrap().port();
        listener.set_nonblocking(true).unwrap();
        let (release_sender, release_receiver) = mpsc::channel();
        let stalled_peer = std::thread::spawn(move || {
            let accept_deadline = Instant::now() + Duration::from_secs(5);
            loop {
                match listener.accept() {
                    Ok((_stream, _)) => {
                        let _ = release_receiver.recv_timeout(Duration::from_secs(5));
                        return true;
                    }
                    Err(error)
                        if error.kind() == std::io::ErrorKind::WouldBlock
                            && Instant::now() < accept_deadline =>
                    {
                        std::thread::sleep(Duration::from_millis(5));
                    }
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => return false,
                    Err(error) => panic!("stalled peer accept failed: {error}"),
                }
            }
        });
        let (client, _) = test_tls_configs();
        let verifier = test_handshake(
            port,
            client,
            true,
            Duration::from_secs(3),
            Duration::from_millis(1_500),
        );
        let started = Instant::now();
        let result = verifier.verify(&SecretBytes::new(b"test-credential-1".to_vec()));
        let elapsed = started.elapsed();
        let _ = release_sender.send(());
        let accepted = stalled_peer.join().unwrap();

        assert!(matches!(result, Err(VerificationError::Unavailable)));
        assert!(accepted, "test peer never observed the proxy connection");
        assert!(elapsed < Duration::from_millis(3_750));
    }

    #[test]
    fn handshake_response_requires_upgrade_and_matching_accept() {
        let key = "dGhlIHNhbXBsZSBub25jZQ==";
        let accept = websocket_accept(key);
        let valid = format!(
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n"
        );
        assert!(validate_handshake_response(valid.as_bytes(), key).is_ok());
        let http_10 = valid.replacen("HTTP/1.1", "HTTP/1.0", 1);
        assert!(matches!(
            validate_handshake_response(http_10.as_bytes(), key),
            Err(VerificationError::Protocol)
        ));
        assert!(matches!(
            validate_handshake_response(b"HTTP/1.1 401 Unauthorized\r\n\r\n", key),
            Err(VerificationError::Rejected)
        ));
        assert!(validate_handshake_response(
            b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: wrong\r\n\r\n",
            key
        )
        .is_err());
        for invalid in [
            format!(
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\nSec-WebSocket-Accept: {accept}\r\n\r\n"
            ),
            format!(
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n"
            ),
            format!(
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\nSec-WebSocket-Protocol: unexpected\r\n\r\n"
            ),
        ] {
            assert!(validate_handshake_response(invalid.as_bytes(), key).is_err());
        }
    }

    #[test]
    fn session_created_requires_authoritative_event_and_requested_model() {
        let payload = format!(
            r#"{{"event_id":"event-authoritative","type":"session.created","session":{{"model":"{TTS_MODEL}"}}}}"#
        );
        assert!(matches!(
            classify_server_event(payload.as_bytes()),
            Ok(ServerEvent::Created { event_id, model })
                if event_id == "event-authoritative" && model == TTS_MODEL
        ));
        assert!(
            classify_server_event(
                br#"{"type":"session.created","session":{"model":"wrong-model"}}"#
            )
            .is_err()
        );
        assert!(matches!(
            classify_server_event(br#"{"type":"error","error":{"code":"server_error"}}"#),
            Err(VerificationError::Protocol)
        ));

        for event_id in [
            "ok\nstatus=injected",
            "ok\u{1b}[31m",
            "ok\u{0085}next",
            "has space",
        ] {
            let payload = serde_json::json!({
                "event_id": event_id,
                "type": "session.created",
                "session": { "model": TTS_MODEL }
            });
            assert!(matches!(
                classify_server_event(payload.to_string().as_bytes()),
                Err(VerificationError::Protocol)
            ));
        }
        assert!(valid_service_event_id("event-01_test.example:reply"));
    }

    #[test]
    fn frame_parser_accepts_fragmentation_and_rejects_masked_or_oversized_events() {
        let mut pending = vec![0x81, 0x02, b'{', b'}'];
        let frame = read_server_frame(&mut std::io::empty(), &mut pending).unwrap();
        assert_eq!(frame.opcode, 1);
        assert_eq!(frame.payload, b"{}");

        let mut first = vec![0x01, 0x03, b'{', b'"', b't'];
        let mut continuation = vec![0x80, 0x06, b'"', b':', b'1', b'}', b' ', b' '];
        let mut fragments = None;
        let frame = read_server_frame(&mut std::io::empty(), &mut first).unwrap();
        assert!(
            assemble_text_frame(frame, &mut fragments)
                .unwrap()
                .is_none()
        );
        let frame = read_server_frame(&mut std::io::empty(), &mut continuation).unwrap();
        assert_eq!(
            assemble_text_frame(frame, &mut fragments).unwrap().unwrap(),
            b"{\"t\":1}  "
        );

        for invalid in [vec![0x81, 0x80], vec![0x09, 0]] {
            assert!(read_server_frame(&mut std::io::empty(), &mut invalid.clone()).is_err());
        }
        let mut extended_ping = vec![0x89, 126, 0, 1, b'x'];
        assert!(read_server_frame(&mut std::io::empty(), &mut extended_ping).is_err());
        let mut non_minimal_16 = vec![0x81, 126, 0, 125];
        non_minimal_16.extend_from_slice(&[b'x'; 125]);
        assert!(read_server_frame(&mut std::io::empty(), &mut non_minimal_16).is_err());
        let mut non_minimal_64 = vec![0x81, 127];
        non_minimal_64.extend_from_slice(&65_535_u64.to_be_bytes());
        assert!(read_server_frame(&mut std::io::empty(), &mut non_minimal_64).is_err());
        let mut high_bit_length = vec![0x81, 127];
        high_bit_length.extend_from_slice(&(1_u64 << 63).to_be_bytes());
        assert!(read_server_frame(&mut std::io::empty(), &mut high_bit_length).is_err());
        let mut oversized = vec![0x81, 127];
        oversized.extend_from_slice(&((MAX_SERVER_EVENT_BYTES as u64) + 1).to_be_bytes());
        assert!(read_server_frame(&mut std::io::empty(), &mut oversized).is_err());
    }

    #[test]
    fn client_control_frames_are_masked_and_close_payloads_are_validated() {
        let payload = 1000_u16.to_be_bytes();
        let mut output = Vec::new();
        write_masked_close(&mut output, &payload).unwrap();
        assert_eq!(output[0], 0x88);
        assert_eq!(output[1] & 0x80, 0x80);
        assert_eq!(output[1] & 0x7f, payload.len() as u8);
        let mask = &output[2..6];
        let decoded = output[6..]
            .iter()
            .enumerate()
            .map(|(index, byte)| byte ^ mask[index % 4])
            .collect::<Vec<_>>();
        assert_eq!(decoded, payload);

        assert!(validate_peer_close_payload(&[]).is_ok());
        assert!(validate_peer_close_payload(&[0x03, 0xE8]).is_ok());
        assert!(validate_peer_close_payload(&[0x03]).is_err());
        assert!(validate_peer_close_payload(&1010_u16.to_be_bytes()).is_err());
        assert!(validate_peer_close_payload(&1005_u16.to_be_bytes()).is_err());
        assert!(validate_peer_close_payload(&2000_u16.to_be_bytes()).is_err());
        assert!(validate_peer_close_payload(&3000_u16.to_be_bytes()).is_ok());
        assert!(validate_peer_close_payload(&3001_u16.to_be_bytes()).is_ok());
        assert!(validate_peer_close_payload(&3002_u16.to_be_bytes()).is_ok());
        assert!(validate_peer_close_payload(&3003_u16.to_be_bytes()).is_ok());
        assert!(validate_peer_close_payload(&3008_u16.to_be_bytes()).is_ok());
        assert!(validate_peer_close_payload(&3999_u16.to_be_bytes()).is_ok());
        assert!(validate_peer_close_payload(&2999_u16.to_be_bytes()).is_err());
        assert!(validate_peer_close_payload(&4000_u16.to_be_bytes()).is_ok());
        assert!(validate_peer_close_payload(&4999_u16.to_be_bytes()).is_ok());
        assert!(validate_peer_close_payload(&5000_u16.to_be_bytes()).is_err());
        assert!(validate_peer_close_payload(&[0x03, 0xE8, 0xFF]).is_err());
    }

    #[test]
    fn successful_client_close_waits_for_a_valid_peer_close() {
        let mut stream = ScriptedStream::new(vec![0x89, 0x01, b'x', 0x88, 0x02, 0x03, 0xE8]);
        await_peer_close(&mut stream, &mut Vec::new()).unwrap();
        assert_eq!(stream.output[0], 0x8A);
        assert_eq!(stream.output[1] & 0x80, 0x80);

        let mut missing = ScriptedStream::new(Vec::new());
        assert!(await_peer_close(&mut missing, &mut Vec::new()).is_err());

        let mut invalid = ScriptedStream::new(vec![0x88, 0x02, 0x07, 0xD0]);
        assert!(await_peer_close(&mut invalid, &mut Vec::new()).is_err());
    }

    #[test]
    fn blocking_platform_setup_obeys_the_absolute_deadline() {
        let started = Instant::now();
        let result = run_blocking_with_deadline(
            started + Duration::from_millis(100),
            || -> Result<(), VerificationError> {
                std::thread::sleep(Duration::from_secs(2));
                Ok(())
            },
        );
        assert!(matches!(result, Err(VerificationError::Unavailable)));
        assert!(started.elapsed() < Duration::from_secs(1));
    }

    #[test]
    fn proxy_parser_supports_local_http_proxy_and_no_proxy() {
        assert_eq!(
            parse_proxy(Some("http://127.0.0.1:7890")).unwrap(),
            Some(ProxyEndpoint {
                host: "127.0.0.1".into(),
                port: 7890
            })
        );
        assert_eq!(
            parse_proxy(Some("http://proxy.example/")).unwrap(),
            Some(ProxyEndpoint {
                host: "proxy.example".into(),
                port: 80
            })
        );
        assert!(parse_proxy(Some("socks5://127.0.0.1:7890")).is_err());
        assert!(parse_proxy(Some("https://127.0.0.1:7890")).is_err());
        assert!(parse_proxy(Some("http://user:secret@proxy:8080")).is_err());
        assert!(bypasses_proxy("localhost,.aliyuncs.com", HOST, PORT));
        assert!(bypasses_proxy("*.aliyuncs.com", HOST, PORT));
        assert!(bypasses_proxy("aliyuncs.com", HOST, PORT));
        assert!(bypasses_proxy("dashscope.aliyuncs.com:443", HOST, PORT));
        assert!(!bypasses_proxy("dashscope.aliyuncs.com:80", HOST, PORT));
        assert!(!bypasses_proxy("localhost,example.com", HOST, PORT));
        assert!(!bypasses_proxy("notaliyuncs.com", HOST, PORT));
    }

    #[test]
    fn asr_sync_http_returns_normalized_text_and_transport() {
        let (port, tls, peer) = spawn_asr_peer(
            200,
            serde_json::json!({
                "choices": [{"message": {"content": "  请继续完成测试。\n"}}]
            }),
        );
        let transcript = test_asr_client(port, tls)
            .transcribe_wav(
                &SecretBytes::new(b"test-asr-credential".to_vec()),
                &minimal_wav(),
            )
            .unwrap();
        assert_eq!(transcript.text, "请继续完成测试。");
        assert_eq!(transcript.model, ASR_MODEL);
        assert_eq!(transcript.transport, "direct");
        peer.join().unwrap();
    }

    #[test]
    fn asr_http_rejections_and_invalid_audio_fail_closed() {
        let secret = SecretBytes::new(b"test-asr-credential".to_vec());
        assert_eq!(
            DashScopeAsrClient::default().transcribe_wav(&secret, b"not a wav"),
            Err(AsrError::InvalidAudio)
        );
        let (port, tls, peer) = spawn_asr_peer(401, serde_json::json!({}));
        assert_eq!(
            test_asr_client(port, tls).transcribe_wav(&secret, &minimal_wav()),
            Err(AsrError::Rejected)
        );
        peer.join().unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn proxy_environment_rejects_non_unicode_and_honors_case_precedence() {
        use std::os::unix::ffi::OsStringExt;

        let missing = EnvironmentValue::Missing;
        let invalid = environment_value(Some(OsString::from_vec(vec![0xFF])));
        let upper = EnvironmentValue::Unicode("http://127.0.0.1:7001".into());
        let lower = EnvironmentValue::Unicode("http://127.0.0.1:7002".into());
        assert!(matches!(
            select_environment_proxy(&missing, &missing, &invalid, &lower),
            Err(VerificationError::Unavailable)
        ));
        assert_eq!(
            select_environment_proxy(&missing, &missing, &upper, &lower).unwrap(),
            EnvironmentProxySelection::Proxy(ProxyEndpoint {
                host: "127.0.0.1".into(),
                port: 7001,
            })
        );
        assert_eq!(
            select_environment_proxy(&missing, &missing, &missing, &lower).unwrap(),
            EnvironmentProxySelection::Proxy(ProxyEndpoint {
                host: "127.0.0.1".into(),
                port: 7002,
            })
        );

        let bypass_upper = EnvironmentValue::Unicode("*.aliyuncs.com".into());
        assert_eq!(
            select_environment_proxy(&bypass_upper, &invalid, &upper, &lower).unwrap(),
            EnvironmentProxySelection::Direct
        );
        assert!(matches!(
            select_environment_proxy(&invalid, &bypass_upper, &upper, &lower),
            Err(VerificationError::Unavailable)
        ));
    }

    #[test]
    fn system_proxy_selection_is_static_or_fails_closed() {
        let static_proxy = SystemProxySettings {
            https_enabled: true,
            https_host: Some("127.0.0.1".into()),
            https_port: Some(7890),
            ..SystemProxySettings::default()
        };
        assert_eq!(
            select_system_proxy(&static_proxy).unwrap(),
            Some(ProxyEndpoint {
                host: "127.0.0.1".into(),
                port: 7890
            })
        );

        for unsupported in [
            SystemProxySettings {
                pac_enabled: true,
                ..SystemProxySettings::default()
            },
            SystemProxySettings {
                auto_discovery_enabled: true,
                ..SystemProxySettings::default()
            },
            SystemProxySettings {
                requires_authentication: true,
                ..static_proxy.clone()
            },
        ] {
            assert!(matches!(
                select_system_proxy(&unsupported),
                Err(VerificationError::Unavailable)
            ));
        }

        let bypassed = SystemProxySettings {
            exceptions: vec!["*.aliyuncs.com".into()],
            pac_enabled: true,
            ..SystemProxySettings::default()
        };
        assert_eq!(select_system_proxy(&bypassed).unwrap(), None);
    }

    #[test]
    fn http_header_scan_enforces_the_exact_boundary() {
        let exact = format!("{}\r\n\r\n", "x".repeat(MAX_HTTP_HEADER_BYTES - 4));
        assert_eq!(
            scan_http_header(exact.as_bytes(), MAX_HTTP_HEADER_BYTES),
            HeaderScan::Complete(MAX_HTTP_HEADER_BYTES)
        );
        let oversized = format!("{}\r\n\r\n", "x".repeat(MAX_HTTP_HEADER_BYTES - 3));
        assert_eq!(
            scan_http_header(oversized.as_bytes(), MAX_HTTP_HEADER_BYTES),
            HeaderScan::TooLarge
        );
        assert_eq!(
            scan_http_header(&vec![b'x'; MAX_HTTP_HEADER_BYTES], MAX_HTTP_HEADER_BYTES),
            HeaderScan::TooLarge
        );
    }

    #[test]
    fn proxy_connect_reader_accepts_partial_headers_and_bounds_stalls() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        let server = std::thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            socket.write_all(b"HTTP/1.1 200 Con").unwrap();
            socket.write_all(b"nection Established\r\n\r\n").unwrap();
        });
        let stream = TcpStream::connect(address).unwrap();
        assert!(
            read_proxy_connect_response(&stream, Instant::now() + Duration::from_secs(1)).is_ok()
        );
        server.join().unwrap();

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        let server = std::thread::spawn(move || {
            let (_socket, _) = listener.accept().unwrap();
            std::thread::sleep(Duration::from_millis(100));
        });
        let stream = TcpStream::connect(address).unwrap();
        assert!(
            read_proxy_connect_response(&stream, Instant::now() + Duration::from_millis(20))
                .is_err()
        );
        server.join().unwrap();
    }

    #[test]
    fn deadline_socket_stops_slow_progress_at_absolute_deadline() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        let server = std::thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            for _ in 0..20 {
                if socket.write_all(b"x").is_err() {
                    break;
                }
                std::thread::sleep(Duration::from_millis(8));
            }
        });
        let stream = TcpStream::connect(address).unwrap();
        let started = Instant::now();
        let mut stream = DeadlineSocket::new(stream, started + Duration::from_millis(35)).unwrap();
        let mut output = [0_u8; 20];
        let error = stream.read_exact(&mut output).unwrap_err();
        assert_eq!(error.kind(), std::io::ErrorKind::TimedOut);
        assert!(started.elapsed() < Duration::from_millis(90));
        server.join().unwrap();
    }
}

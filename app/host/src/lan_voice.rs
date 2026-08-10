use std::collections::{BTreeMap, VecDeque};
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::net::{SocketAddr, UdpSocket};
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, TrySendError};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use hmac::{Hmac, Mac};
use sha1::Sha1;
use sha2::{Digest, Sha256};
use tempfile::Builder;
use thiserror::Error;

use crate::dashscope::{AsrError, DashScopeAsrClient};
use crate::lan_playback::{
    MAILBOX_STATUS_BYTES, MailboxStatus, PLAYBACK_CHUNK_BYTES, PlaybackAck, PlaybackBegin,
    PlaybackFinished, PlaybackIdentity, PlaybackRequest, decode_ack, decode_finished,
    decode_request, encode_begin, encode_data, encode_finished_ack, encode_mailbox_status,
};
use crate::paths::{AppPaths, secure_directory};
use crate::provisioning::{load_device_secret, load_device_secret_path};
use crate::secrets::load_dashscope_env_secret;

pub const LAN_AUDIO_PORT: u16 = 17_333;
const AUDIO_HEADER_BYTES: usize = 32;
const AUDIO_AUTH_TAG_BYTES: usize = 16;
const AUDIO_END_BYTES: usize = AUDIO_HEADER_BYTES + AUDIO_AUTH_TAG_BYTES;
const AUDIO_FRAME_SAMPLES: usize = 320;
const AUDIO_FRAME_BYTES: usize = AUDIO_FRAME_SAMPLES * 2;
const AUDIO_SAMPLE_RATE: u32 = 16_000;
const MAX_CAPTURE_BYTES: usize = AUDIO_SAMPLE_RATE as usize * 2 * 90;
const MIN_CAPTURE_FRAMES: u32 = 10;
const MAX_ACTIVE_CAPTURES: usize = 4;
const MAX_RETIRED_SESSIONS: usize = 128;
const CAPTURE_ABORT_TIMEOUT: Duration = Duration::from_secs(15);
const RECEIVE_POLL: Duration = Duration::from_millis(50);
const AUTH_RELOAD_INTERVAL: Duration = Duration::from_millis(250);
const MAX_TRANSCRIPT_BYTES: usize = 32 * 1024;
const PLAYBACK_RETRY: Duration = Duration::from_millis(250);
const PLAYBACK_MAX_RETRIES: u8 = 16;
const PLAYBACK_FINISH_TIMEOUT: Duration =
    Duration::from_secs(crate::audio::MAX_TTS_SECONDS as u64 + 30);
// Production ESP-IDF config has a six-entry UDP receive mailbox. Never burst
// more datagrams than the device can queue before its main task drains them.
const PLAYBACK_SEND_WINDOW_CHUNKS: usize = 6;
const PLAYBACK_FINISHED_ACK_RETENTION: Duration = Duration::from_secs(120);
const AUTHENTICATED_HEARTBEAT_BYTES: usize = 80;
const HEARTBEAT_AUTH_CONTEXT: &[u8] = b"EasyInput/EISD/v1";
const DEFAULT_MODEL_SHA1: &str = "a3733eda680ef76256db5fc5dd9de8629e62c5e7";
const MIN_MODEL_BYTES: u64 = 1024 * 1024;
const MAX_MODEL_BYTES: u64 = 1024 * 1024 * 1024;

#[derive(Debug, Error)]
pub enum LanVoiceError {
    #[error("LAN voice socket failed")]
    Io(#[from] io::Error),
    #[error("LAN voice packet is invalid")]
    InvalidPacket,
    #[error("LAN voice session identity is invalid")]
    InvalidSession,
    #[error("LAN voice authentication failed")]
    Authentication,
    #[error("LAN voice frame order is invalid")]
    InvalidSequence,
    #[error("LAN voice capture exceeds its limit")]
    CaptureLimit,
    #[error("LAN voice capture is too short")]
    CaptureTooShort,
    #[error("local whisper.cpp is unavailable")]
    AsrUnavailable,
    #[error("local whisper.cpp timed out")]
    AsrTimeout,
    #[error("local whisper.cpp failed")]
    AsrFailed,
    #[error("DashScope rejected the ASR credential")]
    RemoteAsrRejected,
    #[error("DashScope rejected the ASR audio")]
    RemoteAsrInvalidAudio,
    #[error("DashScope returned an invalid ASR response")]
    RemoteAsrProtocol,
    #[error("local whisper.cpp returned invalid text")]
    InvalidTranscript,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CaptureSessionIdentity {
    pub slot: u8,
    pub capture_generation: u32,
    pub connection_generation: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LanVoicePrompt {
    pub slot: u8,
    pub request_id: String,
    pub transcript: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LanPlaybackRequest {
    pub request: PlaybackRequest,
    pub source: SocketAddr,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LanPlaybackEvent {
    Request(LanPlaybackRequest),
    Finished(PlaybackFinished),
    Cancelled(PlaybackIdentity),
}

struct LanPlaybackStart {
    begin: PlaybackBegin,
    source: SocketAddr,
    eiad: zeroize::Zeroizing<Vec<u8>>,
}

enum LanPlaybackCommand {
    Start(LanPlaybackStart),
    FinishAck(PlaybackIdentity),
    Cancel(PlaybackIdentity),
    Mailbox(MailboxStatus),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WhisperModelReceipt {
    pub bytes: u64,
    pub sha1: String,
    pub destination: PathBuf,
}

pub fn import_default_whisper_model(
    paths: &AppPaths,
    source: &Path,
) -> Result<WhisperModelReceipt, LanVoiceError> {
    let metadata = fs::symlink_metadata(source)?;
    if !metadata.is_file()
        || metadata.uid() != unsafe { libc::geteuid() }
        || !(MIN_MODEL_BYTES..=MAX_MODEL_BYTES).contains(&metadata.len())
    {
        return Err(LanVoiceError::AsrUnavailable);
    }
    let model_directory = paths.root.join("models");
    secure_directory(&model_directory)?;
    let destination = model_directory.join("ggml-base-q5_1.bin");
    let mut input = File::open(source)?;
    let mut output = Builder::new()
        .prefix(".ggml-base-")
        .suffix(".tmp")
        .tempfile_in(&model_directory)?;
    output
        .as_file()
        .set_permissions(fs::Permissions::from_mode(0o600))?;
    let mut digest = Sha1::new();
    let mut bytes = 0_u64;
    let mut buffer = [0_u8; 1024 * 1024];
    loop {
        let read = input.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        bytes = bytes
            .checked_add(read as u64)
            .ok_or(LanVoiceError::AsrUnavailable)?;
        if bytes > MAX_MODEL_BYTES {
            return Err(LanVoiceError::AsrUnavailable);
        }
        digest.update(&buffer[..read]);
        output.write_all(&buffer[..read])?;
    }
    buffer.fill(0);
    let sha1 = format!("{:x}", digest.finalize());
    if bytes != metadata.len() || sha1 != DEFAULT_MODEL_SHA1 {
        return Err(LanVoiceError::AsrUnavailable);
    }
    output.as_file().sync_all()?;
    output
        .persist(&destination)
        .map_err(|error| LanVoiceError::Io(error.error))?;
    File::open(&model_directory)?.sync_all()?;
    Ok(WhisperModelReceipt {
        bytes,
        sha1,
        destination,
    })
}

#[derive(Debug, Clone)]
pub struct WhisperCppConfig {
    pub executable: PathBuf,
    pub model: PathBuf,
    pub work_directory: PathBuf,
    pub timeout: Duration,
}

impl WhisperCppConfig {
    pub fn from_paths(paths: &AppPaths) -> Self {
        let executable = std::env::var_os("ECI_WHISPER_EXECUTABLE")
            .map(PathBuf::from)
            .unwrap_or_else(default_whisper_executable);
        let model = std::env::var_os("ECI_WHISPER_MODEL")
            .map(PathBuf::from)
            .unwrap_or_else(|| paths.root.join("models").join("ggml-base-q5_1.bin"));
        Self {
            executable,
            model,
            work_directory: paths.runtime_directory.join("asr"),
            timeout: Duration::from_secs(30),
        }
    }
}

#[derive(Clone)]
pub struct LanVoiceConfig {
    pub bind_port: u16,
    pub whisper: WhisperCppConfig,
    pub dashscope_env: PathBuf,
    pub auth_key: Option<[u8; 32]>,
    pub device_secret_path: PathBuf,
}

impl LanVoiceConfig {
    pub fn from_paths(paths: &AppPaths) -> Self {
        let bind_port = std::env::var("ECI_LAN_AUDIO_PORT")
            .ok()
            .and_then(|value| value.parse::<u16>().ok())
            .unwrap_or(LAN_AUDIO_PORT);
        Self {
            bind_port,
            whisper: WhisperCppConfig::from_paths(paths),
            dashscope_env: paths.dashscope_env.clone(),
            auth_key: load_device_secret(paths).ok(),
            device_secret_path: paths.device_secret.clone(),
        }
    }
}

pub struct LanVoiceIngress {
    receiver: Receiver<LanVoicePrompt>,
    shutdown: Arc<AtomicBool>,
    ingress_worker: Option<JoinHandle<()>>,
    asr_worker: Option<JoinHandle<()>>,
    local_port: u16,
    playback_events: Receiver<LanPlaybackEvent>,
    playback_commands: mpsc::Sender<LanPlaybackCommand>,
}

impl LanVoiceIngress {
    pub fn start(config: LanVoiceConfig) -> Result<Self, LanVoiceError> {
        let socket = UdpSocket::bind(("0.0.0.0", config.bind_port))?;
        socket.set_read_timeout(Some(RECEIVE_POLL))?;
        let local_port = socket.local_addr()?.port();
        let (prompt_sender, receiver) = mpsc::channel();
        let (capture_sender, capture_receiver) = mpsc::sync_channel(MAX_ACTIVE_CAPTURES);
        let (playback_event_sender, playback_events) = mpsc::channel();
        let (playback_commands, playback_command_receiver) = mpsc::channel();
        let shutdown = Arc::new(AtomicBool::new(false));
        let asr_shutdown = Arc::clone(&shutdown);
        let asr_worker = thread::Builder::new()
            .name("eci-lan-asr".to_owned())
            .spawn(move || {
                let transcriber = HybridTranscriber::new(config.dashscope_env, config.whisper);
                while !asr_shutdown.load(Ordering::Acquire) {
                    match capture_receiver.recv_timeout(RECEIVE_POLL) {
                        Ok(capture) => match transcriber.transcribe(capture) {
                            Ok(prompt) => {
                                if prompt_sender.send(prompt).is_err() {
                                    return;
                                }
                            }
                            Err(error) => {
                                eprintln!("lan_voice_rejected={}", error_code(&error));
                            }
                        },
                        Err(mpsc::RecvTimeoutError::Timeout) => {}
                        Err(mpsc::RecvTimeoutError::Disconnected) => return,
                    }
                }
            })?;
        let ingress_shutdown = Arc::clone(&shutdown);
        let ingress_worker = match thread::Builder::new()
            .name("eci-lan-voice".to_owned())
            .spawn(move || {
                let mut assembler = CaptureAssembler::new(config.auth_key);
                let mut playback = ActiveLanPlayback::default();
                let mut next_auth_reload = Instant::now();
                let mut datagram = [0_u8; 1200];
                while !ingress_shutdown.load(Ordering::Acquire) {
                    while let Ok(command) = playback_command_receiver.try_recv() {
                        playback.handle_command(
                            command,
                            &socket,
                            assembler.auth_key.as_ref(),
                            &playback_event_sender,
                        );
                    }
                    let now = Instant::now();
                    if assembler.auth_key.is_none() && now >= next_auth_reload {
                        if let Ok(key) = load_device_secret_path(&config.device_secret_path) {
                            assembler.auth_key = Some(key);
                        }
                        next_auth_reload = now + AUTH_RELOAD_INTERVAL;
                    }
                    match socket.recv_from(&mut datagram) {
                        Ok((length, source)) => {
                            let packet = &datagram[..length];
                            let heartbeat_packet = packet.starts_with(b"EIHB");
                            let playback_packet = packet.len() >= 4 && &packet[..3] == b"EIP";
                            if heartbeat_packet {
                                if let Some(key) = assembler.auth_key.as_ref() {
                                    playback.handle_heartbeat(packet, source, key, &socket);
                                }
                            } else if playback_packet {
                                if let Some(key) = assembler.auth_key.as_ref() {
                                    playback.ingest(
                                        packet,
                                        source,
                                        key,
                                        &socket,
                                        &playback_event_sender,
                                    );
                                }
                            } else if let Err(error) =
                                assembler.ingest(packet, source, Instant::now())
                            {
                                eprintln!("lan_voice_rejected={}", error_code(&error));
                            }
                        }
                        Err(error)
                            if matches!(
                                error.kind(),
                                io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                            ) => {}
                        Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                        Err(_) => break,
                    }
                    for capture in assembler.take_ready() {
                        match capture {
                            Ok(capture) => match capture_sender.try_send(capture) {
                                Ok(()) => {}
                                Err(TrySendError::Full(_)) => {
                                    eprintln!("lan_voice_rejected=capture_queue_full");
                                }
                                Err(TrySendError::Disconnected(_)) => return,
                            },
                            Err(error) => {
                                eprintln!("lan_voice_rejected={}", error_code(&error));
                            }
                        }
                    }
                    for error in assembler.expire_incomplete(Instant::now()) {
                        eprintln!("lan_voice_rejected={}", error_code(&error));
                    }
                    playback.tick(&socket, assembler.auth_key.as_ref(), &playback_event_sender);
                }
            }) {
            Ok(worker) => worker,
            Err(error) => {
                shutdown.store(true, Ordering::Release);
                let _ = asr_worker.join();
                return Err(error.into());
            }
        };
        Ok(Self {
            receiver,
            shutdown,
            ingress_worker: Some(ingress_worker),
            asr_worker: Some(asr_worker),
            local_port,
            playback_events,
            playback_commands,
        })
    }

    pub fn try_recv(&self) -> Option<LanVoicePrompt> {
        self.receiver.try_recv().ok()
    }

    pub fn local_port(&self) -> u16 {
        self.local_port
    }

    pub fn try_recv_playback(&self) -> Option<LanPlaybackEvent> {
        self.playback_events.try_recv().ok()
    }

    pub fn start_playback(
        &self,
        begin: PlaybackBegin,
        source: SocketAddr,
        eiad: zeroize::Zeroizing<Vec<u8>>,
    ) -> bool {
        self.playback_commands
            .send(LanPlaybackCommand::Start(LanPlaybackStart {
                begin,
                source,
                eiad,
            }))
            .is_ok()
    }

    pub fn acknowledge_playback_finished(&self, identity: PlaybackIdentity) -> bool {
        self.playback_commands
            .send(LanPlaybackCommand::FinishAck(identity))
            .is_ok()
    }

    pub fn cancel_playback(&self, identity: PlaybackIdentity) -> bool {
        self.playback_commands
            .send(LanPlaybackCommand::Cancel(identity))
            .is_ok()
    }

    pub fn publish_mailbox_status(&self, status: MailboxStatus) -> bool {
        self.playback_commands
            .send(LanPlaybackCommand::Mailbox(status))
            .is_ok()
    }
}

impl Drop for LanVoiceIngress {
    fn drop(&mut self) {
        self.shutdown.store(true, Ordering::Release);
        if let Some(worker) = self.ingress_worker.take() {
            let _ = worker.join();
        }
        if let Some(worker) = self.asr_worker.take() {
            let _ = worker.join();
        }
    }
}

#[derive(Debug)]
struct ParsedFrame<'a> {
    session_id: u64,
    identity: CaptureSessionIdentity,
    sequence: u32,
    payload: &'a [u8],
}

#[derive(Debug)]
struct ParsedEnd {
    session_id: u64,
    identity: CaptureSessionIdentity,
    final_sequence: u32,
}

#[derive(Debug)]
struct ActiveCapture {
    session_id: u64,
    identity: CaptureSessionIdentity,
    source: SocketAddr,
    next_sequence: u32,
    pcm: Vec<u8>,
    last_frame_at: Instant,
    final_sequence: Option<u32>,
}

#[derive(Debug)]
struct CompletedCapture {
    session_id: u64,
    identity: CaptureSessionIdentity,
    pcm: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PlaybackSendPhase {
    BeginAck,
    DataAck,
    DeviceFinished,
    HostCommit,
}

struct ActivePlaybackTransfer {
    begin: PlaybackBegin,
    source: SocketAddr,
    eiad: zeroize::Zeroizing<Vec<u8>>,
    phase: PlaybackSendPhase,
    acknowledged_offset: usize,
    sent_end_offset: usize,
    last_send: Instant,
    retry_count: u8,
    finish_deadline: Instant,
    started_at: Instant,
}

#[derive(Clone, Copy)]
struct RetiredPlaybackFinish {
    identity: PlaybackIdentity,
    source: SocketAddr,
    expires_at: Instant,
}

#[derive(Default)]
struct ActiveLanPlayback {
    transfer: Option<ActivePlaybackTransfer>,
    retired_finish: Option<RetiredPlaybackFinish>,
    retired_cancel: Option<RetiredPlaybackFinish>,
    accepted_requests: BTreeMap<(u8, u32), u32>,
    mailbox_status: MailboxStatus,
}

impl ActiveLanPlayback {
    fn handle_command(
        &mut self,
        command: LanPlaybackCommand,
        socket: &UdpSocket,
        key: Option<&[u8; 32]>,
        events: &mpsc::Sender<LanPlaybackEvent>,
    ) {
        match command {
            LanPlaybackCommand::Start(start) => {
                if self.transfer.is_some()
                    || start.eiad.is_empty()
                    || start.eiad.len() != start.begin.total_bytes as usize
                {
                    let _ = events.send(LanPlaybackEvent::Cancelled(start.begin.identity));
                    return;
                }
                let Some(key) = key else {
                    let _ = events.send(LanPlaybackEvent::Cancelled(start.begin.identity));
                    return;
                };
                let packet = encode_begin(start.begin, key);
                if socket.send_to(&packet, start.source).is_err() {
                    let _ = events.send(LanPlaybackEvent::Cancelled(start.begin.identity));
                    return;
                }
                self.transfer = Some(ActivePlaybackTransfer {
                    begin: start.begin,
                    source: start.source,
                    eiad: start.eiad,
                    phase: PlaybackSendPhase::BeginAck,
                    acknowledged_offset: 0,
                    sent_end_offset: 0,
                    last_send: Instant::now(),
                    retry_count: 0,
                    finish_deadline: Instant::now() + PLAYBACK_FINISH_TIMEOUT,
                    started_at: Instant::now(),
                });
                eprintln!(
                    "lan_playback=begin_sent slot={} generation={}",
                    start.begin.identity.slot, start.begin.identity.summary_generation
                );
            }
            LanPlaybackCommand::FinishAck(identity) => {
                let exact = self.transfer.as_ref().is_some_and(|transfer| {
                    transfer.begin.identity == identity
                        && transfer.phase == PlaybackSendPhase::HostCommit
                });
                if !exact {
                    return;
                }
                if let (Some(key), Some(transfer)) = (key, self.transfer.as_ref()) {
                    let packet = encode_finished_ack(identity, 0, key);
                    if socket.send_to(&packet, transfer.source).is_ok() {
                        self.retired_finish = Some(RetiredPlaybackFinish {
                            identity,
                            source: transfer.source,
                            expires_at: Instant::now() + PLAYBACK_FINISHED_ACK_RETENTION,
                        });
                        self.transfer = None;
                    }
                }
            }
            LanPlaybackCommand::Cancel(identity) => {
                if self
                    .transfer
                    .as_ref()
                    .is_some_and(|transfer| transfer.begin.identity == identity)
                {
                    self.transfer = None;
                }
            }
            LanPlaybackCommand::Mailbox(status) => self.mailbox_status = status,
        }
    }

    fn handle_heartbeat(
        &self,
        packet: &[u8],
        source: SocketAddr,
        key: &[u8; 32],
        socket: &UdpSocket,
    ) {
        if packet.len() != AUTHENTICATED_HEARTBEAT_BYTES
            || packet[..4] != *b"EIHB"
            || packet[4] != 1
            || packet[5] & !0x03 != 0
            || packet[6..8] != [0, 0]
            || packet[20..24] != *b"EISD"
            || packet[24] != 1
            || packet[25] != 60
            || u16::from_le_bytes(packet[26..28].try_into().unwrap()) & 0x0002 == 0
        {
            return;
        }
        let Ok(mut mac) = Hmac::<Sha256>::new_from_slice(key) else {
            return;
        };
        mac.update(HEARTBEAT_AUTH_CONTEXT);
        mac.update(&packet[..64]);
        let digest = mac.finalize().into_bytes();
        let tag_difference = digest[..16]
            .iter()
            .zip(&packet[64..80])
            .fold(0_u8, |difference, (expected, actual)| {
                difference | (expected ^ actual)
            });
        if tag_difference != 0 {
            return;
        }
        let heartbeat_sequence = u32::from_le_bytes(packet[16..20].try_into().unwrap());
        let Ok(response) = encode_mailbox_status(self.mailbox_status, heartbeat_sequence, key)
        else {
            return;
        };
        debug_assert_eq!(response.len(), MAILBOX_STATUS_BYTES);
        let _ = socket.send_to(&response, source);
    }

    fn ingest(
        &mut self,
        packet: &[u8],
        source: SocketAddr,
        key: &[u8; 32],
        socket: &UdpSocket,
        events: &mpsc::Sender<LanPlaybackEvent>,
    ) {
        if packet.starts_with(b"EIPR") {
            let request = match decode_request(packet, key) {
                Ok(request) => request,
                Err(_) => {
                    eprintln!("lan_playback=request_invalid reason=authentication_or_format");
                    return;
                }
            };
            if !self.accept_request_generation(request) {
                eprintln!("lan_playback=request_replayed slot={}", request.slot);
                return;
            }
            if let Some(transfer) = self.transfer.take() {
                let _ = events.send(LanPlaybackEvent::Cancelled(transfer.begin.identity));
            }
            let _ = events.send(LanPlaybackEvent::Request(LanPlaybackRequest {
                request,
                source,
            }));
            return;
        }
        if packet.starts_with(b"EIPA") {
            let Ok(ack) = decode_ack(packet, key) else {
                return;
            };
            self.accept_ack(ack, source, key, socket, events);
            return;
        }
        if packet.starts_with(b"EIPF") {
            let Ok(finished) = decode_finished(packet, key) else {
                return;
            };
            let Some(transfer) = self.transfer.as_mut() else {
                if self.retired_finish.is_some_and(|retired| {
                    retired.source == source
                        && retired.identity == finished.identity
                        && retired.expires_at > Instant::now()
                }) {
                    let ack = encode_finished_ack(finished.identity, 0, key);
                    let _ = socket.send_to(&ack, source);
                }
                return;
            };
            if transfer.source != source
                || transfer.begin.identity != finished.identity
                || transfer.begin.total_samples != finished.played_samples
                || !matches!(
                    transfer.phase,
                    PlaybackSendPhase::DeviceFinished | PlaybackSendPhase::HostCommit
                )
            {
                return;
            }
            if transfer.phase == PlaybackSendPhase::DeviceFinished {
                transfer.phase = PlaybackSendPhase::HostCommit;
                transfer.finish_deadline = Instant::now() + PLAYBACK_FINISH_TIMEOUT;
                eprintln!(
                    "lan_playback=device_finished slot={} generation={}",
                    finished.identity.slot, finished.identity.summary_generation
                );
                let _ = events.send(LanPlaybackEvent::Finished(finished));
            } else {
                let _ = events.send(LanPlaybackEvent::Finished(finished));
            }
        }
    }

    fn accept_request_generation(&mut self, request: PlaybackRequest) -> bool {
        let key = (request.slot, request.connection_generation);
        if let Some(floor) = self.accepted_requests.get_mut(&key) {
            if request.request_generation <= *floor {
                return false;
            }
            *floor = request.request_generation;
            return true;
        }
        self.accepted_requests
            .insert(key, request.request_generation);
        true
    }

    fn accept_ack(
        &mut self,
        ack: PlaybackAck,
        source: SocketAddr,
        key: &[u8; 32],
        socket: &UdpSocket,
        events: &mpsc::Sender<LanPlaybackEvent>,
    ) {
        let Some(transfer) = self.transfer.as_mut() else {
            if ack.status == 3
                && self.retired_cancel.is_some_and(|retired| {
                    retired.source == source
                        && retired.identity == ack.identity
                        && retired.expires_at > Instant::now()
                })
            {
                let packet = encode_finished_ack(ack.identity, 1, key);
                let _ = socket.send_to(&packet, source);
            }
            return;
        };
        if transfer.source != source || transfer.begin.identity != ack.identity {
            return;
        }
        if ack.status == 1
            && transfer.phase == PlaybackSendPhase::DataAck
            && ack.next_offset as usize >= transfer.acknowledged_offset
            && ack.next_offset as usize <= transfer.sent_end_offset
        {
            let requested_offset = ack.next_offset as usize;
            if requested_offset == transfer.acknowledged_offset {
                if transfer.retry_count >= PLAYBACK_MAX_RETRIES {
                    let identity = transfer.begin.identity;
                    eprintln!(
                        "lan_playback=gap_retry_exhausted slot={} generation={} offset={}",
                        identity.slot, identity.summary_generation, requested_offset
                    );
                    self.transfer = None;
                    let _ = events.send(LanPlaybackEvent::Cancelled(identity));
                    return;
                }
                transfer.retry_count += 1;
            } else {
                transfer.acknowledged_offset = requested_offset;
                transfer.retry_count = 0;
            }
            if !Self::send_next_data(transfer, key, socket) {
                let identity = transfer.begin.identity;
                self.transfer = None;
                let _ = events.send(LanPlaybackEvent::Cancelled(identity));
            }
            return;
        }
        if ack.status != 0 {
            let identity = transfer.begin.identity;
            eprintln!(
                "lan_playback=device_rejected slot={} generation={} status={} diagnostic={}",
                identity.slot,
                identity.summary_generation,
                ack.status,
                if ack.next_offset & 0xFF00_0000 == 0xEC00_0000 {
                    ack.next_offset & 0xFF
                } else {
                    0
                }
            );
            if ack.status == 3 {
                let packet = encode_finished_ack(identity, 1, key);
                let _ = socket.send_to(&packet, source);
                self.retired_cancel = Some(RetiredPlaybackFinish {
                    identity,
                    source,
                    expires_at: Instant::now() + PLAYBACK_FINISHED_ACK_RETENTION,
                });
            }
            self.transfer = None;
            let _ = events.send(LanPlaybackEvent::Cancelled(identity));
            return;
        }
        match transfer.phase {
            PlaybackSendPhase::BeginAck if ack.next_offset == 0 => {
                eprintln!(
                    "lan_playback=begin_ack slot={} generation={}",
                    ack.identity.slot, ack.identity.summary_generation
                );
                transfer.acknowledged_offset = 0;
                transfer.retry_count = 0;
                if !Self::send_next_data(transfer, key, socket) {
                    let identity = transfer.begin.identity;
                    self.transfer = None;
                    let _ = events.send(LanPlaybackEvent::Cancelled(identity));
                }
            }
            PlaybackSendPhase::DataAck
                if ack.next_offset as usize > transfer.acknowledged_offset
                    && ack.next_offset as usize <= transfer.sent_end_offset =>
            {
                transfer.acknowledged_offset = ack.next_offset as usize;
                transfer.retry_count = 0;
                if transfer.acknowledged_offset != transfer.sent_end_offset {
                    return;
                }
                if transfer.acknowledged_offset == transfer.eiad.len() {
                    eprintln!(
                        "lan_playback=transfer_complete slot={} generation={} bytes={} elapsed_ms={}",
                        ack.identity.slot,
                        ack.identity.summary_generation,
                        transfer.acknowledged_offset,
                        transfer.started_at.elapsed().as_millis()
                    );
                    transfer.phase = PlaybackSendPhase::DeviceFinished;
                    transfer.finish_deadline = Instant::now() + PLAYBACK_FINISH_TIMEOUT;
                } else if !Self::send_next_data(transfer, key, socket) {
                    let identity = transfer.begin.identity;
                    self.transfer = None;
                    let _ = events.send(LanPlaybackEvent::Cancelled(identity));
                }
            }
            _ => {}
        }
    }

    fn send_next_data(
        transfer: &mut ActivePlaybackTransfer,
        key: &[u8; 32],
        socket: &UdpSocket,
    ) -> bool {
        let start = transfer.acknowledged_offset;
        let end = start
            .saturating_add(PLAYBACK_CHUNK_BYTES * PLAYBACK_SEND_WINDOW_CHUNKS)
            .min(transfer.eiad.len());
        let mut offset = start;
        while offset < end {
            let chunk_end = (offset + PLAYBACK_CHUNK_BYTES).min(end);
            let Ok(packet) = encode_data(
                transfer.begin.identity,
                transfer.begin.request_nonce,
                offset as u32,
                &transfer.eiad[offset..chunk_end],
                key,
            ) else {
                return false;
            };
            if socket.send_to(&packet, transfer.source).is_err() {
                return false;
            }
            offset = chunk_end;
        }
        transfer.sent_end_offset = end;
        transfer.phase = PlaybackSendPhase::DataAck;
        transfer.last_send = Instant::now();
        true
    }

    fn tick(
        &mut self,
        socket: &UdpSocket,
        key: Option<&[u8; 32]>,
        events: &mpsc::Sender<LanPlaybackEvent>,
    ) {
        if self
            .retired_finish
            .is_some_and(|retired| retired.expires_at <= Instant::now())
        {
            self.retired_finish = None;
        }
        if self
            .retired_cancel
            .is_some_and(|retired| retired.expires_at <= Instant::now())
        {
            self.retired_cancel = None;
        }
        let Some(transfer) = self.transfer.as_mut() else {
            return;
        };
        let now = Instant::now();
        if matches!(
            transfer.phase,
            PlaybackSendPhase::DeviceFinished | PlaybackSendPhase::HostCommit
        ) {
            if now >= transfer.finish_deadline {
                let identity = transfer.begin.identity;
                eprintln!(
                    "lan_playback=device_finish_timeout slot={} generation={}",
                    identity.slot, identity.summary_generation
                );
                self.transfer = None;
                let _ = events.send(LanPlaybackEvent::Cancelled(identity));
            }
            return;
        }
        if now.duration_since(transfer.last_send) < PLAYBACK_RETRY {
            return;
        }
        if transfer.retry_count >= PLAYBACK_MAX_RETRIES {
            let identity = transfer.begin.identity;
            eprintln!(
                "lan_playback=transport_timeout slot={} generation={} phase={:?}",
                identity.slot, identity.summary_generation, transfer.phase
            );
            self.transfer = None;
            let _ = events.send(LanPlaybackEvent::Cancelled(identity));
            return;
        }
        let Some(key) = key else {
            return;
        };
        let sent = match transfer.phase {
            PlaybackSendPhase::BeginAck => socket
                .send_to(&encode_begin(transfer.begin, key), transfer.source)
                .is_ok(),
            PlaybackSendPhase::DataAck => {
                let mut offset = transfer.acknowledged_offset;
                let mut sent = true;
                while offset < transfer.sent_end_offset {
                    let end = (offset + PLAYBACK_CHUNK_BYTES).min(transfer.sent_end_offset);
                    let Ok(packet) = encode_data(
                        transfer.begin.identity,
                        transfer.begin.request_nonce,
                        offset as u32,
                        &transfer.eiad[offset..end],
                        key,
                    ) else {
                        return;
                    };
                    if socket.send_to(&packet, transfer.source).is_err() {
                        sent = false;
                        break;
                    }
                    offset = end;
                }
                sent
            }
            _ => return,
        };
        if sent {
            transfer.retry_count += 1;
            transfer.last_send = now;
        }
    }
}

struct CaptureAssembler {
    active: BTreeMap<u64, ActiveCapture>,
    retired: VecDeque<u64>,
    ready: VecDeque<Result<CompletedCapture, LanVoiceError>>,
    auth_key: Option<[u8; 32]>,
}

impl CaptureAssembler {
    fn new(auth_key: Option<[u8; 32]>) -> Self {
        Self {
            active: BTreeMap::new(),
            retired: VecDeque::new(),
            ready: VecDeque::new(),
            auth_key,
        }
    }

    fn ingest(
        &mut self,
        datagram: &[u8],
        source: SocketAddr,
        now: Instant,
    ) -> Result<(), LanVoiceError> {
        if datagram.len() >= 4 && &datagram[..4] == b"EIAE" {
            return self.ingest_end(datagram, source, now);
        }
        let frame = match parse_audio_frame(datagram, self.auth_key.as_ref()) {
            Ok(frame) => frame,
            Err(LanVoiceError::InvalidPacket)
                if datagram.len() >= 4 && &datagram[..4] == b"EIHB" =>
            {
                return Ok(());
            }
            Err(error) => return Err(error),
        };
        if self.retired.contains(&frame.session_id) {
            return Ok(());
        }
        if !self.active.contains_key(&frame.session_id) {
            if frame.sequence != 0 {
                self.retire(frame.session_id);
                return Err(LanVoiceError::InvalidSequence);
            }
            if self.active.len() >= MAX_ACTIVE_CAPTURES {
                self.retire(frame.session_id);
                return Err(LanVoiceError::CaptureLimit);
            }
            self.active.insert(
                frame.session_id,
                ActiveCapture {
                    session_id: frame.session_id,
                    identity: frame.identity,
                    source,
                    next_sequence: 0,
                    pcm: Vec::with_capacity(AUDIO_FRAME_BYTES * 50),
                    last_frame_at: now,
                    final_sequence: None,
                },
            );
        }

        let capture = self
            .active
            .get_mut(&frame.session_id)
            .ok_or(LanVoiceError::InvalidSession)?;
        if capture.source != source
            || capture.identity != frame.identity
            || frame.sequence != capture.next_sequence
            || capture
                .final_sequence
                .is_some_and(|final_sequence| frame.sequence >= final_sequence)
        {
            self.active.remove(&frame.session_id);
            self.retire(frame.session_id);
            return Err(LanVoiceError::InvalidSequence);
        }
        if capture.pcm.len().saturating_add(frame.payload.len()) > MAX_CAPTURE_BYTES {
            self.active.remove(&frame.session_id);
            self.retire(frame.session_id);
            return Err(LanVoiceError::CaptureLimit);
        }
        capture.pcm.extend_from_slice(frame.payload);
        capture.next_sequence = capture.next_sequence.saturating_add(1);
        capture.last_frame_at = now;
        if capture.final_sequence == Some(capture.next_sequence) {
            self.complete(frame.session_id);
        }
        Ok(())
    }

    fn ingest_end(
        &mut self,
        datagram: &[u8],
        source: SocketAddr,
        now: Instant,
    ) -> Result<(), LanVoiceError> {
        let terminal = parse_audio_end(datagram, self.auth_key.as_ref())?;
        if self.retired.contains(&terminal.session_id) {
            return Ok(());
        }
        let capture = self
            .active
            .get_mut(&terminal.session_id)
            .ok_or(LanVoiceError::InvalidSession)?;
        if capture.source != source
            || capture.identity != terminal.identity
            || terminal.final_sequence < capture.next_sequence
            || terminal.final_sequence as usize * AUDIO_FRAME_BYTES > MAX_CAPTURE_BYTES
        {
            self.active.remove(&terminal.session_id);
            self.retire(terminal.session_id);
            return Err(LanVoiceError::InvalidSequence);
        }
        if let Some(existing) = capture.final_sequence {
            return if existing == terminal.final_sequence {
                capture.last_frame_at = now;
                Ok(())
            } else {
                self.active.remove(&terminal.session_id);
                self.retire(terminal.session_id);
                Err(LanVoiceError::InvalidSequence)
            };
        }
        capture.final_sequence = Some(terminal.final_sequence);
        capture.last_frame_at = now;
        if capture.next_sequence == terminal.final_sequence {
            self.complete(terminal.session_id);
        }
        Ok(())
    }

    fn complete(&mut self, session_id: u64) {
        let Some(capture) = self.active.remove(&session_id) else {
            return;
        };
        self.retire(session_id);
        self.ready
            .push_back(if capture.next_sequence < MIN_CAPTURE_FRAMES {
                Err(LanVoiceError::CaptureTooShort)
            } else {
                Ok(CompletedCapture {
                    session_id: capture.session_id,
                    identity: capture.identity,
                    pcm: capture.pcm,
                })
            });
    }

    fn take_ready(&mut self) -> Vec<Result<CompletedCapture, LanVoiceError>> {
        self.ready.drain(..).collect()
    }

    fn expire_incomplete(&mut self, now: Instant) -> Vec<LanVoiceError> {
        let idle = self
            .active
            .iter()
            .filter_map(|(session, capture)| {
                (now.saturating_duration_since(capture.last_frame_at) >= CAPTURE_ABORT_TIMEOUT)
                    .then_some(*session)
            })
            .collect::<Vec<_>>();
        idle.into_iter()
            .filter_map(|session| {
                self.active.remove(&session)?;
                self.retire(session);
                Some(LanVoiceError::InvalidSequence)
            })
            .collect()
    }

    fn retire(&mut self, session_id: u64) {
        if self.retired.contains(&session_id) {
            return;
        }
        self.retired.push_back(session_id);
        while self.retired.len() > MAX_RETIRED_SESSIONS {
            self.retired.pop_front();
        }
    }
}

struct HybridTranscriber {
    dashscope_env: PathBuf,
    qwen: DashScopeAsrClient,
    whisper: WhisperCppTranscriber,
}

impl HybridTranscriber {
    fn new(dashscope_env: PathBuf, whisper: WhisperCppConfig) -> Self {
        Self {
            dashscope_env,
            qwen: DashScopeAsrClient::default(),
            whisper: WhisperCppTranscriber::new(whisper),
        }
    }

    fn transcribe(&self, capture: CompletedCapture) -> Result<LanVoicePrompt, LanVoiceError> {
        let credential_configured = match fs::symlink_metadata(&self.dashscope_env) {
            Ok(_) => true,
            Err(error) if error.kind() == io::ErrorKind::NotFound => false,
            Err(_) => return Err(LanVoiceError::AsrFailed),
        };
        if credential_configured {
            let secret = load_dashscope_env_secret(&self.dashscope_env)
                .map_err(|_| LanVoiceError::AsrFailed)?;
            let mut wav = encode_wav(&capture.pcm)?;
            let qwen_result = self.qwen.transcribe_wav(&secret, &wav);
            wav.fill(0);
            match qwen_result {
                Ok(transcript) => {
                    eprintln!(
                        "lan_voice_asr={} transport={} slot={}",
                        transcript.model, transcript.transport, capture.identity.slot
                    );
                    return prompt_from_capture(&capture, transcript.text);
                }
                Err(error) => {
                    if !qwen_error_allows_offline_fallback(error) {
                        eprintln!("lan_voice_asr_error={}", asr_error_code(error));
                        return Err(map_qwen_error(error));
                    }
                    eprintln!("lan_voice_asr_fallback={}", asr_error_code(error));
                }
            }
        } else {
            eprintln!("lan_voice_asr_fallback=credential_missing");
        }
        self.whisper.transcribe(&capture)
    }
}

struct WhisperCppTranscriber {
    config: WhisperCppConfig,
}

impl WhisperCppTranscriber {
    fn new(config: WhisperCppConfig) -> Self {
        Self { config }
    }

    fn transcribe(&self, capture: &CompletedCapture) -> Result<LanVoicePrompt, LanVoiceError> {
        if !self.config.executable.is_file() || !self.config.model.is_file() {
            return Err(LanVoiceError::AsrUnavailable);
        }
        secure_directory(&self.config.work_directory)?;
        let mut wav = encode_wav(&capture.pcm)?;
        let mut temporary = Builder::new()
            .prefix("capture-")
            .suffix(".wav")
            .tempfile_in(&self.config.work_directory)?;
        temporary.write_all(&wav)?;
        temporary.as_file().sync_all()?;
        wav.fill(0);

        let mut child = Command::new(&self.config.executable)
            .arg("-m")
            .arg(&self.config.model)
            .arg("-f")
            .arg(temporary.path())
            .args(["-l", "zh", "-nt", "-np", "-t", "4"])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|_| LanVoiceError::AsrUnavailable)?;
        let deadline = Instant::now() + self.config.timeout;
        loop {
            match child.try_wait() {
                Ok(Some(status)) => {
                    let output = child
                        .wait_with_output()
                        .map_err(|_| LanVoiceError::AsrFailed)?;
                    if !status.success() {
                        return Err(LanVoiceError::AsrFailed);
                    }
                    let transcript = String::from_utf8(output.stdout)
                        .map_err(|_| LanVoiceError::InvalidTranscript)?;
                    let transcript = transcript.split_whitespace().collect::<Vec<_>>().join(" ");
                    if transcript.is_empty() || transcript.len() > MAX_TRANSCRIPT_BYTES {
                        return Err(LanVoiceError::InvalidTranscript);
                    }
                    return prompt_from_capture(capture, transcript);
                }
                Ok(None) if Instant::now() < deadline => {
                    thread::sleep(Duration::from_millis(20));
                }
                Ok(None) => {
                    let _ = child.kill();
                    let _ = child.wait();
                    return Err(LanVoiceError::AsrTimeout);
                }
                Err(_) => return Err(LanVoiceError::AsrFailed),
            }
        }
    }
}

fn prompt_from_capture(
    capture: &CompletedCapture,
    transcript: String,
) -> Result<LanVoicePrompt, LanVoiceError> {
    if transcript.is_empty() || transcript.len() > MAX_TRANSCRIPT_BYTES {
        return Err(LanVoiceError::InvalidTranscript);
    }
    Ok(LanVoicePrompt {
        slot: capture.identity.slot,
        request_id: format!("lan-{session:016x}", session = capture.session_id),
        transcript,
    })
}

pub fn decode_capture_session_identity(
    session_id: u64,
) -> Result<CaptureSessionIdentity, LanVoiceError> {
    if (session_id >> 56) as u8 != 0xEC {
        return Err(LanVoiceError::InvalidSession);
    }
    let identity = CaptureSessionIdentity {
        slot: ((session_id >> 52) & 0x0F) as u8,
        connection_generation: ((session_id >> 32) & 0x000F_FFFF) as u32,
        capture_generation: session_id as u32,
    };
    if !(1..=4).contains(&identity.slot)
        || identity.connection_generation == 0
        || identity.capture_generation == 0
    {
        return Err(LanVoiceError::InvalidSession);
    }
    Ok(identity)
}

fn parse_audio_frame<'a>(
    datagram: &'a [u8],
    auth_key: Option<&[u8; 32]>,
) -> Result<ParsedFrame<'a>, LanVoiceError> {
    if datagram.len() != AUDIO_HEADER_BYTES + AUDIO_FRAME_BYTES + AUDIO_AUTH_TAG_BYTES
        || &datagram[..4] != b"EIAU"
        || datagram[4] != 3
        || datagram[5] as usize != AUDIO_HEADER_BYTES
        || datagram[6] != 1
        || datagram[7] != 1
        || read_u32(datagram, 20)? != AUDIO_SAMPLE_RATE
        || read_u16(datagram, 28)? as usize != AUDIO_FRAME_SAMPLES
        || read_u16(datagram, 30)? as usize != AUDIO_FRAME_BYTES
    {
        return Err(LanVoiceError::InvalidPacket);
    }
    verify_audio_authentication(datagram, auth_key)?;
    let session_id = read_u64(datagram, 8)?;
    Ok(ParsedFrame {
        session_id,
        identity: decode_capture_session_identity(session_id)?,
        sequence: read_u32(datagram, 16)?,
        payload: &datagram[AUDIO_HEADER_BYTES..AUDIO_HEADER_BYTES + AUDIO_FRAME_BYTES],
    })
}

fn parse_audio_end(
    datagram: &[u8],
    auth_key: Option<&[u8; 32]>,
) -> Result<ParsedEnd, LanVoiceError> {
    if datagram.len() != AUDIO_END_BYTES
        || &datagram[..4] != b"EIAE"
        || datagram[4] != 3
        || datagram[5] as usize != AUDIO_HEADER_BYTES
        || datagram[6] != 1
        || datagram[7] != 0
        || read_u32(datagram, 20)? != AUDIO_SAMPLE_RATE
        || read_u16(datagram, 28)? != 0
        || read_u16(datagram, 30)? != 0
    {
        return Err(LanVoiceError::InvalidPacket);
    }
    verify_audio_authentication(datagram, auth_key)?;
    let session_id = read_u64(datagram, 8)?;
    Ok(ParsedEnd {
        session_id,
        identity: decode_capture_session_identity(session_id)?,
        final_sequence: read_u32(datagram, 16)?,
    })
}

fn verify_audio_authentication(
    datagram: &[u8],
    auth_key: Option<&[u8; 32]>,
) -> Result<(), LanVoiceError> {
    let key = auth_key.ok_or(LanVoiceError::Authentication)?;
    let authenticated_bytes = datagram
        .len()
        .checked_sub(AUDIO_AUTH_TAG_BYTES)
        .ok_or(LanVoiceError::Authentication)?;
    let mut mac = Hmac::<Sha256>::new_from_slice(key).map_err(|_| LanVoiceError::Authentication)?;
    mac.update(&datagram[..authenticated_bytes]);
    let expected = mac.finalize().into_bytes();
    let mismatch = expected[..AUDIO_AUTH_TAG_BYTES]
        .iter()
        .zip(&datagram[authenticated_bytes..])
        .fold(0_u8, |difference, (left, right)| {
            difference | (left ^ right)
        });
    if mismatch == 0 {
        Ok(())
    } else {
        Err(LanVoiceError::Authentication)
    }
}

fn encode_wav(pcm: &[u8]) -> Result<Vec<u8>, LanVoiceError> {
    let data_bytes = u32::try_from(pcm.len()).map_err(|_| LanVoiceError::CaptureLimit)?;
    let mut wav = Vec::with_capacity(44 + pcm.len());
    wav.extend_from_slice(b"RIFF");
    wav.extend_from_slice(&(36_u32 + data_bytes).to_le_bytes());
    wav.extend_from_slice(b"WAVEfmt \x10\0\0\0\x01\0\x01\0");
    wav.extend_from_slice(&AUDIO_SAMPLE_RATE.to_le_bytes());
    wav.extend_from_slice(&(AUDIO_SAMPLE_RATE * 2).to_le_bytes());
    wav.extend_from_slice(&2_u16.to_le_bytes());
    wav.extend_from_slice(&16_u16.to_le_bytes());
    wav.extend_from_slice(b"data");
    wav.extend_from_slice(&data_bytes.to_le_bytes());
    wav.extend_from_slice(pcm);
    Ok(wav)
}

fn default_whisper_executable() -> PathBuf {
    for candidate in [
        Path::new("/opt/homebrew/bin/whisper-cli"),
        Path::new("/usr/local/bin/whisper-cli"),
    ] {
        if candidate.is_file() {
            return candidate.to_path_buf();
        }
    }
    PathBuf::from("whisper-cli")
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16, LanVoiceError> {
    let value = bytes
        .get(offset..offset + 2)
        .ok_or(LanVoiceError::InvalidPacket)?;
    Ok(u16::from_le_bytes([value[0], value[1]]))
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32, LanVoiceError> {
    let value = bytes
        .get(offset..offset + 4)
        .ok_or(LanVoiceError::InvalidPacket)?;
    Ok(u32::from_le_bytes(
        value.try_into().map_err(|_| LanVoiceError::InvalidPacket)?,
    ))
}

fn read_u64(bytes: &[u8], offset: usize) -> Result<u64, LanVoiceError> {
    let value = bytes
        .get(offset..offset + 8)
        .ok_or(LanVoiceError::InvalidPacket)?;
    Ok(u64::from_le_bytes(
        value.try_into().map_err(|_| LanVoiceError::InvalidPacket)?,
    ))
}

fn error_code(error: &LanVoiceError) -> &'static str {
    match error {
        LanVoiceError::Io(_) => "io",
        LanVoiceError::InvalidPacket => "invalid_packet",
        LanVoiceError::InvalidSession => "invalid_session",
        LanVoiceError::Authentication => "authentication",
        LanVoiceError::InvalidSequence => "invalid_sequence",
        LanVoiceError::CaptureLimit => "capture_limit",
        LanVoiceError::CaptureTooShort => "capture_too_short",
        LanVoiceError::AsrUnavailable => "asr_unavailable",
        LanVoiceError::AsrTimeout => "asr_timeout",
        LanVoiceError::AsrFailed => "asr_failed",
        LanVoiceError::RemoteAsrRejected => "remote_asr_rejected",
        LanVoiceError::RemoteAsrInvalidAudio => "remote_asr_invalid_audio",
        LanVoiceError::RemoteAsrProtocol => "remote_asr_protocol",
        LanVoiceError::InvalidTranscript => "invalid_transcript",
    }
}

fn qwen_error_allows_offline_fallback(error: AsrError) -> bool {
    matches!(error, AsrError::RateLimited | AsrError::Unavailable)
}

fn map_qwen_error(error: AsrError) -> LanVoiceError {
    match error {
        AsrError::Rejected => LanVoiceError::RemoteAsrRejected,
        AsrError::InvalidAudio => LanVoiceError::RemoteAsrInvalidAudio,
        AsrError::Protocol => LanVoiceError::RemoteAsrProtocol,
        AsrError::RateLimited | AsrError::Unavailable => LanVoiceError::AsrUnavailable,
    }
}

fn asr_error_code(error: AsrError) -> &'static str {
    match error {
        AsrError::InvalidAudio => "invalid_audio",
        AsrError::Rejected => "credential_rejected",
        AsrError::RateLimited => "rate_limited",
        AsrError::Unavailable => "unavailable",
        AsrError::Protocol => "protocol",
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::os::unix::fs::PermissionsExt;

    use tempfile::tempdir;

    use super::*;
    use crate::lan_playback::{
        decode_begin, decode_data, decode_finished_ack, encode_ack, encode_finished, encode_request,
    };

    const TEST_AUTH_KEY: [u8; 32] = [0xA5; 32];

    fn session(slot: u8, connection: u32, capture: u32) -> u64 {
        (0xEC_u64 << 56) | ((slot as u64) << 52) | ((connection as u64) << 32) | capture as u64
    }

    fn packet(session_id: u64, sequence: u32, fill: u8) -> Vec<u8> {
        let mut bytes = vec![0_u8; AUDIO_HEADER_BYTES + AUDIO_FRAME_BYTES + AUDIO_AUTH_TAG_BYTES];
        bytes[..4].copy_from_slice(b"EIAU");
        bytes[4] = 3;
        bytes[5] = AUDIO_HEADER_BYTES as u8;
        bytes[6] = 1;
        bytes[7] = 1;
        bytes[8..16].copy_from_slice(&session_id.to_le_bytes());
        bytes[16..20].copy_from_slice(&sequence.to_le_bytes());
        bytes[20..24].copy_from_slice(&AUDIO_SAMPLE_RATE.to_le_bytes());
        bytes[24..28].copy_from_slice(&(sequence * 20).to_le_bytes());
        bytes[28..30].copy_from_slice(&(AUDIO_FRAME_SAMPLES as u16).to_le_bytes());
        bytes[30..32].copy_from_slice(&(AUDIO_FRAME_BYTES as u16).to_le_bytes());
        bytes[AUDIO_HEADER_BYTES..AUDIO_HEADER_BYTES + AUDIO_FRAME_BYTES].fill(fill);
        let authenticated_bytes = bytes.len() - AUDIO_AUTH_TAG_BYTES;
        let mut mac = Hmac::<Sha256>::new_from_slice(&TEST_AUTH_KEY).unwrap();
        mac.update(&bytes[..authenticated_bytes]);
        let tag = mac.finalize().into_bytes();
        bytes[authenticated_bytes..].copy_from_slice(&tag[..AUDIO_AUTH_TAG_BYTES]);
        bytes
    }

    fn end_packet(session_id: u64, final_sequence: u32) -> Vec<u8> {
        let mut bytes = vec![0_u8; AUDIO_END_BYTES];
        bytes[..4].copy_from_slice(b"EIAE");
        bytes[4] = 3;
        bytes[5] = AUDIO_HEADER_BYTES as u8;
        bytes[6] = 1;
        bytes[8..16].copy_from_slice(&session_id.to_le_bytes());
        bytes[16..20].copy_from_slice(&final_sequence.to_le_bytes());
        bytes[20..24].copy_from_slice(&AUDIO_SAMPLE_RATE.to_le_bytes());
        let authenticated_bytes = bytes.len() - AUDIO_AUTH_TAG_BYTES;
        let mut mac = Hmac::<Sha256>::new_from_slice(&TEST_AUTH_KEY).unwrap();
        mac.update(&bytes[..authenticated_bytes]);
        let tag = mac.finalize().into_bytes();
        bytes[authenticated_bytes..].copy_from_slice(&tag[..AUDIO_AUTH_TAG_BYTES]);
        bytes
    }

    #[test]
    fn session_identity_matches_firmware_layout() {
        let expected = decode_capture_session_identity(session(4, 0xABCDE, 0x12345678)).unwrap();
        assert_eq!(
            expected,
            CaptureSessionIdentity {
                slot: 4,
                connection_generation: 0xABCDE,
                capture_generation: 0x12345678,
            }
        );
        assert!(decode_capture_session_identity(0).is_err());
        assert!(decode_capture_session_identity(session(5, 1, 1)).is_err());
        assert!(decode_capture_session_identity(session(1, 0, 1)).is_err());
        assert!(decode_capture_session_identity(session(1, 1, 0)).is_err());
    }

    #[test]
    fn only_transient_qwen_failures_allow_offline_fallback() {
        assert!(qwen_error_allows_offline_fallback(AsrError::Unavailable));
        assert!(qwen_error_allows_offline_fallback(AsrError::RateLimited));
        assert!(!qwen_error_allows_offline_fallback(AsrError::Rejected));
        assert!(!qwen_error_allows_offline_fallback(AsrError::InvalidAudio));
        assert!(!qwen_error_allows_offline_fallback(AsrError::Protocol));
        assert!(matches!(
            map_qwen_error(AsrError::Rejected),
            LanVoiceError::RemoteAsrRejected
        ));
        assert!(matches!(
            map_qwen_error(AsrError::InvalidAudio),
            LanVoiceError::RemoteAsrInvalidAudio
        ));
        assert!(matches!(
            map_qwen_error(AsrError::Protocol),
            LanVoiceError::RemoteAsrProtocol
        ));
    }

    #[test]
    fn ordered_capture_closes_after_authenticated_end_and_retires_replays() {
        let mut assembler = CaptureAssembler::new(Some(TEST_AUTH_KEY));
        let now = Instant::now();
        let source = "127.0.0.1:40000".parse().unwrap();
        let session_id = session(2, 7, 9);
        for sequence in 0..MIN_CAPTURE_FRAMES {
            assembler
                .ingest(&packet(session_id, sequence, sequence as u8), source, now)
                .unwrap();
        }
        assert!(assembler.take_ready().is_empty());
        assert!(
            assembler
                .expire_incomplete(now + Duration::from_millis(350))
                .is_empty()
        );
        assembler
            .ingest(&end_packet(session_id, MIN_CAPTURE_FRAMES), source, now)
            .unwrap();
        let completed = assembler.take_ready().remove(0).unwrap();
        assert_eq!(completed.identity.slot, 2);
        assert_eq!(
            completed.pcm.len(),
            MIN_CAPTURE_FRAMES as usize * AUDIO_FRAME_BYTES
        );
        assembler
            .ingest(&packet(session_id, 0, 1), source, now)
            .unwrap();
        assert!(assembler.active.is_empty());
    }

    #[test]
    fn repeated_end_before_delayed_tail_is_idempotent() {
        let mut assembler = CaptureAssembler::new(Some(TEST_AUTH_KEY));
        let now = Instant::now();
        let source = "127.0.0.1:40000".parse().unwrap();
        let session_id = session(3, 8, 10);
        for sequence in 0..MIN_CAPTURE_FRAMES - 1 {
            assembler
                .ingest(&packet(session_id, sequence, sequence as u8), source, now)
                .unwrap();
        }
        let terminal = end_packet(session_id, MIN_CAPTURE_FRAMES);
        assembler.ingest(&terminal, source, now).unwrap();
        assembler.ingest(&terminal, source, now).unwrap();
        assert!(assembler.take_ready().is_empty());
        assembler
            .ingest(
                &packet(session_id, MIN_CAPTURE_FRAMES - 1, 0x7F),
                source,
                now,
            )
            .unwrap();
        let completed = assembler.take_ready().remove(0).unwrap();
        assert_eq!(completed.identity.slot, 3);
        assert_eq!(
            completed.pcm.len(),
            MIN_CAPTURE_FRAMES as usize * AUDIO_FRAME_BYTES
        );
    }

    #[test]
    fn gaps_wrong_sources_and_malformed_frames_fail_closed() {
        let mut assembler = CaptureAssembler::new(Some(TEST_AUTH_KEY));
        let now = Instant::now();
        let first = "127.0.0.1:40000".parse().unwrap();
        let second = "127.0.0.1:40001".parse().unwrap();
        let gap = session(1, 1, 1);
        assert!(matches!(
            assembler.ingest(&packet(gap, 1, 0), first, now),
            Err(LanVoiceError::InvalidSequence)
        ));
        let changed_source = session(1, 1, 2);
        assembler
            .ingest(&packet(changed_source, 0, 0), first, now)
            .unwrap();
        assert!(matches!(
            assembler.ingest(&packet(changed_source, 1, 0), second, now),
            Err(LanVoiceError::InvalidSequence)
        ));
        assert!(parse_audio_frame(b"short", Some(&TEST_AUTH_KEY)).is_err());
        let mut wrong_rate = packet(session(1, 1, 3), 0, 0);
        wrong_rate[20..24].copy_from_slice(&48_000_u32.to_le_bytes());
        assert!(parse_audio_frame(&wrong_rate, Some(&TEST_AUTH_KEY)).is_err());
        let mut forged = packet(session(1, 1, 4), 0, 0);
        *forged.last_mut().unwrap() ^= 1;
        assert!(matches!(
            parse_audio_frame(&forged, Some(&TEST_AUTH_KEY)),
            Err(LanVoiceError::Authentication)
        ));
    }

    #[test]
    fn short_capture_is_rejected_without_a_prompt() {
        let mut assembler = CaptureAssembler::new(Some(TEST_AUTH_KEY));
        let now = Instant::now();
        let source = "127.0.0.1:40000".parse().unwrap();
        let session_id = session(3, 2, 8);
        assembler
            .ingest(&packet(session_id, 0, 0), source, now)
            .unwrap();
        assembler
            .ingest(&end_packet(session_id, 1), source, now)
            .unwrap();
        assert!(matches!(
            assembler.take_ready().remove(0),
            Err(LanVoiceError::CaptureTooShort)
        ));
    }

    #[test]
    fn whisper_adapter_uses_local_private_wav_and_returns_bounded_text() {
        let temp = tempdir().unwrap();
        let executable = temp.path().join("whisper-cli");
        fs::write(
            &executable,
            "#!/bin/sh\nset -eu\nfile=''\nwhile [ $# -gt 0 ]; do\n  if [ \"$1\" = '-f' ]; then shift; file=$1; fi\n  shift\ndone\nmode=$(stat -f '%Lp' \"$file\" 2>/dev/null || stat -c '%a' \"$file\")\n[ \"$mode\" = '600' ]\nprintf '请继续完成当前实现。\\n'\n",
        )
        .unwrap();
        fs::set_permissions(&executable, fs::Permissions::from_mode(0o700)).unwrap();
        let model = temp.path().join("model.bin");
        fs::write(&model, b"fixture").unwrap();
        let transcriber = WhisperCppTranscriber::new(WhisperCppConfig {
            executable,
            model,
            work_directory: temp.path().join("work"),
            timeout: Duration::from_secs(2),
        });
        let capture = CompletedCapture {
            session_id: session(1, 2, 3),
            identity: CaptureSessionIdentity {
                slot: 1,
                connection_generation: 2,
                capture_generation: 3,
            },
            pcm: vec![0; MIN_CAPTURE_FRAMES as usize * AUDIO_FRAME_BYTES],
        };
        let prompt = transcriber.transcribe(&capture).unwrap();
        assert_eq!(prompt.slot, 1);
        assert_eq!(prompt.transcript, "请继续完成当前实现。");
        assert_eq!(prompt.request_id, "lan-ec10000200000003");
        assert!(
            fs::read_dir(temp.path().join("work"))
                .unwrap()
                .next()
                .is_none()
        );
    }

    #[test]
    fn udp_ingress_fixture_emits_one_idempotent_slot_prompt() {
        let temp = tempdir().unwrap();
        let executable = temp.path().join("whisper-cli");
        fs::write(&executable, "#!/bin/sh\nset -eu\nprintf '继续测试。\\n'\n").unwrap();
        fs::set_permissions(&executable, fs::Permissions::from_mode(0o700)).unwrap();
        let model = temp.path().join("model.bin");
        fs::write(&model, b"fixture").unwrap();
        let ingress = LanVoiceIngress::start(LanVoiceConfig {
            bind_port: 0,
            whisper: WhisperCppConfig {
                executable,
                model,
                work_directory: temp.path().join("work"),
                timeout: Duration::from_secs(2),
            },
            dashscope_env: temp.path().join("missing.env"),
            auth_key: Some(TEST_AUTH_KEY),
            device_secret_path: temp.path().join("device-secret.hex"),
        })
        .unwrap();
        let sender = UdpSocket::bind("127.0.0.1:0").unwrap();
        let session_id = session(4, 8, 12);
        for sequence in 0..MIN_CAPTURE_FRAMES {
            sender
                .send_to(
                    &packet(session_id, sequence, sequence as u8),
                    ("127.0.0.1", ingress.local_port()),
                )
                .unwrap();
        }
        sender
            .send_to(
                &end_packet(session_id, MIN_CAPTURE_FRAMES),
                ("127.0.0.1", ingress.local_port()),
            )
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(3);
        let prompt = loop {
            if let Some(prompt) = ingress.try_recv() {
                break prompt;
            }
            assert!(Instant::now() < deadline, "LAN voice prompt timed out");
            thread::sleep(Duration::from_millis(20));
        };
        assert_eq!(prompt.slot, 4);
        assert_eq!(prompt.transcript, "继续测试。");
        assert_eq!(prompt.request_id, "lan-ec4000080000000c");
        for sequence in 0..MIN_CAPTURE_FRAMES {
            sender
                .send_to(
                    &packet(session_id, sequence, sequence as u8),
                    ("127.0.0.1", ingress.local_port()),
                )
                .unwrap();
        }
        sender
            .send_to(
                &end_packet(session_id, MIN_CAPTURE_FRAMES),
                ("127.0.0.1", ingress.local_port()),
            )
            .unwrap();
        thread::sleep(Duration::from_millis(500));
        assert!(ingress.try_recv().is_none());
    }

    #[test]
    fn daemon_started_before_first_provision_reloads_device_secret() {
        let temp = tempdir().unwrap();
        let executable = temp.path().join("whisper-cli");
        fs::write(
            &executable,
            "#!/bin/sh\nset -eu\nprintf '热加载成功。\\n'\n",
        )
        .unwrap();
        fs::set_permissions(&executable, fs::Permissions::from_mode(0o700)).unwrap();
        let model = temp.path().join("model.bin");
        fs::write(&model, b"fixture").unwrap();
        let secret_path = temp.path().join("device-secret.hex");
        let ingress = LanVoiceIngress::start(LanVoiceConfig {
            bind_port: 0,
            whisper: WhisperCppConfig {
                executable,
                model,
                work_directory: temp.path().join("work"),
                timeout: Duration::from_secs(2),
            },
            dashscope_env: temp.path().join("missing.env"),
            auth_key: None,
            device_secret_path: secret_path.clone(),
        })
        .unwrap();
        fs::write(&secret_path, format!("{}\n", "a5".repeat(32))).unwrap();
        fs::set_permissions(&secret_path, fs::Permissions::from_mode(0o600)).unwrap();
        thread::sleep(AUTH_RELOAD_INTERVAL + Duration::from_millis(100));

        let sender = UdpSocket::bind("127.0.0.1:0").unwrap();
        let session_id = session(1, 9, 13);
        for sequence in 0..MIN_CAPTURE_FRAMES {
            sender
                .send_to(
                    &packet(session_id, sequence, sequence as u8),
                    ("127.0.0.1", ingress.local_port()),
                )
                .unwrap();
        }
        sender
            .send_to(
                &end_packet(session_id, MIN_CAPTURE_FRAMES),
                ("127.0.0.1", ingress.local_port()),
            )
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(3);
        loop {
            if let Some(prompt) = ingress.try_recv() {
                assert_eq!(prompt.slot, 1);
                assert_eq!(prompt.transcript, "热加载成功。");
                break;
            }
            assert!(
                Instant::now() < deadline,
                "reloaded LAN voice prompt timed out"
            );
            thread::sleep(Duration::from_millis(20));
        }
    }

    #[test]
    fn duplicate_finished_packet_replays_ack_after_host_commit() {
        let host = UdpSocket::bind("127.0.0.1:0").unwrap();
        let device = UdpSocket::bind("127.0.0.1:0").unwrap();
        device
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        let device_address = device.local_addr().unwrap();
        let identity = PlaybackIdentity {
            slot: 2,
            request_generation: 3,
            connection_generation: 4,
            summary_generation: 5,
            lease: 6,
        };
        let begin = PlaybackBegin {
            identity,
            total_bytes: 4,
            total_samples: 320,
            chunk_bytes: PLAYBACK_CHUNK_BYTES as u16,
            request_nonce: 11,
        };
        let (events, _event_receiver) = mpsc::channel();
        let mut playback = ActiveLanPlayback::default();
        playback.handle_command(
            LanPlaybackCommand::Start(LanPlaybackStart {
                begin,
                source: device_address,
                eiad: zeroize::Zeroizing::new(vec![1, 2, 3, 4]),
            }),
            &host,
            Some(&TEST_AUTH_KEY),
            &events,
        );
        let mut packet = [0_u8; 128];
        let _ = device.recv_from(&mut packet).unwrap();
        playback.transfer.as_mut().unwrap().phase = PlaybackSendPhase::HostCommit;

        playback.handle_command(
            LanPlaybackCommand::FinishAck(identity),
            &host,
            Some(&TEST_AUTH_KEY),
            &events,
        );
        let (length, _) = device.recv_from(&mut packet).unwrap();
        assert_eq!(
            decode_finished_ack(&packet[..length], &TEST_AUTH_KEY).unwrap(),
            (identity, 0)
        );
        assert!(playback.transfer.is_none());

        let finished = encode_finished(
            PlaybackFinished {
                identity,
                played_samples: 320,
            },
            &TEST_AUTH_KEY,
        );
        playback.ingest(&finished, device_address, &TEST_AUTH_KEY, &host, &events);
        let (length, _) = device.recv_from(&mut packet).unwrap();
        assert_eq!(
            decode_finished_ack(&packet[..length], &TEST_AUTH_KEY).unwrap(),
            (identity, 0)
        );
    }

    #[test]
    fn request_replay_is_rejected_and_new_generation_cancels_before_replacement() {
        let host = UdpSocket::bind("127.0.0.1:0").unwrap();
        let device = UdpSocket::bind("127.0.0.1:0").unwrap();
        let device_address = device.local_addr().unwrap();
        let (events, event_receiver) = mpsc::channel();
        let mut playback = ActiveLanPlayback::default();
        let request = PlaybackRequest {
            slot: 2,
            request_generation: 5,
            connection_generation: 9,
            nonce: 11,
        };
        let packet = encode_request(request, &TEST_AUTH_KEY);
        playback.ingest(&packet, device_address, &TEST_AUTH_KEY, &host, &events);
        assert_eq!(
            event_receiver.try_recv().unwrap(),
            LanPlaybackEvent::Request(LanPlaybackRequest {
                request,
                source: device_address,
            })
        );
        playback.ingest(&packet, device_address, &TEST_AUTH_KEY, &host, &events);
        assert!(event_receiver.try_recv().is_err());

        let identity = PlaybackIdentity {
            slot: 2,
            request_generation: 5,
            connection_generation: 9,
            summary_generation: 7,
            lease: 13,
        };
        let begin = PlaybackBegin {
            identity,
            total_bytes: 4,
            total_samples: 320,
            chunk_bytes: PLAYBACK_CHUNK_BYTES as u16,
            request_nonce: request.nonce,
        };
        playback.handle_command(
            LanPlaybackCommand::Start(LanPlaybackStart {
                begin,
                source: device_address,
                eiad: zeroize::Zeroizing::new(vec![1, 2, 3, 4]),
            }),
            &host,
            Some(&TEST_AUTH_KEY),
            &events,
        );
        let replacement = PlaybackRequest {
            request_generation: 6,
            nonce: 12,
            ..request
        };
        playback.ingest(
            &encode_request(replacement, &TEST_AUTH_KEY),
            device_address,
            &TEST_AUTH_KEY,
            &host,
            &events,
        );
        assert_eq!(
            event_receiver.try_recv().unwrap(),
            LanPlaybackEvent::Cancelled(identity)
        );
        assert_eq!(
            event_receiver.try_recv().unwrap(),
            LanPlaybackEvent::Request(LanPlaybackRequest {
                request: replacement,
                source: device_address,
            })
        );
        assert!(playback.transfer.is_none());

        let reconnected = PlaybackRequest {
            request_generation: 1,
            connection_generation: 10,
            nonce: 13,
            ..request
        };
        playback.ingest(
            &encode_request(reconnected, &TEST_AUTH_KEY),
            device_address,
            &TEST_AUTH_KEY,
            &host,
            &events,
        );
        assert_eq!(
            event_receiver.try_recv().unwrap(),
            LanPlaybackEvent::Request(LanPlaybackRequest {
                request: reconnected,
                source: device_address,
            })
        );
    }

    #[test]
    fn authenticated_ptt_cancel_terminates_device_finished_transport_immediately() {
        let host = UdpSocket::bind("127.0.0.1:0").unwrap();
        let device = UdpSocket::bind("127.0.0.1:0").unwrap();
        device
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        let device_address = device.local_addr().unwrap();
        let (events, event_receiver) = mpsc::channel();
        let identity = PlaybackIdentity {
            slot: 2,
            request_generation: 3,
            connection_generation: 4,
            summary_generation: 5,
            lease: 6,
        };
        let mut playback = ActiveLanPlayback::default();
        playback.handle_command(
            LanPlaybackCommand::Start(LanPlaybackStart {
                begin: PlaybackBegin {
                    identity,
                    total_bytes: 4,
                    total_samples: 320,
                    chunk_bytes: PLAYBACK_CHUNK_BYTES as u16,
                    request_nonce: 7,
                },
                source: device_address,
                eiad: zeroize::Zeroizing::new(vec![1, 2, 3, 4]),
            }),
            &host,
            Some(&TEST_AUTH_KEY),
            &events,
        );
        let mut packet = [0_u8; 128];
        let _ = device.recv_from(&mut packet).unwrap();
        playback.transfer.as_mut().unwrap().phase = PlaybackSendPhase::DeviceFinished;
        let cancel = encode_ack(
            PlaybackAck {
                identity,
                status: 3,
                next_offset: 4,
            },
            &TEST_AUTH_KEY,
        );
        playback.ingest(&cancel, device_address, &TEST_AUTH_KEY, &host, &events);
        let (cancel_ack_length, _) = device.recv_from(&mut packet).unwrap();
        assert_eq!(
            decode_finished_ack(&packet[..cancel_ack_length], &TEST_AUTH_KEY).unwrap(),
            (identity, 1)
        );
        assert!(playback.transfer.is_none());
        assert_eq!(
            event_receiver.try_recv().unwrap(),
            LanPlaybackEvent::Cancelled(identity)
        );
        playback.ingest(&cancel, device_address, &TEST_AUTH_KEY, &host, &events);
        let (retry_ack_length, _) = device.recv_from(&mut packet).unwrap();
        assert_eq!(
            decode_finished_ack(&packet[..retry_ack_length], &TEST_AUTH_KEY).unwrap(),
            (identity, 1)
        );
        assert!(event_receiver.try_recv().is_err());
    }

    #[test]
    fn playback_transfer_recovers_lost_final_data_ack_before_finish_commit() {
        let host = UdpSocket::bind("127.0.0.1:0").unwrap();
        let device = UdpSocket::bind("127.0.0.1:0").unwrap();
        device
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        let device_address = device.local_addr().unwrap();
        let identity = PlaybackIdentity {
            slot: 3,
            request_generation: 7,
            connection_generation: 8,
            summary_generation: 9,
            lease: 10,
        };
        let eiad = (0..1500).map(|index| index as u8).collect::<Vec<_>>();
        let begin = PlaybackBegin {
            identity,
            total_bytes: eiad.len() as u32,
            total_samples: 2880,
            chunk_bytes: PLAYBACK_CHUNK_BYTES as u16,
            request_nonce: 12,
        };
        let (events, event_receiver) = mpsc::channel();
        let mut playback = ActiveLanPlayback::default();
        playback.handle_command(
            LanPlaybackCommand::Start(LanPlaybackStart {
                begin,
                source: device_address,
                eiad: zeroize::Zeroizing::new(eiad.clone()),
            }),
            &host,
            Some(&TEST_AUTH_KEY),
            &events,
        );

        let mut packet = [0_u8; 1200];
        let (length, _) = device.recv_from(&mut packet).unwrap();
        assert_eq!(
            decode_begin(&packet[..length], &TEST_AUTH_KEY).unwrap(),
            begin
        );
        let begin_ack = encode_ack(
            PlaybackAck {
                identity,
                status: 0,
                next_offset: 0,
            },
            &TEST_AUTH_KEY,
        );
        playback.transfer.as_mut().unwrap().retry_count = PLAYBACK_MAX_RETRIES - 1;
        playback.ingest(&begin_ack, device_address, &TEST_AUTH_KEY, &host, &events);
        assert_eq!(playback.transfer.as_ref().unwrap().retry_count, 0);
        let (first_length, _) = device.recv_from(&mut packet).unwrap();
        let (first_identity, first_offset, first_payload) =
            decode_data(&packet[..first_length], begin.request_nonce, &TEST_AUTH_KEY).unwrap();
        assert_eq!(first_identity, identity);
        assert_eq!(first_offset, 0);
        assert_eq!(first_payload, &eiad[..PLAYBACK_CHUNK_BYTES]);
        let first_payload = first_payload.to_vec();
        let (second_length, _) = device.recv_from(&mut packet).unwrap();
        let (_, second_offset, second_payload) = decode_data(
            &packet[..second_length],
            begin.request_nonce,
            &TEST_AUTH_KEY,
        )
        .unwrap();
        assert_eq!(second_offset, PLAYBACK_CHUNK_BYTES as u32);
        assert_eq!(second_payload, &eiad[PLAYBACK_CHUNK_BYTES..]);
        let second_payload = second_payload.to_vec();

        playback.transfer.as_mut().unwrap().last_send = Instant::now() - PLAYBACK_RETRY;
        playback.tick(&host, Some(&TEST_AUTH_KEY), &events);
        let (retry_length, _) = device.recv_from(&mut packet).unwrap();
        let (_, retry_offset, retry_payload) =
            decode_data(&packet[..retry_length], begin.request_nonce, &TEST_AUTH_KEY).unwrap();
        assert_eq!(retry_offset, first_offset);
        assert_eq!(retry_payload, first_payload.as_slice());
        let (second_retry_length, _) = device.recv_from(&mut packet).unwrap();
        let (_, second_retry_offset, second_retry_payload) = decode_data(
            &packet[..second_retry_length],
            begin.request_nonce,
            &TEST_AUTH_KEY,
        )
        .unwrap();
        assert_eq!(second_retry_offset, second_offset);
        assert_eq!(second_retry_payload, second_payload.as_slice());

        let first_ack = encode_ack(
            PlaybackAck {
                identity,
                status: 0,
                next_offset: PLAYBACK_CHUNK_BYTES as u32,
            },
            &TEST_AUTH_KEY,
        );
        playback.ingest(&first_ack, device_address, &TEST_AUTH_KEY, &host, &events);

        // The device accepted the final chunk and started playing, but its ACK
        // was lost. The Host must retransmit that exact chunk and accept the
        // device's replayed cumulative ACK without cancelling the lease.
        playback.transfer.as_mut().unwrap().last_send = Instant::now() - PLAYBACK_RETRY;
        playback.tick(&host, Some(&TEST_AUTH_KEY), &events);
        let (final_retry_length, _) = device.recv_from(&mut packet).unwrap();
        let (retry_identity, retry_offset, retry_payload) = decode_data(
            &packet[..final_retry_length],
            begin.request_nonce,
            &TEST_AUTH_KEY,
        )
        .unwrap();
        assert_eq!(retry_identity, identity);
        assert_eq!(retry_offset, second_offset);
        assert_eq!(retry_payload, second_payload.as_slice());

        let complete_ack = encode_ack(
            PlaybackAck {
                identity,
                status: 0,
                next_offset: eiad.len() as u32,
            },
            &TEST_AUTH_KEY,
        );
        playback.ingest(
            &complete_ack,
            device_address,
            &TEST_AUTH_KEY,
            &host,
            &events,
        );
        assert_eq!(
            playback.transfer.as_ref().unwrap().phase,
            PlaybackSendPhase::DeviceFinished
        );
        let finished = PlaybackFinished {
            identity,
            played_samples: begin.total_samples,
        };
        playback.ingest(
            &encode_finished(finished, &TEST_AUTH_KEY),
            device_address,
            &TEST_AUTH_KEY,
            &host,
            &events,
        );
        assert_eq!(
            event_receiver.recv_timeout(Duration::from_secs(1)).unwrap(),
            LanPlaybackEvent::Finished(finished)
        );
        assert_eq!(
            playback.transfer.as_ref().unwrap().phase,
            PlaybackSendPhase::HostCommit
        );

        // A first cache cleanup attempt may fail after the heard transaction.
        // The device's authenticated EIPF retry must re-drive the idempotent
        // Host finalizer so cleanup and EIPK can still converge.
        playback.ingest(
            &encode_finished(finished, &TEST_AUTH_KEY),
            device_address,
            &TEST_AUTH_KEY,
            &host,
            &events,
        );
        assert_eq!(
            event_receiver.recv_timeout(Duration::from_secs(1)).unwrap(),
            LanPlaybackEvent::Finished(finished)
        );

        playback.handle_command(
            LanPlaybackCommand::FinishAck(identity),
            &host,
            Some(&TEST_AUTH_KEY),
            &events,
        );
        let (finish_ack_length, _) = device.recv_from(&mut packet).unwrap();
        assert_eq!(
            decode_finished_ack(&packet[..finish_ack_length], &TEST_AUTH_KEY).unwrap(),
            (identity, 0)
        );
        assert!(playback.transfer.is_none());
    }

    #[test]
    fn playback_six_chunk_window_bounds_duplicate_gap_ack_retries() {
        let host = UdpSocket::bind("127.0.0.1:0").unwrap();
        let device = UdpSocket::bind("127.0.0.1:0").unwrap();
        device
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        let device_address = device.local_addr().unwrap();
        let identity = PlaybackIdentity {
            slot: 2,
            request_generation: 11,
            connection_generation: 12,
            summary_generation: 13,
            lease: 14,
        };
        let eiad = vec![0x5A; PLAYBACK_CHUNK_BYTES * (PLAYBACK_SEND_WINDOW_CHUNKS + 1)];
        let begin = PlaybackBegin {
            identity,
            total_bytes: eiad.len() as u32,
            total_samples: 4800,
            chunk_bytes: PLAYBACK_CHUNK_BYTES as u16,
            request_nonce: 15,
        };
        let (events, event_receiver) = mpsc::channel();
        let mut playback = ActiveLanPlayback::default();
        playback.handle_command(
            LanPlaybackCommand::Start(LanPlaybackStart {
                begin,
                source: device_address,
                eiad: zeroize::Zeroizing::new(eiad),
            }),
            &host,
            Some(&TEST_AUTH_KEY),
            &events,
        );

        let mut packet = [0_u8; 1200];
        let _ = device.recv_from(&mut packet).unwrap();
        playback.ingest(
            &encode_ack(
                PlaybackAck {
                    identity,
                    status: 0,
                    next_offset: 0,
                },
                &TEST_AUTH_KEY,
            ),
            device_address,
            &TEST_AUTH_KEY,
            &host,
            &events,
        );
        for expected_offset in
            (0..PLAYBACK_SEND_WINDOW_CHUNKS).map(|index| index * PLAYBACK_CHUNK_BYTES)
        {
            let (length, _) = device.recv_from(&mut packet).unwrap();
            let (_, offset, _) =
                decode_data(&packet[..length], begin.request_nonce, &TEST_AUTH_KEY).unwrap();
            assert_eq!(offset as usize, expected_offset);
        }

        playback.transfer.as_mut().unwrap().retry_count = PLAYBACK_MAX_RETRIES - 1;
        let duplicate_gap = encode_ack(
            PlaybackAck {
                identity,
                status: 1,
                next_offset: 0,
            },
            &TEST_AUTH_KEY,
        );
        playback.ingest(
            &duplicate_gap,
            device_address,
            &TEST_AUTH_KEY,
            &host,
            &events,
        );
        assert_eq!(
            playback.transfer.as_ref().unwrap().retry_count,
            PLAYBACK_MAX_RETRIES
        );
        playback.ingest(
            &duplicate_gap,
            device_address,
            &TEST_AUTH_KEY,
            &host,
            &events,
        );
        assert!(playback.transfer.is_none());
        assert_eq!(
            event_receiver.try_recv().unwrap(),
            LanPlaybackEvent::Cancelled(identity)
        );
    }

    #[test]
    fn only_authenticated_mailbox_heartbeat_echoes_the_current_challenge() {
        let host = UdpSocket::bind("127.0.0.1:0").unwrap();
        let device = UdpSocket::bind("127.0.0.1:0").unwrap();
        device
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        let playback = ActiveLanPlayback {
            mailbox_status: MailboxStatus {
                unread_slots: 0b1010,
                running_tasks: 2,
                coverage_by_slot: [0, 12, 0, 3],
            },
            ..Default::default()
        };
        let mut unsigned = [0_u8; 20];
        unsigned[..4].copy_from_slice(b"EIHB");
        unsigned[4] = 1;
        unsigned[5] = 0x02;
        unsigned[16..20].copy_from_slice(&0xA1B2_C3D4_u32.to_le_bytes());
        playback.handle_heartbeat(
            &unsigned,
            device.local_addr().unwrap(),
            &TEST_AUTH_KEY,
            &host,
        );
        let mut response = [0_u8; MAILBOX_STATUS_BYTES];
        device.set_nonblocking(true).unwrap();
        assert!(device.recv_from(&mut response).is_err());
        device.set_nonblocking(false).unwrap();

        let mut heartbeat = [0_u8; AUTHENTICATED_HEARTBEAT_BYTES];
        heartbeat[..4].copy_from_slice(b"EIHB");
        heartbeat[4] = 1;
        heartbeat[5] = 0x02;
        heartbeat[16..20].copy_from_slice(&0xA1B2_C3D4_u32.to_le_bytes());
        heartbeat[20..24].copy_from_slice(b"EISD");
        heartbeat[24] = 1;
        heartbeat[25] = 60;
        heartbeat[26..28].copy_from_slice(&0x0002_u16.to_le_bytes());
        let mut mac = Hmac::<Sha256>::new_from_slice(&TEST_AUTH_KEY).unwrap();
        mac.update(HEARTBEAT_AUTH_CONTEXT);
        mac.update(&heartbeat[..64]);
        heartbeat[64..80].copy_from_slice(&mac.finalize().into_bytes()[..16]);

        playback.handle_heartbeat(
            &heartbeat,
            device.local_addr().unwrap(),
            &TEST_AUTH_KEY,
            &host,
        );

        let (length, _) = device.recv_from(&mut response).unwrap();
        assert_eq!(length, MAILBOX_STATUS_BYTES);
        assert_eq!(
            crate::lan_playback::decode_mailbox_status(&response, &TEST_AUTH_KEY).unwrap(),
            (playback.mailbox_status, 0xA1B2_C3D4)
        );

        heartbeat[64] ^= 1;
        playback.handle_heartbeat(
            &heartbeat,
            device.local_addr().unwrap(),
            &TEST_AUTH_KEY,
            &host,
        );
        device.set_nonblocking(true).unwrap();
        assert!(device.recv_from(&mut response).is_err());
    }
}

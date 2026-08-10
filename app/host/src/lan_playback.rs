use aes_gcm::aead::{AeadInPlace, KeyInit};
use aes_gcm::{Aes256Gcm, Nonce, Tag};
use hmac::{Hmac, Mac};
use sha2::Sha256;
use thiserror::Error;

pub const PLAYBACK_VERSION: u8 = 1;
pub const PLAYBACK_TAG_BYTES: usize = 16;
pub const PLAYBACK_CHUNK_BYTES: usize = 1024;
pub const PLAYBACK_MAX_EIAD_BYTES: usize = 4 * 1024 * 1024;

const REQUEST_BYTES: usize = 40;
const BEGIN_BYTES: usize = 72;
const ACK_BYTES: usize = 52;
const FINISHED_BYTES: usize = 56;
const FINISHED_ACK_BYTES: usize = 48;
const DATA_HEADER_BYTES: usize = 40;
pub const MAILBOX_STATUS_BYTES: usize = 32;
pub const MAILBOX_STATUS_VERSION: u8 = 3;

#[derive(Debug, Error, Clone, Copy, PartialEq, Eq)]
pub enum PlaybackWireError {
    #[error("playback packet is malformed")]
    Malformed,
    #[error("playback packet authentication failed")]
    Authentication,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PlaybackRequest {
    pub slot: u8,
    pub request_generation: u32,
    pub connection_generation: u32,
    pub nonce: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PlaybackIdentity {
    pub slot: u8,
    pub request_generation: u32,
    pub connection_generation: u32,
    pub summary_generation: u64,
    pub lease: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PlaybackBegin {
    pub identity: PlaybackIdentity,
    pub total_bytes: u32,
    pub total_samples: u64,
    pub chunk_bytes: u16,
    pub request_nonce: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PlaybackAck {
    pub identity: PlaybackIdentity,
    pub status: u8,
    pub next_offset: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PlaybackFinished {
    pub identity: PlaybackIdentity,
    pub played_samples: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct MailboxStatus {
    pub unread_slots: u8,
    pub running_tasks: u8,
    pub coverage_by_slot: [u8; 4],
}

pub fn encode_request(request: PlaybackRequest, key: &[u8; 32]) -> Vec<u8> {
    let mut packet = vec![0_u8; REQUEST_BYTES];
    packet[..4].copy_from_slice(b"EIPR");
    packet[4] = PLAYBACK_VERSION;
    packet[5] = request.slot;
    put_u32(&mut packet, 8, request.request_generation);
    put_u32(&mut packet, 12, request.connection_generation);
    put_u64(&mut packet, 16, request.nonce);
    sign(&mut packet, key);
    packet
}

pub fn decode_request(packet: &[u8], key: &[u8; 32]) -> Result<PlaybackRequest, PlaybackWireError> {
    verify_fixed(packet, REQUEST_BYTES, b"EIPR", key)?;
    let request = PlaybackRequest {
        slot: packet[5],
        request_generation: get_u32(packet, 8),
        connection_generation: get_u32(packet, 12),
        nonce: get_u64(packet, 16),
    };
    if packet[6..8] != [0, 0]
        || !(1..=4).contains(&request.slot)
        || request.request_generation == 0
        || request.connection_generation == 0
        || request.nonce == 0
    {
        return Err(PlaybackWireError::Malformed);
    }
    Ok(request)
}

pub fn encode_begin(begin: PlaybackBegin, key: &[u8; 32]) -> Vec<u8> {
    let mut packet = vec![0_u8; BEGIN_BYTES];
    packet[..4].copy_from_slice(b"EIPB");
    packet[4] = PLAYBACK_VERSION;
    packet[5] = begin.identity.slot;
    encode_identity(&mut packet, begin.identity);
    put_u32(&mut packet, 32, begin.total_bytes);
    put_u64(&mut packet, 36, begin.total_samples);
    put_u16(&mut packet, 44, begin.chunk_bytes);
    put_u64(&mut packet, 48, begin.request_nonce);
    sign(&mut packet, key);
    packet
}

pub fn decode_begin(packet: &[u8], key: &[u8; 32]) -> Result<PlaybackBegin, PlaybackWireError> {
    verify_fixed(packet, BEGIN_BYTES, b"EIPB", key)?;
    let begin = PlaybackBegin {
        identity: decode_identity(packet),
        total_bytes: get_u32(packet, 32),
        total_samples: get_u64(packet, 36),
        chunk_bytes: get_u16(packet, 44),
        request_nonce: get_u64(packet, 48),
    };
    if packet[6..8] != [0, 0]
        || packet[46..48] != [0, 0]
        || !valid_identity(begin.identity)
        || begin.total_bytes == 0
        || begin.total_bytes as usize > PLAYBACK_MAX_EIAD_BYTES
        || begin.total_samples == 0
        || begin.chunk_bytes == 0
        || begin.chunk_bytes as usize > PLAYBACK_CHUNK_BYTES
        || begin.request_nonce == 0
    {
        return Err(PlaybackWireError::Malformed);
    }
    Ok(begin)
}

pub fn encode_data(
    identity: PlaybackIdentity,
    request_nonce: u64,
    offset: u32,
    payload: &[u8],
    key: &[u8; 32],
) -> Result<Vec<u8>, PlaybackWireError> {
    if !valid_identity(identity)
        || request_nonce == 0
        || payload.is_empty()
        || payload.len() > PLAYBACK_CHUNK_BYTES
    {
        return Err(PlaybackWireError::Malformed);
    }
    let mut packet = vec![0_u8; DATA_HEADER_BYTES + payload.len() + PLAYBACK_TAG_BYTES];
    packet[..4].copy_from_slice(b"EIPD");
    packet[4] = PLAYBACK_VERSION;
    packet[5] = identity.slot;
    encode_identity(&mut packet, identity);
    put_u32(&mut packet, 32, offset);
    put_u16(&mut packet, 36, payload.len() as u16);
    packet[40..40 + payload.len()].copy_from_slice(payload);
    let data_key = derive_data_key(identity, request_nonce, key);
    let cipher = Aes256Gcm::new_from_slice(&data_key).map_err(|_| PlaybackWireError::Malformed)?;
    let nonce = data_nonce(offset);
    let (header, body_and_tag) = packet.split_at_mut(DATA_HEADER_BYTES);
    let (body, tag_bytes) = body_and_tag.split_at_mut(payload.len());
    let tag = cipher
        .encrypt_in_place_detached(Nonce::from_slice(&nonce), header, body)
        .map_err(|_| PlaybackWireError::Authentication)?;
    tag_bytes.copy_from_slice(tag.as_slice());
    Ok(packet)
}

pub fn decode_data(
    packet: &[u8],
    request_nonce: u64,
    key: &[u8; 32],
) -> Result<(PlaybackIdentity, u32, Vec<u8>), PlaybackWireError> {
    if packet.len() < DATA_HEADER_BYTES + 1 + PLAYBACK_TAG_BYTES
        || packet[..4] != *b"EIPD"
        || packet[4] != PLAYBACK_VERSION
    {
        return Err(PlaybackWireError::Malformed);
    }
    let identity = decode_identity(packet);
    let payload_len = get_u16(packet, 36) as usize;
    if packet[6..8] != [0, 0]
        || packet[38..40] != [0, 0]
        || !valid_identity(identity)
        || request_nonce == 0
        || payload_len == 0
        || payload_len > PLAYBACK_CHUNK_BYTES
        || packet.len() != DATA_HEADER_BYTES + payload_len + PLAYBACK_TAG_BYTES
    {
        return Err(PlaybackWireError::Malformed);
    }
    let offset = get_u32(packet, 32);
    let data_key = derive_data_key(identity, request_nonce, key);
    let cipher = Aes256Gcm::new_from_slice(&data_key).map_err(|_| PlaybackWireError::Malformed)?;
    let mut plaintext = packet[DATA_HEADER_BYTES..DATA_HEADER_BYTES + payload_len].to_vec();
    let tag = Tag::from_slice(&packet[DATA_HEADER_BYTES + payload_len..]);
    cipher
        .decrypt_in_place_detached(
            Nonce::from_slice(&data_nonce(offset)),
            &packet[..DATA_HEADER_BYTES],
            &mut plaintext,
            tag,
        )
        .map_err(|_| PlaybackWireError::Authentication)?;
    Ok((identity, offset, plaintext))
}

pub fn encode_ack(ack: PlaybackAck, key: &[u8; 32]) -> Vec<u8> {
    let mut packet = vec![0_u8; ACK_BYTES];
    packet[..4].copy_from_slice(b"EIPA");
    packet[4] = PLAYBACK_VERSION;
    packet[5] = ack.identity.slot;
    packet[6] = ack.status;
    encode_identity(&mut packet, ack.identity);
    put_u32(&mut packet, 32, ack.next_offset);
    sign(&mut packet, key);
    packet
}

pub fn decode_ack(packet: &[u8], key: &[u8; 32]) -> Result<PlaybackAck, PlaybackWireError> {
    verify_fixed(packet, ACK_BYTES, b"EIPA", key)?;
    let ack = PlaybackAck {
        identity: decode_identity(packet),
        status: packet[6],
        next_offset: get_u32(packet, 32),
    };
    if packet[7] != 0 || !valid_identity(ack.identity) || ack.status > 3 {
        return Err(PlaybackWireError::Malformed);
    }
    Ok(ack)
}

pub fn encode_finished(finished: PlaybackFinished, key: &[u8; 32]) -> Vec<u8> {
    let mut packet = vec![0_u8; FINISHED_BYTES];
    packet[..4].copy_from_slice(b"EIPF");
    packet[4] = PLAYBACK_VERSION;
    packet[5] = finished.identity.slot;
    encode_identity(&mut packet, finished.identity);
    put_u64(&mut packet, 32, finished.played_samples);
    sign(&mut packet, key);
    packet
}

pub fn decode_finished(
    packet: &[u8],
    key: &[u8; 32],
) -> Result<PlaybackFinished, PlaybackWireError> {
    verify_fixed(packet, FINISHED_BYTES, b"EIPF", key)?;
    let finished = PlaybackFinished {
        identity: decode_identity(packet),
        played_samples: get_u64(packet, 32),
    };
    if packet[6..8] != [0, 0] || !valid_identity(finished.identity) || finished.played_samples == 0
    {
        return Err(PlaybackWireError::Malformed);
    }
    Ok(finished)
}

pub fn encode_finished_ack(identity: PlaybackIdentity, status: u8, key: &[u8; 32]) -> Vec<u8> {
    let mut packet = vec![0_u8; FINISHED_ACK_BYTES];
    packet[..4].copy_from_slice(b"EIPK");
    packet[4] = PLAYBACK_VERSION;
    packet[5] = identity.slot;
    packet[6] = status;
    encode_identity(&mut packet, identity);
    sign(&mut packet, key);
    packet
}

pub fn decode_finished_ack(
    packet: &[u8],
    key: &[u8; 32],
) -> Result<(PlaybackIdentity, u8), PlaybackWireError> {
    verify_fixed(packet, FINISHED_ACK_BYTES, b"EIPK", key)?;
    let identity = decode_identity(packet);
    if packet[7] != 0 || !valid_identity(identity) || packet[6] > 1 {
        return Err(PlaybackWireError::Malformed);
    }
    Ok((identity, packet[6]))
}

pub fn encode_mailbox_status(
    status: MailboxStatus,
    heartbeat_sequence: u32,
    key: &[u8; 32],
) -> Result<Vec<u8>, PlaybackWireError> {
    if !valid_mailbox_status(status) {
        return Err(PlaybackWireError::Malformed);
    }
    let mut packet = vec![0_u8; MAILBOX_STATUS_BYTES];
    packet[..4].copy_from_slice(b"EIMB");
    packet[4] = MAILBOX_STATUS_VERSION;
    packet[5] = status.unread_slots;
    packet[6] = status.running_tasks;
    put_u32(&mut packet, 8, heartbeat_sequence);
    packet[12..16].copy_from_slice(&status.coverage_by_slot);
    sign(&mut packet, key);
    Ok(packet)
}

pub fn decode_mailbox_status(
    packet: &[u8],
    key: &[u8; 32],
) -> Result<(MailboxStatus, u32), PlaybackWireError> {
    if packet.len() != MAILBOX_STATUS_BYTES
        || packet[..4] != *b"EIMB"
        || packet[4] != MAILBOX_STATUS_VERSION
    {
        return Err(PlaybackWireError::Malformed);
    }
    verify(packet, key)?;
    let status = MailboxStatus {
        unread_slots: packet[5],
        running_tasks: packet[6],
        coverage_by_slot: packet[12..16].try_into().unwrap(),
    };
    if packet[7] != 0 || !valid_mailbox_status(status) {
        return Err(PlaybackWireError::Malformed);
    }
    Ok((status, get_u32(packet, 8)))
}

fn valid_mailbox_status(status: MailboxStatus) -> bool {
    status.unread_slots & !0x0F == 0
        && status.running_tasks <= 4
        && status
            .coverage_by_slot
            .iter()
            .enumerate()
            .all(|(index, coverage)| {
                let unread = status.unread_slots & (1_u8 << index) != 0;
                unread == (*coverage != 0)
            })
}

fn encode_identity(packet: &mut [u8], identity: PlaybackIdentity) {
    put_u32(packet, 8, identity.request_generation);
    put_u32(packet, 12, identity.connection_generation);
    put_u64(packet, 16, identity.summary_generation);
    put_u64(packet, 24, identity.lease);
}

fn decode_identity(packet: &[u8]) -> PlaybackIdentity {
    PlaybackIdentity {
        slot: packet[5],
        request_generation: get_u32(packet, 8),
        connection_generation: get_u32(packet, 12),
        summary_generation: get_u64(packet, 16),
        lease: get_u64(packet, 24),
    }
}

fn valid_identity(identity: PlaybackIdentity) -> bool {
    (1..=4).contains(&identity.slot)
        && identity.request_generation != 0
        && identity.connection_generation != 0
        && identity.summary_generation != 0
        && identity.lease != 0
}

fn derive_data_key(
    identity: PlaybackIdentity,
    request_nonce: u64,
    root_key: &[u8; 32],
) -> [u8; 32] {
    let mut context = [0_u8; 44];
    context[..8].copy_from_slice(b"EIPDKEY1");
    context[8] = identity.slot;
    put_u32(&mut context, 12, identity.request_generation);
    put_u32(&mut context, 16, identity.connection_generation);
    put_u64(&mut context, 20, identity.summary_generation);
    put_u64(&mut context, 28, identity.lease);
    put_u64(&mut context, 36, request_nonce);
    let mut mac =
        <Hmac<Sha256> as Mac>::new_from_slice(root_key).expect("HMAC accepts a 32-byte key");
    mac.update(&context);
    mac.finalize().into_bytes().into()
}

fn data_nonce(offset: u32) -> [u8; 12] {
    let mut nonce = [0_u8; 12];
    put_u32(&mut nonce, 0, offset);
    nonce
}

fn verify_fixed(
    packet: &[u8],
    length: usize,
    magic: &[u8; 4],
    key: &[u8; 32],
) -> Result<(), PlaybackWireError> {
    if packet.len() != length || packet[..4] != *magic || packet[4] != PLAYBACK_VERSION {
        return Err(PlaybackWireError::Malformed);
    }
    verify(packet, key)
}

fn sign(packet: &mut [u8], key: &[u8; 32]) {
    let split = packet.len() - PLAYBACK_TAG_BYTES;
    let mut mac = <Hmac<Sha256> as Mac>::new_from_slice(key).expect("HMAC accepts a 32-byte key");
    mac.update(&packet[..split]);
    packet[split..].copy_from_slice(&mac.finalize().into_bytes()[..PLAYBACK_TAG_BYTES]);
}

fn verify(packet: &[u8], key: &[u8; 32]) -> Result<(), PlaybackWireError> {
    if packet.len() < PLAYBACK_TAG_BYTES {
        return Err(PlaybackWireError::Malformed);
    }
    let split = packet.len() - PLAYBACK_TAG_BYTES;
    let mut mac = <Hmac<Sha256> as Mac>::new_from_slice(key).expect("HMAC accepts a 32-byte key");
    mac.update(&packet[..split]);
    mac.verify_truncated_left(&packet[split..])
        .map_err(|_| PlaybackWireError::Authentication)
}

fn put_u16(packet: &mut [u8], offset: usize, value: u16) {
    packet[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u32(packet: &mut [u8], offset: usize, value: u32) {
    packet[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(packet: &mut [u8], offset: usize, value: u64) {
    packet[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn get_u16(packet: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(packet[offset..offset + 2].try_into().unwrap())
}

fn get_u32(packet: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(packet[offset..offset + 4].try_into().unwrap())
}

fn get_u64(packet: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(packet[offset..offset + 8].try_into().unwrap())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn identity() -> PlaybackIdentity {
        PlaybackIdentity {
            slot: 2,
            request_generation: 0x1122_3344,
            connection_generation: 0x5566_7788,
            summary_generation: 0x0102_0304_0506_0708,
            lease: 0x1112_1314_1516_1718,
        }
    }

    #[test]
    fn packets_round_trip_and_fail_closed() {
        let key = [0x11; 32];
        let request = PlaybackRequest {
            slot: 2,
            request_generation: 7,
            connection_generation: 9,
            nonce: 11,
        };
        assert_eq!(
            decode_request(&encode_request(request, &key), &key).unwrap(),
            request
        );

        let begin = PlaybackBegin {
            identity: identity(),
            total_bytes: 2049,
            total_samples: 96_001,
            chunk_bytes: PLAYBACK_CHUNK_BYTES as u16,
            request_nonce: 0x2122_2324_2526_2728,
        };
        assert_eq!(
            decode_begin(&encode_begin(begin, &key), &key).unwrap(),
            begin
        );

        let data = encode_data(identity(), begin.request_nonce, 1024, b"frame", &key).unwrap();
        assert!(
            !data
                .windows(b"frame".len())
                .any(|window| window == b"frame")
        );
        let decoded = decode_data(&data, begin.request_nonce, &key).unwrap();
        assert_eq!(decoded, (identity(), 1024, b"frame".to_vec()));

        let ack = PlaybackAck {
            identity: identity(),
            status: 0,
            next_offset: 1029,
        };
        assert_eq!(decode_ack(&encode_ack(ack, &key), &key).unwrap(), ack);

        let finished = PlaybackFinished {
            identity: identity(),
            played_samples: 96_001,
        };
        assert_eq!(
            decode_finished(&encode_finished(finished, &key), &key).unwrap(),
            finished
        );
        assert_eq!(
            decode_finished_ack(&encode_finished_ack(identity(), 0, &key), &key).unwrap(),
            (identity(), 0)
        );

        let mailbox = MailboxStatus {
            unread_slots: 0b0101,
            running_tasks: 3,
            coverage_by_slot: [7, 0, 2, 0],
        };
        let mailbox_packet = encode_mailbox_status(mailbox, 0x1122_3344, &key).unwrap();
        assert_eq!(
            decode_mailbox_status(&mailbox_packet, &key).unwrap(),
            (mailbox, 0x1122_3344)
        );
        assert!(
            encode_mailbox_status(
                MailboxStatus {
                    unread_slots: 0,
                    running_tasks: 0,
                    coverage_by_slot: [1, 0, 0, 0],
                },
                1,
                &key,
            )
            .is_err()
        );
        assert!(
            encode_mailbox_status(
                MailboxStatus {
                    unread_slots: 0,
                    running_tasks: 5,
                    coverage_by_slot: [0; 4],
                },
                1,
                &key,
            )
            .is_err()
        );
        let mut invalid_running_count = mailbox_packet.clone();
        invalid_running_count[6] = 5;
        sign(&mut invalid_running_count, &key);
        assert_eq!(
            decode_mailbox_status(&invalid_running_count, &key),
            Err(PlaybackWireError::Malformed)
        );

        let mut tampered = encode_begin(begin, &key);
        tampered[32] ^= 1;
        assert_eq!(
            decode_begin(&tampered, &key),
            Err(PlaybackWireError::Authentication)
        );
    }

    #[test]
    fn request_golden_vector_is_stable() {
        let key = [0x11; 32];
        let packet = encode_request(
            PlaybackRequest {
                slot: 2,
                request_generation: 0x1122_3344,
                connection_generation: 0x5566_7788,
                nonce: 0x0102_0304_0506_0708,
            },
            &key,
        );
        assert_eq!(
            hex(&packet),
            "4549505201020000443322118877665508070605040302010f9068e8dfef3f65705bc66924a7a35b"
        );
        let begin = PlaybackBegin {
            identity: identity(),
            total_bytes: 2049,
            total_samples: 96_001,
            chunk_bytes: PLAYBACK_CHUNK_BYTES as u16,
            request_nonce: 0x2122_2324_2526_2728,
        };
        assert_eq!(
            hex(&encode_begin(begin, &key)),
            "45495042010200004433221188776655080706050403020118171615141312110108000001770100000000000004000028272625242322215eb7cb9543aaff9d9921aca91d17ac4c"
        );
        assert_eq!(
            hex(&encode_data(identity(), begin.request_nonce, 1024, b"frame", &key).unwrap()),
            "4549504401020000443322118877665508070605040302011817161514131211000400000500000072c72e7422760e9748f0f43d33155889a40b43f66e"
        );
        assert_eq!(
            hex(&encode_ack(
                PlaybackAck {
                    identity: identity(),
                    status: 0,
                    next_offset: 1029,
                },
                &key,
            )),
            "454950410102000044332211887766550807060504030201181716151413121105040000d80cbc8e56210eb573dd2ba2bedad79b"
        );
        assert_eq!(
            hex(&encode_finished(
                PlaybackFinished {
                    identity: identity(),
                    played_samples: 96_001,
                },
                &key,
            )),
            "45495046010200004433221188776655080706050403020118171615141312110177010000000000fe239befd07010e57b2e4fbfdf274034"
        );
        assert_eq!(
            hex(&encode_finished_ack(identity(), 0, &key)),
            "4549504b01020000443322118877665508070605040302011817161514131211c698df6a9dcfb02955ab714203c52598"
        );
        assert_eq!(
            hex(&encode_finished_ack(identity(), 1, &key)),
            "4549504b01020100443322118877665508070605040302011817161514131211d807630e0ebf8ef128cad06c93fefb67"
        );
        assert_eq!(
            hex(&encode_mailbox_status(
                MailboxStatus {
                    unread_slots: 0b0101,
                    running_tasks: 3,
                    coverage_by_slot: [7, 0, 2, 0],
                },
                0x1122_3344,
                &key,
            )
            .unwrap()),
            "45494d42030503004433221107000200ee287f10dfdcd199727c2a2bda61e9ec"
        );
    }

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|byte| format!("{byte:02x}")).collect()
    }
}

use std::fs;
use std::io::Read;
use std::net::{IpAddr, Ipv4Addr};
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::Path;
#[cfg(target_os = "macos")]
use std::thread;
#[cfg(target_os = "macos")]
use std::time::{Duration, Instant};

use serde::Serialize;
use thiserror::Error;
use zeroize::Zeroize;

#[cfg(test)]
use crate::lan_voice::LAN_AUDIO_PORT;
use crate::paths::{AppPaths, open_private_file, replace_private_file};

const CONFIG_REPORT_ID: u8 = 0x10;
const REPORT_BYTES: usize = 64;
const REPORT_BODY_BYTES: usize = REPORT_BYTES - 1;
const CONFIG_HEADER_BYTES: usize = 11;
const CONFIG_CHUNK_BYTES: usize = REPORT_BODY_BYTES - CONFIG_HEADER_BYTES;
const MAX_CONFIG_BYTES: usize = 2048;
#[cfg(any(target_os = "macos", test))]
const APP_COMMAND_REPORT_ID: u8 = 0x11;
#[cfg(any(target_os = "macos", test))]
const APP_COMMAND_CONFIG_ACK: u8 = 0x03;
#[cfg(target_os = "macos")]
const CONFIG_ACK_TIMEOUT: Duration = Duration::from_secs(4);
#[cfg(any(target_os = "macos", test))]
const EASY_INPUT_USB_VID: u16 = 0x303A;
#[cfg(any(target_os = "macos", test))]
const EASY_INPUT_USB_PID: u16 = 0x1006;
#[cfg(any(target_os = "macos", test))]
const EASY_INPUT_USB_INTERFACE: i32 = 0;

#[derive(Debug, Error)]
pub enum ProvisioningError {
    #[error("Wi-Fi SSID must be 1..=32 bytes")]
    InvalidSsid,
    #[error("Wi-Fi password exceeds 64 bytes")]
    InvalidPassword,
    #[error("audio host must be a routable IPv4 address")]
    InvalidHost,
    #[error("audio port must be in 1024..=65535")]
    InvalidPort,
    #[error("provisioning payload is invalid")]
    InvalidPayload,
    #[error("AI keyboard HID device was not found")]
    DeviceNotFound,
    #[error("AI keyboard HID provisioning failed")]
    Hid,
    #[error("AI keyboard HID device could not be opened")]
    HidOpen,
    #[error("AI keyboard HID feature report write failed")]
    HidWrite,
    #[error("AI keyboard did not return the exact saved configuration ACK")]
    HidAck,
    #[error("HID provisioning is currently supported only on macOS")]
    UnsupportedPlatform,
}

#[derive(Clone, PartialEq, Eq)]
pub struct LanProvisioning {
    ssid: String,
    password: String,
    host: Ipv4Addr,
    port: u16,
    device_secret: [u8; 32],
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProvisioningReceipt {
    pub transport: &'static str,
    pub product: String,
    pub payload_bytes: usize,
    pub chunks: usize,
    pub crc16: u16,
}

#[derive(Serialize)]
struct ProvisionPayload<'a> {
    schema: &'static str,
    device_name: &'static str,
    wifi_ssid: &'a str,
    wifi_password: &'a str,
    audio_host: String,
    audio_port: u16,
    audio_enabled: bool,
    speaker_sync_key: String,
    speaker_sync_key_epoch: u16,
    profiles: Vec<Profile>,
}

#[derive(Serialize)]
struct Profile {
    id: &'static str,
    keys: std::collections::BTreeMap<&'static str, Key>,
    encoder: Encoder,
}

#[derive(Serialize)]
struct Key {
    press: &'static str,
}

#[derive(Serialize)]
struct Encoder {
    left: &'static str,
    right: &'static str,
    press: &'static str,
}

impl LanProvisioning {
    pub fn new(
        ssid: String,
        password: String,
        host: IpAddr,
        port: u16,
        device_secret: [u8; 32],
    ) -> Result<Self, ProvisioningError> {
        if ssid.is_empty() || ssid.len() > 32 {
            return Err(ProvisioningError::InvalidSsid);
        }
        if password.len() > 64 {
            return Err(ProvisioningError::InvalidPassword);
        }
        let IpAddr::V4(host) = host else {
            return Err(ProvisioningError::InvalidHost);
        };
        if host.is_unspecified()
            || host.is_loopback()
            || host.is_multicast()
            || host.is_broadcast()
            || host.is_link_local()
        {
            return Err(ProvisioningError::InvalidHost);
        }
        if port < 1024 {
            return Err(ProvisioningError::InvalidPort);
        }
        if device_secret.iter().all(|byte| *byte == 0) {
            return Err(ProvisioningError::InvalidPayload);
        }
        Ok(Self {
            ssid,
            password,
            host,
            port,
            device_secret,
        })
    }

    pub fn payload_json(&self) -> Result<Vec<u8>, ProvisioningError> {
        let keys = (1..=8)
            .map(|slot| {
                let name = match slot {
                    1 => "KEY1",
                    2 => "KEY2",
                    3 => "KEY3",
                    4 => "KEY4",
                    5 => "KEY5",
                    6 => "KEY6",
                    7 => "KEY7",
                    _ => "KEY8",
                };
                (name, Key { press: "disabled" })
            })
            .collect();
        let payload = ProvisionPayload {
            schema: "ai_keyboard.v1",
            device_name: "Easy Codex Input",
            wifi_ssid: &self.ssid,
            wifi_password: &self.password,
            audio_host: self.host.to_string(),
            audio_port: self.port,
            audio_enabled: true,
            speaker_sync_key: hex_encode(&self.device_secret),
            speaker_sync_key_epoch: 1,
            profiles: vec![Profile {
                id: "default",
                keys,
                encoder: Encoder {
                    left: "disabled",
                    right: "disabled",
                    press: "disabled",
                },
            }],
        };
        let json = serde_json::to_vec(&payload).map_err(|_| ProvisioningError::InvalidPayload)?;
        if json.is_empty() || json.len() > MAX_CONFIG_BYTES {
            return Err(ProvisioningError::InvalidPayload);
        }
        Ok(json)
    }

    pub fn reports(&self) -> Result<Vec<[u8; REPORT_BYTES]>, ProvisioningError> {
        encode_reports(&self.payload_json()?)
    }
}

pub fn load_device_secret(paths: &AppPaths) -> Result<[u8; 32], ProvisioningError> {
    load_device_secret_path(&paths.device_secret)
}

pub(crate) fn load_device_secret_path(path: &Path) -> Result<[u8; 32], ProvisioningError> {
    let metadata = fs::symlink_metadata(path).map_err(|_| ProvisioningError::InvalidPayload)?;
    if !metadata.is_file()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.permissions().mode() & 0o077 != 0
        || metadata.len() != 65
    {
        return Err(ProvisioningError::InvalidPayload);
    }
    let mut file = open_private_file(path).map_err(|_| ProvisioningError::InvalidPayload)?;
    let mut encoded = String::with_capacity(65);
    file.read_to_string(&mut encoded)
        .map_err(|_| ProvisioningError::InvalidPayload)?;
    let secret = decode_secret_hex(encoded.trim_end())?;
    if secret.iter().all(|byte| *byte == 0) {
        return Err(ProvisioningError::InvalidPayload);
    }
    Ok(secret)
}

pub fn load_or_create_device_secret(paths: &AppPaths) -> Result<[u8; 32], ProvisioningError> {
    match fs::symlink_metadata(&paths.device_secret) {
        Ok(_) => return load_device_secret(paths),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(_) => return Err(ProvisioningError::InvalidPayload),
    }
    paths
        .prepare()
        .map_err(|_| ProvisioningError::InvalidPayload)?;
    let mut random =
        fs::File::open("/dev/urandom").map_err(|_| ProvisioningError::InvalidPayload)?;
    let mut secret = [0_u8; 32];
    random
        .read_exact(&mut secret)
        .map_err(|_| ProvisioningError::InvalidPayload)?;
    if secret.iter().all(|byte| *byte == 0) {
        return Err(ProvisioningError::InvalidPayload);
    }
    let mut encoded = hex_encode(&secret);
    encoded.push('\n');
    replace_private_file(&paths.device_secret, encoded.as_bytes())
        .map_err(|_| ProvisioningError::InvalidPayload)?;
    encoded.zeroize();
    load_device_secret(paths)
}

pub fn provision_lan(config: &LanProvisioning) -> Result<ProvisioningReceipt, ProvisioningError> {
    #[cfg(target_os = "macos")]
    {
        provision_lan_macos(config)
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = config;
        Err(ProvisioningError::UnsupportedPlatform)
    }
}

#[cfg(target_os = "macos")]
fn provision_lan_macos(config: &LanProvisioning) -> Result<ProvisioningReceipt, ProvisioningError> {
    let payload = config.payload_json()?;
    let reports = encode_reports(&payload)?;
    let api = hidapi::HidApi::new().map_err(|_| ProvisioningError::Hid)?;
    api.set_open_exclusive(false);
    let candidates = api
        .device_list()
        .filter(|device| {
            is_easy_input_usb_interface(
                device.vendor_id(),
                device.product_id(),
                device.interface_number(),
            )
        })
        .collect::<Vec<_>>();
    if candidates.is_empty() {
        return Err(ProvisioningError::DeviceNotFound);
    }
    let mut opened = false;
    let mut wrote_all = false;
    for candidate in candidates {
        let Ok(device) = candidate.open_device(&api) else {
            continue;
        };
        opened = true;
        let mut complete = true;
        for report in &reports {
            if device.send_feature_report(report).is_err() {
                complete = false;
                break;
            }
            thread::sleep(Duration::from_millis(20));
        }
        wrote_all |= complete;
        let crc16 = crc16_ccitt(&payload);
        if complete && wait_for_config_ack(&device, payload.len(), crc16).is_ok() {
            return Ok(ProvisioningReceipt {
                transport: "usb_hid",
                product: candidate
                    .product_string()
                    .unwrap_or("AI Keyboard")
                    .to_owned(),
                payload_bytes: payload.len(),
                chunks: reports.len(),
                crc16,
            });
        }
    }
    if !opened {
        Err(ProvisioningError::HidOpen)
    } else if !wrote_all {
        Err(ProvisioningError::HidWrite)
    } else {
        Err(ProvisioningError::HidAck)
    }
}

#[cfg(any(target_os = "macos", test))]
fn is_easy_input_usb_interface(vendor_id: u16, product_id: u16, interface: i32) -> bool {
    vendor_id == EASY_INPUT_USB_VID
        && product_id == EASY_INPUT_USB_PID
        && (interface == EASY_INPUT_USB_INTERFACE || (cfg!(target_os = "macos") && interface == -1))
}

#[cfg(target_os = "macos")]
fn wait_for_config_ack(
    device: &hidapi::HidDevice,
    expected_bytes: usize,
    expected_crc16: u16,
) -> Result<(), ProvisioningError> {
    let expected_bytes = u16::try_from(expected_bytes).map_err(|_| ProvisioningError::Hid)?;
    let deadline = Instant::now() + CONFIG_ACK_TIMEOUT;
    let mut report = [0_u8; REPORT_BYTES];
    while Instant::now() < deadline {
        let remaining = deadline.saturating_duration_since(Instant::now());
        let wait_ms = remaining.as_millis().clamp(1, 200) as i32;
        match device.read_timeout(&mut report, wait_ms) {
            Ok(0) => continue,
            Ok(length) => {
                if let Some(ack) = decode_config_ack(&report[..length]) {
                    return if ack.phase == 1
                        && ack.ok
                        && ack.saved
                        && ack.bytes == expected_bytes
                        && ack.crc16 == expected_crc16
                    {
                        Ok(())
                    } else {
                        Err(ProvisioningError::Hid)
                    };
                }
            }
            Err(_) => return Err(ProvisioningError::Hid),
        }
    }
    Err(ProvisioningError::Hid)
}

#[cfg(any(target_os = "macos", test))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct ConfigAck {
    phase: u8,
    ok: bool,
    bytes: u16,
    crc16: u16,
    saved: bool,
}

#[cfg(any(target_os = "macos", test))]
fn decode_config_ack(report: &[u8]) -> Option<ConfigAck> {
    if report.len() < 12
        || report[0] != APP_COMMAND_REPORT_ID
        || report[1] != APP_COMMAND_CONFIG_ACK
        || report[2] != 0
        || report[3] != 1
        || report[4] != 7
        || report[6] > 1
        || report[11] > 1
    {
        return None;
    }
    Some(ConfigAck {
        phase: report[5],
        ok: report[6] == 1,
        bytes: u16::from_le_bytes([report[7], report[8]]),
        crc16: u16::from_le_bytes([report[9], report[10]]),
        saved: report[11] == 1,
    })
}

fn encode_reports(json: &[u8]) -> Result<Vec<[u8; REPORT_BYTES]>, ProvisioningError> {
    if json.is_empty() || json.len() > MAX_CONFIG_BYTES {
        return Err(ProvisioningError::InvalidPayload);
    }
    let chunks = json.chunks(CONFIG_CHUNK_BYTES).collect::<Vec<_>>();
    if chunks.is_empty() || chunks.len() > u8::MAX as usize || json.len() > u16::MAX as usize {
        return Err(ProvisioningError::InvalidPayload);
    }
    let total_bytes = json.len() as u16;
    let crc = crc16_ccitt(json);
    let total_chunks = chunks.len() as u8;
    Ok(chunks
        .into_iter()
        .enumerate()
        .map(|(index, chunk)| {
            let mut report = [0_u8; REPORT_BYTES];
            report[0] = CONFIG_REPORT_ID;
            report[1..4].copy_from_slice(b"S3C");
            report[4] = 1;
            report[5] = index as u8;
            report[6] = total_chunks;
            report[7..9].copy_from_slice(&total_bytes.to_le_bytes());
            report[9] = chunk.len() as u8;
            report[10..12].copy_from_slice(&crc.to_le_bytes());
            report[12..12 + chunk.len()].copy_from_slice(chunk);
            report
        })
        .collect())
}

fn crc16_ccitt(bytes: &[u8]) -> u16 {
    let mut crc = 0xFFFF_u16;
    for byte in bytes {
        crc ^= (*byte as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 {
                (crc << 1) ^ 0x1021
            } else {
                crc << 1
            };
        }
    }
    crc
}

fn hex_encode(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn decode_secret_hex(value: &str) -> Result<[u8; 32], ProvisioningError> {
    if value.len() != 64 {
        return Err(ProvisioningError::InvalidPayload);
    }
    let mut secret = [0_u8; 32];
    for (index, byte) in secret.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&value[index * 2..index * 2 + 2], 16)
            .map_err(|_| ProvisioningError::InvalidPayload)?;
    }
    if secret.iter().all(|byte| *byte == 0) {
        return Err(ProvisioningError::InvalidPayload);
    }
    Ok(secret)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::symlink;
    use tempfile::tempdir;

    #[test]
    fn payload_enables_lan_audio_without_exposing_password_in_receipt() {
        let config = LanProvisioning::new(
            "Office WiFi".to_owned(),
            "secret password".to_owned(),
            "192.168.1.20".parse().unwrap(),
            LAN_AUDIO_PORT,
            [7; 32],
        )
        .unwrap();
        let payload = config.payload_json().unwrap();
        let value: serde_json::Value = serde_json::from_slice(&payload).unwrap();
        assert_eq!(value["schema"], "ai_keyboard.v1");
        assert_eq!(value["audio_enabled"], true);
        assert_eq!(value["wifi_ssid"], "Office WiFi");
        assert_eq!(value["wifi_password"], "secret password");
        assert_eq!(value["audio_host"], "192.168.1.20");
        assert_eq!(
            value["speaker_sync_key"],
            "0707070707070707070707070707070707070707070707070707070707070707"
        );
        assert_eq!(value["speaker_sync_key_epoch"], 1);
        assert_eq!(value["profiles"][0]["keys"]["KEY1"]["press"], "disabled");
    }

    #[test]
    fn reports_match_s3c_wire_and_reassemble_exact_payload() {
        let config = LanProvisioning::new(
            "Office WiFi".to_owned(),
            "password".to_owned(),
            "192.168.1.20".parse().unwrap(),
            LAN_AUDIO_PORT,
            [8; 32],
        )
        .unwrap();
        let payload = config.payload_json().unwrap();
        let reports = config.reports().unwrap();
        assert!(reports.len() > 1);
        let mut reassembled = Vec::new();
        for (index, report) in reports.iter().enumerate() {
            assert_eq!(report[0], CONFIG_REPORT_ID);
            assert_eq!(&report[1..4], b"S3C");
            assert_eq!(report[4], 1);
            assert_eq!(report[5], index as u8);
            assert_eq!(report[6], reports.len() as u8);
            assert_eq!(
                u16::from_le_bytes([report[7], report[8]]) as usize,
                payload.len()
            );
            assert_eq!(
                u16::from_le_bytes([report[10], report[11]]),
                crc16_ccitt(&payload)
            );
            let length = report[9] as usize;
            reassembled.extend_from_slice(&report[12..12 + length]);
        }
        assert_eq!(reassembled, payload);
    }

    #[test]
    fn config_ack_requires_exact_saved_fingerprint() {
        let mut report = [0_u8; REPORT_BYTES];
        report[0] = APP_COMMAND_REPORT_ID;
        report[1] = APP_COMMAND_CONFIG_ACK;
        report[3] = 1;
        report[4] = 7;
        report[5] = 1;
        report[6] = 1;
        report[7..9].copy_from_slice(&777_u16.to_le_bytes());
        report[9..11].copy_from_slice(&0xBEEF_u16.to_le_bytes());
        report[11] = 1;
        assert_eq!(
            decode_config_ack(&report),
            Some(ConfigAck {
                phase: 1,
                ok: true,
                bytes: 777,
                crc16: 0xBEEF,
                saved: true,
            })
        );
        report[11] = 0;
        assert!(!decode_config_ack(&report).unwrap().saved);
        report[4] = 6;
        assert!(decode_config_ack(&report).is_none());
    }

    #[test]
    fn invalid_network_values_fail_before_hid_access() {
        assert!(
            LanProvisioning::new(
                "".into(),
                "".into(),
                "192.168.1.2".parse().unwrap(),
                17333,
                [1; 32],
            )
            .is_err()
        );
        assert!(
            LanProvisioning::new(
                "wifi".into(),
                "".into(),
                "127.0.0.1".parse().unwrap(),
                17333,
                [1; 32],
            )
            .is_err()
        );
        assert!(
            LanProvisioning::new(
                "wifi".into(),
                "".into(),
                "192.168.1.2".parse().unwrap(),
                80,
                [1; 32],
            )
            .is_err()
        );
        assert!(
            LanProvisioning::new(
                "wifi".into(),
                "x".repeat(65),
                "192.168.1.2".parse().unwrap(),
                17333,
                [1; 32],
            )
            .is_err()
        );
        assert!(
            LanProvisioning::new(
                "wifi".into(),
                "".into(),
                "192.168.1.2".parse().unwrap(),
                17333,
                [0; 32],
            )
            .is_err()
        );
    }

    #[test]
    fn usb_provisioning_matches_only_the_v2_management_interface() {
        assert!(is_easy_input_usb_interface(0x303A, 0x1006, 0));
        #[cfg(target_os = "macos")]
        assert!(is_easy_input_usb_interface(0x303A, 0x1006, -1));
        #[cfg(not(target_os = "macos"))]
        assert!(!is_easy_input_usb_interface(0x303A, 0x1006, -1));
        assert!(!is_easy_input_usb_interface(0x303A, 0x1006, 1));
        assert!(!is_easy_input_usb_interface(0x303A, 0x1005, 0));
        assert!(!is_easy_input_usb_interface(0xFFFF, 0x1006, 0));
    }

    #[test]
    fn device_secret_is_private_persistent_and_nonzero() {
        let temp = tempdir().unwrap();
        let paths = AppPaths::from_root(temp.path().join("app-support"));
        let first = load_or_create_device_secret(&paths).unwrap();
        let second = load_or_create_device_secret(&paths).unwrap();
        assert_eq!(first, second);
        assert!(first.iter().any(|byte| *byte != 0));
        let metadata = fs::symlink_metadata(&paths.device_secret).unwrap();
        assert!(metadata.is_file());
        assert_eq!(metadata.permissions().mode() & 0o777, 0o600);
        assert_eq!(metadata.len(), 65);
    }

    #[test]
    fn existing_symlink_secret_is_rejected_without_replacement() {
        let temp = tempdir().unwrap();
        let paths = AppPaths::from_root(temp.path().join("app-support"));
        paths.prepare().unwrap();
        let target = temp.path().join("target");
        fs::write(&target, b"do-not-touch").unwrap();
        symlink(&target, &paths.device_secret).unwrap();
        assert!(load_or_create_device_secret(&paths).is_err());
        assert!(
            fs::symlink_metadata(&paths.device_secret)
                .unwrap()
                .file_type()
                .is_symlink()
        );
        assert_eq!(fs::read(&target).unwrap(), b"do-not-touch");
    }
}

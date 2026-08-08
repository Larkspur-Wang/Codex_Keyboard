use thiserror::Error;
use zeroize::Zeroizing;

pub const TTS_SAMPLE_RATE: u32 = 48_000;
pub const MAX_TTS_SECONDS: u32 = 90;
pub const EIAD_FRAME_SAMPLES: usize = 960;

const DEVICE_EIAD_FRAME_SAMPLES: usize = 480;
const DEVICE_EIAD_HEADER_BYTES: usize = 20;
const DEVICE_EIAD_FRAME_HEADER_BYTES: usize = 6;

const WAV_HEADER_BYTES: usize = 44;
const EIAD_HEADER_BYTES: usize = 32;
const EIAD_FRAME_HEADER_BYTES: usize = 8;
const EIAD_VERSION: u16 = 1;
const IMA_INDEX_TABLE: [i8; 16] = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];
const IMA_STEP_TABLE: [i32; 89] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];

#[derive(Debug, Error, Clone, Copy, PartialEq, Eq)]
pub enum AudioError {
    #[error("PCM audio is empty, malformed, or exceeds the duration limit")]
    InvalidPcm,
    #[error("WAV audio is malformed or inconsistent")]
    InvalidWav,
    #[error("EIAD audio is malformed or inconsistent")]
    InvalidEiad,
    #[error("audio size exceeds the supported container range")]
    Size,
}

pub struct AudioArtifacts {
    wav: Zeroizing<Vec<u8>>,
    eiad: Zeroizing<Vec<u8>>,
    samples: u64,
    frames: u32,
}

impl AudioArtifacts {
    pub fn wav(&self) -> &[u8] {
        self.wav.as_slice()
    }

    pub fn eiad(&self) -> &[u8] {
        self.eiad.as_slice()
    }

    pub const fn samples(&self) -> u64 {
        self.samples
    }

    pub const fn frames(&self) -> u32 {
        self.frames
    }

    pub fn duration_ms(&self) -> u64 {
        self.samples.saturating_mul(1_000) / u64::from(TTS_SAMPLE_RATE)
    }
}

pub fn encode_tts_audio(pcm_le: &[u8]) -> Result<AudioArtifacts, AudioError> {
    let samples = validate_pcm(pcm_le)?;
    let wav = Zeroizing::new(encode_wav(pcm_le)?);
    let eiad = Zeroizing::new(encode_eiad(pcm_le)?);
    let metadata = inspect_eiad(eiad.as_slice())?;
    if metadata.samples != samples || metadata.sample_rate != TTS_SAMPLE_RATE {
        return Err(AudioError::InvalidEiad);
    }
    Ok(AudioArtifacts {
        wav,
        eiad,
        samples,
        frames: metadata.frames,
    })
}

fn validate_pcm(pcm_le: &[u8]) -> Result<u64, AudioError> {
    if pcm_le.is_empty() || !pcm_le.len().is_multiple_of(2) {
        return Err(AudioError::InvalidPcm);
    }
    let samples = u64::try_from(pcm_le.len() / 2).map_err(|_| AudioError::Size)?;
    let maximum = u64::from(TTS_SAMPLE_RATE) * u64::from(MAX_TTS_SECONDS);
    if samples > maximum {
        Err(AudioError::InvalidPcm)
    } else {
        Ok(samples)
    }
}

fn encode_wav(pcm_le: &[u8]) -> Result<Vec<u8>, AudioError> {
    validate_pcm(pcm_le)?;
    let data_bytes = u32::try_from(pcm_le.len()).map_err(|_| AudioError::Size)?;
    let riff_bytes = data_bytes.checked_add(36).ok_or(AudioError::Size)?;
    let byte_rate = TTS_SAMPLE_RATE.checked_mul(2).ok_or(AudioError::Size)?;
    let capacity = WAV_HEADER_BYTES
        .checked_add(pcm_le.len())
        .ok_or(AudioError::Size)?;
    let mut output = Vec::with_capacity(capacity);
    output.extend_from_slice(b"RIFF");
    output.extend_from_slice(&riff_bytes.to_le_bytes());
    output.extend_from_slice(b"WAVEfmt ");
    output.extend_from_slice(&16_u32.to_le_bytes());
    output.extend_from_slice(&1_u16.to_le_bytes());
    output.extend_from_slice(&1_u16.to_le_bytes());
    output.extend_from_slice(&TTS_SAMPLE_RATE.to_le_bytes());
    output.extend_from_slice(&byte_rate.to_le_bytes());
    output.extend_from_slice(&2_u16.to_le_bytes());
    output.extend_from_slice(&16_u16.to_le_bytes());
    output.extend_from_slice(b"data");
    output.extend_from_slice(&data_bytes.to_le_bytes());
    output.extend_from_slice(pcm_le);
    Ok(output)
}

pub fn decode_wav(wav: &[u8]) -> Result<Zeroizing<Vec<u8>>, AudioError> {
    if wav.len() < WAV_HEADER_BYTES
        || &wav[0..4] != b"RIFF"
        || &wav[8..16] != b"WAVEfmt "
        || read_u32(wav, 16)? != 16
        || read_u16(wav, 20)? != 1
        || read_u16(wav, 22)? != 1
        || read_u32(wav, 24)? != TTS_SAMPLE_RATE
        || read_u32(wav, 28)? != TTS_SAMPLE_RATE * 2
        || read_u16(wav, 32)? != 2
        || read_u16(wav, 34)? != 16
        || &wav[36..40] != b"data"
    {
        return Err(AudioError::InvalidWav);
    }
    let data_bytes = usize::try_from(read_u32(wav, 40)?).map_err(|_| AudioError::InvalidWav)?;
    let expected = WAV_HEADER_BYTES
        .checked_add(data_bytes)
        .ok_or(AudioError::InvalidWav)?;
    if expected != wav.len()
        || read_u32(wav, 4)? != u32::try_from(wav.len() - 8).map_err(|_| AudioError::InvalidWav)?
    {
        return Err(AudioError::InvalidWav);
    }
    validate_pcm(&wav[WAV_HEADER_BYTES..]).map_err(|_| AudioError::InvalidWav)?;
    Ok(Zeroizing::new(wav[WAV_HEADER_BYTES..].to_vec()))
}

fn encode_eiad(pcm_le: &[u8]) -> Result<Vec<u8>, AudioError> {
    let total_samples = validate_pcm(pcm_le)?;
    let total_samples_usize = usize::try_from(total_samples).map_err(|_| AudioError::Size)?;
    let frame_count_usize = total_samples_usize.div_ceil(EIAD_FRAME_SAMPLES);
    let frame_count = u32::try_from(frame_count_usize).map_err(|_| AudioError::Size)?;
    let estimated = EIAD_HEADER_BYTES
        .checked_add(
            frame_count_usize
                .saturating_mul(EIAD_FRAME_HEADER_BYTES + EIAD_FRAME_SAMPLES.div_ceil(2)),
        )
        .ok_or(AudioError::Size)?;
    let mut output = Vec::with_capacity(estimated);
    output.extend_from_slice(b"EIAD");
    output.extend_from_slice(&EIAD_VERSION.to_le_bytes());
    output.extend_from_slice(&(EIAD_HEADER_BYTES as u16).to_le_bytes());
    output.extend_from_slice(&TTS_SAMPLE_RATE.to_le_bytes());
    output.extend_from_slice(&1_u16.to_le_bytes());
    output.extend_from_slice(&4_u16.to_le_bytes());
    output.extend_from_slice(&(EIAD_FRAME_SAMPLES as u16).to_le_bytes());
    output.extend_from_slice(&(EIAD_FRAME_HEADER_BYTES as u16).to_le_bytes());
    output.extend_from_slice(&total_samples.to_le_bytes());
    output.extend_from_slice(&frame_count.to_le_bytes());

    let samples = Zeroizing::new(
        pcm_le
            .chunks_exact(2)
            .map(|pair| i16::from_le_bytes([pair[0], pair[1]]))
            .collect::<Vec<_>>(),
    );
    for frame in samples.chunks(EIAD_FRAME_SAMPLES) {
        encode_ima_frame(frame, &mut output)?;
    }
    Ok(output)
}

fn encode_ima_frame(samples: &[i16], output: &mut Vec<u8>) -> Result<(), AudioError> {
    encode_ima_frame_with_layout(samples, output, true)
}

fn encode_ima_frame_with_layout(
    samples: &[i16],
    output: &mut Vec<u8>,
    include_payload_length: bool,
) -> Result<(), AudioError> {
    let (&first, rest) = samples.split_first().ok_or(AudioError::InvalidPcm)?;
    let encoded_bytes = rest.len().div_ceil(2);
    let initial_step_index = rest.first().map_or(0, |second| {
        let difference = (i32::from(*second) - i32::from(first)).unsigned_abs() as i32;
        IMA_STEP_TABLE
            .iter()
            .position(|step| *step >= difference.max(7))
            .unwrap_or(88) as u8
    });
    output.extend_from_slice(
        &u16::try_from(samples.len())
            .map_err(|_| AudioError::Size)?
            .to_le_bytes(),
    );
    output.extend_from_slice(&first.to_le_bytes());
    output.push(initial_step_index);
    output.push(0);
    if include_payload_length {
        output.extend_from_slice(
            &u16::try_from(encoded_bytes)
                .map_err(|_| AudioError::Size)?
                .to_le_bytes(),
        );
    }

    let mut predictor = i32::from(first);
    let mut step_index = i32::from(initial_step_index);
    let mut pending = None;
    for &sample in rest {
        let nibble = encode_ima_sample(i32::from(sample), &mut predictor, &mut step_index);
        if let Some(low) = pending.take() {
            output.push(low | (nibble << 4));
        } else {
            pending = Some(nibble);
        }
    }
    if let Some(low) = pending {
        output.push(low);
    }
    Ok(())
}

// The encrypted cache keeps the Host EIAD container. The V2 firmware speaker
// consumes the flash sound-bank EIAD layout (20-byte header, 480-sample frames),
// so convert only at the authenticated LAN transport boundary.
pub fn transcode_eiad_for_device(eiad: &[u8]) -> Result<Zeroizing<Vec<u8>>, AudioError> {
    let pcm = decode_eiad(eiad)?;
    let total_samples = validate_pcm(pcm.as_slice())?;
    let total_samples_usize = usize::try_from(total_samples).map_err(|_| AudioError::Size)?;
    let frame_count_usize = total_samples_usize.div_ceil(DEVICE_EIAD_FRAME_SAMPLES);
    let frame_count = u16::try_from(frame_count_usize).map_err(|_| AudioError::Size)?;
    let total_samples_u32 = u32::try_from(total_samples).map_err(|_| AudioError::Size)?;
    let estimated =
        DEVICE_EIAD_HEADER_BYTES
            .checked_add(frame_count_usize.saturating_mul(
                DEVICE_EIAD_FRAME_HEADER_BYTES + DEVICE_EIAD_FRAME_SAMPLES.div_ceil(2),
            ))
            .ok_or(AudioError::Size)?;
    let mut output = Zeroizing::new(Vec::with_capacity(estimated));
    output.extend_from_slice(b"EIAD");
    output.push(1);
    output.push(1);
    output.extend_from_slice(&TTS_SAMPLE_RATE.to_le_bytes());
    output.extend_from_slice(&(DEVICE_EIAD_FRAME_SAMPLES as u16).to_le_bytes());
    output.extend_from_slice(&frame_count.to_le_bytes());
    output.extend_from_slice(&total_samples_u32.to_le_bytes());
    output.extend_from_slice(&(DEVICE_EIAD_HEADER_BYTES as u16).to_le_bytes());

    let samples = Zeroizing::new(
        pcm.chunks_exact(2)
            .map(|pair| i16::from_le_bytes([pair[0], pair[1]]))
            .collect::<Vec<_>>(),
    );
    for frame in samples.chunks(DEVICE_EIAD_FRAME_SAMPLES) {
        encode_ima_frame_with_layout(frame, &mut output, false)?;
    }
    inspect_device_eiad(output.as_slice())?;
    Ok(output)
}

fn inspect_device_eiad(eiad: &[u8]) -> Result<EiadMetadata, AudioError> {
    if eiad.len() < DEVICE_EIAD_HEADER_BYTES
        || &eiad[..4] != b"EIAD"
        || eiad[4] != 1
        || eiad[5] != 1
        || read_u32(eiad, 6)? != TTS_SAMPLE_RATE
        || read_u16(eiad, 10)? != DEVICE_EIAD_FRAME_SAMPLES as u16
        || read_u16(eiad, 18)? != DEVICE_EIAD_HEADER_BYTES as u16
    {
        return Err(AudioError::InvalidEiad);
    }
    let frames = u32::from(read_u16(eiad, 12)?);
    let samples = u64::from(read_u32(eiad, 14)?);
    if samples == 0
        || samples > u64::from(TTS_SAMPLE_RATE) * u64::from(MAX_TTS_SECONDS)
        || u64::from(frames) != samples.div_ceil(DEVICE_EIAD_FRAME_SAMPLES as u64)
    {
        return Err(AudioError::InvalidEiad);
    }
    let mut offset = DEVICE_EIAD_HEADER_BYTES;
    let mut counted_samples = 0_u64;
    for index in 0..frames {
        let end = offset
            .checked_add(DEVICE_EIAD_FRAME_HEADER_BYTES)
            .ok_or(AudioError::InvalidEiad)?;
        if end > eiad.len() {
            return Err(AudioError::InvalidEiad);
        }
        let frame_samples = usize::from(read_u16(eiad, offset)?);
        let expected_samples = if index + 1 == frames {
            usize::try_from(samples - counted_samples).map_err(|_| AudioError::InvalidEiad)?
        } else {
            DEVICE_EIAD_FRAME_SAMPLES
        };
        if frame_samples != expected_samples
            || frame_samples == 0
            || eiad[offset + 4] > 88
            || eiad[offset + 5] != 0
        {
            return Err(AudioError::InvalidEiad);
        }
        let payload_bytes = frame_samples.saturating_sub(1).div_ceil(2);
        offset = end
            .checked_add(payload_bytes)
            .ok_or(AudioError::InvalidEiad)?;
        if offset > eiad.len() {
            return Err(AudioError::InvalidEiad);
        }
        counted_samples = counted_samples
            .checked_add(frame_samples as u64)
            .ok_or(AudioError::InvalidEiad)?;
    }
    if offset != eiad.len() || counted_samples != samples {
        return Err(AudioError::InvalidEiad);
    }
    Ok(EiadMetadata {
        sample_rate: TTS_SAMPLE_RATE,
        samples,
        frames,
    })
}

fn encode_ima_sample(sample: i32, predictor: &mut i32, step_index: &mut i32) -> u8 {
    let step = IMA_STEP_TABLE[*step_index as usize];
    let mut difference = sample - *predictor;
    let mut code = 0_u8;
    if difference < 0 {
        code |= 8;
        difference = -difference;
    }
    let mut delta = step >> 3;
    if difference >= step {
        code |= 4;
        difference -= step;
        delta += step;
    }
    if difference >= step >> 1 {
        code |= 2;
        difference -= step >> 1;
        delta += step >> 1;
    }
    if difference >= step >> 2 {
        code |= 1;
        delta += step >> 2;
    }
    if code & 8 != 0 {
        *predictor -= delta;
    } else {
        *predictor += delta;
    }
    *predictor = (*predictor).clamp(i32::from(i16::MIN), i32::from(i16::MAX));
    *step_index = (*step_index + i32::from(IMA_INDEX_TABLE[code as usize])).clamp(0, 88);
    code
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EiadMetadata {
    pub sample_rate: u32,
    pub samples: u64,
    pub frames: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EiadSignal {
    pub absolute_peak: u16,
    pub rms_permille: u16,
}

pub fn inspect_eiad(eiad: &[u8]) -> Result<EiadMetadata, AudioError> {
    if eiad.len() < EIAD_HEADER_BYTES
        || &eiad[..4] != b"EIAD"
        || read_u16(eiad, 4)? != EIAD_VERSION
        || read_u16(eiad, 6)? != EIAD_HEADER_BYTES as u16
        || read_u32(eiad, 8)? != TTS_SAMPLE_RATE
        || read_u16(eiad, 12)? != 1
        || read_u16(eiad, 14)? != 4
        || read_u16(eiad, 16)? != EIAD_FRAME_SAMPLES as u16
        || read_u16(eiad, 18)? != EIAD_FRAME_HEADER_BYTES as u16
    {
        return Err(AudioError::InvalidEiad);
    }
    let samples = read_u64(eiad, 20)?;
    let frames = read_u32(eiad, 28)?;
    if samples == 0
        || samples > u64::from(TTS_SAMPLE_RATE) * u64::from(MAX_TTS_SECONDS)
        || u64::from(frames) != samples.div_ceil(EIAD_FRAME_SAMPLES as u64)
    {
        return Err(AudioError::InvalidEiad);
    }
    let mut offset = EIAD_HEADER_BYTES;
    let mut counted_samples = 0_u64;
    for index in 0..frames {
        let end = offset
            .checked_add(EIAD_FRAME_HEADER_BYTES)
            .ok_or(AudioError::InvalidEiad)?;
        if end > eiad.len() {
            return Err(AudioError::InvalidEiad);
        }
        let frame_samples = usize::from(read_u16(eiad, offset)?);
        let step_index = eiad[offset + 4];
        let reserved = eiad[offset + 5];
        let payload_bytes = usize::from(read_u16(eiad, offset + 6)?);
        let expected_samples = if index + 1 == frames {
            usize::try_from(samples - counted_samples).map_err(|_| AudioError::InvalidEiad)?
        } else {
            EIAD_FRAME_SAMPLES
        };
        let expected_payload = expected_samples.saturating_sub(1).div_ceil(2);
        if frame_samples != expected_samples
            || frame_samples == 0
            || frame_samples > EIAD_FRAME_SAMPLES
            || step_index > 88
            || reserved != 0
            || payload_bytes != expected_payload
        {
            return Err(AudioError::InvalidEiad);
        }
        let frame_end = end
            .checked_add(payload_bytes)
            .ok_or(AudioError::InvalidEiad)?;
        if frame_end > eiad.len() {
            return Err(AudioError::InvalidEiad);
        }
        if !(frame_samples - 1).is_multiple_of(2) && eiad[frame_end - 1] & 0xf0 != 0 {
            return Err(AudioError::InvalidEiad);
        }
        offset = frame_end;
        counted_samples = counted_samples
            .checked_add(frame_samples as u64)
            .ok_or(AudioError::InvalidEiad)?;
    }
    if offset != eiad.len() || counted_samples != samples {
        return Err(AudioError::InvalidEiad);
    }
    Ok(EiadMetadata {
        sample_rate: TTS_SAMPLE_RATE,
        samples,
        frames,
    })
}

pub fn decode_eiad(eiad: &[u8]) -> Result<Zeroizing<Vec<u8>>, AudioError> {
    let metadata = inspect_eiad(eiad)?;
    let capacity = usize::try_from(metadata.samples.checked_mul(2).ok_or(AudioError::Size)?)
        .map_err(|_| AudioError::Size)?;
    let mut output = Zeroizing::new(Vec::with_capacity(capacity));
    let mut offset = EIAD_HEADER_BYTES;
    for _ in 0..metadata.frames {
        let samples = usize::from(read_u16(eiad, offset)?);
        let mut predictor = i32::from(i16::from_le_bytes([eiad[offset + 2], eiad[offset + 3]]));
        let mut step_index = i32::from(eiad[offset + 4]);
        let payload_bytes = usize::from(read_u16(eiad, offset + 6)?);
        offset += EIAD_FRAME_HEADER_BYTES;
        output.extend_from_slice(&(predictor as i16).to_le_bytes());
        for sample_index in 1..samples {
            let packed = eiad[offset + (sample_index - 1) / 2];
            let nibble = if !sample_index.is_multiple_of(2) {
                packed & 0x0f
            } else {
                packed >> 4
            };
            predictor = decode_ima_sample(nibble, predictor, &mut step_index);
            output.extend_from_slice(&(predictor as i16).to_le_bytes());
        }
        offset += payload_bytes;
    }
    Ok(output)
}

pub fn inspect_eiad_signal(eiad: &[u8]) -> Result<EiadSignal, AudioError> {
    let pcm = decode_eiad(eiad)?;
    let mut peak = 0_u32;
    let mut sum_squares = 0_u64;
    let mut samples = 0_u64;
    for pair in pcm.chunks_exact(2) {
        let sample = i32::from(i16::from_le_bytes([pair[0], pair[1]]));
        let magnitude = sample.unsigned_abs();
        peak = peak.max(magnitude);
        sum_squares = sum_squares.saturating_add(u64::from(magnitude) * u64::from(magnitude));
        samples += 1;
    }
    if samples == 0 {
        return Err(AudioError::InvalidEiad);
    }
    let rms = ((sum_squares as f64 / samples as f64).sqrt() * 1_000.0 / 32_767.0)
        .round()
        .clamp(0.0, 1_000.0) as u16;
    Ok(EiadSignal {
        absolute_peak: peak.min(i16::MAX as u32) as u16,
        rms_permille: rms,
    })
}

fn decode_ima_sample(code: u8, predictor: i32, step_index: &mut i32) -> i32 {
    let step = IMA_STEP_TABLE[*step_index as usize];
    let mut delta = step >> 3;
    if code & 4 != 0 {
        delta += step;
    }
    if code & 2 != 0 {
        delta += step >> 1;
    }
    if code & 1 != 0 {
        delta += step >> 2;
    }
    let predictor = if code & 8 != 0 {
        predictor - delta
    } else {
        predictor + delta
    }
    .clamp(i32::from(i16::MIN), i32::from(i16::MAX));
    *step_index = (*step_index + i32::from(IMA_INDEX_TABLE[code as usize])).clamp(0, 88);
    predictor
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16, AudioError> {
    let value = bytes
        .get(offset..offset + 2)
        .ok_or(AudioError::InvalidEiad)?;
    Ok(u16::from_le_bytes([value[0], value[1]]))
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32, AudioError> {
    let value = bytes
        .get(offset..offset + 4)
        .ok_or(AudioError::InvalidEiad)?;
    Ok(u32::from_le_bytes(
        value.try_into().map_err(|_| AudioError::InvalidEiad)?,
    ))
}

fn read_u64(bytes: &[u8], offset: usize) -> Result<u64, AudioError> {
    let value = bytes
        .get(offset..offset + 8)
        .ok_or(AudioError::InvalidEiad)?;
    Ok(u64::from_le_bytes(
        value.try_into().map_err(|_| AudioError::InvalidEiad)?,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pcm(samples: usize) -> Vec<u8> {
        (0..samples)
            .flat_map(|index| {
                let value = (((index as i32 * 977) % 50_000) - 25_000) as i16;
                value.to_le_bytes()
            })
            .collect()
    }

    #[test]
    fn wav_is_canonical_pcm16_mono_48khz_and_roundtrips_exactly() {
        let source = pcm(2_401);
        let artifacts = encode_tts_audio(&source).unwrap();
        assert_eq!(decode_wav(artifacts.wav()).unwrap().as_slice(), source);
        assert_eq!(artifacts.samples(), 2_401);
        assert_eq!(artifacts.duration_ms(), 50);
    }

    #[test]
    fn eiad_uses_independent_960_sample_frames_and_exact_tail_counts() {
        for samples in [1, 959, 960, 961, 1_920, 1_921] {
            let artifacts = encode_tts_audio(&pcm(samples)).unwrap();
            let metadata = inspect_eiad(artifacts.eiad()).unwrap();
            assert_eq!(metadata.samples, samples as u64);
            assert_eq!(metadata.frames, samples.div_ceil(EIAD_FRAME_SAMPLES) as u32);
            assert_eq!(artifacts.frames(), metadata.frames);
            assert_eq!(decode_eiad(artifacts.eiad()).unwrap().len(), samples * 2);
        }
    }

    #[test]
    fn every_eiad_frame_is_decodable_without_previous_frame_state() {
        let source = pcm(EIAD_FRAME_SAMPLES * 3 + 17);
        let artifacts = encode_tts_audio(&source).unwrap();
        let decoded = decode_eiad(artifacts.eiad()).unwrap();
        for frame in 0..artifacts.frames() as usize {
            let start = frame * EIAD_FRAME_SAMPLES * 2;
            let first = i16::from_le_bytes(decoded[start..start + 2].try_into().unwrap());
            let expected = i16::from_le_bytes(source[start..start + 2].try_into().unwrap());
            assert_eq!(first, expected);
        }
    }

    #[test]
    fn ima_error_is_bounded_for_representative_waveform() {
        let source = (0..48_000)
            .flat_map(|index| {
                let phase = index as f64 * std::f64::consts::TAU * 440.0 / 48_000.0;
                ((phase.sin() * 20_000.0) as i16).to_le_bytes()
            })
            .collect::<Vec<_>>();
        let decoded = decode_eiad(encode_tts_audio(&source).unwrap().eiad()).unwrap();
        let maximum_error = source
            .chunks_exact(2)
            .zip(decoded.chunks_exact(2))
            .map(|(expected, actual)| {
                let expected = i32::from(i16::from_le_bytes(expected.try_into().unwrap()));
                let actual = i32::from(i16::from_le_bytes(actual.try_into().unwrap()));
                (expected - actual).unsigned_abs()
            })
            .max()
            .unwrap();
        assert!(
            maximum_error < 5_000,
            "unexpected IMA error {maximum_error}"
        );
    }

    #[test]
    fn eiad_signal_distinguishes_silence_from_audible_pcm() {
        let silence = encode_tts_audio(&vec![0_u8; EIAD_FRAME_SAMPLES * 2]).unwrap();
        assert_eq!(
            inspect_eiad_signal(silence.eiad()).unwrap(),
            EiadSignal {
                absolute_peak: 0,
                rms_permille: 0,
            }
        );

        let audible = encode_tts_audio(&pcm(EIAD_FRAME_SAMPLES)).unwrap();
        let signal = inspect_eiad_signal(audible.eiad()).unwrap();
        assert!(signal.absolute_peak > 20_000);
        assert!(signal.rms_permille > 300);
    }

    #[test]
    fn cached_eiad_transcodes_to_the_firmware_sound_bank_layout() {
        for sample_count in [1, 479, 480, 481, 620_280] {
            let artifacts = encode_tts_audio(&pcm(sample_count)).unwrap();
            let device = transcode_eiad_for_device(artifacts.eiad()).unwrap();
            let metadata = inspect_device_eiad(device.as_slice()).unwrap();
            assert_eq!(&device[..4], b"EIAD");
            assert_eq!(device[4], 1);
            assert_eq!(device[5], 1);
            assert_eq!(read_u16(device.as_slice(), 10).unwrap(), 480);
            assert_eq!(read_u16(device.as_slice(), 18).unwrap(), 20);
            assert_eq!(metadata.samples, sample_count as u64);
            assert_eq!(metadata.frames, sample_count.div_ceil(480) as u32);
        }
    }

    #[test]
    fn malformed_empty_odd_and_over_ninety_second_pcm_are_rejected() {
        assert!(matches!(encode_tts_audio(&[]), Err(AudioError::InvalidPcm)));
        assert!(matches!(
            encode_tts_audio(&[0]),
            Err(AudioError::InvalidPcm)
        ));
        let oversized = vec![0_u8; (TTS_SAMPLE_RATE * (MAX_TTS_SECONDS + 1) * 2) as usize];
        assert!(matches!(
            encode_tts_audio(&oversized),
            Err(AudioError::InvalidPcm)
        ));
    }

    #[test]
    fn wav_and_eiad_reject_truncation_trailing_bytes_and_header_lies() {
        let artifacts = encode_tts_audio(&pcm(1_001)).unwrap();
        let mut wav = artifacts.wav().to_vec();
        wav.pop();
        assert!(decode_wav(&wav).is_err());
        let mut eiad = artifacts.eiad().to_vec();
        eiad.pop();
        assert!(inspect_eiad(&eiad).is_err());
        let mut eiad = artifacts.eiad().to_vec();
        eiad.push(0);
        assert!(inspect_eiad(&eiad).is_err());
        let mut eiad = artifacts.eiad().to_vec();
        eiad[28..32].copy_from_slice(&99_u32.to_le_bytes());
        assert!(inspect_eiad(&eiad).is_err());
    }

    #[test]
    fn eiad_rejects_nonzero_unused_high_nibble() {
        let artifacts = encode_tts_audio(&pcm(4)).unwrap();
        let mut eiad = artifacts.eiad().to_vec();
        *eiad.last_mut().unwrap() |= 0xf0;
        assert!(inspect_eiad(&eiad).is_err());
        assert!(decode_eiad(&eiad).is_err());
    }

    #[test]
    fn published_eiad_v1_golden_vector_matches_wav_encoder_and_ima_decoder() {
        let vector: serde_json::Value =
            serde_json::from_str(include_str!("../testdata/eiad-v1.json")).unwrap();
        let samples = vector["input_pcm_samples"]
            .as_array()
            .unwrap()
            .iter()
            .map(|sample| sample.as_i64().unwrap() as i16)
            .collect::<Vec<_>>();
        let pcm = samples
            .iter()
            .flat_map(|sample| sample.to_le_bytes())
            .collect::<Vec<_>>();
        let artifacts = encode_tts_audio(&pcm).unwrap();
        let hex = |bytes: &[u8]| {
            const DIGITS: &[u8; 16] = b"0123456789abcdef";
            let mut output = String::with_capacity(bytes.len() * 2);
            for byte in bytes {
                output.push(DIGITS[(byte >> 4) as usize] as char);
                output.push(DIGITS[(byte & 0x0f) as usize] as char);
            }
            output
        };
        assert_eq!(hex(artifacts.wav()), vector["wav_hex"]);
        assert_eq!(hex(artifacts.eiad()), vector["eiad_hex"]);
        let decoded = decode_eiad(artifacts.eiad()).unwrap();
        let decoded = decoded
            .chunks_exact(2)
            .map(|pair| i16::from_le_bytes(pair.try_into().unwrap()) as i64)
            .collect::<Vec<_>>();
        let expected = vector["decoded_pcm_samples"]
            .as_array()
            .unwrap()
            .iter()
            .map(|sample| sample.as_i64().unwrap())
            .collect::<Vec<_>>();
        assert_eq!(decoded, expected);
    }
}

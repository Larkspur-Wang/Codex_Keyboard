use std::collections::BTreeSet;

use serde::{Deserialize, Serialize};
use thiserror::Error;
use zeroize::Zeroize;

pub const SUMMARY_SCHEMA_VERSION: u8 = 1;
pub const MAX_SUMMARY_DOCUMENT_BYTES: usize = 64 * 1024;
pub const MAX_SUMMARY_ITEMS_PER_SECTION: usize = 32;
pub const MAX_SUMMARY_ITEM_BYTES: usize = 1024;
pub const MAX_SPOKEN_TEXT_BYTES: usize = 24 * 1024;
pub const MAX_COVERS_NEW_COMPLETIONS: usize = 32;
pub const MAX_SOURCE_EVIDENCE_QUOTE_BYTES: usize = 512;
const MAX_SOURCE_EVIDENCE_QUOTE_CHARS: usize = 64;
const TOTAL_SOURCE_EVIDENCE_QUOTE_CHARS: usize = 128;
const MIN_SOURCE_EVIDENCE_QUOTE_CHARS: usize = 4;
const MAX_AUDIBLE_SPOKEN_CHARACTERS: usize = 480;

#[derive(Debug, Error, Clone, Copy, PartialEq, Eq)]
pub enum SummaryDocumentError {
    #[error("summary document is empty or exceeds its size limit")]
    Size,
    #[error("summary document is not valid schema JSON")]
    Json,
    #[error("summary document uses an unsupported schema version")]
    Schema,
    #[error("summary document contains an invalid bounded text field")]
    Text,
    #[error("summary document contains an invalid completion coverage list")]
    Coverage,
    #[error("summary document does not contain valid source evidence")]
    Evidence,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SummarySourceEvidence {
    pub completion_id: String,
    pub exact_quote: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SummaryDocument {
    pub schema: u8,
    pub facts: Vec<String>,
    pub pending: Vec<String>,
    pub decisions: Vec<String>,
    pub spoken_text: String,
    #[serde(default)]
    pub source_evidence: Vec<SummarySourceEvidence>,
    /// The ordered completion delta in this generation's immutable claim.
    /// Cumulative coverage lives in SQLite's completion ledger.
    pub covers_new_completions: Vec<String>,
}

impl Drop for SummaryDocument {
    fn drop(&mut self) {
        self.facts.zeroize();
        self.pending.zeroize();
        self.decisions.zeroize();
        self.spoken_text.zeroize();
        for evidence in &mut self.source_evidence {
            evidence.completion_id.zeroize();
            evidence.exact_quote.zeroize();
        }
        self.source_evidence.clear();
        self.covers_new_completions.zeroize();
    }
}

impl SummaryDocument {
    pub fn parse(bytes: &[u8]) -> Result<Self, SummaryDocumentError> {
        if bytes.is_empty() || bytes.len() > MAX_SUMMARY_DOCUMENT_BYTES {
            return Err(SummaryDocumentError::Size);
        }
        let document: Self =
            serde_json::from_slice(bytes).map_err(|_| SummaryDocumentError::Json)?;
        document.validate()?;
        Ok(document)
    }

    pub fn validate_expected_covers(
        &self,
        expected: &[String],
    ) -> Result<(), SummaryDocumentError> {
        self.validate()?;
        if self.covers_new_completions == expected {
            Ok(())
        } else {
            Err(SummaryDocumentError::Coverage)
        }
    }

    pub fn validate_source_evidence(
        &self,
        expected: &[(&str, &str, &str)],
    ) -> Result<(), SummaryDocumentError> {
        self.validate()?;
        if self.source_evidence.len() != expected.len() {
            return Err(SummaryDocumentError::Evidence);
        }
        for (evidence, (completion_id, assistant_final, required_quote)) in
            self.source_evidence.iter().zip(expected)
        {
            if evidence.completion_id != *completion_id
                || evidence.exact_quote != *required_quote
                || !assistant_final.contains(&evidence.exact_quote)
            {
                return Err(SummaryDocumentError::Evidence);
            }
        }
        Ok(())
    }

    pub fn canonical_json(&self) -> Result<Vec<u8>, SummaryDocumentError> {
        self.validate()?;
        let encoded = serde_json::to_vec(self).map_err(|_| SummaryDocumentError::Json)?;
        if encoded.len() > MAX_SUMMARY_DOCUMENT_BYTES {
            Err(SummaryDocumentError::Size)
        } else {
            Ok(encoded)
        }
    }

    pub(crate) fn validate(&self) -> Result<(), SummaryDocumentError> {
        if self.schema != SUMMARY_SCHEMA_VERSION {
            return Err(SummaryDocumentError::Schema);
        }
        validate_items(&self.facts)?;
        validate_items(&self.pending)?;
        validate_items(&self.decisions)?;
        if !valid_text(&self.spoken_text, MAX_SPOKEN_TEXT_BYTES) {
            return Err(SummaryDocumentError::Text);
        }
        if self.covers_new_completions.is_empty()
            || self.covers_new_completions.len() > MAX_COVERS_NEW_COMPLETIONS
        {
            return Err(SummaryDocumentError::Coverage);
        }
        let mut unique = BTreeSet::new();
        for completion_id in &self.covers_new_completions {
            if uuid::Uuid::parse_str(completion_id).is_err()
                || !unique.insert(completion_id.as_str())
            {
                return Err(SummaryDocumentError::Coverage);
            }
        }
        if self.source_evidence.len() > MAX_COVERS_NEW_COMPLETIONS {
            return Err(SummaryDocumentError::Evidence);
        }
        let mut evidence_ids = BTreeSet::new();
        for evidence in &self.source_evidence {
            if uuid::Uuid::parse_str(&evidence.completion_id).is_err()
                || !evidence_ids.insert(evidence.completion_id.as_str())
                || !valid_text(&evidence.exact_quote, MAX_SOURCE_EVIDENCE_QUOTE_BYTES)
            {
                return Err(SummaryDocumentError::Evidence);
            }
        }
        Ok(())
    }
}

pub(crate) fn source_evidence_quote_budget(completion_count: usize) -> usize {
    (TOTAL_SOURCE_EVIDENCE_QUOTE_CHARS / completion_count.max(1)).clamp(
        MIN_SOURCE_EVIDENCE_QUOTE_CHARS,
        MAX_SOURCE_EVIDENCE_QUOTE_CHARS,
    )
}

pub(crate) fn required_source_evidence_quote(
    assistant_final: &str,
    maximum_characters: usize,
) -> Option<String> {
    let best = assistant_final
        .split(['\n', '\r', '。', '！', '？', '!', '?', '；', ';'])
        .map(str::trim)
        .filter(|segment| !segment.is_empty())
        .max_by_key(|segment| semantic_character_count(segment))?;

    let mut quote = String::new();
    for character in best
        .chars()
        .take(maximum_characters.clamp(1, MAX_SOURCE_EVIDENCE_QUOTE_CHARS))
    {
        if quote.len() + character.len_utf8() > MAX_SOURCE_EVIDENCE_QUOTE_BYTES {
            break;
        }
        quote.push(character);
    }
    (!quote.is_empty()).then_some(quote)
}

fn validate_items(items: &[String]) -> Result<(), SummaryDocumentError> {
    if items.len() > MAX_SUMMARY_ITEMS_PER_SECTION
        || items
            .iter()
            .any(|item| !valid_text(item, MAX_SUMMARY_ITEM_BYTES))
    {
        Err(SummaryDocumentError::Text)
    } else {
        Ok(())
    }
}

fn valid_text(value: &str, max_bytes: usize) -> bool {
    !value.trim().is_empty() && value.len() <= max_bytes && !value.chars().any(char::is_control)
}

pub(crate) fn contains_han_text(value: &str) -> bool {
    value.chars().any(|character| {
        matches!(
            character as u32,
            0x3400..=0x4DBF | 0x4E00..=0x9FFF | 0xF900..=0xFAFF
        )
    })
}

pub(crate) fn meaningful_for_speech(document: &SummaryDocument) -> bool {
    document
        .facts
        .iter()
        .any(|fact| semantic_character_count(fact) >= 8)
        && (16..=MAX_AUDIBLE_SPOKEN_CHARACTERS)
            .contains(&semantic_character_count(&document.spoken_text))
}

pub(crate) fn preserves_previous_unheard(
    previous: Option<&SummaryDocument>,
    candidate: &SummaryDocument,
) -> bool {
    let Some(previous) = previous else {
        return true;
    };
    previous
        .facts
        .iter()
        .all(|item| candidate.facts.contains(item))
        && previous
            .pending
            .iter()
            .all(|item| candidate.pending.contains(item))
        && previous
            .decisions
            .iter()
            .all(|item| candidate.decisions.contains(item))
        && !candidate.spoken_text.trim().is_empty()
}

pub(crate) fn incorporates_new_completion(
    previous: Option<&SummaryDocument>,
    candidate: &SummaryDocument,
) -> bool {
    let Some(previous) = previous else {
        return true;
    };
    let has_new_structured_content =
        candidate
            .facts
            .iter()
            .any(|item| !previous.facts.contains(item) && semantic_character_count(item) >= 8)
            || candidate.pending.iter().any(|item| {
                !previous.pending.contains(item) && semantic_character_count(item) >= 8
            })
            || candidate.decisions.iter().any(|item| {
                !previous.decisions.contains(item) && semantic_character_count(item) >= 8
            });
    let new_spoken = candidate.spoken_text.replacen(&previous.spoken_text, "", 1);
    has_new_structured_content && semantic_character_count(&new_spoken) >= 12
}

fn semantic_character_count(value: &str) -> usize {
    value
        .chars()
        .filter(|character| character.is_alphanumeric())
        .count()
}

#[cfg(test)]
mod tests {
    use super::*;

    const COMPLETION_A: &str = "019fa972-5cfa-75e1-9008-0b17ade9a348";
    const COMPLETION_B: &str = "019fa972-5cfa-75e1-9008-0b17ade9a349";

    fn document() -> SummaryDocument {
        SummaryDocument {
            schema: SUMMARY_SCHEMA_VERSION,
            facts: vec!["Implemented the bounded summary ledger.".into()],
            pending: vec!["Connect the realtime TTS client.".into()],
            decisions: vec!["Keep plaintext out of SQLite.".into()],
            spoken_text: "The summary ledger is ready, and realtime TTS is next.".into(),
            source_evidence: vec![],
            covers_new_completions: vec![COMPLETION_A.into(), COMPLETION_B.into()],
        }
    }

    #[test]
    fn canonical_document_round_trips_and_requires_exact_coverage_order() {
        let document = document();
        let encoded = document.canonical_json().unwrap();
        assert_eq!(SummaryDocument::parse(&encoded).unwrap(), document);
        assert!(
            document
                .validate_expected_covers(&[COMPLETION_A.into(), COMPLETION_B.into()])
                .is_ok()
        );
        assert_eq!(
            document.validate_expected_covers(&[COMPLETION_B.into(), COMPLETION_A.into()]),
            Err(SummaryDocumentError::Coverage)
        );
    }

    #[test]
    fn rejects_unknown_schema_fields_duplicates_controls_and_oversize() {
        let unknown = format!(
            r#"{{"schema":1,"facts":[],"pending":[],"decisions":[],"spoken_text":"ok","covers_new_completions":["{COMPLETION_A}"],"extra":true}}"#
        );
        assert_eq!(
            SummaryDocument::parse(unknown.as_bytes()),
            Err(SummaryDocumentError::Json)
        );

        let mut invalid = document();
        invalid.schema = 2;
        assert_eq!(invalid.canonical_json(), Err(SummaryDocumentError::Schema));
        invalid.schema = 1;
        invalid.spoken_text = "bad\ntext".into();
        assert_eq!(invalid.canonical_json(), Err(SummaryDocumentError::Text));
        invalid.spoken_text = "ok".into();
        invalid.covers_new_completions = vec![COMPLETION_A.into(), COMPLETION_A.into()];
        assert_eq!(
            invalid.canonical_json(),
            Err(SummaryDocumentError::Coverage)
        );

        assert_eq!(
            SummaryDocument::parse(&vec![b'x'; MAX_SUMMARY_DOCUMENT_BYTES + 1]),
            Err(SummaryDocumentError::Size)
        );
    }

    #[test]
    fn detects_han_text_without_treating_ascii_as_chinese() {
        assert!(contains_han_text("任务已经完成，准备播放。"));
        assert!(contains_han_text("ASR 与 TTS ready"));
        assert!(!contains_han_text(
            "The task is complete and ready to play."
        ));
    }

    #[test]
    fn meaningful_speech_requires_a_concrete_fact_and_substantive_narration() {
        let mut summary = document();
        assert!(meaningful_for_speech(&summary));
        summary.facts = vec!["完成".into()];
        assert!(!meaningful_for_speech(&summary));
        summary.facts = vec!["完成了本地语音信箱状态同步".into()];
        summary.spoken_text = "已完成。".into();
        assert!(!meaningful_for_speech(&summary));
    }

    #[test]
    fn source_evidence_is_audit_metadata_not_forced_spoken_text() {
        let mut summary = document();
        summary.source_evidence = vec![SummarySourceEvidence {
            completion_id: COMPLETION_A.into(),
            exact_quote: "authoritative final result".into(),
        }];
        summary.spoken_text = "这是 Spark 自然生成的中文摘要，不机械朗读来源摘录。".into();
        assert!(
            summary
                .validate_source_evidence(&[(
                    COMPLETION_A,
                    "The authoritative final result is available.",
                    "authoritative final result",
                )])
                .is_ok()
        );
    }

    #[test]
    fn cumulative_candidate_must_preserve_every_previous_section_and_spoken_text() {
        let previous = document();
        let mut candidate = previous.clone();
        candidate.facts.push("新完成事项已经通过真机验证".into());
        candidate.spoken_text = format!("{} 新完成事项已经通过真机验证。", previous.spoken_text);
        assert!(preserves_previous_unheard(Some(&previous), &candidate));

        candidate.pending.clear();
        assert!(!preserves_previous_unheard(Some(&previous), &candidate));
        candidate.pending = previous.pending.clone();
        candidate.spoken_text = "重新提炼旧信箱内容，不必逐字重复上一版播报。".into();
        assert!(preserves_previous_unheard(Some(&previous), &candidate));
        assert!(preserves_previous_unheard(None, &candidate));
    }

    #[test]
    fn cumulative_candidate_must_add_structured_and_spoken_content_for_new_completion() {
        let previous = document();
        assert!(!incorporates_new_completion(Some(&previous), &previous));

        let mut candidate = previous.clone();
        candidate.covers_new_completions = vec![COMPLETION_A.into()];
        candidate
            .facts
            .push("新固件已经完成真实键盘烧录验证".into());
        assert!(!incorporates_new_completion(Some(&previous), &candidate));
        candidate.spoken_text = format!(
            "{} 新固件已经完成真实键盘烧录验证，可以继续逐槽灯光测试。",
            previous.spoken_text
        );
        assert!(incorporates_new_completion(Some(&previous), &candidate));
        assert!(incorporates_new_completion(None, &candidate));
    }

    #[test]
    fn required_source_quote_prefers_the_most_substantive_final_sentence() {
        let final_reply =
            "任务已经顺利完成。固件已把四个槽位映射到左起四颗灯，并让第五颗灯保持熄灭。可以继续。";
        assert_eq!(
            required_source_evidence_quote(final_reply, 64).as_deref(),
            Some("固件已把四个槽位映射到左起四颗灯，并让第五颗灯保持熄灭")
        );
        assert_eq!(source_evidence_quote_budget(1), 64);
        assert_eq!(source_evidence_quote_budget(32), 4);
        assert_eq!(
            required_source_evidence_quote("✅", 8).as_deref(),
            Some("✅")
        );
    }

    #[test]
    fn audible_budget_rejects_unbounded_spoken_growth() {
        let mut summary = document();
        summary.facts = vec!["任务事实已经保留并且可以听见".into()];
        summary.spoken_text = "播报".repeat(MAX_AUDIBLE_SPOKEN_CHARACTERS / 2 + 1);
        assert!(!meaningful_for_speech(&summary));
    }
}

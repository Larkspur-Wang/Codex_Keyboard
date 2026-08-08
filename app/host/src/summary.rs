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
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SummaryDocument {
    pub schema: u8,
    pub facts: Vec<String>,
    pub pending: Vec<String>,
    pub decisions: Vec<String>,
    pub spoken_text: String,
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
        Ok(())
    }
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
}

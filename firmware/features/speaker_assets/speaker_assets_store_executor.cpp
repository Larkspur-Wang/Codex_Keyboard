#include "speaker_assets/speaker_assets_store_executor.h"

#include <algorithm>

namespace easy_input::speaker_assets {
namespace {

bool transaction_id_valid(
    const std::array<std::uint8_t, kSoundTransactionIdBytes>&
        transaction_id) {
  return std::any_of(
      transaction_id.begin(),
      transaction_id.end(),
      [](std::uint8_t value) { return value != 0U; });
}

bool identity_equal(const SoundUpdateIdentity& first,
                    const SoundUpdateIdentity& second) {
  return first.generation == second.generation &&
         first.target_bank == second.target_bank &&
         first.transaction_id == second.transaction_id;
}

SoundStoreResult read_expected_progress(
    SoundAssetStore& store,
    const SoundUpdateIdentity& expected,
    SoundUpdateProgress* progress) {
  const auto result = store.update_progress(progress);
  if (result != SoundStoreResult::Ok) {
    return result;
  }
  return identity_equal(progress->identity, expected)
             ? SoundStoreResult::Ok
             : SoundStoreResult::TransactionMismatch;
}

SoundStoreResult capture_progress(
    SoundAssetStore& store,
    SpeakerAssetsActionCompletion* completion) {
  SoundUpdateProgress progress{};
  const auto result = store.update_progress(&progress);
  if (result != SoundStoreResult::Ok) {
    return result;
  }
  completion->progress = progress;
  completion->progress_valid = true;
  return SoundStoreResult::Ok;
}

SoundStoreResult capture_transaction_outcome(
    SoundAssetStore& store,
    const std::array<std::uint8_t, kSoundTransactionIdBytes>&
        transaction_id,
    SpeakerAssetsActionCompletion* completion) {
  SoundTransactionOutcome outcome{};
  const auto query_result =
      store.query_transaction_outcome(transaction_id, &outcome);
  if (query_result != SoundStoreResult::Ok) {
    return query_result;
  }
  completion->identity = outcome.identity;
  completion->identity_valid = true;
  completion->outcome_manifest_bytes = outcome.manifest_bytes;
  completion->outcome_payload_bytes = outcome.payload_bytes;
  switch (outcome.state) {
    case SoundTransactionState::Active: {
      completion->outcome =
          SpeakerAssetsTransactionOutcome::Active;
      const auto progress_result =
          capture_progress(store, completion);
      if (progress_result != SoundStoreResult::Ok) {
        return progress_result;
      }
      if (!identity_equal(
              completion->identity,
              completion->progress.identity)) {
        completion->progress_valid = false;
        return SoundStoreResult::TransactionMismatch;
      }
      break;
    }
    case SoundTransactionState::Committed:
      completion->outcome =
          SpeakerAssetsTransactionOutcome::Committed;
      break;
    case SoundTransactionState::Unknown:
      completion->outcome =
          SpeakerAssetsTransactionOutcome::Unknown;
      break;
  }
  completion->outcome_valid = true;
  return SoundStoreResult::Ok;
}

}  // namespace

bool execute_speaker_assets_store_action(
    SoundAssetStore& store,
    const SpeakerAssetsActionView& action,
    SpeakerAssetsActionCompletion* completion) {
  if (completion == nullptr ||
      action.token == 0U ||
      action.request_id == 0U) {
    return false;
  }
  *completion = {};
  completion->token = action.token;
  completion->kind = action.kind;
  completion->result = SoundStoreResult::InvalidArgument;

  switch (action.kind) {
    case SpeakerAssetsActionKind::Begin: {
      if (action.plan == nullptr) {
        return true;
      }
      SoundUpdateIdentity identity{};
      completion->result =
          store.begin_or_resume_update(*action.plan, &identity);
      if (completion->result == SoundStoreResult::Ok) {
        completion->result =
            capture_progress(store, completion);
        if (completion->result == SoundStoreResult::Ok &&
            !identity_equal(
                identity, completion->progress.identity)) {
          completion->progress_valid = false;
          completion->result =
              SoundStoreResult::TransactionMismatch;
        } else if (
            completion->result == SoundStoreResult::Ok) {
          completion->identity = identity;
          completion->identity_valid = true;
        }
      }
      return true;
    }
    case SpeakerAssetsActionKind::DiscardInvalidStaging: {
      if (action.plan == nullptr) {
        return true;
      }
      completion->result =
          store.discard_invalid_staging(*action.plan);
      return true;
    }
    case SpeakerAssetsActionKind::QueryCurrentActive: {
      completion->result =
          store.query_current_active(&completion->current_active);
      if (completion->result == SoundStoreResult::Ok) {
        completion->current_active_valid = true;
      }
      return true;
    }
    case SpeakerAssetsActionKind::ResumeQuery: {
      if (action.query_mode ==
          SpeakerAssetsResumeQueryMode::CurrentProgress) {
        SoundUpdateProgress progress{};
        completion->result = read_expected_progress(
            store, action.expected_identity, &progress);
        if (completion->result == SoundStoreResult::Ok) {
          completion->progress = progress;
          completion->progress_valid = true;
          completion->identity = progress.identity;
          completion->identity_valid = true;
          completion->outcome =
              SpeakerAssetsTransactionOutcome::Active;
          completion->outcome_valid = true;
        }
        return true;
      }
      if (!transaction_id_valid(
              action.expected_identity.transaction_id)) {
        return true;
      }
      if (store.update_active()) {
        SoundUpdateProgress progress{};
        completion->result = store.update_progress(&progress);
        if (completion->result == SoundStoreResult::Ok &&
            progress.identity.transaction_id !=
                action.expected_identity.transaction_id) {
          completion->result = capture_transaction_outcome(
              store,
              action.expected_identity.transaction_id,
              completion);
          return true;
        }
        if (completion->result == SoundStoreResult::Ok) {
          completion->progress = progress;
          completion->progress_valid = true;
          completion->identity = progress.identity;
          completion->identity_valid = true;
          completion->outcome =
              SpeakerAssetsTransactionOutcome::Active;
          completion->outcome_valid = true;
        }
        return true;
      }

      SoundUpdateIdentity resumed{};
      completion->result = store.resume_update(
          action.expected_identity.transaction_id, &resumed);
      if (completion->result ==
          SoundStoreResult::TransactionMismatch) {
        completion->result = capture_transaction_outcome(
            store,
            action.expected_identity.transaction_id,
            completion);
        return true;
      }
      if (completion->result != SoundStoreResult::Ok) {
        return true;
      }
      completion->result = capture_progress(store, completion);
      if (completion->result == SoundStoreResult::Ok &&
          !identity_equal(completion->progress.identity, resumed)) {
        completion->progress_valid = false;
        completion->result =
            SoundStoreResult::TransactionMismatch;
      } else if (
          completion->result == SoundStoreResult::Ok) {
        completion->identity = resumed;
        completion->identity_valid = true;
        completion->outcome =
            SpeakerAssetsTransactionOutcome::Active;
        completion->outcome_valid = true;
      }
      return true;
    }
    case SpeakerAssetsActionKind::WriteManifest:
    case SpeakerAssetsActionKind::WritePayloadBlock: {
      SoundUpdateProgress before{};
      completion->result = read_expected_progress(
          store, action.expected_identity, &before);
      if (completion->result != SoundStoreResult::Ok) {
        return true;
      }
      if (action.bytes == nullptr || action.length == 0U) {
        completion->result = SoundStoreResult::InvalidArgument;
        return true;
      }
      completion->result =
          action.kind == SpeakerAssetsActionKind::WriteManifest
              ? store.write_manifest(action.bytes, action.length)
              : store.write_payload_block(
                    action.block_index,
                    action.bytes,
                    action.length);
      if (completion->result == SoundStoreResult::Ok) {
        completion->result =
            capture_progress(store, completion);
      }
      return true;
    }
    case SpeakerAssetsActionKind::Commit:
    case SpeakerAssetsActionKind::Abort: {
      SoundUpdateProgress before{};
      completion->result = read_expected_progress(
          store, action.expected_identity, &before);
      if (completion->result != SoundStoreResult::Ok) {
        return true;
      }
      completion->result =
          action.kind == SpeakerAssetsActionKind::Commit
              ? store.commit_update()
              : store.abort_update();
      return true;
    }
  }
  return true;
}

}  // namespace easy_input::speaker_assets

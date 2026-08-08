#pragma once

#include "speaker_assets/speaker_assets_session.h"

namespace easy_input::speaker_assets {

// Executes one borrowed session action on the dedicated Store owner worker.
// This primitive is intentionally synchronous because SoundAssetStore is
// synchronous; callers must never invoke it from TinyUSB/NimBLE callbacks or
// the Runtime/supervisor owner. SpeakerAssetsCooperativeStoreRunner keeps this
// call on its worker stack while permitting each bounded Flash/hash unit.
bool execute_speaker_assets_store_action(
    SoundAssetStore& store,
    const SpeakerAssetsActionView& action,
    SpeakerAssetsActionCompletion* completion);

}  // namespace easy_input::speaker_assets

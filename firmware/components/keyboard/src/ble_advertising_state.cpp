#include "keyboard/ble_advertising_state.h"

namespace ai_keyboard {

BleAdvertisingState::HostObservation BleAdvertisingState::observe_host(
    std::uint32_t generation,
    bool synced) {
  HostObservation observation;
  observation.generation_initialized =
      generation != 0 && !host_generation_observed_;
  observation.generation_changed =
      generation != 0 && host_generation_observed_ &&
      generation != host_generation_;
  observation.sync_changed = synced != host_synced_;

  if (observation.generation_changed || !synced) {
    current_mode_ = BleAdvertisingMode::Stopped;
    started_host_generation_ = 0;
  }
  if (generation != 0) {
    host_generation_observed_ = true;
  }
  host_generation_ = generation;
  host_synced_ = synced;
  return observation;
}

BleAdvertisingState::Action BleAdvertisingState::next_action(
    bool advertising_active,
    BleAdvertisingMode desired,
    bool force_restart) {
  if (!host_synced_ || host_generation_ == 0) {
    return Action::None;
  }

  if (!advertising_active) {
    note_stopped();
    return desired == BleAdvertisingMode::Stopped ? Action::None
                                                  : Action::Start;
  }

  if (force_restart || current_mode_ != desired ||
      started_host_generation_ != host_generation_) {
    return Action::Stop;
  }
  return Action::None;
}

void BleAdvertisingState::note_started(BleAdvertisingMode mode) {
  current_mode_ = mode;
  started_host_generation_ = host_generation_;
}

void BleAdvertisingState::note_stopped() {
  current_mode_ = BleAdvertisingMode::Stopped;
  started_host_generation_ = 0;
}

BleAdvertisingMode BleAdvertisingState::current_mode() const {
  return current_mode_;
}

std::uint32_t BleAdvertisingState::host_generation() const {
  return host_generation_;
}

bool BleAdvertisingState::host_synced() const {
  return host_synced_;
}

}  // namespace ai_keyboard

#pragma once

#include <cstdint>

namespace ai_keyboard {

// Logical advertising profiles. The platform layer owns the NimBLE field and
// GAP calls; this type only records which profile was actually started for one
// concrete NimBLE host lifetime.
enum class BleAdvertisingMode : std::uint8_t {
  Stopped,
  Directed,
  HidFast,
  HidSlow,
  HidConfig,
  ControlSlow,
  ControlConfig,
};

class BleAdvertisingState {
 public:
  struct HostObservation {
    // The first non-zero host generation establishes the observation
    // baseline. It is not a reset and must not invalidate GAP resources that
    // may already have been opened before the application task first polls.
    bool generation_initialized = false;
    bool generation_changed = false;
    bool sync_changed = false;
  };

  enum class Action : std::uint8_t {
    None,
    Stop,
    Start,
  };

  // Observe the adapter level rather than a best-effort START event. A reset
  // generation invalidates every cached advertising profile even if NimBLE
  // resynchronizes before the application task gets CPU time.
  HostObservation observe_host(std::uint32_t generation, bool synced);

  // Reconcile the desired profile with both NimBLE's active level and the
  // profile that this application actually started for the current host
  // generation. An active but unknown/wrong profile must be stopped first.
  Action next_action(bool advertising_active,
                     BleAdvertisingMode desired,
                     bool force_restart = false);

  void note_started(BleAdvertisingMode mode);
  void note_stopped();

  BleAdvertisingMode current_mode() const;
  std::uint32_t host_generation() const;
  bool host_synced() const;

 private:
  std::uint32_t host_generation_ = 0;
  std::uint32_t started_host_generation_ = 0;
  bool host_generation_observed_ = false;
  bool host_synced_ = false;
  BleAdvertisingMode current_mode_ = BleAdvertisingMode::Stopped;
};

}  // namespace ai_keyboard

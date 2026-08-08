#include "speaker_assets/speaker_assets_runtime.h"

#include <algorithm>
#include <limits>

namespace easy_input::speaker_assets {
namespace {

bool route_shape_is_valid(const SpeakerAssetsRouteToken& route) {
  return route.generation != 0U &&
         (route.transport == SpeakerAssetsTransport::Wifi ||
          (route.transport == SpeakerAssetsTransport::Usb &&
           route.route_id == 0U));
}

bool elapsed_at_least(std::uint32_t now,
                      std::uint32_t then,
                      std::uint32_t interval) {
  return static_cast<std::uint32_t>(now - then) >= interval;
}

bool is_known_mutating_or_query_opcode(std::uint8_t opcode) {
  return opcode >= static_cast<std::uint8_t>(
                       SpeakerAssetsOpcode::Begin) &&
         opcode <= static_cast<std::uint8_t>(
                       SpeakerAssetsOpcode::CurrentActive);
}

std::uint32_t mix_route_hash(std::uint32_t value) {
  value ^= value >> 16U;
  value *= 0x7FEB352DU;
  value ^= value >> 15U;
  value *= 0x846CA68BU;
  value ^= value >> 16U;
  return value;
}

std::uint32_t route_hash(const SpeakerAssetsRouteToken& route,
                         std::uint32_t salt) {
  // Keep every field in a separate non-commutative mixing round. XORing the
  // transport and route ID before the same mix creates deterministic aliases
  // between distinct physical routes.
  auto value = mix_route_hash(salt ^ 0x9E3779B9U);
  value = mix_route_hash(
      value ^
      (static_cast<std::uint32_t>(route.transport) *
       0x85EBCA6BU));
  value = mix_route_hash(value ^ route.route_id);
  return mix_route_hash(value ^ route.generation);
}

std::size_t route_filter_bit(const SpeakerAssetsRouteToken& route,
                             std::uint32_t salt) {
  constexpr auto bit_count =
      kSpeakerAssetsRuntimeClosedRouteFilterWords * 64U;
  return static_cast<std::size_t>(
      route_hash(route, salt) %
      static_cast<std::uint32_t>(bit_count));
}

}  // namespace

bool speaker_assets_logical_request_lease_equal(
    const SpeakerAssetsLogicalRequestLease& first,
    const SpeakerAssetsLogicalRequestLease& second) {
  return first.admission_id != 0U &&
         first.admission_id == second.admission_id &&
         speaker_assets_route_equal(first.route, second.route);
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeMailbox::enqueue_route_opened(
    const SpeakerAssetsRouteToken& exact_route) {
  if (!route_shape_is_valid(exact_route)) {
    return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
  }
  if (resource_sync_fail_closed_ ||
      close_pending(exact_route)) {
    return SpeakerAssetsRuntimeEnqueueResult::Coalesced;
  }
  if (revoke_all_pending_) {
    // All lifetimes observed before the global sentinel is consumed are
    // conservatively tombstoned. They must reconnect with a fresh generation.
    return retain_fail_closed_tombstone(exact_route);
  }
  return enqueue_data(
      SpeakerAssetsRuntimeMailboxKind::RouteOpened,
      exact_route,
      nullptr,
      0U,
      0U);
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeMailbox::enqueue_route_closed(
    const SpeakerAssetsRouteToken& exact_route) {
  if (!route_shape_is_valid(exact_route)) {
    return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
  }
  purge_pending_route(exact_route);
  revoke_logical_request_for_route(exact_route);
  if (resource_sync_fail_closed_) {
    return SpeakerAssetsRuntimeEnqueueResult::Coalesced;
  }
  if (revoke_all_pending_) {
    return retain_fail_closed_tombstone(exact_route);
  }
  if (close_pending(exact_route)) {
    return SpeakerAssetsRuntimeEnqueueResult::Coalesced;
  }
  if (close_size_ >= closes_.size()) {
    // A disconnect callback is normally one-shot. Never ask the caller to
    // retain/retry it: retain a sticky high-priority fail-closed event instead.
    revoke_all_pending_ = true;
    revoke_all_overflow_route_ = exact_route;
    fail_close_pending_data();
    clear_logical_request_reservation();
    return SpeakerAssetsRuntimeEnqueueResult::Accepted;
  }
  const auto tail =
      (close_head_ + close_size_) % closes_.size();
  closes_[tail] = exact_route;
  ++close_size_;
  return SpeakerAssetsRuntimeEnqueueResult::Accepted;
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeMailbox::enqueue_usb_frame(
    const SpeakerAssetsRouteToken& exact_route,
    const std::uint8_t* bytes,
    std::size_t length,
    std::uint32_t received_ms) {
  if (exact_route.transport != SpeakerAssetsTransport::Usb ||
      length != kSpeakerAssetsUsbFrameBytes) {
    return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
  }
  if (revoke_all_pending_ || resource_sync_fail_closed_ ||
      close_pending(exact_route)) {
    return SpeakerAssetsRuntimeEnqueueResult::Full;
  }
  bool newly_reserved = false;
  const auto reservation =
      reserve_logical_frame_request(
          exact_route, &newly_reserved);
  if (reservation != SpeakerAssetsRuntimeEnqueueResult::Accepted) {
    return reservation;
  }
  const auto queued = enqueue_data(
      SpeakerAssetsRuntimeMailboxKind::UsbFrame,
      exact_route,
      bytes,
      length,
      received_ms);
  if (queued == SpeakerAssetsRuntimeEnqueueResult::Accepted) {
    logical_frame_request_queued_ = true;
  } else if (newly_reserved) {
    clear_logical_request_reservation();
  }
  return queued;
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeMailbox::enqueue_wifi_frame(
    const SpeakerAssetsRouteToken& exact_route,
    const std::uint8_t* bytes,
    std::size_t length,
    std::uint32_t received_ms) {
  if (exact_route.transport != SpeakerAssetsTransport::Wifi ||
      length < kSpeakerAssetsFrameHeaderBytes ||
      length > kSpeakerAssetsWifiFrameMaxBytes) {
    return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
  }
  if (revoke_all_pending_ || resource_sync_fail_closed_ ||
      close_pending(exact_route)) {
    return SpeakerAssetsRuntimeEnqueueResult::Full;
  }
  bool newly_reserved = false;
  const auto reservation =
      reserve_logical_frame_request(
          exact_route, &newly_reserved);
  if (reservation !=
      SpeakerAssetsRuntimeEnqueueResult::Accepted) {
    return reservation;
  }
  const auto queued = enqueue_data(
      SpeakerAssetsRuntimeMailboxKind::WifiFrame,
      exact_route,
      bytes,
      length,
      received_ms);
  if (queued == SpeakerAssetsRuntimeEnqueueResult::Accepted) {
    logical_frame_request_queued_ = true;
  } else if (newly_reserved) {
    clear_logical_request_reservation();
  }
  return queued;
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeMailbox::enqueue_data(
    SpeakerAssetsRuntimeMailboxKind kind,
    const SpeakerAssetsRouteToken& exact_route,
    const std::uint8_t* bytes,
    std::size_t length,
    std::uint32_t received_ms) {
  const auto lifecycle_open =
      kind == SpeakerAssetsRuntimeMailboxKind::RouteOpened;
  if (!route_shape_is_valid(exact_route) ||
      (!lifecycle_open &&
       (bytes == nullptr ||
        length == 0U ||
        length > kSpeakerAssetsWifiFrameMaxBytes)) ||
      (lifecycle_open && (bytes != nullptr || length != 0U))) {
    return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
  }
  if (data_size_ >= data_.size()) {
    return SpeakerAssetsRuntimeEnqueueResult::Full;
  }
  const auto tail =
      (data_head_ + data_size_) % data_.size();
  auto& record = data_[tail];
  record = {};
  record.kind = kind;
  record.route = exact_route;
  record.admission_id =
      lifecycle_open ? 0U : reserved_admission_id_;
  record.received_ms = received_ms;
  record.length = static_cast<std::uint16_t>(length);
  if (length != 0U) {
    std::copy_n(bytes, length, record.bytes.begin());
  }
  ++data_size_;
  return SpeakerAssetsRuntimeEnqueueResult::Accepted;
}

bool SpeakerAssetsRuntimeMailbox::take_next(
    SpeakerAssetsRuntimeMailboxRecord* record) {
  if (record == nullptr) {
    return false;
  }
  if (revoke_all_pending_) {
    *record = {};
    record->kind =
        SpeakerAssetsRuntimeMailboxKind::AllRoutesClosed;
    record->route = revoke_all_overflow_route_;
    revoke_all_pending_ = false;
    revoke_all_overflow_route_ = {};
    return true;
  }
  if (close_size_ != 0U) {
    *record = {};
    record->kind =
        SpeakerAssetsRuntimeMailboxKind::RouteClosed;
    record->route = closes_[close_head_];
    closes_[close_head_] = {};
    close_head_ = (close_head_ + 1U) % closes_.size();
    --close_size_;
    return true;
  }
  while (data_size_ != 0U && data_[data_head_].cancelled) {
    data_[data_head_] = {};
    data_head_ = (data_head_ + 1U) % data_.size();
    --data_size_;
  }
  if (data_size_ == 0U) {
    return false;
  }
  *record = data_[data_head_];
  data_[data_head_] = {};
  data_head_ = (data_head_ + 1U) % data_.size();
  --data_size_;
  return true;
}

void SpeakerAssetsRuntimeMailbox::clear() {
  data_ = {};
  data_head_ = 0U;
  data_size_ = 0U;
  closes_ = {};
  close_head_ = 0U;
  close_size_ = 0U;
  clear_logical_request_reservation();
  revoke_all_pending_ = false;
  resource_sync_fail_closed_ = false;
  revoke_all_overflow_route_ = {};
}

bool SpeakerAssetsRuntimeMailbox::release_logical_request(
    const SpeakerAssetsLogicalRequestLease& lease) {
  if (!logical_request_reserved_ ||
      lease.admission_id == 0U ||
      lease.admission_id != reserved_admission_id_ ||
      !speaker_assets_route_equal(
          reserved_route_, lease.route)) {
    return false;
  }
  purge_pending_admission(lease.admission_id);
  clear_logical_request_reservation();
  return true;
}

bool SpeakerAssetsRuntimeMailbox::logical_request_lease(
    SpeakerAssetsLogicalRequestLease* lease) const {
  if (lease == nullptr) {
    return false;
  }
  *lease = {};
  if (!logical_request_reserved_) {
    return false;
  }
  lease->route = reserved_route_;
  lease->admission_id = reserved_admission_id_;
  return true;
}

bool SpeakerAssetsRuntimeMailbox::logical_request_reserved_for(
    const SpeakerAssetsRouteToken& exact_route) const {
  return logical_request_reserved_ &&
         speaker_assets_route_equal(
             reserved_route_, exact_route);
}

std::size_t SpeakerAssetsRuntimeMailbox::data_size() const {
  return data_size_;
}

std::size_t SpeakerAssetsRuntimeMailbox::close_size() const {
  return close_size_ + (revoke_all_pending_ ? 1U : 0U);
}

std::size_t SpeakerAssetsRuntimeMailbox::raw_data_credit() const {
  return data_.size() - data_size_;
}

bool SpeakerAssetsRuntimeMailbox::close_pending(
    const SpeakerAssetsRouteToken& exact_route) const {
  if (revoke_all_pending_ &&
      speaker_assets_route_equal(
          revoke_all_overflow_route_, exact_route)) {
    return true;
  }
  for (std::size_t offset = 0U; offset < close_size_; ++offset) {
    const auto index =
        (close_head_ + offset) % closes_.size();
    if (speaker_assets_route_equal(
            closes_[index], exact_route)) {
      return true;
    }
  }
  for (std::size_t offset = 0U; offset < data_size_; ++offset) {
    const auto index =
        (data_head_ + offset) % data_.size();
    if (data_[index].kind ==
            SpeakerAssetsRuntimeMailboxKind::RouteClosed &&
        speaker_assets_route_equal(
            data_[index].route, exact_route)) {
      return true;
    }
  }
  return false;
}

void SpeakerAssetsRuntimeMailbox::purge_pending_route(
    const SpeakerAssetsRouteToken& exact_route) {
  const auto original_size = data_size_;
  for (std::size_t index = 0U; index < original_size; ++index) {
    const auto record = data_[data_head_];
    data_[data_head_] = {};
    data_head_ = (data_head_ + 1U) % data_.size();
    --data_size_;
    if (!speaker_assets_route_equal(
            record.route, exact_route)) {
      const auto tail =
          (data_head_ + data_size_) % data_.size();
      data_[tail] = record;
      ++data_size_;
    }
  }
}

void SpeakerAssetsRuntimeMailbox::purge_pending_admission(
    std::uint64_t admission_id) {
  if (admission_id == 0U) {
    return;
  }
  const auto original_size = data_size_;
  for (std::size_t index = 0U; index < original_size; ++index) {
    const auto record = data_[data_head_];
    data_[data_head_] = {};
    data_head_ = (data_head_ + 1U) % data_.size();
    --data_size_;
    if (record.admission_id != admission_id) {
      const auto tail =
          (data_head_ + data_size_) % data_.size();
      data_[tail] = record;
      ++data_size_;
    }
  }
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeMailbox::retain_fail_closed_tombstone(
    const SpeakerAssetsRouteToken& exact_route) {
  if (close_pending(exact_route)) {
    return SpeakerAssetsRuntimeEnqueueResult::Coalesced;
  }
  if (data_size_ >= data_.size()) {
    // Finite callbacks cannot retain an unbounded disconnect storm. Keep the
    // resource channel shut until reset rather than dropping one exact close.
    resource_sync_fail_closed_ = true;
    return SpeakerAssetsRuntimeEnqueueResult::Accepted;
  }
  const auto tail =
      (data_head_ + data_size_) % data_.size();
  auto& record = data_[tail];
  record = {};
  record.kind = SpeakerAssetsRuntimeMailboxKind::RouteClosed;
  record.route = exact_route;
  ++data_size_;
  return SpeakerAssetsRuntimeEnqueueResult::Accepted;
}

void SpeakerAssetsRuntimeMailbox::fail_close_pending_data() {
  const auto original_size = data_size_;
  for (std::size_t index = 0U; index < original_size; ++index) {
    auto record = data_[data_head_];
    data_[data_head_] = {};
    data_head_ = (data_head_ + 1U) % data_.size();
    --data_size_;
    if (record.kind ==
        SpeakerAssetsRuntimeMailboxKind::RouteOpened) {
      // Preserve an exact tombstone after the global fallback. Keeping it in
      // FIFO position ensures a delayed duplicate Open behind it reaches the
      // Core only after this Close has populated the lifetime filter.
      record.kind =
          SpeakerAssetsRuntimeMailboxKind::RouteClosed;
      record.received_ms = 0U;
      record.length = 0U;
      record.bytes = {};
      record.cancelled = false;
    } else if (record.kind !=
               SpeakerAssetsRuntimeMailboxKind::RouteClosed) {
      // All payload is stale after AllRoutesClosed and must not consume the
      // physical reservation required by the next fresh lifetime.
      continue;
    }
    const auto tail =
        (data_head_ + data_size_) % data_.size();
    data_[tail] = record;
    ++data_size_;
  }
}

void SpeakerAssetsRuntimeMailbox::
clear_logical_request_reservation() {
  reserved_route_ = {};
  reserved_admission_id_ = 0U;
  logical_request_reserved_ = false;
  logical_frame_request_queued_ = false;
}

void SpeakerAssetsRuntimeMailbox::revoke_logical_request_for_route(
    const SpeakerAssetsRouteToken& exact_route) {
  if (logical_request_reserved_ &&
      speaker_assets_route_equal(
          reserved_route_, exact_route)) {
    purge_pending_admission(reserved_admission_id_);
    clear_logical_request_reservation();
  }
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeMailbox::reserve_logical_frame_request(
    const SpeakerAssetsRouteToken& exact_route,
    bool* newly_reserved) {
  if (newly_reserved == nullptr) {
    return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
  }
  *newly_reserved = false;
  if (!logical_request_reserved_) {
    if (next_admission_id_ == 0U) {
      resource_sync_fail_closed_ = true;
      return SpeakerAssetsRuntimeEnqueueResult::Full;
    }
    reserved_route_ = exact_route;
    reserved_admission_id_ = next_admission_id_;
    next_admission_id_ =
        next_admission_id_ ==
                std::numeric_limits<std::uint64_t>::max()
            ? 0U
            : next_admission_id_ + 1U;
    logical_request_reserved_ = true;
    logical_frame_request_queued_ = false;
    *newly_reserved = true;
    return SpeakerAssetsRuntimeEnqueueResult::Accepted;
  }
  if (!speaker_assets_route_equal(
          reserved_route_, exact_route) ||
      logical_frame_request_queued_) {
    return SpeakerAssetsRuntimeEnqueueResult::Full;
  }
  return SpeakerAssetsRuntimeEnqueueResult::Accepted;
}

SpeakerAssetsRuntimeCore::SpeakerAssetsRuntimeCore(
    std::uint32_t cookie_seed,
    std::uint32_t action_token_seed,
    std::uint32_t partial_timeout_ms,
    std::uint32_t session_lease_ms)
    : session_(cookie_seed, action_token_seed),
      partial_timeout_ms_(
          partial_timeout_ms == 0U
              ? kSpeakerAssetsRuntimePartialTimeoutMs
              : partial_timeout_ms),
      session_lease_ms_(
          session_lease_ms == 0U
              ? kSpeakerAssetsRuntimeSessionLeaseMs
              : session_lease_ms) {}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeCore::enqueue_route_opened(
    const SpeakerAssetsRouteToken& exact_route) {
  return enqueue_lifecycle(LifecycleKind::Opened, exact_route);
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeCore::enqueue_route_closed(
    const SpeakerAssetsRouteToken& exact_route) {
  return enqueue_lifecycle(LifecycleKind::Closed, exact_route);
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeCore::enqueue_usb_frame(
    const SpeakerAssetsRouteToken& exact_route,
    const std::uint8_t* bytes,
    std::size_t length,
    std::uint32_t received_ms) {
  if (exact_route.transport != SpeakerAssetsTransport::Usb ||
      length != kSpeakerAssetsUsbFrameBytes) {
    return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
  }
  return enqueue_ingress(
      IngressKind::UsbFrame,
      exact_route,
      bytes,
      length,
      received_ms,
      0U);
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeCore::enqueue_wifi_frame(
    const SpeakerAssetsRouteToken& exact_route,
    const std::uint8_t* bytes,
    std::size_t length,
    std::uint32_t received_ms) {
  if (exact_route.transport != SpeakerAssetsTransport::Wifi ||
      length < kSpeakerAssetsFrameHeaderBytes ||
      length > kSpeakerAssetsWifiFrameMaxBytes) {
    return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
  }
  return enqueue_ingress(
      IngressKind::WifiFrame,
      exact_route,
      bytes,
      length,
      received_ms,
      0U);
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeCore::import_mailbox_record(
    const SpeakerAssetsRuntimeMailboxRecord& record) {
  if (record.cancelled) {
    return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
  }
  switch (record.kind) {
    case SpeakerAssetsRuntimeMailboxKind::RouteOpened:
      return enqueue_route_opened(record.route);
    case SpeakerAssetsRuntimeMailboxKind::RouteClosed:
      return enqueue_route_closed(record.route);
    case SpeakerAssetsRuntimeMailboxKind::AllRoutesClosed:
      revoke_all_routes(record.route);
      return SpeakerAssetsRuntimeEnqueueResult::Accepted;
    case SpeakerAssetsRuntimeMailboxKind::UsbFrame:
      if (record.admission_id == 0U ||
          record.route.transport !=
              SpeakerAssetsTransport::Usb ||
          record.length != kSpeakerAssetsUsbFrameBytes) {
        return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
      }
      return enqueue_ingress(
          IngressKind::UsbFrame,
          record.route,
          record.bytes.data(),
          record.length,
          record.received_ms,
          record.admission_id);
    case SpeakerAssetsRuntimeMailboxKind::WifiFrame:
      if (record.admission_id == 0U ||
          record.route.transport !=
              SpeakerAssetsTransport::Wifi ||
          record.length < kSpeakerAssetsFrameHeaderBytes ||
          record.length > kSpeakerAssetsWifiFrameMaxBytes) {
        return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
      }
      return enqueue_ingress(
          IngressKind::WifiFrame,
          record.route,
          record.bytes.data(),
          record.length,
          record.received_ms,
          record.admission_id);
  }
  return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeCore::enqueue_lifecycle(
    LifecycleKind kind,
    const SpeakerAssetsRouteToken& exact_route) {
  if (!route_shape_is_valid(exact_route)) {
    return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
  }
  for (std::size_t offset = 0U;
       offset < lifecycle_size_;
       ++offset) {
    const auto index =
        (lifecycle_head_ + offset) % lifecycle_.size();
    if (!speaker_assets_route_equal(
            lifecycle_[index].route, exact_route)) {
      continue;
    }
    // Close is terminal for one exact lifetime and dominates a delayed or
    // duplicated Open in either enqueue order. Folding opposite events also
    // preserves the reserved close capacity for already-active routes.
    if (lifecycle_[index].kind == LifecycleKind::Closed) {
      return SpeakerAssetsRuntimeEnqueueResult::Coalesced;
    }
    if (kind == LifecycleKind::Closed) {
      lifecycle_[index].kind = LifecycleKind::Closed;
      return SpeakerAssetsRuntimeEnqueueResult::Accepted;
    }
    return SpeakerAssetsRuntimeEnqueueResult::Coalesced;
  }
  if (lifecycle_size_ >= lifecycle_.size()) {
    if (kind == LifecycleKind::Closed) {
      for (std::size_t offset = 0U;
           offset < lifecycle_size_;
           ++offset) {
        const auto index =
            (lifecycle_head_ + offset) % lifecycle_.size();
        if (lifecycle_[index].kind == LifecycleKind::Opened &&
            speaker_assets_route_equal(
                lifecycle_[index].route, exact_route)) {
          lifecycle_[index].kind = LifecycleKind::Closed;
          return SpeakerAssetsRuntimeEnqueueResult::Accepted;
        }
      }
    }
    return SpeakerAssetsRuntimeEnqueueResult::Full;
  }
  // Keep enough lifecycle slots reserved for one close notification per
  // possible live route. Close is fail-closed control traffic; a burst of
  // opens must never consume all of its mailbox capacity.
  if (kind == LifecycleKind::Opened &&
      lifecycle_size_ >=
          lifecycle_.size() - active_routes_.size()) {
    return SpeakerAssetsRuntimeEnqueueResult::Full;
  }
  const auto tail =
      (lifecycle_head_ + lifecycle_size_) % lifecycle_.size();
  lifecycle_[tail] = {kind, exact_route};
  ++lifecycle_size_;
  return SpeakerAssetsRuntimeEnqueueResult::Accepted;
}

SpeakerAssetsRuntimeEnqueueResult
SpeakerAssetsRuntimeCore::enqueue_ingress(
    IngressKind kind,
    const SpeakerAssetsRouteToken& exact_route,
    const std::uint8_t* bytes,
    std::size_t length,
    std::uint32_t received_ms,
    std::uint64_t admission_id) {
  if (!route_shape_is_valid(exact_route) ||
      bytes == nullptr ||
      length == 0U ||
      length > kSpeakerAssetsWifiFrameMaxBytes) {
    return SpeakerAssetsRuntimeEnqueueResult::InvalidArgument;
  }
  if (ingress_size_ >= ingress_.size()) {
    return SpeakerAssetsRuntimeEnqueueResult::Full;
  }
  const auto tail =
      (ingress_head_ + ingress_size_) % ingress_.size();
  auto& record = ingress_[tail];
  record = {};
  record.kind = kind;
  record.route = exact_route;
  record.admission_id = admission_id;
  record.received_ms = received_ms;
  record.length = static_cast<std::uint16_t>(length);
  std::copy_n(bytes, length, record.bytes.begin());
  ++ingress_size_;
  return SpeakerAssetsRuntimeEnqueueResult::Accepted;
}

SpeakerAssetsRuntimeCore::StepOutcome
SpeakerAssetsRuntimeCore::step(
    std::uint32_t now_ms,
    bool resource_steps_allowed,
    SpeakerAssetsRuntimeActionExecutor* executor) {
  release_now_ = {};
  StepOutcome outcome{};
  outcome.result =
      step_once(now_ms, resource_steps_allowed, executor);
  outcome.release_now = release_now_;
  return outcome;
}

SpeakerAssetsRuntimeStepResult
SpeakerAssetsRuntimeCore::step_once(
    std::uint32_t now_ms,
    bool resource_steps_allowed,
    SpeakerAssetsRuntimeActionExecutor* executor) {
  if (lifecycle_size_ != 0U) {
    return process_lifecycle();
  }
  const auto timeout_result = expire_partials(now_ms);
  if (timeout_result ==
      SpeakerAssetsRuntimeStepResult::PartialExpired) {
    return timeout_result;
  }
  if (pending_action_active_) {
    return process_pending_action(
        now_ms, resource_steps_allowed, executor);
  }
  const auto session_lease_result =
      expire_session_lease(now_ms);
  if (session_lease_result ==
      SpeakerAssetsRuntimeStepResult::SessionLeaseExpired) {
    return session_lease_result;
  }
  if (reply_size_ >= replies_.size()) {
    return SpeakerAssetsRuntimeStepResult::ReplyBackpressure;
  }
  if (ingress_size_ != 0U) {
    const auto record = ingress_[ingress_head_];
    ingress_[ingress_head_] = {};
    ingress_head_ = (ingress_head_ + 1U) % ingress_.size();
    --ingress_size_;
    const auto result = process_ingress(
        record, resource_steps_allowed, now_ms);
    const auto cannot_advance_session_partial =
        result ==
            SpeakerAssetsRuntimeStepResult::InvalidFrameDropped ||
        result ==
            SpeakerAssetsRuntimeStepResult::StaleRouteDropped ||
        result == SpeakerAssetsRuntimeStepResult::SessionRejected;
    if (cannot_advance_session_partial &&
        expire_session_partial_after_logical_frame(now_ms)) {
      return SpeakerAssetsRuntimeStepResult::PartialExpired;
    }
    return result;
  }
  return SpeakerAssetsRuntimeStepResult::Idle;
}

bool SpeakerAssetsRuntimeCore::front_reply(
    SpeakerAssetsRuntimeReply* reply) const {
  if (reply == nullptr || reply_size_ == 0U) {
    return false;
  }
  *reply = replies_[reply_head_];
  return true;
}

bool SpeakerAssetsRuntimeCore::front_reply_for_route(
    const SpeakerAssetsRouteToken& exact_route,
    SpeakerAssetsRuntimeReply* reply) const {
  if (reply == nullptr || !route_shape_is_valid(exact_route)) {
    return false;
  }
  for (std::size_t offset = 0U; offset < reply_size_; ++offset) {
    const auto index =
        (reply_head_ + offset) % replies_.size();
    if (speaker_assets_route_equal(
            replies_[index].route, exact_route)) {
      *reply = replies_[index];
      return true;
    }
  }
  return false;
}

bool SpeakerAssetsRuntimeCore::pop_reply_if_sequence(
    std::uint32_t sequence) {
  if (reply_size_ == 0U ||
      replies_[reply_head_].sequence != sequence) {
    return false;
  }
  replies_[reply_head_] = {};
  reply_head_ = (reply_head_ + 1U) % replies_.size();
  --reply_size_;
  return true;
}

bool SpeakerAssetsRuntimeCore::remove_reply_if_sequence(
    std::uint32_t sequence) {
  if (sequence == 0U || reply_size_ == 0U) {
    return false;
  }
  bool removed = false;
  const auto original_size = reply_size_;
  for (std::size_t index = 0U; index < original_size; ++index) {
    const auto reply = replies_[reply_head_];
    replies_[reply_head_] = {};
    reply_head_ = (reply_head_ + 1U) % replies_.size();
    --reply_size_;
    if (!removed && reply.sequence == sequence) {
      removed = true;
      continue;
    }
    const auto tail =
        (reply_head_ + reply_size_) % replies_.size();
    replies_[tail] = reply;
    ++reply_size_;
  }
  return removed;
}

std::size_t SpeakerAssetsRuntimeCore::ingress_size() const {
  return ingress_size_;
}

std::size_t SpeakerAssetsRuntimeCore::lifecycle_size() const {
  return lifecycle_size_;
}

std::size_t SpeakerAssetsRuntimeCore::reply_size() const {
  return reply_size_;
}

std::size_t SpeakerAssetsRuntimeCore::active_route_count() const {
  return static_cast<std::size_t>(std::count_if(
      active_routes_.begin(),
      active_routes_.end(),
      [](const ActiveRoute& route) { return route.active; }));
}

std::size_t SpeakerAssetsRuntimeCore::raw_ingress_credit() const {
  return ingress_.size() - ingress_size_;
}

std::size_t SpeakerAssetsRuntimeCore::advertised_ingress_credit(
    const SpeakerAssetsRouteToken& exact_route,
    bool resource_steps_allowed) const {
  if (!route_shape_is_valid(exact_route) ||
      !route_is_active(exact_route) ||
      !resource_steps_allowed ||
      pending_action_active_ ||
      lifecycle_size_ != 0U ||
      reply_size_ >= replies_.size() ||
      route_has_ingress(exact_route) ||
      route_has_reply(exact_route)) {
    return 0U;
  }
  return raw_ingress_credit() == 0U ? 0U : 1U;
}

bool SpeakerAssetsRuntimeCore::action_pending() const {
  return pending_action_active_;
}

SpeakerAssetsSessionPhase
SpeakerAssetsRuntimeCore::session_phase() const {
  return session_.phase();
}

bool SpeakerAssetsRuntimeCore::route_is_active(
    const SpeakerAssetsRouteToken& exact_route) const {
  return std::any_of(
      active_routes_.begin(),
      active_routes_.end(),
      [&exact_route](const ActiveRoute& route) {
        return route.active &&
               speaker_assets_route_equal(
                   route.route, exact_route);
      });
}

bool SpeakerAssetsRuntimeCore::activate_route(
    const SpeakerAssetsRouteToken& exact_route) {
  if (route_generation_is_closed(exact_route)) {
    return false;
  }
  if (route_is_active(exact_route)) {
    return true;
  }
  const auto available = std::find_if(
      active_routes_.begin(),
      active_routes_.end(),
      [](const ActiveRoute& route) { return !route.active; });
  if (available == active_routes_.end()) {
    return false;
  }
  available->active = true;
  available->route = exact_route;
  return true;
}

bool SpeakerAssetsRuntimeCore::route_generation_is_closed(
    const SpeakerAssetsRouteToken& exact_route) const {
  constexpr std::uint32_t first_salt = 0xA53C9E17U;
  constexpr std::uint32_t second_salt = 0x6D2B79F5U;
  const auto first = route_filter_bit(exact_route, first_salt);
  const auto second = route_filter_bit(exact_route, second_salt);
  const auto first_mask =
      std::uint64_t{1U} << (first % 64U);
  const auto second_mask =
      std::uint64_t{1U} << (second % 64U);
  return
      (closed_route_filter_[first / 64U] & first_mask) != 0U &&
      (closed_route_filter_[second / 64U] & second_mask) != 0U;
}

void SpeakerAssetsRuntimeCore::remember_closed_route(
    const SpeakerAssetsRouteToken& exact_route) {
  constexpr std::uint32_t first_salt = 0xA53C9E17U;
  constexpr std::uint32_t second_salt = 0x6D2B79F5U;
  const auto first = route_filter_bit(exact_route, first_salt);
  const auto second = route_filter_bit(exact_route, second_salt);
  closed_route_filter_[first / 64U] |=
      std::uint64_t{1U} << (first % 64U);
  closed_route_filter_[second / 64U] |=
      std::uint64_t{1U} << (second % 64U);
}

void SpeakerAssetsRuntimeCore::revoke_all_routes(
    const SpeakerAssetsRouteToken& overflow_route) {
  if (route_shape_is_valid(overflow_route)) {
    remember_closed_route(overflow_route);
  }
  for (std::size_t offset = 0U;
       offset < lifecycle_size_;
       ++offset) {
    const auto index =
        (lifecycle_head_ + offset) % lifecycle_.size();
    remember_closed_route(lifecycle_[index].route);
  }
  for (std::size_t offset = 0U; offset < ingress_size_; ++offset) {
    const auto index =
        (ingress_head_ + offset) % ingress_.size();
    remember_closed_route(ingress_[index].route);
  }
  for (std::size_t offset = 0U; offset < reply_size_; ++offset) {
    const auto index =
        (reply_head_ + offset) % replies_.size();
    remember_closed_route(replies_[index].route);
  }
  for (const auto& active : active_routes_) {
    if (active.active) {
      remember_closed_route(active.route);
    }
  }
  if (session_.route_bound()) {
    remember_closed_route(session_.route());
  }
  if (pending_action_active_ &&
      route_shape_is_valid(pending_action_.route)) {
    remember_closed_route(pending_action_.route);
  }
  if (session_partial_active_ &&
      route_shape_is_valid(session_partial_route_)) {
    remember_closed_route(session_partial_route_);
  }
  session_.all_routes_closed();

  lifecycle_ = {};
  lifecycle_head_ = 0U;
  lifecycle_size_ = 0U;
  ingress_ = {};
  ingress_head_ = 0U;
  ingress_size_ = 0U;
  replies_ = {};
  reply_head_ = 0U;
  reply_size_ = 0U;
  active_routes_ = {};
  session_partial_route_ = {};
  session_partial_last_activity_ms_ = 0U;
  session_partial_active_ = false;
  clear_session_lease();
}

bool SpeakerAssetsRuntimeCore::route_has_ingress(
    const SpeakerAssetsRouteToken& exact_route) const {
  for (std::size_t offset = 0U; offset < ingress_size_; ++offset) {
    const auto index =
        (ingress_head_ + offset) % ingress_.size();
    if (speaker_assets_route_equal(
            ingress_[index].route, exact_route)) {
      return true;
    }
  }
  return false;
}

bool SpeakerAssetsRuntimeCore::route_has_reply(
    const SpeakerAssetsRouteToken& exact_route) const {
  for (std::size_t offset = 0U; offset < reply_size_; ++offset) {
    const auto index =
        (reply_head_ + offset) % replies_.size();
    if (speaker_assets_route_equal(
            replies_[index].route, exact_route)) {
      return true;
    }
  }
  return false;
}

void SpeakerAssetsRuntimeCore::close_route(
    const SpeakerAssetsRouteToken& exact_route) {
  remember_closed_route(exact_route);
  for (auto& active : active_routes_) {
    if (active.active &&
        speaker_assets_route_equal(active.route, exact_route)) {
      active = {};
    }
  }
  session_.route_closed(exact_route);
  if (session_lease_active_ &&
      speaker_assets_route_equal(
          session_lease_route_, exact_route)) {
    clear_session_lease();
  }
  if (session_partial_active_ &&
      speaker_assets_route_equal(
          session_partial_route_, exact_route)) {
    session_partial_active_ = false;
  }
  purge_ingress_for_route(exact_route);
  purge_replies_for_route(exact_route);
}

void SpeakerAssetsRuntimeCore::purge_ingress_for_route(
    const SpeakerAssetsRouteToken& exact_route) {
  const auto original_size = ingress_size_;
  for (std::size_t index = 0U; index < original_size; ++index) {
    const auto record = ingress_[ingress_head_];
    ingress_[ingress_head_] = {};
    ingress_head_ = (ingress_head_ + 1U) % ingress_.size();
    --ingress_size_;
    if (!speaker_assets_route_equal(record.route, exact_route)) {
      const auto tail =
          (ingress_head_ + ingress_size_) % ingress_.size();
      ingress_[tail] = record;
      ++ingress_size_;
    }
  }
}

void SpeakerAssetsRuntimeCore::purge_ingress_for_admission(
    std::uint64_t admission_id) {
  if (admission_id == 0U) {
    return;
  }
  const auto original_size = ingress_size_;
  for (std::size_t index = 0U; index < original_size; ++index) {
    const auto record = ingress_[ingress_head_];
    ingress_[ingress_head_] = {};
    ingress_head_ = (ingress_head_ + 1U) % ingress_.size();
    --ingress_size_;
    if (record.admission_id != admission_id) {
      const auto tail =
          (ingress_head_ + ingress_size_) % ingress_.size();
      ingress_[tail] = record;
      ++ingress_size_;
    }
  }
}

void SpeakerAssetsRuntimeCore::purge_replies_for_route(
    const SpeakerAssetsRouteToken& exact_route) {
  const auto original_size = reply_size_;
  for (std::size_t index = 0U; index < original_size; ++index) {
    const auto reply = replies_[reply_head_];
    replies_[reply_head_] = {};
    reply_head_ = (reply_head_ + 1U) % replies_.size();
    --reply_size_;
    if (!speaker_assets_route_equal(reply.route, exact_route)) {
      const auto tail =
          (reply_head_ + reply_size_) % replies_.size();
      replies_[tail] = reply;
      ++reply_size_;
    }
  }
}

SpeakerAssetsRuntimeStepResult
SpeakerAssetsRuntimeCore::process_lifecycle() {
  const auto record = lifecycle_[lifecycle_head_];
  lifecycle_[lifecycle_head_] = {};
  lifecycle_head_ =
      (lifecycle_head_ + 1U) % lifecycle_.size();
  --lifecycle_size_;
  if (record.kind == LifecycleKind::Opened) {
    return activate_route(record.route)
               ? SpeakerAssetsRuntimeStepResult::LifecycleApplied
               : SpeakerAssetsRuntimeStepResult::
                     RouteCapacityExceeded;
  }
  close_route(record.route);
  return SpeakerAssetsRuntimeStepResult::LifecycleApplied;
}

SpeakerAssetsRuntimeStepResult
SpeakerAssetsRuntimeCore::process_ingress(
    const IngressRecord& record,
    bool resource_steps_allowed,
    std::uint32_t now_ms) {
  if (!route_is_active(record.route)) {
    retire_admission_without_reply(
        record.route, record.admission_id);
    return SpeakerAssetsRuntimeStepResult::StaleRouteDropped;
  }
  return record.kind == IngressKind::UsbFrame
             ? process_usb_frame(
                   record, resource_steps_allowed, now_ms)
             : process_wifi_frame(
                   record, resource_steps_allowed, now_ms);
}

SpeakerAssetsRuntimeStepResult
SpeakerAssetsRuntimeCore::process_usb_frame(
    const IngressRecord& record,
    bool resource_steps_allowed,
    std::uint32_t now_ms) {
  SpeakerAssetsFrame frame{};
  if (decode_speaker_assets_usb_frame(
          record.bytes.data(), record.length, &frame) !=
      SpeakerAssetsProtocolResult::Ok) {
    retire_admission_without_reply(
        record.route, record.admission_id);
    return SpeakerAssetsRuntimeStepResult::InvalidFrameDropped;
  }
  return consume_logical_frame(
      record.route,
      frame,
      now_ms,
      SpeakerAssetsRuntimeStepResult::UsbFrameConsumed,
      resource_steps_allowed,
      record.admission_id);
}

SpeakerAssetsRuntimeStepResult
SpeakerAssetsRuntimeCore::process_wifi_frame(
    const IngressRecord& record,
    bool resource_steps_allowed,
    std::uint32_t now_ms) {
  SpeakerAssetsFrame frame{};
  if (decode_speaker_assets_wifi_frame(
          record.bytes.data(), record.length, &frame) !=
      SpeakerAssetsProtocolResult::Ok) {
    retire_admission_without_reply(
        record.route, record.admission_id);
    return SpeakerAssetsRuntimeStepResult::InvalidFrameDropped;
  }
  return consume_logical_frame(
      record.route,
      frame,
      now_ms,
      SpeakerAssetsRuntimeStepResult::WifiFrameConsumed,
      resource_steps_allowed,
      record.admission_id);
}

SpeakerAssetsRuntimeStepResult
SpeakerAssetsRuntimeCore::consume_logical_frame(
    const SpeakerAssetsRouteToken& exact_route,
    const SpeakerAssetsFrame& frame,
    std::uint32_t activity_ms,
    SpeakerAssetsRuntimeStepResult no_emission_result,
    bool resource_steps_allowed,
    std::uint64_t admission_id) {
  // A decoded logical frame is protocol activity even when foreground
  // voice/edit work pauses it before Session::consume(). Only the exact route
  // currently owning the volatile cookie may renew that binding.
  refresh_session_lease(
      exact_route, frame.session_cookie, activity_ms);
  if (!resource_steps_allowed &&
      is_known_mutating_or_query_opcode(frame.opcode)) {
    // A foreground voice/edit session pauses resource work without cancelling
    // the App's in-flight transfer.  The same authenticated route may keep an
    // already accepted BEGIN/DATA partial alive while it observes
    // PausedForInput.  This changes neither Session bytes nor Flash state; if
    // the App stops retrying, the ordinary no-ingress timeout still reclaims
    // the partial.
    if (session_partial_active_ &&
        speaker_assets_route_equal(
            session_partial_route_, exact_route)) {
      session_partial_last_activity_ms_ = activity_ms;
    }
    SpeakerAssetsFrame paused{};
    paused.opcode = frame.opcode;
    paused.flags =
        kSpeakerAssetsFlagResponse | kSpeakerAssetsFlagError;
    paused.request_id = frame.request_id;
    paused.session_cookie = frame.session_cookie;
    paused.object_offset = frame.object_offset;
    paused.body_length = 1U;
    paused.body[0] = static_cast<std::uint8_t>(
        SpeakerAssetsStatus::PausedForInput);
    if (!push_reply(exact_route, paused, admission_id)) {
      return SpeakerAssetsRuntimeStepResult::ReplyBackpressure;
    }
    return expire_session_partial_after_logical_frame(activity_ms)
               ? SpeakerAssetsRuntimeStepResult::PartialExpired
               : SpeakerAssetsRuntimeStepResult::ReplyQueued;
  }
  SpeakerAssetsEmission emission{};
  const auto partial_activity_before =
      session_.partial_activity_counter();
  if (session_.consume(exact_route, frame, &emission) !=
      SpeakerAssetsSessionResult::Ok) {
    retire_admission_without_reply(
        exact_route, admission_id);
    return SpeakerAssetsRuntimeStepResult::SessionRejected;
  }
  if (!session_.route_bound()) {
    clear_session_lease();
  }
  const auto accepted_partial_activity =
      session_.partial_activity_counter() !=
      partial_activity_before;
  update_session_partial_activity(
      exact_route,
      activity_ms,
      accepted_partial_activity);
  auto partial_expired = false;
  if (!accepted_partial_activity &&
      expire_session_partial_after_logical_frame(activity_ms)) {
    // The first emission describes the old assembler. Never send that stale
    // ACK after clearing the partial: replay the same complete logical frame
    // against the post-expiry Session so its status/bitmap describes the
    // state the next request will actually observe.
    partial_expired = true;
    emission = {};
    const auto replay_activity_before =
        session_.partial_activity_counter();
    if (session_.consume(exact_route, frame, &emission) !=
        SpeakerAssetsSessionResult::Ok) {
      retire_admission_without_reply(
          exact_route, admission_id);
      return SpeakerAssetsRuntimeStepResult::SessionRejected;
    }
    update_session_partial_activity(
        exact_route,
        activity_ms,
        session_.partial_activity_counter() !=
            replay_activity_before);
  }
  switch (emission.kind) {
    case SpeakerAssetsEmissionKind::None:
      retire_admission_without_reply(
          exact_route, admission_id);
      return partial_expired
                 ? SpeakerAssetsRuntimeStepResult::PartialExpired
                 : no_emission_result;
    case SpeakerAssetsEmissionKind::Reply:
      if (!push_reply(
              exact_route, emission.reply, admission_id)) {
        return SpeakerAssetsRuntimeStepResult::ReplyBackpressure;
      }
      return partial_expired
                 ? SpeakerAssetsRuntimeStepResult::PartialExpired
                 : SpeakerAssetsRuntimeStepResult::ReplyQueued;
    case SpeakerAssetsEmissionKind::Action:
      pending_action_ = emission.action;
      pending_action_lease_.route = exact_route;
      pending_action_lease_.admission_id = admission_id;
      pending_action_active_ = true;
      return SpeakerAssetsRuntimeStepResult::ActionQueued;
  }
  return SpeakerAssetsRuntimeStepResult::SessionRejected;
}

SpeakerAssetsRuntimeStepResult
SpeakerAssetsRuntimeCore::process_pending_action(
    std::uint32_t now_ms,
    bool resource_steps_allowed,
    SpeakerAssetsRuntimeActionExecutor* executor) {
  if (!resource_steps_allowed) {
    return SpeakerAssetsRuntimeStepResult::PausedForInput;
  }
  if (reply_size_ >= replies_.size()) {
    return SpeakerAssetsRuntimeStepResult::ReplyBackpressure;
  }
  if (executor == nullptr) {
    return SpeakerAssetsRuntimeStepResult::ExecutorUnavailable;
  }

  SpeakerAssetsActionCompletion completion{};
  const auto execution =
      executor->step(pending_action_, &completion);
  if (execution == SpeakerAssetsActionExecutionResult::Pending) {
    return SpeakerAssetsRuntimeStepResult::ActionExecutionPending;
  }
  const auto executor_rejected =
      execution == SpeakerAssetsActionExecutionResult::Rejected;
  if (executor_rejected) {
    completion = {};
    completion.token = pending_action_.token;
    completion.kind = pending_action_.kind;
    completion.result = SoundStoreResult::Unavailable;
  }

  const auto completed_route = pending_action_.route;
  const auto completed_lease = pending_action_lease_;
  SpeakerAssetsEmission emission{};
  if (session_.complete(completion, &emission) !=
      SpeakerAssetsSessionResult::Ok) {
    return SpeakerAssetsRuntimeStepResult::SessionRejected;
  }
  pending_action_ = {};
  pending_action_lease_ = {};
  pending_action_active_ = false;
  session_partial_active_ = false;
  if (session_.route_bound() &&
      speaker_assets_route_equal(
          session_.route(), completed_route)) {
    // Store work can legitimately span many lease intervals. Start a fresh
    // inactivity window only after its matching completion returns the
    // session to a routable phase.
    refresh_session_lease(
        completed_route, session_.session_cookie(), now_ms);
  } else if (!session_.route_bound()) {
    clear_session_lease();
  }
  if (emission.kind == SpeakerAssetsEmissionKind::Reply &&
      !push_reply(
          completed_route,
          emission.reply,
          completed_lease.admission_id)) {
    return SpeakerAssetsRuntimeStepResult::ReplyBackpressure;
  }
  if (emission.kind == SpeakerAssetsEmissionKind::Action) {
    retire_admission_without_reply(
        completed_lease.route,
        completed_lease.admission_id);
    return SpeakerAssetsRuntimeStepResult::SessionRejected;
  }
  if (emission.kind == SpeakerAssetsEmissionKind::None) {
    retire_admission_without_reply(
        completed_lease.route,
        completed_lease.admission_id);
  }
  return executor_rejected
             ? SpeakerAssetsRuntimeStepResult::ExecutorRejected
             : SpeakerAssetsRuntimeStepResult::ActionCompleted;
}

SpeakerAssetsRuntimeStepResult
SpeakerAssetsRuntimeCore::expire_partials(
    std::uint32_t now_ms) {
  if (session_partial_active_ &&
      !route_has_ingress(session_partial_route_) &&
      elapsed_at_least(
          now_ms,
          session_partial_last_activity_ms_,
          partial_timeout_ms_)) {
    static_cast<void>(
        session_.expire_partial(session_partial_route_));
    session_partial_active_ = false;
    return SpeakerAssetsRuntimeStepResult::PartialExpired;
  }
  return SpeakerAssetsRuntimeStepResult::Idle;
}

SpeakerAssetsRuntimeStepResult
SpeakerAssetsRuntimeCore::expire_session_lease(
    std::uint32_t now_ms) {
  if (!session_lease_active_) {
    return SpeakerAssetsRuntimeStepResult::Idle;
  }
  if (!session_.route_bound() ||
      !speaker_assets_route_equal(
          session_.route(), session_lease_route_)) {
    clear_session_lease();
    return SpeakerAssetsRuntimeStepResult::Idle;
  }
  // A borrowed Store action owns Session memory and may take longer than the
  // lease by design. It is completed or orphaned only through the existing
  // exact action lifecycle, never by an inactivity timer.
  if (pending_action_active_ || session_.action_pending()) {
    return SpeakerAssetsRuntimeStepResult::Idle;
  }
  // Process already-imported traffic from the owner route before judging it
  // inactive.
  if (route_has_ingress(session_lease_route_) ||
      !elapsed_at_least(
          now_ms,
          session_lease_last_activity_ms_,
          session_lease_ms_)) {
    return SpeakerAssetsRuntimeStepResult::Idle;
  }

  const auto expired_route = session_lease_route_;
  session_.route_closed(expired_route);
  if (session_partial_active_ &&
      speaker_assets_route_equal(
          session_partial_route_, expired_route)) {
    session_partial_active_ = false;
  }
  clear_session_lease();
  // Deliberately leave the physical ActiveRoute, replies, ingress admission
  // and Store staging untouched. Their exact leases/ABA guards retire through
  // the existing transport paths; a new logical request can reuse this still
  // connected route after the outstanding admission is released.
  return SpeakerAssetsRuntimeStepResult::SessionLeaseExpired;
}

bool SpeakerAssetsRuntimeCore::
expire_session_partial_after_logical_frame(
    std::uint32_t now_ms) {
  if (!session_partial_active_ ||
      !elapsed_at_least(
          now_ms,
          session_partial_last_activity_ms_,
          partial_timeout_ms_)) {
    return false;
  }
  const auto expired =
      session_.expire_partial(session_partial_route_);
  session_partial_active_ = false;
  return expired;
}

bool SpeakerAssetsRuntimeCore::push_reply(
    const SpeakerAssetsRouteToken& route,
    const SpeakerAssetsFrame& frame,
    std::uint64_t admission_id) {
  if (reply_size_ >= replies_.size()) {
    return false;
  }
  const auto tail =
      (reply_head_ + reply_size_) % replies_.size();
  auto& reply = replies_[tail];
  reply = {};
  reply.sequence = next_reply_sequence_++;
  if (next_reply_sequence_ == 0U) {
    next_reply_sequence_ = 1U;
  }
  reply.route = route;
  reply.lease.route = route;
  reply.lease.admission_id = admission_id;
  reply.frame = frame;
  ++reply_size_;
  return true;
}

void SpeakerAssetsRuntimeCore::retire_admission_without_reply(
    const SpeakerAssetsRouteToken& route,
    std::uint64_t admission_id) {
  if (admission_id == 0U) {
    return;
  }
  purge_admission_tail(admission_id);
  release_now_.route = route;
  release_now_.admission_id = admission_id;
}

void SpeakerAssetsRuntimeCore::purge_admission_tail(
    std::uint64_t admission_id) {
  purge_ingress_for_admission(admission_id);
}

void SpeakerAssetsRuntimeCore::update_session_partial_activity(
    const SpeakerAssetsRouteToken& consumed_route,
    std::uint32_t received_ms,
    bool accepted_partial_activity) {
  const auto phase = session_.phase();
  if (phase == SpeakerAssetsSessionPhase::BeginAssembly) {
    // BEGIN has not created a durable route binding yet. Remember the first
    // route that actually moved the session into assembly, and never refresh
    // that timer with a Busy request from another route.
    if (accepted_partial_activity &&
        (!session_partial_active_ ||
        speaker_assets_route_equal(
            session_partial_route_, consumed_route))) {
      session_partial_route_ = consumed_route;
      session_partial_last_activity_ms_ = received_ms;
      session_partial_active_ = true;
    }
    return;
  }
  if (phase != SpeakerAssetsSessionPhase::UnitAssembly) {
    session_partial_active_ = false;
    return;
  }
  if (!session_.route_bound() ||
      !speaker_assets_route_equal(
          session_.route(), consumed_route) ||
      !accepted_partial_activity) {
    return;
  }
  session_partial_route_ = consumed_route;
  session_partial_last_activity_ms_ = received_ms;
  session_partial_active_ = true;
}

void SpeakerAssetsRuntimeCore::refresh_session_lease(
    const SpeakerAssetsRouteToken& consumed_route,
    std::uint32_t session_cookie,
    std::uint32_t activity_ms) {
  if (session_cookie == 0U ||
      !session_.route_bound() ||
      session_cookie != session_.session_cookie() ||
      !speaker_assets_route_equal(
          session_.route(), consumed_route)) {
    return;
  }
  session_lease_route_ = consumed_route;
  session_lease_last_activity_ms_ = activity_ms;
  session_lease_active_ = true;
}

void SpeakerAssetsRuntimeCore::clear_session_lease() {
  session_lease_route_ = {};
  session_lease_last_activity_ms_ = 0U;
  session_lease_active_ = false;
}

}  // namespace easy_input::speaker_assets
